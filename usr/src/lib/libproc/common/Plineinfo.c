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
 * Copyright 2019 Joyent, Inc.
 */

/*
 * Interface with DWARF and get line information that's required.
 */

#include <sys/types.h>
#include <libdwarf.h>
#include <dwarf.h>
#include <stddef.h>
#include <strings.h>

#include "libproc.h"
#include "Pcontrol.h"

typedef struct file_line {
	avl_node_t	fl_offset;
	uint64_t	fl_start;
	uint64_t	fl_end;
	uint64_t	fl_line;
	uint64_t	fl_column;
	/* XXX This is incredibly wasteful of memory */
	char		*fl_srcfile;
} file_line_t;

void
lineinfo_free(file_info_t *fptr)
{
	void *c = NULL;
	file_line_t *fl;

	while ((fl = avl_destroy_nodes(&fptr->file_lines, &c)) != NULL) {
		free(fl->fl_srcfile);
		free(fl);
	}
	avl_destroy(&fptr->file_lines);
}

static int
lineinfo_comparator(const void *lv, const void *rv)
{
	const file_line_t *l = lv;
	const file_line_t *r = rv;

	dprintf("comparing %" PRIx64 "/% " PRIx64 " to "
	    "%" PRIx64 "/%" PRIx64 "\n", l->fl_start,
	    l->fl_end, r->fl_start, r->fl_end);

	/*
	 * Start with the easy cases. If the left hand size 
	 */
	if (l->fl_end < r->fl_start) {
		return (-1);
	}

	if (l->fl_start > r->fl_end) {
		return (1);
	}

	return (0);
}

static int
lineinfo_process_single(Dwarf_Line line, Dwarf_Unsigned *lineno, Dwarf_Unsigned *srcno,
    Dwarf_Addr *addr, Dwarf_Signed *col, Dwarf_Bool *end)
{
	Dwarf_Error derr;

	if (dwarf_lineno(line, lineno, &derr) != DW_DLV_OK) {
		dprintf("failed to get line number: %s\n",
		    dwarf_errmsg(derr));
		return (EINVAL);
	}

	if (dwarf_line_srcfileno(line, srcno, &derr) !=
	    DW_DLV_OK) {
		dprintf("failed to get source file number: %s\n",
		    dwarf_errmsg(derr));
		return (EINVAL);
	}

	if (dwarf_lineaddr(line, addr, &derr) != DW_DLV_OK) {
		dprintf("failed to get source line address: %s\n",
		    dwarf_errmsg(derr));
		return (EINVAL);
	}

	if (dwarf_lineendsequence(line, end, &derr) !=
	    DW_DLV_OK) {
		dprintf("failed to get source line end information: %s\n",
		    dwarf_errmsg(derr));
		return (EINVAL);
	}

	if (dwarf_lineoff(line, col, &derr) != DW_DLV_OK) {
		dprintf("failed to get source line offset: %s\n",
		    dwarf_errmsg(derr));
		return (EINVAL);
	}

	return (0);
}

static int
lineinfo_process_lines(file_info_t *fptr, Dwarf_Line *lines, Dwarf_Signed count,
    char **srcs, Dwarf_Signed nsrcs)
{
	int ret;
	Dwarf_Signed i;
	Dwarf_Unsigned cur_line, cur_src, next_line, next_src;
	Dwarf_Addr cur_addr, next_addr;
	Dwarf_Signed cur_column, next_column;
	Dwarf_Bool cur_end, next_end;

	if (count == 0) {
		return (0);
	}

	if ((ret = lineinfo_process_single(lines[0], &cur_line, &cur_src,
	    &cur_addr, &cur_column, &cur_end)) != 0) {
		return (EAGAIN);
	}

	for (i = 1; i < count; i++, cur_line = next_line, cur_src = next_src,
	    cur_addr = next_addr, cur_column = next_column,
	    cur_end = next_end) {
		file_line_t *fl, *lookup;
		avl_index_t idx;

		if ((ret = lineinfo_process_single(lines[i], &next_line, &next_src,
		    &next_addr, &next_column, &next_end)) != 0) {
			return (EAGAIN);
		}

		/*
		 * If the line number was present for some reason, we skip this
		 * entry.
		 */
		if (cur_line == 0)
			continue;

		/*
		 * Current address is an ending marker, that means there's not
		 * much use for it.
		 */
		if (cur_end != 0) {
			continue;
		}

		/*
		 * We've seen a few cases where we have the same address twice
		 * in a row often because it's being used to mark the start of a
		 * file or something else is going on. If that's the case, use
		 * the latter entry.
		 */
		if (cur_addr == next_addr) {
			continue;
		}

		if (cur_addr > next_addr) {
			dprintf("next address somehow is less than current! "
			    "Current: %" PRIx64 ", next: %" PRIx64 "\n",
			    cur_addr, next_addr);
			return (EINVAL);
		}

		if ((fl = calloc(1, sizeof (file_line_t))) == NULL) {
			return (ENOMEM);
		}

		fl->fl_start = cur_addr;
		fl->fl_end = next_addr - 1;
		fl->fl_line = cur_line;
		if (cur_column < 0) {
			fl->fl_column = 0;
		} else {
			fl->fl_column = cur_column;
		}
		if (cur_src == 0 || cur_src > nsrcs) {
			fl->fl_srcfile = NULL;
		} else {
			fl->fl_srcfile = strdup(srcs[cur_src - 1]);
		}

		dprintf("adding node %" PRIx64 "-%" PRIx64 "\n", fl->fl_start,
		    fl->fl_end);

		lookup = avl_find(&fptr->file_lines, fl, &idx);
		if (lookup != NULL) {
			free(fl);
			dprintf("encountered overlapping node! Have start %"
			    PRIx64 "/% " PRIx64 ", found %" PRIx64 "/%"
			    PRIx64 "\n", fl->fl_start, fl->fl_end,
			    lookup->fl_start, lookup->fl_end);
			return (EINVAL);
		}
		avl_insert(&fptr->file_lines, fl, idx);
	}

	/*
	 * We skip the last entry becuase either it's an ending marker or if
	 * it's not, we have no idea what the next address we'd stop at is.
	 */

	return (0);
}

static void
lineinfo_free_dwarf_strings(Dwarf_Debug dw, char **srcs, Dwarf_Signed count)
{
	Dwarf_Signed i;

	for (i = 0; i < count; i++) {
		dwarf_dealloc(dw, srcs[i], DW_DLA_STRING);
	}
	dwarf_dealloc(dw, srcs, DW_DLA_LIST);
}

static int
lineinfo_build_dwarf(struct ps_prochandle *P, file_info_t *fptr)
{
	int ret;
	Elf *elf;
	Dwarf_Debug dw = NULL;
	Dwarf_Error derr;
	Dwarf_Die die;
	Dwarf_Unsigned hdrlen, abboff, nexthdr;
	Dwarf_Half addrsz;

	if (fptr->file_dwarf > 0) {
		return (0);
	} else if (fptr->file_dwarf < 0) {
		return (ENOTSUP);
	}

	if (fptr->file_dbgelf != NULL) {
		elf = fptr->file_dbgelf;
	} else if (fptr->file_elf != NULL) { 
		elf = fptr->file_elf;
	} else {
		return (ENOTSUP);
	}

	ret = dwarf_elf_init(elf, DW_DLC_READ, NULL, NULL, &dw, &derr);
	if (ret != DW_DLV_OK) {
		dprintf("failed to open DWARF handle for file %s: %s\n",
		    fptr->file_pname, dwarf_errmsg(derr));
		ret = ESRCH;
		goto err;
	}

	avl_create(&fptr->file_lines, lineinfo_comparator, sizeof (file_line_t),
	    offsetof(file_line_t, fl_offset));

	while ((ret = dwarf_next_cu_header(dw, &hdrlen, NULL, &abboff,
	    &addrsz, &nexthdr, &derr)) != DW_DLV_NO_ENTRY) {
		Dwarf_Line *lines;
		Dwarf_Signed count, srccount;
		char **srcs;

		if ((ret = dwarf_siblingof(dw, NULL, &die, &derr)) != DW_DLV_OK) {
			dprintf("failed to get primary die from CU: %s\n",
			    dwarf_errmsg(derr));
			ret = ESRCH;
			goto err;
		}

		if ((ret = dwarf_srclines(die, &lines, &count, &derr)) !=
		    DW_DLV_OK) {
			dprintf("failed to get line information for die: %s\n",
			    dwarf_errmsg(derr));
			ret = ESRCH;
			goto err;
		}

		if ((ret = dwarf_srcfiles(die, &srcs, &srccount, &derr)) !=
		    DW_DLV_OK) {
			printf("failed to get source files names for die: %s\n",
			    dwarf_errmsg(derr));
			dwarf_srclines_dealloc(dw, lines, count);
			ret = ESRCH;
			goto err;
		}

		if ((ret = lineinfo_process_lines(fptr, lines, count, srcs,
		    srccount)) != 0 && ret != EAGAIN) {
			dprintf("failed to process line info: %d\n", ret);
			dwarf_srclines_dealloc(dw, lines, count);
			lineinfo_free_dwarf_strings(dw, srcs, srccount);
			ret = EINVAL;
			goto err;
		}

		lineinfo_free_dwarf_strings(dw, srcs, srccount);
		dwarf_srclines_dealloc(dw, lines, count);
	}

	(void) dwarf_finish(dw, &derr);
	fptr->file_dwarf = 1;
	return (0);

err:
	if (dw != NULL) {
		(void) dwarf_finish(dw, &derr);
	}
	lineinfo_free(fptr);

	return (ret);
}

int
Paddr_to_lineinfo(struct ps_prochandle *P, uintptr_t addr, prlineinfo_t *linep)
{
	int ret;
	map_info_t *mptr;
	file_info_t *fptr;
	file_line_t *fl, lookup = { 0 };
	uintptr_t search;

	if (!P->info_valid)
		Pupdate_maps(P);

	if ((mptr = Paddr2mptr(P, addr)) == NULL ||
	    (fptr = mptr->map_file) == NULL)
		return (ENOENT);

	Pbuild_file_symtab(P, fptr);

	if ((ret = lineinfo_build_dwarf(P, fptr)) != 0) {
		fptr->file_dwarf = -1;
		return (ret);
	}

	dprintf("vaddr: %p\n", mptr->map_pmap.pr_vaddr);
	if (fptr->file_etype == ET_DYN) {
		search = addr - mptr->map_pmap.pr_vaddr;
	} else {
		search = addr;
	}
	lookup.fl_start = lookup.fl_end = search;
	fl = avl_find(&fptr->file_lines, &lookup, NULL);
	if (fl == NULL) {
		dprintf("couldn't find address %" PRIx64 "\n", addr);
		return (ENOENT);
	}

	linep->prl_addr = addr;
	linep->prl_min_addr = fl->fl_start;
	linep->prl_max_addr = fl->fl_end;
	linep->prl_line = fl->fl_line;
	linep->prl_column = fl->fl_column;
	linep->prl_srcfile = fl->fl_srcfile;

	return (0);
}
