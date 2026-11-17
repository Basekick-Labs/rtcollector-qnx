# Releasing

Maintainers only. A release is built from `main` by `.github/workflows/release.yml`
and published by hand.

## Cut a release

1. On `main`, add `RELEASE_NOTES_X.Y.Z.md` (what changed, for users) through a
   PR as usual, and wait for CI.
2. Create the release branch from `main` with no extra commits:

   ```sh
   git switch main && git pull
   git push origin main:refs/heads/release/X.Y.Z
   ```

3. The workflow runs the host tests, then waits for a reviewer on the
   `release` environment. Approving it lets the QNX build run on the build
   host: the workflow sends `git archive` of the commit over SSH (Tailscale),
   the build host builds both architectures with the QNX SDP as an
   unprivileged user, checks them (`make qnx-check`) and returns the
   packages. The workflow verifies them and creates a **draft** release
   `vX.Y.Z` at that commit, with `SHA256SUMS` and the notes.
4. Check the draft: notes, both tarballs, `SHA256SUMS`. Optionally download
   the aarch64 tarball and run it on a Pi (`docs/getting-started.md`).
5. Publish it. That creates the tag `vX.Y.Z`. Check that it points at the
   commit named in the release body: `git fetch --tags && git rev-parse vX.Y.Z`.

Re-running the workflow for the same version replaces the draft. A published
release is never replaced; fix forward with X.Y.Z+1.

## What a release contains

`rtcollector-X.Y.Z-qnx8.0-<arch>.tar.gz` for `aarch64le` (Raspberry Pi 4) and
`x86_64` (QEMU): stripped `rtcollector` and `rtc_writer`,
`rtcollector-start.sh`, `rtcollector.yml`, `rtc_publish.h`, `LICENSE`,
`NOTICE`, `licenses/`, `README.txt`. The binaries carry no QNX library (`make
qnx-check`); see NOTICE.

## Setup (once)

- Repository Actions secrets, used only by the `qnx-build` job:
  `TAILSCALE_AUTH_KEY` (ephemeral, tagged key), `RELEASE_BUILD_HOST`,
  `RELEASE_BUILD_USER`, `RELEASE_BUILD_SSH_KEY`, `RELEASE_BUILD_KNOWN_HOSTS`.
- Environment `release` with a required reviewer, deployable from
  `release/*` only; a ruleset so only maintainers create `release/*`.
- On the build host: the user named in `RELEASE_BUILD_USER` (no sudo), the
  QNX SDP 8.0 at `/opt/qnx800` (read-only), its QNX licence, and
  `scripts/release/atila-build.sh` installed root-owned as
  `/usr/local/libexec/rtc-release-build`, the forced command of the release
  key in that user's `authorized_keys`:
  `restrict,command="/usr/local/libexec/rtc-release-build" ssh-ed25519 ...`.
  Reinstall it whenever the script changes.
