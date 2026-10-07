/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_msg_id.h"
#include "oblivion/oblivion_cloud.h"
#include "oblivion/oblivion_cloud_share.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtGui/QImage>

class DocumentData;

namespace Data {
class DocumentMedia;
} // namespace Data

// Round 5: sharing, the inner header of the module: what the model
// (oblivion_cloud_share.cpp) gives to the boxes
// (oblivion_cloud_share_ui.cpp). Other modules include only
// oblivion_cloud_share.h.
//
// Everything that comes from the server is validated here once (ids,
// lengths, counts, the stacks of effects go through the deserialisers of
// the editors), the boxes show the values as they are.
namespace Oblivion::Share {

inline constexpr auto kMaxPlaylistTitle = 128;
inline constexpr auto kMaxPlaylistDescription = 500;
inline constexpr auto kMaxTrackText = 128; // limits.track_text.
inline constexpr auto kMaxPlaylistTracks = 500;
inline constexpr auto kMaxPresetTitle = 64;
inline constexpr auto kMaxPresetDescription = 300;
inline constexpr auto kMaxPresetData = 65536;
inline constexpr auto kMaxPhotoEffects = 48;
inline constexpr auto kMaxVideoEffects = 12;
inline constexpr auto kMaxLocalPresets = 500;
inline constexpr auto kMaxTrackDuration = int64(86400000);

// ---- Shared playlists.

struct Track {
	QString id; // Of the track in its playlist.
	QString media; // The sha256 of the file.
	int64 size = 0;
	QString mime;
	QString title;
	QString performer;
	QString fileName;
	int64 duration = 0; // Milliseconds.
	uint64 addedBy = 0;
};

struct Playlist {
	QString id;
	QString link; // Made here from the id, never taken from the server.
	uint64 ownerId = 0;
	QString ownerName;
	QString title;
	QString description;
	bool collab = false; // Followers may add tracks.
	bool listed = false; // Shown in the Oblivion profile of the owner.
	bool canEdit = false; // The user is the owner.
	bool canAdd = false;
	int rev = 0;
	int followers = 0;
	int trackCount = 0;
	int64 totalBytes = 0;
	int64 totalDuration = 0;
	bool full = false; // With the tracks (not a summary of a list).
	std::vector<Track> tracks;

	[[nodiscard]] bool valid() const {
		return !id.isEmpty();
	}
};

// An invalid playlist for anything that is not one. Tracks that are not
// valid are dropped.
[[nodiscard]] Playlist ParsePlaylist(const QJsonObject &object);
// For the copy kept on this device, ParsePlaylist() reads it back.
[[nodiscard]] QJsonObject PlaylistToJson(const Playlist &playlist);

struct TrackInput {
	QString media;
	QString title;
	QString performer;
	QString fileName;
	int64 duration = 0; // Milliseconds.
};
// What the server accepts: a title that is never empty, texts cut to
// their limits, a duration inside 1 ms .. 24 h.
[[nodiscard]] QJsonObject TrackInputToJson(const TrackInput &input);

// "Artist — Title", or the one of them that is there, or the file name.
[[nodiscard]] QString TrackName(
	const QString &title,
	const QString &performer,
	const QString &fileName);

// ---- Presets: stacks of effects of the photo and the video editor.

[[nodiscard]] bool ValidKind(const QString &kind); // "photo" | "video".

// Through the deserialiser of the editor and back: unknown effects are
// dropped, values clamped, not more effects than the editor takes. Empty
// if nothing usable is left. Any thread.
[[nodiscard]] QByteArray SanitizeStack(
	const QString &kind,
	const QByteArray &stack);
// The ids of the effects that are switched on, without repeats.
[[nodiscard]] QStringList StackEffects(
	const QString &kind,
	const QByteArray &stack);
// The name of an effect in the language of the app, empty if this build
// has no such effect. Main thread.
[[nodiscard]] QString EffectName(const QString &kind, const QString &id);
// "Glitch, Old TV, Grain". Main thread.
[[nodiscard]] QString EffectNames(
	const QString &kind,
	const QStringList &ids);

// The "data" object of a preset on the server and back (validated).
[[nodiscard]] QJsonObject PresetData(
	const QString &kind,
	const QByteArray &stack);
[[nodiscard]] QByteArray StackFromData(
	const QString &kind,
	const QJsonObject &data);

// The description of a preset on the server carries the ids of its
// effects in the last line ("fx: lofi.ccd, glitch.rgb"), so a list of
// the gallery (which has no data) can name them in any language.
struct Description {
	QString text;
	QStringList tags;
};
[[nodiscard]] QString ComposeDescription(
	const QString &text,
	const QStringList &tags);
[[nodiscard]] Description ParseDescription(const QString &description);

struct Preset {
	QString id;
	QString link;
	QString kind;
	QString title;
	QString text; // The description without the line of the effects.
	QStringList tags; // The ids of the effects.
	uint64 ownerId = 0;
	QString ownerName;
	bool listed = false; // In the gallery.
	bool hidden = false; // Hidden from the gallery by reports.
	bool canEdit = false;
	bool used = false;
	int uses = 0;
	int appBuild = 0;
	int64 created = 0;
	bool full = false; // The data was there.
	QByteArray stack; // Validated, empty if this build can't use it.

	[[nodiscard]] bool valid() const {
		return !id.isEmpty();
	}
};
[[nodiscard]] Preset ParsePreset(const QJsonObject &object);

// A picture to show a stack on, made in code. Any thread.
[[nodiscard]] QImage SampleImage(QSize size);
// The sample with the stack applied. Slow, for crl::async.
[[nodiscard]] QImage RenderPreview(
	const QString &kind,
	const QByteArray &stack,
	QImage sample);

// The presets saved on this device (tdata/oblivion/presets.json, one
// list for all accounts). Main thread.
struct LocalPreset {
	uint64 id = 0;
	QString kind;
	QString title;
	QByteArray stack;
	QString cloudId; // Published by the user or saved from the gallery.
	int64 created = 0; // Unixtime.
};

class Library final {
public:
	explicit Library(QString path); // Empty: only in the memory.

	[[nodiscard]] const std::vector<LocalPreset> &list();
	[[nodiscard]] const LocalPreset *find(uint64 id);
	[[nodiscard]] const LocalPreset *findByCloudId(const QString &cloudId);

	// 0 if the preset is not valid or there are too many of them. A preset
	// with a cloud id that is saved already is updated.
	uint64 add(LocalPreset preset);
	void rename(uint64 id, const QString &title);
	void setCloudId(uint64 id, const QString &cloudId);
	void remove(uint64 id);

	[[nodiscard]] rpl::producer<> changes() const;

	// For the settings sync: everything, and what came from another
	// device (new presets are added, the known ones are replaced, the
	// ones deleted there are deleted here).
	//
	// A deleted preset leaves its id and the time behind for half a year
	// ({"id", "time"} in exportRemovedJson()). That is what makes a
	// deletion stay: without it the preset would come back with the next
	// copy of a device that still has it. A deletion wins over whatever
	// another device has done to the same preset meanwhile.
	[[nodiscard]] QJsonArray exportJson();
	[[nodiscard]] QJsonArray exportRemovedJson();
	void mergeJson(
		const QJsonArray &list,
		const QJsonArray &removed = QJsonArray());
	// Whether mergeJson() with the same data would change the presets,
	// and how many presets of this device it would delete («Получить»
	// says it before it is done).
	[[nodiscard]] bool mergeChanges(
		const QJsonArray &list,
		const QJsonArray &removed);
	[[nodiscard]] int countRemovals(const QJsonArray &removed);

private:
	struct Removed {
		uint64 id = 0;
		int64 time = 0; // Unixtime.
	};
	struct Merged {
		bool list = false; // The presets have changed.
		bool removed = false; // Only what is remembered as deleted.
	};

	void ensureLoaded();
	void save();
	Merged merge(const QJsonArray &list, const QJsonArray &removed);
	bool mergeList(const QJsonArray &list);
	bool noteRemoved(uint64 id, int64 time);
	[[nodiscard]] bool wasRemoved(uint64 id) const;

	const QString _path;
	std::vector<LocalPreset> _list;
	std::vector<Removed> _removed;
	bool _loaded = false;
	rpl::event_stream<> _changes;

};
[[nodiscard]] Library &PresetLibrary();

// PresetHost::apply that tells whether the editor has taken the stack.
// An editor may refuse (a locked layer, too many effects: it says so
// itself) or have nothing to change, and it is asked through a callback
// that returns nothing, so what tells is its own stack before and after.
// Null for a host that can't apply.
[[nodiscard]] Fn<bool(const QByteArray &stack)> CheckedApply(
	const PresetHost &host);

// ---- What an account remembers about its shared playlists
// (tdata/oblivion/<id>/shared.json).

class Store final {
public:
	explicit Store(QString path); // Empty: only in the memory.

	// The shared playlist a local one became.
	[[nodiscard]] QString publishedId(uint64 localId);
	void setPublished(uint64 localId, const QString &cloudId);
	void forgetPublished(const QString &cloudId);

	// The file a document of Telegram became on the server: a track that
	// was sent once is not downloaded and hashed again.
	[[nodiscard]] QString knownMedia(uint64 document);
	void setKnownMedia(uint64 document, const QString &media);

	// «Добавить к себе»: the copies of the playlists the user keeps.
	[[nodiscard]] const std::vector<Playlist> &saved();
	[[nodiscard]] const Playlist *findSaved(const QString &id);
	void save(const Playlist &playlist);
	void unsave(const QString &id);

	[[nodiscard]] rpl::producer<> changes() const;

private:
	void ensureLoaded();
	void write();

	const QString _path;
	base::flat_map<uint64, QString> _published;
	base::flat_map<uint64, QString> _media;
	std::vector<Playlist> _saved;
	bool _loaded = false;
	rpl::event_stream<> _changes;

};

// ---- The upload of a playlist.

struct UploadSource {
	FullMsgId item; // A music message of Telegram, or
	QString path; // a file on the disk.
	uint64 document = 0;
	QString title;
	QString performer;
	QString fileName;
	int64 duration = 0; // Milliseconds, 0: read from the file.
};

struct UploadRequest {
	uint64 localId = 0; // The local playlist, 0 if there is none.
	QString title;
	QString playlistId; // Add to this one instead of making a new one.
	std::vector<UploadSource> sources;
};

struct UploadStatus {
	enum class Stage {
		Idle,
		Fetching, // The file is taken from Telegram or read from the disk.
		Sending, // To the media relay.
		Adding, // The track is put into the playlist.
		Done,
		Failed, // Stopped at a track, retry() goes on from it.
		Cancelled,
	};
	Stage stage = Stage::Idle;
	int index = 0;
	int count = 0;
	QString track;
	float64 part = 0.; // Of the current stage, 0 .. 1.
	int added = 0;
	int skipped = 0; // Gone, protected, too large or unreadable.
	Cloud::Error error;
	QString title;
	QString playlistId; // Empty until the first track is there.
	bool created = false; // This upload has made the playlist.

	[[nodiscard]] bool finished() const {
		return (stage == Stage::Done)
			|| (stage == Stage::Failed)
			|| (stage == Stage::Cancelled);
	}
};

class Uploader final : public base::has_weak_ptr {
public:
	Uploader(
		not_null<Main::Session*> session,
		not_null<Store*> store,
		UploadRequest &&request);
	~Uploader();

	void start();
	void retry();
	void cancel();
	// The account was switched off (by the user, by the server): what
	// was on the way will never answer. The upload stops as a failed one
	// and retry() goes on from the same track.
	void disconnected();

	[[nodiscard]] const UploadStatus &status() const {
		return _status;
	}
	// The file of this document is being taken from Telegram for the
	// upload right now.
	[[nodiscard]] bool fetching(not_null<DocumentData*> document) const;
	[[nodiscard]] rpl::producer<UploadStatus> statusValue() const;

	// For what follows this upload from outside, dies with it.
	[[nodiscard]] rpl::lifetime &lifetime() {
		return _lifetime;
	}

	// How many boxes show this upload now: the one that starts it tells
	// the result with a toast when there is none.
	int shownBoxes = 0;

private:
	void schedule();
	void process();
	void processMessage(const UploadSource &source);
	void processFile(const UploadSource &source);
	[[nodiscard]] bool documentReady();
	void send(const QString &path, const QByteArray &bytes);
	void add(const QString &media, bool known);
	void added(const Playlist &playlist, const QString &media);
	void skip();
	void fail(const Cloud::Error &error);
	void finish(UploadStatus::Stage stage);
	void stopFetching();
	void clearCurrent();
	void changed();

	const not_null<Main::Session*> _session;
	const not_null<Store*> _store;
	const base::weak_ptr<Cloud::Account> _account;
	Cloud::Sender _sender;
	UploadRequest _request;
	UploadStatus _status;
	rpl::event_stream<UploadStatus> _changes;

	// The track that is being worked on.
	TrackInput _input;
	QString _mime;
	uint64 _documentId = 0;
	DocumentData *_document = nullptr;
	std::shared_ptr<Data::DocumentMedia> _media;
	rpl::lifetime _fetchLifetime;
	bool _fetching = false;
	bool _ownDownload = false;
	bool _requested = false;
	QString _tempPath;
	Cloud::TransferId _transfer = 0;
	base::Timer _watchdog;
	// The server asks to slow down (429) when tracks are added one right
	// after another: the same track is added again a little later, and
	// the rest of this upload keeps that pace instead of being refused
	// track after track.
	base::Timer _addAgain;
	QString _addMedia;
	bool _addKnown = false;
	int _addRetries = 0;
	crl::time _addGap = 0;
	crl::time _addLast = 0;
	int _generation = 0;
	bool _scheduled = false;
	rpl::lifetime _lifetime;

};

// ---- Everything of one session.

struct Playing {
	QString playlistId;
	QString media;
	int index = -1;
	bool loading = false;
	bool failed = false;
	float64 progress = 0.;
};

class Service final : public base::has_weak_ptr {
public:
	explicit Service(not_null<Main::Session*> session);
	~Service();

	[[nodiscard]] Main::Session &session() const;
	[[nodiscard]] Cloud::Account &account() const;
	[[nodiscard]] Store &store();

	// One upload at a time: a new one is refused (nullptr) while another
	// one works. A finished one stays till dropUpload() for its result.
	[[nodiscard]] Uploader *uploader() const;
	Uploader *startUpload(UploadRequest &&request);
	void dropUpload();

	// The tracks are played through the player of the app: a file is
	// taken from the server first (or from the copy on this device).
	void play(const Playlist &playlist, int index);
	void stopLoading();
	[[nodiscard]] const Playing &playing() const;
	[[nodiscard]] rpl::producer<> playingChanges() const;

	// «Добавить к себе»: the playlist stays in the list of the user and
	// its files are kept on this device. The files are taken one by one;
	// what was not taken (the app was closed, the connection was lost)
	// is taken later: when the account connects, a while after a
	// failure, when the playlist is opened. Tracks the owner adds later
	// are saved too, the files of removed ones are deleted.
	void keep(const Playlist &playlist);
	void forget(const Playlist &playlist);
	[[nodiscard]] bool kept(const QString &playlistId);
	[[nodiscard]] int keptTracks(const Playlist &playlist) const;
	[[nodiscard]] bool keeping(const QString &playlistId) const;
	[[nodiscard]] rpl::producer<> keptChanges() const;

	// The newest version of a playlist that was requested in this launch.
	void remember(const Playlist &playlist);
	[[nodiscard]] rpl::producer<Playlist> updates() const;

private:
	struct KeepTask {
		QString playlistId;
		QString media;
	};
	enum class KeepResult {
		Done,
		Gone, // Not on the server any more: not asked again.
		Failed, // No connection: tried again later.
	};

	[[nodiscard]] QString keptPath(const QString &media) const;
	[[nodiscard]] DocumentData *makeDocument(
		const Track &track,
		const QString &path);
	void startAt(int index);
	void playFile(int index, const QString &path);
	void preload(int index);
	void setupPlayer();
	void resetPlaying();
	void disconnected();
	bool queueMissing(const Playlist &playlist);
	void resumeKeeping();
	void pruneKeepQueue();
	void dropUnneeded(const std::vector<Track> &tracks);
	[[nodiscard]] bool neededMedia(const QString &media);
	void keepNext();
	void keepDone(const QString &media, KeepResult result);

	const not_null<Main::Session*> _session;
	const base::weak_ptr<Cloud::Account> _account;
	const QString _folder;
	Store _store;
	std::unique_ptr<Uploader> _uploader;

	std::vector<Track> _queue;
	Playing _playing;
	DocumentData *_current = nullptr;
	Cloud::TransferId _loading = 0;
	Cloud::TransferId _preloading = 0;
	base::flat_map<QString, uint64> _documents;
	bool _playerReady = false;
	rpl::event_stream<> _playingChanges;

	std::vector<KeepTask> _keepQueue;
	// What the server did not give in this launch.
	base::flat_set<QString> _keepGone;
	Cloud::TransferId _keepTransfer = 0;
	bool _keepBusy = false;
	bool _keepLocal = false; // Busy with a copy on the disk, not a download.
	int _keepFailures = 0;
	int _keepGoneRun = 0; // Refusals of the server in a row.
	base::Timer _keepRetry;
	rpl::event_stream<> _keptChanges;

	rpl::event_stream<Playlist> _updates;
	rpl::lifetime _lifetime;

};

[[nodiscard]] Service &ServiceFor(not_null<Main::Session*> session);

} // namespace Oblivion::Share
