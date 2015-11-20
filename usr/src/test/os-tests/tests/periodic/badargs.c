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
 * Test various invalid scenarios.
 */

#include <errno.h>
#include <port.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <priv.h>
#include <sys/debug.h>
#include <libperiodic.h>

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


/*ARGSUSED*/
static void
badarg_callback(void *arg)
{
	exit(1);
}

int
main(void)
{
	int port, ret;
	periodic_handle_t *ph;
	periodic_id_t id;
	priv_set_t *priv;

	port = port_create();
	if ((port = port_create()) < 0) {
		fprintf(stderr, "failed to create event port: %s\n",
		    strerror(errno));
		exit(1);
	}

	ph = periodic_init(-1, NULL, CLOCK_MONOTONIC);
	VERIFY3P(ph, ==, NULL);
	VERIFY3S(errno, ==, EBADF);

	ph = periodic_init(port, NULL, -1);
	VERIFY3P(ph, ==, NULL);
	VERIFY3S(errno, ==, EINVAL);

	ph = periodic_init(port, NULL, CLOCK_MONOTONIC);
	if (ph == NULL) {
		fprintf(stderr, "failed to create periodic handle: %s\n",
		    strerror(errno));
		exit(1);
	}

	/* Garbage cancel values */
	ret = periodic_cancel(ph, -1);
	VERIFY3S(ret, ==, -1);
	VERIFY3S(errno, ==, ENOENT);

	ret = periodic_cancel(ph, 42);
	VERIFY3S(ret, ==, -1);
	VERIFY3S(errno, ==, ENOENT);

	ret = periodic_cancel(ph, INT32_MAX);
	VERIFY3S(ret, ==, -1);
	VERIFY3S(errno, ==, ENOENT);

	ret = periodic_cancel(ph, INT32_MIN);
	VERIFY3S(ret, ==, -1);
	VERIFY3S(errno, ==, ENOENT);

	/* Various garbage values for schedule */
	ret = periodic_schedule(ph, -1, 0, badarg_callback, NULL, &id);
	VERIFY3S(ret, ==, -1);
	VERIFY3S(errno, ==, ERANGE);

	ret = periodic_schedule(ph, MSEC2NSEC(10), 0, NULL, NULL, &id);
	VERIFY3S(ret, ==, -1);
	VERIFY3S(errno, ==, EINVAL);

	ret = periodic_schedule(ph, MSEC2NSEC(10), ~PERIODIC_ONESHOT,
	    badarg_callback, NULL, &id);
	VERIFY3S(ret, ==, -1);
	VERIFY3S(errno, ==, EINVAL);

	ret = periodic_schedule(ph, MSEC2NSEC(10), PERIODIC_ABSOLUTE,
	    badarg_callback, NULL, &id);
	VERIFY3S(ret, ==, -1);
	VERIFY3S(errno, ==, EINVAL);

	ret = periodic_schedule(ph, LLONG_MAX - 1, PERIODIC_ONESHOT,
	    badarg_callback, NULL, &id);
	VERIFY3S(ret, ==, -1);
	VERIFY3S(errno, ==, EOVERFLOW);

	ret = periodic_schedule(ph, LLONG_MAX - gethrtime() + 1,
	    PERIODIC_ONESHOT, badarg_callback, NULL, &id);
	VERIFY3S(ret, ==, -1);
	VERIFY3S(errno, ==, EOVERFLOW);

	ret = periodic_schedule(ph, LLONG_MAX - 1, 0,
	    badarg_callback, NULL, &id);
	VERIFY3S(ret, ==, -1);
	VERIFY3S(errno, ==, EOVERFLOW);

	ret = periodic_schedule(ph, LLONG_MAX - gethrtime() + 1, 0,
	    badarg_callback, NULL, &id);
	VERIFY3S(ret, ==, -1);
	VERIFY3S(errno, ==, EOVERFLOW);

	periodic_fini(ph);

	/*
	 * Verify we can't create a clock monotonic without CLOCK_HIGHRES. Just
	 * switch ourselves to the basic set for now.
	 */
	priv = priv_allocset();
	VERIFY3P(priv, !=, NULL);
	priv_basicset(priv);
	ret = setppriv(PRIV_SET, PRIV_PERMITTED, priv);
	VERIFY3S(ret, ==, 0);
	ret = setppriv(PRIV_SET, PRIV_EFFECTIVE, priv);
	VERIFY3S(ret, ==, 0);

	ph = periodic_init(port, NULL, CLOCK_MONOTONIC);
	VERIFY3P(ph, ==, NULL);
	VERIFY3S(errno, ==, EPERM);

	exit(0);
}
