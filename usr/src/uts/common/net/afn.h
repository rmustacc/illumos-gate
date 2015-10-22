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
 * Copyright 2015 Joyent, Inc.
 */

#ifndef _NET_AFN_H
#define	_NET_AFN_H

/*
 * This file provides definitions for the Address Family Numbers that IANA has
 * assigned. The reference for all of these is:
 *
 * http://www.iana.org/assignments/address-family-numbers/
 *
 * Note, at this time we only include the standardized ones.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define	IANA_AFN_IP	1	/* IP version 4 */
#define	IANA_AFN_IPV6	2	/* IP version 6 */
#define	IANA_AFN_NSAP	3
#define	IANA_AFN_HDLC	4	/* (8-bit multidrop) */
#define	IANA_AFN_BBN_1822	5
#define	IANA_AFN_802	6	/* (includes all 802 media plus Ethernet */
				/* "canonical format") */
#define	IANA_AFN_E163	7	/* E.163 */
#define	IANA_AFN_E164	8	/* E.164 (SMDS, Frame Relay, ATM) */
#define	IANA_AFN_F69	9	/* F.69 (Telex) */
#define	IANA_AFN_X121	10	/* X.121 (X.25, Frame Relay) */
#define	IANA_AFN_IPX	11
#define	IANA_AFN_APPLETALK	12
#define	IANA_AFN_DECNET_IV	13
#define	IANA_AFN_BANYAN_VINES	14
#define	IANA_AFN_E164_NSAP	15	/* E.164 with NSAP format subaddress */
#define	IANA_AFN_DNS		16	/* DNS (Domain Name System) */
#define	IANA_AFN_DN		17	/* Distinguished Name */
#define	IANA_AFN_AS		18	/* AS number */
#define	IANA_AFN_XTP_IPV4	19	/* XTP over IP version 4 */
#define	IANA_AFN_XTP_IPv6	20	/* XTP over IP version 6 */
#define	IANA_AFN_XTP		21	/* XTP native mode XTP */
#define	IANA_AFN_FC_WWPN	22	/* Fibre Channel World-Wide Port Name */
#define	IANA_AFN_FC_WWNN	23	/* Fibre Channel World-Wide Node Name */
#define	IANA_AFN_GWID		24
#define	IANA_AFN_AFI_L2VPN	25	/* AFI for L2VPN information */
#define	IANA_AFN_MPLS_TP_SEI	26	/* MPLS-TP Section Endpoint */
					/* Identifier */
#define	IANA_AFN_MPLS_TP_LSPEI	27	/* MPLS-TP LSP Endpoint Identifier */
#define	IANA_AFN_MPLS_TP_PEI	28	/* MPLS-TP Pseudowire Endpoint */
					/* Identifier */
#define	IANA_AFN_MT_IP		29	/* MT IP: Multi-Topology IP version 4 */
#define	IANA_AFN_MT_IPv6	30	/* MT IPv6: Multi-Topology IP version */
					/* 6 */

#ifdef __cplusplus
}
#endif

#endif /* _NET_AFN_H */
