/*
linphone
Copyright (C) 2000  Simon MORLAT (simon.morlat@linphone.org)

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

#include "linphonecore.h"
#include "lpconfig.h"
#include "private.h"
#include "mediastreamer2/mediastream.h"
#include <eXosip.h>
#include "sdphandler.h"

#include <ortp/telephonyevents.h>
#include <ortp/stun.h>
#include "exevents.h"


#ifdef INET6  
#ifndef WIN32
#include <netdb.h>  
#endif
#endif


static const char *liblinphone_version=LIBLINPHONE_VERSION;

#include "enum.h"

void linphone_core_enable_video_preview(LinphoneCore *lc, bool_t val);
bool_t linphone_core_video_preview_enabled(const LinphoneCore *lc);
void linphone_core_get_local_ip(LinphoneCore *lc, const char *dest, char *result);
static void apply_nat_settings(LinphoneCore *lc);
static void toggle_video_preview(LinphoneCore *lc, bool_t val);

/* relative path where is stored local ring*/
#define LOCAL_RING "rings/oldphone.wav"
/* same for remote ring (ringback)*/
#define REMOTE_RING_FR "ringback.wav"
#define REMOTE_RING_US "ringback.wav"


sdp_handler_t linphone_sdphandler={
	linphone_accept_audio_offer,   /*from remote sdp */
	linphone_accept_video_offer,   /*from remote sdp */
	linphone_set_audio_offer,	/*to local sdp */
	linphone_set_video_offer,	/*to local sdp */
	linphone_read_audio_answer,	/*from incoming answer  */
	linphone_read_video_answer	/*from incoming answer  */
};

void lc_callback_obj_init(LCCallbackObj *obj,LinphoneCoreCbFunc func,void* ud)
{
  obj->_func=func;
  obj->_user_data=ud;
}

int lc_callback_obj_invoke(LCCallbackObj *obj, LinphoneCore *lc){
  if (obj->_func!=NULL) obj->_func(lc,obj->_user_data);
  return 0;
}

static void  linphone_call_init_common(LinphoneCall *call, char *from, char *to){
	sdp_context_set_user_pointer(call->sdpctx,(void*)call);
	call->state=LCStateInit;
	call->start_time=time(NULL);
	call->log=linphone_call_log_new(call, from, to);
}

LinphoneCall * linphone_call_new_outgoing(struct _LinphoneCore *lc, const osip_from_t *from, const osip_to_t *to)
{
	LinphoneCall *call=ms_new0(LinphoneCall,1);
	char localip[LINPHONE_IPADDR_SIZE];
	char *fromstr=NULL,*tostr=NULL;
	call->dir=LinphoneCallOutgoing;
	call->cid=-1;
	call->did=-1;
	call->core=lc;
	linphone_core_get_local_ip(lc,to->url->host,localip);
	call->sdpctx=sdp_handler_create_context(&linphone_sdphandler,localip,from->url->username);
	call->profile=lc->local_profile; /*use the readonly local profile*/
	osip_from_to_str(from,&fromstr);
	osip_to_to_str(to,&tostr);
	linphone_call_init_common(call,fromstr,tostr);
	return call;
}


LinphoneCall * linphone_call_new_incoming(LinphoneCore *lc, const char *from, const char *to, int cid, int did)
{
	char localip[LINPHONE_IPADDR_SIZE];
	osip_from_t *from_url=NULL;
	LinphoneCall *call=ms_new0(LinphoneCall,1);
	osip_from_t *me= linphone_core_get_primary_contact_parsed(lc);
	call->dir=LinphoneCallIncoming;
	call->cid=cid;
	call->did=did;
	call->core=lc;
	osip_from_init(&from_url);
	osip_from_parse(from_url, from);
	linphone_core_get_local_ip(lc,from_url->url->host,localip);
	call->sdpctx=sdp_handler_create_context(&linphone_sdphandler,localip,me->url->username);
	linphone_call_init_common(call, osip_strdup(from), osip_strdup(to));
	osip_from_free(me);
	return call;
}

void linphone_call_destroy(LinphoneCall *obj)
{
	linphone_call_log_completed(obj->log,obj);
	if (obj->profile!=NULL && obj->profile!=obj->core->local_profile) rtp_profile_destroy(obj->profile);
	if (obj->sdpctx!=NULL) sdp_context_free(obj->sdpctx);
	ms_free(obj);
}

/*prevent a gcc bug with %c*/
static size_t my_strftime(char *s, size_t max, const char  *fmt,  const struct tm *tm){
	return strftime(s, max, fmt, tm);
}

LinphoneCallLog * linphone_call_log_new(LinphoneCall *call, char *from, char *to){
	LinphoneCallLog *cl=ms_new0(LinphoneCallLog,1);
	struct tm loctime;
	cl->lc=call->core;
	cl->dir=call->dir;
#ifdef WIN32
	loctime=*localtime(&call->start_time);
#else
	localtime_r(&call->start_time,&loctime);
#endif
	my_strftime(cl->start_date,sizeof(cl->start_date),"%c",&loctime);
	cl->from=from;
	cl->to=to;
	return cl;
}
void linphone_call_log_completed(LinphoneCallLog *calllog, LinphoneCall *call){
	LinphoneCore *lc=call->core;
	calllog->duration=time(NULL)-call->start_time;
	switch(call->state){
		case LCStateInit:
			calllog->status=LinphoneCallAborted;
			break;
		case LCStateRinging:
			if (calllog->dir==LinphoneCallIncoming){
				char *info;
				calllog->status=LinphoneCallMissed;
				lc->missed_calls++;
				info=ortp_strdup_printf(_("You have missed %i call(s)."),lc->missed_calls);
				lc->vtable.display_status(lc,info);
				ms_free(info);
			}
			else calllog->status=LinphoneCallAborted;
			break;
		case LCStateAVRunning:
			calllog->status=LinphoneCallSuccess;
			break;
	}
	lc->call_logs=ms_list_append(lc->call_logs,(void *)calllog);
	if (ms_list_size(lc->call_logs)>lc->max_call_logs){
		MSList *elem;
		elem=lc->call_logs;
		linphone_call_log_destroy((LinphoneCallLog*)elem->data);
		lc->call_logs=ms_list_remove_link(lc->call_logs,elem);
	}
	if (lc->vtable.call_log_updated!=NULL){
		lc->vtable.call_log_updated(lc,calllog);
	}
}

char * linphone_call_log_to_str(LinphoneCallLog *cl){
	char *status;
	switch(cl->status){
		case LinphoneCallAborted:
			status=_("aborted");
			break;
		case LinphoneCallSuccess:
			status=_("completed");
			break;
		case LinphoneCallMissed:
			status=_("missed");
			break;
		default:
			status="unknown";
	}
	return ortp_strdup_printf(_("%s at %s\nFrom: %s\nTo: %s\nStatus: %s\nDuration: %i mn %i sec\n"),
			(cl->dir==LinphoneCallIncoming) ? _("Incoming call") : _("Outgoing call"),
			cl->start_date,
			cl->from,
			cl->to,
			status,
			cl->duration/60,
			cl->duration%60);
}

void linphone_call_log_destroy(LinphoneCallLog *cl){
	if (cl->from!=NULL) osip_free(cl->from);
	if (cl->to!=NULL) osip_free(cl->to);
	ms_free(cl);
}


void linphone_core_enable_logs(FILE *file){
	if (file==NULL) file=stdout;
	ortp_set_log_file(file);
	ortp_set_log_level_mask(ORTP_MESSAGE|ORTP_WARNING|ORTP_ERROR|ORTP_FATAL);
	osip_trace_initialize (OSIP_INFO4, file);
}

void linphone_core_disable_logs(){
	int tl;
	for (tl=0;tl<=OSIP_INFO4;tl++) osip_trace_disable_level(tl);
	ortp_set_log_level_mask(ORTP_ERROR|ORTP_FATAL);
}


void
net_config_read (LinphoneCore *lc)
{
	int tmp;
	const char *tmpstr;
	LpConfig *config=lc->config;

	tmp=lp_config_get_int(config,"net","download_bw",0);
	linphone_core_set_download_bandwidth(lc,tmp);
	tmp=lp_config_get_int(config,"net","upload_bw",0);
	linphone_core_set_upload_bandwidth(lc,tmp);
	linphone_core_set_stun_server(lc,lp_config_get_string(config,"net","stun_server",NULL));
	tmpstr=lp_config_get_string(lc->config,"net","nat_address",NULL);
	if (tmpstr!=NULL && (strlen(tmpstr)<1)) tmpstr=NULL;
	linphone_core_set_nat_address(lc,tmpstr);
	tmp=lp_config_get_int(lc->config,"net","firewall_policy",0);
	linphone_core_set_firewall_policy(lc,tmp);
	tmp=lp_config_get_int(lc->config,"net","nat_sdp_only",0);
}


void sound_config_read(LinphoneCore *lc)
{
	/*int tmp;*/
	const char *tmpbuf;
	const char *devid;
	const MSList *elem;
	const char **devices;
	int ndev;
	int i;
	/* retrieve all sound devices */
	elem=ms_snd_card_manager_get_list(ms_snd_card_manager_get());
	ndev=ms_list_size(elem);
	devices=ms_malloc((ndev+1)*sizeof(const char *));
	for (i=0;elem!=NULL;elem=elem->next,i++){
		devices[i]=ms_snd_card_get_string_id((MSSndCard *)elem->data);
	}
	devices[ndev]=NULL;
	lc->sound_conf.cards=devices;
	devid=lp_config_get_string(lc->config,"sound","playback_dev_id",NULL);
	linphone_core_set_playback_device(lc,devid);
	
	devid=lp_config_get_string(lc->config,"sound","ringer_dev_id",NULL);
	linphone_core_set_ringer_device(lc,devid);
	
	devid=lp_config_get_string(lc->config,"sound","capture_dev_id",NULL);
	linphone_core_set_capture_device(lc,devid);
	
/*
	tmp=lp_config_get_int(lc->config,"sound","play_lev",80);
	linphone_core_set_play_level(lc,tmp);
	tmp=lp_config_get_int(lc->config,"sound","ring_lev",80);
	linphone_core_set_ring_level(lc,tmp);
	tmp=lp_config_get_int(lc->config,"sound","rec_lev",80);
	linphone_core_set_rec_level(lc,tmp);
	tmpbuf=lp_config_get_string(lc->config,"sound","source","m");
	linphone_core_set_sound_source(lc,tmpbuf[0]);
*/
	
	tmpbuf=PACKAGE_SOUND_DIR "/" LOCAL_RING;
	tmpbuf=lp_config_get_string(lc->config,"sound","local_ring",tmpbuf);
	if (access(tmpbuf,F_OK)==0) {
		tmpbuf=PACKAGE_SOUND_DIR "/" LOCAL_RING;
	}
	if (strstr(tmpbuf,".wav")==NULL){
		/* it currently uses old sound files, so replace them */
		tmpbuf=PACKAGE_SOUND_DIR "/" LOCAL_RING;
	}
	
	linphone_core_set_ring(lc,tmpbuf);
	
	tmpbuf=PACKAGE_SOUND_DIR "/" REMOTE_RING_FR;
	tmpbuf=lp_config_get_string(lc->config,"sound","remote_ring",tmpbuf);
	if (access(tmpbuf,F_OK)==0){
		tmpbuf=PACKAGE_SOUND_DIR "/" REMOTE_RING_FR;
	}
	if (strstr(tmpbuf,".wav")==NULL){
		/* it currently uses old sound files, so replace them */
		tmpbuf=PACKAGE_SOUND_DIR "/" REMOTE_RING_FR;
	}
	linphone_core_set_ringback(lc,0);
	check_sound_device(lc);
	lc->sound_conf.latency=0;

	linphone_core_enable_echo_cancelation(lc,
		lp_config_get_int(lc->config,"sound","echocancelation",0));
}

void sip_config_read(LinphoneCore *lc)
{
	char *contact;
	const char *tmpstr;
	int port;
	int i,tmp;
	int ipv6;
	port=lp_config_get_int(lc->config,"sip","use_info",0);
	linphone_core_set_use_info_for_dtmf(lc,port);

	ipv6=lp_config_get_int(lc->config,"sip","use_ipv6",-1);
	if (ipv6==-1){
		ipv6=0;
		if (host_has_ipv6_network()){
			lc->vtable.display_message(lc,_("Your machine appears to be connected to an IPv6 network. By default linphone always uses IPv4. Please update your configuration if you want to use IPv6"));
		}
	}
	linphone_core_enable_ipv6(lc,ipv6);
	port=lp_config_get_int(lc->config,"sip","sip_port",5060);
	linphone_core_set_sip_port(lc,port);
	
	tmpstr=lp_config_get_string(lc->config,"sip","contact",NULL);
	if (tmpstr==NULL) {
		char *hostname=getenv("HOST");
		char *username=getenv("USER");
		if (hostname==NULL) hostname=getenv("HOSTNAME");
		if (hostname==NULL)
			hostname="unknown-host";
		if (username==NULL){
			username="toto";
		}
		contact=ortp_strdup_printf("sip:%s@%s",username,hostname);
	}else contact=ms_strdup(tmpstr);
	linphone_core_set_primary_contact(lc,contact);
	ms_free(contact);
	

	tmp=lp_config_get_int(lc->config,"sip","guess_hostname",1);
	linphone_core_set_guess_hostname(lc,tmp);
	
	
	tmp=lp_config_get_int(lc->config,"sip","inc_timeout",15);
	linphone_core_set_inc_timeout(lc,tmp);

	/* get proxies config */
	for(i=0;; i++){
		LinphoneProxyConfig *cfg=linphone_proxy_config_new_from_config_file(lc->config,i);
		if (cfg!=NULL){
			linphone_core_add_proxy_config(lc,cfg);
		}else{
			break;
		}
	}
	/* get the default proxy */
	tmp=lp_config_get_int(lc->config,"sip","default_proxy",-1);
	linphone_core_set_default_proxy_index(lc,tmp);
	
	/* read authentication information */
	for(i=0;; i++){
		LinphoneAuthInfo *ai=linphone_auth_info_new_from_config_file(lc->config,i);
		if (ai!=NULL){
			linphone_core_add_auth_info(lc,ai);
		}else{
			break;
		}
	}
	
}

void rtp_config_read(LinphoneCore *lc)
{
	int port;
	int jitt_comp;
	port=lp_config_get_int(lc->config,"rtp","audio_rtp_port",7078);
	linphone_core_set_audio_port(lc,port);
	
	port=lp_config_get_int(lc->config,"rtp","video_rtp_port",9078);
	if (port==0) port=9078;
	linphone_core_set_video_port(lc,port);
	
	jitt_comp=lp_config_get_int(lc->config,"rtp","audio_jitt_comp",60);
	linphone_core_set_audio_jittcomp(lc,jitt_comp);		
	jitt_comp=lp_config_get_int(lc->config,"rtp","video_jitt_comp",60);
}


PayloadType * get_codec(LpConfig *config, char* type,int index){
	char codeckey[50];
	const char *mime;
	int rate,enabled;
	PayloadType *pt;
	
	snprintf(codeckey,50,"%s_%i",type,index);
	mime=lp_config_get_string(config,codeckey,"mime",NULL);
	if (mime==NULL || strlen(mime)==0 ) return NULL;
	
	pt=payload_type_new();
	pt->mime_type=ms_strdup(mime);
	
	rate=lp_config_get_int(config,codeckey,"rate",8000);
	pt->clock_rate=rate;
	
	enabled=lp_config_get_int(config,codeckey,"enabled",1);
	if (enabled ) pt->flags|=PAYLOAD_TYPE_ENABLED;
	//ms_message("Found codec %s/%i",pt->mime_type,pt->clock_rate);
	return pt;
}

void codecs_config_read(LinphoneCore *lc)
{
	int i;
	PayloadType *pt;
	MSList *audio_codecs=NULL;
	MSList *video_codecs=NULL;
	for (i=0;;i++){
		pt=get_codec(lc->config,"audio_codec",i);
		if (pt==NULL) break;
		audio_codecs=ms_list_append(audio_codecs,(void *)pt);
	}
	for (i=0;;i++){
		pt=get_codec(lc->config,"video_codec",i);
		if (pt==NULL) break;
		video_codecs=ms_list_append(video_codecs,(void *)pt);
	}
	linphone_core_set_audio_codecs(lc,audio_codecs);
	linphone_core_set_video_codecs(lc,video_codecs);
	linphone_core_setup_local_rtp_profile(lc);
}

void video_config_read(LinphoneCore *lc)
{
	int tmp;
	const char *str;
	
	str=lp_config_get_string(lc->config,"video","device","/dev/video0");
	linphone_core_set_video_device(lc,NULL,str);
	
	tmp=lp_config_get_int(lc->config,"video","enabled",1);
#ifdef VIDEO_ENABLED
	linphone_core_enable_video(lc,tmp);
#endif
}

void ui_config_read(LinphoneCore *lc)
{
	LinphoneFriend *lf;
	int i;
	for (i=0;(lf=linphone_friend_new_from_config_file(lc,i))!=NULL;i++){
		linphone_core_add_friend(lc,lf);
	}
	
}

void autoreplier_config_init(LinphoneCore *lc)
{
	autoreplier_config_t *config=&lc->autoreplier_conf;
	config->enabled=lp_config_get_int(lc->config,"autoreplier","enabled",0);
	config->after_seconds=lp_config_get_int(lc->config,"autoreplier","after_seconds",6);
	config->max_users=lp_config_get_int(lc->config,"autoreplier","max_users",1);
	config->max_rec_time=lp_config_get_int(lc->config,"autoreplier","max_rec_time",60);
	config->max_rec_msg=lp_config_get_int(lc->config,"autoreplier","max_rec_msg",10);
	config->message=lp_config_get_string(lc->config,"autoreplier","message",NULL);
}

void linphone_core_set_download_bandwidth(LinphoneCore *lc, int bw){
	lc->net_conf.download_bw=bw;
	if (bw==0){ /*infinite*/
		lc->dw_audio_bw=-1;
		lc->dw_video_bw=-1;
		return;
	}else if (bw>=512){
		lc->dw_audio_bw=80;
	}else if (bw>=128){
		if (linphone_core_video_enabled(lc))
			lc->dw_audio_bw=30;
		else
			lc->dw_audio_bw=bw;
	}else{
		lc->dw_audio_bw=bw;
	}
	lc->dw_video_bw=bw-lc->dw_audio_bw;
}

void linphone_core_set_upload_bandwidth(LinphoneCore *lc, int bw){
	lc->net_conf.upload_bw=bw;
	if (bw==0){ /*infinite*/
		lc->up_audio_bw=-1;
		lc->up_video_bw=-1;
		return;
	}else if (bw>=512){
		lc->up_audio_bw=80;
	}else if (bw>=128){
		if (linphone_core_video_enabled(lc))
			lc->up_audio_bw=30;
		else 
			lc->up_audio_bw=bw;
	}else{
		lc->up_audio_bw=bw;
	}
	lc->up_video_bw=bw-lc->up_audio_bw;
}

int linphone_core_get_download_bandwidth(const LinphoneCore *lc){
	return lc->net_conf.download_bw;
}

int linphone_core_get_upload_bandwidth(const LinphoneCore *lc){
	return lc->net_conf.upload_bw;
}

const char * linphone_core_get_version(void){
	return liblinphone_version;
}

void
linphone_core_init (LinphoneCore * lc, const LinphoneCoreVTable *vtable, const char *config_path, void * userdata)
{
	ortp_init();
	rtp_profile_set_payload(&av_profile,115,&payload_type_lpc1015);
	rtp_profile_set_payload(&av_profile,110,&payload_type_speex_nb);
	rtp_profile_set_payload(&av_profile,111,&payload_type_speex_wb);
	rtp_profile_set_payload(&av_profile,112,&payload_type_ilbc);
	rtp_profile_set_payload(&av_profile,116,&payload_type_truespeech);
	rtp_profile_set_payload(&av_profile,101,&payload_type_telephone_event);
	
	rtp_profile_set_payload(&av_profile,97,&payload_type_theora);
	rtp_profile_set_payload(&av_profile,98,&payload_type_h263_1998);
	rtp_profile_set_payload(&av_profile,99,&payload_type_mp4v);

	ms_init();
	
	memset (lc, 0, sizeof (LinphoneCore));
	lc->data=userdata;
	
	memcpy(&lc->vtable,vtable,sizeof(LinphoneCoreVTable));

	lc->config=lp_config_new(config_path);

  
#ifdef VINCENT_MAURY_RSVP
	/* default qos parameters : rsvp on, rpc off */
	lc->rsvp_enable = 1;
	lc->rpc_enable = 0;
#endif
	sound_config_read(lc);
	net_config_read(lc);
	rtp_config_read(lc);
	codecs_config_read(lc);
	sip_config_read(lc); /* this will start eXosip*/
	video_config_read(lc);
	//autoreplier_config_init(&lc->autoreplier_conf);
	
	lc->presence_mode=LINPHONE_STATUS_ONLINE;
	lc->max_call_logs=15;
	ui_config_read(lc);
	ms_mutex_init(&lc->lock,NULL);
	lc->vtable.display_status(lc,_("Ready"));
}

LinphoneCore *linphone_core_new(const LinphoneCoreVTable *vtable,
						const char *config_path, void * userdata)
{
	LinphoneCore *core=ms_new(LinphoneCore,1);
	linphone_core_init(core,vtable,config_path,userdata);
	return core;
}

const MSList *linphone_core_get_audio_codecs(const LinphoneCore *lc)
{
	return lc->codecs_conf.audio_codecs;
}

const MSList *linphone_core_get_video_codecs(const LinphoneCore *lc)
{
	return lc->codecs_conf.video_codecs;
}

int linphone_core_set_primary_contact(LinphoneCore *lc,const char *contact)
{
	if (lc->sip_conf.contact!=NULL) ms_free(lc->sip_conf.contact);
	lc->sip_conf.contact=ms_strdup(contact);
	if (lc->sip_conf.guessed_contact!=NULL){
		ms_free(lc->sip_conf.guessed_contact);
		lc->sip_conf.guessed_contact=NULL;
	}
	return 0;
}

static bool_t stun_get_localip(LinphoneCore *lc, char *result){
	const char *server=linphone_core_get_stun_server(lc);
	StunAddress4 addr;
	StunAddress4 mapped;
	StunAddress4 changed;
	if (server!=NULL){
		if (stunParseServerName((char*)server,&addr)){
			if (lc->vtable.display_status!=NULL)
				lc->vtable.display_status(lc,_("Stun lookup in progress..."));
			if (stunTest(&addr,1,TRUE,NULL,&mapped,&changed)==0){
				struct in_addr inaddr;
				char *tmp;
				inaddr.s_addr=ntohl(mapped.addr);
				tmp=inet_ntoa(inaddr);
				strncpy(result,tmp,LINPHONE_IPADDR_SIZE);
				if (lc->vtable.display_status!=NULL)
					lc->vtable.display_status(lc,_("Stun lookup done..."));
				ms_message("Stun server says we have address %s",result);
				return TRUE;
			}else{
				ms_warning("stun lookup failed.");
			}
		}else{
			ms_warning("Fail to resolv or parse %s",server);
		}
	}
	return FALSE;
}

/*result must be an array of chars at least LINPHONE_IPADDR_SIZE */
void linphone_core_get_local_ip(LinphoneCore *lc, const char *dest, char *result){
	char *tmp=NULL;
	if (lc->apply_nat_settings){
		apply_nat_settings(lc);
		lc->apply_nat_settings=false;
	}
	if (linphone_core_get_firewall_policy(lc)==LINPHONE_POLICY_USE_NAT_ADDRESS){
		strncpy(result,linphone_core_get_nat_address(lc),LINPHONE_IPADDR_SIZE);
		return;
	}
	if (linphone_core_get_firewall_policy(lc)==LINPHONE_POLICY_USE_STUN) {
		if (lc->sip_conf.ipv6_enabled){
			ms_warning("stun support is not implemented for ipv6");
		}else{
			ms_message("doing stun lookup for local address...");
			if (stun_get_localip(lc,result)){
				if (!lc->net_conf.nat_sdp_only)
					eXosip_set_firewallip(result);
				return;
			}
			ms_warning("stun lookup failed, falling back to a local interface...");
		}
		
	}
	if (dest==NULL){
		/* try a public address */
		if (!lc->sip_conf.ipv6_enabled){
			dest="15.128.128.93";
		}else{
			dest="3ffe:4015:bbbb:70d0:201:2ff:fe09:81b1";
		}
	}
	eXosip_get_localip_for((char*)dest,&tmp);
	eXosip_set_firewallip("");
	strncpy(result,tmp,LINPHONE_IPADDR_SIZE);
	osip_free(tmp);
}

const char *linphone_core_get_primary_contact(LinphoneCore *lc)
{
	char *identity;
	char tmp[LINPHONE_IPADDR_SIZE];
	if (lc->sip_conf.guess_hostname){
		if (lc->sip_conf.guessed_contact==NULL || lc->sip_conf.loopback_only){
			char *guessed=NULL;
			osip_from_t *url;
			if (lc->sip_conf.guessed_contact!=NULL){
				ms_free(lc->sip_conf.guessed_contact);
				lc->sip_conf.guessed_contact=NULL;
			}
			
			osip_from_init(&url);
			if (osip_from_parse(url,lc->sip_conf.contact)==0){
				
			}else ms_error("Could not parse identity contact !");
			linphone_core_get_local_ip(lc, NULL, tmp);
			if (strcmp(tmp,"127.0.0.1")==0 || strcmp(tmp,"::1")==0 ){
				ms_warning("Local loopback network only !");
				lc->sip_conf.loopback_only=TRUE;
			}else lc->sip_conf.loopback_only=FALSE;
			osip_free(url->url->host);
			url->url->host=osip_strdup(tmp);
			if (url->url->port!=NULL){
				osip_free(url->url->port);
				url->url->port=NULL;
			}
			if (lc->sip_conf.sip_port!=5060){
				url->url->port=ortp_strdup_printf("%i",lc->sip_conf.sip_port);
			}
			osip_from_to_str(url,&guessed);
			lc->sip_conf.guessed_contact=guessed;
			
			osip_from_free(url);
			
		}
		identity=lc->sip_conf.guessed_contact;
	}else{
		identity=lc->sip_conf.contact;
	}
	return identity;
}

void linphone_core_set_guess_hostname(LinphoneCore *lc, bool_t val){
	lc->sip_conf.guess_hostname=val;
}
	
bool_t linphone_core_get_guess_hostname(LinphoneCore *lc){
	return lc->sip_conf.guess_hostname;
}

osip_from_t *linphone_core_get_primary_contact_parsed(LinphoneCore *lc){
	int err;
	osip_from_t *contact;
	osip_from_init(&contact);
	err=osip_from_parse(contact,linphone_core_get_primary_contact(lc));
	if (err<0) {
		osip_from_free(contact);
		return NULL;
	}
	return contact;
}

int linphone_core_set_audio_codecs(LinphoneCore *lc, MSList *codecs)
{
	if (lc->codecs_conf.audio_codecs!=NULL) ms_list_free(lc->codecs_conf.audio_codecs);
	lc->codecs_conf.audio_codecs=codecs;
	return 0;
}

int linphone_core_set_video_codecs(LinphoneCore *lc, MSList *codecs)
{
	if (lc->codecs_conf.video_codecs!=NULL) ms_list_free(lc->codecs_conf.video_codecs);
	lc->codecs_conf.video_codecs=codecs;
	return 0;
}

MSList * linphone_core_get_friend_list(LinphoneCore *lc)
{
	return lc->friends;
}

int linphone_core_get_audio_jittcomp(LinphoneCore *lc)
{
	return lc->rtp_conf.audio_jitt_comp;
}

int linphone_core_get_audio_port(const LinphoneCore *lc)
{
	return lc->rtp_conf.audio_rtp_port;
}

int linphone_core_get_video_port(const LinphoneCore *lc){
	return lc->rtp_conf.video_rtp_port;
}

void linphone_core_set_audio_jittcomp(LinphoneCore *lc, int value)
{
	lc->rtp_conf.audio_jitt_comp=value;
}

void linphone_core_set_audio_port(LinphoneCore *lc, int port)
{
	lc->rtp_conf.audio_rtp_port=port;
}

void linphone_core_set_video_port(LinphoneCore *lc, int port){
	lc->rtp_conf.video_rtp_port=port;
}

bool_t linphone_core_get_use_info_for_dtmf(LinphoneCore *lc)
{
	return lc->sip_conf.use_info;
}

void linphone_core_set_use_info_for_dtmf(LinphoneCore *lc,bool_t use_info)
{
	lc->sip_conf.use_info=use_info;
}

int linphone_core_get_sip_port(LinphoneCore *lc)
{
	return lc->sip_conf.sip_port;
}

static bool_t exosip_running=FALSE;

void linphone_core_set_sip_port(LinphoneCore *lc,int port)
{
	int err=0;
	if (port==lc->sip_conf.sip_port) return;
	lc->sip_conf.sip_port=port;
	if (exosip_running) eXosip_quit();
	err=eXosip_init(NULL,stdout,port);
	if (err<0){
		char *msg=ortp_strdup_printf("UDP port %i seems already in use ! Cannot initialize.",port);
		ms_warning(msg);
		lc->vtable.display_warning(lc,msg);
		ms_free(msg);
		return;
	}
#ifdef VINCENT_MAURY_RSVP
	/* tell exosip the qos settings according to default linphone parameters */
	eXosip_set_rsvp_mode (lc->rsvp_enable);
	eXosip_set_rpc_mode (lc->rpc_enable);
#endif
	eXosip_set_user_agent("Linphone-" LINPHONE_VERSION "/eXosip");
	exosip_running=TRUE;
}

bool_t linphone_core_ipv6_enabled(LinphoneCore *lc){
	return lc->sip_conf.ipv6_enabled;
}
void linphone_core_enable_ipv6(LinphoneCore *lc, bool_t val){
	if (lc->sip_conf.ipv6_enabled!=val){
		lc->sip_conf.ipv6_enabled=val;
		eXosip_enable_ipv6(val);
		if (exosip_running){
			/* we need to restart eXosip */
			linphone_core_set_sip_port(lc, lc->sip_conf.sip_port);
		}
	}
}

static void display_bandwidth(RtpSession *as, RtpSession *vs){
	ms_message("bandwidth usage: audio=[d=%.1f,u=%.1f] video=[d=%.1f,u=%.1f] kbit/sec",
	(as!=NULL) ? (rtp_session_compute_recv_bandwidth(as)*1e-3) : 0,
	(as!=NULL) ? (rtp_session_compute_send_bandwidth(as)*1e-3) : 0,
	(vs!=NULL) ? (rtp_session_compute_recv_bandwidth(vs)*1e-3) : 0,
	(vs!=NULL) ? (rtp_session_compute_send_bandwidth(vs)*1e-3) : 0);
}

void linphone_core_iterate(LinphoneCore *lc)
{
	eXosip_event_t *ev;
#ifdef AMD
	printf("entering linphone_core_iterate()\n");
#endif
	if (lc->preview_finished){
		lc->preview_finished=0;
		ring_stop(lc->ringstream);
		lc->ringstream=NULL;
		lc_callback_obj_invoke(&lc->preview_finished_cb,lc);
	}
#ifdef AMD
	printf("getting eXosip event...\n");
#endif
	
	if (exosip_running){
		while((ev=eXosip_event_wait(0,0))!=NULL){
			linphone_core_process_event(lc,ev);
		}
		linphone_core_update_proxy_register(lc);
		linphone_core_refresh_subscribes(lc);
	}
	if (lc->call!=NULL){
		LinphoneCall *call=lc->call;
		int elapsed;
		time_t curtime=time(NULL);
		if (call->dir==LinphoneCallIncoming && call->state==LCStateRinging){
			elapsed=curtime-call->start_time;
			ms_message("incoming call ringing for %i seconds",elapsed);
			if (elapsed>lc->sip_conf.inc_timeout){
				linphone_core_terminate_call(lc,NULL);
			}
		}else if (call->state==LCStateAVRunning){
			elapsed=curtime-lc->prevtime;
			if (elapsed>=1){
				RtpSession *as=NULL,*vs=NULL;
				lc->prevtime=curtime;
				if (lc->audiostream!=NULL)
					as=lc->audiostream->session;
				if (lc->videostream!=NULL)
					vs=lc->videostream->session;
				display_bandwidth(as,vs);
			}
		}
	}
	if (linphone_core_video_preview_enabled(lc)){
		if (lc->previewstream==NULL)
			toggle_video_preview(lc,TRUE);
	}else{
		if (lc->previewstream!=NULL)
			toggle_video_preview(lc,FALSE);
	}
#ifdef AMD
	printf("iterate done...\n");
#endif
}


bool_t linphone_core_is_in_main_thread(LinphoneCore *lc){
	return TRUE;
}

static osip_to_t *osip_to_create(const char *to){
	osip_to_t *ret;
	osip_to_init(&ret);
	if (osip_to_parse(ret,to)<0){
		osip_to_free(ret);
		return NULL;
	}
	return ret;
}

bool_t linphone_core_interpret_url(LinphoneCore *lc, const char *url, char **real_url, osip_to_t **real_parsed_url){
	enum_lookup_res_t *enumres=NULL;
	osip_to_t *parsed_url=NULL;
	char *enum_domain=NULL;
	LinphoneProxyConfig *proxy;
	char *tmpurl;
	
	if (real_url!=NULL) *real_url=NULL;
	if (real_parsed_url!=NULL) *real_parsed_url=NULL;
	
	if (is_enum(url,&enum_domain)){
		lc->vtable.display_status(lc,_("Looking for telephone number destination..."));
		if (enum_lookup(enum_domain,&enumres)<0){
			lc->vtable.display_status(lc,_("Could not resolve this number."));
			ms_free(enum_domain);
			return FALSE;
		}
		ms_free(enum_domain);
		tmpurl=enumres->sip_address[0];
		if (real_url!=NULL) *real_url=ms_strdup(tmpurl);
		if (real_parsed_url!=NULL) *real_parsed_url=osip_to_create(tmpurl);
		enum_lookup_res_free(enumres);
		return TRUE;
	}
	/* check if we have a "sip:" */
	if (strstr(url,"sip:")==NULL){
		/* this doesn't look like a true sip uri */
		proxy=lc->default_proxy;
		if (proxy!=NULL){
			/* append the proxy suffix */
			osip_uri_t *proxy_uri;
			char *sipaddr;
			const char *proxy_addr=linphone_proxy_config_get_addr(proxy);
			osip_uri_init(&proxy_uri);
			if (osip_uri_parse(proxy_uri,proxy_addr)<0){
				osip_uri_free(proxy_uri);
				return FALSE;
			}
			if (proxy_uri->port!=NULL)
				sipaddr=ortp_strdup_printf("sip:%s@%s:%s",url,proxy_uri->host,proxy_uri->port);
			else
				sipaddr=ortp_strdup_printf("sip:%s@%s",url,proxy_uri->host);
			if (real_parsed_url!=NULL) *real_parsed_url=osip_to_create(sipaddr);
			if (real_url!=NULL) *real_url=sipaddr;
			else ms_free(sipaddr);
			return TRUE;
		}
	}
	parsed_url=osip_to_create(url);
	if (parsed_url!=NULL){
		if (real_url!=NULL) *real_url=ms_strdup(url);
		if (real_parsed_url!=NULL) *real_parsed_url=parsed_url;
		else osip_to_free(parsed_url);
		return TRUE;
	}
	/* else we could not do anything with url given by user, so display an error */
	if (lc->vtable.display_warning!=NULL){
		lc->vtable.display_warning(lc,_("Could not parse given sip address. A sip url usually looks like sip:user@domain"));
	}
	return FALSE;
}

const char * linphone_core_get_identity(LinphoneCore *lc){
	LinphoneProxyConfig *proxy=NULL;
	const char *from;
	linphone_core_get_default_proxy(lc,&proxy);
	if (proxy!=NULL) {
		from=linphone_proxy_config_get_identity(proxy);
	}else from=linphone_core_get_primary_contact(lc);
	return from;
}

const char * linphone_core_get_route(LinphoneCore *lc){
	LinphoneProxyConfig *proxy=NULL;
	const char *route=NULL;
	linphone_core_get_default_proxy(lc,&proxy);
	if (proxy!=NULL) {
		route=linphone_proxy_config_get_route(proxy);
	}
	return route;
}

int linphone_core_invite(LinphoneCore *lc, const char *url)
{
	char *barmsg;
	int err=0;
	char *sdpmesg=NULL;
	char *route=NULL;
	const char *from=NULL;
	osip_message_t *invite=NULL;
	sdp_context_t *ctx=NULL;
	LinphoneProxyConfig *proxy=NULL;
	osip_from_t *parsed_url2=NULL;
	osip_to_t *real_parsed_url=NULL;
	char *real_url=NULL;
	
	if (lc->call!=NULL){
		lc->vtable.display_warning(lc,_("Sorry, having multiple simultaneous calls is not supported yet !"));
	}

	linphone_core_get_default_proxy(lc,&proxy);
	if (!linphone_core_interpret_url(lc,url,&real_url,&real_parsed_url)){
		/* bad url */
		return -1;
	}
	barmsg=ortp_strdup_printf("%s %s", _("Contacting"), real_url);
	lc->vtable.display_status(lc,barmsg);
	ms_free(barmsg);
	if (proxy!=NULL) {
		route=linphone_proxy_config_get_route(proxy);
		from=linphone_proxy_config_get_identity(proxy);
	}
	/* if no proxy or no identity defined for this proxy, default to primary contact*/
	if (from==NULL) from=linphone_core_get_primary_contact(lc);

	err=eXosip_build_initial_invite(&invite,(char*)real_url,(char*)from,
								(char*)route,"Phone call");

	if (err<0){
		ms_warning("Could not build initial invite");
		goto end;
	}
	/* make sdp message */
	
	osip_from_init(&parsed_url2);
	osip_from_parse(parsed_url2,from);
	
	lc->call=linphone_call_new_outgoing(lc,parsed_url2,real_parsed_url);
	ctx=lc->call->sdpctx;
	sdpmesg=sdp_context_get_offer(ctx);
	eXosip_lock();
	err=eXosip_initiate_call_with_body(invite,"application/sdp",sdpmesg,(void*)lc->call);
	lc->call->cid=err;
	eXosip_unlock();
	if (err<0){
		ms_warning("Could not initiate call.");
		lc->vtable.display_status(lc,_("could not call"));
		linphone_call_destroy(lc->call);
		lc->call=NULL;
	}
	
	goto end;
	end:
		if (real_url!=NULL) ms_free(real_url);
		if (real_parsed_url!=NULL) osip_to_free(real_parsed_url);
		if (parsed_url2!=NULL) osip_from_free(parsed_url2);
	return (err<0) ? -1 : 0;
}

int linphone_core_refer(LinphoneCore *lc, const char *url)
{
    char *real_url=NULL;
    osip_to_t *real_parsed_url=NULL;
    LinphoneCall *call;
    if (!linphone_core_interpret_url(lc,url,&real_url,&real_parsed_url)){
        /* bad url */
        return -1;
    }
    call=lc->call;
    if (call==NULL){
        ms_warning("No established call to refer.");
        return -1;
    }
    lc->call=NULL;
    
    eXosip_lock();
    eXosip_transfer_call(call->did, real_url);
    eXosip_unlock();
    return 0;
}    

bool_t linphone_core_inc_invite_pending(LinphoneCore*lc){
	if (lc->call==NULL){
		return FALSE;
	}
	return TRUE;
}

#ifdef VINCENT_MAURY_RSVP
/* on=1 for RPC_ENABLE=1...*/
int linphone_core_set_rpc_mode(LinphoneCore *lc, int on)
{
	if (on==1)
		printf("RPC_ENABLE set on\n");
	else 
		printf("RPC_ENABLE set off\n");
	lc->rpc_enable = (on==1);
	/* need to tell eXosip the new setting */
	if (eXosip_set_rpc_mode (lc->rpc_enable)!=0)
		return -1;
	return 0;
}

/* on=1 for RSVP_ENABLE=1...*/
int linphone_core_set_rsvp_mode(LinphoneCore *lc, int on)
{
	if (on==1)
		printf("RSVP_ENABLE set on\n");
	else 
		printf("RSVP_ENABLE set off\n");
	lc->rsvp_enable = (on==1);
	/* need to tell eXosip the new setting */
	if (eXosip_set_rsvp_mode (lc->rsvp_enable)!=0)
		return -1;
	return 0;
}

/* answer : 1 for yes, 0 for no */
int linphone_core_change_qos(LinphoneCore *lc, int answer)
{
	char *sdpmesg;
	if (lc->call==NULL){
		return -1;
	}
	
	if (lc->rsvp_enable && answer==1)
	{
		/* answer is yes, local setting is with qos, so 
		 * the user chose to continue with no qos ! */
		/* so switch in normal mode : ring and 180 */
		lc->rsvp_enable = 0; /* no more rsvp */
		eXosip_set_rsvp_mode (lc->rsvp_enable);
		/* send 180 */
		eXosip_lock();
		eXosip_answer_call(lc->call->did,180,NULL);
		eXosip_unlock();
		/* play the ring */
		ms_message("Starting local ring...");
		lc->ringstream=ring_start(lc->sound_conf.local_ring,
					2000,ms_snd_card_manager_get_card(ms_snd_card_manager_get(),lc->sound_conf.ring_sndcard));
	}
	else if (!lc->rsvp_enable && answer==1)
	{
		/* switch to QoS mode on : answer 183 session progress */
		lc->rsvp_enable = 1;
		eXosip_set_rsvp_mode (lc->rsvp_enable);
		/* take the sdp already computed, see osipuacb.c */
		sdpmesg=lc->call->sdpctx->answerstr;
		eXosip_lock();
		eXosip_answer_call_with_body(lc->call->did,183,"application/sdp",sdpmesg);
		eXosip_unlock();
	}
	else
	{
		/* decline offer (603) */
		linphone_core_terminate_call(lc, NULL);
	}
	return 0;
}
#endif

void linphone_core_start_media_streams(LinphoneCore *lc, LinphoneCall *call){
	osip_from_t *me=linphone_core_get_primary_contact_parsed(lc);
	const char *tool="linphone-" LINPHONE_VERSION;
	char *cname=ortp_strdup_printf("%s@%s",me->url->username,me->url->host);
	{
		int jitt_comp;
		StreamParams *audio_params=&call->audio_params;
		if (!lc->use_files){
			MSSndCard *playcard=lc->sound_conf.play_sndcard;
			MSSndCard *captcard=lc->sound_conf.capt_sndcard;
			if (playcard==NULL) {
				ms_warning("No card defined for playback !");
				goto end;
			}
			if (captcard==NULL) {
				ms_warning("No card defined for capture !");
				goto end;
			}
			
			/* adjust rtp jitter compensation. It must be at least the latency of the sound card */
			jitt_comp=MAX(lc->sound_conf.latency,lc->rtp_conf.audio_jitt_comp);
			lc->audiostream=audio_stream_start_with_sndcards(
				call->profile,
				audio_params->localport,
				audio_params->remoteaddr,
				audio_params->remoteport,
				audio_params->pt,
				jitt_comp,
				playcard,
				captcard,
				linphone_core_echo_cancelation_enabled(lc));
		}else{
			lc->audiostream=audio_stream_start_with_files(
				call->profile,
				audio_params->localport,
				audio_params->remoteaddr,
				audio_params->remoteport,
				audio_params->pt,
				100,
				lc->play_file,
				lc->rec_file);
		}
		if (lc->audiostream==NULL){
			/* should terminate call */
		}
		else{
			audio_stream_set_rtcp_information(lc->audiostream, cname, tool);
		}
	}
#ifdef VIDEO_ENABLED
	{
		/* shutdown preview */
		if (lc->previewstream!=NULL) {
			video_preview_stop(lc->previewstream);
			lc->previewstream=NULL;
		}
		if (lc->video_conf.enabled){
			int jitt_comp;
			StreamParams *video_params=&call->video_params;
			
			if (video_params->remoteport>0){
				/* adjust rtp jitter compensation. It must be at least the latency of the sound card */
				jitt_comp=MAX(lc->sound_conf.latency,lc->rtp_conf.audio_jitt_comp);
				lc->videostream=video_stream_start(call->profile,video_params->localport,
									video_params->remoteaddr,video_params->remoteport,
									video_params->pt,jitt_comp, lc->video_conf.device);
				video_stream_set_rtcp_information(lc->videostream, cname,tool);
			}
		}
	}
#endif
	goto end;
	end:
	ms_free(cname);
	osip_from_free(me);
	lc->call->state=LCStateAVRunning;
}

void linphone_core_stop_media_streams(LinphoneCore *lc){
	if (lc->audiostream!=NULL) {
		audio_stream_stop(lc->audiostream);
		lc->audiostream=NULL;
	}
#ifdef VIDEO_ENABLED
	if (lc->videostream!=NULL){
		video_stream_stop(lc->videostream);
		lc->videostream=NULL;
	}
	if (linphone_core_video_preview_enabled(lc)){
		if (lc->previewstream==NULL){
			lc->previewstream=video_preview_start(lc->video_conf.device);
		}
	}
#endif
}

int linphone_core_accept_call(LinphoneCore *lc, const char *url)
{
	char *sdpmesg;
	
	if (lc->call==NULL){
		return -1;
	}
	
	if (lc->call->state==LCStateAVRunning){
		/*call already accepted*/
		return -1;
	}
	
	/*stop ringing */
	if (lc->ringstream!=NULL) {
		ring_stop(lc->ringstream);
		lc->ringstream=NULL;
	}
	/* sends a 200 OK */
	sdpmesg=lc->call->sdpctx->answerstr;	/* takes the sdp already computed*/
	eXosip_lock();
	eXosip_answer_call_with_body(lc->call->did,200,"application/sdp",sdpmesg);
	eXosip_unlock();
	lc->vtable.display_status(lc,_("Connected."));
	
	linphone_core_start_media_streams(lc, lc->call);
	return 0;
}

int linphone_core_terminate_call(LinphoneCore *lc, const char *url)
{
	LinphoneCall *call=lc->call;
	if (call==NULL){
		return -1;
	}
	lc->call=NULL;
	
	eXosip_lock();
	eXosip_terminate_call(call->cid,call->did);
	eXosip_unlock();
	
	/*stop ringing*/
	if (lc->ringstream!=NULL) {
		ring_stop(lc->ringstream);
		lc->ringstream=NULL;
	}
	linphone_core_stop_media_streams(lc);
	lc->vtable.display_status(lc,_("Call ended") );
	linphone_call_destroy(call);
	return 0;
}

int linphone_core_send_publish(LinphoneCore *lc,
			       LinphoneOnlineStatus presence_mode)
{
	const MSList *elem;
	for (elem=linphone_core_get_proxy_config_list(lc);elem!=NULL;elem=ms_list_next(elem)){
		LinphoneProxyConfig *cfg=(LinphoneProxyConfig*)elem->data;
		if (cfg->publish) linphone_proxy_config_send_publish(cfg,presence_mode);
	}
	return 0;
}

void linphone_core_set_inc_timeout(LinphoneCore *lc, int seconds){
	lc->sip_conf.inc_timeout=seconds;
}

int linphone_core_get_inc_timeout(LinphoneCore *lc){
	return lc->sip_conf.inc_timeout;
}

void linphone_core_set_presence_info(LinphoneCore *lc,int minutes_away,
													const char *contact,
													LinphoneOnlineStatus presence_mode)
{
	int contactok=-1;
	if (minutes_away>0) lc->minutes_away=minutes_away;
	if (contact!=NULL) {
		osip_from_t *url;
		osip_from_init(&url);
		contactok=osip_from_parse(url,contact);
		if (contactok>=0) {
			ms_message("contact url is correct.");
		}
		osip_from_free(url);
		
	}
	if (contactok>=0){
		if (lc->alt_contact!=NULL) ms_free(lc->alt_contact);
		lc->alt_contact=ms_strdup(contact);
	}
	if (lc->presence_mode!=presence_mode){
		linphone_core_notify_all_friends(lc,presence_mode);
		/* 
		   Improve the use of all LINPHONE_STATUS available.
		   !TODO Do not mix "presence status" with "answer status code"..
		   Use correct parameter to follow sip_if_match/sip_etag.
		 */
		linphone_core_send_publish(lc,presence_mode);
	}
	lc->presence_mode=presence_mode;
	
}

/* sound functions */
int linphone_core_get_play_level(LinphoneCore *lc)
{
	return lc->sound_conf.play_lev;
}
int linphone_core_get_ring_level(LinphoneCore *lc)
{
	return lc->sound_conf.ring_lev;
}
int linphone_core_get_rec_level(LinphoneCore *lc){
	return lc->sound_conf.rec_lev;
}
void linphone_core_set_ring_level(LinphoneCore *lc, int level){
	MSSndCard *sndcard;
	lc->sound_conf.ring_lev=level;
	sndcard=lc->sound_conf.ring_sndcard;
	if (sndcard) ms_snd_card_set_level(sndcard,MS_SND_CARD_PLAYBACK,level);
}

void linphone_core_set_play_level(LinphoneCore *lc, int level){
	MSSndCard *sndcard;
	lc->sound_conf.play_lev=level;
	sndcard=lc->sound_conf.play_sndcard;
	if (sndcard) ms_snd_card_set_level(sndcard,MS_SND_CARD_PLAYBACK,level);
}

void linphone_core_set_rec_level(LinphoneCore *lc, int level)
{
	MSSndCard *sndcard;
	lc->sound_conf.rec_lev=level;
	sndcard=lc->sound_conf.capt_sndcard;
	if (sndcard) ms_snd_card_set_level(sndcard,MS_SND_CARD_CAPTURE,level);
}

static MSSndCard *get_card_from_string_id(const char *devid){
	MSSndCard *sndcard=NULL;
	if (devid!=NULL){
		sndcard=ms_snd_card_manager_get_card(ms_snd_card_manager_get(),devid);
	}
	if (sndcard==NULL) sndcard=ms_snd_card_manager_get_default_card(ms_snd_card_manager_get());
	return sndcard;
}

int linphone_core_set_ringer_device(LinphoneCore *lc, const char * devid){
	lc->sound_conf.ring_sndcard=get_card_from_string_id(devid);
	return 0;
}

int linphone_core_set_playback_device(LinphoneCore *lc, const char * devid){
	lc->sound_conf.play_sndcard=get_card_from_string_id(devid);
	return 0;
}

int linphone_core_set_capture_device(LinphoneCore *lc, const char * devid){
	lc->sound_conf.capt_sndcard=get_card_from_string_id(devid);
	return 0;
}

const char * linphone_core_get_ringer_device(LinphoneCore *lc)
{
	return ms_snd_card_get_string_id(lc->sound_conf.ring_sndcard);
}

const char * linphone_core_get_playback_device(LinphoneCore *lc)
{
	return ms_snd_card_get_string_id(lc->sound_conf.play_sndcard);
}

const char * linphone_core_get_capture_device(LinphoneCore *lc)
{
	return ms_snd_card_get_string_id(lc->sound_conf.capt_sndcard);
}

/* returns a static array of string describing the sound devices */ 
const char**  linphone_core_get_sound_devices(LinphoneCore *lc){
	return lc->sound_conf.cards;
}

char linphone_core_get_sound_source(LinphoneCore *lc)
{
	return lc->sound_conf.source;
}

void linphone_core_set_sound_source(LinphoneCore *lc, char source)
{
	MSSndCard *sndcard=lc->sound_conf.capt_sndcard;
	lc->sound_conf.source=source;
	switch(source){
		case 'm':
			ms_snd_card_set_capture(sndcard,MS_SND_CARD_MIC);
			break;
		case 'l':
			ms_snd_card_set_capture(sndcard,MS_SND_CARD_LINE);
			break;
	}
	
}

void linphone_core_set_ring(LinphoneCore *lc,const char *path){
	if (lc->sound_conf.local_ring!=0){
		ms_free(lc->sound_conf.local_ring);
	}
	lc->sound_conf.local_ring=ms_strdup(path);
}
char *linphone_core_get_ring(LinphoneCore *lc){
	return lc->sound_conf.local_ring;
}

static void notify_end_of_ring(void *ud ,unsigned int event, void * arg){
	LinphoneCore *lc=(LinphoneCore*)ud;
	lc->preview_finished=1;
}

int linphone_core_preview_ring(LinphoneCore *lc, const char *ring,LinphoneCoreCbFunc func,void * userdata)
{
	if (lc->ringstream!=0){
		ms_warning("Cannot start ring now,there's already a ring being played");
		return -1;
	}
	lc_callback_obj_init(&lc->preview_finished_cb,func,userdata);
	lc->preview_finished=0;
	if (lc->sound_conf.ring_sndcard!=NULL){
		lc->ringstream=ring_start_with_cb(ring,2000,lc->sound_conf.ring_sndcard,notify_end_of_ring,(void *)lc);
	}
	return 0;
}


void linphone_core_set_ringback(LinphoneCore *lc,RingBackType type){
	switch(type){
		case RINGBACK_TYPE_FR:
			lc->sound_conf.remote_ring=PACKAGE_SOUND_DIR "/" REMOTE_RING_FR;
		break;
		case RINGBACK_TYPE_US:
			lc->sound_conf.remote_ring=PACKAGE_SOUND_DIR "/" REMOTE_RING_US;
		break;
	}
}
RingBackType linphone_core_get_ringback(LinphoneCore *lc);

void linphone_core_enable_echo_cancelation(LinphoneCore *lc, bool_t val){
	lc->sound_conf.ec=val;
}

bool_t linphone_core_echo_cancelation_enabled(LinphoneCore *lc){
	return lc->sound_conf.ec;
}


void linphone_core_send_dtmf(LinphoneCore *lc,char dtmf)
{
  if (linphone_core_get_use_info_for_dtmf(lc)==0){
    /* In Band DTMF */
    if (lc->audiostream!=NULL){
      audio_stream_send_dtmf(lc->audiostream,dtmf);
    }
  }
  else{
	char dtmf_body[1000];
    /* Out of Band DTMF (use INFO method) */
    LinphoneCall *call=lc->call;
    if (call==NULL){
      return;
    }
    
    
    snprintf(dtmf_body, 999, "Signal=%c\r\nDuration=250\r\n", dtmf);
    eXosip_lock();
    eXosip_info_call(call->did, "application/dtmf-relay", dtmf_body);
    eXosip_unlock();
  }
}

void linphone_core_set_stun_server(LinphoneCore *lc, const char *server){
	if (lc->net_conf.stun_server!=NULL)
		ms_free(lc->net_conf.stun_server);
	if (server)
		lc->net_conf.stun_server=ms_strdup(server);
	else lc->net_conf.stun_server=NULL;
	lc->apply_nat_settings=TRUE;
}

const char * linphone_core_get_stun_server(const LinphoneCore *lc){
	return lc->net_conf.stun_server;
}

static void apply_nat_settings(LinphoneCore *lc){
	char *wmsg;
	char *tmp=NULL;
	int err;
	struct addrinfo hints,*res;
	const char *addr=lc->net_conf.nat_address;
	
	if (lc->net_conf.firewall_policy==LINPHONE_POLICY_USE_NAT_ADDRESS && addr!=NULL && strlen(addr)>0){
		/*check the ip address given */
		memset(&hints,0,sizeof(struct addrinfo));
		if (lc->sip_conf.ipv6_enabled)
			hints.ai_family=AF_INET6;
		else 
			hints.ai_family=AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		err=getaddrinfo(addr,NULL,&hints,&res);
		if (err!=0){
			wmsg=ortp_strdup_printf(_("Invalid nat address '%s' : %s"),
				addr, gai_strerror(err));
			ms_warning(wmsg); // what is this for ?
			lc->vtable.display_warning(lc, wmsg);
			ms_free(wmsg);
			linphone_core_set_firewall_policy(lc,LINPHONE_POLICY_NO_FIREWALL);
			return;
		}
		/*now get it as an numeric ip address */
		tmp=ms_malloc0(50);
		err=getnameinfo(res->ai_addr,res->ai_addrlen,tmp,50,NULL,0,NI_NUMERICHOST);
		if (err!=0){
			wmsg=ortp_strdup_printf(_("Invalid nat address '%s' : %s"),
				addr, gai_strerror(err));
			ms_warning(wmsg); // what is this for ?
			lc->vtable.display_warning(lc, wmsg);
			ms_free(wmsg);
			ms_free(tmp);
			freeaddrinfo(res);
			linphone_core_set_firewall_policy(lc,LINPHONE_POLICY_NO_FIREWALL);
			return;
		}
		freeaddrinfo(res);
	}

	if (lc->net_conf.firewall_policy==LINPHONE_POLICY_USE_NAT_ADDRESS){
		if (tmp!=NULL){
			if (!lc->net_conf.nat_sdp_only)
				eXosip_set_firewallip(tmp);
			ms_free(tmp);
		}
		else 
			eXosip_set_firewallip("");
	}
	else {
		eXosip_set_firewallip("");	
	}
}


void linphone_core_set_nat_address(LinphoneCore *lc, const char *addr)
{
	if (lc->net_conf.nat_address!=NULL){
		ms_free(lc->net_conf.nat_address);
	}
	if (addr!=NULL) lc->net_conf.nat_address=ms_strdup(addr);
	else lc->net_conf.nat_address=NULL;
	lc->apply_nat_settings=TRUE;
}

const char *linphone_core_get_nat_address(const LinphoneCore *lc)
{
	return lc->net_conf.nat_address;
}

void linphone_core_set_firewall_policy(LinphoneCore *lc, LinphoneFirewallPolicy pol){
	lc->net_conf.firewall_policy=pol;
	lc->apply_nat_settings=TRUE;
}

LinphoneFirewallPolicy linphone_core_get_firewall_policy(const LinphoneCore *lc){
	return lc->net_conf.firewall_policy;
}

MSList * linphone_core_get_call_logs(LinphoneCore *lc){
	lc->missed_calls=0;
	return lc->call_logs;
}

static void toggle_video_preview(LinphoneCore *lc, bool_t val){
#ifdef VIDEO_ENABLED
	if (lc->videostream==NULL){
		if (val){
			if (lc->previewstream==NULL){
				lc->previewstream=video_preview_start(lc->video_conf.device);
			}
		}else{
			if (lc->previewstream!=NULL){
				video_preview_stop(lc->previewstream);
				lc->previewstream=NULL;
			}
		}
	}
#endif
}

void linphone_core_enable_video(LinphoneCore *lc, bool_t val){
#ifndef VIDEO_ENABLED
	if (val)
		ms_warning("This version of linphone was built without video support.");
#endif
	lc->video_conf.enabled=val;
	lc->video_conf.show_local=val;
	/* need to re-apply network bandwidth settings*/
	linphone_core_set_download_bandwidth(lc,
		linphone_core_get_download_bandwidth(lc));
	linphone_core_set_upload_bandwidth(lc,
		linphone_core_get_upload_bandwidth(lc));
}

bool_t linphone_core_video_enabled(LinphoneCore *lc){
	return lc->video_conf.enabled;
}


bool_t linphone_core_video_preview_enabled(const LinphoneCore *lc){
	return lc->video_conf.show_local;
}

int linphone_core_set_video_device(LinphoneCore *lc, const char *method, const char *device){
	if (lc->video_conf.device!=NULL){
		ms_free(lc->video_conf.device);
		lc->video_conf.device=NULL;
	}
	if (device!=NULL)
		lc->video_conf.device=ms_strdup(device);
	return 0;
}

void linphone_core_use_files(LinphoneCore *lc, bool_t yesno){
	lc->use_files=yesno;
}

void linphone_core_set_play_file(LinphoneCore *lc, const char *file){
	if (lc->play_file!=NULL){
		ms_free(lc->play_file);
		lc->play_file=NULL;
	}
	if (file!=NULL) {
		lc->play_file=ms_strdup(file);
		if (lc->audiostream)
			audio_stream_play(lc->audiostream,file);
	}
}

void linphone_core_set_record_file(LinphoneCore *lc, const char *file){
	if (lc->rec_file!=NULL){
		ms_free(lc->rec_file);
		lc->rec_file=NULL;
	}
	if (file!=NULL) {
		lc->rec_file=ms_strdup(file);
		if (lc->audiostream) 
			audio_stream_record(lc->audiostream,file);
	}
}


void *linphone_core_get_user_data(LinphoneCore *lc){
	return lc->data;
}

void net_config_uninit(LinphoneCore *lc)
{
	net_config_t *config=&lc->net_conf;
	lp_config_set_int(lc->config,"net","download_bw",config->download_bw);
	lp_config_set_int(lc->config,"net","upload_bw",config->upload_bw);
	
	if (config->stun_server!=NULL)
		lp_config_set_string(lc->config,"net","stun_server",config->stun_server);
	if (config->nat_address!=NULL)
		lp_config_set_string(lc->config,"net","nat_address",config->nat_address);
	lp_config_set_int(lc->config,"net","firewall_policy",config->firewall_policy);
}


void sip_config_uninit(LinphoneCore *lc)
{
	MSList *elem;
	int i;
	sip_config_t *config=&lc->sip_conf;
	lp_config_set_int(lc->config,"sip","sip_port",config->sip_port);
	lp_config_set_int(lc->config,"sip","guess_hostname",config->guess_hostname);
	lp_config_set_string(lc->config,"sip","contact",config->contact);
	lp_config_set_int(lc->config,"sip","inc_timeout",config->inc_timeout);
	lp_config_set_int(lc->config,"sip","use_info",config->use_info);
	lp_config_set_int(lc->config,"sip","use_ipv6",config->ipv6_enabled);
	for(elem=config->proxies,i=0;elem!=NULL;elem=ms_list_next(elem),i++){
		LinphoneProxyConfig *cfg=(LinphoneProxyConfig*)(elem->data);
		linphone_proxy_config_write_to_config_file(lc->config,cfg,i);
		linphone_proxy_config_edit(cfg);	/* to unregister */
	}

	if (exosip_running)
	  {
	    int i;
	    for (i=0;i<20;i++)
	      {
		eXosip_event_t *ev;
		while((ev=eXosip_event_wait(0,0))!=NULL){
		  linphone_core_process_event(lc,ev);
		}
#ifndef WIN32
		usleep(100000);
#else
        Sleep(100);
#endif
	      }
	  }
	
	linphone_proxy_config_write_to_config_file(lc->config,NULL,i);	/*mark the end */
	
	for(elem=lc->auth_info,i=0;elem!=NULL;elem=ms_list_next(elem),i++){
		LinphoneAuthInfo *ai=(LinphoneAuthInfo*)(elem->data);
		linphone_auth_info_write_config(lc->config,ai,i);
	}
	linphone_auth_info_write_config(lc->config,NULL,i); /* mark the end */
}

void rtp_config_uninit(LinphoneCore *lc)
{
	rtp_config_t *config=&lc->rtp_conf;
	lp_config_set_int(lc->config,"rtp","audio_rtp_port",config->audio_rtp_port);
	lp_config_set_int(lc->config,"rtp","video_rtp_port",config->video_rtp_port);
	lp_config_set_int(lc->config,"rtp","audio_jitt_comp",config->audio_jitt_comp);
	lp_config_set_int(lc->config,"rtp","video_jitt_comp",config->audio_jitt_comp);
}

void sound_config_uninit(LinphoneCore *lc)
{
	/*char tmpbuf[2];*/
	sound_config_t *config=&lc->sound_conf;
	lp_config_set_string(lc->config,"sound","playback_dev_id",ms_snd_card_get_string_id(config->play_sndcard));
	lp_config_set_string(lc->config,"sound","ringer_dev_id",ms_snd_card_get_string_id(config->ring_sndcard));
	lp_config_set_string(lc->config,"sound","capture_dev_id",ms_snd_card_get_string_id(config->capt_sndcard));
	ms_free(config->cards);
	/*
	lp_config_set_int(lc->config,"sound","rec_lev",config->rec_lev);
	lp_config_set_int(lc->config,"sound","play_lev",config->play_lev);
	lp_config_set_int(lc->config,"sound","ring_lev",config->ring_lev);
	tmpbuf[0]=config->source;
	tmpbuf[1]='\0';
	lp_config_set_string(lc->config,"sound","source",tmpbuf);
	*/
	lp_config_set_string(lc->config,"sound","local_ring",config->local_ring);
	lp_config_set_string(lc->config,"sound","remote_ring",config->remote_ring);
	lp_config_set_int(lc->config,"sound","echocancelation",config->ec);
}

void video_config_uninit(LinphoneCore *lc)
{
	video_config_t *config=&lc->video_conf;
	lp_config_set_int(lc->config,"video","enabled",config->enabled);
	lp_config_set_int(lc->config,"video","show_local",config->show_local);
}

void codecs_config_uninit(LinphoneCore *lc)
{
	PayloadType *pt;
	codecs_config_t *config=&lc->codecs_conf;
	MSList *node;
	char key[50];
	int index;
	index=0;
	for(node=config->audio_codecs;node!=NULL;node=ms_list_next(node)){
		pt=(PayloadType*)(node->data);
		sprintf(key,"audio_codec_%i",index);
		lp_config_set_string(lc->config,key,"mime",pt->mime_type);
		lp_config_set_int(lc->config,key,"rate",pt->clock_rate);
		lp_config_set_int(lc->config,key,"enabled",payload_type_enabled(pt));
		index++;
	}
	index=0;
	for(node=config->video_codecs;node!=NULL;node=ms_list_next(node)){
		pt=(PayloadType*)(node->data);
		sprintf(key,"video_codec_%i",index);
		lp_config_set_string(lc->config,key,"mime",pt->mime_type);
		lp_config_set_int(lc->config,key,"rate",pt->clock_rate);
		lp_config_set_int(lc->config,key,"enabled",payload_type_enabled(pt));
		index++;
	}
}

void ui_config_uninit(LinphoneCore* lc)
{
	MSList *elem;
	int i;
	for (elem=lc->friends,i=0; elem!=NULL; elem=ms_list_next(elem),i++){
		linphone_friend_write_to_config_file(lc->config,(LinphoneFriend*)elem->data,i);
		linphone_friend_destroy(elem->data);
	}
	linphone_friend_write_to_config_file(lc->config,NULL,i);	/* set the end */
	ms_list_free(lc->friends);
	lc->friends=NULL;
}

LpConfig *linphone_core_get_config(LinphoneCore *lc){
	return lc->config;
}

void linphone_core_uninit(LinphoneCore *lc)
{
#ifdef VIDEO_ENABLED
	if (lc->previewstream!=NULL){
		video_preview_stop(lc->previewstream);
		lc->previewstream=NULL;
	}
#endif
	/* save all config */
	net_config_uninit(lc);
	sip_config_uninit(lc);
	lp_config_set_int(lc->config,"sip","default_proxy",linphone_core_get_default_proxy(lc,NULL));
	rtp_config_uninit(lc);
	sound_config_uninit(lc);
	video_config_uninit(lc);
	codecs_config_uninit(lc);
	ui_config_uninit(lc);
	lp_config_sync(lc->config);
	lp_config_destroy(lc->config);
	
	ortp_exit();
	eXosip_quit();
	exosip_running=FALSE;
}

void linphone_core_destroy(LinphoneCore *lc){
	linphone_core_uninit(lc);
	ms_free(lc);
}
