#!/bin/sh
#
# Copyright (C) 2015  Internet Systems Consortium, Inc. ("ISC")
#
# Permission to use, copy, modify, and/or distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
# REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
# AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
# INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
# LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
# OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
# PERFORMANCE OF THIS SOFTWARE.

SYSTEMTESTTOP=..
. $SYSTEMTESTTOP/conf.sh

. ../getopts.sh

USAGE="$0: [-xD]"
DEBUG=
while getopts "xD" c; do
    case $c in
	x) set -x; DEBUG=-x;;
        D) TEST_DNSRPS="-D";;
	N) NOCLEAN=set;;
	*) echo "$USAGE" 1>&2; exit 1;;
    esac
done
shift `expr $OPTIND - 1 || true`
if test "$#" -ne 0; then
    echo "$USAGE" 1>&2
    exit 1
fi

[ ${NOCLEAN:-unset} = unset ] && $SHELL clean.sh $DEBUG

$PERL testgen.pl
$SEDPORTS < ns1/named.conf.in > ns1/named.conf
echo "${port}" > ns1/named.port
$SEDPORTS < ns2/named.conf.header.in > ns2/named.conf.header
echo "${port}" > ns2/named.port
cp -f ns2/named.default.conf ns2/named.conf
