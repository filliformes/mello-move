#!/bin/bash
# Push the WAV library to Move's /data partition.
#
# Layout: instruments/<library>/<bank>/*.wav
#   library = "Leisureland", "Sonic Bloom", … (the Main-page "Samples Folder")
#   bank    = "Mkii-Flute", "M400-8 Voice Choir", … (the Bank A / Bank B enum)
#
# Uses rsync when available (fast, skips matching files). Falls back to scp
# for Git Bash on Windows, which doesn't ship rsync.
#
# Usage:
#   scripts/install_banks.sh                                # ALL libraries
#   scripts/install_banks.sh Leisureland                    # one whole library
#   scripts/install_banks.sh "Sonic Bloom/MK2 Flute"        # one specific bank
set -e
MODULE_ID="mello"
MOVE_HOST="${MOVE_HOST:-move.local}"
DEST_BASE="${DEST_BASE:-/data/UserData/schwung/modules/sound_generators}"
DEST="$DEST_BASE/$MODULE_ID/instruments"
SRC_BASE="$(cd "$(dirname "$0")/.." && pwd)/instruments"

ssh ableton@$MOVE_HOST "mkdir -p '$DEST'"

HAVE_RSYNC=0
if command -v rsync >/dev/null 2>&1; then HAVE_RSYNC=1; fi

# Sync a single path under SRC_BASE — either a library folder or a
# library/bank pair. The destination mirrors the source structure.
sync_path() {
    local rel="$1"
    local src="$SRC_BASE/$rel"
    local dst="$DEST/$rel"
    if [ ! -d "$src" ]; then
        echo "skip: '$rel' not found in $SRC_BASE" >&2
        return
    fi
    echo "Syncing $rel ..."
    ssh ableton@$MOVE_HOST "mkdir -p '$dst'"
    if [ "$HAVE_RSYNC" = "1" ]; then
        # -a recurses, so a library folder syncs the full bank/wav tree.
        rsync -av --progress -e ssh "$src/" "ableton@$MOVE_HOST:$dst/"
    else
        # scp -r recurses; needed because libraries contain bank subfolders.
        scp -qr "$src"/* "ableton@$MOVE_HOST:$dst/"
    fi
}

if [ $# -eq 0 ]; then
    echo "Syncing ALL libraries to $MOVE_HOST:$DEST ..."
    for lib in "$SRC_BASE"/*/; do
        sync_path "$(basename "$lib")"
    done
else
    for arg in "$@"; do
        sync_path "$arg"
    done
fi

ssh ableton@$MOVE_HOST "chown -R ableton:users '$DEST'"
echo "Done. Power-cycle the Move so chain_host picks up the new bank list."
