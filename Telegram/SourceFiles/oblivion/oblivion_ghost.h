/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class History;
class HistoryItem;
class PeerData;

namespace Api {
struct SendAction;
struct SendOptions;
} // namespace Api

namespace Data {
class Thread;
} // namespace Data

namespace Ui::Menu {
struct MenuCallback;
} // namespace Ui::Menu

namespace Window {
class SessionController;
} // namespace Window

namespace Oblivion {

// Reading in this chat must stay invisible for other people.
// Saved Messages, Replies and service chats are always read normally,
// nobody can see reading there anyway.
[[nodiscard]] bool GhostReadFor(not_null<PeerData*> peer);

// Chats where the per-chat "read invisibly" toggle makes sense.
[[nodiscard]] bool CanToggleGhostRead(not_null<PeerData*> peer);

// Marks history as read till tillId on this device only, no request
// is sent. Returns false and does nothing if the chat is read normally.
// The local read till is remembered on disk, so after a restart the chat
// doesn't become unread again (see ApplyLocalReadTill).
bool ReadInboxLocally(not_null<History*> history, MsgId tillId);

// Called when a dialog arrives from the server: moves the read till
// forward to what was read invisibly before, while the server lags behind.
void ApplyLocalReadTill(
	not_null<History*> history,
	MsgId serverReadTill,
	MsgId topMessageId);

// A message is being sent to a chat that is read invisibly: if the
// "mark chat as read when sending" option is on, the chat (or the forum
// topic) is marked as read on the server as well. Scheduled sends are
// skipped (sends made offline are not, see oblivion_sending.h). Forums and
// administered monoforums are never read as a whole, only a forum topic.
void ReadOnSend(const Api::SendAction &action);

// Same for sends that build their MTP requests by themselves and don't go
// through ApiWrap::sendAction() (share box forwards, story sharing), so
// they read the chat on send like any other send, offline or not. Such
// sends don't read the chat locally either, so in a chat that is read
// invisibly it is read here as well, like a send from inside the chat.
// Chats that are read normally are left as is, like upstream does.
void ReadOnSendFor(
	not_null<Data::Thread*> thread,
	const Api::SendOptions &options);

// The item is an unplayed incoming voice / video message in a chat that
// is read invisibly, so the "played" mark must not be sent to the server.
[[nodiscard]] bool HideContentsRead(not_null<HistoryItem*> item);

// Stories are marked as seen only locally, views are not sent.
[[nodiscard]] bool GhostStories();

void AddGhostReadAction(
	const Ui::Menu::MenuCallback &addAction,
	not_null<Window::SessionController*> controller,
	PeerData *peer);

} // namespace Oblivion
