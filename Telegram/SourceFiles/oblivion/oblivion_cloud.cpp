/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_cloud.h"

#include "base/call_delayed.h"
#include "base/platform/base_platform_info.h"
#include "base/random.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/click_handler_types.h"
#include "main/main_session.h"
#include "mtproto/mtproto_auth_key.h"
#include "oblivion/oblivion_cloud_share.h"
#include "oblivion/oblivion_cloud_social.h"
#include "oblivion/oblivion_cloud_sync.h"
#include "oblivion/oblivion_cloud_update.h"
#include "oblivion/oblivion_room.h"
#include "oblivion/oblivion_send_online.h"
#include "oblivion/oblivion_settings.h"
#include "storage/storage_account.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "settings.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QPointer>
#include <QtCore/QSaveFile>
#include <QtCore/QTimer>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QSslCertificate>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslError>

#include <openssl/evp.h>

#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>

namespace Oblivion::Cloud {
namespace {

// The only place with the address and the certificate of the server. When
// a domain with a public certificate replaces the address, change the URL
// and the certificate here (or drop the pin: IsPinned() and
// PinnedConfiguration() are its only users).
constexpr auto kDefaultBaseUrl = "https://213.108.23.134:8790";
constexpr auto kPinnedDigestHex
	= "96e121e495c0026619df3f8c0dfc46006a4f5fb54d04eaf1494aa6caf7b18b56";
constexpr auto kPinnedCertificate = "-----BEGIN CERTIFICATE-----\n"
	"MIIBxDCCAWugAwIBAgIUDnEf9GU+iFlSus7zGUZLVyN+nhcwCgYIKoZIzj0EAwIw\n"
	"LDEXMBUGA1UEAwwOT2JsaXZpb24gQ2xvdWQxETAPBgNVBAoMCE9ibGl2aW9uMB4X\n"
	"DTI2MTAwNjIyNDMwOVoXDTM2MTAwMzIyNDMwOVowLDEXMBUGA1UEAwwOT2JsaXZp\n"
	"b24gQ2xvdWQxETAPBgNVBAoMCE9ibGl2aW9uMFkwEwYHKoZIzj0CAQYIKoZIzj0D\n"
	"AQcDQgAENoEg6rFwcvX1rpytbxrhdcxrZ7cZ8EIsuQQcbcylcflzxx1LdTqgFP6p\n"
	"U/QyhEBhTzglGT3g1J4DhmJI4eokGqNrMGkwDwYDVR0RBAgwBocE1WwXhjASBgNV\n"
	"HRMBAf8ECDAGAQH/AgEAMA4GA1UdDwEB/wQEAwIChDATBgNVHSUEDDAKBggrBgEF\n"
	"BQcDATAdBgNVHQ4EFgQUhb8UV/z+vET2S9JfZaWCadzRkMswCgYIKoZIzj0EAwID\n"
	"RwAwRAIgIZGwgPFoHtShuRgl+sb5uUibZ/4djq7raRTzzZTKJ5ACIBAFD5DsrAhk\n"
	"QpPzPgXH1tq6VLpJy3ARkewky8sxxXdy\n"
	"-----END CERTIFICATE-----\n";

constexpr auto kUrlVariable = "OBLIVION_CLOUD_URL";
constexpr auto kInsecureVariable = "OBLIVION_CLOUD_INSECURE";
constexpr auto kLiveVariable = "OBLIVION_SELFTEST_CLOUD_LIVE";
constexpr auto kTestKeyVariable = "OBLIVION_SELFTEST_CLOUD_TEST_KEY";
constexpr auto kTestKeyHeader = "X-Oblivion-Test-Key";

constexpr auto kRoomAlphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
constexpr auto kRoomCodeLength = 10;
constexpr auto kShareIdLength = 22;
constexpr auto kMaxUserId = int64(9007199254740991);

constexpr auto kRequestTimeout = crl::time(20'000);
constexpr auto kTransferIdleTimeout = crl::time(60'000);
constexpr auto kRetryCap = crl::time(8'000);
constexpr auto kRetryAfterCap = crl::time(10'000);
constexpr auto kRateRetryMin = crl::time(300);
constexpr auto kRateRetrySpread = crl::time(250);
constexpr auto kPatchMeRateRetries = 2;
constexpr auto kUploadRateRetries = 3;
constexpr auto kConnectBackoffCap = crl::time(300'000);
constexpr auto kStreamBackoffCap = crl::time(300'000);
constexpr auto kStreamBackoffCapInRoom = crl::time(30'000);
constexpr auto kTransferBackoffCap = crl::time(30'000);
constexpr auto kTransferAttempts = 6;
constexpr auto kStreamSilence = crl::time(40'000);
constexpr auto kHeartbeatCheck = crl::time(5'000);
constexpr auto kStreamRestartDelay = crl::time(150);
constexpr auto kStreamReplacedDelay = crl::time(60'000);
constexpr auto kStreamEarlyRetryEach = crl::time(60'000);
constexpr auto kStreamMaxFlaps = 20;
constexpr auto kHelloLifetime = crl::time(3'600'000);
constexpr auto kAuthRecheckEach = crl::time(60'000);
constexpr auto kClockResyncEach = crl::time(300'000);
constexpr auto kClockSamples = 6;
constexpr auto kClockSampleTimeout = crl::time(5'000);
constexpr auto kClockJump = 250.;
constexpr auto kClockSlewPerSecond = 2.;
constexpr auto kClockAhead = int64(1'500);
constexpr auto kClockBehind = int64(5'000);
constexpr auto kClockResyncGap = crl::time(30'000);
constexpr auto kMaxSseLine = 1 << 20;
constexpr auto kMaxSseData = 4 << 20;
constexpr auto kMaxErrorBody = 64 << 10;
constexpr auto kDefaultChunk = 2 << 20;
constexpr auto kHashBlock = 1 << 20;
constexpr auto kMaxDownloads = 4;
constexpr auto kMaxUploads = 2;
constexpr auto kProgressEach = crl::time(100);
constexpr auto kMediaCacheLimit = int64(3) << 30;
constexpr auto kPartLifetime = 3 * 86400;
constexpr auto kRevokeLifetime = 30 * 86400;
constexpr auto kMaxRevokes = 16;
constexpr auto kRevokeRetryDelay = crl::time(60'000);
constexpr auto kRevokeRetries = 5;
constexpr auto kTargetBusyRecheck = crl::time(500);
constexpr auto kPinFailedProperty = "oblivion_pin_failed";

struct Config {
	QString base;
	bool insecure = false;
};

[[nodiscard]] QString StripUrl(const QUrl &url) {
	return url.toString(QUrl::RemovePath
		| QUrl::RemoveQuery
		| QUrl::RemoveFragment
		| QUrl::RemoveUserInfo);
}

[[nodiscard]] Config ConfigFrom(const QString &custom, bool insecure) {
	auto result = Config{ .base = QString::fromLatin1(kDefaultBaseUrl) };
	const auto trimmed = custom.trimmed();
	if (trimmed.isEmpty()) {
		return result;
	}
	const auto url = QUrl(trimmed);
	const auto host = url.host();
	const auto loopback = (host == u"127.0.0.1"_q)
		|| (host == u"localhost"_q)
		|| (host == u"::1"_q);
	if (host.isEmpty()) {
		return result;
	} else if (url.scheme() == u"https"_q) {
		result.base = StripUrl(url);
	} else if (url.scheme() == u"http"_q && insecure && loopback) {
		result.base = StripUrl(url);
		result.insecure = true;
	}
	return result;
}

[[nodiscard]] const Config &CurrentConfig() {
	static const auto result = ConfigFrom(
		qEnvironmentVariable(kUrlVariable),
		(qEnvironmentVariable(kInsecureVariable).trimmed() == u"1"_q));
	return result;
}

[[nodiscard]] QString HostOf(const QString &base) {
	const auto index = base.indexOf(u"://"_q);
	return (index < 0) ? base : base.mid(index + 3);
}

[[nodiscard]] QString CleanRoomCode(const QString &text) {
	const auto upper = text.trimmed().toUpper();
	if (upper.size() != kRoomCodeLength) {
		return QString();
	}
	const auto alphabet = QLatin1String(kRoomAlphabet);
	for (const auto ch : upper) {
		if (!alphabet.contains(ch)) {
			return QString();
		}
	}
	return upper;
}

[[nodiscard]] QByteArray PinnedDigest() {
	return QByteArray::fromHex(kPinnedDigestHex);
}

[[nodiscard]] QByteArray PinnedDer() {
	auto text = QByteArray(kPinnedCertificate);
	text.replace("-----BEGIN CERTIFICATE-----", "");
	text.replace("-----END CERTIFICATE-----", "");
	text.replace("\n", "");
	return QByteArray::fromBase64(text);
}

[[nodiscard]] bool IsPinned(const QSslCertificate &certificate) {
	return !certificate.isNull()
		&& (certificate.digest(QCryptographicHash::Sha256) == PinnedDigest());
}

// What a TLS backend may say about the pinned certificate although it is
// the right one: it is signed by itself, it is not in the system list of
// authorities, it names an address and not a host (Apple also calls a
// certificate that is valid for ten years untrusted). Everything else,
// like an expired or a revoked certificate, stays an error.
[[nodiscard]] bool ExpectedPinError(QSslError::SslError code) {
	switch (code) {
	case QSslError::SelfSignedCertificate:
	case QSslError::SelfSignedCertificateInChain:
	case QSslError::UnableToGetLocalIssuerCertificate:
	case QSslError::UnableToGetIssuerCertificate:
	case QSslError::UnableToVerifyFirstCertificate:
	case QSslError::CertificateUntrusted:
	case QSslError::HostNameMismatch:
		return true;
	default:
		return false;
	}
}

// Whether exactly these errors of a handshake may be ignored: the peer
// shows the pinned certificate and every error is an expected one about
// that very certificate. The errors are then ignored by their list, never
// all at once: an error is equal to another one only together with its
// certificate, so nothing a later handshake with a different certificate
// reports can pass, whoever is (or is not) listening to it.
[[nodiscard]] bool PinnedPeer(
		const QSslCertificate &peer,
		const QList<QSslError> &errors) {
	if (peer.isNull() ? errors.isEmpty() : !IsPinned(peer)) {
		return false;
	}
	for (const auto &error : errors) {
		if (!ExpectedPinError(error.error())
			|| !IsPinned(error.certificate())) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] base::flat_set<int> &PinErrorsSeen() {
	static const auto result = new base::flat_set<int>();
	return *result;
}

// Only the embedded certificate is an anchor, so a certificate of any
// other issuer fails the handshake before a byte of a request is sent.
// What the TLS backend may still dislike in the right one (ten years of
// validity, an address instead of a name) is let through by the digest
// and only for that certificate, see PinnedPeer().
[[nodiscard]] QSslConfiguration PinnedConfiguration() {
	static const auto result = [] {
		auto config = QSslConfiguration::defaultConfiguration();
		config.setCaCertificates(QSslCertificate::fromData(
			QByteArray(kPinnedCertificate),
			QSsl::Pem));
		config.setProtocol(QSsl::TlsV1_2OrLater);
		config.setPeerVerifyMode(QSslSocket::VerifyPeer);
		return config;
	}();
	return result;
}

[[nodiscard]] QString PlatformName() {
	return Platform::IsMac()
		? u"mac"_q
		: Platform::IsWindows()
		? u"win"_q
		: u"linux"_q;
}

[[nodiscard]] QString DeviceName() {
	const auto model = Platform::DeviceModelPretty().simplified();
	const auto name = (model.isEmpty() ? u"Desktop"_q : model)
		+ u" — Oblivion "_q
		+ QString::fromLatin1(kAppVersionStr);
	return name.left(64);
}

[[nodiscard]] QByteArray ClientHeader() {
	return QByteArray(kAppVersionStr) + '/' + PlatformName().toLatin1();
}

// The wall clock is read once, after that only the monotonic clock moves
// the local time: the user may change the system clock at any moment.
[[nodiscard]] int64 LocalWallBase() {
	static const auto result = QDateTime::currentMSecsSinceEpoch()
		- crl::now();
	return result;
}

[[nodiscard]] int64 LocalWallNow() {
	return LocalWallBase() + crl::now();
}

[[nodiscard]] QString MediaCacheFolder() {
	return cWorkingDir() + u"tdata/oblivion/cloud_media/"_q;
}

[[nodiscard]] double RandomJitter() {
	return base::RandomValue<uint32>() / 4294967296.;
}

// 1, 2, 4... seconds up to the cap, each from 75% to 125% of that.
[[nodiscard]] crl::time BackoffDelay(
		int attempt,
		double jitter,
		crl::time cap) {
	const auto shift = std::clamp(attempt - 1, 0, 20);
	const auto base = std::min(crl::time(1000) << shift, cap);
	const auto factor = 0.75 + 0.5 * std::clamp(jitter, 0., 1.);
	return crl::time(std::llround(base * factor));
}

// A stream the server has accepted and that ended before it was 40
// seconds old is a flap. The flaps in a row make the next attempts wait
// longer (a reader that is too slow for a busy room would otherwise come
// back every second for the same megabyte), a stream that lived longer
// ends the row.
[[nodiscard]] int StreamFlaps(int flaps, crl::time lived) {
	return (lived > kStreamSilence)
		? 0
		: std::min(flaps + 1, kStreamMaxFlaps);
}

[[nodiscard]] bool SuccessStatus(int status) {
	return (status >= 200 && status < 300) || (status == 304);
}

[[nodiscard]] bool ValidHeaderValue(const QByteArray &value) {
	return !value.isEmpty() && ranges::all_of(value, [](char ch) {
		return (ch >= 0x20) && (ch < 0x7F);
	});
}

[[nodiscard]] Error ParseHttpError(
		int status,
		const QByteArray &body,
		const QByteArray &retryAfterHeader) {
	auto result = Error{ .type = Error::Type::Http, .status = status };
	const auto document = QJsonDocument::fromJson(body);
	const auto object = document.object().value(u"error"_q).toObject();
	result.code = JsonText(object.value(u"code"_q), 64);
	if (result.code.isEmpty()) {
		result.code = u"http_%1"_q.arg(status);
	}
	result.details = object;
	auto retry = JsonInt(object.value(u"retry_after_ms"_q), -1);
	if (retry < 0) {
		auto ok = false;
		const auto seconds = retryAfterHeader.trimmed().toLongLong(&ok);
		retry = (ok && seconds > 0) ? (seconds * 1000) : 0;
	}
	result.retryAfter = std::clamp(retry, int64(0), int64(3'600'000));
	return result;
}

[[nodiscard]] bool RetryableError(const Error &error) {
	switch (error.type) {
	case Error::Type::Network:
	case Error::Type::Timeout:
		return true;
	case Error::Type::Http:
		return (error.status == 429)
			? (error.retryAfter <= kRetryAfterCap)
			: (error.status == 500
				|| error.status == 502
				|| error.status == 503
				|| error.status == 504);
	default:
		return false;
	}
}

// Request::rateRetries. A request the server has answered with 429 was
// not done, so it may be sent again whatever its method is, after the
// pause the server asks for. 0: it is not sent again (not a 429, the
// repeats are used up, or the server asks to wait for too long: then the
// one who asked decides). The jitter keeps the requests that were refused
// together from coming back together.
[[nodiscard]] crl::time RateRetryDelay(
		const Error &error,
		int attempt,
		int allowed,
		double jitter) {
	if (error.type != Error::Type::Http
		|| error.status != 429
		|| attempt < 0
		|| attempt >= allowed
		|| error.retryAfter > kRetryAfterCap) {
		return 0;
	}
	const auto spread = crl::time(std::llround(
		double(kRateRetrySpread)
			* (attempt + 1)
			* std::clamp(jitter, 0., 1.)));
	return std::max(error.retryAfter, kRateRetryMin) + spread;
}

[[nodiscard]] QString CleanText(QString text, int maxLength, bool singleLine) {
	auto result = QString();
	result.reserve(text.size());
	for (const auto ch : std::as_const(text)) {
		const auto code = ch.unicode();
		const auto breaks = (code == '\n') || (code == '\r') || (code == '\t');
		if (breaks) {
			if (singleLine) {
				result.append(QChar(' '));
			} else if (code != '\r') {
				result.append(ch);
			}
		} else if (code < 0x20
			|| (code >= 0x7F && code < 0xA0)
			|| (code >= 0x202A && code <= 0x202E)
			|| (code >= 0x2066 && code <= 0x2069)
			|| (singleLine && (code == 0x2028 || code == 0x2029))) {
			continue;
		} else {
			result.append(ch);
		}
	}
	if (singleLine) {
		result = result.trimmed();
	}
	if (maxLength >= 0 && result.size() > maxLength) {
		auto cut = maxLength;
		if (cut > 0 && result[cut - 1].isHighSurrogate()) {
			--cut;
		}
		result.truncate(cut);
	}
	return result;
}

struct HashResult {
	bool ok = false;
	QString sha;
	int64 size = 0;
};

[[nodiscard]] HashResult HashFile(
		const QString &path,
		const std::atomic<bool> *cancelled) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	auto buffer = QByteArray(kHashBlock, Qt::Uninitialized);
	auto size = int64(0);
	while (true) {
		if (cancelled && cancelled->load()) {
			return {};
		}
		const auto read = file.read(buffer.data(), buffer.size());
		if (read < 0) {
			return {};
		} else if (!read) {
			break;
		}
		hash.addData(QByteArrayView(buffer.constData(), read));
		size += read;
	}
	return {
		.ok = true,
		.sha = QString::fromLatin1(hash.result().toHex()),
		.size = size,
	};
}

// ---- Server-Sent Events.

class SseParser final {
public:
	struct Frame {
		QByteArray event;
		QByteArray data;
		QByteArray id;
		int retry = -1;
	};

	[[nodiscard]] std::vector<Frame> feed(const QByteArray &chunk);
	[[nodiscard]] bool overflow() const {
		return _overflow;
	}
	void reset();

private:
	void line(QByteArrayView line, std::vector<Frame> &result);

	QByteArray _buffer;
	Frame _current;
	bool _fields = false;
	bool _dataLines = false;
	bool _skipLineFeed = false;
	bool _overflow = false;

};

std::vector<SseParser::Frame> SseParser::feed(const QByteArray &chunk) {
	auto result = std::vector<Frame>();
	if (_overflow) {
		return result;
	}
	_buffer.append(chunk);
	const auto size = int(_buffer.size());
	const auto data = _buffer.constData();
	auto from = 0;
	for (auto i = 0; i != size; ++i) {
		const auto ch = data[i];
		if (ch != '\n' && ch != '\r') {
			continue;
		} else if (ch == '\n' && _skipLineFeed && i == from) {
			_skipLineFeed = false;
			from = i + 1;
			continue;
		}
		_skipLineFeed = (ch == '\r');
		line(QByteArrayView(data + from, i - from), result);
		from = i + 1;
	}
	_buffer.remove(0, from);
	if (_buffer.size() > kMaxSseLine || _current.data.size() > kMaxSseData) {
		_overflow = true;
		_buffer.clear();
	}
	return result;
}

void SseParser::line(QByteArrayView line, std::vector<Frame> &result) {
	if (line.isEmpty()) {
		if (_fields
			&& (!_current.data.isEmpty()
				|| !_current.event.isEmpty()
				|| _current.retry >= 0)) {
			result.push_back(std::move(_current));
		}
		_current = Frame();
		_fields = false;
		_dataLines = false;
		return;
	} else if (line.front() == ':') {
		return;
	}
	const auto colon = line.indexOf(':');
	const auto field = (colon < 0) ? line : line.first(colon);
	auto value = (colon < 0) ? QByteArrayView() : line.sliced(colon + 1);
	if (!value.isEmpty() && value.front() == ' ') {
		value = value.sliced(1);
	}
	_fields = true;
	if (field == QByteArrayView("data")) {
		if (_dataLines) {
			_current.data.append('\n');
		}
		_current.data.append(value.data(), value.size());
		_dataLines = true;
	} else if (field == QByteArrayView("event")) {
		_current.event = value.toByteArray();
	} else if (field == QByteArrayView("id")) {
		if (!value.contains('\0')) {
			_current.id = value.toByteArray();
		}
	} else if (field == QByteArrayView("retry")) {
		auto ok = false;
		const auto number = value.toByteArray().toInt(&ok);
		if (ok && number >= 0) {
			_current.retry = number;
		}
	}
}

void SseParser::reset() {
	_buffer.clear();
	_current = Frame();
	_fields = false;
	_dataLines = false;
	_skipLineFeed = false;
	_overflow = false;
}

[[nodiscard]] std::optional<Event> EventFromFrame(
		const SseParser::Frame &frame) {
	if (frame.data.isEmpty()) {
		return std::nullopt;
	}
	const auto document = QJsonDocument::fromJson(frame.data);
	if (!document.isObject()) {
		return std::nullopt;
	}
	auto result = Event();
	result.type = frame.event.isEmpty()
		? u"message"_q
		: QString::fromUtf8(frame.event).left(64);
	if (!frame.id.isEmpty() && frame.id.size() <= 16) {
		auto ok = false;
		const auto id = frame.id.toLongLong(&ok);
		if (ok && id > 0) {
			result.id = id;
		}
	}
	result.data = document.object();
	result.room = CleanRoomCode(
		result.data.value(u"room"_q).toString().left(32));
	result.ts = JsonInt(result.data.value(u"ts"_q));
	return result;
}

// What was delivered already, per source: the user scope and every room
// have a cursor of their own, because a room joins the stream later with
// the event id of its snapshot, older than what the stream has seen.
// The server sends everything, the replay after a reconnect included, in
// the order of the ids of all its sources. So every event that arrives,
// a new one or a repeated one, tells that nothing older is left for any
// source of this stream and moves all the cursors: a stream that was cut
// in the middle of a long replay continues from that place and does not
// ask for the same backlog again, and a later reconnect asks only for
// what is really new.
class StreamCursor final {
public:
	void setRoomFloor(const QString &code, int64 eventId);
	void removeRoom(const QString &code);
	[[nodiscard]] int64 since(const std::vector<QString> &rooms) const;
	void connected(
		bool withSince,
		int64 helloEventId,
		const std::vector<QString> &rooms);
	[[nodiscard]] bool accept(int64 id, const QString &room);
	[[nodiscard]] bool replaying() const {
		return _replaying;
	}
	void heartbeat();
	void reset();

private:
	void replayDone();

	int64 _user = 0;
	base::flat_map<QString, int64> _rooms;
	std::vector<QString> _streamRooms;
	int64 _helloId = 0;
	bool _replaying = false;

};

void StreamCursor::setRoomFloor(const QString &code, int64 eventId) {
	auto &value = _rooms[code];
	value = std::max(value, eventId);
}

void StreamCursor::removeRoom(const QString &code) {
	_rooms.remove(code);
	_streamRooms.erase(
		ranges::remove(_streamRooms, code),
		end(_streamRooms));
}

int64 StreamCursor::since(const std::vector<QString> &rooms) const {
	auto result = _user;
	for (const auto &code : rooms) {
		const auto i = _rooms.find(code);
		if (i != end(_rooms) && i->second > 0) {
			result = (result > 0) ? std::min(result, i->second) : i->second;
		}
	}
	return result;
}

void StreamCursor::connected(
		bool withSince,
		int64 helloEventId,
		const std::vector<QString> &rooms) {
	_streamRooms = rooms;
	_helloId = helloEventId;
	_replaying = withSince;
	if (!withSince) {
		replayDone();
	}
}

bool StreamCursor::accept(int64 id, const QString &room) {
	if (_replaying && id > _helloId) {
		replayDone();
	}
	auto &cursor = room.isEmpty() ? _user : _rooms[room];
	const auto fresh = (id > cursor);
	cursor = std::max(cursor, id);
	_user = std::max(_user, id);
	for (const auto &code : _streamRooms) {
		auto &value = _rooms[code];
		value = std::max(value, id);
	}
	return fresh;
}

void StreamCursor::heartbeat() {
	if (_replaying) {
		replayDone();
	}
}

void StreamCursor::replayDone() {
	_replaying = false;
	_user = std::max(_user, _helloId);
	for (const auto &code : _streamRooms) {
		auto &value = _rooms[code];
		value = std::max(value, _helloId);
	}
}

void StreamCursor::reset() {
	*this = StreamCursor();
}

// ---- The clock of the server.

struct ClockSample {
	int64 sent = 0;
	int64 received = 0;
	double server = 0.;
};

struct ClockPick {
	bool valid = false;
	double offset = 0.;
	int64 rtt = 0;
};

[[nodiscard]] ClockPick PickClockSample(
		const std::vector<ClockSample> &samples) {
	auto result = ClockPick();
	for (const auto &sample : samples) {
		const auto rtt = sample.received - sample.sent;
		if (rtt < 0 || rtt > kClockSampleTimeout || sample.server <= 0.) {
			continue;
		} else if (!result.valid || rtt < result.rtt) {
			result.valid = true;
			result.rtt = rtt;
			result.offset = sample.server
				- (double(sample.sent) + double(sample.received)) / 2.;
		}
	}
	return result;
}

// The first value and a difference above a quarter of a second are taken
// at once, a smaller one is approached by 2 ms per second, so that a
// player that follows Now() never hears the correction.
class SlewedOffset final {
public:
	void apply(double target, crl::time now);
	[[nodiscard]] double value(crl::time now) const;
	[[nodiscard]] bool valid() const {
		return _valid;
	}

private:
	double _from = 0.;
	double _target = 0.;
	crl::time _since = 0;
	bool _valid = false;

};

void SlewedOffset::apply(double target, crl::time now) {
	const auto current = value(now);
	if (!_valid || std::abs(target - current) > kClockJump) {
		_from = target;
	} else {
		_from = current;
	}
	_target = target;
	_since = now;
	_valid = true;
}

double SlewedOffset::value(crl::time now) const {
	if (!_valid) {
		return 0.;
	}
	const auto elapsed = std::max(now - _since, crl::time(0));
	const auto limit = kClockSlewPerSecond * double(elapsed) / 1000.;
	return _from + std::clamp(_target - _from, -limit, limit);
}

struct ClockState {
	std::mutex mutex;
	SlewedOffset offset;
	crl::time resyncAsked = 0;
	rpl::variable<bool> synced = false;
};

[[nodiscard]] ClockState &Clock() {
	static const auto result = new ClockState();
	return *result;
}

[[nodiscard]] double ClockOffset(crl::time now) {
	auto &clock = Clock();
	const auto lock = std::unique_lock(clock.mutex);
	return clock.offset.value(now);
}

void ClockApply(double offset) {
	auto &clock = Clock();
	{
		const auto lock = std::unique_lock(clock.mutex);
		clock.offset.apply(offset, crl::now());
	}
	clock.synced = true;
}

enum class ClockAdvice {
	Fine,
	Resync,
};

// A timestamp of an event that has just arrived: it can be late (the
// network), but never from the future. If it is, the local monotonic
// clock has stopped for a while (a sleep), so the offset is moved at
// once by what the timestamp proves and measured again.
[[nodiscard]] ClockAdvice ClockHint(int64 ts) {
	auto &clock = Clock();
	if (ts <= 0 || !clock.synced.current()) {
		return ClockAdvice::Fine;
	}
	const auto now = crl::now();
	const auto estimated = int64(std::llround(
		double(LocalWallNow()) + ClockOffset(now)));
	const auto ahead = (ts - estimated > kClockAhead);
	const auto behind = (estimated - ts > kClockBehind);
	if (!ahead && !behind) {
		return ClockAdvice::Fine;
	} else if (ahead) {
		const auto lock = std::unique_lock(clock.mutex);
		clock.offset.apply(double(ts - LocalWallNow()), now);
	}
	if (clock.resyncAsked && (now - clock.resyncAsked < kClockResyncGap)) {
		return ClockAdvice::Fine;
	}
	clock.resyncAsked = now;
	return ClockAdvice::Resync;
}

// ---- The key file of an account.

struct Identity {
	uint64 userId = 0;
	bool consent = false;
	bool disconnected = false;
	bool rejected = false;
	int64 consentAt = 0;
	QString deviceId;
	QString token;

	// The secret this device has made for its registration and has sent
	// (or is about to send) with POST /v1/auth/register: the secret part
	// of the key it will get. It is written to the file before the first
	// attempt and stays till an answer with a key is stored, so that a
	// registration whose answer was lost (a broken connection, the app
	// was closed) is simply sent again, now or at the next launch, and
	// the server gives the same device instead of «уже привязан к другому
	// устройству». Kept as carefully as the key: it is a half of one.
	QString pending;

	QString name;
	QJsonObject me;
};

// 256 random bits as base64url without the padding: exactly 43 characters
// of [A-Za-z0-9_-], the server takes nothing else as "secret".
constexpr auto kDeviceSecretLength = 43;

[[nodiscard]] bool ValidDeviceSecret(const QString &secret) {
	if (secret.size() != kDeviceSecretLength) {
		return false;
	}
	for (const auto ch : secret) {
		const auto code = ch.unicode();
		const auto fine = (code >= '0' && code <= '9')
			|| (code >= 'a' && code <= 'z')
			|| (code >= 'A' && code <= 'Z')
			|| (code == '-')
			|| (code == '_');
		if (!fine) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] QString NewDeviceSecret() {
	const auto result = QString::fromLatin1(RandomBytes(32).toBase64(
		QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));

	// Nothing that is not a full-size random secret is ever sent as one.
	return ValidDeviceSecret(result) ? result : QString();
}

[[nodiscard]] bool ValidToken(const QString &token) {
	if (token.size() < 8 || token.size() > 200 || !token.contains('.')) {
		return false;
	}
	for (const auto ch : token) {
		const auto code = ch.unicode();
		const auto fine = (code >= '0' && code <= '9')
			|| (code >= 'a' && code <= 'z')
			|| (code >= 'A' && code <= 'Z')
			|| (code == '.')
			|| (code == '-')
			|| (code == '_');
		if (!fine) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] QByteArray TokenAad(uint64 userId) {
	return "oblivion-cloud-token:" + QByteArray::number(qulonglong(userId));
}

// Another one, so that what was sealed as a secret that is only on its
// way can't be put into the file as the key, or the other way round.
[[nodiscard]] QByteArray PendingAad(uint64 userId) {
	return "oblivion-cloud-pending:" + QByteArray::number(qulonglong(userId));
}

[[nodiscard]] QByteArray SerializeIdentity(
		const Identity &identity,
		const QByteArray &sealKey) {
	auto object = QJsonObject();
	object.insert(u"v"_q, 1);
	object.insert(u"user_id"_q, QString::number(identity.userId));
	object.insert(u"consent"_q, identity.consent);
	object.insert(u"consent_at"_q, QString::number(identity.consentAt));
	object.insert(u"disconnected"_q, identity.disconnected);
	object.insert(u"rejected"_q, identity.rejected);
	object.insert(u"device_id"_q, identity.deviceId);
	object.insert(u"name"_q, identity.name);
	if (!identity.token.isEmpty()) {
		const auto sealed = (sealKey.size() == 32)
			? AesGcmSeal(
				sealKey,
				identity.token.toLatin1(),
				TokenAad(identity.userId))
			: QByteArray();
		if (!sealed.isEmpty()) {
			object.insert(
				u"token_sealed"_q,
				QString::fromLatin1(sealed.toBase64()));
		} else {
			object.insert(u"token"_q, identity.token);
		}
	}
	if (ValidDeviceSecret(identity.pending)) {
		const auto sealed = (sealKey.size() == 32)
			? AesGcmSeal(
				sealKey,
				identity.pending.toLatin1(),
				PendingAad(identity.userId))
			: QByteArray();
		if (!sealed.isEmpty()) {
			object.insert(
				u"pending_sealed"_q,
				QString::fromLatin1(sealed.toBase64()));
		} else {
			object.insert(u"pending"_q, identity.pending);
		}
	}
	if (!identity.me.isEmpty()) {
		object.insert(u"me"_q, identity.me);
	}
	return QJsonDocument(object).toJson(QJsonDocument::Indented);
}

[[nodiscard]] std::optional<Identity> ParseIdentity(
		const QByteArray &bytes,
		uint64 userId,
		const QByteArray &sealKey) {
	const auto document = QJsonDocument::fromJson(bytes);
	if (!document.isObject()) {
		return std::nullopt;
	}
	const auto object = document.object();
	auto result = Identity();
	result.userId = object.value(u"user_id"_q).toString().toULongLong();
	if (!result.userId || result.userId != userId) {
		return std::nullopt;
	}
	result.consent = object.value(u"consent"_q).toBool();
	result.consentAt = object.value(u"consent_at"_q).toString().toLongLong();
	result.disconnected = object.value(u"disconnected"_q).toBool();
	result.rejected = object.value(u"rejected"_q).toBool();
	result.deviceId = JsonText(object.value(u"device_id"_q), 64);
	result.name = JsonText(object.value(u"name"_q), 64);
	const auto sealed = object.value(u"token_sealed"_q).toString();
	if (!sealed.isEmpty()) {
		const auto opened = AesGcmOpen(
			sealKey,
			QByteArray::fromBase64(sealed.toLatin1()),
			TokenAad(result.userId));
		if (opened) {
			result.token = QString::fromLatin1(*opened);
		}
	} else {
		result.token = object.value(u"token"_q).toString();
	}
	if (!ValidToken(result.token)) {
		result.token = QString();
	}
	const auto pending = object.value(u"pending_sealed"_q).toString();
	if (!pending.isEmpty()) {
		const auto opened = AesGcmOpen(
			sealKey,
			QByteArray::fromBase64(pending.toLatin1()),
			PendingAad(result.userId));
		if (opened) {
			result.pending = QString::fromLatin1(*opened);
		}
	} else {
		result.pending = object.value(u"pending"_q).toString();
	}
	if (!ValidDeviceSecret(result.pending)) {
		// A new one is made for the next registration.
		result.pending = QString();
	}
	result.me = object.value(u"me"_q).toObject();
	return result;
}

[[nodiscard]] QString IdentityPath(const QString &folder) {
	return folder + u"cloud.json"_q;
}

[[nodiscard]] std::optional<Identity> LoadIdentity(
		const QString &folder,
		uint64 userId,
		const QByteArray &sealKey) {
	auto file = QFile(IdentityPath(folder));
	if (!file.open(QIODevice::ReadOnly)) {
		return std::nullopt;
	}
	return ParseIdentity(file.readAll(), userId, sealKey);
}

bool SaveIdentity(
		const QString &folder,
		const Identity &identity,
		const QByteArray &sealKey) {
	if (!QDir().mkpath(folder)) {
		return false;
	}
	auto file = QSaveFile(IdentityPath(folder));
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
	file.write(SerializeIdentity(identity, sealKey));
	return file.commit();
}

[[nodiscard]] bool ParseContentRange(
		const QByteArray &value,
		int64 &from,
		int64 &total) {
	from = -1;
	total = -1;
	const auto trimmed = value.trimmed();
	if (!trimmed.startsWith("bytes ")) {
		return false;
	}
	const auto range = trimmed.mid(6);
	const auto slash = range.indexOf('/');
	if (slash < 0) {
		return false;
	}
	auto ok = false;
	total = range.mid(slash + 1).trimmed().toLongLong(&ok);
	if (!ok || total < 0) {
		total = -1;
		return false;
	}
	const auto span = range.left(slash).trimmed();
	if (span == "*") {
		return true;
	}
	const auto dash = span.indexOf('-');
	if (dash <= 0) {
		return false;
	}
	from = span.left(dash).toLongLong(&ok);
	if (!ok || from < 0) {
		from = -1;
		return false;
	}
	return true;
}

[[nodiscard]] Error ErrorFromReply(
		not_null<QNetworkReply*> reply,
		int status,
		const QByteArray &body) {
	if (reply->property(kPinFailedProperty).toBool()) {
		return { .type = Error::Type::Tls };
	} else if (status > 0 && !SuccessStatus(status)) {
		return ParseHttpError(status, body, reply->rawHeader("Retry-After"));
	}
	const auto error = reply->error();
	if (error == QNetworkReply::SslHandshakeFailedError) {
		return { .type = Error::Type::Tls };
	} else if (error == QNetworkReply::TimeoutError
		|| error == QNetworkReply::OperationCanceledError) {
		return { .type = Error::Type::Timeout };
	}
	return { .type = Error::Type::Network };
}

void DropReply(QPointer<QNetworkReply> &reply, not_null<QObject*> context) {
	if (const auto raw = reply.data()) {
		QObject::disconnect(raw, nullptr, context.get(), nullptr);
		raw->abort();
		raw->deleteLater();
	}
	reply = nullptr;
}

// The files some account of the app writes right now. The accounts share
// the media cache, so two of them may want one file at the same moment:
// the second one waits and then finds the file ready (or loads it itself
// if the first one gave up).
[[nodiscard]] base::flat_set<QString> &BusyTargets() {
	static const auto result = new base::flat_set<QString>();
	return *result;
}

[[nodiscard]] QString TargetKey(const QString &path) {
	const auto clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
	return Platform::IsLinux() ? clean : clean.toLower();
}

// ---- HTTP: requests, uploads and downloads of one account.

class Client final : public base::has_weak_ptr {
public:
	explicit Client(Fn<QByteArray()> token);
	~Client();

	RequestId send(Request &&request, Done done, Fail fail);
	void cancel(RequestId id);

	TransferId upload(UploadArgs &&args);
	TransferId download(DownloadArgs &&args);
	void cancelTransfer(TransferId id);

	void cancelAll();
	void setFailureHook(Fn<void(const Error &error)> hook);
	void setSuccessHook(Fn<void()> hook);
	void failLater(Fail fail, Error error);

	[[nodiscard]] QNetworkRequest prepare(
		const QUrl &url,
		bool auth,
		crl::time timeout) const;
	void watch(not_null<QNetworkReply*> reply) const;
	[[nodiscard]] not_null<QObject*> context() const {
		return _context;
	}
	[[nodiscard]] not_null<QNetworkAccessManager*> manager() const {
		return _manager;
	}
	[[nodiscard]] not_null<QNetworkAccessManager*> streamManager();

private:
	struct Pending {
		Request request;
		Done done;
		Fail fail;
		QPointer<QNetworkReply> reply;
		int attempt = 0;
		int rateAttempt = 0; // Request::rateRetries.
	};
	struct Upload {
		UploadArgs args;
		std::shared_ptr<std::atomic<bool>> cancelled;
		QString sha;
		QString uploadId;
		int64 size = 0;
		int64 offset = 0;
		int64 chunkMax = 0;
		QPointer<QNetworkReply> reply;
		RequestId request = 0;
		crl::time progressAt = 0;
		int attempt = 0;
		int busy = 0;
		bool restarted = false;
		bool started = false;
	};
	// Everybody who asked for the same file gets it from one transfer:
	// two of them writing one "<to>.part" would spoil it for both.
	struct DownloadWaiter {
		TransferId id = 0;
		QString to;
		Fn<void(const QString &path)> done;
		Fail fail;
		Progress progress;
	};
	struct Download {
		DownloadArgs args;
		std::vector<DownloadWaiter> waiters;
		QString key;
		QString urlPath;
		QString part;
		std::unique_ptr<QFile> file;
		std::shared_ptr<std::atomic<bool>> cancelled;
		int64 offset = 0;
		int64 total = 0;
		int64 gained = 0;
		QPointer<QNetworkReply> reply;
		QByteArray errorBody;
		crl::time progressAt = 0;
		int attempt = 0;
		int status = 0;
		bool restarted = false;
		bool started = false;
		bool verifying = false;
		bool claimed = false;
	};

	[[nodiscard]] bool hasToken() const;
	void start(RequestId id);
	void restart(RequestId id);
	void finished(RequestId id, not_null<QNetworkReply*> reply);
	void processQueues();

	[[nodiscard]] Upload *findUpload(TransferId id) const;
	void startUpload(TransferId id);
	void uploadHashed(TransferId id, const HashResult &result);
	void uploadCreate(TransferId id);
	void uploadChunk(TransferId id);
	void uploadSend(
		TransferId id,
		int64 offset,
		const QByteArray &data,
		int64 expected);
	void uploadChunkDone(TransferId id, not_null<QNetworkReply*> reply);
	void uploadRecover(TransferId id, const Error &error);
	void uploadQueryStatus(TransferId id);
	void uploadRestart(TransferId id, const Error &error);
	void uploadFinish(TransferId id);
	void uploadFail(TransferId id, const Error &error);
	std::unique_ptr<Upload> takeUpload(TransferId id);

	[[nodiscard]] Download *findDownload(TransferId id) const;
	void startDownload(TransferId id);
	void openDownload(TransferId id);
	void downloadRead(TransferId id, not_null<QNetworkReply*> reply);
	void downloadDone(TransferId id, not_null<QNetworkReply*> reply);
	void downloadRetry(TransferId id, const Error &error);
	void downloadFromStart(TransferId id, const Error &error);
	void downloadVerify(TransferId id);
	void downloadComplete(TransferId id);
	void downloadFail(TransferId id, const Error &error);
	void downloadProgress(TransferId id, int64 ready, int64 total);
	void downloadFinish(
		std::unique_ptr<Download> task,
		Fn<void(const DownloadWaiter &waiter)> notify);
	[[nodiscard]] bool cancelDownload(TransferId id);
	std::unique_ptr<Download> takeDownload(TransferId id);

	const Fn<QByteArray()> _token;
	const not_null<QObject*> _context;
	const not_null<QNetworkAccessManager*> _manager;
	const not_null<QNetworkAccessManager*> _transfers;
	QNetworkAccessManager *_stream = nullptr;

	base::flat_map<RequestId, Pending> _pending;
	base::flat_map<TransferId, std::unique_ptr<Upload>> _uploads;
	base::flat_map<TransferId, std::unique_ptr<Download>> _downloads;
	base::flat_map<QString, TransferId> _downloadTargets;
	base::flat_map<TransferId, TransferId> _downloadWaiters;
	std::deque<TransferId> _uploadQueue;
	std::deque<TransferId> _downloadQueue;
	Fn<void(const Error &error)> _failureHook;
	Fn<void()> _successHook;
	int _activeUploads = 0;
	int _activeDownloads = 0;
	int _lastId = 0;
	int _generation = 0;

};

Client::Client(Fn<QByteArray()> token)
: _token(std::move(token))
, _context(new QObject())
, _manager(new QNetworkAccessManager())
, _transfers(new QNetworkAccessManager()) {
}

Client::~Client() {
	invalidate_weak_ptrs(this);
	cancelAll();
	_context->deleteLater();
	_manager->deleteLater();
	_transfers->deleteLater();
	if (_stream) {
		_stream->deleteLater();
	}
}

not_null<QNetworkAccessManager*> Client::streamManager() {
	if (!_stream) {
		_stream = new QNetworkAccessManager();
	}
	return _stream;
}

bool Client::hasToken() const {
	return _token && !_token().isEmpty();
}

void Client::setFailureHook(Fn<void(const Error &error)> hook) {
	_failureHook = std::move(hook);
}

void Client::setSuccessHook(Fn<void()> hook) {
	_successHook = std::move(hook);
}

void Client::failLater(Fail fail, Error error) {
	if (!fail) {
		return;
	}
	const auto generation = _generation;
	crl::on_main(this, [=] {
		if (generation == _generation) {
			fail(error);
		}
	});
}

QNetworkRequest Client::prepare(
		const QUrl &url,
		bool auth,
		crl::time timeout) const {
	auto result = QNetworkRequest(url);
	result.setRawHeader(
		"X-Oblivion-Protocol",
		QByteArray::number(kProtocol));
	result.setRawHeader("X-Oblivion-Client", ClientHeader());
	result.setRawHeader(
		"User-Agent",
		QByteArray("Oblivion/") + kAppVersionStr);
	result.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
	result.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::ManualRedirectPolicy);
	result.setAttribute(
		QNetworkRequest::CacheLoadControlAttribute,
		QNetworkRequest::AlwaysNetwork);
	result.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
	result.setAttribute(
		QNetworkRequest::CookieLoadControlAttribute,
		QNetworkRequest::Manual);
	result.setAttribute(
		QNetworkRequest::CookieSaveControlAttribute,
		QNetworkRequest::Manual);
	result.setTransferTimeout(std::chrono::milliseconds(timeout));
	if (url.scheme() == u"https"_q) {
		result.setSslConfiguration(PinnedConfiguration());
	}
	if (auth && _token) {
		const auto token = _token();
		if (!token.isEmpty()) {
			result.setRawHeader("Authorization", "Bearer " + token);
		}
	}
	return result;
}

void Client::watch(not_null<QNetworkReply*> reply) const {
	if (CurrentConfig().insecure) {
		return;
	}
	const auto raw = reply.get();
	QObject::connect(raw, &QNetworkReply::sslErrors, raw, [=](
			const QList<QSslError> &errors) {
		const auto peer = raw->sslConfiguration().peerCertificate();
		if (PinnedPeer(peer, errors)) {
			for (const auto &error : errors) {
				PinErrorsSeen().emplace(int(error.error()));
			}
			raw->ignoreSslErrors(errors);
		} else {
			raw->ignoreSslErrors(QList<QSslError>());
			raw->setProperty(kPinFailedProperty, true);
		}
	});
	QObject::connect(raw, &QNetworkReply::encrypted, raw, [=] {
		if (!IsPinned(raw->sslConfiguration().peerCertificate())) {
			raw->setProperty(kPinFailedProperty, true);
			raw->abort();
		}
	});
}

RequestId Client::send(Request &&request, Done done, Fail fail) {
	if (request.auth && !hasToken()) {
		failLater(std::move(fail), { .type = Error::Type::NotConnected });
		return 0;
	} else if (!request.path.startsWith(u"/v1/"_q)) {
		failLater(std::move(fail), { .type = Error::Type::Protocol });
		return 0;
	}
	const auto id = ++_lastId;
	_pending.emplace(id, Pending{
		.request = std::move(request),
		.done = std::move(done),
		.fail = std::move(fail),
	});
	start(id);
	return id;
}

void Client::start(RequestId id) {
	const auto i = _pending.find(id);
	if (i == end(_pending)) {
		return;
	}
	auto &entry = i->second;
	const auto &data = entry.request;
	auto request = prepare(
		MakeUrl(data.path, data.query),
		data.auth,
		(data.timeout > 0) ? data.timeout : kRequestTimeout);
	for (const auto &[name, value] : data.headers) {
		request.setRawHeader(name, value);
	}
	auto body = QByteArray();
	if (data.json) {
		body = QJsonDocument(*data.json).toJson(QJsonDocument::Compact);
		request.setRawHeader("Content-Type", "application/json");
	} else if (data.raw) {
		body = *data.raw;
		request.setRawHeader("Content-Type", data.rawType);
	}
	const auto method = data.method.toUpper();
	const auto reply = (method == "GET")
		? _manager->get(request)
		: (method == "HEAD")
		? _manager->head(request)
		: _manager->sendCustomRequest(request, method, body);
	entry.reply = reply;
	watch(reply);
	const auto weak = base::make_weak(this);
	QObject::connect(reply, &QNetworkReply::finished, _context.get(), [=] {
		if (const auto that = weak.get()) {
			that->finished(id, reply);
		} else {
			reply->deleteLater();
		}
	});
}

void Client::restart(RequestId id) {
	const auto i = _pending.find(id);
	if (i != end(_pending) && !i->second.reply) {
		start(id);
	}
}

void Client::finished(RequestId id, not_null<QNetworkReply*> reply) {
	reply->deleteLater();
	const auto i = _pending.find(id);
	if (i == end(_pending) || i->second.reply != reply.get()) {
		return;
	}
	const auto status = reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute).toInt();
	auto bytes = reply->readAll();
	if (SuccessStatus(status) && reply->error() == QNetworkReply::NoError) {
		auto response = Response{ .status = status };
		if (bytes.trimmed().startsWith('{')) {
			response.json = QJsonDocument::fromJson(bytes).object();
		}
		response.bytes = std::move(bytes);
		for (const auto &pair : reply->rawHeaderPairs()) {
			response.headers.push_back({ pair.first.toLower(), pair.second });
		}
		response.etag = response.header("etag");
		const auto done = std::move(i->second.done);
		_pending.erase(i);
		if (_successHook) {
			_successHook();
		}
		if (done) {
			done(response);
		}
		return;
	}
	const auto error = ErrorFromReply(reply, status, bytes);
	auto &entry = i->second;
	const auto method = entry.request.method.toUpper();
	const auto retries = (entry.request.retries >= 0)
		? entry.request.retries
		: (method == "GET" || method == "HEAD")
		? 2
		: 0;
	if (RetryableError(error) && entry.attempt < retries) {
		++entry.attempt;
		entry.reply = nullptr;
		const auto delay = std::max(
			BackoffDelay(entry.attempt, RandomJitter(), kRetryCap),
			error.retryAfter);
		base::call_delayed(delay, this, [=] {
			restart(id);
		});
		return;
	}
	const auto rateDelay = RateRetryDelay(
		error,
		entry.rateAttempt,
		entry.request.rateRetries,
		RandomJitter());
	if (rateDelay > 0) {
		// Too often for a route with a bucket of its own: the request was
		// not done, it waits as long as the server asks and goes again.
		++entry.rateAttempt;
		entry.reply = nullptr;
		base::call_delayed(rateDelay, this, [=] {
			restart(id);
		});
		return;
	}
	// The one who asked hears about the failure first: the hook may close
	// the whole account after it (an old protocol, a banned id), and that
	// drops every callback that was not called yet.
	const auto fail = std::move(entry.fail);
	_pending.erase(i);
	const auto generation = _generation;
	const auto weak = base::make_weak(this);
	if (fail) {
		fail(error);
	}
	if (weak && generation == _generation && _failureHook) {
		_failureHook(error);
	}
}

void Client::cancel(RequestId id) {
	const auto i = _pending.find(id);
	if (i == end(_pending)) {
		return;
	}
	DropReply(i->second.reply, _context);
	_pending.erase(i);
}

void Client::cancelAll() {
	++_generation;
	for (auto &[id, entry] : _pending) {
		DropReply(entry.reply, _context);
	}
	_pending.clear();
	for (auto &[id, task] : _uploads) {
		task->cancelled->store(true);
		DropReply(task->reply, _context);
	}
	_uploads.clear();
	for (auto &[id, task] : _downloads) {
		task->cancelled->store(true);
		DropReply(task->reply, _context);
		if (task->claimed) {
			BusyTargets().remove(task->key);
		}
	}
	_downloads.clear();
	_downloadTargets.clear();
	_downloadWaiters.clear();
	_uploadQueue.clear();
	_downloadQueue.clear();
	_activeUploads = 0;
	_activeDownloads = 0;
}

void Client::processQueues() {
	while (_activeUploads < kMaxUploads && !_uploadQueue.empty()) {
		const auto id = _uploadQueue.front();
		_uploadQueue.pop_front();
		if (findUpload(id)) {
			++_activeUploads;
			startUpload(id);
		}
	}
	while (_activeDownloads < kMaxDownloads && !_downloadQueue.empty()) {
		const auto id = _downloadQueue.front();
		_downloadQueue.pop_front();
		if (findDownload(id)) {
			++_activeDownloads;
			startDownload(id);
		}
	}
}

Client::Upload *Client::findUpload(TransferId id) const {
	const auto i = _uploads.find(id);
	return (i != end(_uploads)) ? i->second.get() : nullptr;
}

std::unique_ptr<Client::Upload> Client::takeUpload(TransferId id) {
	const auto i = _uploads.find(id);
	if (i == end(_uploads)) {
		return nullptr;
	}
	auto result = std::move(i->second);
	_uploads.erase(i);
	result->cancelled->store(true);
	DropReply(result->reply, _context);
	if (const auto request = base::take(result->request)) {
		cancel(request);
	}
	if (result->started) {
		--_activeUploads;
	}
	return result;
}

TransferId Client::upload(UploadArgs &&args) {
	if (!hasToken()) {
		failLater(std::move(args.fail), { .type = Error::Type::NotConnected });
		return 0;
	} else if (args.kind.isEmpty()
		|| (args.path.isEmpty() && args.bytes.isEmpty())) {
		failLater(std::move(args.fail), { .type = Error::Type::File });
		return 0;
	}
	const auto id = ++_lastId;
	auto task = std::make_unique<Upload>();
	task->args = std::move(args);
	task->cancelled = std::make_shared<std::atomic<bool>>(false);
	_uploads.emplace(id, std::move(task));
	_uploadQueue.push_back(id);
	processQueues();
	return id;
}

void Client::startUpload(TransferId id) {
	const auto task = findUpload(id);
	if (!task) {
		return;
	}
	task->started = true;
	const auto weak = base::make_weak(this);
	const auto path = task->args.path;
	const auto bytes = task->args.bytes;
	const auto cancelled = task->cancelled;
	crl::async([=] {
		auto result = HashResult();
		if (!path.isEmpty()) {
			result = HashFile(path, cancelled.get());
		} else {
			result.ok = true;
			result.size = bytes.size();
			result.sha = QString::fromLatin1(QCryptographicHash::hash(
				bytes,
				QCryptographicHash::Sha256).toHex());
		}
		crl::on_main(weak, [=] {
			if (const auto that = weak.get()) {
				that->uploadHashed(id, result);
			}
		});
	});
}

void Client::uploadHashed(TransferId id, const HashResult &result) {
	const auto task = findUpload(id);
	if (!task) {
		return;
	} else if (!result.ok || result.size <= 0) {
		uploadFail(id, { .type = Error::Type::File });
		return;
	}
	task->sha = result.sha;
	task->size = result.size;
	uploadCreate(id);
}

void Client::uploadCreate(TransferId id) {
	const auto task = findUpload(id);
	if (!task) {
		return;
	}
	auto body = QJsonObject();
	body.insert(u"sha256"_q, task->sha);
	body.insert(u"size"_q, QJsonValue(qint64(task->size)));
	body.insert(u"kind"_q, task->args.kind);
	body.insert(
		u"mime"_q,
		task->args.mime.isEmpty()
			? u"application/octet-stream"_q
			: task->args.mime);
	auto request = PostRequest(u"/v1/media/uploads"_q, std::move(body));
	request.retries = 2;

	// Declaring has a bucket of its own on the server (1200 at once, then
	// two in a second): "too often" is waited out, a long playlist or a
	// queue of many files does not stop for that.
	request.rateRetries = kUploadRateRetries;
	const auto sent = send(std::move(request), [=](const Response &response) {
		const auto now = findUpload(id);
		if (!now) {
			return;
		}
		now->request = 0;
		const auto status = response.json.value(u"status"_q).toString();
		if (status == u"exists"_q) {
			now->offset = now->size;
			uploadFinish(id);
			return;
		}
		const auto uploadId = response.json.value(u"upload_id"_q).toString();
		const auto offset = JsonInt(response.json.value(u"offset"_q), -1);
		const auto chunkMax = JsonInt(response.json.value(u"chunk_max"_q));
		const auto valid = (status == u"upload"_q)
			&& !uploadId.isEmpty()
			&& (uploadId.size() <= 64)
			&& ranges::all_of(uploadId, [](QChar ch) {
				return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
			})
			&& (offset >= 0)
			&& (offset < now->size);
		if (!valid) {
			uploadFail(id, { .type = Error::Type::Protocol });
			return;
		}
		now->uploadId = uploadId;
		now->offset = offset;
		now->chunkMax = (chunkMax > 0) ? chunkMax : kDefaultChunk;
		uploadChunk(id);
	}, [=](const Error &error) {
		if (const auto now = findUpload(id)) {
			now->request = 0;
			uploadFail(id, error);
		}
	});
	if (const auto now = findUpload(id)) {
		now->request = sent;
	}
}

void Client::uploadChunk(TransferId id) {
	const auto task = findUpload(id);
	if (!task || task->reply) {
		return;
	} else if (task->offset >= task->size) {
		uploadFail(id, { .type = Error::Type::Protocol });
		return;
	}
	const auto wanted = (task->args.chunkSize > 0)
		? int64(task->args.chunkSize)
		: int64(kDefaultChunk);
	const auto length = std::min({
		wanted,
		task->chunkMax,
		task->size - task->offset,
	});
	const auto offset = task->offset;
	if (task->args.path.isEmpty()) {
		uploadSend(id, offset, task->args.bytes.mid(offset, length), length);
		return;
	}
	const auto weak = base::make_weak(this);
	const auto path = task->args.path;
	crl::async([=] {
		auto data = QByteArray();
		auto file = QFile(path);
		if (file.open(QIODevice::ReadOnly) && file.seek(offset)) {
			data = file.read(length);
		}
		crl::on_main(weak, [=] {
			if (const auto that = weak.get()) {
				that->uploadSend(id, offset, data, length);
			}
		});
	});
}

void Client::uploadSend(
		TransferId id,
		int64 offset,
		const QByteArray &data,
		int64 expected) {
	const auto task = findUpload(id);
	if (!task || task->reply || task->offset != offset) {
		return;
	} else if (data.size() != expected) {
		uploadFail(id, { .type = Error::Type::File });
		return;
	}
	auto request = prepare(
		MakeUrl(
			u"/v1/media/uploads/"_q + task->uploadId,
			{ { u"offset"_q, QString::number(offset) } }),
		true,
		kTransferIdleTimeout);
	request.setRawHeader("Content-Type", "application/octet-stream");
	const auto reply = _transfers->put(request, data);
	task->reply = reply;
	watch(reply);
	const auto weak = base::make_weak(this);
	QObject::connect(reply, &QNetworkReply::uploadProgress, _context.get(), [=](
			qint64 sent,
			qint64 total) {
		const auto that = weak.get();
		const auto current = that ? that->findUpload(id) : nullptr;
		if (!current || current->reply != reply || sent <= 0) {
			return;
		}
		const auto now = crl::now();
		if (now - current->progressAt < kProgressEach) {
			return;
		}
		current->progressAt = now;
		if (const auto progress = current->args.progress) {
			progress(
				std::min(offset + int64(sent), current->size),
				current->size);
		}
	});
	QObject::connect(reply, &QNetworkReply::finished, _context.get(), [=] {
		if (const auto that = weak.get()) {
			that->uploadChunkDone(id, reply);
		} else {
			reply->deleteLater();
		}
	});
}

void Client::uploadChunkDone(
		TransferId id,
		not_null<QNetworkReply*> reply) {
	reply->deleteLater();
	const auto task = findUpload(id);
	if (!task || task->reply != reply.get()) {
		return;
	}
	task->reply = nullptr;
	const auto status = reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute).toInt();
	const auto bytes = reply->readAll();
	if (SuccessStatus(status) && reply->error() == QNetworkReply::NoError) {
		const auto json = QJsonDocument::fromJson(bytes).object();
		const auto offset = JsonInt(json.value(u"offset"_q), -1);
		const auto complete = json.value(u"complete"_q).toBool();
		if (offset <= task->offset
			|| offset > task->size
			|| (complete != (offset == task->size))) {
			uploadFail(id, { .type = Error::Type::Protocol });
			return;
		}
		task->offset = offset;
		task->attempt = 0;
		task->busy = 0;
		if (complete) {
			uploadFinish(id);
			return;
		}
		if (const auto progress = task->args.progress) {
			progress(task->offset, task->size);
		}
		uploadChunk(id);
		return;
	}
	const auto error = ErrorFromReply(reply, status, bytes);
	const auto conflict = (error.type == Error::Type::Http)
		&& (error.status == 409);
	if (conflict && error.is("offset_mismatch")) {
		const auto offset = JsonInt(error.details.value(u"offset"_q), -1);
		if (offset >= 0 && offset < task->size && ++task->busy <= 5) {
			task->offset = offset;
			uploadChunk(id);
			return;
		}
	} else if (conflict && error.is("upload_busy") && ++task->busy <= 5) {
		base::call_delayed(crl::time(1500), this, [=] {
			uploadChunk(id);
		});
		return;
	} else if (error.type == Error::Type::Http && error.status == 404) {
		uploadRestart(id, error);
		return;
	} else if (error.type == Error::Type::Network
		|| error.type == Error::Type::Timeout
		|| (error.type == Error::Type::Http
			&& (error.status == 429 || error.status >= 500)
			&& error.status != 507)) {
		uploadRecover(id, error);
		return;
	}
	uploadFail(id, error);
}

void Client::uploadRecover(TransferId id, const Error &error) {
	const auto task = findUpload(id);
	if (!task) {
		return;
	} else if (++task->attempt > kTransferAttempts) {
		uploadFail(id, error);
		return;
	}
	const auto delay = std::max(
		BackoffDelay(task->attempt, RandomJitter(), kTransferBackoffCap),
		std::min(error.retryAfter, kConnectBackoffCap));
	base::call_delayed(delay, this, [=] {
		uploadQueryStatus(id);
	});
}

void Client::uploadQueryStatus(TransferId id) {
	const auto task = findUpload(id);
	if (!task || task->reply || task->request) {
		return;
	}
	auto request = GetRequest(u"/v1/media/uploads/"_q + task->uploadId);
	request.retries = 0;
	const auto sent = send(std::move(request), [=](const Response &response) {
		const auto now = findUpload(id);
		if (!now) {
			return;
		}
		now->request = 0;
		const auto offset = JsonInt(response.json.value(u"offset"_q), -1);
		if (offset < 0 || offset >= now->size) {
			uploadFail(id, { .type = Error::Type::Protocol });
			return;
		}
		now->offset = offset;
		uploadChunk(id);
	}, [=](const Error &error) {
		const auto now = findUpload(id);
		if (!now) {
			return;
		}
		now->request = 0;
		if (error.type == Error::Type::Http && error.status == 404) {
			uploadRestart(id, error);
		} else if (error.type == Error::Type::Http
			&& error.status != 429
			&& error.status < 500) {
			uploadFail(id, error);
		} else {
			uploadRecover(id, error);
		}
	});
	if (const auto now = findUpload(id)) {
		now->request = sent;
	}
}

void Client::uploadRestart(TransferId id, const Error &error) {
	const auto task = findUpload(id);
	if (!task) {
		return;
	} else if (task->restarted) {
		uploadFail(id, error);
		return;
	}
	task->restarted = true;
	task->offset = 0;
	task->uploadId = QString();
	uploadCreate(id);
}

void Client::uploadFinish(TransferId id) {
	const auto task = takeUpload(id);
	if (!task) {
		return;
	}
	const auto weak = base::make_weak(this);
	if (const auto progress = task->args.progress) {
		progress(task->size, task->size);
	}
	if (const auto done = task->args.done) {
		done(task->sha, task->size);
	}
	if (weak) {
		processQueues();
	}
}

void Client::uploadFail(TransferId id, const Error &error) {
	const auto task = takeUpload(id);
	if (!task) {
		return;
	}
	const auto weak = base::make_weak(this);
	if (const auto fail = task->args.fail) {
		fail(error);
	}
	if (weak) {
		processQueues();
	}
}

Client::Download *Client::findDownload(TransferId id) const {
	const auto i = _downloads.find(id);
	return (i != end(_downloads)) ? i->second.get() : nullptr;
}

std::unique_ptr<Client::Download> Client::takeDownload(TransferId id) {
	const auto i = _downloads.find(id);
	if (i == end(_downloads)) {
		return nullptr;
	}
	auto result = std::move(i->second);
	_downloads.erase(i);
	_downloadTargets.remove(result->key);
	if (base::take(result->claimed)) {
		BusyTargets().remove(result->key);
	}
	result->cancelled->store(true);
	DropReply(result->reply, _context);
	result->file = nullptr;
	if (result->started) {
		--_activeDownloads;
	}
	return result;
}

TransferId Client::download(DownloadArgs &&args) {
	auto path = args.path;
	if (!args.media.isEmpty()) {
		path = ValidMediaId(args.media)
			? (u"/v1/media/"_q + args.media)
			: QString();
		args.sha256 = args.media;
	}
	if (args.auth && !hasToken()) {
		failLater(std::move(args.fail), { .type = Error::Type::NotConnected });
		return 0;
	} else if (args.to.isEmpty() || !path.startsWith(u"/v1/"_q)) {
		failLater(std::move(args.fail), { .type = Error::Type::File });
		return 0;
	}
	const auto key = TargetKey(args.to);
	const auto existing = _downloadTargets.find(key);
	const auto shared = (existing != end(_downloadTargets))
		? findDownload(existing->second)
		: nullptr;
	if (shared
		&& (shared->urlPath != path
			|| shared->args.auth != args.auth
			|| shared->args.sha256 != args.sha256)) {
		failLater(std::move(args.fail), { .type = Error::Type::File });
		return 0;
	}
	const auto id = ++_lastId;
	auto waiter = DownloadWaiter{
		.id = id,
		.to = args.to,
		.done = std::move(args.done),
		.fail = std::move(args.fail),
		.progress = std::move(args.progress),
	};
	if (shared) {
		shared->waiters.push_back(std::move(waiter));
		_downloadWaiters.emplace(id, existing->second);
		return id;
	}
	auto task = std::make_unique<Download>();
	task->key = key;
	task->urlPath = path;
	task->part = args.to + u".part"_q;
	task->args = std::move(args);
	task->waiters.push_back(std::move(waiter));
	task->cancelled = std::make_shared<std::atomic<bool>>(false);
	_downloads.emplace(id, std::move(task));
	_downloadTargets[key] = id;
	_downloadWaiters.emplace(id, id);
	_downloadQueue.push_back(id);
	processQueues();
	return id;
}

void Client::startDownload(TransferId id) {
	const auto task = findDownload(id);
	if (!task) {
		return;
	}
	task->started = true;
	if (QFileInfo::exists(task->args.to)) {
		crl::on_main(this, [=] {
			auto taken = takeDownload(id);
			if (!taken) {
				return;
			}
			auto file = QFile(taken->args.to);
			if (file.open(QIODevice::ReadWrite)) {
				file.setFileTime(
					QDateTime::currentDateTime(),
					QFileDevice::FileModificationTime);
				file.close();
			}
			downloadFinish(std::move(taken), [](const DownloadWaiter &waiter) {
				if (waiter.done) {
					waiter.done(waiter.to);
				}
			});
		});
		return;
	} else if (!task->claimed) {
		if (!BusyTargets().emplace(task->key).second) {
			base::call_delayed(kTargetBusyRecheck, this, [=] {
				startDownload(id);
			});
			return;
		}
		task->claimed = true;
	}
	QDir().mkpath(QFileInfo(task->args.to).absolutePath());
	const auto part = QFileInfo(task->part);
	task->offset = part.exists() ? part.size() : 0;
	openDownload(id);
}

void Client::openDownload(TransferId id) {
	const auto task = findDownload(id);
	if (!task || task->reply) {
		return;
	}
	auto request = prepare(
		MakeUrl(task->urlPath),
		task->args.auth,
		kTransferIdleTimeout);
	request.setRawHeader("Accept-Encoding", "identity");
	if (task->offset > 0) {
		request.setRawHeader(
			"Range",
			"bytes=" + QByteArray::number(qlonglong(task->offset)) + '-');
	}
	task->status = 0;
	task->gained = 0;
	task->errorBody.clear();
	task->file = nullptr;
	const auto reply = _transfers->get(request);
	task->reply = reply;
	watch(reply);
	const auto weak = base::make_weak(this);
	QObject::connect(reply, &QNetworkReply::readyRead, _context.get(), [=] {
		if (const auto that = weak.get()) {
			that->downloadRead(id, reply);
		}
	});
	QObject::connect(reply, &QNetworkReply::finished, _context.get(), [=] {
		if (const auto that = weak.get()) {
			that->downloadDone(id, reply);
		} else {
			reply->deleteLater();
		}
	});
}

void Client::downloadRead(TransferId id, not_null<QNetworkReply*> reply) {
	const auto task = findDownload(id);
	if (!task || task->reply != reply.get()) {
		return;
	}
	if (!task->status) {
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (!status) {
			return;
		}
		task->status = status;
		auto mode = QIODevice::OpenMode();
		if (status == 200) {
			task->offset = 0;
			task->total = reply->header(
				QNetworkRequest::ContentLengthHeader).toLongLong();
			mode = QIODevice::WriteOnly | QIODevice::Truncate;
		} else if (status == 206) {
			auto from = int64(-1);
			auto total = int64(-1);
			const auto parsed = ParseContentRange(
				reply->rawHeader("Content-Range"),
				from,
				total);
			if (!parsed || from != task->offset) {
				downloadFromStart(id, { .type = Error::Type::Protocol });
				return;
			}
			task->total = total;
			mode = QIODevice::WriteOnly | QIODevice::Append;
		}
		if (mode) {
			task->file = std::make_unique<QFile>(task->part);
			if (!task->file->open(mode)) {
				downloadFail(id, { .type = Error::Type::File });
				return;
			}
		}
	}
	const auto data = reply->readAll();
	if (data.isEmpty()) {
		return;
	} else if (!task->file) {
		if (task->errorBody.size() < kMaxErrorBody) {
			task->errorBody.append(data);
		}
		return;
	} else if (task->file->write(data) != data.size()) {
		downloadFail(id, { .type = Error::Type::File });
		return;
	}
	task->offset += data.size();
	task->gained += data.size();
	const auto now = crl::now();
	if (now - task->progressAt >= kProgressEach) {
		task->progressAt = now;
		downloadProgress(
			id,
			task->offset,
			std::max(task->total, task->offset));
	}
}

// A progress callback may cancel its own or somebody else's transfer (or
// switch the whole account off), so the list of the waiters is walked by
// their ids and looked up again before every call.
void Client::downloadProgress(TransferId id, int64 ready, int64 total) {
	const auto task = findDownload(id);
	if (!task) {
		return;
	}
	auto ids = std::vector<TransferId>();
	ids.reserve(task->waiters.size());
	for (const auto &waiter : task->waiters) {
		if (waiter.progress) {
			ids.push_back(waiter.id);
		}
	}
	const auto weak = base::make_weak(this);
	for (const auto waiterId : ids) {
		const auto now = findDownload(id);
		if (!now) {
			return;
		}
		const auto i = ranges::find(
			now->waiters,
			waiterId,
			&DownloadWaiter::id);
		if (i == end(now->waiters)) {
			continue;
		}
		const auto progress = i->progress;
		progress(ready, total);
		if (!weak) {
			return;
		}
	}
}

void Client::downloadFinish(
		std::unique_ptr<Download> task,
		Fn<void(const DownloadWaiter &waiter)> notify) {
	if (!task) {
		return;
	}
	const auto weak = base::make_weak(this);
	for (const auto &waiter : task->waiters) {
		if (!_downloadWaiters.remove(waiter.id)) {
			continue;
		}
		notify(waiter);
		if (!weak) {
			return;
		}
	}
	processQueues();
}

void Client::downloadDone(TransferId id, not_null<QNetworkReply*> reply) {
	reply->deleteLater();
	const auto weak = base::make_weak(this);
	downloadRead(id, reply);
	if (!weak) {
		return;
	}
	const auto task = findDownload(id);
	if (!task || task->reply != reply.get()) {
		return;
	}
	task->reply = nullptr;
	const auto status = reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute).toInt();
	const auto failed = (reply->error() != QNetworkReply::NoError);
	if (task->file) {
		task->file->close();
		task->file = nullptr;
	}
	if (reply->property(kPinFailedProperty).toBool()) {
		downloadFail(id, { .type = Error::Type::Tls });
	} else if (status == 416) {
		auto from = int64(-1);
		auto total = int64(-1);
		const auto parsed = ParseContentRange(
			reply->rawHeader("Content-Range"),
			from,
			total);
		if (parsed && total > 0 && QFileInfo(task->part).size() == total) {
			task->offset = task->total = total;
			downloadVerify(id);
		} else {
			downloadFromStart(id, { .type = Error::Type::Protocol });
		}
	} else if (status == 200 || status == 206) {
		const auto complete = !failed
			&& (task->total <= 0 || task->offset == task->total);
		if (complete) {
			downloadVerify(id);
		} else {
			if (task->gained > 0) {
				task->attempt = 0;
			}
			downloadRetry(id, { .type = Error::Type::Network });
		}
	} else if (status > 0) {
		const auto error = ParseHttpError(
			status,
			task->errorBody,
			reply->rawHeader("Retry-After"));
		if (status == 429 || (status >= 500 && status != 507)) {
			downloadRetry(id, error);
		} else {
			downloadFail(id, error);
		}
	} else {
		downloadRetry(id, ErrorFromReply(reply, status, QByteArray()));
	}
}

void Client::downloadRetry(TransferId id, const Error &error) {
	const auto task = findDownload(id);
	if (!task) {
		return;
	} else if (error.type == Error::Type::Tls
		|| ++task->attempt > kTransferAttempts) {
		downloadFail(id, error);
		return;
	}
	const auto delay = std::max(
		BackoffDelay(task->attempt, RandomJitter(), kTransferBackoffCap),
		std::min(error.retryAfter, kConnectBackoffCap));
	base::call_delayed(delay, this, [=] {
		const auto now = findDownload(id);
		if (!now || now->reply || now->verifying) {
			return;
		}
		const auto part = QFileInfo(now->part);
		now->offset = part.exists() ? part.size() : 0;
		openDownload(id);
	});
}

void Client::downloadFromStart(TransferId id, const Error &error) {
	const auto task = findDownload(id);
	if (!task) {
		return;
	}
	DropReply(task->reply, _context);
	task->file = nullptr;
	QFile::remove(task->part);
	if (task->restarted) {
		downloadFail(id, error);
		return;
	}
	task->restarted = true;
	task->verifying = false;
	task->offset = 0;
	task->attempt = 0;
	openDownload(id);
}

void Client::downloadVerify(TransferId id) {
	const auto task = findDownload(id);
	if (!task) {
		return;
	} else if (task->args.sha256.isEmpty()) {
		downloadComplete(id);
		return;
	}
	task->verifying = true;
	const auto weak = base::make_weak(this);
	const auto part = task->part;
	const auto expected = task->args.sha256.toLower();
	const auto cancelled = task->cancelled;
	crl::async([=] {
		const auto result = HashFile(part, cancelled.get());
		crl::on_main(weak, [=] {
			const auto that = weak.get();
			if (!that || !that->findDownload(id)) {
				return;
			} else if (!result.ok) {
				that->downloadFail(id, { .type = Error::Type::File });
			} else if (result.sha != expected) {
				that->downloadFromStart(id, {
					.type = Error::Type::File,
					.code = u"hash_mismatch"_q,
				});
			} else {
				that->downloadComplete(id);
			}
		});
	});
}

void Client::downloadComplete(TransferId id) {
	const auto found = findDownload(id);
	if (!found) {
		return;
	}
	const auto to = found->args.to;
	if (QFileInfo::exists(to)) {
		QFile::remove(to);
	}
	if (!QFile::rename(found->part, to)) {
		downloadFail(id, { .type = Error::Type::File });
		return;
	}
	const auto size = found->offset;
	const auto weak = base::make_weak(this);
	downloadFinish(takeDownload(id), [=](const DownloadWaiter &waiter) {
		if (waiter.progress) {
			waiter.progress(size, size);
		}
		if (weak && waiter.done) {
			waiter.done(waiter.to);
		}
	});
}

void Client::downloadFail(TransferId id, const Error &error) {
	auto task = takeDownload(id);
	if (!task) {
		return;
	}
	const auto copy = error;
	downloadFinish(std::move(task), [=](const DownloadWaiter &waiter) {
		if (waiter.fail) {
			waiter.fail(copy);
		}
	});
}

// A cancelled transfer calls nothing. The file keeps loading while
// somebody else still waits for it.
bool Client::cancelDownload(TransferId id) {
	const auto i = _downloadWaiters.find(id);
	if (i == end(_downloadWaiters)) {
		return false;
	}
	const auto taskId = i->second;
	_downloadWaiters.erase(i);
	if (const auto task = findDownload(taskId)) {
		task->waiters.erase(
			ranges::remove(task->waiters, id, &DownloadWaiter::id),
			end(task->waiters));
		if (task->waiters.empty()) {
			takeDownload(taskId);
		}
	}
	return true;
}

void Client::cancelTransfer(TransferId id) {
	if (const auto task = takeUpload(id)) {
		if (!task->uploadId.isEmpty() && hasToken()) {
			send(
				DeleteRequest(u"/v1/media/uploads/"_q + task->uploadId),
				nullptr,
				nullptr);
		}
		processQueues();
	} else if (cancelDownload(id)) {
		processQueues();
	}
}

[[nodiscard]] Client &PublicClient() {
	static const auto result = new Client(nullptr);
	return *result;
}

// ---- Keys of accounts that logged out of Telegram. The logout of
// Telegram logs only this device out of the cloud: its key is revoked on
// the server (POST /v1/auth/logout) and removed from the disk, so that
// no working key outlives the Telegram session. Nothing the server keeps
// about the user is deleted by that, only «Удалить мои данные с сервера»
// deletes. What a later registration of the same id gets is decided by
// the server (PROTOCOL.md, "Devices"). If the server can't be reached the
// revocation is tried again a few times while the app runs and at the
// next launches, for 30 days.

struct RevokeEntry {
	QString token;
	int64 added = 0;
};

[[nodiscard]] QString RevokePath() {
	return cWorkingDir() + u"tdata/oblivion/cloud_revoke.json"_q;
}

[[nodiscard]] std::vector<RevokeEntry> ReadRevokes() {
	auto result = std::vector<RevokeEntry>();
	auto file = QFile(RevokePath());
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	const auto list = QJsonDocument::fromJson(file.readAll()).array();
	for (const auto &value : list) {
		const auto object = value.toObject();
		auto entry = RevokeEntry{
			.token = object.value(u"token"_q).toString(),
			.added = JsonInt(object.value(u"added"_q)),
		};
		if (ValidToken(entry.token)) {
			result.push_back(std::move(entry));
		}
	}
	return result;
}

void WriteRevokes(const std::vector<RevokeEntry> &list) {
	if (list.empty()) {
		QFile::remove(RevokePath());
		return;
	}
	auto array = QJsonArray();
	for (const auto &entry : list) {
		auto object = QJsonObject();
		object.insert(u"token"_q, entry.token);
		object.insert(u"added"_q, QJsonValue(qint64(entry.added)));
		array.push_back(object);
	}
	QDir().mkpath(QFileInfo(RevokePath()).absolutePath());
	auto file = QSaveFile(RevokePath());
	if (file.open(QIODevice::WriteOnly)) {
		file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
		file.write(QJsonDocument(array).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

void ProcessRevokes();

void RetryRevokesLater() {
	static auto Attempts = 0;
	if (Attempts >= kRevokeRetries) {
		return;
	}
	const auto delay = kRevokeRetryDelay << Attempts++;
	base::call_delayed(delay, &PublicClient(), [] {
		ProcessRevokes();
	});
}

void ProcessRevokes() {
	static auto Running = false;
	if (Running) {
		return;
	}
	auto list = ReadRevokes();
	const auto count = list.size();
	const auto now = int64(base::unixtime::now());
	list.erase(
		ranges::remove_if(list, [&](const RevokeEntry &entry) {
			return (now - entry.added > kRevokeLifetime);
		}),
		end(list));
	if (list.size() != count) {
		WriteRevokes(list);
	}
	if (list.empty()) {
		return;
	}
	Running = true;
	const auto token = list.front().token;
	const auto finish = [=](bool revoked) {
		Running = false;
		if (!revoked) {
			RetryRevokesLater();
			return;
		}
		auto left = ReadRevokes();
		left.erase(
			ranges::remove_if(left, [&](const RevokeEntry &entry) {
				return (entry.token == token);
			}),
			end(left));
		WriteRevokes(left);
		ProcessRevokes();
	};
	auto request = PostRequest(u"/v1/auth/logout"_q);
	request.auth = false;
	request.headers.push_back({
		"Authorization",
		"Bearer " + token.toLatin1(),
	});
	PublicClient().send(std::move(request), [=](const Response &) {
		finish(true);
	}, [=](const Error &error) {
		finish((error.type == Error::Type::Http)
			&& (error.status == 401
				|| error.status == 403
				|| error.status == 404));
	});
}

void RevokeLater(const QString &token) {
	if (!ValidToken(token)) {
		return;
	}
	auto list = ReadRevokes();
	list.push_back({ .token = token, .added = base::unixtime::now() });
	while (int(list.size()) > kMaxRevokes) {
		list.erase(begin(list));
	}
	WriteRevokes(list);
	ProcessRevokes();
}

void TrimMediaCache() {
	static auto Done = false;
	if (Done) {
		return;
	}
	Done = true;
	const auto folder = MediaCacheFolder();
	crl::async([=] {
		auto dir = QDir(folder);
		if (!dir.exists()) {
			return;
		}
		auto files = dir.entryInfoList(QDir::Files, QDir::Time | QDir::Reversed);
		const auto now = QDateTime::currentDateTime();
		auto total = int64(0);
		for (auto i = files.begin(); i != files.end();) {
			const auto stale = i->fileName().endsWith(u".part"_q)
				&& (i->lastModified().secsTo(now) > kPartLifetime);
			if (stale) {
				QFile::remove(i->absoluteFilePath());
				i = files.erase(i);
			} else {
				total += i->size();
				++i;
			}
		}
		for (const auto &info : std::as_const(files)) {
			if (total <= kMediaCacheLimit) {
				break;
			} else if (QFile::remove(info.absoluteFilePath())) {
				total -= info.size();
			}
		}
	});
}

[[nodiscard]] QString AccountFolder(not_null<Main::Session*> session) {
	return cWorkingDir()
		+ u"tdata/oblivion/"_q
		+ (session->isTestMode() ? u"test_"_q : QString())
		+ QString::number(session->userId().bare)
		+ '/';
}

// The key of an account is encrypted with a key made of the local key of
// the app, so it is as hard to read as the Telegram session itself.
[[nodiscard]] QByteArray SealKeyFor(not_null<Main::Session*> session) {
	const auto key = session->local().peekLegacyLocalKey();
	if (!key) {
		return QByteArray();
	}
	const auto data = key->data();
	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	hash.addData(QByteArray("oblivion cloud identity"));
	hash.addData(QByteArray::fromRawData(
		reinterpret_cast<const char*>(data.data()),
		qsizetype(data.size())));
	return hash.result();
}

using AccountsMap = base::flat_map<
	not_null<Main::Session*>,
	std::unique_ptr<Account>>;

[[nodiscard]] AccountsMap &Accounts() {
	static const auto result = new AccountsMap();
	return *result;
}

void DestroyAccount(not_null<Main::Session*> session) {
	auto &map = Accounts();
	const auto i = map.find(session);
	if (i != end(map)) {
		const auto taken = std::move(i->second);
		map.erase(i);
	}
}

} // namespace

QString BaseUrl() {
	return CurrentConfig().base;
}

QUrl MakeUrl(const QString &path, const Query &query) {
	auto result = QUrl(BaseUrl());
	result.setPath(path);
	if (!query.empty()) {
		auto items = QUrlQuery();
		for (const auto &[name, value] : query) {
			items.addQueryItem(
				name,
				QString::fromLatin1(QUrl::toPercentEncoding(value, ",")));
		}
		result.setQuery(items);
	}
	return result;
}

Link ParseLink(const QString &url) {
	auto text = url.trimmed();
	if (text.startsWith(u"https://"_q, Qt::CaseInsensitive)) {
		text = text.mid(8);
	} else if (text.startsWith(u"http://"_q, Qt::CaseInsensitive)) {
		text = text.mid(7);
	}
	auto matched = false;
	const auto bases = { QString::fromLatin1(kDefaultBaseUrl), BaseUrl() };
	for (const auto &base : bases) {
		const auto host = HostOf(base);
		if (text.size() > host.size()
			&& text.startsWith(host, Qt::CaseInsensitive)
			&& text[host.size()] == '/') {
			text = text.mid(host.size());
			matched = true;
			break;
		}
	}
	if (!matched || text.size() < 4 || text[2] != '/') {
		return {};
	}
	const auto kind = text[1].toLower().unicode();
	auto id = text.mid(3);
	for (const auto stop : { '/', '?', '#' }) {
		const auto index = id.indexOf(QChar(stop));
		if (index >= 0) {
			id = id.left(index);
		}
	}
	if (kind == 'r') {
		const auto code = CleanRoomCode(id);
		return code.isEmpty() ? Link() : Link{ LinkKind::Room, code };
	} else if (kind == 'p' && ValidShareId(id)) {
		return { LinkKind::Playlist, id };
	} else if (kind == 'x' && ValidShareId(id)) {
		return { LinkKind::Preset, id };
	}
	return {};
}

QString MakeLink(LinkKind kind, const QString &id) {
	switch (kind) {
	case LinkKind::Room: return BaseUrl() + u"/r/"_q + id;
	case LinkKind::Playlist: return BaseUrl() + u"/p/"_q + id;
	case LinkKind::Preset: return BaseUrl() + u"/x/"_q + id;
	case LinkKind::None: break;
	}
	return QString();
}

QString NormalizeRoomCode(const QString &text) {
	if (text.contains('/')) {
		const auto link = ParseLink(text);
		return (link.kind == LinkKind::Room) ? link.id : QString();
	}
	return CleanRoomCode(text);
}

bool ValidMediaId(const QString &id) {
	if (id.size() != 64) {
		return false;
	}
	for (const auto ch : id) {
		const auto code = ch.unicode();
		if ((code < '0' || code > '9') && (code < 'a' || code > 'f')) {
			return false;
		}
	}
	return true;
}

bool ValidShareId(const QString &id) {
	if (id.size() != kShareIdLength) {
		return false;
	}
	for (const auto ch : id) {
		const auto code = ch.unicode();
		const auto fine = (code >= '0' && code <= '9')
			|| (code >= 'a' && code <= 'z')
			|| (code >= 'A' && code <= 'Z')
			|| (code == '-')
			|| (code == '_');
		if (!fine) {
			return false;
		}
	}
	return true;
}

int64 JsonInt(const QJsonValue &value, int64 fallback) {
	if (value.isDouble()) {
		const auto number = value.toDouble();
		if (!std::isfinite(number)
			|| std::abs(number) > 9007199254740992.) {
			return fallback;
		}
		return int64(std::llround(number));
	} else if (value.isString()) {
		auto ok = false;
		const auto number = value.toString().toLongLong(&ok);
		return ok ? int64(number) : fallback;
	}
	return fallback;
}

uint64 JsonUserId(const QJsonValue &value) {
	const auto number = JsonInt(value);
	return (number > 0 && number <= kMaxUserId) ? uint64(number) : 0;
}

QString JsonText(const QJsonValue &value, int maxLength, bool singleLine) {
	return value.isString()
		? CleanText(value.toString(), maxLength, singleLine)
		: QString();
}

std::optional<QColor> JsonColor(const QJsonValue &value) {
	const auto text = value.toString();
	if (text.size() != 7 || text[0] != '#') {
		return std::nullopt;
	}
	auto ok = false;
	const auto rgb = text.mid(1).toUInt(&ok, 16);
	if (!ok) {
		return std::nullopt;
	}
	return QColor(int((rgb >> 16) & 0xFF), int((rgb >> 8) & 0xFF), int(rgb & 0xFF));
}

Me ParseMe(const QJsonObject &user) {
	auto result = Me();
	result.id = JsonUserId(user.value(u"id"_q));
	if (!result.id) {
		return result;
	}
	result.name = JsonText(user.value(u"name"_q), 64);
	result.avatarRev = int(std::clamp(
		JsonInt(user.value(u"avatar_rev"_q)),
		int64(0),
		int64(1) << 30));
	result.verified = user.value(u"verified"_q).toBool();
	result.test = user.value(u"test"_q).toBool();
	result.badge = user.value(u"badge"_q).toBool();
	const auto profile = user.value(u"profile"_q).toObject();
	result.statusText = JsonText(profile.value(u"status_text"_q), 80);
	result.statusEmoji = JsonText(profile.value(u"status_emoji"_q), 16);
	const auto emojiId = JsonText(profile.value(u"status_emoji_id"_q), 20);
	const auto digits = ranges::all_of(emojiId, [](QChar ch) {
		return ch.isDigit();
	});
	result.statusEmojiId = digits ? emojiId : QString();
	result.accent = JsonColor(profile.value(u"accent"_q))
		? profile.value(u"accent"_q).toString().toLower()
		: QString();
	const auto privacy = user.value(u"privacy"_q).toObject();
	const auto audience = [](const QJsonValue &value) {
		const auto text = value.toString();
		return (text == u"everyone"_q || text == u"chosen"_q)
			? text
			: u"nobody"_q;
	};
	result.profileAudience = audience(privacy.value(u"profile"_q));
	result.activityAudience = audience(privacy.value(u"activity"_q));
	const auto chips = privacy.value(u"chips"_q).toObject();
	result.chipListening = chips.value(u"listening"_q).toBool();
	result.chipRoom = chips.value(u"room"_q).toBool();
	result.chipOnline = chips.value(u"online"_q).toBool();
	const auto chosen = privacy.value(u"chosen"_q).toArray();
	for (const auto &value : chosen) {
		if (const auto id = JsonUserId(value)) {
			result.chosen.push_back(id);
			if (result.chosen.size() >= 1000) {
				break;
			}
		}
	}
	return result;
}

Hello ParseHello(const QJsonObject &object) {
	auto result = Hello();
	result.protocol = int(JsonInt(object.value(u"protocol"_q)));
	result.minProtocol = int(JsonInt(object.value(u"min_protocol"_q)));
	result.version = JsonText(object.value(u"version"_q), 32);
	result.features = object.value(u"features"_q).toObject();
	result.limits = object.value(u"limits"_q).toObject();
	result.botUsername = JsonText(
		object.value(u"bot"_q).toObject().value(u"username"_q),
		64);
	const auto motd = object.value(u"motd"_q).toObject();
	result.motdRu = JsonText(motd.value(u"ru"_q), 300);
	result.motdEn = JsonText(motd.value(u"en"_q), 300);
	result.valid = (object.value(u"server"_q).toString()
			== u"oblivion-cloud"_q)
		&& (result.protocol >= 1)
		&& (result.minProtocol >= 1)
		&& (result.minProtocol <= result.protocol);
	return result;
}

Request GetRequest(const QString &path, Query query) {
	return { .path = path, .query = std::move(query) };
}

Request PostRequest(const QString &path, QJsonObject json) {
	return { .method = "POST", .path = path, .json = std::move(json) };
}

Request PatchRequest(const QString &path, QJsonObject json) {
	return { .method = "PATCH", .path = path, .json = std::move(json) };
}

Request PutRequest(const QString &path, QJsonObject json) {
	return { .method = "PUT", .path = path, .json = std::move(json) };
}

Request PutRawRequest(
		const QString &path,
		QByteArray bytes,
		QByteArray contentType) {
	return {
		.method = "PUT",
		.path = path,
		.raw = std::move(bytes),
		.rawType = std::move(contentType),
	};
}

Request DeleteRequest(const QString &path) {
	return { .method = "DELETE", .path = path };
}

QByteArray Response::header(const QByteArray &name) const {
	const auto lower = name.toLower();
	for (const auto &[key, value] : headers) {
		if (key == lower) {
			return value;
		}
	}
	return QByteArray();
}

QString MediaCachePath(const QString &sha256) {
	return MediaCacheFolder() + sha256;
}

QByteArray RandomBytes(int length) {
	auto result = QByteArray(std::max(length, 0), Qt::Uninitialized);
	if (!result.isEmpty()) {
		base::RandomFill(result.data(), result.size());
	}
	return result;
}

QByteArray Sha256(const QByteArray &bytes) {
	return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

QByteArray AesGcmSeal(
		const QByteArray &key,
		const QByteArray &plain,
		const QByteArray &aad) {
	if (key.size() != 32) {
		return QByteArray();
	}
	const auto context = EVP_CIPHER_CTX_new();
	if (!context) {
		return QByteArray();
	}
	const auto guard = gsl::finally([&] {
		EVP_CIPHER_CTX_free(context);
	});
	const auto bytes = [](const QByteArray &data) {
		return reinterpret_cast<const unsigned char*>(data.constData());
	};
	const auto nonce = RandomBytes(12);
	auto result = QByteArray(28 + plain.size(), Qt::Uninitialized);
	const auto out = reinterpret_cast<unsigned char*>(result.data());
	auto length = 0;
	auto written = 0;
	auto ok = (EVP_EncryptInit_ex(
			context,
			EVP_aes_256_gcm(),
			nullptr,
			nullptr,
			nullptr) == 1)
		&& (EVP_CIPHER_CTX_ctrl(
			context,
			EVP_CTRL_GCM_SET_IVLEN,
			12,
			nullptr) == 1)
		&& (EVP_EncryptInit_ex(
			context,
			nullptr,
			nullptr,
			bytes(key),
			bytes(nonce)) == 1);
	if (ok && !aad.isEmpty()) {
		ok = (EVP_EncryptUpdate(
			context,
			nullptr,
			&length,
			bytes(aad),
			int(aad.size())) == 1);
	}
	if (ok && !plain.isEmpty()) {
		ok = (EVP_EncryptUpdate(
			context,
			out + 28,
			&length,
			bytes(plain),
			int(plain.size())) == 1);
		written = length;
	}
	if (ok) {
		ok = (EVP_EncryptFinal_ex(context, out + 28 + written, &length) == 1);
		written += length;
	}
	if (ok) {
		ok = (EVP_CIPHER_CTX_ctrl(
			context,
			EVP_CTRL_GCM_GET_TAG,
			16,
			out + 12) == 1);
	}
	if (!ok || written != plain.size()) {
		return QByteArray();
	}
	memcpy(out, nonce.constData(), 12);
	return result;
}

std::optional<QByteArray> AesGcmOpen(
		const QByteArray &key,
		const QByteArray &sealed,
		const QByteArray &aad) {
	if (key.size() != 32 || sealed.size() < 28) {
		return std::nullopt;
	}
	const auto context = EVP_CIPHER_CTX_new();
	if (!context) {
		return std::nullopt;
	}
	const auto guard = gsl::finally([&] {
		EVP_CIPHER_CTX_free(context);
	});
	const auto bytes = [](const QByteArray &data) {
		return reinterpret_cast<const unsigned char*>(data.constData());
	};
	const auto size = int(sealed.size()) - 28;
	auto tag = sealed.mid(12, 16);
	auto result = QByteArray(size, Qt::Uninitialized);
	const auto out = reinterpret_cast<unsigned char*>(result.data());
	auto length = 0;
	auto written = 0;
	auto ok = (EVP_DecryptInit_ex(
			context,
			EVP_aes_256_gcm(),
			nullptr,
			nullptr,
			nullptr) == 1)
		&& (EVP_CIPHER_CTX_ctrl(
			context,
			EVP_CTRL_GCM_SET_IVLEN,
			12,
			nullptr) == 1)
		&& (EVP_DecryptInit_ex(
			context,
			nullptr,
			nullptr,
			bytes(key),
			bytes(sealed)) == 1);
	if (ok && !aad.isEmpty()) {
		ok = (EVP_DecryptUpdate(
			context,
			nullptr,
			&length,
			bytes(aad),
			int(aad.size())) == 1);
	}
	if (ok && size > 0) {
		ok = (EVP_DecryptUpdate(
			context,
			out,
			&length,
			bytes(sealed) + 28,
			size) == 1);
		written = length;
	}
	if (ok) {
		ok = (EVP_CIPHER_CTX_ctrl(
			context,
			EVP_CTRL_GCM_SET_TAG,
			16,
			tag.data()) == 1);
	}
	if (ok) {
		ok = (EVP_DecryptFinal_ex(context, out + written, &length) == 1);
		written += length;
	}
	if (!ok || written != size) {
		return std::nullopt;
	}
	return result;
}

QByteArray SlowKey(
		const QByteArray &password,
		const QByteArray &salt,
		int iterations,
		int length) {
	if (iterations <= 0 || length <= 0 || length > 1024) {
		return QByteArray();
	}
	auto result = QByteArray(length, Qt::Uninitialized);
	const auto ok = PKCS5_PBKDF2_HMAC(
		password.constData(),
		int(password.size()),
		reinterpret_cast<const unsigned char*>(salt.constData()),
		int(salt.size()),
		iterations,
		EVP_sha256(),
		length,
		reinterpret_cast<unsigned char*>(result.data()));
	return (ok == 1) ? result : QByteArray();
}

int64 ServerTimeFor(crl::time localTime) {
	return int64(std::llround(
		double(LocalWallBase() + localTime) + ClockOffset(localTime)));
}

crl::time LocalTimeFor(int64 serverTime) {
	return crl::time(std::llround(
		double(serverTime - LocalWallBase()) - ClockOffset(crl::now())));
}

int64 Now() {
	return ServerTimeFor(crl::now());
}

bool TimeSynced() {
	return Clock().synced.current();
}

rpl::producer<bool> TimeSyncedValue() {
	return Clock().synced.value();
}

struct Account::Private final : public base::has_weak_ptr {
	explicit Private(Descriptor &&value);
	~Private();

	void start();
	void save();
	[[nodiscard]] bool usable() const;
	[[nodiscard]] QByteArray token() const;

	void agree(const QString &name);
	void switchOn();
	void switchOff();
	void forgetDevice();
	void redeem(const QString &code, Fn<void()> done, Fail fail);
	void deleteData(Fn<void()> done, Fail fail);
	void deleted();

	void beginConnect();
	void requestHello(bool refresh);
	void requestRegister();
	void requestMe();
	[[nodiscard]] bool applyAuth(const QJsonObject &result);
	void authed();
	void connectFailed(const Error &error);
	void needsLink(const Error &error);
	void keyRejected(const Error &error);
	void closed(State value, const Error &error);
	void stopAll();
	void requestFailed(const Error &error);
	void requestSucceeded();
	void resetIdentity();
	void applyMe(const QJsonObject &user);

	[[nodiscard]] rpl::lifetime subscribe(const QString &raw, int64 eventId);
	void unsubscribe(const QString &code);
	[[nodiscard]] std::vector<QString> wantedRooms() const;
	void restartStreamIfChanged();
	void openStream();
	void closeStream();
	void streamRead(not_null<QNetworkReply*> reply);
	void streamFinished(not_null<QNetworkReply*> reply);
	void streamRetry(const Error &error);
	void checkHeartbeat();
	void handleEvent(Event &&event);
	void handleBye(const QString &reason);
	void afterStreamHello();
	void hint(int64 ts);

	void syncTime(Fn<void(bool)> done);
	void syncStep();
	void syncFinish(bool success);

	const Descriptor descriptor;
	Identity identity;
	Client client;
	rpl::variable<State> state = State::NoConsent;
	rpl::variable<bool> ready = false;
	rpl::event_stream<Error> connectErrors;
	rpl::event_stream<> meUpdates;
	rpl::event_stream<Event> events;
	Error lastError;
	Me me;
	Hello hello;
	crl::time helloAt = 0;
	crl::time authCheckedAt = 0;
	base::Timer connectTimer;
	int connectAttempt = 0;
	bool connecting = false;

	struct DeleteWaiter {
		Fn<void()> done;
		Fail fail;
	};
	std::vector<DeleteWaiter> deleteWaiters;
	bool deleting = false;

	QPointer<QNetworkReply> streamReply;
	SseParser parser;
	StreamCursor cursor;
	base::flat_map<QString, int> roomRefs;
	std::vector<QString> roomOrder;
	std::vector<QString> streamRooms;
	QByteArray streamErrorBody;
	base::Timer streamRestartTimer;
	base::Timer streamRetryTimer;
	base::Timer heartbeatTimer;
	base::Timer clockTimer;
	crl::time streamBytesAt = 0;
	crl::time streamOpenedAt = 0;
	crl::time streamHold = 0;
	crl::time streamNotBefore = 0;
	crl::time streamEarlyAt = 0;
	int streamAttempt = 0;
	int streamFlaps = 0;
	int streamStatus = 0;
	bool streamHello = false;
	bool streamBye = false;
	bool streamWithSince = false;
	bool streamUnauthorized = false;
	bool streamUnreachable = false;

	QPointer<QNetworkReply> syncReply;
	std::vector<ClockSample> syncSamples;
	std::vector<Fn<void(bool)>> syncCallbacks;
	crl::time syncedAt = 0;
	int syncIndex = -1;

};

Account::Private::Private(Descriptor &&value)
: descriptor(std::move(value))
, client([=] { return token(); })
, connectTimer([=] { beginConnect(); })
, streamRestartTimer([=] { restartStreamIfChanged(); })
, streamRetryTimer([=] { openStream(); })
, heartbeatTimer([=] { checkHeartbeat(); })
, clockTimer([=] {
	if (!roomRefs.empty() && state.current() == State::Online) {
		syncTime(nullptr);
	}
}) {
	identity.userId = descriptor.userId;
	client.setFailureHook([=](const Error &error) {
		requestFailed(error);
	});
	client.setSuccessHook([=] {
		requestSucceeded();
	});
}

Account::Private::~Private() {
	invalidate_weak_ptrs(this);
	closeStream();
	DropReply(syncReply, client.context());
}

bool Account::Private::usable() const {
	return descriptor.available
		&& descriptor.userId
		&& identity.consent
		&& !identity.disconnected;
}

QByteArray Account::Private::token() const {
	return usable() ? identity.token.toLatin1() : QByteArray();
}

void Account::Private::save() {
	if (!SaveIdentity(descriptor.folder, identity, descriptor.sealKey)) {
		LOG(("Oblivion Cloud Error: Could not save the key file."));
	}
}

void Account::Private::start() {
	const auto loaded = LoadIdentity(
		descriptor.folder,
		descriptor.userId,
		descriptor.sealKey);
	if (loaded) {
		identity = *loaded;
	}
	identity.userId = descriptor.userId;
	me = ParseMe(identity.me);
	if (!descriptor.available || !descriptor.userId || !identity.consent) {
		state = State::NoConsent;
	} else if (identity.disconnected) {
		state = State::Disconnected;
	} else {
		beginConnect();
	}
}

void Account::Private::agree(const QString &name) {
	if (!descriptor.available || !descriptor.userId) {
		return;
	}
	identity.consent = true;
	identity.disconnected = false;
	identity.name = CleanText(name, 64, true);
	identity.consentAt = base::unixtime::now();
	save();
	connectAttempt = 0;
	beginConnect();
}

void Account::Private::switchOn() {
	if (!descriptor.available || !identity.consent) {
		return;
	}
	identity.disconnected = false;
	save();
	connectAttempt = 0;
	beginConnect();
}

void Account::Private::switchOff() {
	if (!identity.consent || identity.disconnected) {
		return;
	}
	identity.disconnected = true;
	save();
	connecting = false;
	stopAll();
	state = State::Disconnected;
}

void Account::Private::forgetDevice() {
	const auto revoke = descriptor.available ? identity.token : QString();
	resetIdentity();
	RevokeLater(revoke);
}

void Account::Private::resetIdentity() {
	connecting = false;
	stopAll();
	QFile::remove(IdentityPath(descriptor.folder));
	identity = Identity{ .userId = descriptor.userId };
	me = Me();
	cursor.reset();
	lastError = Error();
	state = State::NoConsent;
	meUpdates.fire({});
}

void Account::Private::stopAll() {
	connectTimer.cancel();
	streamRetryTimer.cancel();
	streamRestartTimer.cancel();
	clockTimer.cancel();
	closeStream();
	streamUnauthorized = false;
	streamUnreachable = false;
	streamNotBefore = 0;
	streamHold = 0;
	streamAttempt = 0;
	streamFlaps = 0;
	deleting = false;
	deleteWaiters.clear();
	syncFinish(false);
	client.cancelAll();
	ready = false;
}

void Account::Private::closed(State value, const Error &error) {
	connecting = false;
	lastError = error;
	stopAll();
	state = value;
	connectErrors.fire_copy(error);
}

void Account::Private::beginConnect() {
	connectTimer.cancel();
	if (!usable() || connecting) {
		return;
	}
	connecting = true;
	if (state.current() != State::Offline
		&& state.current() != State::Online) {
		state = State::Connecting;
	}
	requestHello(false);
}

void Account::Private::requestHello(bool refresh) {
	auto request = GetRequest(u"/v1/hello"_q);
	request.auth = false;
	request.retries = refresh ? 0 : 1;
	client.send(std::move(request), [=](const Response &response) {
		const auto parsed = ParseHello(response.json);
		if (!parsed.valid) {
			if (!refresh) {
				connectFailed({ .type = Error::Type::Protocol });
			}
			return;
		}
		hello = parsed;
		helloAt = crl::now();
		if (hello.minProtocol > kProtocol) {
			closed(State::UpgradeRequired, {
				.type = Error::Type::Http,
				.status = 426,
				.code = u"upgrade_required"_q,
			});
		} else if (refresh) {
			return;
		} else if (identity.token.isEmpty()) {
			requestRegister();
		} else {
			requestMe();
		}
	}, [=](const Error &error) {
		if (!refresh) {
			connectFailed(error);
		}
	});
}

void Account::Private::requestRegister() {
	auto body = QJsonObject();
	body.insert(u"user_id"_q, QJsonValue(qint64(descriptor.userId)));
	body.insert(u"device_name"_q, DeviceName());
	body.insert(u"platform"_q, PlatformName());
	if (!identity.name.isEmpty()) {
		body.insert(u"name"_q, identity.name);
	}

	// The secret part of the key is made here and is on the disk before
	// it is sent: if the answer never arrives, the same registration goes
	// again (after the backoff, after «Подключить», at the next launch)
	// with the same secret, and the server gives the same device back
	// instead of 409. See Identity::pending. A server that does not know
	// the field ignores it and makes the secret itself, as before.
	if (!ValidDeviceSecret(identity.pending)) {
		identity.pending = NewDeviceSecret();
		save();
	}
	if (!identity.pending.isEmpty()) {
		body.insert(u"secret"_q, identity.pending);
	}
	auto request = PostRequest(u"/v1/auth/register"_q, std::move(body));
	request.auth = false;
	if (ValidHeaderValue(descriptor.testKey)) {
		// Only the live self-test has it: the server registers the fake
		// test ids with its test key and nothing else needs the key.
		request.headers.push_back({
			QByteArray(kTestKeyHeader),
			descriptor.testKey,
		});
	}
	client.send(std::move(request), [=](const Response &response) {
		if (!applyAuth(response.json)) {
			connectFailed({ .type = Error::Type::Protocol });
		}
	}, [=](const Error &error) {
		const auto bound = (error.type == Error::Type::Http)
			&& ((error.status == 409 && error.is("already_registered"))
				|| (error.status == 403
					&& error.is("verification_required")));
		if (bound) {
			needsLink(error);
		} else {
			connectFailed(error);
		}
	});
}

void Account::Private::requestMe() {
	auto request = GetRequest(u"/v1/me"_q);
	request.retries = 1;
	client.send(std::move(request), [=](const Response &response) {
		applyMe(response.json.value(u"user"_q).toObject());
		if (identity.rejected) {
			identity.rejected = false;
			save();
		}
		authed();
	}, [=](const Error &error) {
		if (error.type == Error::Type::Http && error.status == 401) {
			keyRejected(error);
		} else {
			connectFailed(error);
		}
	});
}

bool Account::Private::applyAuth(const QJsonObject &result) {
	const auto value = result.value(u"token"_q).toString();
	const auto user = result.value(u"user"_q).toObject();
	if (!ValidToken(value)
		|| JsonUserId(result.value(u"user_id"_q)) != descriptor.userId) {
		return false;
	}
	identity.token = value;
	identity.deviceId = JsonText(result.value(u"device_id"_q), 64);
	identity.rejected = false;
	identity.me = user;

	// The key is here (from a registration or from a link code): nothing
	// is on its way any more, the next registration gets a new secret.
	identity.pending = QString();
	me = ParseMe(user);
	save();
	streamRetryTimer.cancel();
	streamUnauthorized = false;
	closeStream();
	authed();
	meUpdates.fire({});
	return true;
}

// Called after a new key and after every check of the kept one. A check
// may happen while the stream is fine (one request was answered 401) or
// while it waits for its next attempt: the stream is left alone then, a
// healthy one is not reopened and a waiting one keeps its backoff.
void Account::Private::authed() {
	connecting = false;
	connectAttempt = 0;
	lastError = Error();
	const auto waiting = !streamReply
		&& !streamUnauthorized
		&& streamRetryTimer.isActive();
	if (streamReply && streamHello) {
		state = State::Online;
	} else if (!waiting && state.current() != State::Online) {
		state = State::Connecting;
	}
	ready = true;
	if (base::take(streamUnauthorized)) {
		// The key is fine for requests, but the stream was refused with
		// it a moment ago: try the stream again later, not in a loop.
		streamRetry({ .type = Error::Type::Http, .status = 401 });
	} else if (!streamReply && !streamRetryTimer.isActive()) {
		openStream();
	}
}

void Account::Private::connectFailed(const Error &error) {
	const auto http = (error.type == Error::Type::Http);
	if (http && error.status == 426) {
		closed(State::UpgradeRequired, error);
		return;
	} else if (http && error.status == 403 && error.is("banned")) {
		closed(State::Banned, error);
		return;
	}
	// A check of the key that could not get through says nothing about
	// a stream that works at the same moment: the state follows the
	// stream then and the check is simply repeated later.
	const auto healthy = ready.current() && streamReply && streamHello;
	connecting = false;
	if (!healthy) {
		lastError = error;
		state = State::Offline;
	}
	++connectAttempt;
	connectTimer.callOnce(std::max(
		BackoffDelay(connectAttempt, RandomJitter(), kConnectBackoffCap),
		std::min(error.retryAfter, kConnectBackoffCap)));
	connectErrors.fire_copy(error);
}

// The server no longer knows the key of this device: the device was
// revoked or the whole account was deleted from another one. Nothing is
// registered again by itself (that would bring a deleted account back
// without a click): the account is switched off, and turning it on asks
// the server as a new device.
void Account::Private::keyRejected(const Error &error) {
	identity.token = QString();
	identity.deviceId = QString();
	identity.rejected = false;
	identity.disconnected = true;
	save();
	closed(State::Disconnected, error);
}

void Account::Private::needsLink(const Error &error) {
	if (!identity.token.isEmpty() && !identity.rejected) {
		identity.rejected = true;
		save();
	}
	closed(State::NeedsLink, error);
}

void Account::Private::requestFailed(const Error &error) {
	if (connecting
		|| !ready.current()
		|| error.type != Error::Type::Http) {
		return;
	} else if (error.status == 426) {
		closed(State::UpgradeRequired, error);
	} else if (error.status == 403 && error.is("banned")) {
		closed(State::Banned, error);
	} else if (error.status == 401) {
		const auto now = crl::now();
		if (authCheckedAt && (now - authCheckedAt < kAuthRecheckEach)) {
			return;
		}
		authCheckedAt = now;
		crl::on_main(this, [=] {
			if (ready.current() && !connecting) {
				connecting = true;
				requestMe();
			}
		});
	}
}

void Account::Private::applyMe(const QJsonObject &user) {
	const auto parsed = ParseMe(user);
	if (!parsed.valid() || parsed.id != descriptor.userId) {
		return;
	}
	me = parsed;
	identity.me = user;
	save();
	meUpdates.fire({});
}

void Account::Private::redeem(
		const QString &code,
		Fn<void()> done,
		Fail fail) {
	if (!usable()) {
		client.failLater(
			std::move(fail),
			{ .type = Error::Type::NotConnected });
		return;
	}
	auto body = QJsonObject();
	body.insert(u"user_id"_q, QJsonValue(qint64(descriptor.userId)));
	body.insert(u"code"_q, code.trimmed().left(32));
	body.insert(u"device_name"_q, DeviceName());
	body.insert(u"platform"_q, PlatformName());
	auto request = PostRequest(u"/v1/auth/link/redeem"_q, std::move(body));
	request.auth = false;
	connectTimer.cancel();
	client.send(std::move(request), [=](const Response &response) {
		if (!applyAuth(response.json)) {
			if (fail) {
				fail({ .type = Error::Type::Protocol });
			}
		} else if (done) {
			done();
		}
	}, [=](const Error &error) {
		if (fail) {
			fail(error);
		}
	});
}

void Account::Private::deleteData(Fn<void()> done, Fail fail) {
	if (!ready.current()) {
		client.failLater(
			std::move(fail),
			{ .type = Error::Type::NotConnected });
		return;
	}
	deleteWaiters.push_back({ std::move(done), std::move(fail) });
	if (deleting) {
		return;
	}
	deleting = true;
	auto body = QJsonObject();
	body.insert(u"confirm"_q, u"delete"_q);
	client.send(
		PostRequest(u"/v1/me/delete"_q, std::move(body)),
		[=](const Response &response) {
			deleted();
		},
		[=](const Error &error) {
			deleting = false;
			const auto waiters = base::take(deleteWaiters);
			const auto weak = base::make_weak(this);
			for (const auto &waiter : waiters) {
				if (waiter.fail) {
					waiter.fail(error);
				}
				if (!weak) {
					return;
				}
			}
		});
}

// The account is gone from the server. That is known from the answer to
// the own request or from the "bye" of the event stream, whichever comes
// first: the server closes the stream before it answers, and forgetting
// the key drops every request that waits, the one that asked for the
// deletion included. So whoever asked is told from here in both cases.
void Account::Private::deleted() {
	const auto waiters = base::take(deleteWaiters);
	const auto weak = base::make_weak(this);
	resetIdentity();
	for (const auto &waiter : waiters) {
		if (!weak) {
			return;
		} else if (waiter.done) {
			waiter.done();
		}
	}
}

// An ordinary request got through while the stream waits for its next
// attempt after the connection was lost or could not be made: the server
// is reachable again, so the stream is tried now and not minutes later.
// Once a minute at most, and never when the server itself has closed or
// refused the stream (then the wait is what it asked for).
void Account::Private::requestSucceeded() {
	if (!streamUnreachable
		|| streamReply
		|| !ready.current()
		|| !streamRetryTimer.isActive()
		|| streamRetryTimer.remainingTime() <= kStreamRestartDelay) {
		return;
	}
	const auto now = crl::now();
	if (streamEarlyAt && (now - streamEarlyAt < kStreamEarlyRetryEach)) {
		return;
	}
	streamEarlyAt = now;
	streamRetryTimer.callOnce(kStreamRestartDelay);
}

rpl::lifetime Account::Private::subscribe(
		const QString &raw,
		int64 eventId) {
	auto result = rpl::lifetime();
	const auto code = CleanRoomCode(raw);
	if (code.isEmpty()) {
		return result;
	}
	if (eventId > 0) {
		cursor.setRoomFloor(code, eventId);
	}
	if (++roomRefs[code] == 1) {
		roomOrder.push_back(code);
		streamRestartTimer.callOnce(kStreamRestartDelay);
	}
	const auto weak = base::make_weak(this);
	result.add([=] {
		if (const auto that = weak.get()) {
			that->unsubscribe(code);
		}
	});
	return result;
}

void Account::Private::unsubscribe(const QString &code) {
	const auto i = roomRefs.find(code);
	if (i == end(roomRefs) || --i->second > 0) {
		return;
	}
	roomRefs.erase(i);
	roomOrder.erase(ranges::remove(roomOrder, code), end(roomOrder));
	cursor.removeRoom(code);
	streamRestartTimer.callOnce(kStreamRestartDelay);
}

std::vector<QString> Account::Private::wantedRooms() const {
	const auto count = int(roomOrder.size());
	const auto skip = std::max(count - kMaxStreamRooms, 0);
	return std::vector<QString>(begin(roomOrder) + skip, end(roomOrder));
}

// A room window was opened or closed. The stream is opened again with
// the new list at once, also when it waits for its next attempt: a click
// of the user is a reason to try. But not when the list is the same after
// all, and not before the moment the server asked to wait until (it
// closed the stream as replaced or answered with Retry-After): the
// attempt that is scheduled takes the new list by itself.
void Account::Private::restartStreamIfChanged() {
	if (!ready.current()) {
		return;
	}
	const auto waiting = streamRetryTimer.isActive();
	if ((streamReply || waiting) && wantedRooms() == streamRooms) {
		return;
	} else if (streamUnauthorized
		|| (waiting && crl::now() < streamNotBefore)) {
		return;
	}
	openStream();
}

void Account::Private::openStream() {
	streamRetryTimer.cancel();
	streamRestartTimer.cancel();
	closeStream();
	if (!ready.current()) {
		return;
	}
	streamRooms = wantedRooms();
	const auto since = cursor.since(streamRooms);
	auto query = Query();
	if (!streamRooms.empty()) {
		auto list = QStringList();
		for (const auto &code : streamRooms) {
			list.push_back(code);
		}
		query.push_back({ u"rooms"_q, list.join(',') });
	}
	if (since > 0) {
		query.push_back({ u"since"_q, QString::number(since) });
	}
	auto request = client.prepare(
		MakeUrl(u"/v1/events"_q, query),
		true,
		0);
	request.setRawHeader("Accept", "text/event-stream");
	request.setRawHeader("Accept-Encoding", "identity");
	const auto reply = client.streamManager()->get(request);
	client.watch(reply);
	streamReply = reply;
	parser.reset();
	streamErrorBody.clear();
	streamHello = false;
	streamBye = false;
	streamWithSince = (since > 0);
	streamStatus = 0;
	streamBytesAt = streamOpenedAt = crl::now();
	const auto weak = base::make_weak(this);
	const auto context = client.context().get();
	QObject::connect(reply, &QNetworkReply::readyRead, context, [=] {
		if (const auto that = weak.get()) {
			that->streamRead(reply);
		}
	});
	QObject::connect(reply, &QNetworkReply::finished, context, [=] {
		if (const auto that = weak.get()) {
			that->streamFinished(reply);
		} else {
			reply->deleteLater();
		}
	});
	heartbeatTimer.callEach(kHeartbeatCheck);
}

void Account::Private::closeStream() {
	heartbeatTimer.cancel();
	DropReply(streamReply, client.context());
}

void Account::Private::streamRead(not_null<QNetworkReply*> reply) {
	if (streamReply != reply.get()) {
		return;
	}
	streamBytesAt = crl::now();
	if (!streamStatus) {
		streamStatus = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (!streamStatus) {
			return;
		}
	}
	const auto bytes = reply->readAll();
	if (streamStatus != 200) {
		if (streamErrorBody.size() < kMaxErrorBody) {
			streamErrorBody.append(bytes);
		}
		return;
	}
	const auto frames = parser.feed(bytes);
	const auto weak = base::make_weak(this);
	for (const auto &frame : frames) {
		if (auto event = EventFromFrame(frame)) {
			handleEvent(std::move(*event));
			if (!weak || streamReply != reply.get()) {
				return;
			}
		}
	}
	if (parser.overflow()) {
		closeStream();
		streamRetry({ .type = Error::Type::Protocol });
	}
}

void Account::Private::streamFinished(not_null<QNetworkReply*> reply) {
	reply->deleteLater();
	if (streamReply != reply.get()) {
		return;
	}
	const auto weak = base::make_weak(this);
	streamRead(reply);
	if (!weak || streamReply != reply.get()) {
		return;
	}
	streamReply = nullptr;
	heartbeatTimer.cancel();
	const auto status = streamStatus
		? streamStatus
		: reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	const auto error = reply->property(kPinFailedProperty).toBool()
		? Error{ .type = Error::Type::Tls }
		: (status && status != 200)
		? ParseHttpError(
			status,
			streamErrorBody,
			reply->rawHeader("Retry-After"))
		: ErrorFromReply(reply, 0, QByteArray());
	const auto http = (error.type == Error::Type::Http);
	if (http && status == 401) {
		lastError = error;
		if (state.current() == State::Online) {
			state = State::Offline;
		}
		streamUnauthorized = true;
		if (!connecting) {
			connecting = true;
			requestMe();
		}
	} else if (http && status == 403 && error.is("banned")) {
		closed(State::Banned, error);
	} else if (http && status == 426) {
		closed(State::UpgradeRequired, error);
	} else {
		streamRetry(error);
	}
}

void Account::Private::streamRetry(const Error &error) {
	if (!ready.current()) {
		return;
	}
	lastError = error;
	const auto now = crl::now();
	if (base::take(streamHello)) {
		streamFlaps = StreamFlaps(streamFlaps, now - streamOpenedAt);
	}
	streamUnreachable = !base::take(streamBye)
		&& (error.type == Error::Type::Network
			|| error.type == Error::Type::Timeout);
	++streamAttempt;
	if (state.current() == State::Online
		|| state.current() == State::Connecting) {
		state = State::Offline;
	}
	const auto cap = roomRefs.empty()
		? kStreamBackoffCap
		: kStreamBackoffCapInRoom;
	const auto asked = std::max(
		std::min(error.retryAfter, kStreamBackoffCap),
		base::take(streamHold));
	streamNotBefore = (asked > 0) ? (now + asked) : 0;
	streamRetryTimer.callOnce(std::max(
		BackoffDelay(streamAttempt + streamFlaps, RandomJitter(), cap),
		asked));
}

void Account::Private::checkHeartbeat() {
	if (!streamReply) {
		return;
	}
	const auto now = crl::now();
	if (now - streamBytesAt > kStreamSilence) {
		closeStream();
		streamRetry({ .type = Error::Type::Timeout });
	} else if (streamHello && (now - streamOpenedAt > kStreamSilence)) {
		streamFlaps = 0;
	}
}

void Account::Private::hint(int64 ts) {
	if (ClockHint(ts) == ClockAdvice::Resync) {
		crl::on_main(this, [=] {
			syncTime(nullptr);
		});
	}
}

void Account::Private::handleEvent(Event &&event) {
	const auto weak = base::make_weak(this);
	if (event.type == u"ping"_q) {
		cursor.heartbeat();
		hint(event.ts);
		return;
	} else if (event.type == u"hello"_q) {
		const auto codes = [&](const char *key) {
			auto result = std::vector<QString>();
			const auto list = event.data.value(QLatin1String(key)).toArray();
			for (const auto &value : list) {
				const auto code = CleanRoomCode(value.toString().left(32));
				if (!code.isEmpty()) {
					result.push_back(code);
				}
			}
			return result;
		};
		const auto rejected = codes("rejected");
		cursor.connected(
			streamWithSince,
			JsonInt(event.data.value(u"event_id"_q)),
			codes("rooms"));
		streamHello = true;
		streamAttempt = 0;
		streamUnreachable = false;
		streamNotBefore = 0;
		lastError = Error();
		state = State::Online;
		const auto ts = event.ts;
		hint(ts);
		events.fire(std::move(event));
		for (const auto &code : rejected) {
			if (!weak) {
				return;
			}
			events.fire({ .type = u"room.rejected"_q, .room = code, .ts = ts });
		}
		if (weak) {
			crl::on_main(this, [=] {
				afterStreamHello();
			});
		}
		return;
	} else if (event.type == u"bye"_q) {
		const auto reason = event.data.value(u"reason"_q).toString();
		streamBye = true;
		events.fire(std::move(event));
		if (weak) {
			handleBye(reason);
		}
		return;
	} else if (event.type == u"resync"_q) {
		events.fire(std::move(event));
		return;
	} else if (event.id > 0 && !cursor.accept(event.id, event.room)) {
		return;
	}
	if (!event.id || !cursor.replaying()) {
		// A replayed event carries the time it was sent at first, that
		// says nothing about the clock.
		hint(event.ts);
	}
	if (event.type == u"me.updated"_q) {
		applyMe(event.data.value(u"user"_q).toObject());
		if (!weak) {
			return;
		}
	}
	events.fire(std::move(event));
}

void Account::Private::handleBye(const QString &reason) {
	if (reason == u"deleted"_q) {
		deleted();
	} else if (reason == u"revoked"_q) {
		keyRejected({
			.type = Error::Type::Http,
			.status = 401,
			.code = u"unauthorized"_q,
		});
	} else if (reason == u"banned"_q) {
		closed(State::Banned, {
			.type = Error::Type::Http,
			.status = 403,
			.code = u"banned"_q,
		});
	} else if (reason == u"replaced"_q) {
		streamHold = kStreamReplacedDelay;
	}
}

void Account::Private::afterStreamHello() {
	if (state.current() != State::Online) {
		return;
	}
	if (!syncedAt || (crl::now() - syncedAt > kClockResyncGap)) {
		syncTime(nullptr);
	}
	if (!clockTimer.isActive()) {
		clockTimer.callEach(kClockResyncEach);
	}
	if (crl::now() - helloAt > kHelloLifetime) {
		requestHello(true);
	}
}

void Account::Private::syncTime(Fn<void(bool)> done) {
	if (done) {
		syncCallbacks.push_back(std::move(done));
	}
	if (syncIndex >= 0) {
		return;
	} else if (!usable()) {
		syncFinish(false);
		return;
	}
	syncSamples.clear();
	syncIndex = 0;
	syncStep();
}

// The first request only opens the connection (the handshake would spoil
// its timing), the next ones are measured.
void Account::Private::syncStep() {
	const auto request = client.prepare(
		MakeUrl(u"/v1/time"_q),
		false,
		kClockSampleTimeout);
	const auto sent = LocalWallNow();
	const auto reply = client.manager()->get(request);
	client.watch(reply);
	syncReply = reply;
	const auto received = std::make_shared<int64>(0);
	const auto weak = base::make_weak(this);
	const auto context = client.context().get();
	QObject::connect(reply, &QNetworkReply::metaDataChanged, context, [=] {
		if (!*received) {
			*received = LocalWallNow();
		}
	});
	QObject::connect(reply, &QNetworkReply::finished, context, [=] {
		reply->deleteLater();
		const auto that = weak.get();
		if (!that || that->syncReply != reply) {
			return;
		}
		that->syncReply = nullptr;
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (status != 200 || reply->error() != QNetworkReply::NoError) {
			that->syncFinish(that->syncSamples.size() >= 2);
			return;
		}
		const auto server = QJsonDocument::fromJson(
			reply->readAll()).object().value(u"t"_q).toDouble();
		if (that->syncIndex > 0 && server > 0.) {
			that->syncSamples.push_back({
				.sent = sent,
				.received = *received ? *received : LocalWallNow(),
				.server = server,
			});
		}
		if (++that->syncIndex > kClockSamples) {
			that->syncFinish(true);
		} else {
			that->syncStep();
		}
	});
}

void Account::Private::syncFinish(bool success) {
	syncIndex = -1;
	DropReply(syncReply, client.context());
	auto applied = false;
	if (success) {
		const auto pick = PickClockSample(syncSamples);
		if (pick.valid) {
			ClockApply(pick.offset);
			syncedAt = crl::now();
			applied = true;
		}
	}
	syncSamples.clear();
	const auto callbacks = base::take(syncCallbacks);
	const auto weak = base::make_weak(this);
	for (const auto &callback : callbacks) {
		callback(applied);
		if (!weak) {
			return;
		}
	}
}

Account::Account(Descriptor &&descriptor)
: _private(std::make_unique<Private>(std::move(descriptor))) {
	_private->start();
}

Account::~Account() = default;

uint64 Account::userId() const {
	return _private->descriptor.userId;
}

bool Account::available() const {
	return _private->descriptor.available && _private->descriptor.userId;
}

State Account::state() const {
	return _private->state.current();
}

rpl::producer<State> Account::stateValue() const {
	return _private->state.value();
}

bool Account::ready() const {
	return _private->ready.current();
}

rpl::producer<bool> Account::readyValue() const {
	return _private->ready.value();
}

bool Account::consented() const {
	return _private->identity.consent;
}

Error Account::lastError() const {
	return _private->lastError;
}

rpl::producer<Error> Account::connectFailed() const {
	return _private->connectErrors.events();
}

void Account::agree(const QString &name) {
	_private->agree(name);
}

void Account::switchOn() {
	_private->switchOn();
}

void Account::switchOff() {
	_private->switchOff();
}

void Account::forgetDevice() {
	_private->forgetDevice();
}

void Account::createLinkCode(Fn<void(const LinkCode &code)> done, Fail fail) {
	request(PostRequest(u"/v1/auth/link/create"_q), [=](
			const Response &response) {
		const auto code = LinkCode{
			.code = JsonText(response.json.value(u"code"_q), 16),
			.expiresAt = JsonInt(response.json.value(u"expires_at"_q)),
		};
		if (code.code.isEmpty()) {
			if (fail) {
				fail({ .type = Error::Type::Protocol });
			}
		} else if (done) {
			done(code);
		}
	}, fail);
}

void Account::redeemLinkCode(
		const QString &code,
		Fn<void()> done,
		Fail fail) {
	_private->redeem(code, std::move(done), std::move(fail));
}

void Account::deleteData(Fn<void()> done, Fail fail) {
	_private->deleteData(std::move(done), std::move(fail));
}

QString Account::chosenName() const {
	return _private->identity.name;
}

const Me &Account::me() const {
	return _private->me;
}

rpl::producer<> Account::meUpdated() const {
	return _private->meUpdates.events();
}

void Account::patchMe(QJsonObject patch, Fn<void()> done, Fail fail) {
	const auto weak = base::make_weak(_private.get());

	// The server takes PATCH /v1/me only so often (30 at once, then one
	// in a second for a device): "too often" is waited out, the switch
	// the user has clicked is not shown as failed for that.
	auto prepared = PatchRequest(u"/v1/me"_q, std::move(patch));
	prepared.rateRetries = kPatchMeRateRetries;
	request(std::move(prepared), [=](const Response &response) {
		if (const auto that = weak.get()) {
			that->applyMe(response.json.value(u"user"_q).toObject());
		}
		if (done) {
			done();
		}
	}, std::move(fail));
}

void Account::applyMe(const QJsonObject &user) {
	_private->applyMe(user);
}

const Hello &Account::hello() const {
	return _private->hello;
}

bool Account::feature(const char *name) const {
	return _private->hello.features.value(QLatin1String(name)).toBool();
}

int64 Account::limit(const char *name, int64 fallback) const {
	const auto value = JsonInt(
		_private->hello.limits.value(QLatin1String(name)),
		-1);
	return (value > 0) ? value : fallback;
}

RequestId Account::request(Request &&request, Done done, Fail fail) {
	const auto allowed = request.auth
		? _private->ready.current()
		: _private->usable();
	if (!allowed) {
		_private->client.failLater(
			std::move(fail),
			{ .type = Error::Type::NotConnected });
		return 0;
	}
	return _private->client.send(
		std::move(request),
		std::move(done),
		std::move(fail));
}

void Account::cancel(RequestId id) {
	if (id) {
		_private->client.cancel(id);
	}
}

TransferId Account::upload(UploadArgs &&args) {
	if (!_private->ready.current()) {
		_private->client.failLater(
			std::move(args.fail),
			{ .type = Error::Type::NotConnected });
		return 0;
	}
	return _private->client.upload(std::move(args));
}

TransferId Account::download(DownloadArgs &&args) {
	const auto allowed = args.auth
		? _private->ready.current()
		: _private->usable();
	if (!allowed) {
		_private->client.failLater(
			std::move(args.fail),
			{ .type = Error::Type::NotConnected });
		return 0;
	}
	return _private->client.download(std::move(args));
}

TransferId Account::downloadMedia(
		const QString &sha256,
		Fn<void(const QString &path)> done,
		Fail fail,
		Progress progress) {
	if (!ValidMediaId(sha256)) {
		_private->client.failLater(
			std::move(fail),
			{ .type = Error::Type::Protocol });
		return 0;
	}
	const auto path = MediaCachePath(sha256);
	if (QFileInfo::exists(path)) {
		auto file = QFile(path);
		if (file.open(QIODevice::ReadWrite)) {
			file.setFileTime(
				QDateTime::currentDateTime(),
				QFileDevice::FileModificationTime);
			file.close();
		}
		if (done) {
			crl::on_main(_private.get(), [=] {
				done(path);
			});
		}
		return 0;
	}
	return download({
		.media = sha256,
		.to = path,
		.done = std::move(done),
		.fail = std::move(fail),
		.progress = std::move(progress),
	});
}

void Account::cancelTransfer(TransferId id) {
	if (id) {
		_private->client.cancelTransfer(id);
	}
}

rpl::producer<Event> Account::events() const {
	return _private->events.events();
}

rpl::producer<Event> Account::roomEvents(const QString &code) const {
	const auto clean = CleanRoomCode(code);
	return _private->events.events(
	) | rpl::filter([=](const Event &event) {
		return !clean.isEmpty()
			&& ((event.room == clean)
				|| ((event.type == u"me.room_removed"_q)
					&& (CleanRoomCode(event.data.value(
						u"code"_q).toString().left(32)) == clean)));
	});
}

rpl::lifetime Account::subscribeRoom(const QString &code, int64 eventId) {
	return _private->subscribe(code, eventId);
}

void Account::setRoomCursor(const QString &code, int64 eventId) {
	const auto clean = CleanRoomCode(code);
	if (!clean.isEmpty() && eventId > 0) {
		_private->cursor.setRoomFloor(clean, eventId);
	}
}

void Account::syncTime(Fn<void(bool success)> done) {
	_private->syncTime(std::move(done));
}

Sender::Sender(not_null<Account*> account)
: _account(base::make_weak(account))
, _ids(std::make_shared<base::flat_set<RequestId>>()) {
}

Sender::~Sender() {
	cancelAll();
}

RequestId Sender::request(Request &&request, Done done, Fail fail) {
	const auto account = _account.get();
	if (!account) {
		return 0;
	}
	const auto ids = std::weak_ptr(_ids);
	const auto holder = std::make_shared<RequestId>(0);
	const auto id = account->request(std::move(request), [=](
			const Response &response) {
		if (const auto strong = ids.lock()) {
			strong->remove(*holder);
			if (done) {
				done(response);
			}
		}
	}, [=](const Error &error) {
		if (const auto strong = ids.lock()) {
			strong->remove(*holder);
			if (fail) {
				fail(error);
			}
		}
	});
	*holder = id;
	if (id) {
		_ids->emplace(id);
	}
	return id;
}

void Sender::cancel(RequestId id) {
	if (!id || !_ids->remove(id)) {
		return;
	} else if (const auto account = _account.get()) {
		account->cancel(id);
	}
}

void Sender::cancelAll() {
	const auto ids = std::exchange(
		_ids,
		std::make_shared<base::flat_set<RequestId>>());
	if (const auto account = _account.get()) {
		for (const auto id : *ids) {
			account->cancel(id);
		}
	}
}

Account *Sender::account() const {
	return _account.get();
}

Account &For(not_null<Main::Session*> session) {
	auto &map = Accounts();
	const auto i = map.find(session);
	if (i != end(map)) {
		return *i->second;
	}
	TrimMediaCache();
	auto account = std::make_unique<Account>(Account::Descriptor{
		.userId = session->userId().bare,
		.folder = AccountFolder(session),
		.sealKey = SealKeyFor(session),
		.available = !session->isTestMode(),
	});
	const auto raw = account.get();
	map.emplace(session, std::move(account));
	session->lifetime().add([=] {
		DestroyAccount(session);
	});
	return *raw;
}

bool AnyReady() {
	for (const auto &[session, account] : Accounts()) {
		const auto state = account->state();
		if (account->consented()
			&& state != State::NoConsent
			&& state != State::Disconnected) {
			return true;
		}
	}
	return false;
}

RequestId PublicRequest(Request &&request, Done done, Fail fail) {
	request.auth = false;
	if (!request.path.startsWith(u"/v1/updates/"_q)) {
		PublicClient().failLater(
			std::move(fail),
			{ .type = Error::Type::Protocol });
		return 0;
	}
	return PublicClient().send(
		std::move(request),
		std::move(done),
		std::move(fail));
}

void CancelPublicRequest(RequestId id) {
	if (id) {
		PublicClient().cancel(id);
	}
}

TransferId PublicDownload(DownloadArgs &&args) {
	args.auth = false;
	args.media = QString();
	if (!args.path.startsWith(u"/v1/updates/"_q)) {
		PublicClient().failLater(
			std::move(args.fail),
			{ .type = Error::Type::Protocol });
		return 0;
	}
	return PublicClient().download(std::move(args));
}

void CancelPublicDownload(TransferId id) {
	if (id) {
		PublicClient().cancelTransfer(id);
	}
}

void SessionStarted(not_null<Main::Session*> session) {
	(void)For(session);
	ProcessRevokes();
	Rooms::Start(session);
	Social::Start(session);
	Share::Start(session);
	Sync::Start(session);
	Update::Start(session);
	SendOnline::Start(session);
}

void SessionLoggedOut(not_null<Main::Session*> session) {
	SendOnline::Forget(session);
	Sync::Forget(session);
	Share::Forget(session);
	Social::Forget(session);
	Rooms::Forget(session);
	For(session).forgetDevice();
	DestroyAccount(session);
}

bool HandleLinkClick(const QString &url, const QVariant &context) {
	if (!Oblivion::Get().cloudLinks()) {
		return false;
	}
	const auto link = ParseLink(url);
	if (!link) {
		return false;
	}
	const auto my = context.value<ClickHandlerContext>();
	auto controller = my.sessionWindow.get();
	if (!controller) {
		if (const auto window = Core::App().activeWindow()) {
			controller = window->sessionController();
		}
	}
	if (!controller) {
		return false;
	}
	switch (link.kind) {
	case LinkKind::Room:
		if (!Oblivion::Get().cloudRooms()) {
			return false;
		}
		Rooms::OpenLink(controller, link.id);
		return true;
	case LinkKind::Playlist:
		Share::OpenPlaylistLink(controller, link.id);
		return true;
	case LinkKind::Preset:
		Share::OpenPresetLink(controller, link.id);
		return true;
	case LinkKind::None:
		break;
	}
	return false;
}

namespace {

class Checker final {
public:
	explicit Checker(QStringList &log);

	void operator()(bool condition, const char *what);
	void section(const char *name);
	[[nodiscard]] int passed() const;
	[[nodiscard]] int failed() const;

private:
	QStringList &_log;
	int _passed = 0;
	int _failed = 0;
	int _sectionPassed = 0;
	int _sectionFailed = 0;

};

Checker::Checker(QStringList &log)
: _log(log) {
}

void Checker::operator()(bool condition, const char *what) {
	if (condition) {
		++_passed;
		++_sectionPassed;
	} else {
		++_failed;
		++_sectionFailed;
		_log.push_back(u"FAILED: "_q + QString::fromUtf8(what));
	}
}

void Checker::section(const char *name) {
	_log.push_back(u"%1: %2 passed, %3 failed"_q.arg(
		QString::fromUtf8(name),
		QString::number(_sectionPassed),
		QString::number(_sectionFailed)));
	_sectionPassed = _sectionFailed = 0;
}

int Checker::passed() const {
	return _passed;
}

int Checker::failed() const {
	return _failed;
}

bool WaitFor(Fn<bool()> done, crl::time timeout) {
	if (done()) {
		return true;
	}
	auto loop = QEventLoop();
	auto timer = QTimer();
	const auto started = crl::now();
	timer.setInterval(15);
	QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
		if (done() || (crl::now() - started > timeout)) {
			loop.quit();
		}
	});
	timer.start();
	loop.exec();
	return done();
}

[[nodiscard]] QJsonObject ParseObject(const char *json) {
	return QJsonDocument::fromJson(QByteArray(json)).object();
}

void TestConfig(Checker &check) {
	const auto standard = QString::fromLatin1(kDefaultBaseUrl);
	check(ConfigFrom(QString(), false).base == standard, "config: default");
	check(!ConfigFrom(QString(), true).insecure, "config: default is https");
	const auto local = ConfigFrom(u" http://127.0.0.1:18790/ "_q, true);
	check(
		local.insecure && local.base == u"http://127.0.0.1:18790"_q,
		"config: loopback http with the insecure flag");
	check(
		ConfigFrom(u"http://127.0.0.1:18790"_q, false).base == standard,
		"config: http without the flag is ignored");
	check(
		ConfigFrom(u"http://example.org:80"_q, true).base == standard,
		"config: http to another host is ignored");
	check(
		ConfigFrom(u"ftp://127.0.0.1"_q, true).base == standard,
		"config: other schemes are ignored");
	const auto domain = ConfigFrom(u"https://cloud.example.org/a?b#c"_q, true);
	check(
		!domain.insecure && domain.base == u"https://cloud.example.org"_q,
		"config: https override keeps only the origin");

	check(PinnedDigest().size() == 32, "pin: digest size");
	check(!PinnedDer().isEmpty(), "pin: embedded certificate decodes");
	check(
		Sha256(PinnedDer()) == PinnedDigest(),
		"pin: digest of the embedded certificate");
	const auto list = QSslCertificate::fromData(
		QByteArray(kPinnedCertificate),
		QSsl::Pem);
	check(
		list.size() == 1 && IsPinned(list.front()),
		"pin: the TLS backend parses the certificate and it is the pinned");
	check(!IsPinned(QSslCertificate()), "pin: a null certificate");
	check(
		!PinnedPeer(QSslCertificate(), {}),
		"pin: no peer and no errors is not trusted");
	if (list.size() == 1) {
		const auto &pinned = list.front();
		check(PinnedPeer(pinned, {}), "pin: the right peer");
		check(
			PinnedPeer(pinned, {
				QSslError(QSslError::CertificateUntrusted, pinned),
				QSslError(QSslError::HostNameMismatch, pinned),
				QSslError(QSslError::SelfSignedCertificate, pinned),
			}),
			"pin: the expected errors of the pinned certificate pass");
		check(
			!PinnedPeer(pinned, {
				QSslError(QSslError::HostNameMismatch, pinned),
				QSslError(QSslError::CertificateExpired, pinned),
			}),
			"pin: an expired pinned certificate is still an error");
		check(
			!PinnedPeer(pinned, {
				QSslError(QSslError::HostNameMismatch, pinned),
				QSslError(QSslError::SelfSignedCertificate),
			}),
			"pin: an error about another certificate is never ignored");
		check(
			PinnedPeer(QSslCertificate(), {
				QSslError(QSslError::CertificateUntrusted, pinned),
			}),
			"pin: no peer yet, the error names the pinned certificate");
		check(
			QSslError(QSslError::CertificateUntrusted, pinned)
				!= QSslError(QSslError::CertificateUntrusted),
			"pin: an ignored error is tied to its certificate");
	}
	check(
		!PinnedPeer(QSslCertificate(), {
			QSslError(QSslError::CertificateUntrusted),
		}),
		"pin: no peer and an error without a certificate");
	check(
		ExpectedPinError(QSslError::SelfSignedCertificateInChain)
			&& ExpectedPinError(QSslError::UnableToGetLocalIssuerCertificate)
			&& ExpectedPinError(QSslError::UnableToVerifyFirstCertificate),
		"pin: an unknown authority is expected");
	check(
		!ExpectedPinError(QSslError::NoError)
			&& !ExpectedPinError(QSslError::NoPeerCertificate)
			&& !ExpectedPinError(QSslError::CertificateNotYetValid)
			&& !ExpectedPinError(QSslError::CertificateRevoked)
			&& !ExpectedPinError(QSslError::CertificateBlacklisted)
			&& !ExpectedPinError(QSslError::InvalidPurpose)
			&& !ExpectedPinError(QSslError::CertificateSignatureFailed)
			&& !ExpectedPinError(QSslError::UnspecifiedError),
		"pin: what is never let through");
	check(
		PinnedConfiguration().caCertificates().size() == 1
			&& (PinnedConfiguration().peerVerifyMode()
				== QSslSocket::VerifyPeer),
		"pin: the only anchor, the peer is verified");
}

void TestUrls(Checker &check) {
	const auto base = BaseUrl();
	check(
		MakeUrl(u"/v1/hello"_q).toString() == base + u"/v1/hello"_q,
		"url: plain path");
	const auto stream = MakeUrl(u"/v1/events"_q, {
		{ u"rooms"_q, u"3JWNR7QVN2,K7QM2XPA9Z"_q },
		{ u"since"_q, u"1791327961174000"_q },
	});
	const auto streamItems = QUrlQuery(stream).queryItems(QUrl::FullyDecoded);
	check(
		stream.path() == u"/v1/events"_q
			&& streamItems.size() == 2
			&& streamItems[0].first == u"rooms"_q
			&& streamItems[0].second == u"3JWNR7QVN2,K7QM2XPA9Z"_q
			&& streamItems[1].first == u"since"_q
			&& streamItems[1].second == u"1791327961174000"_q,
		"url: stream query");
	const auto tricky = u"a b&c=d+e#f"_q;
	const auto escaped = MakeUrl(
		u"/v1/presets/gallery"_q,
		{ { u"cursor"_q, tricky } });
	const auto escapedItems = QUrlQuery(escaped).queryItems(
		QUrl::FullyDecoded);
	const auto encoded = escaped.toString(QUrl::FullyEncoded);
	check(
		escapedItems.size() == 1
			&& escapedItems[0].second == tricky
			&& !escaped.hasFragment()
			&& !encoded.contains(' ')
			&& !encoded.contains('+')
			&& !encoded.contains('#'),
		"url: query values are escaped");
	const auto pathOnly = MakeUrl(u"/v1/rooms/A?B#C/chat"_q);
	check(
		!pathOnly.hasQuery()
			&& !pathOnly.hasFragment()
			&& pathOnly.path() == u"/v1/rooms/A?B#C/chat"_q,
		"url: a path can't add a query");

	const auto origin = QString::fromLatin1(kDefaultBaseUrl);
	const auto host = HostOf(origin);
	const auto room = ParseLink(origin + u"/r/3jwnr7qvn2"_q);
	check(
		room.kind == LinkKind::Room && room.id == u"3JWNR7QVN2"_q,
		"link: room, lower case");
	check(
		ParseLink(host + u"/r/3JWNR7QVN2?x=1#y"_q).id == u"3JWNR7QVN2"_q,
		"link: no scheme, a query and a fragment");
	check(
		ParseLink(origin + u"/r/3JWNR7QVN2/"_q).kind == LinkKind::Room,
		"link: trailing slash");
	check(!ParseLink(origin + u"/r/3JWNR7QVN"_q), "link: short code");
	check(!ParseLink(origin + u"/r/3JWNR7QVN0"_q), "link: bad letter");
	check(
		!ParseLink(u"https://example.org/r/3JWNR7QVN2"_q),
		"link: another host");
	check(
		!ParseLink(origin + u"1/r/3JWNR7QVN2"_q),
		"link: another port");
	check(!ParseLink(origin + u"/q/3JWNR7QVN2"_q), "link: unknown kind");
	check(!ParseLink(origin), "link: no path");
	check(!ParseLink(QString()), "link: empty");
	const auto shareId = u"mK3_abcdefghij-KLMNOPQ"_q;
	const auto playlist = ParseLink(origin + u"/p/"_q + shareId);
	check(
		playlist.kind == LinkKind::Playlist && playlist.id == shareId,
		"link: playlist");
	check(
		ParseLink(origin + u"/x/"_q + shareId).kind == LinkKind::Preset,
		"link: preset");
	check(!ParseLink(origin + u"/p/short"_q), "link: short playlist id");
	check(
		ParseLink(MakeLink(LinkKind::Room, u"3JWNR7QVN2"_q)).id
			== u"3JWNR7QVN2"_q,
		"link: round trip");
	check(MakeLink(LinkKind::None, u"x"_q).isEmpty(), "link: none");

	check(
		NormalizeRoomCode(u"  3jwnr7qvn2 "_q) == u"3JWNR7QVN2"_q,
		"code: trimmed and upper case");
	check(
		NormalizeRoomCode(origin + u"/r/3jwnr7qvn2"_q) == u"3JWNR7QVN2"_q,
		"code: from a link");
	check(NormalizeRoomCode(u"3JWNR7QVNO"_q).isEmpty(), "code: letter O");
	check(NormalizeRoomCode(u"3JWNR7QVN22"_q).isEmpty(), "code: too long");
	check(
		NormalizeRoomCode(origin + u"/p/"_q + shareId).isEmpty(),
		"code: a playlist link is not a room");
	const auto media = u"1cda77c5810cde9c0821e66dda9a0f5a"_q
		+ u"c4c192f0ecd2a0f7c746fa411d43ac3b"_q;
	check(ValidMediaId(media), "media id: valid");
	check(!ValidMediaId(media.toUpper()), "media id: upper case");
	check(!ValidMediaId(media.left(63)), "media id: short");
	check(!ValidMediaId(media.left(63) + 'g'), "media id: not hex");
	check(ValidShareId(shareId), "share id: valid");
	check(!ValidShareId(shareId + 'a'), "share id: long");
	check(!ValidShareId(shareId.left(21) + '/'), "share id: bad symbol");
}

void TestJson(Checker &check) {
	check(
		JsonInt(QJsonValue(1791327961174000.)) == 1791327961174000,
		"json: a big integer");
	check(JsonInt(QJsonValue(u"123"_q)) == 123, "json: a decimal string");
	check(JsonInt(QJsonValue(u"x"_q), -1) == -1, "json: not a number");
	check(JsonInt(QJsonValue(), 7) == 7, "json: null");
	check(JsonInt(QJsonValue(1e300), 5) == 5, "json: too big");
	check(JsonInt(QJsonValue(true), 3) == 3, "json: a bool is not a number");
	check(JsonUserId(QJsonValue(123)) == 123, "user id: number");
	check(
		JsonUserId(QJsonValue(u"9000000000000042"_q))
			== uint64(9000000000000042),
		"user id: string");
	check(!JsonUserId(QJsonValue(0)), "user id: zero");
	check(!JsonUserId(QJsonValue(-5)), "user id: negative");
	check(
		!JsonUserId(QJsonValue(9007199254740992.)),
		"user id: above 2^53 - 1");

	const auto dirty = QString::fromUtf8("  a\x01" "b\nc\td\xE2\x80\xAE" "e ");
	check(
		JsonText(QJsonValue(dirty), 64) == u"ab c de"_q,
		"text: single line, control characters and overrides removed");
	check(
		JsonText(QJsonValue(dirty), 64, false) == u"  ab\nc\tde "_q,
		"text: several lines keep the breaks");
	check(JsonText(QJsonValue(5), 64).isEmpty(), "text: not a string");
	const auto emoji = QString::fromUtf8("ab\xF0\x9F\x98\x80");
	check(
		JsonText(QJsonValue(emoji), 3) == u"ab"_q,
		"text: a surrogate pair is not cut in half");
	check(JsonText(QJsonValue(emoji), 4) == emoji, "text: fits");
	check(JsonText(QJsonValue(emoji), 0).isEmpty(), "text: zero limit");

	const auto color = JsonColor(QJsonValue(u"#7c5cFF"_q));
	check(
		color && color->red() == 0x7C && color->blue() == 0xFF,
		"color: valid");
	check(!JsonColor(QJsonValue(u"#12345"_q)), "color: short");
	check(!JsonColor(QJsonValue(u"7c5cff0"_q)), "color: no hash");
	check(!JsonColor(QJsonValue(u"#GGGGGG"_q)), "color: not hex");
	check(!JsonColor(QJsonValue(7)), "color: not a string");

	const auto limited = ParseHttpError(
		429,
		"{\"error\":{\"code\":\"rate_limited\",\"message\":\"Too many\","
		"\"retry_after_ms\":1200}}",
		QByteArray());
	check(
		limited.type == Error::Type::Http
			&& limited.status == 429
			&& limited.is("rate_limited")
			&& limited.retryAfter == 1200,
		"error: rate limit");
	check(RetryableError(limited), "error: a short wait is retried");
	check(
		ParseHttpError(429, "{}", " 3 ").retryAfter == 3000,
		"error: Retry-After header");
	check(
		!RetryableError(ParseHttpError(429, "{}", "60")),
		"error: a long wait is not retried");
	check(
		RateRetryDelay(limited, 0, 0, 0.5) == 0
			&& RateRetryDelay(limited, 0, 2, 0.) == 1200
			&& RateRetryDelay(limited, 1, 2, 0.) == 1200
			&& RateRetryDelay(limited, 2, 2, 0.) == 0
			&& RateRetryDelay(limited, -1, 2, 0.) == 0,
		"rate: a refused request is repeated only as often as asked");
	check(
		RateRetryDelay(limited, 0, 3, 1.) == 1200 + kRateRetrySpread
			&& RateRetryDelay(limited, 2, 3, 1.) == 1200 + 3 * kRateRetrySpread
			&& RateRetryDelay(limited, 0, 3, 9.) == 1200 + kRateRetrySpread,
		"rate: the repeats are spread");
	check(
		RateRetryDelay(ParseHttpError(429, "{}", QByteArray()), 0, 3, 0.)
			== kRateRetryMin,
		"rate: never at once");
	check(
		RateRetryDelay(ParseHttpError(429, "{}", "60"), 0, 3, 0.) == 0,
		"rate: a long wait is left to the one who asked");
	check(
		RateRetryDelay(ParseHttpError(503, "{}", "1"), 0, 3, 0.) == 0
			&& RateRetryDelay({ .type = Error::Type::Network }, 0, 3, 0.) == 0
			&& RateRetryDelay({ .type = Error::Type::Timeout }, 0, 3, 0.) == 0,
		"rate: only 429, a request that may have been done is not repeated");
	const auto garbage = ParseHttpError(502, "<html>", QByteArray());
	check(
		garbage.code == u"http_502"_q && RetryableError(garbage),
		"error: no body");
	const auto stale = ParseHttpError(
		409,
		"{\"error\":{\"code\":\"offset_mismatch\",\"offset\":131072}}",
		QByteArray());
	check(
		stale.is("offset_mismatch")
			&& JsonInt(stale.details.value(u"offset"_q)) == 131072
			&& !RetryableError(stale),
		"error: details are kept");
	check(
		RetryableError({ .type = Error::Type::Network })
			&& RetryableError({ .type = Error::Type::Timeout })
			&& !RetryableError({ .type = Error::Type::Tls })
			&& !RetryableError({ .type = Error::Type::NotConnected }),
		"error: what is retried");
	check(SuccessStatus(200) && SuccessStatus(204) && SuccessStatus(304),
		"status: success");
	check(!SuccessStatus(0) && !SuccessStatus(301) && !SuccessStatus(404),
		"status: failure");

	const auto hello = ParseHello(ParseObject(
		"{\"server\":\"oblivion-cloud\",\"version\":\"1.0.0\","
		"\"protocol\":1,\"min_protocol\":1,\"time\":1791327935017,"
		"\"features\":{\"rooms\":true,\"bot_verification\":false},"
		"\"bot\":null,\"limits\":{\"chunk_max\":8388608},"
		"\"motd\":{\"ru\":\"Привет\",\"en\":\"Hi\"}}"));
	check(
		hello.valid
			&& hello.protocol == 1
			&& hello.features.value(u"rooms"_q).toBool()
			&& JsonInt(hello.limits.value(u"chunk_max"_q)) == 8388608
			&& hello.botUsername.isEmpty()
			&& hello.motdEn == u"Hi"_q,
		"hello: valid");
	check(
		!ParseHello(ParseObject("{\"protocol\":1,\"min_protocol\":1}")).valid,
		"hello: another server");
	check(
		!ParseHello(ParseObject(
			"{\"server\":\"oblivion-cloud\",\"protocol\":1,"
			"\"min_protocol\":2}")).valid,
		"hello: min above the protocol");
	const auto newer = ParseHello(ParseObject(
		"{\"server\":\"oblivion-cloud\",\"protocol\":3,\"min_protocol\":2,"
		"\"bot\":{\"username\":\"SomeBot\"}}"));
	check(
		newer.valid
			&& newer.minProtocol > kProtocol
			&& newer.botUsername == u"SomeBot"_q,
		"hello: a server that needs a newer client");

	const auto me = ParseMe(ParseObject(
		"{\"id\":9000000000777001,\"name\":\"Probe A\",\"avatar_rev\":3,"
		"\"verified\":false,\"test\":true,\"badge\":true,"
		"\"profile\":{\"status_text\":\"сплю\",\"status_emoji\":\"x\","
		"\"status_emoji_id\":\"5368324170671202286\","
		"\"accent\":\"#7C5CFF\"},"
		"\"privacy\":{\"profile\":\"everyone\",\"activity\":\"all\","
		"\"chips\":{\"listening\":true,\"room\":false,\"online\":true},"
		"\"chosen\":[111,\"222\",0,\"x\"]}}"));
	check(
		me.valid()
			&& me.id == uint64(9000000000777001)
			&& me.name == u"Probe A"_q
			&& me.avatarRev == 3
			&& me.test
			&& me.badge,
		"me: fields");
	check(
		me.statusEmojiId == u"5368324170671202286"_q
			&& me.accent == u"#7c5cff"_q,
		"me: profile");
	check(
		me.profileAudience == u"everyone"_q
			&& me.activityAudience == u"nobody"_q
			&& me.chipListening
			&& !me.chipRoom
			&& me.chipOnline,
		"me: privacy, an unknown audience is nobody");
	check(
		me.chosen == std::vector<uint64>{ 111, 222 },
		"me: chosen ids");
	check(!ParseMe(ParseObject("{\"name\":\"x\"}")).valid(), "me: no id");
	const auto bare = ParseMe(ParseObject("{\"id\":5}"));
	check(
		bare.valid()
			&& bare.profileAudience == u"nobody"_q
			&& !bare.badge
			&& bare.accent.isEmpty(),
		"me: defaults are private");

	check(
		ValidToken(u"OJolNaPQycz8_qao.OJdrdQovFM0_RfVm5FKwOxrCT-ioJk3Sx9JP"_q),
		"token: valid");
	check(!ValidToken(u"abc"_q), "token: short");
	check(!ValidToken(u"abcdefghijklmnop"_q), "token: no dot");
	check(!ValidToken(u"abcdefgh.ijkl mnop"_q), "token: a space");
	check(!ValidToken(u"abcdefgh.ijkl\nmnop"_q), "token: a line break");
	check(
		ValidHeaderValue("local-test-key-0123456789abcdef")
			&& !ValidHeaderValue(QByteArray())
			&& !ValidHeaderValue("key\r\nX-Other: 1")
			&& !ValidHeaderValue("\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87"),
		"header: only one line of printable ASCII is sent as a value");

	auto from = int64(0);
	auto total = int64(0);
	check(
		ParseContentRange("bytes 299990-299999/300000", from, total)
			&& from == 299990
			&& total == 300000,
		"range: a part");
	check(
		ParseContentRange("bytes */300000", from, total)
			&& from == -1
			&& total == 300000,
		"range: not satisfiable");
	check(!ParseContentRange("items 0-1/2", from, total), "range: unit");
	check(!ParseContentRange("bytes 5-9/x", from, total), "range: total");
	check(!ParseContentRange("bytes x-9/10", from, total), "range: start");
	check(!ParseContentRange(QByteArray(), from, total), "range: empty");

	const auto joined = EventFromFrame({
		.event = "room.member_joined",
		.data = "{\"room\":\"3jwnr7qvn2\",\"member\":{\"id\":5},"
			"\"ts\":1791327961597}",
		.id = "1791327961597000",
	});
	check(
		joined
			&& joined->type == u"room.member_joined"_q
			&& joined->id == 1791327961597000
			&& joined->room == u"3JWNR7QVN2"_q
			&& joined->ts == 1791327961597,
		"event: with an id and a room");
	const auto volatileEvent = EventFromFrame({
		.event = "room.reaction",
		.data = "{\"room\":\"3JWNR7QVN2\",\"emoji\":\"x\",\"ts\":5}",
	});
	check(
		volatileEvent && volatileEvent->id == 0,
		"event: volatile has no id");
	check(
		!EventFromFrame({ .event = "x", .data = "not json" }),
		"event: not JSON");
	check(
		!EventFromFrame({ .event = "x", .data = "[1,2]" }),
		"event: not an object");
	check(!EventFromFrame({ .retry = 3000 }), "event: retry only");
	const auto badId = EventFromFrame({
		.event = "x",
		.data = "{}",
		.id = "12ab",
	});
	check(badId && badId->id == 0, "event: a bad id is no id");
	const auto longId = EventFromFrame({
		.event = "x",
		.data = "{\"room\":\"nope\"}",
		.id = "12345678901234567890",
	});
	check(
		longId && longId->id == 0 && longId->room.isEmpty(),
		"event: a too long id and a bad room code");
}

[[nodiscard]] bool SameFrames(
		const std::vector<SseParser::Frame> &a,
		const std::vector<SseParser::Frame> &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (auto i = 0, count = int(a.size()); i != count; ++i) {
		if (a[i].event != b[i].event
			|| a[i].data != b[i].data
			|| a[i].id != b[i].id
			|| a[i].retry != b[i].retry) {
			return false;
		}
	}
	return true;
}

void TestSse(Checker &check) {
	const auto stream = QByteArray(
		"retry: 3000\n"
		"\n"
		"event: hello\n"
		"data: {\"rooms\":[\"3JWNR7QVN2\"],\"event_id\":1791327961597001,"
		"\"ts\":1791327962315}\n"
		"\n"
		": a comment\n"
		"id: 1791327961597000\n"
		"event: room.member_joined\n"
		"data: {\"room\":\"3JWNR7QVN2\",\"member\":{\"id\":9000000000777002}}\n"
		"\n"
		"id: 1791327964373000\n"
		"event: room.chat\n"
		"data: {\"room\":\"3JWNR7QVN2\",\"message\":{\"text\":\""
		"\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 "
		"\xF0\x9F\x94\xA5\"}}\n"
		"\n"
		"event: room.reaction\n"
		"data:{\"emoji\":\"\xF0\x9F\x94\xA5\"}\n"
		"\n"
		"data: first\n"
		"data: second\n"
		"unknown: field\n"
		"nocolon\n"
		"\n"
		"\n"
		"event: ping\n"
		"data: {\"ts\":1}\n"
		"\n"
		"event: tail without the empty line\n");

	auto whole = SseParser();
	const auto expected = whole.feed(stream);
	check(expected.size() == 7, "sse: seven frames");
	if (expected.size() != 7) {
		return;
	}
	check(
		expected[6].event == "ping" && expected[6].data == "{\"ts\":1}",
		"sse: an extra empty line and a frame after it");
	check(
		expected[0].retry == 3000
			&& expected[0].event.isEmpty()
			&& expected[0].data.isEmpty(),
		"sse: retry");
	check(
		expected[1].event == "hello"
			&& expected[1].id.isEmpty()
			&& expected[1].data.startsWith("{\"rooms\""),
		"sse: hello without an id");
	check(
		expected[2].id == "1791327961597000"
			&& expected[2].event == "room.member_joined",
		"sse: id and a comment before it");
	check(
		expected[3].data.contains("\xF0\x9F\x94\xA5")
			&& QJsonDocument::fromJson(expected[3].data).isObject(),
		"sse: UTF-8 data");
	check(
		expected[4].event == "room.reaction"
			&& expected[4].data == "{\"emoji\":\"\xF0\x9F\x94\xA5\"}",
		"sse: no space after the colon");
	check(
		expected[5].data == "first\nsecond" && expected[5].event.isEmpty(),
		"sse: several data lines, unknown fields ignored");

	auto bytewise = SseParser();
	auto collected = std::vector<SseParser::Frame>();
	for (const auto ch : stream) {
		for (auto &frame : bytewise.feed(QByteArray(1, ch))) {
			collected.push_back(std::move(frame));
		}
	}
	check(SameFrames(collected, expected), "sse: byte by byte");

	auto splits = true;
	for (auto i = 1; i < stream.size(); ++i) {
		auto parser = SseParser();
		auto frames = parser.feed(stream.left(i));
		for (auto &frame : parser.feed(stream.mid(i))) {
			frames.push_back(std::move(frame));
		}
		if (!SameFrames(frames, expected)) {
			splits = false;
			break;
		}
	}
	check(splits, "sse: every split in two chunks");

	auto crlf = stream;
	crlf.replace("\n", "\r\n");
	auto splitsCrlf = true;
	for (auto i = 1; i < crlf.size(); ++i) {
		auto parser = SseParser();
		auto frames = parser.feed(crlf.left(i));
		for (auto &frame : parser.feed(crlf.mid(i))) {
			frames.push_back(std::move(frame));
		}
		if (!SameFrames(frames, expected)) {
			splitsCrlf = false;
			break;
		}
	}
	check(splitsCrlf, "sse: CRLF line ends, every split");

	auto cr = stream;
	cr.replace("\n", "\r");
	auto single = SseParser();
	check(SameFrames(single.feed(cr), expected), "sse: CR line ends");

	auto tail = SseParser();
	auto frames = tail.feed("event: a\ndata: 1\n");
	check(frames.empty(), "sse: no frame before the empty line");
	frames = tail.feed("\n");
	check(
		frames.size() == 1 && frames[0].event == "a" && frames[0].data == "1",
		"sse: the empty line ends the frame");
	frames = tail.feed("\n\n\n");
	check(frames.empty(), "sse: empty lines alone give nothing");
	tail.reset();
	frames = tail.feed("id: 7\n\ndata: x\n\n");
	check(
		frames.size() == 1 && frames[0].id.isEmpty() && frames[0].data == "x",
		"sse: an id alone is no event and is not carried over");

	auto big = SseParser();
	const auto first = big.feed(QByteArray(kMaxSseLine + 16, 'x'));
	check(first.empty() && big.overflow(), "sse: an endless line overflows");
	check(big.feed("\ndata: y\n\n").empty(), "sse: nothing after overflow");
	big.reset();
	check(
		!big.overflow() && big.feed("data: y\n\n").size() == 1,
		"sse: reset");
}

void TestCursor(Checker &check) {
	const auto a = u"AAAAAAAAAA"_q;
	const auto b = u"BBBBBBBBBB"_q;
	auto cursor = StreamCursor();
	check(cursor.since({}) == 0, "cursor: a fresh one asks for nothing");
	cursor.connected(false, 1000, {});
	check(cursor.since({}) == 1000, "cursor: hello without since");
	check(cursor.accept(1001, QString()), "cursor: a new user event");
	check(!cursor.accept(1001, QString()), "cursor: the same again");
	check(!cursor.accept(999, QString()), "cursor: an older one");

	cursor.setRoomFloor(a, 900);
	check(
		cursor.since({ a }) == 900,
		"cursor: a room joins with an older snapshot");
	check(cursor.since({}) == 1001, "cursor: without it nothing changes");
	cursor.connected(true, 1100, { a });
	check(cursor.replaying(), "cursor: a stream with since starts by a replay");
	check(cursor.accept(950, a), "cursor: replay, a room event");
	check(!cursor.accept(1001, QString()), "cursor: replay, a seen user one");
	check(
		cursor.since({ a }) == 1001,
		"cursor: a replay that was cut continues from where it stopped");
	check(cursor.accept(1050, QString()), "cursor: replay, a missed user one");
	check(!cursor.accept(950, a), "cursor: replay, a room duplicate");
	check(
		cursor.since({ a }) == 1050 && cursor.since({}) == 1050,
		"cursor: a replayed event moves every cursor");
	check(cursor.accept(1101, a), "cursor: the first live event");
	check(!cursor.replaying(), "cursor: it ends the replay");
	check(
		cursor.since({ a }) == 1101 && cursor.since({}) == 1101,
		"cursor: a live event moves every cursor");
	cursor.setRoomFloor(a, 5);
	check(cursor.since({ a }) == 1101, "cursor: a floor never moves back");

	cursor.setRoomFloor(b, 1090);
	check(cursor.since({ a, b }) == 1090, "cursor: the second room");
	cursor.connected(true, 1200, { a, b });
	check(
		cursor.since({ a, b }) == 1090,
		"cursor: nothing moves before the first event of the replay");
	check(cursor.accept(1095, b), "cursor: replay, the second room");
	check(!cursor.accept(1101, a), "cursor: replay, a seen one of the first");
	check(
		cursor.since({ a, b }) == 1101,
		"cursor: every source follows the replay");
	cursor.heartbeat();
	check(
		cursor.since({ a, b }) == 1200,
		"cursor: a ping ends the replay");
	check(cursor.accept(1300, b), "cursor: live, room b");
	check(
		!cursor.accept(1250, a),
		"cursor: live events are in one order");
	cursor.removeRoom(b);
	cursor.setRoomFloor(b, 1250);
	check(
		cursor.since({ a }) == 1300 && cursor.since({ a, b }) == 1250,
		"cursor: a room that left and came back");
	cursor.reset();
	check(cursor.since({ a, b }) == 0, "cursor: reset");

	auto alone = StreamCursor();
	alone.setRoomFloor(a, 700);
	check(alone.since({ a }) == 700, "cursor: the first connect with a room");
	check(alone.since({ b }) == 0, "cursor: an unknown room");

	// A reader that is too slow for a busy room: the server cuts the
	// stream in the middle of the backlog, again and again. Every attempt
	// has to start after what the previous one has delivered.
	auto slow = StreamCursor();
	slow.connected(false, 500, {});
	slow.setRoomFloor(a, 400);
	auto asked = std::vector<int64>();
	auto delivered = 0;
	for (auto attempt = 0; attempt != 3; ++attempt) {
		const auto since = slow.since({ a });
		asked.push_back(since);
		slow.connected(true, 2000, { a });
		for (auto id = since + 1; id != since + 201; ++id) {
			if (slow.accept(id, (id % 5) ? a : QString())) {
				++delivered;
			}
		}
	}
	check(
		asked == std::vector<int64>{ 400, 600, 800 },
		"cursor: a slow reader moves on with every attempt");
	check(
		delivered == 580 && slow.since({ a }) == 1000,
		"cursor: and gets every event once");
}

void TestTiming(Checker &check) {
	const auto cap = crl::time(300'000);
	check(BackoffDelay(1, 0.5, cap) == 1000, "backoff: first");
	check(BackoffDelay(2, 0.5, cap) == 2000, "backoff: second");
	check(BackoffDelay(3, 0.5, cap) == 4000, "backoff: third");
	check(BackoffDelay(0, 0.5, cap) == 1000, "backoff: zero attempt");
	check(BackoffDelay(-3, 0.5, cap) == 1000, "backoff: negative attempt");
	check(BackoffDelay(1, 0., cap) == 750, "backoff: least jitter");
	check(BackoffDelay(1, 1., cap) == 1250, "backoff: most jitter");
	check(BackoffDelay(1, 7., cap) == 1250, "backoff: jitter is clamped");
	check(BackoffDelay(9, 0.5, cap) == 256'000, "backoff: ninth");
	check(BackoffDelay(10, 0.5, cap) == cap, "backoff: the cap");
	check(BackoffDelay(1000, 0.5, cap) == cap, "backoff: no overflow");
	check(BackoffDelay(5, 0.5, 8000) == 8000, "backoff: a small cap");
	auto grows = true;
	for (auto i = 1; i != 40; ++i) {
		if (BackoffDelay(i + 1, 0.3, cap) < BackoffDelay(i, 0.3, cap)) {
			grows = false;
		}
	}
	check(grows, "backoff: never shrinks");
	const auto jitter = RandomJitter();
	check(jitter >= 0. && jitter < 1., "backoff: random jitter range");

	check(
		StreamFlaps(0, 1000) == 1 && StreamFlaps(4, kStreamSilence) == 5,
		"stream: one that ends early is a flap");
	check(
		StreamFlaps(7, kStreamSilence + 1) == 0,
		"stream: one that lived long ends the row of flaps");
	check(
		StreamFlaps(1000, 0) == kStreamMaxFlaps,
		"stream: the flaps are capped");
	auto flaps = 0;
	auto waits = std::vector<crl::time>();
	for (auto i = 0; i != 7; ++i) {
		flaps = StreamFlaps(flaps, 2000);
		waits.push_back(BackoffDelay(1 + flaps, 0.5, kStreamBackoffCapInRoom));
	}
	check(
		waits == std::vector<crl::time>{
			2000, 4000, 8000, 16'000, 30'000, 30'000, 30'000 },
		"stream: every flap in a row doubles the wait up to the cap");

	const auto pick = PickClockSample({
		{ .sent = 1000, .received = 1080, .server = 5040. },
		{ .sent = 2000, .received = 2020, .server = 6010.5 },
		{ .sent = 3000, .received = 3300, .server = 7000. },
	});
	check(
		pick.valid && pick.rtt == 20 && std::abs(pick.offset - 4000.5) < 0.001,
		"clock: the fastest sample wins");
	check(!PickClockSample({}).valid, "clock: no samples");
	check(
		!PickClockSample({
			{ .sent = 100, .received = 90, .server = 5. },
			{ .sent = 100, .received = 100 + 60'000, .server = 5. },
			{ .sent = 100, .received = 110, .server = 0. },
		}).valid,
		"clock: impossible samples are skipped");
	const auto negative = PickClockSample({
		{ .sent = 5000, .received = 5010, .server = 1005. },
	});
	check(
		negative.valid && std::abs(negative.offset + 4000.) < 0.001,
		"clock: a server behind the local clock");

	auto offset = SlewedOffset();
	const auto now = crl::time(1'000'000);
	check(!offset.valid() && offset.value(now) == 0., "slew: empty");
	offset.apply(100., now);
	check(offset.valid() && offset.value(now) == 100., "slew: first at once");
	offset.apply(130., now);
	check(offset.value(now) == 100., "slew: a small change starts slowly");
	check(
		std::abs(offset.value(now + 5000) - 110.) < 0.001,
		"slew: 2 ms per second");
	check(offset.value(now + 60'000) == 130., "slew: reaches the target");
	offset.apply(90., now + 60'000);
	check(
		std::abs(offset.value(now + 70'000) - 110.) < 0.001,
		"slew: back, at the same speed");
	offset.apply(1000., now + 70'000);
	check(offset.value(now + 70'000) == 1000., "slew: a big change jumps");
	offset.apply(1100., now + 70'000);
	offset.apply(1150., now + 80'000);
	check(
		std::abs(offset.value(now + 80'000) - 1020.) < 0.001,
		"slew: a new target continues from where it is");
	check(offset.value(now - 5000) == 1020., "slew: time never goes back");

	const auto local = crl::now();
	check(
		std::abs(LocalTimeFor(ServerTimeFor(local)) - local) <= 1,
		"clock: local to server and back");
	check(
		std::abs(Now() - ServerTimeFor(crl::now())) <= 50,
		"clock: Now() is the server time of now");
}

void TestCrypto(Checker &check) {
	const auto key = RandomBytes(32);
	const auto plain = QByteArray("OJolNaPQycz8_qao.secret \xD0\xBF\x00z", 28);
	const auto aad = QByteArray("aad");
	const auto sealed = AesGcmSeal(key, plain, aad);
	check(sealed.size() == plain.size() + 28, "gcm: size");
	check(!sealed.contains("secret"), "gcm: not readable");
	check(AesGcmSeal(key, plain, aad) != sealed, "gcm: a new nonce every time");
	const auto opened = AesGcmOpen(key, sealed, aad);
	check(opened && *opened == plain, "gcm: round trip");
	check(!AesGcmOpen(key, sealed, "other"), "gcm: wrong aad");
	check(!AesGcmOpen(RandomBytes(32), sealed, aad), "gcm: wrong key");
	auto broken = sealed;
	broken[broken.size() - 1] = char(broken[broken.size() - 1] ^ 1);
	check(!AesGcmOpen(key, broken, aad), "gcm: a changed byte");
	broken = sealed;
	broken[14] = char(broken[14] ^ 1);
	check(!AesGcmOpen(key, broken, aad), "gcm: a changed tag");
	check(!AesGcmOpen(key, sealed.left(20), aad), "gcm: too short");
	check(AesGcmSeal(key.left(16), plain).isEmpty(), "gcm: key size");
	const auto empty = AesGcmSeal(key, QByteArray());
	const auto emptyOpened = AesGcmOpen(key, empty);
	check(
		empty.size() == 28 && emptyOpened && emptyOpened->isEmpty(),
		"gcm: empty text");

	check(
		SlowKey("password", "salt", 1).toHex()
			== "120fb6cffcf8b32c43e7225256c4f837"
				"a86548c92ccc35480805987cb70be17b",
		"pbkdf2: one iteration");
	check(
		SlowKey("password", "salt", 2).toHex()
			== "ae4d0c95af6b46d32d0adff928f06dd0"
				"2a303f8ef3c251dfd6e2d85a95474c43",
		"pbkdf2: two iterations");
	check(SlowKey("p", "s", 0).isEmpty(), "pbkdf2: no iterations");
	check(SlowKey("p", "s", 1, 64).size() == 64, "pbkdf2: length");
	check(
		Sha256("abc").toHex()
			== "ba7816bf8f01cfea414140de5dae2223"
				"b00361a396177a9cb410ff61f20015ad",
		"sha256");
	check(
		RandomBytes(16).size() == 16
			&& RandomBytes(0).isEmpty()
			&& RandomBytes(16) != RandomBytes(16),
		"random bytes");
}

void TestIdentity(Checker &check, const QString &folder) {
	const auto userId = uint64(9000000000000042);
	const auto key = RandomBytes(32);
	check(!LoadIdentity(folder, userId, key), "identity: no file");

	auto identity = Identity{
		.userId = userId,
		.consent = true,
		.disconnected = true,
		.rejected = true,
		.consentAt = 1791327935,
		.deviceId = u"OJolNaPQycz8_qao"_q,
		.token = u"OJolNaPQycz8_qao.OJdrdQovFM0_RfVm5FKwOxrCT-ioJk3Sx9JP"_q,
		.pending = u"PnD1ngSecret0_RfVm5FKwOxrCT-ioJk3Sx9JPa1b2C"_q,
		.name = QString::fromUtf8("\xD0\x9C\xD0\xB8\xD1\x88\xD0\xB0"),
		.me = ParseObject("{\"id\":9000000000000042,\"name\":\"x\"}"),
	};
	check(
		ValidDeviceSecret(identity.pending)
			&& !ValidDeviceSecret(identity.pending.left(42))
			&& !ValidDeviceSecret(identity.pending + QChar('A'))
			&& !ValidDeviceSecret(identity.pending.left(42) + QChar('+'))
			&& !ValidDeviceSecret(identity.pending.left(42) + QChar('='))
			&& !ValidDeviceSecret(identity.pending.left(42) + QChar('.'))
			&& !ValidDeviceSecret(QString()),
		"secret: exactly 43 characters of base64url");
	const auto made = NewDeviceSecret();
	check(
		ValidDeviceSecret(made)
			&& (made != NewDeviceSecret())
			&& (QByteArray::fromBase64(
				made.toLatin1(),
				QByteArray::Base64UrlEncoding).size() == 32),
		"secret: 256 new random bits every time");
	check(SaveIdentity(folder, identity, key), "identity: saved");
	auto file = QFile(IdentityPath(folder));
	const auto opened = file.open(QIODevice::ReadOnly);
	const auto bytes = opened ? file.readAll() : QByteArray();
	file.close();
	check(
		bytes.contains("token_sealed") && !bytes.contains("OJdrdQovFM0"),
		"identity: the key is not readable in the file");
	check(
		bytes.contains("pending_sealed") && !bytes.contains("PnD1ngSecret"),
		"identity: the secret on its way is not readable in the file");
	auto swapped = QJsonDocument::fromJson(bytes).object();
	swapped.insert(u"token_sealed"_q, swapped.value(u"pending_sealed"_q));
	const auto mixed = ParseIdentity(
		QJsonDocument(swapped).toJson(QJsonDocument::Compact),
		userId,
		key);
	check(
		mixed && mixed->token.isEmpty() && mixed->pending == identity.pending,
		"identity: a sealed secret is not taken for a key");
	const auto loaded = LoadIdentity(folder, userId, key);
	check(
		loaded
			&& loaded->userId == identity.userId
			&& loaded->consent
			&& loaded->disconnected
			&& loaded->rejected
			&& loaded->consentAt == identity.consentAt
			&& loaded->deviceId == identity.deviceId
			&& loaded->token == identity.token
			&& loaded->pending == identity.pending
			&& loaded->name == identity.name
			&& loaded->me == identity.me,
		"identity: round trip");
	const auto wrongKey = LoadIdentity(folder, userId, RandomBytes(32));
	check(
		wrongKey
			&& wrongKey->consent
			&& wrongKey->token.isEmpty()
			&& wrongKey->pending.isEmpty(),
		"identity: a wrong key loses only the key");
	check(
		!LoadIdentity(folder, userId + 1, key),
		"identity: a file of another account is not used");

	check(SaveIdentity(folder, identity, QByteArray()), "identity: no seal");
	const auto plain = LoadIdentity(folder, userId, QByteArray());
	check(
		plain
			&& plain->token == identity.token
			&& plain->pending == identity.pending,
		"identity: plain token");

	// A registration that is on its way: the consent and the secret that
	// was sent, no key yet. This is what the next launch finds when the
	// answer was lost, and what it registers with again.
	identity.token = QString();
	identity.deviceId = QString();
	check(SaveIdentity(folder, identity, key), "identity: saved on the way");
	const auto onTheWay = LoadIdentity(folder, userId, key);
	check(
		onTheWay
			&& onTheWay->consent
			&& onTheWay->token.isEmpty()
			&& onTheWay->pending == identity.pending,
		"identity: the secret of a lost answer is there at the next launch");

	// The answer is stored: nothing is on its way any more.
	identity.pending = QString();
	identity.consent = false;
	check(SaveIdentity(folder, identity, key), "identity: saved again");
	const auto withdrawn = LoadIdentity(folder, userId, key);
	check(
		withdrawn
			&& !withdrawn->consent
			&& withdrawn->token.isEmpty()
			&& withdrawn->pending.isEmpty(),
		"identity: no consent, no key");
	auto after = QFile(IdentityPath(folder));
	check(
		after.open(QIODevice::ReadOnly) && !after.readAll().contains("pending"),
		"identity: nothing about the secret is left in the file");
	after.close();

	auto corrupt = QFile(IdentityPath(folder));
	if (corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		corrupt.write("{\"user_id\":\"9000000000000042\",\"consent\":tr");
		corrupt.close();
	}
	check(!LoadIdentity(folder, userId, key), "identity: a broken file");
	check(
		!ParseIdentity("{\"consent\":true}", userId, key),
		"identity: no user id");
	const auto badToken = ParseIdentity(
		"{\"user_id\":\"9000000000000042\",\"consent\":true,"
		"\"token\":\"with space.x\"}",
		userId,
		key);
	check(
		badToken && badToken->token.isEmpty(),
		"identity: a malformed key is dropped");
	const auto badPending = ParseIdentity(
		"{\"user_id\":\"9000000000000042\",\"consent\":true,"
		"\"pending\":\"too-short\"}",
		userId,
		key);
	check(
		badPending && badPending->consent && badPending->pending.isEmpty(),
		"identity: a malformed secret is dropped");
	QFile::remove(IdentityPath(folder));
}

// The gate: an account without a consent sends nothing, whatever a
// feature asks it for.
void TestGate(Checker &check, const QString &folder) {
	auto account = std::make_unique<Account>(Account::Descriptor{
		.userId = uint64(9000000000000043),
		.folder = folder + u"gate/"_q,
	});
	check(
		account->state() == State::NoConsent
			&& !account->ready()
			&& !account->consented(),
		"gate: a new account has no consent");
	check(
		!QFileInfo::exists(IdentityPath(folder + u"gate/"_q)),
		"gate: nothing is written before the consent");

	auto failures = std::vector<Error::Type>();
	const auto fail = [&](const Error &error) {
		failures.push_back(error.type);
	};
	const auto done = [&](const Response &) {
		failures.push_back(Error::Type::None);
	};
	check(!account->request(GetRequest(u"/v1/me"_q), done, fail), "gate: auth");
	auto open = GetRequest(u"/v1/hello"_q);
	open.auth = false;
	check(!account->request(std::move(open), done, fail), "gate: no auth");
	check(
		!account->upload({
			.bytes = "x",
			.kind = u"audio"_q,
			.fail = fail,
		}),
		"gate: upload");
	check(
		!account->download({
			.media = QString(64, QChar('a')),
			.to = folder + u"gate/file"_q,
			.fail = fail,
		}),
		"gate: download");
	auto linkFailed = false;
	account->createLinkCode(nullptr, [&](const Error &error) {
		linkFailed = (error.type == Error::Type::NotConnected);
	});
	auto deleteFailed = false;
	account->deleteData(nullptr, [&](const Error &error) {
		deleteFailed = (error.type == Error::Type::NotConnected);
	});
	auto redeemFailed = false;
	account->redeemLinkCode(u"AAAA-BBBB"_q, nullptr, [&](const Error &error) {
		redeemFailed = (error.type == Error::Type::NotConnected);
	});
	auto synced = -1;
	account->syncTime([&](bool success) {
		synced = success ? 1 : 0;
	});
	const auto arrived = WaitFor([&] {
		return (failures.size() == 4)
			&& linkFailed
			&& deleteFailed
			&& redeemFailed;
	}, 3000);
	check(arrived, "gate: every call fails from the event loop");
	check(
		ranges::all_of(failures, [](Error::Type type) {
			return (type == Error::Type::NotConnected);
		}),
		"gate: all with NotConnected");
	check(synced == 0, "gate: no time request either");
	account->switchOn();
	account->switchOff();
	check(
		account->state() == State::NoConsent,
		"gate: on / off do nothing without a consent");

	{
		auto dropped = 0;
		auto sender = std::make_unique<Sender>(account.get());
		sender->request(GetRequest(u"/v1/me"_q), nullptr, [&](const Error &) {
			++dropped;
		});
		sender = nullptr;
		auto ticks = 0;
		const auto waited = WaitFor([&] {
			return (++ticks > 20);
		}, 3000);
		check(waited && !dropped, "sender: nothing after it is destroyed");
	}

	auto unavailable = std::make_unique<Account>(Account::Descriptor{
		.userId = uint64(9000000000000044),
		.folder = folder + u"test_dc/"_q,
		.available = false,
	});
	unavailable->agree(u"x"_q);
	check(
		unavailable->state() == State::NoConsent && !unavailable->consented(),
		"gate: a Telegram test server account can't agree");
	account = nullptr;
	unavailable = nullptr;
}

// Everybody who asks for one file shares one transfer. Nothing is
// requested here: the file is there already, so the transfer ends from
// the event loop without a word to the server.
void TestTransfers(Checker &check, const QString &folder) {
	const auto target = folder + u"shared/file.bin"_q;
	QDir().mkpath(folder + u"shared"_q);
	auto file = QFile(target);
	const auto written = file.open(QIODevice::WriteOnly)
		&& (file.write("x") == 1);
	file.close();
	check(written, "transfers: a sample file");

	auto client = std::make_unique<Client>(nullptr);
	auto paths = QStringList();
	auto failures = 0;
	auto cancelled = 0;
	const auto ask = [&](Fn<void(const QString &path)> done) {
		return client->download({
			.path = u"/v1/updates/files/selftest"_q,
			.to = target,
			.auth = false,
			.done = std::move(done),
			.fail = [&](const Error &) { ++failures; },
		});
	};
	const auto keep = [&](const QString &path) {
		paths.push_back(path);
	};
	const auto drop = [&](const QString &path) {
		++cancelled;
	};
	const auto first = ask(keep);
	const auto second = ask(keep);
	const auto third = ask(drop);
	check(
		first && second && third && (first != second) && (second != third),
		"transfers: every caller has an id of its own");
	client->cancelTransfer(third);

	auto refused = Error();
	const auto other = client->download({
		.path = u"/v1/updates/files/another"_q,
		.to = target,
		.auth = false,
		.done = drop,
		.fail = [&](const Error &error) { refused = error; },
	});
	const auto finished = WaitFor([&] {
		return (paths.size() == 2) && (refused.type != Error::Type::None);
	}, 3000);
	check(
		finished && (paths == QStringList{ target, target }),
		"transfers: two callers of one file are served by one transfer");
	check(
		!other && (refused.type == Error::Type::File),
		"transfers: another source for the same file is refused");

	const auto lone = ask(drop);
	client->cancelTransfer(lone);
	const auto again = ask(keep);
	const auto repeated = WaitFor([&] {
		return (paths.size() == 3);
	}, 3000);
	check(
		lone && again && repeated,
		"transfers: the file can be asked for again after a cancel");
	check(
		!cancelled && !failures,
		"transfers: a cancelled caller hears nothing");

	const auto key = TargetKey(target);
	check(
		TargetKey(folder + u"shared/../shared/file.bin"_q) == key,
		"transfers: one file has one key, however its path is spelled");
	check(
		BusyTargets().emplace(key).second
			&& !BusyTargets().emplace(key).second
			&& BusyTargets().remove(key)
			&& BusyTargets().emplace(key).second,
		"transfers: one account at a time writes a file");
	BusyTargets().remove(key);
	client = nullptr;
}

[[nodiscard]] QString DescribeError(const Error &error) {
	return u"type %1, status %2, code '%3'"_q.arg(
		QString::number(int(error.type)),
		QString::number(error.status),
		error.code);
}

[[nodiscard]] bool HasEvent(
		const std::vector<Event> &list,
		const QString &type) {
	return ranges::any_of(list, [&](const Event &event) {
		return (event.type == type);
	});
}

// The answer to a registration is lost. What the device has then is its
// consent and the secret it has sent (Identity::pending), and it registers
// again with them. A server that knows "secret" gives the same device
// back, an older one refuses with 409 as it always did: the client works
// with both, the log says which of them this server is.
void TestLiveReplay(
		Checker &check,
		QStringList &log,
		const QString &folder,
		uint64 userId,
		const QByteArray &testKey) {
	const auto firstFolder = folder + u"replay_first/"_q;
	const auto againFolder = folder + u"replay_again/"_q;
	auto first = std::make_unique<Account>(Account::Descriptor{
		.userId = userId,
		.folder = firstFolder,
		.testKey = testKey,
	});
	first->agree(QString());
	const auto registered = WaitFor([&] { return first->ready(); }, 25'000);
	const auto kept = LoadIdentity(firstFolder, userId, QByteArray());
	check(
		registered
			&& kept
			&& ValidToken(kept->token)
			&& kept->pending.isEmpty(),
		"live: registered, no secret is left on its way");
	if (!registered || !kept || !ValidToken(kept->token)) {
		log.push_back(u"live: replay: "_q + DescribeError(first->lastError()));
		return;
	}
	const auto secret = kept->token.mid(kept->token.indexOf(QChar('.')) + 1);
	auto again = std::unique_ptr<Account>();
	if (!ValidDeviceSecret(secret)) {
		log.push_back(u"live: replay: skipped, the key has another form"_q);
	} else {
		// The file as it was at the moment the answer got lost.
		const auto lost = Identity{
			.userId = userId,
			.consent = true,
			.consentAt = kept->consentAt,
			.pending = secret,
		};
		const auto written = SaveIdentity(againFolder, lost, QByteArray());
		again = std::make_unique<Account>(Account::Descriptor{
			.userId = userId,
			.folder = againFolder,
			.testKey = testKey,
		});
		const auto answered = written && WaitFor([&] {
			return again->ready() || (again->state() == State::NeedsLink);
		}, 25'000);
		const auto now = LoadIdentity(againFolder, userId, QByteArray());
		const auto same = answered
			&& again->ready()
			&& now
			&& (now->token == kept->token)
			&& now->pending.isEmpty();
		const auto refused = answered
			&& !again->ready()
			&& again->lastError().is("already_registered")
			&& now
			&& (now->pending == secret);
		check(
			same || refused,
			"live: a registration sent again: the same device, or 409");
		log.push_back(same
			? u"live: replay: the same device again, registration is "_q
				+ u"idempotent on this server"_q
			: refused
			? u"live: replay: 409, this server does not know \"secret\""_q
			: (u"live: replay: "_q + DescribeError(again->lastError())));
	}

	// Both objects are one device of one test id: it is deleted once.
	again = nullptr;
	auto finished = false;
	first->deleteData([&] {
		finished = true;
	}, [&](const Error &error) {
		finished = true;
		log.push_back(u"live: replay: cleanup failed, "_q + DescribeError(error));
	});
	WaitFor([&] { return finished; }, 15'000);
}

// Against the real server, with fake ids from its reserved test range:
// they live in a world of their own and are erased by the test itself
// (and by the server two hours later, if the test died on the way).
void TestLive(Checker &check, QStringList &log, const QString &folder) {
	const auto first = uint64(9000000000000000)
		+ 100000
		+ (base::RandomValue<uint32>() % 800000);
	log.push_back(u"live: %1, test ids %2 and %3"_q.arg(
		BaseUrl(),
		QString::number(first),
		QString::number(first + 1)));

	// The key the server wants for its fake test ids comes only from the
	// environment of the one who runs the test, it is never in the app.
	const auto testKey = qEnvironmentVariable(
		kTestKeyVariable).trimmed().toLatin1();
	log.push_back(testKey.isEmpty()
		? u"live: no test key, %1 is not set"_q.arg(
			QString::fromLatin1(kTestKeyVariable))
		: !ValidHeaderValue(testKey)
		? u"live: the test key in %1 can't be sent as a header"_q.arg(
			QString::fromLatin1(kTestKeyVariable))
		: u"live: registrations carry the test key in %1"_q.arg(
			QString::fromLatin1(kTestKeyHeader)));

	auto a = std::make_unique<Account>(Account::Descriptor{
		.userId = first,
		.folder = folder + u"a/"_q,
		.sealKey = RandomBytes(32),
		.testKey = testKey,
	});
	auto b = std::make_unique<Account>(Account::Descriptor{
		.userId = first + 1,
		.folder = folder + u"b/"_q,
		.testKey = testKey,
	});
	auto c = std::unique_ptr<Account>();
	const auto nameA = QString::fromUtf8(
		"\xD0\xA1\xD0\xB0\xD0\xBC\xD0\xBE\xD1\x82\xD0\xB5\xD1\x81\xD1\x82 A");
	const auto cleanup = [&] {
		for (const auto account : { a.get(), b.get() }) {
			if (!account || !account->ready()) {
				continue;
			}
			auto finished = false;
			account->deleteData([&] {
				finished = true;
			}, [&](const Error &error) {
				finished = true;
				log.push_back(u"live: cleanup failed, "_q + DescribeError(error));
			});
			WaitFor([&] { return finished; }, 15'000);
		}
	};

	a->agree(nameA);
	b->agree(QString());
	const auto registered = WaitFor([&] {
		return a->ready() && b->ready();
	}, 25'000);
	check(registered, "live: two identities are registered");
	if (!registered) {
		log.push_back(u"live: a: "_q + DescribeError(a->lastError()));
		log.push_back(u"live: b: "_q + DescribeError(b->lastError()));
		cleanup();
		return;
	}
	if (!CurrentConfig().insecure) {
		auto ignored = QStringList();
		for (const auto code : PinErrorsSeen()) {
			ignored.push_back(QString::number(code));
		}
		log.push_back(ignored.isEmpty()
			? u"live: tls: the pinned certificate passed without errors"_q
			: (u"live: tls: errors of the pinned certificate that were "_q
				+ u"ignored by their list (QSslError codes): "_q
				+ ignored.join(u", "_q)));
	}
	check(
		a->me().id == first && a->me().test && a->me().name == nameA,
		"live: the own user object, a test id with the chosen name");
	check(
		a->me().profileAudience == u"nobody"_q && !a->me().badge,
		"live: a new account is private");
	check(
		a->hello().valid && a->feature("rooms") && a->feature("media_relay"),
		"live: hello with the features");
	check(a->limit("chunk_max", 0) > 0, "live: limits");
	check(
		QFileInfo::exists(IdentityPath(folder + u"a/"_q)),
		"live: the key file is written");

	const auto online = WaitFor([&] {
		return (a->state() == State::Online) && (b->state() == State::Online);
	}, 20'000);
	check(online, "live: both event streams are open");

	auto syncs = 0;
	auto syncOk = false;
	a->syncTime([&](bool success) {
		syncOk = success;
		++syncs;
	});
	WaitFor([&] { return syncs > 0; }, 30'000);
	check(syncOk && TimeSynced(), "live: the clock offset is measured");
	const auto offsetFirst = Now() - LocalWallNow();
	b->syncTime([&](bool success) {
		syncOk = success;
		++syncs;
	});
	WaitFor([&] { return syncs > 1; }, 30'000);
	const auto offsetSecond = Now() - LocalWallNow();
	check(
		syncOk && std::abs(offsetSecond - offsetFirst) < 300,
		"live: two measurements agree");
	log.push_back(u"live: clock offset %1 ms, then %2 ms"_q.arg(
		QString::number(offsetFirst),
		QString::number(offsetSecond)));

	auto code = QString();
	auto roomEventId = int64(0);
	auto joinEventId = int64(0);
	auto step = 0;
	auto failed = Error();
	const auto fail = [&](const Error &error) {
		failed = error;
		step = -1;
	};
	auto title = QJsonObject();
	title.insert(u"title"_q, nameA);
	a->request(PostRequest(u"/v1/rooms"_q, title), [&](const Response &r) {
		const auto room = r.json.value(u"room"_q).toObject();
		code = NormalizeRoomCode(room.value(u"code"_q).toString());
		roomEventId = JsonInt(room.value(u"event_id"_q));
		step = 1;
	}, fail);
	WaitFor([&] { return step != 0; }, 20'000);
	check(
		step == 1 && !code.isEmpty() && roomEventId > 0,
		"live: a room is created");
	if (step != 1 || code.isEmpty()) {
		log.push_back(u"live: room: "_q + DescribeError(failed));
		cleanup();
		return;
	}
	check(
		ParseLink(MakeLink(LinkKind::Room, code)).id == code,
		"live: the link of the room is recognised");
	const auto roomPath = u"/v1/rooms/"_q + code;
	step = 0;
	b->request(PostRequest(roomPath + u"/join"_q), [&](const Response &r) {
		const auto room = r.json.value(u"room"_q).toObject();
		joinEventId = JsonInt(room.value(u"event_id"_q));
		step = (room.value(u"members"_q).toArray().size() == 2) ? 1 : -1;
	}, fail);
	WaitFor([&] { return step != 0; }, 20'000);
	check(step == 1 && joinEventId > roomEventId, "live: the second one joins");

	auto gotA = std::vector<Event>();
	auto gotB = std::vector<Event>();
	auto hellosA = 0;
	auto lifetime = rpl::lifetime();
	a->events() | rpl::on_next([&](const Event &event) {
		if (event.type == u"hello"_q) {
			++hellosA;
		}
	}, lifetime);
	a->roomEvents(code) | rpl::on_next([&](const Event &event) {
		gotA.push_back(event);
	}, lifetime);
	b->roomEvents(code) | rpl::on_next([&](const Event &event) {
		gotB.push_back(event);
	}, lifetime);
	auto inRoomA = a->subscribeRoom(code, roomEventId);
	auto inRoomB = b->subscribeRoom(code, joinEventId);
	const auto replayed = WaitFor([&] {
		return HasEvent(gotA, u"room.member_joined"_q);
	}, 20'000);
	check(replayed, "live: the stream replays what came after the snapshot");
	const auto present = WaitFor([&] {
		return ranges::any_of(gotA, [&](const Event &event) {
			return (event.type == u"room.presence"_q)
				&& (JsonUserId(event.data.value(u"user_id"_q)) == first + 1)
				&& event.data.value(u"online"_q).toBool();
		});
	}, 20'000);
	check(present, "live: the other member is seen online in the room");
	check(
		!HasEvent(gotB, u"room.member_joined"_q),
		"live: nothing from before the own snapshot is replayed");

	const auto text = QString::fromUtf8(
		"\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xF0\x9F\x94\xA5");
	auto chat = QJsonObject();
	chat.insert(u"text"_q, text);
	chat.insert(u"client_id"_q, u"selftest-1"_q);
	step = 0;
	b->request(PostRequest(roomPath + u"/chat"_q, chat), [&](const Response &r) {
		step = (r.status == 201) ? 1 : -1;
	}, fail);
	const auto chatOf = [&](const std::vector<Event> &list) {
		return int(ranges::count_if(list, [&](const Event &event) {
			const auto message = event.data.value(u"message"_q).toObject();
			return (event.type == u"room.chat"_q)
				&& (message.value(u"text"_q).toString() == text);
		}));
	};
	const auto chatted = WaitFor([&] {
		return (step != 0) && chatOf(gotA) && chatOf(gotB);
	}, 20'000);
	check(chatted && step == 1, "live: a chat message reaches both");
	auto reaction = QJsonObject();
	reaction.insert(u"emoji"_q, QString::fromUtf8("\xF0\x9F\x94\xA5"));
	b->request(PostRequest(roomPath + u"/react"_q, reaction), nullptr, fail);
	const auto reacted = WaitFor([&] {
		return HasEvent(gotA, u"room.reaction"_q);
	}, 20'000);
	check(reacted, "live: a volatile event arrives");
	check(
		ranges::all_of(gotA, [](const Event &event) {
			return (event.type != u"room.reaction"_q) || (event.id == 0);
		}),
		"live: and has no id");

	// The room window is closed and opened again: the stream reconnects
	// without the room and then with it, from the last event that was
	// seen. What happened in between arrives, nothing arrives twice.
	auto lastSeen = int64(0);
	for (const auto &event : gotA) {
		lastSeen = std::max(lastSeen, event.id);
	}
	const auto hellosBefore = hellosA;
	inRoomA.destroy();
	const auto left = WaitFor([&] {
		return (hellosA > hellosBefore);
	}, 20'000);
	check(left, "live: closing the room reconnects the stream without it");
	const auto secondOf = [&](const std::vector<Event> &list) {
		return int(ranges::count_if(list, [&](const Event &event) {
			const auto message = event.data.value(u"message"_q).toObject();
			return (event.type == u"room.chat"_q)
				&& (message.value(u"text"_q).toString() == u"second"_q);
		}));
	};
	chat.insert(u"text"_q, u"second"_q);
	chat.insert(u"client_id"_q, u"selftest-2"_q);
	step = 0;
	b->request(PostRequest(roomPath + u"/chat"_q, chat), [&](const Response &r) {
		step = 1;
	}, fail);
	WaitFor([&] { return (step != 0) && secondOf(gotB); }, 20'000);
	check(
		step == 1 && !secondOf(gotA),
		"live: a room that is not in the stream sends nothing");
	inRoomA = a->subscribeRoom(code, lastSeen);
	const auto again = WaitFor([&] {
		return secondOf(gotA) > 0;
	}, 25'000);
	check(again, "live: what was missed arrives after the room is back");
	check(
		chatOf(gotA) == 1 && secondOf(gotA) == 1,
		"live: and nothing is delivered twice");

	const auto size = 300'000;
	auto blob = QByteArray(size, Qt::Uninitialized);
	auto seed = uint32(first & 0xFFFFFFFFU) | 1U;
	for (auto i = 0; i != size; ++i) {
		seed = seed * 1664525U + 1013904223U;
		blob[i] = char(seed >> 24);
	}
	const auto sha = QString::fromLatin1(Sha256(blob).toHex());
	const auto source = folder + u"blob.bin"_q;
	auto file = QFile(source);
	const auto written = file.open(QIODevice::WriteOnly)
		&& (file.write(blob) == blob.size());
	file.close();
	check(written, "live: a sample file");

	auto uploaded = QString();
	auto progressCalls = 0;
	auto progressLast = int64(0);
	step = 0;
	a->upload({
		.path = source,
		.kind = u"audio"_q,
		.mime = u"application/octet-stream"_q,
		.done = [&](const QString &media, int64 total) {
			uploaded = media;
			step = (total == size) ? 1 : -1;
		},
		.fail = fail,
		.progress = [&](int64 ready, int64 total) {
			++progressCalls;
			progressLast = ready;
		},
		.chunkSize = 128 * 1024,
	});
	WaitFor([&] { return step != 0; }, 90'000);
	check(step == 1 && uploaded == sha, "live: a chunked upload");
	if (step != 1) {
		log.push_back(u"live: upload: "_q + DescribeError(failed));
	}
	check(
		progressCalls >= 3 && progressLast == size,
		"live: the upload reports its progress");
	step = 0;
	b->upload({
		.bytes = blob,
		.kind = u"audio"_q,
		.done = [&](const QString &media, int64 total) {
			step = (media == sha) ? 1 : -1;
		},
		.fail = fail,
	});
	WaitFor([&] { return step != 0; }, 30'000);
	check(step == 1, "live: the same file is taken from another user too");
	step = 0;
	a->upload({
		.bytes = blob,
		.kind = u"audio"_q,
		.done = [&](const QString &media, int64 total) {
			step = (media == sha) ? 1 : -1;
		},
		.fail = fail,
	});
	WaitFor([&] { return step != 0; }, 30'000);
	check(step == 1, "live: a file the server has from its owner is known");

	const auto target = folder + u"dl/blob.bin"_q;
	step = 0;
	b->download({
		.media = sha,
		.to = target,
		.done = [&](const QString &path) { step = 1; },
		.fail = fail,
	});
	WaitFor([&] { return step != 0; }, 30'000);
	check(
		step == -1 && failed.status == 404,
		"live: a file nobody referenced can't be read");

	auto item = QJsonObject();
	item.insert(u"media"_q, sha);
	item.insert(u"title"_q, u"Selftest"_q);
	item.insert(u"performer"_q, u"Oblivion"_q);
	item.insert(u"duration_ms"_q, 215000);
	auto items = QJsonArray();
	items.push_back(item);
	auto queue = QJsonObject();
	queue.insert(u"items"_q, items);
	step = 0;
	a->request(
		PostRequest(roomPath + u"/queue/music"_q, queue),
		[&](const Response &r) { step = (r.status == 201) ? 1 : -1; },
		fail);
	const auto queued = WaitFor([&] {
		return (step != 0) && HasEvent(gotB, u"room.queue"_q);
	}, 20'000);
	check(queued && step == 1, "live: the queue of the room gets the track");

	QDir().mkpath(folder + u"dl"_q);
	auto part = QFile(target + u".part"_q);
	if (part.open(QIODevice::WriteOnly)) {
		part.write(blob.left(1000));
		part.close();
	}
	step = 0;
	progressLast = 0;
	b->download({
		.media = sha,
		.to = target,
		.done = [&](const QString &path) { step = (path == target) ? 1 : -1; },
		.fail = fail,
		.progress = [&](int64 ready, int64 total) { progressLast = ready; },
	});
	WaitFor([&] { return step != 0; }, 60'000);
	auto result = QFile(target);
	const auto same = result.open(QIODevice::ReadOnly)
		&& (result.readAll() == blob);
	result.close();
	check(
		step == 1 && same && progressLast == size,
		"live: a download resumed from a part is the same file");
	if (step != 1) {
		log.push_back(u"live: download: "_q + DescribeError(failed));
	}
	check(
		!QFileInfo::exists(target + u".part"_q),
		"live: the part file is gone");

	auto linkCode = QString();
	step = 0;
	a->createLinkCode([&](const LinkCode &value) {
		linkCode = value.code;
		step = (value.expiresAt > Now()) ? 1 : -1;
	}, fail);
	WaitFor([&] { return step != 0; }, 20'000);
	check(step == 1 && linkCode.size() >= 8, "live: a link code");
	c = std::make_unique<Account>(Account::Descriptor{
		.userId = first,
		.folder = folder + u"c/"_q,
		.testKey = testKey,
	});
	c->agree(QString());
	const auto bound = WaitFor([&] {
		return (c->state() == State::NeedsLink);
	}, 25'000);
	check(
		bound && !c->ready() && c->lastError().is("already_registered"),
		"live: a second device of a bound id needs the code");
	step = 0;
	c->redeemLinkCode(linkCode.toLower(), [&] { step = 1; }, fail);
	WaitFor([&] { return step != 0; }, 20'000);
	check(
		step == 1 && c->ready() && c->me().id == first,
		"live: the code links the second device");
	if (step != 1) {
		log.push_back(u"live: redeem: "_q + DescribeError(failed));
	}
	const auto second = WaitFor([&] {
		return (c->state() == State::Online);
	}, 20'000);
	check(second, "live: the second device opens its stream");

	b->switchOff();
	check(
		b->state() == State::Disconnected && !b->ready() && b->consented(),
		"live: switched off");
	auto offFailed = false;
	b->request(GetRequest(u"/v1/me"_q), nullptr, [&](const Error &error) {
		offFailed = (error.type == Error::Type::NotConnected);
	});
	WaitFor([&] { return offFailed; }, 3000);
	check(offFailed, "live: nothing is sent while it is off");
	b->switchOn();
	const auto back = WaitFor([&] {
		return b->ready() && (b->state() == State::Online);
	}, 25'000);
	check(back, "live: switched on again with the kept key");

	lifetime.destroy();
	inRoomA.destroy();
	inRoomB.destroy();
	// The server says "bye, deleted" in the event stream before it
	// answers the request, so either of the two may come first here: the
	// one who asked has to hear "done" in both orders, and only once.
	step = 0;
	failed = Error();
	auto deletedA = 0;
	a->deleteData([&] {
		++deletedA;
		step = 1;
	}, fail);
	WaitFor([&] { return step != 0; }, 20'000);
	check(
		step == 1
			&& a->state() == State::NoConsent
			&& !a->consented()
			&& !QFileInfo::exists(IdentityPath(folder + u"a/"_q)),
		"live: the data is deleted and the key is forgotten");
	if (step != 1 || a->state() != State::NoConsent) {
		log.push_back(u"live: delete: step %1, state %2, %3"_q.arg(
			QString::number(step),
			QString::number(int(a->state())),
			DescribeError(failed)));
	}
	const auto told = WaitFor([&] {
		return (c->state() == State::NoConsent)
			|| (c->state() == State::Disconnected);
	}, 20'000);
	check(
		told && !c->ready(),
		"live: the other device learns about the deletion");
	step = 0;
	failed = Error();
	auto deletedB = 0;
	b->deleteData([&] { ++deletedB; }, fail);
	b->deleteData([&] {
		++deletedB;
		step = 1;
	}, fail);
	WaitFor([&] { return step != 0; }, 20'000);
	check(step == 1 && !b->ready(), "live: the second identity is deleted");
	if (step != 1) {
		log.push_back(u"live: delete: step %1, state %2, %3"_q.arg(
			QString::number(step),
			QString::number(int(b->state())),
			DescribeError(failed)));
	}
	auto ticks = 0;
	WaitFor([&] { return (++ticks > 20); }, 3000);
	check(
		deletedA == 1 && deletedB == 2,
		"live: everybody who asked for the deletion is told once");
	cleanup();
	c = nullptr;
	b = nullptr;
	a = nullptr;

	// One more test id, registered and deleted by this check alone.
	TestLiveReplay(check, log, folder, first + 2, testKey);
}

} // namespace

bool RunSelfTest(QStringList &log) {
	auto check = Checker(log);
	const auto folder = cWorkingDir() + u"oblivion_selftest_cloud/"_q;
	QDir(folder).removeRecursively();
	QDir().mkpath(folder);

	TestConfig(check);
	check.section("config and pin");
	TestUrls(check);
	check.section("urls and links");
	TestJson(check);
	check.section("json");
	TestSse(check);
	check.section("sse parser");
	TestCursor(check);
	check.section("stream cursor");
	TestTiming(check);
	check.section("backoff and clock");
	TestCrypto(check);
	check.section("crypto");
	TestIdentity(check, folder);
	check.section("identity file");
	TestGate(check, folder);
	check.section("consent gate");
	TestTransfers(check, folder);
	check.section("shared transfers");
	if (qEnvironmentVariable(kLiveVariable).trimmed() == u"1"_q) {
		TestLive(check, log, folder);
		check.section("live");
	} else {
		log.push_back(u"live: skipped, set %1=1 to run it"_q.arg(
			QString::fromLatin1(kLiveVariable)));
	}
	QDir(folder).removeRecursively();
	log.push_back(u"cloud: %1 checks passed, %2 failed"_q.arg(
		QString::number(check.passed()),
		QString::number(check.failed())));
	return !check.failed();
}

} // namespace Oblivion::Cloud
