/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flat_map.h"
#include "oblivion/oblivion_room.h"

// Round 5: what a room has besides its tabs: reactions, stickers and the
// voice chat.
//
//  - Reactions: the smile button of the header opens a small panel with
//    the usual emoji (and the whole emoji picker of Telegram behind «Все
//    эмодзи»). A reaction is one volatile event: the emoji floats up over
//    the room for everybody who is there now. Nothing is stored.
//  - Stickers and custom emoji: picked in the sticker panel of Telegram
//    and "thrown" into the room, shown big for a moment for everybody.
//    Only a reference travels through the cloud, never the file: a custom
//    emoji is its document id (the others get it from Telegram with
//    messages.getCustomEmojiDocuments), a sticker is its set and its
//    document id (messages.getStickerSet), the emoji of the sticker is
//    what is shown while it loads or when it can't be found.
//  - Voice: there is no voice server. The owner attaches a Telegram group
//    with a video chat: «Создать голосовой чат» creates a private group
//    after a box that says exactly what will be created, «Привязать
//    группу» takes an existing one. The room keeps only an invite link,
//    the microphone button of the header opens it: joining the group and
//    the video chat are the standard dialogs of Telegram.
//    What the room says about its voice chat comes from another member
//    (and through a server), so nothing of it is trusted: the button is
//    there only for an invite link or the public link of a group
//    (ParseVoiceLink, any other t.me link is not a voice chat), where
//    the link leads is asked from Telegram by a click, a channel or a
//    bot behind it is refused, and the chat id the room carries is never
//    used.
//
// Everything registers itself in the room window (an overlay, two header
// buttons, menu items), nothing here is called from outside.
namespace Oblivion::Rooms {

// A token bucket: burst actions at once, then perSecond.
class ReactionLimiter final {
public:
	ReactionLimiter(double burst, double perSecond);

	[[nodiscard]] bool take(crl::time now);

private:
	double _burst = 1.;
	double _rate = 1.;
	double _tokens = 1.;
	crl::time _last = 0;
	bool _started = false;

};

// What is shown of what the others send: a member who floods (or a broken
// client) can't fill the window, the others are still seen.
class IncomingGuard final {
public:
	IncomingGuard(
		double userBurst,
		double userPerSecond,
		double allBurst,
		double allPerSecond);

	[[nodiscard]] bool accept(uint64 userId, crl::time now);

private:
	const double _userBurst;
	const double _userRate;
	base::flat_map<uint64, ReactionLimiter> _users;
	ReactionLimiter _all;

};

// A sticker or a custom emoji thrown into a room: the reference another
// client needs to get the document from Telegram.
struct RoomSticker {
	bool customEmoji = false;
	uint64 documentId = 0;
	uint64 setId = 0;
	uint64 setAccessHash = 0;
	QString setShortName; // [A-Za-z0-9_], up to 64.
	QString emoji; // What is shown instead.
	QString format = u"static"_q; // "static" | "animated" | "video".

	friend inline bool operator==(
		const RoomSticker &,
		const RoomSticker &) = default;
};

// ---- Pure logic (OBLIVION_SELFTEST=room_extras).

// "emoji" of a reaction or of a sticker: untrusted. A short text without
// letters, spaces and markup, or empty. Whether it is an emoji the app
// knows is checked when it is shown.
[[nodiscard]] QString CleanReactionEmoji(const QJsonValue &value);

[[nodiscard]] std::optional<RoomSticker> ParseRoomSticker(
	const QJsonObject &sticker);
[[nodiscard]] QJsonObject SerializeRoomSticker(const RoomSticker &sticker);

// The link of the voice chat of a room: untrusted. Only these forms are
// a voice chat, everything else t.me has (bots with a start parameter,
// mini apps, proxies, share links, sticker sets, posts) is not:
//   https://t.me/+HASH and https://t.me/joinchat/HASH: an invite link;
//   https://t.me/name: the public link of a group;
// each of them may end with "?videochat" or "?voicechat" (with "=HASH"),
// the mark of a video chat link, and with nothing else.
struct VoiceLink {
	QString inviteHash;
	QString username;

	[[nodiscard]] bool valid() const {
		return !inviteHash.isEmpty() || !username.isEmpty();
	}
};
[[nodiscard]] VoiceLink ParseVoiceLink(const QString &link);

// The hash of "https://t.me/+HASH" and "https://t.me/joinchat/HASH",
// empty for anything else (a public link, a phone number).
[[nodiscard]] QString VoiceInviteHash(const QString &link);

[[nodiscard]] bool RunExtrasSelfTest(QStringList &log);

} // namespace Oblivion::Rooms
