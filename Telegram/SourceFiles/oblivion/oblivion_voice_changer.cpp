/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_voice_changer.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/event_filter.h"
#include "base/unique_qptr.h"
#include "base/weak_qptr.h"
#include "core/application.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "media/audio/media_audio_capture.h"
#include "media/audio/media_audio_capture_common.h"
#include "menu/menu_checked_action.h"
#include "oblivion/oblivion_audio.h"
#include "oblivion/oblivion_interface.h"
#include "oblivion/oblivion_noise.h"
#include "oblivion/oblivion_settings.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "storage/file_upload.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/text/format_values.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <crl/crl_async.h>
#include <crl/crl_queue.h>

#include <QtGui/QCursor>
#include <QtGui/QMouseEvent>

#include <deque>

namespace Oblivion {
namespace {

// Processing core: pure audio work, safe on any thread.

enum class EffectType : uchar {
	None,
	Chipmunk,
	Giant,
	Robot,
	Echo,
	Telephone,
	Cave,
	Pitch,
};

struct Effect {
	EffectType type = EffectType::None;
	int semitones = 0; // EffectType::Pitch only.

	[[nodiscard]] bool active() const {
		return (type != EffectType::None)
			&& ((type != EffectType::Pitch) || (semitones != 0));
	}

	friend inline bool operator==(const Effect &, const Effect &) = default;
};

// What is done with an outgoing voice message: the noise suppression
// (oblivion_noise.h) goes first, so the effect gets a clean voice.
struct Processing {
	Effect effect;
	bool denoise = false;

	[[nodiscard]] bool active() const {
		return denoise || effect.active();
	}
};

constexpr auto kRate = 48000; // Opus voice messages are 48 kHz mono.
constexpr auto kPitchMin = -12;
constexpr auto kPitchMax = 12;
constexpr auto kChipmunkSemitones = 7.;
constexpr auto kGiantSemitones = -6.;

// Effects with a tail (echo, reverb) may make a voice message that is
// sent right away longer by this much at most.
constexpr auto kMaxTail = crl::time(1500);
constexpr auto kTailFade = crl::time(300); // When the tail is cut short.
constexpr auto kEndFade = crl::time(20); // When the tail has faded out.
constexpr auto kCutFade = crl::time(60); // When the length is kept.
constexpr auto kTailSilence = 0.002; // About -54 dBFS.
constexpr auto kPeakLimit = 0.97;

// Longer voice messages are sent without the effect: processing keeps
// a few copies of the whole clip in memory (about 170 MB each for
// 15 minutes) and takes about half a minute for such a clip.
constexpr auto kMaxClip = crl::time(15 * 60 * 1000);
// The decoded length may differ from the recorded one a little: up to
// kMaxClip + kMaxClipSlack is processed, decoding stops a bit later.
constexpr auto kMaxClipSlack = crl::time(5 * 1000);

struct VoiceResult {
	QByteArray bytes;
	QVector<signed char> waveform;
	crl::time duration = 0;
};

[[nodiscard]] Audio::Pcm ApplyEffect(
		const Audio::Pcm &pcm,
		const Effect &effect) {
	switch (effect.type) {
	case EffectType::None: return pcm;
	case EffectType::Chipmunk:
		return Audio::ShiftPitch(pcm, kChipmunkSemitones);
	case EffectType::Giant: return Audio::ShiftPitch(pcm, kGiantSemitones);
	case EffectType::Robot: return Audio::Robot(pcm);
	case EffectType::Echo:
		return Audio::Echo(pcm, {
			.delay = 260,
			.feedback = 0.42,
			.mix = 0.5,
		});
	case EffectType::Telephone: return Audio::Telephone(pcm);
	case EffectType::Cave: {
		const auto early = Audio::Echo(pcm, {
			.delay = 110,
			.feedback = 0.3,
			.mix = 0.3,
		});
		return early.empty()
			? Audio::Pcm()
			: Audio::Reverb(early, {
				.roomSize = 0.92,
				.damping = 0.3,
				.wet = 0.55,
				.dry = 0.75,
			});
	}
	case EffectType::Pitch:
		return Audio::ShiftPitch(
			pcm,
			double(std::clamp(effect.semitones, kPitchMin, kPitchMax)));
	}
	return Audio::Pcm();
}

void FadeOutEnd(Audio::Pcm &pcm, int64 frames) {
	const auto total = pcm.frames();
	frames = std::min(frames, total);
	if (frames <= 0) {
		return;
	}
	const auto channels = pcm.channels;
	const auto first = total - frames;
	for (auto i = int64(0); i != frames; ++i) {
		// Raised cosine from 1 to 0.
		const auto t = double(i + 1) / double(frames);
		const auto k = float(0.5 * (1. + std::cos(M_PI * t)));
		auto sample = pcm.samples.data() + (first + i) * channels;
		for (auto c = 0; c != channels; ++c) {
			sample[c] *= k;
		}
	}
}

[[nodiscard]] int64 FramesFor(const Audio::Pcm &pcm, crl::time duration) {
	return int64(pcm.rate) * duration / 1000;
}

// The effects keep the time line of the source and may append a tail.
// Without allowTail the result has exactly sourceFrames frames, else
// the audible part of the tail is kept, at most kMaxTail.
[[nodiscard]] Audio::Pcm FitLength(
		Audio::Pcm pcm,
		int64 sourceFrames,
		bool allowTail) {
	const auto channels = pcm.channels;
	const auto frames = pcm.frames();
	if (frames <= sourceFrames) {
		pcm.samples.resize(size_t(sourceFrames * channels), 0.f);
		return pcm;
	}
	auto keep = sourceFrames;
	auto fade = std::min(sourceFrames, FramesFor(pcm, kCutFade));
	if (allowTail) {
		const auto limit = std::min(
			frames,
			sourceFrames + FramesFor(pcm, kMaxTail));
		auto audible = sourceFrames;
		for (auto i = limit; i > sourceFrames; --i) {
			const auto sample = pcm.samples.data() + (i - 1) * channels;
			auto loud = false;
			for (auto c = 0; c != channels; ++c) {
				if (std::abs(sample[c]) > kTailSilence) {
					loud = true;
					break;
				}
			}
			if (loud) {
				audible = i;
				break;
			}
		}
		keep = std::min(limit, audible + FramesFor(pcm, kEndFade));
		fade = (keep == limit && limit < frames)
			? std::min(keep - sourceFrames, FramesFor(pcm, kTailFade))
			: FramesFor(pcm, kEndFade);
	}
	pcm.samples.resize(size_t(keep * channels));
	FadeOutEnd(pcm, fade);
	return pcm;
}

// As Audio::Normalize(pcm, kPeakLimit) for louder ones, but in place,
// without one more copy of the whole clip.
void LimitPeak(Audio::Pcm &pcm) {
	const auto peak = Audio::Peak(pcm);
	if (peak <= kPeakLimit) {
		return;
	}
	const auto factor = float(kPeakLimit / peak);
	for (auto &value : pcm.samples) {
		value = std::isfinite(value) ? (value * factor) : 0.f;
	}
}

// Decodes a recorded voice message, suppresses the noise and applies
// the effect (whatever of the two is on) to everything after keepPrefix
// (ms, that part already has it) and encodes the result the same way
// Media::Capture does. std::nullopt on any error and for clips longer
// than kMaxClip. The whole clip is in memory a few times over, so every
// copy that is not needed any more is released at once.
[[nodiscard]] std::optional<VoiceResult> ProcessVoice(
		const QByteArray &bytes,
		const QVector<signed char> &waveform,
		crl::time duration,
		const Processing &processing,
		crl::time keepPrefix,
		bool allowTail) {
	const auto &effect = processing.effect;
	auto decoded = Audio::Decode(bytes, {
		.rate = kRate,
		.channels = 1,
		.maxDuration = kMaxClip + 2 * kMaxClipSlack,
	});
	if (!decoded
		|| decoded->empty()
		|| decoded->channels != 1
		|| decoded->duration() > kMaxClip + kMaxClipSlack) {
		return std::nullopt;
	}
	auto source = std::move(*decoded);
	decoded.reset();
	source.samples.shrink_to_fit(); // Decoding grows it up to twice.
	const auto rate = source.rate;
	const auto total = source.frames();
	const auto prefix = std::clamp(
		FramesFor(source, std::max(keepPrefix, crl::time(0))),
		int64(0),
		total);

	auto result = Audio::Pcm();
	if (prefix < total) {
		auto part = Audio::Pcm{ .channels = 1, .rate = rate };
		if (prefix > 0) {
			part.samples.assign(
				begin(source.samples) + prefix,
				end(source.samples));
			source.samples.resize(size_t(prefix));
			source.samples.shrink_to_fit();
		} else {
			part = std::move(source);
		}
		const auto frames = part.frames();
		if (processing.denoise && !Noise::DenoiseVoiceInPlace(part)) {
			return std::nullopt;
		}
		auto processed = effect.active()
			? ApplyEffect(part, effect)
			: std::move(part);
		part = Audio::Pcm();
		if (processed.empty()
			|| processed.channels != 1
			|| processed.rate != rate) {
			return std::nullopt;
		}
		processed = FitLength(std::move(processed), frames, allowTail);
		if (prefix > 0) {
			result = std::move(source);
			result.samples.insert(
				end(result.samples),
				begin(processed.samples),
				end(processed.samples));
		} else {
			result = std::move(processed);
		}
	} else {
		result = std::move(source);
	}
	LimitPeak(result);

	auto encoded = Audio::Encode(
		result,
		Audio::Format::OggOpus,
		{ .voice = true });
	if (encoded.isEmpty()) {
		return std::nullopt;
	}
	auto computed = Audio::MakeVoiceWaveform(result);
	const auto extra = std::max(result.frames() - total, int64(0));
	return VoiceResult{
		.bytes = std::move(encoded),
		.waveform = computed.isEmpty() ? waveform : std::move(computed),
		.duration = ((duration > 0)
			? (duration + extra * 1000 / rate)
			: result.duration()),
	};
}

// For the test recording in the box: no encoding, the tail is kept.
[[nodiscard]] Audio::Pcm PreviewVoice(
		const Audio::Pcm &source,
		const Processing &processing) {
	const auto &effect = processing.effect;
	if (!processing.active()) {
		return source;
	}
	auto clean = Audio::Pcm();
	if (processing.denoise) {
		clean = Noise::DenoiseVoice(source);
		if (clean.empty() || !effect.active()) {
			return clean;
		}
	}
	auto processed = ApplyEffect(
		processing.denoise ? clean : source,
		effect);
	clean = Audio::Pcm();
	if (processed.empty()) {
		return Audio::Pcm();
	}
	auto result = FitLength(std::move(processed), source.frames(), true);
	LimitPeak(result);
	return result;
}

// End of the processing core.

constexpr auto kDefaultCustomPitch = 4;
// Processing takes about 1 second per 30 seconds of voice (mostly the
// Opus encoding), longer clips get a toast so the delay is explained.
constexpr auto kLongClip = crl::time(30 * 1000);
constexpr auto kSampleMaxDuration = crl::time(15 * 1000);
constexpr auto kProcessedAll = crl::time(-1);
constexpr auto kRegistryLimit = size_t(64);

constexpr auto kPresets = std::array{
	EffectType::None,
	EffectType::Chipmunk,
	EffectType::Giant,
	EffectType::Robot,
	EffectType::Echo,
	EffectType::Telephone,
	EffectType::Cave,
};

int LastCustomPitch = kDefaultCustomPitch;

// Set while the record bar handles a recording that already got
// the effect from WrapVoicePreviewCapture (main thread only).
bool ProcessedCaptureActive = false;

// Voice messages taken by InterceptVoiceSend() and not sent yet
// (main thread only). Later ones wait for them to keep the order.
int PendingVoiceSends = 0;

// Quit and log out wait for those voice messages: for the processing,
// then for the upload to start (ApiWrap prepares the file on its own
// thread first, nothing is uploading yet) and to end, then for the
// message itself: it is sent only after its upload, the server has
// to answer. An upload that takes longer gets the usual question
// about stopping it instead.
constexpr auto kUploadCheckStep = crl::time(25);
constexpr auto kUploadStartWait = crl::time(2000);
constexpr auto kUploadWait = crl::time(5000);
constexpr auto kSendWait = crl::time(3000);
constexpr auto kQuitAskAgain = crl::time(1500);

// The toast about the postponed quit tells how to cancel it,
// the usual time is too short to read that.
constexpr auto kQuitToastDuration = crl::time(5000);

struct UploadedMessage {
	base::weak_ptr<Main::Session> session;
	FullMsgId id;
};

struct DelayedLogout {
	base::weak_ptr<Main::Account> account;
	Fn<void()> retry;
};

struct VoiceSendsWait {
	bool quit = false; // Core::Quit() when they are sent.
	bool quitAtOnce = false; // Chosen in the box: quit without them.

	// All are processed, the upload is awaited. True only while
	// an upload check of the current generation is scheduled.
	bool handover = false;
	int generation = 0; // Of the upload checks, a new wait stops the old.
	crl::time quitAsked = 0;

	// The log outs to repeat when they are sent, one for an account.
	std::vector<DelayedLogout> logouts;
	base::weak_qptr<Ui::BoxContent> quitBox;

	// The uploads seen by the checks, until their messages are sent.
	std::vector<UploadedMessage> uploads;
	crl::time uploadSeen = 0;
};

// Main thread only. Never destroyed, so nothing of it runs after the
// application is gone.
[[nodiscard]] VoiceSendsWait &SendsWait() {
	static const auto result = new VoiceSendsWait();
	return *result;
}

struct ProcessedMark {
	size_t hash = 0;
	qsizetype size = 0;
	crl::time prefix = kProcessedAll; // Or the processed prefix, ms.
};

[[nodiscard]] std::deque<ProcessedMark> &Registry() {
	static auto result = std::deque<ProcessedMark>();
	return result;
}

[[nodiscard]] size_t ContentHash(const QByteArray &bytes) {
	return qHash(bytes, size_t(0x0B11F10Eu));
}

void MarkProcessed(const QByteArray &bytes, crl::time prefix) {
	if (bytes.isEmpty() || (prefix == 0)) {
		return;
	}
	auto &list = Registry();
	const auto hash = ContentHash(bytes);
	const auto size = bytes.size();
	for (auto i = begin(list); i != end(list); ++i) {
		if (i->hash == hash && i->size == size) {
			list.erase(i);
			break;
		}
	}
	list.push_back({ .hash = hash, .size = size, .prefix = prefix });
	while (list.size() > kRegistryLimit) {
		list.pop_front();
	}
}

// std::nullopt - unknown (no effect yet), kProcessedAll - done,
// otherwise the duration of the start part that has the effect.
[[nodiscard]] std::optional<crl::time> ProcessedPrefix(
		const QByteArray &bytes) {
	if (bytes.isEmpty()) {
		return std::nullopt;
	}
	const auto hash = ContentHash(bytes);
	const auto size = bytes.size();
	for (const auto &mark : Registry()) {
		if (mark.hash == hash && mark.size == size) {
			return mark.prefix;
		}
	}
	return std::nullopt;
}

[[nodiscard]] Effect ParseEffect(const QString &id) {
	const auto simple = [](EffectType type) {
		return Effect{ .type = type };
	};
	if (id == u"chipmunk"_q) {
		return simple(EffectType::Chipmunk);
	} else if (id == u"giant"_q) {
		return simple(EffectType::Giant);
	} else if (id == u"robot"_q) {
		return simple(EffectType::Robot);
	} else if (id == u"echo"_q) {
		return simple(EffectType::Echo);
	} else if (id == u"telephone"_q) {
		return simple(EffectType::Telephone);
	} else if (id == u"cave"_q) {
		return simple(EffectType::Cave);
	} else if (id.startsWith(u"pitch:"_q)) {
		auto ok = false;
		const auto value = id.mid(6).toInt(&ok);
		if (ok) {
			return Effect{
				.type = EffectType::Pitch,
				.semitones = std::clamp(value, kPitchMin, kPitchMax),
			};
		}
	}
	return Effect();
}

[[nodiscard]] QString EffectId(const Effect &effect) {
	if (!effect.active()) {
		return QString();
	}
	switch (effect.type) {
	case EffectType::None: return QString();
	case EffectType::Chipmunk: return u"chipmunk"_q;
	case EffectType::Giant: return u"giant"_q;
	case EffectType::Robot: return u"robot"_q;
	case EffectType::Echo: return u"echo"_q;
	case EffectType::Telephone: return u"telephone"_q;
	case EffectType::Cave: return u"cave"_q;
	case EffectType::Pitch:
		return u"pitch:"_q + QString::number(effect.semitones);
	}
	return QString();
}

[[nodiscard]] Effect CurrentEffect() {
	return ParseEffect(Get().voiceEffect());
}

[[nodiscard]] Processing CurrentProcessing() {
	return {
		.effect = CurrentEffect(),
		.denoise = Get().voiceNoiseSuppression(),
	};
}

// Toasts name the effect while one is chosen and the noise suppression
// when it works alone.
[[nodiscard]] QString ApplyingText(const Processing &processing) {
	return processing.effect.active()
		? tr::lng_oblivion_voice_effect_applying(tr::now)
		: tr::lng_oblivion_voice_noise_applying(tr::now);
}

[[nodiscard]] QString FailedText(const Processing &processing) {
	return processing.effect.active()
		? tr::lng_oblivion_voice_effect_failed(tr::now)
		: tr::lng_oblivion_voice_noise_failed(tr::now);
}

[[nodiscard]] QString PreviewFailedText(const Processing &processing) {
	return processing.effect.active()
		? tr::lng_oblivion_voice_effect_preview_failed(tr::now)
		: tr::lng_oblivion_voice_noise_preview_failed(tr::now);
}

[[nodiscard]] QString TooLongText(const Processing &processing) {
	return processing.effect.active()
		? tr::lng_oblivion_voice_effect_too_long(tr::now)
		: tr::lng_oblivion_voice_noise_too_long(tr::now);
}

[[nodiscard]] QString FormatSemitones(int value) {
	return (value > 0)
		? (u"+"_q + QString::number(value))
		: (value < 0)
		? (QString(QChar(0x2212)) + QString::number(-value))
		: u"0"_q;
}

[[nodiscard]] QString EffectName(const Effect &effect) {
	if (!effect.active()) {
		return tr::lng_oblivion_voice_effect_off(tr::now);
	}
	switch (effect.type) {
	case EffectType::None:
		return tr::lng_oblivion_voice_effect_off(tr::now);
	case EffectType::Chipmunk:
		return tr::lng_oblivion_voice_effect_chipmunk(tr::now);
	case EffectType::Giant:
		return tr::lng_oblivion_voice_effect_giant(tr::now);
	case EffectType::Robot:
		return tr::lng_oblivion_voice_effect_robot(tr::now);
	case EffectType::Echo:
		return tr::lng_oblivion_voice_effect_echo(tr::now);
	case EffectType::Telephone:
		return tr::lng_oblivion_voice_effect_telephone(tr::now);
	case EffectType::Cave:
		return tr::lng_oblivion_voice_effect_cave(tr::now);
	case EffectType::Pitch:
		return tr::lng_oblivion_voice_effect_custom_value(
			tr::now,
			lt_value,
			FormatSemitones(effect.semitones));
	}
	return QString();
}

[[nodiscard]] const style::icon *EffectIcon(EffectType type) {
	switch (type) {
	case EffectType::None: return &st::menuIconCancel;
	case EffectType::Chipmunk: return &st::menuIconAbove;
	case EffectType::Giant: return &st::menuIconBelow;
	case EffectType::Robot: return &st::menuIconBot;
	case EffectType::Echo: return &st::menuIconSoundOn;
	case EffectType::Telephone: return &st::menuIconPhone;
	case EffectType::Cave: return &st::menuIconNightMode;
	case EffectType::Pitch: return &st::menuIconCustomize;
	}
	return nullptr;
}

// Slider positions skip 0 semitones (that would be no effect).
constexpr auto kPitchPositions = (kPitchMax - kPitchMin);

[[nodiscard]] int PitchFromPosition(int index) {
	const auto value = kPitchMin + index;
	return (value >= 0) ? (value + 1) : value;
}

// Only in a window that exists: the session may have none after
// an account switch and resolving one would switch the account back.
void ShowToast(
		not_null<Main::Session*> session,
		PeerData *peer,
		const QString &text) {
	if (const auto window = ExistingWindow(session, peer)) {
		window->showToast(text);
	} else if (const auto window = Core::App().activePrimaryWindow()) {
		window->showToast(text);
	}
}

// Remembers the uploads that run now, true if there are some.
[[nodiscard]] bool RememberUploads(std::vector<UploadedMessage> &list) {
	const auto &domain = Core::App().domain();
	if (!domain.started()) {
		return false;
	}
	auto result = false;
	for (const auto &[index, account] : domain.accounts()) {
		if (!account->sessionExists()) {
			continue;
		}
		const auto session = &account->session();
		const auto id = session->uploader().currentUploadId();
		if (!id) {
			continue;
		}
		result = true;
		const auto known = [&](const UploadedMessage &entry) {
			return (entry.session.get() == session) && (entry.id == id);
		};
		if (!ranges::any_of(list, known)) {
			list.push_back({ base::make_weak(session), id });
		}
	}
	return result;
}

// Forgets the messages that are sent (they got their real ids), failed
// or gone with the session. True if some are still on the way.
[[nodiscard]] bool StillSending(std::vector<UploadedMessage> &list) {
	list.erase(ranges::remove_if(list, [](const UploadedMessage &entry) {
		const auto session = entry.session.get();
		const auto item = session
			? session->data().message(entry.id)
			: nullptr;
		return !item || !item->isSending();
	}), end(list));
	return !list.empty();
}

// An upload ends before its message is sent: the request only starts
// then, quitting at that moment would lose the message.
[[nodiscard]] bool UploadWaitGoesOn(
		bool uploading,
		bool seen,
		bool sending,
		crl::time passed,
		crl::time idle) {
	return uploading
		? (passed < kUploadWait)
		: ((!seen && (passed < kUploadStartWait))
			|| (sending && (idle < kSendWait)));
}

void FinishSendsWait() {
	auto &wait = SendsWait();
	wait.handover = false;
	wait.uploads.clear();
	// Taken first: closing the box withdraws the quit (that is its
	// cancel) and a repeated log out comes back to the hooks below.
	const auto quit = base::take(wait.quit);
	const auto logouts = base::take(wait.logouts);
	if (const auto box = wait.quitBox.get()) {
		box->closeBox();
	}
	for (const auto &logout : logouts) {
		if (logout.retry) {
			logout.retry();
		}
	}
	if (quit) {
		Core::Quit();
	}
}

void WaitForVoiceUploads(crl::time started, bool seen, int generation) {
	base::call_delayed(kUploadCheckStep, [=] {
		auto &wait = SendsWait();
		if (wait.generation != generation) {
			return;
		} else if (!Core::IsAppLaunched() || Core::Quitting()) {
			wait.handover = false;
			wait.quit = false;
			wait.logouts.clear();
			wait.uploads.clear();
			return;
		} else if (PendingVoiceSends > 0) {
			// One more was sent meanwhile, the wait restarts after it
			// (the counter holds the quit and the log out till then).
			wait.handover = false;
			return;
		}
		const auto now = crl::now();
		const auto uploading = RememberUploads(wait.uploads);
		if (uploading) {
			wait.uploadSeen = now;
		}
		const auto more = UploadWaitGoesOn(
			uploading,
			seen,
			StillSending(wait.uploads),
			now - started,
			now - wait.uploadSeen);
		if (more) {
			WaitForVoiceUploads(started, seen || uploading, generation);
		} else {
			FinishSendsWait();
		}
	});
}

// A voice message went from InterceptVoiceSend() to ApiWrap (or was
// dropped with its session).
void VoiceSendHandedOver() {
	auto &wait = SendsWait();
	if (PendingVoiceSends > 0) {
		return;
	} else if (!wait.quit && wait.logouts.empty() && !wait.handover) {
		wait.uploads.clear();
		return;
	}
	wait.handover = true;
	WaitForVoiceUploads(crl::now(), false, ++wait.generation);
}

struct VoiceEffectBoxArgs {
	std::optional<Effect> initial; // Default: the chosen effect.
	std::optional<bool> denoise; // Default: the noise suppression setting.

	// A test recording to start with (the UI snapshots show the box
	// with one), the box records its own otherwise.
	std::optional<Audio::Pcm> sample;
};

// The presets and the custom pitch as radio buttons in two columns
// (filled top to bottom), so the box stays short enough for a small
// window. The grid spans the whole box width, the radio buttons get
// the box padding and keep their ripple margins inside it.
class EffectRadios final : public Ui::RpWidget {
public:
	EffectRadios(
		QWidget *parent,
		std::shared_ptr<Ui::RadiobuttonGroup> group);

protected:
	int resizeGetHeight(int newWidth) override;

private:
	std::vector<not_null<Ui::Radiobutton*>> _radios;

};

EffectRadios::EffectRadios(
		QWidget *parent,
		std::shared_ptr<Ui::RadiobuttonGroup> group)
: RpWidget(parent) {
	auto options = std::vector<std::pair<EffectType, QString>>();
	for (const auto type : kPresets) {
		options.emplace_back(type, EffectName(Effect{ .type = type }));
	}
	options.emplace_back(
		EffectType::Pitch,
		tr::lng_oblivion_voice_effect_custom(tr::now));
	_radios.reserve(options.size());
	for (const auto &[type, text] : options) {
		const auto radio = Ui::CreateChild<Ui::Radiobutton>(
			this,
			group,
			int(type),
			text,
			st::defaultBoxCheckbox);
		radio->setAllowTextLines(2);
		_radios.push_back(radio);
	}
}

int EffectRadios::resizeGetHeight(int newWidth) {
	const auto count = int(_radios.size());
	const auto rows = (count + 1) / 2;
	const auto left = st::boxPadding.left()
		+ st::boxOptionListPadding.left();
	const auto available = std::max(
		newWidth - left - st::boxPadding.right(),
		2);
	const auto columnWidth = available / 2;

	// Leaves room above the first row for its ripple.
	auto top = st::boxLittleSkip;
	for (auto row = 0; row != rows; ++row) {
		auto rowHeight = 0;
		for (auto column = 0; column != 2; ++column) {
			const auto index = column * rows + row;
			if (index >= count) {
				continue;
			}
			const auto radio = _radios[index];
			const auto margins = radio->getMargins();
			radio->resizeToWidth(column
				? (available - columnWidth)
				: columnWidth);
			radio->moveToLeft(left + column * columnWidth, top, newWidth);
			rowHeight = std::max(
				rowHeight,
				radio->height() - margins.top() - margins.bottom());
		}
		top += rowHeight + st::boxOptionListSkip;
	}
	return top;
}

void VoiceEffectBox(
		not_null<Ui::GenericBox*> box,
		VoiceEffectBoxArgs &&args) {
	using Player = Audio::PreviewPlayer;
	using namespace ::Media::Capture;

	struct State final : base::has_weak_ptr {
		~State() {
			if (captureOwned) {
				if (const auto capture = ::Media::Capture::instance()) {
					capture->stop();
				}
			}
		}

		Effect effect;
		bool denoise = false;
		rpl::variable<int> customPitch = kDefaultCustomPitch;
		std::optional<Audio::Pcm> sample;
		std::unique_ptr<Player> player;
		rpl::lifetime playerLifetime;
		rpl::variable<QString> status;
		rpl::variable<bool> recording = false;
		rpl::variable<bool> hasSample = false;
		rpl::variable<bool> playing = false;
		bool captureOwned = false;
		rpl::lifetime captureLifetime;
		uint64 requestId = 0;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto weak = base::make_weak(state);

	const auto start = args.initial ? *args.initial : CurrentEffect();
	state->effect = start.active() ? start : Effect();
	state->denoise = args.denoise.value_or(Get().voiceNoiseSuppression());
	state->customPitch = (start.type == EffectType::Pitch && start.active())
		? start.semitones
		: LastCustomPitch;
	if (start.type == EffectType::Pitch && !start.active()) {
		// An explicitly requested custom pitch without a value.
		state->effect = Effect{
			.type = EffectType::Pitch,
			.semitones = state->customPitch.current(),
		};
	}

	box->setTitle(tr::lng_oblivion_tools_voice_effect());
	box->setWidth(st::boxWideWidth);

	const auto content = box->verticalLayout();

	const auto group = std::make_shared<Ui::RadiobuttonGroup>(
		int(state->effect.type));
	content->add(object_ptr<EffectRadios>(content, group));

	const auto pitchWrap = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)));
	const auto pitch = pitchWrap->entity();

	// The title on the left and the value on the right above the slider,
	// as the size limit sliders in the settings show them.
	const auto labelSt = box->lifetime().make_state<style::LabelSimple>(
		st::defaultLabelSimple);
	labelSt->font = st::boxTextFont;
	const auto valueSt = box->lifetime().make_state<style::LabelSimple>(
		*labelSt);
	valueSt->textFg = st::windowActiveTextFg;
	const auto pitchRow = pitch->add(
		object_ptr<Ui::FixedHeightWidget>(pitch, labelSt->font->height),
		st::boxRowPadding);
	const auto pitchTitle = Ui::CreateChild<Ui::LabelSimple>(
		pitchRow,
		*labelSt,
		tr::lng_oblivion_voice_effect_pitch_title(tr::now));
	const auto pitchValue = Ui::CreateChild<Ui::LabelSimple>(
		pitchRow,
		*valueSt);
	state->customPitch.value() | rpl::on_next([=](int value) {
		pitchValue->setText(FormatSemitones(value));
	}, pitchRow->lifetime());
	rpl::combine(
		pitchRow->widthValue(),
		pitchValue->widthValue()
	) | rpl::on_next([=](int width, int) {
		pitchTitle->moveToLeft(0, 0, width);
		pitchValue->moveToRight(0, 0, width);
	}, pitchRow->lifetime());

	const auto sliderSt = pitch->lifetime().make_state<style::MediaSlider>(
		st::defaultContinuousSlider);
	sliderSt->seekSize = QSize(
		style::ConvertScale(15),
		style::ConvertScale(15));
	const auto slider = pitch->add(
		object_ptr<Ui::MediaSlider>(pitch, *sliderSt),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
	slider->resize(slider->width(), sliderSt->seekSize.height());
	// The same space as below the radio buttons when this is hidden.
	Ui::AddSkip(pitch, st::boxOptionListSkip);

	// Recording test and preview.
	const auto ensurePlayer = [=] {
		if (!state->player) {
			state->player = std::make_unique<Player>();
			state->player->stateValue(
			) | rpl::on_next([=](Player::State value) {
				state->playing = (value == Player::State::Playing);
			}, state->playerLifetime);
		}
		return state->player.get();
	};
	const auto readyText = [=] {
		return tr::lng_oblivion_voice_effect_ready(
			tr::now,
			lt_duration,
			Ui::FormatDurationText(
				state->sample ? (state->sample->duration() / 1000) : 0));
	};
	const auto refreshPreview = [=](bool autoplay) {
		if (!state->sample) {
			return;
		}
		const auto id = ++state->requestId;
		const auto processing = Processing{
			.effect = state->effect,
			.denoise = state->denoise,
		};
		const auto source = *state->sample;
		state->status = tr::lng_oblivion_voice_effect_processing(tr::now);
		crl::async([=] {
			auto result = PreviewVoice(source, processing);
			crl::on_main(weak, [=, result = std::move(result)]() mutable {
				if (state->requestId != id) {
					return;
				} else if (result.empty()) {
					state->status = PreviewFailedText(processing);
					return;
				}
				state->status = readyText();
				const auto player = ensurePlayer();
				player->setPcm(std::move(result));
				if (autoplay) {
					player->play();
				}
			});
		});
	};
	const auto chooseEffect = [=](Effect effect) {
		if (state->effect == effect) {
			return;
		}
		state->effect = effect;
		refreshPreview(true);
	};

	group->setChangedCallback([=](int value) {
		const auto type = EffectType(value);
		chooseEffect((type == EffectType::Pitch)
			? Effect{
				.type = EffectType::Pitch,
				.semitones = state->customPitch.current(),
			}
			: Effect{ .type = type });
	});
	slider->setPseudoDiscrete(
		kPitchPositions,
		[](int index) { return PitchFromPosition(index); },
		state->customPitch.current(),
		[=](int value) {
			state->customPitch = value;
		},
		[=](int value) {
			state->customPitch = value;
			LastCustomPitch = value;
			if (group->current() == int(EffectType::Pitch)) {
				chooseEffect(Effect{
					.type = EffectType::Pitch,
					.semitones = value,
				});
			}
		});
	pitchWrap->toggleOn(group->value() | rpl::map([](int value) {
		return (value == int(EffectType::Pitch));
	}));
	pitchWrap->finishAnimating();

	// The effect and the noise suppression are two separate settings, so
	// each one is a section that ends with a divider text about it (as in
	// the settings), not one long text about both below the checkbox.
	// The default divider padding is the settings one (22px), here the
	// text keeps the left edge of the rest of the box (boxRowPadding).
	const auto dividerPadding = QMargins(
		st::boxRowPadding.left(),
		st::defaultBoxDividerLabelPadding.top(),
		st::boxRowPadding.right(),
		st::defaultBoxDividerLabelPadding.bottom());
	Ui::AddDividerText(
		content,
		tr::lng_oblivion_voice_effect_about(),
		dividerPadding);

	// The noise suppression works with any effect and without one. The
	// checkbox keeps the left edge of the radio buttons above it and has
	// the same space above and below, between the two dividers.
	const auto denoiseSkip = st::boxLittleSkip + st::defaultVerticalListSkip;
	const auto denoise = content->add(
		object_ptr<Ui::Checkbox>(
			content,
			tr::lng_oblivion_voice_noise_toggle(tr::now),
			state->denoise,
			st::defaultBoxCheckbox),
		QMargins(
			st::boxPadding.left() + st::boxOptionListPadding.left(),
			denoiseSkip,
			st::boxPadding.right(),
			denoiseSkip));
	denoise->checkedChanges() | rpl::on_next([=](bool checked) {
		if (state->denoise != checked) {
			state->denoise = checked;
			refreshPreview(true);
		}
	}, denoise->lifetime());

	Ui::AddDividerText(
		content,
		tr::lng_oblivion_voice_noise_about(),
		dividerPadding);
	Ui::AddSkip(content);
	// Aligned with the labels and buttons below it (box row padding).
	Ui::AddSubsectionTitle(
		content,
		tr::lng_oblivion_voice_effect_sample(),
		QMargins(
			(st::boxRowPadding.left()
				- st::defaultSubsectionTitlePadding.left()),
			0,
			0,
			0));
	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			tr::lng_oblivion_voice_effect_sample_about(),
			st::boxDividerLabel),
		st::boxRowPadding);
	// About the same space as between the subsection title and the text
	// above and between the buttons and the status below (the label ends
	// right at the descenders of its last line).
	Ui::AddSkip(content, style::ConvertScale(14));

	const auto buttons = content->add(
		object_ptr<Ui::FixedHeightWidget>(
			content,
			st::defaultActiveButton.height),
		st::boxRowPadding);
	const auto record = Ui::CreateChild<Ui::RoundButton>(
		buttons,
		state->recording.value() | rpl::map([](bool recording) {
			return recording
				? tr::lng_oblivion_voice_effect_stop()
				: tr::lng_oblivion_voice_effect_record();
		}) | rpl::flatten_latest(),
		st::defaultActiveButton);
	const auto play = Ui::CreateChild<Ui::RoundButton>(
		buttons,
		state->playing.value() | rpl::map([](bool playing) {
			return playing
				? tr::lng_oblivion_voice_effect_pause()
				: tr::lng_oblivion_voice_effect_play();
		}) | rpl::flatten_latest(),
		st::defaultLightButton);
	play->showOn(rpl::combine(
		state->hasSample.value(),
		state->recording.value()
	) | rpl::map([](bool has, bool recording) {
		return has && !recording;
	}));
	rpl::combine(
		buttons->widthValue(),
		record->widthValue()
	) | rpl::on_next([=](int, int recordWidth) {
		record->moveToLeft(0, 0);
		play->moveToLeft(recordWidth + st::boxLittleSkip, 0);
	}, buttons->lifetime());

	// The status line is never empty, so the box keeps its height (and
	// its place) when a recording starts.
	state->status = tr::lng_oblivion_voice_effect_no_sample(tr::now);
	Ui::AddSkip(content, st::boxLittleSkip);
	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			state->status.value(),
			st::boxDividerLabel),
		st::boxRowPadding);
	Ui::AddSkip(content);

	const auto failRecording = [=](const QString &text) {
		state->captureLifetime.destroy();
		state->recording = false;
		state->status = text;
	};
	const auto stopRecording = [=] {
		if (!state->recording.current()) {
			return;
		}
		state->captureLifetime.destroy();
		state->recording = false;
		state->status = tr::lng_oblivion_voice_effect_processing(tr::now);
		const auto capture = instance();
		if (!capture) {
			state->captureOwned = false;
			state->status = tr::lng_oblivion_voice_effect_record_failed(
				tr::now);
			return;
		}
		capture->stop(crl::guard(weak, [=](Result &&result) {
			state->captureOwned = false;
			const auto bytes = result.bytes;
			if (bytes.isEmpty()) {
				state->status = tr::lng_oblivion_voice_effect_too_short(
					tr::now);
				return;
			}
			crl::async([=] {
				auto decoded = Audio::Decode(
					bytes,
					{ .rate = kRate, .channels = 1 });
				crl::on_main(weak, [=, decoded = std::move(decoded)] {
					if (!decoded || decoded->empty()) {
						state->status
							= tr::lng_oblivion_voice_effect_record_failed(
								tr::now);
						return;
					}
					state->sample = *decoded;
					state->hasSample = true;
					refreshPreview(true);
				});
			});
		}));
	};
	const auto startRecording = [=] {
		const auto capture = instance();
		if (!capture || !capture->available()) {
			box->showToast(
				tr::lng_oblivion_voice_effect_mic_unavailable(tr::now));
			return;
		} else if (capture->started() || state->captureOwned) {
			box->showToast(tr::lng_oblivion_voice_effect_mic_busy(tr::now));
			return;
		}
		if (state->player) {
			state->player->stop();
		}
		++state->requestId;
		state->captureOwned = true;
		state->recording = true;
		state->status = tr::lng_oblivion_voice_effect_recording(
			tr::now,
			lt_duration,
			Ui::FormatDurationText(0));
		capture->start();
		// Both handlers destroy captureLifetime (and themselves),
		// so they do it from the next main loop iteration.
		capture->updated(
		) | rpl::on_next_error([=](const Update &update) {
			const auto ms = crl::time(update.samples)
				* 1000
				/ ::Media::Player::kDefaultFrequency;
			state->status = tr::lng_oblivion_voice_effect_recording(
				tr::now,
				lt_duration,
				Ui::FormatDurationText(ms / 1000));
			if (ms >= kSampleMaxDuration) {
				crl::on_main(weak, stopRecording);
			}
		}, [=](Error) {
			state->captureOwned = false;
			crl::on_main(weak, [=] {
				failRecording(
					tr::lng_oblivion_voice_effect_record_failed(tr::now));
			});
		}, state->captureLifetime);
	};
	record->setClickedCallback([=] {
		if (state->recording.current()) {
			stopRecording();
		} else {
			startRecording();
		}
	});
	play->setClickedCallback([=] {
		const auto player = state->player.get();
		if (!player) {
			// The test recording has no preview yet (a given sample).
			refreshPreview(true);
			return;
		} else if (player->state() == Player::State::Playing) {
			player->pause();
		} else {
			player->play();
		}
	});

	if (args.sample && !args.sample->empty()) {
		// The preview is made when it is played for the first time.
		state->sample = std::move(*args.sample);
		state->hasSample = true;
		state->status = readyText();
	}

	box->addButton(tr::lng_settings_save(), [=] {
		stopRecording();
		Get().setVoiceEffect(EffectId(state->effect));
		Get().setVoiceNoiseSuppression(state->denoise);
		box->closeBox();
	});
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
}

void ShowBox(std::shared_ptr<Ui::Show> show, std::optional<Effect> initial) {
	if (show) {
		show->showBox(Box(VoiceEffectBox, VoiceEffectBoxArgs{
			.initial = initial,
		}));
	}
}

void FillMenu(
		not_null<Ui::PopupMenu*> menu,
		std::shared_ptr<Ui::Show> show) {
	const auto current = CurrentEffect();
	const auto choose = [=](Effect effect) {
		Get().setVoiceEffect(EffectId(effect));
		if (show) {
			show->showToast(effect.active()
				? tr::lng_oblivion_voice_effect_chosen(
					tr::now,
					lt_effect,
					EffectName(effect))
				: tr::lng_oblivion_voice_effect_disabled(tr::now));
		}
	};
	for (const auto type : kPresets) {
		const auto effect = Effect{ .type = type };
		const auto checked = current.active()
			? (current.type == type)
			: (type == EffectType::None);
		Menu::AddCheckedAction(
			menu,
			EffectName(effect),
			[=] { choose(effect); },
			EffectIcon(type),
			checked);
	}
	const auto custom = current.active()
		&& (current.type == EffectType::Pitch);
	Menu::AddCheckedAction(
		menu,
		(custom
			? EffectName(current)
			: tr::lng_oblivion_voice_effect_custom_menu(tr::now)),
		[=] {
			ShowBox(show, Effect{
				.type = EffectType::Pitch,
				.semitones = custom ? current.semitones : LastCustomPitch,
			});
		},
		EffectIcon(EffectType::Pitch),
		custom);
	menu->addSeparator();
	const auto denoise = Get().voiceNoiseSuppression();
	Menu::AddCheckedAction(
		menu,
		tr::lng_oblivion_voice_noise_toggle(tr::now),
		[=] {
			Get().setVoiceNoiseSuppression(!denoise);
			if (show) {
				show->showToast(denoise
					? tr::lng_oblivion_voice_noise_off(tr::now)
					: tr::lng_oblivion_voice_noise_on(tr::now));
			}
		},
		&st::menuIconSilent,
		denoise);
	menu->addAction(
		tr::lng_oblivion_voice_effect_menu_setup(tr::now),
		[=] { ShowBox(show, std::nullopt); },
		&st::menuIconSettings);
}

// A test recording for the UI snapshots: a voice-like buzz in syllables,
// four seconds long, made in code.
[[nodiscard]] Audio::Pcm SnapshotSample() {
	constexpr auto kSeconds = 4;
	constexpr auto kBase = 140.; // Hz.
	constexpr auto kSyllables = 3.; // Per second.
	constexpr auto kHarmonics = 6;
	constexpr auto kVolume = 0.25;

	auto result = Audio::Pcm{ .channels = 1, .rate = kRate };
	const auto frames = kRate * kSeconds;
	result.samples.resize(size_t(frames));
	for (auto i = 0; i != frames; ++i) {
		const auto t = double(i) / kRate;
		const auto syllable = std::sin(M_PI * std::fmod(t * kSyllables, 1.));
		auto value = 0.;
		for (auto h = 1; h <= kHarmonics; ++h) {
			value += std::sin(2. * M_PI * kBase * h * t) / h;
		}
		result.samples[i] = float(kVolume * syllable * syllable * value);
	}
	return result;
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	// The window is wider than the box, the image is cropped to the box.
	const auto window = QSize(style::ConvertScale(720), 0);

	// The custom pitch is chosen, so its slider is shown.
	RegisterBoxScene(u"voice_effects"_q, window, [](
			std::shared_ptr<Ui::Show>) {
		return Box(VoiceEffectBox, VoiceEffectBoxArgs{
			.initial = Effect{
				.type = EffectType::Pitch,
				.semitones = 5,
			},
			.denoise = false,
		});
	});

	// A preset is chosen, the noise suppression is on and there is
	// a test recording to play.
	RegisterBoxScene(u"voice_effects_sample"_q, window, [](
			std::shared_ptr<Ui::Show>) {
		return Box(VoiceEffectBox, VoiceEffectBoxArgs{
			.initial = Effect{ .type = EffectType::Robot },
			.denoise = true,
			.sample = SnapshotSample(),
		});
	});

	// Only the noise suppression, without an effect.
	RegisterBoxScene(u"voice_effects_noise"_q, window, [](
			std::shared_ptr<Ui::Show>) {
		return Box(VoiceEffectBox, VoiceEffectBoxArgs{
			.initial = Effect(),
			.denoise = true,
		});
	});
});

} // namespace

QString VoiceEffectName(const QString &id) {
	return id.isEmpty()
		? tr::lng_oblivion_tools_voice_effect_none(tr::now)
		: EffectName(ParseEffect(id));
}

bool VoiceEffectEnabled() {
	return CurrentEffect().active();
}

void ShowVoiceEffectBox(not_null<Window::SessionController*> controller) {
	ShowBox(controller->uiShow(), std::nullopt);
}

void ShowVoiceEffectBox(std::shared_ptr<Ui::Show> show) {
	ShowBox(std::move(show), std::nullopt);
}

void ApplyVoiceEffect(VoiceClip clip, Fn<void(VoiceClip)> done) {
	const auto processing = CurrentProcessing();
	if (!processing.active()
		|| clip.bytes.isEmpty()
		|| (clip.duration > kMaxClip)) {
		if (done) {
			done(std::move(clip));
		}
		return;
	}
	crl::async([=] {
		auto result = ProcessVoice(
			clip.bytes,
			clip.waveform,
			clip.duration,
			processing,
			0,
			true);
		crl::on_main([=, result = std::move(result)]() mutable {
			if (!done) {
				return;
			} else if (!result) {
				done(clip);
				return;
			}
			MarkProcessed(result->bytes, kProcessedAll);
			done(VoiceClip{
				.bytes = std::move(result->bytes),
				.waveform = std::move(result->waveform),
				.duration = result->duration,
			});
		});
	});
}

VoiceCaptureCallback WrapVoicePreviewCapture(
		VoiceCaptureCallback callback,
		rpl::lifetime &alive,
		std::shared_ptr<Ui::Show> show) {
	const auto processing = CurrentProcessing();
	if (!processing.active() || !callback) {
		return callback;
	}
	const auto guard = std::make_shared<bool>(true);
	alive.add([=] { *guard = false; });
	return [=](::Media::Capture::Result &&data) {
		if (!*guard) {
			return;
		} else if (data.bytes.isEmpty()) {
			callback(std::move(data));
			return;
		} else if (data.duration > kMaxClip) {
			if (show) {
				show->showToast(TooLongText(processing));
			}
			callback(std::move(data));
			return;
		}
		const auto bytes = data.bytes;
		const auto waveform = data.waveform;
		const auto duration = data.duration;
		if (duration >= kLongClip && show) {
			show->showToast(ApplyingText(processing));
		}
		crl::async([=] {
			// The preview may be continued and joined later, so the length
			// is kept exactly (no echo / reverb tail here).
			auto result = ProcessVoice(
				bytes,
				waveform,
				duration,
				processing,
				0,
				false);
			crl::on_main([=, result = std::move(result)]() mutable {
				if (!*guard) {
					return;
				}
				auto ready = ::Media::Capture::Result{
					.bytes = bytes,
					.waveform = waveform,
					.duration = duration,
				};
				if (result) {
					MarkProcessed(result->bytes, kProcessedAll);
					ready.bytes = std::move(result->bytes);
					ready.waveform = std::move(result->waveform);
				} else if (show) {
					show->showToast(PreviewFailedText(processing));
				}
				const auto was = std::exchange(
					ProcessedCaptureActive,
					result.has_value());
				callback(std::move(ready));
				ProcessedCaptureActive = was;
			});
		});
	};
}

bool VoicePreviewProcessed() {
	return CurrentProcessing().active();
}

void VoiceRecordMerged(
		const QByteArray &prefix,
		crl::time prefixDuration,
		const QByteArray &combined) {
	if (prefix.isEmpty() || combined.isEmpty() || (combined == prefix)) {
		return;
	}
	// Only a processed start can be remembered. If the prefix has none
	// (its preview failed), nothing is marked even when the new part got
	// the effect before joining: the whole message is processed when it
	// is sent, so no part of it goes out as recorded.
	const auto state = ProcessedPrefix(prefix);
	if (state == kProcessedAll) {
		MarkProcessed(
			combined,
			ProcessedCaptureActive ? kProcessedAll : prefixDuration);
	} else if (state && (*state > 0)) {
		MarkProcessed(combined, std::min(*state, prefixDuration));
	}
}

void VoiceRecordTrimmed(
		const QByteArray &was,
		const QByteArray &now,
		crl::time from) {
	const auto state = ProcessedPrefix(was);
	if (!state || now.isEmpty()) {
		return;
	} else if (*state == kProcessedAll) {
		MarkProcessed(now, kProcessedAll);
	} else if (*state > from) {
		MarkProcessed(now, *state - from);
	}
}

void SetupVoiceEffectButton(
		not_null<Ui::RpWidget*> owner,
		not_null<Ui::RpWidget*> button,
		std::shared_ptr<Ui::Show> show,
		Fn<bool()> enabled) {
	struct State {
		base::unique_qptr<Ui::PopupMenu> menu;
		base::unique_qptr<Ui::RpWidget> badge;

		~State() {
			// Children of the button must not be destroyed right here:
			// that sends a ChildRemoved event to the button while the
			// owner (a half-destroyed record bar) still listens to the
			// button events. Drop everything that refers to the owner
			// and let Qt delete the widgets later (or with the button).
			if (const auto raw = badge.release()) {
				raw->lifetime().destroy();
				raw->deleteLater();
			}
			if (const auto raw = menu.release()) {
				raw->deleteLater();
			}
		}
	};
	const auto state = owner->lifetime().make_state<State>();
	const auto showMenu = [=] {
		state->menu = base::make_unique_q<Ui::PopupMenu>(
			button,
			st::popupMenuWithIcons);
		FillMenu(state->menu.get(), show);
		state->menu->popup(QCursor::pos());
	};
	base::install_event_filter(owner, button, [=](not_null<QEvent*> e) {
		using Result = base::EventFilterResult;
		const auto type = e->type();
		if (type == QEvent::MouseButtonPress
			|| type == QEvent::MouseButtonRelease
			|| type == QEvent::MouseButtonDblClick) {
			const auto mouse = static_cast<QMouseEvent*>(e.get());
			if (mouse->button() != Qt::RightButton || !enabled()) {
				return Result::Continue;
			} else if (type == QEvent::MouseButtonPress && !state->menu) {
				showMenu();
			}
			// The record bar must not start recording on this press.
			return Result::Cancel;
		} else if (type == QEvent::ContextMenu) {
			if (!enabled()) {
				return Result::Continue;
			} else if (!state->menu) {
				showMenu();
			}
			return Result::Cancel;
		}
		return Result::Continue;
	});

	state->badge = base::make_unique_q<Ui::RpWidget>(button);
	const auto badge = state->badge.get();
	badge->setAttribute(Qt::WA_TransparentForMouseEvents);
	button->sizeValue() | rpl::on_next([=](QSize size) {
		badge->setGeometry(QRect(QPoint(), size));
	}, badge->lifetime());
	badge->paintRequest() | rpl::on_next([=] {
		if (!VoiceEffectEnabled() || !enabled()) {
			return;
		}
		auto p = QPainter(badge);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = style::ConvertScaleExact(3.5);
		const auto shift = style::ConvertScale(9);
		const auto center = QPointF(
			badge->width() / 2. + shift,
			badge->height() / 2. - shift);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgActive);
		p.drawEllipse(center, radius, radius);
	}, badge->lifetime());
	Get().changes() | rpl::on_next([=] {
		badge->update();
	}, badge->lifetime());
	badge->show();
}

bool InterceptVoiceSend(
		not_null<Main::Session*> session,
		const QByteArray &bytes,
		const QVector<signed char> &waveform,
		crl::time duration,
		bool video,
		const Api::SendAction &action) {
	if (bytes.isEmpty() || action.options.oblivionAnnounced) {
		return false;
	}
	const auto processing = video ? Processing() : CurrentProcessing();
	const auto state = processing.active()
		? ProcessedPrefix(bytes)
		: std::optional<crl::time>(kProcessedAll);
	const auto wanted = (state != kProcessedAll);
	const auto tooLong = wanted && (duration > kMaxClip);
	const auto process = wanted && !tooLong;
	const auto peer = action.history->peer;
	if (tooLong) {
		ShowToast(session, peer, TooLongText(processing));
	}
	if (!process && !PendingVoiceSends) {
		return false;
	} else if (process && (duration >= kLongClip)) {
		ShowToast(session, peer, ApplyingText(processing));
	}
	const auto keepPrefix = process ? state.value_or(0) : crl::time(0);
	const auto weak = base::make_weak(session.get());

	// The chat handles the send right away, as for any voice message:
	// the reply is cleared, the history is shown at the end and so on
	// (SendConfirmedFile() would do it, it skips that for "later" below).
	auto now = action;
	now.clearDraft = false;
	crl::on_main(weak, [=] {
		session->api().sendAction(now);
	});
	auto later = action;
	later.options.oblivionAnnounced = true;

	// One at a time, so voice messages keep their order. While some are
	// processed the others (no effect, already processed, round videos)
	// go through the queue unchanged.
	static auto queue = crl::queue();
	++PendingVoiceSends;
	queue.async([=] {
		auto result = std::optional<VoiceResult>();
		if (process) {
			result = ProcessVoice(
				bytes,
				waveform,
				duration,
				processing,
				keepPrefix,
				true);
		}
		// Not guarded, the counter goes down even without the session.
		crl::on_main([=, result = std::move(result)]() mutable {
			--PendingVoiceSends;
			if (const auto strong = weak.get()) {
				auto send = VoiceResult{
					.bytes = bytes,
					.waveform = waveform,
					.duration = duration,
				};
				if (result) {
					send = std::move(*result);
				} else if (process) {
					ShowToast(strong, peer, FailedText(processing));
				}
				strong->api().sendVoiceMessage(
					send.bytes,
					send.waveform,
					send.duration,
					video,
					later);
			}
			VoiceSendHandedOver();
		});
	});
	return true;
}

bool VoiceSendsPreventQuit() {
	auto &wait = SendsWait();
	if (base::take(wait.quitAtOnce)) {
		wait.quit = false;
		return false;
	} else if (!PendingVoiceSends && !wait.handover) {
		return false;
	}
	const auto now = crl::now();
	const auto window = Core::App().activePrimaryWindow();
	if (!wait.quit) {
		wait.quit = true;
		wait.quitAsked = now;
		if (window) {
			window->showToast(
				tr::lng_oblivion_voice_quit_wait(tr::now),
				kQuitToastDuration);
		}
	} else if (window
		&& !wait.quitBox
		&& (now - wait.quitAsked >= kQuitAskAgain)) {
		wait.quitAsked = now;
		wait.quitBox = window->show(Ui::MakeConfirmBox({
			.text = tr::lng_oblivion_voice_quit_sure(),
			.confirmed = [](Fn<void()> close) {
				close();
				SendsWait().quitAtOnce = true;
				Core::Quit();
			},
			// The button, Escape and a click outside of the box: the
			// postponed quit is withdrawn, the message is sent as usual.
			.cancelled = [](Fn<void()> close) {
				SendsWait().quit = false;
				close();
			},
			.confirmText = tr::lng_oblivion_voice_quit_now(),
			.cancelText = tr::lng_oblivion_voice_quit_cancel(),
			.confirmStyle = &st::attentionBoxButton,
		}));
		window->activate();
	}
	return true;
}

bool VoiceSendsDelayLogout(
		not_null<Main::Account*> account,
		Fn<void()> retry) {
	auto &wait = SendsWait();
	if (!PendingVoiceSends && !wait.handover) {
		return false;
	}
	const auto known = [&](const DelayedLogout &entry) {
		return (entry.account.get() == account.get());
	};
	if (!ranges::any_of(wait.logouts, known)) {
		wait.logouts.push_back({
			base::make_weak(account.get()),
			std::move(retry),
		});
	}
	if (const auto window = Core::App().activePrimaryWindow()) {
		window->showToast(tr::lng_oblivion_voice_logout_wait(tr::now));
	}
	return true;
}

bool RunVoiceChangerSelfTest(QStringList &log) {
	auto passed = 0;
	auto failed = 0;
	const auto check = [&](
			bool condition,
			const QString &name,
			const QString &details = QString()) {
		log.push_back(u"voice: "_q
			+ (condition ? u"OK   "_q : u"FAIL "_q)
			+ name
			+ (details.isEmpty() ? QString() : (u" ("_q + details + ')')));
		++(condition ? passed : failed);
		return condition;
	};
	const auto text = [](std::optional<crl::time> state) {
		return !state
			? u"unknown"_q
			: (*state == kProcessedAll)
			? u"all"_q
			: (QString::number(*state) + u" ms"_q);
	};

	// Which part of a recording already has the effect, through the
	// joins and trims of the record bar.
	{
		const auto savedMarks = base::take(Registry());
		const auto savedActive = std::exchange(
			ProcessedCaptureActive,
			false);
		const auto restore = gsl::finally([&] {
			Registry() = savedMarks;
			ProcessedCaptureActive = savedActive;
		});
		const auto fake = [](char fill, int size) {
			return QByteArray(size, fill);
		};
		const auto state = [&](const QByteArray &bytes) {
			return ProcessedPrefix(bytes);
		};

		const auto done = fake('a', 100);
		const auto raw = fake('b', 110);
		check(!state(done), u"a new recording is not marked"_q);
		MarkProcessed(done, kProcessedAll);
		check(
			state(done) == kProcessedAll,
			u"a processed preview is marked"_q,
			text(state(done)));
		check(
			!state(fake('a', 101)) && !state(fake('c', 100)),
			u"other bytes are not"_q);

		const auto rawTail = fake('d', 200);
		VoiceRecordMerged(done, 3000, rawTail);
		check(
			state(rawTail) == crl::time(3000),
			u"processed start + raw rest: the start is marked"_q,
			text(state(rawTail)));

		ProcessedCaptureActive = true;
		const auto bothDone = fake('e', 210);
		VoiceRecordMerged(done, 3000, bothDone);
		check(
			state(bothDone) == kProcessedAll,
			u"processed start + processed rest: all is marked"_q,
			text(state(bothDone)));

		const auto rawStart = fake('f', 220);
		VoiceRecordMerged(raw, 3000, rawStart);
		check(
			!state(rawStart),
			u"raw start + processed rest: nothing is marked"_q,
			text(state(rawStart)));

		const auto partStart = fake('g', 230);
		VoiceRecordMerged(rawTail, 5000, partStart);
		check(
			state(partStart) == crl::time(3000),
			u"partly processed start: only that part is marked"_q,
			text(state(partStart)));
		ProcessedCaptureActive = false;

		const auto same = fake('h', 240);
		VoiceRecordMerged(done, 3000, done);
		VoiceRecordMerged(QByteArray(), 0, same);
		check(
			(state(done) == kProcessedAll) && !state(same),
			u"nothing joined: the marks stay"_q);

		const auto cutAll = fake('i', 90);
		const auto cutPart = fake('j', 150);
		const auto cutRaw = fake('k', 60);
		const auto cutUnknown = fake('l', 70);
		VoiceRecordTrimmed(done, cutAll, 500);
		VoiceRecordTrimmed(rawTail, cutPart, 1000);
		VoiceRecordTrimmed(rawTail, cutRaw, 3500);
		VoiceRecordTrimmed(raw, cutUnknown, 500);
		check(
			state(cutAll) == kProcessedAll,
			u"a trimmed processed recording stays processed"_q,
			text(state(cutAll)));
		check(
			state(cutPart) == crl::time(2000),
			u"a trim moves the processed start"_q,
			text(state(cutPart)));
		check(
			!state(cutRaw) && !state(cutUnknown),
			u"a trim to the raw part is not marked"_q);

		for (auto i = 0; i != int(kRegistryLimit); ++i) {
			MarkProcessed(fake('z', 1000 + i), kProcessedAll);
		}
		check(
			(Registry().size() == kRegistryLimit)
				&& !state(done)
				&& (state(fake('z', 1000)) == kProcessedAll),
			u"only the latest marks are kept"_q,
			QString::number(Registry().size()));
	}

	// Quit and log out wait for the message, not only for its upload.
	check(
		UploadWaitGoesOn(false, false, false, 100, 100)
			&& !UploadWaitGoesOn(false, false, false, kUploadStartWait, 0),
		u"the start of the upload is awaited for a while"_q);
	check(
		UploadWaitGoesOn(true, true, true, kUploadWait - 1, 0)
			&& !UploadWaitGoesOn(true, true, true, kUploadWait, 0),
		u"a long upload is left to the usual question"_q);
	check(
		UploadWaitGoesOn(false, true, true, 500, kUploadCheckStep)
			&& UploadWaitGoesOn(false, false, true, kUploadWait, 500)
			&& !UploadWaitGoesOn(false, true, false, 500, kUploadCheckStep)
			&& !UploadWaitGoesOn(false, true, true, 4000, kSendWait),
		u"an uploaded message is awaited until it is sent"_q);

	// Every postponed log out is repeated when the wait ends, not only
	// the last one. Nothing waits here: no application, no session yet.
	{
		auto &wait = SendsWait();
		if (check(
				!PendingVoiceSends
					&& !wait.quit
					&& !wait.handover
					&& wait.logouts.empty(),
				u"nothing is postponed at the start"_q)) {
			auto first = 0;
			auto second = 0;
			auto taken = false;
			wait.logouts.push_back({ .retry = [&] {
				++first;
				taken = SendsWait().logouts.empty();
			} });
			wait.logouts.push_back({ .retry = [&] { ++second; } });
			wait.handover = true;
			FinishSendsWait();
			check(
				(first == 1) && (second == 1),
				u"all postponed log outs are repeated"_q,
				u"%1 and %2 times"_q.arg(first).arg(second));
			check(
				taken && !wait.handover && wait.logouts.empty(),
				u"a repeated log out finds nothing postponed"_q);
			FinishSendsWait();
			check(
				(first == 1) && (second == 1) && !wait.quit,
				u"they are not repeated again"_q);
		}
	}

	// The processing itself on a recorded voice message.
	try {
		const auto sample = SnapshotSample();
		const auto recorded = Audio::Encode(
			sample,
			Audio::Format::OggOpus,
			{ .voice = true });
		const auto duration = sample.duration();
		const auto decodedDuration = [](const QByteArray &bytes) {
			const auto decoded = Audio::Decode(
				bytes,
				{ .rate = kRate, .channels = 1 });
			return decoded ? decoded->duration() : crl::time(0);
		};
		const auto before = decodedDuration(recorded);
		if (check(
				!recorded.isEmpty() && (before > 0),
				u"a test voice message is encoded"_q,
				u"%1 bytes, %2 ms"_q.arg(recorded.size()).arg(before))) {
			const auto denoised = ProcessVoice(
				recorded,
				{},
				duration,
				Processing{ .denoise = true },
				0,
				false);
			if (check(
					denoised && !denoised->bytes.isEmpty(),
					u"noise suppression alone processes it"_q)) {
				const auto after = decodedDuration(denoised->bytes);
				check(
					(denoised->duration == duration)
						&& (std::abs(after - before) <= 60),
					u"the preview keeps the duration"_q,
					u"%1 -> %2 ms"_q.arg(before).arg(after));
				check(
					(denoised->bytes != recorded)
						&& !denoised->waveform.isEmpty(),
					u"the result is new audio with its waveform"_q,
					u"%1 values"_q.arg(denoised->waveform.size()));
			}

			const auto both = ProcessVoice(
				recorded,
				{},
				duration,
				Processing{
					.effect = Effect{ .type = EffectType::Echo },
					.denoise = true,
				},
				duration / 2,
				true);
			if (check(
					both && !both->bytes.isEmpty(),
					u"an effect after a processed start works"_q)) {
				const auto after = decodedDuration(both->bytes);
				check(
					(both->duration >= duration)
						&& (both->duration <= duration + kMaxTail)
						&& (after + 60 >= before)
						&& (after <= before + kMaxTail + 60),
					u"the sent message may only get a short tail"_q,
					u"%1 -> %2 ms"_q.arg(before).arg(after));
			}

			check(
				!ProcessVoice(
					QByteArray("not a voice message"),
					{},
					duration,
					Processing{ .denoise = true },
					0,
					true),
				u"broken data is refused"_q);
		}
	} catch (const std::exception &e) {
		check(false, u"exception"_q, QString::fromUtf8(e.what()));
	}

	log.push_back(u"voice: %1 passed, %2 failed"_q.arg(passed).arg(failed));
	return !failed;
}

} // namespace Oblivion
