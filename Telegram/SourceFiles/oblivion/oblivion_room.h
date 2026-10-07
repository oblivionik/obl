/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flags.h"
#include "base/weak_ptr.h"
#include "oblivion/oblivion_cloud.h"

class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class PopupMenu;
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

// Round 5: rooms. The model and the controller of a room (the snapshot,
// the event stream of Cloud::For(session), rights, members, the queues of
// the two players, the room chat, the media relay helpers).
//
// HOW THE OTHER ROOM MODULES PLUG IN (video, canvas, reactions, voice):
//
//  - a tab, an overlay or a header button is registered from the module's
//    own .cpp with the registrars of oblivion_room_window.h, nothing in
//    the files of the rooms has to be edited for that;
//  - everything a tab needs is the Room it is given: state() is always
//    the current snapshot, changes() says what part of it has changed,
//    events() carries every event of the room after it was applied to the
//    state (volatile ones included: room.stroke_live, room.reaction,
//    room.sticker, room.status), reloaded() fires after the snapshot was
//    replaced as a whole (a "resync" of the stream: reload what you keep
//    yourself, the canvas for example);
//  - requests go through Room::send() with a path relative to the room
//    ("/canvas/strokes"), they die with the room and never call back
//    after it; a failure without an own handler becomes one toast of the
//    window;
//  - the time is Room::now() (the clock of the server, Cloud::Now()), the
//    position of a player at that time is Room::position(kind) or
//    PositionAt();
//  - media: Room::addMedia() uploads a file through the relay and puts it
//    into a queue (progress in uploads()), Room::download() brings a file
//    of a queue item into the temp folder of the room (removed when the
//    room is left), Room::localFile() tells where it is;
//  - a module that needs an object living as long as the room is open
//    keeps it in Room::lifetime().
//
// A Room without a session (Descriptor::session == nullptr) is a sample
// for the UI snapshots and the self-tests: it sends nothing, every request
// is dropped, state() is what it was given.
namespace Oblivion::Rooms {

// Called by Cloud::SessionStarted() / Cloud::SessionLoggedOut() for every
// session, no hooks in main_session.cpp are needed. Nothing is sent from
// Start(): a room is opened only by a click.
void Start(not_null<Main::Session*> session);
void Forget(not_null<Main::Session*> session);

// Settings > Oblivion > «Комнаты» and the main menu: the rooms of the
// user, «Создать комнату», «Войти по ссылке». Implemented in
// oblivion_room_window.cpp.
void ShowRoomsBox(not_null<Window::SessionController*> controller);

// An explicit click on «Создать комнату» somewhere else (the listen
// together bar, a friend row): asks the consent if needed, creates the
// room and opens its window. With an empty title the title is asked.
void CreateRoom(
	not_null<Window::SessionController*> controller,
	const QString &title = QString());

// A room link clicked in a chat (Cloud::HandleLinkClick), a code typed by
// the user, «Войти» of an activity chip: asks the consent if needed,
// shows the preview («Войти в комнату?») and joins only on a click.
// code is already normalized (Cloud::NormalizeRoomCode).
void OpenLink(
	not_null<Window::SessionController*> controller,
	const QString &code);

// «В комнату» for the context menu of a music file message (chats, the
// shared media, the Oblivion playlists): shown only while a room window
// of this account is open and the user may add tracks there. The file is
// uploaded to the room by that click.
void AddToRoomAction(
	not_null<Ui::PopupMenu*> menu,
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item);

// The hook of Window::MainMenu: «Комнаты», shown while the rooms are
// switched on in Settings > Oblivion.
void AddMainMenuEntry(
	not_null<Ui::VerticalLayout*> menu,
	not_null<Window::SessionController*> controller);

// «Открыть как комнату» of the listen together bar: a click creates
// a room (with an empty title the title is asked: the name of a chat is
// never sent to the cloud by itself) and opens its window. Nothing is
// uploaded by that click, the track is added in the window.
void OpenAsRoom(
	not_null<Window::SessionController*> controller,
	const QString &title);

// Core::Application hooks: the passcode lock hides the room windows (the
// rooms stay open), the quit closes them before the sessions are gone.
void HideWindowsForLock();
void RestoreWindowsAfterLock();
void CloseAllWindows();

// OBLIVION_SELFTEST=room and =room_sync (the position maths of a player,
// the drift correction), pure logic, no network.
[[nodiscard]] bool RunSelfTest(QStringList &log);
[[nodiscard]] bool RunSyncSelfTest(QStringList &log);

// ---- The model.

enum class Kind {
	Music,
	Video,
};
[[nodiscard]] QString KindName(Kind kind); // "music" | "video".

enum class Right {
	Control, // play / pause / seek / next / previous / select / repeat.
	Queue, // Reorder, remove any item, clear.
	Add, // Add media, remove own items.
	Draw,
	Chat, // Messages, reactions, stickers.
	Invite, // «Скопировать ссылку».
};
inline constexpr auto kRightsCount = 6;
[[nodiscard]] const char *RightName(Right right); // "control", ...

struct Rights {
	bool control = false;
	bool queue = false;
	bool add = false;
	bool draw = false;
	bool chat = false;
	bool invite = false;

	[[nodiscard]] bool has(Right right) const;
	void set(Right right, bool value);

	// The presets of the «Участники» tab.
	[[nodiscard]] static Rights Everything(); // «Все управляют».
	[[nodiscard]] static Rights OnlyOwner(); // «Только я».

	friend inline bool operator==(const Rights &, const Rights &) = default;
};
[[nodiscard]] Rights ParseRights(const QJsonObject &object);
[[nodiscard]] QJsonObject SerializeRights(const Rights &rights);

// What a member has told about the file of the current item.
struct MemberStatus {
	QString itemId; // Empty: nothing was told.
	bool ready = false;
	int buffered = 0; // 0..100.

	friend inline bool operator==(
		const MemberStatus &,
		const MemberStatus &) = default;
};

struct Member {
	uint64 id = 0;
	QString name;
	int avatarRev = 0;
	bool owner = false;
	Rights rights;
	int64 joinedAt = 0;
	bool online = false;
	MemberStatus music;
	MemberStatus video;
};

enum class Repeat {
	Off,
	All,
	One,
};

struct QueueItem {
	QString id;
	Kind kind = Kind::Music;
	QString media; // sha256 of the file.
	int64 size = 0;
	QString mime;
	QString title;
	QString performer;
	int64 duration = 0; // Milliseconds, never 0 for a valid item.
	QString cover; // sha256 of the cover image or empty.
	QString fileName;
	uint64 addedBy = 0;
	int64 addedAt = 0;
};

struct PlayerState {
	QString itemId; // Empty: nothing is current.
	bool playing = false;
	int64 position = 0; // Milliseconds at the moment anchor.
	int64 anchor = 0; // Server time, may lie in the future.
	Repeat repeat = Repeat::Off;
	int64 rev = 0;
	uint64 updatedBy = 0; // 0: the server itself.
	int64 updatedAt = 0;
};

struct Player {
	std::vector<QueueItem> queue;
	PlayerState state;

	[[nodiscard]] const QueueItem *find(const QString &id) const;
	[[nodiscard]] const QueueItem *current() const;
	[[nodiscard]] int indexOf(const QString &id) const; // -1: not there.
};

struct ChatMessage {
	int64 id = 0;
	uint64 userId = 0;
	QString name;
	QString text;
	int64 ts = 0; // Server time, milliseconds.
	int64 replyTo = 0;
	QString clientId;
};

struct Voice {
	QString link; // Only "https://t.me/..." survives the parsing.
	QString title;
	QString chatId;
	uint64 setBy = 0;
	int64 setAt = 0;

	[[nodiscard]] bool valid() const {
		return !link.isEmpty();
	}
};

struct Banned {
	uint64 id = 0;
	QString name;
};

struct RoomSettings {
	int maxMembers = 30;
	bool inviteOnly = false; // settings.join == "invite".
	bool joinFromActivity = false;
	Rights defaults; // Of a newcomer.
};

struct CanvasInfo {
	int width = 1920;
	int height = 1080;
	QString background; // "#rrggbb".
	int count = 0;
	int64 seq = 0;
};

// Why the room is no more for this user.
enum class Gone {
	No,
	Left, // The user has left by himself.
	Kicked,
	Banned,
	Closed, // By the owner.
	Idle, // By the server: nobody was there for six hours.
	Admin,
	Rejected, // The server does not know the user in this room any more.
};

struct RoomState {
	QString code;
	QString link;
	QString title;
	uint64 ownerId = 0;
	int64 createdAt = 0;
	int64 rev = 0;
	RoomSettings settings;
	bool owner = false; // This user is the owner.
	Rights rights; // Of this user (everything for the owner).
	std::vector<Member> members; // In the order of the server.
	std::vector<uint64> invited;
	std::vector<Banned> banned; // Only the owner gets it.
	Player music;
	Player video;
	Voice voice;
	CanvasInfo canvas;
	int64 chatLastId = 0;
	std::vector<ChatMessage> chat; // What is loaded, ascending by id.
	int64 eventId = 0; // Of the snapshot.
	Gone gone = Gone::No;

	[[nodiscard]] bool valid() const {
		return !code.isEmpty();
	}
	[[nodiscard]] const Player &player(Kind kind) const {
		return (kind == Kind::Video) ? video : music;
	}
	[[nodiscard]] Player &player(Kind kind) {
		return (kind == Kind::Video) ? video : music;
	}
	[[nodiscard]] const Member *member(uint64 id) const;
	[[nodiscard]] int onlineCount() const;
};

// What has changed in the state (Room::changes()).
enum class Change : uint32 {
	Room = (1U << 0), // Title, settings, owner, banned, invited.
	Members = (1U << 1), // The list or somebody's rights.
	Presence = (1U << 2),
	Status = (1U << 3), // MemberStatus of somebody.
	Rights = (1U << 4), // The rights of this user or the ownership.
	MusicQueue = (1U << 5),
	MusicPlayer = (1U << 6),
	VideoQueue = (1U << 7),
	VideoPlayer = (1U << 8),
	Chat = (1U << 9),
	Voice = (1U << 10),
	Canvas = (1U << 11), // Strokes: the canvas tab follows events().
	Gone = (1U << 12), // state().gone is set, the room is over.
	Uploads = (1U << 13), // Room::uploads().
	Covers = (1U << 14), // A cover image has come.
	Connection = (1U << 15), // Room::connected().
	Reloaded = (1U << 16), // The whole snapshot was replaced.
};
inline constexpr bool is_flag_type(Change) { return true; }
using Changes = base::flags<Change>;

[[nodiscard]] Changes QueueChange(Kind kind);
[[nodiscard]] Changes PlayerChange(Kind kind);

// ---- Pure logic (self-tested, no session, no network).

// Everything in a snapshot or an event is untrusted: texts are clamped,
// ids validated, what does not fit is dropped.
[[nodiscard]] RoomState ParseRoom(const QJsonObject &room, uint64 selfId);
[[nodiscard]] Member ParseMember(const QJsonObject &member);
[[nodiscard]] QueueItem ParseQueueItem(const QJsonObject &item);
[[nodiscard]] PlayerState ParsePlayerState(const QJsonObject &state);
[[nodiscard]] ChatMessage ParseChatMessage(const QJsonObject &message);

// The reducer: applies one event of the stream (or the local
// "room.rejected") to the state and tells what has changed. A player
// state with a rev that is not newer is ignored. Unknown events change
// nothing.
[[nodiscard]] Changes ApplyEvent(
	RoomState &state,
	const Cloud::Event &event,
	uint64 selfId);

// A player state of an answer of POST .../player/{kind}: taken only if
// its rev is newer. The Room itself does not use it (and never the queue
// of an answer): an answer may overtake the events, the events come in
// the order of the server, so the state of the players and the queues
// change only with them. A tab that wants an instant reaction keeps it in
// its own widgets till the event comes.
[[nodiscard]] Changes ApplyPlayerAnswer(
	RoomState &state,
	Kind kind,
	const QJsonObject &answer);

// The only formula of the position of a player (PROTOCOL.md 8.1).
[[nodiscard]] int64 PositionAt(
	const PlayerState &state,
	int64 duration,
	int64 serverNow);

// What the server plays when the current item is over: the id of the
// item, or empty if the player stops.
[[nodiscard]] QString NextAfterEnd(const Player &player);

[[nodiscard]] bool CanRemoveItem(
	const RoomState &state,
	const QueueItem &item,
	uint64 selfId);

// "before" of POST .../queue/{kind}/move that brings the item with the
// index from to the index to (as in a list after the move). The first:
// whether anything has to be sent; the second: the id or empty (the end).
[[nodiscard]] std::pair<bool, QString> MoveTarget(
	const std::vector<QueueItem> &queue,
	int from,
	int to);

// A room code from what the user has typed or pasted: a code or a link.
[[nodiscard]] QString ExtractCode(const QString &typed);

// "3:07", "1:02:45".
[[nodiscard]] QString FormatDuration(int64 milliseconds);

// ---- The controller of one open room.

// A file that goes through the relay into a queue.
struct AddMedia {
	Kind kind = Kind::Music;
	QString path; // A local file, or
	QByteArray bytes; // the content itself.
	QString title; // Empty: read from the tags or the file name.
	QString performer;
	QString fileName;
	QString mime; // Empty: by the file name.
	int64 duration = 0; // Milliseconds, 0: read from the file.
	QImage cover; // Null: read from the tags (audio).
	bool next = false; // Right after the current item, not to the end.
	bool play = false; // Switch to it at once (needs Right::Control).
};

struct Upload {
	int id = 0;
	Kind kind = Kind::Music;
	QString title;
	int64 ready = 0;
	int64 total = 0;
	bool failed = false;
	QString error; // For the user, when failed.
};

// An item whose file the server has already (a playlist of the cloud).
struct ItemInput {
	QString media;
	QString title;
	QString performer;
	int64 duration = 0;
	QString cover;
	QString fileName;
};

class MusicEngine;

class Room final : public base::has_weak_ptr {
public:
	struct Descriptor {
		Main::Session *session = nullptr; // nullptr: a sample.
		uint64 selfId = 0;
		RoomState state;
		Fn<int64()> now; // nullptr: Cloud::Now().
		bool connected = true; // For a sample.
		std::vector<Upload> uploads; // For a sample.
	};

	explicit Room(Descriptor &&descriptor);
	~Room();

	// The account is going away (logout, the session is destroyed): the
	// engine, the subscriptions and Room::lifetime() are destroyed, the
	// room sends nothing any more. The window is closed after that.
	void detach();

	// nullptr for a sample and after the session is gone.
	[[nodiscard]] Main::Session *session() const;
	[[nodiscard]] Cloud::Account *account() const;
	[[nodiscard]] bool sample() const;
	[[nodiscard]] uint64 selfId() const;
	[[nodiscard]] const QString &code() const;

	[[nodiscard]] const RoomState &state() const;
	[[nodiscard]] rpl::producer<Changes> changes() const;
	[[nodiscard]] rpl::producer<Cloud::Event> events() const;
	[[nodiscard]] rpl::producer<> reloaded() const;
	// A user action has failed and nobody took the error: one toast.
	[[nodiscard]] rpl::producer<Cloud::Error> errors() const;
	// The same for what is not an error of the server («Файл слишком
	// большой»): a ready text for one toast.
	[[nodiscard]] rpl::producer<QString> toasts() const;
	// Things that live while the room is open.
	[[nodiscard]] rpl::lifetime &lifetime();

	// The event stream of the account is open (the room is live).
	[[nodiscard]] bool connected() const;

	[[nodiscard]] bool can(Right right) const;
	[[nodiscard]] bool owner() const;
	[[nodiscard]] const Player &player(Kind kind) const;
	// The clock of the server and the position of a player by it.
	[[nodiscard]] int64 now() const;
	[[nodiscard]] int64 position(Kind kind) const;

	// The synced music player of this device, nullptr for a sample.
	[[nodiscard]] MusicEngine *music() const;

	// A request of this room: path is relative ("/chat", "/canvas/live",
	// "" for the room itself). Never calls back after the room is
	// destroyed or gone. fail == nullptr: the error goes to errors().
	Cloud::RequestId send(
		Cloud::Request &&request,
		Cloud::Done done = nullptr,
		Cloud::Fail fail = nullptr);
	void cancel(Cloud::RequestId id);

	// The players. Every call is one request, the state changes when the
	// event comes (a moment later). play() and select() of one player
	// pause the other one if it plays.
	void play(Kind kind);
	void pause(Kind kind);
	void seek(Kind kind, int64 position);
	void select(Kind kind, const QString &itemId);
	void next(Kind kind);
	void previous(Kind kind);
	void setRepeat(Kind kind, Repeat repeat);

	// The queues.
	void removeItem(Kind kind, const QString &itemId);
	void moveItem(Kind kind, int from, int to);
	void clearQueue(Kind kind);
	// Uploads the file (and its cover) and adds it to the queue. Returns
	// the id of the row in uploads(), the row goes away when it is done.
	int addMedia(AddMedia &&media);
	void cancelUpload(int id);
	[[nodiscard]] const std::vector<Upload> &uploads() const;
	void addUploaded(
		Kind kind,
		std::vector<ItemInput> items,
		bool next = false,
		bool play = false);

	// «Загружает 40 %» for the others, at most one request a second.
	void reportStatus(
		Kind kind,
		const QString &itemId,
		bool ready,
		int buffered);

	// Members (the owner only).
	void setRights(uint64 userId, const Rights &rights);
	void setDefaults(const Rights &rights, bool applyToAll);
	void kick(uint64 userId, bool ban);
	void unban(uint64 userId);
	void transfer(uint64 userId);
	void rename(const QString &title);

	// The room chat.
	void sendChat(const QString &text);
	void deleteChat(int64 id);
	[[nodiscard]] bool chatLoaded() const;

	// «Выйти из комнаты» and «Закрыть комнату для всех». done is called
	// when the server has answered, state().gone is set then.
	void leave(Fn<void()> done = nullptr);
	void closeForAll(Fn<void()> done = nullptr);

	// The files of the queue items. localFile(): where the file is on
	// this device, empty if it is not here yet. download(): brings it
	// into the temp folder of the room, done at once when it is there.
	[[nodiscard]] QString localFile(const QueueItem &item) const;
	Cloud::TransferId download(
		const QueueItem &item,
		Fn<void(const QString &path)> done,
		Cloud::Fail fail = nullptr,
		Cloud::Progress progress = nullptr);
	void cancelDownload(Cloud::TransferId id);
	// The cover of an item: null while it is not here (it is requested
	// then, Change::Covers when it comes).
	[[nodiscard]] QImage cover(const QString &sha256);

private:
	struct Private;
	const std::unique_ptr<Private> _private;

};

// For the window: the temp folder of a room is removed when the room is
// left or over, and the leftovers of old rooms at the first launch.
void RemoveRoomFolder(uint64 userId, const QString &code);
// 0: the folders nobody has touched for half a day; an id: every folder
// of that user (the logout).
void CleanupRoomFolders(uint64 forgetUserId = 0);

} // namespace Oblivion::Rooms
