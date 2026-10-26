# RATi receipt v1

One envelope carries the facts and result of a check. BRAID owns this format in
`contracts/rati-receipt/v1/`. Each consumer keeps the same schema and test vector.
Format changes require a new version and matching consumer tests.

## Record

- `profile` names the application's mapping and its version.
- `subject` gives a stable, scoped kind and ID.
- `claim` states exactly what the check covers.
- `rules` gives the evaluator ID, version, and SHA-256 commitment.
- `facts.content` preserves the JSON facts used by the evaluator as UTF-8 text.
- `result.content` preserves its JSON result as UTF-8 text.
- `outcome` is `pass`, `fail`, or `unknown`, for this claim and profile.
- `evaluated_at_unix_ms` records the check time as decimal milliseconds since Unix epoch.
- `validity: snapshot` limits the result to that reading. A later check makes a new receipt.
- `issuer` names the producing software. Authentication comes from the delivery channel
  or a separately verified signature. The SHA-256 is an integrity commitment.
- `sha256` binds every other field to one digest.

The JSON text fields retain the original number spelling, integer width, nulls,
and ordering. A reader checks the envelope first, then parses these two fields
with the application's own number handling. Each must be a JSON object.
Applications keep private evidence within their existing access controls.

## Digest algorithm

Validate the exact schema. Remove only the top-level `sha256`. Sort object keys
by ASCII order at every envelope level. Encode compact JSON as UTF-8 with no
spaces or final newline. Use literal Unicode, escape quote and backslash, use
`\b`, `\t`, `\n`, `\f`, `\r` for those controls and lowercase `\u00xx` for
other U+0000 through U+001F characters. Keep slashes literal. Require Unicode
scalar values in strings. All envelope keys are fixed ASCII. All leaves are
strings. Hash these bytes with SHA-256 and write 64 lowercase hex characters.

Treat `facts.content` and `result.content` as strings during this process.
Their exact text is covered by the digest. A change to their whitespace makes
a different receipt. Reordering the envelope's object keys preserves its digest.

`example.json` is the shared test vector. It covers Unicode, line escapes,
nulls, decimal spelling, and a 64-bit integer inside the preserved facts.

## Checking a receipt

1. Validate the envelope, version, and both JSON text fields.
2. Recompute the envelope digest.
3. Resolve the named profile and exact rules digest from a trusted source.
4. Replay the application's check on the retained facts or original artifacts.
5. Apply the caller's freshness policy to the recorded time.

Integrity verification and replay are separate results. A matching digest
establishes intact content; replay establishes that the recorded result follows
from the pinned rules and supplied evidence. Source truth and issuer identity
come from the application's evidence and authentication checks.

## Profiles in this change

| Profile | Existing record | Claim |
| --- | --- | --- |
| `braid.release-verification.v1` | Release manifest, expected ID, verification checks | Release integrity |
| `ratitrust.standards.v1` | Recorded stock/memecoin facts and standards result | Applicable RATi standards |
| `cosyworld.avatar-profile.v1` | Canonical avatar profile, world/epoch, entity versions, event position | Canonical profile resolution |
| `crownless.brain-verification.v1` | Brain receipt, current file hashes, repeated benchmark | Brain validation gates |
| `signal.station-log.v1` | Native station-log verification and smelt receipts | Station-log checks and recorded proof levels |
