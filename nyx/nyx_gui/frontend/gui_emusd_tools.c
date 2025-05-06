#include "gui_emusd_tools.h"
#include "fe_emusd_tools.h"
#include "gui_tools_partition_manager.h"
#include "gui.h"
#include <libs/fatfs/ff.h>
#include <libs/lvgl/lv_core/lv_obj.h>
#include <libs/lvgl/lv_objx/lv_btnm.h>
#include <libs/lvgl/lv_objx/lv_list.h>
#include <libs/lvgl/lv_objx/lv_mbox.h>
#include <storage/boot_storage.h>
#include "fe_emusd_tools.h" 
#include <stdlib.h>
#include <storage/emmc.h>
#include <storage/mbr_gpt.h>
#include <storage/sdmmc.h>
#include <string.h>
#include <utils/sprintf.h>
#include <mem/heap.h>
#include <utils/types.h>
#include <gfx_utils.h>

typedef struct _gpt_ctxt_t{
	u32 sector[3];
	u32 sector_mbr[3];
	u32 part_idx[3];
	u32 size[3];
	char name[3][36];
	u8 cnt;
	u8 valid_parts[3];
	u8 valid_cnt;
} gpt_ctxt_t;

static gpt_ctxt_t gpt_ctxt;

static lv_obj_t *emusd_parent_cont;
static lv_res_t (*emusd_tools)(lv_obj_t *parent);

static void _get_emusd_parts(gpt_t *gpt){
	u8 cnt = 0;
	s32 gpt_idx = gpt_get_part_by_name(gpt, "emusd_mbr", -1);
	while(gpt_idx != -1 && cnt < 3){
		u32 sector_mbr = gpt->entries[gpt_idx].lba_start;
		gpt_idx = gpt_get_part_by_name(gpt, "emusd", gpt_idx);
		if(gpt_idx == -1){
			// should never happen
			continue;
		}

		u32 sector = gpt->entries[gpt_idx].lba_start;
		u32 size = gpt->entries[gpt_idx].lba_end - sector + 1;

		gpt_ctxt.sector_mbr[cnt] = sector_mbr;
		gpt_ctxt.sector[cnt] = sector;
		gpt_ctxt.part_idx[cnt] = gpt_idx;
		gpt_ctxt.size[cnt] = size;
		wctombs(gpt->entries[gpt_idx].name, gpt_ctxt.name[cnt], 36);

		cnt++;

		gpt_idx = gpt_get_part_by_name(gpt, "emusd", gpt_idx);
	}
	gpt_ctxt.cnt = cnt;
}

static lv_res_t _close_mbox_and_reload(lv_obj_t *btns, const char *txt){
	lv_obj_clean(emusd_parent_cont);
	mbox_action(btns, txt);

	(*emusd_tools)(emusd_parent_cont);

	return LV_RES_INV;
}

static void _create_emusd(u8 idx){
	boot_storage_mount();
	emmc_initialize(false);


	static const char *mbox_btn_map[] = { "\251", "\222OK", "\251", "" };

	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	lv_obj_t * mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 6);

	char *txt_buf = malloc(SZ_16K);

	strcpy(txt_buf, "#FF8000 emuSD creation tool#\n\n"
	 	            "#00DDFF Status:# Formatting emuSD partition...");

	lv_mbox_set_text(mbox, txt_buf);
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_top(mbox, true);

	manual_system_maintenance(true);

	int res = create_emusd(gpt_ctxt.part_idx[idx], gpt_ctxt.sector_mbr[idx], gpt_ctxt.sector[idx], gpt_ctxt.size[idx], gpt_ctxt.name[idx]);

	if(res == FR_OK){
		strcpy(txt_buf, "#FF8000 emuSD creation tool#\n\n"
	 	                "#00DDFF Status:# Done!");

		char path[0x80];
		strcpy(path, "emuSD");
		f_mkdir(path);
		s_printf(path + strlen(path), "/RAW_EMMC%d", gpt_ctxt.part_idx[idx]);
		f_mkdir(path);
		strcat(path, "/raw_emmc_based");

		FIL f;
		f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE);
		f_write(&f, &gpt_ctxt.sector_mbr[idx], 4, NULL);
		f_close(&f);

		save_emusd_cfg(gpt_ctxt.part_idx[idx], gpt_ctxt.sector_mbr[idx]); 
	}else{
		strcpy(txt_buf, "#FF8000 emuSD creation tool#\n\n"
	 	                "#00DDFF Status:# Failed to format partition!");
	}

	lv_mbox_set_text(mbox, txt_buf);
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
	lv_mbox_add_btns(mbox, mbox_btn_map, _close_mbox_and_reload);
	manual_system_maintenance(true);

	boot_storage_unmount();
	emmc_end();

	free(txt_buf);
}

static lv_res_t _emusd_create_action(lv_obj_t *btns, const char *txt){
	u32 idx = lv_btnm_get_pressed(btns);

	mbox_action(btns, txt);

	if(idx < gpt_ctxt.cnt){
		_create_emusd(idx);
	}

	return LV_RES_INV;
}

static lv_res_t _emusd_format_action(lv_obj_t *btns, const char *txt){
	u32 idx = lv_btnm_get_pressed(btns);

	mbox_action(btns, txt);

	if(idx){
		create_window_partition_manager(btns, DRIVE_EMMC);
	}

	return LV_RES_INV;
}

static void _change_emusd(u8 idx){
	boot_storage_mount();
	save_emusd_cfg(gpt_ctxt.part_idx[gpt_ctxt.valid_parts[idx]], gpt_ctxt.sector_mbr[gpt_ctxt.valid_parts[idx]]); 
	boot_storage_unmount();
}

static lv_res_t _emusd_change_action(lv_obj_t *btns, const char *txt){
	u32 idx = lv_btnm_get_pressed(btns);

	if(idx < gpt_ctxt.valid_cnt){
		_change_emusd(idx);
		_close_mbox_and_reload(btns, txt);
	}else if(idx == gpt_ctxt.valid_cnt){
		boot_storage_mount();
		save_emusd_cfg(0, 0);
		boot_storage_unmount(); 
		_close_mbox_and_reload(btns, txt);
	}else{
		mbox_action(btns, txt);
	}

	return LV_RES_INV;
}

static lv_res_t _create_mbox_change_emusd(lv_obj_t *btn){
	boot_storage_mount();
	emmc_initialize(false);

	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	lv_obj_t * mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 6);

	static char mbox_part_strs[6][0x10];
	static char *mbox_btn_parts[] = {mbox_part_strs[0], mbox_part_strs[1], mbox_part_strs[2], mbox_part_strs[3], mbox_part_strs[4], mbox_part_strs[5]};

	dirlist_t *emusd_dir_list = dirlist("emuSD", NULL, false, true);
	char path[0x80];
	mbr_t mbr = {0};
	gpt_t *gpt = NULL;
	char *txt_buf = malloc(SZ_16K);

	u32 emusd_idx = 0;
	FIL f;

	sdmmc_storage_read(&emmc_storage, 0, 1, &mbr);

	if(!mbr_has_gpt(&mbr)){
		goto out;
	}

	gpt = zalloc(sizeof(*gpt));
	sdmmc_storage_read(&emmc_storage, 1, sizeof(*gpt) / 0x200, gpt);

	_get_emusd_parts(gpt);

	u8 cnt = 0;

	while(emusd_dir_list && emusd_dir_list->name[emusd_idx]){
		s_printf(path, "emuSD/%s/raw_emmc_based", emusd_dir_list->name[emusd_idx]);
		if(f_stat(path, NULL) == FR_OK){
			f_open(&f, path, FA_READ);
			u32 sector = 0;
			f_read(&f, &sector, 4, NULL);
			f_close(&f);

			for(u32 i = 0; i < gpt_ctxt.cnt ; i++){
				if(sector == gpt_ctxt.sector_mbr[i]){
					s_printf(mbox_btn_parts[cnt], "Part %d", gpt_ctxt.part_idx[i]);
					gpt_ctxt.valid_parts[cnt] = i;
					cnt++;
				}
			}
		}
		emusd_idx++;
	}

	gpt_ctxt.valid_cnt = cnt;

	strcpy(mbox_btn_parts[cnt], "Disable");
	strcpy(mbox_btn_parts[cnt + 1], "Cancel");
	mbox_btn_parts[cnt + 2][0] = 0;

	if(cnt){
		strcpy(txt_buf, "#C7EA46 Found emuSD partition(s)!#\n"
				        "#FF8000 Choose a partition or disable emuSD.#\n\n");
		for(u32 i = 0; i < cnt; i++){
			s_printf(txt_buf + strlen(txt_buf), "Part %d (%s): Start: 0x%x, Size: 0x%x\n", gpt_ctxt.part_idx[gpt_ctxt.valid_parts[i]], gpt_ctxt.name[gpt_ctxt.valid_parts[i]], gpt_ctxt.sector[gpt_ctxt.valid_parts[i]], gpt_ctxt.size[gpt_ctxt.valid_parts[i]]);
		}
	}else{
		strcpy(txt_buf, "#FF8000 No set up emuSD partitions found.\nFirst, create an emuSD\n#");
	}

	lv_mbox_set_text(mbox, txt_buf);
	lv_mbox_add_btns(mbox, (const char **)mbox_btn_parts, _emusd_change_action);
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);

	out:
	free(gpt);
	free(emusd_dir_list);

	boot_storage_unmount();
	emmc_end();

	return LV_RES_OK;
}

static lv_res_t _create_mbox_emusd_create(lv_obj_t *btn){
	lv_obj_t *dark_bg = lv_obj_create(lv_scr_act(), NULL);
	lv_obj_set_style(dark_bg, &mbox_darken);
	lv_obj_set_size(dark_bg, LV_HOR_RES, LV_VER_RES);

	lv_obj_t * mbox = lv_mbox_create(dark_bg, NULL);
	lv_mbox_set_recolor_text(mbox, true);
	lv_obj_set_width(mbox, LV_HOR_RES / 9 * 6);

	static char mbox_part_strs[5][0x10];
	static char *mbox_btn_parts[] = {mbox_part_strs[0], mbox_part_strs[1], mbox_part_strs[2], mbox_part_strs[3], mbox_part_strs[4]};
	static const char *mbox_btn_format[] = {"\222Continue", "\222Cancel", ""};

	char *txt_buf = (char*)malloc(SZ_16K);

	emmc_initialize(false);
	emmc_set_partition(EMMC_GPP);

	gpt_t *gpt = NULL;
	mbr_t mbr = {0};
	u8 cnt = 0;

	sdmmc_storage_read(&emmc_storage, 0, 1, &mbr);

	if(mbr_has_gpt(&mbr)){
		gpt = zalloc(sizeof(*gpt));
		sdmmc_storage_read(&emmc_storage, 1, sizeof(*gpt) / 0x200, gpt);

		_get_emusd_parts(gpt);

		cnt = gpt_ctxt.cnt;
	}

	if(cnt){
		s_printf(txt_buf, 
		         "#C7EA46 Found applicable eMMC partition(s)!#\n"
				 "#FF8000 Choose a partition to continue:#\n\n");
		for(u32 i = 0; i < cnt; i++){
			char name[36];
			u8 idx = gpt_ctxt.part_idx[i];
			u32 size = gpt->entries[idx].lba_end - gpt->entries[idx].lba_start + 1;
			wctombs(gpt->entries[idx].name, name, 36);
			s_printf(txt_buf + strlen(txt_buf), "Part %d (%s): Start: 0x%x, Size: 0x%x\n", idx, name, gpt_ctxt.sector[i], size);
			s_printf(mbox_btn_parts[i], "\222Part %d", gpt_ctxt.part_idx[i]);

		}
		s_printf(mbox_btn_parts[cnt], "\222Cancel");
		mbox_btn_parts[cnt + 1][0] = '\0';

		lv_mbox_add_btns(mbox, (const char **)mbox_btn_parts, _emusd_create_action);
	}else{
		s_printf(txt_buf, "#FFDD00 Failed to find applicable partition!#\n\n"
		                  "#FF8000 Do you want to partition the eMMC?#\n"
						  "#FF8000 (You will be asked on how to proceed)#");
		lv_mbox_add_btns(mbox, mbox_btn_format, _emusd_format_action);
	}

	lv_mbox_set_text(mbox, txt_buf);
	lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);

	emmc_end();

	free(txt_buf);
	free(gpt);

	return LV_RES_OK;
}

lv_res_t create_tab_emusd_tools(lv_obj_t *parent)
{
	emusd_parent_cont = parent;
	emusd_tools = &create_tab_emusd_tools;

	boot_storage_mount();

	emusd_cfg_t emu_info;
	load_emusd_cfg(&emu_info);

	boot_storage_unmount();

	static lv_style_t h_style;
	lv_style_copy(&h_style, &lv_style_transp);
	h_style.body.padding.inner = 0;
	h_style.body.padding.hor = LV_DPI - (LV_DPI / 4);
	h_style.body.padding.ver = LV_DPI / 9;

	// Create emuSD Info & Selection container.
	lv_obj_t *h1 = lv_cont_create(parent, NULL);
	lv_cont_set_style(h1, &h_style);
	lv_cont_set_fit(h1, false, true);
	lv_obj_set_width(h1, (LV_HOR_RES / 9) * 4);
	lv_obj_set_click(h1, false);
	lv_cont_set_layout(h1, LV_LAYOUT_OFF);

	lv_obj_t *label_sep = lv_label_create(h1, NULL);
	lv_label_set_static_text(label_sep, "");

	lv_obj_t *label_txt = lv_label_create(h1, NULL);
	lv_label_set_static_text(label_txt, "emuSD Info & Selection");
	lv_obj_set_style(label_txt, lv_theme_get_current()->label.prim);
	lv_obj_align(label_txt, h1, LV_ALIGN_IN_TOP_LEFT, LV_DPI / 4, -LV_DPI / 9);

	lv_obj_t *line_sep = lv_line_create(h1, NULL);
	static const lv_point_t line_pp[] = { {0, 0}, { LV_HOR_RES - (LV_DPI - (LV_DPI / 4)) * 2, 0} };
	lv_line_set_points(line_sep, line_pp, 2);
	lv_line_set_style(line_sep, lv_theme_get_current()->line.decor);
	lv_obj_align(line_sep, label_txt, LV_ALIGN_OUT_BOTTOM_LEFT, -(LV_DPI / 4), LV_DPI / 8);

	// Create emuSD info labels.
	lv_obj_t *label_btn = lv_label_create(h1, NULL);
	lv_label_set_recolor(label_btn, true);
	lv_label_set_static_text(label_btn, emu_info.enabled ? "#96FF00 "SYMBOL_OK"  Enabled!#" : "#FF8000 "SYMBOL_CLOSE"  Disabled!#");
	lv_obj_align(label_btn, line_sep, LV_ALIGN_OUT_BOTTOM_LEFT, LV_DPI / 4, LV_DPI / 4);

	lv_obj_t *label_txt2 = lv_label_create(h1, NULL);
	lv_label_set_recolor(label_txt2, true);
	char *txt_buf = (char *)malloc(SZ_16K);

	if (emu_info.enabled)
	{
		s_printf(txt_buf, "#00DDFF Type:# eMMC Raw Partition\n#00DDFF Sector:# 0x%08X",
			emu_info.sector);

		lv_label_set_text(label_txt2, txt_buf);
	}
	else
	{
		lv_label_set_static_text(label_txt2, "emuSD is disabled and SD will be used for boot.\n\n");
	}

	free(txt_buf);

	lv_obj_set_style(label_txt2, &hint_small_style);
	lv_obj_align(label_txt2, label_btn, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 3);

	// Create Change emuMMC button.
	lv_obj_t *btn2 = lv_btn_create(h1, NULL);
	lv_btn_set_fit(btn2, true, true);
	label_btn = lv_label_create(btn2, NULL);
	lv_label_set_static_text(label_btn, SYMBOL_SETTINGS"  Change emuSD");
	lv_obj_align(btn2, label_txt2, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI * 6 / 10);
	lv_btn_set_action(btn2, LV_BTN_ACTION_CLICK, _create_mbox_change_emusd);

	label_txt2 = lv_label_create(h1, NULL);
	lv_label_set_recolor(label_txt2, true);
	lv_label_set_static_text(label_txt2,
		"Choose an emuSD partition\n"
		"You can at most have 3 emuSD partitions.");

	lv_obj_set_style(label_txt2, &hint_small_style);
	lv_obj_align(label_txt2, btn2, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 3);

	// Create emuMMC Tools container.
	lv_obj_t *h2 = lv_cont_create(parent, NULL);
	lv_cont_set_style(h2, &h_style);
	lv_cont_set_fit(h2, false, true);
	lv_obj_set_width(h2, (LV_HOR_RES / 9) * 4);
	lv_obj_set_click(h2, false);
	lv_cont_set_layout(h2, LV_LAYOUT_OFF);
	lv_obj_align(h2, h1, LV_ALIGN_OUT_RIGHT_TOP, LV_DPI * 17 / 29, 0);

	label_sep = lv_label_create(h2, NULL);
	lv_label_set_static_text(label_sep, "");

	lv_obj_t *label_txt3 = lv_label_create(h2, NULL);
	lv_label_set_static_text(label_txt3, "emuSD Tools");
	lv_obj_set_style(label_txt3, lv_theme_get_current()->label.prim);
	lv_obj_align(label_txt3, h2, LV_ALIGN_IN_TOP_LEFT, LV_DPI / 4, 0);

	line_sep = lv_line_create(h2, line_sep);
	lv_obj_align(line_sep, label_txt3, LV_ALIGN_OUT_BOTTOM_LEFT, -(LV_DPI / 4), LV_DPI / 8);

	// Create Create emuMMC button.
	lv_obj_t *btn3 = lv_btn_create(h2, btn2);
	label_btn = lv_label_create(btn3, NULL);
	lv_btn_set_fit(btn3, true, true);
	lv_label_set_static_text(label_btn, SYMBOL_DRIVE"  Create emuSD");
	lv_obj_align(btn3, line_sep, LV_ALIGN_OUT_BOTTOM_LEFT, LV_DPI / 4, LV_DPI / 4);
	lv_btn_set_action(btn3, LV_BTN_ACTION_CLICK, _create_mbox_emusd_create);

	lv_obj_t *label_txt4 = lv_label_create(h2, NULL);
	lv_label_set_recolor(label_txt4, true);
	lv_label_set_static_text(label_txt4,
		"Allows you to create a new emuSD on a suitable #C7EA46 eMMC# partition");

	lv_obj_set_style(label_txt4, &hint_small_style);
	lv_obj_align(label_txt4, btn3, LV_ALIGN_OUT_BOTTOM_LEFT, 0, LV_DPI / 3);

	lv_obj_align(h1, parent, LV_ALIGN_IN_TOP_LEFT, 0, 0);
	lv_obj_align(h2, h1, LV_ALIGN_OUT_RIGHT_TOP, LV_DPI * 17 / 29, 0);

	return LV_RES_OK;
}