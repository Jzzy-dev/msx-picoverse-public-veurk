;==============================================================================
; SE Play 0.1 (SymbOS) - network layer for the SymbOS Network Daemon
;
; Replaces SE Play's direct TMTLogic TNC hardware access (I/O ports 40h-49h)
; with calls to the SymbOS Network Daemon ("Network Daem" shared service),
; e.g. the PicoVerse 2350 ESP-01 daemon netd-esp.exe, or any other SymbOS
; network daemon (M4, Net4CPC, DenYoNet, GR8NET...).
;
; Every routine keeps the interface of the routine it replaces, so SE Play's
; compiled Quigs code is unchanged apart from the jumps into this block.
;
;   TNC_GETID  (5071 area 4FECh)  -> net_find    HWID$="" + locate daemon
;   TNC_OPEN   (50CFh)            -> net_open    TCP connect, (7555h)=0/1
;                                               IP 7740h..7746h, port 774Ah/774Ch
;   TNC_SEND   (518Bh)            -> net_send    HTTP request (+Host header)
;   TNC_CLOSE  (5221h)            -> net_close
;   TNC_RXAVL  (5272h)            -> net_rxavail (75A9h)=bytes waiting
;   copy loop  (1D7Eh)            -> net_pump    daemon -> SE-ONE port 22h
;   phone page (2A45h)            -> phone_na    "PHONE: NO TNC"
;
; Assembled by sjasmplus; the relinker (seplay_netd.py) supplies the
; addresses of SE Play's symbols through ext.inc and derives the relocation
; table by assembling twice with shifted addresses.
;==============================================================================

        include "ext.inc"               ; ORG and SE Play symbols

FNC_NET_TCPOPN  equ 16
FNC_NET_TCPCLO  equ 17
FNC_NET_TCPSTA  equ 18
FNC_NET_TCPRCV  equ 19
FNC_NET_TCPSND  equ 20
MSC_SYS_PRGSRV  equ 30
MSR_SYS_PRGSRV  equ 158
PRC_ID_SYSTEM   equ 3

RXBUF_SIZE      equ 1024
REPLY_TIMEOUT   equ 1500                ; idle slices waiting for one daemon reply
OPEN_TIMEOUT    equ 1500                ; idle slices waiting for "established"

SEONE_STREAM    equ #FF22               ; SE-ONE MP3 stream port (CPC: &FF22)

        org BLOCK_ORG

;------------------------------------------------------------------------------
; net_find - replaces TNC_GETID: no network card ID, locate the daemon
;------------------------------------------------------------------------------
net_find
        ld hl,EMPTYSTR
        ld de,(V_HWID0)
        call STRASSIGN                  ; HWID$ = "" -> SE-ONE code paths
        jp net_lookup

;------------------------------------------------------------------------------
; net_lookup - find the network daemon process; (net_pid)=0 if not running
;------------------------------------------------------------------------------
net_lookup
        xor a
        ld (net_pid),a
        ld iy,MSGBUF
        ld (iy+0),MSC_SYS_PRGSRV
        ld hl,netd_name
        ld (MSGBUF+1),hl
        ld a,(BANK)
        ld (MSGBUF+3),a
        ld hl,0                         ; E=0: search application/shared service
        ld (MSGBUF+4),hl
        ld a,(PID)
        ld ixl,a
        ld ixh,PRC_ID_SYSTEM
        rst #10
        ld bc,REPLY_TIMEOUT
nlk_wait
        push bc
        ld a,(PID)
        ld ixl,a
        ld ixh,PRC_ID_SYSTEM
        ld iy,MSGBUF
        rst #18
        pop bc
        dec ixl
        jr z,nlk_got
        push bc
        rst #30
        pop bc
        dec bc
        ld a,b
        or c
        jr nz,nlk_wait
        ret                             ; no answer: no daemon
nlk_got ld a,(MSGBUF+0)
        cp MSR_SYS_PRGSRV
        jr nz,nlk_wait                  ; some other system message: ignore
        ld a,(MSGBUF+1)
        or a
        ret nz                          ; 5 = not found
        ld a,(MSGBUF+9)
        ld (net_pid),a
        ret

;------------------------------------------------------------------------------
; net_req - send a request to the daemon and wait for its answer
; Input   AF,BC,DE,HL,IX,IY = request registers, function code inline
;         after the CALL (db FNC_NET_xxx)
; Output  AF,BC,DE,HL,IX,IY = answer registers (CF=1 error, A=error code)
;------------------------------------------------------------------------------
net_req ld (MSGBUF+4),bc
        ld (MSGBUF+6),de
        ld (MSGBUF+8),hl
        ld (MSGBUF+10),ix
        ld (MSGBUF+12),iy
        push af
        pop hl
        ld (MSGBUF+2),hl
        pop hl                          ; inline function code
        ld a,(hl)
        inc hl
        push hl
        ld (MSGBUF+0),a
        add a,128
        ld (nrq_want),a
        ld a,(net_pid)
        or a
        jr nz,nrq_send
        ld a,1                          ; no daemon
        scf
        ret
nrq_send
        ld a,(PID)
        ld ixl,a
        ld a,(net_pid)
        ld ixh,a
        ld iy,MSGBUF
        rst #10
        ld bc,REPLY_TIMEOUT
nrq_wait
        push bc
        ld a,(PID)
        ld ixl,a
        ld a,(net_pid)
        ld ixh,a
        ld iy,MSGBUF
        rst #18                         ; answer from the daemon only
        pop bc
        dec ixl
        jr z,nrq_got
        push bc
        rst #30
        pop bc
        dec bc
        ld a,b
        or c
        jr nz,nrq_wait
        xor a                           ; timeout: daemon gone
        ld (net_pid),a
        ld a,5
        scf
        ret
nrq_got ld a,(nrq_want)
        ld hl,MSGBUF
        cp (hl)
        jr nz,nrq_wait                  ; TCP/UDP event message: skip
        ld bc,(MSGBUF+4)
        ld de,(MSGBUF+6)
        ld ix,(MSGBUF+10)
        ld iy,(MSGBUF+12)
        ld hl,(MSGBUF+2)
        push hl
        ld hl,(MSGBUF+8)
        pop af
        ret

;------------------------------------------------------------------------------
; net_open - replaces TNC_OPEN: connect to IP (7740h..7746h), port (774Ah)
; Output  HL=(7555h)=0 ok, 1 error
;------------------------------------------------------------------------------
net_open
        call net_close
        ld a,(net_pid)
        or a
        call z,net_lookup               ; daemon may have been started later
        ld a,(net_pid)
        or a
        jr z,nop_fail
        ld a,(V_IP+0)
        ld ixl,a
        ld a,(V_IP+2)
        ld ixh,a
        ld a,(V_IP+4)
        ld iyl,a
        ld a,(V_IP+6)
        ld iyh,a
        call get_port
        ex de,hl                        ; DE = remote port
        ld hl,-1                        ; random local port
        xor a                           ; client
        call net_req
        db FNC_NET_TCPOPN
        jr c,nop_fail
        ld (net_sock),a
        ld bc,OPEN_TIMEOUT
nop_wait
        push bc
        ld a,(net_sock)
        call net_req
        db FNC_NET_TCPSTA
        pop bc
        jr c,nop_fclose
        ld a,l
        and 127
        cp 2
        jr z,nop_ok                     ; established
        cp 3
        jr nc,nop_fclose                ; closing / closed: refused
        push bc
        rst #30
        pop bc
        dec bc
        ld a,b
        or c
        jr nz,nop_wait
nop_fclose
        call net_close
nop_fail
        ld hl,1
        ld (V_RESULT),hl
        ret
nop_ok  ld hl,0
        ld (V_RESULT),hl
        ret

;------------------------------------------------------------------------------
; net_close - replaces TNC_CLOSE
;------------------------------------------------------------------------------
net_close
        ld hl,0
        ld (V_RESULT),hl
        ld a,(net_sock)
        cp 255
        ret z
        ld a,(net_pid)
        or a
        jr z,ncl_done
        ld a,(net_sock)
        call net_req
        db FNC_NET_TCPCLO
ncl_done
        ld a,255
        ld (net_sock),a
        ld hl,0
        ret

;------------------------------------------------------------------------------
; net_send - replaces TNC_SEND: send the request string (75B9h).
; SE Play sends "GET <path> HTTP/1.1<CR><LF><CR><LF>" without a Host header,
; which current Icecast/Shoutcast servers reject. The first line is kept and
; "Host:" and "User-Agent:" headers are added.
; Output  HL=(7555h)=0 ok, 1 error
;------------------------------------------------------------------------------
net_send
        ld hl,0
        ld (V_RESULT),hl
        ld a,(net_sock)
        cp 255
        jp z,nsn_fail
        ld de,rxbuf                     ; build the request in rxbuf
        ld hl,(V_REQ)
        ld bc,RXBUF_SIZE-80
nsn_cpy ld a,(hl)
        or a
        jr z,nsn_eol
        cp 13
        jr z,nsn_eol
        ld (de),a
        inc hl
        inc de
        dec bc
        ld a,b
        or c
        jr nz,nsn_cpy
nsn_eol ld hl,txt_crlf_host
        call strcpy_de
        ld a,(V_IP+0)
        call dec8_de
        ld a,"."
        ld (de),a
        inc de
        ld a,(V_IP+2)
        call dec8_de
        ld a,"."
        ld (de),a
        inc de
        ld a,(V_IP+4)
        call dec8_de
        ld a,"."
        ld (de),a
        inc de
        ld a,(V_IP+6)
        call dec8_de
        call get_port
        ld bc,80
        or a
        sbc hl,bc
        jr z,nsn_p80
        ld a,":"
        ld (de),a
        inc de
        call get_port
        call dec16_de
nsn_p80 ld hl,txt_agent
        call strcpy_de
        ex de,hl                        ; HL = end of request
        ld de,rxbuf
        or a
        sbc hl,de
        ld b,h
        ld c,l                          ; BC = length
        ld hl,rxbuf
nsn_loop
        ld a,b
        or c
        ret z                           ; all sent, (7555h)=0
        push hl
        ld a,(BANK)
        ld e,a
        ld a,(net_sock)
        call net_req
        db FNC_NET_TCPSND
        pop de                          ; DE = address
        jr c,nsn_fail
        push hl                         ; remaining
        ex de,hl
        add hl,bc                       ; address += sent
        pop bc                          ; BC = remaining
        ld a,b
        or c
        ret z
        push hl
        push bc
        rst #30
        pop bc
        pop hl
        jr nsn_loop
nsn_fail
        ld hl,1
        ld (V_RESULT),hl
        ret

; HL = station port. SE Play keeps it split for the TNC protocol:
; (774Ah) = high byte, (774Ch) = low byte.
get_port
        ld a,(V_PORT)
        ld h,a
        ld a,(V_PORTLO)
        ld l,a
        ret

; copy 0-terminated string HL to DE (DE ends on the terminator, not copied)
strcpy_de
        ld a,(hl)
        or a
        ret z
        ld (de),a
        inc hl
        inc de
        jr strcpy_de

; A = 0..255 as decimal at DE
dec8_de ld l,a
        ld h,0
; HL = 0..65535 as decimal at DE, no leading zeros
dec16_de
        ld ix,dec_tab
        ld b,0                          ; B=1 once a digit was written
dec16_1 ld a,(ix+0)
        or (ix+1)
        ret z                           ; end of table
        push de
        ld e,(ix+0)
        ld d,(ix+1)
        ld a,"0"-1
dec16_2 inc a
        or a
        sbc hl,de
        jr nc,dec16_2
        add hl,de
        pop de
        cp "0"
        jr nz,dec16_3
        bit 0,b
        jr nz,dec16_3
        ld c,(ix+0)                     ; leading zero: skip unless divisor 1
        dec c
        jr nz,dec16_4
dec16_3 ld (de),a
        inc de
        ld b,1
dec16_4 inc ix
        inc ix
        jr dec16_1
dec_tab dw 10000,1000,100,10,1,0

;------------------------------------------------------------------------------
; net_rxavail - replaces TNC_RXAVAIL: (75A9h) = received bytes (max buffer)
;------------------------------------------------------------------------------
net_rxavail
        ld hl,0
        ld (V_AVAIL),hl
        ld (V_RESULT),hl
        ld a,(net_sock)
        cp 255
        ret z
        call net_req
        db FNC_NET_TCPSTA
        ld hl,0
        ret c
        ld h,b
        ld l,c
        ld de,RXBUF_SIZE
        or a
        sbc hl,de
        jr c,nrx_ok
        ld b,d
        ld c,e                          ; cap at the buffer size
nrx_ok  ld (V_AVAIL),bc
        ret

;------------------------------------------------------------------------------
; net_pump - replaces the copy loop at 1D7Eh: receive (75A9h) bytes from the
; daemon and write them to the SE-ONE MP3 stream port
;------------------------------------------------------------------------------
net_pump
        ld bc,(V_AVAIL)
        ld a,b
        or c
        ret z
        ld a,(net_sock)
        cp 255
        ret z
        ld hl,rxbuf
        ld a,(BANK)
        ld e,a
        ld a,(net_sock)
        call net_req
        db FNC_NET_TCPRCV
        ret c
        ld d,b
        ld e,c
        ld hl,rxbuf
        ld bc,SEONE_STREAM
npm_out ld a,d
        or e
        ret z
        ld a,(hl)
        inc hl
        out (c),a
        dec de
        jr npm_out

;------------------------------------------------------------------------------
; phone_na - the phone page needs TMTLogic's network card
;------------------------------------------------------------------------------
phone_na
        ld hl,txt_phone
        ld de,(V_DISP1)
        call STRASSIGN
        jp SHOW1

;------------------------------------------------------------------------------
; data
;------------------------------------------------------------------------------
netd_name       db "Network Daem"       ; 12 char shared-service ID
txt_crlf_host   db 13,10,"Host: ",0
txt_agent       db 13,10,"User-Agent: SEPlay/0.1 (SymbOS)",13,10,13,10,0
txt_phone       db "PHONE: NO TNC",0
nrq_want        db 0
net_pid         db 0
net_sock        db 255
                align 2
rxbuf           ds RXBUF_SIZE
BLOCK_END
