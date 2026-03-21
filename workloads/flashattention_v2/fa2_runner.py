#!/usr/bin/env python3

import argparse
import math
import os
import sys
import time
from typing import Optional

TORCH_IMPORT_ERROR: Optional[BaseException] = None
FLASH_ATTN_IMPORT_ERROR: Optional[BaseException] = None

try:
    import torch
except BaseException as exc:
    torch = None  # type: ignore[assignment]
    TORCH_IMPORT_ERROR = exc

try:
    # Most common v2 install path
    from flash_attn.flash_attn_interface import flash_attn_func  # type: ignore
except BaseException as exc:
    flash_attn_func = None  # type: ignore[assignment]
    FLASH_ATTN_IMPORT_ERROR = exc


def _parse_dtype(name: str):
    assert torch is not None
    name = name.lower()
    if name in ("fp16", "float16", "half"):
        return torch.float16
    if name in ("bf16", "bfloat16"):
        return torch.bfloat16
    if name in ("fp32", "float32"):
        return torch.float32
    raise ValueError(f"Unsupported dtype: {name}")


def _run_once(
    *,
    impl: str,
    q,
    k,
    v,
    causal: bool,
    dropout_p: float,
    softmax_scale: Optional[float],
    do_backward: bool,
    dout,
):
    assert torch is not None

    if impl == "flash-attn":
        if flash_attn_func is None:
            raise RuntimeError(
                "flash-attn is not installed (or failed to import).\n"
                f"Import error: {FLASH_ATTN_IMPORT_ERROR}\n\n"
                "Install hint (inside the container/venv):\n"
                "  pip install --upgrade pip\n"
                "  pip install torch --index-url https://download.pytorch.org/whl/cu121\n"
                "  pip install flash-attn --no-build-isolation\n"
            )
        out = flash_attn_func(
            q,
            k,
            v,
            dropout_p=dropout_p,
            causal=causal,
            softmax_scale=softmax_scale,
        )
    elif impl == "torch-sdpa":
        # PyTorch SDPA expects (B, H, S, D)
        q_ = q.transpose(1, 2)
        k_ = k.transpose(1, 2)
        v_ = v.transpose(1, 2)
        out = torch.nn.functional.scaled_dot_product_attention(
            q_, k_, v_, attn_mask=None, dropout_p=dropout_p, is_causal=causal
        ).transpose(1, 2)
    else:
        raise ValueError(f"Unknown impl: {impl}")

    if do_backward:
        if dout is None:
            raise ValueError("dout must be provided when do_backward=True")

        out.backward(dout)

        # Clear grads to keep later iterations consistent.
        q.grad = None
        k.grad = None
        v.grad = None

    return out


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Minimal FlashAttention v2 runner for NVBit tracing"
    )
    parser.add_argument(
        "--impl",
        default="flash-attn",
        choices=("flash-attn", "torch-sdpa"),
        help="Attention implementation to run",
    )
    # Keep defaults intentionally tiny: FlashAttention traces grow extremely
    # fast, and the goal here is to make an end-to-end NVBit→trace→Accel-Sim
    # pipeline that doesn't get OOM-killed.
    parser.add_argument("--batch", type=int, default=1)
    parser.add_argument("--seqlen", type=int, default=16)
    parser.add_argument("--nheads", type=int, default=1)
    parser.add_argument("--headdim", type=int, default=32)
    parser.add_argument(
        "--dtype", default="fp16", choices=("fp16", "bf16", "fp32"), help="Tensor dtype"
    )
    parser.add_argument("--causal", action="store_true", help="Use causal masking")
    parser.add_argument("--dropout", type=float, default=0.0)
    parser.add_argument(
        "--softmax-scale",
        type=float,
        default=None,
        help="Override softmax scale (default: 1/sqrt(headdim))",
    )
    parser.add_argument("--warmup", type=int, default=0)
    parser.add_argument("--iters", type=int, default=1)
    parser.add_argument(
        "--backward", action="store_true", help="Also run backward pass"
    )
    parser.add_argument("--seed", type=int, default=1234)
    parser.add_argument(
        "--device",
        default="cuda:0",
        help="Torch device (must be CUDA for NVBit tracing)",
    )
    parser.add_argument(
        "--roi",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Wrap measured iters with cudaProfilerStart/Stop (recommended for NVBit)",
    )
    args = parser.parse_args()

    if torch is None:
        print(
            "ERROR: PyTorch is not installed (or failed to import).\n"
            f"Import error: {TORCH_IMPORT_ERROR}\n\n"
            "Install hint (inside the container/venv):\n"
            "  pip install --upgrade pip\n"
            "  pip install torch --index-url https://download.pytorch.org/whl/cu121\n",
            file=sys.stderr,
        )
        return 2

    if not torch.cuda.is_available() or not str(args.device).startswith("cuda"):
        print(
            "ERROR: CUDA device is required for this runner (NVBit traces CUDA kernels).",
            file=sys.stderr,
        )
        return 2

    torch.manual_seed(args.seed)
    torch.cuda.manual_seed_all(args.seed)

    device = torch.device(args.device)
    dtype = _parse_dtype(args.dtype)

    b, s, h, d = args.batch, args.seqlen, args.nheads, args.headdim
    if args.softmax_scale is None:
        softmax_scale = 1.0 / math.sqrt(d)
    else:
        softmax_scale = float(args.softmax_scale)

    # Use separate Q/K/V layout; flash-attn supports (B, S, H, D).
    q = torch.randn((b, s, h, d), device=device, dtype=dtype, requires_grad=args.backward)
    k = torch.randn((b, s, h, d), device=device, dtype=dtype, requires_grad=args.backward)
    v = torch.randn((b, s, h, d), device=device, dtype=dtype, requires_grad=args.backward)

    # Pre-allocate a fixed gradient for the output to avoid tracing extra
    # reduction kernels (e.g., sum()) when running backward.
    dout = None
    if args.backward:
        dout = torch.ones((b, s, h, d), device=device, dtype=dtype)

    print(
        f"impl={args.impl} shape=(B={b}, S={s}, H={h}, D={d}) dtype={dtype} "
        f"causal={args.causal} dropout={args.dropout} backward={args.backward}"
    )
    device_index = 0 if device.index is None else int(device.index)
    print(f"device={torch.cuda.get_device_name(device_index)}")

    # Warmup outside ROI to avoid tracing initialization noise.
    for _ in range(max(args.warmup, 0)):
        _run_once(
            impl=args.impl,
            q=q,
            k=k,
            v=v,
            causal=args.causal,
            dropout_p=args.dropout,
            softmax_scale=softmax_scale,
            do_backward=args.backward,
            dout=dout,
        )
        torch.cuda.synchronize()

    torch.cuda.synchronize()
    if args.roi:
        torch.cuda.profiler.start()

    start = time.time()
    out = None
    for _ in range(max(args.iters, 1)):
        out = _run_once(
            impl=args.impl,
            q=q,
            k=k,
            v=v,
            causal=args.causal,
            dropout_p=args.dropout,
            softmax_scale=softmax_scale,
            do_backward=args.backward,
            dout=dout,
        )
        torch.cuda.synchronize()
    elapsed_s = time.time() - start

    if args.roi:
        torch.cuda.profiler.stop()
        torch.cuda.synchronize()

    if out is not None:
        checksum = float(out.detach().float().sum().cpu().item())
        print(f"checksum={checksum}")
    print(f"elapsed_s={elapsed_s}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
