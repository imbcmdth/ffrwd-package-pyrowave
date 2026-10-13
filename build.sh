#!/bin/sh
# Builds the ffrwd/pyrowave modules to wasm32-wasip2 components.
#
#   WASI_SDK=<wasi-sdk 34+ install> PYROWAVE_DIR=<pyrowave checkout> sh build.sh [build-dir]
#
# PYROWAVE_DIR defaults to ../pyrowave, a checkout of the fork carrying the
# WebGPU port (https://github.com/imbcmdth/pyrowave, branch gpu-textures, which
# adds decoding into textures to branch webgpu; this was built against
# f856bf6). The modules are copied to modules/ when done.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
build=${1:-$here/build}
: "${WASI_SDK:?set WASI_SDK to a wasi-sdk install}"
pyrowave=${PYROWAVE_DIR:-$here/../pyrowave}

if command -v ninja >/dev/null 2>&1; then
	generator=Ninja
elif command -v mingw32-make >/dev/null 2>&1; then
	generator="MinGW Makefiles"
else
	generator="Unix Makefiles"
fi

cmake -S "$here" -B "$build" -G "$generator" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_TOOLCHAIN_FILE="$WASI_SDK/share/cmake/wasi-sdk-p2.cmake" \
	-DWASI_SDK_PREFIX="$WASI_SDK" \
	-DPYROWAVE_DIR="$pyrowave"
cmake --build "$build" --parallel

mkdir -p "$here/modules"
for module in module/pyrowave_roundtrip.wasm codec/pyrowave.wasm codec/pyrowave_gpu.wasm; do
	cp "$build/$module" "$here/modules/$(basename "$module")"
	echo "built $here/modules/$(basename "$module")"
done
