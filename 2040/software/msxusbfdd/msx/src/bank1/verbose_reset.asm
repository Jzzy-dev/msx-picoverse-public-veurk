; MSXUSB USB FDD BIOS
; Based on the Rookie Drive USB FDD BIOS created by Konamiman in 2018
; This version is adapted to work with the latest version of the MSXUSB project
;
; This routine resets the USB hardware, resets and initializes the device,
; and prints the device name or the appropriate error message.
; It is executed at boot time and by CALL USBRESET.

VERBOSE_RESET:
    push iy
    ld iy,-36
    add iy,sp
    ld sp,iy
    call _VERBOSE_RESET
    ld iy,36
    add iy,sp
    ld sp,iy
    pop iy
    ret

_VERBOSE_RESET:
    xor a
    call WK_SET_STORAGE_DEV_FLAGS
    call WK_SET_MISC_FLAGS

    call HW_TEST
    ld hl,NOHARD_S
    jp c,PRINT

    ;Check for a FDD first. HWF_MOUNT_DISK makes the CH376 send mass storage
    ;(Bulk-Only) commands to the device, and some FDDs don't handle these well.

    push iy ;Buffer for the device name, USB_INIT_DEV modifies IY
    call _VERBOSE_RESET_INIT_FDD
    pop iy
    ld b,h  ;B = USB error code (must be saved before HL is modified)
    or a
    jp z,_VERBOSE_RESET_FDD_OK
    cp 4
    ld hl,NODEV_S
    jp z,PRINT

    ;* Not a FDD, or error when initializing it: check if it's a storage device

    push bc ;B = USB error code
    push af ;A = FDD init result
    call HW_RESET   ;Let the CH376 enumerate the device from scratch
    push iy
    pop hl
    push hl
    call HWF_MOUNT_DISK
    pop hl
    jr c,_VERBOSE_RESET_NO_STOR
    pop af
    pop bc

    push hl
    ld hl,STOR_FOUND_S
    call PRINT
    pop hl
    ld b,0
    ;Print the device name, collapsing multiple spaces to a single one
_HW_RESET_PRINT:
    ld a,(hl)
    inc hl
    or a
    jr z,_HW_RESET_PRINT_END
    cp ' '
    jr nz,_HW_RESET_PRINT_GO
    cp b
    jr z,_HW_RESET_PRINT
_HW_RESET_PRINT_GO:
    ld b,a
    call CHPUT
    jr _HW_RESET_PRINT
_HW_RESET_PRINT_END:
    call DSK_INIT_WK_FOR_STORAGE_DEV
    ret

    ;* Neither a FDD nor a storage device: print the appropriate error

_VERBOSE_RESET_NO_STOR:
    pop af
    pop bc
    dec a
    ld hl,NO_CBI_DEV_S
    jp z,PRINT
    dec a
    ld hl,RESERR_S
    jp nz,PRINT

    ld a,b  ;A = USB error code
    ld hl,DEVERR_S
    jp PRINT_ERROR

_VERBOSE_RESET_FDD_OK:
    call WK_GET_MISC_FLAGS
    and 1
    ld hl,HUB_FOUND_S
    call nz,PRINT

    ld hl,YES_CBI_DEV_S
    call PRINT
    call PRINT_DEVICE_INFO

    if BOOT_SELFTEST = 1
    jp BOOT_SELFTEST_RUN
    else
    ret
    endif
    ;--- Reset the USB hardware and try to initialize the device as a FDD
    ;    Output: A = 0: Ok, device is a FDD
    ;                1: Device is not a FDD
    ;                2: Error when initializing the device, H = USB error code
    ;                3: Error when resetting the USB hardware
    ;                4: No device connected

_VERBOSE_RESET_INIT_FDD:
    ld b,5
_HW_RESET_TRY:
    push bc
    call HW_RESET
    pop bc
    jr nc,_HW_RESET_TRY_OK
    djnz _HW_RESET_TRY
    ld a,3
    ret
_HW_RESET_TRY_OK:
    inc a
    ld a,4
    ret z

    ld b,5
_TRY_USB_INIT_DEV:
    push bc
    call USB_INIT_DEV
    ld h,b
    pop bc
    cp 2
    ret c
    push hl
    push bc
    call HW_BUS_RESET   ;Get the device back to the default state before retrying
    pop bc
    pop hl
    djnz _TRY_USB_INIT_DEV
    ld a,2
    ret

    if WAIT_KEY_ON_INIT = 1
INIHRD_NEXT:
    jp CHGET
    endif

PRINT:
	ld a,(hl)
	or a
	ret z
	call CHPUT
	inc hl
	jr PRINT

    ;Print A as a two digit hexadecimal number

PRINT_HEX:
    push af
    rrca
    rrca
    rrca
    rrca
    call _PRINT_HEX_DIGIT
    pop af
_PRINT_HEX_DIGIT:
    and 0Fh
    add "0"
    cp "9"+1
    jr c,_PRINT_HEX_DIGIT_2
    add "A"-"9"-1
_PRINT_HEX_DIGIT_2:
    jp CHPUT


; -----------------------------------------------------------------------------
; Print the device name from INQUIRY command

PRINT_DEVICE_INFO_STACK_SPACE: equ 36

PRINT_DEVICE_INFO:
    ld hl,-PRINT_DEVICE_INFO_STACK_SPACE
    add hl,sp
    ld sp,hl

    ld b,3  ;Some drives stall on first command after reset so try a few times
_TRY_INQUIRY:    
    push bc
    push hl
    pop de
    ld hl,INIQUIRY_CMD
    ld bc,36
    ld a,1
    or a
    push de
    call USB_EXECUTE_CBI_WITH_RETRY
    pop hl
    pop bc
    or a
    jr z,_INQUIRY_OK
    djnz _TRY_INQUIRY
    jr _PRINT_DEVICE_INFO_ERR
_INQUIRY_OK:

    ld bc,8
    add hl,bc
    ld b,8
    call PRINT_SPACE_PADDED_STRING
    ld a,' '
    call CHPUT

    ld bc,8 ;base + 16
    add hl,bc
    ld b,16
    call PRINT_SPACE_PADDED_STRING
    ld a,' '
    call CHPUT

    ld bc,16 ;base + 32
    add hl,bc
    ld b,4
    call PRINT_SPACE_PADDED_STRING

    ld hl,CRLF_S
    call PRINT

    jr _PRINT_DEVICE_INFO_END

_PRINT_DEVICE_INFO_ERR:
    push af
    ld hl,ERR_INQUIRY_S
    call PRINT_ERROR
    pop af
    call PRINT_DIAG
_PRINT_DEVICE_INFO_END:    
    ld hl,PRINT_DEVICE_INFO_STACK_SPACE
    add hl,sp
    ld sp,hl
    ret

INIQUIRY_CMD:
    db 12h, 0, 0, 0, 36, 0, 0, 0, 0, 0, 0, 0


    ; Print a fixed-length space-padded string, skipping the padding
    ; Input: HL = String address
    ;        B  = String length

PRINT_SPACE_PADDED_STRING:
    push hl
    call _PRINT_SPACE_PADDED_STRING
    pop hl
    ret

_PRINT_SPACE_PADDED_STRING:
    ld e,b
    ld d,0
    push hl
    add hl,de   ;HL points past the last char of the string
_PSPS_Z_LOOP:
    dec hl
    ld a,(hl)
    cp ' '
    jr nz,_PSPS_DO
    djnz _PSPS_Z_LOOP
    pop hl
    ret         ;All the string is spaces, do nothing

_PSPS_DO:
    pop hl
_PSPS_P_LOOP:
    ld a,(hl)
    call CHPUT
    inc hl
    djnz _PSPS_P_LOOP

    ret


; -----------------------------------------------------------------------------
; Print an error message and the description of an USB error code
;
; Input: HL = Error message
;        A = Error code

PRINT_ERROR:
    push af
    call PRINT
    pop af

    ld ix,PRINT_ERROR_DESCRIPTION
    ld iy,ROM_BANK_0
    call CALL_BANK

    ld hl,CRLF_S
    jp PRINT


; -----------------------------------------------------------------------------
; Print the diagnostics of the floppy drive transport
;
; Format: "Diag: PP FF SS EE XX RR NN SA SB CC" with PP = protocol (0 CBI, 1 CB, 50 Bulk-Only),
; FF = flags (bit 0 = the drive has an interrupt endpoint, bit 1 = it reported a
; status), then, for the first transfer of the last command that did not succeed:
; SS = stage (1 command block, 2 data, 3 status, 5/7 Bulk-Only command/status,
; +10h while requesting the sense data, 20h recovery, FF = none),
; EE = endpoint (00 = control), XX = result (00 ok, 01 failed, 02 stalled,
; 03 timeout, E0 did not complete), RR = USB error code, NN = bytes transferred,
; SA = SIE status flags (80 data toggle error, 40 ack, 20 stall, 10 nak, 08 rx timeout,
; 04 overflow, 02 bit stuff, 01 crc), SB = duration of the slowest transfer since the
; drive was connected, in units of 100 ms (FF = 25.5 s or more),
; and finally CC = number of commands that failed since the drive was connected

PRINT_DIAG:
    ld a,PV_CMD_DEV_INFO
    call PV_EXEC
    ret nz
    ld hl,DIAG_S
    call PRINT
    ld hl,PV_RESP+6
    ld b,10
_PRINT_DIAG_LOOP:
    ld a,(hl)
    inc hl
    push hl
    push bc
    call PRINT_HEX
    ld a,' '
    call CHPUT
    pop bc
    pop hl
    djnz _PRINT_DIAG_LOOP
    ld hl,CRLF_S
    call PRINT

    ;Hardware diagnostics, from the Pico data buffer
    ld hl,-PRINT_DIAG_HW_SPACE
    add hl,sp
    ld sp,hl
    ld bc,PRINT_DIAG_HW_SPACE
    call PV_READ_BUF
    ld hl,DIAG_HW_S
    call PRINT
    ld hl,0
    add hl,sp
    ld b,6
    call PRINT_HEX_BYTES
    push hl
    ld hl,CRLF_S
    call PRINT
    pop hl
    inc hl      ;Skip the two unused bytes
    inc hl
    ld c,4      ;4 lines of 4 words
_PRINT_DIAG_HW_LINE:
    push bc
    ld b,4
_PRINT_DIAG_HW_WORD:
    push bc
    call PRINT_WORD
    pop bc
    djnz _PRINT_DIAG_HW_WORD
    push hl
    ld hl,CRLF_S
    call PRINT
    pop hl
    pop bc
    dec c
    jr nz,_PRINT_DIAG_HW_LINE
    ld hl,PRINT_DIAG_HW_SPACE
    add hl,sp
    ld sp,hl
    ret

PRINT_DIAG_HW_SPACE: equ 72

; Print B bytes starting at HL, each followed by a space. Returns HL after the last byte

PRINT_HEX_BYTES:
    ld a,(hl)
    inc hl
    push hl
    push bc
    call PRINT_HEX
    ld a,' '
    call CHPUT
    pop bc
    pop hl
    djnz PRINT_HEX_BYTES
    ret

; Print the little endian 32 bit word at HL as 8 hex digits followed by a space. Returns HL after the word

PRINT_WORD:
    push hl
    ld de,3
    add hl,de
    ld b,4
_PRINT_WORD_LOOP:
    ld a,(hl)
    dec hl
    push hl
    push bc
    call PRINT_HEX
    pop bc
    pop hl
    djnz _PRINT_WORD_LOOP
    ld a,' '
    call CHPUT
    pop hl
    ld de,4
    add hl,de
    ret


; -----------------------------------------------------------------------------
; Boot self test (see BOOT_SELFTEST in config.asm)
;
; Runs, in the same order Windows does, TEST UNIT READY, READ FORMAT CAPACITIES and READ
; CAPACITY on the floppy drive and prints, for each one, "AA DD EE" with AA = USB error
; code, DD = ASC and EE = ASCQ, the first 8 bytes of the data (only if there was no error)
; and, if the command failed, the diagnostics line.

BOOT_SELFTEST_STACK_SPACE: equ 40

BOOT_SELFTEST_RUN:
    ld hl,-BOOT_SELFTEST_STACK_SPACE
    add hl,sp
    ld sp,hl

    ld hl,SELFTEST_TUR_S
    call PRINT
    ld hl,0
    add hl,sp
    ex de,hl            ;DE = buffer
    ld hl,_UFI_TEST_UNIT_READY_CMD
    ld bc,0
    xor a               ;A=0 and Cy=0: receive, don't retry "media changed"
    call BOOT_SELFTEST_CMD

    ld hl,SELFTEST_FMT_S
    call PRINT
    ld hl,0
    add hl,sp
    ex de,hl
    ld hl,_SELFTEST_READ_FORMAT_CAPACITIES_CMD
    ld bc,36
    xor a
    call BOOT_SELFTEST_CMD
    call z,BOOT_SELFTEST_PRINT8

    ld hl,SELFTEST_CAP_S
    call PRINT
    ld hl,0
    add hl,sp
    ex de,hl
    ld hl,_SELFTEST_READ_CAPACITY_CMD
    ld bc,8
    xor a
    call BOOT_SELFTEST_CMD
    call z,BOOT_SELFTEST_PRINT8

    ld hl,BOOT_SELFTEST_STACK_SPACE
    add hl,sp
    ld sp,hl
    ret

;Print the first 8 bytes of the buffer at the top of the stack of BOOT_SELFTEST_RUN
;(the caller's return address is on the stack, and the buffer is above it)
BOOT_SELFTEST_PRINT8:
    ld hl,2
    add hl,sp
    ld b,8
_BOOT_SELFTEST_PRINT8_LOOP:
    ld a,(hl)
    inc hl
    push hl
    push bc
    call PRINT_HEX
    ld a,' '
    call CHPUT
    pop bc
    pop hl
    djnz _BOOT_SELFTEST_PRINT8_LOOP
    ld hl,CRLF_S
    jp PRINT

;Execute the command and print its result
;Input:  HL = command, DE = buffer, BC = length, A=0, Cy=0
;Output: Z if the command succeeded (no USB error)
BOOT_SELFTEST_CMD:
    call USB_EXECUTE_CBI_WITH_RETRY
    push af
    push de
    call PRINT_HEX      ;USB error code
    ld a,' '
    call CHPUT
    pop de
    push de
    ld a,d
    call PRINT_HEX      ;ASC
    ld a,' '
    call CHPUT
    pop de
    ld a,e
    call PRINT_HEX      ;ASCQ
    ld hl,CRLF_S
    call PRINT
    pop af
    or a
    ret z
    call PRINT_DIAG
    or 1                ;NZ
    ret

_SELFTEST_READ_CAPACITY_CMD:
    db 25h, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0

_SELFTEST_READ_FORMAT_CAPACITIES_CMD:
    db 23h, 0, 0, 0, 0, 0, 0, 0, 36, 0, 0, 0

; -----------------------------------------------------------------------------
; Strings

ROOKIE_S:
	db "PicoVerse USB FDD BIOS "
    include "../build/version.asm"
    db 13,10
	db "Based on USBHOST NestorBIOS 2.2",13,10
	db "(c) 2018-2022 Konamiman",13,10
    db "(c) 2026 The Retro Hacker",13,10
	db 13,10
    db "Initializing device...",13
	db 0

NOHARD_S:
    db  "PicoVerse USB host not found!"
CRLF_S:
    db 13,10,0

NODEV_S:
    db  "No USB device found",27,"K",13,10,0

NO_CBI_DEV_S:
    db  "USB device found, but it's not a FDD unit",13,10,0

YES_CBI_DEV_S:
    db  "USB FDD found: ",27,"K",0    

RESERR_S:
    db  "ERROR initializing USB host or resetting USB device",13,10,0

DEVERR_S:
    db  "ERROR querying the USB device: ",0

ERR_INQUIRY_S:
    db  "ERROR querying the device name: ",0

DIAG_S:
    db  "Diag: ",0
DIAG_HW_S:
    db  "HW: ",0

SELFTEST_TUR_S:
    db  "TUR: ",0

SELFTEST_FMT_S:
    db  "FMT: ",0

SELFTEST_CAP_S:
    db  "CAP: ",0

STOR_FOUND_S:
    db "USB storage device found: ",0

HUB_FOUND_S:
    db "USB hub found",27,"K",13,10,0
