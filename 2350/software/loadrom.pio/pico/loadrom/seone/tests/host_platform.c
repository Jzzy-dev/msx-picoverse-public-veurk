// Host test platform for the SE-ONE emulation: FatFS disk I/O over an image
// file, a WAV-writing audio HAL and a millisecond clock.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ff.h"
#include "diskio.h"
#include "../seone_internal.h"

// ---------------- disk image ----------------
static FILE    *img;
static uint32_t img_sectors;
uint32_t host_disk_reads;

int host_disk_open(const char *path, uint32_t create_sectors)
{
    img = fopen(path, create_sectors ? "w+b" : "r+b");
    if (!img) return -1;
    if (create_sectors)
    {
        img_sectors = create_sectors;
        fseek(img, (long)create_sectors * 512 - 1, SEEK_SET);
        fputc(0, img);
        fflush(img);
    }
    else
    {
        fseek(img, 0, SEEK_END);
        img_sectors = (uint32_t)(ftell(img) / 512);
    }
    return 0;
}

DSTATUS disk_initialize(BYTE pdrv) { return (pdrv == 0 && img) ? 0 : STA_NOINIT; }
DSTATUS disk_status(BYTE pdrv)     { return (pdrv == 0 && img) ? 0 : STA_NOINIT; }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv || !img || sector + count > img_sectors) return RES_PARERR;
    host_disk_reads += count;
    fseek(img, (long)sector * 512, SEEK_SET);
    return fread(buff, 512, count, img) == count ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv || !img || sector + count > img_sectors) return RES_PARERR;
    fseek(img, (long)sector * 512, SEEK_SET);
    return fwrite(buff, 512, count, img) == count ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv || !img) return RES_PARERR;
    switch (cmd)
    {
        case CTRL_SYNC:        fflush(img); return RES_OK;
        case GET_SECTOR_COUNT: *(LBA_t *)buff = img_sectors; return RES_OK;
        case GET_SECTOR_SIZE:  *(WORD *)buff = 512; return RES_OK;
        case GET_BLOCK_SIZE:   *(DWORD *)buff = 1; return RES_OK;
        default:               return RES_OK;
    }
}

DWORD get_fattime(void) { return ((DWORD)(2026 - 1980) << 25) | (10u << 21) | (21u << 16); }

// ---------------- audio HAL ----------------
static int16_t  hal_buf[1152 * 2];
static FILE    *wav;
static uint32_t wav_frames, wav_rate;
uint32_t host_audio_frames;
uint32_t host_audio_last_rate;
bool     host_audio_queue_full;     // simulate a full I2S queue

void host_wav_open(const char *path)
{
    wav = fopen(path, "wb");
    static const uint8_t hdr[44] = { 0 };
    fwrite(hdr, 1, 44, wav);
    wav_frames = 0;
}

void host_wav_close(void)
{
    if (!wav) return;
    uint32_t data = wav_frames * 4, riff = data + 36, fmt_sz = 16, rate = wav_rate ? wav_rate : 44100;
    uint16_t pcm = 1, ch = 2, bits = 16, align = 4;
    uint32_t brate = rate * 4;
    fseek(wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, wav); fwrite(&riff, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav);
    fwrite(&fmt_sz, 4, 1, wav); fwrite(&pcm, 2, 1, wav); fwrite(&ch, 2, 1, wav);
    fwrite(&rate, 4, 1, wav); fwrite(&brate, 4, 1, wav); fwrite(&align, 2, 1, wav);
    fwrite(&bits, 2, 1, wav); fwrite("data", 1, 4, wav); fwrite(&data, 4, 1, wav);
    fclose(wav);
    wav = NULL;
}

bool seone_hal_audio_init(void) { return true; }

int16_t *seone_hal_audio_acquire(uint32_t *max_frames)
{
    if (host_audio_queue_full) return NULL;
    *max_frames = 1152;
    return hal_buf;
}

void seone_hal_audio_commit(uint32_t frames, uint32_t sample_rate)
{
    host_audio_frames += frames;
    host_audio_last_rate = sample_rate;
    if (wav)
    {
        fwrite(hal_buf, 4, frames, wav);
        wav_frames += frames;
        wav_rate = sample_rate;
    }
}

static uint32_t fake_ms;
uint32_t seone_hal_millis(void) { return fake_ms; }
void host_advance_ms(uint32_t ms) { fake_ms += ms; }
