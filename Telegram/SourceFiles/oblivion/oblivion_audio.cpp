/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_audio.h"

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "ffmpeg/ffmpeg_bytes_io_wrap.h"
#include "ffmpeg/ffmpeg_utility.h"
#include "media/audio/media_audio.h"
#include "media/audio/media_audio_track.h"
#include "media/player/media_player_instance.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QMutex>
#include <QtCore/QtEndian>

#include <al.h>
#include <alc.h>
#include <alext.h>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
} // extern "C"

#include <array>
#include <cmath>
#include <complex>
#include <deque>
#include <limits>
#include <string>

namespace Oblivion::Audio {
namespace {

using FFmpeg::AvErrorWrap;
using FFmpeg::LogError;

constexpr auto kPi = 3.14159265358979323846;
constexpr auto kMinRate = 4000;
constexpr auto kMaxRate = 384000;
constexpr auto kMaxChannels = 8;
constexpr auto kMaxDecodedSamples = int64(1) << 28;
constexpr auto kMaxDecodeErrors = 64;
constexpr auto kSafetyPeak = 0.99;
constexpr auto kTailThreshold = 3e-4; // -70 dB
constexpr auto kMicroFade = crl::time(3);
constexpr auto kProcessChunk = 8192;

// Media::Capture records voice messages exactly like this.
constexpr auto kVoiceRate = Media::Player::kDefaultFrequency;
constexpr auto kVoiceBitrate = 32000;
constexpr auto kOpusRate = 48000;
constexpr auto kOpusBitrateMono = 64000;
constexpr auto kOpusBitrateStereo = 128000;
constexpr auto kAacBitrateMono = 128000;
constexpr auto kAacBitrateStereo = 192000;

// Freeverb tuning for 44.1 kHz.
constexpr auto kFreeverbRate = 44100.;
constexpr auto kCombTuning = std::array<int, 8>{
	1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
constexpr auto kAllpassTuning = std::array<int, 4>{ 556, 441, 341, 225 };
constexpr auto kStereoSpread = 23;
constexpr auto kReverbFixedGain = 0.015;
constexpr auto kReverbScaleWet = 3.;
constexpr auto kReverbScaleDamp = 0.4;
constexpr auto kReverbScaleRoom = 0.28;
constexpr auto kReverbOffsetRoom = 0.7;
constexpr auto kReverbMaxTail = crl::time(8000);
constexpr auto kReverbHighPass = 120.;

constexpr auto kEchoMaxTail = crl::time(10000);
constexpr auto kEchoRepeatsCutoff = 5000.;

constexpr auto kRobotPitch = 100.;
constexpr auto kRobotWindow = 0.021;

constexpr auto kPhoneLow = 300.;
constexpr auto kPhoneHigh = 3400.;
constexpr auto kPhoneDrive = 1.6;
constexpr auto kPhoneMu = 255.;

using Complex = std::complex<double>;

//
// Plain helpers.
//

[[nodiscard]] bool Usable(const Pcm &pcm) {
	return !pcm.empty()
		&& (pcm.channels <= kMaxChannels)
		&& (pcm.rate >= kMinRate)
		&& (pcm.rate <= kMaxRate)
		&& (pcm.frames() > 0);
}

[[nodiscard]] Pcm MakePcm(int channels, int rate, int64 frames) {
	auto result = Pcm();
	result.channels = channels;
	result.rate = rate;
	result.samples.assign(size_t(std::max(frames, int64(0)) * channels), 0.f);
	return result;
}

[[nodiscard]] int64 MsToFrames(crl::time ms, int rate) {
	return ms * rate / 1000;
}

[[nodiscard]] double Clamp01(double value) {
	return std::isfinite(value) ? std::clamp(value, 0., 1.) : 0.;
}

[[nodiscard]] float Sanitized(float value) {
	return std::isfinite(value) ? value : 0.f;
}

[[nodiscard]] float ClampSample(float value) {
	return std::isfinite(value) ? std::clamp(value, -1.f, 1.f) : 0.f;
}

[[nodiscard]] double PeakOf(const std::vector<float> &samples) {
	auto result = 0.f;
	for (const auto value : samples) {
		const auto magnitude = std::abs(value);
		if (magnitude > result && std::isfinite(magnitude)) {
			result = magnitude;
		}
	}
	return result;
}

[[nodiscard]] double RmsOf(const float *samples, int64 count) {
	if (count <= 0) {
		return 0.;
	}
	auto sum = 0.;
	for (auto i = int64(0); i != count; ++i) {
		const auto value = double(Sanitized(samples[i]));
		sum += value * value;
	}
	return std::sqrt(sum / double(count));
}

void Scale(Pcm &pcm, double gain) {
	const auto factor = float(gain);
	for (auto &value : pcm.samples) {
		value = Sanitized(value) * factor;
	}
}

void SafetyLimit(Pcm &pcm) {
	const auto peak = PeakOf(pcm.samples);
	if (peak > kSafetyPeak) {
		Scale(pcm, kSafetyPeak / peak);
	}
}

// Raised-cosine gain ramp over [from, from + count) frames.
void FadeRange(Pcm &pcm, int64 from, int64 count, bool in) {
	const auto frames = pcm.frames();
	from = std::clamp(from, int64(0), frames);
	count = std::clamp(count, int64(0), frames - from);
	if (count <= 0) {
		return;
	}
	const auto channels = pcm.channels;
	for (auto i = int64(0); i != count; ++i) {
		const auto progress = (i + 0.5) / double(count);
		const auto ramp = 0.5 - 0.5 * std::cos(kPi * progress);
		const auto gain = float(in ? ramp : (1. - ramp));
		auto sample = pcm.samples.data() + (from + i) * channels;
		for (auto c = 0; c != channels; ++c) {
			sample[c] *= gain;
		}
	}
}

// Finishes an effect tail: cuts the trailing near-silence (70 dB below
// the peak), keeping at least minFrames frames, and fades the tail out
// (up to a second) if it was cut by the tail length limit.
void FinishTail(Pcm &pcm, int64 minFrames, bool limited) {
	const auto channels = pcm.channels;
	const auto threshold = float(std::max(
		PeakOf(pcm.samples) * kTailThreshold,
		1e-6));
	auto frames = pcm.frames();
	while (frames > minFrames) {
		const auto from = (frames - 1) * channels;
		auto silent = true;
		for (auto c = 0; c != channels; ++c) {
			if (std::abs(pcm.samples[from + c]) > threshold) {
				silent = false;
				break;
			}
		}
		if (!silent) {
			break;
		}
		--frames;
	}
	pcm.samples.resize(size_t(frames * channels));
	if (limited) {
		const auto length = std::min(frames - minFrames, int64(pcm.rate));
		FadeRange(pcm, frames - length, length, false);
	}
}

[[nodiscard]] Pcm Remix(const Pcm &pcm, int channels) {
	if (pcm.channels == channels) {
		return pcm;
	}
	const auto from = pcm.channels;
	const auto frames = pcm.frames();
	auto result = MakePcm(channels, pcm.rate, frames);
	const auto source = pcm.samples.data();
	const auto target = result.samples.data();
	for (auto f = int64(0); f != frames; ++f) {
		const auto in = source + f * from;
		const auto out = target + f * channels;
		if (channels == 1) {
			auto sum = 0.f;
			for (auto c = 0; c != from; ++c) {
				sum += in[c];
			}
			out[0] = sum / from;
		} else if (from == 1) {
			for (auto c = 0; c != channels; ++c) {
				out[c] = in[0];
			}
		} else if (channels == 2) {
			// Even source channels go left, odd ones go right.
			auto left = 0.f;
			auto right = 0.f;
			for (auto c = 0; c != from; ++c) {
				((c % 2) ? right : left) += in[c];
			}
			out[0] = left / ((from + 1) / 2);
			out[1] = right / (from / 2);
		} else {
			for (auto c = 0; c != channels; ++c) {
				out[c] = in[c % from];
			}
		}
	}
	return result;
}

// Iterative radix-2 FFT with precomputed tables, the size must be
// a power of two. The complex multiplication is written out by hand,
// std::complex operator* goes through the slow __muldc3 without
// -ffast-math.
class Fft final {
public:
	explicit Fft(size_t size)
	: _size(size)
	, _reversed(size)
	, _twiddles(size / 2) {
		for (size_t i = 1, j = 0; i < size; ++i) {
			auto bit = size >> 1;
			for (; j & bit; bit >>= 1) {
				j ^= bit;
			}
			j ^= bit;
			_reversed[i] = j;
		}
		for (size_t k = 0; k != size / 2; ++k) {
			_twiddles[k] = std::polar(1., -2. * kPi * double(k) / size);
		}
	}

	void transform(std::vector<Complex> &data, bool inverse) const {
		const auto n = _size;
		Assert(data.size() == n);
		for (size_t i = 1; i < n; ++i) {
			const auto j = _reversed[i];
			if (i < j) {
				std::swap(data[i], data[j]);
			}
		}
		const auto sign = inverse ? -1. : 1.;
		for (size_t length = 2; length <= n; length <<= 1) {
			const auto half = length / 2;
			const auto step = n / length;
			for (size_t i = 0; i < n; i += length) {
				for (size_t k = 0; k != half; ++k) {
					const auto &w = _twiddles[k * step];
					const auto wr = w.real();
					const auto wi = w.imag() * sign;
					const auto u = data[i + k];
					const auto b = data[i + k + half];
					const auto v = Complex(
						b.real() * wr - b.imag() * wi,
						b.real() * wi + b.imag() * wr);
					data[i + k] = Complex(
						u.real() + v.real(),
						u.imag() + v.imag());
					data[i + k + half] = Complex(
						u.real() - v.real(),
						u.imag() - v.imag());
				}
			}
		}
		if (inverse) {
			const auto scale = 1. / double(n);
			for (auto &value : data) {
				value = Complex(value.real() * scale, value.imag() * scale);
			}
		}
	}

private:
	size_t _size = 0;
	std::vector<size_t> _reversed;
	std::vector<Complex> _twiddles;

};

[[nodiscard]] double Magnitude(const Complex &value) {
	return std::sqrt(value.real() * value.real()
		+ value.imag() * value.imag());
}

// RBJ cookbook biquad, transposed direct form II.
struct Biquad {
	double b0 = 1.;
	double b1 = 0.;
	double b2 = 0.;
	double a1 = 0.;
	double a2 = 0.;
	double z1 = 0.;
	double z2 = 0.;

	[[nodiscard]] double process(double input) {
		const auto output = b0 * input + z1;
		z1 = b1 * input - a1 * output + z2;
		z2 = b2 * input - a2 * output;
		return output;
	}
};

[[nodiscard]] Biquad MakeBiquad(bool lowPass, double frequency, int rate) {
	const auto omega = 2. * kPi * std::clamp(frequency, 10., rate * 0.45)
		/ double(rate);
	const auto cosine = std::cos(omega);
	const auto alpha = std::sin(omega) / (2. * std::sqrt(0.5));
	const auto a0 = 1. + alpha;
	auto result = Biquad();
	if (lowPass) {
		result.b0 = (1. - cosine) / 2. / a0;
		result.b1 = (1. - cosine) / a0;
		result.b2 = (1. - cosine) / 2. / a0;
	} else {
		result.b0 = (1. + cosine) / 2. / a0;
		result.b1 = -(1. + cosine) / a0;
		result.b2 = (1. + cosine) / 2. / a0;
	}
	result.a1 = -2. * cosine / a0;
	result.a2 = (1. - alpha) / a0;
	return result;
}

class Comb final {
public:
	explicit Comb(int size) : _buffer(size_t(std::max(size, 1)), 0.f) {
	}

	[[nodiscard]] float process(
			float input,
			float feedback,
			float damp1,
			float damp2) {
		const auto output = _buffer[_index];
		_store = output * damp2 + _store * damp1;
		if (std::abs(_store) < 1e-20f) {
			_store = 0.f;
		}
		_buffer[_index] = input + _store * feedback;
		if (++_index == _buffer.size()) {
			_index = 0;
		}
		return output;
	}

private:
	std::vector<float> _buffer;
	size_t _index = 0;
	float _store = 0.f;

};

class Allpass final {
public:
	explicit Allpass(int size) : _buffer(size_t(std::max(size, 1)), 0.f) {
	}

	[[nodiscard]] float process(float input) {
		const auto buffered = _buffer[_index];
		const auto output = buffered - input;
		auto stored = input + buffered * 0.5f;
		if (std::abs(stored) < 1e-20f) {
			stored = 0.f;
		}
		_buffer[_index] = stored;
		if (++_index == _buffer.size()) {
			_index = 0;
		}
		return output;
	}

private:
	std::vector<float> _buffer;
	size_t _index = 0;

};

[[nodiscard]] std::vector<double> TempoChain(double factor) {
	// atempo supports 0.5..100, but sounds best inside 0.5..2.
	auto result = std::vector<double>();
	while (factor < 0.5 - 1e-9) {
		result.push_back(0.5);
		factor /= 0.5;
	}
	while (factor > 2. + 1e-9) {
		result.push_back(2.);
		factor /= 2.;
	}
	if (std::abs(factor - 1.) > 1e-6) {
		result.push_back(factor);
	}
	return result;
}

//
// FFmpeg helpers.
//

struct FileSource {
	QFile file;

	static int Read(void *opaque, uint8_t *buffer, int size) {
		const auto that = static_cast<FileSource*>(opaque);
		const auto read = that->file.read(reinterpret_cast<char*>(buffer), size);
		if (read < 0) {
			return AVERROR(EIO);
		}
		return read ? int(read) : AVERROR_EOF;
	}

	static int64_t Seek(void *opaque, int64_t offset, int whence) {
		const auto that = static_cast<FileSource*>(opaque);
		const auto size = int64_t(that->file.size());
		auto position = int64_t(-1);
		switch (whence & ~AVSEEK_FORCE) {
		case AVSEEK_SIZE: return size;
		case SEEK_SET: position = offset; break;
		case SEEK_CUR: position = int64_t(that->file.pos()) + offset; break;
		case SEEK_END: position = size + offset; break;
		}
		if (position < 0 || position > size || !that->file.seek(position)) {
			return -1;
		}
		return position;
	}
};

struct BytesSource {
	const QByteArray *bytes = nullptr;
	int64_t offset = 0;

	static int Read(void *opaque, uint8_t *buffer, int size) {
		const auto that = static_cast<BytesSource*>(opaque);
		const auto available = int64_t(that->bytes->size()) - that->offset;
		const auto count = std::min(int64_t(size), available);
		if (count <= 0) {
			return AVERROR_EOF;
		}
		memcpy(buffer, that->bytes->constData() + that->offset, count);
		that->offset += count;
		return int(count);
	}

	static int64_t Seek(void *opaque, int64_t offset, int whence) {
		const auto that = static_cast<BytesSource*>(opaque);
		const auto size = int64_t(that->bytes->size());
		auto position = int64_t(-1);
		switch (whence & ~AVSEEK_FORCE) {
		case AVSEEK_SIZE: return size;
		case SEEK_SET: position = offset; break;
		case SEEK_CUR: position = that->offset + offset; break;
		case SEEK_END: position = size + offset; break;
		}
		if (position < 0 || position > size) {
			return -1;
		}
		return (that->offset = position);
	}
};

using ReadMethod = int(*)(void *opaque, uint8_t *buffer, int size);
using SeekMethod = int64_t(*)(void *opaque, int64_t offset, int whence);

struct GraphDeleter {
	void operator()(AVFilterGraph *value) {
		avfilter_graph_free(&value);
	}
};
using GraphPointer = std::unique_ptr<AVFilterGraph, GraphDeleter>;

[[nodiscard]] AVChannelLayout DefaultLayout(int channels) {
	auto result = AVChannelLayout();
	av_channel_layout_default(&result, channels);
	return result;
}

// Interleaved float resampling, the result has exactly
// round(frames * to / from) frames.
[[nodiscard]] std::optional<std::vector<float>> ResampleSamples(
		const float *samples,
		int64 frames,
		int channels,
		int from,
		int to) {
	const auto target = int64(std::llround(double(frames) * to / from));
	if (from == to) {
		return std::vector<float>(samples, samples + frames * channels);
	}
	auto layout = DefaultLayout(channels);
	auto swr = (SwrContext*)nullptr;
	auto error = AvErrorWrap(swr_alloc_set_opts2(
		&swr,
		&layout,
		AV_SAMPLE_FMT_FLT,
		to,
		&layout,
		AV_SAMPLE_FMT_FLT,
		from,
		0,
		nullptr));
	av_channel_layout_uninit(&layout);
	if (error || !swr) {
		LogError(u"swr_alloc_set_opts2"_q, error);
		swr_free(&swr);
		return std::nullopt;
	}
	const auto guard = gsl::finally([&] { swr_free(&swr); });
	if ((error = AvErrorWrap(swr_init(swr)))) {
		LogError(u"swr_init"_q, error);
		return std::nullopt;
	}
	auto result = std::vector<float>();
	result.reserve(size_t((target + 64) * channels));
	const auto convert = [&](const float *input, int count) {
		const auto capacity = swr_get_out_samples(swr, count);
		if (capacity <= 0) {
			return (capacity == 0);
		}
		const auto was = result.size();
		result.resize(was + size_t(capacity) * channels);
		auto out = reinterpret_cast<uint8_t*>(result.data() + was);
		auto in = reinterpret_cast<const uint8_t*>(input);
		const auto converted = swr_convert(
			swr,
			&out,
			capacity,
			input ? &in : nullptr,
			count);
		if (converted < 0) {
			LogError(u"swr_convert"_q, AvErrorWrap(converted));
			return false;
		}
		result.resize(was + size_t(converted) * channels);
		return true;
	};
	for (auto offset = int64(0); offset < frames; offset += kProcessChunk) {
		const auto count = int(std::min(int64(kProcessChunk), frames - offset));
		if (!convert(samples + offset * channels, count)) {
			return std::nullopt;
		}
	}
	for (auto i = 0; i != 16; ++i) {
		const auto was = result.size();
		if (!convert(nullptr, 0)) {
			return std::nullopt;
		} else if (result.size() == was) {
			break;
		}
	}
	result.resize(size_t(target * channels), 0.f);
	return result;
}

[[nodiscard]] Pcm ResampleRate(const Pcm &pcm, int rate) {
	auto samples = ResampleSamples(
		pcm.samples.data(),
		pcm.frames(),
		pcm.channels,
		pcm.rate,
		rate);
	if (!samples) {
		return Pcm();
	}
	auto result = Pcm();
	result.samples = std::move(*samples);
	result.channels = pcm.channels;
	result.rate = rate;
	return result;
}

[[nodiscard]] std::optional<std::vector<float>> ApplyTempo(
		const Pcm &pcm,
		const std::vector<double> &chain) {
	const auto abuffer = avfilter_get_by_name("abuffer");
	const auto abuffersink = avfilter_get_by_name("abuffersink");
	const auto atempo = avfilter_get_by_name("atempo");
	if (!abuffer || !abuffersink || !atempo) {
		LOG(("Oblivion Audio Error: No abuffer / abuffersink / atempo."));
		return std::nullopt;
	}
	auto graph = GraphPointer(avfilter_graph_alloc());
	if (!graph) {
		LogError(u"avfilter_graph_alloc"_q);
		return std::nullopt;
	}
	graph->nb_threads = 1;

	const auto channels = pcm.channels;
	auto layout = DefaultLayout(channels);
	const auto layoutGuard = gsl::finally([&] {
		av_channel_layout_uninit(&layout);
	});
	char layoutName[64] = { 0 };
	av_channel_layout_describe(&layout, layoutName, sizeof(layoutName));

	const auto source = avfilter_graph_alloc_filter(
		graph.get(),
		abuffer,
		"src");
	if (!source) {
		LogError(u"avfilter_graph_alloc_filter"_q, u"abuffer"_q);
		return std::nullopt;
	}
	av_opt_set(source, "channel_layout", layoutName, AV_OPT_SEARCH_CHILDREN);
	av_opt_set_sample_fmt(
		source,
		"sample_fmt",
		AV_SAMPLE_FMT_FLT,
		AV_OPT_SEARCH_CHILDREN);
	av_opt_set_q(
		source,
		"time_base",
		AVRational{ 1, pcm.rate },
		AV_OPT_SEARCH_CHILDREN);
	av_opt_set_int(source, "sample_rate", pcm.rate, AV_OPT_SEARCH_CHILDREN);
	auto error = AvErrorWrap(avfilter_init_str(source, nullptr));
	if (error) {
		LogError(u"avfilter_init_str"_q, error, u"abuffer"_q);
		return std::nullopt;
	}
	auto last = source;
	for (auto i = 0; i != int(chain.size()); ++i) {
		const auto name = "atempo" + std::to_string(i);
		const auto filter = avfilter_graph_alloc_filter(
			graph.get(),
			atempo,
			name.c_str());
		if (!filter) {
			LogError(u"avfilter_graph_alloc_filter"_q, u"atempo"_q);
			return std::nullopt;
		}
		av_opt_set_double(
			filter,
			"tempo",
			chain[i],
			AV_OPT_SEARCH_CHILDREN);
		if ((error = AvErrorWrap(avfilter_init_str(filter, nullptr)))) {
			LogError(u"avfilter_init_str"_q, error, u"atempo"_q);
			return std::nullopt;
		} else if ((error = AvErrorWrap(avfilter_link(last, 0, filter, 0)))) {
			LogError(u"avfilter_link"_q, error, u"atempo"_q);
			return std::nullopt;
		}
		last = filter;
	}
	const auto sink = avfilter_graph_alloc_filter(
		graph.get(),
		abuffersink,
		"sink");
	if (!sink) {
		LogError(u"avfilter_graph_alloc_filter"_q, u"abuffersink"_q);
		return std::nullopt;
	} else if ((error = AvErrorWrap(avfilter_init_str(sink, nullptr)))) {
		LogError(u"avfilter_init_str"_q, error, u"abuffersink"_q);
		return std::nullopt;
	} else if ((error = AvErrorWrap(avfilter_link(last, 0, sink, 0)))) {
		LogError(u"avfilter_link"_q, error, u"abuffersink"_q);
		return std::nullopt;
	} else if ((error = AvErrorWrap(avfilter_graph_config(
			graph.get(),
			nullptr)))) {
		LogError(u"avfilter_graph_config"_q, error);
		return std::nullopt;
	}

	const auto frames = pcm.frames();
	auto product = 1.;
	for (const auto factor : chain) {
		product *= factor;
	}
	auto result = std::vector<float>();
	result.reserve(size_t(channels)
		* size_t(double(frames) / std::max(product, 0.01) + 4096));
	auto input = FFmpeg::MakeFramePointer();
	auto output = FFmpeg::MakeFramePointer();
	if (!input || !output) {
		return std::nullopt;
	}
	const auto pull = [&] {
		while (true) {
			const auto got = av_buffersink_get_frame(sink, output.get());
			if (got == AVERROR(EAGAIN) || got == AVERROR_EOF) {
				return true;
			} else if (got < 0) {
				LogError(u"av_buffersink_get_frame"_q, AvErrorWrap(got));
				return false;
			}
			const auto good = (output->format == AV_SAMPLE_FMT_FLT)
				&& (output->ch_layout.nb_channels == channels);
			if (good) {
				const auto data = reinterpret_cast<const float*>(
					output->extended_data[0]);
				result.insert(
					end(result),
					data,
					data + size_t(output->nb_samples) * channels);
			}
			av_frame_unref(output.get());
			if (!good) {
				LOG(("Oblivion Audio Error: Unexpected atempo output."));
				return false;
			}
		}
	};
	for (auto offset = int64(0); offset < frames; offset += kProcessChunk) {
		const auto count = int(std::min(int64(kProcessChunk), frames - offset));
		input->format = AV_SAMPLE_FMT_FLT;
		input->sample_rate = pcm.rate;
		input->nb_samples = count;
		input->pts = offset;
		av_channel_layout_copy(&input->ch_layout, &layout);
		if ((error = AvErrorWrap(av_frame_get_buffer(input.get(), 0)))) {
			LogError(u"av_frame_get_buffer"_q, error);
			return std::nullopt;
		}
		memcpy(
			input->extended_data[0],
			pcm.samples.data() + offset * channels,
			size_t(count) * channels * sizeof(float));
		error = AvErrorWrap(av_buffersrc_add_frame(source, input.get()));
		av_frame_unref(input.get());
		if (error) {
			LogError(u"av_buffersrc_add_frame"_q, error);
			return std::nullopt;
		} else if (!pull()) {
			return std::nullopt;
		}
	}
	if ((error = AvErrorWrap(av_buffersrc_add_frame(source, nullptr)))) {
		LogError(u"av_buffersrc_add_frame"_q, error, u"EOF"_q);
		return std::nullopt;
	} else if (!pull()) {
		return std::nullopt;
	}
	return result;
}

// Like FFmpeg::MakeSwresamplePointer, but downmixing more than
// two channels is normalized instead of possibly leaving [-1, 1].
[[nodiscard]] FFmpeg::SwresamplePointer MakeDecodeResampler(
		AVChannelLayout *inLayout,
		AVSampleFormat inFormat,
		int inRate,
		AVChannelLayout *outLayout,
		int outRate,
		FFmpeg::SwresamplePointer *existing) {
	if (existing && *existing) {
		const auto &deleter = existing->get_deleter();
		if (deleter.srcFormat == inFormat
			&& deleter.srcRate == inRate
			&& deleter.srcChannels == inLayout->nb_channels
			&& deleter.dstRate == outRate
			&& deleter.dstChannels == outLayout->nb_channels) {
			return std::move(*existing);
		}
	}
	auto result = (SwrContext*)nullptr;
	auto error = AvErrorWrap(swr_alloc_set_opts2(
		&result,
		outLayout,
		AV_SAMPLE_FMT_FLT,
		outRate,
		inLayout,
		inFormat,
		inRate,
		0,
		nullptr));
	if (error || !result) {
		LogError(u"swr_alloc_set_opts2"_q, error);
		swr_free(&result);
		return FFmpeg::SwresamplePointer();
	}
	av_opt_set_double(result, "rematrix_maxval", 1., 0);
	if ((error = AvErrorWrap(swr_init(result)))) {
		LogError(u"swr_init"_q, error);
		swr_free(&result);
		return FFmpeg::SwresamplePointer();
	}
	return FFmpeg::SwresamplePointer(result, {
		.srcFormat = inFormat,
		.srcRate = inRate,
		.srcChannels = inLayout->nb_channels,
		.dstFormat = AV_SAMPLE_FMT_FLT,
		.dstRate = outRate,
		.dstChannels = outLayout->nb_channels,
	});
}

struct OpenedInput {
	FFmpeg::FormatPointer format;
	FFmpeg::CodecPointer codec;
	int stream = -1;
};

[[nodiscard]] std::optional<OpenedInput> OpenInput(
		void *opaque,
		ReadMethod read,
		SeekMethod seek) {
	auto result = OpenedInput();
	result.format = FFmpeg::MakeFormatPointer(opaque, read, nullptr, seek);
	if (!result.format) {
		return std::nullopt;
	}
	const auto format = result.format.get();
	auto error = AvErrorWrap(avformat_find_stream_info(format, nullptr));
	if (error) {
		LogError(u"avformat_find_stream_info"_q, error);
		return std::nullopt;
	}
	result.stream = av_find_best_stream(
		format,
		AVMEDIA_TYPE_AUDIO,
		-1,
		-1,
		nullptr,
		0);
	if (result.stream < 0) {
		return std::nullopt;
	}
	for (auto i = 0; i != int(format->nb_streams); ++i) {
		if (i != result.stream) {
			format->streams[i]->discard = AVDISCARD_ALL;
		}
	}
	const auto stream = format->streams[result.stream];
	result.codec = FFmpeg::CodecPointer(avcodec_alloc_context3(nullptr));
	const auto codec = result.codec.get();
	if (!codec) {
		LogError(u"avcodec_alloc_context3"_q);
		return std::nullopt;
	}
	error = AvErrorWrap(avcodec_parameters_to_context(
		codec,
		stream->codecpar));
	if (error) {
		LogError(u"avcodec_parameters_to_context"_q, error);
		return std::nullopt;
	}
	codec->pkt_timebase = stream->time_base;
	codec->request_sample_fmt = AV_SAMPLE_FMT_FLT;
	const auto decoder = avcodec_find_decoder(codec->codec_id);
	if (!decoder) {
		LogError(
			u"avcodec_find_decoder"_q,
			QString::fromUtf8(avcodec_get_name(codec->codec_id)));
		return std::nullopt;
	} else if ((error = AvErrorWrap(avcodec_open2(codec, decoder, nullptr)))) {
		LogError(u"avcodec_open2"_q, error);
		return std::nullopt;
	}
	return result;
}

[[nodiscard]] std::optional<Pcm> DecodeInput(
		OpenedInput &input,
		const DecodeOptions &options) {
	const auto format = input.format.get();
	const auto codec = input.codec.get();

	auto result = Pcm();
	auto wanted = 0;
	auto limit = int64(0);
	auto limited = false;
	auto outLayout = AVChannelLayout();
	const auto outGuard = gsl::finally([&] {
		av_channel_layout_uninit(&outLayout);
	});
	auto swr = FFmpeg::SwresamplePointer();
	auto frame = FFmpeg::MakeFramePointer();
	auto packet = FFmpeg::Packet();
	if (!frame) {
		return std::nullopt;
	}

	const auto convert = [&](const uint8_t **data, int count) {
		const auto capacity = swr_get_out_samples(swr.get(), count);
		if (capacity <= 0) {
			return (capacity == 0);
		}
		const auto was = result.samples.size();
		result.samples.resize(was + size_t(capacity) * result.channels);
		auto out = reinterpret_cast<uint8_t*>(result.samples.data() + was);
		const auto converted = swr_convert(
			swr.get(),
			&out,
			capacity,
			data,
			count);
		if (converted < 0) {
			result.samples.resize(was);
			LogError(u"swr_convert"_q, AvErrorWrap(converted));
			return false;
		}
		result.samples.resize(was + size_t(converted) * result.channels);
		return true;
	};
	const auto process = [&] {
		if (frame->nb_samples <= 0 || frame->sample_rate <= 0) {
			return true;
		}
		if (!result.rate) {
			// FFmpeg downmixes more than two channels to stereo,
			// mono <-> stereo is done by Remix() in the end.
			const auto source = std::max(frame->ch_layout.nb_channels, 1);
			result.channels = std::min(source, 2);
			wanted = options.channels ? options.channels : result.channels;
			result.rate = options.rate ? options.rate : frame->sample_rate;
			if (result.rate < kMinRate || result.rate > kMaxRate) {
				LOG(("Oblivion Audio Error: Bad decode rate %1."
					).arg(result.rate));
				return false;
			}
			av_channel_layout_default(&outLayout, result.channels);
			limit = kMaxDecodedSamples / std::max(result.channels, wanted);
			if (options.maxDuration > 0) {
				limit = std::min(
					limit,
					std::max(
						MsToFrames(options.maxDuration, result.rate),
						int64(1)));
			}
		}
		auto inLayout = AVChannelLayout();
		if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC
			|| !av_channel_layout_check(&frame->ch_layout)) {
			av_channel_layout_default(
				&inLayout,
				std::max(frame->ch_layout.nb_channels, 1));
		} else {
			av_channel_layout_copy(&inLayout, &frame->ch_layout);
		}
		swr = MakeDecodeResampler(
			&inLayout,
			AVSampleFormat(frame->format),
			frame->sample_rate,
			&outLayout,
			result.rate,
			&swr);
		av_channel_layout_uninit(&inLayout);
		if (!swr) {
			return false;
		}
		return convert(
			const_cast<const uint8_t**>(frame->extended_data),
			frame->nb_samples);
	};

	enum class Received {
		NeedMore,
		Finished,
		Failed,
		Corrupted,
	};
	const auto receive = [&] {
		while (true) {
			const auto error = avcodec_receive_frame(codec, frame.get());
			if (error == AVERROR(EAGAIN)) {
				return Received::NeedMore;
			} else if (error == AVERROR_EOF) {
				return Received::Finished;
			} else if (error < 0) {
				return Received::Corrupted;
			}
			const auto processed = process();
			av_frame_unref(frame.get());
			if (!processed) {
				return Received::Failed;
			} else if (limit > 0 && result.frames() >= limit) {
				limited = true;
				return Received::Finished;
			}
		}
	};

	auto errors = 0;
	auto finished = false;
	while (!finished) {
		auto &fields = packet.fields();
		const auto read = av_read_frame(format, &fields);
		if (read < 0) {
			if (read != AVERROR_EOF) {
				LogError(u"av_read_frame"_q, AvErrorWrap(read));
			}
			break;
		} else if (fields.stream_index != input.stream) {
			av_packet_unref(&fields);
			continue;
		}
		auto sent = avcodec_send_packet(codec, &fields);
		if (sent == AVERROR(EAGAIN)) {
			const auto received = receive();
			if (received == Received::Failed) {
				av_packet_unref(&fields);
				return std::nullopt;
			} else if (received == Received::Finished) {
				av_packet_unref(&fields);
				finished = true;
				break;
			}
			sent = avcodec_send_packet(codec, &fields);
		}
		av_packet_unref(&fields);
		if (sent < 0 && sent != AVERROR(EAGAIN)) {
			if (++errors > kMaxDecodeErrors) {
				LogError(u"avcodec_send_packet"_q, AvErrorWrap(sent));
				break;
			}
			continue;
		}
		switch (receive()) {
		case Received::Failed: return std::nullopt;
		case Received::Finished: finished = true; break;
		case Received::Corrupted:
			if (++errors > kMaxDecodeErrors) {
				finished = true;
			}
			break;
		case Received::NeedMore: break;
		}
	}
	if (!limited) {
		// Drain the decoder.
		if (avcodec_send_packet(codec, nullptr) >= 0) {
			if (receive() == Received::Failed) {
				return std::nullopt;
			}
		}
	}
	if (swr) {
		for (auto i = 0; i != 16; ++i) {
			const auto was = result.samples.size();
			if (!convert(nullptr, 0) || result.samples.size() == was) {
				break;
			}
		}
	}
	const auto stream = format->streams[input.stream];
	if (!limited
		&& result.rate > 0
		&& codec->codec_id == AV_CODEC_ID_AAC
		&& format->duration_estimation_method == AVFMT_DURATION_FROM_STREAM
		&& stream->duration != AV_NOPTS_VALUE
		&& stream->duration > 0) {
		// The mov demuxer applies the edit list start (encoder priming),
		// but the last AAC frame comes with its padding, cut it here.
		const auto expected = int64(av_rescale_q(
			stream->duration,
			stream->time_base,
			AVRational{ 1, result.rate }));
		const auto padding = int64(2048) * result.rate
			/ std::max(codec->sample_rate, 1);
		const auto extra = result.frames() - expected;
		if (extra > 0 && extra <= padding) {
			result.samples.resize(size_t(expected * result.channels));
		}
	}
	if (limit > 0 && result.frames() > limit) {
		result.samples.resize(size_t(limit * result.channels));
		limited = true;
	}
	if (limited && options.maxDuration <= 0) {
		LOG(("Oblivion Audio: Decoded audio cut at %1 ms."
			).arg(result.duration()));
	}
	if (result.empty() || result.frames() <= 0) {
		return std::nullopt;
	} else if (wanted && wanted != result.channels) {
		return Remix(result, wanted);
	}
	return result;
}

[[nodiscard]] std::optional<Pcm> DecodeFrom(
		void *opaque,
		ReadMethod read,
		SeekMethod seek,
		const DecodeOptions &options) {
	if ((options.rate
			&& (options.rate < kMinRate || options.rate > kMaxRate))
		|| (options.channels < 0)
		|| (options.channels > 2)) {
		return std::nullopt;
	}
	auto input = OpenInput(opaque, read, seek);
	if (!input) {
		return std::nullopt;
	}
	return DecodeInput(*input, options);
}

struct EncoderSetup {
	QByteArray muxer;
	const char *encoder = nullptr;
	AVSampleFormat format = AV_SAMPLE_FMT_NONE;
	int bitrate = 0;
	QString title; // Empty = no tag.
	QString performer; // Empty = no tag.
};

// Optional tags never fail the encoding.
void SetMetadata(
		AVDictionary **metadata,
		const char *key,
		const QString &value) {
	if (value.isEmpty()) {
		return;
	}
	const auto utf8 = value.toUtf8();
	const auto error = AvErrorWrap(av_dict_set(
		metadata,
		key,
		utf8.constData(),
		0));
	if (error) {
		LogError(u"av_dict_set"_q, error, QString::fromUtf8(key));
	}
}

// The pcm must already have the rate and the channels for the encoder.
[[nodiscard]] QByteArray EncodeWithFFmpeg(
		const Pcm &pcm,
		const EncoderSetup &setup) {
	auto wrap = FFmpeg::WriteBytesWrap();
	auto format = FFmpeg::MakeWriteFormatPointer(
		static_cast<void*>(&wrap),
		nullptr,
		&FFmpeg::WriteBytesWrap::Write,
		&FFmpeg::WriteBytesWrap::Seek,
		setup.muxer);
	if (!format) {
		return QByteArray();
	}
	const auto name = QString::fromUtf8(setup.encoder);
	const auto codec = avcodec_find_encoder_by_name(setup.encoder);
	if (!codec) {
		LogError(u"avcodec_find_encoder_by_name"_q, name);
		return QByteArray();
	}
	const auto stream = avformat_new_stream(format.get(), codec);
	if (!stream) {
		LogError(u"avformat_new_stream"_q, name);
		return QByteArray();
	}
	auto context = FFmpeg::CodecPointer(avcodec_alloc_context3(codec));
	if (!context) {
		LogError(u"avcodec_alloc_context3"_q, name);
		return QByteArray();
	}
	context->sample_fmt = setup.format;
	context->sample_rate = pcm.rate;
	av_channel_layout_default(&context->ch_layout, pcm.channels);
	context->bit_rate = setup.bitrate;
	context->time_base = AVRational{ 1, pcm.rate };
	if (format->oformat->flags & AVFMT_GLOBALHEADER) {
		context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}
	auto error = AvErrorWrap(avcodec_open2(context.get(), codec, nullptr));
	if (error) {
		LogError(u"avcodec_open2"_q, error, name);
		return QByteArray();
	}
	error = AvErrorWrap(avcodec_parameters_from_context(
		stream->codecpar,
		context.get()));
	if (error) {
		LogError(u"avcodec_parameters_from_context"_q, error, name);
		return QByteArray();
	}
	stream->time_base = context->time_base;

	// The mp4 muxer writes them as iTunes atoms, the ogg muxer copies
	// them to the stream and writes them as Vorbis comments.
	SetMetadata(&format->metadata, "title", setup.title);
	SetMetadata(&format->metadata, "artist", setup.performer);
	if ((error = AvErrorWrap(avformat_write_header(format.get(), nullptr)))) {
		LogError(u"avformat_write_header"_q, error, name);
		return QByteArray();
	}

	auto packet = FFmpeg::Packet();
	const auto drain = [&] {
		while (true) {
			auto &fields = packet.fields();
			const auto received = avcodec_receive_packet(
				context.get(),
				&fields);
			if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
				return true;
			} else if (received < 0) {
				LogError(u"avcodec_receive_packet"_q, AvErrorWrap(received));
				return false;
			}
			av_packet_rescale_ts(
				&fields,
				context->time_base,
				stream->time_base);
			fields.stream_index = stream->index;
			const auto written = av_interleaved_write_frame(
				format.get(),
				&fields);
			if (written < 0) {
				LogError(
					u"av_interleaved_write_frame"_q,
					AvErrorWrap(written));
				return false;
			}
		}
	};

	const auto variable = (codec->capabilities
		& AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
		|| (context->frame_size <= 0);
	const auto frameSize = variable ? 4096 : context->frame_size;
	const auto smallLast = variable
		|| (codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME);
	const auto planar = av_sample_fmt_is_planar(setup.format);
	const auto channels = pcm.channels;
	const auto frames = pcm.frames();
	auto frame = FFmpeg::MakeFramePointer();
	if (!frame) {
		return QByteArray();
	}
	for (auto offset = int64(0); offset < frames; offset += frameSize) {
		const auto count = int(std::min(int64(frameSize), frames - offset));
		const auto size = smallLast ? count : frameSize;
		frame->format = setup.format;
		frame->sample_rate = pcm.rate;
		frame->nb_samples = size;
		av_channel_layout_copy(&frame->ch_layout, &context->ch_layout);
		if ((error = AvErrorWrap(av_frame_get_buffer(frame.get(), 0)))) {
			LogError(u"av_frame_get_buffer"_q, error);
			return QByteArray();
		}
		const auto source = pcm.samples.data() + offset * channels;
		if (planar) {
			for (auto c = 0; c != channels; ++c) {
				const auto out = reinterpret_cast<float*>(
					frame->extended_data[c]);
				for (auto i = 0; i != count; ++i) {
					out[i] = ClampSample(source[i * channels + c]);
				}
				std::fill(out + count, out + size, 0.f);
			}
		} else {
			const auto out = reinterpret_cast<float*>(
				frame->extended_data[0]);
			for (auto i = 0; i != count * channels; ++i) {
				out[i] = ClampSample(source[i]);
			}
			std::fill(out + count * channels, out + size * channels, 0.f);
		}
		frame->pts = offset;
		auto sent = avcodec_send_frame(context.get(), frame.get());
		if (sent == AVERROR(EAGAIN)) {
			if (!drain()) {
				return QByteArray();
			}
			sent = avcodec_send_frame(context.get(), frame.get());
		}
		av_frame_unref(frame.get());
		if (sent < 0) {
			LogError(u"avcodec_send_frame"_q, AvErrorWrap(sent));
			return QByteArray();
		} else if (!drain()) {
			return QByteArray();
		}
	}
	if ((error = AvErrorWrap(avcodec_send_frame(context.get(), nullptr)))) {
		LogError(u"avcodec_send_frame"_q, error, u"flush"_q);
		return QByteArray();
	} else if (!drain()) {
		return QByteArray();
	} else if ((error = AvErrorWrap(av_write_trailer(format.get())))) {
		LogError(u"av_write_trailer"_q, error, name);
		return QByteArray();
	}
	avio_flush(format->pb);
	format = nullptr;
	return std::move(wrap.content);
}

// "LIST" chunk of the "INFO" type with UTF-8 INAM / IART subchunks (like
// the FFmpeg wav muxer writes), empty if there are no tags. Even size.
[[nodiscard]] QByteArray WavInfoChunk(
		const QString &title,
		const QString &performer) {
	auto content = QByteArray();
	const auto add = [&](const char *id, const QString &value) {
		if (value.isEmpty()) {
			return;
		}
		const auto utf8 = value.toUtf8();
		const auto size = uint32(utf8.size() + 1); // With the zero.
		auto header = QByteArray(8, char(0));
		memcpy(header.data(), id, 4);
		qToLittleEndian(size, header.data() + 4);
		content.append(header);
		content.append(utf8);
		content.append(char(0));
		if (size & 1) {
			content.append(char(0));
		}
	};
	add("INAM", title);
	add("IART", performer);
	if (content.isEmpty()) {
		return QByteArray();
	}
	auto result = QByteArray(12, char(0));
	memcpy(result.data(), "LIST", 4);
	qToLittleEndian(uint32(4 + content.size()), result.data() + 4);
	memcpy(result.data() + 8, "INFO", 4);
	return result + content;
}

[[nodiscard]] QByteArray EncodeWav(
		const Pcm &pcm,
		const QString &title,
		const QString &performer) {
	const auto channels = pcm.channels;
	const auto frames = pcm.frames();
	const auto count = frames * channels;
	const auto dataSize = count * 2;
	const auto info = WavInfoChunk(title, performer);
	const auto infoSize = int64(info.size());
	const auto riffSize = 36 + infoSize + dataSize;
	if (riffSize > int64(std::numeric_limits<uint32>::max())) {
		return QByteArray();
	}
	auto result = QByteArray();
	result.resize(44 + infoSize + dataSize);
	const auto data = reinterpret_cast<uchar*>(result.data());
	const auto put32 = [&](int offset, uint32 value) {
		qToLittleEndian(value, data + offset);
	};
	const auto put16 = [&](int offset, uint16 value) {
		qToLittleEndian(value, data + offset);
	};
	memcpy(data, "RIFF", 4);
	put32(4, uint32(riffSize));
	memcpy(data + 8, "WAVEfmt ", 8);
	put32(16, 16);
	put16(20, 1); // PCM
	put16(22, uint16(channels));
	put32(24, uint32(pcm.rate));
	put32(28, uint32(pcm.rate * channels * 2));
	put16(32, uint16(channels * 2));
	put16(34, 16);
	if (infoSize > 0) {
		// Before the samples, so that streaming readers see it as well.
		memcpy(data + 36, info.constData(), size_t(infoSize));
	}
	const auto dataChunk = 36 + int(infoSize);
	memcpy(data + dataChunk, "data", 4);
	put32(dataChunk + 4, uint32(dataSize));
	auto out = data + dataChunk + 8;
	for (auto i = int64(0); i != count; ++i, out += 2) {
		const auto value = ClampSample(pcm.samples[i]);
		qToLittleEndian(int16(std::lround(value * 32767.f)), out);
	}
	return result;
}

//
// Self-test helpers.
//

[[nodiscard]] Pcm MakeTone(
		int rate,
		int channels,
		crl::time duration,
		double frequency,
		double amplitude,
		double rightFrequency = 0.) {
	auto result = MakePcm(channels, rate, MsToFrames(duration, rate));
	const auto frames = result.frames();
	for (auto f = int64(0); f != frames; ++f) {
		const auto time = double(f) / rate;
		for (auto c = 0; c != channels; ++c) {
			const auto hz = (c == 1 && rightFrequency > 0.)
				? rightFrequency
				: frequency;
			result.samples[f * channels + c] = float(
				amplitude * std::sin(2. * kPi * hz * time));
		}
	}
	return result;
}

[[nodiscard]] Pcm MakeChirp(
		int rate,
		crl::time duration,
		double from,
		double till,
		double amplitude) {
	auto result = MakePcm(1, rate, MsToFrames(duration, rate));
	const auto frames = result.frames();
	const auto seconds = double(frames) / rate;
	const auto slope = (till - from) / seconds;
	for (auto f = int64(0); f != frames; ++f) {
		const auto time = double(f) / rate;
		const auto phase = 2. * kPi * (from * time + slope * time * time / 2.);
		result.samples[f] = float(amplitude * std::sin(phase));
	}
	return result;
}

// FFT peak with parabolic interpolation over the middle of the signal.
[[nodiscard]] double DominantFrequency(const Pcm &pcm, int channel = 0) {
	const auto frames = pcm.frames();
	auto size = int64(1);
	while (size * 2 <= std::min(frames, int64(65536))) {
		size *= 2;
	}
	if (size < 1024 || channel >= pcm.channels) {
		return 0.;
	}
	const auto start = (frames - size) / 2;
	auto data = std::vector<Complex>(size_t(size));
	for (auto i = int64(0); i != size; ++i) {
		const auto window = 0.5 - 0.5 * std::cos(2. * kPi * i / (size - 1));
		const auto value = pcm.samples[(start + i) * pcm.channels + channel];
		data[i] = Complex(value * window, 0.);
	}
	Fft(size_t(size)).transform(data, false);
	auto best = int64(2);
	for (auto k = int64(2); k < size / 2 - 1; ++k) {
		if (Magnitude(data[k]) > Magnitude(data[best])) {
			best = k;
		}
	}
	const auto magnitude = [&](int64 k) {
		return std::log(Magnitude(data[k]) + 1e-12);
	};
	const auto alpha = magnitude(best - 1);
	const auto beta = magnitude(best);
	const auto gamma = magnitude(best + 1);
	const auto denominator = alpha - 2. * beta + gamma;
	const auto shift = (std::abs(denominator) > 1e-12)
		? std::clamp(0.5 * (alpha - gamma) / denominator, -0.5, 0.5)
		: 0.;
	return (best + shift) * pcm.rate / double(size);
}

[[nodiscard]] double MiddleRms(const Pcm &pcm) {
	const auto frames = pcm.frames();
	const auto from = frames / 10;
	const auto till = frames - frames / 10;
	if (till <= from) {
		return 0.;
	}
	return RmsOf(
		pcm.samples.data() + from * pcm.channels,
		(till - from) * pcm.channels);
}

[[nodiscard]] bool AllFinite(const Pcm &pcm) {
	for (const auto value : pcm.samples) {
		if (!std::isfinite(value)) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] QString Number(double value, int precision = 2) {
	return QString::number(value, 'f', precision);
}

// "title" / "artist" as FFmpeg reads them back: from the container and
// from the best audio stream (the ogg demuxer keeps Vorbis comments
// in the stream metadata).
struct ReadTags {
	QString formatTitle;
	QString formatArtist;
	QString streamTitle;
	QString streamArtist;
};

[[nodiscard]] std::optional<ReadTags> ReadTagsFrom(const QByteArray &bytes) {
	if (bytes.isEmpty()) {
		return std::nullopt;
	}
	auto source = BytesSource{ .bytes = &bytes };
	auto format = FFmpeg::MakeFormatPointer(
		&source,
		&BytesSource::Read,
		nullptr,
		&BytesSource::Seek);
	if (!format) {
		return std::nullopt;
	}
	const auto raw = format.get();
	if (AvErrorWrap error = avformat_find_stream_info(raw, nullptr)) {
		LogError(u"avformat_find_stream_info"_q, error);
		return std::nullopt;
	}
	const auto value = [](const AVDictionary *metadata, const char *key) {
		const auto entry = av_dict_get(metadata, key, nullptr, 0);
		return entry ? QString::fromUtf8(entry->value) : QString();
	};
	auto result = ReadTags{
		.formatTitle = value(raw->metadata, "title"),
		.formatArtist = value(raw->metadata, "artist"),
	};
	const auto index = av_find_best_stream(
		raw,
		AVMEDIA_TYPE_AUDIO,
		-1,
		-1,
		nullptr,
		0);
	if (index >= 0) {
		const auto metadata = raw->streams[index]->metadata;
		result.streamTitle = value(metadata, "title");
		result.streamArtist = value(metadata, "artist");
	}
	return result;
}

} // namespace

std::optional<Pcm> Decode(const QString &path, DecodeOptions options) {
	auto source = FileSource();
	source.file.setFileName(path);
	if (!source.file.open(QIODevice::ReadOnly)) {
		LOG(("Oblivion Audio Error: Could not open '%1'.").arg(path));
		return std::nullopt;
	}
	return DecodeFrom(
		&source,
		&FileSource::Read,
		&FileSource::Seek,
		options);
}

std::optional<Pcm> Decode(const QByteArray &bytes, DecodeOptions options) {
	if (bytes.isEmpty()) {
		return std::nullopt;
	}
	auto source = BytesSource{ .bytes = &bytes };
	return DecodeFrom(
		&source,
		&BytesSource::Read,
		&BytesSource::Seek,
		options);
}

std::optional<MediaInfo> Probe(const QString &path) {
	auto source = FileSource();
	source.file.setFileName(path);
	if (!source.file.open(QIODevice::ReadOnly)) {
		return std::nullopt;
	}
	auto format = FFmpeg::MakeFormatPointer(
		&source,
		&FileSource::Read,
		nullptr,
		&FileSource::Seek);
	if (!format) {
		return std::nullopt;
	}
	const auto raw = format.get();
	if (AvErrorWrap error = avformat_find_stream_info(raw, nullptr)) {
		LogError(u"avformat_find_stream_info"_q, error);
		return std::nullopt;
	}
	auto result = MediaInfo();
	for (auto i = 0; i != int(raw->nb_streams); ++i) {
		const auto stream = raw->streams[i];
		if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO
			&& !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
			result.hasVideo = true;
		}
	}
	const auto index = av_find_best_stream(
		raw,
		AVMEDIA_TYPE_AUDIO,
		-1,
		-1,
		nullptr,
		0);
	if (index >= 0) {
		const auto stream = raw->streams[index];
		result.rate = stream->codecpar->sample_rate;
		result.channels = stream->codecpar->ch_layout.nb_channels;
		if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
			result.duration = FFmpeg::PtsToTimeCeil(
				stream->duration,
				stream->time_base);
		}
	}
	if (!result.duration
		&& raw->duration != AV_NOPTS_VALUE
		&& raw->duration > 0) {
		result.duration = raw->duration / 1000;
	}
	return result;
}

QByteArray Encode(const Pcm &pcm, Format format, EncodeOptions options) {
	if (!Usable(pcm)) {
		return QByteArray();
	}
	const auto bitrate = std::max(options.bitrate, 0);
	switch (format) {
	case Format::OggOpus: {
		const auto voice = options.voice;
		const auto channels = voice ? 1 : std::min(pcm.channels, 2);
		const auto rate = voice ? kVoiceRate : kOpusRate;
		const auto prepared = Resample(pcm, rate, channels);
		if (prepared.empty()) {
			return QByteArray();
		}
		return EncodeWithFFmpeg(prepared, {
			.muxer = "opus"_q,
			.encoder = "libopus",
			.format = AV_SAMPLE_FMT_FLT,
			.bitrate = (bitrate
				? std::clamp(bitrate, 6000, 510000)
				: voice
				? kVoiceBitrate
				: (channels == 1)
				? kOpusBitrateMono
				: kOpusBitrateStereo),
			.title = options.title,
			.performer = options.performer,
		});
	} break;
	case Format::M4a: {
		const auto channels = std::min(pcm.channels, 2);
		const auto rate = (pcm.rate == 44100 || pcm.rate == 48000)
			? pcm.rate
			: (pcm.rate % 11025 == 0)
			? 44100
			: 48000;
		const auto prepared = Resample(pcm, rate, channels);
		if (prepared.empty()) {
			return QByteArray();
		}
		return EncodeWithFFmpeg(prepared, {
			.muxer = "mp4"_q,
			.encoder = "aac",
			.format = AV_SAMPLE_FMT_FLTP,
			.bitrate = (bitrate
				? std::clamp(bitrate, 32000, 512000)
				: (channels == 1)
				? kAacBitrateMono
				: kAacBitrateStereo),
			.title = options.title,
			.performer = options.performer,
		});
	} break;
	case Format::Wav: {
		const auto channels = std::min(pcm.channels, 2);
		return EncodeWav(
			(channels == pcm.channels) ? pcm : Remix(pcm, channels),
			options.title,
			options.performer);
	} break;
	}
	Unexpected("Format in Oblivion::Audio::Encode.");
}

QString FormatExtension(Format format) {
	switch (format) {
	case Format::OggOpus: return u"ogg"_q;
	case Format::M4a: return u"m4a"_q;
	case Format::Wav: return u"wav"_q;
	}
	Unexpected("Format in Oblivion::Audio::FormatExtension.");
}

QString FormatMimeType(Format format) {
	switch (format) {
	case Format::OggOpus: return u"audio/ogg"_q;
	case Format::M4a: return u"audio/mp4"_q;
	case Format::Wav: return u"audio/wav"_q;
	}
	Unexpected("Format in Oblivion::Audio::FormatMimeType.");
}

Pcm Resample(const Pcm &pcm, int rate, int channels) {
	if (!Usable(pcm)
		|| rate < kMinRate
		|| rate > kMaxRate
		|| channels < 0
		|| channels > kMaxChannels) {
		return Pcm();
	}
	if (!channels) {
		channels = pcm.channels;
	}
	if (channels < pcm.channels) {
		// Mix down first, less to resample.
		const auto mixed = Remix(pcm, channels);
		return (rate == pcm.rate) ? mixed : ResampleRate(mixed, rate);
	} else if (rate == pcm.rate) {
		return Remix(pcm, channels);
	}
	const auto resampled = ResampleRate(pcm, rate);
	return resampled.empty() ? Pcm() : Remix(resampled, channels);
}

Pcm ChangeSpeed(const Pcm &pcm, double factor) {
	if (!Usable(pcm) || !std::isfinite(factor) || factor <= 0.) {
		return Pcm();
	}
	factor = std::clamp(factor, 0.25, 4.);
	if (std::abs(factor - 1.) < 1e-6) {
		return pcm;
	}
	// Treat the samples as if they were recorded at rate * factor.
	const auto virtualRate = int(std::lround(pcm.rate * factor));
	auto samples = ResampleSamples(
		pcm.samples.data(),
		pcm.frames(),
		pcm.channels,
		virtualRate,
		pcm.rate);
	if (!samples) {
		return Pcm();
	}
	auto result = Pcm();
	result.samples = std::move(*samples);
	result.channels = pcm.channels;
	result.rate = pcm.rate;
	const auto target = int64(std::llround(pcm.frames() / factor));
	result.samples.resize(size_t(target * pcm.channels), 0.f);
	return result;
}

Pcm ChangeTempo(const Pcm &pcm, double factor) {
	if (!Usable(pcm) || !std::isfinite(factor) || factor <= 0.) {
		return Pcm();
	}
	factor = std::clamp(factor, 0.25, 4.);
	const auto chain = TempoChain(factor);
	if (chain.empty()) {
		return pcm;
	}
	auto samples = ApplyTempo(pcm, chain);
	if (!samples) {
		return Pcm();
	}
	auto result = Pcm();
	result.samples = std::move(*samples);
	result.channels = pcm.channels;
	result.rate = pcm.rate;
	const auto target = int64(std::llround(pcm.frames() / factor));
	result.samples.resize(size_t(target * pcm.channels), 0.f);
	return result;
}

Pcm ShiftPitch(const Pcm &pcm, double semitones) {
	if (!Usable(pcm) || !std::isfinite(semitones)) {
		return Pcm();
	}
	semitones = std::clamp(semitones, -24., 24.);
	if (std::abs(semitones) < 1e-3) {
		return pcm;
	}
	const auto ratio = std::pow(2., semitones / 12.);

	// Stretch keeping the pitch, then play back faster / slower.
	const auto stretched = ChangeTempo(pcm, 1. / ratio);
	if (stretched.empty()) {
		return Pcm();
	}
	auto result = ChangeSpeed(stretched, ratio);
	if (result.empty()) {
		return Pcm();
	}
	result.samples.resize(size_t(pcm.frames() * pcm.channels), 0.f);
	return result;
}

Pcm Reverb(const Pcm &pcm, const ReverbParams &params) {
	if (!Usable(pcm)) {
		return Pcm();
	}
	const auto room = Clamp01(params.roomSize);
	const auto feedback = float(room * kReverbScaleRoom + kReverbOffsetRoom);
	const auto damp1 = float(Clamp01(params.damping) * kReverbScaleDamp);
	const auto damp2 = 1.f - damp1;
	const auto wetGain = float(Clamp01(params.wet) * kReverbScaleWet);
	const auto dryGain = float(Clamp01(params.dry));
	const auto channels = pcm.channels;
	const auto rate = pcm.rate;
	const auto frames = pcm.frames();
	const auto scale = rate / kFreeverbRate;

	// Until the longest comb decays by 60 dB.
	const auto longest = (kCombTuning.back() + kStereoSpread) * scale;
	const auto loops = std::log(1000.) / -std::log(double(feedback));
	const auto natural = int64(std::ceil(loops * longest));
	const auto tail = std::min(natural, MsToFrames(kReverbMaxTail, rate));
	const auto total = frames + tail;

	struct Network {
		std::vector<Comb> combs;
		std::vector<Allpass> allpasses;
	};
	auto networks = std::vector<Network>();
	networks.reserve(channels);
	for (auto c = 0; c != channels; ++c) {
		const auto spread = (c % 2) ? kStereoSpread : 0;
		auto network = Network();
		for (const auto tuning : kCombTuning) {
			network.combs.emplace_back(
				int(std::lround((tuning + spread) * scale)));
		}
		for (const auto tuning : kAllpassTuning) {
			network.allpasses.emplace_back(
				int(std::lround((tuning + spread) * scale)));
		}
		networks.push_back(std::move(network));
	}

	// Freeverb feeds (left + right) * 0.015, keep that level for any
	// channel count, and cut the lows that only make the tail muddy.
	const auto inputGain = float(kReverbFixedGain * 2.);
	const auto rc = 1. / (2. * kPi * kReverbHighPass);
	const auto highPass = float(rc / (rc + 1. / rate));
	auto highPassIn = 0.f;
	auto highPassOut = 0.f;

	auto result = MakePcm(channels, rate, total);
	for (auto f = int64(0); f != total; ++f) {
		const auto inside = (f < frames);
		const auto in = inside
			? (pcm.samples.data() + f * channels)
			: nullptr;
		auto mono = 0.f;
		if (in) {
			for (auto c = 0; c != channels; ++c) {
				mono += Sanitized(in[c]);
			}
			mono /= channels;
		}
		highPassOut = highPass * (highPassOut + mono - highPassIn);
		highPassIn = mono;
		const auto input = highPassOut * inputGain;
		const auto out = result.samples.data() + f * channels;
		for (auto c = 0; c != channels; ++c) {
			auto &network = networks[c];
			auto sum = 0.f;
			for (auto &comb : network.combs) {
				sum += comb.process(input, feedback, damp1, damp2);
			}
			for (auto &allpass : network.allpasses) {
				sum = allpass.process(sum);
			}
			out[c] = (in ? Sanitized(in[c]) * dryGain : 0.f) + sum * wetGain;
		}
	}
	FinishTail(result, frames, natural > tail);
	SafetyLimit(result);
	return result;
}

Pcm Echo(const Pcm &pcm, const EchoParams &params) {
	if (!Usable(pcm)) {
		return Pcm();
	}
	const auto channels = pcm.channels;
	const auto rate = pcm.rate;
	const auto frames = pcm.frames();
	const auto delayMs = std::clamp(params.delay, crl::time(20), crl::time(2000));
	const auto feedback = std::isfinite(params.feedback)
		? std::clamp(params.feedback, 0., 0.95)
		: 0.;
	const auto mix = float(Clamp01(params.mix));
	const auto delay = std::max(MsToFrames(delayMs, rate), int64(1));
	const auto repeats = (feedback > 0.001)
		? int64(std::ceil(std::log(0.001) / std::log(feedback)))
		: int64(1);
	const auto natural = delay * (repeats + 1);
	const auto tail = std::min(natural, MsToFrames(kEchoMaxTail, rate));
	const auto total = frames + tail;

	// Every repeat goes through a gentle low-pass, like tape echo.
	const auto smoothing = float(1. - std::exp(-2. * kPi * std::min(
		kEchoRepeatsCutoff,
		rate * 0.45) / rate));
	auto lines = std::vector<std::vector<float>>(
		channels,
		std::vector<float>(size_t(delay), 0.f));
	auto lowPass = std::vector<float>(channels, 0.f);
	auto index = size_t(0);
	const auto gain = float(feedback);

	auto result = MakePcm(channels, rate, total);
	for (auto f = int64(0); f != total; ++f) {
		const auto in = (f < frames)
			? (pcm.samples.data() + f * channels)
			: nullptr;
		const auto out = result.samples.data() + f * channels;
		for (auto c = 0; c != channels; ++c) {
			const auto x = in ? Sanitized(in[c]) : 0.f;
			auto &line = lines[c];
			const auto delayed = line[index];
			auto &filtered = lowPass[c];
			filtered += smoothing * (delayed - filtered);
			if (std::abs(filtered) < 1e-20f) {
				filtered = 0.f;
			}
			out[c] = x + mix * delayed;
			line[index] = x + gain * filtered;
		}
		if (++index == size_t(delay)) {
			index = 0;
		}
	}
	FinishTail(result, frames, natural > tail);
	SafetyLimit(result);
	return result;
}

Pcm Robot(const Pcm &pcm) {
	if (!Usable(pcm)) {
		return Pcm();
	}
	const auto channels = pcm.channels;
	const auto rate = pcm.rate;
	const auto frames = pcm.frames();
	auto size = int64(256);
	while (size < rate * kRobotWindow) {
		size *= 2;
	}
	const auto hop = std::max(int64(std::lround(rate / kRobotPitch)), int64(1));
	const auto half = size / 2;
	auto window = std::vector<double>(size_t(size));
	for (auto i = int64(0); i != size; ++i) {
		window[i] = 0.5 - 0.5 * std::cos(2. * kPi * i / double(size));
	}
	auto result = MakePcm(channels, rate, frames);
	auto spectrum = std::vector<Complex>(size_t(size));
	const auto fft = Fft(size_t(size));

	// Overlap-add in a window sliding with the frames: output[i] and
	// norm[i] are the sums for the position start + i. Positions before
	// the next frame start get nothing more, so they are written out
	// after each frame and the scratch size doesn't depend on the length.
	const auto span = size + hop;
	auto output = std::vector<double>(size_t(span));
	auto norm = std::vector<double>(size_t(span));
	for (auto c = 0; c != channels; ++c) {
		std::fill(begin(output), end(output), 0.);
		std::fill(begin(norm), end(norm), 0.);
		for (auto start = -half; start < frames; start += hop) {
			for (auto i = int64(0); i != size; ++i) {
				const auto index = start + i;
				const auto value = (index >= 0 && index < frames)
					? double(Sanitized(pcm.samples[index * channels + c]))
					: 0.;
				spectrum[i] = Complex(value * window[i], 0.);
			}
			fft.transform(spectrum, false);

			// Zero phases: every frame becomes a centered pulse,
			// repeated each hop, which gives the monotone buzz.
			for (auto &value : spectrum) {
				value = Complex(Magnitude(value), 0.);
			}
			fft.transform(spectrum, true);
			for (auto i = int64(0); i != size; ++i) {
				const auto shifted = spectrum[(i + half) % size].real();
				output[i] += shifted * window[i];
				norm[i] += window[i] * window[i];
			}
			for (auto i = int64(0); i != hop; ++i) {
				const auto f = start + i;
				if (f >= 0 && f < frames) {
					result.samples[f * channels + c] = float(
						output[i] / std::max(norm[i], 0.1));
				}
			}
			std::copy(begin(output) + hop, end(output), begin(output));
			std::fill(end(output) - hop, end(output), 0.);
			std::copy(begin(norm) + hop, end(norm), begin(norm));
			std::fill(end(norm) - hop, end(norm), 0.);
		}
	}
	const auto before = RmsOf(pcm.samples.data(), frames * channels);
	const auto after = RmsOf(result.samples.data(), frames * channels);
	if (before > 1e-9 && after > 1e-9) {
		Scale(result, before / after);
	}
	SafetyLimit(result);
	return result;
}

Pcm Telephone(const Pcm &pcm) {
	if (!Usable(pcm)) {
		return Pcm();
	}
	const auto channels = pcm.channels;
	const auto rate = pcm.rate;
	const auto frames = pcm.frames();
	auto filters = std::array<Biquad, 4>{
		MakeBiquad(false, kPhoneLow, rate),
		MakeBiquad(false, kPhoneLow, rate),
		MakeBiquad(true, kPhoneHigh, rate),
		MakeBiquad(true, kPhoneHigh, rate),
	};
	auto band = std::vector<double>(size_t(frames));
	auto peak = 0.;
	for (auto f = int64(0); f != frames; ++f) {
		auto mono = 0.;
		for (auto c = 0; c != channels; ++c) {
			mono += Sanitized(pcm.samples[f * channels + c]);
		}
		auto value = mono / channels;
		peak = std::max(peak, std::abs(value));
		for (auto &filter : filters) {
			value = filter.process(value);
		}
		band[f] = value;
	}
	auto result = MakePcm(channels, rate, frames);
	if (peak < 1e-9) {
		return result;
	}

	// Drive relative to the input level (so out-of-band content stays
	// quiet), soft clip, quantize and go back to the input scale with
	// the unity gain for quiet parts.
	const auto drive = 0.8 / peak;
	const auto saturation = std::tanh(kPhoneDrive);
	const auto restore = saturation / (kPhoneDrive * drive);
	const auto companding = std::log1p(kPhoneMu);
	for (auto f = int64(0); f != frames; ++f) {
		const auto clipped = std::tanh(kPhoneDrive * band[f] * drive)
			/ saturation;

		// 8-bit mu-law, like G.711 telephony.
		const auto sign = (clipped < 0.) ? -1. : 1.;
		const auto compressed = std::log1p(kPhoneMu * std::abs(clipped))
			/ companding;
		const auto quantized = std::round(compressed * 127.) / 127.;
		const auto expanded = sign
			* (std::pow(1. + kPhoneMu, quantized) - 1.)
			/ kPhoneMu;
		const auto value = float(expanded * restore);
		for (auto c = 0; c != channels; ++c) {
			result.samples[f * channels + c] = value;
		}
	}
	SafetyLimit(result);
	return result;
}

Pcm Trim(const Pcm &pcm, crl::time from, crl::time till) {
	if (!Usable(pcm) || till <= from) {
		return Pcm();
	}
	const auto frames = pcm.frames();
	const auto start = std::clamp(
		MsToFrames(std::max(from, crl::time(0)), pcm.rate),
		int64(0),
		frames);
	const auto finish = std::clamp(
		MsToFrames(std::max(till, crl::time(0)), pcm.rate),
		int64(0),
		frames);
	if (finish <= start) {
		return Pcm();
	}
	auto result = Pcm();
	result.channels = pcm.channels;
	result.rate = pcm.rate;
	result.samples.assign(
		pcm.samples.begin() + start * pcm.channels,
		pcm.samples.begin() + finish * pcm.channels);
	const auto fade = MsToFrames(kMicroFade, pcm.rate);
	const auto count = finish - start;
	if (start > 0) {
		FadeRange(result, 0, std::min(fade, count / 2), true);
	}
	if (finish < frames) {
		const auto length = std::min(fade, count / 2);
		FadeRange(result, count - length, length, false);
	}
	return result;
}

Pcm Concat(const std::vector<Pcm> &list, crl::time crossfade) {
	auto pointers = std::vector<not_null<const Pcm*>>();
	pointers.reserve(list.size());
	for (const auto &item : list) {
		pointers.push_back(&item);
	}
	return Concat(pointers, { .crossfade = crossfade });
}

Pcm Concat(
		const std::vector<not_null<const Pcm*>> &list,
		ConcatOptions options) {
	const auto usable = [](not_null<const Pcm*> item) {
		return Usable(*item);
	};
	const auto first = ranges::find_if(list, usable);
	if (first == end(list)) {
		return Pcm();
	}
	const auto channels = options.channels
		? options.channels
		: (*first)->channels;
	const auto rate = options.rate ? options.rate : (*first)->rate;
	if (channels < 1
		|| channels > kMaxChannels
		|| rate < kMinRate
		|| rate > kMaxRate) {
		return Pcm();
	}
	const auto fade = MsToFrames(
		std::max(options.crossfade, crl::time(0)),
		rate);

	// Resample gives exactly round(frames * rate / from) frames,
	// so the result can be allocated once.
	auto total = int64(0);
	for (const auto item : list) {
		if (!usable(item)) {
			continue;
		}
		const auto frames = (item->rate == rate)
			? item->frames()
			: int64(std::llround(double(item->frames()) * rate / item->rate));
		total += frames - std::min({ fade, total, frames });
	}
	auto result = Pcm();
	result.channels = channels;
	result.rate = rate;
	result.samples.reserve(size_t(total * channels));
	for (const auto pointer : list) {
		if (!usable(pointer)) {
			continue;
		}
		const auto &item = *pointer;
		auto converted = Pcm();
		const auto same = (item.channels == channels) && (item.rate == rate);
		if (!same) {
			converted = Resample(item, rate, channels);
			if (converted.empty()) {
				return Pcm();
			}
		}
		const auto &part = same ? item : converted;
		const auto was = result.frames();
		const auto length = std::min({ fade, was, part.frames() });
		if (length <= 0) {
			result.samples.insert(
				end(result.samples),
				begin(part.samples),
				end(part.samples));
			continue;
		}
		const auto overlap = result.samples.data()
			+ (was - length) * channels;
		for (auto i = int64(0); i != length; ++i) {
			const auto progress = (i + 0.5) / double(length);
			const auto out = float(std::cos(progress * kPi / 2.));
			const auto in = float(std::sin(progress * kPi / 2.));
			for (auto c = 0; c != channels; ++c) {
				auto &target = overlap[i * channels + c];
				target = target * out + part.samples[i * channels + c] * in;
			}
		}
		result.samples.insert(
			end(result.samples),
			begin(part.samples) + length * channels,
			end(part.samples));
	}
	return result;
}

Pcm Normalize(const Pcm &pcm, double peak) {
	if (!Usable(pcm)) {
		return Pcm();
	}
	peak = std::isfinite(peak) ? std::clamp(peak, 0.001, 1.) : 0.95;
	auto result = pcm;
	const auto current = PeakOf(result.samples);
	if (current < 1e-9) {
		Scale(result, 1.);
		return result;
	}
	Scale(result, peak / current);
	return result;
}

Pcm NormalizeLoudness(const Pcm &pcm, double rmsDb, double peakLimit) {
	if (!Usable(pcm)) {
		return Pcm();
	}
	rmsDb = std::isfinite(rmsDb) ? std::clamp(rmsDb, -60., 0.) : -16.;
	peakLimit = std::isfinite(peakLimit)
		? std::clamp(peakLimit, 0.001, 1.)
		: 0.95;
	auto result = pcm;
	const auto rms = RmsOf(result.samples.data(), int64(result.samples.size()));
	const auto peak = PeakOf(result.samples);
	if (rms < 1e-9 || peak < 1e-9) {
		Scale(result, 1.);
		return result;
	}
	const auto wanted = std::pow(10., rmsDb / 20.) / rms;
	Scale(result, std::min(wanted, peakLimit / peak));
	return result;
}

Pcm Gain(const Pcm &pcm, double decibels) {
	if (!Usable(pcm) || !std::isfinite(decibels)) {
		return Pcm();
	}
	auto result = pcm;
	Scale(result, std::pow(10., std::clamp(decibels, -96., 48.) / 20.));
	return result;
}

Pcm Fade(const Pcm &pcm, crl::time fadeIn, crl::time fadeOut) {
	if (!Usable(pcm)) {
		return Pcm();
	}
	auto result = pcm;
	const auto frames = result.frames();
	const auto in = std::min(
		MsToFrames(std::max(fadeIn, crl::time(0)), pcm.rate),
		frames);
	const auto out = std::min(
		MsToFrames(std::max(fadeOut, crl::time(0)), pcm.rate),
		frames);
	FadeRange(result, 0, in, true);
	FadeRange(result, frames - out, out, false);
	return result;
}

Pcm Reverse(const Pcm &pcm) {
	if (!Usable(pcm)) {
		return Pcm();
	}
	auto result = pcm;
	const auto channels = result.channels;
	const auto frames = result.frames();
	result.samples.resize(size_t(frames * channels));
	for (auto f = int64(0); f != frames; ++f) {
		for (auto c = 0; c != channels; ++c) {
			result.samples[f * channels + c]
				= pcm.samples[(frames - 1 - f) * channels + c];
		}
	}
	return result;
}

Pcm Silence(crl::time duration, int rate, int channels) {
	if (duration <= 0
		|| rate < kMinRate
		|| rate > kMaxRate
		|| channels < 1
		|| channels > kMaxChannels) {
		return Pcm();
	}
	return MakePcm(channels, rate, MsToFrames(duration, rate));
}

double Peak(const Pcm &pcm) {
	return pcm.empty() ? 0. : PeakOf(pcm.samples);
}

double Rms(const Pcm &pcm) {
	return pcm.empty()
		? 0.
		: RmsOf(pcm.samples.data(), int64(pcm.samples.size()));
}

QVector<signed char> MakeVoiceWaveform(const Pcm &pcm) {
	const auto count = int64(Media::Player::kWaveformSamplesCount);
	if (!Usable(pcm) || pcm.frames() < count) {
		return {};
	}
	const auto channels = pcm.channels;
	const auto frames = pcm.frames();

	// Media::Capture keeps the peak of every 10 ms as value / 256.
	auto each = std::max(int64(pcm.rate / 100), int64(1));
	if (frames / each < count) {
		each = std::max(frames / count, int64(1));
	}
	auto collected = std::vector<uchar>();
	collected.reserve(size_t(frames / each + 1));
	auto peak = uint16(0);
	auto mod = int64(0);
	for (auto f = int64(0); f != frames; ++f) {
		auto loudest = 0.f;
		for (auto c = 0; c != channels; ++c) {
			loudest = std::max(
				loudest,
				std::abs(ClampSample(pcm.samples[f * channels + c])));
		}
		const auto value = uint16(std::lround(loudest * 32767.f));
		peak = std::max(peak, value);
		if (++mod == each) {
			mod = 0;
			collected.push_back(uchar(peak / 256));
			peak = 0;
		}
	}

	// The same as CollectWaveform() in media_audio_capture.cpp.
	const auto total = int64(collected.size());
	if (total < count) {
		return {};
	}
	auto peaks = std::vector<uint16>();
	peaks.reserve(size_t(count));
	auto sum = int64(0);
	peak = 0;
	for (auto i = int64(0); i != total; ++i) {
		const auto sample = uint16(uint16(collected[i]) * 256);
		peak = std::max(peak, sample);
		sum += count;
		if (sum >= total) {
			sum -= total;
			peaks.push_back(peak);
			peak = 0;
		}
	}
	auto all = int64(0);
	for (const auto value : peaks) {
		all += value;
	}
	const auto limit = std::max(
		int64(all * 1.8 / std::max(int64(peaks.size()), int64(1))),
		int64(2500));
	auto result = QVector<signed char>();
	result.reserve(int(peaks.size()));
	for (const auto value : peaks) {
		result.push_back(static_cast<signed char>(std::min(
			int64(31),
			std::min(int64(value), limit) * 31 / limit)));
	}
	return result;
}

//
// PreviewPlayer.
//

namespace {

constexpr auto kPreviewBuffers = 4;
constexpr auto kPreviewBufferDuration = crl::time(250);
constexpr auto kPreviewTimerDelay = crl::time(40);
constexpr auto kPreviewCloseDelay = crl::time(5000);

[[nodiscard]] PFNALCSETTHREADCONTEXTPROC ResolveSetThreadContext() {
	static const auto result = [] {
		const auto name = "ALC_EXT_thread_local_context";
		if (!alcIsExtensionPresent(nullptr, name)) {
			LOG(("Oblivion Audio Error: No %1.").arg(name));
			return PFNALCSETTHREADCONTEXTPROC(nullptr);
		}
		return reinterpret_cast<PFNALCSETTHREADCONTEXTPROC>(
			alcGetProcAddress(nullptr, "alcSetThreadContext"));
	}();
	return result;
}

// Makes our context current for this thread only: the app's own
// OpenAL context stays current for all the other threads and
// becomes current here again when the scope ends.
class ContextScope final {
public:
	explicit ContextScope(ALCcontext *context)
	: _set(ResolveSetThreadContext()) {
		_active = _set && context && _set(context);
	}
	~ContextScope() {
		if (_active) {
			alGetError();
			_set(nullptr);
		}
	}

	explicit operator bool() const {
		return _active;
	}

private:
	PFNALCSETTHREADCONTEXTPROC _set = nullptr;
	bool _active = false;

};

[[nodiscard]] std::string PlaybackDeviceName() {
	if (!Core::IsAppLaunched()) {
		return std::string();
	}
	auto id = Webrtc::DeviceResolvedId();
	{
		QMutexLocker lock(Media::Player::internal::audioPlayerMutex());
		id = Media::Audio::Current().playbackDeviceId();
	}
	return id.isDefault() ? std::string() : id.value.toStdString();
}

void PauseAppPlayback() {
	if (!Core::IsAppLaunched()) {
		return;
	}
	const auto instance = Media::Player::instance();
	for (const auto type : { AudioMsgId::Type::Song, AudioMsgId::Type::Voice }) {
		const auto state = instance->getState(type).state;
		if (!Media::Player::IsStoppedOrStopping(state)
			&& !Media::Player::IsPausedOrPausing(state)) {
			instance->pause(type);
		}
	}
}

} // namespace

struct PreviewPlayer::Private final : base::has_weak_ptr {
	Private();
	~Private();

	[[nodiscard]] bool openDevice();
	void closeDevice();
	[[nodiscard]] bool start();
	void halt();
	void fill();
	void tick();
	[[nodiscard]] int64 currentFrame() const;
	[[nodiscard]] crl::time frameTime(int64 frame) const;

	// Must be the last thing done: a subscriber may destroy us.
	void publish(State newState, int64 frame);

	[[nodiscard]] const Pcm &pcm() const {
		static const auto kEmpty = Pcm();
		return data ? *data : kEmpty;
	}

	std::shared_ptr<const Pcm> data; // Usable, at most two channels.
	float volume = 1.f;
	rpl::variable<State> state = State::Stopped;
	rpl::variable<crl::time> position = 0;
	rpl::variable<crl::time> duration = 0;

	int64 offset = 0;
	int64 queueStart = 0;
	int64 queueEnd = 0;
	std::deque<int64> queued;
	std::vector<ALuint> freeBuffers;
	std::vector<int16> converted;

	ALCdevice *device = nullptr;
	ALCcontext *context = nullptr;
	ALuint source = 0;
	std::array<ALuint, kPreviewBuffers> buffers = { { 0 } };

	base::Timer timer;
	base::Timer closeTimer;
};

PreviewPlayer::Private::Private()
: timer([=] { tick(); })
, closeTimer([=] { closeDevice(); }) {
}

PreviewPlayer::Private::~Private() {
	timer.cancel();
	closeTimer.cancel();
	closeDevice();
}

bool PreviewPlayer::Private::openDevice() {
	if (device) {
		return true;
	} else if (!ResolveSetThreadContext()) {
		return false;
	}
	const auto name = PlaybackDeviceName();
	device = alcOpenDevice(name.empty() ? nullptr : name.c_str());
	if (!device && !name.empty()) {
		device = alcOpenDevice(nullptr);
	}
	if (!device) {
		LOG(("Oblivion Audio Error: Could not open a playback device."));
		return false;
	}
	context = alcCreateContext(device, nullptr);
	if (!context) {
		LOG(("Oblivion Audio Error: Could not create a context."));
		alcCloseDevice(device);
		device = nullptr;
		return false;
	}
	auto ok = false;
	{
		const auto scope = ContextScope(context);
		if (scope) {
			alGetError();
			alDistanceModel(AL_NONE);
			alGenSources(1, &source);
			alGenBuffers(kPreviewBuffers, buffers.data());
			alSourcef(source, AL_PITCH, 1.f);
			alSourcef(source, AL_GAIN, volume);
			alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
			alSource3f(source, AL_POSITION, 0.f, 0.f, 0.f);
			alSource3f(source, AL_VELOCITY, 0.f, 0.f, 0.f);
			alSourcei(source, AL_LOOPING, AL_FALSE);
			ok = (alGetError() == AL_NO_ERROR);
		}
	}
	if (!ok) {
		LOG(("Oblivion Audio Error: Could not create a source."));
		closeDevice();
		return false;
	}
	freeBuffers.assign(begin(buffers), end(buffers));
	return true;
}

void PreviewPlayer::Private::closeDevice() {
	if (!device) {
		return;
	}
	if (const auto scope = ContextScope(context)) {
		alSourceStop(source);
		alSourcei(source, AL_BUFFER, AL_NONE);
		alDeleteSources(1, &source);
		alDeleteBuffers(kPreviewBuffers, buffers.data());
	}
	source = 0;
	buffers = { { 0 } };
	freeBuffers.clear();
	queued.clear();
	alcDestroyContext(context);
	context = nullptr;
	alcCloseDevice(device);
	device = nullptr;
}

void PreviewPlayer::Private::halt() {
	if (const auto scope = ContextScope(context)) {
		alSourceStop(source);
		alSourcei(source, AL_BUFFER, AL_NONE);
	}
	freeBuffers.assign(begin(buffers), end(buffers));
	queued.clear();
	queueStart = queueEnd = offset;
}

void PreviewPlayer::Private::fill() {
	const auto &current = pcm();
	const auto frames = current.frames();
	const auto channels = current.channels;
	const auto chunk = std::max(
		MsToFrames(kPreviewBufferDuration, current.rate),
		int64(1));
	const auto format = (channels == 1)
		? AL_FORMAT_MONO16
		: AL_FORMAT_STEREO16;
	while (!freeBuffers.empty() && queueEnd < frames) {
		const auto count = std::min(chunk, frames - queueEnd);
		converted.resize(size_t(count * channels));
		const auto from = current.samples.data() + queueEnd * channels;
		for (auto i = int64(0); i != count * channels; ++i) {
			converted[i] = int16(std::lround(ClampSample(from[i]) * 32767.f));
		}
		const auto buffer = freeBuffers.back();
		freeBuffers.pop_back();
		alBufferData(
			buffer,
			format,
			converted.data(),
			ALsizei(converted.size() * sizeof(int16)),
			current.rate);
		alSourceQueueBuffers(source, 1, &buffer);
		queued.push_back(count);
		queueEnd += count;
	}
}

bool PreviewPlayer::Private::start() {
	if (!openDevice()) {
		return false;
	}
	halt();
	const auto scope = ContextScope(context);
	if (!scope) {
		return false;
	}
	alSourcef(source, AL_GAIN, volume);
	fill();
	alSourcePlay(source);
	if (alGetError() != AL_NO_ERROR) {
		LOG(("Oblivion Audio Error: Could not start the preview."));
		return false;
	}
	closeTimer.cancel();
	timer.callEach(kPreviewTimerDelay);
	return true;
}

int64 PreviewPlayer::Private::currentFrame() const {
	if (state.current() != State::Playing || !device) {
		return offset;
	}
	auto played = ALint(0);
	if (const auto scope = ContextScope(context)) {
		alGetSourcei(source, AL_SAMPLE_OFFSET, &played);
	}
	return std::clamp(queueStart + played, int64(0), pcm().frames());
}

crl::time PreviewPlayer::Private::frameTime(int64 frame) const {
	const auto rate = pcm().rate;
	return (rate > 0) ? (frame * 1000 / rate) : 0;
}

void PreviewPlayer::Private::publish(State newState, int64 frame) {
	const auto weak = base::make_weak(this);
	position = frameTime(frame);
	if (weak) {
		state = newState;
	}
}

void PreviewPlayer::Private::tick() {
	if (!device) {
		timer.cancel();
		return;
	}
	auto finished = false;
	auto lost = false;
	auto frame = int64(0);
	{
		const auto scope = ContextScope(context);
		if (!scope) {
			return;
		}
		// The state goes first: a source that was already stopped has
		// processed all its buffers, so after unqueueing the processed
		// ones nothing old can be played again by alSourcePlay.
		auto sourceState = ALint(0);
		alGetSourcei(source, AL_SOURCE_STATE, &sourceState);
		auto processed = ALint(0);
		alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
		while (processed-- > 0 && !queued.empty()) {
			auto buffer = ALuint(0);
			alSourceUnqueueBuffers(source, 1, &buffer);
			queueStart += queued.front();
			queued.pop_front();
			freeBuffers.push_back(buffer);
		}
		fill();
		if (sourceState != AL_PLAYING) {
			if (queued.empty()) {
				finished = true;
			} else {
				// Buffer underrun, the main thread was busy.
				alSourcePlay(source);
			}
		}
		if (alcIsExtensionPresent(device, "ALC_EXT_disconnect")) {
			auto connected = ALCint(1);
			alcGetIntegerv(device, ALC_CONNECTED, 1, &connected);
			lost = !connected;
		}
		auto played = ALint(0);
		alGetSourcei(source, AL_SAMPLE_OFFSET, &played);
		frame = std::clamp(queueStart + played, int64(0), pcm().frames());
	}
	if (finished) {
		timer.cancel();
		offset = 0;
		halt();
		closeTimer.callOnce(kPreviewCloseDelay);
		publish(State::Stopped, 0);
	} else if (lost) {
		LOG(("Oblivion Audio: Preview device disconnected."));
		timer.cancel();
		offset = frame;
		closeDevice();
		publish(State::Paused, frame);
	} else {
		position = frameTime(frame);
	}
}

PreviewPlayer::PreviewPlayer()
: _private(std::make_unique<Private>()) {
}

PreviewPlayer::~PreviewPlayer() = default;

void PreviewPlayer::setPcm(Pcm pcm) {
	setPcm(Usable(pcm)
		? std::make_shared<const Pcm>(std::move(pcm))
		: nullptr);
}

void PreviewPlayer::setPcm(std::shared_ptr<const Pcm> pcm) {
	const auto d = _private.get();
	d->timer.cancel();
	d->offset = 0;
	d->halt();
	if (d->device) {
		d->closeTimer.callOnce(kPreviewCloseDelay);
	}
	d->data = (!pcm || !Usable(*pcm))
		? nullptr
		: (pcm->channels > 2)
		? std::make_shared<const Pcm>(Remix(*pcm, 2))
		: std::move(pcm);
	const auto weak = base::make_weak(d);
	d->duration = d->pcm().duration();
	if (weak) {
		d->publish(State::Stopped, 0);
	}
}

void PreviewPlayer::play() {
	const auto d = _private.get();
	if (d->pcm().empty() || d->state.current() == State::Playing) {
		return;
	}
	if (d->offset >= d->pcm().frames()) {
		d->offset = 0;
	}
	PauseAppPlayback();
	if (!d->start()) {
		d->halt();
		d->publish(State::Stopped, d->offset);
		return;
	}
	d->publish(State::Playing, d->offset);
}

void PreviewPlayer::pause() {
	const auto d = _private.get();
	if (d->state.current() != State::Playing) {
		return;
	}
	d->offset = d->currentFrame();
	d->timer.cancel();
	d->halt();
	d->closeTimer.callOnce(kPreviewCloseDelay);
	d->publish(State::Paused, d->offset);
}

void PreviewPlayer::stop() {
	const auto d = _private.get();
	d->timer.cancel();
	d->offset = 0;
	d->halt();
	if (d->device) {
		d->closeTimer.callOnce(kPreviewCloseDelay);
	}
	d->publish(State::Stopped, 0);
}

void PreviewPlayer::seek(crl::time position) {
	const auto d = _private.get();
	const auto frames = d->pcm().frames();
	d->offset = std::clamp(
		MsToFrames(std::max(position, crl::time(0)), std::max(d->pcm().rate, 1)),
		int64(0),
		frames);
	if (d->state.current() != State::Playing) {
		d->position = d->frameTime(d->offset);
		return;
	} else if (d->offset >= frames) {
		stop();
		return;
	} else if (!d->start()) {
		d->timer.cancel();
		d->halt();
		d->publish(State::Paused, d->offset);
		return;
	}
	d->position = d->frameTime(d->offset);
}

void PreviewPlayer::setVolume(float64 volume) {
	const auto d = _private.get();
	d->volume = float(Clamp01(volume));
	if (d->device) {
		if (const auto scope = ContextScope(d->context)) {
			alSourcef(d->source, AL_GAIN, d->volume);
		}
	}
}

crl::time PreviewPlayer::position() const {
	return _private->position.current();
}

crl::time PreviewPlayer::duration() const {
	return _private->duration.current();
}

PreviewPlayer::State PreviewPlayer::state() const {
	return _private->state.current();
}

rpl::producer<PreviewPlayer::State> PreviewPlayer::stateValue() const {
	return _private->state.value();
}

rpl::producer<crl::time> PreviewPlayer::positionValue() const {
	return _private->position.value();
}

rpl::producer<crl::time> PreviewPlayer::durationValue() const {
	return _private->duration.value();
}

//
// Self-test.
//

bool RunSelfTest(QStringList &log) {
	auto passed = 0;
	auto failed = 0;
	const auto check = [&](
			bool condition,
			const QString &name,
			const QString &details = QString()) {
		log.push_back(u"audio: "_q
			+ (condition ? u"OK   "_q : u"FAIL "_q)
			+ name
			+ (details.isEmpty() ? QString() : (u" ("_q + details + ')')));
		++(condition ? passed : failed);
		return condition;
	};
	// Not "near": that is an empty macro in the Windows headers.
	const auto nearly = [](double value, double expected, double tolerance) {
		return std::isfinite(value)
			&& (std::abs(value - expected)
				<= std::abs(expected) * tolerance);
	};
	const auto timer = std::make_shared<QElapsedTimer>();
	const auto elapsed = [=] {
		return u", "_q + QString::number(timer->restart()) + u" ms"_q;
	};
	timer->start();

	try {
		// Signals.
		const auto sine = MakeTone(48000, 1, 3000, 440., 0.5);
		const auto stereo = MakeTone(44100, 2, 2000, 440., 0.5, 660.);
		const auto chirp = MakeChirp(48000, 2000, 200., 2000., 0.5);
		const auto silence = Silence(1000, 48000, 1);
		const auto sineRms = 0.5 / std::sqrt(2.);
		check(
			sine.frames() == 144000 && silence.frames() == 48000,
			u"signals"_q,
			u"sine frames %1, silence frames %2"_q
				.arg(sine.frames()).arg(silence.frames()));
		const auto base = DominantFrequency(sine);
		check(
			nearly(base, 440., 0.005),
			u"frequency estimator"_q,
			u"440 Hz measured as %1 Hz"_q.arg(Number(base)));

		// Encode / decode round-trips.
		struct Trip {
			Format format;
			QString name;
			QByteArray magic;
			int magicOffset = 0;
		};
		const auto trips = std::vector<Trip>{
			{ Format::Wav, u"wav"_q, "RIFF", 0 },
			{ Format::M4a, u"m4a"_q, "ftyp", 4 },
			{ Format::OggOpus, u"ogg"_q, "OggS", 0 },
		};
		auto wavBytes = QByteArray();
		for (const auto &trip : trips) {
			for (const auto input : { &sine, &stereo }) {
				const auto label = trip.name
					+ (input->channels == 1
						? u" mono 48k"_q
						: u" stereo 44.1k"_q);
				const auto bytes = Encode(*input, trip.format);
				const auto magic = (bytes.size() > trip.magicOffset + 4)
					&& (bytes.mid(trip.magicOffset, 4) == trip.magic);
				if (!check(
						magic,
						label + u" encode"_q,
						u"%1 bytes"_q.arg(bytes.size()) + elapsed())) {
					continue;
				}
				if (trip.format == Format::Wav && input == &sine) {
					wavBytes = bytes;
				}
				const auto decoded = Decode(bytes);
				if (!check(
						decoded.has_value(),
						label + u" decode"_q,
						u"%1 frames"_q.arg(decoded ? decoded->frames() : 0)
							+ elapsed())) {
					continue;
				}
				const auto seconds = decoded->seconds();
				const auto level = MiddleRms(*decoded);
				const auto expectedLevel = sineRms;
				const auto levelDb = 20. * std::log10(
					std::max(level, 1e-9) / expectedLevel);
				const auto left = DominantFrequency(*decoded, 0);
				check(
					nearly(seconds, input->seconds(), 0.02)
						&& decoded->channels == input->channels
						&& std::abs(levelDb) < 1.5
						&& nearly(left, 440., 0.02),
					label + u" round-trip"_q,
					u"%1 s of %2 s, %3 ch, %4 Hz, level %5 dB, rate %6"_q
						.arg(Number(seconds, 3))
						.arg(Number(input->seconds(), 3))
						.arg(decoded->channels)
						.arg(Number(left))
						.arg(Number(levelDb))
						.arg(decoded->rate));
				if (input->channels == 2 && decoded->channels == 2) {
					const auto right = DominantFrequency(*decoded, 1);
					check(
						nearly(right, 660., 0.02),
						label + u" right channel"_q,
						u"%1 Hz"_q.arg(Number(right)));
				}
			}
		}

		// Title / artist tags.
		struct TagTrip {
			Format format;
			QString name;
			bool streamTags = false; // The stream metadata may hold them.
		};
		const auto tagTrips = std::vector<TagTrip>{
			{ Format::M4a, u"m4a"_q, false },
			{ Format::OggOpus, u"ogg"_q, true },
			{ Format::Wav, u"wav"_q, false },
		};
		const auto tagTone = MakeTone(48000, 2, 1000, 440., 0.5);
		const auto tags = EncodeOptions{
			.title = u"Sine — Тест (slowed + reverb)"_q,
			.performer = u"Oblivion Sélf-Test"_q,
		};
		for (const auto &trip : tagTrips) {
			const auto bytes = Encode(tagTone, trip.format, tags);
			const auto read = ReadTagsFrom(bytes);
			const auto decoded = Decode(bytes);
			const auto inFormat = read
				&& (read->formatTitle == tags.title)
				&& (read->formatArtist == tags.performer);
			const auto inStream = read
				&& (read->streamTitle == tags.title)
				&& (read->streamArtist == tags.performer);
			check(
				(inFormat || (trip.streamTags && inStream))
					&& decoded
					&& nearly(decoded->seconds(), tagTone.seconds(), 0.05),
				trip.name + u" title / artist tags"_q,
				(read
					? u"format '%1' / '%2', stream '%3' / '%4'"_q.arg(
						read->formatTitle,
						read->formatArtist,
						read->streamTitle,
						read->streamArtist)
					: u"unreadable, %1 bytes"_q.arg(bytes.size()))
					+ u", %1 s"_q.arg(decoded
						? Number(decoded->seconds(), 3)
						: u"-"_q)
					+ elapsed());
			const auto plain = ReadTagsFrom(Encode(tagTone, trip.format));
			check(
				plain
					&& plain->formatTitle.isEmpty()
					&& plain->formatArtist.isEmpty()
					&& plain->streamTitle.isEmpty()
					&& plain->streamArtist.isEmpty(),
				trip.name + u" no tags by default"_q,
				plain
					? (plain->formatTitle
						+ plain->formatArtist
						+ plain->streamTitle
						+ plain->streamArtist)
					: u"unreadable"_q);
		}

		// Voice message.
		const auto voice = Encode(sine, Format::OggOpus, { .voice = true });
		const auto voiceDecoded = Decode(voice);
		check(
			voiceDecoded
				&& voiceDecoded->channels == 1
				&& voiceDecoded->rate == 48000
				&& nearly(voiceDecoded->seconds(), 3., 0.02)
				&& nearly(DominantFrequency(*voiceDecoded), 440., 0.02),
			u"voice ogg opus"_q,
			u"%1 bytes (%2 kbps), %3 s"_q
				.arg(voice.size())
				.arg(Number(voice.size() * 8. / 3000., 1))
				.arg(voiceDecoded
					? Number(voiceDecoded->seconds(), 3)
					: u"-"_q)
				+ elapsed());

		// Decode options and files.
		const auto converted = Decode(
			wavBytes,
			{ .rate = 16000, .channels = 2, .maxDuration = 2000 });
		check(
			converted
				&& converted->rate == 16000
				&& converted->channels == 2
				&& converted->frames() == 32000
				&& nearly(DominantFrequency(*converted, 1), 440., 0.01),
			u"decode with options"_q,
			converted
				? u"%1 Hz, %2 ch, %3 frames"_q
					.arg(converted->rate)
					.arg(converted->channels)
					.arg(converted->frames())
				: u"failed"_q);
		const auto path = QDir::current().absoluteFilePath(
			u"oblivion_selftest_audio.wav"_q);
		auto file = QFile(path);
		if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			file.write(wavBytes);
			file.close();
		}
		const auto fromFile = Decode(path);
		const auto info = Probe(path);
		check(
			fromFile
				&& fromFile->frames() == sine.frames()
				&& info
				&& info->rate == 48000
				&& info->channels == 1
				&& !info->hasVideo
				&& std::abs(info->duration - 3000) <= 20,
			u"decode / probe file"_q,
			u"%1 frames, probe %2 ms"_q
				.arg(fromFile ? fromFile->frames() : 0)
				.arg(info ? info->duration : 0));
		QFile::remove(path);

		// Resampling.
		const auto resampled = Resample(sine, 44100);
		check(
			resampled.frames() == 132300
				&& nearly(DominantFrequency(resampled), 440., 0.005),
			u"resample 48k -> 44.1k"_q,
			u"%1 frames, %2 Hz"_q
				.arg(resampled.frames())
				.arg(Number(DominantFrequency(resampled)))
				+ elapsed());
		const auto upmixed = Resample(sine, 48000, 2);
		const auto downmixed = Resample(stereo, 22050, 1);
		check(
			upmixed.channels == 2
				&& upmixed.frames() == sine.frames()
				&& downmixed.channels == 1
				&& downmixed.frames() == 44100,
			u"remix"_q,
			u"up %1 ch, down %2 ch %3 frames"_q
				.arg(upmixed.channels)
				.arg(downmixed.channels)
				.arg(downmixed.frames()));

		// Speed / tempo / pitch.
		const auto slowed = ChangeSpeed(sine, 0.8);
		check(
			nearly(slowed.seconds(), 3.75, 0.02)
				&& nearly(DominantFrequency(slowed), 352., 0.02),
			u"speed x0.8"_q,
			u"%1 s, %2 Hz"_q
				.arg(Number(slowed.seconds(), 3))
				.arg(Number(DominantFrequency(slowed)))
				+ elapsed());
		const auto faster = ChangeSpeed(stereo, 1.25);
		check(
			nearly(faster.seconds(), 1.6, 0.02)
				&& nearly(DominantFrequency(faster, 1), 825., 0.02),
			u"speed x1.25 stereo"_q,
			u"%1 s, right %2 Hz"_q
				.arg(Number(faster.seconds(), 3))
				.arg(Number(DominantFrequency(faster, 1))));
		for (const auto factor : { 0.5, 1.5, 0.3, 3. }) {
			const auto stretched = ChangeTempo(sine, factor);
			const auto frequency = DominantFrequency(stretched);
			check(
				nearly(stretched.seconds(), 3. / factor, 0.02)
					&& nearly(frequency, 440., 0.02)
					&& AllFinite(stretched),
				u"tempo x%1"_q.arg(Number(factor, 2)),
				u"%1 s, %2 Hz, rms %3"_q
					.arg(Number(stretched.seconds(), 3))
					.arg(Number(frequency))
					.arg(Number(MiddleRms(stretched), 3))
					+ elapsed());
		}
		const auto chirpFast = ChangeTempo(chirp, 2.);
		check(
			chirpFast.frames() == chirp.frames() / 2 && AllFinite(chirpFast),
			u"tempo x2 chirp"_q,
			u"%1 frames"_q.arg(chirpFast.frames()));
		for (const auto semitones : { 12., -7., 5. }) {
			const auto shifted = ShiftPitch(sine, semitones);
			const auto expected = 440. * std::pow(2., semitones / 12.);
			const auto frequency = DominantFrequency(shifted);
			check(
				shifted.frames() == sine.frames()
					&& nearly(frequency, expected, 0.02)
					&& MiddleRms(shifted) > sineRms * 0.5
					&& AllFinite(shifted),
				u"pitch %1 semitones"_q.arg(Number(semitones, 0)),
				u"%1 s, %2 Hz of %3 Hz, rms %4"_q
					.arg(Number(shifted.seconds(), 3))
					.arg(Number(frequency))
					.arg(Number(expected))
					.arg(Number(MiddleRms(shifted), 3))
					+ elapsed());
		}
		const auto shiftedStereo = ShiftPitch(stereo, 5.);
		const auto ratio = std::pow(2., 5. / 12.);
		check(
			shiftedStereo.channels == 2
				&& shiftedStereo.frames() == stereo.frames()
				&& nearly(DominantFrequency(shiftedStereo, 0), 440. * ratio, 0.02)
				&& nearly(DominantFrequency(shiftedStereo, 1), 660. * ratio, 0.02),
			u"pitch +5 stereo"_q,
			u"left %1 Hz, right %2 Hz"_q
				.arg(Number(DominantFrequency(shiftedStereo, 0)))
				.arg(Number(DominantFrequency(shiftedStereo, 1))));

		// Effects.
		struct Effect {
			QString name;
			Fn<Pcm(const Pcm&)> apply;
		};
		const auto effects = std::vector<Effect>{
			{ u"reverb"_q, [](const Pcm &pcm) { return Reverb(pcm); } },
			{ u"reverb large"_q, [](const Pcm &pcm) {
				return Reverb(pcm, {
					.roomSize = 1.,
					.damping = 0.,
					.wet = 1.,
					.dry = 1.,
				});
			} },
			{ u"echo"_q, [](const Pcm &pcm) { return Echo(pcm); } },
			{ u"echo max"_q, [](const Pcm &pcm) {
				return Echo(pcm, {
					.delay = 50,
					.feedback = 0.95,
					.mix = 1.,
				});
			} },
			{ u"robot"_q, [](const Pcm &pcm) { return Robot(pcm); } },
			{ u"telephone"_q, [](const Pcm &pcm) { return Telephone(pcm); } },
		};
		for (const auto &effect : effects) {
			for (const auto input : { &sine, &stereo, &chirp }) {
				const auto label = effect.name + u" on "_q + (input == &sine
					? u"sine"_q
					: input == &stereo
					? u"stereo"_q
					: u"chirp"_q);
				const auto result = effect.apply(*input);
				const auto normalized = Normalize(result);
				const auto peak = Peak(result);
				check(
					!result.empty()
						&& AllFinite(result)
						&& result.channels == input->channels
						&& result.frames() >= input->frames()
						&& peak <= kSafetyPeak + 1e-6
						&& MiddleRms(result) > 0.01
						&& std::abs(Peak(normalized) - 0.95) < 1e-4,
					label,
					u"%1 s, peak %2, rms %3"_q
						.arg(Number(result.seconds(), 3))
						.arg(Number(peak, 3))
						.arg(Number(MiddleRms(result), 3))
						+ elapsed());
			}
			const auto quiet = effect.apply(silence);
			check(
				!quiet.empty() && AllFinite(quiet) && Peak(quiet) < 1e-3,
				effect.name + u" on silence"_q,
				u"peak %1"_q.arg(Number(Peak(quiet), 6)));
		}
		const auto tiny = MakeTone(48000, 2, 1, 440., 0.5);
		const auto tinyReverb = Reverb(tiny);
		const auto tinyRobot = Robot(tiny);
		const auto tinyPitch = ShiftPitch(tiny, 3.);
		check(
			AllFinite(tinyReverb) && AllFinite(tinyRobot)
				&& tinyPitch.frames() == tiny.frames(),
			u"1 ms input"_q,
			u"%1 frames"_q.arg(tiny.frames()));
		const auto slowedReverb = Reverb(
			ChangeSpeed(stereo, 0.85),
			{ .roomSize = 0.85, .damping = 0.4, .wet = 0.45 });
		check(
			!slowedReverb.empty()
				&& AllFinite(slowedReverb)
				&& Peak(slowedReverb) <= kSafetyPeak + 1e-6,
			u"slowed + reverb"_q,
			u"%1 s, peak %2"_q
				.arg(Number(slowedReverb.seconds(), 3))
				.arg(Number(Peak(slowedReverb), 3))
				+ elapsed());

		// Effect behaviour.
		const auto relativeDb = [](double value, double base) {
			return 20. * std::log10(std::max(value, 1e-9) / std::max(base, 1e-9));
		};
		const auto phone = [&](double frequency) {
			return MiddleRms(Telephone(MakeTone(48000, 1, 1000, frequency, 0.1)));
		};
		const auto phoneMid = phone(1000.);
		const auto phoneLow = relativeDb(phone(100.), phoneMid);
		const auto phoneHigh = relativeDb(phone(8000.), phoneMid);
		check(
			phoneMid > 0.03 && phoneLow < -20. && phoneHigh < -20.,
			u"telephone band"_q,
			u"1 kHz rms %1, 100 Hz %2 dB, 8 kHz %3 dB"_q
				.arg(Number(phoneMid, 3))
				.arg(Number(phoneLow, 1))
				.arg(Number(phoneHigh, 1)));

		const auto periodicity = [](const Pcm &pcm, int64 lag) {
			const auto from = pcm.frames() / 4;
			const auto till = pcm.frames() * 3 / 4;
			auto energy = 0.;
			auto correlation = 0.;
			for (auto f = from; f < till && f + lag < pcm.frames(); ++f) {
				energy += double(pcm.samples[f]) * pcm.samples[f];
				correlation += double(pcm.samples[f]) * pcm.samples[f + lag];
			}
			return (energy > 0.) ? (correlation / energy) : 0.;
		};
		const auto robotLag = int64(std::lround(48000 / kRobotPitch));
		const auto robotBefore = periodicity(chirp, robotLag);
		const auto robotAfter = periodicity(Robot(chirp), robotLag);
		check(
			robotAfter > 0.5 && robotAfter > robotBefore + 0.3,
			u"robot buzz"_q,
			u"10 ms autocorrelation %1 -> %2"_q
				.arg(Number(robotBefore, 2))
				.arg(Number(robotAfter, 2)));

		auto impulse = Silence(100, 48000, 1);
		impulse.samples[0] = 0.9f;
		const auto tail = Reverb(impulse, { .wet = 1., .dry = 0. });
		const auto window = int64(2400);
		auto envelope = std::vector<double>();
		for (auto f = int64(0); f + window <= tail.frames(); f += window) {
			envelope.push_back(RmsOf(tail.samples.data() + f, window));
		}
		const auto loudest = envelope.empty()
			? envelope.end()
			: std::max_element(envelope.begin(), envelope.end());
		auto decay40 = -1.;
		if (loudest != envelope.end()) {
			for (auto i = loudest; i != envelope.end(); ++i) {
				if (relativeDb(*i, *loudest) < -40.) {
					decay40 = (i - envelope.begin()) * 0.05;
					break;
				}
			}
		}
		check(
			decay40 > 0.3 && decay40 < 3. && AllFinite(tail),
			u"reverb decay"_q,
			u"-40 dB after %1 s (RT60 about %2 s), tail %3 s"_q
				.arg(Number(decay40, 2))
				.arg(Number(decay40 * 1.5, 2))
				.arg(Number(tail.seconds(), 2)));

		const auto echoed = Echo(impulse, { .delay = 300, .feedback = 0.4, .mix = 0.5 });
		auto firstAt = int64(1);
		for (auto f = int64(1); f < std::min(echoed.frames(), int64(20000)); ++f) {
			if (std::abs(echoed.samples[f]) > std::abs(echoed.samples[firstAt])) {
				firstAt = f;
			}
		}
		auto second = 0.;
		for (auto f = int64(28800); f < std::min(echoed.frames(), int64(29800)); ++f) {
			second += echoed.samples[f];
		}
		check(
			firstAt == 14400
				&& std::abs(echoed.samples[firstAt] - 0.45) < 0.01
				&& nearly(second, 0.18, 0.1),
			u"echo timing"_q,
			u"first at %1 ms = %2, second sum %3"_q
				.arg(firstAt * 1000 / 48000)
				.arg(Number(echoed.samples[firstAt], 3))
				.arg(Number(second, 3)));

		const auto lastPeak = [](const Pcm &pcm, int64 count) {
			auto result = 0.f;
			const auto from = std::max(pcm.frames() - count, int64(0));
			for (auto f = from; f != pcm.frames(); ++f) {
				result = std::max(result, std::abs(pcm.samples[f * pcm.channels]));
			}
			return result;
		};
		const auto longEcho = Echo(sine, {
			.delay = 2000,
			.feedback = 0.95,
			.mix = 1.,
		});
		const auto longReverb = Reverb(sine, {
			.roomSize = 1.,
			.damping = 0.,
			.wet = 1.,
		});
		check(
			longEcho.duration() > 12900
				&& longReverb.duration() > 10900
				&& lastPeak(longEcho, 48) < 1e-3
				&& lastPeak(longReverb, 48) < 1e-3,
			u"limited tails fade out"_q,
			u"echo %1 ms ends at %2, reverb %3 ms ends at %4"_q
				.arg(longEcho.duration())
				.arg(Number(lastPeak(longEcho, 48), 5))
				.arg(longReverb.duration())
				.arg(Number(lastPeak(longReverb, 48), 5)));

		// Editing.
		const auto trimmed = Trim(sine, 500, 2000);
		check(
			trimmed.frames() == 72000,
			u"trim 500..2000 ms"_q,
			u"%1 frames"_q.arg(trimmed.frames()));
		check(
			Trim(sine, 2000, 500).empty()
				&& Trim(sine, -100, 999999).frames() == sine.frames()
				&& Trim(sine, 5000, 6000).empty(),
			u"trim edge cases"_q);
		const auto one = Trim(sine, 0, 1000);
		const auto two = Trim(sine, 0, 2000);
		const auto joined = Concat({ one, two });
		const auto faded = Concat({ one, Pcm(), two }, 100);
		const auto mixed = Concat({ one, Trim(stereo, 0, 1000) });
		check(
			joined.frames() == 144000
				&& faded.frames() == 139200
				&& mixed.frames() == 96000
				&& mixed.channels == 1
				&& mixed.rate == 48000
				&& Concat({}).empty(),
			u"concat"_q,
			u"plain %1, crossfade %2, converted %3 frames"_q
				.arg(joined.frames())
				.arg(faded.frames())
				.arg(mixed.frames()));
		const auto formatted = Concat(
			{ &one, &stereo },
			{ .rate = 48000, .channels = 2 });
		const auto crossfaded = Concat(
			{ &stereo, &one, &two },
			{ .crossfade = 100 });
		check(
			formatted.channels == 2
				&& formatted.rate == 48000
				&& formatted.frames() == 144000
				&& formatted.samples.capacity() == formatted.samples.size()
				&& crossfaded.channels == 2
				&& crossfaded.rate == 44100
				&& crossfaded.frames() == 211680
				&& crossfaded.samples.capacity() == crossfaded.samples.size(),
			u"concat to a format"_q,
			u"upmixed %1, crossfaded %2 frames"_q
				.arg(formatted.frames())
				.arg(crossfaded.frames()));
		const auto loud = Normalize(Gain(sine, 12.));
		const auto leveled = NormalizeLoudness(sine, -20.);
		const auto limited = NormalizeLoudness(sine, -3.);
		check(
			std::abs(Peak(loud) - 0.95) < 1e-4
				&& nearly(Rms(leveled), 0.1, 0.02)
				&& Peak(limited) <= 0.95 + 1e-4,
			u"normalize"_q,
			u"peak %1, rms %2, limited peak %3"_q
				.arg(Number(Peak(loud), 4))
				.arg(Number(Rms(leveled), 4))
				.arg(Number(Peak(limited), 4)));
		const auto fadedEdges = Fade(sine, 100, 100);
		const auto reversed = Reverse(chirp);
		check(
			std::abs(fadedEdges.samples.front()) < 1e-3
				&& std::abs(fadedEdges.samples.back()) < 1e-3
				&& reversed.frames() == chirp.frames()
				&& reversed.samples.front() == chirp.samples.back(),
			u"fade / reverse"_q);

		// Voice waveform.
		const auto waveform = MakeVoiceWaveform(sine);
		const auto quietWaveform = MakeVoiceWaveform(silence);
		const auto chirpWaveform = MakeVoiceWaveform(Fade(chirp, 1000, 0));
		const auto inRange = [](const QVector<signed char> &values) {
			for (const auto value : values) {
				if (value < 0 || value > 31) {
					return false;
				}
			}
			return true;
		};
		auto waveformText = QStringList();
		for (const auto value : chirpWaveform.mid(0, 12)) {
			waveformText.push_back(QString::number(value));
		}
		check(
			waveform.size() == 100
				&& inRange(waveform)
				&& waveform[50] > 0
				&& quietWaveform.size() == 100
				&& ranges::all_of(quietWaveform, [](auto v) { return !v; })
				&& chirpWaveform.size() == 100
				&& inRange(chirpWaveform)
				&& chirpWaveform.front() < chirpWaveform.back()
				&& MakeVoiceWaveform(MakeTone(48000, 1, 1, 440., 0.5)).isEmpty(),
			u"voice waveform"_q,
			u"sine %1, fade-in %2..."_q
				.arg(waveform.isEmpty() ? -1 : int(waveform[50]))
				.arg(waveformText.join(',')));

		// Bad input.
		check(
			!Decode(QByteArray("definitely not audio data"))
				&& !Decode(QByteArray())
				&& !Decode(u"/nonexistent/oblivion.ogg"_q)
				&& Encode(Pcm(), Format::OggOpus).isEmpty()
				&& ChangeTempo(Pcm(), 2.).empty()
				&& ShiftPitch(sine, std::nan("")).empty()
				&& ChangeSpeed(sine, -1.).empty()
				&& Resample(sine, 1000).empty()
				&& Normalize(silence).frames() == silence.frames(),
			u"bad input"_q);
	} catch (const std::exception &e) {
		check(false, u"exception"_q, QString::fromUtf8(e.what()));
	} catch (...) {
		check(false, u"exception"_q, u"unknown"_q);
	}
	log.push_back(u"audio: %1 passed, %2 failed"_q.arg(passed).arg(failed));
	return !failed;
}

} // namespace Oblivion::Audio
