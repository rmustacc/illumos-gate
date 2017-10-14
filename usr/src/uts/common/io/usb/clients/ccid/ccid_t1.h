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

#ifndef _T1_H
#define	_T1_H

/*
 * Definitions for the T=1 protocol.
 */

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(1)
typedef struct t1_hdr {
	uint8_t		t1h_nad;
	uint8_t		t1h_pcb;
	uint8_t		t1h_len;
	uint8_t		t1h_data[];
} t1_hdr_t;
#pragma pack()

/*
 * Per ISO/IEC 7816-3:2006 11.3.1 the maximum amount of data that we can put in
 * the len member of structure is 254 bytes. The value 255 is reserved for
 * future use.
 */
#define	T1_SIZE_MAX	254

/*
 * These macros are used to determine what type the data is. An I-Block has the
 * msb set to zero; however, the other types use two bits to determine what the
 * type is.
 */
#define	T1_TYPE_IBLOCK	0x00
#define	T1_TYPE_RBLOCK	0x80
#define	T1_TYPE_SBLOCK	0xc0

#define	T1_IBLOCK_NS	0x40
#define	T1_IBLOCK_M	0x20

/*
 * The T1 NS sequence must always start at 0 per ISO/IEC 7816-3:2006 11.6.2.1.
 * This is a one bit counter. To increment it we always do an xor with 1.
 */
#define	T1_IBLOCK_NS_DEFVAL	0

#define	T1_RBLOCK_NR	0x10
#define	T1_RBLOCK_STATUS_MASK	0x0f

typedef enum t1_rblock_status {
	T1_RBLOCK_STATUS_OK 	= 0x00,
	T1_RBLOCK_STATUS_PARITY	= 0x01,
	T1_RBLOCK_STATUS_ERROR	= 0x02
} t1_rblock_status_t;

#define	T1_SBLOCK_OP_MASK	0x3f

typedef enum t1_sblock_op {
	T1_SBLOCK_REQ_RESYNCH	= 0x00,
	T1_SBLOCK_RESP_RSYNCH	= 0x20,
	T1_SBLOCK_REQ_IFS	= 0x01,
	T1_SBLOCK_RESP_IFS	= 0x21,
	T1_SBLOCK_REQ_ABORT	= 0x02,
	T1_SBLOCK_RESP_ABORT	= 0x22,
	T1_SBLOCK_REQ_WTX	= 0x03,
	T1_SBLOCK_RESP_WTX	= 0x23
} t1_sblock_op_t;

#ifdef __cplusplus
}
#endif

#endif /* _T1_H */
