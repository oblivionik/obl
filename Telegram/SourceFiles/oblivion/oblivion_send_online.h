/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"

class History;
class PeerData;
struct TextWithTags;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class RpWidget;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

// Round 5: «Отправить, когда будет в сети».
//
// A text message of a private chat waits on this device and is sent when
// the person comes online: when the session gets an "online" status of
// the user (the server sends it by itself or the online tracker asks for
// it, see Online::SetWaitedUsers in oblivion_online.h) or a fresh message
// from them arrives. Nothing waits on the server and nothing is sent to
// anyone before that moment.
//
// The queue is kept per account in tdata/oblivion/<user id>/
// send_online.json (not encrypted, like the other Oblivion files there),
// it survives a restart and is removed on logout.
//
// Exactly once:
// - a message gets its random id when it is put into the queue, every
//   attempt to send it uses that id, so the server refuses a repeat
//   (RANDOM_ID_DUPLICATE is taken as "sent");
// - "sending" is written to the disk before the request leaves; a
//   message found in that state after a restart is sent again by itself
//   only within a few minutes of the attempt, later the user is asked
//   («Не удалось подтвердить отправку»), it never goes out silently;
// - the same after an attempt with no clear answer (a server error, an
//   answer that was not read): it is repeated by itself a few times
//   within those minutes, without waiting for the person to come online
//   once more, then the user is asked; the text of such a message can't
//   be changed any more, the repeat must be the same message;
// - several messages for one person go one by one, in their order.
//
// The limit of Oblivion::Get().sendWhenOnlineHours(): after it a message
// is not sent, it stays in the list as «Срок ожидания истёк» and the user
// is asked what to do. The same when the chat is not available any more
// (deleted or blocked account, paid messages) or the server refused it.
//
// Nothing goes behind the user's back:
// - a message that is being edited, or whose removal is being confirmed,
//   is held until that box is closed, and so are the ones behind it;
// - «Удалить чат» on this device and blocking the person stop everything
//   that waits for them: it stays in the list as not sent, with the
//   reason, and only a click sends it (a chat deleted from another
//   device is not noticed);
// - a person who is not loaded yet (right after the start) and a lost
//   connection are reasons to wait, never to give up or to send later
//   than promised.
//
// Sending goes the way of an ordinary text message, with "send without
// going online" of the ghost mode applied (oblivion_sending.h), but the
// chat is neither read nor scrolled: the user may be away. In a chat
// that is read invisibly the delivery does not mark it as read either.
namespace Oblivion::SendOnline {

// Called by Cloud::SessionStarted() / Cloud::SessionLoggedOut() (the
// feature itself does not use the cloud).
void Start(not_null<Main::Session*> session);
void Forget(not_null<Main::Session*> session);

// Settings > Oblivion > «Ожидают отправки»: the list of the waiting
// messages of this account (edit, send now, wait more, remove).
void ShowList(not_null<Window::SessionController*> controller);

// The send button menu of a chat (history_widget.cpp): whether the item
// is there. The setting is on and the chat is a private one with a
// person that a text can be sent to.
[[nodiscard]] bool Offered(not_null<PeerData*> peer);

// The item was chosen. True: the text is in the queue (or on its way, if
// the person is online right now), the field is to be cleared. False:
// it was not taken, a toast says why, the field stays as it is.
[[nodiscard]] bool Enqueue(
	not_null<Window::SessionController*> controller,
	not_null<History*> history,
	const TextWithTags &text);

// The bars of a chat under its top bar. makeAbove creates the bar that
// was there before (listening together), with the parent it is given.
// For a chat this feature is not for, exactly that bar is returned (or
// nothing). Otherwise both live in one widget, the bar of the waiting
// messages below: it shows how many messages wait or were not sent and
// opens their list. The widget changes its height by itself, the caller
// gives it the width (resizeToWidth) and follows heightValue().
[[nodiscard]] object_ptr<Ui::RpWidget> WithChatBar(
	not_null<QWidget*> parent,
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	Fn<object_ptr<Ui::RpWidget>(not_null<QWidget*>)> makeAbove);

// OBLIVION_SELFTEST=send_online, pure logic.
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::SendOnline
