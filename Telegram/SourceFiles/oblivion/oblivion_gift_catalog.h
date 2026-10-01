/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Window {
class SessionController;
} // namespace Window

namespace Oblivion {

// Opens the collectible gift catalog loaded from api.changes.tg
// (data by @GiftChanges): gifts, their models, patterns and backdrops.
// gift optionally names a gift (as api.changes.tg names it) to open
// directly, an empty string opens the full list.
void ShowGiftCatalog(
	not_null<Window::SessionController*> controller,
	const QString &gift = QString());

} // namespace Oblivion
