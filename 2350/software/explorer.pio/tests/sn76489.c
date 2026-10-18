#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sn76489.h"
#include "emu2149.h"
#include "sn-constants.h"

#define __not_in_flash_func(name) name
#define __no_inline_not_in_flash_func(name) name
#define __scratch_x(group)
typedef unsigned spin_lock_t;
static spin_lock_t lock;
static spin_lock_t *dual_psg_lock;
static bool dual_psg_ready, dual_psg_core1_services_io = true;
static uint8_t ctrl_audio_selection, ctrl_psg_emulation = 1;
static uint16_t dual_psg_write_ring[DUAL_PSG_WRITE_RING_SIZE];
static uint32_t dual_psg_write_ring_head, dual_psg_write_ring_tail;
static int16_t mirrored_sample;
static uint64_t now_us;
static unsigned spin_lock_claim_unused(bool required) { assert(required); return 0; }
static spin_lock_t *spin_lock_init(unsigned number) { (void)number; return &lock; }
static uint32_t spin_lock_blocking(spin_lock_t *p) { assert(p == &lock); return 0; }
static void spin_unlock(spin_lock_t *p, uint32_t saved) { (void)saved; assert(p == &lock); }
static void msx_pio_io_bus_init(void) {}
static void __dmb(void) {}
static uint64_t time_us_64(void) { return now_us; }
static int16_t clamp_i16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
static int16_t soft_limit_i16(int32_t v) { return clamp_i16(v); }
static int16_t apply_audio_volume(int32_t v) { return clamp_i16(v); }
static int16_t main_psg_calc_sample(void) { return mirrored_sample; }
static bool main_psg_ready = true;
#include "sn-production.h"

static void reset_profile(uint8_t profile) {
    ctrl_audio_selection = profile;
    dual_psg_ready = false;
    dual_psg_core1_services_io = false;
    dual_psg_write_ring_head = dual_psg_write_ring_tail = 0;
    mirrored_sample = 0;
    dual_psg_init();
}

static void test_registers_and_frequency(void) {
    sn76489_t chip;
    {
        sn76489_reset(&chip);
        for (unsigned i = 0; i < 100; ++i) assert(sn76489_sample(&chip) == 0);
        sn76489_write(&chip, 0x8E);
        sn76489_write(&chip, 0x0F);
        assert(chip.period[0] == 254);
        sn76489_write(&chip, 0x90);
        sn76489_write(&chip, 7);
        assert(chip.volume[0] == 7 && chip.period[0] == 254);
        sn76489_write(&chip, 0xE7);
        sn76489_write(&chip, 4);
        assert(chip.noise_control == 4);
        assert(chip.noise == 0x8000);
        sn76489_reset(&chip);
        sn76489_write(&chip, 0x8E);
        sn76489_write(&chip, 0x0F);
        sn76489_write(&chip, 0x90);
        unsigned crossings = 0;
        int16_t previous = 0;
        for (unsigned i = 0; i < SN76489_RATE; ++i) {
            int16_t sample = sn76489_sample(&chip);
            if (i > 100 && previous <= 0 && sample > 0) ++crossings;
            previous = sample;
        }
        // 3.579545 MHz / (32 * 254) = 440.396 Hz.
        assert(crossings >= 438 && crossings <= 441);
        sn76489_write(&chip, 0x9F);
        for (unsigned i = 0; i < SN76489_RATE; ++i) (void)sn76489_sample(&chip);
        assert(abs(sn76489_sample(&chip)) < 512);
    }
    puts("PASS: Franky latch/data writes, mute, reset and measured 440 Hz tone");
}

static void test_noise(void) {
    for (unsigned white = 0; white < 2; ++white) {
        sn76489_t chip;
        sn76489_reset(&chip);
        sn76489_write(&chip, (uint8_t)(0xE0 | (white << 2)));
        uint16_t expected = 0x8000;
        unsigned ticks = 0;
        for (unsigned i = 0; i < 2000; ++i) {
            uint32_t fraction = chip.fraction;
            (void)sn76489_sample(&chip);
            const unsigned step = (unsigned)(((uint64_t)SN76489_CLOCK << 16) / (16 * SN76489_RATE));
            unsigned advanced = (fraction + step) >> 16;
            for (unsigned j = 0; j < advanced; ++j) {
                if ((ticks++ % 32) == 0) {
                    unsigned bit = expected & 1;
                    if (white) bit ^= (expected >> 3) & 1;
                    expected = (expected >> 1) | (bit << 15);
                }
            }
            assert(chip.noise == expected);
        }
        sn76489_write(&chip, 0xE0);
        assert(chip.noise == 0x8000);
    }
    puts("PASS: 16-bit white/periodic noise sequences, clock division and noise reset");
}

static void test_edge_cases(void) {
    {
        sn76489_t chip;
        sn76489_reset(&chip);
        sn76489_write(&chip, 0x90);
        unsigned crossings = 0;
        int16_t previous = 0;
        for (unsigned i = 0; i < SN76489_RATE; ++i) {
            int16_t sample = sn76489_sample(&chip);
            if (i > 100 && previous <= 0 && sample > 0) ++crossings;
            previous = sample;
        }
        assert(crossings == 0);
        sn76489_reset(&chip);
        sn76489_write(&chip, 0xC3);
        sn76489_write(&chip, 0);
        sn76489_write(&chip, 0xE7);
        uint16_t expected = 0x8000;
        unsigned count = 1, edge = 0, period = 3;
        for (unsigned i = 0; i < 1000; ++i) {
            if (i == 500) {
                period = 5;
                sn76489_write(&chip, 0xC5);
                sn76489_write(&chip, 0);
            }
            unsigned fraction = chip.fraction;
            (void)sn76489_sample(&chip);
            const unsigned step = (unsigned)(((uint64_t)SN76489_CLOCK << 16) / (16 * SN76489_RATE));
            unsigned advanced = (fraction + step) >> 16;
            for (unsigned j = 0; j < advanced; ++j) {
                if (--count == 0) {
                    count = period;
                    edge ^= 1;
                    if (edge) {
                        unsigned bit = (expected ^ (expected >> 3)) & 1;
                        expected = (expected >> 1) | (bit << 15);
                    }
                }
            }
            assert(chip.noise == expected && chip.counter[2] == count);
        }
    }
    uint64_t energy[2] = {0, 0};
    for (unsigned quiet = 0; quiet < 2; ++quiet) {
        sn76489_t chip;
        sn76489_reset(&chip);
        sn76489_write(&chip, 0x8E);
        sn76489_write(&chip, 0x0F);
        sn76489_write(&chip, quiet ? 0x94 : 0x90);
        for (unsigned i = 0; i < 4096; ++i) {
            int32_t sample = sn76489_sample(&chip);
            if (i >= 1024) energy[quiet] += (uint64_t)(sample * sample);
        }
    }
    assert(energy[1] * 100 > energy[0] * 15 && energy[1] * 100 < energy[0] * 17);
    puts("PASS: Sega zero-period behavior, dynamic tone-2 noise clock and measured 8 dB attenuation");
}

static void test_franky(void) {
    reset_profile(AUDIO_PROFILE_SN76489_FRANKY);
    assert(sn_audio_selected());
    assert(dual_psg_handle_io_write(0x48, 0x8E));
    assert(dual_psg_handle_io_write(0x49, 0x0F));
    assert(dual_psg_handle_io_write(0x48, 0x90));
    assert(!dual_psg_handle_io_write(0x3F, 0x9F));
    assert(!dual_psg_handle_io_write(0x3C, 0x80));
    dual_psg_drain_write_ring();
    assert(sn_chip.period[0] == 254 && sn_chip.volume[0] == 0);
    uint8_t value, previous;
    unsigned changes = 0;
    sn76489_franky_read(&sn_bus, 0x48, 0, &previous);
    for (now_us = 2; now_us < 1000 && changes < 4; now_us += 2) {
        assert(sn_handle_io_read(0x48, &value));
        if (value != previous) {
            assert((uint8_t)(value - previous) == 1);
            previous = value;
            ++changes;
        }
    }
    assert(changes == 4);
    assert(!sn_handle_io_read(0xA2, &value));
    assert(!sn_handle_io_read(0x3C, &value));
    assert(!sn_handle_io_read(0x3F, &value));
    assert(dual_psg_handle_io_write(0x89, 0));
    assert(sn_bus.franky_second);
    assert(sn_handle_io_read(0x89, &value) && value == 0 && !sn_bus.franky_second);
    for (unsigned i = 0; i < DUAL_PSG_WRITE_RING_SIZE; ++i)
        assert(dual_psg_handle_io_write(0x48, 0x90));
    assert(sn_bus.dropped == 1);
    dual_psg_drain_write_ring();
    assert(dual_psg_write_ring_head == dual_psg_write_ring_tail);
    for (unsigned i = 0; i < 600; ++i) {
        dual_psg_handle_io_write(0x49, 0x91);
        dual_psg_drain_write_ring();
    }
    assert(sn_chip.volume[0] == 1 && sn_bus.dropped == 1);
    mirrored_sample = 1000;
    int16_t stereo[2];
    dual_psg_write_stereo_sample(stereo, 0);
    assert(stereo[0] == stereo[1]);
    dual_psg_core1_services_io = true;
    assert(dual_psg_handle_io_write(0x49, 0x9F));
    assert(sn_chip.volume[0] == 15);
    ctrl_audio_selection = 14;
    assert(!sn_audio_selected());
    assert(!sn_handle_io_read(0x48, &value));
    puts("PASS: Franky aliases, VGMPlay counter probe, stereo mix, queue wrap and explicit overflow counter");
}

int main(void) {
    assert(sizeof(sn76489_t) + sizeof(sn76489_bus_t) <= sizeof(PSG));
    test_registers_and_frequency();
    test_noise();
    test_edge_cases();
    test_franky();
    return 0;
}
