// MSX PICOVERSE PROJECT
// pva_engine.c - PicoVerse Audio Engine: commands, folders, play list (core 1).
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "pva_internal.h"

#define DIR_MAX         1024u
#define DIR_POOL        (20u * 1024u)
#define PL_MAX          512u
#define PL_POOL         (24u * 1024u)
#define PATH_MAX_LEN    240u
#define STATUS_MS       40u
#define HEARTBEAT_MS    5u

typedef struct { uint32_t size; uint16_t name; uint8_t type; uint8_t pad; } dent_t;

static FATFS    fs;
static bool     mounted;
static uint8_t  mount_fr = 0xFF;
static uint8_t  last_fr;

// open folder
static dent_t  *dir_ent;
static char    *dir_pool;
static uint32_t dir_pool_used;
static uint16_t dir_count;
static char     cur_dir[PATH_MAX_LEN + 1];   // "" = root, else "/A/B"
static bool     dir_open;
static uint8_t  dir_gen;

// play list (full paths, "/A/B/x.mp3")
static uint16_t *pl_off;
static uint16_t *pl_order;
static char     *pl_pool;
static uint32_t  pl_pool_used;
static uint16_t  pl_count, pl_pos;
static uint8_t   pl_kind;
static char      pl_m3u_dir[PATH_MAX_LEN + 1];

// playback
static uint8_t  state = PVA_STATE_STOPPED;
static uint8_t  mode;
static uint8_t  volume = 80;
static uint8_t  play_error;
static uint16_t track_id;
static pva_track_info_t info;
static uint32_t rng = 12345;

static uint8_t  args[PVA_ARGS_SIZE];
static uint8_t *reply;
static uint32_t reply_len;
static uint32_t handled_post;
static uint32_t last_status_ms, last_hb_ms, last_poll_ms, max_gap_ms;
static uint8_t  hb;
static uint8_t  seq;
static bool     status_dirty = true;

// ---------------------------------------------------------------- helpers
static void fs_refresh(void)
{
    // Nextor writes the card behind FatFS' back (raw sectors through the
    // Sunrise IDE emulation): drop FatFS' cached sector before every command.
    if (mounted && !fs.wflag)
        fs.winsect = (LBA_t)0 - 1;
}

static bool do_mount(void)
{
    f_mount(NULL, "0:", 0);
    FRESULT fr = f_mount(&fs, "0:", 1);
    mount_fr = (uint8_t)fr;
    mounted = (fr == FR_OK);
    if (mounted)
        pva_log("mounted FAT%s, %lu KB clusters", fs.fs_type == FS_FAT12 ? "12" : fs.fs_type == FS_FAT16 ? "16" : fs.fs_type == FS_FAT32 ? "32" : "ex",
                (unsigned long)(fs.csize / 2u));
    else
    {
        last_fr = (uint8_t)fr;
        pva_log("mount failed, FRESULT %d", (int)fr);
    }
    return mounted;
}

static char upc(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static int name_cmp(const char *a, const char *b)
{
    for (;; a++, b++)
    {
        char ca = upc(*a), cb = upc(*b);
        if (ca != cb || !ca) return (int)(uint8_t)ca - (int)(uint8_t)cb;
    }
}

static int dent_cmp(const void *pa, const void *pb)
{
    const dent_t *a = pa, *b = pb;
    if (a->type == PVA_DE_PARENT) return -1;
    if (b->type == PVA_DE_PARENT) return 1;
    bool da = a->type == PVA_DE_DIR, db = b->type == PVA_DE_DIR;
    if (da != db) return da ? -1 : 1;
    return name_cmp(dir_pool + a->name, dir_pool + b->name);
}

static void fatfs_path(char *out, size_t size, const char *path)
{
    snprintf(out, size, "0:%s", path[0] ? path : "/");
}

static void set_reply_byte(uint8_t b)
{
    if (reply_len < PVA_REPLY_SIZE) reply[reply_len++] = b;
}

static void set_reply_str(const char *s)
{
    while (*s && reply_len < PVA_REPLY_SIZE - 1) reply[reply_len++] = (uint8_t)*s++;
    set_reply_byte(0);
}

static void set_reply_text(const char *s, size_t n)
{
    if (n > PVA_REPLY_SIZE) n = PVA_REPLY_SIZE;
    memcpy(reply, s, n);
    reply_len = (uint32_t)n;
}

// ---------------------------------------------------------------- folders
static int read_dir(const char *path)
{
    char fp[PATH_MAX_LEN + 8];
    DIR d;
    FILINFO fno;
    fatfs_path(fp, sizeof fp, path);
    fs_refresh();
    FRESULT fr = f_opendir(&d, fp);
    if (fr != FR_OK)
    {
        last_fr = (uint8_t)fr;
        pva_log("opendir %d: %.30s", (int)fr, fp);
        return (fr == FR_NO_PATH || fr == FR_NO_FILE || fr == FR_INVALID_NAME) ? PVA_E_NOTFOUND : PVA_E_IO;
    }
    dir_count = 0;
    dir_pool_used = 0;
    if (path[0])
    {
        dir_ent[0].type = PVA_DE_PARENT;
        dir_ent[0].size = 0;
        dir_ent[0].name = 0;
        memcpy(dir_pool, "..", 3);
        dir_pool_used = 3;
        dir_count = 1;
    }
    bool full = false;
    for (;;)
    {
        fr = f_readdir(&d, &fno);
        if (fr != FR_OK) { last_fr = (uint8_t)fr; pva_log("readdir %d", (int)fr); break; }
        if (!fno.fname[0]) break;
        if (fno.fattrib & (AM_HID | AM_SYS)) continue;
        if (fno.fname[0] == '.') continue;          // ".", "..", "._" files
        uint8_t type;
        if (fno.fattrib & AM_DIR) type = PVA_DE_DIR;
        else
        {
            int k = pva_kind_from_name(fno.fname);
            if (!k) continue;
            type = (uint8_t)k;
        }
        size_t l = strlen(fno.fname) + 1;
        if (dir_count >= DIR_MAX || dir_pool_used + l > DIR_POOL) { full = true; break; }
        dent_t *e = &dir_ent[dir_count++];
        e->type = type;
        e->size = (uint32_t)fno.fsize;
        e->name = (uint16_t)dir_pool_used;
        memcpy(dir_pool + dir_pool_used, fno.fname, l);
        dir_pool_used += (uint32_t)l;
    }
    f_closedir(&d);
    if (full) pva_log("folder cut at %u entries", (unsigned)dir_count);
    qsort(dir_ent, dir_count, sizeof(dent_t), dent_cmp);
    if (path != cur_dir)
        snprintf(cur_dir, sizeof cur_dir, "%s", path);
    dir_open = true;
    dir_gen++;
    return PVA_OK;
}

static void reply_dir_header(void)
{
    set_reply_byte((uint8_t)(dir_count & 0xFF));
    set_reply_byte((uint8_t)(dir_count >> 8));
    char shown[PATH_MAX_LEN + 1];
    pva_copy_ascii(shown, sizeof shown, cur_dir[0] ? cur_dir : "/", PATH_MAX_LEN);
    set_reply_str(shown);
}

// normalise an MSX path: "\" -> "/", no trailing "/", "" for the root
static bool normalise_path(char *out, size_t size, const char *in)
{
    size_t o = 0;
    if (in[0] != '/' && in[0] != '\\') { if (size) out[0] = 0; return false; }
    for (size_t i = 0; in[i]; i++)
    {
        char c = in[i] == '\\' ? '/' : in[i];
        if (c == '/' && o > 0 && out[o - 1] == '/') continue;
        if (o + 1 >= size) return false;
        out[o++] = c;
    }
    while (o > 0 && out[o - 1] == '/') o--;
    out[o] = 0;
    return true;
}

static int cmd_dir_open(void)
{
    char path[PATH_MAX_LEN + 1];
    args[PVA_ARGS_SIZE - 1] = 0;
    if (!mounted && !do_mount()) return PVA_E_NOCARD;
    if (args[0] == 0)
    {
        int r = read_dir(cur_dir);
        if (r != PVA_OK && cur_dir[0]) { cur_dir[0] = 0; r = read_dir(cur_dir); }
        if (r != PVA_OK) return r;
    }
    else
    {
        if (!normalise_path(path, sizeof path, (const char *)args)) return PVA_E_ARGS;
        int r = read_dir(path);
        if (r != PVA_OK) return r;
    }
    reply_dir_header();
    return PVA_OK;
}

static int cmd_dir_enter(void)
{
    if (!dir_open) return PVA_E_STATE;
    uint16_t idx = (uint16_t)(args[0] | (args[1] << 8));
    if (idx >= dir_count) return PVA_E_RANGE;
    char path[PATH_MAX_LEN + 1];
    if (dir_ent[idx].type == PVA_DE_PARENT)
    {
        snprintf(path, sizeof path, "%s", cur_dir);
        char *s = strrchr(path, '/');
        if (s) *s = 0;
    }
    else if (dir_ent[idx].type == PVA_DE_DIR)
    {
        if (strlen(cur_dir) + 1 + strlen(dir_pool + dir_ent[idx].name) > PATH_MAX_LEN) return PVA_E_ARGS;
        snprintf(path, sizeof path, "%s/%s", cur_dir, dir_pool + dir_ent[idx].name);
    }
    else
        return PVA_E_ARGS;
    int r = read_dir(path);
    if (r != PVA_OK) return r;
    reply_dir_header();
    return PVA_OK;
}

static int cmd_dir_read(void)
{
    if (!dir_open) return PVA_E_STATE;
    uint16_t first = (uint16_t)(args[0] | (args[1] << 8));
    uint8_t max = args[2];
    for (uint16_t i = first; i < dir_count && max; i++, max--)
    {
        const dent_t *e = &dir_ent[i];
        char shown[PVA_NAME_MAX + 1];
        pva_copy_ascii(shown, sizeof shown, dir_pool + e->name, 255);
        uint8_t l = (uint8_t)strlen(shown);
        if (reply_len + 6u + l > PVA_REPLY_SIZE) break;
        set_reply_byte(e->type);
        set_reply_byte((uint8_t)e->size);
        set_reply_byte((uint8_t)(e->size >> 8));
        set_reply_byte((uint8_t)(e->size >> 16));
        set_reply_byte((uint8_t)(e->size >> 24));
        set_reply_byte(l);
        memcpy(reply + reply_len, shown, l);
        reply_len += l;
    }
    return PVA_OK;
}

// ---------------------------------------------------------------- play list
static uint32_t rand32(void)
{
    rng = rng * 1664525u + 1013904223u;
    return rng;
}

static void pl_clear(void)
{
    pl_count = 0;
    pl_pos = 0;
    pl_pool_used = 0;
    pl_kind = 0;
    pl_m3u_dir[0] = 0;
}

static bool pl_add(const char *path)
{
    size_t l = strlen(path) + 1;
    if (pl_count >= PL_MAX || pl_pool_used + l > PL_POOL) return false;
    pl_off[pl_count] = (uint16_t)pl_pool_used;
    pl_order[pl_count] = pl_count;
    memcpy(pl_pool + pl_pool_used, path, l);
    pl_pool_used += (uint32_t)l;
    pl_count++;
    return true;
}

// Shuffle the order, keeping track 'first' at the front.
static void pl_shuffle(uint16_t first)
{
    for (uint16_t i = 0; i < pl_count; i++) pl_order[i] = i;
    if (pl_count < 2) return;
    for (uint16_t i = pl_count - 1; i > 0; i--)
    {
        uint16_t j = (uint16_t)(rand32() % (i + 1u));
        uint16_t t = pl_order[i]; pl_order[i] = pl_order[j]; pl_order[j] = t;
    }
    for (uint16_t i = 0; i < pl_count; i++)
        if (pl_order[i] == first) { pl_order[i] = pl_order[0]; pl_order[0] = first; break; }
}

static void pl_unshuffle(void)
{
    uint16_t cur = pl_count ? pl_order[pl_pos] : 0;
    for (uint16_t i = 0; i < pl_count; i++) pl_order[i] = i;
    pl_pos = cur;
}

// Line reader for M3U files
typedef struct { FIL *f; uint8_t buf[512]; UINT len, pos; bool eof; } line_reader_t;

static bool read_line(line_reader_t *lr, char *out, size_t size)
{
    size_t o = 0;
    bool any = false;
    for (;;)
    {
        if (lr->pos >= lr->len)
        {
            if (lr->eof) break;
            if (f_read(lr->f, lr->buf, sizeof lr->buf, &lr->len) != FR_OK || lr->len == 0) { lr->eof = true; lr->len = 0; break; }
            lr->pos = 0;
        }
        char c = (char)lr->buf[lr->pos++];
        any = true;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (o + 1 < size) out[o++] = c;
    }
    out[o] = 0;
    return any;
}

static int load_m3u(const char *m3u_path)
{
    static FIL f;
    static line_reader_t lr;
    char fp[PATH_MAX_LEN + 8];
    fatfs_path(fp, sizeof fp, m3u_path);
    fs_refresh();
    FRESULT fr = f_open(&f, fp, FA_READ);
    if (fr != FR_OK) { last_fr = (uint8_t)fr; return PVA_E_NOTFOUND; }
    pl_clear();
    pl_kind = 1;
    snprintf(pl_m3u_dir, sizeof pl_m3u_dir, "%s", m3u_path);
    char *s = strrchr(pl_m3u_dir, '/');
    if (s) *s = 0;
    memset(&lr, 0, sizeof lr);
    lr.f = &f;
    char line[PATH_MAX_LEN + 1], path[PATH_MAX_LEN + 1];
    bool first = true;
    while (read_line(&lr, line, sizeof line))
    {
        char *p = line;
        if (first && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) p += 3;
        first = false;
        while (*p == ' ' || *p == '\t') p++;
        size_t l = strlen(p);
        while (l && (p[l - 1] == ' ' || p[l - 1] == '\t')) p[--l] = 0;
        if (!*p || *p == '#') continue;
        if (strstr(p, "://")) continue;          // stream URLs: later
        for (char *q = p; *q; q++) if (*q == '\\') *q = '/';
        if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') p += 2;
        if (!pva_kind_from_name(p) || pva_kind_from_name(p) == PVA_DE_M3U) continue;
        if (p[0] == '/') snprintf(path, sizeof path, "%s", p);
        else snprintf(path, sizeof path, "%s/%s", pl_m3u_dir, p);
        if (!pl_add(path)) { pva_log("play list cut at %u", (unsigned)pl_count); break; }
    }
    f_close(&f);
    pva_log("M3U: %u tracks", (unsigned)pl_count);
    return pl_count ? PVA_OK : PVA_E_FORMAT;
}

// ---------------------------------------------------------------- playback
static void set_title_from_path(const char *path)
{
    const char *n = strrchr(path, '/');
    n = n ? n + 1 : path;
    char tmp[PVA_ST_TITLE_LEN];
    pva_copy_ascii(tmp, sizeof tmp, n, 255);
    char *dot = strrchr(tmp, '.');
    if (dot && dot != tmp) *dot = 0;
    memcpy(info.title, tmp, sizeof info.title);
}

static int open_track(const char *path)
{
    char fp[PATH_MAX_LEN + 8];
    fatfs_path(fp, sizeof fp, path);
    fs_refresh();
    int r = pva_dec_open(fp, &info);
    if (r == PVA_E_NOTFOUND && pl_kind == 1)
    {
        // M3U written on another computer: try the file name in the list's folder
        const char *n = strrchr(path, '/');
        n = n ? n + 1 : path;
        char alt[PATH_MAX_LEN + 8];
        snprintf(alt, sizeof alt, "0:%s/%s", pl_m3u_dir, n);
        r = pva_dec_open(alt, &info);
    }
    if (r == PVA_OK && !info.title[0]) set_title_from_path(path);
    return r;
}

// Start playing play list position 'pos', skipping tracks that fail.
static int play_pos(uint16_t pos)
{
    if (!pl_count) return PVA_E_STATE;
    int r = PVA_E_STATE;
    for (uint16_t tries = 0; tries < pl_count && tries < 16; tries++)
    {
        uint16_t p = (uint16_t)((pos + tries) % pl_count);
        const char *path = pl_pool + pl_off[pl_order[p]];
        r = open_track(path);
        if (r == PVA_OK)
        {
            pl_pos = p;
            track_id++;
            play_error = 0;
            state = PVA_STATE_PLAYING;
            pva_hal_audio_mute(false);
            pva_log("play %u/%u %.32s", (unsigned)(p + 1), (unsigned)pl_count, info.title);
            status_dirty = true;
            return PVA_OK;
        }
        play_error = (uint8_t)r;
        pva_log("skip %.40s (%d)", path, r);
    }
    state = PVA_STATE_STOPPED;
    status_dirty = true;
    return r;
}

static void stop_playback(void)
{
    pva_dec_close();
    state = PVA_STATE_STOPPED;
    pva_hal_audio_mute(true);
    status_dirty = true;
}

static void track_ended(void)
{
    if (mode & PVA_MODE_REPEAT_ONE) { play_pos(pl_pos); return; }
    if (pl_pos + 1u < pl_count) { play_pos((uint16_t)(pl_pos + 1u)); return; }
    if (mode & PVA_MODE_REPEAT_ALL)
    {
        if (mode & PVA_MODE_SHUFFLE) pl_shuffle(pl_order[0]);
        play_pos(0);
        return;
    }
    stop_playback();
    pva_log("end of play list");
}

static int cmd_play_index(void)
{
    if (!dir_open) return PVA_E_STATE;
    uint16_t idx = (uint16_t)(args[0] | (args[1] << 8));
    if (idx >= dir_count) return PVA_E_RANGE;
    const dent_t *sel = &dir_ent[idx];
    char path[PATH_MAX_LEN + 1];
    if (sel->type == PVA_DE_M3U)
    {
        snprintf(path, sizeof path, "%s/%s", cur_dir, dir_pool + sel->name);
        stop_playback();
        int r = load_m3u(path);
        if (r != PVA_OK) return r;
        if (mode & PVA_MODE_SHUFFLE) pl_shuffle((uint16_t)(rand32() % pl_count));
        return play_pos(0);
    }
    if (sel->type != PVA_DE_MP3 && sel->type != PVA_DE_WAV) return PVA_E_FORMAT;
    stop_playback();
    pl_clear();
    uint16_t start = 0;
    for (uint16_t i = 0; i < dir_count; i++)
    {
        const dent_t *e = &dir_ent[i];
        if (e->type != PVA_DE_MP3 && e->type != PVA_DE_WAV) continue;
        if (i == idx) start = pl_count;
        snprintf(path, sizeof path, "%s/%s", cur_dir, dir_pool + e->name);
        if (!pl_add(path))
        {
            pva_log("play list cut at %u", (unsigned)pl_count);
            if (i <= idx) { pl_clear(); snprintf(path, sizeof path, "%s/%s", cur_dir, dir_pool + sel->name); pl_add(path); start = 0; }
            break;
        }
    }
    if (mode & PVA_MODE_SHUFFLE) { pl_shuffle(start); start = 0; }
    return play_pos(start);
}

static int cmd_play_path(void)
{
    char path[PATH_MAX_LEN + 1];
    args[PVA_ARGS_SIZE - 1] = 0;
    if (!normalise_path(path, sizeof path, (const char *)args) || !path[0]) return PVA_E_ARGS;
    if (!mounted && !do_mount()) return PVA_E_NOCARD;
    stop_playback();
    if (pva_kind_from_name(path) == PVA_DE_M3U)
    {
        int r = load_m3u(path);
        if (r != PVA_OK) return r;
        return play_pos(0);
    }
    pl_clear();
    pl_add(path);
    return play_pos(0);
}

static int cmd_mode(void)
{
    uint8_t m = args[0] & (PVA_MODE_REPEAT_ALL | PVA_MODE_SHUFFLE | PVA_MODE_REPEAT_ONE);
    bool was = mode & PVA_MODE_SHUFFLE, now = m & PVA_MODE_SHUFFLE;
    mode = m;
    if (pl_count && now && !was)
    {
        uint16_t cur = pl_order[pl_pos];
        pl_shuffle(cur);
        pl_pos = 0;
    }
    else if (pl_count && was && !now)
        pl_unshuffle();
    return PVA_OK;
}

// ---------------------------------------------------------------- text replies
static int cmd_diag(void)
{
    char t[1024];
    uint32_t up = pva_hal_millis() / 1000u;
    int n = snprintf(t, sizeof t,
        "PicoVerse Audio Engine %d.%d (API %d)\r\n"
        "Uptime %lu s, heartbeat %u, longest gap %lu ms\r\n"
        "microSD: %s (mount FRESULT %d)\r\n",
        PVA_FW_MAJOR, PVA_FW_MINOR, PVA_API_VERSION,
        (unsigned long)up, (unsigned)hb, (unsigned long)max_gap_ms,
        mounted ? "mounted" : "not mounted", (int)mount_fr);
    if (mounted)
        n += snprintf(t + n, sizeof t - (size_t)n, "Volume: FAT%s, %lu sectors, cluster %u KB\r\n",
                      fs.fs_type == FS_FAT12 ? "12" : fs.fs_type == FS_FAT16 ? "16" : fs.fs_type == FS_FAT32 ? "32" : "ex",
                      (unsigned long)((fs.n_fatent - 2u) * fs.csize), (unsigned)(fs.csize / 2u));
    n += snprintf(t + n, sizeof t - (size_t)n,
        "Folder: %.60s (%u entries)\r\n"
        "Play list: %u tracks, position %u, mode %u\r\n"
        "State %u, error %u, last FRESULT %d/%d\r\n"
        "Track: %u Hz, %u ch, %lu kbit/s, %lu s\r\n"
        "Arena free: %lu bytes, log lines %u\r\n",
        cur_dir[0] ? cur_dir : "/", (unsigned)dir_count,
        (unsigned)pl_count, (unsigned)pl_pos, (unsigned)mode,
        (unsigned)state, (unsigned)play_error, (int)last_fr, (int)pva_dec_last_fresult(),
        (unsigned)info.sample_rate, (unsigned)info.channels, (unsigned long)info.bitrate_kbps, (unsigned long)info.total_seconds,
        (unsigned long)pva_arena_free(), (unsigned)pva_log_count());
    set_reply_text(t, (size_t)n);
    return PVA_OK;
}

static int execute(uint8_t code)
{
    switch (code)
    {
        case PVA_C_PING:
        {
            char t[40];
            int n = snprintf(t, sizeof t, "PVAUDIO %d.%d", PVA_FW_MAJOR, PVA_FW_MINOR);
            set_reply_text(t, (size_t)n + 1u);
            return PVA_OK;
        }
        case PVA_C_MOUNT:
            stop_playback();
            dir_open = false;
            return do_mount() ? PVA_OK : PVA_E_NOCARD;
        case PVA_C_LOG:
            reply_len = (uint32_t)pva_log_dump((char *)reply, PVA_REPLY_SIZE);
            return PVA_OK;
        case PVA_C_DIAG:      return cmd_diag();
        case PVA_C_DIR_OPEN:  return cmd_dir_open();
        case PVA_C_DIR_READ:  return cmd_dir_read();
        case PVA_C_DIR_ENTER: return cmd_dir_enter();
        case PVA_C_PLAY_INDEX: return cmd_play_index();
        case PVA_C_PLAY_PATH: return cmd_play_path();
        case PVA_C_PAUSE:
            if (state == PVA_STATE_PLAYING) { state = PVA_STATE_PAUSED; pva_hal_audio_mute(true); }
            else if (state == PVA_STATE_PAUSED) { state = PVA_STATE_PLAYING; pva_hal_audio_mute(false); }
            else if (state == PVA_STATE_STOPPED && pl_count) return play_pos(pl_pos);
            else return PVA_E_STATE;
            return PVA_OK;
        case PVA_C_STOP:
            stop_playback();
            return PVA_OK;
        case PVA_C_NEXT:
            if (!pl_count) return PVA_E_STATE;
            if (pl_pos + 1u < pl_count) return play_pos((uint16_t)(pl_pos + 1u));
            return play_pos(0);
        case PVA_C_PREV:
            if (!pl_count) return PVA_E_STATE;
            if (pva_dec_active() && pva_dec_elapsed() >= 3u) return play_pos(pl_pos);
            return play_pos(pl_pos ? (uint16_t)(pl_pos - 1u) : (uint16_t)(pl_count - 1u));
        case PVA_C_SEEK:
        {
            if (!pva_dec_active()) return PVA_E_STATE;
            if (!info.seekable) return PVA_E_STATE;
            return pva_dec_seek((uint32_t)(args[0] | (args[1] << 8)));
        }
        case PVA_C_VOLUME:
            volume = args[0] > 100 ? 100 : args[0];
            pva_dec_set_volume(volume);
            return PVA_OK;
        case PVA_C_MODE:
            return cmd_mode();
        default:
            return PVA_E_UNKNOWN;
    }
}

static void handle_command(void)
{
    uint32_t post = pva_mailbox.post_count;
    if (post == handled_post) return;
    pva_barrier();
    uint8_t token = pva_mailbox.token;
    uint8_t code = pva_mailbox.code;
    handled_post = post;
    pva_win[PVA_R_BUSY] = 1;
    for (uint32_t i = 0; i < PVA_ARGS_SIZE; i++) args[i] = pva_win[PVA_R_ARGS + i];
    reply_len = 0;
    int r = execute(code);
    for (uint32_t i = 0; i < reply_len; i++) pva_win[PVA_R_REPLY + i] = reply[i];
    pva_win[PVA_R_RESULT] = (uint8_t)r;
    pva_win[PVA_R_REPLY_LEN] = (uint8_t)(reply_len & 0xFF);
    pva_win[PVA_R_REPLY_LEN + 1] = (uint8_t)(reply_len >> 8);
    status_dirty = true;
    pva_barrier();
    pva_win[PVA_R_DONE_TOKEN] = token;
    pva_win[PVA_R_BUSY] = 0;
}

// ---------------------------------------------------------------- status block
static uint8_t st_new[PVA_STATUS_SIZE], st_pub[PVA_STATUS_SIZE];
static void st8(uint32_t o, uint8_t v) { st_new[o] = v; }
static void st16(uint32_t o, uint16_t v) { st8(o, (uint8_t)v); st8(o + 1, (uint8_t)(v >> 8)); }
static void ststr(uint32_t o, const char *s, uint32_t size)
{
    uint32_t i = 0;
    for (; i + 1 < size && s[i]; i++) st8(o + i, (uint8_t)s[i]);
    for (; i < size; i++) st8(o + i, 0);
}

// Build the block, and only when it differs from the published one, copy it
// into the window inside the sequence-byte bracket (so a reader rarely has
// to retry).
static void publish_status(void)
{
    uint8_t vl = 0, vr = 0;
    if (state == PVA_STATE_PLAYING) pva_dec_vu(&vl, &vr);
    bool active = pva_dec_active();
    st8(PVA_ST_STATE, state);
    st8(PVA_ST_FLAGS, (uint8_t)((mounted ? PVA_F_MOUNTED : 0) | (info.tagged && active ? PVA_F_TAGS : 0) |
                                (info.seekable && active ? PVA_F_SEEKABLE : 0) | (dir_open ? PVA_F_DIR_OPEN : 0)));
    st8(PVA_ST_VOLUME, volume);
    st16(PVA_ST_ELAPSED, active ? (uint16_t)pva_dec_elapsed() : 0);
    st16(PVA_ST_TOTAL, active ? (uint16_t)info.total_seconds : 0);
    st16(PVA_ST_BITRATE, active ? (uint16_t)info.bitrate_kbps : 0);
    st16(PVA_ST_RATE, active ? (uint16_t)(info.sample_rate / 10u) : 0);
    st8(PVA_ST_CHANNELS, active ? info.channels : 0);
    st8(PVA_ST_ERROR, play_error);
    st8(PVA_ST_FRESULT, last_fr ? last_fr : pva_dec_last_fresult());
    st16(PVA_ST_TRACK_ID, track_id);
    st8(PVA_ST_VU_L, vl);
    st8(PVA_ST_VU_R, vr);
    st16(PVA_ST_PL_POS, pl_pos);
    st16(PVA_ST_PL_COUNT, pl_count);
    st8(PVA_ST_MODE, mode);
    st16(PVA_ST_UNDERRUNS, 0);
    st8(PVA_ST_MOUNT_FR, mount_fr);
    st8(PVA_ST_DIR_GEN, dir_gen);
    st16(PVA_ST_UPTIME, (uint16_t)(pva_hal_millis() / 1000u));
    st16(PVA_ST_LOG_COUNT, pva_log_count());
    ststr(PVA_ST_TITLE, active || state != PVA_STATE_STOPPED ? info.title : "", PVA_ST_TITLE_LEN);
    ststr(PVA_ST_ARTIST, active ? info.artist : "", PVA_ST_ARTIST_LEN);
    ststr(PVA_ST_ALBUM, active ? info.album : "", PVA_ST_ALBUM_LEN);
    st8(PVA_ST_FORMAT, active ? info.format : 0);
    st8(PVA_ST_PL_KIND, pl_kind);
    st_new[PVA_ST_SEQ] = st_pub[PVA_ST_SEQ];
    if (!memcmp(st_new, st_pub, PVA_STATUS_SIZE)) return;
    seq = (uint8_t)(seq | 1u);
    pva_win[PVA_R_STATUS + PVA_ST_SEQ] = seq;
    pva_barrier();
    for (uint32_t i = 1; i < PVA_STATUS_SIZE; i++)
        if (st_new[i] != st_pub[i]) pva_win[PVA_R_STATUS + i] = st_new[i];
    memcpy(st_pub, st_new, PVA_STATUS_SIZE);
    pva_barrier();
    seq = (uint8_t)(seq + 1u);
    pva_win[PVA_R_STATUS + PVA_ST_SEQ] = seq;
    st_pub[PVA_ST_SEQ] = seq;
}

// ---------------------------------------------------------------- core 1
void pva_core1_init(void)
{
    dir_ent = pva_alloc(DIR_MAX * sizeof(dent_t));
    dir_pool = pva_alloc(DIR_POOL);
    pl_off = pva_alloc(PL_MAX * sizeof(uint16_t));
    pl_order = pva_alloc(PL_MAX * sizeof(uint16_t));
    pl_pool = pva_alloc(PL_POOL);
    reply = pva_alloc(PVA_REPLY_SIZE);
    bool dec_ok = pva_dec_init();
    bool hal_ok = pva_hal_audio_init();
    pva_hal_audio_mute(true);
    pva_log("engine %d.%d start, arena free %lu", PVA_FW_MAJOR, PVA_FW_MINOR, (unsigned long)pva_arena_free());
    if (!dir_ent || !dir_pool || !pl_off || !pl_order || !pl_pool || !reply || !dec_ok)
        pva_log("ERROR: arena too small");
    if (!hal_ok)
        pva_log("ERROR: I2S audio init failed");
    rng ^= pva_hal_millis();
    do_mount();
    handled_post = pva_mailbox.post_count;
    last_poll_ms = pva_hal_millis();
    publish_status();
    pva_win[PVA_R_BUSY] = 0;
}

bool pva_core1_busy(void) { return state == PVA_STATE_PLAYING; }

bool pva_core1_poll(void)
{
    bool work = false;
    uint32_t now = pva_hal_millis();
    uint32_t gap = now - last_poll_ms;
    if (gap > max_gap_ms) max_gap_ms = gap;
    last_poll_ms = now;

    if (pva_mailbox.post_count != handled_post)
    {
        handle_command();
        work = true;
    }

    if (state == PVA_STATE_PLAYING)
    {
        int r = pva_dec_step();
        if (r != 0) work = true;
        if (r < 0)
        {
            uint8_t fr = pva_dec_last_fresult();
            if (fr) { play_error = PVA_E_IO; }
            track_ended();
        }
    }

    if (now - last_hb_ms >= HEARTBEAT_MS)
    {
        last_hb_ms = now;
        pva_win[PVA_R_HEARTBEAT] = ++hb;
    }
    if (status_dirty || now - last_status_ms >= STATUS_MS)
    {
        last_status_ms = now;
        status_dirty = false;
        publish_status();
    }
    return work;
}
