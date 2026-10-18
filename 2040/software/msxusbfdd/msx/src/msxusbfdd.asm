; MSX PICOVERSE 2040 - USB FDD BIOS
; Based on the Rookie Drive USB FDD BIOS created by Konamiman in 2018
; and on the MSXUSB USB FDD BIOS (CH376 version) by The Retro Hacker
; (c) 2026 Cristiano Goncalves
; The Retro Hacker
;
; This is the main file, assembled by the Makefile with:
; sjasm msxusbfdd.asm msxusbfdd.rom
;
; See config.asm for customization options.
;
; The USB host is the RP2040 of the PicoVerse 2040: all the code that talked
; to the CH376 was replaced by bank1/pico.asm and bank1/usb.asm, that talk to
; the firmware through the mailbox described in pico_regs.asm.

CALL_IX:   equ 7FD0h
CALL_BANK: equ CALL_IX+2

    include "constants.asm"
    include "config.asm"
    include "usb_errors.asm"
    include "pico_regs.asm"


    ;--- ROM bank 0:
    ;    - MSX-DOS 1 kernel
    ;    - Entry points for MSX-DOS driver functions located in bank 1
    ;    - CALL commands
    ;    - Choice string for FORMAT
    ;    - Default DPB for use by the kernel
    ;
    ;    The area between the end of the code and CALL_IX is free, and the
    ;    RP2040 uses 7D00h-7FCFh for the mailbox (see pico_regs.asm)

    org 4000h

    include "bank0/kernel.asm"
    include "bank0/driver_entry_points.asm"
    include "bank0/choice_strings.asm"
    include "bank0/oemstat.asm"
DEFDPB:
    include "defdpb.asm"    

    ;The bank code can't reach the area used by the mailbox
    if $ > PV_WIN
    .ERROR Bank 0 overlaps the PicoVerse mailbox
    endif

    ds CALL_IX-$,0FFh
    include "callbnk.asm"

    ds 7FFFh-$,0FFh
    db ROM_BANK_0


    ;--- ROM bank 1: 
    ;    - Initialization routine, executed at boot and by CALL USBRESET
    ;    - MSX-DOS driver function implementations
    ;    - All the USB related code
    ;    - Default DPB for use by the driver

    org 4000h

    include "bank1/header.asm"
    include "bank1/pico.asm" ;USB host hardware dependant code
    include "bank1/inihrd_inienv.asm"
    include "bank1/verbose_reset.asm"
    include "bank1/dskio_dskchg.asm"
    include "bank1/choice_dskfmt.asm"    
    include "bank1/work_area.asm"
    include "bank1/usb.asm"
    include "bank1/misc.asm"
    include "bank1/boot_menu.asm"
    include "bank1/dsk.asm"
DEFDPB_1:
    include "defdpb.asm"

    if $ > PV_WIN
    .ERROR Bank 1 overlaps the PicoVerse mailbox
    endif

    ds CALL_IX-$,0FFh
    include "callbnk.asm"

    ds 7FFFh-$,0FFh
    db ROM_BANK_1
