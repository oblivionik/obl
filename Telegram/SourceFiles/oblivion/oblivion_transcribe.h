/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace Oblivion::Transcribe {

// Api::Transcribes::Entry::requestId while an on-device transcription
// is running (a real MTP request id is always positive).
inline constexpr auto kLocalRequestId = -1;

// Sample rate of the PCM passed to the system recognizer (mono, int16).
inline constexpr auto kSampleRate = 16000;

// Voice and round video messages are transcribed on this device instead
// of messages.transcribeAudio: the setting is on, the account has no
// real Premium (fake Premium doesn't count) and the platform can do it.
[[nodiscard]] bool Active(not_null<Main::Session*> session);

// Starts on-device transcription of the item's voice / round video.
// Api::Transcribes has already created the entry with kLocalRequestId,
// all the results come back through Api::Transcribes::applyLocal().
void Start(not_null<HistoryItem*> item);

// Platform part, oblivion_transcribe_mac.mm on macOS,
// a stub in oblivion_transcribe.cpp on other systems.

enum class Status {
	Partial,
	Done,
	Failed,
	Denied,
	Unavailable,
};

struct Update {
	Status status = Status::Failed;
	QString text; // Empty with Status::Done if there was no speech.
	bool incomplete = false; // Status::Done, but some audio was skipped.
};

struct Request {
	std::vector<int16> samples; // kSampleRate, mono.
	QString locale; // "ru-RU", "ru" or empty.
	bool systemFallback = false; // Use system language if unsupported.
	bool partial = true; // Report Status::Partial updates.
};

[[nodiscard]] bool SystemRecognizerSupported();

// Must be called on the main thread, callback is called on the main
// thread too: any number of Status::Partial updates and then exactly one
// final update (possibly right inside this call). The returned function
// cancels the recognition, no callbacks are called after that.
[[nodiscard]] Fn<void()> SystemRecognize(
	Request &&request,
	Fn<void(Update)> callback);

void OpenSystemPrivacySettings();

} // namespace Oblivion::Transcribe
