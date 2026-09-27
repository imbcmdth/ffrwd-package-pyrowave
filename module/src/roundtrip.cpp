// roundtrip: an ffrwd:av frame filter that encodes each yuv420p
// frame with PyroWave on the GPU and decodes it again, handing back what the
// decoder produced. It changes nothing about the stream's shape; what it
// proves is that the codec runs inside the sidecar through wasi:webgpu, and
// what a given rate costs in quality (compare the output with the input).
//
// Params: {"bytes_per_frame": N}, the encoder's per-frame budget (default
// 60000). One row per frame: {"time", "bytes", "encode_ms", "decode_ms"}.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>
#include <vector>

#include "pyrowave_webgpu.h"
#include "roundtrip.h"

namespace
{
// The export name the lib SQL's AS clause names.
const char *const NAME = "roundtrip";
// What this module's stderr lines start with.
const char *const LOG = "ffrwd/pyrowave roundtrip";
const char *const VERSION = "0.1.0";
const size_t DEFAULT_BYTES_PER_FRAME = 60000;
// The bitstream carries a frame in whole 32x32 blocks; below this a frame
// cannot hold its own headers.
const size_t MIN_BYTES_PER_FRAME = 1024;

const char *const PARAMS_SCHEMA =
		"{\"type\":\"object\",\"properties\":{\"bytes_per_frame\":{\"type\":\"integer\",\"minimum\":1024,"
		"\"description\":\"Encoded size budget per frame, in bytes\"}},\"additionalProperties\":false}";

const char *const ROWS_SCHEMA =
		"{\"type\":\"object\",\"properties\":{"
		"\"time\":{\"type\":\"number\",\"description\":\"Frame time, seconds\"},"
		"\"bytes\":{\"type\":\"integer\",\"description\":\"Encoded size of the frame\"},"
		"\"encode_ms\":{\"type\":\"number\",\"description\":\"Upload, encode and readback, wall clock\"},"
		"\"decode_ms\":{\"type\":\"number\",\"description\":\"Upload, decode and readback, wall clock\"}},"
		"\"required\":[\"time\",\"bytes\",\"encode_ms\",\"decode_ms\"]}";

struct State
{
	pyrowave_webgpu_device device = nullptr;
	pyrowave_webgpu_encoder encoder = nullptr;
	pyrowave_webgpu_decoder decoder = nullptr;
	int width = 0;
	int height = 0;
	size_t bytes_per_frame = DEFAULT_BYTES_PER_FRAME;
	std::vector<uint8_t> bitstream;
};

State state;

double now_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return double(ts.tv_sec) * 1e3 + double(ts.tv_nsec) * 1e-6;
}

void set_string(roundtrip_string_t *out, const std::string &text)
{
	out->len = text.size();
	out->ptr = static_cast<uint8_t *>(malloc(text.size() ? text.size() : 1));
	memcpy(out->ptr, text.data(), text.size());
}

std::string take_string(roundtrip_string_t *in)
{
	std::string text(reinterpret_cast<const char *>(in->ptr), in->len);
	roundtrip_string_free(in);
	return text;
}

void skip_space(const char *&p)
{
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
}

// Parses the params object. Empty, or a JSON object whose only key is
// bytes_per_frame with a whole number value.
bool parse_params(const std::string &text, size_t *bytes_per_frame, std::string *error)
{
	size_t value = DEFAULT_BYTES_PER_FRAME;
	const char *p = text.c_str();
	skip_space(p);
	if (*p == '\0')
	{
		*bytes_per_frame = value;
		return true;
	}
	if (*p++ != '{')
	{
		*error = "params must be a JSON object";
		return false;
	}
	skip_space(p);
	if (*p == '}')
	{
		*bytes_per_frame = value;
		return true;
	}
	for (;;)
	{
		skip_space(p);
		if (*p++ != '"')
		{
			*error = "params: expected a key";
			return false;
		}
		const char *key = p;
		while (*p && *p != '"')
			p++;
		std::string name(key, p - key);
		if (*p++ != '"')
		{
			*error = "params: unterminated key";
			return false;
		}
		skip_space(p);
		if (*p++ != ':')
		{
			*error = "params: expected ':' after " + name;
			return false;
		}
		skip_space(p);
		if (name != "bytes_per_frame")
		{
			*error = "params: unknown key " + name + "; the one key is bytes_per_frame";
			return false;
		}
		char *end = nullptr;
		double number = strtod(p, &end);
		if (end == p || number != double(uint64_t(number)) || number < double(MIN_BYTES_PER_FRAME) ||
		    number > 1e9)
		{
			*error = "params: bytes_per_frame must be a whole number from 1024 up";
			return false;
		}
		value = size_t(number);
		p = end;
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
		skip_space(p);
		if (*p != '\0')
		{
			*error = "params: trailing characters after the object";
			return false;
		}
		*bytes_per_frame = value;
		return true;
	}
}

void on_message(void *, const char *message)
{
	fprintf(stderr, "%s: %s", LOG, message);
}

void release()
{
	if (state.encoder)
		pyrowave_webgpu_encoder_destroy(state.encoder);
	if (state.decoder)
		pyrowave_webgpu_decoder_destroy(state.decoder);
	if (state.device)
		pyrowave_webgpu_device_destroy(state.device);
	state.encoder = nullptr;
	state.decoder = nullptr;
	state.device = nullptr;
}

bool open(int width, int height, std::string *error)
{
	pyrowave_webgpu_device_create_info device_info = {};
	device_info.timed_wait_any = true;
	device_info.message_callback = on_message;
	auto result = pyrowave_webgpu_device_create(&device_info, &state.device);
	if (result != PYROWAVE_WEBGPU_SUCCESS)
	{
		*error = std::string("no GPU device for PyroWave: ") + pyrowave_webgpu_result_to_string(result);
		return false;
	}

	pyrowave_webgpu_encoder_create_info encoder_info = {};
	encoder_info.device = state.device;
	encoder_info.width = width;
	encoder_info.height = height;
	encoder_info.chroma = PYROWAVE_WEBGPU_CHROMA_SUBSAMPLING_420;
	result = pyrowave_webgpu_encoder_create(&encoder_info, &state.encoder);
	if (result != PYROWAVE_WEBGPU_SUCCESS)
	{
		*error = std::string("PyroWave encoder: ") + pyrowave_webgpu_result_to_string(result);
		release();
		return false;
	}

	pyrowave_webgpu_decoder_create_info decoder_info = {};
	decoder_info.device = state.device;
	decoder_info.width = width;
	decoder_info.height = height;
	decoder_info.chroma = PYROWAVE_WEBGPU_CHROMA_SUBSAMPLING_420;
	result = pyrowave_webgpu_decoder_create(&decoder_info, &state.decoder);
	if (result != PYROWAVE_WEBGPU_SUCCESS)
	{
		*error = std::string("PyroWave decoder: ") + pyrowave_webgpu_result_to_string(result);
		release();
		return false;
	}

	state.width = width;
	state.height = height;
	return true;
}

pyrowave_webgpu_cpu_buffer planes(uint8_t *frame, int width, int height)
{
	pyrowave_webgpu_cpu_buffer cpu = {};
	size_t luma = size_t(width) * height;
	size_t chroma = luma / 4;
	cpu.width = width;
	cpu.height = height;
	cpu.format = PYROWAVE_WEBGPU_CPU_BUFFER_FORMAT_YUV420P;
	cpu.data[0] = frame;
	cpu.data[1] = frame + luma;
	cpu.data[2] = frame + luma + chroma;
	cpu.row_stride_in_bytes[0] = size_t(width);
	cpu.row_stride_in_bytes[1] = size_t(width / 2);
	cpu.row_stride_in_bytes[2] = size_t(width / 2);
	cpu.plane_size_in_bytes[0] = luma;
	cpu.plane_size_in_bytes[1] = chroma;
	cpu.plane_size_in_bytes[2] = chroma;
	return cpu;
}

void fail_frame(exports_ffrwd_av_filter_outcome_t *ret, roundtrip_list_u8_t *frame, const char *what,
                pyrowave_webgpu_result result)
{
	// A frame the codec could not carry is a broken run, not a frame to pass
	// through: say why and stop.
	fprintf(stderr, "%s: %s failed: %s\n", LOG, what, pyrowave_webgpu_result_to_string(result));
	roundtrip_list_u8_free(frame);
	abort();
	(void)ret;
}
}

extern "C" {

void exports_ffrwd_av_filter_describe(exports_ffrwd_av_filter_meta_t *ret)
{
	memset(ret, 0, sizeof(*ret));
	set_string(&ret->name, NAME);
	set_string(&ret->version, VERSION);
	set_string(&ret->params_schema, PARAMS_SCHEMA);
	set_string(&ret->rows_schema, ROWS_SCHEMA);
	ret->pixel_formats.len = 1;
	ret->pixel_formats.ptr = static_cast<roundtrip_string_t *>(malloc(sizeof(roundtrip_string_t)));
	set_string(&ret->pixel_formats.ptr[0], "yuv420p");
}

bool exports_ffrwd_av_filter_init(uint32_t width, uint32_t height, roundtrip_string_t *pix_fmt,
                                  exports_ffrwd_av_filter_stream_info_t *stream_info, roundtrip_string_t *params,
                                  roundtrip_string_t *err)
{
	std::string format = take_string(pix_fmt);
	std::string text = take_string(params);
	exports_ffrwd_av_filter_stream_info_free(stream_info);

	std::string error;
	if (format != "yuv420p")
	{
		set_string(err, "pixel format " + format + ": only yuv420p is supported");
		return false;
	}
	size_t bytes_per_frame = 0;
	if (!parse_params(text, &bytes_per_frame, &error))
	{
		set_string(err, error);
		return false;
	}
	release();
	if (!open(int(width), int(height), &error))
	{
		set_string(err, error);
		return false;
	}
	state.bytes_per_frame = bytes_per_frame;
	state.bitstream.resize(bytes_per_frame + 64 * 1024);
	return true;
}

bool exports_ffrwd_av_filter_set_params(roundtrip_string_t *params, roundtrip_string_t *err)
{
	std::string text = take_string(params);
	std::string error;
	size_t bytes_per_frame = 0;
	if (!parse_params(text, &bytes_per_frame, &error))
	{
		set_string(err, error);
		return false;
	}
	state.bytes_per_frame = bytes_per_frame;
	state.bitstream.resize(bytes_per_frame + 64 * 1024);
	return true;
}

bool exports_ffrwd_av_filter_frame_independent(void)
{
	// PyroWave is intra only: each call encodes and decodes its own frame.
	return true;
}

void exports_ffrwd_av_filter_process(exports_ffrwd_av_filter_frame_info_t *info, roundtrip_list_u8_t *frame,
                                     exports_ffrwd_av_filter_outcome_t *ret)
{
	memset(ret, 0, sizeof(*ret));
	pyrowave_webgpu_cpu_buffer cpu = planes(frame->ptr, state.width, state.height);

	double start = now_ms();
	pyrowave_webgpu_rate_control rate = { state.bytes_per_frame };
	auto result = pyrowave_webgpu_encoder_encode_cpu(state.encoder, &cpu, &rate);
	if (result != PYROWAVE_WEBGPU_SUCCESS)
		return fail_frame(ret, frame, "encode", result);
	result = pyrowave_webgpu_encoder_poll(state.encoder, true);
	if (result != PYROWAVE_WEBGPU_SUCCESS)
		return fail_frame(ret, frame, "encode readback", result);
	pyrowave_webgpu_packet packet = {};
	size_t num_packets = 0;
	result = pyrowave_webgpu_encoder_packetize(state.encoder, &packet, state.bitstream.size(), &num_packets,
	                                           state.bitstream.data(), state.bitstream.size());
	if (result != PYROWAVE_WEBGPU_SUCCESS || num_packets != 1)
		return fail_frame(ret, frame, "packetize", result);
	double encoded = now_ms();

	result = pyrowave_webgpu_decoder_push_packet(state.decoder, state.bitstream.data() + packet.offset, packet.size);
	if (result != PYROWAVE_WEBGPU_SUCCESS)
		return fail_frame(ret, frame, "decoder packet", result);
	// The decoded planes overwrite the input frame's own buffer, which is
	// then handed back as the output.
	result = pyrowave_webgpu_decoder_decode_cpu(state.decoder, &cpu);
	if (result != PYROWAVE_WEBGPU_SUCCESS)
		return fail_frame(ret, frame, "decode", result);
	double decoded = now_ms();

	ret->output.tag = EXPORTS_FFRWD_AV_FILTER_OUTPUT_FRAME;
	ret->output.val.frame = *frame;

	char row[256];
	snprintf(row, sizeof(row), "{\"time\":%.6f,\"bytes\":%zu,\"encode_ms\":%.3f,\"decode_ms\":%.3f}", info->time,
	         packet.size, encoded - start, decoded - encoded);
	ret->rows.len = 1;
	ret->rows.ptr = static_cast<roundtrip_string_t *>(malloc(sizeof(roundtrip_string_t)));
	set_string(&ret->rows.ptr[0], row);
}
}
