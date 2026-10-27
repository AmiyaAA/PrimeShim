#!/bin/sh
# The APK no longer bundles HP firmware or A:/C: data.
#
# Users import their own official update package (zip or PRIME_APP.DAT) on
# first launch.  This script only checks that the UI assets are present.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST="$ROOT/android/app/src/main/assets"

for asset in hp_keyboard.skin hp_keys.txt portrait_skin.png app_icon_default.png; do
    if [ ! -f "$DEST/$asset" ]; then
        echo "missing UI asset: $DEST/$asset" >&2
        exit 1
    fi
done

echo "UI assets ready in $DEST (no firmware/data is bundled):"
du -sh "$DEST"/* 2>/dev/null || true
