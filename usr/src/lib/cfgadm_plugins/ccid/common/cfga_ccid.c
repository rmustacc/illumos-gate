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
 * CCID cfgadm plugin
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include <sys/usb/clients/ccid/uccid.h>

#define	CFGA_PLUGIN_LIB
#include <config_admin.h>

int cfga_version = CFGA_HSL_V2;

static cfga_err_t
cfga_ccid_error(cfga_err_t err, char **errp, const char *fmt, ...)
{
	va_list ap;

	if (errp == NULL)
		return (err);

	/*
	 * Try to format a string. However because we have to return allocated
	 * memory, if this fails, then we have no error.
	 */
	va_start(ap, fmt);
	(void) vasprintf(errp, fmt, ap); 
	va_end(ap);

	return (err);
}

cfga_err_t
cfga_change_state(cfga_cmd_t cmd, const char *ap, const char *opts,
    struct cfga_confirm *confp, struct cfga_msg *msgp, char **errp,
    cfga_flags_t flags)
{
	return (CFGA_OPNOTSUPP);
}

cfga_err_t
cfga_private_func(const char *function, const char *ap, const char *opts,
    struct cfga_confirm *confp, struct cfga_msg *msgp, char **errp,
    cfga_flags_t flags)
{
	return (CFGA_OPNOTSUPP);
}

/*
 * We don't support the test entry point for CCID.
 */
cfga_err_t
cfga_test(const char *ap, const char *opts, struct cfga_msg *msgp, char **errp,
    cfga_flags_t flags)
{
	(void) cfga_help(msgp, opts, flags);
	return (CFGA_OPNOTSUPP);
}

static void
cfga_ccid_fill_info(const uccid_cmd_status_t *ucs, char *buf, size_t len)
{
	const char *product, *serial, *tran, *prot;
	uint_t bits = CCID_CLASS_F_TPDU_XCHG | CCID_CLASS_F_SHORT_APDU_XCHG |
	    CCID_CLASS_F_EXT_APDU_XCHG;

	if ((ucs->ucs_status & UCCID_STATUS_F_PRODUCT_VALID) != NULL) {
		product = ucs->ucs_product;
	} else {
		product = "<unknown>";
	}

	if ((ucs->ucs_status & UCCID_STATUS_F_SERIAL_VALID) != NULL) {
		serial = ucs->ucs_serial;
	} else {
		serial = "<unknown>";
	}

	switch (ucs->ucs_class.ccd_dwFeatures & bits) {
	case 0:
		tran = "Character";
		break;
	case CCID_CLASS_F_TPDU_XCHG:
		tran = "TPDU";
		break;
	case CCID_CLASS_F_SHORT_APDU_XCHG:
	case CCID_CLASS_F_EXT_APDU_XCHG:
		tran = "APDU";
		break;
	default:
		tran = "Unknown";
		break;
	}

	if ((ucs->ucs_status & UCCID_STATUS_F_PARAMS_VALID) != 0) {
		switch (ucs->ucs_prot) {
		case UCCID_PROT_T0:
			prot = " (T=0)";
			break;
		case UCCID_PROT_T1:
			prot = " (T=1)";
			break;
		default:
			prot = "<unknown>";
			break;
		}
	} else {
		prot = "<unknown>";
	}

	if ((ucs->ucs_status & UCCID_STATUS_F_CARD_ACTIVE) != 0) {
		(void) snprintf(buf, len, "Product: %s Serial: %s "
		    "Transport: %s Protocol: %s", product, serial,
		    tran, prot);
	} else {
		(void) snprintf(buf, len, "Product: %s Serial: %s ",
		    product, serial);
	}
}

cfga_err_t
cfga_list_ext(const char *ap, struct cfga_list_data **ap_list, int *nlist,
    const char *opts, const char *listopts, char **errp, cfga_flags_t flags)
{
	int fd;
	uccid_cmd_status_t ucs;
	struct cfga_list_data *cld;

	if (errp != NULL) {
		*errp = NULL;
	}

	if (opts != NULL) {
		return (cfga_ccid_error(CFGA_ERROR, errp, "hardware specific options are not supported"));
	}

	if ((fd = open(ap, O_RDWR)) < 0) {
		return (cfga_ccid_error(CFGA_LIB_ERROR, errp, "failed to open %s: %s", ap, strerror(errno)));
	}

	bzero(&ucs, sizeof (ucs));
	ucs.ucs_version = UCCID_VERSION_ONE;

	if (ioctl(fd, UCCID_CMD_STATUS, &ucs) != 0) {
		if (errno == ENODEV) {
			return (cfga_ccid_error(CFGA_LIB_ERROR, errp,
			    "ap %s going away", ap));
		}
		return (cfga_ccid_error(CFGA_ERROR, errp,
		    "ioctl on ap %s failed: %s", ap, strerror(errno)));
	}

	if ((cld = calloc(1, sizeof (*cld))) == NULL) {
		return (cfga_ccid_error(CFGA_LIB_ERROR, errp, "failed to "
		    "allocate memory for list entry"));
	}

	if (snprintf(cld->ap_log_id, sizeof (cld->ap_log_id), "ccid%d/slot%u",
	    ucs.ucs_instance, ucs.ucs_slot) >= sizeof (cld->ap_log_id)) {
		free(cld);
		return (cfga_ccid_error(CFGA_LIB_ERROR, errp, "ap %s logical id "
		    "was too large", ap));
	}

	if (strlcpy(cld->ap_phys_id, ap, sizeof (cld->ap_phys_id)) >=
	    sizeof (cld->ap_phys_id)) {
		free(cld);
		return (cfga_ccid_error(CFGA_LIB_ERROR, errp, "ap %s physical id was too long", ap));
	}

	cld->ap_class[0] = '\0';

	if ((ucs.ucs_status & UCCID_STATUS_F_CARD_PRESENT) != 0) {
		cld->ap_r_state = CFGA_STAT_CONNECTED;
		if ((ucs.ucs_status & UCCID_STATUS_F_CARD_ACTIVE) != 0) {
			cld->ap_o_state = CFGA_STAT_CONFIGURED;
		} else {
			cld->ap_o_state = CFGA_STAT_UNCONFIGURED;
		}
	} else {
		cld->ap_r_state = CFGA_STAT_EMPTY;
		cld->ap_o_state = CFGA_STAT_UNCONFIGURED;
	}

	/*
	 * XXX We should probably have a way to indicate that there's an error
	 * when the ICC is basically foobar'd. We should also allow the status
	 * ioctl to know that the slot is resetting or something else is going
	 * on I guess.
	 */
	cld->ap_cond = CFGA_COND_OK;
	cld->ap_busy = 0;
	cld->ap_status_time = (time_t)-1;
	cfga_ccid_fill_info(&ucs, cld->ap_info, sizeof (cld->ap_info));
	if (strlcpy(cld->ap_type, "icc", sizeof (cld->ap_type)) >= sizeof (cld->ap_type)) {
		free(cld);
		return (cfga_ccid_error(CFGA_LIB_ERROR, errp,
		    "ap %s type overflowed ICC field", ap));
	}

	*ap_list = cld;
	*nlist = 1;
	return (CFGA_OK);
}

cfga_err_t
cfga_help(struct cfga_msg *msgp, const char *opts, cfga_flags_t flags)
{
	(*msgp->message_routine)(msgp, "CCID specific commands:\n");
	(*msgp->message_routine)(msgp, " cfgadm -c [configure|unconfigure] ap_id [ap_id...]\n");
	(*msgp->message_routine)(msgp, " cfgadm -x warm_reset ap_id [ap_id...]\n");

	return (CFGA_OK);
}

int
cfga_ap_id_cmp(const cfga_ap_log_id_t ap_id1, const cfga_ap_log_id_t ap_id2)
{
	return (strcmp(ap_id1, ap_id2));
}
