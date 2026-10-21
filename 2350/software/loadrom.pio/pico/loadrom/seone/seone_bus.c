// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// seone_bus.c - SE-ONE emulation, core 0 (MSX I/O bus) side.
//
// Everything here runs inside the MSX bus service loop and must answer an
// I/O read within the Z80 I/O cycle (no /WAIT is asserted for I/O), so the
// code only touches shared RAM, never blocks and never calls into flash.
//
// I/O map (SE-ONE manual, "IO Settings modes")
//   0x20 R/W  AT command channel. Writes collect a line until CR; reads return
//             the reply one byte at a time and 0x00 when nothing is pending.
//   0x21 R    MP3A/MP3B: stream FIFO fill level in 256-byte units
//             (SE Play webradio only sends more data while this is < 0x75).
//             FM: 1 when RDS text is available (never, no tuner).
//   0x22 W    MP3 data stream (webradio). SYNTH: MIDI bytes (ignored).
//   0x23 R    status byte (bit3 end of music, bit4 USB error, bit5 USB busy,
//             bit6 FIFO full, bit7 FIFO empty)
//   0x25 R    directory listing stream after AT+MSDFILES / REG 0x7B
//   0x26 R    MP3B: left VU (0-8)   REG: command state (0 idle,1 busy,2 error)
//   0x26 W    REG: 0 = reset data pointer, other = execute function
//   0x27 R    MP3B: right VU (0-8)  REG: next response byte
//   0x27 W    REG: command data byte
//   TEST mode: 0x21-0x27 read back the last value written.
//
// TMTLogic network card stub (ports 0x41-0x49)
//   SE Play probes the TNC at start-up by issuing a command on port 0x41 and
//   reading an ID string from port 0x42 until it gets 0x00. Without a card the
//   ports float at 0xFF and SE Play loops until its string space overflows.
//   After software writes one of the TNC ports, reads of 0x41-0x49 return
//   0x00 ("idle" status, empty data) which SE Play treats as "no network".
//   Port 0x40 is the MSX switched-I/O device-ID register; the stub never drives
//   it, and stays silent while ID 0x08 (Panasonic/turbo R internal device) is
//   selected so it cannot collide with on-board switched-I/O hardware.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include "seone_internal.h"

seone_shared_t seone;

#define SW_IO_ID_PANASONIC 0x08u

static inline void SEONE_RAM_FUNC(post_line)(void)
{
    uint8_t idx = seone.line_buf;
    seone.cmd_len = seone.line_overflow ? 0xFFFFu : seone.line_len;
    seone.cmd_index = idx;
    seone_barrier();
    seone.cmd_seq = seone.cmd_seq + 1u;
    seone.line_buf = idx ^ 1u;
    seone.resp_flushed = false;
    seone.resp_pos = 0;
}

static inline void SEONE_RAM_FUNC(post_reg)(uint8_t code)
{
    uint8_t idx = seone.reg_buf;
    seone.regcmd_len = seone.reg_in_len;
    seone.regcmd_index = idx;
    seone.regcmd_code = code;
    seone_barrier();
    seone.regcmd_seq = seone.regcmd_seq + 1u;
    seone.reg_buf = idx ^ 1u;
    seone.reg_in_len = 0;
}

void SEONE_RAM_FUNC(seone_io_write)(uint8_t port, uint8_t data)
{
    switch (port)
    {
        case 0x20:
            if (data == '\r')
            {
                if (seone.line_len != 0u || seone.line_overflow)
                    post_line();
                seone.line_len = 0;
                seone.line_active = false;
                seone.line_overflow = false;
            }
            else if (data != '\n' && data != 0u)
            {
                if (!seone.line_active)
                {
                    // New command: the SE-ONE drops whatever remains of the
                    // previous reply (SE Play reads replies only up to the
                    // first CR and leaves "OK<CR><LF>" behind).
                    seone.line_active = true;
                    seone.resp_flushed = true;
                }
                if (seone.line_len < SEONE_CMD_MAX - 1u)
                    seone.cmd[seone.line_buf][seone.line_len++] = (char)data;
                else
                    seone.line_overflow = true;
            }
            break;

        case 0x22:
        {
            uint8_t mode = seone.mode;
            if (mode == SEONE_MODE_MP3A || mode == SEONE_MODE_MP3B || mode == SEONE_MODE_REG)
            {
                uint32_t head = seone.ring_head;
                if ((uint32_t)(head - seone.ring_tail) < SEONE_RING_SIZE)
                {
                    seone.ring[head & (SEONE_RING_SIZE - 1u)] = data;
                    seone_barrier();
                    seone.ring_head = head + 1u;
                }
            }
            seone.test_reg[2] = data;
            break;
        }

        case 0x26:
            seone.test_reg[6] = data;
            if (data == 0u)
                seone.reg_in_len = 0;          // "start wr pointer"
            else
                post_reg(data);
            break;

        case 0x27:
            seone.test_reg[7] = data;
            if (seone.reg_in_len < SEONE_REG_MAX)
                seone.reg_in[seone.reg_buf][seone.reg_in_len++] = data;
            break;

        case 0x21: case 0x23: case 0x24: case 0x25:
            seone.test_reg[port - 0x20u] = data;
            break;

        case 0x40:
            seone.sw_io_id = data;
            if (data == SW_IO_ID_PANASONIC)
                seone.tnc_armed = false;
            break;

        default:
            // 0x41-0x49: TNC command/data ports
            if (port >= 0x41u && port <= 0x49u && seone.sw_io_id != SW_IO_ID_PANASONIC)
                seone.tnc_armed = true;
            break;
    }
}

bool SEONE_RAM_FUNC(seone_io_read)(uint8_t port, uint8_t *data)
{
    uint8_t mode = seone.mode;
    uint8_t v = 0;

    if (port >= 0x40u)
    {
        if (port == 0x40u || !seone.tnc_armed || seone.sw_io_id == SW_IO_ID_PANASONIC)
            return false;
        *data = 0x00u;     // TNC: status idle / no data
        return true;
    }

    if (mode == SEONE_MODE_TEST && port != 0x20u)
    {
        *data = seone.test_reg[port - 0x20u];
        return true;
    }

    switch (port)
    {
        case 0x20:
            if (!seone.resp_flushed && seone.resp_seq == seone.cmd_seq &&
                seone.resp_pos < seone.resp_len)
                v = (uint8_t)seone.resp[seone.resp_pos++];
            break;

        case 0x21:
            if (mode == SEONE_MODE_FM)
                v = seone.rds_ready;
            else if (mode == SEONE_MODE_MP3A || mode == SEONE_MODE_MP3B || mode == SEONE_MODE_REG)
            {
                uint32_t fill = (uint32_t)(seone.ring_head - seone.ring_tail) >> 8;
                v = (uint8_t)(fill > 0xFFu ? 0xFFu : fill);
            }
            break;

        case 0x23:
            v = seone.status;
            break;

        case 0x25:
        {
            uint32_t gen = seone.list_gen;
            if (gen != seone.list_seen_gen)
            {
                seone.list_seen_gen = gen;
                seone.list_pos = 0;
            }
            if (seone.list_pos < seone.list_len)
                v = seone.list[seone.list_pos++];
            break;
        }

        case 0x26:
            if (mode == SEONE_MODE_REG)
                v = (seone.reg_done_seq == seone.regcmd_seq) ? seone.reg_result : SEONE_REG_BUSY;
            else
                v = seone.vu_left;
            break;

        case 0x27:
            if (mode == SEONE_MODE_REG)
            {
                uint32_t gen = seone.reg_out_gen;
                if (gen != seone.reg_out_seen_gen)
                {
                    seone.reg_out_seen_gen = gen;
                    seone.reg_out_pos = 0;
                }
                if (seone.reg_done_seq == seone.regcmd_seq && seone.reg_out_pos < seone.reg_out_len)
                    v = seone.reg_out[seone.reg_out_pos++];
            }
            else
                v = seone.vu_right;
            break;

        default:
            break;   // 0x22, 0x24: nothing to report
    }

    *data = v;
    return true;
}
