#!/usr/bin/env python3

import argparse
import os
import time


def _parse_dtype(name: str):
    import torch

    name = name.lower()
    if name in ("fp16", "float16", "half"):
        return torch.float16
    if name in ("bf16", "bfloat16"):
        return torch.bfloat16
    if name in ("fp32", "float32"):
        return torch.float32
    raise ValueError(f"Unsupported dtype: {name}")


def _truncate_gpt2_like(model, keep_layers: int) -> bool:
    import torch

    if keep_layers <= 0:
        return False

    transformer = getattr(model, "transformer", None)
    if transformer is None:
        return False
    blocks = getattr(transformer, "h", None)
    if blocks is None:
        return False

    keep_layers = min(int(keep_layers), len(blocks))
    transformer.h = torch.nn.ModuleList(list(blocks)[:keep_layers])

    if hasattr(model, "config"):
        if hasattr(model.config, "n_layer"):
            model.config.n_layer = keep_layers
        if hasattr(model.config, "num_layers"):
            model.config.num_layers = keep_layers
    if hasattr(transformer, "config"):
        if hasattr(transformer.config, "n_layer"):
            transformer.config.n_layer = keep_layers
        if hasattr(transformer.config, "num_layers"):
            transformer.config.num_layers = keep_layers
    return True


def _run_gpu_probe(device, dtype, elements: int, roi: bool) -> None:
    import torch

    elements = int(elements)
    if elements <= 0:
        raise ValueError("--probe-elements must be > 0")

    probe = torch.empty((elements,), device=device, dtype=dtype)
    torch.cuda.synchronize()

    if roi:
        torch.cuda.profiler.start()
    probe.add_(1)
    torch.cuda.synchronize()
    if roi:
        torch.cuda.profiler.stop()
        torch.cuda.synchronize()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run a real Hugging Face model on CUDA (for NVBit tracing)"
    )
    parser.add_argument("--model", default="distilgpt2", help="HF model id")
    parser.add_argument("--dtype", default="fp16", choices=("fp16", "bf16", "fp32"))
    parser.add_argument("--device", default="cuda:0")
    parser.add_argument("--seqlen", type=int, default=16)
    parser.add_argument(
        "--keep-layers",
        type=int,
        default=2,
        help="Keep only the first N transformer blocks to reduce trace size",
    )
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--iters", type=int, default=1)
    parser.add_argument(
        "--prompt",
        default="Hello from Accel-Sim!",
        help="Prompt text (will be repeated/padded to seqlen)",
    )
    parser.add_argument(
        "--random-input",
        action="store_true",
        help="Use random token ids instead of tokenizing the prompt",
    )
    parser.add_argument(
        "--roi",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Wrap measured iters with cudaProfilerStart/Stop (recommended for NVBit)",
    )
    parser.add_argument(
        "--probe-elements",
        type=int,
        default=0,
        help="If >0, run a small GPU memory probe (torch add_) of this many elements",
    )
    parser.add_argument(
        "--probe-only",
        action="store_true",
        help="Only run the GPU probe (still loads the HF model on CPU)",
    )
    parser.add_argument(
        "--local-files-only",
        action="store_true",
        help="Require model/tokenizer to be available in the local HF cache",
    )
    args = parser.parse_args()

    os.environ.setdefault("TOKENIZERS_PARALLELISM", "false")

    import torch
    from transformers import AutoModelForCausalLM, AutoTokenizer

    if not torch.cuda.is_available() or not str(args.device).startswith("cuda"):
        raise SystemExit("ERROR: CUDA device is required (NVBit traces CUDA kernels).")

    torch.set_grad_enabled(False)

    device = torch.device(args.device)
    dtype = _parse_dtype(args.dtype)

    tokenizer = AutoTokenizer.from_pretrained(
        args.model, local_files_only=bool(args.local_files_only)
    )
    model = AutoModelForCausalLM.from_pretrained(
        args.model, local_files_only=bool(args.local_files_only)
    )
    model.eval()

    truncated = _truncate_gpt2_like(model, args.keep_layers)
    if truncated:
        print(f"truncated_layers={args.keep_layers}")
    else:
        print("truncated_layers=0 (model type not recognized; using full model)")

    if args.probe_only:
        elems = int(args.probe_elements) if int(args.probe_elements) > 0 else 4096
        _run_gpu_probe(device=device, dtype=dtype, elements=elems, roi=bool(args.roi))
        print(f"probe_elements={elems}")
        return 0

    model.to(device=device, dtype=dtype)

    if args.random_input:
        vocab = int(getattr(model.config, "vocab_size", 50257))
        input_ids = torch.randint(
            0, vocab, (1, int(args.seqlen)), device=device, dtype=torch.long
        )
    else:
        enc = tokenizer(args.prompt, return_tensors="pt")
        input_ids = enc["input_ids"].to(device)
        if input_ids.shape[1] < int(args.seqlen):
            reps = (int(args.seqlen) + input_ids.shape[1] - 1) // input_ids.shape[1]
            input_ids = input_ids.repeat(1, reps)
        input_ids = input_ids[:, : int(args.seqlen)]

    with torch.no_grad():
        for _ in range(max(int(args.warmup), 0)):
            _ = model(input_ids, use_cache=False)
        torch.cuda.synchronize()

        if args.roi:
            torch.cuda.profiler.start()
        start = time.time()
        out = None
        for _ in range(max(int(args.iters), 1)):
            out = model(input_ids, use_cache=False)
        torch.cuda.synchronize()
        elapsed_s = time.time() - start
        if args.roi:
            torch.cuda.profiler.stop()
            torch.cuda.synchronize()

    checksum = 0.0
    if out is not None and hasattr(out, "logits"):
        checksum = float(out.logits.float().sum().cpu().item())
        print(f"logits_shape={tuple(out.logits.shape)}")
    print(f"checksum={checksum}")
    print(f"elapsed_s={elapsed_s}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
