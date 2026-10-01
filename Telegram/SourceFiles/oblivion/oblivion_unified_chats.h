/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class History;
class QKeyEvent;

namespace Ui {
class RpWidget;
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

// One chats list with the chats of all the authorized accounts, switched
// on and off by Oblivion::Get().unifiedChats() (off by default).
//
// How it is shown: the usual chats list (Dialogs::Widget) stays untouched,
// while the mode is on a panel with the merged list is created as a child
// of its scroll area and covers it. The panel is visible only in the plain
// state of the list: "All chats", no search, no opened archive, forum or
// community. Everything else (search, folders, the contents of the archive,
// forum topics) works as before and stays per-account. With the mode off
// nothing is created and nothing changes.
//
// The merged list has the chats and the archive rows of all the accounts:
// the pinned ones first (account by account), the rest by the date of the
// last message. A click on a chat of another account switches the window
// to that account and opens the chat there.
namespace Oblivion::UnifiedChats {

// The setting is on and more than one account is authorized.
// Main thread only, needs Core::App().
[[nodiscard]] bool Active();

struct AttachArgs {
	// The Dialogs::Widget that gets the unified list.
	not_null<Ui::RpWidget*> dialogs;
	not_null<Window::SessionController*> controller;

	// The scroll area with the usual list, the panel becomes its child
	// and follows its size, visibility and grabs.
	not_null<Ui::RpWidget*> scroll;

	// The height of the bar pinned to the top of the scroll (the top bar
	// suggestion), the panel starts below it.
	rpl::producer<int> topInset;

	// True while the usual list shows plain "All chats" (see above).
	Fn<bool()> plain;

	// Events after which plain() could change its value, in addition
	// to Refresh() calls.
	rpl::producer<> plainChanges;

	// Opens a chat of the account of this window the usual way.
	Fn<void(not_null<History*> history, bool newWindow)> choose;

	// The height of the stories row expanded by a click (0 while it is
	// collapsed) and the way to collapse it: scrolling the unified list
	// down does that, like scrolling the usual one.
	rpl::producer<int> storiesExpanded;
	Fn<void()> collapseStories;
};

// Called once from the Dialogs::Widget constructor (the main layout only).
void Attach(AttachArgs &&args);

// plain() of this Dialogs::Widget could change, checked in a queued call.
void Refresh(not_null<QWidget*> dialogs);

// Up / Down / PageUp / PageDown / Enter while the unified list is shown.
[[nodiscard]] bool HandleKey(
	not_null<QWidget*> dialogs,
	not_null<QKeyEvent*> e);

// The quick toggle in the accounts part of the main menu, visible while
// more than one account is authorized.
void AddMainMenuToggle(not_null<Ui::VerticalLayout*> container);

// The order of the merged list.
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::UnifiedChats
