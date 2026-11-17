#!/usr/bin/env bash
# Packages one architecture's release build (after `make qnx ARCH=...`).
#
#   scripts/release/package.sh aarch64le 0.1.0 <commit sha> <out dir>
#
# Needs the QNX SDP environment (the strip tool). Writes
# <out dir>/rtcollector-<version>-qnx8.0-<arch>.tar.gz.
set -euo pipefail
ARCH=${1:?arch}
VERSION=${2:?version}
SHA=${3:?commit}
OUT=${4:?out dir}
B=build-qnx-$ARCH
STRIP=nto${ARCH/aarch64le/aarch64}-strip
NAME=rtcollector-$VERSION-qnx8.0-$ARCH
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT
P=$D/$NAME
mkdir -p "$P/licenses"

# Stripped: no symbols, no debug info (no build paths) in what is shipped.
for f in rtcollector rtc_writer; do
	"$STRIP" -o "$P/$f" "$B/$f"
	chmod 755 "$P/$f"
done
install -m 755 scripts/release/rtcollector-start.sh "$P/rtcollector-start.sh"
install -m 644 examples/rtcollector.yml "$P/rtcollector.yml"
install -m 644 include/rtc_publish.h "$P/rtc_publish.h"
install -m 644 LICENSE "$P/LICENSE"
install -m 644 NOTICE "$P/NOTICE"
install -m 644 third_party/libyaml/License "$P/licenses/libyaml.txt"
install -m 644 third_party/licenses/zstd-1.5.0-LICENSE "$P/licenses/zstd.txt"
cat >"$P/README.txt" <<EOF
rtcollector $VERSION for QNX 8.0 ($ARCH), built from commit $SHA
https://github.com/Basekick-Labs/rtcollector-qnx

Getting started: https://github.com/Basekick-Labs/rtcollector-qnx/blob/v$VERSION/docs/getting-started.md

On the target the binary needs libcurl.so.12, libsocket.so.4, libm.so.3 and
libc.so.6 (on the QNX 8.0 images). The trace input also needs
libtracelog.so.1 (on the images) and libtraceparser.so.1: copy it from your
QNX SDP 8.0 (\$QNX_TARGET/$ARCH/usr/lib/libtraceparser.so.1) to /usr/lib.

Free for non-commercial use under the terms of the QNX libraries it runs
with; see NOTICE.
EOF

# Nothing from the build host in the shipped files.
for f in "$P/rtcollector" "$P/rtc_writer"; do
	if strings "$f" | grep -qE '/home/|/tmp/tmp|/opt/qnx800|rtcbuild'; then
		echo "package: $f contains a build-host path"
		exit 1
	fi
done

mkdir -p "$OUT"
tar -C "$D" --owner=0 --group=0 --numeric-owner -czf "$OUT/$NAME.tar.gz" "$NAME"
echo "package: $OUT/$NAME.tar.gz"
