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

namespace Oblivion {

// Sends any video as a round video message ("кружок"): an editor box
// with a round preview, the square crop position, trimming to at most
// 60 seconds and a mute toggle, then the video is converted the way
// Ui::RoundVideoRecorder encodes and sent like a recorded one.
void ShowVideoToRound(
	not_null<Window::SessionController*> controller,
	not_null<Data::Thread*> thread,
	const QString &path,
	FullReplyTo replyTo = FullReplyTo());
void ShowVideoContentToRound(
	not_null<Window::SessionController*> controller,
	not_null<Data::Thread*> thread,
	const QByteArray &content,
	FullReplyTo replyTo = FullReplyTo());

// Downloads the document first (as the download button in the chat does),
// the editor shows the progress.
void ShowDocumentToRound(
	not_null<Window::SessionController*> controller,
	not_null<Data::Thread*> thread,
	not_null<DocumentData*> document,
	FullMsgId context);

// Asks for a video file, then continues as ShowVideoToRound().
void ChooseVideoToRound(
	not_null<Window::SessionController*> controller,
	not_null<Data::Thread*> thread);

// Hook for the "..." menu of SendFilesBox: "Send as round video" when the
// box holds exactly one video and was opened from a chat's compose area.
void AddSendAsRoundAction(
	not_null<Ui::PopupMenu*> menu,
	std::shared_ptr<ChatHelpers::Show> show,
	not_null<PeerData*> peer,
	const SendMenu::Details &details,
	Api::SendType sendType,
	const Ui::PreparedList &list,
	FullReplyTo replyTo,
	Fn<void()> closeBox);

// Hook for the context menu of a video (or GIF) message in a chat:
// "Send as round video" to the same chat.
void AddVideoToRoundAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item,
	not_null<DocumentData*> document);

} // namespace Oblivion
