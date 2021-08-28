/*
 * Copyright (c) 2018 naehrwert
 * Copyright (c) 2018-2021 CTCaer
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

#include <string.h>

#include "hos.h"
#include "pkg2.h"
#include "pkg2_ini_kippatch.h"

#include "../config.h"
#include <libs/compr/blz.h>
#include <libs/fatfs/ff.h>
#include <mem/heap.h>
#include <sec/se.h>
#include <sec/se_t210.h>
#include "../storage/emummc.h"
#include <storage/nx_sd.h>
#include <utils/aarch64_util.h>

#include <gfx_utils.h>

extern hekate_config h_cfg;
extern const u8 package2_keyseed[];

u32 pkg2_newkern_ini1_val;
u32 pkg2_newkern_ini1_start;
u32 pkg2_newkern_ini1_end;

#ifdef KIP1_PATCH_DEBUG
	#include <utils/util.h>
	#define DPRINTF(...) gfx_printf(__VA_ARGS__)
	#define DEBUG_PRINTING
#else
	#define DPRINTF(...)
#endif

enum kip_offset_section
{
	KIP_TEXT    = 0,
	KIP_RODATA  = 1,
	KIP_DATA    = 2,
	KIP_BSS     = 3,
	KIP_UNKSEC1 = 4,
	KIP_UNKSEC2 = 5
};

#define KIP_PATCH_SECTION_SHIFT  (29)
#define KIP_PATCH_SECTION_MASK   (7 << KIP_PATCH_SECTION_SHIFT)
#define KIP_PATCH_OFFSET_MASK    (~KIP_PATCH_SECTION_MASK)
#define GET_KIP_PATCH_SECTION(x) (((x) >> KIP_PATCH_SECTION_SHIFT) & 7)
#define GET_KIP_PATCH_OFFSET(x)  ((x) & KIP_PATCH_OFFSET_MASK)
#define KPS(x) ((u32)(x) << KIP_PATCH_SECTION_SHIFT)

#include "pkg2_patches.inl"

static kip1_id_t *_kip_id_sets = _kip_ids;
static u32 _kip_id_sets_cnt = ARRAY_SIZE(_kip_ids);

void pkg2_get_ids(kip1_id_t **ids, u32 *entries)
{
	*ids = _kip_id_sets;
	*entries = _kip_id_sets_cnt;
}

static void parse_external_kip_patches()
{
	static bool ext_patches_done = false;

	if (ext_patches_done)
		return;

	u32 curr_kip_idx = 0;
	char path[64];
	strcpy(path, "bootloader/patches.ini");

	LIST_INIT(ini_kip_sections);
	if (ini_patch_parse(&ini_kip_sections, path))
	{
		// Copy ids into a new patchset.
		_kip_id_sets = calloc(sizeof(kip1_id_t), 256); // Max 256 kip ids.
		memcpy(_kip_id_sets, _kip_ids, sizeof(_kip_ids));

		// Parse patchsets and glue them together.
		LIST_FOREACH_ENTRY(ini_kip_sec_t, ini_psec, &ini_kip_sections, link)
		{
			kip1_id_t* curr_kip = NULL;
			bool found = false;
			for (curr_kip_idx = 0; curr_kip_idx < _kip_id_sets_cnt + 1; curr_kip_idx++)
			{
				curr_kip = &_kip_id_sets[curr_kip_idx];

				if (!curr_kip->name)
					break;

				if (!strcmp(curr_kip->name, ini_psec->name) && !memcmp(curr_kip->hash, ini_psec->hash, 8))
				{
					found = true;
					break;
				}
			}

			if (!curr_kip)
				continue;

			// If not found, create a new empty entry.
			if (!found)
			{
				curr_kip->name = ini_psec->name;
				memcpy(curr_kip->hash, ini_psec->hash, 8);
				curr_kip->patchset = calloc(sizeof(kip1_patchset_t), 1);

				_kip_id_sets_cnt++;
			}

			kip1_patchset_t *patchsets = (kip1_patchset_t *)calloc(sizeof(kip1_patchset_t), 16); // Max 16 patchsets per kip.

			u32 curr_patchset_idx;
			for(curr_patchset_idx = 0; curr_kip->patchset[curr_patchset_idx].name != NULL; curr_patchset_idx++)
			{
				patchsets[curr_patchset_idx].name = curr_kip->patchset[curr_patchset_idx].name;
				patchsets[curr_patchset_idx].patches = curr_kip->patchset[curr_patchset_idx].patches;
			}

			curr_kip->patchset = patchsets;
			bool first_ext_patch = true;
			u32 curr_patch_idx = 0;

			// Parse patches and glue them together to a patchset.
			kip1_patch_t *patches = calloc(sizeof(kip1_patch_t), 32); // Max 32 patches per set.
			LIST_FOREACH_ENTRY(ini_patchset_t, pt, &ini_psec->pts, link)
			{
				if (first_ext_patch)
				{
					first_ext_patch = false;
					patchsets[curr_patchset_idx].name = malloc(strlen(pt->name) + 1);
					strcpy(patchsets[curr_patchset_idx].name, pt->name);
					patchsets[curr_patchset_idx].patches = patches;
				}
				else
				{
					// Check if new patchset name is found and create a new set.
					if (strcmp(pt->name, patchsets[curr_patchset_idx].name))
					{
						curr_patchset_idx++;
						curr_patch_idx = 0;
						patches = calloc(sizeof(kip1_patch_t), 16); // Max 16 patches per set.

						patchsets[curr_patchset_idx].name = malloc(strlen(pt->name) + 1);
						strcpy(patchsets[curr_patchset_idx].name, pt->name);
						patchsets[curr_patchset_idx].patches = patches;
					}
				}

				if (pt->length)
				{
					patches[curr_patch_idx].offset = pt->offset;
					patches[curr_patch_idx].length = pt->length;

					patches[curr_patch_idx].srcData = malloc(pt->length);
					patches[curr_patch_idx].dstData = malloc(pt->length);
					memcpy(patches[curr_patch_idx].srcData, pt->srcData, pt->length);
					memcpy(patches[curr_patch_idx].dstData, pt->dstData, pt->length);
				}
				else
					patches[curr_patch_idx].srcData = malloc(1); // Empty patches check. Keep everything else as 0.

				curr_patch_idx++;
			}
			curr_patchset_idx++;
			patchsets[curr_patchset_idx].name = NULL;
			patchsets[curr_patchset_idx].patches = NULL;
		}
	}

	ext_patches_done = true;
}

const pkg2_kernel_id_t *pkg2_identify(u8 *hash)
{
	for (u32 i = 0; i < ARRAY_SIZE(_pkg2_kernel_ids); i++)
	{
		if (!memcmp(hash, _pkg2_kernel_ids[i].hash, sizeof(_pkg2_kernel_ids[0].hash)))
			return &_pkg2_kernel_ids[i];
	}
	return NULL;
}

static u32 _pkg2_calc_kip1_size(pkg2_kip1_t *kip1)
{
	u32 size = sizeof(pkg2_kip1_t);
	for (u32 j = 0; j < KIP1_NUM_SECTIONS; j++)
		size += kip1->sections[j].size_comp;
	return size;
}

void pkg2_get_newkern_info(u8 *kern_data)
{
	u32 pkg2_newkern_ini1_off = 0;
	pkg2_newkern_ini1_start = 0;

	// Find static OP offset that is close to INI1 offset.
	u32 counter_ops = 0x100;
	while (counter_ops)
	{
		if (*(u32 *)(kern_data + 0x100 - counter_ops) == PKG2_NEWKERN_GET_INI1_HEURISTIC)
		{
			pkg2_newkern_ini1_off = 0x100 - counter_ops + 12; // OP found. Add 12 for the INI1 offset.
			break;
		}

		counter_ops -= 4;
	}

	// Offset not found?
	if (!counter_ops)
		return;

	u32 info_op = *(u32 *)(kern_data + pkg2_newkern_ini1_off);
	pkg2_newkern_ini1_val = ((info_op & 0xFFFF) >> 3) + pkg2_newkern_ini1_off; // Parse ADR and PC.

	pkg2_newkern_ini1_start = *(u32 *)(kern_data + pkg2_newkern_ini1_val);
	pkg2_newkern_ini1_end   = *(u32 *)(kern_data + pkg2_newkern_ini1_val + 0x8);
}

bool pkg2_parse_kips(link_t *info, pkg2_hdr_t *pkg2, bool *new_pkg2)
{
	u8 *ptr;
	// Check for new pkg2 type.
	if (!pkg2->sec_size[PKG2_SEC_INI1])
	{
		pkg2_get_newkern_info(pkg2->data);

		if (!pkg2_newkern_ini1_start)
			return false;

		ptr = pkg2->data + pkg2_newkern_ini1_start;
		*new_pkg2 = true;
	}
	else
		ptr = pkg2->data + pkg2->sec_size[PKG2_SEC_KERNEL];

	pkg2_ini1_t *ini1 = (pkg2_ini1_t *)ptr;
	ptr += sizeof(pkg2_ini1_t);

	for (u32 i = 0; i < ini1->num_procs; i++)
	{
		pkg2_kip1_t *kip1 = (pkg2_kip1_t *)ptr;
		pkg2_kip1_info_t *ki = (pkg2_kip1_info_t *)malloc(sizeof(pkg2_kip1_info_t));
		ki->kip1 = kip1;
		ki->size = _pkg2_calc_kip1_size(kip1);
		list_append(info, &ki->link);
		ptr += ki->size;
DPRINTF(" kip1 %d:%s @ %08X (%08X)\n", i, kip1->name, (u32)kip1, ki->size);
	}

	return true;
}

int pkg2_has_kip(link_t *info, u64 tid)
{
	LIST_FOREACH_ENTRY(pkg2_kip1_info_t, ki, info, link)
		if(ki->kip1->tid == tid)
			return 1;
	return 0;
}

void pkg2_replace_kip(link_t *info, u64 tid, pkg2_kip1_t *kip1)
{
	LIST_FOREACH_ENTRY(pkg2_kip1_info_t, ki, info, link)
	{
		if (ki->kip1->tid == tid)
		{
			ki->kip1 = kip1;
			ki->size = _pkg2_calc_kip1_size(kip1);
DPRINTF("replaced kip %s (new size %08X)\n", kip1->name, ki->size);
			return;
		}
	}
}

void pkg2_add_kip(link_t *info, pkg2_kip1_t *kip1)
{
	pkg2_kip1_info_t *ki = (pkg2_kip1_info_t *)malloc(sizeof(pkg2_kip1_info_t));
	ki->kip1 = kip1;
	ki->size = _pkg2_calc_kip1_size(kip1);
DPRINTF("added kip %s (size %08X)\n", kip1->name, ki->size);
	list_append(info, &ki->link);
}

void pkg2_merge_kip(link_t *info, pkg2_kip1_t *kip1)
{
	if (pkg2_has_kip(info, kip1->tid))
		pkg2_replace_kip(info, kip1->tid, kip1);
	else
		pkg2_add_kip(info, kip1);
}

int pkg2_decompress_kip(pkg2_kip1_info_t* ki, u32 sectsToDecomp)
{
	u32 compClearMask = ~sectsToDecomp;
	if ((ki->kip1->flags & compClearMask) == ki->kip1->flags)
		return 0; // Already decompressed, nothing to do.

	pkg2_kip1_t hdr;
	memcpy(&hdr, ki->kip1, sizeof(hdr));

	unsigned int newKipSize = sizeof(hdr);
	for (u32 sectIdx = 0; sectIdx < KIP1_NUM_SECTIONS; sectIdx++)
	{
		u32 sectCompBit = 1u << sectIdx;
		// For compressed, cant get actual decompressed size without doing it, so use safe "output size".
		if (sectIdx < 3 && (sectsToDecomp & sectCompBit) && (hdr.flags & sectCompBit))
			newKipSize += hdr.sections[sectIdx].size_decomp;
		else
			newKipSize += hdr.sections[sectIdx].size_comp;
	}

	pkg2_kip1_t* newKip = malloc(newKipSize);
	unsigned char* dstDataPtr = newKip->data;
	const unsigned char* srcDataPtr = ki->kip1->data;
	for (u32 sectIdx = 0; sectIdx < KIP1_NUM_SECTIONS; sectIdx++)
	{
		u32 sectCompBit = 1u << sectIdx;
		// Easy copy path for uncompressed or ones we dont want to uncompress.
		if (sectIdx >= 3 || !(sectsToDecomp & sectCompBit) || !(hdr.flags & sectCompBit))
		{
			unsigned int dataSize = hdr.sections[sectIdx].size_comp;
			if (dataSize == 0)
				continue;

			memcpy(dstDataPtr, srcDataPtr, dataSize);
			srcDataPtr += dataSize;
			dstDataPtr += dataSize;
			continue;
		}

		unsigned int compSize = hdr.sections[sectIdx].size_comp;
		unsigned int outputSize = hdr.sections[sectIdx].size_decomp;
		gfx_printf("Decomping %s KIP1 sect %d of size %d...\n", (const char*)hdr.name, sectIdx, compSize);
		if (blz_uncompress_srcdest(srcDataPtr, compSize, dstDataPtr, outputSize) == 0)
		{
			gfx_con.mute = false;
			gfx_printf("%kERROR decomping sect %d of %s KIP!%k\n", 0xFFFF0000, sectIdx, (char*)hdr.name, 0xFFCCCCCC);
			free(newKip);

			return 1;
		}
		else
		{
			DPRINTF("Done! Decompressed size is %d!\n", outputSize);
		}
		hdr.sections[sectIdx].size_comp = outputSize;
		srcDataPtr += compSize;
		dstDataPtr += outputSize;
	}

	hdr.flags &= compClearMask;
	memcpy(newKip, &hdr, sizeof(hdr));
	newKipSize = dstDataPtr-(unsigned char*)(newKip);

	free(ki->kip1);
	ki->kip1 = newKip;
	ki->size = newKipSize;

	return 0;
}

static int _kipm_inject(const char *kipm_path, char *target_name, pkg2_kip1_info_t* ki)
{
	if (!strcmp((const char *)ki->kip1->name, target_name))
	{
		u32 size = 0;
		u8 *kipm_data = (u8 *)sd_file_read(kipm_path, &size);
		if (!kipm_data)
			return 1;

		u32 inject_size = size - sizeof(ki->kip1->caps);
		u8 *kip_patched_data = (u8 *)malloc(ki->size + inject_size);

		// Copy headers.
		memcpy(kip_patched_data, ki->kip1, sizeof(pkg2_kip1_t));

		pkg2_kip1_t *fs_kip = ki->kip1;
		ki->kip1 = (pkg2_kip1_t *)kip_patched_data;
		ki->size = ki->size + inject_size;

		// Patch caps.
		memcpy(&ki->kip1->caps, kipm_data, sizeof(ki->kip1->caps));
		// Copy our .text data.
		memcpy(&ki->kip1->data, kipm_data + sizeof(ki->kip1->caps), inject_size);

		u32 new_offset = 0;

		for (u32 currSectIdx = 0; currSectIdx < KIP1_NUM_SECTIONS - 2; currSectIdx++)
		{
			if(!currSectIdx) // .text.
			{
				memcpy(ki->kip1->data + inject_size, fs_kip->data, fs_kip->sections[0].size_comp);
				ki->kip1->sections[0].size_decomp += inject_size;
				ki->kip1->sections[0].size_comp += inject_size;
			}
			else // Others.
			{
				if (currSectIdx < 3)
					memcpy(ki->kip1->data + new_offset + inject_size, fs_kip->data + new_offset, fs_kip->sections[currSectIdx].size_comp);
				ki->kip1->sections[currSectIdx].offset += inject_size;
			}
			new_offset += fs_kip->sections[currSectIdx].size_comp;
		}

		// Patch PMC capabilities for 1.0.0.
		if (!emu_cfg.fs_ver)
		{
			for (u32 i = 0; i < 0x20; i++)
			{
				if (ki->kip1->caps[i] == 0xFFFFFFFF)
				{
					ki->kip1->caps[i] = 0x07000E7F;
					break;
				}
			}
		}

		free(kipm_data);
		return 0;
	}

	return 1;
}

static bool ext_patches_parsed = false;

const char* pkg2_patch_kips(link_t *info, char* patchNames)
{
	if (patchNames == NULL || patchNames[0] == 0)
		return NULL;

	if (!ext_patches_parsed)
	{
		parse_external_kip_patches();
		ext_patches_parsed = true;
	}

	static const u32 MAX_NUM_PATCHES_REQUESTED = sizeof(u32) * 8;
	char* patches[MAX_NUM_PATCHES_REQUESTED];

	u32 numPatches = 1;
	patches[0] = patchNames;
	{
		for (char* p = patchNames; *p != 0; p++)
		{
			if (*p == ',')
			{
				*p = 0;
				patches[numPatches++] = p + 1;
				if (numPatches >= MAX_NUM_PATCHES_REQUESTED)
					return "too_many_patches";
			}
			else if (*p >= 'A' && *p <= 'Z') // Convert to lowercase.
				*p += 0x20;
		}
	}

	u32 patchesApplied = 0; // Bitset over patches.
	for (u32 i = 0; i < numPatches; i++)
	{
		// Eliminate leading spaces.
		for (const char* p = patches[i]; *p != 0; p++)
		{
			if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
				patches[i]++;
			else
				break;
		}
		int valueLen = strlen(patches[i]);
		if (valueLen == 0)
			continue;

		// Eliminate trailing spaces.
		for (int chIdx = valueLen - 1; chIdx >= 0; chIdx--)
		{
			const char* p = patches[i] + chIdx;
			if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
				valueLen = chIdx;
			else
				break;
		}
		patches[i][valueLen] = 0;

		DPRINTF("Requested patch: '%s'\n", patches[i]);
	}

	u32 shaBuf[32 / sizeof(u32)];
	LIST_FOREACH_ENTRY(pkg2_kip1_info_t, ki, info, link)
	{
		shaBuf[0] = 0; // sha256 for this kip not yet calculated.
		for (u32 currKipIdx = 0; currKipIdx < _kip_id_sets_cnt; currKipIdx++)
		{
			if (strncmp((const char*)ki->kip1->name, _kip_id_sets[currKipIdx].name, sizeof(ki->kip1->name)) != 0)
				continue;

			u32 bitsAffected = 0;
			kip1_patchset_t* currPatchset = _kip_id_sets[currKipIdx].patchset;
			while (currPatchset != NULL && currPatchset->name != NULL)
			{
				for (u32 i = 0; i < numPatches; i++)
				{
					// Continue if patch name does not match.
					if (strcmp(currPatchset->name, patches[i]) != 0)
						continue;

					bitsAffected = i + 1;
					break;
				}
				currPatchset++;
			}

			// Dont bother even hashing this KIP if we dont have any patches enabled for it.
			if (bitsAffected == 0)
				continue;

			if (shaBuf[0] == 0)
			{
				if (!se_calc_sha256_oneshot(shaBuf, ki->kip1, ki->size))
					memset(shaBuf, 0, sizeof(shaBuf));
			}

			if (memcmp(shaBuf, _kip_id_sets[currKipIdx].hash, sizeof(_kip_id_sets[0].hash)) != 0)
				continue;

			// Find out which sections are affected by the enabled patches, to know which to decompress.
			bitsAffected = 0;
			currPatchset = _kip_id_sets[currKipIdx].patchset;
			while (currPatchset != NULL && currPatchset->name != NULL)
			{
				if (currPatchset->patches != NULL)
				{
					for (u32 currEnabIdx = 0; currEnabIdx < numPatches; currEnabIdx++)
					{
						if (strcmp(currPatchset->name, patches[currEnabIdx]))
							continue;

						if (!strcmp(currPatchset->name, "emummc"))
							bitsAffected |= 1u << GET_KIP_PATCH_SECTION(currPatchset->patches->offset);

						for (const kip1_patch_t* currPatch=currPatchset->patches; currPatch != NULL && (currPatch->length != 0); currPatch++)
							bitsAffected |= 1u << GET_KIP_PATCH_SECTION(currPatch->offset);
					}
				}
				currPatchset++;
			}

			// Got patches to apply to this kip, have to decompress it.
#ifdef DEBUG_PRINTING
			u32 preDecompTime = get_tmr_us();
#endif
			if (pkg2_decompress_kip(ki, bitsAffected))
				return (const char*)ki->kip1->name; // Failed to decompress.

#ifdef DEBUG_PRINTING
			u32 postDecompTime = get_tmr_us();
			if (!se_calc_sha256_oneshot(shaBuf, ki->kip1, ki->size))
				memset(shaBuf, 0, sizeof(shaBuf));

			DPRINTF("%dms %s KIP1 size %d hash %08X\n", (postDecompTime-preDecompTime) / 1000, ki->kip1->name, (int)ki->size, __builtin_bswap32(shaBuf[0]));
#endif

			currPatchset = _kip_id_sets[currKipIdx].patchset;
			bool emummc_patch_selected = false;
			while (currPatchset != NULL && currPatchset->name != NULL)
			{
				for (u32 currEnabIdx = 0; currEnabIdx < numPatches; currEnabIdx++)
				{
					if (strcmp(currPatchset->name, patches[currEnabIdx]))
						continue;

					u32 appliedMask = 1u << currEnabIdx;

					if (!strcmp(currPatchset->name, "emummc"))
					{
						emummc_patch_selected = true;
						patchesApplied |= appliedMask;

						continue; // Continue in case it's double defined.
					}

					if (currPatchset->patches == NULL)
					{
						DPRINTF("Patch '%s' not necessary for %s KIP1\n", currPatchset->name, (const char*)ki->kip1->name);
						patchesApplied |= appliedMask;

						continue; // Continue in case it's double defined.
					}

					unsigned char* kipSectData = ki->kip1->data;
					for (u32 currSectIdx = 0; currSectIdx < KIP1_NUM_SECTIONS; currSectIdx++)
					{
						if (bitsAffected & (1u << currSectIdx))
						{
							gfx_printf("Applying patch '%s' on %s KIP1 sect %d\n", currPatchset->name, (const char*)ki->kip1->name, currSectIdx);
							for (const kip1_patch_t* currPatch = currPatchset->patches; currPatch != NULL && currPatch->srcData != 0; currPatch++)
							{
								if (GET_KIP_PATCH_SECTION(currPatch->offset) != currSectIdx)
									continue;

								if (!currPatch->length)
								{
									gfx_con.mute = false;
									gfx_printf("%kPatch is empty!%k\n", 0xFFFF0000, 0xFFCCCCCC);
									return currPatchset->name; // MUST stop here as it's not probably intended.
								}

								u32 currOffset = GET_KIP_PATCH_OFFSET(currPatch->offset);
								// If source does not match and is not already patched, throw an error.
								if ((memcmp(&kipSectData[currOffset], currPatch->srcData, currPatch->length) != 0) &&
									(memcmp(&kipSectData[currOffset], currPatch->dstData, currPatch->length) != 0))
								{
									gfx_con.mute = false;
									gfx_printf("%kPatch data mismatch at 0x%x!%k\n", 0xFFFF0000, currOffset, 0xFFCCCCCC);
									return currPatchset->name; // MUST stop here as kip is likely corrupt.
								}
								else
								{
									DPRINTF("Patching %d bytes at offset 0x%x\n", currPatch->length, currOffset);
									memcpy(&kipSectData[currOffset], currPatch->dstData, currPatch->length);
								}
							}
						}
						kipSectData += ki->kip1->sections[currSectIdx].size_comp;
					}

					patchesApplied |= appliedMask;
					continue; // Continue in case it's double defined.
				}
				currPatchset++;
			}
			if (emummc_patch_selected && !strncmp(_kip_id_sets[currKipIdx].name, "FS", sizeof(ki->kip1->name)))
			{
				emummc_patch_selected = false;
				emu_cfg.fs_ver = currKipIdx;
				if (currKipIdx)
					emu_cfg.fs_ver--;
				if (currKipIdx > 17)
					emu_cfg.fs_ver -= 2;

				gfx_printf("Injecting emuMMC. FS ver: %d\n", emu_cfg.fs_ver);
				if (_kipm_inject("/bootloader/sys/emummc.kipm", "FS", ki))
					return "emummc";
			}
		}
	}

	for (u32 i = 0; i < numPatches; i++)
	{
		if ((patchesApplied & (1u << i)) == 0)
			return patches[i];
	}

	return NULL;
}

//!TODO: Update on mkey changes.
static const u8 mkey_vector_7xx[][SE_KEY_128_SIZE] =
{
	// Master key 7  encrypted with 8.  (7.0.0 with 8.1.0)
	{ 0xEA, 0x60, 0xB3, 0xEA, 0xCE, 0x8F, 0x24, 0x46, 0x7D, 0x33, 0x9C, 0xD1, 0xBC, 0x24, 0x98, 0x29 },
	// Master key 8  encrypted with 9.  (8.1.0 with 9.0.0)
	{ 0x4D, 0xD9, 0x98, 0x42, 0x45, 0x0D, 0xB1, 0x3C, 0x52, 0x0C, 0x9A, 0x44, 0xBB, 0xAD, 0xAF, 0x80 },
	// Master key 9  encrypted with 10. (9.0.0 with 9.1.0)
	{ 0xB8, 0x96, 0x9E, 0x4A, 0x00, 0x0D, 0xD6, 0x28, 0xB3, 0xD1, 0xDB, 0x68, 0x5F, 0xFB, 0xE1, 0x2A },
	// Master key 10 encrypted with 11. (9.1.0 with 12.1.0)
	{ 0xC1, 0x8D, 0x16, 0xBB, 0x2A, 0xE4, 0x1D, 0xD4, 0xC2, 0xC1, 0xB6, 0x40, 0x94, 0x35, 0x63, 0x98 },
};

static bool _pkg2_key_unwrap_validate(pkg2_hdr_t *tmp_test, pkg2_hdr_t *hdr, u8 src_slot, u8 *mkey, const u8 *key_seed)
{
	// Decrypt older encrypted mkey.
	se_aes_crypt_ecb(src_slot, DECRYPT, mkey, SE_KEY_128_SIZE, key_seed, SE_KEY_128_SIZE);
	// Set and unwrap pkg2 key.
	se_aes_key_set(9, mkey, SE_KEY_128_SIZE);
	se_aes_unwrap_key(9, 9, package2_keyseed);

	// Decrypt header.
	se_aes_crypt_ctr(9, tmp_test, sizeof(pkg2_hdr_t), hdr, sizeof(pkg2_hdr_t), hdr);

	// Return if header is valid.
	return (tmp_test->magic == PKG2_MAGIC);
}

u8 pkg2_keyslot;
bool pkg2_broken_keygen_700;
pkg2_hdr_t *pkg2_decrypt(void *data, u8 kb, bool is_exo)
{
	pkg2_hdr_t mkey_test;
	u8 *pdata = (u8 *)data;
	pkg2_keyslot = 8;
	pkg2_broken_keygen_700 = false;

	// Skip signature.
	pdata += 0x100;

	pkg2_hdr_t *hdr = (pkg2_hdr_t *)pdata;

	// Skip header.
	pdata += sizeof(pkg2_hdr_t);

	// Check if we need to decrypt with newer mkeys. Valid for THK for 7.0.0 and up.
	se_aes_crypt_ctr(8, &mkey_test, sizeof(pkg2_hdr_t), hdr, sizeof(pkg2_hdr_t), hdr);

	if (mkey_test.magic == PKG2_MAGIC)
		goto key_found;

	// Decrypt older pkg2 via new mkeys.
	if ((kb >= KB_FIRMWARE_VERSION_700) && (kb < KB_FIRMWARE_VERSION_MAX))
	{
		u8 tmp_mkey[SE_KEY_128_SIZE];
		u8 decr_slot = (h_cfg.t210b01 || !is_exo) ? 7 : 13; // THK mkey or T210B01 mkey.
		u8 mkey_seeds_cnt = sizeof(mkey_vector_7xx) / SE_KEY_128_SIZE;
		u8 mkey_seeds_idx = mkey_seeds_cnt; // Real index + 1.
		u8 mkey_seeds_min_idx = mkey_seeds_cnt - (KB_FIRMWARE_VERSION_MAX - kb);
		// Re-encrypt with initial pkg2 key if 7.0.0 and Erista, because of a bug in Exo2.
		pkg2_broken_keygen_700 = kb == KB_FIRMWARE_VERSION_700 && decr_slot == 13;

		while (mkey_seeds_cnt)
		{
			// Decrypt and validate mkey.
			int res = _pkg2_key_unwrap_validate(&mkey_test, hdr, decr_slot,
				tmp_mkey, mkey_vector_7xx[mkey_seeds_idx - 1]);

			if (res)
			{
				pkg2_keyslot = 9;
				goto key_found;
			}
			else
			{
				// Set current mkey in order to decrypt a lower mkey.
				mkey_seeds_idx--;
				se_aes_key_set(9, tmp_mkey, SE_KEY_128_SIZE);

				decr_slot = 9; // Temp key.

				// Check if we tried last key for that pkg2 version.
				// And start with a lower mkey in case mkey is older.
				if (mkey_seeds_idx == mkey_seeds_min_idx)
				{
					mkey_seeds_cnt--;
					mkey_seeds_idx = mkey_seeds_cnt;
					decr_slot = (h_cfg.t210b01 || !is_exo) ? 7 : 13; // THK mkey or T210B01 mkey.
				}
			}
		}
	}

key_found:
	// Decrypt header.
	se_aes_crypt_ctr(pkg2_keyslot, hdr, sizeof(pkg2_hdr_t), hdr, sizeof(pkg2_hdr_t), hdr);
	//gfx_hexdump((u32)hdr, hdr, 0x100);

	if (hdr->magic != PKG2_MAGIC)
		return NULL;

	for (u32 i = 0; i < 4; i++)
	{
DPRINTF("sec %d has size %08X\n", i, hdr->sec_size[i]);
		if (!hdr->sec_size[i])
			continue;

		se_aes_crypt_ctr(pkg2_keyslot, pdata, hdr->sec_size[i], pdata, hdr->sec_size[i], &hdr->sec_ctr[i * SE_AES_IV_SIZE]);
		//gfx_hexdump((u32)pdata, pdata, 0x100);

		pdata += hdr->sec_size[i];
	}

	if (pkg2_broken_keygen_700)
		pkg2_keyslot = 8;

	return hdr;
}

static u32 _pkg2_ini1_build(u8 *pdst, pkg2_hdr_t *hdr, link_t *kips_info, bool new_pkg2)
{
	u32 ini1_size = sizeof(pkg2_ini1_t);
	pkg2_ini1_t *ini1 = (pkg2_ini1_t *)pdst;
	memset(ini1, 0, sizeof(pkg2_ini1_t));
	ini1->magic = INI1_MAGIC;
	pdst += sizeof(pkg2_ini1_t);
	LIST_FOREACH_ENTRY(pkg2_kip1_info_t, ki, kips_info, link)
	{
DPRINTF("adding kip1 '%s' @ %08X (%08X)\n", ki->kip1->name, (u32)ki->kip1, ki->size);
		memcpy(pdst, ki->kip1, ki->size);
		pdst += ki->size;
		ini1_size += ki->size;
		ini1->num_procs++;
	}
	ini1_size = ALIGN(ini1_size, 4);
	ini1->size = ini1_size;
	if (!new_pkg2)
	{
		hdr->sec_size[PKG2_SEC_INI1] = ini1_size;
		hdr->sec_off[PKG2_SEC_INI1] = 0x14080000;
		se_aes_crypt_ctr(8, ini1, ini1_size, ini1, ini1_size, &hdr->sec_ctr[PKG2_SEC_INI1 * SE_AES_IV_SIZE]);
	}
	else
	{
		hdr->sec_size[PKG2_SEC_INI1] = 0;
		hdr->sec_off[PKG2_SEC_INI1] = 0;
	}

	return ini1_size;
}

void pkg2_build_encrypt(void *dst, void *hos_ctxt, link_t *kips_info)
{
	u8 *pdst = (u8 *)dst;
	launch_ctxt_t * ctxt = (launch_ctxt_t *)hos_ctxt;
	u32 kernel_size = ctxt->kernel_size;
	bool is_meso = *(u32 *)(ctxt->kernel + 4) == ATM_MESOSPHERE;
	u8 kb = ctxt->pkg1_id->kb;

	// Force new Package2 if Mesosphere.
	if (is_meso)
		ctxt->new_pkg2 = true;

	// Signature.
	memset(pdst, 0, 0x100);
	pdst += 0x100;

	// Header.
	pkg2_hdr_t *hdr = (pkg2_hdr_t *)pdst;
	memset(hdr, 0, sizeof(pkg2_hdr_t));

	// Set initial header values.
	hdr->magic = PKG2_MAGIC;
	hdr->bl_ver = 0;
	hdr->pkg2_ver = 0xFF;

	if (!ctxt->new_pkg2)
		hdr->base = 0x10000000;
	else
		hdr->base = 0x60000;
DPRINTF("%s @ %08X (%08X)\n", is_meso ? "Mesosphere": "kernel",(u32)ctxt->kernel, kernel_size);

	pdst += sizeof(pkg2_hdr_t);

	// Kernel.
	memcpy(pdst, ctxt->kernel, kernel_size);
	if (!ctxt->new_pkg2)
		hdr->sec_off[PKG2_SEC_KERNEL] = 0x10000000;
	else
	{
		// Set new INI1 offset to kernel.
		*(u32 *)(pdst + (is_meso ? 8 : pkg2_newkern_ini1_val)) = kernel_size;

		// Build INI1 for new Package2.
		kernel_size += _pkg2_ini1_build(pdst + kernel_size, hdr, kips_info, ctxt->new_pkg2);
		hdr->sec_off[PKG2_SEC_KERNEL] = 0x60000;
	}
	hdr->sec_size[PKG2_SEC_KERNEL] = kernel_size;
	se_aes_crypt_ctr(pkg2_keyslot, pdst, kernel_size, pdst, kernel_size, &hdr->sec_ctr[PKG2_SEC_KERNEL * SE_AES_IV_SIZE]);
	pdst += kernel_size;
DPRINTF("kernel encrypted\n");

	/// Build INI1 for old Package2.
	u32 ini1_size = 0;
	if (!ctxt->new_pkg2)
		ini1_size = _pkg2_ini1_build(pdst, hdr, kips_info, false);
DPRINTF("INI1 encrypted\n");

	// Calculate SHA256 over encrypted Kernel and INI1.
	u8 *pk2_hash_data = (u8 *)dst + 0x100 + sizeof(pkg2_hdr_t);
	se_calc_sha256_oneshot(&hdr->sec_sha256[0x20 * PKG2_SEC_KERNEL],
		(void *)pk2_hash_data, hdr->sec_size[PKG2_SEC_KERNEL]);
	pk2_hash_data += hdr->sec_size[PKG2_SEC_KERNEL];
	se_calc_sha256_oneshot(&hdr->sec_sha256[0x20 * PKG2_SEC_INI1],
		(void *)pk2_hash_data, hdr->sec_size[PKG2_SEC_INI1]);

	// Set key version. For Erista 7.0.0, use max because of a bug in Exo2?
	u8 key_ver = kb ? kb + 1 : 0;
	if (pkg2_broken_keygen_700)
		key_ver = KB_FIRMWARE_VERSION_MAX + 1;

	//Encrypt header.
	*(u32 *)hdr->ctr = 0x100 + sizeof(pkg2_hdr_t) + kernel_size + ini1_size;
	hdr->ctr[4] = key_ver;
	se_aes_crypt_ctr(pkg2_keyslot, hdr, sizeof(pkg2_hdr_t), hdr, sizeof(pkg2_hdr_t), hdr);
	memset(hdr->ctr, 0 , SE_AES_IV_SIZE);
	*(u32 *)hdr->ctr = 0x100 + sizeof(pkg2_hdr_t) + kernel_size + ini1_size;
	hdr->ctr[4] = key_ver;
}
