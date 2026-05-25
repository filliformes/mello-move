#!/bin/bash
# Fast deploy: dsp.so + module.json + help.json only. Run install_banks.sh
# separately for the (much slower) sample library copy.
set -e
MODULE_ID="mello"
MOVE_HOST="${MOVE_HOST:-move.local}"
DEST_BASE="${DEST_BASE:-/data/UserData/schwung/modules/sound_generators}"
DEST="$DEST_BASE/$MODULE_ID"

echo "Installing $MODULE_ID to $MOVE_HOST..."
ssh ableton@$MOVE_HOST "mkdir -p $DEST/instruments"
scp "dist/$MODULE_ID/dsp.so" "dist/$MODULE_ID/module.json" "dist/$MODULE_ID/help.json" "ableton@$MOVE_HOST:$DEST/"
ssh ableton@$MOVE_HOST "chmod +x $DEST/dsp.so && chown -R ableton:users $DEST"
echo "Done. Remove + re-add Mello from FX slot, or power-cycle Move."
echo "Verify: ssh ableton@$MOVE_HOST 'ls -la $DEST/'"
echo ""
echo "Banks: run scripts/install_banks.sh to copy the WAV library (~1 GB)."
