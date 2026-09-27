# webgpu.h over wasi:webgpu@0.0.1, as the `webgpu` target: a static library
# whose users include <webgpu/webgpu.h> and link it. The bindings'
# component-type object travels with it, so a component linked from it
# imports wasi:webgpu.

add_library(webgpu STATIC
		src/webgpu_wasi.c
		src/enums.h
		generated/webgpu_wasi.c
		generated/webgpu_wasi.h)
target_include_directories(webgpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(webgpu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/generated ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(webgpu PRIVATE -Wall -Wextra -Wno-unused-parameter)
# A static library drops an object nothing references, and nothing references
# the component type, so it is linked by path into whatever links this.
target_link_libraries(webgpu INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/generated/webgpu_wasi_component_type.o)
