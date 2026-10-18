; MSX PICOVERSE 2040 - USB FDD BIOS
; Based on the Rookie Drive USB FDD BIOS created by Konamiman in 2018
; and on the MSXUSB USB FDD BIOS (CH376 version) by The Retro Hacker
; (c) 2026 Cristiano Goncalves
; The Retro Hacker
;
; This file replaces ch376.asm: it contains all the code that depends on the
; USB host hardware, which on the PicoVerse 2040 is the RP2040 itself.
;
; The routines keep the contracts of the CH376 version, so the rest of the
; BIOS (usb.asm, dsk.asm, dskio_dskchg.asm, boot_menu.asm...) is unchanged.
; The difference is that the USB protocol (enumeration, CBI/BOT transport,
; hubs) and the FAT file system access are handled by the RP2040, and the MSX
; talks to it through the mailbox described in pico_regs.asm.
;
; All the code in this file is stateless: work area is not used,
; all the required information is passed in registers or buffers.


; -----------------------------------------------------------------------------
; Auxiliary "private" routines
; -----------------------------------------------------------------------------

; --------------------------------------
; PV_EXEC: Execute a mailbox command and wait for it to finish
;
; Input:  A = Command
; Output: A = Result (USB error code or file result), Z if zero
;         All other registers are preserved
;
; If the panic buttons (CAPS+ESC) are pressed while waiting the running
; command is aborted and USB_ERR_PANIC_BUTTON_PRESSED is returned.

PV_EXEC:
    push bc
    push de
    push hl

    ld (PV_CMD),a
    ld d,0  ;D = 1 once the panic button has been handled

_PV_EXEC_WAIT:
    if IMPLEMENT_PANIC_BUTTON=1
    ld a,d
    or a
    jr nz,_PV_EXEC_POLL
    call PANIC_KEYS_PRESSED     ;Modifies A, B and C
    jr nz,_PV_EXEC_POLL
    ld a,PV_CMD_ABORT
    ld (PV_CMD),a
    ld d,1
    endif

_PV_EXEC_POLL:
    ld a,(PV_STATUS)
    and PV_ST_BUSY
    jr nz,_PV_EXEC_WAIT

    ld a,d
    or a
    ld a,USB_ERR_PANIC_BUTTON_PRESSED
    jr nz,_PV_EXEC_END
    ld a,(PV_RESULT)
_PV_EXEC_END:
    or a
    pop hl
    pop de
    pop bc
    ret


; --------------------------------------
; PV_READ_BUF: Copy data from the Pico data buffer (starting at offset 0)
;
; Input:  HL = Destination address
;         BC = Length (maximum PV_BUF_SIZE)
; Output: HL = Address after the last byte copied
;         Modifies AF, BC, DE

PV_READ_BUF:
    ex de,hl    ;DE = destination
    xor a
_PV_READ_BUF_PAGE:
    ld (PV_PAGE),a
    push af
    ld hl,PV_WIN
    ld a,b
    or a
    jr nz,_PV_READ_BUF_FULL
    ld a,c
    or a
    jr z,_PV_READ_BUF_END
    ldir        ;B=0, less than 256 bytes remaining
    jr _PV_READ_BUF_END
_PV_READ_BUF_FULL:
    push bc
    ld bc,PV_WIN_SIZE
    ldir
    pop bc
    dec b       ;Remaining length -= 256
    pop af
    inc a
    jr _PV_READ_BUF_PAGE
_PV_READ_BUF_END:
    pop af
    ex de,hl
    ret


; --------------------------------------
; PV_WRITE_BUF: Copy data to the Pico data buffer (starting at offset 0)
;
; Input:  HL = Source address
;         BC = Length (maximum PV_BUF_SIZE)
; Output: HL = Address after the last byte copied
;         Modifies AF, BC, DE

PV_WRITE_BUF:
    xor a
_PV_WRITE_BUF_PAGE:
    ld (PV_PAGE),a
    push af
    ld de,PV_WIN
    ld a,b
    or a
    jr nz,_PV_WRITE_BUF_FULL
    ld a,c
    or a
    jr z,_PV_WRITE_BUF_END
    ldir        ;B=0, less than 256 bytes remaining
    jr _PV_WRITE_BUF_END
_PV_WRITE_BUF_FULL:
    push bc
    ld bc,PV_WIN_SIZE
    ldir
    pop bc
    dec b       ;Remaining length -= 256
    pop af
    inc a
    jr _PV_WRITE_BUF_PAGE
_PV_WRITE_BUF_END:
    pop af
    ret


; --------------------------------------
; PV_WRITE_STRING
;
; Write a zero or "/" terminated string to the beginning of the Pico data
; buffer (if the terminator is "/" a zero is written). Strings longer
; than 63 characters are truncated, but HL still ends at the terminator.
;
; Input:  HL = Source address of the string
; Output: HL = Points to the terminator

PV_WRITE_STRING:
    xor a
    ld (PV_PAGE),a
    ld de,PV_WIN
    ld b,63     ;Room for the characters after the first one

    ld a,(hl)
    ld (de),a   ;We accept "/" only as the first char
    or a
    ret z
    inc de

_PV_WRITE_STRING_NEXT:
    inc hl
    ld a,(hl)
    or a
    jr z,_PV_WRITE_STRING_END
    cp "/"
    jr z,_PV_WRITE_STRING_END
    ld c,a
    ld a,b
    or a
    jr z,_PV_WRITE_STRING_NEXT  ;Name too long, discard the rest
    dec b
    ld a,c
    ld (de),a
    inc de
    jr _PV_WRITE_STRING_NEXT

_PV_WRITE_STRING_END:
    xor a
    ld (de),a
    ret


; --------------------------------------
; PV_SET_LENGTH: Store BC as the 16 bit length argument (ARG0-1)

PV_SET_LENGTH:
    ld a,c
    ld (PV_ARG),a
    ld a,b
    ld (PV_ARG+1),a
    ret


    if IMPLEMENT_PANIC_BUTTON=1

    ;Return Z=1 if CAPS+ESC is pressed
PANIC_KEYS_PRESSED:
    ld a,6
    call DO_SNSMAT
    and 1000b
    ld b,a
    ld a,7
    call DO_SNSMAT
    and 100b
    or b
    ret

    endif


; -----------------------------------------------------------------------------
; Mandatory routines
; -----------------------------------------------------------------------------

; -----------------------------------------------------------------------------
; HW_TEST: Check if the USB host controller hardware is operational
; -----------------------------------------------------------------------------
; Output: Cy = 0 if hardware is operational, 1 if it's not

HW_TEST:
    ld hl,PV_SIG
    ld de,_PV_SIGNATURE
    ld b,4
_HW_TEST_LOOP:
    ld a,(de)
    cp (hl)
    jr nz,_HW_TEST_ERR
    inc hl
    inc de
    djnz _HW_TEST_LOOP

    ld a,(PV_VERSION)
    cp PV_PROTOCOL_VERSION
    jr nz,_HW_TEST_ERR

    or a
    ret

_HW_TEST_ERR:
    scf
    ret

_PV_SIGNATURE:
    db "PVFD"


; -----------------------------------------------------------------------------
; HW_RESET: Reset the USB host
;
; Waits for the RP2040 to enumerate the device that is connected, if any.
; -----------------------------------------------------------------------------
; Input:  -
; Output: A = 1 if a USB device is connected
;             -1 if no USB device is connected
;         Cy = 1 if reset failed

HW_RESET:
    ld a,PV_CMD_RESET
    call PV_EXEC
    jr nz,_HW_RESET_ERR
    ld a,(PV_RESP)
    or a
    ret

_HW_RESET_ERR:
    scf
    ret


; -----------------------------------------------------------------------------
; HW_DEV_CHANGE: Check for changes in the device connection
;
; The returned status is relative to the last time that the routine
; was called.
; -----------------------------------------------------------------------------
; Input:  -
; Output: A = 1 if a USB device has been connected
;             0 if no change has been detected
;             -1 if the USB device has been disconnected
;         Cy = 1 if the operation failed

HW_DEV_CHANGE:
    ld a,PV_CMD_DEV_CHANGE
    call PV_EXEC
    jr nz,_HW_DEV_CHANGE_ERR
    ld a,(PV_RESP)
    or a
    ret

_HW_DEV_CHANGE_ERR:
    scf
    ret


; -----------------------------------------------------------------------------
; HW_BUS_RESET: Recover the connected device after an error
;
; The RP2040 clears the halt condition of the endpoints and resets the
; transport (Bulk-Only reset for devices that use it).
; -----------------------------------------------------------------------------
; Output: A  = 1
;         Cy = 1 on error

HW_BUS_RESET:
    ld a,PV_CMD_BUS_RESET
    call PV_EXEC
    ld a,1
    ret z
    scf
    ret


; -----------------------------------------------------------------------------
; HW_CONFIGURE_NAK_RETRY
; -----------------------------------------------------------------------------
; Input: Cy = 0 to use the normal command timeouts (this is the default)
;             1 to use long command timeouts, for operations like FORMAT

HW_CONFIGURE_NAK_RETRY:
    push af
    ld a,0
    rla
    ld (PV_ARG),a
    ld a,PV_CMD_LONG_TIMEOUT
    call PV_EXEC
    pop af
    ret


; -----------------------------------------------------------------------------
; File management routines
; -----------------------------------------------------------------------------

; -----------------------------------------------------------------------------
; HWF_MOUNT_DISK: Mounts a storage device if present.
;
; On success it also opens the root directory
; -----------------------------------------------------------------------------
; Input:  HL = address for the name of the device found
; Output: Cy = 0: ok
;              1: error, no device present or it's not a storage device

HWF_MOUNT_DISK:
    push hl
    ld a,PV_CMD_FS_MOUNT
    call PV_EXEC
    pop hl
    scf
    ret nz

    ld bc,28
    call PV_READ_BUF
    xor a
    ld (hl),a
    ret


; -----------------------------------------------------------------------------
; HWF_OPEN_FILE_DIR: Open a file or enter a directory from the current one
; -----------------------------------------------------------------------------
; Input:  HL = Address of file or directory name, relative to current
; Output: A  = 0: ok, file or directory open
;              1: generic error (e.g. no device found)
;              2: file or directory not found
;              
;         Cy = 0: file open (if no error)
;              1: directory open (if no error)
;         Z if ok, NZ if error
;         HL = Pointer to terminator of file or directory name

HWF_OPEN_FILE_DIR:
    call PV_WRITE_STRING

    push hl
    ld a,PV_CMD_FS_OPEN
    call PV_EXEC
    pop hl

    ld b,a
    cp USB_ERR_OK
    ld a,0
    scf
    ccf
    ret z   ;NC, Z

    ld a,b
    cp USB_ERR_OPEN_DIR
    ld a,0
    scf
    ret z   ;C, Z

    ld a,b
    cp USB_ERR_MISS_FILE
    ld a,2
    jr z,_HWF_OPEN_FILE_DIR_END
    dec a

_HWF_OPEN_FILE_DIR_END:
    or a    ;Force NZ
    ret


; -----------------------------------------------------------------------------
; HWF_CREATE_FILE: Create a new file in current directory, overwrite if exists
; -----------------------------------------------------------------------------
; Input:  HL = Address of file or directory name, relative to current
; Output: A  = 0: ok, file open
;              1: error (might be that a directory with the same name exists)

HWF_CREATE_FILE:
    call PV_WRITE_STRING

    ld a,PV_CMD_FS_CREATE
    call PV_EXEC

    cp USB_ERR_OK
    ld a,0
    ret z
    inc a
    ret


; -----------------------------------------------------------------------------
; HWF_CREATE_DIR: Create a new directory in current directory, open if exists
; -----------------------------------------------------------------------------
; Input:  HL = Address of file or directory name, relative to current
; Output: A  = 0: ok, file open
;              1: error (might be that a file with the same name exists)

HWF_CREATE_DIR:
    call PV_WRITE_STRING

    ld a,PV_CMD_FS_MKDIR
    call PV_EXEC

    cp USB_ERR_OK
    ld a,0
    ret z
    inc a
    ret


; -----------------------------------------------------------------------------
; HWF_CLOSE_FILE: Close open file or directory, update size in dir entry
; -----------------------------------------------------------------------------

HWF_CLOSE_FILE:
    ld a,PV_CMD_FS_CLOSE
    jp PV_EXEC


; -----------------------------------------------------------------------------
; HWF_DELETE_FILE: Delete file or directory
; -----------------------------------------------------------------------------
; Input:  HL = File or directory name
; Output: A  = 0: Ok
;              1: Error

HWF_DELETE_FILE:
    ;Very important: close any open file first, or it will be deleted!
    push hl
    call HWF_CLOSE_FILE
    pop hl

    call PV_WRITE_STRING

    ld a,PV_CMD_FS_DELETE
    call PV_EXEC
    or a
    ret z
    ld a,1
    ret


; -----------------------------------------------------------------------------
; HWF_ENUM_FILES: Enumerate files and directories in the current directory
;
; Files/directories whose first character is "_" will be skipped.
;
; The name of each files/directory will be put at the specified address,
; sequentially and with no separators, as a fixed 11 bytes string in the
; same format as in the directory entry (e.g. "FILE    EXT"); after the last
; entry a 0 byte will be placed.
; -----------------------------------------------------------------------------
; Input:  HL = Address of buffer to get file info into
;         BC = Maximum number of files/directories to enumerate
; Output: HL = Pointer to the 0 byte at the end of the list
;         BC = Number of filenames found

HWF_ENUM_FILES:
    push iy
    ld iy,0
    call _HWF_ENUM_FILES_CORE
    pop iy
    ret


; -----------------------------------------------------------------------------
; HWF_FIND_NTH_FILE: Find the Nth file in the current directory
;
; Files whose first character is "_" will be skipped.
;
; The name of the files will be put at the specified address,
; sequentially and with no separators, as a fixed 11 bytes string in the
; same format as in the directory entry (e.g. "FILE    EXT").
; -----------------------------------------------------------------------------
; Input:  HL = Address of buffer to get file info into
;         A  = Index of file to find (first is 0)
; Output: A  = 0: Ok
;              1: Generic error
;              2: File not found
;         HL = Pointer after the filename

HWF_FIND_NTH_FILE:
    push iy
    ld iy,1
    ld c,a
    ld b,0
    inc bc
    call _HWF_ENUM_FILES_CORE
    pop iy
    or a
    ret z
    cp USB_ERR_MISS_FILE
    ld a,2
    ret z
    dec a
    ret

;Input:  HL = Buffer, BC = Max number of files, IY = 0 to list, 1 to find Nth file
;Output: A  = 0 if the requested file was found, USB_ERR_MISS_FILE at the end
;             of the directory, other value on error
_HWF_ENUM_FILES_CORE:
    push hl
    push bc
    ld hl,_PV_ENUM_ALL_S
    call PV_WRITE_STRING
    pop bc
    pop hl

    ld de,0     ;Number of files found
    ld a,PV_CMD_FS_ENUM_FIRST
_HWF_ENUM_FILES_LOOP:
    push hl
    push de
    push bc
    call PV_EXEC
    pop bc
    pop de
    pop hl
    cp PV_RES_ENTRY
    jr nz,_HWF_ENUM_FILES_END

    push bc
    push hl
    push de
    ld bc,32
    call PV_READ_BUF    ;Directory entry, 32 bytes at HL
    pop de
    pop hl
    pop bc

    ld a,(hl)
    cp "_"
    jr z,_HWF_ENUM_DIR_NEXT
    cp "."  ;Skip "." and ".." entries
    jr z,_HWF_ENUM_DIR_NEXT

    push bc
    ld bc,11
    add hl,bc ;Point to file attributes byte
    pop bc

    ld a,(hl)
    and 1110b   ;"Hidden", "System" or "Volume" attribute set?
    jr nz,_HWF_ENUM_SKIP
    ld a,iyl
    or a
    jr z,_HWF_ENUM_USE
    bit 4,(hl)  ;"Directory" attribute set?
    jr z,_HWF_ENUM_DIR_OK

_HWF_ENUM_SKIP:
    push bc
    ld bc,-11
    add hl,bc
    pop bc
    jr _HWF_ENUM_DIR_NEXT

_HWF_ENUM_USE:
    bit 4,(hl)  ;"Directory" attribute set?
    jr z,_HWF_ENUM_DIR_OK
    dec hl
    set 7,(hl)
    inc hl
_HWF_ENUM_DIR_OK:

    inc de

    dec bc
    ld a,b
    or c
    jr z,_HWF_ENUM_FILES_END

    ld a,iyl
    or a
    jr nz,_HWF_ENUM_SKIP

_HWF_ENUM_DIR_NEXT:
    ld a,PV_CMD_FS_ENUM_NEXT
    jr _HWF_ENUM_FILES_LOOP

_HWF_ENUM_FILES_END:
    push af
    ld a,iyl
    or a
    jr nz,_HWF_ENUM_FILES_END_2
    ld (hl),0
_HWF_ENUM_FILES_END_2:
    pop af
    push de
    pop bc
    ret

_PV_ENUM_ALL_S:
    db "*",0


; -----------------------------------------------------------------------------
; HWF_SEEK_FILE: Move the pointer of the currently mounted file
; -----------------------------------------------------------------------------
; Input:  HL:DE = New pointer
; Output: A = 0: Ok
;             1: End of file
;             2: Other error

HWF_SEEK_FILE:
    ld a,e
    ld (PV_ARG),a
    ld a,d
    ld (PV_ARG+1),a
    ld a,l
    ld (PV_ARG+2),a
    ld a,h
    ld (PV_ARG+3),a

    ld a,PV_CMD_FS_SEEK
    call PV_EXEC
    ld a,2
    ret nz

    ld a,(PV_RESP)
    and 1
    ret     ;1 if the pointer is beyond the end of the file, 0 if ok


; -----------------------------------------------------------------------------
; HWF_READ_FILE: Read the currently mounted file
; -----------------------------------------------------------------------------
; Input:  BC = How many bytes to read
;         HL = Destination address
; Output: A  = 0: Ok
;              1: Error
;         BC = How many bytes actually read
;         HL = Address after last byte read

HWF_READ_FILE:
    push hl     ;Initial dest address

_HWF_READ_FILE_LOOP:
    ld a,b
    or c
    jr z,_HWF_READ_FILE_OK

    push bc     ;Remaining length
    ld a,b
    cp PV_BUF_SIZE/256
    jr c,_HWF_READ_FILE_LEN_OK
    ld bc,PV_BUF_SIZE
_HWF_READ_FILE_LEN_OK:
    push bc     ;Length requested in this chunk
    call PV_SET_LENGTH
    ld a,PV_CMD_FS_READ
    call PV_EXEC
    jr nz,_HWF_READ_FILE_ERR

    ld a,(PV_RESP)
    ld c,a
    ld a,(PV_RESP+1)
    ld b,a      ;BC = Bytes read in this chunk
    push bc
    call PV_READ_BUF
    pop bc
    pop de      ;DE = Length requested in this chunk
    push hl
    ld h,d
    ld l,e
    or a
    sbc hl,bc
    ld a,h
    or l        ;Z if the whole chunk was read
    pop hl      ;HL = Address after the last byte read
    pop de      ;DE = Remaining length
    jr nz,_HWF_READ_FILE_OK     ;Short read: end of file

    ex de,hl
    or a
    sbc hl,bc
    ld b,h
    ld c,l      ;BC = Remaining length
    ex de,hl
    jr _HWF_READ_FILE_LOOP

_HWF_READ_FILE_ERR:
    pop bc
    pop bc
    ld a,1
    jr _HWF_READ_FILE_END

_HWF_READ_FILE_OK:
    xor a

_HWF_READ_FILE_END:
    pop de      ;Initial dest address
    push af
    push hl
    or a
    sbc hl,de
    ld b,h
    ld c,l      ;BC = Bytes read
    pop hl
    pop af
    ret


; -----------------------------------------------------------------------------
; HWF_WRITE_FILE: Write to the currently mounted file
; -----------------------------------------------------------------------------
; Input:  BC = How many bytes to write
;         HL = Source address
; Output: A  = 0: Ok
;              1: Error
;         BC = How many bytes actually written
;         HL = Address after last byte written

HWF_WRITE_FILE:
    push hl     ;Initial source address

_HWF_WRITE_FILE_LOOP:
    ld a,b
    or c
    jr z,_HWF_WRITE_FILE_OK

    push bc     ;Remaining length
    ld a,b
    cp PV_BUF_SIZE/256
    jr c,_HWF_WRITE_FILE_LEN_OK
    ld bc,PV_BUF_SIZE
_HWF_WRITE_FILE_LEN_OK:
    push bc     ;Length of this chunk
    push hl     ;Source address of this chunk
    call PV_WRITE_BUF
    pop hl
    pop bc
    push bc
    call PV_SET_LENGTH
    ld a,PV_CMD_FS_WRITE
    call PV_EXEC
    jr nz,_HWF_WRITE_FILE_ERR

    ld a,(PV_RESP)
    ld e,a
    ld a,(PV_RESP+1)
    ld d,a      ;DE = Bytes written in this chunk
    pop bc      ;BC = Length of this chunk
    add hl,de   ;HL = Address after the last byte written
    push hl
    ld h,b
    ld l,c
    or a
    sbc hl,de
    ld a,h
    or l        ;Z if the whole chunk was written
    pop hl
    pop bc      ;BC = Remaining length
    jr nz,_HWF_WRITE_FILE_OK    ;Short write: no space left

    push hl
    ld h,b
    ld l,c
    or a
    sbc hl,de
    ld b,h
    ld c,l      ;BC = Remaining length
    pop hl
    jr _HWF_WRITE_FILE_LOOP

_HWF_WRITE_FILE_ERR:
    pop bc
    pop bc
    ld a,1
    jr _HWF_WRITE_FILE_END

_HWF_WRITE_FILE_OK:
    xor a

_HWF_WRITE_FILE_END:
    pop de      ;Initial source address
    push af
    push hl
    or a
    sbc hl,de
    ld b,h
    ld c,l      ;BC = Bytes written
    pop hl
    pop af
    ret


; -----------------------------------------------------------------------------
; HWF_GET_FILE_ATTR: Get attributes byte of currently mounted file
; -----------------------------------------------------------------------------
; Output: A  = 0: Ok
;              1: Error
;         B  = Attributes byte if ok

HWF_GET_FILE_ATTR:
    push hl
    push bc
    ld a,PV_CMD_FS_ATTR
    call PV_EXEC
    pop bc
    pop hl
    ld a,1
    ret nz

    ld a,(PV_RESP)
    ld b,a
    xor a
    ret
