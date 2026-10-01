/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"

namespace Ui {
class RpWidget;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Oblivion {

struct GhostPreset;

// The ghost mode as the header button sees it: the toggles chosen in
// Oblivion::Get().ghostPreset() (ghostRead, ghostTyping, ghostOnline,
// ghostStories, offlineSend) switched together.
namespace GhostMode {

enum class State : uchar {
	Off, // None of the preset toggles is on.
	Partial, // Some of them are on, for example one enabled in Settings.
	On, // All of them are on.
};

[[nodiscard]] State Current();
[[nodiscard]] rpl::producer<State> Value();

// The button click. Not On: turns on every preset toggle that is off and
// remembers which ones it has turned on (Settings::ghostButtonOwned).
// On: turns off only the remembered ones, so the toggles that were on
// before the first click stay on; with nothing remembered (everything was
// enabled by hand) all the preset toggles are turned off. The server is
// told about a switched "don't update online status" by Api::Updates, as
// for any other way of switching it: "offline" is sent at once when it is
// enabled, "online" when it is disabled in an active window.
// Returns false if there is nothing to switch (an empty preset).
bool Toggle();

// Changes which toggles the button switches. A toggle removed from the
// preset is turned off if the button had turned it on, a toggle added
// while the ghost mode is on is turned on, so the mode stays on.
void SetPreset(GhostPreset preset);

[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace GhostMode

// Oblivion::Get().ghostButton() and its changes, for Dialogs::Widget.
[[nodiscard]] bool GhostButtonShown();
[[nodiscard]] rpl::producer<> GhostButtonShownChanges();

// The ghost mode toggle for the chats list header (Dialogs::Widget shows
// it inside the search field, to the left of the passcode lock button,
// while Oblivion::Get().ghostButton() is on). Left click: GhostMode::Toggle.
// Right click: a menu with the preset toggles, and the ghost mode settings
// if there is a controller. The icon follows GhostMode::Value(): outlined
// in the usual header icon color when off, outlined in the accent color
// when partly on, filled with the accent color when on.
[[nodiscard]] object_ptr<Ui::RpWidget> CreateGhostButton(
	not_null<QWidget*> parent,
	Window::SessionController *controller = nullptr);

// By how much the ghost button stands over the left side of the passcode
// lock button: the lock icon has a wide empty margin there, without the
// overlap the two icons are too far from each other.
[[nodiscard]] int GhostButtonLockOverlap();

} // namespace Oblivion
