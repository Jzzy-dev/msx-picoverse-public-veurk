// MSX PICOVERSE PROJECT
// seone_mp3_alloc.h - allocator hook for the Helix MP3 decoder (picomp3lib).
//
// Force-included into picomp3lib's buffers.c together with
// -DMPDEC_ALLOCATOR=seone_mp3_alloc so the decoder state lives in the SE-ONE
// arena instead of .bss/heap. The pool is reset before every MP3InitDecoder()
// call and hands out zeroed memory, as the decoder expects.

#ifndef SEONE_MP3_ALLOC_H
#define SEONE_MP3_ALLOC_H

#include <stddef.h>

void *seone_mp3_alloc(size_t size);

#endif
