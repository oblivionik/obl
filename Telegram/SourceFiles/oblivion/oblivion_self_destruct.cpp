/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_self_destruct.h"

#include "base/platform/base_platform_info.h"
#include "base/unixtime.h"
#include "core/file_utilities.h"
#include "core/mime_type.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "oblivion/oblivion_deleted.h"
#include "oblivion/oblivion_interface.h"
#include "oblivion/oblivion_settings.h"
#include "settings.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

namespace Oblivion {
namespace {

constexpr auto kMaxChatNameLength = 64;
constexpr auto kSavedKey = "saved";

[[nodiscard]] QString StoragePath() {
	return cWorkingDir() + u"tdata/oblivion/self_destruct.json"_q;
}

[[nodiscard]] QString ItemKey(not_null<const HistoryItem*> item) {
	const auto history = item->history();
	return QString::number(history->session().userId().bare)
		+ ':'
		+ QString::number(history->peer->id.value)
		+ ':'
		+ QString::number(item->id.bare);
}

[[nodiscard]] QString SafeFileName(QString name) {
	static const auto kBad = QRegularExpression(
		u"[\\\\/:*?\"<>|\\x00-\\x1F]"_q);
	name.replace(kBad, u"_"_q);
	name = name.simplified();
	while (name.startsWith('.')) {
		name.remove(0, 1);
	}
	if (name.size() > kMaxChatNameLength) {
		auto cut = kMaxChatNameLength;
		if (name.at(cut - 1).isHighSurrogate()) {
			--cut;
		}
		name = name.mid(0, cut).trimmed();
	}
	return name;
}

[[nodiscard]] QString BaseFileName(not_null<HistoryItem*> item) {
	auto chat = SafeFileName(item->history()->peer->name());
	if (chat.isEmpty()) {
		chat = u"Oblivion"_q;
	}
	const auto date = base::unixtime::parse(item->date()).toString(
		u"yyyy-MM-dd HH-mm-ss"_q);
	return chat + ' ' + date;
}

[[nodiscard]] QString DocumentExtension(not_null<DocumentData*> document) {
	const auto suffix = SafeFileName(
		QFileInfo(document->filename()).suffix());
	if (!suffix.isEmpty()) {
		return '.' + suffix;
	} else if (document->isVoiceMessage()) {
		return u".ogg"_q;
	} else if (document->isVideoMessage() || document->isVideoFile()) {
		return u".mp4"_q;
	}
	const auto patterns = Core::MimeTypeForName(
		document->mimeString()).globPatterns();
	for (const auto &pattern : patterns) {
		if (pattern.startsWith(u"*."_q) && pattern.size() > 2) {
			return pattern.mid(1);
		}
	}
	return QString();
}

[[nodiscard]] QString SaveFolder(not_null<Main::Session*> session) {
	// Downloads/Oblivion, the default download folder of the app.
	auto result = File::DefaultDownloadPath(session);
	if (!result.endsWith('/')) {
		result += '/';
	}
	QDir().mkpath(result);
	return result;
}

class SavedRegistry final {
public:
	[[nodiscard]] static SavedRegistry &Instance() {
		static auto result = SavedRegistry();
		return result;
	}

	[[nodiscard]] bool contains(const QString &key) {
		load();
		return _paths.contains(key);
	}
	[[nodiscard]] QString path(const QString &key) {
		load();
		const auto i = _paths.find(key);
		return (i != end(_paths)) ? i->second : QString();
	}
	void remember(const QString &key, const QString &path) {
		load();
		_paths[key] = path;
		save();
	}

private:
	SavedRegistry() = default;

	void load() {
		if (_loaded) {
			return;
		}
		_loaded = true;
		auto file = QFile(StoragePath());
		if (!file.open(QIODevice::ReadOnly)) {
			return;
		}
		const auto document = QJsonDocument::fromJson(file.readAll());
		const auto saved = document.object().value(
			QString::fromLatin1(kSavedKey)).toObject();
		for (auto i = saved.begin(); i != saved.end(); ++i) {
			const auto path = i.value().toString();
			if (!path.isEmpty()) {
				_paths.emplace(i.key(), path);
			}
		}
	}

	void save() const {
		auto saved = QJsonObject();
		for (const auto &[key, path] : _paths) {
			saved.insert(key, path);
		}
		auto object = QJsonObject();
		object.insert(QString::fromLatin1(kSavedKey), saved);

		const auto path = StoragePath();
		QDir().mkpath(QFileInfo(path).absolutePath());
		auto file = QSaveFile(path);
		if (file.open(QIODevice::WriteOnly)) {
			file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
			file.commit();
		}
	}

	base::flat_map<QString, QString> _paths;
	bool _loaded = false;

};

struct Task {
	Task(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		FullMsgId itemId,
		QString key,
		QString baseName)
	: session(session)
	, peer(peer)
	, itemId(itemId)
	, key(std::move(key))
	, baseName(std::move(baseName)) {
	}

	const not_null<Main::Session*> session;
	const not_null<PeerData*> peer;
	const FullMsgId itemId;
	const QString key;
	const QString baseName;
	QString path;
	std::shared_ptr<Data::PhotoMedia> photo;
	std::shared_ptr<Data::DocumentMedia> document;
	bool finished = false;
	rpl::lifetime lifetime;
};

class Saver final {
public:
	[[nodiscard]] static Saver &Instance() {
		static auto result = Saver();
		return result;
	}

	void enqueue(not_null<HistoryItem*> item, uint64 contentId) {
		const auto session = &item->history()->session();
		const auto itemId = item->fullId();

		// The item is still being constructed here, process it later.
		crl::on_main(session, [=] {
			process(session, itemId, contentId);
		});
	}

private:
	Saver() = default;

	void process(
		not_null<Main::Session*> session,
		FullMsgId itemId,
		uint64 contentId);
	void startPhoto(not_null<Task*> task, not_null<PhotoData*> photo);
	void startDocument(
		not_null<Task*> task,
		not_null<DocumentData*> document);
	void finish(not_null<Task*> task, const QString &path);
	void remove(not_null<Main::Session*> session, const QString &key);
	void track(not_null<Main::Session*> session);

	[[nodiscard]] QString reservePath(
		not_null<Main::Session*> session,
		const QString &baseName,
		const QString &extension);

	base::flat_map<
		not_null<Main::Session*>,
		base::flat_map<QString, std::unique_ptr<Task>>> _tasks;
	base::flat_set<not_null<Main::Session*>> _tracked;
	base::flat_set<QString> _reservedPaths;

};

void Saver::process(
		not_null<Main::Session*> session,
		FullMsgId itemId,
		uint64 contentId) {
	if (!Get().saveSelfDestructing()) {
		return;
	}
	const auto item = session->data().message(itemId);
	if (!item || item->out() || !item->isRegular()) {
		return;
	}

	// Make sure it is the media with ttl_seconds and not, for example,
	// the media of an external reply created for the same item.
	const auto media = item->media();
	const auto photo = media ? media->photo() : nullptr;
	const auto document = media ? media->document() : nullptr;
	const auto goodPhoto = photo
		&& !photo->isNull()
		&& (photo->id == contentId);
	const auto goodDocument = document && (document->id == contentId);
	if (!goodPhoto && !goodDocument) {
		return;
	}
	auto key = ItemKey(item);
	if (SavedRegistry::Instance().contains(key)) {
		return;
	}
	track(session);
	auto &tasks = _tasks[session];
	if (tasks.contains(key)) {
		return;
	}
	const auto task = tasks.emplace(
		key,
		std::make_unique<Task>(
			session,
			item->history()->peer,
			itemId,
			key,
			BaseFileName(item))).first->second.get();
	if (goodPhoto) {
		startPhoto(task, photo);
	} else {
		startDocument(task, document);
	}
}

void Saver::startPhoto(not_null<Task*> task, not_null<PhotoData*> photo) {
	using Data::PhotoSize;

	task->photo = photo->createMediaView();
	const auto check = [=] {
		if (task->finished) {
			return;
		} else if (task->photo->loaded()) {
			const auto video = !task->photo->videoContent(
				PhotoSize::Large).isEmpty();
			task->path = reservePath(
				task->session,
				task->baseName,
				video ? u".mp4"_q : u".jpg"_q);
			const auto saved = task->photo->saveToFile(task->path);
			finish(task, saved ? task->path : QString());
		} else if (photo->failed(PhotoSize::Large)
			&& !photo->loading(PhotoSize::Large)) {
			finish(task, QString());
		}
	};
	task->session->data().photoLoadProgress(
	) | rpl::filter([=](not_null<PhotoData*> updated) {
		return (updated == photo);
	}) | rpl::on_next(check, task->lifetime);

	photo->clearFailed(PhotoSize::Large);
	task->photo->wanted(PhotoSize::Large, task->itemId);
	check();
}

void Saver::startDocument(
		not_null<Task*> task,
		not_null<DocumentData*> document) {
	task->document = document->createMediaView();
	task->path = reservePath(
		task->session,
		task->baseName,
		DocumentExtension(document));
	const auto check = [=] {
		if (task->finished || document->loading()) {
			return;
		}
		const auto info = QFileInfo(task->path);
		const auto good = !document->cancelled()
			&& info.exists()
			&& (info.size() > 0);
		finish(task, good ? task->path : QString());
	};

	// Writes the file right away if the document is already loaded,
	// otherwise starts (or redirects) a loader to this path. Subscribe
	// after that, so a cancelled cache-only loader is not a failure.
	document->save(task->itemId, task->path);
	task->session->data().documentLoadProgress(
	) | rpl::filter([=](not_null<DocumentData*> updated) {
		return (updated == document);
	}) | rpl::on_next(check, task->lifetime);
	check();
}

void Saver::finish(not_null<Task*> task, const QString &path) {
	if (task->finished) {
		return;
	}
	task->finished = true;
	if (!path.isEmpty()) {
		SavedRegistry::Instance().remember(task->key, path);
	}
	if (const auto window = ExistingWindow(task->session, task->peer)) {
		window->showToast(path.isEmpty()
			? tr::lng_oblivion_self_destructing_failed(tr::now)
			: tr::lng_oblivion_self_destructing_saved(
				tr::now,
				lt_path,
				QDir::toNativeSeparators(path)));
	}

	// We may be inside a callback owned by the task, destroy it later.
	const auto session = task->session;
	const auto key = task->key;
	crl::on_main(session.get(), [=] {
		remove(session, key);
	});
}

void Saver::remove(not_null<Main::Session*> session, const QString &key) {
	const auto i = _tasks.find(session);
	if (i == end(_tasks)) {
		return;
	}
	const auto j = i->second.find(key);
	if (j == end(i->second)) {
		return;
	}
	if (!j->second->path.isEmpty()) {
		_reservedPaths.remove(j->second->path);
	}
	i->second.erase(j);
}

void Saver::track(not_null<Main::Session*> session) {
	if (_tracked.contains(session)) {
		return;
	}
	_tracked.emplace(session);
	session->lifetime().add([=] {
		if (const auto i = _tasks.find(session); i != end(_tasks)) {
			for (const auto &[key, task] : i->second) {
				_reservedPaths.remove(task->path);
			}
			_tasks.erase(i);
		}
		_tracked.remove(session);
	});
}

QString Saver::reservePath(
		not_null<Main::Session*> session,
		const QString &baseName,
		const QString &extension) {
	const auto folder = SaveFolder(session);
	auto result = folder + baseName + extension;
	for (auto i = 2
		; QFileInfo::exists(result) || _reservedPaths.contains(result)
		; ++i) {
		result = folder + baseName + u" (%1)"_q.arg(i) + extension;
	}
	_reservedPaths.emplace(result);
	return result;
}

[[nodiscard]] bool IsExpired(const MTPMessageMedia &media) {
	return media.match([](const MTPDmessageMediaPhoto &data) -> bool {
		const auto photo = data.vphoto();
		return data.vttl_seconds()
			&& (!photo || (photo->type() == mtpc_photoEmpty));
	}, [](const MTPDmessageMediaDocument &data) -> bool {
		const auto document = data.vdocument();
		return data.vttl_seconds()
			&& (!document || (document->type() == mtpc_documentEmpty));
	}, [](const auto &) -> bool {
		return false;
	});
}

} // namespace

bool AcceptSelfDestructingMedia(
		not_null<HistoryItem*> item,
		const MTPDmessageMediaPhoto &media) {
	const auto photo = media.vphoto();
	if (!media.vttl_seconds() || !photo || !Get().saveSelfDestructing()) {
		return false;
	}
	return photo->match([&](const MTPDphoto &data) {
		Saver::Instance().enqueue(item, data.vid().v);
		return true;
	}, [](const MTPDphotoEmpty &) {
		// The expired placeholder is handled as in upstream.
		return false;
	});
}

bool AcceptSelfDestructingMedia(
		not_null<HistoryItem*> item,
		const MTPDmessageMediaDocument &media) {
	const auto document = media.vdocument();
	if (!media.vttl_seconds()
		|| !document
		|| !Get().saveSelfDestructing()) {
		return false;
	}
	return document->match([&](const MTPDdocument &data) {
		Saver::Instance().enqueue(item, data.vid().v);
		return true;
	}, [](const MTPDdocumentEmpty &) {
		// The expired placeholder is handled as in upstream.
		return false;
	});
}

bool KeepSelfDestructingMedia(
		not_null<HistoryItem*> item,
		const MTPMessageMedia *media) {
	if (!media || !Get().saveSelfDestructing()) {
		return false;
	}
	const auto current = item->media();
	if (!current || (!current->photo() && !current->document())) {
		return false;
	}
	return IsExpired(*media);
}

bool KeepPlayedSelfDestructingMedia(not_null<HistoryItem*> item) {
	if (!Get().saveSelfDestructing()) {
		return false;
	}
	// The played view drops its open link, recreate it.
	item->history()->owner().requestItemViewRefresh(item);
	return true;
}

QString SelfDestructingSavedPath(not_null<const HistoryItem*> item) {
	if (!item->isRegular()) {
		return QString();
	}
	const auto path = SavedRegistry::Instance().path(ItemKey(item));
	return (!path.isEmpty() && QFileInfo::exists(path)) ? path : QString();
}

void AddSelfDestructingActions(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item) {
	const auto path = SelfDestructingSavedPath(item);
	if (path.isEmpty()) {
		return;
	}
	const auto show = controller->uiShow();
	menu->addAction(
		tr::lng_oblivion_self_destructing_open(tr::now),
		[=] { OpenSavedFile(show, path); },
		&st::menuIconFile);
	menu->addAction(
		(Platform::IsMac()
			? tr::lng_oblivion_self_destructing_finder(tr::now)
			: tr::lng_oblivion_self_destructing_folder(tr::now)),
		[=] { File::ShowInFolder(path); },
		&st::menuIconShowInFolder);
}

} // namespace Oblivion
