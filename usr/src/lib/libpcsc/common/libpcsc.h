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
 * Copyright (c) 2017 Joyent, Inc.
 */

#ifndef _LIBPCSC_H
#define	_LIBPCSC_H

/*
 * This library provides a compatability interface with programs designed
 * against the PC SmartCard Library. This originates from Microsoft and has been
 * used on a few different forms over the years by folks. The purpose of this
 * library is for compatability.
 *
 * New consumers should not use this library and instead should leverage
 * uccid(7I) instead.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * While we don't want to, this expects that we have Win32 style type names.
 * Deal with conversions between Win32 and reality. Remember that Windows is an
 * ILP32 system, but it is a LLP64 system.
 */

typedef const void *LPCVOID;
typedef uint32_t DWORD;
typedef uint32_t *LPDWORD;
typedef int32_t	LONG;
typedef char *LPSTR;
typedef const char *LPCSTR;

/*
 * This is a departure from the PCSC system which treats this as a LONG. We
 * don't, because we'd like a single value that makes sense across j
 */
typedef void *SCARDCONTEXT;
typedef void **PSCARDCONTEXT;
typedef void **LPSCARDCONTEXT;
typedef void *SCARDHANDLE;
typedef void **PSCARDHANDLE;
typedef void **LPSCARDHANDLE;

/*
 * Return values and error codes. We strive to use the same error codes as
 * Microsoft. Right now we only have the values 
 */
#define	SCARD_S_SUCCESS			0x00000000
#define	SCARD_F_INTERNAL_ERROR		0x80100001
#define	SCARD_E_CANCELLED		0x80100002
#define	SCARD_E_INVALID_HANDLE		0x80100003
#define	SCARD_E_INVALID_PARAMETER	0x80100004
#define	SCARD_E_NO_MEMORY		0x80100006
#define	SCARD_E_INSUFFICIENT_BUFFER	0x80100008
#define	SCARD_E_UNKNOWN_READER		0x80100009
#define	SCARD_E_TIMEOUT			0x8010000a
#define	SCARD_E_SHARING_VIOLATION	0x8010000b
#define	SCARD_E_NO_SMARTCARD		0x8010000c
#define	SCARD_E_UNKNOWN_CARD		0x8010000d
#define	SCARD_E_PROTO_MISMATCH		0x8010000f
#define	SCARD_E_INVALID_VALUE		0x80100011
#define	SCARD_F_COMM_ERROR		0x80100013
#define	SCARD_F_UNKNOWN_ERROR		0x80100014
#define	SCARD_E_NO_SERVICE		0x8010001D
#define	SCARD_E_UNSUPPORTED_FEATURE	0x80100022
#define	SCARD_E_NO_READERS_AVAILABLE	0x8010002E
#define	SCARD_W_UNSUPPORTED_CARD	0x80100065
#define	SCARD_W_UNPOWERED_CARD		0x80100067

#define	SCARD_SCOPE_USER		0x0000
#define	SCARD_SCOPE_TERMINAL		0x0001
#define	SCARD_SCOPE_GLOBAL		0x0002
#define	SCARD_SCOPE_SYSTEM		0x0003

#define	SCARD_SHARE_EXCLUSIVE	0x0001
#define	SCARD_SHARE_SHARED	0x0002
#define	SCARD_SHARE_DIRECT	0x0003

#define	SCARD_PROTOCOL_T0	0x0001
#define	SCARD_PROTOCOL_T1	0x0002
#define	SCARD_PROTOCOL_RAW	0x0004
#define	SCARD_PROTOCOL_T15	0x0008

#define	SCARD_LEAVE_CARD	0x0000
#define	SCARD_RESET_CARD	0x0001
#define	SCARD_UNPOWER_CARD	0x0002
#define	SCARD_EJECT_CARD	0x0003

/*
 * This is used to indicate that the framework should allocate memory.
 */
#define	SCARD_AUTOALLOCATE		UINT32_MAX

extern LONG SCardEstablishContext(DWORD, LPCVOID, LPCVOID, LPSCARDCONTEXT);
extern LONG SCardReleaseContext(SCARDCONTEXT);

extern LONG SCardListReaders(SCARDCONTEXT, LPCSTR, LPSTR, LPDWORD);

extern LONG SCardFreeMemory(SCARDCONTEXT, LPCVOID);

extern LONG SCardConnect(SCARDCONTEXT, LPCSTR, DWORD, DWORD, LPSCARDHANDLE,
    LPDWORD);
extern LONG SCardDisconnect(SCARDHANDLE, DWORD);

#ifdef __cplusplus
}
#endif

#endif /* _LIBPCSC_H */
