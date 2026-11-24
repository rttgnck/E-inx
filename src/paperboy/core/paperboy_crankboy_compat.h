#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// X3: portable flash-resident core; no S3 IRAM section requirement.

#define FORCE_INLINE __attribute__((always_inline)) inline

#define __section__(x)
#define CB_IRAM_CODE
#define __shell CB_IRAM_CODE
#define CB_FAST_CODE CB_IRAM_CODE

#define likely(x) (__builtin_expect(!!(x), 1))
#define unlikely(x) (__builtin_expect(!!(x), 0))

#define clalign __attribute__((aligned(32)))

#define CB_ASSERT(x) ((void)0)

#ifndef MAX
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#endif

#ifndef MIN
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#endif

enum cgb_support_e
{
    GB_SUPPORT_DMG = 1,
    GB_SUPPORT_CGB = 2,
    GB_SUPPORT_DMG_AND_CGB = 3,
};

extern int preferences_cgb_speed;
extern int preferences_ppu_timing;
extern int audio_enabled;

static FORCE_INLINE uint8_t reverse_bits_u8(uint8_t b)
{
    b = (uint8_t)(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
    b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
    b = (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
    return b;
}

static FORCE_INLINE uint32_t reverse_bits_in_each_byte_conditional_u16(uint16_t b, bool condition)
{
    if (!condition) {
        return b;
    }

    return (uint32_t)(reverse_bits_u8((uint8_t)(b & 0xFFu)) |
                      ((uint16_t)reverse_bits_u8((uint8_t)((b >> 8) & 0xFFu)) << 8));
}

static inline void *mallocz(size_t size)
{
    return calloc(1, size);
}

static inline char *aprintf(const char *fmt, ...)
{
    va_list args;
    va_list copy;
    int needed;
    char *buffer;

    va_start(args, fmt);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return NULL;
    }

    buffer = malloc((size_t)needed + 1u);
    if (buffer != NULL) {
        vsnprintf(buffer, (size_t)needed + 1u, fmt, args);
    }
    va_end(args);

    return buffer;
}

#if !ENABLE_SOUND
static inline uint8_t audio_read(void *audio, const uint16_t addr)
{
    (void)audio;
    (void)addr;
    return 0xFF;
}

static inline void audio_write(void *audio, const uint16_t addr, const uint8_t val)
{
    (void)audio;
    (void)addr;
    (void)val;
}
#endif

static inline void *cb_malloc(size_t size)
{
    return malloc(size);
}

static inline void *cb_calloc(size_t count, size_t size)
{
    return calloc(count, size);
}

static inline void *cb_realloc(void *ptr, size_t size)
{
    return realloc(ptr, size);
}

static inline void cb_free(void *ptr)
{
    free(ptr);
}