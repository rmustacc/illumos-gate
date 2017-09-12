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
 * Interface with the Hardware Resource Manager (HWRM)
 */

#include "bnxt.h"

/*
 * We need a DMA address for the HWRM to send commands to. To simplify matters,
 * we just allocate a single page of DMA memory and size several of the
 * attributes based on that.
 */
#define	BNXT_HWRM_BUFFER_SIZE	PAGESIZE

/*
 * Per the HSI definitions, the response payload must be 16-byte aligned.
 */

/*
 * The offset from the start of the BAR to the HWRM's doorbell.
 */
#define	BNXT_HWRM_DB_OFF	0x100

/*
 * Time in milliseconds that we should wait between checks. We've opted to take
 * a slightly longer delay as we're going to be doing DMA syncs between these
 * and don't want to do them too often. By default, wait for one second.
 */
#define	BNXT_HWRM_DELAY_MS	1
#define	BNXT_HWRM_DEFAULT_TIMEOUT	1000

/*
 * Value for a Function ID that tells hardware to answer for the requesting
 * function.
 */
#define	BNXT_HWRM_FID_SELF	0xffff

/*
 * XXX Revisit DDI_DEFAULT_ACC for FM
 */
static ddi_device_acc_attr_t bnxt_hwrm_acc_attr = {
	DDI_DEVICE_ATTR_V0,
	DDI_NEVERSWAP_ACC,
	DDI_STRICTORDER_ACC,
	DDI_DEFAULT_ACC
};

/*
 * This is used for all allocations that are used to send and receive
 * allocations. The maximum we'll allocate using this is a single page. We may
 * allocate substantially less than that.
 */
static ddi_dma_attr_t bnxt_hwrm_dma_attr = {
	.dma_attr_version = DMA_ATTR_V0,
	.dma_attr_addr_lo = BNXT_DMA_ADDR_LO,
	.dma_attr_addr_hi = BNXT_DMA_ADDR_HI,
	.dma_attr_count_max = BNXT_DMA_COUNT_MAX,
	.dma_attr_align = BNXT_HWRM_DMA_ALIGN,
	.dma_attr_burstsizes = BNXT_DMA_BURSTSIZES,
	.dma_attr_seg = BNXT_DMA_SEGMENT,
	.dma_attr_minxfer = BNXT_DMA_MINXFER,
	.dma_attr_maxxfer = BNXT_DMA_MAXXFER,
	.dma_attr_sgllen = BNXT_HWRM_DMA_SGLLEN,
	.dma_attr_granular = BNXT_DMA_GRANULARITY,
	.dma_attr_flags = 0
};

/* XXX We hsould maybe inline this */
static void
bnxt_hwrm_write(bnxt_t *bnxt, uintptr_t off, uint32_t val)
{
	uintptr_t addr = (uintptr_t)bnxt->bnxt_dev_base + off;
	ddi_put32(bnxt->bnxt_dev_hdl, (void *)addr, val);
}

static void
bnxt_hwrm_init_header(bnxt_t *bnxt, void *req, uint16_t rtype)
{
	struct input *in = req;

	/*
	 * The seq_id is initialized when sending the request. Note, we don't
	 * need to explicitly use a little-endian conversion, as we're using the
	 * ddi routines with a Little endian access pattern.
	 */
	in->req_type = LE_16(rtype);
	in->cmpl_ring = LE_16(UINT16_MAX);
	in->target_id = LE_16(UINT16_MAX);
	in->resp_addr = LE_64(bnxt->bnxt_hwrm_reply.bdb_cookie.dmac_laddress);
}

/*
 * Send a message to the HWRM. We do this in a few steps:
 *
 * 1. Assign a sequence identifier.
 * 2. Make sure that the output buffer has been zeroed and synced.
 * 3. Write all bytes to the hwrm channel via PIO
 * 4. Write zeroes to the hwrm channel via PIO to get the max buffer size.
 * 5. Ring the doorbell.
 * 6. Wait the timeout for the update length to transition to being non-zero.
 * 7. Wait the timeout for the last byte to be set to the valid bit.
 */
static boolean_t
bnxt_hwrm_send_message(bnxt_t *bnxt, void *req, size_t len, uint_t timeout)
{
	size_t i;
	struct input *in = req;
	struct hwrm_err_output *resp = (void *)bnxt->bnxt_hwrm_reply.bdb_va;
	uint32_t *req32;
	uint8_t *valid;
	uint16_t resplen, rtype, err;
	uint_t maxdelay;

	if ((len % 4) != 0) {
		bnxt_error(bnxt, "!HWRM request must be 4-byte aligned, was %d",
		    len % 4);
		return (B_FALSE);
	}

	if (len > bnxt->bnxt_hwrm_max_req) {
		bnxt_error(bnxt, "!HWRM request too long (%d bytes), max of %d "
		    "bytes", len, bnxt->bnxt_hwrm_max_req);
		return (B_FALSE);
	}

	in->seq_id = bnxt->bnxt_hwrm_seqid;
	bnxt->bnxt_hwrm_seqid++;
	rtype = LE_16(in->req_type);

	/*
	 * Make sure that our output buffer is clean.
	 */
	bzero(bnxt->bnxt_hwrm_reply.bdb_va, bnxt->bnxt_hwrm_reply.bdb_len);
	BNXT_DMA_SYNC(bnxt->bnxt_hwrm_reply, DDI_DMA_SYNC_FORDEV);

	/*
	 * All requests are supposed to be 4-byte aligned.
	 */
	req32 = req;
	for (i = 0; i < len; i += 4, req32++) {
		bnxt_hwrm_write(bnxt, i, *req32);
	}

	for (; i < bnxt->bnxt_hwrm_max_req; i += 4) {
		bnxt_hwrm_write(bnxt, i, 0);
	}

	/*
	 * Note, the doorbell for the HWRM is still off of the main device
	 * handle. It is not off of the doorbell handle.
	 */
	bnxt_hwrm_write(bnxt, BNXT_HWRM_DB_OFF, 1);

	if (timeout == 0)
		timeout = bnxt->bnxt_hwrm_timeout;
	maxdelay = timeout / BNXT_HWRM_DELAY_MS;

	for (i = 0; i < maxdelay; i++) {
		BNXT_DMA_SYNC(bnxt->bnxt_hwrm_reply, DDI_DMA_SYNC_FORKERNEL);
		resplen = LE_16(resp->resp_len);
		if (resplen != 0 && resplen <= BNXT_HWRM_BUFFER_SIZE)
			break;
		delay(drv_usectohz(BNXT_HWRM_DELAY_MS * MILLISEC));
	}

	if (i >= maxdelay) {
		bnxt_error(bnxt, "timed out sending command %s waiting for "
		    "length", GET_HWRM_REQ_TYPE(rtype));
		return (B_FALSE);
	}

	valid = (uint8_t *)(void *)((uintptr_t)resp + resplen - 1);
	for (i = 0; i < maxdelay; i++) {
		BNXT_DMA_SYNC(bnxt->bnxt_hwrm_reply, DDI_DMA_SYNC_FORKERNEL);
		if (*valid == HWRM_RESP_VALID_KEY)
			break;
		delay(drv_usectohz(BNXT_HWRM_DELAY_MS * MILLISEC));
	}

	if (i >= maxdelay) {
		bnxt_error(bnxt, "timed out sending command %s waiting for "
		    "valid byte", GET_HWRM_REQ_TYPE(rtype));
		return (B_FALSE);
	}

	if ((err = LE_16(resp->error_code)) != HWRM_ERR_CODE_SUCCESS) {
		bnxt_error(bnxt, "%s command failed with code %d",
		    GET_HWRM_REQ_TYPE(rtype), err);
		mutex_exit(&bnxt->bnxt_hwrm_lock);
		return (B_FALSE);
	}

	return (B_TRUE);
}

/*
 * This command is used to get basic information about the device. It is run
 * before the device has been fully set up, so we can only assume some basic
 * aspects of the device.
 */
boolean_t
bnxt_hwrm_version_get(bnxt_t *bnxt)
{	
	struct hwrm_ver_get_input req;
	const struct hwrm_ver_get_output *resp;

	bzero(&req, sizeof (req));
	bnxt_hwrm_init_header(bnxt, &req, HWRM_VER_GET);
	req.hwrm_intf_maj = HWRM_VERSION_MAJOR;
	req.hwrm_intf_min = HWRM_VERSION_MINOR;
	req.hwrm_intf_upd = HWRM_VERSION_UPDATE;

	mutex_enter(&bnxt->bnxt_hwrm_lock);
	if (!bnxt_hwrm_send_message(bnxt, &req, sizeof (req), 0)) {
		mutex_exit(&bnxt->bnxt_hwrm_lock);
		return (B_FALSE);
	}

	/*
	 * Snapshot the version output and go through and make sure all fields
	 * that aren't part of the response header are in native endian.
	 */
	resp = (void *)bnxt->bnxt_hwrm_reply.bdb_va;
	bcopy(resp, &bnxt->bnxt_ver, sizeof (bnxt->bnxt_ver));
	bnxt->bnxt_ver.dev_caps_cfg = LE_32(bnxt->bnxt_ver.dev_caps_cfg);
	bnxt->bnxt_ver.chip_num = LE_16(bnxt->bnxt_ver.chip_num);
	bnxt->bnxt_ver.max_req_win_len = LE_16(bnxt->bnxt_ver.max_req_win_len);
	bnxt->bnxt_ver.max_resp_len = LE_16(bnxt->bnxt_ver.max_resp_len);
	bnxt->bnxt_ver.def_req_timeout = LE_16(bnxt->bnxt_ver.def_req_timeout);

	mutex_exit(&bnxt->bnxt_hwrm_lock);

	return (B_TRUE);
}

/*
 * Obtain and save basic NVM information.
 */
boolean_t
bnxt_hwrm_nvm_info_get(bnxt_t *bnxt)
{
	struct hwrm_nvm_get_dev_info_input req;
	const struct hwrm_nvm_get_dev_info_output *resp;

	bzero(&req, sizeof (req));
	bnxt_hwrm_init_header(bnxt, &req, HWRM_NVM_GET_DEV_INFO);

	mutex_enter(&bnxt->bnxt_hwrm_lock);
	/*
	 * While NVM operations may be slower, we still use the default timeout
	 * when simply getting information.
	 */
	if (!bnxt_hwrm_send_message(bnxt, &req, sizeof (req), 0)) {
		mutex_exit(&bnxt->bnxt_hwrm_lock);
		return (B_FALSE);
	}

	resp = (void *)bnxt->bnxt_hwrm_reply.bdb_va;
	bnxt->bnxt_nvm.manufacturer_id = LE_16(resp->manufacturer_id);
	bnxt->bnxt_nvm.device_id = LE_16(resp->device_id);
	bnxt->bnxt_nvm.sector_size = LE_32(resp->sector_size);
	bnxt->bnxt_nvm.nvram_size = LE_32(resp->nvram_size);
	bnxt->bnxt_nvm.reserved_size = LE_32(resp->reserved_size);
	bnxt->bnxt_nvm.available_size = LE_32(resp->available_size);
	mutex_exit(&bnxt->bnxt_hwrm_lock);
	return (B_TRUE);
}

boolean_t
bnxt_hwrm_func_reset(bnxt_t *bnxt)
{
	struct hwrm_func_reset_input req;
	uint_t to;
	boolean_t ret;

	/*
	 * Configure the command to reset just the current PF. By zeroing it,
	 * that tells it to only reset the current PF and ignore any VFs (none
	 * of which are used at this time).
	 */
	bzero(&req, sizeof (req));
	bnxt_hwrm_init_header(bnxt, &req, HWRM_FUNC_RESET);

	/*
	 * Give ourselves a slightly longer time out than normal. We multiply
	 * the standard timeout set by hardware by four (a rather arbitrary
	 * multiplier).
	 */
	to = (uint_t)bnxt->bnxt_hwrm_timeout * 4;
	mutex_enter(&bnxt->bnxt_hwrm_lock);
	ret = bnxt_hwrm_send_message(bnxt, &req, sizeof (req), to);
	mutex_exit(&bnxt->bnxt_hwrm_lock);
	return (ret);
}

boolean_t
bnxt_hwrm_func_qcaps(bnxt_t *bnxt)
{
	struct hwrm_func_qcaps_input req;
	const struct hwrm_func_qcaps_output *resp;

	bzero(&req, sizeof (req));
	bnxt_hwrm_init_header(bnxt, &req, HWRM_FUNC_QCAPS);
	req.fid = LE_16(BNXT_HWRM_FID_SELF);

	mutex_enter(&bnxt->bnxt_hwrm_lock);
	if (!bnxt_hwrm_send_message(bnxt, &req, sizeof (req), 0)) {
		mutex_exit(&bnxt->bnxt_hwrm_lock);
		return (B_FALSE);
	}

	resp = (void *)bnxt->bnxt_hwrm_reply.bdb_va;
	bnxt->bnxt_fid = LE_16(resp->fid);
	bnxt->bnxt_port_id = LE_16(resp->port_id);
	bnxt->bnxt_qcap_flags = LE_32(resp->flags);
	bcopy(resp->mac_address, bnxt->bnxt_macaddr, ETHERADDRL);

	bnxt->bnxt_max_rsscos_ctx = LE_16(resp->max_rsscos_ctx);
	bnxt->bnxt_max_cmpl_rings = LE_16(resp->max_cmpl_rings);
	bnxt->bnxt_max_tx_rings = LE_16(resp->max_tx_rings);
	bnxt->bnxt_max_rx_rings = LE_16(resp->max_rx_rings);
	bnxt->bnxt_max_l2_ctxs = LE_16(resp->max_l2_ctxs);
	bnxt->bnxt_max_vnics = LE_16(resp->max_vnics);
	bnxt->bnxt_max_stat_ctx = LE_16(resp->max_stat_ctx);
	bnxt->bnxt_max_rx_em_flows = LE_32(resp->max_rx_em_flows);
	bnxt->bnxt_max_rx_wm_flows = LE_32(resp->max_rx_wm_flows);
	bnxt->bnxt_max_mcast_filters = LE_32(resp->max_mcast_filters);
	bnxt->bnxt_max_flow_id = LE_32(resp->max_flow_id);
	bnxt->bnxt_max_hw_ring_grps = LE_32(resp->max_hw_ring_grps);

	mutex_exit(&bnxt->bnxt_hwrm_lock);

	return (B_TRUE);
}

boolean_t
bnxt_hwrm_func_qcfg(bnxt_t *bnxt)
{
	struct hwrm_func_qcfg_input req;
	const struct hwrm_func_qcfg_output *resp;

	bzero(&req, sizeof (req));
	bnxt_hwrm_init_header(bnxt, &req, HWRM_FUNC_QCFG);
	req.fid = LE_16(BNXT_HWRM_FID_SELF);

	mutex_enter(&bnxt->bnxt_hwrm_lock);
	if (!bnxt_hwrm_send_message(bnxt, &req, sizeof (req), 0)) {
		mutex_exit(&bnxt->bnxt_hwrm_lock);
		return (B_FALSE);
	}

	resp = (void *)bnxt->bnxt_hwrm_reply.bdb_va;
	bnxt->bnxt_alloc_cmpl_rings = LE_16(resp->alloc_cmpl_rings);
	bnxt->bnxt_alloc_tx_rings = LE_16(resp->alloc_tx_rings);
	bnxt->bnxt_alloc_rx_rings = LE_16(resp->alloc_rx_rings);
	bnxt->bnxt_alloc_vnics = LE_16(resp->alloc_vnics);
	bnxt->bnxt_alloc_mcast_filters = LE_32(resp->alloc_mcast_filters);
	bnxt->bnxt_alloc_hw_ring_grps = LE_32(resp->alloc_hw_ring_grps);
	mutex_exit(&bnxt->bnxt_hwrm_lock);

	return (B_TRUE);
}

boolean_t
bnxt_hwrm_queue_qportcfg(bnxt_t *bnxt)
{
	struct hwrm_queue_qportcfg_input req;
	const struct hwrm_queue_qportcfg_output *resp;

	bzero(&req, sizeof (req));
	bnxt_hwrm_init_header(bnxt, &req, HWRM_QUEUE_QPORTCFG);

	mutex_enter(&bnxt->bnxt_hwrm_lock);
	if (!bnxt_hwrm_send_message(bnxt, &req, sizeof (req), 0)) {
		mutex_exit(&bnxt->bnxt_hwrm_lock);
		return (B_FALSE);
	}

	resp = (void *)bnxt->bnxt_hwrm_reply.bdb_va;
	bcopy(resp, &bnxt->bnxt_qportcfg, sizeof (*resp));
	mutex_exit(&bnxt->bnxt_hwrm_lock);

	return (B_TRUE);
}

boolean_t
bnxt_hwrm_host_unregister(bnxt_t *bnxt)
{
	struct hwrm_func_drv_rgtr_input req;
	boolean_t ret;

	bzero(&req, sizeof (req));
	bnxt_hwrm_init_header(bnxt, &req, HWRM_FUNC_DRV_UNRGTR);
	mutex_enter(&bnxt->bnxt_hwrm_lock);
	ret = bnxt_hwrm_send_message(bnxt, &req, sizeof (req), 0);
	mutex_exit(&bnxt->bnxt_hwrm_lock);
	return (ret);
}

boolean_t
bnxt_hwrm_host_register(bnxt_t *bnxt)
{
	struct hwrm_func_drv_rgtr_input req;
	boolean_t ret;

	bzero(&req, sizeof (req));
	bnxt_hwrm_init_header(bnxt, &req, HWRM_FUNC_DRV_RGTR);
	req.enables = LE_32(HWRM_FUNC_DRV_RGTR_INPUT_ENABLES_VER |
	    HWRM_FUNC_DRV_RGTR_INPUT_ENABLES_OS_TYPE);
	req.os_type = LE_16(HWRM_FUNC_DRV_RGTR_INPUT_OS_TYPE_SOLARIS);

	mutex_enter(&bnxt->bnxt_hwrm_lock);
	ret = bnxt_hwrm_send_message(bnxt, &req, sizeof (req), 0);
	mutex_exit(&bnxt->bnxt_hwrm_lock);
	return (ret);
}

boolean_t
bnxt_hwrm_register_events(bnxt_t *bnxt)
{
	struct hwrm_func_drv_rgtr_input req;
	boolean_t ret;
	ulong_t *bitmap;
	uint_t i;

	bzero(&req, sizeof (req));
	bnxt_hwrm_init_header(bnxt, &req, HWRM_FUNC_DRV_RGTR);
	req.enables = HWRM_FUNC_DRV_RGTR_INPUT_ENABLES_ASYNC_EVENT_FWD;
	bitmap = (ulong_t *)req.async_event_fwd;

	ASSERT0(sizeof (req.async_event_fwd) % sizeof (ulong_t));

	BT_SET(bitmap, HWRM_ASYNC_EVENT_CMPL_EVENT_ID_LINK_STATUS_CHANGE);
	BT_SET(bitmap, HWRM_ASYNC_EVENT_CMPL_EVENT_ID_LINK_MTU_CHANGE);
	BT_SET(bitmap, HWRM_ASYNC_EVENT_CMPL_EVENT_ID_LINK_SPEED_CHANGE);
	BT_SET(bitmap, HWRM_ASYNC_EVENT_CMPL_EVENT_ID_PORT_CONN_NOT_ALLOWED);
	BT_SET(bitmap, HWRM_ASYNC_EVENT_CMPL_EVENT_ID_LINK_SPEED_CFG_CHANGE);
	BT_SET(bitmap, HWRM_ASYNC_EVENT_CMPL_EVENT_ID_HWRM_ERROR);

	for (i = 0; i < sizeof (req.async_event_fwd) / sizeof (ulong_t); i++) {
#ifdef	_LP64
		bitmap[i] = LE_64(bitmap[i]);
#else
		bitmap[i] = LE_32(bitmap[i]);
#endif
	}

	mutex_enter(&bnxt->bnxt_hwrm_lock);
	ret = bnxt_hwrm_send_message(bnxt, &req, sizeof (req), 0);
	mutex_exit(&bnxt->bnxt_hwrm_lock);
	return (ret);
}

boolean_t
bnxt_hwrm_ring_alloc(bnxt_t *bnxt, bnxt_ring_t *brp, uint8_t type,
    uint16_t comp_idx, uint32_t stat_idx)
{
	struct hwrm_ring_alloc_input req;
	const struct hwrm_ring_alloc_output *resp;

	VERIFY0(brp->br_flags & BNXT_RING_F_HW_ALLOCED);
	VERIFY0(brp->br_flags & BNXT_RING_F_INTR_ENABLED);
	VERIFY3U(brp->br_hw_ring_id, ==, (uint16_t)HWRM_NA_SIGNATURE);
	bzero(&req, sizeof (req));
	bnxt_hwrm_init_header(bnxt, &req, HWRM_FUNC_RING_ALLOC);

	/*
	 * By default, don't enable any extra information through the enables
	 * flags unless we need to associate a stats context with this.
	 * Similarly, don't set any of the page bits in another way.
	 */

	req.enables = LE_32(0);
	req.fbo = LE_32(0);

	if (stat_idx != (uint16_t)HWRM_NA_SIGNATURE)
		panic("implement stat_idx ring alloc");

	if (comp_idx != (uint16_t)HWRM_NA_SIGNATURE)
		panic("implement comp_idx ring alloc");

	switch (type) {
	case HWRM_RING_ALLOC_INPUT_RING_TYPE_L2_CMPL:
		req.cmpl_ring_id = (uint16_t)HWRM_NA_SIGNATURE;
		req.queue_id = (uint16_t)HWRM_NA_SIGNATURE;
		break;
	default:
		bnxt_error(bnxt, "asked to allocate unknown ring type: %d",
		    type);
		ieturn (B_FALSE);
	}

	req.ring_type = type;
	req.length = LE_32(brp->br_nentries);
	req.logical_id = LE_16(brp->br_sw_ring_id);


	return (B_FALSE);
}

boolean_t
bnxt_hwrm_ring_free(bnxt_t *bnxt, bnxt_ring_t *brp)
{
	struct hwrm_ring_free_input req;
	boolean_t ret;

	VERIFY(brp->br_flags & BNXT_RING_F_HW_ALLOCED);
	VERIFY0(brp->br_flags & BNXT_RING_F_INTR_ENABLED);
	VERIFY3U(brp->br_hw_ring_id, !=, (uint16_t)HWRM_NA_SIGNATURE);

	bzero(&req, sizeof (req));
	bnxt_hwrm_init_header(bnxt, &req, HWRM_RING_FREE);
	req.ring_type = brp->br_type;
	req.ring_id = LE_16(brp->br_hw_ring_id);

	mutex_enter(&bnxt->bnxt_hwrm_lock);
	ret = bnxt_hwrm_send_message(bnxt, &req, sizeof (req), 0);
	if (ret) {
		brp->br_flags &= ~BNXT_RING_F_HW_ALLOCED;
		brp->br_hw_ring_id = (uint16_t)HWRM_NA_SIGNATURE;
	}
	mutex_exit(&bnxt->bnxt_hwrm_lock);

	return (ret);
}

void
bnxt_hwrm_fini(bnxt_t *bnxt)
{
	bnxt_dma_free(&bnxt->bnxt_hwrm_reply);
	mutex_destroy(&bnxt->bnxt_hwrm_lock);
}

boolean_t
bnxt_hwrm_init(bnxt_t *bnxt)
{
	if (!bnxt_dma_alloc(bnxt, &bnxt->bnxt_hwrm_reply,
	    &bnxt_hwrm_dma_attr, &bnxt_hwrm_acc_attr, B_TRUE,
	    BNXT_HWRM_BUFFER_SIZE, B_TRUE)) {
		return (B_FALSE);
	}

	mutex_init(&bnxt->bnxt_hwrm_lock, NULL, MUTEX_DRIVER, NULL);

	/*
	 * We need too assign a default timeout time. For the moment, we
	 * basically opt to wait for 1 second in 10ms ticks.
	 */
	bnxt->bnxt_hwrm_timeout = BNXT_HWRM_DEFAULT_TIMEOUT;
	bnxt->bnxt_hwrm_max_req = HWRM_MAX_REQ_LEN;

	return (B_TRUE);
}
