#include "sn76489.h"
#include <string.h>

static const int16_t levels[16] = {
    1536, 1220, 969, 770, 612, 486, 386, 306,
    243, 193, 154, 122, 97, 77, 61, 0
};

static uint16_t tone_period(const sn76489_t *chip, unsigned channel)
{
    uint16_t period = chip->period[channel];
    return period ? period : 1u;
}

void sn76489_reset(sn76489_t *chip)
{
    memset(chip, 0, sizeof(*chip));
    chip->noise = 0x8000u;
    memset(chip->volume, 15, sizeof(chip->volume));
    for (unsigned i = 0; i < 4; ++i)
        chip->counter[i] = 1;
}

void sn76489_write(sn76489_t *chip, uint8_t value)
{
    bool latch = (value & 0x80u) != 0;
    if (latch)
        chip->latch = (value >> 4) & 7u;
    unsigned reg = chip->latch;
    if (reg & 1u)
        chip->volume[reg >> 1] = value & 15u;
    else if (reg == 6u)
    {
        chip->noise_control = value & 7u;
        chip->noise = 0x8000u;
    }
    else
    {
        uint16_t *period = &chip->period[reg >> 1];
        *period = latch ? ((*period & 0x3F0u) | (value & 15u))
                        : ((*period & 15u) | ((value & 63u) << 4));
    }
}

static void noise_tick(sn76489_t *chip)
{
    uint16_t feedback = chip->noise & 1u;
    if (chip->noise_control & 4u)
        feedback ^= (chip->noise >> 3) & 1u;
    chip->noise = (chip->noise >> 1) | (feedback << 15);
}

static void tick(sn76489_t *chip)
{
    for (unsigned i = 0; i < 3; ++i)
    {
        if (--chip->counter[i] == 0u)
        {
            chip->counter[i] = tone_period(chip, i);
            chip->edge[i] ^= 1u;
            if (i == 2u && (chip->noise_control & 3u) == 3u && chip->edge[i])
                noise_tick(chip);
        }
    }
    if ((chip->noise_control & 3u) != 3u && --chip->counter[3] == 0u)
    {
        chip->counter[3] = (uint16_t)(16u << (chip->noise_control & 3u));
        chip->edge[3] ^= 1u;
        if (chip->edge[3])
            noise_tick(chip);
    }
}

static int32_t output(const sn76489_t *chip)
{
    int32_t value = 0;
    for (unsigned i = 0; i < 3; ++i)
    {
        bool high = chip->edge[i] || chip->period[i] <= 1u;
        value += high ? levels[chip->volume[i]] : -levels[chip->volume[i]];
    }
    value += (chip->noise & 1u) ? levels[chip->volume[3]] : -levels[chip->volume[3]];
    return value;
}

int16_t sn76489_sample(sn76489_t *chip)
{
    // Integrate across divided-clock edges, including fractional ticks. The
    // 1536 peak keeps the worst-case four-channel integral inside int32_t.
    const uint32_t step = (uint32_t)(((uint64_t)SN76489_CLOCK << 16) / (16u * SN76489_RATE));
    uint32_t remaining = step;
    int32_t integral = 0;
    while (remaining)
    {
        uint32_t span = 65536u - chip->fraction;
        if (span > remaining)
            span = remaining;
        integral += output(chip) * (int32_t)span;
        remaining -= span;
        chip->fraction += span;
        if (chip->fraction == 65536u)
        {
            chip->fraction = 0;
            tick(chip);
        }
    }
    int32_t input = (integral / (int32_t)step) * 4;
    int32_t filtered = input - chip->dc_input + (chip->dc_output * 511) / 512;
    chip->dc_input = input;
    chip->dc_output = filtered;
    if (filtered > 32767) return 32767;
    if (filtered < -32768) return -32768;
    return (int16_t)filtered;
}

void sn76489_franky_write(sn76489_bus_t *bus, uint8_t port)
{
    if (port == 0x89u)
        bus->franky_second = !bus->franky_second;
}

bool sn76489_franky_read(sn76489_bus_t *bus, uint8_t port, uint64_t us, uint8_t *value)
{
    if (port == 0x48u || port == 0x49u)
    {
        // NTSC 192-line counter timing only; no Sega VDP, VRAM or interrupts.
        uint64_t clocks = (us * SN76489_CLOCK) / 1000000u;
        unsigned line = (unsigned)((clocks / 228u) % 262u);
        *value = port == 0x48u ? (uint8_t)(line <= 218u ? line : line - 6u)
                               : (uint8_t)(((clocks % 228u) * 3u) / 4u);
        return true;
    }
    if (port == 0x89u)
    {
        bus->franky_second = false;
        *value = 0;
        return true;
    }
    return false;
}
