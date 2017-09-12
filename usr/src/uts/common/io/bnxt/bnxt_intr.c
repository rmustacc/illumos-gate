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
 * Interrupt handler management
 */

#include "bnxt.h"

uint_t
bnxt_intr_msix(caddr_t arg1, caddr_t arg2)
{
	bnxt_t *bnxt = (bnxt_t *)arg1;
	int vec = (int)(uintptr_t)arg2;

	bnxt_error(bnxt, "received msix on vector %d", vec);
	return (DDI_INTR_CLAIMED);
}

uint_t
bnxt_intr_intx(caddr_t arg1, caddr_t arg2)
{
	bnxt_t *bnxt = (bnxt_t *)arg1;
	int vec = (int)(uintptr_t)arg2;

	bnxt_error(bnxt, "received intx on vector %d", vec);
	/* XXX Don't always claim */
	return (DDI_INTR_CLAIMED);
}
