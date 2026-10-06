/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_sticker_export.h"

#include "base/platform/base_platform_info.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "base/zlib_help.h"
#include "chat_helpers/compose/compose_show.h"
#include "core/file_location.h"
#include "core/file_utilities.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_session.h"
#include "data/stickers/data_stickers.h"
#include "data/stickers/data_stickers_set.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mtproto/sender.h"
#include "oblivion/oblivion_interface.h"
#include "oblivion/oblivion_lottie.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "oblivion/oblivion_video_core.h"
#include "platform/platform_file_utilities.h"
#include "settings.h"
#include "storage/file_download.h"
#include "ui/image/image_prepare.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/menu/menu.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/shadow.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <crl/crl_async.h>

#include <QtCore/QBuffer>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QSaveFile>
#include <QtGui/QImageWriter>
#include <QtWidgets/QMenu>

namespace Oblivion::StickerExport {
namespace {

using Cancel = VideoCore::Cancel;

// GIF made of an animated or a video sticker.
constexpr auto kGifSide = 512;
constexpr auto kGifMaxFps = 30;

// File names.
constexpr auto kNameEmojiLimit = 24;
constexpr auto kNameLimit = 80;
constexpr auto kIndexDigits = 3;

// The files of a set are downloaded strictly one after another, with
// a pause after each one that really came from the network: what took
// longer than a read from the local cache does.
constexpr auto kNetworkPause = crl::time(120);
constexpr auto kCachedThreshold = crl::time(20);

// Archive: names are UTF-8 (general purpose bit 11), sticker files are
// stored as they are (they are compressed already).
//
// The archive says it was made on Unix whatever the system is: the unzip
// of macOS recodes the names of an "MS-DOS" archive as if they were in
// an old code page and then can't create the files with emoji and
// Cyrillic names (checked), bit 11 doesn't stop it. A Unix archive needs
// the access rights of the files, or they are extracted unreadable.
constexpr auto kZipUtf8Flag = 0x800;
constexpr auto kZipMadeBy = (3 << 8) | 20; // Unix, format 2.0.
constexpr auto kZipFileMode = 0100644; // A regular file, rw-r--r--.
constexpr auto kZipMemLevel = 8;
constexpr auto kZipWriteChunk = 1024 * 1024;
constexpr auto kManifestVersion = 1;

// Around a menu in a UI snapshot scene.
constexpr auto kSceneMenuSkip = 12;

enum class Kind : uchar {
	Static,
	Animated,
	Video,
};

enum class Format : uchar {
	Png,
	Webp,
	Tgs,
	Json,
	Gif,
	Webm,
};

enum class SetType : uchar {
	Stickers,
	Emoji,
	Masks,
};

[[nodiscard]] QString Extension(Format format) {
	switch (format) {
	case Format::Png: return u"png"_q;
	case Format::Webp: return u"webp"_q;
	case Format::Tgs: return u"tgs"_q;
	case Format::Json: return u"json"_q;
	case Format::Gif: return u"gif"_q;
	case Format::Webm: return u"webm"_q;
	}
	Unexpected("Format in StickerExport::Extension.");
}

// The format of the file as Telegram keeps it.
[[nodiscard]] Format OriginalFormat(Kind kind, const QString &mime) {
	switch (kind) {
	case Kind::Static:
		return (mime.trimmed().toLower() == u"image/png"_q)
			? Format::Png
			: Format::Webp;
	case Kind::Animated: return Format::Tgs;
	case Kind::Video: return Format::Webm;
	}
	Unexpected("Kind in StickerExport::OriginalFormat.");
}

// What "Save as" offers, in the order of the menu.
[[nodiscard]] std::vector<Format> FormatsFor(Kind kind, Format original) {
	switch (kind) {
	case Kind::Static:
		return (original == Format::Png)
			? std::vector<Format>{ Format::Png }
			: std::vector<Format>{ Format::Png, Format::Webp };
	case Kind::Animated:
		return { Format::Tgs, Format::Json, Format::Gif };
	case Kind::Video:
		return { Format::Webm, Format::Gif };
	}
	Unexpected("Kind in StickerExport::FormatsFor.");
}

[[nodiscard]] QString ManifestName() {
	return u"manifest.json"_q;
}

[[nodiscard]] QString KindName(Kind kind) {
	switch (kind) {
	case Kind::Static: return u"static"_q;
	case Kind::Animated: return u"animated"_q;
	case Kind::Video: return u"video"_q;
	}
	Unexpected("Kind in StickerExport::KindName.");
}

[[nodiscard]] QString SetTypeName(SetType type) {
	switch (type) {
	case SetType::Stickers: return u"stickers"_q;
	case SetType::Emoji: return u"emoji"_q;
	case SetType::Masks: return u"masks"_q;
	}
	Unexpected("SetType in StickerExport::SetTypeName.");
}

[[nodiscard]] bool ForbiddenInName(QChar ch) {
	static const auto kForbidden = u"\\/:*?\"<>|"_q;
	return (ch.unicode() < 0x20)
		|| (ch.unicode() == 0x7F)
		|| kForbidden.contains(ch);
}

// The emoji of a sticker as a part of a file name: empty if it has
// something no file system takes (the keycap "*" is such an emoji), has
// a dot or a space, is broken or too long. Nothing is replaced, a name
// has either the emoji as it is or no emoji at all.
[[nodiscard]] QString NameEmoji(const QString &emoji) {
	const auto text = emoji.trimmed();
	if (text.isEmpty() || text.size() > kNameEmojiLimit) {
		return QString();
	}
	for (auto i = 0, count = int(text.size()); i != count; ++i) {
		const auto ch = text[i];
		if (ForbiddenInName(ch) || ch.isSpace() || ch == QChar('.')) {
			return QString();
		} else if (ch.isHighSurrogate()) {
			if (i + 1 == count || !text[i + 1].isLowSurrogate()) {
				return QString();
			}
			++i;
		} else if (ch.isLowSurrogate()) {
			return QString();
		}
	}
	return text;
}

// "007_<emoji>.webp": the index keeps the order of the set and makes
// the name unique, index is zero-based.
[[nodiscard]] QString EntryName(
		int index,
		int total,
		const QString &emoji,
		const QString &extension) {
	const auto digits = std::max(
		kIndexDigits,
		int(QString::number(std::max(total, 1)).size()));
	const auto number = QString::number(index + 1).rightJustified(
		digits,
		QChar('0'));
	const auto safe = NameEmoji(emoji);
	return safe.isEmpty()
		? (number + QChar('.') + extension)
		: (number + QChar('_') + safe + QChar('.') + extension);
}

// A title as a file name without the extension, may be empty.
[[nodiscard]] QString SafeName(QString name) {
	for (auto &ch : name) {
		if (ForbiddenInName(ch)) {
			ch = QChar('_');
		}
	}
	name = name.simplified();
	if (name.size() > kNameLimit) {
		name = name.left(kNameLimit);
		if (name.back().isHighSurrogate()) {
			name.chop(1);
		}
	}

	// No hidden files and nothing Windows would cut off silently.
	while (name.startsWith(QChar('.')) || name.startsWith(QChar(' '))) {
		name.remove(0, 1);
	}
	while (name.endsWith(QChar('.')) || name.endsWith(QChar(' '))) {
		name.chop(1);
	}

	// The device names of Windows are not files, with any extension.
	static const auto kReserved = QStringList{
		u"CON"_q, u"PRN"_q, u"AUX"_q, u"NUL"_q,
		u"COM1"_q, u"COM2"_q, u"COM3"_q, u"COM4"_q, u"COM5"_q,
		u"COM6"_q, u"COM7"_q, u"COM8"_q, u"COM9"_q,
		u"LPT1"_q, u"LPT2"_q, u"LPT3"_q, u"LPT4"_q, u"LPT5"_q,
		u"LPT6"_q, u"LPT7"_q, u"LPT8"_q, u"LPT9"_q,
	};
	const auto stem = name.section(QChar('.'), 0, 0).trimmed().toUpper();
	if (kReserved.contains(stem)) {
		name.prepend(QChar('_'));
	}
	return name;
}

// The name of the archive of a set, without the extension.
[[nodiscard]] QString ArchiveBaseName(
		const QString &title,
		const QString &shortName) {
	if (auto result = SafeName(title); !result.isEmpty()) {
		return result;
	} else if (auto result = SafeName(shortName); !result.isEmpty()) {
		return result;
	}
	return u"stickers"_q;
}

// The extension the content needs, whatever was typed in the dialog.
// The path of the dialog is kept if it has the extension already,
// otherwise nothing existing may be replaced: that name wasn't confirmed.
[[nodiscard]] QString WithExtension(
		const QString &path,
		const QString &extension) {
	if (!QFileInfo(path).suffix().compare(extension, Qt::CaseInsensitive)) {
		return path;
	}
	auto result = path + QChar('.') + extension;
	for (auto i = 2; QFileInfo::exists(result); ++i) {
		result = path + u" (%1)."_q.arg(i) + extension;
	}
	return result;
}

struct ManifestSet {
	QString title;
	QString shortName;
	SetType type = SetType::Stickers;
	uint64 id = 0;
};

struct ManifestFile {
	QString file; // The name in the archive.
	QString emoji; // The main one.
	QStringList emojis; // All the emoji the sticker is suggested for.
	Kind kind = Kind::Static;
	QSize size;
	bool missing = false; // Could not be downloaded, not in the archive.
};

[[nodiscard]] QString SetLink(const ManifestSet &set) {
	return set.shortName.isEmpty()
		? QString()
		: (u"https://t.me/"_q
			+ ((set.type == SetType::Emoji)
				? u"addemoji/"_q
				: u"addstickers/"_q)
			+ set.shortName);
}

// manifest.json: the title, the short name and the link of the set and
// the list of the files in the order of the set with their emoji.
[[nodiscard]] QByteArray BuildManifest(
		const ManifestSet &set,
		const std::vector<ManifestFile> &files) {
	auto list = QJsonArray();
	auto saved = 0;
	for (const auto &file : files) {
		auto emojis = QJsonArray();
		for (const auto &emoji : file.emojis) {
			emojis.push_back(emoji);
		}
		auto object = QJsonObject();
		object.insert(u"file"_q, file.file);
		object.insert(u"emoji"_q, file.emoji);
		object.insert(u"emojis"_q, emojis);
		object.insert(u"kind"_q, KindName(file.kind));
		if (!file.size.isEmpty()) {
			object.insert(u"width"_q, file.size.width());
			object.insert(u"height"_q, file.size.height());
		}
		if (file.missing) {
			object.insert(u"missing"_q, true);
		} else {
			++saved;
		}
		list.push_back(object);
	}
	auto root = QJsonObject();
	root.insert(u"format"_q, u"oblivion-sticker-set"_q);
	root.insert(u"version"_q, kManifestVersion);
	root.insert(u"title"_q, set.title);
	root.insert(u"short_name"_q, set.shortName);
	root.insert(u"type"_q, SetTypeName(set.type));
	if (set.id) {
		root.insert(u"id"_q, QString::number(set.id));
	}
	if (const auto link = SetLink(set); !link.isEmpty()) {
		root.insert(u"link"_q, link);
	}
	root.insert(u"count"_q, int(files.size()));
	root.insert(u"saved"_q, saved);
	root.insert(u"files"_q, list);
	return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

struct ArchiveFile {
	QString name;
	QByteArray bytes;
	bool compressed = false; // Deflate, otherwise stored as it is.
};

[[nodiscard]] bool Cancelled(const Cancel &cancel) {
	return cancel && cancel->load();
}

// A .zip in memory: a set is tens of megabytes at most, while a file
// name of the user can't be given to minizip on Windows as it is.
// Empty on errors and when cancelled.
[[nodiscard]] QByteArray BuildArchive(
		const std::vector<ArchiveFile> &files,
		const QDateTime &modified,
		const Cancel &cancel = nullptr) {
	auto memory = zlib::internal::InMemoryFile();
	auto functions = memory.funcs();
	const auto handle = zipOpen2(
		nullptr,
		APPEND_STATUS_CREATE,
		nullptr,
		&functions);
	if (!handle) {
		return QByteArray();
	}
	auto info = zip_fileinfo();
	info.external_fa = uLong(kZipFileMode) << 16;
	const auto date = modified.date();
	const auto time = modified.time();
	if (date.isValid() && time.isValid() && date.year() >= 1980) {
		info.tmz_date.tm_year = date.year();
		info.tmz_date.tm_mon = date.month() - 1;
		info.tmz_date.tm_mday = date.day();
		info.tmz_date.tm_hour = time.hour();
		info.tmz_date.tm_min = time.minute();
		info.tmz_date.tm_sec = time.second();
	} else {
		info.tmz_date.tm_year = 1980;
		info.tmz_date.tm_mday = 1;
	}
	auto ok = true;
	for (const auto &file : files) {
		if (Cancelled(cancel)) {
			ok = false;
			break;
		}
		const auto name = file.name.toUtf8();
		ok = !name.isEmpty() && (zipOpenNewFileInZip4(
			handle,
			name.constData(),
			&info,
			nullptr,
			0,
			nullptr,
			0,
			nullptr,
			file.compressed ? Z_DEFLATED : 0,
			file.compressed ? Z_DEFAULT_COMPRESSION : 0,
			0, // raw
			-MAX_WBITS,
			kZipMemLevel,
			Z_DEFAULT_STRATEGY,
			nullptr, // password
			0, // crcForCrypting
			kZipMadeBy,
			kZipUtf8Flag) == ZIP_OK);
		if (!ok) {
			break;
		}
		const auto size = qint64(file.bytes.size());
		for (auto offset = qint64(0); ok && offset < size;) {
			const auto part = std::min(size - offset, qint64(kZipWriteChunk));
			ok = (zipWriteInFileInZip(
				handle,
				file.bytes.constData() + offset,
				unsigned(part)) == ZIP_OK);
			offset += part;
		}
		if (zipCloseFileInZip(handle) != ZIP_OK) {
			ok = false;
		}
		if (!ok) {
			break;
		}
	}
	if (zipClose(handle, nullptr) != ZIP_OK) {
		ok = false;
	}
	return (ok && !memory.error()) ? memory.result() : QByteArray();
}

enum class WriteResult : uchar {
	Written,
	Failed,
	Cancelled,
};

// A cancel that came while the complete file was being put in its place:
// true if the file is removed again. A file that was there before is
// replaced by then and can't be brought back, the new one is kept.
[[nodiscard]] bool DropCancelled(
		const QString &path,
		bool existed,
		const Cancel &cancel) {
	return Cancelled(cancel) && !existed && QFile::remove(path);
}

// The file appears under its name only complete. A cancel stops the
// writing between the parts and before the file is put in its place,
// nothing is changed on the disk then.
[[nodiscard]] WriteResult SaveBytes(
		const QString &path,
		const QByteArray &bytes,
		const Cancel &cancel = nullptr) {
	const auto existed = QFileInfo::exists(path);
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return WriteResult::Failed;
	}
	const auto size = qint64(bytes.size());
	for (auto offset = qint64(0); offset < size;) {
		if (Cancelled(cancel)) {
			file.cancelWriting();
			return WriteResult::Cancelled;
		}
		const auto part = std::min(size - offset, qint64(kZipWriteChunk));
		if (file.write(bytes.constData() + offset, part) != part) {
			file.cancelWriting();
			return WriteResult::Failed;
		}
		offset += part;
	}
	if (Cancelled(cancel)) {
		file.cancelWriting();
		return WriteResult::Cancelled;
	} else if (!file.commit()) {
		return WriteResult::Failed;
	}
	return DropCancelled(path, existed, cancel)
		? WriteResult::Cancelled
		: WriteResult::Written;
}

[[nodiscard]] bool IsPng(const QByteArray &bytes) {
	return bytes.startsWith(QByteArray::fromHex("89504e470d0a1a0a"));
}

[[nodiscard]] QByteArray EncodePng(const QImage &image) {
	if (image.isNull()) {
		return QByteArray();
	}
	auto result = QByteArray();
	auto buffer = QBuffer(&result);
	if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
		return QByteArray();
	}
	buffer.close();
	return result;
}

// Every frame of a 30 fps animation and every second one of a 60 fps
// animation, the timing is kept. Transparent as the sticker is.
[[nodiscard]] QByteArray LottieToGif(
		const QByteArray &tgs,
		int side,
		const Cancel &cancel) {
	auto renderer = Lottie::Renderer(tgs);
	if (!renderer.valid()) {
		return QByteArray();
	}
	const auto info = renderer.info();
	if (!info.valid()) {
		return QByteArray();
	}
	const auto size = info.size.scaled(
		side,
		side,
		Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
	const auto step = std::max(int(std::lround(info.fps / kGifMaxFps)), 1);
	const auto position = [&](int frame) {
		return crl::time(std::llround(frame * 1000. / info.fps));
	};
	auto encoder = VideoCore::GifEncoder(size);
	for (auto frame = 0; frame < info.frames; frame += step) {
		if (Cancelled(cancel)) {
			return QByteArray();
		}
		const auto image = renderer.render(frame, size);
		if (image.isNull()) {
			return QByteArray();
		}
		const auto next = std::min(frame + step, info.frames);
		encoder.add(
			image,
			std::max(position(next) - position(frame), crl::time(1)));
	}
	return encoder.finish();
}

[[nodiscard]] QByteArray VideoToGif(
		const QByteArray &webm,
		int side,
		const Cancel &cancel) {
	auto options = VideoCore::ClipOptions();
	options.content = webm;
	options.maxSide = side;
	options.maxFps = kGifMaxFps;
	const auto result = VideoCore::MakeGif(
		options,
		VideoCore::GifOptions(),
		nullptr,
		cancel);
	return result.ok ? result.content : QByteArray();
}

// Heavy for Format::Gif, any thread. Empty if the file can't be read as
// what it should be, if this kind has no such format or when cancelled.
[[nodiscard]] QByteArray ConvertSticker(
		const QByteArray &original,
		Kind kind,
		Format format,
		const Cancel &cancel = nullptr,
		int gifSide = kGifSide) {
	if (original.isEmpty()) {
		return QByteArray();
	}
	switch (format) {
	case Format::Png:
		return (kind != Kind::Static)
			? QByteArray()
			: IsPng(original)
			? original
			: EncodePng(Images::Read({ .content = original }).image);
	case Format::Webp:
		return (kind == Kind::Static) ? original : QByteArray();
	case Format::Tgs:
		return (kind == Kind::Animated) ? original : QByteArray();
	case Format::Json:
		return (kind == Kind::Animated)
			? Lottie::Unpack(original)
			: QByteArray();
	case Format::Webm:
		return (kind == Kind::Video) ? original : QByteArray();
	case Format::Gif:
		return (kind == Kind::Animated)
			? LottieToGif(original, gifSide, cancel)
			: (kind == Kind::Video)
			? VideoToGif(original, gifSide, cancel)
			: QByteArray();
	}
	Unexpected("Format in StickerExport::ConvertSticker.");
}

// Session-bound part.

[[nodiscard]] Kind KindOf(not_null<const StickerData*> sticker) {
	switch (sticker->type) {
	case StickerType::Webp: return Kind::Static;
	case StickerType::Tgs: return Kind::Animated;
	case StickerType::Webm: return Kind::Video;
	}
	Unexpected("StickerType in StickerExport::KindOf.");
}

[[nodiscard]] QString FormatName(Format format) {
	switch (format) {
	case Format::Png: return tr::lng_oblivion_sexport_format_png(tr::now);
	case Format::Webp: return tr::lng_oblivion_sexport_format_webp(tr::now);
	case Format::Tgs: return tr::lng_oblivion_sexport_format_tgs(tr::now);
	case Format::Json: return tr::lng_oblivion_sexport_format_json(tr::now);
	case Format::Gif: return tr::lng_oblivion_sexport_format_gif(tr::now);
	case Format::Webm: return tr::lng_oblivion_sexport_format_webm(tr::now);
	}
	Unexpected("Format in StickerExport::FormatName.");
}

// A format in the "Save as" submenu.
[[nodiscard]] QString FormatItemText(Format format, Format original) {
	const auto name = FormatName(format);
	return (format == original)
		? tr::lng_oblivion_sexport_format_original(tr::now, lt_format, name)
		: name;
}

[[nodiscard]] QString FileFilter(const QString &name, const QString &mask) {
	return name + u" ("_q + mask + u")"_q;
}

[[nodiscard]] QString SuggestedPath(const QString &fileName) {
	if (cDialogLastPath().isEmpty()) {
		Platform::FileDialog::InitLastPath();
	}
	return filedialogNextFilename(fileName, QString());
}

[[nodiscard]] QPointer<QWidget> DialogParent(
		const std::shared_ptr<ChatHelpers::Show> &show) {
	return (show && show->valid())
		? QPointer<QWidget>(show->toastParent()->window())
		: QPointer<QWidget>();
}

// The show of a closed window can't show anything (and asserts).
void ShowToast(
		const std::shared_ptr<ChatHelpers::Show> &show,
		not_null<Main::Session*> session,
		const QString &text) {
	if (show && show->valid()) {
		show->showToast(text);
	} else if (const auto window = ExistingWindow(session)) {
		window->showToast(text);
	}
}

// The whole file of a loaded document. A sticker is kept in memory or in
// the cache, a file on the disk is read only if somebody has saved it.
[[nodiscard]] QByteArray DocumentBytes(
		not_null<DocumentData*> document,
		const std::shared_ptr<Data::DocumentMedia> &media) {
	auto bytes = media->bytes();
	if (!bytes.isEmpty()) {
		return bytes;
	}
	const auto &location = document->location(true);
	if (location.isEmpty() || !location.accessEnable()) {
		return QByteArray();
	}
	auto file = QFile(location.name());
	if (file.size() < Storage::kMaxFileInMemory
		&& file.open(QIODevice::ReadOnly)) {
		bytes = file.readAll();
		file.close();
	}
	location.accessDisable();
	return bytes;
}

// One sticker.

enum class SaveResult : uchar {
	Saved,
	DownloadFailed,
	ConvertFailed,
	WriteFailed,
	Cancelled,
};

struct SaveRequest {
	std::shared_ptr<ChatHelpers::Show> show;
	not_null<DocumentData*> document;
	Data::FileOrigin origin;
	Kind kind = Kind::Static;
	Format format = Format::Png;
	QString path;
};

// A set.

struct ExportState {
	enum class Step : uchar {
		LoadingSet,
		ChoosingPath,
		Downloading,
		Writing,
		Done,
		Failed,
	};
	enum class Failure : uchar {
		None,
		Set, // The set was not received.
		Empty, // No stickers in it.
		Download, // Not a single file was downloaded.
		Write, // The archive was not written.
	};

	Step step = Step::LoadingSet;
	Failure failure = Failure::None;
	QString title; // Of the set, empty until it is loaded.
	bool emoji = false; // A custom emoji set.
	int total = 0;
	int ready = 0; // Downloaded or given up.
	int missing = 0; // Given up.
	QString path; // Of the archive.

	[[nodiscard]] bool finished() const {
		return (step == Step::Done) || (step == Step::Failed);
	}

	friend inline bool operator==(
		const ExportState &,
		const ExportState &) = default;
};

[[nodiscard]] QString NativePath(const QString &path) {
	return QDir::toNativeSeparators(path);
}

[[nodiscard]] QString CountText(const ExportState &state) {
	return !state.total
		? QString()
		: state.emoji
		? tr::lng_oblivion_sexport_emoji(tr::now, lt_count, state.total)
		: tr::lng_oblivion_sexport_stickers(tr::now, lt_count, state.total);
}

[[nodiscard]] QString FailureText(const ExportState &state) {
	using Failure = ExportState::Failure;
	switch (state.failure) {
	case Failure::None:
	case Failure::Set:
		return tr::lng_oblivion_sexport_failed_set(tr::now);
	case Failure::Empty:
		return tr::lng_oblivion_sexport_failed_empty(tr::now);
	case Failure::Download:
		return tr::lng_oblivion_sexport_failed_download(tr::now);
	case Failure::Write:
		return tr::lng_oblivion_sexport_failed_write(tr::now);
	}
	Unexpected("Failure in StickerExport::FailureText.");
}

[[nodiscard]] QString StatusText(const ExportState &state) {
	using Step = ExportState::Step;
	switch (state.step) {
	case Step::LoadingSet:
		return tr::lng_oblivion_sexport_loading_set(tr::now);
	case Step::ChoosingPath:
		return tr::lng_oblivion_sexport_choose_path(tr::now);
	case Step::Downloading:
		return tr::lng_oblivion_sexport_progress(
			tr::now,
			lt_ready,
			QString::number(state.ready),
			lt_total,
			QString::number(state.total));
	case Step::Writing:
		return tr::lng_oblivion_sexport_writing(tr::now);
	case Step::Done:
		return state.missing
			? tr::lng_oblivion_sexport_done_partial(
				tr::now,
				lt_ready,
				QString::number(state.total - state.missing),
				lt_total,
				QString::number(state.total))
			: tr::lng_oblivion_sexport_done(
				tr::now,
				lt_count,
				state.total);
	case Step::Failed:
		return FailureText(state);
	}
	Unexpected("Step in StickerExport::StatusText.");
}

[[nodiscard]] float64 ProgressValue(const ExportState &state) {
	using Step = ExportState::Step;
	switch (state.step) {
	case Step::LoadingSet:
	case Step::ChoosingPath:
		return 0.;
	case Step::Downloading:
		return state.total
			? std::clamp(state.ready / float64(state.total), 0., 1.)
			: 0.;
	case Step::Writing:
	case Step::Done:
	case Step::Failed:
		return 1.;
	}
	Unexpected("Step in StickerExport::ProgressValue.");
}

struct ExportBoxArgs {
	rpl::producer<ExportState> state;

	// The "Cancel" button, called before the box is closed.
	Fn<void()> cancel;
};

// Only shows the state it is given: closing the box is the cancel, who
// has shown the box watches its lifetime.
void ExportBox(not_null<Ui::GenericBox*> box, ExportBoxArgs &&args) {
	struct State {
		rpl::variable<ExportState> current;
		Fn<void()> cancel;
		bool buttonsReady = false;
		bool finished = false;
		bool done = false;
	};
	const auto state = box->lifetime().make_state<State>();
	state->current = std::move(args.state);
	state->cancel = std::move(args.cancel);

	box->setWidth(st::boxWidth);
	box->setTitle(tr::lng_oblivion_sexport_box_title());

	const auto &padding = st::boxRowPadding;
	const auto withSkips = [&](int top, int bottom) {
		return QMargins(padding.left(), top, padding.right(), bottom);
	};

	// The set: shown as soon as it is known.
	const auto header = box->addRow(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			box,
			object_ptr<Ui::VerticalLayout>(box),
			withSkips(st::boxLittleSkip, st::boxLittleSkip)),
		QMargins());
	header->entity()->add(object_ptr<Ui::FlatLabel>(
		header->entity(),
		state->current.value() | rpl::map([](const ExportState &now) {
			return Ui::Text::Bold(now.title);
		}),
		st::boxLabel));
	header->entity()->add(
		object_ptr<Ui::FlatLabel>(
			header->entity(),
			state->current.value() | rpl::map(CountText),
			st::boxDividerLabel),
		QMargins(0, style::ConvertScale(2), 0, 0));
	header->toggleOn(
		state->current.value() | rpl::map([](const ExportState &now) {
			return !now.title.isEmpty();
		}));

	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			state->current.value() | rpl::map(StatusText),
			st::boxLabel),
		withSkips(st::boxLittleSkip, 0));

	// A thin progress line under the status, in the colors of the default
	// sliders, as in the other download boxes of Oblivion. No skip of its
	// own below: the last row of the box is the skip to the buttons, a
	// second one left the line further from them than in those boxes.
	const auto barWrap = box->addRow(
		object_ptr<Ui::SlideWrap<Ui::FixedHeightWidget>>(
			box,
			object_ptr<Ui::FixedHeightWidget>(box, style::ConvertScale(4)),
			withSkips(style::ConvertScale(14), 0)),
		QMargins());
	const auto bar = barWrap->entity();
	bar->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(bar);
		auto hq = PainterHighQualityEnabler(p);
		const auto height = bar->height();
		const auto radius = height / 2.;
		p.setPen(Qt::NoPen);
		p.setBrush(st::sliderBgInactive);
		p.drawRoundedRect(bar->rect(), radius, radius);
		const auto filled = int(std::round(
			bar->width() * ProgressValue(state->current.current())));
		if (filled > 0) {
			p.setBrush(st::sliderBgActive);
			p.drawRoundedRect(
				QRect(0, 0, std::max(filled, height), height),
				radius,
				radius);
		}
	}, bar->lifetime());
	barWrap->toggleOn(
		state->current.value() | rpl::map([](const ExportState &now) {
			return !now.finished();
		}));

	// Where the archive is, or where it could not be written to.
	const auto pathWrap = box->addRow(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			box,
			object_ptr<Ui::FlatLabel>(
				box,
				state->current.value() | rpl::map([](const ExportState &now) {
					return NativePath(now.path);
				}),
				st::boxDividerLabel),
			withSkips(st::boxLittleSkip, 0)),
		QMargins());
	pathWrap->entity()->setBreakEverywhere(true);
	pathWrap->entity()->setSelectable(true);
	pathWrap->toggleOn(
		state->current.value() | rpl::map([](const ExportState &now) {
			const auto shown = (now.step == ExportState::Step::Done)
				|| (now.failure == ExportState::Failure::Write);
			return shown && !now.path.isEmpty();
		}));

	box->addRow(
		object_ptr<Ui::FixedHeightWidget>(box, st::boxLittleSkip),
		QMargins());

	const auto close = [=] {
		box->closeBox();
	};
	const auto cancel = [=] {
		if (const auto onstack = state->cancel) {
			onstack();
		}
		box->closeBox();
	};
	const auto refreshButtons = [=](const ExportState &now) {
		const auto finished = now.finished();
		const auto done = (now.step == ExportState::Step::Done);
		if (state->buttonsReady
			&& (state->finished == finished)
			&& (state->done == done)) {
			return;
		}
		state->buttonsReady = true;
		state->finished = finished;
		state->done = done;

		box->clearButtons();
		if (!finished) {
			box->addButton(tr::lng_cancel(), cancel);
		} else {
			box->addButton(tr::lng_close(), close);
			if (done) {
				box->addLeftButton(
					(Platform::IsMac()
						? tr::lng_oblivion_sexport_show_in_finder()
						: tr::lng_oblivion_sexport_show_in_folder()),
					[=] {
						const auto path = state->current.current().path;
						if (!path.isEmpty()) {
							File::ShowInFolder(path);
						}
					});
			}
		}

		// A stray click must not cancel a long export.
		box->setCloseByOutsideClick(finished);
	};
	state->current.value(
	) | rpl::on_next([=](const ExportState &now) {
		refreshButtons(now);
		bar->update();
	}, box->lifetime());
}

struct WrittenArchive {
	bool ok = false;
	bool cancelled = false;
};

// Loads the set, asks where to save it, downloads the files one by one
// and writes the archive. Main thread, lives in the Manager below.
class SetExport final : public base::has_weak_ptr {
public:
	SetExport(
		uint64 id,
		std::shared_ptr<ChatHelpers::Show> show,
		not_null<Main::Session*> session,
		StickerSetIdentifier set);
	~SetExport();

	[[nodiscard]] uint64 id() const {
		return _id;
	}
	[[nodiscard]] not_null<Main::Session*> session() const {
		return _session;
	}
	[[nodiscard]] const std::shared_ptr<ChatHelpers::Show> &show() const {
		return _show;
	}
	[[nodiscard]] rpl::producer<ExportState> state() const {
		return _state.value();
	}

	// Neither done nor failed yet.
	[[nodiscard]] bool running() const {
		return !_state.current().finished();
	}

	// The file of this document is being loaded for the archive.
	[[nodiscard]] bool waitsFor(not_null<DocumentData*> document) const {
		return (_waiting || _starting)
			&& (_index < int(_items.size()))
			&& (_items[_index].document == document);
	}

	void start();

private:
	struct Item {
		not_null<DocumentData*> document;
		std::shared_ptr<Data::DocumentMedia> media;
		QByteArray bytes;
		ManifestFile entry;
	};

	template <typename Method>
	void update(Method &&method) {
		auto now = _state.current();
		method(now);
		_state = std::move(now);
	}

	void applySet(const MTPmessages_StickerSet &result);
	void choosePath();
	void pathChosen(QString path);
	void pathCancelled();
	void loadNext();
	void checkCurrent();
	void currentReady(QByteArray bytes);
	void currentMissing();
	void advance(crl::time pause);
	void write();
	void written(WrittenArchive result);
	void fail(ExportState::Failure failure);
	void finished();

	const uint64 _id = 0;
	const std::shared_ptr<ChatHelpers::Show> _show;
	const not_null<Main::Session*> _session;
	const StickerSetIdentifier _set;
	const Cancel _cancel = std::make_shared<std::atomic<bool>>(false);

	MTP::Sender _api;
	mtpRequestId _requestId = 0;

	rpl::variable<ExportState> _state;
	ManifestSet _info;
	Data::FileOrigin _origin;
	std::vector<Item> _items;
	int _index = 0;
	bool _waiting = false; // For the file number _index.
	bool _starting = false; // Inside the call that starts its load.
	crl::time _waitingSince = 0;
	base::Timer _nextTimer;

	rpl::lifetime _lifetime;

};

// Downloads (if needed), converts and writes one sticker.
class SaveTask final : public base::has_weak_ptr {
public:
	SaveTask(uint64 id, SaveRequest &&request);
	~SaveTask();

	[[nodiscard]] not_null<Main::Session*> session() const {
		return _session;
	}

	// The file of this document is being loaded to be saved.
	[[nodiscard]] bool waitsFor(not_null<DocumentData*> document) const {
		return _media
			&& !_working
			&& !_finished
			&& (_request.document == document);
	}

	void start();

private:
	void check();
	void convert();
	void finish(SaveResult result);

	const uint64 _id = 0;
	const SaveRequest _request;
	const not_null<Main::Session*> _session;
	const Cancel _cancel = std::make_shared<std::atomic<bool>>(false);

	std::shared_ptr<Data::DocumentMedia> _media;
	bool _working = false; // Loaded, the file is being written.
	bool _finished = false;

	rpl::lifetime _lifetime;

};

// Everything that is going on, main thread only. A task lives until its
// file is written (or failed), until it is cancelled or until its session
// is gone, whichever comes first.
class Manager final {
public:
	[[nodiscard]] static Manager &Instance();

	void save(SaveRequest &&request);
	void saveFinished(uint64 id, not_null<Main::Session*> session);

	void startExport(
		std::shared_ptr<ChatHelpers::Show> show,
		StickerSetIdentifier set);
	void cancelExport(uint64 id);
	void exportPathCancelled(uint64 id);
	void exportFinished(uint64 id, not_null<Main::Session*> session);
	[[nodiscard]] bool exportBoxShown(uint64 id) const;

	[[nodiscard]] bool waitsFor(not_null<DocumentData*> document) const;

private:
	void track(not_null<Main::Session*> session);
	void exportBoxClosing(uint64 id);

	base::flat_map<uint64, std::unique_ptr<SaveTask>> _saves;
	std::unique_ptr<SetExport> _export;
	QPointer<Ui::BoxContent> _exportBox;
	base::flat_set<not_null<Main::Session*>> _tracked;
	uint64 _lastId = 0;

};

SetExport::SetExport(
	uint64 id,
	std::shared_ptr<ChatHelpers::Show> show,
	not_null<Main::Session*> session,
	StickerSetIdentifier set)
: _id(id)
, _show(std::move(show))
, _session(session)
, _set(std::move(set))
, _api(&session->mtp())
, _nextTimer([=] { loadNext(); }) {
	// One subscription for all the files: only the file that is waited
	// for is checked, the others load for somebody else.
	_session->data().documentLoadProgress(
	) | rpl::on_next([=](not_null<DocumentData*> document) {
		if (_waiting
			&& (_index < int(_items.size()))
			&& (_items[_index].document == document)) {
			checkCurrent();
		}
	}, _lifetime);
}

SetExport::~SetExport() {
	// Stops the archive that may be written right now. The download of
	// the current file is left alone: the same loader may show this
	// sticker somewhere.
	_cancel->store(true);
}

void SetExport::start() {
	_requestId = _api.request(MTPmessages_GetStickerSet(
		Data::InputStickerSet(_set),
		MTP_int(0) // hash
	)).done([=](const MTPmessages_StickerSet &result) {
		_requestId = 0;
		applySet(result);
	}).fail([=] {
		_requestId = 0;
		fail(ExportState::Failure::Set);
	}).send();
}

void SetExport::applySet(const MTPmessages_StickerSet &result) {
	if (result.type() != mtpc_messages_stickerSet) {
		fail(ExportState::Failure::Set);
		return;
	}
	const auto &data = result.c_messages_stickerSet();
	const auto &set = data.vset().data();
	const auto owner = &_session->data();
	_info.title = owner->stickers().getSetTitle(set);
	_info.shortName = qs(set.vshort_name());
	_info.type = set.is_emojis()
		? SetType::Emoji
		: set.is_masks()
		? SetType::Masks
		: SetType::Stickers;
	_info.id = set.vid().v;
	_origin = Data::FileOriginStickerSet(
		set.vid().v,
		set.vaccess_hash().v);

	// All the emoji a sticker is suggested for, in the order of the set.
	auto emojis = base::flat_map<DocumentId, QStringList>();
	for (const auto &pack : data.vpacks().v) {
		const auto &fields = pack.data();
		const auto emoticon = qs(fields.vemoticon());
		if (emoticon.isEmpty()) {
			continue;
		}
		for (const auto &id : fields.vdocuments().v) {
			auto &list = emojis[DocumentId(id.v)];
			if (!list.contains(emoticon)) {
				list.push_back(emoticon);
			}
		}
	}

	auto documents = std::vector<not_null<DocumentData*>>();
	for (const auto &item : data.vdocuments().v) {
		const auto document = owner->processDocument(item);
		if (document->sticker()) {
			documents.push_back(document);
		}
	}
	const auto total = int(documents.size());
	_items.reserve(total);
	for (const auto &document : documents) {
		const auto sticker = document->sticker();
		const auto kind = KindOf(sticker);
		auto list = QStringList();
		if (const auto i = emojis.find(document->id); i != end(emojis)) {
			list = i->second;
		}
		const auto emoji = !sticker->alt.isEmpty()
			? sticker->alt
			: !list.isEmpty()
			? list.front()
			: QString();
		if (list.isEmpty() && !emoji.isEmpty()) {
			list.push_back(emoji);
		}
		const auto extension = Extension(
			OriginalFormat(kind, document->mimeString()));
		_items.push_back({
			.document = document,
			.entry = {
				.file = EntryName(
					int(_items.size()),
					total,
					emoji,
					extension),
				.emoji = emoji,
				.emojis = list,
				.kind = kind,
				.size = document->dimensions,
			},
		});
	}
	if (_items.empty()) {
		update([&](ExportState &now) {
			now.title = _info.title;
			now.emoji = (_info.type == SetType::Emoji);
		});
		fail(ExportState::Failure::Empty);
		return;
	}
	update([&](ExportState &now) {
		now.step = ExportState::Step::ChoosingPath;
		now.title = _info.title;
		now.emoji = (_info.type == SetType::Emoji);
		now.total = total;
	});
	choosePath();
}

void SetExport::choosePath() {
	const auto weak = base::make_weak(this);
	const auto name = ArchiveBaseName(_info.title, _info.shortName);
	FileDialog::GetWritePath(
		DialogParent(_show),
		tr::lng_oblivion_sexport_dialog_set(tr::now),
		FileFilter(tr::lng_oblivion_sexport_format_zip(tr::now), u"*.zip"_q),
		SuggestedPath(name + u".zip"_q),
		[=](QString &&result) {
			if (const auto strong = weak.get()) {
				if (result.isEmpty()) {
					strong->pathCancelled();
				} else {
					strong->pathChosen(std::move(result));
				}
			}
		},
		[=] {
			if (const auto strong = weak.get()) {
				strong->pathCancelled();
			}
		});
}

void SetExport::pathChosen(QString path) {
	if (_state.current().step != ExportState::Step::ChoosingPath) {
		return;
	}
	update([&](ExportState &now) {
		now.step = ExportState::Step::Downloading;
		now.path = WithExtension(path, u"zip"_q);
	});
	_nextTimer.callOnce(0);
}

void SetExport::pathCancelled() {
	// Closes the box, that destroys this object: nothing after it.
	Manager::Instance().exportPathCancelled(_id);
}

void SetExport::loadNext() {
	if (_state.current().step != ExportState::Step::Downloading
		|| _waiting) {
		return;
	}

	// The view of the previous file is released here and not where the
	// file was taken: that could be right inside a call of its loader.
	if (_index > 0 && _index <= int(_items.size())) {
		_items[_index - 1].media = nullptr;
	}
	if (_index >= int(_items.size())) {
		write();
		return;
	}
	auto &item = _items[_index];
	const auto document = item.document;
	if (document->size <= 0
		|| document->size >= Storage::kMaxFileInMemory) {
		// The loader keeps in memory only the files below that size.
		currentMissing();
		return;
	}
	item.media = document->createMediaView();
	if (item.media->loaded(true)) {
		auto bytes = DocumentBytes(document, item.media);
		if (bytes.isEmpty()) {
			currentMissing();
		} else {
			currentReady(std::move(bytes));
		}
		return;
	}
	// Waiting starts only after the call: taking over a loader that
	// somebody has started may cancel it first, that is not a failure.
	// The load could also finish or fail right away.
	_waitingSince = crl::now();
	_starting = true;
	document->save(_origin, QString());
	_starting = false;
	_waiting = true;
	checkCurrent();
}

void SetExport::checkCurrent() {
	if (!_waiting || _index >= int(_items.size())) {
		return;
	}
	auto &item = _items[_index];
	if (item.document->loading()) {
		return;
	} else if (!item.media->loaded(true)) {
		currentMissing();
		return;
	}
	auto bytes = DocumentBytes(item.document, item.media);
	if (bytes.isEmpty()) {
		currentMissing();
	} else {
		currentReady(std::move(bytes));
	}
}

void SetExport::currentReady(QByteArray bytes) {
	auto &item = _items[_index];
	item.bytes = std::move(bytes);
	const auto waited = _waiting ? (crl::now() - _waitingSince) : 0;
	update([&](ExportState &now) {
		++now.ready;
	});

	// What came from the cache needs no pause.
	advance((waited > kCachedThreshold) ? kNetworkPause : 0);
}

void SetExport::currentMissing() {
	auto &item = _items[_index];
	item.entry.missing = true;
	const auto waited = _waiting;
	update([&](ExportState &now) {
		++now.ready;
		++now.missing;
	});
	advance(waited ? kNetworkPause : 0);
}

void SetExport::advance(crl::time pause) {
	_waiting = false;
	++_index;

	// Always through the timer: this may be called from the loader.
	_nextTimer.callOnce(pause);
}

void SetExport::write() {
	auto files = std::vector<ArchiveFile>();
	auto manifest = std::vector<ManifestFile>();
	files.reserve(_items.size() + 1);
	manifest.reserve(_items.size());
	for (auto &item : _items) {
		manifest.push_back(item.entry);
		if (!item.entry.missing) {
			files.push_back({
				.name = item.entry.file,
				.bytes = base::take(item.bytes),
			});
		}
	}
	if (files.empty()) {
		fail(ExportState::Failure::Download);
		return;
	}
	update([&](ExportState &now) {
		now.step = ExportState::Step::Writing;
	});
	const auto weak = base::make_weak(this);
	const auto cancel = _cancel;
	const auto info = _info;
	const auto path = _state.current().path;
	crl::async([
		=,
		files = std::move(files),
		manifest = std::move(manifest)
	]() mutable {
		files.insert(begin(files), ArchiveFile{
			.name = ManifestName(),
			.bytes = BuildManifest(info, manifest),
			.compressed = true,
		});
		const auto archive = BuildArchive(
			files,
			QDateTime::currentDateTime(),
			cancel);
		auto result = WrittenArchive();
		if (Cancelled(cancel)) {
			result.cancelled = true;
		} else if (!archive.isEmpty()) {
			const auto saved = SaveBytes(path, archive, cancel);
			result.ok = (saved == WriteResult::Written);
			result.cancelled = (saved == WriteResult::Cancelled);
		}
		crl::on_main(weak, [=] {
			if (const auto strong = weak.get()) {
				strong->written(result);
			}
		});
	});
}

void SetExport::written(WrittenArchive result) {
	if (result.cancelled) {
		return;
	} else if (!result.ok) {
		fail(ExportState::Failure::Write);
		return;
	}
	update([&](ExportState &now) {
		now.step = ExportState::Step::Done;
	});

	// The box says the same and shows the path, a toast is for the case
	// there is no box to say it.
	if (!Manager::Instance().exportBoxShown(_id)) {
		ShowToast(
			_show,
			_session,
			tr::lng_oblivion_sexport_saved_to(
				tr::now,
				lt_path,
				NativePath(_state.current().path)));
	}
	finished();
}

void SetExport::fail(ExportState::Failure failure) {
	if (_state.current().finished()) {
		return;
	}
	update([&](ExportState &now) {
		now.step = ExportState::Step::Failed;
		now.failure = failure;
	});
	finished();
}

void SetExport::finished() {
	_waiting = false;
	_nextTimer.cancel();
	for (auto &item : _items) {
		item.media = nullptr;
		item.bytes = QByteArray();
	}

	// Lets the next export start, this object is destroyed a bit later.
	Manager::Instance().exportFinished(_id, _session);
}

SaveTask::SaveTask(uint64 id, SaveRequest &&request)
: _id(id)
, _request(std::move(request))
, _session(&_request.document->session()) {
}

SaveTask::~SaveTask() {
	_cancel->store(true);
}

void SaveTask::start() {
	const auto document = _request.document;
	if (document->size >= Storage::kMaxFileInMemory) {
		// The loader keeps in memory only the files below that size.
		finish(SaveResult::DownloadFailed);
		return;
	}
	_media = document->createMediaView();
	if (_media->loaded(true)) {
		convert();
		return;
	}
	document->save(_request.origin, QString());
	_session->data().documentLoadProgress(
	) | rpl::filter([=](not_null<DocumentData*> updated) {
		return (updated == document);
	}) | rpl::on_next([=] {
		check();
	}, _lifetime);
	check();
	if (!_working && !_finished) {
		ShowToast(
			_request.show,
			_session,
			tr::lng_oblivion_sexport_downloading(tr::now));
	}
}

void SaveTask::check() {
	if (_working || _finished || _request.document->loading()) {
		return;
	} else if (_media->loaded(true)) {
		convert();
	} else {
		finish(SaveResult::DownloadFailed);
	}
}

void SaveTask::convert() {
	_working = true;
	auto bytes = DocumentBytes(_request.document, _media);
	if (bytes.isEmpty()) {
		finish(SaveResult::DownloadFailed);
		return;
	}
	const auto kind = _request.kind;
	const auto format = _request.format;
	const auto path = _request.path;
	const auto cancel = _cancel;
	const auto weak = base::make_weak(this);
	if (format == Format::Gif) {
		// Rendering takes a few seconds.
		ShowToast(
			_request.show,
			_session,
			tr::lng_oblivion_sexport_converting(tr::now));
	}
	crl::async([=, bytes = std::move(bytes)] {
		const auto converted = ConvertSticker(bytes, kind, format, cancel);
		const auto result = Cancelled(cancel)
			? SaveResult::Cancelled
			: converted.isEmpty()
			? SaveResult::ConvertFailed
			: (SaveBytes(path, converted) == WriteResult::Written)
			? SaveResult::Saved
			: SaveResult::WriteFailed;
		crl::on_main(weak, [=] {
			if (const auto strong = weak.get()) {
				strong->finish(result);
			}
		});
	});
}

void SaveTask::finish(SaveResult result) {
	if (_finished) {
		return;
	}
	_finished = true;
	if (result != SaveResult::Cancelled) {
		const auto path = NativePath(_request.path);
		ShowToast(_request.show, _session, [&] {
			switch (result) {
			case SaveResult::Saved:
				return tr::lng_oblivion_sexport_saved_to(
					tr::now,
					lt_path,
					path);
			case SaveResult::DownloadFailed:
				return tr::lng_oblivion_sexport_download_failed(tr::now);
			case SaveResult::ConvertFailed:
				return tr::lng_oblivion_sexport_convert_failed(tr::now);
			case SaveResult::WriteFailed:
				return tr::lng_oblivion_sexport_write_failed(
					tr::now,
					lt_path,
					path);
			case SaveResult::Cancelled:
				break;
			}
			Unexpected("Result in StickerExport::SaveTask::finish.");
		}());
	}

	// This may be called from a subscription owned by the task, it is
	// destroyed a bit later: nothing after this call.
	Manager::Instance().saveFinished(_id, _session);
}

Manager &Manager::Instance() {
	static auto result = Manager();
	return result;
}

void Manager::track(not_null<Main::Session*> session) {
	if (!_tracked.emplace(session).second) {
		return;
	}
	session->lifetime().add([=] {
		for (auto i = begin(_saves); i != end(_saves);) {
			if (i->second->session() == session) {
				i = _saves.erase(i);
			} else {
				++i;
			}
		}
		if (_export && (_export->session() == session)) {
			// The box (if it is still there) keeps the last state and
			// may only be closed.
			_export = nullptr;
		}
		_tracked.remove(session);
	});
}

void Manager::save(SaveRequest &&request) {
	const auto id = ++_lastId;
	const auto task = _saves.emplace(
		id,
		std::make_unique<SaveTask>(id, std::move(request))
	).first->second.get();
	track(task->session());
	task->start();
}

void Manager::saveFinished(uint64 id, not_null<Main::Session*> session) {
	crl::on_main(session.get(), [=] {
		_saves.remove(id);
	});
}

void Manager::startExport(
		std::shared_ptr<ChatHelpers::Show> show,
		StickerSetIdentifier set) {
	if (!show || !show->valid() || set.empty()) {
		return;
	} else if (_export) {
		show->showToast(tr::lng_oblivion_sexport_busy(tr::now));
		return;
	}
	const auto session = &show->session();
	const auto id = ++_lastId;
	track(session);
	_export = std::make_unique<SetExport>(id, show, session, std::move(set));

	// Closing the box is the cancel: right when it starts to hide, and
	// when it is destroyed without that (a closed window). The "Cancel"
	// button cancels by itself before it closes the box, so a box that
	// starts to hide with the export still going was closed by something
	// else: Escape, another box shown in its place, a locked window.
	// After the export is finished or its session is gone this does
	// nothing.
	auto box = Box(ExportBox, ExportBoxArgs{
		.state = _export->state(),
		.cancel = [=] { Instance().cancelExport(id); },
	});
	box->boxClosing() | rpl::on_next([=] {
		Instance().exportBoxClosing(id);
	}, box->lifetime());
	box->lifetime().add([=] {
		Instance().cancelExport(id);
	});
	_exportBox = box.data();
	show->showBox(std::move(box));

	// The box could be refused and destroyed at once.
	if (_export && (_export->id() == id)) {
		_export->start();
	}
}

void Manager::cancelExport(uint64 id) {
	if (_export && (_export->id() == id)) {
		_export = nullptr;
	}
}

void Manager::exportBoxClosing(uint64 id) {
	if (!_export || (_export->id() != id)) {
		return;
	}
	const auto interrupted = _export->running();
	const auto show = _export->show();
	const auto session = _export->session();
	_export = nullptr;
	if (!interrupted) {
		return;
	}

	// Nobody has asked for that, so the export doesn't just vanish. Not
	// right from here: the box may be closing together with its window.
	crl::on_main(session.get(), [=] {
		if (show && show->valid()) {
			show->showToast(tr::lng_oblivion_sexport_interrupted(tr::now));
		}
	});
}

void Manager::exportPathCancelled(uint64 id) {
	if (!_export || (_export->id() != id)) {
		return;
	}

	// The export first: its box is closed by the user's own choice.
	const auto box = _exportBox;
	cancelExport(id);
	if (box) {
		box->closeBox();
	}
}

void Manager::exportFinished(uint64 id, not_null<Main::Session*> session) {
	// The export calls this from its own methods.
	crl::on_main(session.get(), [=] {
		cancelExport(id);
	});
}

bool Manager::exportBoxShown(uint64 id) const {
	return _export && (_export->id() == id) && !_exportBox.isNull();
}

bool Manager::waitsFor(not_null<DocumentData*> document) const {
	if (_export && _export->waitsFor(document)) {
		return true;
	}
	for (const auto &[id, task] : _saves) {
		if (task->waitsFor(document)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] QString SaveFileName(
		not_null<const StickerData*> sticker,
		const QString &extension) {
	const auto name = (sticker->setType == Data::StickersType::Emoji)
		? tr::lng_oblivion_sexport_name_emoji(tr::now)
		: tr::lng_oblivion_sexport_name_sticker(tr::now);
	const auto emoji = NameEmoji(sticker->alt);
	auto result = SafeName(emoji.isEmpty()
		? name
		: (name + QChar(' ') + emoji));
	if (result.isEmpty()) {
		result = u"sticker"_q;
	}
	return result + QChar('.') + extension;
}

void ChoosePathAndSave(
		std::shared_ptr<ChatHelpers::Show> show,
		base::weak_ptr<Main::Session> weak,
		not_null<DocumentData*> document,
		Data::FileOrigin origin,
		Format format) {
	// The document lives as long as its session does.
	if (!weak.get() || !show || !show->valid()) {
		return;
	}
	const auto sticker = document->sticker();
	if (!sticker) {
		return;
	}
	const auto kind = KindOf(sticker);
	const auto extension = Extension(format);
	const auto emojiSet = (sticker->setType == Data::StickersType::Emoji);
	FileDialog::GetWritePath(
		DialogParent(show),
		(emojiSet
			? tr::lng_oblivion_sexport_dialog_emoji(tr::now)
			: tr::lng_oblivion_sexport_dialog_sticker(tr::now)),
		FileFilter(FormatName(format), u"*."_q + extension),
		SuggestedPath(SaveFileName(sticker, extension)),
		[=](QString &&result) {
			if (result.isEmpty() || !weak.get()) {
				return;
			}
			Manager::Instance().save({
				.show = show,
				.document = document,
				.origin = origin,
				.kind = kind,
				.format = format,
				.path = WithExtension(result, extension),
			});
		});
}

// The usual menu icons are painted in the colors of the usual menu, in
// the dark menus of the stories and calls they are barely seen. A panel
// keeps its icons inside its style by value, so the address of the icons
// set tells nothing, the colors of the menu itself do.
[[nodiscard]] bool UsualMenu(not_null<Ui::PopupMenu*> menu) {
	const auto &menuSt = menu->st().menu;
	return (menuSt.itemBg == st::defaultMenu.itemBg)
		&& (menuSt.itemFg == st::defaultMenu.itemFg);
}

// UI snapshot scenes (OBLIVION_SELFTEST=ui, see oblivion_ui_snapshots.h).

// The archive is named after the set the way the real export names it.
[[nodiscard]] QString SampleArchivePath(
		const QString &title,
		const QString &folder = u"Downloads"_q) {
	return (Platform::IsWindows()
		? u"C:/Users/Oblivion/"_q
		: u"/Users/oblivion/"_q)
		+ folder
		+ QChar('/')
		+ ArchiveBaseName(title, QString())
		+ u".zip"_q;
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;
	using Step = ExportState::Step;
	using Failure = ExportState::Failure;

	const auto scene = [](QString name, ExportState state) {
		RegisterBoxScene(
			std::move(name),
			QSize(st::boxWidth + style::ConvertScale(120), 0),
			[=](std::shared_ptr<Ui::Show> show) {
				return Box(ExportBox, ExportBoxArgs{
					.state = rpl::single(state),
				});
			});
	};
	const auto title = u"Persik the Cat"_q;
	const auto emojiTitle = u"Пиксельные сердечки"_q;

	// A title of two lines with an emoji, and a path long enough to take
	// several lines and to be broken in the middle of a name.
	const auto longTitle = u"Котик Персик и его друзья — зимние каникулы "_q
		+ QString::fromUtf8("\xF0\x9F\x90\xB1"); // U+1F431
	const auto longFolder = u"Documents/Telegram/StickerArchives"_q;

	// The set is requested, nothing is known about it yet.
	scene(u"sticker_export_loading"_q, {});

	// The set is known, the save dialog is open over the box.
	scene(u"sticker_export_choosing"_q, {
		.step = Step::ChoosingPath,
		.title = title,
		.total = 120,
	});

	// The files are being downloaded.
	scene(u"sticker_export_progress"_q, {
		.step = Step::Downloading,
		.title = title,
		.total = 120,
		.ready = 47,
	});

	// The archive is being written.
	scene(u"sticker_export_writing"_q, {
		.step = Step::Writing,
		.title = emojiTitle,
		.emoji = true,
		.total = 200,
		.ready = 200,
	});

	// Done: everything is in the archive.
	scene(u"sticker_export_done"_q, {
		.step = Step::Done,
		.title = title,
		.total = 120,
		.ready = 120,
		.path = SampleArchivePath(title),
	});

	// Done, but some files could not be downloaded.
	scene(u"sticker_export_done_partial"_q, {
		.step = Step::Done,
		.title = emojiTitle,
		.emoji = true,
		.total = 43,
		.ready = 43,
		.missing = 2,
		.path = SampleArchivePath(emojiTitle),
	});

	// Done, a long title and a long path.
	scene(u"sticker_export_done_long"_q, {
		.step = Step::Done,
		.title = longTitle,
		.total = 88,
		.ready = 88,
		.path = SampleArchivePath(longTitle, longFolder),
	});

	// The set could not be loaded.
	scene(u"sticker_export_failed"_q, {
		.step = Step::Failed,
		.failure = Failure::Set,
	});

	// The set is loaded, but there is nothing in it: a title without
	// the count.
	scene(u"sticker_export_failed_empty"_q, {
		.step = Step::Failed,
		.failure = Failure::Empty,
		.title = emojiTitle,
		.emoji = true,
	});

	// Not a single file was downloaded: the path is chosen already, but
	// nothing was written there, so it is not shown.
	scene(u"sticker_export_failed_download"_q, {
		.step = Step::Failed,
		.failure = Failure::Download,
		.title = title,
		.total = 120,
		.ready = 120,
		.missing = 120,
		.path = SampleArchivePath(title),
	});

	// The archive could not be written.
	scene(u"sticker_export_failed_write"_q, {
		.step = Step::Failed,
		.failure = Failure::Write,
		.title = title,
		.total = 120,
		.ready = 120,
		.path = SampleArchivePath(title),
	});

	// The menu items in the middle of a scene, inside the panel that
	// Ui::PopupMenu paints around them: a popup window itself can't be a
	// part of the scene.
	const auto menuScene = [](
			QString name,
			Fn<void(not_null<Ui::Menu::Menu*>)> fill) {
		const auto skip = style::ConvertScale(kSceneMenuSkip);
		const auto outer = st::popupMenuWithIcons.menu.widthMax + 2 * skip;
		RegisterScene({
			.name = std::move(name),
			.size = QSize(outer, 0),
			.create = [=](not_null<Ui::RpWidget*> parent) {
				const auto padding = st::popupMenuWithIcons.scrollPadding;
				const auto result = Ui::CreateChild<Ui::RpWidget>(
					parent.get());
				const auto menu = Ui::CreateChild<Ui::Menu::Menu>(
					result,
					st::popupMenuWithIcons.menu);
				fill(menu);
				const auto shadow
					= result->lifetime().make_state<Ui::BoxShadow>(
						st::popupMenuWithIcons.shadow);
				result->paintRequest(
				) | rpl::on_next([=] {
					const auto radius = st::popupMenuWithIcons.radius;
					const auto panel = menu->geometry().marginsAdded(
						padding);
					auto p = QPainter(result);
					auto hq = PainterHighQualityEnabler(p);
					p.setPen(Qt::NoPen);
					p.setBrush(st::popupMenuWithIcons.menu.itemBg);
					p.drawRoundedRect(panel, radius, radius);
					shadow->paint(p, panel, radius);
				}, result->lifetime());
				menu->sizeValue(
				) | rpl::on_next([=](QSize menuSize) {
					menu->move(
						(outer - menuSize.width()) / 2,
						skip + padding.top());
					result->setGeometry(
						0,
						0,
						outer,
						skip
							+ padding.top()
							+ menuSize.height()
							+ padding.bottom()
							+ skip);
					result->update();
				}, result->lifetime());
				return result;
			},
			.fit = false,
		});
	};

	// What this module adds to the context menu of a sticker.
	menuScene(u"sticker_export_menu"_q, [](not_null<Ui::Menu::Menu*> menu) {
		menu->addAction(
			tr::lng_oblivion_sexport_save_as(tr::now),
			std::make_unique<QMenu>(),
			&st::menuIconDownload);
		menu->addAction(
			tr::lng_oblivion_sexport_save_set(tr::now),
			[] {},
			&st::menuIconExport);
	});

	// The "Save as" submenu of every kind of a sticker.
	const auto formatsScene = [=](QString name, Kind kind, Format original) {
		menuScene(std::move(name), [=](not_null<Ui::Menu::Menu*> menu) {
			for (const auto format : FormatsFor(kind, original)) {
				menu->addAction(FormatItemText(format, original), [] {});
			}
		});
	};
	formatsScene(u"sticker_export_menu_static"_q, Kind::Static, Format::Webp);
	formatsScene(
		u"sticker_export_menu_animated"_q,
		Kind::Animated,
		Format::Tgs);
	formatsScene(u"sticker_export_menu_video"_q, Kind::Video, Format::Webm);
});

// Self-test helpers.

[[nodiscard]] QString Utf8(const char *text) {
	return QString::fromUtf8(text);
}

[[nodiscard]] QByteArray PatternBytes(int size, uint32 seed) {
	auto result = QByteArray(size, Qt::Uninitialized);
	auto value = seed;
	for (auto i = 0; i != size; ++i) {
		value = value * 1664525u + 1013904223u;
		result[i] = char(value >> 24);
	}
	return result;
}

[[nodiscard]] QImage PatternImage(int side, int shift) {
	auto result = QImage(side, side, QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);
	auto p = QPainter(&result);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(255, 140, 0));
	p.drawEllipse(
		QRectF(side / 8. + shift, side / 8., side / 2., side / 2.));
	p.setBrush(QColor(40, 120, 255));
	p.drawRect(QRectF(side / 2., side / 2. + shift, side / 3., side / 3.));
	p.end();
	return result;
}

[[nodiscard]] QByteArray ResourceSticker() {
	for (const auto name : { "cake", "palette", "ttl", "discussion" }) {
		auto file = QFile(
			u":/animations/"_q + QString::fromLatin1(name) + u".tgs"_q);
		if (file.open(QIODevice::ReadOnly)) {
			auto bytes = file.readAll();
			if (Lottie::ReadInfo(Lottie::Unpack(bytes)).valid()) {
				return bytes;
			}
		}
	}
	return QByteArray();
}

} // namespace

bool QuietLoad(not_null<DocumentData*> document) {
	return Manager::Instance().waitsFor(document);
}

bool RunSelfTest(QStringList &log) {
	auto timer = QElapsedTimer();
	timer.start();
	auto passed = true;
	const auto check = [&](bool ok, const QString &what) {
		if (!ok) {
			passed = false;
			log.push_back(u"FAIL sticker_export: "_q + what);
		}
		return ok;
	};

	const auto grin = Utf8("\xF0\x9F\x98\x80"); // U+1F600
	const auto heart = Utf8("\xE2\x9D\xA4\xEF\xB8\x8F"); // U+2764 U+FE0F
	const auto family = Utf8( // Man, ZWJ, woman, ZWJ, girl.
		"\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
		"\xF0\x9F\x91\xA7");
	const auto keycap = Utf8("*\xEF\xB8\x8F\xE2\x83\xA3"); // "*" keycap.
	const auto cyrillic = Utf8(
		"\xD0\x9A\xD0\xBE\xD1\x82 \xD0\x9F\xD0\xB5\xD1\x80\xD1\x81"
		"\xD0\xB8\xD0\xBA"); // "Kot Persik" in Cyrillic.

	// Formats.
	{
		using Formats = std::vector<Format>;
		check(
			OriginalFormat(Kind::Static, u"image/webp"_q) == Format::Webp
				&& OriginalFormat(Kind::Static, QString()) == Format::Webp
				&& OriginalFormat(Kind::Static, u"Image/PNG"_q)
					== Format::Png
				&& OriginalFormat(Kind::Animated, u"image/webp"_q)
					== Format::Tgs
				&& OriginalFormat(Kind::Video, QString()) == Format::Webm,
			u"OriginalFormat"_q);
		check(
			FormatsFor(Kind::Static, Format::Webp)
				== Formats{ Format::Png, Format::Webp },
			u"FormatsFor a static sticker"_q);
		check(
			FormatsFor(Kind::Static, Format::Png) == Formats{ Format::Png },
			u"FormatsFor a PNG sticker"_q);
		check(
			FormatsFor(Kind::Animated, Format::Tgs)
				== Formats{ Format::Tgs, Format::Json, Format::Gif },
			u"FormatsFor an animated sticker"_q);
		check(
			FormatsFor(Kind::Video, Format::Webm)
				== Formats{ Format::Webm, Format::Gif },
			u"FormatsFor a video sticker"_q);
		check(
			Extension(Format::Png) == u"png"_q
				&& Extension(Format::Webp) == u"webp"_q
				&& Extension(Format::Tgs) == u"tgs"_q
				&& Extension(Format::Json) == u"json"_q
				&& Extension(Format::Gif) == u"gif"_q
				&& Extension(Format::Webm) == u"webm"_q,
			u"Extension"_q);
	}

	// File names.
	{
		check(NameEmoji(grin) == grin, u"NameEmoji keeps an emoji"_q);
		check(
			NameEmoji(u"  "_q + heart + u" "_q) == heart,
			u"NameEmoji trims"_q);
		check(NameEmoji(family) == family, u"NameEmoji keeps ZWJ"_q);
		check(NameEmoji(keycap).isEmpty(), u"NameEmoji drops '*'"_q);
		check(
			NameEmoji(u"a/b"_q).isEmpty()
				&& NameEmoji(u"a\\b"_q).isEmpty()
				&& NameEmoji(u"a:b"_q).isEmpty()
				&& NameEmoji(u"a.b"_q).isEmpty()
				&& NameEmoji(u"a b"_q).isEmpty()
				&& NameEmoji(u"a\nb"_q).isEmpty()
				&& NameEmoji(QString()).isEmpty(),
			u"NameEmoji drops what a file name can't have"_q);
		check(
			NameEmoji(grin.left(1)).isEmpty()
				&& NameEmoji(grin.mid(1)).isEmpty(),
			u"NameEmoji drops a broken surrogate pair"_q);
		check(
			NameEmoji(QString(kNameEmojiLimit + 1, QChar('a'))).isEmpty()
				&& !NameEmoji(QString(kNameEmojiLimit, QChar('a'))).isEmpty(),
			u"NameEmoji length limit"_q);

		check(
			EntryName(0, 120, grin, u"webp"_q)
				== u"001_"_q + grin + u".webp"_q,
			u"EntryName with an emoji: "_q
				+ EntryName(0, 120, grin, u"webp"_q));
		check(
			EntryName(6, 120, keycap, u"tgs"_q) == u"007.tgs"_q,
			u"EntryName without a usable emoji"_q);
		check(
			EntryName(119, 120, QString(), u"webm"_q) == u"120.webm"_q,
			u"EntryName without an emoji"_q);
		check(
			EntryName(0, 1200, QString(), u"webp"_q) == u"0001.webp"_q
				&& EntryName(1199, 1200, QString(), u"webp"_q)
					== u"1200.webp"_q,
			u"EntryName of a huge set"_q);
		auto names = base::flat_set<QString>();
		for (auto i = 0; i != 200; ++i) {
			names.emplace(EntryName(i, 200, grin, u"webp"_q));
		}
		check(names.size() == 200, u"EntryName is unique"_q);
		auto sorted = std::vector<QString>(begin(names), end(names));
		check(
			sorted.front().startsWith(u"001_"_q)
				&& sorted.back().startsWith(u"200_"_q),
			u"EntryName sorts in the order of the set"_q);

		check(
			ArchiveBaseName(cyrillic, u"persik"_q) == cyrillic,
			u"ArchiveBaseName keeps a title"_q);
		check(
			ArchiveBaseName(u"  Cats: the \"best\" / set?  "_q, QString())
				== u"Cats_ the _best_ _ set_"_q,
			u"ArchiveBaseName replaces: "_q
				+ ArchiveBaseName(
					u"  Cats: the \"best\" / set?  "_q,
					QString()));
		check(
			ArchiveBaseName(QString(), u"persik_cat"_q) == u"persik_cat"_q,
			u"ArchiveBaseName falls back to the short name"_q);
		check(
			ArchiveBaseName(u" .. "_q, QString()) == u"stickers"_q,
			u"ArchiveBaseName of nothing"_q);
		check(
			ArchiveBaseName(u"..hidden. "_q, QString()) == u"hidden"_q,
			u"ArchiveBaseName strips dots"_q);
		check(
			ArchiveBaseName(u"con"_q, QString()) == u"_con"_q
				&& ArchiveBaseName(u"LPT1.old"_q, QString())
					== u"_LPT1.old"_q
				&& ArchiveBaseName(u"Console"_q, QString())
					== u"Console"_q,
			u"ArchiveBaseName and the device names"_q);
		const auto longName = ArchiveBaseName(
			QString(kNameLimit - 1, QChar('a')) + grin + grin,
			QString());
		check(
			longName.size() == kNameLimit - 1
				&& !longName.back().isHighSurrogate(),
			u"ArchiveBaseName cuts between characters"_q);

		const auto folder = u"/nonexistent-oblivion-selftest/"_q;
		check(
			WithExtension(folder + u"a.zip"_q, u"zip"_q)
				== folder + u"a.zip"_q
				&& WithExtension(folder + u"a.ZIP"_q, u"zip"_q)
					== folder + u"a.ZIP"_q,
			u"WithExtension keeps the right extension"_q);
		check(
			WithExtension(folder + u"a"_q, u"zip"_q)
				== folder + u"a.zip"_q
				&& WithExtension(folder + u"a.gif"_q, u"png"_q)
					== folder + u"a.gif.png"_q,
			u"WithExtension adds the extension"_q);
	}

	// Manifest.
	const auto set = ManifestSet{
		.title = cyrillic,
		.shortName = u"persik_cat"_q,
		.type = SetType::Stickers,
		.id = 1234567890123456789ULL,
	};
	const auto manifestFiles = std::vector<ManifestFile>{
		{
			.file = EntryName(0, 3, grin, u"webp"_q),
			.emoji = grin,
			.emojis = { grin, heart },
			.kind = Kind::Static,
			.size = QSize(512, 512),
		},
		{
			.file = EntryName(1, 3, family, u"tgs"_q),
			.emoji = family,
			.emojis = { family },
			.kind = Kind::Animated,
			.size = QSize(512, 512),
		},
		{
			.file = EntryName(2, 3, keycap, u"webm"_q),
			.emoji = keycap,
			.emojis = { keycap },
			.kind = Kind::Video,
			.size = QSize(512, 384),
			.missing = true,
		},
	};
	const auto manifest = BuildManifest(set, manifestFiles);
	{
		auto error = QJsonParseError();
		const auto parsed = QJsonDocument::fromJson(manifest, &error);
		if (check(
				(error.error == QJsonParseError::NoError)
					&& parsed.isObject(),
				u"BuildManifest gives JSON: "_q + error.errorString())) {
			const auto root = parsed.object();
			check(
				root.value(u"title"_q).toString() == cyrillic
					&& root.value(u"short_name"_q).toString()
						== u"persik_cat"_q
					&& root.value(u"type"_q).toString() == u"stickers"_q
					&& root.value(u"id"_q).toString()
						== u"1234567890123456789"_q
					&& root.value(u"link"_q).toString()
						== u"https://t.me/addstickers/persik_cat"_q
					&& root.value(u"version"_q).toInt() == kManifestVersion
					&& root.value(u"count"_q).toInt() == 3
					&& root.value(u"saved"_q).toInt() == 2,
				u"Manifest: the set"_q);
			const auto list = root.value(u"files"_q).toArray();
			if (check(list.size() == 3, u"Manifest: three files"_q)) {
				const auto first = list[0].toObject();
				const auto firstEmojis = first.value(u"emojis"_q).toArray();
				check(
					first.value(u"file"_q).toString()
						== u"001_"_q + grin + u".webp"_q
						&& first.value(u"emoji"_q).toString() == grin
						&& firstEmojis.size() == 2
						&& firstEmojis[1].toString() == heart
						&& first.value(u"kind"_q).toString() == u"static"_q
						&& first.value(u"width"_q).toInt() == 512
						&& first.value(u"height"_q).toInt() == 512
						&& !first.contains(u"missing"_q),
					u"Manifest: a static sticker"_q);
				const auto second = list[1].toObject();
				check(
					second.value(u"file"_q).toString()
						== u"002_"_q + family + u".tgs"_q
						&& second.value(u"emoji"_q).toString() == family
						&& second.value(u"kind"_q).toString()
							== u"animated"_q,
					u"Manifest: an animated sticker"_q);
				const auto third = list[2].toObject();
				check(
					third.value(u"file"_q).toString() == u"003.webm"_q
						&& third.value(u"emoji"_q).toString() == keycap
						&& third.value(u"kind"_q).toString() == u"video"_q
						&& third.value(u"height"_q).toInt() == 384
						&& third.value(u"missing"_q).toBool(),
					u"Manifest: a missing video sticker"_q);
			}
		}
		const auto emojiSet = ManifestSet{
			.title = u"Hearts"_q,
			.shortName = u"hearts"_q,
			.type = SetType::Emoji,
		};
		const auto root = QJsonDocument::fromJson(
			BuildManifest(emojiSet, {})).object();
		check(
			root.value(u"type"_q).toString() == u"emoji"_q
				&& root.value(u"link"_q).toString()
					== u"https://t.me/addemoji/hearts"_q
				&& !root.contains(u"id"_q)
				&& root.value(u"files"_q).toArray().isEmpty(),
			u"Manifest of an emoji set"_q);
		check(
			!QJsonDocument::fromJson(BuildManifest(ManifestSet(), {}))
				.object().contains(u"link"_q),
			u"Manifest without a short name has no link"_q);
	}

	// Archive round trip.
	{
		const auto files = std::vector<ArchiveFile>{
			{
				.name = ManifestName(),
				.bytes = manifest,
				.compressed = true,
			},
			{
				.name = manifestFiles[0].file,
				.bytes = PatternBytes(300 * 1024 + 17, 1),
			},
			{
				.name = manifestFiles[1].file,
				.bytes = QByteArray(4096, char(0)),
			},
			{
				.name = u"003.webm"_q,
				.bytes = PatternBytes(1, 3),
			},
			{
				.name = u"004_"_q + cyrillic.left(3) + u".tgs"_q,
				.bytes = PatternBytes(kZipWriteChunk + 5, 4),
				.compressed = true,
			},
		};
		const auto modified = QDateTime(
			QDate(2026, 10, 6),
			QTime(12, 34, 56));
		const auto archive = BuildArchive(files, modified);
		if (check(
				archive.startsWith("PK\x03\x04"),
				u"BuildArchive gives a zip: %1 bytes"_q.arg(
					archive.size()))) {
			auto total = qint64(0);
			for (const auto &file : files) {
				total += file.bytes.size();
			}
			log.push_back(u"sticker_export: archive of %1 files, %2 -> "
				"%3 bytes"_q.arg(files.size()).arg(total).arg(
					archive.size()));

			auto reader = zlib::FileToRead(archive);
			auto global = unz_global_info();
			check(
				reader.getGlobalInfo(&global) == UNZ_OK
					&& int(global.number_entry) == int(files.size()),
				u"Archive: the number of files"_q);
			for (const auto &file : files) {
				const auto name = file.name.toUtf8();
				reader.clearError();
				if (!check(
						reader.locateFile(
							name.constData(),
							zlib::kCaseSensitive) == UNZ_OK,
						u"Archive: locate "_q + file.name)) {
					continue;
				}
				auto info = unz_file_info();
				const auto got = reader.getCurrentFileInfo(
					&info,
					nullptr,
					0,
					nullptr,
					0,
					nullptr,
					0);
				check(
					got == UNZ_OK
						&& (info.flag & kZipUtf8Flag)
						&& (info.version == uLong(kZipMadeBy))
						&& ((info.external_fa >> 16) == uLong(kZipFileMode))
						&& (info.uncompressed_size
							== uLong(file.bytes.size()))
						&& (info.compression_method
							== uLong(file.compressed ? Z_DEFLATED : 0))
						&& (file.compressed
							|| info.compressed_size
								== info.uncompressed_size)
						&& (info.tmu_date.tm_year == 2026)
						&& (info.tmu_date.tm_mon == 9)
						&& (info.tmu_date.tm_mday == 6)
						&& (info.tmu_date.tm_hour == 12)
						&& (info.tmu_date.tm_min == 34)
						&& (info.tmu_date.tm_sec == 56),
					u"Archive: the header of "_q + file.name);
				check(
					reader.readCurrentFileContent(
						16 * 1024 * 1024) == file.bytes,
					u"Archive: the content of "_q + file.name);
			}

			// The order of the files is the order they were given in.
			reader.clearError();
			auto order = QStringList();
			if (reader.goToFirstFile() == UNZ_OK) {
				do {
					order.push_back(reader.getCurrentFileName());
				} while (reader.goToNextFile() == UNZ_OK);
			}
			auto expected = QStringList();
			for (const auto &file : files) {
				expected.push_back(file.name);
			}
			check(order == expected, u"Archive: the order of the files"_q);
		}
		check(
			BuildArchive({}, modified).startsWith("PK"),
			u"BuildArchive of nothing is an empty zip"_q);
		check(
			BuildArchive(
				{ { .name = QString(), .bytes = "x" } },
				modified).isEmpty(),
			u"BuildArchive refuses a file without a name"_q);
		const auto cancelled = std::make_shared<std::atomic<bool>>(true);
		check(
			BuildArchive(files, modified, cancelled).isEmpty(),
			u"BuildArchive stops when cancelled"_q);
		check(
			BuildArchive(files, QDateTime()).startsWith("PK"),
			u"BuildArchive without a date"_q);
	}

	// Writing a file and cancelling that.
	{
		const auto folder = cWorkingDir()
			+ u"oblivion_selftest_sticker_export/"_q;
		QDir(folder).removeRecursively();
		const auto read = [](const QString &path) {
			auto file = QFile(path);
			return file.open(QIODevice::ReadOnly)
				? file.readAll()
				: QByteArray();
		};

		// With what a write that was stopped could leave behind.
		const auto entries = [&] {
			return QDir(folder).entryList(
				QDir::Files | QDir::Hidden | QDir::System);
		};
		if (check(QDir().mkpath(folder), u"A folder for the files"_q)) {
			const auto name = u"set.zip"_q;
			const auto path = folder + name;
			const auto fresh = PatternBytes(2 * kZipWriteChunk + 7, 5);
			const auto old = QByteArray("the file that was here before");
			const auto stopped = std::make_shared<std::atomic<bool>>(true);
			const auto going = std::make_shared<std::atomic<bool>>(false);

			check(
				SaveBytes(path, fresh, stopped) == WriteResult::Cancelled
					&& entries().isEmpty(),
				u"SaveBytes leaves nothing when cancelled"_q);
			check(
				SaveBytes(path, fresh, going) == WriteResult::Written
					&& read(path) == fresh
					&& entries() == QStringList{ name },
				u"SaveBytes writes a file in parts"_q);
			check(
				SaveBytes(path, old) == WriteResult::Written
					&& read(path) == old
					&& entries() == QStringList{ name },
				u"SaveBytes replaces a file"_q);
			check(
				SaveBytes(path, fresh, stopped) == WriteResult::Cancelled
					&& read(path) == old
					&& entries() == QStringList{ name },
				u"SaveBytes keeps the old file when cancelled"_q);
			check(
				SaveBytes(folder + u"no-such-folder/"_q + name, fresh)
					== WriteResult::Failed,
				u"SaveBytes fails where nothing can be written"_q);

			// A cancel that came while the file was put in its place.
			check(
				!DropCancelled(path, false, going)
					&& !DropCancelled(path, false, nullptr)
					&& read(path) == old,
				u"DropCancelled does nothing without a cancel"_q);
			check(
				!DropCancelled(path, true, stopped) && read(path) == old,
				u"DropCancelled keeps a file that replaced another"_q);
			check(
				DropCancelled(path, false, stopped) && entries().isEmpty(),
				u"DropCancelled removes a new file"_q);
		}
		QDir(folder).removeRecursively();
	}

	// Converting.
	{
		const auto image = PatternImage(128, 0);
		const auto png = EncodePng(image);
		if (check(IsPng(png), u"EncodePng"_q)) {
			check(
				ConvertSticker(png, Kind::Static, Format::Png) == png,
				u"A PNG sticker is saved as it is"_q);
		}
		if (QImageWriter::supportedImageFormats().contains(
				QByteArray("webp"))) {
			auto webp = QByteArray();
			{
				auto buffer = QBuffer(&webp);
				buffer.open(QIODevice::WriteOnly);
				auto writer = QImageWriter(&buffer, QByteArray("webp"));
				writer.setQuality(100);
				writer.write(image.convertToFormat(QImage::Format_ARGB32));
			}
			if (check(!webp.isEmpty(), u"A sample WebP"_q)) {
				check(
					ConvertSticker(webp, Kind::Static, Format::Webp) == webp,
					u"A WebP sticker is saved as it is"_q);
				const auto converted = ConvertSticker(
					webp,
					Kind::Static,
					Format::Png);
				const auto read = QImage::fromData(converted, "PNG");
				check(
					IsPng(converted)
						&& (read.size() == image.size())
						&& read.hasAlphaChannel()
						&& (qAlpha(read.pixel(0, 0)) == 0)
						&& (qAlpha(read.pixel(48, 48)) == 255),
					u"WebP to PNG keeps the size and the transparency"_q);
			}
		} else {
			log.push_back(u"sticker_export: no WebP writer, the WebP to "
				"PNG check is skipped"_q);
		}
		check(
			ConvertSticker("not an image", Kind::Static, Format::Png)
				.isEmpty(),
			u"Garbage is not converted to PNG"_q);
		check(
			ConvertSticker(QByteArray(), Kind::Static, Format::Webp)
				.isEmpty(),
			u"Nothing is converted to nothing"_q);
		check(
			ConvertSticker(png, Kind::Static, Format::Gif).isEmpty()
				&& ConvertSticker(png, Kind::Static, Format::Tgs).isEmpty()
				&& ConvertSticker(png, Kind::Video, Format::Png).isEmpty()
				&& ConvertSticker(png, Kind::Animated, Format::Webm)
					.isEmpty(),
			u"A kind has only its own formats"_q);

		const auto tgs = ResourceSticker();
		if (check(!tgs.isEmpty(), u"A sample .tgs from the resources"_q)) {
			const auto json = ConvertSticker(
				tgs,
				Kind::Animated,
				Format::Json);
			const auto info = Lottie::ReadInfo(json);
			check(
				ConvertSticker(tgs, Kind::Animated, Format::Tgs) == tgs,
				u"A .tgs sticker is saved as it is"_q);
			check(
				json.startsWith('{') && info.valid(),
				u"TGS to Lottie JSON"_q);
			const auto side = 96;
			const auto gif = ConvertSticker(
				tgs,
				Kind::Animated,
				Format::Gif,
				nullptr,
				side);
			if (check(gif.startsWith("GIF89a"), u"TGS to GIF"_q)) {
				const auto clip = VideoCore::ReadClipInfo(QString(), gif);
				const auto expected = info.size.scaled(
					side,
					side,
					Qt::KeepAspectRatio);
				check(
					clip.valid()
						&& (clip.size == expected)
						&& (std::abs(clip.duration - info.duration)
							<= std::max(info.duration / 10, crl::time(60))),
					u"TGS to GIF: %1x%2, %3 ms of %4 ms"_q
						.arg(clip.size.width())
						.arg(clip.size.height())
						.arg(clip.duration)
						.arg(info.duration));
				log.push_back(u"sticker_export: GIF of a %1 frames .tgs "
					"at %2px is %3 bytes"_q
					.arg(info.frames)
					.arg(side)
					.arg(gif.size()));
			}
			const auto cancelled = std::make_shared<std::atomic<bool>>(
				true);
			check(
				ConvertSticker(
					tgs,
					Kind::Animated,
					Format::Gif,
					cancelled,
					side).isEmpty(),
				u"TGS to GIF stops when cancelled"_q);
		}
		check(
			ConvertSticker("{}", Kind::Animated, Format::Gif).isEmpty(),
			u"Garbage is not converted to GIF"_q);

		auto frames = std::vector<QImage>();
		for (auto i = 0; i != 8; ++i) {
			frames.push_back(PatternImage(VideoCore::kEmojiSide, i * 3));
		}
		const auto video = VideoCore::EncodeVideoSticker(
			frames,
			50,
			VideoCore::kEmojiSide);
		if (check(
				video.ok && !video.webm.isEmpty(),
				u"A sample WebM: "_q + video.error)) {
			check(
				ConvertSticker(video.webm, Kind::Video, Format::Webm)
					== video.webm,
				u"A video sticker is saved as it is"_q);
			const auto gif = ConvertSticker(
				video.webm,
				Kind::Video,
				Format::Gif);
			if (check(gif.startsWith("GIF8"), u"WebM to GIF"_q)) {
				const auto clip = VideoCore::ReadClipInfo(QString(), gif);
				check(
					clip.valid()
						&& (clip.size == QSize(
							VideoCore::kEmojiSide,
							VideoCore::kEmojiSide))
						&& (std::abs(clip.duration - video.duration) <= 100),
					u"WebM to GIF: %1x%2, %3 ms of %4 ms"_q
						.arg(clip.size.width())
						.arg(clip.size.height())
						.arg(clip.duration)
						.arg(video.duration));
			}
		}
		check(
			ConvertSticker("not a video", Kind::Video, Format::Gif)
				.isEmpty(),
			u"Garbage is not converted to GIF as a video"_q);
	}

	if (passed) {
		log.push_back(u"sticker_export: all checks passed in %1 ms."_q.arg(
			timer.elapsed()));
	}
	return passed;
}

} // namespace Oblivion::StickerExport

namespace Oblivion {

void AddStickerExportActions(
		not_null<Ui::PopupMenu*> menu,
		std::shared_ptr<ChatHelpers::Show> show,
		not_null<DocumentData*> document,
		Data::FileOrigin origin,
		const style::ComposeIcons *icons) {
	using namespace StickerExport;

	const auto sticker = document->sticker();
	if (!sticker || !show || !show->valid()) {
		return;
	}
	if (!origin) {
		origin = document->stickerSetOrigin();
	}
	const auto kind = KindOf(sticker);
	const auto original = OriginalFormat(kind, document->mimeString());
	const auto weak = base::make_weak(&document->session());
	auto submenu = std::make_unique<Ui::PopupMenu>(menu, menu->st());
	for (const auto format : FormatsFor(kind, original)) {
		submenu->addAction(FormatItemText(format, original), [=] {
			ChoosePathAndSave(show, weak, document, origin, format);
		});
	}
	menu->addAction(
		tr::lng_oblivion_sexport_save_as(tr::now),
		std::move(submenu),
		UsualMenu(menu) ? &st::menuIconDownload : nullptr);

	// The set is requested by the account of the window, the access hash
	// of the set is good only for the account the sticker came to.
	if (&show->session() == &document->session()) {
		AddStickerSetExportAction(menu, show, sticker->set, icons);
	}
}

void ExportStickerSet(
		std::shared_ptr<ChatHelpers::Show> show,
		StickerSetIdentifier set) {
	StickerExport::Manager::Instance().startExport(
		std::move(show),
		std::move(set));
}

void AddStickerSetExportAction(
		not_null<Ui::PopupMenu*> menu,
		std::shared_ptr<ChatHelpers::Show> show,
		StickerSetIdentifier set,
		const style::ComposeIcons *icons) {
	if (set.empty() || !show) {
		return;
	}
	menu->addAction(
		tr::lng_oblivion_sexport_save_set(tr::now),
		[=] { ExportStickerSet(show, set); },
		StickerExport::UsualMenu(menu) ? &st::menuIconExport : nullptr);
}

} // namespace Oblivion
