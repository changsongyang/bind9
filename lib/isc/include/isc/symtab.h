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

/*****
***** Module Info
*****/

/*! \file isc/symtab.h
 * \brief Provides a simple memory-based symbol table.
 *
 * Keys are C strings, and key comparisons are case-insensitive.  A type may
 * be specified when looking up, defining, or undefining.  A type value of
 * 0 means "match any type"; any other value will only match the given
 * type.
 *
 * It's possible that a client will attempt to define a <key, type, value>
 * tuple when a tuple with the given key and type already exists in the table.
 * What to do in this case is specified by the client.  Possible policies are:
 *
 *\li	#isc_symexists_reject	Disallow the define, returning #ISC_R_EXISTS
 *\li	#isc_symexists_replace	Replace the old value with the new.  The
 *				undefine action (if provided) will be called
 *				with the old <key, type, value> tuple.
 *\li	#isc_symexists_add	Add the new tuple, leaving the old tuple in
 *				the table.  Subsequent lookups will retrieve
 *				the most-recently-defined tuple.
 *
 * A lookup of a key using type 0 will return the most-recently defined
 * symbol with that key.  An undefine of a key using type 0 will undefine the
 * most-recently defined symbol with that key.  Trying to define a key with
 * type 0 is illegal.
 *
 * The symbol table library does not make a copy the key field, so the
 * caller must ensure that any key it passes to isc_symtab_define() will not
 * change until it calls isc_symtab_undefine() or isc_symtab_destroy().
 *
 * A user-specified action will be called (if provided) when a symbol is
 * undefined.  It can be used to free memory associated with keys and/or
 * values.
 *
 * A symbol table is implemented as a hash table of lists; the size of the
 * hash table is set by the 'size' parameter to isc_symtbl_create().  When
 * the number of entries in the symbol table reaches three quarters of this
 * value, the hash table is reallocated with size doubled, in order to
 * optimize lookup performance.  This has a negative effect on insertion
 * performance, which can be mitigated by sizing the table appropriately
 * when creating it.
 *
 * \li MP:
 *	The callers of this module must ensure any required synchronization.
 *
 * \li Reliability:
 *	No anticipated impact.
 *
 * \li Resources:
 *	TBS
 *
 * \li Security:
 *	No anticipated impact.
 *
 * \li Standards:
 *	None.
 */

/***
 *** Imports.
 ***/

#include <stdbool.h>

#include <isc/types.h>

/*
 *** Symbol Tables.
 ***/
/*% Symbol table value. */
typedef union isc_symvalue {
	void	    *as_pointer;
	const void  *as_cpointer;
	int	     as_integer;
	unsigned int as_uinteger;
} isc_symvalue_t;

typedef void (*isc_symtabaction_t)(char *key, unsigned int type,
				   isc_symvalue_t value, void *userarg);
/*% Symbol table exists. */
typedef enum {
	isc_symexists_reject = 0,  /*%< Disallow the define */
	isc_symexists_replace = 1, /*%< Replace the old value with the new */
	isc_symexists_add = 2	   /*%< Add the new tuple */
} isc_symexists_t;

/*% Create a symbol table. */
isc_result_t
isc_symtab_create(isc_mem_t *mctx, unsigned int size,
		  isc_symtabaction_t undefine_action, void *undefine_arg,
		  bool case_sensitive, isc_symtab_t **symtabp);

/*% Destroy a symbol table. */
void
isc_symtab_destroy(isc_symtab_t **symtabp);

/*% Lookup a symbol table. */
isc_result_t
isc_symtab_lookup(isc_symtab_t *symtab, const char *key, unsigned int type,
		  isc_symvalue_t *value);

/*% Define a symbol table. */
isc_result_t
isc_symtab_define(isc_symtab_t *symtab, const char *key, unsigned int type,
		  isc_symvalue_t value, isc_symexists_t exists_policy);

/*% Undefine a symbol table. */
isc_result_t
isc_symtab_undefine(isc_symtab_t *symtab, const char *key, unsigned int type);

/*% Return the number of items in a symbol table. */
unsigned int
isc_symtab_count(isc_symtab_t *symtab);
