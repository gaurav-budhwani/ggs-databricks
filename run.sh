#!/bin/sh
set -eu
DATA_DIR="${1:-${DATA_DIR:-/data}}"
OUTPUT_DIR="${2:-${OUTPUT_DIR:-/output}}"
SCRATCH_DIR="${3:-${SCRATCH_DIR:-/scratch}}"
export DATA_DIR OUTPUT_DIR SCRATCH_DIR
export TMPDIR="$SCRATCH_DIR" HOME="$SCRATCH_DIR"

if [ -f /app/engine ]; then
    ENGINE="/app/engine"
elif [ -f ./bin/engine ]; then
    ENGINE="./bin/engine"
else
    ENGINE="engine"
fi

exec "$ENGINE" "$DATA_DIR" "$OUTPUT_DIR" "$SCRATCH_DIR"
