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

#ifndef _SYS_USB_UCCID_H
#define	_SYS_USB_UCCID_H

/*
 * Definitions for the userland CCID interface.
 */

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The maximum size of a normal APDU. This is the upper bound of what a user can
 * read or write to a given card.
 */
#define	UCCID_APDU_SIZE_MAX	261

/*
 * This is the maximum length of an ATR as per ISO/IEC 7816-3:2006.
 */
#define	UCCID_ATR_MAX		33


#define	UCCID_IOCTL	(('u' << 24) | ('c' << 16) | ('d') << 8)

#define	UCCID_VERSION_ONE	1
#define	UCCID_CURRENT_VERSION	UCCID_VERSION_ONE

#define	UCCID_TXN_DONT_BLOCK	0x01
#define	UCCID_TXN_END_RESET	0x02
#define	UCCID_TXN_END_RELEASE	0x04

typedef struct uccid_cmd_txn_begin {
	uint32_t	uct_version;
	uint32_t	uct_flags;
} uccid_cmd_txn_begin_t;

/*
 * Attempt to obtain eclusive access. If the UCN_TXN_DONT_BLOCK flag is
 * specified, the ioctl will return immediately if exclusive access cannot be
 * gained. Otherwise, it will block in an interruptable fashion. The argument is
 * a uccid_cmd_txn_begin_t.
 */
#define	UCCID_CMD_TXN_BEGIN	(UCCID_IOCTL | 0x01)

typedef struct uccid_cmd_txn_end {
	uint32_t	uct_version;
} uccid_cmd_txn_end_t;

/*
 * Reliquish exclusive access. Takes a uccid_cmd_txn_end_t. uct_flags must be zero.
 */
#define	UCCID_CMD_TXN_END	(UCCID_IOCTL | 0x02)

/*
 * Bits for UCS Status
 */
#define	UCCID_STATUS_F_CARD_PRESENT	0x01
#define	UCCID_STATUS_F_CARD_ACTIVE	0x02
#define	UCCID_STATUS_F_PRODUCT_VALID	0x04
#define	UCCID_STATUS_F_SERIAL_VALID	0x08

/*
 * Values for various Hardware, Mechanical, and Pin features. These come from
 * the device's class descriptor.
 */
typedef enum ccid_class_mechanical {
	CCID_CLASS_MECH_CARD_ACCEPT	= 0x01,
	CCID_CLASS_MECH_CARD_EJECT	= 0x02,
	CCID_CLASS_MECH_CARD_CAPTURE	= 0x04,
	CCID_CLASS_MECH_CARD_LOCK	= 0x08
} ccid_class_mechanical_t;

typedef enum ccid_class_features {
	CCID_CLASS_F_AUTO_PARAM_ATR	= 0x00000002,
	CCID_CLASS_F_AUTO_ICC_ACTIVATE	= 0x00000004,
	CCID_CLASS_F_AUTO_ICC_VOLTAGE	= 0x00000008,
	CCID_CLASS_F_AUTO_ICC_CLOCK	= 0x00000010,
	CCID_CLASS_F_AUTO_BAUD		= 0x00000020,
	CCID_CLASS_F_AUTO_PARAM_NEG	= 0x00000040,
	CCID_CLASS_F_AUTO_PPS		= 0x00000080,
	CCID_CLASS_F_ICC_CLOCK_STOP	= 0x00000100,
	CCID_CLASS_F_ALTNAD_SUP		= 0x00000200,
	CCID_CLASS_F_AUTO_IFSD		= 0x00000400,
	CCID_CLASS_F_TPDU_XCHG		= 0x00010000,
	CCID_CLASS_F_SHORT_APDU_XCHG	= 0x00020000,
	CCID_CLASS_F_EXT_APDU_XCHG	= 0x00040000,
	CCID_CLASS_F_WAKE_UP		= 0x00100000
} ccid_class_features_t;

typedef enum ccid_class_pin {
	CCID_CLASS_PIN_VERIFICATION	= 0x01,
	CCID_CLASS_PIN_MODIFICATION	= 0x02
} ccid_class_pin_t;

typedef struct uccid_cmd_status {
	uint32_t	ucs_version;
	uint32_t	ucs_status;
	uint32_t	ucs_hwfeatures;
	uint32_t	ucs_mechfeatures;
	uint32_t	ucs_pinfeatures;
	uint8_t		ucs_atr[UCCID_ATR_MAX];
	uint8_t		ucs_atrlen;
	uint8_t		ucs_pad[6];
	int8_t		ucs_product[256];
	int8_t		ucs_serial[256];
} uccid_cmd_status_t;

/*
 * Obtain the status of the slot. Fills in ucs_flags.
 */
#define	UCCID_CMD_STATUS	(UCCID_IOCTL | 0x3)

#if 0
typedef struct uccid_cmd_getbuf {
	uint32_t	ucg_version;
	uint32_t	ucg_buflen;
	/* XXX should this just be a static buffer with a likely maximum size? */
	void		*ucg_buffer;
} uccid_cmd_getbuf_t;

#ifdef	_KERNEL
typedef struct uccid_cmd_getbuf32 {
	uint32_t	ucg_version;
	uint32_t	ucg_buflen;
	uintptr32_t	ucg_buffer;
} uccid_cmd_getbuf32_t;
#endif

/*
 * Obtain the answer to reset for the slot. If ucs_buflen is zero, then the
 * value in ucs_buffer will be ignored and ucs_buflen will be filled in with the
 * size of the ATR. Otherwise, if large enough, it will be written to the
 * buffer.
 */
#define	UCCID_CMD_GETATR	(UCCID_IOCTL | 0x4)

/*
 * Get the USB product string or serial if available. Follows the same semantics
 * as the ATR.
 */
#define	UCCID_CMD_GETPRODSTR	(UCCID_IOCTL | 0x5)
#define	UCCID_CMD_GETSERIAL	(UCCID_IOCTL | 0x6)
#endif

#ifdef __cplusplus
}
#endif


#endif /* _SYS_USB_UCCID_H */
