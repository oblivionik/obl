/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "oblivion/oblivion_room.h"

class DocumentData;
class HistoryItem;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

// Round 5: the «Видео» tab of a room: watching together.
//
// The queue of the video player of the room (Kind::Video) and a synced
// player with sound inside the room window. The tab, a notice overlay and
// the snapshot scenes register themselves with the registrars of
// oblivion_room_window.h, nothing in the files of the rooms is touched.
//
// The picture and the sound come from the streaming player of the app
// (Media::Streaming::Document / Instance) over a loader of our own that
// reads the file of the relay while it is still being downloaded: the
// download of Room::download() goes from the start to the end into
// "<file>.part", a part of the file is given to the player as soon as it
// is on the disk. A video with its index at the start (every video that
// Telegram itself streams) starts after the first megabytes, the other
// ones when the whole file is here. Nothing of it needs the session: the
// file is not a document of Telegram.
//
// Sync. The position the video must have is PositionAt() by the clock of
// the server. A start is prepared on pause at the position the room will
// have a moment later and resumed exactly then; after that the real
// position is compared with the wanted one four times a second by the
// same SyncCorrector the music uses (a tempo nudge of 10 % with the pitch
// kept under 1.2 s, one seek with a cooldown beyond that).
//
// The room is controlled only by the buttons of the tab (with
// Right::Control). The video keeps playing while another tab of the room
// is shown. The mixer of the app has one video track: if another video
// with sound of the app (the media viewer) takes it, or plays already
// when the room starts, the picture of the room stops and the tab offers
// «Вернуться к просмотру»; the sound of the user's own video is never
// taken away without that click and nothing is paused for the others.
// The own volume of the tab scales the video volume of the app only
// while the sound of the room is in the mixer.
//
// Nothing is downloaded or played before the tab was opened once in the
// window of the room; a file goes to the relay only by a click («Добавить»
// in the tab, a dropped file, «Добавить видео в комнату» in the menu of
// a video message).
//
// The disk and the memory of this device. The room itself removes the
// file of an item that has left the queues and its folder after the
// window was closed. The player adds a budget for what is still in the
// queue: what was watched earlier goes first when the files it has
// brought take more than that or the disk is short, and nothing is
// brought when the disk has no room for it (the tab says so). Only the
// files Room::download() has made are ever removed, never a file of the
// user. The streaming reader keeps all it has read in the memory, so the
// player is reopened in the middle of a long video.
namespace Oblivion::Rooms {

// «Добавить видео в комнату» for the context menu of a video message:
// shown only while a room window of this account is open and the user may
// add media there. The file is downloaded from Telegram if it is not here
// yet and uploaded to the room by that click.
void AddVideoToRoomAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	HistoryItem *item,
	not_null<DocumentData*> document);

// OBLIVION_SELFTEST=room_video: the decisions of the synced player, the
// reading of a file that is still being downloaded, the geometry of the
// picture, the queue of the video player. Pure logic and files in the
// working folder, no network, no playback.
[[nodiscard]] bool RunVideoSelfTest(QStringList &log);

// ---- Pure logic (self-tested).

enum class VideoState {
	Idle, // Nothing is current in the room.
	Loading, // The file is being downloaded, nothing can be shown yet.
	Buffering, // It plays, but the download is behind.
	Failed, // The file could not be downloaded.
	NoSpace, // The disk of this device has no room for the file.
	Unplayable, // The file is here, the player can't play it.
	Paused, // The room is on pause.
	Starting, // Waiting for the moment or for the first frame.
	Synced,
	Catching, // The tempo is nudged.
	Away, // Another video has taken the sound: «Вернуться к просмотру».
	Waiting, // The item is over, the server switches.
};

struct VideoStatus {
	VideoState state = VideoState::Idle;
	int percent = 0; // Loading, Buffering: of the download.

	friend inline bool operator==(
		const VideoStatus &,
		const VideoStatus &) = default;
};

// What the player of this device does with the current item.
struct VideoLocal {
	bool active = false; // It was given the item.
	bool running = false; // It plays (or will by itself when it is ready).
	bool scheduled = false; // It is on pause, a resume is planned.
	bool finished = false; // It has played the item to the end.
	bool force = false; // Start as it is: enough restarts were made.
	int64 rev = -1; // PlayerState::rev all that was made for.
	int64 position = 0; // Where it is (or was prepared), milliseconds.
};

enum class VideoAction {
	None,
	Stop, // Nothing is current any more.
	Prepare, // Restart on pause at from, resume after wait if play.
	Hold, // Pause where it is.
	Resume, // Resume after wait.
	Adopt, // The new state of the room is what is played already.
};

struct VideoStep {
	VideoAction action = VideoAction::None;
	int64 from = 0;
	bool play = false;
	crl::time wait = 0;
	VideoState state = VideoState::Idle; // Without the local details.
};

// The only place that decides what the local player does when the state
// of the room or of the player changes. lead: how long a restart takes
// (the room will be at from when the prepared player is resumed).
[[nodiscard]] VideoStep NextVideoStep(
	const PlayerState &room,
	int64 duration,
	int64 serverNow,
	crl::time lead,
	const VideoLocal &local);

// The time a restart is given: follows what the last one has really
// taken, with a reserve.
[[nodiscard]] crl::time VideoPrepareLead(
	crl::time previous,
	crl::time measured);

// A file is read by the parts of the streaming loader. The length of the
// part at offset in a file of total bytes (0: there is no such part) and
// whether it lies within the first have bytes that are on the disk.
[[nodiscard]] int64 VideoPartLength(int64 offset, int64 total);
[[nodiscard]] bool VideoPartReady(int64 offset, int64 total, int64 have);

// The frame is asked from the player already scaled: the size of a video
// fitted into box (device pixels), not larger than limit, never empty.
[[nodiscard]] QSize VideoFrameSize(QSize video, QSize box, QSize limit);
// Where a picture of the size image goes in box (centered, the whole
// picture is seen).
[[nodiscard]] QRect VideoFrameRect(QSize image, QRect box);

// A file the «Видео» tab takes by its name.
[[nodiscard]] bool VideoFileName(const QString &name);

// «Играть следующим»: the index to move the item with the index to, by
// the index of the current one. -1: it is there already.
[[nodiscard]] int VideoPlayNextIndex(int index, int current);

// Whether a picture of the room keeps the display of the device on: only
// while it moves and is really shown (not on pause, not behind another
// tab, not while the user watches something else).
[[nodiscard]] bool VideoKeepsDisplayOn(
	VideoState state,
	bool playing,
	bool picture,
	bool shown);

// A file of the room on the disk of this device.
struct VideoFile {
	QString media;
	int64 bytes = 0;
	crl::time usedAt = 0; // When it was the current item the last time.
	bool kept = false; // Current, next, being downloaded, of the music.
};

struct VideoSpace {
	std::vector<QString> evict; // The files to remove, in this order.
	bool enough = true; // The new file fits after that.
};

// What goes before a new file is brought. files: all the room has here
// with the new one among them (kept), budget: how much they may take
// together, available: the free bytes of the disk (negative: not known),
// needed: how many of them the new file takes with a reserve.
[[nodiscard]] VideoSpace PlanVideoSpace(
	std::vector<VideoFile> files,
	int64 budget,
	int64 available,
	int64 needed);

// The streaming reader never lets go of what it has read from a file
// without a cache. The player is reopened (a pause of a part of a second)
// when that is a lot: read bytes were given to it, sinceLast milliseconds
// have passed after the previous time, left milliseconds of the item are
// still to be played.
[[nodiscard]] bool VideoReaderRestart(
	int64 read,
	crl::time sinceLast,
	int64 left);

} // namespace Oblivion::Rooms
