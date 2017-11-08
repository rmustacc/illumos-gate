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
 * Copyright (c) 2017, Joyent, Inc. 
 */

#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fts.h>
#include <errno.h>
#include <strings.h>
#include <sys/debug.h>

#include <libpcsc.h>

/*
 * Implementation of the PCSC library leveraging the uccid framework.
 */

typedef struct pcsc_hdl {
	void *pcsc_foo;
} pcsc_hdl_t;


/*
 * This is called when a caller wishes to open a new Library context.
 */
LONG
SCardEstablishContext(DWORD scope, LPCVOID unused0, LPCVOID unused1,
    LPSCARDCONTEXT outp)
{
	pcsc_hdl_t *hdl;

	if (outp == NULL) {
		return (SCARD_E_INVALID_PARAMETER);
	}

	if (scope != SCARD_SCOPE_SYSTEM) {
		return (SCARD_E_INVALID_VALUE);
	}

	hdl = calloc(1, sizeof (pcsc_hdl_t));
	if (hdl == NULL) {
		return (SCARD_E_NO_MEMORY);
	}

	*outp = hdl;
	return (SCARD_S_SUCCESS);
}

/*
 * This is called to free a library context from a client.
 */
LONG
SCardReleaseContext(SCARDCONTEXT hdl)
{
	free(hdl);
	return (SCARD_S_SUCCESS);
}

/*
 * This is called to release memory allocated by the library. No, it doesn't
 * make sense to take a const pointer when being given memory to free. It just
 * means we have to cast it, but remember: this isn't our API.
 */
LONG
SCardFreeMemory(SCARDCONTEXT unused, LPCVOID mem)
{
	free((void *)mem);
	return (SCARD_S_SUCCESS);
}

/*
 * This is called by a caller to get a list of readers that exist in the system.
 * If lenp is set to SCARD_AUTOALLOCATE, then we are responsible for dealing
 * with this memory.
 */
LONG
SCardListReaders(SCARDCONTEXT unused, LPCSTR groups, LPSTR bufp, LPDWORD lenp)
{
	FTS *fts;
	FTSENT *ent;
	char *const root[] = { "/dev/ccid", NULL };
	char *ubuf;
	char **readers;
	uint32_t len, ulen, npaths, nalloc, off, i;
	int ret;

	if (groups != NULL || lenp == NULL) {
		return (SCARD_E_INVALID_PARAMETER);
	}

	fts = fts_open(root, FTS_LOGICAL | FTS_NOCHDIR, NULL);
	if (fts == NULL) {
		switch (errno) {
		case ENOENT:
		case ENOTDIR:
			return (SCARD_E_NO_READERS_AVAILABLE);
		case ENOMEM:
		case EAGAIN:
			return (SCARD_E_NO_MEMORY);
		default:
			return (SCARD_E_NO_SERVICE);
		}
	}

	npaths = nalloc = 0;
	/*
	 * Account for the NUL we'll have to place at the end of this.
	 */
	len = 1;
	readers = NULL;
	while ((ent = fts_read(fts)) != NULL) {
		size_t plen;

		if (ent->fts_level != 2 || ent->fts_info == FTS_DP)
			continue;

		if (ent->fts_info == FTS_ERR || ent->fts_info == FTS_NS)
			continue;

		if (S_ISCHR(ent->fts_statp->st_mode) == 0)
			continue;

		plen = strlen(ent->fts_path) + 1;
		if (UINT32_MAX - len <= plen) {
			/*
			 * I mean, it's true. But I wish I could just give you
			 * EOVERFLOW.
			 */
			ret = SCARD_E_INSUFFICIENT_BUFFER;
			goto out;
		}

		if (npaths == nalloc) {
			char **tmp;

			nalloc += 8;
			tmp = reallocarray(readers, nalloc, sizeof (char *));
			if (tmp == NULL) {
				ret = SCARD_E_NO_MEMORY;
				goto out;
			}
			readers = tmp;
		}
		readers[npaths] = strdup(ent->fts_path);
		npaths++;
	}

	if (npaths == 0) {
		ret = SCARD_E_NO_READERS_AVAILABLE;
		goto out;
	}

	ulen = *lenp;
	*lenp = len;
	if (ulen != SCARD_AUTOALLOCATE) {
		if (bufp == NULL) {
			ret = SCARD_S_SUCCESS;
			goto out;
		}

		if (ulen < len) {
			ret = SCARD_E_INSUFFICIENT_BUFFER;
			goto out;
		}
		
		ubuf = bufp;
	} else {
		char **bufpp;
		if (bufp == NULL) {
			ret = SCARD_E_INVALID_PARAMETER;
			goto out;
		}

		ubuf = malloc(ulen);
		if (ubuf == NULL) {
			ret = SCARD_E_NO_MEMORY;
			goto out;
		}

		bufpp = (char **)bufp;
		*bufpp = ubuf;
	}
	ret = SCARD_S_SUCCESS;

	for (off = 0, i = 0; i < npaths; i++) {
		size_t slen = strlen(readers[i]) + 1;
		bcopy(readers[i], ubuf + off, slen);
		off += slen;
		VERIFY3U(off, <=, len);
	}
	VERIFY3U(off, ==, len - 1);
	ubuf[off] = '\0';
out:
	for (i = 0; i < npaths; i++) {
		free(readers[i]);
	}
	free(readers);
	(void) fts_close(fts);
	return (ret);
}
