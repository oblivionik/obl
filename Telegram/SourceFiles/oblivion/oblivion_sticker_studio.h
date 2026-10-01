/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_file_origin.h"

class DocumentData;

namespace ChatHelpers {
class Show;
} // namespace ChatHelpers

namespace Data {
struct UniqueGift;
} // namespace Data

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Oblivion {

// Opens the sticker studio (animated preview, frame slider, HSL recolor
// with live preview, .tgs / .json / .png / .svg export) for the given
// .tgs or Lottie JSON bytes. name is the base file name used
// for saving, without an extension (may be empty).
void ShowStickerStudio(
	not_null<Window::SessionController*> controller,
	QByteArray data,
	QString name);

// Same for a document (animated sticker, gift model or pattern, a .tgs
// file), downloading it first if needed. Shows a toast for documents
// that are not Lottie animations (video and static stickers).
void ShowStickerStudioFor(
	not_null<Window::SessionController*> controller,
	not_null<DocumentData*> document,
	Data::FileOrigin origin,
	const QString &name = QString());

// Asks for a .tgs / .json file and opens it in the sticker studio.
void ShowStickerStudioImport(
	not_null<Window::SessionController*> controller);

// "Model in sticker editor" / "Symbol in sticker editor" for a unique gift.
void AddGiftStudioActions(
	not_null<Ui::PopupMenu*> menu,
	std::shared_ptr<ChatHelpers::Show> show,
	std::shared_ptr<Data::UniqueGift> unique);

} // namespace Oblivion
