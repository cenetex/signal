/*
 * identity.h -- Per-player Ed25519 identity, persisted client-side.
 *
 * Layer A.1 of the off-chain decentralization roadmap (#479).
 * The keypair is purely local in this slice: it is generated on first
 * launch, persisted to disk under a platform-appropriate path, and
 * surfaced in the HUD as an 8-char base58 prefix. The wire protocol
 * is unchanged — session_token still drives identity over the network.
 *
 * Layer A.2 will start sending the pubkey on connect; Layer A.3 will
 * sign inputs; Layer A.4 will migrate save files. None of those land
 * here.
 */
#ifndef SIGNAL_IDENTITY_H
#define SIGNAL_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

#include "signal_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES];
    uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES]; /* (seed || pub) — NaCl convention */
} player_identity_t;

/* Load the player identity from the platform default path. If the file
 * is missing, generate a fresh keypair and save it. If the file exists
 * but is corrupt (wrong size, IO error), rename the bad file to
 * "<path>.bad" so the user can recover it manually, and then generate
 * fresh. Returns true on success.
 *
 * Paths:
 *   POSIX (Linux):  $XDG_DATA_HOME/signal/identity.key
 *                   (default: ~/.local/share/signal/identity.key)
 *   macOS:          ~/Library/Application Support/signal/identity.key
 *   Windows:        %LOCALAPPDATA%\\signal\\identity.key
 *   wasm:           localStorage["signal:identity"] (base64 of secret)
 *
 * On POSIX the file is created with mode 0600 inside a 0700 directory.
 */
bool identity_load_or_generate(player_identity_t *out);

/* Test-only entry points: explicit path, no platform resolution. Useful
 * for unit tests that want to round-trip identities through TMP() paths
 * and verify the corruption recovery behavior. Both return true on
 * success. */
bool identity_load_or_generate_at(player_identity_t *out, const char *path);
bool identity_save_to(const player_identity_t *id, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* SIGNAL_IDENTITY_H */
