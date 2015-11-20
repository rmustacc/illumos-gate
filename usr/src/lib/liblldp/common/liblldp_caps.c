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
 * Common routines to interacting with capabilities and translating to and from
 * them.
 */

#include <libnvpair.h>
#include <sys/debug.h>

static const char *lldp_capstrs[16] = {
	"Other",
	"Repeater",
	"MAC Bridge",
	"WLAN Access Point",
	"Router",
	"Telephone",
	"DOCSIS cable device",
	"Station Only",
	"C-VLAN",
	"S-VLAN",
	"Two Port Mac Relay",
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

int
lldp_parse_caps(uint16_t capbits, nvlist_t **nvpp)
{
	int i, ret;
	nvlist_t *nvl;

	if ((ret = nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0)) != 0) {
		VERIFY3S(ret, ==, ENOMEM);
		return (ret);
	}

	for (i = 0; i < 16; i++) {
		if ((capbits & (1 << i)) == 0)
			continue;
		if (lldp_capstrs[i] == NULL) {
			nvlist_free(nvl);
			return (EINVAL);
		}
		if ((ret = nvlist_add_boolean_value(nvl, lldp_capstrs[i],
		    B_TRUE)) != 0) {
			VERIFY3S(ret, ==, ENOMEM);
			return (ret);
		}
	}

	*nvpp = nvl;
	return (0);
}
