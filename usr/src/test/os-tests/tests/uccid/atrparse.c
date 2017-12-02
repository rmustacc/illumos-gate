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
 * Verify that we can grab a basic exclusive lock through an ioctl on the slot.
 * Then that we can release it afterwards.
 */

#include <err.h>
#include <stdlib.h>

#include <atr.h>

typedef struct atr_vals {
} atr_vals_t;

typedef struct atr_test {
	const char *ar_test;
	uint8_t ar_len;
	uint8_t ar_buf[64];
	atr_parsecode_t ar_retval;
	/* Everything after this is data from the ATR */
	atr_protocol_t ar_sup;
	atr_protocol_t ar_def;
	boolean_t ar_neg;
	uint8_t ar_fi;
	uint8_t ar_di;
	atr_convention_t ar_conv;
	uint8_t ar_guard;
	atr_clock_stop_t ar_stop;
	/* These will be checked based on sup prot */
	uint8_t ar_t0_wi;
	atr_t1_checksum_t  ar_t1_cksum;
	uint8_t ar_t1_bwi;
	uint8_t ar_t1_cwi;
	uint8_t ar_t1_ifsc;
} atr_test_t;

atr_test_t atr_tests[] = {
	{ "zero-length data", 0, { 0 }, ATR_CODE_TOO_SHORT },
	{ "No T0", 1, { 0x3f }, ATR_CODE_TOO_SHORT },
	{ "Too much data", 34, { 0 }, ATR_CODE_TOO_LONG },
	{ "Overrun T0 (1)", 2, { 0x3b, 0x10 }, ATR_CODE_OVERRUN },
	{ "Overrun T0 (2)", 2, { 0x3b, 0x80 }, ATR_CODE_OVERRUN },
	{ "Overrun T0 (3)", 2, { 0x3b, 0x01 }, ATR_CODE_OVERRUN },
	{ "Overrun T0 (4)", 2, { 0x3b, 0x11 }, ATR_CODE_OVERRUN },
	{ "Overrun T0 (5)", 2, { 0x3b, 0xff }, ATR_CODE_OVERRUN },
	{ "Overrun TD1", 3, { 0x3b, 0x80, 0x10 }, ATR_CODE_OVERRUN },
	{ "Overrun TD2", 4, { 0x3b, 0x80, 0x80, 0x10 }, ATR_CODE_OVERRUN },
	{ "Overrun TD", 33, { 0x3b, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
	    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
	    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
	    0x80, 0x80, 0x80 }, ATR_CODE_OVERRUN },
	{ "T0 w/ T=15 and no cksum", 5, { 0x3b, 0x80, 0x80, 0x1f, 0x00 },
	    ATR_CODE_OVERRUN },
	{ "Bad TS (1)", 2, { 0x3a, 0x00 }, ATR_CODE_INVALID_TS },
	{ "Bad TS (2)", 2, { 0xff, 0x00 }, ATR_CODE_INVALID_TS },
	{ "T0 w/ T=15 and bad cksum", 6, { 0x3b, 0x80, 0x80, 0x1f, 0x00, 0x00 },
	    ATR_CODE_CHECKSUM_ERROR },
	{ "T0 w/ T=15 and bad cksum (make sure no TS)", 6, { 0x3b, 0x80, 0x80, 0x1f, 0x00, 0x24 },
	    ATR_CODE_CHECKSUM_ERROR },
	{ "T=15 in TD1", 4, { 0x3b, 0x80, 0x0f, 0x8f }, ATR_CODE_INVALID_TD1 },
	{
		.ar_test = "Minimal T0 Direct",
		.ar_len = 2,
		.ar_buf = { 0x3b, 0x00 },
		.ar_sup = ATR_P_T0,
		.ar_def = ATR_P_T0,
		.ar_neg = B_TRUE,
		.ar_fi = 1,
		.ar_di = 1,
		.ar_conv = ATR_CONVENTION_DIRECT,
		.ar_guard = 0,
		.ar_stop = ATR_CLOCK_STOP_NONE,
		.ar_t0_wi = 10,
	}, {
		.ar_test = "Minimal T0 Inverse",
		.ar_len = 2,
		.ar_buf = { 0x3f, 0x00 },
		.ar_sup = ATR_P_T0,
		.ar_def = ATR_P_T0,
		.ar_neg = B_TRUE,
		.ar_fi = 1,
		.ar_di = 1,
		.ar_conv = ATR_CONVENTION_INVERSE,
		.ar_guard = 0,
		.ar_stop = ATR_CLOCK_STOP_NONE,
		.ar_t0_wi = 10,
	}, {
		.ar_test = "T0 Fi/Di (1)",
		.ar_len = 3,
		.ar_buf = { 0x3b, 0x10, 0x24 },
		.ar_sup = ATR_P_T0,
		.ar_def = ATR_P_T0,
		.ar_neg = B_TRUE,
		.ar_fi = 2,
		.ar_di = 4,
		.ar_conv = ATR_CONVENTION_DIRECT,
		.ar_guard = 0,
		.ar_stop = ATR_CLOCK_STOP_NONE,
		.ar_t0_wi = 10,
	}, {
		.ar_test = "T0 Fi/Di (2)",
		.ar_len = 3,
		.ar_buf = { 0x3b, 0x10, 0x93 },
		.ar_sup = ATR_P_T0,
		.ar_def = ATR_P_T0,
		.ar_neg = B_TRUE,
		.ar_fi = 9,
		.ar_di = 3,
		.ar_conv = ATR_CONVENTION_DIRECT,
		.ar_guard = 0,
		.ar_stop = ATR_CLOCK_STOP_NONE,
		.ar_t0_wi = 10,
	}, {
		.ar_test = "T0 Ignore deprecated TB1",
		.ar_len = 3,
		.ar_buf = { 0x3b, 0x20, 0x42 },
		.ar_sup = ATR_P_T0,
		.ar_def = ATR_P_T0,
		.ar_neg = B_TRUE,
		.ar_fi = 1,
		.ar_di = 1,
		.ar_conv = ATR_CONVENTION_DIRECT,
		.ar_guard = 0,
		.ar_stop = ATR_CLOCK_STOP_NONE,
		.ar_t0_wi = 10,
	}, {
		.ar_test = "T0 Ignore deprecated TB2",
		.ar_len = 4,
		.ar_buf = { 0x3b, 0x80, 0x20, 0x42 },
		.ar_sup = ATR_P_T0,
		.ar_def = ATR_P_T0,
		.ar_neg = B_TRUE,
		.ar_fi = 1,
		.ar_di = 1,
		.ar_conv = ATR_CONVENTION_DIRECT,
		.ar_guard = 0,
		.ar_stop = ATR_CLOCK_STOP_NONE,
		.ar_t0_wi = 10,
	}, {
		.ar_test = "T0 Ignore deprecated TB1/TB2",
		.ar_len = 5,
		.ar_buf = { 0x3b, 0xa0, 0x55, 0x20, 0x42 },
		.ar_sup = ATR_P_T0,
		.ar_def = ATR_P_T0,
		.ar_neg = B_TRUE,
		.ar_fi = 1,
		.ar_di = 1,
		.ar_conv = ATR_CONVENTION_DIRECT,
		.ar_guard = 0,
		.ar_stop = ATR_CLOCK_STOP_NONE,
		.ar_t0_wi = 10,
	}, {
		.ar_test = "T0 Encode TC1",
		.ar_len = 3,
		.ar_buf = { 0x3b, 0x40, 0x23 },
		.ar_sup = ATR_P_T0,
		.ar_def = ATR_P_T0,
		.ar_neg = B_TRUE,
		.ar_fi = 1,
		.ar_di = 1,
		.ar_conv = ATR_CONVENTION_DIRECT,
		.ar_guard = 0x23,
		.ar_stop = ATR_CLOCK_STOP_NONE,
		.ar_t0_wi = 10,
	}, {
		.ar_test = "T0 TA2 says neg",
		.ar_len = 4,
		.ar_buf = { 0x3b, 0x80, 0x10, 0x00 },
		.ar_sup = ATR_P_T0,
		.ar_def = ATR_P_T0,
		.ar_neg = B_TRUE,
		.ar_fi = 1,
		.ar_di = 1,
		.ar_conv = ATR_CONVENTION_DIRECT,
		.ar_guard = 0,
		.ar_stop = ATR_CLOCK_STOP_NONE,
		.ar_t0_wi = 10,
	}, {
		.ar_test = "T0 TA2 says not neg",
		.ar_len = 4,
		.ar_buf = { 0x3b, 0x80, 0x10, 0x80 },
		.ar_sup = ATR_P_T0,
		.ar_def = ATR_P_T0,
		.ar_neg = B_FALSE,
		.ar_fi = 1,
		.ar_di = 1,
		.ar_conv = ATR_CONVENTION_DIRECT,
		.ar_guard = 0,
		.ar_stop = ATR_CLOCK_STOP_NONE,
		.ar_t0_wi = 10,
	}, {
		.ar_test = "T0 TA2 says not neg, honor Fi/Di",
		.ar_len = 5,
		.ar_buf = { 0x3b, 0x90, 0x24, 0x10, 0x80 },
		.ar_sup = ATR_P_T0,
		.ar_def = ATR_P_T0,
		.ar_neg = B_FALSE,
		.ar_fi = 2,
		.ar_di = 4,
		.ar_conv = ATR_CONVENTION_DIRECT,
		.ar_guard = 0,
		.ar_stop = ATR_CLOCK_STOP_NONE,
		.ar_t0_wi = 10,
	}, {
		.ar_test = "T0 TA2 says not neg, don't honor Fi/Di",
		.ar_len = 5,
		.ar_buf = { 0x3b, 0x90, 0x24, 0x10, 0x90 },
		.ar_sup = ATR_P_T0,
		.ar_def = ATR_P_T0,
		.ar_neg = B_FALSE,
		.ar_fi = 1,
		.ar_di = 1,
		.ar_conv = ATR_CONVENTION_DIRECT,
		.ar_guard = 0,
		.ar_stop = ATR_CLOCK_STOP_NONE,
		.ar_t0_wi = 10,
	}, {
		.ar_test = "T0 TC2 set",
		.ar_len = 4,
		.ar_buf = { 0x3b, 0x80, 0x40, 0x35 },
		.ar_sup = ATR_P_T0,
		.ar_def = ATR_P_T0,
		.ar_neg = B_TRUE,
		.ar_fi = 1,
		.ar_di = 1,
		.ar_conv = ATR_CONVENTION_DIRECT,
		.ar_guard = 0,
		.ar_stop = ATR_CLOCK_STOP_NONE,
		.ar_t0_wi = 0x35,
	}, {
		.ar_test = "T0 T15 empty (requires checksum)",
		.ar_len = 5,
		.ar_buf = { 0x3b, 0x80, 0x80, 0x0f, 0x0f },
		.ar_sup = ATR_P_T0,
		.ar_def = ATR_P_T0,
		.ar_neg = B_TRUE,
		.ar_fi = 1,
		.ar_di = 1,
		.ar_conv = ATR_CONVENTION_DIRECT,
		.ar_guard = 0,
		.ar_stop = ATR_CLOCK_STOP_NONE,
		.ar_t0_wi = 10,
	}

};

static void
atr_parse_failed(atr_test_t *test, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void) fprintf(stderr, "Test \"%s\" failed: ", test->ar_test);
	(void) vfprintf(stderr, fmt, ap);
	(void) fprintf(stderr, "\n");
	va_end(ap);
}

static uint_t
atr_parse_one(atr_data_t *data, atr_test_t *test)
{
	uint_t err = 0;
	atr_parsecode_t ret;
	atr_protocol_t sup, def;
	boolean_t neg;
	uint8_t fi, di, guard;
	atr_convention_t conv;
	atr_clock_stop_t stop;

	ret = atr_parse(test->ar_buf, test->ar_len, data); 
	if (ret != test->ar_retval) {
		atr_parse_failed(test, "found unexpected return "
		    "value: %u (%s), expected: %u", ret, atr_strerror(ret),
		    test->ar_retval);
		return (1);
	}

	/* Don't test anything else if it's not OK */
	if (ret != ATR_CODE_OK)
		return (0);

	sup = atr_supported_protocols(data);
	def = atr_default_protocol(data);
	neg = atr_params_negotiable(data);
	fi = atr_fi_index(data);
	di = atr_di_index(data);
	conv = atr_convention(data);
	guard = atr_extra_guardtime(data);
	stop = atr_clock_stop(data);

	if (sup != test->ar_sup) {
		atr_parse_failed(test, "Found mismatched supported "
		    "protocols: %u, expected: %u", sup, test->ar_sup);
		err++;
	}

	if (def != test->ar_def) {
		atr_parse_failed(test, "Found mismatched default "
		    "protocols: %u, expected: %u", def, test->ar_def);
		err++;
	}

	if (neg != test->ar_neg) {
		atr_parse_failed(test, "Found mismatched negotiable bit: "
		   "%u, expected %u", neg, test->ar_neg);
	       err++;	
	}

	if (fi != test->ar_fi) {
		atr_parse_failed(test, "Found mismatched fi index: "
		    "%u, expected: %u", fi, test->ar_fi);
		err++;
	}

	if (di != test->ar_di) {
		atr_parse_failed(test, "Found mismatched di index: "
		    "%u, expected: %u", di, test->ar_di);
		err++;
	}

	if (conv != test->ar_conv) {
		atr_parse_failed(test, "Found mismatched TS convention: "
		    "%u, expected: %u", conv, test->ar_conv);
		err++;
	}

	if (guard != test->ar_guard) {
		atr_parse_failed(test, "Found mismatched extra guardtime: "
		    "%u, expected: %u", guard, test->ar_guard);
		err++;
	}

	if (stop != test->ar_stop) {
		atr_parse_failed(test, "Found mismatched clock stop: "
		    "%u, expected: %u", stop, test->ar_stop);
		err++;
	}

	if ((sup & ATR_P_T0) != 0) {
		uint8_t wi;

		wi = atr_t0_wi(data);
		if (wi != test->ar_t0_wi) {
			atr_parse_failed(test, "Found mismatched T0 wi: "
			    "%u, expected: %u", wi, test->ar_t0_wi);
			err++;
		}
	}

	if ((sup & ATR_P_T1) != 0) {
		atr_t1_checksum_t cksum;
		uint8_t bwi, cwi, ifsc;

		cksum = atr_t1_checksum(data);
		bwi = atr_t1_bwi(data);
		cwi = atr_t1_cwi(data);
		ifsc = atr_t1_ifsc(data);

		if (cksum != test->ar_t1_cksum) {
			atr_parse_failed(test, "Found mistmatched T1 checksum: "
			    "%u, expected: %u", cksum, test->ar_t1_cksum);
			err++;
		}

		if (bwi != test->ar_t1_bwi) {
			atr_parse_failed(test, "Found mistmatched T1 bwi: "
			    "%u, expected: %u", bwi, test->ar_t1_bwi);
			err++;
		}

		if (cwi != test->ar_t1_cwi) {
			atr_parse_failed(test, "Found mistmatched T1 cwi: "
			    "%u, expected: %u", cwi, test->ar_t1_cwi);
			err++;
		}

		if (ifsc != test->ar_t1_ifsc) {
			atr_parse_failed(test, "Found mistmatched T1 ifsc: "
			    "%u, expected: %u", ifsc, test->ar_t1_ifsc);
			err++;
		}
	}

	if (err > 0) {
		atr_data_dump(data, stderr);
		return (1);
	}

	return (0);
}

int
main(void)
{
	uint_t i;
	uint_t errs = 0;
	atr_data_t *data;

	data = atr_data_alloc();
	if (data == NULL) {
		errx(EXIT_FAILURE, "failed to allocate atr_data_t");
	}

	for (i = 0; i < sizeof (atr_tests) / sizeof (atr_test_t); i++) {
		atr_data_reset(data);
		errs += atr_parse_one(data, &atr_tests[i]);
	}

	atr_data_free(data);

	if (errs != 0) {
		warnx("%d test(s) failed", errs);
	}
	return (errs != 0 ? EXIT_FAILURE : EXIT_SUCCESS);
}
