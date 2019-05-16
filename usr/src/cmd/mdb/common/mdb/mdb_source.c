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
 * This implements logic inside of mdb to add support for dispalying and mapping
 * addresses to source file information.
 */

#include <fcntl.h>
#include <inttypes.h>
#include <mdb/mdb_modapi.h>
#include <mdb/mdb.h>
#include <limits.h>

#ifdef	_KMDB
#error	"kmdb is not supported"
#endif

#define	SOURCE_LINEWIDTH	(64 * 1024)
#define	SOURCE_MAXCONTEXT	(1024 * 1024)

typedef struct source_context {
	boolean_t	sc_valid;
	uint64_t	sc_lineno;
	char		sc_buf[SOURCE_LINEWIDTH];
} source_context_t;

static int
source_display(mdb_iob_t *iob, uint64_t line, uint64_t col, uint_t ncontext)
{
	source_context_t *ctxp;
	uint_t i, curctxt, nctxt = ncontext * 2 + 1, firstindex, lastindex;
	uint_t ndigits;
	uint64_t firstline, curline, toread = line + ncontext, t;

	ctxp = mdb_zalloc(sizeof (source_context_t) * nctxt, UM_GC |
	    UM_NOSLEEP);

	for (curctxt = 0, curline = 1; curline <= toread; curline++,
	    curctxt = (curctxt + 1) % nctxt) {
		if ((mdb_iob_getflags(iob) & (MDB_IOB_EOF | MDB_IOB_ERR)) != 0)
			break;
		if (mdb_iob_ngets(iob, ctxp[curctxt].sc_buf,
		    SOURCE_LINEWIDTH) == EOF) {
			break;
		}
		ctxp[curctxt].sc_valid = B_TRUE;
		ctxp[curctxt].sc_lineno = curline;
	}

	if (curline < line) {
		mdb_warn("failed to read file to find line %" PRIu64 ", read "
		    "%" PRIu64 "lines\n", line, curline);
		return (DCMD_ERR);
	}

	/*
	 * Determine how many lines we need to print based on where we ended up
	 * at. Start with the first valid entry with the lowest line number.
	 */
	firstline = UINT64_MAX;
	for (i = 0; i < nctxt; i++) {
		if (!ctxp[i].sc_valid) {
			continue;
		}
		if (ctxp[i].sc_lineno < firstline) {
			firstline = ctxp[i].sc_lineno;
			firstindex = i;
		}
	}

	if (firstline == UINT64_MAX) {
		mdb_warn("somehow found no valid source lines!\n");
		return (DCMD_ERR);
	}

	/*
	 * Determine the last index by finding the last valid entry starting
	 * from firstindex.
	 */
	if (firstindex == 0) {
		lastindex = nctxt - 1;
	} else {
		lastindex = firstindex - 1;
	}
	while (!ctxp[lastindex].sc_valid) {
		if (lastindex == 0) {
			lastindex = nctxt - 1;
		} else {
			lastindex = lastindex - 1;
		}
	}

	t = ctxp[lastindex].sc_lineno;
	ndigits = 0;
	while (t > 0) {
		ndigits++;
		t /= 10;
	}

	for (i = firstindex; ; i = (i + 1) % nctxt) {
		if (!ctxp[i].sc_valid)
			break;
		if (ctxp[i].sc_lineno == line) {
			mdb_printf("%<b>");
		}
		mdb_printf("%*" PRIu64 " %s\n", ndigits, ctxp[i].sc_lineno,
		    ctxp[i].sc_buf);
		if (ctxp[i].sc_lineno == line) {
			mdb_printf("%</b>");
		}
		if (i == lastindex)
			break;
	}

	return (DCMD_OK);
}

int
cmd_source(uintptr_t addr, uint_t flags, int argc, const mdb_arg_t *argv)
{
	int fd, ret;
	mdb_io_t *fio;
	mdb_iob_t *iob;
	mdb_lineinfo_t info;
	uint64_t ncontext = 3;
	const char *dir = NULL, *parg;
	char path[PATH_MAX];

	if ((flags & DCMD_ADDRSPEC) == 0) {
		mdb_warn("::source requiers an address\n");
		return (DCMD_USAGE);
	}

	if (mdb_getopts(argc, argv,
	    'd', MDB_OPT_STR, &dir,
	    'n', MDB_OPT_UINT64, &ncontext,
	    NULL) != argc) {
		return (DCMD_USAGE);
	}

	if (ncontext > SOURCE_MAXCONTEXT) {
		mdb_warn("requested amount of context lines exceeds the max\n");
		return (DCMD_ERR);
	}

	if (mdb_tgt_addr_to_lineinfo(mdb.m_target, addr, &info) != 0) {
		mdb_warn("failed to look up source information");
		return (DCMD_ERR);
	}

	if (info.ml_file == NULL) {
		mdb_warn("debugging information did not provide a valid "
		    "file name\n");
		return (DCMD_ERR);
	}

	if (dir != NULL) {
		if (mdb_snprintf(path, sizeof (path), "%s/%s", dir,
		    info.ml_file) >= sizeof (path)) {
			mdb_warn("path expansion with -D would overflow "
			    "internal buffer\n");
			return (DCMD_ERR);
		}
		parg = path;
	} else {
		parg = info.ml_file;
	}

	fio = mdb_fdio_create_path(NULL, parg, O_RDONLY, 0);
	if (fio == NULL) {
		mdb_warn("failed to open source file %s", info.ml_file);
		return (DCMD_ERR);
	}

	iob = mdb_iob_create(fio, MDB_IOB_RDONLY);
	ret = source_display(iob, info.ml_line, info.ml_column,
	    (uint_t)ncontext);
	mdb_iob_destroy(iob);
	return (ret);
}

void
cmd_source_help(void)
{
	mdb_printf("write me!\n");
}
