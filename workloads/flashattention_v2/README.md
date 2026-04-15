# FlashAttention v2 runner (for NVBit tracing)

This folder contains a minimal Python runner intended to be traced with
`util/tracer_nvbit/tracer_tool/tracer_tool.so`.

## Quick start (inside the docker image)

Enter the Accel-Sim docker image with GPU:

```bash
docker run --rm --gpus all -it -v "$PWD":/accel-sim -w /accel-sim \
  ghcr.io/accel-sim/accel-sim-framework:ubuntu-24.04-cuda-12.8 bash
```

Build NVBit + tracer:

```bash
cd util/tracer_nvbit
./install_nvbit.sh
make -j"$(nproc)"
```

Run the workload under NVBit (ROI tracing recommended):

```bash
mkdir -p /accel-sim/hw_run/traces/fa2_sm89 && cd /accel-sim/hw_run/traces/fa2_sm89
export TRACES_FOLDER=$PWD
export CUDA_VISIBLE_DEVICES=0
export ACTIVE_FROM_START=0

CUDA_INJECTION64_PATH=/accel-sim/util/tracer_nvbit/tracer_tool/tracer_tool.so \
  python /accel-sim/workloads/flashattention_v2/fa2_runner.py --roi --iters 1 --warmup 0

/accel-sim/util/tracer_nvbit/tracer_tool/traces-processing/post-traces-processing "$TRACES_FOLDER/traces"
```

Then simulate with Accel-Sim (example config; use your desired config):

```bash
source /accel-sim/gpu-simulator/setup_environment.sh
/accel-sim/gpu-simulator/bin/release/accel-sim.out \
  -trace "$TRACES_FOLDER/traces/kernelslist.g" \
  -config /accel-sim/gpu-simulator/gpgpu-sim/configs/tested-cfgs/SM86_RTX3070/gpgpusim.config \
  -config /accel-sim/gpu-simulator/configs/tested-cfgs/SM86_RTX3070/trace.config
```

## Notes

- If `flash-attn` is not installed, run `fa2_runner.py --impl torch-sdpa` to
  still generate traces (not FlashAttention).
- For SM89 (Ada/Lovelace), Accel-Sim trace-driven support is currently a
  temporary compatibility path (opcode mapping to Ampere + unknown opcodes fall
  back to NOP). Accuracy will be improved when full SM89 opcode/config support
  is added.

## Tiny transformer backend-map validation

Use the dedicated tiny transformer backend-map flow when you want to validate
semantic VA routing instead of random HBF/HBM tagging:

```bash
bash workloads/flashattention_v2/run_end2end_backend_map.sh
```

This flow:

- builds a tiny decoder-block CUDA app with explicit weight and `kv_cache`
  allocations,
- exports actual traced VAs to `backend_meta.tsv`,
- generates `backend_map.txt`,
- checks that an incomplete map fails fast,
- runs Accel-Sim with the complete map and emits `hbf_requests.trace`,
- annotates the request trace into `hbf_requests_annotated.csv` with
  `region`, `layer`, and `source_hint`.

The semantic regions are:

- `weights_q`
- `weights_k`
- `weights_v`
- `weights_o`
- `weights_mlp_up`
- `weights_mlp_gate`
- `weights_mlp_down`
- `kv_cache`

All weight regions are expected to route to `MQSIM`. `kv_cache` reads and
writes are expected to route to `RAMULATOR`. No activation/scratch/output
buffer is assigned a backend in this validation path.

Outputs land under `hw_run/traces_jenius/tiny_transformer_backend_map/`.
