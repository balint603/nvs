/*
 * crc8.c
 *
 *  Created on: Mar 19, 2021
 *      Author: major
 */

#include <stdio.h>
#include <inttypes.h>
#include "nvs.h"

static const uint8_t crc8_ccitt_small_table[16] = {
	0x00, 0x07, 0x0e, 0x09, 0x1c, 0x1b, 0x12, 0x15,
	0x38, 0x3f, 0x36, 0x31, 0x24, 0x23, 0x2a, 0x2d
};

uint8_t crc8_ccitt(uint8_t val, const void *buf, size_t cnt)
{
	size_t i;
	const uint8_t *p = buf;

	for (i = 0; i < cnt; i++) {
		val ^= p[i];
		val = (val << 4) ^ crc8_ccitt_small_table[val >> 4];
		val = (val << 4) ^ crc8_ccitt_small_table[val >> 4];
	}
	return val;
}
