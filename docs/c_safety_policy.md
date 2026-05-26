# Signal C Safety Policy

Signal remains a C project, so memory safety is enforced by project mechanics:
strict builds, length-carrying data at boundaries, explicit ownership, bounded
parsing, sanitizer runs, static analysis, and review rules.

## Current Gates

- Normal C targets compile with `-Wall -Wextra -Wpedantic -Werror`.
- Linux, macOS, and Windows native builds run in GitHub Actions.
- `make test` rebuilds and runs fast `signal_test` shards.
- `make test-soak` runs the long-horizon sim tests.
- `make test-san` runs `signal_test` with ASan+UBSan locally.
- `make test-tsan` is available for threaded changes.
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

## Review Checklist

- Who owns every pointer crossing this function boundary?
- Can any stored pointer outlive its allocation or arena?
- Does every write know the destination capacity?
- Are all length calculations overflow-checked?
- Are parser failures tested, including short input and malformed input?
- Does ASan+UBSan pass for code touching parsing, serialization, save/load, or
network state?
