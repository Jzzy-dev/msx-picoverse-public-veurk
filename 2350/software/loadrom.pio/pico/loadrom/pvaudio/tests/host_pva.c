// Host test for the PicoVerse Audio Engine.
//
// Builds a FAT16 image with FatFS, then drives the register window the way
// the SymbOS player does: commands through ARGS/CMD_TOKEN/CMD_CODE, replies
// polled through DONE_TOKEN, the status block read with its sequence byte.
// Core 1 runs "concurrently": it is polled between MSX accesses.
//
// usage: host_pva <assets dir>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "ff.h"
#include "../pva.h"
#include "../pva_internal.h"

int  host_disk_open(const char *path, uint32_t create_sectors);
void host_wav_open(const char *path);
void host_wav_close(void);
void host_advance_ms(uint32_t ms);
extern uint32_t host_audio_frames, host_audio_last_rate, host_audio_underruns;
extern bool host_audio_muted, host_audio_unlimited;
extern void (*host_audio_sink)(const int16_t *pcm, uint32_t frames, uint32_t rate);

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } else if (verbose) { printf("  ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int verbose = 1;

// ------------------------------------------------------------------ MSX side
static unsigned access_count;
static void core1_tick(void) { pva_core1_poll(); host_advance_ms(1); }

static uint8_t rd(uint16_t a)
{
    if (++access_count % 100u == 0u) core1_tick();   // a Z80 makes ~100+ accesses per ms
    return pva_mem_read(a);
}
static void wr(uint16_t a, uint8_t v)
{
    if (++access_count % 100u == 0u) core1_tick();
    pva_mem_write(a, v);
}

static uint8_t token;
static uint8_t reply[PVA_REPLY_SIZE + 1];
static uint16_t reply_len;

// one command, waiting like the app does (it checks once per timer tick)
static int cmd(uint8_t code, const void *args, size_t n)
{
    const uint8_t *a = args;
    for (size_t i = 0; i < n; i++) wr((uint16_t)(PVA_WIN_BASE + PVA_R_ARGS + i), a[i]);
    token++;
    wr(PVA_WIN_BASE + PVA_R_CMD_TOKEN, token);
    wr(PVA_WIN_BASE + PVA_R_CMD_CODE, code);
    for (int tick = 0; tick < 2000; tick++)
    {
        if (rd(PVA_WIN_BASE + PVA_R_DONE_TOKEN) == token)
        {
            int r = rd(PVA_WIN_BASE + PVA_R_RESULT);
            reply_len = (uint16_t)(rd(PVA_WIN_BASE + PVA_R_REPLY_LEN) | (rd(PVA_WIN_BASE + PVA_R_REPLY_LEN + 1) << 8));
            for (uint16_t i = 0; i < reply_len && i < PVA_REPLY_SIZE; i++) reply[i] = rd((uint16_t)(PVA_WIN_BASE + PVA_R_REPLY + i));
            reply[reply_len] = 0;
            return r;
        }
        for (int k = 0; k < 20; k++) core1_tick();   // 20 ms between checks
    }
    return -1;
}

static int cmd_str(uint8_t code, const char *s) { return cmd(code, s, strlen(s) + 1); }
static int cmd_u16(uint8_t code, uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; return cmd(code, b, 2); }
static int cmd_u8(uint8_t code, uint8_t v) { return cmd(code, &v, 1); }

static uint8_t st[PVA_STATUS_SIZE];
static void read_status(void)
{
    for (int tries = 0; tries < 10; tries++)
    {
        uint8_t s1 = rd(PVA_WIN_BASE + PVA_R_STATUS);
        for (unsigned i = 0; i < PVA_STATUS_SIZE; i++) st[i] = rd((uint16_t)(PVA_WIN_BASE + PVA_R_STATUS + i));
        uint8_t s2 = rd(PVA_WIN_BASE + PVA_R_STATUS);
        if (s1 == s2 && !(s1 & 1)) return;
    }
    printf("  (status never consistent)\n");
}
static uint16_t st16(int o) { return (uint16_t)(st[o] | (st[o + 1] << 8)); }
static const char *ststr(int o) { return (const char *)&st[o]; }

typedef struct { uint8_t type; uint32_t size; char name[PVA_NAME_MAX + 1]; } entry_t;
static entry_t ents[300];
static int list_dir(uint16_t count)
{
    int n = 0;
    while (n < count)
    {
        uint8_t a[3] = { (uint8_t)n, (uint8_t)(n >> 8), 40 };
        if (cmd(PVA_C_DIR_READ, a, 3) != PVA_OK) return -1;
        if (!reply_len) break;
        for (uint16_t p = 0; p < reply_len && n < 300; )
        {
            entry_t *e = &ents[n++];
            e->type = reply[p];
            e->size = reply[p + 1] | (reply[p + 2] << 8) | ((uint32_t)reply[p + 3] << 16) | ((uint32_t)reply[p + 4] << 24);
            uint8_t l = reply[p + 5];
            memcpy(e->name, &reply[p + 6], l);
            e->name[l] = 0;
            p += 6u + l;
        }
    }
    return n;
}
static int find(int n, const char *name) { for (int i = 0; i < n; i++) if (!strcmp(ents[i].name, name)) return i; return -1; }

static void run_ms(uint32_t ms) { for (uint32_t i = 0; i < ms; i++) core1_tick(); }

// ------------------------------------------------------------------ image
static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("missing %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); *len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len);
    if (fread(b, 1, *len, f) != *len) exit(2);
    fclose(f);
    return b;
}
static void put(const char *dst, const void *data, size_t len)
{
    FIL f; UINT bw;
    if (f_open(&f, dst, FA_CREATE_ALWAYS | FA_WRITE) || f_write(&f, data, (UINT)len, &bw) || bw != len) { printf("write %s failed\n", dst); exit(2); }
    f_close(&f);
}
static const char *assets;
static void put_file(const char *dst, const char *src)
{
    char p[512]; size_t len;
    snprintf(p, sizeof p, "%s/%s", assets, src);
    uint8_t *b = slurp(p, &len);
    put(dst, b, len);
    free(b);
}

static void build_image(void)
{
    static FATFS fs;
    static BYTE work[4096];
    host_disk_open("pva_test.img", 64 * 2048);             // 64 MB
    MKFS_PARM opt = { FM_FAT, 0, 0, 0, 0 };                // FAT16, as Nextor uses
    if (f_mkfs("0:", &opt, work, sizeof work)) { printf("mkfs failed\n"); exit(2); }
    f_mount(&fs, "0:", 1);
    f_mkdir("0:/MUSIC");
    f_mkdir("0:/MUSIC/Caf\xC3\xA9 del Mar");                // UTF-8 folder name
    f_mkdir("0:/MUSIC/Empty");
    f_mkdir("0:/SYMBOS");
    f_mkdir("0:/System Volume Information");
    f_chmod("0:/System Volume Information", AM_HID | AM_SYS, AM_HID | AM_SYS);
    put_file("0:/MUSIC/Caf\xC3\xA9 del Mar/01 Sine.mp3", "cbr128_v23.mp3");
    put_file("0:/MUSIC/Caf\xC3\xA9 del Mar/02 VBR.mp3", "vbr_v24.mp3");
    put_file("0:/MUSIC/Caf\xC3\xA9 del Mar/03 Mono.mp3", "mono22_v1.mp3");
    put_file("0:/MUSIC/Caf\xC3\xA9 del Mar/04 Tone.wav", "tone.wav");
    put_file("0:/MUSIC/long.mp3", "long192.mp3");
    put_file("0:/MUSIC/readme.txt", "tone.wav");
    put_file("0:/MUSIC/._long.mp3", "tone.wav");
    put("0:/SYMBOS/SYM.COM", "x", 1);
    const char *m3u =
        "\xEF\xBB\xBF#EXTM3U\r\n"
        "#EXTINF:20,Sine\r\n"
        "Caf\xC3\xA9 del Mar/01 Sine.mp3\r\n"
        "\r\n"
        "missing track.mp3\r\n"
        "C:\\Users\\me\\Music\\long.mp3\r\n"
        "http://example.com/stream.mp3\r\n"
        "/MUSIC/Caf\xC3\xA9 del Mar/04 Tone.wav\r\n";
    put("0:/MUSIC/Mix.m3u", m3u, strlen(m3u));
    // many files in one folder
    f_mkdir("0:/MUSIC/Many");
    uint8_t *b; size_t bl; char p[300];
    snprintf(p, sizeof p, "%s/mono22_v1.mp3", assets);
    b = slurp(p, &bl);
    for (int i = 0; i < 120; i++)
    {
        snprintf(p, sizeof p, "0:/MUSIC/Many/Track %03d with a rather long file name for paging.mp3", 119 - i);
        put(p, b, bl);
    }
    free(b);
    f_mount(NULL, "0:", 0);
}

// ------------------------------------------------------------------ audio check
static int64_t sink_sum_abs;
static uint32_t sink_frames;
static void sink(const int16_t *pcm, uint32_t frames, uint32_t rate)
{
    (void)rate;
    for (uint32_t i = 0; i < frames * 2; i++) sink_sum_abs += pcm[i] < 0 ? -pcm[i] : pcm[i];
    sink_frames += frames;
}
static double sink_level(void) { double v = sink_frames ? (double)sink_sum_abs / (sink_frames * 2.0) : 0; sink_sum_abs = 0; sink_frames = 0; return v; }

// ------------------------------------------------------------------ tests
int main(int argc, char **argv)
{
    assets = argc > 1 ? argv[1] : "assets";
    build_image();

    static uint8_t arena[132 * 1024];
    pva_init(arena, sizeof arena);
    pva_core1_init();
    host_audio_sink = sink;

    printf("[identification and diagnostics]\n");
    char id[9];
    for (int i = 0; i < 8; i++) id[i] = (char)rd((uint16_t)(PVA_WIN_BASE + i));
    id[8] = 0;
    CHECK(!strcmp(id, "PVAUDIO"), "ID '%s', API %u", id, rd(PVA_WIN_BASE + 8));
    CHECK(rd(PVA_WIN_BASE + 0xFFF) == 0xFF && rd(0x5000) == 0xFF && rd(0x3FFF) == 0xFF, "outside the window reads FFh");
    uint8_t hb0 = rd(PVA_WIN_BASE + PVA_R_HEARTBEAT);
    run_ms(50);
    CHECK((uint8_t)(rd(PVA_WIN_BASE + PVA_R_HEARTBEAT) - hb0) >= 5, "heartbeat runs");
    CHECK(cmd(PVA_C_PING, NULL, 0) == PVA_OK && !strncmp((char *)reply, "PVAUDIO ", 8), "PING -> '%s'", reply);
    read_status();
    CHECK(st[PVA_ST_FLAGS] & PVA_F_MOUNTED, "card mounted at start, mount FRESULT %u", st[PVA_ST_MOUNT_FR]);
    CHECK(cmd(PVA_C_DIAG, NULL, 0) == PVA_OK && strstr((char *)reply, "FAT16"), "DIAG:\n%s", reply);
    CHECK(cmd(0x7E, NULL, 0) == PVA_E_UNKNOWN, "unknown command refused");

    printf("[folders]\n");
    CHECK(cmd_str(PVA_C_DIR_OPEN, "/") == PVA_OK, "open root");
    uint16_t cnt = (uint16_t)(reply[0] | (reply[1] << 8));
    CHECK(!strcmp((char *)reply + 2, "/"), "path '/'");
    int n = list_dir(cnt);
    CHECK(n == 2 && !strcmp(ents[0].name, "MUSIC") && !strcmp(ents[1].name, "SYMBOS"), "root: %d entries (hidden folder left out)", n);
    CHECK(cmd_str(PVA_C_DIR_OPEN, "\\MUSIC\\") == PVA_OK, "open \\MUSIC\\ (backslashes, trailing slash)");
    cnt = (uint16_t)(reply[0] | (reply[1] << 8));
    n = list_dir(cnt);
    for (int i = 0; i < n; i++) printf("    %d %-40s %u\n", ents[i].type, ents[i].name, (unsigned)ents[i].size);
    CHECK(n == 6 && ents[0].type == PVA_DE_PARENT, "MUSIC: %d entries, '..' first", n);
    int cafe = find(n, "Cafe del Mar");
    CHECK(cafe == 1 && ents[cafe].type == PVA_DE_DIR, "UTF-8 folder shown as 'Cafe del Mar', folders first");
    CHECK(find(n, "readme.txt") < 0 && find(n, "._long.mp3") < 0, "non-audio and ._ files left out");
    CHECK(find(n, "Mix.m3u") > 0 && ents[find(n, "Mix.m3u")].type == PVA_DE_M3U, "M3U listed");
    CHECK(cmd_u16(PVA_C_DIR_ENTER, (uint16_t)cafe) == PVA_OK, "enter the UTF-8 folder by index");
    cnt = (uint16_t)(reply[0] | (reply[1] << 8));
    CHECK(!strcmp((char *)reply + 2, "/MUSIC/Cafe del Mar"), "path shown '%s'", reply + 2);
    n = list_dir(cnt);
    CHECK(n == 5 && find(n, "01 Sine.mp3") == 1 && ents[4].type == PVA_DE_WAV, "4 tracks + '..'");
    CHECK(cmd_u16(PVA_C_DIR_ENTER, 0) == PVA_OK && !strcmp((char *)reply + 2, "/MUSIC"), "'..' goes back to /MUSIC");
    n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
    int many = find(n, "Many");
    CHECK(cmd_u16(PVA_C_DIR_ENTER, (uint16_t)many) == PVA_OK, "enter Many");
    cnt = (uint16_t)(reply[0] | (reply[1] << 8));
    n = list_dir(cnt);
    CHECK(cnt == 121 && n == 121 && !strncmp(ents[1].name, "Track 000", 9) && !strncmp(ents[120].name, "Track 119", 9),
          "121 entries over several DIR_READ pages, sorted ('%s' .. '%s')", ents[1].name, ents[120].name);
    CHECK(strlen(ents[5].name) <= PVA_NAME_MAX, "long names cut to %d", PVA_NAME_MAX);
    CHECK(cmd_str(PVA_C_DIR_OPEN, "/NOPE") == PVA_E_NOTFOUND, "missing folder -> NOTFOUND");
    CHECK(cmd_str(PVA_C_DIR_OPEN, "") == PVA_OK && !strcmp((char *)reply + 2, "/MUSIC/Many"), "empty path reopens the current folder");
    CHECK(cmd_u16(PVA_C_DIR_ENTER, 999) == PVA_E_RANGE, "index out of range");

    printf("[playback in real time, browsing while playing]\n");
    host_audio_unlimited = false;
    cmd_str(PVA_C_DIR_OPEN, "/MUSIC/Caf\xC3\xA9 del Mar");
    n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
    CHECK(cmd_u16(PVA_C_PLAY_INDEX, 1) == PVA_OK, "play '01 Sine.mp3'");
    run_ms(200);
    read_status();
    CHECK(st[PVA_ST_STATE] == PVA_STATE_PLAYING, "state playing");
    CHECK(!strcmp(ststr(PVA_ST_TITLE), "Sine Cafe Song") && !strcmp(ststr(PVA_ST_ARTIST), "Unit Tester") && !strcmp(ststr(PVA_ST_ALBUM), "PicoVerse Tests"),
          "ID3v2.3 tags: '%s' / '%s' / '%s'", ststr(PVA_ST_TITLE), ststr(PVA_ST_ARTIST), ststr(PVA_ST_ALBUM));
    CHECK(st16(PVA_ST_TOTAL) >= 19 && st16(PVA_ST_TOTAL) <= 21 && st16(PVA_ST_BITRATE) == 128 && st16(PVA_ST_RATE) == 4410 && st[PVA_ST_CHANNELS] == 2,
          "20 s, %u kbit/s, %u Hz, %u ch", st16(PVA_ST_BITRATE), st16(PVA_ST_RATE) * 10, st[PVA_ST_CHANNELS]);
    CHECK(st16(PVA_ST_PL_COUNT) == 4 && st16(PVA_ST_PL_POS) == 0, "play list = the 4 tracks of the folder");
    CHECK(!host_audio_muted, "output unmuted");
    uint32_t f0 = host_audio_frames, t0 = pva_hal_millis();
    for (int s = 0; s < 5; s++)
    {
        run_ms(1000);
        cmd_str(PVA_C_DIR_OPEN, "/MUSIC/Many");                // browse while playing
        list_dir(121);
        cmd_str(PVA_C_DIR_OPEN, "/MUSIC");
    }
    read_status();
    uint32_t played = host_audio_frames - f0, ms = pva_hal_millis() - t0;
    CHECK(host_audio_underruns == 0, "no audio underruns while browsing (%u)", host_audio_underruns);
    uint32_t expect = (uint32_t)((uint64_t)44100u * ms / 1000u);
    CHECK(played + 6000u > expect && played < expect + 6000u, "audio kept pace with real time: %u frames in %u ms", played, ms);
    CHECK(st16(PVA_ST_ELAPSED) + 1u >= ms / 1000u && st16(PVA_ST_ELAPSED) <= ms / 1000u + 1u, "elapsed %u s", st16(PVA_ST_ELAPSED));
    CHECK(st[PVA_ST_VU_L] > 3 && st[PVA_ST_VU_R] > 3, "VU %u/%u", st[PVA_ST_VU_L], st[PVA_ST_VU_R]);

    printf("[pause, volume, seek]\n");
    CHECK(cmd(PVA_C_PAUSE, NULL, 0) == PVA_OK, "pause");
    read_status();
    uint16_t e0 = st16(PVA_ST_ELAPSED);
    run_ms(2000);
    read_status();
    CHECK(st[PVA_ST_STATE] == PVA_STATE_PAUSED && st16(PVA_ST_ELAPSED) == e0 && host_audio_muted, "paused, time stands still");
    CHECK(cmd(PVA_C_PAUSE, NULL, 0) == PVA_OK, "resume");
    run_ms(500);
    sink_level();
    run_ms(500);
    double full = sink_level();
    CHECK(cmd_u8(PVA_C_VOLUME, 40) == PVA_OK, "volume 40");
    run_ms(500);
    sink_level();
    run_ms(500);
    double low = sink_level();
    CHECK(low < full * 0.1 && low > full * 0.01, "volume 40 is about 30 dB down (%.0f -> %.0f)", full, low);
    cmd_u8(PVA_C_VOLUME, 80);
    CHECK(cmd_u16(PVA_C_SEEK, 15) == PVA_OK, "seek to 15 s");
    run_ms(300);
    read_status();
    CHECK(st16(PVA_ST_ELAPSED) >= 15 && st16(PVA_ST_ELAPSED) <= 16, "elapsed %u s after seek", st16(PVA_ST_ELAPSED));

    printf("[end of track -> next, VBR/Xing, mono 22 kHz, ID3v1, WAV]\n");
    uint16_t tid = st16(PVA_ST_TRACK_ID);
    run_ms(6000);
    read_status();
    CHECK(st16(PVA_ST_TRACK_ID) == tid + 1 && st16(PVA_ST_PL_POS) == 1, "advanced to track 2");
    CHECK(!strcmp(ststr(PVA_ST_TITLE), "VBR Four") && st16(PVA_ST_TOTAL) >= 14 && st16(PVA_ST_TOTAL) <= 16 && st16(PVA_ST_RATE) == 4800,
          "VBR: ID3v2.4 '%s', %u s from the Xing header, %u Hz", ststr(PVA_ST_TITLE), st16(PVA_ST_TOTAL), st16(PVA_ST_RATE) * 10);
    CHECK(cmd(PVA_C_NEXT, NULL, 0) == PVA_OK, "next");
    run_ms(300);
    read_status();
    CHECK(!strcmp(ststr(PVA_ST_TITLE), "Mono V1") && st[PVA_ST_CHANNELS] == 1 && st16(PVA_ST_RATE) == 2205, "ID3v1 '%s', mono 22050 Hz", ststr(PVA_ST_TITLE));
    CHECK(cmd(PVA_C_NEXT, NULL, 0) == PVA_OK, "next");
    run_ms(300);
    read_status();
    CHECK(!strcmp(ststr(PVA_ST_TITLE), "04 Tone") && st[PVA_ST_FORMAT] == PVA_DE_WAV && st16(PVA_ST_TOTAL) == 5, "WAV, title from file name '%s'", ststr(PVA_ST_TITLE));
    run_ms(5500);
    read_status();
    CHECK(st[PVA_ST_STATE] == PVA_STATE_STOPPED, "end of the play list -> stopped");
    CHECK(host_audio_underruns == 0, "still no underruns (%u)", host_audio_underruns);

    printf("[prev, repeat, shuffle]\n");
    CHECK(cmd(PVA_C_PREV, NULL, 0) == PVA_OK, "prev from stopped");
    run_ms(200);
    read_status();
    CHECK(st16(PVA_ST_PL_POS) == 2 && st[PVA_ST_STATE] == PVA_STATE_PLAYING, "track 3");
    run_ms(4000);
    CHECK(cmd(PVA_C_PREV, NULL, 0) == PVA_OK, "prev after 4 s restarts the track");
    run_ms(200);
    read_status();
    CHECK(st16(PVA_ST_PL_POS) == 2 && st16(PVA_ST_ELAPSED) <= 1, "same track from the start");
    CHECK(cmd_u8(PVA_C_MODE, PVA_MODE_REPEAT_ONE) == PVA_OK, "repeat one");
    tid = st16(PVA_ST_TRACK_ID);
    run_ms(9000);
    read_status();
    CHECK(st16(PVA_ST_PL_POS) == 2 && st16(PVA_ST_TRACK_ID) == tid + 1, "track repeated");
    CHECK(cmd_u8(PVA_C_MODE, PVA_MODE_SHUFFLE | PVA_MODE_REPEAT_ALL) == PVA_OK, "shuffle + repeat all");
    read_status();
    CHECK(st16(PVA_ST_PL_POS) == 0 && st[PVA_ST_MODE] == (PVA_MODE_SHUFFLE | PVA_MODE_REPEAT_ALL), "current track stays first in the shuffled order");
    int seen[4] = { 0 };
    for (int i = 0; i < 8; i++)
    {
        cmd(PVA_C_NEXT, NULL, 0);
        run_ms(100);
        read_status();
        const char *t = ststr(PVA_ST_TITLE);
        if (!strcmp(t, "Sine Cafe Song")) seen[0]++;
        else if (!strcmp(t, "VBR Four")) seen[1]++;
        else if (!strcmp(t, "Mono V1")) seen[2]++;
        else if (!strcmp(t, "04 Tone")) seen[3]++;
    }
    CHECK(seen[0] && seen[1] && seen[2] && seen[3], "every track came up in shuffle (%d %d %d %d)", seen[0], seen[1], seen[2], seen[3]);
    cmd_u8(PVA_C_MODE, 0);
    CHECK(cmd(PVA_C_STOP, NULL, 0) == PVA_OK, "stop");
    read_status();
    CHECK(st[PVA_ST_STATE] == PVA_STATE_STOPPED && host_audio_muted && !st[PVA_ST_TITLE], "stopped and muted");

    printf("[M3U play list]\n");
    cmd_str(PVA_C_DIR_OPEN, "/MUSIC");
    n = list_dir((uint16_t)(reply[0] | (reply[1] << 8)));
    CHECK(cmd_u16(PVA_C_PLAY_INDEX, (uint16_t)find(n, "Mix.m3u")) == PVA_OK, "play Mix.m3u");
    run_ms(200);
    read_status();
    CHECK(st16(PVA_ST_PL_COUNT) == 4 && st[PVA_ST_PL_KIND] == 1, "4 tracks (comments, blank line and URL skipped), %u", st16(PVA_ST_PL_COUNT));
    CHECK(!strcmp(ststr(PVA_ST_TITLE), "Sine Cafe Song"), "relative UTF-8 path plays");
    cmd(PVA_C_NEXT, NULL, 0);
    run_ms(200);
    read_status();
    CHECK(!strcmp(ststr(PVA_ST_TITLE), "Long One") && st16(PVA_ST_PL_POS) == 2, "missing entry skipped, Windows path found by name");
    CHECK(st16(PVA_ST_TOTAL) >= 239 && st16(PVA_ST_TOTAL) <= 241, "CBR 192k length %u s", st16(PVA_ST_TOTAL));
    cmd(PVA_C_NEXT, NULL, 0);
    run_ms(200);
    read_status();
    CHECK(!strcmp(ststr(PVA_ST_TITLE), "04 Tone"), "absolute path plays");
    cmd(PVA_C_STOP, NULL, 0);

    printf("[play by path, errors]\n");
    CHECK(cmd_str(PVA_C_PLAY_PATH, "/MUSIC/long.mp3") == PVA_OK, "PLAY_PATH");
    run_ms(100);
    CHECK(cmd_u16(PVA_C_SEEK, 200) == PVA_OK, "seek in a long CBR file");
    run_ms(300);
    read_status();
    CHECK(st16(PVA_ST_ELAPSED) >= 200 && st16(PVA_ST_ELAPSED) <= 201, "elapsed %u", st16(PVA_ST_ELAPSED));
    CHECK(cmd_str(PVA_C_PLAY_PATH, "/MUSIC/none.mp3") == PVA_E_NOTFOUND, "missing file -> NOTFOUND");
    read_status();
    CHECK(st[PVA_ST_STATE] == PVA_STATE_STOPPED, "stopped after the failed play");
    CHECK(cmd_str(PVA_C_PLAY_PATH, "relative.mp3") == PVA_E_ARGS, "relative path refused");
    CHECK(cmd(PVA_C_LOG, NULL, 0) == PVA_OK && strstr((char *)reply, "open"), "LOG has the failures:\n%s", reply);

    printf("[card removed / remounted]\n");
    host_disk_open("/nonexistent/none.img", 0);
    CHECK(cmd(PVA_C_MOUNT, NULL, 0) == PVA_E_NOCARD, "MOUNT without a card -> NOCARD");
    read_status();
    CHECK(!(st[PVA_ST_FLAGS] & PVA_F_MOUNTED) && st[PVA_ST_MOUNT_FR] == FR_NOT_READY, "status shows the mount FRESULT %u", st[PVA_ST_MOUNT_FR]);
    CHECK(cmd_str(PVA_C_DIR_OPEN, "/") == PVA_E_NOCARD, "DIR_OPEN without a card -> NOCARD");
    host_disk_open("pva_test.img", 0);
    CHECK(cmd_str(PVA_C_DIR_OPEN, "/MUSIC") == PVA_OK, "DIR_OPEN mounts the card again");

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
