/*
mediastreamer2 library - modular sound and video processing and streaming
Copyright (C) 2006  Simon MORLAT (simon.morlat@linphone.org)

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <linux/videodev.h>

#include "mediastreamer2/msvideo.h"
#include "mediastreamer2/msticker.h"
#include "mediastreamer2/msv4l.h"

#include "nowebcam.h"

typedef struct V4lState{
	int fd;
	ms_thread_t thread;
	char *dev;
	char *mmapdbuf;
	int msize;/*mmapped size*/
	MSVideoSize vsize;
	int pix_fmt;
	mblk_t *frames[VIDEO_MAX_FRAME];
	mblk_t *mire;
	queue_t rq;
	ms_mutex_t mutex;
	int frame_ind;
	int frame_max;
	float fps;
	float start_time;
	int frame_count;
	bool_t run;
	bool_t usemire;
}V4lState;

static void *v4l_thread(void *s);
static int v4l_configure(V4lState *s);

static void v4l_init(MSFilter *f){
	V4lState *s=ms_new0(V4lState,1);
	s->fd=-1;
	s->run=FALSE;
	s->mmapdbuf=NULL;
	s->vsize.width=MS_VIDEO_SIZE_CIF_W;
	s->vsize.height=MS_VIDEO_SIZE_CIF_H;
	s->pix_fmt=MS_RGB24;
	s->dev=ms_strdup("/dev/video0");
	qinit(&s->rq);
	s->mire=NULL;
	ms_mutex_init(&s->mutex,NULL);
	s->start_time=0;
	s->frame_count=-1;
	s->fps=15;
	s->usemire=(getenv("DEBUG")!=NULL);
	f->data=s;
}

/* we try not to close the /dev/videoX device to workaround a bug of linux kernel.
The bug is this one:
One thread opens /dev/videoX, and mmap()s some pages to get data from the camera.
Then several threads are created (using clone) and automatically get a reference to the mmap'd page, thus a reference to the file_struct of the opened file descriptor.
Then when the first thread closes and unmap() the file descriptor, the .release method of the driver is not called because there are still some threads owning the reference to the mmap'd pages.
As far as I understand.
If all threads are correctly pthread_join()ed, then the file descriptor is closed correctly.
But unfortunately when using alsa dmix/asym plugins, some threads are started by those plugins and are kept alive (or zombie) for some time after the mmap()/close().
*/

static int v4l_fd=-1;
static char *v4l_devname=NULL;

static int v4l_start(MSFilter *f, void *arg)
{
	V4lState *s=(V4lState*)f->data;
	int err=0;
	if (v4l_fd>=0 ){
		if (strcmp(v4l_devname,s->dev)==0){
			/*use this one!*/
			ms_message("v4l_start: reusing previous file descriptor.");
			s->fd=v4l_fd;
		}else{
			close(v4l_fd);
			v4l_fd=-1;
			ms_free(v4l_devname);
			v4l_devname=NULL;
		}
	}
	if (s->fd==-1){
		s->fd=open(s->dev,O_RDONLY);
		ms_message("v4l_start: open, fd=%i",s->fd);
		if (s->fd>=0){
			v4l_fd=s->fd;
			v4l_devname=ms_strdup(s->dev);
		}
	}
	if (s->fd<0){
		ms_error("MSV4l: cannot open video device (%s): %s.",s->dev,strerror(errno));
		return -1;
	}else{
		err=v4l_configure(s);
		if (err<0) 
		{
			ms_error("MSV4l: could not get configuration of video device");
			close(s->fd);
			s->fd=-1;
			return -1;
		}
	}
	return 0;
}

static void v4l_start_capture(V4lState *s){
	if (s->fd>=0){
		s->run=TRUE;
		ms_thread_create(&s->thread,NULL,v4l_thread,s);
	}
}

static int v4l_stop(MSFilter *f, void *arg){
	V4lState *s=(V4lState*)f->data;
	if (s->fd>=0){
		/* do not close*/
		/*
		ms_message("v4l fd %i closed",s->fd);
		if (close(s->fd)<0){
			ms_warning("MSV4l: Could not close(): %s",strerror(errno));
		}
		*/
		s->fd=-1;
		s->frame_count=-1;
	}
	return 0;
}

static void v4l_stop_capture(V4lState *s){
	if (s->run){
		s->run=FALSE;
		ms_thread_join(s->thread,NULL);
		ms_message("v4l thread has joined.");
	}
}


static void v4l_uninit(MSFilter *f){
	V4lState *s=(V4lState*)f->data;
	if (s->fd>=0) v4l_stop(f,NULL);
	ms_free(s->dev);
	flushq(&s->rq,0);
	ms_mutex_destroy(&s->mutex);
	freemsg(s->mire);
	ms_free(s);
}

static bool_t try_format(int fd, struct video_picture *pict, int palette, int depth){
	int err;
	pict->palette=palette;
	pict->depth=depth;
	err=ioctl(fd,VIDIOCSPICT,pict);
	if (err<0){
		ms_warning("Could not set picture properties: %s",strerror(errno));
		return FALSE;
	}
	return TRUE;
}

static int v4l_do_mmap(V4lState *s){
	struct video_mbuf vmbuf;
	int err,i;
	memset(&vmbuf,0,sizeof(vmbuf));
	/* try to get mmap properties */
	err=ioctl(s->fd,VIDIOCGMBUF,&vmbuf);
	if (err<0){
		ms_error("Could not get mmap properties: %s",strerror(errno));
		return -1;
	}else {
		if (vmbuf.size>0){
			/* do the mmap */
			s->msize=vmbuf.size;
			s->frame_max=vmbuf.frames;
		} else {
			ms_error("This device cannot support mmap.");
			return -1;
		}
	}
	s->mmapdbuf=mmap(NULL,s->msize,PROT_READ,MAP_SHARED,s->fd,0);
	if (s->mmapdbuf==(void*)-1) {
		ms_error("Could not mmap: %s",strerror(errno));
		s->mmapdbuf=NULL;
		return -1;
	}else {
		/* initialize the mediastreamer buffers */
		ms_message("Using %i-frames mmap'd buffer at %p, len %i",
			s->frame_max, s->mmapdbuf,s->msize);
		for(i=0;i<s->frame_max;i++){
			mblk_t *buf=esballoc((uint8_t*)s->mmapdbuf+vmbuf.offsets[i],vmbuf.offsets[1],0,NULL);
			/* adjust to real size of picture*/
			if (s->pix_fmt==MS_RGB24)
				buf->b_wptr+=s->vsize.width*s->vsize.height*3;
			else 
				buf->b_wptr+=(s->vsize.width*s->vsize.height*3)/2;
			s->frames[i]=buf;
		}
		s->frame_ind=0;
	}
	return 0;
}

static int v4l_configure(V4lState *s)
{
	struct video_channel chan;
	struct video_picture pict;
	struct video_capability cap;
	struct video_window win;
	int err;
	int i;
	int found=0;
	
	memset(&chan,0,sizeof(chan));
	memset(&pict,0,sizeof(pict));
	memset(&cap,0,sizeof(cap));
	memset(&win,0,sizeof(win));

	err=ioctl(s->fd,VIDIOCGCAP,&cap);
	if (err!=0)
	{
		ms_warning("MSV4l: cannot get device capabilities: %s.",strerror(errno));
		return -1;
	}
	ms_message("Found %s device.",cap.name);
	for (i=0;i<cap.channels;i++)
	{
		chan.channel=i;
		err=ioctl(s->fd,VIDIOCGCHAN,&chan);
		if (err==0)
		{
			ms_message("Getting video channel %s",chan.name);
			switch(chan.type){
				case VIDEO_TYPE_TV:
					ms_message("Channel is a TV.");
				break;
				case VIDEO_TYPE_CAMERA:
					ms_message("Channel is a camera");
				break;
				default:
					ms_warning("unknown video channel type.");
			}
			found=1;
			break;  /* find the first channel */
		}
	}
	if (found) ms_message("A valid video channel was found.");
	/* select this channel */
	ioctl(s->fd,VIDIOCSCHAN,&chan);
	
	/* get picture properties */
	err=ioctl(s->fd,VIDIOCGPICT,&pict);
	if (err<0){
		ms_warning("Could not get picture properties: %s",strerror(errno));
		return -1;
	}
	ms_message("Default picture properties: brightness=%i,hue=%i,colour=%i,contrast=%i,depth=%i, palette=%i.",
						pict.brightness,pict.hue,pict.colour, pict.contrast,pict.depth, pict.palette);
	/* trying YUV420P format:*/
	
	if (try_format(s->fd,&pict,VIDEO_PALETTE_YUV420P,16)){
		ms_message("Driver supports YUV420P, using that format.");
		s->pix_fmt=MS_YUV420P;
	}else{
		ms_message("Driver does not support YUV420P, trying RGB24...");
		if (try_format(s->fd, &pict,VIDEO_PALETTE_RGB24,24)){
			ms_message("Driver supports RGB24, using that format.");
			s->pix_fmt=MS_RGB24;
		}else{
			ms_fatal("Unsupported video pixel format.");
		}
	}
	/*set picture size */
	win.x=win.y=0;
	win.width=s->vsize.width;
	win.height=s->vsize.height;
	win.flags=0;
	win.clips=NULL;
	win.clipcount=0;

	err=ioctl(s->fd,VIDIOCSWIN,&win);
	if (err<0){
		ms_error("Could not set window size: %s",strerror(errno));
	}
	return 0;
}	

#define BPP 3
static inline
void crop( unsigned char *src, int s_width, int s_height, unsigned char *dest, int d_width, int d_height)
{
	register int i;
	register int stride = d_width*BPP;
	register unsigned char *s = src, *d = dest;
	s += ((s_height - d_height)/2 * s_width * BPP) + ((s_width - d_width)/2 * BPP);
	for (i = 0; i < d_height; i++, d += stride, s += s_width * BPP)
		memcpy( d, s, stride);
}

int ms_to_v4l_pix_fmt(MSPixFmt p){
	switch(p){
		case MS_YUV420P:
				return VIDEO_PALETTE_YUV420P;
		case MS_RGB24:
				return VIDEO_PALETTE_RGB24;
		default:
			ms_fatal("unsupported pix fmt");
			return -1;
	}
}

static void v4l_purge(V4lState *s){
	int i;
	int err;
	int jitter=s->frame_max-1;
	for (i=s->frame_ind-jitter;i<s->frame_ind;++i){
		int syncframe=i%s->frame_max;
		ms_message("syncing last frame");
		err=ioctl(s->fd,VIDIOCSYNC,&syncframe);
		if (err<0) {
			ms_warning("v4l_mmap_process: error in VIDIOCSYNC: %s.",strerror(errno));	
		}
	}
}

static mblk_t * v4l_grab_image_mmap(V4lState *s){
	struct video_mmap vmap;
	int err;
	int syncframe;
	int jitter=s->frame_max-1;
	int query_frame;
	vmap.width=s->vsize.width;
	vmap.height=s->vsize.height;
	vmap.format=ms_to_v4l_pix_fmt(s->pix_fmt);
	
	query_frame=(s->frame_ind) % s->frame_max;
	/*ms_message("v4l_mmap_process: query_frame=%i",
			obj->query_frame);*/
	vmap.frame=query_frame;
	err=ioctl(s->fd,VIDIOCMCAPTURE,&vmap);
	if (err<0) {
		ms_warning("v4l_mmap_process: error in VIDIOCMCAPTURE: %s.",strerror(errno));
		return NULL;
	}
	/*g_message("v4l_mmap_process: query_frame=%i done",
			obj->query_frame);*/
	syncframe=(s->frame_ind-jitter);
	s->frame_ind++;
	if (syncframe>=0){
		syncframe=syncframe%s->frame_max;
		/*ms_message("Syncing on frame %i",syncframe);*/
		err=ioctl(s->fd,VIDIOCSYNC,&syncframe);
		if (err<0) {
			ms_warning("v4l_mmap_process: error in VIDIOCSYNC: %s.",strerror(errno));	
			return NULL;
		}
		/*g_message("got frame %i",syncframe);*/
	}else {
		return NULL;
	}
	/* not particularly efficient - hope for a capture source that 
	   provides subcapture or setting window */
	/*
	if (obj->width != MS_VIDEO_SOURCE(obj)->width || obj->height != MS_VIDEO_SOURCE(obj)->height){
		guchar tmp[obj->bsize];
		crop((guchar*) obj->img[syncframe].buffer, obj->width, obj->height, tmp,
			MS_VIDEO_SOURCE(obj)->width, MS_VIDEO_SOURCE(obj)->height);
		memcpy(obj->img[syncframe].buffer, tmp, MS_VIDEO_SOURCE(obj)->width *
			MS_VIDEO_SOURCE(obj)->height * obj->pict.depth/8);
	}
	*/
	return s->frames[syncframe];
}

static mblk_t * v4l_make_mire(V4lState *s){
	unsigned char *data;
	int i,j,line,pos;
	int patternw=s->vsize.width/6; 
	int patternh=s->vsize.height/6;
	int red,green=0,blue=0;
	if (s->mire==NULL){
		s->mire=allocb(s->vsize.width*s->vsize.height*3,0);
		s->mire->b_wptr=s->mire->b_datap->db_lim;
	}
	data=s->mire->b_rptr;
	for (i=0;i<s->vsize.height;++i){
		line=i*s->vsize.width*3;
		if ( ((i+s->frame_ind)/patternh) & 0x1) red=255;
		else red= 0;
		for (j=0;j<s->vsize.width;++j){
			pos=line+(j*3);
			
			if ( ((j+s->frame_ind)/patternw) & 0x1) blue=255;
			else blue= 0;
			
			data[pos]=red;
			data[pos+1]=green;
			data[pos+2]=blue;
		}
	}
	s->frame_ind++;
	return s->mire;
}

static mblk_t * v4l_make_nowebcam(V4lState *s){
	unsigned char *data;
	unsigned char *p;
	int i,j,pos,linepos;
	int starti,startj;
	if (s->mire==NULL){
		s->mire=allocb(s->vsize.width*s->vsize.height*3,0);
		s->mire->b_wptr=s->mire->b_datap->db_lim;
		memset(s->mire->b_rptr,0,s->mire->b_wptr-s->mire->b_rptr);
		data=s->mire->b_rptr;
		p=data;
		pos=0;
		starti=(s->vsize.width/2)-(gimp_image.width/2);
		startj=(s->vsize.height/2)-(gimp_image.height/2);
		linepos=startj*s->vsize.width*3;
		for (j=startj;j<startj+gimp_image.height;++j){
			p=&data[linepos]+(starti*3);
			for(i=starti;i<starti+gimp_image.width;++i){
				p[0]=gimp_image.pixel_data[pos];
				p[1]=gimp_image.pixel_data[pos+1];
				p[2]=gimp_image.pixel_data[pos+2];
				p+=3;
				pos+=3;
			}
			linepos+=s->vsize.width*3;
		}
	}
	s->frame_ind++;
	return s->mire;
}

static void v4l_do_munmap(V4lState *s){
	int i;
	if (s->mmapdbuf!=NULL){
		if (munmap(s->mmapdbuf,s->msize)<0){
			ms_warning("MSV4l: Fail to unmap: %s",strerror(errno));
		}
		ms_message("munmap() done (%p,%i)",s->mmapdbuf,s->msize);
		s->mmapdbuf=NULL;
		s->msize=0;
		for(i=0;i<s->frame_max;++i){
			freemsg(s->frames[i]);
			s->frames[i]=NULL;
		}
	}
}

static void *v4l_thread(void *ptr){
	V4lState *s=(V4lState*)ptr;
	ms_message("v4l_thread starting");
	if (v4l_do_mmap(s)<0){
		ms_thread_exit(NULL);
	}
	while(s->run){
		mblk_t *m;
		m=v4l_grab_image_mmap(s);
		if (m) {
			ms_mutex_lock(&s->mutex);
			putq(&s->rq,dupmsg(m));
			ms_mutex_unlock(&s->mutex);
		}
	}
	v4l_purge(s);
	v4l_do_munmap(s);
	ms_message("v4l_thread exited.");
	ms_thread_exit(NULL);
}


static void v4l_process(MSFilter * obj){
	V4lState *s=(V4lState*)obj->data;
	mblk_t *m;
	uint32_t timestamp;
	int cur_frame;
	if (s->frame_count==-1){
		s->start_time=obj->ticker->time;
		s->frame_count=0;
	}
	cur_frame=((obj->ticker->time-s->start_time)*s->fps/1000.0);
	if (cur_frame>s->frame_count){
		mblk_t *om=NULL;
		ms_mutex_lock(&s->mutex);
		/*keep the most recent frame if several frames have been captured */
		if (s->fd!=-1){
			while((m=getq(&s->rq))!=NULL){
				if (om!=NULL) freemsg(om);
				om=m;
			}
		}else{
			if (s->usemire)
				om=dupmsg(v4l_make_mire(s));
			else {
				om=dupmsg(v4l_make_nowebcam(s));
				s->fps=1;
			}
		}
		ms_mutex_unlock(&s->mutex);
		if (om!=NULL){
			timestamp=obj->ticker->time*90;/* rtp uses a 90000 Hz clockrate for video*/
			mblk_set_timestamp_info(om,timestamp);
			ms_queue_put(obj->outputs[0],om);
			/*ms_message("picture sent");*/
		}
		s->frame_count++;
	}
}

static void v4l_preprocess(MSFilter *f){
	V4lState *s=(V4lState*)f->data;
	v4l_start_capture(s);
}

static void v4l_postprocess(MSFilter *f){
	V4lState *s=(V4lState*)f->data;
	v4l_stop_capture(s);
}

static int v4l_set_fps(MSFilter *f, void *arg){
	V4lState *s=(V4lState*)f->data;
	s->fps=*((float*)arg);
	return 0;
}

static int v4l_get_pix_fmt(MSFilter *f,void *arg){
	V4lState *s=(V4lState*)f->data;
	*((MSPixFmt*)arg) = s->pix_fmt;
	return 0;
}

static int v4l_set_vsize(MSFilter *f, void *arg){
	V4lState *s=(V4lState*)f->data;
	s->vsize=*((MSVideoSize*)arg);
	return 0;
}

static MSFilterMethod methods[]={
	{	MS_FILTER_SET_FPS	,	v4l_set_fps	},
	{	MS_FILTER_GET_PIX_FMT	,	v4l_get_pix_fmt	},
	{	MS_FILTER_SET_VIDEO_SIZE, v4l_set_vsize	},
	{	MS_V4L_START			,	v4l_start		},
	{	MS_V4L_STOP			,	v4l_stop		},
	{	0								,	NULL			}
};

MSFilterDesc ms_v4l_desc={
	.id=MS_V4L_ID,
	.name="MSV4l",
	.text="A video4linux compatible source filter to stream pictures.",
	.ninputs=0,
	.noutputs=1,
	.category=MS_FILTER_OTHER,
	.init=v4l_init,
	.preprocess=v4l_preprocess,
	.process=v4l_process,
	.postprocess=v4l_postprocess,
	.uninit=v4l_uninit,
	.methods=methods
};

MS_FILTER_DESC_EXPORT(ms_v4l_desc)
