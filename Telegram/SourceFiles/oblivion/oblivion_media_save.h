/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class HistoryItem;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

// Saving voice messages as audio files and round video messages as MP4.
//
// A voice message is converted right away (oblivion_audio.h): M4A (AAC),
// WAV or OGG, chosen in the save dialog by the file type. The title tag
// is "<sender> <date>", the performer tag is the sender (the original
// ones for a forwarded message). OGG keeps the file as it was recorded,
// without one more lossy encoding and so without the tags. The FFmpeg
// build here has no MP3 encoder, so there is no MP3.
//
// A round video message already is an MP4 file (H.264 + AAC), it is
// saved as it is, only under a readable name.
//
// Whatever is not downloaded yet is downloaded first, the result is
// reported by a toast with the path.
namespace Oblivion {

// Chat context menu hook: "Save voice message as…" for a voice message,
// "Save video message as MP4" for a round one. Adds nothing for other
// items, for self-destructing media and for chats with protected
// content. The caller checks the copy restrictions of its list.
void AddMediaSaveActions(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item);

} // namespace Oblivion
