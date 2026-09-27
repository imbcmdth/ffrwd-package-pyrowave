# ffrwd/pyrowave

[PyroWave](https://github.com/Themaister/pyrowave), the GPU wavelet codec, as
ffrwd sidecar modules. The codec is the WebGPU port in the fork at
https://github.com/imbcmdth/pyrowave (branch `webgpu`), compiled with wasi-sdk
to a wasm32-wasip2 component that reaches the GPU through `wasi:webgpu`. The
sidecar hosts that interface for a module granted `-gpu`.

For now the package has one module, a round trip: each frame is encoded at a
chosen size and decoded again on the GPU, and the decoded picture is what comes
out. It needs no codec support in the dialect and shows what a rate costs.
Encoder and decoder modules, a codec id in NUT and the dialect spelling
(`WITH (video_codec => ffrwd.pyrowave.encode(...))`) come next.

## Layout

* `module/`: the round-trip filter, C++ on ffrwd:av 0.17.0's `video-module`
  world (`module/wit/deps/ffrwd-av/av.wit` is the sidecar's `wit/av.wit` at
  0.17.0). `module/generated/` is wit-bindgen's C output for it.
* `webgpu-wasi/`: `webgpu.h` implemented over `wasi:webgpu@0.0.1`, the part any
  C or C++ module needs to use the GPU. Self-contained, meant to move to a
  repository of its own; see its README.
* `src/pyrowave.sql`: the lib, `roundtrip` and `roundtrip_stats`.
* `build.sh`, `CMakeLists.txt`: the build.

## Building

Needs wasi-sdk 34 or newer, CMake 3.20 or newer with Ninja or make, and a
checkout of the PyroWave fork (built against 5e80f92):

```
WASI_SDK=/path/to/wasi-sdk PYROWAVE_DIR=/path/to/pyrowave sh build.sh
```

The module lands in `modules/pyrowave_roundtrip.wasm`. The PyroWave fork is
used as is: its `webgpu/` CMake takes any `webgpu` target defined before it,
which is how the shim stands in for wgpu-native.

## Running

The module imports `wasi:webgpu`, so the sidecar refuses to run it without
the grant:

```
ffrwd-wasm -gpu modules/pyrowave_roundtrip.wasm -f nut -i pipe:0 \
  -m modules/pyrowave_roundtrip.wasm -params '{"bytes_per_frame":60000}' \
  -f nut pipe:1 -f ndjson rows.ndjson < in.nut > out.nut
```

Params: `bytes_per_frame`, the encoder's budget per frame (default 60000, at
least 1024). Input is yuv420p. Rows, one per frame: `time`, `bytes`,
`encode_ms`, `decode_ms` (upload, GPU work and readback, wall clock, inside the
module).

The manifest declares the `gpu` capability, which ffrwd 0.22.0 does not know
yet: until the CLI grants it, the package does not link, and a query calling
the module is refused by the sidecar with
`imports wasi:webgpu, and this run grants it no GPU; grant it with -gpu <module>`.

## Measured

RTX 4090, Windows, the sidecar's wgpu on Vulkan, 150 frames of real footage
(`es.mp4`) at 1280x720 yuv420p, compared with the source by ffmpeg's psnr
filter. The native column is the fork's own `pyrowave-webgpu-encode` and
`-decode` on wgpu-native v29 (Vulkan), on the same frames:

| bytes per frame | sidecar Y / U / V (dB) | native Y / U / V (dB) |
|---|---|---|
| 30000 | 30.580 / 39.444 / 34.869 | 30.580 / 39.444 / 34.869 |
| 60000 | 32.798 / 41.097 / 38.020 | 32.798 / 41.097 / 38.020 |
| 120000 | 37.719 / 46.150 / 45.553 | 37.719 / 46.150 / 45.553 |

The decoded frames are the native build's bit for bit (framemd5 at 60000).

Per frame inside the module at 120000 bytes (mean over 145 frames): 720p
encode 0.46 ms and decode 0.48 ms; 1080p encode 1.05 ms and decode 1.16 ms.
The native tools take 0.42 / 0.29 ms at 720p and 0.61 / 0.46 ms at 1080p; the
difference is moving frames and readbacks through the component boundary
(each crosses it as a copy).
