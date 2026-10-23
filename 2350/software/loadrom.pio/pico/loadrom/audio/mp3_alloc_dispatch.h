// MSX PICOVERSE PROJECT
// mp3_alloc_dispatch.h - one allocator hook for the Helix MP3 decoder,
// shared by the engines that use it (SE-ONE emulation, PicoVerse Audio Engine).
//
// picomp3lib's buffers.c is compiled once with -DMPDEC_ALLOCATOR=pv_mp3_alloc;
// the engine that starts sets pv_mp3_alloc_fn to its own arena allocator.

#ifndef MP3_ALLOC_DISPATCH_H
#define MP3_ALLOC_DISPATCH_H

#include <stddef.h>

extern void *(*pv_mp3_alloc_fn)(size_t size);
void *pv_mp3_alloc(size_t size);

#endif
