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

/*
 * Convenience routines for interacting with, and finding things inside of
 * libdwarf.
 */

#include <errno.h>
#include <libdwarf.h>
#include <dwarf.h>
#include <strings.h>

#include <dt_impl.h>
#include <dt_dwarf.h>

/* XXX */
#include <stdio.h>

int
dt_dwarf_flag(Dwarf_Die die, Dwarf_Half attr_name, boolean_t *outp)
{
	int ret;
	Dwarf_Error derr;
	Dwarf_Attribute attr;
	Dwarf_Bool b;

	ret = dwarf_attr(die, attr_name, &attr, &derr);
	if (ret == DW_DLV_NO_ENTRY) {
		return (ENOENT);
	} else if (ret != DW_DLV_OK) {
		return (EIO);
	}

	ret = dwarf_formflag(attr, &b, &derr);
	dwarf_dealloc_attribute(attr);
	if (ret == DW_DLV_OK) {
		*outp = b != 0;
		return (0);
	}

	return (EIO);
}

const char *
dt_dwarf_string(Dwarf_Die die, Dwarf_Half attr_name)
{
	int ret;
	Dwarf_Error derr;
	Dwarf_Attribute attr;
	char *out;

	if (dwarf_attr(die, attr_name, &attr, &derr) != DW_DLV_OK) {
		return (NULL);
	}

	ret = dwarf_formstring(attr, &out, &derr);
	dwarf_dealloc_attribute(attr);
	if (ret == DW_DLV_OK) {
		return (out);
	}

	return (NULL);
}

static boolean_t
dt_dwarf_func_match(Dwarf_Die die, prsyminfo_t *prs)
{
	Dwarf_Half tag;
	Dwarf_Error derr;
	const char *name;
	boolean_t decl;
	int ret;

	if (dwarf_tag(die, &tag, &derr) != DW_DLV_OK) {
		return (B_FALSE);
	}

	if (tag != DW_TAG_subprogram) {
		return (B_FALSE);
	}

	if ((name = dt_dwarf_string(die, DW_AT_name)) == NULL) {
		return (B_FALSE);
	}

	if (strcmp(name, prs->prs_name) != 0) {
		return (B_FALSE);
	}

	/*
	 * Check if this is a declaration to make sure we don't find a PLT stub.
	 * XXX In theory if we have two static functions with the same name we
	 * really need the GElf_Sym's addr to disambiguate the lowpc.
	 */
	if (dt_dwarf_flag(die, DW_AT_declaration, &decl) == 0 && decl) {
		return (B_FALSE);
	}

	return (B_TRUE);
}

/*
 * Attempt to find the named function in dwarf. XXX Right now we're doing this
 * the max power way. The problem is that dwarf information is generally
 * organized by the underlying file a function shows up (as each one has a die).
 * We could opt to trust the symbol table and find the corresponding STT_FILE
 * entry. For now, we do this max power (but slower).
 */
int
dt_dwarf_find_function(Dwarf_Debug dw, GElf_Sym *symp, prsyminfo_t *prs,
    Dwarf_Die *cup, Dwarf_Die *diep)
{
	int ret;
	Dwarf_Unsigned hdrlen, abboff, nexthdr;
	Dwarf_Half addrsz, vers;
	Dwarf_Error derr;

	while ((ret = dwarf_next_cu_header(dw, &hdrlen, &vers, &abboff,
	    &addrsz, &nexthdr, &derr)) != DW_DLV_NO_ENTRY) {
		Dwarf_Die top;
		Dwarf_Die iter;

		ret = dwarf_siblingof(dw, NULL, &top, &derr);
		if (ret != DW_DLV_OK) {
			continue;
		}

		ret = dwarf_child(top, &iter, &derr);
		if (ret != DW_DLV_OK) {
			dwarf_dealloc_die(top);
			continue;
		}

		/*
		 * At this point we will have all the top-level siblings that we
		 * are looking for. From here, attempt to find one that
		 * corresponds to our function name.
		 */
		while (iter != NULL) {
			Dwarf_Die next;

			if (dt_dwarf_func_match(iter, prs)) {
				*cup = top;
				*diep = iter;
				return (0);
			}

			if ((ret = dwarf_siblingof(dw, iter, &next, &derr)) !=
			    DW_DLV_OK) {
				next = NULL;
			}
			dwarf_dealloc_die(iter);
			iter = next;
		}

		dwarf_dealloc_die(top);
	}

	return (ENOENT);
}

int
dt_dwarf_var_iter(Dwarf_Debug dw, Dwarf_Die init_die, dt_dwarf_var_f func, void *arg)
{
	Dwarf_Die iter;
	Dwarf_Error derr;

	if (dwarf_child(init_die, &iter, &derr) != 0) {
		return (-1);
	}

	while (iter != NULL) {
		int ret;
		Dwarf_Die next;
		Dwarf_Half tag;
	
		if (dwarf_tag(iter, &tag, &derr) != DW_DLV_OK) {
			goto next;
		}

		switch (tag) {
		case DW_TAG_formal_parameter:
		case DW_TAG_variable:
			ret = func(dw, iter, tag, arg);
			break;
		case DW_TAG_lexical_block:
			ret = dt_dwarf_var_iter(dw, iter, func, arg);
			break;
		default:
			ret = 0;
			break;
		}

		if (ret != 0) {
			dwarf_dealloc_die(iter);
			return (ret);
		}
next:
		if (dwarf_siblingof(dw, iter, &next, &derr) != DW_DLV_OK) {
			next = NULL;
		}
		dwarf_dealloc_die(iter);
		iter = next;
	}
	return (0);
}

int
dt_dwarf_range_match(uintptr_t addr, Dwarf_Addr base, Dwarf_Small lle, Dwarf_Addr low,
    Dwarf_Addr high, boolean_t *outp)
{
	Dwarf_Addr start, end;

	switch (lle) {
	case DW_LLE_offset_pair:
		/*
		 * If a DW_LLE_base_address was here, then it was taken care of
		 * for us. However, if not, then we need to manually add the
		 * base address.
		 */
		start = base + low;
		end = base + high;
		break;
	case DW_LLE_startx_length:
	case DW_LLE_startx_endx:
	case DW_LLE_start_end:
	case DW_LLE_start_length:
		/*
		 * Note, libdwarf normalizes these for us. In theory it has
		 * normalized most of the DWARF5 x variants as well.
		 */
		start = low;
		end = high;
		break;
	case DW_LLE_base_address:
	case DW_LLE_base_addressx:
		/*
		 * libdwarf has in theory went through and made it so that these
		 * have been handled for us. We will thus never match them.
		 */
		return (B_FALSE);
	default:
		dt_dprintf("unknown lle type: %x", lle);
		return (ENOTSUP);
	}

	*outp = addr >= start && addr < end;
	return (0);
}

/*
 * XXX While libdwarf is meant to make the issue of tracking the base address
 * easy, in a bunch of our testing, it does not. For the time being we work
 * around this ourselves by manually trying to figure out if it successfully
 * recorded a base address or not. If not, we will have to ourselves.
 */
boolean_t
dt_dwarf_loc_need_base(Dwarf_Loc_Head_c head)
{
	Dwarf_Small lkind;
	Dwarf_Unsigned count, version, index, bytes;
	Dwarf_Half offset, addr_size, seg;
	Dwarf_Unsigned total_offset, total, table_off, table_count;
	Dwarf_Bool base_present, addr_present, debug_present;
	Dwarf_Unsigned base, addr, debug_addr, lle_off;
	Dwarf_Error derr;

	if (dwarf_get_loclist_head_basics(head, &lkind, &count, &version,
	    &index, &bytes, &offset, &addr_size, &seg, &total_offset, &total,
	    &table_off, &table_count, &base_present, &base, &addr_present,
	    &addr, &debug_present, &debug_addr, &lle_off, &derr) == DW_DLV_OK) {
		return (addr_present ? B_FALSE: B_TRUE);
	}

	/*
	 * If we failed to get the head, assume we need it regardless.
	 */
	return (B_TRUE);
}

/*
 * Iterate over a series of DWARF location pointer expressions and compile them
 * into a D expression. This will likely need to evolve into a full stack
 * machine with pushing D expressions onto the stack; however, as right now we
 * generally have simpler expressions, we're getting away with just a single
 * pass.
 */
char *
dt_dwarf_loc_compile(Dwarf_Locdesc_c locptr, Dwarf_Unsigned count, uint32_t class)
{
	Dwarf_Error derr;
	Dwarf_Unsigned i;
	char *str = NULL;

	for (i = 0; i < count; i++) {
		Dwarf_Small opcode;
		Dwarf_Unsigned arg1, arg2, arg3, raw1, raw2, raw3, branch;
		uint32_t regno;

		if (dwarf_get_location_op_value_d(locptr, i, &opcode, &arg1,
		    &arg2, &arg3, &raw1, &raw2, &raw3, &branch, &derr) != DW_DLV_OK) {
			goto err;
		}

		/*
		 * Handle large continuous swaths of the space before falling
		 * back to a switch statement.
		 */
		if (opcode >= DW_OP_lit0 && opcode <= DW_OP_lit31) {
			if (asprintf(&str, "%u", opcode - DW_OP_lit0) == -1) {
				goto err;
			}
			continue;
		}

		if (opcode >= DW_OP_reg0 && opcode <= DW_OP_reg31 || opcode == DW_OP_regx) {
			uint32_t dwreg;

			if (opcode == DW_OP_regx) {
				dwreg = arg1;
			} else {
				dwreg = opcode - DW_OP_reg0;
			}

			if (!dt_dwarf_isareg(class, dwreg, &regno)) {
				dt_dprintf("failed to translate op 0x%x, "
				    "class 0x%x", opcode, class);
				goto err;
			}

			if (asprintf(&str, "uregs[%u]", regno) == -1) {
				goto err;
			}
			continue;
		}

		if (opcode >= DW_OP_breg0 && opcode <= DW_OP_breg31 ||
		    opcode == DW_OP_bregx) {
			uint32_t dwreg;
			Dwarf_Signed addend;

			if (opcode == DW_OP_bregx) {
				dwreg = arg1;
				addend = (Dwarf_Signed)arg2;
			} else {
				dwreg = opcode - DW_OP_breg0;
				addend = (Dwarf_Signed)arg1;
			}

			if (!dt_dwarf_isareg(class, dwreg, &regno)) {
				dt_dprintf("failed to translate op 0x%x, "
				    "class 0x%x", opcode, class);
				goto err;
			}

			if (asprintf(&str, "uregs[%u]%s%lld", regno,
			    addend >= 0 ? "+" : "", addend) == -1) {
				goto err;
			}
			continue;
		}

		switch (opcode) {
		case DW_OP_stack_value:
			/*
			 * This tells us to take the current expression on the
			 * DWARF stack. With our current "compiler" this means
			 * that there isn't much that we should have to do and
			 * can just return what we already have.
			 */
			break;
		case DW_OP_GNU_entry_value:
			/*
			 * A DW_OP_GNU_entry_value indicates that if we could
			 * unwind execution state to the start of the function
			 * we could get at this value. While some heuristics
			 * involving saveargs and other things could be
			 * possible, unfortunately, the best option here is to
			 * give up.
			 */
			dt_dprintf("encountered unimplemented entry_value op");
			goto err;
		default:
			dt_dprintf("unhandled opcode 0x%x", opcode);
			goto err;
		}
	}

	return (str);
err:
	free(str);
	return (NULL);
}
