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

#ifndef _BNXT_H
#define	_BNXT_H

/*
 * Describe the purpose of the file here.
 */

#include <sys/types.h>
#include <sys/errno.h>
#include <sys/modctl.h>
#include <sys/conf.h>
#include <sys/devops.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/mac_provider.h>
#include <sys/mac_ether.h>
#include <sys/param.h>
#include <sys/sysmacros.h>
#include <sys/pci.h>
#include <sys/vlan.h>
#include <sys/strsubr.h>

#include "hsi_struct_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Version of the driver.
 */
#define	BNXT_DRV_MAJOR	0
#define	BNXT_DRV_MINOR	1
#define	BNXT_DRV_UPD	0

/*
 * These values indicate the regs array entry for the BAR (PCI base address
 * registers) that we need to use for the different sets of registers. BAR 0
 * contains the main device registers while BAR 2 contains the doorbell
 * registers. The first REGS property always refers to the start of config space
 * and then the BARs follow in subsequent values.
 */
#define	BNXT_BAR_DEVICE		1
#define	BNXT_BAR_DOORBELL	3

/*
 * DMA alignment requirements for various structures. This comes from the HSI
 * specification.
 */
#define	BNXT_HWRM_DMA_ALIGN	16
#define	BNXT_RING_DMA_ALIGN	16

/*
 * DMA SGL lengths for different types of transfers. The various rings and other
 * data structures do not allow for more than one entry. However, when putting
 * in data for transmitting packets, we may use more SGL entries. This
 * restriction comes from the fact that firmware only allows for a single SGL
 * entry. However, in general, we're only ever allocating a page or two for
 * these.
 */
#define	BNXT_HWRM_DMA_SGLLEN	1
#define	BNXT_RING_DMA_SGLLEN	1

/*
 * The bnxt device driver supports a full 64-bit range for performing I/O.
 */
#define	BNXT_DMA_ADDR_LO	0x0
#define	BNXT_DMA_ADDR_HI	UINT64_MAX

/*
 * Default values for the burstsizes, count_max, and segment boundaries. Since
 * there seems to be no restrictions from a HW side, we try to set this to a
 * reasonable value.
 */
#define	BNXT_DMA_COUNT_MAX	UINT32_MAX
#define	BNXT_DMA_BURSTSIZES	0xfff
#define	BNXT_DMA_SEGMENT	UINT32_MAX

/*
 * Minimum and maximum transfer sizes. Since there isn't an obvious maximum
 * transfer size we set this to UINT32_MAX.
 */
#define	BNXT_DMA_MINXFER	1
#define	BNXT_DMA_MAXXFER	UINT32_MAX

/*
 * The DMA granularity for the bnxt driver is always one byte because the PCI
 * express bus is byte addressable.
 */
#define	BNXT_DMA_GRANULARITY	1

/*
 * Maximum Data Payload size
 */
#define	BNXT_MAX_MTU		9500
#define	BNXT_DEFAULT_MTU	1500

/*
 * We always assign the default ring a logical ID of zero.
 */
#define	BNXT_DEFAULT_RING_SW_ID	0

/*
 * Macros to synchronize DMA memory. These Because we never specify an offset or
 * a length, ddi_dma_sync is supposed to always return successfully. Thus we
 * VERIFY this on debug on ignore it on non-debug.
 */
#ifdef	DEBUG
#define	BNXT_DMA_SYNC(dma, flag)	VERIFY0(ddi_dma_sync( \
					    (dma).bdb_dma_handle, 0, 0, (flag)))
#else
#define	BNXT_DMA_SYNC(dma, flag)	((void) ddi_dma_sync( \
					    (dma).bdb_dma_handle, 0, 0, (flag)))
#endif

typedef enum bnxt_attach_state {
	BNXT_ATTACH_PCI_CONFIG		= 1 << 0, /* PCI config space setup */
	BNXT_ATTACH_REGS_MAP		= 1 << 1, /* BARs mapped */
	BNXT_ATTACH_HWRM_INIT		= 1 << 2, /* HWRM initialized */
	BNXT_ATTACH_DEF_RING		= 1 << 3, /* Default ring allocated */
	BNXT_ATTACH_ALLOC_INTRS		= 1 << 4, /* Interrupts alloced */
	BNXT_ATTACH_INTR_HANDLERS 	= 1 << 5, /* Added Interrupt Handlers */
	BNXT_ATTACH_ENABLE_INTRS 	= 1 << 6, /* Interrupts Enabled */
	BNXT_ATTACH_GLDV3		= 1 << 7, /* Register with the GLDv3 */
} bnxt_attach_state_t;

/*
 * This represents a single, logical DMA buffer. The DMA buffer may have a
 * single cookie or multiple. The first cookie, from which ddi_dma_next_cookie()
 * can be called is stored in bdb_cookie.
 */
typedef struct bnxt_dma_buffer {
	caddr_t			bdb_va;		/* Buffer VA */
	size_t			bdb_len;	/* Buffer logical len */
	ddi_acc_handle_t	bdb_acc_handle;	/* Access Handle */
	ddi_dma_handle_t	bdb_dma_handle;	/* DMA Handle */
	uint_t			bdb_ncookies;	/* Actual cookies */
	ddi_dma_cookie_t	bdb_cookie;
} bnxt_dma_buffer_t;

typedef enum bnxt_ring_flags {
	BNXT_RING_F_HW_ALLOCED		= 1 << 0,
	BNXT_RING_F_INTR_ENABLED 	= 1 << 1
} bnxt_ring_flags_t;

/*
 * This data structure represents a single logical ring that may be used for one
 * of many purposes.
 */
typedef struct bnxt_ring {
	bnxt_dma_buffer_t	br_dma;		/* DMA buffer for ring */
	uint16_t		br_sw_ring_id;	/* ID assigned by SW */
	uint16_t		br_hw_ring_id;	/* ID from HW */
	uint32_t		br_nentries;	/* Number of entries in the ring */
	size_t			br_rsize;	/* Size in bytes of ring */
	bnxt_ring_flags_t	br_flags;
	uint8_t			br_type;	/* Assigned in ring alloc */
} bnxt_ring_t;

/*
 * This is used to represent a completion ring in hardware.
 */
typedef struct bnxt_comp_ring {
	bnxt_ring_t		bcr_ring;
	uint8_t			bcr_cycle;
	uint32_t		bcr_tail;
	uint32_t		bcr_hw_stat_id;
} bnxt_comp_ring_t;

typedef enum bnxt_flags {
	BNXT_F_DOUBLE_DOORBELL	= (1 << 0)	/* Two doorbell writes req'd */
} bnxt_flags_t;

typedef struct bnxt {
	dev_info_t		*bnxt_dip;
	bnxt_attach_state_t	bnxt_attach_progress;
	bnxt_flags_t		bnxt_flags;

	/*
	 * PCI and register access
	 */
	ddi_acc_handle_t	bnxt_pci_hdl;
	ddi_acc_handle_t	bnxt_dev_hdl;
	ddi_acc_handle_t	bnxt_doorbell_hdl;
	caddr_t			bnxt_dev_base;
	caddr_t			bnxt_doorbell_base;

	/*
	 * Interrupt data
	 */
	int			bnxt_nintrs_req;
	int			bnxt_nintrs;
	int			bnxt_intr_type;
	ddi_intr_handle_t	*bnxt_intr_handles;
	uint_t			bnxt_intr_pri;
	int			bnxt_intr_caps;

	/*
	 * GLDv3 data
	 */
	mac_handle_t		bnxt_mac_handle;

	/*
	 * MTU data
	 */
	uint16_t		bnxt_mtu;

	/*
	 * HWRM resources
	 */
	kmutex_t		bnxt_hwrm_lock;
	bnxt_dma_buffer_t	bnxt_hwrm_reply;
	uint16_t		bnxt_hwrm_seqid;
	uint16_t		bnxt_hwrm_timeout;
	uint16_t		bnxt_hwrm_max_req;

	/*
	 * Default device MAC
	 */
	uint8_t			bnxt_macaddr[ETHERADDRL];

	/*
	 * Device, Firmware, and NVM information
	 */
	struct hwrm_ver_get_output	bnxt_ver;
	struct hwrm_nvm_get_dev_info_output bnxt_nvm;
	uint16_t		bnxt_fid;
	uint16_t		bnxt_port_id;
	uint32_t		bnxt_qcap_flags;
	uint16_t		bnxt_max_rsscos_ctx;
	uint16_t		bnxt_max_cmpl_rings;
	uint16_t		bnxt_max_tx_rings;
	uint16_t		bnxt_max_rx_rings;
	uint16_t		bnxt_max_l2_ctxs;
	uint16_t		bnxt_max_vnics;
	uint32_t		bnxt_max_stat_ctx;
	uint32_t		bnxt_max_rx_em_flows;
	uint32_t		bnxt_max_rx_wm_flows;
	uint32_t		bnxt_max_mcast_filters;
	uint32_t		bnxt_max_flow_id;
	uint32_t		bnxt_max_hw_ring_grps;

	/*
	 * Current allocations of resources and configuration.
	 */
	uint16_t		bnxt_alloc_cmpl_rings;
	uint16_t		bnxt_alloc_tx_rings;
	uint16_t		bnxt_alloc_rx_rings;
	uint16_t		bnxt_alloc_vnics;
	uint32_t		bnxt_alloc_mcast_filters;
	uint32_t		bnxt_alloc_hw_ring_grps;
	struct hwrm_queue_qportcfg_output bnxt_qportcfg;

	/*
	 * Completion rings
	 */
	uint_t			bnxt_ncomp_rings;
	bnxt_comp_ring_t	*bnxt_comp_rings;
} bnxt_t;

/*
 * Logging Functions
 */
extern void bnxt_log(bnxt_t *, const char *, ...);
extern void bnxt_error(bnxt_t *, const char *, ...);

/*
 * DMA Functions
 */
extern boolean_t bnxt_dma_alloc(bnxt_t *, bnxt_dma_buffer_t *,
    ddi_dma_attr_t *, ddi_device_acc_attr_t *, boolean_t, size_t,
    boolean_t);
extern void bnxt_dma_free(bnxt_dma_buffer_t *);

/*
 * HWRM Functions
 */
extern boolean_t bnxt_hwrm_init(bnxt_t *);
extern void bnxt_hwrm_fini(bnxt_t *);

extern boolean_t bnxt_hwrm_version_get(bnxt_t *);
extern boolean_t bnxt_hwrm_nvm_info_get(bnxt_t *);
extern boolean_t bnxt_hwrm_func_reset(bnxt_t *);
extern boolean_t bnxt_hwrm_host_register(bnxt_t *);
extern boolean_t bnxt_hwrm_register_events(bnxt_t *);
extern boolean_t bnxt_hwrm_host_unregister(bnxt_t *);
extern boolean_t bnxt_hwrm_func_qcaps(bnxt_t *);
extern boolean_t bnxt_hwrm_func_qcfg(bnxt_t *);
extern boolean_t bnxt_hwrm_queue_qportcfg(bnxt_t *);

extern boolean_t bnxt_hwrm_ring_free(bnxt_t *, bnxt_ring_t *);
extern boolean_t bnxt_hwrm_ring_alloc(bnxt_t *, bnxt_ring_t *, uint8_t,
    uint16_t, uint32_t);

/*
 * Interrupt functions
 */
extern uint_t bnxt_intr_msix(caddr_t, caddr_t);
extern uint_t bnxt_intr_intx(caddr_t, caddr_t);

/*
 * MAC functions
 */
extern void bnxt_mac_unregister(bnxt_t *);
extern boolean_t bnxt_mac_register(bnxt_t *);

/*
 * Ring related functions
 */
extern void bnxt_comp_ring_reset(bnxt_comp_ring_t *);

#ifdef __cplusplus
}
#endif

#endif /* _BNXT_H */
