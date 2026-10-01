/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_sticker_packs_core.h"

#include <QtCore/QBuffer>
#include <QtCore/QFile>
#include <QtGui/QImageReader>
#include <QtGui/QImageWriter>
#include <QtGui/QPainter>

#include <cmath>
#include <cstring>
#include <limits>

namespace Oblivion::StickerPacks {
namespace {

constexpr auto kMaxOutline = 32;
constexpr auto kOutlineAlpha = 128; // What counts as the object.
constexpr auto kFar = 1e20; // "No object anywhere" squared distance.

// The rate is the one written in the container: whole milliseconds and
// the rounding give a bit more than 30 for files that keep the limit.
constexpr auto kFrameRateSlack = 1.;

constexpr auto kWebpQualities = std::array{ 95, 90, 82, 72, 60, 45, 30 };

constexpr auto kExportNameMaxLength = 64;

[[nodiscard]] QImage Premultiplied(const QImage &image) {
	auto result = (image.format() == QImage::Format_ARGB32_Premultiplied)
		? image
		: image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	result.setDevicePixelRatio(1.);
	return result;
}

// Squared distance transform of one line (Felzenszwalb, Huttenlocher):
// values[i] becomes min over j of ((i - j)^2 + values[j]).
void DistanceLine(
		std::vector<double> &values,
		int count,
		std::vector<double> &result,
		std::vector<int> &parabolas,
		std::vector<double> &bounds) {
	const auto infinity = std::numeric_limits<double>::infinity();
	const auto intersection = [&](int q, int p) {
		return ((values[q] + double(q) * q) - (values[p] + double(p) * p))
			/ (2. * q - 2. * p);
	};
	auto k = 0;
	parabolas[0] = 0;
	bounds[0] = -infinity;
	bounds[1] = infinity;
	for (auto q = 1; q != count; ++q) {
		auto s = intersection(q, parabolas[k]);
		while (k > 0 && s <= bounds[k]) {
			--k;
			s = intersection(q, parabolas[k]);
		}
		++k;
		parabolas[k] = q;
		bounds[k] = s;
		bounds[k + 1] = infinity;
	}
	k = 0;
	for (auto q = 0; q != count; ++q) {
		while (bounds[k + 1] < q) {
			++k;
		}
		const auto delta = double(q - parabolas[k]);
		result[q] = delta * delta + values[parabolas[k]];
	}
	for (auto q = 0; q != count; ++q) {
		values[q] = result[q];
	}
}

// White under everything that is closer than width to the object.
void ApplyOutline(QImage &image, int width) {
	Expects(image.format() == QImage::Format_ARGB32_Premultiplied);

	const auto w = image.width();
	const auto h = image.height();
	if (w <= 0 || h <= 0 || width <= 0) {
		return;
	}
	auto distances = std::vector<double>(size_t(w) * h, kFar);
	auto found = false;
	for (auto y = 0; y != h; ++y) {
		const auto line = reinterpret_cast<const QRgb*>(
			image.constScanLine(y));
		for (auto x = 0; x != w; ++x) {
			if (qAlpha(line[x]) >= kOutlineAlpha) {
				distances[size_t(y) * w + x] = 0.;
				found = true;
			}
		}
	}
	if (!found) {
		return;
	}
	const auto longest = std::max(w, h);
	auto values = std::vector<double>(longest);
	auto buffer = std::vector<double>(longest);
	auto parabolas = std::vector<int>(longest);
	auto bounds = std::vector<double>(longest + 1);
	for (auto x = 0; x != w; ++x) {
		for (auto y = 0; y != h; ++y) {
			values[y] = distances[size_t(y) * w + x];
		}
		DistanceLine(values, h, buffer, parabolas, bounds);
		for (auto y = 0; y != h; ++y) {
			distances[size_t(y) * w + x] = values[y];
		}
	}
	for (auto y = 0; y != h; ++y) {
		const auto row = distances.data() + size_t(y) * w;
		for (auto x = 0; x != w; ++x) {
			values[x] = row[x];
		}
		DistanceLine(values, w, buffer, parabolas, bounds);
		const auto line = reinterpret_cast<QRgb*>(image.scanLine(y));
		for (auto x = 0; x != w; ++x) {
			const auto coverage = std::clamp(
				width + 0.5 - std::sqrt(values[x]),
				0.,
				1.);
			if (coverage <= 0.) {
				continue;
			}
			const auto pixel = line[x];
			const auto alpha = qAlpha(pixel);
			const auto under = int(std::lround(coverage * (255 - alpha)));
			if (under <= 0) {
				continue;
			}
			line[x] = qRgba(
				std::min(qRed(pixel) + under, 255),
				std::min(qGreen(pixel) + under, 255),
				std::min(qBlue(pixel) + under, 255),
				std::min(alpha + under, 255));
		}
	}
}

[[nodiscard]] QByteArray WriteWebp(const QImage &image, int quality) {
	auto result = QByteArray();
	auto buffer = QBuffer(&result);
	if (!buffer.open(QIODevice::WriteOnly)) {
		return QByteArray();
	}
	auto writer = QImageWriter(&buffer, "webp");
	writer.setQuality(quality);
	if (!writer.write(image)) {
		return QByteArray();
	}
	buffer.close();
	return result;
}

[[nodiscard]] bool StartsWith(const QByteArray &content, const char *magic) {
	return content.startsWith(magic);
}

[[nodiscard]] bool IsGzip(const QByteArray &content) {
	return (content.size() > 2)
		&& (uchar(content[0]) == 0x1F)
		&& (uchar(content[1]) == 0x8B);
}

[[nodiscard]] bool IsJson(const QByteArray &content) {
	for (const auto ch : content) {
		if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
			continue;
		} else if (uchar(ch) == 0xEF || uchar(ch) == 0xBB || uchar(ch) == 0xBF) {
			continue; // UTF-8 byte order mark.
		}
		return (ch == '{');
	}
	return false;
}

[[nodiscard]] bool IsMatroska(const QByteArray &content) {
	return (content.size() > 4)
		&& (uchar(content[0]) == 0x1A)
		&& (uchar(content[1]) == 0x45)
		&& (uchar(content[2]) == 0xDF)
		&& (uchar(content[3]) == 0xA3);
}

// "ftyp" boxes: videos, but HEIF and AVIF images too.
[[nodiscard]] bool IsIsoMedia(const QByteArray &content) {
	return (content.size() > 12) && (content.mid(4, 4) == "ftyp");
}

[[nodiscard]] bool IsIsoImage(const QByteArray &content) {
	if (!IsIsoMedia(content)) {
		return false;
	}
	const auto brand = content.mid(8, 4);
	return (brand == "heic")
		|| (brand == "heix")
		|| (brand == "heif")
		|| (brand == "mif1")
		|| (brand == "msf1")
		|| (brand == "avif")
		|| (brand == "avis");
}

[[nodiscard]] QString Extension(const QString &name) {
	const auto dot = name.lastIndexOf('.');
	return (dot >= 0) ? name.mid(dot + 1).toLower() : QString();
}

// How many frames the image file has, stops counting at two.
[[nodiscard]] int ImageFrames(const QByteArray &content) {
	auto copy = content;
	auto buffer = QBuffer(&copy);
	if (!buffer.open(QIODevice::ReadOnly)) {
		return 0;
	}
	auto reader = QImageReader(&buffer);
	if (!reader.canRead()) {
		return 0;
	} else if (!reader.supportsAnimation()) {
		return 1;
	}
	const auto count = reader.imageCount();
	if (count > 0) {
		return std::min(count, 2);
	}
	// The count is unknown before reading: try to get a second frame.
	return (!reader.read().isNull() && !reader.read().isNull()) ? 2 : 1;
}

[[nodiscard]] QString Transliterated(QChar ch) {
	struct Pair {
		char16_t from = 0;
		const char *to = nullptr;
	};
	static constexpr Pair kTable[] = {
		{ u'а', "a" }, { u'б', "b" }, { u'в', "v" }, { u'г', "g" },
		{ u'д', "d" }, { u'е', "e" }, { u'ё', "e" }, { u'ж', "zh" },
		{ u'з', "z" }, { u'и', "i" }, { u'й', "y" }, { u'к', "k" },
		{ u'л', "l" }, { u'м', "m" }, { u'н', "n" }, { u'о', "o" },
		{ u'п', "p" }, { u'р', "r" }, { u'с', "s" }, { u'т', "t" },
		{ u'у', "u" }, { u'ф', "f" }, { u'х', "h" }, { u'ц', "ts" },
		{ u'ч', "ch" }, { u'ш', "sh" }, { u'щ', "sch" }, { u'ъ', "" },
		{ u'ы', "y" }, { u'ь', "" }, { u'э', "e" }, { u'ю', "yu" },
		{ u'я', "ya" }, { u'і', "i" }, { u'ї', "yi" }, { u'є', "ye" },
		{ u'ґ', "g" }, { u'ў', "u" },
	};
	const auto lower = ch.toLower().unicode();
	if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9')) {
		return QString(QChar(lower));
	}
	for (const auto &pair : kTable) {
		if (pair.from == lower) {
			return QString::fromLatin1(pair.to);
		}
	}
	return u"_"_q;
}

[[nodiscard]] bool IsLatinLetter(QChar ch) {
	const auto code = ch.unicode();
	return (code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z');
}

[[nodiscard]] bool IsDigit(QChar ch) {
	const auto code = ch.unicode();
	return (code >= '0' && code <= '9');
}

} // namespace

QString MimeType(Format format) {
	switch (format) {
	case Format::Static: return u"image/webp"_q;
	case Format::Animated: return u"application/x-tgsticker"_q;
	case Format::Video: return u"video/webm"_q;
	}
	return QString();
}

QString FileName(Format format) {
	switch (format) {
	case Format::Static: return u"sticker.webp"_q;
	case Format::Animated: return u"sticker.tgs"_q;
	case Format::Video: return u"sticker.webm"_q;
	}
	return QString();
}

QRect OpaqueBounds(const QImage &image, int threshold) {
	if (image.isNull()) {
		return QRect();
	} else if (!image.hasAlphaChannel()) {
		return image.rect();
	}
	const auto source = (image.format() == QImage::Format_ARGB32_Premultiplied
		|| image.format() == QImage::Format_ARGB32)
		? image
		: image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	const auto w = source.width();
	const auto h = source.height();
	auto left = w;
	auto top = h;
	auto right = -1;
	auto bottom = -1;
	for (auto y = 0; y != h; ++y) {
		const auto line = reinterpret_cast<const QRgb*>(
			source.constScanLine(y));
		auto first = -1;
		auto last = -1;
		for (auto x = 0; x != w; ++x) {
			if (qAlpha(line[x]) >= threshold) {
				if (first < 0) {
					first = x;
				}
				last = x;
			}
		}
		if (first >= 0) {
			left = std::min(left, first);
			right = std::max(right, last);
			top = std::min(top, y);
			bottom = y;
		}
	}
	return (right < left)
		? QRect()
		: QRect(QPoint(left, top), QPoint(right, bottom));
}

bool HasTransparency(const QImage &image) {
	if (image.isNull() || !image.hasAlphaChannel()) {
		return false;
	}
	const auto source = (image.format() == QImage::Format_ARGB32_Premultiplied
		|| image.format() == QImage::Format_ARGB32)
		? image
		: image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	const auto w = source.width();
	const auto h = source.height();
	for (auto y = 0; y != h; ++y) {
		const auto line = reinterpret_cast<const QRgb*>(
			source.constScanLine(y));
		for (auto x = 0; x != w; ++x) {
			if (qAlpha(line[x]) != 255) {
				return true;
			}
		}
	}
	return false;
}

QImage ComposeSticker(const QImage &image, StaticOptions options) {
	if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
		return QImage();
	}
	auto source = Premultiplied(image);
	if (source.isNull()) {
		return QImage();
	}
	auto width = options.outline
		? std::clamp(options.outlineWidth, 1, kMaxOutline)
		: 0;
	if (width > 0) {
		const auto bounds = OpaqueBounds(source);
		if (bounds.isEmpty()) {
			width = 0; // Nothing to outline.
		} else if (bounds != source.rect()) {
			source = source.copy(bounds);
		}
	}
	const auto inner = kStickerSide - 2 * width;
	const auto fitted = source.size().scaled(
		QSize(inner, inner),
		Qt::KeepAspectRatio
	).expandedTo(QSize(1, 1));
	auto scaled = (source.size() == fitted)
		? source
		: Premultiplied(source.scaled(
			fitted,
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation));
	if (scaled.isNull() || !width) {
		return scaled;
	}
	auto result = QImage(
		fitted + QSize(2 * width, 2 * width),
		QImage::Format_ARGB32_Premultiplied);
	if (result.isNull()) {
		return QImage();
	}
	result.fill(Qt::transparent);
	{
		auto p = QPainter(&result);
		p.setCompositionMode(QPainter::CompositionMode_Source);
		p.drawImage(QPoint(width, width), scaled);
	}
	ApplyOutline(result, width);
	return result;
}

bool WebpSupported() {
	static const auto result = QImageWriter::supportedImageFormats().contains(
		QByteArray("webp"));
	return result;
}

Prepared EncodeStatic(const QImage &composed) {
	if (composed.isNull() || !WebpSupported()) {
		return Prepared();
	}
	const auto image = HasTransparency(composed)
		? composed.convertToFormat(QImage::Format_ARGB32)
		: composed.convertToFormat(QImage::Format_RGB32);
	if (image.isNull()) {
		return Prepared();
	}
	for (const auto quality : kWebpQualities) {
		auto bytes = WriteWebp(image, quality);
		if (bytes.isEmpty()) {
			return Prepared();
		} else if (bytes.size() <= kStaticMaxBytes) {
			return Prepared{
				.format = Format::Static,
				.bytes = std::move(bytes),
				.size = image.size(),
			};
		}
	}
	return Prepared();
}

Prepared PrepareStatic(const QImage &image, StaticOptions options) {
	return EncodeStatic(ComposeSticker(image, options));
}

LottieCheck CheckLottie(const QByteArray &tgsOrJson) {
	return CheckLottie(LottieEdit::Document::FromData(tgsOrJson));
}

LottieCheck CheckLottie(LottieEdit::Document document) {
	auto result = LottieCheck();
	if (!document.valid()) {
		return result;
	}
	result.validation = LottieEdit::Validate(document, {
		.measureSize = true,
		.renderChecks = false,
	});
	result.tgs = document.toTgs();
	result.document = std::move(document);
	return result;
}

LottieCheck FixLottie(const LottieCheck &check) {
	if (!check.fixable()) {
		return check;
	}
	auto edit = LottieEdit::AutoFix(check.document);
	if (!edit.ok()) {
		return check;
	}
	return CheckLottie(std::move(edit.document));
}

Prepared PrepareAnimated(const LottieCheck &check) {
	if (!check.acceptable()) {
		return Prepared();
	}
	const auto fps = check.document.frameRate();
	const auto frames = check.document.frames();
	return Prepared{
		.format = Format::Animated,
		.bytes = check.tgs,
		.size = check.document.size(),
		.duration = (fps > 0.)
			? crl::time(std::llround(frames * 1000. / fps))
			: crl::time(0),
	};
}

VideoCheck CheckVideoSticker(const QByteArray &webm) {
	auto result = VideoCheck();
	if (webm.isEmpty()) {
		return result;
	}
	result.info = VideoCore::ReadClipInfo(QString(), webm);
	const auto &info = result.info;
	const auto longer = std::max(info.size.width(), info.size.height());
	result.problem = !info.valid()
		? VideoProblem::Unreadable
		: (!IsMatroska(webm) || !webm.left(64).contains("webm"))
		? VideoProblem::Container
		: (info.codec != u"vp9"_q)
		? VideoProblem::Codec
		: info.hasAudio
		? VideoProblem::Audio
		: (longer != kStickerSide)
		? VideoProblem::Dimensions
		: (info.duration > VideoCore::kStickerMaxDuration)
		? VideoProblem::Duration
		: (info.fps > VideoCore::kStickerMaxFps + kFrameRateSlack)
		? VideoProblem::FrameRate
		: (webm.size() > VideoCore::kStickerMaxBytes)
		? VideoProblem::FileSize
		: VideoProblem::None;
	return result;
}

Prepared PrepareVideo(const QByteArray &webm, const VideoCheck &check) {
	if (!check.ok()) {
		return Prepared();
	}
	return Prepared{
		.format = Format::Video,
		.bytes = webm,
		.size = check.info.size,
		.duration = std::min(
			check.info.duration,
			VideoCore::kStickerMaxDuration),
	};
}

NameProblem CheckShortName(const QString &name) {
	if (name.isEmpty()) {
		return NameProblem::Empty;
	} else if (name.size() > kShortNameMaxLength) {
		return NameProblem::TooLong;
	}
	for (const auto ch : name) {
		if (!IsLatinLetter(ch) && !IsDigit(ch) && (ch != '_')) {
			return NameProblem::Symbols;
		}
	}
	if (!IsLatinLetter(name.front())) {
		return NameProblem::Start;
	} else if (name.contains(u"__"_q) || name.endsWith('_')) {
		return NameProblem::Underscores;
	}
	return NameProblem::None;
}

QString ShortNameFromTitle(const QString &title) {
	auto result = QString();
	result.reserve(title.size() * 2);
	for (const auto ch : title) {
		const auto part = Transliterated(ch);
		if (part == u"_"_q) {
			if (!result.isEmpty() && !result.endsWith('_')) {
				result.append('_');
			}
		} else {
			result.append(part);
		}
	}
	while (!result.isEmpty() && !IsLatinLetter(result.front())) {
		result.remove(0, 1);
	}
	result.truncate(kShortNameMaxLength);
	while (result.endsWith('_')) {
		result.chop(1);
	}
	return result;
}

QString PackLink(const QString &shortName, bool emoji) {
	return (emoji
		? u"https://t.me/addemoji/"_q
		: u"https://t.me/addstickers/"_q) + shortName;
}

FileKind DetectFileKind(const QString &name, const QByteArray &content) {
	const auto extension = Extension(name);
	if (IsGzip(content) || IsJson(content)) {
		return FileKind::Lottie;
	} else if (IsMatroska(content)) {
		return CheckVideoSticker(content).ok()
			? FileKind::VideoSticker
			: FileKind::Video;
	} else if (IsIsoImage(content)) {
		return FileKind::Image;
	} else if (IsIsoMedia(content)) {
		return FileKind::Video;
	} else if (StartsWith(content, "GIF8")) {
		return (ImageFrames(content) > 1) ? FileKind::Video : FileKind::Image;
	} else if (ImageFrames(content) > 0) {
		return FileKind::Image;
	} else if (extension == u"tgs"_q || extension == u"json"_q) {
		return FileKind::Lottie;
	}
	static const auto kVideo = std::array{
		u"mp4"_q,
		u"mov"_q,
		u"m4v"_q,
		u"mkv"_q,
		u"webm"_q,
		u"gif"_q,
	};
	return ranges::contains(kVideo, extension)
		? FileKind::Video
		: FileKind::Unknown;
}

QString ExportExtension(ExportType type) {
	switch (type) {
	case ExportType::Png: return u"png"_q;
	case ExportType::Webp: return u"webp"_q;
	case ExportType::Tgs: return u"tgs"_q;
	case ExportType::Webm: return u"webm"_q;
	}
	return QString();
}

QString ExportMimeType(ExportType type) {
	switch (type) {
	case ExportType::Png: return u"image/png"_q;
	case ExportType::Webp: return MimeType(Format::Static);
	case ExportType::Tgs: return MimeType(Format::Animated);
	case ExportType::Webm: return MimeType(Format::Video);
	}
	return QString();
}

ExportType ExportTypeFor(Format format, const QString &extension) {
	switch (format) {
	case Format::Static: {
		const auto dot = extension.lastIndexOf('.');
		const auto clean = extension.mid(dot + 1).trimmed().toLower();
		return (clean == u"webp"_q) ? ExportType::Webp : ExportType::Png;
	}
	case Format::Animated: return ExportType::Tgs;
	case Format::Video: return ExportType::Webm;
	}
	return ExportType::Png;
}

ExportType ExportTypeForSending(Format format, qint64 pngBytes) {
	if (format != Format::Static) {
		return ExportTypeFor(format);
	}
	return (pngBytes > 0 && pngBytes <= kStaticMaxBytes)
		? ExportType::Png
		: ExportType::Webp;
}

bool ExportSendsForcedFile(ExportType type) {
	return (type == ExportType::Webp) || (type == ExportType::Webm);
}

QString ExportBaseName(const QString &hint) {
	auto result = QString();
	result.reserve(hint.size());
	for (const auto ch : hint) {
		const auto code = ch.unicode();
		const auto bad = (code < 0x20)
			|| (code == 0x7F)
			|| (ch == '/')
			|| (ch == '\\')
			|| (ch == ':')
			|| (ch == '*')
			|| (ch == '?')
			|| (ch == '"')
			|| (ch == '<')
			|| (ch == '>')
			|| (ch == '|');
		result.append(bad ? QChar('_') : ch);
	}
	result = result.trimmed();
	while (result.startsWith('.')) {
		result = result.mid(1).trimmed();
	}
	result = result.left(kExportNameMaxLength);
	if (!result.isEmpty() && result.back().isHighSurrogate()) {
		result.chop(1);
	}
	result = result.trimmed();
	while (result.endsWith('.')) {
		result.chop(1);
		result = result.trimmed();
	}
	return result.isEmpty() ? u"sticker"_q : result;
}

QString ExportFileName(const QString &hint, ExportType type) {
	return ExportBaseName(hint) + QChar('.') + ExportExtension(type);
}

QString ExportPath(const QString &chosen, ExportType type) {
	if (chosen.isEmpty()) {
		return QString();
	}
	const auto extension = ExportExtension(type);
	const auto slash = std::max(
		chosen.lastIndexOf('/'),
		chosen.lastIndexOf('\\'));
	const auto name = chosen.mid(slash + 1);
	return (name.size() > extension.size() + 1
		&& name.endsWith(QChar('.') + extension, Qt::CaseInsensitive))
		? chosen
		: (chosen + QChar('.') + extension);
}

QByteArray EncodePng(const QImage &composed) {
	if (composed.isNull()) {
		return QByteArray();
	}
	const auto image = composed.convertToFormat(QImage::Format_ARGB32);
	auto result = QByteArray();
	auto buffer = QBuffer(&result);
	if (image.isNull()
		|| !buffer.open(QIODevice::WriteOnly)
		|| !image.save(&buffer, "PNG")) {
		return QByteArray();
	}
	buffer.close();
	return result;
}

ExportFile MakeExportFile(
		const Prepared &prepared,
		const QImage &composed,
		ExportType type,
		const QString &hint) {
	const auto fits = (prepared.format == Format::Static)
		? (type == ExportType::Png || type == ExportType::Webp)
		: (type == ExportTypeFor(prepared.format));
	if (!prepared.valid() || !fits) {
		return ExportFile();
	}
	auto bytes = (type == ExportType::Png)
		? EncodePng(composed)
		: prepared.bytes;
	if (bytes.isEmpty()) {
		return ExportFile();
	}
	return ExportFile{
		.type = type,
		.name = ExportFileName(hint, type),
		.mime = ExportMimeType(type),
		.bytes = std::move(bytes),
		.size = (type == ExportType::Png) ? composed.size() : prepared.size,
	};
}

ExportFile MakeSendFile(
		const Prepared &prepared,
		const QImage &composed,
		const QString &hint) {
	if (prepared.format != Format::Static) {
		return MakeExportFile(
			prepared,
			composed,
			ExportTypeFor(prepared.format),
			hint);
	}
	auto png = MakeExportFile(prepared, composed, ExportType::Png, hint);
	const auto type = ExportTypeForSending(Format::Static, png.bytes.size());
	return (type == ExportType::Png)
		? png
		: MakeExportFile(prepared, composed, type, hint);
}

bool RunSelfTest(QStringList &log) {
	auto passed = true;
	const auto check = [&](bool condition, const QString &what) {
		if (!condition) {
			passed = false;
			log.push_back(u"FAIL: "_q + what);
		}
		return condition;
	};
	const auto sizeText = [](QSize size) {
		return u"%1x%2"_q.arg(size.width()).arg(size.height());
	};

	// Sizes: the longer side is always 512.
	{
		auto landscape = QImage(1200, 800, QImage::Format_RGB32);
		landscape.fill(QColor(40, 120, 200));
		const auto fitted = ComposeSticker(landscape);
		check(
			(fitted.size() == QSize(512, 341))
				&& (fitted.format() == QImage::Format_ARGB32_Premultiplied),
			u"ComposeSticker 1200x800: "_q + sizeText(fitted.size()));

		auto portrait = QImage(100, 300, QImage::Format_ARGB32);
		portrait.fill(QColor(200, 40, 40, 255));
		const auto up = ComposeSticker(portrait);
		check(
			(up.size() == QSize(170, 512)),
			u"ComposeSticker 100x300 is scaled up: "_q + sizeText(up.size()));

		auto thin = QImage(5000, 3, QImage::Format_RGB32);
		thin.fill(Qt::white);
		const auto line = ComposeSticker(thin);
		check(
			(line.width() == 512) && (line.height() >= 1),
			u"ComposeSticker 5000x3: "_q + sizeText(line.size()));

		check(ComposeSticker(QImage()).isNull(), u"ComposeSticker of null"_q);
	}

	// The outline: a disc on a big transparent canvas.
	auto disc = QImage(900, 700, QImage::Format_ARGB32_Premultiplied);
	disc.fill(Qt::transparent);
	{
		auto p = QPainter(&disc);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(220, 40, 60));
		p.drawEllipse(QRect(300, 200, 300, 300));
	}
	{
		const auto bounds = OpaqueBounds(disc);
		check(
			(bounds == QRect(300, 200, 300, 300)),
			u"OpaqueBounds of the disc: %1,%2 %3"_q.arg(
				bounds.x()
			).arg(bounds.y()).arg(sizeText(bounds.size())));
		check(HasTransparency(disc), u"HasTransparency of the disc"_q);
		check(
			OpaqueBounds(QImage()).isEmpty(),
			u"OpaqueBounds of null"_q);

		const auto width = kDefaultOutline;
		const auto outlined = ComposeSticker(disc, {
			.outline = true,
			.outlineWidth = width,
		});
		check(
			(outlined.size() == QSize(512, 512)),
			u"Outlined disc size: "_q + sizeText(outlined.size()));
		if (outlined.size() == QSize(512, 512)) {
			// The disc has the radius 248 around (256, 256).
			const auto at = [&](int x, int y) {
				return outlined.pixel(x, y);
			};
			const auto center = at(256, 256);
			check(
				(qAlpha(center) == 255) && (qRed(center) > 200)
					&& (qGreen(center) < 60),
				u"Outlined disc keeps its color in the center"_q);
			const auto ring = at(256, 3);
			check(
				(qAlpha(ring) == 255) && (qRed(ring) == 255)
					&& (qGreen(ring) == 255) && (qBlue(ring) == 255),
				u"The outline is opaque white: %1"_q.arg(ring, 8, 16));
			const auto side = at(508, 256);
			check(
				(qAlpha(side) == 255) && (qGreen(side) == 255),
				u"The outline is on the right side too"_q);
			check(
				(qAlpha(at(2, 2)) == 0) && (qAlpha(at(509, 509)) == 0),
				u"The corners stay transparent"_q);
			// 45 degrees: the outline ends 256 away from the center.
			const auto diagonal = [&](double radius) {
				const auto shift = int(std::lround(radius / std::sqrt(2.)));
				return at(256 + shift, 256 + shift);
			};
			check(
				(qAlpha(diagonal(252)) == 255)
					&& (qGreen(diagonal(252)) == 255),
				u"The outline is round (inside)"_q);
			check(
				(qAlpha(diagonal(260)) == 0),
				u"The outline is round (outside)"_q);
		}

		// Nothing to outline: no crash, the plain fit.
		auto empty = QImage(64, 32, QImage::Format_ARGB32_Premultiplied);
		empty.fill(Qt::transparent);
		const auto nothing = ComposeSticker(empty, { .outline = true });
		check(
			(nothing.size() == QSize(512, 256)),
			u"Outline of nothing: "_q + sizeText(nothing.size()));

		// An opaque image gets a white frame.
		auto opaque = QImage(300, 300, QImage::Format_RGB32);
		opaque.fill(QColor(10, 20, 30));
		const auto framed = ComposeSticker(opaque, { .outline = true });
		check(
			(framed.size() == QSize(512, 512))
				&& (framed.pixel(256, 2) == 0xFFFFFFFFU)
				&& (qRed(framed.pixel(256, 256)) == 10),
			u"Outline of an opaque image"_q);
	}

	// WebP.
	if (check(WebpSupported(), u"QImageWriter supports webp"_q)) {
		const auto prepared = PrepareStatic(disc, { .outline = true });
		check(
			prepared.valid()
				&& (prepared.format == Format::Static)
				&& (prepared.size == QSize(512, 512))
				&& (prepared.bytes.size() <= kStaticMaxBytes),
			u"PrepareStatic of the disc: %1 bytes"_q.arg(
				prepared.bytes.size()));
		check(
			prepared.bytes.startsWith("RIFF")
				&& (prepared.bytes.mid(8, 4) == "WEBP"),
			u"PrepareStatic writes WebP"_q);
		const auto decoded = QImage::fromData(prepared.bytes, "webp");
		check(
			(decoded.size() == QSize(512, 512))
				&& decoded.hasAlphaChannel()
				&& (qAlpha(decoded.pixel(2, 2)) == 0)
				&& (qAlpha(decoded.pixel(256, 256)) == 255),
			u"The WebP keeps the transparency"_q);
		check(
			DetectFileKind(u"a.bin"_q, prepared.bytes) == FileKind::Image,
			u"DetectFileKind of a WebP"_q);

		// The worst case for the size: noise.
		auto noise = QImage(512, 512, QImage::Format_ARGB32);
		auto seed = uint32(0x9E3779B9U);
		for (auto y = 0; y != noise.height(); ++y) {
			const auto line = reinterpret_cast<QRgb*>(noise.scanLine(y));
			for (auto x = 0; x != noise.width(); ++x) {
				seed = seed * 1664525U + 1013904223U;
				line[x] = (seed | 0xFF000000U);
			}
		}
		const auto heavy = EncodeStatic(noise);
		check(
			heavy.valid() && (heavy.bytes.size() <= kStaticMaxBytes),
			u"Noise fits %1 KB: %2 bytes"_q.arg(
				kStaticMaxBytes / 1024
			).arg(heavy.bytes.size()));
		check(!EncodeStatic(QImage()).valid(), u"EncodeStatic of null"_q);
	}

	// The sticker as a file: the names and the types.
	{
		check(
			(ExportExtension(ExportType::Png) == u"png"_q)
				&& (ExportExtension(ExportType::Webp) == u"webp"_q)
				&& (ExportExtension(ExportType::Tgs) == u"tgs"_q)
				&& (ExportExtension(ExportType::Webm) == u"webm"_q),
			u"ExportExtension"_q);
		check(
			(ExportMimeType(ExportType::Png) == u"image/png"_q)
				&& (ExportMimeType(ExportType::Webp) == u"image/webp"_q)
				&& (ExportMimeType(ExportType::Tgs)
					== u"application/x-tgsticker"_q)
				&& (ExportMimeType(ExportType::Webm) == u"video/webm"_q),
			u"ExportMimeType"_q);

		check(
			(ExportTypeFor(Format::Static) == ExportType::Png)
				&& (ExportTypeFor(Format::Static, u"png"_q)
					== ExportType::Png)
				&& (ExportTypeFor(Format::Static, u"jpg"_q)
					== ExportType::Png),
			u"A static sticker is saved as a PNG by default"_q);
		check(
			(ExportTypeFor(Format::Static, u"webp"_q) == ExportType::Webp)
				&& (ExportTypeFor(Format::Static, u".WebP"_q)
					== ExportType::Webp)
				&& (ExportTypeFor(Format::Static, u"cat.final.WEBP "_q)
					== ExportType::Webp),
			u"A static sticker is saved as a WebP by the extension"_q);
		check(
			(ExportTypeFor(Format::Animated) == ExportType::Tgs)
				&& (ExportTypeFor(Format::Animated, u"webp"_q)
					== ExportType::Tgs)
				&& (ExportTypeFor(Format::Video) == ExportType::Webm)
				&& (ExportTypeFor(Format::Video, u"mp4"_q)
					== ExportType::Webm),
			u"Animated and video stickers have one file type"_q);

		check(
			(ExportTypeForSending(Format::Static, 1) == ExportType::Png)
				&& (ExportTypeForSending(Format::Static, kStaticMaxBytes)
					== ExportType::Png),
			u"A PNG that fits the limit is sent as it is"_q);
		check(
			(ExportTypeForSending(Format::Static, kStaticMaxBytes + 1)
				== ExportType::Webp)
				&& (ExportTypeForSending(Format::Static, 0)
					== ExportType::Webp),
			u"A PNG over the limit is replaced with the WebP"_q);
		check(
			(ExportTypeForSending(Format::Animated, 0) == ExportType::Tgs)
				&& (ExportTypeForSending(Format::Video, 1 << 30)
					== ExportType::Webm),
			u"ExportTypeForSending of animated and video stickers"_q);
		check(
			ExportSendsForcedFile(ExportType::Webp)
				&& ExportSendsForcedFile(ExportType::Webm)
				&& !ExportSendsForcedFile(ExportType::Png)
				&& !ExportSendsForcedFile(ExportType::Tgs),
			u"ExportSendsForcedFile"_q);

		check(
			ExportBaseName(QString()) == u"sticker"_q,
			u"ExportBaseName of nothing"_q);
		check(
			ExportBaseName(u"  Мой кот 1  "_q) == u"Мой кот 1"_q,
			u"ExportBaseName keeps a good name: "_q
				+ ExportBaseName(u"  Мой кот 1  "_q));
		check(
			ExportBaseName(u"a/b\\c:d*e?f\"g<h>i|j\nk"_q)
				== u"a_b_c_d_e_f_g_h_i_j_k"_q,
			u"ExportBaseName replaces what can't be in a name: "_q
				+ ExportBaseName(u"a/b\\c:d*e?f\"g<h>i|j\nk"_q));
		check(
			ExportBaseName(u"..hidden. ."_q) == u"hidden"_q,
			u"ExportBaseName drops the dots around: "_q
				+ ExportBaseName(u"..hidden. ."_q));
		check(
			ExportBaseName(u" . . "_q) == u"sticker"_q,
			u"ExportBaseName of dots and spaces"_q);
		check(
			ExportBaseName(u"clip.v2"_q) == u"clip.v2"_q,
			u"ExportBaseName keeps a dot inside"_q);
		const auto longName = ExportBaseName(QString(200, QChar(u'я')));
		check(
			longName == QString(64, QChar(u'я')),
			u"ExportBaseName of a long name: %1 symbols"_q.arg(
				longName.size()));
		const auto pair = QString::fromUtf8("\xF0\x9F\x98\x8E");
		const auto cut = ExportBaseName(QString(63, QChar('a')) + pair);
		check(
			cut == QString(63, QChar('a')),
			u"ExportBaseName doesn't cut a symbol in two: %1 symbols"_q.arg(
				cut.size()));

		check(
			ExportFileName(u"cat"_q, ExportType::Webm) == u"cat.webm"_q,
			u"ExportFileName: "_q
				+ ExportFileName(u"cat"_q, ExportType::Webm));
		check(
			ExportFileName(QString(), ExportType::Png) == u"sticker.png"_q,
			u"ExportFileName without a hint"_q);
		check(
			ExportFileName(u"../../etc/passwd"_q, ExportType::Tgs)
				== u"_.._etc_passwd.tgs"_q,
			u"ExportFileName can't leave the folder: "_q
				+ ExportFileName(u"../../etc/passwd"_q, ExportType::Tgs));

		check(
			ExportPath(u"/tmp/a"_q, ExportType::Png) == u"/tmp/a.png"_q,
			u"ExportPath adds the extension"_q);
		check(
			ExportPath(u"/tmp/a.PNG"_q, ExportType::Png) == u"/tmp/a.PNG"_q,
			u"ExportPath keeps the extension in any case"_q);
		check(
			ExportPath(u"/tmp/a.webp"_q, ExportType::Webp)
				== u"/tmp/a.webp"_q,
			u"ExportPath keeps .webp"_q);
		check(
			ExportPath(u"/tmp/a.mp4"_q, ExportType::Webm)
				== u"/tmp/a.mp4.webm"_q,
			u"ExportPath doesn't keep a wrong extension"_q);
		check(
			ExportPath(u"/tmp/dir.webm/a"_q, ExportType::Webm)
				== u"/tmp/dir.webm/a.webm"_q,
			u"ExportPath looks at the name, not at the folder"_q);
		check(
			ExportPath(u"C:\\x.tgs\\.tgs"_q, ExportType::Tgs)
				== u"C:\\x.tgs\\.tgs.tgs"_q,
			u"ExportPath of a name that is only the extension"_q);
		check(
			ExportPath(QString(), ExportType::Png).isEmpty(),
			u"ExportPath of nothing"_q);
	}

	// The sticker as a file: PNG keeps the size and the transparency.
	{
		const auto isPng = [](const QByteArray &bytes) {
			return bytes.startsWith(QByteArray::fromHex("89504e470d0a1a0a"));
		};
		const auto composed = ComposeSticker(disc, { .outline = true });
		const auto png = EncodePng(composed);
		check(isPng(png), u"EncodePng writes PNG: %1 bytes"_q.arg(png.size()));
		const auto decoded = QImage::fromData(png, "png");
		check(
			(decoded.size() == QSize(512, 512))
				&& (decoded.size() == composed.size()),
			u"The PNG has the size of the sticker: "_q
				+ sizeText(decoded.size()));
		check(
			decoded.hasAlphaChannel()
				&& (qAlpha(decoded.pixel(2, 2)) == 0)
				&& (qAlpha(decoded.pixel(256, 256)) == 255)
				&& (decoded.pixel(256, 3) == 0xFFFFFFFFU),
			u"The PNG keeps the transparency and the outline"_q);
		check(
			decoded.convertToFormat(QImage::Format_ARGB32)
				== composed.convertToFormat(QImage::Format_ARGB32),
			u"The PNG is lossless"_q);

		auto landscape = QImage(1200, 800, QImage::Format_RGB32);
		landscape.fill(QColor(40, 120, 200));
		const auto wide = QImage::fromData(
			EncodePng(ComposeSticker(landscape)),
			"png");
		const auto color = wide.isNull() ? QRgb(0) : wide.pixel(10, 10);
		check(
			(wide.size() == QSize(512, 341))
				&& (qAlpha(color) == 255)
				&& (std::abs(qRed(color) - 40) <= 2)
				&& (std::abs(qGreen(color) - 120) <= 2)
				&& (std::abs(qBlue(color) - 200) <= 2),
			u"The PNG of a 1200x800 photo: %1, %2"_q.arg(
				sizeText(wide.size())
			).arg(color, 8, 16));
		auto portrait = QImage(100, 300, QImage::Format_ARGB32);
		portrait.fill(QColor(200, 40, 40, 255));
		const auto tall = QImage::fromData(
			EncodePng(ComposeSticker(portrait)),
			"png");
		check(
			(tall.size() == QSize(170, 512)),
			u"The PNG of a small image is 512 px tall: "_q
				+ sizeText(tall.size()));
		check(EncodePng(QImage()).isEmpty(), u"EncodePng of null"_q);

		if (WebpSupported()) {
			const auto prepared = EncodeStatic(composed);
			const auto asPng = MakeExportFile(
				prepared,
				composed,
				ExportType::Png,
				u"disc"_q);
			check(
				asPng.valid()
					&& (asPng.type == ExportType::Png)
					&& (asPng.name == u"disc.png"_q)
					&& (asPng.mime == u"image/png"_q)
					&& (asPng.size == QSize(512, 512))
					&& (asPng.bytes == png),
				u"MakeExportFile of a PNG: "_q + asPng.name);
			const auto asWebp = MakeExportFile(
				prepared,
				composed,
				ExportType::Webp,
				QString());
			check(
				asWebp.valid()
					&& (asWebp.name == u"sticker.webp"_q)
					&& (asWebp.mime == u"image/webp"_q)
					&& (asWebp.bytes == prepared.bytes),
				u"MakeExportFile of a WebP is the uploaded file"_q);
			check(
				!MakeExportFile(
					prepared,
					composed,
					ExportType::Webm,
					QString()).valid(),
				u"MakeExportFile refuses a wrong type"_q);
			check(
				!MakeExportFile(
					prepared,
					QImage(),
					ExportType::Png,
					QString()).valid(),
				u"MakeExportFile of a PNG needs the image"_q);
			check(
				!MakeExportFile(
					Prepared(),
					composed,
					ExportType::Png,
					QString()).valid(),
				u"MakeExportFile of nothing"_q);

			const auto sent = MakeSendFile(prepared, composed, u"disc"_q);
			check(
				sent.valid()
					&& (sent.type == ExportType::Png)
					&& (sent.bytes.size() <= kStaticMaxBytes),
				u"A small sticker is sent as a PNG: %1 bytes"_q.arg(
					sent.bytes.size()));

			// Noise doesn't fit 512 KB as a PNG, but always does as a WebP.
			auto noise = QImage(512, 512, QImage::Format_ARGB32);
			auto seed = uint32(0x2545F491U);
			for (auto y = 0; y != noise.height(); ++y) {
				const auto line = reinterpret_cast<QRgb*>(noise.scanLine(y));
				for (auto x = 0; x != noise.width(); ++x) {
					seed = seed * 1664525U + 1013904223U;
					line[x] = (seed | 0xFF000000U);
				}
			}
			const auto heavyImage = ComposeSticker(noise);
			const auto heavy = EncodeStatic(heavyImage);
			const auto heavyPng = EncodePng(heavyImage);
			const auto heavySent = MakeSendFile(heavy, heavyImage, u"n"_q);
			check(
				heavyPng.size() > kStaticMaxBytes,
				u"The PNG of noise is over the limit: %1 bytes"_q.arg(
					heavyPng.size()));
			check(
				heavySent.valid()
					&& (heavySent.type == ExportType::Webp)
					&& (heavySent.name == u"n.webp"_q)
					&& (heavySent.bytes == heavy.bytes)
					&& (heavySent.bytes.size() <= kStaticMaxBytes),
				u"A heavy sticker is sent as the WebP: %1 bytes"_q.arg(
					heavySent.bytes.size()));
		}

		const auto animated = PrepareAnimated(
			CheckLottie(LottieEdit::Document::Blank()));
		const auto tgs = MakeSendFile(animated, QImage(), u"blank"_q);
		check(
			tgs.valid()
				&& (tgs.type == ExportType::Tgs)
				&& (tgs.name == u"blank.tgs"_q)
				&& (tgs.mime == u"application/x-tgsticker"_q)
				&& (tgs.bytes == animated.bytes),
			u"MakeSendFile of an animated sticker: "_q + tgs.name);
		check(
			!MakeExportFile(
				animated,
				QImage(),
				ExportType::Png,
				QString()).valid(),
			u"An animated sticker is not saved as a PNG"_q);

		const auto video = Prepared{
			.format = Format::Video,
			.bytes = QByteArray("webm"),
			.size = QSize(512, 384),
			.duration = 1000,
		};
		const auto webm = MakeSendFile(video, QImage(), u"clip.mp4"_q);
		check(
			webm.valid()
				&& (webm.type == ExportType::Webm)
				&& (webm.name == u"clip.mp4.webm"_q)
				&& (webm.mime == u"video/webm"_q)
				&& (webm.bytes == video.bytes)
				&& (webm.size == QSize(512, 384)),
			u"MakeSendFile of a video sticker: "_q + webm.name);
	}

	// Short names.
	{
		check(
			CheckShortName(u"my_pack_2026"_q) == NameProblem::None,
			u"A good short name"_q);
		check(
			CheckShortName(QString()) == NameProblem::Empty,
			u"An empty short name"_q);
		check(
			CheckShortName(u"1pack"_q) == NameProblem::Start,
			u"A short name can't start with a digit"_q);
		check(
			CheckShortName(u"_pack"_q) == NameProblem::Start,
			u"A short name can't start with an underscore"_q);
		check(
			CheckShortName(u"мой_набор"_q) == NameProblem::Symbols,
			u"A short name can't have cyrillic letters"_q);
		check(
			CheckShortName(u"my pack"_q) == NameProblem::Symbols,
			u"A short name can't have spaces"_q);
		check(
			CheckShortName(u"my__pack"_q) == NameProblem::Underscores,
			u"A short name can't have two underscores"_q);
		check(
			CheckShortName(u"pack_"_q) == NameProblem::Underscores,
			u"A short name can't end with an underscore"_q);
		check(
			CheckShortName(QString(65, QChar('a'))) == NameProblem::TooLong,
			u"A short name can't be longer than 64"_q);
		check(
			CheckShortName(QString(64, QChar('a'))) == NameProblem::None,
			u"A short name can be 64 symbols long"_q);

		const auto name = ShortNameFromTitle(u"  Мои котики №1! "_q);
		check(
			(name == u"moi_kotiki_1"_q),
			u"ShortNameFromTitle: "_q + name);
		check(
			CheckShortName(name) == NameProblem::None,
			u"ShortNameFromTitle gives a valid name"_q);
		check(
			ShortNameFromTitle(u"123 !!!"_q).isEmpty(),
			u"ShortNameFromTitle of digits only"_q);
		check(
			ShortNameFromTitle(u"Щи & Борщ"_q) == u"schi_borsch"_q,
			u"ShortNameFromTitle: "_q + ShortNameFromTitle(u"Щи & Борщ"_q));
		const auto longName = ShortNameFromTitle(QString(200, QChar(u'я')));
		check(
			(longName.size() <= kShortNameMaxLength)
				&& (CheckShortName(longName) == NameProblem::None),
			u"ShortNameFromTitle of a long title"_q);
		check(
			PackLink(u"cats"_q) == u"https://t.me/addstickers/cats"_q,
			u"PackLink"_q);
		check(
			PackLink(u"cats"_q, true) == u"https://t.me/addemoji/cats"_q,
			u"PackLink of emoji"_q);
	}

	// Animated stickers.
	{
		check(
			!CheckLottie(QByteArray("not a lottie")).parsed(),
			u"CheckLottie of garbage"_q);
		check(
			!PrepareAnimated(CheckLottie(QByteArray())).valid(),
			u"PrepareAnimated of nothing"_q);

		const auto blank = CheckLottie(LottieEdit::Document::Blank());
		check(
			blank.parsed() && blank.acceptable(),
			u"A blank 512x512 composition is acceptable"_q);
		const auto prepared = PrepareAnimated(blank);
		check(
			prepared.valid()
				&& (prepared.format == Format::Animated)
				&& (prepared.size == QSize(512, 512))
				&& (prepared.duration == 3000),
			u"PrepareAnimated of a blank: %1 ms"_q.arg(prepared.duration));
		check(
			DetectFileKind(u"x"_q, prepared.bytes) == FileKind::Lottie,
			u"DetectFileKind of a .tgs"_q);
		check(
			DetectFileKind(u"x"_q, blank.document.toJson())
				== FileKind::Lottie,
			u"DetectFileKind of a Lottie JSON"_q);
		check(
			CheckLottie(prepared.bytes).acceptable(),
			u"The packed .tgs is read back"_q);

		// 256x256, 25 fps, 6 seconds: three errors, all fixable.
		const auto wrong = CheckLottie(
			LottieEdit::Document::Blank(QSize(256, 256), 25., 150));
		check(
			wrong.parsed() && !wrong.acceptable() && wrong.fixable(),
			u"A 256x256 25 fps 6 s composition is rejected"_q);
		check(
			!PrepareAnimated(wrong).valid(),
			u"PrepareAnimated refuses it"_q);
		const auto fixed = FixLottie(wrong);
		check(
			fixed.acceptable()
				&& (fixed.document.size() == QSize(512, 512)),
			u"FixLottie makes it acceptable (%1 issues left)"_q.arg(
				fixed.validation.issues.size()));
		const auto fixedPrepared = PrepareAnimated(fixed);
		check(
			fixedPrepared.valid() && (fixedPrepared.duration <= 3000),
			u"The fixed one is at most 3 s: %1 ms"_q.arg(
				fixedPrepared.duration));

		auto file = QFile(u":/animations/cake.tgs"_q);
		if (file.open(QIODevice::ReadOnly)) {
			const auto cake = CheckLottie(file.readAll());
			check(cake.parsed(), u"cake.tgs is parsed"_q);
			log.push_back(u"cake.tgs: %1 issues, %2, %3 bytes packed"_q.arg(
				cake.validation.issues.size()
			).arg(cake.acceptable()
				? u"acceptable"_q
				: u"not acceptable"_q
			).arg(cake.tgs.size()));
		} else {
			log.push_back(u"cake.tgs is not in the resources, skipped."_q);
		}
	}

	// Video stickers.
	{
		check(
			CheckVideoSticker(QByteArray()).problem
				== VideoProblem::Unreadable,
			u"CheckVideoSticker of nothing"_q);
		check(
			CheckVideoSticker(QByteArray(4096, char(7))).problem
				== VideoProblem::Unreadable,
			u"CheckVideoSticker of garbage"_q);

		auto frames = std::vector<QImage>();
		for (auto i = 0; i != 20; ++i) {
			auto frame = QImage(320, 240, QImage::Format_ARGB32_Premultiplied);
			frame.fill(QColor(20 + i * 10, 90, 200 - i * 8));
			auto p = QPainter(&frame);
			p.setPen(Qt::NoPen);
			p.setBrush(Qt::white);
			p.drawEllipse(QPoint(40 + i * 12, 120), 30, 30);
			p.end();
			frames.push_back(std::move(frame));
		}
		const auto encoded = VideoCore::EncodeVideoSticker(frames, 50);
		if (check(
				encoded.ok && !encoded.webm.isEmpty(),
				u"EncodeVideoSticker: "_q + encoded.error)) {
			const auto video = CheckVideoSticker(encoded.webm);
			check(
				video.ok(),
				u"CheckVideoSticker of a made sticker: problem %1, %2, "
				"%3 ms, %4"_q.arg(
					int(video.problem)
				).arg(sizeText(video.info.size)).arg(
					video.info.duration
				).arg(video.info.codec));
			const auto prepared = PrepareVideo(encoded.webm, video);
			check(
				prepared.valid()
					&& (prepared.format == Format::Video)
					&& (prepared.size == QSize(512, 384))
					&& (prepared.duration > 0)
					&& (prepared.duration <= 3000),
				u"PrepareVideo: %1, %2 ms"_q.arg(
					sizeText(prepared.size)
				).arg(prepared.duration));
			check(
				DetectFileKind(u"x"_q, encoded.webm)
					== FileKind::VideoSticker,
				u"DetectFileKind of a video sticker"_q);

			// The same file with other numbers in its header: twice the
			// frame rate (DefaultDuration, 20 frames a second become 40)
			// and a millisecond over the three seconds (Duration).
			struct Value {
				int from = 0;
				int bytes = 0;
			};
			const auto &source = encoded.webm;
			const auto total = int(source.size());
			const auto valueAt = [&](const QByteArray &idWithSize) {
				const auto at = int(source.indexOf(idWithSize));
				const auto from = at + int(idWithSize.size());
				const auto bytes = (at >= 0)
					? int(uchar(idWithSize.back()) & 0x7F)
					: 0;
				return (bytes > 0 && bytes <= 8 && from + bytes <= total)
					? Value{ from, bytes }
					: Value();
			};
			const auto withValue = [&](Value at, uint64 value) {
				auto result = source;
				for (auto i = 0; i != at.bytes; ++i) {
					const auto shift = 8 * (at.bytes - 1 - i);
					result[at.from + i] = char((value >> shift) & 0xFF);
				}
				return result;
			};
			// 50 ms in nanoseconds take four bytes.
			const auto rateAt = valueAt(QByteArray::fromHex("23e38384"));
			if (check(rateAt.bytes > 0, u"DefaultDuration is found"_q)) {
				auto frame = uint64(0);
				for (auto i = 0; i != rateAt.bytes; ++i) {
					frame = (frame << 8) | uchar(source[rateAt.from + i]);
				}
				const auto fast = withValue(rateAt, frame / 2);
				const auto checked = CheckVideoSticker(fast);
				check(
					checked.problem == VideoProblem::FrameRate,
					u"A 40 fps WebM is too fast: problem %1, %2 fps"_q.arg(
						int(checked.problem)
					).arg(checked.info.fps));
				check(
					!PrepareVideo(fast, checked).valid(),
					u"PrepareVideo refuses a 40 fps WebM"_q);
				check(
					DetectFileKind(u"x.webm"_q, fast) == FileKind::Video,
					u"DetectFileKind of a 40 fps WebM"_q);
			}
			const auto durationAt = valueAt(QByteArray::fromHex("448988"));
			if (check(durationAt.bytes > 0, u"Duration is found"_q)) {
				const auto length = float64(
					VideoCore::kStickerMaxDuration + 1);
				auto bits = uint64(0);
				static_assert(sizeof(bits) == sizeof(length));
				memcpy(&bits, &length, sizeof(bits));
				const auto longer = withValue(durationAt, bits);
				const auto checked = CheckVideoSticker(longer);
				check(
					checked.problem == VideoProblem::Duration,
					u"A 3.001 s WebM is too long: problem %1, %2 ms"_q.arg(
						int(checked.problem)
					).arg(checked.info.duration));
				check(
					DetectFileKind(u"x.webm"_q, longer) == FileKind::Video,
					u"DetectFileKind of a 3.001 s WebM"_q);
			}

			// The longest and the fastest sticker the converter makes.
			auto full = std::vector<QImage>();
			for (auto i = 0; i != 90; ++i) {
				full.push_back(frames[i % frames.size()]);
			}
			const auto longest = VideoCore::EncodeVideoSticker(full, 33);
			if (check(
					longest.ok && !longest.webm.isEmpty(),
					u"EncodeVideoSticker of 3 s: "_q + longest.error)) {
				const auto checked = CheckVideoSticker(longest.webm);
				check(
					checked.ok(),
					u"CheckVideoSticker of a 3 s 30 fps sticker: problem "
					"%1, %2 ms, %3 fps"_q.arg(
						int(checked.problem)
					).arg(checked.info.duration).arg(checked.info.fps));
			}

			// A custom emoji size is not a sticker.
			const auto small = VideoCore::EncodeVideoSticker(
				frames,
				50,
				VideoCore::kEmojiSide);
			if (check(small.ok, u"EncodeVideoSticker 100px"_q)) {
				check(
					CheckVideoSticker(small.webm).problem
						== VideoProblem::Dimensions,
					u"A 100px WebM has wrong dimensions"_q);
				check(
					DetectFileKind(u"x.webm"_q, small.webm)
						== FileKind::Video,
					u"DetectFileKind of a WebM to convert"_q);
				check(
					!PrepareVideo(
						small.webm,
						CheckVideoSticker(small.webm)).valid(),
					u"PrepareVideo refuses it"_q);
			}
		}

		const auto gif = VideoCore::EncodeGif(frames, 50);
		if (check(!gif.isEmpty(), u"EncodeGif"_q)) {
			check(
				CheckVideoSticker(gif).problem == VideoProblem::Container,
				u"A GIF is not a WebM"_q);
			check(
				DetectFileKind(u"x"_q, gif) == FileKind::Video,
				u"DetectFileKind of an animated GIF"_q);
		}
		const auto still = VideoCore::EncodeGif({ frames.front() }, 50);
		if (check(!still.isEmpty(), u"EncodeGif of one frame"_q)) {
			check(
				DetectFileKind(u"x"_q, still) == FileKind::Image,
				u"DetectFileKind of a still GIF"_q);
		}
		check(
			DetectFileKind(u"clip.MP4"_q, QByteArray()) == FileKind::Video,
			u"DetectFileKind by the extension"_q);
		check(
			DetectFileKind(u"notes.txt"_q, QByteArray("hello"))
				== FileKind::Unknown,
			u"DetectFileKind of a text file"_q);
		auto mp4 = QByteArray(32, char(0));
		mp4.replace(4, 8, "ftypisom");
		check(
			DetectFileKind(u"x"_q, mp4) == FileKind::Video,
			u"DetectFileKind of an mp4 header"_q);
		auto heic = QByteArray(32, char(0));
		heic.replace(4, 8, "ftypheic");
		check(
			DetectFileKind(u"x"_q, heic) == FileKind::Image,
			u"DetectFileKind of a HEIC header"_q);
	}

	if (passed) {
		log.push_back(u"Sticker packs core: all checks passed."_q);
	}
	return passed;
}

} // namespace Oblivion::StickerPacks
