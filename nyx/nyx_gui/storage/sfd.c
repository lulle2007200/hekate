#include "sfd.h"
#include <storage/emmc.h>
#include <storage/sdmmc.h>
#include <libs/fatfs/diskio.h>

static sdmmc_storage_t *_storage;
static u32 _offset;
static u32 _size;

static void ensure_partition(){
	if(_storage == &emmc_storage){
		emmc_set_partition(EMMC_GPP);
	}
}

int sfd_read(u32 sector, u32 count, void *buff){
	if(sector + count > _size){
		return 0;
	}
	ensure_partition();
	return sdmmc_storage_read(_storage, sector + _offset, count, buff);
}

int sfd_write(u32 sector, u32 count, void *buff){
	if(sector + count > _size){
		return 0;
	}
	ensure_partition();
	return sdmmc_storage_write(_storage, sector + _offset, count, buff);
}

bool sfd_init(sdmmc_storage_t *storage, u32 offset, u32 size){
	_storage = storage;
	_offset = offset;
	_size = size;
	disk_set_info(DRIVE_SFD, SET_SECTOR_COUNT, &size);
	return true;
}

void sfd_end(){
	_storage = NULL;
	_offset = 0;
	_size = 0;
	u32 size = 0;
	disk_set_info(DRIVE_SFD, SET_SECTOR_COUNT, &size);
}