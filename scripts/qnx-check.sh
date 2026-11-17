#!/usr/bin/env bash
# The QNX binary must need only libraries the QNX 8.0 image has, and must not
# carry libtraceparser (QNX code: it is loaded at run time, see trace.c).
#
#   scripts/qnx-check.sh build-qnx-aarch64le/rtcollector ntoaarch64le-readelf
set -euo pipefail
BIN=${1:?binary}
READELF=${2:?readelf}
ALLOWED=" libcurl.so.12 libsocket.so.4 libm.so.3 libc.so.6 libgcc_s.so.1 "

fail=0
while read -r lib; do
	case "$ALLOWED" in
	*" $lib "*) ;;
	*)
		echo "unexpected NEEDED library: $lib"
		fail=1
		;;
	esac
done < <("$READELF" -d "$BIN" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')

# Defined (not UND) symbols whose name starts with traceparser.
if "$READELF" -s --wide "$BIN" | awk '$7 != "UND" && $8 ~ /^traceparser/' | grep -q .; then
	echo "libtraceparser is linked into $BIN"
	fail=1
fi

[ $fail = 0 ] && echo "qnx-check $BIN: ok"
exit $fail
