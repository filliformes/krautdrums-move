#!/bin/bash
set -e

MODULE_ID="krautdrums"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."

echo "Building $MODULE_ID..."

# Build the cross-compile image
docker build -t schwung-builder "$SCRIPT_DIR"

mkdir -p "$ROOT/dist/$MODULE_ID"

# Use docker create + cp pattern for Windows MSYS compatibility
# (the -v mount pattern breaks on MINGW/Git Bash with mangled paths)
CONTAINER_ID=$(MSYS_NO_PATHCONV=1 docker create -w /build schwung-builder bash -c "
  dos2unix /build/src/dsp/${MODULE_ID}.c 2>/dev/null || true
  aarch64-linux-gnu-gcc \
    -O2 -shared -fPIC -ffast-math \
    -o /build/dist/${MODULE_ID}/dsp.so \
    /build/src/dsp/${MODULE_ID}.c \
    -lm
")

# Copy source into container
docker cp "$ROOT/." "$CONTAINER_ID:/build/"

# Run build
docker start -a "$CONTAINER_ID"

# Explicit exit-code check — Git Bash/MSYS doesn't propagate docker exit through `set -e`,
# so a compile failure inside the container can otherwise sneak past and we deploy stale code.
EXIT_CODE=$(docker inspect "$CONTAINER_ID" --format='{{.State.ExitCode}}')
if [ "$EXIT_CODE" != "0" ]; then
    echo "ERROR: Compile failed (exit $EXIT_CODE). See output above."
    docker rm "$CONTAINER_ID" > /dev/null
    exit 1
fi

# Extract artifacts
docker cp "$CONTAINER_ID:/build/dist/${MODULE_ID}/dsp.so" "$ROOT/dist/${MODULE_ID}/dsp.so"

# Cleanup
docker rm "$CONTAINER_ID" > /dev/null

# Copy module.json alongside the .so
cp "$ROOT/src/module.json" "$ROOT/dist/$MODULE_ID/"

# Copy help.json if present (Shadow UI loads it via per-module dir walk)
[ -f "$ROOT/src/help.json" ] && cp "$ROOT/src/help.json" "$ROOT/dist/$MODULE_ID/"

# Bundle for release
tar -czf "$ROOT/dist/${MODULE_ID}-module.tar.gz" -C "$ROOT/dist" "$MODULE_ID/"

echo "Built: dist/${MODULE_ID}-module.tar.gz"
ls -l "$ROOT/dist/${MODULE_ID}/"
