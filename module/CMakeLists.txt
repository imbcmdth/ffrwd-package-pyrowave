# pyrowave_roundtrip.wasm: the frame filter that encodes and decodes each
# frame on the GPU. A reactor: the sidecar calls its exports, it has no main.

add_executable(pyrowave_roundtrip
		src/roundtrip.cpp
		generated/roundtrip.c
		generated/roundtrip.h
		generated/roundtrip_component_type.o)
set_target_properties(pyrowave_roundtrip PROPERTIES SUFFIX ".wasm")
target_include_directories(pyrowave_roundtrip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/generated)
target_link_libraries(pyrowave_roundtrip PRIVATE pyrowave-webgpu webgpu)
target_compile_options(pyrowave_roundtrip PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_options(pyrowave_roundtrip PRIVATE -mexec-model=reactor)
