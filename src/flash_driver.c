/*
 * nvs_flash.c
 *
 *  Created on: Mar 19, 2021
 *      Author: major
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "stm32f0xx_hal.h"
#include "stm32f0xx_hal_flash_ex.h"
#include "flash_driver.h"


const struct flash_parameters FLASH_PARAMS = {
		.write_block_size = FLASH_WRITE_BLOCK_SIZE,
		.erase_value = FLASH_ERASE_VALUE
};

const struct flash_parameters *flash_get_parameters() {
	return &FLASH_PARAMS;
}

int flash_write_protection_set(bool lock) {
	return (lock) ? HAL_FLASH_Lock() : HAL_FLASH_Unlock() ;
}

int flash_write(int offset, const void *data, size_t len) {
	if ( !data || !len )
		return -1;
	if ( !(IS_FLASH_PROGRAM_ADDRESS(offset)
	  && IS_FLASH_PROGRAM_ADDRESS(offset + len)) ) {
		return -1;
	}
	HAL_StatusTypeDef res;
	while ( len > 0 ) {
		res = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, offset, *((uint64_t*)data));
		switch ( res ) {
			case HAL_BUSY:
				HAL_Delay( 5 );
				break;
			case HAL_OK:
				len -= FLASH_WRITE_BLOCK_SIZE;
				data += FLASH_WRITE_BLOCK_SIZE;
				offset += FLASH_WRITE_BLOCK_SIZE;
				break;
			default:
				return res;
				break;
		}
	}
	return HAL_OK;
}

int flash_read(int offset, void *data, size_t len) {
	if ( !data || !len ) {
		return -1;
	}
	if ( IS_FLASH_PROGRAM_ADDRESS(offset)
	  && IS_FLASH_PROGRAM_ADDRESS(offset + len)) {
		memcpy( data, (void *)(offset), len); // Random Flash memory access.
	} else {
		return -2;
	}
	return 0;
}

int flash_erase(int offset, size_t size) {
	if ( !IS_FLASH_PROGRAM_ADDRESS(offset) )
		return -1;
	FLASH_EraseInitTypeDef erase_conf = {
			.NbPages = 1,
			.PageAddress = (offset),
			.TypeErase = FLASH_TYPEERASE_PAGES
	};
	uint32_t page_error;
	HAL_FLASHEx_Erase(&erase_conf, &page_error);
	if ( page_error != 0xFFFFFFFF )
		return -2;
	return 0;
}



























