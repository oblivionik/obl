/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class QMimeData;

namespace Window {
class SessionController;
} // namespace Window

// Fast creation of stickers and custom emoji in batches: many files are
// chosen (or dropped) into a queue with a thumbnail, the type and the
// emoji of every item (a default emoji for all and a quick edit of one),
// converted automatically (images fit 512 / 100 px, videos: the first
// three seconds or the trim box of oblivion_sticker_trim.h, .tgs / Lottie
// JSON as they are) and uploaded into one of the own sets or a new one,
// a regular sticker set or a custom emoji set (oblivion_sticker_packs.h,
// oblivion_sticker_packs_core.h).
//
// The upload shows the progress, can be paused and cancelled, the failed
// items can be retried. Never more than one request is in flight, the
// pace is gentle and FLOOD_WAIT is waited out in full.
//
// The upload lives in its box. When the app closes the box in the middle
// of a run (the passcode lock, a chat opened from a notification, another
// account shown) the request in flight is cancelled, but the queue is kept
// while the app runs: the next box of the account shows which files have
// got to the set and goes on with the rest, nothing is uploaded twice.
namespace Oblivion {

// Settings > Oblivion > Tools > "Batch sticker creation", the list of the
// own sticker packs and the other tools that have several files for
// stickers at once. paths (optional) are local files and folders put into
// the queue right away: images, videos, GIFs, .tgs and Lottie JSON files,
// what is not one of those is skipped with a toast. With an empty list
// the box opens empty and asks for files.
//
// packId (optional) is the own set of the account, a sticker or a custom
// emoji one, that is chosen in the box from the start. Otherwise it is the
// set used the last time, and the user can choose any or make a new one.
//
// The queue of a run that was interrupted by the app comes back in a box
// opened without packId or for the set that run was filling. The paths
// are added to it, except the files that run has uploaded already. When
// several such runs wait, a box takes the latest one that fits, the
// others stay for the next boxes.
void ShowStickerBatch(
	not_null<Window::SessionController*> controller,
	QStringList paths = {},
	uint64 packId = 0);

namespace StickerBatch {

// For the widgets that take dropped files for the batch box. The first
// one is cheap enough for every drag move: local files with an extension
// the queue takes, or any folder. The second one also lists the folders
// (one level, not their subfolders) and sorts every folder by the names
// the way a file manager does ("2.png" before "10.png"). Main thread.
[[nodiscard]] bool MimeHasFiles(const QMimeData *data);
[[nodiscard]] QStringList PathsFromMime(const QMimeData *data);

// Self-checks for OBLIVION_SELFTEST=sticker_batch, see
// oblivion_selftest.h. No Core::App(), no session: pure logic only
// (the queue, the emoji of the items, the sizes, the pacing of the
// upload) and the conversion of files made in memory.
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace StickerBatch
} // namespace Oblivion
