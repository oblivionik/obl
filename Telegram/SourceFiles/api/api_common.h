/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_drafts.h"

class History;

namespace Data {
class Thread;
} // namespace Data

namespace Api {

inline constexpr auto kScheduledUntilOnlineTimestamp = TimeId(0x7FFFFFFE);

[[nodiscard]] MTPSuggestedPost SuggestToMTP(SuggestOptions suggest);

struct SendOptions {
	uint64 price = 0;
	PeerData *sendAs = nullptr;
	TimeId scheduled = 0;
	TimeId scheduleRepeatPeriod = 0;
	BusinessShortcutId shortcutId = 0;
	EffectId effectId = 0;
	QByteArray stakeSeedHash;
	int64 stakeNanoTon = 0;
	int starsApproved = 0;
	bool silent = false;
	bool handleSupportSwitch = false;
	bool invertCaption = false;
	bool hideViaBot = false;
	bool mediaSpoiler = false;

	// Oblivion: "scheduled" is set only to send without going online,
	// for the user it is a normal send (see oblivion_sending.h).
	bool oblivionOffline = false;

	// Oblivion: ApiWrap::sendAction() already ran for this send when it
	// was made, it is not repeated (voice messages sent after the voice
	// effect is applied, see oblivion_voice_changer.h).
	bool oblivionAnnounced = false;

	crl::time ttlSeconds = 0;
	SuggestOptions suggest;

	friend inline bool operator==(
		const SendOptions &,
		const SendOptions &) = default;
};
[[nodiscard]] SendOptions DefaultSendWhenOnlineOptions();

enum class SendType {
	Normal,
	Scheduled,
	ScheduledToUser, // For "Send when online".
};

struct SendAction {
	explicit SendAction(
		not_null<Data::Thread*> thread,
		SendOptions options = SendOptions());

	not_null<History*> history;
	SendOptions options;
	FullReplyTo replyTo;
	bool clearDraft = true;
	bool generateLocal = true;
	// Oblivion: false keeps the pending forward draft of the thread
	// untouched instead of sending it together with this message.
	bool sendForwardDraft = true;
	MsgId replaceMediaOf = 0;

	[[nodiscard]] MTPInputReplyTo mtpReplyTo() const;

	friend inline bool operator==(
		const SendAction &,
		const SendAction &) = default;
};

struct MessageToSend {
	explicit MessageToSend(SendAction action) : action(action) {
	}

	SendAction action;
	TextWithTags textWithTags;
	Data::WebPageDraft webPage;
};

struct RemoteFileInfo {
	MTPInputFile file;
	std::optional<MTPInputFile> thumb;
	std::optional<MTPInputPhoto> videoCover;
	std::vector<MTPInputDocument> attachedStickers;
	bool forceFile = false;

};

} // namespace Api
