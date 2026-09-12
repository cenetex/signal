# Local world saves

Local mode resumes the selected player's world at the last completed
checkpoint. Native storage lives in `local-<scope>` beside `identity.key`.
The browser stores that directory in IndexedDB under `/signal-client`.
The scope uses the same player key and Local Sector One domain as
[player progress](player-progress.md).

A checkpoint contains the world, station catalog, player ship, and local
story and guide flags. Saves follow docking, station balances, upgrades,
signed actions, construction, and guidance changes. Timed checkpoints run
every five seconds while docked and every thirty seconds during flight.
Native shutdown drains the writer and requests a final checkpoint. Browser
visibility changes request a checkpoint; the last completed IndexedDB
transaction is the recovery point after an abrupt tab or process exit.

Native clients use the server's background snapshot writer. The browser
serializes into its in-memory filesystem, then writes the complete change
through one IndexedDB transaction. An OS file lock or browser Web Lock gives
one session exclusive storage access. A second browser tab shows a message
with the steps to resume in that tab.

Startup validates the published generation and its hashes. A damaged current
generation selects its authenticated previous generation. An invalid pair
leaves the files available for recovery and shows a recovery message. Storage
write failures keep the session running and show a retry notice.

World format v85 records the verified borrower of each station loan. Player
format PLY8 records the exact hull and local guidance flags. A returning
borrower resumes the same hull, upgrades, and cargo. Another player can use a
free station loaner. Station ownership remains attached to the borrowed hull.
Returning players resume at their saved station through the shared player
loader. Supported older world and player formats follow their existing
migrations; v84 hulls begin with an empty borrower field.

Qualification includes native restart, currency preservation, exclusive
storage access, current/previous generation recovery, and the browser's real
reload and authentication path. Shared save tests cover repeated hull reloads,
borrower identity after network slot reuse, rejection of a foreign hull save,
and v84 world upgrade. The rejected foreign save preserves the authority
digest. Local guidance follows the recovered generation.
