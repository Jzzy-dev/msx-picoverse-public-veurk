#ifndef EXPLORER_SN76489_H
#define EXPLORER_SN76489_H

#include <stdbool.h>
#include <stdint.h>

#define SN76489_CLOCK 3579545u
#define SN76489_RATE 44100u

typedef struct {
    uint16_t period[3];
    uint16_t counter[4];
    uint16_t noise;
    uint8_t volume[4];
    uint8_t edge[4];
    uint8_t latch;
    uint8_t noise_control;
    uint32_t fraction;
    int32_t dc_input;
    int32_t dc_output;
} sn76489_t;

typedef struct {
    bool franky_second;
    volatile uint32_t dropped;
    uint32_t reported;
} sn76489_bus_t;

void sn76489_reset(sn76489_t *chip);
void sn76489_write(sn76489_t *chip, uint8_t value);
int16_t sn76489_sample(sn76489_t *chip);
void sn76489_franky_write(sn76489_bus_t *bus, uint8_t port);
bool sn76489_franky_read(sn76489_bus_t *bus, uint8_t port, uint64_t us, uint8_t *value);

#endif
