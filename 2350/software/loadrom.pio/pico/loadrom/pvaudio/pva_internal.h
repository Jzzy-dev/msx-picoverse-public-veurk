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

// ESP-01 link (UNAPI ESP8266 firmware behind the cartridge UART). The engine
// owns it between open and close; the MSX side's WiFi registers are quiet then.
bool     pva_hal_net_open(void);                         // false: no ESP link
void     pva_hal_net_close(void);
size_t   pva_hal_net_write(const uint8_t *p, size_t n);  // non-blocking, bytes taken
size_t   pva_hal_net_read(uint8_t *p, size_t n);         // non-blocking, bytes given
// RAM for the stream buffer outside the arena (NULL: none)
uint8_t *pva_hal_stream_ram(size_t *size);

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

// stream input for the decoder (pva_decode.c)
int  pva_dec_open_stream(pva_track_info_t *info);       // MP3 from pva_net_read()
bool pva_dec_is_stream(void);
void pva_dec_frame_info(pva_track_info_t *info);        // rate/channels/bitrate of the last frame

// network streams (pva_net.c)
typedef struct
{
    uint8_t  state;                 // PVA_NET_xxx
    uint8_t  err;                   // PVA_NERR_xxx of the last failure
    uint16_t http;                  // last HTTP status
    uint32_t rx_bytes;              // audio bytes received
    uint32_t rate_bps;              // bytes per second, about the last 2 s
    uint16_t reconnects;
    uint16_t rcv_max;               // TCP_RCV request size in use
    uint32_t dns_ms, open_ms;       // time of the last DNS lookup and TCP/TLS open
    uint32_t gaps;                  // receive pauses longer than 500 ms
    uint32_t longest_gap_ms;
    uint32_t esp_errors;            // ESP answers with an error code
    uint32_t overflow;              // UART bytes lost (ring full)
    bool     tls;
    uint8_t  title_gen;             // +1 for every new stream title
} pva_net_stats_t;

bool   pva_net_init(void);
int    pva_net_start(const char *url);          // PVA_OK, PVA_E_ARGS, PVA_E_NOWIFI
void   pva_net_stop(void);
void   pva_net_poll(void);
bool   pva_net_active(void);
size_t pva_net_read(uint8_t *dst, size_t n);    // audio bytes from the buffer
size_t pva_net_buffered(void);
size_t pva_net_capacity(void);
bool   pva_net_ended(void);                     // failed for good and the buffer is empty
const pva_net_stats_t *pva_net_stats(void);
const char *pva_net_station(void);              // icy-name (or "")
const char *pva_net_stream_title(void);         // StreamTitle (or "")
const char *pva_net_host(void);

// helpers
int  pva_kind_from_name(const char *name);   // PVA_DE_xxx or 0
void pva_copy_ascii(char *dst, size_t size, const char *src, size_t src_len);

#endif
