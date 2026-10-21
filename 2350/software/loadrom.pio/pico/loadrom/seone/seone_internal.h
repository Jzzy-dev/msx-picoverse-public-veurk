// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// seone_internal.h - shared state between the core 0 bus side and the core 1
// command/audio side of the SE-ONE emulation.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef SEONE_INTERNAL_H
#define SEONE_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "seone.h"

#ifdef SEONE_HOST_TEST
#define SEONE_RAM_FUNC(name) name
#define seone_barrier() __sync_synchronize()
#else
#include "pico.h"
#include "hardware/sync.h"
#define SEONE_RAM_FUNC(name) __not_in_flash_func(name)
#define seone_barrier() __dmb()
#endif

// -----------------------------------------------------------------------
// Sizes
// -----------------------------------------------------------------------
#define SEONE_CMD_MAX        320u    // one AT command line (SE Play strips spaces)
#define SEONE_RESP_MAX       1536u   // one AT response (AT+LIST is the longest)
#define SEONE_REG_MAX        320u    // REG-mode payload / response
#define SEONE_LIST_SIZE      8192u   // port 0x25 directory listing stream
#define SEONE_RING_SIZE      32768u  // port 0x22 MP3 stream FIFO (power of two)
#define SEONE_INBUF_SIZE     8192u   // compressed audio input buffer
#define SEONE_PATH_MAX       256u

// Longest long file name shown in listings. Longer names fall back to the
// 8.3 short name to keep SE Play's (Quigs) string space usage modest.
#define SEONE_LIST_NAME_MAX  20u

// -----------------------------------------------------------------------
// Modes (AT+SEMODE)
// -----------------------------------------------------------------------
enum {
    SEONE_MODE_MP3A = 0,
    SEONE_MODE_MP3B,
    SEONE_MODE_FM,
    SEONE_MODE_REG,
    SEONE_MODE_TEST,
    SEONE_MODE_MIDI,
    SEONE_MODE_SYNTH,
    SEONE_MODE_VOICE,
};

// Port 0x23 status byte (manual "*1 status byte")
#define SEONE_ST_END_OF_MUSIC 0x08u   // 1 = nothing (music) playing
#define SEONE_ST_USB_ERROR    0x10u   // storage not available
#define SEONE_ST_USB_BUSY     0x20u   // talk / sample playback busy
#define SEONE_ST_FIFO_FULL    0x40u   // stream FIFO above high level
#define SEONE_ST_FIFO_EMPTY   0x80u   // stream FIFO below low level

// REG-mode state as read from port 0x26
#define SEONE_REG_IDLE  0u
#define SEONE_REG_BUSY  1u
#define SEONE_REG_ERROR 2u

typedef struct {
    // ---- core 0 private -------------------------------------------------
    uint8_t  line_buf;            // which cmd[] buffer core 0 is filling
    uint16_t line_len;
    bool     line_active;
    bool     line_overflow;
    bool     resp_flushed;        // a new command started: hide old reply
    uint16_t resp_pos;
    uint32_t list_seen_gen;
    uint32_t list_pos;
    uint8_t  reg_buf;             // which reg_in[] buffer core 0 is filling
    uint16_t reg_in_len;
    uint32_t reg_out_seen_gen;
    uint16_t reg_out_pos;
    uint8_t  test_reg[8];
    uint8_t  sw_io_id;            // last value written to switched-I/O port 0x40
    bool     tnc_armed;

    // ---- core 0 -> core 1 ----------------------------------------------
    char              cmd[2][SEONE_CMD_MAX];
    volatile uint16_t cmd_len;     // length of the posted line (0xFFFF = overflow)
    volatile uint8_t  cmd_index;   // which cmd[] holds the posted line
    volatile uint32_t cmd_seq;     // incremented for every posted line

    uint8_t           reg_in[2][SEONE_REG_MAX];
    volatile uint16_t regcmd_len;
    volatile uint8_t  regcmd_index;
    volatile uint8_t  regcmd_code;
    volatile uint32_t regcmd_seq;

    volatile uint32_t ring_head;   // stream FIFO producer index

    // ---- core 1 -> core 0 ----------------------------------------------
    char              resp[SEONE_RESP_MAX];
    volatile uint16_t resp_len;
    volatile uint32_t resp_seq;    // == cmd_seq once resp[] answers that line

    uint8_t          *list;        // SEONE_LIST_SIZE bytes (arena)
    volatile uint32_t list_len;
    volatile uint32_t list_gen;

    uint8_t           reg_out[SEONE_REG_MAX];
    volatile uint16_t reg_out_len;
    volatile uint32_t reg_out_gen;
    volatile uint32_t reg_done_seq;
    volatile uint8_t  reg_result;  // SEONE_REG_IDLE / SEONE_REG_ERROR

    volatile uint8_t  status;      // port 0x23
    volatile uint8_t  vu_left;     // port 0x26 (MP3B): 0..max leds
    volatile uint8_t  vu_right;    // port 0x27 (MP3B)
    volatile uint8_t  mode;        // SEONE_MODE_*
    volatile uint8_t  rds_ready;   // port 0x21 in FM mode

    uint8_t          *ring;        // SEONE_RING_SIZE bytes (arena)
    volatile uint32_t ring_tail;   // stream FIFO consumer index
} seone_shared_t;

extern seone_shared_t seone;

// -----------------------------------------------------------------------
// Arena allocator (core 1, also used by the Helix decoder via MPDEC_ALLOCATOR)
// -----------------------------------------------------------------------
void *seone_arena_alloc(size_t size);

// -----------------------------------------------------------------------
// Command side (seone_cmd.c, core 1)
// -----------------------------------------------------------------------
void seone_cmd_init(void);
void seone_cmd_poll(void);

// -----------------------------------------------------------------------
// Audio engine (seone_audio.c, core 1)
// -----------------------------------------------------------------------
enum {
    SEONE_PLAY_MUSIC = 0,   // AT+MSDPLAY: reported via status bit 3
    SEONE_PLAY_FX,          // AT+MSDTALK / AT+MSDSAMPLE: status bit 5
};

bool     seone_audio_init(void);
void     seone_audio_service(void);
bool     seone_audio_play_file(const char *path, int kind);
void     seone_audio_stop(void);
void     seone_audio_pause(void);
void     seone_audio_resume(void);
bool     seone_audio_music_active(void);   // playing or paused (file or stream)
bool     seone_audio_fx_active(void);
bool     seone_audio_paused(void);
uint32_t seone_audio_file_size(void);
uint8_t  seone_audio_percent(void);
uint32_t seone_audio_elapsed_seconds(void);
void     seone_audio_set_volume(uint8_t left, uint8_t right);
void     seone_audio_set_tone(uint8_t bass, uint8_t treble);
void     seone_audio_set_mute(bool mute);
void     seone_audio_set_stereo_mode(uint8_t mode); // 0 mono,1 linear,2 pseudo,3 spatial
void     seone_audio_set_vu_max(uint8_t leds);
void     seone_audio_set_stream_enabled(bool enabled);
void     seone_audio_flush_stream(void);
uint8_t  seone_audio_peak_left(void);   // 0..255
uint8_t  seone_audio_peak_right(void);

// -----------------------------------------------------------------------
// Hardware abstraction (seone_hal_pico.c on target, host stub in tests)
// -----------------------------------------------------------------------
bool     seone_hal_audio_init(void);
// Non-blocking: returns an interleaved stereo S16 buffer or NULL when the
// output queue is full. *max_frames receives its capacity in frames.
int16_t *seone_hal_audio_acquire(uint32_t *max_frames);
void     seone_hal_audio_commit(uint32_t frames, uint32_t sample_rate);
uint32_t seone_hal_millis(void);

#endif // SEONE_INTERNAL_H
