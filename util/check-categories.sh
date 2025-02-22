#!/bin/sh

# Copyright (C) Internet Systems Consortium, Inc. ("ISC")
#
# SPDX-License-Identifier: MPL-2.0
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0.  If a copy of the MPL was not distributed with this
# file, you can obtain one at https://mozilla.org/MPL/2.0/.
#
# See the COPYRIGHT file distributed with this work for additional
# information regarding copyright ownership.

list1=$(
  grep -h LOGCATEGORY lib/*/include/*/*.h bin/named/include/named/*.h \
    | grep -E "^[[:space:]]+[^[:space:]]+_LOGCATEGORY_[^[:space:]]+([[:space:]]+=[[:space:]]+[-0-9]+)?," \
    | grep -Ev "ISC_LOGCATEGORY_(MAX|INVALID)" \
    | sed -e 's/.*LOGCATEGORY_\([A-Z_]*\).*/\1/' -e 's/^RRL$/rate-limit/' \
      -e 's/DRA/dns-reporting-agent/' \
    | tr 'A-Z' 'a-z' \
    | tr _ - \
    | sed 's/^tat$/trust-anchor-telemetry/' \
    | sort -u
)
list2=$(
  sed -ne 's/^``\(.*\)``/\1/p' doc/arm/logging-categories.inc.rst \
    | sort -u
)
status=0
for i in $list1; do
  ok=no
  for j in $list2; do
    if test $i = $j; then
      ok=yes
    fi
  done
  if test $ok = no; then
    echo "$i missing from doc/arm/logging-categories.rst."
    status=1
  fi
done
for i in $list2; do
  ok=no
  for j in $list1; do
    if test $i = $j; then
      ok=yes
    fi
  done
  if test $ok = no; then
    echo "documented logging category '$i' not in code."
    status=1
  fi
done
exit $status
