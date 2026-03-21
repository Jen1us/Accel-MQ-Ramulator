#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${OUT_DIR:-${ROOT}/hw_run/traces_jenius/flashattention_v2_min}"

APP_SRC="${ROOT}/workloads/flashattention_v2/flashattention_v2_min.cu"
APP_BIN="${OUT}/flashattention_v2_min"

CONDA_ENV="${CONDA_ENV:-accel-sim-nvcc}"
NVCC_ARCH="${NVCC_ARCH:-sm_89}"
NVCC_DEFINES="${NVCC_DEFINES:-}"

mkdir -p "${OUT}"

if ! command -v conda >/dev/null 2>&1; then
  echo "ERROR: conda not found; cannot build NVBit tracer on this machine." >&2
  exit 2
fi
if ! conda run -n "${CONDA_ENV}" nvcc --version >/dev/null 2>&1; then
  cat >&2 <<EOF
ERROR: conda env '${CONDA_ENV}' not found (or missing nvcc).

Create it (minimal CUDA toolkit just for nvcc/ptxas/cudart):
  conda create -y -n accel-sim-nvcc --override-channels \\
    -c https://conda.anaconda.org/nvidia -c https://repo.anaconda.com/pkgs/main \\
    cuda-nvcc=12.8 cuda-cudart=12.8 cuda-cudart-dev=12.8 cuda-cudart-static=12.8
EOF
  exit 2
fi

echo "[1/4] Build NVBit tracer (ARCH=${NVCC_ARCH}, env=${CONDA_ENV})..."
pushd "${ROOT}/util/tracer_nvbit" >/dev/null
./install_nvbit.sh
conda run -n "${CONDA_ENV}" env CXX=/usr/bin/g++ ARCH="${NVCC_ARCH}" make clean
conda run -n "${CONDA_ENV}" env CXX=/usr/bin/g++ ARCH="${NVCC_ARCH}" make -j"$(nproc)"
popd >/dev/null

echo "[2/4] Build CUDA driver..."
conda run -n "${CONDA_ENV}" nvcc -O3 -std=c++17 -lineinfo -arch="${NVCC_ARCH}" \
  ${NVCC_DEFINES} "${APP_SRC}" -o "${APP_BIN}"

echo "[3/4] Generate NVBit traces..."
rm -rf "${OUT}/traces"
mkdir -p "${OUT}"
pushd "${OUT}" >/dev/null
export CUDA_VISIBLE_DEVICES=0
export ACTIVE_FROM_START=0
export TRACE_FILE_COMPRESS=1
CONDA_PREFIX="$(conda run -n "${CONDA_ENV}" bash -lc 'echo $CONDA_PREFIX')"
export PATH="${CONDA_PREFIX}/bin:${PATH}"
LD_PRELOAD="${ROOT}/util/tracer_nvbit/tracer_tool/tracer_tool.so" "${APP_BIN}"
"${ROOT}/util/tracer_nvbit/tracer_tool/traces-processing/post-traces-processing" \
  "${OUT}/traces"
popd >/dev/null

echo "[4/4] Run Accel-Sim (trace-driven) + dump HBF/HBM request trace..."
pushd "${OUT}" >/dev/null
GPGPUSIM_CUDART_DIR="$(dirname "$(find "${ROOT}/gpu-simulator/gpgpu-sim/lib" -type f -name libcudart.so | head -n 1)")"
export LD_LIBRARY_PATH="${GPGPUSIM_CUDART_DIR}:${LD_LIBRARY_PATH-}"
"${ROOT}/gpu-simulator/bin/release/accel-sim.out" \
  -trace "${OUT}/traces/kernelslist.g" \
  -config "${ROOT}/gpu-simulator/gpgpu-sim/configs/tested-cfgs/SM86_RTX3070/gpgpusim.config" \
  -config "${ROOT}/gpu-simulator/configs/tested-cfgs/SM86_RTX3070/trace.config" \
  -config "${ROOT}/workloads/flashattention_v2/hbf.config"
popd >/dev/null

echo "Trace written: ${OUT}/hbf_requests.trace"
echo -n "HBM count: "
rg -c ",HBM$" "${OUT}/hbf_requests.trace" || true
echo -n "HBF count: "
rg -c ",HBF$" "${OUT}/hbf_requests.trace" || true
