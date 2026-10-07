/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_cloud_social.h"

#include "base/random.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "data/data_document.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_instance.h"
#include "oblivion/oblivion_badge.h"
#include "oblivion/oblivion_cloud.h"
#include "oblivion/oblivion_settings.h"
#include "settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

#include <mutex>

namespace Oblivion::Social {
namespace {

constexpr auto kCacheVersion = 1;
constexpr auto kMaxCacheFile = 8 * 1024 * 1024;
constexpr auto kMaxBadges = 200'000;
constexpr auto kMaxDirectory = 20'000;
constexpr auto kMaxActivity = 20'000;
constexpr auto kMaxSharedItems = 200;
constexpr auto kMaxDuration = int64(86'400'000);

// No profile has more public playlists or presets than this (the server
// lets a user keep 50 and 100): a larger number is not a number of the
// server and is not believed at all.
constexpr auto kMaxPublicCount = 1000;

// The protocol asks for these periods; with an ETag an unchanged list
// costs one short answer.
constexpr auto kBadgesRefresh = 30 * 60 * crl::time(1000);
constexpr auto kDirectoryRefresh = 10 * 60 * crl::time(1000);
constexpr auto kManualRefreshGap = crl::time(15'000);
constexpr auto kSaveDelay = crl::time(2000);

// The "badges" event comes to every client at the same moment, and
// anybody can make it come by flipping the own badge. The list is asked
// a few seconds later (each client picks its own moment) and not more
// often than once in kBadgesEventGap, however often the event comes.
constexpr auto kBadgesEventGap = crl::time(60'000);
constexpr auto kBadgesEventJitterMin = crl::time(1500);
constexpr auto kBadgesEventJitterMax = crl::time(6000);

// The activity of the others: the server tells the changes by events,
// but an event can be missed (it is not replayed, and the server sends
// the "nothing to show any more" only to those it knows have seen the
// chip). So the whole list is asked again every kActivityRefresh, when a
// profile is opened (not more often than kActivityCardGap), and «слушает»
// the server has not repeated for kListeningTtl is dropped: the server
// itself forgets a track 15 minutes after it was told. The time is the
// local one of the last answer, never the clock of the other person.
constexpr auto kActivityRefresh = 5 * 60 * crl::time(1000);
constexpr auto kActivityCardGap = crl::time(60'000);
constexpr auto kListeningTtl = 16 * 60 * crl::time(1000);

// The own «слушает»: a new track is told after it has played for a
// moment (skipping through an album tells nothing), not more often than
// once in kPublishGap; the same track is told again before the server
// forgets it (15 minutes there). A request that failed is repeated after
// a growing pause and only while that track is still playing.
constexpr auto kPublishSettle = crl::time(2500);
constexpr auto kPublishGap = crl::time(5000);
constexpr auto kPublishRefresh = 10 * 60 * crl::time(1000);
constexpr auto kPublishFailPause = 2 * 60 * crl::time(1000);
constexpr auto kPublishFailMax = 10 * 60 * crl::time(1000);

// A stop («больше не слушает») that did not get through is repeated
// this many times: after the two pauses the server has forgotten the
// track by itself anyway.
constexpr auto kStopRetries = 2;

// How long «Отключиться» and the quit of the app wait for the answer to
// the last stop. Shorter than kQuitPreventTimeoutMs of the app.
constexpr auto kStopTimeout = crl::time(1200);

struct Listening {
	QString title;
	QString performer;
	int64 durationMs = 0;

	friend inline bool operator==(
		const Listening&,
		const Listening&) = default;
};

// Decides when PUT /v1/me/activity is sent. Knows nothing about the
// network or the clock: the times are given, so it is checked by the
// self-test as it is.
class PublishThrottle final {
public:
	struct Step {
		bool send = false;
		std::optional<Listening> value;
		crl::time wait = 0; // 0 without send: nothing to do.
	};

	// What there is to show now (std::nullopt: nothing). Returns whether
	// that is a change.
	bool want(std::optional<Listening> value, crl::time now);
	[[nodiscard]] Step step(crl::time now) const;
	void sent(crl::time now);
	void succeeded();
	void failed(crl::time now, crl::time retryAfter);
	// The event stream of this device was closed and is open again. The
	// server shows «слушает» of a device only while that device has a
	// stream and may have dropped the track meanwhile: what was told is
	// told again now, not when the period is over.
	void reconnected(crl::time now);
	// The account is not connected any more, or the last stop was sent
	// aside from the throttle (Model::startStop): nothing is known about
	// the server and nothing more is sent from here.
	void reset();
	// What the server shows to the audience, as far as it is known.
	[[nodiscard]] const std::optional<Listening> &published() const;
	// Whether the server may be showing a track of this device: it has
	// taken one, or a request is on its way and its answer is not known
	// yet (a stop that is on its way has not taken the track back yet).
	[[nodiscard]] bool told() const;

private:
	std::optional<Listening> _wanted;
	std::optional<Listening> _server; // What the server shows, as known.
	std::optional<Listening> _sending;
	crl::time _wantedSince = 0;
	crl::time _lastSent = 0;
	crl::time _confirmedAt = 0;
	crl::time _blockedTill = 0;
	bool _everSent = false;
	bool _inFlight = false;
	bool _repeat = false;
	int _fails = 0;
	int _stopFails = 0;

};

bool PublishThrottle::want(std::optional<Listening> value, crl::time now) {
	if (_wanted == value) {
		return false;
	}
	_wanted = std::move(value);
	_wantedSince = now;
	return true;
}

PublishThrottle::Step PublishThrottle::step(crl::time now) const {
	const auto changed = (_wanted != _server);
	if (!changed && !_wanted) {
		return {};
	}
	auto at = now;
	if (!changed) {
		// The period is counted from the last time the server has taken
		// the track. A refresh that did not get through is so tried again
		// after the pause of a failure and not after one more period: by
		// then the server would have forgotten a track that still plays.
		at = std::max(
			_confirmedAt + kPublishRefresh,
			_lastSent + kPublishGap);
	} else {
		if (_wanted) {
			at = std::max(at, _wantedSince + kPublishSettle);
		}
		if (_everSent) {
			at = std::max(at, _lastSent + kPublishGap);
		}
	}
	at = std::max(at, _blockedTill);
	if (at <= now) {
		return { .send = true, .value = _wanted };
	}
	return { .wait = at - now };
}

void PublishThrottle::sent(crl::time now) {
	_sending = _wanted;
	_lastSent = now;
	_everSent = true;
	_inFlight = true;
}

void PublishThrottle::succeeded() {
	_server = base::take(_sending);
	_inFlight = false;

	// A request that was on its way while the stream was reconnecting
	// may have reached the server before it dropped the track: such an
	// answer confirms nothing, the track is told once more after the gap.
	_confirmedAt = base::take(_repeat)
		? (_lastSent - kPublishRefresh)
		: _lastSent;
	_fails = 0;
	_stopFails = 0;
}

void PublishThrottle::failed(crl::time now, crl::time retryAfter) {
	_inFlight = false;
	_repeat = false;
	_fails = std::min(_fails + 1, 8);
	const auto pause = std::min(
		kPublishFailPause * (crl::time(1) << std::min(_fails - 1, 3)),
		kPublishFailMax);
	_blockedTill = now + std::max(pause, retryAfter);
	if (base::take(_sending)) {
		_stopFails = 0;
	} else if (++_stopFails > kStopRetries) {
		// A stop is repeated a couple of times and then given up: the
		// server drops the track by itself in a quarter of an hour.
		_server = std::nullopt;
		_stopFails = 0;
	}
}

void PublishThrottle::reconnected(crl::time now) {
	if (_inFlight) {
		_repeat = true;
	} else if (_server) {
		_confirmedAt = now - kPublishRefresh;
	}
}

void PublishThrottle::reset() {
	*this = PublishThrottle();
}

const std::optional<Listening> &PublishThrottle::published() const {
	return _server;
}

bool PublishThrottle::told() const {
	return _server.has_value() || _inFlight;
}

// Whether the last stop («Отключиться», the quit of the app) is sent at
// all: only when the server may be showing a track of this device and
// can be told so right now, which is with an open event stream. Without
// a stream the server does not show the track of the device and drops it
// by itself, and a server that can't be reached is never waited for.
[[nodiscard]] bool LastStopWanted(bool ready, Cloud::State state, bool told) {
	return ready && told && (state == Cloud::State::Online);
}

[[nodiscard]] QJsonValue ListeningToJson(
		const std::optional<Listening> &value) {
	if (!value) {
		return QJsonValue(QJsonValue::Null);
	}
	auto result = QJsonObject();
	result.insert(u"title"_q, value->title);
	result.insert(u"performer"_q, value->performer);
	result.insert(u"duration_ms"_q, double(value->durationMs));
	return result;
}

[[nodiscard]] bool ShouldPublishListening(const Cloud::Me &me) {
	return me.chipListening
		&& (AudienceFromWire(me.activityAudience) != Audience::Nobody);
}

[[nodiscard]] int ClampInt(const QJsonValue &value, int max) {
	return int(std::clamp(Cloud::JsonInt(value), int64(0), int64(max)));
}

// A number of things a person may have: something that is not a number
// between 0 and max is nothing. It is not cut down to max: a profile
// made of a nonsense number alone must not show a block.
[[nodiscard]] int CountOrZero(const QJsonValue &value, int max) {
	const auto number = Cloud::JsonInt(value);
	return (number > 0 && number <= max) ? int(number) : 0;
}

[[nodiscard]] QString DigitsOnly(const QString &text) {
	const auto digits = ranges::all_of(text, [](QChar ch) {
		return (ch.unicode() >= '0') && (ch.unicode() <= '9');
	});
	return digits ? text : QString();
}

// Whether a person sees anything in the text: a status made of spaces,
// of zero-width characters or of the blank letters people fill empty
// names with is no status.
[[nodiscard]] bool HasVisibleText(const QString &text) {
	return ranges::any_of(text, [](QChar ch) {
		const auto category = ch.category();
		const auto code = ch.unicode();
		const auto blank = ch.isSpace()
			|| (category == QChar::Other_Format)
			|| (category == QChar::Other_Control)
			|| (category == QChar::Separator_Space)
			|| (category == QChar::Separator_Line)
			|| (category == QChar::Separator_Paragraph)
			|| (code == 0x115F)
			|| (code == 0x1160)
			|| (code == 0x3164)
			|| (code == 0xFFA0)
			|| (code == 0x2800)
			|| (code >= 0xFE00 && code <= 0xFE0F);
		return !blank;
	});
}

[[nodiscard]] Profile ParseProfile(const QJsonObject &object) {
	auto result = Profile();
	result.id = Cloud::JsonUserId(object.value(u"id"_q));
	if (!result.id) {
		return result;
	}
	result.name = Cloud::JsonText(object.value(u"name"_q), 64);
	result.avatarRev = ClampInt(object.value(u"avatar_rev"_q), 1 << 30);
	result.verified = object.value(u"verified"_q).toBool();
	result.badge = object.value(u"badge"_q).toBool();
	const auto profile = object.value(u"profile"_q).toObject();
	result.statusText = Cloud::JsonText(profile.value(u"status_text"_q), 80);
	if (!HasVisibleText(result.statusText)) {
		result.statusText = QString();
	}
	result.statusEmoji = Cloud::JsonText(
		profile.value(u"status_emoji"_q),
		16);
	if (!HasVisibleText(result.statusEmoji)) {
		result.statusEmoji = QString();
	}
	result.statusEmojiId = DigitsOnly(
		Cloud::JsonText(profile.value(u"status_emoji_id"_q), 20));
	result.accent = Cloud::JsonColor(profile.value(u"accent"_q));
	result.publicPlaylists = CountOrZero(
		object.value(u"public_playlists"_q),
		kMaxPublicCount);
	result.publicPresets = CountOrZero(
		object.value(u"public_presets"_q),
		kMaxPublicCount);
	return result;
}

[[nodiscard]] QJsonObject ProfileToJson(const Profile &value) {
	auto profile = QJsonObject();
	profile.insert(u"status_text"_q, value.statusText);
	profile.insert(u"status_emoji"_q, value.statusEmoji);
	profile.insert(u"status_emoji_id"_q, value.statusEmojiId);
	profile.insert(
		u"accent"_q,
		value.accent ? value.accent->name(QColor::HexRgb) : QString());
	auto result = QJsonObject();
	result.insert(u"id"_q, double(value.id));
	result.insert(u"name"_q, value.name);
	result.insert(u"avatar_rev"_q, value.avatarRev);
	result.insert(u"verified"_q, value.verified);
	result.insert(u"badge"_q, value.badge);
	result.insert(u"profile"_q, profile);
	result.insert(u"public_playlists"_q, value.publicPlaylists);
	result.insert(u"public_presets"_q, value.publicPresets);
	return result;
}

[[nodiscard]] Profile ProfileFromMe(const Cloud::Me &me) {
	auto result = Profile();
	result.id = me.id;
	result.name = me.name;
	result.avatarRev = me.avatarRev;
	result.verified = me.verified;
	result.badge = me.badge;
	result.statusText = me.statusText;
	result.statusEmoji = me.statusEmoji;
	result.statusEmojiId = me.statusEmojiId;
	const auto accent = QColor(me.accent);
	if (!me.accent.isEmpty() && accent.isValid()) {
		result.accent = accent;
	}
	if (!HasVisibleText(result.statusText)) {
		result.statusText = QString();
	}
	if (!HasVisibleText(result.statusEmoji)) {
		result.statusEmoji = QString();
	}
	return result;
}

[[nodiscard]] Activity ParseActivity(const QJsonObject &object) {
	auto result = Activity();
	result.userId = Cloud::JsonUserId(object.value(u"user_id"_q));
	if (!result.userId) {
		return result;
	}
	result.online = object.value(u"online"_q).toBool();
	const auto listening = object.value(u"listening"_q);
	if (listening.isObject()) {
		const auto data = listening.toObject();
		result.title = Cloud::JsonText(data.value(u"title"_q), 128);
		if (!result.title.isEmpty()) {
			result.listening = true;
			result.performer = Cloud::JsonText(
				data.value(u"performer"_q),
				128);
			result.durationMs = std::clamp(
				Cloud::JsonInt(data.value(u"duration_ms"_q)),
				int64(0),
				kMaxDuration);
			result.since = std::max(
				Cloud::JsonInt(data.value(u"since"_q)),
				int64(0));
		}
	}
	const auto room = object.value(u"room"_q);
	if (room.isObject()) {
		const auto data = room.toObject();
		result.inRoom = true;
		result.roomTitle = Cloud::JsonText(data.value(u"title"_q), 80);
		result.roomMembers = ClampInt(data.value(u"members"_q), 1000);
		result.roomCode = Cloud::NormalizeRoomCode(
			data.value(u"code"_q).toString().left(64));
	}
	return result;
}

// {"users": [...]}: the directory. std::nullopt for an answer that is
// not what the protocol promises (the list that is kept stays then).
[[nodiscard]] std::optional<std::vector<Profile>> ParseDirectory(
		const QJsonObject &object,
		uint64 selfId) {
	const auto value = object.value(u"users"_q);
	if (!value.isArray()) {
		return std::nullopt;
	}
	auto result = std::vector<Profile>();
	auto seen = base::flat_set<uint64>();
	for (const auto &entry : value.toArray()) {
		auto profile = ParseProfile(entry.toObject());
		if (!profile.valid()
			|| (profile.id == selfId)
			|| !seen.emplace(profile.id).second) {
			continue;
		}
		result.push_back(std::move(profile));
		if (int(result.size()) >= kMaxDirectory) {
			break;
		}
	}
	return result;
}

// {"ids": [...]}: sorted, without repeats.
[[nodiscard]] std::optional<std::vector<uint64>> ParseBadges(
		const QJsonObject &object) {
	const auto value = object.value(u"ids"_q);
	if (!value.isArray()) {
		return std::nullopt;
	}
	auto result = std::vector<uint64>();
	const auto list = value.toArray();
	result.reserve(std::min(int(list.size()), kMaxBadges));
	for (const auto &entry : list) {
		if (const auto id = Cloud::JsonUserId(entry)) {
			result.push_back(id);
			if (int(result.size()) >= kMaxBadges) {
				break;
			}
		}
	}
	ranges::sort(result);
	result.erase(ranges::unique(result), end(result));
	return result;
}

[[nodiscard]] std::vector<Activity> ParseActivities(
		const QJsonObject &object) {
	auto result = std::vector<Activity>();
	for (const auto &entry : object.value(u"users"_q).toArray()) {
		auto activity = ParseActivity(entry.toObject());
		if (activity.userId && !activity.empty()) {
			result.push_back(std::move(activity));
			if (int(result.size()) >= kMaxActivity) {
				break;
			}
		}
	}
	return result;
}

// ownerId != 0: only what that user owns (GET /v1/playlists also lists
// the playlists the user follows).
//
// "public" of a playlist is the profile. "public" of a preset is the
// gallery, and the profile shows the presets of the gallery, unless the
// preset has a boolean "profile": that is a server which keeps the two
// apart (see SharedItem).
[[nodiscard]] std::vector<SharedItem> ParseShared(
		const QJsonArray &playlists,
		const QJsonArray &presets,
		uint64 ownerId) {
	auto result = std::vector<SharedItem>();
	const auto owned = [&](const QJsonObject &object) {
		return !ownerId
			|| (Cloud::JsonUserId(
				object.value(u"owner"_q).toObject().value(
					u"id"_q)) == ownerId);
	};
	const auto full = [&] {
		return int(result.size()) >= kMaxSharedItems;
	};
	for (const auto &entry : playlists) {
		const auto object = entry.toObject();
		const auto id = object.value(u"id"_q).toString();
		if (full()) {
			break;
		} else if (!Cloud::ValidShareId(id) || !owned(object)) {
			continue;
		}
		result.push_back({
			.id = id,
			.playlist = true,
			.title = Cloud::JsonText(object.value(u"title"_q), 128),
			.count = ClampInt(object.value(u"track_count"_q), 100'000),
			.shown = object.value(u"public"_q).toBool(),
			.profileSwitch = true,
		});
	}
	for (const auto &entry : presets) {
		const auto object = entry.toObject();
		const auto id = object.value(u"id"_q).toString();
		if (full()) {
			break;
		} else if (!Cloud::ValidShareId(id) || !owned(object)) {
			continue;
		}
		const auto video = (object.value(u"kind"_q).toString()
			== u"video"_q);
		const auto listed = object.value(u"public"_q).toBool();
		const auto profile = object.value(u"profile"_q);
		result.push_back({
			.id = id,
			.playlist = false,
			.title = Cloud::JsonText(object.value(u"title"_q), 64),
			.kind = video ? u"video"_q : u"photo"_q,
			.shown = profile.isBool() ? profile.toBool() : listed,
			.listed = listed,
			.profileSwitch = profile.isBool(),
		});
	}
	return result;
}

// What is sent for a switch of a shared item. std::nullopt: nothing may
// be sent for it.
struct SharedPatch {
	QString path;
	QJsonObject body;
};

// «Показывать в профиле». A playlist: its "public". A preset: only the
// flag of the profile, and only where the server has one. "public" of a
// preset is the gallery for everybody and is never sent from here.
[[nodiscard]] std::optional<SharedPatch> ShownPatch(
		const SharedItem &item,
		bool shown) {
	if (!Cloud::ValidShareId(item.id)
		|| (!item.playlist && !item.profileSwitch)) {
		return std::nullopt;
	}
	auto result = SharedPatch{
		.path = (item.playlist ? u"/v1/playlists/"_q : u"/v1/presets/"_q)
			+ item.id,
	};
	result.body.insert(item.playlist ? u"public"_q : u"profile"_q, shown);
	return result;
}

// «Показывать всем в «Общих наборах»»: presets only.
[[nodiscard]] std::optional<SharedPatch> ListedPatch(
		const SharedItem &item,
		bool listed) {
	if (!Cloud::ValidShareId(item.id) || item.playlist) {
		return std::nullopt;
	}
	auto result = SharedPatch{ .path = u"/v1/presets/"_q + item.id };
	result.body.insert(u"public"_q, listed);
	return result;
}

// The item after a PATCH, from the answer {"playlist": ...} or
// {"preset": ...}. When the answer is not what the protocol promises the
// item is returned as it was asked to become.
[[nodiscard]] SharedItem PatchedItem(
		const QJsonObject &answer,
		SharedItem expected) {
	auto list = QJsonArray();
	list.push_back(answer.value(
		expected.playlist ? u"playlist"_q : u"preset"_q));
	const auto parsed = expected.playlist
		? ParseShared(list, QJsonArray(), 0)
		: ParseShared(QJsonArray(), list, 0);
	if (parsed.size() != 1 || parsed.front().id != expected.id) {
		return expected;
	}
	expected.shown = parsed.front().shown;
	expected.listed = parsed.front().listed;
	expected.profileSwitch = parsed.front().profileSwitch;
	return expected;
}

// What the others are doing, as the server has told it. Knows nothing
// about the network or the clock (the times are given), so the self-test
// checks it as it is.
//
// Two things keep it true when an event is missed or comes at a wrong
// moment: an event that arrives while the whole list is being asked wins
// over that list (the list was made before it), and «слушает» the server
// has not repeated for kListeningTtl is dropped.
class ActivityBook final {
public:
	// An "activity" event. Returns whether what is shown has changed.
	bool apply(Activity &&activity, crl::time now);
	// GET /v1/activity was sent / has failed / was answered.
	void requested();
	void requestFailed();
	bool replace(std::vector<Activity> &&list, crl::time now);
	bool expire(crl::time now);
	// When expire() has something to do, 0: never.
	[[nodiscard]] crl::time nextExpiry() const;
	bool clear();

	[[nodiscard]] Activity find(uint64 id) const;
	[[nodiscard]] int size() const;

private:
	struct Entry {
		Activity data;
		crl::time toldAt = 0;
	};

	base::flat_map<uint64, Entry> _entries;
	base::flat_set<uint64> _touched;
	bool _requested = false;

};

bool ActivityBook::apply(Activity &&activity, crl::time now) {
	const auto id = activity.userId;
	if (!id) {
		return false;
	}
	if (_requested) {
		_touched.emplace(id);
	}
	const auto i = _entries.find(id);
	if (activity.empty()) {
		if (i == end(_entries)) {
			return false;
		}
		_entries.erase(i);
		return true;
	} else if (i != end(_entries)) {
		i->second.toldAt = now;
		if (i->second.data == activity) {
			return false;
		}
		i->second.data = std::move(activity);
		return true;
	} else if (int(_entries.size()) >= kMaxActivity) {
		return false;
	}
	_entries.emplace(id, Entry{ std::move(activity), now });
	return true;
}

void ActivityBook::requested() {
	_requested = true;
	_touched.clear();
}

void ActivityBook::requestFailed() {
	_requested = false;
	_touched.clear();
}

bool ActivityBook::replace(std::vector<Activity> &&list, crl::time now) {
	auto entries = base::flat_map<uint64, Entry>();
	for (auto &activity : list) {
		const auto id = activity.userId;
		if (id
			&& !activity.empty()
			&& !_touched.contains(id)
			&& (int(entries.size()) < kMaxActivity)) {
			entries.emplace(id, Entry{ std::move(activity), now });
		}
	}
	for (const auto id : _touched) {
		const auto i = _entries.find(id);
		if (i != end(_entries)) {
			entries.emplace(id, i->second);
		}
	}
	_requested = false;
	_touched.clear();
	const auto same = (entries.size() == _entries.size())
		&& ranges::all_of(entries, [&](const auto &pair) {
			const auto i = _entries.find(pair.first);
			return (i != end(_entries))
				&& (i->second.data == pair.second.data);
		});
	_entries = std::move(entries);
	return !same;
}

bool ActivityBook::expire(crl::time now) {
	auto changed = false;
	for (auto i = begin(_entries); i != end(_entries);) {
		auto &data = i->second.data;
		if (!data.listening || (now - i->second.toldAt < kListeningTtl)) {
			++i;
			continue;
		}
		changed = true;
		data.listening = false;
		data.title = data.performer = QString();
		data.durationMs = data.since = 0;
		if (data.empty()) {
			i = _entries.erase(i);
		} else {
			++i;
		}
	}
	return changed;
}

crl::time ActivityBook::nextExpiry() const {
	auto result = crl::time(0);
	for (const auto &[id, entry] : _entries) {
		if (entry.data.listening) {
			const auto at = entry.toldAt + kListeningTtl;
			result = result ? std::min(result, at) : at;
		}
	}
	return result;
}

bool ActivityBook::clear() {
	_requested = false;
	_touched.clear();
	if (_entries.empty()) {
		return false;
	}
	_entries.clear();
	return true;
}

Activity ActivityBook::find(uint64 id) const {
	const auto i = _entries.find(id);
	return (i != end(_entries)) ? i->second.data : Activity();
}

int ActivityBook::size() const {
	return int(_entries.size());
}

// How long after a "badges" event the list is asked: the jitter of this
// client, and the rest of kBadgesEventGap since the list was last asked
// (asked == 0: it was not asked in this launch yet).
[[nodiscard]] crl::time BadgesEventDelay(
		crl::time asked,
		crl::time now,
		crl::time jitter) {
	const auto rest = asked
		? std::max(asked + kBadgesEventGap - now, crl::time(0))
		: crl::time(0);
	return rest + std::clamp(
		jitter,
		kBadgesEventJitterMin,
		kBadgesEventJitterMax);
}

// Whether the server shows anything of this account to other people:
// the badge, the profile, or a chip to somebody. It goes on showing that
// when the account is switched off on this device.
[[nodiscard]] bool LeftPublic(const Cloud::Me &me) {
	const auto audience = [](const QString &value) {
		return AudienceFromWire(value) != Audience::Nobody;
	};
	return me.valid()
		&& (me.badge
			|| audience(me.profileAudience)
			|| (audience(me.activityAudience)
				&& (me.chipListening || me.chipRoom || me.chipOnline)));
}

// What is kept on disk for one account.
struct Cache {
	QByteArray badgesEtag;
	std::vector<uint64> badges; // Sorted.
	QByteArray directoryEtag;
	std::vector<Profile> directory;

	[[nodiscard]] bool empty() const {
		return badges.empty()
			&& directory.empty()
			&& badgesEtag.isEmpty()
			&& directoryEtag.isEmpty();
	}
};

// Nothing to keep is an empty array: the file is removed then.
[[nodiscard]] QByteArray SerializeCache(const Cache &cache) {
	if (cache.empty()) {
		return QByteArray();
	}
	auto ids = QJsonArray();
	for (const auto id : cache.badges) {
		ids.push_back(double(id));
	}
	auto users = QJsonArray();
	for (const auto &profile : cache.directory) {
		users.push_back(ProfileToJson(profile));
	}
	auto badges = QJsonObject();
	badges.insert(u"etag"_q, QString::fromLatin1(cache.badgesEtag));
	badges.insert(u"ids"_q, ids);
	auto directory = QJsonObject();
	directory.insert(u"etag"_q, QString::fromLatin1(cache.directoryEtag));
	directory.insert(u"users"_q, users);
	auto object = QJsonObject();
	object.insert(u"version"_q, kCacheVersion);
	object.insert(u"badges"_q, badges);
	object.insert(u"directory"_q, directory);
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

// A part that can't be read comes back empty together with its ETag, so
// the next request asks for the whole list again.
[[nodiscard]] Cache ParseCache(const QByteArray &bytes, uint64 selfId) {
	auto result = Cache();
	if (bytes.isEmpty()) {
		return result;
	}
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(bytes, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return result;
	}
	const auto object = document.object();
	if (object.value(u"version"_q).toInt() != kCacheVersion) {
		return result;
	}
	const auto etag = [](const QJsonObject &part) {
		const auto value = part.value(u"etag"_q).toString();
		return (value.size() <= 128) ? value.toLatin1() : QByteArray();
	};
	const auto badges = object.value(u"badges"_q).toObject();
	if (auto ids = ParseBadges(badges)) {
		result.badges = std::move(*ids);
		result.badgesEtag = etag(badges);
	}
	const auto directory = object.value(u"directory"_q).toObject();
	if (auto users = ParseDirectory(directory, selfId)) {
		result.directory = std::move(*users);
		result.directoryEtag = etag(directory);
	}
	return result;
}

// A write made later must never be replaced by one that was scheduled
// earlier and ran after it: each of them takes a number on the main
// thread and only the latest for its file touches the disk.
struct Disk {
	std::mutex mutex;
	base::flat_map<QString, uint64> generations;
};

[[nodiscard]] Disk &DiskState() {
	static const auto result = new Disk();
	return *result;
}

[[nodiscard]] uint64 NextGeneration(const QString &path) {
	auto &disk = DiskState();
	const auto lock = std::unique_lock(disk.mutex);
	return ++disk.generations[path];
}

[[nodiscard]] QByteArray ReadBytes(const QString &path) {
	const auto lock = std::unique_lock(DiskState().mutex);
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxCacheFile) {
		return QByteArray();
	}
	return file.readAll();
}

void WriteBytes(
		const QString &path,
		const QByteArray &bytes,
		uint64 generation) {
	auto &disk = DiskState();
	const auto lock = std::unique_lock(disk.mutex);
	const auto i = disk.generations.find(path);
	if (i == end(disk.generations) || i->second != generation) {
		return;
	} else if (bytes.isEmpty()) {
		QFile::remove(path);
		return;
	}
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QSaveFile(path);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(bytes);
		file.commit();
	}
}

// The folder of the account, the one Oblivion Cloud keeps its key in: it
// is removed as a whole when the account logs out.
[[nodiscard]] QString CachePath(not_null<Main::Session*> session) {
	return cWorkingDir()
		+ u"tdata/oblivion/"_q
		+ (session->isTestMode() ? u"test_"_q : QString())
		+ QString::number(session->userId().bare)
		+ u"/cloud_social.json"_q;
}

[[nodiscard]] bool EnabledState(Cloud::State state) {
	return (state != Cloud::State::NoConsent)
		&& (state != Cloud::State::Disconnected);
}

[[nodiscard]] bool FlagOf(const Cloud::Me &me, Flag flag) {
	switch (flag) {
	case Flag::Badge: return me.badge;
	case Flag::ChipListening: return me.chipListening;
	case Flag::ChipRoom: return me.chipRoom;
	case Flag::ChipOnline: return me.chipOnline;
	}
	Unexpected("Flag in Oblivion::Social::FlagOf.");
}

// The social data of one account.
class Model final : public base::has_weak_ptr {
public:
	explicit Model(not_null<Main::Session*> session);
	~Model();

	[[nodiscard]] bool badgeListed(uint64 id) const;
	[[nodiscard]] std::optional<Profile> profile(uint64 id) const;
	[[nodiscard]] Activity activity(uint64 id) const;
	[[nodiscard]] std::vector<Profile> directory() const;
	[[nodiscard]] Status status() const;
	[[nodiscard]] rpl::producer<> changes() const;
	[[nodiscard]] Cloud::Account *account() const;

	void refresh();
	void refreshActivity();
	void activityHeard(uint64 userId, const QJsonValue &value);
	void forget();

	void stopPublishing(Fn<void()> done);
	[[nodiscard]] bool quitPrevent();

private:
	enum class QuitStop {
		None,
		Sending,
		Done,
	};

	void cacheLoaded(Cache &&cache);
	void stateChanged(Cloud::State state);
	void readyChanged(bool ready);
	void settingsChanged();
	void meChanged();
	void handleEvent(const Cloud::Event &event);
	void checkLists();
	void badgesEvent(int64 version);
	void requestBadges();
	void requestDirectory();
	void requestActivity();
	void applyActivity(Activity &&activity);
	void clearActivity();
	void expireActivity();
	void scheduleExpiry();
	void rebuildIndex();
	void scheduleChanges();
	void saveSoon();
	void save();
	void wipe();

	void publishCheck();
	void publishStep();
	[[nodiscard]] bool startStop();
	void holdPublishing();
	[[nodiscard]] bool stopRunning() const;
	void stopFinished();
	void flushStopCallbacks();
	[[nodiscard]] std::optional<Listening> currentListening() const;
	[[nodiscard]] Activity ownActivity() const;

	const not_null<Main::Session*> _session;
	const uint64 _selfId = 0;
	const QString _path;
	const base::weak_ptr<Cloud::Account> _weak;
	Cloud::Sender _cloud;

	Cache _cache;
	base::flat_set<uint64> _badges;
	base::flat_map<uint64, int> _directoryIndex;
	ActivityBook _activity;

	Cloud::RequestId _badgesRequest = 0;
	Cloud::RequestId _directoryRequest = 0;
	Cloud::RequestId _activityRequest = 0;
	Cloud::RequestId _publishRequest = 0;
	Cloud::RequestId _stopRequest = 0;
	crl::time _badgesChecked = 0;
	crl::time _badgesAsked = 0;
	int64 _badgesVersion = -1;
	crl::time _directoryChecked = 0;
	crl::time _activityAsked = 0;
	crl::time _refreshed = 0;
	bool _directoryLoaded = false;
	bool _directoryFailed = false;
	bool _activityAgain = false;

	bool _enabled = false;
	bool _ready = false;
	bool _cacheReady = false;
	bool _showBadges = false;
	bool _showProfiles = false;
	bool _ownBadge = false;
	bool _dirty = false;
	bool _forgotten = false;
	bool _changesScheduled = false;
	bool _publishAllowed = false;

	// The account is on its way out («Отключиться», the quit): the last
	// stop is on its way, was sent or had nothing to take back. Nothing
	// more is told till the account is connected anew (or, after a click
	// that did not switch it off after all, till the wait is over).
	bool _stopping = false;
	QuitStop _quitStop = QuitStop::None;
	std::vector<Fn<void()>> _stopCallbacks;

	PublishThrottle _throttle;

	base::Timer _badgesTimer;
	base::Timer _badgesEventTimer;
	base::Timer _directoryTimer;
	base::Timer _activityTimer;
	base::Timer _expireTimer;
	base::Timer _saveTimer;
	base::Timer _publishTimer;
	base::Timer _stopTimer;
	rpl::event_stream<> _changes;
	rpl::lifetime _lifetime;

};

// Never destroyed: the models leave it together with their sessions.
struct Registry {
	base::flat_map<not_null<Main::Session*>, std::unique_ptr<Model>> models;
};

[[nodiscard]] Registry &Global() {
	static const auto result = new Registry();
	return *result;
}

[[nodiscard]] Model *Lookup(not_null<Main::Session*> session) {
	const auto &models = Global().models;
	const auto i = models.find(session);
	return (i != end(models)) ? i->second.get() : nullptr;
}

Model::Model(not_null<Main::Session*> session)
: _session(session)
, _selfId(session->userId().bare)
, _path(CachePath(session))
, _weak(base::make_weak(&Cloud::For(session)))
, _cloud(&Cloud::For(session))
, _showBadges(Get().cloudBadgeShow())
, _showProfiles(Get().cloudProfileShow() || Get().cloudFriends())
, _badgesTimer([=] { requestBadges(); })
, _badgesEventTimer([=] { requestBadges(); })
, _directoryTimer([=] { requestDirectory(); })
, _activityTimer([=] { requestActivity(); })
, _expireTimer([=] { expireActivity(); })
, _saveTimer([=] { save(); })
, _publishTimer([=] { publishStep(); })
, _stopTimer([=] { stopFinished(); }) {
	const auto account = &Cloud::For(session);
	_enabled = EnabledState(account->state());
	_ownBadge = account->me().badge;
	if (_enabled) {
		// Read off the main thread: the directory may be large.
		crl::async([
			path = _path,
			selfId = _selfId,
			weak = base::make_weak(this)
		] {
			const auto cache = std::make_shared<Cache>(
				ParseCache(ReadBytes(path), selfId));
			crl::on_main(weak, [=] {
				if (const auto strong = weak.get()) {
					strong->cacheLoaded(std::move(*cache));
				}
			});
		});
	} else {
		_cacheReady = true;
	}

	account->stateValue(
	) | rpl::on_next([=](Cloud::State state) {
		stateChanged(state);
	}, _lifetime);

	account->readyValue(
	) | rpl::on_next([=](bool ready) {
		readyChanged(ready);
	}, _lifetime);

	account->events(
	) | rpl::on_next([=](const Cloud::Event &event) {
		handleEvent(event);
	}, _lifetime);

	account->meUpdated(
	) | rpl::on_next([=] {
		meChanged();
	}, _lifetime);

	Get().changes(
	) | rpl::on_next([=] {
		settingsChanged();
	}, _lifetime);

	using SongType = AudioMsgId::Type;
	const auto player = Media::Player::instance();
	rpl::merge(
		player->updatedNotifier(
		) | rpl::filter([](const Media::Player::TrackState &state) {
			return (state.id.type() == SongType::Song);
		}) | rpl::to_empty,
		player->trackChanged(
		) | rpl::filter([](SongType type) {
			return (type == SongType::Song);
		}) | rpl::to_empty,
		player->stops(SongType::Song)
	) | rpl::on_next([=] {
		if (_publishAllowed || _publishRequest) {
			publishCheck();
		}
	}, _lifetime);
}

Model::~Model() {
	_lifetime.destroy();
	_saveTimer.cancel();
	_stopTimer.cancel();
	_cloud.cancelAll();

	// «Отключиться» was clicked a moment ago and waits for the answer to
	// the last stop: the account is still there and is switched off now.
	flushStopCallbacks();
	if (_dirty && !_forgotten) {
		WriteBytes(_path, SerializeCache(_cache), NextGeneration(_path));
	}
}

Cloud::Account *Model::account() const {
	return _forgotten ? nullptr : _weak.get();
}

// The own badge and the own profile are what the server has about the
// account, also while the account is switched off: other people go on
// seeing them. (After «Удалить мои данные» and before the consent there
// is nothing: me() is empty.)
bool Model::badgeListed(uint64 id) const {
	if (id == _selfId) {
		const auto strong = account();
		return strong && strong->me().valid() && strong->me().badge;
	}
	return _enabled && _showBadges && _badges.contains(id);
}

std::optional<Profile> Model::profile(uint64 id) const {
	if (id == _selfId) {
		const auto strong = account();
		return (strong && strong->me().valid())
			? ProfileFromMe(strong->me())
			: std::optional<Profile>();
	} else if (!_enabled) {
		return std::nullopt;
	}
	const auto i = _directoryIndex.find(id);
	return (i != end(_directoryIndex))
		? _cache.directory[i->second]
		: std::optional<Profile>();
}

Activity Model::activity(uint64 id) const {
	if (!_enabled) {
		return Activity();
	} else if (id == _selfId) {
		return ownActivity();
	}
	return _activity.find(id);
}

// The server does not send the own activity back: what the audience sees
// of this account is put together here, from what was really published.
// (The «в комнате» chip is made by the server and is not known here.)
Activity Model::ownActivity() const {
	auto result = Activity{ .userId = _selfId };
	const auto strong = account();
	if (!strong || !_ready) {
		return result;
	}
	const auto &me = strong->me();
	if (AudienceFromWire(me.activityAudience) == Audience::Nobody
		|| (strong->state() != Cloud::State::Online)) {
		// Without the event stream the server shows no chip of the device.
		return result;
	}
	result.online = me.chipOnline;
	if (const auto &track = _throttle.published(); track && me.chipListening) {
		result.listening = true;
		result.title = track->title;
		result.performer = track->performer;
		result.durationMs = track->durationMs;
	}
	return result;
}

std::vector<Profile> Model::directory() const {
	return _enabled ? _cache.directory : std::vector<Profile>();
}

Status Model::status() const {
	const auto strong = account();
	if (!strong || !_enabled) {
		return Status::Off;
	}
	switch (strong->state()) {
	case Cloud::State::NoConsent:
	case Cloud::State::Disconnected:
	case Cloud::State::NeedsLink:
	case Cloud::State::UpgradeRequired:
	case Cloud::State::Banned: return Status::Off;
	case Cloud::State::Offline: return Status::Offline;
	case Cloud::State::Connecting:
	case Cloud::State::Online: break;
	}
	return _directoryFailed
		? Status::Offline
		: _directoryLoaded
		? Status::Ready
		: Status::Loading;
}

rpl::producer<> Model::changes() const {
	return _changes.events();
}

void Model::cacheLoaded(Cache &&cache) {
	_cacheReady = true;
	if (!_enabled) {
		// The account was disconnected while the file was being read.
		wipe();
		return;
	}

	// An answer of the server that came first is newer than the file.
	if (_cache.badgesEtag.isEmpty() && _cache.badges.empty()) {
		_cache.badgesEtag = std::move(cache.badgesEtag);
		_cache.badges = std::move(cache.badges);
	}
	if (_cache.directoryEtag.isEmpty() && _cache.directory.empty()) {
		_cache.directoryEtag = std::move(cache.directoryEtag);
		_cache.directory = std::move(cache.directory);
	}
	rebuildIndex();
	Badge::Refresh();
	scheduleChanges();
	checkLists();
}

void Model::stateChanged(Cloud::State state) {
	const auto enabled = EnabledState(state);
	if (state == Cloud::State::NoConsent) {
		// Never agreed, or «Удалить мои данные с сервера»: nothing that
		// came from the server is kept on this device either.
		wipe();
	} else if (state == Cloud::State::Offline) {
		// What people are doing right now is not known any more.
		clearActivity();
	}
	if (_enabled != enabled) {
		_enabled = enabled;
		if (!enabled) {
			clearActivity();
		}
		Badge::Refresh();
	}
	scheduleChanges();
}

void Model::readyChanged(bool ready) {
	if (_ready == ready) {
		return;
	}
	_ready = ready;
	if (ready) {
		_directoryFailed = false;
		_badgesTimer.callEach(kBadgesRefresh);
		_directoryTimer.callEach(kDirectoryRefresh);
		_activityTimer.callEach(kActivityRefresh);
		checkLists();
		requestActivity();
	} else {
		// The account has cancelled its requests itself, nothing of them
		// comes back. The last stop is among them: whoever waits for its
		// answer is let go now, from the event loop.
		_cloud.cancelAll();
		_badgesTimer.cancel();
		_badgesEventTimer.cancel();
		_directoryTimer.cancel();
		_activityTimer.cancel();
		_publishTimer.cancel();
		_badgesRequest = _directoryRequest = 0;
		_activityRequest = _publishRequest = _stopRequest = 0;
		_activityAgain = false;
		_stopping = false;
		_throttle.reset();
		if (_stopTimer.isActive()) {
			_stopTimer.callOnce(crl::time(0));
		}
		clearActivity();
	}
	publishCheck();
	scheduleChanges();
}

void Model::settingsChanged() {
	const auto badges = Get().cloudBadgeShow();
	const auto profiles = Get().cloudProfileShow() || Get().cloudFriends();
	if (_showBadges != badges) {
		_showBadges = badges;
		Badge::Refresh();
	}
	const auto shown = !_showProfiles && profiles;
	_showProfiles = profiles;
	checkLists();
	if (shown) {
		// Nothing was asked while it was off: what is kept is old.
		requestActivity();
	}
}

// The own mark next to the own name follows the badge of the account:
// the list of the badges of the others is not asked for it.
void Model::meChanged() {
	const auto strong = account();
	const auto badge = strong && strong->me().valid() && strong->me().badge;
	if (_ownBadge != badge) {
		_ownBadge = badge;
		Badge::Refresh();
	}
	publishCheck();
	scheduleChanges();
}

void Model::handleEvent(const Cloud::Event &event) {
	if (event.type == u"activity"_q) {
		applyActivity(ParseActivity(event.data));
	} else if (event.type == u"badges"_q) {
		badgesEvent(Cloud::JsonInt(event.data.value(u"version"_q), -1));
	} else if (event.type == u"hello"_q) {
		// The stream is open again: what was missed is asked as a whole,
		// and the own track is told again (the server keeps it only for
		// a device with a stream and may have dropped it meanwhile).
		_directoryFailed = false;
		requestActivity();
		checkLists();
		_throttle.reconnected(crl::now());
		publishStep();
	} else if (event.type == u"resync"_q
		&& event.data.value(u"scope"_q).toString() == u"me"_q) {
		requestActivity();
		requestBadges();
		requestDirectory();
	}
}

// The lists are asked when they are wanted and older than their period
// (or were never asked in this launch).
void Model::checkLists() {
	if (!_ready || !_cacheReady) {
		return;
	}
	const auto now = crl::now();
	if (!_badgesChecked || (now - _badgesChecked >= kBadgesRefresh)) {
		requestBadges();
	}
	if (!_directoryChecked
		|| (now - _directoryChecked >= kDirectoryRefresh)) {
		requestDirectory();
	}
}

// The server says the badge list has changed. Every client hears it at
// the same moment and anybody can make the server say it again and
// again: the list is asked once, a little later, and then not before
// kBadgesEventGap has passed, however many events come meanwhile.
void Model::badgesEvent(int64 version) {
	if (!_ready
		|| !_showBadges
		|| _badgesEventTimer.isActive()
		|| (version >= 0 && version == _badgesVersion)) {
		return;
	}
	const auto spread = int(kBadgesEventJitterMax - kBadgesEventJitterMin);
	_badgesEventTimer.callOnce(BadgesEventDelay(
		_badgesAsked,
		crl::now(),
		kBadgesEventJitterMin + crl::time(base::RandomIndex(spread + 1))));
}

void Model::requestBadges() {
	if (_badgesRequest || !_ready || !_cacheReady || !_showBadges) {
		return;
	}
	auto request = Cloud::GetRequest(u"/v1/badges"_q);
	if (!_cache.badgesEtag.isEmpty()) {
		request.headers.push_back({ "If-None-Match", _cache.badgesEtag });
	}
	_badgesEventTimer.cancel();
	_badgesAsked = crl::now();
	_badgesRequest = _cloud.request(std::move(request), [=](
			const Cloud::Response &response) {
		_badgesRequest = 0;
		_badgesChecked = crl::now();
		if (response.status == 304) {
			return;
		}
		auto ids = ParseBadges(response.json);
		if (!ids) {
			return;
		}
		_badgesVersion = Cloud::JsonInt(
			response.json.value(u"version"_q),
			-1);
		const auto etag = response.etag.left(128);
		if (_cache.badges != *ids) {
			_cache.badgesEtag = etag;
			_cache.badges = std::move(*ids);
			rebuildIndex();
			Badge::Refresh();
			saveSoon();
		} else if (_cache.badgesEtag != etag) {
			// The same people under a new version (somebody has switched
			// the badge on and off): the file is not written for that, the
			// new ETag goes to the disk with the next change.
			_cache.badgesEtag = etag;
			_dirty = true;
		}
	}, [=](const Cloud::Error &error) {
		_badgesRequest = 0;
	});
}

void Model::requestDirectory() {
	if (_directoryRequest || !_ready || !_cacheReady || !_showProfiles) {
		return;
	}
	auto request = Cloud::GetRequest(u"/v1/directory"_q);
	if (!_cache.directoryEtag.isEmpty()) {
		request.headers.push_back({
			"If-None-Match",
			_cache.directoryEtag,
		});
	}
	_directoryRequest = _cloud.request(std::move(request), [=](
			const Cloud::Response &response) {
		_directoryRequest = 0;
		_directoryChecked = crl::now();
		_directoryFailed = false;
		_directoryLoaded = true;
		if (response.status != 304) {
			if (auto users = ParseDirectory(response.json, _selfId)) {
				const auto etag = response.etag.left(128);
				if (_cache.directory != *users) {
					_cache.directoryEtag = etag;
					_cache.directory = std::move(*users);
					rebuildIndex();
					saveSoon();
				} else if (_cache.directoryEtag != etag) {
					_cache.directoryEtag = etag;
					_dirty = true;
				}
			}
		}
		scheduleChanges();
	}, [=](const Cloud::Error &error) {
		_directoryRequest = 0;
		_directoryFailed = true;
		scheduleChanges();
	});
}

// The whole list of what the others are doing. Asked after every connect
// of the stream, every kActivityRefresh, when «Друзья в Oblivion» or a
// profile is opened. A request that is wanted while one is on its way
// (the stream has just opened and the answer may be from before it) is
// sent once more after that answer: once, not in a loop.
void Model::requestActivity() {
	if (!_ready || !_showProfiles) {
		return;
	} else if (_activityRequest) {
		_activityAgain = true;
		return;
	}
	_activityAgain = false;
	_activityAsked = crl::now();
	_activity.requested();
	_activityRequest = _cloud.request(
		Cloud::GetRequest(u"/v1/activity"_q),
		[=](const Cloud::Response &response) {
			_activityRequest = 0;
			const auto changed = _activity.replace(
				ParseActivities(response.json),
				crl::now());
			if (changed) {
				scheduleChanges();
			}
			scheduleExpiry();
			if (base::take(_activityAgain)) {
				requestActivity();
			}
		},
		[=](const Cloud::Error &error) {
			_activityRequest = 0;
			_activityAgain = false;
			_activity.requestFailed();
		});
	if (!_activityRequest) {
		_activity.requestFailed();
	}
}

void Model::applyActivity(Activity &&activity) {
	if (!_enabled) {
		return;
	} else if (_activity.apply(std::move(activity), crl::now())) {
		scheduleChanges();
	}
	scheduleExpiry();
}

// "activity" of GET /v1/users/{id}: what that person is doing right now,
// or null. Anything else than an activity of that very person means
// there is nothing to show.
void Model::activityHeard(uint64 userId, const QJsonValue &value) {
	if (!userId || (userId == _selfId)) {
		return;
	}
	auto activity = value.isObject()
		? ParseActivity(value.toObject())
		: Activity();
	if (activity.userId != userId) {
		activity = Activity{ .userId = userId };
	}
	applyActivity(std::move(activity));
}

void Model::clearActivity() {
	_expireTimer.cancel();
	if (_activity.clear()) {
		scheduleChanges();
	}
}

void Model::expireActivity() {
	if (_activity.expire(crl::now())) {
		scheduleChanges();
	}
	scheduleExpiry();
}

void Model::scheduleExpiry() {
	const auto at = _activity.nextExpiry();
	if (!at) {
		_expireTimer.cancel();
		return;
	}
	_expireTimer.callOnce(std::max(at - crl::now(), crl::time(1)));
}

void Model::rebuildIndex() {
	_badges = base::flat_set<uint64>(
		begin(_cache.badges),
		end(_cache.badges));
	_directoryIndex.clear();
	for (auto i = 0, count = int(_cache.directory.size()); i != count; ++i) {
		_directoryIndex.emplace(_cache.directory[i].id, i);
	}
}

// Not from inside the code that applies an answer or an event: the
// widgets look at the model from the event loop, after that.
void Model::scheduleChanges() {
	if (_changesScheduled || _forgotten) {
		return;
	}
	_changesScheduled = true;
	crl::on_main(this, [=] {
		_changesScheduled = false;
		if (!_forgotten) {
			_changes.fire({});
		}
	});
}

void Model::refresh() {
	const auto now = crl::now();
	if (!_ready || (_refreshed && (now - _refreshed < kManualRefreshGap))) {
		return;
	}
	_refreshed = now;
	_showProfiles = true;
	requestDirectory();
	requestActivity();
	requestBadges();
}

void Model::refreshActivity() {
	if (!_ready
		|| !_showProfiles
		|| (_activityAsked
			&& (crl::now() - _activityAsked < kActivityCardGap))) {
		return;
	}
	requestActivity();
}

void Model::saveSoon() {
	if (_forgotten) {
		return;
	}
	_dirty = true;
	if (!_saveTimer.isActive()) {
		_saveTimer.callOnce(kSaveDelay);
	}
}

void Model::save() {
	if (!_dirty || _forgotten) {
		return;
	}
	_dirty = false;
	crl::async([
		path = _path,
		cache = _cache,
		generation = NextGeneration(_path)
	] {
		WriteBytes(path, SerializeCache(cache), generation);
	});
}

void Model::wipe() {
	_saveTimer.cancel();
	_dirty = false;
	const auto had = !_cache.empty();
	_cache = Cache();
	_expireTimer.cancel();
	_activity.clear();
	_directoryLoaded = false;
	_badgesChecked = _directoryChecked = 0;
	_badgesVersion = -1;
	rebuildIndex();
	crl::async([path = _path, generation = NextGeneration(_path)] {
		WriteBytes(path, QByteArray(), generation);
	});
	if (had) {
		Badge::Refresh();
		scheduleChanges();
	}
}

void Model::forget() {
	_forgotten = true;
	_lifetime.destroy();
	_cloud.cancelAll();
	_badgesTimer.cancel();
	_badgesEventTimer.cancel();
	_directoryTimer.cancel();
	_activityTimer.cancel();
	_expireTimer.cancel();
	_saveTimer.cancel();
	_publishTimer.cancel();
	_stopTimer.cancel();
	_stopRequest = 0;
	_dirty = false;
	_enabled = false;
	_cache = Cache();
	_activity.clear();
	rebuildIndex();
	WriteBytes(_path, QByteArray(), NextGeneration(_path));

	// Whoever waits for the last stop is let go: the account is being
	// forgotten as a whole. The quit of the app among them: it does not
	// ask this model any more.
	flushStopCallbacks();
	if (std::exchange(_quitStop, QuitStop::Done) == QuitStop::Sending) {
		crl::on_main(&Core::App(), [] {
			Core::App().quitPreventFinished();
		});
	}
}

std::optional<Listening> Model::currentListening() const {
	using namespace Media::Player;

	const auto type = AudioMsgId::Type::Song;
	const auto current = instance()->current(type);
	const auto document = current.audio();
	if (!document || (&document->session() != _session)) {
		return std::nullopt;
	}
	const auto state = instance()->getState(type);
	if (state.id != current
		|| IsStoppedOrStopping(state.state)
		|| IsPausedOrPausing(state.state)) {
		return std::nullopt;
	}
	auto result = Listening();
	if (const auto song = document->song()) {
		result.title = song->title.simplified().left(128);
		result.performer = song->performer.simplified().left(128);
	}
	if (result.title.isEmpty()) {
		result.title = QFileInfo(
			document->filename()).completeBaseName().simplified().left(128);
	}
	if (result.title.isEmpty()) {
		return std::nullopt;
	}
	result.durationMs = std::clamp(
		int64(document->duration()),
		int64(0),
		kMaxDuration);
	return result;
}

// Nothing is sent while the «слушает» chip is off, while nobody is in
// the audience of the activity, while the account is not connected or
// after the last stop was sent. Switching any of that off takes back
// what was told before.
void Model::publishCheck() {
	const auto strong = account();
	_publishAllowed = strong
		&& _ready
		&& !_stopping
		&& ShouldPublishListening(strong->me());
	const auto now = crl::now();
	const auto changed = _throttle.want(
		_publishAllowed ? currentListening() : std::nullopt,
		now);
	if (changed) {
		publishStep();
	}
}

// The server shows «слушает» of a device only while that device has an
// event stream, and drops the track a few seconds after the stream is
// closed. So nothing is told without an open stream (State::Online), and
// the "hello" of every new stream comes here again: a track that still
// plays is told anew, a stop that could not be sent is sent then.
void Model::publishStep() {
	_publishTimer.cancel();
	const auto strong = account();
	if (_publishRequest
		|| !_ready
		|| _stopping
		|| !strong
		|| (strong->state() != Cloud::State::Online)) {
		return;
	}
	const auto now = crl::now();
	const auto step = _throttle.step(now);
	if (!step.send) {
		if (step.wait > 0) {
			_publishTimer.callOnce(step.wait);
		}
		return;
	}
	auto body = QJsonObject();
	body.insert(u"listening"_q, ListeningToJson(step.value));
	_throttle.sent(now);
	_publishRequest = _cloud.request(
		Cloud::PutRequest(u"/v1/me/activity"_q, std::move(body)),
		[=](const Cloud::Response &response) {
			_publishRequest = 0;
			_throttle.succeeded();
			scheduleChanges(); // The own profile shows the own chips.
			publishStep();
		},
		[=](const Cloud::Error &error) {
			_publishRequest = 0;
			_throttle.failed(crl::now(), error.retryAfter);
			publishStep();
		});
	if (!_publishRequest) {
		// Nothing could be sent, the fail callback comes from the event
		// loop: till then nothing more is tried.
		_publishRequest = -1;
	}
}

// The last stop, aside from the throttle: the account is about to stop
// talking to the server («Отключиться», the app quits) and the track
// that was told (or is being told right now) is taken back at once.
// Returns false when there is nothing to take back or nothing can be
// sent (see LastStopWanted): then nothing waits.
//
// The request is short-lived on purpose: its answer is waited for by a
// click or by the quit of the app, and _stopTimer ends that wait in
// kStopTimeout whatever happens to the request.
bool Model::startStop() {
	const auto strong = account();
	if (!strong
		|| !LastStopWanted(_ready, strong->state(), _throttle.told())) {
		return false;
	}
	auto body = QJsonObject();
	body.insert(u"listening"_q, ListeningToJson(std::nullopt));
	auto request = Cloud::PutRequest(u"/v1/me/activity"_q, std::move(body));
	request.timeout = kStopTimeout;
	request.retries = 0;

	// Only the answer to this very request ends the wait, and not from
	// inside the answer: whoever waits switches the account off, which
	// cancels every request of it.
	const auto sent = std::make_shared<Cloud::RequestId>(0);
	const auto answered = [=] {
		crl::on_main(this, [=] {
			if (*sent && (_stopRequest == *sent)) {
				_stopRequest = 0;
				stopFinished();
			}
		});
	};
	const auto id = _cloud.request(
		std::move(request),
		[=](const Cloud::Response &response) { answered(); },
		[=](const Cloud::Error &error) { answered(); });
	if (!id) {
		// Nothing was sent, so nothing was changed either.
		return false;
	}
	*sent = _stopRequest = id;
	holdPublishing();
	if (const auto publish = base::take(_publishRequest); publish > 0) {
		_cloud.cancel(publish);
	}
	_throttle.reset();
	_stopTimer.callOnce(kStopTimeout);
	scheduleChanges();
	return true;
}

// Nothing more is told from now on: the account is on its way out.
void Model::holdPublishing() {
	_stopping = true;
	_publishAllowed = false;
	_publishTimer.cancel();
}

bool Model::stopRunning() const {
	return _stopRequest || _stopTimer.isActive();
}

void Model::stopFinished() {
	_stopTimer.cancel();
	if (const auto id = base::take(_stopRequest)) {
		_cloud.cancel(id);
	}
	const auto quitting = (_quitStop == QuitStop::Sending);
	if (quitting) {
		_quitStop = QuitStop::Done;
	}
	const auto weak = base::make_weak(this);
	flushStopCallbacks();
	if (!weak) {
		return;
	} else if (quitting) {
		Core::App().quitPreventFinished();
		return;
	}

	// Whoever has asked for the stop switches the account off from its
	// callback. If the account is still connected after that, nobody
	// did: the track that plays is told again.
	crl::on_main(this, [=] {
		if (_stopping && !stopRunning()) {
			_stopping = false;
			publishCheck();
		}
	});
}

void Model::flushStopCallbacks() {
	for (const auto &callback : base::take(_stopCallbacks)) {
		if (callback) {
			callback();
		}
	}
}

void Model::stopPublishing(Fn<void()> done) {
	if (done) {
		_stopCallbacks.push_back(std::move(done));
	}
	if (stopRunning()) {
		return;
	} else if (!startStop()) {
		// Nothing to take back: done is still called from the event
		// loop, never from inside this call, and no track is told in
		// between.
		holdPublishing();
		_stopTimer.callOnce(crl::time(0));
	}
}

// Core::Application::readyToQuit() asks every time it tries to quit: the
// first time the last stop is sent, and the answer is true only while
// that stop (or one a click on «Отключиться» has sent a moment ago) is
// on its way. Nothing more is told after the first call, whatever else
// keeps the app from quitting for a moment.
bool Model::quitPrevent() {
	if (_quitStop == QuitStop::Done) {
		return false;
	} else if (_quitStop == QuitStop::None) {
		if (!stopRunning() && !startStop()) {
			holdPublishing();
			_quitStop = QuitStop::Done;
			return false;
		}
		_quitStop = QuitStop::Sending;
	}
	return true;
}

class Checker final {
public:
	explicit Checker(QStringList &log);

	void operator()(bool condition, const char *what);
	void section(const char *name);
	[[nodiscard]] int passed() const;
	[[nodiscard]] int failed() const;

private:
	QStringList &_log;
	int _passed = 0;
	int _failed = 0;
	int _sectionPassed = 0;
	int _sectionFailed = 0;

};

Checker::Checker(QStringList &log)
: _log(log) {
}

void Checker::operator()(bool condition, const char *what) {
	if (condition) {
		++_passed;
		++_sectionPassed;
	} else {
		++_failed;
		++_sectionFailed;
		_log.push_back(u"FAILED: "_q + QString::fromUtf8(what));
	}
}

void Checker::section(const char *name) {
	_log.push_back(u"%1: %2 passed, %3 failed"_q.arg(
		QString::fromUtf8(name),
		QString::number(_sectionPassed),
		QString::number(_sectionFailed)));
	_sectionPassed = _sectionFailed = 0;
}

int Checker::passed() const {
	return _passed;
}

int Checker::failed() const {
	return _failed;
}

[[nodiscard]] QJsonObject Json(const char *text) {
	return QJsonDocument::fromJson(QByteArray(text)).object();
}

void TestParsers(Checker &check) {
	const auto full = ParseProfile(Json(R"({
		"id": 111, "name": "  Аня\n Смирнова ", "avatar_rev": 2,
		"verified": false, "badge": true,
		"profile": {"status_text": "сплю", "status_emoji": "😴",
			"status_emoji_id": "5368324170671202286", "accent": "#FF5C8A"},
		"public_playlists": 2, "public_presets": 5, "extra": [1, 2]
	})"));
	check(full.valid() && full.id == 111, "profile: id");
	check(!full.name.contains(QChar('\n')), "profile: name is one line");
	check(full.name == u"Аня  Смирнова"_q
		|| full.name == u"Аня Смирнова"_q, "profile: name is trimmed");
	check(full.badge && !full.verified, "profile: flags");
	check(full.avatarRev == 2, "profile: avatar rev");
	check(full.statusText == u"сплю"_q, "profile: status text");
	check(full.statusEmoji == QString::fromUtf8("😴"), "profile: emoji");
	check(
		full.statusEmojiId == u"5368324170671202286"_q,
		"profile: custom emoji id");
	check(
		full.accent && full.accent->name() == u"#ff5c8a"_q,
		"profile: accent");
	check(
		full.publicPlaylists == 2 && full.publicPresets == 5,
		"profile: public counts");
	check(full.hasContent(), "profile: has content");

	const auto hostile = ParseProfile(Json(R"({
		"id": 222, "name": 5, "avatar_rev": -4,
		"profile": {"status_text": ["x"], "status_emoji": {},
			"status_emoji_id": "12ab", "accent": "red"},
		"public_playlists": 99999999, "public_presets": -3
	})"));
	check(hostile.valid(), "profile: wrong types still give an entry");
	check(hostile.name.isEmpty(), "profile: a name that is not a string");
	check(hostile.avatarRev == 0, "profile: negative avatar rev");
	check(hostile.statusText.isEmpty(), "profile: status of a wrong type");
	check(hostile.statusEmojiId.isEmpty(), "profile: emoji id with letters");
	check(!hostile.accent, "profile: accent that is not #rrggbb");
	check(
		hostile.publicPlaylists == 0,
		"profile: a count out of range is not believed");
	check(hostile.publicPresets == 0, "profile: negative count");
	check(!hostile.hasContent(), "profile: nothing to show");

	// Whatever a hostile answer is made of, a profile without a status
	// and without a believable number of shared things shows no block.
	const auto edge = ParseProfile(Json(R"({
		"id": 223, "public_playlists": 1000, "public_presets": 1001
	})"));
	check(edge.publicPlaylists == 1000, "profile: the largest count");
	check(edge.publicPresets == 0, "profile: a count just over the limit");
	for (const auto text : {
		R"({"id": 224, "public_playlists": "many", "public_presets": 2.5e9})",
		R"({"id": 224, "public_playlists": [3], "public_presets": {"n": 3}})",
		R"({"id": 224, "public_playlists": true, "public_presets": null})",
		R"({"id": 224, "public_playlists": 1e300, "public_presets": -1e300})",
		R"({"id": 224, "profile": "status", "public_playlists": -0.4})",
		R"({"id": 224, "profile": {"status_text": "   ",
			"status_emoji": "​‍"}})",
		R"({"id": 224, "profile": {"status_text": "ㅤ⠀️",
			"status_emoji": " "}})",
		R"({"id": 224, "profile": {"status_text": 0, "status_emoji": 1,
			"status_emoji_id": "5368324170671202286",
			"accent": "#ff5c8a"}, "badge": true, "verified": true})",
	}) {
		check(
			!ParseProfile(Json(text)).hasContent(),
			"profile: hostile or empty data shows nothing");
	}
	const auto dotted = ParseProfile(Json(R"({
		"id": 225, "profile": {"status_text": " . "}
	})"));
	check(
		dotted.hasContent() && dotted.statusText == u"."_q,
		"profile: a status of one visible character is a status");
	auto blankMe = Cloud::Me();
	blankMe.id = 226;
	blankMe.statusText = QString(3, QChar(0x200B));
	blankMe.statusEmoji = QString(1, QChar(0xFE0F));
	check(
		!ProfileFromMe(blankMe).hasContent(),
		"profile: the own blank status shows nothing either");

	auto longText = QJsonObject();
	longText.insert(u"id"_q, 5);
	auto longProfile = QJsonObject();
	longProfile.insert(u"status_text"_q, QString(500, QChar('a')));
	longText.insert(u"profile"_q, longProfile);
	longText.insert(u"name"_q, QString(500, QChar('b')));
	const auto clamped = ParseProfile(longText);
	check(clamped.name.size() == 64, "profile: long name is cut");
	check(clamped.statusText.size() == 80, "profile: long status is cut");

	check(!ParseProfile(Json(R"({"id": 0})")).valid(), "profile: id 0");
	check(!ParseProfile(Json(R"({"id": -5})")).valid(), "profile: id < 0");
	check(!ParseProfile(Json(R"({"name": "x"})")).valid(), "profile: no id");
	check(
		!ParseProfile(Json(R"({"id": 1e300})")).valid(),
		"profile: huge id");
	check(
		ParseProfile(Json(R"({"id": "333"})")).id == 333,
		"profile: id as a string");

	const auto listening = ParseActivity(Json(R"({
		"user_id": 111, "online": true,
		"listening": {"title": "Группа крови", "performer": "Кино",
			"duration_ms": 285000, "since": 1759800000000},
		"room": null
	})"));
	check(listening.userId == 111, "activity: user id");
	check(listening.online && listening.listening, "activity: chips");
	check(!listening.inRoom, "activity: a null room");
	check(
		listening.title == u"Группа крови"_q
			&& listening.performer == u"Кино"_q,
		"activity: track");
	check(listening.durationMs == 285000, "activity: duration");
	check(listening.since == 1759800000000, "activity: since");
	check(!listening.empty(), "activity: not empty");

	const auto room = ParseActivity(Json(R"({
		"user_id": 222, "online": false, "listening": null,
		"room": {"title": "Ночной эфир", "members": 4,
			"code": "k7qm2xpa9z"}
	})"));
	check(room.inRoom && !room.listening, "activity: room only");
	check(room.roomTitle == u"Ночной эфир"_q, "activity: room title");
	check(room.roomMembers == 4, "activity: room members");
	check(room.roomCode == u"K7QM2XPA9Z"_q, "activity: room code");

	const auto closed = ParseActivity(Json(R"({
		"user_id": 222,
		"room": {"title": "Закрытая", "members": 2},
		"listening": {"title": "", "performer": "Никто"}
	})"));
	check(closed.inRoom && closed.roomCode.isEmpty(), "activity: no code");
	check(!closed.listening, "activity: a track without a title");
	check(closed.performer.isEmpty(), "activity: its performer is dropped");

	const auto badCode = ParseActivity(Json(R"({
		"user_id": 222,
		"room": {"title": "x", "code": "https://evil.example/r/1"}
	})"));
	check(badCode.roomCode.isEmpty(), "activity: a code that is not one");

	const auto gone = ParseActivity(Json(R"({
		"user_id": 111, "online": false, "listening": null, "room": null
	})"));
	check(gone.userId == 111 && gone.empty(), "activity: nothing left");
	check(!ParseActivity(Json(R"({"online": true})")).userId, "activity: id");
	check(
		ParseActivity(Json(R"({"user_id": 1, "online": "yes"})")).empty(),
		"activity: online of a wrong type");
	check(
		ParseActivity(Json(R"({"user_id": 1,
			"listening": {"title": "t", "duration_ms": 1e15}})")
		).durationMs == kMaxDuration,
		"activity: duration is clamped");

	const auto list = ParseActivities(Json(R"({"users": [
		{"user_id": 1, "online": true},
		{"user_id": 2},
		{"online": true},
		"junk",
		{"user_id": 3, "room": {"title": "r"}}
	]})"));
	check(list.size() == 2, "activities: empty and broken are skipped");
	check(
		list.size() == 2 && list[0].userId == 1 && list[1].userId == 3,
		"activities: order is kept");
	check(ParseActivities(Json(R"({"users": 5})")).empty(), "activities: type");

	const auto ids = ParseBadges(Json(R"({"version": 42,
		"ids": [333, 111, "222", 111, 0, -1, "x", 1e300, null, {}]})"));
	check(ids.has_value(), "badges: parsed");
	check(
		ids && (*ids == std::vector<uint64>{ 111, 222, 333 }),
		"badges: sorted, unique, only ids");
	check(!ParseBadges(Json(R"({"version": 1})")), "badges: no list");
	check(!ParseBadges(Json(R"({"ids": {}})")), "badges: a wrong type");
	const auto none = ParseBadges(Json(R"({"ids": []})"));
	check(none && none->empty(), "badges: an empty list is a list");

	const auto users = ParseDirectory(Json(R"({"users": [
		{"id": 10, "name": "a"},
		{"id": 77, "name": "me"},
		{"id": 10, "name": "again"},
		{"name": "no id"},
		{"id": 20, "name": "b"}
	]})"), 77);
	check(users.has_value(), "directory: parsed");
	check(users && users->size() == 2, "directory: self and repeats out");
	check(
		users
			&& users->size() == 2
			&& (*users)[0].name == u"a"_q
			&& (*users)[1].id == 20,
		"directory: the first entry of an id wins");
	check(!ParseDirectory(Json(R"({})"), 77), "directory: no list");

	const auto shareId = [](QChar ch) {
		return QString(22, ch);
	};
	const auto entry = [&](
			const QString &id,
			const char *title,
			uint64 owner,
			bool shown) {
		auto ownerObject = QJsonObject();
		ownerObject.insert(u"id"_q, double(owner));
		auto result = QJsonObject();
		result.insert(u"id"_q, id);
		result.insert(u"title"_q, QString::fromUtf8(title));
		result.insert(u"owner"_q, ownerObject);
		result.insert(u"public"_q, shown);
		return result;
	};
	auto playlists = QJsonArray();
	auto mine = entry(shareId('a'), "Ночная", 5, true);
	mine.insert(u"track_count"_q, 12);
	playlists.push_back(mine);
	playlists.push_back(entry(shareId('b'), "Чужая", 6, true));
	playlists.push_back(entry(u"../../etc/passwd"_q, "x", 5, true));
	playlists.push_back(u"junk"_q);
	auto presets = QJsonArray();
	auto photo = entry(shareId('c'), "CCD 2004", 5, false);
	photo.insert(u"kind"_q, u"photo"_q);
	presets.push_back(photo);
	auto video = entry(shareId('d'), "VHS", 5, true);
	video.insert(u"kind"_q, u"video"_q);
	presets.push_back(video);
	auto weird = entry(shareId('e'), "?", 5, false);
	weird.insert(u"kind"_q, u"weird"_q);
	presets.push_back(weird);
	const auto shared = ParseShared(playlists, presets, 5);
	check(shared.size() == 4, "shared: only the own and the valid");
	check(
		shared.size() == 4
			&& shared[0].playlist
			&& shared[0].count == 12
			&& shared[0].shown,
		"shared: a playlist");
	check(
		shared.size() == 4
			&& !shared[1].playlist
			&& shared[1].kind == u"photo"_q
			&& !shared[1].shown,
		"shared: a photo preset");
	check(
		shared.size() == 4 && shared[2].kind == u"video"_q,
		"shared: a video preset");
	check(
		shared.size() == 4 && shared[3].kind == u"photo"_q,
		"shared: an unknown kind is a photo one");
	check(
		ParseShared(playlists, presets, 0).size() == 5,
		"shared: any owner in a profile of somebody");
	if (shared.size() != 4) {
		return;
	}

	// The flag of a playlist is its profile. The flag of a preset is the
	// gallery for everybody, and the profile only repeats it.
	check(
		shared[0].profileSwitch && !shared[0].listed,
		"shared: the flag of a playlist is the profile");
	check(
		!shared[1].listed && !shared[1].profileSwitch,
		"shared: a preset that is not in the gallery");
	check(
		shared[2].listed && shared[2].shown && !shared[2].profileSwitch,
		"shared: a preset of the gallery is what the profile shows");

	// A server that keeps the profile of a preset apart says "profile".
	auto apart = entry(shareId('f'), "Плёнка", 5, true);
	apart.insert(u"profile"_q, false);
	auto only = entry(shareId('g'), "Для своих", 5, false);
	only.insert(u"profile"_q, true);
	auto odd = entry(shareId('h'), "?", 5, true);
	odd.insert(u"profile"_q, u"yes"_q);
	auto separate = QJsonArray();
	separate.push_back(apart);
	separate.push_back(only);
	separate.push_back(odd);
	const auto split = ParseShared(QJsonArray(), separate, 5);
	check(split.size() == 3, "shared: presets with a flag of the profile");
	if (split.size() != 3) {
		return;
	}
	check(
		split[0].listed && !split[0].shown && split[0].profileSwitch,
		"shared: in the gallery and not in the profile");
	check(
		!split[1].listed && split[1].shown && split[1].profileSwitch,
		"shared: in the profile and not in the gallery");
	check(
		split[2].listed && split[2].shown && !split[2].profileSwitch,
		"shared: a flag of a wrong type is no flag");

	// What the two switches send. «Показывать в профиле» of a preset
	// never sends "public": that would put the preset into the gallery
	// for everybody.
	check(
		!ShownPatch(shared[1], true) && !ShownPatch(shared[2], false),
		"switches: the profile of a preset sends nothing without its flag");
	const auto profileOn = ShownPatch(split[0], true);
	check(
		profileOn
			&& profileOn->path == (u"/v1/presets/"_q + split[0].id)
			&& profileOn->body.size() == 1
			&& profileOn->body.value(u"profile"_q).toBool()
			&& !profileOn->body.contains(u"public"_q),
		"switches: the profile of a preset never touches the gallery");
	const auto profileOff = ShownPatch(split[1], false);
	check(
		profileOff
			&& profileOff->body.size() == 1
			&& profileOff->body.value(u"profile"_q).isBool()
			&& !profileOff->body.value(u"profile"_q).toBool(),
		"switches: the profile of a preset is switched off");
	const auto playlistOff = ShownPatch(shared[0], false);
	check(
		playlistOff
			&& playlistOff->path == (u"/v1/playlists/"_q + shared[0].id)
			&& playlistOff->body.size() == 1
			&& playlistOff->body.value(u"public"_q).isBool()
			&& !playlistOff->body.value(u"public"_q).toBool(),
		"switches: the profile of a playlist");
	const auto galleryOn = ListedPatch(shared[1], true);
	check(
		galleryOn
			&& galleryOn->path == (u"/v1/presets/"_q + shared[1].id)
			&& galleryOn->body.size() == 1
			&& galleryOn->body.value(u"public"_q).toBool(),
		"switches: the gallery of a preset");
	check(!ListedPatch(shared[0], true), "switches: a playlist has no gallery");
	auto broken = split[0];
	broken.id = u"../../etc/passwd"_q;
	check(
		!ListedPatch(broken, true) && !ShownPatch(broken, true),
		"switches: nothing for an id that is not one");

	// What is shown after a switch is the answer of the server.
	auto expected = shared[1];
	expected.listed = expected.shown = true;
	auto accepted = entry(shared[1].id, "CCD 2004", 5, true);
	accepted.insert(u"kind"_q, u"photo"_q);
	auto answer = QJsonObject();
	answer.insert(u"preset"_q, accepted);
	const auto patched = PatchedItem(answer, expected);
	check(
		patched.listed
			&& patched.shown
			&& !patched.profileSwitch
			&& patched.title == shared[1].title,
		"switches: the answer of the server is shown");
	auto refused = entry(shared[1].id, "CCD 2004", 5, false);
	auto refusal = QJsonObject();
	refusal.insert(u"preset"_q, refused);
	check(
		!PatchedItem(refusal, expected).listed
			&& !PatchedItem(refusal, expected).shown,
		"switches: the server has the last word");
	check(
		PatchedItem(QJsonObject(), expected) == expected,
		"switches: an answer without the item keeps what was asked");
	auto foreign = QJsonObject();
	foreign.insert(u"preset"_q, entry(shareId('z'), "x", 5, false));
	check(
		PatchedItem(foreign, expected) == expected,
		"switches: an answer about another item is not taken");
	auto listAnswer = QJsonObject();
	listAnswer.insert(u"playlist"_q, entry(shared[0].id, "Ночная", 5, false));
	check(
		!PatchedItem(listAnswer, shared[0]).shown
			&& PatchedItem(listAnswer, shared[0]).count == 12,
		"switches: the answer about a playlist");
}

void TestCache(Checker &check) {
	check(SerializeCache(Cache()).isEmpty(), "cache: nothing to keep");
	check(ParseCache(QByteArray(), 1).empty(), "cache: no file");
	check(ParseCache("not json", 1).empty(), "cache: broken file");
	check(ParseCache("[1, 2]", 1).empty(), "cache: not an object");
	check(
		ParseCache(R"({"version": 99, "badges": {"ids": [1]}})", 1).empty(),
		"cache: another version");

	auto cache = Cache();
	cache.badgesEtag = "\"b42\"";
	cache.badges = { 111, 222, 9007199254740991ULL };
	cache.directoryEtag = "\"d7\"";
	cache.directory.push_back(ParseProfile(Json(R"({
		"id": 111, "name": "Аня", "avatar_rev": 2, "badge": true,
		"profile": {"status_text": "сплю", "status_emoji": "😴",
			"status_emoji_id": "42", "accent": "#ff5c8a"},
		"public_playlists": 2, "public_presets": 5
	})")));
	cache.directory.push_back(ParseProfile(Json(R"({"id": 333})")));
	const auto bytes = SerializeCache(cache);
	check(!bytes.isEmpty(), "cache: serialized");
	const auto parsed = ParseCache(bytes, 1);
	check(parsed.badgesEtag == cache.badgesEtag, "cache: badges etag");
	check(parsed.badges == cache.badges, "cache: badges, the largest id too");
	check(parsed.directoryEtag == cache.directoryEtag, "cache: dir etag");
	check(parsed.directory == cache.directory, "cache: directory round trip");
	check(
		SerializeCache(parsed) == bytes,
		"cache: the second round gives the same bytes");

	// The own entry never gets into the directory, whoever wrote the file.
	check(
		ParseCache(bytes, 111).directory.size() == 1,
		"cache: the own id is dropped from the directory");

	// A broken part loses its ETag too, so the list is asked again.
	const auto broken = ParseCache(R"({"version": 1,
		"badges": {"etag": "\"b1\"", "ids": "oops"},
		"directory": {"etag": "\"d1\"", "users": [{"id": 5}]}})", 1);
	check(broken.badges.empty(), "cache: a broken list is empty");
	check(broken.badgesEtag.isEmpty(), "cache: and has no etag");
	check(broken.directory.size() == 1, "cache: the other part is kept");
	check(broken.directoryEtag == "\"d1\"", "cache: with its etag");

	// The file on disk: written, read, replaced, removed.
	const auto root = QDir::tempPath() + u"/oblivion_selftest_social"_q;
	const auto path = root
		+ QChar('/')
		+ QString::number(crl::now())
		+ u"/cloud_social.json"_q;
	WriteBytes(path, bytes, NextGeneration(path));
	check(ReadBytes(path) == bytes, "disk: written and read");
	const auto stale = NextGeneration(path);
	const auto fresh = NextGeneration(path);
	WriteBytes(path, "new", fresh);
	WriteBytes(path, "old", stale);
	check(ReadBytes(path) == "new", "disk: an older write does not win");
	WriteBytes(path, QByteArray(), NextGeneration(path));
	check(!QFile::exists(path), "disk: nothing to keep removes the file");
	QDir(QFileInfo(path).absolutePath()).removeRecursively();
	QDir().rmdir(root);
}

void TestAudience(Checker &check) {
	check(
		AudienceFromWire(u"everyone"_q) == Audience::Everyone,
		"audience: everyone");
	check(AudienceFromWire(u"chosen"_q) == Audience::Chosen, "audience: chosen");
	check(AudienceFromWire(u"nobody"_q) == Audience::Nobody, "audience: nobody");
	check(
		AudienceFromWire(u"EVERYONE"_q) == Audience::Nobody,
		"audience: anything unknown is nobody");
	check(AudienceFromWire(QString()) == Audience::Nobody, "audience: empty");
	for (const auto value : {
		Audience::Nobody,
		Audience::Chosen,
		Audience::Everyone,
	}) {
		check(
			AudienceFromWire(AudienceToWire(value)) == value,
			"audience: wire round trip");
	}

	const auto chosen = std::vector<uint64>{ 10, 20 };
	check(
		AudienceIncludes(Audience::Everyone, {}, 1, 99),
		"audience: everyone includes a stranger");
	check(
		AudienceIncludes(Audience::Chosen, chosen, 1, 20),
		"audience: chosen includes a chosen one");
	check(
		!AudienceIncludes(Audience::Chosen, chosen, 1, 30),
		"audience: chosen excludes the others");
	check(
		!AudienceIncludes(Audience::Nobody, chosen, 1, 10),
		"audience: nobody excludes even the chosen");
	check(
		AudienceIncludes(Audience::Nobody, {}, 1, 1),
		"audience: the owner always sees the own profile");
	check(
		!AudienceIncludes(Audience::Everyone, {}, 1, 0),
		"audience: no viewer");

	// The own «слушает» is told only with the chip on and somebody in
	// the audience of the activity.
	auto me = Cloud::Me();
	check(!ShouldPublishListening(me), "publish: the defaults tell nothing");
	me.chipListening = true;
	check(!ShouldPublishListening(me), "publish: nobody in the audience");
	me.activityAudience = u"chosen"_q;
	check(ShouldPublishListening(me), "publish: chip and audience");
	me.chipListening = false;
	me.activityAudience = u"everyone"_q;
	check(!ShouldPublishListening(me), "publish: the chip is off");
	me.chipListening = true;
	me.chipRoom = me.chipOnline = false;
	check(ShouldPublishListening(me), "publish: other chips don't matter");

	for (const auto flag : {
		Flag::Badge,
		Flag::ChipListening,
		Flag::ChipRoom,
		Flag::ChipOnline,
	}) {
		check(!FlagOf(Cloud::Me(), flag), "flags: everything is off at first");
	}
	auto flags = Cloud::Me();
	flags.badge = true;
	check(
		FlagOf(flags, Flag::Badge) && !FlagOf(flags, Flag::ChipOnline),
		"flags: the badge only");

	check(!EnabledState(Cloud::State::NoConsent), "state: no consent");
	check(!EnabledState(Cloud::State::Disconnected), "state: switched off");
	check(EnabledState(Cloud::State::Offline), "state: offline keeps data");
	check(EnabledState(Cloud::State::Online), "state: online");

	// An account that is switched off stays public on the server the way
	// it was left: Settings say so while there is anything to see.
	check(!LeftPublic(Cloud::Me()), "left: nothing before the consent");
	auto left = Cloud::Me();
	left.id = 5;
	check(!LeftPublic(left), "left: the defaults show nothing");
	left.badge = true;
	check(LeftPublic(left), "left: the badge is public");
	left.badge = false;
	left.profileAudience = u"chosen"_q;
	check(LeftPublic(left), "left: a profile somebody sees");
	left.profileAudience = u"nobody"_q;
	left.activityAudience = u"everyone"_q;
	check(!LeftPublic(left), "left: an audience without a chip");
	left.chipOnline = true;
	check(LeftPublic(left), "left: a chip somebody sees");
	left.activityAudience = u"nobody"_q;
	check(!LeftPublic(left), "left: a chip nobody sees");
	left.badge = true;
	left.id = 0;
	check(!LeftPublic(left), "left: nothing is known without an id");

	// The "badges" event: the list is asked a little later and not more
	// often than once in the gap.
	check(
		BadgesEventDelay(0, 1000, 0) == kBadgesEventJitterMin,
		"badges: the first event waits for the jitter only");
	check(
		BadgesEventDelay(0, 1000, 10 * kBadgesEventJitterMax)
			== kBadgesEventJitterMax,
		"badges: the jitter is bounded");
	const auto asked = crl::time(500'000);
	check(
		BadgesEventDelay(asked, asked + 10'000, kBadgesEventJitterMin)
			== (kBadgesEventGap - 10'000 + kBadgesEventJitterMin),
		"badges: not before the gap after the last request");
	check(
		BadgesEventDelay(asked, asked + 2 * kBadgesEventGap, 3000) == 3000,
		"badges: a list asked long ago is asked soon");
	check(
		BadgesEventDelay(asked, asked, kBadgesEventJitterMax)
			== (kBadgesEventGap + kBadgesEventJitterMax),
		"badges: the longest wait is the gap and the jitter");
}

void TestThrottle(Checker &check) {
	const auto track = [](const char *title) {
		return Listening{
			.title = QString::fromUtf8(title),
			.performer = u"Кино"_q,
			.durationMs = 200'000,
		};
	};
	const auto second = crl::time(1000);
	const auto start = 100 * second;

	auto idle = PublishThrottle();
	check(!idle.step(start).send, "throttle: nothing to tell at first");
	check(idle.step(start).wait == 0, "throttle: and nothing to wait for");
	check(!idle.want(std::nullopt, start), "throttle: still nothing");

	// A track is told after it has played for a moment.
	auto throttle = PublishThrottle();
	check(throttle.want(track("a"), start), "throttle: a track is a change");
	check(!throttle.want(track("a"), start + 1), "throttle: the same is not");
	auto step = throttle.step(start);
	check(!step.send && step.wait == kPublishSettle, "throttle: settle");
	step = throttle.step(start + kPublishSettle);
	check(step.send && step.value == track("a"), "throttle: then it is sent");
	throttle.sent(start + kPublishSettle);
	check(!throttle.published(), "throttle: not published till the answer");
	throttle.succeeded();
	check(
		throttle.published() == track("a"),
		"throttle: published after the answer");
	const auto sentA = start + kPublishSettle;
	step = throttle.step(sentA + second);
	check(
		!step.send && step.wait == (kPublishRefresh - second),
		"throttle: the same track waits for the refresh");
	step = throttle.step(sentA + kPublishRefresh);
	check(step.send && step.value == track("a"), "throttle: refreshed");
	throttle.sent(sentA + kPublishRefresh);
	throttle.succeeded();

	// Skipping through tracks: only the one that stays is told.
	auto skip = PublishThrottle();
	skip.want(track("a"), start);
	skip.sent(start + kPublishSettle);
	skip.succeeded();
	const auto base = start + kPublishSettle;
	skip.want(track("b"), base + 500);
	skip.want(track("c"), base + 1000);
	skip.want(track("d"), base + 1500);
	step = skip.step(base + 1500);
	check(!step.send, "throttle: a track just switched to is not sent");
	check(
		step.wait == (kPublishGap - 1500),
		"throttle: the gap after the last request holds");
	step = skip.step(base + kPublishGap);
	check(step.send && step.value == track("d"), "throttle: the last one");
	skip.sent(base + kPublishGap);
	skip.succeeded();

	// A stop is told without the settle time, but after the gap.
	skip.want(std::nullopt, base + kPublishGap + second);
	step = skip.step(base + kPublishGap + second);
	check(
		!step.send && step.wait == (kPublishGap - second),
		"throttle: a stop waits for the gap only");
	step = skip.step(base + 2 * kPublishGap);
	check(step.send && !step.value, "throttle: the stop is sent");
	skip.sent(base + 2 * kPublishGap);
	skip.succeeded();
	step = skip.step(base + 100 * kPublishGap);
	check(
		!step.send && step.wait == 0,
		"throttle: after a stop nothing is repeated");

	// Paused and resumed within the settle time: nothing was told and
	// nothing is taken back.
	auto blink = PublishThrottle();
	blink.want(track("a"), start);
	blink.want(std::nullopt, start + second);
	step = blink.step(start + second);
	check(!step.send && step.wait == 0, "throttle: a blink tells nothing");

	// A failure: the same track is tried again after a growing pause.
	auto fail = PublishThrottle();
	fail.want(track("a"), start);
	fail.sent(start + kPublishSettle);
	fail.failed(start + kPublishSettle, 0);
	const auto failedAt = start + kPublishSettle;
	step = fail.step(failedAt + second);
	check(
		!step.send && step.wait == (kPublishFailPause - second),
		"throttle: a failure pauses");
	step = fail.step(failedAt + kPublishFailPause);
	check(step.send && step.value == track("a"), "throttle: then repeats");
	fail.sent(failedAt + kPublishFailPause);
	fail.failed(failedAt + kPublishFailPause, 0);
	step = fail.step(failedAt + kPublishFailPause + second);
	check(
		step.wait == (2 * kPublishFailPause - second),
		"throttle: the pause grows");
	for (auto i = 0; i != 20; ++i) {
		fail.sent(start);
		fail.failed(start, 0);
	}
	check(
		fail.step(start).wait == kPublishFailMax,
		"throttle: the pause is capped");

	// 429 with a longer wait is honoured.
	auto limited = PublishThrottle();
	limited.want(track("a"), start);
	limited.sent(start + kPublishSettle);
	limited.failed(start + kPublishSettle, 30 * 60 * second);
	check(
		limited.step(start + kPublishSettle).wait == 30 * 60 * second,
		"throttle: retry-after of the server wins");

	// The track has ended while the request was failing: nothing was
	// told, so nothing is sent at all.
	auto ended = PublishThrottle();
	ended.want(track("a"), start);
	ended.sent(start + kPublishSettle);
	ended.failed(start + kPublishSettle, 0);
	ended.want(std::nullopt, start + 10 * second);
	step = ended.step(start + 60 * 60 * second);
	check(!step.send && step.wait == 0, "throttle: nothing to take back");

	// A stop that fails is repeated a couple of times (the track must
	// not stay shown because of one lost request) and then given up.
	auto stop = PublishThrottle();
	stop.want(track("a"), start);
	stop.sent(start + kPublishSettle);
	stop.succeeded();
	stop.want(std::nullopt, start + 60 * second);
	check(stop.step(start + 60 * second).send, "throttle: stop goes at once");
	auto stopAt = start + 60 * second;
	for (auto i = 0; i != kStopRetries; ++i) {
		stop.sent(stopAt);
		stop.failed(stopAt, 0);
		check(
			stop.published() == track("a"),
			"throttle: a failed stop leaves the track on the server");
		step = stop.step(stopAt + second);
		check(
			!step.send && step.wait >= (kPublishFailPause - second),
			"throttle: a failed stop waits for the pause");
		stopAt += second + step.wait;
		step = stop.step(stopAt);
		check(step.send && !step.value, "throttle: and is sent again");
	}
	stop.sent(stopAt);
	stop.failed(stopAt, 0);
	step = stop.step(stopAt + 60 * 60 * second);
	check(!step.send && step.wait == 0, "throttle: then it is given up");
	check(!stop.published(), "throttle: and nothing is held as told");

	// A stop that got through after a failure is the end of it.
	auto retried = PublishThrottle();
	retried.want(track("a"), start);
	retried.sent(start + kPublishSettle);
	retried.succeeded();
	retried.want(std::nullopt, start + 60 * second);
	retried.sent(start + 60 * second);
	retried.failed(start + 60 * second, 0);
	retried.sent(start + 60 * second + kPublishFailPause);
	retried.succeeded();
	step = retried.step(start + 60 * 60 * second);
	check(
		!step.send && step.wait == 0 && !retried.published(),
		"throttle: a repeated stop that got through");

	// A refresh that fails: the server still shows the track, and it is
	// tried again after the pause, not after one more period (by then
	// the server would have forgotten a track that still plays).
	auto refresh = PublishThrottle();
	refresh.want(track("a"), start);
	refresh.sent(start + kPublishSettle);
	refresh.succeeded();
	const auto due = start + kPublishSettle + kPublishRefresh;
	check(refresh.step(due).send, "throttle: the refresh is due");
	refresh.sent(due);
	refresh.failed(due, 0);
	check(
		refresh.published() == track("a"),
		"throttle: a failed refresh keeps what the server shows");
	step = refresh.step(due + second);
	check(
		!step.send && step.wait == (kPublishFailPause - second),
		"throttle: a failed refresh waits for the pause only");
	step = refresh.step(due + kPublishFailPause);
	check(
		step.send && step.value == track("a"),
		"throttle: and is sent again in time");
	refresh.sent(due + kPublishFailPause);
	refresh.succeeded();
	step = refresh.step(due + kPublishFailPause + second);
	check(
		!step.send && step.wait == (kPublishRefresh - second),
		"throttle: then the period starts anew");

	// The event stream was closed and is open again: the server may have
	// dropped the track of a device without a stream, so it is told again
	// at once, but not more often than the gap allows.
	auto again = PublishThrottle();
	again.reconnected(start);
	step = again.step(start);
	check(
		!step.send && step.wait == 0,
		"throttle: a reconnect with nothing told sends nothing");
	again.want(track("a"), start);
	again.sent(start + kPublishSettle);
	again.succeeded();
	const auto back = start + kPublishSettle + 60 * second;
	again.reconnected(back);
	step = again.step(back);
	check(
		step.send && step.value == track("a"),
		"throttle: after a reconnect the track is told again");
	again.sent(back);
	again.succeeded();
	again.reconnected(back + second);
	step = again.step(back + second);
	check(
		!step.send && step.wait == (kPublishGap - second),
		"throttle: reconnects in a row keep the gap");
	step = again.step(back + kPublishGap);
	check(step.send && step.value == track("a"), "throttle: then once more");
	again.sent(back + kPublishGap);

	// A request that was on its way during the reconnect proves nothing:
	// the track is told once more after it, and only once.
	again.reconnected(back + kPublishGap + second);
	again.succeeded();
	step = again.step(back + kPublishGap + 2 * second);
	check(
		!step.send && step.wait == (kPublishGap - 2 * second),
		"throttle: an answer from before the reconnect is not enough");
	step = again.step(back + 2 * kPublishGap);
	check(step.send && step.value == track("a"), "throttle: told once more");
	again.sent(back + 2 * kPublishGap);
	again.succeeded();
	step = again.step(back + 2 * kPublishGap + second);
	check(
		!step.send && step.wait == (kPublishRefresh - second),
		"throttle: and then the usual period");

	// A stop is not undone by a reconnect.
	again.want(std::nullopt, back + 3 * kPublishGap);
	again.sent(back + 3 * kPublishGap);
	again.succeeded();
	again.reconnected(back + 4 * kPublishGap);
	step = again.step(back + 4 * kPublishGap);
	check(
		!step.send && step.wait == 0,
		"throttle: nothing is told again after a stop");

	// Track b failed while the server still shows track a: a stop is
	// still told.
	auto stale = PublishThrottle();
	stale.want(track("a"), start);
	stale.sent(start + kPublishSettle);
	stale.succeeded();
	stale.want(track("b"), start + 60 * second);
	stale.sent(start + 70 * second);
	stale.failed(start + 70 * second, 0);
	stale.want(std::nullopt, start + 80 * second);
	step = stale.step(start + 70 * second + kPublishFailPause);
	check(step.send && !step.value, "throttle: the old track is taken back");

	// The chip was switched off (or the account disconnected and reset).
	auto off = PublishThrottle();
	off.want(track("a"), start);
	off.sent(start + kPublishSettle);
	off.succeeded();
	off.reset();
	check(
		!off.step(start + 60 * second).send,
		"throttle: a reset sends nothing");

	// The last stop («Отключиться», the quit of the app) takes back only
	// what the server may be showing.
	auto last = PublishThrottle();
	check(!last.told(), "last stop: nothing was told at first");
	last.want(track("a"), start);
	check(!last.told(), "last stop: a track that only waits was not told");
	last.sent(start + kPublishSettle);
	check(last.told(), "last stop: a request on its way counts as told");
	last.failed(start + kPublishSettle, 0);
	check(!last.told(), "last stop: a failed request is not held as told");
	last.sent(start + kPublishSettle + kPublishFailPause);
	last.succeeded();
	check(last.told(), "last stop: a track the server has taken is told");
	last.want(std::nullopt, start + 60 * 60 * second);
	last.sent(start + 60 * 60 * second);
	check(last.told(), "last stop: a stop on its way is not there yet");
	last.succeeded();
	check(!last.told(), "last stop: after a stop nothing is left to take");
	last.want(track("b"), start + 61 * 60 * second);
	last.sent(start + 62 * 60 * second);
	last.succeeded();
	last.reset();
	check(!last.told(), "last stop: a reset forgets what was told");

	check(
		LastStopWanted(true, Cloud::State::Online, true),
		"last stop: sent while the stream is open");
	check(
		!LastStopWanted(true, Cloud::State::Online, false),
		"last stop: nothing is sent when nothing was told");
	check(
		!LastStopWanted(false, Cloud::State::Online, true),
		"last stop: nothing is sent for an account that is not ready");
	for (const auto state : {
		Cloud::State::NoConsent,
		Cloud::State::Disconnected,
		Cloud::State::NeedsLink,
		Cloud::State::Connecting,
		Cloud::State::Offline,
		Cloud::State::UpgradeRequired,
		Cloud::State::Banned,
	}) {
		check(
			!LastStopWanted(true, state, true),
			"last stop: a server without the stream is never waited for");
	}

	check(
		ListeningToJson(std::nullopt).isNull(),
		"throttle: nothing is a JSON null");
	const auto json = ListeningToJson(track("a")).toObject();
	check(
		json.value(u"title"_q).toString() == u"a"_q
			&& json.value(u"performer"_q).toString() == u"Кино"_q
			&& json.value(u"duration_ms"_q).toDouble() == 200'000.,
		"throttle: a track as the protocol wants it");
}

void TestBook(Checker &check) {
	const auto second = crl::time(1000);
	const auto start = 1000 * second;
	const auto online = [](uint64 id) {
		return Activity{ .userId = id, .online = true };
	};
	const auto listening = [](uint64 id, const char *title) {
		auto result = Activity{ .userId = id, .listening = true };
		result.title = QString::fromUtf8(title);
		return result;
	};
	const auto gone = [](uint64 id) {
		return Activity{ .userId = id };
	};

	auto book = ActivityBook();
	check(!book.apply(Activity(), start), "book: an event without an id");
	check(!book.apply(gone(1), start), "book: nothing to remove");
	check(book.apply(online(1), start), "book: a new entry");
	check(!book.apply(online(1), start + second), "book: the same again");
	check(
		book.find(1).online && book.find(2).empty(),
		"book: who is known and who is not");
	check(book.apply(gone(1), start), "book: an empty event removes");
	check(book.size() == 0, "book: and nothing is left");

	// The whole list replaces what was kept.
	book.apply(online(1), start);
	book.apply(online(2), start);
	book.requested();
	check(
		book.replace({ online(2), online(3) }, start + second),
		"book: a new list is a change");
	check(
		book.find(1).empty() && book.find(2).online && book.find(3).online,
		"book: who is not in the list is gone");
	book.requested();
	check(
		!book.replace({ online(3), online(2) }, start + 2 * second),
		"book: the same list changes nothing");
	check(
		!book.replace({ gone(9), Activity(), online(2), online(3) }, start),
		"book: empty entries of a list are skipped");

	// An event that comes while the list is being asked wins over the
	// list: the list was made before it.
	book.requested();
	check(book.apply(gone(2), start + 3 * second), "book: somebody leaves");
	check(
		book.apply(listening(4, "new"), start + 3 * second),
		"book: somebody starts a track");
	check(
		!book.replace(
			{ online(2), online(3), online(4) },
			start + 4 * second),
		"book: the older list does not undo the events");
	check(book.find(2).empty(), "book: who has left stays gone");
	check(
		book.find(4).listening && !book.find(4).online,
		"book: what an event has told stays");
	book.requested();
	check(
		book.replace({ online(2) }, start + 5 * second),
		"book: the next list is believed as a whole again");
	check(
		book.find(2).online && book.find(4).empty() && book.size() == 1,
		"book: the marks of the events are gone with their list");
	book.requested();
	book.apply(gone(2), start + 6 * second);
	book.requestFailed();
	book.requested();
	book.replace({ online(2) }, start + 7 * second);
	check(book.find(2).online, "book: a failed request leaves no marks");

	// «слушает» nobody has confirmed for longer than the server keeps a
	// track is dropped; the other chips of that person stay.
	auto old = ActivityBook();
	auto both = listening(5, "song");
	both.online = true;
	both.performer = u"band"_q;
	both.durationMs = 200'000;
	both.since = 1759800000000;
	old.apply(Activity(both), start);
	old.apply(listening(6, "other"), start + 60 * second);
	check(
		old.nextExpiry() == start + kListeningTtl,
		"book: the nearest track to drop");
	check(
		!old.expire(start + kListeningTtl - 1),
		"book: nothing is dropped before its time");
	check(old.expire(start + kListeningTtl), "book: an old track is dropped");
	const auto kept = old.find(5);
	check(
		kept.online
			&& !kept.listening
			&& kept.title.isEmpty()
			&& kept.performer.isEmpty()
			&& !kept.durationMs
			&& !kept.since,
		"book: the other chips of that person stay");
	check(old.find(6).listening, "book: a newer track stays");
	check(
		old.nextExpiry() == start + 60 * second + kListeningTtl,
		"book: the next track to drop");
	check(
		!old.apply(listening(6, "other"), start + 600 * second),
		"book: the same track told again is no change");
	check(
		old.nextExpiry() == start + 600 * second + kListeningTtl,
		"book: but it lives on from that moment");
	check(
		old.expire(start + 600 * second + kListeningTtl),
		"book: till nobody repeats it");
	check(
		old.find(6).empty() && old.size() == 1,
		"book: an entry with nothing left is removed");
	check(old.nextExpiry() == 0, "book: nothing more to wait for");
	check(!old.expire(start + 100'000 * second), "book: and nothing to drop");

	auto confirmed = ActivityBook();
	confirmed.apply(listening(7, "a"), start);
	confirmed.requested();
	check(
		!confirmed.replace({ listening(7, "a") }, start + 300 * second),
		"book: a list with the same track is no change");
	check(
		confirmed.nextExpiry() == start + 300 * second + kListeningTtl,
		"book: a list confirms a track too");
	check(
		confirmed.clear() && !confirmed.clear() && confirmed.size() == 0,
		"book: cleared when the connection is lost");
}

void TestChips(Checker &check) {
	const auto phrases = ChipPhrases{
		.listening = u"слушает: {text}"_q,
		.room = u"в комнате: {title}"_q,
		.roomUntitled = u"в комнате"_q,
		.online = u"в Oblivion"_q,
	};
	check(
		TrackText(u"Кино"_q, u"Группа крови"_q)
			== QString::fromUtf8("Кино — Группа крови"),
		"chips: performer and title");
	check(
		TrackText(QString(), u"Группа крови"_q) == u"Группа крови"_q,
		"chips: a title alone");
	check(
		TrackText(u"  "_q, u" Группа  крови "_q) == u"Группа крови"_q,
		"chips: spaces are cleaned");
	check(TrackText(u"Кино"_q, QString()).isEmpty(), "chips: no title");

	check(
		BuildChips(Activity(), phrases, true).empty(),
		"chips: nothing for an empty activity");

	auto online = Activity{ .userId = 1, .online = true };
	auto chips = BuildChips(online, phrases, true);
	check(
		chips.size() == 1
			&& chips[0].type == ChipType::Online
			&& chips[0].text == u"в Oblivion"_q
			&& chips[0].joinCode.isEmpty(),
		"chips: only online");

	auto listening = online;
	listening.listening = true;
	listening.title = u"Группа крови"_q;
	listening.performer = u"Кино"_q;
	chips = BuildChips(listening, phrases, true);
	check(
		chips.size() == 1 && chips[0].type == ChipType::Listening,
		"chips: online is not repeated next to a track");
	check(
		chips.size() == 1
			&& chips[0].text
				== QString::fromUtf8("слушает: Кино — Группа крови"),
		"chips: the text of a track");

	auto both = listening;
	both.inRoom = true;
	both.roomTitle = u"Ночной эфир"_q;
	both.roomCode = u"K7QM2XPA9Z"_q;
	chips = BuildChips(both, phrases, true);
	check(chips.size() == 2, "chips: a track and a room");
	check(
		chips.size() == 2
			&& chips[0].type == ChipType::Listening
			&& chips[1].type == ChipType::Room,
		"chips: the track goes first");
	check(
		chips.size() == 2
			&& chips[1].text == u"в комнате: Ночной эфир"_q
			&& chips[1].joinCode == u"K7QM2XPA9Z"_q,
		"chips: a room with «Войти»");
	chips = BuildChips(both, phrases, false);
	check(
		chips.size() == 2 && chips[1].joinCode.isEmpty(),
		"chips: no «Войти» with the rooms switched off");

	auto closed = Activity{ .userId = 1, .inRoom = true };
	chips = BuildChips(closed, phrases, true);
	check(
		chips.size() == 1
			&& chips[0].text == u"в комнате"_q
			&& chips[0].joinCode.isEmpty(),
		"chips: a room without a title and without a code");

	// A title of somebody else can't break the phrase.
	auto tricky = Activity{ .userId = 1, .listening = true };
	tricky.title = u"{title} {text}"_q;
	chips = BuildChips(tricky, phrases, true);
	check(
		chips.size() == 1 && chips[0].text == u"слушает: {title} {text}"_q,
		"chips: placeholders in a title stay a text");

	auto ranks = std::vector<int>{
		ActivityRank(listening),
		ActivityRank(closed),
		ActivityRank(online),
		ActivityRank(Activity()),
	};
	check(
		ranges::is_sorted(ranks) && (ranks.front() < ranks.back()),
		"friends: a track, a room, online, nothing");
	check(
		ActivityRank(both) == ActivityRank(listening),
		"friends: a track wins over a room");
}

} // namespace

Audience AudienceFromWire(const QString &value) {
	return (value == u"everyone"_q)
		? Audience::Everyone
		: (value == u"chosen"_q)
		? Audience::Chosen
		: Audience::Nobody;
}

QString AudienceToWire(Audience value) {
	switch (value) {
	case Audience::Everyone: return u"everyone"_q;
	case Audience::Chosen: return u"chosen"_q;
	case Audience::Nobody: break;
	}
	return u"nobody"_q;
}

bool AudienceIncludes(
		Audience audience,
		const std::vector<uint64> &chosen,
		uint64 ownerId,
		uint64 viewerId) {
	if (!viewerId) {
		return false;
	} else if (viewerId == ownerId) {
		return true;
	}
	switch (audience) {
	case Audience::Everyone: return true;
	case Audience::Chosen: return ranges::contains(chosen, viewerId);
	case Audience::Nobody: break;
	}
	return false;
}

QString TrackText(const QString &performer, const QString &title) {
	const auto cleanTitle = title.simplified();
	const auto cleanPerformer = performer.simplified();
	return cleanTitle.isEmpty()
		? QString()
		: cleanPerformer.isEmpty()
		? cleanTitle
		: (cleanPerformer + QString::fromUtf8(" — ") + cleanTitle);
}

std::vector<Chip> BuildChips(
		const Activity &activity,
		const ChipPhrases &phrases,
		bool allowJoin) {
	auto result = std::vector<Chip>();
	const auto fill = [](
			const QString &phrase,
			const QString &tag,
			const QString &value) {
		const auto index = phrase.indexOf(tag);
		return (index < 0)
			? (phrase + QChar(' ') + value)
			: (phrase.left(index) + value + phrase.mid(index + tag.size()));
	};
	if (activity.listening) {
		const auto track = TrackText(activity.performer, activity.title);
		if (!track.isEmpty()) {
			result.push_back({
				.type = ChipType::Listening,
				.text = fill(phrases.listening, u"{text}"_q, track),
			});
		}
	}
	if (activity.inRoom) {
		const auto title = activity.roomTitle.simplified();
		result.push_back({
			.type = ChipType::Room,
			.text = (title.isEmpty()
				? phrases.roomUntitled
				: fill(phrases.room, u"{title}"_q, title)),
			.joinCode = allowJoin ? activity.roomCode : QString(),
		});
	}
	if (activity.online && result.empty()) {
		result.push_back({
			.type = ChipType::Online,
			.text = phrases.online,
		});
	}
	return result;
}

int ActivityRank(const Activity &activity) {
	return activity.listening
		? 0
		: activity.inRoom
		? 1
		: activity.online
		? 2
		: 3;
}

void Start(not_null<Main::Session*> session) {
	auto &models = Global().models;
	if (models.contains(session)) {
		return;
	}
	models.emplace(session, std::make_unique<Model>(session));
	session->lifetime().add([=] {
		// Taken out first: the model may still let somebody go who waits
		// for its last stop, and that code must not find it in the map.
		auto &models = Global().models;
		const auto i = models.find(session);
		if (i != end(models)) {
			const auto model = std::move(i->second);
			models.erase(i);
		}
	});
}

void Forget(not_null<Main::Session*> session) {
	auto &models = Global().models;
	const auto i = models.find(session);
	if (i != end(models)) {
		const auto model = std::move(i->second);
		models.erase(i);
		model->forget();
		return;
	}
	const auto path = CachePath(session);
	WriteBytes(path, QByteArray(), NextGeneration(path));
}

bool BadgeListed(not_null<Main::Session*> session, uint64 userId) {
	const auto model = Lookup(session);
	return model && model->badgeListed(userId);
}

std::optional<Profile> ProfileOf(
		not_null<Main::Session*> session,
		uint64 userId) {
	const auto model = Lookup(session);
	return model ? model->profile(userId) : std::nullopt;
}

Activity ActivityOf(not_null<Main::Session*> session, uint64 userId) {
	const auto model = Lookup(session);
	return model ? model->activity(userId) : Activity();
}

std::vector<Profile> Directory(not_null<Main::Session*> session) {
	const auto model = Lookup(session);
	return model ? model->directory() : std::vector<Profile>();
}

Status CurrentStatus(not_null<Main::Session*> session) {
	const auto model = Lookup(session);
	return model ? model->status() : Status::Off;
}

rpl::producer<> Changes(not_null<Main::Session*> session) {
	if (const auto model = Lookup(session)) {
		return model->changes();
	}
	return rpl::never<>();
}

void Refresh(not_null<Main::Session*> session) {
	if (const auto model = Lookup(session)) {
		model->refresh();
	}
}

void RefreshActivity(not_null<Main::Session*> session) {
	if (const auto model = Lookup(session)) {
		model->refreshActivity();
	}
}

void StopPublishing(not_null<Main::Session*> session, Fn<void()> done) {
	if (const auto model = Lookup(session)) {
		model->stopPublishing(std::move(done));
	} else if (done) {
		crl::on_main(session, done);
	}
}

bool IsQuitPrevent() {
	auto result = false;
	for (const auto &[session, model] : Global().models) {
		if (model->quitPrevent()) {
			result = true;
		}
	}
	return result;
}

namespace {

// Nothing was sent: the caller still hears about it, from the event loop.
void FailLater(
		not_null<Main::Session*> session,
		Fn<void(const Cloud::Error&)> fail) {
	if (fail) {
		crl::on_main(session, [=] {
			fail(Cloud::Error{ .type = Cloud::Error::Type::NotConnected });
		});
	}
}

} // namespace

void LoadShared(
		not_null<Main::Session*> session,
		uint64 userId,
		Fn<void(std::vector<SharedItem>)> done,
		Fn<void(const Cloud::Error&)> fail) {
	const auto model = Lookup(session);
	const auto account = model ? model->account() : nullptr;

	// Only for somebody from the directory that is kept here: asking for
	// anybody else would tell the server whose profiles are opened.
	if (!account || !model->profile(userId)) {
		FailLater(session, std::move(fail));
		return;
	}
	const auto own = (userId == session->userId().bare);
	if (own) {
		LoadOwnShared(session, std::move(done), std::move(fail));
		return;
	}
	const auto weak = base::make_weak(model);
	account->request(
		Cloud::GetRequest(u"/v1/users/"_q + QString::number(userId)),
		[=](const Cloud::Response &response) {
			const auto user = response.json.value(u"user"_q).toObject();
			if (const auto strong = weak.get()) {
				// The answer also says what the person is doing now.
				if (user.contains(u"activity"_q)) {
					strong->activityHeard(userId, user.value(u"activity"_q));
				}
			}
			auto list = ParseShared(
				user.value(u"playlists"_q).toArray(),
				user.value(u"presets"_q).toArray(),
				0);
			for (auto &item : list) {
				item.shown = true;
			}
			if (done) {
				done(std::move(list));
			}
		},
		std::move(fail));
}

void LoadOwnShared(
		not_null<Main::Session*> session,
		Fn<void(std::vector<SharedItem>)> done,
		Fn<void(const Cloud::Error&)> fail) {
	const auto model = Lookup(session);
	const auto account = model ? model->account() : nullptr;
	if (!account) {
		FailLater(session, std::move(fail));
		return;
	}
	const auto weak = base::make_weak(account);
	const auto selfId = session->userId().bare;

	// A part the server has switched off is an empty list, not an error.
	const auto disabled = [](const Cloud::Error &error) {
		return (error.status == 404) && error.is("feature_disabled");
	};
	const auto presets = [=](QJsonArray playlists) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		const auto finish = [=](const QJsonArray &list) {
			if (done) {
				done(ParseShared(playlists, list, selfId));
			}
		};
		strong->request(
			Cloud::GetRequest(u"/v1/presets"_q),
			[=](const Cloud::Response &response) {
				finish(response.json.value(u"presets"_q).toArray());
			},
			[=](const Cloud::Error &error) {
				if (disabled(error)) {
					finish(QJsonArray());
				} else if (fail) {
					fail(error);
				}
			});
	};
	account->request(
		Cloud::GetRequest(u"/v1/playlists"_q),
		[=](const Cloud::Response &response) {
			presets(response.json.value(u"playlists"_q).toArray());
		},
		[=](const Cloud::Error &error) {
			if (disabled(error)) {
				presets(QJsonArray());
			} else if (fail) {
				fail(error);
			}
		});
}

namespace {

void SendSharedPatch(
		not_null<Main::Session*> session,
		std::optional<SharedPatch> patch,
		SharedItem expected,
		Fn<void(SharedItem)> done,
		Fn<void(const Cloud::Error&)> fail) {
	const auto model = Lookup(session);
	const auto account = model ? model->account() : nullptr;
	if (!account || !patch) {
		FailLater(session, std::move(fail));
		return;
	}
	account->request(
		Cloud::PatchRequest(patch->path, std::move(patch->body)),
		[=](const Cloud::Response &response) {
			if (done) {
				done(PatchedItem(response.json, expected));
			}
		},
		std::move(fail));
}

} // namespace

void SetSharedShown(
		not_null<Main::Session*> session,
		const SharedItem &item,
		bool shown,
		Fn<void(SharedItem)> done,
		Fn<void(const Cloud::Error&)> fail) {
	auto expected = item;
	expected.shown = shown;
	SendSharedPatch(
		session,
		ShownPatch(item, shown),
		std::move(expected),
		std::move(done),
		std::move(fail));
}

void SetPresetListed(
		not_null<Main::Session*> session,
		const SharedItem &item,
		bool listed,
		Fn<void(SharedItem)> done,
		Fn<void(const Cloud::Error&)> fail) {
	auto expected = item;
	expected.listed = listed;
	if (!item.profileSwitch) {
		// The profile of the owner shows the presets of the gallery.
		expected.shown = listed;
	}
	SendSharedPatch(
		session,
		ListedPatch(item, listed),
		std::move(expected),
		std::move(done),
		std::move(fail));
}

// What the server has about the account, as it was last heard: nothing
// before the consent and after «Удалить мои данные» (me() is empty then),
// and what was left there for an account that is switched off.
bool FlagNow(not_null<Main::Session*> session, Flag flag) {
	const auto &me = Cloud::For(session).me();
	return me.valid() && FlagOf(me, flag);
}

rpl::producer<bool> FlagValue(not_null<Main::Session*> session, Flag flag) {
	const auto account = &Cloud::For(session);
	return rpl::merge(
		account->stateValue() | rpl::to_empty,
		account->meUpdated()
	) | rpl::map([=] {
		return FlagNow(session, flag);
	}) | rpl::distinct_until_changed();
}

Audience AudienceNow(
		not_null<Main::Session*> session,
		AudienceKind kind) {
	const auto &me = Cloud::For(session).me();
	return !me.valid()
		? Audience::Nobody
		: AudienceFromWire((kind == AudienceKind::Profile)
			? me.profileAudience
			: me.activityAudience);
}

rpl::producer<Audience> AudienceValue(
		not_null<Main::Session*> session,
		AudienceKind kind) {
	const auto account = &Cloud::For(session);
	return rpl::merge(
		account->stateValue() | rpl::to_empty,
		account->meUpdated()
	) | rpl::map([=] {
		return AudienceNow(session, kind);
	}) | rpl::distinct_until_changed();
}

rpl::producer<int> ChosenCountValue(not_null<Main::Session*> session) {
	const auto account = &Cloud::For(session);
	return rpl::merge(
		account->stateValue() | rpl::to_empty,
		account->meUpdated()
	) | rpl::map([=] {
		const auto &me = account->me();
		return me.valid() ? int(me.chosen.size()) : 0;
	}) | rpl::distinct_until_changed();
}

rpl::producer<bool> LeftPublicValue(not_null<Main::Session*> session) {
	const auto account = &Cloud::For(session);
	return rpl::merge(
		account->stateValue() | rpl::to_empty,
		account->meUpdated()
	) | rpl::map([=] {
		return (account->state() == Cloud::State::Disconnected)
			&& LeftPublic(account->me());
	}) | rpl::distinct_until_changed();
}

bool RunSelfTest(QStringList &log) {
	auto check = Checker(log);
	TestParsers(check);
	check.section("parsers");
	TestCache(check);
	check.section("directory cache");
	TestAudience(check);
	check.section("audience rules");
	TestThrottle(check);
	check.section("activity throttle");
	TestBook(check);
	check.section("activity book");
	TestChips(check);
	check.section("chips");
	log.push_back(u"cloud_social: %1 checks passed, %2 failed"_q.arg(
		QString::number(check.passed()),
		QString::number(check.failed())));
	return !check.failed();
}

} // namespace Oblivion::Social
