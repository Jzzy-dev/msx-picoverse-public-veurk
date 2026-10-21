// Runs SE Play's own Z80 routines (SEPLAY.EXE loaded at its link address
// 0x0100, relocated like the SymbOS loader does) against the SE-ONE
// emulation. SymbOS kernel calls (RST vectors) and GUI text output are
// stubbed with RET.
//
// Needs SEPLAY.EXE (SE Play 0.1, not included) and the single-file Z80 core
// from https://github.com/superzazu/z80; see the Makefile target "z80".
// usage: z80_seplay SEPLAY.EXE test.img   (test.img is written by host_seplay)
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
static unsigned long io_count, rst_count[8];
static int trace_io;

static uint16_t w16(uint16_t a) { return mem[a] | (mem[a + 1] << 8); }
static uint8_t rd(void *u, uint16_t a) { (void)u; return mem[a]; }
static void wr(void *u, uint16_t a, uint8_t v) { (void)u; mem[a] = v; }

static void tick(void) { seone_core1_poll(); host_advance_ms(1); }

static uint8_t pin(z80 *z, uint8_t port)
{
    (void)z;
    if (++io_count % 5 == 0) tick();
    uint8_t v = 0xFF;
    if (seone_port_claimed(port) && !seone_io_read(port, &v)) v = 0xFF;
    if (trace_io && port != 0x20) printf("      IN  %02X -> %02X\n", port, v);
    return v;
}

static void pout(z80 *z, uint8_t port, uint8_t v)
{
    (void)z;
    if (trace_io && port != 0x40) printf("      OUT %02X <- %02X '%c'\n", port, v, v >= 32 && v < 127 ? v : '.');
    if (seone_port_claimed(port)) seone_io_write(port, v);
}

#define SENTINEL 0xFFF0
// Call a routine and run until it returns. Returns false on runaway.
static const char *str(uint16_t var);
static uint16_t hist[4096]; static unsigned hpos;
static uint16_t calls_hist[256]; static unsigned cpos;
static void dump_hist(void)
{
    printf("    last calls:");
    for (unsigned i = 0; i < 24; i++) printf(" %04X", calls_hist[(cpos - 24 + i) & 255]);
    printf("\n    last PCs:");
    uint16_t prev = 0;
    for (unsigned i = 0; i < 1500; i++) { uint16_t pc = hist[(hpos - 1500 + i) & 4095]; if (pc < prev || pc > prev + 4) printf(" | %04X", pc); prev = pc; }
    printf("\n");
}
static void step(void)
{
    hist[hpos++ & 4095] = cpu.pc;
    uint8_t op = mem[cpu.pc];
    if (op == 0xCD || (op & 0xC7) == 0xC4) calls_hist[cpos++ & 255] = cpu.pc;
    z80_step(&cpu);
}
static bool call(uint16_t addr, unsigned long max_steps)
{
    cpu.sp -= 2;
    mem[cpu.sp] = SENTINEL & 0xFF; mem[cpu.sp + 1] = SENTINEL >> 8;
    cpu.pc = addr;
    for (unsigned long i = 0; i < max_steps; i++)
    {
        if (cpu.pc == SENTINEL) return true;
        if (cpu.pc < 0x40 && (cpu.pc & 7) == 0) rst_count[cpu.pc >> 3]++;
        step();
        if (cpu.pc == 0x5F4C) printf("    OUT OF STRING SPACE: heap ptr %04X limit %04X\n", w16(0x541), 0x73E2 + w16(0x102));
        if (cpu.pc == 0x675A) printf("    GC: heap ptr %04X\n", w16(0x541));
        if (cpu.pc >= 0x7400 && cpu.pc < 0xFF00) { printf("    jumped into data at %04X\n", cpu.pc); dump_hist(); return false; }
    }
    printf("    runaway at PC=%04X\n", cpu.pc);
    dump_hist();
    return false;
}

// Run from 'start' until PC reaches 'stop' (inside a routine).
static bool run_between(uint16_t start, uint16_t stop, unsigned long max_steps)
{
    cpu.pc = start;
    for (unsigned long i = 0; i < max_steps; i++)
    {
        if (cpu.pc == stop) return true;
        if (cpu.pc < 0x40 && (cpu.pc & 7) == 0) rst_count[cpu.pc >> 3]++;
        z80_step(&cpu);
    }
    printf("    runaway at PC=%04X\n", cpu.pc);
    return false;
}


// String variable -> C string (data pointer stored in the variable)
static const char *str(uint16_t var)
{
    static char buf[8][300];
    static int k;
    char *o = buf[k++ & 7];
    uint16_t p = w16(var);
    int n = 0;
    while (mem[p + n] && n < 299) { o[n] = (char)mem[p + n]; n++; }
    o[n] = 0;
    return o;
}

// Put a constant string in free memory in SE Play's const format and
// assign it to a string variable through the program's own STRASSIGN.
static uint16_t const_area = 0xE000;
static void assign(uint16_t var, const char *s)
{
    size_t l = strlen(s);
    mem[const_area] = (uint8_t)(l + 1); mem[const_area + 1] = 0;
    memcpy(&mem[const_area + 2], s, l + 1);
    uint16_t p = const_area + 2;
    const_area += (uint16_t)(l + 4);
    cpu.h = p >> 8; cpu.l = p & 0xFF;
    uint16_t dst = w16(var);
    cpu.d = dst >> 8; cpu.e = dst & 0xFF;
    call(0x5ED7, 100000);
}

static void set16(uint16_t a, uint16_t v) { mem[a] = v & 0xFF; mem[a + 1] = v >> 8; }

static void print_list(void)
{
    printf("    SE Play list array (0x77B0):");
    for (int i = 0; i < 50; i++)
    {
        const char *s = str((uint16_t)(0x77B0 + 2 * i));
        printf(" [%s]", s);
        if (!strcmp(s, "_END_")) break;
    }
    printf("\n");
}

static int fails;
#define EXPECT(c, ...) do { if (!(c)) { fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } else { printf("  ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    size_t n = fread(&mem[0x100], 1, 65536 - 0x100, f);
    fclose(f);
    printf("loaded %zu bytes\n", n);
    for (int i = 0; i < 0x40; i++) mem[i] = 0xC9;    // RST vectors -> RET
    mem[0x38] = 0xFB; mem[0x39] = 0xC9;               // EI;RET
    // GUI text output (marquee/label refresh) needs the SymbOS desktop: stub it
    mem[0x1234] = 0xC9; mem[0x113A] = 0xC9; mem[0x11B3] = 0xC9;

    host_disk_open(argv[2], 0);                       // image made by host_seplay
    static uint8_t arena[128 * 1024];
    seone_init(arena, sizeof arena);
    seone_core1_init();

    z80_init(&cpu);
    cpu.read_byte = rd; cpu.write_byte = wr; cpu.port_in = pin; cpu.port_out = pout;
    // SymbOS loader emulation: the data segment is extended by the header's
    // "extra data memory" (offset 0x3A) - that is where the Quigs string heap
    // grows - and the transfer segment is placed after it. Shift the transfer
    // segment and patch every relocated pointer into it.
    {
        uint16_t code = w16(0x100), data = w16(0x102), trans = w16(0x104);
        uint16_t extra = w16(0x100 + 0x3A);
        uint16_t tr0 = 0x100 + code + data;
        uint32_t reltab = 0x100u + code + data + trans;
        static uint16_t rel[16384];
        unsigned nrel = 0;
        for (uint32_t r = reltab; r + 1 < 0x100u + n && nrel < 16384; r += 2)
            rel[nrel++] = w16((uint16_t)r);
        memmove(&mem[tr0 + extra], &mem[tr0], trans);
        memset(&mem[tr0], 0, extra);
        memset(&mem[tr0 + extra + trans], 0, 0x10000u - (tr0 + extra + trans) - 0x100u);
        unsigned patched = 0;
        for (unsigned i = 0; i < nrel; i++)
        {
            uint16_t at = rel[i];
            if (at >= tr0) at += extra;          // the word itself moved
            uint16_t v = w16(at);
            if (v >= tr0) { v += extra; mem[at] = v & 0xFF; mem[at + 1] = v >> 8; patched++; }
        }
        data += extra;
        mem[0x102] = data & 0xFF; mem[0x103] = data >> 8;
        printf("relocated %u pointers, string heap %04X..%04X, transfer at %04X\n", patched, w16(0x541), tr0 + extra, tr0 + extra);
    }
    cpu.sp = 0x89BB + w16(0x100 + 0x3A);   // relocated stack top

    printf("[platform init 0x4C1E (sets network-card port variables)]\n");
    bool ok0 = call(0x4C1E, 2000000);
    EXPECT(ok0, "platform '%s', TNC ports %04X/%04X/%04X", str(0x7571), w16(0x7535), w16(0x7537), w16(0x753D));

    printf("[TNC probe 0x4FEC (start-up)]\n");
    set16(0x7599, 0);
    bool ok = call(0x4FEC, 2000000);
    EXPECT(ok, "TNC probe returns (hardware id '%s')", str(0x75BB));

    printf("[ATSEND AT+SEMODE=MP3A as at 0x0C21]\n");
    ok = run_between(0x0C21, 0x0C2E, 3000000);
    EXPECT(ok && !strcmp(str(0x77A6), "OK"), "RESP$='%s'", str(0x77A6));

    printf("[MP3 USB page init 0x1DED..: MP3B, USBINIT, TNC probe, MSDPATH, list]\n");
    ok = call(0x1DED, 20000000);
    EXPECT(ok, "page init returned, PATH$='%s'", str(0x77AA));
    print_list();
    EXPECT(!strcmp(str(0x77B0), "/MUSIC"), "first entry /MUSIC");

    printf("[select /MUSIC and enter it via 0x2100]\n");
    set16(0x781C, 0);                // not playing
    assign(0x7818, "/MUSIC");
    ok = call(0x2100, 20000000);
    EXPECT(ok, "PATH$='%s', SEL$='%s'", str(0x77AA), str(0x7818));
    print_list();

    printf("[enter '/Sandra Collection']\n");
    assign(0x7818, "/Sandra Collection");
    ok = call(0x2100, 20000000);
    EXPECT(ok && !strcmp(str(0x77AA), "1:/MUSIC/Sandra Collection"), "PATH$='%s'", str(0x77AA));
    print_list();

    printf("[play 01SAND~1.MP3 via 0x21A7]\n");
    assign(0x7818, "01SAND~1.MP3");
    ok = call(0x21A7, 5000000);
    EXPECT(ok && w16(0x781C) == 1, "playing flag=%u RESP$='%s'", w16(0x781C), str(0x77A6));
    for (int i = 0; i < 50; i++) tick();
    EXPECT(host_audio_frames > 20000, "audio running (%u frames)", host_audio_frames);
    EXPECT(!(seone.status & SEONE_ST_END_OF_MUSIC), "status bit 3 clear while playing");

    printf("[status/VU poll as the timer tick does (0x14B2 VU, INP 0x23)]\n");
    set16(0x76CA, 5);
    ok = call(0x14B2, 2000000);
    EXPECT(ok, "VU routine ran");

    printf("[stop via 0x21F3]\n");
    ok = call(0x21F3, 5000000);
    EXPECT(ok && w16(0x781C) == 0 && (seone.status & SEONE_ST_END_OF_MUSIC), "stopped, flag=%u", w16(0x781C));

    printf("[M3U: load playlist + first track via 0x24B3 path]\n");
    assign(0x75EF, "M3U");
    assign(0x7818, "Sandra.M3U");
    set16(0x7822, 3);    // play mode 3 = repeat all (1 once, 2 random)
    set16(0x781E, 0);
    ok = run_between(0x24B3, 0x2518, 50000000);
    EXPECT(ok, "M3U loaded, row=%u, title='%s', file='%s', RESP$='%s'", w16(0x781E), str(0x7828), str(0x7820), str(0x77A6));
    for (int i = 0; i < 30; i++) tick();
    EXPECT(!(seone.status & SEONE_ST_END_OF_MUSIC) && w16(0x7826) == 1, "M3U track playing (flag %u)", w16(0x7826));

    printf("[next M3U track via 0x2568]\n");
    ok = call(0x2568, 50000000);
    EXPECT(ok, "row=%u file='%s' RESP$='%s'", w16(0x781E), str(0x7820), str(0x77A6));
    for (int i = 0; i < 30; i++) tick();
    EXPECT(!(seone.status & SEONE_ST_END_OF_MUSIC), "second track playing");
    ok = call(0x2568, 50000000);
    EXPECT(ok, "row=%u file='%s' RESP$='%s'", w16(0x781E), str(0x7820), str(0x77A6));
    for (int i = 0; i < 30; i++) tick();
    EXPECT(!(seone.status & SEONE_ST_END_OF_MUSIC), "third track (subfolder) playing");

    printf("\nRST usage:");
    for (int i = 0; i < 8; i++) printf(" %02X:%lu", i * 8, rst_count[i]);
    printf("\n%d failures\n", fails);
    return fails != 0;
}
