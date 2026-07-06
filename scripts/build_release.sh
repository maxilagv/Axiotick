#!/bin/bash
# Phase 20: Release Build Script

echo "[Argentum-FX] Starting Release Build..."

# 1. Clean build directory
rm -rf build
mkdir build
cd build

# 2. Configure with Release optimizations
cmake -DCMAKE_BUILD_TYPE=Release ..

# 3. Build with all cores
cmake --build . --config Release -- -j$(nproc)

echo "[Argentum-FX] Build Complete."
echo "[Argentum-FX] Artifacts located in build/bin/"
