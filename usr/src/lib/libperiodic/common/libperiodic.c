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
 * Provide a library which allows for one shot and periodic callbacks to be
 * made in a manner not disimilar to timeout(9F). Importantly this library does
 * not maintain its own event loop and instead designed to be used in event
 * based systems.
 */

#include <errno.h>
#include <libidspace.h>
#include <limits.h>
#include <port.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <strings.h>
#include <synch.h>
#include <sys/avl.h>
#include <sys/debug.h>
#include <time.h>
#include <umem.h>
#include <libidspace.h>
#include <sys/refhash.h>

#include <libperiodic.h>

typedef enum periodic_flags {
	PERIODIC_F_ONESHOT	= 0x01,
	PERIODIC_F_DELIVERING	= 0x02,
	PERIODIC_F_DESTROY	= 0x04
} periodic_flags_t;

typedef struct periodic {
	periodic_func_t		*peri_func;
	void			*peri_arg;
	periodic_handle_t	*peri_handle;
	id_t			peri_id;
	hrtime_t		peri_value;
	hrtime_t		peri_expire;
	periodic_flags_t	peri_flags;
	avl_node_t		peri_link;
	refhash_link_t		peri_reflink;
} periodic_t;

struct periodic_handle {
	mutex_t		ph_lock;
	cond_t		ph_cond;
	avl_tree_t	ph_tree;
	timer_t		ph_timer;
	boolean_t	ph_processing;
	id_space_t	*ph_idspace;
	refhash_t	*ph_refhash;
};

/*
 * This is an arbitrary prime number we pulled out of thin air, sorry.
 */
#define	PERIODIC_NBUCKETS	73

static void
periodic_hash_dtor(void *buf)
{
	periodic_t *p = buf;
	periodic_handle_t *perh = p->peri_handle;

	VERIFY3S(p->peri_id, !=, -1);
	id_free(perh->ph_idspace, p->peri_id);
	umem_free(p, sizeof (periodic_t));
}

static int
periodic_hash_comparator(const void *l, const void *r)
{
	const id_t lid = *(id_t *)l;
	const id_t rid = *(id_t *)r;

	if (lid > rid)
		return (1);
	if (rid < lid)
		return (-1);
	return (0);
}

static uint64_t
periodic_hash(const void *v)
{
	const id_t *id = v;

	return (*id);
}

static int
periodic_comparator(const void *l, const void *r)
{
	const periodic_t *pl = l, *pr = r;

	if (pl->peri_expire > pr->peri_expire)
		return (1);
	else if (pl->peri_expire < pr->peri_expire)
		return (-1);

	/*
	 * Multiple timers can have the same delivery time, so sort within that
	 * by the address of the timer itself.
	 */
	if ((uintptr_t)l > (uintptr_t)r)
		return (1);
	else if ((uintptr_t)l < (uintptr_t)r)
		return (-1);

	return (0);
}

periodic_handle_t *
periodic_init(int port, void *arg, clockid_t clocktype)
{
	int ret;
	periodic_handle_t *perh;
	port_notify_t pn;
	struct sigevent evp;
	char buf[32];

	perh = umem_alloc(sizeof (periodic_handle_t), UMEM_DEFAULT);
	if (perh == NULL)
		return (NULL);

	if ((ret = mutex_init(&perh->ph_lock, USYNC_THREAD | LOCK_ERRORCHECK,
	    NULL)) != 0) {
		umem_free(perh, sizeof (periodic_handle_t));
		errno = ret;
		return (NULL);
	}

	if ((ret = cond_init(&perh->ph_cond, USYNC_THREAD, NULL)) != 0) {
		VERIFY0(mutex_destroy(&perh->ph_lock));
		umem_free(perh, sizeof (periodic_handle_t));
		errno = ret;
		return (NULL);
	}

	(void) snprintf(buf, sizeof (buf), "periodic_%p", perh);
	if ((perh->ph_idspace = id_space_create(buf, 1, INT32_MAX)) == NULL) {
		ret = errno;
		VERIFY0(cond_destroy(&perh->ph_cond));
		VERIFY0(mutex_destroy(&perh->ph_lock));
		umem_free(perh, sizeof (periodic_handle_t));
		errno = ret;
		return (NULL);
	}

	if ((perh->ph_refhash = refhash_create(PERIODIC_NBUCKETS, periodic_hash,
	    periodic_hash_comparator, periodic_hash_dtor, sizeof (periodic_t),
	    offsetof(periodic_t, peri_reflink), offsetof(periodic_t, peri_id),
	    UMEM_DEFAULT)) == NULL) {
		ret = errno;
		id_space_destroy(perh->ph_idspace);
		VERIFY0(cond_destroy(&perh->ph_cond));
		VERIFY0(mutex_destroy(&perh->ph_lock));
		umem_free(perh, sizeof (periodic_handle_t));
		errno = ret;
		return (NULL);

	}

	avl_create(&perh->ph_tree, periodic_comparator, sizeof (periodic_t),
	    offsetof(periodic_t, peri_link));

	pn.portnfy_port = port;
	pn.portnfy_user = arg;
	evp.sigev_notify = SIGEV_PORT;
	evp.sigev_value.sival_ptr = &pn;

	if (timer_create(clocktype, &evp, &perh->ph_timer) != 0) {
		ret = errno;
		refhash_destroy(perh->ph_refhash);
		id_space_destroy(perh->ph_idspace);
		VERIFY0(cond_destroy(&perh->ph_cond));
		VERIFY0(mutex_destroy(&perh->ph_lock));
		umem_free(perh, sizeof (periodic_handle_t));
		errno = ret;
		return (NULL);
	}

	perh->ph_processing = B_FALSE;

	return (perh);
}

void
periodic_fini(periodic_handle_t *perh)
{
	mutex_enter(&perh->ph_lock);
	VERIFY3S(perh->ph_processing, ==, B_FALSE);
	VERIFY3S(avl_is_empty(&perh->ph_tree), ==, B_TRUE);
	mutex_exit(&perh->ph_lock);
	VERIFY0(timer_delete(perh->ph_timer));
	perh->ph_timer = -1;
	avl_destroy(&perh->ph_tree);
	refhash_destroy(perh->ph_refhash);
	id_space_destroy(perh->ph_idspace);
	VERIFY0(cond_destroy(&perh->ph_cond));
	VERIFY0(mutex_destroy(&perh->ph_lock));
	umem_free(perh, sizeof (periodic_handle_t));
}

static void
periodic_rearm(periodic_handle_t *perh)
{
	struct itimerspec it;
	periodic_t *p;

	VERIFY(MUTEX_HELD(&perh->ph_lock));
	bzero(&it, sizeof (struct itimerspec));
	p = avl_first(&perh->ph_tree);
	if (p != NULL) {
		it.it_value.tv_sec = p->peri_expire / NANOSEC;
		it.it_value.tv_nsec = p->peri_expire % NANOSEC;
	}

	VERIFY0(timer_settime(perh->ph_timer, TIMER_ABSTIME, &it, NULL));
}

void
periodic_fire(periodic_handle_t *perh)
{
	hrtime_t now;
	mutex_enter(&perh->ph_lock);
	now = gethrtime();
	VERIFY3S(perh->ph_processing, ==, B_FALSE);
	perh->ph_processing = B_TRUE;

	for (;;) {
		periodic_t *p;

		p = avl_first(&perh->ph_tree);
		if (p == NULL || p->peri_expire > now)
			break;

		avl_remove(&perh->ph_tree, p);

		/*
		 * Drop the lock to allow for callbacks into the system while
		 * delivering an event.
		 */
		p->peri_flags |= PERIODIC_F_DELIVERING;
		mutex_exit(&perh->ph_lock);

		p->peri_func(p->peri_arg);

		mutex_enter(&perh->ph_lock);
		p->peri_flags &= ~PERIODIC_F_DELIVERING;

		/*
		 * If we have a one shot timer, then it's our responsibility to
		 * clean it up. Otherwise, if we've been marked that it's being
		 * destroyed, due to a call to cancel, then don't touch it again
		 * and signal anyone who might be waiting. Otherwise, we must
		 * have a periodic so, go ahead and reschedule it effectively.
		 */
		if ((p->peri_flags & PERIODIC_F_ONESHOT) &&
		    !(p->peri_flags & PERIODIC_F_DESTROY)) {
			VERIFY3S(p->peri_id, !=, -1);
			refhash_remove(perh->ph_refhash, p);
		} else if ((p->peri_flags & PERIODIC_F_DESTROY) == 0) {
			VERIFY3S((p->peri_flags & PERIODIC_F_ONESHOT), ==, 0);
			VERIFY(LLONG_MAX - p->peri_expire > p->peri_value);
			p->peri_expire += p->peri_value;
			avl_add(&perh->ph_tree, p);
		} else {
			(void) cond_broadcast(&perh->ph_cond);
		}
	}

	periodic_rearm(perh);
	perh->ph_processing = B_FALSE;
	mutex_exit(&perh->ph_lock);
}

int
periodic_schedule(periodic_handle_t *perh, hrtime_t time, int flags,
    periodic_func_t *func, void *farg, periodic_id_t *idp)
{
	periodic_t *p;

	if (flags != 0 && (flags & ~(PERIODIC_ONESHOT | PERIODIC_ABSOLUTE)) != 0) {
		errno = EINVAL;
		return (-1);
	}

	if ((flags & PERIODIC_ABSOLUTE) && !(flags & PERIODIC_ONESHOT)) {
		errno = EINVAL;
		return (-1);
	}

	if (func == NULL) {
		errno = EINVAL;
		return (-1);
	}

	if (time < 0) {
		errno = ERANGE;
		return (-1);
	}

	p = umem_zalloc(sizeof (periodic_t), UMEM_DEFAULT);
	if (p == NULL) {
		errno = ENOMEM;
		return (-1);
	}

	p->peri_func = func;
	p->peri_arg = farg;
	if (flags & PERIODIC_ONESHOT)
		p->peri_flags |= PERIODIC_F_ONESHOT;

	p->peri_handle = perh;
	p->peri_value = time;
	if (flags & PERIODIC_ABSOLUTE) {
		p->peri_expire = time;
	} else {
		p->peri_expire = gethrtime();
		if (LLONG_MAX - p->peri_value < p->peri_value) {
			umem_free(p, sizeof (periodic_t));
			errno = EOVERFLOW;
			return (-1);
		}
		p->peri_expire += p->peri_value;
	}

	mutex_enter(&perh->ph_lock);
	p->peri_id = id_alloc(perh->ph_idspace);
	if (p->peri_id == -1) {
		mutex_exit(&perh->ph_lock);
		umem_free(p, sizeof (periodic_t));
		errno = ENOMEM;
		return (-1);
	}

	refhash_insert(perh->ph_refhash, p);
	avl_add(&perh->ph_tree, p);
	if (perh->ph_processing == B_FALSE)
		periodic_rearm(perh);
	*idp = p->peri_id;
	mutex_exit(&perh->ph_lock);

	return (0);
}

int
periodic_cancel(periodic_handle_t *perh, periodic_id_t id)
{
	periodic_t *p;

	mutex_enter(&perh->ph_lock);
	if ((p = refhash_lookup(perh->ph_refhash, &id)) == NULL) {
		mutex_exit(&perh->ph_lock);
		errno = ENOENT;
		return (-1);
	}

	p->peri_flags |= PERIODIC_F_DESTROY;
	if (p->peri_flags & PERIODIC_F_DELIVERING) {
		while (p->peri_flags & PERIODIC_F_DELIVERING)
			(void) cond_wait(&perh->ph_cond, &perh->ph_lock);
	} else {
		avl_remove(&perh->ph_tree, p);
	}

	refhash_remove(perh->ph_refhash, p);

	if (perh->ph_processing == B_FALSE)
		periodic_rearm(perh);
	mutex_exit(&perh->ph_lock);

	return (0);
}
