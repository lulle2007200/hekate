#include "fe_emusd_tools.h"
#include "../storage/sfd.h"
#include <sec/se.h>
#include <storage/boot_storage.h>
#include <libs/fatfs/ff.h>
#include <storage/emmc.h>
#include <storage/mbr_gpt.h>
#include <storage/sdmmc.h>
#include <utils/list.h>
#include <utils/ini.h>
#include <string.h>
#include <stdlib.h>

void load_emusd_cfg(emusd_cfg_t *emu_info){
	memset(emu_info, 0, sizeof(emusd_cfg_t));

	// Parse emuMMC configuration.
	LIST_INIT(ini_sections);
	if (!ini_parse(&ini_sections, "emuSD/emusd.ini", false))
		return;

	LIST_FOREACH_ENTRY(ini_sec_t, ini_sec, &ini_sections, link)
	{
		if (!strcmp(ini_sec->name, "emusd"))
		{
			LIST_FOREACH_ENTRY(ini_kv_t, kv, &ini_sec->kvs, link)
			{
				if (!strcmp("enabled",     kv->key))
					emu_info->enabled = atoi(kv->val);
				else if(!strcmp("sector", kv->key))
					emu_info->sector = strtol(kv->val, NULL, 16);
				/*else (!strcmp("id",     kv->key))
					emu_info->id     = strtol(kv->val, NULL, 16);*/
			}

			break;
		}
	}

	ini_free(&ini_sections);
}

void save_emusd_cfg(u32 part_idx, u32 sector_start){
	boot_storage_mount();

	char lbuf[16];
	FIL fp;

	f_mkdir("emuSD");

	if (f_open(&fp, "emuSD/emusd.ini", FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
		return;

	// Add config entry.
	f_puts("[emusd]\nenabled=", &fp);
	if(sector_start){
		f_puts("4", &fp);
	}else{
		f_puts("0", &fp);
	}

	if(!sector_start){
		f_puts("\nsector=0x0", &fp);
	}else{
		f_puts("\nsector=0x", &fp);
		itoa(sector_start, lbuf, 16);
		f_puts(lbuf, &fp);
	}
	f_puts("\n", &fp);

	f_close(&fp);
}

int create_emusd(int part_idx, u32 sector_mbr, u32 sector_start, u32 sector_size, const char *name){
	sfd_init(&emmc_storage, sector_start, sector_size);

	u8 *buf = malloc(SZ_4M);
	FIL f;
	u32 cluster_size = 65536;

	u32 mkfs_error = f_mkfs("sfd:", FM_FAT32 | FM_SFD, cluster_size, buf, SZ_4M);

	while(mkfs_error != FR_OK && cluster_size > 4096){
		cluster_size /= 2;
		mkfs_error = f_mkfs("sfd:", FM_FAT32 | FM_SFD, cluster_size, buf, SZ_4M);
	}

	if(mkfs_error == FR_OK){
		FATFS fs;
		mkfs_error = f_mount(&fs, "sfd:", 1);

		if(mkfs_error == FR_OK){
			mkfs_error = f_open(&f, "sfd:.no_boot_storage", FA_CREATE_ALWAYS | FA_WRITE);
			f_close(&f);
			
			if(mkfs_error == FR_OK){
				char label[0x30];
				strcpy(label, "sfd:");
				strcat(label, name);
				mkfs_error = f_setlabel(label);

				if(mkfs_error == FR_OK){
					u8 random_number[16];
					mbr_t mbr = {0};
					se_gen_prng128(random_number);
					memcpy(&mbr.signature, random_number, 4);
					mbr.boot_signature = 0xaa55;
					mbr.partitions[0].start_sct = sector_start - sector_mbr;
					mbr.partitions[0].size_sct = sector_size;
					mbr.partitions[0].type = 0x0c;
				
					mkfs_error = sdmmc_storage_write(&emmc_storage, sector_mbr, 1, &mbr) ? FR_OK : FR_DISK_ERR;
				}
			}
		}

		f_mount(NULL, "sfd:", 0);
	}

	sfd_end();

	return mkfs_error;
}