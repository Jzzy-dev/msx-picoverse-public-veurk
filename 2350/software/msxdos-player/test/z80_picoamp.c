// Runs PICOAMP.COM in a Z80 emulator against the real PicoVerse Audio Engine
// (host build), with just enough MSX around it: BDOS (console, keys), BIOS
// RDSLT/WRSLT/ENASLT/CALSLT, page 1 switched to the engine (slot 1-3), JIFFY and
// the VDP's VRAM writes (kept, and saved for a picture of the screen), the
// frame interrupt at 60 Hz. Engine time follows the Z80 clock (3.58 MHz).
//
// usage: z80_picoamp PICOAMP.COM image "args" "keys" seconds vram.bin
//   keys: one character per half second; '{' up '}' down '<' left '>' right,
//   '^' Enter, '_' Backspace, '.' nothing, others as they are
//
// It prints how long each main loop pass took (frames) and how long the
// program took to answer the keys.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "z80.h"

void pva_init(uint8_t *arena, size_t arena_size);
uint8_t pva_mem_read(uint16_t addr);
void pva_mem_write(uint16_t addr, uint8_t data);
void pva_core1_init(void);
bool pva_core1_poll(void);
int  host_disk_open(const char *path, uint32_t create_sectors);
void host_advance_ms(uint32_t ms);
extern bool host_audio_unlimited;
extern uint32_t host_audio_frames, host_audio_underruns;

#define HZ      3580000UL
#define CYC_MS  3580
#define ENGINE  0x8D

static uint8_t mem[65536];
static uint8_t vram[0x4000];
static uint16_t vaddr;
static bool vlatch;
static uint8_t vfirst;
static uint8_t page1 = 0x00;
static z80 cpu;
static unsigned long next_ms_cyc, next_frame_cyc;
static const char *keys;
static unsigned long next_key_cyc;
static unsigned long frames, halts, last_halt_frame, worst_gap;
static unsigned long key_frame, answered = 1;
static unsigned long vwrites;

static void sync_engine(void)
{
    while (cpu.cyc >= next_ms_cyc) { pva_core1_poll(); host_advance_ms(1); next_ms_cyc += CYC_MS; }
}

static uint8_t rd(void *u, uint16_t a)
{
    (void)u;
    if (page1 == ENGINE && a >= 0x4000 && a < 0x8000) { sync_engine(); return pva_mem_read(a); }
    return mem[a];
}
static void wr(void *u, uint16_t a, uint8_t v)
{
    (void)u;
    if (page1 == ENGINE && a >= 0x4000 && a < 0x8000) { sync_engine(); pva_mem_write(a, v); return; }
    if (a >= 0x100) mem[a] = v;
}
static uint8_t pin(z80 *z, uint8_t p) { (void)z; if (p == 0x99) vlatch = false; return 0; }
static void pout(z80 *z, uint8_t p, uint8_t v)
{
    (void)z;
    if (p == 0x98) { vram[vaddr & 0x3FFF] = v; vaddr++; vwrites++; vlatch = false; }
    else if (p == 0x99)
    {
        if (!vlatch) { vfirst = v; vlatch = true; }
        else
        {
            vlatch = false;
            if (!(v & 0x80)) vaddr = (uint16_t)(vfirst | ((v & 0x3F) << 8));
        }
    }
}

static uint8_t font[2048];
static void load_font(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    int n = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f) && n < 256)
    {
        char *p = strstr(line, "db");
        if (!p || strchr(line, ';') < p) continue;
        p += 2;
        for (int i = 0; i < 8; i++)
        {
            while (*p && *p != '$') p++;
            if (!*p) break;
            font[n * 8 + i] = (uint8_t)strtol(p + 1, &p, 16);
        }
        n++;
    }
    fclose(f);
}

static void ret(void) { cpu.pc = mem[cpu.sp] | (mem[cpu.sp + 1] << 8); cpu.sp += 2; }

static uint8_t key_code(char k)
{
    switch (k)
    {
        case '{': return 30;
        case '}': return 31;
        case '<': return 29;
        case '>': return 28;
        case '^': return 13;
        case '_': return 8;
        case '.': return 0;
    }
    return (uint8_t)k;
}

static int run(unsigned long max_cyc)
{
    while (cpu.cyc < max_cyc)
    {
        sync_engine();
        if (cpu.cyc >= next_frame_cyc)
        {
            next_frame_cyc += HZ / 60;
            frames++;
            uint16_t j = mem[0xFC9E] | (mem[0xFC9F] << 8);
            j++;
            mem[0xFC9E] = (uint8_t)j; mem[0xFC9F] = (uint8_t)(j >> 8);
            if (cpu.iff1) cpu.halted = false;
        }
        if (cpu.halted) { cpu.cyc += 4; continue; }
        switch (cpu.pc)
        {
            case 0x0000: return 0;
            case 0x0005:
                switch (cpu.c)
                {
                    case 0x00: return 0;
                    case 0x02: putchar(cpu.e); break;
                    case 0x09: { uint16_t p = (uint16_t)(cpu.d << 8 | cpu.e); while (mem[p] != '$') putchar(mem[p++]); break; }
                    case 0x06:
                        cpu.a = 0;
                        if (keys && *keys && cpu.cyc >= next_key_cyc)
                        {
                            cpu.a = key_code(*keys++);
                            next_key_cyc = cpu.cyc + HZ / 2;
                            if (cpu.a) { key_frame = frames; answered = 0; }
                        }
                        cpu.l = cpu.a;
                        break;
                    default: printf("[BDOS %02X]\n", cpu.c);
                }
                ret();
                cpu.cyc += 300;
                continue;
            case 0x000C:                // RDSLT
            {
                uint16_t a = (uint16_t)(cpu.h << 8 | cpu.l);
                if (cpu.a == ENGINE) { sync_engine(); cpu.a = pva_mem_read(a); }
                else if (a == 0x0004) cpu.a = 0x00;          // CGTABL -> 3000h (fake ROM)
                else if (a == 0x0005) cpu.a = 0x30;
                else if (a >= 0x3000 && a < 0x3800) cpu.a = font[a - 0x3000];
                else cpu.a = 0xFF;
                cpu.iff1 = cpu.iff2 = false;
                ret();
                cpu.cyc += 150;
                continue;
            }
            case 0x0014:                // WRSLT
                if (cpu.a == ENGINE) { sync_engine(); pva_mem_write((uint16_t)(cpu.h << 8 | cpu.l), cpu.e); }
                cpu.iff1 = cpu.iff2 = false;
                ret();
                cpu.cyc += 150;
                continue;
            case 0x001C:                // CALSLT: CHGMOD etc. do nothing here
                ret();
                cpu.cyc += 2000;
                continue;
            case 0x0024:                // ENASLT A = slot, H = page
                if ((cpu.h & 0xC0) == 0x40) page1 = cpu.a;
                cpu.iff1 = cpu.iff2 = false;
                ret();
                cpu.cyc += 400;
                continue;
        }
        if (mem[cpu.pc] == 0x76)    // HALT: a main loop pass ended
        {
            unsigned long gap = frames - last_halt_frame;
            if (halts > 10 && gap > worst_gap) worst_gap = gap;
            last_halt_frame = frames;
            halts++;
            if (!answered)
            {
                answered = 1;
                printf("  key answered after %lu frame(s)\n", frames - key_frame);
            }
        }
        z80_step(&cpu);
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 7) { printf("usage: z80_picoamp PICOAMP.COM image args keys seconds vram.bin [font.asm]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    size_t n = fread(&mem[0x100], 1, 0xD000, f);
    fclose(f);
    if (argc > 7) load_font(argv[7]);
    if (host_disk_open(argv[2], 0)) { printf("no image\n"); return 2; }
    static uint8_t arena[116 * 1024];
    host_audio_unlimited = false;
    pva_init(arena, sizeof arena);
    pva_core1_init();
    const char *a = argv[3];
    keys = argv[4];
    size_t al = strlen(a);
    mem[0x80] = (uint8_t)(al ? al + 1 : 0);
    mem[0x81] = ' ';
    memcpy(&mem[0x82], a, al);
    memset(&mem[0xFCC1], 0, 4);
    mem[0xFCC1 + 1] = 0x80;
    mem[0xFCC1 + 3] = 0x80;
    mem[0xF342] = 0x00;             // RAMAD1 (not the engine)
    mem[0x0006] = 0x00; mem[0x0007] = 0xF0;     // top of the TPA
    z80_init(&cpu);
    cpu.read_byte = rd; cpu.write_byte = wr; cpu.port_in = pin; cpu.port_out = pout;
    cpu.pc = 0x100;
    cpu.sp = 0xF000;
    mem[0xF000] = 0; mem[0xF001] = 0;
    next_ms_cyc = CYC_MS;
    next_frame_cyc = HZ / 60;
    next_key_cyc = HZ * 3;
    printf("loaded %zu bytes\n", n);
    int r = run(HZ * (unsigned long)atoi(argv[5]));
    printf("[%s after %.1f s: %lu frames, %lu loop passes (%.1f frames each), worst %lu frames, %lu VRAM bytes, %u audio underruns]\n",
           r ? "time up" : "exit", cpu.cyc / (double)HZ, frames, halts, halts ? (double)frames / halts : 0.0, worst_gap, vwrites,
           host_audio_underruns);
    f = fopen(argv[6], "wb");
    fwrite(vram, 1, sizeof vram, f);
    fclose(f);
    return 0;
}
