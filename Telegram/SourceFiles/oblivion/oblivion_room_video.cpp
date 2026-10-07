/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_room_video.h"

#include "base/call_delayed.h"
#include "base/power_save_blocker.h"
#include "base/timer.h"
#include "base/unique_qptr.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/core_settings.h"
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
#include "media/clip/media_clip_reader.h"
#include "media/player/media_player_instance.h"
#include "media/streaming/media_streaming_document.h"
#include "media/streaming/media_streaming_instance.h"
#include "media/streaming/media_streaming_loader.h"
#include "media/streaming/media_streaming_player.h"
#include "oblivion/oblivion_lang.h"
#include "oblivion/oblivion_room_music.h"
#include "oblivion/oblivion_room_window.h"
#include "oblivion/oblivion_settings.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "platform/platform_file_utilities.h"
#include "storage/cache/storage_cache_types.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/effects/animation_value.h"
#include "ui/effects/animations.h"
#include "ui/image/image.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "window/window_session_controller.h"
#include "settings.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_media_player.h"
#include "styles/style_media_view.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QLocale>
#include <QtCore/QMimeData>
#include <QtCore/QPointer>
#include <QtCore/QStorageInfo>
#include <QtCore/QUrl>
#include <QtGui/QCursor>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>
#include <QtGui/QScreen>
#include <QtGui/QWindow>

#include <map>

namespace Oblivion::Rooms {
namespace {

using Media::Streaming::LoadedPart;

constexpr auto kPartSize = Media::Streaming::Loader::kPartSize;
constexpr auto kPollInterval = crl::time(250);
constexpr auto kTick = crl::time(250);
constexpr auto kResumeLead = crl::time(40);
constexpr auto kPrepareLeadMin = crl::time(250);
constexpr auto kPrepareLeadDefault = crl::time(450);
constexpr auto kPrepareLeadMax = crl::time(2500);
constexpr auto kPrepareMeasureMax = crl::time(3000);
constexpr auto kPausedTolerance = int64(350);
constexpr auto kFrameTolerance = int64(40);
constexpr auto kHoldMax = int64(5000);
constexpr auto kSeekThreshold = int64(1200);
constexpr auto kEndMargin = int64(400);
constexpr auto kExtrapolateMax = crl::time(300);
constexpr auto kRetryDelay = crl::time(15000);
constexpr auto kRetryLimit = 3;
constexpr auto kRestartLimit = 3;
constexpr auto kPlayerFailuresLimit = 2;
constexpr auto kMutedTicks = 2;
constexpr auto kPendingLimit = 5;
constexpr auto kBatchLimit = 5;
constexpr auto kVideoBytesDefault = int64(700) * 1024 * 1024;
constexpr auto kFrameLimitWidth = 2560;
constexpr auto kFrameLimitHeight = 1440;
constexpr auto kSmoothLimit = 2'600'000; // Device pixels of the picture.
constexpr auto kPositionTick = crl::time(250);
constexpr auto kSeekShown = crl::time(1500);
constexpr auto kControlsHide = crl::time(2600);
constexpr auto kControlsFade = crl::time(160);
constexpr auto kSpinnerPeriod = crl::time(1100);
constexpr auto kQuietRestart = crl::time(1500);
constexpr auto kLostResetsLimit = 2;
constexpr auto kDiskBudget = int64(3072) * 1024 * 1024;
constexpr auto kDiskReserve = int64(512) * 1024 * 1024;
constexpr auto kReadLimit = int64(384) * 1024 * 1024;
constexpr auto kRecycleInterval = crl::time(10 * 60 * 1000);
constexpr auto kRecycleEndMargin = int64(20'000);
constexpr auto kNonIdlePeriod = crl::time(5000);
constexpr auto kVolumeSaveDelay = crl::time(150);

[[nodiscard]] int Scaled(int value) {
	return style::ConvertScale(value);
}

void Toast(const std::shared_ptr<Ui::Show> &show, const QString &text) {
	if (show && show->valid() && !text.isEmpty()) {
		show->showToast(text);
	}
}

[[nodiscard]] QString SafeSuffix(const QString &fileName) {
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

// The temp folder of the room (the same one Room::download() writes to,
// removed with RemoveRoomFolder() when the room is left).
[[nodiscard]] QString RoomFolderPath(not_null<Room*> room) {
	return cWorkingDir()
		+ u"tdata/oblivion/rooms/"_q
		+ QString::number(room->selfId())
		+ '_'
		+ room->code()
		+ '/';
}

// Where Room::download() keeps the unfinished file of an item. It is only
// a guess: if the file is not there nothing is read before the download
// is done, the video just starts later.
[[nodiscard]] QString PartPathGuess(
		not_null<Room*> room,
		const QueueItem &item) {
	return RoomFolderPath(room)
		+ item.media
		+ SafeSuffix(item.fileName)
		+ u".part"_q;
}

// Whether path is what Room::download() makes for a media in the folder
// of the room: "<sha256>" with the extension of the item.
[[nodiscard]] bool OwnMediaFile(
		not_null<Room*> room,
		const QString &media,
		const QString &path) {
	return Cloud::ValidMediaId(media)
		&& path.startsWith(RoomFolderPath(room) + media);
}

// Removes what Room::download() has made for a media in a folder: the
// files "<sha256>", "<sha256>.<ext>" and the unfinished ".part" of them.
// Nothing else is ever removed by the player: neither a file of the user
// that was added from another place, nor a cover, nor a video saved from
// Telegram. Returns the bytes that were freed.
int64 RemoveMediaFilesIn(const QString &folder, const QString &media) {
	if (folder.isEmpty() || !Cloud::ValidMediaId(media)) {
		return 0;
	}
	auto result = int64(0);
	const auto mask = QString(media + u"*"_q);
	const auto list = QDir(folder).entryInfoList(
		QStringList{ mask },
		QDir::Files);
	for (const auto &info : list) {
		const auto size = int64(info.size());
		if (QFile::remove(info.absoluteFilePath())) {
			result += size;
		}
	}
	return result;
}

// ---- A file that may still be growing, read by parts.

// Main thread only.
struct FileState {
	QString complete; // The whole file, when it is here.
	QString part; // Where the download is being written.
	int64 size = 0;
	int64 served = 0; // The bytes given to the player that reads it now.
	bool failed = false;
	rpl::event_stream<> changes;
};

// The whole file a player was given is not where it was any more, or is
// not the same one: a file of the user may be moved, renamed or removed
// while it is in the queue, a file of the room may be cleaned away.
[[nodiscard]] bool WholeFileLost(const FileState &state) {
	if (state.complete.isEmpty()) {
		return false;
	}
	const auto info = QFileInfo(state.complete);
	return !info.isFile() || (int64(info.size()) != state.size);
}

[[nodiscard]] QByteArray ReadPart(
		const QString &path,
		int64 offset,
		int64 length) {
	if (path.isEmpty() || offset < 0 || length <= 0) {
		return QByteArray();
	}
	// Opened for one read only: the download renames the file when it is
	// done, nothing may hold it then.
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)
		|| file.size() < offset + length
		|| !file.seek(offset)) {
		return QByteArray();
	}
	auto result = file.read(length);
	return (result.size() == length) ? result : QByteArray();
}

struct TakenParts {
	std::vector<LoadedPart> parts;
	bool failed = false;
};

// Takes out of requested what can be read now. The download writes the
// file from its start, so after the first part that is not here nothing
// else is tried.
[[nodiscard]] TakenParts TakeReadyParts(
		const FileState &state,
		base::flat_set<int64> &requested) {
	auto result = TakenParts();
	if (state.failed) {
		requested.clear();
		result.failed = true;
		return result;
	}
	const auto whole = !state.complete.isEmpty();
	const auto &path = whole ? state.complete : state.part;
	while (!requested.empty()) {
		const auto offset = *requested.begin();
		const auto length = VideoPartLength(offset, state.size);
		auto bytes = ReadPart(path, offset, length);
		if (!bytes.isEmpty()) {
			requested.erase(requested.begin());
			result.parts.push_back({ offset, std::move(bytes) });
		} else if (whole || !length) {
			// The whole file is here and it has no such part: it is not
			// the file the room has promised.
			requested.clear();
			result.parts.clear();
			result.failed = true;
			break;
		} else {
			break;
		}
	}
	return result;
}

class FileLoader final
	: public Media::Streaming::Loader
	, public base::has_weak_ptr {
public:
	explicit FileLoader(std::shared_ptr<FileState> state);

	Storage::Cache::Key baseCacheKey() const override;
	int64 size() const override;

	void load(int64 offset) override;
	void cancel(int64 offset) override;
	void resetPriorities() override;
	void setPriority(int priority) override;
	void stop() override;
	void tryRemoveFromQueue() override;

	rpl::producer<LoadedPart> parts() const override;
	rpl::producer<Media::Streaming::SpeedEstimate> speedEstimate(
		) const override;

	void attachDownloader(
		not_null<Storage::StreamedFileDownloader*> downloader) override;
	void clearAttachedDownloader() override;

private:
	void process();

	const std::shared_ptr<FileState> _state;
	const int64 _size = 0;
	base::flat_set<int64> _requested;
	rpl::event_stream<LoadedPart> _parts;
	base::Timer _poll;
	rpl::lifetime _lifetime;

};

FileLoader::FileLoader(std::shared_ptr<FileState> state)
: _state(std::move(state))
, _size(_state->size)
, _poll([=] { process(); }) {
	_state->changes.events(
	) | rpl::on_next([=] {
		process();
	}, _lifetime);
}

Storage::Cache::Key FileLoader::baseCacheKey() const {
	return {};
}

int64 FileLoader::size() const {
	return _size;
}

// The requests come from the thread of the streaming file.
void FileLoader::load(int64 offset) {
	crl::on_main(this, [=] {
		_requested.emplace(offset);
		process();
	});
}

void FileLoader::cancel(int64 offset) {
	crl::on_main(this, [=] {
		_requested.remove(offset);
	});
}

void FileLoader::resetPriorities() {
}

void FileLoader::setPriority(int priority) {
}

void FileLoader::stop() {
	crl::on_main(this, [=] {
		_requested.clear();
		_poll.cancel();
	});
}

void FileLoader::tryRemoveFromQueue() {
}

rpl::producer<LoadedPart> FileLoader::parts() const {
	return _parts.events();
}

rpl::producer<Media::Streaming::SpeedEstimate> FileLoader::speedEstimate(
		) const {
	return rpl::never<Media::Streaming::SpeedEstimate>();
}

void FileLoader::attachDownloader(
		not_null<Storage::StreamedFileDownloader*> downloader) {
}

void FileLoader::clearAttachedDownloader() {
}

void FileLoader::process() {
	if (_requested.empty()) {
		_poll.cancel();
		return;
	}
	auto taken = TakeReadyParts(*_state, _requested);
	if (_requested.empty()) {
		_poll.cancel();
	} else if (!_poll.isActive()) {
		// The progress of the download comes rarely and before the bytes
		// are flushed to the disk.
		_poll.callEach(kPollInterval);
	}
	const auto weak = base::make_weak(this);
	if (taken.failed) {
		_parts.fire({ LoadedPart::kFailedOffset, QByteArray() });
		return;
	}
	for (auto &part : taken.parts) {
		_state->served += int64(part.bytes.size());
		_parts.fire(std::move(part));
		if (!weak) {
			return;
		}
	}
}

// ---- Looking into a video file (any thread).

struct Probed {
	int64 duration = 0;
	QImage cover;
};

[[nodiscard]] Probed ProbeVideo(const QString &path) {
	auto result = Probed();
	const auto info = Media::Clip::PrepareForSending(path, QByteArray());
	using Video = Ui::PreparedFileInformation::Video;
	if (const auto video = std::get_if<Video>(&info.media)) {
		if (video->duration > 0) {
			result.duration = int64(video->duration);
			result.cover = video->thumbnail;
		}
	}
	return result;
}

[[nodiscard]] int64 VideoBytesLimit(not_null<Room*> room) {
	const auto account = room->account();
	const auto result = account
		? account->limit("video_bytes", kVideoBytesDefault)
		: kVideoBytesDefault;
	return (result > 0) ? result : kVideoBytesDefault;
}

[[nodiscard]] QString TooBigText(int64 limit) {
	return tr::lng_oblivion_room_file_too_big(
		tr::now,
		lt_size,
		QString::number(limit / (1024 * 1024)));
}

[[nodiscard]] std::vector<Upload> VideoUploads(not_null<Room*> room) {
	auto result = std::vector<Upload>();
	for (const auto &upload : room->uploads()) {
		if (upload.kind == Kind::Video) {
			result.push_back(upload);
		}
	}
	return result;
}

// ---- The engine: one per open room, made when the tab is first shown.

class VideoEngine final : public base::has_weak_ptr {
public:
	struct PendingRow {
		int id = 0;
		QString title;
		int percent = -1; // -1: «Подготовка…».
	};

	explicit VideoEngine(not_null<Room*> room);
	~VideoEngine();

	// The tab was opened: the current item is downloaded and played.
	void enablePlayback();
	[[nodiscard]] bool playbackEnabled() const;

	[[nodiscard]] VideoStatus status() const;
	[[nodiscard]] rpl::producer<VideoStatus> statusValue() const;

	// A new frame may be painted.
	[[nodiscard]] rpl::producer<> frames() const;
	// The frame fitted into box (device pixels), the last one shown while
	// the player is being restarted, null if there is nothing yet.
	[[nodiscard]] QImage frame(QSize box);

	void rejoin(); // «Вернуться к просмотру».
	void retry(); // «Повторить».

	// What is being downloaded from Telegram or looked into before it
	// goes to the relay.
	[[nodiscard]] std::vector<PendingRow> pending() const;
	[[nodiscard]] rpl::producer<> pendingChanges() const;
	void cancelPending(int id);

	// Only from a click.
	void addDocument(
		not_null<DocumentData*> document,
		FullMsgId origin,
		std::shared_ptr<Ui::Show> show);
	void addFiles(const QStringList &paths, std::shared_ptr<Ui::Show> show);

private:
	struct Private;
	const std::unique_ptr<Private> _private;

};

struct EngineEntry {
	not_null<Room*> room;
	not_null<VideoEngine*> engine;
};

[[nodiscard]] std::vector<EngineEntry> &Engines() {
	static auto result = std::vector<EngineEntry>();
	return result;
}

[[nodiscard]] VideoEngine *FindEngine(not_null<Room*> room) {
	for (const auto &entry : Engines()) {
		if (entry.room == room) {
			return entry.engine;
		}
	}
	return nullptr;
}

[[nodiscard]] VideoEngine *EnsureEngine(not_null<Room*> room) {
	if (const auto found = FindEngine(room)) {
		return found;
	} else if (room->sample()
		|| !room->session()
		|| (room->state().gone != Gone::No)) {
		return nullptr;
	}
	// Dies with Room::lifetime(): when the room is closed or detached
	// from its account, before the session is gone.
	return room->lifetime().make_state<VideoEngine>(room);
}

struct VideoEngine::Private {
	// A file of the relay, by its sha256: the same file may be in the
	// queue more than once, it is downloaded once.
	struct Source {
		std::shared_ptr<FileState> file;
		Cloud::TransferId transfer = 0;
		int64 ready = 0;
		int64 total = 0;
		int failures = 0;
		crl::time failedAt = 0;
		crl::time usedAt = 0; // When it was the current item.
		bool requested = false;
		bool complete = false;
		bool failed = false;
		bool noSpace = false; // Failed: the disk has no room for it.
		bool own = false; // Made by Room::download() in the room folder.

		[[nodiscard]] int percent() const {
			if (complete) {
				return 100;
			}
			return (total > 0)
				? int(std::clamp(ready * 100 / total, int64(0), int64(99)))
				: 0;
		}
	};
	// One play() of the streaming player.
	struct Session {
		bool active = false;
		bool ready = false; // The first frame is here.
		bool running = false;
		bool scheduled = false;
		bool finished = false;
		bool waiting = false; // The player waits for the bytes.
		bool direct = false; // Started without the pause.
		bool silent = false; // Only the picture: the sound is not ours.
		int64 rev = -1;
		int64 position = 0;
		crl::time positionAt = 0; // 0: position is not a played one.
		crl::time issuedAt = 0;
		crl::time startAt = 0;
	};
	struct Pending {
		int id = 0;
		QString title;
		QString fileName;
		QString mime;
		DocumentData *document = nullptr; // nullptr: a file of the user.
		std::shared_ptr<Data::DocumentMedia> media;
		FullMsgId origin;
		std::shared_ptr<Ui::Show> show;
		bool probing = false;
		int percent = -1;
	};

	Private(not_null<VideoEngine*> owner, not_null<Room*> room);

	void sync();
	void scheduleSync();
	void updateProgress();
	void report(const QString &itemId, bool ready, int percent);

	[[nodiscard]] Source *findSource(const QString &media);
	Source &ensureSource(const QueueItem &item);
	void dropSource(Source &source);
	void resetSource(Source &source);
	[[nodiscard]] bool fileLost(const Source &source) const;
	[[nodiscard]] bool makeSpace(const QueueItem &item);
	void cleanupSources();
	void preloadNext();
	void fileReady(const QString &media, const QString &path);
	void fileFailed(const QString &media);
	void fileProgress(const QString &media, int64 ready, int64 total);

	void openDocument(const Source &source);
	void closeDocument();
	void closeSession();
	void keepFrame();
	void startSession(
		int64 from,
		bool play,
		crl::time wait,
		int64 rev,
		bool silent);
	void startDirect(int64 target, int64 rev);
	void holdSession();
	void resumeSession();
	void setSpeed(double value);
	[[nodiscard]] int64 localPosition(crl::time now) const;
	void handleUpdate(Media::Streaming::Update &&update);
	void handleError(Media::Streaming::Error error);

	[[nodiscard]] AudioMsgId soundId() const;
	[[nodiscard]] bool soundIsOurs() const;
	[[nodiscard]] bool soundTaken() const;
	[[nodiscard]] bool otherVideoPlays() const;
	void pauseOtherSound();
	void keepMusicVolume();
	void applyVolume();
	void restoreVolume();

	void setStatus(VideoStatus value);

	[[nodiscard]] Pending *findPending(int id);
	void pendingChanged();
	void checkDocument(not_null<DocumentData*> document);
	void checkDocuments();
	void startProbe(Pending &entry, const QString &path);
	void finishAdd(int id, const QString &path, Probed &&probed);

	const not_null<VideoEngine*> owner;
	const not_null<Room*> room;
	base::Timer timer;
	std::map<QString, Source> sources;
	QString current; // The item of the room the engine follows.
	QString currentMedia;
	std::shared_ptr<Media::Streaming::Document> document;
	std::unique_ptr<Media::Streaming::Instance> instance;
	FileState *documentFile = nullptr;
	rpl::lifetime updates;
	Session session;
	SyncCorrector corrector;
	rpl::variable<VideoStatus> status;
	rpl::event_stream<> frames;
	QImage held;
	QSize frameSize;
	crl::time prepareLead = kPrepareLeadDefault;
	crl::time recycledAt = 0; // The player was reopened to free the memory.
	int64 seenRev = -1;
	int restarts = 0;
	int playerFailures = 0;
	int lostResets = 0;
	int mutedTicks = 0;
	double speed = 1.;
	bool enabled = false;
	bool hasVideo = false;
	bool hasAudio = false;
	bool away = false;
	bool insist = false; // «Вернуться к просмотру» was clicked.
	bool unplayable = false;
	bool progressiveFailed = false;
	bool playerError = false;
	bool syncScheduled = false;
	bool volumeScaled = false;
	bool shownOnce = false; // The current item was seen on this device.
	QString reportedItem;
	bool reportedReady = false;
	int reportedPercent = -1;
	std::vector<Pending> pending;
	int pendingIds = 0;
	rpl::event_stream<> pendingChanges;
	rpl::lifetime lifetime;

};

VideoEngine::Private::Private(
	not_null<VideoEngine*> owner,
	not_null<Room*> room)
: owner(owner)
, room(room)
, timer([=] { sync(); }) {
	room->changes(
	) | rpl::on_next([=](Changes changes) {
		if (changes & (Changes(Change::VideoQueue)
			| Change::VideoPlayer
			| Change::Gone
			| Change::Reloaded)) {
			sync();
		}
	}, lifetime);

	Oblivion::Get().changes(
	) | rpl::on_next([=] {
		applyVolume();
	}, lifetime);

	if (const auto session = room->session()) {
		session->data().documentLoadProgress(
		) | rpl::on_next([=](not_null<DocumentData*> document) {
			checkDocument(document);
		}, lifetime);

		session->downloaderTaskFinished(
		) | rpl::on_next([=] {
			checkDocuments();
		}, lifetime);
	}
}

void VideoEngine::Private::setStatus(VideoStatus value) {
	status = value;
}

void VideoEngine::Private::scheduleSync() {
	if (syncScheduled) {
		return;
	}
	syncScheduled = true;
	crl::on_main(owner, [=] {
		syncScheduled = false;
		sync();
	});
}

VideoEngine::Private::Source *VideoEngine::Private::findSource(
		const QString &media) {
	const auto i = sources.find(media);
	return (i != end(sources)) ? &i->second : nullptr;
}

void VideoEngine::Private::dropSource(Source &source) {
	if (source.transfer) {
		room->cancelDownload(base::take(source.transfer));
	}
	if (source.file && !source.complete) {
		// Whoever still reads it gets an error instead of waiting.
		source.file->failed = true;
		source.file->changes.fire({});
	}
}

// The source is asked for anew: from the disk if its file is (again)
// here, from the relay if it is not.
void VideoEngine::Private::resetSource(Source &source) {
	if (source.file && documentFile == source.file.get()) {
		closeDocument();
	}
	dropSource(source);
	source = Source();
}

bool VideoEngine::Private::fileLost(const Source &source) const {
	return source.complete && source.file && WholeFileLost(*source.file);
}

// The room itself removes the file of an item that has left both queues
// and the whole folder some time after its window was closed. This is
// about what is still in the queue of the video. Before a file is brought
// from the relay, what was watched earlier is removed while the files the
// player has here take more than the budget or the disk is short (such
// an item is downloaded again when its turn comes). Never the current
// item, the next one, a file of the queue of the music or the one the
// player of the app holds now. false: there is no room for the file even
// so.
bool VideoEngine::Private::makeSpace(const QueueItem &item) {
	const auto &data = room->player(Kind::Video);
	const auto next = data.find(NextAfterEnd(data));
	const auto &music = room->player(Kind::Music).queue;
	const auto song = Media::Player::instance()->current(
		AudioMsgId::Type::Song).audio();
	const auto played = song ? song->filepath(true) : QString();
	auto files = std::vector<VideoFile>();
	for (const auto &[media, source] : sources) {
		if (!source.own) {
			continue;
		}
		files.push_back({
			.media = media,
			.bytes = source.complete ? source.ready : source.total,
			.usedAt = source.usedAt,
			.kept = !source.complete
				|| (media == item.media)
				|| (media == currentMedia)
				|| (next && media == next->media)
				|| ranges::contains(music, media, &QueueItem::media)
				|| OwnMediaFile(room, media, played),
		});
	}
	const auto storage = QStorageInfo(cWorkingDir());
	const auto available = (storage.isValid() && storage.isReady())
		? int64(storage.bytesAvailable())
		: int64(-1);
	const auto have = int64(QFileInfo(PartPathGuess(room, item)).size());
	const auto needed = std::max(item.size - have, int64(0)) + kDiskReserve;
	const auto plan = PlanVideoSpace(
		std::move(files),
		kDiskBudget,
		available,
		needed);
	for (const auto &media : plan.evict) {
		sources.erase(media);
		RemoveMediaFilesIn(RoomFolderPath(room), media);
	}
	return plan.enough;
}

VideoEngine::Private::Source &VideoEngine::Private::ensureSource(
		const QueueItem &item) {
	auto &source = sources[item.media];
	if (source.failed
		&& (source.noSpace || source.failures < kRetryLimit)
		&& (crl::now() - source.failedAt > kRetryDelay)) {
		source.failed = false;
		source.noSpace = false;
		source.requested = false;
		source.file = nullptr;
	}
	if (source.requested || source.failed) {
		return source;
	}
	source.requested = true;
	source.file = std::make_shared<FileState>();
	const auto local = room->localFile(item);
	if (!local.isEmpty()) {
		const auto size = QFileInfo(local).size();
		if (size <= 0) {
			source.failed = true;
			source.failures = kRetryLimit;
			return source;
		}
		source.file->complete = local;
		source.file->size = size;
		source.complete = true;
		source.own = OwnMediaFile(room, item.media, local);
		source.ready = source.total = size;
		return source;
	}
	source.own = true;
	source.total = item.size;
	if (!makeSpace(item)) {
		// Asked again by itself in a while and by «Повторить».
		source.own = false;
		source.failed = true;
		source.noSpace = true;
		source.failedAt = crl::now();
		return source;
	}
	source.file->part = PartPathGuess(room, item);
	source.file->size = item.size;
	const auto media = item.media;
	const auto guard = owner.get();
	source.transfer = room->download(
		item,
		crl::guard(guard, [=](const QString &path) {
			fileReady(media, path);
		}),
		crl::guard(guard, [=](const Cloud::Error &) {
			fileFailed(media);
		}),
		crl::guard(guard, [=](int64 ready, int64 total) {
			fileProgress(media, ready, total);
		}));
	return source;
}

void VideoEngine::Private::cleanupSources() {
	const auto &queue = room->player(Kind::Video).queue;
	for (auto i = begin(sources); i != end(sources);) {
		if (ranges::contains(queue, i->first, &QueueItem::media)) {
			++i;
			continue;
		}
		if (i->first == currentMedia) {
			closeDocument();
		}
		dropSource(i->second);
		i = sources.erase(i);
	}
}

// The file of the item that goes next is brought while the current one
// plays, so that the switch is not a wait.
void VideoEngine::Private::preloadNext() {
	const auto source = findSource(currentMedia);
	if (!source || !source->complete) {
		return;
	}
	const auto &data = room->player(Kind::Video);
	const auto next = data.find(NextAfterEnd(data));
	if (next && next->media != currentMedia) {
		ensureSource(*next);
	}
}

void VideoEngine::Private::fileReady(
		const QString &media,
		const QString &path) {
	const auto source = findSource(media);
	if (!source || !source->file) {
		return;
	}
	source->transfer = 0;
	const auto size = QFileInfo(path).size();
	if (size <= 0) {
		fileFailed(media);
		return;
	}
	if (source->file->size != size) {
		// Not what was promised: whoever reads the old state stops, the
		// file is opened anew as it is.
		source->file->failed = true;
		source->file->changes.fire({});
		source->file = std::make_shared<FileState>();
		source->file->size = size;
	}
	source->complete = true;
	source->own = OwnMediaFile(room, media, path);
	source->ready = source->total = size;
	source->file->complete = path;
	source->file->changes.fire({});
	sync();
}

void VideoEngine::Private::fileFailed(const QString &media) {
	const auto source = findSource(media);
	if (!source) {
		return;
	}
	source->transfer = 0;
	source->failed = true;
	source->failedAt = crl::now();
	++source->failures;
	if (source->file) {
		source->file->failed = true;
		source->file->changes.fire({});
	}
	sync();
}

void VideoEngine::Private::fileProgress(
		const QString &media,
		int64 ready,
		int64 total) {
	const auto source = findSource(media);
	if (!source || source->complete) {
		return;
	}
	const auto was = source->percent();
	source->ready = ready;
	if (total > 0) {
		source->total = total;
	}
	if (source->file) {
		source->file->changes.fire({});
	}
	if (media == currentMedia && source->percent() != was) {
		updateProgress();
	}
}

void VideoEngine::Private::report(
		const QString &itemId,
		bool ready,
		int percent) {
	if (reportedItem == itemId
		&& reportedReady == ready
		&& reportedPercent == percent) {
		return;
	}
	reportedItem = itemId;
	reportedReady = ready;
	reportedPercent = percent;
	room->reportStatus(Kind::Video, itemId, ready, percent);
}

void VideoEngine::Private::updateProgress() {
	const auto source = findSource(currentMedia);
	if (!source || current.isEmpty()) {
		return;
	}
	const auto percent = source->percent();
	const auto now = status.current();
	if (now.state == VideoState::Loading
		|| now.state == VideoState::Buffering) {
		setStatus({ .state = now.state, .percent = percent });
	}
	report(current, session.ready || source->complete, percent);
}

void VideoEngine::Private::openDocument(const Source &source) {
	Expects(source.file != nullptr);

	closeDocument();
	documentFile = source.file.get();
	documentFile->served = 0;
	document = std::make_shared<Media::Streaming::Document>(
		std::make_unique<FileLoader>(source.file));
	instance = std::make_unique<Media::Streaming::Instance>(
		document,
		[] {});
	instance->player().updates(
	) | rpl::on_next_error([=](Media::Streaming::Update &&update) {
		handleUpdate(std::move(update));
	}, [=](Media::Streaming::Error &&error) {
		handleError(error);
	}, updates);
}

void VideoEngine::Private::closeDocument() {
	closeSession();
	updates.destroy();
	instance = nullptr;
	document = nullptr;
	documentFile = nullptr;
	hasVideo = false;
	hasAudio = false;
}

// The last picture stays on the screen while the player is restarted.
void VideoEngine::Private::keepFrame() {
	if (!instance
		|| !session.ready
		|| !hasVideo
		|| frameSize.isEmpty()
		|| !instance->player().ready()
		|| instance->player().videoSize().isEmpty()) {
		return;
	}
	const auto image = instance->frame(Media::Streaming::FrameRequest{
		.resize = frameSize,
		.outer = frameSize,
	});
	if (!image.isNull()) {
		held = image;
	}
}

void VideoEngine::Private::closeSession() {
	if (instance && session.active) {
		keepFrame();
		instance->stop();
	}
	session = Session();
	speed = 1.;
	mutedTicks = 0;
	restoreVolume();
}

void VideoEngine::Private::startSession(
		int64 from,
		bool play,
		crl::time wait,
		int64 rev,
		bool silent) {
	Expects(instance != nullptr);

	keepFrame();
	const auto position = std::max(from, int64(0));
	auto options = Media::Streaming::PlaybackOptions();
	if (silent) {
		// Another video of the app has the sound: only the frame is
		// shown, its sound is not taken away.
		options.mode = Media::Streaming::Mode::Video;
	}
	options.position = crl::time(position);
	options.hwAllowed = Core::App().settings().hardwareAcceleratedVideo();
	instance->play(options);
	if (instance->active()) {
		// Prepared on pause: the first frame is shown, resumed exactly
		// when the room comes to it.
		instance->pause();
	}
	const auto now = crl::now();
	session = Session{
		.active = true,
		.scheduled = play,
		.silent = silent,
		.rev = rev,
		.position = position,
		.issuedAt = now,
		.startAt = now + wait,
	};
	speed = 1.;
	mutedTicks = 0;
	if (play) {
		++restarts;
	}
}

// A jump made by the correction: no pause, the player goes as soon as it
// can, the corrector learns how long that takes.
void VideoEngine::Private::startDirect(int64 target, int64 rev) {
	Expects(instance != nullptr);

	keepFrame();
	const auto position = std::max(target, int64(0));
	auto options = Media::Streaming::PlaybackOptions();
	options.position = crl::time(position);
	options.hwAllowed = Core::App().settings().hardwareAcceleratedVideo();
	instance->play(options);
	session = Session{
		.active = true,
		.running = true,
		.direct = true,
		.rev = rev,
		.position = position,
		.issuedAt = crl::now(),
	};
	speed = 1.;
	mutedTicks = 0;
}

void VideoEngine::Private::holdSession() {
	if (!session.active) {
		return;
	}
	const auto position = localPosition(crl::now());
	if (instance && instance->active()) {
		instance->pause();
	}
	session.running = false;
	session.scheduled = false;
	session.waiting = false;
	session.position = position;
	session.positionAt = 0;
	setSpeed(1.);
}

void VideoEngine::Private::resumeSession() {
	session.scheduled = false;
	if (!session.active || !instance || !instance->active()) {
		return;
	}
	pauseOtherSound();
	instance->resume();
	session.running = true;
	session.positionAt = 0;
	insist = false;
	corrector.started(crl::now(), session.position, true);
	applyVolume();
}

void VideoEngine::Private::setSpeed(double value) {
	if (speed == value) {
		return;
	}
	speed = value;
	if (instance && session.active) {
		instance->setSpeed(value);
	}
}

int64 VideoEngine::Private::localPosition(crl::time now) const {
	if (!session.active) {
		return 0;
	}
	auto result = session.position;
	if (session.running
		&& session.ready
		&& !session.waiting
		&& !session.finished
		&& session.positionAt > 0) {
		// The position comes with every frame, between the frames the
		// picture is where the clock says.
		const auto passed = std::clamp(
			now - session.positionAt,
			crl::time(0),
			kExtrapolateMax);
		result += int64(base::SafeRound(passed * speed));
	}
	return result;
}

// Called from inside the player: nothing here may stop or restart it,
// that is done by sync() from the event loop.
void VideoEngine::Private::handleUpdate(Media::Streaming::Update &&update) {
	using namespace Media::Streaming;
	v::match(update.data, [&](Information &info) {
		session.ready = true;
		shownOnce = true;
		hasVideo = !info.video.size.isEmpty();
		hasAudio = (info.audio.state.duration != Media::kTimeUnknown);
		const auto took = crl::now() - session.issuedAt;
		if (!session.direct && took > 0 && took < kPrepareMeasureMax) {
			prepareLead = VideoPrepareLead(prepareLead, took);
		}
		const auto direct = session.direct;
		crl::on_main(owner, [=] {
			keepMusicVolume();
			if (direct && session.active && session.running) {
				pauseOtherSound();
				applyVolume();
			}
		});
		frames.fire({});
		scheduleSync();
	}, [](PreloadedVideo) {
	}, [&](UpdateVideo data) {
		session.position = int64(data.position);
		session.positionAt = crl::now();
		frames.fire({});
	}, [](PreloadedAudio) {
	}, [&](UpdateAudio data) {
		if (!hasVideo) {
			session.position = int64(data.position);
			session.positionAt = crl::now();
		}
	}, [&](WaitingForData data) {
		if (session.waiting == data.waiting) {
			return;
		}
		session.waiting = data.waiting;
		if (!data.waiting && session.running) {
			// The wait says nothing about the latency of a start.
			session.positionAt = 0;
			corrector.started(crl::now(), session.position, true);
		}
		scheduleSync();
	}, [](SpeedEstimate) {
	}, [](MutedByOther) {
	}, [&](Finished) {
		session.finished = true;
		scheduleSync();
	});
}

void VideoEngine::Private::handleError(Media::Streaming::Error error) {
	playerError = true;
	scheduleSync();
}

AudioMsgId VideoEngine::Private::soundId() const {
	if (!instance || !session.active || !session.ready || !hasAudio) {
		return AudioMsgId();
	}
	const auto id = instance->player().prepareLegacyState().id;
	return id.externalPlayId() ? id : AudioMsgId();
}

bool VideoEngine::Private::soundIsOurs() const {
	const auto id = soundId();
	if (!id) {
		return false;
	}
	const auto state = Media::Player::mixer()->currentState(
		AudioMsgId::Type::Video);
	return (state.id == id);
}

// Another video with sound of the app plays through the only video track
// of the mixer: ours is not heard any more.
bool VideoEngine::Private::soundTaken() const {
	const auto id = soundId();
	if (!id) {
		return false;
	}
	const auto state = Media::Player::mixer()->currentState(
		AudioMsgId::Type::Video);
	return state.id && (state.id != id);
}

// A video of the app itself (the media viewer) plays with sound now.
bool VideoEngine::Private::otherVideoPlays() const {
	const auto state = Media::Player::mixer()->currentState(
		AudioMsgId::Type::Video);
	if (!state.id || !Media::Player::ShowPauseIcon(state.state)) {
		return false;
	}
	const auto ours = soundId();
	return !ours || (state.id != ours);
}

// Mixer::play() of a video puts the volume of the songs back to the one
// of the app: a track of the room that plays now gets its own volume
// again (the same value the music of the room sets).
void VideoEngine::Private::keepMusicVolume() {
	const auto scale = Oblivion::Get().roomMusicVolume() / 100.;
	if (!hasAudio || scale >= 1.) {
		return;
	}
	const auto current = Media::Player::instance()->current(
		AudioMsgId::Type::Song);
	if (current && DrivesPlayer(current)) {
		Media::Player::mixer()->setSongVolume(
			Core::App().settings().songVolume() * scale);
	}
}

// The music and the voice messages of the app give way to the video, as
// they do for the media viewer. The music of the room itself is paused
// by the room.
void VideoEngine::Private::pauseOtherSound() {
	if (!hasAudio || Oblivion::Get().roomVideoVolume() <= 0) {
		return;
	}
	const auto player = Media::Player::instance();
	const auto types = {
		AudioMsgId::Type::Song,
		AudioMsgId::Type::Voice,
	};
	for (const auto type : types) {
		const auto state = player->getState(type);
		if (!state.id || !Media::Player::ShowPauseIcon(state.state)) {
			continue;
		} else if (type == AudioMsgId::Type::Song
			&& DrivesPlayer(state.id)) {
			continue;
		}
		player->pause(type);
	}
}

// The own volume of the room video: the volume of the app scaled while
// our sound is in the mixer, the setting of the app is never changed.
void VideoEngine::Private::applyVolume() {
	if (!soundIsOurs()) {
		volumeScaled = false;
		return;
	}
	const auto base = Core::App().settings().videoVolume();
	const auto scale = Oblivion::Get().roomVideoVolume() / 100.;
	Media::Player::mixer()->setVideoVolume(base * scale);
	volumeScaled = (scale < 1.);
}

void VideoEngine::Private::restoreVolume() {
	if (volumeScaled) {
		volumeScaled = false;
		Media::Player::mixer()->setVideoVolume(
			Core::App().settings().videoVolume());
	}
}

void VideoEngine::Private::sync() {
	timer.cancel();
	const auto &state = room->state();
	const auto &data = state.video;
	const auto item = (enabled && state.gone == Gone::No)
		? data.current()
		: nullptr;
	cleanupSources();
	if (!item) {
		closeDocument();
		current = QString();
		currentMedia = QString();
		if (!held.isNull()) {
			held = QImage();
			frames.fire({});
		}
		setStatus({});
		return;
	}
	if (current != item->id || currentMedia != item->media) {
		closeDocument();
		current = item->id;
		currentMedia = item->media;
		held = QImage();
		restarts = 0;
		playerFailures = 0;
		unplayable = false;
		progressiveFailed = false;
		playerError = false;
		shownOnce = false;
		seenRev = -1;
		lostResets = 0;
		recycledAt = 0;
		corrector.reset();
		if (const auto waited = findSource(currentMedia)) {
			if (waited->noSpace) {
				// There was no room for it while it was the next one:
				// what was played before it may go now.
				resetSource(*waited);
			}
		}
		frames.fire({});
	}
	auto &source = ensureSource(*item);
	source.usedAt = crl::now();
	preloadNext();
	if (playerError) {
		playerError = false;
		closeDocument();
		if (source.failed) {
			// The download has failed, the player only says so.
		} else if (!source.complete) {
			// Could not be played while it grows: once more when the
			// whole file is here.
			progressiveFailed = true;
		} else if (fileLost(source) && lostResets < kLostResetsLimit) {
			// The file was moved or removed while it was in the queue:
			// it is asked for anew, the relay has its copy.
			++lostResets;
			resetSource(source);
			ensureSource(*item);
			source.usedAt = crl::now();
		} else if (++playerFailures >= kPlayerFailuresLimit) {
			unplayable = true;
		}
	}
	const auto percent = source.percent();
	if (source.failed) {
		closeDocument();
		setStatus({
			.state = source.noSpace
				? VideoState::NoSpace
				: VideoState::Failed,
		});
		report(item->id, false, percent);
		if (source.noSpace || source.failures < kRetryLimit) {
			timer.callOnce(kRetryDelay + 200);
		}
		return;
	} else if (unplayable) {
		closeDocument();
		setStatus({ .state = VideoState::Unplayable });
		report(item->id, source.complete, percent);
		return;
	}
	const auto usable = source.file
		&& (source.file->size > 0)
		&& (source.complete || !progressiveFailed);
	if (!usable) {
		closeDocument();
		setStatus({ .state = VideoState::Loading, .percent = percent });
		report(item->id, false, percent);
		return;
	}
	if (document
		&& documentFile == source.file.get()
		&& session.running
		&& session.ready
		&& !session.waiting
		&& !session.finished
		&& data.state.playing
		&& VideoReaderRestart(
			documentFile->served,
			(recycledAt > 0) ? (crl::now() - recycledAt) : kRecycleInterval,
			item->duration
				- PositionAt(data.state, item->duration, room->now()))) {
		// The reader of the player holds in the memory every byte it has
		// read from the file. A new one starts empty: the picture stands
		// for a part of a second (the last frame stays), the player is
		// prepared at the place the room will have, as after any jump.
		recycledAt = crl::now();
		closeDocument();
		restarts = 0;
	}
	if (!document || documentFile != source.file.get()) {
		openDocument(source);
	}
	if (seenRev != data.state.rev) {
		seenRev = data.state.rev;
		restarts = 0;
	}
	if (away) {
		holdSession();
		restoreVolume();
		setStatus({ .state = VideoState::Away });
		return;
	}

	const auto rev = data.state.rev;
	const auto nowLocal = crl::now();
	// A session that has no sound of its own (prepared while another
	// video was playing, or its sound was taken while it stood on pause)
	// can show a frame, but it is not what is resumed. Not forever: with
	// no sound device at all the video plays as it is.
	const auto mute = session.active
		&& (restarts < kRestartLimit)
		&& data.state.playing
		&& !session.running
		&& (session.silent
			|| (session.ready && hasAudio && !soundIsOurs()));
	// A prepared start whose moment has passed while the player was
	// getting ready is decided anew.
	const auto late = data.state.playing
		&& session.scheduled
		&& session.ready
		&& (nowLocal - session.startAt > kPausedTolerance);
	const auto step = NextVideoStep(
		data.state,
		item->duration,
		room->now(),
		prepareLead,
		VideoLocal{
			.active = session.active && !mute,
			.running = session.running,
			.scheduled = session.scheduled && !late,
			.finished = session.finished,
			.force = (restarts >= kRestartLimit),
			.rev = session.rev,
			.position = localPosition(nowLocal),
		});
	const auto starts = (step.action == VideoAction::Prepare)
		|| (step.action == VideoAction::Resume);
	const auto busy = starts && !insist && otherVideoPlays();
	if (busy && data.state.playing) {
		// The user watches another video of the app: its sound is not
		// taken away, the room is joined by a click.
		away = true;
		holdSession();
		setStatus({ .state = VideoState::Away });
		return;
	}
	switch (step.action) {
	case VideoAction::None:
		break;
	case VideoAction::Stop:
		closeSession();
		break;
	case VideoAction::Prepare:
		corrector.reset();
		startSession(step.from, step.play, step.wait, rev, busy);
		break;
	case VideoAction::Hold:
		holdSession();
		session.rev = rev;
		break;
	case VideoAction::Resume:
		session.rev = rev;
		session.scheduled = true;
		session.startAt = nowLocal + step.wait;
		break;
	case VideoAction::Adopt:
		session.rev = rev;
		corrector.reset();
		break;
	}

	auto next = crl::time(0);
	if (session.scheduled && session.ready) {
		const auto left = session.startAt - crl::now();
		if (left <= 0) {
			resumeSession();
		} else {
			next = left;
		}
	}
	if (session.running
		&& session.ready
		&& !session.finished
		&& data.state.playing) {
		// Checked twice in a row: our own restart is not somebody else.
		mutedTicks = soundTaken() ? (mutedTicks + 1) : 0;
		if (mutedTicks >= kMutedTicks) {
			mutedTicks = 0;
			away = true;
			insist = false;
			holdSession();
			volumeScaled = false;
			setStatus({ .state = VideoState::Away });
			return;
		}
		applyVolume();
		const auto serverNow = room->now();
		if (!session.waiting
			&& step.state != VideoState::Waiting
			&& serverNow > data.state.anchor) {
			const auto result = corrector.update(
				crl::now(),
				PositionAt(data.state, item->duration, serverNow),
				localPosition(crl::now()));
			if (result.seek) {
				const auto limit = std::max(
					item->duration - kEndMargin,
					int64(0));
				startDirect(std::clamp(result.target, int64(0), limit), rev);
			} else {
				setSpeed(result.speed);
			}
		}
		next = (next > 0) ? std::min(next, kTick) : kTick;
	}

	auto shown = VideoStatus{ .state = step.state };
	const auto restarting = crl::now() - session.issuedAt;
	if (!session.ready
		&& !source.complete
		&& shownOnce
		&& session.active
		&& restarting < kQuietRestart) {
		// A jump inside what is downloaded takes a moment: the picture
		// stays, the loading is shown only if it really is one.
		if (step.state != VideoState::Paused) {
			shown.state = VideoState::Starting;
		}
		const auto left = kQuietRestart - restarting + 20;
		next = (next > 0) ? std::min(next, left) : left;
	} else if (!session.ready && !source.complete) {
		shown = VideoStatus{
			.state = VideoState::Loading,
			.percent = percent,
		};
	} else if (step.state == VideoState::Paused
		|| step.state == VideoState::Waiting
		|| step.state == VideoState::Idle) {
		// As the room says.
	} else if (!session.ready || !session.running) {
		shown.state = VideoState::Starting;
	} else if (session.waiting) {
		shown = VideoStatus{
			.state = VideoState::Buffering,
			.percent = percent,
		};
	} else if (corrector.awaiting()) {
		shown.state = VideoState::Starting;
	} else if (corrector.speed() != 1.) {
		shown.state = VideoState::Catching;
	} else {
		shown.state = VideoState::Synced;
	}
	setStatus(shown);
	report(item->id, session.ready || source.complete, percent);
	if (next > 0) {
		timer.callOnce(next);
	}
}

// ---- Adding: a video of Telegram or a file of the user.

VideoEngine::Private::Pending *VideoEngine::Private::findPending(int id) {
	const auto i = ranges::find(pending, id, &Pending::id);
	return (i != end(pending)) ? &*i : nullptr;
}

void VideoEngine::Private::pendingChanged() {
	pendingChanges.fire({});
}

void VideoEngine::Private::startProbe(Pending &entry, const QString &path) {
	entry.probing = true;
	entry.percent = -1;
	const auto id = entry.id;
	auto known = Probed();
	if (const auto document = entry.document) {
		known.duration = int64(std::max(document->duration(), crl::time(0)));
		if (const auto thumbnail = entry.media
			? entry.media->thumbnail()
			: nullptr) {
			known.cover = thumbnail->original();
		}
	}
	const auto weak = base::make_weak(owner);
	crl::async([=, known = std::move(known)]() mutable {
		if (known.duration <= 0 || known.cover.isNull()) {
			auto found = ProbeVideo(path);
			if (known.duration <= 0) {
				known.duration = found.duration;
			}
			if (known.cover.isNull()) {
				known.cover = std::move(found.cover);
			}
		}
		crl::on_main(weak, [=, known = std::move(known)]() mutable {
			finishAdd(id, path, std::move(known));
		});
	});
}

void VideoEngine::Private::finishAdd(
		int id,
		const QString &path,
		Probed &&probed) {
	const auto i = ranges::find(pending, id, &Pending::id);
	if (i == end(pending)) {
		return; // Cancelled.
	}
	const auto entry = std::move(*i);
	pending.erase(i);
	pendingChanged();
	if (room->state().gone != Gone::No) {
		return;
	} else if (probed.duration <= 0) {
		Toast(entry.show, tr::lng_oblivion_rvideo_not_video(tr::now));
		return;
	} else if (!room->can(Right::Add)) {
		Toast(entry.show, tr::lng_oblivion_rvideo_no_add(tr::now));
		return;
	}
	room->addMedia({
		.kind = Kind::Video,
		.path = path,
		.title = entry.title,
		.fileName = entry.fileName,
		.mime = entry.mime,
		.duration = probed.duration,
		.cover = std::move(probed.cover),
	});
}

void VideoEngine::Private::checkDocument(not_null<DocumentData*> document) {
	auto changed = false;
	for (auto i = begin(pending); i != end(pending);) {
		if (i->document != document.get() || i->probing) {
			++i;
			continue;
		}
		const auto path = document->filepath(true);
		if (!path.isEmpty()) {
			startProbe(*i, path);
			changed = true;
			++i;
		} else if (!document->loading()) {
			Toast(i->show, tr::lng_oblivion_rvideo_download_failed(tr::now));
			i = pending.erase(i);
			changed = true;
		} else {
			const auto percent = int(std::clamp(
				base::SafeRound(document->progress() * 100.),
				0.,
				99.));
			if (i->percent != percent) {
				i->percent = percent;
				changed = true;
			}
			++i;
		}
	}
	if (changed) {
		pendingChanged();
	}
}

void VideoEngine::Private::checkDocuments() {
	auto documents = std::vector<not_null<DocumentData*>>();
	for (const auto &entry : pending) {
		if (entry.document && !entry.probing) {
			documents.push_back(entry.document);
		}
	}
	for (const auto &document : documents) {
		checkDocument(document);
	}
}

VideoEngine::VideoEngine(not_null<Room*> room)
: _private(std::make_unique<Private>(this, room)) {
	Engines().push_back({ room, this });
}

VideoEngine::~VideoEngine() {
	auto &list = Engines();
	list.erase(
		ranges::remove(list, not_null(this), &EngineEntry::engine),
		end(list));
	const auto p = _private.get();
	p->lifetime.destroy();
	p->timer.cancel();
	p->closeDocument();
	for (auto &[id, source] : p->sources) {
		p->dropSource(source);
	}
	p->sources.clear();
	p->pending.clear();
}

void VideoEngine::enablePlayback() {
	const auto p = _private.get();
	if (!p->enabled) {
		p->enabled = true;
		p->scheduleSync();
	}
}

bool VideoEngine::playbackEnabled() const {
	return _private->enabled;
}

VideoStatus VideoEngine::status() const {
	return _private->status.current();
}

rpl::producer<VideoStatus> VideoEngine::statusValue() const {
	return _private->status.value();
}

rpl::producer<> VideoEngine::frames() const {
	return _private->frames.events();
}

QImage VideoEngine::frame(QSize box) {
	const auto p = _private.get();
	if (p->instance
		&& p->session.ready
		&& p->hasVideo
		&& p->instance->player().ready()) {
		const auto video = p->instance->player().videoSize();
		const auto size = VideoFrameSize(
			video,
			box,
			QSize(kFrameLimitWidth, kFrameLimitHeight));
		if (!size.isEmpty()) {
			p->frameSize = size;
			return p->instance->frame(Media::Streaming::FrameRequest{
				.resize = size,
				.outer = size,
			});
		}
	}
	return p->held;
}

void VideoEngine::rejoin() {
	const auto p = _private.get();
	p->away = false;
	p->insist = true;
	p->restarts = 0;
	p->mutedTicks = 0;
	p->corrector.reset();
	p->sync();
}

void VideoEngine::retry() {
	const auto p = _private.get();
	if (const auto source = p->findSource(p->currentMedia)) {
		// A whole file that is not where it was any more (a file of the
		// user that was moved or removed, a drive that was unplugged) is
		// not opened again: the copy of the relay is taken.
		if (source->failed || p->fileLost(*source)) {
			p->resetSource(*source);
		}
	}
	p->unplayable = false;
	p->playerFailures = 0;
	p->progressiveFailed = false;
	p->lostResets = 0;
	p->sync();
}

std::vector<VideoEngine::PendingRow> VideoEngine::pending() const {
	auto result = std::vector<PendingRow>();
	for (const auto &entry : _private->pending) {
		result.push_back({
			.id = entry.id,
			.title = entry.title,
			.percent = entry.probing ? -1 : entry.percent,
		});
	}
	return result;
}

rpl::producer<> VideoEngine::pendingChanges() const {
	return _private->pendingChanges.events();
}

void VideoEngine::cancelPending(int id) {
	const auto p = _private.get();
	const auto i = ranges::find(p->pending, id, &Private::Pending::id);
	if (i != end(p->pending)) {
		// A download from Telegram goes on: the user may want the video
		// itself, only the room does not get it.
		p->pending.erase(i);
		p->pendingChanged();
	}
}

void VideoEngine::addDocument(
		not_null<DocumentData*> document,
		FullMsgId origin,
		std::shared_ptr<Ui::Show> show) {
	const auto p = _private.get();
	const auto room = p->room;
	if (room->state().gone != Gone::No) {
		return;
	} else if (!room->can(Right::Add)) {
		Toast(show, tr::lng_oblivion_rvideo_no_add(tr::now));
		return;
	} else if (ranges::contains(
			p->pending,
			document.get(),
			&Private::Pending::document)) {
		return;
	} else if (int(p->pending.size()) >= kPendingLimit) {
		Toast(show, tr::lng_oblivion_rvideo_wait(tr::now));
		return;
	}
	const auto limit = VideoBytesLimit(room);
	if (document->size > limit) {
		Toast(show, TooBigText(limit));
		return;
	}
	const auto name = document->filename();
	auto title = QFileInfo(name).completeBaseName();
	if (title.isEmpty()) {
		// The text of the message is never sent to the cloud: a video
		// without a file name is called by the date of the message.
		const auto item = document->owner().message(origin);
		const auto date = item
			? base::unixtime::parse(item->date())
			: QDateTime::currentDateTime();
		title = tr::lng_oblivion_rvideo_default_title(
			tr::now,
			lt_date,
			QLocale().toString(date.date(), QLocale::ShortFormat));
	}
	auto entry = Private::Pending{
		.id = ++p->pendingIds,
		.title = title,
		.fileName = name.isEmpty() ? u"video.mp4"_q : name,
		.mime = document->mimeString(),
		.document = document,
		.media = document->createMediaView(),
		.origin = origin,
		.show = show,
	};
	entry.media->thumbnailWanted(origin);
	const auto id = entry.id;
	p->pending.push_back(std::move(entry));
	if (document->filepath(true).isEmpty() && !document->loading()) {
		// Into the temp folder of the room: removed when it is left.
		const auto folder = RoomFolderPath(room);
		if (!QDir().mkpath(folder)) {
			p->pending.pop_back();
			Toast(show, tr::lng_oblivion_rvideo_download_failed(tr::now));
			return;
		}
		document->save(
			origin,
			folder + u"tg_"_q + QString::number(document->id) + u".video"_q);
	}
	p->pendingChanged();
	p->checkDocument(document);
	const auto now = p->findPending(id);
	if (now && !now->probing) {
		Toast(show, tr::lng_oblivion_rvideo_downloading(tr::now));
	} else if (now) {
		Toast(show, tr::lng_oblivion_rvideo_added_toast(tr::now));
	}
}

void VideoEngine::addFiles(
		const QStringList &paths,
		std::shared_ptr<Ui::Show> show) {
	const auto p = _private.get();
	const auto room = p->room;
	if (room->state().gone != Gone::No) {
		return;
	} else if (!room->can(Right::Add)) {
		Toast(show, tr::lng_oblivion_rvideo_no_add(tr::now));
		return;
	}
	const auto limit = VideoBytesLimit(room);
	auto added = 0;
	for (const auto &path : paths) {
		const auto info = QFileInfo(path);
		if (!info.isFile() || !VideoFileName(info.fileName())) {
			Toast(show, tr::lng_oblivion_rvideo_not_video(tr::now));
			continue;
		} else if (info.size() > limit) {
			Toast(show, TooBigText(limit));
			continue;
		} else if (added >= kBatchLimit) {
			Toast(show, tr::lng_oblivion_rvideo_too_many(tr::now));
			break;
		} else if (int(p->pending.size()) >= kPendingLimit) {
			Toast(show, tr::lng_oblivion_rvideo_wait(tr::now));
			break;
		}
		p->pending.push_back({
			.id = ++p->pendingIds,
			.title = info.completeBaseName(),
			.fileName = info.fileName(),
			.show = show,
		});
		p->startProbe(p->pending.back(), info.absoluteFilePath());
		++added;
	}
	if (added) {
		p->pendingChanged();
		Toast(show, tr::lng_oblivion_rvideo_added_toast(tr::now));
	}
}

[[nodiscard]] DocumentData *VideoDocument(HistoryItem *item) {
	const auto media = item ? item->media() : nullptr;
	const auto document = media ? media->document() : nullptr;
	return (document && document->isVideoFile() && !media->ttlSeconds())
		? document
		: nullptr;
}

} // namespace

// ---- Pure logic.

VideoStep NextVideoStep(
		const PlayerState &room,
		int64 duration,
		int64 serverNow,
		crl::time lead,
		const VideoLocal &local) {
	if (room.itemId.isEmpty()) {
		return {
			.action = local.active ? VideoAction::Stop : VideoAction::None,
			.state = VideoState::Idle,
		};
	}
	const auto limit = std::max(duration, int64(0));
	const auto changed = (local.rev != room.rev);
	if (!room.playing) {
		const auto target = std::clamp(room.position, int64(0), limit);
		const auto distance = std::abs(local.position - target);
		auto result = VideoStep{
			.from = target,
			.state = VideoState::Paused,
		};
		if (!local.active) {
			result.action = VideoAction::Prepare;
		} else if (local.finished) {
			// Played to the end: only a new state of the room brings it
			// back.
			result.action = changed
				? VideoAction::Prepare
				: VideoAction::None;
		} else if (local.running || local.scheduled) {
			result.action = (distance <= kPausedTolerance)
				? VideoAction::Hold
				: VideoAction::Prepare;
		} else if (changed) {
			result.action = (distance <= kFrameTolerance)
				? VideoAction::Adopt
				: VideoAction::Prepare;
		}
		return result;
	}
	const auto plan = PlanStart(room, duration, serverNow, lead);
	if (plan.over) {
		// The last moments: the server switches, nothing is started.
		return {
			.action = (changed && local.active)
				? VideoAction::Adopt
				: VideoAction::None,
			.state = (local.running && !local.finished)
				? VideoState::Synced
				: VideoState::Waiting,
		};
	}
	const auto resumeAfter = [](int64 value) {
		return crl::time(std::max(value - int64(kResumeLead), int64(0)));
	};
	const auto prepare = VideoStep{
		.action = VideoAction::Prepare,
		.from = plan.from,
		.play = true,
		.wait = resumeAfter(int64(plan.wait) + int64(lead)),
		.state = VideoState::Starting,
	};
	const auto expected = PositionAt(room, duration, serverNow);
	if (local.active && local.running && !local.finished) {
		if (!changed) {
			return { .state = VideoState::Synced };
		} else if (std::abs(local.position - expected) <= kSeekThreshold) {
			// A change that is not a jump (the repeat mode, the same
			// place): the correction does the rest.
			return {
				.action = VideoAction::Adopt,
				.state = VideoState::Synced,
			};
		}
		return prepare;
	} else if (local.active && local.finished && !changed) {
		// The file was shorter than the room thinks: the item is over
		// here, the server switches when its time comes.
		return { .state = VideoState::Waiting };
	} else if (local.active
		&& local.scheduled
		&& !changed
		&& !local.force) {
		return { .state = VideoState::Starting };
	} else if (local.active && !local.finished) {
		// On pause somewhere: when does the room come there?
		const auto till = (serverNow < room.anchor)
			? ((room.anchor - serverNow)
				+ (local.position
					- std::clamp(room.position, int64(0), limit)))
			: (local.position - expected);
		if (local.force
			|| (till >= -kPausedTolerance && till <= kHoldMax)) {
			return {
				.action = VideoAction::Resume,
				.wait = resumeAfter(std::min(till, kHoldMax)),
				.state = VideoState::Starting,
			};
		}
	}
	return prepare;
}

crl::time VideoPrepareLead(crl::time previous, crl::time measured) {
	const auto wanted = std::max(measured, crl::time(0)) * 5 / 4 + 60;
	const auto mixed = (previous > 0) ? ((previous + wanted) / 2) : wanted;
	return std::clamp(mixed, kPrepareLeadMin, kPrepareLeadMax);
}

int64 VideoPartLength(int64 offset, int64 total) {
	if (offset < 0 || total <= 0 || offset >= total) {
		return 0;
	}
	return std::min(kPartSize, total - offset);
}

bool VideoPartReady(int64 offset, int64 total, int64 have) {
	const auto length = VideoPartLength(offset, total);
	return (length > 0) && (offset + length <= have);
}

QSize VideoFrameSize(QSize video, QSize box, QSize limit) {
	if (video.isEmpty()) {
		return QSize();
	}
	auto bound = box.isEmpty() ? limit : box;
	if (!limit.isEmpty()) {
		bound = bound.boundedTo(limit);
	}
	if (bound.isEmpty()) {
		return QSize();
	}
	const auto result = video.scaled(bound, Qt::KeepAspectRatio);
	return QSize(
		std::max(result.width(), 2),
		std::max(result.height(), 2));
}

QRect VideoFrameRect(QSize image, QRect box) {
	if (image.isEmpty() || box.isEmpty()) {
		return QRect();
	}
	const auto size = image.scaled(box.size(), Qt::KeepAspectRatio);
	return QRect(
		box.x() + (box.width() - size.width()) / 2,
		box.y() + (box.height() - size.height()) / 2,
		size.width(),
		size.height());
}

bool VideoFileName(const QString &name) {
	const auto suffix = QFileInfo(name).suffix().toLower();
	return (suffix == u"mp4"_q)
		|| (suffix == u"m4v"_q)
		|| (suffix == u"mov"_q)
		|| (suffix == u"mkv"_q)
		|| (suffix == u"webm"_q);
}

int VideoPlayNextIndex(int index, int current) {
	if (index < 0
		|| current < 0
		|| index == current
		|| index == current + 1) {
		return -1;
	}
	return (index < current) ? current : (current + 1);
}

bool VideoKeepsDisplayOn(
		VideoState state,
		bool playing,
		bool picture,
		bool shown) {
	if (!playing || !picture || !shown) {
		return false;
	}
	switch (state) {
	case VideoState::Synced:
	case VideoState::Catching:
	case VideoState::Starting:
	case VideoState::Buffering:
		return true;
	default:
		return false;
	}
}

VideoSpace PlanVideoSpace(
		std::vector<VideoFile> files,
		int64 budget,
		int64 available,
		int64 needed) {
	auto result = VideoSpace();
	auto taken = int64(0);
	for (const auto &file : files) {
		taken += std::max(file.bytes, int64(0));
	}
	std::stable_sort(
		begin(files),
		end(files),
		[](const VideoFile &a, const VideoFile &b) {
			return a.usedAt < b.usedAt;
		});
	const auto tight = [&] {
		return (available >= 0) && (available < needed);
	};
	for (const auto &file : files) {
		if (taken <= budget && !tight()) {
			break;
		} else if (file.kept || file.bytes <= 0) {
			continue;
		}
		taken -= file.bytes;
		if (available >= 0) {
			available += file.bytes;
		}
		result.evict.push_back(file.media);
	}
	result.enough = !tight();
	return result;
}

bool VideoReaderRestart(int64 read, crl::time sinceLast, int64 left) {
	return (read >= kReadLimit)
		&& (sinceLast >= kRecycleInterval)
		&& (left >= kRecycleEndMargin);
}

void AddVideoToRoomAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		HistoryItem *item,
		not_null<DocumentData*> document) {
	const auto session = &controller->session();
	const auto room = (item && Oblivion::Get().cloudRooms())
		? ActiveRoom(session)
		: nullptr;
	if (!room
		|| room->sample()
		|| !room->session()
		|| !room->can(Right::Add)
		|| (room->state().gone != Gone::No)
		|| (VideoDocument(item) != document.get())
		|| (&item->history()->session() != session)
		|| item->forbidsSaving()) {
		return;
	}
	const auto weak = base::make_weak(room);
	const auto id = item->fullId();
	const auto show = controller->uiShow();
	menu->addAction(tr::lng_oblivion_rvideo_add_to_room(tr::now), [=] {
		const auto strong = weak.get();
		const auto alive = strong ? strong->session() : nullptr;
		const auto message = alive ? alive->data().message(id) : nullptr;
		const auto video = VideoDocument(message);
		if (!video) {
			return;
		} else if (message->forbidsSaving()) {
			Toast(show, tr::lng_oblivion_rvideo_protected(tr::now));
			return;
		}
		if (const auto engine = EnsureEngine(strong)) {
			engine->addDocument(video, id, show);
		}
	}, &st::menuIconVideoChat);
}

// ---- The picture with the controls over it (the tab and the full
// screen).

namespace {

[[nodiscard]] const style::font &VideoTitleFont() {
	static const auto result = style::font(
		Scaled(15),
		st::semiboldFont->flags(),
		st::semiboldFont->family());
	return result;
}

[[nodiscard]] QColor White(int alpha = 255) {
	return QColor(255, 255, 255, alpha);
}

[[nodiscard]] uint32 SeedHash(const QString &seed) {
	auto result = uint32(2166136261U);
	for (const auto ch : seed) {
		result ^= uint32(ch.unicode());
		result *= uint32(16777619U);
	}
	return result;
}

[[nodiscard]] QColor SeedColor(const QString &seed, int index) {
	const auto hash = SeedHash(seed);
	const auto hue = int((hash + uint32(index) * 53U) % 360U);
	return QColor::fromHsv(hue, 120, index ? 70 : 130);
}

void FillSeedGradient(QPainter &p, QRect rect, const QString &seed) {
	auto gradient = QLinearGradient(rect.topLeft(), rect.bottomRight());
	gradient.setColorAt(0., SeedColor(seed, 0));
	gradient.setColorAt(1., SeedColor(seed, 1));
	p.fillRect(rect, gradient);
}

// A small picture of a queue item: its cover cropped to the frame, or
// a gradient made of the seed with a play sign (not for the item that is
// on: its own sign is painted over the picture).
void PaintThumb(
		QPainter &p,
		QRect rect,
		const QImage &cover,
		const QString &seed,
		int radius,
		bool sign) {
	auto path = QPainterPath();
	path.addRoundedRect(QRectF(rect), radius, radius);
	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	p.setClipPath(path);
	if (!cover.isNull()) {
		const auto part = QSizeF(rect.size()).scaled(
			QSizeF(cover.size()),
			Qt::KeepAspectRatio);
		p.fillRect(rect, Qt::black);
		p.drawImage(
			QRectF(rect),
			cover,
			QRectF(
				(cover.width() - part.width()) / 2.,
				(cover.height() - part.height()) / 2.,
				part.width(),
				part.height()));
	} else {
		FillSeedGradient(p, rect, seed);
		if (sign) {
			const auto side = std::min(rect.width(), rect.height()) * 0.5;
			PaintGlyph(
				p,
				Glyph::Play,
				QRectF(
					rect.x() + (rect.width() - side) / 2.,
					rect.y() + (rect.height() - side) / 2.,
					side,
					side),
				White(170));
		}
	}
	p.restore();
}

// Four corners: outwards to open the full screen, inwards to leave it.
void PaintFullscreenGlyph(
		QPainter &p,
		QRectF rect,
		const QColor &color,
		bool leave) {
	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	const auto side = std::min(rect.width(), rect.height());
	const auto left = rect.x() + (rect.width() - side) / 2.;
	const auto top = rect.y() + (rect.height() - side) / 2.;
	auto pen = QPen(color, side * 0.1);
	pen.setCapStyle(Qt::RoundCap);
	pen.setJoinStyle(Qt::RoundJoin);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	const auto corner = [&](double x, double y, double dx, double dy) {
		auto path = QPainterPath();
		path.moveTo(left + side * (x + dx), top + side * y);
		path.lineTo(left + side * x, top + side * y);
		path.lineTo(left + side * x, top + side * (y + dy));
		p.drawPath(path);
	};
	if (leave) {
		corner(0.4, 0.4, -0.2, -0.2);
		corner(0.6, 0.4, 0.2, -0.2);
		corner(0.4, 0.6, -0.2, 0.2);
		corner(0.6, 0.6, 0.2, 0.2);
	} else {
		corner(0.2, 0.2, 0.2, 0.2);
		corner(0.8, 0.2, -0.2, 0.2);
		corner(0.2, 0.8, 0.2, -0.2);
		corner(0.8, 0.8, -0.2, -0.2);
	}
	p.restore();
}

void PaintSpinner(QPainter &p, QRectF rect, const QColor &color) {
	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	auto pen = QPen(color, std::max(rect.width() * 0.09, 2.));
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	const auto angle = anim::Disabled()
		? 0
		: int((crl::now() % kSpinnerPeriod) * 360 / kSpinnerPeriod);
	p.drawArc(rect, (90 - angle) * 16, -270 * 16);
	p.restore();
}

[[nodiscard]] QString VideoStatusText(
		not_null<Room*> room,
		const VideoStatus &status) {
	const auto percent = QString::number(status.percent);
	switch (status.state) {
	case VideoState::Idle:
		return QString();
	case VideoState::Loading:
		return tr::lng_oblivion_rvideo_state_loading(
			tr::now,
			lt_percent,
			percent);
	case VideoState::Buffering:
		return tr::lng_oblivion_rvideo_state_buffering(
			tr::now,
			lt_percent,
			percent);
	case VideoState::Failed:
		return tr::lng_oblivion_rvideo_state_failed(tr::now);
	case VideoState::NoSpace:
		return tr::lng_oblivion_rvideo_state_no_space(tr::now);
	case VideoState::Unplayable:
		return tr::lng_oblivion_rvideo_state_unplayable(tr::now);
	case VideoState::Paused: {
		const auto &state = room->player(Kind::Video).state;
		const auto by = room->state().member(state.updatedBy);
		return (by && !by->name.isEmpty())
			? tr::lng_oblivion_rvideo_state_paused_by(
				tr::now,
				lt_name,
				by->name)
			: tr::lng_oblivion_rvideo_state_paused(tr::now);
	}
	case VideoState::Starting:
		return tr::lng_oblivion_rvideo_state_starting(tr::now);
	case VideoState::Synced:
		return tr::lng_oblivion_rvideo_state_synced(tr::now);
	case VideoState::Catching:
		return tr::lng_oblivion_rvideo_state_catching(tr::now);
	case VideoState::Away:
		return tr::lng_oblivion_rvideo_state_away(tr::now);
	case VideoState::Waiting:
		return tr::lng_oblivion_rvideo_state_waiting(tr::now);
	}
	return QString();
}

// What a sample room (the snapshot scenes) shows instead of an engine.
struct SampleView {
	VideoStatus status;
	QImage frame;
	std::vector<VideoEngine::PendingRow> pending;
};

[[nodiscard]] std::optional<SampleView> &NextSampleView() {
	static auto result = std::optional<SampleView>();
	return result;
}

class Surface final : public Ui::RpWidget {
public:
	Surface(
		QWidget *parent,
		not_null<Room*> room,
		base::weak_ptr<VideoEngine> engine,
		std::shared_ptr<Ui::Show> show,
		bool fullscreen,
		std::optional<SampleView> sample);

	// The picture is shown by another surface (the full screen).
	void setSuspended(bool suspended);

	[[nodiscard]] VideoStatus status() const;
	[[nodiscard]] rpl::producer<> fullscreenRequests() const {
		return _fullscreenRequests.events();
	}
	[[nodiscard]] rpl::producer<> addRequests() const {
		return _addRequests.events();
	}

protected:
	bool eventHook(QEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	enum class Control {
		None,
		Play,
		Next,
		Mute,
		Fullscreen,
		Center, // The big button in the middle.
		Action, // «Вернуться к просмотру», «Повторить».
	};

	[[nodiscard]] int unit(int value) const;
	[[nodiscard]] QRect centerRect() const;
	[[nodiscard]] QString actionText(VideoState state) const;
	[[nodiscard]] QRect actionRect(const QString &text) const;
	[[nodiscard]] bool centerShown(const VideoStatus &status) const;
	[[nodiscard]] Control hitTest(QPoint point) const;
	[[nodiscard]] bool controlsWanted() const;
	[[nodiscard]] float64 controlsOpacity() const;
	void setOverControl(Control control);
	void activate(Control control);
	void togglePlay();
	void toggleMute();
	[[nodiscard]] bool checkControl();
	void refresh();
	void refreshPosition();
	void updateKeepAwake();
	void updateControls();
	void controlsOpacityChanged();
	void updateGeometry();
	void paintPicture(
		QPainter &p,
		not_null<Room*> room,
		const QueueItem *current);
	void paintState(
		QPainter &p,
		not_null<Room*> room,
		const VideoStatus &status,
		const QueueItem *current);
	void paintControls(
		QPainter &p,
		not_null<Room*> room,
		const QueueItem *current,
		float64 opacity);

	const base::weak_ptr<Room> _room;
	const base::weak_ptr<VideoEngine> _engine;
	const std::shared_ptr<Ui::Show> _show;
	const bool _fullscreen = false;
	const std::optional<SampleView> _sample;
	const not_null<Ui::MediaSlider*> _seek;
	const not_null<Ui::MediaSlider*> _volume;
	rpl::event_stream<> _fullscreenRequests;
	rpl::event_stream<> _addRequests;
	base::Timer _tick;
	base::Timer _hideTimer;
	base::Timer _volumeSave;
	std::unique_ptr<base::PowerSaveBlocker> _keepAwake;
	Ui::Animations::Simple _controlsAnimation;
	Ui::Animations::Basic _spinner;
	QRect _playRect;
	QRect _nextRect;
	QRect _muteRect;
	QRect _fullscreenRect;
	QRect _timeRect;
	int _barHeight = 0;
	Control _overControl = Control::None;
	Control _pressed = Control::None;
	crl::time _lastMove = 0;
	float64 _seeking = -1.;
	float64 _pendingSeek = -1.;
	crl::time _pendingSeekTill = 0;
	crl::time _nonIdleAt = 0;
	int _volumeBeforeMute = 100;
	int _volumeWanted = -1; // Dragged to, not written to the settings yet.
	bool _over = false;
	bool _controlsShown = true;
	bool _suspended = false;
	bool _picture = false;

};

Surface::Surface(
	QWidget *parent,
	not_null<Room*> room,
	base::weak_ptr<VideoEngine> engine,
	std::shared_ptr<Ui::Show> show,
	bool fullscreen,
	std::optional<SampleView> sample)
: RpWidget(parent)
, _room(base::make_weak(room.get()))
, _engine(std::move(engine))
, _show(std::move(show))
, _fullscreen(fullscreen)
, _sample(std::move(sample))
, _seek(Ui::CreateChild<Ui::MediaSlider>(this, st::mediaviewPlayback))
, _volume(Ui::CreateChild<Ui::MediaSlider>(this, st::mediaviewPlayback))
, _tick([=] {
	refreshPosition();
	updateKeepAwake();
})
, _hideTimer([=] { updateControls(); })
, _volumeSave([=] {
	if (_volumeWanted >= 0) {
		Oblivion::Get().setRoomVideoVolume(std::exchange(_volumeWanted, -1));
	}
}) {
	setMouseTracking(true);
	setAttribute(Qt::WA_OpaquePaintEvent);
	if (_fullscreen) {
		setFocusPolicy(Qt::StrongFocus);
	}
	_spinner.init([=] {
		update();
	});

	_seek->setAlwaysDisplayMarker(true);
	_seek->setChangeProgressCallback([=](float64 value) {
		_seeking = value;
		update();
	});
	_seek->setChangeFinishedCallback([=](float64 value) {
		_seeking = -1.;
		const auto room = _room.get();
		const auto current = room
			? room->player(Kind::Video).current()
			: nullptr;
		if (current && checkControl()) {
			// The bar stays where it was dropped till the room answers.
			_pendingSeek = value;
			_pendingSeekTill = crl::now() + kSeekShown;
			room->seek(Kind::Video, int64(value * current->duration));
			base::call_delayed(kSeekShown + 50, this, [=] {
				refreshPosition();
			});
		} else {
			refreshPosition();
		}
	});

	_volume->setAlwaysDisplayMarker(true);
	_volume->setMoveByWheel(true);
	_volume->setValue(Oblivion::Get().roomVideoVolume() / 100.);
	const auto volumeFor = [](float64 value) {
		return int(base::SafeRound(std::clamp(value, 0., 1.) * 100.));
	};
	// Every change of the setting is a write of the settings file: while
	// the slider is dragged (or the wheel is turned) the volume follows a
	// few times a second, not with every pixel.
	_volume->setChangeProgressCallback([=](float64 value) {
		_volumeWanted = volumeFor(value);
		if (!_volumeSave.isActive()) {
			_volumeSave.callOnce(kVolumeSaveDelay);
		}
	});
	_volume->setChangeFinishedCallback([=](float64 value) {
		_volumeSave.cancel();
		_volumeWanted = -1;
		Oblivion::Get().setRoomVideoVolume(volumeFor(value));
	});

	Oblivion::Get().changes(
	) | rpl::on_next([=] {
		if (!_volume->isChanging() && _volumeWanted < 0) {
			_volume->setValue(Oblivion::Get().roomVideoVolume() / 100.);
		}
		update();
	}, lifetime());

	room->changes(
	) | rpl::on_next([=](Changes changes) {
		const auto mine = Changes(Change::VideoQueue)
			| Change::VideoPlayer
			| Change::Rights
			| Change::Covers
			| Change::Members
			| Change::Gone
			| Change::Reloaded;
		if (changes & Change::VideoPlayer) {
			_pendingSeek = -1.;
		}
		if (changes & mine) {
			refresh();
		}
	}, lifetime());

	if (const auto strong = _engine.get()) {
		strong->statusValue(
		) | rpl::on_next([=](const VideoStatus &) {
			refresh();
		}, lifetime());

		strong->frames(
		) | rpl::on_next([=] {
			if (!_suspended && isVisible()) {
				update();
			}
		}, lifetime());
	}

	refresh();
}

int Surface::unit(int value) const {
	return _fullscreen
		? int(base::SafeRound(Scaled(value) * 1.3))
		: Scaled(value);
}

void Surface::setSuspended(bool suspended) {
	if (_suspended == suspended) {
		return;
	}
	_suspended = suspended;
	refresh();
}

VideoStatus Surface::status() const {
	if (const auto engine = _engine.get()) {
		return engine->status();
	} else if (_sample) {
		return _sample->status;
	}
	const auto room = _room.get();
	if (!room) {
		return {};
	}
	const auto &data = room->player(Kind::Video);
	return {
		.state = !data.current()
			? VideoState::Idle
			: data.state.playing
			? VideoState::Synced
			: VideoState::Paused,
	};
}

QRect Surface::centerRect() const {
	const auto size = unit(64);
	return QRect(
		(width() - size) / 2,
		(height() - size) / 2,
		size,
		size);
}

QString Surface::actionText(VideoState state) const {
	switch (state) {
	case VideoState::Away:
		return tr::lng_oblivion_rvideo_rejoin(tr::now);
	case VideoState::Failed:
	case VideoState::NoSpace:
	case VideoState::Unplayable:
		return tr::lng_oblivion_rvideo_retry(tr::now);
	default:
		return QString();
	}
}

QRect Surface::actionRect(const QString &text) const {
	if (text.isEmpty()) {
		return QRect();
	}
	const auto w = st::semiboldFont->width(text) + 2 * unit(18);
	const auto h = unit(34);
	return QRect((width() - w) / 2, height() / 2 + unit(6), w, h);
}

bool Surface::centerShown(const VideoStatus &status) const {
	const auto room = _room.get();
	if (!room || _suspended) {
		return false;
	}
	const auto &data = room->player(Kind::Video);
	return (status.state == VideoState::Paused)
		|| (status.state == VideoState::Idle
			&& !data.queue.empty()
			&& room->can(Right::Control));
}

Surface::Control Surface::hitTest(QPoint point) const {
	if (_suspended) {
		return Control::None;
	}
	const auto now = status();
	const auto action = actionRect(actionText(now.state));
	if (!action.isEmpty() && action.contains(point)) {
		return Control::Action;
	} else if (controlsOpacity() > 0.) {
		if (_playRect.contains(point)) {
			return Control::Play;
		} else if (_nextRect.contains(point)) {
			return Control::Next;
		} else if (_muteRect.contains(point)) {
			return Control::Mute;
		} else if (_fullscreenRect.contains(point)) {
			return Control::Fullscreen;
		}
	}
	if (centerShown(now) && centerRect().contains(point)) {
		return Control::Center;
	}
	return Control::None;
}

bool Surface::controlsWanted() const {
	if (_suspended) {
		return false;
	} else if (_sample || _seek->isChanging() || _volume->isChanging()) {
		return true;
	}
	const auto state = status().state;
	const auto plays = (state == VideoState::Synced)
		|| (state == VideoState::Catching)
		|| (state == VideoState::Buffering)
		|| (state == VideoState::Starting);
	if (!plays) {
		return true;
	} else if (_seek->underMouse() || _volume->underMouse()) {
		return true;
	}
	return _over && (crl::now() - _lastMove < kControlsHide);
}

float64 Surface::controlsOpacity() const {
	return _controlsAnimation.value(_controlsShown ? 1. : 0.);
}

void Surface::updateControls() {
	const auto wanted = controlsWanted();
	if (wanted && (_seek->underMouse() || _volume->underMouse())) {
		// The mouse over a slider does not come here as a move.
		_hideTimer.callOnce(kControlsHide);
	}
	if (_controlsShown != wanted) {
		_controlsShown = wanted;
		_controlsAnimation.start(
			[=] { controlsOpacityChanged(); },
			wanted ? 0. : 1.,
			wanted ? 1. : 0.,
			kControlsFade);
	}
	controlsOpacityChanged();
}

void Surface::controlsOpacityChanged() {
	const auto opacity = controlsOpacity();
	const auto shown = (opacity > 0.);
	_seek->setFadeOpacity(opacity);
	_volume->setFadeOpacity(opacity);
	// With nothing on there is nothing to seek in: an empty line with
	// a stub of a marker only looks broken.
	const auto room = _room.get();
	const auto seekable = shown
		&& room
		&& (room->player(Kind::Video).current() != nullptr);
	if (_seek->isHidden() == seekable) {
		_seek->setVisible(seekable);
	}
	if (_volume->isHidden() == shown) {
		_volume->setVisible(shown);
	}
	if (_fullscreen) {
		setCursor((shown || !_over)
			? ((_overControl != Control::None)
				? style::cur_pointer
				: style::cur_default)
			: Qt::BlankCursor);
	}
	update();
}

void Surface::setOverControl(Control control) {
	if (_overControl != control) {
		_overControl = control;
		setCursor((control != Control::None)
			? style::cur_pointer
			: style::cur_default);
		update();
	}
}

bool Surface::checkControl() {
	const auto room = _room.get();
	if (!room) {
		return false;
	} else if (room->can(Right::Control)) {
		return true;
	}
	Toast(_show, tr::lng_oblivion_rmusic_no_control(tr::now));
	return false;
}

void Surface::togglePlay() {
	const auto room = _room.get();
	if (!room || room->sample()) {
		return;
	}
	const auto &data = room->player(Kind::Video);
	if (data.queue.empty()) {
		_addRequests.fire({});
	} else if (!checkControl()) {
		return;
	} else if (!data.current()) {
		room->select(Kind::Video, data.queue.front().id);
	} else if (data.state.playing) {
		room->pause(Kind::Video);
	} else {
		room->play(Kind::Video);
	}
}

void Surface::toggleMute() {
	_volumeSave.cancel();
	_volumeWanted = -1;
	const auto now = Oblivion::Get().roomVideoVolume();
	if (now > 0) {
		_volumeBeforeMute = now;
		Oblivion::Get().setRoomVideoVolume(0);
	} else {
		Oblivion::Get().setRoomVideoVolume(
			(_volumeBeforeMute > 0) ? _volumeBeforeMute : 100);
	}
}

void Surface::activate(Control control) {
	const auto room = _room.get();
	if (!room) {
		return;
	}
	switch (control) {
	case Control::None:
		break;
	case Control::Play:
	case Control::Center:
		togglePlay();
		break;
	case Control::Next:
		if (!room->sample() && checkControl()) {
			room->next(Kind::Video);
		}
		break;
	case Control::Mute:
		toggleMute();
		break;
	case Control::Fullscreen:
		_fullscreenRequests.fire({});
		break;
	case Control::Action:
		if (const auto engine = _engine.get()) {
			if (engine->status().state == VideoState::Away) {
				engine->rejoin();
			} else {
				engine->retry();
			}
		}
		break;
	}
}

// The display of the device stays on while a picture that moves is really
// seen here, as it does for the media viewer of the app. The clock of the
// auto-lock of the app is moved only from the window the user is in: the
// main window is not the active one then, so nothing is told to Telegram
// (neither "online" nor a read message) because a video plays in a room.
void Surface::updateKeepAwake() {
	const auto room = _room.get();
	const auto top = window();
	const auto block = (room != nullptr)
		&& (top != nullptr)
		&& !_sample
		&& !_suspended
		&& (_engine.get() != nullptr)
		&& !top->isMinimized()
		&& VideoKeepsDisplayOn(
			status().state,
			room->player(Kind::Video).state.playing,
			_picture,
			isVisible());
	base::UpdatePowerSaveBlocker(
		_keepAwake,
		block,
		base::PowerSaveBlockType::PreventDisplaySleep,
		[] { return u"Video playback is active"_q; },
		[top] { return top->windowHandle(); });
	if (!block
		|| !top->isActiveWindow()
		|| !Core::IsAppLaunched()
		|| Core::App().passcodeLocked()) {
		return;
	}
	const auto now = crl::now();
	if (now - _nonIdleAt >= kNonIdlePeriod) {
		_nonIdleAt = now;
		Core::App().updateNonIdle();
	}
}

bool Surface::eventHook(QEvent *e) {
	const auto type = e->type();
	const auto result = RpWidget::eventHook(e);
	if (type == QEvent::Show || type == QEvent::Hide) {
		// Another tab of the room, a minimized or a hidden window.
		updateKeepAwake();
	}
	return result;
}

void Surface::refresh() {
	const auto room = _room.get();
	if (!room) {
		updateKeepAwake();
		return;
	}
	const auto &data = room->player(Kind::Video);
	const auto state = status().state;
	_seek->setDisabled(!room->can(Right::Control) || !data.current());
	const auto ticking = !_sample
		&& !_suspended
		&& data.state.playing
		&& data.current();
	if (!ticking) {
		_tick.cancel();
	} else if (!_tick.isActive()) {
		_tick.callEach(kPositionTick);
	}
	const auto spinning = !_sample
		&& !_suspended
		&& !anim::Disabled()
		&& ((state == VideoState::Loading)
			|| (state == VideoState::Buffering)
			|| (state == VideoState::Starting)
			|| (state == VideoState::Waiting));
	if (spinning && !_spinner.animating()) {
		_spinner.start();
	} else if (!spinning && _spinner.animating()) {
		_spinner.stop();
	}
	refreshPosition();
	updateControls();
	updateKeepAwake();
	update();
}

void Surface::refreshPosition() {
	const auto room = _room.get();
	if (!room) {
		return;
	}
	const auto current = room->player(Kind::Video).current();
	if (_pendingSeek >= 0. && crl::now() >= _pendingSeekTill) {
		_pendingSeek = -1.;
	}
	if (!_seek->isChanging()) {
		_seek->setValue((_pendingSeek >= 0.)
			? _pendingSeek
			: (current && current->duration > 0)
			? std::clamp(
				room->position(Kind::Video) / float64(current->duration),
				0.,
				1.)
			: 0.);
	}
	if (controlsOpacity() > 0.) {
		update(_timeRect);
	}
}

void Surface::updateGeometry() {
	const auto w = width();
	const auto h = height();
	if (w <= 0 || h <= 0) {
		return;
	}
	const auto pad = unit(12);
	const auto button = unit(30);
	const auto seekHeight = st::mediaviewPlayback.seekSize.height();
	const auto rowTop = h - unit(8) - button;
	const auto seekTop = rowTop - unit(2) - seekHeight;
	_seek->setGeometry(pad, seekTop, std::max(w - 2 * pad, 0), seekHeight);
	_playRect = QRect(pad - unit(4), rowTop, button, button);
	_nextRect = QRect(_playRect.x() + button + unit(2), rowTop, button, button);
	_fullscreenRect = QRect(w - pad + unit(4) - button, rowTop, button, button);
	const auto volumeWidth = unit((w < Scaled(420)) ? 48 : 72);
	const auto volumeLeft = _fullscreenRect.x() - unit(8) - volumeWidth;
	_volume->setGeometry(
		volumeLeft,
		rowTop + (button - seekHeight) / 2,
		volumeWidth,
		seekHeight);
	_muteRect = QRect(volumeLeft - unit(4) - button, rowTop, button, button);
	const auto timeLeft = _nextRect.x() + button + unit(6);
	_timeRect = QRect(
		timeLeft,
		rowTop,
		std::max(_muteRect.x() - unit(6) - timeLeft, 0),
		button);
	_barHeight = (h - seekTop) + unit(28);
}

void Surface::resizeEvent(QResizeEvent *e) {
	updateGeometry();
}

void Surface::mouseMoveEvent(QMouseEvent *e) {
	_over = true;
	_lastMove = crl::now();
	setOverControl(hitTest(e->pos()));
	updateControls();
	_hideTimer.callOnce(kControlsHide + 50);
}

void Surface::leaveEventHook(QEvent *e) {
	_over = false;
	setOverControl(Control::None);
	updateControls();
}

void Surface::mousePressEvent(QMouseEvent *e) {
	// A click is the same sign of life as a move.
	_over = true;
	_lastMove = crl::now();
	_hideTimer.callOnce(kControlsHide + 50);
	if (e->button() == Qt::LeftButton) {
		_pressed = hitTest(e->pos());
	}
	updateControls();
}

void Surface::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto pressed = base::take(_pressed);
	const auto control = hitTest(e->pos());
	if (control == pressed && control != Control::None) {
		activate(control);
	}
}

void Surface::mouseDoubleClickEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	} else if (hitTest(e->pos()) == Control::None) {
		_fullscreenRequests.fire({});
	} else {
		// A fast second click on a button is one more click.
		_pressed = hitTest(e->pos());
	}
}

void Surface::keyPressEvent(QKeyEvent *e) {
	if (!_fullscreen) {
		RpWidget::keyPressEvent(e);
	} else if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_F) {
		_fullscreenRequests.fire({});
	} else if (e->key() == Qt::Key_Space) {
		togglePlay();
	} else if (e->key() == Qt::Key_M) {
		toggleMute();
	} else {
		RpWidget::keyPressEvent(e);
	}
}

void Surface::paintPicture(
		QPainter &p,
		not_null<Room*> room,
		const QueueItem *current) {
	_picture = false;
	const auto full = rect();
	if (!current) {
		auto gradient = QLinearGradient(full.topLeft(), full.bottomRight());
		gradient.setColorAt(0., QColor(34, 38, 52));
		gradient.setColorAt(1., QColor(12, 13, 20));
		p.fillRect(full, gradient);
		return;
	}
	const auto ratio = style::DevicePixelRatio();
	const auto engine = _engine.get();
	const auto image = engine
		? engine->frame(full.size() * ratio)
		: _sample
		? _sample->frame
		: QImage();
	const auto draw = [&](const QImage &what) {
		const auto target = VideoFrameRect(what.size(), full);
		if (target.isEmpty()) {
			return false;
		}
		const auto exact = (what.size() == target.size() * ratio);
		const auto pixels = int64(target.width())
			* target.height()
			* ratio
			* ratio;
		p.setRenderHint(
			QPainter::SmoothPixmapTransform,
			!exact && (pixels <= kSmoothLimit));
		p.drawImage(target, what);
		p.setRenderHint(QPainter::SmoothPixmapTransform, false);
		return true;
	};
	if (!image.isNull() && draw(image)) {
		_picture = true;
		return;
	}
	const auto cover = current->cover.isEmpty()
		? QImage()
		: room->cover(current->cover);
	if (cover.isNull() || !draw(cover)) {
		FillSeedGradient(p, full, current->media);
	}
}

void Surface::paintState(
		QPainter &p,
		not_null<Room*> room,
		const VideoStatus &status,
		const QueueItem *current) {
	const auto full = rect();
	const auto &data = room->player(Kind::Video);
	const auto dim = [&](int alpha) {
		p.fillRect(full, QColor(0, 0, 0, alpha));
	};
	const auto caption = [&](const QString &text, int top) {
		p.setFont(st::normalFont);
		p.setPen(White(230));
		p.drawText(
			QRect(unit(16), top, width() - 2 * unit(16), st::normalFont->height),
			Qt::AlignHCenter | Qt::AlignTop,
			st::normalFont->elided(text, width() - 2 * unit(16)));
	};
	const auto spinner = [&](bool withText) {
		const auto size = unit(34);
		const auto top = height() / 2 - size / 2 - (withText ? unit(12) : 0);
		PaintSpinner(
			p,
			QRectF((width() - size) / 2., top, size, size),
			White(235));
		if (withText) {
			caption(VideoStatusText(room, status), top + size + unit(10));
		}
	};
	const auto pill = [&] {
		const auto text = actionText(status.state);
		const auto rect = actionRect(text);
		if (rect.isEmpty()) {
			return;
		}
		caption(
			VideoStatusText(room, status),
			rect.y() - unit(12) - st::normalFont->height);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(White((_overControl == Control::Action) ? 255 : 232));
		p.drawRoundedRect(rect, rect.height() / 2., rect.height() / 2.);
		p.setFont(st::semiboldFont);
		p.setPen(QColor(24, 26, 32));
		p.drawText(rect, Qt::AlignCenter, text);
	};
	switch (status.state) {
	case VideoState::Idle: {
		if (current) {
			break;
		}
		const auto queued = !data.queue.empty();
		const auto size = unit(64);
		const auto circle = QRectF(
			(width() - size) / 2.,
			(height() - size) / 2. - (queued ? 0 : unit(14)),
			size,
			size);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(White((_overControl == Control::Center) ? 56 : 34));
			p.drawEllipse(circle);
		}
		PaintGlyph(
			p,
			Glyph::Play,
			circle.marginsRemoved(QMarginsF(
				size * 0.27,
				size * 0.27,
				size * 0.27,
				size * 0.27)),
			White(queued && room->can(Right::Control) ? 235 : 150));
		if (!queued) {
			p.setFont(VideoTitleFont());
			p.setPen(White(210));
			p.drawText(
				QRect(
					0,
					int(circle.y() + size) + unit(12),
					width(),
					VideoTitleFont()->height),
				Qt::AlignHCenter | Qt::AlignTop,
				tr::lng_oblivion_rvideo_placeholder(tr::now));
		}
	} break;
	case VideoState::Loading:
		dim(_picture ? 90 : 120);
		spinner(true);
		break;
	case VideoState::Buffering:
		dim(60);
		spinner(true);
		break;
	case VideoState::Waiting:
		dim(90);
		spinner(true);
		break;
	case VideoState::Starting:
		if (!_picture) {
			spinner(false);
		}
		break;
	case VideoState::Failed:
	case VideoState::NoSpace:
	case VideoState::Unplayable:
	case VideoState::Away:
		dim(150);
		pill();
		break;
	case VideoState::Paused: {
		dim(70);
		const auto circle = QRectF(centerRect());
		const auto allowed = room->can(Right::Control);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(
				0,
				0,
				0,
				(_overControl == Control::Center) ? 190 : 150));
			p.drawEllipse(circle);
		}
		const auto skip = circle.width() * 0.27;
		PaintGlyph(
			p,
			allowed ? Glyph::Play : Glyph::Pause,
			circle.marginsRemoved(QMarginsF(skip, skip, skip, skip)),
			White(allowed ? 245 : 170));
	} break;
	case VideoState::Synced:
	case VideoState::Catching:
		break;
	}
}

void Surface::paintControls(
		QPainter &p,
		not_null<Room*> room,
		const QueueItem *current,
		float64 opacity) {
	const auto &data = room->player(Kind::Video);
	const auto allowed = room->can(Right::Control);
	p.setOpacity(opacity);
	{
		const auto bar = QRect(0, height() - _barHeight, width(), _barHeight);
		auto gradient = QLinearGradient(bar.topLeft(), bar.bottomLeft());
		gradient.setColorAt(0., QColor(0, 0, 0, 0));
		gradient.setColorAt(1., QColor(0, 0, 0, 200));
		p.fillRect(bar, gradient);
	}
	if (_fullscreen && current) {
		const auto top = QRect(0, 0, width(), unit(72));
		auto gradient = QLinearGradient(top.topLeft(), top.bottomLeft());
		gradient.setColorAt(0., QColor(0, 0, 0, 170));
		gradient.setColorAt(1., QColor(0, 0, 0, 0));
		p.fillRect(top, gradient);
		p.setFont(VideoTitleFont());
		p.setPen(White(240));
		p.drawText(
			unit(20),
			unit(18) + VideoTitleFont()->ascent,
			VideoTitleFont()->elided(current->title, width() - 2 * unit(20)));
	}
	const auto glyphRect = [&](QRect rect) {
		const auto skip = unit(6);
		return QRectF(rect).marginsRemoved(
			QMarginsF(skip, skip, skip, skip));
	};
	const auto hover = [&](Control control, QRect rect) {
		if (_overControl == control) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(White(46));
			p.drawEllipse(rect);
		}
	};
	// With an empty queue the button offers to add a video (togglePlay()):
	// it is bright for those who may add, not for everybody.
	const auto playable = data.queue.empty()
		? room->can(Right::Add)
		: allowed;
	hover(Control::Play, _playRect);
	PaintGlyph(
		p,
		(data.state.playing && current) ? Glyph::Pause : Glyph::Play,
		glyphRect(_playRect),
		White(playable ? 245 : 120));
	hover(Control::Next, _nextRect);
	PaintGlyph(
		p,
		Glyph::Next,
		glyphRect(_nextRect),
		White((allowed && data.queue.size() > 1) ? 245 : 120));
	hover(Control::Mute, _muteRect);
	PaintGlyph(
		p,
		Oblivion::Get().roomVideoVolume() ? Glyph::Volume : Glyph::Mute,
		glyphRect(_muteRect),
		White(245));
	hover(Control::Fullscreen, _fullscreenRect);
	PaintFullscreenGlyph(
		p,
		glyphRect(_fullscreenRect),
		White(245),
		_fullscreen);

	if (current && _timeRect.width() > 0) {
		const auto duration = current->duration;
		const auto position = (_seeking >= 0.)
			? int64(_seeking * duration)
			: (_pendingSeek >= 0.)
			? int64(_pendingSeek * duration)
			: room->position(Kind::Video);
		const auto full = FormatDuration(position)
			+ u" / "_q
			+ FormatDuration(duration);
		const auto text = (st::normalFont->width(full) <= _timeRect.width())
			? full
			: FormatDuration(position);
		if (st::normalFont->width(text) <= _timeRect.width()) {
			p.setFont(st::normalFont);
			p.setPen(White(235));
			p.drawText(
				_timeRect.x(),
				_timeRect.y()
					+ (_timeRect.height() - st::normalFont->height) / 2
					+ st::normalFont->ascent,
				text);
		}
	}
	p.setOpacity(1.);
}

void Surface::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(e->rect(), Qt::black);
	const auto room = _room.get();
	if (!room) {
		return;
	} else if (_suspended) {
		p.setFont(st::normalFont);
		p.setPen(White(150));
		p.drawText(
			rect(),
			Qt::AlignCenter,
			tr::lng_oblivion_rvideo_in_fullscreen(tr::now));
		return;
	}
	const auto now = status();
	const auto current = room->player(Kind::Video).current();
	paintPicture(p, room, current);
	paintState(p, room, now, current);
	const auto opacity = controlsOpacity();
	if (opacity > 0.) {
		paintControls(p, room, current, opacity);
	}
}

// ---- The full screen: a window of its own with the same surface.

class FullscreenWindow final : public Ui::RpWidget {
public:
	FullscreenWindow(
		not_null<Room*> room,
		base::weak_ptr<VideoEngine> engine,
		std::shared_ptr<Ui::Show> show);

	[[nodiscard]] rpl::producer<> closeRequests() const {
		return _closeRequests.events();
	}

protected:
	bool eventHook(QEvent *e) override;

private:
	const not_null<Surface*> _surface;
	rpl::event_stream<> _closeRequests;

};

FullscreenWindow::FullscreenWindow(
	not_null<Room*> room,
	base::weak_ptr<VideoEngine> engine,
	std::shared_ptr<Ui::Show> show)
: RpWidget(nullptr)
, _surface(Ui::CreateChild<Surface>(
	this,
	room,
	std::move(engine),
	std::move(show),
	true,
	std::optional<SampleView>())) {
	setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setFocusProxy(_surface);

	sizeValue(
	) | rpl::on_next([=](QSize size) {
		_surface->setGeometry(QRect(QPoint(), size));
	}, lifetime());

	paintRequest(
	) | rpl::on_next([=](QRect clip) {
		QPainter(this).fillRect(clip, Qt::black);
	}, lifetime());

	_surface->fullscreenRequests(
	) | rpl::start_to_stream(_closeRequests, lifetime());
	_surface->show();
}

bool FullscreenWindow::eventHook(QEvent *e) {
	if (e->type() == QEvent::Close) {
		// Closed by the owner, from the event loop.
		e->ignore();
		_closeRequests.fire({});
		return true;
	}
	return RpWidget::eventHook(e);
}

// ---- The queue.

class VideoQueueList final : public Ui::RpWidget {
public:
	VideoQueueList(
		QWidget *parent,
		not_null<Room*> room,
		base::weak_ptr<VideoEngine> engine,
		std::vector<VideoEngine::PendingRow> samplePending);

	void refresh();
	void setPlaying(bool playing);
	[[nodiscard]] bool empty() const;

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
	// What is on its way to the queue: a download from Telegram, a file
	// being looked into, an upload to the relay.
	struct Progress {
		int id = 0;
		bool upload = false;
		QString title;
		QString text;
		int percent = -1;
	};
	struct Hit {
		int progress = -1;
		int row = -1;
		bool control = false; // The cross of a progress, the dots of a row.

		friend inline bool operator==(const Hit &, const Hit &) = default;
	};

	[[nodiscard]] std::vector<Progress> progresses() const;
	[[nodiscard]] int progressHeight() const;
	[[nodiscard]] int rowHeight() const;
	[[nodiscard]] Hit hitTest(QPoint point) const;
	void setOver(Hit hit);

	const not_null<Room*> _room;
	const base::weak_ptr<VideoEngine> _engine;
	const std::vector<VideoEngine::PendingRow> _samplePending;
	rpl::event_stream<QString> _activations;
	rpl::event_stream<QString> _menuRequests;
	Hit _over;
	Hit _pressed;
	bool _playing = false;

};

VideoQueueList::VideoQueueList(
	QWidget *parent,
	not_null<Room*> room,
	base::weak_ptr<VideoEngine> engine,
	std::vector<VideoEngine::PendingRow> samplePending)
: RpWidget(parent)
, _room(room)
, _engine(std::move(engine))
, _samplePending(std::move(samplePending)) {
	setMouseTracking(true);
}

std::vector<VideoQueueList::Progress> VideoQueueList::progresses() const {
	auto result = std::vector<Progress>();
	const auto engine = _engine.get();
	const auto pending = engine ? engine->pending() : _samplePending;
	for (const auto &row : pending) {
		result.push_back({
			.id = row.id,
			.title = row.title,
			.text = (row.percent < 0)
				? tr::lng_oblivion_rmusic_preparing(tr::now)
				: tr::lng_oblivion_rvideo_download_row(
					tr::now,
					lt_percent,
					QString::number(row.percent)),
			.percent = row.percent,
		});
	}
	for (const auto &upload : VideoUploads(_room)) {
		const auto percent = (upload.total > 0)
			? int(std::clamp(
				upload.ready * 100 / upload.total,
				int64(0),
				int64(100)))
			: -1;
		result.push_back({
			.id = upload.id,
			.upload = true,
			.title = upload.title,
			.text = (percent < 0)
				? tr::lng_oblivion_rmusic_preparing(tr::now)
				: tr::lng_oblivion_rmusic_uploading(
					tr::now,
					lt_percent,
					QString::number(percent)),
			.percent = percent,
		});
	}
	return result;
}

int VideoQueueList::progressHeight() const {
	// The same as a row of the queue: see paintEvent().
	return Scaled(56);
}

int VideoQueueList::rowHeight() const {
	return Scaled(56);
}

bool VideoQueueList::empty() const {
	return _room->player(Kind::Video).queue.empty() && progresses().empty();
}

void VideoQueueList::refresh() {
	resizeToWidth(width());
	update();
}

void VideoQueueList::setPlaying(bool playing) {
	if (_playing != playing) {
		_playing = playing;
		update();
	}
}

int VideoQueueList::resizeGetHeight(int newWidth) {
	const auto above = int(progresses().size());
	const auto rows = int(_room->player(Kind::Video).queue.size());
	return above * progressHeight() + rows * rowHeight() + Scaled(8);
}

VideoQueueList::Hit VideoQueueList::hitTest(QPoint point) const {
	auto result = Hit();
	const auto above = int(progresses().size());
	const auto rows = int(_room->player(Kind::Video).queue.size());
	const auto controlLeft = width() - Scaled(52);
	if (point.y() < 0 || point.x() < 0 || point.x() >= width()) {
		return result;
	} else if (point.y() < above * progressHeight()) {
		result.progress = point.y() / progressHeight();
		result.control = (point.x() >= controlLeft);
		return result;
	}
	const auto index = (point.y() - above * progressHeight()) / rowHeight();
	if (index < rows) {
		result.row = index;
		result.control = (point.x() >= controlLeft);
	}
	return result;
}

void VideoQueueList::setOver(Hit hit) {
	if (_over != hit) {
		_over = hit;
		setCursor((hit.row >= 0 || (hit.progress >= 0 && hit.control))
			? style::cur_pointer
			: style::cur_default);
		update();
	}
}

void VideoQueueList::mouseMoveEvent(QMouseEvent *e) {
	setOver(hitTest(e->pos()));
}

void VideoQueueList::leaveEventHook(QEvent *e) {
	setOver({});
}

void VideoQueueList::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = hitTest(e->pos());
	}
}

void VideoQueueList::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto hit = hitTest(e->pos());
	const auto pressed = base::take(_pressed);
	if (hit != pressed) {
		return;
	}
	const auto above = progresses();
	const auto &queue = _room->player(Kind::Video).queue;
	if (hit.progress >= 0 && hit.progress < int(above.size())) {
		if (!hit.control) {
			return;
		}
		const auto &row = above[hit.progress];
		if (row.upload) {
			_room->cancelUpload(row.id);
		} else if (const auto engine = _engine.get()) {
			engine->cancelPending(row.id);
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

void VideoQueueList::contextMenuEvent(QContextMenuEvent *e) {
	const auto hit = hitTest(e->pos());
	const auto &queue = _room->player(Kind::Video).queue;
	if (hit.row >= 0 && hit.row < int(queue.size())) {
		_menuRequests.fire_copy(queue[hit.row].id);
	}
}

void VideoQueueList::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto clip = e->rect();
	const auto above = progresses();
	const auto &data = _room->player(Kind::Video);
	const auto left = Scaled(20);
	const auto right = Scaled(16);
	const auto thumbHeight = Scaled(38);
	const auto thumbWidth = thumbHeight * 16 / 9;
	auto top = 0;

	for (auto i = 0, count = int(above.size()); i != count; ++i) {
		const auto &row = above[i];
		const auto height = progressHeight();
		const auto rect = QRect(0, top, width(), height);
		top += height;
		if (!rect.intersects(clip)) {
			continue;
		}
		// Laid out as the row of the queue it is going to become: a frame
		// in place of the picture, with a ring that fills up.
		const auto thumb = QRect(
			left,
			rect.y() + (height - thumbHeight) / 2,
			thumbWidth,
			thumbHeight);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(thumb, Scaled(6), Scaled(6));
		const auto ring = Scaled(18);
		const auto circle = QRectF(
			thumb.x() + (thumb.width() - ring) / 2.,
			thumb.y() + (thumb.height() - ring) / 2.,
			ring,
			ring);
		auto ringPen = QPen(st::windowBgRipple->c, Scaled(2));
		ringPen.setCapStyle(Qt::RoundCap);
		p.setBrush(Qt::NoBrush);
		p.setPen(ringPen);
		p.drawEllipse(circle);
		if (row.percent > 0) {
			ringPen.setColor(st::windowBgActive->c);
			p.setPen(ringPen);
			p.drawArc(
				circle,
				90 * 16,
				-std::min(row.percent, 100) * 360 * 16 / 100);
		}
		const auto textLeft = thumb.x() + thumbWidth + Scaled(12);
		const auto textWidth = std::max(
			width() - right - Scaled(40) - textLeft,
			Scaled(40));
		p.setFont(st::semiboldFont);
		p.setPen(st::windowFg);
		p.drawText(
			textLeft,
			rect.y() + Scaled(10) + st::semiboldFont->ascent,
			st::semiboldFont->elided(row.title, textWidth));
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			textLeft,
			rect.y() + Scaled(30) + st::normalFont->ascent,
			st::normalFont->elided(row.text, textWidth));
		const auto over = (_over.progress == i) && _over.control;
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
		const auto thumb = QRect(
			left,
			rect.y() + (height - thumbHeight) / 2,
			thumbWidth,
			thumbHeight);
		PaintThumb(
			p,
			thumb,
			item.cover.isEmpty() ? QImage() : _room->cover(item.cover),
			item.media,
			Scaled(6),
			!current);
		if (current) {
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0, 0, 0, 120));
			p.drawRoundedRect(thumb, Scaled(6), Scaled(6));
			const auto side = Scaled(20);
			PaintGlyph(
				p,
				_playing ? Glyph::Volume : Glyph::Pause,
				QRectF(
					thumb.x() + (thumb.width() - side) / 2.,
					thumb.y() + (thumb.height() - side) / 2.,
					side,
					side),
				Qt::white);
		}
		const auto duration = FormatDuration(item.duration);
		const auto durationWidth = st::normalFont->width(duration);
		const auto dots = over;
		const auto textLeft = thumb.x() + thumbWidth + Scaled(12);
		const auto textRight = width()
			- right
			- durationWidth
			- Scaled(dots ? 40 : 8);
		const auto textWidth = std::max(textRight - textLeft, Scaled(40));
		p.setFont(st::semiboldFont);
		p.setPen(current ? st::windowActiveTextFg : st::windowFg);
		p.drawText(
			textLeft,
			rect.y() + Scaled(10) + st::semiboldFont->ascent,
			st::semiboldFont->elided(item.title, textWidth));
		const auto adder = _room->state().member(item.addedBy);
		const auto sub = (adder && !adder->name.isEmpty())
			? tr::lng_oblivion_rmusic_added_by(
				tr::now,
				lt_name,
				adder->name)
			: item.fileName;
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			textLeft,
			rect.y() + Scaled(30) + st::normalFont->ascent,
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

// ---- The tab.

[[nodiscard]] QStringList DroppedVideos(const QMimeData *data) {
	auto result = QStringList();
	if (!data || !data->hasUrls()) {
		return result;
	}
	for (const auto &url : data->urls()) {
		if (!url.isLocalFile()) {
			return QStringList();
		}
		const auto path = Platform::File::UrlToLocal(url);
		if (!VideoFileName(path)) {
			return QStringList();
		}
		result.push_back(path);
	}
	return result;
}

class VideoTab final : public Ui::RpWidget {
public:
	VideoTab(QWidget *parent, TabContext context);
	~VideoTab();

protected:
	bool eventHook(QEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void dragEnterEvent(QDragEnterEvent *e) override;
	void dragLeaveEvent(QDragLeaveEvent *e) override;
	void dropEvent(QDropEvent *e) override;

private:
	[[nodiscard]] static std::optional<SampleView> TakeSample(
		not_null<Room*> room);
	[[nodiscard]] VideoEngine *engine() const;
	[[nodiscard]] QString othersLoading() const;
	void refreshAll();
	void updateLayout();
	void showAddMenu();
	void showRowMenu(const QString &itemId);
	void addFiles();
	void toggleFullscreen();
	void closeFullscreen();
	void setDragOver(bool over);

	const not_null<Room*> _room;
	const std::shared_ptr<Ui::Show> _show;
	const base::weak_ptr<VideoEngine> _engine;
	const std::optional<SampleView> _sample;
	const not_null<Surface*> _surface;
	const not_null<Ui::RoundButton*> _add;
	const not_null<Ui::ScrollArea*> _scroll;
	const not_null<Ui::RpWidget*> _drop;
	QPointer<VideoQueueList> _list;
	base::unique_qptr<Ui::PopupMenu> _menu;
	base::unique_qptr<FullscreenWindow> _fullscreen;
	QRect _info;
	QRect _queueHeader;

};

std::optional<SampleView> VideoTab::TakeSample(not_null<Room*> room) {
	return room->sample()
		? base::take(NextSampleView())
		: std::optional<SampleView>();
}

VideoTab::VideoTab(QWidget *parent, TabContext context)
: RpWidget(parent)
, _room(context.room)
, _show(context.show)
, _engine(base::make_weak(EnsureEngine(context.room)))
, _sample(TakeSample(context.room))
, _surface(Ui::CreateChild<Surface>(
	this,
	_room,
	_engine,
	_show,
	false,
	_sample))
, _add(Ui::CreateChild<Ui::RoundButton>(
	this,
	tr::lng_oblivion_rmusic_add(),
	st::defaultActiveButton))
, _scroll(Ui::CreateChild<Ui::ScrollArea>(this, st::boxScroll))
, _drop(Ui::CreateChild<Ui::RpWidget>(this)) {
	_list = _scroll->setOwnedWidget(object_ptr<VideoQueueList>(
		this,
		_room,
		_engine,
		_sample ? _sample->pending : std::vector<VideoEngine::PendingRow>()));
	_add->setFullRadius(true);
	setAcceptDrops(true);

	_drop->setAttribute(Qt::WA_TransparentForMouseEvents);
	_drop->hide();
	_drop->paintRequest(
	) | rpl::on_next([=] {
		auto p = QPainter(_drop);
		auto hq = PainterHighQualityEnabler(p);
		auto back = st::windowBg->c;
		back.setAlpha(235);
		p.fillRect(_drop->rect(), back);
		const auto skip = Scaled(14);
		auto pen = QPen(st::windowBgActive->c, Scaled(2));
		pen.setStyle(Qt::DashLine);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(
			QRectF(_drop->rect()).marginsRemoved(
				QMarginsF(skip, skip, skip, skip)),
			Scaled(14),
			Scaled(14));
		p.setFont(st::semiboldFont);
		p.setPen(st::windowActiveTextFg);
		p.drawText(
			_drop->rect().marginsRemoved(
				QMargins(2 * skip, 2 * skip, 2 * skip, 2 * skip)),
			Qt::AlignCenter | Qt::TextWordWrap,
			tr::lng_oblivion_rvideo_drop(tr::now));
	}, _drop->lifetime());

	_add->setClickedCallback([=] { showAddMenu(); });
	_surface->addRequests(
	) | rpl::on_next([=] {
		showAddMenu();
	}, lifetime());
	_surface->fullscreenRequests(
	) | rpl::on_next([=] {
		toggleFullscreen();
	}, lifetime());

	_list->activations(
	) | rpl::on_next([=](const QString &itemId) {
		if (_room->can(Right::Control)) {
			_room->select(Kind::Video, itemId);
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
		const auto mine = Changes(Change::VideoQueue)
			| Change::VideoPlayer
			| Change::Rights
			| Change::Uploads
			| Change::Covers
			| Change::Members
			| Change::Status
			| Change::Presence
			| Change::Gone
			| Change::Reloaded;
		if (changes & Change::Gone) {
			closeFullscreen();
		}
		if (changes & mine) {
			refreshAll();
		}
	}, lifetime());

	if (const auto strong = engine()) {
		// The tab was opened: from now on the video of the room is
		// downloaded and played on this device.
		strong->enablePlayback();
		strong->statusValue(
		) | rpl::on_next([=](const VideoStatus &) {
			refreshAll();
		}, lifetime());
		strong->pendingChanges(
		) | rpl::on_next([=] {
			refreshAll();
		}, lifetime());
	}

	if (Core::IsAppLaunched()) {
		// Nothing of the room stays over the lock screen of the app.
		Core::App().passcodeLockValue(
		) | rpl::filter([](bool locked) {
			return locked;
		}) | rpl::on_next([=](bool) {
			closeFullscreen();
		}, lifetime());
	}

	_add->widthValue(
	) | rpl::on_next([=] {
		updateLayout();
	}, lifetime());

	refreshAll();
}

VideoTab::~VideoTab() {
	_menu = nullptr;
	if (_fullscreen) {
		_fullscreen->hide();
		_fullscreen = nullptr;
	}
}

VideoEngine *VideoTab::engine() const {
	return _engine.get();
}

// «Ещё загружают: Лера 40%»: who can't see the current item yet. The
// percent is written the way every other one in the room is.
QString VideoTab::othersLoading() const {
	const auto current = _room->player(Kind::Video).current();
	if (!current) {
		return QString();
	}
	auto names = QStringList();
	auto more = false;
	for (const auto &member : _room->state().members) {
		if (member.id == _room->selfId()
			|| !member.online
			|| member.video.itemId != current->id
			|| member.video.ready
			|| member.name.isEmpty()) {
			continue;
		} else if (names.size() >= 2) {
			more = true;
			break;
		}
		names.push_back(
			member.name
			+ ' '
			+ QString::number(member.video.buffered)
			+ u"%"_q);
	}
	if (names.isEmpty()) {
		return QString();
	}
	return tr::lng_oblivion_rvideo_others_loading(
		tr::now,
		lt_names,
		names.join(u", "_q) + (more ? u"…"_q : QString()));
}

void VideoTab::refreshAll() {
	const auto &data = _room->player(Kind::Video);
	if (_list) {
		_list->refresh();
		const auto state = _surface->status().state;
		_list->setPlaying(data.state.playing
			&& (state != VideoState::Away)
			&& (state != VideoState::Failed)
			&& (state != VideoState::NoSpace)
			&& (state != VideoState::Unplayable));
	}
	_add->setVisible(_room->can(Right::Add));
	updateLayout();
	update();
}

void VideoTab::updateLayout() {
	const auto w = width();
	const auto h = height();
	if (w <= 0 || h <= 0) {
		return;
	}
	const auto pad = Scaled(20);
	// Rounded up: a 16:9 picture in a frame one pixel lower than it needs
	// gets a black line at each side.
	const auto videoHeight = std::clamp(
		(w * 9 + 15) / 16,
		Scaled(150),
		std::max(h * 56 / 100, Scaled(150)));
	_surface->setGeometry(0, 0, w, videoHeight);
	// «Сейчас ничего не идёт» has no line of the state under it: no empty
	// band is kept for that line then.
	const auto stateLine = !VideoStatusText(_room, _surface->status()).isEmpty()
		|| !othersLoading().isEmpty();
	_info = QRect(
		pad,
		videoHeight + Scaled(12),
		w - 2 * pad,
		VideoTitleFont()->height
			+ (stateLine ? (Scaled(4) + st::normalFont->height) : 0));
	const auto headerTop = _info.y() + _info.height() + Scaled(10);
	const auto headerHeight = std::max(_add->height(), Scaled(34));
	_queueHeader = QRect(pad, headerTop, w - 2 * pad, headerHeight);
	_add->moveToRight(
		pad,
		headerTop + (headerHeight - _add->height()) / 2,
		w);
	const auto scrollTop = headerTop + headerHeight + Scaled(6);
	_scroll->setGeometry(0, scrollTop, w, std::max(h - scrollTop, 0));
	if (_list) {
		_list->resizeToWidth(w);
	}
	_drop->setGeometry(rect());
}

void VideoTab::resizeEvent(QResizeEvent *e) {
	updateLayout();
}

void VideoTab::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	p.fillRect(e->rect(), st::windowBg);

	const auto &data = _room->player(Kind::Video);
	const auto current = data.current();
	const auto status = _surface->status();

	// The title and the state under the picture.
	const auto &titleFont = VideoTitleFont();
	p.setFont(titleFont);
	p.setPen(current ? st::windowFg : st::windowSubTextFg);
	p.drawText(
		_info.x(),
		_info.y() + titleFont->ascent,
		titleFont->elided(
			current
				? current->title
				: tr::lng_oblivion_rvideo_nothing(tr::now),
			_info.width()));
	const auto lineTop = _info.y() + titleFont->height + Scaled(4);
	const auto text = VideoStatusText(_room, status);
	auto used = 0;
	if (!text.isEmpty()) {
		const auto good = (status.state == VideoState::Synced)
			|| (status.state == VideoState::Catching);
		const auto bad = (status.state == VideoState::Failed)
			|| (status.state == VideoState::NoSpace)
			|| (status.state == VideoState::Unplayable);
		const auto color = good
			? st::boxTextFgGood->c
			: bad
			? st::boxTextFgError->c
			: st::windowSubTextFg->c;
		const auto dot = Scaled(7);
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawEllipse(QRectF(
			_info.x(),
			lineTop + (st::normalFont->height - dot) / 2.,
			dot,
			dot));
		const auto available = _info.width() - dot - Scaled(6);
		const auto elided = st::normalFont->elided(text, available);
		p.setFont(st::normalFont);
		p.setPen(color);
		p.drawText(
			_info.x() + dot + Scaled(6),
			lineTop + st::normalFont->ascent,
			elided);
		used = dot + Scaled(6) + st::normalFont->width(elided) + Scaled(16);
	}
	const auto others = othersLoading();
	if (!others.isEmpty()) {
		const auto available = _info.width() - used;
		const auto width = st::normalFont->width(others);
		if (available >= std::min(width, Scaled(120))) {
			const auto elided = st::normalFont->elided(others, available);
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				_info.x() + _info.width() - st::normalFont->width(elided),
				lineTop + st::normalFont->ascent,
				elided);
		}
	}

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
			_queueHeader.x() + st::semiboldFont->width(header) + Scaled(8),
			headerTop,
			QString::number(count));
	}

	if (_list && _list->empty()) {
		const auto area = QRect(
			Scaled(32),
			_scroll->y() + Scaled(14),
			width() - Scaled(64),
			height() - _scroll->y() - Scaled(14));
		p.setFont(st::semiboldFont);
		p.setPen(st::windowFg);
		p.drawText(
			QRect(area.x(), area.y(), area.width(), st::semiboldFont->height),
			Qt::AlignHCenter | Qt::AlignTop,
			tr::lng_oblivion_rvideo_empty(tr::now));
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
				? tr::lng_oblivion_rvideo_empty_about(tr::now)
				: tr::lng_oblivion_rvideo_empty_no_right(tr::now));
	}
}

void VideoTab::showAddMenu() {
	if (!_room->can(Right::Add)) {
		Toast(_show, tr::lng_oblivion_rvideo_no_add(tr::now));
		return;
	}
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::popupMenuWithIcons);
	_menu->addAction(
		tr::lng_oblivion_rvideo_add_files(tr::now),
		[=] { addFiles(); },
		&st::menuIconFile);
	if (_room->can(Right::Queue)
		&& !_room->player(Kind::Video).queue.empty()) {
		_menu->addSeparator();
		_menu->addAction(tr::lng_oblivion_rmusic_clear(tr::now), [=] {
			if (!_show || !_show->valid()) {
				return;
			}
			const auto weak = base::make_weak(_room.get());
			_show->showBox(Ui::MakeConfirmBox({
				.text = tr::lng_oblivion_rvideo_clear_sure(tr::now),
				.confirmed = [=](Fn<void()> close) {
					if (const auto strong = weak.get()) {
						strong->clearQueue(Kind::Video);
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

void VideoTab::showRowMenu(const QString &itemId) {
	const auto &data = _room->player(Kind::Video);
	const auto index = data.indexOf(itemId);
	if (index < 0) {
		return;
	}
	const auto &item = data.queue[index];
	const auto count = int(data.queue.size());
	const auto currentIndex = data.indexOf(data.state.itemId);
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::popupMenuWithIcons);
	const auto room = _room;
	enum class RowMove {
		Next,
		Up,
		Down,
	};
	// The place is counted by the queue as it is when the action is
	// clicked: somebody else may have changed it while the menu was open.
	const auto move = [=](RowMove how) {
		const auto &now = room->player(Kind::Video);
		const auto from = now.indexOf(itemId);
		const auto size = int(now.queue.size());
		const auto to = (from < 0)
			? -1
			: (how == RowMove::Up)
			? (from - 1)
			: (how == RowMove::Down)
			? ((from + 1 < size) ? (from + 1) : -1)
			: VideoPlayNextIndex(from, now.indexOf(now.state.itemId));
		if (to >= 0) {
			room->moveItem(Kind::Video, from, to);
		}
	};
	if (_room->can(Right::Control)) {
		_menu->addAction(
			tr::lng_oblivion_rmusic_row_play(tr::now),
			[=] { room->select(Kind::Video, itemId); },
			&st::menuIconVideoChat);
	}
	if (_room->can(Right::Queue)) {
		if (VideoPlayNextIndex(index, currentIndex) >= 0) {
			_menu->addAction(
				tr::lng_oblivion_rmusic_row_next(tr::now),
				[=] { move(RowMove::Next); },
				&st::menuIconRestore);
		}
		if (index > 0) {
			_menu->addAction(
				tr::lng_oblivion_rmusic_row_up(tr::now),
				[=] { move(RowMove::Up); },
				&st::menuIconAbove);
		}
		if (index + 1 < count) {
			_menu->addAction(
				tr::lng_oblivion_rmusic_row_down(tr::now),
				[=] { move(RowMove::Down); },
				&st::menuIconBelow);
		}
	}
	if (CanRemoveItem(_room->state(), item, _room->selfId())) {
		_menu->addAction(
			tr::lng_oblivion_rmusic_row_remove(tr::now),
			[=] { room->removeItem(Kind::Video, itemId); },
			&st::menuIconDelete);
	}
	if (_menu->empty()) {
		_menu = nullptr;
		return;
	}
	_menu->popup(QCursor::pos());
}

void VideoTab::addFiles() {
	if (_room->sample()) {
		return;
	}
	const auto weak = _engine;
	const auto show = _show;
	FileDialog::GetOpenPaths(
		this,
		tr::lng_oblivion_rvideo_add_files_title(tr::now),
		(tr::lng_oblivion_rvideo_files_filter(tr::now)
			+ u" (*.mp4 *.m4v *.mov *.mkv *.webm);;"_q
			+ FileDialog::AllFilesFilter()),
		crl::guard(this, [=](FileDialog::OpenResult &&result) {
			if (const auto strong = weak.get()) {
				strong->addFiles(result.paths, show);
			}
		}));
}

void VideoTab::toggleFullscreen() {
	if (_fullscreen) {
		closeFullscreen();
		return;
	} else if (_room->sample() || !engine()) {
		return;
	} else if (Core::IsAppLaunched() && Core::App().passcodeLocked()) {
		return;
	}
	_fullscreen = base::make_unique_q<FullscreenWindow>(
		_room,
		_engine,
		_show);
	const auto raw = _fullscreen.get();
	raw->closeRequests(
	) | rpl::on_next([=] {
		// Not from inside its own event: the window is deleted there.
		crl::on_main(this, [=] {
			closeFullscreen();
		});
	}, raw->lifetime());
	const auto &title = _room->state().title;
	raw->setWindowTitle(title.isEmpty()
		? tr::lng_oblivion_room_title_default(tr::now)
		: title);
	if (const auto screen = window()->screen()) {
		raw->setScreen(screen);
		raw->setGeometry(screen->geometry());
	}
	_surface->setSuspended(true);
	raw->showFullScreen();
	raw->raise();
	raw->activateWindow();
	raw->setFocus();
}

void VideoTab::closeFullscreen() {
	if (!_fullscreen) {
		return;
	}
	_fullscreen->hide();
	_fullscreen = nullptr;
	_surface->setSuspended(false);
	if (const auto top = window()) {
		if (!top->isHidden()) {
			top->raise();
			top->activateWindow();
		}
	}
}

bool VideoTab::eventHook(QEvent *e) {
	if (e->type() == QEvent::Hide && _fullscreen) {
		// The window of the room was hidden by a lock of the app. The
		// passcode is watched by itself, this is for any other lock: the
		// full screen does not stay over it either.
		crl::on_main(this, [=] {
			if (_fullscreen && window()->isHidden()) {
				closeFullscreen();
			}
		});
	}
	return RpWidget::eventHook(e);
}

void VideoTab::setDragOver(bool over) {
	if (_drop->isHidden() == over) {
		_drop->setVisible(over);
		if (over) {
			_drop->raise();
		}
	}
}

void VideoTab::dragEnterEvent(QDragEnterEvent *e) {
	if (_room->sample()
		|| !engine()
		|| !_room->can(Right::Add)
		|| DroppedVideos(e->mimeData()).isEmpty()) {
		e->ignore();
		return;
	}
	e->setDropAction(Qt::CopyAction);
	e->accept();
	setDragOver(true);
}

void VideoTab::dragLeaveEvent(QDragLeaveEvent *e) {
	setDragOver(false);
}

void VideoTab::dropEvent(QDropEvent *e) {
	setDragOver(false);
	const auto paths = DroppedVideos(e->mimeData());
	const auto strong = engine();
	if (paths.isEmpty() || !strong || !_room->can(Right::Add)) {
		e->ignore();
		return;
	}
	e->setDropAction(Qt::CopyAction);
	e->accept();
	// The files of the user go to the relay by this drop.
	const auto weak = _engine;
	const auto show = _show;
	crl::on_main(this, [=] {
		if (const auto engine = weak.get()) {
			engine->addFiles(paths, show);
		}
	});
}

// ---- A notice for those who have not opened the tab: nothing of the
// video is downloaded or played for them, they are only told that it is
// on. The widget itself shows nothing.

class VideoNotice final : public Ui::RpWidget {
public:
	VideoNotice(QWidget *parent, TabContext context);

private:
	void check();

	const not_null<Room*> _room;
	const std::shared_ptr<Ui::Show> _show;
	QString _noticed;

};

VideoNotice::VideoNotice(QWidget *parent, TabContext context)
: RpWidget(parent)
, _room(context.room)
, _show(context.show) {
	setAttribute(Qt::WA_TransparentForMouseEvents);
	if (_room->sample()) {
		return;
	}
	_room->changes(
	) | rpl::on_next([=](Changes changes) {
		if (changes & (Changes(Change::VideoQueue)
			| Change::VideoPlayer
			| Change::Reloaded)) {
			check();
		}
	}, lifetime());
	crl::on_main(this, [=] {
		check();
	});
}

void VideoNotice::check() {
	const auto &state = _room->state();
	const auto &data = state.video;
	const auto current = data.current();
	const auto engine = FindEngine(_room);
	if (!_room->session()
		|| state.gone != Gone::No
		|| !current
		|| !data.state.playing) {
		return;
	} else if (engine && engine->playbackEnabled()) {
		return;
	} else if (_noticed == current->id) {
		return;
	}
	_noticed = current->id;
	if (data.state.updatedBy != _room->selfId()) {
		Toast(_show, tr::lng_oblivion_rvideo_notice(
			tr::now,
			lt_title,
			current->title));
	}
}

const auto VideoTabRegistration = TabRegistrar([] {
	return TabDescriptor{
		.id = u"video"_q,
		.order = 200,
		.title = [] { return tr::lng_oblivion_rvideo_tab(tr::now); },
		.create = [](
				QWidget *parent,
				TabContext context) -> object_ptr<Ui::RpWidget> {
			return object_ptr<VideoTab>(parent, std::move(context));
		},
	};
});

const auto VideoNoticeRegistration = OverlayRegistrar([] {
	return OverlayDescriptor{
		.id = u"video_notice"_q,
		.create = [](
				QWidget *parent,
				TabContext context) -> object_ptr<Ui::RpWidget> {
			return object_ptr<VideoNotice>(parent, std::move(context));
		},
	};
});

// ---- OBLIVION_SELFTEST=room_video.

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
constexpr auto kTestDuration = int64(600'000);
constexpr auto kTestLead = crl::time(450);

[[nodiscard]] PlayerState TestVideoState(
		bool playing,
		int64 position,
		int64 anchor,
		int64 rev) {
	return {
		.itemId = u"v1"_q,
		.playing = playing,
		.position = position,
		.anchor = anchor,
		.rev = rev,
	};
}

void TestSteps(Checker &check) {
	const auto step = [](
			const PlayerState &room,
			int64 serverNow,
			const VideoLocal &local,
			crl::time lead = kTestLead) {
		return NextVideoStep(room, kTestDuration, serverNow, lead, local);
	};

	// Nothing is current.
	auto nothing = PlayerState();
	auto result = step(nothing, kBase, {});
	check(result.action == VideoAction::None
		&& result.state == VideoState::Idle, "nothing current: idle");
	result = step(nothing, kBase, { .active = true });
	check(result.action == VideoAction::Stop, "nothing current: stop");

	// The room is on pause.
	const auto paused = TestVideoState(false, 30'000, kBase, 3);
	result = step(paused, kBase + 5'000, {});
	check(result.action == VideoAction::Prepare
		&& result.from == 30'000
		&& !result.play
		&& result.state == VideoState::Paused, "paused: shown on pause");
	result = step(paused, kBase + 5'000, {
		.active = true,
		.rev = 3,
		.position = 30'000,
	});
	check(result.action == VideoAction::None
		&& result.state == VideoState::Paused, "paused: nothing to do");
	result = step(paused, kBase + 5'000, {
		.active = true,
		.running = true,
		.rev = 2,
		.position = 30'200,
	});
	check(result.action == VideoAction::Hold, "paused: held where it is");
	result = step(paused, kBase + 5'000, {
		.active = true,
		.running = true,
		.rev = 2,
		.position = 31'000,
	});
	check(result.action == VideoAction::Prepare
		&& result.from == 30'000
		&& !result.play, "paused: too far, the exact frame");
	result = step(paused, kBase + 5'000, {
		.active = true,
		.scheduled = true,
		.rev = 2,
		.position = 30'150,
	});
	check(result.action == VideoAction::Hold,
		"paused: a planned start is called off");
	const auto seeked = TestVideoState(false, 90'000, kBase, 4);
	result = step(seeked, kBase + 5'000, {
		.active = true,
		.rev = 3,
		.position = 30'000,
	});
	check(result.action == VideoAction::Prepare
		&& result.from == 90'000
		&& !result.play, "paused: a seek on pause");
	const auto touched = TestVideoState(false, 30'000, kBase, 4);
	result = step(touched, kBase + 5'000, {
		.active = true,
		.rev = 3,
		.position = 30'020,
	});
	check(result.action == VideoAction::Adopt,
		"paused: the same place is kept");
	const auto beyond = TestVideoState(false, 700'000, kBase, 3);
	result = step(beyond, kBase, {});
	check(result.from == kTestDuration, "paused: clamped to the length");
	result = step(paused, kBase + 5'000, {
		.active = true,
		.running = true,
		.finished = true,
		.rev = 3,
		.position = 600'000,
	});
	check(result.action == VideoAction::None, "paused: finished stays");
	result = step(paused, kBase + 5'000, {
		.active = true,
		.running = true,
		.finished = true,
		.rev = 2,
		.position = 600'000,
	});
	check(result.action == VideoAction::Prepare
		&& result.from == 30'000, "paused: finished and moved back");

	// The room plays.
	const auto playing = TestVideoState(true, 10'000, kBase, 5);
	const auto now = kBase + 20'000;
	result = step(playing, now, {});
	check(result.action == VideoAction::Prepare
		&& result.play
		&& result.from == 30'450
		&& result.wait == 410
		&& result.state == VideoState::Starting, "playing: a start");
	const auto planned = TestVideoState(true, 50'000, kBase + 300, 6);
	result = step(planned, kBase, {});
	check(result.action == VideoAction::Prepare
		&& result.from == 50'150
		&& result.wait == 410, "playing: the anchor within the lead");
	result = step(planned, kBase, {}, crl::time(100));
	check(result.action == VideoAction::Prepare
		&& result.from == 50'000
		&& result.wait == 260, "playing: waits for the anchor");
	result = step(playing, now, {
		.active = true,
		.running = true,
		.rev = 5,
		.position = 30'040,
	});
	check(result.action == VideoAction::None
		&& result.state == VideoState::Synced, "playing: goes on");
	result = step(playing, now, {
		.active = true,
		.running = true,
		.rev = 4,
		.position = 30'100,
	});
	check(result.action == VideoAction::Adopt,
		"playing: a change that is not a jump");
	result = step(playing, now, {
		.active = true,
		.running = true,
		.rev = 4,
		.position = 100'000,
	});
	check(result.action == VideoAction::Prepare
		&& result.from == 30'450
		&& result.wait == 410, "playing: a jump");
	result = step(playing, now, {
		.active = true,
		.scheduled = true,
		.rev = 5,
		.position = 30'450,
	});
	check(result.action == VideoAction::None
		&& result.state == VideoState::Starting, "playing: start planned");

	// From a pause to playing.
	const auto resumed = TestVideoState(true, 30'000, kBase + 300, 7);
	result = step(resumed, kBase, {
		.active = true,
		.rev = 6,
		.position = 30'000,
	});
	check(result.action == VideoAction::Resume
		&& result.wait == 260, "resume: at the anchor");
	result = step(resumed, kBase, {
		.active = true,
		.rev = 6,
		.position = 30'200,
	});
	check(result.action == VideoAction::Resume
		&& result.wait == 460, "resume: a bit ahead waits longer");
	result = step(resumed, kBase + 1'000, {
		.active = true,
		.rev = 6,
		.position = 29'800,
	});
	check(result.action == VideoAction::Prepare
		&& result.from == 31'150
		&& result.wait == 410, "resume: too far behind restarts");
	result = step(resumed, kBase + 1'000, {
		.active = true,
		.force = true,
		.rev = 6,
		.position = 29'800,
	});
	check(result.action == VideoAction::Resume
		&& result.wait == 0, "resume: forced after many restarts");
	// A prepared start that got ready 200 ms late.
	result = step(playing, now + 650, {
		.active = true,
		.rev = 5,
		.position = 30'450,
	});
	check(result.action == VideoAction::Resume
		&& result.wait == 0, "late start: goes at once");
	result = step(playing, now + 3'000, {
		.active = true,
		.rev = 5,
		.position = 30'450,
	});
	check(result.action == VideoAction::Prepare
		&& result.from == 33'450, "late start: too late, prepared anew");

	// The end of the item.
	const auto ending = kBase + 589'700;
	result = step(playing, ending, {});
	check(result.action == VideoAction::None
		&& result.state == VideoState::Waiting, "end: nothing is started");
	result = step(playing, ending, {
		.active = true,
		.running = true,
		.rev = 5,
		.position = 599'650,
	});
	check(result.action == VideoAction::None
		&& result.state == VideoState::Synced, "end: plays to the end");
	result = step(playing, ending, {
		.active = true,
		.running = true,
		.rev = 4,
		.position = 599'650,
	});
	check(result.action == VideoAction::Adopt, "end: a new rev is taken");
	result = step(playing, now, {
		.active = true,
		.running = true,
		.finished = true,
		.rev = 5,
		.position = 25'000,
	});
	check(result.action == VideoAction::None
		&& result.state == VideoState::Waiting,
		"end: a short file is not restarted");
	result = step(playing, now, {
		.active = true,
		.running = true,
		.finished = true,
		.rev = 4,
		.position = 600'000,
	});
	check(result.action == VideoAction::Prepare
		&& result.play
		&& result.from == 30'450, "end: played again by a new state");
}

void TestLead(Checker &check) {
	check(VideoPrepareLead(0, 200) == 310, "lead: the first measure");
	check(VideoPrepareLead(450, 200) == 380, "lead: averaged");
	check(VideoPrepareLead(450, 10) == 261, "lead: goes down slowly");
	check(VideoPrepareLead(300, 10) == kPrepareLeadMin, "lead: not too small");
	check(VideoPrepareLead(450, 5'000) == kPrepareLeadMax,
		"lead: not too large");
	check(VideoPrepareLead(450, -5) == 255, "lead: a negative measure");
}

void TestParts(Checker &check) {
	const auto total = int64(300'000);
	check(VideoPartLength(0, total) == kPartSize, "part: the first");
	check(VideoPartLength(kPartSize, total) == kPartSize, "part: the second");
	check(VideoPartLength(2 * kPartSize, total) == total - 2 * kPartSize,
		"part: the tail");
	check(VideoPartLength(3 * kPartSize, total) == 0, "part: beyond");
	check(VideoPartLength(-1, total) == 0, "part: negative");
	check(VideoPartLength(1'000, total) == kPartSize, "part: any offset");
	check(VideoPartLength(total - 1, total) == 1, "part: the last byte");
	check(VideoPartLength(0, 0) == 0, "part: an empty file");
	check(VideoPartReady(0, total, kPartSize), "ready: exactly");
	check(!VideoPartReady(0, total, kPartSize - 1), "ready: a byte less");
	check(VideoPartReady(2 * kPartSize, total, total), "ready: the tail");
	check(!VideoPartReady(2 * kPartSize, total, total - 1),
		"ready: the tail a byte less");

	// A real file that grows.
	const auto folder = cWorkingDir() + u"oblivion_room_video_selftest/"_q;
	QDir(folder).removeRecursively();
	if (!QDir().mkpath(folder)) {
		check(false, "file: the folder");
		return;
	}
	auto data = QByteArray(int(total), Qt::Uninitialized);
	for (auto i = 0; i != int(total); ++i) {
		data[i] = char((i * 31 + (i >> 8)) & 0xFF);
	}
	const auto path = folder + u"video.mp4.part"_q;
	const auto write = [&](int from, int till, bool append) {
		auto file = QFile(path);
		const auto mode = append
			? (QIODevice::WriteOnly | QIODevice::Append)
			: (QIODevice::WriteOnly | QIODevice::Truncate);
		return file.open(mode)
			&& (file.write(data.constData() + from, till - from)
				== (till - from));
	};
	auto state = FileState();
	state.part = path;
	state.size = total;
	auto requested = base::flat_set<int64>{
		0,
		kPartSize,
		2 * kPartSize,
	};
	auto taken = TakeReadyParts(state, requested);
	check(taken.parts.empty()
		&& !taken.failed
		&& requested.size() == 3, "file: nothing before the download");
	check(write(0, 200'000, false), "file: written");
	taken = TakeReadyParts(state, requested);
	check(taken.parts.size() == 1
		&& !taken.failed
		&& taken.parts.front().offset == 0
		&& taken.parts.front().bytes == data.mid(0, int(kPartSize))
		&& requested.size() == 2, "file: the first part of a growing file");
	taken = TakeReadyParts(state, requested);
	check(taken.parts.empty() && !taken.failed, "file: no more yet");
	check(write(200'000, int(total), true), "file: the rest is written");
	taken = TakeReadyParts(state, requested);
	check(taken.parts.size() == 2
		&& !taken.failed
		&& taken.parts.back().offset == 2 * kPartSize
		&& taken.parts.back().bytes == data.mid(int(2 * kPartSize))
		&& taken.parts.back().valid(total)
		&& requested.empty(), "file: the rest of the parts");

	// The whole file, as it is after the download.
	auto whole = FileState();
	whole.complete = path;
	whole.size = total;
	requested = { 2 * kPartSize };
	taken = TakeReadyParts(whole, requested);
	check(taken.parts.size() == 1 && !taken.failed, "file: whole");
	whole.size = 400'000;
	requested = { 2 * kPartSize };
	taken = TakeReadyParts(whole, requested);
	check(taken.failed && taken.parts.empty() && requested.empty(),
		"file: shorter than promised fails");
	requested = { 3 * kPartSize };
	taken = TakeReadyParts(state, requested);
	check(taken.failed, "file: a part beyond the size fails");
	state.failed = true;
	requested = { 0 };
	taken = TakeReadyParts(state, requested);
	check(taken.failed && requested.empty(), "file: a failed download");
	QDir(folder).removeRecursively();
}

void TestFrame(Checker &check) {
	const auto limit = QSize(kFrameLimitWidth, kFrameLimitHeight);
	const auto hd = QSize(1920, 1080);
	check(VideoFrameSize(hd, QSize(960, 540), limit) == QSize(960, 540),
		"frame: fitted exactly");
	check(VideoFrameSize(hd, QSize(400, 400), limit) == QSize(400, 225),
		"frame: fitted by the width");
	check(VideoFrameSize(hd, QSize(3840, 2160), limit) == limit,
		"frame: not larger than the limit");
	check(VideoFrameSize(hd, QSize(), limit) == limit,
		"frame: no box is the limit");
	check(VideoFrameSize(QSize(), QSize(960, 540), limit).isEmpty(),
		"frame: no video");
	const auto tiny = VideoFrameSize(hd, QSize(1, 1), limit);
	check(tiny.width() >= 2 && tiny.height() >= 2, "frame: never empty");
	check(VideoFrameRect(hd, QRect(0, 0, 460, 400))
		== QRect(0, 71, 460, 258), "rect: bars above and below");
	check(VideoFrameRect(QSize(1080, 1920), QRect(10, 20, 460, 259))
		== QRect(167, 20, 145, 259), "rect: bars at the sides");
	check(VideoFrameRect(QSize(), QRect(0, 0, 460, 259)).isEmpty(),
		"rect: no image");
	check(VideoFrameRect(hd, QRect()).isEmpty(), "rect: no box");
}

[[nodiscard]] QJsonObject TestVideoItem(
		const QString &id,
		char sha,
		int64 duration,
		uint64 addedBy) {
	auto result = QJsonObject();
	result.insert(u"id"_q, id);
	result.insert(u"kind"_q, u"video"_q);
	result.insert(u"media"_q, QString(64, QChar(sha)));
	result.insert(u"size"_q, 5'000'000);
	result.insert(u"mime"_q, u"video/mp4"_q);
	result.insert(u"title"_q, u"Video "_q + id);
	result.insert(u"performer"_q, QString());
	result.insert(u"duration_ms"_q, double(duration));
	result.insert(u"cover"_q, QJsonValue());
	result.insert(u"file_name"_q, id + u".mp4"_q);
	result.insert(u"added_by"_q, double(addedBy));
	result.insert(u"added_at"_q, double(kBase));
	return result;
}

[[nodiscard]] QJsonObject TestVideoPlayer(
		const QString &itemId,
		bool playing,
		int64 position,
		int64 rev,
		const QString &repeat = u"off"_q) {
	auto result = QJsonObject();
	result.insert(u"item_id"_q, itemId.isEmpty()
		? QJsonValue()
		: QJsonValue(itemId));
	result.insert(u"playing"_q, playing);
	result.insert(u"position_ms"_q, double(position));
	result.insert(u"anchor_ms"_q, double(kBase));
	result.insert(u"rate"_q, 1);
	result.insert(u"repeat"_q, repeat);
	result.insert(u"rev"_q, double(rev));
	result.insert(u"updated_by"_q, double(9000000000000100ULL));
	result.insert(u"updated_at"_q, double(kBase));
	return result;
}

void TestQueue(Checker &check) {
	check(VideoFileName(u"a.MP4"_q)
		&& VideoFileName(u"clip.webm"_q)
		&& VideoFileName(u"/some/folder/film.mkv"_q)
		&& VideoFileName(u"x.mov"_q)
		&& VideoFileName(u"x.m4v"_q), "names: video files");
	check(!VideoFileName(u"song.mp3"_q)
		&& !VideoFileName(u"noext"_q)
		&& !VideoFileName(u"x.avi"_q)
		&& !VideoFileName(u"mp4"_q), "names: other files");
	check(VideoPlayNextIndex(3, 1) == 2, "next: from below");
	check(VideoPlayNextIndex(0, 2) == 2, "next: from above");
	check(VideoPlayNextIndex(2, 1) == -1, "next: is next already");
	check(VideoPlayNextIndex(1, 1) == -1, "next: the current one");
	check(VideoPlayNextIndex(0, -1) == -1, "next: nothing is current");
	check(VideoPlayNextIndex(-1, 0) == -1, "next: no item");

	// The queue of the video player comes with the events of the room.
	const auto self = uint64(9000000000000101ULL);
	const auto other = uint64(9000000000000100ULL);
	auto state = RoomState();
	state.code = u"K7QM2XPA9Z"_q;
	state.rights.add = true;
	const auto apply = [&](const QString &type, const QJsonObject &data) {
		return ApplyEvent(state, Cloud::Event{
			.type = type,
			.id = 1,
			.room = state.code,
			.ts = kBase,
			.data = data,
		}, self);
	};
	auto queue = QJsonArray();
	queue.push_back(TestVideoItem(u"v1"_q, 'a', 120'000, other));
	queue.push_back(TestVideoItem(u"v2"_q, 'b', 60'000, self));
	queue.push_back(TestVideoItem(u"v3"_q, 'c', 30'000, other));
	auto data = QJsonObject();
	data.insert(u"kind"_q, u"video"_q);
	data.insert(u"queue"_q, queue);
	data.insert(u"state"_q, TestVideoPlayer(u"v1"_q, true, 0, 1));
	auto changes = apply(u"room.queue"_q, data);
	check((changes & Change::VideoQueue)
		&& (changes & Change::VideoPlayer)
		&& !(changes & Change::MusicQueue), "queue: the video flags");
	check(state.video.queue.size() == 3
		&& state.music.queue.empty()
		&& state.video.current()
		&& state.video.current()->id == u"v1"_q
		&& state.video.current()->kind == Kind::Video
		&& state.video.current()->size == 5'000'000,
		"queue: the items of the video player");
	check(NextAfterEnd(state.video) == u"v2"_q, "queue: what is preloaded");
	check(CanRemoveItem(state, state.video.queue[1], self)
		&& !CanRemoveItem(state, state.video.queue[0], self),
		"queue: only the own item is removed");
	check(PositionAt(state.video.state, 120'000, kBase + 15'000) == 15'000,
		"queue: the position by the clock");

	data = QJsonObject();
	data.insert(u"kind"_q, u"video"_q);
	data.insert(u"state"_q, TestVideoPlayer(u"v3"_q, false, 7'000, 2));
	changes = apply(u"room.player"_q, data);
	check((changes & Change::VideoPlayer)
		&& !(changes & Change::MusicPlayer)
		&& state.video.state.itemId == u"v3"_q
		&& !state.video.state.playing
		&& state.video.state.position == 7'000, "queue: a select on pause");
	check(NextAfterEnd(state.video).isEmpty(), "queue: the last one");
	data.insert(u"state"_q, TestVideoPlayer(u"v3"_q, true, 0, 3, u"all"_q));
	changes = apply(u"room.player"_q, data);
	check(NextAfterEnd(state.video) == u"v1"_q, "queue: repeat all");

	// The uploads of the video player are the ones the tab shows.
	auto room = Room(Room::Descriptor{
		.selfId = self,
		.state = state,
		.now = [] { return kBase + 4'000; },
		.uploads = {
			Upload{ .id = 1, .kind = Kind::Music, .title = u"Song"_q },
			Upload{
				.id = 2,
				.kind = Kind::Video,
				.title = u"Film"_q,
				.ready = 10,
				.total = 40,
			},
		},
	});
	const auto uploads = VideoUploads(&room);
	check(uploads.size() == 1 && uploads.front().id == 2,
		"queue: the video uploads only");
	check(room.position(Kind::Video) == 4'000, "queue: a sample position");
	check(!FindEngine(&room) && !EnsureEngine(&room),
		"queue: no engine for a sample");
}

// The display, the disk and the memory of the device.
void TestDevice(Checker &check) {
	// The display stays on only for a picture that moves and is seen.
	check(VideoKeepsDisplayOn(VideoState::Synced, true, true, true)
		&& VideoKeepsDisplayOn(VideoState::Catching, true, true, true)
		&& VideoKeepsDisplayOn(VideoState::Starting, true, true, true)
		&& VideoKeepsDisplayOn(VideoState::Buffering, true, true, true),
		"display: kept on while the picture moves");
	check(!VideoKeepsDisplayOn(VideoState::Synced, false, true, true),
		"display: not on pause");
	check(!VideoKeepsDisplayOn(VideoState::Synced, true, false, true),
		"display: not without a picture");
	check(!VideoKeepsDisplayOn(VideoState::Synced, true, true, false),
		"display: not behind another tab");
	check(!VideoKeepsDisplayOn(VideoState::Away, true, true, true)
		&& !VideoKeepsDisplayOn(VideoState::Paused, true, true, true)
		&& !VideoKeepsDisplayOn(VideoState::Loading, true, true, true)
		&& !VideoKeepsDisplayOn(VideoState::Failed, true, true, true)
		&& !VideoKeepsDisplayOn(VideoState::NoSpace, true, true, true)
		&& !VideoKeepsDisplayOn(VideoState::Unplayable, true, true, true)
		&& !VideoKeepsDisplayOn(VideoState::Waiting, true, true, true)
		&& !VideoKeepsDisplayOn(VideoState::Idle, true, true, true),
		"display: not in the other states");

	// The budget of the disk.
	const auto mb = int64(1024) * 1024;
	const auto file = [&](
			const char *media,
			int64 megabytes,
			crl::time usedAt,
			bool kept = false) {
		return VideoFile{
			.media = QString::fromUtf8(media),
			.bytes = megabytes * mb,
			.usedAt = usedAt,
			.kept = kept,
		};
	};
	auto plan = PlanVideoSpace(
		{ file("a", 300, 10), file("new", 700, 0, true) },
		2048 * mb,
		50'000 * mb,
		1'212 * mb);
	check(plan.evict.empty() && plan.enough, "space: within the budget");
	plan = PlanVideoSpace(
		{
			file("old", 600, 10),
			file("older", 700, 5),
			file("current", 700, 30, true),
			file("new", 700, 0, true),
		},
		2048 * mb,
		50'000 * mb,
		1'212 * mb);
	check(plan.evict == std::vector<QString>{ u"older"_q }
		&& plan.enough, "space: the one used longest ago goes first");
	plan = PlanVideoSpace(
		{
			file("old", 600, 10),
			file("older", 700, 5),
			file("oldest", 700, 1),
			file("current", 700, 30, true),
			file("new", 700, 0, true),
		},
		2048 * mb,
		50'000 * mb,
		1'212 * mb);
	check(plan.evict == std::vector<QString>{ u"oldest"_q, u"older"_q }
		&& plan.enough, "space: as many as the budget asks");
	plan = PlanVideoSpace(
		{
			file("current", 700, 30, true),
			file("next", 700, 0, true),
			file("music", 700, 2, true),
			file("new", 700, 0, true),
		},
		2048 * mb,
		50'000 * mb,
		1'212 * mb);
	check(plan.evict.empty() && plan.enough,
		"space: what is needed is never removed");
	plan = PlanVideoSpace(
		{ file("old", 300, 10), file("new", 700, 0, true) },
		2048 * mb,
		1'000 * mb,
		1'212 * mb);
	check(plan.evict == std::vector<QString>{ u"old"_q }
		&& plan.enough, "space: a short disk is freed");
	plan = PlanVideoSpace(
		{ file("old", 100, 10), file("new", 700, 0, true) },
		2048 * mb,
		1'000 * mb,
		1'212 * mb);
	check(plan.evict == std::vector<QString>{ u"old"_q }
		&& !plan.enough, "space: no room even so");
	plan = PlanVideoSpace(
		{ file("new", 700, 0, true) },
		2048 * mb,
		100 * mb,
		1'212 * mb);
	check(plan.evict.empty() && !plan.enough, "space: a full disk");
	plan = PlanVideoSpace(
		{ file("old", 300, 10), file("new", 700, 0, true) },
		2048 * mb,
		-1,
		1'212 * mb);
	check(plan.evict.empty() && plan.enough,
		"space: an unknown disk is not a full one");

	// The reader of the player is restarted in a long video only.
	const auto longAgo = kRecycleInterval;
	check(!VideoReaderRestart(kReadLimit - 1, longAgo, 600'000),
		"memory: not before the limit");
	check(VideoReaderRestart(kReadLimit, longAgo, 600'000),
		"memory: at the limit");
	check(!VideoReaderRestart(2 * kReadLimit, longAgo - 1, 600'000),
		"memory: not too often");
	check(!VideoReaderRestart(2 * kReadLimit, longAgo, 5'000),
		"memory: not at the very end");

	// Only the files of the relay are removed, only of the media asked.
	const auto folder = cWorkingDir() + u"oblivion_room_video_selftest/"_q;
	QDir(folder).removeRecursively();
	if (!QDir().mkpath(folder)) {
		check(false, "files: the folder");
		return;
	}
	const auto write = [&](const QString &name, int size) {
		auto created = QFile(folder + name);
		return created.open(QIODevice::WriteOnly)
			&& (created.write(QByteArray(size, 'x')) == size);
	};
	const auto here = [&](const QString &name) {
		return QFileInfo::exists(folder + name);
	};
	const auto first = QString(64, QChar('a'));
	const auto second = QString(64, QChar('b'));
	check(write(first + u".mp4"_q, 1'000)
		&& write(first + u".mp4.part"_q, 500)
		&& write(first, 200)
		&& write(second + u".mp4"_q, 300)
		&& write(u"cover_"_q + first, 100)
		&& write(u"tg_1.video"_q, 100)
		&& write(u"film.mp4"_q, 100), "files: written");
	check(RemoveMediaFilesIn(folder, u"../film"_q) == 0
		&& RemoveMediaFilesIn(folder, u"*"_q) == 0
		&& RemoveMediaFilesIn(folder, QString()) == 0
		&& RemoveMediaFilesIn(QString(), first) == 0
		&& here(first + u".mp4"_q)
		&& here(u"film.mp4"_q), "files: only a media id is taken");
	check(RemoveMediaFilesIn(folder, first) == 1'700,
		"files: the file, its part and the one without a suffix");
	check(!here(first + u".mp4"_q)
		&& !here(first + u".mp4.part"_q)
		&& !here(first), "files: of the media are gone");
	check(here(second + u".mp4"_q)
		&& here(u"cover_"_q + first)
		&& here(u"tg_1.video"_q)
		&& here(u"film.mp4"_q), "files: the other ones stay");
	check(RemoveMediaFilesIn(folder, first) == 0, "files: nothing twice");

	// A whole file that was moved, removed or replaced.
	auto whole = FileState();
	check(!WholeFileLost(whole), "lost: nothing was given");
	whole.complete = folder + second + u".mp4"_q;
	whole.size = 300;
	check(!WholeFileLost(whole), "lost: the file is here");
	whole.size = 400;
	check(WholeFileLost(whole), "lost: not the same size");
	whole.size = 300;
	check(QFile::remove(whole.complete) && WholeFileLost(whole),
		"lost: the file is gone");
	whole.complete = folder;
	check(WholeFileLost(whole), "lost: a folder is not a file");
	QDir(folder).removeRecursively();
}

// ---- Snapshot scenes (OBLIVION_SELFTEST=ui).

constexpr auto kSampleNow = int64(1791327935000);
constexpr auto kSampleSelf = uint64(9000000000000101ULL);
constexpr auto kSampleOwner = uint64(9000000000000100ULL);
constexpr auto kSampleThird = uint64(9000000000000102ULL);

[[nodiscard]] QString SampleText(const char *ru, const char *en) {
	return QString::fromUtf8(CurrentLanguageIsRussian() ? ru : en);
}

// A frame of a film that does not exist: an evening sky over hills.
[[nodiscard]] QImage SampleFrame() {
	auto result = QImage(1280, 720, QImage::Format_ARGB32_Premultiplied);
	auto p = QPainter(&result);
	p.setRenderHint(QPainter::Antialiasing);
	auto sky = QLinearGradient(0, 0, 0, 720);
	sky.setColorAt(0., QColor(24, 28, 68));
	sky.setColorAt(0.55, QColor(120, 62, 118));
	sky.setColorAt(0.8, QColor(240, 136, 92));
	sky.setColorAt(1., QColor(255, 204, 128));
	p.fillRect(result.rect(), sky);
	p.setPen(Qt::NoPen);
	for (auto i = 0; i != 60; ++i) {
		// Scattered so that no two stars stick together into a smudge.
		const auto x = (i * i * 31 + i * 197 + 61) % 1280;
		const auto y = (i * i * 11 + i * 89 + 23) % 330;
		const auto size = 1.5 + (i % 3);
		p.setBrush(QColor(255, 255, 255, 90 + (i % 4) * 40));
		p.drawEllipse(QPointF(x, y), size, size);
	}
	p.setBrush(QColor(255, 236, 190));
	p.drawEllipse(QPointF(930, 430), 74, 74);
	const auto hills = [&](int base, int height, int shift, QColor color) {
		auto path = QPainterPath();
		path.moveTo(0, 720);
		path.lineTo(0, base);
		for (auto x = 0; x <= 1280; x += 160) {
			const auto up = ((x / 160 + shift) % 2) ? height : -height;
			path.quadTo(x + 80, base + up, x + 160, base);
		}
		path.lineTo(1280, 720);
		path.closeSubpath();
		p.setBrush(color);
		p.drawPath(path);
	};
	hills(520, 70, 0, QColor(86, 46, 96));
	hills(590, 54, 1, QColor(48, 28, 66));
	hills(660, 36, 0, QColor(20, 14, 34));
	return result;
}

[[nodiscard]] Member SampleMember(
		uint64 id,
		const QString &name,
		bool owner,
		bool online,
		const Rights &rights) {
	return {
		.id = id,
		.name = name,
		.owner = owner,
		.rights = owner ? Rights::Everything() : rights,
		.joinedAt = kSampleNow - 3'600'000,
		.online = online,
	};
}

[[nodiscard]] QueueItem SampleVideo(
		const QString &id,
		char sha,
		const QString &title,
		int64 duration,
		uint64 addedBy) {
	return {
		.id = id,
		.kind = Kind::Video,
		.media = QString(64, QChar(sha)),
		.size = 180'000'000,
		.mime = u"video/mp4"_q,
		.title = title,
		.duration = duration,
		.fileName = title + u".mp4"_q,
		.addedBy = addedBy,
		.addedAt = kSampleNow - 600'000,
	};
}

enum class SampleKind {
	Owner,
	Guest, // May add, does not control.
	Viewer, // No rights but the chat.
};

[[nodiscard]] RoomState SampleVideoRoom(SampleKind kind, bool filled) {
	auto state = RoomState();
	state.code = u"K7QM2XPA9Z"_q;
	state.link = Cloud::MakeLink(Cloud::LinkKind::Room, state.code);
	state.title = SampleText("Кино по пятницам", "Friday movies");
	state.rev = 7;
	state.settings.defaults = Rights::Everything();
	const auto owner = (kind == SampleKind::Owner);
	state.ownerId = owner ? kSampleSelf : kSampleOwner;
	state.owner = owner;
	auto limited = Rights::OnlyOwner();
	limited.add = (kind != SampleKind::Viewer);
	limited.queue = false;
	limited.control = false;
	state.rights = owner ? Rights::Everything() : limited;
	const auto misha = SampleText("Миша", "Michael");
	state.members.push_back(SampleMember(
		kSampleSelf,
		SampleText("Аня", "Anna"),
		owner,
		true,
		limited));
	state.members.push_back(SampleMember(
		kSampleOwner,
		misha,
		!owner,
		true,
		Rights::Everything()));
	state.members.push_back(SampleMember(
		kSampleThird,
		SampleText("Лера", "Valerie"),
		false,
		true,
		limited));
	if (!filled) {
		return state;
	}
	state.members[2].video = {
		.itemId = u"v1"_q,
		.ready = false,
		.buffered = 40,
	};
	state.video.queue = {
		SampleVideo(
			u"v1"_q,
			'2',
			SampleText("Поход на Алтай, день второй", "Altai hike, day two"),
			2'412'000,
			kSampleOwner),
		SampleVideo(
			u"v2"_q,
			'9',
			SampleText("Концерт во дворе", "Backyard concert"),
			3'850'000,
			kSampleSelf),
		SampleVideo(
			u"v3"_q,
			'c',
			SampleText("Кот против пылесоса", "Cat vs. vacuum cleaner"),
			42'000,
			kSampleThird),
	};
	state.video.state = {
		.itemId = u"v1"_q,
		.playing = true,
		.position = 765'000,
		.anchor = kSampleNow - 20'000,
		.rev = 12,
		.updatedBy = kSampleOwner,
	};
	return state;
}

[[nodiscard]] Room::Descriptor SampleDescriptor(
		SampleKind kind,
		bool filled = true) {
	return {
		.selfId = kSampleSelf,
		.state = SampleVideoRoom(kind, filled),
		.now = [] { return kSampleNow; },
	};
}

// The whole content of a room window with the «Видео» tab shown.
class VideoSceneHost final : public Ui::RpWidget {
public:
	VideoSceneHost(
		QWidget *parent,
		Room::Descriptor &&descriptor,
		SampleView &&view)
	: RpWidget(parent)
	, _room(std::make_unique<Room>(std::move(descriptor))) {
		NextSampleView() = std::move(view);
		_content = base::unique_qptr<Ui::RpWidget>(CreateRoomWidget(
			this,
			_room.get(),
			SelfTest::SceneShow(this),
			u"video"_q).release());
		NextSampleView() = std::nullopt;
		_content->show();
		sizeValue(
		) | rpl::on_next([=](QSize size) {
			if (_content) {
				_content->setGeometry(QRect(QPoint(), size));
			}
		}, lifetime());
	}
	~VideoSceneHost() {
		_content = nullptr;
	}

private:
	const std::unique_ptr<Room> _room;
	base::unique_qptr<Ui::RpWidget> _content;

};

// Only the surface, as the full screen shows it.
class SurfaceSceneHost final : public Ui::RpWidget {
public:
	SurfaceSceneHost(
		QWidget *parent,
		Room::Descriptor &&descriptor,
		SampleView &&view)
	: RpWidget(parent)
	, _room(std::make_unique<Room>(std::move(descriptor))) {
		_content = base::unique_qptr<Surface>(Ui::CreateChild<Surface>(
			this,
			_room.get(),
			base::weak_ptr<VideoEngine>(),
			SelfTest::SceneShow(this),
			true,
			std::optional<SampleView>(std::move(view))));
		_content->show();
		sizeValue(
		) | rpl::on_next([=](QSize size) {
			if (_content) {
				_content->setGeometry(QRect(QPoint(), size));
			}
		}, lifetime());
	}
	~SurfaceSceneHost() {
		_content = nullptr;
	}

private:
	const std::unique_ptr<Room> _room;
	base::unique_qptr<Surface> _content;

};

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	const auto window = [](
			const QString &name,
			QSize size,
			Fn<Room::Descriptor()> make,
			Fn<SampleView()> view) {
		RegisterScene(name, size, [=](not_null<Ui::RpWidget*> parent) {
			return Ui::CreateChild<VideoSceneHost>(
				parent.get(),
				make(),
				view());
		});
	};
	const auto playing = [] {
		return SampleView{
			.status = { .state = VideoState::Synced },
			.frame = SampleFrame(),
		};
	};
	const auto size = QSize(Scaled(460), Scaled(720));
	window(u"room_video_playing"_q, size, [] {
		auto result = SampleDescriptor(SampleKind::Owner);
		result.uploads.push_back({
			.id = 1,
			.kind = Kind::Video,
			.title = SampleText("День рождения Леры", "Valerie's birthday"),
			.ready = 212'000'000,
			.total = 540'000'000,
		});
		return result;
	}, playing);
	window(u"room_video_buffering"_q, size, [] {
		return SampleDescriptor(SampleKind::Guest);
	}, [] {
		return SampleView{
			.status = { .state = VideoState::Loading, .percent = 42 },
			.pending = { VideoEngine::PendingRow{
				.id = 1,
				.title = SampleText("Видео от 03.10.2026", "Video of 10/3/26"),
				.percent = 63,
			} },
		};
	});
	window(u"room_video_empty"_q, size, [] {
		return SampleDescriptor(SampleKind::Owner, false);
	}, [] {
		return SampleView();
	});
	window(u"room_video_guest"_q, size, [] {
		auto result = SampleDescriptor(SampleKind::Viewer);
		result.state.video.state.playing = false;
		return result;
	}, [] {
		return SampleView{
			.status = { .state = VideoState::Paused },
			.frame = SampleFrame(),
		};
	});
	window(u"room_video_guest_empty"_q, size, [] {
		return SampleDescriptor(SampleKind::Viewer, false);
	}, [] {
		return SampleView();
	});
	window(u"room_video_away"_q, size, [] {
		return SampleDescriptor(SampleKind::Owner);
	}, [] {
		return SampleView{
			.status = { .state = VideoState::Away },
			.frame = SampleFrame(),
		};
	});
	window(u"room_video_failed"_q, size, [] {
		return SampleDescriptor(SampleKind::Guest);
	}, [] {
		return SampleView{
			.status = { .state = VideoState::Failed },
		};
	});
	window(u"room_video_no_space"_q, size, [] {
		return SampleDescriptor(SampleKind::Guest);
	}, [] {
		return SampleView{
			.status = { .state = VideoState::NoSpace },
		};
	});
	window(u"room_video_narrow"_q, QSize(Scaled(380), Scaled(520)), [] {
		return SampleDescriptor(SampleKind::Owner);
	}, playing);
	window(u"room_video_wide"_q, QSize(Scaled(760), Scaled(640)), [] {
		return SampleDescriptor(SampleKind::Owner);
	}, playing);
	RegisterScene(
		u"room_video_fullscreen"_q,
		QSize(Scaled(960), Scaled(540)),
		[=](not_null<Ui::RpWidget*> parent) {
			return Ui::CreateChild<SurfaceSceneHost>(
				parent.get(),
				SampleDescriptor(SampleKind::Owner),
				playing());
		});
});

} // namespace

bool RunVideoSelfTest(QStringList &log) {
	auto check = Checker(log);
	TestSteps(check);
	check.section("steps");
	TestLead(check);
	check.section("lead");
	TestParts(check);
	check.section("parts");
	TestFrame(check);
	check.section("frame");
	TestQueue(check);
	check.section("queue");
	TestDevice(check);
	check.section("device");
	log.push_back(u"room_video: %1 checks, %2 failed"_q.arg(
		QString::number(check.passed() + check.failed()),
		QString::number(check.failed())));
	return !check.failed();
}

} // namespace Oblivion::Rooms
