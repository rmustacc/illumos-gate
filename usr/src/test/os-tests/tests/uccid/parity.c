/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright (c) 2017, Joyent, Inc.
 */

/*
 * Test the two CCID parity functions.
 */

#include <ccid_parity.h>
#include <sys/debug.h>

#include <stdio.h>

int
main(void)
{
	uint8_t v;
	uint8_t lrc1[] = { 0x01, 0x02, 0x04, 0x08 };
	uint8_t lrc2[] = { 0x11, 0x22, 0x44, 0x88 };
	uint8_t crc0[] = { 0x00 };
	uint8_t crc1[] = { 0x12, 0x34 };

	v = ccid_parity_lrc(lrc1, sizeof (lrc1) / sizeof (uint8_t));
	VERIFY3U(v, ==, 0x0f);
	v = ccid_parity_lrc(lrc2, sizeof (lrc2) / sizeof (uint8_t));
	VERIFY3U(v, ==, 0xff);

	v = ccid_parity_crc(crc0, sizeof (crc0) / sizeof (uint8_t));
	printf("0x%x\n", v);
	v = ccid_parity_crc(crc1, sizeof (crc1) / sizeof (uint8_t));
	printf("0x%x\n", v);

	return (0);
}
