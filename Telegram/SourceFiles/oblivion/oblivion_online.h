/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class PeerData;
class UserData;

namespace Main {
class Session;
} // namespace Main

namespace Ui::Menu {
struct MenuCallback;
} // namespace Ui::Menu

namespace Window {
class SessionController;
} // namespace Window

// "X is online" notifications and the online journal.
//
// Settings: Oblivion::Get().onlineJournal() (record online / offline
// intervals of every user whose status updates arrive while the client
// is open) and Oblivion::Get().onlineNotify*() (users to notify about
// when they come online).
//
// The server sends the statuses only to an account it shows as online
// (an active window that was used recently, the "online" ghost switch
// off), so the time without that is "no data" in the journal. The users
// from the notify list are an exception: with onlinePolling() on (the
// default) their statuses are requested by users.getUsers, for the
// notifications to work in the background and their journals to be full.
//
// The official apps send no such request, so it is kept modest:
// - one request each 60-90 seconds at most, at random moments, and two
//   accounts never send theirs within three seconds of each other;
// - one user is asked about by one account only: the active one if it
//   knows the user, otherwise the first of those that do (by the id of
//   the account), and the accounts that the user hides the last seen
//   time from ask only if no account sees it; so the journals of the
//   other accounts have "no data" for the time they are not shown as
//   online;
// - nothing is sent when the answer can't be used (an empty list, the
//   journal is off and the notifications of this account are dropped);
// - an account shown as online asks about its contacts and those it has
//   a chat with once when it becomes shown and then once in five minutes
//   only, the server sends their statuses by itself;
// - more than 100 users are asked about in turns, 100 in a request;
// - FLOOD_WAIT is waited out in full, other errors make the requests
//   rarer, a request without an answer is never repeated at once.
//
// Everything is stored only on this device, per account, in
// tdata/oblivion/<user id>/online.dat, for 60 days.
namespace Oblivion {

// Called once per session from the Main::Session constructor (deferred
// with crl::on_main(session, ...), so the session is fully set up).
void StartOnlineTracker(not_null<Main::Session*> session);

// Called from Main::Session::finishLogout(): drop the stored journal
// of this account.
void ForgetOnlineJournal(not_null<Main::Session*> session);

// Settings > Oblivion > Tracking > "Online notifications": the list of
// users to notify about, with adding and removing.
void ShowOnlineNotifyList(not_null<Window::SessionController*> controller);

// The online journal of one user: when they were online, by days.
void ShowOnlineJournal(
	not_null<Window::SessionController*> controller,
	not_null<UserData*> user);

// "Notify when online" and "Online journal" for the chat menus, added
// only for real users (not for bots, yourself or service accounts).
void AddOnlineActions(
	const Ui::Menu::MenuCallback &addAction,
	not_null<Window::SessionController*> controller,
	PeerData *peer);

namespace Online {

// Self-checks for OBLIVION_SELFTEST=online, see oblivion_selftest.h.
// No Core::App(), no session: pure logic only (intervals, storage).
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Online
} // namespace Oblivion
