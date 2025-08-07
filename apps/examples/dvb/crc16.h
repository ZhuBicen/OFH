/*
 * PICOCOM PROPRIETARY INFORMATION
 *
 * This software is supplied under the terms of a license agreement or
 * nondisclosure agreement with PICOCOM and may not be copied
 * or disclosed except in accordance with the terms of that agreement.
 *
 * Copyright PICOCOM.
 */
#pragma once
#include <stdint.h>
#include <stdlib.h>

uint16_t crc16_optimized(const uint8_t* data, size_t length, uint16_t initial);
uint16_t crc16_4bytes_optimized(const uint8_t* data, size_t length, uint16_t initial);
