/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"

#include <QtCore/QJsonObject>
#include <QtCore/QUrl>

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class Show;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

// Oblivion Cloud: the client of the own server of Oblivion (rooms, shared
// playlists and presets, the badge list, profiles, settings sync, updates).
// The contract is oblivion-cloud/PROTOCOL.md, this file is the only place
// that talks HTTP to that server.
//
// THE RULES (every feature built on this file follows them):
//
//  1. Nothing is sent before the user has agreed for that Telegram
//     account. A feature never checks a setting and then sends a request:
//     it calls RequireConsent() from the click that starts it, or checks
//     For(session).ready() for the background work. Every request with
//     auth fails with Error::Type::NotConnected while the account is not
//     ready, so a mistake here sends nothing.
//  2. Everything lives on the main thread: Account, the callbacks, the
//     rpl producers. Heavy work (hashing, file reads) is done inside on
//     crl::async and comes back by itself.
//  3. A callback is never called after cancel(id), after the Account is
//     destroyed (logout, quit) or after «Отключиться». It can still
//     outlive a widget, so wrap it: crl::guard(widget, [=](...) {...}),
//     or send through a Sender member that cancels in its destructor.
//  4. A failure is quiet: the fail callback gets an Error, the feature
//     shows an inline state or one toast (ShowError()), nothing pops up
//     by itself and nothing is retried in a loop by the caller. GET
//     requests are retried inside with a backoff, the event stream
//     reconnects by itself.
//  5. All playback maths use Now() (the clock of the server), never the
//     local clock.
//  6. What comes from other users through the server is untrusted: names,
//     titles, chat texts, preset data, sticker ids, links. Clamp and
//     validate with the Json* helpers below, never open a link without a
//     click and only an https://t.me/ one.
namespace Oblivion::Cloud {

inline constexpr auto kProtocol = 1;

// The Oblivion build number, compared with "build" of the update manifest
// and sent in X-Oblivion-Client. Raised by hand with every release.
inline constexpr auto kAppBuild = 5000000;
inline constexpr auto kAppVersionStr = "5.0";

inline constexpr auto kMaxStreamRooms = 4;

// ---- Addresses.
//
// The base URL and the pinned certificate are kept in oblivion_cloud.cpp
// (kDefaultBaseUrl, kPinnedCertificate) and nowhere else. The environment
// may override the URL for tests: OBLIVION_CLOUD_URL=http://127.0.0.1:PORT
// together with OBLIVION_CLOUD_INSECURE=1 (plain HTTP is accepted only for
// a loopback address), or an https:// URL that presents the same pinned
// certificate.

[[nodiscard]] QString BaseUrl();

using Query = std::vector<std::pair<QString, QString>>;
using Headers = std::vector<std::pair<QByteArray, QByteArray>>;

// MakeUrl(u"/v1/rooms/ABC/chat"_q, { { u"limit"_q, u"50"_q } }).
[[nodiscard]] QUrl MakeUrl(const QString &path, const Query &query = {});

enum class LinkKind {
	None,
	Room,
	Playlist,
	Preset,
};

struct Link {
	LinkKind kind = LinkKind::None;
	QString id; // A room code (upper case) or a playlist / preset id.

	explicit operator bool() const {
		return (kind != LinkKind::None);
	}
};

// Recognises <base>/r/CODE, <base>/p/ID and <base>/x/ID (with or without
// the scheme, any letter case of a room code). Nothing is requested.
[[nodiscard]] Link ParseLink(const QString &url);
[[nodiscard]] QString MakeLink(LinkKind kind, const QString &id);

// A room code as the server returns it (10 characters, upper case) from
// what the user typed or pasted (a code or a whole link), empty if it is
// not one.
[[nodiscard]] QString NormalizeRoomCode(const QString &text);
[[nodiscard]] bool ValidMediaId(const QString &id); // 64 lower hex.
[[nodiscard]] bool ValidShareId(const QString &id); // 22 of [A-Za-z0-9_-].

// ---- Untrusted JSON.

// An integer from a JSON number or a decimal string, fallback otherwise.
[[nodiscard]] int64 JsonInt(const QJsonValue &value, int64 fallback = 0);
// A Telegram user id (1 .. 2^53 - 1), 0 if the value is not one.
[[nodiscard]] uint64 JsonUserId(const QJsonValue &value);
// A string without control characters, cut to maxLength UTF-16 units. A
// single line one has line breaks and tabs replaced by spaces and is
// trimmed. Not a string: empty.
[[nodiscard]] QString JsonText(
	const QJsonValue &value,
	int maxLength,
	bool singleLine = true);
// "#rrggbb" as a colour, std::nullopt for anything else.
[[nodiscard]] std::optional<QColor> JsonColor(const QJsonValue &value);

// ---- Errors.

struct Error {
	enum class Type {
		None,
		NotConnected, // No consent, switched off or no token: nothing sent.
		Network, // The server could not be reached.
		Timeout,
		Cancelled, // Only for a transfer cancelled by «Отключиться».
		Tls, // The certificate is not the pinned one.
		Protocol, // The answer is not what the protocol promises.
		Http, // The server answered with an error, see status and code.
		File, // A local file could not be read / written / verified.
	};
	Type type = Type::None;
	int status = 0; // HTTP status, 0 if there was no answer.
	QString code; // "rate_limited", "limit_reached", "forbidden"...
	QJsonObject details; // The "error" object: limit, right, offset...
	crl::time retryAfter = 0; // For 429, in milliseconds.

	[[nodiscard]] bool is(const char *value) const {
		return (code == QLatin1String(value));
	}
	[[nodiscard]] QString detail(const char *key) const {
		return details.value(QLatin1String(key)).toString();
	}
	explicit operator bool() const {
		return (type != Type::None);
	}
};

// A short text for the user in the current language, never the English
// "message" of the server. Needs the lang instance (not for self-tests).
[[nodiscard]] QString ErrorText(const Error &error);
// One toast with ErrorText(). Nothing for Error::Type::Cancelled.
void ShowError(std::shared_ptr<Ui::Show> show, const Error &error);

// ---- Requests.

struct Request {
	QByteArray method = "GET";
	QString path; // "/v1/rooms", with the leading slash and "/v1".
	Query query;
	std::optional<QJsonObject> json; // The JSON body.
	std::optional<QByteArray> raw; // Or a raw body (avatar, settings blob).
	QByteArray rawType = "application/octet-stream";
	Headers headers; // If-Match, If-None-Match.
	bool auth = true;
	crl::time timeout = 0; // 0: 20 seconds without any transfer.
	int retries = -1; // -1: 2 for GET / HEAD, 0 for everything else.

	// How many times a request the server has refused with 429 is sent
	// again after the pause the server asks for (retry_after_ms, when it
	// is not longer than ten seconds). Unlike retries this is safe for a
	// POST as well: a refused request was not done. For the routes with a
	// bucket of their own: the queue of a room, PATCH /v1/me.
	int rateRetries = 0;
};

// Shortcuts, so that nobody has to spell the designated initializers:
//   account.request(PostRequest(u"/v1/rooms"_q, { { "title", title } }),
//       crl::guard(this, [=](const Response &r) { ... }),
//       crl::guard(this, [=](const Error &e) { ... }));
[[nodiscard]] Request GetRequest(const QString &path, Query query = {});
[[nodiscard]] Request PostRequest(
	const QString &path,
	QJsonObject json = {});
[[nodiscard]] Request PatchRequest(const QString &path, QJsonObject json);
[[nodiscard]] Request PutRequest(const QString &path, QJsonObject json);
[[nodiscard]] Request PutRawRequest(
	const QString &path,
	QByteArray bytes,
	QByteArray contentType);
[[nodiscard]] Request DeleteRequest(const QString &path);

struct Response {
	int status = 0; // 200, 201, 204, or 304 when If-None-Match matched.
	QJsonObject json; // Empty when the body is not a JSON object.
	QByteArray bytes; // The body as it came.
	QByteArray etag; // The ETag header as it came, with the quotes.
	Headers headers; // Names in lower case.

	[[nodiscard]] QByteArray header(const QByteArray &name) const;
};

using RequestId = int;
using TransferId = int;
using Done = Fn<void(const Response &response)>;
using Fail = Fn<void(const Error &error)>;
using Progress = Fn<void(int64 ready, int64 total)>;

// ---- Objects of the protocol the core parses itself.

struct Me {
	uint64 id = 0;
	QString name;
	int avatarRev = 0;
	bool verified = false;
	bool test = false;
	bool badge = false;
	QString statusText;
	QString statusEmoji;
	QString statusEmojiId;
	QString accent; // "#rrggbb" or empty.
	QString profileAudience = u"nobody"_q; // everyone | chosen | nobody.
	QString activityAudience = u"nobody"_q;
	bool chipListening = false;
	bool chipRoom = false;
	bool chipOnline = false;
	std::vector<uint64> chosen;

	[[nodiscard]] bool valid() const {
		return (id != 0);
	}
};
[[nodiscard]] Me ParseMe(const QJsonObject &user);

struct Hello {
	bool valid = false;
	QString version;
	int protocol = 0;
	int minProtocol = 0;
	QJsonObject features;
	QJsonObject limits;
	QString botUsername; // Empty: no bot verification.
	QString motdRu;
	QString motdEn;
};
[[nodiscard]] Hello ParseHello(const QJsonObject &object);

struct LinkCode {
	QString code; // "K7QM-2XPA", shown as it is.
	int64 expiresAt = 0; // Server time, milliseconds.
};

// One event of the stream. Volatile events (activity, badges, update,
// room.status, room.stroke_live, room.reaction, room.sticker) and the
// service ones (hello, ping, resync, bye) have id 0.
struct Event {
	QString type; // "room.player", "me.updated", ...
	int64 id = 0;
	QString room; // data["room"], upper case; empty for the user scope.
	int64 ts = 0; // Server time of sending.
	QJsonObject data;
};

enum class State {
	NoConsent, // The user has not agreed for this account (the default).
	// Agreed earlier and switched off: by the user (the key is kept), or
	// by itself when the server no longer knows the key (the device was
	// revoked or the account deleted from another device). Nothing is
	// registered again without a click.
	Disconnected,
	// The id is taken: by another device of the user, or by an account
	// that has no device left (the server keeps it reserved after the
	// last one logged out, lastError().details has "reserved" then). A
	// link code is needed, from that device or from the admin.
	NeedsLink,
	Connecting, // Registering or checking the key, the first hello.
	Online, // The event stream is open.
	Offline, // No connection, retried with a backoff by itself.
	UpgradeRequired, // The server no longer speaks this protocol.
	Banned,
};

// ---- Transfers.

struct UploadArgs {
	QString path; // A local file, or
	QByteArray bytes; // the content itself.
	QString kind; // "audio" | "video" | "image".
	QString mime; // "audio/mpeg"...
	Fn<void(const QString &sha256, int64 size)> done;
	Fail fail;
	Progress progress; // Bytes the server has, of the size.
	int chunkSize = 0; // 0: 2 MiB.
};

struct DownloadArgs {
	QString media; // A media id (sha256): GET /v1/media/<id>, or
	QString path; // any other path, like "/v1/updates/files/<name>".
	// The file to create. "<to>.part" is kept for a resume. A file that
	// exists already is taken for the finished download (done at once),
	// so use a name that changes with the content (a hash, a build).
	QString to;
	QString sha256; // Verified after the download (media: always).
	bool auth = true;
	Fn<void(const QString &path)> done;
	Fail fail;
	Progress progress;
};

// The cache of room and playlist media of this installation:
// tdata/oblivion/cloud_media/<sha256>. Trimmed by itself once per launch
// (least recently used first, 3 GiB at most).
[[nodiscard]] QString MediaCachePath(const QString &sha256);

class Account final : public base::has_weak_ptr {
public:
	struct Descriptor {
		uint64 userId = 0;
		QString folder; // Ends with a slash, cloud.json is kept there.
		QByteArray sealKey; // 32 bytes to encrypt the token with, or empty.
		bool available = true; // false: a Telegram test server account.
		// Only the live self-test sets it (from the environment): the
		// server registers the fake test ids only with this key, it is
		// sent with POST /v1/auth/register and with nothing else.
		QByteArray testKey;
	};

	explicit Account(Descriptor &&descriptor);
	~Account();

	[[nodiscard]] uint64 userId() const;
	// false for an account of the Telegram test server: no cloud for it.
	[[nodiscard]] bool available() const;

	[[nodiscard]] State state() const;
	[[nodiscard]] rpl::producer<State> stateValue() const;

	// The user has agreed and the account has a key the server accepts:
	// requests with auth can be sent (they may still fail while offline).
	[[nodiscard]] bool ready() const;
	[[nodiscard]] rpl::producer<bool> readyValue() const;
	[[nodiscard]] bool consented() const;
	// Why the state is Offline / NeedsLink / Banned, for an inline text.
	[[nodiscard]] Error lastError() const;
	// Every failed attempt to connect (the consent flow shows the first).
	[[nodiscard]] rpl::producer<Error> connectFailed() const;

	// The flows of the consent box and of Settings > Oblivion > Oblivion
	// Cloud. Features don't call these, they call RequireConsent().
	void agree(const QString &name); // The consent, then registration.
	void switchOn(); // From Disconnected.
	void switchOff(); // «Отключиться»: the key stays on this device.
	void createLinkCode(Fn<void(const LinkCode &code)> done, Fail fail);
	void redeemLinkCode(const QString &code, Fn<void()> done, Fail fail);
	void deleteData(Fn<void()> done, Fail fail); // «Удалить мои данные».
	[[nodiscard]] QString chosenName() const; // What agree() was given.
	// The logout of Telegram (SessionLoggedOut() calls it): only this
	// device is logged out of the cloud, its key is revoked on the server
	// and removed from the disk. The data of the user stays on the server,
	// deleteData() is the only thing that deletes it.
	void forgetDevice();

	// The own user object as the server has it (cached on disk).
	[[nodiscard]] const Me &me() const;
	[[nodiscard]] rpl::producer<> meUpdated() const;
	// PATCH /v1/me with any subset of name / badge / profile / privacy.
	void patchMe(
		QJsonObject patch,
		Fn<void()> done = nullptr,
		Fail fail = nullptr);
	// For a "user" object a feature got from the server by itself.
	void applyMe(const QJsonObject &user);

	// GET /v1/hello of this launch (refreshed once an hour).
	[[nodiscard]] const Hello &hello() const;
	[[nodiscard]] bool feature(const char *name) const;
	[[nodiscard]] int64 limit(const char *name, int64 fallback) const;

	// Requests. done gets 2xx and 304, fail everything else. Returns 0
	// and calls fail from the event loop when nothing could be sent.
	RequestId request(Request &&request, Done done, Fail fail = nullptr);
	void cancel(RequestId id);

	// Transfers go through their own connections and never starve the
	// requests. At most 4 downloads and 2 uploads run at once, the rest
	// wait in line. A cancelled transfer calls nothing. Downloads of the
	// same file into the same place share one transfer (every caller has
	// an id of its own and is told when it is done), so asking twice for
	// a track that is twice in a queue is fine.
	//
	// An upload that has failed (the server is full in the middle of a
	// file, 507; the connection is gone) leaves what was sent on the
	// server: only cancelTransfer() removes it there. The same file given
	// to upload() again goes on from where it stopped, so «Повторить»
	// after a failure is just another upload().
	TransferId upload(UploadArgs &&args);
	TransferId download(DownloadArgs &&args);
	// A media file into the cache; done at once (from the event loop)
	// when it is there already.
	TransferId downloadMedia(
		const QString &sha256,
		Fn<void(const QString &path)> done,
		Fail fail = nullptr,
		Progress progress = nullptr);
	void cancelTransfer(TransferId id);

	// The event stream: one per account, opened while the account is
	// ready. events() carries everything, service events included.
	[[nodiscard]] rpl::producer<Event> events() const;
	// Events of one room: everything with data["room"] == code, "resync"
	// of that room, "me.room_removed" of that room and the local
	// "room.rejected" (the server did not let this room into the stream:
	// the user is not a member any more).
	[[nodiscard]] rpl::producer<Event> roomEvents(const QString &code) const;
	// While the returned lifetime is alive the room is in the stream,
	// which is what makes the user "online" in it. eventId is "event_id"
	// of the snapshot that was shown (GET /v1/rooms/{code}, /join...):
	// everything after it arrives, nothing twice.
	[[nodiscard]] rpl::lifetime subscribeRoom(
		const QString &code,
		int64 eventId);
	// After "resync" of a room and a reload of its snapshot.
	void setRoomCursor(const QString &code, int64 eventId);

	// Measures the clock offset now (also done by itself after every
	// connect of the stream and every 5 minutes while a room is open).
	void syncTime(Fn<void(bool success)> done = nullptr);

private:
	struct Private;
	const std::unique_ptr<Private> _private;

};

// Requests that die with their owner, like MTP::Sender:
//   Cloud::Sender _cloud; ... _cloud(&Cloud::For(session))
//   _cloud.request(GetRequest(u"/v1/rooms"_q), [=](const Response &r) {});
class Sender final {
public:
	explicit Sender(not_null<Account*> account);
	~Sender();

	RequestId request(Request &&request, Done done, Fail fail = nullptr);
	void cancel(RequestId id);
	void cancelAll();
	[[nodiscard]] Account *account() const;

private:
	const base::weak_ptr<Account> _account;
	std::shared_ptr<base::flat_set<RequestId>> _ids;

};

// The account of a session: created on the first call, destroyed with
// the session. Never null, for a Telegram test server account it stays
// in State::NoConsent forever.
[[nodiscard]] Account &For(not_null<Main::Session*> session);

// Whether some account of this launch has agreed (and is not switched
// off): the background update check is allowed only then.
[[nodiscard]] bool AnyReady();

// ---- The consent gate (oblivion_cloud_ui.cpp).
//
// Every feature calls it from the click that needs the cloud:
//
//   Cloud::RequireConsent(controller, crl::guard(this, [=] {
//       // The account is ready(): send requests.
//   }));
//
// Ready already: done() is called at once. Otherwise the consent box
// (or «Включить снова?», or the link code box) is shown first and done()
// is called after the registration has succeeded. If the user declines
// or the server can't be reached, declined() is called (after a toast
// that says why), done() is not.
void RequireConsent(
	not_null<Window::SessionController*> controller,
	Fn<void()> done,
	Fn<void()> declined = nullptr);
void RequireConsent(
	not_null<Main::Session*> session,
	std::shared_ptr<Ui::Show> show,
	Fn<void()> done,
	Fn<void()> declined = nullptr);

// ---- Time.

// The clock of the server in milliseconds: the local monotonic clock plus
// the measured offset. Before the first measurement it is the local wall
// clock, TimeSynced() tells.
[[nodiscard]] int64 Now();
[[nodiscard]] bool TimeSynced();
[[nodiscard]] rpl::producer<bool> TimeSyncedValue();
// crl::now() at which the server clock shows serverTime, and back.
[[nodiscard]] crl::time LocalTimeFor(int64 serverTime);
[[nodiscard]] int64 ServerTimeFor(crl::time localTime);

// ---- Without an account: only /v1/updates/* (the manifest and builds).
// An explicit click on «Проверить обновления» may use it before any
// consent, the background check only while AnyReady().

RequestId PublicRequest(Request &&request, Done done, Fail fail = nullptr);
void CancelPublicRequest(RequestId id);
TransferId PublicDownload(DownloadArgs &&args);
void CancelPublicDownload(TransferId id);

// ---- Crypto helpers (OpenSSL), for the settings sync and the key file.

// AES-256-GCM: nonce (12) + tag (16) + ciphertext. An empty result means
// a failure (a wrong key size).
[[nodiscard]] QByteArray AesGcmSeal(
	const QByteArray &key,
	const QByteArray &plain,
	const QByteArray &aad = QByteArray());
[[nodiscard]] std::optional<QByteArray> AesGcmOpen(
	const QByteArray &key,
	const QByteArray &sealed,
	const QByteArray &aad = QByteArray());
// PBKDF2-HMAC-SHA256. Slow on purpose: call it on crl::async.
[[nodiscard]] QByteArray SlowKey(
	const QByteArray &password,
	const QByteArray &salt,
	int iterations,
	int length = 32);
[[nodiscard]] QByteArray Sha256(const QByteArray &bytes);
[[nodiscard]] QByteArray RandomBytes(int length);

// ---- Hooks.

// Main::Session: after the session is set up and from finishLogout().
// They call Start() / Forget() of every round 5 module, so the modules
// need no hooks of their own in main_session.cpp.
void SessionStarted(not_null<Main::Session*> session);
void SessionLoggedOut(not_null<Main::Session*> session);

// Core::UiIntegration::handleUrlClick(): a clicked room / playlist /
// preset link is opened in the app (when Oblivion::Get().cloudLinks()).
[[nodiscard]] bool HandleLinkClick(
	const QString &url,
	const QVariant &context);

// OBLIVION_SELFTEST=cloud. Pure logic always; with
// OBLIVION_SELFTEST_CLOUD_LIVE=1 also a round trip against the real
// server with fake test ids (registers, opens a room, exchanges events,
// uploads, downloads, links a device, deletes everything). The server
// registers the fake ids only with its test key: the live part takes it
// from OBLIVION_SELFTEST_CLOUD_TEST_KEY and sends it in the header
// X-Oblivion-Test-Key of its registrations, the key is never a part of
// the app.
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::Cloud
