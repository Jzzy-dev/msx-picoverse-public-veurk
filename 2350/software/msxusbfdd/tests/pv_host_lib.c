// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// pv_host_lib.c - Host library for the Z80 mailbox test (z80_mailbox_test.py)
//
// Exposes the real file system service (FatFs + fs_service.c) on top of a RAM disk, with the
// mailbox structure shared with the Python test, which emulates the Z80 and the mailbox registers.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "diskio.h"
#include "fs_service.h"

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

pv_mailbox_t pv_mbox;

static uint8_t *ram_disk;
static uint32_t ram_sectors;
static bool dev_present = false;
static FATFS tmp_fs;

DSTATUS disk_status(BYTE pdrv) { (void)pdrv; return ram_disk ? 0 : STA_NOINIT; }
DSTATUS disk_initialize(BYTE pdrv) { (void)pdrv; return ram_disk ? 0 : STA_NOINIT; }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    if (sector + count > ram_sectors) return RES_PARERR;
    memcpy(buff, ram_disk + (size_t)sector * 512, (size_t)count * 512);
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    if (sector + count > ram_sectors) return RES_PARERR;
    memcpy(ram_disk + (size_t)sector * 512, buff, (size_t)count * 512);
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)pdrv;
    switch (cmd)
    {
        case CTRL_SYNC: return RES_OK;
        case GET_SECTOR_COUNT: *(LBA_t *)buff = ram_sectors; return RES_OK;
        case GET_SECTOR_SIZE: *(WORD *)buff = 512; return RES_OK;
        case GET_BLOCK_SIZE: *(DWORD *)buff = 1; return RES_OK;
        default: return RES_PARERR;
    }
}

bool fs_dev_present(void) { return dev_present; }

bool fs_dev_name(char *name28)
{
    memcpy(name28, "TestVndrTest Drive      0.01", 28);
    return true;
}

// Create a FAT file system with some files, like a flash drive prepared on a PC
EXPORT int pv_host_init(uint32_t sectors, int fat32)
{
    free(ram_disk);
    ram_sectors = sectors;
    ram_disk = calloc(sectors, 512);

    static BYTE work[FF_MAX_SS * 4];
    MKFS_PARM opt = { .fmt = fat32 ? FM_FAT32 : FM_FAT, .n_fat = 2, .align = 0, .n_root = 0, .au_size = 0 };
    if (f_mkfs("", &opt, work, sizeof(work)) != FR_OK)
    {
        return -1;
    }

    f_mount(&tmp_fs, "", 1);

    FIL f;
    UINT bw;
    uint8_t *image = malloc(720 * 1024);
    for (uint32_t i = 0; i < 720 * 1024; i++)
    {
        image[i] = (uint8_t)((i * 31 + (i >> 8)) & 0xFF);
    }

    f_open(&f, "DISK1.DSK", FA_CREATE_ALWAYS | FA_WRITE); f_write(&f, image, 720 * 1024, &bw); f_close(&f);
    f_open(&f, "HIDDEN.DSK", FA_CREATE_ALWAYS | FA_WRITE); f_close(&f); f_chmod("HIDDEN.DSK", AM_HID, AM_HID);
    f_open(&f, "_SKIP.DSK", FA_CREATE_ALWAYS | FA_WRITE); f_close(&f);
    f_open(&f, "readme.txt", FA_CREATE_ALWAYS | FA_WRITE); f_write(&f, "hello", 5, &bw); f_close(&f);
    f_mkdir("GAMES");
    f_open(&f, "GAMES/ONE.DSK", FA_CREATE_ALWAYS | FA_WRITE); f_close(&f);
    f_open(&f, "GAMES/TWO.DSK", FA_CREATE_ALWAYS | FA_WRITE); f_close(&f);
    f_mount(NULL, "", 0);
    free(image);

    fs_service_invalidate();
    dev_present = true;
    return 0;
}

EXPORT void pv_host_set_present(int present)
{
    dev_present = present != 0;
}

EXPORT uint8_t pv_host_command(uint8_t cmd)
{
    return fs_service_command(cmd, &pv_mbox);
}

EXPORT void pv_host_sync(void)
{
    fs_service_sync();
}

EXPORT uint8_t *pv_host_mbox(void)
{
    return (uint8_t *)&pv_mbox;
}

EXPORT int pv_host_offset(int which)
{
    switch (which)
    {
        case 0: return (int)((uint8_t *)&pv_mbox.status - (uint8_t *)&pv_mbox);
        case 1: return (int)((uint8_t *)&pv_mbox.result - (uint8_t *)&pv_mbox);
        case 2: return (int)((uint8_t *)&pv_mbox.page - (uint8_t *)&pv_mbox);
        case 3: return (int)((uint8_t *)&pv_mbox.arg - (uint8_t *)&pv_mbox);
        case 4: return (int)((uint8_t *)&pv_mbox.resp - (uint8_t *)&pv_mbox);
        case 5: return (int)((uint8_t *)&pv_mbox.buf - (uint8_t *)&pv_mbox);
        default: return -1;
    }
}
