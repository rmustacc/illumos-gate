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
 * Test that we properly are honoring the interval. Meaning that even if we end
 * up being slower the first time, that we end up catching up and firing it
 * right away the next. We use slightly longer times to try and avoid scheduling
 * pathologies.
 */

#include <stdio.h>
#include <port.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/debug.h>

#include <libperiodic.h>

#define	IVAL_PERIOD	MSEC2NSEC(100)
#define	IVAL_STALL	MSEC2NSEC(500)
#define	TRY_MAX		50

static int ival_count;
static hrtime_t ival_time;

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


static void
ival_fire(void *arg)
{
	hrtime_t now, diff;

	if (ival_count == 0) {
		struct timespec tv;
		tv.tv_sec = 0;
		tv.tv_nsec = IVAL_STALL;
		(void) nanosleep(&tv, NULL);
		ival_count++;
		ival_time = gethrtime();
		return;
	}

	now = gethrtime();
	diff = now - ival_time;
	VERIFY3S(diff, <, IVAL_PERIOD);
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

	if (periodic_schedule(ph, IVAL_PERIOD, 0, ival_fire, NULL,
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
	fprintf(stderr, "interval logic did not properly fire");
	exit(1);
}
