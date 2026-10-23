// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// seone_audio.c - SE-ONE emulation, core 1 audio engine.
//
// Replaces the SE-ONE's VS1053 decoder and TDA8425 audio processor:
//   * MP3 (Helix fixed-point decoder, MPEG-1/2/2.5 layer III) and PCM WAV
//     files from the microSD card,
//   * an MP3 byte stream pushed by the MSX through port 0x22 (webradio),
//   * left/right volume, bass, treble, mute and forced mono,
//   * VU levels for ports 0x26/0x27.
//
// The engine never blocks: seone_audio_service() decodes at most one frame
// and only when the I2S queue has a free buffer, so core 1 keeps servicing
// Nextor's Sunrise IDE sector requests between frames.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include <math.h>
#include "ff.h"
#include "mp3dec.h"
#include "seone_internal.h"
#include "seone_mp3_alloc.h"
#ifndef SEONE_HOST_TEST
#include "../audio/mp3_alloc_dispatch.h"
#endif

#define MAX_FRAME_SAMPLES   1152u
#define REFILL_THRESHOLD    (SEONE_INBUF_SIZE / 2u)
#define STREAM_START_BYTES  4096u
#define STREAM_IDLE_MS      3000u
#define VU_DECAY_MS         40u
#define MP3_POOL_SIZE       (30u * 1024u)

enum { SRC_NONE = 0, SRC_MP3, SRC_WAV, SRC_STREAM };

// ---------------- decoder memory pool (Helix MPDEC_ALLOCATOR) ----------------
static uint8_t *mp3_pool;
static size_t   mp3_pool_used;

void *seone_mp3_alloc(size_t size)
{
    size = (size + 7u) & ~(size_t)7u;
    if (!mp3_pool || mp3_pool_used + size > MP3_POOL_SIZE)
        return NULL;
    void *p = mp3_pool + mp3_pool_used;
    mp3_pool_used += size;
    memset(p, 0, size);
    return p;
}

// ---------------- source state ----------------
static HMP3Decoder decoder;
static uint8_t  *inbuf;
static uint32_t  in_len, in_pos;
static FIL       file;
static bool      file_open;
static bool      file_eof;
static uint32_t  file_size;
static uint8_t   src;
static int       src_kind;
static bool      paused;
static uint32_t  rate = 44100;
static uint64_t  samples_out;
static uint8_t   bad_frames;

static uint16_t  wav_channels, wav_bits;
static uint32_t  wav_remaining;

static bool      stream_enabled = true;
static uint32_t  stream_last_data_ms;

static int16_t   pcm[MAX_FRAME_SAMPLES * 2];
static int16_t  *held;
static uint32_t  held_cap;

// ---------------- DSP state ----------------
typedef struct { float b0, b1, b2, a1, a2; float z1[2], z2[2]; } biquad_t;
static float    gain_l = 1.0f, gain_r = 1.0f;
static uint8_t  vol_l = 85, vol_r = 85, bass = 50, treble = 50, stereo_mode = 1;
static bool     muted;
static bool     tone_dirty = true, tone_active;
static uint32_t tone_rate;
static biquad_t low_shelf, high_shelf;

// ---------------- VU ----------------
static uint8_t  vu_max = 8;
static uint8_t  vu_l, vu_r;
static uint8_t  peak_l, peak_r;
static uint32_t vu_last_ms;
static int32_t  vu_threshold[33];

// =====================================================================
// DSP
// =====================================================================
static float db_to_gain(float db) { return powf(10.0f, db / 20.0f); }

static float volume_gain(uint8_t v)
{
    if (v == 0) return 0.0f;
    return db_to_gain(((float)v - 100.0f) * 0.6f);      // 100 = 0 dB, 1 = -59.4 dB
}

static void update_gains(void)
{
    gain_l = muted ? 0.0f : volume_gain(vol_l);
    gain_r = muted ? 0.0f : volume_gain(vol_r);
}

static void shelf(biquad_t *q, bool high, float f0, float db, float fs)
{
    float A = powf(10.0f, db / 40.0f);
    float w0 = 2.0f * (float)M_PI * f0 / fs;
    float c = cosf(w0), s = sinf(w0);
    float alpha = s / 2.0f * sqrtf(2.0f);
    float sa = 2.0f * sqrtf(A) * alpha;
    float b0, b1, b2, a0, a1, a2;
    if (!high)
    {
        b0 = A * ((A + 1) - (A - 1) * c + sa);
        b1 = 2 * A * ((A - 1) - (A + 1) * c);
        b2 = A * ((A + 1) - (A - 1) * c - sa);
        a0 = (A + 1) + (A - 1) * c + sa;
        a1 = -2 * ((A - 1) + (A + 1) * c);
        a2 = (A + 1) + (A - 1) * c - sa;
    }
    else
    {
        b0 = A * ((A + 1) + (A - 1) * c + sa);
        b1 = -2 * A * ((A - 1) + (A + 1) * c);
        b2 = A * ((A + 1) + (A - 1) * c - sa);
        a0 = (A + 1) - (A - 1) * c + sa;
        a1 = 2 * ((A - 1) - (A + 1) * c);
        a2 = (A + 1) - (A - 1) * c - sa;
    }
    q->b0 = b0 / a0; q->b1 = b1 / a0; q->b2 = b2 / a0;
    q->a1 = a1 / a0; q->a2 = a2 / a0;
}

static void update_tone(uint32_t fs)
{
    tone_active = (bass != 50) || (treble != 50);
    if (tone_active)
    {
        // TDA8425 style: about +/-12 dB, 50 = flat
        shelf(&low_shelf, false, 250.0f, ((float)bass - 50.0f) * 0.24f, (float)fs);
        shelf(&high_shelf, true, 4000.0f, ((float)treble - 50.0f) * 0.24f, (float)fs);
    }
    tone_rate = fs;
    tone_dirty = false;
}

static inline float bq_run(biquad_t *q, int ch, float x)
{
    // transposed direct form II
    float y = q->b0 * x + q->z1[ch];
    q->z1[ch] = q->b1 * x - q->a1 * y + q->z2[ch];
    q->z2[ch] = q->b2 * x - q->a2 * y;
    return y;
}

static inline int16_t clamp16(float v)
{
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)v;
}

static void vu_compute_thresholds(void)
{
    // N leds spread over a 48 dB range; led k lights above -48 + 48*k/N dB.
    for (uint32_t k = 1; k <= vu_max; k++)
    {
        float db = -48.0f + 48.0f * (float)k / (float)vu_max - 1.0f;
        vu_threshold[k] = (int32_t)(32767.0f * db_to_gain(db));
    }
}

static uint8_t vu_level(int32_t peak)
{
    uint8_t n = 0;
    while (n < vu_max && peak >= vu_threshold[n + 1]) n++;
    return n;
}

static void vu_update(int32_t pk_l, int32_t pk_r)
{
    uint32_t now = seone_hal_millis();
    uint32_t steps = (now - vu_last_ms) / VU_DECAY_MS;
    if (steps)
        vu_last_ms = now;
    uint8_t nl = vu_level(pk_l), nr = vu_level(pk_r);
    uint8_t dl = (steps >= vu_l) ? 0 : (uint8_t)(vu_l - steps);
    uint8_t dr = (steps >= vu_r) ? 0 : (uint8_t)(vu_r - steps);
    vu_l = nl > dl ? nl : dl;
    vu_r = nr > dr ? nr : dr;
    peak_l = (uint8_t)(pk_l >> 7);
    peak_r = (uint8_t)(pk_r >> 7);
    seone.vu_left = vu_l;
    seone.vu_right = vu_r;
}

// pcm (stereo interleaved) -> out with volume/tone/mono, returns peaks
static void dsp_process(const int16_t *in, int16_t *out, uint32_t frames)
{
    if (tone_dirty || tone_rate != rate)
        update_tone(rate);

    int32_t pk_l = 0, pk_r = 0;
    for (uint32_t i = 0; i < frames; i++)
    {
        float l = in[2 * i], r = in[2 * i + 1];
        if (stereo_mode == 0)
            l = r = 0.5f * (l + r);
        if (tone_active)
        {
            l = bq_run(&high_shelf, 0, bq_run(&low_shelf, 0, l));
            r = bq_run(&high_shelf, 1, bq_run(&low_shelf, 1, r));
        }
        int16_t ol = clamp16(l * gain_l), or_ = clamp16(r * gain_r);
        out[2 * i] = ol;
        out[2 * i + 1] = or_;
        int32_t al = ol < 0 ? -ol : ol, ar = or_ < 0 ? -or_ : or_;
        if (al > pk_l) pk_l = al;
        if (ar > pk_r) pk_r = ar;
    }
    vu_update(pk_l, pk_r);
}

// =====================================================================
// Input handling
// =====================================================================
static void close_source(void)
{
    if (file_open)
    {
        f_close(&file);
        file_open = false;
    }
    src = SRC_NONE;
    paused = false;
    in_len = in_pos = 0;
}

static void compact_input(void)
{
    if (in_pos == 0) return;
    uint32_t remaining = in_len - in_pos;
    if (remaining)
        memmove(inbuf, inbuf + in_pos, remaining);
    in_len = remaining;
    in_pos = 0;
}

static void refill_from_file(void)
{
    if (!file_open || file_eof || (in_len - in_pos) >= REFILL_THRESHOLD)
        return;
    compact_input();
    UINT br = 0;
    if (f_read(&file, inbuf + in_len, SEONE_INBUF_SIZE - in_len, &br) != FR_OK || br == 0)
        file_eof = true;
    in_len += br;
}

static uint32_t ring_fill(void) { return seone.ring_head - seone.ring_tail; }

static void refill_from_stream(void)
{
    if ((in_len - in_pos) >= REFILL_THRESHOLD)
        return;
    compact_input();
    uint32_t avail = ring_fill();
    uint32_t space = SEONE_INBUF_SIZE - in_len;
    uint32_t n = avail < space ? avail : space;
    uint32_t tail = seone.ring_tail;
    seone_barrier();
    for (uint32_t i = 0; i < n; i++)
        inbuf[in_len + i] = seone.ring[(tail + i) & (SEONE_RING_SIZE - 1u)];
    in_len += n;
    seone_barrier();
    seone.ring_tail = tail + n;
    if (n)
        stream_last_data_ms = seone_hal_millis();
}

static void skip_id3v2(void)
{
    uint8_t h[10];
    UINT br = 0;
    if (f_read(&file, h, sizeof h, &br) != FR_OK || br != sizeof h ||
        h[0] != 'I' || h[1] != 'D' || h[2] != '3')
    {
        f_lseek(&file, 0);
        return;
    }
    uint32_t size = ((uint32_t)(h[6] & 0x7F) << 21) | ((uint32_t)(h[7] & 0x7F) << 14) |
                    ((uint32_t)(h[8] & 0x7F) << 7) | (uint32_t)(h[9] & 0x7F);
    uint32_t skip = 10u + size + ((h[5] & 0x10) ? 10u : 0u);
    if (skip >= file_size || f_lseek(&file, skip) != FR_OK)
        f_lseek(&file, 0);
}

static bool reset_decoder(void)
{
    mp3_pool_used = 0;
    decoder = MP3InitDecoder();
    bad_frames = 0;
    return decoder != NULL;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static bool open_wav(void)
{
    uint8_t h[12];
    UINT br;
    if (f_read(&file, h, 12, &br) != FR_OK || br != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        return false;
    bool have_fmt = false;
    for (int guard = 0; guard < 32; guard++)
    {
        uint8_t ch[8];
        if (f_read(&file, ch, 8, &br) != FR_OK || br != 8)
            return false;
        uint32_t size = rd32(ch + 4);
        if (!memcmp(ch, "fmt ", 4))
        {
            uint8_t fmt[16];
            if (size < 16 || f_read(&file, fmt, 16, &br) != FR_OK || br != 16)
                return false;
            uint16_t tag = rd16(fmt);
            wav_channels = rd16(fmt + 2);
            rate = rd32(fmt + 4);
            wav_bits = rd16(fmt + 14);
            if ((tag != 1 && tag != 0xFFFE) || wav_channels < 1 || wav_channels > 2 ||
                (wav_bits != 8 && wav_bits != 16) || rate < 8000 || rate > 48000)
                return false;
            have_fmt = true;
            if (f_lseek(&file, f_tell(&file) + (size - 16u) + (size & 1u)) != FR_OK)
                return false;
        }
        else if (!memcmp(ch, "data", 4))
        {
            if (!have_fmt)
                return false;
            wav_remaining = size;
            return true;
        }
        else if (f_lseek(&file, f_tell(&file) + size + (size & 1u)) != FR_OK)
            return false;
    }
    return false;
}

// =====================================================================
// Frame producers. Return frames produced, 0 = try later, -1 = finished.
// =====================================================================
static int produce_mp3(bool streaming)
{
    for (int attempt = 0; attempt < 8; attempt++)
    {
        if (streaming) refill_from_stream();
        else refill_from_file();

        uint32_t avail = in_len - in_pos;
        bool no_more = streaming ? false : file_eof;
        if (avail < 4u)
            return (no_more || (!streaming && !file_open)) ? -1 : 0;

        int off = MP3FindSyncWord(inbuf + in_pos, (int)avail);
        if (off < 0)
        {
            in_pos = in_len - 3u;               // keep a possible split sync word
            if (no_more) return -1;
            continue;
        }
        in_pos += (uint32_t)off;
        avail -= (uint32_t)off;
        if (avail < 1800u && !no_more)
        {
            // make sure a whole frame (max ~1441 bytes) is buffered
            if (streaming)
            {
                refill_from_stream();
                if (in_len - in_pos < 1800u) return 0;
            }
            else
                refill_from_file();
            avail = in_len - in_pos;
        }

        unsigned char *p = inbuf + in_pos;
        int left = (int)avail;
        int err = MP3Decode(decoder, &p, &left, pcm, 0);
        in_pos = (uint32_t)(p - inbuf);

        if (err == ERR_MP3_NONE)
        {
            MP3FrameInfo fi;
            MP3GetLastFrameInfo(decoder, &fi);
            if (fi.nChans < 1 || fi.outputSamps <= 0)
                continue;
            uint32_t frames = (uint32_t)fi.outputSamps / (uint32_t)fi.nChans;
            if (frames > MAX_FRAME_SAMPLES) frames = MAX_FRAME_SAMPLES;
            if (fi.nChans == 1)
                for (int i = (int)frames - 1; i >= 0; i--)
                    pcm[2 * i] = pcm[2 * i + 1] = pcm[i];
            if (fi.samprate > 0) rate = (uint32_t)fi.samprate;
            bad_frames = 0;
            return (int)frames;
        }
        if (err == ERR_MP3_INDATA_UNDERFLOW)
        {
            if (no_more) return -1;
            if (streaming) return 0;
            continue;
        }
        if (err == ERR_MP3_MAINDATA_UNDERFLOW)
            continue;                            // bit reservoir not filled yet
        // corrupt frame: step past this sync word
        if (in_pos < in_len) in_pos++;
        if (++bad_frames > 64u)
            return -1;
    }
    return 0;
}

static int produce_wav(uint32_t max_frames)
{
    if (wav_remaining == 0)
        return -1;
    uint32_t bpf = (uint32_t)wav_channels * (wav_bits / 8u);
    uint32_t frames = max_frames > MAX_FRAME_SAMPLES ? MAX_FRAME_SAMPLES : max_frames;
    uint32_t bytes = frames * bpf;
    if (bytes > wav_remaining) bytes = wav_remaining - (wav_remaining % bpf);
    if (bytes > SEONE_INBUF_SIZE) bytes = SEONE_INBUF_SIZE - (SEONE_INBUF_SIZE % bpf);
    UINT br = 0;
    if (bytes == 0 || f_read(&file, inbuf, bytes, &br) != FR_OK || br < bpf)
        return -1;
    wav_remaining -= br;
    frames = br / bpf;
    for (uint32_t i = 0; i < frames; i++)
    {
        int16_t l, r;
        const uint8_t *s = inbuf + i * bpf;
        if (wav_bits == 16)
        {
            l = (int16_t)rd16(s);
            r = (wav_channels == 2) ? (int16_t)rd16(s + 2) : l;
        }
        else
        {
            l = (int16_t)(((int)s[0] - 128) << 8);
            r = (wav_channels == 2) ? (int16_t)(((int)s[1] - 128) << 8) : l;
        }
        pcm[2 * i] = l;
        pcm[2 * i + 1] = r;
    }
    return (int)frames;
}

// =====================================================================
// Public API
// =====================================================================
bool seone_audio_init(void)
{
#ifndef SEONE_HOST_TEST
    pv_mp3_alloc_fn = seone_mp3_alloc;
#endif
    inbuf = seone_arena_alloc(SEONE_INBUF_SIZE);
    mp3_pool = seone_arena_alloc(MP3_POOL_SIZE);
    update_gains();
    vu_compute_thresholds();
    vu_last_ms = seone_hal_millis();
    return inbuf && mp3_pool && reset_decoder() && seone_hal_audio_init();
}

bool seone_audio_play_file(const char *path, int kind)
{
    close_source();
    if (f_open(&file, path, FA_READ) != FR_OK)
        return false;
    file_open = true;
    file_eof = false;
    file_size = (uint32_t)f_size(&file);
    samples_out = 0;
    src_kind = kind;

    const char *dot = strrchr(path, '.');
    bool wav = dot && (dot[1] == 'w' || dot[1] == 'W') && (dot[2] == 'a' || dot[2] == 'A') &&
               (dot[3] == 'v' || dot[3] == 'V');
    if (wav)
    {
        if (!open_wav())
        {
            close_source();
            return false;
        }
        src = SRC_WAV;
    }
    else
    {
        skip_id3v2();
        if (!reset_decoder())
        {
            close_source();
            return false;
        }
        src = SRC_MP3;
    }
    return true;
}

void seone_audio_stop(void)        { close_source(); }
void seone_audio_pause(void)       { if (src != SRC_NONE) paused = true; }
void seone_audio_resume(void)      { paused = false; }
bool seone_audio_paused(void)      { return paused; }

bool seone_audio_music_active(void)
{
    return (src == SRC_STREAM) || (src != SRC_NONE && src_kind == SEONE_PLAY_MUSIC);
}

bool seone_audio_fx_active(void)
{
    return src != SRC_NONE && src != SRC_STREAM && src_kind == SEONE_PLAY_FX;
}

uint32_t seone_audio_file_size(void) { return file_open ? file_size : 0; }

uint8_t seone_audio_percent(void)
{
    if (!file_open || file_size == 0)
        return 0;
    uint32_t pos = (uint32_t)f_tell(&file);
    uint32_t buffered = in_len - in_pos;
    pos = pos > buffered ? pos - buffered : 0;
    uint32_t pct = (uint32_t)(((uint64_t)pos * 100u) / file_size);
    return (uint8_t)(pct > 100u ? 100u : pct);
}

uint32_t seone_audio_elapsed_seconds(void) { return rate ? (uint32_t)(samples_out / rate) : 0; }

void seone_audio_set_volume(uint8_t l, uint8_t r) { vol_l = l; vol_r = r; update_gains(); }
void seone_audio_set_tone(uint8_t b, uint8_t t)
{
    if (b != bass || t != treble)
    {
        bass = b; treble = t;
        tone_dirty = true;
        memset(low_shelf.z1, 0, sizeof low_shelf.z1);  memset(low_shelf.z2, 0, sizeof low_shelf.z2);
        memset(high_shelf.z1, 0, sizeof high_shelf.z1); memset(high_shelf.z2, 0, sizeof high_shelf.z2);
    }
}
void seone_audio_set_mute(bool m)           { muted = m; update_gains(); }
void seone_audio_set_stereo_mode(uint8_t m) { stereo_mode = m; }
void seone_audio_set_vu_max(uint8_t leds)
{
    vu_max = (leds < 1) ? 1 : (leds > 32) ? 32 : leds;
    vu_compute_thresholds();
}
void seone_audio_set_stream_enabled(bool e)
{
    stream_enabled = e;
    if (!e)
    {
        seone_audio_flush_stream();
        if (src == SRC_STREAM)
            close_source();
    }
}
void seone_audio_flush_stream(void)
{
    seone.ring_tail = seone.ring_head;
    if (src == SRC_STREAM)
        close_source();
}
uint8_t seone_audio_peak_left(void)  { return peak_l; }
uint8_t seone_audio_peak_right(void) { return peak_r; }

void seone_audio_service(void)
{
    // Webradio: bytes pushed to port 0x22 start a stream when nothing else plays.
    if (src == SRC_NONE)
    {
        if (stream_enabled && ring_fill() >= STREAM_START_BYTES && reset_decoder())
        {
            src = SRC_STREAM;
            src_kind = SEONE_PLAY_MUSIC;
            in_len = in_pos = 0;
            samples_out = 0;
            stream_last_data_ms = seone_hal_millis();
        }
        else
        {
            vu_update(0, 0);
            return;
        }
    }
    else if (src != SRC_STREAM && ring_fill() != 0 && src_kind == SEONE_PLAY_MUSIC)
    {
        // a file is playing: drop stray stream data
        seone.ring_tail = seone.ring_head;
    }

    if (paused)
    {
        vu_update(0, 0);
        return;
    }

    if (!held)
    {
        held = seone_hal_audio_acquire(&held_cap);
        if (!held)
            return;
    }

    int frames;
    if (src == SRC_WAV)
        frames = produce_wav(held_cap);
    else
        frames = produce_mp3(src == SRC_STREAM);

    if (frames > 0)
    {
        if ((uint32_t)frames > held_cap) frames = (int)held_cap;
        dsp_process(pcm, held, (uint32_t)frames);
        seone_hal_audio_commit((uint32_t)frames, rate);
        held = NULL;
        samples_out += (uint32_t)frames;
    }
    else if (frames < 0)
        close_source();
    else if (src == SRC_STREAM && (seone_hal_millis() - stream_last_data_ms) > STREAM_IDLE_MS)
        close_source();
}
