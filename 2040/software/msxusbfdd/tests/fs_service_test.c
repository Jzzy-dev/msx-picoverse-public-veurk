// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// fs_service_test.c - Host test of the file system service (FatFs + mailbox file commands)
//
// Builds a RAM disk with FAT12, FAT16 and FAT32 file systems and exercises the PV_CMD_FS_*
// commands the same way the BIOS does: mount, directory navigation, DSK image creation,
// sector reads and writes, enumeration with the "_" and hidden filtering done by the MSX, etc.
//
// Build and run with "make" in this directory.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "diskio.h"
#include "fs_service.h"

pv_mailbox_t pv_mbox;

// ---------------------------------------------------------------------------
// RAM disk
// ---------------------------------------------------------------------------
static uint8_t *ram_disk;
static uint32_t ram_sectors;
static bool dev_present = true;
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

// ---------------------------------------------------------------------------
// Helpers that mimic what the BIOS does
// ---------------------------------------------------------------------------
static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { printf("  FAIL (line %d): ", __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static uint8_t cmd(uint8_t c)
{
    return fs_service_command(c, &pv_mbox);
}

static void set_name(const char *s)
{
    strcpy((char *)pv_mbox.buf, s);
}

static uint8_t open_name(const char *s) { set_name(s); return cmd(PV_CMD_FS_OPEN); }
static uint8_t create_name(const char *s) { set_name(s); return cmd(PV_CMD_FS_CREATE); }
static uint8_t mkdir_name(const char *s) { set_name(s); return cmd(PV_CMD_FS_MKDIR); }
static uint8_t delete_name(const char *s) { set_name(s); return cmd(PV_CMD_FS_DELETE); }

static uint8_t seek_to(uint32_t pos, uint8_t *eof)
{
    pv_mbox.arg[0] = pos & 0xFF;
    pv_mbox.arg[1] = (pos >> 8) & 0xFF;
    pv_mbox.arg[2] = (pos >> 16) & 0xFF;
    pv_mbox.arg[3] = (pos >> 24) & 0xFF;
    uint8_t r = cmd(PV_CMD_FS_SEEK);
    *eof = pv_mbox.resp[0];
    return r;
}

// Read like HWF_READ_FILE: chunks of PV_BUF_SIZE
static uint32_t read_file(uint8_t *dst, uint32_t len)
{
    uint32_t total = 0;
    while (len)
    {
        uint32_t n = len > PV_BUF_SIZE ? PV_BUF_SIZE : len;
        pv_mbox.arg[0] = n & 0xFF;
        pv_mbox.arg[1] = n >> 8;
        if (cmd(PV_CMD_FS_READ) != PV_RES_OK) break;
        uint32_t got = pv_mbox.resp[0] | (pv_mbox.resp[1] << 8);
        memcpy(dst + total, pv_mbox.buf, got);
        total += got;
        len -= got;
        if (got < n) break;
    }
    return total;
}

static uint32_t write_file(const uint8_t *src, uint32_t len)
{
    uint32_t total = 0;
    while (len)
    {
        uint32_t n = len > PV_BUF_SIZE ? PV_BUF_SIZE : len;
        memcpy(pv_mbox.buf, src + total, n);
        pv_mbox.arg[0] = n & 0xFF;
        pv_mbox.arg[1] = n >> 8;
        if (cmd(PV_CMD_FS_WRITE) != PV_RES_OK) break;
        uint32_t put = pv_mbox.resp[0] | (pv_mbox.resp[1] << 8);
        total += put;
        len -= put;
    }
    return total;
}

// Enumerate like HWF_ENUM_FILES (MSX side filtering: "_", ".", hidden/system/volume)
static int enum_files(char names[][12], int max, bool *is_dir, const char *pattern)
{
    int count = 0;
    set_name(pattern);
    uint8_t r = cmd(PV_CMD_FS_ENUM_FIRST);
    while (r == PV_RES_ENTRY)
    {
        uint8_t *e = pv_mbox.buf;
        if (e[0] != '_' && e[0] != '.' && !(e[11] & 0x0E) && count < max)
        {
            memcpy(names[count], e, 11);
            names[count][11] = 0;
            is_dir[count] = (e[11] & 0x10) != 0;
            count++;
        }
        r = cmd(PV_CMD_FS_ENUM_NEXT);
    }
    CHECK(r == PV_RES_MISS_FILE, "enumeration ended with 0x%02X", r);
    return count;
}

static int find_name(char names[][12], int count, const char *n11)
{
    for (int i = 0; i < count; i++)
    {
        if (memcmp(names[i], n11, 11) == 0) return i;
    }
    return -1;
}

static void pattern_fill(uint8_t *b, uint32_t len, uint32_t seed)
{
    for (uint32_t i = 0; i < len; i++)
    {
        b[i] = (uint8_t)((i * 31 + seed * 7 + (i >> 8)) & 0xFF);
    }
}

// ---------------------------------------------------------------------------
// Test scenario
// ---------------------------------------------------------------------------
static void run_scenario(const char *label, uint32_t sectors, BYTE fmt)
{
    printf("== %s (%u sectors)\n", label, sectors);

    free(ram_disk);
    ram_sectors = sectors;
    ram_disk = calloc(sectors, 512);

    static BYTE work[FF_MAX_SS * 4];
    MKFS_PARM opt = { .fmt = fmt, .n_fat = 2, .align = 0, .n_root = 0, .au_size = 0 };
    FRESULT fr = f_mkfs("", &opt, work, sizeof(work));
    CHECK(fr == FR_OK, "f_mkfs failed (%d)", fr);
    f_mount(&tmp_fs, "", 1);
    // Files created by "other computers": a hidden file, an _underscore file, a lower case name, a directory
    FIL f;
    UINT bw;
    f_open(&f, "HIDDEN.DSK", FA_CREATE_ALWAYS | FA_WRITE); f_close(&f); f_chmod("HIDDEN.DSK", AM_HID, AM_HID);
    f_open(&f, "_SKIP.DSK", FA_CREATE_ALWAYS | FA_WRITE); f_close(&f);
    f_open(&f, "readme.txt", FA_CREATE_ALWAYS | FA_WRITE); f_write(&f, "hello", 5, &bw); f_close(&f);
    f_mkdir("GAMES");
    f_open(&f, "GAMES/ONE.DSK", FA_CREATE_ALWAYS | FA_WRITE); f_close(&f);
    f_mount(NULL, "", 0);

    fs_service_invalidate();

    // --- mount
    uint8_t r = cmd(PV_CMD_FS_MOUNT);
    CHECK(r == PV_RES_OK, "mount returned 0x%02X", r);
    CHECK(memcmp(pv_mbox.buf, "TestVndrTest Drive      0.01", 28) == 0, "device name");

    // --- enumeration of the root directory
    char names[32][12];
    bool is_dir[32];
    int n = enum_files(names, 32, is_dir, "*");
    CHECK(n == 2, "root has %d visible entries, expected 2", n);
    int i = find_name(names, n, "README  TXT");
    CHECK(i >= 0 && !is_dir[i], "README.TXT is listed as a file");
    i = find_name(names, n, "GAMES      ");
    CHECK(i >= 0 && is_dir[i], "GAMES is listed as a directory");

    // --- open: directory, missing file, parent, root
    r = open_name("GAMES");
    CHECK(r == PV_RES_OPEN_DIR, "open dir returned 0x%02X", r);
    n = enum_files(names, 32, is_dir, "*");
    CHECK(n == 1 && find_name(names, n, "ONE     DSK") == 0, "GAMES contains ONE.DSK");
    r = open_name("NOFILE.DSK");
    CHECK(r == PV_RES_MISS_FILE, "open missing file returned 0x%02X", r);
    r = open_name("..");
    CHECK(r == PV_RES_OPEN_DIR, "open .. returned 0x%02X", r);
    n = enum_files(names, 32, is_dir, "*");
    CHECK(n == 2, "back in the root directory");
    r = open_name("GAMES");
    r = open_name("/");
    CHECK(r == PV_RES_OPEN_DIR, "open / returned 0x%02X", r);

    // --- file open and attributes
    r = open_name("README.TXT");
    CHECK(r == PV_RES_OK, "open README.TXT returned 0x%02X", r);
    CHECK(cmd(PV_CMD_FS_ATTR) == PV_RES_OK && pv_mbox.resp[0] == AM_ARC, "attributes 0x%02X", pv_mbox.resp[0]);
    uint8_t buf[16];
    CHECK(read_file(buf, 16) == 5 && memcmp(buf, "hello", 5) == 0, "read README.TXT");
    uint8_t eof;
    CHECK(seek_to(100, &eof) == PV_RES_OK && eof == 1, "seek beyond the end reports end of file");
    CHECK(seek_to(1, &eof) == PV_RES_OK && eof == 0, "seek inside the file");
    CHECK(read_file(buf, 16) == 4 && memcmp(buf, "ello", 4) == 0, "read after seek");
    cmd(PV_CMD_FS_CLOSE);

    // --- the BIOS config directory/file pattern: _USB/CURDIR
    r = mkdir_name("_USB");
    CHECK(r == PV_RES_OK, "mkdir _USB returned 0x%02X", r);
    r = create_name("CURDIR");
    CHECK(r == PV_RES_OK, "create CURDIR returned 0x%02X", r);
    CHECK(write_file((const uint8_t *)"/GAMES", 6) == 6, "write CURDIR");
    cmd(PV_CMD_FS_CLOSE);
    open_name("/");
    r = open_name("_USB");
    CHECK(r == PV_RES_OPEN_DIR, "open _USB returned 0x%02X", r);
    r = open_name("CURDIR");
    CHECK(r == PV_RES_OK, "open CURDIR returned 0x%02X", r);
    memset(buf, 0, sizeof(buf));
    CHECK(read_file(buf, 16) == 6 && memcmp(buf, "/GAMES", 6) == 0, "read CURDIR back");
    cmd(PV_CMD_FS_CLOSE);
    r = delete_name("CURDIR");
    CHECK(r == PV_RES_OK, "delete CURDIR returned 0x%02X", r);
    r = open_name("CURDIR");
    CHECK(r == PV_RES_MISS_FILE, "CURDIR is gone (0x%02X)", r);
    open_name("/");
    n = enum_files(names, 32, is_dir, "*");
    CHECK(n == 2, "_USB is not listed (the MSX skips names starting with _)");

    // --- DEFAULT.DSK search with a pattern
    r = create_name("DEFAULT.DSK");
    CHECK(r == PV_RES_OK, "create DEFAULT.DSK returned 0x%02X", r);
    cmd(PV_CMD_FS_CLOSE);
    set_name("DEFAULT.DS*");
    r = cmd(PV_CMD_FS_ENUM_FIRST);
    CHECK(r == PV_RES_ENTRY && memcmp(pv_mbox.buf, "DEFAULT DSK", 11) == 0, "pattern DEFAULT.DS* finds DEFAULT.DSK");
    set_name("DEFAULT.DX*");
    r = cmd(PV_CMD_FS_ENUM_FIRST);
    CHECK(r == PV_RES_MISS_FILE, "pattern DEFAULT.DX* finds nothing (0x%02X)", r);

    // --- Create a 720K disk image and access sectors like DSKIO does
    const uint32_t img_size = 720 * 1024;
    uint8_t *image = malloc(img_size);
    uint8_t *check = malloc(img_size);
    pattern_fill(image, img_size, 1);

    r = create_name("NEW.DSK");
    CHECK(r == PV_RES_OK, "create NEW.DSK returned 0x%02X", r);
    CHECK(write_file(image, img_size) == img_size, "write the 720K image");
    cmd(PV_CMD_FS_CLOSE);

    r = open_name("NEW.DSK");
    CHECK(r == PV_RES_OK, "open NEW.DSK returned 0x%02X", r);
    CHECK(seek_to(0, &eof) == PV_RES_OK && eof == 0, "seek 0");
    CHECK(read_file(check, img_size) == img_size && memcmp(image, check, img_size) == 0, "image content");

    // random sector reads
    bool ok = true;
    for (uint32_t s = 0; s < 1440; s += 37)
    {
        uint8_t sector[512];
        seek_to(s * 512, &eof);
        if (read_file(sector, 512) != 512 || memcmp(sector, image + s * 512, 512) != 0) ok = false;
    }
    CHECK(ok, "random sector reads");

    // sector writes, flushed by the idle sync and by a close
    uint8_t sector[512];
    pattern_fill(sector, 512, 99);
    seek_to(700 * 512, &eof);
    CHECK(write_file(sector, 512) == 512, "write one sector");
    CHECK(fs_service_dirty(), "file is dirty after a write");
    fs_service_sync();
    CHECK(!fs_service_dirty(), "file is clean after the sync");
    memcpy(image + 700 * 512, sector, 512);
    seek_to(1439 * 512, &eof);
    CHECK(write_file(sector, 512) == 512, "write the last sector");
    memcpy(image + 1439 * 512, sector, 512);
    cmd(PV_CMD_FS_CLOSE);

    // Reload from scratch to be sure it was written to the media
    fs_service_invalidate();
    CHECK(cmd(PV_CMD_FS_MOUNT) == PV_RES_OK, "mount again");
    CHECK(open_name("NEW.DSK") == PV_RES_OK, "open NEW.DSK again");
    CHECK(read_file(check, img_size) == img_size && memcmp(image, check, img_size) == 0, "image content after remount");
    cmd(PV_CMD_FS_CLOSE);

    // --- read only files open for reading, and refuse writes
    fs_service_invalidate();
    f_mount(&tmp_fs, "", 1);
    f_chmod("README.TXT", AM_RDO, AM_RDO);
    f_mount(NULL, "", 0);
    CHECK(cmd(PV_CMD_FS_MOUNT) == PV_RES_OK, "mount");
    r = open_name("README.TXT");
    CHECK(r == PV_RES_OK, "open read only file returned 0x%02X", r);
    CHECK(cmd(PV_CMD_FS_ATTR) == PV_RES_OK && (pv_mbox.resp[0] & AM_RDO), "read only attribute");
    CHECK(write_file((const uint8_t *)"x", 1) == 0, "write to a read only file fails");
    cmd(PV_CMD_FS_CLOSE);

    // --- delete the image and the directory
    CHECK(delete_name("NEW.DSK") == PV_RES_OK, "delete NEW.DSK");
    CHECK(delete_name("GAMES") != PV_RES_OK, "a non empty directory can't be deleted");

    // --- disk full: write a file bigger than the free space
    r = create_name("FULL.DAT");
    CHECK(r == PV_RES_OK, "create FULL.DAT");
    uint8_t *chunk = malloc(PV_BUF_SIZE);
    memset(chunk, 0xA5, PV_BUF_SIZE);
    bool full = false;
    for (uint32_t k = 0; k < (sectors * 512 / PV_BUF_SIZE) + 4; k++)
    {
        memcpy(pv_mbox.buf, chunk, PV_BUF_SIZE);
        pv_mbox.arg[0] = PV_BUF_SIZE & 0xFF;
        pv_mbox.arg[1] = PV_BUF_SIZE >> 8;
        if (cmd(PV_CMD_FS_WRITE) != PV_RES_OK)
        {
            full = true;
            break;
        }
    }
    CHECK(full, "the disk full condition is reported");
    cmd(PV_CMD_FS_CLOSE);
    CHECK(delete_name("FULL.DAT") == PV_RES_OK, "delete FULL.DAT");
    free(chunk);

    // --- device removal
    dev_present = false;
    CHECK(cmd(PV_CMD_FS_OPEN) == PV_RES_NO_DEVICE, "commands fail without the device");
    dev_present = true;
    CHECK(cmd(PV_CMD_FS_OPEN) == PV_RES_NO_DEVICE, "the file system must be mounted again after a removal");
    CHECK(cmd(PV_CMD_FS_MOUNT) == PV_RES_OK, "mount after reinserting");

    free(image);
    free(check);
}

int main(void)
{
    run_scenario("FAT12 1.44MB", 2880, FM_FAT);
    run_scenario("FAT16 64MB", 131072, FM_FAT);
    run_scenario("FAT32 128MB", 262144, FM_FAT32);

    free(ram_disk);
    if (failures)
    {
        printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    printf("\nAll %d checks passed\n", checks);
    return 0;
}
