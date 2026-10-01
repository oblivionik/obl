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

namespace Ui::Menu {
struct MenuCallback;
} // namespace Ui::Menu

namespace Window {
class SessionController;
} // namespace Window

namespace Oblivion {

[[nodiscard]] QString ApplyLocalName(
	not_null<PeerData*> peer,
	const QString &realName);

// Called from ~PeerData, drops the stored real name without any events.
void ForgetPeer(not_null<const PeerData*> peer);

[[nodiscard]] bool HasLocalName(not_null<const PeerData*> peer);
[[nodiscard]] QString OriginalName(not_null<const PeerData*> peer);
[[nodiscard]] QString RealName(not_null<PeerData*> peer);
[[nodiscard]] QString RealShortName(not_null<PeerData*> peer);
[[nodiscard]] rpl::producer<QString> OriginalNameValue(
	not_null<PeerData*> peer);

void SetLocalName(not_null<PeerData*> peer, const QString &name);
void ReapplyLocalName(uint64 peerId);

void ShowLocalNameBox(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer);
void AddLocalNameActions(
	not_null<Window::SessionController*> controller,
	const Dialogs::Key &key,
	const Ui::Menu::MenuCallback &addAction);

} // namespace Oblivion
