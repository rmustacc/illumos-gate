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
 * Verify that if we remove an entry before it fires that we never end up having
 * something fire ourselves.
 */
#include <stdio.h>
#include <port.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/debug.h>

#include <libperiodic.h>


#define	CANCEL_PERIOD	MSEC2NSEC(100)

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
cancel_fire(void *arg)
{
	fprintf(stderr, "cancel_fire fired when it should never have\n");
	exit(1);
}

int
main(void)
{
	int port, ret;
	periodic_handle_t *ph;
	periodic_id_t phid;
	port_event_t pe;
	struct timespec ts;

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

	if (periodic_schedule(ph, CANCEL_PERIOD, 0, cancel_fire, NULL,
	    &phid) != 0) {
		fprintf(stderr, "failed to schedule periodic: %s\n",
		    strerror(errno));
		exit(1);
	}
	ret = periodic_cancel(ph, phid);
	VERIFY3S(ret, ==, 0);

	/*
	 * Depending on everything that happens, we may not be fast enough to
	 * cancel the timer. However, there should be nothing which runs or
	 * fires and taking another loop should verify that we get a timeout.
	 */
	for (;;) {
		ts.tv_sec = 0;
		ts.tv_nsec = CANCEL_PERIOD * 5;
		if (port_get(port, &pe, &ts) == 0) {
			periodic_fire(ph);
			continue;
		}
		VERIFY3S(errno, ==, ETIME);
		break;
	}

	exit(0);
}
