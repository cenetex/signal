# liblecore dependency pin

Signal vendors the liblecore 0.1.0 single-source C distribution for the
holographic reliability pilot. It is an ABI-0 preview and must remain private
to Signal's HNN backend adapter.

- Upstream: `https://github.com/AnOversizedMooseWithSocks/leCore`
- Library version: `0.1.0`
- ABI version: `0`
- ISA version: `1`
- Canonical source revision (embedded by upstream):
  `5da2817e8f2addcc15d3a97c17107c22289bb2609bbdd19f2c199d33238a5a55`
- `lecore.c` SHA-256:
  `f3283ebb033e295e5dbdc46d95add91ab154253169d6a2a1ab696464b051ed07`
- `lecore.h` SHA-256:
  `0ce1fb68095323450087b18b4f8fcbf76ec74fbb68e936f5649f2494b2634302`
- License: MIT; see `LICENSE`.

The installed preview did not include a Git commit identifier. The upstream
generator embeds the canonical-source SHA-256 in both amalgamation files, so
that content address is the source revision for this pin. The two distribution
file hashes are checked separately to detect packaging drift.

The dependency is not read from a developer installation. Configure with
`-DSIGNAL_HNN_ENABLE_LECORE=ON` to compile these exact sources in native or
WebAssembly builds. Enabling the dependency does not select it as Signal's
active numeric backend.
