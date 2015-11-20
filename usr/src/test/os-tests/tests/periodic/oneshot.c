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

/*
 * Create a simple one-shot timer and make sure that it fires.
 */

#include <stdio.h>
#include <port.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include <libperiodic.h>

static hrtime_t oneshot_start;

const char *
_umem_debug_init()
{
	return ("default,verbose");
}

const char *
_umem_logging_init(void)
{
	return ("fail,contents");
}

/* ARGSUSED */
static void
oneshot_fire(void *arg)
{
	hrtime_t end = gethrtime();

	if (end - oneshot_start < MSEC2NSEC(10)) {
		fprintf(stderr, "timer fired, but didn't elapse 10ms, "
		    "start: %llx, end: %llx\n", oneshot_start, end);
		exit(1);
	}
	exit(0);
}

int
main(void)
{
	int port;
	periodic_handle_t *ph;
	periodic_id_t phid;
	port_event_t pe;

	port = port_create();
	if ((port = port_create()) < 0) {
		fprintf(stderr, "failed to create event port: %s\n",
		    strerror(errno));
		exit(1);
	}

	ph = periodic_init(port, NULL, CLOCK_MONOTONIC);
	if (ph == NULL) {
		fprintf(stderr, "failed to create periodic handle: %s\n",
		    strerror(errno));
		exit(1);
	}

	oneshot_start = gethrtime();
	if (periodic_schedule(ph, MSEC2NSEC(10), PERIODIC_ONESHOT,
	    oneshot_fire, NULL, &phid) != 0) {
		fprintf(stderr, "failed to schedule periodic: %s\n",
		    strerror(errno));
		exit(1);
	}

	if (port_get(port, &pe, NULL) != 0) {
		fprintf(stderr, "failed to port_get: %s\n",
		    strerror(errno));
		exit(1);
	}

	periodic_fire(ph);

	/* The oneshot should fire there and we should exit */
	exit(1);
}
