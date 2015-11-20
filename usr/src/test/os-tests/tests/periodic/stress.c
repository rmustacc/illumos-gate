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
 * Generate a lot of periodic timer activity. Then clean everything up. Note
 * that we'll end up cleaning everything up and allow ourselve to be cleaned up.
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

#define	STRESS_NPERIODIC	20
#define	STRESS_NONESHOT		20

#define	STRESS_TIMER_MAX	20
#define	STRESS_TIMEOUT		NANOSEC

static periodic_handle_t *stress_ph;
static periodic_id_t stress_pids[STRESS_NPERIODIC];
static periodic_id_t stress_oids[STRESS_NPERIODIC];

static mutex_t stress_lock = ERRORCHECKMUTEX;
static boolean_t stress_over;

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


static hrtime_t
stress_getto(void)
{
	hrtime_t r;
	r = arc4random_uniform(STRESS_TIMER_MAX) + 1;
	r = MSEC2NSEC(r);
	return (r);
}

static void
stress_periodic_fire(void *arg)
{
}

static void
stress_teardown_fire(void *arg)
{
	mutex_enter(&stress_lock);
	stress_over = B_TRUE;
	mutex_exit(&stress_lock);
}

static void
stress_oneshot_fire(void *arg)
{
	int slot = (uintptr_t)arg;
	hrtime_t next;

	next = stress_getto();

	mutex_enter(&stress_lock);
	if (stress_over == B_TRUE) {
		mutex_exit(&stress_lock);
		return;
	}

	if (periodic_schedule(stress_ph, next, PERIODIC_ONESHOT,
	    stress_oneshot_fire, arg, &stress_oids[slot]) != 0) {
		fprintf(stderr, "failed to schedule periodic: %s\n",
		    strerror(errno));
		exit(1);
	}
	mutex_exit(&stress_lock);
}


/*
 * This function exists for DTrace and leak detection logic which is used
 * outside of the test runner generally. The function must be weak otherwise the
 * compiler can optimize out the call to the nop. Do not remove this or the call
 * after everything has been cleaned up.
 */
#pragma weak stress_leakdetect
void
stress_leakdetect(void)
{
}

int
main(void)
{
	int port, i;
	port_event_t pe;
	periodic_id_t id;

	port = port_create();
	if ((port = port_create()) < 0) {
		fprintf(stderr, "failed to create event port: %s\n",
		    strerror(errno));
		exit(1);
	}

	stress_ph = periodic_init(port, NULL, CLOCK_MONOTONIC);
	if (stress_ph == NULL) {
		fprintf(stderr, "failed to create periodic handle: %s\n",
		    strerror(errno));
		exit(1);
	}

	for (i = 0; i < STRESS_NONESHOT; i++) {
		hrtime_t next = stress_getto();

		if (periodic_schedule(stress_ph, next, PERIODIC_ONESHOT,
		    stress_oneshot_fire, (void *)(uintptr_t)i,
		    &stress_oids[i]) != 0) {
			fprintf(stderr, "failed to schedule periodic: %s\n",
			    strerror(errno));
			exit(1);
		}
	}

	for (i = 0; i < STRESS_NPERIODIC; i++) {
		hrtime_t next = stress_getto();

		if (periodic_schedule(stress_ph, next, 0, stress_periodic_fire,
		    (void *)(uintptr_t)i, &stress_pids[i]) != 0) {
			fprintf(stderr, "failed to schedule periodic: %s\n",
			    strerror(errno));
			exit(1);
		}
	}

	if (periodic_schedule(stress_ph, STRESS_TIMEOUT, PERIODIC_ONESHOT,
	    stress_teardown_fire, NULL, &id) != 0) {
		fprintf(stderr, "failed to schedule periodic: %s\n",
		    strerror(errno));
		exit(1);
	}

	for (;;) {
		/*
		 * The use of the loop counter here is a simple heuristic to try
		 * to make sure that we don't end up infinitely looping and
		 * broken.
		 */
		if (port_get(port, &pe, NULL) != 0) {
			fprintf(stderr, "failed to port_get: %s\n",
			    strerror(errno));
			exit(1);
		}

		periodic_fire(stress_ph);
		mutex_enter(&stress_lock);
		if (stress_over == B_TRUE) {
			mutex_exit(&stress_lock);
			break;
		}
		mutex_exit(&stress_lock);
	}

	for (i = 0; i < STRESS_NPERIODIC; i++) {
		(void) periodic_cancel(stress_ph, stress_pids[i]);
	}

	for (i = 0; i < STRESS_NONESHOT; i++) {
		(void) periodic_cancel(stress_ph, stress_oids[i]);
	}

	periodic_fini(stress_ph);
	stress_leakdetect();
	exit(0);
}
