/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * See the COPYRIGHT file distributed with this work for additional
 * information regarding copyright ownership.
 */

/*
 * Portions copyright (c) 2008 Nominet UK.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


/* privrsa [-m module] [-s $slot] [-p pin] [-t] [-n count] */

/*! \file */

#include <config.h>

#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <isc/commandline.h>
#include <isc/print.h>
#include <isc/result.h>
#include <isc/types.h>
#include <isc/util.h>

#include <pk11/pk11.h>
#include <pk11/result.h>

#ifndef HAVE_CLOCK_GETTIME

#include <sys/time.h>

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

static int clock_gettime(int32_t id, struct timespec *tp);

static int
clock_gettime(int32_t id, struct timespec *tp)
{
	struct timeval tv;
	int result;

	UNUSED(id);

	result = gettimeofday(&tv, NULL);
	if (result)
		return (result);
	tp->tv_sec = tv.tv_sec;
	tp->tv_nsec = (long) tv.tv_usec * 1000;
	return (result);
}
#endif

CK_BYTE modulus[] = {
	0x00, 0xb7, 0x9c, 0x1f, 0x05, 0xa3, 0xc2, 0x99,
	0x44, 0x82, 0x20, 0x78, 0x43, 0x7f, 0x5f, 0x3b,
	0x10, 0xd7, 0x9e, 0x61, 0x42, 0xd2, 0x7a, 0x90,
	0x50, 0x8a, 0x99, 0x33, 0xe7, 0xca, 0xc8, 0x5f,
	0x16, 0x1c, 0x56, 0xf8, 0xc1, 0x06, 0x2f, 0x96,
	0xe7, 0x54, 0xf2, 0x85, 0x89, 0x41, 0x36, 0xf5,
	0x4c, 0xa4, 0x0d, 0x62, 0xd3, 0x42, 0x51, 0x6b,
	0x9f, 0xdc, 0x36, 0xcb, 0xad, 0x56, 0xf4, 0xbd,
	0x2a, 0x60, 0x33, 0xb1, 0x7a, 0x99, 0xad, 0x08,
	0x9f, 0x95, 0xe8, 0xe5, 0x14, 0xd9, 0x68, 0x79,
	0xca, 0x4e, 0x72, 0xeb, 0xfb, 0x2c, 0xf1, 0x45,
	0xd3, 0x33, 0x65, 0xe7, 0xc5, 0x11, 0xdd, 0xe7,
	0x09, 0x83, 0x13, 0xd5, 0x17, 0x1b, 0xf4, 0xbd,
	0x49, 0xdd, 0x8a, 0x3c, 0x3c, 0xf7, 0xa1, 0x5d,
	0x7b, 0xb4, 0xd3, 0x80, 0x25, 0xf4, 0x05, 0x8f,
	0xbc, 0x2c, 0x2a, 0x47, 0xff, 0xd1, 0xc8, 0x34,
	0xbf
};
CK_BYTE pubexp[] = { 0x01, 0x00, 0x01 };
CK_BYTE privexp[] = {
	0x00, 0xae, 0x02, 0xf1, 0x47, 0xa8, 0x07, 0x02,
	0xb8, 0xf1, 0xd6, 0x92, 0x03, 0xee, 0x50, 0x33,
	0xab, 0x67, 0x9e, 0x3b, 0xb1, 0x57, 0xc7, 0x3e,
	0xc4, 0x86, 0x46, 0x61, 0xf1, 0xf8, 0xb6, 0x63,
	0x9f, 0x91, 0xe6, 0x3f, 0x44, 0xb8, 0x77, 0x1b,
	0xbe, 0x4c, 0x3c, 0xb8, 0x9f, 0xf7, 0x45, 0x7d,
	0xbf, 0x4f, 0xef, 0x3b, 0xcc, 0xda, 0x1a, 0x4e,
	0x34, 0xa8, 0x40, 0xea, 0x51, 0x72, 0x8a, 0xea,
	0x47, 0x06, 0x04, 0xd0, 0x62, 0x31, 0xa0, 0x6c,
	0x09, 0x60, 0xf9, 0xc7, 0x95, 0x88, 0x4a, 0xd7,
	0x19, 0xce, 0x89, 0x08, 0x87, 0x14, 0xef, 0xcc,
	0x0a, 0xef, 0x72, 0xb9, 0x21, 0xf5, 0xf0, 0xcd,
	0x6d, 0xe5, 0xfa, 0x15, 0x7f, 0xae, 0x33, 0x9f,
	0x26, 0xac, 0x2e, 0x52, 0x02, 0x07, 0xfb, 0x1d,
	0x4b, 0xec, 0x9a, 0x6b, 0x3b, 0x26, 0x1f, 0x52,
	0xfc, 0x47, 0xf8, 0x66, 0x33, 0xfa, 0x50, 0x6c,
	0x41
};
CK_BYTE prime1[] = {
	0x00, 0xe8, 0x98, 0xeb, 0xa1, 0xf0, 0xce, 0xde,
	0xc2, 0x74, 0x01, 0x18, 0x2b, 0xd3, 0x8f, 0x58,
	0xcd, 0xe9, 0x8e, 0x97, 0xbe, 0xfe, 0xe8, 0x6f,
	0xd6, 0x0c, 0x0a, 0x47, 0xf8, 0x56, 0x84, 0x36,
	0x15, 0xe6, 0x75, 0x1c, 0x69, 0x48, 0x8b, 0xf5,
	0x0f, 0x84, 0xd2, 0x60, 0x8b, 0xa2, 0x2a, 0xa1,
	0xeb, 0xed, 0xbe, 0x2d, 0xe9, 0x41, 0x0b, 0xed,
	0x17, 0x7c, 0xd3, 0xa6, 0x35, 0x6e, 0xa6, 0xd8,
	0x21
};
CK_BYTE prime2[] = {
	0x00, 0xca, 0x15, 0x6a, 0x43, 0x5e, 0x83, 0xc9,
	0x09, 0xeb, 0x14, 0x1e, 0x46, 0x46, 0x97, 0xfa,
	0xfa, 0x3c, 0x61, 0x7e, 0xc1, 0xf8, 0x8c, 0x5e,
	0xcb, 0xbf, 0xe4, 0xb9, 0x78, 0x7f, 0x4f, 0xab,
	0x82, 0x15, 0x53, 0xaa, 0x04, 0xee, 0x11, 0x21,
	0x2e, 0x23, 0x08, 0xa0, 0x14, 0x6d, 0x3a, 0x88,
	0xe6, 0xf8, 0xbe, 0x61, 0x38, 0x99, 0xca, 0x36,
	0x0d, 0x3e, 0x42, 0x0f, 0x63, 0x4d, 0x73, 0xf0,
	0xdf
};
CK_BYTE exp_1[] = {
	0x66, 0x2d, 0xb7, 0x65, 0xbe, 0x99, 0xc2, 0x35,
	0xfe, 0x2b, 0xf4, 0xe8, 0x5b, 0xd9, 0xdf, 0x13,
	0x26, 0x04, 0xe4, 0x18, 0x9d, 0x76, 0x92, 0x9a,
	0x9f, 0x53, 0x6c, 0xe6, 0x65, 0x6b, 0x53, 0x2f,
	0x2f, 0xbc, 0x46, 0xac, 0xe1, 0x97, 0xca, 0x21,
	0xf5, 0x21, 0x4e, 0x14, 0x49, 0x3b, 0x1d, 0x42,
	0xbd, 0x80, 0x0c, 0x3f, 0x29, 0xba, 0x09, 0x7f,
	0x85, 0xf0, 0x9c, 0x55, 0x60, 0xb4, 0x9e, 0xc1
};
CK_BYTE exp_2[] = {
	0x00, 0x87, 0x22, 0x74, 0xf1, 0xe2, 0x15, 0x3c,
	0x6d, 0xde, 0x7e, 0x90, 0x94, 0x2c, 0x06, 0xdb,
	0xb5, 0x54, 0x85, 0x59, 0xcf, 0x7a, 0x56, 0xdb,
	0xd9, 0x62, 0x54, 0x20, 0x56, 0xdc, 0xc3, 0xb9,
	0x0b, 0xff, 0x18, 0xf8, 0x7b, 0xdd, 0x7b, 0x24,
	0xf6, 0x06, 0x45, 0x71, 0x4e, 0xd7, 0x90, 0x2a,
	0x16, 0x52, 0x46, 0x75, 0x1a, 0xf5, 0x74, 0x8c,
	0x5a, 0xa4, 0xc4, 0x66, 0x27, 0xe0, 0x96, 0x64,
	0x7f
};
CK_BYTE coeff[] = {
	0x00, 0xd0, 0x1f, 0xb3, 0x47, 0x40, 0x93, 0x8b,
	0x99, 0xd7, 0xb5, 0xc6, 0x09, 0x82, 0x65, 0x94,
	0x9d, 0x56, 0x0a, 0x05, 0x55, 0x7d, 0x93, 0x04,
	0xa4, 0x26, 0xee, 0x42, 0x86, 0xa3, 0xf1, 0xd5,
	0x7a, 0x42, 0x84, 0x3c, 0x21, 0x96, 0x9a, 0xd9,
	0x36, 0xd4, 0x62, 0x01, 0xb0, 0x8b, 0x77, 0xe5,
	0xcc, 0x1b, 0xd2, 0x12, 0xd2, 0x9c, 0x89, 0x67,
	0x0c, 0x00, 0x09, 0x56, 0x8c, 0x33, 0x57, 0xf9,
	0x8c
};

char label[16];

static CK_BBOOL truevalue = TRUE;
static CK_BBOOL falsevalue = FALSE;

int
main(int argc, char *argv[]) {
	isc_result_t result;
	CK_RV rv;
	CK_SLOT_ID slot = 0;
	CK_SESSION_HANDLE hSession = CK_INVALID_HANDLE;
	CK_OBJECT_HANDLE *hKey;
	CK_OBJECT_CLASS kClass = CKO_PRIVATE_KEY;
	CK_KEY_TYPE kType = CKK_RSA;
	CK_ATTRIBUTE kTemplate[] =
	{
		{ CKA_CLASS, &kClass, (CK_ULONG) sizeof(kClass) },
		{ CKA_KEY_TYPE, &kType, (CK_ULONG) sizeof(kType) },
		{ CKA_TOKEN, &falsevalue, (CK_ULONG) sizeof(falsevalue) },
		{ CKA_PRIVATE, &truevalue, (CK_ULONG) sizeof(truevalue) },
		{ CKA_LABEL, (CK_BYTE_PTR) label, (CK_ULONG) sizeof(label) },
		{ CKA_SIGN, &truevalue, (CK_ULONG) sizeof(truevalue) },
		{ CKA_MODULUS, modulus, (CK_ULONG) sizeof(modulus) },
		{ CKA_PUBLIC_EXPONENT, pubexp, (CK_ULONG) sizeof(pubexp) },
		{ CKA_PRIVATE_EXPONENT, privexp, (CK_ULONG) sizeof(privexp) },
		{ CKA_PRIME_1, prime1, (CK_ULONG) sizeof(prime1) },
		{ CKA_PRIME_2, prime2, (CK_ULONG) sizeof(prime2) },
		{ CKA_EXPONENT_1, exp_1, (CK_ULONG) sizeof(exp_1) },
		{ CKA_EXPONENT_2, exp_2, (CK_ULONG) sizeof(exp_2) },
		{ CKA_COEFFICIENT, coeff, (CK_ULONG) sizeof(coeff) }
	};
	pk11_context_t pctx;
	pk11_optype_t op_type = OP_RSA;
	char *lib_name = NULL;
	char *pin = NULL;
	int error = 0;
	int c, errflg = 0;
	int ontoken  = 0;
	unsigned int count = 1000;
	unsigned int i;
	struct timespec starttime;
	struct timespec endtime;

	while ((c = isc_commandline_parse(argc, argv, ":m:s:p:tn:")) != -1) {
		switch (c) {
		case 'm':
			lib_name = isc_commandline_argument;
			break;
		case 's':
			slot = atoi(isc_commandline_argument);
			op_type = OP_ANY;
			break;
		case 'p':
			pin = isc_commandline_argument;
			break;
		case 't':
			ontoken = 1;
			break;
		case 'n':
			count = atoi(isc_commandline_argument);
			break;
		case ':':
			fprintf(stderr,
				"Option -%c requires an operand\n",
				isc_commandline_option);
			errflg++;
			break;
		case '?':
		default:
			fprintf(stderr, "Unrecognised option: -%c\n",
				isc_commandline_option);
			errflg++;
		}
	}

	if (errflg) {
		fprintf(stderr, "Usage:\n");
		fprintf(stderr,
			"\tprivrsa [-m module] [-s slot] [-p pin] "
			"[-t] [-n count]\n");
		exit(1);
	}

	pk11_result_register();

	/* Allocate hanles */
	hKey = (CK_SESSION_HANDLE *)
		malloc(count * sizeof(CK_SESSION_HANDLE));
	if (hKey == NULL) {
		perror("malloc");
		exit(1);
	}
	for (i = 0; i < count; i++)
		hKey[i] = CK_INVALID_HANDLE;

	/* Initialize the CRYPTOKI library */
	if (lib_name != NULL)
		pk11_set_lib_name(lib_name);

	if (pin == NULL) {
		pin = getpass("Enter Pin: ");
	}

	result = pk11_get_session(&pctx, op_type, ISC_FALSE, ISC_TRUE,
				  ISC_TRUE, (const char *) pin, slot);
	if ((result != ISC_R_SUCCESS) &&
	    (result != PK11_R_NORANDOMSERVICE) &&
	    (result != PK11_R_NODIGESTSERVICE) &&
	    (result != PK11_R_NOAESSERVICE)) {
		fprintf(stderr, "Error initializing PKCS#11: %s\n",
			isc_result_totext(result));
		free(hKey);
		exit(1);
	}

	if (pin != NULL)
		memset(pin, 0, strlen((char *)pin));

	hSession = pctx.session;

	if (ontoken)
		kTemplate[2].pValue = &truevalue;

	if (clock_gettime(CLOCK_REALTIME, &starttime) < 0) {
		perror("clock_gettime(start)");
		goto exit_objects;
	}

	for (i = 0; i < count; i++) {
		(void) snprintf(label, sizeof(label), "obj%u", i);
		kTemplate[4].ulValueLen = strlen(label);
		rv = pkcs_C_CreateObject(hSession, kTemplate, 14, &hKey[i]);
		if (rv != CKR_OK) {
			fprintf(stderr,
				"C_CreateObject[%u]: Error = 0x%.8lX\n",
				i, rv);
			error = 1;
			if (i == 0)
				goto exit_objects;
			break;
		}
	}

	if (clock_gettime(CLOCK_REALTIME, &endtime) < 0) {
		perror("clock_gettime(end)");
		goto exit_objects;
	}

	endtime.tv_sec -= starttime.tv_sec;
	endtime.tv_nsec -= starttime.tv_nsec;
	while (endtime.tv_nsec < 0) {
		endtime.tv_sec -= 1;
		endtime.tv_nsec += 1000000000;
	}
	printf("%u private RSA keys in %ld.%09lds\n", i,
	       endtime.tv_sec, endtime.tv_nsec);
	if (i > 0)
		printf("%g private RSA keys/s\n",
		       1024 * i / ((double) endtime.tv_sec +
				   (double) endtime.tv_nsec / 1000000000.));

    exit_objects:
	for (i = 0; i < count; i++) {
		/* Destroy objects */
		if (hKey[i] == CK_INVALID_HANDLE)
			continue;
		rv = pkcs_C_DestroyObject(hSession, hKey[i]);
		if ((rv != CKR_OK) && !errflg) {
			fprintf(stderr,
				"C_DestroyObject[%u]: Error = 0x%.8lX\n",
				i, rv);
			errflg = 1;
		}
	}

	free(hKey);

	pk11_return_session(&pctx);
	(void) pk11_finalize();

	exit(error);
}
