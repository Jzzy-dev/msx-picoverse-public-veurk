// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// fs_service.c - File system service of the PicoVerse USB FDD firmware
//
// See fs_service.h
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include "ff.h"
#include "fs_service.h"

#define NAME_MAX_LEN    64      // maximum length of a name or pattern, with the terminator
#define DIR_ENTRY_SIZE  32

static FATFS fs;
static FIL   file;
static DIR   dir;
static bool  fs_mounted = false;
static bool  file_open = false;
static bool  file_dirty = false;
static bool  enum_open = false;
static uint8_t file_attr = 0;
static char  enum_pattern[NAME_MAX_LEN];

static bool  sync_failed = false;       // a flush of the open file failed

// Close the open file, returns false if the data could not be written to the media
static bool close_file(void)
{
    bool ok = true;

    if (file_open)
    {
        ok = (f_close(&file) == FR_OK);
        file_open = false;
    }
    file_dirty = false;

    ok = ok && !sync_failed;
    sync_failed = false;
    return ok;
}

static void close_enum(void)
{
    if (enum_open)
    {
        f_closedir(&dir);
        enum_open = false;
    }
}

void fs_service_invalidate(void)
{
    // Don't touch the media: the device is gone or about to be mounted again
    file_open = false;
    file_dirty = false;
    sync_failed = false;
    enum_open = false;
    if (fs_mounted)
    {
        f_mount(NULL, "", 0);
        fs_mounted = false;
    }
}

void fs_service_sync(void)
{
    if (file_open && file_dirty)
    {
        file_dirty = false;
        if (f_sync(&file) != FR_OK)
        {
            sync_failed = true;     // reported when the file is closed
        }
    }
}

bool fs_service_dirty(void)
{
    return file_open && file_dirty;
}

// Map a FatFs error to a mailbox result code
static uint8_t map_error(FRESULT r)
{
    switch (r)
    {
        case FR_OK:
            return PV_RES_OK;
        case FR_NO_FILE:
        case FR_NO_PATH:
            return PV_RES_MISS_FILE;
        case FR_NOT_READY:
        case FR_INVALID_DRIVE:
        case FR_NOT_ENABLED:
            return PV_RES_NO_DEVICE;
        default:
            return PV_RES_UNEXPECTED;
    }
}

// Get the zero terminated name that the MSX wrote at the beginning of the data buffer
static const char *get_name(pv_mailbox_t *mb)
{
    mb->buf[NAME_MAX_LEN - 1] = 0;
    return (const char *)mb->buf;
}

// Case insensitive wildcard match ('*' and '?') of a "NAME.EXT" string
static bool wild_match(const char *pat, const char *str)
{
    while (*pat)
    {
        if (*pat == '*')
        {
            while (*pat == '*')
            {
                pat++;
            }
            if (!*pat)
            {
                return true;
            }
            for (; *str; str++)
            {
                if (wild_match(pat, str))
                {
                    return true;
                }
            }
            return false;
        }

        if (!*str)
        {
            return false;
        }

        char p = *pat, s = *str;
        if (p >= 'a' && p <= 'z') p -= 32;
        if (s >= 'a' && s <= 'z') s -= 32;
        if (p != '?' && p != s)
        {
            return false;
        }
        pat++;
        str++;
    }
    return *str == 0;
}

// Build a raw 32 byte directory entry (as the CH376 returned) from the FatFs file info
static void build_dir_entry(uint8_t *e, const FILINFO *fno)
{
    memset(e, 0, DIR_ENTRY_SIZE);
    memset(e, ' ', 11);

    const char *n = fno->fname;
    int i = 0;
    while (*n && *n != '.' && i < 8)
    {
        e[i++] = (uint8_t)*n++;
    }
    while (*n && *n != '.')
    {
        n++;
    }
    if (*n == '.')
    {
        n++;
        for (i = 8; *n && i < 11; i++, n++)
        {
            e[i] = (uint8_t)*n;
        }
    }

    e[11] = fno->fattrib;
    e[22] = (uint8_t)(fno->ftime & 0xFF);
    e[23] = (uint8_t)(fno->ftime >> 8);
    e[24] = (uint8_t)(fno->fdate & 0xFF);
    e[25] = (uint8_t)(fno->fdate >> 8);
    e[28] = (uint8_t)(fno->fsize & 0xFF);
    e[29] = (uint8_t)((fno->fsize >> 8) & 0xFF);
    e[30] = (uint8_t)((fno->fsize >> 16) & 0xFF);
    e[31] = (uint8_t)((fno->fsize >> 24) & 0xFF);
}

static uint8_t cmd_mount(pv_mailbox_t *mb)
{
    // The MSX may have been reset with an open file: flush it before mounting again
    if (fs_mounted && fs_dev_present())
    {
        close_file();
        close_enum();
    }
    fs_service_invalidate();

    if (!fs_dev_present())
    {
        return PV_RES_NO_DEVICE;
    }

    FRESULT r = f_mount(&fs, "", 1);
    if (r != FR_OK)
    {
        return PV_RES_UNEXPECTED;
    }
    fs_mounted = true;

    r = f_chdir("/");
    if (r != FR_OK)
    {
        return map_error(r);
    }

    fs_dev_name((char *)mb->buf);
    return PV_RES_OK;
}

static uint8_t cmd_open(pv_mailbox_t *mb)
{
    const char *name = get_name(mb);
    FILINFO fno;
    FRESULT r;

    close_file();
    close_enum();

    if (name[0] == '/' && name[1] == 0)
    {
        return f_chdir("/") == FR_OK ? PV_RES_OPEN_DIR : PV_RES_UNEXPECTED;
    }

    r = f_stat(name, &fno);
    if (r == FR_OK)
    {
        if (fno.fattrib & AM_DIR)
        {
            r = f_chdir(name);
            return r == FR_OK ? PV_RES_OPEN_DIR : map_error(r);
        }

        // Read only files are opened for reading only
        r = f_open(&file, name, FA_READ | FA_WRITE | FA_OPEN_EXISTING);
        if (r == FR_DENIED)
        {
            r = f_open(&file, name, FA_READ | FA_OPEN_EXISTING);
        }
        if (r != FR_OK)
        {
            return map_error(r);
        }
        file_open = true;
        file_attr = fno.fattrib;
        return PV_RES_OK;
    }

    if (r == FR_INVALID_NAME)
    {
        // The "." and ".." entries are not files and f_stat doesn't know them
        r = f_chdir(name);
        return r == FR_OK ? PV_RES_OPEN_DIR : map_error(r);
    }

    return map_error(r);
}

static uint8_t cmd_create(pv_mailbox_t *mb)
{
    const char *name = get_name(mb);

    close_file();
    close_enum();

    FRESULT r = f_open(&file, name, FA_CREATE_ALWAYS | FA_READ | FA_WRITE);
    if (r != FR_OK)
    {
        return map_error(r) == PV_RES_NO_DEVICE ? PV_RES_NO_DEVICE : PV_RES_UNEXPECTED;
    }
    file_open = true;
    file_attr = AM_ARC;
    return PV_RES_OK;
}

static uint8_t cmd_mkdir(pv_mailbox_t *mb)
{
    const char *name = get_name(mb);

    close_file();
    close_enum();

    FRESULT r = f_mkdir(name);
    if (r != FR_OK && r != FR_EXIST)
    {
        return PV_RES_UNEXPECTED;
    }

    // Like the CH376, creating a directory also enters it (this fails if a file has that name)
    r = f_chdir(name);
    return r == FR_OK ? PV_RES_OK : PV_RES_UNEXPECTED;
}

static uint8_t cmd_delete(pv_mailbox_t *mb)
{
    const char *name = get_name(mb);

    close_file();
    close_enum();

    return map_error(f_unlink(name));
}

static uint8_t cmd_enum_next(pv_mailbox_t *mb)
{
    FILINFO fno;

    if (!enum_open)
    {
        return PV_RES_MISS_FILE;
    }

    while (true)
    {
        FRESULT r = f_readdir(&dir, &fno);
        if (r != FR_OK)
        {
            close_enum();
            return map_error(r);
        }

        if (fno.fname[0] == 0)
        {
            close_enum();
            return PV_RES_MISS_FILE;
        }

        if (fno.fname[0] == '.')
        {
            continue;   // "." and ".."
        }

        if (wild_match(enum_pattern, fno.fname))
        {
            build_dir_entry(mb->buf, &fno);
            return PV_RES_ENTRY;
        }
    }
}

static uint8_t cmd_enum_first(pv_mailbox_t *mb)
{
    const char *pattern = get_name(mb);

    close_enum();

    strncpy(enum_pattern, pattern, NAME_MAX_LEN - 1);
    enum_pattern[NAME_MAX_LEN - 1] = 0;

    FRESULT r = f_opendir(&dir, "");
    if (r != FR_OK)
    {
        return map_error(r);
    }
    enum_open = true;

    return cmd_enum_next(mb);
}

static uint8_t cmd_seek(pv_mailbox_t *mb)
{
    if (!file_open)
    {
        return PV_RES_UNEXPECTED;
    }

    uint32_t pos = (uint32_t)mb->arg[0] | ((uint32_t)mb->arg[1] << 8) |
                   ((uint32_t)mb->arg[2] << 16) | ((uint32_t)mb->arg[3] << 24);

    // Seeking beyond the end of the file never extends it, it moves to the end and
    // reports it (the CH376 returned an invalid pointer, FFFFFFFFh)
    mb->resp[0] = 0;
    if (pos > f_size(&file))
    {
        pos = f_size(&file);
        mb->resp[0] = 1;
    }

    return f_lseek(&file, pos) == FR_OK ? PV_RES_OK : PV_RES_UNEXPECTED;
}

static uint8_t cmd_read(pv_mailbox_t *mb)
{
    uint32_t len = (uint32_t)mb->arg[0] | ((uint32_t)mb->arg[1] << 8);
    UINT br = 0;

    if (!file_open || len > PV_BUF_SIZE)
    {
        return PV_RES_UNEXPECTED;
    }

    FRESULT r = f_read(&file, mb->buf, len, &br);
    mb->resp[0] = (uint8_t)(br & 0xFF);
    mb->resp[1] = (uint8_t)(br >> 8);
    return r == FR_OK ? PV_RES_OK : PV_RES_UNEXPECTED;
}

static uint8_t cmd_write(pv_mailbox_t *mb)
{
    uint32_t len = (uint32_t)mb->arg[0] | ((uint32_t)mb->arg[1] << 8);
    UINT bw = 0;

    if (!file_open || len > PV_BUF_SIZE)
    {
        return PV_RES_UNEXPECTED;
    }

    FRESULT r = f_write(&file, mb->buf, len, &bw);
    mb->resp[0] = (uint8_t)(bw & 0xFF);
    mb->resp[1] = (uint8_t)(bw >> 8);
    if (bw > 0)
    {
        file_dirty = true;
    }

    // A short write means that the disk is full
    return (r == FR_OK && bw == len) ? PV_RES_OK : PV_RES_UNEXPECTED;
}

uint8_t fs_service_command(uint8_t cmd, pv_mailbox_t *mb)
{
    if (!fs_dev_present())
    {
        fs_service_invalidate();
        return PV_RES_NO_DEVICE;
    }

    if (cmd != PV_CMD_FS_MOUNT && !fs_mounted)
    {
        return PV_RES_NO_DEVICE;
    }

    switch (cmd)
    {
        case PV_CMD_FS_MOUNT:      return cmd_mount(mb);
        case PV_CMD_FS_OPEN:       return cmd_open(mb);
        case PV_CMD_FS_CREATE:     return cmd_create(mb);
        case PV_CMD_FS_MKDIR:      return cmd_mkdir(mb);
        case PV_CMD_FS_CLOSE:      return close_file() ? PV_RES_OK : PV_RES_UNEXPECTED;
        case PV_CMD_FS_DELETE:     return cmd_delete(mb);
        case PV_CMD_FS_ENUM_FIRST: return cmd_enum_first(mb);
        case PV_CMD_FS_ENUM_NEXT:  return cmd_enum_next(mb);
        case PV_CMD_FS_SEEK:       return cmd_seek(mb);
        case PV_CMD_FS_READ:       return cmd_read(mb);
        case PV_CMD_FS_WRITE:      return cmd_write(mb);
        case PV_CMD_FS_ATTR:
            if (!file_open)
            {
                return PV_RES_UNEXPECTED;
            }
            mb->resp[0] = file_attr;
            return PV_RES_OK;
        default:
            return PV_RES_UNEXPECTED;
    }
}
