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
 * Copyright 2015 Joyent, Inc.
 */

#ifndef _LIBPERIODIC_H
#define	_LIBPERIODIC_H

/*
 * This library provides timer infrastructure designed to be a part of an event
 * loop based arond event ports. It manages timer expirations and can be used to
 * maintain a large tree of such events.
 */

#include <sys/types.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef id_t periodic_id_t;
typedef struct periodic_handle periodic_handle_t;
typedef void (periodic_func_t)(void *);

#define	PERIODIC_INVALID_ID	-1

extern periodic_handle_t *periodic_init(int, void *, clockid_t);
extern void periodic_fire(periodic_handle_t *);
extern void periodic_fini(periodic_handle_t *);

#define	PERIODIC_ONESHOT	0x01
#define	PERIODIC_ABSOLUTE	0x02
extern int periodic_schedule(periodic_handle_t *, hrtime_t, int,
    periodic_func_t *, void *, periodic_id_t *);
extern int periodic_cancel(periodic_handle_t *, periodic_id_t);


#ifdef __cplusplus
}
#endif

#endif /* _LIBPERIODIC_H */
