/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * Work with WAV in the proprietary Microsoft format.
 * 
 * Copyright (C) 1999, Mark Spencer
 *
 * Mark Spencer <markster@linux-support.net>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */
 
#include <asterisk/lock.h>
#include <asterisk/channel.h>
#include <asterisk/file.h>
#include <asterisk/logger.h>
#include <asterisk/sched.h>
#include <asterisk/module.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <sys/time.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#ifdef __linux__
#include <endian.h>
#else
#include <machine/endian.h>
#endif

/* Some Ideas for this code came from makewave.c by Jeffery Chilton */

/* Portions of the conversion code are by guido@sienanet.it */

struct ast_filestream {
	void *reserved[AST_RESERVED_POINTERS];
	/* This is what a filestream means to us */
	int fd; /* Descriptor */
	int bytes;
	int needsgain;
	struct ast_channel *owner;
	struct ast_frame fr;				/* Frame information */
	char waste[AST_FRIENDLY_OFFSET];	/* Buffer for sending frames, etc */
	char empty;							/* Empty character */
	short buf[160];	
	int foffset;
	int lasttimeout;
	struct timeval last;
	int adj;
	struct ast_filestream *next;
};


static struct ast_filestream *glist = NULL;
static pthread_mutex_t wav_lock = AST_MUTEX_INITIALIZER;
static int glistcnt = 0;

static char *name = "wav";
static char *desc = "Microsoft WAV format (8000hz Signed Linear)";
static char *exts = "wav";

#define BLOCKSIZE 160

#define GAIN 2		/* 2^GAIN is the multiple to increase the volume by */

#if __BYTE_ORDER == __LITTLE_ENDIAN
#define htoll(b) (b)
#define htols(b) (b)
#define ltohl(b) (b)
#define ltohs(b) (b)
#else
#if __BYTE_ORDER == __BIG_ENDIAN
#define htoll(b)  \
          (((((b)      ) & 0xFF) << 24) | \
	       ((((b) >>  8) & 0xFF) << 16) | \
		   ((((b) >> 16) & 0xFF) <<  8) | \
		   ((((b) >> 24) & 0xFF)      ))
#define htols(b) \
          (((((b)      ) & 0xFF) << 8) | \
		   ((((b) >> 8) & 0xFF)      ))
#define ltohl(b) htoll(b)
#define ltohs(b) htols(b)
#else
#error "Endianess not defined"
#endif
#endif


static int check_header(int fd)
{
	int type, size, formtype;
	int fmt, hsize;
	short format, chans, bysam, bisam;
	int bysec;
	int freq;
	int data;
	if (read(fd, &type, 4) != 4) {
		ast_log(LOG_WARNING, "Read failed (type)\n");
		return -1;
	}
	if (read(fd, &size, 4) != 4) {
		ast_log(LOG_WARNING, "Read failed (size)\n");
		return -1;
	}
	size = ltohl(size);
	if (read(fd, &formtype, 4) != 4) {
		ast_log(LOG_WARNING, "Read failed (formtype)\n");
		return -1;
	}
	if (memcmp(&type, "RIFF", 4)) {
		ast_log(LOG_WARNING, "Does not begin with RIFF\n");
		return -1;
	}
	if (memcmp(&formtype, "WAVE", 4)) {
		ast_log(LOG_WARNING, "Does not contain WAVE\n");
		return -1;
	}
	if (read(fd, &fmt, 4) != 4) {
		ast_log(LOG_WARNING, "Read failed (fmt)\n");
		return -1;
	}
	if (memcmp(&fmt, "fmt ", 4)) {
		ast_log(LOG_WARNING, "Does not say fmt\n");
		return -1;
	}
	if (read(fd, &hsize, 4) != 4) {
		ast_log(LOG_WARNING, "Read failed (formtype)\n");
		return -1;
	}
	if (ltohl(hsize) != 16) {
		ast_log(LOG_WARNING, "Unexpected header size %d\n", ltohl(hsize));
		return -1;
	}
	if (read(fd, &format, 2) != 2) {
		ast_log(LOG_WARNING, "Read failed (format)\n");
		return -1;
	}
	if (ltohs(format) != 1) {
		ast_log(LOG_WARNING, "Not a wav file %d\n", ltohs(format));
		return -1;
	}
	if (read(fd, &chans, 2) != 2) {
		ast_log(LOG_WARNING, "Read failed (format)\n");
		return -1;
	}
	if (ltohs(chans) != 1) {
		ast_log(LOG_WARNING, "Not in mono %d\n", ltohs(chans));
		return -1;
	}
	if (read(fd, &freq, 4) != 4) {
		ast_log(LOG_WARNING, "Read failed (freq)\n");
		return -1;
	}
	if (ltohl(freq) != 8000) {
		ast_log(LOG_WARNING, "Unexpected freqency %d\n", ltohl(freq));
		return -1;
	}
	/* Ignore the byte frequency */
	if (read(fd, &bysec, 4) != 4) {
		ast_log(LOG_WARNING, "Read failed (BYTES_PER_SECOND)\n");
		return -1;
	}
	/* Check bytes per sample */
	if (read(fd, &bysam, 2) != 2) {
		ast_log(LOG_WARNING, "Read failed (BYTES_PER_SAMPLE)\n");
		return -1;
	}
	if (ltohs(bysam) != 2) {
		ast_log(LOG_WARNING, "Can only handle 16bits per sample: %d\n", ltohs(bysam));
		return -1;
	}
	if (read(fd, &bisam, 2) != 2) {
		ast_log(LOG_WARNING, "Read failed (Bits Per Sample): %d\n", ltohs(bisam));
		return -1;
	}
        /* Begin data chunk */
	if (read(fd, &data, 4) != 4) {
		ast_log(LOG_WARNING, "Read failed (data)\n");
		return -1;
	}
	if (memcmp(&data, "data", 4)) {
		ast_log(LOG_WARNING, "Does not say data\n");
		return -1;
	}
	/* Ignore the data length */
	if (read(fd, &data, 4) != 4) {
		ast_log(LOG_WARNING, "Read failed (data)\n");
		return -1;
	}
	return 0;
}

static int update_header(int fd)
{
	off_t cur,end;
	int datalen,filelen,bytes;
	
	
	cur = lseek(fd, 0, SEEK_CUR);
	end = lseek(fd, 0, SEEK_END);
	/* data starts 44 bytes in */
	bytes = end - 44;
	datalen = htoll(bytes);
	/* chunk size is bytes of data plus 36 bytes of header */
	filelen = htoll(36 + bytes);
	
	if (cur < 0) {
		ast_log(LOG_WARNING, "Unable to find our position\n");
		return -1;
	}
	if (lseek(fd, 4, SEEK_SET) != 4) {
		ast_log(LOG_WARNING, "Unable to set our position\n");
		return -1;
	}
	if (write(fd, &filelen, 4) != 4) {
		ast_log(LOG_WARNING, "Unable to set write file size\n");
		return -1;
	}
	if (lseek(fd, 40, SEEK_SET) != 40) {
		ast_log(LOG_WARNING, "Unable to set our position\n");
		return -1;
	}
	if (write(fd, &datalen, 4) != 4) {
		ast_log(LOG_WARNING, "Unable to set write datalen\n");
		return -1;
	}
	if (lseek(fd, cur, SEEK_SET) != cur) {
		ast_log(LOG_WARNING, "Unable to return to position\n");
		return -1;
	}
	return 0;
}

static int write_header(int fd)
{
	unsigned int hz=htoll(8000);
	unsigned int bhz = htoll(16000);
	unsigned int hs = htoll(16);
	unsigned short fmt = htols(1);
	unsigned short chans = htols(1);
	unsigned short bysam = htols(2);
	unsigned short bisam = htols(16);
	unsigned int size = htoll(0);
	/* Write a wav header, ignoring sizes which will be filled in later */
	lseek(fd,0,SEEK_SET);
	if (write(fd, "RIFF", 4) != 4) {
		ast_log(LOG_WARNING, "Unable to write header\n");
		return -1;
	}
	if (write(fd, &size, 4) != 4) {
		ast_log(LOG_WARNING, "Unable to write header\n");
		return -1;
	}
	if (write(fd, "WAVEfmt ", 8) != 8) {
		ast_log(LOG_WARNING, "Unable to write header\n");
		return -1;
	}
	if (write(fd, &hs, 4) != 4) {
		ast_log(LOG_WARNING, "Unable to write header\n");
		return -1;
	}
	if (write(fd, &fmt, 2) != 2) {
		ast_log(LOG_WARNING, "Unable to write header\n");
		return -1;
	}
	if (write(fd, &chans, 2) != 2) {
		ast_log(LOG_WARNING, "Unable to write header\n");
		return -1;
	}
	if (write(fd, &hz, 4) != 4) {
		ast_log(LOG_WARNING, "Unable to write header\n");
		return -1;
	}
	if (write(fd, &bhz, 4) != 4) {
		ast_log(LOG_WARNING, "Unable to write header\n");
		return -1;
	}
	if (write(fd, &bysam, 2) != 2) {
		ast_log(LOG_WARNING, "Unable to write header\n");
		return -1;
	}
	if (write(fd, &bisam, 2) != 2) {
		ast_log(LOG_WARNING, "Unable to write header\n");
		return -1;
	}
	if (write(fd, "data", 4) != 4) {
		ast_log(LOG_WARNING, "Unable to write header\n");
		return -1;
	}
	if (write(fd, &size, 4) != 4) {
		ast_log(LOG_WARNING, "Unable to write header\n");
		return -1;
	}
	return 0;
}

static struct ast_filestream *wav_open(int fd)
{
	/* We don't have any header to read or anything really, but
	   if we did, it would go here.  We also might want to check
	   and be sure it's a valid file.  */
	struct ast_filestream *tmp;
	if ((tmp = malloc(sizeof(struct ast_filestream)))) {
		memset(tmp, 0, sizeof(struct ast_filestream));
		if (check_header(fd)) {
			free(tmp);
			return NULL;
		}
		if (ast_pthread_mutex_lock(&wav_lock)) {
			ast_log(LOG_WARNING, "Unable to lock wav list\n");
			free(tmp);
			return NULL;
		}
		tmp->next = glist;
		glist = tmp;
		tmp->fd = fd;
		tmp->owner = NULL;
		tmp->needsgain = 1;
		tmp->fr.data = tmp->buf;
		tmp->fr.frametype = AST_FRAME_VOICE;
		tmp->fr.subclass = AST_FORMAT_SLINEAR;
		/* datalen will vary for each frame */
		tmp->fr.src = name;
		tmp->fr.mallocd = 0;
		tmp->lasttimeout = -1;
		tmp->bytes = 0;
		glistcnt++;
		ast_pthread_mutex_unlock(&wav_lock);
		ast_update_use_count();
	}
	return tmp;
}

static struct ast_filestream *wav_rewrite(int fd, char *comment)
{
	/* We don't have any header to read or anything really, but
	   if we did, it would go here.  We also might want to check
	   and be sure it's a valid file.  */
	struct ast_filestream *tmp;
	if ((tmp = malloc(sizeof(struct ast_filestream)))) {
		memset(tmp, 0, sizeof(struct ast_filestream));
		if (write_header(fd)) {
			free(tmp);
			return NULL;
		}
		if (ast_pthread_mutex_lock(&wav_lock)) {
			ast_log(LOG_WARNING, "Unable to lock wav list\n");
			free(tmp);
			return NULL;
		}
		tmp->next = glist;
		glist = tmp;
		tmp->fd = fd;
		tmp->owner = NULL;
		tmp->lasttimeout = -1;
		glistcnt++;
		ast_pthread_mutex_unlock(&wav_lock);
		ast_update_use_count();
	} else
		ast_log(LOG_WARNING, "Out of memory\n");
	return tmp;
}

static struct ast_frame *wav_read(struct ast_filestream *s)
{
	return NULL;
}

static void wav_close(struct ast_filestream *s)
{
	struct ast_filestream *tmp, *tmpl = NULL;
	char zero = 0;
	if (ast_pthread_mutex_lock(&wav_lock)) {
		ast_log(LOG_WARNING, "Unable to lock wav list\n");
		return;
	}
	tmp = glist;
	while(tmp) {
		if (tmp == s) {
			if (tmpl)
				tmpl->next = tmp->next;
			else
				glist = tmp->next;
			break;
		}
		tmpl = tmp;
		tmp = tmp->next;
	}
	glistcnt--;
	if (s->owner) {
		s->owner->stream = NULL;
		if (s->owner->streamid > -1)
			ast_sched_del(s->owner->sched, s->owner->streamid);
		s->owner->streamid = -1;
	}
	ast_pthread_mutex_unlock(&wav_lock);
	ast_update_use_count();
	if (!tmp) 
		ast_log(LOG_WARNING, "Freeing a filestream we don't seem to own\n");
	/* Pad to even length */
	if (s->bytes & 0x1)
		write(s->fd, &zero, 1);
	close(s->fd);
	free(s);
	s = NULL;
#if 0
	printf("bytes = %d\n", s->bytes);
#endif
}

static int ast_read_callback(void *data)
{
	int retval = 0;
	int res;
	int delay;
	int x;
	struct ast_filestream *s = data;
	short tmp[sizeof(s->buf) / 2];
	/* Send a frame from the file to the appropriate channel */
	
	if ( (res = read(s->fd, tmp, sizeof(tmp))) <= 0 ) {
		if (res) {
			ast_log(LOG_WARNING, "Short read (%d) (%s)!\n", res, strerror(errno));
		}
		s->owner->streamid = -1;
		return 0;
	}

#if __BYTE_ORDER == __BIG_ENDIAN
	for( x = 0; x < sizeof(tmp)/2; x++) tmp[x] = (tmp[x] << 8) | ((tmp[x] & 0xff00) >> 8);
#endif

	if (s->needsgain) {
		for (x=0;x<sizeof(tmp)/2;x++)
			if (tmp[x] & ((1 << GAIN) - 1)) {
				/* If it has data down low, then it's not something we've artificially increased gain
				   on, so we don't need to gain adjust it */
				s->needsgain = 0;
			}
	}
	if (s->needsgain) {
		for (x=0;x<sizeof(tmp)/2;x++) {
			s->buf[x] = tmp[x] >> GAIN;
		}
	} else {
		memcpy(s->buf, tmp, sizeof(s->buf));
	}
			
	delay = res / 2;
	s->fr.frametype = AST_FRAME_VOICE;
	s->fr.subclass = AST_FORMAT_SLINEAR;
	s->fr.offset = AST_FRIENDLY_OFFSET;
	s->fr.datalen = res;
	s->fr.data = s->buf;
	s->fr.mallocd = 0;
	s->fr.samples = delay;
	delay /= 8;
	/* Lastly, process the frame */	
	if (delay != s->lasttimeout) {
		s->owner->streamid = ast_sched_add(s->owner->sched, delay, ast_read_callback, s);
		s->lasttimeout = delay;
	} else {
		retval = -1;
	}
	
	
	if (ast_write(s->owner, &s->fr)) {
		ast_log(LOG_WARNING, "Failed to write frame\n");
		s->owner->streamid = -1;
		return 0;
	}
	
	return retval;
}

static int wav_apply(struct ast_channel *c, struct ast_filestream *s)
{
	/* Select our owner for this stream, and get the ball rolling. */
	s->owner = c;
	return 0;
}

static int wav_play(struct ast_filestream *s)
{
	ast_read_callback(s);
	return 0;
}

static int wav_write(struct ast_filestream *fs, struct ast_frame *f)
{
	int res = 0;
	int x;
	short tmp[8000], *tmpi;
	float tmpf;
	if (f->frametype != AST_FRAME_VOICE) {
		ast_log(LOG_WARNING, "Asked to write non-voice frame!\n");
		return -1;
	}
	if (f->subclass != AST_FORMAT_SLINEAR) {
		ast_log(LOG_WARNING, "Asked to write non-SLINEAR frame (%d)!\n", f->subclass);
		return -1;
	}
	if (f->datalen > sizeof(tmp)) {
		ast_log(LOG_WARNING, "Data length is too long\n");
		return -1;
	}
	if (!f->datalen)
		return -1;

#if 0
	printf("Data Length: %d\n", f->datalen);
#endif	

	if (fs->buf) {
		tmpi = f->data;
		/* Volume adjust here to accomodate */
		for (x=0;x<f->datalen/2;x++) {
			tmpf = ((float)tmpi[x]) * ((float)(1 << GAIN));
			if (tmpf > 32767.0)
				tmpf = 32767.0;
			if (tmpf < -32768.0)
				tmpf = -32768.0;
			tmp[x] = tmpf;
			tmp[x] &= ~((1 << GAIN) - 1);

#if __BYTE_ORDER == __BIG_ENDIAN
			tmp[x] = (tmp[x] << 8) | ((tmp[x] & 0xff00) >> 8);
#endif

		}
		if ((write (fs->fd, tmp, f->datalen) != f->datalen) ) {
			ast_log(LOG_WARNING, "Bad write (%d): %s\n", res, strerror(errno));
			return -1;
		}
	} else {
		ast_log(LOG_WARNING, "Cannot write data to file.\n");
		return -1;
	}
	
	fs->bytes += f->datalen;
	update_header(fs->fd);
		
	return 0;

}

static int wav_seek(struct ast_filestream *fs, long sample_offset, int whence)
{
	off_t min,max,cur;
	long offset,samples;
	
	samples = sample_offset * 2; /* SLINEAR is 16 bits mono, so sample_offset * 2 = bytes */
	min = 44; /* wav header is 44 bytes */
	cur = lseek(fs->fd, 0, SEEK_CUR);
	max = lseek(fs->fd, 0, SEEK_END);
	if(whence == SEEK_SET)
		offset = samples + min;
	if(whence == SEEK_CUR)
		offset = samples + cur;
	if(whence == SEEK_END)
		offset = max - samples; 
	offset = (offset > max)?max:offset;
	offset = (offset < min)?min:offset;
	return lseek(fs->fd,offset,SEEK_SET);
}

static int wav_trunc(struct ast_filestream *fs)
{
	if(ftruncate(fs->fd, lseek(fs->fd,0,SEEK_CUR)))
		return -1;
	return update_header(fs->fd);
}

static long wav_tell(struct ast_filestream *fs)
{
	off_t offset;
	offset = lseek(fs->fd, 0, SEEK_CUR);
	/* subtract header size to get samples, then divide by 2 for 16 bit samples */
	return (offset - 44)/2;
}

static char *wav_getcomment(struct ast_filestream *s)
{
	return NULL;
}

int load_module()
{
	return ast_format_register(name, exts, AST_FORMAT_SLINEAR,
								wav_open,
								wav_rewrite,
								wav_apply,
								wav_play,
								wav_write,
								wav_seek,
								wav_trunc,
								wav_tell,
								wav_read,
								wav_close,
								wav_getcomment);
								
								
}

int unload_module()
{
	struct ast_filestream *tmp, *tmpl;
	if (ast_pthread_mutex_lock(&wav_lock)) {
		ast_log(LOG_WARNING, "Unable to lock wav list\n");
		return -1;
	}
	tmp = glist;
	while(tmp) {
		if (tmp->owner)
			ast_softhangup(tmp->owner, AST_SOFTHANGUP_APPUNLOAD);
		tmpl = tmp;
		tmp = tmp->next;
		free(tmpl);
	}
	ast_pthread_mutex_unlock(&wav_lock);
	return ast_format_unregister(name);
}	

int usecount()
{
	int res;
	if (ast_pthread_mutex_lock(&wav_lock)) {
		ast_log(LOG_WARNING, "Unable to lock wav list\n");
		return -1;
	}
	res = glistcnt;
	ast_pthread_mutex_unlock(&wav_lock);
	return res;
}

char *description()
{
	return desc;
}


char *key()
{
	return ASTERISK_GPL_KEY;
}
