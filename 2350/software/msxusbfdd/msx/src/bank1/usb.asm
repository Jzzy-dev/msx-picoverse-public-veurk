; MSX PICOVERSE 2040 - USB FDD BIOS
; Based on the Rookie Drive USB FDD BIOS created by Konamiman in 2018
; and on the MSXUSB USB FDD BIOS (CH376 version) by The Retro Hacker
; (c) 2026 Cristiano Goncalves
; The Retro Hacker
;
; This file contains the "high level" (hardware independent) USB routines.
;
; On the CH376 version this file contained the whole USB stack running on the
; Z80: device enumeration, hub handling, endpoint management and the CBI
; transport. On the PicoVerse 2040 the RP2040 runs a real USB host stack
; (TinyUSB) that already does all of that, so the routines here only ask the
; RP2040 what is connected and ask it to run UFI commands:
;
; - USB_INIT_DEV queries the type of the connected device
; - USB_EXECUTE_CBI sends a 12 byte UFI command to the floppy drive, with an
;   optional data stage, and gets back the ASC and ASCQ of the command.
;   The RP2040 handles the CBI/CB/Bulk-Only transport and REQUEST SENSE.


; -----------------------------------------------------------------------------
; USB_CHECK_DEV_CHANGE
;
; Check if a device connection or disconnection has happened.
; On device connection, initialize it.
; On device disconnection, clear work area.
; -----------------------------------------------------------------------------
; Output: Cy = 0 if a properly initialized CBI or storage device is connected
;              1 if not

USB_CHECK_DEV_CHANGE:
    call HW_DEV_CHANGE
    jr c,_USB_CHECK_DEV_CHANGE_NO_DEV   ;Device present, but the check failed

    or a
    jr nz,_USB_CHECK_DEV_CHANGE_CHANGED

    ;* No device change detected, rely on work area

    call WK_HAS_CONTENTS
    ret nc
    scf
    ret

    ;* Device change detected, act accordingly

_USB_CHECK_DEV_CHANGE_CHANGED:
    inc a
    jr z,_USB_CHECK_DEV_CHANGE_NO_DEV   ;Disconnected

    call USB_INIT_DEV
    or a
    ret z   ;Initialization OK
    dec a
    jr nz,_USB_CHECK_DEV_CHANGE_NO_DEV

    ;* Device present but it's not a FDD: check if it's a storage device

    call WK_GET_STORAGE_DEV_FLAGS   ;No disk mounted for now
    and 0FEh
    call WK_SET_STORAGE_DEV_FLAGS
    
    call HWF_MOUNT_DISK
    jr c,_USB_CHECK_DEV_CHANGE_NO_DEV
    call DSK_INIT_WK_FOR_STORAGE_DEV
    ld a,1
    call DSK_DO_BOOT_PROC
    or a
    ret

    ;* No device, or device initialization failed

_USB_CHECK_DEV_CHANGE_NO_DEV:
    call WK_ZERO
    scf
    ret


; -----------------------------------------------------------------------------
; USB_INIT_DEV: Initialize work area for the connected USB device
;
; This routine is invoked after a device connections is detected.
; It asks the RP2040 for the type of the device, and if it is a FDD
; it initializes the work area; if not, it empties the work area.
; -----------------------------------------------------------------------------
; Output: A = Initialization result
;             0: Ok, device is a FDD
;             1: The device is not a FDD
;             2: Error when querying the device, or no device present
;         B = USB error code if A = 2

USB_INIT_DEV:
    call WK_ZERO

    ld a,PV_CMD_DEV_INFO
    call PV_EXEC
    jr nz,_USB_INIT_DEV_ERR

    ld a,(PV_RESP)
    cp PV_DEV_FDD
    jr z,_USB_INIT_DEV_FDD

    or a    ;PV_DEV_NONE
    jr nz,_USB_INIT_DEV_NOT_FDD
    ld a,USB_ERR_NO_DEVICE

_USB_INIT_DEV_ERR:
    ld b,a
    ld a,2
    ret

_USB_INIT_DEV_NOT_FDD:
    ld a,1
    ret

_USB_INIT_DEV_FDD:
    ld a,(PV_RESP+5)    ;1 if the device is connected through a hub
    and 1
    call nz,WK_SET_MISC_FLAGS

    ;The work area is considered to have contents when the first byte is
    ;not zero, and the bit 7 must stay clear for a FDD: store endpoint 2
    ;as the bulk OUT endpoint number (the real endpoints are managed by the RP2040)

    ld a,2
    ld b,0
    call WK_SET_EP_NUMBER

    xor a
    ret


; -----------------------------------------------------------------------------
; USB_PROCESS_ERROR
;
; If USB error is "device disconnected", clear work area.
; Otherwise, if it's not STALL, reset device.
; -----------------------------------------------------------------------------
; Does not modify registers

USB_PROCESS_ERROR:
    push bc
    push de
    push hl
    push af
    ld hl,_USB_PROCESS_ERROR_END
    push hl
    cp USB_ERR_NO_DEVICE
    jp z,WK_ZERO
    cp USB_ERR_STALL
    jr nz,_USB_PROCESS_ERROR_RESET
    pop hl
_USB_PROCESS_ERROR_END:
    pop af
    call WK_SET_ERROR
    pop hl
    pop de
    pop bc
    ret

_USB_PROCESS_ERROR_RESET:
    call HW_BUS_RESET
    call USB_INIT_DEV
    ret


; -----------------------------------------------------------------------------
; USB_EXECUTE_CBI_WITH_RETRY: Execute a command with error retry
;
; Retried errors are those of type "device powered" or "not ready to
; ready transition", plus optionally "media changed"
; -----------------------------------------------------------------------------
; Input:  Same as USB_EXECUTE_CBI, plus:
;         A = 1 to retry "media changed" errors, 0 to no retry them
; Output: Same as USB_EXECUTE_CBI

USB_EXECUTE_CBI_WITH_RETRY:
    call _USB_EXECUTE_CBI_WITH_RETRY
    or a
    ret nz  ;USB error will already have been logged by USB_PROCESS_ERROR
    push bc
    push de
    call WK_SET_ERROR
    pop de
    pop bc
    xor a
    ret

_USB_EXECUTE_CBI_WITH_RETRY:
    push hl
    push de
    push bc
    push af

    call USB_EXECUTE_CBI
    or a
    jr nz,_USB_ECBIR_POPALL_END_NZ   ;USB level error: do not retry

    ld a,d
    or a    ;No error at all
    jr z,_USB_ECBIR_POPALL_END_NZ

    ;Report success if the error is one of the "recovered data" (17h or 18h)

    cp 17h
    jr z,_USB_ECBIR_POPALL_END
    cp 18h
    jr z,_USB_ECBIR_POPALL_END

    ;Retry if ASC=4 and ASCQ=1 (unit becoming ready) or FFh (unit busy)

    cp 4
    jr nz,_USB_ECBIR_NO_ASC_4
    ld a,e
    cp 1
    jr z,_USB_ECBIR_DO_RETRY
    cp 0FFh
    jr z,_USB_ECBIR_DO_RETRY
    jr _USB_ECBIR_POPALL_END
_USB_ECBIR_NO_ASC_4:

    ;Retry "device powered" error

    ld a,d
    cp 29h
    jr z,_USB_ECBIR_DO_RETRY

    ;Retry "media changed" only if we were instructed to do so

    cp 28h
    jr nz,_USB_ECBIR_POPALL_END
    pop af
    push af
    or a
    jr z,_USB_ECBIR_POPALL_END

    ;Here we know we must retry

_USB_ECBIR_DO_RETRY:
    pop af
    pop bc
    pop de
    pop hl    
    jr _USB_EXECUTE_CBI_WITH_RETRY

    ;Success, or do not retry

_USB_ECBIR_POPALL_END:
    xor a
_USB_ECBIR_POPALL_END_NZ:
    pop hl
    pop hl
    pop hl
    pop hl
    ret


; -----------------------------------------------------------------------------
; USB_EXECUTE_CBI: Execute a UFI command on the floppy drive
; -----------------------------------------------------------------------------
; Input:  HL = Address of the 12 byte command to execute
;         DE = Address of the input or output data buffer
;         BC = Length of data to send or receive (maximum PV_BUF_SIZE)
;         Cy = 0 to receive data, 1 to send data
; Output: A  = USB error code
;         BC = Amount of data actually transferred (if IN transfer)
;         D  = ASC (if no error)
;         E  = ASCQ (if no error)
;
; The RP2040 sends the command using the transport of the drive (CBI, CB or
; Bulk-Only). If the command fails (STALL or non-zero ASC in the status),
; it also gets the sense data with REQUEST SENSE and returns its ASC and
; ASCQ with A = 0, like the CH376 version did.

USB_EXECUTE_CBI:
    push af     ;Cy = send flag

    ld a,b
    cp PV_BUF_SIZE/256+1
    jr nc,_USB_EXECUTE_CBI_TOO_BIG
    cp PV_BUF_SIZE/256
    jr nz,_USB_EXECUTE_CBI_SIZE_OK
    ld a,c
    or a
    jr z,_USB_EXECUTE_CBI_SIZE_OK

_USB_EXECUTE_CBI_TOO_BIG:
    pop af
    ld a,USB_ERR_DATA_ERROR
    ld bc,0
    ld de,0
    ret

_USB_EXECUTE_CBI_SIZE_OK:
    push de     ;Data buffer
    push bc     ;Data length

    ld de,PV_ARG
    ld bc,12
    ldir        ;UFI command

    pop bc
    pop de
    pop af
    ld a,0
    rla         ;A = 1 to send data, 0 to receive data
    ld (PV_ARG+14),a
    push af
    ld a,c
    ld (PV_ARG+12),a
    ld a,b
    ld (PV_ARG+13),a
    pop af

    push de     ;Data buffer
    push bc     ;Data length
    or a
    jr z,_USB_EXECUTE_CBI_GO

    ex de,hl    ;HL = data to send
    call PV_WRITE_BUF

_USB_EXECUTE_CBI_GO:
    ld a,PV_CMD_UFI_EXEC
    call PV_EXEC
    pop bc
    pop hl      ;HL = Data buffer
    jr nz,_USB_EXECUTE_CBI_ERR

    ld a,(PV_RESP)
    ld c,a
    ld a,(PV_RESP+1)
    ld b,a      ;BC = Data actually transferred

    ld a,(PV_ARG+14)
    or a
    jr nz,_USB_EXECUTE_CBI_DONE

    push bc
    call PV_READ_BUF    ;Copy the received data
    pop bc

_USB_EXECUTE_CBI_DONE:
    ld a,(PV_RESP+2)
    ld d,a
    ld a,(PV_RESP+3)
    ld e,a
    xor a
    ret

_USB_EXECUTE_CBI_ERR:
    call USB_PROCESS_ERROR
    ld bc,0
    ld de,0
    ret
