#!/bin/bash
# Mello — ARM64 cross-compile via Docker. Uses the create + cp pattern (Windows
# safe) with an explicit exit-code check so silent compile failures don't ship
# a stale .so. See move skill memory "Git Bash on Windows: set -e ...".
set -e

MODULE_ID="mello"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
IMAGE="schwung-mello-builder"

# Build the cross-compile image (cached after first run).
docker build -t "$IMAGE" -f "$SCRIPT_DIR/Dockerfile" "$ROOT"

# Container builds the .so, copies module.json, tars dist/mello/.
CONTAINER=$(MSYS_NO_PATHCONV=1 docker create "$IMAGE" \
  bash -c "set -e && \
    mkdir -p /build/dist/$MODULE_ID && \
    cp /build/src/module.json /build/dist/$MODULE_ID/ && \
    aarch64-linux-gnu-gcc \
      -O3 -shared -fPIC -ffast-math \
      -march=armv8-a -mtune=cortex-a72 \
      -Wall -Wno-unused-parameter -Wno-unused-but-set-variable -Wno-unused-variable \
      -I /build/src/dsp \
      -o /build/dist/$MODULE_ID/dsp.so \
      /build/src/dsp/mello.c \
      /build/src/dsp/wav_bank.c \
      -lm -lpthread && \
    cd /build/dist && tar -czf /build/dist/$MODULE_ID-module.tar.gz $MODULE_ID/")

docker start -a "$CONTAINER"

# CRITICAL — `set -e` does not propagate docker exit codes on Git Bash / MSYS.
EXIT_CODE=$(docker inspect "$CONTAINER" --format='{{.State.ExitCode}}')
if [ "$EXIT_CODE" != "0" ]; then
    echo "ERROR: compile failed inside container (exit $EXIT_CODE)" >&2
    docker rm "$CONTAINER" > /dev/null
    exit 1
fi

mkdir -p "$ROOT/dist/$MODULE_ID"
docker cp "$CONTAINER:/build/dist/$MODULE_ID/dsp.so"           "$ROOT/dist/$MODULE_ID/dsp.so"
docker cp "$CONTAINER:/build/dist/$MODULE_ID-module.tar.gz"    "$ROOT/dist/$MODULE_ID-module.tar.gz"
docker rm "$CONTAINER" > /dev/null

cp "$ROOT/src/module.json" "$ROOT/dist/$MODULE_ID/"

echo "Built  : dist/$MODULE_ID/dsp.so"
echo "Archive: dist/$MODULE_ID-module.tar.gz"
