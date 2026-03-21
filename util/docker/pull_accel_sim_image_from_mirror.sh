#!/usr/bin/env bash
set -euo pipefail

REGISTRY="${REGISTRY:-ghcr.nju.edu.cn}"
REPO="${REPO:-accel-sim/accel-sim-framework}"
TAG="${TAG:-ubuntu-24.04-cuda-12.8}"

# The tag you want to end up with after `docker load`.
REF="${REF:-ghcr.io/${REPO}:${TAG}}"

CACHE_DIR="${CACHE_DIR:-.cache/accel-sim-image}"
OUT_TAR="${OUT_TAR:-accel-sim.tar}"

ACCEPT_MANIFEST_V2='application/vnd.docker.distribution.manifest.v2+json'

layout_dir="${CACHE_DIR}/layout"
manifest_path="${CACHE_DIR}/registry-manifest.json"

mkdir -p "${layout_dir}/blobs/sha256"

echo "[1/4] Fetching registry manifest..."
curl -fsSL --retry 20 --retry-all-errors \
  -H "Accept: ${ACCEPT_MANIFEST_V2}" \
  "https://${REGISTRY}/v2/${REPO}/manifests/${TAG}" \
  -o "${manifest_path}"

echo "[2/4] Downloading config + layers (resume supported)..."
python3 - <<'PY' "${manifest_path}" "${REGISTRY}" "${REPO}" "${layout_dir}"
import json
import os
import subprocess
import sys
from pathlib import Path

manifest_path, registry, repo, layout_dir = sys.argv[1:5]
layout_dir = Path(layout_dir)
blob_dir = layout_dir / "blobs" / "sha256"
blob_dir.mkdir(parents=True, exist_ok=True)

manifest = json.loads(Path(manifest_path).read_text())

items = [("config", manifest["config"])] + [("layer", l) for l in manifest["layers"]]
total = manifest["config"]["size"] + sum(l["size"] for l in manifest["layers"])
print(f"  - blobs: {len(items)}")
print(f"  - total: {total/1024/1024/1024:.2f} GiB")

def download_blob(digest: str, size: int) -> None:
    if not digest.startswith("sha256:"):
        raise SystemExit(f"Unsupported digest: {digest}")
    hex_digest = digest.split(":", 1)[1]
    dest = blob_dir / hex_digest

    # Fast path: already downloaded and size matches.
    if dest.exists():
        current = dest.stat().st_size
        if current == size:
            print(f"  [skip] {digest} ({size} bytes)")
            return
        if current > size:
            print(f"  [redo] {digest} local size {current} > expected {size}, deleting")
            dest.unlink()

    url = f"https://{registry}/v2/{repo}/blobs/{digest}"

    print(f"  [get ] {digest} -> {dest} ({size} bytes)")
    cmd = [
        "curl",
        "-fL",
        "--retry",
        "50",
        "--retry-all-errors",
        "--retry-delay",
        "2",
        "-C",
        "-",
        "-o",
        str(dest),
        url,
    ]
    subprocess.run(cmd, check=True)

    got = dest.stat().st_size
    if got != size:
        raise SystemExit(f"Downloaded size mismatch for {digest}: got {got}, expected {size}")

for kind, entry in items:
    download_blob(entry["digest"], int(entry["size"]))
PY

echo "[3/4] Writing OCI index/manifest + docker load metadata..."
python3 - <<'PY' "${manifest_path}" "${REF}" "${layout_dir}"
import hashlib
import json
import sys
from pathlib import Path

manifest_path, ref, layout_dir = sys.argv[1:4]
layout_dir = Path(layout_dir)
blob_dir = layout_dir / "blobs" / "sha256"

reg_manifest = json.loads(Path(manifest_path).read_text())
config = reg_manifest["config"]
layers = reg_manifest["layers"]

oci_manifest = {
    "schemaVersion": 2,
    "mediaType": "application/vnd.oci.image.manifest.v1+json",
    "config": {
        "mediaType": "application/vnd.oci.image.config.v1+json",
        "digest": config["digest"],
        "size": int(config["size"]),
    },
    "layers": [
        {
            "mediaType": "application/vnd.oci.image.layer.v1.tar+gzip",
            "digest": l["digest"],
            "size": int(l["size"]),
        }
        for l in layers
    ],
}

oci_manifest_bytes = json.dumps(oci_manifest, separators=(",", ":"), sort_keys=True).encode("utf-8")
oci_manifest_digest = hashlib.sha256(oci_manifest_bytes).hexdigest()
oci_manifest_size = len(oci_manifest_bytes)
(blob_dir / oci_manifest_digest).write_bytes(oci_manifest_bytes)

index = {
    "schemaVersion": 2,
    "mediaType": "application/vnd.oci.image.index.v1+json",
    "manifests": [
        {
            "mediaType": "application/vnd.oci.image.manifest.v1+json",
            "digest": f"sha256:{oci_manifest_digest}",
            "size": oci_manifest_size,
            "annotations": {
                "io.containerd.image.name": ref,
                "org.opencontainers.image.ref.name": ref.split(":", 1)[1] if ":" in ref else "latest",
            },
        }
    ],
}
layout_dir.joinpath("index.json").write_text(json.dumps(index, separators=(",", ":")) + "\n")
layout_dir.joinpath("oci-layout").write_text('{"imageLayoutVersion":"1.0.0"}\n')

layers_paths = [f"blobs/sha256/{l['digest'].split(':',1)[1]}" for l in layers]
config_path = f"blobs/sha256/{config['digest'].split(':',1)[1]}"

docker_manifest = [
    {
        "Config": config_path,
        "RepoTags": [ref],
        "Layers": layers_paths,
        "LayerSources": {
            l["digest"]: {
                "mediaType": "application/vnd.oci.image.layer.v1.tar+gzip",
                "size": int(l["size"]),
                "digest": l["digest"],
            }
            for l in layers
        },
    }
]
layout_dir.joinpath("manifest.json").write_text(json.dumps(docker_manifest, separators=(",", ":")) + "\n")

# `repositories` is legacy metadata; keep it for compatibility.
repo_name, tag = (ref.rsplit(":", 1) + ["latest"])[:2]
top_layer_hex = layers[-1]["digest"].split(":", 1)[1] if layers else config["digest"].split(":", 1)[1]
layout_dir.joinpath("repositories").write_text(json.dumps({repo_name: {tag: top_layer_hex}}, separators=(",", ":")) + "\n")
PY

echo "[4/4] Creating ${OUT_TAR} ..."
tar -C "${layout_dir}" -cf "${OUT_TAR}" .
ls -lh "${OUT_TAR}"

echo "Done."
echo "Next: docker load -i ${OUT_TAR}"
