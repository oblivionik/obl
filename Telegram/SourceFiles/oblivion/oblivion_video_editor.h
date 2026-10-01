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
class PeerData;

namespace Api {
enum class SendType;
} // namespace Api

namespace ChatHelpers {
class Show;
} // namespace ChatHelpers

namespace Data {
class Thread;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace SendMenu {
struct Details;
} // namespace SendMenu

namespace Ui {
class PopupMenu;
struct PreparedList;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

// Video editor: a box with a preview, a timeline of clips with thumbnails
// (trimming by the edges of a clip, splitting at the playhead, removing
// and reordering clips, adding more videos), one crop (free or with fixed
// proportions), rotation and speed for the whole result, the sound kept
// or not. The result is exported off the main thread (oblivion_video_project.h
// over oblivion_video_core.h) as an MP4 video, a "Telegram GIF" (MP4
// without sound), a real GIF file, a video sticker for a sticker pack
// (oblivion_sticker_packs.h) or a custom emoji, and is saved to a file or
// sent to a chat through the usual send files box.
//
// The preview plays without sound. Shortcuts (the timeline has the focus):
// Space plays, Left / Right step by a frame (with Shift by a second),
// Home / End, S splits, Delete removes the clip, Cmd+Z / Cmd+Shift+Z.
//
// Cancel and Escape ask before the changes that were not exported are
// lost. When the editor is closed by something else (the passcode lock,
// a chat opened from a notification, a switch to another account) its
// project is kept in the memory and offered when the editor is opened the
// next time, and a running export goes on: the result is written to the
// file chosen for it or to the downloads folder. There is one editor at
// a time in the whole application (one project can be kept), opening the
// second one only shows a toast.
namespace Oblivion {

// Opens the editor for a local video (or GIF) file. thread (optional)
// is the chat the video came from, for "Send to <chat>".
void ShowVideoEditor(
	not_null<Window::SessionController*> controller,
	const QString &path,
	Data::Thread *thread = nullptr);

// The same for a video that is only in memory (a pasted one).
void ShowVideoEditorContent(
	not_null<Window::SessionController*> controller,
	const QByteArray &content,
	Data::Thread *thread = nullptr);

// Downloads the video first (into the temporary folder, as the round video
// editor does), the editor shows the progress.
void ShowVideoEditorForDocument(
	not_null<Window::SessionController*> controller,
	not_null<DocumentData*> document,
	FullMsgId context);

// Settings > Oblivion > Tools > "Video editor": asks for a file first.
void ShowVideoEditorImport(not_null<Window::SessionController*> controller);

// Hook for Core::Application::preventsQuit(), before the "hold to quit"
// check: asks before quitting while a result is being written to its file
// (it would be left incomplete), while a video is exported or there are
// changes that were not exported. The application never quits later by
// itself, the question about the file only closes when it is written.
[[nodiscard]] bool VideoEditorPreventsQuit();

// Hook for the start of a session: removes what a previous launch has left
// in the temporary folder of the account (videos downloaded to be edited,
// files written to be sent).
void CleanupVideoEditorTemp(not_null<Main::Session*> session);

// Hook for the context menu of a video, GIF or round video message:
// "Open in video editor". The caller checks the copy restrictions.
void AddVideoEditorAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item,
	not_null<DocumentData*> document);

// Hook for the "..." menu of SendFilesBox, next to "Send as round video":
// "Open in video editor" when the box holds exactly one video.
void AddSendFilesVideoEditorAction(
	not_null<Ui::PopupMenu*> menu,
	std::shared_ptr<ChatHelpers::Show> show,
	not_null<PeerData*> peer,
	const SendMenu::Details &details,
	Api::SendType sendType,
	const Ui::PreparedList &list,
	Fn<void()> closeBox);

} // namespace Oblivion
