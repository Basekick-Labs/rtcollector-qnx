#!/usr/bin/env bash
# Release build on the build host. Installed root-owned as
# /usr/local/libexec/rtc-release-build and run as an unprivileged build user
# (no sudo, shell /bin/sh), as the forced command of the release workflow's
# SSH key, in a root-owned keys file the user cannot change
# (sshd: Match User ... AuthorizedKeysFile /etc/ssh/authorized_keys.d/...):
#
#   restrict,from="100.64.0.0/10",command="/usr/local/libexec/rtc-release-build" ssh-ed25519 AAAA... rtc-release
#
# Tailscale SSH must be off on the host: it would take port 22 from the
# tailnet and skip authorized_keys and the forced command. See
# docs/releasing.md.
#
# The workflow sends `build X.Y.Z <commit sha>` as the command and a
# `git archive` of that commit on stdin. Stdout carries only a tar of the
# packages and MANIFEST.sha256; everything else goes to stderr.
set -euo pipefail
exec 3>&1 1>&2
export PATH=/usr/bin:/bin LC_ALL=C
umask 022

SDP=/opt/qnx800
MAX_SRC=$((64 * 1024 * 1024))

fail() {
	echo "rtc-release-build: $*"
	exit 2
}

cmd=${SSH_ORIGINAL_COMMAND:-}
re='^build ([0-9]+\.[0-9]+\.[0-9]+) ([0-9a-f]{40})$'
[[ $cmd =~ $re ]] || fail "refused: expected 'build X.Y.Z <40-hex commit>'"
VERSION=${BASH_REMATCH[1]}
SHA=${BASH_REMATCH[2]}

# One build at a time; a second one fails at once rather than queueing.
exec 9>"$HOME/.rtc-release-build.lock"
flock -n 9 || fail "another release build is running"

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

# The source: a git archive of SHA, capped in size.
head -c $((MAX_SRC + 1)) >"$W/src.tar"
[ "$(stat -c %s "$W/src.tar")" -le $MAX_SRC ] || fail "source archive over $MAX_SRC bytes"
got=$(git get-tar-commit-id <"$W/src.tar" || true)
[ "$got" = "$SHA" ] || fail "the archive is commit '${got:-none}', not $SHA"
if grep -qE '^/|(^|/)\.\.(/|$)' <<<"$(tar -tf "$W/src.tar")"; then
	fail "absolute or .. paths in the archive"
fi
mkdir "$W/src"
tar -xf "$W/src.tar" -C "$W/src" --no-same-owner --no-same-permissions
cd "$W/src"

# git archive stamps every file with the commit time.
BUILD_EPOCH=$(stat -c %Y Makefile)
export BUILD_EPOCH

# shellcheck disable=SC1091
source "$SDP/qnxsdp-env.sh" >/dev/null
mkdir "$W/out"
for arch in aarch64le x86_64; do
	echo "rtc-release-build: $VERSION $SHA $arch"
	# Children get neither the output channel (fd 3) nor the lock (fd 9).
	timeout 15m make -s qnx ARCH="$arch" VERSION="$VERSION" >"$W/make-$arch.log" 2>&1 3>&- 9>&- ||
		{ tail -40 "$W/make-$arch.log"; fail "build failed for $arch"; }
	make -s qnx-check ARCH="$arch" 3>&- 9>&-
	scripts/release/package.sh "$arch" "$VERSION" "$SHA" "$W/out" 3>&- 9>&-
done
(cd "$W/out" && sha256sum -- *.tar.gz >MANIFEST.sha256)
tar -C "$W/out" -cf - . >&3
echo "rtc-release-build: done"
