// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// seone_cmd.c - SE-ONE emulation, core 1 command side.
//
// Executes the AT command set (port 0x20) and the REG-mode function set
// (ports 0x26/0x27) of the TMTLogic SE-ONE against the microSD card.
//
// Behaviour notes derived from SE Play 0.1 (SymbOS) and the SE-ONE manual:
//  * Replies are "AT+NAME=value<CR><LF>OK<CR><LF>" for queries and
//    "OK<CR><LF>" for actions. SE Play only reads up to the first CR/LF
//    (max 50 chars), keeps the text after '=' as the value, and polls the
//    port about 5000 times (~0.5 s) before giving up.
//  * SE Play removes every space from a command before sending it, so long
//    file names containing spaces cannot be sent verbatim. Names are therefore
//    matched case-insensitively against the real long name, the long name
//    with spaces removed, the 8.3 short name, and the name SE Play derives
//    itself from M3U rows (first 6 non-space chars + "~1" + extension).
//  * AT+MSDFILES streams "NAME<TAB>" entries through port 0x25, directories
//    prefixed with '/', terminated by <LF> and then 0x00. SE Play reads the
//    stream twice (directories, then files) and keeps at most 49 entries.
//  * AT+MSDPATH always receives an absolute path ("0:/DIR/SUB", root is
//    "0:" or "1:"). Both drive numbers map to the microSD card.
//  * AT+MSDM3UFILE / AT+MSDM3UGETROW=n (1-based) return raw playlist rows
//    and "#END#" past the end.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include "ff.h"
#include "seone_internal.h"

#define SEONE_FW_DATE_YMD  "20250306"     // firmware generation we answer as
#define SEONE_FW_DATE_DMY  "06032025"
#define SEONE_MAX_ENTRIES  256u
#define SEONE_TALK_QUEUE   64u
#define SEONE_SAMPLE_SLOTS 10u

// -----------------------------------------------------------------------
// Arena
// -----------------------------------------------------------------------
static uint8_t *arena_ptr;
static size_t   arena_left;

void *seone_arena_alloc(size_t size)
{
    size = (size + 7u) & ~(size_t)7u;
    if (size > arena_left)
        return NULL;
    void *p = arena_ptr;
    arena_ptr += size;
    arena_left -= size;
    memset(p, 0, size);
    return p;
}

// -----------------------------------------------------------------------
// State
// -----------------------------------------------------------------------
typedef struct {
    char name[SEONE_LIST_NAME_MAX + 22];   // display name (no '/' prefix)
    bool dir;
} list_entry_t;

static FATFS    g_fs;
static bool     g_mounted;
static uint8_t  g_fault;
static char     g_cwd[SEONE_PATH_MAX];          // real path below the root, "" = root
static char     g_cwd_client[SEONE_PATH_MAX];   // as last given to AT+MSDPATH
static char     g_play_name[SEONE_PATH_MAX];
static char     g_m3u[SEONE_PATH_MAX];          // real path of the loaded playlist
static char     g_m3u_dir[SEONE_PATH_MAX];
static bool     g_m3u_loaded;
static char     g_sample[SEONE_SAMPLE_SLOTS][16];
static uint8_t  g_talk_queue[SEONE_TALK_QUEUE];
static uint8_t  g_talk_head, g_talk_tail;
static uint8_t  g_vol_l = 85, g_vol_r = 85, g_bass = 50, g_treble = 50, g_stereo = 1;
static bool     g_dsp_mute;
static uint8_t  g_dsp_select = 1;               // 0 = FM chip, 1 = MP3 chip
static uint8_t  g_vu_max = 8;
static uint32_t g_low_level = 2048, g_high_level = 28672;
static uint16_t g_fm_freq = 8750;               // MHz * 100
static uint32_t g_handled_cmd_seq;
static uint32_t g_handled_reg_seq;
static list_entry_t *g_entries;
static uint32_t *g_core1_stack;

static const char *const fault_text[] = {
    "Succeeded", "Disk IO errror", "Assertion failed", "The physical drive cannot work",
    "Could not find the file", "File not found", "Invalid path name",
    "Access denied directory full", "Access denied", "Invalid  file/directory",
    "Write protected", "Invalid logical drive", "The volume has no work area",
    "No valid FAT volume", "The f_mkfs() aborted", "Could not to access volume",
    "Rejected according  sharing policy", "LFN could not be allocated",
    "Number of open files", "Invalid parameter", "Unknow at commando", "Syntax error",
    "Usb busy", "Invalid at command", "Usb already installed", "AT parameter error",
    "AT not in uppercase", "ARM flash error", "Invalid input", "Wrong SE mode",
};

enum { F_OK = 0, F_DISK = 1, F_NOFILE = 4, F_NOPATH = 5, F_BADPATH = 6, F_NOVOL = 13,
       F_UNKNOWN = 20, F_SYNTAX = 21, F_INVALID = 23, F_PARAM = 25, F_INPUT = 28 };

// Generic stored settings for commands we accept but cannot act on (FM tuner,
// analogue switches...). SET stores the text, GET returns it.
typedef struct { const char *name; char value[12]; bool read_only; } setting_t;
static setting_t g_settings[] = {
    { "FMSM", "OFF", false },     { "FMMUTE", "OFF", false },  { "FMPLL", "0", false },
    { "FMSUD", "UP", false },     { "FMSSL", "2", false },     { "FMHLSI", "HIGH", false },
    { "FMHISI", "HIGH", false },  { "FMMS", "STEREO", false }, { "FMBAND", "EUR", false },
    { "FMHCC", "ON", false },     { "FMSNC", "OFF", false },   { "FMSTARTFREQ", "87.5", false },
    { "FMREADY", "OFF", true },   { "FMBLF", "OFF", true },    { "FMSTEREO", "OFF", true },
    { "STEREO", "OFF", true },    { "FMIF", "0", true },       { "FMADC", "0", true },
    { "MSXAUDIO", "OFF", false }, { "MSXSTARTAUDIO", "OFF", false },
    { "SETUNE", "OFF", false },   { "TMTID", "0", false },
};

// -----------------------------------------------------------------------
// Small string helpers
// -----------------------------------------------------------------------
static int ci_cmp(const char *a, const char *b)
{
    for (;; a++, b++)
    {
        int ca = toupper((unsigned char)*a), cb = toupper((unsigned char)*b);
        if (ca != cb || ca == 0)
            return ca - cb;
    }
}

static void strip_spaces(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (; *in && o + 1 < out_size; in++)
        if (*in != ' ')
            out[o++] = *in;
    out[o] = 0;
}

static void copy_str(char *dst, const char *src, size_t dst_size)
{
    if (!dst_size) return;
    size_t n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

// The short name SE Play makes up from an M3U row (SEPLAY.EXE 0x274A):
// rows of up to 10 chars are used as-is, otherwise the first 6 non-space
// characters among the first 20, then "~1", then the last 4 characters.
static void seplay_derived_name(const char *name, char *out, size_t out_size)
{
    size_t len = strlen(name);
    if (len <= 10)
    {
        copy_str(out, name, out_size);
        return;
    }
    char tmp[16];
    size_t n = 0;
    for (size_t i = 0; i < len && i < 20 && n < 6; i++)
        if (name[i] != ' ')
            tmp[n++] = name[i];
    tmp[n] = 0;
    snprintf(out, out_size, "%s~1%s", tmp, name + len - 4);
}

static bool name_matches(const char *req, const char *candidate)
{
    char a[SEONE_PATH_MAX], b[SEONE_PATH_MAX];
    if (!candidate || !candidate[0])
        return false;
    if (ci_cmp(req, candidate) == 0)
        return true;
    strip_spaces(req, a, sizeof a);
    strip_spaces(candidate, b, sizeof b);
    if (ci_cmp(a, b) == 0)
        return true;
    seplay_derived_name(candidate, b, sizeof b);
    return ci_cmp(req, b) == 0;
}

static bool has_ext(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');
    return dot && ci_cmp(dot + 1, ext) == 0;
}

static bool is_playable(const char *name)
{
    return has_ext(name, "MP3") || has_ext(name, "WAV");
}

// Join "0:" + dir + "/" + name into out (dir is "" for root or "/A/B").
static void make_fatfs_path(char *out, size_t out_size, const char *dir, const char *name)
{
    if (name && name[0])
        snprintf(out, out_size, "0:%s/%s", dir, name);
    else
        snprintf(out, out_size, "0:%s%s", dir, dir[0] ? "" : "/");
}

// -----------------------------------------------------------------------
// Reply builder
// -----------------------------------------------------------------------
static char     r_buf[SEONE_RESP_MAX];
static uint16_t r_len;

static void r_reset(void) { r_len = 0; }

static void r_puts(const char *s)
{
    while (*s && r_len < SEONE_RESP_MAX - 1u)
        r_buf[r_len++] = *s++;
}

static void r_printf(const char *fmt, ...)
{
    char tmp[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    r_puts(tmp);
}

static void r_ok(void)                 { r_puts("OK\r\n"); }
static void r_err(uint8_t fault)       { g_fault = fault; r_puts("ERR\r\n"); }
static void r_ill(void)                { g_fault = F_INVALID; r_puts("ILL\r\n"); }

static void r_value(const char *name, const char *fmt, ...)
{
    char tmp[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    r_printf("AT+%s=%s\r\nOK\r\n", name, tmp);
}

static void update_status(void);

static void publish_response(uint32_t seq)
{
    update_status();               // status must be current before the reply
    seone.resp_len = 0;
    seone_barrier();
    memcpy(seone.resp, r_buf, r_len);
    seone_barrier();
    seone.resp_len = r_len;
    seone_barrier();
    seone.resp_seq = seq;
}

// -----------------------------------------------------------------------
// Storage
// -----------------------------------------------------------------------
static bool fs_mount(void)
{
    f_mount(NULL, "0:", 0);
    FRESULT fr = f_mount(&g_fs, "0:", 1);
    g_mounted = (fr == FR_OK);
    if (!g_mounted)
        g_fault = (fr == FR_NO_FILESYSTEM) ? F_NOVOL : F_DISK;
    return g_mounted;
}

// Nextor writes the card behind FatFS' back (raw sectors through the
// Sunrise IDE emulation), so drop FatFS' sector window before every command.
static void fs_refresh(void)
{
    if (g_mounted && !g_fs.wflag)
        g_fs.winsect = (LBA_t)0 - 1;
}

// Find 'req' inside directory 'dir' (real path, "" = root). On success the
// real entry name is copied to out_name.
static bool find_entry(const char *dir, const char *req, char *out_name, size_t out_size, bool *is_dir)
{
    char path[SEONE_PATH_MAX + 16];
    FILINFO fno;

    make_fatfs_path(path, sizeof path, dir, req);
    if (f_stat(path, &fno) == FR_OK)
    {
        copy_str(out_name, fno.fname, out_size);
        if (is_dir) *is_dir = (fno.fattrib & AM_DIR) != 0;
        return true;
    }

    DIR d;
    make_fatfs_path(path, sizeof path, dir, NULL);
    if (f_opendir(&d, path) != FR_OK)
        return false;
    bool found = false;
    while (f_readdir(&d, &fno) == FR_OK && fno.fname[0])
    {
        if (name_matches(req, fno.fname) || (fno.altname[0] && ci_cmp(req, fno.altname) == 0))
        {
            copy_str(out_name, fno.fname, out_size);
            if (is_dir) *is_dir = (fno.fattrib & AM_DIR) != 0;
            found = true;
            break;
        }
    }
    f_closedir(&d);
    return found;
}

// Resolve a client path to a real path. Absolute when it starts with a drive
// ("0:") or a slash, otherwise relative to the current directory.
static bool resolve_path(const char *client, char *real, size_t real_size, bool want_dir)
{
    const char *p = client;
    char cur[SEONE_PATH_MAX];

    if (isdigit((unsigned char)p[0]) && p[1] == ':')
    {
        p += 2;
        cur[0] = 0;
    }
    else if (*p == '/' || *p == '\\')
        cur[0] = 0;
    else
        copy_str(cur, g_cwd, sizeof cur);

    bool last_is_dir = true;
    while (*p)
    {
        while (*p == '/' || *p == '\\') p++;
        if (!*p) break;
        char comp[SEONE_PATH_MAX];
        size_t n = 0;
        while (*p && *p != '/' && *p != '\\' && n + 1 < sizeof comp)
            comp[n++] = *p++;
        comp[n] = 0;

        if (strcmp(comp, ".") == 0)
            continue;
        if (strcmp(comp, "..") == 0)
        {
            char *slash = strrchr(cur, '/');
            if (slash) *slash = 0;
            last_is_dir = true;
            continue;
        }
        if (!last_is_dir)
            return false;            // file in the middle of a path

        char real_name[SEONE_PATH_MAX];
        bool is_dir = false;
        if (!find_entry(cur, comp, real_name, sizeof real_name, &is_dir))
            return false;
        if (strlen(cur) + strlen(real_name) + 2 > sizeof cur)
            return false;
        strcat(cur, "/");
        strcat(cur, real_name);
        last_is_dir = is_dir;
    }
    if (want_dir && !last_is_dir)
        return false;
    if (!want_dir && last_is_dir)
        return false;
    copy_str(real, cur, real_size);
    return true;
}

// -----------------------------------------------------------------------
// Directory listing (port 0x25)
// -----------------------------------------------------------------------
static bool printable_ascii(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20u || (unsigned char)*s > 0x7Eu)
            return false;
    return true;
}

static int entry_cmp(const void *pa, const void *pb)
{
    const list_entry_t *a = pa, *b = pb;
    if (a->dir != b->dir)
        return a->dir ? -1 : 1;
    return ci_cmp(a->name, b->name);
}

static void publish_list(const uint8_t *data, uint32_t len)
{
    seone.list_len = 0;
    seone_barrier();
    if (data != seone.list)
        memcpy(seone.list, data, len);
    seone_barrier();
    seone.list_len = len;
    seone_barrier();
    seone.list_gen = seone.list_gen + 1u;
}

static bool build_listing(void)
{
    char path[SEONE_PATH_MAX + 4];
    DIR d;
    FILINFO fno;
    uint32_t count = 0;

    make_fatfs_path(path, sizeof path, g_cwd, NULL);
    if (f_opendir(&d, path) != FR_OK)
        return false;

    while (count < SEONE_MAX_ENTRIES && f_readdir(&d, &fno) == FR_OK && fno.fname[0])
    {
        if (fno.fattrib & (AM_HID | AM_SYS))
            continue;
        if (fno.fname[0] == '.')
            continue;                  // ".", "..", macOS "._*" files
        bool dir = (fno.fattrib & AM_DIR) != 0;
        if (!dir && !is_playable(fno.fname) && !has_ext(fno.fname, "M3U"))
            continue;

        const char *shown = fno.fname;
        if ((strlen(fno.fname) > SEONE_LIST_NAME_MAX || !printable_ascii(fno.fname)) && fno.altname[0])
            shown = fno.altname;

        list_entry_t *e = &g_entries[count++];
        copy_str(e->name, shown, sizeof e->name);
        e->dir = dir;
        if (!dir)
        {
            // SE Play recognises playlists with RIGHT$(name,4) = ".M3U"
            char *dot = strrchr(e->name, '.');
            for (; dot && *dot; dot++)
                *dot = (char)toupper((unsigned char)*dot);
        }
    }
    f_closedir(&d);

    qsort(g_entries, count, sizeof g_entries[0], entry_cmp);

    uint8_t *out = seone.list;
    uint32_t len = 0;
    seone.list_len = 0;
    seone_barrier();
    for (uint32_t i = 0; i < count; i++)
    {
        size_t n = strlen(g_entries[i].name);
        if (len + n + 4u >= SEONE_LIST_SIZE)
            break;
        if (g_entries[i].dir)
            out[len++] = '/';
        memcpy(out + len, g_entries[i].name, n);
        len += (uint32_t)n;
        out[len++] = '\t';
    }
    out[len++] = '\n';
    publish_list(out, len);
    return true;
}

// -----------------------------------------------------------------------
// M3U playlists
// -----------------------------------------------------------------------
// Walk the non-empty rows of the loaded playlist in one buffered pass.
// 'fn' returns true to stop. Rows are trimmed; a UTF-8 BOM is dropped.
typedef bool (*m3u_row_fn)(uint32_t row, char *text, void *ctx);

static bool m3u_iterate(m3u_row_fn fn, void *ctx)
{
    char path[SEONE_PATH_MAX + 4];
    static FIL f;
    static char chunk[512];
    static char line[SEONE_PATH_MAX];
    make_fatfs_path(path, sizeof path, g_m3u, NULL);
    if (f_open(&f, path, FA_READ) != FR_OK)
        return false;

    uint32_t row = 0;
    size_t n = 0;
    bool first = true, stopped = false;
    for (;;)
    {
        UINT br = 0;
        if (f_read(&f, chunk, sizeof chunk, &br) != FR_OK)
            br = 0;
        bool eof = (br == 0);
        for (UINT i = 0; i <= br && !stopped; i++)
        {
            bool end_of_line = (i == br) ? eof : (chunk[i] == '\n' || chunk[i] == '\r');
            if (i == br && !eof)
                break;
            if (!end_of_line)
            {
                if (n + 1 < sizeof line) line[n++] = chunk[i];
                continue;
            }
            line[n] = 0;
            n = 0;
            char *t = line;
            if (first && (uint8_t)t[0] == 0xEF && (uint8_t)t[1] == 0xBB && (uint8_t)t[2] == 0xBF)
                t += 3;
            first = false;
            while (*t == ' ' || *t == '\t') t++;
            size_t l = strlen(t);
            while (l && (t[l - 1] == ' ' || t[l - 1] == '\t')) t[--l] = 0;
            if (l && fn(++row, t, ctx))
                stopped = true;
        }
        if (eof || stopped)
            break;
    }
    f_close(&f);
    return stopped;
}

typedef struct { uint32_t want; char *out; size_t out_size; } m3u_get_ctx_t;

static bool m3u_get_cb(uint32_t row, char *text, void *ctx)
{
    m3u_get_ctx_t *c = ctx;
    if (row != c->want)
        return false;
    copy_str(c->out, text, c->out_size);
    return true;
}

// Read the n-th non-empty row (1-based) of the loaded playlist.
static bool m3u_get_row(uint32_t row, char *out, size_t out_size)
{
    m3u_get_ctx_t c = { row, out, out_size };
    return m3u_iterate(m3u_get_cb, &c);
}

typedef struct { const char *req; char *real; size_t real_size; } m3u_find_ctx_t;

static bool m3u_find_cb(uint32_t row, char *text, void *ctx)
{
    (void)row;
    m3u_find_ctx_t *c = ctx;
    if (text[0] == '#')
        return false;
    const char *base = text;
    for (const char *p = text; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    if (!name_matches(c->req, text) && !name_matches(c->req, base))
        return false;

    // Try the row as a path relative to the playlist, then its base name.
    char saved[SEONE_PATH_MAX];
    copy_str(saved, g_cwd, sizeof saved);
    copy_str(g_cwd, g_m3u_dir, sizeof g_cwd);
    bool ok = false;
    if (!(isalpha((unsigned char)text[0]) && text[1] == ':'))
        ok = resolve_path(text, c->real, c->real_size, false);
    if (!ok)
        ok = resolve_path(base, c->real, c->real_size, false);
    copy_str(g_cwd, saved, sizeof g_cwd);
    return ok;
}

// Map a name SE Play built from a playlist row back to that row's file.
static bool m3u_lookup(const char *req, char *real, size_t real_size)
{
    if (!g_m3u_loaded)
        return false;
    m3u_find_ctx_t c = { req, real, real_size };
    return m3u_iterate(m3u_find_cb, &c);
}

// Limit a row to SE Play's 50-character reply window while keeping the
// extension, so its derived short name still matches.
static void m3u_shorten_row(char *row)
{
    size_t l = strlen(row);
    if (l <= 50u)
        return;
    if (row[0] == '#')
        row[50] = 0;
    else
    {
        memmove(row + 46, row + l - 4, 5);
    }
}

// -----------------------------------------------------------------------
// Playback helpers
// -----------------------------------------------------------------------
static bool play_real(const char *real, int kind)
{
    char path[SEONE_PATH_MAX + 4];
    make_fatfs_path(path, sizeof path, real, NULL);
    return seone_audio_play_file(path, kind);
}

static bool play_request(const char *req)
{
    char real[SEONE_PATH_MAX];
    bool ok;
    if (strchr(req, '/') || strchr(req, '\\') || (isdigit((unsigned char)req[0]) && req[1] == ':'))
        ok = resolve_path(req, real, sizeof real, false);
    else
    {
        char name[SEONE_PATH_MAX];
        bool is_dir = false;
        ok = find_entry(g_cwd, req, name, sizeof name, &is_dir) && !is_dir;
        if (ok)
            snprintf(real, sizeof real, "%s/%s", g_cwd, name);
    }
    // Names SE Play derived from playlist rows can contain path separators
    // ("sub/03~1.mp3", "C:\Mus~1.mp3"): fall back to the loaded playlist.
    if (!ok)
        ok = m3u_lookup(req, real, sizeof real);
    if (!ok)
    {
        g_fault = F_NOFILE;
        return false;
    }
    if (!play_real(real, SEONE_PLAY_MUSIC))
    {
        g_fault = F_DISK;
        return false;
    }
    copy_str(g_play_name, req, sizeof g_play_name);
    return true;
}

static bool play_fx(const char *dir, const char *file)
{
    char real[SEONE_PATH_MAX];
    char name[SEONE_PATH_MAX];
    bool is_dir;
    if (!find_entry(dir, file, name, sizeof name, &is_dir) || is_dir)
        return false;
    snprintf(real, sizeof real, "%s/%s", dir, name);
    return play_real(real, SEONE_PLAY_FX);
}

static void talk_service(void)
{
    if (g_talk_head == g_talk_tail || seone_audio_fx_active())
        return;
    uint8_t code = g_talk_queue[g_talk_tail];
    g_talk_tail = (uint8_t)((g_talk_tail + 1u) % SEONE_TALK_QUEUE);
    char file[16];
    snprintf(file, sizeof file, "SP%03XH.MP3", code);
    if (!play_fx("/SETALK", file))
        g_fault = F_NOFILE;
}

static bool talk_push(uint32_t code)
{
    if (code > 0x3Fu)
        return false;
    uint8_t next = (uint8_t)((g_talk_head + 1u) % SEONE_TALK_QUEUE);
    if (next == g_talk_tail)
        return false;
    g_talk_queue[g_talk_head] = (uint8_t)code;
    g_talk_head = next;
    talk_service();
    return true;
}

static void talk_clear(void) { g_talk_head = g_talk_tail = 0; }

static bool sample_play(uint32_t slot)
{
    if (slot >= SEONE_SAMPLE_SLOTS || !g_sample[slot][0])
        return false;
    return play_fx("/SAMPLE", g_sample[slot]);
}

static void apply_dsp(void)
{
    seone_audio_set_volume(g_vol_l, g_vol_r);
    seone_audio_set_tone(g_bass, g_treble);
    seone_audio_set_mute(g_dsp_mute || g_dsp_select == 0);
    seone_audio_set_stereo_mode(g_stereo);
}

static void set_mode(uint8_t mode)
{
    seone.mode = mode;
    bool audio_path = (mode == SEONE_MODE_MP3A || mode == SEONE_MODE_MP3B ||
                       mode == SEONE_MODE_REG || mode == SEONE_MODE_TEST);
    if (!audio_path)
    {
        seone_audio_stop();
        talk_clear();
    }
    seone_audio_set_stream_enabled(mode == SEONE_MODE_MP3A || mode == SEONE_MODE_MP3B || mode == SEONE_MODE_REG);
}

static void update_status(void)
{
    uint8_t st = 0;
    if (!seone_audio_music_active())
        st |= SEONE_ST_END_OF_MUSIC;
    if (!g_mounted)
        st |= SEONE_ST_USB_ERROR;
    if (seone_audio_fx_active() || g_talk_head != g_talk_tail)
        st |= SEONE_ST_USB_BUSY;
    uint32_t fill = seone.ring_head - seone.ring_tail;
    if (fill >= g_high_level)
        st |= SEONE_ST_FIFO_FULL;
    if (fill <= g_low_level)
        st |= SEONE_ST_FIFO_EMPTY;
    seone.status = st;
}

static bool parse_uint(const char *s, uint32_t *out)
{
    if (!s || !*s) return false;
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*end) return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_onoff(const char *s, bool *out)
{
    if (ci_cmp(s, "ON") == 0 || strcmp(s, "1") == 0) { *out = true; return true; }
    if (ci_cmp(s, "OFF") == 0 || strcmp(s, "0") == 0) { *out = false; return true; }
    return false;
}

// FM frequency text "88.7" -> 8870
static bool parse_freq(const char *s, uint16_t *out)
{
    char *end;
    double f = strtod(s, &end);
    if (end == s || f < 76.0 || f > 108.95)
        return false;
    *out = (uint16_t)(f * 100.0 + 0.5);
    return true;
}

// -----------------------------------------------------------------------
// AT command handlers
// -----------------------------------------------------------------------
enum { OP_EXEC = 0, OP_SET, OP_GET, OP_HELP };
typedef void (*at_fn_t)(const char *name, int op, char *args);
typedef struct { const char *name; at_fn_t fn; const char *help; } at_cmd_t;

static void at_list(const char *name, int op, char *args);

static void at_cartridge(const char *name, int op, char *args)
{
    (void)args;
    if (op == OP_GET) r_value(name, "SEONE/M");
    else r_ill();
}

static void at_semode(const char *name, int op, char *args)
{
    static const char *const modes[] = { "MP3A", "MP3B", "FM", "REG", "TEST", "MIDI", "SYNTH", "VOICE" };
    if (op == OP_GET) { r_value(name, "%s", modes[seone.mode]); return; }
    if (op != OP_SET) { r_ill(); return; }
    for (uint8_t i = 0; i < sizeof modes / sizeof modes[0]; i++)
        if (ci_cmp(args, modes[i]) == 0)
        {
            set_mode(i);
            r_ok();
            return;
        }
    r_err(F_PARAM);
}

static void at_info(const char *name, int op, char *args)
{
    (void)args;
    if (op != OP_GET && op != OP_EXEC) { r_ill(); return; }
    if (strcmp(name, "SELOGBOOK") == 0)        r_value(name, "PICOVERSE SE-ONE EMULATION");
    else if (strcmp(name, "SEVERSION") == 0)   r_value(name, "2");
    else if (strcmp(name, "SEFIRMWARE") == 0)  r_value(name, "SEONE" SEONE_FW_DATE_DMY ".DFU");
    else if (strcmp(name, "SEFAULTNR") == 0)   r_value(name, "%u", g_fault);
    else if (strcmp(name, "SEFAULTTXT") == 0)  r_value(name, "%s", fault_text[g_fault < 30 ? g_fault : 0]);
    else if (strcmp(name, "MP3TYPE") == 0)     r_value(name, "VS1053");
    else if (strcmp(name, "DSPTYPE") == 0)     r_value(name, "TDA8425");
    else if (strcmp(name, "FMTYPE") == 0)      r_value(name, "NONE");
    else if (strcmp(name, "USBDEVICE") == 0)   r_value(name, g_mounted ? "MSD" : "NONE");
    else r_ill();
}

static void at_fwcheck(const char *name, int op, char *args)
{
    if (op != OP_SET || strlen(args) != 8) { r_err(F_PARAM); return; }
    // DDMMYYYY -> YYYYMMDD for comparison
    char want[9] = { args[4], args[5], args[6], args[7], args[2], args[3], args[0], args[1], 0 };
    r_value(name, strcmp(SEONE_FW_DATE_YMD, want) >= 0 ? "TRUE" : "FALSE");
}

static void at_simple_ok(const char *name, int op, char *args)
{
    (void)name; (void)op; (void)args;
    r_ok();
}

static void at_setting(const char *name, int op, char *args)
{
    for (unsigned i = 0; i < sizeof g_settings / sizeof g_settings[0]; i++)
    {
        setting_t *s = &g_settings[i];
        if (strcmp(s->name, name) != 0)
            continue;
        if (op == OP_GET) { r_value(name, "%s", s->value); return; }
        if (op == OP_SET && !s->read_only)
        {
            for (char *p = args; *p; p++) *p = (char)toupper((unsigned char)*p);
            copy_str(s->value, args, sizeof s->value);
            r_ok();
            return;
        }
        r_ill();
        return;
    }
    r_ill();
}

static void at_fm(const char *name, int op, char *args)
{
    if (strcmp(name, "FMFREQ") == 0 || strcmp(name, "SEFREQ") == 0)
    {
        if (op == OP_GET) { r_value("FMFREQ", "%u.%u", g_fm_freq / 100u, (g_fm_freq / 10u) % 10u); return; }
        if (op == OP_SET)
        {
            uint16_t f;
            if (!parse_freq(args, &f)) { r_err(F_PARAM); return; }
            g_fm_freq = f;
            r_ok();
            return;
        }
        r_ill();
        return;
    }
    if (strcmp(name, "FMSCANUP") == 0 || strcmp(name, "FMSCANDOWN") == 0)
    {
        bool up = name[6] == 'U';
        uint16_t f = (uint16_t)(g_fm_freq + (up ? 10 : -10));
        if (f < 7600 || f > 10800) f = up ? 7600 : 10800;
        g_fm_freq = f;
        r_ok();
        return;
    }
    if (strcmp(name, "FMGETRDS") == 0)
    {
        r_value(name, " ");
        return;
    }
    r_ill();
}

static void at_dsp_level(const char *name, int op, char *args)
{
    uint8_t *target;
    if (strcmp(name, "DSPVOLL") == 0) target = &g_vol_l;
    else if (strcmp(name, "DSPVOLR") == 0) target = &g_vol_r;
    else if (strcmp(name, "DSPBASS") == 0) target = &g_bass;
    else target = &g_treble;

    if (op == OP_GET) { r_value(name, "%u", *target); return; }
    uint32_t v;
    if (op != OP_SET || !parse_uint(args, &v) || v > 100u) { r_err(F_PARAM); return; }
    *target = (uint8_t)v;
    apply_dsp();
    r_ok();
}

static void at_dsp_step(const char *name, int op, char *args)
{
    (void)op; (void)args;
    int delta = (strcmp(name, "DSPVOLUP") == 0) ? 10 : -10;
    int l = g_vol_l + delta, r = g_vol_r + delta;
    g_vol_l = (uint8_t)(l < 0 ? 0 : l > 100 ? 100 : l);
    g_vol_r = (uint8_t)(r < 0 ? 0 : r > 100 ? 100 : r);
    apply_dsp();
    r_ok();
}

static void at_dsp_mute(const char *name, int op, char *args)
{
    if (op == OP_GET) { r_value(name, g_dsp_mute ? "ON" : "OFF"); return; }
    bool on;
    if (op != OP_SET || !parse_onoff(args, &on)) { r_err(F_PARAM); return; }
    g_dsp_mute = on;
    apply_dsp();
    r_ok();
}

static void at_mp3_level(const char *name, int op, char *args)
{
    uint32_t *target = (name[3] == 'L') ? &g_low_level : &g_high_level;
    if (op == OP_GET) { r_value(name, "%lu", (unsigned long)*target); return; }
    uint32_t v;
    if (op != OP_SET || !parse_uint(args, &v) || v > 65535u) { r_err(F_PARAM); return; }
    *target = v > SEONE_RING_SIZE ? SEONE_RING_SIZE : v;
    r_ok();
}

static void at_vu(const char *name, int op, char *args)
{
    if (strcmp(name, "VULEDS") == 0)
    {
        uint32_t v;
        if (op == OP_GET) { r_value(name, "%u", g_vu_max); return; }
        if (op != OP_SET || !parse_uint(args, &v) || v < 1u || v > 32u) { r_err(F_PARAM); return; }
        g_vu_max = (uint8_t)v;
        seone_audio_set_vu_max(g_vu_max);
        r_ok();
        return;
    }
    if (op == OP_SET) { r_ok(); return; }        // manual LED override: accepted, ignored
    if (op != OP_GET) { r_ill(); return; }
    if (strcmp(name, "VULEFT") == 0)       r_value(name, "%u", seone_audio_peak_left());
    else if (strcmp(name, "VURIGHT") == 0) r_value(name, "%u", seone_audio_peak_right());
    else if (strcmp(name, "VULN") == 0)    r_value(name, "%u", seone.vu_left);
    else                                   r_value(name, "%u", seone.vu_right);
}

static void at_usb(const char *name, int op, char *args)
{
    (void)op; (void)args;
    if (strcmp(name, "USBINIT") == 0)
    {
        if (g_mounted || fs_mount()) r_ok();
        else r_err(g_fault);
    }
    else if (strcmp(name, "USBDEINIT") == 0)
        r_ok();
    else
        r_err(F_INVALID);   // USBFLASH: no DFU here
}

static void at_msdpath(const char *name, int op, char *args)
{
    if (op == OP_GET)
    {
        r_value(name, "%s", g_cwd_client[0] ? g_cwd_client : "0:/");
        return;
    }
    if (strcmp(name, "MSDPATHUP") == 0)
    {
        char *slash = strrchr(g_cwd, '/');
        if (slash) *slash = 0;
        slash = strrchr(g_cwd_client, '/');
        if (slash) *slash = 0;
        r_ok();
        return;
    }
    if (op != OP_SET) { r_ill(); return; }
    if (!g_mounted && !fs_mount()) { r_err(g_fault); return; }
    char real[SEONE_PATH_MAX];
    if (!resolve_path(args, real, sizeof real, true)) { r_err(F_NOPATH); return; }
    copy_str(g_cwd, real, sizeof g_cwd);
    copy_str(g_cwd_client, args, sizeof g_cwd_client);
    r_ok();
}

static void at_msdfiles(const char *name, int op, char *args)
{
    (void)name; (void)op; (void)args;
    if (!g_mounted && !fs_mount()) { publish_list((const uint8_t *)"", 0); r_err(g_fault); return; }
    if (!build_listing()) { publish_list((const uint8_t *)"", 0); r_err(F_NOPATH); return; }
    r_ok();
}

static void at_msdplay(const char *name, int op, char *args)
{
    if (op == OP_GET) { r_value(name, seone_audio_music_active() ? "BUSY" : "IDLE"); return; }
    if (op != OP_SET || !args[0]) { r_err(F_PARAM); return; }
    if (!g_mounted && !fs_mount()) { r_err(g_fault); return; }
    if (play_request(args)) r_ok();
    else r_err(g_fault);
}

static void at_transport(const char *name, int op, char *args)
{
    (void)op; (void)args;
    if (strcmp(name, "MSDSTOP") == 0)
    {
        seone_audio_stop();
        seone_audio_flush_stream();
        talk_clear();
    }
    else if (strcmp(name, "MSDPAUSE") == 0)
        seone_audio_pause();
    else
        seone_audio_resume();
    r_ok();
}

static void at_msdinfo(const char *name, int op, char *args)
{
    (void)args;
    if (op != OP_GET && op != OP_EXEC) { r_ill(); return; }
    if (strcmp(name, "MSDSIZE") == 0) r_value(name, "%lu", (unsigned long)seone_audio_file_size());
    else r_value(name, "%u", seone_audio_percent());
}

static void at_msdtalk(const char *name, int op, char *args)
{
    (void)name;
    uint32_t v;
    if (op != OP_SET || !parse_uint(args, &v) || !talk_push(v)) { r_err(F_PARAM); return; }
    r_ok();
}

// "n@FILE.MP3" stores a slot; plain "n" plays it (AT+MSDSAMPLE accepts both).
static void at_msdsample(const char *name, int op, char *args)
{
    if (op != OP_SET) { r_err(F_PARAM); return; }
    char *at = strchr(args, '@');
    uint32_t slot;
    if (at)
    {
        *at = 0;
        if (!parse_uint(args, &slot) || slot >= SEONE_SAMPLE_SLOTS || strlen(at + 1) > 12u)
        { r_err(F_PARAM); return; }
        copy_str(g_sample[slot], at + 1, sizeof g_sample[slot]);
        r_ok();
        return;
    }
    if (strcmp(name, "MSDSETSAMPLE") == 0 || !parse_uint(args, &slot)) { r_err(F_PARAM); return; }
    if (sample_play(slot)) r_ok();
    else r_err(F_NOFILE);
}

static void at_m3ufile(const char *name, int op, char *args)
{
    (void)name;
    if (op != OP_SET) { r_err(F_PARAM); return; }
    if (!g_mounted && !fs_mount()) { r_err(g_fault); return; }
    char real[SEONE_PATH_MAX];
    if (!resolve_path(args, real, sizeof real, false)) { g_m3u_loaded = false; r_err(F_NOFILE); return; }
    copy_str(g_m3u, real, sizeof g_m3u);
    copy_str(g_m3u_dir, real, sizeof g_m3u_dir);
    char *slash = strrchr(g_m3u_dir, '/');
    if (slash) *slash = 0;
    g_m3u_loaded = true;
    r_ok();
}

static void at_m3urow(const char *name, int op, char *args)
{
    (void)name;
    uint32_t row;
    if (op != OP_SET || !parse_uint(args, &row) || row == 0) { r_err(F_PARAM); return; }
    char text[SEONE_PATH_MAX];
    if (!g_m3u_loaded || !m3u_get_row(row, text, sizeof text))
        copy_str(text, "#END#", sizeof text);
    m3u_shorten_row(text);
    r_printf("%s\r\nOK\r\n", text);
}

static const at_cmd_t at_cmds[] = {
    { "CARTRIDGE",     at_cartridge,  "Get cartridge type" },
    { "LIST",          at_list,       "Get the AT command list" },
    { "SEMODE",        at_semode,     "Set/get mode: MP3A,MP3B,FM,REG,TEST,MIDI,SYNTH,VOICE" },
    { "SELOGBOOK",     at_info,       "Get system logbook" },
    { "SEVERSION",     at_info,       "Get version: 1 radio, 2 MP3, 3 both" },
    { "SEFIRMWARE",    at_info,       "Get firmware file name" },
    { "SEFIRMWARECHK", at_fwcheck,    "Check firmware date DDMMYYYY" },
    { "SETUNE",        at_setting,    "Start-up tune ON/OFF" },
    { "SEPLAYTUNE",    at_simple_ok,  "Play the intro tune" },
    { "PLAYTUNE",      at_simple_ok,  "Play the intro tune" },
    { "SEFAULTNR",     at_info,       "Get last error number" },
    { "SEFAULTTXT",    at_info,       "Get last error text" },
    { "SEFREQ",        at_fm,         "Set FM frequency" },
    { "TMTID",         at_setting,    "TMT id" },
    { "LEDTEST",       at_simple_ok,  "LED test" },
    { "TEST",          at_simple_ok,  "Test" },
    { "FMTYPE",        at_info,       "Get radio chip type" },
    { "FMFREQ",        at_fm,         "Set/get FM frequency" },
    { "FMSCANUP",      at_fm,         "Scan up" },
    { "FMSCANDOWN",    at_fm,         "Scan down" },
    { "FMGETRDS",      at_fm,         "Get RDS text" },
    { "FMSM",          at_setting,    "Search mode ON/OFF" },
    { "FMMUTE",        at_setting,    "FM mute ON/OFF" },
    { "FMPLL",         at_setting,    "FM PLL value" },
    { "FMSUD",         at_setting,    "Search direction UP/DOWN" },
    { "FMSSL",         at_setting,    "Search stop level 1-3" },
    { "FMHLSI",        at_setting,    "High/low side injection" },
    { "FMHISI",        at_setting,    "High/low side injection" },
    { "FMMS",          at_setting,    "Force STEREO/MONO" },
    { "FMBAND",        at_setting,    "Band EUR/JAP" },
    { "FMHCC",         at_setting,    "High cut control ON/OFF" },
    { "FMSNC",         at_setting,    "Stereo noise cancelling ON/OFF" },
    { "FMREADY",       at_setting,    "Get ready flag" },
    { "FMBLF",         at_setting,    "Get band limit flag" },
    { "FMSTEREO",      at_setting,    "Get stereo flag" },
    { "STEREO",        at_setting,    "Get stereo flag" },
    { "FMIF",          at_setting,    "Get IF counter" },
    { "FMADC",         at_setting,    "Get signal level" },
    { "FMSTARTFREQ",   at_setting,    "Start-up FM frequency" },
    { "DSPTYPE",       at_info,       "Get DSP chip type" },
    { "DSPVOLL",       at_dsp_level,  "Left volume 0-100" },
    { "DSPVOLR",       at_dsp_level,  "Right volume 0-100" },
    { "DSPBASS",       at_dsp_level,  "Bass 0-100" },
    { "DSPTREBLE",     at_dsp_level,  "Treble 0-100" },
    { "DSPVOLUP",      at_dsp_step,   "Volume +10" },
    { "DSPVOLDOWN",    at_dsp_step,   "Volume -10" },
    { "DSPMUTE",       at_dsp_mute,   "DSP mute ON/OFF" },
    { "MP3TYPE",       at_info,       "Get MP3 chip type" },
    { "MP3LOWLEVEL",   at_mp3_level,  "FIFO low level (status bit 7)" },
    { "MP3HIGHLEVEL",  at_mp3_level,  "FIFO high level (status bit 6)" },
    { "VULEFT",        at_vu,         "Left VU byte" },
    { "VURIGHT",       at_vu,         "Right VU byte" },
    { "VULN",          at_vu,         "Left VU leds" },
    { "VURN",          at_vu,         "Right VU leds" },
    { "VULEDS",        at_vu,         "Number of VU leds 1-32" },
    { "MSXAUDIO",      at_setting,    "MSX audio switch ON/OFF" },
    { "MSXSTARTAUDIO", at_setting,    "Start-up MSX audio switch" },
    { "USBINIT",       at_usb,        "Mount the storage" },
    { "USBDEINIT",     at_usb,        "Release the storage" },
    { "USBDEVICE",     at_info,       "Get storage type NONE/MSD" },
    { "USBFLASH",      at_usb,        "Firmware update (not available)" },
    { "MSDPATH",       at_msdpath,    "Set/get directory 0:/DIR" },
    { "MSDPATHUP",     at_msdpath,    "Directory up" },
    { "MSDFILES",      at_msdfiles,   "List directory to port 25h" },
    { "MSDPLAY",       at_msdplay,    "Play file / get BUSY,IDLE" },
    { "MSDPAUSE",      at_transport,  "Pause" },
    { "MSDSTART",      at_transport,  "Continue" },
    { "MSDSTOP",       at_transport,  "Stop" },
    { "MSDTALK",       at_msdtalk,    "Speak SP0256 allophone 0-63 from /SETALK" },
    { "MSDSETSAMPLE",  at_msdsample,  "Store sample n@FILE.MP3 (from /SAMPLE)" },
    { "MSDSAMPLE",     at_msdsample,  "Play sample n" },
    { "MSDSIZE",       at_msdinfo,    "Get file size" },
    { "MSDCNT",        at_msdinfo,    "Get played percentage" },
    { "MSDM3UFILE",    at_m3ufile,    "Load M3U playlist" },
    { "MSDM3UGETROW",  at_m3urow,     "Get playlist row n" },
};
static const unsigned at_cmd_count = sizeof at_cmds / sizeof at_cmds[0];

static void at_list(const char *name, int op, char *args)
{
    (void)name; (void)op; (void)args;
    r_puts("AT+LIST=\r");
    for (unsigned i = 0; i < at_cmd_count; i++)
        r_printf("AT+%s\r", at_cmds[i].name);
    r_puts("OK\r\n");
}


static void at_execute(char *line, uint16_t len, bool overflow)
{
    r_reset();
    if (overflow) { r_err(F_INPUT); return; }
    line[len] = 0;

    // trim
    char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    size_t l = strlen(s);
    while (l && (s[l - 1] == ' ' || s[l - 1] == '\t')) s[--l] = 0;

    if (l < 2 || toupper((unsigned char)s[0]) != 'A' || toupper((unsigned char)s[1]) != 'T')
    { r_err(F_INVALID); return; }
    if (l == 2) { r_ok(); return; }
    if (s[2] != '+') { r_err(F_SYNTAX); return; }

    char *name = s + 3;
    char *p = name;
    while (*p && *p != '=' && *p != '?' && *p != '/')
    {
        *p = (char)toupper((unsigned char)*p);
        p++;
    }
    int op = OP_EXEC;
    char *args = p;
    if (*p == '=')
    {
        op = OP_SET;
        *p = 0;
        args = p + 1;
        while (*args == ' ') args++;
    }
    else if (*p == '?')
    {
        op = OP_GET;
        *p = 0;
        args = p + 1;
    }
    else if (*p == '/')
    {
        op = (toupper((unsigned char)p[1]) == 'H') ? OP_HELP : OP_EXEC;
        *p = 0;
        args = p + 1;
    }

    // "AT+NAME>" typo in the manual for AT+FMSM> - tolerate a trailing '>'
    size_t nl = strlen(name);
    if (nl && name[nl - 1] == '>') { name[--nl] = 0; op = OP_GET; }

    for (unsigned i = 0; i < at_cmd_count; i++)
    {
        if (strcmp(at_cmds[i].name, name) == 0)
        {
            if (op == OP_HELP)
            {
                r_printf("AT+%s/H=%s\r\nOK\r\n", name, at_cmds[i].help);
                return;
            }
            fs_refresh();
            at_cmds[i].fn(name, op, args);
            return;
        }
    }
    r_err(F_UNKNOWN);
}

// -----------------------------------------------------------------------
// REG mode (ports 0x26 / 0x27)
// -----------------------------------------------------------------------
static uint8_t  reg_out[SEONE_REG_MAX];
static uint16_t reg_out_len;

static void reg_put(uint8_t b)          { if (reg_out_len < SEONE_REG_MAX) reg_out[reg_out_len++] = b; }
static void reg_put_str(const char *s)  { while (*s) reg_put((uint8_t)*s++); reg_put(0); }

static bool reg_execute(uint8_t code, const uint8_t *in, uint16_t len)
{
    char text[SEONE_PATH_MAX];
    size_t tl = len < sizeof text - 1 ? len : sizeof text - 1;
    memcpy(text, in, tl);
    text[tl] = 0;
    reg_out_len = 0;
    fs_refresh();

    switch (code)
    {
        case 16:  // set DSP select
            if (len < 1) return false;
            g_dsp_select = in[0] ? 1 : 0;
            apply_dsp();
            return true;
        case 17:  reg_put(g_dsp_select); return true;
        case 32:  return true;                           // FM write registers
        case 33:  for (int i = 0; i < 5; i++) reg_put(0); return true;
        case 34:  { uint16_t f; if (!parse_freq(text, &f)) return false; g_fm_freq = f; return true; }
        case 35:
        {
            char tmp[12];
            snprintf(tmp, sizeof tmp, "%u.%u", g_fm_freq / 100u, (g_fm_freq / 10u) % 10u);
            for (char *c = tmp; *c; c++) reg_put((uint8_t)*c);
            reg_put(13);
            return true;
        }
        case 36:
            if (len < 2) return false;
            { uint16_t f = (uint16_t)((in[0] << 8) | in[1]); if (f < 7600 || f > 10895) return false; g_fm_freq = f; }
            return true;
        case 37:  reg_put((uint8_t)(g_fm_freq >> 8)); reg_put((uint8_t)g_fm_freq); return true;
        case 38:  g_fm_freq = (uint16_t)(g_fm_freq + ((len && in[0]) ? 10 : -10)); return true;
        case 48:  { int v = g_vol_l + 10; g_vol_l = (uint8_t)(v > 100 ? 100 : v); v = g_vol_r + 10; g_vol_r = (uint8_t)(v > 100 ? 100 : v); apply_dsp(); return true; }
        case 49:  { int v = g_vol_l - 10; g_vol_l = (uint8_t)(v < 0 ? 0 : v); v = g_vol_r - 10; g_vol_r = (uint8_t)(v < 0 ? 0 : v); apply_dsp(); return true; }
        case 50:
            if (len < 2 || in[0] > 100 || in[1] > 100) return false;
            g_vol_l = in[0]; g_vol_r = in[1];
            apply_dsp();
            return true;
        case 51:  reg_put(g_vol_l); reg_put(g_vol_r); return true;
        case 52:
            if (len < 4 || in[0] > 100 || in[1] > 100 || in[2] > 3) return false;
            g_bass = in[0]; g_treble = in[1]; g_stereo = in[2]; g_dsp_mute = in[3] != 0;
            apply_dsp();
            return true;
        case 53:  reg_put(g_bass); reg_put(g_treble); reg_put(g_stereo); reg_put(g_dsp_mute); return true;
        case 64:  return true;                           // VU enable
        case 66:  return true;                           // VU manual override
        case 67:  reg_put(seone_audio_peak_left()); reg_put(seone_audio_peak_right()); return true;
        case 68:
            if (len < 1 || in[0] < 1 || in[0] > 32) return false;
            g_vu_max = in[0];
            seone_audio_set_vu_max(g_vu_max);
            return true;
        case 69:  reg_put(seone.vu_left); reg_put(seone.vu_right); return true;
        case 70:  for (int i = 0; i < 16; i++) reg_put(0); return true;
        case 0x60: reg_put_str(SEONE_FW_DATE_YMD); return true;
        case 0x70: return g_mounted || fs_mount();
        case 0x71: return true;
        case 0x72: reg_put(g_mounted ? 1 : 0); return true;
        case 0x74: return (g_mounted || fs_mount()) && play_request(text);
        case 0x75: reg_put_str(g_play_name); return true;
        case 0x76:
        {
            char real[SEONE_PATH_MAX];
            if (!(g_mounted || fs_mount()) || !resolve_path(text, real, sizeof real, true)) return false;
            copy_str(g_cwd, real, sizeof g_cwd);
            copy_str(g_cwd_client, text, sizeof g_cwd_client);
            return true;
        }
        case 0x77: reg_put_str(g_cwd_client[0] ? g_cwd_client : "0:/"); return true;
        case 0x78: seone_audio_resume(); return true;
        case 0x79: seone_audio_stop(); seone_audio_flush_stream(); talk_clear(); return true;
        case 0x7A: seone_audio_pause(); return true;
        case 0x7B:
            if (!(g_mounted || fs_mount()) || !build_listing()) return false;
            // REG listing is NUL terminated: replace the final LF by 0
            if (seone.list_len) seone.list[seone.list_len - 1] = 0;
            return true;
        case 0x7C:
        {
            uint32_t sz = seone_audio_file_size();
            reg_put((uint8_t)(sz >> 24)); reg_put((uint8_t)(sz >> 16)); reg_put((uint8_t)(sz >> 8)); reg_put((uint8_t)sz);
            return true;
        }
        case 0x7D: reg_put(seone_audio_percent()); return true;
        case 126:  return len >= 1 && talk_push(in[0]);
        case 127:
            if (len < 2 || in[0] >= SEONE_SAMPLE_SLOTS || len - 1u > 12u) return false;
            memcpy(g_sample[in[0]], in + 1, len - 1u);
            g_sample[in[0]][len - 1u] = 0;
            return true;
        case 128:  return len >= 1 && sample_play(in[0]);
        default:   return false;
    }
}

// -----------------------------------------------------------------------
// Public (core 1)
// -----------------------------------------------------------------------
void seone_init(uint8_t *arena, size_t arena_size)
{
    memset(&seone, 0, sizeof seone);
    arena_ptr = arena;
    arena_left = arena_size;
    seone.list = seone_arena_alloc(SEONE_LIST_SIZE);
    seone.ring = seone_arena_alloc(SEONE_RING_SIZE);
    g_entries = seone_arena_alloc(sizeof(list_entry_t) * SEONE_MAX_ENTRIES);
    g_core1_stack = seone_arena_alloc(SEONE_CORE1_STACK_SIZE);
    seone.mode = SEONE_MODE_MP3A;
    seone.status = SEONE_ST_END_OF_MUSIC | SEONE_ST_USB_ERROR | SEONE_ST_FIFO_EMPTY;
    seone.reg_result = SEONE_REG_IDLE;
}

uint32_t *seone_core1_stack(void)
{
    return g_core1_stack;
}

void seone_cmd_init(void)
{
    g_cwd[0] = 0;
    copy_str(g_cwd_client, "0:", sizeof g_cwd_client);
    fs_mount();
    apply_dsp();
    seone_audio_set_vu_max(g_vu_max);
    seone_audio_set_stream_enabled(true);
    update_status();
}

void seone_cmd_poll(void)
{
    // AT command line
    uint32_t seq = seone.cmd_seq;
    if (seq != g_handled_cmd_seq)
    {
        seone_barrier();
        char line[SEONE_CMD_MAX + 1];
        uint16_t len = seone.cmd_len;
        uint8_t idx = seone.cmd_index;
        bool overflow = (len == 0xFFFFu);
        if (overflow) len = 0;
        if (len > SEONE_CMD_MAX) len = SEONE_CMD_MAX;
        memcpy(line, seone.cmd[idx], len);
        g_handled_cmd_seq = seq;
        at_execute(line, len, overflow);
        publish_response(seq);
    }

    // REG function
    uint32_t rseq = seone.regcmd_seq;
    if (rseq != g_handled_reg_seq)
    {
        seone_barrier();
        uint8_t code = seone.regcmd_code;
        uint16_t len = seone.regcmd_len;
        uint8_t in[SEONE_REG_MAX];
        if (len > SEONE_REG_MAX) len = SEONE_REG_MAX;
        memcpy(in, seone.reg_in[seone.regcmd_index], len);
        g_handled_reg_seq = rseq;
        bool ok = reg_execute(code, in, len);
        update_status();
        seone.reg_out_len = 0;
        seone_barrier();
        memcpy(seone.reg_out, reg_out, reg_out_len);
        seone_barrier();
        seone.reg_out_len = reg_out_len;
        seone.reg_result = ok ? SEONE_REG_IDLE : SEONE_REG_ERROR;
        seone_barrier();
        seone.reg_out_gen = seone.reg_out_gen + 1u;
        seone.reg_done_seq = rseq;
    }

    talk_service();
    update_status();
}

void seone_core1_init(void)
{
    seone_audio_init();
    seone_cmd_init();
}

void seone_core1_poll(void)
{
    seone_cmd_poll();
    seone_audio_service();
}
