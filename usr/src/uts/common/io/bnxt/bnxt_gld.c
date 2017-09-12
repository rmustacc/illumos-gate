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
 * GLDv3 entry points
 */

#include "bnxt.h"

int
bnxt_m_stat(void *arg, uint_t stat, uint64_t *val)
{
	return (ENOTSUP);
}

static void
bnxt_m_stop(void *arg)
{
	/*
	 * XXX When tearing down we need to make sure that all rings have not
	 * had traffic on them for more than 500ms before we call the
	 * hwrm_ring_free_input() functions. See the comments above
	 * hwrm_ring_free_input() for more information.
	 */
}

/*
 * Logically start the chip so we can use this. At this point, the only thing we
 * have done is allocate and initialize interrupts and the hwrm resources. We
 * also know the total number of rings that we have.
 *
 * XXX Remember that completion rings are basically mapped to interrupts and
 * doorbells. This is important. This means that when we create rx / tx rings
 * here, if they're mapped to the same thing, then we're going to end up in a
 * very confusing world as the blanking of interrupts applies to the doorbell
 * itself, not the queue. Now, in theory, we could have larger completion rings
 * to make up for this and process these, even when shared.
 */
static int
bnxt_m_start(void *arg)
{
	bnxt_t *bnxt = arg;

	bnxt_comp_ring_reset(&bnxt->bnxt_default_ring);
	return (EIO);
}

static int
bnxt_m_setpromisc(void *arg, boolean_t on)
{
	return (EIO);
}

static int
bnxt_m_multicast(void *arg, boolean_t add, const uint8_t *multicast_address)
{
	return (EIO);
}

#ifndef XXX_RINGS
static int
bnxt_m_unicast(void *arg, const uint8_t *mac)
{
	return (EIO);
}

static mblk_t *
bnxt_m_tx(void *arg, mblk_t *chain)
{
	freemsgchain(chain);
	return (NULL);
}
#endif

static boolean_t
bnxt_m_getcapab(void *arg, mac_capab_t cap, void *cap_data)
{
#if 0
	mac_capab_rings_t *cap_rings;

	switch (cap) {
	case MAC_CAPAB_RINGS:
		cap_rings = cap_data;

		switch (cap->rings_mr_type) {
		case MAC_RING_TYPE_TX:
			cap_rings->mr_gnum = 0;
			cap_rings->rm_rnum = 1;
			cap_rings->mr_rget = bnxt_fill_tx_ring;
			cap_rings->mr_gget = NULL;
			cap_rings->mr_gaddring = NULL;
			cap_rings->mr_grem_ring = NULL;
			break;
		case MAC_RING_TYPE_RX:
			cap_rings->mr_gnum = 1;
			cap_rings->rm_rnum = 1;
			cap_rings->mr_rget = bnxt_fill_rx_ring;
			cap_rings->mr_gget = bnxt_fill_rx_group;
			cap_rings->mr_gaddring = NULL;
			cap_rings->mr_grem_ring = NULL;
			break;
		default:
			return (B_FALSE);
		}
		break;
	default:
		return (B_FALSE);
	}
	return (B_TRUE);
#endif
	return (B_FALSE);
}

static int
bnxt_m_setprop(void *arg, const char *pr_name, mac_prop_id_t pr_num,
    uint_t pr_valsize, const void *pr_val)
{
	return (ENOTSUP);
}

static int
bnxt_m_getprop(void *arg, const char *pr_name, mac_prop_id_t pr_num,
    uint_t pr_valsize, void *pr_val)
{
	return (ENOTSUP);
}

static void
bnxt_m_propinfo(void *arg, const char *pr_name, mac_prop_id_t pr_num,
    mac_prop_info_handle_t prh)
{

}

#define	BNXT_M_CALLBACK_FLAGS \
    (MC_GETCAPAB | MC_SETPROP | MC_GETPROP | MC_PROPINFO)

/*
 * XXX Make this static
 */
mac_callbacks_t bnxt_m_callbacks = {
	.mc_callbacks = BNXT_M_CALLBACK_FLAGS,
	.mc_getstat = bnxt_m_stat,
	.mc_start = bnxt_m_start,
	.mc_stop = bnxt_m_stop,
	.mc_setpromisc = bnxt_m_setpromisc,
	.mc_multicst = bnxt_m_multicast,
	.mc_getcapab = bnxt_m_getcapab,
	.mc_setprop = bnxt_m_setprop,
	.mc_getprop = bnxt_m_getprop,
#ifndef XXX_RINGS
	.mc_tx = bnxt_m_tx,
	.mc_unicst = bnxt_m_unicast,
#endif
	.mc_propinfo = bnxt_m_propinfo
};

void
bnxt_mac_unregister(bnxt_t *bnxt)
{
	int ret;

	/*
	 * Because we only register and unregister from MAC in the context of
	 * detach, it shouldn't be possible for us to detach and fail as MAC
	 * should have already released everything related to us before
	 * detach(9E) can be called.
	 */
	if ((ret = mac_unregister(bnxt->bnxt_mac_handle)) != 0) {
		bnxt_error(bnxt, "failed to unregister from MAC: %d",
		    ret);
	}
}

boolean_t
bnxt_mac_register(bnxt_t *bnxt)
{
	int ret;
	mac_register_t *mac;

	mac = mac_alloc(MAC_VERSION);
	if (mac == NULL) {
		bnxt_error(bnxt, "failed to alloc MAC handle");
		return (B_FALSE);
	}

	mac->m_type_ident = MAC_PLUGIN_IDENT_ETHER;
	mac->m_driver = bnxt;
	mac->m_dip = bnxt->bnxt_dip;
	mac->m_src_addr = bnxt->bnxt_macaddr;
	mac->m_dst_addr = NULL;
	mac->m_callbacks = &bnxt_m_callbacks;
	mac->m_min_sdu = 0;
	mac->m_max_sdu = bnxt->bnxt_mtu;
	mac->m_pdata = NULL;
	mac->m_pdata_size = 0;
	mac->m_priv_props = NULL;
	mac->m_margin = VLAN_TAGSZ;
#if 0
	mac->m_v12n = MAC_VIRT_LEVEL1;
#endif

	ret = mac_register(mac, &bnxt->bnxt_mac_handle);
	if (ret != 0) {
		bnxt_error(bnxt, "mac_register failed %d", ret);
	}

	mac_free(mac);
	return (ret == 0);
}
