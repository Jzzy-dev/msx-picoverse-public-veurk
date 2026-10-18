// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// mailbox.h - Mailbox shared between the MSX bus loop (core 0) and the USB host (core 1)
//
// The MSX sees the mailbox as memory mapped registers inside the 4000h-7FFFh window of the
// cartridge slot (see msx/src/pico_regs.asm, which must be kept in sync with this file):
//
//   7D00h-7DFFh  Data window, 256 bytes of the data buffer selected by the page register
//   7E00h        STATUS  (R)   bit 7 = BUSY
//   7E01h        RESULT  (R)   result of the last command
//   7E02h        PAGE    (R/W) data window page (0-31)
//   7E03h        CMD     (W)   command, writing it starts the execution
//   7E04h-7E13h  ARG     (R/W) 16 argument bytes
//   7E14h-7E23h  RESP    (R)   16 response bytes
//   7E30h-7E33h  signature "PVFD"
//   7E34h        mailbox protocol version
//
// The MSX writes the arguments (and data), writes the command and polls STATUS until BUSY is clear.
// Core 0 only moves bytes; the commands are executed by core 1, where the USB host stack runs.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef MAILBOX_H
#define MAILBOX_H

#include <stdbool.h>
#include <stdint.h>

#define PV_WIN_BASE        0x7D00u   // Data window (256 bytes)
#define PV_REG_BASE        0x7E00u   // Control registers
#define PV_REG_END         0x7FCFu   // Last address of the mailbox (7FD0h is ROM code)

#define PV_REG_STATUS      0x00
#define PV_REG_RESULT      0x01
#define PV_REG_PAGE        0x02
#define PV_REG_CMD         0x03
#define PV_REG_ARG         0x04
#define PV_REG_RESP        0x14
#define PV_REG_SIG         0x30
#define PV_REG_VERSION     0x34

#define PV_ARG_SIZE        16
#define PV_RESP_SIZE       16
#define PV_PROTOCOL_VERSION 1

#define PV_ST_BUSY         0x80
#define PV_BUF_SIZE        8192

// Commands
#define PV_CMD_NOP            0x00
#define PV_CMD_RESET          0x01  // wait for the device, RESP0 = 1 present, 0xFF none
#define PV_CMD_DEV_CHANGE     0x02  // RESP0 = 1 connected, 0 no change, 0xFF disconnected
#define PV_CMD_DEV_INFO       0x03  // RESP0 = PV_DEV_*, RESP1-4 = VID, PID, RESP5 = 1 behind a hub,
                                    // floppy diagnostics: RESP6 protocol, RESP7 flags (bit 0 interrupt endpoint, bit 1 it works),
                                    // RESP8-13 stage, endpoint, result, USB error, length and SIE status of the first failed
                                    // transfer, RESP14 slowest transfer in 100 ms units, RESP15 number of commands that failed;
                                    // data buffer: 6 counters (RX timeouts, sequence errors, stalls, transfers, buffers,
                                    // connection events), 2 unused bytes, 16 LE words of controller registers (see hw_snap_t)
#define PV_CMD_BUS_RESET      0x04
#define PV_CMD_LONG_TIMEOUT   0x05  // ARG0 = 1 long timeouts (format), 0 normal
#define PV_CMD_UFI_EXEC       0x10  // ARG0-11 UFI command, ARG12-13 length, ARG14 bit 0 = data OUT
#define PV_CMD_FS_MOUNT       0x20
#define PV_CMD_FS_OPEN        0x21
#define PV_CMD_FS_CREATE      0x22
#define PV_CMD_FS_MKDIR       0x23
#define PV_CMD_FS_CLOSE       0x24
#define PV_CMD_FS_DELETE      0x25
#define PV_CMD_FS_ENUM_FIRST  0x26
#define PV_CMD_FS_ENUM_NEXT   0x27
#define PV_CMD_FS_SEEK        0x28
#define PV_CMD_FS_READ        0x29
#define PV_CMD_FS_WRITE       0x2A
#define PV_CMD_FS_ATTR        0x2B
#define PV_CMD_ABORT          0xFF  // accepted while BUSY, aborts the running command

// Device types (RESP0 of PV_CMD_DEV_INFO)
#define PV_DEV_NONE           0
#define PV_DEV_FDD            1     // USB floppy drive (UFI command set)
#define PV_DEV_STORAGE        2     // USB mass storage device (SCSI, Bulk-Only)
#define PV_DEV_OTHER          3     // anything else

// Result codes, the USB_ERR_* values of msx/src/usb_errors.asm
#define PV_RES_OK             0x00
#define PV_RES_NAK            0x01
#define PV_RES_STALL          0x02
#define PV_RES_TIMEOUT        0x03
#define PV_RES_DATA_ERROR     0x04
#define PV_RES_NO_DEVICE      0x05
#define PV_RES_PANIC          0x06
#define PV_RES_UNEXPECTED     0x07
#define PV_RES_ENTRY          0x1D  // directory entry available (CH376 INT_DISK_READ)
#define PV_RES_OPEN_DIR       0x41  // FS_OPEN opened a directory
#define PV_RES_MISS_FILE      0x42  // file not found / end of the directory

typedef struct {
    volatile uint8_t status;                // read by the MSX (bit 7 = BUSY)
    volatile uint8_t result;                // read by the MSX
    volatile uint8_t page;                  // data window page, written by the MSX
    volatile uint8_t cmd;                   // command waiting for core 1, 0 if none
    volatile bool    abort;                 // set by core 0 when the MSX asks to abort the command
    uint8_t          arg[PV_ARG_SIZE];      // written by the MSX
    uint8_t          resp[PV_RESP_SIZE];    // written by core 1
    uint8_t          buf[PV_BUF_SIZE] __attribute__((aligned(4)));
} pv_mailbox_t;

extern pv_mailbox_t pv_mbox;

#endif // MAILBOX_H
