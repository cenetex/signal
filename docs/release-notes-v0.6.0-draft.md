# v0.6.0 — First Outpost (draft)

Status: candidate work is in progress. Publish after the acceptance checks in
[the release plan](release-plan-v0.6.0.md) pass.

## Player changes

- Story and guide progress follow each player and their local world or server.
- Local play saves the world, exact hull, cargo, and progress in complete
  generations. Native saves use a background writer; browser saves use IndexedDB.
- Escape closes the active panel. Trade rows fit narrow screens, and receipts
  show readable station names.
- Crate prices include the carrier frame. The server sends the exact charge
  and source station for unpacking.
- A picked-up source crate stays on the player's tow until release.

## Downloads and compatibility

The candidate uses protocol 8, world save 85, and player save PLY8. Update the
server and clients together. Save readers support the documented older formats.

The media pack carries file hashes and source records. Native and web packages
include the same verified media. Server packages carry the executable and a
checksum list. Final download links follow archive qualification.

Before the production upgrade, record a coherent data snapshot and its matching
server image. A rollback restores that pair. The snapshot and brief service
pause are awaiting approval.

## Release checks still open

- Confirm the worker story through a new-player play session.
- Record the haul, combat, and camera review on named hardware.
- Qualify slow-disk saves and recovery with connected players.
- Supply the three station portraits and publication permission notes, then
  verify real native and web archives. All 28 tracks and 10 clips are recovered.
- Record the final memory-check result, then verify deployment and public
  version and chain health. Required implementation CI already passes.
