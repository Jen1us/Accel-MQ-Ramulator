#!/usr/bin/env bash
set -euo pipefail

# 1. 环境路径定义 (保持与主脚本一致)
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
#OUT="${OUT_DIR:-${ROOT}/hw_run/traces_jenius/tiny_transformer_backend_map}"
OUT="${OUT_DIR:-${ROOT}/hw_run}"

# 1. 自动获取当前项目的绝对路径
REAL_ROOT=$(pwd)
# 设定你当前的输出目录为 hw_run
REAL_OUT="${REAL_ROOT}/hw_run"

# 2. 使用你的实际路径重写配置文件
cat > "${REAL_OUT}/backend_map.config" <<EOF
-hbm_use_ramulator2 1
-hbm_ramulator2_wrapper ${REAL_ROOT}/external_wrappers/ramulator2_wrap/build/libramulator2_wrap.so
-hbm_ramulator2_config ${REAL_ROOT}/workloads/ramulator2/hbm2_16ch.yaml
-hbf_use_mqsim 1
-hbf_mqsim_wrapper ${REAL_ROOT}/external_wrappers/mqsim_wrap/build/libmqsim_wrap.so
-hbf_mqsim_config ${REAL_ROOT}/workloads/mqsim/hbf_1p2TBs.xml
-hbf_subpage_bytes 512
-hbf_partition_count 0
-hbf_random_access 0
-hbf_request_trace 1
-hbf_request_trace_file ${REAL_OUT}/hbf_requests.trace
-hbf_request_trace_limit 0
-mem_backend_map_file ${REAL_OUT}/backend_map.txt
-mem_backend_map_default unspecified
EOF

echo "配置文件已修正，当前指向：${REAL_ROOT}"


# 配置文件与 Trace 路径
META_FILE="${OUT}/backend_meta.tsv"
CFG_FILE="${OUT}/backend_map.config"
TRACE_FILE="${OUT}/hbf_requests.trace"
ANNOTATED_TRACE_FILE="${OUT}/hbf_requests_annotated.csv"

# 模拟器配置 (SM86_RTX3070)
MODEL_LAYERS="${MODEL_LAYERS:-1}"
BASE_GPGPUSIM_CONFIG="${BASE_GPGPUSIM_CONFIG:-${ROOT}/gpu-simulator/gpgpu-sim/configs/tested-cfgs/SM86_RTX3070/gpgpusim.config}"
TRACE_CONFIG="${TRACE_CONFIG:-${ROOT}/gpu-simulator/configs/tested-cfgs/SM86_RTX3070/trace.config}"

# 2. 初始化环境
echo "[Step 0] Initializing environment..."
source "${HOME}/miniconda3/etc/profile.d/conda.sh"
set +u  # 临时关闭“未绑定变量检测”
conda activate accel-sim-nvcc
set -u  # 重新开启严格模式

prepare_accelsim_env() {
    # 显式指定你电脑上 CUDA 11.0 的实际安装路径
    export CUDA_INSTALL_PATH=/usr/local/cuda-11.0 
    export PATH=$CUDA_INSTALL_PATH/bin:$PATH
    
    set +u
    source "${ROOT}/gpu-simulator/setup_environment.sh" >/dev/null
    set -u
}
prepare_accelsim_env

# 3. 运行 Accel-Sim 仿真 (核心第 7 步)
echo "[Step 7] Running Accel-Sim with complete backend map..."
if [ ! -d "${OUT}/traces" ]; then
    echo "错误: 找不到 Trace 文件夹，请确保已从采集端拷贝数据到 ${OUT}/traces"
    exit 1
fi

"${ROOT}/gpu-simulator/bin/release/accel-sim.out" \
    -trace "${OUT}/traces/kernelslist.g" \
    -config "${BASE_GPGPUSIM_CONFIG}" \
    -config "${TRACE_CONFIG}" \
    -config "${CFG_FILE}" \
    | tee "${OUT}/sim.log"

# 4. 统计存储后端请求分布
echo "--------------------------------------"
echo "后端请求分布统计:"
echo -n "MQSIM (HBF) Count: "
grep -c ',MQSIM,' "${TRACE_FILE}" || true
echo -n "RAMULATOR (HBM) Count: "
grep -c ',RAMULATOR,' "${TRACE_FILE}" || true
echo "--------------------------------------"

# 5. 生成标注后的 Trace 报表
echo "[Post-Process] Annotating backend trace..."
python3 "${ROOT}/workloads/flashattention_v2/annotate_backend_trace.py" \
    --meta "${META_FILE}" \
    --trace "${TRACE_FILE}" \
    --output "${ANNOTATED_TRACE_FILE}" \
    --layers "${MODEL_LAYERS}"

# 6. 运行 Python 命中率分析脚本
echo "[Post-Process] Running hit analysis..."
python3 - "${META_FILE}" "${ANNOTATED_TRACE_FILE}" "${MODEL_LAYERS}" <<'PY'
import csv
import collections
import pathlib
import sys

# (此处保持原脚本中的 Python 逻辑不变，用于验证 KV Cache 和 Weights 的访问正确性)
# ... [省略重复的 Python 代码以保持简洁，使用时直接贴入原脚本末尾的 PY 部分即可] ...
PY

echo "分析完成！标注后的数据请查看: ${ANNOTATED_TRACE_FILE}"