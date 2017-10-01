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
#include <sys/debug.h>
#include <sys/limits.h>

#ifdef	_KERNEL
#include <sys/inttypes.h>
#include <sys/sunddi.h>
#include <sys/kmem.h>
#else
#include <inttypes.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#endif

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
#define	ATR_TS_INVERSE	0x3F
#define	ATR_TS_DIRECT	0x3B

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

/*
 * Maximum number of historic ATR bytes. This is limited by the fact that it's a
 * 4-bit nibble.
 */
#define	ATR_HISTORICAL_MAX	15

/*
 * The maximum number of TA, TB, TC, and TD levels that can be encountered in a
 * given structure. In the best case, there are 30 bytes available (TS, T0, and
 * TCK use the others). Given that each one of these needs 4 bytes to be
 * represented, the maximum number of layers that can fit is seven.
 */
#define	ATR_TI_MAX	7

/*
 * Defined protocol values. See ISO/IEC 7816-3:2006 8.2.3 for this list.
 * Reserved values are noted but not defined.
 */
#define	ATR_PROTOCOL_T0		0
#define	ATR_PROTOCOL_T1		1
/*
 * T=2 and T=3 are reserved for future full-duplex operation.
 * T=4 is reserved for enhacned half-duplex character transmission.
 * T=5-13 are reserved for future use by ISO/IEC JTC 1/SC 17.
 * T=14 is for protocols not standardized by ISO/IEC JTC 1/SC 17.
 */
#define	ATR_PROTOCOL_T15	15

/*
 * This enum and subsequent structure is used to represent a single level of
 * 'T'. This includes the possibility for all three values to be set and records
 * the protocol.
 */
typedef enum atr_ti_flags {
	ATR_TI_HAVE_TA	= 1 << 0,
	ATR_TI_HAVE_TB	= 1 << 1,
	ATR_TI_HAVE_TC	= 1 << 2
} atr_ti_flags_t;

typedef struct atr_ti {
	uint8_t		atrti_protocol;
	uint8_t		atrti_ti_val;
	atr_ti_flags_t	atrti_flags;
	uint8_t		atrti_ta;
	uint8_t		atrti_tb;
	uint8_t		atrti_tc;
} atr_ti_t;

typedef enum atr_flags {
	ATR_F_USES_DIRECT	= 1 << 0,
	ATR_F_USES_INVERSE	= 1 << 1,
	ATR_F_HAS_CHECKSUM	= 1 << 2,
	ATR_F_VALID		= 1 << 3,
} atr_flags_t;


struct atr_data {
	atr_flags_t	atr_flags;
	uint8_t		atr_nti;
	atr_ti_t	atr_ti[ATR_TI_MAX];
	uint8_t		atr_nhistoric;
	uint8_t		atr_historic[ATR_HISTORICAL_MAX];
	uint8_t 	atr_cksum;
};

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

/*
 * Parse out ATR values. Focus on only parsing it and not interpretting it.
 * Interpretation should be done in other functions that can walk over the data
 * and be more protocol-aware.
 */
atr_parsecode_t
atr_parse(const uint8_t *buf, size_t len, atr_data_t *data)
{
	uint_t nhist, cbits, ncbits, idx, Ti, prot;
	boolean_t ncksum = B_FALSE;
	atr_ti_t *atp;

	/*
	 * Zero out data in case someone's come back around for another loop on
	 * the same data.
	 */
	bzero(data, sizeof (atr_data_t));

	if (len < ATR_LEN_MIN) { 
		return (ATR_CODE_TOO_SHORT);
	}

	if (len > ATR_LEN_MAX) {
		return (ATR_CODE_TOO_LONG);
	}

	if (buf[ATR_TS_IDX] != ATR_TS_INVERSE &&
	    buf[ATR_TS_IDX] != ATR_TS_DIRECT) {
		return (ATR_CODE_INVALID_TS);
	}

	if (buf[ATR_TS_IDX] == ATR_TS_DIRECT) {
		data->atr_flags |= ATR_F_USES_DIRECT;
	} else {
		data->atr_flags |= ATR_F_USES_INVERSE;
	}

	/*
	 * The protocol of T0 is the number of historical bits present.
	 */
	nhist = ATR_TD_PROT(buf[ATR_T0_IDX]);
	cbits = ATR_TD_NBITS(buf[ATR_T0_IDX]);
	idx = ATR_T0_IDX + 1;
	ncbits = atr_count_cbits(cbits);

	/*
	 * Ti is used to track the current iteration of T[A,B,C,D] that we are
	 * on, as the ISO/IEC standard suggests. The way that values are
	 * interpretted depends on the value of Ti.
	 * 
	 * When Ti is one, TA, TB, and TC represent global properties. TD's protocol
	 * represents the preferred protocol.
	 *
	 * When Ti is two TA, TB, and TC also represent global properties.
	 * However, TC only has meaning if the protocol is T=0.
	 *
	 * When Ti is 15, it indicates more global properties.
	 *
	 * For all other values of Ti, the meaning depends on the protocol in
	 * question and they are all properties specific to that protocol.
	 */
	Ti = 1;
	/*
	 * Initialize prot to an invalid protocol to help us deal with the
	 * normal workflow and make sure that we don't mistakenly do anything.
	 */
	prot = UINT32_MAX;
	do {
		atp = &data->atr_ti[data->atr_nti];
		data->atr_nti++;
		ASSERT3U(data->atr_nti, <=, ATR_TI_MAX);

		/*
		 * Make sure that we have enough space to read all the cbits.
		 * idx points to the first cbit, which could also potentially be
		 * over the length of the buffer. This is why we subtract one
		 * from idx when doing the calculation.
		 */
		if (idx - 1 + ncbits >= len) {
			return (ATR_CODE_OVERRUN);
		}

		ASSERT3U(Ti, !=, 0);

		/*
		 * At the moment we opt to ignore reserved protocols.
		 */
		atp->atrti_protocol = prot;
		atp->atrti_ti_val = Ti;

		if (cbits & ATR_TA_MASK) {
			atp->atrti_flags |= ATR_TI_HAVE_TA;
			atp->atrti_ta = buf[idx];
			idx++;
		}

		if (cbits & ATR_TB_MASK) {
			atp->atrti_flags |= ATR_TI_HAVE_TB;
			atp->atrti_tb = buf[idx];
			idx++;
		}

		if (cbits & ATR_TC_MASK) {
			atp->atrti_flags |= ATR_TI_HAVE_TC;
			atp->atrti_tc = buf[idx];
			idx++;
		}

		if (cbits & ATR_TD_MASK) {
			cbits = ATR_TD_NBITS(buf[idx]);
			prot = ATR_TD_PROT(buf[idx]);
			ncbits = atr_count_cbits(cbits);
			if (prot != 0)
				ncksum = B_TRUE;
			idx++;
			/*
			 * Encountering TD means that once we take the next loop
			 * and we need to increment Ti.
			 */
			Ti++;
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

	if (idx + nhist + ncksum != len) {
		return (ATR_CODE_UNDERRUN);
	}

	if (nhist > 0) {
		data->atr_nhistoric = nhist;
		bcopy(&buf[idx], data->atr_historic, nhist);
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
		data->atr_flags |= ATR_F_HAS_CHECKSUM;
		data->atr_cksum = buf[len - 1];
	}

	data->atr_flags |= ATR_F_VALID;
	return (ATR_CODE_OK);
}

/*
 * Parse the data to determine which protocols are supported in this atr data.
 * Based on this, users can come and ask us to fill in protocol information.
 */
atr_protocol_t
atr_supported_protocols(atr_data_t *data)
{
	uint_t i;
	atr_protocol_t prot;

	if ((data->atr_flags & ATR_F_VALID) == 0)
		return (ATR_P_NONE);

	/*
	 * Based on 8.2.3 of ISO/IEC 7816-3:2006, if TD1 is present, then that
	 * indicates the first protocol. However, if it is not present, then
	 * that implies that T=0 is the only supported protocol. Otherwise, all
	 * protocols are referenced in ascending order. The first entry in
	 * atr_ti refers to data from T0, so the protocol in the second entry
	 * would have the TD1 data.
	 */
	if (data->atr_nti < 2) {
		return (ATR_P_T0);
	}

	prot = ATR_P_NONE;
	for (i = 0; i < data->atr_nti; i++) {
		switch (data->atr_ti[i].atrti_protocol) {
		case ATR_PROTOCOL_T0:
			prot |= ATR_PROTOCOL_T0;
			break;
		case ATR_PROTOCOL_T1:
			prot |= ATR_PROTOCOL_T1;
			break;
		default:
			continue;
		}
	}

	return (prot);
}

#ifdef	_KERNEL
atr_data_t *
atr_data_alloc(void)
{
	return (kmem_zalloc(sizeof (atr_data_t), KM_SLEEP));
}

void
atr_data_free(atr_data_t *data)
{
	if (data == NULL)
		return;

	kmem_free(data, sizeof (atr_data_t));
}

#else	/* !_KERNEL */
atr_data_t *
atr_data_alloc(void)
{
	return (calloc(sizeof (atr_data_t), 1));
}

void
atr_data_free(atr_data_t *data)
{
	if (data == NULL)
		return;
	free(data);
}

void
atr_data_dump(atr_data_t *data, FILE *out)
{
	if ((data->atr_flags & ATR_F_VALID) == 0)
		return;


}
#endif	/* _KERNEL */
