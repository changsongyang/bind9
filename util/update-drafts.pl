#!/usr/local/bin/perl -w
#
# Copyright (C) 2004, 2007  Internet Systems Consortium, Inc. ("ISC")
# Copyright (C) 2000, 2001  Internet Software Consortium.
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

# $Id: update-drafts.pl,v 1.7 2007/06/19 23:47:24 tbox Exp $

#
# Replace internet drafts with updated versions, if any.
#
# Usage:
#
#   cd doc/draft
#   perl ../../util/update-drafts.pl *.txt
#   (ignore "404 Not Found" errors from FTP)
#   cvs commit -m"updated drafts"
#

foreach (@ARGV) {
    $ofile = $_;
    if (/^(.*-)([0-9][0-9])(\.txt)/) {
	    $nfile = "$1" . sprintf("%02d", $2 + 1) . "$3";
	    print $nfile, "\n";
	    system "ftp http://www.ietf.org/internet-drafts/$nfile";
	    if ($? == 0) {
		unlink($ofile);
		system "cvs remove $ofile";
		system "cvs add $nfile";
	    }
    }
}
