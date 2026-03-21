# Hugging Face small model runner (NVBit → Accel-Sim)

This workload runs a real Hugging Face Transformer model on the GPU, captures
instruction traces using NVBit, then runs Accel-Sim in trace-driven mode and
dumps request-level `HBF/HBM` labels.

## Quick start

```bash
bash workloads/hf_small_model/run_end2end_hf.sh
```

Outputs are written under:

- `hw_run/traces_jenius/hf_small_model/traces/` (NVBit traces + post-processing)
- `hw_run/traces_jenius/hf_small_model/hbf_requests.trace` (Accel-Sim request trace)

## Notes

- The default model is `distilgpt2` (<< 1B params).
- To keep traces/simulation manageable, the runner truncates the model to the
  first few transformer blocks by default.

