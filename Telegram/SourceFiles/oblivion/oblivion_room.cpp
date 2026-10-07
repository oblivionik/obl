/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_room.h"

#include "base/random.h"
#include "base/timer.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "oblivion/oblivion_audio.h"
#include "oblivion/oblivion_room_extras.h"
#include "oblivion/oblivion_room_music.h"
#include "ui/chat/attach/attach_prepare.h"
#include "settings.h"

#include <QtCore/QBuffer>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>

namespace Oblivion::Rooms {
namespace {

constexpr auto kTitleLimit = 64;
constexpr auto kNameLimit = 64;
constexpr auto kTrackTextLimit = 128; // limits.track_text of the server.
constexpr auto kChatTextLimit = 2000;
constexpr auto kChatKeep = 500;
constexpr auto kChatLoad = 50;
constexpr auto kIdLimit = 64;
constexpr auto kMembersLimit = 64;
constexpr auto kQueueLimit = 200;
constexpr auto kListLimit = 1000;
constexpr auto kMaxDuration = int64(86'400'000);
constexpr auto kStatusGap = crl::time(1000);
constexpr auto kReloadRetry = crl::time(4000);
constexpr auto kReloadRetryMax = crl::time(120'000);
constexpr auto kReloadAttempts = 6;
constexpr auto kCoverSide = 320;
constexpr auto kCoverBytesLimit = 500 * 1024;
constexpr auto kAudioBytesDefault = int64(83'886'080);
constexpr auto kVideoBytesDefault = int64(734'003'200);
constexpr auto kFolderTtl = int64(3600);
constexpr auto kOrphanDelay = crl::time(30'000);
constexpr auto kQueueRateRetries = 3;

// The queue of a room has a bucket of its own on the server (150 changes
// at once, then three in a second for a device). "Too often" is not an
// error to show: the request was not done, it waits as long as the server
// asks and goes again, see Cloud::Request::rateRetries.
[[nodiscard]] Cloud::Request QueueRequest(Cloud::Request request) {
	request.rateRetries = kQueueRateRetries;
	return request;
}

[[nodiscard]] bool ValidItemId(const QString &id) {
	if (id.isEmpty() || id.size() > kIdLimit) {
		return false;
	}
	for (const auto ch : id) {
		const auto c = ch.unicode();
		const auto good = (c >= 'a' && c <= 'z')
			|| (c >= 'A' && c <= 'Z')
			|| (c >= '0' && c <= '9')
			|| (c == '_')
			|| (c == '-');
		if (!good) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] QString ItemIdFrom(const QJsonValue &value) {
	const auto id = Cloud::JsonText(value, kIdLimit + 1);
	return ValidItemId(id) ? id : QString();
}

[[nodiscard]] Kind KindFrom(const QJsonValue &value) {
	return (value.toString() == u"video"_q) ? Kind::Video : Kind::Music;
}

[[nodiscard]] Repeat RepeatFrom(const QJsonValue &value) {
	const auto text = value.toString();
	return (text == u"all"_q)
		? Repeat::All
		: (text == u"one"_q)
		? Repeat::One
		: Repeat::Off;
}

[[nodiscard]] QString RepeatName(Repeat repeat) {
	switch (repeat) {
	case Repeat::All: return u"all"_q;
	case Repeat::One: return u"one"_q;
	case Repeat::Off: break;
	}
	return u"off"_q;
}

[[nodiscard]] MemberStatus ParseStatus(const QJsonValue &value) {
	if (!value.isObject()) {
		return {};
	}
	const auto object = value.toObject();
	return {
		.itemId = ItemIdFrom(object.value(u"item_id"_q)),
		.ready = object.value(u"ready"_q).toBool(),
		.buffered = int(std::clamp(
			Cloud::JsonInt(object.value(u"buffered"_q)),
			int64(0),
			int64(100))),
	};
}

[[nodiscard]] Voice ParseVoice(const QJsonValue &value) {
	if (!value.isObject()) {
		return {};
	}
	const auto object = value.toObject();
	const auto link = Cloud::JsonText(object.value(u"link"_q), 256);
	if (!ParseVoiceLink(link).valid()) {
		// Only an invite link or the public link of a group is a voice
		// chat, see oblivion_room_extras.h. "tg_chat_id" of the room is
		// not read at all: a chat is never picked by what a room says.
		return {};
	}
	return {
		.link = link,
		.title = Cloud::JsonText(object.value(u"title"_q), kTitleLimit),
		.setBy = Cloud::JsonUserId(object.value(u"set_by"_q)),
		.setAt = Cloud::JsonInt(object.value(u"set_at"_q)),
	};
}

[[nodiscard]] RoomSettings ParseSettings(const QJsonObject &object) {
	return {
		.maxMembers = int(std::clamp(
			Cloud::JsonInt(object.value(u"max_members"_q), 30),
			int64(2),
			int64(kMembersLimit))),
		.inviteOnly = (object.value(u"join"_q).toString() == u"invite"_q),
		.joinFromActivity = object.value(u"join_from_activity"_q).toBool(),
		.defaults = ParseRights(
			object.value(u"default_rights"_q).toObject()),
	};
}

[[nodiscard]] std::vector<QueueItem> ParseQueue(const QJsonValue &value) {
	auto result = std::vector<QueueItem>();
	const auto list = value.toArray();
	result.reserve(std::min(int(list.size()), kQueueLimit));
	for (const auto &entry : list) {
		auto item = ParseQueueItem(entry.toObject());
		const auto known = [&] {
			return ranges::contains(result, item.id, &QueueItem::id);
		};
		if (!item.id.isEmpty() && !known()) {
			result.push_back(std::move(item));
			if (int(result.size()) >= kQueueLimit) {
				break;
			}
		}
	}
	return result;
}

[[nodiscard]] Player ParsePlayer(const QJsonObject &object) {
	auto result = Player{
		.queue = ParseQueue(object.value(u"queue"_q)),
		.state = ParsePlayerState(object.value(u"state"_q).toObject()),
	};
	if (!result.state.itemId.isEmpty() && !result.current()) {
		result.state.itemId = QString();
		result.state.playing = false;
	}
	return result;
}

[[nodiscard]] std::vector<Banned> ParseBanned(const QJsonValue &value) {
	auto result = std::vector<Banned>();
	for (const auto &entry : value.toArray()) {
		const auto object = entry.toObject();
		const auto id = Cloud::JsonUserId(object.value(u"id"_q));
		if (id && int(result.size()) < kListLimit) {
			result.push_back({
				.id = id,
				.name = Cloud::JsonText(object.value(u"name"_q), kNameLimit),
			});
		}
	}
	return result;
}

[[nodiscard]] std::vector<uint64> ParseIds(const QJsonValue &value) {
	auto result = std::vector<uint64>();
	for (const auto &entry : value.toArray()) {
		const auto id = Cloud::JsonUserId(entry);
		if (id && int(result.size()) < kListLimit) {
			result.push_back(id);
		}
	}
	return result;
}

[[nodiscard]] Gone GoneFrom(const QString &reason, Gone fallback) {
	return (reason == u"kicked"_q)
		? Gone::Kicked
		: (reason == u"banned"_q)
		? Gone::Banned
		: (reason == u"closed"_q || reason == u"owner"_q)
		? Gone::Closed
		: (reason == u"empty"_q)
		? Gone::Closed
		: (reason == u"idle"_q)
		? Gone::Idle
		: (reason == u"admin"_q)
		? Gone::Admin
		: (reason == u"left"_q || reason == u"deleted"_q)
		? Gone::Left
		: fallback;
}

// The owner flag of every member and the rights of this user follow the
// owner of the room.
[[nodiscard]] Changes ApplyOwner(
		RoomState &state,
		uint64 ownerId,
		uint64 selfId) {
	if (!ownerId || state.ownerId == ownerId) {
		return {};
	}
	auto result = Changes(Change::Room) | Change::Members;
	state.ownerId = ownerId;
	for (auto &member : state.members) {
		member.owner = (member.id == ownerId);
		if (member.owner) {
			member.rights = Rights::Everything();
		}
	}
	const auto owner = (ownerId == selfId);
	if (state.owner != owner) {
		state.owner = owner;
		result |= Change::Rights;
	}
	if (owner && state.rights != Rights::Everything()) {
		state.rights = Rights::Everything();
		result |= Change::Rights;
	} else if (!owner) {
		if (const auto self = state.member(selfId)) {
			if (state.rights != self->rights) {
				state.rights = self->rights;
				result |= Change::Rights;
			}
		}
	}
	return result;
}

[[nodiscard]] Changes UpsertMember(
		RoomState &state,
		Member &&member,
		uint64 selfId) {
	if (!member.id) {
		return {};
	}
	auto result = Changes(Change::Members);
	if (member.owner) {
		member.rights = Rights::Everything();
	}
	if (member.id == selfId) {
		if (state.rights != member.rights || state.owner != member.owner) {
			state.rights = member.rights;
			state.owner = member.owner;
			result |= Change::Rights;
		}
	}
	const auto i = ranges::find(state.members, member.id, &Member::id);
	if (i != end(state.members)) {
		if (i->online != member.online) {
			result |= Change::Presence;
		}
		*i = std::move(member);
	} else if (int(state.members.size()) < kMembersLimit) {
		state.members.push_back(std::move(member));
		result |= Change::Presence;
	}
	return result;
}

[[nodiscard]] bool AddChatMessage(RoomState &state, ChatMessage &&message) {
	if (message.id <= 0) {
		return false;
	}
	auto &list = state.chat;
	const auto i = ranges::lower_bound(
		list,
		message.id,
		ranges::less(),
		&ChatMessage::id);
	if (i != end(list) && i->id == message.id) {
		return false;
	}
	state.chatLastId = std::max(state.chatLastId, message.id);
	list.insert(i, std::move(message));
	if (int(list.size()) > kChatKeep) {
		list.erase(begin(list), begin(list) + (list.size() - kChatKeep));
	}
	return true;
}

// The state of a player from an event or an answer. A queue that came
// with it must be applied before: the current item is looked up there.
[[nodiscard]] bool TakePlayerState(
		Player &player,
		PlayerState &&state,
		bool equalRevToo) {
	if (state.rev < player.state.rev
		|| (!equalRevToo && state.rev == player.state.rev)) {
		return false;
	}
	if (!state.itemId.isEmpty() && !player.find(state.itemId)) {
		// An item this client does not know (a queue event is on its
		// way): nothing plays until the queue comes.
		state.itemId = QString();
		state.playing = false;
	}
	player.state = std::move(state);
	return true;
}

[[nodiscard]] QString RoomsFolder() {
	return cWorkingDir() + u"tdata/oblivion/rooms/"_q;
}

[[nodiscard]] QString RoomFolder(uint64 userId, const QString &code) {
	return RoomsFolder() + RoomFolderName(userId, code) + '/';
}

[[nodiscard]] QString SafeExtension(const QString &fileName) {
	const auto suffix = QFileInfo(fileName).suffix().toLower();
	if (suffix.isEmpty() || suffix.size() > 5) {
		return QString();
	}
	for (const auto ch : suffix) {
		const auto c = ch.unicode();
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
			return QString();
		}
	}
	return '.' + suffix;
}

// The name Room::download() gives to the file of an item in the temp
// folder of the room: by the content, so two items with the same file
// share it.
[[nodiscard]] QString MediaFileName(const QueueItem &item) {
	return item.media + SafeExtension(item.fileName);
}

// path -> sha256 for the files of the items of both queues.
[[nodiscard]] base::flat_map<QString, QString> QueueFiles(
		const RoomState &state,
		const QString &folder) {
	auto result = base::flat_map<QString, QString>();
	for (const auto kind : { Kind::Music, Kind::Video }) {
		for (const auto &item : state.player(kind).queue) {
			result.emplace(folder + MediaFileName(item), item.media);
		}
	}
	return result;
}

// What was in the queues and is in none of them now: path -> sha256.
[[nodiscard]] base::flat_map<QString, QString> LeftFiles(
		const base::flat_map<QString, QString> &was,
		const base::flat_map<QString, QString> &now) {
	auto result = base::flat_map<QString, QString>();
	for (const auto &[path, sha] : was) {
		if (!now.contains(path)) {
			result.emplace(path, sha);
		}
	}
	return result;
}

// A file right in that folder (folder ends with a slash), not the folder
// itself and nothing deeper or outside of it.
[[nodiscard]] bool InFolder(const QString &path, const QString &folder) {
	if (folder.isEmpty()
		|| path.size() <= folder.size()
		|| !path.startsWith(folder)) {
		return false;
	}
	const auto name = path.mid(folder.size());
	return !name.contains(QChar('/'))
		&& !name.contains(QChar('\\'))
		&& (name != u".."_q)
		&& (name != u"."_q);
}

[[nodiscard]] QString MimeFor(const QString &fileName, Kind kind) {
	const auto suffix = QFileInfo(fileName).suffix().toLower();
	static const auto map = base::flat_map<QString, QString>{
		{ u"mp3"_q, u"audio/mpeg"_q },
		{ u"m4a"_q, u"audio/mp4"_q },
		{ u"aac"_q, u"audio/aac"_q },
		{ u"ogg"_q, u"audio/ogg"_q },
		{ u"oga"_q, u"audio/ogg"_q },
		{ u"opus"_q, u"audio/ogg"_q },
		{ u"flac"_q, u"audio/flac"_q },
		{ u"wav"_q, u"audio/wav"_q },
		{ u"mp4"_q, u"video/mp4"_q },
		{ u"m4v"_q, u"video/mp4"_q },
		{ u"mov"_q, u"video/quicktime"_q },
		{ u"webm"_q, u"video/webm"_q },
		{ u"mkv"_q, u"video/x-matroska"_q },
	};
	const auto i = map.find(suffix);
	if (i == end(map)) {
		return u"application/octet-stream"_q;
	}
	// The relay takes only audio/* and video/mp4 as a track of music.
	return (kind == Kind::Music
		&& i->second.startsWith(u"video/"_q)
		&& i->second != u"video/mp4"_q)
		? u"application/octet-stream"_q
		: i->second;
}

// The kinds of content the relay takes for a queue item.
[[nodiscard]] QString RelayMime(const QString &mime, Kind kind) {
	const auto audio = mime.startsWith(u"audio/"_q);
	const auto video = mime.startsWith(u"video/"_q);
	const auto fits = (mime.size() <= 64)
		&& ((kind == Kind::Video)
			? (audio || video)
			: (audio
				|| (mime == u"video/mp4"_q)
				|| (mime == u"application/ogg"_q)));
	return fits ? mime : u"application/octet-stream"_q;
}

[[nodiscard]] QByteArray EncodeCover(QImage image) {
	if (image.isNull()) {
		return QByteArray();
	}
	if (image.width() > kCoverSide || image.height() > kCoverSide) {
		image = image.scaled(
			kCoverSide,
			kCoverSide,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
	}
	if (image.hasAlphaChannel()) {
		auto opaque = QImage(image.size(), QImage::Format_RGB32);
		opaque.fill(Qt::black);
		{
			auto p = QPainter(&opaque);
			p.drawImage(0, 0, image);
		}
		image = std::move(opaque);
	}
	auto result = QByteArray();
	{
		auto buffer = QBuffer(&result);
		if (!buffer.open(QIODevice::WriteOnly)
			|| !image.save(&buffer, "JPG", 86)) {
			return QByteArray();
		}
	}
	return (result.size() <= kCoverBytesLimit) ? result : QByteArray();
}

struct Prepared {
	bool ok = false;
	int64 size = 0;
	QString title;
	QString performer;
	int64 duration = 0;
	QByteArray cover;
};

// Any thread: reads the size, the tags and the duration of the file.
[[nodiscard]] Prepared PrepareMedia(
		Kind kind,
		const QString &path,
		const QByteArray &bytes,
		QString title,
		QString performer,
		int64 duration,
		QImage cover) {
	auto result = Prepared{
		.size = path.isEmpty() ? int64(bytes.size()) : QFileInfo(path).size(),
		.title = std::move(title),
		.performer = std::move(performer),
		.duration = duration,
	};
	if (result.size <= 0) {
		return result;
	}
	if (kind == Kind::Music
		&& (result.duration <= 0 || result.title.isEmpty() || cover.isNull())) {
		const auto info = Media::Player::PrepareForSending(path, bytes);
		using Song = Ui::PreparedFileInformation::Song;
		if (const auto song = std::get_if<Song>(&info.media)) {
			if (result.duration <= 0 && song->duration > 0) {
				result.duration = song->duration;
			}
			if (result.title.isEmpty()) {
				result.title = song->title.trimmed();
				if (result.performer.isEmpty()) {
					result.performer = song->performer.trimmed();
				}
			}
			if (cover.isNull()) {
				cover = song->cover;
			}
		}
	}
	if (result.duration <= 0 && !path.isEmpty()) {
		if (const auto info = Audio::Probe(path)) {
			result.duration = info->duration;
		}
	}
	result.cover = EncodeCover(std::move(cover));
	result.ok = (result.duration > 0) && (result.duration <= kMaxDuration);
	return result;
}

[[nodiscard]] QJsonObject SerializeItem(const ItemInput &item) {
	auto result = QJsonObject();
	result.insert(u"media"_q, item.media);
	result.insert(u"title"_q, item.title.left(kTrackTextLimit));
	result.insert(u"performer"_q, item.performer.left(kTrackTextLimit));
	result.insert(u"duration_ms"_q, double(item.duration));
	if (Cloud::ValidMediaId(item.cover)) {
		result.insert(u"cover"_q, item.cover);
	} else {
		result.insert(u"cover"_q, QJsonValue());
	}
	result.insert(u"file_name"_q, item.fileName.left(kTrackTextLimit));
	return result;
}

} // namespace

QString KindName(Kind kind) {
	return (kind == Kind::Video) ? u"video"_q : u"music"_q;
}

const char *RightName(Right right) {
	switch (right) {
	case Right::Control: return "control";
	case Right::Queue: return "queue";
	case Right::Add: return "add";
	case Right::Draw: return "draw";
	case Right::Chat: return "chat";
	case Right::Invite: return "invite";
	}
	return "";
}

bool Rights::has(Right right) const {
	switch (right) {
	case Right::Control: return control;
	case Right::Queue: return queue;
	case Right::Add: return add;
	case Right::Draw: return draw;
	case Right::Chat: return chat;
	case Right::Invite: return invite;
	}
	return false;
}

void Rights::set(Right right, bool value) {
	switch (right) {
	case Right::Control: control = value; break;
	case Right::Queue: queue = value; break;
	case Right::Add: add = value; break;
	case Right::Draw: draw = value; break;
	case Right::Chat: chat = value; break;
	case Right::Invite: invite = value; break;
	}
}

Rights Rights::Everything() {
	return {
		.control = true,
		.queue = true,
		.add = true,
		.draw = true,
		.chat = true,
		.invite = true,
	};
}

Rights Rights::OnlyOwner() {
	return { .draw = true, .chat = true };
}

Rights ParseRights(const QJsonObject &object) {
	auto result = Rights();
	for (auto i = 0; i != kRightsCount; ++i) {
		const auto right = Right(i);
		result.set(
			right,
			object.value(QLatin1String(RightName(right))).toBool());
	}
	return result;
}

QJsonObject SerializeRights(const Rights &rights) {
	auto result = QJsonObject();
	for (auto i = 0; i != kRightsCount; ++i) {
		const auto right = Right(i);
		result.insert(QLatin1String(RightName(right)), rights.has(right));
	}
	return result;
}

QJsonObject SerializeRight(Right right, bool value) {
	auto result = QJsonObject();
	result.insert(QLatin1String(RightName(right)), value);
	return result;
}

crl::time ReloadRetryDelay(int attempt) {
	if (attempt < 0 || attempt >= kReloadAttempts) {
		return 0;
	}
	return std::min(kReloadRetry << attempt, kReloadRetryMax);
}

const QueueItem *Player::find(const QString &id) const {
	const auto i = ranges::find(queue, id, &QueueItem::id);
	return (id.isEmpty() || i == end(queue)) ? nullptr : &*i;
}

const QueueItem *Player::current() const {
	return find(state.itemId);
}

int Player::indexOf(const QString &id) const {
	const auto i = ranges::find(queue, id, &QueueItem::id);
	return (id.isEmpty() || i == end(queue)) ? -1 : int(i - begin(queue));
}

const Member *RoomState::member(uint64 id) const {
	const auto i = ranges::find(members, id, &Member::id);
	return (i != end(members)) ? &*i : nullptr;
}

int RoomState::onlineCount() const {
	return int(ranges::count(members, true, &Member::online));
}

Changes QueueChange(Kind kind) {
	return (kind == Kind::Video) ? Change::VideoQueue : Change::MusicQueue;
}

Changes PlayerChange(Kind kind) {
	return (kind == Kind::Video) ? Change::VideoPlayer : Change::MusicPlayer;
}

Member ParseMember(const QJsonObject &member) {
	const auto status = member.value(u"status"_q).toObject();
	auto result = Member{
		.id = Cloud::JsonUserId(member.value(u"id"_q)),
		.name = Cloud::JsonText(member.value(u"name"_q), kNameLimit),
		.avatarRev = int(std::clamp(
			Cloud::JsonInt(member.value(u"avatar_rev"_q)),
			int64(0),
			int64(1'000'000'000))),
		.owner = (member.value(u"role"_q).toString() == u"owner"_q),
		.rights = ParseRights(member.value(u"rights"_q).toObject()),
		.joinedAt = Cloud::JsonInt(member.value(u"joined_at"_q)),
		.online = member.value(u"online"_q).toBool(),
		.music = ParseStatus(status.value(u"music"_q)),
		.video = ParseStatus(status.value(u"video"_q)),
	};
	if (result.owner) {
		result.rights = Rights::Everything();
	}
	return result;
}

QueueItem ParseQueueItem(const QJsonObject &item) {
	auto result = QueueItem{
		.id = ItemIdFrom(item.value(u"id"_q)),
		.kind = KindFrom(item.value(u"kind"_q)),
		.media = Cloud::JsonText(item.value(u"media"_q), 65),
		.size = std::max(Cloud::JsonInt(item.value(u"size"_q)), int64(0)),
		.mime = Cloud::JsonText(item.value(u"mime"_q), 64),
		.title = Cloud::JsonText(item.value(u"title"_q), kTrackTextLimit),
		.performer = Cloud::JsonText(
			item.value(u"performer"_q),
			kTrackTextLimit),
		.duration = Cloud::JsonInt(item.value(u"duration_ms"_q)),
		.cover = Cloud::JsonText(item.value(u"cover"_q), 65),
		.fileName = Cloud::JsonText(
			item.value(u"file_name"_q),
			kTrackTextLimit),
		.addedBy = Cloud::JsonUserId(item.value(u"added_by"_q)),
		.addedAt = Cloud::JsonInt(item.value(u"added_at"_q)),
	};
	if (!Cloud::ValidMediaId(result.media)
		|| result.duration <= 0
		|| result.duration > kMaxDuration) {
		result.id = QString();
	}
	if (!Cloud::ValidMediaId(result.cover)) {
		result.cover = QString();
	}
	return result;
}

PlayerState ParsePlayerState(const QJsonObject &state) {
	return {
		.itemId = ItemIdFrom(state.value(u"item_id"_q)),
		.playing = state.value(u"playing"_q).toBool(),
		.position = std::clamp(
			Cloud::JsonInt(state.value(u"position_ms"_q)),
			int64(0),
			kMaxDuration),
		.anchor = std::max(
			Cloud::JsonInt(state.value(u"anchor_ms"_q)),
			int64(0)),
		.repeat = RepeatFrom(state.value(u"repeat"_q)),
		.rev = std::max(Cloud::JsonInt(state.value(u"rev"_q)), int64(0)),
		.updatedBy = Cloud::JsonUserId(state.value(u"updated_by"_q)),
		.updatedAt = Cloud::JsonInt(state.value(u"updated_at"_q)),
	};
}

ChatMessage ParseChatMessage(const QJsonObject &message) {
	return {
		.id = Cloud::JsonInt(message.value(u"id"_q)),
		.userId = Cloud::JsonUserId(message.value(u"user_id"_q)),
		.name = Cloud::JsonText(message.value(u"name"_q), kNameLimit),
		.text = Cloud::JsonText(
			message.value(u"text"_q),
			kChatTextLimit,
			false).trimmed(),
		.ts = Cloud::JsonInt(message.value(u"ts"_q)),
		.replyTo = Cloud::JsonInt(message.value(u"reply_to"_q)),
		.clientId = Cloud::JsonText(message.value(u"client_id"_q), 32),
	};
}

RoomState ParseRoom(const QJsonObject &room, uint64 selfId) {
	auto result = RoomState();
	result.code = Cloud::NormalizeRoomCode(
		Cloud::JsonText(room.value(u"code"_q), 16));
	if (result.code.isEmpty()) {
		return result;
	}
	result.link = Cloud::MakeLink(Cloud::LinkKind::Room, result.code);
	result.title = Cloud::JsonText(room.value(u"title"_q), kTitleLimit);
	result.ownerId = Cloud::JsonUserId(room.value(u"owner_id"_q));
	result.createdAt = Cloud::JsonInt(room.value(u"created_at"_q));
	result.rev = Cloud::JsonInt(room.value(u"rev"_q));
	result.settings = ParseSettings(room.value(u"settings"_q).toObject());
	for (const auto &entry : room.value(u"members"_q).toArray()) {
		auto member = ParseMember(entry.toObject());
		if (member.id
			&& !result.member(member.id)
			&& int(result.members.size()) < kMembersLimit) {
			member.owner = (member.id == result.ownerId);
			if (member.owner) {
				member.rights = Rights::Everything();
			}
			result.members.push_back(std::move(member));
		}
	}
	const auto me = room.value(u"me"_q).toObject();
	result.owner = (result.ownerId == selfId)
		|| (me.value(u"role"_q).toString() == u"owner"_q);
	result.rights = result.owner
		? Rights::Everything()
		: ParseRights(me.value(u"rights"_q).toObject());
	result.invited = ParseIds(room.value(u"invited"_q));
	result.banned = ParseBanned(room.value(u"banned"_q));
	result.music = ParsePlayer(room.value(u"music"_q).toObject());
	result.video = ParsePlayer(room.value(u"video"_q).toObject());
	result.voice = ParseVoice(room.value(u"voice"_q));
	const auto canvas = room.value(u"canvas"_q).toObject();
	result.canvas = {
		.width = int(std::clamp(
			Cloud::JsonInt(canvas.value(u"width"_q), 1920),
			int64(16),
			int64(8192))),
		.height = int(std::clamp(
			Cloud::JsonInt(canvas.value(u"height"_q), 1080),
			int64(16),
			int64(8192))),
		.background = Cloud::JsonText(canvas.value(u"background"_q), 7),
		.count = int(std::max(
			Cloud::JsonInt(canvas.value(u"count"_q)),
			int64(0))),
		.seq = Cloud::JsonInt(canvas.value(u"seq"_q)),
	};
	result.chatLastId = Cloud::JsonInt(
		room.value(u"chat"_q).toObject().value(u"last_id"_q));
	result.eventId = Cloud::JsonInt(room.value(u"event_id"_q));
	return result;
}

Changes ApplyEvent(
		RoomState &state,
		const Cloud::Event &event,
		uint64 selfId) {
	if (!state.valid() || state.gone != Gone::No) {
		return {};
	}
	const auto &type = event.type;
	const auto &data = event.data;
	if (type == u"room.rejected"_q) {
		state.gone = Gone::Rejected;
		return Change::Gone;
	} else if (type == u"me.room_removed"_q) {
		state.gone = GoneFrom(
			data.value(u"reason"_q).toString(),
			Gone::Rejected);
		return Change::Gone;
	} else if (type == u"room.closed"_q) {
		state.gone = GoneFrom(
			data.value(u"reason"_q).toString(),
			Gone::Closed);
		return Change::Gone;
	} else if (type == u"room.updated"_q) {
		// The answer of a later request of this user may be here already
		// (two quick clicks on the rights of newcomers): an older state
		// of the room does not come back for a moment.
		const auto rev = Cloud::JsonInt(data.value(u"rev"_q));
		if (rev > 0 && rev < state.rev) {
			return {};
		}
		auto result = Changes(Change::Room);
		if (data.contains(u"title"_q)) {
			state.title = Cloud::JsonText(data.value(u"title"_q), kTitleLimit);
		}
		if (data.value(u"settings"_q).isObject()) {
			state.settings = ParseSettings(
				data.value(u"settings"_q).toObject());
		}
		state.rev = std::max(
			state.rev,
			Cloud::JsonInt(data.value(u"rev"_q)));
		result |= ApplyOwner(
			state,
			Cloud::JsonUserId(data.value(u"owner_id"_q)),
			selfId);
		return result;
	} else if (type == u"room.member_joined"_q
		|| type == u"room.member_updated"_q) {
		auto member = ParseMember(data.value(u"member"_q).toObject());
		if (member.owner && member.id != state.ownerId) {
			auto result = ApplyOwner(state, member.id, selfId);
			return result | UpsertMember(state, std::move(member), selfId);
		}
		member.owner = (member.id == state.ownerId);
		return UpsertMember(state, std::move(member), selfId);
	} else if (type == u"room.member_left"_q) {
		const auto id = Cloud::JsonUserId(data.value(u"user_id"_q));
		if (id == selfId && id) {
			state.gone = GoneFrom(
				data.value(u"reason"_q).toString(),
				Gone::Left);
			return Change::Gone;
		}
		const auto i = ranges::find(state.members, id, &Member::id);
		if (i == end(state.members)) {
			return {};
		}
		state.members.erase(i);
		return Changes(Change::Members) | Change::Presence;
	} else if (type == u"room.presence"_q) {
		const auto id = Cloud::JsonUserId(data.value(u"user_id"_q));
		const auto online = data.value(u"online"_q).toBool();
		const auto i = ranges::find(state.members, id, &Member::id);
		if (i == end(state.members) || i->online == online) {
			return {};
		}
		i->online = online;
		return Change::Presence;
	} else if (type == u"room.status"_q) {
		const auto id = Cloud::JsonUserId(data.value(u"user_id"_q));
		const auto i = ranges::find(state.members, id, &Member::id);
		if (i == end(state.members)) {
			return {};
		}
		auto music = ParseStatus(data.value(u"music"_q));
		auto video = ParseStatus(data.value(u"video"_q));
		if (i->music == music && i->video == video) {
			return {};
		}
		i->music = std::move(music);
		i->video = std::move(video);
		return Change::Status;
	} else if (type == u"room.queue"_q) {
		// The events come in the order of the server: the queue of the
		// last one is the queue, whatever the rev of its state is (the
		// queue changes without a rev).
		const auto kind = KindFrom(data.value(u"kind"_q));
		auto &player = state.player(kind);
		player.queue = ParseQueue(data.value(u"queue"_q));
		const auto taken = TakePlayerState(
			player,
			ParsePlayerState(data.value(u"state"_q).toObject()),
			true);
		if (!taken && !player.current()) {
			player.state.itemId = QString();
			player.state.playing = false;
		}
		return QueueChange(kind) | PlayerChange(kind);
	} else if (type == u"room.player"_q) {
		const auto kind = KindFrom(data.value(u"kind"_q));
		return TakePlayerState(
			state.player(kind),
			ParsePlayerState(data.value(u"state"_q).toObject()),
			false) ? PlayerChange(kind) : Changes();
	} else if (type == u"room.chat"_q) {
		auto message = ParseChatMessage(data.value(u"message"_q).toObject());
		return AddChatMessage(state, std::move(message))
			? Changes(Change::Chat)
			: Changes();
	} else if (type == u"room.chat_deleted"_q) {
		const auto id = Cloud::JsonInt(data.value(u"id"_q));
		const auto i = ranges::find(state.chat, id, &ChatMessage::id);
		if (i == end(state.chat)) {
			return {};
		}
		state.chat.erase(i);
		return Change::Chat;
	} else if (type == u"room.voice"_q) {
		state.voice = ParseVoice(data.value(u"voice"_q));
		return Change::Voice;
	} else if (type == u"room.stroke"_q) {
		++state.canvas.count;
		state.canvas.seq = std::max(
			state.canvas.seq,
			Cloud::JsonInt(
				data.value(u"stroke"_q).toObject().value(u"seq"_q)));
		return Change::Canvas;
	} else if (type == u"room.stroke_removed"_q) {
		state.canvas.count = std::max(state.canvas.count - 1, 0);
		return Change::Canvas;
	} else if (type == u"room.canvas_cleared"_q) {
		state.canvas.count = 0;
		const auto background = Cloud::JsonText(
			data.value(u"background"_q),
			7);
		if (!background.isEmpty()) {
			state.canvas.background = background;
		}
		return Change::Canvas;
	}
	return {};
}

Changes ApplyPlayerAnswer(
		RoomState &state,
		Kind kind,
		const QJsonObject &answer) {
	if (!state.valid()
		|| state.gone != Gone::No
		|| !answer.value(u"state"_q).isObject()) {
		return {};
	}
	return TakePlayerState(
		state.player(kind),
		ParsePlayerState(answer.value(u"state"_q).toObject()),
		false) ? PlayerChange(kind) : Changes();
}

int64 PositionAt(const PlayerState &state, int64 duration, int64 serverNow) {
	if (state.itemId.isEmpty()) {
		return 0;
	}
	const auto limit = std::max(duration, int64(0));
	const auto clamped = [&](int64 value) {
		return std::clamp(value, int64(0), limit);
	};
	if (!state.playing || serverNow <= state.anchor) {
		return clamped(state.position);
	}
	return clamped(state.position + (serverNow - state.anchor));
}

QString NextAfterEnd(const Player &player) {
	const auto index = player.indexOf(player.state.itemId);
	if (index < 0) {
		return QString();
	} else if (player.state.repeat == Repeat::One) {
		return player.state.itemId;
	} else if (index + 1 < int(player.queue.size())) {
		return player.queue[index + 1].id;
	} else if (player.state.repeat == Repeat::All) {
		return player.queue.front().id;
	}
	return QString();
}

bool CanRemoveItem(
		const RoomState &state,
		const QueueItem &item,
		uint64 selfId) {
	return state.rights.queue
		|| (state.rights.add && selfId && item.addedBy == selfId);
}

std::pair<bool, QString> MoveTarget(
		const std::vector<QueueItem> &queue,
		int from,
		int to) {
	const auto count = int(queue.size());
	if (from < 0 || from >= count || to < 0 || to >= count || from == to) {
		return { false, QString() };
	}
	const auto before = (to < from) ? to : (to + 1);
	return { true, (before < count) ? queue[before].id : QString() };
}

std::pair<int, int> MoveIndexes(
		const Player &player,
		const QString &itemId,
		QueueMove move) {
	const auto none = std::pair(-1, -1);
	const auto count = int(player.queue.size());
	const auto from = player.indexOf(itemId);
	if (from < 0) {
		return none;
	}
	switch (move) {
	case QueueMove::Up:
		return (from > 0) ? std::pair(from, from - 1) : none;
	case QueueMove::Down:
		return (from + 1 < count) ? std::pair(from, from + 1) : none;
	case QueueMove::Next: {
		const auto current = player.indexOf(player.state.itemId);
		if (current < 0 || from == current || from == current + 1) {
			return none;
		}
		// The item leaves its place first: what is above the current
		// one lands on the place of the current one, right after it.
		return std::pair(from, (from < current) ? current : (current + 1));
	}
	}
	return none;
}

QString ExtractCode(const QString &typed) {
	const auto trimmed = typed.trimmed();
	if (trimmed.isEmpty() || trimmed.size() > 256) {
		return QString();
	}
	const auto link = Cloud::ParseLink(trimmed);
	if (link.kind == Cloud::LinkKind::Room) {
		return link.id;
	} else if (link) {
		return QString();
	}
	return Cloud::NormalizeRoomCode(trimmed);
}

QString FormatDuration(int64 milliseconds) {
	const auto seconds = std::max(milliseconds, int64(0)) / 1000;
	const auto hours = seconds / 3600;
	const auto minutes = (seconds % 3600) / 60;
	const auto rest = seconds % 60;
	return hours
		? u"%1:%2:%3"_q
			.arg(hours)
			.arg(minutes, 2, 10, QChar('0'))
			.arg(rest, 2, 10, QChar('0'))
		: u"%1:%2"_q.arg(minutes).arg(rest, 2, 10, QChar('0'));
}

// ---- Room.

struct Room::Private {
	struct Task {
		int id = 0;
		AddMedia media;
		QByteArray cover;
		QString sha;
		QString coverSha;
		Cloud::TransferId transfer = 0;
		Cloud::RequestId request = 0;
	};
	struct StatusOut {
		QString itemId;
		bool ready = false;
		int buffered = 0;

		friend inline bool operator==(
			const StatusOut &,
			const StatusOut &) = default;
	};

	explicit Private(not_null<Room*> owner);

	void handle(const Cloud::Event &event);
	void apply(const Cloud::Event &event);
	void fire(Changes changes);
	void wentGone();
	void reload(bool retry = false);
	void reloadLater();
	void reloadDone(const QJsonObject &room);
	void applyRoomInfo(const QJsonObject &room);
	void refreshRoomInfo();
	void checkMember();
	void loadChat();
	void setConnected(bool value);
	void trackFiles();
	void removeLeftFiles();

	void playerAction(Kind kind, QJsonObject body, bool withRev);
	void pauseOther(Kind kind);

	[[nodiscard]] Task *task(int id);
	void rebuildRows();
	void setProgress(int id, int64 ready, int64 total);
	void prepared(int id, Prepared &&result);
	void startUpload(int id);
	void uploadCover(int id);
	void finishTask(int id);
	void failTask(int id, const Cloud::Error &error);
	void failTask(int id, const QString &text);
	void removeTask(int id, bool cancel);

	void sendStatus();
	[[nodiscard]] QString folder() const;
	[[nodiscard]] QString mediaPath(const QueueItem &item) const;

	const not_null<Room*> owner;
	base::weak_ptr<Main::Session> session;
	base::weak_ptr<Cloud::Account> account;
	std::optional<Cloud::Sender> sender;
	uint64 selfId = 0;
	RoomState state;
	Fn<int64()> now;
	bool sample = false;
	bool connected = true;

	rpl::event_stream<Changes> changes;
	rpl::event_stream<Cloud::Event> events;
	rpl::event_stream<> reloaded;
	rpl::event_stream<Cloud::Error> errors;
	rpl::event_stream<QString> toasts;

	bool reloading = false;
	bool reloadFailed = false;
	int reloadAttempts = 0;
	std::vector<Cloud::Event> queued;
	base::Timer reloadTimer;
	Cloud::RequestId memberCheck = 0;
	bool chatLoaded = false;

	std::vector<std::unique_ptr<Task>> tasks;
	std::vector<Upload> rows;
	int taskIds = 0;

	StatusOut statusWanted[2];
	StatusOut statusSent[2];
	bool statusKnown[2] = { true, true }; // Nothing was told: as sent.
	crl::time statusSentAt = 0;
	base::Timer statusTimer;

	base::flat_map<QString, QImage> covers;
	base::flat_set<QString> coversRequested;
	base::flat_set<QString> coversFailed; // Asked again after a reconnect.
	base::flat_map<QString, QString> known; // sha256 -> an own file.

	// The files of the temp folder: path -> sha256. What has left both
	// queues is deleted a bit later, the folder does not grow for as long
	// as the window stays open.
	base::flat_map<QString, QString> queueFiles;
	base::flat_map<QString, QString> leftFiles;
	base::Timer leftTimer;

	std::unique_ptr<MusicEngine> music;
	rpl::lifetime subscription;
	rpl::lifetime lifetime;
	rpl::lifetime external;

};

Room::Private::Private(not_null<Room*> owner)
: owner(owner)
, reloadTimer([=] { if (reloadFailed && connected) { reload(true); } })
, statusTimer([=] { sendStatus(); })
, leftTimer([=] { removeLeftFiles(); }) {
}

void Room::Private::fire(Changes value) {
	if (value) {
		const auto queues = Changes(Change::MusicQueue)
			| Change::VideoQueue
			| Change::Reloaded;
		if (value & queues) {
			trackFiles();
		}
		changes.fire_copy(value);
	}
}

// Called after every change of a queue: remembers what has left both
// queues. The files (and the covers) of those items are removed a bit
// later, when the players have surely let them go.
void Room::Private::trackFiles() {
	if (sample || !sender || state.gone != Gone::No) {
		return;
	}
	auto now = QueueFiles(state, folder());
	const auto left = LeftFiles(queueFiles, now);
	for (const auto &[path, sha] : left) {
		leftFiles.emplace(path, sha);
	}
	for (const auto &[path, sha] : now) {
		leftFiles.remove(path);
	}
	queueFiles = std::move(now);
	if (leftFiles.empty()) {
		leftTimer.cancel();
	} else if (!left.empty() || !leftTimer.isActive()) {
		// Counted from the last item that has left.
		leftTimer.callOnce(kOrphanDelay);
	}

	// The covers of the items that are gone: out of the memory at once.
	auto used = base::flat_set<QString>();
	for (const auto kind : { Kind::Music, Kind::Video }) {
		for (const auto &item : state.player(kind).queue) {
			if (!item.cover.isEmpty()) {
				used.emplace(item.cover);
			}
		}
	}
	for (auto i = begin(covers); i != end(covers);) {
		if (used.contains(i->first)) {
			++i;
		} else {
			coversRequested.remove(i->first);
			i = covers.erase(i);
		}
	}
	for (auto i = begin(coversFailed); i != end(coversFailed);) {
		if (used.contains(*i)) {
			++i;
		} else {
			coversRequested.remove(*i);
			i = coversFailed.erase(i);
		}
	}
}

// Only what Room::download() or the room itself has put into the temp
// folder is deleted, never a file of the user (known[] may point to the
// original he has added from the disk).
void Room::Private::removeLeftFiles() {
	if (sample || state.gone != Gone::No) {
		leftFiles.clear();
		return;
	}
	const auto root = folder();
	const auto inQueues = [&](const QString &sha) {
		for (const auto &[path, media] : queueFiles) {
			if (media == sha) {
				return true;
			}
		}
		return false;
	};
	auto later = base::flat_map<QString, QString>();
	const auto list = base::take(leftFiles);
	for (const auto &[path, sha] : list) {
		if (queueFiles.contains(path) || !InFolder(path, root)) {
			continue;
		}
		// The copy made while a file of Telegram went to the relay.
		const auto i = known.find(sha);
		const auto copy = (i != end(known)
			&& !inQueues(sha)
			&& InFolder(i->second, root))
			? i->second
			: QString();
		const auto uploading = !copy.isEmpty()
			&& ranges::contains(
				tasks,
				copy,
				[](const std::unique_ptr<Task> &task) {
					return task->media.path;
				});
		const auto playing = music
			&& (music->usesFile(path)
				|| (!copy.isEmpty() && music->usesFile(copy)));
		if (uploading || playing) {
			// Still in the player (the user listens to it on his own)
			// or on its way to the relay once more.
			later.emplace(path, sha);
			continue;
		}
		QFile::remove(path);
		QFile::remove(path + u".part"_q);
		if (!copy.isEmpty()) {
			QFile::remove(copy);
			known.erase(i);
		}
	}
	leftFiles = std::move(later);
	if (!leftFiles.empty()) {
		leftTimer.callOnce(kOrphanDelay);
	}
}

void Room::Private::handle(const Cloud::Event &event) {
	if (state.gone != Gone::No) {
		return;
	} else if (event.type == u"resync"_q) {
		reload();
		return;
	} else if (reloading) {
		queued.push_back(event);
		return;
	}
	apply(event);
}

void Room::Private::apply(const Cloud::Event &event) {
	const auto weak = base::make_weak(owner.get());
	const auto wasOwner = state.owner;
	const auto result = ApplyEvent(state, event, selfId);
	if (result & Change::Gone) {
		wentGone();
	} else if (!wasOwner && state.owner) {
		// The banned and the invited lists are given only to the owner:
		// the new one asks for them once.
		refreshRoomInfo();
	}
	events.fire_copy(event);
	if (weak) {
		fire(result);
	}
}

void Room::Private::wentGone() {
	subscription.destroy();
	reloadTimer.cancel();
	statusTimer.cancel();
	leftTimer.cancel();
	leftFiles.clear();
	reloading = false;
	memberCheck = 0;
	queued.clear();
	if (sender) {
		sender->cancelAll();
	}
	while (!tasks.empty()) {
		removeTask(tasks.back()->id, true);
	}
}

// retry: one more attempt of the same reload (the timer), otherwise a new
// reason to reload ("resync", the stream is back) and the count of the
// attempts starts over.
void Room::Private::reload(bool retry) {
	if (reloading || !sender || state.gone != Gone::No) {
		return;
	}
	if (!retry) {
		reloadAttempts = 0;
	}
	reloadTimer.cancel();
	reloading = true;
	reloadFailed = false;
	owner->send(Cloud::GetRequest(QString()), [=](
			const Cloud::Response &response) {
		reloadDone(response.json.value(u"room"_q).toObject());
	}, [=](const Cloud::Error &error) {
		if (error.status == 404 || error.status == 403) {
			reloading = false;
			auto rejected = Cloud::Event{ .type = u"room.rejected"_q };
			rejected.room = state.code;
			queued.clear();
			apply(rejected);
			return;
		}
		reloadLater();
	});
}

// The snapshot did not come (or it is not one): the events that were held
// back are applied to what there is, the snapshot is asked for again
// after a pause that grows, a few times at most. After that the room
// waits for the stream to reconnect or to say "resync" once more.
void Room::Private::reloadLater() {
	reloading = false;
	reloadFailed = true;
	if (const auto delay = ReloadRetryDelay(reloadAttempts)) {
		++reloadAttempts;
		reloadTimer.callOnce(delay);
	}
	const auto weak = base::make_weak(owner.get());
	const auto list = base::take(queued);
	for (const auto &event : list) {
		if (!weak || state.gone != Gone::No) {
			return;
		}
		apply(event);
	}
}

void Room::Private::reloadDone(const QJsonObject &room) {
	auto fresh = ParseRoom(room, selfId);
	if (!fresh.valid() || fresh.code != state.code) {
		reloadLater();
		return;
	}
	reloading = false;
	reloadAttempts = 0;
	fresh.chat = std::move(state.chat);
	state = std::move(fresh);
	if (const auto strong = account.get()) {
		strong->setRoomCursor(state.code, state.eventId);
	}
	const auto weak = base::make_weak(owner.get());
	const auto cursor = state.eventId;
	for (const auto &event : base::take(queued)) {
		if (!event.id || event.id > cursor) {
			const auto result = ApplyEvent(state, event, selfId);
			if (result & Change::Gone) {
				wentGone();
				events.fire_copy(event);
				if (weak) {
					fire(result);
				}
				return;
			}
		}
	}
	reloaded.fire({});
	if (!weak) {
		return;
	}
	fire(Changes::from_raw(0x1FFFFU) & ~Changes(Change::Gone));
	if (weak) {
		loadChat();
	}
}

// Title, settings, owner, banned and invited lists from a room object of
// an answer. The members and the players come with the events.
void Room::Private::applyRoomInfo(const QJsonObject &room) {
	const auto fresh = ParseRoom(room, selfId);
	if (!fresh.valid() || fresh.code != state.code || fresh.rev < state.rev) {
		return;
	}
	auto result = Changes(Change::Room);
	state.title = fresh.title;
	state.settings = fresh.settings;
	state.rev = fresh.rev;
	state.banned = fresh.banned;
	state.invited = fresh.invited;
	result |= ApplyOwner(state, fresh.ownerId, selfId);
	fire(result);
}

void Room::Private::refreshRoomInfo() {
	owner->send(Cloud::GetRequest(QString()), [=](
			const Cloud::Response &response) {
		applyRoomInfo(response.json.value(u"room"_q).toObject());
	}, [](const Cloud::Error &) {
	});
}

// "me.rooms": the list of the rooms of the user has changed on another
// device. If this room was left there nothing else tells this window
// about it (the stream of the room just goes silent), so the room is
// asked once whether the user is still in it.
void Room::Private::checkMember() {
	if (memberCheck || reloading || state.gone != Gone::No) {
		return;
	}
	memberCheck = owner->send(Cloud::GetRequest(QString()), [=](
			const Cloud::Response &response) {
		memberCheck = 0;
		applyRoomInfo(response.json.value(u"room"_q).toObject());
	}, [=](const Cloud::Error &error) {
		memberCheck = 0;
		if (error.status == 404 && state.gone == Gone::No) {
			auto rejected = Cloud::Event{ .type = u"room.rejected"_q };
			rejected.room = state.code;
			apply(rejected);
		}
	});
}

void Room::Private::loadChat() {
	auto request = Cloud::GetRequest(
		u"/chat"_q,
		{ { u"limit"_q, QString::number(kChatLoad) } });
	owner->send(std::move(request), [=](const Cloud::Response &response) {
		const auto list = response.json.value(u"messages"_q).toArray();
		for (const auto &entry : list) {
			[[maybe_unused]] const auto added = AddChatMessage(
				state,
				ParseChatMessage(entry.toObject()));
		}
		chatLoaded = true;
		fire(Change::Chat);
	}, [=](const Cloud::Error &) {
		chatLoaded = true;
		fire(Change::Chat);
	});
}

void Room::Private::setConnected(bool value) {
	if (connected == value) {
		return;
	}
	connected = value;
	if (connected && reloadFailed) {
		reload();
	}
	auto result = Changes(Change::Connection);
	if (connected && !coversFailed.empty()) {
		// The covers that did not come are asked for once more when
		// they are painted again.
		const auto failed = base::take(coversFailed);
		for (const auto &sha : failed) {
			coversRequested.remove(sha);
		}
		result |= Change::Covers;
	}
	fire(result);
}

void Room::Private::playerAction(Kind kind, QJsonObject body, bool withRev) {
	if (withRev) {
		body.insert(u"if_rev"_q, double(state.player(kind).state.rev));
	}
	// The answer is not applied: the state of a player changes only
	// with the events, which come in the order of the server.
	owner->send(
		Cloud::PostRequest(u"/player/"_q + KindName(kind), std::move(body)),
		nullptr,
		[=](const Cloud::Error &error) {
			// "stale": somebody was faster, his action is the one.
			if (!error.is("stale")) {
				errors.fire_copy(error);
			}
		});
}

void Room::Private::pauseOther(Kind kind) {
	const auto other = (kind == Kind::Music) ? Kind::Video : Kind::Music;
	if (state.player(other).state.playing && state.rights.control) {
		auto body = QJsonObject();
		body.insert(u"action"_q, u"pause"_q);
		playerAction(other, std::move(body), false);
	}
}

Room::Private::Task *Room::Private::task(int id) {
	const auto i = ranges::find(tasks, id, &Task::id);
	return (i != end(tasks)) ? i->get() : nullptr;
}

void Room::Private::rebuildRows() {
	auto fresh = std::vector<Upload>();
	fresh.reserve(tasks.size());
	for (const auto &task : tasks) {
		const auto i = ranges::find(rows, task->id, &Upload::id);
		fresh.push_back((i != end(rows)) ? *i : Upload{
			.id = task->id,
			.kind = task->media.kind,
			.title = task->media.title,
		});
	}
	rows = std::move(fresh);
	fire(Change::Uploads);
}

void Room::Private::setProgress(int id, int64 ready, int64 total) {
	const auto i = ranges::find(rows, id, &Upload::id);
	if (i == end(rows) || (i->ready == ready && i->total == total)) {
		return;
	}
	i->ready = ready;
	i->total = total;
	fire(Change::Uploads);
}

void Room::Private::prepared(int id, Prepared &&result) {
	const auto raw = task(id);
	const auto strong = account.get();
	if (!raw || !strong) {
		return;
	}
	const auto video = (raw->media.kind == Kind::Video);
	const auto limit = video
		? strong->limit("video_bytes", kVideoBytesDefault)
		: strong->limit("audio_bytes", kAudioBytesDefault);
	if (!result.ok) {
		failTask(id, tr::lng_oblivion_room_file_failed(tr::now));
		return;
	} else if (result.size > limit) {
		failTask(id, tr::lng_oblivion_room_file_too_big(
			tr::now,
			lt_size,
			QString::number(limit / (1024 * 1024))));
		return;
	}
	auto &media = raw->media;
	if (media.title.isEmpty() || !result.title.isEmpty()) {
		media.title = !result.title.isEmpty()
			? result.title
			: QFileInfo(media.fileName).completeBaseName();
		if (media.performer.isEmpty()) {
			media.performer = result.performer;
		}
	}
	if (media.title.isEmpty()) {
		media.title = tr::lng_oblivion_room_untitled(tr::now);
	}
	media.duration = result.duration;
	raw->cover = std::move(result.cover);
	const auto i = ranges::find(rows, id, &Upload::id);
	if (i != end(rows)) {
		i->title = media.title;
		i->total = result.size;
	}
	fire(Change::Uploads);
	startUpload(id);
}

void Room::Private::startUpload(int id) {
	const auto raw = task(id);
	const auto strong = account.get();
	if (!raw || !strong) {
		return;
	}
	const auto guard = owner.get();
	const auto &media = raw->media;
	raw->transfer = strong->upload({
		.path = media.path,
		.bytes = media.bytes,
		.kind = (media.kind == Kind::Video) ? u"video"_q : u"audio"_q,
		.mime = media.mime,
		.done = crl::guard(guard, [=](const QString &sha, int64 size) {
			const auto raw = task(id);
			if (!raw) {
				return;
			}
			raw->transfer = 0;
			raw->sha = sha;
			raw->media.bytes = QByteArray();
			if (!raw->media.path.isEmpty()) {
				known[sha] = raw->media.path;
			}
			setProgress(id, size, size);
			if (raw->cover.isEmpty()) {
				finishTask(id);
			} else {
				uploadCover(id);
			}
		}),
		.fail = crl::guard(guard, [=](const Cloud::Error &error) {
			if (const auto raw = task(id)) {
				raw->transfer = 0;
				failTask(id, error);
			}
		}),
		.progress = crl::guard(guard, [=](int64 ready, int64 total) {
			setProgress(id, ready, total);
		}),
	});
}

void Room::Private::uploadCover(int id) {
	const auto raw = task(id);
	const auto strong = account.get();
	if (!raw || !strong) {
		return;
	}
	const auto guard = owner.get();
	raw->transfer = strong->upload({
		.bytes = base::take(raw->cover),
		.kind = u"image"_q,
		.mime = u"image/jpeg"_q,
		.done = crl::guard(guard, [=](const QString &sha, int64) {
			if (const auto raw = task(id)) {
				raw->transfer = 0;
				raw->coverSha = sha;
				finishTask(id);
			}
		}),
		.fail = crl::guard(guard, [=](const Cloud::Error &) {
			// A track without a cover is still a track.
			if (const auto raw = task(id)) {
				raw->transfer = 0;
				finishTask(id);
			}
		}),
	});
}

void Room::Private::finishTask(int id) {
	const auto raw = task(id);
	if (!raw) {
		return;
	}
	const auto &media = raw->media;
	const auto kind = media.kind;
	auto items = QJsonArray();
	items.push_back(SerializeItem({
		.media = raw->sha,
		.title = media.title,
		.performer = media.performer,
		.duration = media.duration,
		.cover = raw->coverSha,
		.fileName = media.fileName,
	}));
	auto body = QJsonObject();
	body.insert(u"items"_q, items);
	body.insert(u"position"_q, media.next ? u"next"_q : u"end"_q);
	body.insert(u"play"_q, media.play && state.rights.control);
	if (media.play) {
		pauseOther(kind);
	}
	raw->request = owner->send(
		QueueRequest(Cloud::PostRequest(
			u"/queue/"_q + KindName(kind),
			std::move(body))),
		[=](const Cloud::Response &) {
			removeTask(id, false);
		},
		[=](const Cloud::Error &error) {
			if (const auto raw = task(id)) {
				raw->request = 0;
				failTask(id, error);
			}
		});
	if (!raw->request) {
		removeTask(id, false);
	}
}

void Room::Private::failTask(int id, const Cloud::Error &error) {
	removeTask(id, false);
	if (error.type != Cloud::Error::Type::Cancelled) {
		errors.fire_copy(error);
	}
}

void Room::Private::failTask(int id, const QString &text) {
	removeTask(id, false);
	toasts.fire_copy(text);
}

void Room::Private::removeTask(int id, bool cancel) {
	const auto i = ranges::find(tasks, id, &Task::id);
	if (i == end(tasks)) {
		return;
	}
	const auto raw = i->get();
	if (cancel) {
		if (const auto strong = account.get()) {
			if (raw->transfer) {
				strong->cancelTransfer(raw->transfer);
			}
		}
		if (raw->request && sender) {
			sender->cancel(raw->request);
		}
	}
	tasks.erase(i);
	rebuildRows();
}

void Room::Private::sendStatus() {
	const auto same = [&](int index) {
		return statusKnown[index]
			&& (statusWanted[index] == statusSent[index]);
	};
	if (same(0) && same(1)) {
		return;
	}
	const auto now = crl::now();
	if (statusSentAt && now - statusSentAt < kStatusGap) {
		if (!statusTimer.isActive()) {
			statusTimer.callOnce(kStatusGap - (now - statusSentAt));
		}
		return;
	}
	const auto serialize = [&](int index) -> QJsonValue {
		const auto &status = statusWanted[index];
		if (status.itemId.isEmpty()) {
			return QJsonValue();
		}
		auto result = QJsonObject();
		result.insert(u"item_id"_q, status.itemId);
		result.insert(u"ready"_q, status.ready);
		result.insert(u"buffered"_q, status.buffered);
		return result;
	};
	auto body = QJsonObject();
	body.insert(u"music"_q, serialize(0));
	body.insert(u"video"_q, serialize(1));
	const auto sent = owner->send(
		Cloud::PostRequest(u"/status"_q, std::move(body)),
		nullptr,
		[](const Cloud::Error &) {});
	if (sent) {
		statusSentAt = now;
		for (auto i = 0; i != 2; ++i) {
			statusSent[i] = statusWanted[i];
			statusKnown[i] = true;
		}
	}
}

QString Room::Private::folder() const {
	return RoomFolder(selfId, state.code);
}

QString Room::Private::mediaPath(const QueueItem &item) const {
	return folder() + MediaFileName(item);
}

Room::Room(Descriptor &&descriptor)
: _private(std::make_unique<Private>(this)) {
	const auto p = _private.get();
	p->selfId = descriptor.selfId;
	p->state = std::move(descriptor.state);
	p->now = std::move(descriptor.now);
	p->sample = !descriptor.session;
	p->connected = descriptor.connected;
	p->rows = std::move(descriptor.uploads);
	if (p->sample || !p->state.valid()) {
		p->chatLoaded = true;
		return;
	}
	const auto session = descriptor.session;
	const auto account = &Cloud::For(session);
	p->session = base::make_weak(session);
	p->account = base::make_weak(account);
	p->sender.emplace(account);
	p->connected = (account->state() == Cloud::State::Online);
	p->queueFiles = QueueFiles(p->state, p->folder());
	p->subscription = account->subscribeRoom(p->state.code, p->state.eventId);
	account->roomEvents(
		p->state.code
	) | rpl::on_next([=](const Cloud::Event &event) {
		p->handle(event);
	}, p->lifetime);
	account->events(
	) | rpl::filter([](const Cloud::Event &event) {
		return (event.type == u"me.rooms"_q);
	}) | rpl::on_next([=](const Cloud::Event &) {
		p->checkMember();
	}, p->lifetime);
	account->stateValue(
	) | rpl::on_next([=](Cloud::State state) {
		p->setConnected(state == Cloud::State::Online);
	}, p->lifetime);
	p->loadChat();
	p->music = CreateMusicEngine(this);
}

Room::~Room() {
	const auto p = _private.get();
	p->external.destroy();
	p->music = nullptr;
	p->lifetime.destroy();
	p->subscription.destroy();
	while (!p->tasks.empty()) {
		p->removeTask(p->tasks.back()->id, true);
	}
	p->sender.reset();
	if (!p->sample && p->state.gone != Gone::No) {
		RemoveRoomFolder(p->selfId, p->state.code);
	}
}

void Room::detach() {
	const auto p = _private.get();
	p->external.destroy();
	p->music = nullptr;
	p->lifetime.destroy();
	p->subscription.destroy();
	p->reloadTimer.cancel();
	p->statusTimer.cancel();
	p->leftTimer.cancel();
	p->leftFiles.clear();
	p->reloading = false;
	p->memberCheck = 0;
	p->queued.clear();
	while (!p->tasks.empty()) {
		p->removeTask(p->tasks.back()->id, true);
	}
	p->sender.reset();
	p->account = nullptr;
	p->session = nullptr;
	p->connected = false;
}

Main::Session *Room::session() const {
	return _private->session.get();
}

Cloud::Account *Room::account() const {
	return _private->account.get();
}

bool Room::sample() const {
	return _private->sample;
}

uint64 Room::selfId() const {
	return _private->selfId;
}

const QString &Room::code() const {
	return _private->state.code;
}

const RoomState &Room::state() const {
	return _private->state;
}

rpl::producer<Changes> Room::changes() const {
	return _private->changes.events();
}

rpl::producer<Cloud::Event> Room::events() const {
	return _private->events.events();
}

rpl::producer<> Room::reloaded() const {
	return _private->reloaded.events();
}

rpl::producer<Cloud::Error> Room::errors() const {
	return _private->errors.events();
}

rpl::producer<QString> Room::toasts() const {
	return _private->toasts.events();
}

rpl::lifetime &Room::lifetime() {
	return _private->external;
}

bool Room::connected() const {
	return _private->connected;
}

bool Room::can(Right right) const {
	return _private->state.owner || _private->state.rights.has(right);
}

bool Room::owner() const {
	return _private->state.owner;
}

const Player &Room::player(Kind kind) const {
	return std::as_const(_private->state).player(kind);
}

int64 Room::now() const {
	return _private->now ? _private->now() : Cloud::Now();
}

int64 Room::position(Kind kind) const {
	const auto &data = player(kind);
	const auto current = data.current();
	return PositionAt(data.state, current ? current->duration : 0, now());
}

MusicEngine *Room::music() const {
	return _private->music.get();
}

Cloud::RequestId Room::send(
		Cloud::Request &&request,
		Cloud::Done done,
		Cloud::Fail fail) {
	const auto p = _private.get();
	if (!p->sender || !p->account || p->state.gone != Gone::No) {
		return 0;
	}
	request.path = u"/v1/rooms/"_q + p->state.code + request.path;
	const auto weak = base::make_weak(this);
	return p->sender->request(std::move(request), [=](
			const Cloud::Response &response) {
		if (weak && done) {
			done(response);
		}
	}, [=](const Cloud::Error &error) {
		if (const auto strong = weak.get()) {
			if (fail) {
				fail(error);
			} else {
				strong->_private->errors.fire_copy(error);
			}
		}
	});
}

void Room::cancel(Cloud::RequestId id) {
	if (_private->sender && id) {
		_private->sender->cancel(id);
	}
}

void Room::play(Kind kind) {
	_private->pauseOther(kind);
	auto body = QJsonObject();
	body.insert(u"action"_q, u"play"_q);
	_private->playerAction(kind, std::move(body), false);
}

void Room::pause(Kind kind) {
	auto body = QJsonObject();
	body.insert(u"action"_q, u"pause"_q);
	_private->playerAction(kind, std::move(body), false);
}

void Room::seek(Kind kind, int64 position) {
	const auto current = player(kind).current();
	if (!current) {
		return;
	}
	auto body = QJsonObject();
	body.insert(u"action"_q, u"seek"_q);
	body.insert(
		u"position_ms"_q,
		double(std::clamp(position, int64(0), current->duration)));
	_private->playerAction(kind, std::move(body), false);
}

void Room::select(Kind kind, const QString &itemId) {
	if (!player(kind).find(itemId)) {
		return;
	}
	_private->pauseOther(kind);
	auto body = QJsonObject();
	body.insert(u"action"_q, u"select"_q);
	body.insert(u"item_id"_q, itemId);
	_private->playerAction(kind, std::move(body), false);
}

void Room::next(Kind kind) {
	auto body = QJsonObject();
	body.insert(u"action"_q, u"next"_q);
	_private->playerAction(kind, std::move(body), true);
}

void Room::previous(Kind kind) {
	auto body = QJsonObject();
	body.insert(u"action"_q, u"prev"_q);
	_private->playerAction(kind, std::move(body), true);
}

void Room::setRepeat(Kind kind, Repeat repeat) {
	auto body = QJsonObject();
	body.insert(u"action"_q, u"repeat"_q);
	body.insert(u"mode"_q, RepeatName(repeat));
	_private->playerAction(kind, std::move(body), false);
}

void Room::removeItem(Kind kind, const QString &itemId) {
	if (!player(kind).find(itemId)) {
		return;
	}
	send(QueueRequest(Cloud::DeleteRequest(
		u"/queue/"_q + KindName(kind) + '/' + itemId)));
}

void Room::moveItem(Kind kind, int from, int to) {
	const auto &queue = player(kind).queue;
	const auto target = MoveTarget(queue, from, to);
	if (!target.first) {
		return;
	}
	auto body = QJsonObject();
	body.insert(u"item_id"_q, queue[from].id);
	body.insert(u"before"_q, target.second.isEmpty()
		? QJsonValue()
		: QJsonValue(target.second));
	send(QueueRequest(Cloud::PostRequest(
		u"/queue/"_q + KindName(kind) + u"/move"_q,
		std::move(body))));
}

void Room::moveItem(Kind kind, const QString &itemId, QueueMove move) {
	const auto indexes = MoveIndexes(player(kind), itemId, move);
	if (indexes.first >= 0) {
		moveItem(kind, indexes.first, indexes.second);
	}
}

void Room::clearQueue(Kind kind) {
	send(QueueRequest(Cloud::PostRequest(
		u"/queue/"_q + KindName(kind) + u"/clear"_q)));
}

int Room::addMedia(AddMedia &&media) {
	const auto p = _private.get();
	if (!p->sender || !p->account || p->state.gone != Gone::No) {
		return 0;
	} else if (media.path.isEmpty() && media.bytes.isEmpty()) {
		return 0;
	}
	if (media.fileName.isEmpty() && !media.path.isEmpty()) {
		media.fileName = QFileInfo(media.path).fileName();
	}
	if (media.mime.isEmpty()) {
		media.mime = MimeFor(media.fileName, media.kind);
	}
	media.mime = RelayMime(media.mime.toLower(), media.kind);
	const auto id = ++p->taskIds;
	auto task = std::make_unique<Private::Task>();
	task->id = id;
	task->media = std::move(media);
	const auto &stored = task->media;
	const auto kind = stored.kind;
	const auto path = stored.path;
	const auto bytes = stored.bytes;
	const auto title = stored.title;
	const auto performer = stored.performer;
	const auto duration = stored.duration;
	const auto cover = base::take(task->media.cover);
	p->tasks.push_back(std::move(task));
	p->rows.push_back({
		.id = id,
		.kind = kind,
		.title = !title.isEmpty()
			? title
			: QFileInfo(stored.fileName).completeBaseName(),
	});
	p->fire(Change::Uploads);

	const auto weak = base::make_weak(this);
	crl::async([=] {
		auto result = PrepareMedia(
			kind,
			path,
			bytes,
			title,
			performer,
			duration,
			cover);
		crl::on_main(weak, [=, result = std::move(result)]() mutable {
			p->prepared(id, std::move(result));
		});
	});
	return id;
}

void Room::cancelUpload(int id) {
	_private->removeTask(id, true);
}

const std::vector<Upload> &Room::uploads() const {
	return _private->rows;
}

void Room::addUploaded(
		Kind kind,
		std::vector<ItemInput> items,
		bool next,
		bool play) {
	if (items.empty()) {
		return;
	}
	auto list = QJsonArray();
	for (const auto &item : items) {
		if (Cloud::ValidMediaId(item.media) && item.duration > 0) {
			list.push_back(SerializeItem(item));
		}
	}
	if (list.isEmpty()) {
		return;
	}
	const auto p = _private.get();
	auto body = QJsonObject();
	body.insert(u"items"_q, list);
	body.insert(u"position"_q, next ? u"next"_q : u"end"_q);
	body.insert(u"play"_q, play && p->state.rights.control);
	if (play) {
		p->pauseOther(kind);
	}
	send(QueueRequest(Cloud::PostRequest(
		u"/queue/"_q + KindName(kind),
		std::move(body))));
}

void Room::reportStatus(
		Kind kind,
		const QString &itemId,
		bool ready,
		int buffered) {
	const auto p = _private.get();
	const auto index = (kind == Kind::Video) ? 1 : 0;
	p->statusWanted[index] = {
		.itemId = itemId,
		.ready = ready,
		.buffered = std::clamp(buffered, 0, 100),
	};
	p->sendStatus();
}

void Room::setRight(uint64 userId, Right right, bool value) {
	const auto p = _private.get();
	auto body = QJsonObject();
	body.insert(u"rights"_q, SerializeRight(right, value));
	send(
		Cloud::PatchRequest(
			u"/members/"_q + QString::number(userId),
			std::move(body)),
		[=](const Cloud::Response &response) {
			// The answer of an earlier click may come after the event
			// of a later one: only the right that was asked for is taken
			// from it, the whole member comes with the events.
			const auto answer = ParseMember(
				response.json.value(u"member"_q).toObject());
			const auto i = ranges::find(
				p->state.members,
				userId,
				&Member::id);
			if (answer.id != userId
				|| i == end(p->state.members)
				|| i->owner
				|| i->rights.has(right) == answer.rights.has(right)) {
				return;
			}
			auto member = *i;
			member.rights.set(right, answer.rights.has(right));
			p->fire(UpsertMember(p->state, std::move(member), p->selfId));
		});
}

void Room::setDefaultRight(Right right, bool value) {
	const auto p = _private.get();
	auto settings = QJsonObject();
	settings.insert(u"default_rights"_q, SerializeRight(right, value));
	auto body = QJsonObject();
	body.insert(u"settings"_q, settings);
	send(
		Cloud::PatchRequest(QString(), std::move(body)),
		[=](const Cloud::Response &response) {
			p->applyRoomInfo(response.json.value(u"room"_q).toObject());
		});
}

void Room::setRights(uint64 userId, const Rights &rights) {
	const auto p = _private.get();
	auto body = QJsonObject();
	body.insert(u"rights"_q, SerializeRights(rights));
	send(
		Cloud::PatchRequest(
			u"/members/"_q + QString::number(userId),
			std::move(body)),
		[=](const Cloud::Response &response) {
			auto member = ParseMember(
				response.json.value(u"member"_q).toObject());
			member.owner = (member.id == p->state.ownerId);
			p->fire(UpsertMember(p->state, std::move(member), p->selfId));
		});
}

void Room::setDefaults(const Rights &rights, bool applyToAll) {
	const auto p = _private.get();
	auto settings = QJsonObject();
	settings.insert(u"default_rights"_q, SerializeRights(rights));
	auto body = QJsonObject();
	body.insert(u"settings"_q, settings);
	body.insert(u"apply_to_all"_q, applyToAll);
	send(
		Cloud::PatchRequest(QString(), std::move(body)),
		[=](const Cloud::Response &response) {
			p->applyRoomInfo(response.json.value(u"room"_q).toObject());
		});
}

void Room::kick(uint64 userId, bool ban) {
	const auto p = _private.get();
	auto body = QJsonObject();
	body.insert(u"user_id"_q, double(userId));
	body.insert(u"ban"_q, ban);
	send(
		Cloud::PostRequest(u"/kick"_q, std::move(body)),
		[=](const Cloud::Response &) {
			if (ban) {
				p->refreshRoomInfo();
			}
		});
}

void Room::unban(uint64 userId) {
	const auto p = _private.get();
	auto body = QJsonObject();
	body.insert(u"user_id"_q, double(userId));
	send(
		Cloud::PostRequest(u"/unban"_q, std::move(body)),
		[=](const Cloud::Response &) {
			p->refreshRoomInfo();
		});
}

void Room::transfer(uint64 userId) {
	const auto p = _private.get();
	auto body = QJsonObject();
	body.insert(u"user_id"_q, double(userId));
	send(
		Cloud::PostRequest(u"/transfer"_q, std::move(body)),
		[=](const Cloud::Response &response) {
			p->applyRoomInfo(response.json.value(u"room"_q).toObject());
		});
}

void Room::rename(const QString &title) {
	const auto p = _private.get();
	auto body = QJsonObject();
	body.insert(u"title"_q, title.trimmed().left(kTitleLimit));
	send(
		Cloud::PatchRequest(QString(), std::move(body)),
		[=](const Cloud::Response &response) {
			p->applyRoomInfo(response.json.value(u"room"_q).toObject());
		});
}

bool Room::sendChat(const QString &text, Fn<void()> failed) {
	const auto trimmed = text.trimmed().left(kChatTextLimit);
	if (trimmed.isEmpty()) {
		return false;
	}
	const auto p = _private.get();
	auto body = QJsonObject();
	body.insert(u"text"_q, trimmed);
	body.insert(
		u"client_id"_q,
		u"c-"_q + QString::number(base::RandomValue<uint32>(), 16));
	const auto sent = send(
		Cloud::PostRequest(u"/chat"_q, std::move(body)),
		[=](const Cloud::Response &response) {
			auto message = ParseChatMessage(
				response.json.value(u"message"_q).toObject());
			if (AddChatMessage(p->state, std::move(message))) {
				p->fire(Change::Chat);
			}
		},
		[=](const Cloud::Error &error) {
			const auto weak = base::make_weak(this);
			p->errors.fire_copy(error);
			if (weak && failed) {
				failed();
			}
		});
	return (sent != 0);
}

void Room::deleteChat(int64 id) {
	const auto p = _private.get();
	send(
		Cloud::DeleteRequest(u"/chat/"_q + QString::number(id)),
		[=](const Cloud::Response &) {
			const auto i = ranges::find(p->state.chat, id, &ChatMessage::id);
			if (i != end(p->state.chat)) {
				p->state.chat.erase(i);
				p->fire(Change::Chat);
			}
		});
}

bool Room::chatLoaded() const {
	return _private->chatLoaded;
}

void Room::leave(Fn<void()> done) {
	const auto p = _private.get();
	const auto finish = [=](Gone reason) {
		if (p->state.gone == Gone::No) {
			p->state.gone = reason;
			p->wentGone();
			p->fire(Change::Gone);
		}
	};
	const auto weak = base::make_weak(this);
	const auto sent = send(
		Cloud::PostRequest(u"/leave"_q),
		[=](const Cloud::Response &) {
			finish(Gone::Left);
			if (weak && done) {
				done();
			}
		});
	if (!sent && p->sample && done) {
		done();
	}
}

void Room::closeForAll(Fn<void()> done) {
	const auto p = _private.get();
	const auto weak = base::make_weak(this);
	send(
		Cloud::DeleteRequest(QString()),
		[=](const Cloud::Response &) {
			if (p->state.gone == Gone::No) {
				p->state.gone = Gone::Closed;
				p->wentGone();
				p->fire(Change::Gone);
			}
			if (weak && done) {
				done();
			}
		});
}

QString Room::localFile(const QueueItem &item) const {
	const auto p = _private.get();
	const auto i = p->known.find(item.media);
	if (i != end(p->known) && QFile::exists(i->second)) {
		return i->second;
	}
	const auto path = p->mediaPath(item);
	return (!p->sample && QFile::exists(path)) ? path : QString();
}

Cloud::TransferId Room::download(
		const QueueItem &item,
		Fn<void(const QString &path)> done,
		Cloud::Fail fail,
		Cloud::Progress progress) {
	const auto p = _private.get();
	const auto strong = p->account.get();
	if (!strong || p->state.gone != Gone::No) {
		return 0;
	}
	const auto have = localFile(item);
	if (!have.isEmpty()) {
		crl::on_main(this, [=] {
			if (done) {
				done(have);
			}
		});
		return 0;
	}
	QDir().mkpath(p->folder());
	return strong->download({
		.media = item.media,
		.to = p->mediaPath(item),
		.done = crl::guard(this, [=](const QString &path) {
			if (done) {
				done(path);
			}
		}),
		.fail = crl::guard(this, [=](const Cloud::Error &error) {
			if (fail) {
				fail(error);
			}
		}),
		.progress = crl::guard(this, [=](int64 ready, int64 total) {
			if (progress) {
				progress(ready, total);
			}
		}),
	});
}

void Room::cancelDownload(Cloud::TransferId id) {
	if (const auto strong = _private->account.get()) {
		if (id) {
			strong->cancelTransfer(id);
		}
	}
}

QImage Room::cover(const QString &sha256) {
	const auto p = _private.get();
	const auto i = p->covers.find(sha256);
	if (i != end(p->covers)) {
		return i->second;
	}
	const auto strong = p->account.get();
	if (!strong
		|| p->state.gone != Gone::No
		|| !Cloud::ValidMediaId(sha256)
		|| !p->coversRequested.emplace(sha256).second) {
		return QImage();
	}
	QDir().mkpath(p->folder());
	const auto weak = base::make_weak(this);
	strong->download({
		.media = sha256,
		.to = p->folder() + u"cover_"_q + sha256,
		.done = crl::guard(this, [=](const QString &path) {
			crl::async([=] {
				auto image = QImage(path);
				if (!image.isNull()
					&& (image.width() > 2 * kCoverSide
						|| image.height() > 2 * kCoverSide)) {
					image = image.scaled(
						2 * kCoverSide,
						2 * kCoverSide,
						Qt::KeepAspectRatio,
						Qt::SmoothTransformation);
				}
				crl::on_main(weak, [=, image = std::move(image)] {
					if (!image.isNull()
						&& p->coversRequested.contains(sha256)) {
						p->covers[sha256] = image;
						p->fire(Change::Covers);
					}
				});
			});
		}),
		.fail = crl::guard(this, [=](const Cloud::Error &) {
			// Asked for again after the next reconnect, not before.
			if (p->coversRequested.contains(sha256)) {
				p->coversFailed.emplace(sha256);
			}
		}),
	});
	return QImage();
}

void RemoveRoomFolder(uint64 userId, const QString &code) {
	if (!userId || Cloud::NormalizeRoomCode(code).isEmpty()) {
		return;
	}
	const auto path = RoomFolder(userId, code);
	crl::async([=] {
		QDir(path).removeRecursively();
	});
}

QString RoomFolderName(uint64 userId, const QString &code) {
	return QString::number(userId) + '_' + code;
}

void CleanupRoomFolders(uint64 forgetUserId, const QStringList &keep) {
	const auto root = RoomsFolder();
	const auto prefix = forgetUserId
		? (QString::number(forgetUserId) + '_')
		: QString();
	crl::async([=] {
		const auto now = QDateTime::currentSecsSinceEpoch();
		const auto list = QDir(root).entryInfoList(
			QDir::Dirs | QDir::NoDotAndDotDot);
		for (const auto &info : list) {
			const auto name = info.fileName();
			const auto forget = !prefix.isEmpty() && name.startsWith(prefix);
			// The age is that of the last file added to the folder: it
			// says nothing about a room that is open, so those are never
			// judged by it.
			const auto old = prefix.isEmpty()
				&& !keep.contains(name)
				&& ((now - info.lastModified().toSecsSinceEpoch())
					> kFolderTtl);
			if (forget || old) {
				QDir(info.absoluteFilePath()).removeRecursively();
			}
		}
	});
}

// ---- OBLIVION_SELFTEST=room.

namespace {

class Checker final {
public:
	explicit Checker(QStringList &log) : _log(log) {
	}

	void operator()(bool condition, const char *what) {
		if (condition) {
			++_passed;
			++_sectionPassed;
		} else {
			++_failed;
			++_sectionFailed;
			_log.push_back(u"FAILED: "_q + QString::fromUtf8(what));
		}
	}
	void section(const char *name) {
		_log.push_back(u"%1: %2 passed, %3 failed"_q.arg(
			QString::fromUtf8(name),
			QString::number(_sectionPassed),
			QString::number(_sectionFailed)));
		_sectionPassed = _sectionFailed = 0;
	}
	[[nodiscard]] int passed() const {
		return _passed;
	}
	[[nodiscard]] int failed() const {
		return _failed;
	}

private:
	QStringList &_log;
	int _passed = 0;
	int _failed = 0;
	int _sectionPassed = 0;
	int _sectionFailed = 0;

};

constexpr auto kTestSelf = uint64(9000000000000101ULL);
constexpr auto kTestOwner = uint64(9000000000000100ULL);
constexpr auto kTestOther = uint64(9000000000000102ULL);

[[nodiscard]] QString TestSha(char fill) {
	return QString(64, QChar(fill));
}

[[nodiscard]] QJsonObject TestRights(bool value) {
	auto rights = Rights();
	for (auto i = 0; i != kRightsCount; ++i) {
		rights.set(Right(i), value);
	}
	return SerializeRights(rights);
}

[[nodiscard]] QJsonObject TestMember(
		uint64 id,
		const QString &name,
		bool owner,
		bool online) {
	auto result = QJsonObject();
	result.insert(u"id"_q, double(id));
	result.insert(u"name"_q, name);
	result.insert(u"avatar_rev"_q, 1);
	result.insert(u"role"_q, owner ? u"owner"_q : u"member"_q);
	result.insert(u"rights"_q, TestRights(true));
	result.insert(u"joined_at"_q, double(1759800000000));
	result.insert(u"online"_q, online);
	return result;
}

[[nodiscard]] QJsonObject TestItem(
		const QString &id,
		char sha,
		int64 duration,
		uint64 addedBy) {
	auto result = QJsonObject();
	result.insert(u"id"_q, id);
	result.insert(u"kind"_q, u"music"_q);
	result.insert(u"media"_q, TestSha(sha));
	result.insert(u"size"_q, 1000);
	result.insert(u"mime"_q, u"audio/mpeg"_q);
	result.insert(u"title"_q, u"Song "_q + id);
	result.insert(u"performer"_q, u"Artist"_q);
	result.insert(u"duration_ms"_q, double(duration));
	result.insert(u"cover"_q, QJsonValue());
	result.insert(u"file_name"_q, id + u".mp3"_q);
	result.insert(u"added_by"_q, double(addedBy));
	result.insert(u"added_at"_q, double(1759800000000));
	return result;
}

[[nodiscard]] QJsonObject TestPlayerState(
		const QString &itemId,
		bool playing,
		int64 position,
		int64 anchor,
		int64 rev,
		const QString &repeat = u"off"_q) {
	auto result = QJsonObject();
	result.insert(u"item_id"_q, itemId.isEmpty()
		? QJsonValue()
		: QJsonValue(itemId));
	result.insert(u"playing"_q, playing);
	result.insert(u"position_ms"_q, double(position));
	result.insert(u"anchor_ms"_q, double(anchor));
	result.insert(u"rate"_q, 1);
	result.insert(u"repeat"_q, repeat);
	result.insert(u"rev"_q, double(rev));
	result.insert(u"updated_by"_q, double(kTestOwner));
	result.insert(u"updated_at"_q, double(anchor));
	return result;
}

[[nodiscard]] QJsonObject TestRoom() {
	auto members = QJsonArray();
	members.push_back(TestMember(kTestOwner, u"Owner"_q, true, true));
	members.push_back(TestMember(kTestSelf, u"Me"_q, false, true));
	auto queue = QJsonArray();
	queue.push_back(TestItem(u"a1"_q, 'a', 200000, kTestOwner));
	queue.push_back(TestItem(u"b2"_q, 'b', 180000, kTestSelf));
	queue.push_back(TestItem(u"c3"_q, 'c', 240000, kTestOther));
	auto music = QJsonObject();
	music.insert(u"queue"_q, queue);
	music.insert(
		u"state"_q,
		TestPlayerState(u"a1"_q, true, 1000, 1759800000000, 5));
	auto video = QJsonObject();
	video.insert(u"queue"_q, QJsonArray());
	video.insert(u"state"_q, TestPlayerState(QString(), false, 0, 0, 0));
	auto settings = QJsonObject();
	settings.insert(u"max_members"_q, 30);
	settings.insert(u"join"_q, u"link"_q);
	settings.insert(u"join_from_activity"_q, false);
	settings.insert(u"default_rights"_q, TestRights(true));
	auto me = QJsonObject();
	me.insert(u"role"_q, u"member"_q);
	auto mine = Rights::OnlyOwner();
	mine.add = true;
	me.insert(u"rights"_q, SerializeRights(mine));
	auto chat = QJsonObject();
	chat.insert(u"last_id"_q, 3);
	chat.insert(u"count"_q, 3);
	auto result = QJsonObject();
	result.insert(u"code"_q, u"k7qm2xpa9z"_q);
	result.insert(u"link"_q, u"https://example.com/evil"_q);
	result.insert(u"title"_q, QString(200, QChar('T')));
	result.insert(u"owner_id"_q, double(kTestOwner));
	result.insert(u"created_at"_q, double(1759800000000));
	result.insert(u"rev"_q, 7);
	result.insert(u"settings"_q, settings);
	result.insert(u"me"_q, me);
	result.insert(u"members"_q, members);
	result.insert(u"music"_q, music);
	result.insert(u"video"_q, video);
	result.insert(u"voice"_q, QJsonValue());
	result.insert(u"chat"_q, chat);
	result.insert(u"event_id"_q, double(1759800000123400));
	return result;
}

[[nodiscard]] Cloud::Event TestEvent(
		const QString &type,
		QJsonObject data,
		int64 id = 1) {
	auto result = Cloud::Event();
	result.type = type;
	result.id = id;
	result.room = u"K7QM2XPA9Z"_q;
	result.ts = 1759800001000;
	data.insert(u"room"_q, result.room);
	result.data = std::move(data);
	return result;
}

void TestParse(Checker &check) {
	const auto state = ParseRoom(TestRoom(), kTestSelf);
	check(state.valid() && state.code == u"K7QM2XPA9Z"_q, "code upper");
	check(state.link == Cloud::MakeLink(Cloud::LinkKind::Room, state.code),
		"link is built locally");
	check(state.title.size() == kTitleLimit, "title clamped");
	check(!state.owner && state.ownerId == kTestOwner, "not the owner");
	check(state.rights.add && state.rights.chat && !state.rights.control,
		"own rights");
	check(state.members.size() == 2, "two members");
	check(state.members[0].owner
		&& state.members[0].rights == Rights::Everything(),
		"the owner has everything");
	check(state.music.queue.size() == 3, "three items");
	check(state.music.current()
		&& state.music.current()->id == u"a1"_q, "current item");
	check(state.music.state.rev == 5 && state.music.state.playing,
		"player state");
	check(state.video.state.itemId.isEmpty() && !state.video.current(),
		"empty video player");
	check(state.eventId == 1759800000123400, "event id");
	check(state.chatLastId == 3, "chat last id");
	check(state.onlineCount() == 2, "online count");
	check(!ParseRoom(QJsonObject(), kTestSelf).valid(), "empty is invalid");

	auto asOwner = ParseRoom(TestRoom(), kTestOwner);
	check(asOwner.owner && asOwner.rights == Rights::Everything(),
		"the owner by id");

	auto bad = TestItem(u"bad id!"_q, 'a', 1000, 1);
	check(ParseQueueItem(bad).id.isEmpty(), "bad item id dropped");
	bad = TestItem(u"ok"_q, 'a', 0, 1);
	check(ParseQueueItem(bad).id.isEmpty(), "no duration dropped");
	bad = TestItem(u"ok"_q, 'a', 1000, 1);
	bad.insert(u"media"_q, u"../../etc/passwd"_q);
	check(ParseQueueItem(bad).id.isEmpty(), "bad media dropped");
	bad = TestItem(u"ok"_q, 'a', 1000, 1);
	bad.insert(u"cover"_q, u"nothex"_q);
	check(!ParseQueueItem(bad).id.isEmpty()
		&& ParseQueueItem(bad).cover.isEmpty(), "bad cover ignored");

	auto voice = QJsonObject();
	voice.insert(u"link"_q, u"https://evil.example/x"_q);
	check(!ParseVoice(voice).valid(), "foreign voice link dropped");
	voice.insert(u"link"_q, u"https://t.me/+AbCd"_q);
	check(!ParseVoice(voice).valid(), "too short an invite dropped");
	voice.insert(u"link"_q, u"https://t.me/SomeBot?start=payload"_q);
	check(!ParseVoice(voice).valid(), "a bot link is not a voice chat");
	voice.insert(u"link"_q, u"https://t.me/+AbCdEfGh1234"_q);
	voice.insert(u"tg_chat_id"_q, u"-1001234567890"_q);
	check(ParseVoice(voice).valid()
		&& ParseVoice(voice).link == u"https://t.me/+AbCdEfGh1234"_q,
		"an invite link kept");
	voice.insert(u"link"_q, u"https://t.me/some_group?videochat"_q);
	check(ParseVoice(voice).valid(), "a public group link kept");

	auto message = QJsonObject();
	message.insert(u"id"_q, 5);
	message.insert(u"user_id"_q, double(kTestOther));
	message.insert(u"name"_q, u"A\nB"_q);
	message.insert(u"text"_q, QString(5000, QChar('x')));
	const auto parsed = ParseChatMessage(message);
	check(parsed.text.size() == kChatTextLimit, "chat text clamped");
	check(!parsed.name.contains(QChar('\n')), "name is one line");
}

void TestReducer(Checker &check) {
	auto state = ParseRoom(TestRoom(), kTestSelf);
	const auto apply = [&](const QString &type, QJsonObject data) {
		return ApplyEvent(state, TestEvent(type, std::move(data)), kTestSelf);
	};

	// room.updated.
	auto data = QJsonObject();
	data.insert(u"title"_q, u"Night"_q);
	data.insert(u"owner_id"_q, double(kTestOwner));
	data.insert(u"rev"_q, 8);
	auto result = apply(u"room.updated"_q, data);
	check((result & Change::Room) && state.title == u"Night"_q
		&& state.rev == 8, "room.updated title");
	check(!(result & Change::Rights), "room.updated keeps rights");

	// room.member_joined / updated / presence / status / left.
	data = QJsonObject();
	data.insert(u"member"_q, TestMember(kTestOther, u"Third"_q, false, false));
	result = apply(u"room.member_joined"_q, data);
	check((result & Change::Members) && state.members.size() == 3,
		"member joined");
	result = apply(u"room.member_joined"_q, data);
	check(state.members.size() == 3, "member joined twice is one");

	data = QJsonObject();
	data.insert(u"user_id"_q, double(kTestOther));
	data.insert(u"online"_q, true);
	result = apply(u"room.presence"_q, data);
	check((result & Change::Presence) && state.member(kTestOther)->online,
		"presence");
	check(!apply(u"room.presence"_q, data), "the same presence is nothing");

	auto status = QJsonObject();
	status.insert(u"item_id"_q, u"a1"_q);
	status.insert(u"ready"_q, false);
	status.insert(u"buffered"_q, 140);
	data = QJsonObject();
	data.insert(u"user_id"_q, double(kTestOther));
	data.insert(u"music"_q, status);
	data.insert(u"video"_q, QJsonValue());
	result = apply(u"room.status"_q, data);
	check((result & Change::Status)
		&& state.member(kTestOther)->music.buffered == 100
		&& !state.member(kTestOther)->music.ready, "status clamped");

	auto mine = TestMember(kTestSelf, u"Me"_q, false, true);
	mine.insert(u"rights"_q, TestRights(false));
	data = QJsonObject();
	data.insert(u"member"_q, mine);
	result = apply(u"room.member_updated"_q, data);
	check((result & Change::Rights) && !state.rights.chat
		&& !state.rights.add, "own rights follow member_updated");

	data = QJsonObject();
	data.insert(u"user_id"_q, double(kTestOther));
	data.insert(u"reason"_q, u"left"_q);
	result = apply(u"room.member_left"_q, data);
	check((result & Change::Members) && state.members.size() == 2,
		"member left");

	// The ownership comes to this user.
	data = QJsonObject();
	data.insert(u"owner_id"_q, double(kTestSelf));
	data.insert(u"rev"_q, 9);
	result = apply(u"room.updated"_q, data);
	check((result & Change::Rights) && state.owner
		&& state.rights == Rights::Everything()
		&& state.member(kTestSelf)->owner
		&& !state.member(kTestOwner)->owner, "ownership transfer");

	// An older room.updated that comes after the answer of a later
	// request changes nothing.
	data = QJsonObject();
	data.insert(u"title"_q, u"Old"_q);
	data.insert(u"owner_id"_q, double(kTestOwner));
	data.insert(u"rev"_q, 8);
	result = apply(u"room.updated"_q, data);
	check(!result && state.title == u"Night"_q && state.owner
		&& state.rev == 9, "a stale room.updated is ignored");
	data.insert(u"rev"_q, 9);
	data.insert(u"owner_id"_q, double(kTestSelf));
	data.insert(u"title"_q, u"Same rev"_q);
	result = apply(u"room.updated"_q, data);
	check((result & Change::Room) && state.title == u"Same rev"_q,
		"room.updated with the same rev is taken");

	// room.player: only a newer rev.
	data = QJsonObject();
	data.insert(u"kind"_q, u"music"_q);
	data.insert(
		u"state"_q,
		TestPlayerState(u"b2"_q, true, 0, 1759800005000, 6));
	result = apply(u"room.player"_q, data);
	check((result & Change::MusicPlayer)
		&& state.music.state.itemId == u"b2"_q, "player newer rev");
	data.insert(
		u"state"_q,
		TestPlayerState(u"a1"_q, false, 0, 1759800004000, 6));
	check(!apply(u"room.player"_q, data)
		&& state.music.state.itemId == u"b2"_q, "player same rev ignored");
	data.insert(
		u"state"_q,
		TestPlayerState(u"a1"_q, false, 0, 1759800004000, 4));
	check(!apply(u"room.player"_q, data), "player older rev ignored");
	data.insert(u"kind"_q, u"video"_q);
	data.insert(
		u"state"_q,
		TestPlayerState(u"zz"_q, true, 0, 1759800004000, 3));
	result = apply(u"room.player"_q, data);
	check((result & Change::VideoPlayer)
		&& state.video.state.itemId.isEmpty()
		&& !state.video.state.playing, "unknown item does not play");

	// room.queue: the queue always, the state by rev.
	auto queue = QJsonArray();
	queue.push_back(TestItem(u"b2"_q, 'b', 180000, kTestSelf));
	queue.push_back(TestItem(u"d4"_q, 'd', 100000, kTestOther));
	data = QJsonObject();
	data.insert(u"kind"_q, u"music"_q);
	data.insert(u"queue"_q, queue);
	data.insert(
		u"state"_q,
		TestPlayerState(u"b2"_q, true, 0, 1759800005000, 6));
	result = apply(u"room.queue"_q, data);
	check((result & Change::MusicQueue) && state.music.queue.size() == 2
		&& state.music.current(), "queue with the same rev");
	queue = QJsonArray();
	queue.push_back(TestItem(u"d4"_q, 'd', 100000, kTestOther));
	data.insert(u"queue"_q, queue);
	data.insert(
		u"state"_q,
		TestPlayerState(u"d4"_q, true, 0, 1759800006000, 7));
	result = apply(u"room.queue"_q, data);
	check(state.music.queue.size() == 1
		&& state.music.state.itemId == u"d4"_q
		&& state.music.state.rev == 7, "queue with a newer state");
	data.insert(u"queue"_q, QJsonArray());
	data.insert(
		u"state"_q,
		TestPlayerState(u"d4"_q, true, 0, 0, 2));
	result = apply(u"room.queue"_q, data);
	check((result & Change::MusicQueue) && state.music.queue.empty()
		&& state.music.state.itemId.isEmpty()
		&& !state.music.state.playing
		&& state.music.state.rev == 7,
		"the queue always, an older state never");

	// Chat.
	auto message = QJsonObject();
	message.insert(u"id"_q, 10);
	message.insert(u"user_id"_q, double(kTestOwner));
	message.insert(u"name"_q, u"Owner"_q);
	message.insert(u"text"_q, u"hi"_q);
	message.insert(u"ts"_q, double(1759800001000));
	data = QJsonObject();
	data.insert(u"message"_q, message);
	result = apply(u"room.chat"_q, data);
	check((result & Change::Chat) && state.chat.size() == 1
		&& state.chatLastId == 10, "chat message");
	check(!apply(u"room.chat"_q, data), "chat message twice is one");
	message.insert(u"id"_q, 8);
	data.insert(u"message"_q, message);
	result = apply(u"room.chat"_q, data);
	check(state.chat.size() == 2 && state.chat.front().id == 8,
		"chat sorted by id");
	data = QJsonObject();
	data.insert(u"id"_q, 8);
	result = apply(u"room.chat_deleted"_q, data);
	check((result & Change::Chat) && state.chat.size() == 1,
		"chat deleted");
	for (auto i = 0; i != kChatKeep + 20; ++i) {
		message.insert(u"id"_q, 100 + i);
		data = QJsonObject();
		data.insert(u"message"_q, message);
		[[maybe_unused]] const auto ignored = apply(u"room.chat"_q, data);
	}
	check(int(state.chat.size()) == kChatKeep, "chat capped");

	// Voice, canvas, unknown.
	auto voice = QJsonObject();
	voice.insert(u"link"_q, u"https://t.me/+AbCdEfGh1234"_q);
	data = QJsonObject();
	data.insert(u"voice"_q, voice);
	result = apply(u"room.voice"_q, data);
	check((result & Change::Voice) && state.voice.valid(), "voice set");
	data.insert(u"voice"_q, QJsonValue());
	result = apply(u"room.voice"_q, data);
	check((result & Change::Voice) && !state.voice.valid(), "voice unset");
	auto stroke = QJsonObject();
	stroke.insert(u"seq"_q, 41);
	data = QJsonObject();
	data.insert(u"stroke"_q, stroke);
	result = apply(u"room.stroke"_q, data);
	check((result & Change::Canvas) && state.canvas.seq == 41
		&& state.canvas.count == 1, "stroke counted");
	check(apply(u"room.stroke_removed"_q, {}) == Changes(Change::Canvas)
		&& state.canvas.count == 0, "stroke removed");
	data = QJsonObject();
	data.insert(u"background"_q, u"#101010"_q);
	check(apply(u"room.canvas_cleared"_q, data) == Changes(Change::Canvas)
		&& state.canvas.background == u"#101010"_q, "canvas cleared");
	check(!apply(u"room.stroke_live"_q, {}), "live stroke is not state");
	check(!apply(u"room.reaction"_q, {}), "reaction is not state");
	check(!apply(u"room.something_new"_q, {}), "unknown event ignored");

	// The ends.
	auto copy = state;
	data = QJsonObject();
	data.insert(u"reason"_q, u"owner"_q);
	result = ApplyEvent(copy, TestEvent(u"room.closed"_q, data), kTestSelf);
	check((result & Change::Gone) && copy.gone == Gone::Closed, "closed");
	check(!ApplyEvent(copy, TestEvent(u"room.voice"_q, {}), kTestSelf),
		"nothing after the end");

	copy = state;
	data = QJsonObject();
	data.insert(u"user_id"_q, double(kTestSelf));
	data.insert(u"reason"_q, u"kicked"_q);
	result = ApplyEvent(
		copy,
		TestEvent(u"room.member_left"_q, data),
		kTestSelf);
	check((result & Change::Gone) && copy.gone == Gone::Kicked, "kicked");

	copy = state;
	data = QJsonObject();
	data.insert(u"code"_q, u"K7QM2XPA9Z"_q);
	data.insert(u"reason"_q, u"banned"_q);
	result = ApplyEvent(
		copy,
		TestEvent(u"me.room_removed"_q, data, 0),
		kTestSelf);
	check(copy.gone == Gone::Banned, "banned by me.room_removed");

	copy = state;
	result = ApplyEvent(copy, TestEvent(u"room.rejected"_q, {}, 0), kTestSelf);
	check(copy.gone == Gone::Rejected, "rejected by the stream");

	copy = state;
	data = QJsonObject();
	data.insert(u"reason"_q, u"idle"_q);
	result = ApplyEvent(copy, TestEvent(u"room.closed"_q, data), kTestSelf);
	check(copy.gone == Gone::Idle, "idle");
}

void TestAnswers(Checker &check) {
	auto state = ParseRoom(TestRoom(), kTestSelf);
	auto answer = QJsonObject();
	answer.insert(
		u"state"_q,
		TestPlayerState(u"c3"_q, false, 500, 1759800002000, 6));
	answer.insert(u"queue"_q, QJsonArray());
	auto result = ApplyPlayerAnswer(state, Kind::Music, answer);
	check((result & Change::MusicPlayer)
		&& state.music.state.itemId == u"c3"_q
		&& state.music.queue.size() == 3, "answer: state yes, queue no");
	check(!ApplyPlayerAnswer(state, Kind::Music, answer),
		"answer with the same rev ignored");
	check(!ApplyPlayerAnswer(state, Kind::Music, QJsonObject()),
		"answer without a state ignored");
}

void TestRightsAndQueue(Checker &check) {
	const auto all = Rights::Everything();
	const auto only = Rights::OnlyOwner();
	for (auto i = 0; i != kRightsCount; ++i) {
		check(all.has(Right(i)), "everything has each right");
	}
	check(!only.control && !only.queue && !only.add && only.draw
		&& only.chat && !only.invite, "only owner preset");
	check(ParseRights(SerializeRights(only)) == only, "rights round trip");
	auto rights = Rights();
	rights.set(Right::Invite, true);
	check(rights.invite && rights.has(Right::Invite)
		&& !rights.has(Right::Chat), "set one right");
	check(QLatin1String(RightName(Right::Control)) == u"control"_q
		&& QLatin1String(RightName(Right::Invite)) == u"invite"_q,
		"right names");

	auto state = ParseRoom(TestRoom(), kTestSelf);
	const auto &queue = state.music.queue;
	check(!CanRemoveItem(state, queue[0], kTestSelf),
		"add right: not a foreign item");
	check(CanRemoveItem(state, queue[1], kTestSelf),
		"add right: the own item");
	state.rights.queue = true;
	check(CanRemoveItem(state, queue[0], kTestSelf),
		"queue right: any item");
	state.rights = Rights();
	check(!CanRemoveItem(state, queue[1], kTestSelf),
		"no rights: nothing");

	check(state.music.indexOf(u"b2"_q) == 1
		&& state.music.indexOf(u"zz"_q) == -1
		&& state.music.indexOf(QString()) == -1, "index of");
	check(MoveTarget(queue, 0, 0).first == false, "move to itself");
	check(MoveTarget(queue, 0, 5).first == false, "move out of range");
	check(MoveTarget(queue, 0, 1)
		== std::pair(true, u"c3"_q), "move down by one");
	check(MoveTarget(queue, 0, 2)
		== std::pair(true, QString()), "move to the end");
	check(MoveTarget(queue, 2, 0)
		== std::pair(true, u"a1"_q), "move to the top");
	check(MoveTarget(queue, 2, 1)
		== std::pair(true, u"b2"_q), "move up by one");

	// A row menu: the place is counted by the queue at the click. The
	// queue is a1 (current), b2, c3.
	const auto none = std::pair(-1, -1);
	check(MoveIndexes(state.music, u"a1"_q, QueueMove::Up) == none,
		"the first item does not go up");
	check(MoveIndexes(state.music, u"c3"_q, QueueMove::Down) == none,
		"the last item does not go down");
	check(MoveIndexes(state.music, u"c3"_q, QueueMove::Up)
		== std::pair(2, 1), "up by one");
	check(MoveIndexes(state.music, u"a1"_q, QueueMove::Down)
		== std::pair(0, 1), "down by one");
	check(MoveIndexes(state.music, u"zz"_q, QueueMove::Up) == none,
		"an item that is gone moves nowhere");
	check(MoveIndexes(state.music, u"a1"_q, QueueMove::Next) == none,
		"the current item is not its own next");
	check(MoveIndexes(state.music, u"b2"_q, QueueMove::Next) == none,
		"the next item is next already");
	check(MoveIndexes(state.music, u"c3"_q, QueueMove::Next)
		== std::pair(2, 1), "play next from below");
	{
		// The menu of c3 was opened, then somebody has removed a1 and
		// the room went on to c3: the indexes are those of the new queue.
		auto changed = state.music;
		changed.queue.erase(begin(changed.queue));
		changed.state.itemId = u"c3"_q;
		check(MoveIndexes(changed, u"b2"_q, QueueMove::Next)
			== std::pair(0, 1), "play next from above, after a change");
		check(MoveIndexes(changed, u"c3"_q, QueueMove::Up)
			== std::pair(1, 0), "up after a change");
		check(MoveIndexes(changed, u"b2"_q, QueueMove::Up) == none,
			"nothing above after a change");
		const auto indexes = MoveIndexes(changed, u"b2"_q, QueueMove::Next);
		check(MoveTarget(changed.queue, indexes.first, indexes.second)
			== std::pair(true, QString()), "play next goes after the current");
		changed.state.itemId = QString();
		check(MoveIndexes(changed, u"b2"_q, QueueMove::Next) == none,
			"no current item: no next");
	}

	// One click on a right sends that right only.
	const auto single = SerializeRight(Right::Queue, false);
	check(single.size() == 1 && single.contains(u"queue"_q)
		&& !single.value(u"queue"_q).toBool(true), "one right, one key");
	check(SerializeRight(Right::Invite, true).value(u"invite"_q).toBool(),
		"one right switched on");

	check(NextAfterEnd(state.music) == u"b2"_q, "next after the first");
	state.music.state.itemId = u"c3"_q;
	check(NextAfterEnd(state.music).isEmpty(), "stop after the last");
	state.music.state.repeat = Repeat::All;
	check(NextAfterEnd(state.music) == u"a1"_q, "repeat all wraps");
	state.music.state.repeat = Repeat::One;
	check(NextAfterEnd(state.music) == u"c3"_q, "repeat one stays");
	state.music.state.itemId = QString();
	check(NextAfterEnd(state.music).isEmpty(), "nothing after nothing");
}

void TestLinks(Checker &check) {
	const auto code = u"K7QM2XPA9Z"_q;
	check(ExtractCode(u"  k7qm2xpa9z "_q) == code, "a typed code");
	check(ExtractCode(Cloud::MakeLink(Cloud::LinkKind::Room, code)) == code,
		"a pasted link");
	check(ExtractCode(u"hello"_q).isEmpty(), "not a code");
	check(ExtractCode(QString()).isEmpty(), "empty");
	check(ExtractCode(QString(300, QChar('A'))).isEmpty(), "too long");
	check(ExtractCode(Cloud::MakeLink(
		Cloud::LinkKind::Playlist,
		QString(22, QChar('a')))).isEmpty(), "a playlist link is not a room");
	check(FormatDuration(0) == u"0:00"_q, "0:00");
	check(FormatDuration(187'000) == u"3:07"_q, "3:07");
	check(FormatDuration(3'765'000) == u"1:02:45"_q, "1:02:45");
	check(KindName(Kind::Music) == u"music"_q
		&& KindName(Kind::Video) == u"video"_q, "kind names");
	check(MimeFor(u"a.MP3"_q, Kind::Music) == u"audio/mpeg"_q, "mp3 mime");
	check(MimeFor(u"a.mkv"_q, Kind::Music)
		== u"application/octet-stream"_q, "mkv as music");
	check(MimeFor(u"a.mkv"_q, Kind::Video)
		== u"video/x-matroska"_q, "mkv as video");
	check(SafeExtension(u"x.Mp3"_q) == u".mp3"_q
		&& SafeExtension(u"x.tar/../y"_q).isEmpty()
		&& SafeExtension(u"noext"_q).isEmpty(), "safe extension");
}

void TestFilesAndRetries(Checker &check) {
	// The snapshot is asked for again with a growing pause, a few times.
	check(ReloadRetryDelay(0) == kReloadRetry, "the first retry");
	check(ReloadRetryDelay(1) == 2 * kReloadRetry, "the pause doubles");
	check(ReloadRetryDelay(kReloadAttempts - 1) == kReloadRetryMax,
		"the pause is capped");
	check(ReloadRetryDelay(kReloadAttempts) == 0
		&& ReloadRetryDelay(100) == 0
		&& ReloadRetryDelay(-1) == 0, "the retries end");
	auto total = crl::time(0);
	for (auto i = 0; i != 100; ++i) {
		total += ReloadRetryDelay(i);
	}
	check(total > 60'000 && total < 600'000, "minutes of retries, not more");

	// A change of the queue the server finds too frequent is waited out,
	// a limited number of times. Nothing else of it is ever sent twice.
	const auto queued = QueueRequest(Cloud::PostRequest(u"/queue/music"_q));
	check(queued.rateRetries == kQueueRateRetries
		&& queued.retries < 0
		&& queued.method == "POST"
		&& queued.path == u"/queue/music"_q,
		"too often for the queue: waited out, not an error");
	check(Cloud::PostRequest(u"/chat"_q).rateRetries == 0
		&& Cloud::DeleteRequest(u"/chat/1"_q).rateRetries == 0,
		"only the queue is sent again");

	// The files of the temp folder follow the queues.
	const auto folder = u"/tmp/rooms/1_K7QM2XPA9Z/"_q;
	auto state = ParseRoom(TestRoom(), kTestSelf);
	const auto was = QueueFiles(state, folder);
	check(was.size() == 3
		&& was.contains(folder + TestSha('a') + u".mp3"_q)
		&& was.contains(folder + TestSha('c') + u".mp3"_q),
		"a file for every item");
	check(LeftFiles(was, was).empty(), "nothing has left");

	// b2 is removed, the file of a1 is added to the video queue too.
	state.music.queue.erase(begin(state.music.queue) + 1);
	auto copy = state.music.queue.front();
	copy.id = u"v1"_q;
	copy.kind = Kind::Video;
	state.video.queue.push_back(copy);
	const auto now = QueueFiles(state, folder);
	const auto left = LeftFiles(was, now);
	check(now.size() == 2, "the same file in two queues is one file");
	check(left.size() == 1
		&& left.contains(folder + TestSha('b') + u".mp3"_q)
		&& left.begin()->second == TestSha('b'),
		"only what is in no queue has left");
	check(LeftFiles(now, was).empty(), "an item that came is not left");

	check(InFolder(folder + u"tg_5.audio"_q, folder), "a file of the folder");
	check(!InFolder(folder, folder), "the folder is not its file");
	check(!InFolder(folder + u"sub/file"_q, folder), "nothing deeper");
	check(!InFolder(folder + u".."_q, folder), "not the parent");
	check(!InFolder(u"/home/me/Music/song.mp3"_q, folder),
		"a file of the user is never in the folder");
	check(!InFolder(u"/tmp/rooms/1_K7QM2XPA9Zx/song.mp3"_q, folder),
		"not a folder with a longer name");
	check(!InFolder(u"song.mp3"_q, QString()), "no folder, no file");

	check(RoomFolderName(42, u"K7QM2XPA9Z"_q) == u"42_K7QM2XPA9Z"_q,
		"the name of the temp folder");
}

void TestSampleRoom(Checker &check) {
	auto room = Room(Room::Descriptor{
		.selfId = kTestSelf,
		.state = ParseRoom(TestRoom(), kTestSelf),
		.now = [] { return int64(1759800003000); },
	});
	check(room.sample() && !room.session() && !room.account(),
		"a sample has no session");
	check(room.code() == u"K7QM2XPA9Z"_q, "sample code");
	check(room.position(Kind::Music) == 4000, "sample position");
	check(room.position(Kind::Video) == 0, "sample empty position");
	check(room.can(Right::Add) && !room.can(Right::Control),
		"sample rights");
	check(room.send(Cloud::PostRequest(u"/chat"_q)) == 0,
		"a sample sends nothing");
	check(room.addMedia({ .path = u"/nothing"_q }) == 0,
		"a sample uploads nothing");
	room.play(Kind::Music);
	room.sendChat(u"text"_q);
	check(room.state().chat.empty() && room.uploads().empty(),
		"a sample stays as it is");
	check(!room.music(), "a sample has no engine");
}

} // namespace

bool RunSelfTest(QStringList &log) {
	auto check = Checker(log);
	TestParse(check);
	check.section("parse");
	TestReducer(check);
	check.section("reducer");
	TestAnswers(check);
	check.section("answers");
	TestRightsAndQueue(check);
	check.section("rights and queue");
	TestLinks(check);
	check.section("links and names");
	TestFilesAndRetries(check);
	check.section("files and retries");
	TestSampleRoom(check);
	check.section("sample room");
	log.push_back(u"room: %1 checks, %2 failed"_q.arg(
		QString::number(check.passed() + check.failed()),
		QString::number(check.failed())));
	return !check.failed();
}

} // namespace Oblivion::Rooms
