#include "boot_storage.h"
#include <libs/fatfs/ff.h>
#include <fatfs_cfg.h>
#include <storage/sd.h>
#include <storage/emmc.h>
#include <utils/types.h>

#define DEV_INVALID 0xff

static FATFS boot_storage_fs;
static BYTE drive = -1;

static const char* drive_base_paths[] = {
	[DRIVE_SD]        = XSTR(DRIVE_SD)        ":",
	[DRIVE_BOOT1]     = XSTR(DRIVE_BOOT1)     ":",
	[DRIVE_BOOT1_1MB] = XSTR(DRIVE_BOOT1_1MB) ":",
	[DRIVE_EMMC]       = XSTR(DRIVE_EMMC)       ":",
};

static bool _is_eligible(){
	if(f_stat(".no_boot_storage", NULL) == FR_OK){
		return false;
	}
	return true;
} 

bool boot_storage_get_mounted(){
	return drive != DEV_INVALID;
}

bool boot_storage_get_initialized(){
	switch(drive){
	case DRIVE_BOOT1:
	case DRIVE_EMMC:
	case DRIVE_BOOT1_1MB:
		return emmc_get_initialized();
	case DRIVE_SD:
		return sd_get_card_initialized();
	}
	return false;
}

static bool _boot_storage_initialize(){
	switch(drive){
	case DRIVE_BOOT1:
	case DRIVE_EMMC:
	case DRIVE_BOOT1_1MB:
		return emmc_initialize(false);
	case DRIVE_SD:
		return sd_initialize(false);
	}
	return false;
}

static void _boot_storage_end(bool deinit){
	if(boot_storage_get_mounted()){
		if(drive == DRIVE_SD){
			sd_unmount();
		}else{
			f_mount(NULL, drive_base_paths[drive], false);
		}
		drive = DEV_INVALID;
	}
	if(deinit){
		if(drive == DRIVE_SD){
			sd_end();
		}else{
			emmc_end();
		}
	}
}

void boot_storage_unmount(){
	_boot_storage_end(false);
}

void boot_storage_end(){
	_boot_storage_end(true);
}

static bool _boot_storage_mount(){
	// may want to check sd card first and prioritize it

	FRESULT res;

	if(!emmc_get_initialized() && !emmc_initialize(false)){
		goto emmc_init_fail;
	}

	static const BYTE emmc_drives[] = {DRIVE_BOOT1_1MB, DRIVE_BOOT1, DRIVE_EMMC}; 

	for(BYTE i = 0; i < ARRAY_SIZE(emmc_drives); i++){
		res = f_mount(&boot_storage_fs, drive_base_paths[i], true);
		if(res == FR_OK){
			res = f_chdrive(drive_base_paths[i]);
			if(res == FR_OK && _is_eligible()){
				drive = i;
				break;
			}else{
				f_mount(NULL, drive_base_paths[i],false);
				res = FR_INVALID_DRIVE;
			}
		}
	}

	if(res != FR_OK){
		emmc_end();
	}

	if(res == FR_OK){
		return true;
	}

emmc_init_fail:
	if(!sd_initialize(false)){
		goto out;
	}

	if(!sd_mount()){
		sd_end();
		goto out;
	}

	res = f_chdrive(drive_base_paths[DRIVE_SD]);

	if(res == FR_OK && _is_eligible()){
		drive = DRIVE_SD;
		return true;
	}

	sd_end();

out:
	return false;
}

bool boot_storage_mount(){
	bool mounted = boot_storage_get_mounted();
	bool initialized = boot_storage_get_initialized();
	if(mounted && initialized){
		return true;
	}

	if(!mounted){
		// not mounted. mounting will also initialize.
		return _boot_storage_mount();
	}

	if(!initialized){
		return _boot_storage_initialize();
	}

	return true;
}