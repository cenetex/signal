# Player progress

The economy guide and worker story share a small progress record. Its key is
the SHA-256 of a versioned domain, the public player key, and the selected
server endpoint. Local Sector One has its own scope.

For shared servers, native clients keep `progress-<key>.txt` beside `identity.key`. Writes flush a
temporary file and replace the prior record atomically. Browser clients keep
`signal_progress_v2:<key>` in local storage. A record contains the guide's ten
flags and the story's eight ordered milestones. Invalid records start at the
first guide and story steps.

The first selected browser scope receives the old `signal_onboarding` and
`signal_story_loop_v1` values. `signal_progress_v2_legacy_owner` binds that
one-time import to the selected player and endpoint. The old entries remain
available for recovery. Later scopes begin with their own progress.

Identity and endpoint selection happen before either guide loads. Reconnect
keeps the same scope. A different player, endpoint, or local/remote selection
loads its own record. A storage error leaves the current session usable and
prints a save warning.

Local worlds keep guide and story flags in the player file within the complete
world save generation. A checkpoint captures those flags with the ship,
station balances, cargo, and outposts. Recovery loads the matching flags from
the selected generation. The scoped record supplies the first local world's
initial guidance, including the browser migration above.

See [local world saves](local-world-saves.md) for save timing and recovery.
