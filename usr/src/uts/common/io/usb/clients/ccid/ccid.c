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
 * CCID_FLAG_DISCOVER_REQUESTED and CCID_FLAG_DISCOVER_RUNNING. The first is used
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

typedef enum ccid_slot_flags {
	CCID_SLOT_F_CHANGED	= 1 << 0,
	CCID_SLOT_F_INTR_GONE	= 1 << 1,
	CCID_SLOT_F_INTR_ADD	= 1 << 2,
	CCID_SLOT_F_PRESENT	= 1 << 3,
	CCID_SLOT_F_ACTIVE	= 1 << 4
} ccid_slot_flags_t;

#define	CCID_SLOT_F_INTR_MASK	(CCID_SLOT_F_CHANGED | CCID_SLOT_F_INTR_GONE | \
    CCID_SLOT_F_INTR_ADD)

typedef struct ccid_slot {
	uint_t			cs_slotno;	/* WO */
	kmutex_t		cs_mutex;
	ccid_slot_flags_t	cs_flags;
	ccid_class_voltage_t	cs_voltage;
	mblk_t			*cs_atr;
} ccid_slot_t;

typedef enum ccid_attach_state {
	CCID_ATTACH_USB_CLIENT	= 1 << 0,
	CCID_ATTACH_MUTEX_INIT	= 1 << 1,
	CCID_ATTACH_TASKQ	= 1 << 2,
	CCID_ATTACH_CMD_LIST	= 1 << 3,
	CCID_ATTACH_OPEN_PIPES	= 1 << 4,
	CCID_ATTACH_ID_SPACE	= 1 << 5,
	CCID_ATTACH_SLOTS	= 1 << 6,
	CCID_ATTACH_HOTPLUG_CB	= 1 << 7,
	CCID_ATTACH_INTR_ACTIVE	= 1 << 8,
	CCID_ATTACH_ACTIVE	= 1 << 9
} ccid_attach_state_t;

typedef enum ccid_flags {
	CCID_FLAG_HAS_INTR		= 1 << 0,
	CCID_FLAG_DETACHING		= 1 << 1,
	CCID_FLAG_DISCOVER_REQUESTED	= 1 << 2,
	CCID_FLAG_DISCOVER_RUNNING	= 1 << 3,
	CCID_FLAG_DISCONNECTED		= 1 << 4
} ccid_flags_t;

#define	CCID_FLAG_DISCOVER_MASK	(CCID_FLAG_DISCOVER_REQUESTED | \
    CCID_FLAG_DISCOVER_RUNNING)

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
} ccid_command_state;

typedef struct ccid_command {
	list_node_t	cc_list_node;
	kcondvar_t	cc_cv;
	uint8_t		cc_mtype;
	uint8_t		cc_slot;
	ccid_command_state	cc_state;
	int		cc_usb;
	usb_cr_t	cc_usbcr;
	size_t		cc_reqlen;
	id_t		cc_seq;
	usb_bulk_req_t	*cc_ubrp;
	ccid_t		*cc_ccid;
	hrtime_t	cc_queue_time;
	hrtime_t	cc_dispatch_time;
	hrtime_t	cc_dispatch_cb_time;
	hrtime_t	cc_response_time;
	hrtime_t	cc_completion_time;
	mblk_t		*cc_response;
} ccid_command_t;

/*
 * ddi_soft_state(9F) pointer.
 */
static void *ccid_softstate;

/*
 * Required Forwards
 */
static void ccid_intr_poll_init(ccid_t *);
static void ccid_discover_request(ccid_t *);
static void ccid_command_dispatch(ccid_t *);
static int ccid_bulkin_schedule(ccid_t *);

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
ccid_command_complete(ccid_command_t *cc)
{
	ccid_t *ccid = cc->cc_ccid;

	VERIFY(MUTEX_HELD(&ccid->ccid_mutex));
	cc->cc_completion_time = gethrtime();
	list_remove(&ccid->ccid_command_queue, cc);
	list_insert_tail(&ccid->ccid_complete_queue, cc);
	cv_broadcast(&cc->cc_cv);

	/*
	 * Finally, we also need to kick off the next command.
	 */
	ccid_command_dispatch(ccid);
}

static void
ccid_command_transport_error(ccid_command_t *cc, int usb_status, usb_cr_t cr)
{
	VERIFY(MUTEX_HELD(&cc->cc_ccid->ccid_mutex));

	cc->cc_state = CCID_COMMAND_TRANSPORT_ERROR;
	cc->cc_usb = usb_status;
	cc->cc_usbcr = cr;
	cc->cc_response = NULL;

	ccid_command_complete(cc);
}

static void
ccid_command_status_decode(ccid_command_t *cc, ccid_reply_command_status_t *comp,
    ccid_reply_slot_status_t *slotp, ccid_command_err_t *errp)
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

	if (slotp != NULL) { 
		*slotp = CCID_REPLY_SLOT(cch.ch_param0);
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
		cc->cc_state = CCID_COMMAND_CCID_ABORTED;
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
		cc->cc_state = CCID_COMMAND_CCID_ABORTED;
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
	cc->cc_state = CCID_COMMAND_COMPLETE;
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
		cc->cc_state = CCID_COMMAND_DISPATCHED;
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
	cc->cc_state = CCID_COMMAND_QUEUED;
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
	cc->cc_state = CCID_COMMAND_REPLYING;
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
    size_t datasz, uint8_t mtype, uint8_t param0, uint8_t param1,
    uint8_t param2, ccid_command_t **ccp)
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
		kmflag = KM_NOSLEEP;
		usbflag = 0;
	}

	if (datasz + sizeof (ccid_header_t) < datasz)
		return (EINVAL);
	allocsz = datasz + sizeof (ccid_header_t);
	if (datasz > ccid->ccid_bufsize)
		return (EINVAL);

	cc = kmem_zalloc(sizeof (ccid_command_t), kmflag);
	if (cc == NULL)
		return (ENOMEM);

	cc->cc_ubrp = usb_alloc_bulk_req(ccid->ccid_dip, allocsz, usbflag);
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
ccid_command_slot_status(ccid_t *ccid, ccid_slot_t *slot,
    ccid_reply_slot_status_t *ssp)
{
	int ret;
	ccid_command_t *cc;

	if ((ret = ccid_command_alloc(ccid, slot, B_TRUE, 0,
	    CCID_REQUEST_SLOT_STATUS, 0, 0, 0, &cc)) != 0) {
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

	ccid_command_status_decode(cc, NULL, ssp, NULL);
	ret = 0;
done:
	ccid_command_free(cc);
	return (ret);
}

static int
ccid_command_power_off(ccid_t *ccid, ccid_slot_t *cs)
{
	int ret;
	ccid_command_t *cc;
	ccid_reply_command_status_t crs;

	if ((ret = ccid_command_alloc(ccid, cs, B_TRUE, 0,
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

	ccid_command_status_decode(cc, &crs, NULL, NULL);
	if (cs != 0) {
		ret = EIO;
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
	ccid_reply_slot_status_t css;
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

	if ((ret = ccid_command_alloc(ccid, cs, B_TRUE, 0,
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
	ccid_command_status_decode(cc, &crs, &css, &cce);
	if (crs == CCID_REPLY_STATUS_FAILED) {
		if (css == CCID_REPLY_SLOT_MISSING) {
			ret = ENOENT;
		} else if (css == CCID_REPLY_SLOT_INACTIVE &&
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
		mutex_exit(&ccid->ccid_mutex);

		explen = 1 + ((2 * ccid->ccid_nslots + (NBBY-1)) / NBBY);
		if (msglen < explen) {
			mutex_enter(&ccid->ccid_mutex);
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
				ccid_slot_t *cs = &ccid->ccid_slots[i];

				mutex_enter(&cs->cs_mutex);
				cs->cs_flags &= ~CCID_SLOT_F_INTR_MASK;
				cs->cs_flags |= CCID_SLOT_F_CHANGED;
				if (mp->b_rptr[byte] & present) {
					cs->cs_flags |= CCID_SLOT_F_INTR_ADD;
				} else {
					cs->cs_flags |= CCID_SLOT_F_INTR_GONE;
				}
				mutex_exit(&cs->cs_mutex);
				change = B_TRUE;
			}
		}

		if (change) {
			ccid_discover_request(ccid);
		}
		break;
	case CCID_INTR_CODE_HW_ERROR:
		mutex_enter(&ccid->ccid_mutex);
		ccid->ccid_stats.cst_intr_hwerr++;
		mutex_exit(&ccid->ccid_mutex);

		if (msglen < sizeof (ccid_intr_hwerr_t)) {
			mutex_enter(&ccid->ccid_mutex);
			ccid->ccid_stats.cst_intr_inval++;
			mutex_exit(&ccid->ccid_mutex);
			goto done;
		}

		/* XXX what should we do with this? */
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
ccid_slot_removed(ccid_t *ccid, ccid_slot_t *cs)
{
	/*
	 * Nothing to do right now.
	 */
	mutex_enter(&cs->cs_mutex);
	if ((cs->cs_flags & CCID_SLOT_F_PRESENT) == 0) {
		mutex_exit(&cs->cs_mutex);
		return;
	}
	cs->cs_flags &= ~CCID_SLOT_F_PRESENT;
	cs->cs_flags &= ~CCID_SLOT_F_ACTIVE;
	cs->cs_voltage = 0;
	freemsgchain(cs->cs_atr);
	cs->cs_atr = NULL;
	mutex_exit(&cs->cs_mutex);
}

static void
ccid_slot_inserted(ccid_t *ccid, ccid_slot_t *cs)
{
	uint_t nvolts = 4;
	uint_t cvolt = 0;
	mblk_t *atr = NULL;
	ccid_class_voltage_t volts[4] = { CCID_CLASS_VOLT_AUTO,
	    CCID_CLASS_VOLT_5_0, CCID_CLASS_VOLT_3_0, CCID_CLASS_VOLT_1_8 };

	mutex_enter(&cs->cs_mutex);
	if ((cs->cs_flags & CCID_SLOT_F_ACTIVE) != 0) {
		mutex_exit(&cs->cs_mutex);
		return;
	}

	cs->cs_flags |= CCID_SLOT_F_PRESENT;
	mutex_exit(&cs->cs_mutex);

	ccid_reply_slot_status_t ss;
	(void) ccid_command_slot_status(ccid, cs, &ss);
	(void) ccid_command_slot_status(ccid, cs, &ss);
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

		if ((ret = ccid_command_power_on(ccid, cs, volts[cvolt],
		    &atr)) != 0) {
			freemsg(atr);
			atr = NULL;

			/*
			 * If we got ENOENT, then we know that there is no CCID
			 * present. This could happen for a number of reasons.
			 * For example, we could have just started up and no
			 * card was plugged in (we default to assuming that one
			 * is). Also, some readers won't really tell us that
			 * nothing is there until after the power on fails,
			 * hence why we don't bother with doing a status check
			 * and just try to power on.
			 */
			if (ret == ENOENT) {
				mutex_enter(&cs->cs_mutex);
				cs->cs_flags &= ~CCID_SLOT_F_PRESENT;
				mutex_exit(&cs->cs_mutex);
				return;
			}
			/* XXX we should probably worry about failure */
			(void) ccid_command_power_off(ccid, cs);
			continue;
		}

		break;
	}


	if (cvolt >= nvolts) {
		ccid_error(ccid, "!failed to activate and power on ICC, no "
		    "supported voltages found");
		return;
	}


	mutex_enter(&cs->cs_mutex);
	cs->cs_voltage = volts[cvolt];
	cs->cs_atr = atr;
	cs->cs_flags |= CCID_SLOT_F_ACTIVE;
	mutex_exit(&cs->cs_mutex);
}

static void
ccid_discover(void *arg)
{
	uint_t i;
	ccid_t *ccid = arg;

	mutex_enter(&ccid->ccid_mutex);
	ccid->ccid_stats.cst_ndiscover++;
	ccid->ccid_stats.cst_lastdiscover = gethrtime();
	if (ccid->ccid_flags & CCID_FLAG_DETACHING) {
		ccid->ccid_flags &= ~CCID_FLAG_DISCOVER_MASK;
		mutex_exit(&ccid->ccid_mutex);
		return;
	}
	ccid->ccid_flags |= CCID_FLAG_DISCOVER_RUNNING;
	ccid->ccid_flags &= ~CCID_FLAG_DISCOVER_REQUESTED;
	mutex_exit(&ccid->ccid_mutex);

	for (i = 0; i < ccid->ccid_nslots; i++) {
		ccid_slot_t *cs = &ccid->ccid_slots[i];
		ccid_reply_slot_status_t ss;
		int ret;
		uint_t flags;

		mutex_enter(&cs->cs_mutex);

		/*
		 * Snapshot flags at this point in time before we start
		 * processing. Note that the flags may change as soon as we drop
		 * this lock the slot lock, which will happen as part or
		 * processing the commands.
		 */
		flags = cs->cs_flags & CCID_SLOT_F_INTR_MASK;
		cs->cs_flags &= ~CCID_SLOT_F_INTR_MASK;

		if ((flags & CCID_SLOT_F_CHANGED) == 0) {
			mutex_exit(&cs->cs_mutex);
			continue;
		}

		mutex_exit(&cs->cs_mutex);

		/*
		 * We either have a hardware notified insertion or removal or we
		 * have something that we think might have been inserted.
		 */
		if (flags & CCID_SLOT_F_INTR_GONE) {
			ccid_slot_removed(ccid, cs);
		} else {
			ccid_slot_inserted(ccid, cs);
		}

	}

	mutex_enter(&ccid->ccid_mutex);
	ccid->ccid_flags &= ~CCID_FLAG_DISCOVER_RUNNING;
	if (ccid->ccid_flags & CCID_FLAG_DETACHING) {
		mutex_exit(&ccid->ccid_mutex);
		return;
	}

	if ((ccid->ccid_flags & CCID_FLAG_DISCOVER_REQUESTED) != 0 &&
	    (ccid->ccid_flags & CCID_FLAG_DETACHING) == 0) {
		(void) ddi_taskq_dispatch(ccid->ccid_taskq, ccid_discover, ccid,
		    DDI_SLEEP);
	}
	mutex_exit(&ccid->ccid_mutex);
}

static void
ccid_discover_request(ccid_t *ccid)
{
	boolean_t run;

	mutex_enter(&ccid->ccid_mutex);
	if (ccid->ccid_flags & CCID_FLAG_DETACHING) {
		mutex_exit(&ccid->ccid_mutex);
		return;
	}

	run = (ccid->ccid_flags & CCID_FLAG_DISCOVER_MASK) == 0; 
	ccid->ccid_flags |= CCID_FLAG_DISCOVER_REQUESTED;
	if (run) {
		(void) ddi_taskq_dispatch(ccid->ccid_taskq, ccid_discover, ccid,
		    DDI_SLEEP);
	}
	mutex_exit(&ccid->ccid_mutex);
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

		freemsgchain(ccid->ccid_slots[i].cs_atr);
		mutex_destroy(&ccid->ccid_slots[i].cs_mutex);
	}
	kmem_free(ccid->ccid_slots, sizeof (ccid_slot_t) * ccid->ccid_nslots);
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
		mutex_init(&ccid->ccid_slots[i].cs_mutex, NULL, MUTEX_DRIVER,
		    ccid->ccid_dev_data->dev_iblock_cookie);
		/*
		 * We initialize every possible slot as having changed to make
		 * sure that we have a chance to discover it. See the slot
		 * detection section in the big theory statement for more info.
		 */
		ccid->ccid_slots[i].cs_flags |= CCID_SLOT_F_CHANGED;
		ccid->ccid_slots[i].cs_slotno = i;
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

	/*
	 * XXX we need to handle outstanding commands before detaching or
	 * refuse.
	 */

	/*
	 * Now, let's make sure that we stop any ongoing discovery. The
	 * detaching flag will make sure that nothing else schedules anything.
	 */
	if (ccid->ccid_attach & CCID_ATTACH_ACTIVE) {
		ddi_taskq_wait(ccid->ccid_taskq);
		mutex_enter(&ccid->ccid_mutex);
		VERIFY0(ccid->ccid_flags & CCID_FLAG_DISCOVER_MASK);
		mutex_exit(&ccid->ccid_mutex);
		ccid->ccid_attach &= ~CCID_ATTACH_ACTIVE;
	}

	if (ccid->ccid_attach & CCID_ATTACH_INTR_ACTIVE) {
		ccid_intr_poll_fini(ccid);
		ccid->ccid_attach &= ~CCID_ATTACH_INTR_ACTIVE;
	}

	if (ccid->ccid_attach & CCID_ATTACH_HOTPLUG_CB) {
		usb_unregister_event_cbs(dip, &ccid_usb_events);
		ccid->ccid_attach &= ~CCID_ATTACH_HOTPLUG_CB;
	}

	if (ccid->ccid_attach & CCID_ATTACH_SLOTS) {
		ccid_slots_fini(ccid);
		ccid->ccid_attach &= ~CCID_ATTACH_SLOTS;
	}

	if (ccid->ccid_attach & CCID_ATTACH_ID_SPACE) {
		id_space_destroy(ccid->ccid_seqs);
		ccid->ccid_seqs = NULL;
		ccid->ccid_attach &= ~CCID_ATTACH_ID_SPACE;
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
		VERIFY(list_is_empty(&ccid->ccid_command_queue));
		list_destroy(&ccid->ccid_command_queue);
		VERIFY(list_is_empty(&ccid->ccid_complete_queue));
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
		ccid_cleanup(dip);
		return (DDI_FAILURE);
	}
	ccid->ccid_attach |= CCID_ATTACH_USB_CLIENT;

	if ((ret = usb_get_dev_data(dip, &ccid->ccid_dev_data, USB_PARSE_LVL_IF,
	    0)) != USB_SUCCESS) {
		ccid_error(ccid, "failed to get usb device data: %d", ret);
		ccid_cleanup(dip);
		return (DDI_FAILURE);
	}

	mutex_init(&ccid->ccid_mutex, NULL, MUTEX_DRIVER,
	    ccid->ccid_dev_data->dev_iblock_cookie);
	ccid->ccid_attach |= CCID_ATTACH_MUTEX_INIT;

	(void) snprintf(buf, sizeof (buf), "ccid%d_taskq", inst);
	ccid->ccid_taskq = ddi_taskq_create(dip, buf, 1, TASKQ_DEFAULTPRI, 0);;
	if (ccid->ccid_taskq == NULL) {
		ccid_error(ccid, "failed to create CCID taskq");
		ccid_cleanup(dip);
		return (DDI_FAILURE);
	}
	ccid->ccid_attach |= CCID_ATTACH_TASKQ;

	list_create(&ccid->ccid_command_queue, sizeof (ccid_command_t),
	    offsetof(ccid_command_t, cc_list_node));
	list_create(&ccid->ccid_complete_queue, sizeof (ccid_command_t),
	    offsetof(ccid_command_t, cc_list_node));

	if (!ccid_parse_class_desc(ccid)) {
		ccid_error(ccid, "failed to parse CCID class descriptor");
		ccid_cleanup(dip);
		return (DDI_FAILURE);
	}

	if (!ccid_supported(ccid)) {
		ccid_error(ccid, "CCID reader is not supported, not attaching");
		ccid_cleanup(dip);
		return (DDI_FAILURE);
	}

	if (!ccid_open_pipes(ccid)) {
		ccid_error(ccid, "failed to open CCID pipes, not attaching");
		ccid_cleanup(dip);
		return (DDI_FAILURE);
	}
	ccid->ccid_attach |= CCID_ATTACH_OPEN_PIPES;

	(void) snprintf(buf, sizeof (buf), "ccid%d_seqs", inst);
	if ((ccid->ccid_seqs = id_space_create(buf, CCID_SEQ_MIN,
	    CCID_SEQ_MAX + 1)) == NULL) {
		ccid_error(ccid, "failed to create CCID sequence id space");
		ccid_cleanup(dip);
		return (DDI_FAILURE);
	}
	ccid->ccid_attach |= CCID_ATTACH_ID_SPACE;

	if (!ccid_slots_init(ccid)) {
		ccid_error(ccid, "failed to initialize CCID slot structures");
		ccid_cleanup(dip);
		return (DDI_FAILURE);
	}
	ccid->ccid_attach |= CCID_ATTACH_SLOTS;

	if (usb_register_event_cbs(dip, &ccid_usb_events, 0) != USB_SUCCESS) {
		ccid_error(ccid, "failed to register USB hotplug callbacks");
		ccid_cleanup(dip);
		return (DDI_FAILURE);
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
	 * Before we kick off discovery, mark that we're fully active at this
	 * point.
	 */
	ccid->ccid_attach |= CCID_ATTACH_ACTIVE;
	ccid_discover_request(ccid);

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

	/*
	 * XXX we should probably figure out how to not detach from being idle,
	 * only when being unplugged.
	 */

	ccid_cleanup(dip);
	return (DDI_SUCCESS);
}

static struct cb_ops ccid_cb_ops = {
	nulldev,		/* cb_open */
	nulldev,		/* cb_close */
	nodev,			/* cb_strategy */
	nodev,			/* cb_print */
	nodev,			/* cb_dump */
	nodev,			/* cb_read */
	nodev,			/* cb_write */
	nodev,			/* cb_ioctl */
	nodev,			/* cb_devmap */
	nodev,			/* cb_mmap */
	nodev,			/* cb_segmap */
	nochpoll,		/* cb_chpoll */
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

	if ((ret = mod_install(&ccid_modlinkage)) != 0) {
		ddi_soft_state_fini(&ccid_softstate);
		return (ret);
	}

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

	ddi_soft_state_fini(&ccid_softstate);

	return (ret);
}
