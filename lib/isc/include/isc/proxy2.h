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

#include <isc/buffer.h>
#include <isc/mem.h>
#include <isc/sockaddr.h>

/* Definitions taken or derived from the specification */

#define ISC_PROXY2_HEADER_SIGNATURE \
	("\x0D\x0A\x0D\x0A\x00\x0D\x0A\x51\x55\x49\x54\x0A")

#define ISC_PROXY2_HEADER_SIGNATURE_SIZE (12)

#define ISC_PROXY2_HEADER_SIZE                                            \
	(ISC_PROXY2_HEADER_SIGNATURE_SIZE + 1 /* version and command */ + \
	 1 /* protocol and family */ + 2 /* data size */)

#define ISC_PROXY2_MAX_SIZE (ISC_PROXY2_HEADER_SIZE + UINT16_MAX)

#define ISC_PROXY2_MIN_AF_INET_SIZE                                     \
	(ISC_PROXY2_HEADER_SIZE + 4 /* src_addr */ + 4 /* dst_addr */ + \
	 2 /* src port */ + 2 /* dst_port */)

#define ISC_PROXY2_MIN_AF_INET6_SIZE                                      \
	(ISC_PROXY2_HEADER_SIZE + 16 /* src_addr */ + 16 /* dst_addr */ + \
	 2 /* src port */ + 2 /* dst_port */)

#define ISC_PROXY2_AF_UNIX_MAX_PATH_LEN (108)

#define ISC_PROXY2_MIN_AF_UNIX_SIZE                       \
	(ISC_PROXY2_HEADER_SIZE +                         \
	 ISC_PROXY2_AF_UNIX_MAX_PATH_LEN /* src_addr */ + \
	 ISC_PROXY2_AF_UNIX_MAX_PATH_LEN /* dst_addr */)

#define ISC_PROXY2_TLV_HEADER_SIZE \
	(1 /* type */ + 1 /* length_hi */ + 1 /* length_lo */)

#define ISC_PROXY2_TLS_SUBHEADER_MIN_SIZE \
	(1 /* client_flags */ + 4 /* verify */)

typedef enum isc_proxy2_command {
	ISC_PROXY2_CMD_ILLEGAL = -1,
	/*
	 * PROXYv2 header does not contain any addresses and is supposedly
	 * created on behalf of locally running software.
	 */
	ISC_PROXY2_CMD_LOCAL = 0,
	/*
	 * PROXYv2 header contains address-related information and is
	 * created on a behalf of the client.
	 */
	ISC_PROXY2_CMD_PROXY = 1
} isc_proxy2_command_t;

typedef enum isc_proxy2_addrfamily {
	ISC_PROXY2_AF_UNSPEC = 0,
	ISC_PROXY2_AF_INET = 1,
	ISC_PROXY2_AF_INET6 = 2,
	ISC_PROXY2_AF_UNIX = 3
} isc_proxy2_addrfamily_t;

typedef enum isc_proxy2_socktype {
	ISC_PROXY2_SOCK_ILLEGAL = -1,
	ISC_PROXY2_SOCK_UNSPEC = 0,
	ISC_PROXY2_SOCK_STREAM = 1,
	ISC_PROXY2_SOCK_DGRAM = 2
} isc_proxy2_socktype_t;

typedef enum isc_proxy2_tlv_type {
	/*
	 * Application-Layer Protocol Negotiation (ALPN). It is a byte
	 * sequence defining the upper layer protocol in use over the
	 * connection.
	 */
	ISC_PROXY2_TLV_TYPE_ALPN = 0x01,
	/*
	 * Contains the host name value passed by the client, as an
	 * UTF8-encoded string.
	 */
	ISC_PROXY2_TLV_TYPE_AUTHORITY = 0x02,
	/*
	 * The value is a 32-bit number storing the CRC32c checksum of the
	 * PROXY protocol header.
	 */
	ISC_PROXY2_TLV_TYPE_CRC32C = 0x03,
	/*
	 * The TLV of this type should be ignored when parsed. The value
	 * is zero or more bytes. Can be used for data padding or
	 * alignment.
	 */
	ISC_PROXY2_TLV_TYPE_NOOP = 0x04,
	/*
	 * The value is an opaque byte sequence of up to 128 bytes
	 * generated by the upstream proxy that uniquely identifies the
	 * connection.
	 */
	ISC_PROXY2_TLV_TYPE_UNIQUE_ID = 0x05,
	/*
	 * SSL type contains subfields of the given subtypes (see
	 * isc_proxy2_tlv_subtype_tls_t). The header contains:
	 *
	 * - uint8_t client is a bit-field made of
	 *   isc_proxy2_tls_client_flags_t;
	 * - uint32_t verify (0 for a successfully verified certificate);
	 */
	ISC_PROXY2_TLV_TYPE_TLS = 0x20,
	/*
	 * The type PP2_TYPE_NETNS defines the value as the US-ASCII
	 * string representation of the namespace's name.
	 */
	ISC_PROXY2_TLV_TYPE_NETNS = 0x30,
	/*
	 * The following range of 16 type values is reserved for
	 * application-specific data and will be never used by the PROXY
	 * Protocol.
	 */
	ISC_PROXY2_TLV_TYPE_MIN_CUSTOM = 0xE0,
	ISC_PROXY2_TLV_TYPE_MAX_CUSTOM = 0xEF,
	/*
	 * This range of 8 values is reserved for temporary experimental
	 * use by application developers and protocol designers.
	 */
	ISC_PROXY2_TLV_TYPE_MIN_EXPERIMENT = 0xF0,
	ISC_PROXY2_TLV_TYPE_MAX_EXPERIMENT = 0xF7,
	/*
	 * The following range of 8 values is reserved for future use,
	 * potentially to extend the protocol with multibyte type values.
	 */
	ISC_PROXY2_TLV_TYPE_MIN_FUTURE = 0xF8,
	ISC_PROXY2_TLV_TYPE_MAX_FUTURE = 0xFF
} isc_proxy2_tlv_type_t;

typedef enum isc_proxy2_tls_client_flags {
	/* The flag indicates that the client connected over SSL/TLS. */
	ISC_PROXY2_CLIENT_TLS = 0x01,
	/* The client provided a certificate over the current connection. */
	ISC_PROXY2_CLIENT_CERT_CONN = 0x02,
	/*
	 * The client provided a certificate at least once over the TLS
	 * session this connection belongs to.
	 */
	ISC_PROXY2_CLIENT_CERT_SESS = 0x04
} isc_proxy2_tls_client_flags_t;

typedef enum isc_proxy2_tlv_subtype_tls {
	/*
	 * The US-ASCII string representation of the TLS version the TLV
	 * format.
	 */
	ISC_PROXY2_TLV_SUBTYPE_TLS_VERSION = 0x21,
	/*
	 * The Common Name field of the client certificate's Distinguished
	 * Name in the TLV format.
	 */
	ISC_PROXY2_TLV_SUBTYPE_TLS_CN = 0x22,
	/*
	 * The US-ASCII string name of the used cipher, for
	 * example "ECDHE-RSA-AES128-GCM-SHA256".
	 */
	ISC_PROXY2_TLV_SUBTYPE_TLS_CIPHER = 0x23,
	/*
	 * The US-ASCII string name of the algorithm used to sign the
	 * certificate presented by the frontend when the incoming
	 * connection was made over an SSL/TLS transport layer, for
	 * example "SHA256".
	 */
	ISC_PROXY2_TLV_SUBTYPE_TLS_SIG_ALG = 0x24,
	/*
	 * The US-ASCII string name of the algorithm used to generate the
	 * key of the certificate presented by the frontend when the
	 * incoming connection was made over an SSL/TLS transport layer,
	 * for example "RSA2048".
	 */
	ISC_PROXY2_TLV_SUBTYPE_TLS_KEY_ALG = 0x25
} isc_proxy2_tlv_subtype_tls_t;

/*
 * Definitions related to processing and verification of existing PROXYv2
 * headers
 */

typedef struct isc_proxy2_handler isc_proxy2_handler_t;
/*!<
 * 'isc_proxy2_handler_t' is an entity designed for processing of the
 * PROXYv2 data received from a network. Despite its purpose, it is
 * designed as a state machine which, in fact, has no direct connection
 * to the networking code. Interaction with the networking code is done
 * via the provided API only.
 *
 * The entity is designed as a state machine which accepts data on
 * input and calls a user-provided data processing callback to notify
 * about data processing status and, in the case of successful
 * processing, provide the upper level code with the data obtained
 * from the PROXYv2 header and associated payload.
 *
 * The reason for the state machine-based approach is
 * many-fold. Firstly, the protocol itself is well suited for
 * processing by a state machine with well-defined steps. Secondly,
 * such a design allows iterative data processing combined with
 * verification, which is more secure than trying to read seemingly
 * enough data, process it, and then retrospectively verify
 * it. Thirdly, such an approach aligns well with how stream-based
 * transports work - PROXYv2 headers might arrive torn into multiple
 * parts which need to be assembled (theoretically, it is fine to send
 * data over TCP bite-by-bite), and we should handle such
 * cases. Additionally to that, we should stop reading data as soon as
 * we detect that it is ill-formed. This design allows that and also
 * can be used easily with datagram-based networking code.
 *
 * Another important characteristic of the state machine-based code is
 * that it can be unit-tested separately from the rest of the code
 * base. Of course, we use that to our advantage.
 *
 * The implementations closely follows the PROXYv2 protocol
 * specification from 2020/03/05
 * (https://www.haproxy.org/download/2.9/doc/proxy-protocol.txt),
 * however, enough functionality is provided to handle future
 * extensions, too. To be fair, our needs are quite modest and we are
 * not interested in all the information PROXYv2 protocol could carry
 * - we are mostly interested in the basics. However, our protocol
 * handling code is fairly complete at the time of writing leaving a
 * good foundation for further extensions, as the PROXYv2 protocol is
 * itself extensible. The only missing thing is header checksum
 * verification - but that functionality is optional. That being said, it is
 * easy to add that, should we ever need to.
 */

typedef void (*isc_proxy2_handler_cb_t)(const isc_result_t	   result,
					const isc_proxy2_command_t cmd,
					const int		   socktype,
					const isc_sockaddr_t *restrict src_addr,
					const isc_sockaddr_t *restrict dst_addr,
					const isc_region_t *restrict tlv_data,
					const isc_region_t *restrict extra,
					void *cbarg);
/*!<
 * PROXYv2 data processing callback.
 *
 * Arguments:
 *\li	'result' - error status code;
 *\li	'cmd' - PROXYv2 command;
 *\li	'socktype' - PROXYv2 addresses socket type
 *(SOCK_STREAM, SOCK_DGRAM, or '0' for "unspecified" (SOCK_UNSPEC)).
 *\li	'src_addr' - original source address extracted from the PROXYv2 header;
 *\li	'dst_addr' - original destination address extracted from the PROXYv2
 *header;
 *\li	'tlv_data' - TLV-data extracted from the header;
 *\li	'extra_data' - extra unprocessed data past the PROXYv2 header. It is
 *not a part of the header, but it is fine to receive this when reading data
 *over TCP-based transports. In general, needs to be passed to the upper
 *level as is;
 *\li	'cbarg' - opaque pointer to user supplied data.
 *
 * The user-provided data processing callback function can get the
 * following error status codes:
 * \li	'ISC_R_SUCCESS' - the header has been processed, and data has been
 * extracted from the received header and its payload;
 * \li	'ISC_R_NOMORE' - the data passed was processed, and we need more to
 * continue processing (=resume reading from the network as we have no more
 * data to process);
 * \li	'ISC_R_UNEXPECTED' - an unexpected value has been detected;
 * \li	'ISC_R_RANGE' - an expected value is not within an expected range.
 *
 * When processing error status within the callback, in general, we
 * are interested in dispatching on the first two values, as anything
 * else can be treated as hard-stop errors: their purpose is to give a
 * little insight into what has happened without going into gory
 * details (as we are not interested in them most of the time anyway).
 *
 * Any of the argument pointers can be 'NULL', identifying that the
 * corresponding data is not present in the PROXYv2 header. Also,
 * 'socktype' can be '-1' in a case of processing error.
 *
 */

struct isc_proxy2_handler {
	isc_buffer_t hdrbuf; /*!< Internal buffer for assembling PROXYv2 header
			      */
	uint8_t buf[256];    /*!< Internal buffer static storage */

	int	 state;	      /*!< Current state machine state */
	uint16_t expect_data; /*!< How much data do we need to switch to the
				 next state */
	uint16_t max_size; /*!< Max PROXYv2 header size including its payload */

	isc_proxy2_handler_cb_t cb;    /*!< Data processing callback. */
	void		       *cbarg; /*!< Callback argument. */
	bool calling_cb; /*<! Callback calling marker. Used to detect recursive
		    object uses (changing the data state from within
		    the callback). */
	isc_result_t result; /*<! The last passed to the callback processing
				status value. */
	isc_mem_t *mctx;

	uint16_t header_size;	/*!< Total PROXYv2 header size (including the
				   payload. */
	uint16_t tlv_data_size; /*!< The size of TLVs payload size */

	isc_proxy2_command_t	cmd; /*!< The decoded PROXYv2 command */
	isc_proxy2_addrfamily_t proxy_addr_family; /*!< The decoded PROXYv2
						      address family */
	isc_proxy2_socktype_t proxy_socktype; /*!< The decoded PROXYv2 socket
						 type */

	isc_region_t tlv_data;	 /*!< TLV data region within the handled PROXYv2
				    header */
	isc_region_t extra_data; /*!< Data past the PROXYv2 header (not
				    belonging to it) */
};

void
isc_proxy2_handler_init(isc_proxy2_handler_t *restrict handler, isc_mem_t *mctx,
			const uint16_t max_size, isc_proxy2_handler_cb_t cb,
			void *cbarg);
/*!<
 * \brief Initialise the given 'isc_proxy2_handler_t' object, attach
 * to the memory context.
 *
 * Arguments:
 *\li	'mctx' -  memory context;
 *\li	'max_size' - the upper limit for the PROXYv2 header and its payload (0 -
 *unlimited);
 *\li	'cb' - data processing callback;
 *\li	'cbarg' - data processing callback argument.
 *
 * Requires:
 *\li	'handler' is not NULL;
 *\li	'mctx' is not NULL;
 *\li	'max_size' is >= `ISC_PROXY2_HEADER_SIZE` or is 0;
 *\li	'cb' is not NULL.
 */

void
isc_proxy2_handler_uninit(isc_proxy2_handler_t *restrict handler);
/*!<
 * \brief Un-initialise the given 'isc_proxy2_handler_t' object, detach
 * from the attached memory context. Invalidate any internal unprocessed data.
 *
 * Requires:
 *\li	'handler' is not NULL.
 */

void
isc_proxy2_handler_clear(isc_proxy2_handler_t *restrict handler);
/*!<
 * \brief Clear the given 'isc_proxy2_handler_t' object from
 * any unprocessed data, clear the last data processing status (set it to
 * 'ISC_R_UNSET'). Effectively, the function returns the object to its initial
 * state.
 *
 * Requires:
 *\li	'handler' is not NULL.
 */

isc_proxy2_handler_t *
isc_proxy2_handler_new(isc_mem_t *mctx, const uint16_t max_size,
		       isc_proxy2_handler_cb_t cb, void *cbarg);
/*!<
 * \brief Allocate and initialise a new 'isc_proxy2_handler_t'
 * object, attach to the memory context.
 *
 * Arguments:
 *\li	'mctx' -  memory context;
 *\li	'max_size' - the upper limit for the PROXYv2 header and its payload (0 -
 *unlimited);
 *\li	'cb' - data processing callback;
 *\li	'cbarg' - data
 *processing callback argument.
 *
 * Requires:
 *\li	'mctx' is not NULL;
 *\li	'max_size' is >= `ISC_PROXY2_HEADER_SIZE` or is 0;
 *\li	'cb' is not NULL.
 */

void
isc_proxy2_handler_free(isc_proxy2_handler_t **restrict handler);
/*!<
 * \brief Un-initialise the given 'isc_proxy2_handler_t' object, detach
 * from the attached memory context, free the memory consumed by the object.
 *
 * Requires:
 *\li	'handler' is not NULL;
 *\li	'handler' is not pointing to NULL.
 */

void
isc_proxy2_handler_setcb(isc_proxy2_handler_t *restrict handler,
			 isc_proxy2_handler_cb_t cb, void *cbarg);
/*!<
 * \brief Change the data processing callback and its argument within the
 * given 'isc_proxy2_handler_t' object.
 *
 * Arguments:
 *\li	'handler' - PROXYv2 handler object;
 *\li	'cb' - new data processing callback;
 *\li	'cbarg' - new data processing callback argument.
 *
 * Requires:
 *\li	'handler' is not NULL;
 *\li	'cb' is not NULL.
 */

isc_result_t
isc_proxy2_handler_push_data(isc_proxy2_handler_t *restrict handler,
			     const void *restrict buf,
			     const unsigned int buf_size);
/*!<
 * \brief Push new data to the given 'isc_proxy2_handler_t'
 * object. Call the callback passing a status and a result of data
 * processing to it.
 *
 * To avoid erroneously recursive usage of the object, it is forbidden to call
 * this function from within the callback. Doing so will abort the program.
 *
 * Requires:
 *\li	'handler' is not NULL;
 *\li	'buf' is not NULL;
 *\li	'buf_size' is not 0.
 */

isc_result_t
isc_proxy2_handler_push(isc_proxy2_handler_t *restrict handler,
			const isc_region_t *restrict region);
/*!<
 * \brief The same as 'isc_proxy2_handler_push_data()' but pushes that
 * data for processing via an 'isc_region_t' object.
 *
 * Requires:
 *\li	'handler' is not NULL;
 *\li	'region' is not NULL.
 */

isc_result_t
isc_proxy2_handler_result(const isc_proxy2_handler_t *restrict handler);
/*!<
 * \brief Return the last data processing status passed to the
 * callback.
 *
 * Requires:
 *\li	'handler' is not NULL.
 *
 * Return values:
 * \li	'ISC_R_SUCCESS' - the header has been processed, and data has been
 * extracted from the received header and its payload;
 * \li	'ISC_R_NOMORE' - the data passed was processed, and we need more to
 * continue processing (=resume reading from the network as we have no more
 * data to process);
 * \li	'ISC_R_UNEXPECTED' - an unexpected value has been detected;
 * \li	'ISC_R_RANGE' - an expected value is not within an expected range.
 */

size_t
isc_proxy2_handler_header(const isc_proxy2_handler_t *restrict handler,
			  isc_region_t *restrict region);
/*!<
 * \brief Get the complete processed PROXYv2 header as is
 * (e.g. for forwarding).
 *
 * Requires:
 *\li	'handler' is not NULL;
 *\li	'region' is NULL or points to a zeroed 'isc_region_t' object.
 *
 * Return the size of the header or 0 on error (if it has not been
 * processed yet).
 */

size_t
isc_proxy2_handler_tlvs(const isc_proxy2_handler_t *restrict handler,
			isc_region_t *restrict region);
/*!<
 * \brief Get the TLV-data within the processed PROXYv2 header.
 *
 * Requires:
 *\li	'handler' is not NULL;
 *\li	'region' is NULL or points to a zeroed 'isc_region_t' object.
 *
 * Return the size of the header or 0 on error (if it has not been
 * processed yet).
 */

size_t
isc_proxy2_handler_extra(const isc_proxy2_handler_t *restrict handler,
			 isc_region_t *restrict region);
/*!<
 * \brief Get the data past the processed PROXYv2 header. The data
 * is not the part of the PROXYv2 header itself. That can happen (and
 * does happen) when data is being sent over TCP.
 *
 * Requires:
 *\li	'handler' is not NULL;
 *\li	'region' is NULL or points to a zeroed 'isc_region_t' object.
 *
 * Return the size of the header or 0 on error (if it has not been
 * processed yet).
 */

isc_result_t
isc_proxy2_handler_addresses(const isc_proxy2_handler_t *restrict handler,
			     int *restrict psocktype,
			     isc_sockaddr_t *restrict psrc_addr,
			     isc_sockaddr_t *restrict pdst_addr);
/*!<
 * \brief Get the addresses directly from the processed PROXYv2
 * header. If you are not interested in particular data, you can pass
 * NULL as the argument to ignore it.
 *
 * Requires:
 *\li	'handler' is not NULL.
 */

isc_result_t
isc_proxy2_header_handle_directly(const isc_region_t *restrict header_data,
				  const isc_proxy2_handler_cb_t cb,
				  void			       *cbarg);
/*!<
 * \brief Process PROXYv2 header in one go directly without memory
 * allocation and copying. Specifically designed to work when a
 * complete header and associated follow-up data is expected (for
 * example, when datagram transports are used, like UDP).
 *
 * Requires:
 *\li	'header_data' is not NULL;
 *\li	'cb' is not NULL.
 *
 * Return values are the same that get passed to the processing
 * callback. Given that processing should complete in one go, getting
 * anything except `ISC_R_SUCCESS` indicates failure.
 */

typedef bool (*isc_proxy2_tlv_cb_t)(const isc_proxy2_tlv_type_t tlv_type,
				    const isc_region_t *restrict data,
				    void *cbarg);
/*!<
 * \brief Callback used for iterating over TLV data extracted from
 * PROXYv2 headers.
 *
 * Arguments:
 *\li	'tlv_type' - type value (see the 'isc_proxy2_tlv_type_t' enumeration);
 *\li	'data' - pointer to 'isc_region_t' object referring to the data;
 *\li	'cbarg' - opaque pointer to user supplied data.
 *
 * Return values:
 *\li	'true' - continue processing the next TLV entry (if any);
 *\li	'false' - stop processing TLV-entries.
 */

isc_result_t
isc_proxy2_tlv_iterate(const isc_region_t *restrict tlv_data,
		       const isc_proxy2_tlv_cb_t cb, void *cbarg);
/*!<
 * \brief Iterate over the TLV data extracted from PROXYv2 headers.
 *
 * Arguments:
 *\li	'tlv_data' - TLV data extracted from a PROXYv2 header;
 *\li	'cb' - user provided iteration callback;
 *\li	'cbarg' - user provided iteration callback argument.
 *
 * Requires:
 *\li	'tlv_data' is not NULL;
 *\li	'cb' is not NULL.
 *
 * Return values:
 *\li	'ISC_R_SUCCESS' - iteration over the data was successful;
 *\li	'ISC_R_RANGE' - malformed TLV data was detected.
 */

typedef bool (*isc_proxy2_tls_subtlv_cb_t)(
	const uint8_t client_flags, const bool client_cert_verified,
	const isc_proxy2_tlv_subtype_tls_t tls_subtlv_type,
	const isc_region_t *restrict data, void *cbarg);
/*!<
 * \brief Callback used for iterating over TLS sub-TLV data extracted from
 * PROXYv2 headers.
 *
 * Arguments:
 *\li	'client_flags' - TLS client flags extracted from TLS TLV data
 * (see 'isc_proxy2_tls_client_flags_t' enumeration);
 *\li	'client_cert_verified' - flag which indicates if the supplied
 *TLS client certificate was verified- (if provided by the client);
 *\li	'tls_subtlv_type' - TLS sub-TLV type (see the
 *'isc_proxy2_tlv_subtype_tls_t' enumeration);
 *\li	'cbarg' - opaque pointer to user supplied data.
 *
 * Return values:
 *\li	'true' - continue processing the next TLV entry (if any);
 *\li	'false' - stop processing TLV-entries.
 */

isc_result_t
isc_proxy2_subtlv_tls_header_data(const isc_region_t *restrict tls_tlv_data,
				  uint8_t *restrict pclient_flags,
				  bool *restrict pclient_cert_verified);
/*!<
 * \brief Get data from a TLS ('ISC_PROXY2_TLV_TYPE_TLS') TLV value.
 *
 * Arguments:
 *\li	'pclient_flags' - a pointer to the variable to receive the TLS client
 *flags (see 'isc_proxy2_tls_client_flags_t' enumeration for more details);
 *\li	'pclient_cert_verified' - a pointer the value to receive TLS client
 *certificate verification status ('true' - verified).
 *
 * Requires:
 *\li	'tls_tlv_data' is not NULL;
 *\li	'pclient_flags' is either NULL or a pointer pointing to a
 *zeroed variable;
 *\li	'pclient_cert_verified' is either NULL or a pointer pointing to a
 *zeroed variable.
 *
 * Return values:
 *\li	'ISC_R_SUCCESS' - iteration over the data was successful;
 *\li	'ISC_R_RANGE' - malformed TLV data was detected.
 */

isc_result_t
isc_proxy2_subtlv_tls_iterate(const isc_region_t *restrict tls_tlv_data,
			      const isc_proxy2_tls_subtlv_cb_t cb, void *cbarg);
/*!<
 * \brief Iterate over the sub-TLV data extracted from TLS
 * ('ISC_PROXY2_TLV_TYPE_TLS') TLV value of a PROXYv2 header.
 *
 * Arguments:
 *\li	'tls_tlv_data' - TLS-realted sub-TLV data extracted from
 *a PROXYv2 header;
 *\li	'cb' - user provided iteration callback;
 *\li	'cbarg' - user provided iteration callback argument.
 *
 * Requires:
 *\li	'tls_tlv_data' is not NULL;
 *\li	'cb' is not NULL.
 *
 * Return values:
 *\li	'ISC_R_SUCCESS' - iteration over the data was successful;
 *\li	'ISC_R_RANGE' - malformed TLV data was detected.
 */

isc_result_t
isc_proxy2_tlv_data_verify(const isc_region_t *restrict tlv_data);
/*!<
 * \brief Verify TLV-data structure extracted from a PROXYv2 header.
 * The function loops over the data verifying that TLVs are structured
 * in a correct way.
 *
 * NOTE: If you are using an 'isc_proxy2_handler_t' object then there
 * is no need for you to call this function as it is called during
 * the normal operation. It is exposed mostly for unit testing
 * purposes or for verifying outgoing data, should it be required.
 *
 * Arguments:
 *\li	'tlv_data' - TLV data extracted from a PROXYv2 header.
 *
 * Requires:
 *\li	'tlv_data' is not NULL.
 *
 * Return values:
 *\li	'ISC_R_SUCCESS' - iteration over the data was successful;
 *\li	'ISC_R_RANGE' - malformed TLV data was detected.
 */

/*
 * Definitions related to generation of PROXYv2 headers
 */

isc_result_t
isc_proxy2_make_header(isc_buffer_t *restrict outbuf,
		       const isc_proxy2_command_t cmd, const int socktype,
		       const isc_sockaddr_t *restrict src_addr,
		       const isc_sockaddr_t *restrict dst_addr,
		       const isc_region_t *restrict tlv_data);
/*!<
 * \brief Create a PROXYv2 header.
 *
 * Arguments:
 *\li	'outbuf' - the output buffer;
 *\li	'cmd' - PROXYv2 command;
 *\li	'socktype' - PROXYv2 socket type (possible values are 'SOCK_STREAM',
 *'SOCK_DGRAM', or '0' for "unspecified");
 *\li	'src_addr' - source address, if any;
 *\li	'dst_addr' - destination address, if any;
 *\li	'tlv_data' - TLV data, if any.
 *
 * Requires:
 *\li	'outbuf' is not NULL;
 *\li	'cmd' is 'ISC_PROXY2_CMD_PROXY' or 'socktype' is equal to '0';
 *\li	either both of 'src_addr' and 'dst_addr' are NULL or both are not;
 *\li	both of 'src_addr' and 'dst_addr' are of the same type when specified.
 *
 * Notes:
 *
 * When 'cmd' equals 'ISC_PROXY2_CMD_LOCAL', then 'socktype' must equal '0'
 * (unspecified) and both 'src_addr' and 'dst_addr' must be 'NULL'.
 * When 'cmd' equals 'ISC_PROXY2_CMD_PROXY', then having 'socktype' being equal
 * to '0' will instruct the function to create PROXYv2 header marked as bearing
 * address of "unspecified" ('0') opaque type. In this case both 'src_addr' and
 * 'dst_addr' must be 'NULL'. In other cases the address type is determined
 * from the 'src_addr' and 'dst_addr' arguments (and might 'AF_INET',
 * 'AF_INET6', and 'AF_UNIX' per the protocol spec).
 * The socket type, when applicable, is determined from the 'socktype' argument
 * and must be any of 'SOCK_STREAM', 'SOCK_DGRAM', when applicable, or '0'
 * (unspecified).
 *
 * Return values:
 *\li	'ISC_R_SUCCESS' - iteration over the data was successful;
 *\li	'ISC_R_UNEXPECTED' - an unexpected value has been detected in the input
 *data (function arguments);
 *\li	'ISC_R_NOSPACE' - not enough data in the output buffer;
 *\li	'ISC_R_RANGE' - too much data to fit PROXYv2 header.
 */

isc_result_t
isc_proxy2_header_append(isc_buffer_t *restrict outbuf,
			 const isc_region_t *restrict data);
/*!<
 * \brief Append arbitrary data to PROXYv2 header and update the
 * length field within the header accordingly. It is used as foundation
 * for TLV appending functionality. Also, it can be used to add address
 * information in the case when "unspecified" opaque format is used.
 *
 * Arguments:
 *\li	'outbuf' - the output buffer containing a valid PROXYv2 header;
 *\li	'data' - use provided arbitrary data.
 *
 * Requires:
 *\li	'outbuf' is not NULL;
 *\li	used region within 'outbuf' is more or equal
 *to 'ISC_PROXY2_HEADER_SIZE';
 *\li	'data' is not NULL.
 *
 * Return values:
 *\li	'ISC_R_SUCCESS' - iteration over the data was successful;
 *\li	'ISC_R_NOSPACE' - not enough data in the output buffer;
 *\li	'ISC_R_RANGE' - too much data for PROXYv2 header.
 */

isc_result_t
isc_proxy2_header_append_tlv(isc_buffer_t *restrict outbuf,
			     const isc_proxy2_tlv_type_t tlv_type,
			     const isc_region_t *restrict data);
/*!<
 * \brief Append TLV data to PROXYv2 header and update the
 * length field within the header accordingly.
 *
 * Requires:
 *\li	'outbuf' is not NULL;
 *\li	used region within 'outbuf' is more or equal
 *to 'ISC_PROXY2_HEADER_SIZE';
 *\li	'data' is not NULL.
 *
 * Return values:
 *\li	'ISC_R_SUCCESS' - iteration over the data was successful;
 *\li	'ISC_R_NOSPACE' - not enough data in the output buffer
 *\li	'ISC_R_RANGE' - too much data for PROXYv2 header.
 */

isc_result_t
isc_proxy2_header_append_tlv_string(isc_buffer_t *restrict outbuf,
				    const isc_proxy2_tlv_type_t tlv_type,
				    const char *restrict str);
/*!<
 * \brief Append the string as TLV data to PROXYv2 header and update the
 * length field within the header accordingly.
 *
 * Requires:
 *\li	'outbuf' is not NULL;
 *\li	used region within 'outbuf' is more or equal
 *to 'ISC_PROXY2_HEADER_SIZE';
 *\li	'data' is not NULL.
 *
 * Return values:
 *\li	'ISC_R_SUCCESS' - iteration over the data was successful;
 *\li	'ISC_R_NOSPACE' - not enough data in the output buffer.
 *\li	'ISC_R_RANGE' - too much data for PROXYv2 header.
 */

isc_result_t
isc_proxy2_make_tls_subheader(isc_buffer_t *restrict outbuf,
			      const uint8_t client_flags,
			      const bool    client_cert_verified,
			      const isc_region_t *restrict tls_subtlvs_data);
/*!<
 * \brief Create TLS (ISC_PROXY2_TLV_TYPE_TLS) TLV subheader which
 * can later be added to the PROXYv2 header TLV data.
 *
 * Arguments:
 *\li	'client_flags' - TLS client flags (see
 'isc_proxy2_tls_client_flags_t' enumeration for more details);
 *\li	'client_cert_verified' - TLS client certificate verification
 *status ('true' - verified).
 *\li	'tls_subtlvs_data' - TLS subtlvs data, if any (see
 'isc_proxy2_tlv_subtype_tls_t' for more details).
 *
 * Requires:
 *\li	'outbuf' is not NULL;
 *
 * Return values:
 *\li	'ISC_R_SUCCESS' - iteration over the data was successful;
 *\li	'ISC_R_NOSPACE' - not enough data in the output buffer;
  *\li	'ISC_R_RANGE' - too much data for a TLV value.
 */

isc_result_t
isc_proxy2_append_tlv(isc_buffer_t *restrict outbuf, const uint8_t type,
		      const isc_region_t *restrict data);
/*!<
 * \brief Append TLV data to the end of the buffer. Compared to
 * 'isc_proxy2_header_append_tlv()' it does not try to look for a
 * correct PROXYv2 header at the beginning of the buffer and update
 * its length field. The main purpose of this function is to work with
 * sub-TLVs.
 *
 * Requires:
 *\li	'outbuf' is not NULL;
 *\li	'data' is not NULL.
 *
 * Return values:
 *\li	'ISC_R_SUCCESS' - iteration over the data was successful;
 *\li	'ISC_R_NOSPACE' - not enough data in the output buffer
 *\li	'ISC_R_RANGE' - too much data for PROXYv2 header.
 */

isc_result_t
isc_proxy2_append_tlv_string(isc_buffer_t *restrict outbuf, const uint8_t type,
			     const char *restrict str);
/*!<
 * \brief Append the string as TLV data to the end of the
 * buffer. Compared to 'isc_proxy2_header_append_tlv_string()' it does not
 * try to look for a correct PROXYv2 header at the beginning of the
 * buffer and update its length field. The main purpose of this
 * function is to work with sub-TLVs.
 *
 * Requires:
 *\li	'outbuf' is not NULL;
 *\li	'data' is not NULL.
 *
 * Return values:
 *\li	'ISC_R_SUCCESS' - iteration over the data was successful;
 *\li	'ISC_R_NOSPACE' - not enough data in the output buffer
 *\li	'ISC_R_RANGE' - too much data for PROXYv2 header.
 */
