#ifndef _FE_EMUSD_TOOLS_H_
#define _FE_EMUSD_TOOLS_H_

#include "gui.h"

typedef struct _emusd_cfg_t{
	// 0: disabled, 1: enabled
	int enabled;
	u32 sector;
} emusd_cfg_t;

void load_emusd_cfg(emusd_cfg_t *emu_info);
void save_emusd_cfg(u32 part_idx, u32 sector_start);
int create_emusd(int part_idx, u32 sector_mbr, u32 sector_start, u32 sector_size, const char *name);

#endif
