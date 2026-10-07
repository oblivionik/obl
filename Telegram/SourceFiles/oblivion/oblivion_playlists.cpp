/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_playlists.h"

#include "apiwrap.h"
#include "base/random.h"
#include "base/unique_qptr.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_audio_msg_id.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_list_widget.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_instance.h"
#include "media/media_common.h"
#include "oblivion/oblivion_cloud_share.h"
#include "oblivion/oblivion_music_editor.h"
#include "oblivion/oblivion_room.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "settings/settings_common.h"
#include "settings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/effects/ripple_animation.h"
#include "ui/empty_userpic.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/text/format_values.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/wrap/vertical_layout_reorder.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "window/window_session_controller.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

#include "styles/style_boxes.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

namespace Oblivion {
namespace {

constexpr auto kVersion = 1;
constexpr auto kMaxNameLength = 64;
constexpr auto kMaxTracks = 10000;
constexpr auto kRequestLimit = 100;
constexpr auto kCoverSize = 38;
constexpr auto kCoverColors = 7;

struct Track {
	FullMsgId id;
	DocumentId document = 0;
	QString title;
	QString performer;
	QString fileName;
	int duration = 0;

	// Channel access hash, so that the message can be requested after
	// a restart even if the channel itself is not loaded (not joined).
	uint64 accessHash = 0;
};

[[nodiscard]] bool SameTrack(const Track &a, const Track &b) {
	return (a.id == b.id)
		&& (a.document == b.document)
		&& (a.title == b.title)
		&& (a.performer == b.performer)
		&& (a.fileName == b.fileName)
		&& (a.duration == b.duration)
		&& (a.accessHash == b.accessHash);
}

// A fresh track from a loaded message keeps a known access hash
// if the channel of the message has none right now.
[[nodiscard]] Track MergeTrack(const Track &was, Track fresh) {
	if (!fresh.accessHash) {
		fresh.accessHash = was.accessHash;
	}
	return fresh;
}

[[nodiscard]] bool ValidTrackPeer(PeerId peer) {
	return (peer.value & PeerId::kChatTypeMask)
		&& !(peer.value >> 56)
		&& (peerIsUser(peer) || peerIsChat(peer) || peerIsChannel(peer));
}

struct Playlist {
	uint64 id = 0;
	QString name;
	std::vector<Track> tracks;
};

[[nodiscard]] bool Contains(const Playlist &playlist, const Track &track) {
	return ranges::any_of(playlist.tracks, [&](const Track &existing) {
		return (existing.id == track.id)
			|| (track.document && existing.document == track.document);
	});
}

enum class TrackStatus : uchar {
	Ready,
	Loading,
	Unavailable, // Loaded, but the message is gone or has no music.
	Failed, // The request failed, can be retried.
};

[[nodiscard]] DocumentData *TrackDocument(not_null<HistoryItem*> item) {
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	return (document && document->isAudioFile() && !media->ttlSeconds())
		? document
		: nullptr;
}

[[nodiscard]] Track TrackFromItem(
		not_null<HistoryItem*> item,
		not_null<DocumentData*> document) {
	auto result = Track{
		.id = item->fullId(),
		.document = document->id,
		.fileName = document->filename(),
		.duration = int(std::max(document->duration(), crl::time(0))
			/ 1000),
	};
	if (const auto song = document->song()) {
		result.title = song->title;
		result.performer = song->performer;
	}
	if (const auto channel = item->history()->peer->asChannel()) {
		result.accessHash = channel->accessHash();
	}
	return result;
}

[[nodiscard]] QJsonObject SerializeTrack(const Track &track) {
	auto result = QJsonObject();
	result.insert(u"peer"_q, QString::number(track.id.peer.value));
	result.insert(u"msg"_q, QString::number(track.id.msg.bare));
	result.insert(u"doc"_q, QString::number(track.document));
	result.insert(u"title"_q, track.title);
	result.insert(u"performer"_q, track.performer);
	result.insert(u"file"_q, track.fileName);
	result.insert(u"duration"_q, track.duration);
	if (track.accessHash) {
		result.insert(u"hash"_q, QString::number(track.accessHash));
	}
	return result;
}

[[nodiscard]] std::optional<Track> ParseTrack(const QJsonObject &object) {
	const auto peer = PeerId(uint64(
		object.value(u"peer"_q).toString().toULongLong()));
	const auto msg = object.value(u"msg"_q).toString().toLongLong();
	if (!ValidTrackPeer(peer) || !IsServerMsgId(MsgId(msg))) {
		return std::nullopt;
	}
	return Track{
		.id = FullMsgId(peer, MsgId(msg)),
		.document = object.value(u"doc"_q).toString().toULongLong(),
		.title = object.value(u"title"_q).toString(),
		.performer = object.value(u"performer"_q).toString(),
		.fileName = object.value(u"file"_q).toString(),
		.duration = std::max(object.value(u"duration"_q).toInt(), 0),
		.accessHash = (peerIsChannel(peer)
			? object.value(u"hash"_q).toString().toULongLong()
			: uint64()),
	};
}

template <typename T>
void MoveInVector(std::vector<T> &list, int from, int to) {
	const auto count = int(list.size());
	if (from < 0 || from >= count || to < 0 || to >= count || from == to) {
		return;
	} else if (from < to) {
		std::rotate(
			begin(list) + from,
			begin(list) + from + 1,
			begin(list) + to + 1);
	} else {
		std::rotate(
			begin(list) + to,
			begin(list) + from,
			begin(list) + from + 1);
	}
}

class Store final {
public:
	explicit Store(QString path);

	// Not backed by a file, for the UI snapshots.
	explicit Store(std::vector<Playlist> list);

	[[nodiscard]] const std::vector<Playlist> &list();
	[[nodiscard]] const Playlist *find(uint64 id);

	[[nodiscard]] rpl::producer<uint64> changes() const;

	uint64 create(const QString &name);
	void rename(uint64 id, const QString &name);
	void remove(uint64 id);
	void movePlaylist(int from, int to);
	bool add(uint64 id, Track track);
	void removeTrack(uint64 id, FullMsgId trackId);
	void moveTrack(uint64 id, int from, int to);
	void moveTrackBy(uint64 id, FullMsgId trackId, int delta);
	void removeTracks(uint64 id, Fn<bool(const Track&)> predicate);
	void refresh(const Track &fresh);

	[[nodiscard]] TrackStatus status(
		not_null<Main::Session*> session,
		FullMsgId id);
	void retry(FullMsgId id);
	void retryFailed();
	[[nodiscard]] rpl::producer<FullMsgId> resolved() const;

	void forget();

private:
	void ensureLoaded();
	void load();
	void save();
	void changed(uint64 id);
	[[nodiscard]] Playlist *findMutable(uint64 id);
	[[nodiscard]] uint64 storedAccessHash(PeerId peer) const;

	void resetRequests(not_null<Main::Session*> session);
	void scheduleRequests();
	void sendRequests();
	void sendRequest(
		not_null<Main::Session*> session,
		PeerId peer,
		std::vector<FullMsgId> ids);

	const QString _path;
	std::vector<Playlist> _list;
	bool _loaded = false;
	rpl::event_stream<uint64> _changes;

	// Lookups are valid only for the session they were sent in.
	base::weak_ptr<Main::Session> _session;
	base::flat_map<PeerId, std::vector<FullMsgId>> _queued;
	base::flat_set<FullMsgId> _requested;
	base::flat_set<FullMsgId> _failed;
	base::flat_set<FullMsgId> _errors;
	rpl::event_stream<FullMsgId> _resolved;
	bool _sendScheduled = false;

};

Store::Store(QString path) : _path(std::move(path)) {
}

Store::Store(std::vector<Playlist> list)
: _list(std::move(list))
, _loaded(true) {
}

void Store::ensureLoaded() {
	if (!_loaded) {
		_loaded = true;
		load();
	}
}

void Store::load() {
	auto file = QFile(_path);
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(file.readAll(), &error);
	file.close();
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		LOG(("Oblivion: could not parse playlists, keeping a backup."));
		const auto backup = _path + u".bad"_q;
		QFile::remove(backup);
		QFile::rename(_path, backup);
		return;
	}
	const auto playlists = document.object().value(u"playlists"_q).toArray();
	for (const auto &value : playlists) {
		const auto object = value.toObject();
		auto playlist = Playlist{
			.id = object.value(u"id"_q).toString().toULongLong(),
			.name = object.value(
				u"name"_q).toString().trimmed().left(kMaxNameLength),
		};
		if (!playlist.id
			|| playlist.name.isEmpty()
			|| ranges::contains(_list, playlist.id, &Playlist::id)) {
			continue;
		}
		const auto tracks = object.value(u"tracks"_q).toArray();
		for (const auto &track : tracks) {
			if (int(playlist.tracks.size()) >= kMaxTracks) {
				break;
			} else if (auto parsed = ParseTrack(track.toObject())) {
				if (!Contains(playlist, *parsed)) {
					playlist.tracks.push_back(std::move(*parsed));
				}
			}
		}
		_list.push_back(std::move(playlist));
	}
}

void Store::save() {
	if (_path.isEmpty()) {
		return;
	}
	auto playlists = QJsonArray();
	for (const auto &playlist : _list) {
		auto tracks = QJsonArray();
		for (const auto &track : playlist.tracks) {
			tracks.push_back(SerializeTrack(track));
		}
		auto object = QJsonObject();
		object.insert(u"id"_q, QString::number(playlist.id));
		object.insert(u"name"_q, playlist.name);
		object.insert(u"tracks"_q, tracks);
		playlists.push_back(object);
	}
	auto root = QJsonObject();
	root.insert(u"version"_q, kVersion);
	root.insert(u"playlists"_q, playlists);

	QDir().mkpath(QFileInfo(_path).absolutePath());

	auto file = QSaveFile(_path);
	if (!file.open(QIODevice::WriteOnly)) {
		LOG(("Oblivion: could not write playlists to '%1'.").arg(_path));
		return;
	}
	file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	if (!file.commit()) {
		LOG(("Oblivion: could not save playlists to '%1'.").arg(_path));
	}
}

void Store::changed(uint64 id) {
	save();
	_changes.fire_copy(id);
}

const std::vector<Playlist> &Store::list() {
	ensureLoaded();
	return _list;
}

const Playlist *Store::find(uint64 id) {
	ensureLoaded();
	const auto i = ranges::find(_list, id, &Playlist::id);
	return (i != end(_list)) ? &*i : nullptr;
}

Playlist *Store::findMutable(uint64 id) {
	return const_cast<Playlist*>(find(id));
}

rpl::producer<uint64> Store::changes() const {
	return _changes.events();
}

uint64 Store::create(const QString &name) {
	ensureLoaded();
	auto id = uint64();
	do {
		id = base::RandomValue<uint64>() & 0x7FFFFFFFFFFFFFFFULL;
	} while (!id || find(id));
	_list.push_back({
		.id = id,
		.name = name.trimmed().left(kMaxNameLength),
	});
	changed(id);
	return id;
}

void Store::rename(uint64 id, const QString &name) {
	const auto playlist = findMutable(id);
	const auto trimmed = name.trimmed().left(kMaxNameLength);
	if (playlist && !trimmed.isEmpty() && playlist->name != trimmed) {
		playlist->name = trimmed;
		changed(id);
	}
}

void Store::remove(uint64 id) {
	ensureLoaded();
	const auto i = ranges::find(_list, id, &Playlist::id);
	if (i != end(_list)) {
		_list.erase(i);
		changed(id);
	}
}

void Store::movePlaylist(int from, int to) {
	ensureLoaded();
	const auto count = int(_list.size());
	if (from != to
		&& from >= 0
		&& from < count
		&& to >= 0
		&& to < count) {
		MoveInVector(_list, from, to);
		changed(0);
	}
}

bool Store::add(uint64 id, Track track) {
	const auto playlist = findMutable(id);
	if (!playlist
		|| Contains(*playlist, track)
		|| int(playlist->tracks.size()) >= kMaxTracks) {
		return false;
	}
	playlist->tracks.push_back(std::move(track));
	changed(id);
	return true;
}

void Store::removeTrack(uint64 id, FullMsgId trackId) {
	if (const auto playlist = findMutable(id)) {
		const auto i = ranges::find(playlist->tracks, trackId, &Track::id);
		if (i != end(playlist->tracks)) {
			playlist->tracks.erase(i);
			changed(id);
		}
	}
}

void Store::moveTrack(uint64 id, int from, int to) {
	if (const auto playlist = findMutable(id)) {
		const auto count = int(playlist->tracks.size());
		if (from != to
			&& from >= 0
			&& from < count
			&& to >= 0
			&& to < count) {
			MoveInVector(playlist->tracks, from, to);
			changed(id);
		}
	}
}

void Store::moveTrackBy(uint64 id, FullMsgId trackId, int delta) {
	if (const auto playlist = findMutable(id)) {
		const auto &tracks = playlist->tracks;
		const auto i = ranges::find(tracks, trackId, &Track::id);
		if (i != end(tracks)) {
			const auto from = int(i - begin(tracks));
			moveTrack(id, from, from + delta);
		}
	}
}

void Store::removeTracks(uint64 id, Fn<bool(const Track&)> predicate) {
	if (const auto playlist = findMutable(id)) {
		const auto was = playlist->tracks.size();
		playlist->tracks.erase(
			ranges::remove_if(playlist->tracks, predicate),
			end(playlist->tracks));
		if (playlist->tracks.size() != was) {
			changed(id);
		}
	}
}

void Store::refresh(const Track &fresh) {
	ensureLoaded();
	auto changedId = std::optional<uint64>();
	for (auto &playlist : _list) {
		for (auto &track : playlist.tracks) {
			if (track.id != fresh.id) {
				continue;
			}
			auto updated = MergeTrack(track, fresh);
			if (!SameTrack(track, updated)) {
				track = std::move(updated);
				changedId = changedId ? uint64(0) : playlist.id;
			}
		}
	}
	if (changedId) {
		changed(*changedId);
	}
}

TrackStatus Store::status(
		not_null<Main::Session*> session,
		FullMsgId id) {
	if (const auto item = session->data().message(id)) {
		return TrackDocument(item)
			? TrackStatus::Ready
			: TrackStatus::Unavailable;
	} else if (!ValidTrackPeer(id.peer) || !IsServerMsgId(id.msg)) {
		return TrackStatus::Unavailable;
	}
	resetRequests(session);
	if (_failed.contains(id)) {
		return TrackStatus::Unavailable;
	} else if (_errors.contains(id)) {
		return TrackStatus::Failed;
	} else if (_requested.emplace(id).second) {
		_queued[peerIsChannel(id.peer) ? id.peer : PeerId()].push_back(id);
		scheduleRequests();
	}
	return TrackStatus::Loading;
}

void Store::resetRequests(not_null<Main::Session*> session) {
	if (_session.get() == session) {
		return;
	}
	// Lookups sent by a previous session of this account (before a logout
	// and a new login) will never finish, its ApiWrap is destroyed.
	_session = base::make_weak(session);
	_queued.clear();
	_requested.clear();
	_failed.clear();
	_errors.clear();
	_sendScheduled = false;
}

void Store::scheduleRequests() {
	const auto session = _session.get();
	if (_sendScheduled || !session) {
		return;
	}
	_sendScheduled = true;
	crl::on_main(session, [=] {
		sendRequests();
	});
}

void Store::sendRequests() {
	_sendScheduled = false;
	const auto session = _session.get();
	if (!session) {
		return;
	}
	auto queued = base::take(_queued);
	for (auto &[peer, ids] : queued) {
		const auto count = int(ids.size());
		for (auto from = 0; from < count; from += kRequestLimit) {
			const auto till = std::min(from + kRequestLimit, count);
			sendRequest(
				session,
				peer,
				std::vector<FullMsgId>(begin(ids) + from, begin(ids) + till));
		}
	}
}

uint64 Store::storedAccessHash(PeerId peer) const {
	for (const auto &playlist : _list) {
		for (const auto &track : playlist.tracks) {
			if (track.id.peer == peer && track.accessHash) {
				return track.accessHash;
			}
		}
	}
	return 0;
}

void Store::sendRequest(
		not_null<Main::Session*> session,
		PeerId peer,
		std::vector<FullMsgId> ids) {
	auto list = QVector<MTPInputMessage>();
	list.reserve(ids.size());
	for (const auto &id : ids) {
		list.push_back(MTP_inputMessageID(MTP_int(id.msg.bare)));
	}
	const auto channelId = peerToChannel(peer);
	const auto weak = base::make_weak(session);

	// Only a received answer without the message makes a track
	// unavailable, a failed request (for example, a wrong access hash)
	// leaves it retryable, so that "Remove unavailable" keeps it.
	const auto finish = [=](bool received) {
		const auto strong = weak.get();
		if (!strong || _session.get() != strong) {
			return;
		}
		auto resolved = std::vector<FullMsgId>();
		resolved.reserve(ids.size());
		for (const auto &id : ids) {
			if (!_requested.remove(id)) {
				continue;
			} else if (!strong->data().message(id)) {
				(received ? _failed : _errors).emplace(id);
			}
			resolved.push_back(id);
		}
		for (const auto &id : resolved) {
			_resolved.fire_copy(id);
		}
	};
	const auto done = [=](const MTPmessages_Messages &result) {
		if (const auto strong = weak.get()) {
			auto &owner = strong->data();
			owner.processExistingMessages(
				(channelId ? owner.channel(channelId).get() : nullptr),
				result);
		}
		finish(true);
	};
	const auto fail = [=] {
		finish(false);
	};
	if (!channelId) {
		session->api().request(MTPmessages_GetMessages(
			MTP_vector<MTPInputMessage>(std::move(list))
		)).done(done).fail(fail).send();
		return;
	}
	// After a restart a channel the user did not join is not loaded
	// and has no access hash, so use the one saved with its tracks.
	const auto channel = session->data().channel(channelId);
	const auto accessHash = storedAccessHash(peer);
	const auto input = (channel->isLoaded()
		|| channel->accessHash()
		|| !accessHash)
		? channel->inputChannel()
		: MTP_inputChannel(MTP_long(channelId.bare), MTP_long(accessHash));
	session->api().request(MTPchannels_GetMessages(
		input,
		MTP_vector<MTPInputMessage>(std::move(list))
	)).done(done).fail(fail).send();
}

void Store::retry(FullMsgId id) {
	_errors.remove(id);
	_failed.remove(id);
}

void Store::retryFailed() {
	_errors.clear();
	_failed.clear();
}

rpl::producer<FullMsgId> Store::resolved() const {
	return _resolved.events();
}

void Store::forget() {
	_list.clear();
	_queued.clear();
	_requested.clear();
	_failed.clear();
	_errors.clear();

	// Treat as loaded, so the removed file is never read back.
	_loaded = true;
	if (!_path.isEmpty()) {
		QFile::remove(_path);
		QFile::remove(_path + u".bad"_q);
	}
	_changes.fire(0);
}

[[nodiscard]] Store &StoreFor(not_null<Main::Session*> session) {
	static const auto stores = new base::flat_map<
		QString,
		std::unique_ptr<Store>>();
	const auto id = (session->isTestMode() ? u"test_"_q : QString())
		+ QString::number(session->userId().bare);
	auto i = stores->find(id);
	if (i == end(*stores)) {
		i = stores->emplace(
			id,
			std::make_unique<Store>(cWorkingDir()
				+ u"tdata/oblivion/"_q
				+ id
				+ u"/playlists.json"_q)).first;
	}
	return *i->second;
}

class Queue final {
public:
	Queue(
		not_null<Main::Session*> session,
		not_null<Store*> store,
		uint64 playlistId,
		bool shuffle);

	[[nodiscard]] uint64 playlistId() const {
		return _playlistId;
	}
	[[nodiscard]] Main::Session *session() const {
		return _session.get();
	}
	[[nodiscard]] bool active() const {
		return _active;
	}
	[[nodiscard]] bool drives(const AudioMsgId &current) const;
	[[nodiscard]] bool canMove(int delta) const;
	bool move(int delta);
	bool start(int index);
	void deactivate();

private:
	struct Pending {
		FullMsgId id;
		int position = 0;
		int step = 0;
		bool wrap = false;
		int attempts = 0;
	};

	[[nodiscard]] int step(int delta) const;
	[[nodiscard]] int origin(int step) const;
	[[nodiscard]] bool repeatAll() const;
	[[nodiscard]] int currentTrackIndex() const;
	void fillOrder(int first);
	void sync();
	void applyOrderMode(Media::OrderMode mode);
	void resolved(FullMsgId id);
	bool advance(int from, int step, bool wrap, int attempts);
	bool playAt(int position);

	const base::weak_ptr<Main::Session> _session;
	const not_null<Store*> _store;
	const uint64 _playlistId = 0;
	std::vector<FullMsgId> _tracks;
	std::vector<int> _order;
	int _position = -1;
	FullMsgId _current;
	Pending _pending;
	bool _shuffled = false;
	bool _active = true;

	// The current track was removed from the playlist: the playback
	// position is between _position and _position + 1 in _order.
	bool _gap = false;
	rpl::lifetime _lifetime;

};

struct PlayerState {
	std::unique_ptr<Queue> queue;
	rpl::event_stream<> changes;
	rpl::lifetime lifetime;
	bool subscribed = false;
};

[[nodiscard]] PlayerState &PlayerData() {
	static const auto result = new PlayerState();
	return *result;
}

[[nodiscard]] bool IsCurrentTrack(
		not_null<Main::Session*> session,
		FullMsgId id) {
	const auto current = Media::Player::instance()->current(
		AudioMsgId::Type::Song);
	const auto document = current.audio();
	return document
		&& (&document->session() == session)
		&& (current.contextId() == id);
}

[[nodiscard]] bool IsPlayingNow(
		not_null<Main::Session*> session,
		FullMsgId id) {
	using namespace Media::Player;
	return IsCurrentTrack(session, id)
		&& !IsStoppedOrStopping(
			instance()->getState(AudioMsgId::Type::Song).state);
}

Queue::Queue(
	not_null<Main::Session*> session,
	not_null<Store*> store,
	uint64 playlistId,
	bool shuffle)
: _session(base::make_weak(session))
, _store(store)
, _playlistId(playlistId)
, _shuffled(shuffle
	|| (Core::App().settings().playerOrderMode()
		== Media::OrderMode::Shuffle)) {
	if (const auto playlist = _store->find(_playlistId)) {
		_tracks = playlist->tracks
			| ranges::views::transform(&Track::id)
			| ranges::to_vector;
	}
	fillOrder(-1);

	_store->changes(
	) | rpl::filter([=](uint64 id) {
		return !id || (id == _playlistId);
	}) | rpl::on_next([=] {
		sync();
	}, _lifetime);

	_store->resolved(
	) | rpl::on_next([=](FullMsgId id) {
		resolved(id);
	}, _lifetime);

	Core::App().settings().playerOrderModeChanges(
	) | rpl::on_next([=](Media::OrderMode mode) {
		applyOrderMode(mode);
	}, _lifetime);
}

bool Queue::drives(const AudioMsgId &current) const {
	const auto session = _session.get();
	const auto document = current.audio();
	return _active
		&& session
		&& document
		&& (&document->session() == session)
		&& (_current.msg != 0)
		&& (current.contextId() == _current);
}

int Queue::step(int delta) const {
	const auto reverse = !_shuffled
		&& (Core::App().settings().playerOrderMode()
			== Media::OrderMode::Reverse);
	return reverse ? -delta : delta;
}

int Queue::origin(int step) const {
	// Inside a gap _position is the entry right before it,
	// so moving backwards has to start right after it.
	return (_gap && step < 0) ? (_position + 1) : _position;
}

bool Queue::repeatAll() const {
	return (Core::App().settings().playerRepeatMode()
		== Media::RepeatMode::All);
}

int Queue::currentTrackIndex() const {
	const auto i = ranges::find(_tracks, _current);
	return (i != end(_tracks)) ? int(i - begin(_tracks)) : -1;
}

void Queue::fillOrder(int first) {
	const auto count = int(_tracks.size());
	_order = ranges::views::ints(0, count) | ranges::to_vector;
	if (!_shuffled) {
		return;
	}
	for (auto i = count - 1; i > 0; --i) {
		std::swap(_order[i], _order[base::RandomIndex(i + 1)]);
	}
	if (first >= 0 && first < count) {
		const auto i = ranges::find(_order, first);
		std::rotate(begin(_order), i, i + 1);
	}
}

bool Queue::canMove(int delta) const {
	const auto count = int(_order.size());
	if (!_active || !count) {
		return false;
	} else if (repeatAll()) {
		return true;
	}
	const auto shift = step(delta);
	const auto next = origin(shift) + shift;
	return (next >= 0) && (next < count);
}

bool Queue::move(int delta) {
	if (!_active || _order.empty()) {
		return false;
	}
	const auto shift = step(delta);
	return advance(origin(shift), shift, repeatAll(), int(_order.size()));
}

bool Queue::start(int index) {
	const auto session = _session.get();
	const auto count = int(_tracks.size());
	if (!session || !count) {
		return false;
	} else if (index >= count) {
		index = -1;
	}
	if (_shuffled) {
		fillOrder(index);
	}
	if (index >= 0 && IsPlayingNow(session, _tracks[index])) {
		_current = _tracks[index];
		_position = int(ranges::find(_order, index) - begin(_order));
		_gap = false;
		PlayerData().changes.fire({});
		return true;
	}
	if (_shuffled) {
		return advance(-1, 1, false, count);
	}
	const auto forward = step(1);
	const auto from = (index >= 0)
		? index
		: (forward > 0)
		? 0
		: (count - 1);
	return advance(from - forward, forward, repeatAll(), count);
}

void Queue::deactivate() {
	_active = false;
	_pending = Pending();
	PlayerData().changes.fire({});
}

bool Queue::advance(int from, int step, bool wrap, int attempts) {
	const auto session = _session.get();
	const auto count = int(_order.size());
	_pending = Pending();
	if (!_active || !session || !count || !step) {
		return false;
	}
	auto position = from;
	for (auto i = 0; i < attempts; ++i) {
		position += step;
		if (position < 0 || position >= count) {
			if (!wrap) {
				return false;
			}
			position = ((position % count) + count) % count;
		}
		const auto id = _tracks[_order[position]];
		switch (_store->status(session, id)) {
		case TrackStatus::Ready:
			if (playAt(position)) {
				return true;
			}
			break;
		case TrackStatus::Loading:
			_pending = Pending{
				.id = id,
				.position = position,
				.step = step,
				.wrap = wrap,
				.attempts = attempts - i,
			};
			return true;
		case TrackStatus::Unavailable:
		case TrackStatus::Failed:
			break;
		}
	}
	return false;
}

void Queue::resolved(FullMsgId id) {
	if (!_active || !_pending.id || _pending.id != id) {
		return;
	}
	const auto pending = base::take(_pending);
	advance(
		pending.position - pending.step,
		pending.step,
		pending.wrap,
		pending.attempts);
}

bool Queue::playAt(int position) {
	const auto session = _session.get();
	const auto id = _tracks[_order[position]];
	const auto item = session ? session->data().message(id) : nullptr;
	const auto document = item ? TrackDocument(item) : nullptr;
	if (!document) {
		return false;
	}
	const auto wasCurrent = std::exchange(_current, id);
	const auto wasPosition = std::exchange(_position, position);
	const auto wasGap = std::exchange(_gap, false);
	const auto player = Media::Player::instance();
	player->play(AudioMsgId(document, id));
	const auto now = player->current(AudioMsgId::Type::Song);
	if (now.audio() != document || now.contextId() != id) {
		_current = wasCurrent;
		_position = wasPosition;
		_gap = wasGap;
		return false;
	}
	_store->refresh(TrackFromItem(item, document));
	PlayerData().changes.fire({});
	return true;
}

void Queue::sync() {
	const auto playlist = _store->find(_playlistId);
	if (!playlist) {
		deactivate();
		return;
	}
	auto tracks = playlist->tracks
		| ranges::views::transform(&Track::id)
		| ranges::to_vector;
	if (tracks == _tracks) {
		return;
	}
	const auto count = int(tracks.size());

	auto order = std::vector<int>();
	order.reserve(count);
	auto used = std::vector<bool>(count, false);
	auto position = -1;
	auto keptBefore = 0;

	// Inside a gap the entry at _position is before the current one.
	const auto before = _gap ? (_position + 1) : _position;
	for (auto i = 0; i != int(_order.size()); ++i) {
		const auto &id = _tracks[_order[i]];
		const auto j = int(ranges::find(tracks, id) - begin(tracks));
		if (j == count || used[j]) {
			continue;
		}
		used[j] = true;
		if (!_gap && i == _position) {
			position = int(order.size());
		} else if (i < before) {
			++keptBefore;
		}
		order.push_back(j);
	}
	const auto gap = (position < 0);
	if (gap) {
		position = keptBefore - 1;
	}
	if (_shuffled) {
		for (auto j = 0; j != count; ++j) {
			if (!used[j]) {
				const auto from = position + 1;
				const auto size = int(order.size());
				const auto at = from + base::RandomIndex(size - from + 1);
				order.insert(begin(order) + at, j);
			}
		}
	} else {
		const auto current = (position >= 0 && position < int(order.size()))
			? order[position]
			: -1;
		order = ranges::views::ints(0, count) | ranges::to_vector;
		if (current >= 0) {
			position = current;
		}
	}
	_tracks = std::move(tracks);
	_order = std::move(order);
	_position = std::clamp(position, -1, count - 1);
	_gap = gap;
	PlayerData().changes.fire({});
}

void Queue::applyOrderMode(Media::OrderMode mode) {
	const auto shuffle = (mode == Media::OrderMode::Shuffle);
	if (shuffle == _shuffled) {
		return;
	}
	const auto current = currentTrackIndex();
	_shuffled = shuffle;
	fillOrder(current);
	_position = (current < 0)
		? -1
		: int(ranges::find(_order, current) - begin(_order));
	_gap = (current < 0);
	PlayerData().changes.fire({});
}

void EnsurePlayerSubscriptions() {
	auto &state = PlayerData();
	if (state.subscribed) {
		return;
	}
	state.subscribed = true;

	Media::Player::instance()->trackChanged(
	) | rpl::filter(
		rpl::mappers::_1 == AudioMsgId::Type::Song
	) | rpl::on_next([] {
		const auto queue = PlayerData().queue.get();
		if (queue && queue->active()) {
			const auto current = Media::Player::instance()->current(
				AudioMsgId::Type::Song);
			if (!queue->drives(current)) {
				queue->deactivate();
			}
		}
	}, state.lifetime);
}

[[nodiscard]] Queue *ActiveQueue() {
	const auto queue = PlayerData().queue.get();
	return (queue && queue->active()) ? queue : nullptr;
}

[[nodiscard]] bool PlaylistPlaying(
		not_null<Main::Session*> session,
		uint64 playlistId) {
	const auto queue = ActiveQueue();
	const auto current = Media::Player::instance()->current(
		AudioMsgId::Type::Song);
	return queue
		&& (queue->playlistId() == playlistId)
		&& queue->drives(current)
		&& (&current.audio()->session() == session);
}

void StartPlaylist(
		not_null<Window::SessionController*> controller,
		uint64 playlistId,
		int index,
		bool shuffle) {
	EnsurePlayerSubscriptions();

	auto &state = PlayerData();
	const auto session = &controller->session();
	state.queue = std::make_unique<Queue>(
		session,
		&StoreFor(session),
		playlistId,
		shuffle);
	if (!state.queue->start(index)) {
		state.queue = nullptr;
		state.changes.fire({});
		controller->showToast(tr::lng_oblivion_playlists_nothing(tr::now));
	}
}

[[nodiscard]] rpl::producer<> PlayerChanges() {
	return rpl::merge(
		Media::Player::instance()->trackChanged() | rpl::to_empty,
		Media::Player::instance()->stops(AudioMsgId::Type::Song),
		PlayerData().changes.events());
}

class ListRow final : public Ui::RippleButton {
public:
	explicit ListRow(QWidget *parent);

	void setContent(
		QString index,
		QString title,
		QString status,
		QString right);

	// A round cover in the color of the playlist instead of the index,
	// the texts are aligned with the settings buttons above the rows.
	void setCover(uint64 playlistId);

	void setGreyed(bool greyed);
	void setActive(bool active);

	[[nodiscard]] rpl::producer<> menuRequests() const {
		return _menuRequests.events();
	}

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	QImage prepareRippleMask() const override;

private:
	void paintCover(QPainter &p) const;

	const style::PeerListItem &_st;
	const not_null<Ui::IconButton*> _more;
	QString _index;
	QString _title;
	QString _status;
	QString _right;
	std::optional<uint8> _cover;
	bool _greyed = false;
	bool _active = false;
	rpl::event_stream<> _menuRequests;

};

ListRow::ListRow(QWidget *parent)
: RippleButton(parent, st::defaultRippleAnimation)
, _st(st::defaultPeerListItem)
, _more(Ui::CreateChild<Ui::IconButton>(this, st::themesMenuToggle)) {
	_more->setClickedCallback([=] {
		_menuRequests.fire({});
	});
	resize(width(), _st.height);
}

void ListRow::setContent(
		QString index,
		QString title,
		QString status,
		QString right) {
	_index = std::move(index);
	_title = std::move(title);
	_status = std::move(status);
	_right = std::move(right);
	setAccessibleName(_title);
	update();
}

void ListRow::setCover(uint64 playlistId) {
	_cover = uint8(playlistId % kCoverColors);
	update();
}

void ListRow::setGreyed(bool greyed) {
	if (_greyed != greyed) {
		_greyed = greyed;
		update();
	}
}

void ListRow::setActive(bool active) {
	if (_active != active) {
		_active = active;
		update();
	}
}

int ListRow::resizeGetHeight(int newWidth) {
	// The dots are in one column with the box title menu dots
	// (st::boxTitleMenu is flush right, its icon is centered in it).
	_more->moveToRight(
		std::max(st::boxTitleMenu.width / 2 - _more->width() / 2, 0),
		(_st.height - _more->height()) / 2,
		newWidth);
	return _st.height;
}

void ListRow::paintCover(QPainter &p) const {
	// Centered under the round icon of the "New playlist" button.
	const auto size = style::ConvertScale(kCoverSize);
	const auto center = st::settingsButtonActive.iconLeft
		+ st::settingsIconAdd.width() / 2;
	const auto rect = QRect(
		center - size / 2,
		(height() - size) / 2,
		size,
		size);
	const auto colors = Ui::EmptyUserpic::UserpicColor(*_cover);
	{
		auto hq = PainterHighQualityEnabler(p);
		auto gradient = QLinearGradient(rect.topLeft(), rect.bottomLeft());
		gradient.setStops({
			{ 0., colors.color1->c },
			{ 1., colors.color2->c },
		});
		p.setPen(Qt::NoPen);
		p.setBrush(gradient);
		p.drawEllipse(rect);
	}
	st::infoIconMediaAudio.paintInCenter(
		p,
		rect,
		st::historyPeerUserpicFg->c);
}

void ListRow::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);

	const auto over = isOver() || isDown();
	p.fillRect(e->rect(), over ? st::windowBgOver : st::windowBg);
	paintRipple(p, 0, 0);

	const auto skip = st::normalFont->spacew * 2;
	auto left = st::boxRowPadding.left();
	if (_cover) {
		paintCover(p);
		left = st::settingsButtonActive.padding.left();
	} else if (!_index.isEmpty()) {
		const auto indexWidth = st::normalFont->width(u"0000"_q);
		p.setFont(st::normalFont);
		p.setPen(_active
			? st::windowActiveTextFg
			: over
			? st::windowSubTextFgOver
			: st::windowSubTextFg);
		p.drawText(
			QRect(left - skip, 0, indexWidth, height()),
			_index,
			style::al_center);
		left += indexWidth;
	}

	// A row without the second line has its title centered vertically.
	const auto titleTop = _status.isEmpty()
		? (height() - st::semiboldFont->height) / 2
		: _st.namePosition.y();

	// The right text (a duration) is centered vertically, in one line
	// with the index and the menu dots, both text lines end before it.
	auto right = _more->x();
	if (!_right.isEmpty()) {
		const auto rightWidth = st::normalFont->width(_right);
		p.setFont(st::normalFont);
		p.setPen(over ? st::windowSubTextFgOver : st::windowSubTextFg);
		p.drawTextLeft(
			right - rightWidth,
			(height() - st::normalFont->height) / 2,
			width(),
			_right);
		right -= rightWidth + st::boxLittleSkip;
	}
	const auto textWidth = right - left;
	if (textWidth > 0) {
		p.setFont(st::semiboldFont);
		p.setPen(_greyed
			? st::windowSubTextFg
			: _active
			? st::windowActiveTextFg
			: st::contactsNameFg);
		p.drawTextLeft(
			left,
			titleTop,
			width(),
			st::semiboldFont->elided(_title, textWidth));
	}
	if (textWidth > 0 && !_status.isEmpty()) {
		p.setFont(st::normalFont);
		p.setPen(over ? st::windowSubTextFgOver : st::windowSubTextFg);
		p.drawTextLeft(
			left,
			_st.statusPosition.y(),
			width(),
			st::normalFont->elided(_status, textWidth));
	}
}

void ListRow::contextMenuEvent(QContextMenuEvent *e) {
	e->accept();
	_menuRequests.fire({});
}

QImage ListRow::prepareRippleMask() const {
	return Ui::RippleAnimation::RectMask(size());
}

struct TrackTexts {
	QString title;
	QString status;
};

[[nodiscard]] TrackTexts ComposeTexts(const Track &track) {
	auto title = track.title.trimmed();
	auto performer = track.performer.trimmed();
	const auto file = track.fileName.trimmed();
	if (title.isEmpty()) {
		title = !file.isEmpty()
			? file
			: tr::lng_oblivion_playlists_unknown(tr::now);
	} else if (performer.isEmpty() && file != title) {
		performer = file;
	}
	return { title, performer };
}

[[nodiscard]] QString PlaylistStatus(const Playlist &playlist) {
	auto total = int64();
	for (const auto &track : playlist.tracks) {
		total += track.duration;
	}
	auto result = tr::lng_oblivion_playlists_tracks_count(
		tr::now,
		lt_count,
		int(playlist.tracks.size()));
	if (total > 0) {
		result += QString::fromUtf8(" \xC2\xB7 ")
			+ Ui::FormatDurationText(total);
	}
	return result;
}

void NameBox(
		not_null<Ui::GenericBox*> box,
		rpl::producer<QString> title,
		QString initial,
		rpl::producer<QString> submitText,
		Fn<void(QString)> done) {
	box->setTitle(std::move(title));

	const auto field = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::lng_oblivion_playlists_name(),
			initial));
	field->setMaxLength(kMaxNameLength);
	box->setFocusCallback([=] {
		field->setFocusFast();
		field->selectAll();
	});

	const auto submit = [=] {
		const auto name = field->getLastText().trimmed();
		if (name.isEmpty()) {
			field->showError();
			return;
		}
		box->closeBox();
		done(name);
	};
	field->submits() | rpl::on_next(submit, field->lifetime());

	box->addButton(std::move(submitText), submit);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void ShowNameBox(
		std::shared_ptr<Ui::Show> show,
		rpl::producer<QString> title,
		QString initial,
		rpl::producer<QString> submitText,
		Fn<void(QString)> done) {
	show->showBox(Box(
		NameBox,
		std::move(title),
		std::move(initial),
		std::move(submitText),
		std::move(done)));
}

void ConfirmDeletePlaylist(
		std::shared_ptr<Ui::Show> show,
		not_null<Store*> store,
		uint64 playlistId) {
	const auto playlist = store->find(playlistId);
	if (!playlist) {
		return;
	}
	show->showBox(Ui::MakeConfirmBox({
		.text = tr::lng_oblivion_playlists_delete_sure(
			lt_name,
			rpl::single(playlist->name)),
		.confirmed = [=](Fn<void()> close) {
			store->remove(playlistId);
			close();
		},
		.confirmText = tr::lng_box_delete(),
		.confirmStyle = &st::attentionBoxButton,
	}));
}

void RenamePlaylist(
		std::shared_ptr<Ui::Show> show,
		not_null<Store*> store,
		uint64 playlistId) {
	const auto playlist = store->find(playlistId);
	if (!playlist) {
		return;
	}
	ShowNameBox(
		std::move(show),
		tr::lng_oblivion_playlists_rename_title(),
		playlist->name,
		tr::lng_settings_save(),
		[=](QString name) { store->rename(playlistId, name); });
}

void AddTrack(
		not_null<Window::SessionController*> controller,
		uint64 playlistId,
		Track track) {
	const auto store = &StoreFor(&controller->session());
	const auto playlist = store->find(playlistId);
	if (!playlist) {
		return;
	}
	const auto name = playlist->name;
	controller->showToast(store->add(playlistId, std::move(track))
		? tr::lng_oblivion_playlists_added(tr::now, lt_name, name)
		: tr::lng_oblivion_playlists_already(tr::now, lt_name, name));
}

void GoToTrack(
		not_null<Window::SessionController*> controller,
		FullMsgId id) {
	if (const auto item = controller->session().data().message(id)) {
		controller->hideLayer();
		controller->showMessage(item);
	}
}

// The boxes reach the session (loaded messages, the player, navigation)
// only through a PlaylistsBackend, so that the UI snapshots can show
// them with sample data and no session.
class PlaylistsBackend {
public:
	virtual ~PlaylistsBackend() = default;

	[[nodiscard]] virtual not_null<Store*> store() = 0;
	[[nodiscard]] virtual TrackStatus status(FullMsgId id) = 0;

	// The data of a Ready track, taken from its loaded message.
	[[nodiscard]] virtual std::optional<Track> loaded(FullMsgId id) = 0;

	// The message is loaded and has music, so it can be played right away.
	[[nodiscard]] virtual bool playable(FullMsgId id) = 0;

	[[nodiscard]] virtual bool current(FullMsgId id) = 0;
	[[nodiscard]] virtual bool playing(uint64 playlistId) = 0;
	virtual void start(uint64 playlistId, int index, bool shuffle) = 0;
	virtual void playPause() = 0;

	// "Go to message" and the music editor for a playable track.
	virtual void addMessageActions(
		not_null<Ui::PopupMenu*> menu,
		FullMsgId id) = 0;

	[[nodiscard]] virtual rpl::producer<FullMsgId> removed() = 0;
	[[nodiscard]] virtual rpl::producer<> playerChanges() = 0;

	// Oblivion round 5: sharing (oblivion_cloud_share.h). «Поделиться…»
	// in the menu of a playlist and the way to «Общие плейлисты», nothing
	// without a session.
	virtual void addShareActions(
			not_null<Ui::PopupMenu*> menu,
			uint64 playlistId) {
	}
	[[nodiscard]] virtual Fn<void()> sharedLibrary() {
		return nullptr;
	}

};

class SessionPlaylistsBackend final : public PlaylistsBackend {
public:
	explicit SessionPlaylistsBackend(
		not_null<Window::SessionController*> controller);

	not_null<Store*> store() override;
	TrackStatus status(FullMsgId id) override;
	std::optional<Track> loaded(FullMsgId id) override;
	bool playable(FullMsgId id) override;
	bool current(FullMsgId id) override;
	bool playing(uint64 playlistId) override;
	void start(uint64 playlistId, int index, bool shuffle) override;
	void playPause() override;
	void addMessageActions(
		not_null<Ui::PopupMenu*> menu,
		FullMsgId id) override;
	rpl::producer<FullMsgId> removed() override;
	rpl::producer<> playerChanges() override;
	void addShareActions(
		not_null<Ui::PopupMenu*> menu,
		uint64 playlistId) override;
	Fn<void()> sharedLibrary() override;

private:
	[[nodiscard]] HistoryItem *playableItem(FullMsgId id) const;

	const not_null<Window::SessionController*> _controller;
	const not_null<Main::Session*> _session;
	const not_null<Store*> _store;

};

SessionPlaylistsBackend::SessionPlaylistsBackend(
	not_null<Window::SessionController*> controller)
: _controller(controller)
, _session(&controller->session())
, _store(&StoreFor(_session)) {
}

not_null<Store*> SessionPlaylistsBackend::store() {
	return _store;
}

TrackStatus SessionPlaylistsBackend::status(FullMsgId id) {
	return _store->status(_session, id);
}

HistoryItem *SessionPlaylistsBackend::playableItem(FullMsgId id) const {
	const auto item = _session->data().message(id);
	return (item && TrackDocument(item)) ? item : nullptr;
}

std::optional<Track> SessionPlaylistsBackend::loaded(FullMsgId id) {
	if (const auto item = playableItem(id)) {
		return TrackFromItem(item, TrackDocument(item));
	}
	return std::nullopt;
}

bool SessionPlaylistsBackend::playable(FullMsgId id) {
	return (playableItem(id) != nullptr);
}

bool SessionPlaylistsBackend::current(FullMsgId id) {
	return IsCurrentTrack(_session, id);
}

bool SessionPlaylistsBackend::playing(uint64 playlistId) {
	return PlaylistPlaying(_session, playlistId);
}

void SessionPlaylistsBackend::start(
		uint64 playlistId,
		int index,
		bool shuffle) {
	StartPlaylist(_controller, playlistId, index, shuffle);
}

void SessionPlaylistsBackend::playPause() {
	Media::Player::instance()->playPause(AudioMsgId::Type::Song);
}

void SessionPlaylistsBackend::addMessageActions(
		not_null<Ui::PopupMenu*> menu,
		FullMsgId id) {
	const auto item = playableItem(id);
	if (!item) {
		return;
	}
	const auto controller = _controller;
	menu->addAction(
		tr::lng_context_to_msg(tr::now),
		[=] { GoToTrack(controller, id); },
		&st::menuIconShowInChat);

	// The music editor saves and sends the file, so it follows
	// the same content protection rule as the chat menus.
	using HistoryView::CopyRestrictionType;
	const auto restriction = HistoryView::CopyMediaRestrictionTypeFor(
		item->history()->peer,
		item);
	if (restriction == CopyRestrictionType::None) {
		AddMusicEditorAction(menu, controller, item);
	}
	Rooms::AddToRoomAction(menu, controller, item); // Oblivion rooms.
}

rpl::producer<FullMsgId> SessionPlaylistsBackend::removed() {
	return _session->data().itemRemoved(
	) | rpl::map([](not_null<const HistoryItem*> item) {
		return item->fullId();
	});
}

rpl::producer<> SessionPlaylistsBackend::playerChanges() {
	return PlayerChanges();
}

// Oblivion round 5: sharing. The tracks go to Share::SharePlaylist() as
// plain values, it asks the consent and a confirmation before anything is
// taken from Telegram or sent.
void SessionPlaylistsBackend::addShareActions(
		not_null<Ui::PopupMenu*> menu,
		uint64 playlistId) {
	const auto playlist = _store->find(playlistId);
	if (!playlist || playlist->tracks.empty()) {
		return;
	}
	const auto controller = _controller;
	const auto session = _session;
	const auto store = _store;
	menu->addAction(tr::lng_oblivion_share_playlist_menu(tr::now), [=] {
		const auto playlist = store->find(playlistId);
		if (!playlist) {
			return;
		}
		auto tracks = std::vector<Share::LocalTrack>();
		tracks.reserve(playlist->tracks.size());
		for (const auto &track : playlist->tracks) {
			// Messages that are not in the memory yet are asked for now,
			// they are there by the time their turn comes.
			[[maybe_unused]] const auto status = store->status(
				session,
				track.id);
			tracks.push_back({
				.peer = track.id.peer.value,
				.msg = track.id.msg.bare,
				.document = track.document,
				.title = track.title,
				.performer = track.performer,
				.fileName = track.fileName,
				.duration = track.duration,
			});
		}
		Share::SharePlaylist(
			controller,
			playlistId,
			playlist->name,
			std::move(tracks));
	}, &st::menuIconShare);
}

Fn<void()> SessionPlaylistsBackend::sharedLibrary() {
	const auto controller = _controller;
	return [=] {
		Share::ShowLibrary(controller);
	};
}

struct ListState {
	std::unique_ptr<Ui::VerticalLayoutReorder> reorder;
	base::unique_qptr<Ui::PopupMenu> menu;
	Fn<void()> rebuild;
	int reordering = 0;
	bool scheduled = false;
	bool delayed = false;
};

void ScheduleRebuild(not_null<QWidget*> guard, not_null<ListState*> state) {
	if (state->scheduled) {
		return;
	}
	state->scheduled = true;
	Ui::PostponeCall(guard, [=] {
		state->scheduled = false;
		if (state->reordering) {
			state->delayed = true;
		} else if (state->rebuild) {
			state->rebuild();
		}
	});
}

void SetupReorder(
		not_null<Ui::GenericBox*> box,
		not_null<Ui::VerticalLayout*> list,
		not_null<ListState*> state,
		Fn<void(int, int)> moved) {
	state->reorder = std::make_unique<Ui::VerticalLayoutReorder>(list);
	state->reorder->updates(
	) | rpl::on_next([=](Ui::VerticalLayoutReorder::Single data) {
		using State = Ui::VerticalLayoutReorder::State;
		if (data.state == State::Started) {
			++state->reordering;
			return;
		}
		Ui::PostponeCall(box, [=] {
			--state->reordering;
			if (!state->reordering && state->delayed) {
				state->delayed = false;
				ScheduleRebuild(box, state);
			}
		});
		if (data.state == State::Applied) {
			moved(data.oldPosition, data.newPosition);
		}
	}, box->lifetime());
}

void ShowMenu(
		not_null<ListState*> state,
		not_null<QWidget*> parent,
		Fn<void(not_null<Ui::PopupMenu*>)> fill) {
	state->menu = base::make_unique_q<Ui::PopupMenu>(
		parent,
		st::popupMenuWithIcons);
	fill(state->menu.get());
	if (state->menu->empty()) {
		state->menu = nullptr;
	} else {
		state->menu->popup(QCursor::pos());
	}
}

void PlaylistBox(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<Ui::Show> show,
		std::shared_ptr<PlaylistsBackend> backend,
		uint64 playlistId) {
	struct State {
		ListState list;
		std::vector<std::pair<not_null<ListRow*>, FullMsgId>> rows;
		style::RoundButton shuffleSt = st::defaultLightButton;
	};
	const auto store = backend->store();
	const auto owned = box->lifetime().make_state<State>();
	const auto state = &owned->list;
	const auto rows = &owned->rows;

	store->retryFailed();

	box->setWidth(st::boxWideWidth);
	box->setMaxHeight(st::boxMaxListHeight);
	box->setTitle(rpl::single(
		playlistId
	) | rpl::then(
		store->changes()
	) | rpl::map([=] {
		const auto playlist = store->find(playlistId);
		return playlist ? playlist->name : QString();
	}));

	const auto unavailable = [=](const Track &track) {
		return (backend->status(track.id) == TrackStatus::Unavailable);
	};
	const auto top = box->addTopButton(st::boxTitleMenu);
	top->setClickedCallback([=] {
		ShowMenu(state, box, [=](not_null<Ui::PopupMenu*> menu) {
			const auto playlist = store->find(playlistId);
			if (!playlist) {
				return;
			}
			menu->addAction(
				tr::lng_oblivion_playlists_rename(tr::now),
				[=] { RenamePlaylist(show, store, playlistId); },
				&st::menuIconEdit);
			backend->addShareActions(menu, playlistId); // Oblivion round 5.
			if (ranges::any_of(playlist->tracks, unavailable)) {
				menu->addAction(
					tr::lng_oblivion_playlists_remove_unavailable(tr::now),
					[=] { store->removeTracks(playlistId, unavailable); },
					&st::menuIconClear);
			}
			menu->addAction(
				tr::lng_oblivion_playlists_delete(tr::now),
				[=] { ConfirmDeletePlaylist(show, store, playlistId); },
				&st::menuIconDelete);
		});
	});

	const auto content = box->verticalLayout();

	const auto controls = content->add(
		object_ptr<Ui::SlideWrap<Ui::RpWidget>>(
			content,
			object_ptr<Ui::RpWidget>(content),
			st::boxRowPadding + style::margins(
				0,
				st::boxLittleSkip,
				0,
				st::boxLittleSkip)),
		style::margins());
	const auto buttons = controls->entity();
	const auto play = Ui::CreateChild<Ui::RoundButton>(
		buttons,
		tr::lng_oblivion_playlists_play(),
		st::defaultActiveButton);
	owned->shuffleSt.textBg = st::lightButtonBgOver;
	const auto shuffle = Ui::CreateChild<Ui::RoundButton>(
		buttons,
		tr::lng_oblivion_playlists_shuffle(),
		owned->shuffleSt);
	buttons->resize(
		buttons->width(),
		std::max(play->height(), shuffle->height()));
	buttons->widthValue(
	) | rpl::on_next([=](int width) {
		const auto skip = st::boxLittleSkip;
		const auto half = std::max((width - skip) / 2, 0);
		play->setFullWidth(half);
		shuffle->setFullWidth(std::max(width - skip - half, 0));
		play->moveToLeft(0, 0, width);
		shuffle->moveToLeft(half + skip, 0, width);
	}, buttons->lifetime());
	play->setClickedCallback([=] {
		backend->start(playlistId, -1, false);
	});
	shuffle->setClickedCallback([=] {
		backend->start(playlistId, -1, true);
	});

	const auto empty = content->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			content,
			object_ptr<Ui::FlatLabel>(
				content,
				tr::lng_oblivion_playlists_no_tracks(),
				st::membersAbout),
			st::boxRowPadding + style::margins(
				0,
				st::boxMediumSkip,
				0,
				st::boxMediumSkip)),
		style::margins());
	const auto list = content->add(
		object_ptr<Ui::VerticalLayout>(content),
		style::margins());
	const auto about = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)),
		style::margins());
	Ui::AddSkip(about->entity());
	Ui::AddDividerText(about->entity(), tr::lng_oblivion_playlists_about());

	const auto updateActive = [=] {
		for (const auto &[row, id] : *rows) {
			row->setActive(backend->current(id));
		}
	};

	// Returns the index of the track or -1 if it is not in the playlist.
	const auto indexOf = [=](FullMsgId id) {
		const auto playlist = store->find(playlistId);
		if (!playlist) {
			return -1;
		}
		const auto &tracks = playlist->tracks;
		const auto i = ranges::find(tracks, id, &Track::id);
		return (i != end(tracks)) ? int(i - begin(tracks)) : -1;
	};

	const auto showTrackMenu = [=](FullMsgId id) {
		ShowMenu(state, box, [=](not_null<Ui::PopupMenu*> menu) {
			const auto index = indexOf(id);
			if (index < 0) {
				return;
			}
			const auto count = int(store->find(playlistId)->tracks.size());
			if (backend->playable(id)) {
				menu->addAction(
					tr::lng_oblivion_playlists_play_from(tr::now),
					[=] {
						const auto index = indexOf(id);
						if (index >= 0) {
							backend->start(playlistId, index, false);
						}
					},
					&st::menuIconSoundOn);
				backend->addMessageActions(menu, id);
			}
			if (index > 0) {
				menu->addAction(
					tr::lng_oblivion_playlists_move_up(tr::now),
					[=] { store->moveTrackBy(playlistId, id, -1); },
					&st::menuIconAbove);
			}
			if (index + 1 < count) {
				menu->addAction(
					tr::lng_oblivion_playlists_move_down(tr::now),
					[=] { store->moveTrackBy(playlistId, id, 1); },
					&st::menuIconBelow);
			}
			menu->addAction(
				tr::lng_oblivion_playlists_remove_track(tr::now),
				[=] { store->removeTrack(playlistId, id); },
				&st::menuIconDelete);
		});
	};

	const auto clicked = [=](FullMsgId id) {
		if (state->reordering) {
			return;
		}
		const auto index = indexOf(id);
		if (index < 0) {
			return;
		}
		const auto status = backend->status(id);
		if (status == TrackStatus::Unavailable) {
			show->showToast(
				tr::lng_oblivion_playlists_unavailable_toast(tr::now));
		} else if (backend->current(id) && backend->playing(playlistId)) {
			backend->playPause();
		} else {
			if (status == TrackStatus::Failed) {
				// Request it again, it is played once it is loaded.
				store->retry(id);
				ScheduleRebuild(box, state);
			}
			backend->start(playlistId, index, false);
		}
	};

	state->rebuild = [=] {
		const auto playlist = store->find(playlistId);
		if (!playlist) {
			box->closeBox();
			return;
		}
		state->reorder->cancel();
		rows->clear();
		list->clear();

		auto fresh = std::vector<Track>();
		auto index = 0;
		for (const auto &track : playlist->tracks) {
			const auto id = track.id;
			const auto status = backend->status(id);
			auto shown = track;
			if (status == TrackStatus::Ready) {
				if (auto loaded = backend->loaded(id)) {
					const auto updated = MergeTrack(
						track,
						std::move(*loaded));
					if (!SameTrack(updated, track)) {
						fresh.push_back(updated);
					}
					shown = updated;
				}
			}
			const auto texts = ComposeTexts(shown);
			const auto row = list->add(object_ptr<ListRow>(list));
			row->setContent(
				QString::number(++index),
				texts.title,
				((status == TrackStatus::Unavailable)
					? tr::lng_oblivion_playlists_unavailable(tr::now)
					: (status == TrackStatus::Failed)
					? tr::lng_oblivion_playlists_load_failed(tr::now)
					: (status == TrackStatus::Loading
						&& texts.status.isEmpty())
					? tr::lng_oblivion_playlists_loading(tr::now)
					: texts.status),
				(shown.duration > 0
					? Ui::FormatDurationText(shown.duration)
					: QString()));
			row->setGreyed(status == TrackStatus::Unavailable
				|| status == TrackStatus::Failed);
			row->setClickedCallback([=] { clicked(id); });
			row->menuRequests() | rpl::on_next([=] {
				showTrackMenu(id);
			}, row->lifetime());
			rows->emplace_back(row, id);
		}
		updateActive();

		const auto has = !playlist->tracks.empty();
		empty->toggle(!has, anim::type::instant);
		controls->toggle(has, anim::type::instant);
		about->toggle(has, anim::type::instant);
		box->setAdditionalTitle(rpl::single(has
			? QString::number(playlist->tracks.size())
			: QString()));

		state->reorder->start();

		for (const auto &track : fresh) {
			store->refresh(track);
		}
	};

	SetupReorder(box, list, state, [=](int from, int to) {
		store->moveTrack(playlistId, from, to);
	});

	store->changes(
	) | rpl::filter([=](uint64 id) {
		return !id || (id == playlistId);
	}) | rpl::on_next([=] {
		ScheduleRebuild(box, state);
	}, box->lifetime());

	const auto listed = [=](FullMsgId id) {
		return ranges::contains(*rows, id, [](const auto &pair) {
			return pair.second;
		});
	};
	rpl::merge(
		store->resolved(),
		backend->removed()
	) | rpl::filter(listed) | rpl::on_next([=] {
		ScheduleRebuild(box, state);
	}, box->lifetime());

	backend->playerChanges(
	) | rpl::on_next(updateActive, box->lifetime());

	state->rebuild();

	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

void PlaylistsBox(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<Ui::Show> show,
		std::shared_ptr<PlaylistsBackend> backend) {
	struct State {
		ListState list;
		std::vector<std::pair<not_null<ListRow*>, uint64>> rows;
	};
	const auto store = backend->store();
	const auto owned = box->lifetime().make_state<State>();
	const auto state = &owned->list;
	const auto rows = &owned->rows;

	box->setWidth(st::boxWideWidth);
	box->setMaxHeight(st::boxMaxListHeight);
	box->setTitle(tr::lng_oblivion_tools_playlists());

	const auto content = box->verticalLayout();
	const auto open = [=](uint64 id) {
		show->showBox(Box(PlaylistBox, show, backend, id));
	};

	const auto create = ::Settings::AddButtonWithIcon(
		content,
		tr::lng_oblivion_playlists_new(),
		st::settingsButtonActive,
		{
			&st::settingsIconAdd,
			::Settings::IconType::Round,
			&st::windowBgActive,
		});
	create->setClickedCallback([=] {
		ShowNameBox(
			show,
			tr::lng_oblivion_playlists_new(),
			QString(),
			tr::lng_oblivion_playlists_create(),
			[=](QString name) { open(store->create(name)); });
	});

	const auto list = content->add(
		object_ptr<Ui::VerticalLayout>(content),
		style::margins());
	const auto empty = content->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			content,
			object_ptr<Ui::FlatLabel>(
				content,
				tr::lng_oblivion_playlists_empty(),
				st::membersAbout),
			st::boxRowPadding + style::margins(
				0,
				st::boxMediumSkip,
				0,
				st::boxMediumSkip)),
		style::margins());
	const auto about = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)),
		style::margins());
	Ui::AddSkip(about->entity());
	Ui::AddDividerText(
		about->entity(),
		tr::lng_oblivion_playlists_list_about());

	const auto updateActive = [=] {
		for (const auto &[row, id] : *rows) {
			row->setActive(backend->playing(id));
		}
	};

	const auto showMenu = [=](uint64 id) {
		ShowMenu(state, box, [=](not_null<Ui::PopupMenu*> menu) {
			const auto playlist = store->find(id);
			if (!playlist) {
				return;
			}
			if (!playlist->tracks.empty()) {
				menu->addAction(
					tr::lng_oblivion_playlists_play(tr::now),
					[=] { backend->start(id, -1, false); },
					&st::menuIconSoundOn);
				menu->addAction(
					tr::lng_oblivion_playlists_shuffle(tr::now),
					[=] { backend->start(id, -1, true); },
					&st::menuIconChangeOrder);
			}
			menu->addAction(
				tr::lng_oblivion_playlists_rename(tr::now),
				[=] { RenamePlaylist(show, store, id); },
				&st::menuIconEdit);
			backend->addShareActions(menu, id); // Oblivion round 5.
			menu->addAction(
				tr::lng_oblivion_playlists_delete(tr::now),
				[=] { ConfirmDeletePlaylist(show, store, id); },
				&st::menuIconDelete);
		});
	};

	state->rebuild = [=] {
		state->reorder->cancel();
		rows->clear();
		list->clear();

		const auto &playlists = store->list();
		for (const auto &playlist : playlists) {
			const auto id = playlist.id;
			const auto row = list->add(object_ptr<ListRow>(list));
			row->setCover(id);
			row->setContent(
				QString(),
				playlist.name,
				PlaylistStatus(playlist),
				QString());
			row->setClickedCallback([=] {
				if (!state->reordering) {
					open(id);
				}
			});
			row->menuRequests() | rpl::on_next([=] {
				showMenu(id);
			}, row->lifetime());
			rows->emplace_back(row, id);
		}
		updateActive();

		empty->toggle(playlists.empty(), anim::type::instant);
		about->toggle(!playlists.empty(), anim::type::instant);

		state->reorder->start();
	};

	SetupReorder(box, list, state, [=](int from, int to) {
		store->movePlaylist(from, to);
	});

	store->changes(
	) | rpl::on_next([=] {
		ScheduleRebuild(box, state);
	}, box->lifetime());

	backend->playerChanges(
	) | rpl::on_next(updateActive, box->lifetime());

	state->rebuild();

	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	// Oblivion round 5: sharing.
	if (const auto library = backend->sharedLibrary()) {
		box->addLeftButton(tr::lng_oblivion_share_library(), library);
	}
}

// Snapshot scenes (OBLIVION_SELFTEST=ui, see oblivion_ui_snapshots.h).
// The sample playlists live in memory, nothing is read or saved.

constexpr auto kSampleWindowWidth = 480;
constexpr auto kSampleChannelId = ChannelId(1000000001);
constexpr auto kSampleRoadId = uint64(4001);
constexpr auto kSampleJazzId = uint64(4002);
constexpr auto kSampleSportId = uint64(4003);
constexpr auto kSampleFavoriteId = uint64(4004);
constexpr auto kSampleTracksPerPlaylist = 100;

class SamplePlaylistsBackend final : public PlaylistsBackend {
public:
	SamplePlaylistsBackend(
		std::vector<Playlist> list,
		base::flat_set<FullMsgId> unavailable,
		FullMsgId current,
		uint64 playing)
	: _store(std::move(list))
	, _unavailable(std::move(unavailable))
	, _current(current)
	, _playing(playing) {
	}

	not_null<Store*> store() override {
		return &_store;
	}
	TrackStatus status(FullMsgId id) override {
		return _unavailable.contains(id)
			? TrackStatus::Unavailable
			: TrackStatus::Ready;
	}
	std::optional<Track> loaded(FullMsgId) override {
		return std::nullopt;
	}
	bool playable(FullMsgId id) override {
		return (status(id) == TrackStatus::Ready);
	}
	bool current(FullMsgId id) override {
		return _current && (id == _current);
	}
	bool playing(uint64 playlistId) override {
		return _playing && (playlistId == _playing);
	}
	void start(uint64, int, bool) override {
	}
	void playPause() override {
	}
	void addMessageActions(not_null<Ui::PopupMenu*>, FullMsgId) override {
	}
	rpl::producer<FullMsgId> removed() override {
		return rpl::never<FullMsgId>();
	}
	rpl::producer<> playerChanges() override {
		return rpl::never<>();
	}

private:
	Store _store;
	const base::flat_set<FullMsgId> _unavailable;
	const FullMsgId _current;
	const uint64 _playing = 0;

};

[[nodiscard]] FullMsgId SampleTrackId(uint64 playlistId, int index) {
	const auto bare = int64(playlistId % 1000) * kSampleTracksPerPlaylist
		+ index
		+ 1;
	return FullMsgId(peerFromChannel(kSampleChannelId), MsgId(bare));
}

struct SampleTrackData {
	QString title;
	QString performer;
	QString fileName;
	int duration = 0;
};

[[nodiscard]] Playlist SamplePlaylist(
		uint64 id,
		QString name,
		std::vector<SampleTrackData> tracks) {
	auto result = Playlist{ .id = id, .name = std::move(name) };
	for (auto &track : tracks) {
		const auto trackId = SampleTrackId(id, int(result.tracks.size()));
		result.tracks.push_back(Track{
			.id = trackId,
			.document = DocumentId(trackId.msg.bare),
			.title = std::move(track.title),
			.performer = std::move(track.performer),
			.fileName = std::move(track.fileName),
			.duration = track.duration,
		});
	}
	return result;
}

// The second track is playing, the sixth one was deleted from its chat,
// the seventh one has no song tags, only the file name.
constexpr auto kSampleCurrentIndex = 1;
constexpr auto kSampleUnavailableIndex = 5;

[[nodiscard]] Playlist SampleRoadPlaylist() {
	return SamplePlaylist(kSampleRoadId, u"Для дороги"_q, {
		{ u"Ночная трасса"_q, u"Северный ветер"_q, u"trassa.mp3"_q, 227 },
		{ u"Golden Hour"_q, u"Neon Harbor"_q, u"golden_hour.mp3"_q, 252 },
		{ u"Дорога домой"_q, u"Полярная ночь"_q, u"domoy.mp3"_q, 185 },
		{
			u"Midnight Train to Nowhere (Extended Mix)"_q,
			u"Low Tide Radio"_q,
			u"midnight_train.mp3"_q,
			331,
		},
		{ u"Километры"_q, u"Белые ночи"_q, u"km.mp3"_q, 178 },
		{ u"Летний дождь"_q, u"Сад камней"_q, u"dozhd.mp3"_q, 203 },
		{ QString(), QString(), u"road_mix_2026.mp3"_q, 296 },
		{ u"Рассвет над заливом"_q, u"Тихий океан"_q, u"rassvet.mp3"_q, 362 },
	});
}

// Short enough to fit the box without scrolling, with the tip below.
[[nodiscard]] Playlist SampleJazzPlaylist() {
	return SamplePlaylist(kSampleJazzId, u"Вечерний джаз"_q, {
		{ u"Синий час"_q, u"Квартет «Мост»"_q, u"siniy_chas.mp3"_q, 264 },
		{ u"Осенние огни"_q, u"Трио Невы"_q, u"ogni.mp3"_q, 218 },
		{
			u"Velvet Avenue"_q,
			u"Late Night Trio"_q,
			u"velvet_avenue.mp3"_q,
			305,
		},
		{ u"Кофе после полуночи"_q, u"Оркестр «Лира»"_q, u"kofe.mp3"_q, 241 },
		{ u"Последний трамвай"_q, u"Квартет «Мост»"_q, u"tramvay.mp3"_q, 167 },
	});
}

[[nodiscard]] std::vector<SampleTrackData> SampleGeneratedTracks(
		int count,
		int seed) {
	auto result = std::vector<SampleTrackData>();
	for (auto i = 0; i != count; ++i) {
		const auto number = QString::number(i + 1);
		result.push_back({
			.title = u"Track "_q + number,
			.performer = u"Sample Band"_q,
			.fileName = u"track_"_q + number + u".mp3"_q,
			.duration = 150 + ((seed + i * 53) % 190),
		});
	}
	return result;
}

[[nodiscard]] std::vector<Playlist> SamplePlaylists() {
	return {
		SampleRoadPlaylist(),
		SampleJazzPlaylist(),
		SamplePlaylist(
			kSampleSportId,
			u"Тренировка"_q,
			SampleGeneratedTracks(14, 31)),
	};
}

[[nodiscard]] std::shared_ptr<PlaylistsBackend> SampleBackendWith(
		std::vector<Playlist> list,
		FullMsgId current = FullMsgId(),
		uint64 playing = 0) {
	auto unavailable = base::flat_set<FullMsgId>{
		SampleTrackId(kSampleRoadId, kSampleUnavailableIndex),
	};
	return std::make_shared<SamplePlaylistsBackend>(
		std::move(list),
		std::move(unavailable),
		current,
		playing);
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	const auto size = QSize(style::ConvertScale(kSampleWindowWidth), 0);
	const auto current = SampleTrackId(kSampleRoadId, kSampleCurrentIndex);

	RegisterBoxScene(u"playlists_list"_q, size, [=](
			std::shared_ptr<Ui::Show> show) {
		return Box(
			PlaylistsBox,
			show,
			SampleBackendWith(SamplePlaylists(), current, kSampleRoadId));
	});
	RegisterBoxScene(u"playlists_tracks"_q, size, [=](
			std::shared_ptr<Ui::Show> show) {
		return Box(
			PlaylistBox,
			show,
			SampleBackendWith(SamplePlaylists(), current, kSampleRoadId),
			kSampleRoadId);
	});
	RegisterBoxScene(u"playlists_tracks_short"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(
			PlaylistBox,
			show,
			SampleBackendWith(SamplePlaylists()),
			kSampleJazzId);
	});
	RegisterBoxScene(u"playlists_empty"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(PlaylistsBox, show, SampleBackendWith({}));
	});
	RegisterBoxScene(u"playlists_tracks_empty"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		auto list = std::vector<Playlist>{
			Playlist{ .id = kSampleFavoriteId, .name = u"Любимое"_q },
		};
		return Box(
			PlaylistBox,
			show,
			SampleBackendWith(std::move(list)),
			kSampleFavoriteId);
	});
	RegisterBoxScene(u"playlists_name"_q, size, [](
			std::shared_ptr<Ui::Show>) {
		return Box(
			NameBox,
			tr::lng_oblivion_playlists_new(),
			QString(),
			tr::lng_oblivion_playlists_create(),
			Fn<void(QString)>([](QString) {}));
	});
});

} // namespace

void ShowPlaylists(not_null<Window::SessionController*> controller) {
	controller->show(Box(
		PlaylistsBox,
		controller->uiShow(),
		std::make_shared<SessionPlaylistsBackend>(controller)));
}

void AddToPlaylistMenu(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item) {
	// Oblivion rooms: «Добавить в комнату» while a room window is open.
	Rooms::AddToRoomAction(menu, controller, item);
	const auto document = TrackDocument(item);
	const auto session = &controller->session();
	if (!document
		|| !item->isRegular()
		|| item->isScheduled()
		|| item->isSavedMusicItem()
		|| !IsServerMsgId(item->id)
		|| (&item->history()->session() != session)) {
		return;
	}
	const auto store = &StoreFor(session);
	const auto track = TrackFromItem(item, document);
	const auto weak = base::make_weak(controller);

	auto submenu = std::make_unique<Ui::PopupMenu>(menu, menu->st());
	for (const auto &playlist : store->list()) {
		const auto id = playlist.id;
		submenu->addAction(
			Ui::Text::FixAmpersandInAction(playlist.name),
			[=] {
				if (const auto strong = weak.get()) {
					AddTrack(strong, id, track);
				}
			});
	}
	if (!submenu->empty()) {
		submenu->addSeparator();
	}
	submenu->addAction(
		tr::lng_oblivion_playlists_new_menu(tr::now),
		[=] {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			ShowNameBox(
				strong->uiShow(),
				tr::lng_oblivion_playlists_new(),
				QString(),
				tr::lng_oblivion_playlists_create(),
				[=](QString name) {
					if (const auto strong = weak.get()) {
						const auto store = &StoreFor(&strong->session());
						AddTrack(strong, store->create(name), track);
					}
				});
		},
		&st::menuIconAdd);
	menu->addAction(
		tr::lng_oblivion_playlists_add_to(tr::now),
		std::move(submenu),
		&st::menuIconAddToFolder);
}

void ForgetPlaylists(not_null<Main::Session*> session) {
	auto &state = PlayerData();
	if (state.queue) {
		const auto owner = state.queue->session();
		if (!owner || owner == session) {
			state.queue = nullptr;
			state.changes.fire({});
		}
	}
	StoreFor(session).forget();
}

bool PlaylistDrivesPlayer(const AudioMsgId &current) {
	const auto queue = PlayerData().queue.get();
	return queue && queue->drives(current);
}

bool PlaylistPlayerCanMove(int delta) {
	const auto queue = PlayerData().queue.get();
	return queue && queue->canMove(delta);
}

bool PlaylistPlayerMove(int delta, [[maybe_unused]] bool autonext) {
	const auto queue = PlayerData().queue.get();
	return queue && queue->move(delta);
}

rpl::producer<> PlaylistPlayerChanges() {
	return PlayerData().changes.events();
}

// Oblivion rooms: «Добавить из плейлиста».
std::vector<RoomPlaylistBrief> RoomPlaylists(
		not_null<Main::Session*> session) {
	auto result = std::vector<RoomPlaylistBrief>();
	for (const auto &playlist : StoreFor(session).list()) {
		result.push_back({
			.id = playlist.id,
			.name = playlist.name,
			.count = int(playlist.tracks.size()),
		});
	}
	return result;
}

std::vector<RoomPlaylistTrack> RoomPlaylistTracks(
		not_null<Main::Session*> session,
		uint64 playlistId) {
	auto result = std::vector<RoomPlaylistTrack>();
	const auto store = &StoreFor(session);
	const auto playlist = store->find(playlistId);
	if (!playlist) {
		return result;
	}
	const auto tracks = playlist->tracks;
	for (const auto &track : tracks) {
		const auto status = store->status(session, track.id);
		result.push_back({
			.id = track.id,
			.title = track.title,
			.performer = track.performer,
			.fileName = track.fileName,
			.duration = track.duration,
			.ready = (status == TrackStatus::Ready),
			.failed = (status == TrackStatus::Unavailable)
				|| (status == TrackStatus::Failed),
		});
	}
	return result;
}

rpl::producer<> RoomPlaylistsChanges(not_null<Main::Session*> session) {
	const auto store = &StoreFor(session);
	return rpl::merge(
		store->changes() | rpl::to_empty,
		store->resolved() | rpl::to_empty);
}

} // namespace Oblivion
