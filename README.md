# ffrwd/pyrowave

[PyroWave](https://github.com/Themaister/pyrowave), the GPU wavelet codec, as
ffrwd sidecar modules. The codec is the WebGPU port in the fork at
https://github.com/imbcmdth/pyrowave (branch `webgpu`), compiled with wasi-sdk
to a wasm32-wasip2 component that reaches the GPU through `wasi:webgpu`. The
sidecar hosts that interface for a module granted `-gpu`.

The package has two modules:

* `modules/pyrowave.wasm`, the codec: an encoder and a decoder in one
  component, on ffrwd:av 0.18.0's `codec-module` world. The lib calls them
  `encode` and `decode`; the codec is `pyrowave` and its tag on the wire is
  `PYRW`.
* `modules/pyrowave_roundtrip.wasm`, a round trip on ffrwd:av 0.18.0's
  `video-module` world: each frame is encoded at a chosen size and decoded
  again, and the decoded picture is what comes out. It shows what a rate
  costs.

Both need ffrwd 0.24.0 or newer (see Running).

## Layout

* `codec/`: the codec module. `codec/src/pyrowave_codec.cpp` holds the
  extradata header, the params, the size per frame and the GPU encoder and
  decoder; `codec/src/codec.cpp` is the glue to the `encoder` and `decoder`
  exports. `codec/wit/deps/ffrwd-av/av.wit` is the CLI's `sidecar/wit/av.wit`
  at v0.24.0 (ffrwd:av 0.18.0, the same file `ffrwd/wasm` 0.18.0 ships), and
  `codec/generated/` is wit-bindgen's C output for it. `codec/test/` is a
  native test of the same code (see Testing).
* `module/`: the round-trip filter, C++ on ffrwd:av 0.18.0's `video-module`
  world (`module/wit/deps/ffrwd-av/av.wit` is the same v0.24.0 file).
  `module/generated/` is wit-bindgen's C output for it.
* `webgpu-wasi/`: `webgpu.h` implemented over `wasi:webgpu@0.0.1`, the part any
  C or C++ module needs to use the GPU. Self-contained, meant to move to a
  repository of its own; see its README.
* `src/codec.sql`: the lib's `encode` and `decode`. `src/pyrowave.sql`:
  `roundtrip` and `roundtrip_stats`.
* `build.sh`, `CMakeLists.txt`: the build.

## Building

Needs wasi-sdk 34 or newer, CMake 3.20 or newer with Ninja or make, and a
checkout of the PyroWave fork (built against 5e80f92):

```
WASI_SDK=/path/to/wasi-sdk PYROWAVE_DIR=/path/to/pyrowave sh build.sh
```

The modules land in `modules/`. The PyroWave fork is used as is: its
`webgpu/` CMake takes any `webgpu` target defined before it, which is how the
shim stands in for wgpu-native.

Regenerating the bindings with wit-bindgen-cli 0.62, from `codec/` and from
`module/`:

```
wit-bindgen c wit --world codec --out-dir generated
wit-bindgen c wit --world roundtrip --out-dir generated
```

## The codec

`pyrowave.wasm` exports `ffrwd:av/encoder@0.18.0` and
`ffrwd:av/decoder@0.18.0` and imports `wasi:webgpu/webgpu@0.0.1` beside
WASI 0.2. Both describe themselves as `pyrowave` with pixel formats
`yuv420p` and `yuv444p`. The encoder's codec is `pyrowave`, its fourcc `PYRW`,
and its delay, decode delay and frame samples are all 0: PyroWave is intra
only, so every frame leaves as one packet in the call that brings it, and
every packet is a keyframe with `dts` equal to `pts` and the frame's own
duration. The decoder reads `PYRW` with a delay of 0 and answers each packet
with one frame at the packet's `pts` and duration.

Each instance opens its GPU device at its first `init` and keeps it; an
instance that runs both an encoder and a decoder shares the one device.

### Encoder params

JSON, as the lib's `encode` passes them:

* `bitrate`: bits per second (default 200000000). The encoder's `init` takes
  the stream's nominal frame rate as `frame-rate: option<rational>` beside
  the params; ffrwd hands it the rate its input was probed at (the sidecar's
  `-frame_rate`). With a rate, every frame's size is bitrate / frame rate / 8
  bytes: 14400000 at 30000/1001 is 60060. With none, a frame's size is the
  bitrate over the frame's duration: the frame's own `duration`, else the gap
  to the next frame's `pts` in the same call, else the last duration seen,
  else one tick of the time base when the time base reads as a frame rate (1
  to 240 per second). A frame with none of these is refused with a message
  asking for `bytes_per_frame`. A rate with a zero or negative part counts as
  none.
* `bytes_per_frame`: a fixed size for every frame, from 1024 bytes. Wins over
  `bitrate` when set.

Either may be left out or null. Sizes are floored at 1024 bytes and capped at
twice the raw frame; the default bitrate is near lossless at 720p30 (about
613 kB a frame, 68 dB).

The decoder takes no params: empty, or `{}`.

### Extradata

The coded stream's extradata is a 16-byte header, little-endian, which the
decoder needs before the first packet to open at the right size and chroma
layout:

| offset | size | field |
|---|---|---|
| 0 | 4 | magic, `PYRW` |
| 4 | 1 | version, 1 |
| 5 | 1 | header size in bytes, 16; a reader skips bytes past what it knows |
| 6 | 1 | chroma: 0 is 4:2:0, 1 is 4:4:4 (the bitstream's `chroma_resolution`) |
| 7 | 1 | reserved, 0 |
| 8 | 4 | width, u32 |
| 12 | 4 | height, u32 |

A 1280x720 4:2:0 stream's is `50595257 01 10 00 00 00050000 d0020000`. The
decoder refuses another magic or version, a short header, and a width or
height other than the one the coded stream declares. With no extradata at all
it opens at the coded stream's size as 4:2:0; a 4:4:4 stream then fails at its
first packet, whose own sequence header says which it is.

A packet is one whole PyroWave frame: the bitstream of `bitstream.md` in the
PyroWave repository, one sequence header and the frame's blocks, nothing
added.

## Running

Both modules import `wasi:webgpu`, so the sidecar refuses to run either
without the grant (`-gpu <module>`).

This needs ffrwd 0.24.0 or newer, whose sidecar hosts ffrwd:av 0.18.0's
`encoder` and `decoder` with the encoder's `init` taking `frame-rate`; the
manifest's `ffrwd` key says `>=0.24.0`. This version's codec does not
instantiate on an ffrwd from before that signature. ffrwd grants `-gpu`
itself, from the module's own imports.

```
uv venv ffrwd-0.24.0
uv pip install --python ffrwd-0.24.0 ffrwd==0.24.0
```

The package publishes private, so installing it from the registry needs a
token that can read the `ffrwd` namespace (`ffrwd login`), and then, in a
project:

```
ffrwd install ffrwd/pyrowave
```

Or linked from a checkout, from the package's directory and then the
project's:

```
ffrwd link
ffrwd link ffrwd/pyrowave
```

The encoder is written in a query where a codec name goes. Audio can be
copied beside it:

```
ffrwd run -y "COPY (SELECT f.video[1], f.audio[1] FROM input('src.mp4') f)
  TO 'enc.nut' WITH (video_codec ffrwd.pyrowave.encode(bitrate => 200000000), audio_codec 'copy')"
ffrwd run -y "COPY (SELECT f.video[1], f.audio[1] FROM input('src.mp4') f)
  TO 'enc.nut' WITH (video_codec ffrwd.pyrowave.encode(bytes_per_frame => 60000), audio_codec 'copy')"
```

ffrwd passes the input's frame rate to `init` (`-frame_rate 30/1` for a 30
fps input), so `bitrate => 14400000` on a 30 fps input writes the same
packets as `bytes_per_frame => 60000`.

The decoder is used unasked wherever a `PYRW` stream is read as frames, or
named outright with `decoder =>`:

```
ffrwd run -y "COPY (SELECT f.video[1] FROM input('enc.nut') f)
  TO 'back.y4m' WITH (video_codec 'rawvideo')"
ffrwd run -y "COPY (SELECT f.video[1], f.audio[1] FROM input('enc.nut') f)
  TO 'back.mkv' WITH (video_codec 'ffv1')"
ffrwd run -y "COPY (SELECT f.video[1] FROM input('enc.nut', decoder => ffrwd.pyrowave.decode()) f)
  TO 'back.y4m' WITH (video_codec 'rawvideo')"
```

An output that names no video codec copies the coded stream, which `.nut`,
`.mkv` and `.mov` keep by its tag, with the 16-byte extradata:

```
ffrwd run -y "COPY (SELECT f.video[1], f.audio[1] FROM input('enc.nut') f) TO 'copy.mkv'"
ffrwd run -y "COPY (SELECT f.video[1], f.audio[1] FROM input('enc.nut') f) TO 'copy.mov'"
```

A `.y4m` or `.mp4` output with no codec named is refused, since neither
holds `PYRW`.

A 4:4:4 source is coded 4:4:4: ffrwd hands the encoder yuv444p, which it
takes, and the extradata's chroma byte says so. `pix_fmt` names it outright,
and reads it back as yuv444p:

```
ffrwd run -y "COPY (SELECT f.video[1] FROM input('src444.mkv') f)
  TO 'enc444.nut' WITH (video_codec ffrwd.pyrowave.encode(bytes_per_frame => 120000), pix_fmt 'yuv444p')"
ffrwd run -y "COPY (SELECT f.video[1] FROM input('enc444.nut') f)
  TO 'back444.y4m' WITH (video_codec 'rawvideo', pix_fmt 'yuv444p')"
```

Colour tags travel with the container. A bt709 source encoded to `.mkv`
keeps `bt709` primaries, transfer and matrix and the tv range, and so does
what is decoded from it into another `.mkv`. NUT has no field for them, so a
stream that passes through `.nut` loses them, and a copy out of it into
`.mkv` or `.mov` has none to keep.

The round trip is a function on a video stream, through ffrwd:

```
ffrwd run -y "COPY (SELECT ffrwd.pyrowave.roundtrip(f.video[1], bytes_per_frame => 60000) FROM input('src.mp4') f)
  TO 'rt.y4m' WITH (video_codec 'rawvideo')"
```

or on the sidecar as it is:

```
ffrwd-wasm -gpu modules/pyrowave_roundtrip.wasm -f nut -i pipe:0   -m modules/pyrowave_roundtrip.wasm -params '{"bytes_per_frame":60000}'   -f nut pipe:1 -f ndjson rows.ndjson < in.nut > out.nut
```

Params: `bytes_per_frame`, the encoder's budget per frame (default 60000, at
least 1024). Input is yuv420p. Rows, one per frame: `time`, `bytes`,
`encode_ms`, `decode_ms` (upload, GPU work and readback, wall clock, inside the
module).

The manifest declares the `gpu` capability. A sidecar run without the grant
refuses the module with
`imports wasi:webgpu, and this run grants it no GPU; grant it with -gpu <module>`.

## Testing

`codec/test/` builds the codec's own sources (`codec.cpp`,
`pyrowave_codec.cpp` and the generated bindings) natively against
wgpu-native and calls the exports the way a host does, owning and freeing
what crosses. `pyrowave-codec-test checks` covers the describes, the params,
the size per frame (with a frame rate, with none, and at 30000/1001), the
extradata and the refusals; `pyrowave-codec-test roundtrip in.y4m out.y4m`
encodes a y4m through `encode`, hands the packets and the extradata to
`decode`, and writes what comes back (`--rate NUM/DEN` passes `init` a frame
rate). See `codec/test/CMakeLists.txt` for the build.

`cmake -P codec/test/check.cmake` builds the test and runs its `checks`. It
is the manifest's `test`, which `ffrwd publish` runs in the package's
directory before it packs, and a CMake script so the one command works from
cmd.exe and from a POSIX shell. It needs clang and clang++ on PATH
(llvm-mingw on Windows), Ninja or make, the PyroWave fork at `../pyrowave`
or `PYROWAVE_DIR` with wgpu-native in its `webgpu/external/` (or
`PYROWAVE_WEBGPU_NATIVE_DIR`), and a GPU. It builds under the temp directory
(`PYROWAVE_TEST_BUILD` moves it). From a clean build directory it takes about
10 s here and ends `passed: 0 failed`.

## Publishing

The archive `ffrwd publish` packs holds the manifest, the lib SQL
(`src/codec.sql`, `src/pyrowave.sql`), the two modules the SQL declares
(`modules/pyrowave.wasm`, `modules/pyrowave_roundtrip.wasm`, shipped
although `.gitignore` names `modules/`), this README and `LICENSE`: seven
files, about 665 kB gzipped. `.ffrwdignore` names the sources and the build,
which stay here. Build the modules before publishing: the archive takes
what is in `modules/`.

## License

MIT, in `LICENSE`, which also carries the notices for what the modules are
compiled from: PyroWave (MIT) and `webgpu.h` (BSD-3-Clause). The WIT under
`webgpu-wasi/wit/` is Apache-2.0 WITH LLVM-exception (`webgpu-wasi/wit/LICENSE`).

## Measured

### The codec

RTX 4090, Windows. `pyrowave.wasm` itself, run on wasmtime 48 with the
sidecar's vendored wasi:webgpu host (wgpu 29, Vulkan) by a stand-in for the
sidecar's codec host, written before it, and the native test on wgpu-native v29
(Vulkan), both over the same 150 frames of `es.mp4`. Every decoded frame is
bit for bit what the fork's own `pyrowave-webgpu-encode` and `-decode` make
at the same size (framemd5), in 4:2:0 at 720p and 1080p and in 4:4:4 at
720p (`testsrc2`, 30 frames).

| stream | params | mean bytes per frame | Y / U / V (dB) |
|---|---|---|---|
| 720p 4:2:0 | `bytes_per_frame` 60000 | 59959.3 | 32.798 / 41.097 / 38.020 |
| 720p 4:2:0 | `bitrate` 14400000 at 1/30 | 59959.3 | the same frames |
| 720p 4:2:0 | defaults (200 Mb/s) | 613347.2 | 68.286 / 67.344 / 67.132 |
| 1080p 4:2:0 | `bytes_per_frame` 120000 | 119962.1 | 38.954 / 44.347 / 42.237 |
| 720p 4:4:4 | `bytes_per_frame` 120000 | 119965.1 | 44.003 / 44.393 / 44.170 |

`bitrate` 14400000 at 30 frames a second is 60000 bytes a frame, and gives the
same packets whether the size comes from the frame rate `init` is given, from
the frames' durations, from the gaps between their `pts` or from a 1/30 time
base, at 1/30 or 1/90000, one frame per call or several.

Per frame at 720p, 60000 bytes, one frame per call, measured by the stand-in
host around each call (so including the copies across the component
boundary): encode 0.73 ms and decode 0.88 ms; at 1080p and 120000 bytes,
1.45 and 1.81 ms. The native test takes 0.42 / 0.37 ms and 0.78 / 0.84 ms.
Opening the device and compiling the encode pipelines takes about 60 ms per
instance; a decoder in the same instance reuses the device.

### Through ffrwd

RTX 4090, Windows, the released ffrwd 0.24.0 from PyPI and its own sidecar
(`FFRWD_WASM` unset), this version's archive linked into a scratch project,
over all of `es.mp4`: 300 frames of 1280x720 yuv420p at 30 fps, bt709, with
AAC copied beside them. The coded NUT's video stream has the tag `PYRW`, 16
bytes of extradata (`50595257 01 10 00 00 00050000 d0020000`), 300 packets,
all keyframes, in the time base NUT picks (1/61440 with the audio, a frame
every 2048 ticks). Decoded again by ffrwd, the frames are bit for bit the
native test's at the same params (framemd5), and so are those decoded with
`decoder => ffrwd.pyrowave.decode()` named, into `.mkv` as ffv1, from the
stream copied into `.mkv` and `.mov`, and what `roundtrip` makes at 60000.

| params | mean bytes per frame | Y / U / V (dB), 300 frames | first 150 frames |
|---|---|---|---|
| `bytes_per_frame` 60000 | 59956.6 | 33.827 / 41.993 / 39.192 | 32.798 / 41.097 / 38.020 |
| `bitrate` 14400000 (at 30/1) | 59956.6 | the same file, byte for byte | the same |
| `bitrate` 200000000 (at 30/1) | 548183.4 | 68.048 / 68.396 / 68.202 | |

The first 150 frames give the numbers of the codec's table above.

A 4:4:4 source, 60 frames of 1280x720 yuv444p in FFV1, encoded with
`pix_fmt 'yuv444p'` at `bytes_per_frame` 120000: extradata
`50595257 01 10 01 00 00050000 d0020000` (chroma 1, 4:4:4), 60 keyframes of
119953.3 bytes on average, decoded back as yuv444p bit for bit what the
native test makes from the same frames, 37.660 / 49.534 / 48.133 dB against
the source. Without `pix_fmt` the coded file is the same.

The bt709 source encoded to `.mkv` probes `color_range=tv`,
`color_space=bt709`, `color_transfer=bt709`, `color_primaries=bt709`, and so
does its decode into another `.mkv`.

Wall time of a whole `ffrwd run`, median of three, including what ffrwd
spends before ffmpeg starts (Python, probing the input, describing the
module):

| run | wall | frames per second |
|---|---|---|
| encode, 60000 bytes a frame, audio copied | 1.35 s | 222 |
| encode, 200 Mb/s, audio copied | 1.46 s | 205 |
| decode to y4m, 60000 bytes a frame | 1.44 s | 209 |
| decode to y4m, 200 Mb/s | 1.58 s | 190 |
| no codec, the source to y4m (for scale) | 0.85 s | 353 |

### The round trip

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
