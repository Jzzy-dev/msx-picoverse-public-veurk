// MSX PICOVERSE PROJECT
// (c) 2026 Cristiano Goncalves
// The Retro Hacker
//
// seone.h - TMTLogic SE-ONE cartridge emulation (public interface)
//
// Emulates the I/O interface of the TMTLogic SE-ONE MP3 / FM / USB cartridge
// (ports 0x20-0x27) on top of the PicoVerse 2350 microSD card and I2S DAC, so
// software written for the SE-ONE (most notably "SE Play" for SymbOS) can
// browse and play MP3/WAV files and M3U playlists from the microSD card.
//
// Threading model
//   Core 0  (MSX bus loop, time critical, runs from RAM):
//       seone_io_write() / seone_io_read() are called for every I/O cycle that
//       targets a port claimed by seone_port_claimed(). They only touch shared
//       memory and never block.
//   Core 1  (storage + audio):
//       seone_core1_init() once after the SD card is up, then
//       seone_core1_poll() continuously. Executes AT and REG commands, file
//       system access (FatFS), MP3/WAV decoding and I2S output.
//
// This work is licensed under a "Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International
// License". https://creativecommons.org/licenses/by-nc-sa/4.0/

#ifndef SEONE_H
#define SEONE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Minimum scratch memory required by seone_init() (buffers, MP3 decoder and
// the core 1 stack).
#define SEONE_ARENA_MIN_SIZE   (112u * 1024u)

// Core 1 stack size. The command side (paths, long file names, FatFS) needs
// ~5 KB worst case, far more than the SDK's default 2 KB core 1 stack.
#define SEONE_CORE1_STACK_SIZE (16u * 1024u)

// Initialise shared state. 'arena' must stay valid forever and must not be
// touched by anything else. Call on core 0 before launching core 1.
void seone_init(uint8_t *arena, size_t arena_size);

// Stack for multicore_launch_core1_with_stack() (allocated from the arena).
uint32_t *seone_core1_stack(void);

// True for every I/O port this emulation needs to see (reads or writes).
// 0x20-0x27: SE-ONE. 0x40-0x49: minimal TMTLogic network-card (TNC) stub.
static inline bool seone_port_claimed(uint8_t port)
{
    return (uint8_t)(port - 0x20u) < 8u || (uint8_t)(port - 0x40u) < 10u;
}

// Core 0: handle an I/O write to a claimed port.
void seone_io_write(uint8_t port, uint8_t data);

// Core 0: handle an I/O read from a claimed port. Returns true when the
// cartridge drives the data bus (value in *data), false to leave it floating.
bool seone_io_read(uint8_t port, uint8_t *data);

// Core 1: mount the microSD card and start the audio output.
void seone_core1_init(void);

// Core 1: service pending commands and keep the audio pipeline fed.
void seone_core1_poll(void);

#ifdef __cplusplus
}
#endif

#endif // SEONE_H
