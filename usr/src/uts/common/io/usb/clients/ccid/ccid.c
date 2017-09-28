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
 * Copyright (c) 2017, Joyent, Inc. 
 */

/*
 * USB CCID class driver
 *
 * Slot Detection
 * --------------
 *
 * A CCID reader has one or more slots, each of which may or may not have a card
 * present. Some devices actually have a card that's permanently plugged in
 * while other readers allow for cards to be inserted and removed. We model all
 * CCID readers that don't have removable cards as ones that are removable, but
 * never fire any events. Removable devices are required to have an Interrupt-IN
 * pipe.
 *
 * Each slot starts in an unknown state. After attaching we always kick off a
 * discovery. When a change event comes in, that causes us to kick off a
 * discovery again, though we focus it only on those endpoints that have noted a
 * change. At attach time we logically mark that every endpoint has changed,
 * allowing us to figure out what its actual state is. We don't rely on any
 * initial Interrupt-IN polling to allow for the case where either the hardware
 * doesn't report it or to better handle the devices without an Interrupt-IN
 * entry. Just because we open up the Interupt-IN pipe, hardware is not
 * obligated to tell us, as the adding and removing a driver will not cause a
 * power cycle.
 *
 * The Interrupt-IN exception callback may need to restart polling. In addition,
 * we may fail to start or restart polling due to a transient issue. In cases
 * where the attempt to start polling has failed, we try again in one second
 * with a timeout.
 *
 * Discovery is run through a taskq. The various slots are checked serially. If
 * a discovery is running when another change event comes in, we flag ourselves
 * for a follow up run. This means that it's possible that we end up processing
 * items early and that the follow up run is ignored.
 *
 * Two state flags are used to keep track of this dance:
 * CCID_FLAG_WORKER_REQUESTED and CCID_FLAG_WORKER_RUNNING. The first is used
 * to indicate that discovery is desired. The second is to indicate that it is
 * actively running. When discovery is requested, the caller first checks to
 * make sure the current flags. If neither flag is set, then it knows that it
 * can kick off discovery. Regardless if it can kick off the taskq, it always
 * sets requested. Once the taskq entry starts, it removes any
 * DISCOVER_REQUESTED flags and sets DISCOVER_RUNNING. If at the end of
 * discovery, we find that another request has been made, the discovery function
 * will kick off another entry in the taskq.
 *
 * The one possible problem with this model is that it means that we aren't
 * throttling the set of incoming requests with respect to taskq dispatches.
 * However, because these are only driven by an Interrupt-IN pipe, it is hoped
 * that the frequency will be rather reduced. If it turns out that that's not
 * the case, we may need to use a timeout or another trick to ensure that only
 * one discovery per tick or so is initialized. The main reason we don't just do
 * that off the bat and add a delay is because of contactless cards which may
 * need to be acted upon in a soft real-time fashion.
 *
 * Command Handling
 * ----------------
 *
 * Commands are issued to a CCID reader on a Bulk-OUT pipe. Responses are
 * generated as a series of one or more messages on a Bulk-IN pipe. To correlate
 * these commands a sequence number is used. This sequence number is one byte
 * and can be in the range [ CCID_SEQ_MIN, CCID_SEQ_MAX ]. To keep track of the
 * allocatd IDs we leverage an ID space.
 *
 * A CCID reader contains a number of slots. Each slot can be addressed
 * separately as each slot represents a separate place that a card may be
 * inserted or not. A given slot may only have a single outstanding command. A
 * given CCID reader may only have a number of commands outstanding to the CCID
 * device as a whole based on a value in the class descriptor (see the
 * ccd_bMacCCIDBusySlots member of the ccid_class_descr_t).
 *
 * To simplify the driver, we only support issuing a single command to a CCID
 * reader at any given time. All commands that are outstanding are queued in a
 * global list ccid_command_queue. The head of the queue is the current command
 * that we believe is outstanding to the reader or will be shortly. The command
 * is issued by sending a Bulk-OUT request with a CCID header. Once we have the
 * Bulk-OUT request acknowledged, we begin sending Bulk-IN messages to the
 * controller. Once the Bulk-IN message is acknowledged, then we complete the
 * command proceed to the next command. This is summarized in the following
 * state machine:
 *
 * XXX
 */

/*
 * Various XXX:
 *
 * o If hardware says that the ICC became shut down / disactivated. Should we
 * explicitly reactivate it as part of something or just make that a future
 * error?
 *  - Should we provide an ioctl to try to reactivate?
 */

#include <sys/modctl.h>
#include <sys/errno.h>
#include <sys/conf.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/cmn_err.h>
#include <sys/sysmacros.h>
#include <sys/stream.h>
#include <sys/strsun.h>
#include <sys/strsubr.h>

#define	USBDRV_MAJOR_VER	2
#define	USBDRV_MINOR_VER	0
#include <sys/usb/usba.h>
#include <sys/usb/usba/usbai_private.h>
#include <sys/usb/clients/ccid/ccid.h>
#include <sys/usb/clients/ccid/uccid.h>

/*
 * Set the amount of parallelism we'll want to have from kernel threads which
 * are processing CCID requests. This is used to size the number of asynchronous
 * requests in the pipe policy. A single command can only ever be outstanding to
 * a single slot. However, multiple slots may potentially be able to be
 * scheduled in parallel. However, we don't actually support this at all and
 * we'll only ever issue a single command. This basically covers the ability to
 * have some other asynchronous operation outstanding if needed.
 */
#define	CCID_NUM_ASYNC_REQS	2

/*
 * Pick a number of default responses to have queued up with the device. This is
 * an arbitrary number. It was picked based on the theory that we don't want too
 * much overhead per-device, but we'd like at least a few. Even if this is
 * broken up into multiple chunks of DMA memory in the controller, it's unlikely
 * to exceed the ring size.
 *
 * We have double this amount allocated sitting around in the 
 */
#define	CCID_BULK_NRESPONSES		2
#define	CCID_BULK_NALLOCED		8

/*
 * XXX This is a time in seconds for the bulk-out command to run and be
 * submitted. We'll need to evaluate this and see if it actually makes sense.
 */
#define	CCID_BULK_OUT_TIMEOUT	5
#define	CCID_BULK_IN_TIMEOUT	5

/*
 * There are two different Interrupt-IN packets that we might receive. The
 * first, RDR_to_PC_HardwareError, is a fixed four byte packet. However, the
 * other one, RDR_to_PC_NotifySlotChange, varies in size as it has two bits per
 * potential slot plus one byte that's always used. The maximum number of slots
 * in a device is 256. This means there can be up to 64 bytes worth of data plus
 * the extra byte, so 65 bytes.
 */
#define	CCID_INTR_RESPONSE_SIZE	65

/*
 * Minimum and maximum minor ids. We treat the maximum valid 32-bit minor as
 * what we can use due to issues in some file systems and the minors that they
 * can use. We reserved zero as an invalid minor number to make it easier to
 * tell if things have been initailized or not.
 */
#define	CCID_MINOR_MIN		1
#define	CCID_MINOR_MAX		MAXMIN32
#define	CCID_MINOR_INVALID	0

/*
 * This structure is used to map between the global set of minor numbers and the
 * things represented by them.
 */
typedef struct ccid_minor_idx {
	id_t cmi_minor;
	avl_node_t cmi_avl;
	boolean_t cmi_isslot;
	union {
		struct ccid_slot *cmi_slot;
		struct ccid_minor *cmi_user;
	} cmi_data;
} ccid_minor_idx_t;

typedef enum ccid_minor_flags {
	CCID_MINOR_F_WAITING		= 1 << 0,
	CCID_MINOR_F_HAS_EXCL		= 1 << 1,
	CCID_MINOR_F_TXN_RESET		= 1 << 2,
} ccid_minor_flags_t;

typedef struct ccid_minor {
	ccid_minor_idx_t	cm_idx;		/* WO */
	cred_t			*cm_opener;	/* WO */
	struct ccid_slot	*cm_slot;	/* WO */
	list_node_t		cm_minor_list;
	list_node_t		cm_excl_list;
	kcondvar_t		cm_read_cv;
	kcondvar_t		cm_excl_cv;
	ccid_minor_flags_t	cm_flags;
	struct pollhead		cm_pollhead;
} ccid_minor_t;

typedef enum ccid_slot_flags {
	CCID_SLOT_F_CHANGED		= 1 << 0,
	CCID_SLOT_F_INTR_GONE		= 1 << 1,
	CCID_SLOT_F_INTR_ADD		= 1 << 2,
	CCID_SLOT_F_PRESENT		= 1 << 3,
	CCID_SLOT_F_ACTIVE		= 1 << 4,
	CCID_SLOT_F_NEED_TXN_RESET	= 1 << 5
} ccid_slot_flags_t;

#define	CCID_SLOT_F_INTR_MASK	(CCID_SLOT_F_CHANGED | CCID_SLOT_F_INTR_GONE | \
    CCID_SLOT_F_INTR_ADD)
#define	CCID_SLOT_F_WORK_MASK	(CCID_SLOT_F_INTR_MASK | \
    CCID_SLOT_F_NEED_TXN_RESET)

typedef struct ccid_slot {
	ccid_minor_idx_t	cs_idx;		/* WO */
	uint_t			cs_slotno;	/* WO */
	struct ccid		*cs_ccid;	/* WO */
	ccid_slot_flags_t	cs_flags;
	ccid_class_voltage_t	cs_voltage;
	mblk_t			*cs_atr;
	struct ccid_command	*cs_command;
	ccid_minor_t		*cs_excl_minor;
	list_t			cs_excl_waiters;
	list_t			cs_minors;
} ccid_slot_t;

typedef enum ccid_attach_state {
	CCID_ATTACH_USB_CLIENT	= 1 << 0,
	CCID_ATTACH_MUTEX_INIT	= 1 << 1,
	CCID_ATTACH_TASKQ	= 1 << 2,
	CCID_ATTACH_CMD_LIST	= 1 << 3,
	CCID_ATTACH_OPEN_PIPES	= 1 << 4,
	CCID_ATTACH_SEQ_IDS	= 1 << 5,
	CCID_ATTACH_SLOTS	= 1 << 6,
	CCID_ATTACH_HOTPLUG_CB	= 1 << 7,
	CCID_ATTACH_INTR_ACTIVE	= 1 << 8,
	CCID_ATTACH_MINORS	= 1 << 9,
} ccid_attach_state_t;

typedef enum ccid_flags {
	CCID_FLAG_HAS_INTR		= 1 << 0,
	CCID_FLAG_DETACHING		= 1 << 1,
	CCID_FLAG_WORKER_REQUESTED	= 1 << 2,
	CCID_FLAG_WORKER_RUNNING	= 1 << 3,
	CCID_FLAG_DISCONNECTED		= 1 << 4
} ccid_flags_t;

#define	CCID_FLAG_WORKER_MASK	(CCID_FLAG_WORKER_REQUESTED | \
    CCID_FLAG_WORKER_RUNNING)

typedef struct ccid_stats {
	uint64_t	cst_intr_errs;
	uint64_t	cst_intr_restart;
	uint64_t	cst_intr_unknown;
	uint64_t	cst_intr_slot_change;
	uint64_t	cst_intr_hwerr;
	uint64_t	cst_intr_inval;
	uint64_t	cst_ndiscover;
	hrtime_t	cst_lastdiscover;
} ccid_stats_t;

typedef struct ccid {
	dev_info_t		*ccid_dip;
	kmutex_t		ccid_mutex;
	ccid_attach_state_t	ccid_attach;
	ccid_flags_t		ccid_flags;
	id_space_t		*ccid_seqs;
	ddi_taskq_t		*ccid_taskq;
	usb_client_dev_data_t	*ccid_dev_data;
	ccid_class_descr_t	ccid_class;		/* WO */
	usb_ep_xdescr_t		ccid_bulkin_xdesc;	/* WO */
	usb_pipe_handle_t	ccid_bulkin_pipe;	/* WO */
	usb_ep_xdescr_t		ccid_bulkout_xdesc;	/* WO */
	usb_pipe_handle_t	ccid_bulkout_pipe;	/* WO */
	usb_ep_xdescr_t		ccid_intrin_xdesc;	/* WO */
	usb_pipe_handle_t	ccid_intrin_pipe;	/* WO */
	usb_pipe_handle_t	ccid_control_pipe;	/* WO */
	uint_t			ccid_nslots;		/* WO */
	size_t			ccid_bufsize;		/* WO */
	ccid_slot_t		*ccid_slots;
	timeout_id_t		ccid_poll_timeout;
	ccid_stats_t		ccid_stats;
	list_t			ccid_command_queue;
	list_t			ccid_complete_queue;
	usb_bulk_req_t		*ccid_bulkin_cache[CCID_BULK_NALLOCED];
	uint_t			ccid_bulkin_alloced;
	usb_bulk_req_t		*ccid_bulkin_dispatched;
} ccid_t;

/*
 * Command structure for an individual CCID command that we issue to a
 * controller. Note that the command caches a copy of some of the data that's
 * normally inside the CCID header in host-endian fashion.
 */
typedef enum ccid_command_state {
	CCID_COMMAND_ALLOCATED	= 0x0,
	CCID_COMMAND_QUEUED,
	CCID_COMMAND_DISPATCHED,
	CCID_COMMAND_REPLYING,
	CCID_COMMAND_COMPLETE,
	CCID_COMMAND_TRANSPORT_ERROR,
	CCID_COMMAND_CCID_ABORTED
} ccid_command_state_t;

typedef enum ccid_command_flags {
	CCID_COMMAND_F_USER	= 1 << 0,
	CCID_COMMAND_F_ABANDON	= 1 << 1,
} ccid_command_flags_t;

typedef struct ccid_command {
	list_node_t		cc_list_node;
	kcondvar_t		cc_cv;
	uint8_t			cc_mtype;
	uint8_t			cc_slot;
	ccid_command_state_t	cc_state;
	ccid_command_flags_t	cc_flags;
	int			cc_usb;
	usb_cr_t		cc_usbcr;
	size_t			cc_reqlen;
	id_t			cc_seq;
	boolean_t		cc_isuser;
	usb_bulk_req_t		*cc_ubrp;
	ccid_t			*cc_ccid;
	hrtime_t		cc_queue_time;
	hrtime_t		cc_dispatch_time;
	hrtime_t		cc_dispatch_cb_time;
	hrtime_t		cc_response_time;
	hrtime_t		cc_completion_time;
	mblk_t			*cc_response;
} ccid_command_t;

/*
 * ddi_soft_state(9F) pointer. This is used for instances of a CCID controller.
 */
static void *ccid_softstate;

/*
 * This is used to keep track of our minor nodes. We have two different kinds of
 * minor nodes. The first are CCID slots. The second are cloned opens of those
 * slots. Each of these items has a ccid_minor_idx_t embedded in them that is
 * used to index them in an AVL tree. Given that the number of entries that
 * should be present here is unlikely to be terribly large at any given time, it
 * is hoped that an AVL tree will suffice for now.
 */
static kmutex_t ccid_idxlock;
static avl_tree_t ccid_idx;
static id_space_t *ccid_minors;

/*
 * Required Forwards
 */
static void ccid_intr_poll_init(ccid_t *);
static void ccid_worker_request(ccid_t *);
static void ccid_command_dispatch(ccid_t *);
static void ccid_command_free(ccid_command_t *);
static int ccid_bulkin_schedule(ccid_t *);

static int
ccid_idx_comparator(const void *l, const void *r)
{
	const ccid_minor_idx_t *lc = l, *rc = r;

	if (lc->cmi_minor > rc->cmi_minor)
		return (1);
	if (lc->cmi_minor < rc->cmi_minor)
		return (-1);
	return (0);
}

static void
ccid_error(ccid_t *ccid, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (ccid != NULL) {
		vdev_err(ccid->ccid_dip, CE_WARN, fmt, ap);
	} else {
		vcmn_err(CE_WARN, fmt, ap);
	}
	va_end(ap);
}

static void
ccid_minor_idx_free(ccid_minor_idx_t *idx)
{
	ccid_minor_idx_t *ip;

	VERIFY3S(idx->cmi_minor, !=, CCID_MINOR_INVALID);
	mutex_enter(&ccid_idxlock);
	ip = avl_find(&ccid_idx, idx, NULL);
	VERIFY3P(idx, ==, ip);
	avl_remove(&ccid_idx, idx);
	id_free(ccid_minors, idx->cmi_minor);
	idx->cmi_minor = CCID_MINOR_INVALID;
	mutex_exit(&ccid_idxlock);
}

static boolean_t
ccid_minor_idx_alloc(ccid_minor_idx_t *idx, boolean_t sleep)
{
	id_t id;
	ccid_minor_idx_t *ip;
	avl_index_t where;

	mutex_enter(&ccid_idxlock);
	if (sleep) {
		id = id_alloc(ccid_minors);
	} else {
		id = id_alloc_nosleep(ccid_minors);
	}
	if (id == -1) {
		mutex_exit(&ccid_idxlock);
		return (B_FALSE);
	}
	idx->cmi_minor = id;
	ip = avl_find(&ccid_idx, idx, &where);
	VERIFY3P(ip, ==, NULL);
	avl_insert(&ccid_idx, idx, where);
	mutex_exit(&ccid_idxlock);

	return (B_TRUE);
}

static ccid_minor_idx_t *
ccid_minor_find(minor_t m)
{
	ccid_minor_idx_t i = { 0 };
	ccid_minor_idx_t *ret;

	i.cmi_minor = m;
	mutex_enter(&ccid_idxlock);
	ret = avl_find(&ccid_idx, &i, NULL);
	mutex_exit(&ccid_idxlock);

	return (ret);
}

static ccid_minor_idx_t *
ccid_minor_find_user(minor_t m)
{
	ccid_minor_idx_t *idx;

	idx = ccid_minor_find(m);
	if (idx == NULL) {
		return (NULL);
	}
	ASSERT0(idx->cmi_isslot);
	if (idx->cmi_isslot)
		return (NULL);
	return (idx);
}

static void
ccid_slot_excl_signal(ccid_slot_t *slot)
{
	ccid_minor_t *cmp;

	VERIFY(MUTEX_HELD(&slot->cs_ccid->ccid_mutex));
	VERIFY3P(slot->cs_excl_minor, ==, NULL);
	VERIFY0(slot->cs_flags & CCID_SLOT_F_NEED_TXN_RESET);


	cmp = list_head(&slot->cs_excl_waiters);
	if (cmp == NULL)
		return;
	cv_signal(&cmp->cm_excl_cv);
}

static void
ccid_slot_excl_rele(ccid_slot_t *slot)
{
	ccid_minor_t *cmp;
	ccid_t *ccid = slot->cs_ccid;

	VERIFY(MUTEX_HELD(&ccid->ccid_mutex));
	VERIFY3P(slot->cs_excl_minor, !=, NULL);

	cmp = slot->cs_excl_minor;
	cmp->cm_flags &= ~CCID_MINOR_F_HAS_EXCL;
	slot->cs_excl_minor = NULL;

	/*
	 * If we have an outstanding command left by the user when they've
	 * closed the slot, we need to clean up this command. If the command has
	 * completed, as in the user never called read, then we can simply free
	 * this command. Otherwise, we must tag this command as being abandoned,
	 * which will cause it to be cleaned up when the command is completed.
	 * This does mean that if a user releases the slot and then someone else
	 * gets it, they may not be able to write initially as the slot is still
	 * busy.
	 */
	if (slot->cs_command != NULL) {
		if (slot->cs_command->cc_state < CCID_COMMAND_COMPLETE) {
			slot->cs_command->cc_flags |= CCID_COMMAND_F_ABANDON;
		} else {
			ccid_command_free(slot->cs_command);
			slot->cs_command = NULL;
		}
	}

	/*
	 * Regardless of when we're polling, we need to go through and error
	 * out.
	 */
	pollwakeup(&cmp->cm_pollhead, POLLERR);

	/*
	 * If we've been asked to reset the device before handing it off,
	 * schedule that. Otherwise, allow the next entry in the queue to get
	 * woken up and given access to the device.
	 */
	if (cmp->cm_flags & CCID_MINOR_F_TXN_RESET) {
		slot->cs_flags |= CCID_SLOT_F_NEED_TXN_RESET;
		ccid_worker_request(ccid);
	} else {
		ccid_slot_excl_signal(slot);
	}
}

static int
ccid_slot_excl_req(ccid_slot_t *slot, ccid_minor_t *cmp, boolean_t nosleep)
{
	ccid_minor_t *check;

	VERIFY(MUTEX_HELD(&slot->cs_ccid->ccid_mutex));

	if (slot->cs_excl_minor == cmp) {
		VERIFY(cmp->cm_flags & CCID_MINOR_F_HAS_EXCL);
		return (EEXIST);
	}

	if (cmp->cm_flags & CCID_MINOR_F_WAITING) {
		return (EINPROGRESS);
	}

	/*
	 * If we were asked to try and fail quickly, do that before the main
	 * loop.
	 */
	if (nosleep && slot->cs_excl_minor != NULL &&
	    !(slot->cs_flags & CCID_SLOT_F_NEED_TXN_RESET)) {
		return (EBUSY);
	}

	/*
	 * Mark that we're waiting in case we race with another thread trying to
	 * claim exclusive access for this. Insert ourselves on the wait list.
	 * If for some reason we get a signal, then we can't know for certain if
	 * we had a signal / cv race. In such a case, we always wake up the
	 * next person in the queue (potentially spuriously).
	 */
	cmp->cm_flags |= CCID_MINOR_F_WAITING;
	list_insert_tail(&slot->cs_excl_waiters, cmp);
	while (slot->cs_excl_minor != NULL ||
	    slot->cs_flags & CCID_SLOT_F_NEED_TXN_RESET) {
		if (cv_wait_sig(&cmp->cm_excl_cv, &slot->cs_ccid->ccid_mutex) ==
		    0) {
			list_remove(&slot->cs_excl_waiters, cmp);
			cmp->cm_flags &= ~CCID_MINOR_F_WAITING;
			ccid_slot_excl_signal(slot);
			return (EINTR);
		}
	}

	VERIFY3P(cmp, ==, list_head(&slot->cs_excl_waiters));
	VERIFY0(slot->cs_flags & CCID_SLOT_F_NEED_TXN_RESET);
	list_remove(&slot->cs_excl_waiters, cmp);

	cmp->cm_flags &= ~CCID_MINOR_F_WAITING;
	cmp->cm_flags |= CCID_MINOR_F_HAS_EXCL;
	slot->cs_excl_minor = cmp;
	return (0);
}

/*
 * XXX This will probably need to change when we start doing TPDU processing.
 */
static size_t
ccid_command_resp_length(ccid_command_t *cc)
{
	uint32_t len;
	const ccid_header_t *cch;

	VERIFY3P(cc, !=, NULL);
	VERIFY3P(cc->cc_response, !=, NULL);

	/*
	 * Fetch out an arbitrarily aligned LE uint32_t value from the header.
	 */
	cch = (ccid_header_t *)cc->cc_response->b_rptr;
	bcopy(&cch->ch_length, &len, sizeof (len));
	len = LE_32(len);
	return (len);
}

static void
ccid_command_complete_user(ccid_t *ccid, ccid_command_t *cc)
{
	ccid_slot_t *slot;
	ccid_minor_t *cmp;

	VERIFY(MUTEX_HELD(&ccid->ccid_mutex));

	slot = &ccid->ccid_slots[cc->cc_slot];
	VERIFY3P(slot->cs_command, ==, cc);

	/*
	 * If we somehow lost the minor associated with this command, free it.
	 */
	cmp = slot->cs_excl_minor;
	if (cmp == NULL) {
		slot->cs_command = NULL;
		ccid_command_free(cc);
		return;
	}

	/*
	 * If this was abandoned, free the command and signal that the slot is
	 * writable again.
	 */
	if (cc->cc_flags & CCID_COMMAND_F_ABANDON) {
		slot->cs_command = NULL;
		ccid_command_free(cc);
		pollwakeup(&cmp->cm_pollhead, POLLOUT);
		return;
	}

	/*
	 * Append this to the end of the list and wake up anyone who was blocked
	 * or polling. At this point, we only need to signal readers, but we
	 * need to wake up pollers for read and write.
	 */
	pollwakeup(&cmp->cm_pollhead, POLLIN | POLLRDNORM);
	cv_signal(&cmp->cm_read_cv);
}

/*
 * Complete a single command. The way that a command completes depends on the
 * kind of command that occurs. If this is commad is flagged as a user command,
 * that implies that it must be handled in a different way from administrative
 * commands. User commands are placed into the minor to consume via a read(9E).
 * Non-user commands are placed into a completion queue and must be picked up
 * via the ccid_command_poll() interface.
 */
static void
ccid_command_complete(ccid_command_t *cc)
{
	ccid_t *ccid = cc->cc_ccid;

	VERIFY(MUTEX_HELD(&ccid->ccid_mutex));
	cc->cc_completion_time = gethrtime();
	list_remove(&ccid->ccid_command_queue, cc);

	if (cc->cc_flags & CCID_COMMAND_F_USER) {
		ccid_command_complete_user(ccid, cc);
	} else {
		list_insert_tail(&ccid->ccid_complete_queue, cc);
		cv_broadcast(&cc->cc_cv);
	}

	/*
	 * Finally, we also need to kick off the next command.
	 */
	ccid_command_dispatch(ccid);
}

static void
ccid_command_state_transition(ccid_command_t *cc, ccid_command_state_t state)
{
	VERIFY(MUTEX_HELD(&cc->cc_ccid->ccid_mutex));

	cc->cc_state = state;
	cv_broadcast(&cc->cc_cv);
}

static void
ccid_command_transport_error(ccid_command_t *cc, int usb_status, usb_cr_t cr)
{
	VERIFY(MUTEX_HELD(&cc->cc_ccid->ccid_mutex));

	ccid_command_state_transition(cc, CCID_COMMAND_TRANSPORT_ERROR);
	cc->cc_usb = usb_status;
	cc->cc_usbcr = cr;
	cc->cc_response = NULL;

	ccid_command_complete(cc);
}

static void
ccid_command_status_decode(ccid_command_t *cc, ccid_reply_command_status_t *comp,
    ccid_reply_icc_status_t *iccp, ccid_command_err_t *errp)
{
	ccid_header_t cch;
	size_t mblen;

	VERIFY3S(cc->cc_state, ==, CCID_COMMAND_COMPLETE);
	VERIFY3P(cc->cc_response, !=, NULL);
	mblen = msgsize(cc->cc_response);
	VERIFY3U(mblen, >=, sizeof (cch));

	bcopy(cc->cc_response->b_rptr, &cch, sizeof (cch));
	if (comp != NULL) {
		*comp = CCID_REPLY_STATUS(cch.ch_param0);
	}

	if (iccp != NULL) { 
		*iccp = CCID_REPLY_ICC(cch.ch_param0);
	}

	if (errp != NULL) {
		*errp = cch.ch_param1;
	}
}

static void
ccid_reply_bulk_cb(usb_pipe_handle_t ph, usb_bulk_req_t *ubrp)
{
	size_t mlen;
	ccid_t *ccid;
	ccid_header_t cch;
	ccid_command_t *cc;

	boolean_t header_valid = B_FALSE;

	VERIFY(ubrp->bulk_data != NULL);
	mlen = msgsize(ubrp->bulk_data);
	ccid = (ccid_t *)ubrp->bulk_client_private;
	mutex_enter(&ccid->ccid_mutex);

	/*
	 * Before we do anything else, we should mark that this Bulk-IN request
	 * is no longer being dispatched.
	 */
	VERIFY3P(ubrp, ==, ccid->ccid_bulkin_dispatched);
	ccid->ccid_bulkin_dispatched = NULL;

	if ((cc = list_head(&ccid->ccid_command_queue)) == NULL) {
		/*
		 * This is certainly an odd case. This means that we got some
		 * response but there are no entries in the queue. Go ahead and
		 * free this. We're done here.
		 */
		mutex_exit(&ccid->ccid_mutex);
		usb_free_bulk_req(ubrp);
		return;
	}

	if (mlen >= sizeof (ccid_header_t)) {
		bcopy(ubrp->bulk_data->b_rptr, &cch, sizeof (cch));
		header_valid = B_TRUE;
	}

	/*
	 * If the current command isn't in the replying state, then something is
	 * clearly wrong and this probably isn't intended for the current
	 * command. That said, if we have enough bytes, let's check the sequence
	 * number as that might be indicative of a bug otherwise.
	 */
	if (cc->cc_state != CCID_COMMAND_REPLYING) {
		if (header_valid) {
			VERIFY3S(cch.ch_seq, !=, cc->cc_seq);
		}
		mutex_exit(&ccid->ccid_mutex);
		usb_free_bulk_req(ubrp);
		return;
	}

	/*
	 * CCID section 6.2.7 says that if we get a short or zero length packet,
	 * then we need to treat that as though the running command was aborted
	 * for some reason. However, section 3.1.3 talks about sending zero
	 * length packets on general principle.  To further complicate things,
	 * we don't have the sequence number.
	 *
	 * If we have an outstanding command still, then we opt to treat the
	 * zero length packet as an abort.
	 */
	if (!header_valid) {
		ccid_command_state_transition(cc, CCID_COMMAND_CCID_ABORTED);
		ccid_command_complete(cc);
		mutex_exit(&ccid->ccid_mutex);
		usb_free_bulk_req(ubrp);
		return;
	}

	/*
	 * If the sequence number doesn't match the head of the list then we
	 * should be very suspect of the hardware at this point. At a minimum we
	 * should fail this command, 
	 */
	if (cch.ch_seq != cc->cc_seq) {
		/*
		 * XXX we should fail this command in a way to indicate that
		 * this has happened and figure out how to clean up.
		 */
		mutex_exit(&ccid->ccid_mutex);
		usb_free_bulk_req(ubrp);
		return;
	}

	/*
	 * Check that we have all the bytes that we were told we'd have. If we
	 * dno't, simulate this as an aborted command. XXX is this the right
	 * thing to do?
	 */
	if (LE_32(cch.ch_length) + sizeof (ccid_header_t) > mlen) {
		ccid_command_state_transition(cc, CCID_COMMAND_CCID_ABORTED);
		ccid_command_complete(cc);
		mutex_exit(&ccid->ccid_mutex);
		usb_free_bulk_req(ubrp);
		return;
	}

	/*
	 * This response is for us. Before we complete the command check to see
	 * what the state of the command is. If the command indicates that more
	 * time has been requested, then we need to schedule a new Bulk-IN
	 * request.
	 *
	 * XXX Should we actually just always honor this and not check the
	 * message type?
	 *
	 * XXX What about checking that the slot makes sense?
	 *
	 * XXX What about checking if the thing didn't post us all the bytes
	 * that it said it would
	 */
	if (CCID_REPLY_STATUS(cch.ch_param0) == CCID_REPLY_STATUS_MORE_TIME) {
		int ret;

		ret = ccid_bulkin_schedule(ccid);
		if (ret != USB_SUCCESS) {
			ccid_command_transport_error(cc, ret, USB_CR_OK); 
		}
		mutex_exit(&ccid->ccid_mutex);
		usb_free_bulk_req(ubrp);
		return;
	}

	/*
	 * Take the message block from the Bulk-IN request and store it on the
	 * command. We wnat this regardless if it succeeded, failed, or we have
	 * some unexpected status value.
	 */
	cc->cc_response = ubrp->bulk_data;
	ubrp->bulk_data = NULL;
	ccid_command_state_transition(cc, CCID_COMMAND_COMPLETE);
	ccid_command_complete(cc);
	mutex_exit(&ccid->ccid_mutex);
	usb_free_bulk_req(ubrp);
}

static void
ccid_reply_bulk_exc_cb(usb_pipe_handle_t ph, usb_bulk_req_t *ubrp)
{
	ccid_t *ccid;
	ccid_command_t *cc;

	ccid = (ccid_t *)ubrp->bulk_client_private;
	mutex_enter(&ccid->ccid_mutex);

	/*
	 * Before we do anything else, we should mark that this Bulk-IN request
	 * is no longer being dispatched.
	 */
	VERIFY3P(ubrp, ==, ccid->ccid_bulkin_dispatched);
	ccid->ccid_bulkin_dispatched = NULL;

	/*
	 * While there are many different reasons that the Bulk-IN request could
	 * have failed, each of these are treated as a transport error. If we
	 * have a dispatched command, then we treat this as corresponding to
	 * that command. Otherwise, we drop this.
	 */
	if ((cc = list_head(&ccid->ccid_command_queue)) != NULL) {
		if (cc->cc_state == CCID_COMMAND_REPLYING) {
			ccid_command_transport_error(cc, USB_SUCCESS,
			    ubrp->bulk_completion_reason);
		}
	}
	mutex_exit(&ccid->ccid_mutex);
	usb_free_bulk_req(ubrp);
}

/*
 * Fill the Bulk-IN cache. If we do not entirely fill this, that's fine. If
 * there are no scheduled resources then we'll deal with that when we actually
 * get there.
 */
static void
ccid_bulkin_cache_refresh(ccid_t *ccid)
{
	mutex_enter(&ccid->ccid_mutex);
	while (ccid->ccid_bulkin_alloced < CCID_BULK_NALLOCED) {
		usb_bulk_req_t *ubrp;
		/*
		 * Drop the lock during allocation to make sure we don't block
		 * the rest of the driver needlessly.
		 */
		mutex_exit(&ccid->ccid_mutex);
		ubrp = usb_alloc_bulk_req(ccid->ccid_dip, ccid->ccid_bufsize, 0);
		if (ubrp == NULL)
			return;

		ubrp->bulk_len = ccid->ccid_bufsize;
		ubrp->bulk_timeout = CCID_BULK_IN_TIMEOUT;
		ubrp->bulk_client_private = (usb_opaque_t)ccid;
		ubrp->bulk_attributes = USB_ATTRS_SHORT_XFER_OK |
		    USB_ATTRS_AUTOCLEARING;
		ubrp->bulk_cb = ccid_reply_bulk_cb;
		ubrp->bulk_exc_cb = ccid_reply_bulk_exc_cb;

		mutex_enter(&ccid->ccid_mutex);
		if (ccid->ccid_bulkin_alloced >= CCID_BULK_NALLOCED ||
		    (ccid->ccid_flags & CCID_FLAG_DETACHING)) {
			mutex_exit(&ccid->ccid_mutex);
			usb_free_bulk_req(ubrp);
			return;
		}
		ccid->ccid_bulkin_cache[ccid->ccid_bulkin_alloced] = ubrp;
		ccid->ccid_bulkin_alloced++;
	}

	mutex_exit(&ccid->ccid_mutex);
}

/*
 * Attempt to schedule a Bulk-In request. Note that only one should ever be
 * scheduled at any time.
 */
static int
ccid_bulkin_schedule(ccid_t *ccid)
{
	VERIFY(MUTEX_HELD(&ccid->ccid_mutex));
	if (ccid->ccid_bulkin_dispatched == NULL) {
		usb_bulk_req_t *ubrp;
		int ret;

		/* XXX Maybe try to alloc again? */
		if (ccid->ccid_bulkin_alloced == 0)
			return (USB_NO_RESOURCES);

		ccid->ccid_bulkin_alloced--;
		ubrp = ccid->ccid_bulkin_cache[ccid->ccid_bulkin_alloced];
		VERIFY3P(ubrp, !=, NULL);
		ccid->ccid_bulkin_cache[ccid->ccid_bulkin_alloced] = NULL;

		if ((ret = usb_pipe_bulk_xfer(ccid->ccid_bulkin_pipe, ubrp,
		    0)) != USB_SUCCESS) {
			ccid_error(ccid, "failed to schedule Bulk-In response: %d", ret);
			ccid->ccid_bulkin_cache[ccid->ccid_bulkin_alloced] =
			    ubrp;
			ccid->ccid_bulkin_alloced++;
			return (ret);
		}

		ccid->ccid_bulkin_dispatched = ubrp;
	}

	return (USB_SUCCESS);
}

/*
 * Make sure that the head of the queue has been dispatched. If a dispatch to
 * the device fails, fail the command and try the next one.
 */
static void
ccid_command_dispatch(ccid_t *ccid)
{
	ccid_command_t *cc;

	VERIFY(MUTEX_HELD(&ccid->ccid_mutex));
	while ((cc = list_head(&ccid->ccid_command_queue)) != NULL) {
		int ret;

		if (ccid->ccid_flags & CCID_FLAG_DETACHING)
			return;

		/*
		 * Head of the queue is already being processed. We're done
		 * here.
		 */
		if (cc->cc_state > CCID_COMMAND_QUEUED) {
			return;
		}

		/*
		 * Mark the command as being dispatched to the device. This
		 * prevents anyone else from getting in and confusing things.
		 */
		ccid_command_state_transition(cc, CCID_COMMAND_DISPATCHED);
		cc->cc_dispatch_time = gethrtime();

		/*
		 * Drop the global lock while we schedule the USB I/O. 
		 */
		mutex_exit(&ccid->ccid_mutex);

		ret = usb_pipe_bulk_xfer(ccid->ccid_bulkout_pipe, cc->cc_ubrp,
		    0);
		mutex_enter(&ccid->ccid_mutex);
		if (ret != USB_SUCCESS) {
			/*
			 * We don't need to free the usb_bulk_req_t here as it
			 * will be taken care of when the command itself is
			 * freed.
			 */
			ccid_error(ccid, "Bulk pipe dispatch failed: %d\n", ret);
			ccid_command_transport_error(cc, ret, USB_CR_OK);
		}
	}
}

static int
ccid_command_queue(ccid_t *ccid, ccid_command_t *cc)
{
	id_t seq;
	ccid_header_t *cchead;

	/*
	 * When queueing this command, go ahead and make sure our reply cache is
	 * full.
	 */
	ccid_bulkin_cache_refresh(ccid);

	seq = id_alloc_nosleep(ccid->ccid_seqs);
	if (seq == -1)
		return (ENOMEM);
	cc->cc_seq = seq;
	VERIFY(seq <= UINT8_MAX);
	cchead = (void *)cc->cc_ubrp->bulk_data->b_rptr;
	cchead->ch_seq = (uint8_t)seq;
	mutex_enter(&ccid->ccid_mutex);
	list_insert_tail(&ccid->ccid_command_queue, cc);
	ccid_command_state_transition(cc, CCID_COMMAND_QUEUED);
	cc->cc_queue_time = gethrtime();
	ccid_command_dispatch(ccid);
	mutex_exit(&ccid->ccid_mutex);

	return (0);
}

/*
 * Normal callback for Bulk-Out requests which represents commands issued to the
 * device.
 */
static void
ccid_dispatch_bulk_cb(usb_pipe_handle_t ph, usb_bulk_req_t *ubrp)
{
	int ret;
	ccid_command_t *cc = (void *)ubrp->bulk_client_private;
	ccid_t *ccid = cc->cc_ccid;

	mutex_enter(&ccid->ccid_mutex);
	VERIFY3S(cc->cc_state, ==, CCID_COMMAND_DISPATCHED);
	ccid_command_state_transition(cc, CCID_COMMAND_REPLYING);
	cc->cc_dispatch_cb_time = gethrtime();

	/*
	 * Since we have successfully sent the command, give it a Bulk-In
	 * response to reply to us with. If that fails, we'll note a transport
	 * error which will kick off the next command if needed.
	 */
	ret = ccid_bulkin_schedule(ccid);
	if (ret != USB_SUCCESS) {
		ccid_command_transport_error(cc, ret, USB_CR_OK); 
	}
	mutex_exit(&ccid->ccid_mutex);
}

/*
 * Exception callback for the Bulk-Out requests which represent commands issued
 * to the device.
 */
static void
ccid_dispatch_bulk_exc_cb(usb_pipe_handle_t ph, usb_bulk_req_t *ubrp)
{
	ccid_command_t *cc = (void *)ubrp->bulk_client_private;
	ccid_t *ccid = cc->cc_ccid;

	mutex_enter(&ccid->ccid_mutex);
	ccid_command_transport_error(cc, USB_SUCCESS,
	    ubrp->bulk_completion_reason);
	mutex_exit(&ccid->ccid_mutex);
}

static void
ccid_command_free(ccid_command_t *cc)
{
	VERIFY0(list_link_active(&cc->cc_list_node));
	VERIFY(cc->cc_state == CCID_COMMAND_ALLOCATED ||
	    cc->cc_state >= CCID_COMMAND_COMPLETE);

	if (cc->cc_response != NULL) {
		freemsgchain(cc->cc_response);
		cc->cc_response = NULL;
	}

	if (cc->cc_ubrp != NULL) {
		usb_free_bulk_req(cc->cc_ubrp);
		cc->cc_ubrp = NULL;
	}

	if (cc->cc_seq != 0) {
		id_free(cc->cc_ccid->ccid_seqs, cc->cc_seq);
		cc->cc_seq = 0;
	}

	cv_destroy(&cc->cc_cv);
	kmem_free(cc, sizeof (ccid_command_t));
}

/*
 * Allocate a command of a specific size and parameters. This will allocate a
 * USB bulk transfer that the caller will copy data to.
 */
static int
ccid_command_alloc(ccid_t *ccid, ccid_slot_t *slot, boolean_t block,
    mblk_t *datamp, size_t datasz, uint8_t mtype, uint8_t param0,
    uint8_t param1, uint8_t param2, ccid_command_t **ccp)
{
	size_t allocsz;
	int kmflag, usbflag;
	ccid_command_t *cc;
	ccid_header_t *cchead;

	switch (mtype) {
	case CCID_REQUEST_POWER_ON:
	case CCID_REQUEST_POWER_OFF:
	case CCID_REQUEST_SLOT_STATUS:
	case CCID_REQUEST_GET_PARAMS:
	case CCID_REQUEST_RESET_PARAMS:
	case CCID_REQUEST_ICC_CLOCK:
	case CCID_REQUEST_T0APDU:
	case CCID_REQUEST_MECHANICAL:
	case CCID_REQEUST_ABORT:
		if (datasz != 0)
			return (EINVAL);
		break;
	case CCID_REQUEST_TRANSFER_BLOCK:
	case CCID_REQUEST_ESCAPE:
	case CCID_REQUEST_SECURE:
	case CCID_REQUEST_SET_PARAMS:
	case CCID_REQUEST_DATA_CLOCK:
		break;
	default:
		return (EINVAL);
	}

	if (block) {
		kmflag = KM_SLEEP;
		usbflag = USB_FLAGS_SLEEP;
	} else {
		kmflag = KM_NOSLEEP | KM_NORMALPRI;
		usbflag = 0;
	}

	if (datasz + sizeof (ccid_header_t) < datasz)
		return (EINVAL);
	if (datasz > ccid->ccid_bufsize)
		return (EINVAL);

	cc = kmem_zalloc(sizeof (ccid_command_t), kmflag);
	if (cc == NULL)
		return (ENOMEM);

	allocsz = datasz + sizeof (ccid_header_t);
	if (datamp == NULL) {
		cc->cc_ubrp = usb_alloc_bulk_req(ccid->ccid_dip, allocsz, usbflag);
	} else {
		cc->cc_ubrp = usb_alloc_bulk_req(ccid->ccid_dip, 0, usbflag);
	}
	if (cc->cc_ubrp == NULL) {
		kmem_free(cc, sizeof (ccid_command_t));
		return (ENOMEM);
	}

	list_link_init(&cc->cc_list_node);
	cv_init(&cc->cc_cv, NULL, CV_DRIVER, NULL);
	cc->cc_mtype = mtype;
	cc->cc_slot = slot->cs_slotno;
	cc->cc_reqlen = datasz;
	cc->cc_ccid = ccid;
	cc->cc_state = CCID_COMMAND_ALLOCATED;

	/*
	 * Fill in bulk request attributes. Note that short transfers out
	 * are not OK.
	 */
	if (datamp != NULL) {
		cc->cc_ubrp->bulk_data = datamp;
	}
	cc->cc_ubrp->bulk_len = allocsz;
	cc->cc_ubrp->bulk_timeout = CCID_BULK_OUT_TIMEOUT;
	cc->cc_ubrp->bulk_client_private = (usb_opaque_t)cc;
	cc->cc_ubrp->bulk_attributes = USB_ATTRS_AUTOCLEARING;
	cc->cc_ubrp->bulk_cb = ccid_dispatch_bulk_cb;
	cc->cc_ubrp->bulk_exc_cb = ccid_dispatch_bulk_exc_cb;

	/*
	 * Fill in the command header. We fill in everything except the sequence
	 * number, which is done by the actual dispatch code.
	 */
	cchead = (void *)cc->cc_ubrp->bulk_data->b_rptr;
	cchead->ch_mtype = mtype;
	cchead->ch_length = LE_32(datasz);
	cchead->ch_slot = slot->cs_slotno;
	cchead->ch_seq = 0;
	cchead->ch_param0 = param0;
	cchead->ch_param1 = param1;
	cchead->ch_param2 = param2;
	*ccp = cc;
	return (0);
}

/*
 * The rest of the stack is in charge of timing out commands and potentially
 * aborting them. At this point in time, there's no specific timeout aspect
 * here.
 */
static void
ccid_command_poll(ccid_t *ccid, ccid_command_t *cc)
{
	VERIFY0(cc->cc_flags & CCID_COMMAND_F_USER);

	mutex_enter(&ccid->ccid_mutex);
	while (cc->cc_state < CCID_COMMAND_COMPLETE) {
		cv_wait(&cc->cc_cv, &ccid->ccid_mutex);
	}

	/*
	 * Treat this as a consumption and remove it from the completion list.
	 */
#ifdef DEBUG
	ccid_command_t *check;
	for (check = list_head(&ccid->ccid_complete_queue); check != NULL;
	    check = list_next(&ccid->ccid_complete_queue, check)) {
		if (cc == check)
			break;
	}
	ASSERT3P(check, !=, NULL);
#endif
	VERIFY(list_link_active(&cc->cc_list_node));
	list_remove(&ccid->ccid_complete_queue, cc);
	mutex_exit(&ccid->ccid_mutex);
}

static int
ccid_command_power_off(ccid_t *ccid, ccid_slot_t *cs)
{
	int ret;
	ccid_command_t *cc;
	ccid_reply_icc_status_t cis;
	ccid_reply_command_status_t crs;

	if ((ret = ccid_command_alloc(ccid, cs, B_TRUE, NULL, 0,
	    CCID_REQUEST_POWER_OFF, 0, 0, 0, &cc)) != 0) {
		return (ret);
	}

	if ((ret = ccid_command_queue(ccid, cc)) != 0) {
		ccid_command_free(cc);
		return (ret);
	}

	ccid_command_poll(ccid, cc);

	if (cc->cc_state != CCID_COMMAND_COMPLETE) {
		ret = EIO;
		goto done;
	}

	ccid_command_status_decode(cc, &crs, &cis, NULL);
	if (crs == CCID_REPLY_STATUS_FAILED) {
		if (cis == CCID_REPLY_ICC_MISSING) {
			ret = ENXIO;
		} else {
			ret = EIO;
		}
	} else {
		ret = 0;
	}
done:
	ccid_command_free(cc);
	return (ret);
}

static int
ccid_command_power_on(ccid_t *ccid, ccid_slot_t *cs, ccid_class_voltage_t volt,
    mblk_t **atrp)
{
	int ret;
	ccid_command_t *cc;
	ccid_reply_command_status_t crs;
	ccid_reply_icc_status_t cis;
	ccid_command_err_t cce;

	if (atrp == NULL)
		return (EINVAL);

	*atrp = NULL;

	switch (volt) {
	case CCID_CLASS_VOLT_AUTO:
	case CCID_CLASS_VOLT_5_0:
	case CCID_CLASS_VOLT_3_0:
	case CCID_CLASS_VOLT_1_8:
		break;
	default:
		return (EINVAL);
	}

	if ((ret = ccid_command_alloc(ccid, cs, B_TRUE, NULL, 0,
	    CCID_REQUEST_POWER_ON, volt, 0, 0, &cc)) != 0) {
		return (ret);
	}

	if ((ret = ccid_command_queue(ccid, cc)) != 0) {
		ccid_command_free(cc);
		return (ret);
	}

	ccid_command_poll(ccid, cc);

	if (cc->cc_state != CCID_COMMAND_COMPLETE) {
		ret = EIO;
		goto done;
	}

	/*
	 * XXX Assume slot and message type logic is being done for us. Look for
	 * a few specific errors here:
	 *
	 * - ICC_MUTE via a few potential ways
	 * - Bad voltage
	 */
	ccid_command_status_decode(cc, &crs, &cis, &cce);
	if (crs == CCID_REPLY_STATUS_FAILED) {
		if (cis == CCID_REPLY_ICC_MISSING) {
			ret = ENXIO;
		} else if (cis == CCID_REPLY_ICC_INACTIVE &&
		    cce == 7) {
			/*
			 * This means that byte 7 was invalid. In other words,
			 * that the voltage wasn't correct. See Table 6.1-2
			 * 'Errors' in the CCID r1.1.0 spec.
			 */
			ret = ENOTSUP;
		} else {
			ret = EIO;
		}
	} else {
		size_t len;

		len = ccid_command_resp_length(cc);
		if (len == 0) {
			/*
			 * XXX Could probably use more descriptive errors and
			 * not errnos
			 */
			ret = EINVAL;
			goto done;
		}

#ifdef	DEBUG
		/*
		 * This should have already been checked by the response
		 * framework, but sanity check this again.
		 */
		size_t mlen = msgsize(cc->cc_response);
		VERIFY3U(mlen, >=, len + sizeof (ccid_header_t));
#endif

		/*
		 * Munge the message block to have the ATR. We want to make sure
		 * that the write pointer is set to the maximum length that we
		 * got back from the driver (the message block could strictly
		 * speaking be larger, because we got a larger transfer for some
		 * reason).
		 */
		cc->cc_response->b_rptr += sizeof (ccid_header_t);
		cc->cc_response->b_wptr = cc->cc_response->b_rptr + len;
		*atrp = cc->cc_response;
		cc->cc_response = NULL;
		ret = 0;
	}

done:
	ccid_command_free(cc);
	return (ret);
}

static void
ccid_intr_pipe_cb(usb_pipe_handle_t ph, usb_intr_req_t *uirp)
{
	mblk_t *mp;
	size_t msglen, explen;
	uint_t i;
	boolean_t change;
	ccid_t *ccid = (ccid_t *)uirp->intr_client_private;

	mp = uirp->intr_data;
	if (mp == NULL)
		goto done;

	msglen = msgsize(mp);
	if (msglen == 0)
		goto done;

	switch (mp->b_rptr[0]) {
	case CCID_INTR_CODE_SLOT_CHANGE:
		mutex_enter(&ccid->ccid_mutex);
		ccid->ccid_stats.cst_intr_slot_change++;

		explen = 1 + ((2 * ccid->ccid_nslots + (NBBY-1)) / NBBY);
		if (msglen < explen) {
			ccid->ccid_stats.cst_intr_inval++;
			mutex_exit(&ccid->ccid_mutex);
			goto done;
		}

		change = B_FALSE;
		for (i = 0; i < ccid->ccid_nslots; i++) {
			uint_t byte = (i * 2 / NBBY) + 1;
			uint_t shift = i * 2 % NBBY;
			uint_t present = 1 << shift;
			uint_t delta = 2 << shift;

			if (mp->b_rptr[byte] & delta) {
				ccid_slot_t *slot = &ccid->ccid_slots[i];

				slot->cs_flags &= ~CCID_SLOT_F_INTR_MASK;
				slot->cs_flags |= CCID_SLOT_F_CHANGED;
				if (mp->b_rptr[byte] & present) {
					slot->cs_flags |= CCID_SLOT_F_INTR_ADD;
				} else {
					slot->cs_flags |= CCID_SLOT_F_INTR_GONE;
				}
				change = B_TRUE;
			}
		}

		if (change) {
			ccid_worker_request(ccid);
		}
		mutex_exit(&ccid->ccid_mutex);
		break;
	case CCID_INTR_CODE_HW_ERROR:
		mutex_enter(&ccid->ccid_mutex);
		ccid->ccid_stats.cst_intr_hwerr++;

		if (msglen < sizeof (ccid_intr_hwerr_t)) {
			ccid->ccid_stats.cst_intr_inval++;
			mutex_exit(&ccid->ccid_mutex);
			goto done;
		}

		/* XXX what should we do with this? */
		mutex_exit(&ccid->ccid_mutex);
		break;
	default:
		mutex_enter(&ccid->ccid_mutex);
		ccid->ccid_stats.cst_intr_unknown++;
		mutex_exit(&ccid->ccid_mutex);
		break;
	}

done:
	usb_free_intr_req(uirp);
}

static void
ccid_intr_pipe_except_cb(usb_pipe_handle_t ph, usb_intr_req_t *uirp)
{
	ccid_t *ccid = (ccid_t *)uirp->intr_client_private;

	ccid->ccid_stats.cst_intr_errs++;
	switch (uirp->intr_completion_reason) {
	case USB_CR_PIPE_RESET:
	case USB_CR_NO_RESOURCES:
		ccid->ccid_stats.cst_intr_restart++;
		ccid_intr_poll_init(ccid);
		break;
	default:
		break;
	}
	usb_free_intr_req(uirp);
}

/*
 * The given CCID slot has been removed. Handle insertion.
 */
static void
ccid_slot_removed(ccid_t *ccid, ccid_slot_t *slot, boolean_t notify)
{
	/*
	 * Nothing to do right now.
	 */
	VERIFY(MUTEX_HELD(&ccid->ccid_mutex));
	if ((slot->cs_flags & CCID_SLOT_F_PRESENT) == 0) {
		VERIFY0(slot->cs_flags & CCID_SLOT_F_ACTIVE);
		return;
	}
	slot->cs_flags &= ~CCID_SLOT_F_PRESENT;
	slot->cs_flags &= ~CCID_SLOT_F_ACTIVE;
	slot->cs_voltage = 0;
	freemsgchain(slot->cs_atr);
	slot->cs_atr = NULL;
	if (slot->cs_excl_minor != NULL && notify) {
		pollwakeup(&slot->cs_excl_minor->cm_pollhead, POLLHUP);
	}
}

static void
ccid_slot_inserted(ccid_t *ccid, ccid_slot_t *slot)
{
	uint_t nvolts = 4;
	uint_t cvolt = 0;
	mblk_t *atr = NULL;
	ccid_class_voltage_t volts[4] = { CCID_CLASS_VOLT_AUTO,
	    CCID_CLASS_VOLT_5_0, CCID_CLASS_VOLT_3_0, CCID_CLASS_VOLT_1_8 };

	VERIFY(MUTEX_HELD(&ccid->ccid_mutex));
	if ((slot->cs_flags & CCID_SLOT_F_ACTIVE) != 0) {
		return;
	}

	slot->cs_flags |= CCID_SLOT_F_PRESENT;
	mutex_exit(&ccid->ccid_mutex);

	/*
	 * Now, we need to activate this ccid device before we can do anything
	 * with it. First, power on the device. There are two hardware features
	 * which may be at play. There may be automatic voltage detection and
	 * automatic activation on insertion. In theory, when either of those
	 * are present, we should always try to use the auto voltage.
	 *
	 * What's less clear in the specification is if the Auto-Voltage
	 * property is present is if we should try manual voltages or not. For
	 * the moment we do.
	 *
	 * Also, don't forget to drop the lock while performing this I/O.
	 * Nothing else should be able to access the ICC yet, as there is no
	 * minor node present.
	 */
	if ((ccid->ccid_class.ccd_dwFeatures &
	    (CCID_CLASS_F_AUTO_ICC_ACTIVATE | CCID_CLASS_F_AUTO_ICC_VOLTAGE)) ==
	    0) {
		/* Skip auto-voltage */
		cvolt++;
	}

	for (; cvolt < nvolts; cvolt++) {
		int ret;

		if (volts[cvolt] != CCID_CLASS_VOLT_AUTO &&
		    (ccid->ccid_class.ccd_bVoltageSupport & volts[cvolt]) ==
		    0) {
			continue;
		}

		if ((ret = ccid_command_power_on(ccid, slot, volts[cvolt],
		    &atr)) != 0) {
			freemsg(atr);
			atr = NULL;

			/*
			 * If we got ENXIO, then we know that there is no CCID
			 * present. This could happen for a number of reasons.
			 * For example, we could have just started up and no
			 * card was plugged in (we default to assuming that one
			 * is). Also, some readers won't really tell us that
			 * nothing is there until after the power on fails,
			 * hence why we don't bother with doing a status check
			 * and just try to power on.
			 */
			if (ret == ENXIO) {
				mutex_enter(&ccid->ccid_mutex);
				slot->cs_flags &= ~CCID_SLOT_F_PRESENT;
				return;
			}

			/*
			 * If we fail to power off the card, check to make sure
			 * it hasn't been removed.
			 */
			ret = ccid_command_power_off(ccid, slot);
			if (ret == ENXIO) {
				mutex_enter(&ccid->ccid_mutex);
				slot->cs_flags &= ~CCID_SLOT_F_PRESENT;
				return;
			}
			continue;
		}

		break;
	}


	if (cvolt >= nvolts) {
		ccid_error(ccid, "!failed to activate and power on ICC, no "
		    "supported voltages found");
		return;
	}


	mutex_enter(&ccid->ccid_mutex);
	slot->cs_voltage = volts[cvolt];
	slot->cs_atr = atr;
	slot->cs_flags |= CCID_SLOT_F_ACTIVE;
}

static boolean_t
ccid_slot_reset(ccid_t *ccid, ccid_slot_t *slot)
{
	int ret;

	VERIFY(MUTEX_HELD(&ccid->ccid_mutex));
	VERIFY(ccid->ccid_flags & CCID_SLOT_F_NEED_TXN_RESET);
	VERIFY(ccid->ccid_flags & CCID_FLAG_WORKER_RUNNING);

	mutex_exit(&ccid->ccid_mutex);
	ret = ccid_command_power_off(ccid, slot);
	mutex_enter(&ccid->ccid_mutex);

	/*
	 * If the CCID was removed, that's fine. We can actually just mark it as
	 * removed and move onto the next user. Note, we don't opt to notify the
	 * user as we'll wait for that to happen as part of our normal
	 * transition, which should still occur here.
	 */
	if (ret != 0 && ret == ENXIO) {
		ccid_slot_removed(ccid, slot, B_FALSE);
		return (B_TRUE);
	}

	if (ret != 0) {
		ccid_error(ccid, "failed to reset slot %d for next txn: %d; "
		    "taking another lap", ret);
		return (B_FALSE);
	}

	ccid->ccid_flags &= ~CCID_SLOT_F_ACTIVE;
	freemsgchain(slot->cs_atr);
	slot->cs_atr = NULL;

	mutex_exit(&ccid->ccid_mutex);
	/*
	 * Attempt to insert this into the slot. Don't worry about success or
	 * failure, because as far as we care for resetting it, we've done our
	 * duty once we've powered it off successfully.
	 */
	ccid_slot_inserted(ccid, slot);
	mutex_enter(&ccid->ccid_mutex);

	return (B_TRUE);
}

/*
 * We've been asked to perform some amount of work on the various slots that we
 * have. This may be because the slot needs to be reset due to the completion of
 * a transaction or it may be because an ICC inside of the slot has been
 * removed.
 */
static void
ccid_worker(void *arg)
{
	uint_t i;
	ccid_t *ccid = arg;

	mutex_enter(&ccid->ccid_mutex);
	ccid->ccid_stats.cst_ndiscover++;
	ccid->ccid_stats.cst_lastdiscover = gethrtime();
	if (ccid->ccid_flags & CCID_FLAG_DETACHING) {
		ccid->ccid_flags &= ~CCID_FLAG_WORKER_MASK;
		mutex_exit(&ccid->ccid_mutex);
		return;
	}
	ccid->ccid_flags |= CCID_FLAG_WORKER_RUNNING;
	ccid->ccid_flags &= ~CCID_FLAG_WORKER_REQUESTED;

	for (i = 0; i < ccid->ccid_nslots; i++) {
		ccid_slot_t *slot = &ccid->ccid_slots[i];
		ccid_reply_icc_status_t ss;
		int ret;
		uint_t flags;
		boolean_t skip_reset;

		VERIFY(MUTEX_HELD(&ccid->ccid_mutex));

		/*
		 * Snapshot the flags before we start processing the worker. At
		 * this time we clear out all of the change flags as we'll be
		 * operating on the device. We do not clear the
		 * CCID_SLOT_F_NEED_TXN_RESET flag, as we want to make sure that
		 * this is maintained until we're done here.
		 */
		flags = slot->cs_flags & CCID_SLOT_F_WORK_MASK;
		slot->cs_flags &= ~CCID_SLOT_F_INTR_MASK;

		if (flags & CCID_SLOT_F_CHANGED) {
			if (flags & CCID_SLOT_F_INTR_GONE) {
				ccid_slot_removed(ccid, slot, B_TRUE);
			} else {
				ccid_slot_inserted(ccid, slot);
			}
			VERIFY(MUTEX_HELD(&ccid->ccid_mutex));
		}

		if (flags & CCID_SLOT_F_NEED_TXN_RESET) {
			/*
			 * If the CCID_SLOT_F_PRESENT flag is set, then we
			 * should attempt to power off and power on the ICC in
			 * an attempt to reset it. If this fails, trigger
			 * another worker that needs to operate.
			 */
			if (flags & CCID_SLOT_F_PRESENT) {
				if (!ccid_slot_reset(ccid, slot)) {
					ccid_worker_request(ccid);
					continue;
				}
			}

			slot->cs_flags &= ~CCID_SLOT_F_NEED_TXN_RESET;
			ccid_slot_excl_signal(slot);
			VERIFY(MUTEX_HELD(&ccid->ccid_mutex));
		}
	}

	/*
	 * If we have a request to operate again, delay before we consider this,
	 * to make sure we don't do too much work ourselves.
	 */
	if (ccid->ccid_flags & CCID_FLAG_WORKER_REQUESTED) {
		mutex_exit(&ccid->ccid_mutex);
		delay(drv_usectohz(1000) * 10);
		mutex_enter(&ccid->ccid_mutex);
	}

	ccid->ccid_flags &= ~CCID_FLAG_WORKER_RUNNING;
	if (ccid->ccid_flags & CCID_FLAG_DETACHING) {
		mutex_exit(&ccid->ccid_mutex);
		return;
	}

	if ((ccid->ccid_flags & CCID_FLAG_WORKER_REQUESTED) != 0) {
		(void) ddi_taskq_dispatch(ccid->ccid_taskq, ccid_worker, ccid,
		    DDI_SLEEP);
	}
	mutex_exit(&ccid->ccid_mutex);
}

static void
ccid_worker_request(ccid_t *ccid)
{
	boolean_t run;

	VERIFY(MUTEX_HELD(&ccid->ccid_mutex));
	if (ccid->ccid_flags & CCID_FLAG_DETACHING) {
		return;
	}

	run = (ccid->ccid_flags & CCID_FLAG_WORKER_MASK) == 0; 
	ccid->ccid_flags |= CCID_FLAG_WORKER_REQUESTED;
	if (run) {
		mutex_exit(&ccid->ccid_mutex);
		(void) ddi_taskq_dispatch(ccid->ccid_taskq, ccid_worker, ccid,
		    DDI_SLEEP);
		mutex_enter(&ccid->ccid_mutex);
	}
}

static void
ccid_intr_restart_timeout(void *arg)
{
	ccid_t *ccid = arg;

	mutex_enter(&ccid->ccid_mutex);
	if (ccid->ccid_flags & CCID_FLAG_DETACHING) {
		ccid->ccid_poll_timeout = NULL;
		mutex_exit(&ccid->ccid_mutex);
	}
	mutex_exit(&ccid->ccid_mutex);

	ccid_intr_poll_init(ccid);
}

/*
 * Search for the current class descriptor from the configuration cloud and
 * parse it for our use. We do this by first finding the current interface
 * descriptor and expecting it to be one of the next descriptors 
 */
static boolean_t
ccid_parse_class_desc(ccid_t *ccid)
{
	uint_t i;
	size_t len, tlen;
	usb_client_dev_data_t *dp;
	usb_alt_if_data_t *alt;

	/*
	 * Establish the target length we're looking for from usb_parse_data().
	 * Note that we cannot use the sizeof (ccid_class_descr_t) for this
	 * because that function does not know how to account for the padding at
	 * the end of the target structure (which is resasonble). So we manually
	 * figure out the number of bytes it should in theory write.
	 */
	tlen = offsetof(ccid_class_descr_t, ccd_bMaxCCIDBusySlots) +
	    sizeof (ccid->ccid_class.ccd_bMaxCCIDBusySlots);
	dp = ccid->ccid_dev_data;
	alt = &dp->dev_curr_cfg->cfg_if[dp->dev_curr_if].if_alt[0];
	for (i = 0; i < alt->altif_n_cvs; i++) {
		usb_cvs_data_t *cvs = &alt->altif_cvs[i];
		if (cvs->cvs_buf == NULL)
			continue;
		if (cvs->cvs_buf_len != CCID_DESCR_LENGTH)
			continue;
		if (cvs->cvs_buf[1] != CCID_DESCR_TYPE)
			continue;
		if ((len = usb_parse_data("ccscc3lcllc5lccscc", cvs->cvs_buf,
		    cvs->cvs_buf_len, &ccid->ccid_class,
		    sizeof (ccid->ccid_class))) >= tlen) {
			return (B_TRUE);
		}
		ccid_error(ccid, "faild to parse CCID class descriptor from "
		    "cvs %u, expected %lu bytes, received %lu", i, tlen, len);
	}

	ccid_error(ccid, "failed to find matching CCID class descriptor");
	return (B_FALSE);
}

/*
 * Look at the ccid device in question and determine whether or not we can use
 * it.
 *
 * XXX we should use this as a basis for determining which features we require
 * from a card. This set should evolve as we implement features and know which
 * ones we're missing.
 */
static boolean_t
ccid_supported(ccid_t *ccid)
{
	usb_client_dev_data_t *dp;
	usb_alt_if_data_t *alt;
	uint16_t ver = ccid->ccid_class.ccd_bcdCCID;

	if (CCID_VERSION_MAJOR(ver) != CCID_VERSION_ONE) {
		ccid_error(ccid, "refusing to attach to CCID with unsupported "
		   "version %x.%2x", CCID_VERSION_MAJOR(ver),
		   CCID_VERSION_MINOR(ver));
		return (B_FALSE);
	}

	/*
	 * Check the number of endpoints. This should have either two or three.
	 * If three, that means we should expect an interrupt-IN endpoint.
	 * Otherwise, we shouldn't. Any other value indicates something weird
	 * that we should ignore.
	 */
	dp = ccid->ccid_dev_data;
	alt = &dp->dev_curr_cfg->cfg_if[dp->dev_curr_if].if_alt[0];
	switch (alt->altif_descr.bNumEndpoints) {
	case 2:
		ccid->ccid_flags &= ~CCID_FLAG_HAS_INTR;
		break;
	case 3:
		ccid->ccid_flags |= CCID_FLAG_HAS_INTR;
		break;
	default:
		ccid_error(ccid, "refusing to attach to CCID with unsupported "
		    "number of endpoints: %d", alt->altif_descr.bNumEndpoints);
		return (B_FALSE);
	}

	/*
	 * Try and determine the appropriate buffer size. This can be a little
	 * tricky. The class descriptor tells us the maximum size that the
	 * reader excepts. While it may be tempting to try and use a larger
	 * value such as the maximum size, the readers really don't like
	 * receiving bulk transfers that large. However, there are also reports
	 * of readers that will overwrite to a fixed minimum size. XXX which
	 * devices were those and should this be a p2roundup on the order of 256
	 * bytes maybe?
	 */
	ccid->ccid_bufsize = ccid->ccid_class.ccd_dwMaxCCIDMessageLength;

	/*
	 * At this time, we do not require that the system have automatic ICC
	 * activation or automatic ICC voltage. These are handled automatically
	 * by the system.
	 */

	/*
	 * Explicitly require some form of APDU support. Note, at this time we
	 * check for this when performing writes, but warn about it here.
	 * 
	 * XXX We should figure out whether we should fail attach or not here.
	 * Maybe not for some kind of ugen thing?
	 */
	if ((ccid->ccid_class.ccd_dwFeatures & (CCID_CLASS_F_SHORT_APDU_XCHG |
	    CCID_CLASS_F_EXT_APDU_XCHG)) == 0) {
		ccid_error(ccid, "CCID does not support required APDU transfer "
		    "capabilities");
	}


	return (B_TRUE);
}

static boolean_t
ccid_open_pipes(ccid_t *ccid)
{
	int ret;
	usb_ep_data_t *ep;
	usb_client_dev_data_t *data;
	usb_pipe_policy_t policy;

	data = ccid->ccid_dev_data;

	/*
	 * First fill all the descriptors.
	 */
	ep = usb_lookup_ep_data(ccid->ccid_dip, data, data->dev_curr_if, 0, 0,
	    USB_EP_ATTR_BULK, USB_EP_DIR_IN);
	if (ep == NULL) {
		ccid_error(ccid, "failed to find CCID Bulk-IN endpoint");
		return (B_FALSE);
	}

	if ((ret = usb_ep_xdescr_fill(USB_EP_XDESCR_CURRENT_VERSION,
	    ccid->ccid_dip, ep, &ccid->ccid_bulkin_xdesc)) != USB_SUCCESS) {
		ccid_error(ccid, "failed to fill Bulk-IN xdescr: %d", ret);
		return (B_FALSE);
	}

	ep = usb_lookup_ep_data(ccid->ccid_dip, data, data->dev_curr_if, 0, 0,
	    USB_EP_ATTR_BULK, USB_EP_DIR_OUT);
	if (ep == NULL) {
		ccid_error(ccid, "failed to find CCID Bulk-OUT endpoint");
		return (B_FALSE);
	}

	if ((ret = usb_ep_xdescr_fill(USB_EP_XDESCR_CURRENT_VERSION,
	    ccid->ccid_dip, ep, &ccid->ccid_bulkout_xdesc)) != USB_SUCCESS) {
		ccid_error(ccid, "failed to fill Bulk-OUT xdescr: %d", ret);
		return (B_FALSE);
	}

	if (ccid->ccid_flags & CCID_FLAG_HAS_INTR) {
		ep = usb_lookup_ep_data(ccid->ccid_dip, data, data->dev_curr_if,
		    0, 0, USB_EP_ATTR_INTR, USB_EP_DIR_IN);
		if (ep == NULL) {
			ccid_error(ccid, "failed to find CCID Intr-IN "
			    "endpoint");
			return (B_FALSE);
		}

		if ((ret = usb_ep_xdescr_fill(USB_EP_XDESCR_CURRENT_VERSION,
		    ccid->ccid_dip, ep, &ccid->ccid_intrin_xdesc)) !=
		    USB_SUCCESS) {
			ccid_error(ccid, "failed to fill Intr-OUT xdescr: %d",
			    ret);
			return (B_FALSE);
		}
	}

	/*
	 * Now open up the pipes.
	 */

	/*
	 * First determine the maximum number of asynchronous requests. This
	 * determines the maximum 
	 */
	bzero(&policy, sizeof (policy));
	policy.pp_max_async_reqs = CCID_NUM_ASYNC_REQS;

	if ((ret = usb_pipe_xopen(ccid->ccid_dip, &ccid->ccid_bulkin_xdesc,
	    &policy, USB_FLAGS_SLEEP, &ccid->ccid_bulkin_pipe)) != USB_SUCCESS) {
		ccid_error(ccid, "failed to open Bulk-IN pipe: %d\n", ret);
		return (B_FALSE);
	}

	if ((ret = usb_pipe_xopen(ccid->ccid_dip, &ccid->ccid_bulkout_xdesc,
	    &policy, USB_FLAGS_SLEEP, &ccid->ccid_bulkout_pipe)) != USB_SUCCESS) {
		ccid_error(ccid, "failed to open Bulk-OUT pipe: %d\n", ret);
		usb_pipe_close(ccid->ccid_dip, ccid->ccid_bulkin_pipe,
		    USB_FLAGS_SLEEP, NULL, NULL);
		ccid->ccid_bulkin_pipe = NULL;
		return (B_FALSE);
	}

	if (ccid->ccid_flags & CCID_FLAG_HAS_INTR) {
		if ((ret = usb_pipe_xopen(ccid->ccid_dip,
		    &ccid->ccid_intrin_xdesc, &policy, USB_FLAGS_SLEEP,
		    &ccid->ccid_intrin_pipe)) != USB_SUCCESS) {
			ccid_error(ccid, "failed to open Intr-IN pipe: %d\n",
			    ret);
			usb_pipe_close(ccid->ccid_dip, ccid->ccid_bulkin_pipe,
			    USB_FLAGS_SLEEP, NULL, NULL);
			ccid->ccid_bulkin_pipe = NULL;
			usb_pipe_close(ccid->ccid_dip, ccid->ccid_bulkout_pipe,
			    USB_FLAGS_SLEEP, NULL, NULL);
			ccid->ccid_bulkout_pipe = NULL;
			return (B_FALSE);
		}
	}

	ccid->ccid_control_pipe = data->dev_default_ph;
	return (B_TRUE);
}

static void
ccid_slots_fini(ccid_t *ccid)
{
	uint_t i;

	for (i = 0; i < ccid->ccid_nslots; i++) {
		VERIFY3U(ccid->ccid_slots[i].cs_slotno, ==, i);

		if (ccid->ccid_slots[i].cs_command != NULL) {
			ccid_command_free(ccid->ccid_slots[i].cs_command);
			ccid->ccid_slots[i].cs_command = NULL;
		}

		freemsgchain(ccid->ccid_slots[i].cs_atr);
		list_destroy(&ccid->ccid_slots[i].cs_minors);
		list_destroy(&ccid->ccid_slots[i].cs_excl_waiters);
	}

	ddi_remove_minor_node(ccid->ccid_dip, NULL);
	kmem_free(ccid->ccid_slots, sizeof (ccid_slot_t) * ccid->ccid_nslots);
	ccid->ccid_nslots = 0;
	ccid->ccid_slots = NULL;
}

static boolean_t
ccid_slots_init(ccid_t *ccid)
{
	uint_t i;

	/*
	 * The class descriptor has the maximum index that one can index into.
	 * We therefore have to add one to determine the actual number of slots
	 * that exist.
	 */
	ccid->ccid_nslots = ccid->ccid_class.ccd_bMaxSlotIndex + 1;
	ccid->ccid_slots = kmem_zalloc(sizeof (ccid_slot_t) * ccid->ccid_nslots,
	    KM_SLEEP);
	for (i = 0; i < ccid->ccid_nslots; i++) {
		/*
		 * We initialize every possible slot as having changed to make
		 * sure that we have a chance to discover it. See the slot
		 * detection section in the big theory statement for more info.
		 */
		ccid->ccid_slots[i].cs_flags |= CCID_SLOT_F_CHANGED;
		ccid->ccid_slots[i].cs_slotno = i;
		ccid->ccid_slots[i].cs_ccid = ccid;
		ccid->ccid_slots[i].cs_idx.cmi_minor = CCID_MINOR_INVALID;
		ccid->ccid_slots[i].cs_idx.cmi_isslot = B_TRUE;
		ccid->ccid_slots[i].cs_idx.cmi_data.cmi_slot =
		    &ccid->ccid_slots[i];
		list_create(&ccid->ccid_slots[i].cs_minors, sizeof (ccid_minor_t),
		   offsetof(ccid_minor_t, cm_minor_list)); 
		list_create(&ccid->ccid_slots[i].cs_excl_waiters, sizeof (ccid_minor_t),
		   offsetof(ccid_minor_t, cm_excl_list)); 
	}

	return (B_TRUE);
}

static void
ccid_minors_fini(ccid_t *ccid)
{
	uint_t i;

	ddi_remove_minor_node(ccid->ccid_dip, NULL);
	for (i = 0; i < ccid->ccid_nslots; i++) {
		if (ccid->ccid_slots[i].cs_idx.cmi_minor == CCID_MINOR_INVALID)
			continue;
		ccid_minor_idx_free(&ccid->ccid_slots[i].cs_idx);
	}
}

static boolean_t
ccid_minors_init(ccid_t *ccid)
{
	uint_t i;

	for (i = 0; i < ccid->ccid_nslots; i++) {
		char buf[32];

		(void) ccid_minor_idx_alloc(&ccid->ccid_slots[i].cs_idx, B_TRUE);

		(void) snprintf(buf, sizeof (buf), "slot%d", i);
		/* XXX I wonder if this should be a new DDI_NT type (ccid) */
		if (ddi_create_minor_node(ccid->ccid_dip, buf, S_IFCHR,
		    ccid->ccid_slots[i].cs_idx.cmi_minor, "ccid", 0) !=
		    DDI_SUCCESS) {
			ccid_minors_fini(ccid);
			return (B_FALSE);
		}
	}

	return (B_TRUE);
}

static void
ccid_intr_poll_fini(ccid_t *ccid)
{
	if (ccid->ccid_flags & CCID_FLAG_HAS_INTR) {
		timeout_id_t tid;
		mutex_enter(&ccid->ccid_mutex);
		tid = ccid->ccid_poll_timeout;
		ccid->ccid_poll_timeout = NULL;
		mutex_exit(&ccid->ccid_mutex);
		(void) untimeout(tid);
		usb_pipe_stop_intr_polling(ccid->ccid_intrin_pipe,
		    USB_FLAGS_SLEEP);
	} else {
		VERIFY3P(ccid->ccid_intrin_pipe, ==, NULL);
	}
}

static void
ccid_intr_poll_init(ccid_t *ccid)
{
	int ret;
	usb_intr_req_t *uirp;

	uirp = usb_alloc_intr_req(ccid->ccid_dip, 0, USB_FLAGS_SLEEP);
	uirp->intr_client_private = (usb_opaque_t)ccid;
	uirp->intr_attributes = USB_ATTRS_SHORT_XFER_OK |
	    USB_ATTRS_AUTOCLEARING;
	uirp->intr_len = CCID_INTR_RESPONSE_SIZE;
	uirp->intr_cb = ccid_intr_pipe_cb;
	uirp->intr_exc_cb = ccid_intr_pipe_except_cb;

	mutex_enter(&ccid->ccid_mutex);
	if (ccid->ccid_flags & CCID_FLAG_DETACHING) {
		mutex_exit(&ccid->ccid_mutex);
		usb_free_intr_req(uirp);
		return;
	}

	if ((ret = usb_pipe_intr_xfer(ccid->ccid_intrin_pipe, uirp,
	    USB_FLAGS_SLEEP)) != USB_SUCCESS) {
		ccid_error(ccid, "!failed to start polling on CCID Intr-IN "
		    "pipe: %d", ret);
		ccid->ccid_poll_timeout = timeout(ccid_intr_restart_timeout,
		    ccid, drv_usectohz(1000000));
		usb_free_intr_req(uirp);
	}
	mutex_exit(&ccid->ccid_mutex);
}

static void
ccid_cleanup_bulkin(ccid_t *ccid)
{
	uint_t i;

	VERIFY3P(ccid->ccid_bulkin_dispatched, ==, NULL);
	for (i = 0; i < ccid->ccid_bulkin_alloced; i++) {
		VERIFY3P(ccid->ccid_bulkin_cache[i], !=, NULL);
		usb_free_bulk_req(ccid->ccid_bulkin_cache[i]);
		ccid->ccid_bulkin_cache[i] = NULL;
	}

#ifdef	DEBUG
	for (i = 0; i < CCID_BULK_NALLOCED; i++) {
		VERIFY3P(ccid->ccid_bulkin_cache[i], ==, NULL);
	}
#endif
	ccid->ccid_bulkin_alloced = 0;
}

static int
ccid_disconnect_cb(dev_info_t *dip)
{
	int inst;
	ccid_t *ccid;

	if (dip == NULL)
		goto done;

	inst = ddi_get_instance(dip);
	ccid = ddi_get_soft_state(ccid_softstate, inst);
	if (ccid == NULL)
		goto done;
	VERIFY3P(dip, ==, ccid->ccid_dip);

	/*
	 * XXX We need to check this and throw errors throughout, throw out
	 * poll, etc.
	 */
	mutex_enter(&ccid->ccid_mutex);
	ccid->ccid_flags |= CCID_FLAG_DISCONNECTED;
	mutex_exit(&ccid->ccid_mutex);

done:
	return (USB_SUCCESS);
}

static usb_event_t ccid_usb_events = {
	ccid_disconnect_cb,
	NULL,
	NULL,
	NULL
};

static void
ccid_cleanup(dev_info_t *dip)
{
	int inst;
	ccid_t *ccid;

	if (dip == NULL)
		return;

	inst = ddi_get_instance(dip);
	ccid = ddi_get_soft_state(ccid_softstate, inst);
	if (ccid == NULL)
		return;
	VERIFY3P(dip, ==, ccid->ccid_dip);

	/*
	 * Make sure we set the detaching flag so anything running in the
	 * background knows to stop.
	 */
	mutex_enter(&ccid->ccid_mutex);
	ccid->ccid_flags |= CCID_FLAG_DETACHING;
	mutex_exit(&ccid->ccid_mutex);

	if (ccid->ccid_attach & CCID_ATTACH_MINORS) {
		ccid_minors_fini(ccid);
		ccid->ccid_attach &= ~CCID_ATTACH_MINORS;
	}

	if (ccid->ccid_attach & CCID_ATTACH_INTR_ACTIVE) {
		ccid_intr_poll_fini(ccid);
		ccid->ccid_attach &= ~CCID_ATTACH_INTR_ACTIVE;
	}

	/*
	 * At this point, we have shut down the interrupt pipe, the last place
	 * aside from a user that could have kicked off I/O. So finally wait for
	 * any worker threads.
	 */
	if (ccid->ccid_taskq != NULL) {
		ddi_taskq_wait(ccid->ccid_taskq);
		mutex_enter(&ccid->ccid_mutex);
		VERIFY0(ccid->ccid_flags & CCID_FLAG_WORKER_MASK);
		mutex_exit(&ccid->ccid_mutex);
	}

	if (ccid->ccid_attach & CCID_ATTACH_HOTPLUG_CB) {
		usb_unregister_event_cbs(dip, &ccid_usb_events);
		ccid->ccid_attach &= ~CCID_ATTACH_HOTPLUG_CB;
	}

	if (ccid->ccid_attach & CCID_ATTACH_SLOTS) {
		ccid_slots_fini(ccid);
		ccid->ccid_attach &= ~CCID_ATTACH_SLOTS;
	}

	if (ccid->ccid_attach & CCID_ATTACH_SEQ_IDS) {
		id_space_destroy(ccid->ccid_seqs);
		ccid->ccid_seqs = NULL;
		ccid->ccid_attach &= ~CCID_ATTACH_SEQ_IDS;
	}

	if (ccid->ccid_attach & CCID_ATTACH_OPEN_PIPES) {
		usb_pipe_close(dip, ccid->ccid_bulkin_pipe, USB_FLAGS_SLEEP,
		    NULL, NULL);
		ccid->ccid_bulkin_pipe = NULL;
		usb_pipe_close(dip, ccid->ccid_bulkout_pipe, USB_FLAGS_SLEEP,
		    NULL, NULL);
		ccid->ccid_bulkout_pipe = NULL;
		if (ccid->ccid_flags & CCID_FLAG_HAS_INTR) {
			usb_pipe_close(dip, ccid->ccid_intrin_pipe,
			    USB_FLAGS_SLEEP, NULL, NULL);
			ccid->ccid_intrin_pipe = NULL;
		} else {
			VERIFY3P(ccid->ccid_intrin_pipe, ==, NULL);
		}
		ccid->ccid_control_pipe = NULL;
		ccid->ccid_attach &= ~CCID_ATTACH_OPEN_PIPES;
	}

	/*
	 * Now that all of the pipes are closed. If we happened to have any
	 * cached bulk requests, we should free them.
	 */
	ccid_cleanup_bulkin(ccid);

	if (ccid->ccid_attach & CCID_ATTACH_CMD_LIST) {
		ccid_command_t *cc;

		while ((cc = list_remove_head(&ccid->ccid_command_queue)) !=
		    NULL) {
			ccid_command_free(cc);
		}
		list_destroy(&ccid->ccid_command_queue);

		while ((cc = list_remove_head(&ccid->ccid_complete_queue)) !=
		    NULL) {
			ccid_command_free(cc);
		}
		list_destroy(&ccid->ccid_complete_queue);
	}

	if (ccid->ccid_attach & CCID_ATTACH_TASKQ) {
		ddi_taskq_destroy(ccid->ccid_taskq);
		ccid->ccid_taskq = NULL;
		ccid->ccid_attach &= ~CCID_ATTACH_TASKQ;
	}

	if (ccid->ccid_attach & CCID_ATTACH_MUTEX_INIT) {
		mutex_destroy(&ccid->ccid_mutex);
		ccid->ccid_attach &= ~CCID_ATTACH_MUTEX_INIT;
	}

	if (ccid->ccid_attach & CCID_ATTACH_USB_CLIENT) {
		usb_client_detach(dip, ccid->ccid_dev_data);
		ccid->ccid_dev_data = NULL;
		ccid->ccid_attach &= ~CCID_ATTACH_USB_CLIENT;
	}

	ASSERT0(ccid->ccid_attach);
	ddi_soft_state_free(ccid_softstate, inst);
}

static int
ccid_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	ccid_t *ccid;
	int inst, ret;
	char buf[64];

	if (cmd != DDI_ATTACH)
		return (DDI_FAILURE);

	inst = ddi_get_instance(dip);
	if (ddi_soft_state_zalloc(ccid_softstate, inst) != DDI_SUCCESS) {
		ccid_error(NULL, "failed to allocate soft state for ccid "
		    "instance %d", inst);
		return (DDI_FAILURE);
	}

	ccid = ddi_get_soft_state(ccid_softstate, inst);
	ccid->ccid_dip = dip;

	if ((ret = usb_client_attach(dip, USBDRV_VERSION, 0)) != USB_SUCCESS) {
		ccid_error(ccid, "failed to attach to usb client: %d", ret);
		goto cleanup;
	}
	ccid->ccid_attach |= CCID_ATTACH_USB_CLIENT;

	if ((ret = usb_get_dev_data(dip, &ccid->ccid_dev_data, USB_PARSE_LVL_IF,
	    0)) != USB_SUCCESS) {
		ccid_error(ccid, "failed to get usb device data: %d", ret);
		goto cleanup;
	}

	mutex_init(&ccid->ccid_mutex, NULL, MUTEX_DRIVER,
	    ccid->ccid_dev_data->dev_iblock_cookie);
	ccid->ccid_attach |= CCID_ATTACH_MUTEX_INIT;

	(void) snprintf(buf, sizeof (buf), "ccid%d_taskq", inst);
	ccid->ccid_taskq = ddi_taskq_create(dip, buf, 1, TASKQ_DEFAULTPRI, 0);
	if (ccid->ccid_taskq == NULL) {
		ccid_error(ccid, "failed to create CCID taskq");
		goto cleanup;
	}
	ccid->ccid_attach |= CCID_ATTACH_TASKQ;

	list_create(&ccid->ccid_command_queue, sizeof (ccid_command_t),
	    offsetof(ccid_command_t, cc_list_node));
	list_create(&ccid->ccid_complete_queue, sizeof (ccid_command_t),
	    offsetof(ccid_command_t, cc_list_node));

	if (!ccid_parse_class_desc(ccid)) {
		ccid_error(ccid, "failed to parse CCID class descriptor");
		goto cleanup;
	}

	if (!ccid_supported(ccid)) {
		ccid_error(ccid, "CCID reader is not supported, not attaching");
		goto cleanup;
	}

	if (!ccid_open_pipes(ccid)) {
		ccid_error(ccid, "failed to open CCID pipes, not attaching");
		goto cleanup;
	}
	ccid->ccid_attach |= CCID_ATTACH_OPEN_PIPES;

	(void) snprintf(buf, sizeof (buf), "ccid%d_seqs", inst);
	if ((ccid->ccid_seqs = id_space_create(buf, CCID_SEQ_MIN,
	    CCID_SEQ_MAX + 1)) == NULL) {
		ccid_error(ccid, "failed to create CCID sequence id space");
		goto cleanup;
	}
	ccid->ccid_attach |= CCID_ATTACH_SEQ_IDS;

	if (!ccid_slots_init(ccid)) {
		ccid_error(ccid, "failed to initialize CCID slot structures");
		goto cleanup;
	}
	ccid->ccid_attach |= CCID_ATTACH_SLOTS;

	if (usb_register_event_cbs(dip, &ccid_usb_events, 0) != USB_SUCCESS) {
		ccid_error(ccid, "failed to register USB hotplug callbacks");
		goto cleanup;
	}
	ccid->ccid_attach |= CCID_ATTACH_HOTPLUG_CB;

	/*
	 * Before we enable the interrupt pipe, take a shot at priming our
	 * bulkin_cache.
	 */
	ccid_bulkin_cache_refresh(ccid);

	if (ccid->ccid_flags & CCID_FLAG_HAS_INTR) {
		ccid_intr_poll_init(ccid);
	}
	ccid->ccid_attach |= CCID_ATTACH_INTR_ACTIVE;

	/*
	 * Create minor nodes for each slot.
	 */
	if (!ccid_minors_init(ccid)) {
		ccid_error(ccid, "failed to create minor nodes");
		goto cleanup;
	}
	ccid->ccid_attach |= CCID_ATTACH_MINORS;

	mutex_enter(&ccid->ccid_mutex);
	ccid_worker_request(ccid);
	mutex_exit(&ccid->ccid_mutex);

	return (DDI_SUCCESS);

cleanup:
	ccid_cleanup(dip);
	return (DDI_SUCCESS);
}

static int
ccid_getinfo(dev_info_t *dip, ddi_info_cmd_t cmd, void *arg, void **outp)
{
	return (DDI_FAILURE);
}

static int
ccid_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	int inst;
	uint_t i;
	ccid_t *ccid;

	if (cmd != DDI_DETACH)
		return (DDI_FAILURE);

	inst = ddi_get_instance(dip);
	ccid = ddi_get_soft_state(ccid_softstate, inst);
	VERIFY3P(ccid, !=, NULL);
	VERIFY3P(dip, ==, ccid->ccid_dip);

	mutex_enter(&ccid->ccid_mutex);

	/*
	 * If the device hasn't been disconnected from a USB sense, refuse to
	 * detach. Otherwise, there's no way to guarantee that the ccid
	 * driver will be attached when a user hotplugs an ICC.
	 */
	if ((ccid->ccid_flags & CCID_FLAG_DISCONNECTED) == 0) {
		mutex_exit(&ccid->ccid_mutex);
		return (DDI_FAILURE);
	}

	if (list_is_empty(&ccid->ccid_command_queue) == 0 ||
	    list_is_empty(&ccid->ccid_complete_queue) == 0) {
		mutex_exit(&ccid->ccid_mutex);
		return (DDI_FAILURE);
	}
	mutex_exit(&ccid->ccid_mutex);

	ccid_cleanup(dip);
	return (DDI_SUCCESS);
}

static void
ccid_minor_free(ccid_minor_t *cmp)
{
	ccid_command_t *cc;

	/*
	 * Clean up queued commands.
	 */
	VERIFY3U(cmp->cm_idx.cmi_minor, ==, CCID_MINOR_INVALID);
	crfree(cmp->cm_opener);
	cv_destroy(&cmp->cm_read_cv);
	cv_destroy(&cmp->cm_excl_cv);
	kmem_free(cmp, sizeof (ccid_minor_t));

}

static int
ccid_open(dev_t *devp, int flag, int otyp, cred_t *credp)
{
	int ret;
	ccid_minor_idx_t *idx;
	ccid_minor_t *cmp;
	ccid_slot_t *slot;

	/*
	 * Always check the zone first, to make sure we lie about it existing.
	 */
	if (crgetzoneid(credp) != GLOBAL_ZONEID)
		return (ENOENT);

	if (otyp & (FNDELAY | FEXCL))
		return (EINVAL);

	if (drv_priv(credp) != 0)
		return (EPERM);

	if (otyp & OTYP_BLK || !(otyp & OTYP_CHR))
		return (ENOTSUP);

	if ((flag & (FREAD | FWRITE)) != (FREAD | FWRITE))
		return (EINVAL);

	idx = ccid_minor_find(getminor(*devp));
	if (idx == NULL) {
		return (ENOENT);
	}

	/*
	 * We don't expect anyone to be able to get a non-slot related minor. If
	 * that somehow happens, guard against it and error out.
	 */
	if (!idx->cmi_isslot) {
		return (ENOENT);
	}

	slot = idx->cmi_data.cmi_slot;
	cmp = kmem_zalloc(sizeof (ccid_minor_t), KM_SLEEP);

	cmp->cm_idx.cmi_minor = CCID_MINOR_INVALID;
	cmp->cm_idx.cmi_isslot = B_FALSE;
	cmp->cm_idx.cmi_data.cmi_user = cmp;
	if (!ccid_minor_idx_alloc(&cmp->cm_idx, B_FALSE)) {
		kmem_free(cmp, sizeof (ccid_minor_t));
		return (ENOSPC);
	}
	cv_init(&cmp->cm_excl_cv, NULL, CV_DRIVER, NULL);
	cv_init(&cmp->cm_read_cv, NULL, CV_DRIVER, NULL);
	cmp->cm_opener = crdup(credp);
	cmp->cm_slot = slot;
	*devp = makedevice(getmajor(*devp), cmp->cm_idx.cmi_minor);

	mutex_enter(&slot->cs_ccid->ccid_mutex);
	list_insert_tail(&slot->cs_minors, cmp);
	mutex_exit(&slot->cs_ccid->ccid_mutex);

	return (0);
}

/*
 * Copy a command which may have a message block chain out to the user.
 */
static int
ccid_read_copyout(struct uio *uiop, ccid_command_t *cc, size_t len)
{
	int ret;
	mblk_t *mp;
	offset_t off;

	off = uiop->uio_loffset;
	mp = cc->cc_response;
	while (len > 0) {
		size_t tocopy;
		/*
		 * Each message block in the chain has its CCID header at the
		 * front.
		 *
		 * XXX This may or may not be true for TPDU land.
		 */
		mp->b_rptr += sizeof (ccid_header_t);
		tocopy = MIN(len, MBLKL(mp));
		ret = uiomove(mp->b_rptr, tocopy, UIO_READ, uiop);
		mp->b_rptr -= sizeof (ccid_header_t);
		if (ret != 0)
			return (EFAULT);
		len -= tocopy;
		if (len != 0) {
			mp = mp->b_cont;
			VERIFY3P(mp, !=, NULL);
		}
	}
	uiop->uio_loffset = off;

	return (0);
}

static int
ccid_write_copyin(struct uio *uiop, mblk_t **mpp)
{
	mblk_t *mp;
	int ret;
	size_t len = uiop->uio_resid + sizeof (ccid_header_t);
	offset_t off;

	*mpp = NULL;
	mp = allocb(len, 0);
	if (mp == NULL) {
		return (ENOMEM);
	}
	mp->b_wptr = mp->b_rptr + len;

	/*
	 * Copy in the buffer, leaving enough space for the ccid header.
	 */
	off = uiop->uio_loffset;
	if ((ret = uiomove(mp->b_rptr + sizeof (ccid_header_t), uiop->uio_resid,
	    UIO_WRITE, uiop)) != 0) {
		freemsg(mp);
		return (ret);
	}
	uiop->uio_loffset = off;
	*mpp = mp;
	return (0);
}

static int
ccid_read(dev_t dev, struct uio *uiop, cred_t *credp)
{
	int ret;
	ccid_minor_idx_t *idx;
	ccid_minor_t *cmp;
	ccid_slot_t *slot;
	ccid_t *ccid;
	ccid_command_t *cc;
	ccid_reply_command_status_t crs;
	ccid_reply_icc_status_t cis;
	ccid_command_err_t cce;

	if (uiop->uio_resid <= 0) {
		return (EINVAL);
	}

	if ((idx = ccid_minor_find(getminor(dev))) == NULL) {
		return (ENOENT);
	}

	if (idx->cmi_isslot) {
		return (ENXIO);
	}

	cmp = idx->cmi_data.cmi_user;
	slot = cmp->cm_slot;
	ccid = slot->cs_ccid;

	mutex_enter(&ccid->ccid_mutex);
	if ((ccid->ccid_flags & CCID_FLAG_DETACHING) != 0) {
		mutex_exit(&ccid->ccid_mutex);
		return (ENODEV);
	}

	/*
	 * First, check if we have exclusive access. If not, we're done.
	 */
	if (!(cmp->cm_flags & CCID_MINOR_F_HAS_EXCL)) {
		mutex_exit(&ccid->ccid_mutex);
		return (EACCES);
	}

	/*
	 * While it's tempting to care if the slot is active, that actually
	 * doesn't matter. All that matters is whether or not we have commands
	 * here that are in progress or readable.
	 *
	 * The only unfortunate matter is that we can't check if the user can
	 * read this command until one is available. Which means that someone
	 * could be blocked for some time.
	 */
	while (slot->cs_command == NULL ||
	    slot->cs_command->cc_state < CCID_COMMAND_COMPLETE) {
		if (uiop->uio_fmode & FNONBLOCK) {
			mutex_exit(&ccid->ccid_mutex);
			return (EWOULDBLOCK);
		}

		if (cv_wait_sig(&cmp->cm_read_cv, &ccid->ccid_mutex) == 0) {
			mutex_exit(&ccid->ccid_mutex);
			return (EINTR);
		}
	}

	/*
	 * Decode the status of the first command. If the command was
	 * successful, we need to check the user's buffer size. Otherwise, we
	 * can consume it all.
	 *
	 * XXX The command status logic may not be correct for TPDU.
	 */
	cc = slot->cs_command;

	/*
	 * If we didn't get a successful command, then we go ahead and consume
	 * this and mark it as having generated an EIO.
	 */
	if (cc->cc_state != CCID_COMMAND_COMPLETE) {
		ret = EIO;
		goto consume;
	}

	ccid_command_status_decode(cc, &crs, &cis, &cce);
	if (crs == CCID_REPLY_STATUS_COMPLETE) {
		size_t len;

		/*
		 * Note, as part of processing the reply, the driver has already
		 * gone through and made sure that we have a message block large
		 * enough for this command.
		 */
		len = ccid_command_resp_length(cc);
		if (len > uiop->uio_resid) {
			mutex_exit(&ccid->ccid_mutex);
			return (EOVERFLOW);
		}

		/*
		 * Copy out the resulting data.
		 */
		ret = ccid_read_copyout(uiop, cc, len);
		if (ret != 0) {
			mutex_exit(&ccid->ccid_mutex);
			return (ret);
		}
	} else {
		if (cis == CCID_REPLY_ICC_MISSING) {
			ret = ENXIO;
		} else {
			/*
			 * XXX There are a few more semantic things we can do
			 * with the errors here that we're throwing out and
			 * lumping as EIO. Oh well.
			 */
			ret = EIO;
		}
	}

consume:
	pollwakeup(&cmp->cm_pollhead, POLLOUT);
	slot->cs_command = NULL;
	mutex_exit(&ccid->ccid_mutex);
	ccid_command_free(cc);

	return (ret);
}

static int
ccid_write(dev_t dev, struct uio *uiop, cred_t *credp)
{
	int ret;
	ccid_minor_idx_t *idx;
	ccid_minor_t *cmp;
	ccid_slot_t *slot;
	ccid_t *ccid;
	mblk_t *mp = NULL;
	ccid_command_t *cc = NULL;
	size_t len;

	if (uiop->uio_resid > CCID_APDU_LEN_MAX) {
		return (E2BIG);
	}

	if (uiop->uio_resid <= 0) {
		return (EINVAL);
	}

	idx = ccid_minor_find(getminor(dev));
	if (idx == NULL) {
		return (ENOENT);
	}

	if (idx->cmi_isslot) {
		return (ENXIO);
	}

	cmp = idx->cmi_data.cmi_user;
	slot = cmp->cm_slot;
	ccid = slot->cs_ccid;

	/*
	 * Copy in the uio data into an mblk. For the iosize we use the actual
	 * size of the data we care about. We put a ccid_header_t worth of data
	 * in front of this so we have space for the header. Snapshot the size
	 * before we do the uiomove(). If for some reason the I/O fails, we
	 * don't worry about trying to restore the original resid, consumers
	 * should ignore it on failure.
	 */
	len = uiop->uio_resid;
	if ((ret = ccid_write_copyin(uiop, &mp)) != 0) {
		return (ret);
	}

	if ((ret = ccid_command_alloc(ccid, slot, B_FALSE, mp, len,
	    CCID_REQUEST_TRANSFER_BLOCK, 0, 0, 0, &cc)) != 0) {
		freemsg(mp);
		return (ret);
	}
	cc->cc_flags |= CCID_COMMAND_F_USER;

	mutex_enter(&ccid->ccid_mutex);

	if ((ccid->ccid_flags & CCID_FLAG_DETACHING) != 0) {
		mutex_exit(&ccid->ccid_mutex);
		return (ENODEV);
	}

	/*
	 * Check if we have exclusive access and if there's a card present. If
	 * not, both are errors.
	 */
	if (!(cmp->cm_flags & CCID_MINOR_F_HAS_EXCL)) {
		mutex_exit(&ccid->ccid_mutex);
		ccid_command_free(cc);
		return (EACCES);
	}

	if (!(slot->cs_flags & CCID_SLOT_F_ACTIVE)) {
		mutex_exit(&ccid->ccid_mutex);
		ccid_command_free(cc);
		return (ENXIO);
	}

	/*
	 * Make sure that we have a supported short APDU form.
	 */
	if ((ccid->ccid_class.ccd_dwFeatures & (CCID_CLASS_F_SHORT_APDU_XCHG |
	    CCID_CLASS_F_EXT_APDU_XCHG)) == 0) {
		mutex_exit(&ccid->ccid_mutex);
		ccid_command_free(cc);
		return (ENOTSUP);
	}

	/*
	 * XXX This isn't taking into accounts commands that we need to issue
	 * from the perspective of hardware and all those other fun things.
	 */
	if (slot->cs_command != NULL) {
		mutex_exit(&ccid->ccid_mutex);
		ccid_command_free(cc);
		return (EBUSY);
	}

	slot->cs_command = cc;
	mutex_exit(&ccid->ccid_mutex);

	/*
	 * Now that the slot is set up. Try and queue on the command. After
	 * that, we'll poll until it's gotten to the point where something is
	 * replying ot it. At that point, we'll be free to return the write.
	 */

	if ((ret = ccid_command_queue(ccid, cc)) != 0) {
		ccid_command_free(cc);
		return (ret);
	}

	mutex_enter(&ccid->ccid_mutex);
	while (cc->cc_state < CCID_COMMAND_REPLYING) {
		/*
		 * If we receive a signal, break out of the loop. Don't return
		 * EINTR, as this has been successfully dispatched. It just
		 * means that it'll be a little while before more I/O is ready.
		 */
		if (cv_wait_sig(&cc->cc_cv, &ccid->ccid_mutex) == 0)
			break;
	}

	mutex_exit(&ccid->ccid_mutex);

	return (0);
}

static int
ccid_ioctl_status(ccid_slot_t *slot, intptr_t arg, int mode)
{
	uccid_cmd_status_t ucs;

	if (ddi_copyin((void *)arg, &ucs, sizeof (ucs), mode & FKIOCTL) != 0)
		return (EFAULT);

	if (ucs.ucs_version != UCCID_VERSION_ONE)
		return (EINVAL);

	ucs.ucs_status = 0;
	mutex_enter(&slot->cs_ccid->ccid_mutex);
	if ((slot->cs_ccid->ccid_flags & CCID_FLAG_DETACHING) != 0) {
		mutex_exit(&slot->cs_ccid->ccid_mutex);
		return (ENODEV);
	}

	if (slot->cs_flags & CCID_SLOT_F_PRESENT)
		ucs.ucs_status |= UCCID_STATUS_F_CARD_PRESENT;
	if (slot->cs_flags & CCID_SLOT_F_ACTIVE)
		ucs.ucs_status |= UCCID_STATUS_F_CARD_ACTIVE;
	mutex_exit(&slot->cs_ccid->ccid_mutex);

	if (ddi_copyout(&ucs, (void *)arg, sizeof (ucs), mode & FKIOCTL) != 0)
		return (EFAULT);

	return (0);
}

static boolean_t
ccid_ioctl_copyin_buf(intptr_t arg, int mode, uccid_cmd_getbuf_t *ucg)
{
	if (ddi_model_convert_from(mode & FMODELS) == DDI_MODEL_ILP32) {
		uccid_cmd_getbuf32_t ucg32;

		if (ddi_copyin((void *)arg, &ucg32, sizeof (ucg32),
		    mode & FKIOCTL) != 0)
			return (B_FALSE);
		ucg->ucg_version = ucg32.ucg_version;
		ucg->ucg_buflen = ucg32.ucg_buflen;
		ucg->ucg_buffer = (void *)(uintptr_t)ucg32.ucg_buffer;
	} else {
		if (ddi_copyin((void *)arg, ucg, sizeof (*ucg),
		    mode & FKIOCTL) != 0)
			return (B_FALSE);
	}

	return (B_TRUE);
}

static boolean_t
ccid_ioctl_copyout_buf(intptr_t arg, int mode, uccid_cmd_getbuf_t *ucg)
{
	if (ddi_model_convert_from(mode & FMODELS) == DDI_MODEL_ILP32) {
		uccid_cmd_getbuf32_t ucg32;

		ucg32.ucg_version = ucg->ucg_version;
		ucg32.ucg_buflen = (uint32_t)ucg->ucg_buflen;
		ucg32.ucg_buffer = (uintptr32_t)(uintptr_t)ucg->ucg_buffer;

		if (ddi_copyout(&ucg32, (void *)arg, sizeof (ucg32),
		    mode & FKIOCTL) != 0)
			return (B_FALSE);
	} else {
		if (ddi_copyout(ucg, (void *)arg, sizeof (ucg),
		    mode & FKIOCTL) != 0)
			return (B_FALSE);
	}

	return (B_TRUE);
}

static boolean_t
ccid_ioctl_copyout_bufdata(void *data, size_t len, uccid_cmd_getbuf_t *ucg,
    int mode, int *errp)
{
	int ret;

	if (ddi_model_convert_from(mode & FMODELS) == DDI_MODEL_ILP32) {
		if (len > UINT32_MAX) {
			*errp = ERANGE;
			return (B_FALSE);
		}
	}

	if (ucg->ucg_buflen >= len) {
		if (ddi_copyout(data, ucg->ucg_buffer, len,
		    mode & FKIOCTL) != 0) {
			*errp = EFAULT;
			return (B_FALSE);
		}
		*errp = 0;
	} else {
		*errp = EOVERFLOW;
	}

	ucg->ucg_buflen = len;
	return (B_TRUE);
}

static int
ccid_ioctl_getatr(ccid_slot_t *slot, intptr_t arg, int mode)
{
	int ret;
	size_t atrlen;
	uccid_cmd_getbuf_t ucg;

	if (!ccid_ioctl_copyin_buf(arg, mode, &ucg)) {
		return (EFAULT);
	}

	if (ucg.ucg_version != UCCID_VERSION_ONE)
		return (EINVAL);

	mutex_enter(&slot->cs_ccid->ccid_mutex);
	if ((slot->cs_ccid->ccid_flags & CCID_FLAG_DETACHING) != 0) {
		mutex_exit(&slot->cs_ccid->ccid_mutex);
		return (ENODEV);
	}

	if (slot->cs_atr == NULL) {
		mutex_exit(&slot->cs_ccid->ccid_mutex);
		return (ENXIO);
	}

	atrlen = msgsize(slot->cs_atr);
	if (!ccid_ioctl_copyout_bufdata(slot->cs_atr->b_rptr, atrlen, &ucg,
	    mode, &ret)) {
		return (ret);
	}
	mutex_exit(&slot->cs_ccid->ccid_mutex);

	if (!ccid_ioctl_copyout_buf(arg, mode, &ucg)) {
		return (EFAULT);
	}

	return (ret);
}

static int
ccid_ioctl_getprodstr(ccid_slot_t *slot, intptr_t arg, int mode)
{
	int ret;
	size_t len;
	uccid_cmd_getbuf_t ucg;
	ccid_t *ccid;

	if (!ccid_ioctl_copyin_buf(arg, mode, &ucg)) {
		return (EFAULT);
	}

	if (ucg.ucg_version != UCCID_VERSION_ONE)
		return (EINVAL);

	ccid = slot->cs_ccid;
	mutex_enter(&ccid->ccid_mutex);
	if ((slot->cs_ccid->ccid_flags & CCID_FLAG_DETACHING) != 0) {
		mutex_exit(&ccid->ccid_mutex);
		return (ENODEV);
	}

	if (ccid->ccid_dev_data->dev_product == NULL) {
		mutex_exit(&ccid->ccid_mutex);
		return (ENOENT);
	}

	len = strlen(ccid->ccid_dev_data->dev_product) + 1;
	if (!ccid_ioctl_copyout_bufdata(ccid->ccid_dev_data->dev_product, len,
	    &ucg, mode, &ret)) {
		return (ret);
	}
	mutex_exit(&slot->cs_ccid->ccid_mutex);

	if (!ccid_ioctl_copyout_buf(arg, mode, &ucg)) {
		return (EFAULT);
	}

	return (ret);
}

static int
ccid_ioctl_getserial(ccid_slot_t *slot, intptr_t arg, int mode)
{
	int ret;
	size_t len;
	uccid_cmd_getbuf_t ucg;
	ccid_t *ccid;

	if (!ccid_ioctl_copyin_buf(arg, mode, &ucg)) {
		return (EFAULT);
	}

	if (ucg.ucg_version != UCCID_VERSION_ONE)
		return (EINVAL);

	ccid = slot->cs_ccid;
	mutex_enter(&ccid->ccid_mutex);
	if ((slot->cs_ccid->ccid_flags & CCID_FLAG_DETACHING) != 0) {
		mutex_exit(&ccid->ccid_mutex);
		return (ENODEV);
	}

	if (ccid->ccid_dev_data->dev_serial == NULL) {
		mutex_exit(&ccid->ccid_mutex);
		return (ENOENT);
	}

	len = strlen(ccid->ccid_dev_data->dev_serial) + 1;
	if (!ccid_ioctl_copyout_bufdata(ccid->ccid_dev_data->dev_serial, len,
	    &ucg, mode, &ret)) {
		return (ret);
	}
	mutex_exit(&slot->cs_ccid->ccid_mutex);

	if (!ccid_ioctl_copyout_buf(arg, mode, &ucg)) {
		return (EFAULT);
	}

	return (ret);
}

static int
ccid_ioctl_txn_begin(ccid_slot_t *slot, ccid_minor_t *cmp, intptr_t arg, int mode)
{
	int ret;
	uccid_cmd_txn_begin_t uct;
	boolean_t nowait;

	if (ddi_copyin((void *)arg, &uct, sizeof (uct), mode & FKIOCTL) != 0)
		return (EFAULT);

	if (uct.uct_version != UCCID_VERSION_ONE)
		return (EINVAL);
	if ((uct.uct_flags & ~(UCCID_TXN_DONT_BLOCK | UCCID_TXN_END_RESET |
	    UCCID_TXN_END_RELEASE)) != 0)
		return (EINVAL);
	if ((uct.uct_flags & UCCID_TXN_END_RESET) != 0 &&
	    (uct.uct_flags & UCCID_TXN_END_RELEASE) != 0) {
		return (EINVAL);
	}

	nowait = !!(uct.uct_flags & UCCID_TXN_DONT_BLOCK);

	mutex_enter(&slot->cs_ccid->ccid_mutex);
	if ((slot->cs_ccid->ccid_flags & CCID_FLAG_DETACHING) != 0) {
		mutex_exit(&slot->cs_ccid->ccid_mutex);
		return (ENODEV);
	}

	ret = ccid_slot_excl_req(slot, cmp, nowait);

	/*
	 * If successful, record whether or not we need to reset the ICC when
	 * this transaction is ended (whether explicitly or implicitly). The
	 * reason we have two flags in the ioctl, but only one here is to force
	 * the consumer to make a concrious choice in which behavior they should
	 * be using.
	 */
	if (ret == 0) {
		if (uct.uct_flags & UCCID_TXN_END_RESET) {
			cmp->cm_flags |= CCID_MINOR_F_TXN_RESET;
		}
	}
	mutex_exit(&slot->cs_ccid->ccid_mutex);

	return (ret);
}

static int
ccid_ioctl_txn_end(ccid_slot_t *slot, ccid_minor_t *cmp, intptr_t arg, int mode)
{
	int ret;
	uccid_cmd_txn_end_t uct;
	boolean_t nowait;

	if (ddi_copyin((void *)arg, &uct, sizeof (uct), mode & FKIOCTL) != 0)
		return (EFAULT);

	if (uct.uct_version != UCCID_VERSION_ONE)
		return (EINVAL);

	mutex_enter(&slot->cs_ccid->ccid_mutex);
	if ((slot->cs_ccid->ccid_flags & CCID_FLAG_DETACHING) != 0) {
		mutex_exit(&slot->cs_ccid->ccid_mutex);
		return (ENODEV);
	}

	if (slot->cs_excl_minor != cmp) {
		mutex_exit(&slot->cs_ccid->ccid_mutex);
		return (ENXIO);
	}
	VERIFY3S(cmp->cm_flags & CCID_MINOR_F_HAS_EXCL, !=, 0);
	ccid_slot_excl_rele(slot);
	mutex_exit(&slot->cs_ccid->ccid_mutex);

	return (0);
}

static int
ccid_ioctl(dev_t dev, int cmd, intptr_t arg, int mode, cred_t *credp, int
    *rvalp)
{
	ccid_minor_idx_t *idx;
	ccid_slot_t *slot;
	ccid_minor_t *cmp;

	idx = ccid_minor_find_user(getminor(dev));
	if (idx == NULL) {
		return (ENOENT);
	}

	if (idx->cmi_isslot) {
		return (ENXIO);
	}

	cmp = idx->cmi_data.cmi_user;
	slot = cmp->cm_slot;

	switch (cmd) {
	case UCCID_CMD_TXN_BEGIN:
		return (ccid_ioctl_txn_begin(slot, cmp, arg, mode));
	case UCCID_CMD_TXN_END:
		return (ccid_ioctl_txn_end(slot, cmp, arg, mode));
	case UCCID_CMD_STATUS:
		return (ccid_ioctl_status(slot, arg, mode));
	case UCCID_CMD_GETATR:
		return (ccid_ioctl_getatr(slot, arg, mode));
	case UCCID_CMD_GETPRODSTR:
		return (ccid_ioctl_getprodstr(slot, arg, mode));
	case UCCID_CMD_GETSERIAL:
		return (ccid_ioctl_getserial(slot, arg, mode));
	default:
		break;
	}

	return (ENOTTY);
}

static int
ccid_chpoll(dev_t dev, short events, int anyyet, short *reventsp,
    struct pollhead **phpp)
{
	short ready = 0;
	ccid_minor_idx_t *idx;
	ccid_minor_t *cmp;
	ccid_slot_t *slot;
	ccid_t *ccid;

	idx = ccid_minor_find_user(getminor(dev));
	if (idx == NULL) {
		return (ENOENT);
	}

	if (idx->cmi_isslot) {
		return (ENXIO);
	}

	/*
	 * First tear down the global index entry.
	 */
	cmp = idx->cmi_data.cmi_user;
	slot = cmp->cm_slot;
	ccid = slot->cs_ccid;

	mutex_enter(&ccid->ccid_mutex);
	if ((ccid->ccid_flags & CCID_FLAG_DETACHING) != 0) {
		mutex_exit(&ccid->ccid_mutex);
		return (ENODEV);
	}

	if (!(cmp->cm_flags & CCID_MINOR_F_HAS_EXCL)) {
		mutex_exit(&ccid->ccid_mutex);
		return (EACCES);
	}

	if (slot->cs_command == NULL) {
		ready |= POLLOUT;
	} else if (slot->cs_command->cc_state >= CCID_COMMAND_COMPLETE) {
		ready |= POLLIN | POLLRDNORM;
	}

	if (!(slot->cs_flags & CCID_SLOT_F_PRESENT)) {
		ready |= POLLHUP;
	}

	*reventsp = ready & events;
	if ((*reventsp == 0 && !anyyet) || (events & POLLET)) {
		*phpp = &cmp->cm_pollhead;
	}

	mutex_exit(&ccid->ccid_mutex);

	return (0);
}

static int
ccid_close(dev_t dev, int flag, int otyp, cred_t *credp)
{
	ccid_minor_idx_t *idx;
	ccid_minor_t *cmp;
	ccid_slot_t *slot;

	idx = ccid_minor_find_user(getminor(dev));
	if (idx == NULL) {
		return (ENOENT);
	}

	/*
	 * First tear down the global index entry.
	 */
	cmp = idx->cmi_data.cmi_user;
	slot = cmp->cm_slot;
	ccid_minor_idx_free(idx);

	mutex_enter(&slot->cs_ccid->ccid_mutex);
	if (cmp->cm_flags & CCID_MINOR_F_HAS_EXCL) {
		ccid_slot_excl_rele(slot);
	}

	list_remove(&slot->cs_minors, cmp);
	mutex_exit(&slot->cs_ccid->ccid_mutex);

	pollhead_clean(&cmp->cm_pollhead);
	ccid_minor_free(cmp);

	return (0);
}

static struct cb_ops ccid_cb_ops = {
	ccid_open,		/* cb_open */
	ccid_close,		/* cb_close */
	nodev,			/* cb_strategy */
	nodev,			/* cb_print */
	nodev,			/* cb_dump */
	ccid_read,		/* cb_read */
	ccid_write,		/* cb_write */
	ccid_ioctl,		/* cb_ioctl */
	nodev,			/* cb_devmap */
	nodev,			/* cb_mmap */
	nodev,			/* cb_segmap */
	ccid_chpoll,		/* cb_chpoll */
	ddi_prop_op,		/* cb_prop_op */
	NULL,			/* cb_stream */
	D_MP,			/* cb_flag */
	CB_REV,			/* cb_rev */
	nodev,			/* cb_aread */
	nodev			/* cb_awrite */
};

static struct dev_ops ccid_dev_ops = {
	DEVO_REV,		/* devo_rev */
	0,			/* devo_refcnt */
	ccid_getinfo,		/* devo_getinfo */
	nulldev,		/* devo_identify */
	nulldev,		/* devo_probe */
	ccid_attach,		/* devo_attach */
	ccid_detach,		/* devo_detach */
	nodev,			/* devo_reset */
	&ccid_cb_ops,		/* devo_cb_ops */
	NULL,			/* devo_bus_ops */
	NULL,			/* devo_power */
	ddi_quiesce_not_supported /* devo_quiesce */
};

static struct modldrv ccid_modldrv = {
	&mod_driverops,
	"USB CCID",
	&ccid_dev_ops
};

static struct modlinkage ccid_modlinkage = {
	MODREV_1,
	&ccid_modldrv,
	NULL
};

int
_init(void)
{
	int ret;

	if ((ret = ddi_soft_state_init(&ccid_softstate, sizeof (ccid_t),
	    0)) != 0) {
		return (ret);
	}

	if ((ccid_minors = id_space_create("ccid_minors", CCID_MINOR_MIN, INT_MAX)) == NULL) {
		ddi_soft_state_fini(&ccid_softstate);
		return (ret);
	}

	if ((ret = mod_install(&ccid_modlinkage)) != 0) {
		id_space_destroy(ccid_minors);
		ccid_minors = NULL;
		ddi_soft_state_fini(&ccid_softstate);
		return (ret);
	}

	mutex_init(&ccid_idxlock, NULL, MUTEX_DRIVER, NULL);
	avl_create(&ccid_idx, ccid_idx_comparator, sizeof (ccid_minor_idx_t),
	    offsetof(ccid_minor_idx_t, cmi_avl));

	return (ret);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&ccid_modlinkage, modinfop));
}

int
_fini(void)
{
	int ret;

	if ((ret = mod_remove(&ccid_modlinkage)) != 0) {
		return (ret);
	}

	avl_destroy(&ccid_idx);
	mutex_destroy(&ccid_idxlock);
	id_space_destroy(ccid_minors);
	ccid_minors = NULL;
	ddi_soft_state_fini(&ccid_softstate);

	return (ret);
}
