/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_noise.h"

#include <QtCore/QElapsedTimer>

// <rnnoise.h> is not on the include path of this target (see the header),
// these are its declarations for the functions used here.
extern "C" {

struct DenoiseState;
struct RNNModel;

int rnnoise_get_frame_size();
DenoiseState *rnnoise_create(RNNModel *model);
void rnnoise_destroy(DenoiseState *st);
float rnnoise_process_frame(DenoiseState *st, float *out, const float *in);

} // extern "C"

namespace Oblivion::Noise {
namespace {

// RNNoise expects floats in the 16 bit integer range.
constexpr auto kScale = 32768.f;

// How often a long job asks whether it was cancelled (1.28 seconds).
constexpr auto kCancelCheckFrames = 128;

struct StateDeleter {
	void operator()(DenoiseState *state) const {
		rnnoise_destroy(state);
	}
};
using StatePointer = std::unique_ptr<DenoiseState, StateDeleter>;

[[nodiscard]] StatePointer CreateState() {
	if (rnnoise_get_frame_size() != kFrameSize) {
		return StatePointer();
	}
	// RNNoise fills the tables shared by all the states in the first
	// rnnoise_process_frame() without any lock. A throwaway state runs
	// one frame before any real one (once: a local static is initialized
	// under a lock), so jobs on different threads never fill them at the
	// same time. Nothing here is destroyed at exit.
	[[maybe_unused]] static const auto warmed = [] {
		const auto state = StatePointer(rnnoise_create(nullptr));
		if (!state) {
			return false;
		}
		float input[kFrameSize] = { 0.f };
		float output[kFrameSize] = { 0.f };
		rnnoise_process_frame(state.get(), output, input);
		return true;
	}();
	return StatePointer(rnnoise_create(nullptr));
}

[[nodiscard]] float MixFor(float64 strength) {
	return std::isfinite(strength)
		? float(std::clamp(strength, 0., 1.))
		: 1.f;
}

[[nodiscard]] float Finite(float value) {
	return std::isfinite(value) ? value : 0.f;
}

// 48 kHz mono samples are processed in place, the count is kept.
//
// RNNoise returns every frame one frame later (the windows overlap), so
// one more frame of silence is fed in at the end and the first returned
// frame is dropped: the result is aligned with the source. That is also
// why it can be written in place, it never gets ahead of the reading.
[[nodiscard]] bool DenoiseMono(
		std::vector<float> &samples,
		float mix,
		const Fn<bool()> &cancelled) {
	const auto total = int64(samples.size());
	if (mix <= 0.f) {
		for (auto &sample : samples) {
			sample = Finite(sample);
		}
		return true;
	} else if (!total) {
		return true;
	}
	const auto state = CreateState();
	if (!state) {
		LOG(("Oblivion Noise Error: Could not create the RNNoise state."));
		return false;
	}
	float input[kFrameSize] = { 0.f };
	float output[kFrameSize] = { 0.f };
	float current[kFrameSize] = { 0.f };
	float previous[kFrameSize] = { 0.f };
	auto frame = 0;
	const auto data = samples.data();
	for (auto position = int64(0)
		; position < total + kFrameSize
		; position += kFrameSize, ++frame) {
		for (auto i = 0; i != kFrameSize; ++i) {
			const auto index = position + i;
			current[i] = (index < total) ? Finite(data[index]) : 0.f;
			input[i] = current[i] * kScale;
		}
		rnnoise_process_frame(state.get(), output, input);
		if (position > 0) {
			const auto start = position - kFrameSize;
			const auto count = int(std::min(int64(kFrameSize), total - start));
			for (auto i = 0; i < count; ++i) {
				const auto clean = Finite(output[i] / kScale);
				data[start + i] = previous[i] + (clean - previous[i]) * mix;
			}
		}
		std::copy(current, current + kFrameSize, previous);
		if (cancelled
			&& ((frame % kCancelCheckFrames) == (kCancelCheckFrames - 1))
			&& cancelled()) {
			return false;
		}
	}
	return true;
}

// channel < 0 gives the average of all the channels.
[[nodiscard]] Audio::Pcm ExtractMono(const Audio::Pcm &pcm, int channel) {
	auto result = Audio::Pcm{ .channels = 1, .rate = pcm.rate };
	const auto channels = pcm.channels;
	const auto frames = pcm.frames();
	result.samples.resize(size_t(frames));
	const auto from = pcm.samples.data();
	const auto to = result.samples.data();
	if (channel >= 0) {
		for (auto i = int64(0); i != frames; ++i) {
			to[i] = from[i * channels + channel];
		}
	} else {
		for (auto i = int64(0); i != frames; ++i) {
			auto sum = 0.f;
			for (auto c = 0; c != channels; ++c) {
				sum += from[i * channels + c];
			}
			to[i] = sum / channels;
		}
	}
	return result;
}

//
// Self-test helpers.
//

constexpr auto kTestCycle = 1.2; // Seconds: a phrase and a pause.
constexpr auto kTestVoiced = 0.6; // The phrase part of the cycle.
constexpr auto kTestNoise = 0.02; // About -34 dBFS, as a loud room.

// A speech-like signal: phrases of a harmonic tone (the pitch wanders
// around 130 Hz, two formants, syllables at 4 Hz) with pauses between
// them. The noise is white, from a fixed generator.
struct TestSignal {
	Audio::Pcm clean;
	Audio::Pcm noisy;
};

[[nodiscard]] TestSignal MakeTestSignal(
		int rate,
		int channels,
		int seconds,
		double noise = kTestNoise) {
	constexpr auto kHarmonics = 20;
	constexpr auto kVolume = 0.12;

	auto result = TestSignal();
	const auto frames = int64(rate) * seconds;
	for (const auto pcm : { &result.clean, &result.noisy }) {
		pcm->channels = channels;
		pcm->rate = rate;
		pcm->samples.resize(size_t(frames * channels));
	}
	auto generator = uint32(0x0B11F10E);
	const auto random = [&] {
		// Xorshift, the sum of three uniform values is close to normal
		// with a unit deviation.
		auto sum = 0.;
		for (auto i = 0; i != 3; ++i) {
			generator ^= generator << 13;
			generator ^= generator >> 17;
			generator ^= generator << 5;
			sum += (double(generator) / 2147483648.) - 1.;
		}
		return sum;
	};
	auto phase = 0.;
	for (auto i = int64(0); i != frames; ++i) {
		const auto t = double(i) / rate;
		const auto inCycle = std::fmod(t, kTestCycle);
		const auto pitch = 130. + 25. * std::sin(2. * M_PI * 1.3 * t);
		phase += 2. * M_PI * pitch / rate;
		if (phase > 2. * M_PI) {
			phase -= 2. * M_PI;
		}
		auto value = 0.;
		if (inCycle < kTestVoiced) {
			const auto envelope = std::sin(M_PI * inCycle / kTestVoiced);
			const auto syllable = 0.6 + 0.4 * std::sin(2. * M_PI * 4. * t);
			for (auto h = 1; h <= kHarmonics; ++h) {
				const auto frequency = pitch * h;
				const auto first = (frequency - 700.) / 200.;
				const auto second = (frequency - 1200.) / 250.;
				const auto weight = (1.
					+ 2. * std::exp(-first * first)
					+ 1.5 * std::exp(-second * second)) / h;
				value += weight * std::sin(phase * h);
			}
			value *= kVolume * envelope * syllable;
		}
		for (auto c = 0; c != channels; ++c) {
			const auto index = size_t(i * channels + c);
			result.clean.samples[index] = float(value);
			result.noisy.samples[index] = float(value + noise * random());
		}
	}
	return result;
}

[[nodiscard]] double SegmentRms(
		const Audio::Pcm &pcm,
		double from,
		double till,
		int channel = 0) {
	const auto frames = pcm.frames();
	const auto first = std::clamp(int64(from * pcm.rate), int64(0), frames);
	const auto last = std::clamp(int64(till * pcm.rate), first, frames);
	if (last <= first || channel >= pcm.channels) {
		return 0.;
	}
	auto sum = 0.;
	for (auto i = first; i != last; ++i) {
		const auto value = double(pcm.samples[i * pcm.channels + channel]);
		sum += value * value;
	}
	return std::sqrt(sum / (last - first));
}

// Average RMS over the middle parts of the phrases (voiced) or of the
// pauses between them, skipping the first cycle while RNNoise adapts.
[[nodiscard]] double CyclesRms(
		const Audio::Pcm &pcm,
		bool voiced,
		int channel = 0) {
	const auto cycles = int(pcm.seconds() / kTestCycle + 0.01);
	auto sum = 0.;
	auto count = 0;
	for (auto i = 1; i < cycles; ++i) {
		const auto start = i * kTestCycle;
		sum += voiced
			? SegmentRms(pcm, start + 0.1, start + 0.5, channel)
			: SegmentRms(pcm, start + 0.7, start + 1.1, channel);
		++count;
	}
	return count ? (sum / count) : 0.;
}

[[nodiscard]] bool AllFinite(const Audio::Pcm &pcm) {
	return ranges::all_of(pcm.samples, [](float value) {
		return std::isfinite(value);
	});
}

[[nodiscard]] double MaxDifference(
		const Audio::Pcm &a,
		const Audio::Pcm &b) {
	if (a.samples.size() != b.samples.size()) {
		return std::numeric_limits<double>::infinity();
	}
	auto result = 0.;
	for (auto i = size_t(0), count = a.samples.size(); i != count; ++i) {
		result = std::max(
			result,
			double(std::abs(a.samples[i] - b.samples[i])));
	}
	return result;
}

// Mono only: how well a (moved by shift frames) matches b.
[[nodiscard]] double Correlation(
		const Audio::Pcm &a,
		const Audio::Pcm &b,
		int shift) {
	const auto frames = std::min(a.frames(), b.frames());
	const auto margin = int64(std::abs(shift)) + 1;
	auto sum = 0.;
	for (auto i = margin; i < frames - margin; ++i) {
		sum += double(a.samples[i + shift]) * double(b.samples[i]);
	}
	return sum;
}

[[nodiscard]] QString Number(double value) {
	return QString::number(value, 'g', 4);
}

} // namespace

Audio::Pcm DenoiseVoice(const Audio::Pcm &pcm, float64 strength) {
	if (pcm.empty()) {
		return Audio::Pcm();
	}
	auto result = (pcm.channels == 1) ? pcm : ExtractMono(pcm, -1);
	if (result.rate != kRate) {
		result = Audio::Resample(result, kRate);
		if (result.empty() || result.rate != kRate || result.channels != 1) {
			return Audio::Pcm();
		}
	}
	if (!DenoiseMono(result.samples, MixFor(strength), nullptr)) {
		return Audio::Pcm();
	}
	return result;
}

bool DenoiseVoiceInPlace(Audio::Pcm &pcm, float64 strength) {
	return !pcm.empty()
		&& (pcm.rate == kRate)
		&& (pcm.channels == 1)
		&& DenoiseMono(pcm.samples, MixFor(strength), nullptr);
}

Audio::Pcm Denoise(
		const Audio::Pcm &pcm,
		float64 strength,
		const Fn<bool()> &cancelled) {
	if (pcm.empty()) {
		return Audio::Pcm();
	}
	const auto mix = MixFor(strength);
	const auto channels = pcm.channels;
	const auto frames = pcm.frames();
	auto result = Audio::Pcm{ .channels = channels, .rate = pcm.rate };
	result.samples.resize(size_t(frames * channels), 0.f);
	for (auto c = 0; c != channels; ++c) {
		auto mono = ExtractMono(pcm, c);
		if (pcm.rate != kRate) {
			mono = Audio::Resample(mono, kRate);
			if (mono.empty() || mono.rate != kRate || mono.channels != 1) {
				return Audio::Pcm();
			}
		}
		if ((cancelled && cancelled())
			|| !DenoiseMono(mono.samples, mix, cancelled)) {
			return Audio::Pcm();
		}
		if (pcm.rate != kRate) {
			mono = Audio::Resample(mono, pcm.rate);
			if (mono.empty() || mono.channels != 1) {
				return Audio::Pcm();
			}
		}
		const auto count = std::min(frames, mono.frames());
		const auto to = result.samples.data();
		for (auto i = int64(0); i != count; ++i) {
			to[i * channels + c] = mono.samples[i];
		}
	}
	return result;
}

QByteArray DenoiseVoice(const QByteArray &oggOpus, float64 strength) {
	if (oggOpus.isEmpty()) {
		return QByteArray();
	}
	auto decoded = Audio::Decode(oggOpus, { .rate = kRate, .channels = 1 });
	if (!decoded
		|| decoded->empty()
		|| (decoded->rate != kRate)
		|| (decoded->channels != 1)
		|| !DenoiseMono(decoded->samples, MixFor(strength), nullptr)) {
		return QByteArray();
	}
	return Audio::Encode(
		*decoded,
		Audio::Format::OggOpus,
		{ .voice = true });
}

bool RunSelfTest(QStringList &log) {
	auto passed = 0;
	auto failed = 0;
	const auto check = [&](
			bool condition,
			const QString &name,
			const QString &details = QString()) {
		log.push_back(u"noise: "_q
			+ (condition ? u"OK   "_q : u"FAIL "_q)
			+ name
			+ (details.isEmpty() ? QString() : (u" ("_q + details + ')')));
		++(condition ? passed : failed);
		return condition;
	};
	auto timer = QElapsedTimer();
	timer.start();

	try {
		check(
			rnnoise_get_frame_size() == kFrameSize,
			u"RNNoise frame size"_q,
			QString::number(rnnoise_get_frame_size()));
		check(CreateState() != nullptr, u"RNNoise state"_q);

		// The main case: a voice in a noisy room, 48 kHz mono.
		const auto signal = MakeTestSignal(kRate, 1, 8);
		const auto &noisy = signal.noisy;
		const auto noiseWas = CyclesRms(noisy, false);
		const auto voiceWas = CyclesRms(noisy, true);
		timer.restart();
		const auto clean = DenoiseVoice(noisy);
		const auto took = timer.elapsed();
		check(
			!clean.empty()
				&& (clean.rate == kRate)
				&& (clean.channels == 1)
				&& (clean.frames() == noisy.frames()),
			u"the format and the length are kept"_q,
			u"%1 frames, %2 ms"_q.arg(clean.frames()).arg(took));
		check(AllFinite(clean), u"the result is finite"_q);
		const auto noiseNow = CyclesRms(clean, false);
		const auto voiceNow = CyclesRms(clean, true);
		check(
			(noiseWas > 0.) && (noiseNow < noiseWas * 0.25),
			u"the noise in the pauses drops"_q,
			u"RMS %1 -> %2"_q.arg(Number(noiseWas), Number(noiseNow)));
		check(
			(voiceNow > voiceWas * 0.7) && (voiceNow < voiceWas * 1.1),
			u"the voice keeps its energy"_q,
			u"RMS %1 -> %2"_q.arg(Number(voiceWas), Number(voiceNow)));
		const auto aligned = Correlation(clean, signal.clean, 0);
		check(
			(aligned > 0.)
				&& (aligned > Correlation(clean, signal.clean, kFrameSize))
				&& (aligned > Correlation(clean, signal.clean, -kFrameSize)),
			u"the result is aligned with the source"_q);

		{
			auto copy = noisy;
			const auto done = DenoiseVoiceInPlace(copy);
			const auto difference = MaxDifference(copy, clean);
			check(
				done && (difference < 1e-6),
				u"in place gives the same result"_q,
				Number(difference));
		}
		{
			const auto same = DenoiseVoice(noisy, 0.);
			const auto difference = MaxDifference(same, noisy);
			check(
				difference < 1e-6,
				u"strength 0 keeps the source"_q,
				Number(difference));
			const auto half = DenoiseVoice(noisy, 0.5);
			const auto ratio = CyclesRms(half, false) / noiseWas;
			check(
				(half.frames() == noisy.frames())
					&& (ratio > 0.4)
					&& (ratio < 0.65),
				u"strength 0.5 halves the noise"_q,
				Number(ratio));
		}

		// Any length, also shorter than one frame.
		for (const auto frames : { 1, 479, 480, 481, 959, 1000, 48007 }) {
			auto part = Audio::Pcm{ .channels = 1, .rate = kRate };
			part.samples.assign(
				begin(noisy.samples),
				begin(noisy.samples) + frames);
			const auto result = DenoiseVoice(part);
			check(
				(result.frames() == frames) && AllFinite(result),
				u"%1 frames in, the same out"_q.arg(frames),
				QString::number(result.frames()));
		}

		// Edge cases.
		check(
			DenoiseVoice(Audio::Pcm()).empty()
				&& Denoise(Audio::Pcm()).empty()
				&& DenoiseVoice(QByteArray()).isEmpty(),
			u"empty in, empty out"_q);
		{
			const auto silence = Audio::Silence(1000, kRate, 1);
			const auto result = DenoiseVoice(silence);
			check(
				(result.frames() == silence.frames())
					&& (Audio::Peak(result) < 1e-4),
				u"silence stays silence"_q,
				Number(Audio::Peak(result)));
		}
		{
			// Far out of range and with broken samples.
			auto loud = noisy;
			for (auto &sample : loud.samples) {
				sample *= 40.f;
			}
			loud.samples[100] = std::numeric_limits<float>::infinity();
			loud.samples[5000] = std::numeric_limits<float>::quiet_NaN();
			const auto result = DenoiseVoice(loud);
			check(
				(result.frames() == loud.frames()) && AllFinite(result),
				u"too loud and broken samples give a finite result"_q,
				Number(Audio::Peak(result)));
		}

		// Other formats.
		const auto other = MakeTestSignal(44100, 2, 6);
		{
			auto wrong = other.noisy;
			const auto done = DenoiseVoiceInPlace(wrong);
			check(
				!done && (MaxDifference(wrong, other.noisy) == 0.),
				u"in place refuses 44.1 kHz stereo"_q);

			const auto result = DenoiseVoice(other.noisy);
			const auto expected = int64(std::llround(
				double(other.noisy.frames()) * kRate / 44100));
			const auto ratio = CyclesRms(result, false)
				/ CyclesRms(other.noisy, false);
			check(
				(result.rate == kRate)
					&& (result.channels == 1)
					&& (std::abs(result.frames() - expected) <= 1)
					&& AllFinite(result)
					&& (ratio < 0.25),
				u"44.1 kHz stereo becomes 48 kHz mono without the noise"_q,
				u"%1 frames, noise x%2"_q.arg(result.frames()).arg(
					Number(ratio)));
		}
		{
			timer.restart();
			const auto result = Denoise(other.noisy);
			const auto took = timer.elapsed();
			auto good = (result.rate == other.noisy.rate)
				&& (result.channels == other.noisy.channels)
				&& (result.frames() == other.noisy.frames())
				&& AllFinite(result);
			auto details = QStringList();
			for (auto c = 0; good && (c != result.channels); ++c) {
				const auto noise = CyclesRms(result, false, c)
					/ CyclesRms(other.noisy, false, c);
				const auto voice = CyclesRms(result, true, c)
					/ CyclesRms(other.noisy, true, c);
				good = (noise < 0.25) && (voice > 0.7) && (voice < 1.1);
				details.push_back(u"noise x%1, voice x%2"_q.arg(
					Number(noise),
					Number(voice)));
			}
			details.push_back(u"%1 ms"_q.arg(took));
			check(
				good,
				u"Denoise keeps the rate and both channels"_q,
				details.join(u"; "_q));

			auto asked = 0;
			const auto cancelled = Denoise(other.noisy, 1., [&] {
				return (++asked > 1);
			});
			check(
				cancelled.empty() && (asked == 2),
				u"Denoise stops when cancelled"_q,
				QString::number(asked));
		}

		// A recorded voice message, as it goes through the voice changer.
		{
			const auto recorded = Audio::Encode(
				noisy,
				Audio::Format::OggOpus,
				{ .voice = true });
			const auto before = Audio::Decode(
				recorded,
				{ .rate = kRate, .channels = 1 });
			timer.restart();
			const auto bytes = DenoiseVoice(recorded);
			const auto took = timer.elapsed();
			const auto after = Audio::Decode(
				bytes,
				{ .rate = kRate, .channels = 1 });
			if (check(
					!recorded.isEmpty() && before && !before->empty(),
					u"a test voice message is encoded"_q,
					u"%1 bytes"_q.arg(recorded.size()))
				&& check(
					!bytes.isEmpty() && after && !after->empty(),
					u"a voice message is denoised"_q,
					u"%1 bytes, %2 ms"_q.arg(bytes.size()).arg(took))) {
				const auto shift = std::abs(
					after->duration() - before->duration());
				check(
					shift <= 60,
					u"the voice message keeps its duration"_q,
					u"%1 -> %2 ms"_q.arg(before->duration()).arg(
						after->duration()));
				const auto noise = CyclesRms(*after, false)
					/ CyclesRms(*before, false);
				const auto voice = CyclesRms(*after, true)
					/ CyclesRms(*before, true);
				check(
					(noise < 0.35) && (voice > 0.6) && (voice < 1.2),
					u"the voice message loses the noise, not the voice"_q,
					u"noise x%1, voice x%2"_q.arg(
						Number(noise),
						Number(voice)));
			}
		}

		// The speed, only for the report.
		{
			const auto minute = MakeTestSignal(kRate, 1, 60);
			timer.restart();
			auto copy = minute.noisy;
			const auto done = DenoiseVoiceInPlace(copy);
			check(
				done && AllFinite(copy),
				u"a minute of voice"_q,
				u"%1 ms"_q.arg(timer.elapsed()));
		}
	} catch (const std::exception &e) {
		check(false, u"exception"_q, QString::fromUtf8(e.what()));
	}

	log.push_back(u"noise: %1 passed, %2 failed"_q.arg(passed).arg(failed));
	return !failed;
}

} // namespace Oblivion::Noise
