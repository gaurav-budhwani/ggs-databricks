#!/bin/sh
set -eu

DATA_DIR="${1:-${DATA_DIR:-/data}}"
OUTPUT_DIR="${2:-${OUTPUT_DIR:-/output}}"

if [ -n "${3:-}" ]; then
    SCRATCH_DIR="$3"
elif [ -d "${SCRATCH_DIR:-}" ] && [ -w "${SCRATCH_DIR:-}" ]; then
    SCRATCH_DIR="$SCRATCH_DIR"
else
    SCRATCH_DIR="$(mktemp -d 2>/dev/null || mktemp -d -t 'ss_scratch')"
fi

export DATA_DIR OUTPUT_DIR SCRATCH_DIR
export TMPDIR="$SCRATCH_DIR" HOME="$SCRATCH_DIR"

if [ -f /app/engine ]; then
    ENGINE="/app/engine"
elif [ -f ./bin/engine ]; then
    ENGINE="./bin/engine"
else
    ENGINE="engine"
fi

if [ -f /app/src/write_parquet.py ]; then
    PY_SCRIPT="/app/src/write_parquet.py"
else
    PY_SCRIPT="./src/write_parquet.py"
fi

if [ -f /app/.venv/bin/python ]; then
    PYTHON="/app/.venv/bin/python"
elif [ -f ./.venv/bin/python ]; then
    PYTHON="./.venv/bin/python"
elif command -v python3 >/dev/null 2>&1; then
    PYTHON="python3"
else
    PYTHON="python"
fi

# Step 1: Run C++ engine synchronously until completion
"$ENGINE" "$DATA_DIR" "$SCRATCH_DIR"

# Step 2: Run Python Parquet writer synchronously
"$PYTHON" "$PY_SCRIPT" "$SCRATCH_DIR" "$OUTPUT_DIR"
