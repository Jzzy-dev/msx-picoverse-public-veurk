; MSX PICOVERSE 2040 - USB FDD BIOS
; (c) 2026 Cristiano Goncalves
; The Retro Hacker
;
; PicoVerse mailbox register map.
;
; The RP2040 (see pico/msxusbfdd/mailbox.h, which must be kept in sync with
; this file) overlays a small memory mapped mailbox on the cartridge slot,
; inside the 4000h-7FFFh window and above all the code of both ROM banks:
;
;   7D00h-7DFFh  Data window, 256 bytes of the Pico data buffer selected
;                by PV_PAGE (read and write)
;   7E00h-7FCFh  Control registers
;
; The MSX writes the arguments, writes the command to PV_CMD and polls
; PV_STATUS until the BUSY bit is clear. The USB host stack runs on the
; second core of the RP2040, the MSX never talks to USB directly.

PV_WIN:         equ 7D00h   ;Data window (256 bytes)
PV_WIN_SIZE:    equ 256

PV_STATUS:      equ 7E00h   ;R:   Bit 7 = BUSY
PV_RESULT:      equ 7E01h   ;R:   Result of the last command (USB_ERR_* or file result)
PV_PAGE:        equ 7E02h   ;R/W: Data window page (256 byte pages of the data buffer)
PV_CMD:         equ 7E03h   ;W:   Command, writing starts execution
PV_ARG:         equ 7E04h   ;R/W: 16 argument bytes
PV_RESP:        equ 7E14h   ;R:   16 response bytes
PV_SIG:         equ 7E30h   ;R:   "PVFD" signature
PV_VERSION:     equ 7E34h   ;R:   Mailbox protocol version

PV_PROTOCOL_VERSION: equ 1

;Optional routine implemented by pico.asm: HW_CONFIGURE_NAK_RETRY
HW_IMPL_CONFIGURE_NAK_RETRY: equ 1

PV_ST_BUSY:     equ 80h
PV_BUF_SIZE:    equ 8192    ;Size of the Pico data buffer (32 pages)

;--- Commands

PV_CMD_NOP:             equ 00h
PV_CMD_RESET:           equ 01h ;Wait for the device, out: RESP0 = 1 present, FFh none
PV_CMD_DEV_CHANGE:      equ 02h ;Out: RESP0 = 1 connected, 0 no change, FFh disconnected
PV_CMD_DEV_INFO:        equ 03h ;Out: RESP0 = PV_DEV_*, RESP1-4 = VID, PID, RESP5 = 1 if behind a hub
                                ;     floppy drive diagnostics: RESP6 = protocol, RESP7 = flags,
                                ;     RESP8-13 = stage, endpoint, result, USB error, length and SIE status of the first
                                ;     failed transfer, RESP14 = slowest transfer in 100 ms units,
                                ;     RESP15 = number of commands that failed
PV_CMD_BUS_RESET:       equ 04h ;Recover the device after a transfer error
PV_CMD_LONG_TIMEOUT:    equ 05h ;ARG0 = 1 for long command timeouts (format), 0 for normal ones
PV_CMD_UFI_EXEC:        equ 10h ;ARG0-11 = UFI command, ARG12-13 = data length, ARG14 bit 0 = data OUT
                                ;Out: RESP0-1 = data transferred, RESP2 = ASC, RESP3 = ASCQ
PV_CMD_FS_MOUNT:        equ 20h ;Out: data buffer = 28 byte device name
PV_CMD_FS_OPEN:         equ 21h ;Data buffer = zero terminated name
PV_CMD_FS_CREATE:       equ 22h ;Data buffer = zero terminated name
PV_CMD_FS_MKDIR:        equ 23h ;Data buffer = zero terminated name
PV_CMD_FS_CLOSE:        equ 24h
PV_CMD_FS_DELETE:       equ 25h ;Data buffer = zero terminated name
PV_CMD_FS_ENUM_FIRST:   equ 26h ;Data buffer = zero terminated pattern, out: data buffer = 32 byte dir entry
PV_CMD_FS_ENUM_NEXT:    equ 27h ;Out: data buffer = 32 byte dir entry
PV_CMD_FS_SEEK:         equ 28h ;ARG0-3 = new pointer, out: RESP0 = 1 if beyond the end of the file
PV_CMD_FS_READ:         equ 29h ;ARG0-1 = length, out: RESP0-1 = bytes read, data buffer = data
PV_CMD_FS_WRITE:        equ 2Ah ;ARG0-1 = length, data buffer = data, out: RESP0-1 = bytes written
PV_CMD_FS_ATTR:         equ 2Bh ;Out: RESP0 = attributes of the open file
PV_CMD_ABORT:           equ 0FFh ;Accepted while BUSY, aborts the running command (panic button)

;--- Device types returned by PV_CMD_DEV_INFO in RESP0

PV_DEV_NONE:            equ 0
PV_DEV_FDD:             equ 1   ;USB floppy drive (UFI command set)
PV_DEV_STORAGE:         equ 2   ;USB mass storage device with a FAT file system
PV_DEV_OTHER:           equ 3   ;Anything else

;--- File command results, same values the CH376 used

PV_RES_ENTRY:           equ 1Dh ;Directory entry available (FS_ENUM_*)
