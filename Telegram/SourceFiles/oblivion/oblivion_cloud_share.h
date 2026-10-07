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

namespace Ui {
class PopupMenu;
class Show;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

// Round 5: sharing. Shared playlists, presets of the photo and the video
// editor, the public gallery. This header has the entry points for the
// other modules. The model (objects of the protocol, the local library of
// presets, the upload, the player of shared tracks) is declared in
// oblivion_cloud_share_ui.h and lives in oblivion_cloud_share.cpp, the
// boxes are in oblivion_cloud_share_ui.cpp.
//
// Nothing here talks to the server before a click, and every click goes
// through Cloud::RequireConsent() first.
namespace Oblivion::Share {

// Called by Cloud::SessionStarted() / Cloud::SessionLoggedOut().
void Start(not_null<Main::Session*> session);
void Forget(not_null<Main::Session*> session);

// A playlist / preset link clicked in a chat (Cloud::HandleLinkClick) or
// an item of somebody's Oblivion profile. Asks the consent if needed and
// requests the object only then: GET /v1/playlists/{id} makes the user
// a follower, so it is never called before a click. id is validated
// (Cloud::ValidShareId). Implemented in oblivion_cloud_share_ui.cpp.
void OpenPlaylistLink(
	not_null<Window::SessionController*> controller,
	const QString &id);
void OpenPresetLink(
	not_null<Window::SessionController*> controller,
	const QString &id);

// «Общие наборы»: kind is "photo", "video" or empty for both.
void ShowGallery(
	not_null<Window::SessionController*> controller,
	const QString &kind = QString());

// «Общие плейлисты»: the playlists the user has published or added.
void ShowLibrary(not_null<Window::SessionController*> controller);

// ---- The hook of the playlists UI (oblivion_playlists.cpp).

// A track of a local Oblivion playlist: a music message of Telegram.
struct LocalTrack {
	uint64 peer = 0; // PeerId::value of the chat.
	int64 msg = 0; // The id of the message.
	uint64 document = 0;
	QString title;
	QString performer;
	QString fileName;
	int duration = 0; // Seconds.
};

// «Поделиться» of a local playlist. Asks the consent and a confirmation,
// then the tracks are taken from Telegram one by one, sent to the media
// relay (what the server has already is not sent again) and the link is
// shown. A playlist that was shared before opens as it is on the server,
// with «Добавить новые треки».
void SharePlaylist(
	not_null<Window::SessionController*> controller,
	uint64 localId,
	const QString &name,
	std::vector<LocalTrack> tracks);

// ---- The hook of the editors (the effect stack of the photo editor,
// the effects box of the video editor).

// What an editor gives to the menu of presets. The stack is the JSON of
// the editor's own serialiser (Photo::SerializeFxStack /
// VideoFx::Serialize), so this header knows nothing about the effects.
struct PresetHost {
	QString kind; // "photo" | "video".
	std::shared_ptr<Ui::Show> show; // Boxes and toasts of the editor.
	// The effects that are in the editor now, empty without them.
	Fn<QByteArray()> current;
	// Puts a stack into the editor (as one undo step). The data is
	// validated already, the editor still applies its own limits.
	Fn<void(const QByteArray &stack)> apply;
	// The menu of the photo editor is dark in every theme.
	bool dark = false;
};

// «Сохранить набор…», «Мои наборы…», «Поделиться набором…», «Общие
// наборы…». The first two are local and need no cloud. The cloud ones
// use the account of the active window and ask its consent.
void FillPresetsMenu(not_null<Ui::PopupMenu*> menu, PresetHost host);
void ShowPresetsMenu(
	not_null<QWidget*> parent,
	QPoint globalPosition,
	PresetHost host);

// OBLIVION_SELFTEST=cloud_share, pure logic, no network.
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::Share
