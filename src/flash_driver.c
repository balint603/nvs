/*
 * nvs_flash.c
 *
 *  Created on: Mar 19, 2021
 *      Author: major
 */

#include <stdio.h>
#include <stdbool.h>

#include "stm32f0xx_hal.h"
#include "flash_driver.h"


const struct flash_parameters FLASH_PARAMS = {
		.write_block_size = FLASH_WRITE_BLOCK_SIZE,
		.erase_value = FLASH_ERASE_VALUE
};

const struct flash_parameters *flash_get_parameters() {
	return &FLASH_PARAMS;
}

int flash_write_protection_set(bool lock) {
	HAL_StatusTypeDef result;
	result = (lock) ? HAL_FLASH_Lock() : HAL_FLASH_Unlock() ;
	if ( result )										// todo If HAL_status busy then must wait a bit?
		return -1;
	else
		return 0;
}

int flash_write(int offset, const void *data, size_t len) {
	HAL_FLASH_Program(FLASH_WRITE_BLOCK_SIZE, offset, data);
}
int flash_read(int offset, void *data, size_t len);
int flash_erase(int offset, size_t size);
