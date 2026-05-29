/*
 * CRC-32 (IEEE 802.3) implementations.
 * Polynomial: 0x04C11DB7 (reflected: 0xEDB88320)
 */

#include "crc32.h"

#include <stdint.h>
#include <string.h>
#include <nmmintrin.h>   // _mm_crc32_u*

#define CRC32_POLY_REF 0xEDB88320U

/* =============== Reference: byte-by-byte table =============== */

static uint32_t crc32_table[256];
static int crc32_table_ready = 0;

static void init_crc32_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? ((c >> 1) ^ CRC32_POLY_REF) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_table_ready = 1;
}

uint32_t crc32_ref(const uint8_t* data, size_t length, uint32_t init) {
    if (!crc32_table_ready) init_crc32_table();
    uint32_t crc = init ^ 0xFFFFFFFFU;
    for (size_t i = 0; i < length; i++)
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xFF];
    return crc ^ 0xFFFFFFFFU;
}

/* =============== Slicing-by-4: 4 independent lookups =============== */

static uint32_t crc32_slice[4][256];
static int slice_ready = 0;

static void init_slice_tables(void) {
    for (int i = 0; i < 256; i++)
        crc32_slice[0][i] = crc32_table[i];
    for (int k = 1; k < 4; k++)
        for (int i = 0; i < 256; i++) {
            uint32_t crc = crc32_table[i];
            for (int n = 0; n < k; n++)
                crc = (crc >> 8) ^ crc32_table[crc & 0xFF];
            crc32_slice[k][i] = crc;
        }
    slice_ready = 1;
}

uint32_t crc32_slicing4(const uint8_t* data, size_t length, uint32_t init) {
    if (!crc32_table_ready) init_crc32_table();
    if (!slice_ready) init_slice_tables();

    uint32_t crc = init ^ 0xFFFFFFFFU;

    uint32_t align = (4 - ((intptr_t)data & 0x3)) & 0x3;
    uint32_t prefix = (align < length) ? align : (uint32_t)length;
    for (uint32_t i = 0; i < prefix; i++)
        crc = (crc >> 8) ^ crc32_table[(crc ^ *data++) & 0xFF];
    length -= prefix;

    uint32_t payload;
    while (length >= 4) {
        memcpy(&payload, data, 4);
        uint32_t tmp = crc ^ payload;
        crc = crc32_slice[3][tmp & 0xFF]
            ^ crc32_slice[2][(tmp >> 8) & 0xFF]
            ^ crc32_slice[1][(tmp >> 16) & 0xFF]
            ^ crc32_slice[0][(tmp >> 24) & 0xFF];
        data += 4;
        length -= 4;
    }

    while (length--)
        crc = (crc >> 8) ^ crc32_table[(crc ^ *data++) & 0xFF];

    return crc ^ 0xFFFFFFFFU;
}

/* =============== Hardware CRC32C instruction (SSE4.2) =============== */
/*
 * Intel's crc32 instruction computes CRC-32C (Castagnoli, polynomial
 * 0x1EDC6F41), NOT CRC-32 (IEEE).  This is the fastest possible CRC-32C
 * on x86 — a single micro-op per 8 bytes with 1-cycle throughput.
 */

uint32_t crc32_hw(const uint8_t* data, size_t length, uint32_t init) {
    uint32_t crc = init ^ 0xFFFFFFFFU;

    while (length >= 8) {
        crc = _mm_crc32_u64(crc, *(const uint64_t*)data);
        data += 8; length -= 8;
    }
    while (length >= 4) {
        crc = _mm_crc32_u32(crc, *(const uint32_t*)data);
        data += 4; length -= 4;
    }
    while (length--)
        crc = _mm_crc32_u8(crc, *data++);

    return crc ^ 0xFFFFFFFFU;
}
