/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_room_music.h"

#include "base/call_delayed.h"
#include "base/random.h"
#include "base/timer.h"
#include "base/unique_qptr.h"
#include "calls/calls_instance.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_location.h"
#include "core/file_utilities.h"
#include "data/data_audio_msg_id.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_instance.h"
#include "oblivion/oblivion_lang.h"
#include "oblivion/oblivion_look.h"
#include "oblivion/oblivion_playlists.h"
#include "oblivion/oblivion_room_window.h"
#include "oblivion/oblivion_settings.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "storage/file_download.h"
#include "ui/boxes/confirm_box.h"
#include "ui/image/image.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "settings.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_media_player.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtGui/QCursor>
#include <QtGui/QMouseEvent>

#include <map>

namespace Oblivion::Rooms {
namespace {

using SongType = AudioMsgId::Type;

constexpr auto kLeadMax = crl::time(800);
constexpr auto kAwaitStep = int64(30);
constexpr auto kAwaitSlack = int64(400);
constexpr auto kAwaitTimeout = int64(5000);
constexpr auto kSeekThreshold = int64(1200);
constexpr auto kSeekCooldown = crl::time(3000);
constexpr auto kSeekCooldownMax = crl::time(30000);
constexpr auto kNudgeStart = int64(80);
constexpr auto kNudgeStop = int64(25);
constexpr auto kNudgeStep = 0.1;
constexpr auto kEndMargin = int64(400);
constexpr auto kEndSlack = int64(5000);
constexpr auto kTick = crl::time(1000);
constexpr auto kOwnActionGuard = crl::time(800);
constexpr auto kRetryDelay = crl::time(15000);
constexpr auto kRetryLimit = 3;
constexpr auto kStartLimit = 3;
constexpr auto kPendingLimit = 5;
constexpr auto kBatchLimit = 20;
constexpr auto kPositionTick = crl::time(250);
constexpr auto kSeekShown = crl::time(1500);
constexpr auto kVolumeSaveDelay = crl::time(200);

// The looks («Тема Oblivion»), at the 100% scale: the place of the ring
// around the round cover of «Ночной эфир», the square cover of «Тишина»
// in a narrow and in a wide window, the sizes of the titles.
constexpr auto kCoverRing = 10;
constexpr auto kSilenceCover = 96;
constexpr auto kSilenceCoverWide = 116;
constexpr auto kAirTitle = 20;
constexpr auto kSilenceTitle = 22;

[[nodiscard]] int Scaled(int value) {
	return style::ConvertScale(value);
}

[[nodiscard]] int64 ToMs(int64 value, int frequency) {
	return (value > 0 && frequency > 0) ? (value * 1000 / frequency) : 0;
}

void Toast(const std::shared_ptr<Ui::Show> &show, const QString &text) {
	if (show && show->valid() && !text.isEmpty()) {
		show->showToast(text);
	}
}

[[nodiscard]] std::vector<not_null<MusicEngine*>> &Engines() {
	static auto result = std::vector<not_null<MusicEngine*>>();
	return result;
}

[[nodiscard]] rpl::event_stream<> &PlayerChangesStream() {
	static auto result = rpl::event_stream<>();
	return result;
}

[[nodiscard]] MusicEngine *EngineFor(const AudioMsgId &current) {
	for (const auto &engine : Engines()) {
		if (engine->drives(current)) {
			return engine;
		}
	}
	return nullptr;
}

[[nodiscard]] DocumentData *MusicDocument(not_null<HistoryItem*> item) {
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	return (document && document->isAudioFile() && !media->ttlSeconds())
		? document
		: nullptr;
}

} // namespace

StartPlan PlanStart(
		const PlayerState &state,
		int64 duration,
		int64 serverNow,
		crl::time lead) {
	if (state.itemId.isEmpty() || !state.playing) {
		return {};
	}
	auto result = StartPlan{ .play = true };
	const auto limit = std::max(duration, int64(0));
	const auto begin = serverNow + int64(std::max(lead, crl::time(0)));
	if (begin < state.anchor) {
		result.wait = crl::time(state.anchor - begin);
		result.from = std::clamp(state.position, int64(0), limit);
	} else {
		result.from = PositionAt(state, duration, begin);
	}
	result.over = (duration > 0)
		&& (PositionAt(state, duration, serverNow) >= duration - kEndMargin);
	return result;
}

StopKind ClassifyStop(bool atEnd, bool error, bool endedKnown) {
	// The player reports the end, then it is cleared and reports a plain
	// stop of the same track: only a plain stop that follows no end is
	// the user's own.
	return atEnd
		? StopKind::Ended
		: error
		? StopKind::Failed
		: endedKnown
		? StopKind::AfterEnd
		: StopKind::ByUser;
}

void SyncCorrector::reset() {
	_seeked = false;
	_cooldown = kSeekCooldown;
	_count = 0;
	_drift = 0;
	_speed = 1.;
}

void SyncCorrector::started(crl::time now, int64 target, bool cold) {
	_awaiting = true;
	_cold = cold;
	_issuedAt = now;
	_issuedTarget = target;
	_count = 0;
	_speed = 1.;
}

SyncCorrector::Result SyncCorrector::update(
		crl::time now,
		int64 expected,
		int64 actual) {
	if (_awaiting) {
		// Until the sound really goes the player reports the position it
		// was sent to. It can't be further than the time that has passed.
		const auto elapsed = int64(now - _issuedAt);
		const auto moved = actual - _issuedTarget;
		if (moved < kAwaitStep || moved > elapsed + kAwaitSlack) {
			if (elapsed > kAwaitTimeout) {
				_awaiting = false;
			}
			return { .speed = _speed };
		}
		_awaiting = false;
		_count = 0;
		if (!_cold) {
			const auto latency = crl::time(elapsed - moved);
			_lead = std::clamp(
				(_lead + latency) / 2,
				crl::time(0),
				kLeadMax);
		}
	}
	// The position of the player comes in steps of about 50 ms: the
	// median of the last three samples is what is corrected.
	_recent[_count % 3] = actual - expected;
	++_count;
	const auto have = std::min(_count, 3);
	int64 sorted[3] = { _recent[0], _recent[1], _recent[2] };
	std::sort(sorted, sorted + have);
	_drift = sorted[(have - 1) / 2];

	if (std::abs(_drift) > kSeekThreshold) {
		_speed = 1.;
		if (_seeked && (now - _lastSeek < _cooldown)) {
			return { .speed = _speed };
		}
		if (_seeked) {
			_cooldown = std::min(_cooldown * 2, kSeekCooldownMax);
		}
		_seeked = true;
		_lastSeek = now;
		const auto target = std::max(expected + int64(_lead), int64(0));
		started(now, target, false);
		return { .seek = true, .target = target, .speed = 1. };
	}
	_cooldown = kSeekCooldown;
	if (std::abs(_drift) <= kNudgeStart) {
		_seeked = false;
	}
	if (_speed == 1.) {
		if (_drift > kNudgeStart) {
			_speed = 1. - kNudgeStep; // Ahead of the room: slower.
		} else if (_drift < -kNudgeStart) {
			_speed = 1. + kNudgeStep;
		}
	} else if (std::abs(_drift) < kNudgeStop
		|| (_speed < 1. && _drift < 0)
		|| (_speed > 1. && _drift > 0)) {
		_speed = 1.;
	}
	return { .speed = _speed };
}

// ---- The engine.

struct MusicEngine::Private {
	struct Entry {
		QString media;
		QString path;
		DocumentData *document = nullptr;
		Cloud::TransferId transfer = 0;
		bool requested = false;
		bool failed = false;
		int failures = 0;
		crl::time failedAt = 0;
		int percent = 0;
	};
	struct PendingStart {
		DocumentData *document = nullptr;
		crl::time position = -1;
	};
	struct PendingDocument {
		not_null<DocumentData*> document;
		std::shared_ptr<Data::DocumentMedia> media;
		FullMsgId origin;
		std::shared_ptr<Ui::Show> show;
	};

	Private(not_null<MusicEngine*> owner, not_null<Room*> room);

	void sync();
	void playerUpdated();
	void trackChanged();
	void seekFinished();
	void closeRequested();

	enum class Added {
		Sent, // The file is here, it goes to the relay.
		Loading, // It is being downloaded from Telegram first.
		Failed,
		Busy, // Too many are being downloaded already.
		Skipped, // A track of the room itself, or it waits already.
	};

	[[nodiscard]] Entry &ensureEntry(const QueueItem &item);
	[[nodiscard]] Entry *findEntry(const QString &itemId);
	void cleanupEntries();
	void retryFailed();
	void fileReady(const QString &itemId, const QString &path);
	void fileFailed(const QString &itemId);
	void fileProgress(const QString &itemId, int64 ready, int64 total);
	[[nodiscard]] DocumentData *makeDocument(
		const QueueItem &item,
		const QString &path);

	[[nodiscard]] bool oursCurrent() const;
	[[nodiscard]] bool endedHere(const QueueItem &item) const;
	void startPlay(not_null<DocumentData*> document, int64 from, bool cold);
	void seekLocal(int64 target);
	void resumeLocal(int64 from);
	void pauseLocal();
	void holdOurs();
	void stopOurs();
	void setSpeed(double value);
	void applyVolume();
	void restoreVolume();
	void setStatus(LocalStatus value);

	[[nodiscard]] bool tryUpload(const PendingDocument &pending);
	void checkPending(not_null<DocumentData*> document);
	[[nodiscard]] Added start(
		not_null<DocumentData*> document,
		FullMsgId origin,
		std::shared_ptr<Ui::Show> show);
	void feedBacklog();
	void scheduleFeed();

	const not_null<MusicEngine*> owner;
	const not_null<Room*> room;
	std::map<QString, Entry> entries;
	// The documents made for the files of the room, with those files.
	base::flat_map<not_null<DocumentData*>, QString> documents;
	std::vector<PendingDocument> pending;
	std::vector<FullMsgId> backlog; // «Добавить все»: what waits in line.
	std::shared_ptr<Ui::Show> backlogShow;
	bool feeding = false;
	bool feedScheduled = false;
	PendingStart pendingStart;
	SyncCorrector corrector;
	base::Timer timer;
	rpl::variable<LocalStatus> status;
	QString itemId; // The current item of the room the engine has seen.
	QString playingItem; // The item whose document was given to the player.
	// The item that was played to its end at the rev endedRev of the
	// room: nothing is started for it again, the server switches.
	QString endedItem;
	int64 endedRev = -1;
	int64 startRev = -1;
	int startAttempts = 0;
	crl::time pauseIssuedAt = 0;
	crl::time playIssuedAt = 0;
	double speed = 1.;
	bool following = true;
	bool pausedByUs = false;
	bool acting = false;
	bool volumeScaled = false;
	rpl::lifetime lifetime;

};

MusicEngine::Private::Private(
	not_null<MusicEngine*> owner,
	not_null<Room*> room)
: owner(owner)
, room(room)
, timer([=] { sync(); }) {
	const auto player = Media::Player::instance();
	player->updatedNotifier(
	) | rpl::filter([](const Media::Player::TrackState &state) {
		return (state.id.type() == SongType::Song);
	}) | rpl::on_next([=](const Media::Player::TrackState &) {
		playerUpdated();
	}, lifetime);

	player->trackChanged(
	) | rpl::filter([](AudioMsgId::Type type) {
		return (type == SongType::Song);
	}) | rpl::on_next([=](AudioMsgId::Type) {
		trackChanged();
	}, lifetime);

	player->seekingChanges(
		SongType::Song
	) | rpl::filter([](Media::Player::Instance::Seeking seeking) {
		return (seeking == Media::Player::Instance::Seeking::Finish);
	}) | rpl::on_next([=](Media::Player::Instance::Seeking) {
		seekFinished();
	}, lifetime);

	player->closePlayerRequests(
	) | rpl::on_next([=] {
		closeRequested();
	}, lifetime);

	room->changes(
	) | rpl::on_next([=](Changes changes) {
		if (changes & (Changes(Change::Rights) | Change::MusicQueue)) {
			PlayerChangesStream().fire({});
		}
		const auto back = (changes & Change::Connection)
			&& this->room->connected();
		if (back) {
			// The connection is here again: what could not be downloaded
			// while it was away gets its attempts once more.
			retryFailed();
		}
		if (back || (changes & (Changes(Change::MusicQueue)
			| Change::MusicPlayer
			| Change::Gone
			| Change::Reloaded))) {
			if (changes & Change::MusicPlayer) {
				corrector.reset();
			}
			sync();
		}
		if (changes & Change::Uploads) {
			scheduleFeed();
		}
	}, lifetime);

	Oblivion::Get().changes(
	) | rpl::on_next([=] {
		applyVolume();
	}, lifetime);

	Core::App().settings().songVolumeChanges(
	) | rpl::on_next([=](float64) {
		applyVolume();
		crl::on_main(owner, [=] {
			applyVolume();
		});
	}, lifetime);

	if (const auto session = room->session()) {
		session->data().documentLoadProgress(
		) | rpl::on_next([=](not_null<DocumentData*> document) {
			checkPending(document);
		}, lifetime);
	}
}

bool MusicEngine::Private::oursCurrent() const {
	const auto current = Media::Player::instance()->current(SongType::Song);
	const auto document = current.audio();
	return document && documents.contains(document);
}

// The file may be a bit shorter than the room thinks (a duration that
// came from Telegram is in whole seconds). Once the item was played to its
// end here, at this state of the room, nothing of it is started or
// corrected again and the next track that may play already is not
// interrupted: the server switches in a moment. A file that ends long
// before its time is another story, that one is not "ended".
bool MusicEngine::Private::endedHere(const QueueItem &item) const {
	const auto &state = room->player(Kind::Music).state;
	return (endedRev == state.rev)
		&& (endedItem == item.id)
		&& (PositionAt(state, item.duration, room->now())
			>= item.duration - kEndSlack);
}

MusicEngine::Private::Entry *MusicEngine::Private::findEntry(
		const QString &itemId) {
	const auto i = entries.find(itemId);
	return (i != end(entries)) ? &i->second : nullptr;
}

MusicEngine::Private::Entry &MusicEngine::Private::ensureEntry(
		const QueueItem &item) {
	auto &entry = entries[item.id];
	if (entry.media != item.media) {
		entry = Entry{ .media = item.media };
	}
	if (entry.failed
		&& entry.failures < kRetryLimit
		&& (crl::now() - entry.failedAt > kRetryDelay)) {
		entry.failed = false;
		entry.requested = false;
	}
	if (entry.document || entry.requested || entry.failed) {
		return entry;
	}
	entry.requested = true;
	const auto id = item.id;
	const auto guard = owner.get();
	entry.transfer = room->download(
		item,
		crl::guard(guard, [=](const QString &path) {
			fileReady(id, path);
		}),
		crl::guard(guard, [=](const Cloud::Error &) {
			fileFailed(id);
		}),
		crl::guard(guard, [=](int64 ready, int64 total) {
			fileProgress(id, ready, total);
		}));
	return entry;
}

void MusicEngine::Private::cleanupEntries() {
	const auto &data = room->player(Kind::Music);
	for (auto i = begin(entries); i != end(entries);) {
		if (data.find(i->first) || i->first == playingItem) {
			++i;
			continue;
		}
		if (i->second.transfer) {
			room->cancelDownload(i->second.transfer);
		}
		i = entries.erase(i);
	}
}

// The files that could not be downloaded get their attempts again: the
// connection has come back, or the user has asked for it.
void MusicEngine::Private::retryFailed() {
	for (auto &[id, entry] : entries) {
		if (entry.failed && !entry.document && !entry.transfer) {
			entry.failed = false;
			entry.requested = false;
			entry.failures = 0;
			entry.percent = 0;
		}
	}
	startAttempts = 0;
}

DocumentData *MusicEngine::Private::makeDocument(
		const QueueItem &item,
		const QString &path) {
	const auto session = room->session();
	const auto size = QFileInfo(path).size();
	if (!session || size <= 0) {
		return nullptr;
	}
	const auto document = session->data().document(
		base::RandomValue<DocumentId>());
	const auto seconds = int(std::max(item.duration / 1000, int64(1)));
	// The attribute goes without the title and the performer: with both
	// of them the document asks Telegram for an album cover at once, and
	// this one is not a document of Telegram (nothing about a track of
	// a room is ever sent there). The texts for the player bar are set
	// right after, they are read when the bar is painted.
	document->setattributes({
		MTP_documentAttributeAudio(
			MTP_flags(0),
			MTP_int(seconds),
			MTPstring(),
			MTPstring(),
			MTPstring()),
		MTP_documentAttributeFilename(MTP_string(item.fileName.isEmpty()
			? u"track"_q
			: item.fileName)),
	});
	if (const auto song = document->song()) {
		song->title = item.title;
		song->performer = item.performer;
	}
	document->size = size;
	document->setLocation(Core::FileLocation(path));
	return document->filepath(true).isEmpty() ? nullptr : document.get();
}

void MusicEngine::Private::fileReady(
		const QString &itemId,
		const QString &path) {
	const auto entry = findEntry(itemId);
	const auto item = room->player(Kind::Music).find(itemId);
	if (!entry) {
		return;
	}
	entry->transfer = 0;
	if (!item || item->media != entry->media) {
		entries.erase(itemId);
		return;
	}
	entry->path = path;
	entry->percent = 100;
	entry->document = makeDocument(*item, path);
	if (entry->document) {
		documents.emplace(entry->document, path);
	} else {
		entry->failed = true;
		entry->failures = kRetryLimit;
	}
	sync();
}

void MusicEngine::Private::fileFailed(const QString &itemId) {
	if (const auto entry = findEntry(itemId)) {
		entry->transfer = 0;
		entry->failed = true;
		entry->failedAt = crl::now();
		++entry->failures;
		sync();
	}
}

void MusicEngine::Private::fileProgress(
		const QString &itemId,
		int64 ready,
		int64 total) {
	const auto entry = findEntry(itemId);
	if (!entry || total <= 0) {
		return;
	}
	const auto percent = int(std::clamp(
		ready * 100 / total,
		int64(0),
		int64(99)));
	if (entry->percent == percent) {
		return;
	}
	entry->percent = percent;
	if (itemId == this->itemId && !entry->document) {
		room->reportStatus(Kind::Music, itemId, false, percent);
		setStatus({ .state = LocalState::Loading, .percent = percent });
	}
}

void MusicEngine::Private::startPlay(
		not_null<DocumentData*> document,
		int64 from,
		bool cold) {
	acting = true;
	pausedByUs = false;
	speed = 1.;
	pendingStart = { .document = document, .position = crl::time(from) };
	Media::Player::instance()->play(AudioMsgId(document, FullMsgId()));
	pendingStart = {};
	acting = false;
	playIssuedAt = crl::now();
	corrector.started(playIssuedAt, from, cold);
	applyVolume();
	PlayerChangesStream().fire({});
}

void MusicEngine::Private::seekLocal(int64 target) {
	const auto player = Media::Player::instance();
	const auto state = player->getState(SongType::Song);
	const auto length = ToMs(state.length, state.frequency);
	if (length <= 0) {
		return;
	}
	const auto to = std::clamp(target, int64(0), length);
	acting = true;
	pausedByUs = false;
	player->finishSeeking(SongType::Song, to / float64(length));
	acting = false;
	playIssuedAt = crl::now();
}

void MusicEngine::Private::resumeLocal(int64 from) {
	const auto player = Media::Player::instance();
	seekLocal(from);
	// The seek starts the playback, but it can't be done while the
	// length of the track is not known yet.
	const auto state = player->getState(SongType::Song);
	if (Media::Player::IsPausedOrPausing(state.state)) {
		acting = true;
		pausedByUs = false;
		player->play(SongType::Song);
		acting = false;
	}
	playIssuedAt = crl::now();
	corrector.started(playIssuedAt, from, false);
}

void MusicEngine::Private::pauseLocal() {
	acting = true;
	pausedByUs = true;
	Media::Player::instance()->pause(SongType::Song);
	acting = false;
	pauseIssuedAt = crl::now();
	setSpeed(1.);
}

// The player is not left playing a track of the room when the room does
// not play it: it is on pause, or the file of the current item is not
// here yet.
void MusicEngine::Private::holdOurs() {
	if (!oursCurrent()) {
		return;
	}
	const auto state = Media::Player::instance()->getState(SongType::Song);
	if (state.id && Media::Player::ShowPauseIcon(state.state)) {
		pauseLocal();
	}
}

void MusicEngine::Private::stopOurs() {
	if (oursCurrent()) {
		const auto player = Media::Player::instance();
		const auto voice = player->getState(AudioMsgId::Type::Voice);
		acting = true;
		if (voice.id && !Media::Player::IsStoppedOrStopping(voice.state)) {
			// A voice message is in the player too (playing or on
			// pause): only the track of the room goes, the message and
			// the bar with it stay.
			player->stop(SongType::Song);
		} else {
			player->stopAndClose();
		}
		acting = false;
	}
	playingItem = QString();
	restoreVolume();
}

void MusicEngine::Private::setSpeed(double value) {
	const auto changed = (speed != value);
	speed = value;
	if (changed || value != 1.) {
		if (oursCurrent()) {
			Media::Player::instance()->updatePlaybackSpeed();
		}
	}
}

// The own volume of the room music: the volume of the app scaled while
// a track of the room is in the player, the setting of the app itself
// is never changed.
void MusicEngine::Private::applyVolume() {
	const auto base = Core::App().settings().songVolume();
	const auto scale = Oblivion::Get().roomMusicVolume() / 100.;
	if (oursCurrent()) {
		Media::Player::mixer()->setSongVolume(base * scale);
		volumeScaled = (scale < 1.);
	} else if (volumeScaled) {
		Media::Player::mixer()->setSongVolume(base);
		volumeScaled = false;
	}
}

void MusicEngine::Private::restoreVolume() {
	if (volumeScaled) {
		volumeScaled = false;
		Media::Player::mixer()->setSongVolume(
			Core::App().settings().songVolume());
	}
}

void MusicEngine::Private::setStatus(LocalStatus value) {
	status = value;
}

void MusicEngine::Private::sync() {
	timer.cancel();
	const auto &state = room->state();
	if (state.gone != Gone::No) {
		backlog.clear();
		stopOurs();
		setStatus({});
		return;
	}
	const auto &data = state.music;
	const auto item = data.current();
	cleanupEntries();
	if (!item) {
		itemId = QString();
		holdOurs();
		room->reportStatus(Kind::Music, QString(), false, 0);
		setStatus({});
		return;
	}
	if (itemId != item->id) {
		itemId = item->id;
		corrector.reset();
	}
	if (startRev != data.state.rev) {
		startRev = data.state.rev;
		startAttempts = 0;
	}
	const auto nextId = NextAfterEnd(data);
	if (const auto next = data.find(nextId)) {
		if (next->id != item->id) {
			[[maybe_unused]] auto &preloaded = ensureEntry(*next);
		}
	}
	auto &entry = ensureEntry(*item);
	room->reportStatus(
		Kind::Music,
		item->id,
		(entry.document != nullptr),
		entry.document ? 100 : entry.percent);
	if (entry.failed) {
		holdOurs();
		setStatus({ .state = LocalState::Failed });
		if (entry.failures < kRetryLimit) {
			// One more attempt by itself a bit later. After the last
			// one: when the connection comes back or by «Повторить
			// загрузку», nothing is asked in a loop.
			timer.callOnce(kRetryDelay + crl::time(200));
		}
		return;
	} else if (!entry.document) {
		holdOurs();
		setStatus({
			.state = LocalState::Loading,
			.percent = entry.percent,
		});
		return;
	} else if (!following) {
		setStatus({ .state = LocalState::Away });
		return;
	}
	const auto plan = PlanStart(
		data.state,
		item->duration,
		room->now(),
		corrector.lead());
	if (!plan.play) {
		holdOurs();
		setStatus({ .state = LocalState::Paused });
		return;
	}
	timer.callOnce(kTick);
	const auto player = Media::Player::instance();
	const auto current = player->current(SongType::Song);
	const auto playerState = player->getState(SongType::Song);
	const auto same = (current.audio() == entry.document);
	const auto active = same
		&& playerState.id
		&& !Media::Player::IsStopped(playerState.state);
	const auto playing = active
		&& Media::Player::ShowPauseIcon(playerState.state);
	if (plan.over || endedHere(*item)) {
		// The end of a track belongs to the server.
		setStatus({ .state = LocalState::Waiting });
		return;
	} else if (plan.wait > 0) {
		if (playing) {
			pauseLocal();
		}
		setStatus({ .state = LocalState::Starting });
		timer.callOnce(std::min(plan.wait, kTick));
		return;
	}
	if (!active) {
		if (++startAttempts > kStartLimit) {
			setStatus({ .state = LocalState::Failed });
			return;
		}
		playingItem = item->id;
		startPlay(entry.document, plan.from, true);
		setStatus({ .state = LocalState::Starting });
	} else if (!playing) {
		playingItem = item->id;
		resumeLocal(plan.from);
		setStatus({ .state = LocalState::Starting });
	}
}

void MusicEngine::Private::playerUpdated() {
	if (acting) {
		return;
	}
	const auto player = Media::Player::instance();
	const auto current = player->current(SongType::Song);
	const auto document = current.audio();
	if (!document || !documents.contains(document)) {
		return;
	}
	const auto state = player->getState(SongType::Song);
	if (!state.id || room->state().gone != Gone::No) {
		return;
	}
	const auto now = crl::now();
	const auto playing = Media::Player::ShowPauseIcon(state.state);
	const auto paused = Media::Player::IsPausedOrPausing(state.state);
	const auto &data = room->player(Kind::Music);
	const auto item = data.current();
	const auto entry = item ? findEntry(item->id) : nullptr;
	if (!item || !entry || entry->document != document) {
		// The player has another track of the room: the previous one,
		// or the next one started ahead of the server. Play pressed on
		// it while away brings back to the air.
		if (playing && !following && (now - pauseIssuedAt > kOwnActionGuard)) {
			following = true;
			corrector.reset();
			sync();
		}
		return;
	} else if (Media::Player::IsStopped(state.state)) {
		using PlayState = Media::Player::State;
		const auto kind = ClassifyStop(
			(state.state == PlayState::StoppedAtEnd),
			(state.state == PlayState::StoppedAtError
				|| state.state == PlayState::StoppedAtStart),
			(endedRev == data.state.rev) && (endedItem == item->id));
		if (kind == StopKind::Ended) {
			endedRev = data.state.rev;
			endedItem = item->id;
			if (following && data.state.playing) {
				setStatus({ .state = LocalState::Waiting });
			}
		} else if (kind == StopKind::ByUser && following) {
			// Stop of a media key or of the system controls: the user
			// does not listen any more, the same as closing the player
			// bar. Otherwise the track would start again in a second.
			following = false;
			speed = 1.;
			setStatus({ .state = LocalState::Away });
		}
		return;
	} else if (!data.state.playing) {
		// The room is on pause and it is resumed from its window. Play
		// of the app's own player only brings back to the air.
		if (playing && (now - pauseIssuedAt > kOwnActionGuard)) {
			following = true;
			pauseLocal();
			setStatus({ .state = LocalState::Paused });
		}
		return;
	} else if (paused) {
		// The user, a call or a video has paused the player: that is
		// never a pause for everybody.
		if (!pausedByUs
			&& following
			&& (now - playIssuedAt > kOwnActionGuard)) {
			following = false;
			setSpeed(1.);
			setStatus({ .state = LocalState::Away });
		}
		return;
	}
	pausedByUs = false;
	if (!following) {
		following = true;
		corrector.reset();
	}
	if (endedHere(*item)) {
		// The track was played to its end and plays again from the start
		// ahead of the server (the room repeats it): it is not sent back
		// to the end by the old state, the new one comes in a moment.
		return;
	}
	const auto serverNow = room->now();
	if (serverNow + int64(corrector.lead()) < data.state.anchor) {
		return;
	}
	const auto expected = PositionAt(data.state, item->duration, serverNow);
	if (expected >= item->duration - kEndMargin) {
		return;
	}
	const auto position = ToMs(state.position, state.frequency);
	const auto result = corrector.update(now, expected, position);
	if (result.seek && (result.target < item->duration - kEndMargin)) {
		seekLocal(result.target);
	}
	setSpeed(result.speed);
	setStatus({
		.state = (corrector.awaiting()
			? LocalState::Starting
			: (result.seek || result.speed != 1.)
			? LocalState::Catching
			: LocalState::Synced),
	});
}

void MusicEngine::Private::trackChanged() {
	if (acting) {
		return;
	}
	applyVolume();
	if (oursCurrent() || playingItem.isEmpty()) {
		return;
	}
	// Something else is in the player now: the user has left the air.
	playingItem = QString();
	speed = 1.;
	if (following) {
		following = false;
		sync();
	}
}

void MusicEngine::Private::seekFinished() {
	if (acting || !oursCurrent() || !following) {
		return;
	}
	const auto player = Media::Player::instance();
	const auto &data = room->player(Kind::Music);
	const auto item = data.current();
	const auto entry = item ? findEntry(item->id) : nullptr;
	const auto current = player->current(SongType::Song);
	if (!entry
		|| entry->document != current.audio()
		|| !room->can(Right::Control)) {
		// Without the right the correction brings the player back.
		return;
	}
	const auto state = player->getState(SongType::Song);
	corrector.reset();
	room->seek(Kind::Music, ToMs(state.position, state.frequency));
}

void MusicEngine::Private::closeRequested() {
	if (acting || !oursCurrent()) {
		return;
	}
	// The player bar was closed by the user: he does not listen.
	playingItem = QString();
	speed = 1.;
	if (following) {
		following = false;
		crl::on_main(owner, [=] {
			applyVolume();
			sync();
		});
	}
}

bool MusicEngine::Private::tryUpload(const PendingDocument &pending) {
	const auto document = pending.document;
	if (!pending.media->loaded(true)) {
		return false;
	}
	auto bytes = pending.media->bytes();
	const auto path = bytes.isEmpty() ? document->filepath(true) : QString();
	if (bytes.isEmpty() && path.isEmpty()) {
		return false;
	}
	auto media = AddMedia{
		.kind = Kind::Music,
		.path = path,
		.bytes = std::move(bytes),
		.fileName = document->filename(),
		.mime = document->mimeString(),
		.duration = int64(std::max(document->duration(), crl::time(0))),
	};
	if (const auto song = document->song()) {
		media.title = song->title.trimmed();
		media.performer = song->performer.trimmed();
	}
	if (const auto thumbnail = pending.media->thumbnail()) {
		media.cover = thumbnail->original();
	}
	room->addMedia(std::move(media));
	return true;
}

void MusicEngine::Private::checkPending(not_null<DocumentData*> document) {
	auto freed = false;
	for (auto i = begin(pending); i != end(pending);) {
		if (i->document != document) {
			++i;
		} else if (tryUpload(*i)) {
			i = pending.erase(i);
			freed = true;
		} else if (!document->loading()) {
			Toast(
				i->show,
				tr::lng_oblivion_rmusic_download_failed(tr::now));
			i = pending.erase(i);
			freed = true;
		} else {
			++i;
		}
	}
	if (freed) {
		scheduleFeed();
	}
}

// The line of «Добавить все» moves on from the event loop, never from
// inside the walk over the list above or a change of the room.
void MusicEngine::Private::scheduleFeed() {
	if (backlog.empty() || feedScheduled) {
		return;
	}
	feedScheduled = true;
	crl::on_main(owner, [=] {
		feedScheduled = false;
		feedBacklog();
	});
}

// The file of a music message of Telegram goes to the room: at once if it
// is here, after the download from Telegram otherwise.
MusicEngine::Private::Added MusicEngine::Private::start(
		not_null<DocumentData*> document,
		FullMsgId origin,
		std::shared_ptr<Ui::Show> show) {
	if (documents.contains(document)) {
		return Added::Skipped;
	}
	auto entry = PendingDocument{
		.document = document,
		.media = document->createMediaView(),
		.origin = origin,
		.show = std::move(show),
	};
	entry.media->thumbnailWanted(origin);
	if (tryUpload(entry)) {
		return Added::Sent;
	} else if (ranges::contains(
			pending,
			document,
			&PendingDocument::document)) {
		return Added::Skipped;
	} else if (int(pending.size()) >= kPendingLimit) {
		return Added::Busy;
	}
	if (!document->loading()) {
		// Into memory if the file is small enough, FileLoader asserts on
		// larger ones without a target file: those go to the temp folder
		// of the room.
		auto target = QString();
		if (document->size >= Storage::kMaxFileInMemory) {
			const auto folder = cWorkingDir()
				+ u"tdata/oblivion/rooms/"_q
				+ QString::number(room->selfId())
				+ '_'
				+ room->code()
				+ '/';
			if (!QDir().mkpath(folder)) {
				return Added::Failed;
			}
			target = folder
				+ u"tg_"_q
				+ QString::number(document->id)
				+ u".audio"_q;
		}
		document->save(origin, target);
		if (!document->loading()) {
			return tryUpload(entry) ? Added::Sent : Added::Failed;
		}
	}
	pending.push_back(std::move(entry));
	return Added::Loading;
}

// «Добавить все»: the messages that wait in line go on while there is
// room for them, so that only a few files are downloaded from Telegram
// and kept for the relay at once.
void MusicEngine::Private::feedBacklog() {
	if (feeding || backlog.empty()) {
		return;
	}
	feeding = true;
	const auto guard = gsl::finally([&] { feeding = false; });
	const auto session = room->session();
	if (!session
		|| room->state().gone != Gone::No
		|| !room->can(Right::Add)) {
		backlog.clear();
		backlogShow = nullptr;
		return;
	}
	const auto uploads = [&] {
		return int(ranges::count(room->uploads(), Kind::Music, &Upload::kind));
	};
	while (!backlog.empty()
		&& int(pending.size()) < kPendingLimit
		&& uploads() < kPendingLimit) {
		const auto id = backlog.front();
		backlog.erase(begin(backlog));
		const auto item = session->data().message(id);
		const auto document = item ? MusicDocument(item) : nullptr;
		if (!document || item->forbidsSaving()) {
			continue;
		}
		const auto result = start(document, id, backlogShow);
		if (result == Added::Failed) {
			Toast(
				backlogShow,
				tr::lng_oblivion_rmusic_download_failed(tr::now));
		}
	}
	if (backlog.empty()) {
		backlogShow = nullptr;
	}
}

MusicEngine::MusicEngine(not_null<Room*> room)
: _private(std::make_unique<Private>(this, room)) {
	Engines().push_back(this);
	crl::on_main(this, [=] {
		_private->sync();
	});
}

MusicEngine::~MusicEngine() {
	auto &list = Engines();
	list.erase(ranges::remove(list, not_null(this)), end(list));
	const auto p = _private.get();
	p->lifetime.destroy();
	p->timer.cancel();
	for (auto &[id, entry] : p->entries) {
		if (entry.transfer) {
			p->room->cancelDownload(entry.transfer);
		}
	}
	if (p->room->session()) {
		// The session is alive: the player may still have our track.
		p->stopOurs();
	}
	p->restoreVolume();
	PlayerChangesStream().fire({});
}

LocalStatus MusicEngine::status() const {
	return _private->status.current();
}

rpl::producer<LocalStatus> MusicEngine::statusValue() const {
	return _private->status.value();
}

void MusicEngine::rejoin() {
	const auto p = _private.get();
	p->following = true;
	p->retryFailed();
	p->corrector.reset();
	p->sync();
}

void MusicEngine::addDocument(
		not_null<DocumentData*> document,
		FullMsgId origin,
		std::shared_ptr<Ui::Show> show) {
	const auto p = _private.get();
	const auto room = p->room;
	if (room->state().gone != Gone::No) {
		return;
	} else if (!room->can(Right::Add)) {
		Toast(show, tr::lng_oblivion_rmusic_no_add(tr::now));
		return;
	}
	using Added = Private::Added;
	switch (p->start(document, origin, show)) {
	case Added::Sent:
		Toast(show, tr::lng_oblivion_rmusic_added_toast(tr::now));
		break;
	case Added::Loading:
		Toast(show, tr::lng_oblivion_rmusic_downloading(tr::now));
		break;
	case Added::Failed:
		Toast(show, tr::lng_oblivion_rmusic_download_failed(tr::now));
		break;
	case Added::Busy:
		Toast(show, tr::lng_oblivion_rmusic_wait(tr::now));
		break;
	case Added::Skipped:
		break;
	}
}

int MusicEngine::addMessages(
		const std::vector<FullMsgId> &ids,
		std::shared_ptr<Ui::Show> show) {
	const auto p = _private.get();
	const auto room = p->room;
	if (room->state().gone != Gone::No || !room->session()) {
		return 0;
	} else if (!room->can(Right::Add)) {
		Toast(show, tr::lng_oblivion_rmusic_no_add(tr::now));
		return 0;
	}
	auto taken = 0;
	for (const auto &id : ids) {
		if (int(p->backlog.size()) >= kBatchLimit) {
			break;
		} else if (!ranges::contains(p->backlog, id)) {
			p->backlog.push_back(id);
			++taken;
		}
	}
	if (taken) {
		p->backlogShow = std::move(show);
		p->feedBacklog();
	}
	return taken;
}

bool MusicEngine::usesFile(const QString &path) const {
	const auto p = _private.get();
	const auto playing = p->entries.find(p->playingItem);
	if (playing != end(p->entries) && playing->second.path == path) {
		return true;
	}
	const auto player = Media::Player::instance();
	const auto document = player->current(SongType::Song).audio();
	const auto i = document
		? p->documents.find(not_null(document))
		: end(p->documents);
	if (i == end(p->documents) || i->second != path) {
		return false;
	}
	// The document of that file is in the player: it holds the file
	// while it plays or is on pause.
	const auto state = player->getState(SongType::Song);
	return state.id && !Media::Player::IsStopped(state.state);
}

bool MusicEngine::drives(const AudioMsgId &current) const {
	const auto document = current.audio();
	return document && _private->documents.contains(document);
}

bool MusicEngine::canMove(int delta) const {
	const auto room = _private->room;
	return room->can(Right::Control)
		&& (room->state().gone == Gone::No)
		&& !room->player(Kind::Music).queue.empty();
}

bool MusicEngine::move(int delta, bool autonext) {
	const auto p = _private.get();
	const auto room = p->room;
	if (room->state().gone != Gone::No) {
		return false;
	} else if (!autonext) {
		if (!room->can(Right::Control)) {
			return false;
		} else if (delta > 0) {
			room->next(Kind::Music);
		} else {
			room->previous(Kind::Music);
		}
		return true;
	}
	// Our track is over. The server switches by itself; if the file of
	// the next item is here it starts at once, the exact state comes
	// with the event.
	const auto &data = room->player(Kind::Music);
	const auto current = data.current();
	p->endedRev = data.state.rev;
	p->endedItem = p->playingItem;
	if (!p->following
		|| !data.state.playing
		|| !current
		|| current->id != p->playingItem) {
		return false;
	}
	const auto next = data.find(NextAfterEnd(data));
	const auto entry = next ? p->findEntry(next->id) : nullptr;
	if (!entry || !entry->document) {
		return false;
	}
	p->playingItem = next->id;
	p->startPlay(entry->document, 0, false);
	return true;
}

crl::time MusicEngine::takeStartPosition(const AudioMsgId &audioId) {
	const auto p = _private.get();
	if (!p->pendingStart.document
		|| p->pendingStart.document != audioId.audio()) {
		return -1;
	}
	return base::take(p->pendingStart).position;
}

float64 MusicEngine::speedFor(const AudioMsgId &audioId) const {
	return drives(audioId) ? _private->speed : 0.;
}

std::unique_ptr<MusicEngine> CreateMusicEngine(not_null<Room*> room) {
	return std::make_unique<MusicEngine>(room);
}

bool DrivesPlayer(const AudioMsgId &current) {
	return EngineFor(current) != nullptr;
}

bool PlayerCanMove(int delta) {
	const auto current = Media::Player::instance()->current(SongType::Song);
	const auto engine = EngineFor(current);
	return engine && engine->canMove(delta);
}

bool PlayerMove(int delta, bool autonext) {
	const auto current = Media::Player::instance()->current(SongType::Song);
	const auto engine = EngineFor(current);
	return engine && engine->move(delta, autonext);
}

rpl::producer<> PlayerChanges() {
	return PlayerChangesStream().events();
}

crl::time TakeStartPosition(const AudioMsgId &audioId) {
	for (const auto &engine : Engines()) {
		const auto result = engine->takeStartPosition(audioId);
		if (result >= 0) {
			return result;
		}
	}
	return -1;
}

float64 PlayerSpeed(const AudioMsgId &audioId) {
	const auto engine = EngineFor(audioId);
	return engine ? engine->speedFor(audioId) : 0.;
}

void AddToRoomAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item) {
	const auto session = &controller->session();
	const auto room = Oblivion::Get().cloudRooms()
		? ActiveRoom(session)
		: nullptr;
	const auto document = MusicDocument(item);
	if (!room
		|| !document
		|| !room->music()
		|| !room->can(Right::Add)
		|| (room->state().gone != Gone::No)
		|| (&item->history()->session() != session)
		|| item->forbidsSaving()) {
		return;
	}
	const auto weak = base::make_weak(room);
	const auto id = item->fullId();
	const auto show = controller->uiShow();
	menu->addAction(tr::lng_oblivion_rmusic_add_to_room(tr::now), [=] {
		const auto strong = weak.get();
		const auto engine = strong ? strong->music() : nullptr;
		const auto alive = strong ? strong->session() : nullptr;
		const auto message = alive ? alive->data().message(id) : nullptr;
		const auto track = message ? MusicDocument(message) : nullptr;
		if (engine && track) {
			engine->addDocument(track, id, show);
		}
	}, &st::menuIconSoundOn);
}

// ---- The tab.

namespace {

[[nodiscard]] const style::font &TitleFont() {
	static const auto result = style::font(
		Scaled(17),
		st::semiboldFont->flags(),
		st::semiboldFont->family());
	return result;
}

// The title of what plays now: «Ночной эфир» and «Тишина» write it
// bigger.
[[nodiscard]] const style::font &NowTitleFont() {
	const auto big = [](int size) {
		return style::font(
			Scaled(size),
			st::semiboldFont->flags(),
			st::semiboldFont->family());
	};
	switch (Look::Current()) {
	case Look::kNightAir: {
		static const auto result = big(kAirTitle);
		return result;
	}
	case Look::kSilence: {
		static const auto result = big(kSilenceTitle);
		return result;
	}
	}
	return TitleFont();
}

// «Тишина» says «сейчас играет» in small capitals over the title, the
// line takes this much.
[[nodiscard]] int NowLabelHeight() {
	return Look::Is(Look::kSilence)
		? (st::semiboldFont->height + Scaled(2))
		: 0;
}

// The seek bar of somebody who can't control the player (and of an empty
// player) is still a whole bar: the default disabled colours leave only
// the played part of it, in the colour of the unplayed one.
[[nodiscard]] const style::MediaSlider &SeekStyle() {
	static const auto result = [] {
		auto copy = st::mediaPlayerPanelPlayback;
		copy.activeFgDisabled = st::mediaPlayerActiveFg;
		copy.inactiveFgDisabled = st::mediaPlayerInactiveFg;
		return copy;
	}();
	return result;
}

// With a look on the tab paints the seek and the volume bars by itself
// (the sliders only take the mouse then): a thicker bar with the gradient
// in «Ночной эфир», a hairline in «Тишина».
struct LookSlider {
	QColor active;
	QColor inactive;
	QColor marker;
	int width = 0; // The thickness of the bar.
	int markerSize = 0;
	bool gradient = false;
};

[[nodiscard]] LookSlider LookSliderFor(bool seek) {
	using Role = Look::Role;
	const auto look = Look::Current();
	const auto silence = (look == Look::kSilence);
	const auto dark = Look::Dark();
	// «Тишина»: the acid accent on black, the colour of the text on paper.
	const auto loud = silence
		? Look::Color(dark ? Role::Highlight : Role::Text)
		: Look::Color(Role::AccentFill);
	// The marker never leaves the rect of the slider: a slider repaints
	// only itself when its value changes.
	const auto markerMax = st::mediaPlayerPanelPlayback.seekSize.height();
	auto result = LookSlider();
	result.inactive = Look::Color(silence ? Role::Divider : Role::Pill);
	if (!seek) {
		result.active = Look::Color(Role::SubText);
		result.marker = Look::Color(Role::Text);
		result.width = Scaled(silence ? 2 : 4);
		result.markerSize = std::min(Scaled(7), markerMax);
		return result;
	}
	result.active = loud;
	result.gradient = (look == Look::kNightAir);
	result.marker = silence
		? loud
		: (look == Look::kNightAir)
		? Look::Color(Role::Inverse)
		: dark
		? QColor(255, 255, 255)
		: loud;
	result.width = std::min(
		Scaled(silence ? 2 : result.gradient ? 6 : 4),
		markerMax);
	result.markerSize = markerMax;
	return result;
}

// Where the played part of a bar ends, from its left edge: the same place
// Ui::MediaSlider puts its marker to, so the mouse and the picture agree.
// seekSize: the width that slider keeps for the marker, 0 when it shows
// none (then the bar is filled from edge to edge).
[[nodiscard]] int SliderFill(float64 value, int length, int seekSize) {
	const auto clamped = std::clamp(value, 0., 1.);
	return (seekSize > 0 && length > seekSize)
		? qRound(seekSize / 2. + clamped * (length - seekSize))
		: qRound(clamped * std::max(length, 0));
}

void PaintLookSlider(
		QPainter &p,
		QRect geometry,
		float64 value,
		bool seek,
		bool keepsMarker,
		bool showsMarker) {
	if (geometry.isEmpty()) {
		return;
	}
	const auto colors = LookSliderFor(seek);
	const auto seekSize = st::mediaPlayerPanelPlayback.seekSize.width();
	const auto fill = SliderFill(
		value,
		geometry.width(),
		keepsMarker ? seekSize : 0);
	const auto radius = colors.width / 2.;
	const auto bar = QRectF(
		geometry.x(),
		geometry.y() + (geometry.height() - colors.width) / 2.,
		geometry.width(),
		colors.width);
	p.setPen(Qt::NoPen);
	p.setBrush(colors.inactive);
	p.drawRoundedRect(bar, radius, radius);
	if (fill > 0) {
		const auto played = QRectF(bar.x(), bar.y(), fill, bar.height());
		if (colors.gradient) {
			p.setBrush(Look::AccentBrush(played));
		} else {
			p.setBrush(colors.active);
		}
		p.drawRoundedRect(played, radius, radius);
	}
	if (showsMarker) {
		const auto size = colors.markerSize;
		p.setBrush(colors.marker);
		p.drawEllipse(QRectF(
			geometry.x() + fill - size / 2.,
			geometry.y() + (geometry.height() - size) / 2.,
			size,
			size));
	}
}

// The uploads of the other players of the room are shown in their tabs.
[[nodiscard]] std::vector<Upload> MusicUploads(not_null<Room*> room) {
	auto result = std::vector<Upload>();
	for (const auto &upload : room->uploads()) {
		if (upload.kind == Kind::Music) {
			result.push_back(upload);
		}
	}
	return result;
}

void AddItemDocument(
		not_null<Room*> room,
		FullMsgId id,
		std::shared_ptr<Ui::Show> show) {
	const auto session = room->session();
	const auto engine = room->music();
	const auto item = session ? session->data().message(id) : nullptr;
	if (!engine || !item) {
		return;
	}
	const auto document = MusicDocument(item);
	if (!document) {
		return;
	} else if (item->forbidsSaving()) {
		Toast(show, tr::lng_oblivion_rmusic_protected(tr::now));
		return;
	}
	engine->addDocument(document, id, show);
}

// A row of the boxes with playlists and their tracks, made like a row
// of the queue: a cover, two lines and something short at the right.
struct PickRowArgs {
	QString seed; // The colours of the cover.
	QString title;
	QString about;
	QString aside; // At the right: the duration of a track.
	bool faded = false; // Can't be chosen now.
	bool failed = false; // «about» says what is wrong.
};

class PickRow final : public Ui::AbstractButton {
public:
	PickRow(QWidget *parent, PickRowArgs &&args);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;

private:
	const PickRowArgs _args;

};

PickRow::PickRow(QWidget *parent, PickRowArgs &&args)
: AbstractButton(parent)
, _args(std::move(args)) {
	setAccessibleName(_args.title);
	if (_args.faded) {
		setAttribute(Qt::WA_TransparentForMouseEvents);
	}
}

int PickRow::resizeGetHeight(int newWidth) {
	return Scaled(54);
}

void PickRow::onStateChanged(State was, StateChangeSource source) {
	update();
}

void PickRow::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto pad = st::boxRowPadding.left();
	if (isOver() || isDown()) {
		// The row is in a box: the colours of the window, the radius of
		// the look («Тема Oblivion»).
		const auto inset = pad - Scaled(10);
		const auto radius = Look::RowRadius(Scaled(10));
		p.setPen(Qt::NoPen);
		p.setBrush(isDown() ? st::windowBgRipple : st::windowBgOver);
		p.drawRoundedRect(
			QRectF(rect()).marginsRemoved(QMarginsF(inset, 1, inset, 1)),
			radius,
			radius);
	}
	const auto cover = Scaled(38);
	p.setOpacity(_args.faded ? 0.45 : 1.);
	PaintCover(
		p,
		QRect(pad, (height() - cover) / 2, cover, cover),
		QImage(),
		_args.seed,
		Scaled(8));
	p.setOpacity(1.);
	const auto left = pad + cover + Scaled(12);
	auto right = width() - st::boxRowPadding.right();
	if (!_args.aside.isEmpty()) {
		const auto asideWidth = st::normalFont->width(_args.aside);
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			right - asideWidth,
			(height() - st::normalFont->height) / 2 + st::normalFont->ascent,
			_args.aside);
		right -= asideWidth + Scaled(10);
	}
	const auto textWidth = std::max(right - left, Scaled(40));
	const auto single = _args.about.isEmpty();
	p.setFont(st::semiboldFont);
	p.setPen(_args.faded ? st::windowSubTextFg : st::windowFg);
	p.drawText(
		left,
		(single ? ((height() - st::semiboldFont->height) / 2) : Scaled(9))
			+ st::semiboldFont->ascent,
		st::semiboldFont->elided(_args.title, textWidth));
	if (!single) {
		p.setFont(st::normalFont);
		p.setPen(_args.failed ? st::boxTextFgError : st::windowSubTextFg);
		p.drawText(
			left,
			Scaled(29) + st::normalFont->ascent,
			st::normalFont->elided(_args.about, textWidth));
	}
}

// «У вас пока нет плейлистов», «в плейлисте нет треков».
[[nodiscard]] const style::FlatLabel &PickEmptyLabelStyle() {
	static const auto result = [] {
		auto copy = st::boxDividerLabel;
		copy.align = style::al_top;
		return copy;
	}();
	return result;
}

void AddPickEmptyLabel(
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<QString> text) {
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			std::move(text),
			PickEmptyLabelStyle()),
		st::boxRowPadding + QMargins(
			0,
			st::boxMediumSkip,
			0,
			st::boxMediumSkip),
		style::al_top);
}

// chosen may be null: the rows are only shown then.
void FillPlaylistTracks(
		not_null<Ui::VerticalLayout*> container,
		const std::vector<RoomPlaylistTrack> &tracks,
		Fn<void(FullMsgId)> chosen) {
	if (tracks.empty()) {
		AddPickEmptyLabel(container, tr::lng_oblivion_rmusic_playlist_empty());
	}
	for (const auto &track : tracks) {
		const auto title = track.title.isEmpty()
			? track.fileName
			: track.title;
		const auto state = track.ready
			? QString()
			: track.failed
			? tr::lng_oblivion_rmusic_playlist_gone(tr::now)
			: tr::lng_oblivion_rmusic_playlist_wait(tr::now);
		const auto button = container->add(object_ptr<PickRow>(
			container,
			PickRowArgs{
				.seed = track.performer + title,
				.title = title,
				.about = state.isEmpty()
					? track.performer
					: track.performer.isEmpty()
					? state
					: (track.performer + u" · "_q + state),
				.aside = (track.duration > 0)
					? FormatDuration(int64(track.duration) * 1000)
					: QString(),
				.faded = !track.ready,
				.failed = !track.ready && track.failed,
			}));
		if (track.ready && chosen) {
			const auto id = track.id;
			button->setClickedCallback([=] {
				chosen(id);
			});
		}
	}
}

void PlaylistTracksBox(
		not_null<Ui::GenericBox*> box,
		base::weak_ptr<Room> weak,
		uint64 playlistId,
		QString name,
		std::shared_ptr<Ui::Show> show) {
	box->setTitle(rpl::single(name));
	box->setWidth(st::boxWideWidth);
	const auto container = box->verticalLayout();
	const auto fill = [=] {
		container->clear();
		const auto room = weak.get();
		const auto session = room ? room->session() : nullptr;
		if (!session) {
			return;
		}
		FillPlaylistTracks(
			container,
			RoomPlaylistTracks(session, playlistId),
			[=](FullMsgId id) {
				if (const auto room = weak.get()) {
					AddItemDocument(room, id, show);
				}
			});
		container->resizeToWidth(box->width());
	};
	fill();
	if (const auto room = weak.get()) {
		if (const auto session = room->session()) {
			RoomPlaylistsChanges(
				session
			) | rpl::on_next(fill, box->lifetime());
		}
	}
	box->addButton(tr::lng_oblivion_rmusic_playlist_add_all(), [=] {
		const auto room = weak.get();
		const auto session = room ? room->session() : nullptr;
		if (!session) {
			box->closeBox();
			return;
		}
		// Only what can really go: the message is here, it has a music
		// file and its chat lets the file be saved. The tracks wait in
		// line inside the engine, none is dropped because the others are
		// still being downloaded, and the toast says how many were taken.
		const auto engine = room->music();
		auto ids = std::vector<FullMsgId>();
		for (const auto &track : RoomPlaylistTracks(session, playlistId)) {
			const auto item = track.ready
				? session->data().message(track.id)
				: nullptr;
			if (item
				&& MusicDocument(item)
				&& !item->forbidsSaving()
				&& int(ids.size()) < kBatchLimit) {
				ids.push_back(track.id);
			}
		}
		const auto added = engine ? engine->addMessages(ids, show) : 0;
		if (added > 0) {
			Toast(show, tr::lng_oblivion_rmusic_batch_toast(
				tr::now,
				lt_count,
				added));
		}
		box->closeBox();
	});
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

void PlaylistsBox(
		not_null<Ui::GenericBox*> box,
		base::weak_ptr<Room> weak,
		std::vector<RoomPlaylistBrief> list,
		std::shared_ptr<Ui::Show> show) {
	box->setTitle(tr::lng_oblivion_rmusic_playlists_title());
	box->setWidth(st::boxWideWidth);
	const auto container = box->verticalLayout();
	if (list.empty()) {
		AddPickEmptyLabel(
			container,
			tr::lng_oblivion_rmusic_playlists_empty());
	}
	for (const auto &playlist : list) {
		const auto id = playlist.id;
		const auto name = playlist.name;
		const auto button = container->add(object_ptr<PickRow>(
			container,
			PickRowArgs{
				.seed = u"playlist"_q + QString::number(id) + name,
				.title = name,
				.about = tr::lng_oblivion_playlists_tracks_count(
					tr::now,
					lt_count,
					playlist.count),
			}));
		button->setClickedCallback([=] {
			if (show && show->valid()) {
				show->showBox(Box(PlaylistTracksBox, weak, id, name, show));
			}
		});
	}
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

class QueueList final : public Ui::RpWidget {
public:
	QueueList(QWidget *parent, not_null<Room*> room);

	void refresh();
	void setPlaying(bool playing);

	[[nodiscard]] rpl::producer<QString> activations() const {
		return _activations.events();
	}
	[[nodiscard]] rpl::producer<QString> menuRequests() const {
		return _menuRequests.events();
	}

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;

private:
	struct Hit {
		int upload = -1;
		int row = -1;
		bool control = false; // The cross of an upload, the dots of a row.

		friend inline bool operator==(const Hit &, const Hit &) = default;
	};

	[[nodiscard]] int uploadHeight() const;
	[[nodiscard]] int rowHeight() const;
	[[nodiscard]] Hit hitTest(QPoint point) const;
	void setOver(Hit hit);

	const not_null<Room*> _room;
	rpl::event_stream<QString> _activations;
	rpl::event_stream<QString> _menuRequests;
	Hit _over;
	Hit _pressed;
	bool _playing = false;

};

QueueList::QueueList(QWidget *parent, not_null<Room*> room)
: RpWidget(parent)
, _room(room) {
	setMouseTracking(true);
}

int QueueList::uploadHeight() const {
	return rowHeight(); // The same rhythm as the tracks under them.
}

int QueueList::rowHeight() const {
	return Scaled(54);
}

void QueueList::refresh() {
	resizeToWidth(width());
	update();
}

void QueueList::setPlaying(bool playing) {
	if (_playing != playing) {
		_playing = playing;
		update();
	}
}

int QueueList::resizeGetHeight(int newWidth) {
	const auto uploads = int(MusicUploads(_room).size());
	const auto rows = int(_room->player(Kind::Music).queue.size());
	return uploads * uploadHeight() + rows * rowHeight() + Scaled(8);
}

QueueList::Hit QueueList::hitTest(QPoint point) const {
	auto result = Hit();
	const auto uploads = int(MusicUploads(_room).size());
	const auto rows = int(_room->player(Kind::Music).queue.size());
	const auto controlLeft = width() - Scaled(52);
	if (point.y() < 0 || point.x() < 0 || point.x() >= width()) {
		return result;
	} else if (point.y() < uploads * uploadHeight()) {
		result.upload = point.y() / uploadHeight();
		result.control = (point.x() >= controlLeft);
		return result;
	}
	const auto index = (point.y() - uploads * uploadHeight()) / rowHeight();
	if (index < rows) {
		result.row = index;
		result.control = (point.x() >= controlLeft);
	}
	return result;
}

void QueueList::setOver(Hit hit) {
	if (_over != hit) {
		_over = hit;
		setCursor((hit.row >= 0 || (hit.upload >= 0 && hit.control))
			? style::cur_pointer
			: style::cur_default);
		update();
	}
}

void QueueList::mouseMoveEvent(QMouseEvent *e) {
	setOver(hitTest(e->pos()));
}

void QueueList::leaveEventHook(QEvent *e) {
	setOver({});
}

void QueueList::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = hitTest(e->pos());
	}
}

void QueueList::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto hit = hitTest(e->pos());
	const auto pressed = base::take(_pressed);
	if (hit != pressed) {
		return;
	}
	const auto uploads = MusicUploads(_room);
	const auto &queue = _room->player(Kind::Music).queue;
	if (hit.upload >= 0 && hit.upload < int(uploads.size())) {
		if (hit.control) {
			_room->cancelUpload(uploads[hit.upload].id);
		}
	} else if (hit.row >= 0 && hit.row < int(queue.size())) {
		const auto id = queue[hit.row].id;
		if (hit.control) {
			_menuRequests.fire_copy(id);
		} else {
			_activations.fire_copy(id);
		}
	}
}

void QueueList::contextMenuEvent(QContextMenuEvent *e) {
	const auto hit = hitTest(e->pos());
	const auto &queue = _room->player(Kind::Music).queue;
	if (hit.row >= 0 && hit.row < int(queue.size())) {
		_menuRequests.fire_copy(queue[hit.row].id);
	}
}

void QueueList::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto clip = e->rect();
	const auto uploads = MusicUploads(_room);
	const auto &data = _room->player(Kind::Music);
	// The looks («Тема Oblivion»). With cards the list lies in the card of
	// the queue and is narrower than the tab by its margins; the track
	// that plays is the selected row of the look; «Тишина» rules the rows
	// with hairlines.
	const auto look = Look::Current();
	const auto left = RoomContentPadding() - RoomCardMargin();
	const auto right = left;
	const auto coverSize = Scaled(38);
	const auto coverRadius = CoverRadius(Scaled(8));
	const auto selects = (look == Look::kNative)
		|| (look == Look::kNightAir);
	const auto lined = (look == Look::kSilence);
	const auto underline = [&](const QRect &row) {
		if (lined) {
			Look::PaintDivider(
				p,
				QRectF(
					left,
					row.y() + row.height() - st::lineWidth,
					width() - left - right,
					st::lineWidth),
				st::shadowFg->c);
		}
	};
	auto top = 0;

	// A file on its way to the room is a row like a track: in the place
	// of the cover there is a ring that fills up as it is sent.
	for (auto i = 0, count = int(uploads.size()); i != count; ++i) {
		const auto &upload = uploads[i];
		const auto height = uploadHeight();
		const auto rect = QRect(0, top, width(), height);
		top += height;
		if (!rect.intersects(clip)) {
			continue;
		}
		const auto failed = upload.failed && !upload.error.isEmpty();
		const auto percent = (upload.total > 0)
			? int(std::clamp(
				upload.ready * 100 / upload.total,
				int64(0),
				int64(100)))
			: -1;
		const auto cover = QRect(
			left,
			rect.y() + (height - coverSize) / 2,
			coverSize,
			coverSize);
		underline(rect);
		p.setPen(Qt::NoPen);
		p.setBrush(RoomHoverColor());
		p.drawRoundedRect(cover, coverRadius, coverRadius);
		const auto ring = QRectF(cover).marginsRemoved(QMarginsF(
			Scaled(10),
			Scaled(10),
			Scaled(10),
			Scaled(10)));
		if (failed) {
			PaintGlyph(p, Glyph::Cross, ring, st::boxTextFgError->c);
		} else {
			auto track = st::windowSubTextFg->c;
			track.setAlphaF(0.3);
			auto pen = QPen(track, Scaled(2));
			pen.setCapStyle(Qt::RoundCap);
			p.setBrush(Qt::NoBrush);
			p.setPen(pen);
			p.drawEllipse(ring);
			pen.setColor(st::windowBgActive->c);
			p.setPen(pen);
			// From the top, clockwise; a quarter while nothing is known.
			p.drawArc(
				ring,
				90 * 16,
				-((percent < 0) ? 90 : (percent * 360 / 100)) * 16);
		}
		const auto textLeft = cover.x() + coverSize + Scaled(12);
		const auto textWidth = std::max(
			width() - right - Scaled(30) - textLeft,
			Scaled(40));
		p.setFont(st::semiboldFont);
		p.setPen(st::windowFg);
		p.drawText(
			textLeft,
			rect.y() + Scaled(9) + st::semiboldFont->ascent,
			st::semiboldFont->elided(upload.title, textWidth));
		p.setFont(st::normalFont);
		p.setPen(failed ? st::boxTextFgError : st::windowSubTextFg);
		p.drawText(
			textLeft,
			rect.y() + Scaled(29) + st::normalFont->ascent,
			st::normalFont->elided(
				failed
					? upload.error
					: (percent < 0)
					? tr::lng_oblivion_rmusic_preparing(tr::now)
					: tr::lng_oblivion_rmusic_uploading(
						tr::now,
						lt_percent,
						QString::number(percent)),
				textWidth));
		const auto over = (_over.upload == i) && _over.control;
		PaintGlyph(
			p,
			Glyph::Cross,
			QRectF(
				width() - right - Scaled(16),
				rect.y() + (height - Scaled(20)) / 2.,
				Scaled(20),
				Scaled(20)),
			over ? st::windowFg->c : st::windowSubTextFg->c);
	}

	const auto currentId = data.state.itemId;
	for (auto i = 0, count = int(data.queue.size()); i != count; ++i) {
		const auto &item = data.queue[i];
		const auto height = rowHeight();
		const auto rect = QRect(0, top, width(), height);
		top += height;
		if (!rect.intersects(clip)) {
			continue;
		}
		const auto current = (item.id == currentId);
		const auto over = (_over.row == i);
		const auto back = QRectF(rect).marginsRemoved(
			QMarginsF(Scaled(8), 1, Scaled(8), 1));
		if (current && selects) {
			Look::PaintSelected(p, back, st::windowBgOver->c, Scaled(10));
		} else if (over || (current && !lined)) {
			PaintRoomHover(p, back);
		}
		underline(rect);
		const auto cover = QRect(
			left,
			rect.y() + (height - coverSize) / 2,
			coverSize,
			coverSize);
		PaintCover(
			p,
			cover,
			item.cover.isEmpty() ? QImage() : _room->cover(item.cover),
			item.media,
			coverRadius);
		if (current) {
			// Over the picture of the cover, whatever the theme is.
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0, 0, 0, 110));
			p.drawRoundedRect(cover, coverRadius, coverRadius);
			if (_playing) {
				// The bars of an equaliser: «this one is on air».
				const auto bar = double(Scaled(3));
				const auto skip = double(Scaled(2));
				const auto full = double(Scaled(14));
				const auto barsLeft = cover.x()
					+ (coverSize - 3 * bar - 2 * skip) / 2.;
				const auto barsBottom = cover.y() + (coverSize + full) / 2.;
				auto index = 0;
				p.setBrush(Qt::white);
				for (const auto part : { 0.55, 1., 0.75 }) {
					p.drawRoundedRect(
						QRectF(
							barsLeft + index * (bar + skip),
							barsBottom - full * part,
							bar,
							full * part),
						bar / 2.,
						bar / 2.);
					++index;
				}
			} else {
				PaintGlyph(
					p,
					Glyph::Pause,
					QRectF(cover).marginsRemoved(QMarginsF(
						Scaled(10),
						Scaled(10),
						Scaled(10),
						Scaled(10))),
					Qt::white);
			}
		}
		const auto duration = FormatDuration(item.duration);
		const auto durationWidth = st::normalFont->width(duration);
		const auto dots = over;
		const auto adder = _room->state().member(item.addedBy);
		// Who has put the track on: a small userpic in a column of its
		// own before the duration (while that member is in the room).
		const auto who = Scaled(20);
		const auto durationRight = width() - right - (dots ? Scaled(26) : 0);
		const auto whoLeft = durationRight
			- std::max(durationWidth, st::normalFont->width(u"00:00"_q))
			- Scaled(8)
			- who;
		const auto textLeft = cover.x() + coverSize + Scaled(12);
		const auto textRight = (adder
			? whoLeft
			: (durationRight - durationWidth)) - Scaled(8);
		const auto textWidth = std::max(textRight - textLeft, Scaled(40));
		if (adder) {
			PaintUserpic(
				p,
				QRect(whoLeft, rect.y() + (height - who) / 2, who, who),
				adder->id,
				adder->name);
		}
		// «Ночной эфир» and «Тишина» mark the track that plays by its row
		// and its cover, the title keeps the colour of the text.
		const auto accented = current && !Look::CapsLabels();
		p.setFont(st::semiboldFont);
		p.setPen(accented ? st::windowActiveTextFg : st::windowFg);
		p.drawText(
			textLeft,
			rect.y() + Scaled(9) + st::semiboldFont->ascent,
			st::semiboldFont->elided(item.title, textWidth));
		const auto sub = !item.performer.isEmpty()
			? item.performer
			: (adder && !adder->name.isEmpty())
			? tr::lng_oblivion_rmusic_added_by(
				tr::now,
				lt_name,
				adder->name)
			: item.fileName;
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			textLeft,
			rect.y() + Scaled(29) + st::normalFont->ascent,
			st::normalFont->elided(sub, textWidth));
		p.drawText(
			durationRight - durationWidth,
			rect.y() + (height - st::normalFont->height) / 2
				+ st::normalFont->ascent,
			duration);
		if (dots) {
			PaintGlyph(
				p,
				Glyph::More,
				QRectF(
					width() - right - Scaled(18),
					rect.y() + (height - Scaled(22)) / 2.,
					Scaled(22),
					Scaled(22)),
				_over.control ? st::windowFg->c : st::windowSubTextFg->c);
		}
	}
}

class MusicTab final : public Ui::RpWidget {
public:
	MusicTab(
		QWidget *parent,
		not_null<Room*> room,
		std::shared_ptr<Ui::Show> show);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

private:
	void refreshAll();
	void refreshControls();
	void refreshPosition();
	void updateLayout();
	void applyLook();
	[[nodiscard]] LocalStatus localStatus() const;
	[[nodiscard]] QString statusText(const LocalStatus &status) const;
	[[nodiscard]] int textsTop() const;
	[[nodiscard]] int titleTop() const;
	[[nodiscard]] bool checkControl();
	void showAddMenu();
	void showRowMenu(const QString &itemId);
	void addFromPlayer();
	void addFromPlaylists();
	void addFiles();
	void togglePlay();
	void cycleRepeat();
	void saveVolume(float64 value);

	const not_null<Room*> _room;
	const std::shared_ptr<Ui::Show> _show;
	const not_null<Ui::MediaSlider*> _seek;
	const not_null<Ui::MediaSlider*> _volume;
	const not_null<GlyphButton*> _repeat;
	const not_null<GlyphButton*> _previous;
	const not_null<GlyphButton*> _play;
	const not_null<GlyphButton*> _next;
	const not_null<Ui::RoundButton*> _add;
	const not_null<Ui::RoundButton*> _rejoin;
	const not_null<Ui::ScrollArea*> _scroll;
	QPointer<QueueList> _list;
	base::unique_qptr<Ui::PopupMenu> _menu;
	base::Timer _timer;
	base::Timer _volumeTimer;
	QRect _cover;
	QRect _texts;
	QRect _times;
	QRect _queueHeader;
	QRect _volumeIcon;
	QRect _playerCard; // The cards of a look, see RoomHasCards().
	QRect _queueCard;
	float64 _seeking = -1.;
	float64 _pendingSeek = -1.;
	crl::time _pendingSeekTill = 0;
	float64 _volumeWanted = -1.;
	bool _rejoinRetries = false; // «Повторить загрузку», not «В эфир».

};

MusicTab::MusicTab(
	QWidget *parent,
	not_null<Room*> room,
	std::shared_ptr<Ui::Show> show)
: RpWidget(parent)
, _room(room)
, _show(std::move(show))
, _seek(Ui::CreateChild<Ui::MediaSlider>(this, SeekStyle()))
, _volume(Ui::CreateChild<Ui::MediaSlider>(
	this,
	st::mediaPlayerPanelPlayback))
, _repeat(Ui::CreateChild<GlyphButton>(this, Glyph::Repeat, Scaled(36)))
, _previous(Ui::CreateChild<GlyphButton>(this, Glyph::Previous, Scaled(40)))
, _play(Ui::CreateChild<GlyphButton>(this, Glyph::Play, Scaled(52), true))
, _next(Ui::CreateChild<GlyphButton>(this, Glyph::Next, Scaled(40)))
, _add(Ui::CreateChild<Ui::RoundButton>(
	this,
	tr::lng_oblivion_rmusic_add(),
	st::defaultActiveButton))
, _rejoin(Ui::CreateChild<Ui::RoundButton>(
	this,
	tr::lng_oblivion_rmusic_rejoin(),
	st::defaultLightButton))
, _scroll(Ui::CreateChild<Ui::ScrollArea>(this, st::boxScroll))
, _timer([=] { refreshPosition(); })
, _volumeTimer([=] {
	if (_volumeWanted >= 0.) {
		saveVolume(base::take(_volumeWanted));
	}
}) {
	_list = _scroll->setOwnedWidget(object_ptr<QueueList>(this, room));
	_add->setFullRadius(true);
	_rejoin->hide();
	_play->setPrimary(true);

	_seek->setAlwaysDisplayMarker(true);
	_seek->setChangeProgressCallback([=](float64 value) {
		_seeking = value;
		update(_times);
	});
	_seek->setChangeFinishedCallback([=](float64 value) {
		_seeking = -1.;
		const auto current = _room->player(Kind::Music).current();
		if (current && checkControl()) {
			// The bar stays where it was dropped till the room answers.
			_pendingSeek = value;
			_pendingSeekTill = crl::now() + kSeekShown;
			_room->seek(Kind::Music, int64(value * current->duration));
			base::call_delayed(kSeekShown + 50, this, [=] {
				refreshPosition();
			});
		} else {
			refreshPosition();
		}
	});

	_volume->setAlwaysDisplayMarker(true);
	_volume->setMoveByWheel(true);
	_volume->setValue(Oblivion::Get().roomMusicVolume() / 100.);
	// Every change of the setting writes the settings file: while the
	// slider is dragged that is done a few times a second, not on every
	// step, and once more when it is dropped.
	_volume->setChangeProgressCallback([=](float64 value) {
		_volumeWanted = value;
		if (!_volumeTimer.isActive()) {
			_volumeTimer.callOnce(kVolumeSaveDelay);
		}
	});
	_volume->setChangeFinishedCallback([=](float64 value) {
		_volumeTimer.cancel();
		_volumeWanted = -1.;
		saveVolume(value);
	});

	_play->setClickedCallback([=] { togglePlay(); });
	_previous->setClickedCallback([=] {
		if (checkControl()) {
			_room->previous(Kind::Music);
		}
	});
	_next->setClickedCallback([=] {
		if (checkControl()) {
			_room->next(Kind::Music);
		}
	});
	_repeat->setClickedCallback([=] { cycleRepeat(); });
	_add->setClickedCallback([=] { showAddMenu(); });
	_rejoin->setClickedCallback([=] {
		if (const auto engine = _room->music()) {
			engine->rejoin();
		}
	});

	_list->activations(
	) | rpl::on_next([=](const QString &itemId) {
		if (_room->can(Right::Control)) {
			_room->select(Kind::Music, itemId);
		} else {
			showRowMenu(itemId);
		}
	}, lifetime());
	_list->menuRequests(
	) | rpl::on_next([=](const QString &itemId) {
		showRowMenu(itemId);
	}, lifetime());

	_room->changes(
	) | rpl::on_next([=](Changes changes) {
		const auto mine = Changes(Change::MusicQueue)
			| Change::MusicPlayer
			| Change::Rights
			| Change::Uploads
			| Change::Covers
			| Change::Members
			| Change::Gone
			| Change::Reloaded;
		if (changes & Change::MusicPlayer) {
			_pendingSeek = -1.;
		}
		if (changes & mine) {
			refreshAll();
		}
	}, lifetime());

	if (const auto engine = _room->music()) {
		engine->statusValue(
		) | rpl::on_next([=](const LocalStatus &) {
			refreshControls();
			update();
		}, lifetime());
	}

	rpl::merge(
		_add->widthValue() | rpl::to_empty,
		_rejoin->widthValue() | rpl::to_empty
	) | rpl::on_next([=] {
		updateLayout();
	}, lifetime());

	// A look («Тема Oblivion») has its own cover, cards and bars.
	Look::Updates(
	) | rpl::on_next([=] {
		applyLook();
		refreshAll();
	}, lifetime());
	applyLook();

	refreshAll();
}

void MusicTab::applyLook() {
	// With a look on the tab paints both bars, see PaintLookSlider().
	const auto own = !Look::Is(Look::kPlain);
	_seek->disablePaint(own);
	_volume->disablePaint(own);
}

LocalStatus MusicTab::localStatus() const {
	if (const auto engine = _room->music()) {
		return engine->status();
	}
	// A sample room: what the room itself says.
	const auto &data = _room->player(Kind::Music);
	return {
		.state = !data.current()
			? LocalState::Idle
			: data.state.playing
			? LocalState::Synced
			: LocalState::Paused,
	};
}

QString MusicTab::statusText(const LocalStatus &status) const {
	switch (status.state) {
	case LocalState::Idle: return QString();
	case LocalState::Loading:
		return tr::lng_oblivion_rmusic_state_loading(
			tr::now,
			lt_percent,
			QString::number(status.percent));
	case LocalState::Failed:
		return tr::lng_oblivion_rmusic_state_failed(tr::now);
	case LocalState::Paused: {
		const auto &state = _room->player(Kind::Music).state;
		const auto by = _room->state().member(state.updatedBy);
		return (by && !by->name.isEmpty())
			? tr::lng_oblivion_rmusic_state_paused_by(
				tr::now,
				lt_name,
				by->name)
			: tr::lng_oblivion_rmusic_state_paused(tr::now);
	}
	case LocalState::Starting:
		return tr::lng_oblivion_rmusic_state_starting(tr::now);
	case LocalState::Synced:
		return tr::lng_oblivion_rmusic_state_synced(tr::now);
	case LocalState::Catching:
		return tr::lng_oblivion_rmusic_state_catching(tr::now);
	case LocalState::Away:
		return tr::lng_oblivion_rmusic_state_away(tr::now);
	case LocalState::Waiting:
		return tr::lng_oblivion_rmusic_state_waiting(tr::now);
	}
	return QString();
}

// The lines next to the cover (the title, who plays it, the state or the
// button that stands in its place) are centred against the cover: one
// line of an empty player does not hang at its top.
int MusicTab::textsTop() const {
	const auto status = localStatus();
	const auto button = (status.state == LocalState::Away)
		|| (status.state == LocalState::Failed);
	const auto line = !button && !statusText(status).isEmpty();
	const auto block = NowLabelHeight()
		+ NowTitleFont()->height
		+ (_room->player(Kind::Music).current()
			? (Scaled(4) + st::normalFont->height)
			: 0)
		+ (button
			? (Scaled(2) + _rejoin->height())
			: line
			? (Scaled(6) + st::normalFont->height)
			: 0);
	return _cover.y() + std::max((_cover.height() - block) / 2, Scaled(2));
}

// Under the «сейчас играет» of «Тишина», when it is there.
int MusicTab::titleTop() const {
	return textsTop() + NowLabelHeight();
}

bool MusicTab::checkControl() {
	if (_room->can(Right::Control)) {
		return true;
	}
	Toast(_show, tr::lng_oblivion_rmusic_no_control(tr::now));
	return false;
}

void MusicTab::togglePlay() {
	const auto &data = _room->player(Kind::Music);
	if (data.queue.empty()) {
		showAddMenu();
	} else if (!checkControl()) {
		return;
	} else if (data.state.playing) {
		_room->pause(Kind::Music);
	} else {
		_room->play(Kind::Music);
	}
}

void MusicTab::saveVolume(float64 value) {
	Oblivion::Get().setRoomMusicVolume(
		int(base::SafeRound(std::clamp(value, 0., 1.) * 100.)));
	update(_volumeIcon);
}

void MusicTab::cycleRepeat() {
	if (!checkControl()) {
		return;
	}
	const auto now = _room->player(Kind::Music).state.repeat;
	_room->setRepeat(Kind::Music, (now == Repeat::Off)
		? Repeat::All
		: (now == Repeat::All)
		? Repeat::One
		: Repeat::Off);
}

void MusicTab::refreshAll() {
	if (_list) {
		_list->refresh();
	}
	refreshControls();
	refreshPosition();
	updateLayout();
	update();
}

void MusicTab::refreshControls() {
	const auto &data = _room->player(Kind::Music);
	const auto control = _room->can(Right::Control);
	const auto status = localStatus();
	_play->setGlyph(data.state.playing ? Glyph::Pause : Glyph::Play);
	_play->setDimmed(!control && !data.queue.empty());
	_previous->setDimmed(!control || data.queue.empty());
	_next->setDimmed(!control || data.queue.empty());
	_repeat->setDimmed(!control);
	_repeat->setGlyph((data.state.repeat == Repeat::One)
		? Glyph::RepeatOne
		: Glyph::Repeat);
	_repeat->setHighlighted(data.state.repeat != Repeat::Off);
	_seek->setDisabled(!control || !data.current());
	// No track: a clean empty bar, without the stub that the place of
	// the marker leaves at its start.
	_seek->setAlwaysDisplayMarker(data.current() != nullptr);
	_add->setVisible(_room->can(Right::Add));
	// The same button brings back to the air and, when the file of the
	// track could not be brought, asks for it once more.
	const auto away = (status.state == LocalState::Away);
	const auto failed = (status.state == LocalState::Failed);
	if (failed != _rejoinRetries && (failed || away)) {
		_rejoinRetries = failed;
		_rejoin->setText(failed
			? tr::lng_oblivion_rmusic_retry()
			: tr::lng_oblivion_rmusic_rejoin());
	}
	const auto offered = away || failed;
	if (_rejoin->isHidden() == offered) {
		_rejoin->setVisible(offered);
		updateLayout();
	}
	if (_list) {
		_list->setPlaying(data.state.playing
			&& (status.state != LocalState::Away));
	}
	if (data.state.playing && data.current()) {
		if (!_timer.isActive()) {
			_timer.callEach(kPositionTick);
		}
	} else {
		_timer.cancel();
	}
}

void MusicTab::refreshPosition() {
	const auto current = _room->player(Kind::Music).current();
	if (_pendingSeek >= 0. && crl::now() >= _pendingSeekTill) {
		_pendingSeek = -1.;
	}
	if (!_seek->isChanging()) {
		_seek->setValue((_pendingSeek >= 0.)
			? _pendingSeek
			: (current && current->duration > 0)
			? std::clamp(
				_room->position(Kind::Music) / float64(current->duration),
				0.,
				1.)
			: 0.);
	}
	update(_times);
}

void MusicTab::updateLayout() {
	// The looks («Тема Oblivion»): with cards the player and the queue are
	// two cards on the page, the content keeps its place inside them.
	const auto look = Look::Current();
	const auto cards = RoomHasCards();
	const auto margin = RoomCardMargin();
	const auto pad = RoomContentPadding();
	const auto w = width();
	if (w <= 0) {
		return;
	}
	const auto narrow = (w < Scaled(480));
	// «Ночной эфир» has a round cover in a ring, the ring stands around
	// the picture; «Тишина» has a bigger square one.
	const auto ring = (look == Look::kNightAir) ? Scaled(kCoverRing) : 0;
	const auto coverSize = (look == Look::kSilence)
		? Scaled(narrow ? kSilenceCover : kSilenceCoverWide)
		: (Scaled(narrow ? 76 : 104) + 2 * ring);
	const auto top = cards ? (margin + Scaled(16)) : pad;
	_cover = QRect(pad, top, coverSize, coverSize);
	const auto textLeft = _cover.x() + coverSize + Scaled(16);
	_texts = QRect(textLeft, top, w - textLeft - pad, coverSize);
	// «Вернуться в эфир» takes the place of the state line.
	_rejoin->moveToLeft(
		textLeft - Scaled(2),
		titleTop()
			+ NowTitleFont()->height
			+ Scaled(4)
			+ st::normalFont->height
			+ Scaled(2),
		w);

	const auto seekHeight = st::mediaPlayerPanelPlayback.seekSize.height();
	const auto seekTop = _cover.y() + coverSize + Scaled(14);
	_seek->setGeometry(pad, seekTop, w - 2 * pad, seekHeight);
	_times = QRect(
		pad,
		seekTop + seekHeight + Scaled(2),
		w - 2 * pad,
		st::normalFont->height);

	// «Play» with its two neighbours stands in the middle of the window,
	// the repeat mode and the volume take the edges. In a narrow window
	// the middle moves a little to the left, away from the volume.
	const auto controlsTop = _times.y() + _times.height() + Scaled(6);
	const auto gap = Scaled(narrow ? 6 : 12);
	const auto volumeWidth = Scaled(narrow ? 70 : 96);
	const auto volumeIcon = Scaled(24);
	const auto volumeLeft = w - pad - volumeWidth;
	const auto half = _play->width() / 2;
	const auto repeatLeft = pad - Scaled(7); // Its glyph starts at pad.
	const auto center = std::max(
		std::min(
			w / 2,
			volumeLeft
				- volumeIcon
				- Scaled(6)
				- Scaled(8)
				- _next->width()
				- gap
				- half),
		repeatLeft + _repeat->width() + _previous->width() + gap + half);
	const auto middle = controlsTop + _play->height() / 2;
	_repeat->move(repeatLeft, middle - _repeat->height() / 2);
	_previous->move(
		center - half - gap - _previous->width(),
		middle - _previous->height() / 2);
	_play->move(center - half, middle - _play->height() / 2);
	_next->move(center + half + gap, middle - _next->height() / 2);
	_volume->setGeometry(
		volumeLeft,
		middle - seekHeight / 2,
		volumeWidth,
		seekHeight);
	_volumeIcon = QRect(
		volumeLeft - volumeIcon - Scaled(6),
		middle - volumeIcon / 2,
		volumeIcon,
		volumeIcon);

	const auto playerBottom = controlsTop + _play->height() + Scaled(14);
	auto headerTop = playerBottom;
	_playerCard = _queueCard = QRect();
	if (cards) {
		const auto queueTop = playerBottom + margin;
		_playerCard = QRect(
			margin,
			margin,
			w - 2 * margin,
			playerBottom - margin);
		_queueCard = QRect(
			margin,
			queueTop,
			w - 2 * margin,
			std::max(height() - margin - queueTop, 0));
		headerTop = queueTop + Scaled(8);
	}
	const auto headerHeight = std::max(_add->height(), Scaled(34));
	_queueHeader = QRect(pad, headerTop, w - 2 * pad, headerHeight);
	_add->moveToRight(
		pad,
		headerTop + (headerHeight - _add->height()) / 2,
		w);
	const auto scrollTop = headerTop + headerHeight + Scaled(6);
	if (cards) {
		// The list ends before the round corners of its card.
		_scroll->setGeometry(
			margin,
			scrollTop,
			w - 2 * margin,
			std::max(height() - margin - Scaled(8) - scrollTop, 0));
	} else {
		_scroll->setGeometry(
			0,
			scrollTop,
			w,
			std::max(height() - scrollTop, 0));
	}
	if (_list) {
		_list->resizeToWidth(_scroll->width());
	}
}

void MusicTab::resizeEvent(QResizeEvent *e) {
	updateLayout();
}

void MusicTab::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	PaintRoomGround(p, this, e->rect());

	// The looks («Тема Oblivion»): the cards under the player and the
	// queue, or the hairline of «Тишина» between them.
	const auto look = Look::Current();
	PaintRoomCard(p, _playerCard);
	PaintRoomCard(p, _queueCard);
	if (look == Look::kSilence) {
		Look::PaintDivider(
			p,
			QRectF(
				_queueHeader.x(),
				_queueHeader.y() - Scaled(7),
				_queueHeader.width(),
				st::lineWidth),
			st::shadowFg->c);
	}

	const auto &data = _room->player(Kind::Music);
	const auto current = data.current();
	// «Ночной эфир»: a round picture in the gradient ring.
	const auto ring = (look == Look::kNightAir) ? Scaled(kCoverRing) : 0;
	const auto picture = _cover.marginsRemoved({ ring, ring, ring, ring });
	if (ring > 0) {
		Look::PaintCoverRing(p, QRectF(picture));
	}
	PaintCover(
		p,
		picture,
		(current && !current->cover.isEmpty())
			? _room->cover(current->cover)
			: QImage(),
		current ? current->media : QString(),
		(ring > 0) ? (picture.width() / 2) : Scaled(14));

	const auto status = localStatus();
	const auto &titleFont = NowTitleFont();
	auto top = textsTop();
	if (const auto label = NowLabelHeight()) {
		PaintRoomLabel(
			p,
			_texts.x(),
			top,
			tr::lng_oblivion_look_room_now_playing(tr::now),
			_texts.width(),
			st::windowSubTextFg->c);
		top += label;
	}
	p.setFont(titleFont);
	p.setPen(st::windowFg);
	p.drawText(
		_texts.x(),
		top + titleFont->ascent,
		titleFont->elided(
			current
				? current->title
				: tr::lng_oblivion_rmusic_nothing(tr::now),
			_texts.width()));
	top += titleFont->height + Scaled(4);
	p.setFont(st::normalFont);
	p.setPen(st::windowSubTextFg);
	if (current) {
		const auto adder = _room->state().member(current->addedBy);
		const auto sub = !current->performer.isEmpty()
			? current->performer
			: (adder && !adder->name.isEmpty())
			? tr::lng_oblivion_rmusic_added_by(
				tr::now,
				lt_name,
				adder->name)
			: current->fileName;
		p.drawText(
			_texts.x(),
			top + st::normalFont->ascent,
			st::normalFont->elided(sub, _texts.width()));
		top += st::normalFont->height;
	}
	top += Scaled(6);
	const auto text = statusText(status);
	// Away and Failed: the button stands in the place of this line.
	if (!text.isEmpty()
		&& status.state != LocalState::Away
		&& status.state != LocalState::Failed) {
		const auto good = (status.state == LocalState::Synced)
			|| (status.state == LocalState::Catching);
		const auto bad = (status.state == LocalState::Failed);
		const auto color = good
			? st::boxTextFgGood->c
			: bad
			? st::boxTextFgError->c
			: st::windowSubTextFg->c;
		const auto dot = Scaled(7);
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawEllipse(QRectF(
			_texts.x(),
			top + (st::normalFont->height - dot) / 2.,
			dot,
			dot));
		// «Тишина» keeps the colour for the dot, the line is quiet.
		p.setPen((good && look == Look::kSilence)
			? st::windowSubTextFg->c
			: color);
		p.drawText(
			_texts.x() + dot + Scaled(6),
			top + st::normalFont->ascent,
			st::normalFont->elided(
				text,
				_texts.width() - dot - Scaled(6)));
	}

	// The times under the seek bar.
	const auto duration = current ? current->duration : int64(0);
	const auto position = (_seeking >= 0.)
		? int64(_seeking * duration)
		: (_pendingSeek >= 0.)
		? int64(_pendingSeek * duration)
		: _room->position(Kind::Music);
	p.setFont(st::normalFont);
	p.setPen(st::windowSubTextFg);
	p.drawText(
		_times.x(),
		_times.y() + st::normalFont->ascent,
		FormatDuration(position));
	const auto total = FormatDuration(duration);
	p.drawText(
		_times.x() + _times.width() - st::normalFont->width(total),
		_times.y() + st::normalFont->ascent,
		total);

	PaintGlyph(
		p,
		Oblivion::Get().roomMusicVolume() ? Glyph::Volume : Glyph::Mute,
		_volumeIcon,
		st::windowSubTextFg->c);

	if (look != Look::kPlain) {
		// The sliders are silent then, see applyLook(). The marker of the
		// seek bar is shown when Ui::MediaSlider would show it: to
		// somebody who can move it.
		const auto movable = current && _room->can(Right::Control);
		PaintLookSlider(
			p,
			_seek->geometry(),
			_seek->value(),
			true,
			(current != nullptr),
			movable);
		PaintLookSlider(
			p,
			_volume->geometry(),
			_volume->value(),
			false,
			true,
			true);
	}

	// The header of the queue.
	const auto count = int(data.queue.size());
	const auto headerTop = _queueHeader.y()
		+ (_queueHeader.height() - st::semiboldFont->height) / 2;
	const auto headerWidth = PaintRoomLabel(
		p,
		_queueHeader.x(),
		headerTop,
		tr::lng_oblivion_rmusic_queue(tr::now),
		_queueHeader.width(),
		st::windowFg->c);
	if (count > 0) {
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			_queueHeader.x() + headerWidth + Scaled(8),
			headerTop + st::semiboldFont->ascent,
			QString::number(count));
	}

	if (!count && MusicUploads(_room).empty()) {
		const auto area = QRect(
			Scaled(32),
			_scroll->y() + Scaled(18),
			width() - Scaled(64),
			height() - _scroll->y() - Scaled(18));
		p.setFont(st::semiboldFont);
		p.setPen(st::windowFg);
		p.drawText(
			QRect(area.x(), area.y(), area.width(), st::semiboldFont->height),
			Qt::AlignHCenter | Qt::AlignTop,
			tr::lng_oblivion_rmusic_empty(tr::now));
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(
				area.x(),
				area.y() + st::semiboldFont->height + Scaled(6),
				area.width(),
				std::max(area.height() - st::semiboldFont->height, 0)),
			Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
			_room->can(Right::Add)
				? tr::lng_oblivion_rmusic_empty_about(tr::now)
				: tr::lng_oblivion_rmusic_empty_no_right(tr::now));
	}
}

void MusicTab::showAddMenu() {
	if (!_room->can(Right::Add)) {
		Toast(_show, tr::lng_oblivion_rmusic_no_add(tr::now));
		return;
	}
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::popupMenuWithIcons);
	_menu->addAction(
		tr::lng_oblivion_rmusic_add_player(tr::now),
		[=] { addFromPlayer(); },
		&st::menuIconSoundOn);
	_menu->addAction(
		tr::lng_oblivion_rmusic_add_playlist(tr::now),
		[=] { addFromPlaylists(); },
		&st::menuIconSavedMessages);
	_menu->addAction(
		tr::lng_oblivion_rmusic_add_files(tr::now),
		[=] { addFiles(); },
		&st::menuIconFile);
	if (_room->can(Right::Queue)
		&& !_room->player(Kind::Music).queue.empty()) {
		_menu->addSeparator();
		_menu->addAction(tr::lng_oblivion_rmusic_clear(tr::now), [=] {
			if (!_show || !_show->valid()) {
				return;
			}
			const auto weak = base::make_weak(_room.get());
			_show->showBox(Ui::MakeConfirmBox({
				.text = tr::lng_oblivion_rmusic_clear_sure(tr::now),
				.confirmed = [=](Fn<void()> close) {
					if (const auto strong = weak.get()) {
						strong->clearQueue(Kind::Music);
					}
					close();
				},
				.confirmText = tr::lng_oblivion_rmusic_clear(tr::now),
				.confirmStyle = &st::attentionBoxButton,
			}));
		}, &st::menuIconClear);
	}
	_menu->popup(QCursor::pos());
}

void MusicTab::showRowMenu(const QString &itemId) {
	const auto &data = _room->player(Kind::Music);
	const auto index = data.indexOf(itemId);
	if (index < 0) {
		return;
	}
	const auto &item = data.queue[index];
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::popupMenuWithIcons);
	const auto room = _room;
	// Which actions are offered is decided by the queue as it is now,
	// where the track goes is counted when the action is clicked: the
	// queue may change while the menu is open.
	const auto can = [&](QueueMove how) {
		return MoveIndexes(data, itemId, how).first >= 0;
	};
	const auto move = [=](QueueMove how) {
		return [=] {
			room->moveItem(Kind::Music, itemId, how);
		};
	};
	if (_room->can(Right::Control)) {
		_menu->addAction(
			tr::lng_oblivion_rmusic_row_play(tr::now),
			[=] { room->select(Kind::Music, itemId); },
			&st::menuIconSoundOn);
	}
	if (_room->can(Right::Queue)) {
		if (can(QueueMove::Next)) {
			_menu->addAction(
				tr::lng_oblivion_rmusic_row_next(tr::now),
				move(QueueMove::Next),
				&st::menuIconRestore);
		}
		if (can(QueueMove::Up)) {
			_menu->addAction(
				tr::lng_oblivion_rmusic_row_up(tr::now),
				move(QueueMove::Up),
				&st::menuIconAbove);
		}
		if (can(QueueMove::Down)) {
			_menu->addAction(
				tr::lng_oblivion_rmusic_row_down(tr::now),
				move(QueueMove::Down),
				&st::menuIconBelow);
		}
	}
	if (CanRemoveItem(_room->state(), item, _room->selfId())) {
		_menu->addAction(
			tr::lng_oblivion_rmusic_row_remove(tr::now),
			[=] { room->removeItem(Kind::Music, itemId); },
			&st::menuIconDelete);
	}
	if (_menu->empty()) {
		_menu = nullptr;
		return;
	}
	_menu->popup(QCursor::pos());
}

void MusicTab::addFromPlayer() {
	const auto session = _room->session();
	const auto engine = _room->music();
	if (!session || !engine) {
		return;
	}
	const auto current = Media::Player::instance()->current(SongType::Song);
	const auto document = current.audio();
	if (!document
		|| (&document->session() != session)
		|| engine->drives(current)
		|| !document->isAudioFile()) {
		Toast(_show, tr::lng_oblivion_rmusic_player_empty(tr::now));
		return;
	}
	const auto id = current.contextId();
	if (const auto item = session->data().message(id)) {
		if (item->forbidsSaving()) {
			Toast(_show, tr::lng_oblivion_rmusic_protected(tr::now));
			return;
		}
	}
	engine->addDocument(document, id, _show);
}

void MusicTab::addFromPlaylists() {
	const auto session = _room->session();
	if (!session || !_show || !_show->valid()) {
		return;
	}
	_show->showBox(Box(
		PlaylistsBox,
		base::make_weak(_room.get()),
		RoomPlaylists(session),
		_show));
}

void MusicTab::addFiles() {
	if (_room->sample()) {
		return;
	}
	const auto weak = base::make_weak(_room.get());
	const auto show = _show;
	FileDialog::GetOpenPaths(
		this,
		tr::lng_oblivion_rmusic_add_files_title(tr::now),
		FileDialog::AudioFilesFilter(),
		crl::guard(this, [=](FileDialog::OpenResult &&result) {
			const auto room = weak.get();
			if (!room) {
				return;
			}
			auto added = 0;
			for (const auto &path : result.paths) {
				if (added >= kBatchLimit) {
					Toast(show, tr::lng_oblivion_rmusic_too_many(tr::now));
					break;
				}
				if (room->addMedia({ .kind = Kind::Music, .path = path })) {
					++added;
				}
			}
		}));
}

// ---- Snapshot scenes (OBLIVION_SELFTEST=ui): the boxes of «Добавить» →
// «Из плейлиста Oblivion…». The tab itself is in the scenes of the window.

const auto MusicSnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	const auto text = [](const char *ru, const char *en) {
		return QString::fromUtf8(CurrentLanguageIsRussian() ? ru : en);
	};
	const auto size = QSize(st::boxWideWidth * 2, 0);
	RegisterBoxScene(u"room_music_playlists_box"_q, size, [=](
			std::shared_ptr<Ui::Show> show) {
		return Box(
			PlaylistsBox,
			base::weak_ptr<Room>(),
			std::vector<RoomPlaylistBrief>{
				{
					.id = 1,
					.name = text("Ночные поездки", "Night rides"),
					.count = 24,
				},
				{
					.id = 2,
					.name = text("Русский рок", "Russian rock"),
					.count = 112,
				},
				{
					.id = 3,
					.name = text("Для работы", "For work"),
					.count = 5,
				},
			},
			show);
	});
	RegisterBoxScene(u"room_music_playlists_box_empty"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(
			PlaylistsBox,
			base::weak_ptr<Room>(),
			std::vector<RoomPlaylistBrief>(),
			show);
	});
	// The real box takes the tracks from the session, this one shows the
	// same rows with every state of a track.
	RegisterBoxScene(u"room_music_tracks_box"_q, size, [=](
			std::shared_ptr<Ui::Show> show) {
		return Box([=](not_null<Ui::GenericBox*> box) {
			box->setTitle(rpl::single(text("Ночные поездки", "Night rides")));
			box->setWidth(st::boxWideWidth);
			FillPlaylistTracks(box->verticalLayout(), {
				{
					.title = u"Midnight City"_q,
					.performer = u"M83"_q,
					.duration = 243,
					.ready = true,
				},
				{
					.title = text(
						"Звезда по имени Солнце",
						"A Star Called Sun"),
					.performer = text("Кино", "Kino"),
					.duration = 225,
					.ready = true,
				},
				{
					.title = text("Запись с репетиции", "Rehearsal take"),
					.duration = 95,
					.ready = true,
				},
				{
					.title = u"Instant Crush"_q,
					.performer = u"Daft Punk"_q,
					.duration = 337,
				},
				{
					.title = u"Let It Happen"_q,
					.performer = u"Tame Impala"_q,
					.duration = 467,
					.failed = true,
				},
			}, nullptr);
			box->addButton(tr::lng_oblivion_rmusic_playlist_add_all(), [=] {
				box->closeBox();
			});
			box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		});
	});
});

// ---- OBLIVION_SELFTEST=room_sync.

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

constexpr auto kBase = int64(1759800000000);

[[nodiscard]] PlayerState TestState(
		bool playing,
		int64 position,
		int64 anchor) {
	return {
		.itemId = u"a1"_q,
		.playing = playing,
		.position = position,
		.anchor = anchor,
		.rev = 1,
	};
}

void TestPosition(Checker &check) {
	const auto duration = int64(200'000);
	auto state = TestState(true, 41'250, kBase);
	check(PositionAt(state, duration, kBase + 1000) == 42'250,
		"playing: position grows with the time");
	check(PositionAt(state, duration, kBase - 500) == 41'250,
		"before the anchor: the scheduled position");
	check(PositionAt(state, duration, kBase) == 41'250,
		"at the anchor: the scheduled position");
	check(PositionAt(state, duration, kBase + 500'000) == duration,
		"never beyond the duration");
	state.playing = false;
	check(PositionAt(state, duration, kBase + 9000) == 41'250,
		"paused: stays");
	state.itemId = QString();
	check(PositionAt(state, duration, kBase + 9000) == 0,
		"nothing current: zero");
	state = TestState(true, 300'000, kBase);
	check(PositionAt(state, duration, kBase) == duration,
		"a position beyond the duration is clamped");
	state = TestState(true, 1000, kBase);
	check(PositionAt(state, 0, kBase + 5000) == 0,
		"no duration: zero");
}

void TestPlan(Checker &check) {
	const auto duration = int64(200'000);
	const auto lead = crl::time(120);
	auto plan = PlanStart(TestState(false, 5000, kBase), duration, kBase, lead);
	check(!plan.play, "a paused room starts nothing");
	auto none = TestState(true, 0, kBase);
	none.itemId = QString();
	check(!PlanStart(none, duration, kBase, lead).play,
		"no item starts nothing");

	// The start is scheduled 300 ms ahead.
	plan = PlanStart(TestState(true, 5000, kBase + 300), duration, kBase, lead);
	check(plan.play && plan.wait == 180 && plan.from == 5000 && !plan.over,
		"scheduled: wait till anchor minus lead");
	plan = PlanStart(
		TestState(true, 5000, kBase + 300),
		duration,
		kBase + 180,
		lead);
	check(plan.play && plan.wait == 0 && plan.from == 5000,
		"scheduled: start exactly lead before the anchor");
	plan = PlanStart(
		TestState(true, 5000, kBase + 300),
		duration,
		kBase + 250,
		lead);
	check(plan.wait == 0 && plan.from == 5070,
		"late for the anchor: start ahead by the lead");

	// Joined in the middle of a track.
	plan = PlanStart(
		TestState(true, 5000, kBase),
		duration,
		kBase + 60'000,
		lead);
	check(plan.wait == 0 && plan.from == 65'120 && !plan.over,
		"in the middle: the position plus the lead");

	// The end.
	plan = PlanStart(
		TestState(true, 0, kBase),
		duration,
		kBase + duration - 100,
		lead);
	check(plan.play && plan.over, "the last moments belong to the server");
	plan = PlanStart(
		TestState(true, 0, kBase),
		duration,
		kBase + duration + 5000,
		lead);
	check(plan.over && plan.from == duration, "after the end");
	plan = PlanStart(TestState(true, 0, kBase), duration, kBase, 0);
	check(plan.wait == 0 && plan.from == 0, "no lead, at the anchor");
}

void TestCorrector(Checker &check) {
	// In place: nothing is done.
	{
		auto corrector = SyncCorrector();
		auto now = crl::time(1000);
		auto fine = true;
		for (auto i = 0; i != 20; ++i, now += 100) {
			const auto expected = int64(10'000 + i * 100);
			const auto jitter = int64((i % 3) * 20 - 30);
			const auto result = corrector.update(
				now,
				expected,
				expected + jitter);
			fine = fine && !result.seek && (result.speed == 1.);
		}
		check(fine, "small jitter changes nothing");
		check(std::abs(corrector.drift()) <= 30, "the drift is the median");
	}

	// Behind by 300 ms: the tempo goes up, then back to normal.
	{
		auto corrector = SyncCorrector();
		auto now = crl::time(1000);
		auto expected = int64(10'000);
		auto actual = expected - 300;
		auto sped = false;
		auto seeked = false;
		auto steps = 0;
		for (; steps != 100; ++steps) {
			const auto result = corrector.update(now, expected, actual);
			seeked = seeked || result.seek;
			if (result.speed > 1.) {
				sped = true;
			} else if (sped) {
				break;
			}
			now += 100;
			expected += 100;
			actual += int64(100 * result.speed + 0.5);
		}
		check(sped && !seeked, "behind: the tempo is raised, no seek");
		check(steps < 60 && std::abs(actual - expected) <= 60,
			"behind: caught up without a jump");
		check(corrector.speed() == 1., "behind: back to the normal tempo");
	}

	// Ahead by 200 ms: the tempo goes down.
	{
		auto corrector = SyncCorrector();
		auto now = crl::time(1000);
		auto expected = int64(10'000);
		auto actual = expected + 200;
		auto slowed = false;
		auto seeked = false;
		for (auto i = 0; i != 100; ++i) {
			const auto result = corrector.update(now, expected, actual);
			seeked = seeked || result.seek;
			if (result.speed < 1.) {
				slowed = true;
			} else if (slowed) {
				break;
			}
			now += 100;
			expected += 100;
			actual += int64(100 * result.speed + 0.5);
		}
		check(slowed && !seeked && std::abs(actual - expected) <= 60,
			"ahead: slowed down into place");
	}

	// Far away: one seek that aims ahead by the lead, then a cooldown.
	{
		auto corrector = SyncCorrector();
		const auto lead = corrector.lead();
		auto result = corrector.update(1000, 60'000, 10'000);
		check(result.seek && result.target == 60'000 + int64(lead)
			&& result.speed == 1., "far: a seek ahead by the lead");
		check(corrector.awaiting(), "far: waits for the sound");
		// The player still reports the target: nothing new.
		result = corrector.update(1100, 60'100, 60'000 + int64(lead));
		check(!result.seek && corrector.awaiting(),
			"the reported target is not a position");
		// The sound has started 200 ms after the seek, but the room is
		// far away again: the cooldown holds the next seek.
		result = corrector.update(
			1300,
			80'000,
			60'000 + int64(lead) + 100);
		check(!result.seek && !corrector.awaiting(),
			"the sound goes: measured, no second seek at once");
		check(corrector.lead() == (lead + 200) / 2,
			"the latency of the seek goes into the lead");
		result = corrector.update(1000 + 3100, 82'800, 63'000);
		check(result.seek, "a seek after the cooldown");
		result = corrector.update(1000 + 3200, 82'900, 63'100);
		check(!result.seek, "awaiting after the second seek");
	}

	// A cold start is not measured.
	{
		auto corrector = SyncCorrector();
		const auto lead = corrector.lead();
		corrector.started(1000, 5000, true);
		auto result = corrector.update(1900, 5900, 5100);
		check(!corrector.awaiting() && corrector.lead() == lead,
			"a cold start does not change the lead");
		check(!result.seek, "a cold start within the limit: no seek");
		corrector.reset();
		check(corrector.speed() == 1. && corrector.drift() == 0,
			"reset");
	}

	// The sound never comes: the wait is over after the timeout.
	{
		auto corrector = SyncCorrector();
		corrector.started(1000, 5000, false);
		auto result = corrector.update(3000, 7000, 5000);
		check(corrector.awaiting() && !result.seek, "still waiting");
		result = corrector.update(1000 + 5100, 10'100, 5000);
		check(!corrector.awaiting(), "the wait times out");
		result = corrector.update(1000 + 5200, 10'200, 5000);
		check(result.seek, "and the player is sent to the place");
	}

	// The tempo is dropped when the sign of the drift flips.
	{
		auto corrector = SyncCorrector();
		auto result = corrector.update(1000, 10'000, 9700);
		check(result.speed > 1., "behind: faster");
		result = corrector.update(1100, 10'100, 10'150);
		result = corrector.update(1200, 10'200, 10'250);
		check(result.speed == 1., "ahead now: the nudge is over");
	}
}

void TestStops(Checker &check) {
	// The end of a track: first "stopped at the end", then the player is
	// cleared and says plainly "stopped" about the same track.
	check(ClassifyStop(true, false, false) == StopKind::Ended,
		"the end of a track is the end");
	check(ClassifyStop(true, false, true) == StopKind::Ended,
		"the end seen twice is still the end");
	check(ClassifyStop(false, false, true) == StopKind::AfterEnd,
		"a plain stop after the end is not the user");
	// Stop of a media key: a plain stop with no end before it. That one
	// leaves the air, the engine does not start the track again.
	check(ClassifyStop(false, false, false) == StopKind::ByUser,
		"a plain stop out of nowhere is the user's stop");
	check(ClassifyStop(false, true, false) == StopKind::Failed
		&& ClassifyStop(false, true, true) == StopKind::Failed,
		"a player error is never the user's stop");

	// A file that ends a bit before its time by the room: the last
	// moments belong to the server, nothing is started again.
	const auto duration = int64(200'000);
	const auto state = TestState(true, 0, kBase);
	const auto early = kBase + duration - 900;
	check(PositionAt(state, duration, early) >= duration - kEndSlack,
		"a second before the end is within the slack");
	check(!PlanStart(state, duration, early, 120).over,
		"and it is not the end by the plan yet");
	check(PositionAt(state, duration, kBase + 60'000) < duration - kEndSlack,
		"the middle of a track is not");
}

// The bars a look paints by itself («Тема Oblivion»): the played part
// ends where the slider that takes the mouse keeps its marker.
void TestSlider(Checker &check) {
	check(SliderFill(0., 300, 9) == 5 && SliderFill(1., 300, 9) == 296,
		"the marker stays inside the bar at both ends");
	check(SliderFill(0.5, 300, 9) == 150,
		"the middle of the bar is the middle");
	check(SliderFill(0., 300, 0) == 0 && SliderFill(1., 300, 0) == 300,
		"a bar without a marker is filled from edge to edge");
	check(SliderFill(-1., 300, 9) == SliderFill(0., 300, 9)
		&& SliderFill(7., 300, 9) == SliderFill(1., 300, 9),
		"a value out of range is clamped");
	auto last = -1;
	auto monotone = true;
	for (auto i = 0; i <= 100; ++i) {
		const auto now = SliderFill(i / 100., 300, 9);
		monotone = monotone && (now >= last);
		last = now;
	}
	check(monotone, "the played part never goes back as the value grows");
	check(SliderFill(0.5, 0, 9) == 0 && SliderFill(0.5, 5, 9) == 3,
		"a bar shorter than the marker does not break");
}

} // namespace

object_ptr<Ui::RpWidget> CreateMusicTab(
		QWidget *parent,
		not_null<Room*> room,
		std::shared_ptr<Ui::Show> show) {
	return object_ptr<MusicTab>(parent, room, std::move(show));
}

bool RunSyncSelfTest(QStringList &log) {
	auto check = Checker(log);
	TestPosition(check);
	check.section("position");
	TestPlan(check);
	check.section("start plan");
	TestCorrector(check);
	check.section("corrector");
	TestStops(check);
	check.section("stops and ends");
	TestSlider(check);
	check.section("look slider");
	log.push_back(u"room_sync: %1 checks, %2 failed"_q.arg(
		QString::number(check.passed() + check.failed()),
		QString::number(check.failed())));
	return !check.failed();
}

} // namespace Oblivion::Rooms
