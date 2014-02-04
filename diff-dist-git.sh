#!/bin/sh

set -e

if [ ! -f "$1" ]; then
	(
		echo "Syntax: $0 [path-to-dist.tar.gz]"
		echo
		echo "Build such file with ./autogen.sh; ./configure; make dist-gzip"
		echo "This tool can then be used to check the differences between the git"
		echo "repository and the tar; it might show added files for autotools"
		echo "(compile, configure, Makefile.in, m4, ...) and should remove"
		echo ".gitignore files and some helper scripts (packdist.sh and this file)"
	) >&2
	exit 1
fi

tmpdir=$(mktemp --tmpdir -d diff-dist-tar-git-XXXXXXX)
trap 'rm -rf "${tmpdir}"' EXIT

git archive --format tar.gz -o "${tmpdir}/lighttpd.tar.gz" --prefix "lighttpd-2.0.0/" HEAD
tardiff --modified --autoskip "${tmpdir}/lighttpd.tar.gz" "$1"
