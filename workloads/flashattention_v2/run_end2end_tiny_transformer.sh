#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"

# "Tiny transformer" preset (well under 3B params).
# Chosen to keep NVBit traces + Accel-Sim runtime manageable while still having
# multiple layers/heads and a non-trivial KV working set.
export OUT_DIR="${OUT_DIR:-${ROOT}/hw_run/traces_jenius/tiny_transformer}"
export NVCC_DEFINES="${NVCC_DEFINES:--DHEAD_DIM=64 -DHEADS_NUM=32 -DLAYERS_NUM=22 -DLOGICAL_SEQ_LEN=256 -DPHYSICAL_RING_LEN=256 -DGEN_LEN=1}"

bash "${ROOT}/workloads/flashattention_v2/run_end2end_cuda.sh"
