# Production Ephemeral Relay

The production relay is an ephemeral Fargate task. Its filesystem is only a
task-local cache. Durable state must be explicit and external.

## Modes

`SIGNAL_PERSISTENCE_MODE` controls how the server treats local save files:

- `local`: load and save files in `SIGNAL_DATA_DIR`. This is the default for
  local development.
- `external_s3`: load and save files in `SIGNAL_DATA_DIR`, with the container
  entrypoint syncing that directory to `SIGNAL_STATE_S3_URI`.
- `ephemeral`: ignore local save/catalog/player/chain files and skip disk
  writes. This is useful for disposable test relays.

Production uses:

```sh
SIGNAL_PERSISTENCE_MODE=external_s3
SIGNAL_DATA_DIR=/app/data
SIGNAL_STATE_S3_URI=s3://signal-ratimics-state/prod/
SIGNAL_STATE_SYNC_INTERVAL_SEC=60
```

The server reports the active mode and external state URI in `/health` under
`persistence`.

## Cutover Caveat

The original production task used task-local files without ECS Exec or a
mounted volume. Those files cannot be exported from the running task after the
fact. The first `external_s3` production deployment therefore boots from the S3
prefix above; if that prefix is empty, the relay starts a fresh world and then
persists future state externally.

## What Is Durable

The current external store mirrors the existing file-backed layout:

- `world.sav`: session snapshot
- `saves/`: player saves, split into `pubkey/` and `legacy/`
- `stations/`: station catalog
- `chain/`: station signed event logs

This is a first externalization layer, not the final database model. It makes
the ECS task replaceable without trusting the previous task filesystem.

## Boot And Shutdown

The container entrypoint:

1. Creates `SIGNAL_DATA_DIR`.
2. In `external_s3` mode, syncs `SIGNAL_STATE_S3_URI` down into that directory.
3. Starts `signal-server` from inside the data directory.
4. Periodically syncs local state back to S3.
5. On `SIGTERM` / `SIGINT`, asks the server to shut down, waits for its final
   save, then performs a final S3 sync.

The boot sync is authoritative: if the S3 restore fails, the container exits
instead of starting from an unknown local directory. Periodic and shutdown
uploads log failures and keep the server process outcome intact, but the
dashboard and logs should be treated as unhealthy until sync is restored.

## Remaining Work

This keeps compute ephemeral, but the storage model is still file-shaped. The
next step is to split the mirrored directory into purpose-built stores:

- Station catalog and chain-log segments in object storage.
- Player profiles and session pointers in DynamoDB or another indexed store.
- Session snapshots as explicitly selected restore points.
- Chain-log head verification before accepting new appends.

See #577 for the production-runtime requirement and #314 for the broader
layered persistence refactor.
