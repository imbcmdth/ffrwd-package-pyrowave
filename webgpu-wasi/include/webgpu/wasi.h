// What webgpu-wasi adds to webgpu.h: a wasi:webgpu texture handle crossing to
// or from another interface of the component as a WGPUTexture. An interface
// that hands textures in or out (a host's frames, say) carries them as
// own<gpu-texture> handles, which wit-bindgen's C spells as a struct holding
// an int32_t.

#ifndef WEBGPU_WASI_H_
#define WEBGPU_WASI_H_

#include <stdint.h>

#include "webgpu/webgpu.h"

#ifdef __cplusplus
extern "C" {
#endif

// Wraps `handle`, an own<gpu-texture> handed in, as a WGPUTexture holding one
// reference; releasing the last reference drops the handle. `descriptor` is
// what the texture was made with, which the getters answer.
WGPUTexture webgpu_wasi_texture_from_handle(int32_t handle, WGPUTextureDescriptor const* descriptor);

// Takes the own<gpu-texture> handle out of `texture`, which must hold its last
// reference, for handing out; the WGPUTexture is gone afterwards.
int32_t webgpu_wasi_texture_into_handle(WGPUTexture texture);

#ifdef __cplusplus
}
#endif

#endif
