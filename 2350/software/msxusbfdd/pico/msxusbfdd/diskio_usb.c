// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// diskio_usb.c - FatFs low level disk I/O on top of the TinyUSB mass storage class host driver
//
// The functions block (running the USB host stack) until the transfer finishes, they are called
// from the mailbox command handlers running on core 1.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include "pico/stdlib.h"
#include "tusb.h"
#include "ff.h"
#include "diskio.h"
#include "usb_host.h"

#define MSC_TIMEOUT_MS      5000
#define MSC_MAX_SECTORS     64      // the USB transfer length of a command is limited to 16 bits

static volatile bool msc_done;
static volatile bool msc_ok;

static bool msc_complete_cb(uint8_t dev_addr, tuh_msc_complete_data_t const *cb_data)
{
    (void)dev_addr;

    msc_ok = (cb_data->csw->status == MSC_CSW_STATUS_PASSED);
    msc_done = true;
    return true;
}

// Wait for the end of a command. Returns false if the command failed or if the transfer timed out,
// in which case the device is dropped since the transfer can't be cancelled.
static bool msc_wait(void)
{
    absolute_time_t end = make_timeout_time_ms(MSC_TIMEOUT_MS);

    while (!msc_done)
    {
        tuh_task();
        if (!usb_msc_ready())
        {
            return false;   // the device was removed
        }
        if (time_reached(end))
        {
            usb_msc_fault();
            return false;
        }
    }
    return msc_ok;
}

DSTATUS disk_status(BYTE pdrv)
{
    (void)pdrv;
    return usb_msc_ready() ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    (void)pdrv;
    return usb_msc_ready() ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;

    if (!usb_msc_ready())
    {
        return RES_NOTRDY;
    }

    while (count)
    {
        UINT n = count > MSC_MAX_SECTORS ? MSC_MAX_SECTORS : count;

        msc_done = false;
        if (!tuh_msc_read10(usb_msc_addr(), 0, buff, (uint32_t)sector, (uint16_t)n, msc_complete_cb, 0))
        {
            return RES_ERROR;
        }
        if (!msc_wait())
        {
            return RES_ERROR;
        }

        buff += n * 512;
        sector += n;
        count -= n;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;

    if (!usb_msc_ready())
    {
        return RES_NOTRDY;
    }

    while (count)
    {
        UINT n = count > MSC_MAX_SECTORS ? MSC_MAX_SECTORS : count;

        msc_done = false;
        if (!tuh_msc_write10(usb_msc_addr(), 0, buff, (uint32_t)sector, (uint16_t)n, msc_complete_cb, 0))
        {
            return RES_ERROR;
        }
        if (!msc_wait())
        {
            return RES_ERROR;
        }

        buff += n * 512;
        sector += n;
        count -= n;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)pdrv;

    if (!usb_msc_ready())
    {
        return RES_NOTRDY;
    }

    switch (cmd)
    {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            *(LBA_t *)buff = tuh_msc_get_block_count(usb_msc_addr(), 0);
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD *)buff = 512;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *(DWORD *)buff = 1;
            return RES_OK;
        default:
            return RES_PARERR;
    }
}
