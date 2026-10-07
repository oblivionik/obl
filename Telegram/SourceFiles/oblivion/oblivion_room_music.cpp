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
#include "oblivion/oblivion_playlists.h"
#include "oblivion/oblivion_room_window.h"
#include "oblivion/oblivion_settings.h"
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
constexpr auto kTick = crl::time(1000);
constexpr auto kOwnActionGuard = crl::time(800);
constexpr auto kRetryDelay = crl::time(15000);
constexpr auto kRetryLimit = 3;
constexpr auto kStartLimit = 3;
constexpr auto kPendingLimit = 5;
constexpr auto kBatchLimit = 20;
constexpr auto kPositionTick = crl::time(250);
constexpr auto kSeekShown = crl::time(1500);

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

	[[nodiscard]] Entry &ensureEntry(const QueueItem &item);
	[[nodiscard]] Entry *findEntry(const QString &itemId);
	void cleanupEntries();
	void fileReady(const QString &itemId, const QString &path);
	void fileFailed(const QString &itemId);
	void fileProgress(const QString &itemId, int64 ready, int64 total);
	[[nodiscard]] DocumentData *makeDocument(
		const QueueItem &item,
		const QString &path);

	[[nodiscard]] bool oursCurrent() const;
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

	const not_null<MusicEngine*> owner;
	const not_null<Room*> room;
	std::map<QString, Entry> entries;
	base::flat_set<not_null<DocumentData*>> documents;
	std::vector<PendingDocument> pending;
	PendingStart pendingStart;
	SyncCorrector corrector;
	base::Timer timer;
	rpl::variable<LocalStatus> status;
	QString itemId; // The current item of the room the engine has seen.
	QString playingItem; // The item whose document was given to the player.
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
		if (changes & (Changes(Change::MusicQueue)
			| Change::MusicPlayer
			| Change::Gone
			| Change::Reloaded)) {
			if (changes & Change::MusicPlayer) {
				corrector.reset();
			}
			sync();
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
	using Flag = MTPDdocumentAttributeAudio::Flag;
	document->setattributes({
		MTP_documentAttributeAudio(
			MTP_flags(Flag::f_title | Flag::f_performer),
			MTP_int(seconds),
			MTP_string(item.title),
			MTP_string(item.performer),
			MTPstring()),
		MTP_documentAttributeFilename(MTP_string(item.fileName.isEmpty()
			? u"track"_q
			: item.fileName)),
	});
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
		documents.emplace(entry->document);
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
		acting = true;
		Media::Player::instance()->stopAndClose();
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
		timer.callOnce(kRetryDelay);
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
	const auto ended = same
		&& (playerState.state == Media::Player::State::StoppedAtEnd)
		&& (endedRev == data.state.rev);
	if (plan.over || ended) {
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
		if (state.state == Media::Player::State::StoppedAtEnd) {
			endedRev = data.state.rev;
			if (following && data.state.playing) {
				setStatus({ .state = LocalState::Waiting });
			}
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
	for (auto i = begin(pending); i != end(pending);) {
		if (i->document != document) {
			++i;
		} else if (tryUpload(*i)) {
			i = pending.erase(i);
		} else if (!document->loading()) {
			Toast(
				i->show,
				tr::lng_oblivion_rmusic_download_failed(tr::now));
			i = pending.erase(i);
		} else {
			++i;
		}
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
	p->startAttempts = 0;
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
	} else if (p->documents.contains(document)) {
		return;
	}
	auto pending = Private::PendingDocument{
		.document = document,
		.media = document->createMediaView(),
		.origin = origin,
		.show = show,
	};
	pending.media->thumbnailWanted(origin);
	if (p->tryUpload(pending)) {
		Toast(show, tr::lng_oblivion_rmusic_added_toast(tr::now));
		return;
	} else if (ranges::contains(
			p->pending,
			document,
			&Private::PendingDocument::document)) {
		return;
	} else if (int(p->pending.size()) >= kPendingLimit) {
		Toast(show, tr::lng_oblivion_rmusic_wait(tr::now));
		return;
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
				Toast(
					show,
					tr::lng_oblivion_rmusic_download_failed(tr::now));
				return;
			}
			target = folder
				+ u"tg_"_q
				+ QString::number(document->id)
				+ u".audio"_q;
		}
		document->save(origin, target);
		if (!document->loading() && !p->tryUpload(pending)) {
			Toast(show, tr::lng_oblivion_rmusic_download_failed(tr::now));
			return;
		} else if (!document->loading()) {
			Toast(show, tr::lng_oblivion_rmusic_added_toast(tr::now));
			return;
		}
	}
	Toast(show, tr::lng_oblivion_rmusic_downloading(tr::now));
	p->pending.push_back(std::move(pending));
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
		const auto tracks = RoomPlaylistTracks(session, playlistId);
		if (tracks.empty()) {
			container->add(
				object_ptr<Ui::FlatLabel>(
					container,
					tr::lng_oblivion_rmusic_playlist_empty(),
					st::boxDividerLabel),
				st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
		}
		for (const auto &track : tracks) {
			const auto line = track.performer.isEmpty()
				? (track.title.isEmpty() ? track.fileName : track.title)
				: (track.performer + u" — "_q + track.title);
			const auto text = track.ready
				? line
				: track.failed
				? tr::lng_oblivion_rmusic_playlist_unavailable(
					tr::now,
					lt_track,
					line)
				: tr::lng_oblivion_rmusic_playlist_loading(
					tr::now,
					lt_track,
					line);
			const auto button = container->add(
				object_ptr<Ui::SettingsButton>(
					container,
					rpl::single(text),
					st::settingsButtonNoIcon));
			if (!track.ready) {
				button->setAttribute(Qt::WA_TransparentForMouseEvents);
				continue;
			}
			const auto id = track.id;
			button->setClickedCallback([=] {
				if (const auto room = weak.get()) {
					AddItemDocument(room, id, show);
				}
			});
		}
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
		auto added = 0;
		for (const auto &track : RoomPlaylistTracks(session, playlistId)) {
			if (track.ready && added < kBatchLimit) {
				AddItemDocument(room, track.id, nullptr);
				++added;
			}
		}
		if (added) {
			Toast(show, tr::lng_oblivion_rmusic_added_toast(tr::now));
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
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				tr::lng_oblivion_rmusic_playlists_empty(),
				st::boxDividerLabel),
			st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
	}
	for (const auto &playlist : list) {
		const auto id = playlist.id;
		const auto name = playlist.name;
		const auto text = name
			+ u" · "_q
			+ tr::lng_oblivion_playlists_tracks_count(
				tr::now,
				lt_count,
				playlist.count);
		const auto button = container->add(
			object_ptr<Ui::SettingsButton>(
				container,
				rpl::single(text),
				st::settingsButtonNoIcon));
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
	return Scaled(46);
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
	const auto left = Scaled(20);
	const auto right = Scaled(16);
	auto top = 0;

	for (auto i = 0, count = int(uploads.size()); i != count; ++i) {
		const auto &upload = uploads[i];
		const auto height = uploadHeight();
		const auto rect = QRect(0, top, width(), height);
		top += height;
		if (!rect.intersects(clip)) {
			continue;
		}
		const auto textWidth = width() - left - right - Scaled(40);
		p.setFont(st::semiboldFont);
		p.setPen(st::windowFg);
		p.drawText(
			left,
			rect.y() + Scaled(8) + st::semiboldFont->ascent,
			st::semiboldFont->elided(upload.title, textWidth));
		const auto percent = (upload.total > 0)
			? int(std::clamp(
				upload.ready * 100 / upload.total,
				int64(0),
				int64(100)))
			: -1;
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			left,
			rect.y() + Scaled(26) + st::normalFont->ascent,
			(percent < 0)
				? tr::lng_oblivion_rmusic_preparing(tr::now)
				: tr::lng_oblivion_rmusic_uploading(
					tr::now,
					lt_percent,
					QString::number(percent)));
		const auto line = QRectF(
			left,
			rect.y() + height - Scaled(3),
			textWidth,
			Scaled(2));
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgRipple);
		p.drawRoundedRect(line, line.height() / 2., line.height() / 2.);
		if (percent > 0) {
			p.setBrush(st::windowBgActive);
			p.drawRoundedRect(
				QRectF(
					line.x(),
					line.y(),
					line.width() * percent / 100.,
					line.height()),
				line.height() / 2.,
				line.height() / 2.);
		}
		const auto over = (_over.upload == i) && _over.control;
		PaintGlyph(
			p,
			Glyph::Cross,
			QRectF(
				width() - right - Scaled(26),
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
		if (over || current) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgOver);
			p.drawRoundedRect(
				QRectF(rect).marginsRemoved(
					QMarginsF(Scaled(8), 1, Scaled(8), 1)),
				Scaled(10),
				Scaled(10));
		}
		const auto coverSize = Scaled(38);
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
			Scaled(8));
		if (current) {
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0, 0, 0, 110));
			p.drawRoundedRect(cover, Scaled(8), Scaled(8));
			PaintGlyph(
				p,
				_playing ? Glyph::Volume : Glyph::Pause,
				QRectF(cover).marginsRemoved(QMarginsF(
					Scaled(9),
					Scaled(9),
					Scaled(9),
					Scaled(9))),
				Qt::white);
		}
		const auto duration = FormatDuration(item.duration);
		const auto durationWidth = st::normalFont->width(duration);
		const auto dots = over;
		const auto textLeft = cover.x() + coverSize + Scaled(12);
		const auto textRight = width()
			- right
			- durationWidth
			- Scaled(dots ? 40 : 8);
		const auto textWidth = std::max(textRight - textLeft, Scaled(40));
		p.setFont(st::semiboldFont);
		p.setPen(current ? st::windowActiveTextFg : st::windowFg);
		p.drawText(
			textLeft,
			rect.y() + Scaled(9) + st::semiboldFont->ascent,
			st::semiboldFont->elided(item.title, textWidth));
		const auto adder = _room->state().member(item.addedBy);
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
			width() - right - durationWidth - (dots ? Scaled(32) : 0),
			rect.y() + (height - st::normalFont->height) / 2
				+ st::normalFont->ascent,
			duration);
		if (dots) {
			PaintGlyph(
				p,
				Glyph::More,
				QRectF(
					width() - right - Scaled(24),
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
	[[nodiscard]] LocalStatus localStatus() const;
	[[nodiscard]] QString statusText(const LocalStatus &status) const;
	[[nodiscard]] bool checkControl();
	void showAddMenu();
	void showRowMenu(const QString &itemId);
	void addFromPlayer();
	void addFromPlaylists();
	void addFiles();
	void togglePlay();
	void cycleRepeat();

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
	QRect _cover;
	QRect _texts;
	QRect _times;
	QRect _queueHeader;
	QRect _volumeIcon;
	float64 _seeking = -1.;
	float64 _pendingSeek = -1.;
	crl::time _pendingSeekTill = 0;

};

MusicTab::MusicTab(
	QWidget *parent,
	not_null<Room*> room,
	std::shared_ptr<Ui::Show> show)
: RpWidget(parent)
, _room(room)
, _show(std::move(show))
, _seek(Ui::CreateChild<Ui::MediaSlider>(this, st::mediaPlayerPanelPlayback))
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
, _timer([=] { refreshPosition(); }) {
	_list = _scroll->setOwnedWidget(object_ptr<QueueList>(this, room));
	_add->setFullRadius(true);
	_rejoin->hide();

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
	const auto applyVolume = [=](float64 value) {
		Oblivion::Get().setRoomMusicVolume(
			int(base::SafeRound(std::clamp(value, 0., 1.) * 100.)));
		update(_volumeIcon);
	};
	_volume->setChangeProgressCallback(applyVolume);
	_volume->setChangeFinishedCallback(applyVolume);

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

	refreshAll();
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
	_add->setVisible(_room->can(Right::Add));
	const auto away = (status.state == LocalState::Away);
	if (_rejoin->isHidden() == away) {
		_rejoin->setVisible(away);
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
	const auto pad = Scaled(20);
	const auto w = width();
	if (w <= 0) {
		return;
	}
	const auto narrow = (w < Scaled(480));
	const auto coverSize = Scaled(narrow ? 76 : 104);
	_cover = QRect(pad, pad, coverSize, coverSize);
	const auto textLeft = _cover.x() + coverSize + Scaled(16);
	_texts = QRect(textLeft, pad, w - textLeft - pad, coverSize);
	// «Вернуться в эфир» takes the place of the state line.
	_rejoin->moveToLeft(
		textLeft - Scaled(2),
		pad
			+ Scaled(4)
			+ TitleFont()->height
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

	const auto controlsTop = _times.y() + _times.height() + Scaled(6);
	const auto gap = Scaled(narrow ? 6 : 12);
	const auto controlsWidth = _repeat->width()
		+ _previous->width()
		+ _play->width()
		+ _next->width()
		+ 3 * gap;
	const auto volumeWidth = Scaled(narrow ? 70 : 96);
	const auto volumeIcon = Scaled(24);
	const auto centered = (w - controlsWidth) / 2;
	const auto volumeLeft = w - pad - volumeWidth;
	auto left = (centered + controlsWidth + Scaled(12)
		> volumeLeft - volumeIcon - Scaled(6))
		? pad
		: centered;
	const auto middle = controlsTop + _play->height() / 2;
	const auto place = [&](not_null<GlyphButton*> button) {
		button->move(left, middle - button->height() / 2);
		left += button->width() + gap;
	};
	place(_repeat);
	place(_previous);
	place(_play);
	place(_next);
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

	const auto headerTop = controlsTop + _play->height() + Scaled(14);
	const auto headerHeight = std::max(_add->height(), Scaled(34));
	_queueHeader = QRect(pad, headerTop, w - 2 * pad, headerHeight);
	_add->moveToRight(
		pad,
		headerTop + (headerHeight - _add->height()) / 2,
		w);
	const auto scrollTop = headerTop + headerHeight + Scaled(6);
	_scroll->setGeometry(0, scrollTop, w, std::max(height() - scrollTop, 0));
	if (_list) {
		_list->resizeToWidth(w);
	}
}

void MusicTab::resizeEvent(QResizeEvent *e) {
	updateLayout();
}

void MusicTab::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	p.fillRect(e->rect(), st::windowBg);

	const auto &data = _room->player(Kind::Music);
	const auto current = data.current();
	PaintCover(
		p,
		_cover,
		(current && !current->cover.isEmpty())
			? _room->cover(current->cover)
			: QImage(),
		current ? current->media : QString(),
		Scaled(14));

	const auto status = localStatus();
	const auto &titleFont = TitleFont();
	auto top = _texts.y() + Scaled(4);
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
	}
	top += st::normalFont->height + Scaled(6);
	const auto text = statusText(status);
	if (!text.isEmpty() && status.state != LocalState::Away) {
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
		p.setPen(color);
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

	// The header of the queue.
	const auto count = int(data.queue.size());
	p.setFont(st::semiboldFont);
	p.setPen(st::windowFg);
	const auto header = tr::lng_oblivion_rmusic_queue(tr::now);
	const auto headerTop = _queueHeader.y()
		+ (_queueHeader.height() - st::semiboldFont->height) / 2
		+ st::semiboldFont->ascent;
	p.drawText(_queueHeader.x(), headerTop, header);
	if (count > 0) {
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			_queueHeader.x()
				+ st::semiboldFont->width(header)
				+ Scaled(8),
			headerTop,
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
	const auto count = int(data.queue.size());
	const auto currentIndex = data.indexOf(data.state.itemId);
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::popupMenuWithIcons);
	const auto room = _room;
	const auto move = [=](int to) {
		const auto from = room->player(Kind::Music).indexOf(itemId);
		if (from >= 0) {
			room->moveItem(Kind::Music, from, to);
		}
	};
	if (_room->can(Right::Control)) {
		_menu->addAction(
			tr::lng_oblivion_rmusic_row_play(tr::now),
			[=] { room->select(Kind::Music, itemId); },
			&st::menuIconSoundOn);
	}
	if (_room->can(Right::Queue)) {
		if (currentIndex >= 0
			&& index != currentIndex
			&& index != currentIndex + 1) {
			const auto to = (index < currentIndex)
				? currentIndex
				: (currentIndex + 1);
			_menu->addAction(
				tr::lng_oblivion_rmusic_row_next(tr::now),
				[=] { move(to); },
				&st::menuIconRestore);
		}
		if (index > 0) {
			_menu->addAction(
				tr::lng_oblivion_rmusic_row_up(tr::now),
				[=] { move(index - 1); },
				&st::menuIconAbove);
		}
		if (index + 1 < count) {
			_menu->addAction(
				tr::lng_oblivion_rmusic_row_down(tr::now),
				[=] { move(index + 1); },
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
	log.push_back(u"room_sync: %1 checks, %2 failed"_q.arg(
		QString::number(check.passed() + check.failed()),
		QString::number(check.failed())));
	return !check.failed();
}

} // namespace Oblivion::Rooms
