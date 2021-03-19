/*  NVS: non volatile storage in flash
 *
 * Copyright (c) 2018 Laczen
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <errno.h>
#include <flash_driver.h>
#include <inttypes.h>
#include <nvs.h>
#include "nvs_priv.h"
#include "shell.h"

const char *TAG = "NVS";

/* basic routines */
/* nvs_al_size returns size aligned to fs->write_block_size */
static inline size_t nvs_al_size(struct nvs_fs *fs, size_t len)
{
	uint8_t write_block_size = fs->flash_parameters->write_block_size;

	if (write_block_size <= 1U) {
		return len;
	}
	return (len + (write_block_size - 1U)) & ~(write_block_size - 1U);
}
/* end basic routines */

/* flash routines */
/* basic aligned flash write to nvs address */
static int nvs_flash_al_wrt(struct nvs_fs *fs, uint32_t addr, const void *data,
			     size_t len)
{
	const uint8_t *data8 = (const uint8_t *)data;
	int rc = 0;
	off_t offset;
	size_t blen;
	uint8_t buf[NVS_BLOCK_SIZE];

	if (!len) {
		/* Nothing to write, avoid changing the flash protection */
		return 0;
	}

	offset = fs->offset;
	offset += fs->sector_size * (addr >> ADDR_SECT_SHIFT);
	offset += addr & ADDR_OFFS_MASK;

	rc = flash_write_protection_set(fs->flash_device, false);
	if (rc) {
		/* flash protection set error */
		return rc;
	}
	blen = len & ~(fs->flash_parameters->write_block_size - 1U);
	if (blen > 0) {
		rc = flash_write(fs->flash_device, offset, data8, blen);
		if (rc) {
			/* flash write error */
			goto end;
		}
		len -= blen;
		offset += blen;
		data8 += blen;
	}
	if (len) {
		memcpy(buf, data8, len);
		(void)memset(buf + len, fs->flash_parameters->erase_value,
			fs->flash_parameters->write_block_size - len);

		rc = flash_write(fs->flash_device, offset, buf,
				 fs->flash_parameters->write_block_size);
		if (rc) {
			/* flash write error */
			goto end;
		}
	}

end:
	(void) flash_write_protection_set(fs->flash_device, true);
	return rc;
}

/* basic flash read from nvs address */
static int nvs_flash_rd(struct nvs_fs *fs, uint32_t addr, void *data,
			 size_t len)
{
	int rc;
	off_t offset;

	offset = fs->offset;
	offset += fs->sector_size * (addr >> ADDR_SECT_SHIFT);
	offset += addr & ADDR_OFFS_MASK;

	rc = flash_read(fs->flash_device, offset, data, len);
	return rc;

}

/* allocation entry write */
static int nvs_flash_ate_wrt(struct nvs_fs *fs, const struct nvs_ate *entry)
{
	int rc;

	rc = nvs_flash_al_wrt(fs, fs->ate_wra, entry,
			       sizeof(struct nvs_ate));
	fs->ate_wra -= nvs_al_size(fs, sizeof(struct nvs_ate));

	return rc;
}

/* data write */
static int nvs_flash_data_wrt(struct nvs_fs *fs, const void *data, size_t len)
{
	int rc;

	rc = nvs_flash_al_wrt(fs, fs->data_wra, data, len);
	fs->data_wra += nvs_al_size(fs, len);

	return rc;
}

/* flash ate read */
static int nvs_flash_ate_rd(struct nvs_fs *fs, uint32_t addr,
			     struct nvs_ate *entry)
{
	return nvs_flash_rd(fs, addr, entry, sizeof(struct nvs_ate));
}

/* end of basic flash routines */

/* advanced flash routines */

/* nvs_flash_block_cmp compares the data in flash at addr to data
 * in blocks of size NVS_BLOCK_SIZE aligned to fs->write_block_size
 * returns 0 if equal, 1 if not equal, errcode if error
 */
static int nvs_flash_block_cmp(struct nvs_fs *fs, uint32_t addr, const void *data,
				size_t len)
{
	const uint8_t *data8 = (const uint8_t *)data;
	int rc;
	size_t bytes_to_cmp, block_size;
	uint8_t buf[NVS_BLOCK_SIZE];

	block_size =
		NVS_BLOCK_SIZE & ~(fs->flash_parameters->write_block_size - 1U);

	while (len) {
		bytes_to_cmp = MIN(block_size, len);
		rc = nvs_flash_rd(fs, addr, buf, bytes_to_cmp);
		if (rc) {
			return rc;
		}
		rc = memcmp(data8, buf, bytes_to_cmp);
		if (rc) {
			return 1;
		}
		len -= bytes_to_cmp;
		addr += bytes_to_cmp;
		data8 += bytes_to_cmp;
	}
	return 0;
}

/* nvs_flash_cmp_const compares the data in flash at addr to a constant
 * value. returns 0 if all data in flash is equal to value, 1 if not equal,
 * errcode if error
 */
static int nvs_flash_cmp_const(struct nvs_fs *fs, uint32_t addr, uint8_t value,
				size_t len)
{
	int rc;
	size_t bytes_to_cmp, block_size;
	uint8_t cmp[NVS_BLOCK_SIZE];

	block_size =
		NVS_BLOCK_SIZE & ~(fs->flash_parameters->write_block_size - 1U);

	(void)memset(cmp, value, block_size);
	while (len) {
		bytes_to_cmp = MIN(block_size, len);
		rc = nvs_flash_block_cmp(fs, addr, cmp, bytes_to_cmp);
		if (rc) {
			return rc;
		}
		len -= bytes_to_cmp;
		addr += bytes_to_cmp;
	}
	return 0;
}

/* flash block move: move a block at addr to the current data write location
 * and updates the data write location.
 */
static int nvs_flash_block_move(struct nvs_fs *fs, uint32_t addr, size_t len)
{
	int rc;
	size_t bytes_to_copy, block_size;
	uint8_t buf[NVS_BLOCK_SIZE];

	block_size =
		NVS_BLOCK_SIZE & ~(fs->flash_parameters->write_block_size - 1U);

	while (len) {
		bytes_to_copy = MIN(block_size, len);
		rc = nvs_flash_rd(fs, addr, buf, bytes_to_copy);
		if (rc) {
			return rc;
		}
		rc = nvs_flash_data_wrt(fs, buf, bytes_to_copy);
		if (rc) {
			return rc;
		}
		len -= bytes_to_copy;
		addr += bytes_to_copy;
	}
	return 0;
}

/* erase a sector by first checking it is used and then erasing if required
 * return 0 if OK, errorcode on error.
 */
static int nvs_flash_erase_sector(struct nvs_fs *fs, uint32_t addr)
{
	int rc;
	off_t offset;

	addr &= ADDR_SECT_MASK;
	rc = nvs_flash_cmp_const(fs, addr, fs->flash_parameters->erase_value,
			fs->sector_size);
	if (rc <= 0) {
		/* flash error or empty sector */
		return rc;
	}

	offset = fs->offset;
	offset += fs->sector_size * (addr >> ADDR_SECT_SHIFT);

	rc = flash_write_protection_set(fs->flash_device, false);
	if (rc) {
		/* flash protection set error */
		return rc;
	}
	LOG_DBG("Erasing flash at %lx, len %d", (long int) offset,
		fs->sector_size);
	rc = flash_erase(fs->flash_device, offset, fs->sector_size);
	if (rc) {
		/* flash erase error */
		return rc;
	}
	(void) flash_write_protection_set(fs->flash_device, true);
	return 0;
}

/* crc update on allocation entry */
static void nvs_ate_crc8_update(struct nvs_ate *entry)
{
	uint8_t crc8;

	crc8 = crc8_ccitt(0xff, entry, offsetof(struct nvs_ate, crc8));
	entry->crc8 = crc8;
}

/* crc check on allocation entry
 * returns 0 if OK, 1 on crc fail
 */
static int nvs_ate_crc8_check(const struct nvs_ate *entry)
{
	uint8_t crc8;

	crc8 = crc8_ccitt(0xff, entry, offsetof(struct nvs_ate, crc8));
	if (crc8 == entry->crc8) {
		return 0;
	}
	return 1;
}

/* nvs_ate_cmp_const compares an ATE to a constant value. returns 0 if
 * the whole ATE is equal to value, 1 if not equal.
 */

static int nvs_ate_cmp_const(const struct nvs_ate *entry, uint8_t value)
{
	const uint8_t *data8 = (const uint8_t *)entry;
	int i;

	for (i = 0; i < sizeof(struct nvs_ate); i++) {
		if (data8[i] != value) {
			return 1;
		}
	}

	return 0;
}

/* store an entry in flash */
static int nvs_flash_wrt_entry(struct nvs_fs *fs, uint16_t id, const void *data,
				size_t len)
{
	int rc;
	struct nvs_ate entry;
	size_t ate_size;

	ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));

	entry.id = id;
	entry.offset = (uint16_t)(fs->data_wra & ADDR_OFFS_MASK);
	entry.len = (uint16_t)len;
	entry.part = 0xff;

	nvs_ate_crc8_update(&entry);

	rc = nvs_flash_data_wrt(fs, data, len);
	if (rc) {
		return rc;
	}
	rc = nvs_flash_ate_wrt(fs, &entry);
	if (rc) {
		return rc;
	}

	return 0;
}
/* end of flash routines */

/* If the closing ate has an invalid crc8, its offset cannot be trusted and
 * the last valod ate of the sector should instead try to be recovered by going
 * through all ate's.
 *
 * addr should point to the faulty closing ate and will be updated to the last
 * valid ate. If no valid ate is found it will be left untouched.
 */
static int nvs_recover_last_ate(struct nvs_fs *fs, uint32_t *addr)
{
	uint32_t data_end_addr, ate_end_addr;
	struct nvs_ate end_ate;
	size_t ate_size;
	int rc;

	LOG_DBG("Recovering last ate from sector %d",
		(*addr >> ADDR_SECT_SHIFT));

	ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));

	*addr -= ate_size;
	ate_end_addr = *addr;
	data_end_addr = *addr & ADDR_SECT_MASK;
	while (ate_end_addr > data_end_addr) {
		rc = nvs_flash_ate_rd(fs, ate_end_addr, &end_ate);
		if (rc) {
			return rc;
		}
		if (!nvs_ate_crc8_check(&end_ate)) {
			/* found a valid ate, update data_end_addr and *addr */
			data_end_addr &= ADDR_SECT_MASK;
			data_end_addr += end_ate.offset + end_ate.len;
			*addr = ate_end_addr;
		}
		ate_end_addr -= ate_size;
	}

	return 0;
}

/* walking through allocation entry list, from newest to oldest entries
 * read ate from addr, modify addr to the previous ate
 */
static int nvs_prev_ate(struct nvs_fs *fs, uint32_t *addr, struct nvs_ate *ate)
{
	int rc;
	struct nvs_ate close_ate;
	size_t ate_size;

	ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));

	rc = nvs_flash_ate_rd(fs, *addr, ate);
	if (rc) {
		return rc;
	}

	*addr += ate_size;
	if (((*addr) & ADDR_OFFS_MASK) != (fs->sector_size - ate_size)) {
		return 0;
	}

	/* last ate in sector, do jump to previous sector */
	if (((*addr) >> ADDR_SECT_SHIFT) == 0U) {
		*addr += ((fs->sector_count - 1) << ADDR_SECT_SHIFT);
	} else {
		*addr -= (1 << ADDR_SECT_SHIFT);
	}

	rc = nvs_flash_ate_rd(fs, *addr, &close_ate);
	if (rc) {
		return rc;
	}

	rc = nvs_ate_cmp_const(&close_ate, fs->flash_parameters->erase_value);
	/* at the end of filesystem */
	if (!rc) {
		*addr = fs->ate_wra;
		return 0;
	}

	if (!nvs_ate_crc8_check(&close_ate)) {
		/* update the address so it points to the last added ate.
		 * do a check on close_ate.offset so that it does not point
		 * outside a sector and is aligned to ate size.
		 */
		if (close_ate.offset < (fs->sector_size - ate_size) &&
		    !(close_ate.offset % ate_size)) {
			(*addr) &= ADDR_SECT_MASK;
			(*addr) += close_ate.offset;
			return 0;
		}
	}
	/* The close_ate had an invalid CRC8 or the last added ate offset was
	 * recognized as incorrect, `lets find out the last valid ate
	 * and point the address to this found ate.
	 *
	 * remark: if there was absolutely no valid data in the sector *addr
	 * is kept at sector_end - 2*ate_size, the next read will contain
	 * invalid data and continue with a sector jump
	 */
	return nvs_recover_last_ate(fs, addr);
}

static void nvs_sector_advance(struct nvs_fs *fs, uint32_t *addr)
{
	*addr += (1 << ADDR_SECT_SHIFT);
	if ((*addr >> ADDR_SECT_SHIFT) == fs->sector_count) {
		*addr -= (fs->sector_count << ADDR_SECT_SHIFT);
	}
}

/* allocation entry close (this closes the current sector) by writing offset
 * of last ate to the sector end.
 */
static int nvs_sector_close(struct nvs_fs *fs)
{
	int rc;
	struct nvs_ate close_ate;
	size_t ate_size;

	ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));

	close_ate.id = 0xFFFF;
	close_ate.len = 0U;
	close_ate.offset = (uint16_t)((fs->ate_wra + ate_size) & ADDR_OFFS_MASK);

	fs->ate_wra &= ADDR_SECT_MASK;
	fs->ate_wra += (fs->sector_size - ate_size);

	nvs_ate_crc8_update(&close_ate);

	rc = nvs_flash_ate_wrt(fs, &close_ate);

	nvs_sector_advance(fs, &fs->ate_wra);

	fs->data_wra = fs->ate_wra & ADDR_SECT_MASK;

	return 0;
}


/* garbage collection: the address ate_wra has been updated to the new sector
 * that has just been started. The data to gc is in the sector after this new
 * sector.
 */
static int nvs_gc(struct nvs_fs *fs)
{
	int rc;
	struct nvs_ate close_ate, gc_ate, wlk_ate;
	uint32_t sec_addr, gc_addr, gc_prev_addr, wlk_addr, wlk_prev_addr,
	      data_addr, stop_addr;
	size_t ate_size;

	ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));

	sec_addr = (fs->ate_wra & ADDR_SECT_MASK);
	nvs_sector_advance(fs, &sec_addr);
	gc_addr = sec_addr + fs->sector_size - ate_size;

	/* if the sector is not closed don't do gc */
	rc = nvs_flash_ate_rd(fs, gc_addr, &close_ate);
	if (rc < 0) {
		/* flash error */
		return rc;
	}

	rc = nvs_ate_cmp_const(&close_ate, fs->flash_parameters->erase_value);
	if (!rc) {
		rc = nvs_flash_erase_sector(fs, sec_addr);
		if (rc) {
			return rc;
		}
		return 0;
	}

	stop_addr = gc_addr - ate_size;

	if (!nvs_ate_crc8_check(&close_ate)) {
		gc_addr &= ADDR_SECT_MASK;
		gc_addr += close_ate.offset;
	} else {
		rc = nvs_recover_last_ate(fs, &gc_addr);
		if (rc) {
			return rc;
		}
	}

	do {
		gc_prev_addr = gc_addr;
		rc = nvs_prev_ate(fs, &gc_addr, &gc_ate);
		if (rc) {
			return rc;
		}

		if (nvs_ate_crc8_check(&gc_ate)) {
			continue;
		}

		wlk_addr = fs->ate_wra;
		do {
			wlk_prev_addr = wlk_addr;
			rc = nvs_prev_ate(fs, &wlk_addr, &wlk_ate);
			if (rc) {
				return rc;
			}
			/* if ate with same id is reached we might need to copy.
			 * only consider valid wlk_ate's. Something wrong might
			 * have been written that has the same ate but is
			 * invalid, don't consider these as a match.
			 */
			if ((wlk_ate.id == gc_ate.id) &&
			    (!nvs_ate_crc8_check(&wlk_ate))) {
				break;
			}
		} while (wlk_addr != fs->ate_wra);

		/* if walk has reached the same address as gc_addr copy is
		 * needed unless it is a deleted item.
		 */
		if ((wlk_prev_addr == gc_prev_addr) && gc_ate.len) {
			/* copy needed */
			LOG_DBG("Moving %d, len %d", gc_ate.id, gc_ate.len);

			data_addr = (gc_prev_addr & ADDR_SECT_MASK);
			data_addr += gc_ate.offset;

			gc_ate.offset = (uint16_t)(fs->data_wra & ADDR_OFFS_MASK);
			nvs_ate_crc8_update(&gc_ate);

			rc = nvs_flash_block_move(fs, data_addr, gc_ate.len);
			if (rc) {
				return rc;
			}

			rc = nvs_flash_ate_wrt(fs, &gc_ate);
			if (rc) {
				return rc;
			}
		}
	} while (gc_prev_addr != stop_addr);

	rc = nvs_flash_erase_sector(fs, sec_addr);
	if (rc) {
		return rc;
	}
	return 0;
}

static int nvs_startup(struct nvs_fs *fs)
{
	int rc;
	struct nvs_ate last_ate;
	size_t ate_size, empty_len;
	/* Initialize addr to 0 for the case fs->sector_count == 0. This
	 * should never happen as this is verified in nvs_init() but both
	 * Coverity and GCC believe the contrary.
	 */
	uint32_t addr = 0U;
	uint16_t i, closed_sectors = 0;
	uint8_t erase_value = fs->flash_parameters->erase_value;

	k_mutex_lock(&fs->nvs_lock, K_FOREVER);

	ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));
	/* step through the sectors to find a open sector following
	 * a closed sector, this is where NVS can to write.
	 */
	for (i = 0; i < fs->sector_count; i++) {
		addr = (i << ADDR_SECT_SHIFT) +
		       (uint16_t)(fs->sector_size - ate_size);
		rc = nvs_flash_cmp_const(fs, addr, erase_value,
					 sizeof(struct nvs_ate));
		if (rc) {
			/* closed sector */
			closed_sectors++;
			nvs_sector_advance(fs, &addr);
			rc = nvs_flash_cmp_const(fs, addr, erase_value,
						 sizeof(struct nvs_ate));
			if (!rc) {
				/* open sector */
				break;
			}
		}
	}
	/* all sectors are closed, this is not a nvs fs */
	if (closed_sectors == fs->sector_count) {
		rc = -EDEADLK;
		goto end;
	}

	if (i == fs->sector_count) {
		/* none of the sectors where closed, in most cases we can set
		 * the address to the first sector, except when there are only
		 * two sectors. Then we can only set it to the first sector if
		 * the last sector contains no ate's. So we check this first
		 */
		rc = nvs_flash_cmp_const(fs, addr - ate_size, erase_value,
				sizeof(struct nvs_ate));
		if (!rc) {
			/* empty ate */
			nvs_sector_advance(fs, &addr);
		}
	}

	/* addr contains address of the last ate in the most recent sector
	 * search for the first ate containing all cells erased.
	 */
	fs->ate_wra = addr - ate_size;
	fs->data_wra = addr & ADDR_SECT_MASK;

	while (fs->ate_wra >= fs->data_wra) {
		rc = nvs_flash_ate_rd(fs, fs->ate_wra, &last_ate);
		if (rc) {
			goto end;
		}

		rc = nvs_ate_cmp_const(&last_ate, erase_value);

		if (!rc) {
			/* found ff empty location */
			break;
		}

		if (!nvs_ate_crc8_check(&last_ate)) {
			/* crc8 is ok, complete write of ate was performed */
			fs->data_wra = addr & ADDR_SECT_MASK;
			fs->data_wra += last_ate.offset;
			fs->data_wra += nvs_al_size(fs, last_ate.len);

			/* ate on the last possition within the sector is
			 * reserved for deletion an entry
			 */
			if (fs->ate_wra == fs->data_wra && last_ate.len) {
				/* not a delete ate */
				rc = -ESPIPE;
				goto end;
			}
		}

		fs->ate_wra -= ate_size;
	}

	/* possible data write after last ate write, update data_wra */
	while (fs->ate_wra > fs->data_wra) {
		empty_len = fs->ate_wra - fs->data_wra;

		rc = nvs_flash_cmp_const(fs, fs->data_wra, erase_value,
				empty_len);
		if (rc < 0) {
			goto end;
		}
		if (!rc) {
			break;
		}

		fs->data_wra += fs->flash_parameters->write_block_size;
	}

	/* if the sector after the write sector is not empty gc was interrupted
	 * we need to restart gc, first erase the sector before restarting gc
	 * otherwise the data may not fit into the sector.
	 */
	addr = fs->ate_wra & ADDR_SECT_MASK;
	nvs_sector_advance(fs, &addr);
	rc = nvs_flash_cmp_const(fs, addr, erase_value, fs->sector_size);
	if (rc < 0) {
		goto end;
	}
	if (rc) {
		/* the sector after fs->ate_wrt is not empty */
		rc = nvs_flash_erase_sector(fs, fs->ate_wra);
		if (rc) {
			goto end;
		}
		fs->ate_wra &= ADDR_SECT_MASK;
		fs->ate_wra += (fs->sector_size - 2 * ate_size);
		fs->data_wra = (fs->ate_wra & ADDR_SECT_MASK);
		rc = nvs_gc(fs);
		if (rc) {
			goto end;
		}
	}

end:
	k_mutex_unlock(&fs->nvs_lock);
	return rc;
}

int nvs_clear(struct nvs_fs *fs)
{
	int rc;
	uint32_t addr;

	if (!fs->ready) {
		LOG_ERR("NVS not initialized");
		return -EACCES;
	}

	for (uint16_t i = 0; i < fs->sector_count; i++) {
		addr = i << ADDR_SECT_SHIFT;
		rc = nvs_flash_erase_sector(fs, addr);
		if (rc) {
			return rc;
		}
	}
	return 0;
}

int nvs_init(struct nvs_fs *fs, const char *dev_name)
{

	int rc;
	//struct flash_pages_info info;
	size_t pagesize;

	k_mutex_init(&fs->nvs_lock);

	/*fs->flash_device = device_get_binding(dev_name);
	if (!fs->flash_device) {
		LOG_ERR("No valid flash device found");
		return -ENXIO;
	} */

	fs->flash_parameters = flash_get_parameters();
	if (fs->flash_parameters == NULL) {
		sh_log_printf(TAG, "Could not obtain flash parameters");
		return -EINVAL;
	}

	/* check that the write block size is supported */
	if (FLASH_WRITE_BLOCK_SIZE > NVS_BLOCK_SIZE || FLASH_WRITE_BLOCK_SIZE == 0) {
		sh_log_printf(TAG, "Unsupported write block size");
		return -EINVAL;
	}

	/* check that sector size is a multiple of pagesize */
	if (!fs->sector_size || fs->sector_size % FLASH_PAGE_SIZE) {
		sh_log_printf(TAG, "Invalid sector size");
		return -EINVAL;
	}

	/* check the number of sectors, it should be at least 2 */
	if (fs->sector_count < 2) {
		sh_log_printf(TAG, "Configuration error - sector count");
		return -EINVAL;
	}

	rc = nvs_startup(fs);
	if (rc) {
		return rc;
	}

	/* nvs is ready for use */
	fs->ready = true;

	sh_log_printf(TAG, "%d Sectors of %d bytes", fs->sector_count, fs->sector_size);
	sh_log_printf(TAG, "alloc wra: %d, %x",
		(fs->ate_wra >> ADDR_SECT_SHIFT),
		(fs->ate_wra & ADDR_OFFS_MASK));
	sh_log_printf(TAG, "data wra: %d, %x",
		(fs->data_wra >> ADDR_SECT_SHIFT),
		(fs->data_wra & ADDR_OFFS_MASK));

	return 0;
}

size_t nvs_write(struct nvs_fs *fs, uint16_t id, const void *data, size_t len)
{
	int rc, gc_count;
	size_t ate_size, data_size;
	struct nvs_ate wlk_ate;
	uint32_t wlk_addr, rd_addr;
	uint16_t required_space = 0U; /* no space, appropriate for delete ate */
	bool prev_found = false;

	if (!fs->ready) {
		LOG_ERR("NVS not initialized");
		return -EACCES;
	}

	ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));
	data_size = nvs_al_size(fs, len);

	/* The maximum data size is sector size - 3 ate
	 * where: 1 ate for data, 1 ate for sector close
	 * and 1 ate to always allow a delete.
	 */
	if ((len > (fs->sector_size - 3 * ate_size)) ||
	    ((len > 0) && (data == NULL))) {
		return -EINVAL;
	}

	/* find latest entry with same id */
	wlk_addr = fs->ate_wra;
	rd_addr = wlk_addr;

	while (1) {
		rd_addr = wlk_addr;
		rc = nvs_prev_ate(fs, &wlk_addr, &wlk_ate);
		if (rc) {
			return rc;
		}
		if ((wlk_ate.id == id) && (!nvs_ate_crc8_check(&wlk_ate))) {
			prev_found = true;
			break;
		}
		if (wlk_addr == fs->ate_wra) {
			break;
		}
	}

	if (prev_found) {
		/* previous entry found */
		rd_addr &= ADDR_SECT_MASK;
		rd_addr += wlk_ate.offset;

		if (len == 0) {
			/* do not try to compare with empty data */
			if (wlk_ate.len == 0U) {
				/* skip delete entry as it is already the
				 * last one
				 */
				return 0;
			}
		} else if (len == wlk_ate.len) {
			/* do not try to compare if lengths are not equal */
			/* compare the data and if equal return 0 */
			rc = nvs_flash_block_cmp(fs, rd_addr, data, len);
			if (rc <= 0) {
				return rc;
			}
		}
	} else {
		/* skip delete entry for non-existing entry */
		if (len == 0) {
			return 0;
		}
	}

	/* calculate required space if the entry contains data */
	if (data_size) {
		/* Leave space for delete ate */
		required_space = data_size + ate_size;
	}

	k_mutex_lock(&fs->nvs_lock, K_FOREVER);

	gc_count = 0;
	while (1) {
		if (gc_count == fs->sector_count) {
			/* gc'ed all sectors, no extra space will be created
			 * by extra gc.
			 */
			rc = -ENOSPC;
			goto end;
		}

		if (fs->ate_wra >= fs->data_wra + required_space) {

			rc = nvs_flash_wrt_entry(fs, id, data, len);
			if (rc) {
				goto end;
			}
			break;
		}


		rc = nvs_sector_close(fs);
		if (rc) {
			goto end;
		}

		rc = nvs_gc(fs);
		if (rc) {
			goto end;
		}
		gc_count++;
	}
	rc = len;
end:
	k_mutex_unlock(&fs->nvs_lock);
	return rc;
}

int nvs_delete(struct nvs_fs *fs, uint16_t id)
{
	return nvs_write(fs, id, NULL, 0);
}

size_t nvs_read_hist(struct nvs_fs *fs, uint16_t id, void *data, size_t len,
		      uint16_t cnt)
{
	int rc;
	uint32_t wlk_addr, rd_addr;
	uint16_t cnt_his;
	struct nvs_ate wlk_ate;
	size_t ate_size;

	if (!fs->ready) {
		LOG_ERR("NVS not initialized");
		return -EACCES;
	}

	ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));

	if (len > (fs->sector_size - 2 * ate_size)) {
		return -EINVAL;
	}

	cnt_his = 0U;

	wlk_addr = fs->ate_wra;
	rd_addr = wlk_addr;

	while (cnt_his <= cnt) {
		rd_addr = wlk_addr;
		rc = nvs_prev_ate(fs, &wlk_addr, &wlk_ate);
		if (rc) {
			goto err;
		}
		if ((wlk_ate.id == id) &&  (!nvs_ate_crc8_check(&wlk_ate))) {
			cnt_his++;
		}
		if (wlk_addr == fs->ate_wra) {
			break;
		}
	}

	if (((wlk_addr == fs->ate_wra) && (wlk_ate.id != id)) ||
	    (wlk_ate.len == 0U) || (cnt_his < cnt)) {
		return -ENOENT;
	}

	rd_addr &= ADDR_SECT_MASK;
	rd_addr += wlk_ate.offset;
	rc = nvs_flash_rd(fs, rd_addr, data, MIN(len, wlk_ate.len));
	if (rc) {
		goto err;
	}

	return wlk_ate.len;

err:
	return rc;
}

size_t nvs_read(struct nvs_fs *fs, uint16_t id, void *data, size_t len)
{
	int rc;

	rc = nvs_read_hist(fs, id, data, len, 0);
	return rc;
}

size_t nvs_calc_free_space(struct nvs_fs *fs)
{

	int rc;
	struct nvs_ate step_ate, wlk_ate;
	uint32_t step_addr, wlk_addr;
	size_t ate_size, free_space;

	if (!fs->ready) {
		LOG_ERR("NVS not initialized");
		return -EACCES;
	}

	ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));

	free_space = 0;
	for (uint16_t i = 1; i < fs->sector_count; i++) {
		free_space += (fs->sector_size - ate_size);
	}

	step_addr = fs->ate_wra;

	while (1) {
		rc = nvs_prev_ate(fs, &step_addr, &step_ate);
		if (rc) {
			return rc;
		}

		wlk_addr = fs->ate_wra;

		while (1) {
			rc = nvs_prev_ate(fs, &wlk_addr, &wlk_ate);
			if (rc) {
				return rc;
			}
			if ((wlk_ate.id == step_ate.id) ||
			    (wlk_addr == fs->ate_wra)) {
				break;
			}
		}

		if ((wlk_addr == step_addr) && step_ate.len &&
		    (!nvs_ate_crc8_check(&step_ate))) {
			/* count needed */
			free_space -= nvs_al_size(fs, step_ate.len);
			free_space -= ate_size;
		}

		if (step_addr == fs->ate_wra) {
			break;
		}

	}
	return free_space;
}
