/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_cloud_social.h"

#include "base/timer.h"
#include "base/weak_ptr.h"
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

// The protocol asks for these periods; with an ETag an unchanged list
// costs one short answer.
constexpr auto kBadgesRefresh = 30 * 60 * crl::time(1000);
constexpr auto kDirectoryRefresh = 10 * 60 * crl::time(1000);
constexpr auto kManualRefreshGap = crl::time(15'000);
constexpr auto kSaveDelay = crl::time(2000);

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
	// The account was disconnected: what the server has is forgotten
	// there by itself, nothing more is sent.
	void reset();
	// What the server shows to the audience, as far as it is known.
	[[nodiscard]] const std::optional<Listening> &published() const;

private:
	std::optional<Listening> _wanted;
	std::optional<Listening> _server; // What the server shows, as known.
	std::optional<Listening> _sending;
	crl::time _wantedSince = 0;
	crl::time _lastSent = 0;
	crl::time _blockedTill = 0;
	bool _everSent = false;
	int _fails = 0;

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
		at = _lastSent + kPublishRefresh;
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
}

void PublishThrottle::succeeded() {
	_server = base::take(_sending);
	_fails = 0;
}

void PublishThrottle::failed(crl::time now, crl::time retryAfter) {
	_fails = std::min(_fails + 1, 8);
	const auto pause = std::min(
		kPublishFailPause * (crl::time(1) << std::min(_fails - 1, 3)),
		kPublishFailMax);
	_blockedTill = now + std::max(pause, retryAfter);
	if (!base::take(_sending)) {
		// A stop that did not get through is not repeated: the server
		// drops the activity by itself in a quarter of an hour.
		_server = std::nullopt;
	}
}

void PublishThrottle::reset() {
	*this = PublishThrottle();
}

const std::optional<Listening> &PublishThrottle::published() const {
	return _server;
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

[[nodiscard]] QString DigitsOnly(const QString &text) {
	const auto digits = ranges::all_of(text, [](QChar ch) {
		return (ch.unicode() >= '0') && (ch.unicode() <= '9');
	});
	return digits ? text : QString();
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
	result.statusEmoji = Cloud::JsonText(
		profile.value(u"status_emoji"_q),
		16);
	result.statusEmojiId = DigitsOnly(
		Cloud::JsonText(profile.value(u"status_emoji_id"_q), 20));
	result.accent = Cloud::JsonColor(profile.value(u"accent"_q));
	result.publicPlaylists = ClampInt(
		object.value(u"public_playlists"_q),
		1000);
	result.publicPresets = ClampInt(object.value(u"public_presets"_q), 1000);
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
		result.push_back({
			.id = id,
			.playlist = false,
			.title = Cloud::JsonText(object.value(u"title"_q), 64),
			.kind = video ? u"video"_q : u"photo"_q,
			.shown = object.value(u"public"_q).toBool(),
		});
	}
	return result;
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
	void forget();

private:
	void cacheLoaded(Cache &&cache);
	void stateChanged(Cloud::State state);
	void readyChanged(bool ready);
	void settingsChanged();
	void handleEvent(const Cloud::Event &event);
	void checkLists();
	void requestBadges();
	void requestDirectory();
	void requestActivity();
	void applyActivity(Activity &&activity);
	void clearActivity();
	void rebuildIndex();
	void scheduleChanges();
	void saveSoon();
	void save();
	void wipe();

	void publishCheck();
	void publishStep();
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
	base::flat_map<uint64, Activity> _activity;

	Cloud::RequestId _badgesRequest = 0;
	Cloud::RequestId _directoryRequest = 0;
	Cloud::RequestId _activityRequest = 0;
	Cloud::RequestId _publishRequest = 0;
	crl::time _badgesChecked = 0;
	crl::time _directoryChecked = 0;
	crl::time _refreshed = 0;
	bool _directoryLoaded = false;
	bool _directoryFailed = false;

	bool _enabled = false;
	bool _ready = false;
	bool _cacheReady = false;
	bool _showBadges = false;
	bool _showProfiles = false;
	bool _dirty = false;
	bool _forgotten = false;
	bool _changesScheduled = false;
	bool _publishAllowed = false;

	PublishThrottle _throttle;

	base::Timer _badgesTimer;
	base::Timer _directoryTimer;
	base::Timer _saveTimer;
	base::Timer _publishTimer;
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
, _directoryTimer([=] { requestDirectory(); })
, _saveTimer([=] { save(); })
, _publishTimer([=] { publishStep(); }) {
	const auto account = &Cloud::For(session);
	_enabled = EnabledState(account->state());
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
		publishCheck();
		scheduleChanges();
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
	if (_dirty && !_forgotten) {
		WriteBytes(_path, SerializeCache(_cache), NextGeneration(_path));
	}
}

Cloud::Account *Model::account() const {
	return _forgotten ? nullptr : _weak.get();
}

bool Model::badgeListed(uint64 id) const {
	if (!_enabled) {
		return false;
	} else if (id == _selfId) {
		const auto strong = account();
		return strong && strong->me().badge;
	}
	return _showBadges && _badges.contains(id);
}

std::optional<Profile> Model::profile(uint64 id) const {
	if (!_enabled) {
		return std::nullopt;
	} else if (id == _selfId) {
		const auto strong = account();
		return (strong && strong->me().valid())
			? ProfileFromMe(strong->me())
			: std::optional<Profile>();
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
	const auto i = _activity.find(id);
	return (i != end(_activity)) ? i->second : Activity();
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
	if (AudienceFromWire(me.activityAudience) == Audience::Nobody) {
		return result;
	}
	result.online = me.chipOnline
		&& (strong->state() == Cloud::State::Online);
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
		checkLists();
		requestActivity();
	} else {
		// The account has cancelled its requests itself, nothing of them
		// comes back.
		_cloud.cancelAll();
		_badgesTimer.cancel();
		_directoryTimer.cancel();
		_publishTimer.cancel();
		_badgesRequest = _directoryRequest = 0;
		_activityRequest = _publishRequest = 0;
		_throttle.reset();
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
	_showProfiles = profiles;
	checkLists();
}

void Model::handleEvent(const Cloud::Event &event) {
	if (event.type == u"activity"_q) {
		applyActivity(ParseActivity(event.data));
	} else if (event.type == u"badges"_q) {
		requestBadges();
	} else if (event.type == u"hello"_q) {
		// The stream is open again: what was missed is asked as a whole.
		_directoryFailed = false;
		requestActivity();
		checkLists();
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

void Model::requestBadges() {
	if (_badgesRequest || !_ready || !_cacheReady || !_showBadges) {
		return;
	}
	auto request = Cloud::GetRequest(u"/v1/badges"_q);
	if (!_cache.badgesEtag.isEmpty()) {
		request.headers.push_back({ "If-None-Match", _cache.badgesEtag });
	}
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
		_cache.badgesEtag = response.etag.left(128);
		if (_cache.badges != *ids) {
			_cache.badges = std::move(*ids);
			rebuildIndex();
			Badge::Refresh();
		}
		saveSoon();
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
				_cache.directoryEtag = response.etag.left(128);
				if (_cache.directory != *users) {
					_cache.directory = std::move(*users);
					rebuildIndex();
				}
				saveSoon();
			}
		}
		scheduleChanges();
	}, [=](const Cloud::Error &error) {
		_directoryRequest = 0;
		_directoryFailed = true;
		scheduleChanges();
	});
}

void Model::requestActivity() {
	if (_activityRequest || !_ready || !_showProfiles) {
		return;
	}
	_activityRequest = _cloud.request(
		Cloud::GetRequest(u"/v1/activity"_q),
		[=](const Cloud::Response &response) {
			_activityRequest = 0;
			_activity.clear();
			for (auto &activity : ParseActivities(response.json)) {
				const auto id = activity.userId;
				_activity.emplace(id, std::move(activity));
			}
			scheduleChanges();
		},
		[=](const Cloud::Error &error) {
			_activityRequest = 0;
		});
}

void Model::applyActivity(Activity &&activity) {
	const auto id = activity.userId;
	if (!id || !_enabled) {
		return;
	}
	const auto i = _activity.find(id);
	if (activity.empty()) {
		if (i == end(_activity)) {
			return;
		}
		_activity.erase(i);
	} else if (i != end(_activity)) {
		if (i->second == activity) {
			return;
		}
		i->second = std::move(activity);
	} else if (int(_activity.size()) < kMaxActivity) {
		_activity.emplace(id, std::move(activity));
	} else {
		return;
	}
	scheduleChanges();
}

void Model::clearActivity() {
	if (!_activity.empty()) {
		_activity.clear();
		scheduleChanges();
	}
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
	_activity.clear();
	_directoryLoaded = false;
	_badgesChecked = _directoryChecked = 0;
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
	_directoryTimer.cancel();
	_saveTimer.cancel();
	_publishTimer.cancel();
	_dirty = false;
	_enabled = false;
	_cache = Cache();
	_activity.clear();
	rebuildIndex();
	WriteBytes(_path, QByteArray(), NextGeneration(_path));
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
// the audience of the activity or while the account is not connected.
// Switching any of that off takes back what was told before.
void Model::publishCheck() {
	const auto strong = account();
	_publishAllowed = strong
		&& _ready
		&& ShouldPublishListening(strong->me());
	const auto now = crl::now();
	const auto changed = _throttle.want(
		_publishAllowed ? currentListening() : std::nullopt,
		now);
	if (changed) {
		publishStep();
	}
}

void Model::publishStep() {
	_publishTimer.cancel();
	if (_publishRequest || !_ready) {
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
	check(hostile.publicPlaylists == 1000, "profile: count is clamped");
	check(hostile.publicPresets == 0, "profile: negative count");
	check(!hostile.hasContent(), "profile: nothing to show");

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

	// A stop that fails is not repeated.
	auto stop = PublishThrottle();
	stop.want(track("a"), start);
	stop.sent(start + kPublishSettle);
	stop.succeeded();
	stop.want(std::nullopt, start + 60 * second);
	check(stop.step(start + 60 * second).send, "throttle: stop goes at once");
	stop.sent(start + 60 * second);
	stop.failed(start + 60 * second, 0);
	step = stop.step(start + 60 * 60 * second);
	check(!step.send && step.wait == 0, "throttle: a failed stop is final");

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
		Global().models.remove(session);
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
		if (model && fail) {
			crl::on_main(model, [=] {
				fail(Cloud::Error{
					.type = Cloud::Error::Type::NotConnected,
				});
			});
		}
		return;
	}
	const auto own = (userId == session->userId().bare);
	if (own) {
		LoadOwnShared(session, std::move(done), std::move(fail));
		return;
	}
	account->request(
		Cloud::GetRequest(u"/v1/users/"_q + QString::number(userId)),
		[=](const Cloud::Response &response) {
			const auto user = response.json.value(u"user"_q).toObject();
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

void SetSharedShown(
		not_null<Main::Session*> session,
		const SharedItem &item,
		bool shown,
		Fn<void()> done,
		Fn<void(const Cloud::Error&)> fail) {
	const auto model = Lookup(session);
	const auto account = model ? model->account() : nullptr;
	if (!account || !Cloud::ValidShareId(item.id)) {
		return;
	}
	auto body = QJsonObject();
	body.insert(u"public"_q, shown);
	account->request(
		Cloud::PatchRequest(
			(item.playlist ? u"/v1/playlists/"_q : u"/v1/presets/"_q)
				+ item.id,
			std::move(body)),
		[=](const Cloud::Response &response) {
			if (done) {
				done();
			}
		},
		std::move(fail));
}

bool FlagNow(not_null<Main::Session*> session, Flag flag) {
	const auto account = &Cloud::For(session);
	return EnabledState(account->state()) && FlagOf(account->me(), flag);
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
	const auto account = &Cloud::For(session);
	const auto &me = account->me();
	return !EnabledState(account->state())
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
		return EnabledState(account->state())
			? int(account->me().chosen.size())
			: 0;
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
	TestChips(check);
	check.section("chips");
	log.push_back(u"cloud_social: %1 checks passed, %2 failed"_q.arg(
		QString::number(check.passed()),
		QString::number(check.failed())));
	return !check.failed();
}

} // namespace Oblivion::Social
