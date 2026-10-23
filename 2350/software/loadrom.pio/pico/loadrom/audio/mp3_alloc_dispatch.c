// MSX PICOVERSE PROJECT
// mp3_alloc_dispatch.c - see mp3_alloc_dispatch.h

#include "mp3_alloc_dispatch.h"

void *(*pv_mp3_alloc_fn)(size_t size);

void *pv_mp3_alloc(size_t size)
{
    return pv_mp3_alloc_fn ? pv_mp3_alloc_fn(size) : NULL;
}
