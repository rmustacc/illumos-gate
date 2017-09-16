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

#ifdef __cplusplus
extern "C" {
#endif

#define	UCCID_IOCTL	(('u' << 24) | ('c' << 16) | ('d') << 8)

#define	UCCID_VERSION_ONE	1
#define	UCCID_CURRENT_VERSION	UCCID_VERSIN_ONE

/*
 * XXX We should probably put a version at the start of every ioctl.
 */

#define	UCCID_TXN_DONT_BLOCK	0x01
typedef struct uccid_cmd_txn_begin {
	uint_t	uct_version;
	uint_t	uct_flags;
} uccid_cmd_txn_begin_t;

/*
 * Attempt to obtain eclusive access. If the UCN_TXN_DONT_BLOCK flag is
 * specified, the ioctl will return immediately if exclusive access cannot be
 * gained. Otherwise, it will block in an interruptable fashion. The argument is
 * a uccid_cmd_txn_begin_t.
 */
#define	UCCID_CMD_TXN_BEGIN	(UCCID_IOCTL | 0x01)

typedef struct uccid_cmd_txn_end {
	uint_t	uct_version;
} uccid_cmd_txn_end_t;

/*
 * Reliquish exclusive access. Takes a uccid_cmd_txn_end_t. uct_flags must be zero.
 */
#define	UCCID_CMD_TXN_END	(UCCID_IOCTL | 0x02)

#define	UCCID_STATUS_F_CARD_PRESENT	0x01
#define	UCCID_STATUS_F_CARD_ACTIVE	0x02
typedef struct uccid_cmd_status {
	uint_t	ucs_version;
	uint_t	ucs_status;
} uccid_cmd_status_t;

/*
 * Obtain the status of the slot. Fills in ucs_flags.
 */
#define	UCCID_CMD_STATUS	(UCCID_IOCTL | 0x3)

typedef struct uccid_cmd_getatr {
	uint_t	ucg_version;
	uint_t	ucg_buflen;
	/* XXX should this just be a static buffer with a likely maximum size? */
	void	*ucg_buffer;
} uccid_cmd_getatr_t;

/*
 * Obtain the answer to reset for the slot. If ucs_buflen is zero, then the
 * value in ucs_buffer will be ignored and ucs_buflen will be filled in with the
 * size of the ATR. Otherwise, if large enough, it will be written to the
 * buffer.
 */
#define	UCCID_CMD_GETATR	(UCCID_IOCTL | 0x4)

#ifdef __cplusplus
}
#endif


#endif /* _SYS_USB_UCCID_H */
