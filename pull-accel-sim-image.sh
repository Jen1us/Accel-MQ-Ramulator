#!/usr/bin/env bash
set -euo pipefail

IMAGE_MIRROR="${IMAGE_MIRROR:-ghcr.nju.edu.cn/accel-sim/accel-sim-framework:ubuntu-24.04-cuda-12.8}"
IMAGE_CANONICAL="${IMAGE_CANONICAL:-ghcr.io/accel-sim/accel-sim-framework:ubuntu-24.04-cuda-12.8}"
OUT_TAR="${OUT_TAR:-./accel-sim.tar}"
SLEEP_SECONDS="${SLEEP_SECONDS:-30}"

log() { printf '[pull] %s\n' "$*"; }

log "start: $(date)"
log "mirror: ${IMAGE_MIRROR}"
log "canonical: ${IMAGE_CANONICAL}"
log "out: ${OUT_TAR}"
log "sleep: ${SLEEP_SECONDS}s"

attempt=1
while true; do
  log "attempt ${attempt}: $(date)"
  if docker pull "$IMAGE_MIRROR"; then
    log "pull succeeded: $(date)"
    break
  fi
  rc=$?
  log "pull failed (exit=${rc}), retry in ${SLEEP_SECONDS}s"
  attempt=$((attempt + 1))
  sleep "$SLEEP_SECONDS"
done

log "tagging canonical..."
docker tag "$IMAGE_MIRROR" "$IMAGE_CANONICAL" || true

log "saving tar..."
rm -f "$OUT_TAR" "$OUT_TAR.sha256"
docker save -o "$OUT_TAR" "$IMAGE_MIRROR"
sha256sum "$OUT_TAR" > "$OUT_TAR.sha256"
ls -lh "$OUT_TAR" "$OUT_TAR.sha256"

log "done: $(date)"
