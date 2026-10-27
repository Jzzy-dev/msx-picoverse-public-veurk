;==============================================================================
; PICOAMP.COM - a full-screen player for the PicoVerse Audio Engine, MSX-DOS
;
;   PICOAMP            start in the root folder of the card
;   PICOAMP /MUSIC     start in a folder
;
; MSX2: SCREEN 4 with every screen cell a pattern of its own (a bitmap with
; two colours per 8-pixel row). The engine decodes and analyses the sound;
; this program copies its status and its spectrum/VU/scope data once a frame
; and draws what changed.
;
; Keys: Up/Down select, Left/Right page, Enter open/play, BS parent folder,
;       Space pause/play, S stop, N next, P previous, + (or =) / - volume,
;       , / . 10 seconds back / on, H shuffle, R repeat,
;       V spectrum / oscilloscope, G equalizer screen, E next preset,
;       ESC or Q quit (the music keeps playing: the cartridge plays it).
; Equalizer screen: Left/Right choose a band (or the balance), Up/Down set
;       it, 0 back to the middle, E next preset, G / ESC / Enter close.
; Mouse (either port): click a list entry to select it, click it again to
;       open or play it; the right button goes to the parent folder (closes
;       the equalizer). The control row, SHF/REP/EQ, the volume, the seek
;       bar, the view label and the equalizer sliders can all be clicked.
;
; Assemble: sjasmplus --raw=PICOAMP.COM picoamp.asm
;           (digits.inc from gen_digits.py, tables.inc from gen_tables.py)
;==============================================================================

        org #0100

BDOS    equ #0005
RDSLT   equ #000C
WRSLT   equ #0014
CALSLT  equ #001C
ENASLT  equ #0024
CHGMOD  equ #005F
GTTRIG  equ #00D8
GTPAD   equ #00DB
EXPTBL  equ #FCC1
JIFFY   equ #FC9E
RAMAD1  equ #F342
RG1SAV  equ #F3E0
RG8SAV  equ #FFE7
CGTABL  equ #0004
ROMID   equ #002B               ; bit 7: 50 Hz

VDP_D   equ #98
VDP_C   equ #99
VDP_P   equ #9A

W_ID     equ #4000
W_TOKEN  equ #4010
W_CODE   equ #4011
W_DONE   equ #4012
W_RESULT equ #4013
W_RLEN   equ #4014
W_HB     equ #4016
W_STATUS equ #4020
W_ARGS   equ #4100
W_REPLY  equ #4400
W_VIZ    equ #4C00

C_PING   equ #01
C_DOPEN  equ #10
C_DREAD  equ #11
C_DENTER equ #12
C_PLAYI  equ #20
C_PAUSE  equ #22
C_STOP   equ #23
C_NEXT   equ #24
C_PREV   equ #25
C_SEEK   equ #26
C_VOLUME equ #27
C_MODE   equ #28
C_EQ     equ #2A
C_BAL    equ #2B
M_REPEAT equ #01
M_SHUF   equ #02
E_BUSY   equ #0B

; status block offsets
ST_SEQ   equ 0
ST_STATE equ 1
ST_FLAGS equ 2
ST_VOL   equ 3
ST_ELAPS equ 4
ST_TOTAL equ 6
ST_BRATE equ 8
ST_RATE  equ 10
ST_CHAN  equ 12
ST_PLPOS equ 19
ST_PLCNT equ 21
ST_MODE  equ 23
ST_TITLE equ 32
ST_ARTIST equ 80
ST_ALBUM equ 120
ST_BUFP  equ 163
ST_DGEN  equ 173
ST_DIRST equ 177
ST_EQ    equ 179
ST_BAL   equ 184
F_SEEK   equ #04
F_STREAM equ #10
F_BUFFER equ #20

; visualisation block offsets
V_SEQ    equ #00
V_LEVEL  equ #04
V_PEAK   equ #20
V_VU     equ #40
V_SCOPE  equ #48
V_FREQ   equ #90
NBARS    equ 24

; palette indices (set by setpal)
BLK     equ 1
BLUE_D  equ 2
BLUE    equ 3
CYAN    equ 4
WHITE   equ 5
GREY    equ 6
GREEN_D equ 7
GREEN   equ 8
YELLOW  equ 9
ORANGE  equ 10
RED     equ 11
MAGENTA equ 12
GREY_D  equ 13
NAVY    equ 14
GREY_L  equ 15
A_TITLE equ WHITE*16+BLUE
A_NORM  equ WHITE*16+NAVY
A_SEL   equ NAVY*16+CYAN
A_BAR   equ GREY_L*16+BLUE_D

LIST_ROW equ 19
LIST_N   equ 4

; tickers (long texts that scroll): one record each
T_ACT    equ 0              ; 1 = longer than its place
T_LEN    equ 1
T_W      equ 2
T_ROW    equ 3
T_COL    equ 4
T_START  equ 5              ; JIFFY when its turn (or its round) began
T_LAST   equ 7              ; pixel offset shown
T_TEXT   equ 9              ; up to 63 characters and a 0
TSIZE    equ T_TEXT+64
GAP      equ 4              ; spaces between the end and the start again

;------------------------------------------------------------------------------
start:  ld sp,(#0006)
        call bss_init
        call find
        jp c,notfound
        ld hl,W_DONE
        call rd
        ld (token),a
        ld a,C_PING
        ld bc,0
        call cmd
        jp c,noanswer
        ; the starting folder: the argument, or the root
        ld hl,#0080
        ld de,argbuf
        ld a,(hl)
        or a
        jr z,.root
        ld b,a
        inc hl
.skip:  ld a,(hl)
        cp ' '
        jr nz,.cp
        inc hl
        djnz .skip
        jr .root
.cp:    ld a,(hl)
        cp ' '+1
        jr c,.end
        ld (de),a
        inc hl
        inc de
        djnz .cp
.end:   xor a
        ld (de),a
        jr .go
.root:  ld a,'/'
        ld (de),a
        inc de
        xor a
        ld (de),a
.go:    call screen_init
        call hz_init
        call draw_static
        call fetch_status
        ld hl,st+ST_EQ
        call find_eq
        call show_view
        ld hl,argbuf
        call open_path
        call draw_info
mainloop:
        ei
        halt
        call fetch_frame
        call draw_view
        ld a,(gen_new)
        ld hl,gen_shown
        cp (hl)
        jr z,.same
        ld (hl),a
        call fetch_status
        call draw_info
.same:  ld a,(loading)
        or a
        call nz,check_loading
        call ticker_run
        call mouse
        call keys
        jr mainloop

bss_init:
        ld hl,bss_start
        ld de,bss_start+1
        ld bc,bss_end-bss_start-1
        ld (hl),0
        ldir
        ld hl,cellcache
        ld bc,(NBARS+2)*10
        ld a,#FF
        call memset
        ld hl,scope_old
        ld bc,64
        ld a,40
        call memset
        ld hl,bar_last
        ld bc,(NBARS+2)*4       ; and bar_raw
        ld a,#FF
        call memset
        ld a,#FF
        ld (tk_title+T_ROW),a
        ld (tk_artist+T_ROW),a
        ld (tk_list+T_ROW),a
        ret

memset: ld (hl),a               ; BC bytes at HL = A (BC > 0)
        ld d,h
        ld e,l
        inc de
        dec bc
        ld a,b
        or c
        ret z
        ldir
        ret

; one second in interrupts: 50 or 60
hz_init:
        ld a,(EXPTBL)
        ld hl,ROMID
        call RDSLT
        ei
        ld b,60
        bit 7,a
        jr z,.s
        ld b,50
.s:     ld a,b
        ld (hold),a
        ret

;------------------------------------------------------------------------------
; engine access: commands through RDSLT/WRSLT (as PVTEST), the per-frame block
; copies with page 1 switched to the engine (ENASLT), interrupts off meanwhile.

; copy BC bytes from engine address HL to DE (page 1 switched for the copy)
eng_copy:
        di
        push hl
        push de
        push bc
        ld a,(slot)
        ld h,#40
        call ENASLT
        di
        pop bc
        pop de
        pop hl
        ldir
        ld a,(RAMAD1)
        ld h,#40
        call ENASLT
        ei
        ret

; the picture data (only when the engine has new data: its sequence byte
; changed) and the display generation of the status, with page 1 switched
; to the engine once
fetch_frame:
        di
        ld a,(slot)
        ld h,#40
        call ENASLT
        di
        ld a,(W_STATUS+ST_DGEN)
        ld (gen_new),a
        ld a,(W_VIZ+V_SEQ)
        ld hl,viz_seq
        cp (hl)
        jr z,.same
        ld (hl),a
        ld bc,V_SCOPE           ; levels, peaks, VU
        ld a,(view)
        or a
        jr z,.cp
        ld bc,V_SCOPE+64        ; and the scope
.cp:    ld hl,W_VIZ
        ld de,viz
        ldir
        ld a,1
        ld (viz_new),a
.same:  ld a,(RAMAD1)
        ld h,#40
        call ENASLT
        ei
        ret

fetch_status:
        ld hl,W_STATUS
        ld de,st
        ld bc,224
        jp eng_copy

; cmd - A = command, BC = argument bytes in 'args'
; Output: CF=1 no answer (3 s), else ZF=1 if PVA_OK, A = result, reply in 'reply'
cmd:    ld (code),a
        ld a,b
        or c
        jr z,.noargs
        ld hl,args
        ld de,W_ARGS
        call wrblock
.noargs:
        ld a,(token)
        inc a
        ld (token),a
        ld e,a
        ld hl,W_TOKEN
        call wr
        ld a,(code)
        ld e,a
        ld hl,W_CODE
        call wr
        ld hl,(JIFFY)
        ld (t0),hl
.wait:  ld hl,W_DONE
        call rd
        ld hl,token
        cp (hl)
        jr z,.done
        ld hl,(JIFFY)
        ld de,(t0)
        or a
        sbc hl,de
        ld de,180
        or a
        sbc hl,de
        jr c,.wait
        scf
        ret
.done:  ld hl,W_RLEN
        call rd
        ld (rlen),a
        ld hl,W_RLEN+1
        call rd
        and 7
        ld (rlen+1),a
        ld hl,(rlen)
        ld de,REPLY_MAX
        or a
        sbc hl,de
        jr c,.lenok
        ld (rlen),de
.lenok: ld bc,(rlen)
        ld a,b
        or c
        jr z,.empty
        ld hl,W_REPLY
        ld de,reply
        call eng_copy
.empty: ld hl,(rlen)
        ld de,reply
        add hl,de
        ld (hl),0
        ld hl,W_RESULT
        call rd
        ld (result),a
        or a
        ret

cmd_u16:                        ; A = command, HL = argument
        ld (args),hl
        ld bc,2
        jp cmd

cmd_u8:                         ; A = command, E = argument
        push af
        ld a,e
        ld (args),a
        pop af
        ld bc,1
        jp cmd

cmd0:   ld bc,0                 ; A = command, no arguments
        jp cmd

rd:     push bc
        push de
        ld a,(slot)
        call RDSLT
        ei
        pop de
        pop bc
        ret

wr:     push bc
        push de
        ld a,(slot)
        call WRSLT
        ei
        pop de
        pop bc
        ret

wrblock:
.l:     ld a,b
        or c
        ret z
        ld a,(hl)
        push hl
        push de
        push bc
        ex de,hl
        ld e,a
        call wr
        pop bc
        pop de
        pop hl
        inc hl
        inc de
        dec bc
        jr .l

; find - "PVAUDIO" at 4000h in every slot and sub-slot. CF=1 not found.
find:   ld b,0
.p:     ld hl,EXPTBL
        ld e,b
        ld d,0
        add hl,de
        ld a,(hl)
        and #80
        or b
        ld c,a
        bit 7,a
        jr z,.one
        ld d,0
.s:     ld a,d
        rlca
        rlca
        or c
        ld (slot),a
        push bc
        push de
        call check
        pop de
        pop bc
        ret nc
        inc d
        ld a,d
        cp 4
        jr c,.s
        jr .nextp
.one:   ld a,c
        ld (slot),a
        push bc
        call check
        pop bc
        ret nc
.nextp: inc b
        ld a,b
        cp 4
        jr c,.p
        scf
        ret

check:  ld hl,W_ID
        ld de,idtext
        ld b,8
.c:     push bc
        push de
        call rd
        pop de
        pop bc
        ex de,hl
        cp (hl)
        ex de,hl
        scf
        ret nz
        inc hl
        inc de
        djnz .c
        or a
        ret

;------------------------------------------------------------------------------
; screen: SCREEN 4, palette, every cell its own pattern, the mouse pointer
; sprites (8 x 8, hidden), the font
screen_init:
        ld a,4
        ld ix,CHGMOD
        call bios
        call setpal
        ; name table: 0..255 three times
        ld hl,#1800
        call vwrite
        ld b,3
.nt:    push bc
        xor a
.ntc:   out (VDP_D),a
        inc a
        jr nz,.ntc
        pop bc
        djnz .nt
        ei
        ; patterns 0, colours navy
        ld hl,#0000
        ld bc,#1800
        xor a
        call vfill
        ld hl,#2000
        ld bc,#1800
        ld a,NAVY*16+NAVY
        call vfill
        ; sprites: 8 x 8, not magnified, shown (R#1 bits 1-0, R#8 bit 1)
        ld a,(RG1SAV)
        and #FC
        ld (RG1SAV),a
        ld b,1
        call vreg
        ld a,(RG8SAV)
        and #FD
        ld (RG8SAV),a
        ld b,8
        call vreg
        ld hl,#3800             ; pattern 0: the arrow
        call vwrite
        ld hl,pat_arrow
        ld b,8
        ld c,VDP_D
        otir
        ei
        ld hl,#1C00             ; colours: sprite 0 white, sprite 1 (its shadow) black
        ld bc,16
        ld a,WHITE
        call vfill
        ld hl,#1C10
        ld bc,16
        ld a,BLK
        call vfill
        ld hl,#1E00             ; none shown
        call vwrite
        ld a,216
        out (VDP_D),a
        ei
        ; backdrop colour (R#7)
        ld a,NAVY
        ld b,7
        call vreg
        ; the machine's font, shifted one pixel right (it is drawn for 6-pixel cells)
        ld a,(EXPTBL)
        ld hl,CGTABL
        call RDSLT
        ld (tmp16),a            ; RDSLT changes DE
        ld a,(EXPTBL)
        ld hl,CGTABL+1
        call RDSLT
        ld h,a
        ld a,(tmp16)
        ld l,a
        ld de,32*8
        add hl,de               ; from the space
        ld de,font
        ld bc,96*8
.f:     push bc
        push de
        ld a,(EXPTBL)
        call RDSLT
        pop de
        pop bc
        srl a
        ld (de),a
        inc hl
        inc de
        dec bc
        ld a,b
        or c
        jr nz,.f
        ei
        ret

; call the main ROM routine at IX (A and the other registers passed)
bios:   ld iy,(EXPTBL-1)
        call CALSLT
        ei
        ret

; VRAM write address HL (below 16 KB). The caller keeps interrupts off until
; its bytes are out: the interrupt routine talks to the VDP as well.
vwrite: di
        ld a,l
        out (VDP_C),a
        ld a,h
        and #3F
        or #40
        out (VDP_C),a
        ret

; fill BC bytes of VRAM from HL with A
vfill:  push af
        call vwrite
        pop af
        ld d,a
.l:     ld a,d
        out (VDP_D),a
        dec bc
        ld a,b
        or c
        jr nz,.l
        ei
        ret

; VDP register B = A
vreg:   di
        out (VDP_C),a
        ld a,b
        or #80
        out (VDP_C),a
        ei
        ret

setpal: xor a
        ld b,16
        call vreg
        ld hl,palette
        ld b,32
        ld c,VDP_P
        otir
        ret

restore_screen:
        xor a
        ld b,16
        call vreg
        ld hl,palette_msx
        ld b,32
        ld c,VDP_P
        otir
        xor a
        ld ix,CHGMOD
        jp bios

;------------------------------------------------------------------------------
; cells: row D (0-23), column E (0-31). Pattern at D*256+E*8, colour +2000h.
cell_addr:                      ; HL = pattern address of cell DE
        ld h,d
        ld a,e
        add a,a
        add a,a
        add a,a
        ld l,a
        ret

; put_cell: DE = cell, IX = 8 pattern bytes, A = colour byte for all rows
put_cell:
        ld (cellcol),a
        call cell_addr
        push hl
        call vwrite
        push ix
        pop hl
        ld b,8
        ld c,VDP_D
        otir
        pop hl
        ld a,h
        add a,#20
        ld h,a
        call vwrite
        ld a,(cellcol)
        ld b,8
.c:     out (VDP_D),a
        djnz .c
        ei
        ret

; put_cell2: DE = cell, IX = 8 pattern bytes, IY = 8 colour bytes
put_cell2:
        call cell_addr
        push hl
        call vwrite
        push ix
        pop hl
        ld b,8
        ld c,VDP_D
        otir
        pop hl
        ld a,h
        add a,#20
        ld h,a
        call vwrite
        push iy
        pop hl
        ld b,8
        otir
        ei
        ret

; put_char: DE = cell, A = character, C = colour
put_char:
        push de
        push bc
        sub 32
        jr nc,.ok
        xor a
.ok:    cp 96
        jr c,.ok2
        ld a,'?'-32
.ok2:   ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        ld bc,font
        add hl,bc
        push hl
        pop ix
        pop bc
        push bc
        ld a,c
        call put_cell
        pop bc
        pop de
        ret

; put_str: DE = cell, HL = 0-terminated text, C = colour, B = width (pads with spaces)
put_str:
.l:     ld a,b
        or a
        ret z
        ld a,(hl)
        or a
        jr z,.pad
        inc hl
        push hl
        push bc
        call put_char
        pop bc
        pop hl
        inc e
        dec b
        jr .l
.pad:   ld a,' '
        push hl
        push bc
        call put_char
        pop bc
        pop hl
        inc e
        djnz .pad
        ret

; fill_row: D = row, E = first column, B = cells, C = colour (blank)
fill_row:
.l:     push bc
        ld a,' '
        call put_char
        pop bc
        inc e
        djnz .l
        ret

; a big digit (0-9 in A) at cell DE: 2 x 3 cells
put_digit:
        push de
        ld l,a
        ld h,0
        add hl,hl               ; *48
        add hl,hl
        add hl,hl
        add hl,hl
        ld b,h
        ld c,l
        add hl,hl
        add hl,bc
        ld bc,digits
        add hl,bc
        push hl
        pop ix
        ld b,3
.r:     push bc
        ld a,RED*16+NAVY
        call put_cell
        ld bc,8
        add ix,bc
        inc e
        ld a,RED*16+NAVY
        call put_cell
        ld bc,8
        add ix,bc
        dec e
        inc d
        pop bc
        djnz .r
        pop de
        ret

put_colon:
        push de
        ld ix,colon
        ld b,3
.r:     push bc
        ld a,RED*16+NAVY
        call put_cell
        ld bc,8
        add ix,bc
        inc d
        pop bc
        djnz .r
        pop de
        ret

;------------------------------------------------------------------------------
; the parts that do not change
draw_static:
        ld de,#0000
        ld b,32
        ld c,A_TITLE
        call fill_row
        ld de,#0001
        ld hl,txt_title
        ld b,9
        ld c,A_TITLE
        call put_str
        ld de,#001F             ; close box for the mouse
        ld ix,pat_close
        ld a,WHITE*16+RED
        call put_cell
        ; seek track
        ld de,#0401
        ld b,30
.t:     push bc
        ld ix,pat_track
        ld a,GREY*16+NAVY
        call put_cell
        inc e
        pop bc
        djnz .t
        ld de,#0519
        ld hl,txt_level
        ld b,5
        ld c,MAGENTA*16+NAVY
        call put_str
        ld de,#101A
        ld a,'L'
        ld c,CYAN*16+NAVY
        call put_char
        ld de,#101D
        ld a,'R'
        call put_char
        ; separator
        ld de,#1100
        ld b,32
.s:     push bc
        ld ix,pat_hline
        ld a,BLUE*16+NAVY
        call put_cell
        inc e
        pop bc
        djnz .s
        ; list page arrows
        ld de,#131F
        ld ix,pat_up
        ld a,GREY*16+NAVY
        call put_cell
        ld de,#161F
        ld ix,pat_down
        ld a,GREY*16+NAVY
        call put_cell
        ; control row
        ld de,#1700
        ld b,32
        ld c,A_BAR
        call fill_row
        ld de,#1701
        ld hl,icons
        ld b,7
.ic:    push bc
        push hl
        push hl
        pop ix
        ld a,WHITE*16+BLUE_D
        call put_cell
        pop hl
        ld bc,8
        add hl,bc
        inc e
        pop bc
        djnz .ic
        ld de,#1709
        ld hl,txt_keys
        ld b,11
        ld c,A_BAR
        call put_str
        ld de,#1714
        ld hl,txt_vol
        ld b,3
        ld c,A_BAR
        jp put_str

; freq_label: A = bar, writes "86", "1K", "12K" ... under it (row 16)
freq_label:
        ld (lab_bar),a
        ld l,a
        ld h,0
        add hl,hl
        ld de,W_VIZ+V_FREQ
        add hl,de
        call rd
        ld c,a
        inc hl
        call rd
        ld h,a
        ld l,c
        ld de,numbuf
        ld bc,1000
        or a
        sbc hl,bc
        jr nc,.k
        add hl,bc
        call fmt_u16
        jr .put
.k:     add hl,bc
        ; kHz, rounded
        ld bc,500
        add hl,bc
        ld de,1000
        call div16              ; HL = HL / DE
        ld de,numbuf
        call fmt_u16
        ld a,'K'
        ld (de),a
        inc de
        xor a
        ld (de),a
.put:   ld a,(lab_bar)
        inc a
        ld e,a
        ld d,16
        ld hl,numbuf
        ld b,3
        ld c,CYAN*16+NAVY
        jp put_str

;------------------------------------------------------------------------------
; the picture area (rows 6-15, columns 1-24): spectrum bars, the oscilloscope
; or the equalizer. The VU meters (columns 26 and 29) are always drawn.

draw_view:
        ld a,(viz_new)
        or a
        ret z
        xor a
        ld (viz_new),a
        ld a,(eq_open)
        or a
        jp nz,draw_vu
        ld a,(view)
        or a
        jp z,draw_bars
        jp draw_scope

; V / a click on the label: spectrum <-> oscilloscope
toggle_view:
        xor a
        ld (eq_open),a
        ld a,(view)
        xor 1
        ld (view),a
        ; fall through
; draw what the area shows now (the equalizer closed)
show_view:
        call draw_area_labels
        ld a,(view)
        or a
        jr nz,.scope
        ld hl,bar_raw           ; every bar cell drawn again
        ld bc,NBARS*2
        ld a,#FF
        call memset
        ld hl,bar_last
        ld bc,NBARS*2
        ld a,#FF
        call memset
        ld hl,cellcache
        ld bc,NBARS*10
        ld a,#FF
        jp memset
.scope: call vb_clear
        ld hl,scope_old
        ld bc,64
        ld a,40
        call memset
        ld hl,scope_colrow
        ld (colfn),hl
        call area_out_col
        jp area_out_pat

; row 5: what the area is and the equalizer preset; row 16: its labels
draw_area_labels:
        ld hl,txt_spectrum
        ld a,(view)
        or a
        jr z,.l
        ld hl,txt_scope
.l:     ld a,(eq_open)
        or a
        jr z,.l2
        ld hl,txt_equalizer
.l2:    ld de,#0501
        ld b,9
        ld c,MAGENTA*16+NAVY
        call put_str
        ld a,#FF
        ld (eq_shown),a
        call draw_eq_name
        ld de,#1001
        ld b,24
        ld c,CYAN*16+NAVY
        call fill_row
        ld a,(eq_open)
        or a
        jp nz,draw_eq_info
        ld a,(view)
        or a
        ret nz
        ; frequency labels under bars 0, 6, 12, 17, 21 (from the engine's table)
        ld hl,label_bars
.lab:   ld a,(hl)
        cp #FF
        ret z
        inc hl
        push hl
        call freq_label
        pop hl
        jr .lab

; spectrum and VU: 24 + 2 bars of 10 cells, 2 LED segments a cell (20 levels)
draw_bars:
        ld b,0
        jr bars_from
draw_vu:
        ld b,NBARS
bars_from:
        ld a,(viz+V_SEQ)
        bit 0,a
        ret nz                  ; caught while the engine wrote it: next frame
.b:     ld a,b
        cp NBARS
        jr nc,.vu
        ld e,b
        ld d,0
        ld hl,viz+V_LEVEL
        add hl,de
        ld c,(hl)
        ld hl,viz+V_PEAK
        add hl,de
        ld a,(hl)
        ld (pk_level),a
        ld a,b
        inc a                   ; column 1..24
        ld (bar_col),a
        jr .draw
.vu:    ld a,b
        sub NBARS
        ld hl,viz+V_VU
        call idx
        ld c,(hl)
        ld a,c
        ld (pk_level),a         ; no peak dot of its own
        ld a,b
        sub NBARS
        ld e,26
        jr z,.l
        ld e,29
.l:     ld a,e
        ld (bar_col),a
.draw:  push bc                 ; the same level and peak as drawn: skip it
        ld a,b
        ld (bar_no),a
        add a,a
        ld e,a
        ld d,0
        ld hl,bar_raw
        add hl,de
        ld a,c
        cp (hl)
        jr nz,.new
        inc hl
        ld a,(pk_level)
        cp (hl)
        jr z,.skip
        dec hl
.new:   ld (hl),c
        inc hl
        ld a,(pk_level)
        ld (hl),a
        call draw_bar
.skip:  pop bc
        inc b
        ld a,b
        cp NBARS+2
        jr c,.b
        ret

idx:    ld e,a                  ; HL += A
        ld d,0
        add hl,de
        ret

; draw_bar: C = level 0-31, (pk_level), (bar_col), (bar_no)
draw_bar:
        ld hl,seg_of
        ld a,c
        call idx
        ld a,(hl)
        ld (lit),a
        ld a,(pk_level)
        or a
        ld a,#FF
        jr z,.pk                ; nothing: no dot
        ld hl,seg_of
        ld a,(pk_level)
        call idx
        ld a,(hl)
        ld hl,lit
        cp (hl)
        jr nc,.pk
        ld a,#FF                ; under the bar: no dot
.pk:    cp #FF
        jr z,.pk2
        cp 20
        jr c,.pk2
        ld a,19
.pk2:   ld (pk),a
        ; the same as last time: nothing to do
        ld a,(bar_no)
        add a,a
        ld e,a
        ld d,0
        ld hl,bar_last
        add hl,de
        ld a,(lit)
        cp (hl)
        jr nz,.chg
        inc hl
        ld a,(pk)
        cp (hl)
        ret z
        dec hl
.chg:   ; only the cells between the old and the new top, and the peaks
        ld b,(hl)               ; old lit
        inc hl
        ld c,(hl)               ; old peak
        ld a,(pk)
        ld (hl),a
        dec hl
        ld a,(lit)
        ld (hl),a
        ld a,b
        cp #FF
        jr nz,.part
        ld de,#0900             ; unknown: all of it (D = 9, E = 0)
        jr .range
.part:  ld e,a                  ; E = lowest segment, D = highest
        ld d,a
        ld a,(lit)
        call .mm
        ld a,c
        call .mmp
        ld a,(pk)
        call .mmp
        ld a,e                  ; cells: segment / 2 (a changed top is the
        srl a                   ; segment just under it)
        jr z,.lo0
        ld a,e
        dec a
        srl a
.lo0:   ld e,a
        ld a,d
        srl a
        cp 10
        jr c,.hi
        ld a,9
.hi:    ld d,a
.range: ld a,e
        ld (cell_hi),a          ; (used as the first cell)
        ld a,d
        ld (cell_top),a
        ; the cache of this bar's cells
        ld a,(bar_no)
        ld l,a
        ld h,0
        add hl,hl
        ld e,l
        ld d,h
        add hl,hl
        add hl,hl
        add hl,de               ; *10
        ld de,cellcache
        add hl,de
        ld (cache_p),hl
        ld a,(cell_hi)
        ld b,a                  ; cell 0 = bottom
        jr .c
.mmp:   cp #FF                  ; a peak (FF: none)
        ret z
.mm:    cp e                    ; E = min(E, A), D = max(D, A)
        jr nc,.m1
        ld e,a
.m1:    cp d
        ret c
        ld d,a
        ret
.c:     ; code: bit0 lower lit, bit1 upper lit, bit2 lower peak, bit3 upper peak
        ld a,b
        add a,a
        ld e,a                  ; lower segment number
        ld c,0
        ld a,(lit)
        cp e
        jr c,.n1
        jr z,.n1
        set 0,c
.n1:    inc e
        ld a,(lit)
        cp e
        jr c,.n2
        jr z,.n2
        set 1,c
.n2:    dec e
        ld a,(pk)
        cp e
        jr nz,.n3
        set 2,c
.n3:    inc e
        cp e
        jr nz,.n4
        set 3,c
.n4:    ld hl,(cache_p)
        ld e,b
        ld d,0
        add hl,de
        ld a,(hl)
        cp c
        jr z,.next
        ld (hl),c
        push bc
        call bar_cell
        pop bc
.next:  ld a,(cell_top)
        cp b
        ret z
        inc b
        jr .c

; bar_cell: C = code, B = cell (0 bottom)
bar_cell:
        ; patterns: rows 0-2 upper, 4-6 lower
        ld a,c
        and #0A                 ; upper lit or peak
        ld a,0
        jr z,.u
        ld a,#7E
.u:     ld (cellpat),a
        ld (cellpat+1),a
        ld (cellpat+2),a
        ld a,c
        and #05
        ld a,0
        jr z,.lo
        ld a,#7E
.lo:    ld (cellpat+4),a
        ld (cellpat+5),a
        ld (cellpat+6),a
        xor a
        ld (cellpat+3),a
        ld (cellpat+7),a
        ; colours: the segment's colour, white for a peak
        ld a,b
        add a,a
        inc a                   ; upper segment
        ld hl,seg_col
        call idx
        ld a,(hl)
        bit 3,c
        jr z,.uc
        ld a,WHITE*16+NAVY
.uc:    ld (cellclr),a
        ld (cellclr+1),a
        ld (cellclr+2),a
        ld (cellclr+3),a
        ld a,b
        add a,a
        ld hl,seg_col
        call idx
        ld a,(hl)
        bit 2,c
        jr z,.lc
        ld a,WHITE*16+NAVY
.lc:    ld (cellclr+4),a
        ld (cellclr+5),a
        ld (cellclr+6),a
        ld (cellclr+7),a
        ld a,15
        sub b
        ld d,a                  ; row 15 - cell
        ld a,(bar_col)
        ld e,a
        ld ix,cellpat
        ld iy,cellclr
        jp put_cell2

;------------------------------------------------------------------------------
; the bitmap of the area: 'vizbuf', 10 rows of 24 cells x 8 bytes, the same
; order as the pattern table, so each row goes out in one run.

vb_clear:
        ld hl,vizbuf
        ld bc,1920
        xor a
        jp memset

; patterns of the area from vizbuf
area_out_pat:
        ld d,6
        ld e,16
; area_out_rows: screen rows D up to E (not included)
area_out_rows:
        push de
        ld a,d
        sub 6
        ld l,a
        ld h,0
        ld de,192
        call mul16
        ld de,vizbuf
        add hl,de
        pop de
.r:     push de
        push hl
        ld h,d
        ld l,8
        call vwrite
        pop hl
        ld b,192
        ld c,VDP_D
        otir
        ei
        pop de
        inc d
        ld a,d
        cp e
        jr c,.r
        ret

; colours of the area, a row at a time from the routine at (colfn): A = row
; 0-9, fills rowcol (24 cells x 8 bytes)
area_out_col:
        ld a,#FF
        ld (cr_last),a
        ld d,6
.r:     push de
        ld a,d
        sub 6
        ld hl,(colfn)
        call jp_hl
        pop de
        push de
        ld a,d
        add a,#20
        ld h,a
        ld l,8
        call vwrite
        ld hl,rowcol
        ld b,192
        ld c,VDP_D
        otir
        ei
        pop de
        inc d
        ld a,d
        cp 16
        jr c,.r
        ret

jp_hl:  jp (hl)

; vb_ptr: A = y (0-79), C = cell (0-23) -> HL = its byte in vizbuf
vb_ptr: push de
        ld l,a
        ld h,0
        add hl,hl
        ld de,row_off
        add hl,de
        ld e,(hl)
        inc hl
        ld d,(hl)
        ld hl,vizbuf
        add hl,de
        ld a,c
        add a,a
        add a,a
        add a,a
        ld e,a
        ld d,0
        add hl,de
        pop de
        ret

; vb_text: HL = text, C = first cell, B = y of its top line
vb_text:
.ch:    ld a,(hl)
        or a
        ret z
        push hl
        sub 32
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        ld de,font
        add hl,de
        ld e,0
.ln:    push hl
        ld a,b
        add a,e
        call vb_ptr
        ex (sp),hl
        ld a,(hl)
        ex (sp),hl
        or (hl)
        ld (hl),a
        pop hl
        inc hl
        inc e
        ld a,e
        cp 8
        jr c,.ln
        pop hl
        inc hl
        inc c
        jr .ch

; vb_hline: B = y, C = first cell, D = cells, E = byte ORed in
vb_hline:
.l:     ld a,b
        call vb_ptr
        ld a,(hl)
        or e
        ld (hl),a
        inc c
        dec d
        jr nz,.l
        ret

; vb_sprite8: E = x, B = y, D = lines, IX = an 8-pixel-wide pattern
vb_sprite8:
.ln:    push de
        push bc
        ld a,(ix+0)
        inc ix
        ld h,a
        ld l,0
        ld a,e
        and 7
        jr z,.ns
.sh:    srl h
        rr l
        dec a
        jr nz,.sh
.ns:    push hl
        ld a,e
        rrca
        rrca
        rrca
        and #1F
        ld c,a
        ld a,b
        call vb_ptr
        pop de
        ld a,(hl)
        or d
        ld (hl),a
        ld a,e
        or a
        jr z,.n2
        ld bc,8
        add hl,bc
        ld a,(hl)
        or e
        ld (hl),a
.n2:    pop bc
        pop de
        inc b
        dec d
        jr nz,.ln
        ret

;------------------------------------------------------------------------------
; the oscilloscope: the engine's 64 newest samples (every 4th one), 3 pixels
; each, joined by vertical lines. The old trace is taken off (AND), the new
; one put on (OR), and the area sent.

scope_colrow:                   ; A = row: the colours by line (scope_col)
        add a,a
        add a,a
        add a,a
        ld e,a
        ld d,0
        ld hl,scope_col
        add hl,de
        ld de,rowcol
        ld b,24
.c:     push bc
        push hl
        ld bc,8
        ldir
        pop hl
        pop bc
        djnz .c
        ret

draw_scope:
        ld a,(viz+V_SEQ)
        bit 0,a
        ret nz
        ld hl,scope_tick        ; every other update (25 a second): the trace
        inc (hl)                ; is the busiest drawing, and the MSX's RAM is
        bit 0,(hl)              ; the cartridge's, shared with the decoder
        jp nz,draw_vu
        ld a,#A1                ; and c
        ld (vspan.op),a
        ld a,#FF
        ld (plot_inv),a
        ld hl,scope_old
        call scope_plot
        ld ix,viz+V_SCOPE
        ld hl,scope_old
        ld b,64
.y:     ld a,(ix+0)
        inc ix
        push hl
        ld e,a
        ld d,0
        ld hl,scope_y
        add hl,de
        ld a,(hl)
        pop hl
        ld (hl),a
        inc hl
        djnz .y
        ld a,#B1                ; or c
        ld (vspan.op),a
        xor a
        ld (plot_inv),a
        ld hl,scope_old
        call scope_plot
        call area_out_pat
        jp draw_vu

; scope_plot: HL = 64 y values
scope_plot:
        ld (sp_ys),hl
        ld a,(hl)
        ld (sp_prev),a
        xor a
        ld (sp_x),a
        ld b,64
.s:     push bc
        ld hl,(sp_ys)
        ld a,(hl)
        inc hl
        ld (sp_ys),hl
        ld (sp_cur),a
        ld c,a
        ld a,(sp_prev)
        cp c
        jr c,.ok                ; A = top, C = bottom
        ld e,c
        ld c,a
        ld a,e
.ok:    ld e,a
        ld a,c
        sub e
        inc a
        ld b,a
        ld a,(sp_x)
        ld d,a
        call vspan
        ld a,(sp_x)
        inc a
        ld d,a
        ld a,(sp_cur)
        ld e,a
        ld b,2                  ; the next two pixels: x+1 and x+2 share a
        call hspan2             ; byte unless x+1 ends one
        ld a,(sp_cur)
        ld (sp_prev),a
        ld a,(sp_x)
        add a,3
        ld (sp_x),a
        pop bc
        djnz .s
        ret

hspan2: push de                 ; D = x, E = y: pixels (x, y) and (x+1, y)
        ld b,1
        call vspan
        pop de
        inc d
        ld b,1
        ; fall through

; vspan: D = x, E = top y, B = pixels down
vspan:  ld a,d
        and 7
        ld c,a
        push bc
        ld b,0
        ld hl,bitmask
        add hl,bc
        ld a,(plot_inv)
        xor (hl)
        ld c,a                  ; C = the pixel's bit (or all but it)
        ld a,d
        and #F8
        ld (vs_x),a
        ld l,e
        ld h,0
        add hl,hl
        push de
        ld de,row_off
        add hl,de
        ld e,(hl)
        inc hl
        ld d,(hl)
        ld hl,vizbuf
        add hl,de
        ld a,(vs_x)
        ld e,a
        ld d,0
        add hl,de
        pop de
        ld a,e
        and 7
        ld d,a                  ; D = line in the cell
        pop af                  ; A = count (B)
        ld b,a
.l:     ld a,(hl)
.op:    or c
        ld (hl),a
        dec b
        ret z
        inc d
        bit 3,d
        jr nz,.nr
        inc hl
        jr .l
.nr:    ld d,0
        ld a,l
        add a,185               ; to line 0 of the cell below: + 192 - 7
        ld l,a
        jr nc,.l
        inc h
        jr .l

;------------------------------------------------------------------------------
; the equalizer screen: five sliders (-12..+12 dB) and the balance

eq_open_screen:
        ld a,1
        ld (eq_open),a
        ld hl,st+ST_EQ
        ld de,eqg
        ld bc,5
        ldir
        ld a,(st+ST_BAL)
        ld (eqbal),a
        call draw_area_labels
        call eq_build_cols
        ld hl,eq_colrow
        ld (colfn),hl
        call area_out_col
        jp eq_render

eq_close:
        xor a
        ld (eq_open),a
        ld a,#FF
        ld (eq_drag),a
        jp show_view

eq_redraw:
        call eq_render
        jp draw_eq_info

; the bitmap: scale, 0 dB line, tracks, knobs, band names, balance
eq_render:
        call vb_clear
        ld hl,txt_p12
        ld c,0
        ld b,3
        call vb_text
        ld hl,txt_z
        ld c,1
        ld b,27
        call vb_text
        ld hl,txt_m12
        ld c,0
        ld b,51
        call vb_text
        ld b,30
        ld c,3
        ld d,20
        ld e,#88
        call vb_hline
        ld hl,txt_bal
        ld c,0
        ld b,70
        call vb_text
        ld b,73
        ld c,3
        ld d,20
        ld e,#FF
        call vb_hline
        ld b,74
        ld c,3
        ld d,20
        ld e,#FF
        call vb_hline
        xor a
.band:  push af
        call eq_draw_band
        pop af
        inc a
        cp 5
        jr c,.band
        call eq_draw_balknob
        jp area_out_pat

; eq_draw_band: A = band: its track, knob and name into vizbuf
eq_draw_band:
        ld (er_b),a
        add a,a
        add a,a
        add a,5
        ld (er_cc),a            ; the band's cell: 5, 9, 13, 17, 21
        ld b,2
.tr:    ld a,(er_cc)
        ld c,a
        ld a,b
        call vb_ptr
        ld a,(hl)
        or #18
        ld (hl),a
        inc b
        ld a,b
        cp 59
        jr c,.tr
        ; the knob: top line 27 - 2 * gain (centre 6 at +12, 54 at -12)
        ld a,(er_b)
        ld e,a
        ld d,0
        ld hl,eqg
        add hl,de
        ld a,(hl)
        add a,a
        ld c,a
        ld a,27
        sub c
        ld b,a
        ld a,(er_cc)
        add a,a
        add a,a
        add a,a
        ld e,a
        ld d,7
        ld ix,knob_v
        call vb_sprite8
        ld a,(er_b)
        add a,a
        add a,a
        ld e,a
        ld d,0
        ld hl,eq_lab
        add hl,de
        ld a,(er_cc)
        dec a
        ld c,a
        ld b,60
        jp vb_text

; the balance knob: x = 28 + balance * 148 / 100
eq_draw_balknob:
        ld a,(eqbal)
        ld l,a
        ld h,0
        ld de,148
        call mul16
        ld de,100
        call div16
        ld a,l
        add a,28
        ld e,a
        ld b,70
        ld d,8
        ld ix,knob_h
        jp vb_sprite8

; eq_band_redraw: A = band: only its column again
eq_band_redraw:
        push af
        add a,a
        add a,a
        add a,5
        ld c,a
        ld b,0
.cl:    ld a,b
        call vb_ptr
        ld (hl),0
        inc b
        ld a,b
        cp 80
        jr c,.cl
        ld d,1
        ld e,#88
        ld b,30
        push bc
        call vb_hline
        pop bc
        ld d,1
        ld e,#FF
        ld b,73
        push bc
        call vb_hline
        pop bc
        ld d,1
        ld e,#FF
        ld b,74
        push bc
        call vb_hline
        pop bc
        pop af
        call eq_draw_band
        call eq_draw_balknob
        ; out: the column's 10 cells
        ld a,(er_cc)
        add a,a
        add a,a
        add a,a
        ld e,a
        ld d,0
        ld hl,vizbuf
        add hl,de
        ld a,(er_cc)
        inc a
        ld e,a
        ld d,6
.o:     push de
        push hl
        call cell_addr
        call vwrite
        pop hl
        push hl
        ld b,8
        ld c,VDP_D
        otir
        ei
        pop hl
        ld bc,192
        add hl,bc
        pop de
        inc d
        ld a,d
        cp 16
        jr c,.o
        ret

; eq_bal_redraw: only the balance lines (70-77) again
eq_bal_redraw:
        ld b,70
.l:     ld c,3
.c:     ld a,b
        call vb_ptr
        ld (hl),0
        inc c
        ld a,c
        cp 23
        jr c,.c
        inc b
        ld a,b
        cp 78
        jr c,.l
        ld b,73
        ld c,3
        ld d,20
        ld e,#FF
        call vb_hline
        ld b,74
        ld c,3
        ld d,20
        ld e,#FF
        call vb_hline
        call eq_draw_balknob
        ld d,14
        ld e,16
        jp area_out_rows

; the colours of the three parts (sliders, names, balance) by cell
eq_build_cols:
        ld hl,eqcols
        ld ix,eq_party
        ld b,3
.p:     push bc
        ld c,0
.c:     push bc
        push hl
        ld a,(ix+0)
        call eq_colour
        pop hl
        ld (hl),a
        inc hl
        pop bc
        inc c
        ld a,c
        cp 24
        jr c,.c
        inc ix
        pop bc
        djnz .p
        ret

; eq_colrow: A = row: rowcol from eqcols; rows 0-6 are all alike
eq_colrow:
        cp 7
        jr nc,.go
        ld hl,cr_last
        cp (hl)                 ; FF after a change: built once for rows 0-6
        ret z
        ld a,0
        ld (hl),a
.go:    add a,a
        add a,a
        add a,a
        ld (cr_y0),a
        ld de,rowcol
        ld b,0                  ; line
.l:     ld a,(cr_y0)
        add a,b
        ld hl,eqcols
        cp 58
        jr c,.r
        ld hl,eqcols+24
        cp 66
        jr c,.r
        ld hl,eqcols+48
.r:     push de
        push bc
        ld b,24
.c:     ld a,(hl)
        ld (de),a
        inc hl
        ld a,e
        add a,8
        ld e,a
        jr nc,.n
        inc d
.n:     djnz .c
        pop bc
        pop de
        inc de
        inc b
        ld a,b
        cp 8
        jr c,.l
        ret

; eq_colour: C = cell, A = y -> A = colour byte
eq_colour:
        ld b,a
        cp 66
        jr c,.top
        ld a,c                  ; the balance part
        cp 3
        ld a,CYAN
        jr c,.out
        ld a,(eq_sel)
        cp 5
        ld a,YELLOW
        jr z,.out
        ld a,WHITE
        jr .out
.top:   ld a,c
        cp 3
        ld a,CYAN
        jr c,.out               ; the scale
        ld hl,band_cell
        ld a,c
        call idx
        ld a,(hl)
        cp #FF
        ld a,GREY
        jr z,.out               ; between the bands: the 0 dB dots
        ld d,(hl)
        ld a,(eq_sel)
        cp d
        ld a,YELLOW
        jr z,.out
        ld a,b
        cp 58
        ld a,CYAN
        jr nc,.out              ; the band names
        ld a,d
        add a,a
        add a,a
        add a,5
        cp c
        ld a,WHITE
        jr z,.out               ; the slider
        ld a,GREY
.out:   add a,a
        add a,a
        add a,a
        add a,a
        add a,NAVY
        ret

; row 16: the chosen band and its gain, or the balance
draw_eq_info:
        ld de,linebuf
        ld a,(eq_sel)
        cp 5
        jr z,.bal
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        ld bc,eq_hz
        add hl,bc
        call strcpy
        ld a,' '
        ld (de),a
        inc de
        ld a,(eq_sel)
        ld c,a
        ld b,0
        ld hl,eqg
        add hl,bc
        ld a,(hl)
        or a
        jr z,.num
        ld c,'+'
        bit 7,a
        jr z,.pos
        neg
        ld c,'-'
.pos:   push af
        ld a,c
        ld (de),a
        inc de
        pop af
.num:   ld l,a
        ld h,0
        call fmt_u16
        ld hl,txt_db
        call strcpy
        jr .put
.bal:   ld hl,txt_balance
        call strcpy
        ld a,(eqbal)
        cp 50
        jr nz,.bn
        ld hl,txt_centre
        call strcpy
        jr .put
.bn:    ld l,a
        ld h,0
        call fmt_u16
.put:   ld de,#1001
        ld hl,linebuf
        ld b,16
        ld c,WHITE*16+NAVY
        call put_str
        ld de,#1011
        ld hl,txt_gclose
        ld b,8
        ld c,GREY*16+NAVY
        jp put_str

eq_select:                      ; A = band 0-4, 5 = balance
        ld hl,eq_sel
        cp (hl)
        ret z
        ld (hl),a
        call eq_build_cols
        ld hl,eq_colrow
        ld (colfn),hl
        call area_out_col
        jp draw_eq_info

eq_set_gain:                    ; A = band, C = gain
        ld (er_band),a
        ld e,a
        ld d,0
        ld hl,eqg
        add hl,de
        ld a,(hl)
        cp c
        ret z
        ld (hl),c
        call eq_send
        ld a,(er_band)
        call eq_band_redraw
        jp draw_eq_info

eq_send:
        ld hl,eqg
        ld de,args
        ld bc,5
        ldir
        ld a,C_EQ
        ld bc,5
        call cmd
        ld hl,eqg
        call find_eq
        jp draw_eq_name

eq_set_bal:                     ; C = 0..100
        ld a,(eqbal)
        cp c
        ret z
        ld a,c
        ld (eqbal),a
        ld e,a
        ld a,C_BAL
        call cmd_u8
        call eq_bal_redraw
        jp draw_eq_info

; Up / Down: C = +1 or -1
eq_step:
        ld a,(eq_sel)
        cp 5
        jr z,.bal
        ld e,a
        ld d,0
        ld hl,eqg
        add hl,de
        ld a,(hl)
        bit 7,c
        jr nz,.dn
        cp 12
        ret z
        inc a
        jr .set
.dn:    cp -12
        ret z
        dec a
.set:   ld c,a
        ld a,(eq_sel)
        jp eq_set_gain
.bal:   ld a,(eqbal)
        bit 7,c
        jr nz,.bd
        add a,5
        cp 101
        jr c,.bs
        ld a,100
        jr .bs
.bd:    sub 5
        jr nc,.bs
        xor a
.bs:    ld c,a
        jp eq_set_bal

eq_reset:                       ; 0: the chosen one back to the middle
        ld a,(eq_sel)
        cp 5
        ld c,50
        jp z,eq_set_bal
        ld c,0
        jp eq_set_gain

; E: the next equalizer preset (the same ones as PicoPlay)
next_eq:
        ld a,(eq_preset)
        inc a
        cp 8
        jr c,.ok
        xor a
.ok:    ld l,a
        ld h,0
        ld e,l
        ld d,h
        add hl,hl
        add hl,hl
        add hl,de               ; *5
        ld de,eq_tab
        add hl,de
        ld de,eqg
        ld bc,5
        ldir
        call eq_send
        ld a,(eq_open)
        or a
        ret z
        jp eq_redraw

; find_eq: HL = 5 gains -> eq_preset (8 = its own setting, from PicoPlay)
find_eq:
        ld (fe_p),hl
        ld b,0
        ld hl,eq_tab
.p:     push bc
        push hl
        ld de,(fe_p)
        ld b,5
.c:     ld a,(de)
        cp (hl)
        jr nz,.no
        inc hl
        inc de
        djnz .c
        pop hl
        pop bc
        ld a,b
        ld (eq_preset),a
        ret
.no:    pop hl
        ld bc,5
        add hl,bc
        pop bc
        inc b
        ld a,b
        cp 8
        jr c,.p
        ld (eq_preset),a
        ret

draw_eq_name:                   ; "EQ ROCK" between the area's and the LEVEL labels
        ld a,(eq_preset)
        ld hl,eq_shown
        cp (hl)
        ret z
        ld (hl),a
        add a,a                 ; *10
        ld l,a
        add a,a
        add a,a
        add a,l
        ld l,a
        ld h,0
        ld de,eq_names
        add hl,de
        ld de,#050B
        ld b,9
        ld c,GREY*16+NAVY
        ld a,(eq_open)
        or a
        jr z,.p
        ld c,YELLOW*16+NAVY
.p:     jp put_str

;------------------------------------------------------------------------------
; the status: time, title, artist, format, seek bar, state, volume
draw_info:
        ; time m:ss (elapsed)
        ld hl,(st+ST_ELAPS)
        ld de,60
        call div16              ; HL = minutes, DE = seconds
        push de
        ld a,l
        cp 100
        jr c,.m
        ld a,99
.m:     call two_digits         ; D = tens, E = units
        push de
        ld a,d
        ld de,#0101
        call put_digit
        pop de
        ld a,e
        ld de,#0103
        call put_digit
        ld de,#0105
        call put_colon
        pop de
        ld a,e
        call two_digits
        push de
        ld a,d
        ld de,#0106
        call put_digit
        pop de
        ld a,e
        ld de,#0108
        call put_digit
        ; title / artist (they scroll when long) / format
        ld hl,st+ST_ARTIST
        ld a,(hl)
        or a
        jr nz,.a
        ld hl,st+ST_ALBUM       ; a stream: the station
.a:     ld de,#020B
        ld b,21
        ld c,CYAN*16+NAVY
        ld ix,tk_artist
        call ticker_text          ; (the artist first: a new title then has the turn)
        ld hl,st+ST_TITLE
        ld a,(hl)
        or a
        jr nz,.t
        ld hl,txt_notrack
.t:     ld de,#010B
        ld b,21
        ld c,YELLOW*16+NAVY
        ld ix,tk_title
        call ticker_text
        call fmt_format
        ld de,#030B
        ld hl,linebuf
        ld b,21
        ld c,GREEN*16+NAVY
        call put_str
        call draw_seek
        call draw_state
        call draw_modes
        ld hl,st+ST_EQ
        call find_eq
        call draw_eq_name
        jp draw_volume

draw_modes:
        ld a,(st+ST_MODE)
        ld hl,mode_shown
        cp (hl)
        ret z
        ld (hl),a
        ld c,GREY*16+BLUE_D
        bit 1,a
        jr z,.s
        ld c,GREEN*16+BLUE_D
.s:     ld de,#1709
        ld hl,txt_shf
        ld b,3
        call put_str
        ld a,(st+ST_MODE)
        ld c,GREY*16+BLUE_D
        bit 0,a
        jr z,.r
        ld c,GREEN*16+BLUE_D
.r:     ld de,#170D
        ld hl,txt_rep
        ld b,3
        jp put_str

; "128K 44.1KHZ ST" or "BUFFERING 40%"
fmt_format:
        ld de,linebuf
        ld a,(st+ST_FLAGS)
        and F_BUFFER
        jr z,.f
        ld hl,txt_buffering
        call strcpy
        ld a,(st+ST_BUFP)
        ld l,a
        ld h,0
        call fmt_u16
        ld a,'%'
        ld (de),a
        inc de
        xor a
        ld (de),a
        ret
.f:     ld hl,(st+ST_BRATE)
        ld a,h
        or l
        jr nz,.r
        xor a
        ld (de),a
        ret
.r:     call fmt_u16
        ld hl,txt_kbps
        call strcpy
        ld hl,(st+ST_RATE)      ; Hz / 10
        push de
        ld de,100
        call div16              ; HL = kHz, DE = rest (0..99)
        ld a,e
        pop de
        push af
        call fmt_u16
        ld a,'.'
        ld (de),a
        inc de
        pop af
        ld b,'0'-1
.t:     inc b
        sub 10
        jr nc,.t
        ld a,b
        ld (de),a
        inc de
        ld hl,txt_khz
        call strcpy
        ld a,(st+ST_CHAN)
        cp 1
        ld hl,txt_mono
        jr z,.ch
        ld hl,txt_stereo
.ch:    jp strcpy

draw_seek:
        ; knob position 0..29 = elapsed * 29 / total (streams: the buffer fill)
        ld a,(st+ST_FLAGS)
        and F_STREAM
        jr z,.file
        ld a,(st+ST_BUFP)
        ld l,a
        ld h,0
        ld de,29
        call mul16
        ld de,100
        call div16
        jr .pos
.file:  ld hl,(st+ST_TOTAL)
        ld a,h
        or l
        jr z,.zero
        ld hl,(st+ST_ELAPS)
        ld de,29
        call mul16
        ld de,(st+ST_TOTAL)
        call div16
        jr .pos
.zero:  ld hl,0
.pos:   ld a,l
        cp 30
        jr c,.ok
        ld a,29
.ok:    ld hl,knob_pos
        cp (hl)
        ret z
        push af
        ld a,(hl)
        inc a
        ld e,a
        ld d,4
        ld ix,pat_track
        ld a,GREY*16+NAVY
        call put_cell
        pop af
        ld (knob_pos),a
        inc a
        ld e,a
        ld d,4
        ld ix,pat_knob
        ld a,WHITE*16+NAVY
        jp put_cell

draw_state:
        ld de,linebuf
        ld a,(loading)
        or a
        jr z,.s
        ld hl,txt_loading
        call strcpy
        jr .put
.s:     ld a,(st+ST_STATE)
        ld hl,txt_stopped
        or a
        jr z,.c
        ld hl,txt_paused
        cp 2
        jr z,.c
        ld hl,txt_playing
.c:     call strcpy
        ld a,(st+ST_STATE)
        cp 1
        jr nz,.put
        ld a,' '
        ld (de),a
        inc de
        ld hl,(st+ST_PLPOS)
        inc hl
        call fmt_u16
        ld a,'/'
        ld (de),a
        inc de
        ld hl,(st+ST_PLCNT)
        call fmt_u16
.put:   ld de,#000B
        ld hl,linebuf
        ld b,20
        ld c,A_TITLE
        jp put_str

draw_volume:
        ld a,(st+ST_VOL)
        ld hl,vol_shown
        cp (hl)
        ret z
        ld (hl),a
        ld c,a
        ld b,0
.l:     ld a,b                  ; cell b is on when the volume is above 12 * b
        add a,a
        add a,a
        ld e,a
        add a,a
        add a,e
        cp c
        jr nc,.off
        ld ix,pat_vol_on
        ld a,GREEN*16+BLUE_D
        jr .put
.off:   ld ix,pat_vol_off
        ld a,GREY*16+BLUE_D
.put:   push bc
        push af
        ld a,b
        add a,24
        ld e,a
        ld d,#17
        pop af
        call put_cell
        pop bc
        inc b
        ld a,b
        cp 8
        jr c,.l
        ret

;------------------------------------------------------------------------------
; the folder list: 4 rows of the open folder, the cursor 'sel', from 'top'
open_path:                      ; HL = path
        ld de,args
        call strcpy0
        ex de,hl
        ld de,args
        or a
        sbc hl,de
        ld b,h
        ld c,l
        ld a,C_DOPEN
        call cmd
        jr c,.no
        jr nz,.no
        jp got_folder
.no:    ld hl,txt_nofolder
        jp show_path_msg

got_folder:                     ; reply of DIR_OPEN / DIR_ENTER: u16 count, path
        ld hl,(reply)
        ld (count),hl
        ld hl,reply+2
        ld de,path
        call strcpy0
        ld hl,0
        ld (sel),hl
        ld (top),hl
        call draw_path
        jp draw_list

draw_path:
        ld de,#1201
        ld hl,path
        ld b,24
        ld c,YELLOW*16+NAVY
        call put_str
        ld de,linebuf
        ld hl,(count)
        call fmt_u16
        ld de,#1219
        ld hl,linebuf
        ld b,6
        ld c,GREY*16+NAVY
        jp put_str

show_path_msg:
        ld de,#1201
        ld b,30
        ld c,RED*16+NAVY
        jp put_str

; read the visible entries (DIR_READ top, 4) and draw them; the chosen one
; becomes a ticker when its name is longer than the row
draw_list:
        xor a
        ld (tk_list+T_ACT),a
        dec a
        ld (tk_list+T_ROW),a
        ld hl,(top)
        ld (args),hl
        ld a,LIST_N
        ld (args+2),a
        ld bc,3
        ld a,C_DREAD
        call cmd
        ld a,0
        jr c,.none
        jr nz,.none
        ld a,1
.none:  ld (have_list),a
        ld hl,reply
        ld (ent_p),hl
        xor a
        ld (row_no),a
.r:     ld a,(row_no)
        add a,LIST_ROW
        ld d,a
        ld e,1
        ld (row_cell),de
        ld hl,(top)
        ld a,(row_no)
        ld e,a
        ld d,0
        add hl,de
        ld (row_idx),hl
        ld de,(count)
        or a
        sbc hl,de
        jp nc,.blank
        ld a,(have_list)
        or a
        jp z,.blank
        ; entry: type, size (4), len, name
        ld hl,(ent_p)
        ld a,(hl)
        ld (ent_type),a
        ld de,5
        add hl,de
        ld c,(hl)
        inc hl
        push hl
        ld b,0
        add hl,bc
        ld (ent_p),hl
        pop hl
        ld de,linebuf
        ld a,(ent_type)
        cp 1
        jr z,.dir
        cp 5
        jr nz,.nm
.dir:   ld a,'['
        ld (de),a
        inc de
.nm:    ld a,c
        cp 60
        jr c,.cp
        ld c,60
.cp:    ld b,0
        ld a,c
        or a
        jr z,.cpe
        ldir
.cpe:   ld a,(ent_type)
        cp 1
        jr z,.dir2
        cp 5
        jr nz,.z
.dir2:  ld a,']'
        ld (de),a
        inc de
.z:     xor a
        ld (de),a
        ld hl,(row_idx)
        ld de,(sel)
        or a
        sbc hl,de
        jr z,.chosen
        ld de,(row_cell)
        ld hl,linebuf
        ld b,30
        ld c,A_NORM
        call put_str
        jr .next
.chosen:
        ld de,(row_cell)
        ld hl,linebuf
        ld b,30
        ld c,A_SEL
        ld ix,tk_list
        call ticker_text
        jr .next
.blank: ld de,(row_cell)
        ld b,30
        ld c,A_NORM
        call fill_row
.next:  ld a,(row_no)
        inc a
        ld (row_no),a
        cp LIST_N
        jp c,.r
        ret

; the type of entry 'sel' (reads it again)
sel_type:
        ld hl,(sel)
        ld (args),hl
        ld a,1
        ld (args+2),a
        ld bc,3
        ld a,C_DREAD
        call cmd
        ld a,0
        ret c
        ret nz
        ld a,(reply)
        ret

; move the cursor by HL (signed)
move:   ex de,hl
        ld hl,(sel)
        add hl,de
        bit 7,h
        jr z,.p
        ld hl,0
.p:     ld de,(count)
        ld a,d
        or e
        ret z
        push hl
        or a
        sbc hl,de
        pop hl
        jr c,.in
        ld hl,(count)
        dec hl
.in:    ld (sel),hl
        ; keep it in view
        ld de,(top)
        or a
        sbc hl,de
        jr nc,.below
        ld hl,(sel)
        ld (top),hl
        jp draw_list
.below: ld de,LIST_N
        or a
        sbc hl,de
        jp c,draw_list
        ld hl,(sel)
        ld de,LIST_N-1
        or a
        sbc hl,de
        ld (top),hl
        jp draw_list

activate:
        ld hl,(count)
        ld a,h
        or l
        ret z
        call sel_type
        cp 1
        jr z,.enter
        cp 5
        jr z,.enter
        ld hl,(sel)
        ld a,C_PLAYI
        call cmd_u16
        ret
.enter: ld hl,(sel)
        ld a,C_DENTER
        call cmd_u16
        ret c
        or a
        jp z,got_folder
        cp E_BUSY
        jr z,.busy
        ret
.busy:  ld a,1
        ld (loading),a
        jp draw_state

; BS / right button / a click on the path: the parent folder (entry 0 is
; ".." outside the root)
parent: ld hl,0
        ld (sel),hl
        call sel_type
        cp 5
        ret nz
        jp activate

check_loading:
        ld hl,W_STATUS+ST_DIRST
        call rd
        cp 1
        ret z                   ; still loading
        push af
        xor a
        ld (loading),a
        pop af
        cp 2
        jr nz,.failed
        ld hl,txt_empty
        ld de,args
        call strcpy0
        ld a,C_DOPEN
        ld bc,1
        call cmd
        jp z,got_folder
.failed:
        ld hl,txt_listfail
        call show_path_msg
        jp draw_state

;------------------------------------------------------------------------------
; tickers: a text longer than its place stands still for a second, then moves
; left a pixel at a time, round and round with a gap. One moves at a time
; (title, artist, the chosen list entry in turn) so the rest stays smooth.

; ticker_text: IX = ticker, HL = text, DE = cell, B = width, C = colour.
; Draws the text; nothing happens when it is the same text in the same place.
ticker_text:
        ld (tt_text),hl
        ld (tt_cell),de
        ld (tt_wc),bc
        ld a,(ix+T_ROW)
        cp d
        jr nz,.new
        ld a,(ix+T_COL)
        cp e
        jr nz,.new
        push ix
        pop de
        ld hl,T_TEXT
        add hl,de
        ex de,hl
        ld hl,(tt_text)
        ld b,63
.cmp:   ld a,(de)
        cp (hl)
        jr nz,.new
        or a
        ret z
        inc hl
        inc de
        djnz .cmp
        ret
.new:   ld de,(tt_cell)
        ld (ix+T_ROW),d
        ld (ix+T_COL),e
        ld bc,(tt_wc)
        ld (ix+T_W),b
        push ix
        pop de
        ld hl,T_TEXT
        add hl,de
        ex de,hl
        ld hl,(tt_text)
        ld b,0
.cp:    ld a,(hl)
        or a
        jr z,.ce
        ld a,b
        cp 63
        jr nc,.ce
        ld a,(hl)
        ld (de),a
        inc hl
        inc de
        inc b
        jr .cp
.ce:    xor a
        ld (de),a
        ld (ix+T_LEN),b
        ld (ix+T_LAST),a
        ld (ix+T_LAST+1),a
        push ix
        pop hl
        ld de,T_TEXT
        add hl,de
        ld de,(tt_cell)
        ld bc,(tt_wc)
        push ix
        call put_str
        pop ix
        ld a,(ix+T_W)
        cp (ix+T_LEN)
        jr nc,.still
        ld (ix+T_ACT),1
        jp tk_take
.still: ld (ix+T_ACT),0
        ret

; give the turn to IX (the one that had it goes back to its start)
tk_take:
        ld hl,(tk_turn)
        ld a,h
        or l
        jr z,.set
        push ix
        pop de
        or a
        sbc hl,de
        jr z,.set
        add hl,de
        push ix
        push hl
        pop ix
        call tk_home
        pop ix
.set:   ld (tk_turn),ix
        ld hl,(JIFFY)
        ld (ix+T_START),l
        ld (ix+T_START+1),h
        ret

tk_home:                        ; IX back at its start
        ld a,(ix+T_LAST)
        or (ix+T_LAST+1)
        ret z
        xor a
        ld (ix+T_LAST),a
        ld (ix+T_LAST+1),a
        ld a,(ix+T_ACT)
        or a
        ret z
        ld hl,0
        jp tk_render

ticker_run:
        ld hl,(tk_turn)
        ld a,h
        or l
        jp z,.find
        push hl
        pop ix
        ld a,(ix+T_ACT)
        or a
        jr z,.next
        ld hl,(JIFFY)
        ld e,(ix+T_START)
        ld d,(ix+T_START+1)
        or a
        sbc hl,de
        ld a,(hold)
        ld e,a
        ld d,0
        or a
        sbc hl,de
        ret c                   ; the still second
        ld d,h                  ; offset = time * 3 / 4 (45 pixels a second)
        ld e,l
        add hl,hl
        add hl,de
        srl h
        rr l
        srl h
        rr l
        push hl
        ld a,(ix+T_LEN)
        add a,GAP
        ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        ex de,hl
        pop hl
        or a
        sbc hl,de
        jr nc,.done
        add hl,de
        ld a,l
        cp (ix+T_LAST)
        jr nz,.draw
        ld a,h
        cp (ix+T_LAST+1)
        ret z
.draw:  ld (ix+T_LAST),l
        ld (ix+T_LAST+1),h
        jp tk_render
.done:  call tk_home            ; a whole round
.next:  ld b,3
.nx:    ld de,TSIZE
        add ix,de
        push ix
        pop hl
        ld de,tk_end
        or a
        sbc hl,de
        jr c,.chk
        ld ix,tk_title
.chk:   ld a,(ix+T_ACT)
        or a
        jr nz,.got
        djnz .nx
        ld hl,0
        ld (tk_turn),hl
        ret
.got:   ld (tk_turn),ix
        ld hl,(JIFFY)
        ld (ix+T_START),l
        ld (ix+T_START+1),h
        ret
.find:  ld ix,tk_list           ; nobody's turn: look from the title on
        jr .next

; tk_render: IX = ticker, HL = pixel offset
tk_render:
        ld a,l
        and 7
        ld b,a
        ld a,7
        sub b
        ld (.jr+1),a          ; the shift: skip 7 - s of the 7 'add hl,hl'
        srl h
        rr l
        srl h
        rr l
        srl h
        rr l
        ld a,l
        ld (tr_k),a
        ld a,(ix+T_LEN)
        add a,GAP
        ld (tr_n),a
        ld de,tkbuf
        ld b,(ix+T_W)
.cell:  push bc
        ld a,(tr_k)
        call tk_glyph
        push bc
        ld a,(tr_k)
        inc a
        ld hl,tr_n
        cp (hl)
        jr c,.k2
        xor a
.k2:    ld (tr_k),a
        call tk_glyph
        push bc
        pop iy
        pop bc
.ln:    ld a,(bc)
        ld h,a
        ld l,(iy+0)
.jr:    jr .sh                  ; displacement patched
.sh:    add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        add hl,hl
        ld a,h
        ld (de),a
        inc de
        inc bc
        inc iy
        ld a,c
        and 7
        jr nz,.ln
        pop bc
        djnz .cell
        ld d,(ix+T_ROW)
        ld e,(ix+T_COL)
        call cell_addr
        call vwrite
        ld a,(ix+T_W)
        add a,a
        add a,a
        add a,a
        ld b,a
        ld c,VDP_D
        ld hl,tkbuf
        otir
        ei
        ret

; tk_glyph: A = position in the text (and gap) -> BC = its glyph
tk_glyph:
        push de
        ld c,' '
        cp (ix+T_LEN)
        jr nc,.ch
        push ix
        pop hl
        ld e,a
        ld d,0
        add hl,de
        ld de,T_TEXT
        add hl,de
        ld c,(hl)
.ch:    ld a,c
        sub 32
        jr nc,.a
        xor a
.a:     cp 96
        jr c,.b
        ld a,'?'-32
.b:     ld l,a
        ld h,0
        add hl,hl
        add hl,hl
        add hl,hl
        ld de,font
        add hl,de
        ld b,h
        ld c,l
        pop de
        ret

;------------------------------------------------------------------------------
; the mouse, in either port (found when it first moves); an arrow sprite with
; a shadow
mouse:  ld a,(ms_port)
        or a
        jr nz,.read
        ld a,(ms_probe)         ; not found yet: look every 8th pass
        inc a
        ld (ms_probe),a
        and 7
        ret nz
        ld a,1
        call ms_probe1
        jr nz,.found
        ld a,2
        call ms_probe1
        ret z
.found: ld (ms_port),a
        jr .move
.read:  call ms_read
.move:  ld a,(ms_dx)            ; GTPAD's offsets: + is right / down
        call sext
        ld a,(ms_x)
        ld l,a
        ld h,0
        add hl,de
        bit 7,h
        jr z,.x1
        ld hl,0
.x1:    ld a,h
        or a
        jr z,.x2
        ld l,255
.x2:    ld a,l
        ld (ms_x),a
        ld a,(ms_dy)
        call sext
        ld a,(ms_y)
        ld l,a
        ld h,0
        add hl,de
        bit 7,h
        jr z,.y1
        ld hl,0
.y1:    ld a,h
        or a
        jr nz,.yc
        ld a,l
        cp 192
        jr c,.y2
.yc:    ld l,191
.y2:    ld a,l
        ld (ms_y),a
        call ms_sprite
        ; the buttons
        ld a,(ms_port)
        ld ix,GTTRIG
        call bios
        and 1
        ld b,a
        push bc
        ld a,(ms_port)
        add a,2
        ld ix,GTTRIG
        call bios
        pop bc
        and 2
        or b
        ld b,a
        ld a,(ms_prev)
        cpl
        and b
        ld c,a                  ; just pressed
        ld a,b
        ld (ms_prev),a
        bit 0,c
        jp nz,left_click
        bit 1,c
        jp nz,right_click
        bit 0,b
        jr z,.up
        ld a,(eq_open)          ; held: dragging an equalizer slider
        or a
        ret z
        jp eq_drag_update
.up:    ld a,#FF
        ld (eq_drag),a
        ret

; ms_probe1: A = port; ZF=0 (A = port) when a mouse is there: an empty port
; (or a joystick) gives the same non-zero offsets every time (1,1 or -1,-1),
; a mouse gives 0,0 at rest. So first a rest, then a movement.
ms_probe1:
        ld (ms_try),a
        call ms_read
        ld a,(ms_try)
        ld hl,ms_rest
        jr nz,.moved
        ld a,(ms_dx)            ; 0,0: a mouse at rest (or FF,FF: nothing)
        ld b,a
        ld a,(ms_dy)
        or b
        jr nz,.none             ; FF,FF: nothing there
        ld a,(ms_try)
        or (hl)
        ld (hl),a               ; this port has rested
        xor a
        ret
.moved: and (hl)                ; moved, after a rest?
        ret z
        ld a,(ms_try)
        or a
        ret
.none:  xor a
        ret

; ms_read: A = port -> ms_dx, ms_dy; ZF=1 when nothing moved (or no mouse)
ms_read:
        cp 1
        ld a,12
        jr z,.p
        ld a,16
.p:     ld (ms_fn),a
        ld ix,GTPAD
        call bios
        ld a,(ms_fn)
        inc a
        ld ix,GTPAD
        call bios
        ld (ms_dx),a
        ld a,(ms_fn)
        add a,2
        ld ix,GTPAD
        call bios
        ld (ms_dy),a
        ld c,a
        ld a,(ms_dx)
        ld b,a
        and c
        inc a
        ret z                   ; FF, FF: nothing in the port
        ld a,b
        or c
        ret

sext:   ld e,a                  ; DE = A, signed
        add a,a
        sbc a,a
        ld d,a
        ret

ms_sprite:
        ld a,(ms_x)
        ld hl,ms_shown_x
        cp (hl)
        jr nz,.d
        ld a,(ms_y)
        ld hl,ms_shown_y
        cp (hl)
        ret z
.d:     ld a,(ms_x)
        ld (ms_shown_x),a
        ld a,(ms_y)
        ld (ms_shown_y),a
        ld hl,#1E00
        call vwrite
        ld a,(ms_y)
        dec a
        ld b,a
        out (VDP_D),a
        ld a,(ms_x)
        ld c,a
        out (VDP_D),a
        xor a
        out (VDP_D),a
        out (VDP_D),a
        ld a,b                  ; the shadow, a pixel right and down
        inc a
        out (VDP_D),a
        ld a,c
        inc a
        jr nz,.sx
        dec a
.sx:    out (VDP_D),a
        xor a
        out (VDP_D),a
        out (VDP_D),a
        ld a,216
        out (VDP_D),a
        ei
        ret

right_click:
        ld a,(eq_open)
        or a
        jp nz,eq_close
        jp parent

; left_click: what is under the pointer
left_click:
        ld a,(ms_x)
        rrca
        rrca
        rrca
        and #1F
        ld e,a                  ; E = column
        ld a,(ms_y)
        rrca
        rrca
        rrca
        and #1F
        ld d,a                  ; D = row
        or a
        jr nz,.r4
        ld a,e                  ; the title bar's close box
        cp 31
        jp z,quit
        ret
.r4:    cp 4
        jr nz,.r5
        ld a,e
        or a
        ret z
        cp 31
        ret nc
        jp click_seek
.r5:    cp 5
        jr nz,.area
        ld a,e
        cp 10
        jr nc,.r5b
        ld a,(eq_open)
        or a
        jp nz,eq_close
        jp toggle_view
.r5b:   cp 20
        ret nc
        ld a,(eq_open)
        or a
        jp nz,next_eq
        jp eq_open_screen
.area:  cp 6
        ret c
        cp 16
        jr nc,.r18
        ld a,(eq_open)
        or a
        jp nz,eq_press
        ld a,e
        or a
        ret z
        cp 25
        ret nc
        jp toggle_view
.r18:   cp 18
        jp z,parent
        cp 19
        ret c
        cp 23
        jp z,click_bar
        ld a,e                  ; the list
        cp 31
        jr nz,.row
        ld a,d
        cp 19
        ld hl,-LIST_N
        jp z,move
        cp 22
        ld hl,LIST_N
        jp z,move
        ret
.row:   or a
        ret z
        ld a,d
        sub LIST_ROW
        ld e,a
        ld d,0
        ld hl,(top)
        add hl,de
        ld de,(count)
        or a
        sbc hl,de
        ret nc
        add hl,de
        ld de,(sel)
        or a
        sbc hl,de
        jp z,activate           ; the chosen one again: open or play it
        add hl,de
        ld (sel),hl
        jp draw_list

; the control row: E = column
click_bar:
        ld a,e
        cp 1
        jp z,do_prev
        cp 2
        jp z,seek_back
        cp 3
        jp z,play_btn
        cp 4
        jp z,pause_btn
        cp 5
        jp z,do_stop
        cp 6
        jp z,seek_fwd
        cp 7
        jp z,do_next
        cp 9
        ret c
        cp 12
        jp c,tog_shuf
        cp 13
        ret c
        cp 16
        jp c,tog_rep
        cp 17
        ret c
        cp 19
        jp c,eq_toggle
        cp 24
        ret c
        ; the volume meter: (x - 191) * 25 / 16
        ld a,(ms_x)
        sub 191
        ld l,a
        ld h,0
        ld de,25
        call mul16
        srl h
        rr l
        srl h
        rr l
        srl h
        rr l
        srl h
        rr l
        ld a,l
        cp 101
        jr c,.v
        ld a,100
.v:     ld e,a
        ld a,C_VOLUME
        jp cmd_u8

click_seek:                     ; the seek bar: (total / 16) * (x - 8) / 15
        ld a,(st+ST_FLAGS)
        and F_STREAM
        ret nz
        ld hl,(st+ST_TOTAL)
        ld a,h
        or l
        ret z
        srl h
        rr l
        srl h
        rr l
        srl h
        rr l
        srl h
        rr l
        ld a,(ms_x)
        sub 8
        ld e,a
        ld d,0
        call mul16
        ld de,15
        call div16
        ld a,C_SEEK
        jp cmd_u16

; the equalizer screen under the pointer: which slider
eq_press:
        call eq_hit
        ld a,(eq_drag)
        cp #FF
        ret z
        call eq_select
        ; fall through
eq_drag_update:
        ld a,(eq_drag)
        cp 5
        jr z,.bal
        cp #FF
        ret z
        ld d,a
        ld a,(ms_y)             ; gain = 12 - (y - 48 - 5) / 2
        sub 53
        jr nc,.y1
        xor a
.y1:    srl a
        cp 25
        jr c,.y2
        ld a,24
.y2:    ld c,a
        ld a,12
        sub c
        ld c,a
        ld a,d
        jp eq_set_gain
.bal:   ld a,(ms_x)             ; balance = (x - 40) * 100 / 148
        sub 40
        jr nc,.b1
        xor a
.b1:    ld l,a
        ld h,0
        ld de,100
        call mul16
        ld de,148
        call div16
        ld a,l
        cp 101
        jr c,.b2
        ld a,100
.b2:    cp 47                   ; near the middle: the middle
        jr c,.b3
        cp 54
        jr nc,.b3
        ld a,50
.b3:    ld c,a
        jp eq_set_bal

eq_hit: ld a,#FF
        ld (eq_drag),a
        ld a,(ms_x)
        sub 8
        ret c
        cp 192
        ret nc
        ld b,a
        ld a,(ms_y)
        sub 48
        ret c
        cp 80
        ret nc
        cp 64
        jr nc,.bal
        ld a,b
        rrca
        rrca
        rrca
        and #1F
        ld hl,band_cell
        call idx
        ld a,(hl)
        ld (eq_drag),a
        ret
.bal:   ld a,b
        cp 24
        ret c
        ld a,5
        ld (eq_drag),a
        ret

;------------------------------------------------------------------------------
keys:   ld c,#06
        ld e,#FF
        call BDOS
        or a
        ret z
        ld b,a
        ld a,(eq_open)
        or a
        ld a,b
        jp nz,eq_keys
        cp 27
        jp z,quit
        cp 30
        jr nz,.k1
        ld hl,-1
        jp move
.k1:    cp 31
        jr nz,.k2
        ld hl,1
        jp move
.k2:    cp 29
        jr nz,.k3
        ld hl,-LIST_N
        jp move
.k3:    cp 28
        jr nz,.k4
        ld hl,LIST_N
        jp move
.k4:    cp 13
        jp z,activate
        cp 8
        jp z,parent
        ; fall through

; the keys of both screens: A = key
keys_common:
        cp ' '
        jp z,do_pause
        cp '+'
        jr z,vol_up
        cp '='
        jr z,vol_up
        cp '-'
        jr z,vol_down
        cp ','
        jp z,seek_back
        cp '<'
        jp z,seek_back
        cp '.'
        jp z,seek_fwd
        cp '>'
        jp z,seek_fwd
        and #DF
        cp 'Q'
        jp z,quit
        cp 'S'
        jp z,do_stop
        cp 'N'
        jp z,do_next
        cp 'P'
        jp z,do_prev
        cp 'H'
        jp z,tog_shuf
        cp 'R'
        jp z,tog_rep
        cp 'E'
        jp z,next_eq
        cp 'G'
        jp z,eq_toggle
        cp 'V'
        jp z,toggle_view
        ret

vol_up: ld a,(st+ST_VOL)
        add a,5
        cp 101
        jr c,vol_set
        ld a,100
        jr vol_set
vol_down:
        ld a,(st+ST_VOL)
        sub 5
        jr nc,vol_set
        xor a
vol_set:
        ld e,a
        ld a,C_VOLUME
        jp cmd_u8

do_pause:
        ld a,C_PAUSE
        jp cmd0
do_stop:
        ld a,C_STOP
        jp cmd0
do_next:
        ld a,C_NEXT
        jp cmd0
do_prev:
        ld a,C_PREV
        jp cmd0

play_btn:                       ; resume, or play the chosen entry
        ld a,(st+ST_STATE)
        cp 2
        jr z,do_pause
        or a
        ret nz
        jp activate
pause_btn:
        ld a,(st+ST_STATE)
        cp 1
        ret nz
        jr do_pause

seek_back:
        ld de,-10
        jr seek_by
seek_fwd:
        ld de,10
seek_by:                        ; DE = seconds (signed)
        ld a,(st+ST_FLAGS)
        and F_SEEK
        ret z
        ld hl,(st+ST_ELAPS)
        add hl,de
        bit 7,h
        jr z,.ok
        ld hl,0
.ok:    ld a,C_SEEK
        jp cmd_u16

tog_shuf:
        ld e,M_SHUF
        jr tog_mode
tog_rep:
        ld e,M_REPEAT
tog_mode:
        ld a,(st+ST_MODE)
        xor e
        ld e,a
        ld a,C_MODE
        call cmd_u8
        call fetch_status
        jp draw_info

eq_toggle:
        ld a,(eq_open)
        or a
        jp nz,eq_close
        jp eq_open_screen

; the equalizer screen's keys
eq_keys:
        cp 27
        jp z,eq_close
        cp 13
        jp z,eq_close
        cp 8
        jp z,eq_close
        cp 29
        jr nz,.k1
        ld a,(eq_sel)
        or a
        ret z
        dec a
        jp eq_select
.k1:    cp 28
        jr nz,.k2
        ld a,(eq_sel)
        cp 5
        ret z
        inc a
        jp eq_select
.k2:    cp 30
        jr nz,.k3
        ld c,1
        jp eq_step
.k3:    cp 31
        jr nz,.k4
        ld c,-1
        jp eq_step
.k4:    cp '0'
        jp z,eq_reset
        jp keys_common

quit:   call restore_screen
        ld de,txt_bye
        ld c,#09
        call BDOS
        ld c,0
        jp BDOS

notfound:
        ld de,txt_notfound
        ld c,#09
        call BDOS
        ld c,0
        jp BDOS

noanswer:
        ld de,txt_noanswer
        ld c,#09
        call BDOS
        ld c,0
        jp BDOS

;------------------------------------------------------------------------------
; small helpers
strcpy:                         ; HL -> DE, DE ends on the 0
        ld a,(hl)
        ld (de),a
        or a
        ret z
        inc hl
        inc de
        jr strcpy

strcpy0:                        ; HL -> DE with the 0, DE after it
        ld a,(hl)
        ld (de),a
        inc hl
        inc de
        or a
        jr nz,strcpy0
        ret

; fmt_u16: HL as decimal at DE (no leading zeros), DE ends on the 0
fmt_u16:
        push bc
        xor a
        ld (fmt_started),a
        ld bc,10000
        call .digit
        ld bc,1000
        call .digit
        ld bc,100
        call .digit
        ld bc,10
        call .digit
        ld a,l
        add a,'0'
        ld (de),a
        inc de
        xor a
        ld (de),a
        pop bc
        ret
.digit: ld a,'0'-1
.d:     inc a
        or a
        sbc hl,bc
        jr nc,.d
        add hl,bc
        cp '0'
        jr nz,.w
        push af
        ld a,(fmt_started)
        or a
        jr nz,.w1
        pop af
        ret
.w1:    pop af
.w:     ld (de),a
        inc de
        ld a,1
        ld (fmt_started),a
        ret

; two_digits: A (0..99) -> D tens, E units
two_digits:
        ld d,0
.t:     cp 10
        jr c,.u
        sub 10
        inc d
        jr .t
.u:     ld e,a
        ret

; div16: HL = HL / DE, DE = remainder
div16:  push bc
        ld a,d
        or e
        jr z,.z
        ld b,h
        ld c,l
        ld hl,0
        ld a,16
.l:     sla c
        rl b
        adc hl,hl
        or a
        sbc hl,de
        jr nc,.s
        add hl,de
        jr .n
.s:     inc c
.n:     dec a
        jr nz,.l
        ex de,hl                ; DE = remainder
        ld h,b
        ld l,c
        pop bc
        ret
.z:     ld hl,0
        ld de,0
        pop bc
        ret

; mul16: HL = HL * DE (low 16 bits)
mul16:  push bc
        ld b,h
        ld c,l
        ld hl,0
        ld a,16
.l:     add hl,hl
        sla e
        rl d
        jr nc,.n
        add hl,bc
.n:     dec a
        jr nz,.l
        pop bc
        ret

;------------------------------------------------------------------------------
; data
idtext:         db "PVAUDIO",0
txt_title:      db "PICOAMP",0
txt_spectrum:   db "SPECTRUM",0
txt_scope:      db "SCOPE",0
txt_equalizer:  db "EQUALIZER",0
txt_level:      db "LEVEL",0
txt_keys:       db "SHF REP EQ",0
txt_shf:        db "SHF",0
txt_rep:        db "REP",0
txt_vol:        db "VOL",0
txt_notrack:    db "NO TRACK",0
txt_playing:    db "PLAYING",0
txt_paused:     db "PAUSED",0
txt_stopped:    db "STOPPED",0
txt_loading:    db "LOADING THE LIST...",0
txt_buffering:  db "BUFFERING ",0
txt_kbps:       db "K ",0
txt_khz:        db "KHZ ",0
txt_mono:       db "MONO",0
txt_stereo:     db "ST",0
txt_nofolder:   db "FOLDER NOT FOUND",0
txt_listfail:   db "THE STATION LIST DID NOT LOAD",0
txt_p12:        db "+12",0
txt_z:          db "0",0
txt_m12:        db "-12",0
txt_bal:        db "BAL",0
txt_db:         db " DB",0
txt_balance:    db "BALANCE ",0
txt_centre:     db "CENTRE",0
txt_gclose:     db "G:CLOSE",0
txt_empty:      db 0
txt_bye:        db "PicoAmp: the music plays on.",13,10,"$"
txt_notfound:   db "No PicoVerse Audio Engine found (flash LoadROM -p1).",13,10,"$"
txt_noanswer:   db "The engine does not answer.",13,10,"$"
label_bars:     db 0,6,12,17,21,#FF

eq_tab:         db 0,0,0,0,0, 5,2,-2,3,5, -1,3,4,2,-1, 3,1,-1,2,4, 3,0,-1,1,3, -4,-1,4,3,-2, 8,4,0,0,0, 6,2,0,1,5
eq_names:       db "EQ FLAT",0,0,0, "EQ ROCK",0,0,0, "EQ POP",0,0,0,0, "EQ JAZZ",0,0,0, "EQ CLASS",0,0
                db "EQ VOICE",0,0, "EQ BASS",0,0,0, "EQ LOUD",0,0,0, "EQ OWN",0,0,0,0
eq_lab:         db " 60",0, "250",0, " 1K",0, " 4K",0, "12K",0
eq_hz:          db "60 HZ",0,0,0, "250 HZ",0,0, "1 KHZ",0,0,0, "4 KHZ",0,0,0, "12 KHZ",0,0
; the band of each cell of the equalizer picture (FF: none)
band_cell:      db #FF,#FF,#FF,#FF,0,0,0,#FF,1,1,1,#FF,2,2,2,#FF,3,3,3,#FF,4,4,4,#FF
eq_party:       db 0,58,66              ; a line of each part
knob_v:         db #3C,#7E,#FF,#FF,#FF,#7E,#3C
knob_h:         db #3C,#7E,#FF,#FF,#FF,#FF,#7E,#3C
bitmask:        db #80,#40,#20,#10,#08,#04,#02,#01

; level 0..31 -> lit segments 0..20
seg_of:         db 0,1,1,2,3,3,4,5,5,6,6,7,8,8,9,10,10,11,12,12,13,14,14,15,15,16,17,17,18,19,19,20
; segment colours (fg green / yellow / red over navy)
seg_col:        db GREEN*16+NAVY,GREEN*16+NAVY,GREEN*16+NAVY,GREEN*16+NAVY,GREEN*16+NAVY
                db GREEN*16+NAVY,GREEN*16+NAVY,GREEN*16+NAVY,GREEN*16+NAVY,GREEN*16+NAVY
                db GREEN*16+NAVY,YELLOW*16+NAVY,YELLOW*16+NAVY,YELLOW*16+NAVY,YELLOW*16+NAVY
                db YELLOW*16+NAVY,RED*16+NAVY,RED*16+NAVY,RED*16+NAVY,RED*16+NAVY

pat_track:      db #00,#00,#00,#FF,#FF,#00,#00,#00
pat_knob:       db #00,#3C,#7E,#FF,#FF,#7E,#3C,#00
pat_hline:      db #00,#00,#00,#FF,#00,#00,#00,#00
pat_vol_on:     db #00,#7E,#7E,#7E,#7E,#7E,#7E,#00
pat_vol_off:    db #00,#00,#00,#00,#00,#00,#7E,#00
pat_close:      db #00,#42,#24,#18,#18,#24,#42,#00
pat_up:         db #00,#10,#38,#7C,#FE,#00,#00,#00
pat_down:       db #00,#00,#00,#FE,#7C,#38,#10,#00
pat_arrow:      db #80,#C0,#E0,#F0,#F8,#E0,#B0,#18
icons:          db #00,#46,#4E,#5E,#5E,#4E,#46,#00   ; |<
                db #00,#12,#36,#7E,#7E,#36,#12,#00   ; <<
                db #00,#20,#30,#38,#3C,#38,#30,#20   ; >
                db #00,#6C,#6C,#6C,#6C,#6C,#6C,#00   ; ||
                db #00,#7C,#7C,#7C,#7C,#7C,#7C,#00   ; []
                db #00,#48,#6C,#7E,#7E,#6C,#48,#00   ; >>
                db #00,#62,#72,#7A,#7A,#72,#62,#00   ; >|

; palette: 16 x (0RRR0BBB, 00000GGG)
palette:        db #00,#00, #00,#00, #13,#01, #26,#02, #37,#06, #77,#07, #45,#04, #01,#04
                db #22,#07, #71,#07, #70,#04, #71,#01, #66,#02, #23,#02, #02,#00, #56,#05
palette_msx:    db #00,#00, #00,#00, #11,#06, #33,#07, #17,#01, #27,#03, #51,#01, #27,#06
                db #71,#01, #73,#03, #61,#06, #64,#06, #11,#04, #65,#02, #55,#05, #77,#07

        include "digits.inc"
        include "tables.inc"

;------------------------------------------------------------------------------
; variables with a starting value
slot:           db 0
token:          db 0
code:           db 0
result:         db 0
t0:             dw 0
rlen:           dw 0
gen_new:        db 0
viz_seq:        db #FF
viz_new:        db 0
scope_tick:     db 0
gen_shown:      db #FF
loading:        db 0
vol_shown:      db #FF
eq_preset:      db 0
eq_shown:       db #FF
eq_open:        db 0
eq_sel:         db 0
eq_drag:        db #FF
eqbal:          db 50
view:           db 0
hold:           db 60
mode_shown:     db #FF
knob_pos:       db 0
fmt_started:    db 0
lab_bar:        db 0
bar_col:        db 0
bar_no:         db 0
pk_level:       db 0
lit:            db 0
pk:             db 0
cell_hi:        db 0
cell_top:       db 0
cache_p:        dw 0
cellcol:        db 0
cellpat:        ds 8
cellclr:        ds 8
count:          dw 0
sel:            dw 0
top:            dw 0
row_idx:        dw 0
row_cell:       dw 0
have_list:      db 0
ent_p:          dw 0
row_no:         db 0
ent_type:       db 0
tmp16:          dw 0
colfn:          dw scope_colrow
plot_inv:       db 0
sp_ys:          dw 0
sp_prev:        db 0
sp_cur:         db 0
sp_x:           db 0
vs_x:           db 0
er_b:           db 0
er_cc:          db 0
cr_y0:          db 0
cr_last:        db #FF
er_band:        db 0
fe_p:           dw 0
tt_text:        dw 0
tt_cell:        dw 0
tt_wc:          dw 0
tk_turn:        dw 0
tr_k:           db 0
tr_n:           db 0
ms_port:        db 0
ms_probe:       db 0
ms_fn:          db 0
ms_dx:          db 0
ms_dy:          db 0
ms_x:           db 128
ms_y:           db 96
ms_shown_x:     db #FF
ms_shown_y:     db #FF
ms_prev:        db 0
ms_try:         db 0
ms_rest:        db 0                    ; bit 0 / 1: port 1 / 2 gave 0,0
progend:

; buffers (not in the file; cleared at the start)
REPLY_MAX       equ 1024
bss_start       equ (progend + 7) & #FFF8
font            equ bss_start           ; 96 x 8, aligned to 8 (the tickers count on it)
vizbuf          equ font + 96*8         ; the area: 10 x 192
rowcol          equ vizbuf + 1920       ; colours of one area row
tkbuf           equ rowcol + 192        ; one ticker row
tk_title        equ tkbuf + 240
tk_artist       equ tk_title + TSIZE
tk_list         equ tk_artist + TSIZE
tk_end          equ tk_list + TSIZE
scope_old       equ tk_end              ; the trace on screen (64 y values)
cellcache       equ scope_old + 64      ; (NBARS + 2) x 10 bar cells
st              equ cellcache + (NBARS+2)*10
viz             equ st + 224
args            equ viz + V_SCOPE + 64  ; 256
reply           equ args + 256          ; REPLY_MAX + 1
linebuf         equ reply + REPLY_MAX + 1
path            equ linebuf + 64
argbuf          equ path + 64           ; 128
numbuf          equ argbuf + 128
eqg             equ numbuf + 8          ; the equalizer screen's gains
eqcols          equ eqg + 8             ; 3 x 24 colours of its parts
bar_last        equ eqcols + 72         ; (NBARS + 2) x (lit segments, peak) drawn
bar_raw         equ bar_last + (NBARS+2)*2  ; (level, peak) from the engine, drawn
bss_end         equ bar_raw + (NBARS+2)*2
        assert bss_end < #4000
