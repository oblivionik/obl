/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class PeerData;

namespace Dialogs {
class Key;
} // namespace Dialogs

namespace Ui::Menu {
struct MenuCallback;
} // namespace Ui::Menu

namespace Window {
class SessionController;
} // namespace Window

// The UI of the chat statistics (oblivion_chat_stats.h): a box that reads
// the history of the chat with progress and cancel, then shows the
// numbers and the charts, with the choice of the period.
namespace Oblivion {

// Opens the statistics of a chat: a private chat with a user or a group
// (a basic group or a supergroup). For anything else (a channel, a bot,
// Saved Messages, a service chat) only a toast says that there is nothing
// to count. peer must belong to the account of this window.
//
// The history starts being read at once (what is not in the cache yet)
// and goes on after the box is closed, till it is complete or stopped.
// A reading the user has stopped is not started again by opening the box,
// only by the button in it.
void ShowChatStats(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer);

// "Chat statistics" in the peer menus (window_peer_menu.cpp): for the
// chats ShowChatStats() can count, not for a topic or a sublist.
void AddChatStatsAction(
	not_null<Window::SessionController*> controller,
	const Dialogs::Key &key,
	const Ui::Menu::MenuCallback &addAction);

} // namespace Oblivion
