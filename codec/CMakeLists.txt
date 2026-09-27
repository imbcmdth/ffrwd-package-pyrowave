# pyrowave.wasm: PyroWave's encoder and decoder, one component on ffrwd:av
# 0.18.0's codec-module world. A reactor: the sidecar calls its exports, it
# has no main.

add_executable(pyrowave_codec
		src/codec.cpp
		src/pyrowave_codec.cpp
		src/pyrowave_codec.hpp
		generated/codec.c
		generated/codec.h
		generated/codec_component_type.o)
set_target_properties(pyrowave_codec PROPERTIES OUTPUT_NAME pyrowave SUFFIX ".wasm")
target_include_directories(pyrowave_codec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/generated ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pyrowave_codec PRIVATE pyrowave-webgpu webgpu)
target_compile_options(pyrowave_codec PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_options(pyrowave_codec PRIVATE -mexec-model=reactor)
