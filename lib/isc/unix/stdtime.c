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

/*! \file */

#include <stdbool.h>
#include <stddef.h> /* NULL */
#include <stdlib.h> /* NULL */
#include <syslog.h>

#include <isc/stdtime.h>
#include <isc/util.h>

#include <sys/time.h>

#ifndef ISC_FIX_TV_USEC
#define ISC_FIX_TV_USEC 1
#endif /* ifndef ISC_FIX_TV_USEC */

#define US_PER_S 1000000

#if ISC_FIX_TV_USEC
static inline void
fix_tv_usec(struct timeval *tv) {
	bool fixed = false;

	if (tv->tv_usec < 0) {
		fixed = true;
		do {
			tv->tv_sec -= 1;
			tv->tv_usec += US_PER_S;
		} while (tv->tv_usec < 0);
	} else if (tv->tv_usec >= US_PER_S) {
		fixed = true;
		do {
			tv->tv_sec += 1;
			tv->tv_usec -= US_PER_S;
		} while (tv->tv_usec >= US_PER_S);
	}
	/*
	 * Call syslog directly as we are called from the logging functions.
	 */
	if (fixed) {
		(void)syslog(LOG_ERR, "gettimeofday returned bad tv_usec: "
				      "corrected");
	}
}
#endif /* if ISC_FIX_TV_USEC */

void
isc_stdtime_get(isc_stdtime_t *t) {
	struct timeval tv;

	/*
	 * Set 't' to the number of seconds since 00:00:00 UTC, January 1,
	 * 1970.
	 */

	REQUIRE(t != NULL);

	RUNTIME_CHECK(gettimeofday(&tv, NULL) != -1);

#if ISC_FIX_TV_USEC
	fix_tv_usec(&tv);
	INSIST(tv.tv_usec >= 0);
#else  /* if ISC_FIX_TV_USEC */
	INSIST(tv.tv_usec >= 0 && tv.tv_usec < US_PER_S);
#endif /* if ISC_FIX_TV_USEC */

	*t = (unsigned int)tv.tv_sec;
}
