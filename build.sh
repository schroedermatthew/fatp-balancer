#!/usr/bin/env bash
# BALANCER_META:
#   meta_version: 1
#   component: fatp-balancer
#   file_role: build_script
#   path: build.sh
#   layer: Testing
#   summary: Unix build script for fatp-balancer — configure, build, test.
#   api_stability: in_work

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
FATP_DIR="${FATP_INCLUDE_DIR:-${SCRIPT_DIR}/../FatP/include/fat_p}"

ASAN=OFF
TSAN=OFF
UBSAN=OFF
BUILD_TYPE=Debug

for arg in "$@"; do
    case "$arg" in
        --asan)   ASAN=ON ;;
        --tsan)   TSAN=ON ;;
        --ubsan)  UBSAN=ON ;;
        --release) BUILD_TYPE=Release ;;
        *) echo "Unknown option: $arg"; exit 1 ;;
    esac
done

echo "=== fatp-balancer build ==="
echo "  Build type : ${BUILD_TYPE}"
echo "  FAT-P dir  : ${FATP_DIR}"
echo "  ASan       : ${ASAN}"
echo "  TSan       : ${TSAN}"
echo "  UBSan      : ${UBSAN}"

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DFATP_INCLUDE_DIR="${FATP_DIR}" \
    -DBALANCER_ENABLE_ASAN="${ASAN}" \
    -DBALANCER_ENABLE_TSAN="${TSAN}" \
    -DBALANCER_ENABLE_UBSAN="${UBSAN}"

cmake --build "${BUILD_DIR}" --parallel "$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)"

echo ""
echo "=== Running tests ==="
cd "${BUILD_DIR}"
ctest --output-on-failure --parallel 4

echo ""
echo "=== All done ==="
