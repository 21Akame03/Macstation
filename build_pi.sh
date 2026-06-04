#!/usr/bin/env bash
# Configure and compile only the Raspberry Pi target (pipette_pi).
# Run this on the Pi (or any non-macOS host) — the macOS host target is skipped.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build-pi"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" --target pipette_pi -j"$(nproc 2>/dev/null || echo 4)"

echo "Built: ${BUILD_DIR}/pi/pipette_pi"
