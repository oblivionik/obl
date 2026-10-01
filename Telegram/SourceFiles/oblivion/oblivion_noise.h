/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "oblivion/oblivion_audio.h"

// Noise suppression for voice messages with RNNoise (linked for tgcalls:
// rnnoise_create / rnnoise_process_frame), used before sending while
// Oblivion::Get().voiceNoiseSuppression() is on.
//
// librnnoise.a gets into the app through lib_tgcalls, but its include
// folder (Libraries/rnnoise/include) is not on the include path of the
// Telegram target: the few C functions are declared with extern "C" in
// the .cpp instead of including <rnnoise.h>.
//
// RNNoise is a recurrent network trained on speech: it keeps the voice
// and removes everything else (hum, hiss, keyboard, street), so music
// is damaged by it. It works on 10 ms frames of 48 kHz mono and delays
// the signal by one frame, the functions here compensate that: the
// result is aligned with the source sample to sample.
//
// Synchronous, no global state, safe to call from any thread: run it
// through crl::async and deliver the result with crl::on_main. About
// 100 times faster than real time on one core.
namespace Oblivion::Noise {

// RNNoise works on frames of 480 samples (10 ms) of 48 kHz mono.
inline constexpr auto kRate = 48000;
inline constexpr auto kFrameSize = 480;

// Any rate / channel count in, 48 kHz mono out. strength (0..1) mixes
// the denoised signal with the original one (1 = fully denoised).
// Empty Pcm on errors.
[[nodiscard]] Audio::Pcm DenoiseVoice(
	const Audio::Pcm &pcm,
	float64 strength = 1.);

// The same without a copy, for a Pcm that already is 48 kHz mono (the
// way the voice changer decodes a recording). The frame count is kept.
// False (and the Pcm is left as it was) for another format or on errors.
[[nodiscard]] bool DenoiseVoiceInPlace(
	Audio::Pcm &pcm,
	float64 strength = 1.);

// Keeps the rate, the channel count and the frame count: every channel
// is denoised on its own (through 48 kHz and back), for the music
// editor. cancelled is checked about once per second of audio, a true
// result stops the work. Empty Pcm on errors and when cancelled.
[[nodiscard]] Audio::Pcm Denoise(
	const Audio::Pcm &pcm,
	float64 strength = 1.,
	const Fn<bool()> &cancelled = nullptr);

// A recorded voice message (OGG Opus, as Media::Capture produces it) in,
// the same format out (Audio::Encode with voice = true), the duration
// is kept. Empty on errors.
[[nodiscard]] QByteArray DenoiseVoice(
	const QByteArray &oggOpus,
	float64 strength = 1.);

// Self-checks for OBLIVION_SELFTEST=noise, see oblivion_selftest.h.
// Runs before Core::Application exists (no Core::App(), no session).
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::Noise
