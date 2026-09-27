# A native test of the codec module: the same codec.cpp, pyrowave_codec.cpp
# and generated bindings the component is built from, linked against
# wgpu-native instead of the wasi:webgpu shim, and driven through the exports
# the sidecar calls. Not part of the package build; configure it on its own,
# with clang or GCC (MSVC cannot compile wit-bindgen's C, which casts structs
# to their own type). On Windows, llvm-mingw:
#
#   cmake -S codec/test -B <dir> -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release #     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DPYROWAVE_DIR=<pyrowave checkout>
#   cmake --build <dir>
#   <dir>/pyrowave-codec-test checks
#   <dir>/pyrowave-codec-test roundtrip in.y4m out.y4m --params '{"bytes_per_frame":60000}'
#
# PyroWave's webgpu/ CMake finds wgpu-native in its external/ directory
# (cmake -P webgpu/fetch_wgpu_native.cmake there), or set
# PYROWAVE_WEBGPU_NATIVE_DIR.

cmake_minimum_required(VERSION 3.20)
project(ffrwd-pyrowave-codec-test LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PYROWAVE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../pyrowave" CACHE PATH "PyroWave checkout with the webgpu/ port")
set(PYROWAVE_WEBGPU_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory("${PYROWAVE_DIR}/webgpu" pyrowave-webgpu)

set(CODEC ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_executable(pyrowave-codec-test
		codec_test.cpp
		${CODEC}/src/codec.cpp
		${CODEC}/src/pyrowave_codec.cpp
		${CODEC}/generated/codec.c)
target_include_directories(pyrowave-codec-test PRIVATE ${CODEC}/generated ${CODEC}/src)
target_link_libraries(pyrowave-codec-test PRIVATE pyrowave-webgpu)
target_compile_options(pyrowave-codec-test PRIVATE -Wall -Wextra -Wno-unused-parameter)
if (MSVC)
	message(FATAL_ERROR "Build the test with clang or GCC: MSVC cannot compile wit-bindgen's C.")
endif()
# The bindings' wasm export attributes mean nothing natively.
set_source_files_properties(${CODEC}/generated/codec.c PROPERTIES COMPILE_OPTIONS "-Wno-unknown-attributes")
if (WIN32 AND EXISTS "${PYROWAVE_WEBGPU_NATIVE_DIR}/lib/wgpu_native.dll")
	add_custom_command(TARGET pyrowave-codec-test POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PYROWAVE_WEBGPU_NATIVE_DIR}/lib/wgpu_native.dll"
			        $<TARGET_FILE_DIR:pyrowave-codec-test>)
endif()
