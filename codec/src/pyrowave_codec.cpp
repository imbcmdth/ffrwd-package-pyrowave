// The codec module's working parts; see pyrowave_codec.hpp.

#include "pyrowave_codec.hpp"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace pyrowave_codec
{
const char *const LOG = "ffrwd/pyrowave codec";
const char *const CODEC = "pyrowave";
const char *const FOURCC = "PYRW";

namespace
{
// A frame rate a time base may stand for when nothing else gives a frame's
// duration: one tick is one frame from 1 up to this many per second.
const double MAX_TIME_BASE_RATE = 240.0;

pyrowave_webgpu_device device = nullptr;

void on_message(void *, const char *message)
{
	fprintf(stderr, "%s: %s", LOG, message);
}

pyrowave_webgpu_chroma_subsampling subsampling(Chroma chroma)
{
	return chroma == Chroma::C444 ? PYROWAVE_WEBGPU_CHROMA_SUBSAMPLING_444 : PYROWAVE_WEBGPU_CHROMA_SUBSAMPLING_420;
}

pyrowave_webgpu_cpu_buffer planes(const StreamShape &shape, const uint8_t *frame)
{
	pyrowave_webgpu_cpu_buffer cpu = {};
	size_t width = shape.width;
	size_t height = shape.height;
	size_t chroma_width = shape.chroma == Chroma::C444 ? width : width / 2;
	size_t chroma_height = shape.chroma == Chroma::C444 ? height : height / 2;
	size_t luma = width * height;
	size_t chroma = chroma_width * chroma_height;
	uint8_t *base = const_cast<uint8_t *>(frame);
	cpu.width = int(width);
	cpu.height = int(height);
	cpu.format = shape.chroma == Chroma::C444 ? PYROWAVE_WEBGPU_CPU_BUFFER_FORMAT_YUV444P
	                                          : PYROWAVE_WEBGPU_CPU_BUFFER_FORMAT_YUV420P;
	cpu.data[0] = base;
	cpu.data[1] = base + luma;
	cpu.data[2] = base + luma + chroma;
	cpu.row_stride_in_bytes[0] = width;
	cpu.row_stride_in_bytes[1] = chroma_width;
	cpu.row_stride_in_bytes[2] = chroma_width;
	cpu.plane_size_in_bytes[0] = luma;
	cpu.plane_size_in_bytes[1] = chroma;
	cpu.plane_size_in_bytes[2] = chroma;
	return cpu;
}

std::string failed(const char *what, pyrowave_webgpu_result result)
{
	return std::string(what) + ": " + pyrowave_webgpu_result_to_string(result);
}

void put_u32(uint8_t *out, uint32_t value)
{
	out[0] = uint8_t(value);
	out[1] = uint8_t(value >> 8);
	out[2] = uint8_t(value >> 16);
	out[3] = uint8_t(value >> 24);
}

uint32_t get_u32(const uint8_t *in)
{
	return uint32_t(in[0]) | uint32_t(in[1]) << 8 | uint32_t(in[2]) << 16 | uint32_t(in[3]) << 24;
}

// A value in the params object: a number, or null for "take the default".
struct Value
{
	bool null = false;
	double number = 0.0;
};

void skip_space(const char *&p)
{
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
}

// Walks a flat JSON object whose values are numbers or null, calling
// `take(key, value, error)` for each. Empty text is an empty object.
template <typename Take>
bool parse_object(const std::string &text, Take take, std::string *error)
{
	const char *p = text.c_str();
	skip_space(p);
	if (*p == '\0')
		return true;
	if (*p++ != '{')
	{
		*error = "params must be a JSON object";
		return false;
	}
	skip_space(p);
	if (*p == '}')
	{
		p++;
	}
	else
	{
		for (;;)
		{
			skip_space(p);
			if (*p++ != '"')
			{
				*error = "params: expected a key";
				return false;
			}
			const char *key = p;
			while (*p && *p != '"' && *p != '\\')
				p++;
			std::string name(key, size_t(p - key));
			if (*p++ != '"')
			{
				*error = "params: a key is not a plain string";
				return false;
			}
			skip_space(p);
			if (*p++ != ':')
			{
				*error = "params: expected ':' after " + name;
				return false;
			}
			skip_space(p);
			Value value;
			if (strncmp(p, "null", 4) == 0)
			{
				value.null = true;
				p += 4;
			}
			else
			{
				char *end = nullptr;
				value.number = strtod(p, &end);
				if (end == p)
				{
					*error = "params: " + name + " must be a number";
					return false;
				}
				p = end;
			}
			if (!take(name, value, error))
				return false;
			skip_space(p);
			if (*p == ',')
			{
				p++;
				continue;
			}
			if (*p++ != '}')
			{
				*error = "params: expected ',' or '}'";
				return false;
			}
			break;
		}
	}
	skip_space(p);
	if (*p != '\0')
	{
		*error = "params: trailing characters after the object";
		return false;
	}
	return true;
}
}

const char *pix_fmt(Chroma chroma)
{
	return chroma == Chroma::C444 ? "yuv444p" : "yuv420p";
}

bool chroma_of(const std::string &pix_fmt, Chroma *chroma)
{
	if (pix_fmt == "yuv420p")
		*chroma = Chroma::C420;
	else if (pix_fmt == "yuv444p")
		*chroma = Chroma::C444;
	else
		return false;
	return true;
}

bool check_shape(const StreamShape &shape, std::string *error)
{
	if (shape.width < 1 || shape.height < 1 || shape.width > 16384 || shape.height > 16384)
	{
		*error = "PyroWave codes 1 to 16384 pixels each way, not " + std::to_string(shape.width) + "x" +
		         std::to_string(shape.height);
		return false;
	}
	if (shape.chroma == Chroma::C420 && ((shape.width | shape.height) & 1))
	{
		*error = "4:2:0 needs an even width and height, not " + std::to_string(shape.width) + "x" +
		         std::to_string(shape.height);
		return false;
	}
	return true;
}

size_t frame_size(const StreamShape &shape)
{
	size_t luma = size_t(shape.width) * shape.height;
	return shape.chroma == Chroma::C444 ? luma * 3 : luma + 2 * (luma / 4);
}

void write_extradata(const StreamShape &shape, uint8_t out[EXTRADATA_SIZE])
{
	memcpy(out, FOURCC, 4);
	out[4] = EXTRADATA_VERSION;
	out[5] = uint8_t(EXTRADATA_SIZE);
	out[6] = uint8_t(shape.chroma);
	out[7] = 0;
	put_u32(out + 8, shape.width);
	put_u32(out + 12, shape.height);
}

bool read_extradata(const uint8_t *data, size_t size, StreamShape *shape, std::string *error)
{
	if (size < 6 || memcmp(data, FOURCC, 4) != 0)
	{
		*error = "the stream's extradata is not a PyroWave header";
		return false;
	}
	if (data[4] != EXTRADATA_VERSION)
	{
		*error = "PyroWave header version " + std::to_string(data[4]) + ": this decoder reads version " +
		         std::to_string(EXTRADATA_VERSION);
		return false;
	}
	if (data[5] < EXTRADATA_SIZE || size < data[5])
	{
		*error = "the stream's PyroWave header is cut short";
		return false;
	}
	if (data[6] > 1)
	{
		*error = "the stream's PyroWave header names chroma layout " + std::to_string(data[6]);
		return false;
	}
	shape->chroma = Chroma(data[6]);
	shape->width = get_u32(data + 8);
	shape->height = get_u32(data + 12);
	return check_shape(*shape, error);
}

bool parse_encoder_params(const std::string &text, EncoderParams *params, std::string *error)
{
	EncoderParams parsed;
	auto take = [&](const std::string &name, const Value &value, std::string *err) -> bool {
		if (name == "bitrate")
		{
			if (value.null)
				return true;
			if (!(value.number >= 1.0 && value.number <= 1e12))
			{
				*err = "params: bitrate must be a number of bits per second, from 1";
				return false;
			}
			parsed.bitrate = value.number;
			return true;
		}
		if (name == "bytes_per_frame")
		{
			if (value.null)
				return true;
			if (!(value.number >= double(MIN_BYTES_PER_FRAME) && value.number <= 1e9) ||
			    value.number != floor(value.number))
			{
				*err = "params: bytes_per_frame must be a whole number from " +
				       std::to_string(MIN_BYTES_PER_FRAME) + " up";
				return false;
			}
			parsed.bytes_per_frame = size_t(value.number);
			return true;
		}
		*err = "params: unknown key " + name + "; the keys are bitrate and bytes_per_frame";
		return false;
	};
	if (!parse_object(text, take, error))
		return false;
	*params = parsed;
	return true;
}

bool parse_decoder_params(const std::string &text, std::string *error)
{
	auto take = [](const std::string &name, const Value &, std::string *err) -> bool {
		*err = "params: unknown key " + name + "; the decoder takes none";
		return false;
	};
	return parse_object(text, take, error);
}

void Budget::init(const EncoderParams &params_, int32_t tb_num_, int32_t tb_den_, int32_t rate_num_,
                  int32_t rate_den_, size_t max_bytes_)
{
	params = params_;
	tb_num = tb_num_;
	tb_den = tb_den_;
	bool rate_known = rate_num_ > 0 && rate_den_ > 0;
	rate_num = rate_known ? rate_num_ : 0;
	rate_den = rate_known ? rate_den_ : 0;
	max_bytes = max_bytes_;
	last_duration = -1;
}

size_t Budget::clamp(double size) const
{
	if (size < double(MIN_BYTES_PER_FRAME))
		size = double(MIN_BYTES_PER_FRAME);
	if (size > double(max_bytes))
		size = double(max_bytes);
	return size_t(size);
}

bool Budget::next(int64_t duration, int64_t gap, size_t *bytes, std::string *error)
{
	if (params.bytes_per_frame)
	{
		*bytes = params.bytes_per_frame < max_bytes ? params.bytes_per_frame : max_bytes;
		return true;
	}

	if (rate_num)
	{
		*bytes = clamp(floor(params.bitrate * double(rate_den) / double(rate_num) / 8.0));
		return true;
	}

	if (duration <= 0)
		duration = gap;
	if (duration <= 0)
		duration = last_duration;
	if (duration <= 0 && tb_num > 0 && tb_den > 0)
	{
		double rate = double(tb_den) / double(tb_num);
		if (rate >= 1.0 && rate <= MAX_TIME_BASE_RATE)
			duration = 1;
	}
	if (duration <= 0)
	{
		*error = "a frame has no duration and the time base " + std::to_string(tb_num) + "/" +
		         std::to_string(tb_den) +
		         " does not read as a frame rate, so bitrate cannot become a size per frame; "
		         "set bytes_per_frame";
		return false;
	}
	last_duration = duration;

	double seconds = double(duration) * double(tb_num) / double(tb_den);
	*bytes = clamp(floor(params.bitrate * seconds / 8.0));
	return true;
}

pyrowave_webgpu_device shared_device(std::string *error)
{
	if (device)
		return device;
	pyrowave_webgpu_device_create_info info = {};
#ifdef __wasi__
	// The wasi:webgpu shim offers TimedWaitAny and never needs to spin;
	// wgpu-native v29, which the native test runs on, has no such feature.
	info.timed_wait_any = true;
#endif
	info.message_callback = on_message;
	auto result = pyrowave_webgpu_device_create(&info, &device);
	if (result != PYROWAVE_WEBGPU_SUCCESS)
	{
		device = nullptr;
		*error = failed("no GPU device for PyroWave", result);
		return nullptr;
	}
	return device;
}

void release_shared_device()
{
	if (device)
		pyrowave_webgpu_device_destroy(device);
	device = nullptr;
}

Encoder::~Encoder()
{
	close();
}

void Encoder::close()
{
	if (encoder)
		pyrowave_webgpu_encoder_destroy(encoder);
	encoder = nullptr;
}

bool Encoder::open(const StreamShape &shape, std::string *error)
{
	close();
	if (!check_shape(shape, error))
		return false;
	auto gpu = shared_device(error);
	if (!gpu)
		return false;
	pyrowave_webgpu_encoder_create_info info = {};
	info.device = gpu;
	info.width = int(shape.width);
	info.height = int(shape.height);
	info.chroma = subsampling(shape.chroma);
	auto result = pyrowave_webgpu_encoder_create(&info, &encoder);
	if (result != PYROWAVE_WEBGPU_SUCCESS)
	{
		encoder = nullptr;
		*error = failed("PyroWave encoder", result);
		return false;
	}
	shape_ = shape;
	return true;
}

size_t Encoder::capacity(size_t budget)
{
	return budget + 64 * 1024;
}

bool Encoder::encode(const uint8_t *frame, size_t size, size_t budget, uint8_t *out, size_t *written,
                     std::string *error)
{
	if (!encoder)
	{
		*error = "the encoder was not opened";
		return false;
	}
	if (size != frame_size(shape_))
	{
		*error = "a " + std::string(pix_fmt(shape_.chroma)) + " frame at " + std::to_string(shape_.width) + "x" +
		         std::to_string(shape_.height) + " is " + std::to_string(frame_size(shape_)) + " bytes, not " +
		         std::to_string(size);
		return false;
	}
	pyrowave_webgpu_cpu_buffer cpu = planes(shape_, frame);
	pyrowave_webgpu_rate_control rate = { budget };
	auto result = pyrowave_webgpu_encoder_encode_cpu(encoder, &cpu, &rate);
	if (result != PYROWAVE_WEBGPU_SUCCESS)
	{
		*error = failed("encode", result);
		return false;
	}
	return packet(budget, out, written, error);
}

bool Encoder::encode_textures(const WGPUTexture planes[3], size_t budget, uint8_t *out, size_t *written,
                              std::string *error)
{
	if (!encoder)
	{
		*error = "the encoder was not opened";
		return false;
	}
	pyrowave_webgpu_gpu_input input = {};
	for (int plane = 0; plane < 3; plane++)
		input.planes[plane] = planes[plane];
	pyrowave_webgpu_rate_control rate = { budget };
	auto result = pyrowave_webgpu_encoder_encode_gpu(encoder, &input, &rate);
	if (result != PYROWAVE_WEBGPU_SUCCESS)
	{
		*error = failed("encode", result);
		return false;
	}
	return packet(budget, out, written, error);
}

bool Encoder::packet(size_t budget, uint8_t *out, size_t *written, std::string *error)
{
	auto result = pyrowave_webgpu_encoder_poll(encoder, true);
	if (result != PYROWAVE_WEBGPU_SUCCESS)
	{
		*error = failed("encode readback", result);
		return false;
	}
	// One packet as large as the room there is: the whole frame.
	size_t room = capacity(budget);
	pyrowave_webgpu_packet packet = {};
	size_t count = 0;
	result = pyrowave_webgpu_encoder_packetize(encoder, &packet, room, &count, out, room);
	if (result != PYROWAVE_WEBGPU_SUCCESS)
	{
		*error = failed("packetize", result);
		return false;
	}
	if (count != 1 || packet.offset + packet.size > room)
	{
		*error = "packetize made " + std::to_string(count) + " packets of one frame";
		return false;
	}
	if (packet.offset)
		memmove(out, out + packet.offset, packet.size);
	*written = packet.size;
	return true;
}

Decoder::~Decoder()
{
	close();
}

void Decoder::close()
{
	if (decoder)
		pyrowave_webgpu_decoder_destroy(decoder);
	decoder = nullptr;
}

bool Decoder::open(const StreamShape &shape, std::string *error)
{
	close();
	if (!check_shape(shape, error))
		return false;
	auto gpu = shared_device(error);
	if (!gpu)
		return false;
	pyrowave_webgpu_decoder_create_info info = {};
	info.device = gpu;
	info.width = int(shape.width);
	info.height = int(shape.height);
	info.chroma = subsampling(shape.chroma);
	auto result = pyrowave_webgpu_decoder_create(&info, &decoder);
	if (result != PYROWAVE_WEBGPU_SUCCESS)
	{
		decoder = nullptr;
		*error = failed("PyroWave decoder", result);
		return false;
	}
	shape_ = shape;
	return true;
}

bool Decoder::decode(const uint8_t *packet, size_t size, uint8_t *frame, std::string *error)
{
	if (!decoder)
	{
		*error = "the decoder was not opened";
		return false;
	}
	if (!push(packet, size, error))
		return false;
	pyrowave_webgpu_cpu_buffer cpu = planes(shape_, frame);
	auto result = pyrowave_webgpu_decoder_decode_cpu(decoder, &cpu);
	if (result != PYROWAVE_WEBGPU_SUCCESS)
	{
		*error = failed("decode", result);
		return false;
	}
	return true;
}

bool Decoder::push(const uint8_t *packet, size_t size, std::string *error)
{
	if (!decoder)
	{
		*error = "the decoder was not opened";
		return false;
	}
	// Every packet is a whole frame and stands alone: forget the previous
	// one's sequence number, so that the parser's ordering (a 3-bit counter
	// that drops a frame it thinks is late) never discards one.
	pyrowave_webgpu_decoder_clear(decoder);
	auto result = pyrowave_webgpu_decoder_push_packet(decoder, packet, size);
	if (result != PYROWAVE_WEBGPU_SUCCESS)
	{
		*error = failed("packet", result);
		return false;
	}
	if (!pyrowave_webgpu_decoder_decode_is_ready(decoder, false))
	{
		*error = "a packet does not hold a whole frame";
		return false;
	}
	return true;
}

void plane_size(const StreamShape &shape, int plane, uint32_t *width, uint32_t *height)
{
	bool half = plane != 0 && shape.chroma == Chroma::C420;
	*width = half ? shape.width / 2 : shape.width;
	*height = half ? shape.height / 2 : shape.height;
}

bool Decoder::decode_textures(const uint8_t *packet, size_t size, WGPUTexture planes[3], std::string *error)
{
	for (int plane = 0; plane < 3; plane++)
		planes[plane] = nullptr;
	if (!push(packet, size, error))
		return false;
	WGPUInstance instance = nullptr;
	WGPUAdapter adapter = nullptr;
	WGPUDevice gpu = nullptr;
	pyrowave_webgpu_device_get_handles(device, &instance, &adapter, &gpu);
	pyrowave_webgpu_gpu_output output = {};
	for (int plane = 0; plane < 3; plane++)
	{
		WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
		desc.usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc;
		desc.dimension = WGPUTextureDimension_2D;
		plane_size(shape_, plane, &desc.size.width, &desc.size.height);
		desc.size.depthOrArrayLayers = 1;
		desc.format = WGPUTextureFormat_R8Unorm;
		desc.mipLevelCount = 1;
		desc.sampleCount = 1;
		planes[plane] = wgpuDeviceCreateTexture(gpu, &desc);
		if (!planes[plane])
		{
			release_textures(planes);
			*error = "no texture for a decoded plane";
			return false;
		}
		output.planes[plane] = planes[plane];
	}
	auto result = pyrowave_webgpu_decoder_decode_gpu(decoder, &output);
	if (result != PYROWAVE_WEBGPU_SUCCESS)
	{
		release_textures(planes);
		*error = failed("decode", result);
		return false;
	}
	return true;
}

void release_textures(WGPUTexture planes[3])
{
	for (int plane = 0; plane < 3; plane++)
	{
		if (planes[plane])
			wgpuTextureRelease(planes[plane]);
		planes[plane] = nullptr;
	}
}
}
