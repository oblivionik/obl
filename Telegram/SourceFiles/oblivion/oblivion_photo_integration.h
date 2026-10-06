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

namespace ChatHelpers {
class Show;
} // namespace ChatHelpers

namespace Data {
class Thread;
} // namespace Data

namespace Ui {
class PopupMenu;
class Show;
struct PreparedFile;
namespace Menu {
struct MenuCallback;
} // namespace Menu
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

// Where the Oblivion photo editor (oblivion_photo_editor.h, the editing UI
// over oblivion_photo_core.h) is opened from, and what happens with its
// result: saving to a file (PNG / JPEG / WebP), copying, sending to a chat
// or returning it into the send files box.
//
// After "Done" in the editor a result box offers: save as (format tabs,
// quality slider, the file size is estimated in the background), copy to
// the clipboard, send to the chat the photo came from, send to any chat
// (through the send files box, so a caption, "compress" (photo or file)
// and the spoiler work as usual) and continue editing (the editor opens
// again with the same original and edit state). The send actions are also
// in the "More" menu of the editor itself (it stays open under the boxes).
//
// Entry points:
//  - chat context menu of a photo or an image file (downloaded first);
//  - the "..." menu of an attached image in the send files box: the edited
//    image replaces the attachment (and can be edited again from the
//    untouched original while the box is open);
//  - the media viewer menu of a photo or an image file;
//  - Settings > Oblivion > Tools > Photo editor: open a file, paste from
//    the clipboard or drop an image.
//
// An editor (or the result box after it) that is closed not by the user,
// with all the layers of its window (the passcode lock, a switch to
// another account, a chat opened from a notification), keeps the edit
// with its layers in the memory. A toast tells about it (after the
// passcode is entered), and the next time the editor is opened from any
// of the entry points the user is asked whether to continue that edit or
// to start the new one. It is dropped when its account is logged out.
namespace Oblivion {

// Settings > Oblivion > Tools.
void ShowPhotoEditorImport(not_null<Window::SessionController*> controller);

// Settings > Oblivion > Tools > Photo collage: asks for several photos
// and opens the editor with a collage of them (the collage tool is
// chosen). "Done" shows the same result box as for a photo.
void ShowPhotoCollage(not_null<Window::SessionController*> controller);

// Opens a ready image (from a file, the clipboard, etc.).
// name is the suggested file name for "Save as" without an extension,
// thread (optional) adds "Send to <chat>" next to "Send to chat...".
void ShowPhotoEditorWithImage(
	not_null<Window::SessionController*> controller,
	QImage image,
	QString name,
	Data::Thread *thread = nullptr);

// Downloads the full size photo / the image file first (with progress).
void ShowPhotoEditorForPhoto(
	not_null<Window::SessionController*> controller,
	not_null<PhotoData*> photo,
	FullMsgId context);
void ShowPhotoEditorForDocument(
	not_null<Window::SessionController*> controller,
	not_null<DocumentData*> document,
	FullMsgId context);

// The loading part of the two functions above, for the other tools (text
// recognition, background removal): downloads the full size photo / the
// image file (a progress box with this title is shown while it takes
// time, an empty title is "Photo editor"), decodes it off the main thread
// (EXIF orientation, sRGB) and calls done on the main thread with the
// image and a file name suggestion without an extension. Failures are
// shown as toasts and done is not called, done must check what it uses.
void LoadPhotoImage(
	not_null<Window::SessionController*> controller,
	not_null<PhotoData*> photo,
	FullMsgId context,
	Fn<void(QImage image, QString name)> done,
	QString title = QString());
void LoadDocumentImage(
	not_null<Window::SessionController*> controller,
	not_null<DocumentData*> document,
	FullMsgId context,
	Fn<void(QImage image, QString name)> done,
	QString title = QString());

// A static image file (not a sticker, GIF or video) that fits the editor.
[[nodiscard]] bool PhotoEditorAcceptsDocument(
	not_null<DocumentData*> document);

// Chat context menu hooks: "Open in photo editor". The caller checks
// the copy restrictions of the message (hasCopyMediaRestriction).
void AddPhotoEditorAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	not_null<PhotoData*> photo,
	HistoryItem *item);
void AddPhotoEditorAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	not_null<DocumentData*> document,
	HistoryItem *item);

// Send files box hook, the per-file "..." / right click menu: adds
// "Oblivion photo editor" for a static image. box owns the originals kept
// for editing again, replace(apply) must call apply on the same file in
// the box list (if it is still there) and refresh the previews.
void AddAttachPhotoEditorAction(
	not_null<Ui::PopupMenu*> menu,
	std::shared_ptr<ChatHelpers::Show> show,
	not_null<QWidget*> box,
	const Ui::PreparedFile &file,
	Fn<void(Fn<void(Ui::PreparedFile&)> apply)> replace);
// The same without a menu, for a button: opens the editor right away.
// False (and nothing happens) if the file is not a static image.
bool OpenAttachPhotoEditor(
	std::shared_ptr<ChatHelpers::Show> show,
	not_null<QWidget*> box,
	const Ui::PreparedFile &file,
	Fn<void(Fn<void(Ui::PreparedFile&)> apply)> replace);

// Media viewer menu hook: "Edit in photo editor" for a photo or an image
// file. resolveWindow finds the session window when the item is chosen,
// close hides the viewer before the editor is shown there.
void AddMediaViewPhotoEditorAction(
	const Ui::Menu::MenuCallback &addAction,
	Fn<Window::SessionController*()> resolveWindow,
	PhotoData *photo,
	DocumentData *document,
	FullMsgId context,
	Fn<void()> close);

} // namespace Oblivion
