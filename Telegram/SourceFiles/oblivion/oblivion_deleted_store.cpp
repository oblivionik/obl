/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_deleted_store.h"

#include "base/unixtime.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_poll.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/history_item_edition.h"
#include "main/main_session.h"
#include "oblivion/oblivion_listen.h"
#include "oblivion/oblivion_settings.h"
#include "ui/image/image.h"
#include "settings.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMimeDatabase>
#include <QtCore/QSaveFile>

namespace Oblivion {
namespace {

constexpr auto kDeletedFile = "deleted.jsonl";
constexpr auto kEditsFile = "edits.jsonl";
constexpr auto kMaxDeleted = 20000;
constexpr auto kDeletedSlack = 500;
constexpr auto kMaxVersionsPerMessage = 20;
constexpr auto kMaxEditVersions = 50000;
constexpr auto kEditVersionsAfterTrim = 40000;
constexpr auto kMinEditLinesForCompact = 2000;
constexpr auto kMaxMediaSize = int64(100) * 1024 * 1024;
constexpr auto kThumbSize = 320;
constexpr auto kPhotoLoadTimeout = crl::time(120) * 1000;
constexpr auto kMaxPendingPhotos = size_t(30);

struct EntityName {
	EntityType type = EntityType::Invalid;
	const char *name = nullptr;
};

constexpr auto kEntityNames = std::array{
	EntityName{ EntityType::Url, "url" },
	EntityName{ EntityType::CustomUrl, "curl" },
	EntityName{ EntityType::Email, "email" },
	EntityName{ EntityType::Hashtag, "hashtag" },
	EntityName{ EntityType::Cashtag, "cashtag" },
	EntityName{ EntityType::Mention, "mention" },
	EntityName{ EntityType::MentionName, "mention_name" },
	EntityName{ EntityType::CustomEmoji, "emoji" },
	EntityName{ EntityType::BotCommand, "command" },
	EntityName{ EntityType::Phone, "phone" },
	EntityName{ EntityType::BankCard, "card" },
	EntityName{ EntityType::Bold, "b" },
	EntityName{ EntityType::Italic, "i" },
	EntityName{ EntityType::Underline, "u" },
	EntityName{ EntityType::StrikeOut, "s" },
	EntityName{ EntityType::Code, "code" },
	EntityName{ EntityType::Pre, "pre" },
	EntityName{ EntityType::Blockquote, "quote" },
	EntityName{ EntityType::Spoiler, "spoiler" },
};

[[nodiscard]] QString DeletedMark() {
	return u"\U0001F5D1 "_q;
}

[[nodiscard]] QJsonArray SerializeEntities(const EntitiesInText &entities) {
	auto result = QJsonArray();
	for (const auto &entity : entities) {
		const auto i = ranges::find(
			kEntityNames,
			entity.type(),
			&EntityName::type);
		if (i == end(kEntityNames)) {
			continue;
		}
		auto item = QJsonArray{
			QString::fromLatin1(i->name),
			entity.offset(),
			entity.length(),
		};
		if (!entity.data().isEmpty()) {
			item.push_back(entity.data());
		}
		result.push_back(item);
	}
	return result;
}

[[nodiscard]] EntitiesInText ParseEntities(
		const QJsonArray &array,
		int textLength) {
	auto result = EntitiesInText();
	for (const auto &value : array) {
		const auto item = value.toArray();
		if (item.size() < 3) {
			continue;
		}
		const auto name = item[0].toString().toLatin1();
		const auto i = ranges::find_if(kEntityNames, [&](EntityName e) {
			return (name == e.name);
		});
		const auto offset = item[1].toInt(-1);
		const auto length = item[2].toInt(0);
		if (i == end(kEntityNames)
			|| offset < 0
			|| length <= 0
			|| offset + length > textLength) {
			continue;
		}
		result.push_back(EntityInText(
			i->type,
			offset,
			length,
			(item.size() > 3) ? item[3].toString() : QString()));
	}
	return result;
}

void WriteText(QJsonObject &object, const TextWithEntities &text) {
	if (!text.text.isEmpty()) {
		object.insert(u"x"_q, text.text);
	}
	if (!text.entities.isEmpty()) {
		object.insert(u"e"_q, SerializeEntities(text.entities));
	}
}

[[nodiscard]] TextWithEntities ReadText(const QJsonObject &object) {
	auto result = TextWithEntities();
	result.text = object.value(u"x"_q).toString();
	result.entities = ParseEntities(
		object.value(u"e"_q).toArray(),
		result.text.size());
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

[[nodiscard]] QByteArray SerializeDeleted(const DeletedRecord &record) {
	auto object = QJsonObject();
	object.insert(u"p"_q, QString::number(record.peerId));
	object.insert(u"m"_q, double(record.messageId));
	if (record.topicRootId) {
		object.insert(u"r"_q, double(record.topicRootId));
	}
	if (record.senderId) {
		object.insert(u"s"_q, QString::number(record.senderId));
	}
	object.insert(u"sn"_q, record.senderName);
	object.insert(u"cn"_q, record.chatName);
	object.insert(u"d"_q, double(record.date));
	object.insert(u"t"_q, double(record.deleted));
	if (record.out) {
		object.insert(u"o"_q, true);
	}
	WriteText(object, record.text);
	const auto &media = record.media;
	if (!media.type.isEmpty()) {
		auto data = QJsonObject();
		data.insert(u"type"_q, media.type);
		if (!media.name.isEmpty()) {
			data.insert(u"name"_q, media.name);
		}
		if (!media.mime.isEmpty()) {
			data.insert(u"mime"_q, media.mime);
		}
		if (!media.file.isEmpty()) {
			data.insert(u"file"_q, media.file);
		}
		if (!media.thumb.isEmpty()) {
			data.insert(u"thumb"_q, media.thumb);
		}
		if (media.size > 0) {
			data.insert(u"size"_q, double(media.size));
		}
		if (media.duration > 0) {
			data.insert(u"duration"_q, media.duration);
		}
		object.insert(u"media"_q, data);
	}
	return ToLine(object);
}

[[nodiscard]] std::optional<DeletedRecord> ParseDeleted(
		const QByteArray &line) {
	const auto parsed = FromLine(line);
	if (!parsed) {
		return std::nullopt;
	}
	const auto &object = *parsed;
	auto result = DeletedRecord();
	result.peerId = object.value(u"p"_q).toString().toULongLong();
	result.messageId = int64(object.value(u"m"_q).toDouble());
	if (!result.peerId || !result.messageId) {
		return std::nullopt;
	}
	result.topicRootId = int64(object.value(u"r"_q).toDouble());
	result.senderId = object.value(u"s"_q).toString().toULongLong();
	result.senderName = object.value(u"sn"_q).toString();
	result.chatName = object.value(u"cn"_q).toString();
	result.date = TimeId(object.value(u"d"_q).toDouble());
	result.deleted = TimeId(object.value(u"t"_q).toDouble());
	result.out = object.value(u"o"_q).toBool();
	result.text = ReadText(object);
	const auto media = object.value(u"media"_q).toObject();
	if (!media.isEmpty()) {
		result.media.type = media.value(u"type"_q).toString();
		result.media.name = media.value(u"name"_q).toString();
		result.media.mime = media.value(u"mime"_q).toString();
		result.media.file = media.value(u"file"_q).toString();
		result.media.thumb = media.value(u"thumb"_q).toString();
		result.media.size = int64(media.value(u"size"_q).toDouble());
		result.media.duration = media.value(u"duration"_q).toInt();
	}
	return result;
}

[[nodiscard]] QByteArray SerializeEdit(
		uint64 peerId,
		int64 messageId,
		const EditVersion &version) {
	auto object = QJsonObject();
	object.insert(u"p"_q, QString::number(peerId));
	object.insert(u"m"_q, double(messageId));
	object.insert(u"a"_q, double(version.since));
	object.insert(u"t"_q, double(version.replaced));
	WriteText(object, version.text);
	return ToLine(object);
}

[[nodiscard]] bool IsSafeRelativePath(const QString &relative) {
	return !relative.isEmpty()
		&& !relative.contains(u".."_q)
		&& !relative.startsWith('/')
		&& !relative.contains('\\')
		&& !relative.contains(':');
}

void SaveThumbnail(QImage image, const QString &path) {
	if (image.isNull() || path.isEmpty()) {
		return;
	}
	if (image.width() > kThumbSize || image.height() > kThumbSize) {
		image = image.scaled(
			kThumbSize,
			kThumbSize,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
	}
	if (image.hasAlphaChannel()) {
		auto opaque = QImage(image.size(), QImage::Format_RGB32);
		opaque.fill(Qt::white);
		auto p = QPainter(&opaque);
		p.drawImage(0, 0, image);
		p.end();
		image = std::move(opaque);
	}
	image.save(path, "JPG", 85);
}

void WritePhoto(
		const std::shared_ptr<Data::PhotoMedia> &view,
		const QString &file,
		const QString &thumb) {
	const auto large = Data::PhotoSize::Large;
	const auto bytes = view->imageBytes(large);
	const auto loaded = view->image(large);
	const auto image = loaded ? loaded->original() : QImage();
	if (bytes.isEmpty() && image.isNull()) {
		return;
	}
	crl::async([=] {
		if (!bytes.isEmpty()) {
			auto output = QFile(file);
			if (output.open(QIODevice::WriteOnly)) {
				output.write(bytes);
				output.close();
			}
		} else {
			image.save(file, "JPG", 92);
		}
		SaveThumbnail(image.isNull() ? QImage::fromData(bytes) : image, thumb);
	});
}

class MediaSaver final {
public:
	explicit MediaSaver(not_null<Main::Session*> session);

	void savePhoto(
		not_null<PhotoData*> photo,
		FullMsgId origin,
		const QString &file,
		const QString &thumb);

private:
	struct Pending {
		std::shared_ptr<Data::PhotoMedia> view;
		QString file;
		QString thumb;
		crl::time started = 0;
	};

	void check();

	const not_null<Main::Session*> _session;
	std::vector<Pending> _pending;
	rpl::lifetime _subscription;

};

MediaSaver::MediaSaver(not_null<Main::Session*> session)
: _session(session) {
}

void MediaSaver::savePhoto(
		not_null<PhotoData*> photo,
		FullMsgId origin,
		const QString &file,
		const QString &thumb) {
	auto view = photo->createMediaView();
	if (view->loaded()) {
		WritePhoto(view, file, thumb);
		return;
	}
	const auto small = view->image(Data::PhotoSize::Thumbnail);
	if (const auto preview = small
			? small
			: view->image(Data::PhotoSize::Small)) {
		const auto image = preview->original();
		crl::async([=] {
			SaveThumbnail(image, thumb);
		});
	}
	check();
	if (_pending.size() >= kMaxPendingPhotos) {
		return;
	}
	view->wanted(Data::PhotoSize::Large, origin);
	_pending.push_back({
		.view = std::move(view),
		.file = file,
		.thumb = thumb,
		.started = crl::now(),
	});
	if (!_subscription) {
		_session->downloaderTaskFinished(
		) | rpl::on_next([=] {
			check();
		}, _subscription);
	}
}

void MediaSaver::check() {
	const auto now = crl::now();
	for (auto i = begin(_pending); i != end(_pending);) {
		if (i->view->loaded()) {
			WritePhoto(i->view, i->file, i->thumb);
			i = _pending.erase(i);
		} else if (now - i->started > kPhotoLoadTimeout) {
			i = _pending.erase(i);
		} else {
			++i;
		}
	}
}

[[nodiscard]] MediaSaver &SaverFor(not_null<Main::Session*> session) {
	static auto savers = base::flat_map<
		not_null<Main::Session*>,
		MediaSaver*>();
	const auto i = savers.find(session);
	if (i != end(savers)) {
		return *i->second;
	}
	const auto result = session->lifetime().make_state<MediaSaver>(
		session);
	savers.emplace(session, result);
	session->lifetime().add([=] {
		savers.remove(session);
	});
	return *result;
}

[[nodiscard]] QString SanitizeSuffix(QString suffix) {
	suffix = suffix.toLower();
	suffix.removeIf([](QChar ch) {
		return !((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'));
	});
	return suffix.left(8);
}

[[nodiscard]] QString DocumentSuffix(
		not_null<DocumentData*> document,
		const QString &local) {
	auto result = SanitizeSuffix(QFileInfo(document->filename()).suffix());
	if (result.isEmpty() && !local.isEmpty()) {
		result = SanitizeSuffix(QFileInfo(local).suffix());
	}
	if (result.isEmpty() && !document->mimeString().isEmpty()) {
		result = SanitizeSuffix(QMimeDatabase().mimeTypeForName(
			document->mimeString()).preferredSuffix());
	}
	if (!result.isEmpty()) {
		return result;
	} else if (const auto sticker = document->sticker()) {
		return sticker->isLottie()
			? u"tgs"_q
			: sticker->isWebm()
			? u"webm"_q
			: u"webp"_q;
	} else if (document->isVoiceMessage()) {
		return u"ogg"_q;
	} else if (document->isVideoMessage()
		|| document->isVideoFile()
		|| document->isAnimation()) {
		return u"mp4"_q;
	}
	return u"bin"_q;
}

[[nodiscard]] QString DocumentType(not_null<DocumentData*> document) {
	if (document->sticker()) {
		return u"sticker"_q;
	} else if (document->isVideoMessage()) {
		return u"round"_q;
	} else if (document->isVoiceMessage()) {
		return u"voice"_q;
	} else if (document->isAnimation()) {
		return u"gif"_q;
	} else if (document->isVideoFile()) {
		return u"video"_q;
	} else if (document->isAudioFile()) {
		return u"audio"_q;
	}
	return u"file"_q;
}

[[nodiscard]] DeletedMedia DescribeMedia(not_null<HistoryItem*> item) {
	auto result = DeletedMedia();
	const auto media = item->media();
	if (!media || media->webpage()) {
		// A link preview is not the message's own media, even though
		// MediaWebPage returns the page photo / document.
		return result;
	} else if (const auto photo = media->photo()) {
		result.type = u"photo"_q;
		result.size = photo->imageByteSize(Data::PhotoSize::Large);
	} else if (const auto document = media->document()) {
		result.type = DocumentType(document);
		result.name = document->filename();
		if (const auto sticker = document->sticker()) {
			if (result.name.isEmpty()) {
				result.name = sticker->alt;
			}
		}
		result.mime = document->mimeString();
		result.size = document->size;
		result.duration = int(document->duration() / 1000);
	} else if (const auto poll = media->poll()) {
		result.type = u"poll"_q;
		result.name = poll->question.text;
	} else if (const auto contact = media->sharedContact()) {
		result.type = u"contact"_q;
		auto parts = QStringList();
		const auto name = (contact->firstName
			+ ' '
			+ contact->lastName).trimmed();
		if (!name.isEmpty()) {
			parts.push_back(name);
		}
		if (!contact->phoneNumber.isEmpty()) {
			parts.push_back(contact->phoneNumber);
		}
		result.name = parts.join(u", "_q);
	} else if (media->location()) {
		result.type = u"location"_q;
	} else {
		result.type = u"other"_q;
		result.name = media->notificationText().text;
	}
	return result;
}

void SaveMediaCopy(
		not_null<HistoryItem*> item,
		const DeletedStore &store,
		DeletedMedia &media) {
	const auto data = item->media();
	if (!data || data->webpage()) {
		return;
	}
	const auto base = u"media/"_q
		+ QString::number(item->history()->peer->id.value)
		+ '_'
		+ QString::number(item->id.bare);
	const auto prepareFolder = [&] {
		QDir().mkpath(store.folder() + u"media"_q);
	};
	if (const auto photo = data->photo()) {
		prepareFolder();
		media.file = base + u".jpg"_q;
		media.thumb = base + u"_thumb.jpg"_q;
		SaverFor(&item->history()->session()).savePhoto(
			photo,
			item->fullId(),
			store.absolutePath(media.file),
			store.absolutePath(media.thumb));
		return;
	}
	const auto document = data->document();
	if (!document) {
		return;
	}
	const auto view = document->activeMediaView();
	if (view) {
		if (const auto thumbnail = view->thumbnail()) {
			prepareFolder();
			media.thumb = base + u"_thumb.jpg"_q;
			const auto image = thumbnail->original();
			const auto path = store.absolutePath(media.thumb);
			crl::async([=] {
				SaveThumbnail(image, path);
			});
		}
	}
	if (document->size > kMaxMediaSize) {
		return;
	}
	const auto local = document->filepath(true);
	const auto bytes = (local.isEmpty() && view && view->loaded())
		? view->bytes()
		: QByteArray();
	if (local.isEmpty() && bytes.isEmpty()) {
		return;
	}
	prepareFolder();
	media.file = base + '.' + DocumentSuffix(document, local);
	const auto target = store.absolutePath(media.file);
	crl::async([=] {
		QFile::remove(target);
		if (!local.isEmpty()) {
			QFile::copy(local, target);
		} else {
			auto output = QFile(target);
			if (output.open(QIODevice::WriteOnly)) {
				output.write(bytes);
				output.close();
			}
		}
	});
}

[[nodiscard]] bool Recordable(not_null<HistoryItem*> item) {
	return !item->isService()
		&& item->isRegular()
		&& !item->isEphemeral()
		&& !item->isScheduled()
		&& !item->isBusinessShortcut()
		&& !item->isSponsored()
		&& IsServerMsgId(item->id);
}

using KeptDeletedMap = base::flat_map<uint64, base::flat_set<FullMsgId>>;

[[nodiscard]] KeptDeletedMap &KeptDeleted() {
	static auto result = KeptDeletedMap();
	return result;
}

} // namespace

DeletedStore::DeletedStore(QString folder)
: _folder(std::move(folder)) {
}

QString DeletedStore::folder() const {
	return _folder;
}

QString DeletedStore::absolutePath(const QString &relative) const {
	return IsSafeRelativePath(relative) ? (_folder + relative) : QString();
}

void DeletedStore::appendLine(const QString &name, const QByteArray &line) {
	QDir().mkpath(_folder);
	auto file = QFile(_folder + name);
	if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
		file.write(line);
		file.close();
	}
}

void DeletedStore::ensureDeletedLoaded() {
	if (_deletedLoaded) {
		return;
	}
	_deletedLoaded = true;
	auto file = QFile(_folder + QString::fromLatin1(kDeletedFile));
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
		auto parsed = ParseDeleted(line);
		if (!parsed) {
			broken = true;
			continue;
		}
		const auto key = Key(parsed->peerId, parsed->messageId);
		if (!_deletedKeys.emplace(key).second) {
			broken = true;
			continue;
		}
		++_deletedPerPeer[parsed->peerId];
		_deleted.push_back(std::move(*parsed));
	}
	if (int(_deleted.size()) > kMaxDeleted) {
		trimDeleted();
		broken = true;
	}
	if (broken) {
		rewriteDeleted();
	}
}

void DeletedStore::trimDeleted() {
	const auto remove = int(_deleted.size()) - kMaxDeleted;
	if (remove <= 0) {
		return;
	}
	for (auto i = 0; i != remove; ++i) {
		const auto &record = _deleted[i];
		removeMediaFiles(record);
		_deletedKeys.erase(Key(record.peerId, record.messageId));
		const auto j = _deletedPerPeer.find(record.peerId);
		if (j != end(_deletedPerPeer) && !--j->second) {
			_deletedPerPeer.erase(j);
		}
	}
	_deleted.erase(begin(_deleted), begin(_deleted) + remove);
}

void DeletedStore::rewriteDeleted() {
	const auto path = _folder + QString::fromLatin1(kDeletedFile);
	if (_deleted.empty()) {
		QFile::remove(path);
		return;
	}
	QDir().mkpath(_folder);
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return;
	}
	for (const auto &record : _deleted) {
		file.write(SerializeDeleted(record));
	}
	file.commit();
}

void DeletedStore::removeMediaFiles(const DeletedRecord &record) {
	for (const auto &relative : { record.media.file, record.media.thumb }) {
		const auto path = absolutePath(relative);
		if (!path.isEmpty()) {
			QFile::remove(path);
		}
	}
}

void DeletedStore::addDeleted(DeletedRecord &&record) {
	ensureDeletedLoaded();
	const auto key = Key(record.peerId, record.messageId);
	if (!_deletedKeys.emplace(key).second) {
		return;
	}
	appendLine(QString::fromLatin1(kDeletedFile), SerializeDeleted(record));
	const auto peerId = record.peerId;
	++_deletedPerPeer[peerId];
	_deleted.push_back(std::move(record));
	const auto trim = (int(_deleted.size()) > kMaxDeleted + kDeletedSlack);
	if (trim) {
		trimDeleted();
		rewriteDeleted();
	}
	_deletedChanges.fire({ .peerId = peerId, .removed = trim });
}

bool DeletedStore::hasDeleted(uint64 peerId) {
	return deletedCount(peerId) > 0;
}

bool DeletedStore::hasDeletedMessage(uint64 peerId, int64 messageId) {
	ensureDeletedLoaded();
	return _deletedKeys.contains(Key(peerId, messageId));
}

int DeletedStore::deletedCount(uint64 peerId) {
	ensureDeletedLoaded();
	if (!peerId) {
		return int(_deleted.size());
	}
	const auto i = _deletedPerPeer.find(peerId);
	return (i != end(_deletedPerPeer)) ? i->second : 0;
}

const std::vector<DeletedRecord> &DeletedStore::deleted() {
	ensureDeletedLoaded();
	return _deleted;
}

void DeletedStore::clearDeleted(uint64 peerId) {
	ensureDeletedLoaded();
	if (!peerId) {
		_deleted.clear();
		_deletedKeys.clear();
		_deletedPerPeer.clear();
		QFile::remove(_folder + QString::fromLatin1(kDeletedFile));
		QDir(_folder + u"media"_q).removeRecursively();
	} else if (_deletedPerPeer.contains(peerId)) {
		for (const auto &record : _deleted) {
			if (record.peerId == peerId) {
				removeMediaFiles(record);
				_deletedKeys.erase(Key(record.peerId, record.messageId));
			}
		}
		_deleted.erase(ranges::remove(
			_deleted,
			peerId,
			&DeletedRecord::peerId), end(_deleted));
		_deletedPerPeer.remove(peerId);
		rewriteDeleted();
	} else {
		return;
	}
	_deletedChanges.fire({ .peerId = peerId, .removed = true });
}

rpl::producer<DeletedChange> DeletedStore::deletedChanges() const {
	return _deletedChanges.events();
}

void DeletedStore::ensureEditsLoaded() {
	if (_editsLoaded) {
		return;
	}
	_editsLoaded = true;
	auto file = QFile(_folder + QString::fromLatin1(kEditsFile));
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
		++_editsLines;
		const auto parsed = FromLine(line);
		const auto peerId = parsed
			? parsed->value(u"p"_q).toString().toULongLong()
			: uint64();
		const auto messageId = parsed
			? int64(parsed->value(u"m"_q).toDouble())
			: int64();
		if (!peerId || !messageId) {
			broken = true;
			continue;
		}
		auto &list = _edits[Key(peerId, messageId)];
		list.push_back({
			.since = TimeId(parsed->value(u"a"_q).toDouble()),
			.replaced = TimeId(parsed->value(u"t"_q).toDouble()),
			.text = ReadText(*parsed),
		});
		++_editsTotal;
		if (int(list.size()) > kMaxVersionsPerMessage) {
			list.erase(begin(list));
			--_editsTotal;
		}
	}
	if (_editsTotal > kMaxEditVersions) {
		trimEdits();
		broken = true;
	}
	if (broken
		|| (_editsLines > std::max(2 * _editsTotal, kMinEditLinesForCompact))) {
		rewriteEdits();
	}
}

void DeletedStore::trimEdits() {
	if (_editsTotal <= kMaxEditVersions) {
		return;
	}
	auto order = std::vector<std::pair<TimeId, Key>>();
	order.reserve(_edits.size());
	for (const auto &[key, list] : _edits) {
		order.emplace_back(list.empty() ? 0 : list.back().replaced, key);
	}
	ranges::sort(order);
	for (const auto &[time, key] : order) {
		if (_editsTotal <= kEditVersionsAfterTrim) {
			break;
		}
		const auto i = _edits.find(key);
		_editsTotal -= int(i->second.size());
		_edits.erase(i);
	}
}

void DeletedStore::rewriteEdits() {
	const auto path = _folder + QString::fromLatin1(kEditsFile);
	_editsLines = 0;
	if (_edits.empty()) {
		QFile::remove(path);
		return;
	}
	QDir().mkpath(_folder);
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return;
	}
	for (const auto &[key, list] : _edits) {
		for (const auto &version : list) {
			file.write(SerializeEdit(key.first, key.second, version));
			++_editsLines;
		}
	}
	file.commit();
}

void DeletedStore::addEdit(
		uint64 peerId,
		int64 messageId,
		EditVersion &&version) {
	ensureEditsLoaded();
	auto &list = _edits[Key(peerId, messageId)];
	if (!list.empty() && list.back().text.text == version.text.text) {
		return;
	}
	appendLine(
		QString::fromLatin1(kEditsFile),
		SerializeEdit(peerId, messageId, version));
	++_editsLines;
	list.push_back(std::move(version));
	++_editsTotal;
	if (int(list.size()) > kMaxVersionsPerMessage) {
		list.erase(begin(list));
		--_editsTotal;
	}
	if (_editsTotal > kMaxEditVersions) {
		trimEdits();
		rewriteEdits();
	} else if (_editsLines
		> std::max(2 * _editsTotal, kMinEditLinesForCompact)) {
		rewriteEdits();
	}
}

std::vector<EditVersion> DeletedStore::edits(
		uint64 peerId,
		int64 messageId) {
	ensureEditsLoaded();
	const auto i = _edits.find(Key(peerId, messageId));
	return (i != end(_edits)) ? i->second : std::vector<EditVersion>();
}

// Oblivion round 5: for the search in the saved messages.
std::vector<EditedMessage> DeletedStore::edited() {
	ensureEditsLoaded();
	auto result = std::vector<EditedMessage>();
	result.reserve(_edits.size());
	for (const auto &[key, list] : _edits) {
		if (!list.empty()) {
			result.push_back({
				.peerId = key.first,
				.messageId = key.second,
				.versions = list,
			});
		}
	}
	return result;
}

void DeletedStore::forget() {
	_deleted.clear();
	_deletedKeys.clear();
	_deletedPerPeer.clear();
	_edits.clear();
	_editsTotal = _editsLines = 0;

	// Treat as loaded, so the files being removed are never read back.
	_deletedLoaded = _editsLoaded = true;

	crl::async([folder = _folder] {
		QDir(folder).removeRecursively();
	});
	_deletedChanges.fire({ .peerId = 0, .removed = true });
}

DeletedStore &StoreFor(not_null<Main::Session*> session) {
	static auto stores = base::flat_map<
		QString,
		std::unique_ptr<DeletedStore>>();
	const auto id = (session->isTestMode() ? u"test_"_q : QString())
		+ QString::number(session->userId().bare);
	auto i = stores.find(id);
	if (i == end(stores)) {
		i = stores.emplace(
			id,
			std::make_unique<DeletedStore>(
				cWorkingDir() + u"tdata/oblivion/"_q + id + '/')).first;
	}
	return *i->second;
}

void RememberDeleted(not_null<HistoryItem*> item) {
	if (!Get().keepDeleted() || !Recordable(item)) {
		return;
	}
	const auto history = item->history();
	auto &store = StoreFor(&history->session());
	const auto peerId = history->peer->id.value;
	if (store.hasDeletedMessage(peerId, item->id.bare)) {
		return;
	}
	const auto from = item->from();
	auto record = DeletedRecord{
		.peerId = peerId,
		.messageId = item->id.bare,
		.topicRootId = item->topicRootId().bare,
		.senderId = from->id.value,
		.senderName = from->name(),
		.chatName = history->peer->name(),
		.date = item->date(),
		.deleted = base::unixtime::now(),
		.text = item->originalText(),
		.media = DescribeMedia(item),
		.out = item->out(),
	};
	if (record.text.text.startsWith(DeletedMark())) {
		return;
	}
	SaveMediaCopy(item, store, record.media);
	store.addDeleted(std::move(record));
}

bool MarkKeptDeleted(not_null<HistoryItem*> item) {
	const auto session = &item->history()->session();
	const auto sessionId = session->uniqueId();
	auto &map = KeptDeleted();
	if (!map.contains(sessionId)) {
		session->lifetime().add([=] {
			KeptDeleted().remove(sessionId);
		});
	}
	return map[sessionId].emplace(item->fullId()).second;
}

bool IsKeptDeleted(not_null<const HistoryItem*> item) {
	const auto &map = KeptDeleted();
	if (map.empty()) {
		return false;
	}
	const auto i = map.find(item->history()->session().uniqueId());
	return (i != end(map)) && i->second.contains(item->fullId());
}

HistoryItemsList WithoutKeptDeleted(HistoryItemsList items) {
	if (!KeptDeleted().empty()) {
		items.erase(ranges::remove_if(items, [](not_null<HistoryItem*> item) {
			return IsKeptDeleted(item);
		}), end(items));
	}
	return items;
}

void ForgetDeleted(not_null<Main::Session*> session) {
	StoreFor(session).forget();
}

EditHistory &EditHistory::Instance() {
	static auto result = EditHistory();
	return result;
}

void EditHistory::remember(
		not_null<HistoryItem*> item,
		const HistoryMessageEdition &edition) {
	if (!Get().keepEditHistory() || !Recordable(item)) {
		return;
	}
	const auto &was = item->originalText();
	const auto &now = edition.textWithEntities;
	if (was.text.isEmpty()
		|| (was.text == now.text)
		|| was.text.startsWith(DeletedMark())
		|| (now.text == DeletedMark() + was.text)) {
		return;
	} else if (Listen::IsControlEdit(item, was, now)) {
		// Round 4, listen together: the host of a session edits its
		// control message when the track changes and when the session
		// ends, these are not edits made by a person. Only an edit from
		// one valid state of a session to its next one is left out, an
		// ordinary message can't be hidden by a link added to it.
		return;
	}
	const auto edited = item->Get<HistoryMessageEdited>();
	auto version = EditVersion{
		.since = (edited && edited->date > 0) ? edited->date : item->date(),
		.replaced = (edition.editDate > 0)
			? edition.editDate
			: base::unixtime::now(),
		.text = was,
	};
	StoreFor(&item->history()->session()).addEdit(
		item->history()->peer->id.value,
		item->id.bare,
		std::move(version));
}

std::vector<EditVersion> EditHistory::versions(
		not_null<HistoryItem*> item) const {
	if (!Recordable(item)) {
		return {};
	}
	return StoreFor(&item->history()->session()).edits(
		item->history()->peer->id.value,
		item->id.bare);
}

} // namespace Oblivion
