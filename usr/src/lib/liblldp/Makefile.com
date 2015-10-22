#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#

#
# Copyright 2015 Joyent, Inc.
#

LIBRARY =		 liblldp.a
VERS =			 .1
OBJECTS =		liblldp.o
USDT_PROVIDERS =	lldp_provider.d

include ../../Makefile.lib

LIBS =			 $(DYNLIB) $(LINTLIB)

SRCDIR =		../common
CPPFLAGS +=		-I.
LDLIBS +=		-lc -lnvpair -lsocket -lnsl -lcmdutils

$(LINTLIB) := SRCS = $(SRCDIR)/$(LINTSRC)

.KEEP_STATE:

all: $(LIBS)

lint: lintcheck

include ../../Makefile.targ
include ../../Makefile.usdt
