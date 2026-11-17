#!/usr/bin/env bash
# The QNX binary must need only libraries the QNX 8.0 image has, and must not
# carry libtraceparser (QNX code: it is loaded at run time, see trace.c).
#
#   scripts/qnx-check.sh build-qnx-aarch64le/rtcollector ntoaarch64-readelf
set -euo pipefail
BIN=${1:?binary}
READELF=${2:?readelf}
ALLOWED=" libcurl.so.12 libsocket.so.4 libm.so.3 libc.so.6 libgcc_s.so.1 "

# Run the tool first: a missing or failing readelf must fail the check, not
# read as "nothing found".
command -v "$READELF" >/dev/null || { echo "qnx-check: $READELF not found"; exit 1; }
dyn=$("$READELF" -d "$BIN")
syms=$("$READELF" -s --wide "$BIN")
[ -n "$dyn" ] && [ -n "$syms" ] || { echo "qnx-check: $READELF printed nothing for $BIN"; exit 1; }

fail=0
needed=$(printf '%s\n' "$dyn" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')
[ -n "$needed" ] || { echo "qnx-check: no NEEDED entries in $BIN"; exit 1; }
for lib in $needed; do
	case "$ALLOWED" in
	*" $lib "*) ;;
	*)
		echo "unexpected NEEDED library: $lib"
		fail=1
		;;
	esac
done

# libtraceparser's own functions, defined in the binary (not UND).
if printf '%s\n' "$syms" |
	awk '$7 != "UND" && $8 ~ /^traceparser(_(init|destroy|cs|cs_range|get_info|one_event|debug))?$/' |
	grep -q .; then
	echo "libtraceparser is linked into $BIN"
	fail=1
fi

[ $fail = 0 ] && echo "qnx-check $BIN: ok ($(echo $needed))"
exit $fail
