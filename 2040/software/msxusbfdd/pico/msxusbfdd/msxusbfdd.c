// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// msxusbfdd.c - Raspberry Pico firmware that turns the PicoVerse 2040 into a USB floppy disk interface
//
// The MSX sees a 32KB disk ROM (MSX-DOS 1 kernel + USB FDD BIOS) mapped with the Konami SCC mapper
// at 4000h-7FFFh, plus a mailbox (see mailbox.h) at 7D00h-7FCFh. The ROM image is stored in the flash
// right after this firmware, appended by the msxusbfdd tool.
//
// Core 0 serves the MSX bus: ROM reads, mapper writes and the mailbox registers.
// Core 1 runs the USB host stack and executes the mailbox commands (usb_host.c): UFI commands for USB
// floppy drives and file commands for disk image (DSK) files on USB flash drives (fs_service.c).
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "hardware/sync.h"

#include "msxusbfdd.h"
#include "mailbox.h"
#include "usb_host.h"
#include "msx_bus.pio.h"

// This symbol marks the end of the main program in flash.
// The ROM image starts right after it
extern unsigned char __flash_binary_end;
static const uint8_t *rom = (const uint8_t *)&__flash_binary_end;

// The ROM runs from SRAM, flash reads are too slow and are shared with the USB code of core 1
static uint8_t rom_sram[ROM_SIZE] __attribute__((aligned(4)));

// Konami SCC mapper: the 8KB banks selected for 4000h-5FFFh and 6000h-7FFFh
static uint8_t bank_reg[2] = { 0, 1 };

pv_mailbox_t pv_mbox;

// Not const (volatile keeps the compiler from moving it to flash): it has to be in SRAM,
// a flash read from the bus loop could stall on the XIP cache
static volatile uint8_t pv_signature[4] = { 'P', 'V', 'F', 'D' };

typedef struct {
    PIO pio;
    uint sm_read;
    uint sm_write;
    uint offset_read;
    uint offset_write;
} msx_pio_bus_t;

static msx_pio_bus_t msx_bus;

static inline void setup_gpio(void)
{
    for (uint pin = PIN_A0; pin <= PIN_A15; ++pin)
    {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
    }

    for (uint pin = PIN_D0; pin <= PIN_D7; ++pin)
    {
        gpio_init(pin);
    }

    gpio_init(PIN_RD);      gpio_set_dir(PIN_RD, GPIO_IN);
    gpio_init(PIN_WR);      gpio_set_dir(PIN_WR, GPIO_IN);
    gpio_init(PIN_IORQ);    gpio_set_dir(PIN_IORQ, GPIO_IN);
    gpio_init(PIN_SLTSL);   gpio_set_dir(PIN_SLTSL, GPIO_IN);
    gpio_init(PIN_BUSSDIR); gpio_set_dir(PIN_BUSSDIR, GPIO_IN);
}

// Copy the ROM image to SRAM, holding the MSX with /WAIT while it's done
static void load_rom(void)
{
    gpio_init(PIN_WAIT);
    gpio_set_dir(PIN_WAIT, GPIO_OUT);
    gpio_put(PIN_WAIT, 0);

    memcpy(rom_sram, rom, ROM_SIZE);

    gpio_set_dir(PIN_WAIT, GPIO_IN);
}

static void msx_pio_bus_init(void)
{
    msx_bus.pio = pio0;
    msx_bus.sm_read  = 0;
    msx_bus.sm_write = 1;

    msx_bus.offset_read  = pio_add_program(msx_bus.pio, &msx_read_responder_program);
    msx_bus.offset_write = pio_add_program(msx_bus.pio, &msx_write_captor_program);

    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_read, false);
    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_write, false);
    pio_sm_clear_fifos(msx_bus.pio, msx_bus.sm_read);
    pio_sm_clear_fifos(msx_bus.pio, msx_bus.sm_write);
    pio_sm_restart(msx_bus.pio, msx_bus.sm_read);
    pio_sm_restart(msx_bus.pio, msx_bus.sm_write);

    pio_sm_config cfg_read = msx_read_responder_program_get_default_config(msx_bus.offset_read);
    sm_config_set_in_pins(&cfg_read, PIN_A0);
    sm_config_set_in_shift(&cfg_read, false, false, 16);
    sm_config_set_out_pins(&cfg_read, PIN_D0, 8);
    sm_config_set_out_shift(&cfg_read, true, false, 32);
    sm_config_set_set_pins(&cfg_read, PIN_WAIT, 1);
    sm_config_set_jmp_pin(&cfg_read, PIN_RD);
    sm_config_set_clkdiv(&cfg_read, 1.0f);
    pio_sm_init(msx_bus.pio, msx_bus.sm_read, msx_bus.offset_read, &cfg_read);

    pio_sm_config cfg_write = msx_write_captor_program_get_default_config(msx_bus.offset_write);
    sm_config_set_in_pins(&cfg_write, PIN_A0);
    sm_config_set_in_shift(&cfg_write, false, false, 32);
    sm_config_set_fifo_join(&cfg_write, PIO_FIFO_JOIN_RX);
    sm_config_set_jmp_pin(&cfg_write, PIN_WR);
    sm_config_set_clkdiv(&cfg_write, 1.0f);
    pio_sm_init(msx_bus.pio, msx_bus.sm_write, msx_bus.offset_write, &cfg_write);

    pio_sm_set_pins_with_mask(msx_bus.pio, msx_bus.sm_read, 0u, (1u << PIN_WAIT));
    pio_gpio_init(msx_bus.pio, PIN_WAIT);
    pio_sm_set_consecutive_pindirs(msx_bus.pio, msx_bus.sm_read, PIN_WAIT, 1, false);

    for (uint pin = PIN_D0; pin <= PIN_D7; ++pin)
    {
        pio_gpio_init(msx_bus.pio, pin);
    }
    pio_sm_set_consecutive_pindirs(msx_bus.pio, msx_bus.sm_read, PIN_D0, 8, false);
    pio_sm_set_consecutive_pindirs(msx_bus.pio, msx_bus.sm_write, PIN_D0, 8, false);

    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_read, true);
    pio_sm_set_enabled(msx_bus.pio, msx_bus.sm_write, true);
}

static inline uint16_t __not_in_flash_func(pio_build_token)(bool drive, uint8_t data)
{
    uint8_t dir_mask = drive ? 0xFFu : 0x00u;
    return (uint16_t)data | ((uint16_t)dir_mask << 8);
}

// A command was written by the MSX: hand it over to core 1
static inline void __not_in_flash_func(mbox_command)(uint8_t cmd)
{
    if (cmd == PV_CMD_ABORT)
    {
        if (pv_mbox.status & PV_ST_BUSY)
        {
            pv_mbox.abort = true;
        }
        return;
    }

    if (cmd == PV_CMD_NOP || (pv_mbox.status & PV_ST_BUSY))
    {
        return;
    }

    pv_mbox.abort = false;
    pv_mbox.result = 0;
    pv_mbox.status = PV_ST_BUSY;
    __dmb();
    pv_mbox.cmd = cmd;
}

static inline void __not_in_flash_func(handle_write)(uint16_t addr, uint8_t data)
{
    if (addr < 0x4000u || addr > 0x7FFFu)
    {
        return;
    }

    if (addr >= PV_WIN_BASE && addr <= PV_REG_END)
    {
        if (addr < PV_REG_BASE)
        {
            pv_mbox.buf[((uint32_t)(pv_mbox.page & 0x1Fu) << 8) | (addr & 0xFFu)] = data;
            return;
        }

        uint32_t reg = addr - PV_REG_BASE;
        if (reg == PV_REG_PAGE)
        {
            pv_mbox.page = data & 0x1Fu;
        }
        else if (reg == PV_REG_CMD)
        {
            mbox_command(data);
        }
        else if (reg >= PV_REG_ARG && reg < PV_REG_ARG + PV_ARG_SIZE)
        {
            pv_mbox.arg[reg - PV_REG_ARG] = data;
        }
        return;
    }

    // Konami SCC mapper bank registers: 5000h-57FFh for 4000h-5FFFh and 7000h-77FFh for 6000h-7FFFh
    uint16_t sel = addr & 0xF800u;
    if (sel == 0x5000u)
    {
        bank_reg[0] = data & 0x03u;
    }
    else if (sel == 0x7000u)
    {
        bank_reg[1] = data & 0x03u;
    }
}

static inline uint8_t __not_in_flash_func(handle_read)(uint16_t addr)
{
    if (addr >= PV_WIN_BASE && addr <= PV_REG_END)
    {
        if (addr < PV_REG_BASE)
        {
            return pv_mbox.buf[((uint32_t)(pv_mbox.page & 0x1Fu) << 8) | (addr & 0xFFu)];
        }

        uint32_t reg = addr - PV_REG_BASE;
        if (reg == PV_REG_STATUS)
        {
            return pv_mbox.status;
        }
        if (reg == PV_REG_RESULT)
        {
            return pv_mbox.result;
        }
        if (reg == PV_REG_PAGE)
        {
            return pv_mbox.page;
        }
        if (reg >= PV_REG_ARG && reg < PV_REG_ARG + PV_ARG_SIZE)
        {
            return pv_mbox.arg[reg - PV_REG_ARG];
        }
        if (reg >= PV_REG_RESP && reg < PV_REG_RESP + PV_RESP_SIZE)
        {
            return pv_mbox.resp[reg - PV_REG_RESP];
        }
        if (reg >= PV_REG_SIG && reg < PV_REG_SIG + 4)
        {
            return pv_signature[reg - PV_REG_SIG];
        }
        if (reg == PV_REG_VERSION)
        {
            return PV_PROTOCOL_VERSION;
        }
        return 0xFFu;
    }

    uint32_t bank = bank_reg[addr >= 0x6000u ? 1 : 0];
    return rom_sram[(bank << 13) | (addr & 0x1FFFu)];
}

// Main loop of core 0: service the MSX bus
//
// The write FIFO of the PIO is only 8 entries deep, so it is drained continuously, even while
// waiting for the next read: bursts of writes (bank switches, mailbox arguments and data)
// without a read in between would stall the PIO and lose writes otherwise.
static void __no_inline_not_in_flash_func(msx_bus_loop)(void)
{
    PIO pio = msx_bus.pio;

    while (true)
    {
        uint16_t addr;
        uint16_t waddr;
        uint32_t sample;

        while (true)
        {
            while (!pio_sm_is_rx_fifo_empty(pio, msx_bus.sm_write))
            {
                sample = pio_sm_get(pio, msx_bus.sm_write);
                handle_write((uint16_t)(sample & 0xFFFFu), (uint8_t)((sample >> 16) & 0xFFu));
            }
            if (!pio_sm_is_rx_fifo_empty(pio, msx_bus.sm_read))
            {
                addr = (uint16_t)pio_sm_get(pio, msx_bus.sm_read);
                break;
            }
        }

        // Writes that arrived together with the read must be processed before it is answered
        while (!pio_sm_is_rx_fifo_empty(pio, msx_bus.sm_write))
        {
            sample = pio_sm_get(pio, msx_bus.sm_write);
            waddr = (uint16_t)(sample & 0xFFFFu);
            handle_write(waddr, (uint8_t)((sample >> 16) & 0xFFu));
        }

        bool in_window = (addr >= 0x4000u) && (addr <= 0x7FFFu);
        uint8_t data = 0xFFu;

        if (in_window)
        {
            data = handle_read(addr);
        }

        pio_sm_put_blocking(pio, msx_bus.sm_read, pio_build_token(in_window, data));
    }
}

int main(void)
{
    set_sys_clock_khz(230000, true);     // Set system clock to 230MHz
    setup_gpio();

    load_rom();

    memset(&pv_mbox, 0, sizeof(pv_mbox));

    // The USB host stack runs on core 1
    multicore_launch_core1(usb_host_task);

    msx_pio_bus_init();
    msx_bus_loop();

    return 0;
}
