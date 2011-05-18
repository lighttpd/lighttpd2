#!/bin/sh
# Run this to generate all the initial makefiles, etc.

if which glibtoolize >/dev/null 2>&1; then
  LIBTOOLIZE=${LIBTOOLIZE:-glibtoolize}
else
  LIBTOOLIZE=${LIBTOOLIZE:-libtoolize}
fi
LIBTOOLIZE_FLAGS="--copy --force"
ACLOCAL=${ACLOCAL:-aclocal}
AUTOHEADER=${AUTOHEADER:-autoheader}
AUTOMAKE=${AUTOMAKE:-automake}
AUTOMAKE_FLAGS="--add-missing --copy"
AUTOCONF=${AUTOCONF:-autoconf}

ARGV0=$0

set -e


run() {
	echo "$ARGV0: running \`$@'"
	$@
}

run $LIBTOOLIZE $LIBTOOLIZE_FLAGS
run $ACLOCAL $ACLOCAL_FLAGS
run $AUTOHEADER
run $AUTOMAKE $AUTOMAKE_FLAGS
run $AUTOCONF
echo "Now type './configure ...' and 'make' to compile."
