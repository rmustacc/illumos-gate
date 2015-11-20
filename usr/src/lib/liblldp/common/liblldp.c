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

/*
 * LLDP routines to parse an LLDPDU into an nvlist
 */

#include <sys/types.h>
#include <netinet/in.h>
#include <inttypes.h>
#include <strings.h>
#include <sys/debug.h>
#include <sys/ethernet.h>
#include <net/afn.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <libcmdutils.h>
#include <errno.h>
#include <sys/sysmacros.h>

#include <liblldp.h>
#include <lldp_provider_impl.h>

typedef enum lldp_tlv_type {
	LLDP_TLV_T_EOL		= 0,
	LLDP_TLV_T_CHASSIS	= 1,
	LLDP_TLV_T_PORT_ID	= 2,
	LLDP_TLV_T_TTL		= 3,
	LLDP_TLV_T_PORT_DESC	= 4,
	LLDP_TLV_T_SYS_NAME	= 5,
	LLDP_TLV_T_SYS_DESC	= 6,
	LLDP_TLV_T_SYS_CAPS	= 7,
	LLDP_TLV_T_MGMT_ADDR	= 8,
	LLDP_TLV_T_OST		= 127
} lldp_tlv_type_t;

typedef enum lldp_chassis_type {
	LLDP_CHASSIS_COMPONENT	= 1,
	LLDP_CHASSIS_IFALIAS	= 2,
	LLDP_CHASSIS_PORT	= 3,
	LLDP_CHASSIS_MAC	= 4,
	LLDP_CHASSIS_NET	= 5,
	LLDP_CHASSIS_IFNAME	= 6,
	LLDP_CHASSIS_LOCAL	= 7
} lldp_chassis_type_t;

typedef enum lldp_port_type {
	LLDP_PORT_IFALIAS	= 1,
	LLDP_PORT_COMPONENT	= 2,
	LLDP_PORT_MAC		= 3,
	LLDP_PORT_NET		= 4,
	LLDP_PORT_IFNAME	= 5,
	LLDP_PORT_CIRCUIT	= 6,
	LLDP_PORT_LOCAL		= 7
} lldp_port_type_t;

#define	LLDP_CHASSIS_LEN_MIN	2
#define	LLDP_CHASSIS_LEN_MAX	255

#define	LLDP_PORTID_LEN_MIN	2
#define	LLDP_PORTID_LEN_MAX	255

#define	LLDP_TTL_LEN_MIN	2

#define	LLDP_TLVSTR_LEN_MIN	0
#define	LLDP_TLVSTR_LEN_MAX	255

#define	LLDP_SYSDESC_LEN	4

#define	LLDP_MGMT_LEN_MIN	9
#define	LLDP_MGMT_LEN_MAX	167
#define	LLDP_MGMT_ADDR_MIN	2
#define	LLDP_MGMT_ADDR_MAX	32
#define	LLDP_MGMT_OID_MIN	0
#define	LLDP_MGMT_OID_MAX	128

#define	LLDP_OST_LEN_MIN	4
#define	LLDP_OST_LEN_MAX	511

#define	LLDP_TLV_TYPE_SHIFT	9
#define	LLDP_TLV_LEN_MASK	0x01ff

/*
 * This is the largest buffer we support with the parse_iana_string code.
 */
#define	LLDP_IANA_STRLEN	INET6_ADDRSTRLEN

typedef struct lldp_tlv_hdr {
	uint16_t	lth_type: 7;
	uint16_t	lth_len: 9;
} lldp_tlv_hdr_t;

typedef struct lldp_rx_stat {
	uint64_t	lrxs_framediscard;
	uint64_t	lrxs_frameerrors;
	uint64_t	lrxs_tlvdiscard;
} lldp_rx_stat_t;

typedef struct lldp_parse {
	const void	*lp_buf;
	int		lp_rem;
	nvlist_t	*lp_nvl;
	lldp_tlv_hdr_t	lp_tlv;
	lldp_rx_stat_t	lp_stat;
} lldp_parse_t;

static void
lldp_parse_advance(lldp_parse_t *lp, int len)
{
	VERIFY3S(len, >=, 0);
	lp->lp_buf += len;
	lp->lp_rem -= len;
}

static void
lldp_parse_string(lldp_parse_t *lp, size_t strlen, char *buf,
    size_t buflen)
{
	VERIFY3U(strlen, <, buflen);
	bcopy(lp->lp_buf, buf, strlen);
	buf[strlen] = '\0';
}

/*
 * Unfortunately this string can be arbitrarily large, therefore we actually
 * create a buffer for its use as we write into it.
 */
static int
lldp_parse_circuit_string(lldp_parse_t *lp, int strlen, custr_t **bufp)
{
	int i;
	const char *buf = lp->lp_buf;
	custr_t *str;

	if (custr_alloc(&str) == -1)
		return (errno);

	for (i = 0; i < strlen; i++) {
		if (custr_append_printf(str, "%02x", buf[i]) == -1) {
			int e = errno;
			custr_free(str);
			return (e);
		}
	}

	return (0);
}

static int
lldp_parse_iana_string(const void *datap, int strlen, char *buf, size_t buflen)
{
	const uint8_t *fp = datap;
	uint8_t afn;
	int af;

	VERIFY3U(strlen, >, 1);
	VERIFY3U(buflen, >=, LLDP_IANA_STRLEN);
	afn = *fp;
	fp++;

	/*
	 * While there are many IANA address families registered, the only ones
	 * we actually care about are IPv4 and IPv6. If there are others we
	 * should care about, we'll want to see them in the wild.
	 */
	switch (afn) {
	case IANA_AFN_IP:
		if (strlen != sizeof (struct in_addr) + 1)
			return (EINVAL);
		VERIFY(inet_ntop(AF_INET, fp, buf, buflen) != NULL);
		break;
	case IANA_AFN_IPV6:
		if (strlen != sizeof (struct in6_addr) + 1)
			return (EINVAL);
		VERIFY(inet_ntop(AF_INET6, fp, buf, buflen) != NULL);
		break;
	case IANA_AFN_802:
		if (strlen != ETHERADDRL)
			return (EINVAL);
		(void) ether_ntoa_r((const void *)fp, buf);
		break;
	default:
		return (EINVAL);
	}

	return (0);
}

static int
lldp_parse_tlv(lldp_parse_t *lp)
{
	uint16_t tlv;
	const uint16_t *tlvp = lp->lp_buf;

	if (lp->lp_rem < sizeof (uint16_t)) {
		lp->lp_stat.lrxs_tlvdiscard++;
		lp->lp_stat.lrxs_frameerrors++;
		return (ERANGE);
	}

	tlv = ntohs(*tlvp);
	lp->lp_tlv.lth_type = tlv >> LLDP_TLV_TYPE_SHIFT;
	lp->lp_tlv.lth_len = tlv & LLDP_TLV_LEN_MASK;
	lldp_parse_advance(lp, sizeof (uint16_t));

	return (0);
}

static int
lldp_parse_tlv_chassis(lldp_parse_t *lp)
{
	int ret;
	lldp_chassis_type_t ct;
	const uint8_t *stp = lp->lp_buf;
	char buf[LLDP_CHASSIS_LEN_MAX];
	uchar_t raw[LLDP_CHASSIS_LEN_MAX];
	nvlist_t *nvl;

	if (lp->lp_tlv.lth_len < LLDP_CHASSIS_LEN_MIN ||
	    lp->lp_tlv.lth_len > LLDP_CHASSIS_LEN_MAX) {
		LIBLLDP_DISCARD("invalid length for chassis");
		lp->lp_stat.lrxs_framediscard++;
		lp->lp_stat.lrxs_frameerrors++;
		return (EINVAL);
	}

	ct = *stp;
	lldp_parse_advance(lp, 1);

	bcopy(lp->lp_buf, raw, lp->lp_tlv.lth_len - 1);

	switch (ct) {
	case LLDP_CHASSIS_COMPONENT:
	case LLDP_CHASSIS_IFALIAS:
	case LLDP_CHASSIS_PORT:
	case LLDP_CHASSIS_IFNAME:
	case LLDP_CHASSIS_LOCAL:
		/*
		 * These are all just to be interpretted as strings. We have no
		 * idea if they're null terminated or not. The maximum size is
		 * LLDP_CHASSIS_LEN_MAX - 1.
		 */
		lldp_parse_string(lp, lp->lp_tlv.lth_len - 1, buf,
		    sizeof (buf));
		break;
	case LLDP_CHASSIS_MAC:
		VERIFY3U(sizeof (buf), >=, ETHERADDRSTRL);
		if (lp->lp_tlv.lth_len - 1 != ETHERADDRL) {
			LIBLLDP_DISCARD("chassis type was mac, but length "
			    "was not ETHERADDRL");
			lp->lp_stat.lrxs_framediscard++;
			lp->lp_stat.lrxs_frameerrors++;
			return (EINVAL);
		}
		(void) ether_ntoa_r(lp->lp_buf, buf);
		break;
	case LLDP_CHASSIS_NET:
		if ((ret = lldp_parse_iana_string(lp->lp_buf, lp->lp_tlv.lth_len - 1,
		    buf, sizeof (buf))) != 0) {
			LIBLLDP_DISCARD("chassis type was netaddr, but it was "
			    "invalid");
			lp->lp_stat.lrxs_framediscard++;
			lp->lp_stat.lrxs_frameerrors++;
			return (EINVAL);
		}
		break;
	default:
		LIBLLDP_DISCARD("invalid chassis subtype");
		lp->lp_stat.lrxs_framediscard++;
		lp->lp_stat.lrxs_frameerrors++;
		return (EINVAL);
	}

	if ((ret = nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0)) != 0)
		return (ret);

	if ((ret = nvlist_add_uint8(nvl, "subtype", ct)) != 0 ||
	    (ret = nvlist_add_string(nvl, "id", buf)) != 0 ||
	    (ret = nvlist_add_byte_array(nvl, "raw", raw, 
	    lp->lp_tlv.lth_len - 1)) != 0 ||
	    (ret = nvlist_add_nvlist(lp->lp_nvl, "chassis", nvl)) != 0) {
		VERIFY3U(ret, ==, ENOMEM);
		nvlist_free(nvl);
		return (ret);
	}

	nvlist_free(nvl);
	lldp_parse_advance(lp, lp->lp_tlv.lth_len - 1);
	return (0);
}

static int
lldp_parse_tlv_portid(lldp_parse_t *lp)
{
	int ret;
	lldp_port_type_t pt;
	const uint8_t *ptp = lp->lp_buf;
	char buf[LLDP_PORTID_LEN_MAX];
	uchar_t raw[LLDP_CHASSIS_LEN_MAX];
	const char *port;
	nvlist_t *nvl;
	custr_t *str = NULL;

	if (lp->lp_tlv.lth_len < LLDP_PORTID_LEN_MIN ||
	    lp->lp_tlv.lth_len > LLDP_PORTID_LEN_MAX) {
		LIBLLDP_DISCARD("invalid length for portid");
		lp->lp_stat.lrxs_framediscard++;
		lp->lp_stat.lrxs_frameerrors++;
		return (EINVAL);
	}

	pt = *ptp;
	lldp_parse_advance(lp, 1);

	bcopy(lp->lp_buf, raw, lp->lp_tlv.lth_len - 1);

	switch (pt) {
	case LLDP_PORT_COMPONENT:
	case LLDP_PORT_IFALIAS:
	case LLDP_PORT_IFNAME:
	case LLDP_PORT_LOCAL:
		/*
		 * These are all just to be interpretted as strings. We have no
		 * idea if they're null terminated or not. The maximum size is
		 * LLDP_PORT_LEN_MAX - 1.
		 */
		lldp_parse_string(lp, lp->lp_tlv.lth_len - 1, buf,
		    sizeof (buf));
		port = buf;
		break;
	case LLDP_PORT_CIRCUIT:
		/*
		 * This only fails due to internal memory errors, not other
		 * reasons. There's no counters to bump as a result.
		 */
		if ((ret = lldp_parse_circuit_string(lp,
		    lp->lp_tlv.lth_len - 1, &str)) != 0) {
			VERIFY(ret == EAGAIN || ret == ENOMEM);
			return (ret);
		}
		port = custr_cstr(str);
		break;
	case LLDP_PORT_NET:
		if ((ret = lldp_parse_iana_string(lp->lp_buf, lp->lp_tlv.lth_len - 1,
		    buf, sizeof (buf))) != 0) {
			LIBLLDP_DISCARD("port type was netaddr, but it was "
			    "invalid");
			lp->lp_stat.lrxs_framediscard++;
			lp->lp_stat.lrxs_frameerrors++;
			return (EINVAL);
		}
		port = buf;
		break;
	case LLDP_PORT_MAC:
		VERIFY3U(sizeof (buf), >=, ETHERADDRSTRL);
		if (lp->lp_tlv.lth_len - 1 != ETHERADDRL) {
			LIBLLDP_DISCARD("port type was mac, but length "
			    "was not ETHERADDRL");
			lp->lp_stat.lrxs_framediscard++;
			lp->lp_stat.lrxs_frameerrors++;
			return (EINVAL);
		}
		(void) ether_ntoa_r(lp->lp_buf, buf);
		port = buf;
		break;
	default:
		LIBLLDP_DISCARD("invalid port subtype");
		lp->lp_stat.lrxs_framediscard++;
		lp->lp_stat.lrxs_frameerrors++;
		return (EINVAL);
	}

	if ((ret = nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0)) != 0) {
		if (str != NULL)
			custr_free(str);
		return (ret);
	}

	if ((ret = nvlist_add_uint8(nvl, "subtype", pt)) != 0 ||
	    (ret = nvlist_add_string(nvl, "id", buf)) != 0 ||
	    (ret = nvlist_add_byte_array(nvl, "raw", raw, 
	    lp->lp_tlv.lth_len - 1)) != 0 ||
	    (ret = nvlist_add_nvlist(lp->lp_nvl, "portid", nvl)) != 0) {
		VERIFY3U(ret, ==, ENOMEM);
		if (str != NULL)
			custr_free(str);
		nvlist_free(nvl);
		return (ret);
	}

	if (str != NULL)
		custr_free(str);
	nvlist_free(nvl);
	lldp_parse_advance(lp, lp->lp_tlv.lth_len - 1);

	return (0);
}

static int
lldp_parse_tlv_ttl(lldp_parse_t *lp)
{
	int ret;
	const uint16_t *ttlp = lp->lp_buf;
	uint16_t ttl;

	if (lp->lp_tlv.lth_len < LLDP_TTL_LEN_MIN) {
		LIBLLDP_DISCARD("invalid length for ttl");
		lp->lp_stat.lrxs_framediscard++;
		lp->lp_stat.lrxs_frameerrors++;
		return (EINVAL);
	}

	ttl = ntohs(*ttlp);
	if ((ret = nvlist_add_uint16(lp->lp_nvl, "ttl", ttl)) != 0) {
		VERIFY3U(ret, ==, ENOMEM);
		return (ret);
	}

	/*
	 * Unlike the other TLVs, the TTL is allowed to have more bytes defined
	 * than is consumed. In this case we should go through and actually
	 * increment it by the total amount. See 9.2.7.7.1 c).
	 */
	lldp_parse_advance(lp, lp->lp_tlv.lth_len);

	return (0);
}

/*
 * This handles the general case of a TLV which is really just a string which is
 * the case for LLDP_TLV_T_PORT_DESC and LLDP_TLV_T_SYS_NAME. They may be
 * between 
 */
static int
lldp_parse_tlv_string(lldp_parse_t *lp, const char *key)
{
	int ret;
	char buf[LLDP_PORTID_LEN_MAX + 1];

	/*
	 * Note that according to 9.2.7.7.2 d), if the length is outside the
	 * range for this entry then we should skip it and 'trust' the length
	 * specifier for the next entry.
	 */
	if (lp->lp_tlv.lth_len < LLDP_PORTID_LEN_MIN ||
	    lp->lp_tlv.lth_len > LLDP_PORTID_LEN_MAX) {
		LIBLLDP_DISCARD("invalid length for tlv string");
		lp->lp_stat.lrxs_tlvdiscard++;
		lp->lp_stat.lrxs_frameerrors++;
		goto done;
	}

	lldp_parse_string(lp, lp->lp_tlv.lth_len, buf, sizeof (buf));
	if ((ret = nvlist_add_string(lp->lp_nvl, key, buf)) != 0) {
		VERIFY3U(ret, ==, ENOMEM);
		return (ret);
	}

done:
	lldp_parse_advance(lp, lp->lp_tlv.lth_len);
	return (0);
}


static int
lldp_parse_tlv_sysdesc(lldp_parse_t *lp)
{
	int ret;
	const uint16_t *capp = lp->lp_buf;
	uint16_t caps, enab;
	nvlist_t *nvl;

	if (lp->lp_tlv.lth_len != LLDP_SYSDESC_LEN) {
		LIBLLDP_DISCARD("invalid length for sys caps");
		lp->lp_stat.lrxs_tlvdiscard++;
		lp->lp_stat.lrxs_frameerrors++;
		goto done;
	}

	caps = ntohs(capp[0]);
	enab = ntohs(capp[1]);

	if ((ret = nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0)) != 0) {
		VERIFY3U(ret, ==, ENOMEM);
		return (ret);
	}
	if ((ret = nvlist_add_uint16(nvl, "capabilities", caps)) != 0 ||
	    (ret = nvlist_add_uint16(nvl, "enabled", enab)) != 0 ||
	    (ret = nvlist_add_nvlist(lp->lp_nvl, "syscap", nvl)) != 0) {
		VERIFY3U(ret, ==, ENOMEM);
		nvlist_free(nvl);
		return (ret);
	}
	nvlist_free(nvl);

done:
	lldp_parse_advance(lp, lp->lp_tlv.lth_len);
	return (0);
}

static int
lldp_parse_tlv_mgmtaddr(lldp_parse_t *lp)
{
	int ret;
	nvlist_t *nvl;
	const uint8_t *bufp = lp->lp_buf;
	const uint8_t *mgmtstr, *oidstr;
	const uint32_t *ifnump;
	uint8_t mgmtstrlen, oidstrlen, ifn;
	uint32_t ifnum;
	uchar_t mgmtraw[LLDP_MGMT_ADDR_MAX], oidraw[LLDP_MGMT_OID_MAX];
	char mgmt[LLDP_IANA_STRLEN];
	nvlist_t *nvp = NULL;

	if (lp->lp_tlv.lth_len < LLDP_MGMT_LEN_MIN ||
	    lp->lp_tlv.lth_len > LLDP_MGMT_LEN_MAX) {
		LIBLLDP_DISCARD("invalid length for mgmt tlv");
		lp->lp_stat.lrxs_tlvdiscard++;
		lp->lp_stat.lrxs_frameerrors++;
		goto done;
	}

	mgmtstrlen = *bufp;
	if (mgmtstrlen < LLDP_MGMT_ADDR_MIN ||
	    mgmtstrlen > LLDP_MGMT_ADDR_MAX) {
		LIBLLDP_DISCARD("invalid length for mgmt addr");
		lp->lp_stat.lrxs_tlvdiscard++;
		lp->lp_stat.lrxs_frameerrors++;
		goto done;
	}

	bufp++;
	mgmtstr = bufp;

	/*
	 * At this point, make sure taht the string legnth of the management
	 * string still gives us enough space for the rest of the buffer before
	 * we try to parse the next pieces. Here we account for the management
	 * address length (1 byte), the interface numbering type (1 byte), the
	 * interface number (4 bytes) and the OID string length (1 byte), for a
	 * total of 7 bytes in addition to the management string length (which
	 * includes the management string subtype).
	 */
	if (7 + mgmtstrlen > lp->lp_tlv.lth_len) {
		LIBLLDP_DISCARD("violated length constraints");
		lp->lp_stat.lrxs_tlvdiscard++;
		lp->lp_stat.lrxs_frameerrors++;
		goto done;
	}

	bufp += mgmtstrlen;
	ifn = *bufp;
	bufp++;

	ifnump = (const uint32_t *)bufp;
	ifnum = ntohl(*ifnump);
	bufp += 4;
	oidstrlen = *bufp;
	bufp++;

	oidstr = bufp;

	/*
	 * Now, let's make sure the oid string fits within our buffer. If it
	 * does, then let's go ahead and start filling in an nvlist_t.
	 */
	if (7 + mgmtstrlen + oidstrlen > lp->lp_tlv.lth_len) {
		LIBLLDP_DISCARD("violated length constraints");
		lp->lp_stat.lrxs_tlvdiscard++;
		lp->lp_stat.lrxs_frameerrors++;
		goto done;
	}

	/*
	 * Now the only thing that can stop us is ENOMEM.
	 */
	bcopy(mgmtstr, mgmtraw, mgmtstrlen);

	if ((ret = nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0)) != 0)
		return (ret);

	if (lldp_parse_iana_string(mgmtstr, mgmtstrlen, mgmt,
	    sizeof (mgmt)) == 0) {
		if ((ret = nvlist_add_string(nvl, "address", mgmt)) != 0) {
			VERIFY(ret == ENOMEM);
			nvlist_free(nvl);
			return (ret);
		}
	}

	if (oidstrlen != 0) {
		bcopy(oidstr, oidraw, oidstrlen);
		if ((ret = nvlist_add_byte_array(nvl, "oid", oidraw, oidstrlen)) != 0) {
			VERIFY(ret == ENOMEM);
			nvlist_free(nvl);
			return (ret);
		}
	}

	if ((ret = nvlist_add_byte_array(nvl, "raw", mgmtraw,
	    mgmtstrlen)) != 0 ||
	    (ret = nvlist_add_uint8(nvl, "ifnumtype", ifn)) != 0 ||
	    (ret = nvlist_add_uint32(nvl, "ifnum", ifnum)) != 0 ||
	    (ret = nvlist_add_nvlist(lp->lp_nvl, "mgmtaddr", nvl)) != 0) {
		VERIFY(ret == ENOMEM);
		nvlist_free(nvl);
		return (ret);
	}

done:
	lldp_parse_advance(lp, lp->lp_tlv.lth_len);
	return (0);
}

static int
lldp_parse_tlv_ost(lldp_parse_t *lp)
{
	int ret;
	const uint8_t *dp = lp->lp_buf;
	uint8_t oui[3], subtype;
	char ouistr[ETHERADDRSTRL], substr[32];
	nvlist_t *nvl = NULL;

	if (lp->lp_tlv.lth_len < LLDP_OST_LEN_MIN ||
	    lp->lp_tlv.lth_len > LLDP_OST_LEN_MAX) {
		LIBLLDP_DISCARD("invalid length for ost tlv");
		lp->lp_stat.lrxs_tlvdiscard++;
		lp->lp_stat.lrxs_frameerrors++;
		goto done;
	}

	oui[0] = dp[0];
	oui[1] = dp[1];
	oui[2] = dp[2];
	subtype = dp[3];
	dp += 4;

	VERIFY(snprintf(ouistr, sizeof (ouistr), "%x:%x:%x", oui[0], oui[1],
	    oui[2]) < sizeof (ouistr));

	VERIFY(snprintf(substr, sizeof (substr), "%d", subtype) < sizeof (substr));

	if ((ret = nvlist_lookup_nvlist(lp->lp_nvl, ouistr, &nvl)) != 0) {
		nvlist_t *tmp;
		VERIFY3S(ret, ==, ENOENT);
		if ((ret = nvlist_alloc(&tmp, NV_UNIQUE_NAME, 0)) != 0) {
			VERIFY3S(ret, ==, ENOMEM);
			return (ret);
		}
		if ((ret = nvlist_add_nvlist(lp->lp_nvl, ouistr, tmp)) != 0) {
			VERIFY3S(ret, ==, ENOMEM);
			nvlist_free(tmp);
			return (ret);
		}

		if ((ret = nvlist_lookup_nvlist(lp->lp_nvl, ouistr, &nvl)) != 0) {
			VERIFY3S(ret, ==, ENOMEM);
			nvlist_free(tmp);
			return (ret);
		}
		nvlist_free(tmp);
	}

	if (lp->lp_tlv.lth_len - 4 > 0) {
		uchar_t raw[LLDP_OST_LEN_MAX];
		bcopy(dp, raw, lp->lp_tlv.lth_len - 4);
		ret = nvlist_add_byte_array(nvl, substr, raw,
		    lp->lp_tlv.lth_len - 4);
	} else {
		ret = nvlist_add_byte_array(nvl, substr, NULL, 0);
	}

	if (ret != 0) {
		VERIFY3S(ret, ==, ENOMEM);
		return (ret);
	}

done:
	lldp_parse_advance(lp, lp->lp_tlv.lth_len);
	return (0);
}


/*
 * XXX In general we want to revisit this for lldpd which is going to want to
 * have specific mibs and errors incremented and available.
 */
int
lldp_parse_frame(const void *buf, int buflen, nvlist_t **nvpp)
{
	int ret;
	lldp_parse_t lp;
	nvlist_t *nvp = NULL;

	if (buf == NULL || nvpp == NULL || buflen <= 0)
		return (EINVAL);

	/* XXX Figure out flag type */
	if ((ret = nvlist_alloc(&nvp, NV_UNIQUE_NAME, 0)) != 0)
		return (ret);

	bzero(&lp, sizeof (lldp_parse_t));
	lp.lp_nvl = nvp;
	lp.lp_buf = buf;
	lp.lp_rem = buflen;

	if ((ret = lldp_parse_tlv(&lp)) != 0) {
		LIBLLDP_DISCARD("failed to parse TLV, expected chassis");
		lp.lp_stat.lrxs_framediscard++;
		goto err;
	}

	/* XXX Want a better errno */
	if (lp.lp_tlv.lth_type != LLDP_TLV_T_CHASSIS) {
		LIBLLDP_DISCARD("got wrong TLV, expected chassis");
		lp.lp_stat.lrxs_framediscard++;
		lp.lp_stat.lrxs_frameerrors++;
		ret = EINVAL;
		goto err;
	}

	/* Stats and probes done by caller */
	if ((ret = lldp_parse_tlv_chassis(&lp)) != 0)
		goto err;

	if ((ret = lldp_parse_tlv(&lp)) != 0) {
		LIBLLDP_DISCARD("failed to parse TLV, expected port id");
		lp.lp_stat.lrxs_framediscard++;
		goto err;
	}

	/* XXX Want a better errno */
	if (lp.lp_tlv.lth_type != LLDP_TLV_T_PORT_ID) {
		LIBLLDP_DISCARD("got wrong TLV, expectted port id");
		lp.lp_stat.lrxs_framediscard++;
		lp.lp_stat.lrxs_frameerrors++;
		ret = EINVAL;
		goto err;
	}

	if ((ret = lldp_parse_tlv_portid(&lp)) != 0) {
		LIBLLDP_DISCARD("failed to parse port id TLV");
		goto err;
	}

	if ((ret = lldp_parse_tlv(&lp)) != 0) {
		LIBLLDP_DISCARD("failed to parse TLV, expected ttl");
		lp.lp_stat.lrxs_framediscard++;
		goto err;
	}

	/* XXX Want a better errno */
	if (lp.lp_tlv.lth_type != LLDP_TLV_T_TTL) {
		LIBLLDP_DISCARD("got wrong TLV, expectted ttl");
		lp.lp_stat.lrxs_framediscard++;
		lp.lp_stat.lrxs_frameerrors++;
		ret = EINVAL;
		goto err;
	}

	if ((ret = lldp_parse_tlv_ttl(&lp)) != 0) {
		LIBLLDP_DISCARD("failed to parse ttl TLV");
		return (ret);
	}

	for (;;) {
		/*
		 * If we run out of bytes in the frame, then we're done. We
		 * don't have to encounter an explicit End of LLDPDU TLV. See
		 * 9.2.7.7.1, h).
		 *
		 * Note, a TLV may lie about the amount of data in it and thus
		 * cause us to go below zero bytes.
		 */
		if (lp.lp_rem <= 0)
			goto done;

		/*
		 * Section 9.2.7.7.2, e) basically says that if we have a TLV
		 * that extends beyond the end and we're not a mandatory one,
		 * then at this point we should basically just return what we
		 * have and not worry too much. Note, lldp_parse_tlv() bumps the
		 * required stats here.
		 */
		if ((ret = lldp_parse_tlv(&lp)) != 0) {
			VERIFY3U(ret, ==, ERANGE);
			goto done;
		}

		switch (lp.lp_tlv.lth_type) {
		case LLDP_TLV_T_EOL:
			/*
			 * The EOL (end of LLDPDU) is supposed to have a zero
			 * length. If not, we discard the frame, following
			 * 9.2.7.7.2 b).
			 */
			if (lp.lp_tlv.lth_len != 0) {
				LIBLLDP_DISCARD("Encountered EOL TTL "
				    "with non-zero length");
				lp.lp_stat.lrxs_framediscard++;
				lp.lp_stat.lrxs_frameerrors++;
				ret = EINVAL;
			}
			goto done;
		case LLDP_TLV_T_CHASSIS:
		case LLDP_TLV_T_PORT_ID:
		case LLDP_TLV_T_TTL:
			/*
			 * These are only allowed to occur once. We should bump
			 * some stats, but we discard everything. See 9.2.7.7.1
			 * a).
			 */
			LIBLLDP_DISCARD("Encountered duplicate mandatory "
			    "TLV");
			lp.lp_stat.lrxs_framediscard++;
			lp.lp_stat.lrxs_frameerrors++;
			ret = EINVAL;
			goto err;
		case LLDP_TLV_T_PORT_DESC:
			if ((ret = lldp_parse_tlv_string(&lp, "portdesc")) != 0)
				return (ret);
			break;
		case LLDP_TLV_T_SYS_NAME:
			if ((ret = lldp_parse_tlv_string(&lp, "sysname")) != 0)
				return (ret);
			break;
		case LLDP_TLV_T_SYS_DESC:
			if ((ret = lldp_parse_tlv_string(&lp, "sysdesc")) != 0)
				return (ret);
			break;
		case LLDP_TLV_T_SYS_CAPS:
			if ((ret = lldp_parse_tlv_sysdesc(&lp)) != 0)
				return (ret);
			break;
		case LLDP_TLV_T_MGMT_ADDR:
			if ((ret = lldp_parse_tlv_mgmtaddr(&lp)) != 0)
				return (ret);
			break;
		case LLDP_TLV_T_OST:
			if ((ret = lldp_parse_tlv_ost(&lp)) != 0)
				return (ret);
			break;
		default:
			/*
			 * For unkonwn TLVs we're supposed to ignore them. In
			 * this case, increment the size based on the size field
			 * and continue onto the next one.
			 */
			lp.lp_stat.lrxs_tlvdiscard++;
			lp.lp_stat.lrxs_frameerrors++;
			lldp_parse_advance(&lp, lp.lp_tlv.lth_len);
			break;
		}
	}

done:
	*nvpp = nvp;
	return (0);

err:
	nvlist_free(nvp);
	return (ret);
}
