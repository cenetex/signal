#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CDN_BASE=${SIGNAL_ASSET_CDN:-https://signal-ratimics-assets.s3.amazonaws.com}
ASSET_DIR=${SIGNAL_ASSET_DIR:-"$ROOT/assets"}
MANIFEST=${SIGNAL_ASSET_MANIFEST:-"$ROOT/assets/manifest.txt"}
FORCE=0

usage() {
    echo "usage: $0 [--force]" >&2
    echo "env: SIGNAL_ASSET_CDN, SIGNAL_ASSET_DIR, SIGNAL_ASSET_MANIFEST" >&2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --force) FORCE=1 ;;
        -h|--help) usage; exit 0 ;;
        *) usage; exit 2 ;;
    esac
    shift
done

if ! command -v curl >/dev/null 2>&1; then
    echo "sync-assets: curl is required" >&2
    exit 1
fi

if [ ! -f "$MANIFEST" ]; then
    echo "sync-assets: missing manifest: $MANIFEST" >&2
    exit 1
fi

mkdir -p "$ASSET_DIR"

while IFS= read -r rel || [ -n "$rel" ]; do
    case "$rel" in
        ""|\#*) continue ;;
    esac

    dest="$ASSET_DIR/$rel"
    if [ "$FORCE" -eq 0 ] && [ -s "$dest" ]; then
        echo "sync-assets: keep $rel"
        continue
    fi

    mkdir -p "$(dirname -- "$dest")"
    tmp="$dest.part"
    url="$CDN_BASE/$rel"
    echo "sync-assets: fetch $url"
    curl -fL --retry 3 --retry-delay 1 -o "$tmp" "$url"
    mv "$tmp" "$dest"
done < "$MANIFEST"

echo "sync-assets: complete in $ASSET_DIR"
