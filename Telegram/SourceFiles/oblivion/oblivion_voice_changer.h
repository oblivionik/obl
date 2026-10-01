/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Api {
struct SendAction;
} // namespace Api

namespace Main {
class Account;
class Session;
} // namespace Main

namespace Media::Capture {
struct Result;
} // namespace Media::Capture

namespace Ui {
class RpWidget;
class Show;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Oblivion {

// The chosen effect id is stored in Settings::voiceEffect(),
// an empty string means no effect. Ids: "chipmunk", "giant", "robot",
// "echo", "telephone", "cave" and "pitch:N" (N semitones, -12..12).

// Localized effect name for labels, "None" for an empty id.
[[nodiscard]] QString VoiceEffectName(const QString &id);

// True if an effect is chosen for outgoing voice messages.
[[nodiscard]] bool VoiceEffectEnabled();

// Lets the user choose the effect for voice messages and switch the
// noise suppression, with a test recording played through both.
void ShowVoiceEffectBox(not_null<Window::SessionController*> controller);
void ShowVoiceEffectBox(std::shared_ptr<Ui::Show> show);

// A recorded voice message, as HistoryView::Controls::VoiceToSend
// and Media::Capture::Result carry it.
struct VoiceClip {
	QByteArray bytes; // OGG Opus.
	QVector<signed char> waveform; // Same type as ::VoiceWaveform.
	crl::time duration = 0;
};

// Applies the chosen effect on a background thread and calls done on
// the main thread. If no effect is chosen, the clip is too long for it
// (more than 15 minutes) or processing fails, done receives the clip
// unchanged (possibly synchronously).
void ApplyVoiceEffect(VoiceClip clip, Fn<void(VoiceClip)> done);

// Hooks for the voice recording flow.
//
// Voice messages sent right after recording go to ApiWrap as recorded
// and InterceptVoiceSend() applies the effect there, so the target chat
// is fixed before the (short) processing starts. The recording preview
// ("listen" state of the record bar) gets the effect before it is shown
// by WrapVoicePreviewCapture(), so the user hears the processed voice.
// Byte arrays that already carry the effect are remembered by content
// (the hooks below keep that through trimming and joining), so they are
// never processed twice.
//
// The noise suppression (Settings::voiceNoiseSuppression(), see
// oblivion_noise.h) goes through the same hooks: it cleans the voice
// before the effect and also works alone, when no effect is chosen, so
// "the effect" below means whatever of the two is on.

using VoiceCaptureCallback = Fn<void(Media::Capture::Result&&)>;

// Wraps a Media::Capture pause callback of the record bar: the recorded
// voice gets the effect (keeping its length) before callback receives
// it, a voice too long for the effect is passed with a toast. Returns
// callback itself when no effect is chosen. Nothing is called after
// alive is destroyed (recording finished or cancelled).
[[nodiscard]] VoiceCaptureCallback WrapVoicePreviewCapture(
	VoiceCaptureCallback callback,
	rpl::lifetime &alive,
	std::shared_ptr<Ui::Show> show);

// True if WrapVoicePreviewCapture() would process the recording now:
// its callback is called with a delay then, so the record bar asks for
// one preview at a time.
[[nodiscard]] bool VoicePreviewProcessed();

// The record bar joined a resumed recording to the previous (prefix)
// part, keeps track of which part already has the effect.
void VoiceRecordMerged(
	const QByteArray &prefix,
	crl::time prefixDuration,
	const QByteArray &combined);

// The record bar preview trimmed was to now, starting at from (ms).
void VoiceRecordTrimmed(
	const QByteArray &was,
	const QByteArray &now,
	crl::time from);

// Right click on the record (microphone) button shows the effect menu
// (with the noise suppression toggle) while enabled() is true, a small
// badge on the button shows that an effect is on. Everything is removed
// together with owner.
void SetupVoiceEffectButton(
	not_null<Ui::RpWidget*> owner,
	not_null<Ui::RpWidget*> button,
	std::shared_ptr<Ui::Show> show,
	Fn<bool()> enabled);

// ApiWrap::sendVoiceMessage hook. Returns true if the voice message is
// taken: ApiWrap::sendAction() runs for it right away (the chat clears
// the reply and so on, as for any voice message) and it is sent again
// later with SendOptions::oblivionAnnounced, so that is not repeated.
// Voice messages get the effect this way (or go unchanged with a toast
// if processing fails or they are too long for it). While some are
// processed, all other voice and round video messages are taken too
// and sent unchanged after them, so the order is kept. Round videos
// never get the effect.
[[nodiscard]] bool InterceptVoiceSend(
	not_null<Main::Session*> session,
	const QByteArray &bytes,
	const QVector<signed char> &waveform,
	crl::time duration,
	bool video,
	const Api::SendAction &action);

// Core::Application hooks. A voice message taken by InterceptVoiceSend()
// exists only here until it is processed and handed to the uploader, so
// quitting and logging out wait for that (a toast tells about it). True
// means the action is postponed: Core::Quit() or retry is called when
// the message is uploaded, or when its upload takes long enough to ask
// about it the usual way. A repeated quit request offers to quit at once
// without the message or not to quit at all. Log outs of several
// accounts are all kept, one retry for an account.
[[nodiscard]] bool VoiceSendsPreventQuit();
[[nodiscard]] bool VoiceSendsDelayLogout(
	not_null<Main::Account*> account,
	Fn<void()> retry);

// Self-checks for OBLIVION_SELFTEST=voice, see oblivion_selftest.h.
// Runs before Core::Application exists (no Core::App(), no session).
[[nodiscard]] bool RunVoiceChangerSelfTest(QStringList &log);

} // namespace Oblivion
