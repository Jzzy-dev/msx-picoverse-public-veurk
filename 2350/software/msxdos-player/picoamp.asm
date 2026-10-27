;==============================================================================
; PICOAMP.COM - a full-screen player for the PicoVerse Audio Engine, MSX-DOS
;
;   PICOAMP            start in the root folder of the card
;   PICOAMP /MUSIC     start in a folder
;
; MSX2: SCREEN 4 with every screen cell a pattern of its own (a bitmap with
; two colours per 8-pixel row). The engine decodes and analyses the sound;
; this program copies its status and its spectrum/VU data once a frame and
; draws what changed.
;
; Keys: Up/Down select, Left/Right page, Enter open/play, BS parent folder,
;       Space pause/play, S stop, N next, P previous, + / - volume,
;       H shuffle, R repeat, E next equalizer preset,
;       ESC quit (the music keeps playing: the cartridge plays it).
;
; Assemble: sjasmplus --raw=PICOAMP.COM picoamp.asm
;==============================================================================

        org #0100

BDOS    equ #0005
RDSLT   equ #000C
WRSLT   equ #0014
CALSLT  equ #001C
ENASLT  equ #0024
EXPTBL  equ #FCC1
JIFFY   equ #FC9E
RAMAD1  equ #F342
CHGMOD  equ #005F
CGTABL  equ #0004

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
C_VOLUME equ #27
C_MODE   equ #28
C_EQ     equ #2A
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
F_STREAM equ #10
F_BUFFER equ #20

; visualisation block offsets
V_SEQ    equ #00
V_LEVEL  equ #04
V_PEAK   equ #20
V_VU     equ #40
V_FREQ   equ #90
V_COPY   equ #C0            ; bytes copied each frame (up to the frequencies)
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

;------------------------------------------------------------------------------
start:  ld sp,(#0006)
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
        call draw_static
        call find_eq
        call draw_eq_name
        ld hl,argbuf
        call open_path
        call fetch_status
        call draw_info
mainloop:
        ei
        halt
        call fetch_frame
        call draw_bars
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
        call keys
        jr mainloop

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

fetch_frame:
        ld hl,W_VIZ
        ld de,viz
        ld bc,V_COPY
        call eng_copy
        ld hl,W_STATUS+ST_DGEN
        call rd
        ld (gen_new),a
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
        ld bc,(rlen)
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
; screen: SCREEN 4, palette, every cell its own pattern, sprites off, the font
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
        ; no sprites
        ld hl,#1E00
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

; call the main ROM routine at IX
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

; write the 8 pattern bytes at HL' (IX) and 8 colour bytes (IY) of cell DE
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

; a big digit (0-9 in A) at cell DE, colour C: 2 x 3 cells
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
        ld de,#0501
        ld hl,txt_spectrum
        ld b,8
        ld c,MAGENTA*16+NAVY
        call put_str
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
        ; frequency labels under bars 0, 6, 12, 17, 21 (from the engine's table)
        ld hl,label_bars
.lab:   ld a,(hl)
        cp #FF
        jr z,.labend
        inc hl
        push hl
        call freq_label
        pop hl
        jr .lab
.labend:
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
; spectrum and VU: 24 + 2 bars of 10 cells, 2 LED segments a cell (20 levels)
draw_bars:
        ld a,(viz+V_SEQ)
        bit 0,a
        ret nz                  ; caught while the engine wrote it: next frame
        ld b,0
.b:     ld a,b
        cp NBARS
        jr nc,.vu
        ld hl,viz+V_LEVEL
        call idx
        ld c,(hl)
        ld hl,viz+V_PEAK
        call idx
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
.draw:  push bc
        ld a,b
        ld (bar_no),a
        call draw_bar
        pop bc
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
        ld b,0                  ; cell 0 = bottom
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
        ld a,b
        call idx
        ld a,(hl)
        cp c
        jr z,.next
        ld (hl),c
        push bc
        call bar_cell
        pop bc
.next:  inc b
        ld a,b
        cp 10
        jr c,.c
        ret

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
; the status: time, title, artist, format, seek bar, state, volume
draw_info:
        ; time m:ss (elapsed)
        ld hl,(st+ST_ELAPS)
        ld de,60
        call div16              ; HL = minutes, A = seconds... (remainder in DE)
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
        ; title / artist / format
        ld de,#010B
        ld hl,st+ST_TITLE
        ld a,(hl)
        or a
        jr nz,.t
        ld hl,txt_notrack
.t:     ld b,21
        ld c,YELLOW*16+NAVY
        call put_str
        ld de,#020B
        ld hl,st+ST_ARTIST
        ld a,(hl)
        or a
        jr nz,.a
        ld hl,st+ST_ALBUM       ; a stream: the station
.a:     ld b,21
        ld c,CYAN*16+NAVY
        call put_str
        call fmt_format
        ld de,#030B
        ld hl,linebuf
        ld b,21
        ld c,GREEN*16+NAVY
        call put_str
        call draw_seek
        call draw_state
        call draw_modes
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
        ld b,21
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

; read the visible entries (DIR_READ top, 4) and draw them
draw_list:
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
        cp 26
        jr c,.cp
        ld c,26
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
        ld c,A_NORM
        jr nz,.col
        ld c,A_SEL
.col:   ld de,(row_cell)
        ld hl,linebuf
        ld b,30
        call put_str
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
keys:   ld c,#06
        ld e,#FF
        call BDOS
        or a
        ret z
        cp 27
        jp z,quit
        cp 'q'
        jp z,quit
        cp 'Q'
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
        jr nz,.k5
        ; parent: entry 0 is ".." outside the root
        ld hl,0
        ld (sel),hl
        call sel_type
        cp 5
        ret nz
        jp activate
.k5:    cp ' '
        jr nz,.k6
        ld a,C_PAUSE
        ld bc,0
        jp cmd
.k6:    and #DF
        cp 'S'
        jr nz,.k7
        ld a,C_STOP
        ld bc,0
        jp cmd
.k7:    cp 'N'
        jr nz,.k8
        ld a,C_NEXT
        ld bc,0
        jp cmd
.k8:    cp 'P'
        jr nz,.k8b
        ld a,C_PREV
        ld bc,0
        jp cmd
.k8b:   cp 'H'
        jr nz,.k8c
        ld e,M_SHUF
        jr .mode
.k8c:   cp 'R'
        jr nz,.k8d
        ld e,M_REPEAT
.mode:  ld a,(st+ST_MODE)
        xor e
        ld e,a
        ld a,C_MODE
        call cmd_u8
        jp fetch_status_info
.k8d:   cp 'E'
        jr nz,.k9
        jp next_eq
.k9:    cp '+' & #DF
        jr z,.up
        cp '-' & #DF
        jr z,.down
        ret
.up:    ld a,(st+ST_VOL)
        add a,5
        cp 101
        jr c,.v
        ld a,100
        jr .v
.down:  ld a,(st+ST_VOL)
        sub 5
        jr nc,.v
        xor a
.v:     ld e,a
        ld a,C_VOLUME
        jp cmd_u8

; E: the next equalizer preset (the same ones as PicoPlay)
next_eq:
        ld a,(eq_preset)
        inc a
        cp 8
        jr c,.ok
        xor a
.ok:    ld (eq_preset),a
        ld l,a
        ld h,0
        ld e,l
        ld d,h
        add hl,hl
        add hl,hl
        add hl,de               ; *5
        ld de,eq_tab
        add hl,de
        ld de,args
        ld bc,5
        ldir
        ld bc,5
        ld a,C_EQ
        call cmd
; which preset the engine's equalizer is (8 = its own setting, from PicoPlay)
find_eq:
        ld hl,W_STATUS+179
        ld de,linebuf
        ld bc,5
        call eng_copy
        ld b,0
        ld hl,eq_tab
.p:     push bc
        push hl
        ld de,linebuf
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

draw_eq_name:                   ; "EQ ROCK" between the SPECTRUM and LEVEL labels
        ld a,(eq_preset)
        add a,a
        add a,a
        add a,a                 ; *8
        ld l,a
        ld h,0
        ld de,eq_names
        add hl,de
        ld de,#050B
        ld b,9
        ld c,GREY*16+NAVY
        jp put_str

fetch_status_info:
        call fetch_status
        jp draw_info

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
txt_empty:      db 0
txt_bye:        db "PicoAmp: the music plays on.",13,10,"$"
txt_notfound:   db "No PicoVerse Audio Engine found (flash LoadROM -p1).",13,10,"$"
txt_noanswer:   db "The engine does not answer.",13,10,"$"
label_bars:     db 0,6,12,17,21,#FF

eq_tab:         db 0,0,0,0,0, 5,2,-2,3,5, -1,3,4,2,-1, 3,1,-1,2,4, 3,0,-1,1,3, -4,-1,4,3,-2, 8,4,0,0,0, 6,2,0,1,5
eq_names:       db "EQ FLAT",0, "EQ ROCK",0, "EQ POP",0,0, "EQ JAZZ",0, "EQ CLASS",0, "EQ VOICE",0, "EQ BASS",0, "EQ LOUD",0, "EQ OWN",0,0

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

;------------------------------------------------------------------------------
; variables
slot:           db 0
token:          db 0
code:           db 0
result:         db 0
t0:             dw 0
rlen:           dw 0
gen_new:        db 0
gen_shown:      db #FF
loading:        db 0
vol_shown:      db #FF
eq_preset:      db 0
mode_shown:     db #FF
knob_pos:       db 0
fmt_started:    db 0
lab_bar:        db 0
bar_col:        db 0
bar_no:         db 0
pk_level:       db 0
lit:            db 0
pk:             db 0
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
ent_size:       dw 0
tmp16:          dw 0
numbuf:         ds 8
linebuf:        ds 64
path:           ds 64
argbuf:         ds 128
cellcache:      ds (NBARS+2)*10,#FF
font:           ds 96*8
st:             ds 224
viz:            ds V_COPY
args:           ds 512
reply:          ds 2049
progend:
        assert progend < #4000
