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

typedef enum atr_convention {
	ATR_CONVENTION_DIRECT 	= 0x00,
	ATR_CONVENTION_INVERSE 	= 0x01
} atr_convention_t;

typedef enum atr_clock_stop {
	ATR_CLOCK_STOP_NONE	= 0x00,
	ATR_CLOCK_STOP_LOW	= 0x01,
	ATR_CLOCK_STOP_HI	= 0x02,
	ATR_CLOCK_STOP_BOTH	= 0x03
} atr_clock_stop_t;

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

/*
 * Based on the ATR determine what the default protocol is and whether or not it
 * supports negotiation. When a ICC is not negotiable, it will always start up
 * with a sepcific protocol and parameters based on the ATR and be ready to use.
 * Otherwise, the card will be in a negotiable mode and be set to a default set
 * of parameters.
 */
extern boolean_t atr_params_negotiable(atr_data_t *);
extern atr_protocol_t atr_default_protocol(atr_data_t *);

/*
 * Obtain the table indexes that should be used by the device.
 */
extern uint8_t atr_fi_index(atr_data_t *);
extern uint8_t atr_di_index(atr_data_t *);
extern atr_convention_t atr_convention(atr_data_t *);
extern uint8_t atr_extra_guardtime(atr_data_t *);
extern uint8_t atr_t0_wi(atr_data_t *);
extern uint8_t atr_t1_bwi(atr_data_t *);
extern uint8_t atr_t1_cwi(atr_data_t *);
extern atr_clock_stop_t atr_clock_stop(atr_data_t *);
extern uint8_t atr_t1_ifsc(atr_data_t *);

#ifndef	_KERNEL
extern void atr_data_dump(atr_data_t *, FILE *);
#endif

/*
 * Get a string for an ATR protocol.
 */
extern const char *atr_protocol_to_string(atr_protocol_t);

extern const char *atr_fi_index_to_string(uint8_t);
extern const char *atr_fmax_index_to_string(uint8_t);
extern const char *atr_di_index_to_string(uint8_t);
extern const char *atr_clock_stop_to_string(atr_clock_stop_t);
extern const char *atr_convention_to_string(atr_convention_t);

#ifdef __cplusplus
}
#endif

#endif /* _ATR_H */
