/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_msg_id.h"

class DocumentData;
class HistoryItem;
class PhotoData;

namespace Data {
class Thread;
} // namespace Data

namespace Ui {
class PopupMenu;
namespace Menu {
struct MenuCallback;
} // namespace Menu
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

// The UI of the on-device image analysis (oblivion_vision.h):
//
//  - "Copy text from photo": recognizes the text off the main thread and
//    shows it in a box (selectable, with a "Copy" button, "No text found"
//    if there is none);
//  - "Cut out object": removes the background and shows the cutout with
//    the actions: copy, save as PNG, make a sticker (oblivion_sticker_packs.h),
//    open in the photo editor. The cutout is cropped to the object unless
//    that is switched off in the box.
//
// Both are in the chat context menu of a photo or an image file (the full
// size image is downloaded first) and in the media viewer menu. Items are
// not added on the systems without the feature (the background removal
// needs macOS 14). The photo editor has its own "Remove background" on
// the Effects tab, see oblivion_photo_editor.h.
namespace Oblivion {

// With a ready image (from the clipboard, a file, etc.).
void ShowRecognizedText(
	not_null<Window::SessionController*> controller,
	QImage image);

// name is the suggested file name without an extension, thread (optional)
// is the chat for "Send to <chat>" in the photo editor opened from the box.
void ShowCutout(
	not_null<Window::SessionController*> controller,
	QImage image,
	QString name,
	Data::Thread *thread = nullptr);

// Chat context menu hooks. The caller checks the copy restrictions of
// the message (hasCopyMediaRestriction).
void AddVisionActions(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	not_null<PhotoData*> photo,
	HistoryItem *item);
void AddVisionActions(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	not_null<DocumentData*> document,
	HistoryItem *item);

// Media viewer menu hook, for a photo or an image file. resolveWindow
// finds the session window when an item is chosen, close hides the viewer
// before the box is shown there.
void AddMediaViewVisionActions(
	const Ui::Menu::MenuCallback &addAction,
	Fn<Window::SessionController*()> resolveWindow,
	PhotoData *photo,
	DocumentData *document,
	FullMsgId context,
	Fn<void()> close);

} // namespace Oblivion
