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

#include "mediastreamer2/mscommon.h"
#include "mediastreamer2/msfilter.h"

#include "alldescs.h"
#include "mediastreamer2/mssndcard.h"

#include <sys/types.h>
#ifndef WIN32
#include <dirent.h>
#else
#ifndef PACKAGE_PLUGINS_DIR
#define PACKAGE_PLUGINS_DIR "."
#endif
#endif
#ifdef HAVE_DLOPEN
#include <dlfcn.h>
#endif


MSList *ms_list_new(void *data){
	MSList *new_elem=ms_new(MSList,1);
	new_elem->prev=new_elem->next=NULL;
	new_elem->data=data;
	return new_elem;
}

MSList * ms_list_append(MSList *elem, void * data){
	MSList *new_elem=ms_list_new(data);
	MSList *it=elem;
	if (elem==NULL) return new_elem;
	while (it->next!=NULL) it=ms_list_next(it);
	it->next=new_elem;
	new_elem->prev=it;
	return elem;
}

MSList * ms_list_prepend(MSList *elem, void *data){
	MSList *new_elem=ms_list_new(data);
	if (elem!=NULL) {
		new_elem->next=elem;
		elem->prev=new_elem;
	}
	return new_elem;
}


MSList * ms_list_concat(MSList *first, MSList *second){
	MSList *it=first;
	if (it==NULL) return second;
	while(it->next!=NULL) it=ms_list_next(it);
	it->next=second;
	second->prev=it;
	return first;
}

MSList * ms_list_free(MSList *list){
	MSList *elem = list;
	MSList *tmp;
	if (list==NULL) return NULL;
	while(elem->next!=NULL) {
		tmp = elem;
		elem = elem->next;
		ms_free(tmp);
	}
	ms_free(elem);
	return NULL;
}

MSList * ms_list_remove(MSList *first, void *data){
	MSList *it;
	it=ms_list_find(first,data);
	if (it) return ms_list_remove_link(first,it);
	else {
		ms_warning("ms_list_remove: no element with %p data was in the list", data);
		return first;
	}
}

int ms_list_size(const MSList *first){
	int n=0;
	while(first!=NULL){
		++n;
		first=first->next;
	}
	return n;
}

void ms_list_for_each(const MSList *list, void (*func)(void *)){
	for(;list!=NULL;list=list->next){
		func(list->data);
	}
}

void ms_list_for_each2(const MSList *list, void (*func)(void *, void *), void *user_data){
	for(;list!=NULL;list=list->next){
		func(list->data,user_data);
	}
}

MSList *ms_list_remove_link(MSList *list, MSList *elem){
	MSList *ret;
	if (elem==list){
		ret=elem->next;
		elem->prev=NULL;
		elem->next=NULL;
		if (ret!=NULL) ret->prev=NULL;
		ms_free(elem);
		return ret;
	}
	elem->prev->next=elem->next;
	if (elem->next!=NULL) elem->next->prev=elem->prev;
	elem->next=NULL;
	elem->prev=NULL;
	ms_free(elem);
	return list;
}

MSList *ms_list_find(MSList *list, void *data){
	for(;list!=NULL;list=list->next){
		if (list->data==data) return list;
	}
	return NULL;
}

MSList *ms_list_find_custom(MSList *list, int (*compare_func)(const void *, const void*), void *user_data){
	for(;list!=NULL;list=list->next){
		if (compare_func(list->data,user_data)==0) return list;
	}
	return NULL;
}

void * ms_list_nth_data(MSList *list, int index){
	int i;
	for(i=0;list!=NULL;list=list->next,++i){
		if (i==index) return list->data;
	}
	ms_error("ms_list_nth_data: no such index in list.");
	return NULL;
}

int ms_list_position(MSList *list, MSList *elem){
	int i;
	for(i=0;list!=NULL;list=list->next,++i){
		if (elem==list) return i;
	}
	ms_error("ms_list_position: no such element in list.");
	return -1;
}

int ms_list_index(MSList *list, void *data){
	int i;
	for(i=0;list!=NULL;list=list->next,++i){
		if (data==list->data) return i;
	}
	ms_error("ms_list_index: no such element in list.");
	return -1;
}

MSList *ms_list_insert_sorted(MSList *list, void *data, int (*compare_func)(const void *, const void*)){
	MSList *it,*previt;
	MSList *nelem;
	MSList *ret=list;
	if (list==NULL) return ms_list_append(list,data);
	nelem=ms_list_new(data);
	for(it=list;it!=NULL;it=it->next){
		previt=it;
		if (compare_func(data,it->data)<=0){
			nelem->prev=it->prev;
			nelem->next=it;
			if (it->prev!=NULL)
				it->prev->next=nelem;
			else{
				ret=nelem;
			}
			it->prev=nelem;
			return ret;
		}
	}
	previt->next=nelem;
	nelem->prev=previt;
	return ret;
}




#define PLUGINS_EXT ".so"

typedef void (*init_func_t)(void);

void ms_load_plugins(const char *dir){
#ifdef WIN32
    WIN32_FIND_DATA FileData; 
    HANDLE hSearch; 
    char szDirPath[] = "plugins\\"; 
    char szPluginFile[256]; 
    BOOL fFinished = FALSE;
    
    // Create a new directory.    
    if (!CreateDirectory(szDirPath, NULL))
    {
        ms_message("plugins directory already exist.");
    }
    
    // Start searching for .TXT files in the current directory.
    
    hSearch = FindFirstFile("plugins\\*.dll", &FileData);
    if (hSearch == INVALID_HANDLE_VALUE)
    {
        ms_message("no plugin (*.dll) found.");
    }

    while (!fFinished) 
    {
        /* load library */
        HINSTANCE os_handle;
        UINT em;
        em = SetErrorMode (SEM_FAILCRITICALERRORS);

        snprintf(szPluginFile, 256, "plugins\\%s", FileData.cFileName);
        os_handle = LoadLibraryEx (szPluginFile, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (os_handle==NULL)
        {
            os_handle = LoadLibraryEx (szPluginFile, NULL, 0);
        }
        SetErrorMode (em);
        if (os_handle==NULL)
            ms_warning("Fail to load plugin %s :", szPluginFile); 
        else
        {
            init_func_t initroutine;
            char szPluginName[256]; 
            char szMethodName[256]; 
            snprintf(szPluginName, 256, "%s", FileData.cFileName);
            szPluginName[strlen(szPluginName)-4]='\0';
            snprintf(szMethodName, 256, "%s_init", szPluginName);
            initroutine = (init_func_t) GetProcAddress (os_handle, szMethodName);
			if (initroutine!=NULL){
				initroutine();
				ms_message("Plugin loaded.");
			}else{
				ms_warning("Could not locate init routine of plugin %s", szPluginFile);
			}
        }


        if (!FindNextFile(hSearch, &FileData)) 
        {
            if (GetLastError() == ERROR_NO_MORE_FILES) 
            { 
                fFinished = TRUE; 
            } 
            else 
            { 
                ms_error("couldn't find next plugin dll."); 
                fFinished = TRUE; 
            } 
        }
    } 
     
    // Close the search handle. 
     
    FindClose(hSearch);

#elif HAVE_DLOPEN
	DIR *ds;
	struct dirent *de;
	char *fullpath;
	ds=opendir(dir);	
	if (ds==NULL){
		ms_message("Cannot open directory %s: %s",dir,strerror(errno));
		return;
	}
	while( (de=readdir(ds))!=NULL){
		if (de->d_type==DT_REG && strstr(de->d_name,PLUGINS_EXT)!=NULL){
			void *handle;
			fullpath=ms_strdup_printf("%s/%s",dir,de->d_name);
			ms_message("Loading plugin %s...",fullpath);
			
			if ( (handle=dlopen(fullpath,RTLD_NOW))==NULL){
				ms_warning("Fail to load plugin %s : %s",fullpath,dlerror());
			}else {
				char *initroutine_name=ms_malloc0(strlen(de->d_name)+10);
				char *p;
				void *initroutine;
				strcpy(initroutine_name,de->d_name);
				p=strstr(initroutine_name,PLUGINS_EXT);
				strcpy(p,"_init");
				initroutine=dlsym(handle,initroutine_name);
				if (initroutine!=NULL){
					init_func_t func=(init_func_t)initroutine;
					func();
					ms_message("Plugin loaded.");
				}else{
					ms_warning("Could not locate init routine of plugin %s",de->d_name);
				}
				ms_free(initroutine_name);
			}
			ms_free(fullpath);
		}
	}
	closedir(ds);
#else
	ms_warning("no loadable plugin support: plugins cannot be loaded.");
#endif
}


#ifdef __ALSA_ENABLED__
extern MSSndCardDesc alsa_card_desc;
#endif

#ifdef HAVE_SYS_SOUNDCARD_H
extern MSSndCardDesc oss_card_desc;
#endif

#ifdef __ARTS_ENABLED__
extern MSSndCardDesc arts_card_desc;
#endif

#ifdef WIN32
extern MSSndCardDesc winsnd_card_desc;
#endif

#ifdef __PORTAUDIO_ENABLED__
extern MSSndCardDesc pasnd_card_desc;
#endif

static MSSndCardDesc * ms_snd_card_descs[]={
#ifdef __ALSA_ENABLED__
	&alsa_card_desc,
#endif
#ifdef HAVE_SYS_SOUNDCARD_H
	&oss_card_desc,
#endif
#ifdef __ARTS_ENABLED__
	&arts_card_desc,
#endif
#ifdef WIN32
	&winsnd_card_desc,
#endif
#ifdef __PORTAUDIO_ENABLED__
	&pasnd_card_desc,
#endif
	NULL
};

void ms_init(){
	int i;
	MSSndCardManager *cm;

	if (getenv("MEDIASTREAMER_DEBUG")!=NULL){
		ortp_set_log_level_mask(ORTP_DEBUG|ORTP_MESSAGE|ORTP_WARNING|ORTP_ERROR|ORTP_FATAL);
	}
	/* register builtin MSFilter's */
	for (i=0;ms_filter_descs[i]!=NULL;i++){
		ms_filter_register(ms_filter_descs[i]);
	}
	/*register SndCardDesc */
	cm=ms_snd_card_manager_get();
	for (i=0;ms_snd_card_descs[i]!=NULL;i++){
		ms_snd_card_manager_register_desc(cm,ms_snd_card_descs[i]);
	}
	ms_load_plugins(PACKAGE_PLUGINS_DIR);
}

extern void ms_snd_card_manager_destroy(void);

void ms_exit(){
	ms_filter_unregister_all();
	ms_snd_card_manager_destroy();
}
