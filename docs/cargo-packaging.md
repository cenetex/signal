# Cargo package identity

Cargo packages group payload; they are not construction cells. The accepted
handling batch is four members because the refinery creates four ingots per
fragment. A partial batch contains one to three real members and carries the
explicit `CARGO_PACKAGE_FLAG_PARTIAL` bit. Missing icon positions are empty;
no placeholder cargo unit is minted.

`cargo_package_t` keeps three identities separate:

1. each complete `cargo_unit_t` member and its `pub`;
2. `package_pub`, a domain-separated hash of version, member count and the
   canonical sorted-member Merkle root;
3. `carrier_shell_pub`, an optional reference to the independent physical hex
   shell.

Pack and unpack are reversible reorderings. They consume no struts and retain
every complete member row. Moving a package between carrier, ship and station
changes only custody metadata; the package root and members do not change.

The canonical `PKG1` codec is padding-free apart from the already pinned
80-byte cargo-unit rows. It is suitable for save or wire embedding and
validates the root on decode. Old units whose `pub` is unavailable remain
members with `provenance=UNKNOWN` and a zero package root. This is deliberate:
loading legacy cargo never fabricates a proof. A future world-save migration
may wrap those rows with this codec without changing that rule.

Unpacking does not tombstone the package identity in V1. A custody/event log
records the transition; recreating the same canonical member set recreates the
same content identity, just as moving a content-addressed manifest does.
