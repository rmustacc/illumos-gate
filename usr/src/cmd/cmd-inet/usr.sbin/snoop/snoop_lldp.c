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
	nvlist_t *nvl, *chassis, *port;
	char *chassis, *port;
	uint16_t ttl;

	if ((ret = lldp_frame_parse(data, fraglen, &nvl)) != 0) {
		(void) snprintf(get_sum_line(), MAXLINE, "LLDP BOGON");
		if (flags & F_DTAIL)
			show_header("LLDP BOGON:  ", "Invalid packet", fraglen);
		return (fraglen);
	}

	/*
	 * For the summary we use the three mandatory TLVs, chassis, port, and
	 * ttl. We dump the others only in detail mode.
	 */
	chassis = lldp_frame_chassis(nvl);
	port = lldp_frame_port(nvl);
	ttl = lldp_frame_ttl(nvl);
	VERIFY(nvlist_lookup_string(nvl, "chassis", &chassis) == 0);
	VERIFY(nvlist_lookup_string(nvl, "port", &port) == 0);
	VERIFY(nvlist_lookup_uint16(nvl, "ttl", &ttl) == 0);

	/*
	 * For the summary use the chassis and port
	 */
	if (flags & F_SUM) {
		(void) snprintf(get_sum_line(), MAXLINE, "LLDPDU from chassis "
		    "%s port %s", chassis, port);
	}

	dump_nvlist(nvl, 0);
	nvlist_free(nvl);
	return (0);
}
