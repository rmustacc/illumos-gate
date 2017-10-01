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

#ifndef _ATR_H
#define	_ATR_H

/*
 * Parse Anser-To-Reset values. This header file is private to illumos and
 * should not be shipped or used by applications.
 *
 * This is based on ISO/IEC 7816-3:2006. It has been designed such that if newer
 * revisions come out that define reserved values, they will be ignored until
 * this code is updated.
 */

#include <sys/types.h>
#ifndef	_KERNEL
#include <stdio.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum atr_parsecode {
	ATR_CODE_OK	= 0,
	ATR_CODE_TOO_SHORT,
	ATR_CODE_TOO_LONG,
	ATR_CODE_INVALID_TS,
	ATR_CODE_OVERRUN,
	ATR_CODE_UNDERRUN,
	ATR_CODE_CHECKSUM_ERROR,
} atr_parsecode_t;

typedef enum atr_protocol {
	ATR_P_NONE	= 0,
	ATR_P_T0	= 1 << 0,
	ATR_P_T1	= 1 << 1
} atr_protocol_t;

typedef struct atr_data atr_data_t;

/*
 * Allocate and free ATR data.
 */
extern atr_data_t *atr_data_alloc(void);
extern void atr_data_free(atr_data_t *);

/*
 * Parse the ATR data into an opaque structure that organizes the data and
 * allows for various queries to be made on it later.
 */
extern atr_parsecode_t atr_parse(const uint8_t *, size_t, atr_data_t *data);
extern const char *atr_strerror(atr_parsecode_t);

/*
 * Get an eumeration of supported protocols in this ATR data. Note that if a
 * reserved protocol is encountered, we may not report it as we don't know of it
 * at this time.
 */
extern atr_protocol_t atr_supported_protocols(atr_data_t *);

#ifndef	_KERNEL
extern void atr_dump(atr_data_t *, FILE *);
#endif

#ifdef __cplusplus
}
#endif

#endif /* _ATR_H */
