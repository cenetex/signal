# Signal C Safety Policy

Signal remains a C project, so memory safety is enforced by project mechanics:
strict builds, length-carrying data at boundaries, explicit ownership, bounded
parsing, sanitizer runs, static analysis, and review rules.

## Current Gates

- Normal C targets compile with `-Wall -Wextra -Wpedantic -Werror`.
- Linux, macOS, and Windows native builds run in GitHub Actions.
- `make test` rebuilds and runs fast `signal_test` shards.
- `make test-soak` runs every `RUN_SOAK` long-horizon test as a distinct
  pull-request status and is rerun before release or deployment.
- `make test-san` runs the non-soak suite with ASan+UBSan and enables
  LeakSanitizer on Linux; `make test-san-soak` covers every functional soak on
  a weekly schedule. Apple's ASan runtime keeps leak detection disabled.
- `make soak-automation` checks the exact tagged-test inventory, build and
  registry reachability, and every required native/sanitizer workflow.
- `make test-msan` runs a focused, fully instrumented native secret-lifecycle
  subset under Clang MemorySanitizer.
- `make test-tsan` runs the bounded four-thread HNN/independent-world
  regression. Both specialized sanitizers run weekly and on manual dispatch.
- `make banned-apis` fails on banned libc calls in owned C source.
- `make cppcheck` runs in CI against owned production C source.
- `make vendor-drift` checks and mutation-tests the Docker vendor-context
  invariant in CI.
- Nightly Valgrind checks run against the non-soak test suite.

## Static Analysis Scope

Cppcheck is the repository's blocking general-purpose static-analysis gate.
The checked-in `.clang-tidy` file provides C11 defaults for editors and local
experiments, with diagnostics limited to owned `client/`, `server/`, and
`shared/` headers. Clang-tidy and scan-build are not advertised as Make or CI
gates: their current output varies across LLVM/platform versions and the
existing diagnostic backlog has not been baselined. Promoting either tool to a
gate requires pinning a toolchain and making its selected checks clean; until
then, CI documentation must not claim that it ran them.

## Banned APIs

Owned source must not introduce:

`gets`, `strcpy`, `strcat`, `sprintf`, `vsprintf`, `scanf`, `sscanf`, `fscanf`,
`strncpy`, `strncat`, `atoi`, `atol`, `atoll`, `rand`, `tmpnam`, `mktemp`.

Use `snprintf` with checked return values, `memcpy` only after a visible length
check, `strtol`/`strtof` with full-tail validation, and `shared/rng.h` or an
explicit deterministic generator for random behavior.

Vendored sources are excluded from the banned API gate. Do not weaken the
project gate to accommodate vendor code.

## Pointers And Ownership

- Pointer parameters are non-null unless the name says `maybe_` or `optional_`.
- Returned or stored pointers need an ownership comment in public headers.
- Functions must not retain borrowed pointers unless documented.
- Pointer arithmetic belongs only in low-level serialization, parsing, or
geometry modules where bounds are visible.
- Free functions should null caller-owned pointers when practical.

## Buffers And Strings

- New public APIs should pass a length-carrying type from `shared/safe_types.h`
or an explicit pointer plus length/capacity pair.
- No write may occur unless the destination capacity is known in the same
scope or encoded in the target type.
- C strings are boundary data. Internal parsing and protocol paths should carry
lengths and add a NUL only when calling C-string APIs.
- `memcpy` and `memmove` are allowed for fixed-width protocol fields and
validated slices; reviewers should be able to see the bound.

## Integer Safety

- Allocation size calculations must use checked arithmetic.
- Sizes, indexes, and capacities should use unsigned size types unless the
wire/save format requires otherwise.
- Signed/unsigned conversions and truncating casts need a visible range check
or a comment explaining the wire-format cap.

## Error Paths

- Resource-owning functions should use one cleanup path.
- Functions that can fail should return `bool`, status enum, or a count with a
documented sentinel.
- Out-parameters are initialized only on success unless the function documents
a different contract.
- Every fixed memory or bounds bug gets a regression test.

## Secret Wiping

Use `signal_memzero_explicit()` for secret keys, secret-derived seeds, and
signing or verification scratch. Ordinary `memset` is not a secret wipe
because an optimizer may remove dead stores.

The shared implementation uses Windows `SecureZeroMemory`, C23
`memset_explicit` on other conforming platforms, or a reviewed C11
volatile-byte-store fallback. `make memzero-codegen` forces that fallback
through an optimized LLVM build and verifies that its observable zero stores
remain. Runtime tests separately check the byte-range contract.

Only the upstream `vendor/tweetnacl/tweetnacl.c` translation unit receives
TweetNaCl's warning and deliberate-UB sanitizer exemptions. Project-owned
entropy, wrapper, and wipe code must remain fully instrumented.

Long-lived lifecycle boundaries wipe the station authority root, station
private keys, client identity, and the networking identity copy. Derivation,
identity-file, base64, and network-preservation scratch is wiped before its
stack lifetime ends. Test fixtures must use `ship_reset()`, `station_reset()`,
or the cleanup-aware harness helpers before overwriting an object that owns a
manifest.

## Review Checklist

- Who owns every pointer crossing this function boundary?
- Can any stored pointer outlive its allocation or arena?
- Does every write know the destination capacity?
- Are all length calculations overflow-checked?
- Are parser failures tested, including short input and malformed input?
- Does ASan+UBSan pass for code touching parsing, serialization, save/load, or
network state?
- Does secret scratch cross a lifecycle boundary without an explicit wipe?
