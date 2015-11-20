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
 * Create a periodic and a one shot. Have the one shot be a multiple of the
 * periodic and verify that the periodic will fire a few times before the
 * one shot and then that the one shot fires again afterwards. eg. we want an
 * A | A | B | A  pattern.
 */

#include <stdio.h>
#include <port.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <thread.h>
#include <synch.h>
#include <sys/debug.h>

#include <libperiodic.h>

static mutex_t ileave_lock = ERRORCHECKMUTEX;
static int ileave_pcount;
static int ileave_ocount;

#define	ILEAVE_PERIOD	MSEC2NSEC(45)
#define	ILEAVE_ONESHOT	MSEC2NSEC(100)
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


/*
 * Oneshot callback
 */
static void
ileave_ofire(void *arg)
{
	mutex_enter(&ileave_lock);
	VERIFY3S(ileave_pcount, >=, 2);
	VERIFY3S(ileave_ocount, ==, 0);
	ileave_ocount++;
	mutex_exit(&ileave_lock);
}

/*
 * Periodic callback
 */
static void
ileave_pfire(void *arg)
{
	mutex_enter(&ileave_lock);
	ileave_pcount++;
	if (ileave_ocount > 0) {
		VERIFY3S(ileave_pcount, >, 1);
		exit(0);
	}
	mutex_exit(&ileave_lock);
}

int
main(void)
{
	int port, loopcount;
	periodic_handle_t *ph;
	periodic_id_t phid, ohid;
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

	if (periodic_schedule(ph, ILEAVE_PERIOD, 0, ileave_pfire, NULL,
	    &phid) != 0) {
		fprintf(stderr, "failed to schedule periodic: %s\n",
		    strerror(errno));
		exit(1);
	}

	if (periodic_schedule(ph, ILEAVE_ONESHOT, PERIODIC_ONESHOT,
	    ileave_ofire, NULL, &ohid) != 0) {
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
	fprintf(stderr, "interleaving did not occur");
	exit(1);
}
