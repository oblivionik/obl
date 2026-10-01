/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "oblivion/oblivion_lottie_doc.h"
#include "oblivion/oblivion_video_core.h"

#include <QtGui/QImage>

// The session-free part of the own sticker packs (oblivion_sticker_packs.h):
// turning an image, a Lottie animation or a video into the file Telegram
// accepts as a sticker, and the rules of the pack short names.
//
//  - static: WebP, the longer side exactly 512, up to 512 KB;
//  - animated: .tgs (gzipped Lottie), 512x512, up to 3 s, up to 64 KB;
//  - video: WebM VP9 without audio, the longer side exactly 512, up to
//    3 s and 30 frames per second, up to 256 KB (made by
//    Oblivion::VideoCore::MakeVideoSticker).
//
// Everything is synchronous, keeps no state and is safe to call from any
// thread, the UI calls it through crl::async.
namespace Oblivion::StickerPacks {

inline constexpr auto kStickerSide = 512;
inline constexpr auto kStaticMaxBytes = 512 * 1024;
inline constexpr auto kMaxEmoji = 20; // For one sticker.
inline constexpr auto kMaxStickers = 120; // In one set.
inline constexpr auto kTitleMaxLength = 64;
inline constexpr auto kShortNameMaxLength = 64;
inline constexpr auto kDefaultOutline = 8; // Pixels of a 512px sticker.

enum class Format : uchar {
	Static,
	Animated,
	Video,
};

// What is uploaded.
struct Prepared {
	Format format = Format::Static;
	QByteArray bytes; // .webp / .tgs / .webm
	QSize size;
	crl::time duration = 0; // Animated and Video.

	[[nodiscard]] bool valid() const {
		return !bytes.isEmpty() && !size.isEmpty();
	}
};

[[nodiscard]] QString MimeType(Format format);
[[nodiscard]] QString FileName(Format format); // "sticker.webp"

struct StaticOptions {
	// A white outline around the object, as the stickers cut out of
	// photos have. The transparent margins are cropped first.
	bool outline = false;
	int outlineWidth = kDefaultOutline;
};

// The image as it is shown in the sticker: the longer side is exactly
// kStickerSide (small images are scaled up, Telegram requires the size),
// ARGB32_Premultiplied. Null for a null or an empty image.
[[nodiscard]] QImage ComposeSticker(
	const QImage &image,
	StaticOptions options = {});

// The bounds of everything that is not transparent, empty if nothing is.
[[nodiscard]] QRect OpaqueBounds(const QImage &image, int threshold = 8);

// True if the image has any pixels that are not fully opaque.
[[nodiscard]] bool HasTransparency(const QImage &image);

// WebP of an image from ComposeSticker(), the quality is lowered until
// the file fits kStaticMaxBytes. Invalid on errors.
[[nodiscard]] Prepared EncodeStatic(const QImage &composed);
[[nodiscard]] Prepared PrepareStatic(
	const QImage &image,
	StaticOptions options = {});
[[nodiscard]] bool WebpSupported();

// Animated stickers: the TGS validator of oblivion_lottie_doc.h.
struct LottieCheck {
	LottieEdit::Document document; // Invalid if the data wasn't parsed.
	LottieEdit::ValidationResult validation;
	QByteArray tgs; // The document packed, empty if it can't be.

	[[nodiscard]] bool parsed() const {
		return document.valid();
	}
	// Nothing Telegram would reject, warnings may remain.
	[[nodiscard]] bool acceptable() const {
		return parsed() && validation.ok() && !tgs.isEmpty();
	}
	[[nodiscard]] bool fixable() const {
		return parsed() && validation.hasFixable();
	}
};

[[nodiscard]] LottieCheck CheckLottie(const QByteArray &tgsOrJson);
[[nodiscard]] LottieCheck CheckLottie(LottieEdit::Document document);

// Applies every automatic fix and checks the result again. Returns the
// check as it was if nothing could be fixed.
[[nodiscard]] LottieCheck FixLottie(const LottieCheck &check);

// Invalid if the check is not acceptable().
[[nodiscard]] Prepared PrepareAnimated(const LottieCheck &check);

enum class VideoProblem : uchar {
	None,
	Unreadable, // Not a video the FFmpeg build here reads.
	Container, // Not WebM.
	Codec, // Not VP9.
	Dimensions, // The longer side is not 512.
	Duration, // Longer than 3 seconds.
	FileSize, // More than 256 KB.
	Audio, // Has an audio track.
	FrameRate, // More than 30 frames per second.
};

struct VideoCheck {
	VideoCore::ClipInfo info;
	VideoProblem problem = VideoProblem::Unreadable;

	[[nodiscard]] bool ok() const {
		return (problem == VideoProblem::None);
	}
};

// Is this file a ready video sticker? Anything else goes through
// VideoCore::MakeVideoSticker() first.
[[nodiscard]] VideoCheck CheckVideoSticker(const QByteArray &webm);

// Invalid if the check is not ok().
[[nodiscard]] Prepared PrepareVideo(
	const QByteArray &webm,
	const VideoCheck &check);

// Short names: the end of the t.me/addstickers/<name> link. Latin
// letters, digits and underscores, starts with a letter, no two
// underscores in a row and none at the end, 1..64 symbols. Whether the
// name is free is known only by the server.
enum class NameProblem : uchar {
	None,
	Empty,
	TooLong,
	Start, // Doesn't start with a letter.
	Symbols, // Something that is not a latin letter, a digit or '_'.
	Underscores, // "__" inside or '_' at the end.
};

[[nodiscard]] NameProblem CheckShortName(const QString &name);

// A name from the title, for the time the server gives no suggestion:
// transliterated, lower case, may still be taken. Empty if nothing of
// the title can be used.
[[nodiscard]] QString ShortNameFromTitle(const QString &title);

// "https://t.me/addstickers/<name>" ("addemoji" for the emoji sets).
[[nodiscard]] QString PackLink(const QString &shortName, bool emoji = false);

// What kind of a sticker a file chosen by the user can become.
enum class FileKind : uchar {
	Unknown,
	Image, // Static sticker from an image.
	Lottie, // .tgs or Lottie JSON.
	Video, // Any video or an animated GIF: trimmed and converted.
	VideoSticker, // A WebM that passes CheckVideoSticker() as is.
};

// By the content first (gzip and JSON, the image and video headers), by
// the extension of the name when the content says nothing.
[[nodiscard]] FileKind DetectFileKind(
	const QString &name,
	const QByteArray &content);

// The ready sticker as a file for the user: saved to the disk or sent to
// a chat as a document. The @Stickers bot takes only files, a sticker
// message is not accepted by it.
enum class ExportType : uchar {
	Png, // Static, lossless, with the transparency.
	Webp, // Static, the very file that is uploaded to a pack.
	Tgs, // Animated.
	Webm, // Video.
};

struct ExportFile {
	ExportType type = ExportType::Png;
	QString name; // With the extension, "cat.webm".
	QString mime;
	QByteArray bytes;
	QSize size; // In pixels.

	[[nodiscard]] bool valid() const {
		return !name.isEmpty() && !bytes.isEmpty();
	}
};

[[nodiscard]] QString ExportExtension(ExportType type); // "png"
[[nodiscard]] QString ExportMimeType(ExportType type);

// By the extension of the name chosen in the save dialog (with or without
// the dot, in any case): a static sticker is a PNG unless "webp" is
// chosen, the other formats have a single type each.
[[nodiscard]] ExportType ExportTypeFor(
	Format format,
	const QString &extension = QString());

// What goes to a chat: a static sticker as a PNG while that fits the
// kStaticMaxBytes the @Stickers bot takes, the WebP otherwise. A sent PNG
// is always a plain file, while a WebP of this size is what Telegram
// apps show as a sticker.
[[nodiscard]] ExportType ExportTypeForSending(Format format, qint64 pngBytes);

// The message has to stay a file. A WebP and a WebM of the sticker size
// are what Telegram turns into a sticker or a video, they are uploaded
// with the "force file" flag (as the app does for a video sent without
// compression) and without the sticker and video attributes. A PNG is a
// file for everyone as it is. A .tgs can't be anything but an animated
// sticker for any Telegram app, by its type alone: it is sent exactly as
// the app sends every .tgs file, that is how they get to the bot.
[[nodiscard]] bool ExportSendsForcedFile(ExportType type);

// A file name without the extension made of the hint: nothing that can't
// be in a file name, up to 64 symbols, "sticker" if nothing is left.
[[nodiscard]] QString ExportBaseName(const QString &hint);
[[nodiscard]] QString ExportFileName(const QString &hint, ExportType type);

// The path chosen in the save dialog, with the extension of the type
// added if the name has another one or none.
[[nodiscard]] QString ExportPath(const QString &chosen, ExportType type);

// PNG (RGBA) of an image from ComposeSticker(): the same pixels and the
// same size, the longer side is kStickerSide. Empty on errors.
[[nodiscard]] QByteArray EncodePng(const QImage &composed);

// composed is used only for ExportType::Png. Invalid if the sticker is
// not valid() or the type is not one of its format.
[[nodiscard]] ExportFile MakeExportFile(
	const Prepared &prepared,
	const QImage &composed,
	ExportType type,
	const QString &hint);

// The file of ExportTypeForSending().
[[nodiscard]] ExportFile MakeSendFile(
	const Prepared &prepared,
	const QImage &composed,
	const QString &hint);

// Self-checks for OBLIVION_SELFTEST=sticker_packs, see oblivion_selftest.h.
// Runs before Core::Application exists (no Core::App(), no session).
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::StickerPacks
