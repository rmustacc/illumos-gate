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

#ifndef _CCID_PARITY_H
#define	_CCID_PARITY_H

/*
 * Parity routines required by the CCID and ISO specifcations.
 */

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t ccid_parity_lrc(const uint8_t *, size_t);
extern uint16_t ccid_parity_crc(const uint8_t *, size_t);

#ifdef __cplusplus
}
#endif

#endif /* _CCID_PARITY_H */
