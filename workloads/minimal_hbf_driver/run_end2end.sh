#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

OUT_DIR="${OUT_DIR:-$ROOT/hw_run/traces_jenius/minimal_hbf_driver}"
# Use sm_86 by default so this runs on systems with older nvcc toolchains.
CUDA_ARCH="${CUDA_ARCH:-sm_86}"
DEVICE_ID="${DEVICE_ID:-0}"

BASE_GPGPUSIM_CONFIG="${BASE_GPGPUSIM_CONFIG:-$ROOT/gpu-simulator/gpgpu-sim/configs/tested-cfgs/SM86_RTX3070/gpgpusim.config}"
TRACE_CONFIG="${TRACE_CONFIG:-$ROOT/gpu-simulator/configs/tested-cfgs/SM86_RTX3070/trace.config}"
HBF_CONFIG="${HBF_CONFIG:-$ROOT/workloads/minimal_hbf_driver/hbf.config}"

# Optional newer CUDA tools (cuobjdump) for NVBit on SM89 GPUs.
CUDA_TOOLS_BIN="${CUDA_TOOLS_BIN:-$ROOT/tools/cuda-12.8/bin}"

echo "[1/6] Build Accel-Sim (gpgpu-sim + accel-sim.out)"
git config --global --add safe.directory "$ROOT" >/dev/null 2>&1 || true
git config --global --add safe.directory "$ROOT/gpu-simulator/gpgpu-sim" >/dev/null 2>&1 || true
set +u
source "$ROOT/gpu-simulator/setup_environment.sh" >/dev/null
set -u
if [ -d "$CUDA_TOOLS_BIN" ]; then
  export PATH="$CUDA_TOOLS_BIN:$PATH"
  if [ -z "${NVDISASM:-}" ] && [ -x "$CUDA_TOOLS_BIN/nvdisasm" ]; then
    export NVDISASM="$CUDA_TOOLS_BIN/nvdisasm"
  fi
fi
make -C "$ROOT/gpu-simulator" -j"$(nproc)"

echo "[2/6] Build NVBit + tracer"
"$ROOT/util/tracer_nvbit/install_nvbit.sh"
export ARCH="${ARCH:-sm_86}"
make -C "$ROOT/util/tracer_nvbit/tracer_tool" -j"$(nproc)"
make -C "$ROOT/util/tracer_nvbit/tracer_tool/traces-processing" -j"$(nproc)"

echo "[3/6] Build minimal CUDA app"
mkdir -p "$OUT_DIR"
nvcc -O2 -arch="$CUDA_ARCH" \
  "$ROOT/workloads/minimal_hbf_driver/min_hbf_driver.cu" \
  -o "$OUT_DIR/min_hbf_driver"

echo "[4/6] Run under NVBit (generate traces)"
rm -rf "$OUT_DIR/traces"
mkdir -p "$OUT_DIR/traces"
export CUDA_VISIBLE_DEVICES="$DEVICE_ID"
export TRACES_FOLDER="$OUT_DIR"
export ACTIVE_FROM_START=0
export TRACE_FILE_COMPRESS=1

CUDA_INJECTION64_PATH="$ROOT/util/tracer_nvbit/tracer_tool/tracer_tool.so" \
  "$OUT_DIR/min_hbf_driver" | tee "$OUT_DIR/app.log"

echo "[5/6] Post-process traces (kernelslist.g / *.traceg)"
"$ROOT/util/tracer_nvbit/tracer_tool/traces-processing/post-traces-processing" \
  "$OUT_DIR/traces"

echo "[6/6] Run Accel-Sim with HBF overlay"
"$ROOT/gpu-simulator/bin/release/accel-sim.out" \
  -trace "$OUT_DIR/traces/kernelslist.g" \
  -config "$BASE_GPGPUSIM_CONFIG" \
  -config "$HBF_CONFIG" \
  -config "$TRACE_CONFIG" \
  | tee "$OUT_DIR/sim.log"

echo "Done. Logs:"
echo "  $OUT_DIR/app.log"
echo "  $OUT_DIR/sim.log"
