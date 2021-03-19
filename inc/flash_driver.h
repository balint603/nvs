/*
 * nvs_flash.h
 *
 *  Created on: Mar 19, 2021
 *      Author: nobody
 */

#ifndef NVS_FLASH_H_
#define NVS_FLASH_H_

#include <inttypes.h>

struct flash_parameters {
	const size_t write_block_size;
	uint8_t erase_value;
};

struct flash_pages_info {
	int start_offset; /* offset from the base of flash address */
	size_t size;
	uint32_t index;
};

#define FLASH_WRITE_BLOCK_SIZE 	4	/** 32bit writing. */
#define FLASH_PAGE_SIZE			1024

const struct flash_parameters *flash_get_parameters();
size_t flash_get_write_block_size();
int flash_get_pagesize_by_offset(int offset, size_t *size);


#endif /* NVS_FLASH_H_ */
