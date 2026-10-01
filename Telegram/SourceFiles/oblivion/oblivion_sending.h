/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class ApiWrap;
class HistoryItem;
class PeerData;
struct FilePrepareResult;

namespace Api {
struct SendOptions;
struct SendAction;
struct MessageToSend;
} // namespace Api

namespace Data {
class Thread;
} // namespace Data

namespace Oblivion {

// "Send without going online".
//
// A normal send becomes a scheduled one, a few seconds ahead. The server
// then delivers the message by itself and doesn't mark us online.
//
// Every send path calls AdjustSendAction() right after it has fired
// ApiWrap::sendAction(), so the chat UI still sees an ordinary send:
// it doesn't open the Scheduled section and doesn't show any toasts,
// and the chat is read on send as usual (see ReadOnSend() in
// oblivion_ghost.h for chats that are read invisibly).
// The message shows up in the chat when the server delivers it.
//
// text is the message text or caption, it keeps bot commands that expect
// an instant ephemeral answer out of it.
//
// count is how many messages the send makes (a forward of many messages,
// a message with a forward draft), the server keeps only a limited number
// of scheduled messages in a chat, a send that doesn't fit goes normally.
//
// Options that are offline already (see IsOfflineSchedule) stay offline,
// only a date that got too close is moved forward.
void AdjustSendAction(
	Api::SendAction &action,
	const QString &text = QString(),
	int count = 1);

// Files: same as above, and the date is copied into the prepared file
// and its album, so it survives until the upload finishes.
void AdjustConfirmedFile(
	Api::SendAction &action,
	const std::shared_ptr<FilePrepareResult> &file);

// Right before the MTP request with an uploaded file: after a long upload
// the offline date may be too close or already passed, move it forward.
// User-scheduled dates are never touched.
void RefreshSendOptions(
	Api::SendOptions &options,
	not_null<PeerData*> peer);

// The date was put into the options by AdjustSendAction(), so for the user
// it is a normal send, not a scheduled one (SendOptions::oblivionOffline).
// Such options travel further in some cases (forwards sent together with
// a message, uploads, copies of protected messages sent a bit later),
// ApiWrap::sendAction() shows them to the UI as not scheduled.
[[nodiscard]] bool IsOfflineSchedule(const Api::SendOptions &options);

// For code that builds MTP requests by itself (share box, stories share).
//
// Such a send may first send a comment (ApiWrap::sendMessage() takes
// the forward draft of the chat along), then its own request. Decides
// once for all of that, so that everything goes the same way and keeps
// its order: returns the options for the comment and for the request,
// offline ones (see IsOfflineSchedule) or the given ones as is.
// comment is the comment text, empty if there is none, count is how
// many messages the request itself sends.
// Such code reads the chat on send by itself, see ReadOnSendFor() in
// oblivion_ghost.h, a delivered message relies on that read.
[[nodiscard]] Api::SendOptions OfflineShareOptions(
	not_null<Data::Thread*> thread,
	const Api::SendOptions &options,
	const QString &comment,
	int count);

// Sends the comment with the options from OfflineShareOptions(), as
// ApiWrap::sendMessage() does, but doesn't decide for it once again:
// with the options that are not offline it goes as is, otherwise it
// could go offline alone and be delivered after the request.
void SendShareComment(
	not_null<ApiWrap*> api,
	Api::MessageToSend &&message);

// The schedule date for the request built with OfflineShareOptions(),
// or 0 to send as is. The date is moved forward if it got too close, so
// the request is never delivered before the comment sent right before.
[[nodiscard]] TimeId OfflineScheduleFor(
	not_null<Data::Thread*> thread,
	const Api::SendOptions &options);

// A message sent offline has just been delivered by the server: it is
// our own message, so it must not ping us with a notification.
[[nodiscard]] bool IsOfflineSentDelivery(not_null<const HistoryItem*> item);

} // namespace Oblivion
