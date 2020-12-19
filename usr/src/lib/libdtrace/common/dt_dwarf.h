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
 * Copyright 2020 Oxide Computer Company
 */

#ifndef _DT_DWARF_H
#define	_DT_DWARF_H

#include <gelf.h>
#include <libdwarf.h>
#include <libproc.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int dt_dwarf_find_function(Dwarf_Debug, GElf_Sym *, prsyminfo_t *, Dwarf_Die *, Dwarf_Die *);

typedef int (*dt_dwarf_var_f)(Dwarf_Debug, Dwarf_Die, Dwarf_Half, void *);
extern int dt_dwarf_var_iter(Dwarf_Debug, Dwarf_Die, dt_dwarf_var_f, void *);

extern const char *dt_dwarf_string(Dwarf_Die, Dwarf_Half);
extern int dt_dwarf_flag(Dwarf_Die, Dwarf_Half, boolean_t *);
extern boolean_t dt_dwarf_loc_need_base(Dwarf_Loc_Head_c);

extern int dt_dwarf_range_match(uintptr_t, Dwarf_Addr, Dwarf_Small, Dwarf_Addr,
    Dwarf_Addr, boolean_t *);

extern char *dt_dwarf_loc_compile(Dwarf_Locdesc_c, Dwarf_Unsigned, uint32_t);

extern boolean_t dt_dwarf_isareg(uint32_t, uint32_t, uint32_t *);

#ifdef __cplusplus
}
#endif

#endif /* _DT_DWARF_H */
