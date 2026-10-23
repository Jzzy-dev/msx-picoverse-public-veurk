// MSX PICOVERSE PROJECT
// pva_hal_pico.c - PicoVerse Audio Engine: I2S output and clock (RP2350, core 1).
//
// pico_audio_i2s (pico-extras) on PIO0 SM2 (SM0/SM1 run the MSX memory bus,
// PIO1 the I/O bus) with the PicoVerse 2350 DAC pins, as the Explorer MP3
// player uses them. Must run on core 1: the DMA completion IRQ is registered
// on the calling core and must never interrupt the core 0 bus loop.
//
// Never blocks: acquire returns NULL when every buffer is queued. When no
// buffer is queued pico_audio_i2s plays silence by itself.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include "pico/stdlib.h"
#include "pico/audio_i2s.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "pva_internal.h"

#define PVA_I2S_DATA_PIN  29
#define PVA_I2S_BCLK_PIN  30   // LRCLK = BCLK + 1 = 31
#define PVA_I2S_MUTE_PIN  32   // high = muted
#define PVA_I2S_BUFFERS   6    // ~157 ms of 44.1 kHz audio queued
#define PVA_I2S_FRAMES    1152

static audio_format_t audio_format = {
    .sample_freq = 44100,
    .format = AUDIO_BUFFER_FORMAT_PCM_S16,
    .channel_count = 2,
};

static struct audio_buffer_format producer_format = {
    .format = &audio_format,
    .sample_stride = 4,
};

static struct audio_buffer_pool *pool;
static struct audio_buffer *current;

bool pva_hal_audio_init(void)
{
    gpio_init(PVA_I2S_MUTE_PIN);
    gpio_set_dir(PVA_I2S_MUTE_PIN, GPIO_OUT);
    gpio_put(PVA_I2S_MUTE_PIN, 1);

    pool = audio_new_producer_pool(&producer_format, PVA_I2S_BUFFERS, PVA_I2S_FRAMES);
    if (!pool)
        return false;

    int dma = dma_claim_unused_channel(false);
    if (dma < 0)
        return false;
    dma_channel_unclaim((uint)dma);   // audio_i2s_setup claims it again

    struct audio_i2s_config cfg = {
        .data_pin = PVA_I2S_DATA_PIN,
        .clock_pin_base = PVA_I2S_BCLK_PIN,
        .dma_channel = (uint8_t)dma,
        .pio_sm = 2,
    };
    if (!audio_i2s_setup(&audio_format, &cfg))
        return false;
    if (!audio_i2s_connect(pool))
        return false;
    audio_i2s_set_enabled(true);
    return true;
}

void pva_hal_audio_mute(bool mute)
{
    gpio_put(PVA_I2S_MUTE_PIN, mute ? 1 : 0);
}

int16_t *pva_hal_audio_acquire(uint32_t *max_frames)
{
    if (!pool)
        return NULL;
    if (!current)
        current = take_audio_buffer(pool, false);
    if (!current)
        return NULL;
    *max_frames = current->max_sample_count;
    return (int16_t *)current->buffer->bytes;
}

void pva_hal_audio_commit(uint32_t frames, uint32_t sample_rate)
{
    if (!current)
        return;
    // pico_audio_i2s retunes the PIO clock when the producer rate changes
    if (sample_rate && audio_format.sample_freq != sample_rate)
        audio_format.sample_freq = sample_rate;
    current->sample_count = frames;
    give_audio_buffer(pool, current);
    current = NULL;
}

uint32_t pva_hal_millis(void)
{
    return to_ms_since_boot(get_absolute_time());
}
