/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

// Round 5: settings sync between the own devices of the user.
//
// What is sent is one encrypted file: the settings of Oblivion
// (oblivion.json without the values that belong to a device or to the
// cloud connection itself, see kDeviceKeys in the .cpp) and the saved
// presets of the editors. It is compressed and sealed with AES-256-GCM,
// the key is PBKDF2-HMAC-SHA256 of the sync password (600 000 rounds, a
// random salt), everything on this device: the server stores bytes it
// can't read and nothing is ever sent in the clear.
//
// The password is never stored. After a successful «Отправить» or
// «Получить» the derived key is kept on the device (sealed with a key
// made of the local key of the app, in tdata/oblivion/<id>/sync.json),
// so that the next time needs no typing and the automatic mode can work.
//
// Manual: «Отправить» replaces the copy of the server, «Получить»
// replaces the settings of this device; both say what will be replaced
// and ask first. Automatic (Oblivion::Get().cloudSettingsAutoSync(), off
// by default): local changes are sent a little after they are made, a
// newer copy of another device is applied when this device has nothing
// unsent. When both have changed nothing is overwritten: the box asks.
namespace Oblivion::Sync {

// Called by Cloud::SessionStarted() / Cloud::SessionLoggedOut().
void Start(not_null<Main::Session*> session);
void Forget(not_null<Main::Session*> session);

// Settings > Oblivion > «Синхронизация настроек»: the sync password,
// «Отправить», «Получить».
void ShowBox(not_null<Window::SessionController*> controller);

// OBLIVION_SELFTEST=cloud_sync, pure logic, no network. Runs the checks
// of the update manifest (oblivion_cloud_update.h) as well.
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::Sync
