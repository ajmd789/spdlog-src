#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
mkdir -p "${BUILD_DIR}"
find "${BUILD_DIR}/_deps" -maxdepth 2 -type f -path "*-subbuild/CMakeCache.txt" -delete 2>/dev/null || true
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" --config Release
