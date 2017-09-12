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
 * bnxt main driver
 */

#include "bnxt.h"

/*
 * These represent the access attributes for the PCIe backed hardware registers.
 * Note, we set neverswap for the hwrm_registers. This is a bit different from
 * normal, but the problem is that we have to write out structures as a series
 * of 4 byte writes, which means that if we do swapping, we'll end up screwing
 * things up on big endin systems.
 * XXX We shoul enable flagerr
 */
static ddi_device_acc_attr_t bnxt_hwrm_regs_acc_attr = {
	DDI_DEVICE_ATTR_V1,
	DDI_NEVERSWAP_ACC,
	DDI_STRICTORDER_ACC
};

static ddi_device_acc_attr_t bnxt_doorbell_acc_attr = {
	DDI_DEVICE_ATTR_V1,
	DDI_STRUCTURE_LE_ACC,
	DDI_STRICTORDER_ACC
};

/*
 * Basic DMA attributes for completion rings
 */
static ddi_device_acc_attr_t bnxt_comp_rings_acc_attr = {
	DDI_DEVICE_ATTR_V1,
	DDI_NEVERSWAP_ACC,
	DDI_STRICTORDER_ACC
};

static ddi_dma_attr_t bnxt_comp_rings_dma_attr = {
	.dma_attr_version = DMA_ATTR_V0,
	.dma_attr_addr_lo = BNXT_DMA_ADDR_LO,
	.dma_attr_addr_hi = BNXT_DMA_ADDR_HI,
	.dma_attr_count_max = BNXT_DMA_COUNT_MAX,
	.dma_attr_align = BNXT_RING_DMA_ALIGN,
	.dma_attr_burstsizes = BNXT_DMA_BURSTSIZES,
	.dma_attr_seg = BNXT_DMA_SEGMENT,
	.dma_attr_minxfer = 0x1,
	.dma_attr_maxxfer = BNXT_DMA_MAXXFER,
	.dma_attr_sgllen = BNXT_RING_DMA_SGLLEN,
	.dma_attr_granular = BNXT_DMA_GRANULARITY,
	.dma_attr_flags = 0
};

void
bnxt_log(bnxt_t *bnxt, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vdev_err(bnxt->bnxt_dip, CE_NOTE, fmt, ap);
	va_end(ap);
}

void
bnxt_error(bnxt_t *bnxt, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vdev_err(bnxt->bnxt_dip, CE_WARN, fmt, ap);
	va_end(ap);
}

/*
 * Reset the completion ring in question by setting the cycle bit to the default
 * value (1), and then marking all entries to be the opposite of that.
 */
void
bnxt_comp_ring_reset(bnxt_comp_ring_t *bcrp)
{
	struct cmpl_base *cp;

	cp = (struct cmpl_base *)bcrp->bcr_ring.br_dma.bdb_va;

	/*
	 * Zero the ring just to make sure any leftovers are gone. This also
	 * ensures that we've set the required completion to bit to zero. We
	 * should expect that a consumer will write ones the first time.
	 */
	bzero(cp, bcrp->bcr_ring.br_rsize);
	bcrp->bcr_cycle = 1;
	BNXT_DMA_SYNC(bcrp->bcr_ring.br_dma, DDI_DMA_SYNC_FORDEV);
}

/*
 * We need to map two different BARs: the core device registers and the
 * doorbells.
 */
static boolean_t
bnxt_regs_map(bnxt_t *bnxt)
{
	int ret;
	off_t devsz, bellsz;

	if (ddi_dev_regsize(bnxt->bnxt_dip, BNXT_BAR_DEVICE, &devsz) != DDI_SUCCESS) {
		bnxt_error(bnxt, "failed to get register size for BAR %d",
		    BNXT_BAR_DEVICE);
		return (B_FALSE);
	}

	if (ddi_dev_regsize(bnxt->bnxt_dip, BNXT_BAR_DOORBELL, &bellsz) != DDI_SUCCESS) {
		bnxt_error(bnxt, "failed to get register size for BAR %d",
		    BNXT_BAR_DEVICE);
		return (B_FALSE);
	}

	if ((ret = ddi_regs_map_setup(bnxt->bnxt_dip, BNXT_BAR_DEVICE,
	    &bnxt->bnxt_dev_base, 0, devsz, &bnxt_hwrm_regs_acc_attr,
	    &bnxt->bnxt_dev_hdl)) != DDI_SUCCESS) {
		bnxt_error(bnxt, "failed to map bar %d: %d", BNXT_BAR_DEVICE, ret);
		return (B_FALSE);
	}

	if ((ret = ddi_regs_map_setup(bnxt->bnxt_dip, BNXT_BAR_DOORBELL,
	    &bnxt->bnxt_doorbell_base, 0, bellsz, &bnxt_doorbell_acc_attr,
	    &bnxt->bnxt_doorbell_hdl)) != DDI_SUCCESS) {
		ddi_regs_map_free(&bnxt->bnxt_dev_hdl);
		bnxt_error(bnxt, "failed to map bar %d: %d", BNXT_BAR_DOORBELL, ret);
		return (B_FALSE);
	}

	return (B_TRUE);
}

static void
bnxt_default_ring_free(bnxt_t *bnxt)
{
	bnxt_comp_ring_t *bcrp = &bnxt->bnxt_default_ring;

	bcrp->bcr_ring.br_sw_ring_id = (uint16_t)HWRM_NA_SIGNATURE;
	bcrp->bcr_ring.br_hw_ring_id = (uint16_t)HWRM_NA_SIGNATURE;
	bcrp->bcr_ring.br_rsize = 0;
	bcrp->bcr_ring.br_nentries = 0;
	bnxt_dma_free(&bcrp->bcr_ring.br_dma);
}
/*
 * XXX Refactor this into a common routine for ring allocation at some
 * point.
 */
static boolean_t
bnxt_default_ring_alloc(bnxt_t *bnxt)
{
	bnxt_comp_ring_t *bcrp = &bnxt->bnxt_default_ring;

	bcrp->bcr_ring.br_rsize = PAGESIZE;
	if (!bnxt_dma_alloc(bnxt, &bcrp->bcr_ring.br_dma,
	    &bnxt_comp_rings_dma_attr, &bnxt_comp_rings_acc_attr, B_TRUE, PAGESIZE,
	    B_TRUE)) {
		return (B_FALSE);
	}

	bcrp->bcr_ring.br_sw_ring_id = BNXT_DEFAULT_RING_SW_ID;
	bcrp->bcr_ring.br_hw_ring_id = (uint16_t)HWRM_NA_SIGNATURE;
	bcrp->bcr_ring.br_rsize = PAGESIZE;
	bcrp->bcr_ring.br_nentries = bcrp->bcr_ring.br_rsize /
	    sizeof (struct cmpl_base);

	/*
	 * Initialize the cycle to zero and don't enable any stats for the
	 * default callback ring.
	 */
	bcrp->bcr_cycle = 0;
	bcrp->bcr_tail = 0;
	bcrp->bcr_hw_stat_id = HWRM_NA_SIGNATURE;

	return (B_TRUE);
}

static void
bnxt_intrs_free(bnxt_t *bnxt)
{
	int i, ret;

	if (bnxt->bnxt_nintrs > 0) {
		VERIFY3P(bnxt->bnxt_intr_handles, !=, NULL);
	}

	for (i = 0; i < bnxt->bnxt_nintrs; i++) {
		/*
		 * If this fails, there's really not much that we can do in this
		 * state. The manual page doesn't define very clear reasons that
		 * the interrupt handle freeing should fail.
		 */
		if ((ret = ddi_intr_free(bnxt->bnxt_intr_handles[i])) !=
		    DDI_SUCCESS) {
			bnxt_log(bnxt, "failed to free interrupt %d: %d", i,
			    ret);
		}
	}

	if (bnxt->bnxt_intr_handles != NULL) {
		kmem_free(bnxt->bnxt_intr_handles,
		    sizeof (ddi_intr_handle_t) * bnxt->bnxt_nintrs_req);
		bnxt->bnxt_intr_handles = NULL;
		bnxt->bnxt_nintrs_req = 0;
	}

	/*
	 * Zero out fields that may or may not have been filled in.
	 */
	bnxt->bnxt_intr_type = 0;
	bnxt->bnxt_intr_pri = 0;
	bnxt->bnxt_intr_caps = 0;
	VERIFY0(bnxt->bnxt_nintrs_req);
}

static boolean_t
bnxt_alloc_intr_handles(bnxt_t *bnxt, int type)
{
	int avail, min, ret, alloc;

	if ((ret = ddi_intr_get_navail(bnxt->bnxt_dip, type, &avail)) !=
	    DDI_SUCCESS) {
		bnxt_error(bnxt, "failed to get available interrupts: %d", ret);
		return (B_FALSE);
	}

	switch (type) {
	case DDI_INTR_TYPE_MSIX:
		/*
		 * XXX We need to come back here and size the number of rings,
		 * etc. that we actually want to allocate and use.
		 */
		alloc = MIN(1, avail);
		min = 1;
		break;
	case DDI_INTR_TYPE_FIXED:
		alloc = 1;
		min = 1;
		break;
	default:
		return (B_FALSE);
	}

	bnxt->bnxt_nintrs_req = alloc;
	bnxt->bnxt_intr_handles = kmem_alloc(sizeof (ddi_intr_handle_t) *
	    bnxt->bnxt_nintrs_req, KM_SLEEP);
	if ((ret = ddi_intr_alloc(bnxt->bnxt_dip, bnxt->bnxt_intr_handles, type, 0, alloc,
	    &bnxt->bnxt_nintrs, DDI_INTR_ALLOC_NORMAL)) != DDI_SUCCESS) {
		bnxt_error(bnxt, "failed to allocate %d interrupts of "
		    "type %d: %d", alloc, type, ret);
		goto cleanup;
	}
	bnxt->bnxt_intr_type = type;

	if (bnxt->bnxt_nintrs < min) {
		bnxt_error(bnxt, "allocated less than the required number of "
		    "interrupts of type %d, received %d, minimum was %d", type,
		    bnxt->bnxt_nintrs, min);
		goto cleanup;
	}

	if ((ret = ddi_intr_get_cap(bnxt->bnxt_intr_handles[0],
	    &bnxt->bnxt_intr_caps)) != DDI_SUCCESS) {
		bnxt_error(bnxt, "failed to get interrupt capabilities for "
		    "type %d: %d", type, ret);
		goto cleanup;
	}

	if ((ret = ddi_intr_get_pri(bnxt->bnxt_intr_handles[0],
	    &bnxt->bnxt_intr_pri)) != DDI_SUCCESS) {
		bnxt_error(bnxt, "failed to get interrupt priorites for "
		    "type %d: %d", type, ret);
		goto cleanup;
	}

	return (B_TRUE);
cleanup:
	bnxt_intrs_free(bnxt);
	return (B_FALSE);
}

static boolean_t
bnxt_intrs_alloc(bnxt_t *bnxt)
{
	int types, ret;

	if ((ret = ddi_intr_get_supported_types(bnxt->bnxt_dip, &types)) !=
	    DDI_SUCCESS) {
		bnxt_error(bnxt, "failed to get supported interrupt types: %d",
		    ret);
	}

	if (types & DDI_INTR_TYPE_MSIX) {
		if (bnxt_alloc_intr_handles(bnxt, DDI_INTR_TYPE_MSIX))
			return (B_TRUE);
	}

	/*
	 * While the PCI device may have MSI support advertised, there is no
	 * support in the hwardware/software interface for specifying that we
	 * have an MSI interrupt in the hardware completion rings. This is why
	 * we only check for MSI-X and INTx interrupts.
	 */
	if (types & DDI_INTR_TYPE_FIXED) {
		if (bnxt_alloc_intr_handles(bnxt, DDI_INTR_TYPE_FIXED))
			return (B_TRUE);
	}

	/*
	 * At this time, we're not bothering to support fixed interrupts for
	 * this device.
	 */
	bnxt_error(bnxt, "failed to allocate interrupts: failed to allocate "
	    "MSI or MSI-X interrupts");

	return (B_FALSE);
}

/*
 * Go through and parse the identifying information that we received. Make sure
 * that we can support this firmware revision and if there are device-specific
 * flags that we need to set.
 */
static boolean_t
bnxt_version_parse(bnxt_t *bnxt)
{
	/*
	 * First check the firmware rev. If older firmware is in place, then we
	 * need to error out.
	 */
	bnxt_log(bnxt, "!HWRM interface at version %d.%d.%d",
	    bnxt->bnxt_ver.hwrm_intf_maj, bnxt->bnxt_ver.hwrm_intf_min,
	    bnxt->bnxt_ver.hwrm_intf_upd);
	if (bnxt->bnxt_ver.hwrm_intf_maj < 1) {
		bnxt_error(bnxt, "bnxt driver requires HWRM at least at major "
		    "version 1, at %d.%d.%d", bnxt->bnxt_ver.hwrm_intf_maj,
		    bnxt->bnxt_ver.hwrm_intf_min, bnxt->bnxt_ver.hwrm_intf_upd);
		return (B_FALSE);
	}

	/*
	 * The HWRM offers a secondary short command mode. At the moment we have
	 * not implemented it. If hardware marks that it requires it in the
	 * version structures, then we need to fail attach.
	 */
	if ((bnxt->bnxt_ver.dev_caps_cfg &
	    HWRM_VER_GET_OUTPUT_DEV_CAPS_CFG_SHORT_CMD_SUPPORTED) &&
	    (bnxt->bnxt_ver.dev_caps_cfg &
	    HWRM_VER_GET_OUTPUT_DEV_CAPS_CFG_SHORT_CMD_REQUIRED)) {
		bnxt_error(bnxt, "HWRM requires unsupported short command mode");
		return (B_FALSE);
	}

	/*
	 * A class of devices need to ring the doorbell twice when performing
	 * I/O.
	 *
	 * XXX For the moment we set this on everything like fbsd. We should be
	 * have like Linux and not always do it.
	 */
	bnxt->bnxt_flags |= BNXT_F_DOUBLE_DOORBELL;

	/*
	 * Know that we know what hardware supports, update the default timeout.
	 */
	if (bnxt->bnxt_ver.def_req_timeout != 0) {
		bnxt->bnxt_hwrm_timeout = bnxt->bnxt_ver.def_req_timeout;
	}

	if (bnxt->bnxt_ver.max_req_win_len != 0) {
		bnxt->bnxt_hwrm_max_req = bnxt->bnxt_ver.max_req_win_len;
	}

	return (B_TRUE);
}

static void
bnxt_intr_handlers_fini(bnxt_t *bnxt)
{
	int i, ret;

	for (i = 0; i < bnxt->bnxt_nintrs; i++) {
		/*
		 * There is little we can do in the face of failure here. Log
		 * that this occurred and move on.
		 */
		ret = ddi_intr_remove_handler(bnxt->bnxt_intr_handles[i]);
		if (ret != DDI_SUCCESS) {
			bnxt_error(bnxt, "failed to remove interrupt handler "
			    "%d: %d", i, ret);
		}
	}
}

static boolean_t
bnxt_intr_handlers_init(bnxt_t *bnxt)
{
	int i, ret;

	switch (bnxt->bnxt_intr_type) {
	case DDI_INTR_TYPE_MSIX:
		for (i = 0; i < bnxt->bnxt_nintrs; i++) {
			ret = ddi_intr_add_handler(bnxt->bnxt_intr_handles[i],
			    bnxt_intr_msix, bnxt, (void *)(uintptr_t)i);
			if (ret != DDI_SUCCESS) {
				bnxt_error(bnxt, "failed to add MSIX interrupt "
				    "handler failed: %d", ret);
				for (i--; i >= 0; i--) {
					(void) ddi_intr_remove_handler(
					    bnxt->bnxt_intr_handles[i]);
				}
				return (B_FALSE);
			}
		}
		break;
	case DDI_INTR_TYPE_FIXED:
		if ((ret = ddi_intr_add_handler(bnxt->bnxt_intr_handles[0],
		    bnxt_intr_intx, bnxt, (void *)(uintptr_t)0)) !=
		    DDI_SUCCESS) {
			bnxt_error(bnxt, "failed to add MSI interrupt handler "
			    "failed: %d", ret);
			return (B_FALSE);
		}
	default:
		bnxt_error(bnxt, "cannot add intr handles for unknown "
		    "interrupt type %d", bnxt->bnxt_intr_type);
		return (B_FALSE);
	}

	return (B_TRUE);
}

static void
bnxt_intrs_disable(bnxt_t *bnxt)
{
	int ret;

	if (bnxt->bnxt_intr_caps & DDI_INTR_FLAG_BLOCK) {
		if ((ret = ddi_intr_block_disable(bnxt->bnxt_intr_handles,
		    bnxt->bnxt_nintrs)) != DDI_SUCCESS) {
			bnxt_error(bnxt, "failed to block-disable "
			    "interrupts: %d", ret);
		}
	} else {
		int i;

		for (i = 0; i < bnxt->bnxt_nintrs; i++) {
			ret = ddi_intr_disable(bnxt->bnxt_intr_handles[i]);
			if (ret != DDI_SUCCESS) {
				bnxt_error(bnxt, "failed to disable "
				    "interrupt %d", ret);
			}
		}
	}
}

static boolean_t
bnxt_intrs_enable(bnxt_t *bnxt)
{
	int ret;

	if (bnxt->bnxt_intr_caps & DDI_INTR_FLAG_BLOCK) {
		if ((ret = ddi_intr_block_enable(bnxt->bnxt_intr_handles,
		    bnxt->bnxt_nintrs)) != DDI_SUCCESS) {
			bnxt_error(bnxt, "interrupt block-enable failed: %d",
			    ret);
		}
	} else {
		int i;
		for (i = 0; i < bnxt->bnxt_nintrs; i++) {
			ret = ddi_intr_enable(bnxt->bnxt_intr_handles[i]);
			if (ret != DDI_SUCCESS) {
				bnxt_error(bnxt, "failed to enable interrupt "
				    "%d: %d", i, ret);
				while (--i >= 0) {
					ret = ddi_intr_disable(
					    bnxt->bnxt_intr_handles[i]);
					if (ret != DDI_SUCCESS) {
						bnxt_error(bnxt, "failed to disable "
						    "interrupt %d: %d",
						    i, ret);
					}
				}
			}
		}
	}

	return (B_TRUE);
}

static void
bnxt_teardown(dev_info_t *dip, bnxt_t *bnxt)
{
	if (bnxt->bnxt_attach_progress & BNXT_ATTACH_GLDV3) {
		bnxt_mac_unregister(bnxt);
		bnxt->bnxt_attach_progress &= ~BNXT_ATTACH_GLDV3;
	}

	if (bnxt->bnxt_attach_progress & BNXT_ATTACH_ENABLE_INTRS) {
		bnxt_intrs_disable(bnxt);
		bnxt->bnxt_attach_progress &= ~BNXT_ATTACH_ENABLE_INTRS;
	}

	if (bnxt->bnxt_attach_progress & BNXT_ATTACH_INTR_HANDLERS) {
		bnxt_intr_handlers_fini(bnxt);
		bnxt->bnxt_attach_progress &= ~BNXT_ATTACH_INTR_HANDLERS;
	}

	if (bnxt->bnxt_attach_progress & BNXT_ATTACH_ALLOC_INTRS) {
		bnxt_intrs_free(bnxt);
		bnxt->bnxt_attach_progress &= ~BNXT_ATTACH_ALLOC_INTRS;
	}

	if (bnxt->bnxt_attach_progress & BNXT_ATTACH_DEF_RING) {
		bnxt_default_ring_free(bnxt);
		bnxt->bnxt_attach_progress &= ~BNXT_ATTACH_DEF_RING;
	}

	if (bnxt->bnxt_attach_progress & BNXT_ATTACH_HWRM_INIT) {
		(void) bnxt_hwrm_host_unregister(bnxt);
		bnxt_hwrm_fini(bnxt);
		bnxt->bnxt_attach_progress &= ~BNXT_ATTACH_HWRM_INIT;
	}

	if (bnxt->bnxt_attach_progress & BNXT_ATTACH_REGS_MAP) {
		ddi_regs_map_free(&bnxt->bnxt_dev_hdl);
		ddi_regs_map_free(&bnxt->bnxt_doorbell_hdl);
		bnxt->bnxt_dev_hdl = NULL;
		bnxt->bnxt_doorbell_hdl = NULL;
		bnxt->bnxt_attach_progress &= ~BNXT_ATTACH_PCI_CONFIG;
	}

	if (bnxt->bnxt_attach_progress & BNXT_ATTACH_PCI_CONFIG) {
		pci_config_teardown(&bnxt->bnxt_pci_hdl);
		bnxt->bnxt_pci_hdl = NULL;
		bnxt->bnxt_attach_progress &= ~BNXT_ATTACH_PCI_CONFIG;
	}

	kmem_free(bnxt, sizeof (bnxt_t));
	ddi_set_driver_private(dip, NULL);
}

/*
 * XXX This should probably allow a property to override this default MTU.
 */
static void
bnxt_mtu_init(bnxt_t *bnxt)
{
	bnxt->bnxt_mtu = BNXT_DEFAULT_MTU;
}

static int
bnxt_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	bnxt_t *bnxt;

	if (cmd != DDI_ATTACH)
		return (DDI_FAILURE);

	bnxt = kmem_zalloc(sizeof (bnxt_t), KM_SLEEP);
	bnxt->bnxt_dip = dip;

	ddi_set_driver_private(dip, bnxt);

	if (pci_config_setup(dip, &bnxt->bnxt_pci_hdl) != DDI_SUCCESS) {
		bnxt_error(bnxt, "failed to map PCI configurations");
		goto cleanup;
	}
	bnxt->bnxt_attach_progress |= BNXT_ATTACH_PCI_CONFIG;

	/*
	 * XXX if we support or disable FM, we should update the copied attr
	 * appropriately.
	 */
	if (!bnxt_regs_map(bnxt)) {
		bnxt_error(bnxt, "failed to map device registers");
		goto cleanup;
	}
	bnxt->bnxt_attach_progress |= BNXT_ATTACH_REGS_MAP;

	if (!bnxt_hwrm_init(bnxt)) {
		bnxt_error(bnxt, "failed to allocate HWRM resources");
		goto cleanup;
	}
	bnxt->bnxt_attach_progress |= BNXT_ATTACH_HWRM_INIT;

	/*
	 * Before we proceed further, we need to use the HWRM to ask the version
	 * of the device. This will tell us major aspects of the firmware.
	 */
	if (!bnxt_hwrm_version_get(bnxt)) {
		bnxt_error(bnxt, "failed to get HWRM version");
		goto cleanup;
	}

	if (!bnxt_version_parse(bnxt)) {
		bnxt_error(bnxt, "failed to parse version info");
		goto cleanup;
	}

	if (!bnxt_hwrm_nvm_info_get(bnxt)) {
		bnxt_error(bnxt, "failed to get nvm info");
		goto cleanup;
	}

	/*
	 * Register with the device.
	 */
	if (!bnxt_hwrm_host_register(bnxt)) {
		bnxt_error(bnxt, "failed to register host driver");
		goto cleanup;
	}

	if (!bnxt_hwrm_register_events(bnxt)) {
		bnxt_error(bnxt, "failed to request event notification");
		goto cleanup;
	}

	/*
	 * Reset the function into a known state so we can configure all the
	 * needed resources.
	 */
	if (!bnxt_hwrm_func_reset(bnxt)) {
		bnxt_error(bnxt, "failed to reset PCI function");
		goto cleanup;
	}

	/*
	 * XXX We should come back and gather LED caps
	 */
	if (!bnxt_hwrm_func_qcaps(bnxt)) {
		bnxt_error(bnxt, "failed to get PF queue caps");
		goto cleanup;
	}

	if (!bnxt_hwrm_func_qcfg(bnxt)) {
		bnxt_error(bnxt, "failed to get PF queue config");
		goto cleanup;
	}

	if (!bnxt_hwrm_queue_qportcfg(bnxt)) {
		bnxt_error(bnxt, "failed to get PF port queue config");
		goto cleanup;
	}

	if (!bnxt_default_ring_alloc(bnxt)) {
		bnxt_error(bnxt, "failed to enable default completion ring");
		goto cleanup;
	}
	bnxt->bnxt_attach_progress |= BNXT_ATTACH_DEF_RING;

	if (!bnxt_intrs_alloc(bnxt)) {
		bnxt_error(bnxt, "failed to allocate interrupts");
		goto cleanup;
	}
	bnxt->bnxt_attach_progress |= BNXT_ATTACH_ALLOC_INTRS;

	if (!bnxt_intr_handlers_init(bnxt)) {
		bnxt_error(bnxt, "failed to allocate interrupt handlers");
		goto cleanup;
	}
	bnxt->bnxt_attach_progress |= BNXT_ATTACH_INTR_HANDLERS;

	if (!bnxt_intrs_enable(bnxt)) {
		bnxt_error(bnxt, "failed to enable interrupts");
		goto cleanup;
	}
	bnxt->bnxt_attach_progress |= BNXT_ATTACH_ENABLE_INTRS;

	/*
	 * Initialize misc. properties before we register with mac.
	 */
	bnxt_mtu_init(bnxt);

	if (!bnxt_mac_register(bnxt)) {
		bnxt_error(bnxt, "failed to register with MAC");
		goto cleanup;
	}
	bnxt->bnxt_attach_progress |= BNXT_ATTACH_GLDV3;

	return (DDI_SUCCESS);

cleanup:
	bnxt_teardown(dip, bnxt);
	return (DDI_FAILURE);
}

static int
bnxt_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	bnxt_t *bnxt;

	if (cmd != DDI_DETACH)
		return (DDI_FAILURE);

	bnxt = ddi_get_driver_private(dip);
	if (bnxt == NULL) {
		dev_err(dip, CE_NOTE,
		    "bnxt_detach() called with no bnxt pointer");
		return (DDI_FAILURE);
	}

	bnxt_teardown(dip, bnxt);
	return (DDI_SUCCESS);
}

static struct cb_ops bnxt_cb_ops = {
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
	D_MP | D_HOTPLUG,	/* cb_flag */
	CB_REV,			/* cb_rev */
	nodev,			/* cb_aread */
	nodev			/* cb_awrite */
};

static struct dev_ops bnxt_dev_ops = {
	DEVO_REV,		/* devo_rev */
	0,			/* devo_refcnt */
	NULL,			/* devo_getinfo */
	nulldev,		/* devo_identify */
	nulldev,		/* devo_probe */
	bnxt_attach,		/* devo_attach */
	bnxt_detach,		/* devo_detach */
	nodev,			/* devo_reset */
	&bnxt_cb_ops,		/* devo_cb_ops */
	NULL,			/* devo_bus_ops */
	ddi_power,		/* devo_power */
	ddi_quiesce_not_supported /* devo_quiesce */
};

static struct modldrv bnxt_modldrv = {
	&mod_driverops,
	"Broadcom NetXtreme-C/E network driver",
	&bnxt_dev_ops
};

static struct modlinkage bnxt_modlinkage = {
	MODREV_1,
	&bnxt_modldrv,
	NULL
};

int
_init(void)
{
	int status;

	mac_init_ops(&bnxt_dev_ops, "bnxt");
	status = mod_install(&bnxt_modlinkage);
	if (status != DDI_SUCCESS) {
		mac_fini_ops(&bnxt_dev_ops);
	}
	return (status);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&bnxt_modlinkage, modinfop));
}

int
_fini(void)
{
	int status;

	status = mod_remove(&bnxt_modlinkage);
	if (status == DDI_SUCCESS) {
		mac_fini_ops(&bnxt_dev_ops);
	}
	return (status);
}
