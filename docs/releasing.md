# Releasing

Maintainers only. A release is built from `main` by `.github/workflows/release.yml`
and published by hand.

## Cut a release

1. On `main`, add `RELEASE_NOTES_X.Y.Z.md` (what changed, for users) through a
   PR as usual, and wait for CI.
2. Create the release branch from `main` with no extra commits (the workflow
   refuses a commit that is not on `main`):

   ```sh
   git switch main && git pull
   git push origin main:refs/heads/release/X.Y.Z
   ```

3. The workflow runs the host tests, then the QNX build on the build host:
   it sends `git archive` of the commit over SSH (Tailscale),
   the build host builds both architectures with the QNX SDP as an
   unprivileged user, checks them (`make qnx-check`) and returns the
   packages. The workflow verifies them and creates a **draft** release
   `vX.Y.Z` at that commit, with `SHA256SUMS` and the notes.
4. Check the draft: notes, both tarballs, `SHA256SUMS`. Optionally download
   the aarch64 tarball and run it on a Pi (`docs/getting-started.md`).
5. Before publishing, check that no tag `vX.Y.Z` exists (`git ls-remote --tags
   origin vX.Y.Z` prints nothing): an existing tag would win over the commit
   the draft names. Publish; that creates the tag. Then check it points at
   the commit named in the release body: `git fetch --tags && git rev-parse
   vX.Y.Z`.

Re-running the workflow for the same version replaces the draft. A published
release is never replaced; fix forward with X.Y.Z+1. Only one release build
runs on the build host at a time.

## What a release contains

`rtcollector-X.Y.Z-qnx8.0-<arch>.tar.gz` for `aarch64le` (Raspberry Pi 4) and
`x86_64` (QEMU): stripped `rtcollector` and `rtc_writer`,
`rtcollector-start.sh`, `rtcollector.yml`, `rtc_publish.h`, `LICENSE`,
`NOTICE`, `licenses/`, `README.txt`. `make qnx-check` proves the binaries
need only libraries the QNX images have and carry no libtraceparser; NOTICE
says what they do contain.

## Setup (once)

GitHub:

- Repository Actions secrets: `TAILSCALE_AUTH_KEY`, `RELEASE_BUILD_HOST`,
  `RELEASE_BUILD_USER`, `RELEASE_BUILD_SSH_KEY`, `RELEASE_BUILD_KNOWN_HOSTS`.
  Forks never receive them; anyone with push access to this repository can
  use them in a workflow, so keep push access to maintainers.
- `TAILSCALE_AUTH_KEY`: reusable, ephemeral, tagged (the tailnet policy lets
  that tag reach only the build host's port 22). Auth keys expire (90 days at
  most): rotate it, or use a Tailscale OAuth client instead.

Build host:

- A build user with no sudo and no other access, login shell `/bin/sh` (it
  reads no start-up files for commands run over SSH), the QNX SDP 8.0 at
  `/opt/qnx800` (root-owned, read-only) and a copy of the QNX licence in the
  build user's `~/.qnx`.
- `scripts/release/build-host.sh` installed root-owned as
  `/usr/local/libexec/rtc-release-build`; reinstall it whenever it changes.
- The release key in a root-owned file outside the build user's home, read
  through `Match User <build user>` / `AuthorizedKeysFile
  /etc/ssh/authorized_keys.d/<build user>` in the sshd configuration:
  `restrict,from="100.64.0.0/10",command="/usr/local/libexec/rtc-release-build" ssh-ed25519 ...`.
  The key only runs that script, only from the tailnet; the build runs the
  repository's Makefile as the build user, which therefore must not be able
  to change its own keys or start-up files.
- Tailscale SSH off on the build host (`tailscale set --ssh=false`): with it
  on, Tailscale answers port 22 from the tailnet and the forced command does
  not apply.
