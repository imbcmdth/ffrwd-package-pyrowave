// Native test of the codec module. It links the same codec.cpp,
// pyrowave_codec.cpp and generated bindings the component is built from, and
// calls the exports the way the sidecar will: describe, init, then encode and
// decode over lists of frames and packets, handing each call arguments it
// owns and freeing what it answers.
//
//   pyrowave-codec-test checks
//       The describes, and init and encode refusing what they should.
//   pyrowave-codec-test roundtrip <in.y4m> <out.y4m> [options]
//       Encodes every frame of a y4m, decodes the packets again and writes
//       what the decoder produced, with the sizes and timings on stdout.
//       --params JSON     encoder params (default {})
//       --batch N         frames per encode call (default 1)
//       --tb NUM/DEN      the stream's time base (default: the y4m's frame
//                         period, so one frame is one tick)
//       --no-duration     frames carry no duration
//       --packets FILE    write the packets, each as a u32 size and the bytes

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <string>
#include <vector>

#include "codec.h"
#include "pyrowave_codec.hpp"

extern "C" void __component_type_object_force_link_codec(void)
{
}

namespace
{
int failures = 0;

void check(bool ok, const char *what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failures++;
}

codec_string_t str(const std::string &text)
{
	codec_string_t out;
	out.len = text.size();
	out.ptr = static_cast<uint8_t *>(malloc(text.size() ? text.size() : 1));
	memcpy(out.ptr, text.data(), text.size());
	return out;
}

std::string text(const codec_string_t &s)
{
	return std::string(reinterpret_cast<const char *>(s.ptr), s.len);
}

ffrwd_av_types_format_t video_format(uint32_t width, uint32_t height, const char *pix_fmt)
{
	ffrwd_av_types_format_t format;
	memset(&format, 0, sizeof(format));
	format.tag = FFRWD_AV_TYPES_FORMAT_VIDEO;
	format.val.video.width = width;
	format.val.video.height = height;
	format.val.video.pix_fmt = str(pix_fmt);
	format.val.video.color.is_some = false;
	return format;
}

ffrwd_av_types_stream_info_t stream_info(int32_t num, int32_t den)
{
	ffrwd_av_types_stream_info_t info;
	memset(&info, 0, sizeof(info));
	info.index = 0;
	info.kind = str("video");
	info.codec = str("rawvideo");
	info.time_base.num = num;
	info.time_base.den = den;
	return info;
}

// Init an encoder; on success `coded` holds what it answered.
bool encoder_init(uint32_t width, uint32_t height, const char *pix_fmt, int32_t num, int32_t den,
                  const std::string &params, ffrwd_av_types_coded_stream_t *coded, std::string *error)
{
	error->clear();
	auto format = video_format(width, height, pix_fmt);
	auto info = stream_info(num, den);
	auto p = str(params);
	codec_string_t err = {};
	if (exports_ffrwd_av_encoder_init(&format, &info, &p, coded, &err))
		return true;
	*error = text(err);
	codec_string_free(&err);
	return false;
}

bool decoder_init(ffrwd_av_types_coded_stream_t *coded, const std::string &params, ffrwd_av_types_format_t *format,
                  std::string *error)
{
	error->clear();
	auto info = stream_info(coded->time_base.num, coded->time_base.den);
	auto p = str(params);
	codec_string_t err = {};
	if (exports_ffrwd_av_decoder_init(coded, &info, &p, format, &err))
		return true;
	*error = text(err);
	codec_string_free(&err);
	return false;
}

ffrwd_av_types_coded_stream_t coded_stream(uint32_t width, uint32_t height, const std::vector<uint8_t> &extradata)
{
	ffrwd_av_types_coded_stream_t coded;
	memset(&coded, 0, sizeof(coded));
	coded.codec = str("PYRW");
	coded.time_base.num = 1;
	coded.time_base.den = 30;
	coded.format.tag = FFRWD_AV_TYPES_CODED_FORMAT_VIDEO;
	coded.format.val.video.width = width;
	coded.format.val.video.height = height;
	coded.extradata.len = extradata.size();
	coded.extradata.ptr = static_cast<uint8_t *>(malloc(extradata.size() ? extradata.size() : 1));
	memcpy(coded.extradata.ptr, extradata.data(), extradata.size());
	return coded;
}

// One call to encode with `count` copies of `frame`; answers the error, or
// empty on success, with the packet sizes.
std::string encode_frames(const std::vector<uint8_t> &frame, size_t count, int64_t first_pts, bool durations,
                          std::vector<size_t> *sizes)
{
	exports_ffrwd_av_encoder_list_raw_frame_t frames;
	frames.len = count;
	frames.ptr = static_cast<ffrwd_av_types_raw_frame_t *>(malloc(count ? count * sizeof(*frames.ptr) : 1));
	for (size_t i = 0; i < count; i++)
	{
		auto &f = frames.ptr[i];
		f.pts = first_pts + int64_t(i);
		f.duration.is_some = durations;
		f.duration.val = 1;
		f.data.len = frame.size();
		f.data.ptr = static_cast<uint8_t *>(malloc(frame.size() ? frame.size() : 1));
		memcpy(f.data.ptr, frame.data(), frame.size());
	}
	exports_ffrwd_av_encoder_list_packet_t packets = {};
	codec_string_t err = {};
	if (!exports_ffrwd_av_encoder_encode(&frames, false, &packets, &err))
	{
		std::string error = text(err);
		codec_string_free(&err);
		return error;
	}
	for (size_t i = 0; sizes && i < packets.len; i++)
		sizes->push_back(packets.ptr[i].data.len);
	exports_ffrwd_av_encoder_list_packet_free(&packets);
	return std::string();
}

bool contains(const std::string &haystack, const char *needle)
{
	return haystack.find(needle) != std::string::npos;
}

int run_checks()
{
	{
		exports_ffrwd_av_encoder_encoder_meta_t meta;
		exports_ffrwd_av_encoder_describe(&meta);
		check(text(meta.codec) == "pyrowave" && text(meta.fourcc) == "PYRW", "encoder describe: pyrowave, PYRW");
		check(meta.delay == 0 && meta.decode_delay == 0 && meta.frame_samples == 0, "encoder describe: delays 0");
		check(meta.meta.pixel_formats.len == 2 && text(meta.meta.pixel_formats.ptr[0]) == "yuv420p" &&
		      text(meta.meta.pixel_formats.ptr[1]) == "yuv444p" && meta.meta.sample_formats.len == 0,
		      "encoder describe: yuv420p, yuv444p");
		check(contains(text(meta.meta.params_schema), "\"bitrate\"") &&
		      contains(text(meta.meta.params_schema), "\"bytes_per_frame\""),
		      "encoder describe: params schema names bitrate and bytes_per_frame");
		exports_ffrwd_av_encoder_encoder_meta_free(&meta);

		exports_ffrwd_av_decoder_decoder_meta_t dmeta;
		exports_ffrwd_av_decoder_describe(&dmeta);
		check(dmeta.fourccs.len == 1 && text(dmeta.fourccs.ptr[0]) == "PYRW" && dmeta.delay == 0,
		      "decoder describe: reads PYRW, delay 0");
		exports_ffrwd_av_decoder_decoder_meta_free(&dmeta);
	}

	ffrwd_av_types_coded_stream_t coded;
	std::string error;
	check(!encoder_init(256, 144, "rgb24", 1, 30, "", &coded, &error) && contains(error, "rgb24"),
	      ("encoder refuses rgb24: " + error).c_str());
	check(!encoder_init(255, 144, "yuv420p", 1, 30, "", &coded, &error) && contains(error, "even"),
	      ("encoder refuses an odd 4:2:0 width: " + error).c_str());
	check(!encoder_init(256, 144, "yuv420p", 1, 30, "{\"bogus\":1}", &coded, &error) && contains(error, "bogus"),
	      ("encoder refuses an unknown param: " + error).c_str());
	check(!encoder_init(256, 144, "yuv420p", 1, 30, "{\"bytes_per_frame\":100}", &coded, &error),
	      ("encoder refuses bytes_per_frame 100: " + error).c_str());
	check(!encoder_init(256, 144, "yuv420p", 1, 30, "[1]", &coded, &error),
	      ("encoder refuses params that are not an object: " + error).c_str());

	// A frame: a gradient, so the budget is spent on something.
	const uint32_t w = 256, h = 144;
	std::vector<uint8_t> frame(w * h * 3 / 2);
	for (size_t i = 0; i < frame.size(); i++)
		frame[i] = uint8_t((i * 7) ^ (i >> 5));

	bool ok = encoder_init(w, h, "yuv420p", 1, 30, "{\"bitrate\":1200000,\"bytes_per_frame\":null}", &coded, &error);
	check(ok, ("encoder init, bitrate 1200000 at 1/30: " + error).c_str());
	if (ok)
	{
		check(text(coded.codec) == "pyrowave" && coded.time_base.num == 1 && coded.time_base.den == 30,
		      "coded stream: pyrowave at 1/30");
		check(coded.format.tag == FFRWD_AV_TYPES_CODED_FORMAT_VIDEO && coded.format.val.video.width == w &&
		      coded.format.val.video.height == h, "coded stream: 256x144");
		const uint8_t want[16] = { 'P', 'Y', 'R', 'W', 1, 16, 0, 0, 0, 1, 0, 0, 144, 0, 0, 0 };
		check(coded.extradata.len == 16 && memcmp(coded.extradata.ptr, want, 16) == 0,
		      "extradata: PYRW v1, 16 bytes, 4:2:0, 256x144");
		ffrwd_av_types_coded_stream_free(&coded);

		std::vector<size_t> sizes;
		error = encode_frames(frame, 3, 0, true, &sizes);
		// 1200000 / 8 / 30 = 5000 bytes a frame.
		check(error.empty() && sizes.size() == 3 && sizes[0] <= 5000 && sizes[0] > 2000,
		      ("bitrate 1200000 at 30 fps: a frame is at most 5000 bytes, got " +
		       (sizes.empty() ? std::string("none") : std::to_string(sizes[0])) + " " + error).c_str());
		sizes.clear();
		error = encode_frames(frame, 3, 3, false, &sizes);
		check(error.empty() && sizes.size() == 3, ("frames with no duration take the gap or 1/30: " + error).c_str());
		error = encode_frames(std::vector<uint8_t>(frame.begin(), frame.end() - 1), 1, 6, true, nullptr);
		check(contains(error, "bytes, not"), ("encode refuses a frame one byte short: " + error).c_str());
	}

	ok = encoder_init(w, h, "yuv420p", 1, 90000, "{\"bitrate\":1200000}", &coded, &error);
	if (ok)
	{
		ffrwd_av_types_coded_stream_free(&coded);
		error = encode_frames(frame, 1, 0, false, nullptr);
		check(contains(error, "bytes_per_frame"),
		      ("a lone frame with no duration at 1/90000 asks for bytes_per_frame: " + error).c_str());
		std::vector<size_t> sizes;
		error = encode_frames(frame, 1, 0, true, &sizes);
		// 3000 ticks of 1/90000 is 1/30 s; one tick is 1/90000 s, which floors
		// to the 1024 minimum.
		check(error.empty() && sizes.size() == 1 && sizes[0] <= 1024,
		      ("a 1-tick frame at 1/90000 gets the 1024-byte minimum: " + error).c_str());
	}

	ok = encoder_init(w, h, "yuv420p", 1, 30, "{\"bytes_per_frame\":3000,\"bitrate\":1}", &coded, &error);
	if (ok)
	{
		ffrwd_av_types_coded_stream_free(&coded);
		std::vector<size_t> sizes;
		error = encode_frames(frame, 2, 0, true, &sizes);
		check(error.empty() && sizes.size() == 2 && sizes[0] <= 3000 && sizes[0] > 2000,
		      "bytes_per_frame 3000 wins over bitrate 1");
	}

	ffrwd_av_types_format_t format;
	std::vector<uint8_t> header = { 'P', 'Y', 'R', 'W', 1, 16, 1, 0, 0, 1, 0, 0, 144, 0, 0, 0 };
	auto cs = coded_stream(w, h, header);
	check(decoder_init(&cs, "{}", &format, &error) && text(format.val.video.pix_fmt) == "yuv444p" &&
	      format.val.video.width == w && format.val.video.height == h,
	      ("decoder reads a 4:4:4 header: " + error).c_str());
	ffrwd_av_types_format_free(&format);

	cs = coded_stream(w, h, {});
	check(decoder_init(&cs, "", &format, &error) && text(format.val.video.pix_fmt) == "yuv420p",
	      ("decoder takes a stream with no extradata as 4:2:0: " + error).c_str());
	ffrwd_av_types_format_free(&format);

	auto bad = header;
	bad[0] = 'X';
	cs = coded_stream(w, h, bad);
	check(!decoder_init(&cs, "", &format, &error), ("decoder refuses a bad magic: " + error).c_str());
	bad = header;
	bad[4] = 2;
	cs = coded_stream(w, h, bad);
	check(!decoder_init(&cs, "", &format, &error), ("decoder refuses version 2: " + error).c_str());
	cs = coded_stream(w, h, std::vector<uint8_t>(header.begin(), header.begin() + 12));
	check(!decoder_init(&cs, "", &format, &error), ("decoder refuses a cut header: " + error).c_str());
	cs = coded_stream(w + 2, h, header);
	check(!decoder_init(&cs, "", &format, &error), ("decoder refuses a geometry mismatch: " + error).c_str());
	cs = coded_stream(w, h, header);
	check(!decoder_init(&cs, "{\"x\":1}", &format, &error), ("decoder refuses params: " + error).c_str());

	// A packet from a 4:2:0 encoder into a decoder opened 4:2:0, then a
	// garbage packet.
	ok = encoder_init(w, h, "yuv420p", 1, 30, "{\"bytes_per_frame\":4000}", &coded, &error);
	if (ok && decoder_init(&coded, "", &format, &error))
	{
		ffrwd_av_types_format_free(&format);
		exports_ffrwd_av_decoder_list_packet_t packets;
		packets.len = 1;
		packets.ptr = static_cast<ffrwd_av_types_packet_t *>(malloc(sizeof(ffrwd_av_types_packet_t)));
		memset(packets.ptr, 0, sizeof(ffrwd_av_types_packet_t));
		packets.ptr[0].pts = 7;
		packets.ptr[0].data.len = 64;
		packets.ptr[0].data.ptr = static_cast<uint8_t *>(malloc(64));
		memset(packets.ptr[0].data.ptr, 0xa5, 64);
		exports_ffrwd_av_decoder_list_raw_frame_t frames = {};
		codec_string_t err = {};
		bool decoded = exports_ffrwd_av_decoder_decode(&packets, false, &frames, &err);
		error = decoded ? std::string() : text(err);
		check(!decoded && contains(error, "pts 7"), ("decoder refuses a garbage packet: " + error).c_str());
		if (!decoded)
			codec_string_free(&err);
		else
			exports_ffrwd_av_decoder_list_raw_frame_free(&frames);
	}

	pyrowave_codec::release_shared_device();
	printf("%s: %d failed\n", failures ? "FAILED" : "passed", failures);
	return failures ? 1 : 0;
}

struct Y4m
{
	FILE *file = nullptr;
	std::string header;
	uint32_t width = 0;
	uint32_t height = 0;
	int fps_num = 0;
	int fps_den = 0;
	std::string pix_fmt;
	size_t frame_bytes = 0;
};

bool open_y4m(const char *path, Y4m *y4m)
{
	y4m->file = fopen(path, "rb");
	if (!y4m->file)
		return false;
	int c;
	while ((c = fgetc(y4m->file)) != EOF && c != '\n')
		y4m->header.push_back(char(c));
	if (y4m->header.compare(0, 9, "YUV4MPEG2") != 0)
		return false;
	std::string chroma = "420";
	size_t pos = 9;
	while (pos < y4m->header.size())
	{
		while (pos < y4m->header.size() && y4m->header[pos] == ' ')
			pos++;
		size_t end = y4m->header.find(' ', pos);
		if (end == std::string::npos)
			end = y4m->header.size();
		std::string token = y4m->header.substr(pos, end - pos);
		pos = end;
		if (token.empty())
			continue;
		if (token[0] == 'W')
			y4m->width = uint32_t(atoi(token.c_str() + 1));
		else if (token[0] == 'H')
			y4m->height = uint32_t(atoi(token.c_str() + 1));
		else if (token[0] == 'F')
			sscanf(token.c_str() + 1, "%d:%d", &y4m->fps_num, &y4m->fps_den);
		else if (token[0] == 'C')
			chroma = token.substr(1);
	}
	y4m->pix_fmt = chroma.compare(0, 3, "444") == 0 ? "yuv444p" : chroma.compare(0, 3, "420") == 0 ? "yuv420p" : "";
	size_t luma = size_t(y4m->width) * y4m->height;
	y4m->frame_bytes = y4m->pix_fmt == "yuv444p" ? luma * 3 : luma * 3 / 2;
	return !y4m->pix_fmt.empty() && y4m->width && y4m->height;
}

bool read_frame(Y4m *y4m, std::vector<uint8_t> *frame)
{
	char tag[6];
	if (fread(tag, 1, 5, y4m->file) != 5 || memcmp(tag, "FRAME", 5) != 0)
		return false;
	int c;
	while ((c = fgetc(y4m->file)) != EOF && c != '\n')
		;
	frame->resize(y4m->frame_bytes);
	return fread(frame->data(), 1, frame->size(), y4m->file) == frame->size();
}

double now_ms()
{
	using namespace std::chrono;
	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

int run_roundtrip(int argc, char **argv)
{
	if (argc < 4)
	{
		fprintf(stderr, "usage: %s roundtrip <in.y4m> <out.y4m> [--params JSON] [--batch N] [--tb NUM/DEN] "
		                "[--no-duration] [--packets FILE]\n", argv[0]);
		return 2;
	}
	std::string params = "{}";
	size_t batch = 1;
	int tb_num = 0, tb_den = 0;
	bool durations = true;
	const char *packets_path = nullptr;
	for (int i = 4; i < argc; i++)
	{
		std::string arg = argv[i];
		if (arg == "--params" && i + 1 < argc)
			params = argv[++i];
		else if (arg == "--batch" && i + 1 < argc)
			batch = size_t(atoi(argv[++i]));
		else if (arg == "--tb" && i + 1 < argc)
			sscanf(argv[++i], "%d/%d", &tb_num, &tb_den);
		else if (arg == "--no-duration")
			durations = false;
		else if (arg == "--packets" && i + 1 < argc)
			packets_path = argv[++i];
		else
		{
			fprintf(stderr, "unknown argument %s\n", arg.c_str());
			return 2;
		}
	}
	if (batch < 1)
		batch = 1;

	Y4m in;
	if (!open_y4m(argv[2], &in))
	{
		fprintf(stderr, "cannot read %s as a 4:2:0 or 4:4:4 y4m\n", argv[2]);
		return 1;
	}
	if (!tb_num)
	{
		tb_num = in.fps_den;
		tb_den = in.fps_num;
	}
	// A frame's duration in ticks: one frame period in the time base.
	int64_t frame_ticks = int64_t(double(in.fps_den) / in.fps_num * tb_den / tb_num + 0.5);
	if (frame_ticks < 1)
		frame_ticks = 1;

	ffrwd_av_types_coded_stream_t coded;
	std::string error;
	double t0 = now_ms();
	if (!encoder_init(in.width, in.height, in.pix_fmt.c_str(), tb_num, tb_den, params, &coded, &error))
	{
		fprintf(stderr, "encoder init: %s\n", error.c_str());
		return 1;
	}
	double t_enc_init = now_ms() - t0;
	std::string extradata;
	for (size_t i = 0; i < coded.extradata.len; i++)
	{
		char hex[3];
		snprintf(hex, sizeof(hex), "%02x", coded.extradata.ptr[i]);
		extradata += hex;
	}
	ffrwd_av_types_format_t format;
	t0 = now_ms();
	if (!decoder_init(&coded, "", &format, &error))
	{
		fprintf(stderr, "decoder init: %s\n", error.c_str());
		return 1;
	}
	double t_dec_init = now_ms() - t0;
	std::string out_pix_fmt = text(format.val.video.pix_fmt);
	if (format.val.video.width != in.width || format.val.video.height != in.height || out_pix_fmt != in.pix_fmt)
	{
		fprintf(stderr, "decoder init answered %ux%u %s\n", format.val.video.width, format.val.video.height,
		        out_pix_fmt.c_str());
		return 1;
	}
	ffrwd_av_types_format_free(&format);

	FILE *out = fopen(argv[3], "wb");
	FILE *packets_file = packets_path ? fopen(packets_path, "wb") : nullptr;
	fprintf(out, "%s\n", in.header.c_str());

	size_t frames_done = 0, bytes = 0, min_bytes = SIZE_MAX, max_bytes = 0;
	double encode_ms = 0, decode_ms = 0;
	bool finished = false;
	std::vector<uint8_t> frame;
	while (!finished)
	{
		// One call's frames.
		std::vector<std::vector<uint8_t>> pending;
		while (pending.size() < batch && read_frame(&in, &frame))
			pending.push_back(frame);
		finished = pending.size() < batch;
		bool last = finished;

		exports_ffrwd_av_encoder_list_raw_frame_t frames;
		frames.len = pending.size();
		frames.ptr = static_cast<ffrwd_av_types_raw_frame_t *>(malloc(pending.size() * sizeof(*frames.ptr) + 1));
		for (size_t i = 0; i < pending.size(); i++)
		{
			auto &f = frames.ptr[i];
			f.pts = int64_t(frames_done + i) * frame_ticks;
			f.duration.is_some = durations;
			f.duration.val = frame_ticks;
			f.data.len = pending[i].size();
			f.data.ptr = static_cast<uint8_t *>(malloc(pending[i].size()));
			memcpy(f.data.ptr, pending[i].data(), pending[i].size());
		}
		size_t sent = frames.len;
		exports_ffrwd_av_encoder_list_packet_t packets = {};
		codec_string_t err = {};
		t0 = now_ms();
		if (!exports_ffrwd_av_encoder_encode(&frames, last, &packets, &err))
		{
			fprintf(stderr, "encode: %s\n", text(err).c_str());
			return 1;
		}
		encode_ms += now_ms() - t0;
		if (packets.len != sent)
		{
			fprintf(stderr, "encode answered %zu packets for %zu frames\n", packets.len, sent);
			return 1;
		}
		for (size_t i = 0; i < packets.len; i++)
		{
			const auto &p = packets.ptr[i];
			int64_t pts = int64_t(frames_done + i) * frame_ticks;
			if (p.pts != pts || !p.dts.is_some || p.dts.val != pts || !p.keyframe ||
			    p.duration.is_some != durations || (durations && p.duration.val != frame_ticks))
			{
				fprintf(stderr, "packet %zu: pts %lld dts %lld keyframe %d duration %lld\n", frames_done + i,
				        (long long)p.pts, (long long)p.dts.val, int(p.keyframe), (long long)p.duration.val);
				return 1;
			}
			bytes += p.data.len;
			min_bytes = p.data.len < min_bytes ? p.data.len : min_bytes;
			max_bytes = p.data.len > max_bytes ? p.data.len : max_bytes;
			if (packets_file)
			{
				uint32_t size = uint32_t(p.data.len);
				fwrite(&size, 4, 1, packets_file);
				fwrite(p.data.ptr, 1, p.data.len, packets_file);
			}
		}

		// The encoder's packets go to the decoder as they are, which owns
		// them from here.
		exports_ffrwd_av_decoder_list_packet_t handed = { packets.ptr, packets.len };
		exports_ffrwd_av_decoder_list_raw_frame_t decoded = {};
		t0 = now_ms();
		if (!exports_ffrwd_av_decoder_decode(&handed, last, &decoded, &err))
		{
			fprintf(stderr, "decode: %s\n", text(err).c_str());
			return 1;
		}
		decode_ms += now_ms() - t0;
		if (decoded.len != sent)
		{
			fprintf(stderr, "decode answered %zu frames for %zu packets\n", decoded.len, sent);
			return 1;
		}
		for (size_t i = 0; i < decoded.len; i++)
		{
			const auto &f = decoded.ptr[i];
			int64_t pts = int64_t(frames_done + i) * frame_ticks;
			if (f.pts != pts || f.data.len != in.frame_bytes || f.duration.is_some != durations)
			{
				fprintf(stderr, "frame %zu: pts %lld, %zu bytes\n", frames_done + i, (long long)f.pts, f.data.len);
				return 1;
			}
			fputs("FRAME\n", out);
			fwrite(f.data.ptr, 1, f.data.len, out);
		}
		exports_ffrwd_av_decoder_list_raw_frame_free(&decoded);
		frames_done += sent;
	}
	fclose(out);
	if (packets_file)
		fclose(packets_file);
	fclose(in.file);
	pyrowave_codec::release_shared_device();

	double seconds = double(frames_done) * in.fps_den / in.fps_num;
	printf("{\"frames\":%zu,\"width\":%u,\"height\":%u,\"pix_fmt\":\"%s\",\"time_base\":\"%d/%d\","
	       "\"params\":%s,\"batch\":%zu,\"extradata\":\"%s\",\"bytes\":%zu,\"min_bytes\":%zu,\"max_bytes\":%zu,"
	       "\"mean_bytes\":%.1f,\"bitrate\":%.0f,\"encode_init_ms\":%.1f,\"decode_init_ms\":%.1f,"
	       "\"encode_ms_per_frame\":%.3f,\"decode_ms_per_frame\":%.3f}\n",
	       frames_done, in.width, in.height, in.pix_fmt.c_str(), tb_num, tb_den, params.c_str(), batch,
	       extradata.c_str(), bytes, frames_done ? min_bytes : 0, max_bytes,
	       frames_done ? double(bytes) / frames_done : 0.0, seconds > 0 ? bytes * 8.0 / seconds : 0.0, t_enc_init,
	       t_dec_init, frames_done ? encode_ms / frames_done : 0.0, frames_done ? decode_ms / frames_done : 0.0);
	return 0;
}
}

int main(int argc, char **argv)
{
	if (argc >= 2 && strcmp(argv[1], "checks") == 0)
		return run_checks();
	if (argc >= 2 && strcmp(argv[1], "roundtrip") == 0)
		return run_roundtrip(argc, argv);
	fprintf(stderr, "usage: %s checks | roundtrip <in.y4m> <out.y4m> [options]\n", argv[0]);
	return 2;
}
