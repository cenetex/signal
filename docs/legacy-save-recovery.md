# Authenticated legacy save recovery

The arbitrary-basename legacy claim protocol remains retired. The dedicated
server now has a bounded recovery path for the one canonical token-keyed save
that belongs to the currently authenticated session:

`saves/legacy/player_<lowerhex(current_session_token)>.sav`

The server never lists the legacy namespace and never accepts a basename,
path, token prefix, or other filesystem selector from the client.

## Protocol and lifecycle

After session authentication and pubkey proof, a pubkey save miss causes the
server to probe only the canonical token-derived source. If a source,
conflicting destination, or invalid canonical node exists, the server sends
one opaque offer containing a random 128-bit ID and a bounded lifetime. The
offer is bound to:

- the exact transport generation;
- the authenticated session token and proven pubkey;
- the consumed pubkey-proof challenge transcript; and
- its absolute expiry.

Confirmation is `SIGNED_ACTION_RECOVER_LEGACY_SAVE` and carries only the
opaque offer ID. The ordinary Ed25519 action envelope proves the pubkey and
supplies a monotonic nonce. A successful transaction persists
`max(decoded_nonce, live_nonce, confirmation_nonce)`.

The production client surfaces this pre-gameplay decision immediately:
`ENTER` signs the opaque offer and `ESC` closes the provisional connection
without changing the source. Input already held when the offer arrives is
disarmed, so approval always requires a fresh post-offer keypress. Semantic
success and rejection states are shown to the player instead of leaving an
unhandled offer to time out silently. The network layer latches that exact
connection-local offer and makes it the sole signed action admitted before
gameplay; a successful send consumes the latch, while result, disconnect,
reconnect, and authentication reset erase it. Issue #658 still owns the richer
docked, controller, mobile, and narrow-layout recovery surface.

An expired, malformed, stale, replayed, or failed attempt consumes the offer
but does not advance the live nonce. Until recovery commits, bootstrap does
not finalize a registry row, migrate a ledger, create or assign a ship, bind a
character, publish gameplay presence, or accept any signed action other than
the confirmation. On terminal failure the server erases that unpublished
slot before ending the global autosave pause and closing the transport. A
reconnect receives a new transport-bound opportunity. One idle or adversarial
claimant therefore cannot stall persistence indefinitely or leak provisional
authority into an unrelated generation.

## Source eligibility

All accepted sources are regular, single-link files between 12 bytes and
8 MiB. POSIX opens anchor the selected player directory and traverse
`legacy/` with `openat`, `O_DIRECTORY`, and `O_NOFOLLOW`; Windows holds
non-reparse handles for the selected directory and `legacy/` while opening the
file with `FILE_FLAG_OPEN_REPARSE_POINT`. Final-file symlinks, hardlinks, and
linked parent directories fail closed. The `pubkey/` destination parent is
checked through the same boundary.

PLY7 requires an exact CRC32 trailer and an exact structural decode. PLY4,
PLY5, and PLY6 predate that trailer but are the legacy formats this mechanism
exists to recover. They are accepted only through their exact historical
length envelope, bounded manifest/resource counts, finite and ranged ship
fields, and a full-file SHA-256 snapshot binding that is rechecked during the
commit. This is a deliberate compatibility exception: SHA-256 detects a
source change between validation and publication, but it cannot prove that an
old unchecksummed file was never corrupted before recovery. PLY1 through PLY3
are rejected because their raw ABI/global-credit layouts do not provide a
bounded ownership-safe migration.

## Transaction boundary

Recovery decodes into a detached world candidate. The live world is not
changed until all of the following succeed:

1. The canonical source's size, SHA-256 digest, single-link identity, and
   no-follow player/`legacy` pathname still match the validated snapshot, and
   the selected `pubkey` destination remains absent.
2. The complete previous player namespace is copied into a fresh immutable
   generation, omitting only that exact canonical source name.
3. The recovered pubkey save is durably published with atomic no-replace
   semantics. A destination created after the initial probe wins and is never
   overwritten.
4. The matching world snapshot, station catalogs, player namespace, and
   a bearer-free `LEGACY-RECOVERY-CONSUMED` marker become durable. The
   generation manifest authenticates the marker, source digest, and proven
   destination key.
5. `CURRENT` atomically selects the complete generation. A recovery pointer
   intentionally records no `previous` generation: the source-bearing
   predecessor remains an archive but can never be selected by automatic
   fallback after consumption. A later ordinary generation may restore a
   fallback edge because both sides then descend from the source-free
   recovery generation. An ambiguous
   post-rename directory-sync result is re-resolved before the live candidate
   is adopted.

Injected failures leave the live world, its nonce, and the selected
generation unchanged. Candidate generation debris is unselected and ignored.
Audit records contain only a bounded result name; bearer material, paths,
offer IDs, pubkeys, and filenames are not logged.

## Durable ownership limitation

Exact token-derived station-ledger rows can be promoted to the proven pubkey,
and the player save carries its bounded ship cargo, manifest, receipt chains,
and nonce. Current world-owned contracts, deliveries, builds, and ship assets
already use canonical actor principals.

Historical v81 migration intentionally quarantined ambiguous legacy ownership
without retaining session tokens. A quarantine row records only a public
reason and source locator; it cannot prove which later claimant owned the
row. Slot-based inference would recreate the authorization vulnerability this
work removes. Recovery therefore fails with `migration-failure` whenever any
unresolved ownership quarantine row exists, rather than partially promoting a
player while silently abandoning contracts, deliveries, builds, scaffolds, or
assets.

This is the remaining blocker to closing #672 for every historical world. A
future operator reconciliation format must add non-bearer ownership evidence
or explicitly retire each quarantined row. The expanded UI in #658 may consume
only the server-authorized opaque offer and must never restore legacy-save
browsing or basename entry.

## Regression coverage

The C suite covers proof/connection/expiry binding, byte-exact provisional
slot cleanup followed by an unrelated generation round trip, entropy failure,
replay and wrong-ID consumption, nonce byte identity, PLY4–PLY7 envelopes,
PLY1–PLY3 rejection, CRC corruption, final and parent symlinks, hardlinks,
source swaps, candidate and selected-namespace destination races, no-replace
publication, every generation fault phase, visible-pointer recovery,
source-bearing fallback fencing after current-generation loss, quarantine
refusal, audit redaction, and restart loading of the published pubkey save.
