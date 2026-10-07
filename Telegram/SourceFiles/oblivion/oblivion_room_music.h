/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"
#include "oblivion/oblivion_room.h"

class AudioMsgId;
class DocumentData;

namespace Ui {
class RpWidget;
class Show;
} // namespace Ui

// Round 5: the «Музыка» tab of a room: the shared queue and the synced
// player.
//
// The sound goes through the audio player of the app (Media::Player), so
// the bar under the chats list, the system media keys and the volume of
// the app work as for any other track. A track of the room is a file of
// the relay downloaded into the temp folder of the room and played as
// a local music document.
//
// Sync. The position the track must have is PositionAt() by the clock of
// the server (Cloud::Now()). The engine starts the track a bit before
// the moment (the measured latency of a start), then compares the real
// position with the wanted one on every update of the player:
//  - under 80 ms: nothing;
//  - up to 1.2 s: the tempo is changed by 10 % (with the pitch kept, so
//    there is no jump and no "chipmunk") till the difference is under
//    25 ms. 10 % is the smallest step the mixer of the app has;
//  - more, or the difference grows: one seek, not more often than once
//    in three seconds (doubled every time it did not help).
// The end of a track belongs to the server: the engine never sends
// "next" by itself, it starts the next file at once if it is here
// (gapless) and takes the exact state when the event comes.
//
// The local player and the room. The room is controlled from its window
// (and by next / previous / a seek made in the bar of the app or on the
// media keys, with Right::Control). Pause and play of the app's own
// player are the user's own business: the app pauses the player by
// itself too (a call, a video with sound), and that must never stop the
// music for everybody. A pause leaves the air, play (or «Вернуться в
// эфир» in the tab) brings back to where the room is now. Playing
// another track of Telegram leaves the air as well. Without
// Right::Control a seek is undone by the correction.
namespace Oblivion::Rooms {

// ---- Pure sync maths (OBLIVION_SELFTEST=room_sync).

// When and from where to start a playing item so that the sound is
// where PositionAt() says. lead: the latency of a start of the player.
struct StartPlan {
	bool play = false; // false: the room does not play now.
	bool over = false; // The item is over, the server switches.
	crl::time wait = 0; // Start after that, 0: now.
	int64 from = 0; // The position to start from, milliseconds.
};
[[nodiscard]] StartPlan PlanStart(
	const PlayerState &state,
	int64 duration,
	int64 serverNow,
	crl::time lead);

class SyncCorrector final {
public:
	struct Result {
		bool seek = false;
		int64 target = 0; // For a seek.
		double speed = 1.; // 0.9, 1. or 1.1.
	};

	// A new item or a new state of the room.
	void reset();
	// The player was told to play from target. cold: the first start of
	// a file, its latency says nothing about the later ones.
	void started(crl::time now, int64 target, bool cold);
	// expected: where the room is now, actual: where the player is.
	[[nodiscard]] Result update(crl::time now, int64 expected, int64 actual);

	[[nodiscard]] crl::time lead() const {
		return _lead;
	}
	[[nodiscard]] double speed() const {
		return _speed;
	}
	[[nodiscard]] bool awaiting() const {
		return _awaiting;
	}
	[[nodiscard]] int64 drift() const {
		return _drift;
	}

private:
	crl::time _lead = 120;
	crl::time _issuedAt = 0;
	int64 _issuedTarget = 0;
	crl::time _lastSeek = 0;
	crl::time _cooldown = 3000;
	int64 _recent[3] = { 0, 0, 0 };
	int _count = 0;
	int64 _drift = 0;
	double _speed = 1.;
	bool _awaiting = false;
	bool _cold = false;
	bool _seeked = false;

};

// ---- The engine: one per open room (Room::music()).

enum class LocalState {
	Idle, // Nothing is current in the room.
	Loading, // The file of the current item is being downloaded.
	Failed, // It could not be downloaded or played.
	Paused, // The room is on pause.
	Starting, // Waiting for the moment or for the sound.
	Synced,
	Catching, // The tempo is nudged or a seek was just made.
	Away, // The user has left the air: «Вернуться в эфир».
	Waiting, // The item is over, the server switches.
};

struct LocalStatus {
	LocalState state = LocalState::Idle;
	int percent = 0; // Loading.

	friend inline bool operator==(
		const LocalStatus &,
		const LocalStatus &) = default;
};

class MusicEngine final : public base::has_weak_ptr {
public:
	explicit MusicEngine(not_null<Room*> room);
	~MusicEngine();

	[[nodiscard]] LocalStatus status() const;
	[[nodiscard]] rpl::producer<LocalStatus> statusValue() const;

	// «Вернуться в эфир».
	void rejoin();

	// A music file message of Telegram goes to the queue of the room:
	// downloaded from Telegram if it is not here yet (a toast says so),
	// then uploaded to the relay. Called only from a click.
	void addDocument(
		not_null<DocumentData*> document,
		FullMsgId origin,
		std::shared_ptr<Ui::Show> show);

	// The hooks below.
	[[nodiscard]] bool drives(const AudioMsgId &current) const;
	[[nodiscard]] bool canMove(int delta) const;
	bool move(int delta, bool autonext);
	[[nodiscard]] crl::time takeStartPosition(const AudioMsgId &audioId);
	[[nodiscard]] float64 speedFor(const AudioMsgId &audioId) const;

private:
	struct Private;
	const std::unique_ptr<Private> _private;

};

[[nodiscard]] std::unique_ptr<MusicEngine> CreateMusicEngine(
	not_null<Room*> room);

// Media::Player::Instance hooks, song playback only. While the current
// song is a track of an open room: previous / next go to the room (with
// Right::Control), the end of the track starts the next file of the
// queue, the start position and the tempo are those of the engine.
[[nodiscard]] bool DrivesPlayer(const AudioMsgId &current);
[[nodiscard]] bool PlayerCanMove(int delta);
bool PlayerMove(int delta, bool autonext);
[[nodiscard]] rpl::producer<> PlayerChanges();
// -1: not a start made by a room.
[[nodiscard]] crl::time TakeStartPosition(const AudioMsgId &audioId);
// 0.: not a track of a room.
[[nodiscard]] float64 PlayerSpeed(const AudioMsgId &audioId);

// ---- The tab.

[[nodiscard]] object_ptr<Ui::RpWidget> CreateMusicTab(
	QWidget *parent,
	not_null<Room*> room,
	std::shared_ptr<Ui::Show> show);

} // namespace Oblivion::Rooms
