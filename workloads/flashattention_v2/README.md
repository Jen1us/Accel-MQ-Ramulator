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
