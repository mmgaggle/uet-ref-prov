/*
 * Check crc32c_update() (the CRC32 instruction when the CPU has it)
 * against the table implementation and against a bit-at-a-time CRC-32C,
 * over the RFC 3720 vectors and over random buffers: every length from 0
 * to 1100, random lengths up to 20000, every start alignment from 0 to 15,
 * and random starting CRC values, including updates split at random
 * points.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "crc32c.h"

#define MAX_LEN 20000u
#define RANDOM_RUNS 20000u

static uint32_t crc32c_bitwise(uint32_t crc, const uint8_t *p, size_t len)
{
	while (len--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc & 1) ? (crc >> 1) ^ 0x82f63b78u : crc >> 1;
	}
	return crc;
}

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;

static uint64_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return rng_state;
}

static unsigned int failures;

static void check(uint32_t init, const uint8_t *p, size_t len)
{
	uint32_t want = crc32c_bitwise(init, p, len);
	uint32_t sw = crc32c_update_sw(init, p, len);
	uint32_t got = crc32c_update(init, p, len);
	size_t cut = len ? rng() % (len + 1) : 0;
	uint32_t split = crc32c_update(crc32c_update(init, p, cut), p + cut,
				       len - cut);

	if (sw != want || got != want || split != want) {
		if (failures++ < 10)
			fprintf(stderr, "len %zu align %u init %08x: bitwise "
				"%08x table %08x crc32c_update %08x split "
				"at %zu %08x\n", len,
				(unsigned int)((uintptr_t)p & 15), init, want,
				sw, got, cut, split);
	}
}

int main(void)
{
	/* RFC 3720 B.4, as crc32c() puts them in the packet */
	static const uint32_t rfc[4] = { 0x8a9136aa, 0x62a8ab43, 0x46dd794e,
					 0x113fdb5c };
	uint8_t *buf = malloc(MAX_LEN + 16);
	uint8_t data[32];
	size_t len, i;
	unsigned int a;

	if (buf == NULL)
		return 2;
	for (i = 0; i < MAX_LEN + 16; i++)
		buf[i] = (uint8_t)rng();

	memset(data, 0, sizeof(data));
	if (ntohl(crc32c(data, 32)) != rfc[0])
		failures++;
	memset(data, 0xff, sizeof(data));
	if (ntohl(crc32c(data, 32)) != rfc[1])
		failures++;
	for (i = 0; i < 32; i++)
		data[i] = (uint8_t)i;
	if (ntohl(crc32c(data, 32)) != rfc[2])
		failures++;
	for (i = 0; i < 32; i++)
		data[i] = (uint8_t)(31 - i);
	if (ntohl(crc32c(data, 32)) != rfc[3])
		failures++;
	if (failures)
		fprintf(stderr, "RFC 3720 vectors: %u wrong\n", failures);

	for (a = 0; a < 16; a++)
		for (len = 0; len <= 1100; len++)
			check(~0u, buf + a, len);
	for (i = 0; i < RANDOM_RUNS; i++)
		check((uint32_t)rng(), buf + rng() % 16, rng() % (MAX_LEN + 1));
	/* the lengths a packet has: the stream sizes and either side */
	for (a = 0; a < 16; a++)
		for (len = 6144 - 9; len <= 6144 + 9; len++)
			check(~0u, buf + a, len);
	for (a = 0; a < 16; a++)
		for (len = 768 - 9; len <= 768 + 9; len++)
			check(~0u, buf + a, len);

	printf("crc32c: %s path, %u mismatches\n",
	       crc32c_hw() ? "SSE4.2" : "table", failures);
	free(buf);
	return failures ? 1 : 0;
}
