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

include $(SRC)/Makefile.master

ROOTOPTPKG = $(ROOT)/opt/os-tests
TESTDIR = $(ROOTOPTPKG)/tests/periodic

PROGS = 		\
	absolute	\
	badargs		\
	cancel		\
	interleave	\
	interval	\
	oneshot 	\
	periodic	\
	reschedule	\
	stress

include $(SRC)/cmd/Makefile.cmd
include $(SRC)/test/Makefile.com

CMDS = $(PROGS:%=$(TESTDIR)/%)
$(CMDS) := FILEMODE = 0555

CPPFLAGS += -D_REENTRANT
LDLIBS += -lumem -lperiodic

all: $(PROGS)

install: all $(CMDS)

lint: lint_SRCS

clobber: clean
	-$(RM) $(PROGS)

clean:
	-$(RM) *.o

%.o: %.c
	$(COMPILE.c) -o $@ -c $<
	$(POST_PROCESS_O)

$(PROG): $(OBJS)
	$(LINK.c) $(OBJS) -o $@ $(LDLIBS)
	$(POST_PROCESS)

$(CMDS): $(TESTDIR) $(PROGS)

$(TESTDIR):
	$(INS.dir)

$(TESTDIR)/%: %
	$(INS.file)
