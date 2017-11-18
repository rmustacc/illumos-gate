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
# Copyright (c) 2017, Joyent, Inc.
#

include $(SRC)/Makefile.master

ROOTOPTPKG = $(ROOT)/opt/os-tests
TESTDIR = $(ROOTOPTPKG)/tests/uccid

PROGS = 		\
	excl-basic	\
	excl-badread	\
	excl-close	\
	excl-loop	\
	excl-nonblock	\
	excl-reset	\
	notxn-poll	\
	status		\
	pollin		\
	pollout		\
	txn-pollerr	\
	yk

COMMON_OBJS = \
	atr.o

LNPROGS = $(PROGS:%=%.ln)

include $(SRC)/cmd/Makefile.cmd
include $(SRC)/test/Makefile.com

CMDS = $(PROGS:%=$(TESTDIR)/%)
$(CMDS) := FILEMODE = 0555

CPPFLAGS += -D_REENTRANT -I$(SRC)/common/ccid/

all: $(PROGS) $(PARITYPROGS)

install: all $(CMDS)

lint: $(LNPROGS)

clobber: clean
	-$(RM) $(PROGS) $(PARITYPROGS)

clean:
	-$(RM) *.o

%.o: $(SRC)/common/ccid/%.c
	$(COMPILE.c) -o $@ -c $<
	$(POST_PROCESS_O)

%.o: %.c
	$(COMPILE.c) -o $@ -c $<
	$(POST_PROCESS_O)

%: %.o
	$(LINK.c) -o $@ $< $(EXTRA_OBJS)
	$(POST_PROCESS)

%.ln: %
	$(LINT.c) $<.c $(LDLIBS)

$(PROGS): $(COMMON_OBJS)

$(CMDS): $(TESTDIR) $(PROGS)

$(TESTDIR):
	$(INS.dir)

$(TESTDIR)/%: %
	$(INS.file)
