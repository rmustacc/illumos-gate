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
 * Create a simple periodic timer and make sure that it fires.
 */

#include <stdio.h>
#include <port.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include <libperiodic.h>

static hrtime_t period_start;
static int period_count;

#define	PERIOD_MAX	10
#define	TRY_MAX		100

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
period_fire(void *arg)
{
	hrtime_t end = gethrtime();

	period_count++;
	if (end - period_start < MSEC2NSEC(10) * period_count) {
		fprintf(stderr, "timer fired, but didn't elapse 10ms, "
		    "start: %lx, end: %lx\n", period_start, end);
		exit(1);
	}
	if (period_count == PERIOD_MAX)
		exit(0);
}

int
main(void)
{
	int port, loopcount;
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

	period_start = gethrtime();
	if (periodic_schedule(ph, MSEC2NSEC(10), 0, period_fire, NULL,
	    &phid) != 0) {
		fprintf(stderr, "failed to schedule periodic: %s\n",
		    strerror(errno));
		exit(1);
	}

	loopcount = 0;
	for (;;) {
		/*
		 * The use of the loop counter here is a simple heuristic to try
		 * to make sure that we don't end up infinitely looping and
		 * broken.
		 */
		loopcount++;
		if (loopcount > TRY_MAX)
			break;

		if (port_get(port, &pe, NULL) != 0) {
			fprintf(stderr, "failed to port_get: %s\n",
			    strerror(errno));
			exit(1);
		}

		periodic_fire(ph);


	}

	/* The oneshot should fire there and we should exit */
	fprintf(stderr, "periodic timer did not fire sufficiently");
	exit(1);
}
