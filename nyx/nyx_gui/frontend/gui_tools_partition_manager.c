/*
 * Copyright (c) 2019-2024 CTCaer
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <fatfs_cfg.h>
#include <libs/fatfs/ff.h>
#include <libs/lvgl/lv_core/lv_obj.h>
#include <libs/lvgl/lv_misc/lv_area.h>
#include <libs/lvgl/lv_objx/lv_bar.h>
#include <libs/lvgl/lv_objx/lv_label.h>
#include <libs/lvgl/lv_objx/lv_slider.h>
#include <mem/heap.h>
#include <memory_map.h>
#include <stdlib.h>

#include <bdk.h>

#include "gui.h"
#include "gui_tools.h"
#include "gui_tools_partition_manager.h"
#include <libs/fatfs/diskio.h>
#include <libs/lvgl/lvgl.h>
#include <storage/boot_storage.h>
#include <storage/emmc.h>
#include <storage/mbr_gpt.h>
#include <storage/sd.h>
#include <storage/sdmmc.h>
#include <string.h>
#include <utils/btn.h>
#include <utils/sprintf.h>
#include <utils/types.h>
#include "../storage/sfd.h"

#define AU_ALIGN_SECTORS 0x8000 // 16MB.
#define AU_ALIGN_BYTES   (AU_ALIGN_SECTORS * SD_BLOCKSIZE)

#define SECTORS_PER_GB   0x200000

#define HOS_MIN_SIZE_MB        2048
#define ANDROID_SYSTEM_SIZE_MB 6144 // 6 GB. Fits both Legacy (4912MB) and Dynamic (6144MB) partition schemes.
#define ANDROID_SYSTEM_SIZE_LEGACY_MB 4912

#define ENABLE_DUAL_ANDROID 1

extern volatile boot_cfg_t *b_cfg;
extern volatile nyx_storage_t *nyx_str;


typedef struct _partition_ctxt_t
{
	u32 total_sct;
	u32 alignment;
	int backup_possible;
	bool skip_backup;
	u8 drive;

	u32 hos_os_og_size;

	s32 hos_size;
	u32 emu_size;
	u32 l4t_size;
	u32 and_size;
	u32 hos_os_size;
	u32 emu_sd_size;

	u32 hos_sys_size_mb;
	s32 hos_min_size_mb;

	bool emu_double;
	bool and_double;
	bool emu_sd_double;
	bool emmc_is_64gb;
	bool auto_assign_free_storage;

	bool and_dynamic;

	mbr_t mbr_old;

	lv_obj_t *bar_hos;
	lv_obj_t *bar_emu;
	lv_obj_t *bar_l4t;
	lv_obj_t *bar_and;
	lv_obj_t *bar_hos_os;
	lv_obj_t *bar_remaining;
	lv_obj_t *bar_emu_sd;

	lv_obj_t *sep_emu;
	lv_obj_t *sep_l4t;
	lv_obj_t *sep_and;
	lv_obj_t *sep_hos;
	lv_obj_t *sep_hos_os;
	lv_obj_t *sep_emu_sd;

	lv_obj_t *slider_bar_hos;
	lv_obj_t *slider_emu;
	lv_obj_t *slider_l4t;
	lv_obj_t *slider_and;
	lv_obj_t *slider_hos_os;
	lv_obj_t *slider_emu_sd;

	lv_obj_t *lbl_hos;
	lv_obj_t *lbl_emu;
	lv_obj_t *lbl_l4t;
	lv_obj_t *lbl_and;
	lv_obj_t *lbl_hos_os;
	lv_obj_t *lbl_emu_sd;
} partition_ctxt_t;

typedef struct _l4t_flasher_ctxt_t
{
	u32 offset_sct;
	u32 image_size_sct;
} l4t_flasher_ctxt_t;

partition_ctxt_t part_info;
l4t_flasher_ctxt_t l4t_flash_ctxt;

lv_obj_t *btn_flash_l4t;
lv_obj_t *btn_flash_android;

static void _wctombs(const u16 *src, char *dest, u32 len_max){
	const u16 *cur = src;
	do{
		*dest++ = *cur & 0xff;
		len_max--;
	}while(*cur++ && len_max);
}

static void _ctowcs(const char *src, u16 *dest, u32 len_max){
	const char *cur = src;
	do{
		*dest++ = *cur;
		len_max--; 
	}while(*cur++ && len_max);
}

static bool _has_gpt(const mbr_t *mbr){
	for(u32 i = 0; i < 4; i++){
		if(mbr->partitions[i].type == 0xee){
			return true;
		}
	}
	return false;
}

int _copy_file(const char *src, const char *dst, const char *path)
{
	FIL fp_src;
	FIL fp_dst;
	int res;

	// Open file for reading.
	f_chdrive(src);
	res = f_open(&fp_src, path, FA_READ);
	if (res != FR_OK)
		return res;

	u32 file_bytes_left = f_size(&fp_src);

	// Open file for writing.
	f_chdrive(dst);
	f_open(&fp_dst, path, FA_CREATE_ALWAYS | FA_WRITE);
	f_lseek(&fp_dst, f_size(&fp_src));
	f_lseek(&fp_dst, 0);

	while (file_bytes_left)
	{
		u32 chunk_size = MIN(file_bytes_left, SZ_4M); // 4MB chunks.
		file_bytes_left -= chunk_size;

		// Copy file to buffer.
		f_read(&fp_src, (void *)SDXC_BUF_ALIGNED, chunk_size, NULL);

		// Write file to disk.
		f_write(&fp_dst, (void *)SDXC_BUF_ALIGNED, chunk_size, NULL);
	}

	f_close(&fp_dst);
	f_chdrive(src);
	f_close(&fp_src);

	return FR_OK;
}

static int _stat_and_copy_files(const char *src, const char *dst, char *path, u32 *total_files, u32 *total_size, lv_obj_t **labels)
{
	FRESULT res;
	FIL fp_src;
	FIL fp_dst;
	DIR dir;
	u32 dirLength = 0;
	static FILINFO fno;

	f_chdrive(src);

	// Open directory.
	res = f_opendir(&dir, path);
	if (res != FR_OK)
		return res;

	if (labels)
		lv_label_set_text(labels[0], path);

	dirLength = strlen(path);

	// Hard limit path to 1024 characters. Do not result to error.
	if (dirLength > 1024)
		goto out;

	for (;;)
	{
		// Clear file path.
		path[dirLength] = 0;

		// Read a directory item.
		res = f_readdir(&dir, &fno);

		// Break on error or end of dir.
		if (res != FR_OK || fno.fname[0] == 0)
			break;

		// Set new directory or file.
		memcpy(&path[dirLength], "/", 1);
		strcpy(&path[dirLength + 1], fno.fname);

		if (labels)
		{
			lv_label_set_text(labels[1], fno.fname);
			manual_system_maintenance(true);
		}

		// Copy file to destination disk.
		if (!(fno.fattrib & AM_DIR))
		{
			u32 file_size = fno.fsize > RAMDISK_CLUSTER_SZ ? fno.fsize : RAMDISK_CLUSTER_SZ; // Ramdisk cluster size.

			// Check for overflow.
			if ((file_size + *total_size) < *total_size)
			{
				// Set size to > 1GB, skip next folders and return.
				*total_size = SZ_2G;
				res = -1;
				break;
			}

			*total_size += file_size;
			*total_files += 1;

			if (dst)
			{
				u32 file_bytes_left = fno.fsize;

				// Open file for writing.
				f_chdrive(dst);
				f_open(&fp_dst, path, FA_CREATE_ALWAYS | FA_WRITE);
				f_lseek(&fp_dst, fno.fsize);
				f_lseek(&fp_dst, 0);

				// Open file for reading.
				f_chdrive(src);
				f_open(&fp_src, path, FA_READ);

				while (file_bytes_left)
				{
					u32 chunk_size = MIN(file_bytes_left, SZ_4M); // 4MB chunks.
					file_bytes_left -= chunk_size;

					// Copy file to buffer.
					f_read(&fp_src, (void *)SDXC_BUF_ALIGNED, chunk_size, NULL);
					manual_system_maintenance(true);

					// Write file to disk.
					f_write(&fp_dst, (void *)SDXC_BUF_ALIGNED, chunk_size, NULL);
				}

				// Finalize copied file.
				f_close(&fp_dst);
				f_chdrive(dst);
				f_chmod(path, fno.fattrib, 0xFF);

				f_chdrive(src);
				f_close(&fp_src);
			}

			// If total is > 1GB exit.
			if (*total_size > (RAM_DISK_SZ - SZ_16M)) // 0x2400000.
			{
				// Skip next folders and return.
				res = -1;
				break;
			}
		}
		else // It's a directory.
		{
			if (!memcmp("System Volume Information", fno.fname, 25))
				continue;

			// Create folder to destination.
			if (dst)
			{
				f_chdrive(dst);
				f_mkdir(path);
				f_chmod(path, fno.fattrib, 0xFF);
			}

			// Enter the directory.
			res = _stat_and_copy_files(src, dst, path, total_files, total_size, labels);
			if (res != FR_OK)
				break;

			if (labels)
			{
				// Clear folder path.
				path[dirLength] = 0;
				lv_label_set_text(labels[0], path);
			}
		}
	}

out:
	f_closedir(&dir);

	return res;
}

static void _create_gpt_partition(gpt_t *gpt, u32 *gpt_idx, u32 *curr_part_lba, u32 size_lba, bool align, const char *name, const u8 type_guid[16], const u8 guid[16], bool clear){
	memcpy(gpt->entries[*gpt_idx].type_guid, type_guid, 16);

	if(!guid){
		u8 random_number[16];
		se_gen_prng128(random_number);
		memcpy(gpt->entries[*gpt_idx].part_guid, random_number, 16);
	}else{
		memcpy(gpt->entries[*gpt_idx].part_guid, guid, 16);
	}

	if(align){
		(*curr_part_lba) = ALIGN(*curr_part_lba, AU_ALIGN_SECTORS);
	}

	gpt->entries[*gpt_idx].lba_start = *curr_part_lba;
	gpt->entries[*gpt_idx].lba_end = *curr_part_lba + size_lba - 1;

	_ctowcs(name, gpt->entries[*gpt_idx].name, 36);

	if(clear){
		sdmmc_storage_t *storage = part_info.drive == DRIVE_SD ? &sd_storage : &emmc_storage;
		sdmmc_storage_write(storage, *curr_part_lba, 0x800, (void *)SDMMC_UPPER_BUFFER);
	}

	(*curr_part_lba) += size_lba;

	(*gpt_idx)++;
}

static void _make_part_name(char *buf, const char* base_name, u32 idx){
	if(idx > 1){
		s_printf(buf, "%s%d", base_name, idx);
	}else{
		strcpy(buf, base_name);
	}
}

static s32 _get_gpt_part_by_name(gpt_t *gpt, const char* name, s32 prev){
	u16 wc_name[36];
	_ctowcs(name, wc_name, 36);
	for(s32 i = prev++; i < (s32)gpt->header.num_part_ents && i < 128; i++){
		if(!memcmp(wc_name, gpt->entries[i].name, strlen(name) * 2)){
			return i;
		}
	}
	return -1;
}

static void _prepare_and_flash_mbr_gpt()
{
	sdmmc_storage_t *storage = part_info.drive == DRIVE_SD ? &sd_storage : &emmc_storage;

	mbr_t mbr;
	u8 random_number[16];

	memset((void *)SDMMC_UPPER_BUFFER, 0, AU_ALIGN_BYTES);

	s32 l4t_idx    = -1;
	s32 emu_idx[2] = {-1, -1};
	s32 hos_idx    = -1;

	// Create new GPT
	gpt_t *gpt = zalloc(sizeof(*gpt));
	memcpy(&gpt->header.signature, "EFI PART", 8);
	gpt->header.revision = 0x10000;
	gpt->header.size = 92;
	gpt->header.my_lba = 1;
	gpt->header.alt_lba = storage->sec_cnt - 1;
	gpt->header.first_use_lba = (sizeof(mbr_t) + sizeof(gpt_t)) >> 9;
	gpt->header.last_use_lba = storage->sec_cnt - 0x800 - 1;
	gpt->header.part_ent_lba = 2;
	gpt->header.part_ent_size = 128;
	se_gen_prng128(random_number);
	memcpy(gpt->header.disk_guid, random_number, 10);
	memcpy(gpt->header.disk_guid + 10, "NYXGPT", 6);

	u32 gpt_idx = 0;
	u32 gpt_next_lba = AU_ALIGN_SECTORS;
	if(part_info.hos_os_size){
		gpt_t *old_gpt = zalloc(sizeof(*old_gpt));
		sdmmc_storage_read(storage, 1, sizeof(*gpt) / 0x200, gpt);

		// Used by HOS for backup of first 3 partition entries
		memcpy(gpt->header.res2, old_gpt->header.res2, sizeof(gpt->header.res2));

		// user original disk guid
		memcpy(gpt->header.disk_guid, old_gpt->header.disk_guid, sizeof(gpt->header.disk_guid));

		// Copy HOS OS partition entries
		for(u32 i = 0; i < 11; i++){
			gpt->entries[i] = old_gpt->entries[i];
		}

		// Update HOS USER size
		gpt->entries[10].lba_end = gpt->entries[10].lba_start + ((part_info.hos_os_size - part_info.hos_sys_size_mb) << 11) - 1;
		gpt_next_lba = ALIGN(gpt->entries[10].lba_end + 1, AU_ALIGN_SECTORS);

		gpt_idx = 11;

		free(old_gpt);
	}

	static const u8 basic_part_guid[]        = { 0xA2, 0xA0, 0xD0, 0xEB,  0xE5, 0xB9,  0x33, 0x44,  0x87, 0xC0,  0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 };
	static const u8 linux_part_guid[]        = { 0xAF, 0x3D, 0xC6, 0x0F,  0x83, 0x84,  0x72, 0x47,  0x8E, 0x79,  0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 };
	static const u8 emu_part_guid[]          = { 0x00, 0x7E, 0xCA, 0x11,  0x00, 0x00,  0x00, 0x00,  0x00, 0x00,  'e', 'm', 'u', 'M', 'M', 'C' };
	// static const u8 emu_sd_part_guid[]       = { 0x00, 0x7E, 0xCA, 0x11,  0x00, 0x00,  0x00, 0x00,  0x00, 0x00,  0x00, 'e', 'm', 'u', 'S', 'D'};
	static const u8 emu_sd_mbr_part_guid[]   = { 0x00, 0x7E, 0xCA, 0x11,  0x00, 0x00,  0x00, 0x00, 'e', 'm', 'u', 'S', 'D', 'M', 'B', 'R'};

	if(part_info.hos_size){
		hos_idx = gpt_idx;
		_create_gpt_partition(gpt, &gpt_idx, &gpt_next_lba, part_info.hos_size << 11, true, "hos_data", basic_part_guid, NULL, true);
		// Clear non-standard Windows MBR attributes. bit4: Read only, bit5: Shadow copy, bit6: Hidden, bit7: No drive letter.
		gpt->entries[0].part_guid[7] = 0;
	}

	if(part_info.l4t_size){
		l4t_idx = gpt_idx;
		_create_gpt_partition(gpt, &gpt_idx, &gpt_next_lba, part_info.l4t_size << 11, true, "l4t", linux_part_guid, NULL, true);
	}

	if(part_info.and_size){
		u32 and_system_size;
		if(part_info.and_dynamic){
			and_system_size = ANDROID_SYSTEM_SIZE_MB;
		}else{
			and_system_size = ANDROID_SYSTEM_SIZE_LEGACY_MB;
		}

		u32 user_size;
		if(part_info.and_double){
			user_size = (part_info.and_size / 2) - and_system_size;
		}else{
			user_size = part_info.and_size - and_system_size;
		}

		for(u32 i = 1; i <= (part_info.and_double ? 2 : 1); i++){
			char part_name[36];
			if(part_info.and_dynamic){
				// Android Kernel, 64MB
				_make_part_name(part_name, "boot", i);
				_create_gpt_partition(gpt, &gpt_idx, &gpt_next_lba, 0x20000, false, part_name, linux_part_guid, NULL, true);

				// Android Recovery, 64MB
				_make_part_name(part_name, "recovery", i);
				_create_gpt_partition(gpt, &gpt_idx, &gpt_next_lba, 0x20000, false, part_name, linux_part_guid, NULL, true);

				// Android DTB, 1MB
				_make_part_name(part_name, "dtb", i);
				_create_gpt_partition(gpt, &gpt_idx, &gpt_next_lba, 0x800, false, part_name, linux_part_guid, NULL, true);

				// Android Misc, 3MB
				_make_part_name(part_name, "misc", i);
				_create_gpt_partition(gpt, &gpt_idx, &gpt_next_lba, 0x1800, false, part_name, linux_part_guid, NULL, true);

				// Android Cache, 60MB
				_make_part_name(part_name, "cache", i);
				_create_gpt_partition(gpt, &gpt_idx, &gpt_next_lba, 0x1E000, false, part_name, linux_part_guid, NULL, true);

				// Android Dynamic, 5922MB
				_make_part_name(part_name, "super", i);
				_create_gpt_partition(gpt, &gpt_idx, &gpt_next_lba, 0xB91000, true, part_name, linux_part_guid, NULL, true);

				// Android Userdata
				_make_part_name(part_name, "userdata", i);
				_create_gpt_partition(gpt, &gpt_idx, &gpt_next_lba, user_size << 11, true, part_name, linux_part_guid, NULL, true);
			}else{
				// Android Vendor, 1GB
				_make_part_name(part_name, "vendor", i);
				_create_gpt_partition(gpt, &gpt_idx, &gpt_next_lba, 0x200000, false, part_name, linux_part_guid, NULL, true);

				// Android System, 3GB
				_make_part_name(part_name, "APP", i);
				_create_gpt_partition(gpt, &gpt_idx, &gpt_next_lba, 0x600000, false, part_name, linux_part_guid, NULL, true);

				// Android Kernel, 32MB
				_make_part_name(part_name, "LNX", i);
				_create_gpt_partition(gpt, &gpt_idx, &gpt_next_lba, 0x10000, false, part_name, linux_part_guid, NULL, true);

				// Android Recovery, 64MB
				_make_part_name(part_name, "SOS", i);
				_create_gpt_partition(gpt, &gpt_idx, &gpt_next_lba, 0x20000, false, part_name, linux_part_guid, NULL, true);

				// Android DTB, 1MB
				_make_part_name(part_name, "DTB", i);
				_create_gpt_partition(gpt, &gpt_idx, &gpt_next_lba, 0x800, false, part_name, linux_part_guid, NULL, true);

				// Android encrypted metadata partition, 16MB to ensure alignment of following partitions.
				// If more, tiny partitions must be added, split off from MDA 
				_make_part_name(part_name, "MDA", i);
				// clear out entire partition
				sdmmc_storage_write(storage, gpt_next_lba, 0x8000, (void*)SDMMC_UPPER_BUFFER);
				_create_gpt_partition(gpt, &gpt_idx, &gpt_next_lba, 0x8000, false, part_name, linux_part_guid, NULL, true);

				// Android Cache, 700MB
				_make_part_name(part_name, "CAC", i);
				_create_gpt_partition(gpt, &gpt_idx, &gpt_next_lba, 0x15E000, false, part_name, linux_part_guid, NULL, true);

				// Android Misc, 3MB
				_make_part_name(part_name, "MSC", i);
				_create_gpt_partition(gpt, &gpt_idx, &gpt_next_lba, 0x1800, false, part_name, linux_part_guid, NULL, true);

				// Android Userdata
				_make_part_name(part_name, "UDA", i);
				_create_gpt_partition(gpt, &gpt_idx, &gpt_next_lba, user_size << 11, true, part_name, linux_part_guid, NULL, true);
			}
		}
	}

	if(part_info.emu_size){
		u32 emu_size;
		if(part_info.emu_double){
			emu_size = part_info.emu_size / 2;
		}else{
			emu_size = part_info.emu_size;
		}

		char part_name[36];
		for(u32 i = 1; i <= (part_info.emu_double ? 2 : 1); i++){
			emu_idx[i - 1] = gpt_idx;
			_make_part_name(part_name, "emummc", i);
			_create_gpt_partition(gpt, &gpt_idx, &gpt_next_lba, emu_size << 11, true, part_name, emu_part_guid, NULL, true);
		}
	}

	if(part_info.emu_sd_size){
		u32 emu_sd_size;
		if(part_info.emu_sd_double){
			emu_sd_size = part_info.emu_sd_size / 2;
		}else{
			emu_sd_size = part_info.emu_sd_size;
		}

		char part_name[36];
		for(u32 i = 1; i <= (part_info.emu_sd_double ? 2 : 1); i++){
			// split up emu sd partition into two
			// one partition that only covers the mbr
			// one partition that corresponds the actual fat32 partition
			// this way it can be accessed normally
			_make_part_name(part_name, "emusd_mbr", i);
			_create_gpt_partition(gpt, &gpt_idx, &gpt_next_lba, 1, true, part_name, emu_sd_mbr_part_guid, NULL, false);
			_make_part_name(part_name, "emusd", i);
			_create_gpt_partition(gpt, &gpt_idx, &gpt_next_lba, emu_sd_size << 11, true, part_name, basic_part_guid, NULL, true);
			// clear windows hidden attributes
			gpt->entries[gpt_idx - 1].part_guid[7] = 0;
		}
	}

	gpt->header.num_part_ents = gpt_idx;
	gpt->header.part_ents_crc32 = crc32_calc(0, (const u8 *)gpt->entries, sizeof(gpt_entry_t) * gpt->header.num_part_ents);
	gpt->header.crc32 = 0; // Set to 0 for calculation.
	gpt->header.crc32 = crc32_calc(0, (const u8 *)&gpt->header, gpt->header.size);

	gpt_header_t gpt_backup_header = {0};
	memcpy(&gpt_backup_header, &gpt->header, sizeof(gpt_backup_header));
	gpt_backup_header.my_lba = sd_storage.sec_cnt - 1;
	gpt_backup_header.alt_lba = 1;
	gpt_backup_header.part_ent_lba = sd_storage.sec_cnt - 33;
	gpt_backup_header.crc32 = 0; // Set to 0 for calculation.
	gpt_backup_header.crc32 = crc32_calc(0, (const u8 *)&gpt_backup_header, gpt_backup_header.size);

	bool need_gpt = gpt_idx > 4 || part_info.drive == DRIVE_EMMC;

	// Create new MBR
	if(part_info.drive == DRIVE_SD){
		// on sd, add hos_data, l4t and emummc partitions to mbr, if possible
		memset(&mbr, 0, sizeof(mbr));
		se_gen_prng128(random_number);
		memcpy(&mbr.signature, random_number, 4);

		mbr.boot_signature = 0xaa55;

		u32 mbr_idx = 0;

		if(hos_idx != -1){
			mbr.partitions[hos_idx].type      = 0x0c; // fat32
			mbr.partitions[hos_idx].start_sct = gpt->entries[hos_idx].lba_start;
			mbr.partitions[hos_idx].size_sct  = gpt->entries[hos_idx].lba_end - gpt->entries[hos_idx].lba_start + 1;
			mbr_idx++;
		}

		if(!need_gpt && l4t_idx != -1){
			mbr.partitions[l4t_idx].type      = 0x83; // linux partition
			mbr.partitions[l4t_idx].start_sct = gpt->entries[l4t_idx].lba_start;
			mbr.partitions[l4t_idx].size_sct  = gpt->entries[l4t_idx].lba_end - gpt->entries[l4t_idx].lba_start + 1;
			mbr_idx++;
		}

		for(u32 i = 0; i < 2; i++){
			if(emu_idx[i] != -1){
				mbr.partitions[mbr_idx].type      = 0xe0; // emummc partition
				mbr.partitions[mbr_idx].start_sct = gpt->entries[emu_idx[i]].lba_start;
				mbr.partitions[mbr_idx].size_sct  = gpt->entries[emu_idx[i]].lba_end - gpt->entries[emu_idx[i]].lba_start + 1;
				mbr_idx++;
			}
		}

		if(need_gpt){
			mbr.partitions[mbr_idx].type = 0xee;
			mbr.partitions[mbr_idx].start_sct = 1;
			mbr.partitions[mbr_idx].size_sct = 0xffffffff;
		}

	}else{
		// For eMMC, only gpt protective partition spanning entire disk
		memset(&mbr, 0, sizeof(mbr));
		mbr.partitions[0].start_sct_chs.sector = 0x02;
		mbr.partitions[0].end_sct_chs.sector   = 0xff;
		mbr.partitions[0].end_sct_chs.cylinder = 0xff;
		mbr.partitions[0].end_sct_chs.head     = 0xff;
		mbr.partitions[0].type                 = 0xee;
		mbr.partitions[0].start_sct            = 0x1;
		mbr.partitions[0].size_sct             = 0xffffffff;
		mbr.boot_signature = 0xaa55;
	}


	if(!part_info.hos_os_size){
		// only clear first 16mb if not keeping hos
		sdmmc_storage_write(storage, 0, 0x800, (void*)SDMMC_UPPER_BUFFER);
	}

	// write mbr
	sdmmc_storage_write(storage, 0, 1, &mbr);

	if(need_gpt){
		// write primary gpt
		sdmmc_storage_write(storage, 1, sizeof(*gpt) / 0x200, gpt);
		// write backup gpt entries
		sdmmc_storage_write(storage, gpt_backup_header.part_ent_lba, (sizeof(gpt_entry_t) * 128) / 0x200, gpt->entries);
		// write backup gpt header
		sdmmc_storage_write(storage, gpt_backup_header.my_lba, sizeof(gpt_backup_header) / 0x200, &gpt_backup_header);
	}

	free(gpt);
}

static lv_res_t _action_part_manager_ums_sd(lv_obj_t *btn)
{
	action_ums_sd(btn);

	// Close and reopen partition manager.
	lv_action_t close_btn_action = lv_btn_get_action(close_btn, LV_BTN_ACTION_CLICK);
	close_btn_action(close_btn);
	lv_obj_del(ums_mbox);
	create_window_partition_manager(NULL, DRIVE_SD);

	return LV_RES_INV;
}

static lv_res_t _action_part_manager_ums_emmc(lv_obj_t *btn){
	action_ums_emmc_gpp(btn);

	// Close and reopen partition manager.
	lv_action_t close_btn_action = lv_btn_get_action(close_btn, LV_BTN_ACTION_CLICK);
	close_btn_action(close_btn);
	lv_obj_del(ums_mbox);
	create_window_partition_manager(NULL, DRIVE_EMMC);

	return LV_RES_INV;
}

static lv_res_t _action_delete_linux_installer_files(lv_obj_t * btns, const char * txt)
{

	int btn_idx = lv_btnm_get_pressed(btns);

	// Delete parent mbox.
	mbox_action(btns, txt);

	// Flash Linux.
	if (!btn_idx)
	{
		char path[128];

		boot_storage_mount();

		strcpy(path, "switchroot/install/l4t.");

		// Delete all l4t.xx files.
		u32 idx = 0;
		while (true)
		{
			if (idx < 10)
			{
				path[23] = '0';
				itoa(idx, &path[23 + 1], 10);
			}
			else
				itoa(idx, &path[23], 10);

			if (!f_stat(path, NULL))
			{
				f_unlink(path);
			}
			else
				break;

			idx++;
		}

		boot_storage_unmount();
	}

	return LV_RES_INV;
}

static lv_res_t _action_flash_linux_data(lv_obj_t * btns, const char * txt)
{
	int btn_idx = lv_btnm_get_pressed(btns);

	// Delete parent mbox.
	mbox_action(btns, txt);

	bool succeeded = false;

	if (btn_idx)
		return LV_RES_INV;

	// Flash Linux.
	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *mbox_btn_map[] = { "\251", "\222OK", "\251", "" };
	static const char *mbox_btn_map2[] = { "\223Delete Installation Files", "\221OK", "" };
	lv_obj_t *mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);
	lv_obj_set_width(mbox, LV_HOR_RES / 10 * 5);

	lv_mbox_set_text(mbox, "#FF8000 Linux Flasher#");

	lv_obj_t *lbl_status = lv_label_create(mbox, NULL);
	lv_label_set_recolor(lbl_status, true);
	lv_label_set_text(lbl_status, "#C7EA46 Status:# Flashing Linux...");

	// Create container to keep content inside.
	lv_obj_t *h1 = lv_cont_create(mbox, NULL);
	lv_cont_set_fit(h1, true, true);
	lv_cont_set_style(h1, &lv_style_transp_tight);

	lv_obj_t *bar = lv_bar_create(h1, NULL);
	lv_obj_set_size(bar, LV_DPI * 30 / 10, LV_DPI / 5);
	lv_bar_set_range(bar, 0, 100);
	lv_bar_set_value(bar, 0);

	lv_obj_t *label_pct = lv_label_create(h1, NULL);
	lv_label_set_recolor(label_pct, true);
	lv_label_set_text(label_pct, " "SYMBOL_DOT" 0%");
	lv_label_set_style(label_pct, lv_theme_get_current()->label.prim);
	lv_obj_align(label_pct, bar, LV_ALIGN_OUT_RIGHT_MID, LV_DPI / 20, 0);

	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);

	boot_storage_mount();
	sd_mount();

	int res = 0;
	char *path = malloc(1024);
	char *txt_buf = malloc(SZ_4K);
	strcpy(path, "switchroot/install/l4t.00");
	u32 path_len = strlen(path) - 2;

	FIL fp;

	res = f_open(&fp, path, FA_READ);
	if (res)
	{
		lv_label_set_text(lbl_status, "#FFDD00 Error:# Failed to open 1st part!");

		goto exit;
	}

	u64 fileSize = (u64)f_size(&fp);

	u32 num = 0;
	u32 pct = 0;
	u32 lba_curr = 0;
	u32 bytesWritten = 0;
	u32 currPartIdx = 0;
	u32 prevPct = 200;
	int retryCount = 0;
	u32 total_size_sct = l4t_flash_ctxt.image_size_sct;

	u8 *buf = (u8 *)MIXD_BUF_ALIGNED;
	DWORD *clmt = f_expand_cltbl(&fp, SZ_4M, 0);

	// Start flashing L4T.
	while (total_size_sct > 0)
	{
		// If we have more than one part, check the size for the split parts and make sure that the bytes written is not more than that.
		if (bytesWritten >= fileSize)
		{
			// If we have more bytes written then close the file pointer and increase the part index we are using
			f_close(&fp);
			free(clmt);
			memset(&fp, 0, sizeof(fp));
			currPartIdx++;

			if (currPartIdx < 10)
			{
				path[path_len] = '0';
				itoa(currPartIdx, &path[path_len + 1], 10);
			}
			else
				itoa(currPartIdx, &path[path_len], 10);

			// Try to open the next file part
			res = f_open(&fp, path, FA_READ);
			if (res)
			{
				s_printf(txt_buf, "#FFDD00 Error:# Failed to open part %d#", currPartIdx);
				lv_label_set_text(lbl_status, txt_buf);
				manual_system_maintenance(true);

				goto exit;
			}
			fileSize = (u64)f_size(&fp);
			bytesWritten = 0;
			clmt = f_expand_cltbl(&fp, SZ_4M, 0);
		}

		retryCount = 0;
		num = MIN(total_size_sct, 8192);

		// Read next data block from SD.
		res = f_read_fast(&fp, buf, num << 9);
		manual_system_maintenance(false);

		if (res)
		{
			lv_label_set_text(lbl_status, "#FFDD00 Error:# Reading from SD!");
			manual_system_maintenance(true);

			f_close(&fp);
			free(clmt);
			goto exit;
		}

		// Write data block to L4T partition.
		res = !sdmmc_storage_write(&sd_storage, lba_curr + l4t_flash_ctxt.offset_sct, num, buf);

		manual_system_maintenance(false);

		// If failed, retry 3 more times.
		while (res)
		{
			msleep(150);
			manual_system_maintenance(true);

			if (retryCount >= 3)
			{
				lv_label_set_text(lbl_status, "#FFDD00 Error:# Writing to SD!");
				manual_system_maintenance(true);

				f_close(&fp);
				free(clmt);
				goto exit;
			}

			res = !sdmmc_storage_write(&sd_storage, lba_curr + l4t_flash_ctxt.offset_sct, num, buf);
			manual_system_maintenance(false);
		}

		// Update completion percentage.
		pct = (u64)((u64)lba_curr * 100u) / (u64)l4t_flash_ctxt.image_size_sct;
		if (pct != prevPct)
		{
			lv_bar_set_value(bar, pct);
			s_printf(txt_buf, " #DDDDDD "SYMBOL_DOT"# %d%%", pct);
			lv_label_set_text(label_pct, txt_buf);
			manual_system_maintenance(true);
			prevPct = pct;
		}

		lba_curr += num;
		total_size_sct -= num;
		bytesWritten += num * EMMC_BLOCKSIZE;
	}
	lv_bar_set_value(bar, 100);
	lv_label_set_text(label_pct, " "SYMBOL_DOT" 100%");
	manual_system_maintenance(true);

	// Restore operation ended successfully.
	f_close(&fp);
	free(clmt);

	succeeded = true;

exit:
	free(path);
	free(txt_buf);

	if (!succeeded)
		lv_mbox_add_btns(mbox, mbox_btn_map, mbox_action);
	else
		lv_mbox_add_btns(mbox, mbox_btn_map2, _action_delete_linux_installer_files);
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);

	sd_unmount();
	boot_storage_unmount();

	return LV_RES_INV;
}

static u32 _get_available_l4t_partition()
{
	mbr_t mbr = { 0 };
	gpt_t *gpt = zalloc(sizeof(gpt_t));

	memset(&l4t_flash_ctxt, 0, sizeof(l4t_flasher_ctxt_t));

	// Read MBR.
	sdmmc_storage_read(&sd_storage, 0, 1, &mbr);

	// Read main GPT.
	sdmmc_storage_read(&sd_storage, 1, sizeof(gpt_t) >> 9, gpt);

	// Search for a suitable partition.
	u32 size_sct = 0;
	if (!memcmp(&gpt->header.signature, "EFI PART", 8) || gpt->header.num_part_ents > 128)
	{
		for (u32 i = 0; i < gpt->header.num_part_ents; i++)
		{
			if (!memcmp(gpt->entries[i].name, (char[]) { 'l', 0, '4', 0, 't', 0 }, 6))
			{
				l4t_flash_ctxt.offset_sct = gpt->entries[i].lba_start;
				size_sct = (gpt->entries[i].lba_end + 1) - gpt->entries[i].lba_start;
				break;
			}

			if (i > 126)
				break;
		}
	}
	else
	{
		for (u32 i = 1; i < 4; i++)
		{
			if (mbr.partitions[i].type == 0x83)
			{
				l4t_flash_ctxt.offset_sct = mbr.partitions[i].start_sct;
				size_sct = mbr.partitions[i].size_sct;
				break;
			}
		}
	}

	free(gpt);

	return size_sct;
}

static int _get_available_android_partition()
{
	gpt_t *gpt = zalloc(sizeof(gpt_t));

	// Read main GPT.
	sdmmc_storage_read(&sd_storage, 1, sizeof(gpt_t) >> 9, gpt);

	// Check if GPT.
	if (memcmp(&gpt->header.signature, "EFI PART", 8) || gpt->header.num_part_ents > 128)
		goto out;

	// Find kernel partition.
	for (u32 i = 0; i < gpt->header.num_part_ents; i++)
	{
		if (gpt->entries[i].lba_start)
		{
			int found  = !memcmp(gpt->entries[i].name, (char[]) { 'b', 0, 'o', 0, 'o', 0, 't', 0 }, 8) ? 2 : 0;
				found |= !memcmp(gpt->entries[i].name, (char[]) { 'L', 0, 'N', 0, 'X', 0 },                     6) ? 1 : 0;

			if (found)
			{
				free(gpt);

				return found;
			}
		}

		if (i > 126)
			break;
	}

out:
	free(gpt);

	return false;
}

static lv_res_t _action_check_flash_linux(lv_obj_t *btn)
{
	FILINFO fno;
	char path[128];

	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *mbox_btn_map[] = { "\251", "\222OK", "\251", "" };
	static const char *mbox_btn_map2[] = { "\222Continue", "\222Cancel", "" };
	lv_obj_t *mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 6);

	lv_mbox_set_text(mbox, "#FF8000 Linux Flasher#");

	lv_obj_t *lbl_status = lv_label_create(mbox, NULL);
	lv_label_set_recolor(lbl_status, true);
	lv_label_set_text(lbl_status, "#C7EA46 Status:# Searching for files and partitions...");

	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);

	manual_system_maintenance(true);

	boot_storage_mount();

	// Check if L4T image exists.
	strcpy(path, "switchroot/install/l4t.00");
	if (f_stat(path, NULL))
	{
		lv_label_set_text(lbl_status, "#FFDD00 Error:# Installation files not found!");
		goto error;
	}

	// Find an applicable partition for L4T.
	u32 size_sct = _get_available_l4t_partition();
	if (!l4t_flash_ctxt.offset_sct || size_sct < 0x800000)
	{
		lv_label_set_text(lbl_status, "#FFDD00 Error:# No partition found!");
		goto error;
	}

	u32 idx = 0;
	path[23] = 0;

	// Validate L4T images and consolidate their info.
	while (true)
	{
		if (idx < 10)
		{
			path[23] = '0';
			itoa(idx, &path[23 + 1], 10);
		}
		else
			itoa(idx, &path[23], 10);

		// Check for alignment.
		if (f_stat(path, &fno))
			break;

		// Check if current part is unaligned.
		if ((u64)fno.fsize % SZ_4M)
		{
			// Get next part filename.
			idx++;
			if (idx < 10)
			{
				path[23] = '0';
				itoa(idx, &path[23 + 1], 10);
			}
			else
				itoa(idx, &path[23], 10);

			// If it exists, unaligned size for current part is not permitted.
			if (!f_stat(path, NULL)) // NULL: Don't override current part fs info.
			{
				lv_label_set_text(lbl_status, "#FFDD00 Error:# The image is not aligned to 4 MiB!");
				goto error;
			}

			// Last part. Align size to LBA (SD_BLOCKSIZE).
			fno.fsize = ALIGN((u64)fno.fsize, SD_BLOCKSIZE);
			idx--;
		}
		l4t_flash_ctxt.image_size_sct += (u64)fno.fsize >> 9;

		idx++;
	}

	// Check if image size is bigger than the partition available.
	if (l4t_flash_ctxt.image_size_sct > size_sct)
	{
		lv_label_set_text(lbl_status, "#FFDD00 Error:# The image is bigger than the partition!");
		goto error;
	}

	char *txt_buf = malloc(SZ_4K);
	s_printf(txt_buf,
		"#C7EA46 Status:# Found installation files and partition.\n"
		"#00DDFF Offset:# %08x, #00DDFF Size:# %X, #00DDFF Image size:# %d MiB\n"
		"\nDo you want to continue?", l4t_flash_ctxt.offset_sct, size_sct, l4t_flash_ctxt.image_size_sct >> 11);
	lv_label_set_text(lbl_status, txt_buf);
	free(txt_buf);
	lv_mbox_add_btns(mbox, mbox_btn_map2, _action_flash_linux_data);
	goto exit;

error:
	lv_mbox_add_btns(mbox, mbox_btn_map, mbox_action);

exit:
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);

	boot_storage_unmount();

	return LV_RES_OK;
}

static lv_res_t _action_reboot_recovery(lv_obj_t * btns, const char * txt)
{
	int btn_idx = lv_btnm_get_pressed(btns);

	// Delete parent mbox.
	mbox_action(btns, txt);

	if (!btn_idx)
	{
		// Set custom reboot type to Android Recovery.
		PMC(APBDEV_PMC_SCRATCH0) |= PMC_SCRATCH0_MODE_RECOVERY;

		// Enable hekate boot configuration.
		b_cfg->boot_cfg = BOOT_CFG_FROM_ID | BOOT_CFG_AUTOBOOT_EN;

		// Set id to Android.
		strcpy((char *)b_cfg->id, "SWANDR");

		void (*main_ptr)() = (void *)nyx_str->hekate;

		// Deinit hardware.
		sd_end();
		hw_deinit(false, 0);

		// Chainload to hekate main.
		(*main_ptr)();
	}

	return LV_RES_INV;
}

static lv_res_t _action_flash_android_data(lv_obj_t * btns, const char * txt)
{
	int btn_idx = lv_btnm_get_pressed(btns);
	bool boot_recovery = false;

	// Delete parent mbox.
	mbox_action(btns, txt);

	if (btn_idx)
		return LV_RES_INV;

	// Flash Android components.
	char path[128];
	gpt_t *gpt = zalloc(sizeof(gpt_t));
	char *txt_buf = malloc(SZ_4K);

	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *mbox_btn_map[] = { "\251", "\222OK", "\251", "" };
	static const char *mbox_btn_map2[] = { "\222Continue", "\222No", "" };
	lv_obj_t *mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 6);

	lv_mbox_set_text(mbox, "#FF8000 Android Flasher#");

	lv_obj_t *lbl_status = lv_label_create(mbox, NULL);
	lv_label_set_recolor(lbl_status, true);
	lv_label_set_text(lbl_status, "#C7EA46 Status:# Searching for files and partitions...");

	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);

	manual_system_maintenance(true);

	sd_mount();
	boot_storage_mount();

	// Read main GPT.
	sdmmc_storage_read(&sd_storage, 1, sizeof(gpt_t) >> 9, gpt);

	// Validate GPT header.
	if (memcmp(&gpt->header.signature, "EFI PART", 8) || gpt->header.num_part_ents > 128)
	{
		lv_label_set_text(lbl_status, "#FFDD00 Error:# No Android GPT was found!");
		goto error;
	}

	u32 offset_sct = 0;
	u32 size_sct = 0;

	// Check if Kernel image should be flashed.
	strcpy(path, "switchroot/install/boot.img");
	if (f_stat(path, NULL))
	{
		s_printf(txt_buf, "#FF8000 Warning:# Kernel image not found!\n");
		goto boot_img_not_found;
	}

	// Find Kernel partition.
	for (u32 i = 0; i < gpt->header.num_part_ents; i++)
	{
		if (!memcmp(gpt->entries[i].name, (char[]) { 'L', 0, 'N', 0, 'X', 0 }, 6) || !memcmp(gpt->entries[i].name, (char[]) { 'b', 0, 'o', 0, 'o', 0, 't', 0 }, 8))
		{
			offset_sct = gpt->entries[i].lba_start;
			size_sct = (gpt->entries[i].lba_end + 1) - gpt->entries[i].lba_start;
			break;
		}

		if (i > 126)
			break;
	}

	// Flash Kernel.
	if (offset_sct && size_sct)
	{
		u32 file_size = 0;
		u8 *buf = boot_storage_file_read(path, &file_size);

		if (file_size % 0x200)
		{
			file_size = ALIGN(file_size, 0x200);
			u8 *buf_tmp = zalloc(file_size);
			memcpy(buf_tmp, buf, file_size);
			free(buf);
			buf = buf_tmp;
		}

		if ((file_size >> 9) > size_sct)
			s_printf(txt_buf, "#FF8000 Warning:# Kernel image too big!\n");
		else
		{
			sdmmc_storage_write(&sd_storage, offset_sct, file_size >> 9, buf);

			s_printf(txt_buf, "#C7EA46 Success:# Kernel image flashed!\n");
			f_unlink(path);
		}

		free(buf);
	}
	else
		s_printf(txt_buf, "#FF8000 Warning:# Kernel partition not found!\n");

boot_img_not_found:
	lv_label_set_text(lbl_status, txt_buf);
	manual_system_maintenance(true);

	// Check if Recovery should be flashed.
	strcpy(path, "switchroot/install/recovery.img");
	if (f_stat(path, NULL))
	{
		// Not found, try twrp.img instead.
		strcpy(path, "switchroot/install/twrp.img");
		if (f_stat(path, NULL))
		{
			strcat(txt_buf, "#FF8000 Warning:# Recovery image not found!\n");
			goto recovery_not_found;
		}
	}

	offset_sct = 0;
	size_sct = 0;

	// Find Recovery partition.
	for (u32 i = 0; i < gpt->header.num_part_ents; i++)
	{
		if (!memcmp(gpt->entries[i].name, (char[]) { 'S', 0, 'O', 0, 'S', 0 }, 6) || !memcmp(gpt->entries[i].name, (char[]) { 'r', 0, 'e', 0, 'c', 0, 'o', 0, 'v', 0, 'e', 0, 'r', 0, 'y', 0 }, 16))
		{
			offset_sct = gpt->entries[i].lba_start;
			size_sct = (gpt->entries[i].lba_end + 1) - gpt->entries[i].lba_start;
			break;
		}

		if (i > 126)
			break;
	}

	// Flash Recovery.
	if (offset_sct && size_sct)
	{
		u32 file_size = 0;
		u8 *buf = boot_storage_file_read(path, &file_size);

		if (file_size % 0x200)
		{
			file_size = ALIGN(file_size, 0x200);
			u8 *buf_tmp = zalloc(file_size);
			memcpy(buf_tmp, buf, file_size);
			free(buf);
			buf = buf_tmp;
		}

		if ((file_size >> 9) > size_sct)
			strcat(txt_buf, "#FF8000 Warning:# Recovery image too big!\n");
		else
		{
			sdmmc_storage_write(&sd_storage, offset_sct, file_size >> 9, buf);
			strcat(txt_buf, "#C7EA46 Success:# Recovery image flashed!\n");
			f_unlink(path);
		}

		free(buf);
	}
	else
		strcat(txt_buf, "#FF8000 Warning:# Recovery partition not found!\n");

recovery_not_found:
	lv_label_set_text(lbl_status, txt_buf);
	manual_system_maintenance(true);

	// Check if Device Tree should be flashed.
	strcpy(path, "switchroot/install/nx-plat.dtimg");
	if (f_stat(path, NULL))
	{
		strcpy(path, "switchroot/install/tegra210-icosa.dtb");
		if (f_stat(path, NULL))
		{
			strcat(txt_buf, "#FF8000 Warning:# DTB image not found!");
			goto dtb_not_found;
		}
	}

	offset_sct = 0;
	size_sct = 0;

	// Find Device Tree partition.
	for (u32 i = 0; i < gpt->header.num_part_ents; i++)
	{
		if (!memcmp(gpt->entries[i].name, (char[]) { 'D', 0, 'T', 0, 'B', 0 }, 6) || !memcmp(gpt->entries[i].name, (char[]) { 'd', 0, 't', 0, 'b', 0 }, 6))
		{
			offset_sct = gpt->entries[i].lba_start;
			size_sct = (gpt->entries[i].lba_end + 1) - gpt->entries[i].lba_start;
			break;
		}

		if (i > 126)
			break;
	}

	// Flash Device Tree.
	if (offset_sct && size_sct)
	{
		u32 file_size = 0;
		u8 *buf = boot_storage_file_read(path, &file_size);

		if (file_size % 0x200)
		{
			file_size = ALIGN(file_size, 0x200);
			u8 *buf_tmp = zalloc(file_size);
			memcpy(buf_tmp, buf, file_size);
			free(buf);
			buf = buf_tmp;
		}

		if ((file_size >> 9) > size_sct)
			strcat(txt_buf, "#FF8000 Warning:# DTB image too big!");
		else
		{
			sdmmc_storage_write(&sd_storage, offset_sct, file_size >> 9, buf);
			strcat(txt_buf, "#C7EA46 Success:# DTB image flashed!");
			f_unlink(path);
		}

		free(buf);
	}
	else
		strcat(txt_buf, "#FF8000 Warning:# DTB partition not found!");

dtb_not_found:
	lv_label_set_text(lbl_status, txt_buf);

	// Check if Recovery is flashed unconditionally.
	for (u32 i = 0; i < gpt->header.num_part_ents; i++)
	{
		if (!memcmp(gpt->entries[i].name, (char[]) { 'S', 0, 'O', 0, 'S', 0 }, 6) || !memcmp(gpt->entries[i].name, (char[]) { 'r', 0, 'e', 0, 'c', 0, 'o', 0, 'v', 0, 'e', 0, 'r', 0, 'y', 0 }, 16))
		{
			u8 *buf = malloc(SD_BLOCKSIZE);
			sdmmc_storage_read(&sd_storage, gpt->entries[i].lba_start, 1, buf);
			if (!memcmp(buf, "ANDROID", 7))
				boot_recovery = true;
			free(buf);
			break;
		}

		if (i > 126)
			break;
	}

error:
	if (boot_recovery)
	{
		// If a Recovery partition was found, ask user if rebooting into it is wanted.
		strcat(txt_buf,"\n\nDo you want to reboot into Recovery\nto finish Android installation?");
		lv_label_set_text(lbl_status, txt_buf);
		lv_mbox_add_btns(mbox, mbox_btn_map2, _action_reboot_recovery);
	}
	else
		lv_mbox_add_btns(mbox, mbox_btn_map, mbox_action);

	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);

	free(txt_buf);
	free(gpt);

	sd_unmount();
	boot_storage_unmount();

	return LV_RES_INV;
}

static lv_res_t _action_flash_android(lv_obj_t *btn)
{
	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *mbox_btn_map[] = { "\222Continue", "\222Cancel", "" };
	lv_obj_t *mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 6);

	lv_mbox_set_text(mbox, "#FF8000 Android Flasher#");

	lv_obj_t *lbl_status = lv_label_create(mbox, NULL);
	lv_label_set_recolor(lbl_status, true);
	lv_label_set_text(lbl_status,
		"This will flash #C7EA46 Kernel#, #C7EA46 DTB# and #C7EA46 Recovery# if found.\n"
		"These will be deleted after a successful flash.\n"
		"Do you want to continue?");

	lv_mbox_add_btns(mbox, mbox_btn_map,  _action_flash_android_data);
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);

	return LV_RES_OK;
}

static lv_res_t _action_part_manager_flash_options0(lv_obj_t *btns, const char *txt)
{
	int btn_idx = lv_btnm_get_pressed(btns);

	switch (btn_idx)
	{
	case 0:
		action_ums_sd(btns);
		lv_obj_del(ums_mbox);
		break;
	case 1:
		_action_check_flash_linux(btns);
		break;
	case 2:
		_action_flash_android(btns);
		break;
	case 3:
		mbox_action(btns, txt);
		return LV_RES_INV;
	}

	return LV_RES_OK;
}

static lv_res_t _action_part_manager_flash_options1(lv_obj_t *btns, const char *txt)
{
	int btn_idx = lv_btnm_get_pressed(btns);

	switch (btn_idx)
	{
	case 0:
		action_ums_sd(btns);
		lv_obj_del(ums_mbox);
		break;
	case 1:
		mbox_action(btns, txt);
		_action_check_flash_linux(NULL);
		return LV_RES_INV;
	case 2:
		mbox_action(btns, txt);
		return LV_RES_INV;
	}

	return LV_RES_OK;
}

static lv_res_t _action_part_manager_flash_options2(lv_obj_t *btns, const char *txt)
{
	int btn_idx = lv_btnm_get_pressed(btns);

	switch (btn_idx)
	{
	case 0:
		action_ums_sd(btns);
		lv_obj_del(ums_mbox);
		break;
	case 1:
		mbox_action(btns, txt);
		_action_flash_android(NULL);
		return LV_RES_INV;
	case 2:
		mbox_action(btns, txt);
		return LV_RES_INV;
	}

	return LV_RES_OK;
}

static int _backup_and_restore_files(bool backup, const char *drive, lv_obj_t **labels)
{
	const char *src_drv = backup ? drive  : "ram:";
	const char *dst_drv = backup ? "ram:" : drive;

	int res = 0;
	u32 total_size = 0;
	u32 total_files = 0;
	char *path = malloc(0x1000);
	path[0] = 0; // Set default as root folder.

	// Check if Mariko Warmboot Storage exists in source drive.
	f_chdrive(src_drv);
	bool backup_mws = !part_info.backup_possible && !f_stat("warmboot_mariko", NULL);
	bool backup_pld = !part_info.backup_possible && !f_stat("payload.bin", NULL);

	if (!part_info.backup_possible)
	{
		// Change path to hekate/Nyx.
		strcpy(path, "bootloader");

		// Create hekate/Nyx/MWS folders in destination drive.
		f_chdrive(dst_drv);
		f_mkdir("bootloader");
		if (backup_mws)
			f_mkdir("warmboot_mariko");
	}

	// Copy all or hekate/Nyx files.
	res = _stat_and_copy_files(src_drv, dst_drv, path, &total_files, &total_size, labels);

	// If incomplete backup mode, copy MWS and payload.bin also.
	if (!res)
	{
		if (backup_mws)
		{
			strcpy(path, "warmboot_mariko");
			res = _stat_and_copy_files(src_drv, dst_drv, path, &total_files, &total_size, labels);
		}

		if (!res && backup_pld)
		{
			strcpy(path, "payload.bin");
			res = _copy_file(src_drv, dst_drv, path);
		}
	}

	free(path);

	return res;
}

static DWORD _format_fat_partition(const char* path, u8 flags){
	// Set cluster size to 64KB and try to format.
	u8 *buf = malloc(SZ_4M);
	u32 cluster_size = 65536;
	u32 mkfs_error = f_mkfs(path, flags, cluster_size, buf, SZ_4M);

	// Retry formatting by halving cluster size, until one succeeds.
	while (mkfs_error != FR_OK && cluster_size > 4096)
	{
		cluster_size /= 2;
		mkfs_error = f_mkfs(path, flags, cluster_size, buf, SZ_4M);
	}

	free(buf);

	return mkfs_error;
}

static lv_res_t _create_mbox_start_partitioning(lv_obj_t *btn)
{
	char cwd[0x200];
	gpt_t *new_gpt = NULL;

	// TODO: remove the cwd stuff
	f_getcwd(cwd, sizeof(cwd));

	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *mbox_btn_map[] =  { "\251", "\222OK", "\251", "" };
	static const char *mbox_btn_map1[] = { "\222SD UMS", "\222Flash Linux", "\222Flash Android", "\221OK", "" };
	static const char *mbox_btn_map2[] = { "\222SD UMS", "\222Flash Linux", "\221OK", "" };
	static const char *mbox_btn_map3[] = { "\222SD UMS", "\222Flash Android", "\221OK", "" };

	static const char *mbox_btn_map1_emmc[] = { "\222eMMC UMS", "\222Flash Linux", "\222Flash Android", "\221OK", "" };
	static const char *mbox_btn_map2_emmc[] = { "\222eMMC UMS", "\222Flash Linux", "\221OK", "" };
	static const char *mbox_btn_map3_emmc[] = { "\222eMMC UMS", "\222Flash Android", "\221OK", "" };

	sdmmc_storage_t *storage = part_info.drive == DRIVE_SD ? &sd_storage : &emmc_storage;

	lv_obj_t *mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 6);

	lv_mbox_set_text(mbox, "#FF8000 Partition Manager#");
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);

	bool buttons_set = false;

	// Use safety wait if backup is not possible.
	char *txt_buf = malloc(SZ_4K);
	strcpy(txt_buf, "#FF8000 Partition Manager#\n\nSafety wait ends in ");
	lv_mbox_set_text(mbox, txt_buf);

	u32 seconds = 5;
	u32 text_idx = strlen(txt_buf);
	while (seconds)
	{
		s_printf(txt_buf + text_idx, "%d seconds...", seconds);
		lv_mbox_set_text(mbox, txt_buf);
		manual_system_maintenance(true);
		msleep(1000);
		seconds--;
	}

	lv_mbox_set_text(mbox,
		"#FF8000 Partition Manager#\n\n"
		"#FFDD00 Warning: Do you really want to continue?!#\n\n"
		"Press #FF8000 POWER# to Continue.\nPress #FF8000 VOL# to abort.");
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	manual_system_maintenance(true);

	free(txt_buf);

	if (!(btn_wait() & BTN_POWER))
		goto exit;

	lv_mbox_set_text(mbox, "#FF8000 Partition Manager#");
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	manual_system_maintenance(true);

	lv_obj_t *lbl_status = lv_label_create(mbox, NULL);
	lv_label_set_recolor(lbl_status, true);

	lv_obj_t *lbl_paths[2];

	// Create backup/restore paths labels.
	lbl_paths[0] = lv_label_create(mbox, NULL);
	lv_label_set_text(lbl_paths[0], "/");
	lv_label_set_long_mode(lbl_paths[0], LV_LABEL_LONG_DOT);
	lv_cont_set_fit(lbl_paths[0], false, true);
	lv_obj_set_width(lbl_paths[0], (LV_HOR_RES / 9 * 6) - LV_DPI / 2);
	lv_label_set_align(lbl_paths[0], LV_LABEL_ALIGN_CENTER);
	lbl_paths[1] = lv_label_create(mbox, NULL);
	lv_label_set_text(lbl_paths[1], " ");
	lv_label_set_long_mode(lbl_paths[1], LV_LABEL_LONG_DOT);
	lv_cont_set_fit(lbl_paths[1], false, true);
	lv_obj_set_width(lbl_paths[1], (LV_HOR_RES / 9 * 6) - LV_DPI / 2);
	lv_label_set_align(lbl_paths[1], LV_LABEL_ALIGN_CENTER);

	FATFS ram_fs;

	if(part_info.drive == DRIVE_SD){
		sd_mount();
	}else{
		emmc_mount();
	}

	// Read current MBR.
	sdmmc_storage_read(storage, 0, 1, &part_info.mbr_old);

	if(!part_info.skip_backup && part_info.hos_size){
		// can't backup / restore if new scheme has no fat partition. dialog will have warned about this
		lv_label_set_text(lbl_status, "#00DDFF Status:# Initializing Ramdisk...");
		lv_label_set_text(lbl_paths[0], "Please wait...");
		lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
		manual_system_maintenance(true);

		// Initialize RAM disk.
		if (ram_disk_init(&ram_fs, RAM_DISK_SZ))
		{
			lv_label_set_text(lbl_status, "#FFDD00 Error:# Failed to initialize Ramdisk!");
			goto error;
		}

		lv_label_set_text(lbl_status, "#00DDFF Status:# Backing up files...");
		manual_system_maintenance(true);

		// Do full or hekate/Nyx backup.
		if (_backup_and_restore_files(true, part_info.drive == DRIVE_SD ? "sd:" : "emmc:", lbl_paths))
		{
			if (part_info.backup_possible)
				lv_label_set_text(lbl_status, "#FFDD00 Error:# Failed to back up files!");
			else
				lv_label_set_text(lbl_status, "#FFDD00 Error:# Failed to back up files!\nBootloader folder exceeds 1GB or corrupt!");
			manual_system_maintenance(true);

			goto error;
		}
	}

	if(part_info.drive == DRIVE_SD){
		sd_unmount();
	}else{
		emmc_unmount();
	}

	lv_label_set_text(lbl_status, "#00ddff Status:# Writing new partition table...");
	lv_label_set_text(lbl_paths[0], "Please wait...");
	lv_label_set_text(lbl_paths[1], " ");
	manual_system_maintenance(true);

	_prepare_and_flash_mbr_gpt();

	mbr_t new_mbr;

	sdmmc_storage_read(storage, 0, 1, &new_mbr);

	bool has_gpt = _has_gpt(&new_mbr);

	if(part_info.hos_size){
		u32 hos_start = 0;
		u32 hos_size = 0;
		if(!has_gpt){
			hos_size = new_mbr.partitions[0].size_sct;
			hos_start = new_mbr.partitions[0].start_sct;
		}else{
			new_gpt = zalloc(sizeof(*new_gpt));
			sdmmc_storage_read(storage, 1, sizeof(*new_gpt) / 0x200, new_gpt);
			int hos_idx = _get_gpt_part_by_name(new_gpt, "hos_data", -1);
			if(hos_idx != -1){
				hos_size = new_gpt->entries[hos_idx].lba_end - new_gpt->entries[hos_idx].lba_start + 1;
				hos_start = new_gpt->entries[hos_idx].lba_start;
			}
		}


		lv_label_set_text(lbl_status, "#00ddff Status:# Formatting FAT32 partition...");
		manual_system_maintenance(true);

		sfd_init(storage, hos_start, hos_size);
		u32 mkfs_error = _format_fat_partition("sfd:", FM_FAT32 | FM_SFD);
		
		u8 *buf = malloc(0x200);

		if(mkfs_error != FR_OK){
			// Error
			s_printf((char*)buf, "#FFDD00 Error:# Failed to format disk (%d)!\n\n", mkfs_error);
			if(part_info.drive == DRIVE_EMMC || part_info.skip_backup){
				// on eMMC not much we can do
				strcat((char*)buf, "Press #FF8000 POWER# to continue!");
			}else{
				strcat((char*)buf, "Remove the SD card and check that it is OK.\nIf not, format it, reinsert it and\npress #FF8000 POWER# to continue!");
			}

			lv_label_set_text(lbl_status, (char *)buf);
			lv_label_set_text(lbl_paths[0], " ");
			manual_system_maintenance(true);

			if(part_info.drive == DRIVE_SD && !part_info.skip_backup){
				sd_end();
				while((!btn_wait()) & BTN_POWER){}
				sd_mount();

				lv_label_set_text(lbl_status, "#00DDFF Status:# Restoring files...");
				manual_system_maintenance(true);

				// Try twice to restore files
				if (_backup_and_restore_files(false, "sd:", lbl_paths) == FR_OK || 
				    _backup_and_restore_files(false, "sd:", lbl_paths) == FR_OK){
					lv_label_set_text(lbl_status, "#00DDFF Status:# Restored files but the operation failed!");
				}else{
					lv_label_set_text(lbl_status, "#FFDD00 Error:# Failed to restore files!");
				}
				manual_system_maintenance(true);
			}else{
				// On eMMC/when skipping backup, nothing we can do
				while((!btn_wait()) & BTN_POWER){}
			}
			f_mount(NULL, "ram:", 0);
			sfd_end();
			free(buf);
			goto error;
		}else{
			// No error
			// remount
			if(part_info.drive == DRIVE_SD){
				sd_mount();
			}else{
				emmc_mount();
			}

			f_setlabel(part_info.drive == DRIVE_SD ? "sd:SWITCH SD" : "emmc:SWITCH EMMC");
			
			lv_label_set_text(lbl_status, "#00DDFF Status:# Restoring files...");
			manual_system_maintenance(true);
			// Try twice to restroe files
			if (_backup_and_restore_files(false, part_info.drive == DRIVE_SD ? "sd:" : "emmc:", lbl_paths) != FR_OK &&
			    _backup_and_restore_files(false, part_info.drive == DRIVE_SD ? "sd:" : "emmc:", lbl_paths) != FR_OK)
			{
				// Restore failed
				lv_label_set_text(lbl_status, "#FFDD00 Error:# Failed to restore files!");
				manual_system_maintenance(true);
				f_mount(NULL, "ram:", 0);
				sfd_end();
				free(buf);
				goto error;
			}
			f_mount(NULL, "ram:", 0);
			free(buf);
		}
		sfd_end();
	}

	if(has_gpt){
		s32 gpt_idx = -1;
		while((gpt_idx = _get_gpt_part_by_name(new_gpt, "emusd_mbr", gpt_idx)) != -1){
			u32 emu_sd_start = new_gpt->entries[gpt_idx].lba_start;
			u32 emu_sd_size = part_info.emu_sd_size;
			if(part_info.emu_sd_double){
				emu_sd_size /= 2;
			}

			FIL f;
			u32 res;

			lv_label_set_text(lbl_status, "#00DDFF Status:# Formatting emuSD partition...");
			manual_system_maintenance(true);

			sfd_init(storage, emu_sd_start, emu_sd_size);

			res = _format_fat_partition("sfd:", FM_FAT32);
			if(res == FR_OK){
				res = f_open(&f, "sfd:.no_boot_storage", FA_CREATE_ALWAYS | FA_WRITE);
				f_close(&f);
			}

			if(res == FR_OK){
				lv_label_set_text(lbl_status, "#FFDD00 Error:# Failed format emuSD partition!");
				manual_system_maintenance(true);
				sfd_end();
				goto error;
			}

			sfd_end();
		}
	}

	// Enable/Disable buttons depending on partition layout.
	if (part_info.l4t_size)
	{
		lv_obj_set_click(btn_flash_l4t, true);
		lv_btn_set_state(btn_flash_l4t, LV_BTN_STATE_REL);
	}
	else
	{
		lv_obj_set_click(btn_flash_l4t, false);
		lv_btn_set_state(btn_flash_l4t, LV_BTN_STATE_INA);
	}

	// Enable/Disable buttons depending on partition layout.
	if (part_info.and_size)
	{
		lv_obj_set_click(btn_flash_android, true);
		lv_btn_set_state(btn_flash_android, LV_BTN_STATE_REL);
	}
	else
	{
		lv_obj_set_click(btn_flash_android, false);
		lv_btn_set_state(btn_flash_android, LV_BTN_STATE_INA);
	}

	lv_label_set_text(lbl_status, "#00DDFF Status:# Done!");
	manual_system_maintenance(true);

	// Set buttons depending on what user chose to create.
	if (part_info.l4t_size && part_info.and_size)
		lv_mbox_add_btns(mbox, part_info.drive == DRIVE_SD ? mbox_btn_map1 : mbox_btn_map1_emmc, _action_part_manager_flash_options0);
	else if (part_info.l4t_size)
		lv_mbox_add_btns(mbox, part_info.drive == DRIVE_SD ? mbox_btn_map2 : mbox_btn_map2_emmc, _action_part_manager_flash_options1);
	else if (part_info.and_size)
		lv_mbox_add_btns(mbox, part_info.drive == DRIVE_SD ? mbox_btn_map3 : mbox_btn_map3_emmc, _action_part_manager_flash_options2);

	if (part_info.l4t_size || part_info.and_size)
		buttons_set = true;


error:
	lv_obj_del(lbl_paths[0]);
	lv_obj_del(lbl_paths[1]);

exit:
	if(!buttons_set){
		lv_mbox_add_btns(mbox, mbox_btn_map, mbox_action);
	}
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);

	// Disable partitioning button.
	if (btn){
		lv_btn_set_state(btn, LV_BTN_STATE_INA);
	}

	free(new_gpt);

	return LV_RES_OK;
}

static lv_res_t _create_mbox_partitioning_option0(lv_obj_t *btns, const char *txt)
{
	int btn_idx = lv_btnm_get_pressed(btns);

	switch (btn_idx)
	{
	case 0:
		if(part_info.drive == DRIVE_SD){
			action_ums_sd(btns);
		}else{
			action_ums_emmc_gpp(btns);
		}
		return LV_RES_OK;
	case 1:
		mbox_action(btns, txt);
		_create_mbox_start_partitioning(NULL);
		break;
	case 2:
		mbox_action(btns, txt);
		break;
	}

	return LV_RES_INV;
}

static lv_res_t _create_mbox_partitioning_option1(lv_obj_t *btns, const char *txt)
{
	int btn_idx = lv_btnm_get_pressed(btns);

	mbox_action(btns, txt);

	if (!btn_idx)
	{
		mbox_action(btns, txt);
		_create_mbox_start_partitioning(NULL);
		return LV_RES_INV;
	}

	return LV_RES_OK;
}

static lv_res_t _create_mbox_partitioning_warn(lv_obj_t *btn)
{
	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *mbox_btn_map[] = { "\222SD UMS", "\222Start", "\222Cancel", "" };
	static const char *mbox_btn_map3[] = { "\222eMMC UMS", "\222Start", "\222Cancel", "" };
	static const char *mbox_btn_map2[] = { "\222Start", "\222Cancel", "" };
	lv_obj_t * mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);

	char *txt_buf = malloc(SZ_4K);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 6);
	lv_mbox_set_text(mbox, "#FF8000 Partition Manager#");

	lv_obj_t *lbl_status = lv_label_create(mbox, NULL);
	lv_label_set_recolor(lbl_status, true);

	if(part_info.drive == DRIVE_SD){
		s_printf(txt_buf, "#FFDD00 Warning: This will partition the SD Card!#\n\n");
	}else{
		s_printf(txt_buf, "#FFDD00 Warning: This will partition the eMMC!#\n\n");
	}

	if (part_info.backup_possible && part_info.hos_size)
	{
		strcat(txt_buf, "#C7EA46 Your files will be backed up and restored!#\n"
			"#FFDD00 Any other partition will be wiped!#");
	}
	else if(part_info.skip_backup)
	{
		// We have no files to back up
		if(part_info.drive == DRIVE_SD){
			strcat(txt_buf, "#FFDD00 All partitions will be wiped!#\n");
		}else{
			if(part_info.hos_os_size){
				strcat(txt_buf, "#FFDD00 All partitions (except HOS ones) will be wiped!#\n");
			}else{
				strcat(txt_buf, "#FFDD00 All partitions will be wiped!#\n");
			}
		}
	}else{
		// Have files, can't back up
		strcat(txt_buf, "#FFDD00 Your files will be wiped!#\n");
		if(part_info.drive == DRIVE_SD){
			strcat(txt_buf, "#FFDD00 All partitions will be also wiped!#\n");
		}else{
			if(part_info.hos_os_size){
				strcat(txt_buf, "#FFDD00 All partitions (except HOS ones) will also be wiped!#\n");
			}else{
				strcat(txt_buf, "#FFDD00 All partitions will also be wiped!#\n");
			}
		}
		strcat(txt_buf, "#FFDD00 Use USB UMS to copy them over!#");
	}

	lv_label_set_text(lbl_status, txt_buf);

	if ((part_info.backup_possible && part_info.hos_size) || part_info.skip_backup)
		lv_mbox_add_btns(mbox, mbox_btn_map2, _create_mbox_partitioning_option1);
	else
		lv_mbox_add_btns(mbox, part_info.drive == DRIVE_SD ? mbox_btn_map : mbox_btn_map3, _create_mbox_partitioning_option0);

	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);

	free(txt_buf);

	return LV_RES_OK;
}

static lv_res_t _create_mbox_partitioning_android(lv_obj_t *btns, const char *txt)
{
	int btn_idx = lv_btnm_get_pressed(btns);

	mbox_action(btns, txt);

	part_info.and_dynamic = !btn_idx;
	_create_mbox_partitioning_warn(NULL);

	return LV_RES_INV;
}

static lv_res_t _create_mbox_partitioning_andr_part(lv_obj_t *btn)
{
	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *mbox_btn_map[] = { "\222Dynamic", "\222Legacy", "" };
	lv_obj_t * mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);

	lv_obj_set_width(mbox, LV_HOR_RES / 10 * 5);
	lv_mbox_set_text(mbox, "#FF8000 Android Partitioning#");

	lv_obj_t *lbl_status = lv_label_create(mbox, NULL);
	lv_label_set_recolor(lbl_status, true);

	lv_label_set_text(lbl_status,
		"Please select a partition scheme:\n\n"
		"#C7EA46 Dynamic:# Android 13+\n"
		"#C7EA46 Legacy:# Android 10-11\n");

	lv_mbox_add_btns(mbox, mbox_btn_map, _create_mbox_partitioning_android);
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);

	return LV_RES_OK;
}

static lv_res_t _create_mbox_partitioning_next(lv_obj_t *btn) {
	if (part_info.and_size)
		return _create_mbox_partitioning_andr_part(NULL);
	else
		return _create_mbox_partitioning_warn(NULL);
}

static void _update_partition_bar()
{
	lv_obj_t *h1 = lv_obj_get_parent(part_info.bar_hos);

	// Set widths based on max bar width.
	lv_coord_t w = lv_obj_get_width(h1);
	
	// account for alignment + 1mb for backup gpt
	u32 total_size         = (part_info.total_sct - AU_ALIGN_SECTORS - (1 << 11)) / SECTORS_PER_GB;

	u32 bar_hos_size       = w * (part_info.hos_size    >> 10) / total_size;
	u32 bar_emu_size       = w * (part_info.emu_size    >> 10) / total_size;
	u32 bar_l4t_size       = w * (part_info.l4t_size    >> 10) / total_size;
	u32 bar_and_size       = w * (part_info.and_size    >> 10) / total_size;
	u32 bar_hos_os_size    = w * (part_info.hos_os_size >> 10) / total_size;
	u32 bar_emu_sd_size    = w * (part_info.emu_sd_size >> 10) / total_size;

	u32 bar_remaining_size = w - (bar_hos_size + bar_emu_size + bar_l4t_size + bar_and_size + bar_hos_os_size + bar_emu_sd_size);
	bar_remaining_size = bar_remaining_size <= 7 ? 0 : bar_remaining_size;

	// Update bar widths.
	lv_obj_set_size(part_info.bar_hos,       bar_hos_size,       LV_DPI / 2);
	lv_obj_set_size(part_info.bar_emu,       bar_emu_size,       LV_DPI / 2);
	lv_obj_set_size(part_info.bar_l4t,       bar_l4t_size,       LV_DPI / 2);
	lv_obj_set_size(part_info.bar_and,       bar_and_size,       LV_DPI / 2);
	lv_obj_set_size(part_info.bar_hos_os,    bar_hos_os_size,    LV_DPI / 2);
	lv_obj_set_size(part_info.bar_remaining, bar_remaining_size, LV_DPI / 2);
	lv_obj_set_size(part_info.bar_emu_sd,    bar_emu_sd_size,    LV_DPI / 2);

	// Re-align bars.
	lv_obj_align(part_info.bar_hos, part_info.bar_hos_os, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
	lv_obj_align(part_info.bar_emu, part_info.bar_hos, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
	lv_obj_align(part_info.bar_l4t, part_info.bar_emu_sd, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
	lv_obj_align(part_info.bar_and, part_info.bar_l4t, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
	lv_obj_align(part_info.bar_remaining, part_info.bar_and, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
	lv_obj_align(part_info.bar_emu_sd, part_info.bar_emu, LV_ALIGN_OUT_RIGHT_MID, 0, 0);

	// Set HOS OS blending separator sizes and realign.
	lv_obj_set_size(part_info.sep_hos_os, bar_hos_os_size && (bar_remaining_size || bar_hos_size || bar_and_size || bar_l4t_size || bar_emu_sd_size || bar_emu_size) ? 8 : 0, LV_DPI / 2);
	lv_obj_align(part_info.sep_hos_os, part_info.bar_hos_os, LV_ALIGN_OUT_RIGHT_MID, -4, 0);

	// Set hos separator
	lv_obj_set_size(part_info.sep_hos, bar_hos_size && (bar_remaining_size || bar_and_size || bar_l4t_size || bar_emu_sd_size || bar_emu_size) ? 8 : 0, LV_DPI / 2);
	lv_obj_align(part_info.sep_hos, part_info.bar_hos, LV_ALIGN_OUT_RIGHT_MID, -4, 0);

	// Set emuMMC blending separator sizes and realign.
	lv_obj_set_size(part_info.sep_emu, bar_emu_size && (bar_remaining_size || bar_and_size || bar_l4t_size || bar_emu_sd_size) ? 8 : 0, LV_DPI / 2);
	lv_obj_align(part_info.sep_emu, part_info.bar_emu, LV_ALIGN_OUT_RIGHT_MID, -4, 0);

	// Set emuSD blending separator
	lv_obj_set_size(part_info.sep_emu_sd, bar_emu_sd_size && (bar_remaining_size || bar_and_size || bar_l4t_size) ? 8 : 0, LV_DPI / 2);
	lv_obj_align(part_info.sep_emu_sd, part_info.bar_emu_sd, LV_ALIGN_OUT_RIGHT_MID, -4, 0);

	// Set L4T blending separator sizes and realign.
	lv_obj_set_size(part_info.sep_l4t, bar_l4t_size && (bar_remaining_size || bar_and_size) ? 8 : 0, LV_DPI / 2);
	lv_obj_align(part_info.sep_l4t, part_info.bar_l4t, LV_ALIGN_OUT_RIGHT_MID, -4, 0);

	// Set Android blending separator sizes and realign.
	lv_obj_set_size(part_info.sep_and, bar_and_size && bar_remaining_size ? 8 : 0, LV_DPI / 2);
	lv_obj_align(part_info.sep_and, part_info.bar_and, LV_ALIGN_OUT_RIGHT_MID, -4, 0);
}

static lv_res_t _action_slider_hos(lv_obj_t *slider){
	char lbl_text[64];

	u32 size = (u32)lv_slider_get_value(slider) << 10;

	if(size < (u32)part_info.hos_min_size_mb / 2){
		size = 0;
	}else if(size < (u32)part_info.hos_min_size_mb){
		size = part_info.hos_min_size_mb;
	}

	part_info.auto_assign_free_storage = size != 0;

	if(size){
		// account for alignment and 1mb for backup gpt
		size = (part_info.total_sct >> 11) - 16 - 1 - part_info.and_size - part_info.emu_size - part_info.hos_os_size - part_info.l4t_size - part_info.emu_sd_size;
	}

	part_info.hos_size = size;
	lv_slider_set_value(slider, size >> 10);


	s_printf(lbl_text, "#96FF00 %d GiB#", size >> 10);
	lv_label_set_text(part_info.lbl_hos, lbl_text);

	_update_partition_bar();

	return LV_RES_OK;
}

static lv_res_t _action_slider_hos_os(lv_obj_t *slider){
	char lbl_text[64];

	u32 user_size = (u32)lv_slider_get_value(slider) << 10;
	u32 user_size_og = part_info.hos_os_og_size - part_info.hos_sys_size_mb;

	// min. 4Gb for HOS USER
	if (user_size < 2048)
		user_size = 0;
	else if (user_size < 4096)
		user_size = 4096;
	else if(user_size >= user_size_og - 3072 && user_size <= user_size_og + 3072){
		user_size = user_size_og;
	}

	u32 hos_os_size = user_size ? (user_size + part_info.hos_sys_size_mb) : 0;
	s32 hos_size = (part_info.total_sct >> 11) - 16 - 1 - part_info.emu_size - part_info.l4t_size - part_info.and_size - hos_os_size - part_info.emu_sd_size;

	// Sanitize sizes based on new HOS OS size.
	if(!part_info.auto_assign_free_storage){
		u32 total = part_info.and_size + part_info.hos_size + part_info.emu_size + part_info.l4t_size + hos_os_size + part_info.emu_sd_size;
		if(total > (part_info.total_sct >> 11) - 16 - 1){
			hos_os_size = (part_info.total_sct >> 11) - 16 - 1 - part_info.l4t_size - part_info.and_size - part_info.emu_size - part_info.hos_size - part_info.emu_sd_size;
			lv_slider_set_value(slider, (hos_os_size - part_info.hos_sys_size_mb) >> 10);
		}
	}else if (hos_size > part_info.hos_min_size_mb)
	{
		if (user_size <= 4096)
			lv_slider_set_value(slider, user_size >> 10);
	}
	else
	{
		hos_os_size = (part_info.total_sct >> 11) - 16 - 1 - part_info.emu_size - part_info.l4t_size - part_info.and_size - part_info.hos_min_size_mb - part_info.emu_sd_size;
		hos_size = (part_info.total_sct >> 11) - 16 - 1 - part_info.emu_size - part_info.l4t_size - part_info.and_size - hos_os_size - part_info.emu_sd_size;
		if (hos_size < part_info.hos_min_size_mb || hos_os_size < part_info.hos_sys_size_mb + 4096)
		{
			lv_slider_set_value(slider, (part_info.hos_os_size - part_info.hos_sys_size_mb) >> 10);
			goto out;
		}
		user_size = hos_os_size - part_info.hos_sys_size_mb;
		lv_slider_set_value(slider, user_size >> 10);
	}

	part_info.hos_os_size = hos_os_size;

	if(part_info.auto_assign_free_storage){
		part_info.hos_size = hos_size;
		s_printf(lbl_text, "#96FF00 %d GiB#", hos_size >> 10);
		lv_label_set_text(part_info.lbl_hos, lbl_text);
		lv_bar_set_value(part_info.slider_bar_hos, hos_size >> 10);
	}

	if(user_size == user_size_og){
		s_printf(lbl_text, "#FFD300 %d FULL#", user_size >> 10);
	}else{
		s_printf(lbl_text, "#FFD300 %d GiB#", user_size >> 10);
	}
	lv_label_set_text(part_info.lbl_hos_os, lbl_text);

	_update_partition_bar();

out:
	return LV_RES_OK;
}

static lv_res_t _action_slider_emu(lv_obj_t *slider)
{
	#define EMUMMC_32GB_FULL 29856
	#define EMUMMC_64GB_FULL 59664

	static const u32 rsvd_mb = 4 + 4 + 16 + 8; // BOOT0 + BOOT1 + 16MB offset + 8MB alignment.
	u32 max_emmc_size = !part_info.emmc_is_64gb ? EMUMMC_32GB_FULL : EMUMMC_64GB_FULL;

	u32 size;
	char lbl_text[64];
	int slide_val = lv_slider_get_value(slider);

	int max_slider = lv_slider_get_max_value(slider);

	size = slide_val > (max_slider / 2) ? slide_val - (max_slider / 2) : slide_val;
	size <<= 10;

	bool is_full = false;

	// min 4Gb for emuMMC
	if(size < 4096 / 2){
		size = 0;
	}else if(size < 4096){
		size = 4096;
	}else if(size <= max_emmc_size + 3072 && size >= max_emmc_size - 3072){
		size = max_emmc_size;
		is_full = true;
	}

	bool emu_double = slide_val > max_slider / 2 && size;

	if(size){
		size += rsvd_mb; // Add reserved size.
	}

	if(emu_double){
		size *= 2;
	}

	// Sanitize sizes based on new HOS size.
	s32 hos_size = (part_info.total_sct >> 11) - 16 - 1 - size - part_info.emu_sd_size - part_info.l4t_size - part_info.and_size - part_info.hos_os_size;
	u32 total = part_info.l4t_size + part_info.and_size + part_info.hos_size + part_info.hos_os_size + part_info.emu_sd_size + size;

	if ((part_info.auto_assign_free_storage && hos_size > part_info.hos_min_size_mb) || (!part_info.auto_assign_free_storage && total <= (part_info.total_sct >> 11) - 16 - 1))
	{
		part_info.emu_size = size;
		part_info.emu_double	 = emu_double;

		u32 temp_size = emu_double ? size / 2 : size;
		// set slider value again manually if in the snapping region
		if(temp_size <= 4096){
			u32 new_val = part_info.emu_double ? ((max_slider << 10) + part_info.emu_size) / 2 : part_info.emu_size;
			lv_slider_set_value(slider, new_val >> 10);
		}

		if(part_info.auto_assign_free_storage){
			part_info.hos_size = hos_size;
			s_printf(lbl_text, "#96FF00 %d GiB#", hos_size >> 10);
			lv_label_set_text(part_info.lbl_hos, lbl_text);
			lv_bar_set_value(part_info.slider_bar_hos, hos_size >> 10);
		}

		if (!emu_double)
		{
			if(is_full){
				s_printf(lbl_text, "#FF3C28 %d FULL#", size >> 10);
			}else{
				s_printf(lbl_text, "#FF3C28 %d GiB#", size >> 10);
			}
		}else{
			if(is_full){
				s_printf(lbl_text, "#FFDD00 2x##FF3C28 %d FULL#", size >> 11);
			}else{
				s_printf(lbl_text, "#FFDD00 2x##FF3C28 %d GiB#", size >> 11);
			}
		}
		lv_label_set_text(part_info.lbl_emu, lbl_text);
	}
	else
	{
		// reset slider to old value
		u32 old_val = part_info.emu_double ? ((max_slider << 10) + part_info.emu_size) / 2 : part_info.emu_size;
		lv_slider_set_value(slider, old_val >> 10);
	}

	_update_partition_bar();

	return LV_RES_OK;
}

static lv_res_t _action_slider_emu_sd(lv_obj_t *slider){
	u32 size;
	char lbl_text[64];
	int slide_val = lv_slider_get_value(slider);

	int max_slider = lv_slider_get_max_value(slider);

	size = slide_val > (max_slider / 2) ? slide_val - (max_slider / 2) : slide_val;
	size <<= 10;

	bool emu_sd_double = slide_val > max_slider / 2;

	if(size < (u32)part_info.hos_min_size_mb / 2){
		size = 0;
	}else if(size < (u32)part_info.hos_min_size_mb){
		size = part_info.hos_min_size_mb;
	}

	if(emu_sd_double){
		size *= 2;
	}

	// Sanitize sizes based on new HOS size.
	s32 hos_size = (part_info.total_sct >> 11) - 16 - 1 - size - part_info.emu_size	- part_info.l4t_size - part_info.and_size - part_info.hos_os_size;
	u32 total = part_info.l4t_size + part_info.and_size + part_info.hos_size + part_info.hos_os_size + part_info.emu_size + size;

	if ((part_info.auto_assign_free_storage && hos_size > part_info.hos_min_size_mb) || (!part_info.auto_assign_free_storage && total <= (part_info.total_sct >> 11) - 16 - 1))
	{
		part_info.emu_sd_size = size;
		part_info.emu_sd_double	 = emu_sd_double;

		u32 temp_size = emu_sd_double ? size / 2 : size;
		if(temp_size <= (u32)part_info.hos_min_size_mb){
			u32 new_val = part_info.emu_sd_double ? ((max_slider << 10) + part_info.emu_sd_size) / 2 : part_info.emu_sd_size;
			lv_slider_set_value(slider, new_val >> 10);
		}

		if(part_info.auto_assign_free_storage){
			part_info.hos_size = hos_size;
			s_printf(lbl_text, "#96FF00 %d GiB#", hos_size >> 10);
			lv_label_set_text(part_info.lbl_hos, lbl_text);
			lv_bar_set_value(part_info.slider_bar_hos, hos_size >> 10);
		}

		if (!emu_sd_double)
		{
			s_printf(lbl_text, "#ff00d6 %d GiB#", size >> 10);
		}else{
			s_printf(lbl_text, "#FFDD00 2x##ff00d6 %d GiB#", size >> 11);
		}
		lv_label_set_text(part_info.lbl_emu_sd, lbl_text);
	}
	else
	{
		// reset slider to old value
		u32 old_val = part_info.emu_sd_double ? ((max_slider << 10) + part_info.emu_sd_size) / 2 : part_info.emu_sd_size;
		lv_slider_set_value(slider, old_val >> 10);
	}

	_update_partition_bar();

	return LV_RES_OK;
}

static lv_res_t _action_slider_l4t(lv_obj_t *slider)
{
	char lbl_text[64];

	u32 size = (u32)lv_slider_get_value(slider) << 10;
	if (size < 4096)
		size = 0;
	else if (size < 8192)
		size = 8192;

	s32 hos_size = (part_info.total_sct >> 11) - 16 - 1 - part_info.hos_os_size - part_info.emu_size - size - part_info.and_size - part_info.emu_sd_size;

	// Sanitize sizes based on new HOS size.
	if(!part_info.auto_assign_free_storage){
		u32 total = part_info.and_size + part_info.hos_os_size + part_info.emu_size + part_info.hos_size + size + part_info.emu_sd_size;
		if(total > (part_info.total_sct >> 11) - 16 - 1){
			size = (part_info.total_sct >> 11) - 16 - 1 - part_info.hos_os_size - part_info.and_size - part_info.emu_size - part_info.hos_size - part_info.emu_sd_size;
			lv_slider_set_value(slider, size >> 10);
		}
	}else if (hos_size > part_info.hos_min_size_mb)
	{
		if (size <= 8192)
			lv_slider_set_value(slider, size >> 10);
	}
	else
	{
		size = (part_info.total_sct >> 11) - 16 - 1 - part_info.emu_size - part_info.hos_os_size - part_info.and_size - part_info.emu_sd_size - 2048;
		hos_size = (part_info.total_sct >> 11) - 16 - 1 - part_info.emu_size - part_info.hos_os_size - part_info.and_size - size - part_info.emu_sd_size;
		if (hos_size < part_info.hos_min_size_mb || size < 8192)
		{
			lv_slider_set_value(slider, part_info.l4t_size >> 10);
			goto out;
		}
		lv_slider_set_value(slider, size >> 10);
	}

	if(part_info.auto_assign_free_storage){
		part_info.hos_size = hos_size;
		s_printf(lbl_text, "#96FF00 %d GiB#", hos_size >> 10);
		lv_label_set_text(part_info.lbl_hos, lbl_text);
		lv_bar_set_value(part_info.slider_bar_hos, hos_size >> 10);
	}

	part_info.l4t_size = size;

	s_printf(lbl_text, "#00DDFF %d GiB#", size >> 10);
	lv_label_set_text(part_info.lbl_l4t, lbl_text);

	_update_partition_bar();

out:
	return LV_RES_OK;
}

static lv_res_t _action_slider_and(lv_obj_t *slider)
{
	u32 user_size;
	u32 and_size;
	char lbl_text[64];
	int slide_val = lv_slider_get_value(slider);

	int max_slider = lv_slider_get_max_value(slider);

	#ifdef ENABLE_DUAL_ANDROID
	user_size = slide_val > (max_slider / 2) ? slide_val - (max_slider / 2) : slide_val;
	#else
	user_size = slide_val;
	#endif
	user_size <<= 10;

	if(user_size < 4096 / 2){
		user_size = 0;
	}else if(user_size < 4096){
		user_size = 4096;
	}

	#ifdef ENABLE_DUAL_ANDROID
	bool and_double = slide_val > max_slider / 2 && user_size;
	#else
	bool and_double = false;
	#endif

	and_size = 0;
	if(user_size){
		and_size = user_size + ANDROID_SYSTEM_SIZE_MB;
	}

	if(and_double){
		and_size *= 2;
	};

	// Sanitize sizes based on new HOS size.
	s32 hos_size = (part_info.total_sct >> 11) - 16 - 1 - and_size - part_info.hos_os_size - part_info.emu_size- part_info.l4t_size - part_info.emu_sd_size ;
	u32 total = part_info.l4t_size + part_info.emu_sd_size + part_info.hos_size + part_info.hos_os_size + part_info.emu_size + and_size;

	if ((part_info.auto_assign_free_storage && hos_size > part_info.hos_min_size_mb) || (!part_info.auto_assign_free_storage && total <= (part_info.total_sct >> 11) - 16 - 1))
	{
		part_info.and_size = and_size;
		part_info.and_double = and_double;

		if(user_size <= 4096){
			u32 new_val = part_info.and_double ? (max_slider << 10) / 2 + user_size : user_size;
			lv_slider_set_value(slider, new_val >> 10);
		}

		if(part_info.auto_assign_free_storage){
			part_info.hos_size = hos_size;
			s_printf(lbl_text, "#96FF00 %d GiB#", hos_size >> 10);
			lv_label_set_text(part_info.lbl_hos, lbl_text);
			lv_bar_set_value(part_info.slider_bar_hos, hos_size >> 10);
		}

		if (!and_double)
		{
			s_printf(lbl_text, "#FF8000 %d GiB#", user_size >> 10);
		}else{
			s_printf(lbl_text, "#FFDD00 2x##FF8000 %d GiB#", user_size >> 10);
		}
		lv_label_set_text(part_info.lbl_and, lbl_text);
	}
	else
	{
		// reset slider to old value
		u32 old_val = part_info.and_double ? ((max_slider << 10) + part_info.and_size) / 2 : part_info.and_size;
		old_val -= ANDROID_SYSTEM_SIZE_MB;
		lv_slider_set_value(slider, old_val >> 10);
	}

	_update_partition_bar();

	return LV_RES_OK;
}

static lv_res_t _mbox_check_files_total_size_option(lv_obj_t *btns, const char *txt)
{
	// If "don't backup" button was pressed, disable backup/restore of files.
	if (!lv_btnm_get_pressed(btns))
		part_info.backup_possible = false;

	mbox_action(btns, txt);

	return LV_RES_INV;
}

static void _create_mbox_check_files_total_size(u8 drive)
{
	static lv_style_t bar_hos_os_ind, bar_hos_ind, bar_emu_ind, bar_l4t_ind, bar_and_ind, bar_emu_sd_ind, bar_remaining_ind;
	static lv_style_t sep_hos_os_bg, sep_hos_bg, sep_emu_bg, sep_l4t_bg, sep_emu_sd_bg, sep_and_bg;

	// Set HOS bar style.
	lv_style_copy(&bar_hos_ind, lv_theme_get_current()->bar.indic);
	bar_hos_ind.body.main_color = LV_COLOR_HEX(0x96FF00);
	bar_hos_ind.body.grad_color = bar_hos_ind.body.main_color;

	// Set emuMMC bar style.
	lv_style_copy(&bar_emu_ind, lv_theme_get_current()->bar.indic);
	bar_emu_ind.body.main_color = LV_COLOR_HEX(0xFF3C28);
	bar_emu_ind.body.grad_color = bar_emu_ind.body.main_color;

	// Set L4T bar style.
	lv_style_copy(&bar_l4t_ind, lv_theme_get_current()->bar.indic);
	bar_l4t_ind.body.main_color = LV_COLOR_HEX(0x00DDFF);
	bar_l4t_ind.body.grad_color = bar_l4t_ind.body.main_color;

	// Set Android bar style.
	lv_style_copy(&bar_and_ind, lv_theme_get_current()->bar.indic);
	bar_and_ind.body.main_color = LV_COLOR_HEX(0xff8000);
	bar_and_ind.body.grad_color = bar_and_ind.body.main_color;

	// Set HOS OS bar style.
	lv_style_copy(&bar_hos_os_ind, lv_theme_get_current()->bar.indic);
	bar_hos_os_ind.body.main_color = LV_COLOR_HEX(0xffd300);
	bar_hos_os_ind.body.grad_color = bar_hos_os_ind.body.main_color;

	// Set Remaining bar style.
	lv_style_copy(&bar_remaining_ind, lv_theme_get_current()->bar.indic);
	bar_remaining_ind.body.main_color = LV_COLOR_HEX(0xc9c9c9);
	bar_remaining_ind.body.grad_color = bar_remaining_ind.body.main_color;

	// Set emu sd bar style.
	lv_style_copy(&bar_emu_sd_ind, lv_theme_get_current()->bar.indic);
	bar_emu_sd_ind.body.main_color = LV_COLOR_HEX(0xff00d6);
	bar_emu_sd_ind.body.grad_color = bar_emu_sd_ind.body.main_color;

	// Set separator styles.
	lv_style_copy(&sep_hos_os_bg, lv_theme_get_current()->cont);
	sep_hos_os_bg.body.main_color = LV_COLOR_HEX(0xc9c9c9);
	sep_hos_os_bg.body.grad_color = sep_hos_os_bg.body.main_color;
	sep_hos_os_bg.body.radius = 0;

	lv_style_copy(&sep_hos_bg, &sep_hos_os_bg);
	sep_hos_bg.body.main_color = LV_COLOR_HEX(0x96FF00);
	sep_hos_bg.body.grad_color = sep_hos_bg.body.main_color;

	lv_style_copy(&sep_and_bg, &sep_hos_os_bg);
	sep_and_bg.body.main_color = LV_COLOR_HEX(0xff8000);
	sep_and_bg.body.grad_color = sep_and_bg.body.main_color;

	lv_style_copy(&sep_emu_bg, &sep_hos_os_bg);
	sep_emu_bg.body.main_color = LV_COLOR_HEX(0xFF3C28);
	sep_emu_bg.body.grad_color = sep_emu_bg.body.main_color;

	lv_style_copy(&sep_l4t_bg, &sep_hos_os_bg);
	sep_l4t_bg.body.main_color = LV_COLOR_HEX(0x00DDFF);
	sep_l4t_bg.body.grad_color = sep_l4t_bg.body.main_color;

	lv_style_copy(&sep_emu_sd_bg, &sep_hos_os_bg);
	sep_emu_sd_bg.body.main_color = LV_COLOR_HEX(0xff00d6);
	sep_emu_sd_bg.body.grad_color = sep_emu_sd_bg.body.main_color;

	char *txt_buf = malloc(SZ_8K);

	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *mbox_btn_map[] = { "\251", "\222OK", "\251", "" };
	static const char *mbox_btn_map2[] = { "\222Don't Backup", "\222OK", "" };
	lv_obj_t *mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 6);

	lv_mbox_set_text(mbox, drive == DRIVE_SD ? "Analyzing SD card usage. This might take a while..." : "Analyzing eMMC usage. This might take a while...");

	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);
	manual_system_maintenance(true);

	char *path = malloc(0x1000);
	u32 total_files = 0;
	u32 total_size = 0;
	path[0] = 0;

	// Check total size of files.
	int res = _stat_and_copy_files(drive == DRIVE_SD ? "sd:" : "emmc:", NULL, path, &total_files, &total_size, NULL);

	if(res == FR_NO_FILESYSTEM){
		// no fat system on selected storage, nothing to backup
		part_info.skip_backup = true;
	}

	// Not more than 1.0GB.
	part_info.backup_possible = !res && !(total_size > (RAM_DISK_SZ - SZ_16M));

	if (part_info.backup_possible)
	{
		s_printf(txt_buf,
			"#96FF00 The %s files will be backed up automatically!#\n"
			"#FFDD00 Any other partitions %swill be wiped!#\n"
			"#00DDFF Total files:# %d, #00DDFF Total size:# %d MiB", 
			drive == DRIVE_SD ? "SD card" : "eMMC", 
			drive == DRIVE_SD ? "" : "(except HOS ones) ", 
			total_files, 
			total_size >> 20);
		lv_mbox_set_text(mbox, txt_buf);
	}
	else
	{
		if(res){
			s_printf(txt_buf,
				"#96FF00 No %s files to be backed up!#\n"
				"#FFDD00 Any other partitions %swill be wiped!#\n", 
				drive == DRIVE_SD ? "SD card" : "eMMC", drive == DRIVE_SD ? "" : "(except HOS ones) ");
		}else{
			s_printf(txt_buf,
				"#FFDD00 The %s files cannot be backed up automatically!#\n"
				"#FFDD00 Any other partitions %swill be wiped!#\n\n"
				"You will be asked to back up your files later via UMS.",
				drive == DRIVE_SD ? "SD card" : "eMMC", 
				drive == DRIVE_SD ? "" : "(except HOS ones) ");
		}
		lv_mbox_set_text(mbox, txt_buf);
	}

	// Create container to keep content inside.
	lv_obj_t *h1 = lv_cont_create(mbox, NULL);
	lv_cont_set_fit(h1, false, true);
	lv_cont_set_style(h1, &lv_style_transp_tight);
	lv_obj_set_width(h1, lv_obj_get_width(mbox) - LV_DPI * 3);


	u32 bar_hos_size       = 0;
	u32 bar_emu_size       = 0;
	u32 bar_l4t_size       = 0;
	u32 bar_and_size       = 0;
	u32 bar_hos_os_size    = 0;
	u32 bar_remaining_size = 0;
	u32 bar_emu_sd_size    = 0;
	mbr_t *mbr = zalloc(sizeof(*mbr));
	gpt_t *gpt = NULL;
	bool has_gpt = false;

	sdmmc_storage_t *storage = drive == DRIVE_SD ? &sd_storage : &emmc_storage;
	total_size = storage->sec_cnt;

	// Read current MBR.
	sdmmc_storage_read(storage, 0, 1, mbr);

	// check if we have gpt
	has_gpt = _has_gpt(mbr);

	lv_obj_t *lbl_part = lv_label_create(h1, NULL);
	lv_label_set_recolor(lbl_part, true);
	s_printf(txt_buf, "#00DDFF Current %s partition layout:#", has_gpt ? "GPT" : "MBR");
	lv_label_set_text(lbl_part, txt_buf);
	
	if(!has_gpt){
		// Calculate MBR partitions size.
		bar_hos_size = mbr->partitions[0].size_sct;
		for (u32 i = 1; i < 4; i++)
			if (mbr->partitions[i].type == 0xE0)
				bar_emu_size += mbr->partitions[i].size_sct;

		for (u32 i = 1; i < 4; i++)
			if (mbr->partitions[i].type == 0x83)
				bar_l4t_size += mbr->partitions[i].size_sct;
	}else{
		// Calculate GPT part size.
		gpt = zalloc(sizeof(*gpt));

		sdmmc_storage_read(storage, 1, sizeof(*gpt) >> 9, gpt);

		u32 i = 0;
		if(!memcmp(gpt->entries[10].name, (char[]){'U', 0, 'S', 0, 'E', 0, 'R', 0}, 7)){
			bar_hos_os_size += gpt->entries[10].lba_end - gpt->entries[0].lba_start + 1;
			i = 11;
		}

		for(; i < gpt->header.num_part_ents && i < 128; i++){
			gpt_entry_t *entry = &gpt->entries[i];

			if(!memcmp(entry->name, (char[]){ 'e', 0, 'm', 0, 'u', 0, 'm', 0, 'm', 0, 'c', 0 }, 12)){
				bar_emu_size += entry->lba_end - entry->lba_start + 1;
			}

			if(!memcmp(entry->name, (char[]){ 'b', 0, 'o', 0, 'o', 0, 't', 0 }, 8)){
				if((i + 6) < gpt->header.num_part_ents && (i + 6) < 128){
					if(!memcmp(gpt->entries[i + 6].name, (char[]){ 'u', 0, 's', 0, 'e', 0, 'r', 0, 'd', 0, 'a', 0, 't', 0, 'a', 0 }, 16)){
						// found android dynamic
						bar_and_size += gpt->entries[i + 6].lba_end - gpt->entries[i].lba_start + 1;
						i += 6;
					}
				}
			}

			if(!memcmp(entry->name, (char[]){ 'v', 0, 'e', 0, 'n', 0, 'd', 0, 'o', 0, 'r', 0 }, 12)){
				if(i + 8 < gpt->header.num_part_ents && i + 8 < 128){
					if(!memcmp(gpt->entries[i + 8].name, (char[]){ 'U', 0, 'D', 0, 'A', 0 }, 6)){
						// found android regular
						bar_and_size += gpt->entries[i + 8].lba_end - gpt->entries[i].lba_start + 1;
						i += 8;
					}
				}
			}

			if(!memcmp(entry->name, (char[]){ 'l', 0, '4', 0, 't', 0 }, 6)){
				bar_l4t_size += entry->lba_end - entry->lba_start + 1;
			}

			if(!memcmp(entry->name, (char[]){ 'h', 0, 'o', 0, 's', 0, '_', 0, 'd', 0, 'a', 0, 't', 0, 'a', 0 }, 16)){
				bar_hos_size += entry->lba_end - entry->lba_start + 1;
			}

			if(!memcmp(entry->name, (char[]){ 'e', 0, 'm', 0, 'u', 0, 's', 0, 'd', 0, '_', 0, 'm', 0, 'b', 0, 'r', 0 }, 16)){
				if(!memcmp(entry->name, (char[]){ 'e', 0, 'm', 0, 'u', 0, 's', 0, 'd', 0 }, 16)){
					bar_emu_sd_size += gpt->entries[i + 1].lba_end - gpt->entries[i].lba_start + 1;
					i++;
				}
			}
		}
	}

	bar_remaining_size = total_size - (bar_l4t_size + bar_and_size + bar_hos_os_size + bar_hos_size + bar_emu_size + bar_emu_sd_size);

	lv_coord_t w = lv_obj_get_width(h1);
	bar_remaining_size = w * (bar_remaining_size / SECTORS_PER_GB) / (total_size / SECTORS_PER_GB);
	bar_remaining_size = bar_remaining_size <= 7 ? 0 : bar_remaining_size;
	bar_l4t_size       = w * (bar_l4t_size       / SECTORS_PER_GB) / (total_size / SECTORS_PER_GB);
	bar_and_size       = w * (bar_and_size       / SECTORS_PER_GB) / (total_size / SECTORS_PER_GB);
	bar_hos_os_size    = w * (bar_hos_os_size    / SECTORS_PER_GB) / (total_size / SECTORS_PER_GB);
	bar_hos_size       = w * (bar_hos_size       / SECTORS_PER_GB) / (total_size / SECTORS_PER_GB);
	bar_emu_size       = w * (bar_emu_size       / SECTORS_PER_GB) / (total_size / SECTORS_PER_GB);
	bar_emu_sd_size    = w * (bar_emu_sd_size    / SECTORS_PER_GB) / (total_size / SECTORS_PER_GB);

	// Create HOS OS bar.
	lv_obj_t *bar_hos_os = lv_bar_create(h1, NULL);
	lv_obj_set_size(bar_hos_os, bar_hos_os_size, LV_DPI / 3);
	lv_bar_set_range(bar_hos_os, 0, 1);
	lv_bar_set_value(bar_hos_os, 1);
	lv_bar_set_style(bar_hos_os, LV_BAR_STYLE_INDIC, &bar_hos_os_ind);
	lv_obj_align(bar_hos_os, lbl_part, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 6);

	// Create HOS bar.
	lv_obj_t *bar_hos = lv_bar_create(h1, bar_hos_os);
	lv_obj_set_size(bar_hos, bar_hos_size, LV_DPI / 3);
	lv_bar_set_style(bar_hos, LV_BAR_STYLE_INDIC, &bar_hos_ind);
	lv_obj_align(bar_hos, bar_hos_os, LV_ALIGN_OUT_RIGHT_MID, 0, 0);

	// Create emuMMC bar.
	lv_obj_t *bar_emu = lv_bar_create(h1, bar_hos_os);
	lv_obj_set_size(bar_emu, bar_emu_size, LV_DPI / 3);
	lv_bar_set_style(bar_emu, LV_BAR_STYLE_INDIC, &bar_emu_ind);
	lv_obj_align(bar_emu, bar_hos, LV_ALIGN_OUT_RIGHT_MID, 0, 0);

	// Create emuSD bar.
	lv_obj_t *bar_emu_sd = lv_bar_create(h1, bar_hos_os);
	lv_obj_set_size(bar_emu_sd, bar_emu_sd_size, LV_DPI / 3);
	lv_bar_set_style(bar_emu_sd, LV_BAR_STYLE_INDIC, &bar_emu_sd_ind);
	lv_obj_align(bar_emu_sd, bar_emu, LV_ALIGN_OUT_RIGHT_MID, 0, 0);

	// Create L4T bar.
	lv_obj_t *bar_l4t = lv_bar_create(h1, bar_hos_os);
	lv_obj_set_size(bar_l4t, bar_l4t_size, LV_DPI / 3);
	lv_bar_set_style(bar_l4t, LV_BAR_STYLE_INDIC, &bar_l4t_ind);
	lv_obj_align(bar_l4t, bar_emu_sd, LV_ALIGN_OUT_RIGHT_MID, 0, 0);

	// Create android bar.
	lv_obj_t *bar_and = lv_bar_create(h1, bar_hos_os);
	lv_obj_set_size(bar_and, bar_and_size, LV_DPI / 3);
	lv_bar_set_style(bar_and, LV_BAR_STYLE_INDIC, &bar_and_ind);
	lv_obj_align(bar_and, bar_l4t, LV_ALIGN_OUT_RIGHT_MID, 0, 0);

	//Create Remaining bar.
	lv_obj_t *bar_remaining = lv_bar_create(h1, bar_hos_os);
	lv_obj_set_size(bar_remaining, bar_remaining_size, LV_DPI / 3);
	lv_bar_set_style(bar_remaining, LV_BAR_STYLE_INDIC, &bar_remaining_ind);
	lv_obj_align(bar_remaining, bar_and, LV_ALIGN_OUT_RIGHT_MID, 0, 0);


	// Create HOS OS separator.
	lv_obj_t *sep_hos_os = lv_cont_create(h1, NULL);
	lv_obj_set_size(sep_hos_os, bar_hos_os_size && (bar_emu_sd_size || bar_and_size || bar_remaining_size || bar_l4t_size || bar_emu_size || bar_hos_size) ? 8 : 0, LV_DPI / 3);
	lv_obj_set_style(sep_hos_os, &sep_hos_os_bg);
	lv_obj_align(sep_hos_os, bar_hos_os, LV_ALIGN_OUT_RIGHT_MID, -4, 0);

	// Create HOS separator.
	lv_obj_t *sep_hos = lv_cont_create(h1, NULL);
	lv_obj_set_size(sep_hos, bar_hos_size && (bar_emu_sd_size || bar_and_size || bar_remaining_size || bar_l4t_size || bar_emu_size)? 8 : 0, LV_DPI / 3);
	lv_obj_set_style(sep_hos, &sep_hos_bg);
	lv_obj_align(sep_hos, bar_hos, LV_ALIGN_OUT_RIGHT_MID, -4, 0);

	// Create emuMMC separator.
	lv_obj_t *sep_emu = lv_cont_create(h1, NULL);
	lv_obj_set_size(sep_emu, bar_emu_size && (bar_emu_sd_size || bar_and_size || bar_remaining_size || bar_l4t_size)? 8 : 0, LV_DPI / 3);
	lv_obj_set_style(sep_emu, &sep_emu_bg);
	lv_obj_align(sep_emu, bar_emu, LV_ALIGN_OUT_RIGHT_MID, -4, 0);

	// Create emuSD separator.
	lv_obj_t *sep_emu_sd = lv_cont_create(h1, NULL);
	lv_obj_set_size(sep_emu_sd, bar_emu_sd_size && (bar_emu_size || bar_and_size || bar_remaining_size || bar_l4t_size)? 8 : 0, LV_DPI / 3);
	lv_obj_set_style(sep_emu_sd, &sep_emu_sd_bg);
	lv_obj_align(sep_emu_sd, bar_emu_sd, LV_ALIGN_OUT_RIGHT_MID, -4, 0);

	// Create L4T separator.
	lv_obj_t *sep_l4t = lv_cont_create(h1, NULL);
	lv_obj_set_size(sep_l4t, bar_l4t_size && (bar_and_size || bar_remaining_size) ? 8 : 0, LV_DPI / 3);
	lv_obj_set_style(sep_l4t, &sep_l4t_bg);
	lv_obj_align(sep_l4t, bar_l4t, LV_ALIGN_OUT_RIGHT_MID, -4, 0);

	// Create Android separator.
	lv_obj_t *sep_and = lv_cont_create(h1, NULL);
	lv_obj_set_size(sep_and, bar_and_size && bar_remaining_size ? 8 : 0, LV_DPI / 3);
	lv_obj_set_style(sep_and, &sep_and_bg);
	lv_obj_align(sep_and, bar_and, LV_ALIGN_OUT_RIGHT_MID, -4, 0);

	// Print partition table info.
	if(!has_gpt){
		// print mbr table
		s_printf(txt_buf,
			"Part. 0 - Type: %02x, Start: %08x, Size: %08x\n"
			"Part. 1 - Type: %02x, Start: %08x, Size: %08x\n"
			"Part. 2 - Type: %02x, Start: %08x, Size: %08x\n"
			"Part. 3 - Type: %02x, Start: %08x, Size: %08x",
			mbr->partitions[0].type, mbr->partitions[0].start_sct, mbr->partitions[0].size_sct,
			mbr->partitions[1].type, mbr->partitions[1].start_sct, mbr->partitions[1].size_sct,
			mbr->partitions[2].type, mbr->partitions[2].start_sct, mbr->partitions[2].size_sct,
			mbr->partitions[3].type, mbr->partitions[3].start_sct, mbr->partitions[3].size_sct);
	}else{
		strcpy(txt_buf, "");
		for(u32 i = 0; i < gpt->header.num_part_ents && i < 128; i++){
			char txt_buf2[36];
			_wctombs((u16*)&gpt->entries[i].name, txt_buf2, 36);
			if(gpt->header.num_part_ents > 9){
				s_printf(txt_buf + strlen(txt_buf), "Part. %02d - Name : %s\n           Start: %08x, Size: %08x%c", i, txt_buf2, (u32)gpt->entries[i].lba_start, (u32)(gpt->entries[i].lba_end - gpt->entries[i].lba_start + 1), i == gpt->header.num_part_ents || i == 127 ? '\0' : '\n');
			}else{
				s_printf(txt_buf + strlen(txt_buf), "Part. %d - Name: %s\n          Start: %08x, Size: %08x%c", i, txt_buf2, (u32)gpt->entries[i].lba_start, (u32)(gpt->entries[i].lba_end - gpt->entries[i].lba_start + 1), i == gpt->header.num_part_ents || i == 127 ? '\0' : '\n');
			}
		}
	}


	lv_obj_t *ta_table = lv_ta_create(h1, NULL);
	lv_ta_set_cursor_type(ta_table, LV_CURSOR_NONE);
	lv_ta_set_text_align(ta_table, LV_LABEL_ALIGN_LEFT);
	lv_ta_set_sb_mode(ta_table, LV_SB_MODE_AUTO);
	lv_ta_set_style(ta_table, LV_TA_STYLE_BG, &monospace_text);
	lv_obj_set_size(ta_table, w, w * 2 / 7);
	lv_ta_set_text(ta_table, txt_buf);
	lv_obj_align(ta_table, h1, LV_ALIGN_IN_TOP_MID, 0, LV_DPI);


	if (!part_info.backup_possible)
		lv_mbox_add_btns(mbox, mbox_btn_map, mbox_action);
	else
		lv_mbox_add_btns(mbox, mbox_btn_map2, _mbox_check_files_total_size_option);

	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);

	free(txt_buf);
	free(path);
	free(mbr);
	free(gpt);
}

static lv_res_t _action_fix_mbr(lv_obj_t *btn)
{
	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	static const char *mbox_btn_map[] = { "\251", "\222OK", "\251", "" };
	lv_obj_t * mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);

	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 6);
	lv_mbox_set_text(mbox, "#FF8000 Fix Hybrid MBR#");

	lv_obj_t *lbl_status = lv_label_create(mbox, NULL);
	lv_label_set_recolor(lbl_status, true);

	mbr_t mbr[2] = { 0 };
	gpt_t *gpt = zalloc(sizeof(gpt_t));
	gpt_header_t gpt_hdr_backup = { 0 };

	bool has_mbr_attributes = false;
	bool hybrid_mbr_changed = false;
	bool gpt_partition_exists = false;

	// Try to init sd card. No need for valid MBR.
	if (!sd_mount() && !sd_get_card_initialized())
	{
		lv_label_set_text(lbl_status, "#FFDD00 Failed to init SD!#");
		goto out;
	}

	sdmmc_storage_read(&sd_storage, 0, 1, &mbr[0]);
	sdmmc_storage_read(&sd_storage, 1, sizeof(gpt_t) >> 9, gpt);

	memcpy(&mbr[1], &mbr[0], sizeof(mbr_t));

	sd_unmount();

	// Check for secret MBR attributes.
	if (gpt->entries[0].part_guid[7])
		has_mbr_attributes = true;

	// Check if there's a GPT Protective partition.
	for (u32 i = 0; i < 4; i++)
	{
		if (mbr[0].partitions[i].type == 0xEE)
			gpt_partition_exists = true;
	}

	// Check if GPT is valid.
	if (!gpt_partition_exists || memcmp(&gpt->header.signature, "EFI PART", 8) || gpt->header.num_part_ents > 128)
	{
		lv_label_set_text(lbl_status, "#FFDD00 Warning:# No valid GPT was found!");

		gpt_partition_exists = false;

		if (has_mbr_attributes)
			goto check_changes;
		else
			goto out;
	}

	sdmmc_storage_read(&sd_storage, gpt->header.alt_lba, 1, &gpt_hdr_backup);

	// Parse GPT.
	LIST_INIT(gpt_parsed);
	for (u32 i = 0; i < gpt->header.num_part_ents; i++)
	{
		emmc_part_t *part = (emmc_part_t *)zalloc(sizeof(emmc_part_t));

		if (gpt->entries[i].lba_start < gpt->header.first_use_lba)
			continue;

		part->index = i;
		part->lba_start = gpt->entries[i].lba_start;
		part->lba_end = gpt->entries[i].lba_end;

		// ASCII conversion. Copy only the LSByte of the UTF-16LE name.
		for (u32 j = 0; j < 36; j++)
			part->name[j] = gpt->entries[i].name[j];
		part->name[35] = 0;

		list_append(&gpt_parsed, &part->link);
	}

	// Set FAT and emuMMC partitions.
	u32 mbr_idx = 1;
	bool found_hos_data = false;
	LIST_FOREACH_ENTRY(emmc_part_t, part, &gpt_parsed, link)
	{
		// FatFS simple GPT found a fat partition, set it.
		if (sd_fs.part_type && !part->index)
		{
			mbr[1].partitions[0].type = sd_fs.fs_type == FS_EXFAT ? 0x7 : 0xC;
			mbr[1].partitions[0].start_sct = part->lba_start;
			mbr[1].partitions[0].size_sct = (part->lba_end - part->lba_start + 1);
		}

		// FatFS simple GPT didn't find a fat partition as the first one.
		if (!sd_fs.part_type && !found_hos_data && !strcmp(part->name, "hos_data"))
		{
			mbr[1].partitions[0].type = 0xC;
			mbr[1].partitions[0].start_sct = part->lba_start;
			mbr[1].partitions[0].size_sct = (part->lba_end - part->lba_start + 1);
			found_hos_data = true;
		}

		// Set up to max 2 emuMMC partitions.
		if (!strcmp(part->name, "emummc") || !strcmp(part->name, "emummc2"))
		{
			mbr[1].partitions[mbr_idx].type = 0xE0;
			mbr[1].partitions[mbr_idx].start_sct = part->lba_start;
			mbr[1].partitions[mbr_idx].size_sct = (part->lba_end - part->lba_start + 1);
			mbr_idx++;
		}

		// Total reached last slot.
		if (mbr_idx >= 3)
			break;
	}

	emmc_gpt_free(&gpt_parsed);

	// Set GPT protective partition.
	mbr[1].partitions[mbr_idx].type = 0xEE;
	mbr[1].partitions[mbr_idx].start_sct = 1;
	mbr[1].partitions[mbr_idx].size_sct = sd_storage.sec_cnt - 1;

	// Check for differences.
	for (u32 i = 1; i < 4; i++)
	{
		if ((mbr[0].partitions[i].type      != mbr[1].partitions[i].type)      ||
			(mbr[0].partitions[i].start_sct != mbr[1].partitions[i].start_sct) ||
			(mbr[0].partitions[i].size_sct  != mbr[1].partitions[i].size_sct))
		{
			hybrid_mbr_changed = true;
			break;
		}
	}

check_changes:
	if (!hybrid_mbr_changed && !has_mbr_attributes)
	{
		lv_label_set_text(lbl_status, "#96FF00 Warning:# The Hybrid MBR needs no change!#");
		goto out;
	}

	char *txt_buf = malloc(SZ_16K);

	// Current MBR info.
	s_printf(txt_buf, "#00DDFF Current MBR Layout:#\n");
	s_printf(txt_buf + strlen(txt_buf),
		"Partition 0 - Type: %02x, Start: %08x, Size: %08x\n"
		"Partition 1 - Type: %02x, Start: %08x, Size: %08x\n"
		"Partition 2 - Type: %02x, Start: %08x, Size: %08x\n"
		"Partition 3 - Type: %02x, Start: %08x, Size: %08x\n\n",
		mbr[0].partitions[0].type, mbr[0].partitions[0].start_sct, mbr[0].partitions[0].size_sct,
		mbr[0].partitions[1].type, mbr[0].partitions[1].start_sct, mbr[0].partitions[1].size_sct,
		mbr[0].partitions[2].type, mbr[0].partitions[2].start_sct, mbr[0].partitions[2].size_sct,
		mbr[0].partitions[3].type, mbr[0].partitions[3].start_sct, mbr[0].partitions[3].size_sct);

	// New MBR info.
	s_printf(txt_buf + strlen(txt_buf), "#00DDFF New MBR Layout:#\n");
	s_printf(txt_buf + strlen(txt_buf),
		"Partition 0 - Type: %02x, Start: %08x, Size: %08x\n"
		"Partition 1 - Type: %02x, Start: %08x, Size: %08x\n"
		"Partition 2 - Type: %02x, Start: %08x, Size: %08x\n"
		"Partition 3 - Type: %02x, Start: %08x, Size: %08x",
		mbr[1].partitions[0].type, mbr[1].partitions[0].start_sct, mbr[1].partitions[0].size_sct,
		mbr[1].partitions[1].type, mbr[1].partitions[1].start_sct, mbr[1].partitions[1].size_sct,
		mbr[1].partitions[2].type, mbr[1].partitions[2].start_sct, mbr[1].partitions[2].size_sct,
		mbr[1].partitions[3].type, mbr[1].partitions[3].start_sct, mbr[1].partitions[3].size_sct);

	lv_label_set_text(lbl_status, txt_buf);
	lv_label_set_style(lbl_status, &monospace_text);

	free(txt_buf);

	lbl_status = lv_label_create(mbox, NULL);
	lv_label_set_recolor(lbl_status, true);
	lv_label_set_align(lbl_status, LV_LABEL_ALIGN_CENTER);

	lv_label_set_text(lbl_status,
		"#FF8000 Warning: Do you really want to continue?!#\n\n"
		"Press #FF8000 POWER# to Continue.\nPress #FF8000 VOL# to abort.");

	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);

	manual_system_maintenance(true);

	if (btn_wait() & BTN_POWER)
	{
		sd_mount();

		// Write MBR.
		if (hybrid_mbr_changed)
			sdmmc_storage_write(&sd_storage, 0, 1, &mbr[1]);

		// Fix MBR secret attributes.
		if (has_mbr_attributes)
		{
			// Clear secret attributes.
			gpt->entries[0].part_guid[7] = 0;

			if (gpt_partition_exists)
			{
				// Fix CRC32s.
				u32 entries_size = sizeof(gpt_entry_t) * gpt->header.num_part_ents;
				gpt->header.part_ents_crc32 = crc32_calc(0, (const u8 *)gpt->entries, entries_size);
				gpt->header.crc32 = 0; // Set to 0 for calculation.
				gpt->header.crc32 = crc32_calc(0, (const u8 *)&gpt->header, gpt->header.size);

				gpt_hdr_backup.part_ents_crc32 = gpt->header.part_ents_crc32;
				gpt_hdr_backup.crc32 = 0; // Set to 0 for calculation.
				gpt_hdr_backup.crc32 = crc32_calc(0, (const u8 *)&gpt_hdr_backup, gpt_hdr_backup.size);

				// Write main GPT.
				u32 aligned_entries_size = ALIGN(entries_size, SD_BLOCKSIZE);
				sdmmc_storage_write(&sd_storage, gpt->header.my_lba, (sizeof(gpt_header_t) + aligned_entries_size) >> 9, gpt);

				// Write backup GPT partition table.
				sdmmc_storage_write(&sd_storage, gpt_hdr_backup.part_ent_lba, aligned_entries_size >> 9, gpt->entries);

				// Write backup GPT header.
				sdmmc_storage_write(&sd_storage, gpt_hdr_backup.my_lba, 1, &gpt_hdr_backup);
			}
			else
			{
				// Only write the relevant sector if the only change is MBR attributes.
				sdmmc_storage_write(&sd_storage, 2, 1, &gpt->entries[0]);
			}
		}

		sd_unmount();

		lv_label_set_text(lbl_status, "#96FF00 The new Hybrid MBR was written successfully!#");
	}
	else
		lv_label_set_text(lbl_status, "#FFDD00 Warning: The Hybrid MBR Fix was canceled!#");

out:
	free(gpt);

	lv_mbox_add_btns(mbox, mbox_btn_map, mbox_action);

	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);

	return LV_RES_OK;
}

lv_res_t create_window_partition_manager(lv_obj_t *btn, u8 drive)
{
	char title_str[0x20];
	s_printf(title_str, "%s %s Partition Manager", drive == DRIVE_SD ? SYMBOL_SD : SYMBOL_CHIP, drive == DRIVE_SD ? "SD" : "eMMC");
	lv_obj_t *win = nyx_create_standard_window(title_str);

	if(drive == DRIVE_SD){
		lv_win_add_btn(win, NULL, SYMBOL_MODULES_ALT" Fix Hybrid MBR", _action_fix_mbr);
	}

	static lv_style_t bar_hos_os_bg, bar_hos_bg, bar_emu_bg, bar_l4t_bg, bar_and_bg, bar_emu_sd_bg;
	static lv_style_t bar_hos_os_ind, bar_hos_ind, bar_emu_ind, bar_l4t_ind, bar_and_ind, bar_remaining_ind, bar_emu_sd_ind;
	static lv_style_t bar_hos_os_btn, bar_hos_btn, bar_emu_btn, bar_l4t_btn, bar_and_btn, bar_emu_sd_btn;
	static lv_style_t sep_emu_bg, sep_l4t_bg, sep_and_bg, sep_hos_bg, sep_hos_os_bg, sep_emu_sd_bg;

	// Set HOS bar styles.
	lv_style_copy(&bar_hos_bg, lv_theme_get_current()->bar.bg);
	bar_hos_bg.body.main_color = LV_COLOR_HEX(0x4A8000);
	bar_hos_bg.body.grad_color = bar_hos_bg.body.main_color;
	lv_style_copy(&bar_hos_ind, lv_theme_get_current()->bar.indic);
	bar_hos_ind.body.main_color = LV_COLOR_HEX(0x96FF00);
	bar_hos_ind.body.grad_color = bar_hos_ind.body.main_color;
	lv_style_copy(&bar_hos_btn, lv_theme_get_current()->slider.knob);
	bar_hos_btn.body.main_color = LV_COLOR_HEX(0x77CC00);
	bar_hos_btn.body.grad_color = bar_hos_btn.body.main_color;
	lv_style_copy(&sep_hos_bg, lv_theme_get_current()->cont);
	sep_hos_bg.body.main_color = LV_COLOR_HEX(0x96FF00);
	sep_hos_bg.body.grad_color = sep_hos_bg.body.main_color;
	sep_hos_bg.body.radius = 0;

	// Set Remaining Space style
	lv_style_copy(&bar_remaining_ind, lv_theme_get_current()->bar.indic);
	bar_remaining_ind.body.main_color = LV_COLOR_HEX(0xc9c9c9);
	bar_remaining_ind.body.grad_color = bar_remaining_ind.body.main_color;

	// Set eMUMMC bar styles.
	lv_style_copy(&bar_emu_bg, lv_theme_get_current()->bar.bg);
	bar_emu_bg.body.main_color = LV_COLOR_HEX(0x940F00);
	bar_emu_bg.body.grad_color = bar_emu_bg.body.main_color;
	lv_style_copy(&bar_emu_ind, lv_theme_get_current()->bar.indic);
	bar_emu_ind.body.main_color = LV_COLOR_HEX(0xFF3C28);
	bar_emu_ind.body.grad_color = bar_emu_ind.body.main_color;
	lv_style_copy(&bar_emu_btn, lv_theme_get_current()->slider.knob);
	bar_emu_btn.body.main_color = LV_COLOR_HEX(0xB31200);
	bar_emu_btn.body.grad_color = bar_emu_btn.body.main_color;
	lv_style_copy(&sep_emu_bg, &sep_hos_bg);
	sep_emu_bg.body.main_color = LV_COLOR_HEX(0xFF3C28);
	sep_emu_bg.body.grad_color = sep_emu_bg.body.main_color;
	sep_emu_bg.body.radius = 0;

	// Set L4T bar styles.
	lv_style_copy(&bar_l4t_bg, lv_theme_get_current()->bar.bg);
	bar_l4t_bg.body.main_color = LV_COLOR_HEX(0x006E80);
	bar_l4t_bg.body.grad_color = bar_l4t_bg.body.main_color;
	lv_style_copy(&bar_l4t_ind, lv_theme_get_current()->bar.indic);
	bar_l4t_ind.body.main_color = LV_COLOR_HEX(0x00DDFF);
	bar_l4t_ind.body.grad_color = bar_l4t_ind.body.main_color;
	lv_style_copy(&bar_l4t_btn, lv_theme_get_current()->slider.knob);
	bar_l4t_btn.body.main_color = LV_COLOR_HEX(0x00B1CC);
	bar_l4t_btn.body.grad_color = bar_l4t_btn.body.main_color;
	lv_style_copy(&sep_l4t_bg, &sep_hos_bg);
	sep_l4t_bg.body.main_color = LV_COLOR_HEX(0x00DDFF);
	sep_l4t_bg.body.grad_color = sep_l4t_bg.body.main_color;

	// Set Android bar styles.
	lv_style_copy(&bar_and_bg, lv_theme_get_current()->bar.bg);
	bar_and_bg.body.main_color = LV_COLOR_HEX(0x804000);
	bar_and_bg.body.grad_color = bar_and_bg.body.main_color;
	lv_style_copy(&bar_and_ind, lv_theme_get_current()->bar.indic);
	bar_and_ind.body.main_color = LV_COLOR_HEX(0xFF8000);
	bar_and_ind.body.grad_color = bar_and_ind.body.main_color;
	lv_style_copy(&bar_and_btn, lv_theme_get_current()->slider.knob);
	bar_and_btn.body.main_color = LV_COLOR_HEX(0xCC6600);
	bar_and_btn.body.grad_color = bar_and_btn.body.main_color;
	lv_style_copy(&sep_and_bg, &sep_hos_bg);
	sep_and_bg.body.main_color = LV_COLOR_HEX(0xFF8000);
	sep_and_bg.body.grad_color = sep_and_bg.body.main_color;

	// Set HOS OS bar styles.
	lv_style_copy(&bar_hos_os_bg, lv_theme_get_current()->bar.bg);
	bar_hos_os_bg.body.main_color = LV_COLOR_HEX(0xb89900);
	bar_hos_os_bg.body.grad_color = bar_hos_os_bg.body.main_color;
	lv_style_copy(&bar_hos_os_ind, lv_theme_get_current()->bar.indic);
	bar_hos_os_ind.body.main_color = LV_COLOR_HEX(0xffd300);
	bar_hos_os_ind.body.grad_color = bar_hos_os_ind.body.main_color;
	lv_style_copy(&bar_hos_os_btn, lv_theme_get_current()->slider.knob);
	bar_hos_os_btn.body.main_color = LV_COLOR_HEX(0xe6bf00);
	bar_hos_os_btn.body.grad_color = bar_hos_os_btn.body.main_color;
	lv_style_copy(&sep_hos_os_bg, &sep_hos_bg);
	sep_hos_os_bg.body.main_color = LV_COLOR_HEX(0xffd300);
	sep_hos_os_bg.body.grad_color = sep_hos_os_bg.body.main_color;

	// Set emuSD bar styles.
	lv_style_copy(&bar_emu_sd_bg, lv_theme_get_current()->bar.bg);
	bar_emu_sd_bg.body.main_color = LV_COLOR_HEX(0x96007e);
	bar_emu_sd_bg.body.grad_color = bar_emu_sd_bg.body.main_color;
	lv_style_copy(&bar_emu_sd_ind, lv_theme_get_current()->bar.indic);
	bar_emu_sd_ind.body.main_color = LV_COLOR_HEX(0xff00d6);
	bar_emu_sd_ind.body.grad_color = bar_emu_sd_ind.body.main_color;
	lv_style_copy(&bar_emu_sd_btn, lv_theme_get_current()->slider.knob);
	bar_emu_sd_btn.body.main_color = LV_COLOR_HEX(0xc700a7);
	bar_emu_sd_btn.body.grad_color = bar_emu_sd_btn.body.main_color;
	lv_style_copy(&sep_emu_sd_bg, &sep_hos_bg);
	sep_emu_sd_bg.body.main_color = LV_COLOR_HEX(0xff00d6);
	sep_emu_sd_bg.body.grad_color = sep_emu_sd_bg.body.main_color;

	lv_obj_t *sep = lv_label_create(win, NULL);
	lv_label_set_static_text(sep, "");
	lv_obj_align(sep, NULL, LV_ALIGN_IN_TOP_MID, 0, 0);

	// Create container to keep content inside.
	lv_obj_t *h1 = lv_cont_create(win, NULL);
	lv_obj_set_size(h1, LV_HOR_RES - (LV_DPI * 8 / 10), LV_VER_RES - LV_DPI);


	sdmmc_storage_t *storage = drive == DRIVE_SD ? &sd_storage : &emmc_storage;
	bool res = false;
	if(drive == DRIVE_SD){
		res = sd_mount() || sd_initialize(false);
	}else{
		if(boot_storage_get_drive() == DRIVE_EMMC){
			boot_storage_unmount();
		}
		res = emmc_initialize(false);
	}

	if (!res)
	{
		lv_obj_t *lbl = lv_label_create(h1, NULL);
		lv_label_set_recolor(lbl, true);
		lv_label_set_text(lbl, drive == DRIVE_SD ? "#FFDD00 Failed to init SD!#" : "#FFDD00 Failed to init eMMC!#");
		return LV_RES_OK;
	}

	memset(&part_info, 0, sizeof(partition_ctxt_t));
	part_info.drive = drive;
	_create_mbox_check_files_total_size(drive);

	char *txt_buf = malloc(SZ_8K);

	part_info.total_sct = storage->sec_cnt;

	// Align down total size to ensure alignment of all partitions after HOS one.
	part_info.alignment = part_info.total_sct - ALIGN_DOWN(part_info.total_sct, AU_ALIGN_SECTORS);
	part_info.total_sct -= part_info.alignment;

	// Reserved 16MB for alignment
	u32 extra_sct = AU_ALIGN_SECTORS;
	if(drive == DRIVE_SD){
		 // On SD, also Reserve 2GB for FAT partition
		 extra_sct += 0x400000; 
	}

	// Set initial HOS partition size, so the correct cluster size can be selected.
	if(drive == DRIVE_SD){
		part_info.hos_size = (part_info.total_sct >> 11) - 16 - 1; // Important if there's no slider change.
	}else{
		// On eMMC, default is to not have a FAT32 partition
		part_info.hos_size = 0;
	}


	// Check if eMMC should be 64GB (Aula).
	part_info.emmc_is_64gb = fuse_read_hw_type() == FUSE_NX_HW_TYPE_AULA;
	part_info.auto_assign_free_storage = drive == DRIVE_SD ? true : false;

	part_info.hos_min_size_mb = HOS_MIN_SIZE_MB;

	// Read current MBR.
	mbr_t mbr = { 0 };
	gpt_t *gpt = NULL;
	bool has_gpt = false;
	bool has_hos_os = false;
	sdmmc_storage_read(storage, 0, 1, &mbr);
	has_gpt = _has_gpt(&mbr);
	if(has_gpt){
		gpt = zalloc(sizeof(*gpt));
		sdmmc_storage_read(storage, 1, sizeof(*gpt) >> 9, gpt);

		if(gpt->header.num_part_ents >= 10 && drive == DRIVE_EMMC){
			if(!memcmp(gpt->entries[10].name, (char[]){'U', 0, 'S', 0, 'E', 0, 'R', 0}, 8)){
				// Found HOS USER partition
				has_hos_os = true;
				// system only size (excl. user)
				// part_info.hos_sys_size_mb = (gpt->entries[10].lba_start - gpt->entries[0].lba_start) >> 11;

				// We assume first partition is 16mb aligned and starts at 0x800
				part_info.hos_sys_size_mb = (gpt->entries[10].lba_start - 0x800) >> 11;

				part_info.hos_os_size = (gpt->entries[10].lba_end - 0x800 + 1) >> 11;
				// original hos size
				part_info.hos_os_og_size = part_info.hos_os_size;
			}
		}
	}


	// account for alignment + 1mb for backup gpt
	u32 total_size         = (part_info.total_sct - AU_ALIGN_SECTORS - (1 << 11)) / SECTORS_PER_GB;
	u32 bar_emu_sd_size    = lv_obj_get_width(h1) * (part_info.emu_sd_size >> 10) / total_size;
	u32 bar_hos_size       = lv_obj_get_width(h1) * (part_info.hos_size    >> 10) / total_size;
	u32 bar_emu_size       = lv_obj_get_width(h1) * (part_info.emu_size    >> 10) / total_size;
	u32 bar_l4t_size       = lv_obj_get_width(h1) * (part_info.l4t_size    >> 10) / total_size;
	u32 bar_and_size       = lv_obj_get_width(h1) * (part_info.and_size    >> 10) / total_size;
	u32 bar_hos_os_size    = lv_obj_get_width(h1) * (part_info.hos_os_size >> 10) / total_size;
	u32 bar_remaining_size = lv_obj_get_width(h1) - (bar_hos_size + bar_hos_os_size + bar_emu_size + bar_l4t_size + bar_and_size);
	bar_remaining_size = bar_remaining_size <= 7 ? 0 : bar_remaining_size;

	lv_obj_t *lbl = lv_label_create(h1, NULL);
	lv_label_set_recolor(lbl, true);
	lv_label_set_text(lbl, "Choose #FFDD00 new# partition layout:");

	// Create disk layout blocks.
	// HOS OS partition block
	lv_obj_t *bar_hos_os = lv_bar_create(h1, NULL);
	lv_obj_set_size(bar_hos_os, bar_hos_os_size, LV_DPI / 2);
	lv_bar_set_range(bar_hos_os, 0, 1);
	lv_bar_set_value(bar_hos_os, 1);
	lv_bar_set_style(bar_hos_os, LV_BAR_STYLE_INDIC, &bar_hos_os_ind);
	lv_obj_align(bar_hos_os, lbl, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 6);
	part_info.bar_hos_os = bar_hos_os;

	// HOS partition block.
	lv_obj_t *bar_hos = lv_bar_create(h1, bar_hos_os);
	lv_obj_set_size(bar_hos, bar_hos_size, LV_DPI / 2);
	lv_bar_set_style(bar_hos, LV_BAR_STYLE_INDIC, &bar_hos_ind);
	lv_obj_align(bar_hos, bar_hos_os, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
	part_info.bar_hos = bar_hos;

	// emuMMC partition block.
	lv_obj_t *bar_emu = lv_bar_create(h1, bar_hos_os);
	lv_obj_set_size(bar_emu, bar_emu_size, LV_DPI / 2);
	lv_bar_set_style(bar_emu, LV_BAR_STYLE_INDIC, &bar_emu_ind);
	lv_obj_align(bar_emu, bar_hos, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
	part_info.bar_emu = bar_emu;

	// emuSD partition block
	lv_obj_t *bar_emu_sd = lv_bar_create(h1, bar_hos_os);
	lv_obj_set_size(bar_emu_sd, bar_emu_sd_size, LV_DPI / 2);
	lv_bar_set_style(bar_emu_sd, LV_BAR_STYLE_INDIC, &bar_emu_sd_ind);
	lv_obj_align(bar_emu_sd, bar_emu, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
	part_info.bar_emu_sd = bar_emu_sd;

	// L4T partition block.
	lv_obj_t *bar_l4t = lv_bar_create(h1, bar_hos_os);
	lv_obj_set_size(bar_l4t, bar_l4t_size, LV_DPI / 2);
	lv_bar_set_style(bar_l4t, LV_BAR_STYLE_INDIC, &bar_l4t_ind);
	lv_obj_align(bar_l4t, bar_emu_sd, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
	part_info.bar_l4t = bar_l4t;

	// Android partition block.
	lv_obj_t *bar_and = lv_bar_create(h1, bar_hos_os);
	lv_obj_set_size(bar_and, bar_and_size, LV_DPI / 2);
	lv_bar_set_style(bar_and, LV_BAR_STYLE_INDIC, &bar_and_ind);
	lv_obj_align(bar_and, bar_l4t, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
	part_info.bar_and = bar_and;

	// Remaining space
	lv_obj_t *bar_remaining = lv_bar_create(h1, bar_hos_os);
	lv_obj_set_size(bar_remaining, bar_remaining_size, LV_DPI / 2);
	lv_bar_set_style(bar_remaining, LV_BAR_STYLE_INDIC, &bar_remaining_ind);
	lv_obj_align(bar_remaining, bar_and, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
	part_info.bar_remaining = bar_remaining;

	// -------------------------------------------------------------------------
	// Create disk layout blending separators.
	lv_obj_t *sep_hos_os = lv_cont_create(h1, NULL);
	lv_cont_set_fit(sep_hos_os, false, false);
	lv_obj_set_size(sep_hos_os, 0, LV_DPI / 2);
	lv_obj_set_style(sep_hos_os, &sep_hos_os_bg);
	lv_obj_align(sep_hos_os, bar_hos_os, LV_ALIGN_OUT_RIGHT_MID, -4, 0);
	part_info.sep_hos_os = sep_hos_os;

	lv_obj_t *sep_hos = lv_cont_create(h1, NULL);
	lv_cont_set_fit(sep_hos, false, false);
	lv_obj_set_size(sep_hos, 0, LV_DPI / 2);
	lv_obj_set_style(sep_hos, &sep_hos_bg);
	lv_obj_align(sep_hos, bar_hos, LV_ALIGN_OUT_RIGHT_MID, -4, 0);
	part_info.sep_hos = sep_hos;

	lv_obj_t *sep_emu_sd = lv_cont_create(h1, sep_hos);
	lv_obj_set_style(sep_emu_sd, &sep_emu_sd_bg);
	lv_obj_align(sep_emu_sd, bar_emu_sd, LV_ALIGN_OUT_RIGHT_MID, -4, 0);
	part_info.sep_emu_sd = sep_emu_sd;

	lv_obj_t *sep_emu = lv_cont_create(h1, sep_hos);
	lv_obj_set_style(sep_emu, &sep_emu_bg);
	lv_obj_align(sep_emu, bar_emu, LV_ALIGN_OUT_RIGHT_MID, -4, 0);
	part_info.sep_emu = sep_emu;

	lv_obj_t *sep_l4t = lv_cont_create(h1, sep_emu);
	lv_obj_set_style(sep_l4t, &sep_l4t_bg);
	lv_obj_align(sep_l4t, bar_l4t, LV_ALIGN_OUT_RIGHT_MID, -4, 0);
	part_info.sep_l4t = sep_l4t;

	lv_obj_t *sep_and = lv_cont_create(h1, sep_emu);
	lv_obj_set_style(sep_and, &sep_and_bg);
	lv_obj_align(sep_and, bar_and, LV_ALIGN_OUT_RIGHT_MID, -4, 0);
	part_info.sep_and = sep_and;

	// Create slider type labels.
	lv_obj_t *lbl_hos_os = lv_label_create(h1, NULL);
	lv_label_set_recolor(lbl_hos_os, true);
	lv_label_set_static_text(lbl_hos_os, "#FFD300 "SYMBOL_DOT" HOS (USER):#");
	lv_obj_align(lbl_hos_os, bar_hos_os, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 2);
	lv_obj_set_hidden(lbl_hos_os, !has_hos_os);

	lv_coord_t spacing;
	if(drive == DRIVE_EMMC){
		if(has_hos_os){
			// adjust spacing when we have 5 sliders
			spacing = (LV_DPI / 3) - (lv_obj_get_height(lbl_hos_os) + LV_DPI / 3) / 3;
		}else{
			spacing = (LV_DPI / 3) - (lv_obj_get_height(lbl_hos_os) + LV_DPI / 3) / 4;
		}
	}else{
		spacing = LV_DPI / 3;
	}
	
	lv_obj_t *lbl_hos = lv_label_create(h1, NULL);
	lv_label_set_recolor(lbl_hos, true);
	lv_label_set_static_text(lbl_hos, "#96FF00 "SYMBOL_DOT" HOS (FAT32):#");
	lv_obj_align(lbl_hos, has_hos_os ? lbl_hos_os : bar_hos_os, LV_ALIGN_OUT_BOTTOM_LEFT, 0, has_hos_os ? spacing : LV_DPI / 2);

	lv_obj_t *lbl_emu = lv_label_create(h1, lbl_hos);
	lv_label_set_static_text(lbl_emu, "#FF3C28 "SYMBOL_DOT" emuMMC (RAW):#");
	lv_obj_align(lbl_emu, lbl_hos, LV_ALIGN_OUT_BOTTOM_LEFT, 0, spacing);

	lv_obj_t *lbl_emu_sd = lv_label_create(	h1, lbl_hos);
	lv_label_set_static_text(lbl_emu_sd, "#FF00D6 " SYMBOL_DOT " emuSD:#");
	lv_obj_align(lbl_emu_sd, lbl_emu, LV_ALIGN_OUT_BOTTOM_LEFT, 0, spacing);
	lv_obj_set_hidden(lbl_emu_sd, drive == DRIVE_SD);

	lv_obj_t *lbl_l4t = lv_label_create(h1, lbl_hos);
	lv_label_set_static_text(lbl_l4t, "#00DDFF "SYMBOL_DOT" Linux (EXT4):#");
	lv_obj_align(lbl_l4t, drive == DRIVE_SD ? lbl_emu : lbl_emu_sd, LV_ALIGN_OUT_BOTTOM_LEFT, 0, spacing);

	lv_obj_t *lbl_and = lv_label_create(h1, lbl_hos);
	lv_label_set_static_text(lbl_and, "#FF8000 "SYMBOL_DOT" Android (USER):#");
	lv_obj_align(lbl_and, lbl_l4t, LV_ALIGN_OUT_BOTTOM_LEFT, 0, spacing);


	// Create HOS OS size slider
	lv_obj_t *slider_hos_os = lv_slider_create(h1, NULL);
	lv_obj_set_size(slider_hos_os, LV_DPI * 7, LV_DPI / 3);
	lv_slider_set_range(slider_hos_os, 0, (part_info.total_sct - AU_ALIGN_SECTORS - (part_info.hos_sys_size_mb << 10)) / SECTORS_PER_GB);
	lv_slider_set_value(slider_hos_os, (part_info.hos_os_size - part_info.hos_sys_size_mb) >> 10);
	lv_slider_set_style(slider_hos_os, LV_SLIDER_STYLE_BG, &bar_hos_os_bg);
	lv_slider_set_style(slider_hos_os, LV_SLIDER_STYLE_INDIC, &bar_hos_os_ind);
	lv_slider_set_style(slider_hos_os, LV_SLIDER_STYLE_KNOB, &bar_hos_os_btn);
	lv_obj_align(slider_hos_os, lbl_hos_os, LV_ALIGN_IN_LEFT_MID, LV_DPI * 3, 0);
	lv_slider_set_action(slider_hos_os, _action_slider_hos_os);
	lv_obj_set_hidden(slider_hos_os, !has_hos_os);
	part_info.slider_hos_os = slider_hos_os;

	// Create HOS size slider. Non-interactive.
	lv_obj_t *slider_hos;
	if(drive == DRIVE_EMMC){
		// Allow adjustment of fat partition on emmc
		slider_hos = lv_slider_create(h1, NULL);
		lv_obj_set_size(slider_hos, LV_DPI * 7, LV_DPI / 3);
		lv_slider_set_style(slider_hos, LV_SLIDER_STYLE_KNOB, &bar_hos_btn);
		lv_slider_set_action(slider_hos, _action_slider_hos);
	}else{
		slider_hos = lv_bar_create(h1, NULL);
		lv_obj_set_size(slider_hos, LV_DPI * 7, LV_DPI * 3 / 17);
	}
	lv_bar_set_range(slider_hos, 0, (part_info.total_sct - AU_ALIGN_SECTORS) / SECTORS_PER_GB);
	lv_bar_set_value(slider_hos, part_info.hos_size >> 10);
	lv_bar_set_style(slider_hos, LV_SLIDER_STYLE_BG, &bar_hos_bg);
	lv_bar_set_style(slider_hos, LV_SLIDER_STYLE_INDIC, &bar_hos_ind);
	lv_obj_align(slider_hos, lbl_hos, LV_ALIGN_IN_LEFT_MID, LV_DPI * 3, 0);

	part_info.slider_bar_hos = slider_hos;

	// Create emuMMC size slider.
	lv_obj_t *slider_emu = lv_slider_create(h1, NULL);
	lv_obj_set_size(slider_emu, LV_DPI * 7, LV_DPI / 3);
	lv_slider_set_range(slider_emu, 0, ((part_info.total_sct - AU_ALIGN_SECTORS) / SECTORS_PER_GB) * 2);
	lv_slider_set_value(slider_emu, part_info.emu_size >> 10);
	lv_slider_set_style(slider_emu, LV_SLIDER_STYLE_BG, &bar_emu_bg);
	lv_slider_set_style(slider_emu, LV_SLIDER_STYLE_INDIC, &bar_emu_ind);
	lv_slider_set_style(slider_emu, LV_SLIDER_STYLE_KNOB, &bar_emu_btn);
	lv_obj_align(slider_emu, lbl_emu, LV_ALIGN_IN_LEFT_MID, LV_DPI * 3,0);
	lv_slider_set_action(slider_emu, _action_slider_emu);
	part_info.slider_emu = slider_hos;

	// Create L4T size slider.
	lv_obj_t *slider_l4t = lv_slider_create(h1, NULL);
	lv_obj_set_size(slider_l4t, LV_DPI * 7, LV_DPI / 3);
	lv_slider_set_range(slider_l4t, 0, (part_info.total_sct - extra_sct) / SECTORS_PER_GB);
	lv_slider_set_value(slider_l4t, part_info.l4t_size >> 10);
	lv_slider_set_style(slider_l4t, LV_SLIDER_STYLE_BG, &bar_l4t_bg);
	lv_slider_set_style(slider_l4t, LV_SLIDER_STYLE_INDIC, &bar_l4t_ind);
	lv_slider_set_style(slider_l4t, LV_SLIDER_STYLE_KNOB, &bar_l4t_btn);
	lv_obj_align(slider_l4t, lbl_l4t, LV_ALIGN_IN_LEFT_MID, LV_DPI * 3, 0);
	lv_slider_set_action(slider_l4t, _action_slider_l4t);
	part_info.slider_l4t = slider_l4t;

	// Create Android size slider.
	lv_obj_t *slider_and = lv_slider_create(h1, NULL);
	lv_obj_set_size(slider_and, LV_DPI * 7, LV_DPI / 3);
	#ifdef ENABLE_DUAL_ANDROID
	lv_slider_set_range(slider_and, 0, ((part_info.total_sct - extra_sct) / SECTORS_PER_GB - 4) * 2); // Subtract android reserved size.
	#else
	lv_slider_set_range(slider_and, 0, (part_info.total_sct - extra_sct) / SECTORS_PER_GB - 4); // Subtract android reserved size.
	#endif
	lv_slider_set_value(slider_and, part_info.and_size >> 10);
	lv_slider_set_style(slider_and, LV_SLIDER_STYLE_BG, &bar_and_bg);
	lv_slider_set_style(slider_and, LV_SLIDER_STYLE_INDIC, &bar_and_ind);
	lv_slider_set_style(slider_and, LV_SLIDER_STYLE_KNOB, &bar_and_btn);
	lv_obj_align(slider_and, lbl_and, LV_ALIGN_IN_LEFT_MID, LV_DPI * 3, 0);
	lv_slider_set_action(slider_and, _action_slider_and);
	part_info.slider_and = slider_and;

	// Create emuSD size slider
	lv_obj_t *slider_emu_sd = lv_slider_create(h1, NULL);
	lv_obj_set_size(slider_emu_sd, LV_DPI * 7, LV_DPI / 3);
	lv_slider_set_range(slider_emu_sd, 0, ((part_info.total_sct - extra_sct) / SECTORS_PER_GB) * 2); // Subtract android reserved size.
	lv_slider_set_value(slider_emu_sd, part_info.emu_sd_size >> 10);
	lv_slider_set_style(slider_emu_sd, LV_SLIDER_STYLE_BG, &bar_emu_sd_bg);
	lv_slider_set_style(slider_emu_sd, LV_SLIDER_STYLE_INDIC, &bar_emu_sd_ind);
	lv_slider_set_style(slider_emu_sd, LV_SLIDER_STYLE_KNOB, &bar_emu_sd_btn);
	lv_obj_align(slider_emu_sd, lbl_emu_sd, LV_ALIGN_IN_LEFT_MID, LV_DPI * 3, 0);
	lv_slider_set_action(slider_emu_sd, _action_slider_emu_sd);
	lv_obj_set_hidden(slider_emu_sd, drive == DRIVE_SD);
	part_info.slider_emu_sd = slider_emu_sd;

	// Create HOS OS size lable
	lv_obj_t *lbl_sl_hos_os = lv_label_create(h1, NULL);
	lv_label_set_recolor(lbl_sl_hos_os, true);
	if(part_info.hos_os_size == part_info.hos_os_og_size){
		s_printf(txt_buf, "#E6BF00 %d FULL#", (part_info.hos_os_size - part_info.hos_sys_size_mb) >> 10);
	}else{
		s_printf(txt_buf, "#E6BF00 %d GiB#", (part_info.hos_os_size - part_info.hos_sys_size_mb) >> 10);
	}
	lv_label_set_text(lbl_sl_hos_os, txt_buf);
	lv_obj_align(lbl_sl_hos_os, slider_hos_os, LV_ALIGN_OUT_RIGHT_MID, LV_DPI * 4 / 7, 0);
	lv_obj_set_hidden(lbl_sl_hos_os, !has_hos_os);
	part_info.lbl_hos_os = lbl_sl_hos_os;

	// Create HOS size label.
	lv_obj_t *lbl_sl_hos = lv_label_create(h1, NULL);
	lv_label_set_recolor(lbl_sl_hos, true);
	s_printf(txt_buf, "#96FF00 %d GiB#", part_info.hos_size >> 10);
	lv_label_set_text(lbl_sl_hos, txt_buf);
	lv_obj_align(lbl_sl_hos, slider_hos, LV_ALIGN_OUT_RIGHT_MID, LV_DPI * 4 / 7, 0);
	part_info.lbl_hos = lbl_sl_hos;

	// Create emuMMC size label.
	lv_obj_t *lbl_sl_emu = lv_label_create(h1, lbl_sl_hos);
	lv_label_set_text(lbl_sl_emu, "#FF3C28 0 GiB#");
	lv_obj_align(lbl_sl_emu, slider_emu, LV_ALIGN_OUT_RIGHT_MID, LV_DPI * 4 / 7, 0);
	part_info.lbl_emu = lbl_sl_emu;

	// Create L4T size label.
	lv_obj_t *lbl_sl_l4t = lv_label_create(h1, lbl_sl_hos);
	lv_label_set_text(lbl_sl_l4t, "#00DDFF 0 GiB#");
	lv_obj_align(lbl_sl_l4t, slider_l4t, LV_ALIGN_OUT_RIGHT_MID, LV_DPI * 4 / 7, 0);
	part_info.lbl_l4t = lbl_sl_l4t;

	// Create Android size label.
	lv_obj_t *lbl_sl_and = lv_label_create(h1, lbl_sl_hos);
	lv_label_set_text(lbl_sl_and, "#FF8000 0 GiB#");
	lv_obj_align(lbl_sl_and, slider_and, LV_ALIGN_OUT_RIGHT_MID, LV_DPI * 4 / 7, 0);
	part_info.lbl_and = lbl_sl_and;

	// Create emuSD size label
	lv_obj_t *lbl_sl_emu_sd = lv_label_create(h1, lbl_sl_hos);
	lv_label_set_text(lbl_sl_emu_sd, "#FF00D6 0 GiB#");
	lv_obj_align(lbl_sl_emu_sd, slider_emu_sd, LV_ALIGN_OUT_RIGHT_MID, LV_DPI * 4 / 7, 0);
	lv_obj_set_hidden(lbl_sl_emu_sd, drive == DRIVE_SD);
	part_info.lbl_emu_sd = lbl_sl_emu_sd;

	// Set partition manager notes.
	const char *sd_notes = 
		"Note 1: Only up to #C7EA46 1GB# can be backed up. If more, you will be asked to back them manually at the next step.\n"
		"Note 2: Resized emuMMC formats the USER partition. A save data manager can be used to move them over.\n"
		"Note 3: The #C7EA46 Flash Linux# and #C7EA46 Flash Android# will flash files if suitable partitions and installer files are found.\n";

	const char *emmc_notes =
		"Note 1: Resizing the #C7EA46 HOS USER# partition will format it, setting it to 0 will #C7EA46 remove# HOS from eMMC.\n"
		"Note 2: Resized emuMMC formats the USER partition. A save data manager can be used to move them over.\n"
		"Note 3: The #C7EA46 Flash Linux# and #C7EA46 Flash Android# will flash files if suitable partitions and installer files are found.\n"
		"Note 4: When creating a #C7EA46 FAT32# partition, all unused storage will be assigned to it.\n";

	lv_obj_t *lbl_notes = lv_label_create(h1, NULL);
	lv_label_set_recolor(lbl_notes, true);
	lv_label_set_static_text(lbl_notes, drive == DRIVE_SD ? sd_notes : emmc_notes);
	lv_label_set_style(lbl_notes, &hint_small_style);
	lv_obj_align(lbl_notes, lbl_and, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 5);

	// Create UMS button.
	lv_obj_t *btn1 = lv_btn_create(h1, NULL);
	lv_obj_t *label_btn = lv_label_create(btn1, NULL);
	lv_btn_set_fit(btn1, true, true);
	lv_label_set_static_text(label_btn, drive == DRIVE_SD ? SYMBOL_USB"  SD UMS" : SYMBOL_CHIP "  eMMC UMS");
	lv_obj_align(btn1, h1, LV_ALIGN_IN_TOP_LEFT, 0, LV_DPI * 5);
	lv_btn_set_action(btn1, LV_BTN_ACTION_CLICK, drive == DRIVE_SD ? _action_part_manager_ums_sd : _action_part_manager_ums_emmc);

	// Create Flash Linux button.
	btn_flash_l4t = lv_btn_create(h1, NULL);
	lv_obj_t *label_btn2 = lv_label_create(btn_flash_l4t, NULL);
	lv_btn_set_fit(btn_flash_l4t, true, true);
	lv_label_set_static_text(label_btn2, SYMBOL_DOWNLOAD"  Flash Linux");
	lv_obj_align(btn_flash_l4t, btn1, LV_ALIGN_OUT_RIGHT_MID, LV_DPI / 3, 0);
	lv_btn_set_action(btn_flash_l4t, LV_BTN_ACTION_CLICK, _action_check_flash_linux);

	// Disable Flash Linux button if partition not found.
	u32 size_sct = _get_available_l4t_partition();
	if (!l4t_flash_ctxt.offset_sct || size_sct < 0x800000)
	{
		lv_obj_set_click(btn_flash_l4t, false);
		lv_btn_set_state(btn_flash_l4t, LV_BTN_STATE_INA);
	}

	// TODO: check for multiple android slots, if multiple, add another mbox to first select which slot to use
	int part_type_and = _get_available_android_partition();

	// Create Flash Android button.
	btn_flash_android = lv_btn_create(h1, NULL);
	label_btn = lv_label_create(btn_flash_android, NULL);
	lv_btn_set_fit(btn_flash_android, true, true);
	switch (part_type_and)
	{
	case 0: // Disable Flash Android button if partition not found.
		lv_label_set_static_text(label_btn, SYMBOL_DOWNLOAD"  Flash Android");
		lv_obj_set_click(btn_flash_android, false);
		lv_btn_set_state(btn_flash_android, LV_BTN_STATE_INA);
		break;
	case 1: // Android 10/11.
		lv_label_set_static_text(label_btn, SYMBOL_DOWNLOAD"  Flash Android 10/11");
		break;
	case 2: // Android 13+
		lv_label_set_static_text(label_btn, SYMBOL_DOWNLOAD"  Flash Android 13+");
		break;
	}
	lv_obj_align(btn_flash_android, btn_flash_l4t, LV_ALIGN_OUT_RIGHT_MID, LV_DPI / 3, 0);
	lv_btn_set_action(btn_flash_android, LV_BTN_ACTION_CLICK, _action_flash_android);

	// Create next step button.
	btn1 = lv_btn_create(h1, NULL);
	label_btn = lv_label_create(btn1, NULL);
	lv_btn_set_fit(btn1, true, true);
	lv_label_set_static_text(label_btn, SYMBOL_SD"  Next Step");
	lv_obj_align(btn1, h1, LV_ALIGN_IN_TOP_RIGHT, 0, LV_DPI * 5);
	lv_btn_set_action(btn1, LV_BTN_ACTION_CLICK, _create_mbox_partitioning_next);

	free(txt_buf);
	free(gpt);

	_update_partition_bar();

	return LV_RES_OK;
}
