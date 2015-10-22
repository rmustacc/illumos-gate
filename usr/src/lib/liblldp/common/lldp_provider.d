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

provider liblldp {
	probe	discard(char *);
	probe	skip(char *);
};

#pragma D attributes Stable/Stable/ISA          provider liblldp provider
#pragma D attributes Private/Private/Unknown    provider liblldp module
#pragma D attributes Private/Private/Unknown    provider liblldp function
#pragma D attributes Evolving/Evolving/ISA          provider liblldp name
#pragma D attributes Evolving/Evolving/ISA          provider liblldp args
