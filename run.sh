#!/bin/sh
set -eu
: "${DATA_DIR:=/data}" "${OUTPUT_DIR:=/output}" "${SCRATCH_DIR:=/scratch}"
export DATA_DIR OUTPUT_DIR SCRATCH_DIR
export TMPDIR="$SCRATCH_DIR" HOME="$SCRATCH_DIR"

/app/engine "$DATA_DIR" "$SCRATCH_DIR"
python3 /app/src/write_parquet.py "$SCRATCH_DIR" "$OUTPUT_DIR"
