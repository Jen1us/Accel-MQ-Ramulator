#!/usr/bin/env python3

import argparse
import pathlib
import sys


VALID_BACKENDS = {"ramulator", "mqsim", "unspecified"}


def parse_row(line: str, line_no: int):
    parts = line.split()
    if len(parts) < 4:
        raise ValueError(
            f"line {line_no}: expected 'region base size backend [label]', got: {line}"
        )

    region = parts[0]
    base = int(parts[1], 0)
    size = int(parts[2], 0)
    backend = parts[3].lower()
    label = " ".join(parts[4:]) if len(parts) > 4 else region

    if size <= 0:
        raise ValueError(f"line {line_no}: size must be > 0")
    if backend not in VALID_BACKENDS:
        raise ValueError(
            f"line {line_no}: backend must be one of {sorted(VALID_BACKENDS)}, got: {backend}"
        )

    return {
        "region": region,
        "base": base,
        "size": size,
        "backend": backend,
        "label": label,
    }


def load_rows(path: pathlib.Path):
    rows = []
    for line_no, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        rows.append(parse_row(line, line_no))
    return rows


def format_backend_map(rows):
    output = []
    for row in rows:
        output.append(
            f"{hex(row['base'])} {hex(row['size'])} {row['backend']} {row['label']}"
        )
    return "\n".join(output) + ("\n" if output else "")


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate mem_backend_map file")
    parser.add_argument("--input", required=True, help="Metadata input path")
    parser.add_argument("--output", required=True, help="Backend map output path")
    args = parser.parse_args()

    try:
        rows = load_rows(pathlib.Path(args.input))
        text = format_backend_map(rows)
        pathlib.Path(args.output).write_text(text, encoding="utf-8")
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
