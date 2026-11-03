# libyaml 0.2.5 (vendored)

Upstream: https://github.com/yaml/libyaml, release 0.2.5,
`yaml-0.2.5.tar.gz` sha256
`c642ae9b75fee120b2d96c712538bd2cf283228d2337df2cf2988e3c02678ef4`.
MIT licence, see `License`. Unmodified: only `include/yaml.h`, `src/*.c` and
`src/yaml_private.h` are copied.

Used only for QNX builds: QNX SDP 8.0 does not ship libyaml. Host builds link
the system libyaml. Built with its own flags (no `-Werror`); see the Makefile.
