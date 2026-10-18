// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// fs_service.h - File system service of the PicoVerse USB FDD firmware
//
// Implements the file commands of the mailbox (PV_CMD_FS_*) on top of FatFs and a USB mass storage
// device. They replace the file commands of the CH376 that the MSXUSB version of the BIOS used,
// and keep their behavior: a current directory, at most one open file, and a directory enumeration
// that returns the raw 32 byte directory entries.
//
// This module doesn't depend on the Pico SDK, so it can be tested on the PC (see tests/).
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef FS_SERVICE_H
#define FS_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include "mailbox.h"

// Provided by the platform code
bool fs_dev_present(void);                // a mass storage device is ready
bool fs_dev_name(char *name28);           // 28 bytes: vendor (8), product (16) and revision (4)

// Reset the service state, the storage device was removed (or is about to be mounted)
void fs_service_invalidate(void);

// Execute a PV_CMD_FS_* command using the mailbox arguments and data buffer.
// Returns the result code and fills the response bytes and the data buffer.
uint8_t fs_service_command(uint8_t cmd, pv_mailbox_t *mb);

// Flush the open file if it was modified. Called when the MSX is not using the file system.
void fs_service_sync(void);

// True if the open file has data that was not flushed yet
bool fs_service_dirty(void);

#endif // FS_SERVICE_H
