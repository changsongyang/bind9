/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * See the COPYRIGHT file distributed with this work for additional
 * information regarding copyright ownership.
 */

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include <isc/crypto.h>
#include <isc/fips.h>
#include <isc/log.h>
#include <isc/mem.h>
#include <isc/tls.h>
#include <isc/util.h>

static isc_mem_t *isc__crypto_mctx = NULL;

const EVP_MD *isc__crypto_md5 = NULL;
const EVP_MD *isc__crypto_sha1 = NULL;
const EVP_MD *isc__crypto_sha224 = NULL;
const EVP_MD *isc__crypto_sha256 = NULL;
const EVP_MD *isc__crypto_sha384 = NULL;
const EVP_MD *isc__crypto_sha512 = NULL;

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#define md_register_algorithm(alg, algname)                            \
	{                                                              \
		REQUIRE(isc__crypto_##alg == NULL);                    \
		isc__crypto_##alg = EVP_MD_fetch(NULL, algname, NULL); \
		if (isc__crypto_##alg == NULL) {                       \
			ERR_clear_error();                             \
		}                                                      \
	}

#define md_unregister_algorithm(alg)                             \
	{                                                        \
		if (isc__crypto_##alg != NULL) {                 \
			EVP_MD_free(UNCONST(isc__crypto_##alg)); \
			isc__crypto_##alg = NULL;                \
		}                                                \
	}

#else /* OPENSSL_VERSION_NUMBER >= 0x30000000L */
#define md_register_algorithm(alg, algname)      \
	{                                        \
		isc__crypto_##alg = EVP_##alg(); \
		if (isc__crypto_##alg == NULL) { \
			ERR_clear_error();       \
		}                                \
	}
#define md_unregister_algorithm(alg)
#endif /* OPENSSL_VERSION_NUMBER >= 0x30000000L */

#if !defined(LIBRESSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30000000L
/*
 * This was crippled with LibreSSL, so just skip it:
 * https://cvsweb.openbsd.org/src/lib/libcrypto/Attic/mem.c
 */

#if ISC_MEM_TRACKLINES
/*
 * We use the internal isc__mem API here, so we can pass the file and line
 * arguments passed from OpenSSL >= 1.1.0 to our memory functions for better
 * tracking of the OpenSSL allocations.  Without this, we would always just see
 * isc__crypto_{malloc,realloc,free} in the tracking output, but with this in
 * place we get to see the places in the OpenSSL code where the allocations
 * happen.
 */

static void *
isc__crypto_malloc_ex(size_t size, const char *file, int line) {
	return isc__mem_allocate(isc__crypto_mctx, size, 0, file,
				 (unsigned int)line);
}

static void *
isc__crypto_realloc_ex(void *ptr, size_t size, const char *file, int line) {
	return isc__mem_reallocate(isc__crypto_mctx, ptr, size, 0, file,
				   (unsigned int)line);
}

static void
isc__crypto_free_ex(void *ptr, const char *file, int line) {
	if (ptr == NULL) {
		return;
	}
	if (isc__crypto_mctx != NULL) {
		isc__mem_free(isc__crypto_mctx, ptr, 0, file,
			      (unsigned int)line);
	}
}

#else /* ISC_MEM_TRACKLINES */

static void *
isc__crypto_malloc_ex(size_t size, const char *file, int line) {
	UNUSED(file);
	UNUSED(line);
	return isc_mem_allocate(isc__crypto_mctx, size);
}

static void *
isc__crypto_realloc_ex(void *ptr, size_t size, const char *file, int line) {
	UNUSED(file);
	UNUSED(line);
	return isc_mem_reallocate(isc__crypto_mctx, ptr, size);
}

static void
isc__crypto_free_ex(void *ptr, const char *file, int line) {
	UNUSED(file);
	UNUSED(line);
	if (ptr == NULL) {
		return;
	}
	if (isc__crypto_mctx != NULL) {
		isc__mem_free(isc__crypto_mctx, ptr, 0);
	}
}

#endif /* ISC_MEM_TRACKLINES */

#endif /* !defined(LIBRESSL_VERSION_NUMBER) */

void
isc__crypto_setdestroycheck(bool check) {
	isc_mem_setdestroycheck(isc__crypto_mctx, check);
}

void
isc__crypto_initialize(void) {
	uint64_t opts = OPENSSL_INIT_LOAD_CONFIG;

	isc_mem_create(&isc__crypto_mctx);
	isc_mem_setname(isc__crypto_mctx, "OpenSSL");
	isc_mem_setdestroycheck(isc__crypto_mctx, false);

#if !defined(LIBRESSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x30000000L
	/*
	 * CRYPTO_set_mem_(_ex)_functions() returns 1 on success or 0 on
	 * failure, which means OpenSSL already allocated some memory.  There's
	 * nothing we can do about it.
	 */
	(void)CRYPTO_set_mem_functions(isc__crypto_malloc_ex,
				       isc__crypto_realloc_ex,
				       isc__crypto_free_ex);
#endif /* !defined(LIBRESSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= \
	  0x30000000L  */

#if defined(OPENSSL_INIT_NO_ATEXIT)
	/*
	 * We call OPENSSL_cleanup() manually, in a correct order, thus disable
	 * the automatic atexit() handler.
	 */
	opts |= OPENSSL_INIT_NO_ATEXIT;
#endif

	RUNTIME_CHECK(OPENSSL_init_ssl(opts, NULL) == 1);

	/* Protect ourselves against unseeded PRNG */
	if (RAND_status() != 1) {
		isc_tlserr2result(ISC_LOGCATEGORY_GENERAL, ISC_LOGMODULE_CRYPTO,
				  "RAND_status", ISC_R_CRYPTOFAILURE);
		FATAL_ERROR("OpenSSL pseudorandom number generator "
			    "cannot be initialized (see the `PRNG not "
			    "seeded' message in the OpenSSL FAQ)");
	}

#if defined(ENABLE_FIPS_MODE)
	if (!isc_fips_mode()) {
		if (isc_fips_set_mode(1) != ISC_R_SUCCESS) {
			isc_tlserr2result(ISC_LOGCATEGORY_GENERAL,
					  ISC_LOGMODULE_CRYPTO, "FIPS_mode_set",
					  ISC_R_CRYPTOFAILURE);
			exit(EXIT_FAILURE);
		}
	}
#endif

	md_register_algorithm(md5, "MD5");
	md_register_algorithm(sha1, "SHA1");
	md_register_algorithm(sha224, "SHA224");
	md_register_algorithm(sha256, "SHA256");
	md_register_algorithm(sha384, "SHA384");
	md_register_algorithm(sha512, "SHA512");
}

void
isc__crypto_shutdown(void) {
	md_unregister_algorithm(sha512);
	md_unregister_algorithm(sha384);
	md_unregister_algorithm(sha256);
	md_unregister_algorithm(sha224);
	md_unregister_algorithm(sha1);
	md_unregister_algorithm(md5);

	OPENSSL_cleanup();

	isc_mem_destroy(&isc__crypto_mctx);
}

#undef md_unregister_algorithm
#undef md_register_algorithm
