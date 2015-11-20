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
 * Simulate a classic kernel subsystem doing a traditional settimeout() in a
 * loop in one thread and another coming around and canceling it. We then wait
 * enough time to verify that it hasn't fired again.
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


static mutex_t resched_lock = ERRORCHECKMUTEX;
static cond_t resched_cond = DEFAULTCV;
static periodic_id_t resched_id;
static int resched_count;
static boolean_t resched_cancel;
static periodic_handle_t *resched_ph;

#define	RESCHED_INTERVAL	MSEC2NSEC(10)
#define	RESCHED_MAX	10
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
static void *
resched_watchdog(void *arg)
{
	int ret, count;
	struct timespec ts;

	mutex_enter(&resched_lock);
	while (resched_count < RESCHED_MAX)
		(void) cond_wait(&resched_cond, &resched_lock);
	resched_cancel = B_TRUE;
	mutex_exit(&resched_lock);

	ret = periodic_cancel(resched_ph, resched_id);
	if (ret == -1) {
		VERIFY3S(errno, ==, ENOENT);
	}

	/*
	 * We want to verify that the periodic is now empty and no more events
	 * will fire. There's no great way to do this. So let's just sleep for
	 * 10 normal timer intervals and verify that we don't increase the
	 * resched_count. Note, depending on timing, we may have gotten one more
	 * tick in than we originally anticipated because it changed the
	 * resched_id out from under us, but it will cancel. To allow for this,
	 * count may also be resched_count - 1.
	 */
	mutex_enter(&resched_lock);
	count = resched_count;
	mutex_exit(&resched_lock);

	ts.tv_sec = 0;
	ts.tv_nsec = 10 * RESCHED_INTERVAL;
	(void) nanosleep(&ts, NULL);

	mutex_enter(&resched_lock);
	if (count != resched_count && count + 1 != resched_count) {
		fprintf(stderr, "resched_count is off, something must have "
		    "fired after the fact: expected %d, got %d\n", count,
		    resched_count);
		exit(1);
	}
	mutex_exit(&resched_lock);

	exit(0);
}

/* ARGSUSED */
static void
resched_tick(void *arg)
{
	mutex_enter(&resched_lock);
	resched_count++;

	if (resched_count == RESCHED_MAX)
		cond_signal(&resched_cond);

	if (resched_cancel == B_FALSE) {
		if (periodic_schedule(resched_ph, RESCHED_INTERVAL,
		    PERIODIC_ONESHOT, resched_tick, NULL, &resched_id) != 0) {
			fprintf(stderr, "failed to schedule periodic: %s\n",
			    strerror(errno));
			exit(1);
		}
	}

	mutex_exit(&resched_lock);
}

int
main(void)
{
	int port, ret, loopcount;
	port_event_t pe;
	thread_t thr;

	port = port_create();
	if ((port = port_create()) < 0) {
		fprintf(stderr, "failed to create event port: %s\n",
		    strerror(errno));
		exit(1);
	}

	resched_ph = periodic_init(port, NULL, CLOCK_MONOTONIC);
	if (resched_ph == NULL) {
		fprintf(stderr, "failed to create periodic handle: %s\n",
		    strerror(errno));
		exit(1);
	}

	if ((ret = thr_create(NULL, 0, resched_watchdog, NULL, 0, &thr)) != 0) {
		fprintf(stderr, "failed to create watchdog therad: %s\n",
		    strerror(errno));
		exit(1);
	}

	if (periodic_schedule(resched_ph, RESCHED_INTERVAL,
	    PERIODIC_ONESHOT, resched_tick, NULL, &resched_id) != 0) {
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

		periodic_fire(resched_ph);
	}

	/* The oneshot should fire there and we should exit */
	fprintf(stderr, "reschedule loop count exceeded");
	exit(1);
}
