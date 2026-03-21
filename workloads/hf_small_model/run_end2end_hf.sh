#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${OUT_DIR:-${ROOT}/hw_run/traces_jenius/hf_small_model}"

NVCC_ENV="${NVCC_ENV:-accel-sim-nvcc}"
PY_ENV="${PY_ENV:-accel-sim-hf}"
NVCC_ARCH="${NVCC_ARCH:-sm_89}"

MODEL_ID="${MODEL_ID:-roneneldan/TinyStories-1M}"
KEEP_LAYERS="${KEEP_LAYERS:-2}"
SEQLEN="${SEQLEN:-16}"
ROI="${ROI:-1}"
PROBE_ONLY="${PROBE_ONLY:-1}"
PROBE_ELEMENTS="${PROBE_ELEMENTS:-4096}"

mkdir -p "${OUT}"

if ! command -v conda >/dev/null 2>&1; then
  echo "ERROR: conda not found." >&2
  exit 2
fi

echo "[0/4] Ensure Python env (${PY_ENV}) exists..."
if ! conda run -n "${PY_ENV}" python -c "import sys; print(sys.version)" >/dev/null 2>&1; then
  conda create -y -n "${PY_ENV}" --override-channels -c https://repo.anaconda.com/pkgs/main python=3.12 pip
fi

echo "[0/4] Ensure Python deps (torch + transformers)..."
if ! conda run -n "${PY_ENV}" python -c "import torch, transformers" >/dev/null 2>&1; then
  conda run -n "${PY_ENV}" python -m pip install --upgrade pip
  conda run -n "${PY_ENV}" python -m pip install --upgrade \
    "torch" --index-url https://download.pytorch.org/whl/cu121
  conda run -n "${PY_ENV}" python -m pip install --upgrade \
    "transformers>=4.41.0" "safetensors" "huggingface_hub"
fi

echo "[1/4] Build NVBit tracer (ARCH=${NVCC_ARCH}, env=${NVCC_ENV})..."
pushd "${ROOT}/util/tracer_nvbit" >/dev/null
./install_nvbit.sh
conda run -n "${NVCC_ENV}" env CXX=/usr/bin/g++ ARCH="${NVCC_ARCH}" make -j"$(nproc)"
popd >/dev/null

echo "[2/4] Generate NVBit traces (HF model=${MODEL_ID})..."
rm -rf "${OUT}/traces"
mkdir -p "${OUT}"
pushd "${OUT}" >/dev/null

export CUDA_VISIBLE_DEVICES=0
export HF_HOME="${HF_HOME:-${ROOT}/hw_run/hf_cache}"
export TRANSFORMERS_CACHE="${TRANSFORMERS_CACHE:-${HF_HOME}/transformers}"

# Pre-download the model/tokenizer without NVBit loaded; then we can trace in
# offline mode to avoid network flakiness inside an instrumented process.
echo "[2/4] Pre-download model into HF cache..."
conda run -n "${PY_ENV}" python -c "from transformers import AutoModelForCausalLM, AutoTokenizer; model_id=${MODEL_ID@Q}; AutoTokenizer.from_pretrained(model_id); AutoModelForCausalLM.from_pretrained(model_id); print('cached_ok=1')"

# NVBit/NVBit-tracer knobs (override via env when debugging).
export ACTIVE_FROM_START="${ACTIVE_FROM_START:-0}"
export DYNAMIC_KERNEL_RANGE="${DYNAMIC_KERNEL_RANGE:-}"
export TERMINATE_UPON_LIMIT="${TERMINATE_UPON_LIMIT:-0}"
export TRACE_FILE_COMPRESS="${TRACE_FILE_COMPRESS:-0}"
export NO_EAGER_LOAD="${NO_EAGER_LOAD:-1}"

# Ensure NVBit can find cuobjdump/nvdisasm that support sm_89.
NVCC_PREFIX="$(conda run -n "${NVCC_ENV}" bash -lc 'echo $CONDA_PREFIX')"
export PATH="${NVCC_PREFIX}/bin:${PATH}"

PY_PREFIX="$(conda run -n "${PY_ENV}" bash -lc 'echo $CONDA_PREFIX')"
export HF_HUB_OFFLINE=1
export TRANSFORMERS_OFFLINE=1
ROI_FLAG="--roi"
if [[ "${ROI}" == "0" ]]; then
  ROI_FLAG="--no-roi"
fi
EXTRA_ARGS=()
if [[ "${PROBE_ONLY}" == "1" ]]; then
  EXTRA_ARGS+=(--probe-only --probe-elements "${PROBE_ELEMENTS}")
fi
CUDA_INJECTION64_PATH="${ROOT}/util/tracer_nvbit/tracer_tool/tracer_tool.so" \
  "${PY_PREFIX}/bin/python" "${ROOT}/workloads/hf_small_model/hf_runner.py" \
    --model "${MODEL_ID}" \
    --keep-layers "${KEEP_LAYERS}" \
    --seqlen "${SEQLEN}" \
    --warmup 0 \
    --iters 1 \
    "${EXTRA_ARGS[@]}" \
    --local-files-only \
    "${ROI_FLAG}"

"${ROOT}/util/tracer_nvbit/tracer_tool/traces-processing/post-traces-processing" \
  "${OUT}/traces"
popd >/dev/null

echo "[3/4] Run Accel-Sim (trace-driven) + dump HBF/HBM request trace..."
pushd "${OUT}" >/dev/null
GPGPUSIM_CUDART_DIR="$(dirname "$(find "${ROOT}/gpu-simulator/gpgpu-sim/lib" -type f -name libcudart.so | head -n 1)")"
export LD_LIBRARY_PATH="${GPGPUSIM_CUDART_DIR}:${LD_LIBRARY_PATH-}"

"${ROOT}/gpu-simulator/bin/release/accel-sim.out" \
  -trace "${OUT}/traces/kernelslist.g" \
  -config "${ROOT}/gpu-simulator/gpgpu-sim/configs/tested-cfgs/SM86_RTX3070/gpgpusim.config" \
  -config "${ROOT}/gpu-simulator/configs/tested-cfgs/SM86_RTX3070/trace.config" \
  -config "${ROOT}/workloads/hf_small_model/hbf.config"
popd >/dev/null

echo "Trace written: ${OUT}/hbf_requests.trace"
echo -n "HBM count: "
rg -c ",HBM$" "${OUT}/hbf_requests.trace" || true
echo -n "HBF count: "
rg -c ",HBF$" "${OUT}/hbf_requests.trace" || true
