# Signal C Safety Policy

Signal remains a C project, so memory safety is enforced by project mechanics:
strict builds, length-carrying data at boundaries, explicit ownership, bounded
parsing, sanitizer runs, static analysis, and review rules.

## Current Gates

- Normal C targets compile with `-Wall -Wextra -Wpedantic -Werror`.
- Linux, macOS, and Windows native builds run in GitHub Actions.
- `make test` rebuilds and runs fast `signal_test` shards.
- `make test-soak` runs the long-horizon sim tests.
- `make test-san` runs `signal_test` with ASan+UBSan and enables
  LeakSanitizer on Linux.
- `make test-msan` runs the instrumentable crypto, identity, station-authority,
  and signed-action subset with Clang MemorySanitizer.
- `make test-tsan` runs the bounded concurrent HNN/simulation regression. CI
  runs it weekly and on manual dispatch.
- `make banned-apis` fails on banned libc calls in owned C source.
- `make cppcheck`, scan-build, and clang-tidy run in CI static analysis.
- Nightly Valgrind checks run against the non-soak test suite.

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

## Sanitizer Scope

Linux CI is authoritative for leak detection. Its normal sanitizer job sets
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`. Apple's ASan runtime does not
support LeakSanitizer, so the same target uses `detect_leaks=0` on macOS while
retaining address and undefined-behavior coverage.

Fixture code must preserve production ownership rules. Use `SHIP_DECL`,
`STATION_DECL`, `STATION_ARRAY`, and `WORLD_DECL` for owned test objects. Use
`ship_reset()`, `station_reset()`, and the world slot-reset helpers instead of
overwriting a live object with `memset`; a live manifest must be cleaned before
its owning object is reset.

MemorySanitizer requires a consistently Clang-instrumented executable. Its CI
lane therefore covers a useful native-core subset that does not depend on
graphical or audio system libraries. This is an explicit supported scope, not
a source-file suppression.

The scheduled ThreadSanitizer lane runs a purpose-built regression with four
threads. Each thread initializes HNN key/feature caches and advances an
independent player-only simulation. HNN caches are thread-local: they retain
the single-thread hot-path cache without sharing mutable key-generation state
between simulation workers.

TweetNaCl is vendored upstream code. Its warning and signed-overflow/shift
sanitizer compatibility flags apply only to `SIGNAL_CRYPTO_VENDOR_SOURCES` in
`CMakeLists.txt`. Project-owned crypto glue, explicit wiping, identity, and
authority code remain fully instrumented.

## Secret Wiping

Use `signal_memzero_explicit()` for secret keys, secret-derived seeds, signing
scratch buffers, and lifecycle teardown. Ordinary `memset` is not a secret
wipe because an optimizer may remove dead stores.

The implementation selects:

- C23 `memset_explicit` when the compiler advertises C23;
- Windows `SecureZeroMemory`;
- a reviewed C11 volatile-byte-store fallback elsewhere.

The fallback's volatile stores are observable side effects and cannot be
elided. `make memzero-codegen` compiles the C11 fallback to optimized LLVM IR
and requires its volatile byte stores to remain. The runtime contract test
also calls the separately compiled wipe function, checks every byte, accepts
null/zero-length calls, and reports the selected backend. Keep clearing
centralized in this API so compiler and platform changes have one review
point.

Lifecycle boundaries currently include station cleanup/reset, authority-root
replacement and shutdown, identity teardown, network identity teardown,
secret-file/base64 temporaries, derived station seeds, and signing/verification
scratch buffers.

## Review Checklist

- Who owns every pointer crossing this function boundary?
- Can any stored pointer outlive its allocation or arena?
- Does every write know the destination capacity?
- Are all length calculations overflow-checked?
- Are parser failures tested, including short input and malformed input?
- Does ASan+UBSan pass for code touching parsing, serialization, save/load, or
network state?
- Does a lifecycle boundary wipe long-lived or temporary key material through
  `signal_memzero_explicit()`?
- Does fixture reset clean owned manifests before overwriting the object?
