#!/bin/sh

# only builds snapshots for now
# tarball name contains date and git short id

SRCTEST=src/main/lighttpd.c
PACKAGE=lighttpd
REV=${REV}
BASEDOWNLOADURL="http://download.lighttpd.net/lighttpd/snapshots-2.0.x"

if [ ! -f ${SRCTEST} ]; then
	echo "Current directory is not the source directory"
	exit 1
fi

dopack=1
if [ "$1" = "--nopack" ]; then
	dopack=0
	shift
fi

append="$1"

function force() {
	"$@" || {
		echo "Command failed: $*"
		exit 1
	}
}

if [ ${dopack} = "1" ]; then
	force ./autogen.sh

	if [ -d distbuild ]; then
		# make distcheck may leave readonly files
		chmod u+w -R distbuild
		rm -rf distbuild
	fi

	force mkdir distbuild
	force cd distbuild

	force ../configure --prefix=/usr

	# force make
	# force make check

	force make distcheck
else
	force cd distbuild
fi

version=`./config.status -V | head -n 1 | cut -d' ' -f3`
name="${PACKAGE}-${version}"
append="-snap-$(date '+%Y%m%d')${REV}-g$(git rev-list --abbrev-commit --abbrev=6 HEAD^..HEAD)"
if [ -n "${append}" ]; then
	cp "${name}.tar.gz" "${name}${append}.tar.gz"
	cp "${name}.tar.bz2" "${name}${append}.tar.bz2"
	name="${name}${append}"
fi

force sha256sum "${name}.tar."{gz,bz2} > "${name}.sha256sum"

echo wget "${BASEDOWNLOADURL}/${name}".'{tar.gz,tar.bz2,sha256sum}; sha256sum -c '${name}'.sha256sum'
