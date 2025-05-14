#include "emusd.h"
#include <bdk.h>
#include <string.h>
#include <utils/ini.h>

emusd_cfg_t emu_sd_cfg = {0};

void emusd_load_cfg()
{
	emu_sd_cfg.enabled = 0;
	emu_sd_cfg.fs_ver  = 0;
	emu_sd_cfg.id      = 0;
	emu_sd_cfg.sector  = 0;
	if(!emu_sd_cfg.path) {
		emu_sd_cfg.path = (char*)malloc(0x200);
	}
	if(emu_sd_cfg.emummc_file_based_path){
		emu_sd_cfg.emummc_file_based_path = (char*)malloc(0x200);
	}

	LIST_INIT(ini_sections);
	if(ini_parse(&ini_sections, "emuSD/emusd.ini", false))
	{
		LIST_FOREACH_ENTRY(ini_sec_t, ini_sec, &ini_sections, link)
		{
			if(ini_sec->type == INI_CHOICE)
			{
				if(strcmp(ini_sec->name, "emusd")){
					continue;
				}

				LIST_FOREACH_ENTRY(ini_kv_t, kv, &ini_sec->kvs, link)
				{
					if(!strcmp(kv->key, "enabled")){
						emu_sd_cfg.enabled = atoi(kv->val);
					} else if(!strcmp(kv->key, "sector")){
						emu_sd_cfg.sector = strtol(kv->val, NULL, 16);
					}
				}
				break;
			}
		}
	}
}

bool emusd_set_path(char *path) {
	FIL fp;
	bool found = false;
	char sd_path[0x80];

	// TODO: use emu_sd.file_path  instead
	strcpy(sd_path, path);
	strcat(sd_path, "/raw_emmc_based");

	if(!f_open(&fp, sd_path, FA_READ))
	{
		if(!f_read(&fp, &emu_sd_cfg.sector, 4, NULL)){
			if(emu_sd_cfg.sector){
				found = true;
				emu_sd_cfg.enabled = 4;
				goto out;
			}
		}
	}

	// TODO: file based / SD partition based support

	out:
	return found;
}