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
 * Two state flags are used to keep track of this dance: CCID_F_WORKER_REQUESTED
 * and CCID_F_WORKER_RUNNING. The first is used to indicate that discovery is
 * desired. The second is to indicate that it is actively running. When
 * discovery is requested, the caller first checks to make sure the current
 * flags. If neither flag is set, then it knows that it can kick off discovery.
 * Regardless if it can kick off the taskq, it always sets requested. Once the
 * taskq entry starts, it removes any DISCOVER_REQUESTED flags and sets
 * DISCOVER_RUNNING. If at the end of discovery, we find that another request
 * has been made, the discovery function will kick off another entry in the
 * taskq.
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
 *
 * APDU and TPDU Processing and Parameter Selection
 * ------------------------------------------------
 *
 * Readers provide four different modes for us to be able to transmit data to
 * and from the card. These are:
 *
 * 1. Character Mode 2. TPDU Mode 3. Short APDU Mode 4. Extended APDU Mode
 *
 * Devices either support mode 1, mode 2, mode 3, or mode 3 and 4. All readers
 * that support extended APDUs support short APDUs. At this time, we do not
 * support character mode.
 *
 * The ICC and the reader need to be in agreement in order for them to be able
 * to exchange information. The ICC indicates what it supports by replying to a
 * power on command with an ATR (answer to reset). This data can be parsed to
 * indicate which of two protocols the ICC supports. These protocols are
 * referred to as:
 *
 *  o T=0
 *  o T=1
 *
 * These protocols are defined in the ISO/IEC 7816-3:2006 specification. When a
 * reader supports an APDU mode, then it does not have to worry about the
 * underlying protocol and can just send an application data unit (APDU).
 * Otherwise, the reader must take the application data (APDU) and transform it
 * into the form required by the corresponding protocol.
 *
 * There are several parameters that need to be negotiated to insure that the
 * protocols work correctly. To negotiate these parameters and to select a
 * protocol, the reader must construct a PPS (protocol and parameters structure)
 * request and exchange that with the ICC. A reader may optionally take care of
 * performing this and indicates its support for this in dwFeatures member of
 * the USB class descriptor.
 *
 * In addition, the reader itself must often be told of these configuration
 * changes through the means of a CCID_REQUEST_SET_PARAMS command. Once both of
 * these have been performed, the reader and ICC can communicate to their hearts
 * desire.
 *
 * Both the negotiation and the setting of the parameters can be performed
 * automatically by the CCID reader. When the reader supports APDU exchanges,
 * then it must support some aspects of this negotiation. Because of that, we
 * never consider performing this for APDU related activity and only worry about
 * this for TPDU transfers.
 *
 * In the ATR data the device can indicate whether or not it supports
 * negotiating this information. If the hardware does not support negotiation,
 * then it likely does not support a PPS and in which case we need to program
 * the hardware with the parameters indicated by the ATR through a
 * CCID_REQUEST_SET_PARAMS command and do not need to negotiation a PPS.
 * 
 * Many ICC devices support negotiation. When an ICC supporting negotiation is
 * first turned on then it enters into a default mode and uses the default
 * values while in that mode. The PPS may be used to change the protocol as well
 * as several parameters. Once the PPS has been agreed upon, this driver just
 * send a CCID_REQUEST_SET_PARAMS command to inform the reader what is going on.
 *
 * If the CCID reader supports neither of the hardware related mechanisms for a
 * PPS exchange, then we must do both of these. If hardware supports automatic
 * parameter negotiation then we do not need to send either the PPS or the
 * CCID_REQUEST_SET_PARAMS command.
 *
 * The ATR offers us what the hardware's maximum value of Di and Fi are. If the
 * reader supports higher speeds, then we will 
 *
 * XXX At the moment we're not adjusting any of the Di or Fi values beyond their
 * default.
 *
 * To summarize this all, the following is the flow chart we perform after
 * successfully powering on the device:
 *
 *  - If the reader supports APDU transfers, then we are done.
 *     XXX Depending on level of automation we may need to still do things.
 *  - If the reader supports 
 *
 * User I/O Basics
 * ---------------
 *
 * A user performs I/O by writing APDUs (Application Protocol Data Units). A
 * user issues a system call that ends up in write(9E) (write(2), writev(2),
 * pwrite(2), pwritev(2), etc.). The user data is consumed by the CCID driver
 * and a series of commands will then be issued to the device, depending on the
 * protocol mode. The write(9E) call does not block for this to finish. Once
 * write(9E) has returned, the user may block in a read(2) related system call
 * or poll for POLLIN.
 *
 * A thread may not call read(9E) without having called write(9E). This model is
 * due to the limited capability of hardware. Only a single command can be going
 * on a given slot and due to the fact that many commands change the hardware
 * state, we do not try to multiplex multiple calls to write() or read().
 *
 *
 * User I/O, Transaction Ends, ICC removals, and Reader Removals
 * -------------------------------------------------------------
 *
 * While the I/O model given to user land is somewhat simple, there are a lot of
 * tricky pieces to get right because we are in a multi-threaded pre-emptible
 * system. In general, there are four different levels of state that we need to
 * keep track of:
 *
 *   1. User threads in I/O
 *   2. Kernel protocol level support (T=1, apdu, etc.).
 *   3. Slot/ICC state
 *   4. CCID Reader state
 *
 * Of course, each level cares about the state above it. The kernel protocol
 * level state (2), cares about the User threads in I/O (1). The same is true
 * with the other levels caring about the levels above it. With this in mind
 * there are three non-data path things that can go wrong:
 *
 *   A. The user can end a transaction (whether through an ioctl or close(9E)).
 *   B. The ICC can be removed
 *   C. The CCID device can be removed or is reset at a USB level.
 *
 * Each of these has implications on the outstanding I/O and other states of
 * the world. When events of type A occur, we need to clean up states 1 and 2.
 * Then events of type B occur we need to clean up states 1-3. When events of
 * type C occur we need to clean up states 1-4. The following discusses how we
 * should clean up these different states:
 *
 * Cleaning up State 1:
 *
 *   To clean up the User threads in I/O there are three different cases to
 *   consider. The first is cleaning up a thread that is in the middle of
 *   write(9E). The second is cleaning up thread that is blocked in read(9E).
 *   The third is dealing with threads that are stuck in chpoll(9E).
 *
 *   To handle the write case, we have a series of flags that is on the CCID
 *   slot's I/O structure (ccid_io_t, cs_io on the ccid_slot_t). When a thread
 *   begins its I/O it will set the CCID_IO_F_PREPARING flag. This flag is used
 *   to indicate that there is a thread that is performing a write(9E), but it
 *   is not holding the ccid_mutex because of the operations that it is taking.
 *   Once it has finished, the thread will remove that flag and instead
 *   CCID_IO_F_IN_PROGRESS will be set. If we find that the CCID_IO_F_PREPARING
 *   flag is set, then we will need to wait for it to be removed before
 *   continuing. The fact that there is an outstanding physical I/O will be
 *   dealt with when we clean up state 2.
 *
 *   To handle the read case, we have a flag on the ccid_minor_t which indicates
 *   that a thread is blocked on a condition variable (cm_read_cv), waiting for
 *   the I/O to complete. The way this gets cleaned up varies a bit on each of
 *   the different cases as each one will trigger a different error to the
 *   thread. In all cases, the condition variable will be signaled. Then,
 *   whenever the thread comes out of the condition variable it will always
 *   check the state to see if it has been woken up because the transaction is
 *   being closed, the ICC has been removed, or the reader is being
 *   disconnected. In all such cases, the thread in read will end up receiving
 *   an error (ECANCELED, ENXIO, and ENODEV respectively).
 *
 *   If we have hit the case that this needs to be cleaned up, then the
 *   CCID_MINOR_F_READ_WAITING flag will be set on the ccid_minor_t's flags
 *   member (cm_flags). In this case, the broader system must change the
 *   corresponding system state flag for the appropriate condition, signal the
 *   read cv, and then wait on an additional cv in the minor, the
 *   ccid_iowait_cv).
 *
 *   Cleaning up the poll state is somewhat simpler. If any of the conditions
 *   (A-C) occur, then we must flag POLLERR. In addition if B and C occur, then
 *   we will flag POLLHUP at the same time. This will guarantee that any threads
 *   in poll(9E) are woken up.
 *
 * Cleaning up State 2.
 *
 *   While the user I/O thread is a somewhat straightforward, the kernel
 *   protocol level is a bit more complicated. The core problem is that when a
 *   user issues a logical I/O through an APDU, that may result in a series of
 *   one or protocol level, physical commands. The core crux of the issue with
 *   cleaning up this state is twofold:
 *
 *     1. We don't want to block a user thread while I/O is outstanding
 *     2. We need to take one of several steps to clean up the aforementioned
 *        I/O
 *
 *   To try and deal with that, there are a number of different things that we
 *   do. The first thing we do is that we clean up the user state based on the
 *   notes in cleaning up in State 1. Importantly we need to _block_ on this
 *   activity.
 *
 *   Once that is done, we need to proceed to step 2. The way that this happens
 *   will depend on the protocol in use and the state that it has. For example,
 *   when performing APDU processing, this is as simple as waiting for that
 *   command to complete and/or potentially issues an abort or reset. However,
 *   for TPDU T=1 processing, we may need to issue subsequent commands to abort
 *   the state. The amount of work that we do depends on what the user
 *   configured options are when they ended the transaction. They may tell us to
 *   either reset or to keep the card in the same state.
 *
 *   While this is ongoing an additional flag (XXX) will be set on the slot to
 *   make sure that we know that we can't issue new I/O or that we can't proceed
 *   to the next transaction until this phase is finished. XXX This feels rather
 *   rough.
 *
 * Cleaning up State 3
 *
 *   When the ICC is removed, this is not dissimilar to the previous states. To
 *   handle this we need to first make sure that state 1 and state 2 are
 *   finished being cleaned up. We will have to _block_ on this from the worker
 *   thread. The problem is that we have certain values such as the operations
 *   vector, the ATR data, etc. that we need to make sure are still valid while
 *   we're in the process of cleaning up state. Only once all that is done
 *   should we consider processing a new ICC insertion or dealing with other
 *   aspects of this. The one good side is that if the ICC was removed, then it
 *   should be simpler to handle all of the outstanding I/O.
 *
 *   XXX We need more details about how all this happens, etc.
 *
 * Cleaning up State 4
 *
 *   When the reader is removed, then we need to clean up all the prior states.
 *   However, this is somewhat simpler than the other cases, as once this
 *   happens our detach endpoint will be called to clean up all of our
 *   resources. Therefore, before we call detach, we need to explicitly clean up
 *   state 1; however, we then at this time leave all the remaining state to be
 *   cleaned up during detach(9E) as part of normal tear down.
 *
 *   XXX Is that really true, this seems like a lot of BS.
 */

/*
 * Various XXX:
 *
 * o If hardware says that the ICC became shut down / disactivated. Should we
 * explicitly reactivate it as part of something or just make that a future
 * error?
 *  - Should we provide an ioctl to try to reactivate?
 *
 * o There is a series of edge cases that we need to handle with both read /
 *   write. These include:
 *
 *   + I/O in flight when end transaction occurs
 *       o Quiesce the I/O (may involve reset and abort) from a kernel
 *         perspective
 *       o Hand off to next transaction only when above is complete
 *       o POLLERR should signaled on the minor's pollhead
 *   + I/O in flight when ICC is removed
 *       o Quiesce the I/O from a kernel perspective
 *       o End the I/O with an ENXIO (maybe ECONNRESET?) from a user perspective
 *       o Kernel worker thread should block on kernel clean up, but not user
 *         consumption. We should not call into rx function to try and consume /
 *         clean up. It should get cleaned up by other functions.
 *       o POLLOUT is not signalled until both I/O is consumed and new ICC is
 *         present
 *       o POLLIN | POLLHUP should be signaled
 *   + I/O in flight when reader is removed
 *       o Ensure that I/O is quiesced from a kernel perspective, nothing should
 *         be queued for user
 *       o POLLERR | POLLHUP should be signaled to tell the user that this I/O
 *         is not coming back.
 *   + Blocked in read when an end transaction occurs
 *       o Quiesce the I/O (may involve reset and abort) form a kernel
 *         perspective
 *       o Signal and wake up the thread blocked in read(), it should get
 *         ECANCELED
 *       o Don't allow the transaction hand off to progress until read thread is
 *         gone
 *       o Follow all of the I/O in flight when transaction ends steps
 *   + Blocked in read when an ICC is removed
 *       o follow all I/O in flight when ICC is removed steps
 *       o Signal and wake up the thread blocked in read() to get the error set
 *         on disconnect.
 *   + Blocked in read when an reader is removed
 *       o Follow all normal I/O steps when reader removed
 *       o Signal and wake up the thread blocked in read(). It should check the
 *         DISCONNECTED, not the DETACHED flag.
 *   + Unread, but completed I/O when an end transaction occurs w/ ICC
 *       o Consume logical I/O state. Do not signal in this case
 *       o Potentially warm reset ICC
 *       o POLLERR should be raised with transaction end
 *   + Unread, but completed I/O when an end transaction occurs w/o ICC
 *       o Consume logical I/O state. Do not signal.
 *       o POLLERR should be raised with transaction end
 *   + Unread, but completed I/O when the ICC is removed
 *       o XXX This one is tricky, because we might want to reset our T=1 state
 *         on insertion of a new ICC before this is read. Ugh. Maybe we should
 *         pull out the mblk_t chain when the I/O is completed so we can
 *         disassociate this state. 
 *       o Still need to signal POLLHUP, but POLLIN should already have been
 *         done
 *   + Unread, but completed I/O when the reader is removed
 *       o POLLERR | POLLHUP? should be signaled on the device
 *       o Outstanding I/O should be cleaned up as part of minor close
 *
 * o Proper POLLOUT on ICC insertion / activation
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

#include <atr.h>
#include <ccid_t1.h>

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
 * This value represents the minimum size value that we require in the CCID
 * class descriptor's dwMaxCCIDMessageLength member. We got to 64 bytes based on
 * the required size of a bulk transfer packet size. Especially as many CCID
 * devices are these class of speeds. The specification does require that the
 * minimu size of the dwMaxCCIDMessageLength member is at least the size of its
 * bulk endpoint packet size.
 */
#define	CCID_MIN_MESSAGE_LENGTH	64

/*
 * Required forward declarations.
 */
struct ccid;
struct ccid_slot;
struct ccid_minor;
struct ccid_command;

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
	CCID_MINOR_F_READ_WAITING	= 1 << 3,
	CCID_MINOR_F_TXN_ENDING		= 1 << 4
} ccid_minor_flags_t;

typedef struct ccid_minor {
	ccid_minor_idx_t	cm_idx;		/* WO */
	cred_t			*cm_opener;	/* WO */
	struct ccid_slot	*cm_slot;	/* WO */
	list_node_t		cm_minor_list;
	list_node_t		cm_excl_list;
	kcondvar_t		cm_read_cv;
	kcondvar_t		cm_iowait_cv;
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

typedef int (*icc_transmit_func_t)(struct ccid *, struct ccid_slot *);
typedef void (*icc_complete_func_t)(struct ccid *, struct ccid_slot *,
    struct ccid_command *);
typedef int (*icc_read_func_t)(struct ccid *, struct ccid_slot *, struct uio *);
typedef void (*icc_excl_clean_func_t)(struct ccid *, struct ccid_slot *);
typedef void (*icc_teardown_func_t)(struct ccid *, struct ccid_slot *, boolean_t);

typedef struct ccid_icc {
	atr_data_t		*icc_atr_data;
	atr_protocol_t		icc_protocols;
	atr_protocol_t		icc_cur_protocol;
	ccid_params_t		icc_params;
	icc_transmit_func_t	icc_tx;
	icc_complete_func_t	icc_complete;
	icc_read_func_t		icc_rx;
	icc_teardown_func_t	icc_teardown;
} ccid_icc_t;

/*
 * Structure used to take care of and map I/O requests and things. This may not
 * make sense as we develop the T=0 and T=1 code.
 */
typedef enum ccid_io_flags {
	/*
	 * This flag is used during the period that a thread has started calling
	 * into ccid_write(9E), but before it has finished queuing up the write.
	 * This blocks pollout or another thread in write.
	 */
	CCID_IO_F_PREPARING	= 1 << 0,
	/*
	 * This flag is used once a ccid_write() ICC tx function has
	 * successfully completed. While this is set, the device is not
	 * writable; however, it is legal to call ccid_read() and block. This
	 * flag will remain set until the actual write is done.
	 */
	CCID_IO_F_IN_PROGRESS	= 1 << 1,
	/*
	 * This flag is used to indicate that the logical I/O has completed in
	 * one way or the other and that a reader can consume data. When this
	 * flag is set, then POLLIN | POLLRDNORM should be signaled. Until the
	 * I/O is consumed via ccid_read(), calls to ccid_write() will fail with
	 * EBUSY.
	 */
	CCID_IO_F_DONE		= 1 << 2,
	/*
	 * This flag is used to indicate that a given I/O has been abandoned by
	 * the user and that we need to clean things up before the ICC is usable
	 * again. 
	 *
	 * XXX Should this really be set? I'm now starting to wonder if this
	 * would make more sent to have like we have the resetting flag.
	 * Especially if for T=1 we issue an abort.
	 */
	CCID_IO_F_ABANDONED	= 1 << 3
} ccid_io_flags_t;

/*
 * This group of flags is used to make sure that we can't have multiple threads
 * in ccid_write(9E) at any given time. This is also used to indicate when we
 * are or aren't pollable. When one of these flags changes, then we must wake up
 * the pollhead.
 */
#define	CCID_IO_F_POLLOUT_FLAGS	(CCID_IO_F_PREPARING | CCID_IO_F_IN_PROGRESS)
#define	CCID_IO_F_ALL_FLAGS	(CCID_IO_F_PREPARING | CCID_IO_F_IN_PROGRESS | \
    CCID_IO_F_DONE | CCID_IO_F_ABANDONED)

typedef struct ccid_io {
	ccid_io_flags_t	ci_flags;
	size_t		ci_ilen;
	uint8_t		ci_ibuf[CCID_APDU_LEN_MAX];
	mblk_t		*ci_omp;
	kcondvar_t	ci_cv;
	struct ccid_command *ci_command;
	int		ci_errno;
	t1_state_t	ci_t1;
} ccid_io_t;

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
	ccid_icc_t		cs_icc;
	ccid_io_t		cs_io;
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
	CCID_F_HAS_INTR		= 1 << 0,
	CCID_F_NEEDS_PPS	= 1 << 1,
	CCID_F_NEEDS_PARAMS	= 1 << 2,
	CCID_F_NEEDS_DATAFREQ	= 1 << 3,
	CCID_F_NEEDS_IFSD	= 1 << 4,
	CCID_F_DETACHING	= 1 << 5,
	CCID_F_WORKER_REQUESTED	= 1 << 6,
	CCID_F_WORKER_RUNNING	= 1 << 7,
	CCID_F_DISCONNECTED	= 1 << 8
} ccid_flags_t;

#define	CCID_F_WORKER_MASK	(CCID_F_WORKER_REQUESTED | \
    CCID_F_WORKER_RUNNING)
#define	CCID_F_ICC_INIT_MASK	(CCID_F_NEEDS_PPS | CCID_F_NEEDS_PARAMS | \
    CCID_F_NEEDS_IFSD | CCID_F_NEEDS_DATAFREQ)

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
static void ccid_command_bcopy(ccid_command_t *, const void *, size_t);

/*
 * XXX Are these needed?
 */
static int ccid_write_apdu(ccid_t *, ccid_slot_t *);
static void ccid_complete_apdu(ccid_t *, ccid_slot_t *, ccid_command_t *);
static void ccid_teardown_apdu(ccid_t *, ccid_slot_t *, boolean_t);
static int ccid_read_apdu(ccid_t *, ccid_slot_t *, struct uio *);
static int ccid_write_tpdu_t1(ccid_t *, ccid_slot_t *);
static void ccid_complete_tpdu_t1(ccid_t *, ccid_slot_t *, ccid_command_t *);
static void ccid_teardown_tpdu_t1(ccid_t *, ccid_slot_t *, boolean_t);
static int ccid_read_tpdu_t1(ccid_t *, ccid_slot_t *, struct uio *);


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

/*
 * XXX This could be called while the user has threads in read() or write(). We
 * should block on those being completed / signal the reads() to break out.
 * Basically we'll need to block while either of CCID_IO_F_PREPARING or
 * CCID_MINOR_F_READ_WAITING is set.
 */
static void
ccid_slot_excl_rele(ccid_slot_t *slot)
{
	ccid_minor_t *cmp;
	ccid_t *ccid = slot->cs_ccid;

	VERIFY(MUTEX_HELD(&ccid->ccid_mutex));
	VERIFY3P(slot->cs_excl_minor, !=, NULL);

	cmp = slot->cs_excl_minor;

	cmp->cm_flags |= CCID_MINOR_F_TXN_ENDING;
	cv_signal(&cmp->cm_read_cv);

	/*
	 * There may either be a thread blocked in read or in the process of
	 * preparing a write. In either case, we need to make sure that they're
	 * woken up or finish, before we finish tear down.
	 */
	while ((cmp->cm_flags & CCID_MINOR_F_READ_WAITING) != 0 ||
	    (slot->cs_io.ci_flags & CCID_IO_F_PREPARING) != 0) {
		cv_wait(&cmp->cm_iowait_cv, &ccid->ccid_mutex);
	}

	/*
	 * At this point, we hold the lock and there should be no other threads
	 * that are past the basic sanity checks. So at this point, note that
	 * this minor no longer has exclusive access (causing other read/write
	 * calls to fail) and start the process of cleaning up the outstanding
	 * I/O on the slot. It is OK that at this point the thread may try to
	 * obtain exclusive access again. It will end up blocking on everything
	 * else.
	 */
	cmp->cm_flags &= ~CCID_MINOR_F_TXN_ENDING;
	cmp->cm_flags &= ~CCID_MINOR_F_HAS_EXCL;
	slot->cs_excl_minor = NULL;

	/*
	 * If we have an outstanding command left by the user when they've
	 * closed the slot, we need to clean up this command. We need to call
	 * the protocol specific handler here to determine what to do. If the
	 * command has completed, but the user has never called read, then it
	 * will simply clean it up. Otherwise it will indicate that there is
	 * some amount of external state still ongoing to take care of and clean
	 * up later.
	 */
	if (slot->cs_icc.icc_teardown != NULL) {
		slot->cs_icc.icc_teardown(ccid, slot, B_FALSE);
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
	 *
	 * XXX We need to wait here for I/O to complete or be torn down.
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

		/* XXX Waiting on a lock, need to reassert usability of device /
		 * going awayness */
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

static uint8_t
ccid_command_resp_param2(ccid_command_t *cc)
{
	uint8_t val;
	const ccid_header_t *cch;

	VERIFY3P(cc, !=, NULL);
	VERIFY3P(cc->cc_response, !=, NULL);

	cch = (ccid_header_t *)cc->cc_response->b_rptr;
	bcopy(&cch->ch_param2, &val, sizeof (val));
	return (val);
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
		ccid_slot_t *slot;

		slot = &ccid->ccid_slots[cc->cc_slot];
		/*
		 * If the user ops vector has been destroyed, free this command.
		 * There's not much we can do at this point. Otherwise, deliver
		 * it.
		 */
		if (slot->cs_icc.icc_rx == NULL) {
			ccid_command_free(cc);
		} else {
			slot->cs_icc.icc_complete(ccid, slot, cc);
		}
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
		    (ccid->ccid_flags & CCID_F_DETACHING)) {
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

		if (ccid->ccid_flags & CCID_F_DETACHING)
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
	VERIFY3U(seq, <=, UINT8_MAX);
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
 * Copy len bytes of data from buf into the allocated message block.
 */
static void
ccid_command_bcopy(ccid_command_t *cc, const void *buf, size_t len)
{
	size_t mlen;

	mlen = msgsize(cc->cc_ubrp->bulk_data);
	VERIFY3U(mlen + len, >=, len);
	VERIFY3U(mlen + len, >=, mlen);
	mlen += len;
	VERIFY3U(mlen, <=, cc->cc_ubrp->bulk_len);

	bcopy(buf, cc->cc_ubrp->bulk_data->b_wptr, len);
	cc->cc_ubrp->bulk_data->b_wptr += len;
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
	if (datasz + sizeof (ccid_header_t) > ccid->ccid_bufsize)
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
	cc->cc_ubrp->bulk_data->b_wptr += sizeof (ccid_header_t);
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

static int
ccid_command_get_parameters(ccid_t *ccid, ccid_slot_t *slot,
    atr_protocol_t *protp, ccid_params_t *paramsp)
{
	int ret;
	uint8_t prot;
	size_t mlen;
	ccid_header_t cch;
	ccid_command_t *cc;
	ccid_reply_command_status_t crs;
	ccid_reply_icc_status_t cis;
	const void *cpbuf;

	if ((ret = ccid_command_alloc(ccid, slot, B_TRUE, NULL, 0,
	   CCID_REQUEST_GET_PARAMS, 0, 0, 0, &cc)) != 0) {
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
	if (crs != CCID_REPLY_STATUS_COMPLETE) {
		if (cis == CCID_REPLY_ICC_MISSING) {
			ret = ENXIO;
		} else {
			ret = EIO;
		}
		goto done;
	}

	/*
	 * The protocol is in ch_param2 of the header.
	 */
	prot = ccid_command_resp_param2(cc);
	mlen = ccid_command_resp_length(cc);
	cpbuf = cc->cc_response->b_rptr + sizeof (ccid_header_t);

	ret = 0;
	switch (prot) {
	case 0:
		if (mlen < sizeof (ccid_params_t0_t)) {
			ret = EOVERFLOW;
			goto done;
		}
		*protp = ATR_P_T0;
		bcopy(cpbuf, &paramsp->ccp_t0, sizeof (ccid_params_t0_t));
		break;
	case 1:
		if (mlen < sizeof (ccid_params_t1_t)) {
			ret = EOVERFLOW;
			goto done;
		}
		*protp = ATR_P_T1;
		bcopy(cpbuf, &paramsp->ccp_t1, sizeof (ccid_params_t1_t));
		break;
	default:
		ret = ECHRNG;
		break;
	}

done:
	ccid_command_free(cc);
	return (ret);
}

static int
ccid_command_set_parameters(ccid_t *ccid, ccid_slot_t *slot, atr_protocol_t protocol, void *params)
{
	int ret;
	ccid_command_t *cc;
	uint8_t prot;
	size_t len;
	ccid_reply_command_status_t crs;
	ccid_reply_icc_status_t cis;
	ccid_command_err_t cce;

	switch (protocol) {
	case ATR_P_T0:
		prot = 0;
		len = sizeof (ccid_params_t0_t);
		break;
	case ATR_P_T1:
		prot = 1;
		len = sizeof (ccid_params_t1_t);
		break;
	default:
		return (EINVAL);
	}

	if ((ret = ccid_command_alloc(ccid, slot, B_TRUE, NULL, len,
	   CCID_REQUEST_SET_PARAMS, prot, 0, 0, &cc)) != 0) {
		return (ret);
	}
	ccid_command_bcopy(cc, params, len);
	if ((ret = ccid_command_queue(ccid, cc)) != 0) {
		ccid_command_free(cc);
		return (ret);
	}

	ccid_command_poll(ccid, cc);

	if (cc->cc_state != CCID_COMMAND_COMPLETE) {
		ret = EIO;
		goto done;
	}

	ccid_command_status_decode(cc, &crs, &cis, &cce);
	if (crs != CCID_REPLY_STATUS_COMPLETE) {
		if (cis == CCID_REPLY_ICC_MISSING) {
			ret = ENXIO;
		} else {
			ccid_error(ccid, "failed to set parameters on slot %u: "
			    "%u\n", slot->cs_slotno, cce);
			ret = EIO;
		}
	} else {
		ret = 0;
	}

done:
	ccid_command_free(cc);
	return (ret);
}

/*
 * Initiate a polled data transfer. This should not be used for any user I/O,
 * only for PPS and IFSD transactions while initializing the card. Generally
 * this is only used for CCID devices that support TPDU.
 */
static int
ccid_command_transfer(ccid_t *ccid, ccid_slot_t *slot, const void *buf,
    size_t len, mblk_t **outp)
{
	int ret;
	ccid_command_t *cc;
	uint8_t *datap;
	ccid_reply_command_status_t crs;
	ccid_reply_icc_status_t cis;
	ccid_command_err_t cce;

	if (buf == NULL || len == 0 || outp == NULL)
		return (EINVAL);

	*outp = NULL;
	if ((ret = ccid_command_alloc(ccid, slot, B_TRUE, NULL, len,
	   CCID_REQUEST_TRANSFER_BLOCK, 0, 0, 0, &cc)) != 0) {
		return (ret);
	}

	ccid_command_bcopy(cc, buf, len);

	if ((ret = ccid_command_queue(ccid, cc)) != 0) {
		ccid_command_free(cc);
		return (ret);
	}

	ccid_command_poll(ccid, cc);

	if (cc->cc_state != CCID_COMMAND_COMPLETE) {
		ret = EIO;
		goto done;
	}

	ccid_command_status_decode(cc, &crs, &cis, &cce);
	if (crs == CCID_REPLY_STATUS_COMPLETE) {
		mblk_t *mp;

		/* Take ownership of the data from the command */
		mp = cc->cc_response;
		cc->cc_response = NULL;
		mp->b_rptr += sizeof (ccid_header_t);
		*outp = mp;
		ret = 0;
	} else {
		if (cis == CCID_REPLY_ICC_MISSING) {
			ret = ENXIO;
		} else {
			ret = EIO;
		}
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
 * The given CCID slot has been removed. Clean up.
 */
static void
ccid_slot_removed(ccid_t *ccid, ccid_slot_t *slot, boolean_t notify)
{
	VERIFY(MUTEX_HELD(&ccid->ccid_mutex));
	if ((slot->cs_flags & CCID_SLOT_F_PRESENT) == 0) {
		VERIFY0(slot->cs_flags & CCID_SLOT_F_ACTIVE);
		return;
	}


	slot->cs_flags &= ~CCID_SLOT_F_PRESENT;
	slot->cs_flags &= ~CCID_SLOT_F_ACTIVE;

	/*
	 * Make sure that user I/O, if it exists, is torn down.
	 */
	if (slot->cs_icc.icc_teardown != NULL) {
		slot->cs_icc.icc_teardown(ccid, slot, B_TRUE);
	}

	/*
	 * XXX Reset ops / icc data.
	 */

	slot->cs_voltage = 0;
	freemsgchain(slot->cs_atr);
	slot->cs_atr = NULL;
	if (slot->cs_excl_minor != NULL && notify) {
		pollwakeup(&slot->cs_excl_minor->cm_pollhead, POLLHUP);
	}
}

static boolean_t
ccid_slot_send_pps(ccid_t *ccid, ccid_slot_t *slot, atr_data_t *data,
    uint8_t *fi, uint8_t *di, atr_protocol_t prot)
{
	mblk_t *mp;
	uint_t len;
	boolean_t changefi;
	int ret;
	uint8_t pps[PPS_BUFFER_MAX];

	if (fi == NULL && di == NULL) {
		len = atr_pps_generate(pps, sizeof (pps), prot, B_FALSE, 0, 0,
		    B_FALSE, 0);
	} else if (fi != NULL && di != NULL) {
		len = atr_pps_generate(pps, sizeof (pps), prot, B_TRUE, *fi,
		    *di, B_FALSE, 0);
	} else {
		return (B_FALSE);
	}

	if (len == 0) {
		ccid_error(ccid, "!failed to generate pps data");
		return (B_FALSE);
	}

	if ((ret = ccid_command_transfer(ccid, slot, pps, len, &mp)) != 0) {
		ccid_error(ccid, "!failed to perform PPS exchange: %d", ret);
		return (B_FALSE);
	}

	if (!atr_pps_valid(pps, sizeof (pps), mp->b_rptr, msgsize(mp))) {
		ccid_error(ccid, "!PPS reply was invalid\n");
		return (B_FALSE);
	}

	/*
	 * If the proposed Fi/Di values that we sent in the PPS were not
	 * accepted, then we need to use the default index values.
	 */
	if (!atr_pps_fidi_accepted(mp->b_rptr, msgsize(mp))) {
		*fi = atr_fi_default_index();
		*di = atr_di_default_index();
	}

	return (B_TRUE);
}

static boolean_t
ccid_slot_params_t0_init(ccid_t *ccid, ccid_slot_t *slot, atr_data_t *data,
    uint8_t fi, uint8_t di)
{
	int ret;
	ccid_params_t0_t p;
	atr_convention_t conv;
	atr_clock_stop_t stop;

	bzero(&p, sizeof (p));
	conv = atr_convention(data);
	/* XXX Macroify */
	p.cp0_bmFindexDindex = ((fi & 0x0f) << 4) | (di & 0x0f);
	/* B0 is set t0 0 for T=0 */
	p.cp0_bmTCCKST0 = 0;
	if (conv == ATR_CONVENTION_INVERSE) {
		p.cp0_bmTCCKST0 |= CCID_P_TCCKST0_INVERSE;
	} else {
		p.cp0_bmTCCKST0 |= CCID_P_TCCKST0_DIRECT;
	}
	p.cp0_bGuardTimeT0 = atr_extra_guardtime(data);
	p.cp0_bWaitingIntegerT0 = atr_t0_wi(data);
	p.cp0_bClockStop = atr_clock_stop(data);

	if ((ret = ccid_command_set_parameters(ccid, slot, ATR_P_T0,
	    &p)) != 0) {
		ccid_error(ccid, "failed to set T=0 params on slot %u: %d",
		    slot->cs_slotno, ret);
		return (B_FALSE);
	}

	return (B_TRUE);
}

static boolean_t
ccid_slot_params_t1_init(ccid_t *ccid, ccid_slot_t *slot, atr_data_t *data,
    uint8_t fi, uint8_t di)
{
	int ret;
	uint8_t bwi, cwi;
	ccid_params_t1_t p;
	atr_convention_t conv;
	atr_t1_checksum_t cksum;

	bzero(&p, sizeof (p));
	/* XXX Macroify */
	conv = atr_convention(data);
	cksum = atr_t1_checksum(data);
	bwi = atr_t1_bwi(data);
	cwi = atr_t1_cwi(data);
	p.cp1_bmFindexDindex = ((fi & 0x0f) << 4) | (di & 0x0f);
	p.cp1_bmTCCKST1 = 0x10;
	if (cksum == ATR_T1_CHECKSUM_CRC) {
		p.cp1_bmTCCKST1 |= 0x1;
	}
	if (conv == ATR_CONVENTION_INVERSE) {
		p.cp1_bmTCCKST1 |= 0x02;
	}
	p.cp1_bGuardTimeT1 = atr_extra_guardtime(data);
	p.cp1_bmWaitingIntegersT1 = ((bwi & 0x0f) << 4) | (cwi & 0x0f);
	p.cp1_bClockStop = atr_clock_stop(data);
	p.cp1_bIFSC = atr_t1_ifsc(data);

	/*
	 * We always set NAD to zero. NAD is used as a way to multiplex logical
	 * connections in T=1. However, we only ever have a single writer so
	 * this functionality is not useful. In addition, several readers don't
	 * support non-zero NAD values.
	 */
	p.cp1_bNadValue = 0;

	if ((ret = ccid_command_set_parameters(ccid, slot, ATR_P_T1,
	    &p)) != 0) {
		ccid_error(ccid, "failed to set T=1 params on slot %u: %d",
		    slot->cs_slotno, ret);
		return (B_FALSE);
	}

	return (B_TRUE);
}

static boolean_t
ccid_slot_t1_ifsd(ccid_t *ccid, ccid_slot_t *slot)
{
	const void *buf;
	size_t len;
	mblk_t *mp;
	int ret;
	t1_validate_t t1v;

	t1_ifsd(&slot->cs_io.ci_t1, ccid->ccid_class.ccd_dwMaxIFSD, &buf, &len);

	if ((ret = ccid_command_transfer(ccid, slot, buf, len, &mp)) != 0) {
		ccid_error(ccid, "!failed to perform IFSD exchange: %d", ret);
		return (B_FALSE);
	}

	t1v = t1_ifsd_resp(&slot->cs_io.ci_t1, mp->b_rptr, MBLKL(mp));
	freemsg(mp);
	if (t1v != T1_VALIDATE_OK) {
		ccid_error(ccid, "received invalid t1 response (%u): %s", t1v,
		    t1_errmsg(&slot->cs_io.ci_t1));
		return (B_FALSE);
	}

	return (B_TRUE);
}

/*
 * We have an ICC present in a slot. Before we can send commands to it, we
 * initialize the slot in some form or fashion. The steps that we must take
 * depend on the features that the card presents. To prepare the slot we must
 * make sure the following are set:
 *
 * - Negotiate and send the PPS (CCID_F_NEEDS_PPS)
 * - Set the CCID reader's parameters (CCID_F_NEEDS_PARAMS)
 * - Set the CCID reader's clock and data rate (CCID_F_NEEDS_DATAFREQ)
 * - Snapshot the current paramters being used for userland
 * - Set the IFSD for T=1 (CCID_F_NEEDS_IFSD)
 */
static boolean_t
ccid_slot_params_init(ccid_t *ccid, ccid_slot_t *slot, mblk_t *atr)
{
	int ret;
	boolean_t neg;
	atr_parsecode_t p;
	atr_protocol_t sup, def, prot, usable;
	atr_data_t *data;

	/*
	 * Hardware handles all initialization features. There's nothing else
	 * that we need to do for now.
	 */
	if ((ccid->ccid_flags & CCID_F_ICC_INIT_MASK) == 0)
		return (B_TRUE);

	/*
	 * Use the slot's atr data structure. This is only used when we're in
	 * the worker context, so it should be safe to access in a lockless
	 * fashion.
	 */
	data = slot->cs_icc.icc_atr_data;
	atr_data_reset(data);
	if ((p = atr_parse(atr->b_rptr, msgsize(atr), data)) != ATR_CODE_OK) {
		ccid_error(ccid, "!failed to parse ATR data from slot %d: %s",
		    slot->cs_slotno, atr_strerror(p));
		return (B_FALSE);
	}

	/*
	 * Snapshot the supported and default protocols. Snapshot whether we can
	 * negotiate this or not.
	 */
	def = atr_default_protocol(data);
	sup = atr_supported_protocols(data);
	neg = atr_params_negotiable(data);
	usable = sup & ccid->ccid_class.ccd_dwProtocols;

	/*
	 * We need to check if the reader supports the protocols supported by
	 * the ICC. If it does not, then we cannot use this ICC.
	 */
	if (usable == 0) {
		ccid_error(ccid, "!reader and ICC do not support common "
		    "protocols, reader 0x%x, ICC 0x%x\n",
		    ccid->ccid_class.ccd_dwProtocols, sup);
		return (B_FALSE);
	}

	/*
	 * If we encounter an ICC that needs manual setting of the frequency and
	 * data rates, we don't support that at this time. Warn about that fact.
	 * When we do support this, this should be folded into the general
	 * parameter logic detection as it will be driven based on
	 * atr_data_rate().
	 */
	if ((ccid->ccid_flags & CCID_F_NEEDS_DATAFREQ) != 0) {
		ccid_error(ccid, "!ccid reader requires unsupproted manual "
		    "clock and data rate settings, failing to activate ICC");
		return (B_FALSE);
	}

	/*
	 * If we need to send a PPS or we need to send parameters to the ICC,
	 * then we must go through and determine what the values we're sending
	 * should be.
	 *
	 * If the card has automatic parameter negotiation according to various
	 * specifications, then we don't bother trying to change the protocol
	 * and thus we don't enter this if block.
	 */
	if ((ccid->ccid_flags & (CCID_F_NEEDS_PPS | CCID_F_NEEDS_PARAMS)) != 0) {
		atr_data_rate_choice_t rate;
		uint8_t fi, di;
		boolean_t changeprot;

		fi = atr_fi_index(data);
		di = atr_di_index(data);
		rate = atr_data_rate(data, &ccid->ccid_class, NULL, 0, NULL);
		switch (rate) {
		case ATR_RATE_UNSUPPORTED:
			ccid_error(ccid, "!cannot use Fi/Di (%u/%u) values "
			    "for ICC", fi, di);
			return (B_FALSE);
		case ATR_RATE_USEDEFAULT:
			fi = atr_fi_default_index();
			di = atr_fi_default_index();
			break;
		case ATR_RATE_USEATR:
			break;
		case ATR_RATE_USEATR_SETRATE:
			ccid_error(ccid, "!ccid driver does not support "
			    "manual data rate setting for ICC, cannot activate");
			return (B_FALSE);
		default:
			ccid_error(ccid, "!unsupported data rate choice: %u",
			    rate);
			return (B_FALSE);
		}

		/*
		 * Determine what protocol we're going to negotiate or use to
		 * set parameters. Prefer T=1 if present. If not negotiable, use
		 * the default. Keep in mind, we have to consider which
		 * protocols the CCID reader supports as well.
		 */
		if (neg) {
			if (usable & ATR_P_T1)
				prot = ATR_P_T1;
			else
				prot = ATR_P_T0;
		} else {
			prot = def;
			if ((def & usable) == 0) {
				ccid_error(ccid, "!ICC does not support "
				    "negotiation and default protocol (0x%x) "
				    "is not supported by the reader", def);
				return (B_FALSE);
			}
		}

		changeprot = prot != def;

		/*
		 * Determine whether or not we need to send a PPS. We need to if
		 * we're going to change the protocol, if we need to change the
		 * Di/Fi values or we need to change the protocol, and if the
		 * hardware requires that we perform all this work. If we're
		 * sending a PPS, we do not have to send a new value of Fi and
		 * Di, but we must send a protocol.
		 */
		if ((ccid->ccid_flags & CCID_F_NEEDS_PPS) != 0 && neg &&
		    (changeprot || rate != ATR_RATE_USEDEFAULT)) {
			uint8_t *fip, *dip;

			if (rate != ATR_RATE_USEDEFAULT) {
				fip = &fi;
				dip = &di;
			} else {
				fip = dip = NULL;
			}
			ccid_slot_send_pps(ccid, slot, data, fip, dip, prot);
		}

		/*
		 * Now that we've (potentially) sent a PPS which has changed our
		 * parameters, we need to move on and send a CCID_SET_PARAMETERS
		 * command to make sure that the reader honors these.
		 */
		if ((ccid->ccid_flags & CCID_F_NEEDS_PARAMS) != 0) {
			if (prot == ATR_P_T0) {
				if (!ccid_slot_params_t0_init(ccid, slot, data,
				    fi, di)) {
					ccid_error(ccid, "!failed to send T=0 "
					    "paramters to device");
				}
			} else if (prot == ATR_P_T1) {
				if (!ccid_slot_params_t1_init(ccid, slot, data,
				    fi, di)) {
					ccid_error(ccid, "!failed to send T=1 "
					    "paramters to device");
				}
			}
		}
	}

	if ((ret = ccid_command_get_parameters(ccid, slot, &prot,
	    &slot->cs_icc.icc_params)) != 0) {
		ccid_error(ccid, "failed to get parameters for slot %u: %d",
		    slot->cs_slotno, ret);
		return (B_FALSE);
	}

	/*
	 * If we're using the T=1 protocol and operating at a TPDU level, then
	 * we need to initialize the state machine and potentially set the IFSD.
	 *
	 * If the reader is using APDU exchanges with the ICC then we don't
	 * bother trying to set the IFSD as we don't want to get in the way of
	 * any operations it is taking.
	 */
	if (prot == ATR_P_T1 &&
	    (ccid->ccid_class.ccd_dwFeatures & (CCID_CLASS_F_SHORT_APDU_XCHG |
	    CCID_CLASS_F_EXT_APDU_XCHG)) == 0) {

		t1_state_init_icc(&slot->cs_io.ci_t1, data, ccid->ccid_bufsize);

		/*
		 * XXX If we need to negotiate the IFSD and it fails, we could
		 * in theory drive on; however, it's probably better to fail
		 * hard in this case.
		 */
		if ((ccid->ccid_flags & CCID_F_NEEDS_IFSD) != 0) {
			if (!ccid_slot_t1_ifsd(ccid, slot)) {
				ccid_error(ccid, "failed to initialize IFSD");
				return (B_FALSE);
			}
		}
	}

	slot->cs_icc.icc_protocols = sup;
	slot->cs_icc.icc_cur_protocol = prot;

	return (B_TRUE);
}

static void
ccid_slot_setup_functions(ccid_t *ccid, ccid_slot_t *slot)
{
	uint_t bits = CCID_CLASS_F_TPDU_XCHG | CCID_CLASS_F_SHORT_APDU_XCHG |
	    CCID_CLASS_F_EXT_APDU_XCHG;
	switch (ccid->ccid_class.ccd_dwFeatures & bits) {
	case CCID_CLASS_F_SHORT_APDU_XCHG:
	case CCID_CLASS_F_EXT_APDU_XCHG:
		slot->cs_icc.icc_tx = ccid_write_apdu;
		slot->cs_icc.icc_complete = ccid_complete_apdu;
		slot->cs_icc.icc_teardown = ccid_teardown_apdu;
		slot->cs_icc.icc_rx = ccid_read_apdu;
		break;
	case CCID_CLASS_F_TPDU_XCHG:
		switch (slot->cs_icc.icc_cur_protocol) {
		case ATR_P_T1:
			/*
			 * At this time, we don't support the use of the CRC
			 * checksum for CCID devices. This is mostly because we
			 * haven't found any ICC devices that support its use.
			 * As such, if for some reason the parameters indicate
			 * that we're using T=1 and that we've specified the CRC
			 * versus LRC, we need to regretfully note that we can't
			 * perform I/O.
			 */
			if (atr_t1_checksum(slot->cs_icc.icc_atr_data) ==
			    ATR_T1_CHECKSUM_CRC) {
				ccid_error(ccid, "!ICC uses unsupported T=1 CRC "
				    "checksum. Please report this so support "
				    "can be added");
				slot->cs_icc.icc_tx = NULL;
				slot->cs_icc.icc_complete = NULL;
				slot->cs_icc.icc_teardown = NULL;
				slot->cs_icc.icc_rx = NULL;
				break;
			}

			slot->cs_icc.icc_tx = ccid_write_tpdu_t1;
			slot->cs_icc.icc_complete = ccid_complete_tpdu_t1;
			slot->cs_icc.icc_teardown = ccid_teardown_tpdu_t1;
			slot->cs_icc.icc_rx = ccid_read_tpdu_t1;
			break;
		case ATR_P_T0:
		default:
			slot->cs_icc.icc_tx = NULL;
			slot->cs_icc.icc_complete = NULL;
			slot->cs_icc.icc_teardown = NULL;
			slot->cs_icc.icc_rx = NULL;
			break;
		}
		break;
	default:
		slot->cs_icc.icc_tx = NULL;
		slot->cs_icc.icc_complete = NULL;
		slot->cs_icc.icc_teardown = NULL;
		slot->cs_icc.icc_rx = NULL;
	}

	/*
	 * When we don't have a support tx or rx function, we don't want to end
	 * up blocking attach. It's important we attach so that users can try
	 * and determine information about the ICC and reader.
	 */
	if (slot->cs_icc.icc_tx == NULL) {
		ccid_error(ccid, "CCID does not support I/O transfers to ICC");
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
		freemsg(atr);
		mutex_enter(&ccid->ccid_mutex);
		return;
	}

	if (!ccid_slot_params_init(ccid, slot, atr)) {
		ccid_error(ccid, "!failed to set slot paramters for ICC");
		freemsg(atr);
		mutex_enter(&ccid->ccid_mutex);
		return;
	}

	mutex_enter(&ccid->ccid_mutex);
	ccid_slot_setup_functions(ccid, slot);

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
	VERIFY(ccid->ccid_flags & CCID_F_WORKER_RUNNING);

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

	/*
	 * Make sure that user I/O, if it exists, is torn down.
	 */
	if (slot->cs_icc.icc_teardown != NULL) {
		slot->cs_icc.icc_teardown(ccid, slot, B_TRUE);
	}

	/*
	 * XXX Reset ops / icc data.
	 */

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
	if (ccid->ccid_flags & CCID_F_DETACHING) {
		ccid->ccid_flags &= ~CCID_F_WORKER_MASK;
		mutex_exit(&ccid->ccid_mutex);
		return;
	}
	ccid->ccid_flags |= CCID_F_WORKER_RUNNING;
	ccid->ccid_flags &= ~CCID_F_WORKER_REQUESTED;

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
	if (ccid->ccid_flags & CCID_F_WORKER_REQUESTED) {
		mutex_exit(&ccid->ccid_mutex);
		delay(drv_usectohz(1000) * 10);
		mutex_enter(&ccid->ccid_mutex);
	}

	ccid->ccid_flags &= ~CCID_F_WORKER_RUNNING;
	if (ccid->ccid_flags & CCID_F_DETACHING) {
		mutex_exit(&ccid->ccid_mutex);
		return;
	}

	if ((ccid->ccid_flags & CCID_F_WORKER_REQUESTED) != 0) {
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
	if (ccid->ccid_flags & CCID_F_DETACHING) {
		return;
	}

	run = (ccid->ccid_flags & CCID_F_WORKER_MASK) == 0; 
	ccid->ccid_flags |= CCID_F_WORKER_REQUESTED;
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
	if (ccid->ccid_flags & CCID_F_DETACHING) {
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
	ccid_class_features_t feat;
	uint_t bits;
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
		ccid->ccid_flags &= ~CCID_F_HAS_INTR;
		break;
	case 3:
		ccid->ccid_flags |= CCID_F_HAS_INTR;
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
	if (ccid->ccid_bufsize < CCID_MIN_MESSAGE_LENGTH) {
		ccid_error(ccid, "CCID reader maximum CCID message length (%u) is "
		    "less than minimum packet length (%u)", ccid->ccid_bufsize,
		    CCID_MIN_MESSAGE_LENGTH);
		return (B_FALSE);
	}

	/*
	 * At this time, we do not require that the system have automatic ICC
	 * activation or automatic ICC voltage. These are handled automatically
	 * by the system.
	 */
	feat = ccid->ccid_class.ccd_dwFeatures;

	/*
	 * Check the number of data rates that are supported by the reader. If
	 * the reader has a non-zero value
	 */
	if (ccid->ccid_class.ccd_bNumDataRatesSupported != 0) {
		ccid_error(ccid, "!CCID reader only supports fixed clock rates, "
		    "data will be limited to default values");
	}

	/*
	 * Check which automatic features the reader provides and which features
	 * it does not. Missing features will require additional work before a
	 * card can be activated. Note, this also applies to APDU based devices
	 * which may need to have various aspects of the device negotiated.
	 */

	/*
	 * The footnote for these two bits in CCID r1.1.0 indicates that
	 * when neither are missing we have to do the PPS negotiation
	 * ourselves.
	 */
	bits = CCID_CLASS_F_AUTO_PARAM_NEG | CCID_CLASS_F_AUTO_PPS;
	if ((feat & bits) == 0) {
		ccid->ccid_flags |= CCID_F_NEEDS_PPS;
	}

	if ((feat & CCID_CLASS_F_AUTO_PARAM_NEG) == 0) {
		ccid->ccid_flags |= CCID_F_NEEDS_PARAMS;
	}

	bits = CCID_CLASS_F_AUTO_BAUD | CCID_CLASS_F_AUTO_ICC_CLOCK;
	if ((feat & bits) != bits) {
		ccid->ccid_flags |= CCID_F_NEEDS_DATAFREQ;
	}

	/*
	 * XXX This should probably check on the actual support for T=1. If it
	 * doesn't exist, we should probably ignore it.
	 */
	if ((feat & CCID_CLASS_F_AUTO_IFSD) == 0) {
		ccid->ccid_flags |= CCID_F_NEEDS_IFSD;

		/*
		 * If there is no support for negotiating the IFSD, we need to
		 * check to make sure that the IFSD that's supported is at least
		 * the default size. If it is less than the default T=1 size,
		 * then we should probably reject this reader for the time
		 * being. It is possible that we could support it at a smaller
		 * IFSD; however, ISO/IEC 7816-3:2006 recommends that it be at
		 * least 20 bytes.
		 */
		if (ccid->ccid_class.ccd_dwMaxIFSD < T1_IFSD_DEFAULT) {
			ccid_error(ccid, "CCID reader max IFSD (%d) is less "
			    "T=1 default", ccid->ccid_class.ccd_dwMaxIFSD,
			    T1_IFSD_DEFAULT);
			return (B_FALSE);
		}
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

	if (ccid->ccid_flags & CCID_F_HAS_INTR) {
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

	if (ccid->ccid_flags & CCID_F_HAS_INTR) {
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

		cv_destroy(&ccid->ccid_slots[i].cs_io.ci_cv);
		freemsgchain(ccid->ccid_slots[i].cs_atr);
		atr_data_free(ccid->ccid_slots[i].cs_icc.icc_atr_data);
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
		ccid->ccid_slots[i].cs_icc.icc_atr_data = atr_data_alloc();
		ccid->ccid_slots[i].cs_idx.cmi_minor = CCID_MINOR_INVALID;
		ccid->ccid_slots[i].cs_idx.cmi_isslot = B_TRUE;
		ccid->ccid_slots[i].cs_idx.cmi_data.cmi_slot =
		    &ccid->ccid_slots[i];
		cv_init(&ccid->ccid_slots[i].cs_io.ci_cv, NULL, CV_DRIVER, NULL);
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
	if (ccid->ccid_flags & CCID_F_HAS_INTR) {
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
	if (ccid->ccid_flags & CCID_F_DETACHING) {
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
	uint_t i;

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
	/*
	 * First, set the disconnected flag. This will make sure that anyone
	 * that tries to make additional operations will be kicked out. This
	 * flag is checked by detach and by users.
	 */
	ccid->ccid_flags |= CCID_F_DISCONNECTED;

	/*
	 * Now, we need to basically wake up anyone blocked in read and make
	 * sure that they don't wait there forever and make sure that anyone
	 * polling gets a POLLHUP. We can't really distinguish between this and
	 * an ICC being removed. It will be discovered when someone tries to do
	 * an operation and they receive an EXDEV. We only need to do this on
	 * minors that have exclusive access.
	 */
	for (i = 0; i < ccid->ccid_nslots; i++) {
		ccid_slot_t *slot = &ccid->ccid_slots[i];
		if (slot->cs_excl_minor == NULL)
			continue;

		pollwakeup(&slot->cs_excl_minor->cm_pollhead, POLLHUP);
		cv_signal(&slot->cs_excl_minor->cm_read_cv);

		/* XXX We maybe should wait for the read flag to disappear? */
	}

	/*
	 * XXX If there are outstanding commands, they should ultimately be
	 * cleaned up as the USB commands themselves time out. It's not clear
	 * that we need to clean them up ourselves or how all those callbacks
	 * will function exactly.
	 */
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
	ccid->ccid_flags |= CCID_F_DETACHING;
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
		VERIFY0(ccid->ccid_flags & CCID_F_WORKER_MASK);
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
		if (ccid->ccid_flags & CCID_F_HAS_INTR) {
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

	if (ccid->ccid_flags & CCID_F_HAS_INTR) {
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
	if ((ccid->ccid_flags & CCID_F_DISCONNECTED) == 0) {
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
	cv_destroy(&cmp->cm_iowait_cv);
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

	/* XXX We should maybe reduce this for just getting the status */
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
	cv_init(&cmp->cm_iowait_cv, NULL, CV_DRIVER, NULL);
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
ccid_read_copyout_t1(struct uio *uiop, const mblk_t *mp)
{
	offset_t off;

	off = uiop->uio_loffset;

	for (; mp != NULL; mp = mp->b_cont) {
		int ret;

		if (MBLKL(mp) == 0)
			continue;

		ret = uiomove(mp->b_rptr, MBLKL(mp), UIO_READ, uiop);
		if (ret != 0) {
			return (EFAULT);
		}
	}

	return (0);
}

static int
ccid_read_copyout_apdu(struct uio *uiop, const ccid_command_t *cc, size_t len)
{
	int ret;
	mblk_t *mp;
	offset_t off;

	off = uiop->uio_loffset;
	mp = cc->cc_response;
	while (len > 0) {
		size_t tocopy, mblen;
		/*
		 * Each message block in the chain has its CCID header at the
		 * front.
		 */
		mblen = MBLKL(mp) - sizeof (ccid_header_t);
		tocopy = MIN(len, mblen);
		ret = uiomove(mp->b_rptr + sizeof (ccid_header_t), tocopy, UIO_READ, uiop);
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

/*
 * Called to indicate that the I/O has been completed and another I/O may be
 * issued to the device.
 */
static void
ccid_user_io_free(ccid_t *ccid, ccid_slot_t *slot, boolean_t signal)
{
	VERIFY(MUTEX_HELD(&ccid->ccid_mutex));

	/*
	 * Clear I/O flags.
	 */
	slot->cs_io.ci_flags &= ~CCID_IO_F_ALL_FLAGS;

	/*
	 * Always signal the I/O cv. Something may be waiting for this I/O to be
	 * done such as an ongoing teardown.
	 */
	cv_broadcast(&slot->cs_io.ci_cv);

	/*
	 * Wake up anyone waiting for writes. Note, they may not exist.  We
	 * shouldn't have to worry about a reset, as there will be nothing
	 * assinged to cs_excl_minor at that time.
	 *
	 * XXX This isn't quite right. One can have a valid minor but not be
	 * able to write because there is no ICC, etc. Sigh, these semantics are
	 * shitty.
	 */
	if (slot->cs_excl_minor != NULL && signal) {
		pollwakeup(&slot->cs_excl_minor->cm_pollhead, POLLOUT);
	}
}

/*
 * Called to indicate that we are ready for a user to consume the I/O.
 */
static void
ccid_user_io_done(ccid_t *ccid, ccid_slot_t *slot)
{
	ccid_minor_t *cmp;

	VERIFY(MUTEX_HELD(&ccid->ccid_mutex));
	cmp = slot->cs_excl_minor;
	VERIFY3P(cmp, !=, NULL);

	slot->cs_io.ci_flags |= CCID_IO_F_DONE;
	pollwakeup(&cmp->cm_pollhead, POLLIN | POLLRDNORM);
	cv_signal(&cmp->cm_read_cv);
}

static void
ccid_teardown_tpdu_t1(ccid_t *ccid, ccid_slot_t *slot, boolean_t wait)
{
}

/*
 * This is called in response to us having a command completed for a T=1 TPDU.
 * At this point we need to go through and now advance the state machine that
 * we've created and figure out what the next step is. Unlike with APDU level
 * transfers, we may need to go and send additional commands for the clients
 * APDU.
 */
static void
ccid_complete_tpdu_t1(ccid_t *ccid, ccid_slot_t *slot, ccid_command_t *cc)
{
	const void *buf;
	size_t len;
	mblk_t *mp;
	int ret;
	ccid_reply_command_status_t crs;
	ccid_reply_icc_status_t cis;
	t1_validate_t t1err;
	t1_state_t *t1 = &slot->cs_io.ci_t1;

	VERIFY(MUTEX_HELD(&ccid->ccid_mutex));
	VERIFY3P(slot->cs_io.ci_command, ==, cc);

	/*
	 * XXX At this time we do not properly implement the state machine that
	 * is described by the ISO/IEC 7816-3:2006 specification. If we get
	 * errors at a reader level or a failure to transmit the command, that
	 * might leave the ICC in an arbitrary state. We need to handle this and
	 * go from there. It's likely that we should treat this as a warm reset
	 * case like everything else and basically return EIO.
	 */
	if (cc->cc_state > CCID_COMMAND_COMPLETE) {
		/*
		 * XXX Take out the system until we fix this.
		 */
		cmn_err(CE_PANIC, "implement cc->cc_state > CCID_COMMAND_COMPLETED case");
	}

	/*
	 * Check the CCID command level case. If we were told the slot is going
	 * away, mark that and notify the user that the command is done.
	 * 
	 * XXX In terms of failure we should be looking at one of several
	 * different things here. We should see if there was a bit error, etc.
	 * and act accordingly per the spec.
	 */
	ccid_command_status_decode(cc, &crs, &cis, NULL);
	if (crs == CCID_REPLY_STATUS_FAILED && cis == CCID_REPLY_ICC_MISSING) {
		/*
		 * The ICC was removed. The user will likely be notified of this
		 * at some point soon. Keep the ccid_command_t around until they
		 * call read for debugging purposes.
		 */
		slot->cs_io.ci_errno = ENXIO;
		ccid_user_io_done(ccid, slot);
		return;
	} else if (crs != CCID_REPLY_STATUS_COMPLETE) {
		/* XXX */
		cmn_err(CE_PANIC, "implement crs != CCID_REPLY_STATUS_COMPLETE case");
	}

	/*
	 * The system has already verified that the CCID payload length makes
	 * sense for the message block, so we do not need to check that here as
	 * we take ownership of the message block from the command and free the
	 * command.
	 */
	mp = cc->cc_response;
	cc->cc_response = NULL;
	mp->b_rptr += sizeof (ccid_header_t);

	slot->cs_io.ci_command = NULL;
	ccid_command_free(cc);
	cc = NULL;

	if ((t1err = t1_reply(t1, mp)) != T1_VALIDATE_OK) {
		ccid_error(ccid, "!Received t1 error (%u): %s", t1err, t1_errmsg(t1));
	}

	switch (t1_step(t1)) {
	case T1_ACTION_SEND_COMMAND:
		break;
	case T1_ACTION_WARM_RESET:
		/* XXX Actually issue the reset */
		slot->cs_io.ci_errno = EIO;
		ccid_user_io_done(ccid, slot);
		return;
	case T1_ACTION_DONE:
		slot->cs_io.ci_errno = 0;
		ccid_user_io_done(ccid, slot);
		return;
	}

	/*
	 * We've been asked to send another command by the T=1 state machine. Do
	 * so.
	 */
	t1_data(t1, &buf, &len);
	/*
	 * XXX Right now we're purposefully not dropping the lock across the
	 * command allocation. I'm not sure if that's good or not. THe problem
	 * is that if we drop it, we need to make sure that the ICC state is
	 * still good. If not, then we would need to throw this out, but it
	 * means that the system can advance in the face of memory pressure,
	 * which is good.
	 *
	 * XXX We need to actually ask the T=1 state machine for the WTX for
	 * this block. We also may need to adjust the timeout on the USB command.
	 */
	if ((ret = ccid_command_alloc(ccid, slot, B_FALSE, NULL, len,
	    CCID_REQUEST_TRANSFER_BLOCK, 0, 0, 0,
	    &cc)) != 0) {
		slot->cs_io.ci_errno = ENOMEM;
		ccid_user_io_done(ccid, slot);
		return;
	}
	cc->cc_flags |= CCID_COMMAND_F_USER;
	ccid_command_bcopy(cc, buf, len);

	/*
	 * Now, finally drop the lock to queue the command.
	 */
	mutex_exit(&ccid->ccid_mutex);

	if ((ret = ccid_command_queue(ccid, cc)) != 0) {
		mutex_enter(&ccid->ccid_mutex);
		/* XXX Do we need to clean up the T=1 state here potentially? Or
		 * can we leave it to be cleaned up by something else that next
		 * uses it? Becuse we've dropped the lock, it's not clear what
		 * we can or cannot do.
		 *
		 * XXX For the moment I'm going to mark this command done. This
		 * is really getting far too complicated.
		 */
		slot->cs_io.ci_command = NULL;
		ccid_command_free(cc);
		slot->cs_io.ci_errno = ENOMEM;
		ccid_user_io_done(ccid, slot);
		return;
	}

	mutex_enter(&ccid->ccid_mutex);
}

static int
ccid_read_tpdu_t1(ccid_t *ccid, ccid_slot_t *slot, struct uio *uiop)
{
	int ret;
	const mblk_t *mp;
	size_t len;

	VERIFY(MUTEX_HELD(&ccid->ccid_mutex));

	/*
	 * Check to see if we have an error set on I/O structure. If so, then we
	 * need to go ahead and give that to the user.
	 *
	 * XXX We need to clean up any T=1 state associated with this.
	 */
	if (slot->cs_io.ci_errno != 0) {
		ret = slot->cs_io.ci_errno;
		goto consume;
	}

	/*
	 * XXX It's not clear right now when we'd not get a message block chain,
	 * but we don't have an error. For now, treat it as a zero byte read.
	 */
	if ((mp = t1_state_cmd_data(&slot->cs_io.ci_t1)) == NULL) {
		ret = 0;
		goto consume;
	}

	/*
	 * XXX Deal with the fact that msgsize can't deal with constness.
	 */
	len = msgsize((mblk_t *)mp);
	if (len > uiop->uio_resid) {
		return (EOVERFLOW);
	}

	/*
	 * Copy out the resulting data. Don't consume it if we can't copy it all
	 * out.
	 */
	ret = ccid_read_copyout_t1(uiop, mp);
	if (ret != 0) {
		return (ret);
	}

consume:
	slot->cs_io.ci_errno = 0;
	/* XXX Clean up the T=1 state machine for the next command */
	t1_state_cmd_done(&slot->cs_io.ci_t1);
	ccid_user_io_free(ccid, slot, B_TRUE);
	return (ret);
}

static int
ccid_write_tpdu_t1(ccid_t *ccid, ccid_slot_t *slot)
{
	int ret;
	ccid_command_t *cc;
	const void *buf;
	size_t len;

	VERIFY(MUTEX_HELD(&ccid->ccid_mutex));

	/*
	 * Initiailze a new command and kick off the internal state machine.
	 */
	t1_state_cmd_init(&slot->cs_io.ci_t1, slot->cs_io.ci_ibuf,
	    slot->cs_io.ci_ilen);

	switch (t1_step(&slot->cs_io.ci_t1)) {
	case T1_ACTION_SEND_COMMAND:
		break;
	case T1_ACTION_WARM_RESET:
	case T1_ACTION_DONE:
		/* XXX Figure out if this can happen. */
		return (EIO);
	}

	t1_data(&slot->cs_io.ci_t1, &buf, &len);

	/*
	 * XXX Right now we're purposefully not dropping the lock across the
	 * command allocation. I'm not sure if that's good or not. THe problem
	 * is that if we drop it, we need to make sure that the ICC state is
	 * still good. If not, then we would need to throw this out, but it
	 * means that the system can advance in the face of memory pressure,
	 * which is good.
	 */
	if ((ret = ccid_command_alloc(ccid, slot, B_FALSE, NULL, len,
	    CCID_REQUEST_TRANSFER_BLOCK, 0, 0, 0,
	    &cc)) != 0) {
		/* XXX Invalidate command state? */
		return (ret);
	}
	cc->cc_flags |= CCID_COMMAND_F_USER;
	ccid_command_bcopy(cc, buf, len);

	/*
	 * Before we submit this command, assign it to our internal state. We
	 * need to do this before we submit the command. Otherwise, we could be
	 * pathologically scheduled and not get the chance.
	 */
	slot->cs_io.ci_command = cc;

	/*
	 * Now, finally drop the lock to queue the command.
	 */
	mutex_exit(&ccid->ccid_mutex);

	if ((ret = ccid_command_queue(ccid, cc)) != 0) {
		mutex_enter(&ccid->ccid_mutex);
		/* XXX Do we need to clean up the T=1 state here potentially? Or
		 * can we leave it to be cleaned up by something else that next
		 * uses it? Becuse we've dropped the lock, it's not clear what
		 * we can or cannot do.
		 */
		slot->cs_io.ci_command = NULL;
		ccid_command_free(cc);
		return (ret);
	}

	mutex_enter(&ccid->ccid_mutex);

	return (0);
}

/*
 * This is called in a few different sitautions. It's called when an exclusive
 * hold is being released by a user on a the slot. It's also called when the ICC
 * is removed, the reader has been unplugged, or the ICC is being reset. In all
 * these cases we need to make sure that I/O is taken care of and we won't be
 * leaving behind vestigial garbage.
 *
 * The boolean_t wait is used to indicate whether or not this is a user hold
 * removal or one of the other conditions. In all of the non-user hold
 * conditions, the operations vector will be torn down for the slot's I/O
 * module. As such we need to make sure we wait for things to finish up before
 * we return, in the exclusive hold case, we don't.
 */
static void
ccid_teardown_apdu(ccid_t *ccid, ccid_slot_t *slot, boolean_t wait)
{
	ccid_command_t *cc;

	VERIFY(MUTEX_HELD(&ccid->ccid_mutex));

	/*
	 * If no I/O is in progres, then there's nothing to do.
	 */
	if ((slot->cs_io.ci_flags & CCID_IO_F_IN_PROGRESS) == 0) {
		return;
	}

	/*
	 * If the command is outstanding, then we must wait for it to be issued
	 * and failed by the device.
	 */
	cc = slot->cs_io.ci_command;
	if (cc->cc_state >= CCID_COMMAND_COMPLETE) {
		slot->cs_io.ci_command = NULL;
		ccid_command_free(cc);
		ccid_user_io_free(ccid, slot, B_FALSE);
		return;
	}

	/*
	 * At this point, we expect that the completion function is going to
	 * run. Mark this as abandoned, so it knows to clean itself up.
	 */
	slot->cs_io.ci_flags |= CCID_IO_F_ABANDONED;
	if (!wait) {
		return;
	}

	while (slot->cs_io.ci_command != NULL) {
		cv_wait(&slot->cs_io.ci_cv, &ccid->ccid_mutex);
		/* XXX Should we be waiting for other conditions here */
	}
}

/*
 * This function is called in response to a CCID command completing.
 */
static void
ccid_complete_apdu(ccid_t *ccid, ccid_slot_t *slot, ccid_command_t *cc)
{
	ccid_minor_t *cmp;

	VERIFY(MUTEX_HELD(&ccid->ccid_mutex));
	VERIFY3P(slot->cs_io.ci_command, ==, cc);

	/*
	 * First, see if it's even worth processing this command. To do that, we
	 * want to go through and see if there's still a minor. If there isn't,
	 * we can free this command and free up the I/O.
	 * XXX The excl_minor bit looks suspicious
	 */
	cmp = slot->cs_excl_minor;
	if ((slot->cs_io.ci_flags & CCID_IO_F_ABANDONED) != 0 ||
	    slot->cs_excl_minor == NULL) {
		ccid_command_free(cc);
		slot->cs_io.ci_command = NULL;
		ccid_user_io_free(ccid, slot, B_TRUE);
		return;
	}

	/*
	 * Now, we can go ahead and wake up a reader to process this command.
	 */
	ccid_user_io_done(ccid, slot);
}

static int
ccid_read_apdu(ccid_t *ccid, ccid_slot_t *slot, struct uio *uiop)
{
	int ret;
	ccid_command_t *cc;
	ccid_reply_command_status_t crs;
	ccid_reply_icc_status_t cis;
	ccid_command_err_t cce;

	VERIFY(MUTEX_HELD(&ccid->ccid_mutex));

	/*
	 * XXX Check if the framework has left us an error in ci_errno and if
	 * so, use that to bypass any command and return this.
	 */

	/*
	 * Check if the command is present and our error status. We may not have
	 * a command present if an error is set. For example, if an ICC was
	 * removed while processing this event. However, if the command
	 * structure is present and the error is non-zero then we must consume
	 * and free the command.
	 */
	cc = slot->cs_io.ci_command;
	if (cc == NULL) {
		VERIFY3S(slot->cs_io.ci_errno, !=, 0);
	}

	if (slot->cs_io.ci_errno != 0) {
		ret = slot->cs_io.ci_errno;
		slot->cs_io.ci_errno = 0;
		goto consume;
	}

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
			return (EOVERFLOW);
		}

		/*
		 * Copy out the resulting data.
		 */
		ret = ccid_read_copyout_apdu(uiop, cc, len);
		if (ret != 0) {
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
	slot->cs_io.ci_command = NULL;
	if (cc != NULL) {
		ccid_command_free(cc);
	}
	ccid_user_io_free(ccid, slot, B_TRUE);
	return (ret);
}

/*
 * We have the uswer buffer in the CCID slot. Given that, transform it into
 * something that we can send to the device. For APDU's this is simply creating
 * a transfer command and copying it into that buffer.
 */
static int
ccid_write_apdu(ccid_t *ccid, ccid_slot_t *slot)
{
	int ret;
	ccid_command_t *cc;

	VERIFY(MUTEX_HELD(&ccid->ccid_mutex));

	if ((ret = ccid_command_alloc(ccid, slot, B_FALSE, NULL,
	    slot->cs_io.ci_ilen, CCID_REQUEST_TRANSFER_BLOCK, 0, 0, 0,
	    &cc)) != 0) {
		mutex_enter(&ccid->ccid_mutex);
		return (ret);
	}

	cc->cc_flags |= CCID_COMMAND_F_USER;
	ccid_command_bcopy(cc, slot->cs_io.ci_ibuf, slot->cs_io.ci_ilen);

	slot->cs_io.ci_command = cc;
	mutex_exit(&ccid->ccid_mutex);

	if ((ret = ccid_command_queue(ccid, cc)) != 0) {
		mutex_enter(&ccid->ccid_mutex);
		slot->cs_io.ci_command = NULL;
		ccid_command_free(cc);
		return (ret);
	}

	mutex_enter(&ccid->ccid_mutex);

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
	if ((ccid->ccid_flags & CCID_F_DISCONNECTED) != 0) {
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
	 * Make sure we have a read function for this minor, otherwise we won't
	 * be able to receive data.
	 */
	if (slot->cs_icc.icc_rx == NULL) {
		mutex_exit(&ccid->ccid_mutex);
		return (ENOTSUP);
	}

	/*
	 * If there's been no write I/O issued, then this read is not allowed.
	 * While this may seem like a silly constraint, it certainly simplifies
	 * a lot of the surrounding logic.
	 *
	 * XXX This is a stupid errno.
	 */
	if ((slot->cs_io.ci_flags & (CCID_IO_F_IN_PROGRESS | CCID_IO_F_DONE)) == 0) {
		mutex_exit(&ccid->ccid_mutex);
		return (ENODATA);
	}

	/*
	 * If another thread is already blocked in read, then don't allow them
	 * in. We only want to allow one thread to attempt to consume a read,
	 * just as we only allow one thread to initiate a write.
	 */
	if ((cmp->cm_flags & CCID_MINOR_F_READ_WAITING) != 0) {
		mutex_exit(&ccid->ccid_mutex);
		return (EBUSY);
	}

	/*
	 * Check if an I/O has completed. Once it has, call the protocol
	 * specific code. Note that the lock may be dropped after polling. In
	 * such a case we will have to logically recheck several conditions.
	 *
	 * Note, we don't really care if the slot is active or not as I/O could
	 * have been in flight while the slot was inactive.
	 */
	while ((slot->cs_io.ci_flags & CCID_IO_F_DONE) == 0) {
		if (uiop->uio_fmode & FNONBLOCK) {
			mutex_exit(&ccid->ccid_mutex);
			return (EWOULDBLOCK);
		}

		/*
		 * While we perform a cv_wait_sig() we'll end up dropping the
		 * CCID mutex. This means that we need to notify the rest of the
		 * driver that a thread is blocked in read. This is used not
		 * only for excluding multiple threads trying to read from the
		 * device, but more importantly so that we know that if the ICC
		 * or reader are removed, that we need to wake up this thread.
		 */
		cmp->cm_flags |= CCID_MINOR_F_READ_WAITING;
		if (cv_wait_sig(&cmp->cm_read_cv, &ccid->ccid_mutex) == 0) {
			mutex_exit(&ccid->ccid_mutex);
			return (EINTR);
		}
		cmp->cm_flags &= CCID_MINOR_F_READ_WAITING;
		cv_signal(&cmp->cm_iowait_cv);

		/*
		 * Check if the reader has been removed.
		 */
		if ((ccid->ccid_flags & CCID_F_DISCONNECTED) != 0) {
			mutex_exit(&ccid->ccid_mutex);
			return (ENODEV);
		}

		/*
		 * Check if the user ended a transcation while still blocked in
		 * read.
		 * XXX This isn't currently signaled.
		 */
		if (!(cmp->cm_flags & CCID_MINOR_F_TXN_ENDING)) {
			mutex_exit(&ccid->ccid_mutex);
			return (ECANCELED);
		}

		/*
		 * XXX Check and make sure that we still have an icc_rx
		 * operation and in general, handle the case that the ICC has
		 * been removed. The way that this happens will probably be a
		 * bit complicated since we don't want to generally check if the
		 * slot is active. So really it's check to see if there isn't
		 * some kind of error on the I/O or similar. The trick case here
		 * is if there wasn't a write in progress.
		 */
	}

	/*
	 * Callers of this function should in general not drop the lock. Doing
	 * so might lead to some confusion in the broader readable state.
	 */
	ret = slot->cs_icc.icc_rx(ccid, slot, uiop);
	mutex_exit(&ccid->ccid_mutex);

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
	size_t len, cbytes;

	if (uiop->uio_resid > CCID_APDU_LEN_MAX) {
		return (E2BIG);
	}

	if (uiop->uio_resid <= 0) {
		return (EINVAL);
	}

	len = uiop->uio_resid;
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
	 * Now that we have the slot, verify whether or not we can perform this
	 * I/O.
	 */
	mutex_enter(&ccid->ccid_mutex);
	if ((ccid->ccid_flags & CCID_F_DISCONNECTED) != 0) {
		mutex_exit(&ccid->ccid_mutex);
		return (ENODEV);
	}

	/*
	 * Check if we have exclusive access and if there's a card present. If
	 * not, both are errors.
	 */
	if (!(cmp->cm_flags & CCID_MINOR_F_HAS_EXCL)) {
		mutex_exit(&ccid->ccid_mutex);
		return (EACCES);
	}

	if (!(slot->cs_flags & CCID_SLOT_F_ACTIVE)) {
		mutex_exit(&ccid->ccid_mutex);
		return (ENXIO);
	}

	/*
	 * Make sure that we have a supported transmit function.
	 */
	if (slot->cs_icc.icc_tx == NULL) {
		mutex_exit(&ccid->ccid_mutex);
		return (ENOTSUP);
	}

	/*
	 * See if another command is in progress. If so, try to claim it.
	 * Otherwise, fail with EBUSY. Note, we only fail for commands that are
	 * user initiated. There may be other commands that are ongoing in the
	 * system.
	 */
	if ((slot->cs_io.ci_flags & CCID_IO_F_POLLOUT_FLAGS) != 0) {
		mutex_exit(&ccid->ccid_mutex);
		return (EBUSY);
	}

	/*
	 * Use uiocopy and not uiomove. This way if we fail for whatever reason,
	 * we don't have to worry about restoring the original buffer.
	 */
	if (uiocopy(slot->cs_io.ci_ibuf, len, UIO_WRITE, uiop, &cbytes) != 0) {
		mutex_exit(&ccid->ccid_mutex);
		return (EFAULT);
	}

	slot->cs_io.ci_ilen = len;
	slot->cs_io.ci_flags |= CCID_IO_F_PREPARING;
	slot->cs_io.ci_omp = NULL;

	/*
	 * Now that we're here, go ahead and call the actual tx function.
	 */
	if ((ret = slot->cs_icc.icc_tx(ccid, slot)) != 0) {
		/*
		 * The command wasn't actually transmitted. In this case we need
		 * to reset the copied in data and signal anyone who is polling
		 * that this is writeable again. We don't have to worry about
		 * readers at this point, as they won't get in unless
		 * CCID_IO_F_IN_PROGRESS has been set.
		 */
		slot->cs_io.ci_ilen = 0;
		bzero(slot->cs_io.ci_ibuf, sizeof (slot->cs_io.ci_ibuf));
		slot->cs_io.ci_flags &= ~CCID_IO_F_PREPARING;
		/*
		 * XXX We should be checking more conditions then just this. We
		 * don't want to signal, if for example, we're disconnected, or
		 * we're going to end up going away, etc.
		 */
		if (slot->cs_excl_minor != NULL) {
			pollwakeup(&slot->cs_excl_minor->cm_pollhead, POLLOUT);
		}
	} else {
		slot->cs_io.ci_flags &= ~CCID_IO_F_PREPARING;
		slot->cs_io.ci_flags |= CCID_IO_F_IN_PROGRESS;
		uiop->uio_resid -= cbytes;
	}
	/*
	 * Notify a waiter that we've moved on.
	 */
	cv_signal(&slot->cs_excl_minor->cm_iowait_cv);
	mutex_exit(&ccid->ccid_mutex);

	return (ret);
}

static int
ccid_ioctl_status(ccid_slot_t *slot, intptr_t arg, int mode)
{
	uccid_cmd_status_t ucs;
	ccid_t *ccid = slot->cs_ccid;

	if (ddi_copyin((void *)arg, &ucs, sizeof (ucs), mode & FKIOCTL) != 0)
		return (EFAULT);

	if (ucs.ucs_version != UCCID_VERSION_ONE)
		return (EINVAL);

	ucs.ucs_status = 0;
	mutex_enter(&slot->cs_ccid->ccid_mutex);
	if ((slot->cs_ccid->ccid_flags & CCID_F_DISCONNECTED) != 0) {
		mutex_exit(&slot->cs_ccid->ccid_mutex);
		return (ENODEV);
	}

	if (slot->cs_flags & CCID_SLOT_F_PRESENT)
		ucs.ucs_status |= UCCID_STATUS_F_CARD_PRESENT;
	if (slot->cs_flags & CCID_SLOT_F_ACTIVE)
		ucs.ucs_status |= UCCID_STATUS_F_CARD_ACTIVE;

	if (slot->cs_atr != NULL) {
		ucs.ucs_atrlen = MIN(UCCID_ATR_MAX, MBLKL(slot->cs_atr));
		bcopy(slot->cs_atr->b_rptr, ucs.ucs_atr, ucs.ucs_atrlen);
	} else {
		bzero(ucs.ucs_atr, sizeof (ucs.ucs_atr));
		ucs.ucs_atrlen = 0;
	}

	bcopy(&ccid->ccid_class, &ucs.ucs_class, sizeof (ucs.ucs_class));

	if (ccid->ccid_dev_data->dev_product != NULL) {
		(void) strlcpy(ucs.ucs_product, ccid->ccid_dev_data->dev_product,
		    sizeof (ucs.ucs_product));
		ucs.ucs_status |= UCCID_STATUS_F_PRODUCT_VALID;
	} else {
		ucs.ucs_product[0] = '\0';
	}

	if (ccid->ccid_dev_data->dev_serial != NULL) {
		(void) strlcpy(ucs.ucs_serial, ccid->ccid_dev_data->dev_serial,
		    sizeof (ucs.ucs_serial));
		ucs.ucs_status |= UCCID_STATUS_F_SERIAL_VALID;
	} else {
		ucs.ucs_serial[0] = '\0';
	}
	mutex_exit(&slot->cs_ccid->ccid_mutex);

	if ((slot->cs_flags & CCID_SLOT_F_ACTIVE) != 0) {
		ucs.ucs_status |= UCCID_STATUS_F_PARAMS_VALID;
		ucs.ucs_prot = slot->cs_icc.icc_cur_protocol;
		ucs.ucs_params = slot->cs_icc.icc_params;
	}

	if (ddi_copyout(&ucs, (void *)arg, sizeof (ucs), mode & FKIOCTL) != 0)
		return (EFAULT);

	return (0);
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

	if ((uct.uct_flags & ~UCCID_TXN_DONT_BLOCK) != 0)
		return (EINVAL);
	nowait = (uct.uct_flags & UCCID_TXN_DONT_BLOCK) != 0;

	mutex_enter(&slot->cs_ccid->ccid_mutex);
	if ((slot->cs_ccid->ccid_flags & CCID_F_DISCONNECTED) != 0) {
		mutex_exit(&slot->cs_ccid->ccid_mutex);
		return (ENODEV);
	}
	ret = ccid_slot_excl_req(slot, cmp, nowait);
	mutex_exit(&slot->cs_ccid->ccid_mutex);

	return (ret);
}

static int
ccid_ioctl_txn_end(ccid_slot_t *slot, ccid_minor_t *cmp, intptr_t arg, int mode)
{
	int ret;
	uccid_cmd_txn_end_t uct;
	boolean_t nowait;

	if (ddi_copyin((void *)arg, &uct, sizeof (uct), mode & FKIOCTL) != 0) {
		return (EFAULT);
	}

	if (uct.uct_version != UCCID_VERSION_ONE) {
		return (EINVAL);
	}

	if ((uct.uct_flags & ~(UCCID_TXN_END_RESET |
	    UCCID_TXN_END_RELEASE)) != 0) {
		return (EINVAL);
	}

	/*
	 * Require at least one of these flags to be set.
	 */
	if ((((uct.uct_flags & UCCID_TXN_END_RESET) != 0) ^
	    ((uct.uct_flags & UCCID_TXN_END_RELEASE) != 0)) == 0) {
		return (EINVAL);
	}

	mutex_enter(&slot->cs_ccid->ccid_mutex);
	if ((slot->cs_ccid->ccid_flags & CCID_F_DISCONNECTED) != 0) {
		mutex_exit(&slot->cs_ccid->ccid_mutex);
		return (ENODEV);
	}

	if (slot->cs_excl_minor != cmp) {
		mutex_exit(&slot->cs_ccid->ccid_mutex);
		return (ENXIO);
	}
	VERIFY3S(cmp->cm_flags & CCID_MINOR_F_HAS_EXCL, !=, 0);

	if (uct.uct_flags & UCCID_TXN_END_RESET) {
		cmp->cm_flags |= CCID_MINOR_F_TXN_RESET;
	}
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
	if ((ccid->ccid_flags & CCID_F_DISCONNECTED) != 0) {
		mutex_exit(&ccid->ccid_mutex);
		return (ENODEV);
	}

	if (!(cmp->cm_flags & CCID_MINOR_F_HAS_EXCL)) {
		mutex_exit(&ccid->ccid_mutex);
		return (EACCES);
	}

	if ((slot->cs_io.ci_flags & CCID_IO_F_DONE) != 0) {
		ready |= POLLIN | POLLRDNORM;
	} else if ((slot->cs_io.ci_flags & CCID_IO_F_POLLOUT_FLAGS) == 0) {
		ready |= POLLOUT;
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

	/*
	 * If the minor node was closed without an explicit transaction end,
	 * then we need to assume that the reader's ICC is in an arbitrary
	 * state. For example, the ICC could have a specific PIV applet
	 * selected. In such a case, the only safe thing to do is to force a
	 * reset.
	 */
	mutex_enter(&slot->cs_ccid->ccid_mutex);
	if (cmp->cm_flags & CCID_MINOR_F_HAS_EXCL) {
		cmp->cm_flags |= CCID_MINOR_F_TXN_RESET;
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
	{ &ccid_modldrv, NULL }
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
