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
} ccid_list_ofmt_arg_t;

static void
ccidadm_list_slot_status_str(uccid_cmd_status_t *ucs, char *buf, uint_t buflen)
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
		if (snprintf(buf, buflen, "%s", cloa->cloa_status->ucs_product) >=
		    buflen) {
			return (B_FALSE);
		}
		break;
	case CCIDADM_LIST_STATE:
		ccidadm_list_slot_status_str(cloa->cloa_status, buf, buflen);
		break;
	default:
		return (B_FALSE);
	}

	return (B_TRUE);
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

	if ((ucs.ucs_status & UCCID_STATUS_F_PRODUCT_VALID) == 0) {
		(void) strlcpy(ucs.ucs_product, "<unknown>",
		    sizeof (ucs.ucs_product));
	}

	cloa.cloa_ccid = cciddir;
	cloa.cloa_slot = slotdir;
	cloa.cloa_status = &ucs;
	ofmt_print(ofmt, &cloa);
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

/* XXX Probably fold this back into the atr common code */
static void
ccidadm_atr_hexdump(const uint8_t *buf, size_t nbytes)
{
	size_t i;

	/* Print out the header */
	(void) printf("%*s    0", 4, "");
	for (i = 1; i < 16; i++) {
		if (i % 4 == 0 && i % 16 != 0) {
			(void) printf(" ");
		}

		(void) printf("%2x", i);
	}
	(void) printf("  0123456789abcdef\n");

	/* Print out data */
	for (i = 0; i < nbytes; i++) {

		if (i % 16 == 0) {
			(void) printf("%04x:  ", i);
		}

		if (i % 4 == 0 && i % 16 != 0) {
			(void) printf(" ");
		}

		(void) printf("%02x", buf[i]);

		if (i % 16 == 15 || i + 1 == nbytes) {
			int j;
			for (j = (i % 16); j <= 16; j++) {
				if (j % 4 == 0 && j % 16 != 0) {
					(void) printf(" ");
				}

				(void) printf("  ");
			}

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
}

/*
 * Print out logical information about the ICC's ATR. This includes information
 * about what protocols it supports, required negotiation, etc.
 */
static void
ccidadm_atr_props(uccid_cmd_status_t *ucs)
{
	int ret;
	atr_data_t *data;
	atr_protocol_t prots, defprot;
	boolean_t negotiate;

	if ((data = atr_data_alloc()) == NULL) {
		err(EXIT_FAILURE, "failed to allocate memory for "
		    "ATR data");
	}

	ret = atr_parse(ucs->ucs_atr, ucs->ucs_atrlen, data);
	if (ret != ATR_CODE_OK) {
		errx(EXIT_FAILURE, "failed to parse ATR data: %s",
		    atr_strerror(ret));
	}

	prots = atr_supported_protocols(data);
	(void) printf("ICC supports protocol(s): ");
	if (prots == ATR_P_NONE) {
		(void) printf("none\n");
		return;
	}

	(void) printf("%s\n", atr_protocol_to_string(prots));

	negotiate = atr_params_negotiable(data);
	defprot = atr_default_protocol(data);

	if (negotiate) {
		(void) printf("Card protocol is negotiable; starts with "
		    "default %s parameters\n", atr_protocol_to_string(defprot));
	} else {
		(void) printf("Card protocol is not negotiable; starts with "
		    "specific %s parameters\n", atr_protocol_to_string(defprot));
	}

	/*
	 * For each supported protocol, figure out parameters we would negoiate
	 */
	if ((ucs->ucs_hwfeatures & (CCID_CLASS_F_AUTO_PARAM_NEG |
	    CCID_CLASS_F_AUTO_PPS)) == 0) {
		(void) printf("CCID/ICC require explicit parameter/PPS negotiation\n");
	}

	if (prots & ATR_P_T0) {
		uint8_t fi, di;
		atr_convention_t conv;
		atr_clock_stop_t clock;

		fi = atr_fi_index(data);
		di = atr_di_index(data);
		conv = atr_convention(data);
		clock = atr_clock_stop(data);
		(void) printf("T=0 properties that would be negotiated:\n");
		(void) printf("  + Fi/Fmax Index: %u (Fi %s/Fmax %s MHz)\n",
		    fi, atr_fi_index_to_string(fi),
		    atr_fmax_index_to_string(fi));
		(void) printf("  + Di Index: %u (Di %s)\n", di,
		    atr_di_index_to_string(di));
		(void) printf("  + Clock Convention: %u (%s)\n", conv,
		    atr_convention_to_string(conv));
		(void) printf("  + Extra Guardtime: %u\n",
		    atr_extra_guardtime(data));
		(void) printf("  + WI: %u\n", atr_t0_wi(data));
		(void) printf("  + Clock Stop: %u (%s)\n", clock,
		    atr_clock_stop_to_string(clock));
	}

	if (prots & ATR_P_T1) {
		uint8_t fi, di;
		atr_clock_stop_t clock;

		fi = atr_fi_index(data);
		di = atr_di_index(data);
		clock = atr_clock_stop(data);
		(void) printf("T=1 properties that would be negotiated:\n");
		(void) printf("  + Fi/Fmax Index: %u (Fi %s/Fmax %s MHz)\n",
		    fi, atr_fi_index_to_string(fi),
		    atr_fmax_index_to_string(fi));
		(void) printf("  + Di Index: %u (Di %s)\n", di,
		    atr_di_index_to_string(di));
		(void) printf("  + Extra Guardtime: %u\n",
		    atr_extra_guardtime(data));
		(void) printf("  + BWI: %u\n", atr_t1_bwi(data));
		(void) printf("  + CWI: %u\n", atr_t1_cwi(data));
		(void) printf("  + Clock Stop: %u (%s)\n", clock,
		    atr_clock_stop_to_string(clock));
		(void) printf("  + IFSC: %u\n", atr_t1_ifsc(data));
		(void) printf("  + CCID Supports NAD: %s\n",
		    ucs->ucs_hwfeatures & CCID_CLASS_F_ALTNAD_SUP ?
		    "yes" : "no");
	}

	atr_data_free(data);
}

static void
ccidadm_atr_verbose(uccid_cmd_status_t *ucs)
{
	int ret;
	atr_data_t *data;

	if ((data = atr_data_alloc()) == NULL) {
		err(EXIT_FAILURE, "failed to allocate memory for "
		    "ATR data");
	}

	ret = atr_parse(ucs->ucs_atr, ucs->ucs_atrlen, data);
	if (ret != ATR_CODE_OK) {
		errx(EXIT_FAILURE, "failed to parse ATR data: %s",
		    atr_strerror(ret));
	}
	atr_data_dump(data, stdout);
	atr_data_free(data);
}

static void
ccidadm_atr_fetch(int fd, const char *name, boolean_t hex, boolean_t props,
    boolean_t verbose)
{
	uccid_cmd_status_t ucs;

	bzero(&ucs, sizeof (ucs));
	ucs.ucs_version = UCCID_CURRENT_VERSION;

	if (ioctl(fd, UCCID_CMD_STATUS, &ucs) != 0) {
		err(EXIT_FAILURE, "failed to issue status ioctl to %s",
		    name);
	}

	if (ucs.ucs_atrlen == 0) {
		warnx("slot %s has no card inserted or activated", name);
		return;
	}

	(void) printf("ATR for %s (%u bytes)\n", name, ucs.ucs_atrlen);
	if (props) {
		ccidadm_atr_props(&ucs);
	}

	if (hex) {
		ccidadm_atr_hexdump(ucs.ucs_atr, ucs.ucs_atrlen);
	}

	if (verbose) {
		ccidadm_atr_verbose(&ucs);
	}
}

static void
ccidadm_do_atr(int argc, char *argv[])
{
	uint_t i;
	int c;
	boolean_t do_verbose, do_props, do_hex;

	if (argc < 1) {
		errx(EXIT_USAGE, "missing device name");
	}

	do_verbose = do_props = do_hex = B_FALSE;
	optind = 0;
	while ((c = getopt(argc, argv, "vpx")) != -1) {
		switch (c) {
		case 'v':
			do_verbose = B_TRUE;
			break;
		case 'p':
			do_props = B_TRUE;
			break;
		case 'x':
			do_hex = B_TRUE;
			break;
		case ':':
			errx(EXIT_USAGE, "Option -%c requires an argument\n", optopt);
		case '?':
			errx(EXIT_USAGE, "Unknown option: -%c\n", optopt);
		}
	}

	if (!do_verbose && !do_props && !do_hex) {
		do_hex = B_TRUE;
	}

	argc -= optind;
	argv += optind;

	for (i = 0; i < argc; i++) {
		int fd;
		char path[PATH_MAX];

		(void) snprintf(path, sizeof (path), "%s/%s", CCID_ROOT,
		    argv[i]);
		if ((fd = open(path, O_RDWR)) < 0) {
			warn("failed to open %s", argv[i]);
			errx(EXIT_FAILURE, "valid CCID slot?");
		}

		ccidadm_atr_fetch(fd, argv[i], do_hex, do_props, do_verbose);
		(void) close(fd);
		if (i + 1 < argc) {
			(void) printf("\n");
		}
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
