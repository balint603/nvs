/*
 * nvs_flash.h
 *
 *  Created on: Mar 19, 2021
 *      Author: nobody
 */

#ifndef NVS_FLASH_H_
#define NVS_FLASH_H_

#include <inttypes.h>
#include <stdbool.h>

struct flash_parameters {
	const size_t write_block_size;
	uint8_t erase_value;
};

struct flash_pages_info {
	int start_offset; /* offset from the base of flash address */
	size_t size;
	uint32_t index;
};

#define FLASH_WRITE_BLOCK_SIZE 	4	/** Choose TYPEPROGAM from HAL. */
#define FLASH_ERASE_VALUE		0xFF

const struct flash_parameters *flash_get_parameters();

int flash_write_protection_set(bool lock);

int flash_write(int offset, const void *data, size_t len);
int flash_read(int offset, void *data, size_t len);
int flash_erase(int offset, size_t size);


#endif /* NVS_FLASH_H_ */
