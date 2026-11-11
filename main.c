// SPDX-License-Identifier: GPL-3.0-only

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <pspsdk.h>
#include <pspiofilemgr.h>
#include <psploadcore.h>
#include <pspmodulemgr.h>

#define MODULE_NAME "ppsspp_simple_ctf_loader"
#define PLUGIN_HOME "ms0:/PSP/PLUGINS/ppsspp_simple_ctf_loader"

#if DEBUG_LOG
#define LOG_PATH PLUGIN_HOME"/log.txt"
#define LOG(...) { \
	char _log_buf[128] = {0}; \
	int _len = sprintf(_log_buf, __VA_ARGS__); \
	int _log_fd = sceIoOpen(LOG_PATH, PSP_O_CREAT | PSP_O_APPEND | PSP_O_WRONLY, 0777); \
	if (_log_fd >= 0){ \
		sceIoWrite(_log_fd, _log_buf, _len); \
		sceIoClose(_log_fd); \
	} \
}
#else
#define LOG(...)
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

PSP_MODULE_INFO(MODULE_NAME, PSP_MODULE_KERNEL, 1, 0);

/* design
 * - hook sceIoOpen
 * - hook sceIoGetstat
 * - for simplicity, the goal is to detect ctf change then extract it
 *   1. check if PLUGIN_HOME/theme.ctf exists, if so, if the hash has changed since last time
 *   2. if the hash is changed, extract the theme file to PLUGIN_HOME/active_theme
 *   3. use ms0:/PSP/PLUGINS/ppsspp_simple_ctf_loader/active_theme as an overlay
 *  makes very good use of the fact that PPSSPP ignore kernel/user boundary, beware when studying this for console plugins
 */

SceUID sceIoOpen_hooked(const char *file, int flags, SceMode mode){
	LOG("%s: opening %s\n", __func__, file);
	return sceIoOpen(file, flags, mode);
}

int sceIoGetstat_hooked(const char *file, SceIoStat *stat){
	LOG("%s: getstat on %s\n", __func__, file);
	return sceIoGetstat(file, stat);
}

uint32_t make_jump_instruction(void *to){
	return 0x08000000 | (((uint32_t)to & 0x0FFFFFFC) >> 2);
}

bool rewrite_import_table_by_nid(char *module_name, char *library_name, uint32_t target_nid, void *function){
	SceModule *mod = sceKernelFindModuleByName(module_name);
	if (mod == NULL){
		LOG("%s: module %s not found\n", __func__, library_name);
		return false;
	}

	SceLibraryStubTable *library = (SceLibraryStubTable *)mod->stub_top;
	while ((uint32_t)library < (uint32_t)mod->stub_top + (uint32_t)mod->stub_size){
		if (strcmp(library_name, library->libname) == 0){
			for (int i = 0;i < library->stubcount;i++){
				uint32_t *nid = (uint32_t *)&library->nidtable[library->vstubcount + i];
				uint32_t *stub = &((uint32_t *)library->stubtable)[i * 2];

				if (nid[0] == target_nid){
					nid[0] = 0;
					stub[0] = make_jump_instruction(function);
					stub[1] = 0; // nop
					return true;
				}
			}
		}

		library = (SceLibraryStubTable *)((uint32_t)library + library->len * 4);
	}

	LOG("%s: module %s did not import %s 0x%lx\n", __func__, module_name, library_name, target_nid);

	return false;

}

void hook(){
	SceUID mod_ids[256] = {0};
	int num_mods = 0;
	sceKernelGetModuleIdList(mod_ids, ARRAY_SIZE(mod_ids), &num_mods);
	for (int i = 0;i < num_mods;i++){
		SceKernelModuleInfo mod_info = {0};
		mod_info.size = sizeof(mod_info);
		sceKernelQueryModuleInfo(mod_ids[i], &mod_info);
		char mod_name[sizeof(mod_info.name) + 1] = {0};
		memcpy(mod_name, mod_info.name, sizeof(mod_info.name));
		if (strcmp(MODULE_NAME, mod_name) == 0){
			continue;
		}
		LOG("%s: hooking %s\n", __func__, mod_name);
		rewrite_import_table_by_nid(mod_name, "IoFileMgrForUser", 0x109F50BC, sceIoOpen_hooked);
		rewrite_import_table_by_nid(mod_name, "IoFileMgrForUser", 0xACE946E8, sceIoGetstat_hooked);
	}
}

int module_start(SceSize args, void *argp){
	#if DEBUG_LOG
	sceIoRemove(LOG_PATH);
	#endif
	LOG("%s: module started\n", __func__);

	hook();

	return 0;
}

int module_stop(SceSize args, void *argp){
	LOG("attempting to stop this module, but unload is not really implemented...");
	return 0;
}
