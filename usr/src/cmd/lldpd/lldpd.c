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
 * lldpd - the link layer discovery daemon
 */

/*
 * General notes on watching changes. There are a bunch of things that we care
 * about in terms of both per-link state and per-host state. We can get most
 * everything through a dlpi notification. However, we need to also rig up a
 * sysevent on hostname changes.
 */

#include <stdlib.h>
#include <libgen.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/dls_mgmt.h>
#include <fcntl.h>
#include <paths.h>
#include <sys/debug.h>
#include <sys/resource.h>
#include <sys/param.h>
#include <sys/fork.h>
#include <signal.h>
#include <sys/wait.h>
#include <priv.h>
#include <port.h>
#include <libperiodic.h>
#include <thread.h>
#include <synch.h>
#include <sys/signalfd.h>
#include <libdladm.h>
#include <libdllink.h>
#include <libdlpi.h>
#include <sys/list.h>
#include <stddef.h>
#include <netdb.h>
#include <umem.h>
#include <liblldp.h>
#include <sys/avl.h>
#include <sys/ethernet.h>

#define	LLDPD_DATA_DIR	"/var/lldpd"
#define	LLDPD_DOOR_PATH	"/var/run/lldpd.door"

#define	LLDPD_EXIT_REQUESTED	0
#define	LLDPD_EXIT_STARTED	0
#define	LLDPD_EXIT_FATAL	1
#define	LLDPD_EXIT_USAGE	2

typedef struct lldpd lldpd_t;
typedef struct lldpd_datalink lldpd_datalink_t;

typedef void (lldpd_event_f)(lldpd_t *, port_event_t *, void *);

typedef struct lldpd_event {
	lldpd_event_f	*le_func;
	void		*le_arg;
	int		le_events;
} lldpd_event_t;

typedef struct lldpd_buffer {
	size_t	lb_bufsize;
	void	*lb_readbuf;
	void	*lb_writebuf;
} lldpd_buffer_t;

/*
 * We persist remote entries based on the following schema:
 *
 * %datalink.%mac
 *
 * Here %datalink is the name of the datalink that the LLPDU was received on
 * (that's a datalink on the local machine) and %mac is the MAC address of the
 * remote target that we received the LLPDU from.
 */
#define	LLDPD_PERSIST_NAMELEN	(ETHERADDRSTRL + DLPI_LINKNAME_MAX + 1)

typedef enum lldpd_remote_state {
	LRS_REMOVING	= 0x00,
	LRS_RETIRED	= 0x01,
	LRS_VALID	= 0x02
} lldpd_remote_state_t;

typedef struct lldpd_rhost {
	avl_node_t		lr_link;
	lldpd_datalink_t	*lr_dlp;
	char			lr_name[LLDPD_PERSIST_NAMELEN];
	uint8_t			lr_addr[ETHERADDRL];
	hrtime_t		lr_expire;
	lldpd_remote_state_t	lr_state;
	periodic_id_t		lr_peri;
	nvlist_t		*lr_data;
} lldpd_rhost_t;

struct lldpd_datalink {
	list_node_t		ld_lnode;
	lldpd_t			*ld_lldpd;
	mutex_t			ld_lock;
	boolean_t		ld_bound;
	link_state_t		ld_linkstate;
	int			ld_fd;
	datalink_class_t	ld_dlclass;
	datalink_id_t		ld_dlid;
	dlpi_handle_t		ld_dlpi;
	dlpi_info_t		ld_info;
	dlpi_notifyid_t		ld_notify;
	lldpd_buffer_t		ld_bufs;
	lldpd_event_t		ld_event;
	uint64_t		ld_baddlpi;
	avl_tree_t		ld_rhosts;
}; 

/*
 * This contains the primary daemon state. It's in one static structure mostly
 * for convenience.
 */
struct lldpd {
	mutex_t			lldpd_lock;
	boolean_t		lldpd_teardown;
	datalink_class_t	lldpd_dlclass;
	uint32_t		lldpd_dlmedia;
	periodic_handle_t	*lldpd_perh;
	dladm_handle_t		lldpd_dladm;
	int			lldpd_dirfd;		/* WO */
	int			lldpd_port;		/* WO */
	lldpd_event_t		lldpd_perh_event;
	int			lldpd_sigfd;
	lldpd_event_t		lldpd_sig_event;
	list_t			lldpd_datalinks;
	char			lldpd_hostname[MAXHOSTNAMELEN];
};

static const uint8_t lldpd_bindmac[6] = { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x0e };
static const char *lldpd_progname;
static lldpd_t lldpd_state;

#ifdef	DEBUG
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
#endif	/* DEBUG */

static void
lldpd_vwarn(FILE *out, const char *fmt, va_list ap)
{
	int error = errno;

	(void) fprintf(out, "%s: ", lldpd_progname);
	(void) vfprintf(out, fmt, ap);

	if (fmt[strlen(fmt) - 1] != '\n')
		(void) fprintf(out, ": %s\n", strerror(error));

	(void) fflush(out);
}

static void
lldpd_warn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	lldpd_vwarn(stderr, fmt, ap);
	va_end(ap);
}

static void
lldpd_fatal(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	lldpd_vwarn(stderr, fmt, ap);
	va_end(ap);

	exit(LLDPD_EXIT_FATAL);
}

static void
lldpd_abort(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	lldpd_vwarn(stderr, fmt, ap);
	va_end(ap);

	abort();
}

static void
lldpd_dfatal(int dfd, const char *fmt, ...)
{
	int status = LLDPD_EXIT_FATAL;
	va_list ap;

	va_start(ap, fmt);
	lldpd_vwarn(stderr, fmt, ap);
	va_end(ap);

	/* Take a single shot at this */
	(void) write(dfd, &status, sizeof (status));
	exit(status);
}

static int
lldpd_rhost_comparator(const void *l, const void *r)
{
	int ret;
	const lldpd_rhost_t *ll = l, *rl = r;

	ret = memcmp(ll->lr_addr, rl->lr_addr, ETHERADDRL);
	if (ret > 0)
		return (1);
	if (ret < 0)
		return (-1);
	return (0);
}

/*
 * At the moment we don't allow this function to fail and VERIFY that
 * associating works. This is a bit unfortuante, but it's hard to figure out
 * what the daemon should do in the fact of association failures and memory
 * failures. While the extent store of records could be useful, not much else is
 * unfortunately.
 */
static void
lldpd_event_associate(lldpd_t *lldpd, lldpd_event_t *lep, int fd)
{
	int ret;

	ret = port_associate(lldpd->lldpd_port, PORT_SOURCE_FD, fd,
	    lep->le_events, lep);
	VERIFY0(ret);
}

static void
lldpd_rhost_fini(lldpd_t *lldpd, lldpd_rhost_t *lrp)
{
	if (lrp->lr_data != NULL)
		nvlist_free(lrp->lr_data);
	umem_free(lrp, sizeof (lldpd_rhost_t));
}

static void
lldpd_rhost_timer(void *arg)
{
	hrtime_t now;
	lldpd_rhost_t *lrp = arg;
	lldpd_datalink_t *dlp = lrp->lr_dlp;
	lldpd_t *lldpd = dlp->ld_lldpd;

	mutex_enter(&dlp->ld_lock);

	VERIFY(lrp->lr_state == LRS_REMOVING || lrp->lr_state == LRS_VALID);

	/*
	 * Someone's trying to remove it already, let it go.
	 */
	if (lrp->lr_state == LRS_REMOVING) { 
		mutex_exit(&dlp->ld_lock);
		return;
	}
	now = gethrtime();

	if (lrp->lr_expire < now) {
		/*
		 * Link's down, keep us around until the link is back up to give
		 * the poor administrator who's trying to figure out what we
		 * used to be talking to.
		 */
		if (dlp->ld_linkstate == LINK_STATE_DOWN) {
			lrp->lr_state = LRS_RETIRED;
			mutex_exit(&dlp->ld_lock);
			return;
		}

		/*
		 * Time to tear this down, it's expired. Remove it from the tree
		 * and clean it up.
		 */
		avl_remove(&dlp->ld_rhosts, lrp);
		lldpd_rhost_fini(lldpd, lrp);
	}
	mutex_exit(&dlp->ld_lock);
}

/*
 * Attempt to save information about this host. If we cannot allocate memory for
 * this host, then, we end up dropping it. Note, if the ttl is zero, then
 * instead of looking the host, we need to instead tear it down.
 */
static void
lldpd_rhost_update(lldpd_t *lldpd, lldpd_datalink_t *dlp, const uint8_t *mac,
    nvlist_t *nvp)
{
	uint16_t ttl;
	lldpd_rhost_t lookup, *lrp;
	avl_index_t where;

	ttl = fnvlist_lookup_uint16(nvp, "ttl");
	bcopy(mac, lookup.lr_addr, ETHERADDRL);

	mutex_enter(&dlp->ld_lock);

	lrp = avl_find(&dlp->ld_rhosts, &lookup, &where);

	/*
	 * No such entry exists, try to create a new one.
	 */
	if (lrp == NULL) {
		char macstr[ETHERADDRSTRL];

		lrp = umem_zalloc(sizeof (lldpd_rhost_t), UMEM_DEFAULT);
		if (lrp == NULL) {
			/* XXX bump counter and basically treat as a drop */
			goto done;
		}

		(void) ether_ntoa_r((struct ether_addr *)mac, macstr);
		(void) snprintf(lrp->lr_name, sizeof (lrp->lr_name), "%s.%s",
		    dlp->ld_info.di_linkname, macstr);
		bcopy(mac, lrp->lr_addr, ETHERADDRL);
		lrp->lr_state = LRS_VALID;
		lrp->lr_data = nvp;
		lrp->lr_peri = PERIODIC_INVALID_ID;
		lrp->lr_dlp = dlp;
		avl_insert(&dlp->ld_rhosts, lrp, where);
		goto update;
	}

	/*
	 * If the ttl is zero, that indicates that the remote entry is being
	 * shut down and we should proactively remove it.
	 */
	if (ttl == 0) {
		avl_remove(&dlp->ld_rhosts, lrp);
		lldpd_rhost_fini(lldpd, lrp);
		goto done;
	}

	/*
	 * This is a standard update.
	 * XXX We should figure out htf to diff the nvlist_t.
	 */
	nvlist_free(lrp->lr_data);
	lrp->lr_data = nvp;
	lrp->lr_state = LRS_VALID;

	/*
	 * Now we need to update the timeout for this entry. This is done in two
	 * parts. The first is to go through and update the expiration time,
	 * then we have to reschedule a periodic.
	 */
update:
	lrp->lr_expire = gethrtime() + ttl * NANOSEC;
	mutex_exit(&dlp->ld_lock);

	(void) periodic_cancel(lldpd->lldpd_perh, lrp->lr_peri);
	if (periodic_schedule(lldpd->lldpd_perh, lrp->lr_expire,
	    PERIODIC_ONESHOT | PERIODIC_ABSOLUTE, lldpd_rhost_timer, lrp,
	    &lrp->lr_peri) != 0) {
		if (errno != ENOMEM)
			lldpd_abort("programmer error from periodic_schedule");
		/*
		 * Not enough memory, drop this entry. XXX Bump a stat.
		 */
		avl_remove(&dlp->ld_rhosts, lrp);
		lldpd_rhost_fini(lldpd, lrp);
	}

	mutex_enter(&dlp->ld_lock);


done:
	mutex_exit(&dlp->ld_lock);
}

/*
 * Update the send and receive buffers. If the new size is less than our current
 * size (eg. because the MTU changed), then do not bother shrinking the buffer.
 *
 * TODO We should figure out a more graceful way to handle this kind of memory
 * failure.
 */
static void
lldpd_dlbuf_update(lldpd_datalink_t *dlp, size_t sz)
{
	void *rx, *tx;

	VERIFY(MUTEX_HELD(&dlp->ld_lock));
	if (dlp->ld_bufs.lb_bufsize >= sz) {
		mutex_exit(&dlp->ld_lock);
		return;
	}

	rx = umem_zalloc(sz, UMEM_DEFAULT);
	if (rx == NULL) {
		lldpd_abort("failed to allocated %d bytes for %s's rx buffer",
		    sz, dlp->ld_info.di_linkname);
	}
	if (dlp->ld_bufs.lb_bufsize != 0)
		umem_free(dlp->ld_bufs.lb_readbuf, dlp->ld_bufs.lb_bufsize);
	dlp->ld_bufs.lb_readbuf = rx;

	tx = umem_zalloc(sz, UMEM_DEFAULT);
	if (tx == NULL) {
		lldpd_abort("failed to allocated %d bytes for %s's tx buffer",
		    sz, dlp->ld_info.di_linkname);
	}
	if (dlp->ld_bufs.lb_bufsize != 0)
		umem_free(dlp->ld_bufs.lb_writebuf, dlp->ld_bufs.lb_bufsize);
	dlp->ld_bufs.lb_writebuf = tx;

	dlp->ld_bufs.lb_bufsize = sz;
}

static void
lldpd_datalink_fini(lldpd_t *lldpd, lldpd_datalink_t *dlp)
{
	lldpd_rhost_t *lrp;
	void *cookie = NULL;

	VERIFY3P(dlp, !=, NULL);
	VERIFY3P(dlp->ld_dlpi, !=, NULL);
	dlpi_close(dlp->ld_dlpi);
	dlp->ld_dlpi = NULL;

	if (dlp->ld_bufs.lb_bufsize != 0) {
		VERIFY3P(dlp->ld_bufs.lb_readbuf, !=, NULL);
		VERIFY3P(dlp->ld_bufs.lb_readbuf, !=, NULL);
		umem_free(dlp->ld_bufs.lb_readbuf, dlp->ld_bufs.lb_bufsize);
		umem_free(dlp->ld_bufs.lb_writebuf, dlp->ld_bufs.lb_bufsize);
	}

	while ((lrp = avl_destroy_nodes(&dlp->ld_rhosts, &cookie)) != NULL)
		lldpd_rhost_fini(lldpd, lrp);

	VERIFY0(mutex_destroy(&dlp->ld_lock));
	umem_free(dlp, sizeof (lldpd_datalink_t));
}

static void
lldpd_datalink_notify(dlpi_handle_t dhp, dlpi_notifyinfo_t *ip, void *arg)
{
	const dlpi_notifyinfo_t *info = ip;
	lldpd_datalink_t *dlp = arg;
	boolean_t update = B_FALSE;

	mutex_enter(&dlp->ld_lock);
	switch (info->dni_note) {
	case DL_NOTE_SDU_SIZE:
		if (info->dni_size != dlp->ld_info.di_max_sdu) {
			lldpd_dlbuf_update(dlp, info->dni_size);
			dlp->ld_info.di_max_sdu = info->dni_size;
		}
		break;
	case DL_NOTE_PHYS_ADDR:
		VERIFY3S(info->dni_physaddrlen, <=, DLPI_PHYSADDR_MAX);
		if (dlp->ld_info.di_physaddrlen == info->dni_physaddrlen &&
		    bcmp(dlp->ld_info.di_physaddr, info->dni_physaddr,
		    info->dni_physaddrlen) == 0)
			break;
		dlp->ld_info.di_physaddrlen = info->dni_physaddrlen;
		bcopy(info->dni_physaddr, dlp->ld_info.di_physaddr,
		    info->dni_physaddrlen);
		update = B_TRUE;
		break;
	case DL_NOTE_LINK_DOWN:
		dlp->ld_linkstate = LINK_STATE_DOWN;
		break;
	case DL_NOTE_LINK_UP:
		if (dlp->ld_linkstate == LINK_STATE_UP)
			break;
		update = B_TRUE;
		dlp->ld_linkstate = LINK_STATE_UP;
		break;
	default:
		break;
	}

	/*
	 * On some of these events, we should consider updating information
	 * about ourselves. For example, if our address changed, then we should
	 * go through and make sure that we toggle the state machine. Similarly
	 * if we just came up, we should probably consider letting the other end
	 * know that we exist.
	 */
	if (update == B_TRUE) {
		lldpd_warn("XXX need to send updates due to link notification\n");
	}

	mutex_exit(&dlp->ld_lock);
}

static void
lldpd_datalink_recv(lldpd_t *lldpd, lldpd_datalink_t *dlp)
{
	int ret;
	uint8_t saddr[DLPI_PHYSADDR_MAX];
	size_t saddrlen = sizeof (saddr);
	size_t msglen = dlp->ld_bufs.lb_bufsize;
	char saddrstr[ETHERADDRSTRL];
	nvlist_t *nvl;

	/*
	 * To simulate a non-blocking read/write on the file descriptor, we use
	 * a timeout of zero miliseconds.
	 */
	ret = dlpi_recv(dlp->ld_dlpi, saddr, &saddrlen, dlp->ld_bufs.lb_readbuf,
	    &msglen, 0, NULL);
	if (ret != DLPI_SUCCESS) {
		switch (ret) {
		case DLPI_ETIMEDOUT:
			goto done;
		case DLPI_EINVAL:
		case DLPI_EINHANDLE:
		case DLPI_EUNAVAILSAP:
			lldpd_abort("failed to recv on %s: %s\n",
			    dlp->ld_info.di_linkname, dlpi_strerror(ret));
		default:
			/*
			 * This indicates that some other DLPI error has
			 * happened. In general, we don't expect that this
			 * should happen and while several of these indicate
			 * that something fundamentally is wrong, it's not 100%
			 * obvious so we don't abort and instead bump a counter
			 * and try again in the future.
			 */
			/* XXX atomic on lldpd_t */
			dlp->ld_baddlpi++;
			return;
		}
	}

	VERIFY3S(saddrlen, ==, ETHERADDRL);
	(void) ether_ntoa_r((struct ether_addr *)saddr, saddrstr);
	printf("received %d bytes from %s\n", msglen, saddrstr);
	if (lldp_parse_frame(dlp->ld_bufs.lb_readbuf, msglen, &nvl) != 0) {
		/* TODO bump counters and drop log msg */
		lldpd_warn("failed to parse lldp frame from %s\n", saddrstr);
		goto done;
	}
	printf("successfully parsed lldp frame\n");
	nvlist_print(stderr, nvl);

	lldpd_rhost_update(lldpd, dlp, saddr, nvl);

done:
	dlp->ld_event.le_events |= (POLLIN | POLLRDNORM);
}

static void
lldpd_datalink_fire(lldpd_t *lldpd, port_event_t *pe, void *arg)
{
	lldpd_datalink_t *dlp = arg;

	/*
	 * XXX in theory we should lock around the buffer use, but it's not
	 * really worth and shouldn't be needed generally.
	 */
	if (pe->portev_events & (POLLIN | POLLRDNORM)) {
		lldpd_datalink_recv(lldpd, dlp);
	}

	if (pe->portev_events & POLLOUT) {

	}

	lldpd_event_associate(lldpd, &dlp->ld_event, dlp->ld_fd);
}

static void
lldpd_datalink_init(lldpd_t *lldpd, char *name, datalink_id_t id,
    datalink_class_t class)
{
	int ret;
	lldpd_datalink_t *dlp;

	dlp = umem_zalloc(sizeof (lldpd_datalink_t), UMEM_DEFAULT);
	if (dlp == NULL) {
		lldpd_abort("failed to allocate memory for datalink %s\n",
		    name);
	}
	VERIFY0(mutex_init(&dlp->ld_lock, USYNC_THREAD | LOCK_ERRORCHECK,
	    NULL));

	dlp->ld_lldpd = lldpd;
	dlp->ld_linkstate = LINK_STATE_UNKNOWN;
	dlp->ld_dlclass = class;
	dlp->ld_dlid = id;
	avl_create(&dlp->ld_rhosts, lldpd_rhost_comparator, sizeof (lldpd_rhost_t),
	    offsetof(lldpd_rhost_t, lr_link));

	/*
	 * We should be able to open just about every datalink, even if we
	 * cannot bind to them. However, we do not treat a failure to open the
	 * datalink as fatal (as long as the reason isn't EACCES which
	 * indicates that we don't have the privilege).
	 */
	if ((ret = dlpi_open(name, &dlp->ld_dlpi, 0)) != DLPI_SUCCESS) {
		if (ret == DL_SYSERR && errno == EACCES)
			lldpd_abort("failed to open datalink %s due to missing "
			    "priv: %s", name, dlpi_strerror(ret));
		lldpd_warn("failed to open a dlpi handle to %s: %s\n", name,
		    dlpi_strerror(ret));
		umem_free(dlp, sizeof (lldpd_datalink_t));
		return;
	}

	/*
	 * Attempt to bind to the datalink. Note, that there are some cases
	 * where we may not be able to bind to the device. Generally this
	 * indicates that either someone else is listening on our SAP (aka
	 * Ethertype) or the link has been exclusively opened, eg. it is part of
	 * an aggergation. In that case, we keep the lldpd_datalink_t around so
	 * that we can report on this fact to users.
	 */
	ret = dlpi_bind(dlp->ld_dlpi, ETHERTYPE_LLDP, NULL);
	if (ret != DLPI_SUCCESS) {
		VERIFY3S(ret, ==, DLPI_EUNAVAILSAP);
		ret = dlpi_info(dlp->ld_dlpi, &dlp->ld_info, 0);
		VERIFY3S(ret, ==, DLPI_SUCCESS);
		lldpd_warn("failed to bind to link %s, something is already "
		    "using the link for lldp or it may be part of an "
		    "aggregation", name);
		goto insert;
	}

	ret = dlpi_enabmulti(dlp->ld_dlpi, lldpd_bindmac, sizeof lldpd_bindmac);
	if (ret != DLPI_SUCCESS) {
		lldpd_warn("failed to enable multicast on link %s: %s\n",
		    link, dlpi_strerror(ret));
		dlpi_close(dlp->ld_dlpi);
		umem_free(dlp, sizeof (lldpd_datalink_t));
		return;
	}

	ret = dlpi_info(dlp->ld_dlpi, &dlp->ld_info, 0);
	VERIFY3S(ret, ==, DLPI_SUCCESS);

	/*
	 * We attempt to enable notifications on the link for all the
	 * information that we wish to know about changing; however, if for some
	 * reason we cannot enable it (it's not supported, etc.) then we warn
	 * about it, but keep the link in service.
	 */
	ret = dlpi_enabnotify(dlp->ld_dlpi, DL_NOTE_LINK_UP | DL_NOTE_SDU_SIZE |
	    DL_NOTE_PHYS_ADDR | DL_NOTE_LINK_DOWN, lldpd_datalink_notify, dlp,
	    &dlp->ld_notify);
	if (ret != DLPI_SUCCESS) {
		lldpd_warn("failed to enable notifications on link %s: %s\n",
		    name, dlpi_strerror(ret));
		lldpd_warn("link information on %s may go stale\n", name);
	}

	mutex_enter(&dlp->ld_lock);
	lldpd_dlbuf_update(dlp, dlp->ld_info.di_max_sdu);
	mutex_exit(&dlp->ld_lock);

	dlp->ld_fd = dlpi_fd(dlp->ld_dlpi);
	VERIFY3S(dlp->ld_fd, >, -1);
	dlp->ld_event.le_func = lldpd_datalink_fire;
	dlp->ld_event.le_arg = dlp;
	dlp->ld_event.le_events = POLLIN | POLLRDNORM;
	lldpd_event_associate(lldpd, &dlp->ld_event, dlp->ld_fd);

insert:
	list_insert_tail(&lldpd->lldpd_datalinks, dlp);
}


static void
lldpd_dir_fini(lldpd_t *lldpd)
{
	VERIFY3S(lldpd->lldpd_dirfd, >, -1);
	VERIFY0(close(lldpd->lldpd_dirfd));
	lldpd->lldpd_dirfd = -1;
}

static void
lldpd_dir_init(lldpd_t *lldpd)
{
	int fd;

	if (mkdir(LLDPD_DATA_DIR, 0755) != 0) {
		if (errno != EEXIST) {
			lldpd_fatal("failed to make data directory %s",
			    LLDPD_DATA_DIR);
		}
	}

	if ((fd = open(LLDPD_DATA_DIR, O_RDONLY)) < 0)
		lldpd_fatal("failed to open data directory");

	if (fchown(fd, UID_NETADM, GID_NETADM) != 0)
		lldpd_fatal("failed to set the uid/gid for the data directory");

	if (fchmod(fd, 0755) != 0)
		lldpd_fatal("failed to set data directory permissions");

	if (fchdir(fd) != 0)
		lldpd_fatal("failed to cd to %s", LLDPD_DATA_DIR);

	lldpd->lldpd_dirfd = fd;
}

static void
lldpd_door_fini(lldpd_t *lldpd)
{

}

static void
lldpd_door_init(lldpd_t *lldpd, int dfd)
{

}

/*
 * Note, that even though we're being started as a privileged process, we may
 * not actually have PRIV_CLOCK_HIGHRES available to us as it could be
 * restricted in a given zone. We only try to assert it if we came in with it
 * on. The timer code can deal with a lack f PRIV_CLOCK_HIGHRES.
 */
static void
lldpd_drop_privs(int dfd)
{
	boolean_t hrclock = B_FALSE;
	priv_set_t *pset;

	if (setsid() == -1)
		lldpd_dfatal(dfd, "failed to create session");

	if ((pset = priv_allocset()) == NULL)
		lldpd_dfatal(dfd, "failed to allocate privilege set");

	VERIFY0(setgroups(0, NULL));
	VERIFY0(setgid(GID_NETADM));
	VERIFY0(seteuid(UID_NETADM));

	VERIFY0(getppriv(PRIV_PERMITTED, pset));
	if (priv_ismember(pset, PRIV_PROC_CLOCK_HIGHRES) == B_TRUE)
		hrclock = B_TRUE;

	priv_basicset(pset);
	if (priv_delset(pset, PRIV_PROC_EXEC) == -1 ||
	    priv_delset(pset, PRIV_PROC_INFO) == -1 ||
	    priv_delset(pset, PRIV_PROC_FORK) == -1 ||
	    priv_delset(pset, PRIV_PROC_SESSION) == -1 ||
	    priv_delset(pset, PRIV_FILE_LINK_ANY) == -1 ||
	    priv_delset(pset, PRIV_NET_ACCESS) == -1 ||
	    priv_addset(pset, PRIV_NET_RAWACCESS) == -1)
		lldpd_abort("failed to initialize fill out privilege set");

	if (hrclock == B_TRUE &&
	    priv_addset(pset, PRIV_PROC_CLOCK_HIGHRES) == -1)
		lldpd_abort("failed to add CLOCK_HIGHRES to privilege set");

	if (setppriv(PRIV_SET, PRIV_PERMITTED, pset) == -1)
		lldpd_dfatal(dfd, "failed to set permitted privilege set");
	if (setppriv(PRIV_SET, PRIV_EFFECTIVE, pset) == -1)
		lldpd_dfatal(dfd, "failed to set effective privilege set");

	priv_freeset(pset);
}

/*
 * Daemonize ourselves. This mean that we need to do the following:
 *
 * o Ensure all file descriptors after stderr are closed.
 * o Make sure stdin is /dev/null.
 * o Ensure we properly have our core rlimit tuned as much as we can.
 * o Ensure that we have /var/lldpd/ and the appropriate perms are on it.
 * o Do our fork / setsid dance
 * o Open our door server
 * o change to NETADM and drop ancillary groups
 * o Drop most privileges. We keep PRIV_NET_RAWACCESS from the privileged
 *   privs and file read/write. We don't bother with socket access as we use
 *   dlpi. We attempt to claim PRIC_PROC_CLOCK_HIGHRES, but carry on when we
 *   don't have it.
 */
static int
lldpd_daemonize(lldpd_t *lldpd)
{
	int dnull;
	int estatus, pfds[2];
	struct rlimit rlim;
	sigset_t set, oset;
	pid_t child;

	dnull = open(_PATH_DEVNULL, O_RDONLY);
	if (dnull < 0)
		lldpd_fatal("failed to open %s", _PATH_DEVNULL);
	if (dup2(dnull, STDIN_FILENO) != 0)
		lldpd_fatal("failed to dup stdin to %s", _PATH_DEVNULL);
	closefrom(STDERR_FILENO + 1);

	rlim.rlim_cur = RLIM_INFINITY;
	rlim.rlim_max = RLIM_INFINITY;
	(void) setrlimit(RLIMIT_CORE, &rlim);

	lldpd_dir_init(lldpd);

	/*
	 * At this point block all signals going in so we don't have the parent
	 * mistakingly exit when the child is running, but never block SIGABRT.
	 */
	VERIFY0(sigfillset(&set));
	VERIFY0(sigdelset(&set, SIGABRT));
	VERIFY0(sigprocmask(SIG_BLOCK, &set, &oset));

	if (pipe(pfds) != 0)
		lldpd_fatal("failed to create pipe for daemonizing");

	if ((child = forkx(FORK_WAITPID | FORK_NOSIGCHLD)) == -1)
		lldpd_fatal("failed to fork for daemonizing");

	if (child != 0) {
		/*
		 * Normally one should VERIFY0 the below call, but it's more
		 * important that we get our child's status and exit
		 * appropriately.
		 */
		(void) close(pfds[1]);
		if (read(pfds[0], &estatus, sizeof (estatus)) !=
		    sizeof (estatus))
			_exit(LLDPD_EXIT_FATAL);

		if (estatus == 0)
			_exit(LLDPD_EXIT_STARTED);

		if (waitpid(child, &estatus, 0) == child && WIFEXITED(estatus))
			_exit(WEXITSTATUS(estatus));

		_exit(LLDPD_EXIT_FATAL);
	}

	VERIFY0(close(pfds[0]));

	/*
	 * We should initialize our door here. If we don't do it here, then
	 * we'll lose the chance later. Kind of unfortunate privilege challenge,
	 * but oh well. Thankfully we already have the right signal handler
	 * setup.
	 */
	lldpd_door_init(lldpd, pfds[1]);

	lldpd_drop_privs(pfds[1]);

	VERIFY0(sigprocmask(SIG_SETMASK, &oset, NULL));
	(void) umask(0022);

	return (pfds[1]);
}

static void
lldpd_event_fini(lldpd_t *lldpd)
{
	VERIFY3S(lldpd->lldpd_port, >, -1);
	VERIFY0(close(lldpd->lldpd_port));
	lldpd->lldpd_port = -1;
}

static void
lldpd_event_init(lldpd_t *lldpd, int dfd)
{
	int p;
	p = port_create();
	if (p < 0)
		lldpd_dfatal(dfd, "failed to create event port");
	lldpd->lldpd_port = p;
}

static void
lldpd_timer_fini(lldpd_t *lldpd)
{
	VERIFY3P(lldpd->lldpd_perh, !=, NULL);
	periodic_fini(lldpd->lldpd_perh);
	lldpd->lldpd_perh = NULL;
}

static void
lldpd_timer_fire(lldpd_t *lldpd, port_event_t *pe, void *arg)
{
	periodic_fire(lldpd->lldpd_perh);
}

/*
 * We always attempt to use CLOCK_HIGHRES before falling back to CLOCK_REALTIME.
 * We do the fallback in case we're running in a zone where CLOCK_HIGHRES is
 * not permitted.
 */
static void
lldpd_timer_init(lldpd_t *lldpd, int dfd)
{
	periodic_handle_t *perh;

	lldpd->lldpd_perh_event.le_func = lldpd_timer_fire;
	lldpd->lldpd_perh_event.le_arg = NULL;
	lldpd->lldpd_perh_event.le_events = 0;

	perh = periodic_init(lldpd->lldpd_port, &lldpd->lldpd_perh_event,
	    CLOCK_HIGHRES);
	if (perh == NULL) {
		if (errno != EPERM) {
			lldpd_dfatal(dfd, "failed to create libperiodic handle "
			    "with highres clock");
		}

		lldpd_warn("cannot use high resolution clock, falling back to "
		    "realtime clock\n");
		perh = periodic_init(lldpd->lldpd_port, &lldpd->lldpd_perh_event,
		    CLOCK_REALTIME);
		if (perh == NULL) {
			lldpd_dfatal(dfd, "failed to create libperiodic handle "
			    "with realtime clock");
		}
	}

	lldpd->lldpd_perh = perh;
}

static void
lldpd_signal_fini(lldpd_t *lldpd)
{
	VERIFY3S(lldpd->lldpd_sigfd, >, -1);
	VERIFY0(close(lldpd->lldpd_sigfd));
	lldpd->lldpd_sigfd = -1;
}

static void
lldpd_signal_fire(lldpd_t *lldpd, port_event_t *pe, void *arg)
{
	ssize_t ret;
	struct signalfd_siginfo si;

	VERIFY3S(pe->portev_events & (POLLIN | POLLRDNORM), !=, 0);

	/*
	 * signalfd should atomically give us a single datum. Partial reads
	 * aren't supported by it, so we'll either get everything or nothing.
	 */
	do {
		ret = read(lldpd->lldpd_sigfd, &si, sizeof (si));
	} while (ret == -1 && errno == EINTR);

	if (ret == -1) {
		if (errno == EAGAIN) {
			lldpd_event_associate(lldpd, &lldpd->lldpd_sig_event,
			    lldpd->lldpd_sigfd);
			return;
		}

		lldpd_abort("received unexpected errno when reading signalfd");
	}

	if (ret != sizeof (si))
		lldpd_abort("signalfd_siginfo read size mismatch, expected: %d,"
		    "actual: %d\n", sizeof (si), ret);

	/*
	 * Every signal that we receive indicates a teardown at this point, so
	 * don't bother rearming the signalfd event.
	 */
	mutex_enter(&lldpd->lldpd_lock);
	lldpd->lldpd_teardown = B_TRUE;
	mutex_exit(&lldpd->lldpd_lock);
}

/*
 * Until we can use event ports to notify that a signal we care about is
 * consumable, use signalfd(3C). Therefore we need to make sure that we mask off
 * all the signals we actually want to receive via signalfd from our main
 * thread. Remember that the threads for doors have signals generally masked so
 * these can't come to those threads.
 */
static void
lldpd_singal_init(lldpd_t *lldpd, int dfd)
{
	int s;
	sigset_t mask;

	if (sigemptyset(&mask) != 0 ||
	    sigaddset(&mask, SIGHUP) != 0 ||
	    sigaddset(&mask, SIGINT) != 0 ||
	    sigaddset(&mask, SIGQUIT) != 0 ||
	    sigaddset(&mask, SIGTERM) != 0 ||
	    sigprocmask(SIG_BLOCK, &mask, NULL) != 0)
		lldpd_abort("failed to assemble signal mask");

	lldpd->lldpd_sig_event.le_func = lldpd_signal_fire;
	lldpd->lldpd_sig_event.le_arg = NULL;
	lldpd->lldpd_sig_event.le_events = POLLIN | POLLRDNORM;

	s = signalfd(-1, &mask, SFD_NONBLOCK);
	if (s < 0)
		lldpd_dfatal(dfd, "failed to create signalfd");

	lldpd->lldpd_sigfd = s;
	lldpd_event_associate(lldpd, &lldpd->lldpd_sig_event,
	    lldpd->lldpd_sigfd);
}

/*
 * Clean up and flush all datalink state. At this point, we assume that the
 * event loop has been torn down.
 */
static void
lldpd_dladm_fini(lldpd_t *lldpd)
{
	lldpd_datalink_t *dlp;

	while ((dlp = list_remove_head(&lldpd->lldpd_datalinks)) != NULL)
		lldpd_datalink_fini(lldpd, dlp);
	list_destroy(&lldpd->lldpd_datalinks);

	VERIFY3P(lldpd->lldpd_dladm, !=, NULL);
	dladm_close(lldpd->lldpd_dladm);
	lldpd->lldpd_dladm = NULL;
}

static int
lldpd_dladm_init_cb(dladm_handle_t dlhp, datalink_id_t id, void *arg)
{
	lldpd_t *lldpd = arg;
	dladm_status_t err;
	char link[MAXLINKNAMELEN];
	char errmsg[DLADM_STRSIZE];
	datalink_class_t class;
	uint32_t media;

	if ((err = dladm_datalink_id2info(dlhp, id, NULL, &class, &media,
	    link, sizeof (link))) != DLADM_STATUS_OK) {
		lldpd_warn("failed to get dladm information for datalink "
		    "%d: %s", id, dladm_status2str(err, errmsg));
		return (DLADM_WALK_CONTINUE);
	}

	if ((class & lldpd->lldpd_dlclass) == 0)
		return (DLADM_WALK_CONTINUE);

	if (media != lldpd->lldpd_dlmedia)
		return (DLADM_WALK_CONTINUE);

	lldpd_datalink_init(lldpd, link, id, class);

	return (DLADM_WALK_CONTINUE);
}

static void
lldpd_dladm_init(lldpd_t *lldpd, int dfd)
{
	dladm_status_t err;
	dladm_handle_t dlhp;
	char errmsg[DLADM_STRSIZE];

	list_create(&lldpd->lldpd_datalinks, sizeof (lldpd_datalink_t),
	    offsetof(lldpd_datalink_t, ld_lnode));

	if ((err = dladm_open(&dlhp)) != DLADM_STATUS_OK) {
		lldpd_dfatal(dfd, "failed to open handle to libdladm: %s\n",
		    dladm_status2str(err, errmsg));
	}

	(void) dladm_walk_datalink_id(lldpd_dladm_init_cb, dlhp, lldpd,
	    lldpd->lldpd_dlclass, lldpd->lldpd_dlmedia, DLADM_OPT_ACTIVE);

	lldpd->lldpd_dladm = dlhp;
}

static void
lldpd_daemonize_fini(int dfd)
{
	int val = 0;
	int err;

	do {
		err = write(dfd, &val, sizeof (val));
	} while (err == -1 && errno == EINTR);
	VERIFY0(close(dfd));
}

/*
 * This is the main lldpd event loop. Refer to the big theory statement for
 * everything that's supposed to happen here.
 */
static void
lldpd_run(lldpd_t *lldpd)
{
	for (;;) {
		int ret;
		port_event_t pe;
		lldpd_event_t *lep;

		mutex_enter(&lldpd->lldpd_lock);
		if (lldpd->lldpd_teardown == B_TRUE) {
			mutex_exit(&lldpd->lldpd_lock);
			return;
		}
		mutex_exit(&lldpd->lldpd_lock);

		ret = port_get(lldpd->lldpd_port, &pe, NULL);
		if (ret != 0) {
			switch (errno) {
			case EFAULT:
			case EBADF:
			case EBADFD:
			case EINVAL:
				lldpd_abort("unexpected port_get error");
			default:
				/*
				 * This means that we in theory got EINTR or
				 * ETIME. While there's no reason that we should
				 * get ETIME, we may get EINTR thanks to a
				 * debugger or other tool being on the scene. We
				 * definitely shouldn't abort in that case.
				 */
				continue;
			}
		}

		VERIFY3P(pe.portev_user, !=, NULL);
		lep = (lldpd_event_t *)pe.portev_user;
		lep->le_func(lldpd, &pe, lep->le_arg);
	}
}

static void
lldpd_base_fini(lldpd_t *lldpd)
{
	VERIFY3S(lldpd->lldpd_dirfd, ==, -1);
	VERIFY3S(lldpd->lldpd_port, ==, -1);
	VERIFY3S(lldpd->lldpd_sigfd, ==, -1);
	VERIFY0(mutex_destroy(&lldpd->lldpd_lock));
}

static void
lldpd_base_init(lldpd_t *lldpd)
{
	bzero(lldpd, sizeof (lldpd_t));

	lldpd->lldpd_dlclass = DATALINK_CLASS_PHYS;
	lldpd->lldpd_dlmedia = DL_ETHER;
	lldpd->lldpd_dirfd = -1;
	lldpd->lldpd_port = -1;
	lldpd->lldpd_sigfd = -1;
	VERIFY0(mutex_init(&lldpd->lldpd_lock, USYNC_THREAD | LOCK_ERRORCHECK,
	    NULL));
	VERIFY0(gethostname(lldpd->lldpd_hostname, sizeof (lldpd->lldpd_hostname)));
}

int
main(int argc, char *argv[])
{
	int dfd = -1;
	lldpd_t *lldpd = &lldpd_state;

	lldpd_progname = basename(argv[0]);

	lldpd_base_init(lldpd);
	dfd = lldpd_daemonize(lldpd);
	lldpd_event_init(lldpd, dfd);
	lldpd_timer_init(lldpd, dfd);
	lldpd_singal_init(lldpd, dfd);
	lldpd_dladm_init(lldpd, dfd);
	lldpd_daemonize_fini(dfd);
	dfd = -1;

	lldpd_run(lldpd);

	lldpd_door_fini(lldpd);
	lldpd_event_fini(lldpd);
	lldpd_dladm_fini(lldpd);
	lldpd_signal_fini(lldpd);
	lldpd_timer_fini(lldpd);
	lldpd_dir_fini(lldpd);
	lldpd_base_fini(lldpd);

	exit(LLDPD_EXIT_REQUESTED);
}
