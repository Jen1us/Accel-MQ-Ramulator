#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${OUT_DIR:-${ROOT}/hw_run/traces_jenius/tiny_transformer_backend_map}"

APP_SRC="${ROOT}/workloads/flashattention_v2/tiny_transformer_backend_map.cu"
APP_BIN="${OUT}/tiny_transformer_backend_map"
META_FILE="${OUT}/backend_meta.tsv"
META_FILE_MISSING="${OUT}/backend_meta_missing_weights.tsv"
MAP_FILE="${OUT}/backend_map.txt"
MAP_FILE_MISSING="${OUT}/backend_map_missing_weights.txt"
CFG_FILE="${OUT}/backend_map.config"
CFG_FILE_MISSING="${OUT}/backend_map_missing_weights.config"
TRACE_FILE="${OUT}/hbf_requests.trace"
ANNOTATED_TRACE_FILE="${OUT}/hbf_requests_annotated.csv"

CONDA_NVCC_ENV="${CONDA_NVCC_ENV:-accel-sim-nvcc}"
CONDA_WRAPPER_ENV="${CONDA_WRAPPER_ENV:-base}"
NVCC_ARCH="${NVCC_ARCH:-sm_89}"
NVCC_DEFINES="${NVCC_DEFINES:--DHEAD_DIM=32 -DHEADS_NUM=2 -DLAYERS_NUM=1 -DLOGICAL_SEQ_LEN=16 -DPHYSICAL_RING_LEN=16 -DGEN_LEN=1 -DMLP_EXPAND=2 -DPROJ_TAPS=1 -DMLP_TAPS=1 -DATTN_WINDOW=2}"
MODEL_LAYERS="${MODEL_LAYERS:-1}"
BASE_GPGPUSIM_CONFIG="${BASE_GPGPUSIM_CONFIG:-${ROOT}/gpu-simulator/gpgpu-sim/configs/tested-cfgs/SM86_RTX3070/gpgpusim.config}"
TRACE_CONFIG="${TRACE_CONFIG:-${ROOT}/gpu-simulator/configs/tested-cfgs/SM86_RTX3070/trace.config}"
RUN_FAILFAST_CHECK="${RUN_FAILFAST_CHECK:-1}"

mkdir -p "${OUT}"

source "${HOME}/miniconda3/etc/profile.d/conda.sh"

build_wrapper() {
  local env_name="$1"
  local src_dir="$2"
  local build_dir="$3"
  conda run -n "${env_name}" cmake -S "${src_dir}" -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release
  conda run -n "${env_name}" cmake --build "${build_dir}" -j"$(nproc)"
}

write_config() {
  local cfg_path="$1"
  local map_path="$2"
  local trace_path="$3"
  cat > "${cfg_path}" <<EOF
-hbm_use_ramulator2 1
-hbm_ramulator2_wrapper ${ROOT}/external_wrappers/ramulator2_wrap/build/libramulator2_wrap.so
-hbm_ramulator2_config ${ROOT}/workloads/ramulator2/hbm2_16ch.yaml
-hbf_use_mqsim 1
-hbf_mqsim_wrapper ${ROOT}/external_wrappers/mqsim_wrap/build/libmqsim_wrap.so
-hbf_mqsim_config ${ROOT}/workloads/mqsim/hbf_1p2TBs.xml
-hbf_subpage_bytes 512
-hbf_partition_count 0
-hbf_random_access 0
-hbf_request_trace 1
-hbf_request_trace_file ${trace_path}
-hbf_request_trace_limit 0
-mem_backend_map_file ${map_path}
-mem_backend_map_default unspecified
EOF
}

prepare_accelsim_env() {
  git config --global --add safe.directory "${ROOT}" >/dev/null 2>&1 || true
  git config --global --add safe.directory "${ROOT}/gpu-simulator/gpgpu-sim" >/dev/null 2>&1 || true
  export CUDA_INSTALL_PATH=/usr
  set +u
  source "${ROOT}/gpu-simulator/setup_environment.sh" >/dev/null
  set -u
}

echo "[1/7] Build NVBit tracer"
pushd "${ROOT}/util/tracer_nvbit" >/dev/null
./install_nvbit.sh
conda run -n "${CONDA_NVCC_ENV}" env CXX=/usr/bin/g++ ARCH="${NVCC_ARCH}" make clean
conda run -n "${CONDA_NVCC_ENV}" env CXX=/usr/bin/g++ ARCH="${NVCC_ARCH}" make -j"$(nproc)"
popd >/dev/null

echo "[2/7] Build wrapper libraries"
build_wrapper "${CONDA_WRAPPER_ENV}" "${ROOT}/external_wrappers/mqsim_wrap" \
  "${ROOT}/external_wrappers/mqsim_wrap/build"
build_wrapper "${CONDA_WRAPPER_ENV}" "${ROOT}/external_wrappers/ramulator2_wrap" \
  "${ROOT}/external_wrappers/ramulator2_wrap/build"

echo "[3/7] Build tiny decoder-block backend-map driver"
conda run -n "${CONDA_NVCC_ENV}" nvcc -O3 -std=c++17 -lineinfo -arch="${NVCC_ARCH}" \
  ${NVCC_DEFINES} "${APP_SRC}" -o "${APP_BIN}"

echo "[4/7] Generate NVBit traces and buffer metadata"
rm -rf "${OUT}/traces"
mkdir -p "${OUT}"
pushd "${OUT}" >/dev/null
export CUDA_VISIBLE_DEVICES=0
export ACTIVE_FROM_START=0
export TRACE_FILE_COMPRESS=1
export TRACES_FOLDER="${OUT}"
export ACCELSIM_BACKEND_META_PATH="${META_FILE}"
CONDA_PREFIX="$(conda run -n "${CONDA_NVCC_ENV}" bash -lc 'echo $CONDA_PREFIX')"
export PATH="${CONDA_PREFIX}/bin:${PATH}"
LD_PRELOAD="${ROOT}/util/tracer_nvbit/tracer_tool/tracer_tool.so" "${APP_BIN}" \
  | tee "${OUT}/app.log"
"${ROOT}/util/tracer_nvbit/tracer_tool/traces-processing/post-traces-processing" \
  "${OUT}/traces"
popd >/dev/null

echo "[5/7] Generate backend maps"
python3 "${ROOT}/workloads/flashattention_v2/gen_backend_map.py" \
  --input "${META_FILE}" \
  --output "${MAP_FILE}"
grep -v '^weights_q ' "${META_FILE}" > "${META_FILE_MISSING}"
python3 "${ROOT}/workloads/flashattention_v2/gen_backend_map.py" \
  --input "${META_FILE_MISSING}" \
  --output "${MAP_FILE_MISSING}"
write_config "${CFG_FILE}" "${MAP_FILE}" "${TRACE_FILE}"
write_config "${CFG_FILE_MISSING}" "${MAP_FILE_MISSING}" "${OUT}/hbf_requests_missing_weights.trace"

prepare_accelsim_env

echo "[6/7] Run fail-fast check with missing weight mapping"
if [ "${RUN_FAILFAST_CHECK}" = "1" ]; then
  set +e
  "${ROOT}/gpu-simulator/bin/release/accel-sim.out" \
    -trace "${OUT}/traces/kernelslist.g" \
    -config "${BASE_GPGPUSIM_CONFIG}" \
    -config "${TRACE_CONFIG}" \
    -config "${CFG_FILE_MISSING}" \
    > "${OUT}/sim_missing_weights.log" 2>&1
  status=$?
  set -e
  if [ "${status}" -eq 0 ]; then
    echo "ERROR: missing-weight map unexpectedly succeeded" >&2
    exit 1
  fi
  grep -q "FATAL: off-chip mem_fetch without resolved backend" "${OUT}/sim_missing_weights.log"
fi

echo "[7/7] Run Accel-Sim with complete backend map"
"${ROOT}/gpu-simulator/bin/release/accel-sim.out" \
  -trace "${OUT}/traces/kernelslist.g" \
  -config "${BASE_GPGPUSIM_CONFIG}" \
  -config "${TRACE_CONFIG}" \
  -config "${CFG_FILE}" \
  | tee "${OUT}/sim.log"

echo "Trace written: ${TRACE_FILE}"
echo -n "MQSIM count: "
grep -c ',MQSIM,' "${TRACE_FILE}" || true
echo -n "RAMULATOR count: "
grep -c ',RAMULATOR,' "${TRACE_FILE}" || true

python3 "${ROOT}/workloads/flashattention_v2/annotate_backend_trace.py" \
  --meta "${META_FILE}" \
  --trace "${TRACE_FILE}" \
  --output "${ANNOTATED_TRACE_FILE}" \
  --layers "${MODEL_LAYERS}"

python3 - "${META_FILE}" "${ANNOTATED_TRACE_FILE}" "${MODEL_LAYERS}" <<'PY'
import csv
import collections
import pathlib
import sys

meta_path = pathlib.Path(sys.argv[1])
annotated_trace_path = pathlib.Path(sys.argv[2])
model_layers = int(sys.argv[3])

expected_regions = [
    "weights_q",
    "weights_k",
    "weights_v",
    "weights_o",
    "weights_mlp_up",
    "weights_mlp_gate",
    "weights_mlp_down",
    "kv_cache",
]

meta_regions = []
for line in meta_path.read_text(encoding="utf-8").splitlines():
    line = line.strip()
    if not line or line.startswith("#"):
        continue
    region, *_ = line.split()
    meta_regions.append(region)

if meta_regions != expected_regions:
    raise SystemExit(
        f"unexpected metadata regions: {meta_regions!r}, expected {expected_regions!r}"
    )

with annotated_trace_path.open("r", encoding="utf-8") as fp:
    rows = list(csv.DictReader(fp))

region_hits = collections.Counter(row["region"] for row in rows)
layer_hits = collections.Counter((row["region"], row["layer"]) for row in rows)
source_hits = collections.Counter(row["source_hint"] for row in rows)
backend_matches = collections.Counter(row["backend_match"] for row in rows)

print(f"backend_match_ones={backend_matches.get('1', 0)}")
print(f"backend_match_zeroes={backend_matches.get('0', 0)}")
if backend_matches.get("0", 0) != 0:
    raise SystemExit("found backend_match=0 rows in annotated trace")

for region in expected_regions:
    hits = region_hits.get(region, 0)
    print(f"{region}_hits={hits}")
    if hits == 0:
        raise SystemExit(f"expected at least one annotated trace hit for {region}")

for region in expected_regions:
    if region == "kv_cache":
        continue
    for layer in range(model_layers):
        layer = str(layer)
        hits = layer_hits.get((region, layer), 0)
        print(f"{region}_layer_{layer}_hits={hits}")
        if hits == 0:
            raise SystemExit(
                f"expected at least one annotated trace hit for {region} layer {layer}"
            )

print(f"kv_write_hits={source_hits.get('decoder_block_kernel:kv_write', 0)}")
print(f"kv_read_hits={source_hits.get('decoder_block_kernel:kv_read', 0)}")
if source_hits.get("decoder_block_kernel:kv_write", 0) == 0:
    raise SystemExit("expected at least one KV-cache write hit")
if source_hits.get("decoder_block_kernel:kv_read", 0) == 0:
    raise SystemExit("expected at least one KV-cache read hit")
PY

echo "Annotated trace written: ${ANNOTATED_TRACE_FILE}"
