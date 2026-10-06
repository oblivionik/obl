/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"
#include "data/data_msg_id.h"

class PeerData;

namespace Api {
enum class SendType;
} // namespace Api

namespace ChatHelpers {
class Show;
} // namespace ChatHelpers

namespace SendMenu {
struct Details;
} // namespace SendMenu

namespace Ui {
class RpWidget;
struct PreparedFile;
struct PreparedList;
} // namespace Ui

// Oblivion tools in the send files box: a visible row of buttons for the
// tools that fit the attached file. The row is shown while the box holds
// exactly one file (an album keeps the per-file "..." menu):
//
//  - a photo or an image file: the photo editor (the result replaces the
//    attachment), "make a sticker", (macOS 14+) "remove background" and
//    (macOS) "copy text";
//  - a video: the video editor and a round video message (both take the
//    video away and close the box, as their items of the "..." menu do,
//    and their tooltips say so), a video sticker, a GIF (the video is
//    converted here and replaces the attachment);
//  - an animated GIF file: the video editor, a video sticker;
//  - a music file: the music editor;
//  - a .tgs or a Lottie JSON file: the Lottie editor.
//
// The buttons that are not supported on this system are not shown. With
// Oblivion::Get().attachTools() off nothing is created and the box stays
// exactly as it is upstream.
namespace Oblivion {

struct AttachToolsArgs {
	std::shared_ptr<ChatHelpers::Show> show;

	// The chat the files are sent to, how they are sent and the reply
	// they are sent with: the same values the "..." menu of the box gives
	// to AddSendAsRoundAction() and AddSendFilesVideoEditorAction().
	not_null<PeerData*> peer;
	Api::SendType sendType = Api::SendType();
	Fn<SendMenu::Details()> sendMenuDetails;
	Fn<FullReplyTo()> replyTo;

	// The files of the box in the order they are shown in (the box applies
	// a dragged album order before it returns). The reference is used
	// right away and never kept. Called only by the row while it is alive,
	// and the row is a child of the box.
	Fn<const Ui::PreparedList&()> list;

	// Fires after the list has changed in any way (a file added, removed,
	// replaced or edited), the row then chooses its buttons again. The box
	// counts its size after it has fired this, not before: a row that is
	// hidden (by itself or with the box, under another layer) gets no
	// resize events, so heightValue() tells only about the changes the row
	// makes while it is on the screen.
	rpl::producer<> listChanges;

	// Calls apply for the file at this position of the list, if it is
	// still there, and refreshes the previews: how a tool puts its result
	// back into the box (as for AddAttachPhotoEditorAction()). Guarded by
	// the box: does nothing after the box is closed.
	Fn<void(int index, Fn<void(Ui::PreparedFile&)> apply)> replace;

	// Whether a file a tool has made may be sent to this chat the way the
	// box sends now (the rights for GIFs differ from the ones for videos).
	// Shows the reason itself when it may not. Called only by the row.
	Fn<bool(const Ui::PreparedFile &file)> canSend;

	// For the tools that take the file away and send the result by
	// themselves (a round video message, the video editor). Closes the
	// box. Where a message field takes the caption back (the boxes of a
	// chat), it gets the caption; any other box restores what was in the
	// field before it, as when it is cancelled. Guarded by the box as well.
	Fn<void()> closeBox;

	// Whether the box still reads a file that was added to it: the list is
	// about to change, the tools wait for it. Called only by the row.
	Fn<bool()> busy;
};

// The hook of SendFilesBox: called once while the box is prepared, box is
// the SendFilesBox itself (it also owns the originals the photo editor
// keeps for editing again). The box gives the row its width, places it
// under the previews and follows its height: the row hides itself (with
// a zero height) while no tool fits the list.
//
// Returns nullptr when Oblivion::Get().attachTools() is off. Designated
// initializers of the arguments must follow the declaration order (MSVC).
[[nodiscard]] object_ptr<Ui::RpWidget> CreateAttachToolsRow(
	not_null<QWidget*> box,
	AttachToolsArgs &&args);

namespace AttachTools {

// Whether the box holds exactly one file and no other is on its way to
// it: none waits in filesToProcess and none is being read right now (the
// box takes a file out of filesToProcess while it reads that file, so the
// box itself tells about "reading"). The tools that take the file away
// and close the box are offered only then, by the row and by the "..."
// menu of the box alike: a file that is still being read would be thrown
// away together with the box.
[[nodiscard]] bool HoldsOnlyFile(const Ui::PreparedList &list, bool reading);

// Self-checks for OBLIVION_SELFTEST=attach_tools, see oblivion_selftest.h:
// which tools a file gets, how the buttons are wrapped. Runs before
// Core::Application exists (no Core::App(), no session).
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace AttachTools
} // namespace Oblivion
