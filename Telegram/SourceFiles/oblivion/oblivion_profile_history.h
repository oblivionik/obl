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

namespace Main {
class Session;
} // namespace Main

namespace Ui::Menu {
struct MenuCallback;
} // namespace Ui::Menu

namespace Window {
class SessionController;
} // namespace Window

// Profile history: name, username, bio and photo changes of the contacts
// (with the old photos), recorded while Oblivion::Get().profileHistory()
// is on. Everything is stored only on this device, per account:
//
// tdata/oblivion/<user id>/profiles/state.jsonl    last known profiles,
// tdata/oblivion/<user id>/profiles/changes.jsonl  the recorded changes,
// tdata/oblivion/<user id>/profiles/<peer id>/<photo id>[_big].jpg
//
// Tracked are the contacts, the chats from the chats list (users, groups
// and channels) and every profile the history box was opened for. The
// first time a profile is seen it only becomes the baseline, changes are
// recorded against it, including the ones that happened while the app
// was closed (those get the time they were noticed at).
//
// The name of a contact is not tracked: the server sends the name from
// our own address book in place of the real one.
namespace Oblivion {

// Called once per session from the Main::Session constructor (deferred
// with crl::on_main(session, ...), so the session is fully set up).
void StartProfileHistory(not_null<Main::Session*> session);

// Called from Main::Session::finishLogout(): drop the stored history
// of this account.
void ForgetProfileHistory(not_null<Main::Session*> session);

// The recorded changes of one profile. Opening it starts tracking
// the profile even if it is not a contact and not in the chats list.
void ShowProfileHistory(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer);

// "Profile history" in the peer menus (window_peer_menu.cpp): shown when
// the feature is on or there are recorded changes of this profile. For
// a deleted account only when there are recorded changes.
void AddProfileHistoryAction(
	not_null<Window::SessionController*> controller,
	const Dialogs::Key &key,
	const Ui::Menu::MenuCallback &addAction);

namespace ProfileHistory {

// Called from Data::Session::processUser() / processChat() after a full
// (not "min") object from the server was applied. Only such profiles are
// compared with the stored ones: the peers restored from the local cache
// may be outdated and incomplete (no usernames list, for example), they
// would give false changes. Cheap: a hash set lookup, the comparison
// itself is deferred and batched.
void Seen(not_null<PeerData*> peer);

// Self-checks for OBLIVION_SELFTEST=profile_history, see
// oblivion_selftest.h. No Core::App(), no session: pure logic only.
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace ProfileHistory
} // namespace Oblivion
