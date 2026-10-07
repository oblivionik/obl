/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"

#include <QtGui/QColor>

class UserData;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class RpWidget;
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Oblivion::Cloud {
struct Error;
struct Me;
} // namespace Oblivion::Cloud

// Round 5: social. The badge list, the directory, Oblivion profiles, the
// activity chips and «Друзья в Oblivion». The model lives in
// oblivion_cloud_social.cpp, the UI in oblivion_cloud_social_ui.cpp.
//
// What is kept and where it comes from:
//
//  - the badge list (GET /v1/badges, ETag): the ids of the users who have
//    switched «Значок Oblivion» on. Oblivion::Badge::Has() looks into it;
//  - the directory (GET /v1/directory, ETag): the people whose profile
//    audience includes this account. Both lists are downloaded as a whole,
//    kept in tdata/oblivion/<id>/cloud_social.json and compared with the
//    chats and the contacts of the account on this device: no id of a
//    contact or of a chat is ever sent to the server;
//  - the activity (GET /v1/activity and the "activity" events): kept in
//    memory only, forgotten when the connection is lost. It is asked again
//    every few minutes and «слушает» nobody has confirmed for longer than
//    the server keeps it is dropped, so a chip never outlives what the
//    server shows by more than that;
//  - GET /v1/users/{id} is sent only after a click (the list of public
//    playlists and presets) and only for an id from the cached directory.
//
// What is public about the own account (the badge, the audiences, the
// chips, the chosen people) belongs to one Telegram account: it is read
// from Cloud::Account::me() and changed by PATCH /v1/me from a click in
// the window of that account. A switch flipped in one account publishes
// nothing about the others. The switches show what the server holds: an
// account that is switched off («Отключиться») keeps its badge and its
// profile on the server, so they are shown as they were left there.
namespace Oblivion::Social {

// Called by Cloud::SessionStarted() / Cloud::SessionLoggedOut().
void Start(not_null<Main::Session*> session);
void Forget(not_null<Main::Session*> session);

// ---- What the server lets this account see.

// An entry of the directory: somebody who has opened the profile to this
// account. Everything in it has come through the server from another
// person: clamped and validated when parsed.
struct Profile {
	uint64 id = 0;
	QString name; // The name in Oblivion, not the one in Telegram.
	int avatarRev = 0;
	bool verified = false;
	bool badge = false;
	QString statusText;
	QString statusEmoji; // One emoji or empty.
	QString statusEmojiId;
	std::optional<QColor> accent;
	int publicPlaylists = 0;
	int publicPresets = 0;

	[[nodiscard]] bool valid() const {
		return (id != 0);
	}
	// Whether the «Oblivion» block has anything to show.
	[[nodiscard]] bool hasContent() const {
		return !statusText.isEmpty()
			|| !statusEmoji.isEmpty()
			|| (publicPlaylists > 0)
			|| (publicPresets > 0);
	}
	friend inline bool operator==(const Profile&, const Profile&) = default;
};

struct Activity {
	uint64 userId = 0;
	bool online = false; // «в Oblivion».
	bool listening = false; // «слушает».
	QString title;
	QString performer;
	int64 durationMs = 0;
	int64 since = 0; // Server time.
	bool inRoom = false; // «в комнате».
	QString roomTitle;
	int roomMembers = 0;
	QString roomCode; // Not empty: the room lets people in, «Войти».

	[[nodiscard]] bool empty() const {
		return !online && !listening && !inRoom;
	}
	friend inline bool operator==(const Activity&, const Activity&) = default;
};

// A playlist or a preset of an owner: what is public in a profile, or the
// own ones in the editor of the profile.
//
// Two things are kept apart here, because they are seen by different
// people:
//  - "shown": listed in the profile of the owner, for the audience of
//    that profile only;
//  - "listed" (presets): in the gallery «Общие наборы», where everybody
//    who uses Oblivion sees the preset together with the name of its
//    owner, whoever the profile is open to.
// For a playlist the server has one flag, "public", and it means the
// profile. For a preset "public" means the gallery. A server that keeps
// a flag of its own for the profile of a preset says so by a boolean
// "profile" in the preset: then profileSwitch is true and the two are
// changed apart. Without it the profile shows exactly the presets of the
// gallery, "shown" only repeats "listed" and can't be changed by itself:
// the switch «Показывать в профиле» never publishes a preset.
struct SharedItem {
	QString id;
	bool playlist = false; // false: a preset of effects.
	QString title;
	QString kind; // A preset: "photo" or "video".
	int count = 0; // A playlist: the number of tracks.
	bool shown = false;
	bool listed = false;
	bool profileSwitch = false;

	friend inline bool operator==(
		const SharedItem&,
		const SharedItem&) = default;
};

enum class Status {
	Off, // No consent, switched off, no key: nothing is asked or shown.
	Loading, // The first answer of this launch is on its way.
	Ready,
	Offline, // The server can't be reached, what was saved is shown.
};

// Cheap lookups in what is kept in memory, main thread, nothing is sent.
// About other people all of them answer "nothing" while the account is
// not connected. The own badge and the own profile are what the server
// has, also for an account that is switched off: they stay public there.
[[nodiscard]] bool BadgeListed(
	not_null<Main::Session*> session,
	uint64 userId);
// For the own id: what the server has about this account.
[[nodiscard]] std::optional<Profile> ProfileOf(
	not_null<Main::Session*> session,
	uint64 userId);
// For the own id: what the audience sees of this account, put together
// here from what was really published («слушает», «в Oblivion»; the room
// chip is made by the server and is not known on this side).
[[nodiscard]] Activity ActivityOf(
	not_null<Main::Session*> session,
	uint64 userId);
[[nodiscard]] std::vector<Profile> Directory(
	not_null<Main::Session*> session);
[[nodiscard]] Status CurrentStatus(not_null<Main::Session*> session);

// Fires from the event loop after the directory, somebody's activity or
// the status could have changed. Ends with the session.
[[nodiscard]] rpl::producer<> Changes(not_null<Main::Session*> session);

// From a click («Друзья в Oblivion» was opened): the directory and the
// activity are asked now, not more often than once in several seconds.
void Refresh(not_null<Main::Session*> session);
// A profile was opened: the activity is asked again (one request without
// any id in it, not more often than once a minute), so the chips of the
// page are what the server shows now.
void RefreshActivity(not_null<Main::Session*> session);

// The public playlists and presets of a person from the directory, asked
// after a click. The callbacks may outlive the widget: guard them. One of
// the two is always called (from the event loop when nothing was sent),
// unless the account is disconnected while the request is on its way:
// then nothing comes back, see Cloud::Account.
void LoadShared(
	not_null<Main::Session*> session,
	uint64 userId,
	Fn<void(std::vector<SharedItem>)> done,
	Fn<void(const Cloud::Error&)> fail);
// The own playlists and presets that are on the server.
void LoadOwnShared(
	not_null<Main::Session*> session,
	Fn<void(std::vector<SharedItem>)> done,
	Fn<void(const Cloud::Error&)> fail);
// «Показывать в профиле». For a preset it is sent only to a server that
// keeps the profile apart from the gallery (item.profileSwitch): it
// never touches "public" of a preset. done gets the item as the server
// has it after the change.
void SetSharedShown(
	not_null<Main::Session*> session,
	const SharedItem &item,
	bool shown,
	Fn<void(SharedItem)> done,
	Fn<void(const Cloud::Error&)> fail);
// «Показывать всем в «Общих наборах»»: "public" of a preset. Switching
// it on is a public statement, the UI asks before it.
void SetPresetListed(
	not_null<Main::Session*> session,
	const SharedItem &item,
	bool listed,
	Fn<void(SharedItem)> done,
	Fn<void(const Cloud::Error&)> fail);

// ---- Leaving.
//
// The server shows the chips of a device only while that device has an
// event stream and forgets them by itself a few seconds after the stream
// is closed (limits.presence_grace_ms of the server, 8 seconds), whatever
// has closed it: «Отключиться», a logout, the quit, a crash, a lost
// connection. That alone ends every chip, and for «в Oblivion» and «в
// комнате» there is nothing else. The track of «слушает» is not left up
// even for those seconds where a request can still be sent: it is taken
// back by one PUT /v1/me/activity {"listening": null} before the stream
// is closed. There are two such places, each with a function of its own
// that is to be called from exactly one spot:
//
//  - StopPublishing(): for the click on «Отключиться», from the
//    confirmation in Cloud::ToggleFromSettings() (oblivion_cloud_ui.cpp),
//    in place of Cloud::Account::switchOff(): the account is switched
//    off from done. done is called once, on the main thread and never
//    from inside this call: when the server has answered, in 1.2 seconds
//    at the latest, on the next turn of the event loop when there is
//    nothing to take back, and at once when the account goes away
//    before that (a logout or the quit: from Forget() or from the
//    destructor of the model, the Cloud::Account is still alive then).
//    Only a session that was logged out before the call gets no done:
//    it has no account to switch off. done must do nothing but switch
//    the account off and tell the user;
//  - IsQuitPrevent(): for Core::Application::readyToQuit(), next to
//    Oblivion::Listen::IsQuitPrevent(), one call for all the accounts.
//    The first call sends the stop of every account that has a track
//    told, and the answer is waited for the way Telegram waits for its
//    own «не в сети» there: the windows are hidden at once, the call is
//    true till the answer has come (1.2 seconds at the latest, the app
//    itself gives a second and a half to everything together), then
//    Core::App().quitPreventFinished() is called and the call is false
//    for good. This is the one place where Oblivion Cloud is waited
//    for on the way out, and only for this one request;
//  - a logout of Telegram can't wait and sends nothing from here: the key
//    of the device is revoked right away, see Cloud::SessionLoggedOut().
//
// Nothing is sent and nothing waits when no track was told and when the
// event stream is not open: a server that can't be reached is never
// waited for, neither by the click nor by the quit (it does not show the
// track of a device without a stream anyway).
void StopPublishing(not_null<Main::Session*> session, Fn<void()> done);
[[nodiscard]] bool IsQuitPrevent();

// ---- What is public about the own account.

enum class Flag {
	Badge, // «Значок Oblivion»: the id is in the public badge list.
	ChipListening,
	ChipRoom,
	ChipOnline,
};
// What the server has, as this device knows it. An account that is
// switched off keeps showing what was left on the server (it is still
// public there); false only for an account that has never agreed or has
// deleted its data.
[[nodiscard]] bool FlagNow(not_null<Main::Session*> session, Flag flag);
[[nodiscard]] rpl::producer<bool> FlagValue(
	not_null<Main::Session*> session,
	Flag flag);
// Switching on asks the consent to Oblivion Cloud first (and, for the
// badge, says in a box what becomes public), switching off needs nothing.
void ToggleFlag(not_null<Window::SessionController*> controller, Flag flag);

enum class Audience {
	Nobody = 0,
	Chosen = 1,
	Everyone = 2,
};
enum class AudienceKind {
	Profile,
	Activity,
};
[[nodiscard]] Audience AudienceNow(
	not_null<Main::Session*> session,
	AudienceKind kind);
[[nodiscard]] rpl::producer<Audience> AudienceValue(
	not_null<Main::Session*> session,
	AudienceKind kind);
[[nodiscard]] rpl::producer<int> ChosenCountValue(
	not_null<Main::Session*> session);
// The account is switched off («Отключиться») while the server still
// shows something of it to other people (the badge, the profile, the
// chips): Settings > Oblivion says so under the switches.
[[nodiscard]] rpl::producer<bool> LeftPublicValue(
	not_null<Main::Session*> session);

// ---- Pure helpers (OBLIVION_SELFTEST=cloud_social checks them).

[[nodiscard]] Audience AudienceFromWire(const QString &value);
[[nodiscard]] QString AudienceToWire(Audience value);
// Whether somebody with this id is in the audience, the way the server
// decides it. The owner is always in.
[[nodiscard]] bool AudienceIncludes(
	Audience audience,
	const std::vector<uint64> &chosen,
	uint64 ownerId,
	uint64 viewerId);

enum class ChipType {
	Listening,
	Room,
	Online,
};
struct Chip {
	ChipType type = ChipType::Online;
	QString text;
	QString joinCode; // Not empty: the chip has «Войти».

	friend inline bool operator==(const Chip&, const Chip&) = default;
};
// The phrases with their placeholders left in: "слушает: {text}",
// "в комнате: {title}".
struct ChipPhrases {
	QString listening;
	QString room;
	QString roomUntitled;
	QString online;
};
// "Кино — Группа крови", or only the title.
[[nodiscard]] QString TrackText(
	const QString &performer,
	const QString &title);
// «слушает», «в комнате», and «в Oblivion» only when there is nothing
// more to tell.
[[nodiscard]] std::vector<Chip> BuildChips(
	const Activity &activity,
	const ChipPhrases &phrases,
	bool allowJoin);
// The order of «Друзья в Oblivion»: who listens goes first, then who is
// in a room, who is in Oblivion, and the rest. Smaller is earlier.
[[nodiscard]] int ActivityRank(const Activity &activity);

// ---- The UI (oblivion_cloud_social_ui.cpp).

// Settings > Oblivion and the main menu.
void ShowFriends(not_null<Window::SessionController*> controller);
void ShowMyProfile(not_null<Window::SessionController*> controller);
void ShowChosenList(not_null<Window::SessionController*> controller);
void ShowAudienceBox(
	not_null<Window::SessionController*> controller,
	AudienceKind kind);
[[nodiscard]] QString AudienceName(Audience audience);
// The text under the switches of Settings > Oblivion > «Профиль и
// видимость»: what they are, and for an account that is switched off
// while something of it is still public, that it is and how to hide it.
[[nodiscard]] rpl::producer<QString> SettingsAboutValue(
	not_null<Main::Session*> session);
// «Убрать старую метку из «О себе»»: says what the old marker is and,
// after a confirmation, calls Oblivion::Badge::RemoveOldMarker().
void ShowRemoveMarker(not_null<Window::SessionController*> controller);

// The hook of Info::Profile::InnerWidget: the activity chips and the
// «Oblivion» block of a user, the first thing under the cover. The
// widget is of zero height while there is nothing to show: no profile
// and no activity is known for the user, the account is not connected to
// Oblivion Cloud, or «Показывать профили и активность Oblivion у других»
// is off. Then "shown" is false, the separator after the block is hidden
// too and the page looks exactly as it does without Oblivion. widget is
// null for a peer that can't have a profile (a bot, a service account).
struct ProfileBlock {
	object_ptr<Ui::RpWidget> widget = { nullptr };
	rpl::producer<bool> shown;
};
[[nodiscard]] ProfileBlock CreateProfileBlock(
	not_null<QWidget*> parent,
	not_null<Window::SessionController*> controller,
	not_null<UserData*> user);

// The hook of Window::MainMenu: «Друзья в Oblivion», shown while it is
// switched on in Settings > Oblivion.
void AddMainMenuEntry(
	not_null<Ui::VerticalLayout*> menu,
	not_null<Window::SessionController*> controller);

// OBLIVION_SELFTEST=cloud_social, pure logic, no network: the parsers,
// the cache of the lists, the audience rules, what is sent for the two
// switches of a shared item, the throttle of the own activity, the book
// of the activity of the others, the texts of the chips, the order of
// the friends.
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::Social
