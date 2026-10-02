/*
 * Copyright (c) 2024, Broadcom. All rights reserved. The term
 * Broadcom refers to Broadcom Limited and/or its subsidiaries.
 */

#ifndef _CRC32C_H_
#define _CRC32C_H_

#include <stdint.h>

#define CRC_LEN 4

uint32_t crc32c_init(void);

/*
 * Uses the CPU's CRC32 instruction (SSE4.2) when it has one, and the
 * table otherwise; crc32c_hw() says which.
 */
uint32_t crc32c_update(uint32_t crc,
		       const uint8_t *data,
		       uint32_t length);

/* the table, a byte at a time, whatever the CPU */
uint32_t crc32c_update_sw(uint32_t crc,
			  const uint8_t *data,
			  uint32_t length);

/* 1 when crc32c_update() uses a CRC instruction */
int crc32c_hw(void);

uint32_t crc32c_finish(uint32_t crc);

uint32_t crc32c(const uint8_t *data,
		uint32_t length);

#endif /* _CRC32C_H_ */

