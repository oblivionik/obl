/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_msg_id.h"

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

// Oblivion rooms («Добавить из плейлиста»): the playlists of the account
// as plain data. RoomPlaylistTracks() asks the server for the messages of
// that playlist that are not loaded yet (the same lookups the playlist
// box makes when it is opened), RoomPlaylistsChanges() fires when one has
// come or a playlist has changed.
struct RoomPlaylistBrief {
	uint64 id = 0;
	QString name;
	int count = 0;
};
struct RoomPlaylistTrack {
	FullMsgId id;
	QString title;
	QString performer;
	QString fileName;
	int duration = 0; // Seconds.
	bool ready = false; // The message with the file is loaded.
	bool failed = false; // It is gone or can't be asked for.
};
[[nodiscard]] std::vector<RoomPlaylistBrief> RoomPlaylists(
	not_null<Main::Session*> session);
[[nodiscard]] std::vector<RoomPlaylistTrack> RoomPlaylistTracks(
	not_null<Main::Session*> session,
	uint64 playlistId);
[[nodiscard]] rpl::producer<> RoomPlaylistsChanges(
	not_null<Main::Session*> session);

} // namespace Oblivion
