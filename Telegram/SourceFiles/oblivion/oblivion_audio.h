/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <optional>
#include <vector>

// Audio toolkit used by the voice changer, round video and music editor.
//
// Everything except PreviewPlayer is synchronous, keeps no global state
// and is safe to call from any thread (callers run heavy work through
// crl::async). Functions returning Pcm return an empty Pcm on error
// (including an empty or unusable input: no samples, rate outside
// 4..384 kHz or more than 8 channels).
//
// Processing functions keep the rate and the channel count of the input.
// Samples may leave [-1, 1] after Gain / ChangeSpeed / ShiftPitch, the
// effects (Reverb, Echo, Robot, Telephone) scale their result down to
// a 0.99 peak when needed, and Encode clamps everything to [-1, 1].
namespace Oblivion::Audio {

struct Pcm {
	std::vector<float> samples; // Interleaved, nominal range [-1, 1].
	int channels = 0;
	int rate = 0; // Samples per second per channel.

	[[nodiscard]] bool empty() const {
		return samples.empty() || (channels <= 0) || (rate <= 0);
	}
	[[nodiscard]] int64 frames() const {
		return (channels > 0) ? (int64(samples.size()) / channels) : 0;
	}
	[[nodiscard]] crl::time duration() const {
		return (rate > 0) ? (frames() * 1000 / rate) : 0;
	}
	[[nodiscard]] double seconds() const {
		return (rate > 0) ? (double(frames()) / rate) : 0.;
	}
};

struct DecodeOptions {
	int rate = 0; // 0 = keep the source rate.
	int channels = 0; // 0 = keep (more than 2 are mixed to stereo).

	// 0 = default limit. The result never exceeds 2^28 samples (about
	// 46 minutes of 48 kHz stereo), longer inputs are cut at the limit.
	crl::time maxDuration = 0;
};

// Decodes the first (best) audio stream of any supported container /
// codec, including the audio track of a video file (mp4 / mov / mkv).
// std::nullopt on error or if there is no audio.
[[nodiscard]] std::optional<Pcm> Decode(
	const QString &path,
	DecodeOptions options = {});
[[nodiscard]] std::optional<Pcm> Decode(
	const QByteArray &bytes,
	DecodeOptions options = {});

// Reads only the headers, fast.
struct MediaInfo {
	crl::time duration = 0; // 0 if unknown.
	int rate = 0;
	int channels = 0;
	bool hasVideo = false;
};
[[nodiscard]] std::optional<MediaInfo> Probe(const QString &path);

enum class Format : uchar {
	OggOpus, // Opus in Ogg, 48 kHz, mono or stereo.
	M4a, // AAC-LC in an MP4 (.m4a) container, 44.1 or 48 kHz.
	Wav, // PCM s16le, source rate, mono or stereo.
};

struct EncodeOptions {
	// Bits per second, 0 = default for the format: OggOpus 64 / 128 kbps
	// (mono / stereo), 32 kbps for voice; M4a 128 / 192 kbps.
	int bitrate = 0;

	// Telegram voice message: OggOpus only, mono 48 kHz 32 kbps, exactly
	// like Media::Capture records it. Send it with MakeVoiceWaveform().
	bool voice = false;

	// Tags written into the file, empty = no tag: iTunes atoms (©nam,
	// ©ART) in M4a, Vorbis comments (title, artist) in OggOpus and
	// a RIFF INFO list (INAM, IART) in Wav. FFmpeg reads them back as
	// the "title" / "artist" metadata.
	QString title;
	QString performer;
};

// Resamples / remixes as the format requires. Empty on error.
[[nodiscard]] QByteArray Encode(
	const Pcm &pcm,
	Format format,
	EncodeOptions options = {});

[[nodiscard]] QString FormatExtension(Format format); // "ogg", "m4a", "wav"
[[nodiscard]] QString FormatMimeType(Format format); // "audio/ogg", ...

// channels == 0 keeps the channel count (1 <-> 2 remix otherwise:
// stereo -> mono averages, mono -> stereo duplicates).
[[nodiscard]] Pcm Resample(const Pcm &pcm, int rate, int channels = 0);

// Vinyl-style: speed and pitch change together (factor 2. = twice
// as fast and an octave higher, 0.8 = "slowed"). Factor 0.25..4.
// The result has exactly round(frames / factor) frames.
[[nodiscard]] Pcm ChangeSpeed(const Pcm &pcm, double factor);

// Tempo only, pitch kept (factor 2. = twice as fast). Factor 0.25..4,
// FFmpeg atempo (WSOLA), chained for factors outside 0.5..2.
// The result has exactly round(frames / factor) frames.
[[nodiscard]] Pcm ChangeTempo(const Pcm &pcm, double factor);

// Pitch only, duration kept exactly (12. = one octave up). -24..24.
[[nodiscard]] Pcm ShiftPitch(const Pcm &pcm, double semitones);

// Freeverb (8 damped combs + 4 allpasses per channel, stereo spread,
// lows below 120 Hz kept out of the tail). The tail is appended to the
// end until it decays by 70 dB, but at most 8 seconds (faded out then).
// RT60 is about 1.4 s with the defaults.
// Slowed + reverb: ChangeSpeed(pcm, 0.85) and then
// Reverb(pcm, { .roomSize = 0.85, .damping = 0.4, .wet = 0.45 }).
struct ReverbParams {
	double roomSize = 0.6; // 0..1
	double damping = 0.5; // 0..1
	double wet = 0.35; // 0..1
	double dry = 0.8; // 0..1
};
[[nodiscard]] Pcm Reverb(const Pcm &pcm, const ReverbParams &params = {});

// Feedback delay, repeats go through a 5 kHz low-pass (tape-like).
// The first echo comes after delay with mix gain. The tail is appended
// (at most 10 seconds, faded out then).
struct EchoParams {
	crl::time delay = 300; // Milliseconds, 20..2000.
	double feedback = 0.4; // 0..0.95
	double mix = 0.5; // 0..1
};
[[nodiscard]] Pcm Echo(const Pcm &pcm, const EchoParams &params = {});

// Robotization (STFT with zeroed phases, a monotone ~100 Hz buzz)
// with the loudness of the source.
[[nodiscard]] Pcm Robot(const Pcm &pcm);

// Band-pass 300..3400 Hz (4th order), soft clipping of the loud parts
// and 8-bit mu-law quantization, quiet parts keep their level (peaks
// end up about 3 dB lower). The result is mono content in every
// channel of the source.
[[nodiscard]] Pcm Telephone(const Pcm &pcm);

// Milliseconds, clamped to [0, duration()]; till <= from gives empty.
// Cut points inside the source get a 3 ms fade against clicks.
[[nodiscard]] Pcm Trim(const Pcm &pcm, crl::time from, crl::time till);

// Joins in order, converting everything to the rate and channel count
// of the first non-empty item, with an equal-power crossfade between
// neighbours (0 = plain concatenation). Empty items are skipped.
// Result frames = sum of frames - crossfade frames for every join
// (the crossfade is shortened to the shorter neighbour if needed).
[[nodiscard]] Pcm Concat(
	const std::vector<Pcm> &list,
	crl::time crossfade = 0);

// The same for items owned elsewhere (nothing is copied, the result is
// allocated once), converting everything to the given format.
struct ConcatOptions {
	crl::time crossfade = 0;
	int rate = 0; // 0 = the rate of the first non-empty item.
	int channels = 0; // 0 = the channel count of the first non-empty item.
};
[[nodiscard]] Pcm Concat(
	const std::vector<not_null<const Pcm*>> &list,
	ConcatOptions options);

// Scales so that the absolute peak equals peak (0..1]. Silence is
// returned as is. Non-finite samples are replaced with zeros.
[[nodiscard]] Pcm Normalize(const Pcm &pcm, double peak = 0.95);

// Scales towards the RMS level rmsDb (dBFS, e.g. -16.), but never
// above the peakLimit absolute peak, so it never clips.
[[nodiscard]] Pcm NormalizeLoudness(
	const Pcm &pcm,
	double rmsDb = -16.,
	double peakLimit = 0.95);

// Plain gain in decibels, may leave [-1, 1] (use Normalize after).
[[nodiscard]] Pcm Gain(const Pcm &pcm, double decibels);

// Raised-cosine fades at the start / end, milliseconds (0 = none).
[[nodiscard]] Pcm Fade(const Pcm &pcm, crl::time fadeIn, crl::time fadeOut);

[[nodiscard]] Pcm Reverse(const Pcm &pcm);
[[nodiscard]] Pcm Silence(crl::time duration, int rate, int channels);

[[nodiscard]] double Peak(const Pcm &pcm); // Absolute peak, 0..
[[nodiscard]] double Rms(const Pcm &pcm); // Linear RMS, 0..

// Telegram voice waveform, the same format as Media::Capture produces
// (same type as ::VoiceWaveform: 100 values in [0, 31]). Empty if the
// Pcm is shorter than 100 frames.
[[nodiscard]] QVector<signed char> MakeVoiceWaveform(const Pcm &pcm);

// Plays a Pcm through its own OpenAL device (the one chosen in
// Telegram's settings), independent of the app mixer, so it never
// disturbs Telegram's own playback state. play() pauses the music
// and voice messages being played by Telegram's media player.
// The device is released a few seconds after pause / stop / end.
// Main thread only, needs Core::App() (not usable in the self-test).
// Safe to destroy in any state.
class PreviewPlayer final {
public:
	enum class State : uchar {
		Stopped,
		Playing,
		Paused,
	};

	PreviewPlayer();
	~PreviewPlayer();

	void setPcm(Pcm pcm); // Stops the playback, position becomes 0.
	void setPcm(std::shared_ptr<const Pcm> pcm); // Shared, not copied.
	void play(); // From the current position (from 0 after the end).
	void pause();
	void stop(); // Position becomes 0.
	void seek(crl::time position); // Keeps playing if it was playing.
	void setVolume(float64 volume); // 0..1, 1 by default.

	[[nodiscard]] crl::time position() const;
	[[nodiscard]] crl::time duration() const;
	[[nodiscard]] State state() const;

	// The position is updated about 25 times per second while playing.
	// At the end the state becomes Stopped and the position 0.
	[[nodiscard]] rpl::producer<State> stateValue() const;
	[[nodiscard]] rpl::producer<crl::time> positionValue() const;
	[[nodiscard]] rpl::producer<crl::time> durationValue() const;

private:
	struct Private;
	const std::unique_ptr<Private> _private;

};

// Self-checks for OBLIVION_SELFTEST, appends human-readable lines to log.
// Runs on the main thread before Core::Application exists: QApplication
// and crl are ready, but there is no Core::App(), no audio mixer,
// no session and no event loop running.
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::Audio
