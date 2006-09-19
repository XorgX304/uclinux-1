/****************************************************************************
 *
 *  $Id: commands.c,v 1.29 2006/08/28 14:29:07 smorlat Exp $
 *
 *  Copyright (C) 2006  Sandro Santilli <strk@keybit.net>
 *  Copyright (C) 2004  Simon MORLAT <simon.morlat@linphone.org>
 *
 ****************************************************************************
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 ****************************************************************************/

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <unistd.h>
#include <linphonecore.h>
#include "linphonec.h"

/***************************************************************************
 *
 *  Forward declarations 
 *
 ***************************************************************************/

extern char *lpc_strip_blanks(char *input);

/* Command handlers */
static int lpc_cmd_help(LinphoneCore *, char *);
static int lpc_cmd_proxy(LinphoneCore *, char *);
static int lpc_cmd_call(LinphoneCore *, char *);
static int lpc_cmd_answer(LinphoneCore *, char *);
static int lpc_cmd_terminate(LinphoneCore *, char *);
static int lpc_cmd_call_logs(LinphoneCore *, char *);
static int lpc_cmd_ipv6(LinphoneCore *, char *);
static int lpc_cmd_refer(LinphoneCore *, char *);
static int lpc_cmd_quit(LinphoneCore *, char *);
static int lpc_cmd_nat(LinphoneCore *, char *);
static int lpc_cmd_friend(LinphoneCore *, char*);
static int lpc_cmd_soundcard(LinphoneCore *, char *);
/* Command handler helpers */
static void linphonec_proxy_add(LinphoneCore *lc);
static void linphonec_proxy_display(LinphoneProxyConfig *lc);
static void linphonec_proxy_list(LinphoneCore *lc);
static void linphonec_proxy_remove(LinphoneCore *lc, int index);
static  int linphonec_proxy_use(LinphoneCore *lc, int index);
static void linphonec_friend_display(LinphoneFriend *fr);
static int linphonec_friend_list(LinphoneCore *lc, char *arg);
static void linphonec_display_command_help(LPC_COMMAND *cmd);
static int linphonec_friend_call(LinphoneCore *lc, unsigned int num);

/* Command table management */
static LPC_COMMAND *lpc_find_command(const char *name);

/***************************************************************************
 *
 *  Global variables
 *
 ***************************************************************************/

/*
 * Commands table.
 */
LPC_COMMAND commands[] = {
	{ "help", lpc_cmd_help, "Print commands help", NULL },
	{ "call", lpc_cmd_call, "Call a SIP uri",
		"'call <sip-url>' or 'c <sip-url>' "
		": initiate a call to the specified destination."
		},
	{ "terminate", lpc_cmd_terminate, "Terminate the current call",
		NULL },
	{ "answer", lpc_cmd_answer, "Answer a call",
		"Accept an incoming call."
	},
	{ "proxy", lpc_cmd_proxy, "Manage proxies",
		"'proxy list' : list all proxy setups.\n"
		"'proxy add' : add a new proxy setup.\n"
		"'proxy remove <index>' : remove proxy setup with number index.\n"
		"'proxy use <index>' : use proxy with number index as default proxy.\n"
		"'proxy unuse' : don't use a default proxy."
	},
	{ "soundcard", lpc_cmd_soundcard, "Manage soundcards",
		"'soundcard list' : list all sound devices.\n"
		"'soundcard use <index>' : select a sound device.\n"
	},
	{ "ipv6", lpc_cmd_ipv6, "Use IPV6",
		"'ipv6 status' : show ipv6 usage status.\n"
		"'ipv6 enable' : enable the use of the ipv6 network.\n"
		"'ipv6 disable' : do not use ipv6 network."
	},
	{ "refer", lpc_cmd_refer,
		"Refer the current call to the specified destination.",
		"'refer <sip-url>' or 'r <sip-url>' "
		": refer the current call to the specified destination."
	},
	{ "nat", lpc_cmd_nat, "Set nat address",
		"'nat'        : show nat settings.\n"
		"'nat <addr>' : set nat address (and enable its use).\n"
		"'nat use'    : enable nat use (ip must be set).\n"
		"'nat unuse'  : disable nat use.\n"
	},
        { "call-logs", lpc_cmd_call_logs, "Calls history",
                NULL },
	{ "friend", lpc_cmd_friend, "Manage friends",
		"'friend list [<pattern>]'    : list friends.\n"
		"'friend call <index>         : call a friend."
	},
	{ "quit", lpc_cmd_quit, "Exit linphonec", NULL },
	{ (char *)NULL, (lpc_cmd_handler)NULL, (char *)NULL, (char *)NULL }
};

/***************************************************************************
 *
 *  Public interface 
 *
 ***************************************************************************/

/*
 * Main command dispatcher.
 * WARNING: modifies second argument!
 *
 * Always return 1 currently.
 */
int
linphonec_parse_command_line(LinphoneCore *lc, char *cl)
{
	char *ptr=cl;
	char *args=NULL;
	LPC_COMMAND *cmd;

	/* Isolate first word and args */
	while(*ptr && !isspace(*ptr)) ++ptr;
	if (*ptr)
	{
		*ptr='\0';
 		/* set args to first nonblank */
		args=ptr+1;
		while(*args && isspace(*args)) ++args;
	}

	/* Handle DTMF */
	if ( isdigit(*cl) || *cl == '#' || *cl == '*' )
	{
		while ( isdigit(*cl) || *cl == '#' || *cl == '*' )
		{
			linphone_core_send_dtmf(lc, *cl);
			sleep(1); // be nice
			++cl;
		}

		// discard spurious trailing chars
		return 1;
	}

	/* Handle other kind of commands */
	cmd=lpc_find_command(cl);
	if ( !cmd )
	{
		printf("'%s': Cannot understand this.\n", cl);
		return 1;
	}

	if ( ! cmd->func(lc, args) )
	{
		printf("Syntax error.\n");
		linphonec_display_command_help(cmd);
	}

	return 1;
}

/*
 * Generator function for command completion.
 * STATE let us know whether to start from scratch;
 * without any state (STATE==0), then we start at the
 * top of the list.
 */
char *
linphonec_command_generator(const char *text, int state)
{
	static int index, len;
	char *name;

	if ( ! state )
	{
		index=0;
		len=strlen(text);
	}

	/*
 	 * Return the next name which partially matches
	 * from the commands list
	 */
	while ((name=commands[index].name))
	{
		++index; /* so next call get next command */

		if (strncmp(name, text, len) == 0)
		{
			return strdup(name);
		}
	}

	return NULL;
}


/***************************************************************************
 *
 *  Command handlers 
 *
 ***************************************************************************/

static int
lpc_cmd_help(LinphoneCore *lc, char *arg)
{
	int i=0;
	LPC_COMMAND *cmd;

	if (!arg || !*arg)
	{
		printf("Commands are:\n");
		printf("---------------------------\n");

		while (commands[i].help)
		{
			printf("%10.10s\t%s\n", commands[i].name,
				commands[i].help);
			i++;
		}
		
		printf("---------------------------\n");
		printf("Type 'help <command>' for more details.\n");

		return 1;
	}

	cmd=lpc_find_command(arg);
	if ( !cmd )
	{
		printf("No such command.\n");
		return 1;
	}

	linphonec_display_command_help(cmd);
	return 1;

}

static int
lpc_cmd_call(LinphoneCore *lc, char *args)
{
	if ( lc->call != NULL )
	{
		printf("Terminate current call first.\n");
	}
	else
	{
		if ( -1 == linphone_core_invite(lc, args) )
		{
			printf("Error from linphone_core_invite.\n");
		}
		else
		{
			/* current_call=args; */
		}
	}
	return 1;
}

static int
lpc_cmd_refer(LinphoneCore *lc, char *args)
{
	linphone_core_refer(lc, args);
	return 1;
}

static int
lpc_cmd_terminate(LinphoneCore *lc, char *args)
{
	if ( -1 == linphone_core_terminate_call(lc, NULL) )
	{
		printf("No active call.\n");
	}
	return 1;
}

static int
lpc_cmd_answer(LinphoneCore *lc, char *args)
{
	if ( -1 == linphone_core_accept_call(lc, NULL) )
	{
		printf("No incoming call.\n");
	}
	return 1;
}

static int
lpc_cmd_quit(LinphoneCore *lc, char *args)
{
	linphonec_finish(EXIT_SUCCESS);
	return 1;
}

static int
lpc_cmd_nat(LinphoneCore *lc, char *args)
{
	bool_t use;
	const char *nat;

	if ( args ) args=lpc_strip_blanks(args);

	if ( args && *args )
	{
		if ( ! strncmp(args, "use", 3) )
		{
			linphone_core_use_nat_address(lc,TRUE);
		}
		else if ( ! strncmp(args, "unuse", 5) )
		{
			linphone_core_use_nat_address(lc,FALSE);
		}
		else
		{
			linphone_core_set_nat_address(lc, args);
			linphone_core_use_nat_address(lc,TRUE);
		}
	}

	nat = linphone_core_get_nat_address(lc);
	use = linphone_core_nat_address_used(lc);
	printf("Nat address: %s%s\n", nat, use?"":" (unused)");

	return 1;
}

static int
lpc_cmd_friend(LinphoneCore *lc, char *args)
{
	int friend_num;

	if ( args ) args=lpc_strip_blanks(args);

	if ( ! args || ! *args ) return 0;

	if ( !strncmp(args, "list", 4) )
	{
		return linphonec_friend_list(lc, args+4);
		return 1;
	}
	else if ( !strncmp(args, "call", 4) )
	{
		args+=4;
		if ( ! *args ) return 0;
		friend_num = strtol(args, NULL, 10);
		if ( errno == ERANGE ) {
			printf("Invalid friend number\n");
			return 0;
		}
		linphonec_friend_call(lc, friend_num);
		return 1;
	}

	return 0;
}

/*
 * Modified input
 */
static int
lpc_cmd_proxy(LinphoneCore *lc, char *args)
{
	char *arg1 = args;
	char *arg2 = NULL;
	char *ptr = args;
	int proxynum;

	if ( ! arg1 ) return 0;

	/* Isolate first and second arg */
	while(*ptr && !isspace(*ptr)) ++ptr;
	if ( *ptr )
	{
		*ptr='\0';
		arg2=ptr+1;
		while(*arg2 && isspace(*arg2)) ++arg2;
	}

	if (strcmp(arg1,"add")==0)
	{
		rl_inhibit_completion=1;
		linphonec_proxy_add(lc);
		rl_inhibit_completion=0;
	}
	else if (strcmp(arg1,"list")==0)
	{
		linphonec_proxy_list(lc);
	}
	else if (strcmp(arg1,"remove")==0)
	{
		linphonec_proxy_remove(lc,atoi(arg2));
	}
	else if (strcmp(arg1,"use")==0)
	{
		if ( arg2 && *arg2 )
		{
			proxynum=atoi(arg2);
			if ( linphonec_proxy_use(lc, proxynum) )
				printf("Default proxy set to %d.\n", proxynum);
		}
		else
		{
			proxynum=linphone_core_get_default_proxy(lc, NULL);
			if ( proxynum == -1 ) printf("No default proxy.\n");
			else printf("Current default proxy is %d.\n", proxynum);
		}
	}
	else if (strcmp(arg1, "unuse")==0)
	{
		linphone_core_set_default_proxy(lc, NULL);
		printf("Use no proxy.\n");
	}
	else
	{
		printf("Syntax error - see 'help proxy'\n");
	}

	return 1;
}

static int
lpc_cmd_call_logs(LinphoneCore *lc, char *args)
{
	MSList *elem=linphone_core_get_call_logs(lc);
	for (;elem!=NULL;elem=ms_list_next(elem))
	{
		LinphoneCallLog *cl=(LinphoneCallLog*)elem->data;
		char *str=linphone_call_log_to_str(cl);
		printf("%s\n",str);
		ms_free(str);
	}
	return 1;
}

static int
lpc_cmd_ipv6(LinphoneCore *lc, char *arg1)
{
	if ( ! arg1 )
	{
		printf("Syntax error - see 'help ipv6'\n");
		return 1;
	}

	if (strcmp(arg1,"status")==0)
	{
		printf("ipv6 use enabled: %s\n",linphone_core_ipv6_enabled(lc) ? "true":"false");
	}
	else if (strcmp(arg1,"enable")==0)
	{
		linphone_core_enable_ipv6(lc,TRUE);
		printf("ipv6 use enabled.\n");
	}
	else if (strcmp(arg1,"disable")==0)
	{
		linphone_core_enable_ipv6(lc,FALSE);
		printf("ipv6 use disabled.\n");
	}
	else
	{
		printf("Syntax error - see 'help ipv6'\n");
	}
	return 1;
}

static int lpc_cmd_soundcard(LinphoneCore *lc, char *cmd){
	int i;
	if (cmd==NULL){
		printf("Syntax error - see 'help soundcard'\n");
		return 1;
	}
	if (strcmp(cmd,"list")==0){
		const char **dev=linphone_core_get_sound_devices(lc);
		for(i=0;dev[i]!=NULL;i++){
			printf("%i: %s\n",i,dev[i]);
		}
		return 1;
	}else{
		char *tmp=alloca(strlen(cmd)+1);
		int index;
		int n=sscanf(cmd,"%s %i",tmp,&index);
		if (n==2 && strcmp(tmp,"use")==0){
			const char **dev=linphone_core_get_sound_devices(lc);
			for(i=0;dev[i]!=NULL;i++){
				if (i==index){
					linphone_core_set_ringer_device(lc,dev[i]);
					linphone_core_set_playback_device(lc,dev[i]);
					linphone_core_set_capture_device(lc,dev[i]);
					printf("Using sound device %s\n",dev[i]);
					return 1;
				}
			}
			printf("no such sound device\n");
			return 1;
		}
		printf("Syntax error - see 'help soundcard'\n");
	}
	return 1;
}

/***************************************************************************
 *
 *  Commands helper functions
 *
 ***************************************************************************/


static void
linphonec_proxy_add(LinphoneCore *lc)
{
	bool_t enable_register=FALSE;
	LinphoneProxyConfig *cfg;

	printf("Adding new proxy setup. Hit ^D to abort.\n");

	/*
	 * SIP Proxy address
	 */
	while (1)
	{
		char *input=readline("Enter proxy sip address: ");
		char *clean;

		if ( ! input ) {
			printf("Aborted.\n");
			return;
		}

		/* Strip blanks */
		clean=lpc_strip_blanks(input);
		if ( ! *clean ) {
			free(input);
			continue;
		}

		cfg=linphone_proxy_config_new(clean);
		if (cfg==NULL)
		{
			printf("Invalid sip address (sip:sip.domain.tld).\n");
			free(input);
			continue;
		}
		free(input);
		break;
	}

	/*
	 * SIP Proxy identity
	 */
	while (1)
	{
		char *input=readline("Your identity for this proxy: ");
		char *clean;

		if ( ! input ) {
			printf("Aborted.\n");
			linphone_proxy_config_destroy(cfg);
			return;
		}

		/* Strip blanks */
		clean=lpc_strip_blanks(input);
		if ( ! *clean ) {
			free(input);
			continue;
		}

		linphone_proxy_config_set_identity(cfg, clean);
		if ( ! cfg->reg_identity )
		{
			printf("Invalid identity (sip:name@sip.domain.tld).\n");
			free(input);
			continue;
		}
		free(input);
		break;
	}

	/*
	 * SIP Proxy enable register
	 */
	while (1)
	{
		char *input=readline("Do you want to register on this proxy (yes/no): ");
		char *clean;

		if ( ! input ) {
			printf("Aborted.\n");
			linphone_proxy_config_destroy(cfg);
			return;
		}

		/* Strip blanks */
		clean=lpc_strip_blanks(input);
		if ( ! *clean ) {
			free(input);
			continue;
		}

		if ( ! strcmp(clean, "yes") ) enable_register=TRUE;
		else if ( ! strcmp(clean, "no") ) enable_register=FALSE;
		else {
			printf("Please answer with 'yes' or 'no'\n");
			free(input);
			continue;
		}
		linphone_proxy_config_enableregister(cfg, enable_register);
		free(input);
		break;
	}

	/*
	 * SIP Proxy registration expiration
	 */
	if ( enable_register==TRUE )
	{
		long int expires=0;
		while (1)
		{
			char *input=readline("Specify register expiration time"
				" in seconds (default is 600): ");

			if ( ! input ) {
				printf("Aborted.\n");
				linphone_proxy_config_destroy(cfg);
				return;
			}

			expires=strtol(input, (char **)NULL, 10);
			if ( expires == LONG_MIN || expires == LONG_MAX )
			{
				printf("Invalid value: %s\n", strerror(errno));
				free(input);
				continue;
			}

			linphone_proxy_config_expires(cfg, expires);
			printf("Expiration: %d seconds\n", cfg->expires);

			free(input);
			break;
		}
	}

	/*
	 * SIP proxy route
	 */
	while (1)
	{
		char *input=readline("Specify route if needed: ");
		char *clean;

		if ( ! input ) {
			printf("Aborted.\n");
			linphone_proxy_config_destroy(cfg);
			return;
		}

		/* Strip blanks */
		clean=lpc_strip_blanks(input);
		if ( ! *clean ) {
			free(input);
			printf("No route specified.\n");
			break;
		}

		linphone_proxy_config_set_route(cfg, clean);
		if ( ! cfg->reg_route )
		{
			printf("Invalid route.\n");
			free(input);
			continue;
		}

		free(input);
		break;
	}

	/*
	 * Final confirmation 
	 */
	while (1)
	{
		char *input;
		char *clean;

		printf("--------------------------------------------\n");
		linphonec_proxy_display(cfg);
		printf("--------------------------------------------\n");
		input=readline("Accept the above proxy configuration (yes/no) ?: ");


		if ( ! input ) {
			printf("Aborted.\n");
			linphone_proxy_config_destroy(cfg);
			return;
		}

		/* Strip blanks */
		clean=lpc_strip_blanks(input);
		if ( ! *clean ) {
			free(input);
			continue;
		}

		if ( ! strcmp(clean, "yes") ) break;
		else if ( ! strcmp(clean, "no") )
		{
			printf("Declined.\n");
			linphone_proxy_config_destroy(cfg);
			free(input);
			return;
		}

		printf("Please answer with 'yes' or 'no'\n");
		free(input);
		continue;
	}


	linphone_core_add_proxy_config(lc,cfg);

	/* automatically set the last entered proxy as the default one */
	linphone_core_set_default_proxy(lc,cfg);

	printf("Proxy added.\n");
}

static void
linphonec_proxy_display(LinphoneProxyConfig *cfg)
{
	printf("sip address: %s\nroute: %s\nidentity: %s\nregister: %s\nexpires: %i\n",
			cfg->reg_proxy,
			(cfg->reg_route!=NULL)?cfg->reg_route:"",
			(cfg->reg_identity!=NULL)?cfg->reg_identity:"",
			(cfg->reg_sendregister)?"yes":"no",
			cfg->expires);
}

static void
linphonec_proxy_list(LinphoneCore *lc)
{
	MSList *proxies;
	int n;
	int def=linphone_core_get_default_proxy(lc,NULL);
	
	proxies=linphone_core_get_proxy_config_list(lc);
	for(n=0;proxies!=NULL;proxies=ms_list_next(proxies),n++){
		if (n==def)
			printf("****** Proxy %i - this is the default one - *******\n",n);
		else 
			printf("****** Proxy %i *******\n",n);
		linphonec_proxy_display((LinphoneProxyConfig*)proxies->data);
	}
}

static void
linphonec_proxy_remove(LinphoneCore *lc, int index)
{
	MSList *proxies;
	LinphoneProxyConfig *cfg;
	proxies=linphone_core_get_proxy_config_list(lc);
	cfg=(LinphoneProxyConfig*)ms_list_nth_data(proxies,index);
	if (cfg==NULL){
		printf("No such proxy.\n");
		return;
	}
	linphone_core_remove_proxy_config(lc,cfg);
	printf("Proxy %s removed.\n", cfg->reg_proxy);
	linphone_proxy_config_destroy(cfg);
}

static int
linphonec_proxy_use(LinphoneCore *lc, int index)
{
	MSList *proxies;
	LinphoneProxyConfig *cfg;
	proxies=linphone_core_get_proxy_config_list(lc);
	cfg=(LinphoneProxyConfig*)ms_list_nth_data(proxies,index);
	if (cfg==NULL){
		printf("No such proxy (try 'proxy list').");
		return 0;
	}
	linphone_core_set_default_proxy(lc,cfg);
	return 1;
}

static void
linphonec_friend_display(LinphoneFriend *fr)
{
	char *name = linphone_friend_get_name(fr);
	char *addr = linphone_friend_get_addr(fr);
	//char *url = linphone_friend_get_url(fr);

	printf("name: %s\n", name);
	printf("address: %s\n", addr);
}

static int
linphonec_friend_list(LinphoneCore *lc, char *pat)
{
	MSList *friend;
	int n;

	if (pat) {
		pat=lpc_strip_blanks(pat);
		if (!*pat) pat = NULL;
	}

	friend = linphone_core_get_friend_list(lc);
	for(n=0; friend!=NULL; friend=ms_list_next(friend), ++n )
	{
		if ( pat ) {
			char *name = linphone_friend_get_name(friend->data);
			if ( ! strstr(name, pat) ) continue;
		}
		printf("****** Friend %i *******\n",n);
		linphonec_friend_display((LinphoneFriend*)friend->data);
	}

	return 1;
}

static int
linphonec_friend_call(LinphoneCore *lc, unsigned int num)
{
	MSList *friend = linphone_core_get_friend_list(lc);
	unsigned int n;
	char *addr;

	for(n=0; friend!=NULL; friend=ms_list_next(friend), ++n )
	{
		if ( n == num )
		{
			addr = linphone_friend_get_addr(friend->data);
			return lpc_cmd_call(lc, addr);
		}
	}
	printf("No such friend %u\n", num);
	return 1;
}

static void
linphonec_display_command_help(LPC_COMMAND *cmd)
{
	if ( cmd->doc ) printf ("%s\n", cmd->doc);
	else printf("%s\n", cmd->help);
}

/***************************************************************************
 *
 *  Command table management funx
 *
 ***************************************************************************/

/*
 * Find a command given its name
 */
static LPC_COMMAND *
lpc_find_command(const char *name)
{
	int i;

	for (i=0; commands[i].name; ++i)
	{
		if (strcmp(name, commands[i].name) == 0)
			return &commands[i];
	}

	return (LPC_COMMAND *)NULL;
}


/****************************************************************************
 *
 * $Log: commands.c,v $
 * Revision 1.29  2006/08/28 14:29:07  smorlat
 * fix bug.
 *
 * Revision 1.28  2006/08/21 12:49:59  smorlat
 * merged several little patches.
 *
 * Revision 1.27  2006/07/17 18:45:00  smorlat
 * support for several event queues in ortp.
 * glib dependency removed from coreapi/ and console/
 *
 * Revision 1.26  2006/04/14 15:16:36  smorlat
 * soundcard use did nothing !
 *
 * Revision 1.25  2006/04/06 20:09:33  smorlat
 * add linphonec command to see and select sound devices.
 *
 * Revision 1.24  2006/03/04 11:17:10  smorlat
 * mediastreamer2 in progress.
 *
 * Revision 1.23  2006/02/20 21:14:01  strk
 * Handled syntax errors with 'friend' command
 *
 * Revision 1.22  2006/02/20 10:20:29  strk
 * Added substring-based filter support for command 'friend list'
 *
 * Revision 1.21  2006/02/02 15:39:18  strk
 * - Added 'friend list' and 'friend call' commands
 * - Allowed for multiple DTFM send in a single line
 * - Added status-specific callback (bare version)
 *
 * Revision 1.20  2006/01/26 11:54:34  strk
 * More robust 'nat' command handler (strip blanks in args)
 *
 * Revision 1.19  2006/01/26 09:48:05  strk
 * Added limits.h include
 *
 * Revision 1.18  2006/01/26 02:18:05  strk
 * Added new commands 'nat use' and 'nat unuse'.
 * These will required a pending patch to linphonecore.c
 * in order to work.
 *
 * Revision 1.17  2006/01/20 14:12:33  strk
 * Added linphonec_init() and linphonec_finish() functions.
 * Handled SIGINT and SIGTERM to invoke linphonec_finish().
 * Handling of auto-termination (-t) moved to linphonec_finish().
 * Reworked main (input read) loop to not rely on 'terminate'
 * and 'run' variable (dropped). configfile_name allocated on stack
 * using PATH_MAX limit. Changed print_usage signature to allow
 * for an exit_status specification.
 *
 * Revision 1.16  2006/01/18 09:25:32  strk
 * Command completion inhibited in proxy addition and auth request prompts.
 * Avoided use of readline's internal filename completion.
 *
 * Revision 1.15  2006/01/14 13:29:32  strk
 * Reworked commands interface to use a table structure,
 * used by command line parser and help function.
 * Implemented first level of completion (commands).
 * Added notification of invalid "answer" and "terminate"
 * commands (no incoming call, no active call).
 * Forbidden "call" intialization when a call is already active.
 * Cleaned up all commands, adding more feedback and error checks.
 *
 * Revision 1.14  2006/01/13 13:00:29  strk
 * Added linphonec.h. Code layout change (added comments, forward decl,
 * globals on top, copyright notices and Logs). Handled out-of-memory
 * condition on history management. Removed assumption on sizeof(char).
 * Fixed bug in authentication prompt (introduced by readline).
 * Added support for multiple authentication requests (up to MAX_PENDING_AUTH).
 *
 *
 ****************************************************************************/
