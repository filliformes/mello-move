#!/bin/bash
# Push the WAV library to Move's /data partition.
#
# Layout: instruments/<library>/<bank>/*.wav
#   library = "Leisureland", "Sonic Bloom", … (the Main-page "Samples Folder")
#   bank    = "Mkii-Flute", "M400-8 Voice Choir", … (the Bank A / Bank B enum)
#
# Strategy: enumerate banks locally, enumerate banks on Move, push only the
# missing ones (per-bank scp).  This recovers from partial transfers — re-run
# until the local and remote bank counts match.  rsync is used when available
# (Linux/macOS); on Git Bash for Windows the script falls back to scp on a
# per-bank loop so a single failed bank doesn't abort the whole sync.
#
# Re-run safely: if a transfer dies, this script picks up where it left off
# (only banks present locally but absent on Move are pushed).  To force a
# full re-push of one bank, delete it on the Move first.
#
# Usage:
#   scripts/install_banks.sh                                # ALL libraries
#   scripts/install_banks.sh MT                             # one whole library
#   scripts/install_banks.sh "SB/MK2 Flute"                 # one specific bank
set -e
MODULE_ID="mello"
MOVE_HOST="${MOVE_HOST:-move.local}"
DEST_BASE="${DEST_BASE:-/data/UserData/schwung/modules/sound_generators}"
DEST="$DEST_BASE/$MODULE_ID/instruments"
SRC_BASE="$(cd "$(dirname "$0")/.." && pwd)/instruments"

ssh ableton@$MOVE_HOST "mkdir -p '$DEST'"

HAVE_RSYNC=0
if command -v rsync >/dev/null 2>&1; then HAVE_RSYNC=1; fi

# Push one bank directory.  Quotes the source path so spaces in bank names
# survive; remote destination has no spaces so a bare quoted arg is enough.
push_bank() {
    local lib="$1"
    local bank="$2"
    local src="$SRC_BASE/$lib/$bank"
    local dst="$DEST/$lib"
    if [ ! -d "$src" ]; then
        echo "  skip (no source): $lib/$bank" >&2
        return
    fi
    if [ "$HAVE_RSYNC" = "1" ]; then
        rsync -a --partial -e ssh "$src/" "ableton@$MOVE_HOST:$dst/$bank/" \
            || echo "  ERROR pushing $lib/$bank" >&2
    else
        scp -qr "$src" "ableton@$MOVE_HOST:$dst/" \
            || echo "  ERROR pushing $lib/$bank" >&2
    fi
}

# Sync one library: find local banks, find Move banks, push only missing.
sync_library() {
    local lib="$1"
    local src="$SRC_BASE/$lib"
    local dst="$DEST/$lib"
    if [ ! -d "$src" ]; then
        echo "skip (no source): $lib" >&2
        return
    fi

    echo ""
    echo "=== Library: $lib ==="
    ssh ableton@$MOVE_HOST "mkdir -p '$dst'"

    local local_list move_list missing
    local_list=$(ls -1 "$src" 2>/dev/null | sort)
    move_list=$(ssh ableton@$MOVE_HOST "ls -1 '$dst' 2>/dev/null" | sort)

    local n_local n_move
    n_local=$(echo "$local_list" | grep -cv '^$' || true)
    n_move=$(echo "$move_list"   | grep -cv '^$' || true)
    echo "  local: $n_local banks | move: $n_move banks"

    # Banks present locally but absent on Move (or zero-sized) — push them.
    missing=$(comm -23 <(echo "$local_list") <(echo "$move_list"))
    if [ -z "$missing" ]; then
        echo "  in sync"
    else
        local count
        count=$(echo "$missing" | grep -cv '^$')
        echo "  $count missing — pushing:"
        local i=0
        while IFS= read -r bank; do
            [ -z "$bank" ] && continue
            i=$((i+1))
            echo "  [$i/$count] $bank"
            push_bank "$lib" "$bank"
        done <<< "$missing"
    fi
}

# Sync one specific bank (lib/bank).  Always pushes (no diff check).
sync_one_bank() {
    local pair="$1"
    local lib="${pair%%/*}"
    local bank="${pair#*/}"
    echo "=== One bank: $lib/$bank ==="
    ssh ableton@$MOVE_HOST "mkdir -p '$DEST/$lib'"
    push_bank "$lib" "$bank"
}

if [ $# -eq 0 ]; then
    echo "Syncing ALL libraries to $MOVE_HOST:$DEST"
    [ "$HAVE_RSYNC" = "1" ] && echo "(rsync available — fast)" || echo "(no rsync — using per-bank scp)"
    for lib in "$SRC_BASE"/*/; do
        sync_library "$(basename "$lib")"
    done
else
    for arg in "$@"; do
        if [[ "$arg" == */* ]]; then
            sync_one_bank "$arg"
        else
            sync_library "$arg"
        fi
    done
fi

ssh ableton@$MOVE_HOST "chown -R ableton:users '$DEST'"
echo ""
echo "Done. Power-cycle the Move so chain_host picks up the new bank list."
echo "Verify counts: ssh ableton@$MOVE_HOST \"ls -1 '$DEST'/*/ | wc -l\""
