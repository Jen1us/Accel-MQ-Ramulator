#!/usr/bin/env python3

import argparse
import csv
import pathlib
import sys


SOURCE_HINTS = {
    "weights_q": "decoder_block_kernel:q_proj",
    "weights_k": "decoder_block_kernel:k_proj",
    "weights_v": "decoder_block_kernel:v_proj",
    "weights_o": "decoder_block_kernel:o_proj",
    "weights_mlp_up": "decoder_block_kernel:mlp_up",
    "weights_mlp_gate": "decoder_block_kernel:mlp_gate",
    "weights_mlp_down": "decoder_block_kernel:mlp_down",
}

LAYERED_REGIONS = set(SOURCE_HINTS) | {"kv_cache"}


def resolve_source_hint(region_name: str, row):
    if region_name == "kv_cache":
        return (
            "decoder_block_kernel:kv_write"
            if row.get("is_write") == "1"
            else "decoder_block_kernel:kv_read"
        )
    return SOURCE_HINTS.get(region_name, "unknown")


def load_meta(path: pathlib.Path):
    regions = []
    for line_no, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) < 4:
            raise ValueError(
                f"line {line_no}: expected 'region base size backend [label]', got: {line}"
            )
        region = parts[0]
        base = int(parts[1], 0)
        size = int(parts[2], 0)
        backend = parts[3].upper()
        label = " ".join(parts[4:]) if len(parts) > 4 else region
        regions.append(
            {
                "region": region,
                "base": base,
                "end": base + size,
                "size": size,
                "backend": backend,
                "label": label,
            }
        )
    return regions


def annotate_row(row, regions, layers):
    addr = int(row["addr"], 0)
    for region in regions:
        if region["base"] <= addr < region["end"]:
            offset = addr - region["base"]
            layer = ""
            if region["region"] in LAYERED_REGIONS:
                if layers <= 0:
                    raise ValueError("--layers must be > 0")
                chunk = region["size"] // layers
                if chunk == 0:
                    raise ValueError(
                        f"region '{region['region']}' too small for {layers} layers"
                    )
                layer = str(min(offset // chunk, layers - 1))

            annotated = dict(row)
            annotated["region"] = region["region"]
            annotated["label"] = region["label"]
            annotated["layer"] = layer
            annotated["region_offset"] = hex(offset)
            annotated["source_hint"] = resolve_source_hint(region["region"], row)
            annotated["expected_backend"] = region["backend"]
            annotated["backend_match"] = "1" if row["backend"].upper() == region["backend"] else "0"
            return annotated

    raise ValueError(f"unmapped trace row addr={row['addr']} uid={row['uid']}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Annotate backend trace rows")
    parser.add_argument("--meta", required=True, help="Path to backend_meta.tsv")
    parser.add_argument("--trace", required=True, help="Path to hbf_requests.trace")
    parser.add_argument("--output", required=True, help="Output CSV path")
    parser.add_argument("--layers", type=int, default=1, help="Number of model layers")
    args = parser.parse_args()

    try:
        regions = load_meta(pathlib.Path(args.meta))
        with pathlib.Path(args.trace).open("r", encoding="utf-8") as fp:
            rows = list(csv.DictReader(fp))

        annotated_rows = [annotate_row(row, regions, args.layers) for row in rows]
        fieldnames = list(rows[0].keys()) + [
            "region",
            "label",
            "layer",
            "region_offset",
            "source_hint",
            "expected_backend",
            "backend_match",
        ]
        with pathlib.Path(args.output).open("w", encoding="utf-8", newline="") as fp:
            writer = csv.DictWriter(fp, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(annotated_rows)
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
