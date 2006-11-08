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


#ifdef HAVE_CONFIG_H
#include "mediastreamer-config.h"
#endif

#include "mediastreamer2/mediastream.h"

#include "mediastreamer2/dtmfgen.h"
#include "mediastreamer2/mssndcard.h"
#include "mediastreamer2/msrtp.h"
#include "mediastreamer2/msfileplayer.h"
#include "mediastreamer2/msfilerec.h"

#ifdef INET6
	#include <sys/types.h>
#ifndef WIN32
	#include <sys/socket.h>
	#include <netdb.h>
#endif
#endif


#define MAX_RTP_SIZE	1500


/* this code is not part of the library itself, it is part of the mediastream program */
void audio_stream_free(AudioStream *stream)
{
	if (stream->session!=NULL) rtp_session_destroy(stream->session);
	if (stream->rtpsend!=NULL) ms_filter_destroy(stream->rtpsend);
	if (stream->rtprecv!=NULL) ms_filter_destroy(stream->rtprecv);
	if (stream->soundread!=NULL) ms_filter_destroy(stream->soundread);
	if (stream->soundwrite!=NULL) ms_filter_destroy(stream->soundwrite);
	if (stream->encoder!=NULL) ms_filter_destroy(stream->encoder);
	if (stream->decoder!=NULL) ms_filter_destroy(stream->decoder);
	if (stream->dtmfgen!=NULL) ms_filter_destroy(stream->dtmfgen);
	if (stream->ec!=NULL)	ms_filter_destroy(stream->ec);
	if (stream->ticker!=NULL) ms_ticker_destroy(stream->ticker);
	ms_free(stream);
}

static int dtmf_tab[16]={'0','1','2','3','4','5','6','7','8','9','*','#','A','B','C','D'};

static void on_dtmf_received(RtpSession *s, int dtmf, void * user_data)
{
	MSFilter *dtmfgen=(MSFilter*)user_data;
	if (dtmf>15){
		ms_warning("Unsupported telephone-event type.");
		return;
	}
	ms_message("Receiving dtmf %c.",dtmf_tab[dtmf]);
	if (dtmfgen!=NULL){
		ms_filter_call_method(dtmfgen,MS_DTMF_GEN_PUT,&dtmf_tab[dtmf]);
	}
}

#if 0

static void on_timestamp_jump(RtpSession *s,uint32_t* ts, void * user_data)
{
	ms_warning("The remote sip-phone has send data with a future timestamp: %u,"
			"resynchronising session.",*ts);
	rtp_session_reset(s);
}

#endif

static const char *ip4local="0.0.0.0";

#ifdef INET6
static const char *ip6local="::";
#endif

const char *get_local_addr_for(const char *remote)
{
	const char *ret;
#ifdef INET6
	struct addrinfo hints, *res0;
	int err;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	err = getaddrinfo(remote,"8000", &hints, &res0);
	if (err!=0) {
		ms_warning ("get_local_addr_for: %s", gai_strerror(err));
		return ip4local;
	}
	ret=(res0->ai_addr->sa_family==AF_INET6) ? ip6local : ip4local; 
	freeaddrinfo(res0);
#else
	ret=ip4local;
#endif
	return ret;
}

RtpSession * create_duplex_rtpsession(RtpProfile *profile, int locport,const char *remip,int remport, int payload,int jitt_comp){
	RtpSession *rtpr;
	rtpr=rtp_session_new(RTP_SESSION_SENDRECV);
	rtp_session_set_recv_buf_size(rtpr,MAX_RTP_SIZE);
	rtp_session_set_profile(rtpr,profile);
	rtp_session_set_local_addr(rtpr,get_local_addr_for(remip),locport);
	if (remport>0) rtp_session_set_remote_addr(rtpr,remip,remport);
	rtp_session_set_scheduling_mode(rtpr,0);
	rtp_session_set_blocking_mode(rtpr,0);
	rtp_session_set_payload_type(rtpr,payload);
	rtp_session_set_jitter_compensation(rtpr,jitt_comp);
	rtp_session_enable_adaptive_jitter_compensation(rtpr,TRUE);
	/*rtp_session_signal_connect(rtpr,"timestamp_jump",(RtpCallback)on_timestamp_jump,NULL);*/
	return rtpr;
}




AudioStream * audio_stream_start_full(RtpProfile *profile, int locport, const char *remip,int remport, int payload,int jitt_comp, const char *infile, const char *outfile, MSSndCard *playcard, MSSndCard *captcard, bool_t use_ec)
{
	AudioStream *stream=ms_new0(AudioStream,1);
	RtpSession *rtps;
	PayloadType *pt;
	
	rtps=create_duplex_rtpsession(profile,locport,remip,remport,payload,jitt_comp);
	
	stream->rtpsend=ms_filter_new(MS_RTP_SEND_ID);
	ms_filter_call_method(stream->rtpsend,MS_RTP_SEND_SET_SESSION,rtps);
	stream->rtprecv=ms_filter_new(MS_RTP_RECV_ID);
	ms_filter_call_method(stream->rtprecv,MS_RTP_RECV_SET_SESSION,rtps);
	stream->session=rtps;
	
	stream->dtmfgen=ms_filter_new(MS_DTMF_GEN_ID);
	rtp_session_signal_connect(rtps,"telephone-event",(RtpCallback)on_dtmf_received,(unsigned long)stream->dtmfgen);
	
	/* creates the local part */
	if (infile==NULL) stream->soundread=ms_snd_card_create_reader(captcard);
	else {
		stream->soundread=ms_filter_new(MS_FILE_PLAYER_ID);
		ms_filter_call_method(stream->soundread,MS_FILE_PLAYER_OPEN,(void*)infile);
		ms_filter_call_method_noarg(stream->soundread,MS_FILE_PLAYER_START);
	}
	if (outfile==NULL) stream->soundwrite=ms_snd_card_create_writer(playcard);
	else {
		stream->soundwrite=ms_filter_new(MS_FILE_REC_ID);
		ms_filter_call_method(stream->soundwrite,MS_FILE_REC_OPEN,(void*)outfile);
		ms_filter_call_method_noarg(stream->soundwrite,MS_FILE_REC_START);
	}
	
	/* creates the couple of encoder/decoder */
	pt=rtp_profile_get_payload(profile,payload);
	if (pt==NULL){
		ms_error("audiostream.c: undefined payload type.");
		return NULL;
	}
	stream->encoder=ms_filter_create_encoder(pt->mime_type);
	stream->decoder=ms_filter_create_decoder(pt->mime_type);
	if ((stream->encoder==NULL) || (stream->decoder==NULL)){
		/* big problem: we have not a registered codec for this payload...*/
		audio_stream_free(stream);
		ms_error("mediastream.c: No decoder available for payload %i.",payload);
		return NULL;
	}
	
	if (use_ec) stream->ec=ms_filter_new(MS_SPEEX_EC_ID);	

	/* give the sound filters some properties */
	ms_filter_call_method(stream->soundread,MS_FILTER_SET_SAMPLE_RATE,&pt->clock_rate);
	ms_filter_call_method(stream->soundwrite,MS_FILTER_SET_SAMPLE_RATE,&pt->clock_rate);
	if (outfile==NULL) {
		int tmp = 1;

		ms_filter_call_method(stream->soundwrite,MS_FILTER_SET_NCHANNELS, &tmp);
	}
	
	/* give the encoder/decoder some parameters*/
	ms_filter_call_method(stream->encoder,MS_FILTER_SET_SAMPLE_RATE,&pt->clock_rate);
	ms_filter_call_method(stream->encoder,MS_FILTER_SET_BITRATE,&pt->normal_bitrate);
	ms_filter_call_method(stream->decoder,MS_FILTER_SET_SAMPLE_RATE,&pt->clock_rate);
	ms_filter_call_method(stream->decoder,MS_FILTER_SET_BITRATE,&pt->normal_bitrate);
	
	if (pt->send_fmtp!=NULL) ms_filter_call_method(stream->encoder,MS_FILTER_SET_FMTP, (void*)pt->send_fmtp);
	if (pt->recv_fmtp!=NULL) ms_filter_call_method(stream->decoder,MS_FILTER_SET_FMTP,(void*)pt->recv_fmtp);
	
	/* and then connect all */
	/* tip: draw yourself the picture if you don't understand */
	if (stream->ec){
		ms_filter_link(stream->soundread,0,stream->ec,1);
		ms_filter_link(stream->ec,1,stream->encoder,0);
		ms_filter_link(stream->dtmfgen,0,stream->ec,0);
		ms_filter_link(stream->ec,0,stream->soundwrite,0);
	}else{
		ms_filter_link(stream->soundread,0,stream->encoder,0);
		ms_filter_link(stream->dtmfgen,0,stream->soundwrite,0);
	}
	
	ms_filter_link(stream->encoder,0,stream->rtpsend,0);
	ms_filter_link(stream->rtprecv,0,stream->decoder,0);
	ms_filter_link(stream->decoder,0,stream->dtmfgen,0);
	
	/* create ticker */
	stream->ticker=ms_ticker_new();

	ms_ticker_attach(stream->ticker,stream->soundread);
	ms_ticker_attach(stream->ticker,stream->rtprecv);
	
	return stream;
}


AudioStream * audio_stream_start_with_files(RtpProfile *prof,int locport,const char *remip, int remport,int profile,int jitt_comp, const char *infile, const char*outfile)
{
	return audio_stream_start_full(prof,locport,remip,remport,profile,jitt_comp,infile,outfile,NULL,NULL,FALSE);
}

AudioStream * audio_stream_start(RtpProfile *prof,int locport,const char *remip,int remport,int profile,int jitt_comp, bool_t ec)
{
	MSSndCard *sndcard;
	sndcard=ms_snd_card_manager_get_default_card(ms_snd_card_manager_get());
	if (sndcard==NULL)
		return NULL;
	return audio_stream_start_full(prof,locport,remip,remport,profile,jitt_comp,NULL,NULL,sndcard,sndcard,ec);
}

AudioStream *audio_stream_start_with_sndcards(RtpProfile *prof,int locport,const char *remip,int remport,int profile,int jitt_comp,MSSndCard *playcard, MSSndCard *captcard, bool_t ec)
{
	if (playcard==NULL) {
		ms_error("No playback card.");
		return NULL;
	}
	if (captcard==NULL) {
		ms_error("No capture card.");
		return NULL;
	}
	return audio_stream_start_full(prof,locport,remip,remport,profile,jitt_comp,NULL,NULL,playcard,captcard,ec);
}

void audio_stream_set_rtcp_information(AudioStream *st, const char *cname, const char *tool){
	if (st->session!=NULL){
		rtp_session_set_source_description(st->session,cname,NULL,NULL,NULL,NULL,tool , "This is free software (GPL) !");
	}
}

void audio_stream_stop(AudioStream * stream)
{
	
	ms_ticker_detach(stream->ticker,stream->soundread);
	ms_ticker_detach(stream->ticker,stream->rtprecv);
	
	rtp_stats_display(rtp_session_get_stats(stream->session),"Audio session's RTP statistics");

	if (stream->ec!=NULL){
		ms_filter_unlink(stream->soundread,0,stream->ec,1);
		ms_filter_unlink(stream->ec,1,stream->encoder,0);
		ms_filter_unlink(stream->dtmfgen,0,stream->ec,0);
		ms_filter_unlink(stream->ec,0,stream->soundwrite,0);
	}else{
		ms_filter_unlink(stream->soundread,0,stream->encoder,0);
		ms_filter_unlink(stream->dtmfgen,0,stream->soundwrite,0);
	}

	ms_filter_unlink(stream->encoder,0,stream->rtpsend,0);
	ms_filter_unlink(stream->rtprecv,0,stream->decoder,0);
	ms_filter_unlink(stream->decoder,0,stream->dtmfgen,0);
	
	audio_stream_free(stream);
}

RingStream * ring_start(const char *file, int interval, MSSndCard *sndcard){
   return ring_start_with_cb(file,interval,sndcard,NULL,NULL);
}

RingStream * ring_start_with_cb(const char *file,int interval,MSSndCard *sndcard, MSFilterNotifyFunc func,void * user_data)
{
	RingStream *stream;
	int tmp;
	stream=ms_new0(RingStream,1);
	stream->source=ms_filter_new(MS_FILE_PLAYER_ID);
	ms_filter_call_method(stream->source,MS_FILE_PLAYER_OPEN,(void*)file);
	ms_filter_call_method(stream->source,MS_FILE_PLAYER_LOOP,&interval);
	ms_filter_call_method_noarg(stream->source,MS_FILE_PLAYER_START);
	if (func!=NULL)
		ms_filter_set_notify_callback(stream->source,func,user_data);
	stream->sndwrite=ms_snd_card_create_writer(sndcard);
	ms_filter_call_method(stream->source,MS_FILTER_GET_SAMPLE_RATE,&tmp);
	ms_filter_call_method(stream->sndwrite,MS_FILTER_SET_SAMPLE_RATE,&tmp);
	ms_filter_call_method(stream->source,MS_FILTER_GET_NCHANNELS,&tmp);
	ms_filter_call_method(stream->sndwrite,MS_FILTER_SET_NCHANNELS,&tmp);
	stream->ticker=ms_ticker_new();
	ms_filter_link(stream->source,0,stream->sndwrite,0);
	ms_ticker_attach(stream->ticker,stream->source);
	return stream;
}

void ring_stop(RingStream *stream){
	ms_ticker_detach(stream->ticker,stream->source);
	ms_filter_unlink(stream->source,0,stream->sndwrite,0);
	ms_ticker_destroy(stream->ticker);
	ms_filter_destroy(stream->source);
	ms_filter_destroy(stream->sndwrite);
	ms_free(stream);
}


int audio_stream_send_dtmf(AudioStream *stream, char dtmf)
{
	ms_filter_call_method(stream->rtpsend,MS_RTP_SEND_SEND_DTMF,&dtmf);
	ms_filter_call_method(stream->dtmfgen,MS_DTMF_GEN_PUT,&dtmf);
	return 0;
}
