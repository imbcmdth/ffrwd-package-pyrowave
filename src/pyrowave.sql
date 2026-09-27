-- The round trip, hosted as the wasm module the package builds.
--
-- `roundtrip` encodes each frame with PyroWave on the GPU at no more than
-- `bytes_per_frame` bytes, decodes it again, and returns what the decoder
-- produced: the same size and format as what went in, at the quality that
-- rate buys. The module takes yuv420p only, and needs the GPU granted.
CREATE FUNCTION roundtrip(v video_stream, bytes_per_frame number DEFAULT 60000)
RETURNS video_stream
  AS 'modules/pyrowave_roundtrip.wasm', 'roundtrip' LANGUAGE wasm;

-- The same round trip with one row per frame beside the picture: the frame's
-- time, the bytes the encoder used, and the wall clock the encode and the
-- decode took inside the module.
CREATE FUNCTION roundtrip_stats(v video_stream, bytes_per_frame number DEFAULT 60000)
RETURNS STRUCT(v video_stream, frames STRUCT(time number, bytes number,
                                             encode_ms number, decode_ms number)[])
  AS 'modules/pyrowave_roundtrip.wasm', 'roundtrip' LANGUAGE wasm;
