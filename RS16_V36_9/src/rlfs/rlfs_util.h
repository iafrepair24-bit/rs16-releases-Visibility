/*
 * lfs utility functions
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef RLFS_UTIL_H
#define RLFS_UTIL_H

// RS16: salinan littlefs v2.11.2 (BSD-3, lihat LICENSE_littlefs.md) dengan awalan rlfs_ supaya tidak bentrok
// dengan LittleFS bawaan core ESP32. Dipakai untuk IC flash W25Q128 (U7) = riwayat balap.
#define RLFS_NO_DEBUG
#define RLFS_NO_WARN
#define RLFS_NO_ERROR   // pesan sendiri di firmware RS16 sudah cukup (IC flash kosong pertama kali = "Corrupted dir pair")
#define RLFS_NO_ASSERT   // flash rusak -> error biasa, bukan restart berulang

#define RLFS_STRINGIZE(x) RLFS_STRINGIZE2(x)
#define RLFS_STRINGIZE2(x) #x

// Users can override rlfs_util.h with their own configuration by defining
// RLFS_CONFIG as a header file to include (-DLFS_CONFIG=rlfs_config.h).
//
// If RLFS_CONFIG is used, none of the default utils will be emitted and must be
// provided by the config file. To start, I would suggest copying rlfs_util.h
// and modifying as needed.
#ifdef RLFS_CONFIG
#include RLFS_STRINGIZE(RLFS_CONFIG)
#else

// Alternatively, users can provide a header file which defines
// macros and other things consumed by littlefs.
//
// For example, provide my_defines.h, which contains
// something like:
//
// #include <stddef.h>
// extern void *my_malloc(size_t sz);
// #define RLFS_MALLOC(sz) my_malloc(sz)
//
// And build littlefs with the header by defining RLFS_DEFINES.
// (-DLFS_DEFINES=my_defines.h)

#ifdef RLFS_DEFINES
#include RLFS_STRINGIZE(RLFS_DEFINES)
#endif

// System includes
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

#ifndef RLFS_NO_MALLOC
#include <stdlib.h>
#endif
#ifndef RLFS_NO_ASSERT
#include <assert.h>
#endif
#if !defined(RLFS_NO_DEBUG) || \
        !defined(RLFS_NO_WARN) || \
        !defined(RLFS_NO_ERROR) || \
        defined(RLFS_YES_TRACE)
#include <stdio.h>
#endif

#ifdef __cplusplus
extern "C"
{
#endif


// Macros, may be replaced by system specific wrappers. Arguments to these
// macros must not have side-effects as the macros can be removed for a smaller
// code footprint

// Logging functions
#ifndef RLFS_TRACE
#ifdef RLFS_YES_TRACE
#define RLFS_TRACE_(fmt, ...) \
    printf("%s:%d:trace: " fmt "%s\n", __FILE__, __LINE__, __VA_ARGS__)
#define RLFS_TRACE(...) RLFS_TRACE_(__VA_ARGS__, "")
#else
#define RLFS_TRACE(...)
#endif
#endif

#ifndef RLFS_DEBUG
#ifndef RLFS_NO_DEBUG
#define RLFS_DEBUG_(fmt, ...) \
    printf("%s:%d:debug: " fmt "%s\n", __FILE__, __LINE__, __VA_ARGS__)
#define RLFS_DEBUG(...) RLFS_DEBUG_(__VA_ARGS__, "")
#else
#define RLFS_DEBUG(...)
#endif
#endif

#ifndef RLFS_WARN
#ifndef RLFS_NO_WARN
#define RLFS_WARN_(fmt, ...) \
    printf("%s:%d:warn: " fmt "%s\n", __FILE__, __LINE__, __VA_ARGS__)
#define RLFS_WARN(...) RLFS_WARN_(__VA_ARGS__, "")
#else
#define RLFS_WARN(...)
#endif
#endif

#ifndef RLFS_ERROR
#ifndef RLFS_NO_ERROR
#define RLFS_ERROR_(fmt, ...) \
    printf("%s:%d:error: " fmt "%s\n", __FILE__, __LINE__, __VA_ARGS__)
#define RLFS_ERROR(...) RLFS_ERROR_(__VA_ARGS__, "")
#else
#define RLFS_ERROR(...)
#endif
#endif

// Runtime assertions
#ifndef RLFS_ASSERT
#ifndef RLFS_NO_ASSERT
#define RLFS_ASSERT(test) assert(test)
#else
#define RLFS_ASSERT(test)
#endif
#endif


// Builtin functions, these may be replaced by more efficient
// toolchain-specific implementations. RLFS_NO_INTRINSICS falls back to a more
// expensive basic C implementation for debugging purposes

// Min/max functions for unsigned 32-bit numbers
static inline uint32_t rlfs_max(uint32_t a, uint32_t b) {
    return (a > b) ? a : b;
}

static inline uint32_t rlfs_min(uint32_t a, uint32_t b) {
    return (a < b) ? a : b;
}

// Align to nearest multiple of a size
static inline uint32_t rlfs_aligndown(uint32_t a, uint32_t alignment) {
    return a - (a % alignment);
}

static inline uint32_t rlfs_alignup(uint32_t a, uint32_t alignment) {
    return rlfs_aligndown(a + alignment-1, alignment);
}

// Find the smallest power of 2 greater than or equal to a
static inline uint32_t rlfs_npw2(uint32_t a) {
#if !defined(RLFS_NO_INTRINSICS) && (defined(__GNUC__) || defined(__CC_ARM))
    return 32 - __builtin_clz(a-1);
#else
    uint32_t r = 0;
    uint32_t s;
    a -= 1;
    s = (a > 0xffff) << 4; a >>= s; r |= s;
    s = (a > 0xff  ) << 3; a >>= s; r |= s;
    s = (a > 0xf   ) << 2; a >>= s; r |= s;
    s = (a > 0x3   ) << 1; a >>= s; r |= s;
    return (r | (a >> 1)) + 1;
#endif
}

// Count the number of trailing binary zeros in a
// rlfs_ctz(0) may be undefined
static inline uint32_t rlfs_ctz(uint32_t a) {
#if !defined(RLFS_NO_INTRINSICS) && defined(__GNUC__)
    return __builtin_ctz(a);
#else
    return rlfs_npw2((a & -a) + 1) - 1;
#endif
}

// Count the number of binary ones in a
static inline uint32_t rlfs_popc(uint32_t a) {
#if !defined(RLFS_NO_INTRINSICS) && (defined(__GNUC__) || defined(__CC_ARM))
    return __builtin_popcount(a);
#else
    a = a - ((a >> 1) & 0x55555555);
    a = (a & 0x33333333) + ((a >> 2) & 0x33333333);
    return (((a + (a >> 4)) & 0xf0f0f0f) * 0x1010101) >> 24;
#endif
}

// Find the sequence comparison of a and b, this is the distance
// between a and b ignoring overflow
static inline int rlfs_scmp(uint32_t a, uint32_t b) {
    return (int)(unsigned)(a - b);
}

// Convert between 32-bit little-endian and native order
static inline uint32_t rlfs_fromle32(uint32_t a) {
#if (defined(  BYTE_ORDER  ) && defined(  ORDER_LITTLE_ENDIAN  ) &&   BYTE_ORDER   ==   ORDER_LITTLE_ENDIAN  ) || \
    (defined(__BYTE_ORDER  ) && defined(__ORDER_LITTLE_ENDIAN  ) && __BYTE_ORDER   == __ORDER_LITTLE_ENDIAN  ) || \
    (defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    return a;
#elif !defined(RLFS_NO_INTRINSICS) && ( \
    (defined(  BYTE_ORDER  ) && defined(  ORDER_BIG_ENDIAN  ) &&   BYTE_ORDER   ==   ORDER_BIG_ENDIAN  ) || \
    (defined(__BYTE_ORDER  ) && defined(__ORDER_BIG_ENDIAN  ) && __BYTE_ORDER   == __ORDER_BIG_ENDIAN  ) || \
    (defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__))
    return __builtin_bswap32(a);
#else
    return ((uint32_t)((uint8_t*)&a)[0] <<  0) |
           ((uint32_t)((uint8_t*)&a)[1] <<  8) |
           ((uint32_t)((uint8_t*)&a)[2] << 16) |
           ((uint32_t)((uint8_t*)&a)[3] << 24);
#endif
}

static inline uint32_t rlfs_tole32(uint32_t a) {
    return rlfs_fromle32(a);
}

// Convert between 32-bit big-endian and native order
static inline uint32_t rlfs_frombe32(uint32_t a) {
#if !defined(RLFS_NO_INTRINSICS) && ( \
    (defined(  BYTE_ORDER  ) && defined(  ORDER_LITTLE_ENDIAN  ) &&   BYTE_ORDER   ==   ORDER_LITTLE_ENDIAN  ) || \
    (defined(__BYTE_ORDER  ) && defined(__ORDER_LITTLE_ENDIAN  ) && __BYTE_ORDER   == __ORDER_LITTLE_ENDIAN  ) || \
    (defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__))
    return __builtin_bswap32(a);
#elif (defined(  BYTE_ORDER  ) && defined(  ORDER_BIG_ENDIAN  ) &&   BYTE_ORDER   ==   ORDER_BIG_ENDIAN  ) || \
    (defined(__BYTE_ORDER  ) && defined(__ORDER_BIG_ENDIAN  ) && __BYTE_ORDER   == __ORDER_BIG_ENDIAN  ) || \
    (defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    return a;
#else
    return ((uint32_t)((uint8_t*)&a)[0] << 24) |
           ((uint32_t)((uint8_t*)&a)[1] << 16) |
           ((uint32_t)((uint8_t*)&a)[2] <<  8) |
           ((uint32_t)((uint8_t*)&a)[3] <<  0);
#endif
}

static inline uint32_t rlfs_tobe32(uint32_t a) {
    return rlfs_frombe32(a);
}

// Calculate CRC-32 with polynomial = 0x04c11db7
#ifdef RLFS_CRC
static inline uint32_t rlfs_crc(uint32_t crc, const void *buffer, size_t size) {
    return RLFS_CRC(crc, buffer, size);
}
#else
uint32_t rlfs_crc(uint32_t crc, const void *buffer, size_t size);
#endif

// Allocate memory, only used if buffers are not provided to littlefs
//
// littlefs current has no alignment requirements, as it only allocates
// byte-level buffers.
static inline void *rlfs_malloc(size_t size) {
#if defined(RLFS_MALLOC)
    return RLFS_MALLOC(size);
#elif !defined(RLFS_NO_MALLOC)
    return malloc(size);
#else
    (void)size;
    return NULL;
#endif
}

// Deallocate memory, only used if buffers are not provided to littlefs
static inline void rlfs_free(void *p) {
#if defined(RLFS_FREE)
    RLFS_FREE(p);
#elif !defined(RLFS_NO_MALLOC)
    free(p);
#else
    (void)p;
#endif
}


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
#endif
