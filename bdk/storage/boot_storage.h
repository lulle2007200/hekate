#ifndef _BOOT_STORAGE_H
#define _BOOT_STORAGE_H

#include <utils/types.h>

// check if boot1 (1mb), boot1, gpp, sd (in that order) have fat32 partition,
// mount the partition and set the current drive to it
bool boot_storage_mount();

void boot_storage_unmount();
void boot_storage_end();

bool boot_storage_get_mounted();
bool boot_storage_get_initialized();

#endif