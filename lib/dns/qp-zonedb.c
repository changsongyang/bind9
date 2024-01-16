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

/*! \file */

#include <inttypes.h>
#include <stdbool.h>
#include <sys/mman.h>

#include <isc/ascii.h>
#include <isc/async.h>
#include <isc/atomic.h>
#include <isc/crc64.h>
#include <isc/file.h>
#include <isc/hash.h>
#include <isc/hashmap.h>
#include <isc/heap.h>
#include <isc/hex.h>
#include <isc/loop.h>
#include <isc/mem.h>
#include <isc/mutex.h>
#include <isc/once.h>
#include <isc/random.h>
#include <isc/refcount.h>
#include <isc/result.h>
#include <isc/rwlock.h>
#include <isc/serial.h>
#include <isc/stdio.h>
#include <isc/string.h>
#include <isc/time.h>
#include <isc/urcu.h>
#include <isc/util.h>

#include <dns/callbacks.h>
#include <dns/db.h>
#include <dns/dbiterator.h>
#include <dns/fixedname.h>
#include <dns/log.h>
#include <dns/masterdump.h>
#include <dns/nsec.h>
#include <dns/nsec3.h>
#include <dns/qp.h>
#include <dns/rbt.h>
#include <dns/rdata.h>
#include <dns/rdataset.h>
#include <dns/rdatasetiter.h>
#include <dns/rdataslab.h>
#include <dns/rdatastruct.h>
#include <dns/stats.h>
#include <dns/time.h>
#include <dns/view.h>
#include <dns/zone.h>
#include <dns/zonekey.h>

#include "db_p.h"
#include "qpdb_p.h"

#define CHECK(op)                            \
	do {                                 \
		result = (op);               \
		if (result != ISC_R_SUCCESS) \
			goto failure;        \
	} while (0)

#define EXISTS(header)                                 \
	((atomic_load_acquire(&(header)->attributes) & \
	  DNS_SLABHEADERATTR_NONEXISTENT) == 0)
#define NONEXISTENT(header)                            \
	((atomic_load_acquire(&(header)->attributes) & \
	  DNS_SLABHEADERATTR_NONEXISTENT) != 0)
#define IGNORE(header)                                 \
	((atomic_load_acquire(&(header)->attributes) & \
	  DNS_SLABHEADERATTR_IGNORE) != 0)
#define RESIGN(header)                                 \
	((atomic_load_acquire(&(header)->attributes) & \
	  DNS_SLABHEADERATTR_RESIGN) != 0)
#define ANCIENT(header)                                \
	((atomic_load_acquire(&(header)->attributes) & \
	  DNS_SLABHEADERATTR_ANCIENT) != 0)

#define QPDB_ATTR_LOADED  0x01
#define QPDB_ATTR_LOADING 0x02

static isc_result_t
findnsec3node(dns_db_t *db, const dns_name_t *name, bool create,
	      dns_dbnode_t **nodep DNS__DB_FLARG) {
	dns_qpdb_t *qpdb = (dns_qpdb_t *)db;

	REQUIRE(VALID_QPDB(qpdb));

	return (dns__qpdb_findnodeintree(qpdb, qpdb->nsec3, name, create,
					 nodep DNS__DB_FLARG_PASS));
}

static isc_result_t
check_zonecut(dns_rbtnode_t *node, void *arg DNS__DB_FLARG) {
	qpdb_search_t *search = arg;
	dns_slabheader_t *header = NULL, *header_next = NULL;
	dns_slabheader_t *dname_header = NULL, *sigdname_header = NULL;
	dns_slabheader_t *ns_header = NULL;
	dns_slabheader_t *found = NULL;
	isc_result_t result = DNS_R_CONTINUE;
	dns_rbtnode_t *onode = NULL;
	isc_rwlocktype_t nlocktype = isc_rwlocktype_none;

	/*
	 * We only want to remember the topmost zone cut, since it's the one
	 * that counts, so we'll just continue if we've already found a
	 * zonecut.
	 */
	if (search->zonecut != NULL) {
		return (result);
	}

	onode = search->qpdb->origin_node;

	NODE_RDLOCK(&(search->qpdb->node_locks[node->locknum].lock),
		    &nlocktype);

	/*
	 * Look for an NS or DNAME rdataset active in our version.
	 */
	for (header = node->data; header != NULL; header = header_next) {
		header_next = header->next;
		if (header->type == dns_rdatatype_ns ||
		    header->type == dns_rdatatype_dname ||
		    header->type == DNS_SIGTYPE(dns_rdatatype_dname))
		{
			do {
				if (header->serial <= search->serial &&
				    !IGNORE(header))
				{
					/*
					 * Is this a "this rdataset doesn't
					 * exist" record?
					 */
					if (NONEXISTENT(header)) {
						header = NULL;
					}
					break;
				} else {
					header = header->down;
				}
			} while (header != NULL);
			if (header != NULL) {
				if (header->type == dns_rdatatype_dname) {
					dname_header = header;
				} else if (header->type ==
					   DNS_SIGTYPE(dns_rdatatype_dname))
				{
					sigdname_header = header;
				} else if (node != onode ||
					   IS_STUB(search->qpdb))
				{
					/*
					 * We've found an NS rdataset that
					 * isn't at the origin node.  We check
					 * that they're not at the origin node,
					 * because otherwise we'd erroneously
					 * treat the zone top as if it were
					 * a delegation.
					 */
					ns_header = header;
				}
			}
		}
	}

	/*
	 * Did we find anything?
	 */
	if (!IS_STUB(search->qpdb) && ns_header != NULL) {
		/*
		 * Note that NS has precedence over DNAME if both exist
		 * in a zone.  Otherwise DNAME take precedence over NS.
		 */
		found = ns_header;
		search->zonecut_sigheader = NULL;
	} else if (dname_header != NULL) {
		found = dname_header;
		search->zonecut_sigheader = sigdname_header;
	} else if (ns_header != NULL) {
		found = ns_header;
		search->zonecut_sigheader = NULL;
	}

	if (found != NULL) {
		/*
		 * We increment the reference count on node to ensure that
		 * search->zonecut_header will still be valid later.
		 */
		dns__qpdb_newref(search->qpdb, node,
				 isc_rwlocktype_read DNS__DB_FLARG_PASS);
		search->zonecut = node;
		search->zonecut_header = found;
		search->need_cleanup = true;
		/*
		 * Since we've found a zonecut, anything beneath it is
		 * glue and is not subject to wildcard matching, so we
		 * may clear search->wild.
		 */
		search->wild = false;
		if ((search->options & DNS_DBFIND_GLUEOK) == 0) {
			/*
			 * If the caller does not want to find glue, then
			 * this is the best answer and the search should
			 * stop now.
			 */
			result = DNS_R_PARTIALMATCH;
		} else {
			dns_name_t *zcname = NULL;

			/*
			 * The search will continue beneath the zone cut.
			 * This may or may not be the best match.  In case it
			 * is, we need to remember the node name.
			 */
			zcname = dns_fixedname_name(&search->zonecut_name);
			dns_name_copy(node->name, zcname);
			search->copy_name = true;
		}
	} else {
		/*
		 * There is no zonecut at this node which is active in this
		 * version.
		 *
		 * If this is a "wild" node and the caller hasn't disabled
		 * wildcard matching, remember that we've seen a wild node
		 * in case we need to go searching for wildcard matches
		 * later on.
		 */
		if (node->wild && (search->options & DNS_DBFIND_NOWILD) == 0) {
			search->wild = true;
		}
	}

	NODE_UNLOCK(&(search->qpdb->node_locks[node->locknum].lock),
		    &nlocktype);

	return (result);
}

static isc_result_t
setup_delegation(qpdb_search_t *search, dns_dbnode_t **nodep,
		 dns_name_t *foundname, dns_rdataset_t *rdataset,
		 dns_rdataset_t *sigrdataset DNS__DB_FLARG) {
	dns_name_t *zcname = NULL;
	dns_typepair_t type;
	dns_rbtnode_t *node = NULL;

	REQUIRE(search != NULL);
	REQUIRE(search->zonecut != NULL);
	REQUIRE(search->zonecut_header != NULL);

	/*
	 * The caller MUST NOT be holding any node locks.
	 */

	node = search->zonecut;
	type = search->zonecut_header->type;

	/*
	 * If we have to set foundname, we do it before anything else.
	 * If we were to set foundname after we had set nodep or bound the
	 * rdataset, then we'd have to undo that work if dns_name_copy()
	 * failed.  By setting foundname first, there's nothing to undo if
	 * we have trouble.
	 */
	if (foundname != NULL && search->copy_name) {
		zcname = dns_fixedname_name(&search->zonecut_name);
		dns_name_copy(zcname, foundname);
	}
	if (nodep != NULL) {
		/*
		 * Note that we don't have to increment the node's reference
		 * count here because we're going to use the reference we
		 * already have in the search block.
		 */
		*nodep = node;
		search->need_cleanup = false;
	}
	if (rdataset != NULL) {
		isc_rwlocktype_t nlocktype = isc_rwlocktype_none;
		NODE_RDLOCK(&(search->qpdb->node_locks[node->locknum].lock),
			    &nlocktype);
		dns__qpdb_bindrdataset(
			search->qpdb, node, search->zonecut_header, search->now,
			isc_rwlocktype_read, rdataset DNS__DB_FLARG_PASS);
		if (sigrdataset != NULL && search->zonecut_sigheader != NULL) {
			dns__qpdb_bindrdataset(search->qpdb, node,
					       search->zonecut_sigheader,
					       search->now, isc_rwlocktype_read,
					       sigrdataset DNS__DB_FLARG_PASS);
		}
		NODE_UNLOCK(&(search->qpdb->node_locks[node->locknum].lock),
			    &nlocktype);
	}

	if (type == dns_rdatatype_dname) {
		return (DNS_R_DNAME);
	}
	return (DNS_R_DELEGATION);
}

typedef enum { FORWARD, BACK } direction_t;

/*
 * Step backwards or forwards through the database until we find a
 * node with data in it for the desired version. If 'nextname' is not NULL,
 * and we found a predecessor or successor, save the name we found in it.
 * Return true if we found a predecessor or successor.
 */
static bool
step(qpdb_search_t *search, dns_qpiter_t *iter, direction_t direction,
     dns_name_t *nextname) {
	dns_fixedname_t fnodename;
	dns_name_t *nodename = dns_fixedname_initname(&fnodename);
	dns_qpdb_t *qpdb = NULL;
	dns_rbtnode_t *node = NULL;
	isc_result_t result = ISC_R_SUCCESS;
	dns_slabheader_t *header = NULL;

	qpdb = search->qpdb;

	result = dns_qpiter_current(iter, nodename, (void **)&node, NULL);

	while (result == ISC_R_SUCCESS || result == DNS_R_NEWORIGIN) {
		isc_rwlocktype_t nlocktype = isc_rwlocktype_none;

		NODE_RDLOCK(&(qpdb->node_locks[node->locknum].lock),
			    &nlocktype);
		for (header = node->data; header != NULL; header = header->next)
		{
			if (header->serial <= search->serial &&
			    !IGNORE(header) && EXISTS(header))
			{
				break;
			}
		}
		NODE_UNLOCK(&(qpdb->node_locks[node->locknum].lock),
			    &nlocktype);
		if (header != NULL) {
			break;
		}

		if (direction == FORWARD) {
			result = dns_qpiter_next(iter, nodename, (void **)&node,
						 NULL);
		} else {
			result = dns_qpiter_prev(iter, nodename, (void **)&node,
						 NULL);
		}
	};
	if (result == ISC_R_SUCCESS) {
		if (nextname != NULL) {
			dns_name_copy(nodename, nextname);
		}
		return (true);
	}
	return (false);
}

/*
 * Use step() to find the successor to the current name, and then
 * check to see whether it's a subdomain of the current name. If so,
 * then this is an empty non-terminal in the currently active version
 * of the database.
 */
static bool
activeempty(qpdb_search_t *search, dns_qpiter_t *iter,
	    const dns_name_t *current) {
	isc_result_t result;
	dns_fixedname_t fnext;
	dns_name_t *next = dns_fixedname_initname(&fnext);

	result = dns_qpiter_next(iter, NULL, NULL, NULL);
	if (result != ISC_R_SUCCESS && result != DNS_R_NEWORIGIN) {
		return (false);
	}
	return (step(search, iter, FORWARD, next) &&
		dns_name_issubdomain(next, current));
}

static bool
wildcard_blocked(qpdb_search_t *search, const dns_name_t *qname,
		 dns_name_t *wname) {
	isc_result_t result;
	dns_fixedname_t fnext;
	dns_fixedname_t fprev;
	dns_name_t *next = NULL, *prev = NULL;
	dns_name_t name;
	dns_name_t rname;
	dns_name_t tname;
	dns_qpiter_t iter;
	bool check_next = false;
	bool check_prev = false;
	unsigned int n;

	dns_name_init(&name, NULL);
	dns_name_init(&tname, NULL);
	dns_name_init(&rname, NULL);
	next = dns_fixedname_initname(&fnext);
	prev = dns_fixedname_initname(&fprev);

	/*
	 * The qname seems to have matched a wildcard, but we
	 * need to find out if there's an empty nonterminal node
	 * between the wildcard level and the qname.
	 *
	 * search->iter should now be pointing at the predecessor
	 * of the searched-for name. We are using a local copy of the
	 * iterator so as not to change the state of search->iter.
	 * step() will walk backward until we find a predecessor with
	 * data.
	 */
	iter = search->iter;
	check_prev = step(search, &iter, BACK, prev);

	/* Now reset the iterator and look for a successor with data. */
	iter = search->iter;
	result = dns_qpiter_next(&iter, NULL, NULL, NULL);
	if (result == ISC_R_SUCCESS) {
		check_next = step(search, &iter, FORWARD, next);
	}

	if (!check_prev && !check_next) {
		/* No predecessor or successor was found at all? */
		return (false);
	}

	dns_name_clone(qname, &rname);

	/*
	 * Remove the wildcard label to find the terminal name.
	 */
	n = dns_name_countlabels(wname);
	dns_name_getlabelsequence(wname, 1, n - 1, &tname);

	do {
		if ((check_prev && dns_name_issubdomain(prev, &rname)) ||
		    (check_next && dns_name_issubdomain(next, &rname)))
		{
			return (true);
		}

		/*
		 * Remove the leftmost label from the qname and check again.
		 */
		n = dns_name_countlabels(&rname);
		dns_name_getlabelsequence(&rname, 1, n - 1, &rname);
	} while (!dns_name_equal(&rname, &tname));

	return (false);
}

static isc_result_t
find_wildcard(qpdb_search_t *search, dns_rbtnode_t **nodep,
	      const dns_name_t *qname) {
	dns_slabheader_t *header = NULL;
	isc_result_t result = ISC_R_NOTFOUND;
	dns_qpdb_t *qpdb = NULL;
	bool wild, active;

	/*
	 * Caller must be holding the tree lock and MUST NOT be holding
	 * any node locks.
	 */

	/*
	 * Examine each ancestor level.  If the level's wild bit
	 * is set, then construct the corresponding wildcard name and
	 * search for it.  If the wildcard node exists, and is active in
	 * this version, we're done.  If not, then we next check to see
	 * if the ancestor is active in this version.  If so, then there
	 * can be no possible wildcard match and again we're done.  If not,
	 * continue the search.
	 */

	qpdb = search->qpdb;
	for (int i = dns_qpchain_length(&search->chain) - 1; i >= 0; i--) {
		dns_rbtnode_t *node = NULL;
		isc_rwlock_t *lock = NULL;
		isc_rwlocktype_t nlocktype = isc_rwlocktype_none;

		dns_qpchain_node(&search->chain, i, NULL, (void **)&node, NULL);
		lock = &qpdb->node_locks[node->locknum].lock;

		NODE_RDLOCK(lock, &nlocktype);
		/*
		 * First we try to figure out if this node is active in
		 * the search's version.  We do this now, even though we
		 * may not need the information, because it simplifies the
		 * locking and code flow.
		 */
		for (header = node->data; header != NULL; header = header->next)
		{
			if (header->serial <= search->serial &&
			    !IGNORE(header) && EXISTS(header) &&
			    !ANCIENT(header))
			{
				break;
			}
		}
		active = (header != NULL);
		wild = node->wild;
		NODE_UNLOCK(lock, &nlocktype);

		if (wild) {
			dns_rbtnode_t *wnode = NULL;
			dns_fixedname_t fwname;
			dns_name_t *wname = NULL;
			dns_qpiter_t witer;

			/*
			 * Construct the wildcard name for this level.
			 */
			wname = dns_fixedname_initname(&fwname);
			result = dns_name_concatenate(dns_wildcardname,
						      node->name, wname, NULL);
			if (result != ISC_R_SUCCESS) {
				break;
			}

			wnode = NULL;
			result = dns_qp_lookup(qpdb->tree, wname, NULL, &witer,
					       NULL, (void **)&wnode, NULL);
			if (result == ISC_R_SUCCESS) {
				/*
				 * We have found the wildcard node.  If it
				 * is active in the search's version, we're
				 * done.
				 */
				lock = &qpdb->node_locks[wnode->locknum].lock;
				NODE_RDLOCK(lock, &nlocktype);
				for (header = wnode->data; header != NULL;
				     header = header->next)
				{
					if (header->serial <= search->serial &&
					    !IGNORE(header) && EXISTS(header) &&
					    !ANCIENT(header))
					{
						break;
					}
				}
				NODE_UNLOCK(lock, &nlocktype);
				if (header != NULL ||
				    activeempty(search, &witer, wname))
				{
					if (wildcard_blocked(search, qname,
							     wname))
					{
						return (ISC_R_NOTFOUND);
					}
					/*
					 * The wildcard node is active!
					 *
					 * Note: result is still ISC_R_SUCCESS
					 * so we don't have to set it.
					 */
					*nodep = wnode;
					break;
				}
			} else if (result != ISC_R_NOTFOUND &&
				   result != DNS_R_PARTIALMATCH)
			{
				/*
				 * An error has occurred.  Bail out.
				 */
				break;
			}
		}

		if (active) {
			/*
			 * The level node is active.  Any wildcarding
			 * present at higher levels has no
			 * effect and we're done.
			 */
			result = ISC_R_NOTFOUND;
			break;
		}
	}

	return (result);
}

static bool
matchparams(dns_slabheader_t *header, qpdb_search_t *search) {
	dns_rdata_t rdata = DNS_RDATA_INIT;
	dns_rdata_nsec3_t nsec3;
	unsigned char *raw = NULL;
	unsigned int rdlen, count;
	isc_region_t region;
	isc_result_t result;

	REQUIRE(header->type == dns_rdatatype_nsec3);

	raw = (unsigned char *)header + sizeof(*header);
	count = raw[0] * 256 + raw[1]; /* count */
	raw += DNS_RDATASET_COUNT + DNS_RDATASET_LENGTH;

	while (count-- > 0) {
		rdlen = raw[0] * 256 + raw[1];
		raw += DNS_RDATASET_ORDER + DNS_RDATASET_LENGTH;
		region.base = raw;
		region.length = rdlen;
		dns_rdata_fromregion(&rdata, search->qpdb->common.rdclass,
				     dns_rdatatype_nsec3, &region);
		raw += rdlen;
		result = dns_rdata_tostruct(&rdata, &nsec3, NULL);
		INSIST(result == ISC_R_SUCCESS);
		if (nsec3.hash == search->rbtversion->hash &&
		    nsec3.iterations == search->rbtversion->iterations &&
		    nsec3.salt_length == search->rbtversion->salt_length &&
		    memcmp(nsec3.salt, search->rbtversion->salt,
			   nsec3.salt_length) == 0)
		{
			return (true);
		}
		dns_rdata_reset(&rdata);
	}
	return (false);
}

/*
 * Find node of the NSEC/NSEC3 record that is 'name'.
 */
static isc_result_t
previous_closest_nsec(dns_rdatatype_t type, qpdb_search_t *search,
		      dns_name_t *name, dns_name_t *origin,
		      dns_rbtnode_t **nodep, dns_qpiter_t *nseciter,
		      bool *firstp) {
	dns_fixedname_t ftarget;
	dns_name_t *target = NULL;
	dns_rbtnode_t *nsecnode = NULL;
	isc_result_t result;

	REQUIRE(nodep != NULL && *nodep == NULL);
	REQUIRE(type == dns_rdatatype_nsec3 || firstp != NULL);

	if (type == dns_rdatatype_nsec3) {
		return (dns_qpiter_prev(&search->iter, name, (void **)nodep,
					NULL));
	}

	target = dns_fixedname_initname(&ftarget);

	for (;;) {
		if (*firstp) {
			/*
			 * Construct the name of the second node to check.
			 * It is the first node sought in the NSEC tree.
			 */
			*firstp = false;
			result = dns_name_concatenate(name, origin, target,
						      NULL);
			if (result != ISC_R_SUCCESS) {
				return (result);
			}
			nsecnode = NULL;
			result = dns_qp_lookup(search->qpdb->nsec, name, NULL,
					       nseciter, NULL,
					       (void **)&nsecnode, NULL);
			if (result == ISC_R_SUCCESS) {
				/*
				 * Since this was the first loop, finding the
				 * name in the NSEC tree implies that the first
				 * node checked in the main tree had an
				 * unacceptable NSEC record.
				 * Try the previous node in the NSEC tree.
				 */
				result = dns_qpiter_prev(nseciter, name, NULL,
							 NULL);
				if (result == DNS_R_NEWORIGIN) {
					result = ISC_R_SUCCESS;
				}
			} else if (result == ISC_R_NOTFOUND ||
				   result == DNS_R_PARTIALMATCH)
			{
				/* The iterator is already where we want it */
				result = dns_qpiter_current(nseciter, name,
							    NULL, NULL);
			}
		} else {
			/*
			 * This is a second or later trip through the auxiliary
			 * tree for the name of a third or earlier NSEC node in
			 * the main tree.  Previous trips through the NSEC tree
			 * must have found nodes in the main tree with NSEC
			 * records.  Perhaps they lacked signature records.
			 */
			result = dns_qpiter_prev(nseciter, name, NULL, NULL);
			if (result == DNS_R_NEWORIGIN) {
				result = ISC_R_SUCCESS;
			}
		}
		if (result != ISC_R_SUCCESS) {
			return (result);
		}

		*nodep = NULL;
		result = dns_qp_lookup(search->qpdb->tree, name, NULL,
				       &search->iter, &search->chain,
				       (void **)nodep, NULL);
		if (result == ISC_R_SUCCESS) {
			return (result);
		}

		/*
		 * There should always be a node in the main tree with the
		 * same name as the node in the auxiliary NSEC tree, except for
		 * nodes in the auxiliary tree that are awaiting deletion.
		 */
		if (result != DNS_R_PARTIALMATCH && result != ISC_R_NOTFOUND) {
			isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
				      DNS_LOGMODULE_CACHE, ISC_LOG_ERROR,
				      "previous_closest_nsec(): %s",
				      isc_result_totext(result));
			return (DNS_R_BADDB);
		}
	}
}

/*
 * Find the NSEC/NSEC3 which is or before the current point on the
 * search chain.  For NSEC3 records only NSEC3 records that match the
 * current NSEC3PARAM record are considered.
 */
static isc_result_t
find_closest_nsec(qpdb_search_t *search, dns_dbnode_t **nodep,
		  dns_name_t *foundname, dns_rdataset_t *rdataset,
		  dns_rdataset_t *sigrdataset, dns_qp_t *tree,
		  bool secure DNS__DB_FLARG) {
	dns_rbtnode_t *node = NULL, *prevnode = NULL;
	dns_slabheader_t *header = NULL, *header_next = NULL;
	dns_qpiter_t nseciter;
	bool empty_node;
	isc_result_t result;
	dns_fixedname_t fname, forigin;
	dns_name_t *name = NULL, *origin = NULL;
	dns_rdatatype_t type;
	dns_typepair_t sigtype;
	bool wraps;
	bool first = true;
	bool need_sig = secure;

	if (tree == search->qpdb->nsec3) {
		type = dns_rdatatype_nsec3;
		sigtype = DNS_SIGTYPE(dns_rdatatype_nsec3);
		wraps = true;
	} else {
		type = dns_rdatatype_nsec;
		sigtype = DNS_SIGTYPE(dns_rdatatype_nsec);
		wraps = false;
	}

	/*
	 * Use the auxiliary tree only starting with the second node in the
	 * hope that the original node will be right much of the time.
	 */
	name = dns_fixedname_initname(&fname);
	origin = dns_fixedname_initname(&forigin);

	result = dns_qpiter_current(&search->iter, name, (void **)&node, NULL);
	if (result != ISC_R_SUCCESS) {
		return (result);
	}

again:
	do {
		dns_slabheader_t *found = NULL, *foundsig = NULL;
		isc_rwlocktype_t nlocktype = isc_rwlocktype_none;
		NODE_RDLOCK(&(search->qpdb->node_locks[node->locknum].lock),
			    &nlocktype);
		empty_node = true;
		for (header = node->data; header != NULL; header = header_next)
		{
			header_next = header->next;
			/*
			 * Look for an active, extant NSEC or RRSIG NSEC.
			 */
			do {
				if (header->serial <= search->serial &&
				    !IGNORE(header))
				{
					/*
					 * Is this a "this rdataset doesn't
					 * exist" record?
					 */
					if (NONEXISTENT(header)) {
						header = NULL;
					}
					break;
				} else {
					header = header->down;
				}
			} while (header != NULL);
			if (header != NULL) {
				/*
				 * We now know that there is at least one
				 * active rdataset at this node.
				 */
				empty_node = false;
				if (header->type == type) {
					found = header;
					if (foundsig != NULL) {
						break;
					}
				} else if (header->type == sigtype) {
					foundsig = header;
					if (found != NULL) {
						break;
					}
				}
			}
		}
		if (!empty_node) {
			if (found != NULL && search->rbtversion->havensec3 &&
			    found->type == dns_rdatatype_nsec3 &&
			    !matchparams(found, search))
			{
				empty_node = true;
				found = NULL;
				foundsig = NULL;
				result = previous_closest_nsec(
					type, search, name, origin, &prevnode,
					NULL, NULL);
			} else if (found != NULL &&
				   (foundsig != NULL || !need_sig))
			{
				/*
				 * We've found the right NSEC/NSEC3 record.
				 *
				 * Note: for this to really be the right
				 * NSEC record, it's essential that the NSEC
				 * records of any nodes obscured by a zone
				 * cut have been removed; we assume this is
				 * the case.
				 */
				result = dns_name_concatenate(name, origin,
							      foundname, NULL);
				if (result == ISC_R_SUCCESS) {
					if (nodep != NULL) {
						dns__qpdb_newref(
							search->qpdb, node,
							isc_rwlocktype_read
								DNS__DB_FLARG_PASS);
						*nodep = node;
					}
					dns__qpdb_bindrdataset(
						search->qpdb, node, found,
						search->now,
						isc_rwlocktype_read,
						rdataset DNS__DB_FLARG_PASS);
					if (foundsig != NULL) {
						dns__qpdb_bindrdataset(
							search->qpdb, node,
							foundsig, search->now,
							isc_rwlocktype_read,
							sigrdataset
								DNS__DB_FLARG_PASS);
					}
				}
			} else if (found == NULL && foundsig == NULL) {
				/*
				 * This node is active, but has no NSEC or
				 * RRSIG NSEC.  That means it's glue or
				 * other obscured zone data that isn't
				 * relevant for our search.  Treat the
				 * node as if it were empty and keep looking.
				 */
				empty_node = true;
				result = previous_closest_nsec(
					type, search, name, origin, &prevnode,
					&nseciter, &first);
			} else {
				/*
				 * We found an active node, but either the
				 * NSEC or the RRSIG NSEC is missing.  This
				 * shouldn't happen.
				 */
				result = DNS_R_BADDB;
			}
		} else {
			/*
			 * This node isn't active.  We've got to keep
			 * looking.
			 */
			result = previous_closest_nsec(type, search, name,
						       origin, &prevnode,
						       &nseciter, &first);
		}
		NODE_UNLOCK(&(search->qpdb->node_locks[node->locknum].lock),
			    &nlocktype);
		node = prevnode;
		prevnode = NULL;
	} while (empty_node && result == ISC_R_SUCCESS);

	if (result == ISC_R_NOMORE && wraps) {
		result = dns_qpiter_prev(&search->iter, name, (void **)&node,
					 NULL);
		if (result == ISC_R_SUCCESS || result == DNS_R_NEWORIGIN) {
			wraps = false;
			goto again;
		}
	}

	/*
	 * If the result is ISC_R_NOMORE, then we got to the beginning of
	 * the database and didn't find a NSEC record.  This shouldn't
	 * happen.
	 */
	if (result == ISC_R_NOMORE) {
		result = DNS_R_BADDB;
	}

	return (result);
}

static isc_result_t
zone_find(dns_db_t *db, const dns_name_t *name, dns_dbversion_t *version,
	  dns_rdatatype_t type, unsigned int options,
	  isc_stdtime_t now ISC_ATTR_UNUSED, dns_dbnode_t **nodep,
	  dns_name_t *foundname, dns_rdataset_t *rdataset,
	  dns_rdataset_t *sigrdataset DNS__DB_FLARG) {
	dns_rbtnode_t *node = NULL;
	isc_result_t result;
	qpdb_search_t search;
	bool cname_ok = true;
	bool close_version = false;
	bool maybe_zonecut = false;
	bool at_zonecut = false;
	bool wild = false;
	bool empty_node;
	dns_slabheader_t *header = NULL, *header_next = NULL;
	dns_slabheader_t *found = NULL, *nsecheader = NULL;
	dns_slabheader_t *foundsig = NULL, *cnamesig = NULL, *nsecsig = NULL;
	dns_typepair_t sigtype;
	bool active;
	isc_rwlock_t *lock = NULL;
	dns_qp_t *tree = NULL;
	isc_rwlocktype_t nlocktype = isc_rwlocktype_none;
	isc_rwlocktype_t tlocktype = isc_rwlocktype_none;

	REQUIRE(VALID_QPDB((dns_qpdb_t *)db));
	INSIST(version == NULL ||
	       ((dns_qpdb_version_t *)version)->qpdb == (dns_qpdb_t *)db);

	/*
	 * If the caller didn't supply a version, attach to the current
	 * version.
	 */
	if (version == NULL) {
		dns__qpdb_currentversion(db, &version);
		close_version = true;
	}

	search = (qpdb_search_t){
		.qpdb = (dns_qpdb_t *)db,
		.rbtversion = version,
		.serial = ((dns_qpdb_version_t *)version)->serial,
		.options = options,
	};
	dns_fixedname_init(&search.zonecut_name);

	TREE_RDLOCK(&search.qpdb->tree_lock, &tlocktype);

	/*
	 * Search down from the root of the tree.
	 */
	tree = (options & DNS_DBFIND_FORCENSEC3) != 0 ? search.qpdb->nsec3
						      : search.qpdb->tree;
	result = dns_qp_lookup(tree, name, foundname, &search.iter,
			       &search.chain, (void **)&node, NULL);

	/*
	 * Check the QP chain to see if there's a node above us with a
	 * active DNAME or NS rdatasets.
	 *
	 * We're only interested in nodes above QNAME, so if the result
	 * was success, then we skip the last item in the chain.
	 */
	unsigned int len = dns_qpchain_length(&search.chain);
	if (result == ISC_R_SUCCESS) {
		len--;
	}

	for (unsigned int i = 0; i < len; i++) {
		isc_result_t zcresult;
		dns_rbtnode_t *encloser = NULL;

		dns_qpchain_node(&search.chain, i, NULL, (void **)&encloser,
				 NULL);

		if (encloser->find_callback) {
			zcresult = check_zonecut(
				encloser, (void *)&search DNS__DB_FLARG_PASS);
			if (zcresult != DNS_R_CONTINUE) {
				result = DNS_R_PARTIALMATCH;
				search.chain.len = i - 1;
				node = encloser;
				break;
			}
		}
	}

	if (result == DNS_R_PARTIALMATCH) {
	partial_match:
		if (search.zonecut != NULL) {
			result = setup_delegation(
				&search, nodep, foundname, rdataset,
				sigrdataset DNS__DB_FLARG_PASS);
			goto tree_exit;
		}

		if (search.wild) {
			/*
			 * At least one of the levels in the search chain
			 * potentially has a wildcard.  For each such level,
			 * we must see if there's a matching wildcard active
			 * in the current version.
			 */
			result = find_wildcard(&search, &node, name);
			if (result == ISC_R_SUCCESS) {
				dns_name_copy(name, foundname);
				wild = true;
				goto found;
			} else if (result != ISC_R_NOTFOUND) {
				goto tree_exit;
			}
		}

		active = false;
		if ((options & DNS_DBFIND_FORCENSEC3) == 0) {
			/*
			 * The NSEC3 tree won't have empty nodes,
			 * so it isn't necessary to check for them.
			 */
			dns_qpiter_t iter = search.iter;
			active = activeempty(&search, &iter, name);
		}

		/*
		 * If we're here, then the name does not exist, is not
		 * beneath a zonecut, and there's no matching wildcard.
		 */
		if ((search.rbtversion->secure &&
		     !search.rbtversion->havensec3) ||
		    (search.options & DNS_DBFIND_FORCENSEC3) != 0)
		{
			result = find_closest_nsec(
				&search, nodep, foundname, rdataset,
				sigrdataset, tree,
				search.rbtversion->secure DNS__DB_FLARG_PASS);
			if (result == ISC_R_SUCCESS) {
				result = active ? DNS_R_EMPTYNAME
						: DNS_R_NXDOMAIN;
			}
		} else {
			result = active ? DNS_R_EMPTYNAME : DNS_R_NXDOMAIN;
		}
		goto tree_exit;
	} else if (result != ISC_R_SUCCESS) {
		goto tree_exit;
	}

found:
	/*
	 * We have found a node whose name is the desired name, or we
	 * have matched a wildcard.
	 */

	lock = &search.qpdb->node_locks[node->locknum].lock;
	NODE_RDLOCK(lock, &nlocktype);

	if (search.zonecut != NULL) {
		/*
		 * If we're beneath a zone cut, we don't want to look for
		 * CNAMEs because they're not legitimate zone glue.
		 */
		cname_ok = false;
	} else {
		/*
		 * The node may be a zone cut itself.  If it might be one,
		 * make sure we check for it later.
		 *
		 * DS records live above the zone cut in ordinary zone so
		 * we want to ignore any referral.
		 *
		 * Stub zones don't have anything "above" the delegation so
		 * we always return a referral.
		 */
		if (node->find_callback && ((node != search.qpdb->origin_node &&
					     !dns_rdatatype_atparent(type)) ||
					    IS_STUB(search.qpdb)))
		{
			maybe_zonecut = true;
		}
	}

	/*
	 * Certain DNSSEC types are not subject to CNAME matching
	 * (RFC4035, section 2.5 and RFC3007).
	 *
	 * We don't check for RRSIG, because we don't store RRSIG records
	 * directly.
	 */
	if (type == dns_rdatatype_key || type == dns_rdatatype_nsec) {
		cname_ok = false;
	}

	/*
	 * We now go looking for rdata...
	 */

	found = NULL;
	foundsig = NULL;
	sigtype = DNS_SIGTYPE(type);
	nsecheader = NULL;
	nsecsig = NULL;
	cnamesig = NULL;
	empty_node = true;
	for (header = node->data; header != NULL; header = header_next) {
		header_next = header->next;
		/*
		 * Look for an active, extant rdataset.
		 */
		do {
			if (header->serial <= search.serial && !IGNORE(header))
			{
				/*
				 * Is this a "this rdataset doesn't
				 * exist" record?
				 */
				if (NONEXISTENT(header)) {
					header = NULL;
				}
				break;
			} else {
				header = header->down;
			}
		} while (header != NULL);
		if (header != NULL) {
			/*
			 * We now know that there is at least one active
			 * rdataset at this node.
			 */
			empty_node = false;

			/*
			 * Do special zone cut handling, if requested.
			 */
			if (maybe_zonecut && header->type == dns_rdatatype_ns) {
				/*
				 * We increment the reference count on node to
				 * ensure that search->zonecut_header will
				 * still be valid later.
				 */
				dns__qpdb_newref(search.qpdb, node,
						 nlocktype DNS__DB_FLARG_PASS);
				search.zonecut = node;
				search.zonecut_header = header;
				search.zonecut_sigheader = NULL;
				search.need_cleanup = true;
				maybe_zonecut = false;
				at_zonecut = true;
				/*
				 * It is not clear if KEY should still be
				 * allowed at the parent side of the zone
				 * cut or not.  It is needed for RFC3007
				 * validated updates.
				 */
				if ((search.options & DNS_DBFIND_GLUEOK) == 0 &&
				    type != dns_rdatatype_nsec &&
				    type != dns_rdatatype_key)
				{
					/*
					 * Glue is not OK, but any answer we
					 * could return would be glue.  Return
					 * the delegation.
					 */
					found = NULL;
					break;
				}
				if (found != NULL && foundsig != NULL) {
					break;
				}
			}

			/*
			 * If the NSEC3 record doesn't match the chain
			 * we are using behave as if it isn't here.
			 */
			if (header->type == dns_rdatatype_nsec3 &&
			    !matchparams(header, &search))
			{
				NODE_UNLOCK(lock, &nlocktype);
				goto partial_match;
			}
			/*
			 * If we found a type we were looking for,
			 * remember it.
			 */
			if (header->type == type || type == dns_rdatatype_any ||
			    (header->type == dns_rdatatype_cname && cname_ok))
			{
				/*
				 * We've found the answer!
				 */
				found = header;
				if (header->type == dns_rdatatype_cname &&
				    cname_ok)
				{
					/*
					 * We may be finding a CNAME instead
					 * of the desired type.
					 *
					 * If we've already got the CNAME RRSIG,
					 * use it, otherwise change sigtype
					 * so that we find it.
					 */
					if (cnamesig != NULL) {
						foundsig = cnamesig;
					} else {
						sigtype = DNS_SIGTYPE(
							dns_rdatatype_cname);
					}
				}
				/*
				 * If we've got all we need, end the search.
				 */
				if (!maybe_zonecut && foundsig != NULL) {
					break;
				}
			} else if (header->type == sigtype) {
				/*
				 * We've found the RRSIG rdataset for our
				 * target type.  Remember it.
				 */
				foundsig = header;
				/*
				 * If we've got all we need, end the search.
				 */
				if (!maybe_zonecut && found != NULL) {
					break;
				}
			} else if (header->type == dns_rdatatype_nsec &&
				   !search.rbtversion->havensec3)
			{
				/*
				 * Remember a NSEC rdataset even if we're
				 * not specifically looking for it, because
				 * we might need it later.
				 */
				nsecheader = header;
			} else if (header->type ==
					   DNS_SIGTYPE(dns_rdatatype_nsec) &&
				   !search.rbtversion->havensec3)
			{
				/*
				 * If we need the NSEC rdataset, we'll also
				 * need its signature.
				 */
				nsecsig = header;
			} else if (cname_ok &&
				   header->type ==
					   DNS_SIGTYPE(dns_rdatatype_cname))
			{
				/*
				 * If we get a CNAME match, we'll also need
				 * its signature.
				 */
				cnamesig = header;
			}
		}
	}

	if (empty_node) {
		/*
		 * We have an exact match for the name, but there are no
		 * active rdatasets in the desired version.  That means that
		 * this node doesn't exist in the desired version, and that
		 * we really have a partial match.
		 */
		if (!wild) {
			NODE_UNLOCK(lock, &nlocktype);
			goto partial_match;
		}
	}

	/*
	 * If we didn't find what we were looking for...
	 */
	if (found == NULL) {
		if (search.zonecut != NULL) {
			/*
			 * We were trying to find glue at a node beneath a
			 * zone cut, but didn't.
			 *
			 * Return the delegation.
			 */
			NODE_UNLOCK(lock, &nlocktype);
			result = setup_delegation(
				&search, nodep, foundname, rdataset,
				sigrdataset DNS__DB_FLARG_PASS);
			goto tree_exit;
		}
		/*
		 * The desired type doesn't exist.
		 */
		result = DNS_R_NXRRSET;
		if (search.rbtversion->secure &&
		    !search.rbtversion->havensec3 &&
		    (nsecheader == NULL || nsecsig == NULL))
		{
			/*
			 * The zone is secure but there's no NSEC,
			 * or the NSEC has no signature!
			 */
			if (!wild) {
				result = DNS_R_BADDB;
				goto node_exit;
			}

			NODE_UNLOCK(lock, &nlocktype);
			result = find_closest_nsec(
				&search, nodep, foundname, rdataset,
				sigrdataset, search.qpdb->tree,
				search.rbtversion->secure DNS__DB_FLARG_PASS);
			if (result == ISC_R_SUCCESS) {
				result = DNS_R_EMPTYWILD;
			}
			goto tree_exit;
		}
		if (nodep != NULL) {
			dns__qpdb_newref(search.qpdb, node,
					 nlocktype DNS__DB_FLARG_PASS);
			*nodep = node;
		}
		if ((search.rbtversion->secure &&
		     !search.rbtversion->havensec3))
		{
			dns__qpdb_bindrdataset(search.qpdb, node, nsecheader, 0,
					       nlocktype,
					       rdataset DNS__DB_FLARG_PASS);
			if (nsecsig != NULL) {
				dns__qpdb_bindrdataset(
					search.qpdb, node, nsecsig, 0,
					nlocktype,
					sigrdataset DNS__DB_FLARG_PASS);
			}
		}
		if (wild) {
			foundname->attributes.wildcard = true;
		}
		goto node_exit;
	}

	/*
	 * We found what we were looking for, or we found a CNAME.
	 */

	if (type != found->type && type != dns_rdatatype_any &&
	    found->type == dns_rdatatype_cname)
	{
		/*
		 * We weren't doing an ANY query and we found a CNAME instead
		 * of the type we were looking for, so we need to indicate
		 * that result to the caller.
		 */
		result = DNS_R_CNAME;
	} else if (search.zonecut != NULL) {
		/*
		 * If we're beneath a zone cut, we must indicate that the
		 * result is glue, unless we're actually at the zone cut
		 * and the type is NSEC or KEY.
		 */
		if (search.zonecut == node) {
			/*
			 * It is not clear if KEY should still be
			 * allowed at the parent side of the zone
			 * cut or not.  It is needed for RFC3007
			 * validated updates.
			 */
			if (type == dns_rdatatype_nsec ||
			    type == dns_rdatatype_nsec3 ||
			    type == dns_rdatatype_key)
			{
				result = ISC_R_SUCCESS;
			} else if (type == dns_rdatatype_any) {
				result = DNS_R_ZONECUT;
			} else {
				result = DNS_R_GLUE;
			}
		} else {
			result = DNS_R_GLUE;
		}
	} else {
		/*
		 * An ordinary successful query!
		 */
		result = ISC_R_SUCCESS;
	}

	if (nodep != NULL) {
		if (!at_zonecut) {
			dns__qpdb_newref(search.qpdb, node,
					 nlocktype DNS__DB_FLARG_PASS);
		} else {
			search.need_cleanup = false;
		}
		*nodep = node;
	}

	if (type != dns_rdatatype_any) {
		dns__qpdb_bindrdataset(search.qpdb, node, found, 0, nlocktype,
				       rdataset DNS__DB_FLARG_PASS);
		if (foundsig != NULL) {
			dns__qpdb_bindrdataset(search.qpdb, node, foundsig, 0,
					       nlocktype,
					       sigrdataset DNS__DB_FLARG_PASS);
		}
	}

	if (wild) {
		foundname->attributes.wildcard = true;
	}

node_exit:
	NODE_UNLOCK(lock, &nlocktype);

tree_exit:
	TREE_UNLOCK(&search.qpdb->tree_lock, &tlocktype);

	/*
	 * If we found a zonecut but aren't going to use it, we have to
	 * let go of it.
	 */
	if (search.need_cleanup) {
		node = search.zonecut;
		INSIST(node != NULL);
		lock = &(search.qpdb->node_locks[node->locknum].lock);

		NODE_RDLOCK(lock, &nlocktype);
		dns__qpdb_decref(search.qpdb, node, 0, &nlocktype, &tlocktype,
				 true, false DNS__DB_FLARG_PASS);
		NODE_UNLOCK(lock, &nlocktype);
		INSIST(tlocktype == isc_rwlocktype_none);
	}

	if (close_version) {
		dns__qpdb_closeversion(db, &version, false DNS__DB_FLARG_PASS);
	}

	return (result);
}

static isc_result_t
zone_findrdataset(dns_db_t *db, dns_dbnode_t *node, dns_dbversion_t *version,
		  dns_rdatatype_t type, dns_rdatatype_t covers,
		  isc_stdtime_t now, dns_rdataset_t *rdataset,
		  dns_rdataset_t *sigrdataset DNS__DB_FLARG) {
	dns_qpdb_t *qpdb = (dns_qpdb_t *)db;
	dns_rbtnode_t *rbtnode = (dns_rbtnode_t *)node;
	dns_slabheader_t *header = NULL, *header_next = NULL;
	dns_slabheader_t *found = NULL, *foundsig = NULL;
	uint32_t serial;
	dns_qpdb_version_t *rbtversion = version;
	bool close_version = false;
	dns_typepair_t matchtype, sigmatchtype;
	isc_rwlocktype_t nlocktype = isc_rwlocktype_none;

	REQUIRE(VALID_QPDB(qpdb));
	REQUIRE(type != dns_rdatatype_any);
	INSIST(rbtversion == NULL || rbtversion->qpdb == qpdb);

	if (rbtversion == NULL) {
		dns__qpdb_currentversion(
			db, (dns_dbversion_t **)(void *)(&rbtversion));
		close_version = true;
	}
	serial = rbtversion->serial;
	now = 0;

	NODE_RDLOCK(&qpdb->node_locks[rbtnode->locknum].lock, &nlocktype);

	matchtype = DNS_TYPEPAIR_VALUE(type, covers);
	if (covers == 0) {
		sigmatchtype = DNS_SIGTYPE(type);
	} else {
		sigmatchtype = 0;
	}

	for (header = rbtnode->data; header != NULL; header = header_next) {
		header_next = header->next;
		do {
			if (header->serial <= serial && !IGNORE(header)) {
				/*
				 * Is this a "this rdataset doesn't
				 * exist" record?
				 */
				if (NONEXISTENT(header)) {
					header = NULL;
				}
				break;
			} else {
				header = header->down;
			}
		} while (header != NULL);
		if (header != NULL) {
			/*
			 * We have an active, extant rdataset.  If it's a
			 * type we're looking for, remember it.
			 */
			if (header->type == matchtype) {
				found = header;
				if (foundsig != NULL) {
					break;
				}
			} else if (header->type == sigmatchtype) {
				foundsig = header;
				if (found != NULL) {
					break;
				}
			}
		}
	}
	if (found != NULL) {
		dns__qpdb_bindrdataset(qpdb, rbtnode, found, now,
				       isc_rwlocktype_read,
				       rdataset DNS__DB_FLARG_PASS);
		if (foundsig != NULL) {
			dns__qpdb_bindrdataset(qpdb, rbtnode, foundsig, now,
					       isc_rwlocktype_read,
					       sigrdataset DNS__DB_FLARG_PASS);
		}
	}

	NODE_UNLOCK(&qpdb->node_locks[rbtnode->locknum].lock, &nlocktype);

	if (close_version) {
		dns__qpdb_closeversion(
			db, (dns_dbversion_t **)(void *)(&rbtversion),
			false DNS__DB_FLARG_PASS);
	}

	if (found == NULL) {
		return (ISC_R_NOTFOUND);
	}

	return (ISC_R_SUCCESS);
}

static bool
delegating_type(dns_qpdb_t *qpdb, dns_rbtnode_t *node, dns_typepair_t type) {
	if (type == dns_rdatatype_dname ||
	    (type == dns_rdatatype_ns &&
	     (node != qpdb->origin_node || IS_STUB(qpdb))))
	{
		return (true);
	}
	return (false);
}

/*
 * load a non-NSEC3 node in the main tree and optionally to the auxiliary NSEC
 */
static isc_result_t
loadnode(dns_qpdb_t *qpdb, const dns_name_t *name, dns_rbtnode_t **nodep,
	 bool hasnsec) {
	isc_result_t noderesult, nsecresult;
	dns_rbtnode_t *nsecnode = NULL, *node = NULL;

	noderesult = dns_qp_getname(qpdb->tree, name, (void **)&node, NULL);
	if (noderesult != ISC_R_SUCCESS) {
		INSIST(node == NULL);
		node = dns_qpdata_create(qpdb, name);
		noderesult = dns_qp_insert(qpdb->tree, node, 0);
		INSIST(noderesult == ISC_R_SUCCESS);
		dns_qpdata_unref(node);
	} else if (noderesult == ISC_R_SUCCESS) {
		/*
		 * Add a node to the auxiliary NSEC tree for an old node
		 * just now getting an NSEC record.
		 */
		if (node->nsec == DNS_DB_NSEC_HAS_NSEC) {
			goto done;
		}
	} else {
		goto done;
	}

	if (!hasnsec) {
		goto done;
	}

	/*
	 * Build the auxiliary tree for NSECs as we go.
	 * This tree speeds searches for closest NSECs that would otherwise
	 * need to examine many irrelevant nodes in large TLDs.
	 *
	 * Add nodes to the auxiliary tree after corresponding nodes have
	 * been added to the main tree.
	 */
	nsecresult = dns_qp_getname(qpdb->nsec, name, (void **)&nsecnode, NULL);
	if (nsecresult == ISC_R_SUCCESS) {
#if 1 /* 0 */
		isc_log_write(dns_lctx, DNS_LOGCATEGORY_DATABASE,
			      DNS_LOGMODULE_CACHE, ISC_LOG_WARNING,
			      "addnode: NSEC node already exists");
#endif /* if 1 */
		node->nsec = DNS_DB_NSEC_HAS_NSEC;
		goto done;
	} else {
		INSIST(nsecnode == NULL);
		nsecnode = dns_qpdata_create(qpdb, name);
		nsecresult = dns_qp_insert(qpdb->nsec, nsecnode, 0);
		INSIST(nsecresult == ISC_R_SUCCESS);
	}
	nsecnode->nsec = DNS_DB_NSEC_NSEC;
	node->nsec = DNS_DB_NSEC_HAS_NSEC;

done:
	if (noderesult == ISC_R_SUCCESS || noderesult == ISC_R_EXISTS) {
		*nodep = node;
	}

	return (noderesult);
}

static isc_result_t
loading_addrdataset(void *arg, const dns_name_t *name,
		    dns_rdataset_t *rdataset DNS__DB_FLARG) {
	qpdb_load_t *loadctx = arg;
	dns_qpdb_t *qpdb = (dns_qpdb_t *)loadctx->db;
	dns_rbtnode_t *node = NULL;
	isc_result_t result;
	isc_region_t region;
	dns_slabheader_t *newheader = NULL;
	isc_rwlocktype_t nlocktype = isc_rwlocktype_none;

	REQUIRE(rdataset->rdclass == qpdb->common.rdclass);

	/*
	 * SOA records are only allowed at top of zone.
	 */
	if (rdataset->type == dns_rdatatype_soa &&
	    !dns_name_equal(name, &qpdb->common.origin))
	{
		return (DNS_R_NOTZONETOP);
	}

	if (rdataset->type != dns_rdatatype_nsec3 &&
	    rdataset->covers != dns_rdatatype_nsec3)
	{
		dns__qpzone_addwildcards(qpdb, name, false);
	}

	if (dns_name_iswildcard(name)) {
		/*
		 * NS record owners cannot legally be wild cards.
		 */
		if (rdataset->type == dns_rdatatype_ns) {
			return (DNS_R_INVALIDNS);
		}
		/*
		 * NSEC3 record owners cannot legally be wild cards.
		 */
		if (rdataset->type == dns_rdatatype_nsec3) {
			return (DNS_R_INVALIDNSEC3);
		}
		result = dns__qpzone_wildcardmagic(qpdb, name, false);
		if (result != ISC_R_SUCCESS) {
			return (result);
		}
	}

	if (rdataset->type == dns_rdatatype_nsec3 ||
	    rdataset->covers == dns_rdatatype_nsec3)
	{
		result = dns_qp_getname(qpdb->nsec3, name, (void **)&node,
					NULL);
		if (result != ISC_R_SUCCESS) {
			INSIST(node == NULL);
			node = dns_qpdata_create(qpdb, name);
			result = dns_qp_insert(qpdb->nsec3, node, 0);
			INSIST(result == ISC_R_SUCCESS);
		}
		node->nsec = DNS_DB_NSEC_NSEC3;
	} else if (rdataset->type == dns_rdatatype_nsec) {
		result = loadnode(qpdb, name, &node, true);
	} else {
		result = loadnode(qpdb, name, &node, false);
	}
	if (result != ISC_R_SUCCESS && result != ISC_R_EXISTS) {
		return (result);
	}
	if (result == ISC_R_SUCCESS) {
		node->locknum = node->hashval % qpdb->node_lock_count;
	}

	result = dns_rdataslab_fromrdataset(rdataset, qpdb->common.mctx,
					    &region, sizeof(dns_slabheader_t));
	if (result != ISC_R_SUCCESS) {
		return (result);
	}
	newheader = (dns_slabheader_t *)region.base;
	*newheader = (dns_slabheader_t){
		.type = DNS_TYPEPAIR_VALUE(rdataset->type, rdataset->covers),
		.ttl = rdataset->ttl + loadctx->now,
		.trust = rdataset->trust,
		.node = node,
		.serial = 1,
		.count = 1,
	};

	dns_slabheader_reset(newheader, (dns_db_t *)qpdb, node);
	dns_slabheader_setownercase(newheader, name);

	if ((rdataset->attributes & DNS_RDATASETATTR_RESIGN) != 0) {
		DNS_SLABHEADER_SETATTR(newheader, DNS_SLABHEADERATTR_RESIGN);
		newheader->resign =
			(isc_stdtime_t)(dns_time64_from32(rdataset->resign) >>
					1);
		newheader->resign_lsb = rdataset->resign & 0x1;
	}

	NODE_WRLOCK(&qpdb->node_locks[node->locknum].lock, &nlocktype);
	result = dns__qpdb_add(qpdb, node, name, qpdb->current_version,
			       newheader, DNS_DBADD_MERGE, true, NULL,
			       0 DNS__DB_FLARG_PASS);
	NODE_UNLOCK(&qpdb->node_locks[node->locknum].lock, &nlocktype);

	if (result == ISC_R_SUCCESS &&
	    delegating_type(qpdb, node, rdataset->type))
	{
		node->find_callback = 1;
	} else if (result == DNS_R_UNCHANGED) {
		result = ISC_R_SUCCESS;
	}

	return (result);
}

static isc_result_t
beginload(dns_db_t *db, dns_rdatacallbacks_t *callbacks) {
	qpdb_load_t *loadctx = NULL;
	dns_qpdb_t *qpdb = (dns_qpdb_t *)db;

	REQUIRE(DNS_CALLBACK_VALID(callbacks));
	REQUIRE(VALID_QPDB(qpdb));

	loadctx = isc_mem_get(qpdb->common.mctx, sizeof(*loadctx));

	loadctx->db = db;
	loadctx->now = 0;

	RWLOCK(&qpdb->lock, isc_rwlocktype_write);

	REQUIRE((qpdb->attributes & (QPDB_ATTR_LOADED | QPDB_ATTR_LOADING)) ==
		0);
	qpdb->attributes |= QPDB_ATTR_LOADING;

	RWUNLOCK(&qpdb->lock, isc_rwlocktype_write);

	callbacks->add = loading_addrdataset;
	callbacks->add_private = loadctx;

	return (ISC_R_SUCCESS);
}

static isc_result_t
endload(dns_db_t *db, dns_rdatacallbacks_t *callbacks) {
	qpdb_load_t *loadctx = NULL;
	dns_qpdb_t *qpdb = (dns_qpdb_t *)db;

	REQUIRE(VALID_QPDB(qpdb));
	REQUIRE(DNS_CALLBACK_VALID(callbacks));
	loadctx = callbacks->add_private;
	REQUIRE(loadctx != NULL);
	REQUIRE(loadctx->db == db);

	RWLOCK(&qpdb->lock, isc_rwlocktype_write);

	REQUIRE((qpdb->attributes & QPDB_ATTR_LOADING) != 0);
	REQUIRE((qpdb->attributes & QPDB_ATTR_LOADED) == 0);

	qpdb->attributes &= ~QPDB_ATTR_LOADING;
	qpdb->attributes |= QPDB_ATTR_LOADED;

	/*
	 * If there's a KEY rdataset at the zone origin containing a
	 * zone key, we consider the zone secure.
	 */
	if (qpdb->origin_node != NULL) {
		dns_dbversion_t *version = qpdb->current_version;
		RWUNLOCK(&qpdb->lock, isc_rwlocktype_write);
		dns__qpdb_setsecure(db, version, qpdb->origin_node);
	} else {
		RWUNLOCK(&qpdb->lock, isc_rwlocktype_write);
	}

	callbacks->add = NULL;
	callbacks->add_private = NULL;

	isc_mem_put(qpdb->common.mctx, loadctx, sizeof(*loadctx));

	return (ISC_R_SUCCESS);
}

static bool
issecure(dns_db_t *db) {
	dns_qpdb_t *qpdb = (dns_qpdb_t *)db;
	bool secure;

	REQUIRE(VALID_QPDB(qpdb));

	RWLOCK(&qpdb->lock, isc_rwlocktype_read);
	secure = qpdb->current_version->secure;
	RWUNLOCK(&qpdb->lock, isc_rwlocktype_read);

	return (secure);
}

static isc_result_t
getnsec3parameters(dns_db_t *db, dns_dbversion_t *version, dns_hash_t *hash,
		   uint8_t *flags, uint16_t *iterations, unsigned char *salt,
		   size_t *salt_length) {
	dns_qpdb_t *qpdb = (dns_qpdb_t *)db;
	isc_result_t result = ISC_R_NOTFOUND;
	dns_qpdb_version_t *rbtversion = version;

	REQUIRE(VALID_QPDB(qpdb));
	INSIST(rbtversion == NULL || rbtversion->qpdb == qpdb);

	RWLOCK(&qpdb->lock, isc_rwlocktype_read);
	if (rbtversion == NULL) {
		rbtversion = qpdb->current_version;
	}

	if (rbtversion->havensec3) {
		if (hash != NULL) {
			*hash = rbtversion->hash;
		}
		if (salt != NULL && salt_length != NULL) {
			REQUIRE(*salt_length >= rbtversion->salt_length);
			memmove(salt, rbtversion->salt,
				rbtversion->salt_length);
		}
		if (salt_length != NULL) {
			*salt_length = rbtversion->salt_length;
		}
		if (iterations != NULL) {
			*iterations = rbtversion->iterations;
		}
		if (flags != NULL) {
			*flags = rbtversion->flags;
		}
		result = ISC_R_SUCCESS;
	}
	RWUNLOCK(&qpdb->lock, isc_rwlocktype_read);

	return (result);
}

static isc_result_t
getsize(dns_db_t *db, dns_dbversion_t *version, uint64_t *records,
	uint64_t *xfrsize) {
	dns_qpdb_t *qpdb = (dns_qpdb_t *)db;
	isc_result_t result = ISC_R_SUCCESS;
	dns_qpdb_version_t *rbtversion = version;

	REQUIRE(VALID_QPDB(qpdb));
	INSIST(rbtversion == NULL || rbtversion->qpdb == qpdb);

	RWLOCK(&qpdb->lock, isc_rwlocktype_read);
	if (rbtversion == NULL) {
		rbtversion = qpdb->current_version;
	}

	RWLOCK(&rbtversion->rwlock, isc_rwlocktype_read);
	SET_IF_NOT_NULL(records, rbtversion->records);

	SET_IF_NOT_NULL(xfrsize, rbtversion->xfrsize);
	RWUNLOCK(&rbtversion->rwlock, isc_rwlocktype_read);
	RWUNLOCK(&qpdb->lock, isc_rwlocktype_read);

	return (result);
}

static isc_result_t
setsigningtime(dns_db_t *db, dns_rdataset_t *rdataset, isc_stdtime_t resign) {
	dns_qpdb_t *qpdb = (dns_qpdb_t *)db;
	dns_slabheader_t *header, oldheader;
	isc_rwlocktype_t nlocktype = isc_rwlocktype_none;

	REQUIRE(VALID_QPDB(qpdb));
	REQUIRE(!IS_CACHE(qpdb));
	REQUIRE(rdataset != NULL);
	REQUIRE(rdataset->methods == &dns_rdataslab_rdatasetmethods);

	header = dns_slabheader_fromrdataset(rdataset);

	NODE_WRLOCK(&qpdb->node_locks[QPDB_HEADERNODE(header)->locknum].lock,
		    &nlocktype);

	oldheader = *header;

	/*
	 * Only break the heap invariant (by adjusting resign and resign_lsb)
	 * if we are going to be restoring it by calling isc_heap_increased
	 * or isc_heap_decreased.
	 */
	if (resign != 0) {
		header->resign = (isc_stdtime_t)(dns_time64_from32(resign) >>
						 1);
		header->resign_lsb = resign & 0x1;
	}
	if (header->heap_index != 0) {
		INSIST(RESIGN(header));
		if (resign == 0) {
			isc_heap_delete(
				qpdb->heaps[QPDB_HEADERNODE(header)->locknum],
				header->heap_index);
			header->heap_index = 0;
			header->heap = NULL;
		} else if (qpdb->sooner(header, &oldheader)) {
			isc_heap_increased(
				qpdb->heaps[QPDB_HEADERNODE(header)->locknum],
				header->heap_index);
		} else if (qpdb->sooner(&oldheader, header)) {
			isc_heap_decreased(
				qpdb->heaps[QPDB_HEADERNODE(header)->locknum],
				header->heap_index);
		}
	} else if (resign != 0) {
		DNS_SLABHEADER_SETATTR(header, DNS_SLABHEADERATTR_RESIGN);
		dns__qpzone_resigninsert(qpdb, QPDB_HEADERNODE(header)->locknum,
					 header);
	}
	NODE_UNLOCK(&qpdb->node_locks[QPDB_HEADERNODE(header)->locknum].lock,
		    &nlocktype);
	return (ISC_R_SUCCESS);
}

static isc_result_t
getsigningtime(dns_db_t *db, dns_rdataset_t *rdataset,
	       dns_name_t *foundname DNS__DB_FLARG) {
	dns_qpdb_t *qpdb = (dns_qpdb_t *)db;
	dns_slabheader_t *header = NULL, *this = NULL;
	unsigned int i;
	isc_result_t result = ISC_R_NOTFOUND;
	unsigned int locknum = 0;
	isc_rwlocktype_t tlocktype = isc_rwlocktype_none;
	isc_rwlocktype_t nlocktype = isc_rwlocktype_none;

	REQUIRE(VALID_QPDB(qpdb));

	TREE_RDLOCK(&qpdb->tree_lock, &tlocktype);

	for (i = 0; i < qpdb->node_lock_count; i++) {
		NODE_RDLOCK(&qpdb->node_locks[i].lock, &nlocktype);

		/*
		 * Find for the earliest signing time among all of the
		 * heaps, each of which is covered by a different bucket
		 * lock.
		 */
		this = isc_heap_element(qpdb->heaps[i], 1);
		if (this == NULL) {
			/* Nothing found; unlock and try the next heap. */
			NODE_UNLOCK(&qpdb->node_locks[i].lock, &nlocktype);
			continue;
		}

		if (header == NULL) {
			/*
			 * Found a signing time: retain the bucket lock and
			 * preserve the lock number so we can unlock it
			 * later.
			 */
			header = this;
			locknum = i;
			nlocktype = isc_rwlocktype_none;
		} else if (qpdb->sooner(this, header)) {
			/*
			 * Found an earlier signing time; release the
			 * previous bucket lock and retain this one instead.
			 */
			NODE_UNLOCK(&qpdb->node_locks[locknum].lock,
				    &nlocktype);
			header = this;
			locknum = i;
		} else {
			/*
			 * Earliest signing time in this heap isn't
			 * an improvement; unlock and try the next heap.
			 */
			NODE_UNLOCK(&qpdb->node_locks[i].lock, &nlocktype);
		}
	}

	if (header != NULL) {
		nlocktype = isc_rwlocktype_read;
		/*
		 * Found something; pass back the answer and unlock
		 * the bucket.
		 */
		dns__qpdb_bindrdataset(qpdb, QPDB_HEADERNODE(header), header, 0,
				       isc_rwlocktype_read,
				       rdataset DNS__DB_FLARG_PASS);

		if (foundname != NULL) {
			dns_name_copy(QPDB_HEADERNODE(header)->name, foundname);
		}

		NODE_UNLOCK(&qpdb->node_locks[locknum].lock, &nlocktype);

		result = ISC_R_SUCCESS;
	}

	TREE_UNLOCK(&qpdb->tree_lock, &tlocktype);

	return (result);
}

static isc_result_t
setgluecachestats(dns_db_t *db, isc_stats_t *stats) {
	dns_qpdb_t *qpdb = (dns_qpdb_t *)db;

	REQUIRE(VALID_QPDB(qpdb));
	REQUIRE(!IS_CACHE(qpdb) && !IS_STUB(qpdb));
	REQUIRE(stats != NULL);

	isc_stats_attach(stats, &qpdb->gluecachestats);
	return (ISC_R_SUCCESS);
}

static dns_glue_t *
new_gluelist(isc_mem_t *mctx, dns_name_t *name) {
	dns_glue_t *glue = isc_mem_get(mctx, sizeof(*glue));
	*glue = (dns_glue_t){ 0 };
	dns_name_t *gluename = dns_fixedname_initname(&glue->fixedname);

	isc_mem_attach(mctx, &glue->mctx);
	dns_name_copy(name, gluename);

	return (glue);
}

static isc_result_t
glue_nsdname_cb(void *arg, const dns_name_t *name, dns_rdatatype_t qtype,
		dns_rdataset_t *unused DNS__DB_FLARG) {
	dns_glue_additionaldata_ctx_t *ctx = NULL;
	isc_result_t result;
	dns_fixedname_t fixedname_a;
	dns_name_t *name_a = NULL;
	dns_rdataset_t rdataset_a, sigrdataset_a;
	dns_rbtnode_t *node_a = NULL;
	dns_fixedname_t fixedname_aaaa;
	dns_name_t *name_aaaa = NULL;
	dns_rdataset_t rdataset_aaaa, sigrdataset_aaaa;
	dns_rbtnode_t *node_aaaa = NULL;
	dns_glue_t *glue = NULL;

	UNUSED(unused);

	/*
	 * NS records want addresses in additional records.
	 */
	INSIST(qtype == dns_rdatatype_a);

	ctx = (dns_glue_additionaldata_ctx_t *)arg;

	name_a = dns_fixedname_initname(&fixedname_a);
	dns_rdataset_init(&rdataset_a);
	dns_rdataset_init(&sigrdataset_a);

	name_aaaa = dns_fixedname_initname(&fixedname_aaaa);
	dns_rdataset_init(&rdataset_aaaa);
	dns_rdataset_init(&sigrdataset_aaaa);

	result = zone_find(ctx->db, name, ctx->version, dns_rdatatype_a,
			   DNS_DBFIND_GLUEOK, 0, (dns_dbnode_t **)&node_a,
			   name_a, &rdataset_a,
			   &sigrdataset_a DNS__DB_FLARG_PASS);
	if (result == DNS_R_GLUE) {
		glue = new_gluelist(ctx->db->mctx, name_a);

		dns_rdataset_init(&glue->rdataset_a);
		dns_rdataset_init(&glue->sigrdataset_a);
		dns_rdataset_init(&glue->rdataset_aaaa);
		dns_rdataset_init(&glue->sigrdataset_aaaa);

		dns_rdataset_clone(&rdataset_a, &glue->rdataset_a);
		if (dns_rdataset_isassociated(&sigrdataset_a)) {
			dns_rdataset_clone(&sigrdataset_a,
					   &glue->sigrdataset_a);
		}
	}

	result = zone_find(ctx->db, name, ctx->version, dns_rdatatype_aaaa,
			   DNS_DBFIND_GLUEOK, 0, (dns_dbnode_t **)&node_aaaa,
			   name_aaaa, &rdataset_aaaa,
			   &sigrdataset_aaaa DNS__DB_FLARG_PASS);
	if (result == DNS_R_GLUE) {
		if (glue == NULL) {
			glue = new_gluelist(ctx->db->mctx, name_aaaa);

			dns_rdataset_init(&glue->rdataset_a);
			dns_rdataset_init(&glue->sigrdataset_a);
			dns_rdataset_init(&glue->rdataset_aaaa);
			dns_rdataset_init(&glue->sigrdataset_aaaa);
		} else {
			INSIST(node_a == node_aaaa);
			INSIST(dns_name_equal(name_a, name_aaaa));
		}

		dns_rdataset_clone(&rdataset_aaaa, &glue->rdataset_aaaa);
		if (dns_rdataset_isassociated(&sigrdataset_aaaa)) {
			dns_rdataset_clone(&sigrdataset_aaaa,
					   &glue->sigrdataset_aaaa);
		}
	}

	/*
	 * If the currently processed NS record is in-bailiwick, mark any glue
	 * RRsets found for it with DNS_RDATASETATTR_REQUIRED.  Note that for
	 * simplicity, glue RRsets for all in-bailiwick NS records are marked
	 * this way, even though dns_message_rendersection() only checks the
	 * attributes for the first rdataset associated with the first name
	 * added to the ADDITIONAL section.
	 */
	if (glue != NULL && dns_name_issubdomain(name, ctx->nodename)) {
		if (dns_rdataset_isassociated(&glue->rdataset_a)) {
			glue->rdataset_a.attributes |=
				DNS_RDATASETATTR_REQUIRED;
		}
		if (dns_rdataset_isassociated(&glue->rdataset_aaaa)) {
			glue->rdataset_aaaa.attributes |=
				DNS_RDATASETATTR_REQUIRED;
		}
	}

	if (glue != NULL) {
		glue->next = ctx->glue_list;
		ctx->glue_list = glue;
	}

	result = ISC_R_SUCCESS;

	if (dns_rdataset_isassociated(&rdataset_a)) {
		dns_rdataset_disassociate(&rdataset_a);
	}
	if (dns_rdataset_isassociated(&sigrdataset_a)) {
		dns_rdataset_disassociate(&sigrdataset_a);
	}

	if (dns_rdataset_isassociated(&rdataset_aaaa)) {
		dns_rdataset_disassociate(&rdataset_aaaa);
	}
	if (dns_rdataset_isassociated(&sigrdataset_aaaa)) {
		dns_rdataset_disassociate(&sigrdataset_aaaa);
	}

	if (node_a != NULL) {
		dns__db_detachnode(ctx->db,
				   (dns_dbnode_t *)&node_a DNS__DB_FLARG_PASS);
	}
	if (node_aaaa != NULL) {
		dns__db_detachnode(
			ctx->db, (dns_dbnode_t *)&node_aaaa DNS__DB_FLARG_PASS);
	}

	return (result);
}

#define IS_REQUIRED_GLUE(r) (((r)->attributes & DNS_RDATASETATTR_REQUIRED) != 0)

static void
addglue_to_message(dns_glue_t *ge, dns_message_t *msg) {
	for (; ge != NULL; ge = ge->next) {
		dns_name_t *name = NULL;
		dns_rdataset_t *rdataset_a = NULL;
		dns_rdataset_t *sigrdataset_a = NULL;
		dns_rdataset_t *rdataset_aaaa = NULL;
		dns_rdataset_t *sigrdataset_aaaa = NULL;
		dns_name_t *gluename = dns_fixedname_name(&ge->fixedname);
		bool prepend_name = false;

		dns_message_gettempname(msg, &name);

		dns_name_copy(gluename, name);

		if (dns_rdataset_isassociated(&ge->rdataset_a)) {
			dns_message_gettemprdataset(msg, &rdataset_a);
		}

		if (dns_rdataset_isassociated(&ge->sigrdataset_a)) {
			dns_message_gettemprdataset(msg, &sigrdataset_a);
		}

		if (dns_rdataset_isassociated(&ge->rdataset_aaaa)) {
			dns_message_gettemprdataset(msg, &rdataset_aaaa);
		}

		if (dns_rdataset_isassociated(&ge->sigrdataset_aaaa)) {
			dns_message_gettemprdataset(msg, &sigrdataset_aaaa);
		}

		if (rdataset_a != NULL) {
			dns_rdataset_clone(&ge->rdataset_a, rdataset_a);
			ISC_LIST_APPEND(name->list, rdataset_a, link);
			if (IS_REQUIRED_GLUE(rdataset_a)) {
				prepend_name = true;
			}
		}

		if (sigrdataset_a != NULL) {
			dns_rdataset_clone(&ge->sigrdataset_a, sigrdataset_a);
			ISC_LIST_APPEND(name->list, sigrdataset_a, link);
		}

		if (rdataset_aaaa != NULL) {
			dns_rdataset_clone(&ge->rdataset_aaaa, rdataset_aaaa);
			ISC_LIST_APPEND(name->list, rdataset_aaaa, link);
			if (IS_REQUIRED_GLUE(rdataset_aaaa)) {
				prepend_name = true;
			}
		}
		if (sigrdataset_aaaa != NULL) {
			dns_rdataset_clone(&ge->sigrdataset_aaaa,
					   sigrdataset_aaaa);
			ISC_LIST_APPEND(name->list, sigrdataset_aaaa, link);
		}

		dns_message_addname(msg, name, DNS_SECTION_ADDITIONAL);

		/*
		 * When looking for required glue, dns_message_rendersection()
		 * only processes the first rdataset associated with the first
		 * name added to the ADDITIONAL section.  dns_message_addname()
		 * performs an append on the list of names in a given section,
		 * so if any glue record was marked as required, we need to
		 * move the name it is associated with to the beginning of the
		 * list for the ADDITIONAL section or else required glue might
		 * not be rendered.
		 */
		if (prepend_name) {
			ISC_LIST_UNLINK(msg->sections[DNS_SECTION_ADDITIONAL],
					name, link);
			ISC_LIST_PREPEND(msg->sections[DNS_SECTION_ADDITIONAL],
					 name, link);
		}
	}
}

static dns_glue_t *
newglue(dns_qpdb_t *qpdb, dns_qpdb_version_t *rbtversion, dns_rbtnode_t *node,
	dns_rdataset_t *rdataset) {
	dns_fixedname_t nodename;
	dns_glue_additionaldata_ctx_t ctx = {
		.db = (dns_db_t *)qpdb,
		.version = (dns_dbversion_t *)rbtversion,
		.nodename = dns_fixedname_initname(&nodename),
	};

	/*
	 * Get the owner name of the NS RRset - it will be necessary for
	 * identifying required glue in glue_nsdname_cb() (by
	 * determining which NS records in the delegation are
	 * in-bailiwick).
	 */
	dns__qpdb_nodefullname((dns_db_t *)qpdb, node, ctx.nodename);

	(void)dns_rdataset_additionaldata(rdataset, dns_rootname,
					  glue_nsdname_cb, &ctx);

	return (ctx.glue_list);
}

static isc_result_t
addglue(dns_db_t *db, dns_dbversion_t *version, dns_rdataset_t *rdataset,
	dns_message_t *msg) {
	dns_qpdb_t *qpdb = (dns_qpdb_t *)db;
	dns_qpdb_version_t *rbtversion = version;
	dns_rbtnode_t *node = (dns_rbtnode_t *)rdataset->slab.node;
	dns_slabheader_t *header = dns_slabheader_fromrdataset(rdataset);

	REQUIRE(rdataset->type == dns_rdatatype_ns);
	REQUIRE(qpdb == (dns_qpdb_t *)rdataset->slab.db);
	REQUIRE(qpdb == rbtversion->qpdb);
	REQUIRE(!IS_CACHE(qpdb) && !IS_STUB(qpdb));

	rcu_read_lock();

	dns_glue_t *glue = rcu_dereference(header->glue_list);
	if (glue == NULL) {
		/* No cached glue was found in the table. Get new glue. */
		glue = newglue(qpdb, rbtversion, node, rdataset);

		/* Cache the glue or (void *)-1 if no glue was found. */
		dns_glue_t *old_glue = rcu_cmpxchg_pointer(
			&header->glue_list, NULL, (glue) ? glue : (void *)-1);
		if (old_glue != NULL) {
			/* Somebody else was faster */
			dns__qpdb_freeglue(glue);
			glue = old_glue;
		} else if (glue != NULL) {
			cds_wfs_push(&rbtversion->glue_stack,
				     &header->wfs_node);
		}
	}

	/* We have a cached result. Add it to the message and return. */

	if (qpdb->gluecachestats != NULL) {
		isc_stats_increment(
			qpdb->gluecachestats,
			(glue == (void *)-1)
				? dns_gluecachestatscounter_hits_absent
				: dns_gluecachestatscounter_hits_present);
	}

	/*
	 * (void *)-1 is a special value that means no glue is present in the
	 * zone.
	 */
	if (glue != (void *)-1) {
		addglue_to_message(glue, msg);
	}

	rcu_read_unlock();

	return (ISC_R_SUCCESS);
}

dns_dbmethods_t dns__qpdb_zonemethods = {
	.destroy = dns__qpdb_destroy,
	.beginload = beginload,
	.endload = endload,
	.currentversion = dns__qpdb_currentversion,
	.newversion = dns__qpdb_newversion,
	.attachversion = dns__qpdb_attachversion,
	.closeversion = dns__qpdb_closeversion,
	.findnode = dns__qpdb_findnode,
	.find = zone_find,
	.attachnode = dns__qpdb_attachnode,
	.detachnode = dns__qpdb_detachnode,
	.createiterator = dns__qpdb_createiterator,
	.findrdataset = zone_findrdataset,
	.allrdatasets = dns__qpdb_allrdatasets,
	.addrdataset = dns__qpdb_addrdataset,
	.subtractrdataset = dns__qpdb_subtractrdataset,
	.deleterdataset = dns__qpdb_deleterdataset,
	.issecure = issecure,
	.nodecount = dns__qpdb_nodecount,
	.setloop = dns__qpdb_setloop,
	.getoriginnode = dns__qpdb_getoriginnode,
	.getnsec3parameters = getnsec3parameters,
	.findnsec3node = findnsec3node,
	.setsigningtime = setsigningtime,
	.getsigningtime = getsigningtime,
	.getsize = getsize,
	.setgluecachestats = setgluecachestats,
	.locknode = dns__qpdb_locknode,
	.unlocknode = dns__qpdb_unlocknode,
	.addglue = addglue,
	.deletedata = dns__qpdb_deletedata,
};

void
dns__qpzone_resigninsert(dns_qpdb_t *qpdb, int idx,
			 dns_slabheader_t *newheader) {
	INSIST(!IS_CACHE(qpdb));
	INSIST(newheader->heap_index == 0);
	INSIST(!ISC_LINK_LINKED(newheader, link));

	isc_heap_insert(qpdb->heaps[idx], newheader);
	newheader->heap = qpdb->heaps[idx];
}

void
dns__qpzone_resigndelete(dns_qpdb_t *qpdb, dns_qpdb_version_t *version,
			 dns_slabheader_t *header DNS__DB_FLARG) {
	/*
	 * Remove the old header from the heap
	 */
	if (header != NULL && header->heap_index != 0) {
		isc_heap_delete(qpdb->heaps[QPDB_HEADERNODE(header)->locknum],
				header->heap_index);
		header->heap_index = 0;
		if (version != NULL) {
			dns__qpdb_newref(
				qpdb, QPDB_HEADERNODE(header),
				isc_rwlocktype_write DNS__DB_FLARG_PASS);
			ISC_LIST_APPEND(version->resigned_list, header, link);
		}
	}
}

isc_result_t
dns__qpzone_wildcardmagic(dns_qpdb_t *qpdb, const dns_name_t *name, bool lock) {
	isc_result_t result;
	dns_name_t foundname;
	dns_offsets_t offsets;
	unsigned int n;
	dns_rbtnode_t *node = NULL;
	isc_rwlocktype_t nlocktype = isc_rwlocktype_none;

	dns_name_init(&foundname, offsets);
	n = dns_name_countlabels(name);
	INSIST(n >= 2);
	n--;
	dns_name_getlabelsequence(name, 1, n, &foundname);

	result = dns_qp_getname(qpdb->tree, &foundname, (void **)&node, NULL);
	if (result != ISC_R_SUCCESS) {
		INSIST(node == NULL);
		node = dns_qpdata_create(qpdb, &foundname);
		result = dns_qp_insert(qpdb->tree, node, 0);
		INSIST(result == ISC_R_SUCCESS);
	}

	INSIST(result == ISC_R_SUCCESS);
	node->nsec = DNS_DB_NSEC_NORMAL;
	node->find_callback = 1;
	if (lock) {
		NODE_WRLOCK(&qpdb->node_locks[node->locknum].lock, &nlocktype);
	}
	node->wild = 1;
	if (lock) {
		NODE_UNLOCK(&qpdb->node_locks[node->locknum].lock, &nlocktype);
	}
	return (ISC_R_SUCCESS);
}

isc_result_t
dns__qpzone_addwildcards(dns_qpdb_t *qpdb, const dns_name_t *name, bool lock) {
	isc_result_t result;
	dns_name_t foundname;
	dns_offsets_t offsets;
	unsigned int n, l, i;

	dns_name_init(&foundname, offsets);
	n = dns_name_countlabels(name);
	l = dns_name_countlabels(&qpdb->common.origin);
	i = l + 1;
	while (i < n) {
		dns_rbtnode_t *node = NULL;
		dns_name_getlabelsequence(name, n - i, i, &foundname);
		if (dns_name_iswildcard(&foundname)) {
			result = dns__qpzone_wildcardmagic(qpdb, &foundname,
							   lock);
			if (result != ISC_R_SUCCESS) {
				return (result);
			}

			result = dns_qp_getname(qpdb->tree, name,
						(void **)&node, NULL);
			if (result != ISC_R_SUCCESS) {
				INSIST(node == NULL);
				node = dns_qpdata_create(qpdb, name);
				result = dns_qp_insert(qpdb->tree, node, 0);
				INSIST(result == ISC_R_SUCCESS);
			}
			if (result == ISC_R_SUCCESS) {
				node->nsec = DNS_DB_NSEC_NORMAL;
			}
		}
		i++;
	}
	return (ISC_R_SUCCESS);
}
