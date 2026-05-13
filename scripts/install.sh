#!/bin/bash
set -e

MODULE_ID="krautdrums"
MOVE_HOST="${MOVE_HOST:-move.local}"
DEST="/data/UserData/schwung/modules/sound_generators/${MODULE_ID}"

if [ ! -f "dist/${MODULE_ID}/dsp.so" ]; then
  echo "Error: dist/${MODULE_ID}/dsp.so not found. Run ./scripts/build.sh first."
  exit 1
fi

echo "Installing ${MODULE_ID} to ${MOVE_HOST}:${DEST}..."

# Create destination on Move (use root via ssh, then chown to ableton)
ssh "ableton@${MOVE_HOST}" "mkdir -p ${DEST}"

# Flat scp to avoid nested-dir bug
scp \
  "dist/${MODULE_ID}/dsp.so" \
  "dist/${MODULE_ID}/module.json" \
  "ableton@${MOVE_HOST}:${DEST}/"

# Ensure executable bit on .so (dlopen fails silently without it)
ssh "ableton@${MOVE_HOST}" "chmod +x ${DEST}/dsp.so"

echo ""
echo "✅ Deployed to ${MOVE_HOST}:${DEST}/"
echo ""
echo "Next steps on the Move:"
echo "  1. Open Schwung"
echo "  2. Add a Sound Generator slot"
echo "  3. Select 'KrautDrums' from the list"
echo "  4. If module.json was changed, power-cycle the Move (it caches metadata)"
