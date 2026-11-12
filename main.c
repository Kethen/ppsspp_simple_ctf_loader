// SPDX-License-Identifier: GPL-3.0-only

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <pspsdk.h>
#include <pspiofilemgr.h>
#include <psploadcore.h>
#include <pspmodulemgr.h>
#include <pspsysmem.h>
#include <psputils.h>
#include <pspthreadman.h>
#include <psploadexec_kernel.h>

#define MODULE_NAME "ppsspp_simple_ctf_loader"
#define PLUGIN_HOME "ms0:/PSP/PLUGINS/ppsspp_simple_ctf_loader"
#define THEME_FILE PLUGIN_HOME"/theme.ctf"
#define THEME_DIR PLUGIN_HOME"/active_theme"
#define EXTRACTED_HASH PLUGIN_HOME"/active_theme_hash"

#if DEBUG_LOG

#define VERBOSE 0

#define LOG_PATH PLUGIN_HOME"/log.txt"
#define LOG(...) { \
	char _log_buf[1024] = {0}; \
	int _len = sprintf(_log_buf, __VA_ARGS__); \
	int _log_fd = sceIoOpen(LOG_PATH, PSP_O_CREAT | PSP_O_APPEND | PSP_O_WRONLY, 0777); \
	if (_log_fd >= 0){ \
		sceIoWrite(_log_fd, _log_buf, _len); \
		sceIoClose(_log_fd); \
	} \
}

#if VERBOSE
#define LOG_VERBOSE(...) LOG(__VA_ARGS__)
#else
#define LOG_VERBOSE(...)
#endif

#else

#define LOG(...)
#define LOG_VERBOSE(...)

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
 *   - makes very good use of the fact that PPSSPP ignore kernel/user boundary, beware when studying this for console plugins
 *   - ppsspp also requires no i/d cache invalidation
 */

bool is_dir(const char *path){
	SceIoStat file_stat = {0};
	int get_stat_status = sceIoGetstat(path, &file_stat);

	if (get_stat_status < 0){
		return false;
	}

	if (!FIO_S_ISDIR(file_stat.st_mode)){
		return false;
	}

	return true;
}

bool is_file(const char *path){
	SceIoStat file_stat = {0};
	int get_stat_status = sceIoGetstat(path, &file_stat);

	if (get_stat_status < 0){
		return false;
	}

	if (FIO_S_ISDIR(file_stat.st_mode)){
		return false;
	}

	return true;
}

void to_lower(char *out, const char *in){
	int len = strlen(in);
	for (int i = 0;i < len;i++){
		out[i] = in[i];
		if (out[i] >= 'A' && out[i] <= 'Z'){
			out[i] = out[i] + ('a' - 'A');
		}
	}
	out[len] = '\0';
}

void overlay(const char *path, char *out_path){
	char path_lowered[256] = {0};
	int path_len = strlen(path);

	if (path_len < 8){
		strcpy(out_path, path);
		return;
	}

	to_lower(path_lowered, path);

	if (memcmp("flash0:/", path_lowered, 8) != 0){
		strcpy(out_path, path);
		return;
	}

	char possible_path[256] = {0};
	sprintf(possible_path, "%s/%s", THEME_DIR, &path[8]);
	if (!is_file(possible_path)){
		strcpy(out_path, path);
		return;
	}

	LOG_VERBOSE("%s: %s -> %s\n", __func__, path, possible_path);

	strcpy(out_path, possible_path);
	return;
}

SceUID custom_theme_dat_fd = -1;
char selected_ctf_path[256] = {0};

SceUID sceIoOpen_hooked(const char *file, int flags, SceMode mode){
	char overlay_path[256] = {0};
	overlay(file, overlay_path);

	SceUID ret = sceIoOpen(overlay_path, flags, mode);

	LOG_VERBOSE("%s: %s, 0x%x\n", __func__, file, ret);

	if (ret < 0){
		return ret;
	}

	if (strcmp(file, "flash1:/vsh/theme/custom_theme.dat") == 0 && flags == (PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC)){
		custom_theme_dat_fd = ret;
	}

	return ret;
}

int vsh_reboot_thread_func(SceSize args, void *argp){
	sceKernelDelayThread(3650000);
	LOG_VERBOSE("%s: rebooting vsh\n", __func__);
	sceKernelExitVSHVSH(NULL);
	return 0;
}

void schedule_vsh_reboot(){
	int thread = sceKernelCreateThread("reboot vsh", vsh_reboot_thread_func, 0x60, 1024 * 4, 0, NULL);
	sceKernelStartThread(thread, 0, NULL);
}

bool fd_copy(int write_fd, int read_fd, int size){
	int bytes_written = 0;
	while(bytes_written < size){
		static char dump_buf[8192];
		int rem = size - bytes_written;
		int to_read = sizeof(dump_buf) > rem ? rem : sizeof(dump_buf);
		int read_status = sceIoRead(read_fd, dump_buf, to_read);
		if (read_status != to_read){
			return false;
		}
		int write_status = sceIoWrite(write_fd, dump_buf, read_status);
		if (write_status != read_status){
			return false;
		}
		bytes_written += read_status;
	}
	return true;
}

bool copy_file(const char *to, const char *from){
	int read_fd = sceIoOpen(from, PSP_O_RDONLY, 0777);
	if (read_fd < 0){
		LOG("%s: failed opening %s for reading while copying\n", __func__, from);
		return false;
	}
	int write_fd = sceIoOpen(to, PSP_O_CREAT | PSP_O_TRUNC | PSP_O_WRONLY, 0777);
	if (write_fd < 0){
		LOG("%s: failed opening %s for writing while copying\n", __func__, to);
		sceIoClose(read_fd);
		return false;
	}
	int size = sceIoLseek(read_fd, 0, PSP_SEEK_END);
	if (size < 0){
		LOG("%s: bad size of from file\n", __func__);
		sceIoClose(write_fd);
		sceIoClose(read_fd);
		return false;
	}
	sceIoLseek(read_fd, 0, PSP_SEEK_SET);
	int ret = fd_copy(write_fd, read_fd, size);
	sceIoClose(write_fd);
	sceIoClose(read_fd);
	if (!ret){
		LOG("%s: failed copying from %s to %s\n", __func__, from, to);
	}
	return ret;
}

int sceIoClose_hooked(int fd){
	int ret = sceIoClose(fd);
	if (fd != -1 && fd == custom_theme_dat_fd){
		custom_theme_dat_fd = -1;
		if(copy_file(THEME_FILE, selected_ctf_path)){
			schedule_vsh_reboot();
		}
	}
	LOG_VERBOSE("%s: %d\n", __func__, fd);
	return ret;
}

int sceIoGetstat_hooked(const char *file, SceIoStat *stat){
	char overlay_path[256] = {0};
	overlay(file, overlay_path);
	LOG_VERBOSE("%s: %s\n", __func__, file);

	int ret = sceIoGetstat(overlay_path, stat);

	if (ret < 0){
		return ret;
	}

	char path_lowered[256] = {0};
	to_lower(path_lowered, file);
	int path_len = strlen(path_lowered);
	if (path_len > 4){
		if (memcmp(".ctf", &path_lowered[path_len - 4], 4) == 0){
			strcpy(selected_ctf_path, file);
			// fake the size
			int size_from_file = 0;
			int ctf_fd = sceIoOpen(file, PSP_O_RDONLY, 0777);
			if (ctf_fd < 0){
				LOG("%s: failed opening ctf while faking size\n", __func__);
				return ret;
			}
			sceIoLseek(ctf_fd, 0x1c, PSP_SEEK_SET);
			int read_status = sceIoRead(ctf_fd, &size_from_file, sizeof(size_from_file));
			if (read_status != sizeof(size_from_file)){
				LOG("%s: failed reading from ctf while faking size\n", __func__);
				sceIoClose(ctf_fd);
				return ret;
			}
			sceIoClose(ctf_fd);
			if (size_from_file > 0){
				stat->st_size = size_from_file;
			}else{
				stat->st_size = 0x00020000;
			}
			LOG_VERBOSE("%s: faking ctf size as %d\n", __func__, (int)stat->st_size);
		}
	}

	return ret;
}

bool rm_recursive(const char *file){
	SceIoStat file_stat;
	int get_stat_status = sceIoGetstat(file, &file_stat);
	if (get_stat_status < 0){
		// assume file not found
		return true;
	}

	if (!FIO_S_ISDIR(file_stat.st_mode)){
		int remove_status = sceIoRemove(file);
		if (remove_status < 0){
			LOG("%s: failed removing file %s\n", __func__, file);
			return false;
		}
		return true;
	}

	int dir_fd = sceIoDopen(file);
	if (dir_fd < 0){
		// uh, what
		LOG("%s: failed opening directory %s\n", __func__, file);
		return false;
	}
	SceIoDirent entry;
	while (sceIoDread(dir_fd, &entry) > 0){
		if (strcmp(entry.d_name, ".") == 0 || strcmp(entry.d_name, "..") == 0){
			continue;
		}
		char path_buf[1024] = {0};
		sprintf(path_buf, "%s/%s", file, entry.d_name);
		bool rm_status = rm_recursive(path_buf);
		if (!rm_status){
			sceIoDclose(dir_fd);
			return false;
		}
	}

	sceIoDclose(dir_fd);

	int dir_remove_status = sceIoRmdir(file);
	if (dir_remove_status < 0){
		LOG("%s: failed removing directory %s\n", __func__, file);
		return false;
	}

	return true;
}

int sceIoRemove_hooked(const char *file){
	int ret = sceIoRemove(file);

	if (strcmp("flash1:/vsh/theme/custom_theme.dat", file) == 0){
		sceIoRemove(THEME_FILE);
		rm_recursive(THEME_DIR);
		schedule_vsh_reboot();
	}

	LOG_VERBOSE("%s: %s, 0x%x\n", __func__, file, ret);
	return ret;
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
					LOG_VERBOSE("%s: hooking %s 's %s 0x%lx\n", __func__, module_name, library_name, target_nid);
					return true;
				}
			}
		}

		library = (SceLibraryStubTable *)((uint32_t)library + library->len * 4);
	}

	LOG_VERBOSE("%s: module %s did not import %s 0x%lx\n", __func__, module_name, library_name, target_nid);

	return false;

}

int sceKernelUtilsSha1BlockInit_(SceKernelUtilsSha1Context *ctx);
int sceKernelUtilsSha1BlockUpdate_(SceKernelUtilsSha1Context *ctx, u8 *data, u32 size);
int sceKernelUtilsSha1BlockResult_(SceKernelUtilsSha1Context *ctx, u8 *digest);

bool hash_file(const char *path, char *digest){
	SceKernelUtilsSha1Context ctx = {0};
	sceKernelUtilsSha1BlockInit_(&ctx);

	int read_fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
	if (read_fd < 0){
		return false;
	}

	static char hash_buf[8192] = {0};
	while(true){
		int read_status = sceIoRead(read_fd, hash_buf, sizeof(hash_buf));
		if (read_status < 0){
			sceIoClose(read_fd);
			return false;
		}
		if (read_status == 0){
			break;
		}

		sceKernelUtilsSha1BlockUpdate_(&ctx, (uint8_t *)hash_buf, read_status);
	}
	sceIoClose(read_fd);
	memset(digest, 0, 20);
	sceKernelUtilsSha1BlockResult_(&ctx, (uint8_t *)digest);

	LOG_VERBOSE("%s: %s: ", __func__, path);
	for(int i = 0;i < 20;i++){
		LOG_VERBOSE("%02x", (uint8_t)digest[i]);
	}
	LOG_VERBOSE("\n");

	return true;
}

bool read_file(const char *path, char *buf, int size){
	int read_fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
	if (read_fd < 0){
		return false;
	}

	int read_result = sceIoRead(read_fd, buf, size);
	if (read_result != size){
		sceIoClose(read_fd);
		return false;
	}

	sceIoClose(read_fd);
	return true;
}

int sceKernelGetCompiledSdkVersion_(void);
int sceKernelDevkitVersion_(void);

int get_firmware_version_from_file(){
	char buf[13] = {0};
	if (!read_file("flash0:/vsh/etc/version.txt", buf, 13)){
		LOG("%s: failed reading firmware version file\n", __func__);
		return 0;
	}

	#define CHECK(s, r) \
	if (memcmp(s, buf, 13) == 0){ \
		return r; \
	}

	CHECK("release:5.00:", 0x05000010);
	CHECK("release:5.02:", 0x05000210);
	CHECK("release:5.03:", 0x05000310);
	CHECK("release:5.50:", 0x05050010);
	CHECK("release:6.20:", 0x06020010);
	CHECK("release:6.31:", 0x06030110);
	CHECK("release:6.35:", 0x06030510);
	CHECK("release:6.37:", 0x06030710);
	CHECK("release:6.39:", 0x06030910);
	CHECK("release:6.60:", 0x06060010);
	CHECK("release:6.61:", 0x06060110);

	#undef CHECK

	return 0;
}

int get_firmware_version(){
	static int cache = -1;
	if (cache == -1){
		// there must be a better way right?
		//cache = sceKernelGetCompiledSdkVersion_();
		//cache = sceKernelDevkitVersion_();
		cache = get_firmware_version_from_file();
	}
	return cache;
}

void patch_syscon(){
	SceModule *mod = sceKernelFindModuleByName("sysconf_plugin_module");

	uint32_t offset = 0;
	int fw_version = get_firmware_version();
	switch(fw_version){
		case 0x05000010:
		case 0x05000210:
		case 0x05000310:
			offset = 0x00026050;
			break;
		case 0x05050010:
			offset = 0x000257B8;
			break;
		case 0x06020010:
			offset = 0x0002BDB4;
			break;
		case 0x06030110:
		case 0x06030510:
		case 0x06030710:
		case 0x06030810:
		case 0x06030910:
			offset = 0x0002C724;
			break;
		case 0x06060010:
		case 0x06060110:
			offset = 0x0002CB84;
			break;
	}

	if (offset == 0){
		LOG("%s: syscon patch not available for fw 0x%x\n", __func__, fw_version);
		return;
	}

	if (mod == NULL){
		LOG("%s: syscon not found\n", __func__);
		return;
	}

	uint32_t location = offset + (uint32_t)mod->text_addr;
	*(char *)location = 'C';
	LOG_VERBOSE("%s: syscon patched for fw 0x%x\n", __func__, fw_version);
}

SceUID sceKernelLoadModule_hooked(const char *path, int flags, SceKernelLMOption *option);
SceUID sceKernelLoadModuleVSH_hooked(const char *path, int flags, SceKernelLMOption *option);
void hook(char *mod_name){
	rewrite_import_table_by_nid(mod_name, "IoFileMgrForUser", 0x109F50BC, sceIoOpen_hooked);
	rewrite_import_table_by_nid(mod_name, "IoFileMgrForKernel", 0x109F50BC, sceIoOpen_hooked);
	rewrite_import_table_by_nid(mod_name, "IoFileMgrForUser", 0xACE946E8, sceIoGetstat_hooked);
	rewrite_import_table_by_nid(mod_name, "IoFileMgrForKernel", 0xACE946E8, sceIoGetstat_hooked);
	rewrite_import_table_by_nid(mod_name, "IoFileMgrForUser", 0x810C4BC3, sceIoClose_hooked);
	rewrite_import_table_by_nid(mod_name, "IoFileMgrForKernel", 0x810C4BC3, sceIoClose_hooked);
	rewrite_import_table_by_nid(mod_name, "IoFileMgrForUser", 0xF27A9C51, sceIoRemove_hooked);
	rewrite_import_table_by_nid(mod_name, "IoFileMgrForKernel", 0xF27A9C51, sceIoRemove_hooked);

	rewrite_import_table_by_nid(mod_name, "ModuleMgrForUser", 0x977DE386, sceKernelLoadModule_hooked);
	rewrite_import_table_by_nid(mod_name, "ModuleMgrForKernel", 0xD5DDAB1F, sceKernelLoadModuleVSH_hooked);
	rewrite_import_table_by_nid(mod_name, "ModuleMgrForKernel", 0xA4370E7C, sceKernelLoadModuleVSH_hooked);
	rewrite_import_table_by_nid(mod_name, "ModuleMgrForKernel", 0xCCDE84A8, sceKernelLoadModuleVSH_hooked);
	rewrite_import_table_by_nid(mod_name, "ModuleMgrForKernel", 0xFE586962, sceKernelLoadModuleVSH_hooked);
	rewrite_import_table_by_nid(mod_name, "ModuleMgrForKernel", 0x329C89DB, sceKernelLoadModuleVSH_hooked);
	rewrite_import_table_by_nid(mod_name, "ModuleMgrForKernel", 0x8909A807, sceKernelLoadModuleVSH_hooked);
	rewrite_import_table_by_nid(mod_name, "ModuleMgrForKernel", 0xBDFEEC4F, sceKernelLoadModuleVSH_hooked);
	if (strcmp(mod_name, "sysconf_plugin_module") == 0){
		patch_syscon();
	}
}

SceUID sceKernelLoadModule_hooked(const char *path, int flags, SceKernelLMOption *option){
	char overlay_path[256] = {0};
	overlay(path, overlay_path);
	int mod_id = sceKernelLoadModule(overlay_path, flags, option);
	if (mod_id >= 0){
		SceKernelModuleInfo mod_info = {0};
		mod_info.size = sizeof(mod_info);
		sceKernelQueryModuleInfo(mod_id, &mod_info);
		char mod_name[sizeof(mod_info.name) + 1] = {0};
		memcpy(mod_name, mod_info.name, sizeof(mod_info.name));
		LOG_VERBOSE("%s: hooking %s\n", __func__, mod_name);
		hook(mod_name);
	}
	return mod_id;
}

SceUID sceKernelLoadModuleVSH_hooked(const char *path, int flags, SceKernelLMOption *option){
	return sceKernelLoadModule_hooked(path, flags, option);
}

void initial_hook(){
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
		LOG_VERBOSE("%s: hooking %s\n", __func__, mod_name);
		hook(mod_name);
	}
}

void mkdir_recursive(const char *dir){
	char full_path[256] = {0};
	int path_len = strlen(dir);
	for (int i = 0;i < path_len;i++){
		full_path[i] = dir[i];
		if (dir[i] == '/'){
			sceIoMkdir(full_path, 0777);
		}
	}
	sceIoMkdir(full_path, 0777);
}

void dirname(const char *path, char *out){
	int path_len = strlen(path);
	int last_slash = 0;
	for (int i = 0;i < path_len;i++){
		out[i] = path[i];
		if (path[i] == '/'){
			last_slash = i;
		}
	}
	out[last_slash] = '\0';
	return;
}

struct replace_entry{
	char name[64];
	unsigned int start;
	unsigned int size;
} __attribute__((packed));

struct ctf_header{
	uint8_t unk[0x10];
	uint32_t magic;
	uint32_t ctf_sig;
	uint32_t header_size;
} __attribute__((packed));

struct patch_list_entry{
	const char *library_name;
	const char *patch_path;
};

struct patch_list_entry patch_list[] = {
	{
		.library_name = "scePaf_Module",
		.patch_path = "/vsh/module/paf.prx"
	},
	{
		.library_name = "sceVshCommonGui_Module",
		.patch_path = "/vsh/module/common_gui.prx"
	},
	{
		.library_name = "vsh_module",
		.patch_path = "/vsh/module/vshmain.prx"
	}
};

struct patch_header{
	uint32_t location;
	uint32_t size;
} __attribute__((packed));

bool extract_ctf(){
	int ctf_fd = sceIoOpen(THEME_FILE, PSP_O_RDONLY, 0777);
	if (ctf_fd < 0){
		LOG("%s: failed opening theme %s for reading, 0x%x\n", __func__, THEME_FILE, ctf_fd);
		return false;
	}

	static struct ctf_header header;
	static struct replace_entry entries[1024];

	int bytes_read = sceIoRead(ctf_fd, &header, sizeof(header));
	if (bytes_read != sizeof(header)){
		LOG("%s: bad ctf file that is smaller than header\n", __func__);
		sceIoClose(ctf_fd);
		return false;
	}
	// TODO maaaaybe actually check this
	LOG("%s: theme magic is 0x%lx, your firmware is maybe 0x%x, good luck :)\n", __func__, header.magic, get_firmware_version());

	if (header.header_size > ARRAY_SIZE(entries)){
		LOG("%s: bad number of entries in ctf file\n", __func__);
		sceIoClose(ctf_fd);
		return false;
	}

	int entries_bytes = header.header_size * sizeof(struct replace_entry);
	sceIoLseek(ctf_fd, entries_bytes * -1, PSP_SEEK_END);

	bytes_read = sceIoRead(ctf_fd, entries, entries_bytes);
	if (bytes_read != entries_bytes){
		LOG("%s: bad ctf file while reading replacement entries\n", __func__);
		sceIoClose(ctf_fd);
		return false;
	}

	// clean previous theme
	if (!rm_recursive(THEME_DIR)){
		sceIoClose(ctf_fd);
		return false;
	}

	for (int i = 0;i < header.header_size;i++){
		LOG_VERBOSE("%s: replace entry %s at 0x%x size %d\n", __func__, entries[i].name, entries[i].start, entries[i].size);
		char out_path[256] = {0};
		sprintf(out_path, "%s%s", THEME_DIR, entries[i].name);
		char out_dirname[256] = {0};
		dirname(out_path, out_dirname);
		mkdir_recursive(out_dirname);
		int out_file_fd = sceIoOpen(out_path, PSP_O_CREAT | PSP_O_TRUNC | PSP_O_WRONLY, 0777);
		if (out_file_fd < 0){
			LOG("%s: failed opening %s for writing\n", __func__, out_path);
			sceIoClose(ctf_fd);
			return false;
		}

		sceIoLseek(ctf_fd, entries[i].start, PSP_SEEK_SET);

		bool is_patch = false;
		for (int j = 0;j < ARRAY_SIZE(patch_list);j++){
			if (strcmp(entries[i].name, patch_list[j].patch_path) == 0){
				is_patch = true;
				// it is a patch, the size in the header means something else
				for (int k = 0;k < entries[i].size;k++){
					struct patch_header patch_header;
					int read_status = sceIoRead(ctf_fd, &patch_header, sizeof(patch_header));
					if (read_status != sizeof(patch_header)){
						LOG("%s: failed reading patch %s for writing\n", __func__, patch_list[j].patch_path);
						sceIoClose(ctf_fd);
						sceIoClose(out_file_fd);
						return false;
					}
					sceIoLseek(ctf_fd, sizeof(patch_header) * -1, PSP_SEEK_CUR);
					if (!fd_copy(out_file_fd, ctf_fd, sizeof(patch_header) + patch_header.size)){
						LOG("%s: failed copying patch %s\n", __func__, entries[i].name);
						sceIoClose(ctf_fd);
						sceIoClose(out_file_fd);
						return false;
					}
				}

				break;
			}
		}
		if (is_patch){
			sceIoClose(out_file_fd);
			continue;
		}

		if (!fd_copy(out_file_fd, ctf_fd, entries[i].size)){
			LOG("%s: failed copying file %s\n", __func__, entries[i].name);
			sceIoClose(ctf_fd);
			sceIoClose(out_file_fd);
			return false;
		}
		sceIoClose(out_file_fd);
	}
	sceIoClose(ctf_fd);

	return true;
}

bool write_file(const char *path, const char *buf, int size){
	int write_fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (write_fd < 0){
		return false;
	}

	int write_result = sceIoWrite(write_fd, buf, size);
	if (write_result != size){
		sceIoClose(write_fd);
		return false;
	}

	sceIoClose(write_fd);
	return true;
}

bool is_hash_same(){
	if(!is_file(EXTRACTED_HASH)){
		return false;
	}

	char existing_hash[20];
	if (!read_file(EXTRACTED_HASH, existing_hash, sizeof(existing_hash))){
		return false;
	}

	char cur_hash[20];
	if (!hash_file(THEME_FILE, cur_hash)){
		return false;
	}
	if (memcmp(cur_hash, existing_hash, 20) != 0){
		return false;
	}
	return true;
}

void apply_patches(){
	for (int i = 0;i < ARRAY_SIZE(patch_list);i++){
		char patch_path[256] = {0};
		sprintf(patch_path, "%s%s", THEME_DIR, patch_list[i].patch_path);
		if (is_file(patch_path)){
			SceModule *mod = sceKernelFindModuleByName(patch_list[i].library_name);
			if (mod == NULL){
				LOG("%s: %s not found, huh\n", __func__, patch_list[i].library_name);
				continue;
			}

			int patch_fd = sceIoOpen(patch_path, PSP_O_RDONLY, 0777);
			if (patch_fd < 0){
				LOG("%s: failed opening patch file %s\n", __func__, patch_path);
				continue;
			}

			struct patch_header patch_header;
			bool patch_failed = false;
			while(sceIoRead(patch_fd, &patch_header, sizeof(patch_header)) == sizeof(patch_header)){
				uint32_t location = patch_header.location + (uint32_t)mod->text_addr;
				int read_status = sceIoRead(patch_fd, (void *)location, patch_header.size);
				if (read_status != patch_header.size){
					LOG("%s: patching of %s failed\n", __func__, patch_list[i].library_name);
					patch_failed = true;
					break;
				}
			}

			sceIoClose(patch_fd);
			if (!patch_failed){
				LOG_VERBOSE("%s: patch applied to %s\n", __func__, patch_list[i].library_name);
			}
		}
	}
}

int module_start(SceSize args, void *argp){
	#if DEBUG_LOG
	sceIoRemove(LOG_PATH);
	#endif
	LOG("%s: module started\n", __func__);

	if (!is_file(THEME_FILE)){
		sceIoRemove(EXTRACTED_HASH);
	} else {
		if (!is_hash_same() || !is_dir(THEME_DIR)){
			LOG_VERBOSE("%s: hash changed, extracting ctf again\n", __func__);
			if (!extract_ctf()){
				rm_recursive(THEME_DIR);
			} else {
				char new_digest[20];
				if (!hash_file(THEME_FILE, new_digest)){
					LOG("%s: failed creating file hash for newly extracted ctf\n", __func__);
				} else {
					if (!write_file(EXTRACTED_HASH, new_digest, 20)){
						LOG("%s: failed writing hash for newly extracted ctf\n", __func__);
					}
				}
			}
		}
	}

	apply_patches();

	initial_hook();

	return 0;
}

int module_stop(SceSize args, void *argp){
	LOG("%s: attempting to stop this module, but unload is not really implemented...\n", __func__);
	return 0;
}
