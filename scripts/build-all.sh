#!/usr/bin/env bash
# Configure and build the XPMultiCrew X-Plane plugin on Linux/macOS.
#
# Usage:
#   ./scripts/build-all.sh [/path/to/extracted/XPSDK]
#
# If no SDK path is given, the script expects the SDK to already be
# extracted at third_party/XPSDK (see plugin/CMakeLists.txt).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
XPSDK_ROOT="${1:-${ROOT_DIR}/third_party/XPSDK}"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DXPSDK_ROOT="${XPSDK_ROOT}"

cmake --build "${BUILD_DIR}" --config RelWithDebInfo -j

echo "Built plugin: ${BUILD_DIR}/XPMultiCrew"
