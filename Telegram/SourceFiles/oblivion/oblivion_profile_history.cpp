/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_profile_history.h"

#include "base/timer.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_cloud_file.h"
#include "data/data_file_origin.h"
#include "data/data_folder.h"
#include "data/data_peer.h"
#include "data/data_saved_sublist.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_key.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "oblivion/oblivion_deleted.h"
#include "oblivion/oblivion_interface.h"
#include "oblivion/oblivion_lang.h"
#include "oblivion/oblivion_local_names.h"
#include "oblivion/oblivion_settings.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "settings.h"
#include "storage/file_download.h"
#include "ui/boxes/confirm_box.h"
#include "ui/effects/animation_value.h"
#include "ui/image/image_prepare.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "window/window_session_controller.h"

#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QSaveFile>
#include <QtGui/QImageReader>

#include <deque>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace Oblivion::ProfileHistory {
namespace {

constexpr auto kStateFile = "state.jsonl";
constexpr auto kChangesFile = "changes.jsonl";

// How many profiles are tracked and how many changes are kept. The oldest
// changes are dropped first, together with the photos only they refer to.
constexpr auto kMaxPeers = 10000;
constexpr auto kMaxChanges = 20000;
constexpr auto kMaxChangesPerPeer = 100;
constexpr auto kChangesSlack = 500;
constexpr auto kStateSlack = 1000;

constexpr auto kMaxNameLength = 256;
constexpr auto kMaxAboutLength = 2048;
constexpr auto kMaxUsernames = 32;
constexpr auto kMaxUsernameLength = 64;

constexpr auto kFlushDelay = crl::time(1500);
constexpr auto kFirstScanDelay = crl::time(10) * 1000;
constexpr auto kEnabledScanDelay = crl::time(500);
constexpr auto kScanInterval = crl::time(60) * 1000;

// Photos are loaded one at a time with a pause, most of them come from
// the local cache: the userpics that were already shown in the app.
constexpr auto kPhotoTimeout = crl::time(40) * 1000;
constexpr auto kPhotoPause = crl::time(350);
constexpr auto kPhotoAttempts = 3;
constexpr auto kMaxPhotoQueue = kMaxPeers;
constexpr auto kMaxPhotoBytes = 4 * 1024 * 1024;
constexpr auto kSmallPhotoSide = 160;
constexpr auto kBigPhotoSide = 640;

constexpr auto kPageSize = 40;
constexpr auto kInlineValueLimit = 44;
constexpr auto kThumbSize = 48;
constexpr auto kThumbArrowWidth = 28;

enum class Field : uchar {
	Name,
	Username,
	About,
	Photo,
	Deleted,
};

// The last known state of a profile. A field that is not known yet (the
// bio before the full profile was loaded, the photo hidden by a personal
// one, ...) is never compared and never overwrites a known value.
struct Snapshot {
	QString first; // Users: the first name, chats: the title.
	QString last;
	QStringList usernames; // Active ones, in the profile order.
	QString about;
	uint64 photoId = 0;
	uint64 savedPhoto = 0; // The small photo with this id is on disk.
	int photoDc = 0;
	TimeId since = 0;
	bool nameKnown = false;
	bool usernamesKnown = false;
	bool aboutKnown = false;
	bool photoKnown = false;
	bool deleted = false;

	// The name of a contact is the one from our own address book, the
	// server doesn't send the real one: it is stored, but not compared.
	bool contact = false;

	friend inline bool operator==(
		const Snapshot &,
		const Snapshot &) = default;
};

struct Change {
	uint64 peerId = 0;
	TimeId date = 0;
	Field field = Field::Name;
	QString was;
	QString now;
	uint64 wasPhoto = 0;
	uint64 nowPhoto = 0;
	bool late = false; // Noticed later, happened while not watched.

	friend inline bool operator==(const Change &, const Change &) = default;
};

struct Limits {
	int maxPeers = kMaxPeers;
	int maxChanges = kMaxChanges;
	int maxChangesPerPeer = kMaxChangesPerPeer;
	int changesSlack = kChangesSlack;
	int stateSlack = kStateSlack;
};

struct DiffResult {
	Snapshot merged;
	std::vector<Change> changes;
};

[[nodiscard]] QString FullName(const Snapshot &snapshot) {
	return (snapshot.first + ' ' + snapshot.last).trimmed();
}

[[nodiscard]] QString FieldName(Field field) {
	switch (field) {
	case Field::Name: return u"name"_q;
	case Field::Username: return u"username"_q;
	case Field::About: return u"about"_q;
	case Field::Photo: return u"photo"_q;
	case Field::Deleted: return u"deleted"_q;
	}
	Unexpected("Field in ProfileHistory::FieldName.");
}

[[nodiscard]] std::optional<Field> ParseField(const QString &name) {
	for (const auto field : {
		Field::Name,
		Field::Username,
		Field::About,
		Field::Photo,
		Field::Deleted,
	}) {
		if (FieldName(field) == name) {
			return field;
		}
	}
	return std::nullopt;
}

[[nodiscard]] bool HasDuplicates(const QStringList &list) {
	const auto count = int(list.size());
	for (auto i = 0; i != count; ++i) {
		for (auto j = i + 1; j != count; ++j) {
			if (list[i] == list[j]) {
				return true;
			}
		}
	}
	return false;
}

[[nodiscard]] DiffResult Diff(const Snapshot &was, const Snapshot &now) {
	auto result = DiffResult{ .merged = was };
	auto &merged = result.merged;
	if (now.deleted) {
		if (!was.deleted) {
			merged.deleted = true;
			result.changes.push_back({
				.field = Field::Deleted,
				.was = FullName(was),
			});
		}
		return result;
	}
	merged.deleted = false;
	if (now.nameKnown) {
		const auto old = FullName(was);
		const auto fresh = FullName(now);

		// Adding, renaming or deleting a contact changes the name we see,
		// not the profile: the new name only replaces the stored one.
		const auto ours = was.contact || now.contact;
		if (was.nameKnown && !ours && old != fresh) {
			result.changes.push_back({
				.field = Field::Name,
				.was = old,
				.now = fresh,
			});
		}
		merged.first = now.first;
		merged.last = now.last;
		merged.nameKnown = true;
		merged.contact = now.contact;
	}
	if (now.usernamesKnown) {
		if (was.usernamesKnown && was.usernames != now.usernames) {
			result.changes.push_back({
				.field = Field::Username,
				.was = was.usernames.join(' '),
				.now = now.usernames.join(' '),
			});
		}
		merged.usernames = now.usernames;
		merged.usernamesKnown = true;
	}
	if (now.aboutKnown) {
		if (was.aboutKnown && was.about != now.about) {
			result.changes.push_back({
				.field = Field::About,
				.was = was.about,
				.now = now.about,
			});
		}
		merged.about = now.about;
		merged.aboutKnown = true;
	}
	if (now.photoKnown) {
		if (was.photoKnown && was.photoId != now.photoId) {
			result.changes.push_back({
				.field = Field::Photo,
				.wasPhoto = was.photoId,
				.nowPhoto = now.photoId,
			});
		}
		merged.photoId = now.photoId;
		if (now.photoDc || !now.photoId) {
			merged.photoDc = now.photoDc;
		}
		merged.photoKnown = true;
	}
	return result;
}

[[nodiscard]] QByteArray ToLine(const QJsonObject &object) {
	return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

[[nodiscard]] std::optional<QJsonObject> FromLine(const QByteArray &line) {
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(line, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return std::nullopt;
	}
	return document.object();
}

void WriteId(QJsonObject &object, const QString &key, uint64 value) {
	// As a string: ids don't fit into a JSON double.
	if (value) {
		object.insert(key, QString::number(value));
	}
}

[[nodiscard]] uint64 ReadId(const QJsonObject &object, const QString &key) {
	return object.value(key).toString().toULongLong();
}

enum SnapshotFlag {
	kNameKnown = 0x01,
	kUsernamesKnown = 0x02,
	kAboutKnown = 0x04,
	kPhotoKnown = 0x08,
	kDeleted = 0x10,
	kContact = 0x20,
};

[[nodiscard]] QByteArray SerializeSnapshot(
		uint64 peerId,
		const Snapshot &snapshot) {
	auto object = QJsonObject();
	object.insert(u"p"_q, QString::number(peerId));
	if (!snapshot.first.isEmpty()) {
		object.insert(u"f"_q, snapshot.first);
	}
	if (!snapshot.last.isEmpty()) {
		object.insert(u"l"_q, snapshot.last);
	}
	if (!snapshot.usernames.isEmpty()) {
		object.insert(u"u"_q, QJsonArray::fromStringList(snapshot.usernames));
	}
	if (!snapshot.about.isEmpty()) {
		object.insert(u"a"_q, snapshot.about);
	}
	WriteId(object, u"i"_q, snapshot.photoId);
	WriteId(object, u"s"_q, snapshot.savedPhoto);
	if (snapshot.photoDc) {
		object.insert(u"dc"_q, snapshot.photoDc);
	}
	object.insert(u"t"_q, double(snapshot.since));
	object.insert(u"k"_q, 0
		| (snapshot.nameKnown ? kNameKnown : 0)
		| (snapshot.usernamesKnown ? kUsernamesKnown : 0)
		| (snapshot.aboutKnown ? kAboutKnown : 0)
		| (snapshot.photoKnown ? kPhotoKnown : 0)
		| (snapshot.deleted ? kDeleted : 0)
		| (snapshot.contact ? kContact : 0));
	return ToLine(object);
}

[[nodiscard]] std::optional<std::pair<uint64, Snapshot>> ParseSnapshot(
		const QByteArray &line) {
	const auto parsed = FromLine(line);
	if (!parsed) {
		return std::nullopt;
	}
	const auto &object = *parsed;
	const auto peerId = ReadId(object, u"p"_q);
	if (!peerId || !object.contains(u"k"_q)) {
		return std::nullopt;
	}
	auto result = Snapshot();
	result.first = object.value(u"f"_q).toString().left(kMaxNameLength);
	result.last = object.value(u"l"_q).toString().left(kMaxNameLength);
	for (const auto &value : object.value(u"u"_q).toArray()) {
		const auto username = value.toString().left(kMaxUsernameLength);
		if (!username.isEmpty()
			&& !username.contains(' ')
			&& result.usernames.size() < kMaxUsernames) {
			result.usernames.push_back(username);
		}
	}
	result.about = object.value(u"a"_q).toString().left(kMaxAboutLength);
	result.photoId = ReadId(object, u"i"_q);
	result.savedPhoto = ReadId(object, u"s"_q);
	result.photoDc = object.value(u"dc"_q).toInt();
	result.since = TimeId(object.value(u"t"_q).toDouble());
	const auto flags = object.value(u"k"_q).toInt();
	result.nameKnown = (flags & kNameKnown);
	result.usernamesKnown = (flags & kUsernamesKnown);
	result.aboutKnown = (flags & kAboutKnown);
	result.photoKnown = (flags & kPhotoKnown);
	result.deleted = (flags & kDeleted);
	result.contact = (flags & kContact);
	return std::make_pair(peerId, std::move(result));
}

[[nodiscard]] QByteArray SerializeChange(const Change &change) {
	auto object = QJsonObject();
	object.insert(u"p"_q, QString::number(change.peerId));
	object.insert(u"t"_q, double(change.date));
	object.insert(u"k"_q, FieldName(change.field));
	if (!change.was.isEmpty()) {
		object.insert(u"w"_q, change.was);
	}
	if (!change.now.isEmpty()) {
		object.insert(u"n"_q, change.now);
	}
	WriteId(object, u"wi"_q, change.wasPhoto);
	WriteId(object, u"ni"_q, change.nowPhoto);
	if (change.late) {
		object.insert(u"late"_q, true);
	}
	return ToLine(object);
}

[[nodiscard]] std::optional<Change> ParseChange(const QByteArray &line) {
	const auto parsed = FromLine(line);
	if (!parsed) {
		return std::nullopt;
	}
	const auto &object = *parsed;
	const auto field = ParseField(object.value(u"k"_q).toString());
	auto result = Change();
	result.peerId = ReadId(object, u"p"_q);
	result.date = TimeId(object.value(u"t"_q).toDouble());
	if (!result.peerId || !field || result.date <= 0) {
		return std::nullopt;
	}
	result.field = *field;
	result.was = object.value(u"w"_q).toString().left(kMaxAboutLength);
	result.now = object.value(u"n"_q).toString().left(kMaxAboutLength);
	result.wasPhoto = ReadId(object, u"wi"_q);
	result.nowPhoto = ReadId(object, u"ni"_q);
	result.late = object.value(u"late"_q).toBool();
	return result;
}

// A list with a repeated username is never a real one, see Capture().
// The changes recorded from such lists before they were skipped are
// dropped when the file is read.
[[nodiscard]] bool FalseUsernameChange(const Change &change) {
	return (change.field == Field::Username)
		&& (HasDuplicates(change.was.split(' ', Qt::SkipEmptyParts))
			|| HasDuplicates(change.now.split(' ', Qt::SkipEmptyParts)));
}

[[nodiscard]] crl::queue &IoQueue() {
	static const auto result = new crl::queue();
	return *result;
}

struct IoBatch {
	QString folder;
	std::optional<QByteArray> stateFull;
	QByteArray stateAppend;
	std::optional<QByteArray> changesFull;
	QByteArray changesAppend;
	QStringList removeFiles;
	bool removeAll = false;

	[[nodiscard]] bool empty() const {
		return !removeAll
			&& !stateFull
			&& !changesFull
			&& stateAppend.isEmpty()
			&& changesAppend.isEmpty()
			&& removeFiles.isEmpty();
	}
};

void WriteFull(const QString &path, const QByteArray &bytes) {
	if (bytes.isEmpty()) {
		QFile::remove(path);
		return;
	}
	auto file = QSaveFile(path);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(bytes);
		file.commit();
	}
}

void WriteAppend(const QString &path, const QByteArray &bytes) {
	if (bytes.isEmpty()) {
		return;
	}
	auto file = QFile(path);
	if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
		file.write(bytes);
		file.close();
	}
}

void Perform(const IoBatch &batch) {
	if (batch.removeAll) {
		QDir(batch.folder).removeRecursively();
	}
	for (const auto &path : batch.removeFiles) {
		QFile::remove(path);
	}
	if (!batch.stateFull
		&& !batch.changesFull
		&& batch.stateAppend.isEmpty()
		&& batch.changesAppend.isEmpty()) {
		return;
	}
	QDir().mkpath(batch.folder);
	const auto state = batch.folder + QString::fromLatin1(kStateFile);
	const auto changes = batch.folder + QString::fromLatin1(kChangesFile);
	if (batch.stateFull) {
		WriteFull(state, *batch.stateFull);
	}
	WriteAppend(state, batch.stateAppend);
	if (batch.changesFull) {
		WriteFull(changes, *batch.changesFull);
	}
	WriteAppend(changes, batch.changesAppend);
}

// Telegram photos are JPEG files, they are stored as they came. Anything
// else that still decodes as an image is converted to a JPEG.
[[nodiscard]] bool WritePhotoFile(
		const QString &path,
		const QByteArray &bytes) {
	if (bytes.isEmpty() || bytes.size() > kMaxPhotoBytes) {
		return false;
	}
	auto data = bytes;
	if (!bytes.startsWith("\xFF\xD8")) {
		auto image = QImage::fromData(bytes);
		if (image.isNull()) {
			return false;
		}
		if (image.hasAlphaChannel()) {
			auto opaque = QImage(image.size(), QImage::Format_RGB32);
			opaque.fill(Qt::white);
			auto p = QPainter(&opaque);
			p.drawImage(0, 0, image);
			p.end();
			image = std::move(opaque);
		}
		data = QByteArray();
		auto buffer = QBuffer(&data);
		if (!buffer.open(QIODevice::WriteOnly)
			|| !image.save(&buffer, "JPG", 90)) {
			return false;
		}
	}
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QSaveFile(path);
	return file.open(QIODevice::WriteOnly)
		&& (file.write(data) == data.size())
		&& file.commit();
}

// A big photo that is not larger than the small one is not kept: either
// the original is that small or the small copy was received instead.
[[nodiscard]] bool LargerThanSmallPhoto(const QByteArray &bytes) {
	auto data = bytes;
	auto buffer = QBuffer(&data);
	if (!buffer.open(QIODevice::ReadOnly)) {
		return false;
	}
	const auto size = QImageReader(&buffer).size();
	return std::max(size.width(), size.height()) > kSmallPhotoSide;
}

// The profiles and the changes of one account. Everything is kept in
// memory, the files are read once and then only written: batched, on the
// IoQueue(), see flush().
class Store final {
public:
	explicit Store(QString folder, Limits limits = {});

	[[nodiscard]] QString photoPath(
		uint64 peerId,
		uint64 photoId,
		bool big) const;

	[[nodiscard]] std::optional<Snapshot> baseline(uint64 peerId);
	[[nodiscard]] bool tracked(uint64 peerId);
	[[nodiscard]] int peersCount();
	[[nodiscard]] int changesCount();

	// The first snapshot of a profile is only remembered. The bio is
	// loaded separately from the rest, much later or never, so whether
	// its change was noticed late is passed separately as well.
	std::vector<Change> apply(
		uint64 peerId,
		const Snapshot &now,
		TimeId date,
		bool late = false,
		bool force = false,
		bool lateAbout = false);
	[[nodiscard]] bool hasChanges(uint64 peerId);
	[[nodiscard]] std::vector<Change> changes(uint64 peerId);

	void setSavedPhoto(uint64 peerId, uint64 photoId);
	void notify(uint64 peerId);
	void clear(uint64 peerId);
	void forget();

	void flush(bool sync = false);
	[[nodiscard]] rpl::producer<> dirtied() const;
	[[nodiscard]] rpl::producer<uint64> updates() const;

private:
	void ensureLoaded();
	void loadState();
	void loadChanges();
	void writeState(uint64 peerId, const Snapshot &snapshot);
	void trimPeer(uint64 peerId, std::vector<Change> &list);
	void trimTotal();
	void dropPhotos(uint64 peerId, const std::vector<Change> &removed);
	void checkCompact();
	void markDirty();
	[[nodiscard]] QByteArray serializeState();
	[[nodiscard]] QByteArray serializeChanges();

	const QString _folder;
	const Limits _limits;

	std::unordered_map<uint64, Snapshot> _state;
	std::unordered_map<uint64, std::vector<Change>> _changes;
	int _total = 0;
	int _stateLines = 0;
	int _changesLines = 0;
	bool _loaded = false;

	QByteArray _stateAppend;
	QByteArray _changesAppend;
	QStringList _removeFiles;
	bool _stateRewrite = false;
	bool _changesRewrite = false;
	bool _removeAll = false;
	bool _dirty = false;

	rpl::event_stream<> _dirtied;
	rpl::event_stream<uint64> _updates;

};

Store::Store(QString folder, Limits limits)
: _folder(std::move(folder))
, _limits(limits) {
}

QString Store::photoPath(uint64 peerId, uint64 photoId, bool big) const {
	return _folder
		+ QString::number(peerId)
		+ '/'
		+ QString::number(photoId)
		+ (big ? u"_big.jpg"_q : u".jpg"_q);
}

void Store::ensureLoaded() {
	if (_loaded) {
		return;
	}
	_loaded = true;
	loadState();
	loadChanges();
}

void Store::loadState() {
	auto file = QFile(_folder + QString::fromLatin1(kStateFile));
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto content = file.readAll();
	file.close();
	auto broken = !content.isEmpty() && !content.endsWith('\n');
	for (const auto &line : content.split('\n')) {
		if (line.trimmed().isEmpty()) {
			continue;
		}
		++_stateLines;
		auto parsed = ParseSnapshot(line);
		if (!parsed) {
			broken = true;
			continue;
		}
		_state[parsed->first] = std::move(parsed->second);
	}
	if (broken || (_stateLines > int(_state.size()) + _limits.stateSlack)) {
		_stateRewrite = true;
		markDirty();
	}
}

void Store::loadChanges() {
	auto file = QFile(_folder + QString::fromLatin1(kChangesFile));
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto content = file.readAll();
	file.close();
	auto broken = !content.isEmpty() && !content.endsWith('\n');
	for (const auto &line : content.split('\n')) {
		if (line.trimmed().isEmpty()) {
			continue;
		}
		++_changesLines;
		auto parsed = ParseChange(line);
		if (!parsed || FalseUsernameChange(*parsed)) {
			broken = true;
			continue;
		}
		_changes[parsed->peerId].push_back(std::move(*parsed));
		++_total;
	}
	for (auto &[peerId, list] : _changes) {
		trimPeer(peerId, list);
	}
	trimTotal();
	if (broken) {
		_changesRewrite = true;
		markDirty();
	}
	checkCompact();
}

void Store::markDirty() {
	if (!_dirty) {
		_dirty = true;
		_dirtied.fire({});
	}
}

void Store::writeState(uint64 peerId, const Snapshot &snapshot) {
	_stateAppend.append(SerializeSnapshot(peerId, snapshot));
	++_stateLines;
	if (_stateLines > int(_state.size()) + 1 + _limits.stateSlack) {
		_stateRewrite = true;
	}
	markDirty();
}

void Store::dropPhotos(uint64 peerId, const std::vector<Change> &removed) {
	auto candidates = base::flat_set<uint64>();
	for (const auto &change : removed) {
		if (change.field == Field::Photo) {
			candidates.emplace(change.wasPhoto);
			candidates.emplace(change.nowPhoto);
		}
	}
	candidates.remove(0);
	if (candidates.empty()) {
		return;
	}
	const auto current = _state.find(peerId);
	if (current != end(_state)) {
		candidates.remove(current->second.photoId);
		candidates.remove(current->second.savedPhoto);
	}
	const auto remaining = _changes.find(peerId);
	if (remaining != end(_changes)) {
		for (const auto &change : remaining->second) {
			candidates.remove(change.wasPhoto);
			candidates.remove(change.nowPhoto);
		}
	}
	for (const auto photoId : candidates) {
		_removeFiles.push_back(photoPath(peerId, photoId, false));
		_removeFiles.push_back(photoPath(peerId, photoId, true));
	}
	if (!candidates.empty()) {
		markDirty();
	}
}

void Store::trimPeer(uint64 peerId, std::vector<Change> &list) {
	const auto extra = int(list.size()) - _limits.maxChangesPerPeer;
	if (extra <= 0) {
		return;
	}
	const auto removed = std::vector<Change>(
		begin(list),
		begin(list) + extra);
	list.erase(begin(list), begin(list) + extra);
	_total -= extra;
	dropPhotos(peerId, removed);
}

void Store::trimTotal() {
	if (_total <= _limits.maxChanges + _limits.changesSlack) {
		return;
	}
	auto dates = std::vector<TimeId>();
	dates.reserve(_total);
	for (const auto &[peerId, list] : _changes) {
		for (const auto &change : list) {
			dates.push_back(change.date);
		}
	}
	const auto remove = int(dates.size()) - _limits.maxChanges;
	if (remove <= 0) {
		return;
	}
	std::nth_element(begin(dates), begin(dates) + (remove - 1), end(dates));
	const auto cutoff = dates[remove - 1];
	const auto older = int(std::count_if(
		begin(dates),
		end(dates),
		[&](TimeId date) { return date < cutoff; }));

	// Everything older than the cutoff and as many of the changes made
	// exactly at the cutoff as needed.
	auto sameLeft = remove - older;
	for (auto i = begin(_changes); i != end(_changes);) {
		auto &list = i->second;
		auto removed = std::vector<Change>();
		const auto from = std::remove_if(
			begin(list),
			end(list),
			[&](const Change &change) {
				if (change.date > cutoff) {
					return false;
				} else if (change.date == cutoff) {
					if (sameLeft <= 0) {
						return false;
					}
					--sameLeft;
				}
				removed.push_back(change);
				return true;
			});
		list.erase(from, end(list));
		_total -= int(removed.size());
		dropPhotos(i->first, removed);
		if (list.empty()) {
			i = _changes.erase(i);
		} else {
			++i;
		}
	}
}

void Store::checkCompact() {
	if (_changesLines - _total > _limits.changesSlack) {
		_changesRewrite = true;
		markDirty();
	}
}

std::optional<Snapshot> Store::baseline(uint64 peerId) {
	ensureLoaded();
	const auto i = _state.find(peerId);
	return (i != end(_state)) ? i->second : std::optional<Snapshot>();
}

bool Store::tracked(uint64 peerId) {
	ensureLoaded();
	return _state.find(peerId) != end(_state);
}

int Store::peersCount() {
	ensureLoaded();
	return int(_state.size());
}

int Store::changesCount() {
	ensureLoaded();
	return _total;
}

std::vector<Change> Store::apply(
		uint64 peerId,
		const Snapshot &now,
		TimeId date,
		bool late,
		bool force,
		bool lateAbout) {
	ensureLoaded();
	if (!peerId) {
		return {};
	}
	const auto i = _state.find(peerId);
	if (i == end(_state)) {
		if (now.deleted
			|| (!force && int(_state.size()) >= _limits.maxPeers)) {
			return {};
		}
		auto snapshot = now;
		snapshot.since = date;
		snapshot.savedPhoto = 0;
		writeState(peerId, snapshot);
		_state.emplace(peerId, std::move(snapshot));
		_updates.fire_copy(peerId);
		return {};
	}
	auto diff = Diff(i->second, now);

	// What an open history box shows besides the changes themselves.
	const auto shown = (diff.merged.contact != i->second.contact)
		|| (diff.merged.deleted != i->second.deleted);
	if (diff.merged != i->second) {
		i->second = diff.merged;
		writeState(peerId, i->second);
	}
	if (diff.changes.empty()) {
		if (shown) {
			_updates.fire_copy(peerId);
		}
		return {};
	}
	auto &list = _changes[peerId];
	for (auto &change : diff.changes) {
		change.peerId = peerId;
		change.date = date;
		change.late = late
			|| (lateAbout && change.field == Field::About);
		_changesAppend.append(SerializeChange(change));
		++_changesLines;
		list.push_back(change);
		++_total;
	}
	trimPeer(peerId, list);
	trimTotal();
	checkCompact();
	markDirty();
	_updates.fire_copy(peerId);
	return std::move(diff.changes);
}

bool Store::hasChanges(uint64 peerId) {
	ensureLoaded();
	const auto i = _changes.find(peerId);
	return (i != end(_changes)) && !i->second.empty();
}

std::vector<Change> Store::changes(uint64 peerId) {
	ensureLoaded();
	const auto i = _changes.find(peerId);
	return (i != end(_changes)) ? i->second : std::vector<Change>();
}

void Store::setSavedPhoto(uint64 peerId, uint64 photoId) {
	ensureLoaded();
	const auto i = _state.find(peerId);
	if (i != end(_state) && i->second.savedPhoto != photoId) {
		i->second.savedPhoto = photoId;
		writeState(peerId, i->second);
	}
}

void Store::notify(uint64 peerId) {
	_updates.fire_copy(peerId);
}

void Store::clear(uint64 peerId) {
	ensureLoaded();
	const auto i = _changes.find(peerId);
	if (i == end(_changes)) {
		return;
	}
	const auto removed = base::take(i->second);
	_total -= int(removed.size());
	_changes.erase(i);
	dropPhotos(peerId, removed);
	_changesRewrite = true;
	markDirty();
	_updates.fire_copy(peerId);
}

void Store::forget() {
	_state.clear();
	_changes.clear();
	_total = _stateLines = _changesLines = 0;
	_stateAppend.clear();
	_changesAppend.clear();
	_removeFiles.clear();
	_stateRewrite = _changesRewrite = false;

	// Treat as loaded, so the files being removed are never read back.
	_loaded = true;
	_removeAll = true;
	flush();
	_updates.fire(0);
}

QByteArray Store::serializeState() {
	auto result = QByteArray();
	for (const auto &[peerId, snapshot] : _state) {
		result.append(SerializeSnapshot(peerId, snapshot));
	}
	_stateLines = int(_state.size());
	return result;
}

QByteArray Store::serializeChanges() {
	auto result = QByteArray();
	for (const auto &[peerId, list] : _changes) {
		for (const auto &change : list) {
			result.append(SerializeChange(change));
		}
	}
	_changesLines = _total;
	return result;
}

void Store::flush(bool sync) {
	auto batch = IoBatch{ .folder = _folder };
	batch.removeAll = base::take(_removeAll);
	batch.removeFiles = base::take(_removeFiles);
	if (base::take(_stateRewrite)) {
		_stateAppend.clear();
		batch.stateFull = serializeState();
	} else {
		batch.stateAppend = base::take(_stateAppend);
	}
	if (base::take(_changesRewrite)) {
		_changesAppend.clear();
		batch.changesFull = serializeChanges();
	} else {
		batch.changesAppend = base::take(_changesAppend);
	}
	_dirty = false;
	if (sync) {
		IoQueue().sync([&] {
			Perform(batch);
		});
	} else if (!batch.empty()) {
		IoQueue().async([batch = std::move(batch)] {
			Perform(batch);
		});
	}
}

rpl::producer<> Store::dirtied() const {
	return _dirtied.events();
}

rpl::producer<uint64> Store::updates() const {
	return _updates.events();
}

struct ViewPhoto {
	QString path; // Empty: the photo was not saved.
	QString open; // The best saved version to open.
	QImage image; // Used instead of the file, for the snapshot scenes.

	[[nodiscard]] bool empty() const {
		return path.isEmpty() && image.isNull();
	}
};

struct ViewChange {
	Field field = Field::Name;
	QString was;
	QString now;
	bool hadPhoto = false;
	bool hasPhoto = false;
	ViewPhoto wasPhoto;
	ViewPhoto nowPhoto;
};

struct ViewGroup {
	TimeId date = 0;
	bool late = false;
	std::vector<ViewChange> changes;
};

struct ViewData {
	QString name;
	bool user = true;
	bool enabled = true;
	bool contact = false;
	bool deleted = false;
	TimeId since = 0; // Zero: the profile is not remembered yet.
	std::vector<ViewGroup> groups; // The newest first.
};

struct BoxArgs {
	std::shared_ptr<Ui::Show> show;
	rpl::producer<ViewData> data;
	Fn<void()> clear;
};

[[nodiscard]] QString Arrow() {
	return QString::fromUtf8(" \xE2\x86\x92 ");
}

[[nodiscard]] QString FormatDate(TimeId date) {
	const auto russian = CurrentLanguageIsRussian();
	const auto locale = QLocale(russian
		? QLocale::Russian
		: QLocale::English);
	const auto parsed = base::unixtime::parse(date);
	return locale.toString(
		parsed.date(),
		russian ? u"d MMMM yyyy"_q : u"MMMM d, yyyy"_q
	) + u", "_q + FormatMessageTime(parsed.time());
}

[[nodiscard]] QString FormatUsernames(const QString &stored) {
	auto result = QStringList();
	for (const auto &username : stored.split(' ', Qt::SkipEmptyParts)) {
		result.push_back('@' + username);
	}
	return result.join(u", "_q);
}

[[nodiscard]] QString CaptionText(const ViewChange &change, bool user) {
	switch (change.field) {
	case Field::Name:
		return (user
			? tr::lng_oblivion_profile_history_name
			: tr::lng_oblivion_profile_history_chat_title)(tr::now);
	case Field::Username:
		return (user
			? tr::lng_oblivion_profile_history_username
			: tr::lng_oblivion_profile_history_public_link)(tr::now);
	case Field::About:
		return (user
			? tr::lng_oblivion_profile_history_bio
			: tr::lng_oblivion_profile_history_description)(tr::now);
	case Field::Photo:
		return (!change.hasPhoto
			? tr::lng_oblivion_profile_history_photo_removed
			: !change.hadPhoto
			? tr::lng_oblivion_profile_history_photo_set
			: tr::lng_oblivion_profile_history_photo_changed)(tr::now);
	case Field::Deleted:
		return tr::lng_oblivion_profile_history_deleted(tr::now);
	}
	Unexpected("Field in ProfileHistory::CaptionText.");
}

[[nodiscard]] int RowTextLeft();

// The width of the texts in a row, see Row::resizeGetHeight().
[[nodiscard]] int RowTextWidth() {
	return st::boxWideWidth - RowTextLeft() - st::boxRowPadding.right();
}

[[nodiscard]] TextWithEntities ValueText(const ViewChange &change) {
	if (change.field == Field::Deleted) {
		return tr::marked(change.was);
	}
	const auto usernames = (change.field == Field::Username);
	const auto was = usernames ? FormatUsernames(change.was) : change.was;
	const auto now = usernames ? FormatUsernames(change.now) : change.now;
	const auto wrap = [](const QString &value) {
		return value.isEmpty()
			? tr::italic(tr::lng_oblivion_profile_history_none(tr::now))
			: tr::marked(value);
	};
	// "Was -> now" only while it fits in one line of the row, a line
	// broken somewhere inside of it is harder to read than two lines.
	const auto none = tr::lng_oblivion_profile_history_none(tr::now);
	const auto inlined = (was.isEmpty() ? none : was)
		+ Arrow()
		+ (now.isEmpty() ? none : now);
	const auto simple = (was.size() + now.size() <= kInlineValueLimit)
		&& !was.contains('\n')
		&& !now.contains('\n')
		&& (st::normalFont->width(inlined) <= RowTextWidth());
	auto result = tr::marked();
	if (simple) {
		result.append(wrap(was)).append(Arrow()).append(wrap(now));
	} else {
		result.append(
			tr::bold(tr::lng_oblivion_profile_history_was(tr::now))
		).append(' ').append(wrap(was)).append('\n').append(
			tr::bold(tr::lng_oblivion_profile_history_now(tr::now))
		).append(' ').append(wrap(now));
	}
	return result;
}

[[nodiscard]] QImage SquareThumb(QImage image, int side) {
	if (image.isNull() || side <= 0) {
		return QImage();
	}
	image = image.scaled(
		side,
		side,
		Qt::KeepAspectRatioByExpanding,
		Qt::SmoothTransformation);
	if (image.width() != side || image.height() != side) {
		image = image.copy(
			(image.width() - side) / 2,
			(image.height() - side) / 2,
			side,
			side);
	}
	return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

class Thumb final : public Ui::AbstractButton {
public:
	Thumb(
		QWidget *parent,
		const ViewPhoto &photo,
		int size,
		Fn<void(QString)> open);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	void setSquare(QImage image);

	const int _size = 0;
	QImage _image;

};

Thumb::Thumb(
	QWidget *parent,
	const ViewPhoto &photo,
	int size,
	Fn<void(QString)> open)
: AbstractButton(parent)
, _size(size) {
	resize(size, size);
	const auto path = photo.open.isEmpty() ? photo.path : photo.open;
	if (open && !path.isEmpty()) {
		setClickedCallback([=] {
			open(path);
		});
	} else {
		setPointerCursor(false);
	}
	const auto side = size * style::DevicePixelRatio();
	if (!photo.image.isNull()) {
		setSquare(SquareThumb(photo.image, side));
	} else if (!photo.path.isEmpty()) {
		const auto file = photo.path;
		crl::async([=, weak = base::make_weak(this)] {
			auto image = SquareThumb(QImage(file), side);
			crl::on_main(weak, [=, image = std::move(image)]() mutable {
				weak->setSquare(std::move(image));
			});
		});
	}
}

void Thumb::setSquare(QImage image) {
	if (image.isNull()) {
		return;
	}
	_image = Images::Circle(std::move(image));
	_image.setDevicePixelRatio(style::DevicePixelRatio());
	update();
}

void Thumb::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	if (!_image.isNull()) {
		p.drawImage(QRect(0, 0, _size, _size), _image);
		return;
	}
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(st::windowBgOver);
	p.drawEllipse(0, 0, _size, _size);
}

class PhotoStrip final : public Ui::RpWidget {
public:
	PhotoStrip(
		QWidget *parent,
		const ViewChange &change,
		Fn<void(QString)> open);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	QRect _arrow;

};

PhotoStrip::PhotoStrip(
	QWidget *parent,
	const ViewChange &change,
	Fn<void(QString)> open)
: RpWidget(parent) {
	const auto size = style::ConvertScale(kThumbSize);
	auto left = 0;
	const auto add = [&](const ViewPhoto &photo) {
		const auto thumb = Ui::CreateChild<Thumb>(this, photo, size, open);
		thumb->move(left, 0);
		left += size;
	};
	if (!change.wasPhoto.empty()) {
		add(change.wasPhoto);
	}
	if (!change.nowPhoto.empty()) {
		if (left > 0) {
			const auto width = style::ConvertScale(kThumbArrowWidth);
			_arrow = QRect(left, 0, width, size);
			left += width;
		}
		add(change.nowPhoto);
	}
	resize(left, size);
	setNaturalWidth(left);
}

void PhotoStrip::paintEvent(QPaintEvent *e) {
	if (_arrow.isEmpty()) {
		return;
	}
	// Painted, not a font glyph: Open Sans has no arrows, the one from
	// a fallback font is too thin and small between two userpics.
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto center = QRectF(_arrow).center();
	const auto half = style::ConvertScaleExact(6.);
	const auto head = style::ConvertScaleExact(4.);
	const auto from = QPointF(center.x() - half, center.y());
	const auto tip = QPointF(center.x() + half, center.y());
	auto pen = QPen(st::windowSubTextFg->c);
	pen.setWidthF(style::ConvertScaleExact(1.5));
	pen.setCapStyle(Qt::RoundCap);
	pen.setJoinStyle(Qt::RoundJoin);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	p.drawLine(from, tip);
	p.drawLine(QPointF(tip.x() - head, tip.y() - head), tip);
	p.drawLine(QPointF(tip.x() - head, tip.y() + head), tip);
}

// FlatLabel styles with a zero minWidth (the default label styles) are
// for single lines only: a text that doesn't fit is painted wrapped, but
// its height is counted as if it was not, so the last lines are cut off.
// Every text here may wrap (a long name, a bio, a list of usernames), so
// wrapping styles are used: this one for the main texts and
// st::boxDividerLabel for the gray ones.
[[nodiscard]] const style::FlatLabel &TextLabelStyle() {
	static const auto result = [] {
		auto result = st::defaultFlatLabel;
		result.minWidth = st::boxDividerLabel.minWidth;
		return result;
	}();
	return result;
}

[[nodiscard]] int RowTop() {
	return style::ConvertScale(8);
}

[[nodiscard]] int RowBottom() {
	return style::ConvertScale(8);
}

[[nodiscard]] int TimelineDotRadius() {
	return style::ConvertScale(4);
}

[[nodiscard]] int TimelineLeft() {
	return st::boxRowPadding.left() + TimelineDotRadius();
}

[[nodiscard]] int RowTextLeft() {
	return st::boxRowPadding.left() + style::ConvertScale(22);
}

class Row final : public Ui::RpWidget {
public:
	Row(
		QWidget *parent,
		const ViewGroup &group,
		bool user,
		bool first,
		bool last,
		Fn<void(QString)> open);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;

private:
	const not_null<Ui::VerticalLayout*> _content;
	const bool _first = false;
	const bool _last = false;

};

Row::Row(
	QWidget *parent,
	const ViewGroup &group,
	bool user,
	bool first,
	bool last,
	Fn<void(QString)> open)
: RpWidget(parent)
, _content(Ui::CreateChild<Ui::VerticalLayout>(this))
, _first(first)
, _last(last) {
	const auto skip = style::ConvertScale(6);
	const auto small = style::ConvertScale(2);
	const auto date = FormatDate(group.date);
	_content->add(object_ptr<Ui::FlatLabel>(
		_content,
		(group.late
			? tr::lng_oblivion_profile_history_noticed(
				tr::now,
				lt_date,
				date)
			: date),
		st::boxDividerLabel));
	for (const auto &change : group.changes) {
		_content->add(
			object_ptr<Ui::FlatLabel>(
				_content,
				rpl::single(tr::bold(CaptionText(change, user))),
				TextLabelStyle()),
			style::margins(0, skip, 0, 0));
		if (change.field == Field::Photo) {
			if (!change.wasPhoto.empty() || !change.nowPhoto.empty()) {
				_content->add(
					object_ptr<PhotoStrip>(_content, change, open),
					style::margins(0, skip, 0, small),
					style::al_left);
			}
			if (change.hadPhoto && change.wasPhoto.empty()) {
				_content->add(
					object_ptr<Ui::FlatLabel>(
						_content,
						tr::lng_oblivion_profile_history_photo_missing(
							tr::now),
						st::boxDividerLabel),
					style::margins(0, small, 0, 0));
			}
			continue;
		}
		auto text = ValueText(change);
		if (text.text.isEmpty()) {
			continue;
		}
		const auto value = _content->add(
			object_ptr<Ui::FlatLabel>(_content, TextLabelStyle()),
			style::margins(0, small, 0, 0));
		value->setMarkedText(text);
		value->setSelectable(true);
	}
	_content->heightValue(
	) | rpl::on_next([=](int height) {
		const auto wanted = RowTop() + height + RowBottom();
		if (width() > 0 && this->height() != wanted) {
			resize(width(), wanted);
		}
	}, lifetime());
}

int Row::resizeGetHeight(int newWidth) {
	const auto left = RowTextLeft();
	const auto available = std::max(
		newWidth - left - st::boxRowPadding.right(),
		1);
	_content->resizeToWidth(available);
	_content->moveToLeft(left, RowTop(), newWidth);
	return RowTop() + _content->height() + RowBottom();
}

void Row::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto x = float64(TimelineLeft());
	const auto radius = float64(TimelineDotRadius());
	const auto line = float64(style::ConvertScale(2));
	const auto dot = RowTop()
		+ (st::boxDividerLabel.style.font->height / 2.);
	const auto top = _first ? dot : 0.;
	const auto bottom = _last ? dot : float64(height());
	p.setPen(Qt::NoPen);
	if (bottom > top) {
		p.setBrush(anim::with_alpha(st::windowSubTextFg->c, 0.3));
		p.drawRect(QRectF(x - line / 2., top, line, bottom - top));
	}
	p.setBrush(st::boxBg);
	p.drawEllipse(QPointF(x, dot), radius + line, radius + line);
	p.setBrush(st::windowBgActive);
	p.drawEllipse(QPointF(x, dot), radius, radius);
}

void FillHeader(
		not_null<Ui::VerticalLayout*> header,
		const ViewData &data) {
	const auto &padding = st::boxRowPadding;
	const auto skip = st::boxLittleSkip;
	header->clear();
	if (!data.name.isEmpty()) {
		header->add(
			object_ptr<Ui::FlatLabel>(
				header,
				rpl::single(tr::bold(data.name)),
				TextLabelStyle()),
			padding);
	}
	auto status = QStringList();
	if (!data.enabled) {
		status.push_back(tr::lng_oblivion_profile_history_off(tr::now));
	} else {
		if (data.since) {
			status.push_back(tr::lng_oblivion_profile_history_since(
				tr::now,
				lt_date,
				FormatDate(data.since)));
		}
		if (data.contact && !data.deleted) {
			status.push_back(
				tr::lng_oblivion_profile_history_contact(tr::now));
		}
	}
	if (!status.isEmpty()) {
		header->add(
			object_ptr<Ui::FlatLabel>(
				header,
				status.join('\n'),
				st::boxDividerLabel),
			padding + style::margins(0, skip / 4, 0, 0));
	}
	if (!data.groups.empty()) {
		Ui::AddSkip(header, skip);
		return;
	}
	auto text = tr::bold(tr::lng_oblivion_profile_history_empty(tr::now));
	if (data.enabled) {
		// "Remembered" only when it is: a profile restored from the local
		// cache waits for its fresh data, a deleted account never gets any.
		text.append(u"\n\n"_q).append((data.deleted
			? tr::lng_oblivion_profile_history_empty_deleted
			: data.since
			? tr::lng_oblivion_profile_history_empty_about
			: tr::lng_oblivion_profile_history_empty_waiting)(tr::now));
	}
	header->add(
		object_ptr<Ui::FlatLabel>(
			header,
			rpl::single(std::move(text)),
			st::membersAbout),
		// The box buttons add their own padding below: with a smaller
		// bottom margin the text looks centered between the header and
		// the buttons.
		style::margins(
			padding.left(),
			st::boxMediumSkip * 2,
			padding.right(),
			st::boxMediumSkip + (st::boxLittleSkip / 2)),
		style::al_top);
}

void HistoryBox(not_null<Ui::GenericBox*> box, BoxArgs &&args) {
	struct State {
		ViewData data;
		int shown = 0;
		bool adding = false;
		bool scrollReady = false;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto show = args.show;

	box->setWidth(st::boxWideWidth);
	box->setMaxHeight(st::boxMaxListHeight);
	box->setTitle(tr::lng_oblivion_profile_history_title());

	const auto content = box->verticalLayout();
	// Justified: a left aligned layout shrinks to the natural width of
	// its rows, with only short lines in the header (tracking is off) the
	// centered empty state was centered in that width, not in the box.
	const auto header = content->add(
		object_ptr<Ui::VerticalLayout>(content),
		style::al_justify);
	const auto rows = content->add(object_ptr<Ui::VerticalLayout>(content));
	const auto footer = content->add(object_ptr<Ui::VerticalLayout>(content));

	const auto open = [=](QString path) {
		if (show && QFile::exists(path)) {
			OpenSavedFile(show, path);
		}
	};
	const auto showMore = [=] {
		const auto &data = state->data;
		const auto count = int(data.groups.size());
		const auto till = std::min(state->shown + kPageSize, count);
		state->adding = true;
		for (auto i = state->shown; i < till; ++i) {
			rows->add(object_ptr<Row>(
				rows,
				data.groups[i],
				data.user,
				(i == 0),
				(i + 1 == count),
				open));
		}
		state->shown = till;
		state->adding = false;
	};
	const auto refill = [=] {
		// The rows are rebuilt in place: the same amount of them is
		// shown again and the scroll position is put back, so a change
		// that arrives while the box is open doesn't throw the reader
		// back to the top of the list.
		const auto top = state->scrollReady ? box->scrollTop() : 0;
		const auto count = int(state->data.groups.size());
		const auto wanted = std::clamp(
			state->shown,
			std::min(kPageSize, count),
			count);
		FillHeader(header, state->data);
		rows->clear();
		footer->clear();
		state->shown = 0;
		while (state->shown < wanted) {
			showMore();
		}
		if (count > 0) {
			const auto skip = st::boxLittleSkip;
			footer->add(
				object_ptr<Ui::FlatLabel>(
					footer,
					tr::lng_oblivion_profile_history_about(tr::now),
					st::boxDividerLabel),
				st::boxRowPadding + style::margins(0, skip, 0, skip));
		}
		if (top > 0) {
			box->scrollToY(top);
		}
	};

	box->setInitScrollCallback([=] {
		state->scrollReady = true;
		box->scrolls() | rpl::on_next([=] {
			if (state->adding
				|| state->shown >= int(state->data.groups.size())) {
				return;
			}
			const auto visible = box->scrollHeight();
			if (box->scrollTop() + 2 * visible >= content->height()) {
				showMore();
			}
		}, box->lifetime());
	});

	std::move(
		args.data
	) | rpl::on_next([=](ViewData &&data) {
		state->data = std::move(data);
		refill();
	}, box->lifetime());

	if (args.clear && !state->data.groups.empty()) {
		const auto clear = args.clear;
		box->addLeftButton(tr::lng_oblivion_profile_history_clear(), [=] {
			if (state->data.groups.empty() || !show) {
				return;
			}
			show->showBox(Ui::MakeConfirmBox({
				.text = tr::lng_oblivion_profile_history_clear_sure(),
				.confirmed = [=](Fn<void()> close) {
					clear();
					close();
				},
				.confirmText = tr::lng_oblivion_profile_history_clear(),
				.confirmStyle = &st::attentionBoxButton,
			}));
		}, st::attentionBoxButton);
	}
	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
	});
}

[[nodiscard]] QString SessionFolder(not_null<Main::Session*> session) {
	return cWorkingDir()
		+ u"tdata/oblivion/"_q
		+ (session->isTestMode() ? u"test_"_q : QString())
		+ QString::number(session->userId().bare)
		+ u"/profiles/"_q;
}

[[nodiscard]] Store &StoreFor(not_null<Main::Session*> session) {
	static auto stores = base::flat_map<QString, std::unique_ptr<Store>>();
	const auto folder = SessionFolder(session);
	auto i = stores.find(folder);
	if (i == end(stores)) {
		i = stores.emplace(folder, std::make_unique<Store>(folder)).first;
	}
	return *i->second;
}

[[nodiscard]] bool Forbidden(not_null<PeerData*> peer) {
	if (const auto chat = peer->asChat()) {
		return chat->isForbidden();
	} else if (const auto channel = peer->asChannel()) {
		return channel->isForbidden();
	}
	return false;
}

[[nodiscard]] bool TrackableType(not_null<PeerData*> peer) {
	if (peer->isSelf()
		|| peer->isServiceUser()
		|| peer->isNotificationsUser()
		|| peer->isRepliesChat()
		|| peer->isVerifyCodes()
		|| peer->isSavedHiddenAuthor()
		|| peer->isMonoforum()
		|| peer->migrateTo()) {
		return false;
	} else if (const auto chat = peer->asChat()) {
		return !chat->isDeactivated();
	}
	return true;
}

[[nodiscard]] bool Inaccessible(not_null<PeerData*> peer) {
	const auto user = peer->asUser();
	return user && user->isInaccessible();
}

[[nodiscard]] int UserpicDc(not_null<PeerData*> peer) {
	const auto location = peer->userpicLocation();
	const auto storage = std::get_if<StorageFileLocation>(
		&location.file().data);
	return storage ? storage->dcId() : 0;
}

[[nodiscard]] Snapshot Capture(not_null<PeerData*> peer) {
	auto result = Snapshot();
	const auto user = peer->asUser();
	if (user && user->isInaccessible()) {
		// The "Deleted Account" name depends on the app language.
		result.deleted = true;
		return result;
	}
	if (user) {
		result.first = user->firstName.left(kMaxNameLength);
		result.last = user->lastName.left(kMaxNameLength);
		result.contact = user->isContact();
	} else {
		result.first = (HasLocalName(peer)
			? OriginalName(peer)
			: peer->name()).left(kMaxNameLength);
	}
	result.nameKnown = !FullName(result).isEmpty();

	// A chat we are banned in comes without the photo and the link.
	const auto forbidden = Forbidden(peer);
	if (!forbidden) {
		for (const auto &username : peer->usernames()) {
			if (!username.isEmpty()
				&& result.usernames.size() < kMaxUsernames) {
				result.usernames.push_back(
					username.left(kMaxUsernameLength));
			}
		}

		// A "min" object of a user whose first username is not the
		// editable one leaves a second copy of it in the list till the
		// next full object. The real list has no duplicates, so such a
		// list is not compared and doesn't replace the stored one.
		if (HasDuplicates(result.usernames)) {
			result.usernames.clear();
		} else {
			result.usernamesKnown = true;
		}
	}
	if (peer->wasFullUpdated()) {
		result.about = peer->about().left(kMaxAboutLength);
		result.aboutKnown = true;
	}
	// A photo set by us for a contact hides the real one.
	if (!forbidden
		&& !peer->userpicPhotoUnknown()
		&& (!user || !user->hasPersonalPhoto())) {
		result.photoId = peer->userpicPhotoId();
		result.photoDc = result.photoId ? UserpicDc(peer) : 0;
		result.photoKnown = true;
	}
	return result;
}

[[nodiscard]] ImageLocation PhotoLocation(
		not_null<PeerData*> peer,
		uint64 photoId,
		int dcId,
		bool big) {
	using Flag = MTPDinputPeerPhotoFileLocation::Flag;
	const auto side = big ? kBigPhotoSide : kSmallPhotoSide;
	return ImageLocation(
		{ StorageFileLocation(
			dcId,
			UserId(),
			MTP_inputPeerPhotoFileLocation(
				MTP_flags(big ? Flag::f_big : Flag(0)),
				peer->input(),
				MTP_long(photoId))) },
		side,
		side);
}

class Tracker final : public base::has_weak_ptr {
public:
	Tracker(not_null<Main::Session*> session, not_null<Store*> store);
	~Tracker();

	void seen(not_null<PeerData*> peer);
	void force(not_null<PeerData*> peer);
	void forget();

private:
	using PhotoKey = std::tuple<uint64, uint64, bool>;
	struct PhotoJob {
		uint64 peerId = 0;
		uint64 photoId = 0;
		bool big = false;
		int attempts = 0;
		ImageLocation location;
	};
	struct ActivePhoto {
		PhotoJob job;
		Data::CloudFile file; // The small photos.
		std::unique_ptr<FileLoader> loader; // The big ones.
		bool done = false;
	};

	[[nodiscard]] bool relevant(not_null<PeerData*> peer) const;
	void check(not_null<PeerData*> peer);
	void processPending();
	void scan();

	void addPhotoJob(
		not_null<PeerData*> peer,
		uint64 photoId,
		int dcId,
		bool big,
		bool urgent);
	void startPhoto();
	void photoTimer();
	void photoDone(not_null<ActivePhoto*> active, QByteArray bytes);
	void photoWritten(const PhotoJob &job, bool saved);

	const not_null<Main::Session*> _session;
	const not_null<Store*> _store;

	std::unordered_set<uint64> _fresh;
	std::unordered_set<uint64> _checked;
	std::unordered_set<uint64> _aboutChecked;
	base::flat_set<uint64> _forced;
	std::vector<uint64> _pending;
	bool _pendingScheduled = false;
	bool _enabled = false;
	bool _fullScan = false;
	bool _forgotten = false;

	std::deque<PhotoJob> _urgentPhotos;
	std::deque<PhotoJob> _idlePhotos;
	std::set<PhotoKey> _queuedPhotos;
	std::set<PhotoKey> _failedPhotos;
	std::unique_ptr<ActivePhoto> _photo;

	base::Timer _photoTimer;
	base::Timer _scanTimer;
	base::Timer _flushTimer;
	rpl::lifetime _lifetime;

};

Tracker::Tracker(not_null<Main::Session*> session, not_null<Store*> store)
: _session(session)
, _store(store)
, _enabled(Get().profileHistory())
, _photoTimer([=] { photoTimer(); })
, _scanTimer([=] { scan(); })
, _flushTimer([=] { _store->flush(); }) {
	using Flag = Data::PeerUpdate::Flag;
	_session->changes().peerUpdates(Flag::Name
		| Flag::Username
		| Flag::Usernames
		| Flag::Photo
		| Flag::About
		| Flag::FullInfo
		| Flag::IsContact
	) | rpl::on_next([=](const Data::PeerUpdate &update) {
		check(update.peer);
	}, _lifetime);

	_store->dirtied() | rpl::on_next([=] {
		if (!_flushTimer.isActive()) {
			_flushTimer.callOnce(kFlushDelay);
		}
	}, _lifetime);

	Get().changes() | rpl::on_next([=] {
		const auto enabled = Get().profileHistory();
		if (enabled && !_enabled) {
			_checked.clear();
			_aboutChecked.clear();
			_fullScan = true;
			_scanTimer.callOnce(kEnabledScanDelay);
		}
		_enabled = enabled;
	}, _lifetime);

	_scanTimer.callOnce(kFirstScanDelay);
}

Tracker::~Tracker() {
	_photo = nullptr;
	_store->flush(true);
}

bool Tracker::relevant(not_null<PeerData*> peer) const {
	if (const auto user = peer->asUser()) {
		if (user->isContact()) {
			return true;
		}
	}
	const auto history = _session->data().historyLoaded(peer);
	return history && history->inChatList();
}

void Tracker::seen(not_null<PeerData*> peer) {
	const auto id = peer->id.value;
	if (!_fresh.emplace(id).second && !HasLocalName(peer)) {
		// Later changes arrive through session().changes(), except for
		// the real name hidden under a local one.
		return;
	}
	_pending.push_back(id);
	if (!_pendingScheduled) {
		_pendingScheduled = true;
		crl::on_main(this, [=] {
			processPending();
		});
	}
}

void Tracker::processPending() {
	_pendingScheduled = false;
	const auto list = base::take(_pending);
	if (!Get().profileHistory()) {
		return;
	}
	for (const auto id : list) {
		if (const auto peer = _session->data().peerLoaded(PeerId(id))) {
			check(peer);
		}
	}
}

void Tracker::force(not_null<PeerData*> peer) {
	const auto id = peer->id.value;
	_forced.emplace(id);
	if (!_forgotten
		&& Get().profileHistory()
		&& (_fresh.find(id) == end(_fresh))
		&& TrackableType(peer)
		&& !Inaccessible(peer)) {
		// Not received from the server in this launch, so not compared
		// and not remembered, see check(). The fresh profile comes with
		// its full info and gets here through seen().
		peer->updateFull();
	}
	check(peer);
}

void Tracker::check(not_null<PeerData*> peer) {
	const auto id = peer->id.value;
	if (_forgotten
		|| !Get().profileHistory()
		|| (_fresh.find(id) == end(_fresh))
		|| !peer->isLoaded()
		|| !TrackableType(peer)) {
		return;
	}
	const auto was = _store->baseline(id);
	const auto forced = _forced.contains(id);
	if (!was && !forced && !relevant(peer)) {
		return;
	}
	const auto snapshot = Capture(peer);
	const auto late = _checked.emplace(id).second;
	const auto lateAbout = snapshot.aboutKnown
		&& _aboutChecked.emplace(id).second;
	const auto changes = _store->apply(
		id,
		snapshot,
		base::unixtime::now(),
		late,
		forced,
		lateAbout);
	const auto now = _store->baseline(id);
	if (!now) {
		return;
	}
	auto urgent = false;
	for (const auto &change : changes) {
		if (change.field != Field::Photo) {
			continue;
		}
		urgent = true;
		const auto photoId = change.wasPhoto;
		if (photoId && was) {
			const auto dcId = was->photoDc ? was->photoDc : now->photoDc;
			if (was->savedPhoto != photoId) {
				addPhotoJob(peer, photoId, dcId, false, true);
			}
			addPhotoJob(peer, photoId, dcId, true, true);
		}
	}
	if (!now->deleted
		&& now->photoKnown
		&& now->photoId
		&& (now->savedPhoto != now->photoId)) {
		addPhotoJob(peer, now->photoId, now->photoDc, false, urgent);
	}
}

void Tracker::scan() {
	_scanTimer.callOnce(kScanInterval);
	if (!Get().profileHistory()) {
		return;
	}
	// Profiles that got into the chats list or the contacts without any
	// change since they were received: their baseline is taken here.
	const auto full = base::take(_fullScan);
	const auto add = [&](not_null<const Dialogs::IndexedList*> list) {
		for (const auto &row : list->all()) {
			if (const auto history = row->history()) {
				const auto peer = history->peer;
				if (full || !_store->tracked(peer->id.value)) {
					check(peer);
				}
			}
		}
	};
	const auto owner = &_session->data();
	add(owner->chatsList()->indexed());
	if (const auto folder = owner->folderLoaded(Data::Folder::kId)) {
		add(folder->chatsList()->indexed());
	}
	add(owner->contactsNoChatsList());
}

void Tracker::addPhotoJob(
		not_null<PeerData*> peer,
		uint64 photoId,
		int dcId,
		bool big,
		bool urgent) {
	if (!photoId || !dcId) {
		return;
	}
	const auto peerId = peer->id.value;
	const auto key = PhotoKey(peerId, photoId, big);
	if ((!urgent && int(_idlePhotos.size()) >= kMaxPhotoQueue)
		|| _failedPhotos.contains(key)
		|| !_queuedPhotos.emplace(key).second) {
		return;
	}
	(urgent ? _urgentPhotos : _idlePhotos).push_back({
		.peerId = peerId,
		.photoId = photoId,
		.big = big,
		.location = PhotoLocation(peer, photoId, dcId, big),
	});
	if (!_photo && !_photoTimer.isActive()) {
		startPhoto();
	}
}

void Tracker::startPhoto() {
	if (_photo) {
		return;
	}
	auto &queue = _urgentPhotos.empty() ? _idlePhotos : _urgentPhotos;
	if (queue.empty()) {
		return;
	}
	_photo = std::make_unique<ActivePhoto>();
	_photo->job = std::move(queue.front());
	queue.pop_front();
	_photo->file.location = _photo->job.location;

	// The callbacks may be called more than once and right from the
	// call below, the file itself is destroyed only from the timer.
	const auto raw = _photo.get();
	_photoTimer.callOnce(kPhotoTimeout);
	if (raw->job.big) {
		// Both sizes of a peer photo have the same key in the image cache
		// and the app keeps the small one there. The big one is loaded
		// past the cache: it is neither read from it nor put into it.
		raw->loader = CreateFileLoader(
			_session,
			raw->job.location.file(),
			Data::FileOriginPeerPhoto(PeerId(raw->job.peerId)),
			QString(),
			0,
			0,
			UnknownFileLocation,
			LoadToFileOnly,
			LoadFromCloudOrLocal,
			false,
			Data::kImageCacheTag);

		// The loader may be in its destructor already when this is
		// called, but then it is not the active one anymore.
		const auto finished = [=](bool failed) {
			if (_photo.get() != raw || raw->done) {
				return;
			}
			const auto loader = raw->loader.get();
			const auto loaded = loader && !failed && !loader->cancelled();
			photoDone(raw, loaded ? loader->bytes() : QByteArray());
		};
		raw->loader->updates(
		) | rpl::on_error_done([=](FileLoader::Error) {
			finished(true);
		}, [=] {
			finished(false);
		}, raw->loader->lifetime());
		raw->loader->start();
		return;
	}
	Data::LoadCloudFile(
		_session,
		raw->file,
		Data::FileOriginPeerPhoto(PeerId(raw->job.peerId)),
		LoadFromCloudOrLocal,
		false,
		Data::kImageCacheTag,
		nullptr,
		Fn<void(QByteArray)>([=](QByteArray bytes) {
			photoDone(raw, std::move(bytes));
		}),
		[=](bool) {
			photoDone(raw, QByteArray());
		});
}

void Tracker::photoTimer() {
	if (const auto active = base::take(_photo)) {
		if (!active->done) {
			// Timed out, most likely there is no connection right now.
			auto job = std::move(active->job);
			if (++job.attempts < kPhotoAttempts) {
				_idlePhotos.push_back(std::move(job));
			} else {
				_queuedPhotos.erase(
					PhotoKey(job.peerId, job.photoId, job.big));
			}
		}
	}
	startPhoto();
}

void Tracker::photoDone(not_null<ActivePhoto*> active, QByteArray bytes) {
	if (_photo.get() != active.get() || active->done) {
		return;
	}
	active->done = true;
	const auto job = active->job;
	if (bytes.isEmpty()) {
		photoWritten(job, false);
	} else {
		const auto path = _store->photoPath(
			job.peerId,
			job.photoId,
			job.big);
		IoQueue().async([=, weak = base::make_weak(this)] {
			const auto saved = (!job.big || LargerThanSmallPhoto(bytes))
				&& WritePhotoFile(path, bytes);
			crl::on_main(weak, [=] {
				weak->photoWritten(job, saved);
			});
		});
	}
	_photoTimer.callOnce(kPhotoPause);
}

void Tracker::photoWritten(const PhotoJob &job, bool saved) {
	const auto key = PhotoKey(job.peerId, job.photoId, job.big);
	_queuedPhotos.erase(key);
	if (!saved) {
		// Not asked again till the next launch: an old photo that was
		// deleted by its owner fails every time.
		_failedPhotos.emplace(key);
		return;
	}
	if (!job.big) {
		const auto now = _store->baseline(job.peerId);
		if (now && now->photoId == job.photoId) {
			_store->setSavedPhoto(job.peerId, job.photoId);
		}
	}
	if (_store->hasChanges(job.peerId)) {
		_store->notify(job.peerId);
	}
}

void Tracker::forget() {
	_forgotten = true;
	_photoTimer.cancel();
	_photo = nullptr;
	_urgentPhotos.clear();
	_idlePhotos.clear();
	_queuedPhotos.clear();
	_failedPhotos.clear();
	_pending.clear();
	_forced.clear();
	_checked.clear();
	_aboutChecked.clear();
}

using Trackers = base::flat_map<
	not_null<Main::Session*>,
	not_null<Tracker*>>;

[[nodiscard]] Trackers &AllTrackers() {
	static auto result = Trackers();
	return result;
}

[[nodiscard]] Tracker *TrackerFor(not_null<Main::Session*> session) {
	const auto &trackers = AllTrackers();
	const auto i = trackers.find(session);
	return (i != end(trackers)) ? i->second.get() : nullptr;
}

// The small copy is the thumbnail. The big one is opened only when there
// is more in it: a copy of the small photo saved under the name of the
// big one has the same size in bytes.
[[nodiscard]] ViewPhoto ChoosePhoto(
		const QString &small,
		qint64 smallSize,
		const QString &big,
		qint64 bigSize) {
	if (smallSize <= 0 && bigSize <= 0) {
		return {};
	}
	return {
		.path = (smallSize > 0) ? small : big,
		.open = (bigSize > smallSize) ? big : small,
	};
}

[[nodiscard]] ViewPhoto ResolvePhoto(
		not_null<Store*> store,
		uint64 peerId,
		uint64 photoId) {
	if (!photoId) {
		return {};
	}
	const auto small = store->photoPath(peerId, photoId, false);
	const auto big = store->photoPath(peerId, photoId, true);
	return ChoosePhoto(
		small,
		QFileInfo(small).size(),
		big,
		QFileInfo(big).size());
}

[[nodiscard]] ViewData CollectView(
		not_null<Store*> store,
		uint64 peerId,
		const QString &name,
		bool user,
		bool deleted) {
	auto result = ViewData{
		.name = name,
		.user = user,
		.enabled = Get().profileHistory(),
		.deleted = deleted,
	};
	if (const auto baseline = store->baseline(peerId)) {
		result.since = baseline->since;
		result.contact = baseline->contact;
		result.deleted = deleted || baseline->deleted;
	}
	const auto changes = store->changes(peerId);
	for (auto i = changes.rbegin(); i != changes.rend(); ++i) {
		const auto &change = *i;
		if (result.groups.empty()
			|| result.groups.back().date != change.date
			|| result.groups.back().late != change.late) {
			result.groups.push_back({
				.date = change.date,
				.late = change.late,
			});
		}
		auto view = ViewChange{
			.field = change.field,
			.was = change.was,
			.now = change.now,
			.hadPhoto = (change.wasPhoto != 0),
			.hasPhoto = (change.nowPhoto != 0),
		};
		if (change.field == Field::Photo) {
			view.wasPhoto = ResolvePhoto(store, peerId, change.wasPhoto);
			view.nowPhoto = ResolvePhoto(store, peerId, change.nowPhoto);
		}
		auto &list = result.groups.back().changes;
		list.insert(begin(list), std::move(view));
	}
	return result;
}

[[nodiscard]] QImage SampleAvatar(QRgb top, QRgb bottom) {
	const auto side = kSmallPhotoSide;
	auto result = QImage(side, side, QImage::Format_ARGB32_Premultiplied);
	auto p = QPainter(&result);
	auto gradient = QLinearGradient(0, 0, 0, side);
	gradient.setColorAt(0., QColor(top));
	gradient.setColorAt(1., QColor(bottom));
	p.fillRect(result.rect(), gradient);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(255, 255, 255, 230));
	p.drawEllipse(QPointF(side / 2., side * 0.4), side * 0.18, side * 0.18);
	p.drawEllipse(QPointF(side / 2., side * 1.02), side * 0.36, side * 0.36);
	p.end();
	return result;
}

[[nodiscard]] TimeId SampleDate(int day, int hour, int minute) {
	return TimeId(QDateTime(
		QDate(2026, 9, day),
		QTime(hour, minute)).toSecsSinceEpoch());
}

[[nodiscard]] ViewData SampleUserHistory() {
	const auto blue = SampleAvatar(0x72D5FD, 0x2A9EF1);
	const auto orange = SampleAvatar(0xFEBB5B, 0xF8833D);
	const auto green = SampleAvatar(0xA0DE7E, 0x54CB68);
	auto result = ViewData{
		.name = u"Ваня Петров"_q,
		.user = true,
		.enabled = true,
		.since = SampleDate(12, 18, 4),
	};
	result.groups.push_back({
		.date = SampleDate(30, 21, 47),
		.changes = {
			{
				.field = Field::Name,
				.was = u"Иван Петров"_q,
				.now = u"Ваня Петров"_q,
			},
			{
				.field = Field::Photo,
				.hadPhoto = true,
				.hasPhoto = true,
				.wasPhoto = { .image = orange },
				.nowPhoto = { .image = blue },
			},
		},
	});
	result.groups.push_back({
		.date = SampleDate(28, 9, 12),
		.late = true,
		.changes = { {
			.field = Field::About,
			.was = u"Дизайнер, Москва. Пишите в любое время."_q,
			.now = u"В отпуске до 10 октября, отвечаю медленно"_q,
		} },
	});
	result.groups.push_back({
		.date = SampleDate(21, 14, 30),
		.changes = { {
			.field = Field::Username,
			.was = u"ivan_petrov"_q,
			.now = u"vanya ivan_petrov"_q,
		} },
	});
	result.groups.push_back({
		.date = SampleDate(17, 23, 58),
		.changes = { {
			.field = Field::Photo,
			.hadPhoto = true,
			.hasPhoto = true,
			.nowPhoto = { .image = orange },
		} },
	});
	result.groups.push_back({
		.date = SampleDate(14, 8, 5),
		.changes = {
			{
				.field = Field::Photo,
				.hadPhoto = true,
				.wasPhoto = { .image = green },
			},
			{
				.field = Field::Username,
				.now = u"ivan_petrov"_q,
			},
		},
	});
	return result;
}

[[nodiscard]] ViewData SampleGroupHistory() {
	const auto violet = SampleAvatar(0xE0A2F3, 0xD669ED);
	auto result = ViewData{
		.name = u"Походы выходного дня"_q,
		.user = false,
		.enabled = true,
		.since = SampleDate(3, 10, 0),
	};
	result.groups.push_back({
		.date = SampleDate(29, 19, 20),
		.changes = {
			{
				.field = Field::Name,
				.was = u"Поход 27 сентября"_q,
				.now = u"Походы выходного дня"_q,
			},
			{
				.field = Field::About,
				.now = (u"Собираемся по субботам в 8:00 у метро.\n"_q
					+ u"Список снаряжения в закреплённом сообщении."_q),
			},
			{
				.field = Field::Photo,
				.hasPhoto = true,
				.nowPhoto = { .image = violet },
			},
		},
	});
	result.groups.push_back({
		.date = SampleDate(20, 12, 45),
		.changes = { {
			.field = Field::Username,
			.now = u"weekend_hikes"_q,
		} },
	});
	return result;
}

// The photo cases that are scrolled out in the first scene: the old photo
// that was not saved, a removed photo followed by one more change. And
// the note about the name of a contact.
[[nodiscard]] ViewData SamplePhotosHistory() {
	const auto orange = SampleAvatar(0xFEBB5B, 0xF8833D);
	const auto green = SampleAvatar(0xA0DE7E, 0x54CB68);
	auto result = ViewData{
		.name = u"Ваня Петров"_q,
		.user = true,
		.enabled = true,
		.contact = true,
		.since = SampleDate(12, 18, 4),
	};
	result.groups.push_back({
		.date = SampleDate(26, 20, 15),
		.changes = { {
			.field = Field::Photo,
			.hadPhoto = true,
			.hasPhoto = true,
			.nowPhoto = { .image = orange },
		} },
	});
	result.groups.push_back({
		.date = SampleDate(19, 7, 40),
		.late = true,
		.changes = {
			{
				.field = Field::Photo,
				.hadPhoto = true,
				.wasPhoto = { .image = green },
			},
			{
				.field = Field::Username,
				.was = u"ivan_petrov"_q,
			},
		},
	});
	result.groups.push_back({
		.date = SampleDate(15, 13, 5),
		.changes = { {
			.field = Field::Photo,
			.hadPhoto = true,
		} },
	});
	return result;
}

// Everything that has to wrap: a long name, a bio with a link that has
// no spaces in it, several usernames. And a deleted account.
[[nodiscard]] ViewData SampleLongHistory() {
	auto result = ViewData{
		.name = u"Александра Константиновна Преображенская-Белозерская"_q,
		.user = true,
		.enabled = true,
		.since = SampleDate(2, 9, 30),
	};
	result.groups.push_back({
		.date = SampleDate(30, 10, 15),
		.changes = { {
			.field = Field::Deleted,
			.was = u"Александра Преображенская"_q,
		} },
	});
	result.groups.push_back({
		.date = SampleDate(24, 22, 48),
		.late = true,
		.changes = { {
			.field = Field::About,
			.was = (u"Архитектор. Проекты и контакты: https://example.com/"_q
				+ u"portfolio/aleksandra-preobrazhenskaya-belozerskaya"_q),
			.now = (u"Архитектор, в отпуске.\n"_q
				+ u"По срочным вопросам пишите Марии."_q),
		} },
	});
	result.groups.push_back({
		.date = SampleDate(11, 16, 2),
		.changes = { {
			.field = Field::Username,
			.was = u"aleksandra_preobrazhenskaya"_q,
			.now = (u"sasha_arch aleksandra_preobrazhenskaya "_q
				+ u"sasha_belozerskaya"_q),
		} },
	});
	return result;
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;
	const auto add = [](QString name, Fn<ViewData()> data) {
		RegisterScene({
			.name = std::move(name),
			.size = QSize(st::boxWideWidth * 2, 0),
			.box = [=](std::shared_ptr<Ui::Show> show) {
				return Box(HistoryBox, BoxArgs{
					.show = std::move(show),
					.data = rpl::single(data()),
					.clear = [] {},
				});
			},
			.ready = [](not_null<QWidget*> widget) {
				return true;
			},
		});
	};

	add(u"profile_history_box"_q, [] {
		return SampleUserHistory();
	});

	add(u"profile_history_group"_q, [] {
		return SampleGroupHistory();
	});

	add(u"profile_history_photos"_q, [] {
		return SamplePhotosHistory();
	});

	add(u"profile_history_long"_q, [] {
		return SampleLongHistory();
	});

	add(u"profile_history_empty"_q, [] {
		return ViewData{
			.name = u"Мария Смирнова"_q,
			.user = true,
			.enabled = true,
			.since = SampleDate(25, 16, 40),
		};
	});

	add(u"profile_history_waiting"_q, [] {
		return ViewData{
			.name = u"Мария Смирнова"_q,
			.user = true,
			.enabled = true,
		};
	});

	add(u"profile_history_deleted"_q, [] {
		return ViewData{
			.name = u"Удалённый аккаунт"_q,
			.user = true,
			.enabled = true,
			.deleted = true,
			.since = SampleDate(25, 16, 40),
		};
	});

	add(u"profile_history_off"_q, [] {
		return ViewData{
			.name = u"Мария Смирнова"_q,
			.user = true,
			.enabled = false,
		};
	});
});

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

};

Checker::Checker(QStringList &log)
: _log(log) {
}

void Checker::operator()(bool condition, const char *what) {
	if (condition) {
		++_passed;
	} else {
		++_failed;
		_log.push_back(u"FAILED: "_q + QString::fromUtf8(what));
	}
}

void Checker::section(const char *name) {
	_log.push_back(QString::fromUtf8(name)
		+ u": "_q
		+ QString::number(_passed)
		+ u" checks passed so far"_q);
}

int Checker::passed() const {
	return _passed;
}

int Checker::failed() const {
	return _failed;
}

[[nodiscard]] Snapshot TestProfile() {
	auto result = Snapshot();
	result.first = u"Иван"_q;
	result.last = u"Петров"_q;
	result.nameKnown = true;
	result.usernames = QStringList{ u"ivan"_q };
	result.usernamesKnown = true;
	result.photoId = 0xF123456789ABCDEFULL;
	result.photoDc = 2;
	result.photoKnown = true;
	return result;
}

[[nodiscard]] QByteArray ReadFile(const QString &path) {
	auto file = QFile(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void TouchFile(const QString &path) {
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QFile(path);
	if (file.open(QIODevice::WriteOnly)) {
		file.write("\xFF\xD8\xFF");
		file.close();
	}
}

void TestDiff(Checker &check) {
	const auto base = TestProfile();

	check(Diff(base, base).changes.empty(), "same profile, no changes");
	check(Diff(base, base).merged == base, "same profile, same baseline");
	check(Diff(base, Snapshot()).changes.empty(), "nothing known, no changes");
	check(Diff(base, Snapshot()).merged == base, "nothing known, kept");

	auto renamed = base;
	renamed.first = u"Ваня"_q;
	auto diff = Diff(base, renamed);
	check(diff.changes.size() == 1
		&& diff.changes[0].field == Field::Name
		&& diff.changes[0].was == u"Иван Петров"_q
		&& diff.changes[0].now == u"Ваня Петров"_q, "name change");
	check(diff.merged.first == u"Ваня"_q, "name stored");

	auto split = base;
	split.first = u"Иван Петров"_q;
	split.last = QString();
	diff = Diff(base, split);
	check(diff.changes.empty(), "name split, no change");
	check(diff.merged.first == u"Иван Петров"_q, "name split stored");

	auto added = base;
	added.first = u"Ваня"_q;
	added.last = u"работа"_q;
	added.contact = true;
	diff = Diff(base, added);
	check(diff.changes.empty(), "contact added, no change");
	check(diff.merged.contact
		&& diff.merged.first == u"Ваня"_q
		&& diff.merged.last == u"работа"_q, "contact added, name stored");
	auto edited = added;
	edited.first = u"Иван"_q;
	edited.last = u"сосед"_q;
	diff = Diff(added, edited);
	check(diff.changes.empty(), "contact renamed by us, no change");
	check(diff.merged.contact && diff.merged.last == u"сосед"_q,
		"contact renamed by us, name stored");
	diff = Diff(edited, renamed);
	check(diff.changes.empty(), "contact deleted, no change");
	check(!diff.merged.contact && diff.merged.first == u"Ваня"_q,
		"contact deleted, real name stored");
	check(Diff(diff.merged, base).changes.size() == 1,
		"not a contact anymore, name change");
	check(Diff(added, Snapshot()).merged.contact,
		"nothing known, contact mark kept");
	auto contactPhoto = added;
	contactPhoto.photoId = 42;
	diff = Diff(added, contactPhoto);
	check(diff.changes.size() == 1
		&& diff.changes[0].field == Field::Photo, "contact, photo change");

	auto usernames = base;
	usernames.usernames = QStringList{ u"vanya"_q, u"ivan"_q };
	diff = Diff(base, usernames);
	check(diff.changes.size() == 1
		&& diff.changes[0].field == Field::Username
		&& diff.changes[0].was == u"ivan"_q
		&& diff.changes[0].now == u"vanya ivan"_q, "username change");
	auto unknownUsernames = base;
	unknownUsernames.usernames.clear();
	unknownUsernames.usernamesKnown = false;
	check(Diff(base, unknownUsernames).changes.empty(),
		"unknown usernames, no change");
	check(Diff(base, unknownUsernames).merged.usernames == base.usernames,
		"unknown usernames, kept");
	check(!HasDuplicates({})
		&& !HasDuplicates({ u"ivan"_q })
		&& !HasDuplicates({ u"ivan"_q, u"vanya"_q, u"Ivan"_q }),
		"usernames without duplicates");
	check(HasDuplicates({ u"ivan"_q, u"ivan"_q })
		&& HasDuplicates({ u"ivan"_q, u"vanya"_q, u"ivan"_q }),
		"usernames with duplicates");
	check(FalseUsernameChange({
		.field = Field::Username,
		.was = u"ivan"_q,
		.now = u"ivan ivan"_q,
	}) && FalseUsernameChange({
		.field = Field::Username,
		.was = u"ivan vanya ivan"_q,
		.now = u"ivan vanya"_q,
	}), "false username change");
	check(!FalseUsernameChange({
		.field = Field::Username,
		.was = u"ivan"_q,
		.now = u"vanya ivan"_q,
	}) && !FalseUsernameChange({
		.field = Field::About,
		.was = u"да да"_q,
		.now = u"нет нет"_q,
	}), "real changes are kept");

	auto about = base;
	about.about = u"Первая\nстрока"_q;
	about.aboutKnown = true;
	diff = Diff(base, about);
	check(diff.changes.empty(), "bio first known, no change");
	check(diff.merged.aboutKnown && diff.merged.about == about.about,
		"bio first known, stored");
	auto about2 = about;
	about2.about = QString();
	diff = Diff(about, about2);
	check(diff.changes.size() == 1
		&& diff.changes[0].field == Field::About
		&& diff.changes[0].was == about.about
		&& diff.changes[0].now.isEmpty(), "bio removed");
	diff = Diff(about, base);
	check(diff.changes.empty() && diff.merged.about == about.about,
		"bio unknown, kept");

	auto photo = base;
	photo.photoId = 42;
	photo.photoDc = 4;
	diff = Diff(base, photo);
	check(diff.changes.size() == 1
		&& diff.changes[0].field == Field::Photo
		&& diff.changes[0].wasPhoto == base.photoId
		&& diff.changes[0].nowPhoto == 42, "photo change");
	check(diff.merged.photoId == 42 && diff.merged.photoDc == 4,
		"photo stored");
	auto noPhoto = base;
	noPhoto.photoId = 0;
	noPhoto.photoDc = 0;
	diff = Diff(base, noPhoto);
	check(diff.changes.size() == 1
		&& diff.changes[0].wasPhoto == base.photoId
		&& diff.changes[0].nowPhoto == 0, "photo removed");
	auto hidden = base;
	hidden.photoId = 0;
	hidden.photoKnown = false;
	diff = Diff(base, hidden);
	check(diff.changes.empty() && diff.merged.photoId == base.photoId,
		"photo unknown, kept");
	auto saved = base;
	saved.savedPhoto = base.photoId;
	check(Diff(saved, photo).merged.savedPhoto == base.photoId,
		"saved photo mark kept");

	auto many = base;
	many.first = u"Ваня"_q;
	many.usernames.clear();
	many.photoId = 7;
	check(Diff(base, many).changes.size() == 3, "three changes at once");

	auto deleted = Snapshot();
	deleted.deleted = true;
	diff = Diff(base, deleted);
	check(diff.changes.size() == 1
		&& diff.changes[0].field == Field::Deleted
		&& diff.changes[0].was == u"Иван Петров"_q, "account deleted");
	check(diff.merged.deleted && diff.merged.first == base.first,
		"deleted, profile kept");
	check(Diff(diff.merged, deleted).changes.empty(), "deleted only once");
}

void TestSerialization(Checker &check) {
	auto profile = TestProfile();
	profile.about = u"Строка \"в кавычках\"\nвторая \\ строка \U0001F600"_q;
	profile.aboutKnown = true;
	profile.usernames = QStringList{ u"ivan"_q, u"vanya"_q };
	profile.savedPhoto = profile.photoId;
	profile.since = 1790000000;
	const auto peerId = 0xFFFFFFFFFFFFFF01ULL;
	const auto line = SerializeSnapshot(peerId, profile);
	check(line.endsWith('\n') && line.count('\n') == 1, "profile: one line");
	const auto parsed = ParseSnapshot(line);
	check(parsed
		&& parsed->first == peerId
		&& parsed->second == profile, "profile round-trip");
	auto contact = profile;
	contact.contact = true;
	const auto parsedContact = ParseSnapshot(
		SerializeSnapshot(peerId, contact));
	check(parsedContact
		&& parsedContact->second == contact
		&& parsedContact->second != profile, "contact round-trip");
	const auto before = ParseSnapshot(
		"{\"p\":\"7\",\"f\":\"Иван\",\"k\":1,\"t\":5}");
	check(before
		&& before->second.nameKnown
		&& !before->second.contact, "profile saved without the contact mark");

	auto deleted = Snapshot();
	deleted.deleted = true;
	deleted.since = 5;
	const auto parsedDeleted = ParseSnapshot(SerializeSnapshot(1, deleted));
	check(parsedDeleted && parsedDeleted->second == deleted,
		"deleted profile round-trip");

	for (const auto field : {
		Field::Name,
		Field::Username,
		Field::About,
		Field::Photo,
		Field::Deleted,
	}) {
		const auto change = Change{
			.peerId = peerId,
			.date = 1790000123,
			.field = field,
			.was = u"было\nтак"_q,
			.now = u"стало \"так\""_q,
			.wasPhoto = 0xF000000000000001ULL,
			.nowPhoto = 3,
			.late = (field == Field::About),
		};
		const auto text = SerializeChange(change);
		const auto back = ParseChange(text);
		check(text.count('\n') == 1 && back && *back == change,
			"change round-trip");
	}

	check(!ParseSnapshot("not json\n"), "bad profile line");
	check(!ParseSnapshot("{\"p\":\"0\",\"k\":1}"), "profile without id");
	check(!ParseSnapshot("{\"p\":\"5\"}"), "profile without flags");
	check(!ParseSnapshot(line.left(line.size() / 2)), "cut profile line");
	check(!ParseChange("[1,2,3]"), "bad change line");
	check(!ParseChange("{\"p\":\"5\",\"t\":10,\"k\":\"nope\"}"),
		"unknown change kind");
	check(!ParseChange("{\"p\":\"5\",\"t\":0,\"k\":\"name\"}"),
		"change without date");
}

void TestStore(Checker &check, const QString &folder) {
	const auto statePath = folder + QString::fromLatin1(kStateFile);
	const auto changesPath = folder + QString::fromLatin1(kChangesFile);
	const auto base = TestProfile();
	const auto first = uint64(1001);
	const auto second = 0xFFFFFFFF00000002ULL;

	auto renamed = base;
	renamed.first = u"Ваня"_q;
	auto photo = renamed;
	photo.photoId = 777;
	{
		auto store = Store(folder);
		auto updates = std::vector<uint64>();
		auto lifetime = rpl::lifetime();
		store.updates() | rpl::on_next([&](uint64 peerId) {
			updates.push_back(peerId);
		}, lifetime);

		check(!store.tracked(first), "not tracked before");
		check(store.apply(first, base, 100).empty(), "baseline: no changes");
		check(store.tracked(first) && !store.hasChanges(first),
			"baseline: tracked");
		check(store.baseline(first)->since == 100, "baseline: since");
		check(store.apply(first, base, 150).empty(), "same: no changes");
		check(updates == std::vector<uint64>{ first },
			"baseline: one update, none without changes");

		check(store.apply(first, renamed, 200).size() == 1, "name recorded");
		check(store.apply(first, renamed, 210).empty(), "name only once");
		check(store.apply(first, photo, 300, true).size() == 1,
			"photo recorded");
		check(store.apply(second, base, 250).empty(), "second baseline");

		auto bio = base;
		bio.about = u"Первое"_q;
		bio.aboutKnown = true;
		check(store.apply(second, bio, 251).empty(), "bio is a baseline");
		bio.about = u"Второе"_q;
		bio.first = u"Пётр"_q;
		const auto mixed = store.apply(second, bio, 252, false, false, true);
		check(mixed.size() == 2
			&& mixed[0].field == Field::Name
			&& !mixed[0].late
			&& mixed[1].field == Field::About
			&& mixed[1].late, "bio noticed late");
		check(updates == std::vector<uint64>{
			first,
			first,
			first,
			second,
			second,
		}, "updates");

		auto deleted = Snapshot();
		deleted.deleted = true;
		check(store.apply(3003, deleted, 260).empty()
			&& !store.tracked(3003), "deleted is not tracked");

		const auto changes = store.changes(first);
		check(changes.size() == 2
			&& changes[0].field == Field::Name
			&& changes[0].date == 200
			&& !changes[0].late
			&& changes[1].field == Field::Photo
			&& changes[1].date == 300
			&& changes[1].late
			&& changes[1].peerId == first, "changes list");

		auto contact = photo;
		contact.first = u"Сосед"_q;
		contact.contact = true;
		check(store.apply(first, contact, 310).empty()
			&& store.baseline(first)->contact
			&& store.baseline(first)->first == u"Сосед"_q,
			"contact added: name stored, no changes");
		check(store.apply(first, photo, 320).empty()
			&& !store.baseline(first)->contact
			&& store.baseline(first)->first == u"Ваня"_q
			&& store.changes(first).size() == 2,
			"contact deleted: name stored, no changes");
		check(updates.size() == 7 && updates.back() == first,
			"contact: updates");

		store.setSavedPhoto(first, 777);
		store.flush(true);
	}
	check(ReadFile(statePath).count('\n') == 9, "state file lines");
	check(ReadFile(changesPath).count('\n') == 4, "changes file lines");
	{
		auto store = Store(folder);
		check(store.peersCount() == 2 && store.changesCount() == 4,
			"reloaded counts");
		const auto now = store.baseline(first);
		check(now
			&& now->first == u"Ваня"_q
			&& now->photoId == 777
			&& now->savedPhoto == 777
			&& now->since == 100, "reloaded baseline");
		const auto other = store.baseline(second);
		check(other
			&& other->first == u"Пётр"_q
			&& other->about == u"Второе"_q
			&& store.changes(second).size() == 2
			&& store.changes(second)[1].late, "reloaded big id");
		const auto changes = store.changes(first);
		check(changes.size() == 2
			&& changes[0].was == u"Иван Петров"_q
			&& changes[0].now == u"Ваня Петров"_q
			&& changes[1].wasPhoto == base.photoId
			&& changes[1].nowPhoto == 777, "reloaded changes");
		check(store.apply(first, photo, 400).empty(),
			"reloaded: no false changes");
	}

	{
		auto state = QFile(statePath);
		check(state.open(QIODevice::WriteOnly | QIODevice::Append),
			"state file opens");
		state.write("garbage\n{\"x\":1}\n{\"p\":\"1001\",\"f\":\"Обрыв");
		state.close();
		auto changes = QFile(changesPath);
		check(changes.open(QIODevice::WriteOnly | QIODevice::Append),
			"changes file opens");
		changes.write("{\"p\":\"1001\",\"t\":450,\"k\":\"username\","
			"\"w\":\"ivan\",\"n\":\"ivan ivan\"}\n");
		changes.write("\x01\x02\x03\n\n{\"p\":\"1001\",\"t\":500,\"k\":\"na");
		changes.close();
	}
	{
		auto store = Store(folder);
		check(store.peersCount() == 2 && store.changesCount() == 4,
			"damaged: good records survive");
		check(store.changes(first).size() == 2,
			"damaged: false username change dropped");
		check(store.baseline(first)->first == u"Ваня"_q,
			"damaged: baseline intact");
		store.flush(true);
	}
	const auto cleanState = ReadFile(statePath);
	const auto cleanChanges = ReadFile(changesPath);
	check(cleanState.count('\n') == 2
		&& cleanState.endsWith('\n')
		&& !cleanState.contains("garbage"), "damaged: state rewritten");
	check(cleanChanges.count('\n') == 4
		&& cleanChanges.endsWith('\n')
		&& !cleanChanges.contains('\x01')
		&& !cleanChanges.contains("ivan ivan"), "damaged: changes rewritten");
	{
		auto store = Store(folder);
		check(store.peersCount() == 2
			&& store.changes(first).size() == 2, "rewritten: reloaded");

		store.clear(first);
		check(!store.hasChanges(first) && store.tracked(first), "cleared");
		store.flush(true);
		check(ReadFile(changesPath).count('\n') == 2
			&& store.changes(second).size() == 2, "cleared: others stay");
		store.clear(second);
		store.flush(true);
		check(ReadFile(changesPath).isEmpty(), "cleared: file removed");

		store.forget();
		check(!store.tracked(first) && !store.peersCount(), "forgotten");
		store.flush(true);
		check(!QDir(folder).exists(), "forgotten: folder removed");
	}
}

void TestRetention(Checker &check, const QString &folder) {
	const auto changesPath = folder + QString::fromLatin1(kChangesFile);
	const auto limits = Limits{
		.maxPeers = 3,
		.maxChanges = 10,
		.maxChangesPerPeer = 4,
		.changesSlack = 2,
		.stateSlack = 5,
	};
	auto profile = TestProfile();
	const auto withPhoto = [&](uint64 photoId) {
		auto result = profile;
		result.photoId = photoId;
		return result;
	};
	{
		auto store = Store(folder, limits);

		const auto peer = uint64(1);
		for (auto id = uint64(1); id != 8; ++id) {
			TouchFile(store.photoPath(peer, id, false));
			TouchFile(store.photoPath(peer, id, true));
		}
		store.apply(peer, withPhoto(1), 10);
		for (auto id = uint64(2); id != 8; ++id) {
			store.apply(peer, withPhoto(id), TimeId(10 * id));
		}
		const auto changes = store.changes(peer);
		check(changes.size() == 4
			&& changes.front().wasPhoto == 3
			&& changes.back().nowPhoto == 7, "per profile cap");
		check(store.changesCount() == 4, "per profile cap: total");
		store.flush(true);
		check(!QFile::exists(store.photoPath(peer, 1, false))
			&& !QFile::exists(store.photoPath(peer, 2, true)),
			"dropped photos removed");
		check(QFile::exists(store.photoPath(peer, 3, false))
			&& QFile::exists(store.photoPath(peer, 7, true)),
			"referenced photos kept");

		store.apply(2, profile, 100);
		store.apply(3, profile, 100);
		store.apply(4, profile, 100);
		check(store.peersCount() == 3 && !store.tracked(4), "profiles cap");
		store.apply(4, profile, 100, false, true);
		check(store.tracked(4), "forced over the cap");

		auto names = profile;
		for (auto i = 0; i != 4; ++i) {
			names.first = u"Имя %1"_q.arg(i);
			store.apply(2, names, 200 + i);
			store.apply(3, names, 300 + i);
		}
		check(store.changesCount() == 12, "total within the slack");
		names.first = u"Последнее"_q;
		store.apply(4, names, 400);
		check(store.changesCount() == 10, "total cap");
		check(store.changes(peer).size() == 1
			&& store.changes(peer)[0].date == 70, "total cap: the oldest go");
		check(store.changes(4).size() == 1, "total cap: the newest stay");
		store.flush(true);
		check(!QFile::exists(store.photoPath(peer, 3, false))
			&& QFile::exists(store.photoPath(peer, 6, false))
			&& QFile::exists(store.photoPath(peer, 7, false)),
			"total cap: photos");
		check(ReadFile(changesPath).count('\n') == 10,
			"total cap: file compacted");
	}
	{
		auto smaller = limits;
		smaller.maxChangesPerPeer = 2;
		smaller.maxChanges = 5;
		smaller.changesSlack = 0;
		auto store = Store(folder, smaller);
		check(store.changes(2).size() <= 2
			&& store.changes(3).size() <= 2
			&& store.changesCount() == 5, "caps on load");
		check(store.peersCount() == 4, "profiles stay on load");
		store.flush(true);
		check(ReadFile(changesPath).count('\n') == 5,
			"caps on load: file compacted");
		store.forget();
		store.flush(true);
	}
}

[[nodiscard]] QByteArray TestImageBytes(int side) {
	auto image = QImage(side, side, QImage::Format_RGB32);
	image.fill(Qt::darkCyan);
	auto result = QByteArray();
	auto buffer = QBuffer(&result);
	if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
		return QByteArray();
	}
	return result;
}

void TestPhotos(Checker &check) {
	const auto small = TestImageBytes(kSmallPhotoSide);
	const auto big = TestImageBytes(kBigPhotoSide);
	check(!small.isEmpty() && !big.isEmpty(), "test images");
	check(!LargerThanSmallPhoto(small), "small copy is not a big photo");
	check(LargerThanSmallPhoto(big), "big photo accepted");
	check(LargerThanSmallPhoto(TestImageBytes(kSmallPhotoSide + 1)),
		"a bit larger photo accepted");
	check(!LargerThanSmallPhoto("\xFF\xD8\xFF not an image")
		&& !LargerThanSmallPhoto(QByteArray()), "garbage is not a big photo");

	const auto smallPath = u"small.jpg"_q;
	const auto bigPath = u"big.jpg"_q;
	check(ChoosePhoto(smallPath, 0, bigPath, 0).empty(), "no saved photo");
	auto chosen = ChoosePhoto(smallPath, 4000, bigPath, 0);
	check(chosen.path == smallPath && chosen.open == smallPath,
		"only the small photo");
	chosen = ChoosePhoto(smallPath, 4000, bigPath, 60000);
	check(chosen.path == smallPath && chosen.open == bigPath,
		"big photo is opened");
	chosen = ChoosePhoto(smallPath, 4000, bigPath, 4000);
	check(chosen.path == smallPath && chosen.open == smallPath,
		"a copy of the small photo is not opened as the big one");
	chosen = ChoosePhoto(smallPath, 0, bigPath, 60000);
	check(chosen.path == bigPath && chosen.open == bigPath,
		"only the big photo");
}

} // namespace

void Seen(not_null<PeerData*> peer) {
	const auto &trackers = AllTrackers();
	if (trackers.empty()) {
		return;
	}
	const auto i = trackers.find(
		not_null<Main::Session*>(&peer->session()));
	if (i != end(trackers)) {
		i->second->seen(peer);
	}
}

bool RunSelfTest(QStringList &log) {
	auto check = Checker(log);
	const auto folder = cWorkingDir() + u"oblivion_selftest_profiles/"_q;
	QDir(folder).removeRecursively();

	TestDiff(check);
	check.section("diff");
	TestSerialization(check);
	check.section("serialization");
	TestStore(check, folder + u"store/"_q);
	check.section("store");
	TestRetention(check, folder + u"retention/"_q);
	check.section("retention");
	TestPhotos(check);
	check.section("photos");

	QDir(folder).removeRecursively();
	log.push_back(u"profile_history: %1 checks passed, %2 failed"_q.arg(
		QString::number(check.passed()),
		QString::number(check.failed())));
	return !check.failed();
}

} // namespace Oblivion::ProfileHistory

namespace Oblivion {

void StartProfileHistory(not_null<Main::Session*> session) {
	using namespace ProfileHistory;
	auto &trackers = AllTrackers();
	if (trackers.contains(session)) {
		return;
	}
	const auto tracker = session->lifetime().make_state<Tracker>(
		session,
		&StoreFor(session));
	trackers.emplace(session, tracker);
	session->lifetime().add([=] {
		AllTrackers().remove(session);
	});
}

void ForgetProfileHistory(not_null<Main::Session*> session) {
	using namespace ProfileHistory;
	if (const auto tracker = TrackerFor(session)) {
		tracker->forget();
	}
	StoreFor(session).forget();
}

void ShowProfileHistory(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	using namespace ProfileHistory;
	const auto target = peer->migrateToOrMe();
	const auto session = &target->session();
	const auto store = &StoreFor(session);
	if (const auto tracker = TrackerFor(session)) {
		tracker->force(target);
	}
	const auto peerId = target->id.value;
	const auto name = target->name();
	const auto user = target->isUser();
	const auto deleted = Inaccessible(target);
	auto data = rpl::single(
		rpl::empty
	) | rpl::then(rpl::merge(
		store->updates() | rpl::filter([=](uint64 id) {
			return !id || (id == peerId);
		}) | rpl::to_empty,
		Get().changes()
	)) | rpl::map([=] {
		return CollectView(store, peerId, name, user, deleted);
	});
	controller->show(Box(HistoryBox, BoxArgs{
		.show = controller->uiShow(),
		.data = std::move(data),
		.clear = [=] { store->clear(peerId); },
	}));
}

void AddProfileHistoryAction(
		not_null<Window::SessionController*> controller,
		const Dialogs::Key &key,
		const Ui::Menu::MenuCallback &addAction) {
	using namespace ProfileHistory;
	if (key.topic()) {
		return;
	}
	const auto sublist = key.sublist();
	const auto peer = sublist ? sublist->sublistPeer().get() : key.peer();
	if (!peer) {
		return;
	}
	const auto target = peer->migrateToOrMe();
	if (!TrackableType(target)) {
		return;
	}
	// With the feature off and for a deleted account there is nothing to
	// start tracking: only what was already recorded can be shown.
	const auto onlyRecorded = !Get().profileHistory()
		|| Inaccessible(target);
	if (onlyRecorded
		&& !StoreFor(&target->session()).hasChanges(target->id.value)) {
		return;
	}
	const auto weak = base::make_weak(controller);
	addAction(
		tr::lng_oblivion_profile_history_menu(tr::now),
		[=] {
			if (const auto strong = weak.get()) {
				ShowProfileHistory(strong, target);
			}
		},
		&st::menuIconGroupLog);
}

} // namespace Oblivion
