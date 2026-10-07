/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class PeerData;

namespace Ui {
class Show;
} // namespace Ui

namespace Ui::Menu {
struct MenuCallback;
} // namespace Ui::Menu

namespace Window {
class SessionController;
} // namespace Window

namespace Oblivion {

void ShowDeletedMessages(
	not_null<Window::SessionController*> controller,
	PeerData *peer);

void AddDeletedMessagesAction(
	not_null<Window::SessionController*> controller,
	PeerData *peer,
	const Ui::Menu::MenuCallback &addAction);

// Opens a file saved by Oblivion (a media copy of a deleted message,
// a saved self-destructing media). Such files keep the name and the
// extension chosen by the sender, so they are opened with the same
// warnings upstream shows for downloaded documents: executables,
// files that may reveal the IP address when opened, etc.
void OpenSavedFile(std::shared_ptr<Ui::Show> show, const QString &path);

// Oblivion round 5, for the search (oblivion_deleted_search.h): what a
// saved media is in a few words ("Photo · name.jpg · 1.2 MB") and one
// saved record shown in full, with its media, the way the list shows it.
struct DeletedMedia;
struct DeletedRecord;
[[nodiscard]] QString DeletedMediaLabel(const DeletedMedia &media);
void ShowDeletedRecord(
	not_null<Window::SessionController*> controller,
	const DeletedRecord &record);

} // namespace Oblivion
