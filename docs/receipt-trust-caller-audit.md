# Receipt Trust Caller Audit

Provenance-sensitive gameplay uses
`cargo_receipt_evaluate_at_station()` before changing cargo, receipts,
ownership, credits, ledgers, contracts, construction progress, production
output, or NPC delivery state.

The audited mutation modules are:

- `server/game_sim.c`: player station sale/delivery paths;
- `server/sim_production.c`: smelt, craft, and production input/output paths;
- `server/sim_construction.c`: station and module material delivery;
- `server/sim_ai.c`: NPC hauling, station acceptance, and delivery completion;
- `server/cargo_receipt_issue.c`: transfer preparation and receipt
  presentation.

`scripts/check_cargo_trust_boundaries.py` keeps this boundary executable. Raw
`cargo_receipt_chain_verify()` and `cargo_receipt_verify_signature()` calls are
restricted to the server's receipt/trust implementation, so gameplay cannot
silently substitute cryptographic validity for origin, authority-lifecycle,
and station-policy trust. The audit also rejects ignored
`chain_log_emit_batch()` and `cargo_receipt_commit_prepared_transfer()` results.
Its per-module evaluator minima make removal of an audited caller an explicit,
reviewed policy update.

Low-level receipt storage, wire decoding, client inspection, save migration,
and test/tool verification are outside the gameplay decision boundary. They
may validate chain integrity, but they cannot authorize an authoritative
gameplay mutation.

`shared/settlement_engine.c` is a separate offline import boundary. Its
trusted segment API accepts caller-resolved receipt/origin/authority evidence,
reuses `cargo_receipt_trust_verify()`, binds the final receipt holder to the
event actor, and applies only through a private all-or-nothing state copy.
The legacy event/segment APIs reject transfer, sell, and construction-input
events because they cannot carry that evidence.

Transaction fault tests remain the behavioral proof: blocked and injected
append failures at SMELT, CRAFT, TRANSFER, and TRADE callers must leave cargo
stores, pods, balances, ledgers, contracts, construction state, ownership, and
produced outputs unchanged.
