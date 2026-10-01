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

// Icon for the Dock tile and windows, null when the bundle icon is used.
[[nodiscard]] QIcon AppIconOverride();

// Applies the saved icon choice, called once on startup.
void StartAppIcon();

void ShowAppIconBox(not_null<Window::SessionController*> controller);

namespace internal {

// Platform part, see oblivion_app_icon_mac.mm.
[[nodiscard]] QImage ReadIconImage(const QString &path, int maxSide);
[[nodiscard]] QImage BundleIconImage(int size);

} // namespace internal
} // namespace Oblivion
