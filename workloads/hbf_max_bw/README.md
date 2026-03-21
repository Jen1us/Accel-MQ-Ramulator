# HBF max-bandwidth microbenchmark (NVBit -> Accel-Sim)

This workload exists to generate **lots of off-chip traffic** with a **small NVBit trace**,
so we can validate HBF link bandwidth / queueing behavior.

## Run end-to-end

```bash
workloads/hbf_max_bw/run_end2end.sh
```

Useful overrides:

```bash
# Use QV100 as the simulated GPU (recommended for the 1.2TB/s-per-stack overlay)
BASE_GPGPUSIM_CONFIG=gpu-simulator/gpgpu-sim/configs/tested-cfgs/SM7_QV100/gpgpusim.config \
TRACE_CONFIG=gpu-simulator/configs/tested-cfgs/SM7_QV100/trace.config \
HBF_CONFIG=workloads/hbf_max_bw/hbf_qv100_1p2TBs.config \
CUDA_ARCH=sm_89 \
OUT_DIR=hw_run/traces/hbf_max_bw \
workloads/hbf_max_bw/run_end2end.sh
```

Program knobs (passed to the CUDA binary) are in `run_end2end.sh` as `APP_ARGS`
and can be adjusted to trade off trace size vs. traffic.

