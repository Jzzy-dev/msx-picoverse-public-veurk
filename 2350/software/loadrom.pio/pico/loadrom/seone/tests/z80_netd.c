// End-to-end test of the network-daemon patched SE Play (SEPLAY_NETD.EXE).
//
// Runs SE Play's Z80 code (loaded and relocated like the SymbOS loader) with:
//   * a minimal SymbOS message kernel: rst #10 send, rst #18 / #08 receive,
//     rst #30 idle,
//   * a system manager answering MSC_SYS_PRGSRV (shared service search),
//   * a fake SymbOS Network Daemon ("Network Daem", process 9) that accepts
//     TCP connections and serves an MP3 file as an endless "web radio",
//   * the PicoVerse SE-ONE emulation on ports 20h-27h (MP3 decoding).
// It checks that SE Play never touches the TNC ports 40h-49h.
//
// usage: z80_netd SEPLAYN.EXE test.img stream.mp3   (test.img is written by host_seplay)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "z80.h"
#include "ff.h"
#include "seone.h"
#include "seone_internal.h"

int  host_disk_open(const char *path, uint32_t create_sectors);
void host_advance_ms(uint32_t ms);
extern uint32_t host_audio_frames;

static uint8_t mem[65536];
static z80 cpu;
static uint16_t g_shift;          // bytes the patch added to the code segment
#define D(x) ((uint16_t)((x) >= 0x73E2 ? (x) + g_shift : (x)))
static unsigned long tnc_io, io_count;
static const uint8_t APP_PID = 7, NET_PID = 9, SYS_PID = 3;
static int daemon_running = 1;

static uint16_t w16(uint16_t a) { return mem[a] | (mem[a + 1] << 8); }
static void s16(uint16_t a, uint16_t v) { mem[a] = v & 0xFF; mem[a + 1] = v >> 8; }
static uint8_t rd(void *u, uint16_t a) { (void)u; return mem[a]; }
static void wr(void *u, uint16_t a, uint8_t v) { (void)u; mem[a] = v; }
static void tick(void) { seone_core1_poll(); host_advance_ms(1); }

static uint8_t pin(z80 *z, uint8_t port)
{
    (void)z;
    if (++io_count % 5 == 0) tick();
    if (port >= 0x40 && port <= 0x49) tnc_io++;
    uint8_t v = 0xFF;
    if (port >= 0x20 && port <= 0x27 && !seone_io_read(port, &v)) v = 0xFF;
    return v;
}
static void pout(z80 *z, uint8_t port, uint8_t v)
{
    (void)z;
    if (port >= 0x40 && port <= 0x49) tnc_io++;
    if (port >= 0x20 && port <= 0x27) seone_io_write(port, v);
}

// ------------------------------------------------------------ message kernel
typedef struct { uint8_t from, to, d[14]; } msg_t;
static msg_t q[64];
static int qn;
static void post(uint8_t from, uint8_t to, const uint8_t *d)
{
    if (qn < 64) { q[qn].from = from; q[qn].to = to; memcpy(q[qn].d, d, 14); qn++; }
}

// fake daemon state
static uint8_t *stream; static size_t stream_len, stream_pos;
static int sock_open, events_sent;
static char http_req[1024]; static size_t http_len;
static unsigned long net_calls[256];

static void reply(uint8_t to, uint8_t fn, uint8_t a, int cf, int zf, uint16_t bc, uint16_t de, uint16_t hl, uint16_t ix, uint16_t iy)
{
    uint8_t m[14] = {0};
    m[0] = fn;
    m[2] = (cf ? 1 : 0) | (zf ? 0x40 : 0);
    m[3] = a;
    m[4] = bc; m[5] = bc >> 8; m[6] = de; m[7] = de >> 8;
    m[8] = hl; m[9] = hl >> 8; m[10] = ix; m[11] = ix >> 8; m[12] = iy; m[13] = iy >> 8;
    post(NET_PID, to, m);
}

static void daemon_handle(uint8_t from, const uint8_t *m)
{
    uint8_t fn = m[0], a = m[3];
    uint16_t bc = m[4] | (m[5] << 8), de = m[6] | (m[7] << 8), hl = m[8] | (m[9] << 8);
    uint16_t ix = m[10] | (m[11] << 8), iy = m[12] | (m[13] << 8);
    net_calls[fn]++;
    if (++events_sent % 3 == 0)
    {   // unsolicited TCP event in between, must be skipped by SE Play
        uint8_t ev[14] = { 159, 0 };
        post(NET_PID, from, ev);
    }
    size_t avail = stream_len - stream_pos;
    switch (fn)
    {
        case 16:  // TCPOPN
            printf("    daemon: TCPOPN mode %u to %u.%u.%u.%u:%u local %04X\n", a, ix & 0xFF, ix >> 8, iy & 0xFF, iy >> 8, de, hl);
            sock_open = 1; stream_pos = 0;
            reply(from, fn + 128, 0, 0, 0, 0, 0, 0, 0, 0);
            break;
        case 18:  // TCPSTA
            if (!sock_open) { reply(from, fn + 128, 9, 1, 0, 0, 0, 0, 0, 0); break; }
            reply(from, fn + 128, a, 0, 0, avail > 2920 ? 2920 : (uint16_t)avail, 8000, (avail ? 128 : 0) | 2, 0, 0);
            break;
        case 19:  // TCPRCV
        {
            size_t n = bc;
            if (n > avail) n = avail;
            if (n > 2920) n = 2920;
            memcpy(&mem[hl], stream + stream_pos, n);
            stream_pos += n;
            if (stream_pos >= stream_len) stream_pos = 0;          // endless radio
            reply(from, fn + 128, a, 0, 0, (uint16_t)n, 0, 0, 0, 0);
            break;
        }
        case 20:  // TCPSND
            if (http_len + bc < sizeof http_req) { memcpy(http_req + http_len, &mem[hl], bc); http_len += bc; }
            reply(from, fn + 128, a, 0, 1, bc, 0, 0, 0, 0);
            break;
        case 17:  // TCPCLO
            sock_open = 0;
            reply(from, fn + 128, a, 0, 0, 0, 0, 0, 0, 0);
            break;
        default:
            reply(from, fn + 128, 3, 1, 0, 0, 0, 0, 0, 0);
    }
}

static void system_handle(uint8_t from, const uint8_t *m)
{
    if (m[0] != 30) return;                        // MSC_SYS_PRGSRV
    uint16_t name = m[1] | (m[2] << 8);
    uint8_t r[14] = { 158, 5 };
    if (daemon_running && memcmp(&mem[name], "Network Daem", 12) == 0)
    {
        r[1] = 0; r[8] = 5; r[9] = NET_PID;
    }
    post(SYS_PID, from, r);
}

static void ret_from_rst(void) { cpu.pc = w16(cpu.sp); cpu.sp += 2; }

// returns true if PC was a kernel entry that has been emulated
static bool kernel(void)
{
    uint8_t ixl = cpu.ix & 0xFF, ixh = cpu.ix >> 8;
    switch (cpu.pc)
    {
        case 0x10:      // send: IXL=sender IXH=receiver IY=message
        {
            uint8_t m[14];
            memcpy(m, &mem[cpu.iy], 14);
            if (ixh == NET_PID) daemon_handle(ixl, m);
            else if (ixh == SYS_PID) system_handle(ixl, m);
            else post(ixl, ixh, m);
            ret_from_rst();
            return true;
        }
        case 0x08: case 0x18:   // receive: IXL=receiver IXH=sender filter (-1 any)
        {
            for (int i = 0; i < qn; i++)
                if (q[i].to == ixl && (ixh == 0xFF || q[i].from == ixh))
                {
                    memcpy(&mem[cpu.iy], q[i].d, 14);
                    cpu.ix = (uint16_t)(q[i].from << 8) | 1;
                    memmove(&q[i], &q[i + 1], (qn - i - 1) * sizeof q[0]);
                    qn--;
                    ret_from_rst();
                    return true;
                }
            cpu.ix = (cpu.ix & 0xFF00);      // IXL=0: nothing
            ret_from_rst();
            return true;
        }
        case 0x30:      // idle
            tick();
            ret_from_rst();
            return true;
        case 0x20: case 0x28: case 0x38: case 0x00:
            ret_from_rst();
            return true;
    }
    return false;
}

#define SENTINEL 0xFFF0
static bool call(uint16_t addr, unsigned long max_steps)
{
    cpu.sp -= 2;
    s16(cpu.sp, SENTINEL);
    cpu.pc = addr;
    for (unsigned long i = 0; i < max_steps; i++)
    {
        if (cpu.pc == SENTINEL) return true;
        if (cpu.pc < 0x40 && kernel()) continue;
        z80_step(&cpu);
    }
    printf("    runaway at PC=%04X\n", cpu.pc);
    return false;
}

static const char *str(uint16_t var)
{
    static char buf[8][300]; static int k;
    char *o = buf[k++ & 7];
    uint16_t p = w16(D(var));
    int n = 0;
    while (mem[p + n] && n < 299) { o[n] = (char)mem[p + n]; n++; }
    o[n] = 0;
    return o;
}

static uint16_t const_area = 0xF100;
static void assign(uint16_t var, const char *s)
{
    size_t l = strlen(s);
    mem[const_area] = (uint8_t)(l + 1); mem[const_area + 1] = 0;
    memcpy(&mem[const_area + 2], s, l + 1);
    uint16_t p = const_area + 2;
    const_area += (uint16_t)(l + 4);
    cpu.h = p >> 8; cpu.l = p & 0xFF;
    uint16_t dst = w16(D(var));
    cpu.d = dst >> 8; cpu.e = dst & 0xFF;
    call(0x5ED7, 100000);
}

static int fails;
#define EXPECT(c, ...) do { if (!(c)) { fails++; printf("  FAIL: "); } else printf("  ok: "); printf(__VA_ARGS__); printf("\n"); } while (0)

static void load(const char *exe)
{
    memset(mem, 0, sizeof mem);
    FILE *f = fopen(exe, "rb");
    size_t n = fread(&mem[0x100], 1, 65536 - 0x100, f);
    fclose(f);
    for (int i = 0; i < 0x40; i++) mem[i] = 0xC9;
    uint16_t code = w16(0x100), data = w16(0x102), trans = w16(0x104), extra = w16(0x13A);
    g_shift = code - 0x72E2;
    uint16_t tr0 = 0x100 + code + data;
    uint32_t reltab = 0x100u + code + data + trans;
    static uint16_t rel[16384];
    unsigned nrel = 0;
    for (uint32_t r = reltab; r + 1 < 0x100u + n && nrel < 16384; r += 2) rel[nrel++] = w16((uint16_t)r);
    memmove(&mem[tr0 + extra], &mem[tr0], trans);
    memset(&mem[tr0], 0, extra);
    memset(&mem[tr0 + extra + trans], 0, 0xF000u - (tr0 + extra + trans));
    for (unsigned i = 0; i < nrel; i++)
    {
        uint16_t at = rel[i];
        if (at >= tr0) at += extra;
        uint16_t v = w16(at);
        if (v >= tr0) s16(at, v + extra);
    }
    s16(0x102, data + extra);
    mem[0x10E] = 1;                              // ram bank
    mem[D(0x7E6E)] = APP_PID;                    // own process ID
    for (int i = 0; i < 0x40; i++) mem[i] = 0xC9;
    // GUI output needs the SymbOS desktop
    mem[0x1234] = 0xC9; mem[0x113A] = 0xC9; mem[0x11B3] = 0xC9;
    z80_init(&cpu);
    cpu.read_byte = rd; cpu.write_byte = wr; cpu.port_in = pin; cpu.port_out = pout;
    cpu.sp = 0x89BB + g_shift + extra;
    printf("loaded %s: code +%u bytes, %u relocations\n", exe, g_shift, nrel);
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[3], "rb");
    fseek(f, 0, SEEK_END); stream_len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    stream = malloc(stream_len); fread(stream, 1, stream_len, f); fclose(f);

    host_disk_open(argv[2], 0);
    static uint8_t arena[128 * 1024];
    seone_init(arena, sizeof arena);
    seone_core1_init();

    load(argv[1]);
    printf("[start-up: platform init + network card probe]\n");
    EXPECT(call(0x4C1E, 2000000), "platform init");
    s16(D(0x7599), 0);
    EXPECT(call(0x4FEC, 2000000), "probe returns, HWID$='%s'", str(0x75BB));
    EXPECT(net_calls[16] == 0 && qn == 0, "probe only asked the system manager");
    {   // AT+SEMODE=MP3A as at 0C21..0C2E
        cpu.pc = 0x0C21;
        for (long i = 0; i < 3000000 && cpu.pc != 0x0C2E; i++) { if (cpu.pc < 0x40 && kernel()) continue; z80_step(&cpu); }
        EXPECT(!strcmp(str(0x77A6), "OK"), "AT+SEMODE=MP3A -> '%s'", str(0x77A6));
    }

    printf("[MP3 page still works with the relinked data segment]\n");
    EXPECT(call(0x1DED, 20000000), "MP3 page init, PATH$='%s'", str(0x77AA));
    printf("    list:");
    for (int i = 0; i < 6; i++) printf(" [%s]", str((uint16_t)(0x77B0 + 2 * i)));
    printf("\n");
    EXPECT(!strcmp(str(0x77B0), "/MUSIC"), "first entry /MUSIC");

    printf("[web radio: station from WEBRADIO.WRC, connect through the daemon]\n");
    assign(0x7776, "089.147.108.034\t|8000\t|/radio.mp3\t\t\t\t\t\t|PIGPEN\t\t|www.pigpenradio.org\t|Reggae\t\t|dub\t\t|UK|");
    s16(D(0x779E), 1);
    EXPECT(call(0x1ACD, 5000000), "station parsed: %u.%u.%u.%u:%u stream '%s' name '%s'",
           w16(D(0x7740)), w16(D(0x7742)), w16(D(0x7744)), w16(D(0x7746)), w16(D(0x774A)) * 256 + w16(D(0x774C)), str(0x774E), str(0x7750));
    assign(0x75EF, "WEBRADIO");
    EXPECT(call(0x1724, 5000000), "web radio page: AT+SEMODE=MP3B");
    EXPECT(call(0x1885, 20000000), "connect routine returned, display '%s'", str(0x76BB));
    EXPECT(w16(D(0x77A2)) == 1, "stream mode 1 (MSX pushes data), got %u", w16(D(0x77A2)));
    http_req[http_len] = 0;
    printf("    HTTP request sent:\n----\n%s----\n", http_req);
    EXPECT(!strncmp(http_req, "GET /radio.mp3 HTTP/1.1\r\nHost: 89.147.108.34:8000\r\n", 51), "request line + Host header");
    EXPECT(strstr(http_req, "\r\n\r\n") != NULL, "request terminated by an empty line");

    printf("[stream tick 1D40 pumps daemon data into the SE-ONE]\n");
    uint32_t f0 = host_audio_frames;
    size_t s0 = stream_pos;
    for (int t = 0; t < 1500; t++)
    {
        call(0x1D40, 5000000);
        for (int k = 0; k < 3; k++) tick();
    }
    printf("    %u bytes received, %u frames decoded, daemon calls: TCPSTA %lu TCPRCV %lu\n",
           (unsigned)(stream_pos - s0), host_audio_frames - f0, net_calls[18], net_calls[19]);
    EXPECT(host_audio_frames - f0 > 100000, "web radio audio is playing");
    EXPECT(!(seone.status & SEONE_ST_END_OF_MUSIC), "SE-ONE reports a stream playing");

    printf("[stop web radio]\n");
    EXPECT(call(0x1927, 5000000) && !sock_open && w16(D(0x77A2)) == 0, "socket closed, stream mode 0");

    printf("[phone page]\n");
    assign(0x75EF, "WEBRADIO");
    EXPECT(call(0x2A45, 2000000) && !strcmp(str(0x76BB), "PHONE: NO TNC") && strcmp(str(0x75EF), "VOIP"),
           "phone shows '%s', mode stays '%s'", str(0x76BB), str(0x75EF));

    printf("[no daemon running]\n");
    daemon_running = 0;
    unsigned long opens = net_calls[16];
    EXPECT(call(0x4FEC, 2000000), "probe returns without daemon");
    EXPECT(call(0x1885, 20000000), "connect returns, display '%s'", str(0x76BB));
    EXPECT(net_calls[16] == opens && w16(D(0x77A2)) == 0, "no TCP open attempted, not streaming");

    printf("[daemon started later]\n");
    daemon_running = 1;
    EXPECT(call(0x1885, 20000000) && w16(D(0x77A2)) == 1 && sock_open, "connects once the daemon is there");
    call(0x1927, 5000000);

    EXPECT(tnc_io == 0, "no access to TNC ports 40h-49h (%lu)", tnc_io);
    printf("\n%d failures\n", fails);
    return fails != 0;
}
