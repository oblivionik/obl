/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_media_save.h"

#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "core/file_location.h"
#include "core/file_utilities.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "oblivion/oblivion_audio.h"
#include "oblivion/oblivion_interface.h"
#include "platform/platform_file_utilities.h"
#include "settings.h"
#include "storage/file_download.h"
#include "storage/storage_account.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"

#include <crl/crl_async.h>

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>

namespace Oblivion {
namespace {

constexpr auto kMaxSenderLength = 64;

enum class Kind : uchar {
	Voice,
	Round,
};

enum class Result : uchar {
	Saved,
	DownloadFailed,
	ConvertFailed,
	WriteFailed,
};

// Who has recorded the message and when: for a forwarded message that
// is the original sender and the original date.
struct Meta {
	QString sender;
	QDateTime date;
};

[[nodiscard]] Meta MetaFor(not_null<HistoryItem*> item) {
	auto result = Meta();
	if (const auto sender = item->originalSender()) {
		result.sender = sender->name();
	} else if (const auto hidden = item->originalHiddenSenderInfo()) {
		result.sender = hidden->name;
	}
	auto date = item->date();
	if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
		if (forwarded->originalDate > 0) {
			date = forwarded->originalDate;
		}
	}
	result.sender = result.sender.simplified();
	result.date = base::unixtime::parse(date);
	return result;
}

[[nodiscard]] QString SafeFileName(QString name) {
	static const auto forbidden = u"/\\:*?\"<>|"_q;
	for (auto &ch : name) {
		if (ch.unicode() < 32 || forbidden.contains(ch)) {
			ch = QChar('_');
		}
	}
	name = name.simplified();
	while (name.startsWith('.')) {
		name.remove(0, 1);
	}
	return name;
}

// "Voice message Ivan Petrov 2026-10-01 14-32-05": no dots and colons
// in the time, the only dot of the name is the one of the extension.
[[nodiscard]] QString FileBaseName(Kind kind, const Meta &meta) {
	const auto sender = meta.sender.left(kMaxSenderLength);
	const auto date = meta.date.toString(u"yyyy-MM-dd HH-mm-ss"_q);
	return SafeFileName((kind == Kind::Voice)
		? tr::lng_oblivion_save_voice_name(
			tr::now,
			lt_name,
			sender,
			lt_date,
			date)
		: tr::lng_oblivion_save_round_name(
			tr::now,
			lt_name,
			sender,
			lt_date,
			date));
}

// The title written into the tags of the audio file.
[[nodiscard]] QString TagTitle(const Meta &meta) {
	const auto date = QLocale().toString(meta.date, QLocale::ShortFormat);
	return meta.sender.isEmpty() ? date : (meta.sender + ' ' + date);
}

[[nodiscard]] QString FileFilter(const QString &name, const QString &mask) {
	return name + u" ("_q + mask + u")"_q;
}

[[nodiscard]] QString VoiceFilters() {
	return QStringList{
		FileFilter(tr::lng_oblivion_music_file_m4a(tr::now), u"*.m4a"_q),
		FileFilter(tr::lng_oblivion_music_file_wav(tr::now), u"*.wav"_q),
		FileFilter(tr::lng_oblivion_music_file_ogg(tr::now), u"*.ogg"_q),
	}.join(u";;"_q);
}

[[nodiscard]] QString SuggestedPath(const QString &fileName) {
	if (cDialogLastPath().isEmpty()) {
		Platform::FileDialog::InitLastPath();
	}
	return filedialogNextFilename(fileName, QString());
}

// The extension was not confirmed in the dialog, so nothing existing
// may be replaced by the file with it.
[[nodiscard]] QString WithExtension(
		const QString &path,
		const QString &extension) {
	auto result = path + '.' + extension;
	for (auto i = 2; QFileInfo::exists(result); ++i) {
		result = path + u" (%1)."_q.arg(i) + extension;
	}
	return result;
}

// The format is chosen in the dialog by the extension, M4A is used for
// everything unknown (the extension is added then).
[[nodiscard]] Audio::Format VoiceFormatFor(QString &path) {
	const auto suffix = QFileInfo(path).suffix().toLower();
	if (suffix == u"wav"_q) {
		return Audio::Format::Wav;
	} else if (suffix == u"ogg"_q
		|| suffix == u"oga"_q
		|| suffix == u"opus"_q) {
		return Audio::Format::OggOpus;
	} else if (suffix != u"m4a"_q) {
		path = WithExtension(
			path,
			Audio::FormatExtension(Audio::Format::M4a));
	}
	return Audio::Format::M4a;
}

// Removes a partially written file.
[[nodiscard]] bool WriteFile(const QString &path, const QByteArray &bytes) {
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return false;
	} else if ((file.write(bytes) != bytes.size()) || !file.flush()) {
		file.close();
		file.remove();
		return false;
	}
	return true;
}

// QFileInfo compares the canonical paths the way the file system does:
// on Windows canonicalFilePath() keeps the letter case it was given,
// while names that differ only by the case are the same file there.
[[nodiscard]] bool SamePath(const QString &a, const QString &b) {
	const auto first = QFileInfo(a);
	return first.exists() && (first == QFileInfo(b));
}

// Any thread. A voice message saved as OGG stays as it was recorded,
// without one more lossy encoding (and so without the tags).
[[nodiscard]] Result ConvertVoice(
		const QByteArray &bytes,
		Audio::Format format,
		const QString &title,
		const QString &performer,
		const QString &path) {
	auto encoded = QByteArray();
	if ((format == Audio::Format::OggOpus) && bytes.startsWith("OggS")) {
		encoded = bytes;
	} else {
		const auto decoded = Audio::Decode(bytes);
		if (!decoded || decoded->empty()) {
			return Result::ConvertFailed;
		}
		encoded = Audio::Encode(*decoded, format, {
			.title = title,
			.performer = performer,
		});
		if (encoded.isEmpty()) {
			return Result::ConvertFailed;
		}
	}
	return WriteFile(path, encoded) ? Result::Saved : Result::WriteFailed;
}

struct Request {
	Kind kind = Kind::Voice;
	not_null<DocumentData*> document;
	not_null<PeerData*> peer;
	FullMsgId itemId;
	QString path;
	Audio::Format format = Audio::Format::M4a; // Kind::Voice only.
	QString title; // Tags, Kind::Voice only.
	QString performer;
};

// Downloads what is not downloaded yet and writes the files, main
// thread only. A task lives until its file is saved (or failed) or
// until its session is gone, the window that has started it may be
// closed meanwhile.
class Saver final {
public:
	[[nodiscard]] static Saver &Instance();

	void start(Request &&request);

private:
	struct Task {
		Task(uint64 id, Request &&request)
		: id(id)
		, request(std::move(request))
		, session(&this->request.document->session()) {
		}

		const uint64 id = 0;
		const Request request;
		const not_null<Main::Session*> session;
		std::shared_ptr<Data::DocumentMedia> media;
		QString tempPath; // A large voice message goes through a file.
		bool working = false; // Loaded, the file is being written.
		bool finished = false;
		rpl::lifetime lifetime;
	};

	void track(not_null<Main::Session*> session);
	[[nodiscard]] Task *find(uint64 id) const;
	void download(not_null<Task*> task);
	void check(uint64 id);
	void save(not_null<Task*> task);
	void saveRound(not_null<Task*> task);
	void saveVoice(not_null<Task*> task);
	void finish(uint64 id, Result result);

	base::flat_map<uint64, std::unique_ptr<Task>> _tasks;
	base::flat_set<not_null<Main::Session*>> _tracked;
	uint64 _lastId = 0;

};

Saver &Saver::Instance() {
	static auto result = Saver();
	return result;
}

void Saver::track(not_null<Main::Session*> session) {
	if (!_tracked.emplace(session).second) {
		return;
	}
	session->lifetime().add([=] {
		for (auto i = begin(_tasks); i != end(_tasks);) {
			if (i->second->session == session) {
				i = _tasks.erase(i);
			} else {
				++i;
			}
		}
		_tracked.remove(session);
	});
}

Saver::Task *Saver::find(uint64 id) const {
	const auto i = _tasks.find(id);
	return (i != end(_tasks)) ? i->second.get() : nullptr;
}

void Saver::start(Request &&request) {
	const auto id = ++_lastId;
	const auto task = _tasks.emplace(
		id,
		std::make_unique<Task>(id, std::move(request))).first->second.get();
	track(task->session);
	task->media = task->request.document->createMediaView();
	if (task->media->loaded(true)) {
		save(task);
	} else {
		download(task);
	}
}

void Saver::download(not_null<Task*> task) {
	const auto id = task->id;
	const auto session = task->session;
	const auto document = task->request.document;
	auto target = QString();
	if (task->request.kind == Kind::Round) {
		// As the usual "Save as": the loader writes the chosen file.
		target = task->request.path;
	} else if (document->size >= Storage::kMaxFileInMemory) {
		// FileLoader keeps in memory only the files below that size
		// (and asserts on larger ones without a target file).
		const auto folder = QDir(session->local().tempDirectory()).filePath(
			u"oblivion_save"_q);
		if (!QDir().mkpath(folder)) {
			finish(id, Result::DownloadFailed);
			return;
		}
		target = QDir(folder).filePath(
			QString::number(document->id) + u".ogg"_q);
		task->tempPath = target;
	}

	// Subscribe after that: redirecting a loader that already works may
	// cancel it first, that is not a failure.
	document->save(task->request.itemId, target);
	session->data().documentLoadProgress(
	) | rpl::filter([=](not_null<DocumentData*> updated) {
		return (updated == document);
	}) | rpl::on_next([=] {
		check(id);
	}, task->lifetime);
	check(id);

	if (const auto now = find(id); now && !now->working && !now->finished) {
		if (const auto window = ExistingWindow(session, now->request.peer)) {
			window->showToast(tr::lng_oblivion_save_downloading(tr::now));
		}
	}
}

void Saver::check(uint64 id) {
	const auto task = find(id);
	if (!task
		|| task->working
		|| task->finished
		|| task->request.document->loading()) {
		return;
	} else if (task->media->loaded(true)) {
		save(task);
	} else {
		finish(id, Result::DownloadFailed);
	}
}

void Saver::save(not_null<Task*> task) {
	task->working = true;
	if (task->request.kind == Kind::Round) {
		saveRound(task);
	} else {
		saveVoice(task);
	}
}

void Saver::saveRound(not_null<Task*> task) {
	const auto &path = task->request.path;
	const auto document = task->request.document;
	const auto bytes = task->media->bytes();
	const auto existing = document->filepath(true);
	if (!bytes.isEmpty()) {
		finish(
			task->id,
			WriteFile(path, bytes) ? Result::Saved : Result::WriteFailed);
	} else if (existing.isEmpty()) {
		finish(task->id, Result::DownloadFailed);
	} else if (SamePath(existing, path)) {
		// The loader has written the chosen file itself.
		finish(task->id, Result::Saved);
	} else {
		const auto &location = document->location(true);
		auto copied = false;
		if (location.accessEnable()) {
			QFile::remove(path);
			copied = QFile::copy(location.name(), path);
			location.accessDisable();
		}
		finish(task->id, copied ? Result::Saved : Result::WriteFailed);
	}
}

void Saver::saveVoice(not_null<Task*> task) {
	const auto id = task->id;
	const auto document = task->request.document;
	auto bytes = task->media->bytes();
	if (bytes.isEmpty()) {
		const auto &location = document->location(true);
		if (location.accessEnable()) {
			auto file = QFile(location.name());
			if (file.open(QIODevice::ReadOnly)) {
				bytes = file.readAll();
			}
			location.accessDisable();
		}
	}
	if (!task->tempPath.isEmpty()) {
		// Only this task needed that file. Forget it, as if the user
		// has removed it.
		QFile::remove(task->tempPath);
		(void)document->location(true);
	}
	if (bytes.isEmpty()) {
		finish(id, Result::DownloadFailed);
		return;
	}
	const auto weak = base::make_weak(task->session.get());
	const auto format = task->request.format;
	const auto title = task->request.title;
	const auto performer = task->request.performer;
	const auto path = task->request.path;
	crl::async([=, bytes = std::move(bytes)] {
		const auto result = ConvertVoice(
			bytes,
			format,
			title,
			performer,
			path);
		crl::on_main(weak, [=] {
			Instance().finish(id, result);
		});
	});
}

void Saver::finish(uint64 id, Result result) {
	const auto task = find(id);
	if (!task || task->finished) {
		return;
	}
	task->finished = true;
	const auto session = task->session;
	if (const auto window = ExistingWindow(session, task->request.peer)) {
		const auto path = QDir::toNativeSeparators(task->request.path);
		window->showToast([&] {
			switch (result) {
			case Result::Saved:
				return tr::lng_oblivion_saved_to(tr::now, lt_path, path);
			case Result::DownloadFailed:
				return tr::lng_oblivion_save_download_failed(tr::now);
			case Result::ConvertFailed:
				return tr::lng_oblivion_save_convert_failed(tr::now);
			case Result::WriteFailed:
				return tr::lng_oblivion_save_write_failed(
					tr::now,
					lt_path,
					path);
			}
			Unexpected("Result in Oblivion::Saver::finish.");
		}());
	}

	// This may be called from a subscription owned by the task.
	crl::on_main(session.get(), [=] {
		_tasks.remove(id);
	});
}

void ChoosePathAndSave(
		not_null<Window::SessionController*> controller,
		FullMsgId itemId) {
	const auto item = controller->session().data().message(itemId);
	const auto media = item ? item->media() : nullptr;
	const auto document = media ? media->document() : nullptr;
	if (!document
		|| (!document->isVoiceMessage() && !document->isVideoMessage())) {
		return;
	}
	const auto kind = document->isVoiceMessage() ? Kind::Voice : Kind::Round;
	const auto voice = (kind == Kind::Voice);
	const auto meta = MetaFor(item);
	const auto peer = item->history()->peer;
	const auto title = TagTitle(meta);
	const auto performer = meta.sender;
	const auto extension = voice
		? Audio::FormatExtension(Audio::Format::M4a)
		: u"mp4"_q;
	FileDialog::GetWritePath(
		controller->widget().get(),
		(voice
			? tr::lng_oblivion_save_voice_title(tr::now)
			: tr::lng_oblivion_save_round_title(tr::now)),
		(voice
			? VoiceFilters()
			: FileFilter(tr::lng_oblivion_save_file_mp4(tr::now), u"*.mp4"_q)),
		SuggestedPath(FileBaseName(kind, meta) + '.' + extension),
		crl::guard(controller, [=](QString &&result) {
			if (result.isEmpty()) {
				return;
			}
			auto path = std::move(result);
			auto format = Audio::Format::M4a;
			if (voice) {
				format = VoiceFormatFor(path);
			} else if (QFileInfo(path).suffix().isEmpty()) {
				path = WithExtension(path, extension);
			}
			Saver::Instance().start({
				.kind = kind,
				.document = document,
				.peer = peer,
				.itemId = itemId,
				.path = path,
				.format = format,
				.title = title,
				.performer = performer,
			});
		}));
}

} // namespace

void AddMediaSaveActions(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item) {
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;

	// Some lists show items of all accounts, while the action looks
	// the message up again in the controller's session.
	if (!document
		|| (&item->history()->session() != &controller->session())
		|| media->ttlSeconds()
		|| item->forbidsSaving()
		|| item->isSending()
		|| item->hasFailed()
		|| (!document->isVoiceMessage() && !document->isVideoMessage())) {
		return;
	}
	const auto itemId = item->fullId();
	menu->addAction(
		(document->isVoiceMessage()
			? tr::lng_oblivion_save_voice_as(tr::now)
			: tr::lng_oblivion_save_round_as(tr::now)),
		crl::guard(controller, [=] {
			ChoosePathAndSave(controller, itemId);
		}),
		&st::menuIconDownload);
}

} // namespace Oblivion
