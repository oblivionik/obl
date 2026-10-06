/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"

class PeerData;

namespace Ui {
class BoxContent;
class RpWidget;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

// The UI of listening together (oblivion_listen.h): the bar of a chat
// with a session and the box that asks before a session is started.
namespace Oblivion::Listen {

// The per-chat bar, created once by the chat widget for its chat and
// placed under the top bar below the other bars of the chat (the pinned
// message, the group call). While the chat has a live session it shows
// who listens to what and "Join"; for those who have joined the state
// of the playback, their own volume and "Leave"; for the host "End",
// and "Send to chat" while he plays a track that the chat does not have
// (nothing is sent without that click, the listeners are on pause).
// When the session is over it says so for a few seconds. Otherwise it
// is hidden with a zero height. It changes its height by itself, with
// an animation (the first state is applied at once): the caller gives
// it the width (resizeToWidth), listens to heightValue() and lays out
// the chat as for the other bars.
//
// Returns nullptr when Oblivion::Get().listenTogether() is off or the
// chat is not one where a session is possible (a channel, a bot, a group
// with topics): the chat then stays exactly as it is upstream.
[[nodiscard]] object_ptr<Ui::RpWidget> CreateBar(
	not_null<QWidget*> parent,
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer);

// "Listen together": what will be sent to the chat, "Start" / "Cancel".
// sendsTrack: the track is not a message of the chat yet, the box says
// that it is sent there as well, that one track and nothing after it.
// onlineNote: "send messages without going online" is switched on, the
// box says that the edits of the control message are not covered by it.
struct StartBoxArgs {
	QString chat;
	QString track;
	bool sendsTrack = false;
	bool onlineNote = false;
	Fn<void()> confirmed;
};
[[nodiscard]] object_ptr<Ui::BoxContent> MakeStartBox(StartBoxArgs &&args);

} // namespace Oblivion::Listen
