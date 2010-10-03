#!/bin/sh

TESTSRC="$1"
BUILD="$2"

ARGS="${RUNTEST_ARGS}"

exec "${TESTSRC}/runtests.py" --angel "${BUILD}/src/angel/lighttpd2" --worker "${BUILD}/src/main/lighttpd2-worker" --plugindir "${BUILD}/src/modules/.libs" $ARGS
