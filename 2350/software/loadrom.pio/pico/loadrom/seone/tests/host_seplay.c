// Host test for the SE-ONE emulation.
//
// Builds a FAT16 image with FatFS, then drives the emulated ports exactly the
// way SE Play 0.1 (SEPLAY.EXE, SymbOS) does, as reconstructed from its Z80
// code: ATSEND/ATRECV (0x5A2D/0x5A98), the two-pass MSDFILES reader (0x1E5E),
// M3U row handling (0x2568, 0x274A), status polling (0x103B) and the TNC
// network-card probe (0x4FEC). Core 1 is simulated by calling
// seone_core1_poll() only every few port polls.
//
// usage: host_seplay <dir with SE-ONE USB_FILES>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "ff.h"
#include "../seone.h"
#include "../seone_internal.h"

int  host_disk_open(const char *path, uint32_t create_sectors);
void host_wav_open(const char *path);
void host_wav_close(void);
void host_advance_ms(uint32_t ms);
extern uint32_t host_audio_frames, host_audio_last_rate, host_disk_reads;
extern bool host_audio_queue_full;

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ------------------------------------------------------------------ ports
static unsigned poll_counter;
static void core1_tick(void) { seone_core1_poll(); host_advance_ms(1); }

static uint8_t inp(uint8_t port)
{
    if (++poll_counter % 7u == 0u) core1_tick();     // core 1 runs concurrently
    uint8_t v = 0xFF;
    if (seone_port_claimed(port) && !seone_io_read(port, &v)) v = 0xFF;
    return v;
}

static void outp(uint8_t port, uint8_t v)
{
    if (seone_port_claimed(port)) seone_io_write(port, v);
}

// ------------------------------------------------------------------ SE Play ATSEND / ATRECV
static char RESP[64], RVAL[64];

static int atsend(const char *fmt, ...)
{
    char cmd[400];
    va_list ap; va_start(ap, fmt); vsnprintf(cmd, sizeof cmd, fmt, ap); va_end(ap);
    for (const char *c = cmd; *c; c++)
        if (*c != ' ') outp(0x20, (uint8_t)*c);           // spaces are never sent
    outp(0x20, 13);

    int i = 0, flag = 0, done = 0, cnt = 0, ri = 0, vi = 0;
    while (!done)
    {
        int d = inp(0x20);
        if (d > 31 && i < 50)
        {
            RESP[ri++] = (char)d;
            if (flag) RVAL[vi++] = (char)d;
            if (d == '=') flag = 1;
            i++;
        }
        if (d == 13 || d == 10) done = 1;
        if (i > 50) done = 2;
        if (cnt > 5000) done = 2;
        cnt++;
    }
    RESP[ri] = 0; RVAL[vi] = 0;
    if (done == 2) strcpy(RESP, "AT+ERR");
    return done;
}

// ------------------------------------------------------------------ SE Play file list (two passes)
static char LIST[64][64];
static int  LIST_N;

static void rtrim(char *s) { size_t l = strlen(s); while (l && (unsigned char)s[l - 1] <= 32) s[--l] = 0; }

static void read_pass(int want_dirs, int m3u_mode)
{
    char t[300]; int tl = 0;
    int guard = 0;
    int d = inp(0x25);
    while (d != 0 && guard++ < 20000)
    {
        if (d == 9 && LIST_N < 49)
        {
            t[tl] = 0; rtrim(t);
            int is_dir = t[0] == '/';
            size_t l = strlen(t);
            int is_m3u = l >= 4 && strcmp(t + l - 4, ".M3U") == 0;
            int take = want_dirs ? is_dir : (!is_dir && (m3u_mode ? is_m3u : !is_m3u));
            if (take) strcpy(LIST[LIST_N++], t);
            tl = 0;
        }
        else if (tl < 299) t[tl++] = (char)d;
        d = inp(0x25);
    }
}

static void list_dir(int m3u_mode)
{
    LIST_N = 0;
    atsend("AT+MSDFILES");
    read_pass(1, m3u_mode);
    atsend("AT+MSDFILES");
    read_pass(0, m3u_mode);
    strcpy(LIST[LIST_N], "_END_");
}

static int list_has(const char *name)
{
    for (int i = 0; i < LIST_N; i++) if (strcmp(LIST[i], name) == 0) return 1;
    return 0;
}

static void list_print(void)
{
    printf("    list:");
    for (int i = 0; i < LIST_N; i++) printf(" [%s]", LIST[i]);
    printf("\n");
}

// SE Play's own short-name guess for an M3U row (0x274A)
static void derive(const char *row, char *out)
{
    size_t len = strlen(row);
    if (len <= 10) { strcpy(out, row); return; }
    char t[8]; int n = 0;
    for (size_t i = 0; i < len && i < 20 && n < 6; i++) if (row[i] != ' ') t[n++] = row[i];
    t[n] = 0;
    sprintf(out, "%s~1%s", t, row + len - 4);
}

static uint8_t status(void) { return inp(0x23); }

static uint32_t run_until_end(uint32_t max_ticks)
{
    uint32_t t = 0;
    while (!(status() & SEONE_ST_END_OF_MUSIC) && t < max_ticks) { core1_tick(); t++; }
    return t;
}

// ------------------------------------------------------------------ image
static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("missing %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); *len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len); fread(b, 1, *len, f); fclose(f);
    return b;
}

static void put(const char *dst, const void *data, size_t len)
{
    FIL f; UINT bw;
    FRESULT fr = f_open(&f, dst, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr) { printf("f_open %s: %d\n", dst, fr); exit(2); }
    f_write(&f, data, (UINT)len, &bw);
    f_close(&f);
}

static void put_file(const char *dst, const char *src_dir, const char *src)
{
    char p[512]; size_t len;
    snprintf(p, sizeof p, "%s/%s", src_dir, src);
    uint8_t *b = slurp(p, &len);
    put(dst, b, len);
    free(b);
}

static void build_image(const char *files)
{
    static FATFS fs;
    static BYTE work[4096];
    host_disk_open("test.img", 128 * 2048);               // 128 MB
    MKFS_PARM opt = { FM_FAT, 0, 0, 0, 0 };                // FAT16, as Nextor needs
    if (f_mkfs("0:", &opt, work, sizeof work)) { printf("mkfs failed\n"); exit(2); }
    f_mount(&fs, "0:", 1);

    f_mkdir("0:/MUSIC");
    f_mkdir("0:/MUSIC/Sandra Collection");
    f_mkdir("0:/MUSIC/Sandra Collection/bonus");
    f_mkdir("0:/SAMPLE");
    f_mkdir("0:/SETALK");
    f_mkdir("0:/System Volume Information");
    f_chmod("0:/System Volume Information", AM_HID | AM_SYS, AM_HID | AM_SYS);

    put_file("0:/MUSIC/Sandra Collection/01 Sandra - Maria Magdalena.mp3", files, "AUDIO/test.mp3");
    put_file("0:/MUSIC/Sandra Collection/02 Sandra - In The Heat Of The Night.mp3", files, "AUDIO/SE_CPC.MP3");
    put_file("0:/MUSIC/Sandra Collection/bonus/03 Bonus track number three.mp3", files, "AUDIO/test.mp3");
    put_file("0:/MUSIC/Sandra Collection/04 A very long title that goes on and on and on forever.mp3", files, "AUDIO/test.mp3");
    put_file("0:/MUSIC/short.mp3", files, "AUDIO/test.mp3");
    put_file("0:/MUSIC/test.wav", files, "AUDIO/test.wav");
    put_file("0:/MUSIC/readme.txt", files, "SAMPLE/readme.txt");
    put_file("0:/MUSIC/._short.mp3", files, "SAMPLE/readme.txt");
    put_file("0:/ROOT.MP3", files, "AUDIO/SE_CPC.MP3");
    put_file("0:/SAMPLE/BELL.MP3", files, "SAMPLE/BELL.MP3");
    put_file("0:/SETALK/SP02CH.MP3", files, "SETALK/SP02CH.MP3");
    put_file("0:/SETALK/SP000H.mp3", files, "SETALK/SP000H.mp3");

    const char *m3u =
        "\xEF\xBB\xBF#EXTM3U\r\n"
        "#EXTINF:238,01 Sandra - Maria Magdalena\r\n"
        "01 Sandra - Maria Magdalena.mp3\r\n"
        "\r\n"
        "#EXTINF:239,02 Sandra - In The Heat Of The Night\r\n"
        "C:\\Users\\me\\Music\\Sandra\\02 Sandra - In The Heat Of The Night.mp3\r\n"
        "#EXTINF:100,03 Bonus\r\n"
        "bonus/03 Bonus track number three.mp3\r\n"
        "#EXTINF:100,04 Long\r\n"
        "04 A very long title that goes on and on and on forever.mp3\r\n";
    put("0:/MUSIC/Sandra Collection/Sandra.m3u", m3u, strlen(m3u));

    // a big playlist: 1000 entries, only the last one exists
    static char big[200000];
    size_t bl = 0;
    bl += sprintf(big + bl, "#EXTM3U\r\n");
    for (int i = 1; i < 1000; i++)
        bl += sprintf(big + bl, "#EXTINF:100,Artist %04d - Some Title\r\nMissing Artist %04d - Some Title.mp3\r\n", i, i);
    bl += sprintf(big + bl, "#EXTINF:100,Last\r\nshort.mp3\r\n");
    put("0:/MUSIC/Big.m3u", big, bl);
    f_mount(NULL, "0:", 0);
}

// ------------------------------------------------------------------ tests
int main(int argc, char **argv)
{
    const char *files = argc > 1 ? argv[1] : "USB_FILES";
    build_image(files);

    static uint8_t arena[128 * 1024];
    seone_init(arena, sizeof arena);
    seone_core1_init();

    if (argc > 2 && !strcmp(argv[2], "--ref"))
    {
        // decode /MUSIC/short.mp3 at 0 dB, flat tone, for comparison with a reference decoder
        host_wav_open("ref_out.wav");
        atsend("AT+SEMODE=MP3B");
        atsend("AT+DSPVOLL=100"); atsend("AT+DSPVOLR=100");
        atsend("AT+MSDPATH=0:/MUSIC");
        atsend("AT+MSDPLAY=short.mp3");
        run_until_end(400000);
        host_wav_close();
        return 0;
    }
    host_wav_open("out.wav");

    printf("[TNC probe at start-up]\n");
    {
        CHECK(inp(0x41) == 0xFF, "TNC port must float before it is used");
        outp(0x41, 0); outp(0x42, 0x1A); outp(0x42, 0); outp(0x41, 0x48);
        int w = 0; while (inp(0x41) == 1 && w < 100000) w++;
        CHECK(w < 100000, "status never left busy");
        char id[300]; int n = 0, d;
        while ((d = inp(0x42)) > 0 && n < 299) id[n++] = (char)d;
        id[n] = 0;
        CHECK(n == 0, "ID string should be empty, got %d bytes", n);
        outp(0x40, 0x08);
        CHECK(inp(0x41) == 0xFF, "must stay off the bus while switched-I/O ID 8 is selected");
        outp(0x40, 'A');
    }

    printf("[start-up AT traffic]\n");
    CHECK(atsend("AT+SEMODE=MP3A") == 1 && strcmp(RESP, "OK") == 0, "SEMODE=MP3A -> %s", RESP);
    CHECK(atsend("AT+MSDSTOP") == 1 && strcmp(RESP, "OK") == 0, "MSDSTOP -> %s", RESP);
    atsend("AT+SEMODE?");
    CHECK(strcmp(RESP, "AT+SEMODE=MP3A") == 0 && strcmp(RVAL, "MP3A") == 0, "SEMODE? -> %s / %s", RESP, RVAL);
    atsend("AT+DSPVOLL=50");
    atsend("AT+DSPVOLL?");
    CHECK(strcmp(RVAL, "50") == 0, "DSPVOLL? -> %s", RVAL);
    atsend("AT+DSPBASS=70");
    atsend("AT+DSPTREBLE?");
    CHECK(strcmp(RVAL, "50") == 0, "DSPTREBLE? -> %s", RVAL);
    atsend("AT+BOGUS");
    CHECK(strcmp(RESP, "ERR") == 0, "unknown command -> %s", RESP);
    atsend("AT+SEFAULTNR?");
    CHECK(strcmp(RVAL, "20") == 0, "fault after unknown -> %s", RVAL);
    atsend("at+semode?");
    CHECK(strcmp(RVAL, "MP3A") == 0, "lower case command -> %s", RESP);
    atsend("AT+FMFREQ?");
    CHECK(strcmp(RVAL, "87.5") == 0, "FMFREQ? -> %s", RVAL);

    printf("[MP3 USB page: mode, init, root listing]\n");
    CHECK(atsend("AT+SEMODE=MP3B") == 1 && !strcmp(RESP, "OK"), "MP3B");
    CHECK(atsend("AT+USBINIT") == 1 && !strcmp(RESP, "OK"), "USBINIT -> %s", RESP);
    CHECK(atsend("AT+MSDPATH=1:/") == 1 && !strcmp(RESP, "OK"), "MSDPATH=1:/ -> %s", RESP);
    list_dir(0);
    list_print();
    CHECK(list_has("/MUSIC") && list_has("/SAMPLE") && list_has("/SETALK"), "root dirs");
    CHECK(list_has("ROOT.MP3"), "root file");
    CHECK(!list_has("/System Volume Information"), "hidden/system dir must be skipped");

    printf("[enter /MUSIC]\n");
    char path[256] = "1:";
    strcat(path, "/MUSIC");
    CHECK(atsend("AT+MSDPATH=%s", path) == 1 && !strcmp(RESP, "OK"), "MSDPATH %s -> %s", path, RESP);
    list_dir(0);
    list_print();
    CHECK(list_has("/Sandra Collection"), "LFN directory with a space");
    CHECK(list_has("short.MP3") && list_has("test.WAV"), "files with upper-case extension");
    CHECK(!list_has("readme.TXT") && !list_has("._short.MP3"), "non-audio / dot files filtered");

    printf("[enter '/Sandra Collection' (space stripped by SE Play)]\n");
    strcat(path, "/Sandra Collection");
    CHECK(atsend("AT+MSDPATH=%s", path) == 1 && !strcmp(RESP, "OK"), "MSDPATH %s -> %s", path, RESP);
    list_dir(0);
    list_print();
    CHECK(list_has("/bonus"), "subdir");
    CHECK(list_has("01SAND~1.MP3"), "long name falls back to 8.3");
    list_dir(1);
    list_print();
    CHECK(LIST_N == 2 && !strcmp(LIST[0], "/bonus") && !strcmp(LIST[1], "Sandra.M3U"), "M3U page: folders + playlists only");

    printf("[play a file picked from the list]\n");
    list_dir(0);
    CHECK(atsend("AT+MSDPLAY=%s", "01SAND~1.MP3") == 1 && !strcmp(RESP, "OK"), "MSDPLAY -> %s", RESP);
    CHECK(!(status() & SEONE_ST_END_OF_MUSIC), "bit3 must clear as soon as OK is seen");
    atsend("AT+MSDPLAY?");
    CHECK(!strcmp(RVAL, "BUSY"), "MSDPLAY? -> %s", RVAL);
    atsend("AT+MSDSIZE?");
    CHECK(atoi(RVAL) > 1000, "MSDSIZE -> %s", RVAL);
    uint32_t f0 = host_audio_frames;
    for (int i = 0; i < 60; i++) { core1_tick(); }
    uint8_t vl = inp(0x26), vr = inp(0x27);
    printf("    VU after 60 frames: %u %u, rate %u\n", vl, vr, host_audio_last_rate);
    CHECK(host_audio_frames > f0 + 30000, "audio frames produced (%u)", host_audio_frames - f0);
    CHECK(vl <= 8 && vr <= 8, "VU range");
    atsend("AT+MSDPAUSE");
    uint32_t f1 = host_audio_frames;
    for (int i = 0; i < 20; i++) core1_tick();
    CHECK(host_audio_frames == f1, "paused: no output");
    CHECK(!(status() & SEONE_ST_END_OF_MUSIC), "paused still counts as playing");
    atsend("AT+MSDSTART");
    uint32_t t = run_until_end(200000);
    uint32_t total = host_audio_frames - f0;
    printf("    track finished after %u ticks, %u frames (%.1f s)\n", t, total, total / 44100.0);
    CHECK(status() & SEONE_ST_END_OF_MUSIC, "bit3 set at end of track");
    atsend("AT+MSDPLAY?");
    CHECK(!strcmp(RVAL, "IDLE"), "MSDPLAY? after end -> %s", RVAL);

    printf("[play LFN typed without spaces / with a different case]\n");
    CHECK(atsend("AT+MSDPLAY=02 Sandra - In The Heat Of The Night.mp3") == 1 && !strcmp(RESP, "OK"), "stripped LFN -> %s", RESP);
    atsend("AT+MSDSTOP");
    CHECK(status() & SEONE_ST_END_OF_MUSIC, "stopped");
    CHECK(atsend("AT+MSDPLAY=nothere.mp3") == 1 && !strcmp(RESP, "ERR"), "missing file -> %s", RESP);

    printf("[M3U playlist]\n");
    {
        char m3u_path[300];
        snprintf(m3u_path, sizeof m3u_path, "%s/%s", path, "Sandra.M3U");
        CHECK(atsend("AT+MSDM3UFILE=%s", m3u_path) == 1 && !strcmp(RESP, "OK"), "M3UFILE -> %s", RESP);
        int played = 0;
        for (int row = 1; row < 20; row++)
        {
            atsend("AT+MSDM3UGETROW=%d", row);
            if (!strncmp(RESP, "#END#", 5)) { printf("    row %d: #END#\n", row); break; }
            printf("    row %d: %s\n", row, RESP);
            if (RESP[0] == '#') continue;
            char name[80];
            derive(RESP, name);
            int ok = atsend("AT+MSDPLAY=%s", name) == 1 && !strcmp(RESP, "OK");
            CHECK(ok, "playlist entry '%s' -> %s", name, RESP);
            if (ok) played++;
            atsend("AT+MSDSTOP");
        }
        CHECK(played == 4, "all 4 playlist entries playable (%d)", played);
        atsend("AT+MSDM3UGETROW=99");
        CHECK(!strcmp(RESP, "#END#"), "past end -> %s", RESP);
    }

    printf("[1000-entry playlist stays linear]\n");
    {
        atsend("AT+MSDPATH=0:/MUSIC");
        CHECK(atsend("AT+MSDM3UFILE=0:/MUSIC/Big.M3U") == 1 && !strcmp(RESP, "OK"), "big M3U -> %s", RESP);
        uint32_t r0 = host_disk_reads;
        atsend("AT+MSDM3UGETROW=2001");
        CHECK(!strcmp(RESP, "short.mp3"), "last row -> %s", RESP);
        uint32_t r1 = host_disk_reads;
        CHECK(atsend("AT+MSDPLAY=nosuchfile.mp3") == 1 && !strcmp(RESP, "ERR"), "unknown name scans whole list -> %s", RESP);
        uint32_t r2 = host_disk_reads;
        printf("    sector reads: GETROW %u, failed lookup %u\n", r1 - r0, r2 - r1);
        CHECK(r2 - r1 < 400, "lookup must be one pass over the playlist (%u reads)", r2 - r1);
        atsend("AT+MSDSTOP");
        atsend("AT+MSDM3UFILE=0:/MUSIC/SandraCollection/Sandra.M3U");
    }

    printf("[path navigation edge cases]\n");
    CHECK(atsend("AT+MSDPATH=0:") == 1 && !strcmp(RESP, "OK"), "root as '0:' -> %s", RESP);
    CHECK(atsend("AT+MSDPATH=0:/NOPE") == 1 && !strcmp(RESP, "ERR"), "missing dir -> %s", RESP);
    CHECK(atsend("AT+MSDPATH=0:/ROOT.MP3") == 1 && !strcmp(RESP, "ERR"), "file as dir -> %s", RESP);
    atsend("AT+MSDPATH=0:/music");
    CHECK(!strcmp(RESP, "OK"), "case-insensitive dir -> %s", RESP);
    atsend("AT+MSDPATH?");
    CHECK(!strcmp(RVAL, "0:/music"), "MSDPATH? -> %s", RVAL);

    printf("[WAV file]\n");
    CHECK(atsend("AT+MSDPLAY=test.WAV") == 1 && !strcmp(RESP, "OK"), "WAV -> %s", RESP);
    f0 = host_audio_frames;
    run_until_end(200000);
    printf("    wav frames %u @ %u Hz\n", host_audio_frames - f0, host_audio_last_rate);
    CHECK(host_audio_frames > f0, "WAV produced audio");

    printf("[speech + samples]\n");
    CHECK(atsend("AT+MSDTALK=44") == 1 && !strcmp(RESP, "OK"), "MSDTALK -> %s", RESP);
    CHECK(status() & SEONE_ST_USB_BUSY, "bit5 busy while speaking");
    CHECK(status() & SEONE_ST_END_OF_MUSIC, "speech is not music");
    { int w = 0; while ((status() & SEONE_ST_USB_BUSY) && w++ < 100000) core1_tick(); CHECK(w < 100000, "speech ends"); }
    CHECK(atsend("AT+MSDSETSAMPLE=1@BELL.MP3") == 1 && !strcmp(RESP, "OK"), "SETSAMPLE -> %s", RESP);
    CHECK(atsend("AT+MSDSAMPLE=1") == 1 && !strcmp(RESP, "OK"), "SAMPLE -> %s", RESP);
    { int w = 0; while ((status() & SEONE_ST_USB_BUSY) && w++ < 100000) core1_tick(); }

    printf("[webradio stream through port 0x22]\n");
    {
        size_t len; char p[512];
        snprintf(p, sizeof p, "%s/AUDIO/test.mp3", files);
        uint8_t *mp3 = slurp(p, &len);
        f0 = host_audio_frames;
        size_t pos = 0; int stalls = 0;
        while (pos < len && stalls < 1000000)
        {
            // SE Play 0x1D40: only push while INP(&H21) < &H75
            if (inp(0x21) < 0x75)
            {
                size_t chunk = len - pos < 512 ? len - pos : 512;
                for (size_t i = 0; i < chunk; i++) outp(0x22, mp3[pos + i]);
                pos += chunk;
            }
            else { core1_tick(); stalls++; }
        }
        for (int i = 0; i < 400; i++) core1_tick();
        printf("    streamed %zu bytes -> %u frames\n", len, host_audio_frames - f0);
        CHECK(host_audio_frames - f0 > 40000, "stream decoded");
        host_advance_ms(5000); core1_tick(); core1_tick();
        CHECK(status() & SEONE_ST_END_OF_MUSIC, "stream ends after data stops");
        free(mp3);
    }

    printf("[I2S queue full: commands still answered]\n");
    atsend("AT+MSDPATH=0:/");
    atsend("AT+MSDPLAY=ROOT.MP3");
    host_audio_queue_full = true;
    CHECK(atsend("AT+DSPVOLR=40") == 1 && !strcmp(RESP, "OK"), "busy audio does not block AT");
    host_audio_queue_full = false;
    atsend("AT+MSDSTOP");

    printf("[REG mode]\n");
    atsend("AT+SEMODE=REG");
    outp(0x26, 0x60);
    { int w = 0; while (inp(0x26) == SEONE_REG_BUSY && w++ < 100000) {} }
    char fw[16]; int n = 0, d;
    while ((d = inp(0x27)) != 0 && n < 15) fw[n++] = (char)d;
    fw[n] = 0;
    CHECK(!strcmp(fw, "20250306"), "REG 0x60 firmware -> %s", fw);
    outp(0x26, 0); outp(0x27, 45); outp(0x27, 55); outp(0x26, 50);
    { int w = 0; while (inp(0x26) == SEONE_REG_BUSY && w++ < 100000) {} }
    CHECK(inp(0x26) == SEONE_REG_IDLE, "REG 50 ok");
    outp(0x26, 51);
    { int w = 0; while (inp(0x26) == SEONE_REG_BUSY && w++ < 100000) {} }
    int a = inp(0x27), b = inp(0x27);
    CHECK(a == 45 && b == 55, "REG 51 volume -> %d %d", a, b);
    outp(0x26, 0xEE);
    { int w = 0; while (inp(0x26) == SEONE_REG_BUSY && w++ < 100000) {} }
    CHECK(inp(0x26) == SEONE_REG_ERROR, "unknown REG function -> error");
    atsend("AT+SEMODE=MP3B");

    printf("[AT+LIST / help]\n");
    atsend("AT+LIST");
    CHECK(!strcmp(RESP, "AT+LIST="), "LIST first line -> %s", RESP);
    atsend("AT+MSDPLAY/H");
    CHECK(!strncmp(RESP, "AT+MSDPLAY/H=", 13), "help -> %s", RESP);

    host_wav_close();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
