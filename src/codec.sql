-- PyroWave as a codec, hosted as the one wasm module that exports both its
-- encoder and its decoder (ffrwd:av 0.18.0's codec-module world). Both need
-- the GPU granted.
--
-- `encode` is written where a codec name goes in a COPY's options:
--   WITH (video_codec => ffrwd.pyrowave.encode(bitrate => 50000000))
-- Every frame is coded on its own (intra only) at no more than a size per
-- frame: `bytes_per_frame` when it is set, and otherwise `bitrate` (bits per
-- second) over each frame's duration. It takes yuv420p or yuv444p and tags
-- the stream PYRW.
CREATE FUNCTION encode(bitrate number DEFAULT 200000000, bytes_per_frame number DEFAULT NULL)
RETURNS encoder
  AS 'modules/pyrowave.wasm', 'encode' LANGUAGE wasm;

-- `decode` reads a PYRW stream back to the frames it was made from, in the
-- pixel format the stream's header names.
CREATE FUNCTION decode() RETURNS decoder
  AS 'modules/pyrowave.wasm', 'decode' LANGUAGE wasm;

-- `encode_gpu` and `decode_gpu` are `encode` and `decode` built to take and
-- hand back their pictures as textures of the sidecar's device. Where the
-- pictures are held on the GPU on that side - a module holding them there
-- writes them, or reads what is decoded - nothing crosses host memory but
-- the packets; elsewhere they read and write bytes as `encode` and `decode`
-- do. The packets are the same either way. A PYRW stream is decoded with
-- `decode` unless `decode_gpu` is named:
--   input('enc.nut', decoder => ffrwd.pyrowave.decode_gpu())
CREATE FUNCTION encode_gpu(bitrate number DEFAULT 200000000, bytes_per_frame number DEFAULT NULL)
RETURNS encoder
  AS 'modules/pyrowave_gpu.wasm', 'encode' LANGUAGE wasm;

CREATE FUNCTION decode_gpu() RETURNS decoder
  AS 'modules/pyrowave_gpu.wasm', 'decode' LANGUAGE wasm;
