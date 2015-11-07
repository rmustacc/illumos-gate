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

LIBRARY=	libdwarf.a
VERS=		.1

OBJECTS=dwarf_abbrev.o		\
	dwarf_addr_finder.o	\
	dwarf_alloc.o		\
	dwarf_arange.o		\
	dwarf_die_deliv.o	\
	dwarf_elf_access.o	\
	dwarf_error.o		\
	dwarf_form.o		\
	dwarf_frame.o		\
	dwarf_frame2.o		\
	dwarf_frame3.o		\
	dwarf_funcs.o		\
	dwarf_global.o		\
	dwarf_harmless.o	\
	dwarf_init_finish.o	\
	dwarf_leb.o		\
	dwarf_line.o		\
	dwarf_line2.o		\
	dwarf_loc.o		\
	dwarf_macro.o		\
	dwarf_names.o		\
	dwarf_original_elf_init.o	\
	dwarf_print_lines.o	\
	dwarf_pubtypes.o	\
	dwarf_query.o		\
	dwarf_ranges.o		\
	dwarf_sort_line.o	\
	dwarf_string.o		\
	dwarf_stubs.o		\
	dwarf_types.o		\
	dwarf_util.o		\
	dwarf_vars.o		\
	dwarf_weaks.o		\
	malloc_check.o		\
	pro_alloc.o		\
	pro_arange.o		\
	pro_die.o		\
	pro_encode_nm.o		\
	pro_error.o		\
	pro_expr.o		\
	pro_finish.o		\
	pro_forms.o		\
	pro_frame.o		\
	pro_funcs.o		\
	pro_init.o		\
	pro_line.o		\
	pro_macinfo.o		\
	pro_pubnames.o		\
	pro_reloc.o		\
	pro_reloc_stream.o	\
	pro_reloc_symbolic.o	\
	pro_section.o		\
	pro_types.o		\
	pro_vars.o		\
	pro_weaks.o

include ../../Makefile.lib
include ../../Makefile.rootfs

LIBS =		$(DYNLIB) $(LINTLIB)
LDLIBS +=	-lelf -lc

SRCDIR =	../common
CPPFLAGS +=	-I$(SRCDIR) -DELF_TARGET_ALL=1
CERRWARN +=	-_gcc=-Wno-unused
CERRWARN +=	-_gcc=-Wno-implicit-function-declaration

#
# DWARF has never really been linted regardless of where it lives. Longer term
# we should work witih upstream to upgrade and clean up some of these.
#
LINTCHECKFLAGS   +=    -erroff=E_FUNC_RET_ALWAYS_IGNOR2
LINTCHECKFLAGS   +=    -erroff=E_FUNC_RET_MAYBE_IGNORED2
LINTCHECKFLAGS   +=    -erroff=E_INCONS_ARG_DECL2
LINTCHECKFLAGS   +=    -erroff=E_FUNC_ARG_UNUSED
LINTCHECKFLAGS   +=    -erroff=E_FUNC_VAR_UNUSED
LINTCHECKFLAGS   +=    -erroff=E_BAD_PTR_CAST_ALIGN
LINTCHECKFLAGS   +=    -erroff=E_INCONS_VAL_TYPE_DECL2
LINTCHECKFLAGS   +=    -erroff=E_CONSTANT_CONDITION
LINTCHECKFLAGS   +=    -erroff=E_TOO_MANY_ARG_FOR_FMT2
LINTCHECKFLAGS   +=    -erroff=E_FUNC_SET_NOT_USED
LINTCHECKFLAGS   +=    -erroff=E_CASE_FALLTHRU
LINTCHECKFLAGS   +=    -erroff=E_NOP_IF_STMT
LINTCHECKFLAGS   +=    -erroff=E_NOP_ELSE_STMT
LINTCHECKFLAGS   +=    -erroff=E_END_OF_LOOP_CODE_NOT_REACHED
LINTCHECKFLAGS   +=    -erroff=E_RET_INT_IMPLICITLY

.KEEP_STATE:

all:	$(LIBS)

lint:	lintcheck

include ../../Makefile.targ
