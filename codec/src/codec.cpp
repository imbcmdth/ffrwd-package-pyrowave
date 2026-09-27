// The codec module: PyroWave as an ffrwd:av 0.18.0 encoder and decoder, one
// component exporting both (the codec-module world). The work is in
// pyrowave_codec.cpp; this file moves values across the component boundary.
//
// Encoder params: {"bitrate": bits per second, "bytes_per_frame": N}, either
// left out or null for its default; bytes_per_frame wins when set. Every
// packet is one whole frame and a keyframe, with dts = pts. The extradata is
// the 16-byte header pyrowave_codec.hpp describes. The decoder takes no
// params.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "codec.h"
#include "pyrowave_codec.hpp"

using namespace pyrowave_codec;

namespace
{
const char *const VERSION = "0.2.0";

const char *const ENCODER_PARAMS_SCHEMA =
		"{\"type\":\"object\",\"properties\":{"
		"\"bitrate\":{\"type\":\"number\",\"minimum\":1,"
		"\"description\":\"Bits per second; with each frame's duration it sets the size of the frame\"},"
		"\"bytes_per_frame\":{\"type\":\"integer\",\"minimum\":1024,"
		"\"description\":\"Encoded size of every frame, in bytes; wins over bitrate\"}},"
		"\"additionalProperties\":false}";

const char *const DECODER_PARAMS_SCHEMA = "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}";

struct EncoderState
{
	Encoder encoder;
	Budget budget;
	bool open = false;
};

struct DecoderState
{
	Decoder decoder;
	bool open = false;
};

EncoderState encoder_state;
DecoderState decoder_state;

void set_string(codec_string_t *out, const std::string &text)
{
	out->len = text.size();
	out->ptr = static_cast<uint8_t *>(malloc(text.size() ? text.size() : 1));
	memcpy(out->ptr, text.data(), text.size());
}

std::string as_string(const codec_string_t &in)
{
	return std::string(reinterpret_cast<const char *>(in.ptr), in.len);
}

void set_meta(ffrwd_av_types_meta_t *meta, const char *params_schema)
{
	memset(meta, 0, sizeof(*meta));
	set_string(&meta->name, CODEC);
	set_string(&meta->version, VERSION);
	set_string(&meta->params_schema, params_schema);
	set_string(&meta->rows_schema, "");
	meta->pixel_formats.len = 2;
	meta->pixel_formats.ptr = static_cast<codec_string_t *>(malloc(2 * sizeof(codec_string_t)));
	set_string(&meta->pixel_formats.ptr[0], pix_fmt(Chroma::C420));
	set_string(&meta->pixel_formats.ptr[1], pix_fmt(Chroma::C444));
}

std::string at_pts(int64_t pts, const std::string &error)
{
	return "frame at pts " + std::to_string(pts) + ": " + error;
}
}

extern "C" {

// -- encoder ------------------------------------------------------------------

void exports_ffrwd_av_encoder_describe(exports_ffrwd_av_encoder_encoder_meta_t *ret)
{
	memset(ret, 0, sizeof(*ret));
	set_meta(&ret->meta, ENCODER_PARAMS_SCHEMA);
	set_string(&ret->codec, CODEC);
	set_string(&ret->fourcc, FOURCC);
	// Intra only: every frame leaves in the call that brings it, and decode
	// order is presentation order.
	ret->delay = 0;
	ret->decode_delay = 0;
	ret->frame_samples = 0;
}

bool exports_ffrwd_av_encoder_init(exports_ffrwd_av_encoder_format_t *format,
                                   exports_ffrwd_av_encoder_stream_info_t *info, codec_string_t *params,
                                   exports_ffrwd_av_encoder_coded_stream_t *ret, codec_string_t *err)
{
	std::string text = as_string(*params);
	codec_string_free(params);
	ffrwd_av_types_rational_t time_base = info->time_base;
	exports_ffrwd_av_encoder_stream_info_free(info);

	std::string error;
	StreamShape shape;
	EncoderParams parsed;
	encoder_state.open = false;
	if (format->tag != FFRWD_AV_TYPES_FORMAT_VIDEO)
	{
		error = "PyroWave codes video, not audio";
	}
	else if (!chroma_of(as_string(format->val.video.pix_fmt), &shape.chroma))
	{
		error = "pixel format " + as_string(format->val.video.pix_fmt) + ": PyroWave takes yuv420p or yuv444p";
	}
	else
	{
		shape.width = format->val.video.width;
		shape.height = format->val.video.height;
		if (parse_encoder_params(text, &parsed, &error))
			encoder_state.open = encoder_state.encoder.open(shape, &error);
	}
	if (!encoder_state.open)
	{
		exports_ffrwd_av_encoder_format_free(format);
		set_string(err, error);
		return false;
	}

	// The budget never needs more than twice the raw frame: past that the
	// GPU buffers would only grow.
	size_t max_bytes = 2 * frame_size(shape);
	if (max_bytes < 64 * 1024)
		max_bytes = 64 * 1024;
	encoder_state.budget.init(parsed, time_base.num, time_base.den, max_bytes);

	memset(ret, 0, sizeof(*ret));
	set_string(&ret->codec, CODEC);
	ret->time_base = time_base;
	ret->format.tag = FFRWD_AV_TYPES_CODED_FORMAT_VIDEO;
	ret->format.val.video.width = shape.width;
	ret->format.val.video.height = shape.height;
	ret->format.val.video.sample_aspect_ratio.is_some = false;
	// The colorimetry passes through as the input declared it; the move
	// leaves the format without it, so freeing the format keeps it.
	ret->format.val.video.color = format->val.video.color;
	format->val.video.color.is_some = false;
	ret->extradata.len = EXTRADATA_SIZE;
	ret->extradata.ptr = static_cast<uint8_t *>(malloc(EXTRADATA_SIZE));
	write_extradata(shape, ret->extradata.ptr);
	ret->profile.is_some = false;
	ret->level.is_some = false;
	exports_ffrwd_av_encoder_format_free(format);
	return true;
}

bool exports_ffrwd_av_encoder_encode(exports_ffrwd_av_encoder_list_raw_frame_t *frames, bool last,
                                     exports_ffrwd_av_encoder_list_packet_t *ret, codec_string_t *err)
{
	(void)last; // nothing is held back, so the last call has nothing more to give
	ret->len = 0;
	ret->ptr = nullptr;
	if (!encoder_state.open)
	{
		exports_ffrwd_av_encoder_list_raw_frame_free(frames);
		set_string(err, "encode before a successful init");
		return false;
	}

	size_t count = frames->len;
	if (count)
		ret->ptr = static_cast<exports_ffrwd_av_encoder_packet_t *>(malloc(count * sizeof(*ret->ptr)));
	std::string error;
	for (size_t i = 0; i < count; i++)
	{
		const auto &frame = frames->ptr[i];
		int64_t duration = frame.duration.is_some ? frame.duration.val : -1;
		int64_t gap = i + 1 < count ? frames->ptr[i + 1].pts - frame.pts : -1;
		size_t budget = 0;
		if (!encoder_state.budget.next(duration, gap, &budget, &error))
		{
			error = at_pts(frame.pts, error);
			break;
		}
		uint8_t *data = static_cast<uint8_t *>(malloc(Encoder::capacity(budget)));
		size_t written = 0;
		if (!encoder_state.encoder.encode(frame.data.ptr, frame.data.len, budget, data, &written, &error))
		{
			free(data);
			error = at_pts(frame.pts, error);
			break;
		}
		if (uint8_t *shrunk = static_cast<uint8_t *>(realloc(data, written ? written : 1)))
			data = shrunk;

		auto &packet = ret->ptr[ret->len++];
		packet.pts = frame.pts;
		packet.dts.is_some = true;
		packet.dts.val = frame.pts;
		packet.duration = frame.duration;
		packet.keyframe = true;
		packet.data.ptr = data;
		packet.data.len = written;
	}
	exports_ffrwd_av_encoder_list_raw_frame_free(frames);

	if (!error.empty())
	{
		// The list frees its packets and itself, but not an array it
		// holds none of.
		if (ret->len)
			exports_ffrwd_av_encoder_list_packet_free(ret);
		else
			free(ret->ptr);
		ret->len = 0;
		ret->ptr = nullptr;
		set_string(err, error);
		return false;
	}
	return true;
}

// -- decoder ------------------------------------------------------------------

void exports_ffrwd_av_decoder_describe(exports_ffrwd_av_decoder_decoder_meta_t *ret)
{
	memset(ret, 0, sizeof(*ret));
	set_meta(&ret->meta, DECODER_PARAMS_SCHEMA);
	ret->fourccs.len = 1;
	ret->fourccs.ptr = static_cast<codec_string_t *>(malloc(sizeof(codec_string_t)));
	set_string(&ret->fourccs.ptr[0], FOURCC);
	ret->delay = 0;
}

bool exports_ffrwd_av_decoder_init(exports_ffrwd_av_decoder_coded_stream_t *coded,
                                   exports_ffrwd_av_decoder_stream_info_t *info, codec_string_t *params,
                                   exports_ffrwd_av_decoder_format_t *ret, codec_string_t *err)
{
	std::string text = as_string(*params);
	codec_string_free(params);
	exports_ffrwd_av_decoder_stream_info_free(info);

	std::string error;
	StreamShape shape;
	decoder_state.open = false;
	if (coded->format.tag != FFRWD_AV_TYPES_CODED_FORMAT_VIDEO)
	{
		error = "PyroWave decodes video only";
	}
	else if (coded->extradata.len)
	{
		if (read_extradata(coded->extradata.ptr, coded->extradata.len, &shape, &error))
		{
			uint32_t width = coded->format.val.video.width;
			uint32_t height = coded->format.val.video.height;
			if (width && height && (width != shape.width || height != shape.height))
				error = "the stream declares " + std::to_string(width) + "x" + std::to_string(height) +
				        " and its PyroWave header " + std::to_string(shape.width) + "x" +
				        std::to_string(shape.height);
		}
	}
	else
	{
		// No header: a container that dropped it. The geometry is the
		// stream's, and 4:2:0 the guess; a 4:4:4 stream then fails at its
		// first packet, whose own header says which it is.
		shape.width = coded->format.val.video.width;
		shape.height = coded->format.val.video.height;
		shape.chroma = Chroma::C420;
	}
	if (error.empty() && parse_decoder_params(text, &error))
		decoder_state.open = decoder_state.decoder.open(shape, &error);
	if (!decoder_state.open)
	{
		exports_ffrwd_av_decoder_coded_stream_free(coded);
		set_string(err, error);
		return false;
	}

	memset(ret, 0, sizeof(*ret));
	ret->tag = FFRWD_AV_TYPES_FORMAT_VIDEO;
	ret->val.video.width = shape.width;
	ret->val.video.height = shape.height;
	set_string(&ret->val.video.pix_fmt, pix_fmt(shape.chroma));
	ret->val.video.color = coded->format.val.video.color;
	coded->format.val.video.color.is_some = false;
	exports_ffrwd_av_decoder_coded_stream_free(coded);
	return true;
}

bool exports_ffrwd_av_decoder_decode(exports_ffrwd_av_decoder_list_packet_t *packets, bool last,
                                     exports_ffrwd_av_decoder_list_raw_frame_t *ret, codec_string_t *err)
{
	(void)last; // every packet is a whole frame, so nothing is ever held
	ret->len = 0;
	ret->ptr = nullptr;
	if (!decoder_state.open)
	{
		exports_ffrwd_av_decoder_list_packet_free(packets);
		set_string(err, "decode before a successful init");
		return false;
	}

	size_t count = packets->len;
	size_t size = frame_size(decoder_state.decoder.shape());
	if (count)
		ret->ptr = static_cast<exports_ffrwd_av_decoder_raw_frame_t *>(malloc(count * sizeof(*ret->ptr)));
	std::string error;
	for (size_t i = 0; i < count; i++)
	{
		const auto &packet = packets->ptr[i];
		uint8_t *data = static_cast<uint8_t *>(malloc(size));
		if (!decoder_state.decoder.decode(packet.data.ptr, packet.data.len, data, &error))
		{
			free(data);
			error = "packet at pts " + std::to_string(packet.pts) + ": " + error;
			break;
		}
		auto &frame = ret->ptr[ret->len++];
		frame.pts = packet.pts;
		frame.duration = packet.duration;
		frame.data.ptr = data;
		frame.data.len = size;
	}
	exports_ffrwd_av_decoder_list_packet_free(packets);

	if (!error.empty())
	{
		if (ret->len)
			exports_ffrwd_av_decoder_list_raw_frame_free(ret);
		else
			free(ret->ptr);
		ret->len = 0;
		ret->ptr = nullptr;
		set_string(err, error);
		return false;
	}
	return true;
}
}
