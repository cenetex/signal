#!/bin/sh
# Push the Signal itch.io embed page to itch.io.
#
# Usage:
#   scripts/itch-release.sh          # dry run (validate the embed page)
#   scripts/itch-release.sh --push   # upload to itch.io
#
# The itch page embeds the live game at https://signal.ratimics.com —
# the iframe keeps the WebSocket connection same-origin with the live
# site, so no server changes are needed.
#
# Requirements: butler installed and logged in; the itch project must
# already exist at ITCH_USER/ITCH_GAME below.

set -eu

ITCH_USER="${ITCH_USER:-ratimics}"
ITCH_GAME="${ITCH_GAME:-signal}"
ITCH_CHANNEL="${ITCH_CHANNEL:-html}"
VERSION="${VERSION:-0.1.0}"
PAGE_DIR="itch"

if command -v butler >/dev/null 2>&1; then
    BUTLER=butler
else
    BUTLER="$HOME/.local/bin/butler"
fi
if [ ! -x "$(command -v "$BUTLER" 2>/dev/null || echo "$BUTLER")" ]; then
    echo "error: butler not found. Install it from https://itch.io/docs/butler/" >&2
    exit 1
fi

if [ ! -f "$PAGE_DIR/index.html" ]; then
    echo "error: $PAGE_DIR/index.html is missing" >&2
    exit 1
fi

LIVE="$(curl -s -o /dev/null -w '%{http_code}' -L https://signal.ratimics.com/play || true)"
if [ "$LIVE" != "200" ]; then
    echo "warning: signal.ratimics.com/play returned HTTP $LIVE — check the live site before pushing" >&2
fi

echo "==> Embed page ready: $PAGE_DIR"

if [ "${1:-}" != "--push" ]; then
    echo "==> Dry run. Re-run with --push to upload $ITCH_USER/$ITCH_GAME:$ITCH_CHANNEL"
    exit 0
fi

"$BUTLER" push "$PAGE_DIR" "$ITCH_USER/$ITCH_GAME:$ITCH_CHANNEL" \
    --userversion "$VERSION"
echo "==> Uploaded $ITCH_USER/$ITCH_GAME:$ITCH_CHANNEL version $VERSION"