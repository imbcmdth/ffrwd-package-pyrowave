// The codec module's working parts, free of the component bindings: the
// extradata header, the params, the per-frame budget, and a PyroWave encoder
// and decoder on one GPU device. codec.cpp is the ffrwd:av 0.18.0 glue over
// these; the native test links the same file against wgpu-native.

#ifndef FFRWD_PYROWAVE_CODEC_HPP
#define FFRWD_PYROWAVE_CODEC_HPP

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "pyrowave_webgpu.h"

namespace pyrowave_codec
{
// What this module's stderr lines start with.
extern const char *const LOG;

// The codec's name and its tag on the wire.
extern const char *const CODEC;
extern const char *const FOURCC;

enum class Chroma : uint8_t
{
	// The values are the bitstream's own chroma_resolution.
	C420 = 0,
	C444 = 1
};

// The pix-fmt a chroma layout is carried as, and back. False for a pix-fmt
// the codec does not take.
const char *pix_fmt(Chroma chroma);
bool chroma_of(const std::string &pix_fmt, Chroma *chroma);

// One stream's fixed shape, which is what the extradata carries.
struct StreamShape
{
	uint32_t width = 0;
	uint32_t height = 0;
	Chroma chroma = Chroma::C420;
};

// Checks the geometry PyroWave takes: 1 to 16384 each way, even for 4:2:0.
bool check_shape(const StreamShape &shape, std::string *error);

// Bytes in one tightly packed planar frame of the shape.
size_t frame_size(const StreamShape &shape);

// The extradata: a 16-byte versioned header, little-endian.
//
//   0  4  magic "PYRW"
//   4  1  version, 1
//   5  1  header size in bytes, 16; a reader skips what it does not know
//   6  1  chroma: 0 = 4:2:0, 1 = 4:4:4 (the bitstream's chroma_resolution)
//   7  1  reserved, 0
//   8  4  width, u32
//  12  4  height, u32
const size_t EXTRADATA_SIZE = 16;
const uint8_t EXTRADATA_VERSION = 1;
void write_extradata(const StreamShape &shape, uint8_t out[EXTRADATA_SIZE]);
bool read_extradata(const uint8_t *data, size_t size, StreamShape *shape, std::string *error);

// The encoder's params, from the SQL call's JSON:
//   bitrate          bits per second (default 200000000)
//   bytes_per_frame  a fixed budget per frame; wins over bitrate when set
// A key left out or null takes its default.
const double DEFAULT_BITRATE = 200e6;
// The bitstream carries a frame in whole 32x32 blocks; below this a frame
// cannot hold its own headers.
const size_t MIN_BYTES_PER_FRAME = 1024;
struct EncoderParams
{
	double bitrate = DEFAULT_BITRATE;
	size_t bytes_per_frame = 0; // 0: not set
};
bool parse_encoder_params(const std::string &text, EncoderParams *params, std::string *error);
// The decoder takes none: empty, or an empty object.
bool parse_decoder_params(const std::string &text, std::string *error);

// Turns the params into a byte budget per frame. With bytes_per_frame set
// that is the budget. Otherwise, with the stream's nominal frame rate known,
// it is the bitrate over that rate, the same for every frame. With no rate it
// is the bitrate over the frame's duration: the frame's own, else the gap to
// the next frame's pts, else the last duration seen, else one tick of the
// time base when the time base reads as a frame rate (1 to 240 per second).
class Budget
{
public:
	// `rate_num`/`rate_den` is the frame rate in frames per second; 0/0
	// when the host gave none.
	void init(const EncoderParams &params, int32_t tb_num, int32_t tb_den, int32_t rate_num, int32_t rate_den,
	          size_t max_bytes);
	// `duration` and `gap` are in time base ticks; negative when unknown.
	bool next(int64_t duration, int64_t gap, size_t *bytes, std::string *error);

private:
	size_t clamp(double size) const;

	EncoderParams params;
	int32_t tb_num = 1;
	int32_t tb_den = 1;
	int32_t rate_num = 0;
	int32_t rate_den = 0;
	size_t max_bytes = 0;
	int64_t last_duration = -1;
};

// The GPU device, opened once per instance on first use and shared by the
// encoder and the decoder. The sidecar hosts one instance per worker and caps
// GPU jobs at 2, so there are never more than a couple of these.
pyrowave_webgpu_device shared_device(std::string *error);
// Destroys it. Only after every encoder and decoder is gone.
void release_shared_device();

class Encoder
{
public:
	~Encoder();
	bool open(const StreamShape &shape, std::string *error);
	void close();
	const StreamShape &shape() const { return shape_; }

	// Room one packet needs at a budget: the budget plus the packetizer's
	// slack.
	static size_t capacity(size_t budget);

	// Encodes one frame of frame_size(shape()) bytes at no more than
	// `budget` bytes, into `out`, which has capacity(budget) bytes of room.
	bool encode(const uint8_t *frame, size_t size, size_t budget, uint8_t *out, size_t *written,
	            std::string *error);

private:
	pyrowave_webgpu_encoder encoder = nullptr;
	StreamShape shape_;
};

class Decoder
{
public:
	~Decoder();
	bool open(const StreamShape &shape, std::string *error);
	void close();
	const StreamShape &shape() const { return shape_; }

	// Decodes one packet holding one whole frame into `frame`, which has
	// frame_size(shape()) bytes of room.
	bool decode(const uint8_t *packet, size_t size, uint8_t *frame, std::string *error);

private:
	pyrowave_webgpu_decoder decoder = nullptr;
	StreamShape shape_;
};
}

#endif
