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

#include "bnxt.h"

void
bnxt_dma_free(bnxt_dma_buffer_t *bdb)
{
	if (bdb->bdb_ncookies > 0) {
		VERIFY3P(bdb->bdb_dma_handle, !=, NULL);
		(void) ddi_dma_unbind_handle(bdb->bdb_dma_handle);
		bdb->bdb_ncookies = 0;
		bzero(&bdb->bdb_cookie, sizeof (ddi_dma_cookie_t));
		bdb->bdb_len = 0;
	}

	if (bdb->bdb_acc_handle != NULL) {
		ddi_dma_mem_free(&bdb->bdb_acc_handle);
		bdb->bdb_acc_handle = NULL;
		bdb->bdb_va = NULL;
	}

	if (bdb->bdb_dma_handle != NULL) {
		ddi_dma_free_handle(&bdb->bdb_dma_handle);
		bdb->bdb_dma_handle = NULL;
	}

	ASSERT3P(bdb->bdb_va, ==, NULL);
	ASSERT3U(bdb->bdb_ncookies, ==, 0);
	ASSERT3U(bdb->bdb_len, ==, 0);
}

/*
 * Allocate a DMA buffer based on the specified properties. If more than one
 * segment has been 
 */
boolean_t
bnxt_dma_alloc(bnxt_t *bnxt, bnxt_dma_buffer_t *bdb,
    ddi_dma_attr_t *attrp, ddi_device_acc_attr_t *accp, boolean_t zero,
    size_t size, boolean_t wait)
{
	int ret;
	uint_t flags = DDI_DMA_CONSISTENT;
	size_t len;
	int (*memcb)(caddr_t);

	if (wait == B_TRUE) {
		memcb = DDI_DMA_SLEEP;
	} else {
		memcb = DDI_DMA_DONTWAIT;
	}

	ret = ddi_dma_alloc_handle(bnxt->bnxt_dip, attrp, memcb, NULL,
	    &bdb->bdb_dma_handle);
	if (ret != 0) {
		bnxt_log(bnxt, "!failed to allocate DMA memory: %d", ret);
		bdb->bdb_dma_handle = NULL;
		return (B_FALSE);
	}

	ret = ddi_dma_mem_alloc(bdb->bdb_dma_handle, size, accp, flags,
	    memcb, NULL, &bdb->bdb_va, &len, &bdb->bdb_acc_handle);
	if (ret != DDI_SUCCESS) {
		bnxt_log(bnxt, "!failed to allocate DMA memory: %d", ret);
		bdb->bdb_va = NULL;
		bdb->bdb_acc_handle = NULL;
		bnxt_dma_free(bdb);
		return (B_FALSE);
	}

	if (zero) {
		bzero(bdb->bdb_va, len);
	}

	ret = ddi_dma_addr_bind_handle(bdb->bdb_dma_handle, NULL,
	    bdb->bdb_va, len, DDI_DMA_RDWR | flags, memcb, NULL,
	    &bdb->bdb_cookie, &bdb->bdb_ncookies);
	if (ret != DDI_DMA_MAPPED) {
		bdb->bdb_ncookies = 0;
		bzero(&bdb->bdb_cookie, sizeof (ddi_dma_cookie_t));
		bnxt_log(bnxt, "!failed to bind DMA memory: %d", ret);
		bnxt_dma_free(bdb);
		return (B_FALSE);
	}

	bdb->bdb_len = size;
	return (B_TRUE);
}
