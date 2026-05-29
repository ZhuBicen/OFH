#ifndef CRC32_H
#define CRC32_H

#include <stddef.h>
#include <stdint.h>

uint32_t crc32_ref(const uint8_t* data, size_t length, uint32_t init);
uint32_t crc32_slicing4(const uint8_t* data, size_t length, uint32_t init);
uint32_t crc32_hw(const uint8_t* data, size_t length, uint32_t init);

#endif
