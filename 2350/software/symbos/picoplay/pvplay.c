/*
 * PicoPlay - music player for SymbOS on the MSX PicoVerse 2350
 *
 * The PicoVerse cartridge reads the music from its microSD card and decodes
 * it itself (PicoVerse Audio Engine, LoadROM -p1). This program only sends
 * short commands to the engine and shows its status block. It never waits
 * for the engine without giving the CPU back to SymbOS, and every wait has a
 * time limit, so a missing or stuck cartridge can not freeze the system.
 *
 * Build: cc pvplay.c pvwin.s -o PICOPLAY.EXE   (SymbOS C Compiler)
 */

#ifdef PV_HOST_TEST
#include "test/host_symbos.h"
#else
#include <symbos.h>
#endif
#include <string.h>
#include <stdio.h>
#include "pva_proto.h"

#define APP_VERSION "0.1"

/* ------------------------------------------------------------ engine access */
extern void pv_setslot(unsigned char prim, unsigned char sub);
extern unsigned char pv_peek(unsigned short addr);
extern void pv_poke(unsigned short addr, unsigned char value);
extern void pv_read(unsigned short addr, char *dst, unsigned short len);
extern void pv_write(unsigned short addr, char *src, unsigned short len);
extern void pv_post(unsigned char token, unsigned char code);
extern unsigned char pv_prim, pv_sub;

#define W(off) (PVA_WIN_BASE + (off))

_transfer char xbuf[PVA_REPLY_SIZE + 2];    /* copy buffer, must be in page 3 */
_transfer char stbuf[PVA_STATUS_SIZE];
char st[PVA_STATUS_SIZE];                   /* last consistent status block */
unsigned short reply_len;
unsigned char token;
unsigned char engine_ok;                    /* 0 = not found, 1 = ok, 2 = not answering */
unsigned char last_hb;
unsigned short hb_time;

#define ST16(o) ((unsigned short)(unsigned char)st[o] | ((unsigned short)(unsigned char)st[(o) + 1] << 8))

static unsigned char detect(void)
{
    static unsigned char prims[4] = { 1, 2, 0, 3 };
    static unsigned char subs[4] = { 3, 0, 1, 2 };
    unsigned char p, s;
    for (p = 0; p < 4; ++p) {
        for (s = 0; s < 4; ++s) {
            pv_setslot(prims[p], subs[s]);
            pv_read(W(PVA_R_ID), xbuf, 8);
            if (!memcmp(xbuf, "PVAUDIO", 8))
                return 1;
        }
    }
    return 0;
}

/* Send one command and wait (giving the CPU back) for its answer, 3 s at most.
 * Returns the engine result, or -1 when the engine did not answer. */
static int pv_cmd(unsigned char code, char *args, unsigned short alen)
{
    unsigned short t0;
    unsigned char r;
    if (engine_ok == 0)
        return -1;
    if (alen) {
        memcpy(xbuf, args, alen);
        pv_write(W(PVA_R_ARGS), xbuf, alen);
    }
    ++token;
    pv_post(token, code);
    t0 = Sys_Counter16();
    while (pv_peek(W(PVA_R_DONE_TOKEN)) != token) {
        if ((unsigned short)(Sys_Counter16() - t0) > 300) {
            engine_ok = 2;
            return -1;
        }
        Idle();
    }
    engine_ok = 1;
    r = pv_peek(W(PVA_R_RESULT));
    reply_len = pv_peek(W(PVA_R_REPLY_LEN)) | ((unsigned short)pv_peek(W(PVA_R_REPLY_LEN + 1)) << 8);
    if (reply_len > PVA_REPLY_SIZE)
        reply_len = PVA_REPLY_SIZE;
    if (reply_len)
        pv_read(W(PVA_R_REPLY), xbuf, reply_len);
    xbuf[reply_len] = 0;
    return r;
}

static int pv_cmd_u16(unsigned char code, unsigned short v)
{
    char a[2];
    a[0] = (char)v;
    a[1] = (char)(v >> 8);
    return pv_cmd(code, a, 2);
}

static int pv_cmd_u8(unsigned char code, unsigned char v)
{
    return pv_cmd(code, (char *)&v, 1);
}

static unsigned char read_status(void)
{
    unsigned char i, s1;
    for (i = 0; i < 5; ++i) {
        s1 = pv_peek(W(PVA_R_STATUS + PVA_ST_SEQ));
        pv_read(W(PVA_R_STATUS), stbuf, PVA_STATUS_SIZE);
        if ((unsigned char)stbuf[0] == s1 && !(s1 & 1)) {
            memcpy(st, stbuf, PVA_STATUS_SIZE);
            return 1;
        }
    }
    return 0;
}

#ifdef PV_HOST_TEST
#include "test/host_gui.h"
#else
#include "pvgui.h"
#endif

signed char win_id = -1;
signed char text_id = -1;
signed char timer_id = -1;

/* Every way out must stop the wake timer first: it runs in our memory. */
static void app_quit(void)
{
    if (timer_id >= 0)
        Timer_Delete(timer_id);
    timer_id = -1;
    exit(0);
}

/* ------------------------------------------------------------ helpers */
static void set_status(char *s)
{
    strncpy(txt_status, s, TXT_STATUS_SIZE - 1);
    txt_status[TXT_STATUS_SIZE - 1] = 0;
    if (win_id >= 0)
        Win_Redraw_Status(win_id);
}

static void error_status(char *what, int r)
{
    char t[80];
    if (r < 0)
        sprintf(t, "%s: the engine does not answer", what);
    else if (r == PVA_E_NOCARD)
        sprintf(t, "%s: microSD not mounted (FRESULT %u)", what, (unsigned char)st[PVA_ST_MOUNT_FR]);
    else if (r == PVA_E_NOTFOUND)
        sprintf(t, "%s: not found", what);
    else if (r == PVA_E_FORMAT)
        sprintf(t, "%s: not a playable file", what);
    else if (r == PVA_E_IO)
        sprintf(t, "%s: read error (FRESULT %u)", what, (unsigned char)st[PVA_ST_FRESULT]);
    else
        sprintf(t, "%s: error %d", what, r);
    set_status(t);
}

static void fmt_time(char *d, unsigned short s)
{
    if (s >= 3600)
        sprintf(d, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    else
        sprintf(d, "%02u:%02u", s / 60, s % 60);
}

static void redraw(unsigned char c)
{
    if (win_id >= 0)
        Win_Redraw(win_id, c, 0);
}

/* ------------------------------------------------------------ folder list */
static void fill_page(void)
{
    unsigned short idx, got, p;
    unsigned char r = 0, i, l, type;
    unsigned long size;
    char a[3];
    if (page_first > 0) {
        strcpy(row_name[r], "<< previous entries");
        row_info[r][0] = 0;
        row_entry[r] = -1;
        row_type[r] = 0;
        ++r;
    }
    idx = page_first;
    while (idx < dir_count && r < LIST_PAGE + (page_first > 0 ? 1 : 0)) {
        a[0] = (char)idx;
        a[1] = (char)(idx >> 8);
        a[2] = 30;
        if (pv_cmd(PVA_C_DIR_READ, a, 3) != PVA_OK || reply_len == 0)
            break;
        got = 0;
        for (p = 0; p + 6 <= reply_len && idx < dir_count && r < LIST_PAGE + (page_first > 0 ? 1 : 0); ) {
            type = (unsigned char)xbuf[p];
            size = (unsigned char)xbuf[p + 1] | ((unsigned long)(unsigned char)xbuf[p + 2] << 8) |
                   ((unsigned long)(unsigned char)xbuf[p + 3] << 16) | ((unsigned long)(unsigned char)xbuf[p + 4] << 24);
            l = (unsigned char)xbuf[p + 5];
            if (type == PVA_DE_DIR || type == PVA_DE_PARENT) {
                row_name[r][0] = '[';
                for (i = 0; i < l && i < NAME_LEN - 2; ++i)
                    row_name[r][i + 1] = xbuf[p + 6 + i];
                row_name[r][i + 1] = ']';
                row_name[r][i + 2] = 0;
                strcpy(row_info[r], type == PVA_DE_PARENT ? "up" : "dir");
            } else {
                for (i = 0; i < l && i < NAME_LEN; ++i)
                    row_name[r][i] = xbuf[p + 6 + i];
                row_name[r][i] = 0;
                if (type == PVA_DE_M3U)
                    strcpy(row_info[r], "list");
                else if (size >= 10240000UL)
                    sprintf(row_info[r], "%luM", size >> 20);
                else
                    sprintf(row_info[r], "%luK", (size + 1023) >> 10);
            }
            row_entry[r] = (short)idx;
            row_type[r] = type;
            ++r;
            ++idx;
            ++got;
            p += 6 + l;
        }
        if (!got)
            break;
    }
    if (idx < dir_count) {
        strcpy(row_name[r], ">> more entries");
        row_info[r][0] = 0;
        row_entry[r] = -2;
        row_type[r] = 0;
        ++r;
    }
    for (i = 0; i < r; ++i) {
        rows[i].flags = i;
        rows[i].value1 = row_name[i];
        rows[i].value2 = row_info[i];
    }
    if (r)
        rows[0].flags |= ROW_MARKED;
    cd_list.lines = r;
    cd_list.scroll = 0;
    cd_list.clicked = 0;
    redraw(C_LIST);
}

/* keyboard selection: move the marked row and keep it in view */
#define LIST_VISIBLE 9
static void select_row(short n)
{
    unsigned short i;
    if (!cd_list.lines)
        return;
    if (n < 0)
        n = 0;
    if (n >= (short)cd_list.lines)
        n = cd_list.lines - 1;
    for (i = 0; i < cd_list.lines; ++i)
        rows[i].flags &= ~ROW_MARKED;
    rows[n].flags |= ROW_MARKED;
    cd_list.clicked = n;
    if (n < (short)cd_list.scroll)
        cd_list.scroll = n;
    else if (n >= (short)(cd_list.scroll + LIST_VISIBLE))
        cd_list.scroll = n - LIST_VISIBLE + 1;
    redraw(C_LIST);
}

static void show_folder_reply(void)
{
    dir_count = (unsigned char)xbuf[0] | ((unsigned short)(unsigned char)xbuf[1] << 8);
    strncpy(txt_path, xbuf + 2, TXT_PATH_SIZE - 1);
    txt_path[TXT_PATH_SIZE - 1] = 0;
    redraw(C_PATH);
    page_first = 0;
    fill_page();
}

static void open_folder(char *path)
{
    int r = pv_cmd(PVA_C_DIR_OPEN, path, strlen(path) + 1);
    if (r != PVA_OK) {
        error_status("Folder", r);
        return;
    }
    show_folder_reply();
}

static void activate_row(unsigned short row)
{
    short e;
    int r;
    if (row >= cd_list.lines)
        return;
    e = row_entry[row];
    if (e == -1) {
        page_first = page_first > LIST_PAGE ? page_first - LIST_PAGE : 0;
        fill_page();
        return;
    }
    if (e == -2) {
        page_first += LIST_PAGE;
        fill_page();
        return;
    }
    if (row_type[row] == PVA_DE_DIR || row_type[row] == PVA_DE_PARENT) {
        r = pv_cmd_u16(PVA_C_DIR_ENTER, (unsigned short)e);
        if (r != PVA_OK)
            error_status("Folder", r);
        else
            show_folder_reply();
        return;
    }
    set_status("Starting...");
    r = pv_cmd_u16(PVA_C_PLAY_INDEX, (unsigned short)e);
    if (r != PVA_OK)
        error_status("Play", r);
}

/* ------------------------------------------------------------ status display */
unsigned short shown_track = 0xFFFF, shown_elapsed = 0xFFFF, shown_total = 0xFFFF;
unsigned char shown_state = 0xFF, shown_prog = 0xFF, shown_mode = 0xFF, shown_vol = 0xFF;

/* copy 'src' into a control text and redraw it, only when it changed */
static void put_text(char *dst, unsigned short size, char *src, unsigned char ctl)
{
    if (!strncmp(dst, src, size - 1))
        return;
    strncpy(dst, src, size - 1);
    dst[size - 1] = 0;
    redraw(ctl);
}

static char *net_reason(unsigned char e)
{
    switch (e) {
    case PVA_NERR_NOWIFI:  return "WiFi not connected";
    case PVA_NERR_DNS:     return "host not found";
    case PVA_NERR_CONNECT: return "connection refused";
    case PVA_NERR_HTTP:    return "server error";
    case PVA_NERR_FORMAT:  return "not MP3 (AAC/Ogg/HLS)";
    case PVA_NERR_TIMEOUT: return "no answer";
    case PVA_NERR_CLOSED:  return "closed by the server";
    case PVA_NERR_URL:     return "bad address";
    }
    return "network error";
}

static unsigned char shown_err;
/* Streams change their buffer level and rate all the time. Each redraw costs
   the desktop a lot of time on a Z80, and when the desktop's message queue is
   full a redraw request waits (spinning) for room: the desktop locks up. So
   those details are refreshed at most once a second, in coarse steps. */
static unsigned char slow_ticks, shown_rate_ticks;

static void update_display(void)
{
    char t[84], t2[12];
    unsigned short el, tot;
    unsigned char state, prog, flags, stream, bufp;
    unsigned short tid;

    if (!read_status())
        return;
    state = (unsigned char)st[PVA_ST_STATE];
    flags = (unsigned char)st[PVA_ST_FLAGS];
    stream = (flags & PVA_F_STREAM) != 0;
    tid = ST16(PVA_ST_TRACK_ID);
    el = ST16(PVA_ST_ELAPSED);
    tot = ST16(PVA_ST_TOTAL);
    if (stream && el == shown_elapsed && tid == shown_track && state == shown_state && ++slow_ticks < 5)
        return;                                 /* nothing that must be shown now */
    slow_ticks = 0;
    bufp = (unsigned char)((((unsigned char)st[PVA_ST_BUF_PCT] + 2) / 5) * 5);

    /* title, second line, format */
    if (state == PVA_STATE_STOPPED && st[PVA_ST_TITLE] == 0) {
        put_text(txt_title, TXT_TITLE_SIZE, "No track", C_TITLE);
        put_text(txt_artist, TXT_ARTIST_SIZE, "", C_ARTIST);
        put_text(txt_fmt, TXT_FMT_SIZE, "", C_FMT);
    } else {
        put_text(txt_title, TXT_TITLE_SIZE, st + PVA_ST_TITLE, C_TITLE);
        t[0] = 0;
        if (st[PVA_ST_ARTIST]) {
            strcpy(t, st + PVA_ST_ARTIST);
            if (st[PVA_ST_ALBUM]) {
                strcat(t, " - ");
                strcat(t, st + PVA_ST_ALBUM);
            }
        } else if (stream && strcmp(st + PVA_ST_ALBUM, st + PVA_ST_TITLE))
            strcpy(t, st + PVA_ST_ALBUM);           /* the station under the stream title */
        put_text(txt_artist, TXT_ARTIST_SIZE, t, C_ARTIST);
        if (stream && (flags & PVA_F_BUFFERING))
            sprintf(t, "Buffering %u%%", bufp);
        else if (stream)
            sprintf(t, "%uk %u.%ukHz %u%%", ST16(PVA_ST_BITRATE), ST16(PVA_ST_RATE) / 100,
                    (ST16(PVA_ST_RATE) / 10) % 10, bufp);
        else
            sprintf(t, "%s %uk %u.%ukHz%s", st[PVA_ST_FORMAT] == PVA_DE_WAV ? "WAV" : "MP3",
                    ST16(PVA_ST_BITRATE), ST16(PVA_ST_RATE) / 100, (ST16(PVA_ST_RATE) / 10) % 10,
                    st[PVA_ST_CHANNELS] == 1 ? " mono" : "");
        put_text(txt_fmt, TXT_FMT_SIZE, t, C_FMT);
    }

    /* play button, status bar */
    if (state != shown_state) {
        strcpy(txt_play, state == PVA_STATE_PLAYING ? "Pause" : "Play");
        redraw(C_PLAY);
    }
    t[0] = 0;
    if (state == PVA_STATE_PLAYING) {
        sprintf(t, "Playing %u of %u", ST16(PVA_ST_PL_POS) + 1, ST16(PVA_ST_PL_COUNT));
        if (stream) {
            unsigned char ns = (unsigned char)st[PVA_ST_NET_STATE];
            if (ns == PVA_NET_LOOKUP || ns == PVA_NET_CONNECT || ns == PVA_NET_REQUEST)
                strcat(t, ns == PVA_NET_CONNECT ? " - connecting" : " - opening");
            else if (ns == PVA_NET_RETRY)
                strcat(t, " - reconnecting");
            else if (++shown_rate_ticks >= 2 || !strstr(txt_status, "KB/s")) {
                unsigned short need = (unsigned short)(ST16(PVA_ST_BITRATE) * 10U / 8U);   /* 0.1 KB/s */
                shown_rate_ticks = 0;           /* the rate: every 2 s */
                if (ST16(PVA_ST_UNDERRUNS) && ST16(PVA_ST_NET_RATE) < need - need / 20U)
                    /* the link is slower than the stream (a far server): say so */
                    sprintf(t + strlen(t), " - too slow: %u.%u of %u.%u KB/s", ST16(PVA_ST_NET_RATE) / 10,
                            ST16(PVA_ST_NET_RATE) % 10, need / 10, need % 10);
                else
                    sprintf(t + strlen(t), " - %u.%u KB/s", ST16(PVA_ST_NET_RATE) / 10, ST16(PVA_ST_NET_RATE) % 10);
            } else
                t[0] = 0;                       /* keep the status as it is */
        }
    } else if (state == PVA_STATE_PAUSED)
        strcpy(t, "Paused");
    else if ((unsigned char)st[PVA_ST_ERROR] == PVA_E_NET) {
        sprintf(t, "Stream stopped: %s", net_reason((unsigned char)st[PVA_ST_NET_ERR]));
        if ((unsigned char)st[PVA_ST_NET_ERR] == PVA_NERR_HTTP)
            sprintf(t + strlen(t), " %u", ST16(PVA_ST_HTTP));
    } else if (shown_state != 0xFF && (shown_state != state || shown_err == PVA_E_NET))
        strcpy(t, "Stopped");
    shown_err = (unsigned char)st[PVA_ST_ERROR];
    if (t[0] && strcmp(t, txt_status))
        set_status(t);

    /* time and progress (a stream shows its buffer fill) */
    if (el != shown_elapsed || tot != shown_total || tid != shown_track || stream) {
        if (state == PVA_STATE_STOPPED && st[PVA_ST_TITLE] == 0)
            strcpy(t, "--:-- / --:--");
        else {
            fmt_time(t, el);
            if (tot) {
                fmt_time(t2, tot);
                strcat(t, " / ");
                strcat(t, t2);
            }
        }
        put_text(txt_time, TXT_TIME_SIZE, t, C_TIME);
        if (stream)
            prog = (unsigned char)(((unsigned short)bufp * 255U) / 100U);
        else
            prog = tot ? (unsigned char)(((unsigned long)el * 255UL) / tot) : 0;
        if (prog != shown_prog) {
            ctl_prog.param = ((unsigned short)prog << 8) | PROG_COLORS;
            redraw(C_PROG);
            shown_prog = prog;
        }
    }
    if ((unsigned char)st[PVA_ST_MODE] != shown_mode) {
        shown_mode = (unsigned char)st[PVA_ST_MODE];
        chk_shuffle = (shown_mode & PVA_MODE_SHUFFLE) ? 1 : 0;
        chk_repeat = (shown_mode & PVA_MODE_REPEAT_ALL) ? 1 : 0;
        redraw(C_SHUF);
        redraw(C_REP);
    }
    if ((unsigned char)st[PVA_ST_VOLUME] != shown_vol) {
        shown_vol = (unsigned char)st[PVA_ST_VOLUME];
        cd_volume.value = shown_vol;
        redraw(C_VOL);
    }
    shown_track = tid;
    shown_state = state;
    shown_elapsed = el;
    shown_total = tot;
}

/* called 5 times per second */
/* A status redraw asked for right after Win_Open, before the desktop has
   drawn the window, is lost; repeat it on the first timer ticks. */
unsigned char status_redraws = 2;

static void tick(void)
{
    unsigned char hb;
    if (status_redraws) {
        --status_redraws;
        Win_Redraw_Status(win_id);
    }
    if (engine_ok == 0)
        return;
    hb = pv_peek(W(PVA_R_HEARTBEAT));
    if (hb != last_hb) {
        last_hb = hb;
        hb_time = Sys_Counter16();
        if (engine_ok == 2) {
            engine_ok = 1;
            set_status("Engine answering again");
        }
    } else if ((unsigned short)(Sys_Counter16() - hb_time) > 200) {
        if (engine_ok == 1)
            set_status("The engine has stopped running (no heartbeat)");
        engine_ok = 2;
        return;
    }
    update_display();
}

/* ------------------------------------------------------------ text window */
static void show_text(char *title, char *text)
{
    unsigned char n = 0, c = 0;
    char *p = text;
    while (*p && n < TEXT_LINES) {
        if (*p == 13) {
            ++p;
            continue;
        }
        if (*p == 10 || c >= TEXT_LEN) {
            tline[n][c] = 0;
            ++n;
            c = 0;
            if (*p == 10)
                ++p;
            continue;
        }
        tline[n][c++] = *p++;
    }
    if (c && n < TEXT_LINES) {
        tline[n][c] = 0;
        ++n;
    }
    for (c = 0; c < n; ++c) {
        trows[c].flags = c;
        trows[c].value = tline[c];
    }
    cd_tlist.lines = n;
    cd_tlist.scroll = 0;
    strcpy(txt_twin, title);
    if (text_id >= 0) {
        Win_Redraw(text_id, -1, 0);
        Win_Redraw_Title(text_id);
        Win_Focus(text_id);
    } else
        text_id = Win_Open(_symbank, &win_text);
}

static void menu_action(unsigned short v)
{
    int r;
    switch (v) {
    case M_REFRESH:
        open_folder("");
        break;
    case M_REMOUNT:
        r = pv_cmd(PVA_C_MOUNT, 0, 0);
        if (r == PVA_OK) {
            set_status("microSD mounted");
            open_folder("/");
        } else
            error_status("Mount", r);
        break;
    case M_DIAG:
        r = pv_cmd(PVA_C_DIAG, 0, 0);
        if (r == PVA_OK)
            show_text("Diagnostics", xbuf);
        else
            error_status("Diagnostics", r);
        break;
    case M_LOG:
        r = pv_cmd(PVA_C_LOG, 0, 0);
        if (r == PVA_OK)
            show_text("Engine log", xbuf);
        else
            error_status("Log", r);
        break;
    case M_ABOUT:
        /* Not MsgBox: it blocks this loop until the box is closed, while the
           wake timer keeps posting messages; on SymbOS 4.0 that stalls the
           desktop (half-drawn box, frozen mouse and keys). */
        show_text("About PicoPlay",
                  "PicoPlay " APP_VERSION " for SymbOS\n"
                  "Music player for the MSX PicoVerse 2350\n"
                  "The cartridge decodes and plays, SymbOS stays free.\n"
                  "\n"
                  "Up/Down select, Left/Right page, Enter open/play\n"
                  "Backspace parent folder\n"
                  "Space play/pause, N next, P previous, S stop\n"
                  "+/- volume, D diagnostics, L engine log");
        break;
    case M_QUIT:
        app_quit();
    }
}

static void control_click(unsigned char ctrl, unsigned char sub, unsigned short mx)
{
    int r = PVA_OK;
    unsigned short tot, sec;
    unsigned char m;
    switch (ctrl) {
    case C_PLAY:
        if (shown_state == PVA_STATE_STOPPED && cd_list.lines && ST16(PVA_ST_PL_COUNT) == 0) {
            activate_row(cd_list.clicked);
            return;
        }
        r = pv_cmd(PVA_C_PAUSE, 0, 0);
        break;
    case C_STOP: r = pv_cmd(PVA_C_STOP, 0, 0); break;
    case C_NEXT: r = pv_cmd(PVA_C_NEXT, 0, 0); break;
    case C_PREV: r = pv_cmd(PVA_C_PREV, 0, 0); break;
    case C_SHUF:
    case C_REP:
        m = (chk_shuffle ? PVA_MODE_SHUFFLE : 0) | (chk_repeat ? PVA_MODE_REPEAT_ALL : 0);
        r = pv_cmd_u8(PVA_C_MODE, m);
        break;
    case C_VOL:
        r = pv_cmd_u8(PVA_C_VOLUME, (unsigned char)cd_volume.value);
        break;
    case C_PROG:
        tot = ST16(PVA_ST_TOTAL);
        if (tot && (st[PVA_ST_FLAGS] & PVA_F_SEEKABLE) && mx >= PROG_X) {
            sec = (unsigned short)(((unsigned long)(mx - PROG_X) * tot) / PROG_W);
            r = pv_cmd_u16(PVA_C_SEEK, sec);
        }
        break;
    case C_LIST:
        if (sub == DSK_SUB_MDCLICK)
            activate_row(cd_list.clicked);
        return;
    default:
        return;
    }
    if (r != PVA_OK && r != PVA_E_STATE)
        error_status("Command", r);
    tick();
}

static void key_press(unsigned char k)
{
    switch (k) {
    case ' ': control_click(C_PLAY, 0, 0); break;
    case 'n': case 'N': control_click(C_NEXT, 0, 0); break;
    case 'p': case 'P': control_click(C_PREV, 0, 0); break;
    case 's': case 'S': control_click(C_STOP, 0, 0); break;
    case 13: activate_row(cd_list.clicked); break;
    case KEY_UP: select_row((short)cd_list.clicked - 1); break;
    case KEY_DOWN: select_row((short)cd_list.clicked + 1); break;
    case KEY_LEFT: select_row((short)cd_list.clicked - LIST_VISIBLE); break;
    case KEY_RIGHT: select_row((short)cd_list.clicked + LIST_VISIBLE); break;
    case 'd': case 'D': menu_action(M_DIAG); break;
    case 'a': case 'A': menu_action(M_ABOUT); break;
    case 'l': case 'L': menu_action(M_LOG); break;
    case 8:                                     /* backspace: parent folder */
        if (cd_list.lines && row_type[0] == PVA_DE_PARENT)
            activate_row(0);
        break;
    case '+':
        cd_volume.value = cd_volume.value <= 95 ? cd_volume.value + 5 : 100;
        control_click(C_VOL, 0, 0);
        break;
    case '-':
        cd_volume.value = cd_volume.value >= 5 ? cd_volume.value - 5 : 0;
        control_click(C_VOL, 0, 0);
        break;
    }
}

/* ------------------------------------------------------------ main */
int main(int argc, char *argv[])
{
    unsigned char wid;
    char t[80];
    int r;

    gui_texts_init();
    win_id = Win_Open(_symbank, &win_main);
    if (win_id < 0)
        return 1;

    engine_ok = detect();
    if (!engine_ok) {
        set_status("PicoVerse Audio Engine not found");
        MsgBox("The PicoVerse Audio Engine was not found.",
               "Flash a LoadROM image built with -p1 (or -p1 -w)",
               "and start SymbOS again.", COLOR_BLACK, BUTTON_OK | TITLE_ERROR, 0, 0, &win_main);
    } else {
        token = pv_peek(W(PVA_R_DONE_TOKEN));
        last_hb = pv_peek(W(PVA_R_HEARTBEAT));
        hb_time = Sys_Counter16();
        r = pv_cmd(PVA_C_PING, 0, 0);
        if (r == PVA_OK) {
            sprintf(t, "%s in slot %u-%u", xbuf, pv_prim, pv_sub);
            set_status(t);
            read_status();
            open_folder("");
        } else
            error_status("Engine", r);
    }

    timer_id = Timer_Wake(_sympid, 200, 10);

    while (1) {
        _symmsg[0] = 0;
        Msg_Sleep(_sympid, -1, _symmsg);
        if ((unsigned char)_symmsg[0] == 200) {
            tick();
            continue;
        }
        if ((unsigned char)_symmsg[0] != MSR_DSK_WCLICK)
            continue;
        wid = (unsigned char)_symmsg[1];
        if (text_id >= 0 && wid == (unsigned char)text_id) {
            if (_symmsg[2] == DSK_ACT_CLOSE || (_symmsg[2] == DSK_ACT_CONTENT && _symmsg[8] == 2) ||
                (_symmsg[2] == DSK_ACT_KEY && (unsigned char)_symmsg[4] == KEY_ESC)) {
                Win_Close(text_id);
                text_id = -1;
            } else if (_symmsg[2] == DSK_ACT_KEY)
                key_press((unsigned char)_symmsg[4]);   /* player keys work here too */
            continue;
        }
        switch (_symmsg[2]) {
        case DSK_ACT_CLOSE:
            app_quit();
        case DSK_ACT_MENU:
            menu_action(*(unsigned short *)&_symmsg[8]);
            break;
        case DSK_ACT_CONTENT:
            control_click((unsigned char)_symmsg[8], (unsigned char)_symmsg[3], *(unsigned short *)&_symmsg[4]);
            break;
        case DSK_ACT_KEY:
            key_press((unsigned char)_symmsg[4]);
            break;
        }
    }
}
