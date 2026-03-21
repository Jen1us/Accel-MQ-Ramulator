# Minimal HBF driver (tiny NVBit trace)

FlashAttention kernels can generate very large traces even for small shapes.
If you only need an application-driven end-to-end smoke test (NVBit → traces →
Accel-Sim) to validate that your HBF plumbing works, use this tiny CUDA app.

## Run end-to-end (inside docker, with GPU)

```bash
docker run --rm --gpus all -it -v "$PWD":/accel-sim -w /accel-sim \
  ghcr.io/accel-sim/accel-sim-framework:ubuntu-24.04-cuda-12.8 bash

bash /accel-sim/workloads/minimal_hbf_driver/run_end2end.sh
```

Outputs land in `hw_run/traces/minimal_hbf_driver/` (ignored by git):

- `app.log` — stdout of the CUDA app under NVBit
- `traces/` — raw `.trace(.xz)` + processed `.traceg(.xz)` and `kernelslist.g`
- `sim.log` — Accel-Sim run output
- `hbf_requests.trace` — request-level HBF/HBM tag trace (simulator output)

## HBF knobs

Edit `workloads/minimal_hbf_driver/hbf.config`:

- `-hbf_partition_start` / `-hbf_partition_count`
- `-hbf_random_access` / `-hbf_random_access_percent` / `-hbf_random_access_seed`
- `-hbf_dram_latency` / `-hbf_l2_rop_latency` (0 = inherit)

Tip: enable `-hbf_random_access 1` to get a tiny mixed HBF/HBM run without
needing a real address partitioning rule yet.
