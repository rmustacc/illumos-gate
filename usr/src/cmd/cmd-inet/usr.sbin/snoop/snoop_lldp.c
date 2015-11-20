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
 * Parse IEEE 802.1AB Link Layer Discovery Protocol (LLDP)
 */

#include <liblldp.h>
#include <libnvpair.h>
#include <string.h>
#include <sys/debug.h>
#include "snoop.h"

int
interpret_lldp(int flags, char *data, int fraglen)
{
	int ret;
	nvlist_t *nvl, *cnvl, *pnvl;
	char *chassis, *port;
	uint16_t ttl;

	if ((ret = lldp_parse_frame(data, fraglen, &nvl)) != 0) {
		(void) snprintf(get_sum_line(), MAXLINE, "LLDP BOGON");
		if (flags & F_DTAIL)
			show_header("LLDP BOGON:  ", "Invalid packet", fraglen);
		return (fraglen);
	}

	/*
	 * For the summary we use the three mandatory TLVs, chassis, port, and
	 * ttl. We dump the others only in detail mode.
	 */
	cnvl = fnvlist_lookup_nvlist(nvl, "chassis");
	pnvl = fnvlist_lookup_nvlist(nvl, "portid");
	chassis = fnvlist_lookup_string(cnvl, "id");
	port = fnvlist_lookup_string(pnvl, "id");
	ttl = fnvlist_lookup_uint16(nvl, "ttl");

	/*
	 * For the summary use the chassis and port
	 */
	if (flags & F_SUM) {
		(void) snprintf(get_sum_line(), MAXLINE, "LLDPDU from "
		    "%s port %s", chassis, port);
	}

	if (flags & F_DTAIL) {
		char *portdesc, *sysname, *sysdesc;
		nvlist_t *capsnvl;
		uint16_t caps, enab;
		boolean_t hcaps = B_FALSE;

		if ((ret = nvlist_lookup_string(nvl, "portdesc",
		    &portdesc)) != 0) {
			VERIFY3S(ret, ==, ENOENT);
			portdesc = NULL;
		}

		if ((ret = nvlist_lookup_string(nvl, "sysname",
		    &sysname)) != 0)
			sysname = NULL;

		if ((ret = nvlist_lookup_string(nvl, "sysdesc",
		    &sysdesc)) != 0)
			sysdesc = NULL;

		if (nvlist_lookup_nvlist(nvl, "syscap", &capsnvl) == 0) {
			hcaps = B_TRUE;
			caps = fnvlist_lookup_uint16(capsnvl, "capabilities");
			enab = fnvlist_lookup_uint16(capsnvl, "enabled");
		} 

		show_header("LLDP:  ", "LLDP PDU", fraglen);
		show_space();
		(void) snprintf(get_line(0, 0), get_line_remain(),
		    "Chassis: %s", chassis);
		(void) snprintf(get_line(0, 0), get_line_remain(),
		    "Port ID: %s", port);
		(void) snprintf(get_line(0, 0), get_line_remain(),
		    "TTL: %d seconds", ttl);
		if (portdesc != NULL) {
			(void) snprintf(get_line(0, 0), get_line_remain(),
			    "Port Description: %s", portdesc);
		}

		if (sysname != NULL) {
			(void) snprintf(get_line(0, 0), get_line_remain(),
			    "System Name: %s", sysname);
		}

		if (sysdesc != NULL) {
			(void) snprintf(get_line(0, 0), get_line_remain(),
			    "System Description: %s", sysdesc);
		}

		if (hcaps == B_TRUE) {
			(void) snprintf(get_line(0, 0), get_line_remain(),
			    "Capabilities present: %02x", caps);
			(void) snprintf(get_line(0, 0), get_line_remain(),
			    "Capabilities enabled: %02x", enab);
		}

		show_space();
	}

#if 0
	dump_nvlist(nvl, 0);
#endif
	nvlist_free(nvl);
	return (0);
}
