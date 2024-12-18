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

#pragma once

#include <stdlib.h>

/*! \file isc/entropy.h
 * \brief Implements wrapper around CSPRNG cryptographic library calls
 * for getting cryptographically secure pseudo-random numbers.
 *
 * Uses synchronous version of uv_random().
 */

void
isc_entropy_get(void *buf, size_t buflen);
/*!<
 * \brief Get cryptographically-secure pseudo-random data.
 */
