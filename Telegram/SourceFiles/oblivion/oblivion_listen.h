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
class PeerData;
class QVariant;

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Ui::Menu {
struct MenuCallback;
} // namespace Ui::Menu

namespace Window {
class SessionController;
} // namespace Window

// Listening to music together: synchronised playback between Oblivion
// users over an ordinary chat, there is no server for it.
//
// The host starts a session in a private chat or a group, the track must
// be a message of that chat. The client of the host posts one control
// message, a headphones emoji and "Listening together: Artist - Title",
// and keeps the playback state in it: the track, the position at
// a server-synchronised time, playing or paused, the next tracks,
// a sequence number.
//
// Nothing is ever sent to the chat by itself. A track is sent there only
// by a click of the host for that very track: "Start" in the box that
// says so (a session started with a track from another chat), or "Send
// to chat" in the bar. When the host plays something that is not
// a message of the chat (another chat, a playlist), the session is on
// pause for the listeners, their bars say why, and the host is offered
// that button. A copy is a plain forward without the sender name and
// without the caption. The manager sends it by itself and knows what
// became of it: while the server has not answered (no connection) the
// track is "being sent" and the button is not offered again, so a click
// never gives two copies; it is offered again only after the server has
// refused the forward. A forward that still waits for the connection is
// taken back when the session is over. The chat is not marked as read
// by such a send.
//
// A group with the slow mode on takes one message at a time and the
// next one after a wait. There a session is started only with a track
// that the chat already has (the track and the control message can't go
// one after another), not during the countdown, and "Send to chat" is
// offered when the countdown is over.
//
// How a track is named. The ids of the messages of a private chat or
// of a basic group are different for every account, only a supergroup
// has the same ids for everybody. So a track is the id of its music file
// (the same for all accounts), the date of its message and the id that
// the message has for the host. A listener finds the message with that
// file among the messages of the chat he has; if it is not there, he
// asks the server: in a supergroup for the message by its id, once; in
// other chats he searches the music of the chat by that date, around it
// and then (a message that the host has just uploaded has a later date
// for the server) in the hour after it. One request at a time, three
// seconds between two of them, not more than ten in five minutes, two
// searches for a file at most. The pace (with a FLOOD_WAIT that the
// server has asked for) is one for the whole app, and what is found or
// not found is remembered for the session: leaving it and joining it
// again starts nothing over and asks nothing twice.
//
// Where the state lives: the headphones emoji is a text link
// (messageEntityTextUrl), the state is the fragment of its address,
// "https://t.me/#oblivion-listen.<base64url>". The server stores such
// an address as it is and the message is sent and edited without a link
// preview, so every other app shows a normal short text with an emoji.
// Oblivion finds the entity when it reads the message (EntityType::
// CustomUrl, see Api::EntitiesFromMTP) and never needs anything but the
// messages the chat already delivers.
//
// Oblivion clients in the chat show "Join" (oblivion_listen_ui.h): their
// player plays the same track from the chat and follows the host (drift
// correction, pause, seek, the next track) with their own volume and a
// "Leave" button. Only the host controls the playback. Ending the
// session edits the message to "ended".
//
// The host updates the state by editing its message: not more often
// than once in two seconds with the changes coalesced, one request in
// flight, FLOOD_WAIT waited out in full, nothing repeated in a loop.
// Nothing is sent while nothing changes: a playing track needs no edits
// at all, the listeners compute the position from the time.
//
// When a session ends for the host:
// - "End" in the bar of the chat, or the player is closed by the host
//   while it has a song (the player that closes by itself after a voice
//   message ends nothing);
// - the playback stays paused or stopped for an hour;
// - the control message is deleted, or it can't be edited any more;
// - the setting is switched off, the account logs out;
// - the app quits: the "ended" edit is sent on the way out, the quit
//   waits for it for a moment (Core::Application::readyToQuit).
// Closing the chat or switching to another chat or account does not end
// it, the session belongs to the player, not to the chat window.
// If the app of the host dies without the "ended" edit, the listeners
// end the session themselves when the track is over and nothing new has
// come from the host for half a minute.
//
// With Oblivion::Get().listenTogether() off no session can be started
// or joined and the control messages are shown as the plain texts they
// are.
namespace Oblivion::Listen {

// Host side: asks and then starts a session in the chat peer (a private
// chat with a user or a group) with the track of the message track,
// a music file of the account of this window. If the chat has no message
// with that file, the box says that the track will be sent there, and it
// is sent after "Start", that one track only; the session starts when it
// is really there. What can't be done (a channel, a group with topics,
// no right to send there, paid messages, not a music file, the setting
// is off, another session is hosted already, the slow mode of the group
// does not let the messages in now) is said by a toast. Main thread.
void Start(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	FullMsgId track);

// The filter for the "edited messages history" (EditHistory::remember()
// in oblivion_deleted_store.cpp, called there before an edit is applied
// to item): true only for an edit that the host of a session makes to
// its control message. That is: the message is an ordinary text message
// written by a person (not forwarded, not a channel post, not sent via
// a bot; a session whose host writes to the group anonymously or as
// a channel can be joined, but the edits of its message are recorded as
// any other), its text before the edit and its text after the edit both
// are exact control messages (the headphones, the link over them, a state
// that decodes with a right checksum), of the same session, the state
// after is newer than the state before, and the session was not ended
// before. Anything else is recorded as usual: an ordinary message that
// is edited into something with such a link, a message that only looks
// like a control one, an edit of an ended session. Only the edits that
// change the visible text get here at all (the next track, the end),
// the others keep the text and are skipped by the caller.
// Main thread, works with the setting off as well.
[[nodiscard]] bool IsControlEdit(
	not_null<const HistoryItem*> item,
	const TextWithEntities &was,
	const TextWithEntities &now);

// Self-checks for OBLIVION_SELFTEST=listen, see oblivion_selftest.h.
// No Core::App(), no session, no network: pure logic only (the encoding
// of the state, the filter of the edits, the choice of the track message,
// the pace of its lookups and what is remembered about them, the slow
// mode check and the refusals of a send, the clock, the throttle, the
// drift correction).
[[nodiscard]] bool RunSelfTest(QStringList &log);

// "Listen together in this chat" for the context menu of a music file
// message: the session starts in the chat of the message. Adds nothing
// if it can't be offered (not a music file, the chat doesn't fit, the
// setting is off, this app hosts a session already).
void AddStartAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item);

// The same for the menu of a chat: the track is the one that the audio
// player of this account has now, playing or paused. Adds nothing while
// the player is empty.
void AddStartAction(
	const Ui::Menu::MenuCallback &addAction,
	not_null<Window::SessionController*> controller,
	PeerData *peer);

// Hook for HiddenUrlClickHandler::Open(): a click on the headphones of
// a control message joins the session instead of opening the address.
// False (and nothing done) for any other address and with the setting
// off.
[[nodiscard]] bool OpenControlLink(
	const QString &url,
	const QVariant &context);

// Media::Player::Instance hooks, song playback only. While this app
// follows a host and the current song is the track of the session, the
// listener can't switch tracks: previous / next are not available, and
// when the track is over the player goes to the next track of the queue
// that the host has published (so there is no gap while the edit of the
// host is on its way), or stops and waits for the host.
[[nodiscard]] bool Follows(const AudioMsgId &current);
bool FollowerMove(int delta, bool autonext);
[[nodiscard]] rpl::producer<> FollowChanges();

// Hook for Media::Player::Instance::streamingOptions(): the position in
// milliseconds from which the player of a listener starts audioId (where
// the host is now), or -1 for any playback that was not started by this
// code. The value is given out once.
[[nodiscard]] crl::time TakeStartPosition(const AudioMsgId &audioId);

// Hook for Core::Application::readyToQuit(): true while the "ended"
// edit of the hosted session is on its way, the first call sends it.
// Core::App().quitPreventFinished() is called when it is done.
[[nodiscard]] bool IsQuitPrevent();

// A track of a session: a message of the chat with a music file.
struct Track {
	int64 id = 0; // The id that the message has for the host.
	uint64 document = 0; // The id of the file, the same for everybody.
	int64 date = 0; // The date of the message, unixtime in seconds.

	friend inline bool operator==(const Track &, const Track &) = default;
};

// The state of a session as it is written into the control message.
// Times are in milliseconds, at is the server-synchronised unixtime.
struct State {
	uint64 session = 0; // 48 random bits, the same for all the edits.
	uint32 seq = 0; // Grows with every edit.
	Track track;
	int64 position = 0; // The position of the track at the moment at.
	int64 at = 0;
	int64 duration = 0; // Of the track, 0 if it is not known.
	bool playing = false;
	bool ended = false;
	bool away = false; // The host plays what is not in the chat.
	std::vector<Track> queue; // What the host plays next.

	friend inline bool operator==(const State &, const State &) = default;
};

// What the bar of a chat shows, see oblivion_listen_ui.h.
struct ChatView {
	enum class Kind {
		None, // No live session in the chat.
		Offer, // Somebody listens there, "Join".
		Joined, // This app follows the host, "Leave".
		Hosting, // This app is the host, "End".
		Starting, // This app is the host, the messages are being sent.
	};
	enum class Sync {
		Fine,
		Loading, // The track is being looked for or loaded.
		Unavailable, // The track message can't be found or played.
		OwnPause, // The listener has paused the player himself.
		Waiting, // The track is over, waiting for the host.
		Away, // The host plays a track that is not in the chat.
	};
	Kind kind = Kind::None;
	Sync sync = Sync::Fine;
	QString track; // "Artist - Title".
	QString host; // The short name of the host, empty for Hosting.
	bool playing = false;
	bool canSend = false; // Hosting, Away: "Send to chat" is offered.
	bool sending = false; // Hosting, Away: the track is being sent.
	uint64 sendId = 0; // Hosting, Away: the file of the track named.

	// Look again after that, 0 is never. Offer: the session is not live
	// any more then. Hosting, Away: the slow mode of the group lets the
	// track in then.
	crl::time staleIn = 0;

	friend inline bool operator==(
		const ChatView &,
		const ChatView &) = default;
};

// For oblivion_listen_ui.cpp, main thread. ChatFits() is true for the
// chats where a session is possible at all: private chats with people
// and groups without topics. Lookup() is cheap and sends nothing;
// Changes() fires after it could have changed for any chat.
[[nodiscard]] bool ChatFits(not_null<PeerData*> peer);
[[nodiscard]] ChatView Lookup(not_null<PeerData*> peer);
[[nodiscard]] rpl::producer<> Changes();

// Looks through the loaded messages of the chat for a control message,
// if no session is known there yet: the messages of an opened chat come
// from the server later than its bar is created. True if it has found
// one, Lookup() should be asked again then. Sends nothing.
bool Rescan(not_null<PeerData*> peer);

void Join(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer);
void Leave();
void End();

// The host has pressed "Send to chat" (ChatView::canSend): the track
// named in the bar (ChatView::sendId), the one his player has now, is
// forwarded to the session chat. One track for one click, nothing if
// the player has gone to another track meanwhile or a track is on its
// way to the chat already.
void SendTrack(uint64 sendId);

} // namespace Oblivion::Listen
