/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"
#include "oblivion/oblivion_cloud.h"

namespace Ui {
class RpWidget;
} // namespace Ui

// The UI of the Oblivion Cloud core: the consent box, the link code
// boxes, the status row and the actions of Settings > Oblivion >
// «Oblivion Cloud». Cloud::RequireConsent(), Cloud::ErrorText() and
// Cloud::ShowError() (declared in oblivion_cloud.h) are implemented here.
namespace Oblivion::Cloud {

// What the status row shows, free of a session (the snapshot scenes
// build it by hand).
struct StatusInfo {
	State state = State::NoConsent;
	QString name; // The name of the user in Oblivion, may be empty.
	bool unavailable = false; // An account of the Telegram test server.
	QString motd; // One line from the admin of the server, or empty.
	// NeedsLink only: the server keeps the account, but no device is
	// linked to it any more (after a logout of Telegram on the last one),
	// so there is no "other device" to get a code from.
	bool reserved = false;
};

[[nodiscard]] rpl::producer<StatusInfo> StatusValue(
	not_null<Account*> account);
[[nodiscard]] QString StatusTitle(const StatusInfo &info);
[[nodiscard]] QString StatusAbout(const StatusInfo &info);

// The row above the buttons of the section: a dot in the colour of the
// state, the state in a word and a line about it.
[[nodiscard]] object_ptr<Ui::RpWidget> CreateStatusRow(
	QWidget *parent,
	rpl::producer<StatusInfo> info);

// The toggle «Oblivion Cloud для этого аккаунта»: its value and its
// click (on: the consent and the registration, off: a question first).
[[nodiscard]] rpl::producer<bool> EnabledValue(
	not_null<Main::Session*> session);
void ToggleFromSettings(not_null<Window::SessionController*> controller);

// «Привязать другое устройство», «Ввести код с другого устройства»,
// «Удалить мои данные с сервера».
void ShowLinkDevice(not_null<Window::SessionController*> controller);
void ShowEnterCode(not_null<Window::SessionController*> controller);
void ShowDeleteData(not_null<Window::SessionController*> controller);

} // namespace Oblivion::Cloud
