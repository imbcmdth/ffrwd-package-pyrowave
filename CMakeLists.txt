# ffrwd/pyrowave: PyroWave's WebGPU port as ffrwd sidecar modules.
#
# Built for wasm32-wasip2 with wasi-sdk (34 or newer), as a component:
#
#   cmake -S . -B build -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=$WASI_SDK/share/cmake/wasi-sdk-p2.cmake \
#     -DWASI_SDK_PREFIX=$WASI_SDK -DPYROWAVE_DIR=../pyrowave
#   cmake --build build
#
# build.sh does the same. The module lands in build/pyrowave_roundtrip.wasm.

cmake_minimum_required(VERSION 3.20)
project(ffrwd-pyrowave LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
	message(FATAL_ERROR "Build with wasi-sdk's toolchain file (wasi-sdk-p2.cmake): the modules are wasm32-wasip2 components.")
endif()

# The PyroWave fork carrying the WebGPU port (branch webgpu). Only its
# webgpu/ directory and the Granite-free bitstream code it compiles are used.
set(PYROWAVE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pyrowave" CACHE PATH "PyroWave checkout with the webgpu/ port")
if (NOT EXISTS "${PYROWAVE_DIR}/webgpu/CMakeLists.txt")
	message(FATAL_ERROR "No PyroWave WebGPU port at ${PYROWAVE_DIR}; set PYROWAVE_DIR.")
endif()

# No exceptions and no threads: the sidecar hosts one module instance per
# thread, and nothing here throws.
add_compile_options(-fno-exceptions)

# webgpu.h over wasi:webgpu. Defined as the `webgpu` target before PyroWave's
# directory is added, which is how its CMake takes an implementation other
# than wgpu-native or Dawn.
add_subdirectory(webgpu-wasi)

set(PYROWAVE_WEBGPU_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory("${PYROWAVE_DIR}/webgpu" pyrowave-webgpu)

add_subdirectory(module)
