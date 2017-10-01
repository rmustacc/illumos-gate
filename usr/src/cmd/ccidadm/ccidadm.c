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

/*
 * Print out information about a CCID device.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <err.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include <ofmt.h>
#include <libgen.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

#include <sys/usb/clients/ccid/uccid.h>
#include <atr.h>

#define	EXIT_USAGE	2

static const char *ccidadm_pname;

#define	CCID_ROOT	"/dev/ccid/"

typedef enum {
	CCIDADM_LIST_DEVICE,
	CCIDADM_LIST_PRODUCT,
	CCIDADM_LIST_STATE
} ccidadm_list_index_t;

typedef struct ccid_list_ofmt_arg {
	struct dirent		*cloa_ccid;
	struct dirent		*cloa_slot;
	uccid_cmd_status_t	*cloa_status;
	char			*cloa_product;
} ccid_list_ofmt_arg_t;

static void
ccid_list_slot_status_str(uccid_cmd_status_t *ucs, char *buf, uint_t buflen)
{
	if (!(ucs->ucs_status & UCCID_STATUS_F_CARD_PRESENT)) {
		(void) snprintf(buf, buflen, "missing");
		return;
	}

	if (ucs->ucs_status & UCCID_STATUS_F_CARD_ACTIVE) {
		(void) snprintf(buf, buflen, "activated");
		return;
	}

	(void) snprintf(buf, buflen, "unactivated");
}

static boolean_t
ccidadm_list_ofmt_cb(ofmt_arg_t *ofmt, char *buf, uint_t buflen)
{
	ccid_list_ofmt_arg_t *cloa = ofmt->ofmt_cbarg;

	switch (ofmt->ofmt_id) {
	case CCIDADM_LIST_DEVICE:
		if (snprintf(buf, buflen, "%s/%s", cloa->cloa_ccid->d_name,
		    cloa->cloa_slot->d_name) >= buflen) {
			return (B_FALSE);
		}
		break;
	case CCIDADM_LIST_PRODUCT:
		if (snprintf(buf, buflen, "%s", cloa->cloa_product) >=
		    buflen) {
			return (B_FALSE);
		}
		break;
	case CCIDADM_LIST_STATE:
		ccid_list_slot_status_str(cloa->cloa_status, buf, buflen);
		break;
	default:
		return (B_FALSE);
	}

	return (B_TRUE);
}

static char *
ccidadm_list_slot_product(int slotfd)
{
	uccid_cmd_getbuf_t ucg;
	char *buf;

	bzero(&ucg, sizeof (ucg));
	ucg.ucg_version = UCCID_CURRENT_VERSION;
	ucg.ucg_buflen = 0;

	(void) ioctl(slotfd, UCCID_CMD_GETPRODSTR, &ucg);
	switch (errno) {
	case ENOENT:
		return (strdup("<unknown>"));
	case EOVERFLOW:
		break;
	default:
		return (NULL);
	}

	buf = malloc(ucg.ucg_buflen);
	ucg.ucg_buffer = buf;
	if (ioctl(slotfd, UCCID_CMD_GETPRODSTR, &ucg) == -1) {
		free(buf);
		return (NULL);
	}

	return (buf);
}

static void
ccidadm_list_slot(ofmt_handle_t ofmt, int ccidfd, struct dirent *cciddir, struct dirent *slotdir)
{
	int slotfd;
	uccid_cmd_status_t ucs;
	ccid_list_ofmt_arg_t cloa;

	if ((slotfd = openat(ccidfd, slotdir->d_name, O_RDWR)) < 0) {
		err(EXIT_FAILURE, "failed to open cccid slot %s/%s",
		    cciddir->d_name, slotdir->d_name);
	}

	bzero(&ucs, sizeof (ucs));
	ucs.ucs_version = UCCID_CURRENT_VERSION;

	if (ioctl(slotfd, UCCID_CMD_STATUS, &ucs) != 0) {
		err(EXIT_FAILURE, "failed to issue status ioctl to %s/%s",
		    cciddir->d_name, slotdir->d_name);
	}
	cloa.cloa_ccid = cciddir;
	cloa.cloa_slot = slotdir;
	cloa.cloa_status = &ucs;
	if ((cloa.cloa_product = ccidadm_list_slot_product(slotfd)) == NULL) {
		err(EXIT_FAILURE, "failed to retrieve product string from "
		    "%s/%s", cciddir->d_name, slotdir->d_name);
	}

	ofmt_print(ofmt, &cloa);
	free(cloa.cloa_product);
	(void) close(slotfd);
}

static void
ccidadm_list_ccid(ofmt_handle_t ofmt, int dirfd, struct dirent *ccidd)
{
	int ccidfd;
	DIR *instdir;
	struct dirent *d;

	if ((ccidfd = openat(dirfd, ccidd->d_name, O_RDONLY)) < 0) {
		err(EXIT_FAILURE, "failed to open ccid %s", ccidd->d_name);
	}

	if ((instdir = fdopendir(ccidfd)) == NULL) {
		err(EXIT_FAILURE, "failed to open ccid %s", ccidd->d_name);
	}

	while ((d = readdir(instdir)) != NULL) {
		if (strcmp(".", d->d_name) == 0 || strcmp("..", d->d_name) == 0)
			continue;
		ccidadm_list_slot(ofmt, ccidfd, ccidd, d);
	}

	closedir(instdir);
}

static ofmt_field_t ccidadm_list_fields[] = {
	{ "PRODUCT",	24,	CCIDADM_LIST_PRODUCT,	ccidadm_list_ofmt_cb },
	{ "DEVICE",	16,	CCIDADM_LIST_DEVICE, 	ccidadm_list_ofmt_cb },
	{ "CARD STATE",	12,	CCIDADM_LIST_STATE, 	ccidadm_list_ofmt_cb },
	{ NULL,		0,	0,			NULL	}
};

static void
ccidadm_do_list(int argc, char *argv[])
{
	int fd;
	DIR *cr;
	struct dirent *d;
	ofmt_handle_t ofmt;

	if ((fd = open(CCID_ROOT, O_RDONLY)) < 0) {
		err(EXIT_FAILURE, "failed to open %s", CCID_ROOT);
	}

	cr = fdopendir(fd);
	if (cr == NULL) {
		err(EXIT_FAILURE, "failed to open %s", CCID_ROOT);
	}

	if (ofmt_open(NULL, ccidadm_list_fields, 0, 0, &ofmt) != OFMT_SUCCESS) {
		errx(EXIT_FAILURE, "failed to initialize ofmt state");
	}

	while ((d = readdir(cr)) != NULL) {
		if (strcmp(".", d->d_name) == 0 || strcmp("..", d->d_name) == 0)
			continue;
		ccidadm_list_ccid(ofmt, fd, d);
	}

	closedir(cr);
	ofmt_close(ofmt);
}

static void
ccidadm_list_usage(FILE *out)
{
	(void) fprintf(out, "\tlist\n");
}

static void
ccidadm_atr_hexdump(const uint8_t *buf, size_t nbytes)
{
	size_t i;
	static boolean_t first = B_TRUE;

	if (first) {
		(void) printf("%*s    0", 4, "");
		for (i = 1; i < 16; i++) {
			if (i % 4 == 0 && i % 16 != 0) {
				(void) printf(" ");
			}

			(void) printf("%2x", i);
		}
		(void) printf("  v123456789abcdef\n");
		first = B_FALSE;
	}
	for (i = 0; i < nbytes; i++) {

		if (i % 16 == 0) {
			(void) printf("%04x:  ", i);
		}

		if (i % 4 == 0 && i % 16 != 0) {
			(void) printf(" ");
		}


		(void) printf("%02x", buf[i]);

		if (i % 16 == 15) {
			int j;
			(void) printf("  ");
			for (j = i - (i % 16); j <= i; j++) {
				if (!isprint(buf[j])) {
					(void) printf(".");
				} else {
					(void) printf("%c", buf[j]);
				}
			}
			(void) printf("\n");
		}
	}

	if (i % 16 != 0) {
		size_t last = i;
		int j;
		for (i = (last % 16); i <= 16; i++) {
			if (i % 4 == 0 && i % 16 != 0) {
				(void) printf(" ");
			}

			(void) printf("  ");
		}
		for (j = last - (last % 16); j < last; j++) {
			if (!isprint(buf[j])) {
				(void) printf(".");
			} else {
				(void) printf("%c", buf[j]);
			}

		}
		(void) printf("\n");
	}
}

static void
ccidadm_atr_print(const uint8_t *buf, size_t len)
{
	int ret;
	atr_data_t *data;

	if ((data = atr_data_alloc()) == NULL) {
		err(EXIT_FAILURE, "failed to allocate memory for "
		    "ATR data");
	}

	ret = atr_parse(buf, len, data);
	printf("parse results: %s (%d)\n", atr_strerror(ret), ret);

	atr_data_free(data);
}

static void
ccidadm_atr_fetch(int fd, const char *name)
{
	uccid_cmd_getbuf_t ucg;
	uint8_t *buf;

	bzero(&ucg, sizeof (ucg));
	ucg.ucg_version = UCCID_CURRENT_VERSION;
	ucg.ucg_buflen = 0;

	/*
	 * This will fail.
	 */
	(void) ioctl(fd, UCCID_CMD_GETATR, &ucg);
	switch (errno) {
	case ENXIO:
		warn("slot %s has no card inserted or activated", name);
		return;
	case EOVERFLOW:
		break;
	default:
		err(EXIT_FAILURE, "failed to get ATR from %s", name);
	}

	buf = malloc(ucg.ucg_buflen);
	ucg.ucg_buffer = buf;

	if (ioctl(fd, UCCID_CMD_GETATR, &ucg) == -1) {
		switch (errno) {
		case ENXIO:
			warn("slot %s has no card inserted or activated", name);
			return;
		default:
			err(EXIT_FAILURE, "failed to get ATR from %s", name);
		}
	}

	(void) printf("ATR for %s (%u bytes)\n", name, ucg.ucg_buflen);
	ccidadm_atr_hexdump(ucg.ucg_buffer, ucg.ucg_buflen);
	ccidadm_atr_print(ucg.ucg_buffer, ucg.ucg_buflen);
	free(buf);
}

static void
ccidadm_do_atr(int argc, char *argv[])
{
	uint_t i;

	if (argc < 1) {
		errx(EXIT_USAGE, "missing device name");
	}

	for (i = 0; i < argc; i++) {
		int fd;
		char path[PATH_MAX];

		(void) snprintf(path, sizeof (path), "%s/%s", CCID_ROOT,
		    argv[i]);
		if ((fd = open(path, O_RDWR)) < 0) {
			warn("failed to open %s", argv[i]);
			errx(EXIT_FAILURE, "valid CCID slot?");
		}

		ccidadm_atr_fetch(fd, argv[i]);
		(void) close(fd);
	}
}

static void
ccidadm_atr_usage(FILE *out)
{
	(void) fprintf(stderr, "\tatr\tdevice ...\n");
}

typedef struct ccidadm_cmdtab {
	const char *cc_name;
	void (*cc_op)(int, char *[]);
	void (*cc_usage)(FILE *);
} ccidadm_cmdtab_t;

static ccidadm_cmdtab_t ccidadm_cmds[] = {
	{ "list", ccidadm_do_list, ccidadm_list_usage },
	{ "atr", ccidadm_do_atr, ccidadm_atr_usage },
	{ NULL }
};

static int
ccidadm_usage(const char *format, ...)
{
	ccidadm_cmdtab_t *tab;

	if (format != NULL) {
		va_list ap;

		va_start(ap, format);
		(void) fprintf(stderr, "%s: ", ccidadm_pname);
		(void) vfprintf(stderr, format, ap);
		(void) fprintf(stderr, "\n");
		va_end(ap);
	}

	(void) fprintf(stderr, "usage:  %s <subcommand> <args> ...\n\n", ccidadm_pname);
	(void) fprintf(stderr, "Subcommands:\n");
	for (tab = ccidadm_cmds; tab->cc_name != NULL; tab++) {
		tab->cc_usage(stderr);
	}

	return (EXIT_USAGE);
}

int
main(int argc, char *argv[])
{
	ccidadm_cmdtab_t *tab;

	ccidadm_pname = basename(argv[0]);
	if (argc < 2) {
		return (ccidadm_usage("missing required subcommand"));
	}

	for (tab = ccidadm_cmds; tab->cc_name != NULL; tab++) {
		if (strcmp(argv[1], tab->cc_name) == 0) {
			argc -= 2;
			argv += 2;
			tab->cc_op(argc, argv);
			return (EXIT_SUCCESS);
		}
	}

	return (ccidadm_usage("unknown command: %s", argv[1]));
}
