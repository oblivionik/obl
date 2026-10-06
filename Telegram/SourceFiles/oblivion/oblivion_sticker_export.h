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

namespace style {
struct ComposeIcons;
} // namespace style

namespace Ui {
class PopupMenu;
} // namespace Ui

// Saving stickers and custom emoji to files.
//
// One sticker: the "Save as" submenu with its natural formats (static:
// PNG / WebP, animated: TGS / Lottie JSON / GIF, video: WebM / GIF), then
// the usual save dialog. The file is downloaded first if needed, the GIF
// is rendered off the main thread, the result is reported by a toast
// with the path.
//
// A whole set: "Save the whole set (.zip)", all the files in their
// original format with readable names (index + emoji) and a manifest.json
// (title, short name, the emoji of every file). One request for the set,
// then the files are downloaded strictly one by one with a pause between
// them, in a box with the progress (closing it is the cancel), the
// archive is built and written off the main thread.
//
// Everything here works on the main thread and may be called for any
// account: the session is taken from show.
namespace Oblivion {

// Context menu filler for one sticker or custom emoji document: the
// sticker / emoji panel of a chat and the sticker set box. Adds "Save
// as" and, when the document belongs to a set, "Save the whole set
// (.zip)". Adds nothing for a document that is not a sticker.
//
// origin is used to refresh the file reference, an empty one means
// document->stickerSetOrigin().
//
// icons is the icons set of the menu the way FillStickerSetContextMenu()
// gets it. It is not used: a panel keeps its icons inside its style by
// value, so their address tells nothing. The items get the usual menu
// icons if the menu has the colors of the usual menu, in the dark menus
// (stories, calls) they are added without icons.
void AddStickerExportActions(
	not_null<Ui::PopupMenu*> menu,
	std::shared_ptr<ChatHelpers::Show> show,
	not_null<DocumentData*> document,
	Data::FileOrigin origin = Data::FileOrigin(),
	const style::ComposeIcons *icons = nullptr);

// "Save the whole set (.zip)" for a set: shows the progress box, loads
// the set (so the archive can be named after it), asks where to save,
// then downloads the files and writes the archive. Closing the box
// cancels everything and leaves nothing new on the disk. The only step
// that can't be stopped is putting the finished archive in its place: a
// cancel that comes right then removes the archive again, unless it has
// replaced a file that was there before (the user agreed to replace it
// in the save dialog, the old one can't be brought back). A box closed
// by anything but its "Cancel" button or the cancelled save dialog
// (Escape, another box shown in its place, a locked window) is reported
// by a toast. set needs the id with the access hash or the short name:
// regular, mask and custom emoji sets, installed or not. One export at a
// time in the whole application, starting the second one only shows a
// toast.
//
// Callable from the sticker set box, the panel and "My sticker packs".
void ExportStickerSet(
	std::shared_ptr<ChatHelpers::Show> show,
	StickerSetIdentifier set);

// The same as a menu item (the text and the icon come from here, so the
// callers need no lang keys of their own): the "..." menu of the sticker
// set box, the menu of a set in the panel, a row of "My sticker packs".
// Adds nothing for an empty set identifier. icons is as above.
void AddStickerSetExportAction(
	not_null<Ui::PopupMenu*> menu,
	std::shared_ptr<ChatHelpers::Show> show,
	StickerSetIdentifier set,
	const style::ComposeIcons *icons = nullptr);

namespace StickerExport {

// True while this module waits for the file of the document, loaded into
// memory to be saved: a failed load is reported here (the sticker is
// skipped in the archive of a set, a toast for one sticker). For
// DocumentData::handleLoaderUpdates(): its "download failed, try again?"
// box for a load that failed part-way would be shown in place of the
// export box, which cancels the whole export, and "try again" would only
// load that one sticker into memory. Main thread.
[[nodiscard]] bool QuietLoad(not_null<DocumentData*> document);

// Self-checks for OBLIVION_SELFTEST=sticker_export, see
// oblivion_selftest.h. No Core::App(), no session: file names, the
// manifest, the archive written and read back, the converters.
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace StickerExport
} // namespace Oblivion
