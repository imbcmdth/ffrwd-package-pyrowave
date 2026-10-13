# pyrowave.wasm: PyroWave's encoder and decoder, one component on ffrwd:av
# 0.18.0's codec-module world. A reactor: the sidecar calls its exports, it
# has no main.
#
# pyrowave_gpu.wasm: the same sources built with PYROWAVE_GPU against the
# bindings in generated-gpu/, which add ffrwd:gpu's encoder and decoder: the
# pictures cross as textures of the sidecar's device.

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

add_executable(pyrowave_codec_gpu
		src/codec.cpp
		src/pyrowave_codec.cpp
		src/pyrowave_codec.hpp
		generated-gpu/codec.c
		generated-gpu/codec.h
		generated-gpu/codec_component_type.o)
set_target_properties(pyrowave_codec_gpu PROPERTIES OUTPUT_NAME pyrowave_gpu SUFFIX ".wasm")
target_include_directories(pyrowave_codec_gpu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/generated-gpu ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(pyrowave_codec_gpu PRIVATE PYROWAVE_GPU=1)
target_link_libraries(pyrowave_codec_gpu PRIVATE pyrowave-webgpu webgpu)
target_compile_options(pyrowave_codec_gpu PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_options(pyrowave_codec_gpu PRIVATE -mexec-model=reactor)
