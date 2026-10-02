/*
 * crc32c checksum
 *
 * Guy Castagnoli and Stefan Braeuer and Martin Herrman
 * Optimization of Cyclic Redundancy-Check Codes with 24 and 32 Parity Bits
 * IEEE Transactions on Communication
 * June 1993 Volume 41 Number 6
 *
 * Used by the iSCSI driver, possibly others, and derived from the
 * the iscsi-crc.c module of the linux-iscsi driver at
 * http://linux-iscsi.sourceforge.net.
 *
 * Copyright (c) 2004 Cisco Systems, Inc.
 * Copyright (c) 2008 Herbert Xu <herbert@gondor.apana.org.au>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * Copyright (c) 2024, Broadcom. All rights reserved. The term
 * Broadcom refers to Broadcom Limited and/or its subsidiaries.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>

#include "crc32c.h"

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <nmmintrin.h>
#define CRC32C_HAVE_SSE42 1
#endif

/*
 * This is the CRC-32C table
 * Generated with:
 * width = 32 bits
 * poly = 0x1EDC6F41
 * reflect input bytes = true
 * reflect output bytes = true
 */

static const uint32_t crc32c_table[256] = {
	0x00000000L, 0xF26B8303L, 0xE13B70F7L, 0x1350F3F4L,
	0xC79A971FL, 0x35F1141CL, 0x26A1E7E8L, 0xD4CA64EBL,
	0x8AD958CFL, 0x78B2DBCCL, 0x6BE22838L, 0x9989AB3BL,
	0x4D43CFD0L, 0xBF284CD3L, 0xAC78BF27L, 0x5E133C24L,
	0x105EC76FL, 0xE235446CL, 0xF165B798L, 0x030E349BL,
	0xD7C45070L, 0x25AFD373L, 0x36FF2087L, 0xC494A384L,
	0x9A879FA0L, 0x68EC1CA3L, 0x7BBCEF57L, 0x89D76C54L,
	0x5D1D08BFL, 0xAF768BBCL, 0xBC267848L, 0x4E4DFB4BL,
	0x20BD8EDEL, 0xD2D60DDDL, 0xC186FE29L, 0x33ED7D2AL,
	0xE72719C1L, 0x154C9AC2L, 0x061C6936L, 0xF477EA35L,
	0xAA64D611L, 0x580F5512L, 0x4B5FA6E6L, 0xB93425E5L,
	0x6DFE410EL, 0x9F95C20DL, 0x8CC531F9L, 0x7EAEB2FAL,
	0x30E349B1L, 0xC288CAB2L, 0xD1D83946L, 0x23B3BA45L,
	0xF779DEAEL, 0x05125DADL, 0x1642AE59L, 0xE4292D5AL,
	0xBA3A117EL, 0x4851927DL, 0x5B016189L, 0xA96AE28AL,
	0x7DA08661L, 0x8FCB0562L, 0x9C9BF696L, 0x6EF07595L,
	0x417B1DBCL, 0xB3109EBFL, 0xA0406D4BL, 0x522BEE48L,
	0x86E18AA3L, 0x748A09A0L, 0x67DAFA54L, 0x95B17957L,
	0xCBA24573L, 0x39C9C670L, 0x2A993584L, 0xD8F2B687L,
	0x0C38D26CL, 0xFE53516FL, 0xED03A29BL, 0x1F682198L,
	0x5125DAD3L, 0xA34E59D0L, 0xB01EAA24L, 0x42752927L,
	0x96BF4DCCL, 0x64D4CECFL, 0x77843D3BL, 0x85EFBE38L,
	0xDBFC821CL, 0x2997011FL, 0x3AC7F2EBL, 0xC8AC71E8L,
	0x1C661503L, 0xEE0D9600L, 0xFD5D65F4L, 0x0F36E6F7L,
	0x61C69362L, 0x93AD1061L, 0x80FDE395L, 0x72966096L,
	0xA65C047DL, 0x5437877EL, 0x4767748AL, 0xB50CF789L,
	0xEB1FCBADL, 0x197448AEL, 0x0A24BB5AL, 0xF84F3859L,
	0x2C855CB2L, 0xDEEEDFB1L, 0xCDBE2C45L, 0x3FD5AF46L,
	0x7198540DL, 0x83F3D70EL, 0x90A324FAL, 0x62C8A7F9L,
	0xB602C312L, 0x44694011L, 0x5739B3E5L, 0xA55230E6L,
	0xFB410CC2L, 0x092A8FC1L, 0x1A7A7C35L, 0xE811FF36L,
	0x3CDB9BDDL, 0xCEB018DEL, 0xDDE0EB2AL, 0x2F8B6829L,
	0x82F63B78L, 0x709DB87BL, 0x63CD4B8FL, 0x91A6C88CL,
	0x456CAC67L, 0xB7072F64L, 0xA457DC90L, 0x563C5F93L,
	0x082F63B7L, 0xFA44E0B4L, 0xE9141340L, 0x1B7F9043L,
	0xCFB5F4A8L, 0x3DDE77ABL, 0x2E8E845FL, 0xDCE5075CL,
	0x92A8FC17L, 0x60C37F14L, 0x73938CE0L, 0x81F80FE3L,
	0x55326B08L, 0xA759E80BL, 0xB4091BFFL, 0x466298FCL,
	0x1871A4D8L, 0xEA1A27DBL, 0xF94AD42FL, 0x0B21572CL,
	0xDFEB33C7L, 0x2D80B0C4L, 0x3ED04330L, 0xCCBBC033L,
	0xA24BB5A6L, 0x502036A5L, 0x4370C551L, 0xB11B4652L,
	0x65D122B9L, 0x97BAA1BAL, 0x84EA524EL, 0x7681D14DL,
	0x2892ED69L, 0xDAF96E6AL, 0xC9A99D9EL, 0x3BC21E9DL,
	0xEF087A76L, 0x1D63F975L, 0x0E330A81L, 0xFC588982L,
	0xB21572C9L, 0x407EF1CAL, 0x532E023EL, 0xA145813DL,
	0x758FE5D6L, 0x87E466D5L, 0x94B49521L, 0x66DF1622L,
	0x38CC2A06L, 0xCAA7A905L, 0xD9F75AF1L, 0x2B9CD9F2L,
	0xFF56BD19L, 0x0D3D3E1AL, 0x1E6DCDEEL, 0xEC064EEDL,
	0xC38D26C4L, 0x31E6A5C7L, 0x22B65633L, 0xD0DDD530L,
	0x0417B1DBL, 0xF67C32D8L, 0xE52CC12CL, 0x1747422FL,
	0x49547E0BL, 0xBB3FFD08L, 0xA86F0EFCL, 0x5A048DFFL,
	0x8ECEE914L, 0x7CA56A17L, 0x6FF599E3L, 0x9D9E1AE0L,
	0xD3D3E1ABL, 0x21B862A8L, 0x32E8915CL, 0xC083125FL,
	0x144976B4L, 0xE622F5B7L, 0xF5720643L, 0x07198540L,
	0x590AB964L, 0xAB613A67L, 0xB831C993L, 0x4A5A4A90L,
	0x9E902E7BL, 0x6CFBAD78L, 0x7FAB5E8CL, 0x8DC0DD8FL,
	0xE330A81AL, 0x115B2B19L, 0x020BD8EDL, 0xF0605BEEL,
	0x24AA3F05L, 0xD6C1BC06L, 0xC5914FF2L, 0x37FACCF1L,
	0x69E9F0D5L, 0x9B8273D6L, 0x88D28022L, 0x7AB90321L,
	0xAE7367CAL, 0x5C18E4C9L, 0x4F48173DL, 0xBD23943EL,
	0xF36E6F75L, 0x0105EC76L, 0x12551F82L, 0xE03E9C81L,
	0x34F4F86AL, 0xC69F7B69L, 0xD5CF889DL, 0x27A40B9EL,
	0x79B737BAL, 0x8BDCB4B9L, 0x988C474DL, 0x6AE7C44EL,
	0xBE2DA0A5L, 0x4C4623A6L, 0x5F16D052L, 0xAD7D5351L
};

uint32_t crc32c_init(void)
{
	return ~0;
}

/*
 * Steps through buffer one byte at at time, calculates reflected
 * crc using table. This is the portable implementation, and the one
 * every other is checked against.
 */
uint32_t crc32c_update_sw(uint32_t crc,
			  const uint8_t *data,
			  uint32_t length)
{
	while (length--)
		crc = crc32c_table[(crc ^ *data++) & 0xFFL] ^ (crc >> 8);

	return crc;
}

#ifdef CRC32C_HAVE_SSE42

/*
 * SSE4.2 has an instruction for exactly this CRC (CRC32 computes CRC-32C,
 * reflected, on 1 to 8 bytes at a time). One instruction has a latency of
 * three cycles and a throughput of one per cycle, so a long buffer is cut
 * into three streams that are computed together, and the three CRCs are
 * then combined, as the Linux kernel and isa-l do. Combining shifts a CRC
 * over the length of the streams after it: CRC register r followed by n
 * zero bytes is r * x^(8n) modulo the polynomial, a linear map that is
 * applied with four 256-entry tables, one per byte of r.
 *
 * Streams are CRC32C_LONG bytes for buffers of three of those or more (a
 * jumbo UET packet), and CRC32C_SHORT for what remains.
 */
#define CRC32C_POLY_REFLECTED	0x82f63b78u
#define CRC32C_LONG		2048u
#define CRC32C_SHORT		256u

static uint32_t crc32c_long_shift[4][256];
static uint32_t crc32c_short_shift[4][256];
static pthread_once_t crc32c_once = PTHREAD_ONCE_INIT;
static int crc32c_use_sse42;

/* a * b modulo the CRC-32C polynomial, reflected (bit 31 is x^0) */
static uint32_t crc32c_multmodp(uint32_t a, uint32_t b)
{
	uint32_t m = 1u << 31;
	uint32_t p = 0;

	for (;;) {
		if (a & m) {
			p ^= b;
			if ((a & (m - 1)) == 0)
				break;
		}
		m >>= 1;
		b = (b & 1) ? ((b >> 1) ^ CRC32C_POLY_REFLECTED) : (b >> 1);
	}
	return p;
}

/* x^(8 * n) modulo the polynomial */
static uint32_t crc32c_x8nmodp(size_t n)
{
	uint32_t result = 1u << 31;	/* x^0 */
	uint32_t power = 1u << 30;	/* x^1 */
	size_t e = n * 8;

	while (e) {
		if (e & 1)
			result = crc32c_multmodp(power, result);
		power = crc32c_multmodp(power, power);
		e >>= 1;
	}
	return result;
}

static void crc32c_shift_init(uint32_t tab[4][256], size_t n)
{
	uint32_t xn = crc32c_x8nmodp(n);
	unsigned int k, i;

	for (k = 0; k < 4; k++)
		for (i = 0; i < 256; i++)
			tab[k][i] = crc32c_multmodp(xn, (uint32_t)i << (8 * k));
}

static uint32_t crc32c_shift(const uint32_t tab[4][256], uint32_t crc)
{
	return (tab[0][crc & 0xff] ^ tab[1][(crc >> 8) & 0xff] ^
		tab[2][(crc >> 16) & 0xff] ^ tab[3][crc >> 24]);
}

static void crc32c_setup(void)
{
	if (!__builtin_cpu_supports("sse4.2"))
		return;
	crc32c_shift_init(crc32c_long_shift, CRC32C_LONG);
	crc32c_shift_init(crc32c_short_shift, CRC32C_SHORT);
	crc32c_use_sse42 = 1;
}

static inline uint64_t crc32c_load64(const uint8_t *p)
{
	uint64_t v;

	memcpy(&v, p, sizeof(v));
	return v;
}

/* three streams of 'stream' bytes each, combined with 'tab' */
#define CRC32C_3WAY(stream, tab)					\
	while (len >= 3 * (stream)) {					\
		uint64_t c1 = 0, c2 = 0;				\
		const uint8_t *end = p + (stream);			\
									\
		do {							\
			c0 = _mm_crc32_u64(c0, crc32c_load64(p));	\
			c1 = _mm_crc32_u64(c1,				\
				crc32c_load64(p + (stream)));		\
			c2 = _mm_crc32_u64(c2,				\
				crc32c_load64(p + 2 * (stream)));	\
			p += 8;						\
		} while (p < end);					\
		c0 = crc32c_shift(tab, (uint32_t)c0) ^ c1;		\
		c0 = crc32c_shift(tab, (uint32_t)c0) ^ c2;		\
		p += 2 * (stream);					\
		len -= 3 * (stream);					\
	}

__attribute__((target("sse4.2")))
static uint32_t crc32c_update_sse42(uint32_t crc, const uint8_t *p,
				    size_t len)
{
	uint64_t c0 = crc;

	while (len && ((uintptr_t)p & 7)) {
		c0 = _mm_crc32_u8((uint32_t)c0, *p++);
		len--;
	}

	CRC32C_3WAY(CRC32C_LONG, crc32c_long_shift)
	CRC32C_3WAY(CRC32C_SHORT, crc32c_short_shift)

	while (len >= 8) {
		c0 = _mm_crc32_u64(c0, crc32c_load64(p));
		p += 8;
		len -= 8;
	}
	while (len--)
		c0 = _mm_crc32_u8((uint32_t)c0, *p++);

	return (uint32_t)c0;
}

int crc32c_hw(void)
{
	pthread_once(&crc32c_once, crc32c_setup);
	return crc32c_use_sse42;
}

uint32_t crc32c_update(uint32_t crc,
		       const uint8_t *data,
		       uint32_t length)
{
	if (crc32c_hw())
		return crc32c_update_sse42(crc, data, length);
	return crc32c_update_sw(crc, data, length);
}

#else /* !CRC32C_HAVE_SSE42 */

int crc32c_hw(void)
{
	return 0;
}

uint32_t crc32c_update(uint32_t crc,
		       const uint8_t *data,
		       uint32_t length)
{
	return crc32c_update_sw(crc, data, length);
}

#endif /* CRC32C_HAVE_SSE42 */

uint32_t crc32c_finish(uint32_t crc)
{
	return htonl(~crc);
}

uint32_t crc32c(const uint8_t *data,
		uint32_t length)
{
	uint32_t crc = crc32c_init();
	crc = crc32c_update(crc, data, length);
	return crc32c_finish(crc);
}

#if 0

#include <string.h>
#include <stdio.h>

/*
 * Test vectors from RFC 3720:
 * https://www.rfc-editor.org/rfc/rfc3720#appendix-B.4
 */
int main(int argc, char *argv[])
{
#define DATA_LEN 32
	uint8_t data[DATA_LEN];
	int i, j;

	memset((void *)data, 0, sizeof(data));
	printf("crc32c: %08x\n", crc32c(data, DATA_LEN));

	memset((void *)data, 0xFF, DATA_LEN);
	printf("crc32c: %08x\n", crc32c(data, DATA_LEN));

	for (i = 0; i < DATA_LEN; i++)
		data[i] = i;
	printf("crc32c: %08x\n", crc32c(data, DATA_LEN));

	for (i = DATA_LEN - 1, j = 0; i >= 0; i--, j++)
		data[i] = j;
	printf("crc32c: %08x\n", crc32c(data, DATA_LEN));

	return 0;
}
#endif

