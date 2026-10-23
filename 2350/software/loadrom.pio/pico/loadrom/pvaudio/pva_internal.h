// MSX PICOVERSE PROJECT
// pva_internal.h - PicoVerse Audio Engine internals (core 1 side).

#ifndef PVA_INTERNAL_H
#define PVA_INTERNAL_H

#include "pva.h"

#ifdef PVA_HOST
#define pva_barrier() __sync_synchronize()
#else
#include "hardware/sync.h"
#define pva_barrier() __dmb()
#endif

// register window + mailbox, shared by both cores
extern volatile uint8_t *pva_win;
typedef struct
{
    volatile uint32_t post_count;   // core 0: +1 per CMD_CODE write
    volatile uint8_t  token;
    volatile uint8_t  code;
} pva_mailbox_t;
extern pva_mailbox_t pva_mailbox;

// arena
void *pva_alloc(size_t size);       // zeroed, 4-byte aligned; NULL when full
size_t pva_arena_free(void);

// log (engine/LOG command)
void pva_log(const char *fmt, ...);
size_t pva_log_dump(char *out, size_t size);
uint16_t pva_log_count(void);

// hardware layer (pva_hal_pico.c, or the host/emulator one)
bool     pva_hal_audio_init(void);
int16_t *pva_hal_audio_acquire(uint32_t *max_frames);   // NULL: no free buffer
void     pva_hal_audio_commit(uint32_t frames, uint32_t sample_rate);
void     pva_hal_audio_mute(bool mute);
uint32_t pva_hal_millis(void);

// decoder (pva_decode.c)
typedef struct
{
    char     title[PVA_ST_TITLE_LEN];
    char     artist[PVA_ST_ARTIST_LEN];
    char     album[PVA_ST_ALBUM_LEN];
    bool     tagged;
    uint8_t  format;                // PVA_DE_MP3 / PVA_DE_WAV
    uint32_t total_seconds;
    uint32_t bitrate_kbps;
    uint32_t sample_rate;
    uint8_t  channels;
    bool     seekable;
} pva_track_info_t;

bool pva_dec_init(void);
int  pva_dec_open(const char *fatfs_path, pva_track_info_t *info);  // PVA_OK / PVA_E_xxx
void pva_dec_close(void);
bool pva_dec_active(void);
// Produce at most one block of audio. Returns 1 if audio was produced,
// 0 if nothing could be done now (no free buffer), -1 at the end of the track.
int  pva_dec_step(void);
int  pva_dec_seek(uint32_t seconds);
uint32_t pva_dec_elapsed(void);
void pva_dec_set_volume(uint8_t vol);     // 0..100
void pva_dec_vu(uint8_t *l, uint8_t *r);
uint8_t pva_dec_last_fresult(void);

// helpers
int  pva_kind_from_name(const char *name);   // PVA_DE_xxx or 0
void pva_copy_ascii(char *dst, size_t size, const char *src, size_t src_len);

#endif
