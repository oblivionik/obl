/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class AudioMsgId;
class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Oblivion {

// Opens the local playlists manager (create, rename, reorder, play).
// Playlists are stored per account in tdata/oblivion/<userId>/.
void ShowPlaylists(not_null<Window::SessionController*> controller);

// Adds "Add to playlist" to a message context menu if the message
// has a music (song / audio file) document. Does nothing otherwise.
void AddToPlaylistMenu(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item);

// Called on logout, like the rest of the account's local data playlists
// are forgotten: playlists.json is removed and the in-memory playlists,
// pending lookups and the playback queue of this account are dropped.
void ForgetPlaylists(not_null<Main::Session*> session);

// Media::Player::Instance hooks, song playback only.
// While the current song was started from a local playlist, previous /
// next / auto-advance follow the playlist order instead of the chat's
// shared music. Any other playback leaves the upstream logic untouched.
[[nodiscard]] bool PlaylistDrivesPlayer(const AudioMsgId &current);
[[nodiscard]] bool PlaylistPlayerCanMove(int delta);
bool PlaylistPlayerMove(int delta, bool autonext);
[[nodiscard]] rpl::producer<> PlaylistPlayerChanges();

} // namespace Oblivion
