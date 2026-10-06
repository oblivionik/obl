/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtGui/QImage>

class DocumentData;
class HistoryItem;
class PhotoData;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

// Own sticker packs made right in the client (stickers.createStickerSet /
// stickers.addStickerToSet, no @stickers bot): static stickers from images
// (a photo with the background removed, the photo editor), animated ones
// from Lottie (the Lottie editor, the sticker studio) and video stickers
// (oblivion_video_core.h, oblivion_sticker_trim.h).
//
// The packs of the account are listed by messages.getMyStickers. A pack is
// viewed, reordered, renamed and deleted in the usual sticker set box, it
// has all of that for the creator of the set.
//
// Many files at once, custom emoji sets included, are added by the batch
// box of oblivion_sticker_batch.h: the list of the packs opens it by the
// "Add several…" button, from the menu of a pack and for the files
// dropped onto the list.
namespace Oblivion {

namespace StickerPacks {
struct BatchBackend;
} // namespace StickerPacks

struct StickerSource {
	enum class Type : uchar {
		Image, // image: any size, becomes a 512px WebP static sticker.
		Lottie, // data: .tgs (gzipped) or Lottie JSON, an animated sticker.
		Video, // data: WebM VP9 within VideoCore::kSticker* limits.
	};

	Type type = Type::Image;
	QImage image; // Type::Image.
	QByteArray data; // Type::Lottie and Type::Video.
	QString emoji; // Suggested emoji, empty = let the user choose.
	QString name; // Hint for titles and file names, may be empty.

	// Type::Image: an object cut out of a photo, the white outline is
	// switched on from the start (it can be switched off in the box).
	bool cutout = false;

	[[nodiscard]] static StickerSource FromImage(QImage image) {
		return { .type = Type::Image, .image = std::move(image) };
	}
	[[nodiscard]] static StickerSource FromLottie(QByteArray tgsOrJson) {
		return { .type = Type::Lottie, .data = std::move(tgsOrJson) };
	}
	[[nodiscard]] static StickerSource FromVideo(QByteArray webm) {
		return { .type = Type::Video, .data = std::move(webm) };
	}

	[[nodiscard]] bool empty() const {
		return (type == Type::Image) ? image.isNull() : data.isEmpty();
	}
};

// Settings > Oblivion > Tools > "My sticker packs": the packs created
// by this account, creating a new one, adding stickers from files.
void ShowStickerPacks(not_null<Window::SessionController*> controller);

// "Add to sticker pack" from the other tools: the preview, the emoji,
// one of the own packs or a new one, then the upload.
//
// added is called once, on the main thread, after the sticker was really
// added to a set: never if the box is not shown or is cancelled, and not
// on an error. The box may be closed long after the caller is gone, so
// added must guard everything it uses.
void AddToStickerPack(
	not_null<Window::SessionController*> controller,
	StickerSource source,
	Fn<void()> added = nullptr);

// The same for the tools without a session (the Lottie editor window):
// uses the last active window with an account, without one only a toast
// in the window of the tool says that an account is needed.
void AddToStickerPack(StickerSource source, Fn<void()> added = nullptr);

// Settings > Oblivion > Tools > "Sticker converter": a file from the disk
// (an image, a .tgs or a Lottie JSON, a video or a GIF, which is trimmed
// first) becomes a sticker file in the same box. Besides adding it to
// a pack, its "File" button saves the file (.png or .webp, .tgs, .webm)
// or sends it to a chat as a document, the @Stickers bot takes only such.
void ShowStickerConverter(not_null<Window::SessionController*> controller);

// The session part of the batch box, for oblivion_sticker_batch.cpp only:
// the own sets of the account of the window (sticker and custom emoji
// ones), the box that asks for the name of a new set, the upload of one
// sticker at a time (BatchBackend in oblivion_sticker_packs_core.h). What
// it returns must not outlive the session: it is kept by a box of that
// window and by nothing else.
[[nodiscard]] StickerPacks::BatchBackend MakeStickerBatchBackend(
	not_null<Window::SessionController*> controller);

// Chat context menu hooks. "Make a sticker" for a photo or an image file
// (the full size image is downloaded first, the background can be removed
// in the box), "Make a video sticker" for a video, a GIF or a round video
// message (downloaded, then trimmed in oblivion_sticker_trim.h). The
// caller checks the copy restrictions of the message.
void AddStickerPackActions(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	not_null<PhotoData*> photo,
	HistoryItem *item);
void AddStickerPackActions(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	not_null<DocumentData*> document,
	HistoryItem *item);

} // namespace Oblivion
