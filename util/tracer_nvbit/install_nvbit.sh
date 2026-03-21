#!/bin/bash
set -euo pipefail

export BASH_ROOT="$( cd "$( dirname "$BASH_SOURCE" )" && pwd )"

NVBIT_VERSION="${NVBIT_VERSION:-1.7.6}"
NVBIT_ARCHIVE="nvbit-Linux-x86_64-${NVBIT_VERSION}.tar.bz2"
NVBIT_URL="https://github.com/NVlabs/NVBit/releases/download/v${NVBIT_VERSION}/${NVBIT_ARCHIVE}"

NVBIT_CACHE_DIR="${BASH_ROOT}/.cache"
NVBIT_ARCHIVE_PATH="${NVBIT_CACHE_DIR}/${NVBIT_ARCHIVE}"
NVBIT_INSTALL_DIR="${BASH_ROOT}/nvbit_release"

if [ -f "${NVBIT_INSTALL_DIR}/core/libnvbit.a" ]; then
  echo "NVBit already installed: ${NVBIT_INSTALL_DIR}"
  exit 0
fi

mkdir -p "${NVBIT_CACHE_DIR}"

# GitHub release assets download via a short-lived signed URL; with flaky
# networks the token can expire mid-download. We loop and resume (-C -) until it
# completes.
max_attempts="${NVBIT_MAX_DOWNLOAD_ATTEMPTS:-20}"
attempt=0
while true; do
  attempt=$((attempt + 1))
  if [ "${attempt}" -gt "${max_attempts}" ]; then
    echo "ERROR: failed to download NVBit after ${max_attempts} attempts" >&2
    exit 1
  fi

  echo "Downloading NVBit ${NVBIT_VERSION} (attempt ${attempt}/${max_attempts})..."
  if command -v curl >/dev/null 2>&1; then
    if curl -fL --retry 3 --retry-delay 3 --retry-all-errors -C - \
      -o "${NVBIT_ARCHIVE_PATH}" "${NVBIT_URL}"; then
      break
    fi
  else
    if wget -c --tries=3 --waitretry=3 --read-timeout=30 --timeout=30 \
      --retry-connrefused -O "${NVBIT_ARCHIVE_PATH}" "${NVBIT_URL}"; then
      break
    fi
  fi

  echo "Download failed; retrying in 5s..." >&2
  sleep 5
done

rm -rf "${NVBIT_INSTALL_DIR}"
mkdir -p "${NVBIT_INSTALL_DIR}"
tar -xf "${NVBIT_ARCHIVE_PATH}" -C "${NVBIT_INSTALL_DIR}" --strip-components=1
echo "NVBit installed: ${NVBIT_INSTALL_DIR}"
