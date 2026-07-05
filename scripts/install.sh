#!/bin/bash
# Fast deploy: dsp.so + module.json + help.json only. Run install_banks.sh
# separately for the (much slower) sample library copy.
#
# RUN THIS FROM YOUR COMPUTER — not on the Move itself. The script uses
# ssh/scp to push files TO the Move over the network.
set -e
MODULE_ID="mello"
MOVE_HOST="${MOVE_HOST:-move.local}"
DEST_BASE="${DEST_BASE:-/data/UserData/schwung/modules/sound_generators}"
DEST="$DEST_BASE/$MODULE_ID"

# Guard: refuse to run ON the Move. /dev/ablspi0.0 is the Move's SPI device —
# it exists only on the device itself. (Real user report: someone SSH'd into
# the Move and ran this there; it failed with "ssh: command not found".)
if [ -e /dev/ablspi0.0 ]; then
    echo "ERROR: this script must run on your COMPUTER, not on the Move." >&2
    echo "Exit this SSH session, then from your computer's terminal run:"  >&2
    echo "  git clone https://github.com/filliformes/mello-move.git"        >&2
    echo "  cd mello-move && ./scripts/build.sh && ./scripts/install.sh"    >&2
    exit 1
fi

if [ ! -f "dist/$MODULE_ID/dsp.so" ]; then
    echo "ERROR: dist/$MODULE_ID/dsp.so not found — run ./scripts/build.sh first." >&2
    exit 1
fi

echo "Installing $MODULE_ID to $MOVE_HOST..."
ssh ableton@$MOVE_HOST "mkdir -p $DEST/instruments"
scp "dist/$MODULE_ID/dsp.so" "dist/$MODULE_ID/module.json" "dist/$MODULE_ID/help.json" "ableton@$MOVE_HOST:$DEST/"
ssh ableton@$MOVE_HOST "chmod +x $DEST/dsp.so && chown -R ableton:users $DEST"

# Verify the transfer: remote md5 must match local. A truncated scp or a
# deploy to the wrong path otherwise fails SILENTLY and you test a stale
# binary without knowing.
LOCAL_MD5=$(md5sum "dist/$MODULE_ID/dsp.so" | cut -d' ' -f1)
REMOTE_MD5=$(ssh ableton@$MOVE_HOST "md5sum $DEST/dsp.so" | cut -d' ' -f1)
if [ "$LOCAL_MD5" != "$REMOTE_MD5" ]; then
    echo "ERROR: md5 mismatch after copy (local $LOCAL_MD5, remote $REMOTE_MD5)" >&2
    exit 1
fi
echo "Verified: dsp.so on Move matches local build ($LOCAL_MD5)."

# Warn about duplicate installs — an old copy under move-anything/ can be
# loaded INSTEAD of this one, so your new build never takes effect.
DUPES=$(ssh ableton@$MOVE_HOST \
    "ls -d /data/UserData/move-anything/modules/sound_generators/$MODULE_ID 2>/dev/null" || true)
if [ -n "$DUPES" ]; then
    echo ""
    echo "WARNING: found another Mello install at:"
    echo "  $DUPES"
    echo "The framework may load THAT copy instead of the one just installed."
    echo "Remove it with:"
    echo "  ssh ableton@$MOVE_HOST 'rm -rf $DUPES'"
fi

echo ""
echo "========================================================"
echo " POWER-CYCLE THE MOVE NOW (full off/on, not just sleep)"
echo " module.json is cached at startup — without a power"
echo " cycle you will keep hearing the PREVIOUS version."
echo "========================================================"
echo ""
echo "Banks: run scripts/install_banks.sh to copy the WAV library (~5 GB)."
