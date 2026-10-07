/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "oblivion/oblivion_cloud.h"

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

// Round 5: updates. The manifest of the cloud is checked once a day
// (only while Cloud::AnyReady() and Oblivion::Get().cloudUpdateCheck())
// and on demand; a new version is offered with its notes and a download
// button, nothing is installed silently. Uses Cloud::PublicRequest() and
// Cloud::PublicDownload().
//
// The download goes through the app, not through a browser: the server
// has a self-signed certificate that only Oblivion trusts, a browser
// would show a warning instead of the file. The file is saved to the
// Downloads folder, its SHA-256 is compared with the manifest and the
// user is shown where it is. Installing is the user's own step.
namespace Oblivion::Update {

// The build number and the version of this Oblivion, compared with
// "build" of the manifest (deploy/publish-update.sh <version> <build>).
// The numbers themselves are in ONE place, oblivion_cloud.h
// (Cloud::kAppBuild, Cloud::kAppVersionStr), because the same build goes
// into the X-Oblivion-Client header: raise them there with every release
// and everything that uses these two follows.
inline constexpr auto kOblivionBuild = Cloud::kAppBuild;
inline constexpr auto kOblivionVersion = Cloud::kAppVersionStr;

// Called by Cloud::SessionStarted().
void Start(not_null<Main::Session*> session);

// Settings > Oblivion > «Проверить обновления»: an explicit click, it
// may ask the server before any consent (nothing but the address of the
// user is sent).
void CheckNow(not_null<Window::SessionController*> controller);

// The checks of the manifest, a part of OBLIVION_SELFTEST=cloud_sync.
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::Update
