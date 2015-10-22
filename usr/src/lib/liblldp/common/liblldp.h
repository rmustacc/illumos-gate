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
 * Copyright 2015 Joyent, Inc.
 */

#ifndef _LIBLLDP_H
#define	_LIBLLDP_H

/*
 * Useful LLDP Functions
 */

#include <libnvpair.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * XXX Kind of want a handle that I can query for stats for parsing a given
 * frame or something to that effect.
 */

/*
 * nvlist_t output format: the '*' indicates that it's optional.
 *
 * "chassis" 	<nvlist_t>
 * 	"subtype"	<uint8_t>
 * 	"id"		<string>
 * 	"raw"		<byte array>
 *
 * "portid"	<nvlist_t>
 * 	"subtype"	<uint8_t>
 * 	"id"		<string>
 * 	"raw"		<byte array>
 *
 * "ttl"	<uint16_t>
 *
 * "portdesc"	<string>
 *
 * "sysname"	<string>
 *
 * "syscap"	<nvlist_t>
 * 	"capabilities"	<uint16_t>
 * 	"enabled"	<uint16_t>
 *
 * "mgmtaddr"	<nvlist_t>
 * 	"address"	<string>*
 * 	"raw"		<byte array>
 * 	"ifnumtype"	<uint8_t>
 * 	"ifnum"		<uint32_t>*
 * 	"oid"		<byte array>*
 *
 * The ost field is the set of organization specific TLVs. They all have a
 * defined sub-type and are indexed based on their OUI. If a type can be
 * understood by the standard lldp parserr, then additional keys under the oui
 * will be added that have specific meanings.
 *
 * "ost"	<nvlist_t>
 * 	<oui>		<nvlist_t>
 * 		<subtype>	<byte array>
 * 	<oui>		<nvlist_t>
 * 		<subtype>	<byte array>
 * 		<subtype>	<byte array>
 */

extern int lldp_frame_parse(const void *, int, nvlist_t **);

#ifdef __cplusplus
}
#endif

#endif /* _LIBLLDP_H */
