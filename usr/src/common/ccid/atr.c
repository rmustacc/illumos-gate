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
 * ATR parsing routines shared between userland (ccidadm) and the kernel (CCID
 * driver)
 */

#include "atr.h"

/* XXX */
#include <stdio.h>

/*

[root@00-0c-29-ca-c7-23 /var/crash/volatile]# /var/tmp/ccidadm atr ccid0/slot0
ATR for ccid0/slot0 (18 bytes)
        0 1 2 3  4 5 6 7  8 9 a b  c d e f  v123456789abcdef
0000:  3bf81300 008131fe 15597562 696b6579  ;.....1..Yubikey
0010:  34d4

[root@00-0c-29-ca-c7-23 /var/crash/volatile]# /var/tmp/ccidadm atr ccid1/slot0
ATR for ccid1/slot0 (22 bytes)
        0 1 2 3  4 5 6 7  8 9 a b  c d e f  v123456789abcdef
0000:  3bfc1800 00813180 45906746 4a006416  ;.....1.E.gFJ.d.
0010:  06f2727e 00e0


 */

/*
 * The ATR must have at least 2 bytes and then may have up to 33 bytes. The
 * first byte is always TS and the second required byte is T0.
 */
#define	ATR_LEN_MIN	2
#define	ATR_LEN_MAX	33
#define	ATR_TS_IDX	0
#define	ATR_T0_IDX	1

/*
 * There are two valid values for TS. It must either be 0x3F or 0x3B. This is
 * required per ISO/IEC 7816-3:2006 section 8.1.
 */
#define	ATR_TS_VAL0	0x3F
#define	ATR_TS_VAL1	0x3B

/*
 * After TS, each word is used t indicate a combination of protocol and the
 * number of bits defined for that protocol. The lower nibble is treated as the
 * protocol. The upper nibble is treated to indicate which of four defined words
 * are present. These are usually referred to as TA, TB, TC, and TD. TD is
 * always used to indicate the next protocol and the number of bytes present for
 * that. T0 works in a similar way, except that it defines the number of
 * historical bytes present in its protocol section and then it refers to a set
 * of pre-defined global bytes that may be present.
 */
#define	ATR_TD_PROT(x)	((x) & 0x0f)
#define	ATR_TD_NBITS(x)	(((x) & 0xf0) >> 4) 
#define	ATR_TA_MASK	0x1
#define	ATR_TB_MASK	0x2
#define	ATR_TC_MASK	0x4
#define	ATR_TD_MASK	0x8

/*
 * When the checksum is required in the ATR, each byte must XOR to zero.
 */
#define	ATR_CKSUM_TARGET	0

const char *
atr_strerror(atr_parsecode_t code)
{
	switch (code) {
	case ATR_CODE_OK:
		return ("ATR parsed successfully");
	case ATR_CODE_TOO_SHORT:
		return ("Specified buffer too short");
	case ATR_CODE_TOO_LONG:
		return ("Specified buffer too long");
	case ATR_CODE_INVALID_TS:
		return ("ATR has invalid TS byte value");
	case ATR_CODE_OVERRUN:
		return ("ATR data requires more bytes than provided");
	case ATR_CODE_UNDERRUN:
		return ("ATR data did not use all provided bytes");
	case ATR_CODE_CHECKSUM_ERROR:
		return ("ATR data did not checksum correctly");
	default:
		return ("Unknown Parse Code");
	}
}

static uint_t
atr_count_cbits(uint8_t x)
{
	uint_t ret = 0;

	if (x & ATR_TA_MASK)
		ret++;
	if (x & ATR_TB_MASK)
		ret++;
	if (x & ATR_TC_MASK)
		ret++;
	if (x & ATR_TD_MASK)
		ret++;
	return (ret);
}

atr_parsecode_t
atr_parse(uint8_t *buf, size_t len)
{
	uint_t nhist, cbits, ncbits, idx;
	boolean_t ncksum = B_FALSE;

	if (len < ATR_LEN_MIN) { 
		return (ATR_CODE_TOO_SHORT);
	}

	if (len > ATR_LEN_MAX) {
		return (ATR_CODE_TOO_LONG);
	}

	if (buf[ATR_TS_IDX] != ATR_TS_VAL0 && buf[ATR_TS_IDX] != ATR_TS_VAL1) {
		return (ATR_CODE_INVALID_TS);
	}

	/*
	 * The protocol of T0 is the number of bits present.
	 */
	nhist = ATR_TD_PROT(buf[ATR_T0_IDX]);
	cbits = ATR_TD_NBITS(buf[ATR_T0_IDX]);
	idx = ATR_T0_IDX + 1;
	ncbits = atr_count_cbits(cbits);
	do {
		/*
		 * Make sure that we have enough space to read all the cbits.
		 * idx points to the first cbit, which could also potentially be
		 * over the length of the buffer. This is why we subtract one
		 * from idx when doing the calculation.
		 */
		if (idx - 1 + ncbits >= len) {
			return (ATR_CODE_OVERRUN);
		}

		if (cbits & ATR_TA_MASK) {
			idx++;
		}

		if (cbits & ATR_TB_MASK) {
			idx++;
		}

		if (cbits & ATR_TC_MASK) {
			idx++;
		}

		if (cbits & ATR_TD_MASK) {
			/* XXX set next protocol */
			cbits = ATR_TD_NBITS(buf[idx]);
			ncbits = atr_count_cbits(cbits);
			if (ATR_TD_PROT(cbits) != 0)
				ncksum = B_TRUE;
			idx++;
		} else {
			ncbits = 0;
			cbits = 0;
		}
	} while (ncbits != 0);

	/*
	 * We've parsed all of the cbits. At this point, we should take into
	 * account all of the historical bits and potentially the checksum.
	 */
	if (idx - 1 + nhist + ncksum >= len) {
		return (ATR_CODE_OVERRUN);
	}

	idx += nhist + ncksum;
	if (idx != len) {
		return (ATR_CODE_UNDERRUN);
	}

	if (ncksum) {
		size_t i;
		uint8_t val;

		/*
		 * Per ISO/IEC 7816-3:2006 Section 8.2.5 the checksum is all
		 * bytes excluding TS. Therefore, we must start at byte 1.
		 */
		for (val = 0, i = 1; i < len; i++) {
			val ^= buf[i];
		}
		if (val != ATR_CKSUM_TARGET) {
			return (ATR_CODE_CHECKSUM_ERROR);
		}
	}

	return (ATR_CODE_OK);
}
