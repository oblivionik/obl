/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_gift_catalog.h"

#include "base/timer.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "core/file_utilities.h"
#include "data/data_star_gift.h"
#include "lang/lang_keys.h"
#include "oblivion/oblivion_lottie.h"
#include "oblivion/oblivion_sticker_studio.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "platform/platform_file_utilities.h"
#include "settings.h"
#include "ui/abstract_button.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/text/text_entity.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/widgets/multi_select.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QUrl>
#include <QtGui/QCursor>
#include <QtGui/QPainterPath>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace Oblivion {
namespace {

constexpr auto kBaseUrl = "https://api.changes.tg"_cs;
constexpr auto kAttributionUrl = "https://t.me/GiftChanges"_cs;
constexpr auto kAttributionName = "@GiftChanges"_cs;
constexpr auto kRequestTimeout = 20 * 1000;
constexpr auto kMaxParallelRequests = 6;
constexpr auto kListTtl = crl::time(24 * 60 * 60 * 1000);
constexpr auto kPreviewForget = crl::time(1500);
constexpr auto kFailedPreviewRetry = crl::time(30 * 1000);
constexpr auto kMaxPreviews = 400;
constexpr auto kEvictPreviews = 150;
constexpr auto kMaxColorized = 120;
constexpr auto kPruneAge = qint64(45) * 24 * 60 * 60 * 1000;
constexpr auto kPruneLimit = qint64(300) * 1024 * 1024;
constexpr auto kPruneTarget = qint64(200) * 1024 * 1024;
constexpr auto kPreviewSize = 128;
constexpr auto kColumns = 3;
constexpr auto kPngSizes = std::array{ 64, 128, 256, 512, 1024 };

constexpr auto kModelsTab = 0;
constexpr auto kPatternsTab = 1;
constexpr auto kBackdropsTab = 2;

enum class Error : uchar {
	Network,
	Timeout,
	NotFound,
	Server,
	Data,
};

enum class Cache : uchar {
	None,
	List,
	Asset,
};

[[nodiscard]] int Px(int value) {
	return style::ConvertScale(value);
}

[[nodiscard]] QString PathWithoutQuery(const QString &path) {
	const auto query = path.indexOf('?');
	return (query >= 0) ? path.left(query) : path;
}

[[nodiscard]] QString CacheExtension(const QString &path) {
	const auto clean = PathWithoutQuery(path);
	return clean.endsWith(u".png"_q)
		? u".png"_q
		: clean.endsWith(u".tgs"_q)
		? u".tgs"_q
		: u".json"_q;
}

[[nodiscard]] QImage DecodeImage(const QByteArray &bytes) {
	auto image = QImage::fromData(bytes, "PNG");
	if (image.isNull()) {
		return image;
	}
	return std::move(image).convertToFormat(
		QImage::Format_ARGB32_Premultiplied);
}

[[nodiscard]] bool ValidPayload(
		const QString &path,
		const QByteArray &bytes) {
	if (bytes.isEmpty()) {
		return false;
	}
	const auto extension = CacheExtension(path);
	if (extension == u".png"_q) {
		return bytes.startsWith("\x89PNG");
	} else if (extension == u".tgs"_q) {
		return (bytes.size() > 2)
			&& (uchar(bytes[0]) == 0x1F)
			&& (uchar(bytes[1]) == 0x8B);
	}
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(bytes, &error);
	return (error.error == QJsonParseError::NoError)
		&& !document.isNull();
}

void WriteCacheFile(const QString &file, const QByteArray &bytes) {
	QDir().mkpath(QFileInfo(file).absolutePath());
	auto output = QSaveFile(file);
	if (output.open(QIODevice::WriteOnly)
		&& (output.write(bytes) == bytes.size())) {
		output.commit();
	} else {
		output.cancelWriting();
	}
}

void PruneCache(const QString &folder) {
	struct File {
		QString path;
		qint64 size = 0;
		qint64 modified = 0;
	};
	auto files = std::vector<File>();
	auto total = qint64();
	const auto now = QDateTime::currentMSecsSinceEpoch();
	auto i = QDirIterator(folder, QDir::Files | QDir::NoDotAndDotDot);
	while (i.hasNext()) {
		const auto info = i.nextFileInfo();
		const auto modified = info.lastModified().toMSecsSinceEpoch();
		if (now - modified > kPruneAge) {
			QFile::remove(info.absoluteFilePath());
			continue;
		}
		files.push_back({ info.absoluteFilePath(), info.size(), modified });
		total += info.size();
	}
	if (total <= kPruneLimit) {
		return;
	}
	ranges::sort(files, ranges::less(), &File::modified);
	for (const auto &file : files) {
		if (total <= kPruneTarget) {
			break;
		} else if (QFile::remove(file.path)) {
			total -= file.size;
		}
	}
}

[[nodiscard]] Error MapError(QNetworkReply::NetworkError error, int status) {
	if (status == 404 || error == QNetworkReply::ContentNotFoundError) {
		return Error::NotFound;
	} else if (status >= 400
		|| error == QNetworkReply::InternalServerError
		|| error == QNetworkReply::ServiceUnavailableError
		|| error == QNetworkReply::UnknownServerError) {
		return Error::Server;
	} else if (error == QNetworkReply::OperationCanceledError
		|| error == QNetworkReply::TimeoutError) {
		return Error::Timeout;
	}
	return Error::Network;
}

[[nodiscard]] QString ErrorText(Error error) {
	switch (error) {
	case Error::Timeout:
		return tr::lng_oblivion_gifts_error_timeout(tr::now);
	case Error::NotFound:
		return tr::lng_oblivion_gifts_error_not_found(tr::now);
	case Error::Server:
		return tr::lng_oblivion_gifts_error_server(tr::now);
	case Error::Data:
		return tr::lng_oblivion_gifts_error_data(tr::now);
	case Error::Network:
		break;
	}
	return tr::lng_oblivion_gifts_error_network(tr::now);
}

[[nodiscard]] QString DownloadErrorText(Error error) {
	return (error == Error::NotFound)
		? tr::lng_oblivion_gifts_error_file(tr::now)
		: ErrorText(error);
}

// A small HTTP client for api.changes.tg. Requests for the same path are
// merged, at most kMaxParallelRequests run at once (lists and downloads
// first, then previews, most recently painted first) and previews nobody
// painted for a while are dropped from the queue. Everything goes through
// QNetworkAccessManager, so the application proxy set by
// Core::Sandbox::refreshGlobalProxy() applies. Results are cached in memory
// (lists for a day, previews in a bounded LRU) and on disk in
// tdata/oblivion/changes_cache/, where a stale list is still used when the
// server can't be reached. A list is cached only if its validator accepts
// it, so a response of an unexpected shape is asked for again on retry.
//
// A client made from a Fixture serves only the given responses and
// previews, synchronously, without the network and the disk cache
// (the UI snapshot scenes use it).
class Client final : public base::has_weak_ptr {
public:
	using Validator = Fn<bool(const QByteArray &bytes)>;

	struct Fixture {
		base::flat_map<QString, QByteArray> responses; // By request path.
		base::flat_map<QString, QImage> previews; // By preview path.
		std::optional<Error> error; // Every request fails with it.
		bool hang = false; // Requests never finish (the loading state).
	};

	Client();
	explicit Client(Fixture fixture);
	~Client();

	[[nodiscard]] static std::shared_ptr<Client> Instance();

	// The validator runs on a background thread.
	void load(
		const QString &path,
		Cache cache,
		Fn<void(QByteArray)> done,
		Fn<void(Error)> fail,
		Validator validate = nullptr);
	[[nodiscard]] const QImage *preview(const QString &path);
	[[nodiscard]] rpl::producer<> previewsUpdated() const {
		return _previewsUpdated.events();
	}

private:
	struct Pending {
		Cache cache = Cache::None;
		bool preview = false;
		Validator validate;
		crl::time wanted = 0;
		QByteArray stale;
		QImage staleImage;
		crl::time staleAge = 0;
		std::vector<Fn<void(QByteArray)>> done;
		std::vector<Fn<void(Error)>> fail;
	};
	struct Memory {
		QByteArray bytes;
		crl::time received = 0;
	};
	struct Preview {
		QImage image;
		uint64 used = 0;
	};

	[[nodiscard]] QString cachePath(const QString &path) const;
	void readDisk(const QString &path);
	void diskRead(
		const QString &path,
		QByteArray bytes,
		QImage image,
		crl::time age);
	void enqueue(const QString &path);
	void processQueue();
	void send(const QString &path);
	void received(const QString &path, QByteArray bytes);
	void finish(
		const QString &path,
		QByteArray bytes,
		QImage image,
		crl::time age);
	void failed(const QString &path, Error error);
	void storePreview(const QString &path, QImage image);
	void failedPreviewsTimeout();

	[[nodiscard]] bool loadFixture(
		const QString &path,
		const Fn<void(QByteArray)> &done,
		const Fn<void(Error)> &fail,
		const Validator &validate) const;

	const std::unique_ptr<const Fixture> _fixture;
	const QString _cacheFolder;
	std::unique_ptr<QNetworkAccessManager> _manager;
	std::unique_ptr<QObject> _context;
	base::flat_map<QString, Pending> _pending;
	std::vector<QString> _queue;
	base::flat_map<QString, Memory> _memory;
	base::flat_map<QString, Preview> _previews;
	base::flat_map<QString, crl::time> _failedPreviews;
	base::Timer _failedPreviewsTimer;
	uint64 _previewsUsed = 0;
	int _active = 0;
	rpl::event_stream<> _previewsUpdated;

};

Client::Client()
: _cacheFolder(cWorkingDir() + u"tdata/oblivion/changes_cache/"_q)
, _manager(std::make_unique<QNetworkAccessManager>())
, _context(std::make_unique<QObject>())
, _failedPreviewsTimer([=] { failedPreviewsTimeout(); }) {
	static auto pruned = false;
	if (!pruned) {
		pruned = true;
		crl::async([folder = _cacheFolder] {
			PruneCache(folder);
		});
	}
}

Client::Client(Fixture fixture)
: _fixture(std::make_unique<const Fixture>(std::move(fixture)))
, _context(std::make_unique<QObject>())
, _failedPreviewsTimer([=] { failedPreviewsTimeout(); }) {
}

Client::~Client() {
	// The last owner may be released from inside a reply's finished()
	// handler, so the manager (and the reply emitting the signal) must
	// outlive this call. Dropping the context disconnects our handlers.
	_context = nullptr;
	if (const auto manager = _manager.release()) {
		manager->deleteLater();
	}
}

std::shared_ptr<Client> Client::Instance() {
	static auto instance = std::weak_ptr<Client>();
	if (auto strong = instance.lock()) {
		return strong;
	}
	auto result = std::make_shared<Client>();
	instance = result;
	return result;
}

QString Client::cachePath(const QString &path) const {
	const auto hash = QCryptographicHash::hash(
		path.toUtf8(),
		QCryptographicHash::Md5).toHex();
	return _cacheFolder + QString::fromLatin1(hash) + CacheExtension(path);
}

void Client::load(
		const QString &path,
		Cache cache,
		Fn<void(QByteArray)> done,
		Fn<void(Error)> fail,
		Validator validate) {
	if (loadFixture(path, done, fail, validate)) {
		return;
	} else if (cache == Cache::List) {
		const auto i = _memory.find(path);
		if (i != end(_memory)
			&& (crl::now() - i->second.received < kListTtl)) {
			const auto bytes = i->second.bytes;
			if (done) {
				done(bytes);
			}
			return;
		}
	}
	auto i = _pending.find(path);
	const auto fresh = (i == end(_pending));
	if (fresh) {
		i = _pending.emplace(path, Pending{ .cache = cache }).first;
	}
	if (validate && !i->second.validate) {
		i->second.validate = std::move(validate);
	}
	i->second.done.push_back(std::move(done));
	i->second.fail.push_back(std::move(fail));
	if (!fresh) {
		return;
	} else if (cache == Cache::None) {
		enqueue(path);
	} else {
		readDisk(path);
	}
}

bool Client::loadFixture(
		const QString &path,
		const Fn<void(QByteArray)> &done,
		const Fn<void(Error)> &fail,
		const Validator &validate) const {
	if (!_fixture) {
		return false;
	} else if (_fixture->hang) {
		return true;
	}
	const auto i = _fixture->responses.find(path);
	const auto error = _fixture->error
		? _fixture->error
		: (i == end(_fixture->responses))
		? std::make_optional(Error::NotFound)
		: (!ValidPayload(path, i->second)
			|| (validate && !validate(i->second)))
		? std::make_optional(Error::Data)
		: std::nullopt;
	if (error) {
		if (fail) {
			fail(*error);
		}
	} else if (done) {
		done(i->second);
	}
	return true;
}

const QImage *Client::preview(const QString &path) {
	if (_fixture) {
		const auto i = _fixture->previews.find(path);
		return (i != end(_fixture->previews)) ? &i->second : nullptr;
	} else if (const auto i = _previews.find(path); i != end(_previews)) {
		i->second.used = ++_previewsUsed;
		return &i->second.image;
	}
	const auto now = crl::now();
	if (const auto i = _pending.find(path); i != end(_pending)) {
		// The same file may be downloading for "Save as PNG", keep the
		// image as the preview when it arrives.
		i->second.preview = true;
		i->second.wanted = now;
		return nullptr;
	}
	if (const auto i = _failedPreviews.find(path)
		; i != end(_failedPreviews)) {
		if (now - i->second < kFailedPreviewRetry) {
			return nullptr;
		}
		_failedPreviews.erase(i);
	}
	_pending.emplace(path, Pending{
		.cache = Cache::Asset,
		.preview = true,
		.wanted = now,
	});
	readDisk(path);
	return nullptr;
}

void Client::readDisk(const QString &path) {
	const auto i = _pending.find(path);
	if (i == end(_pending)) {
		return;
	}
	const auto file = cachePath(path);
	const auto preview = i->second.preview;
	const auto validate = i->second.validate;
	crl::async([=, weak = base::make_weak(this)] {
		auto bytes = QByteArray();
		auto image = QImage();
		auto age = crl::time(-1);
		auto input = QFile(file);
		if (input.open(QIODevice::ReadOnly)) {
			bytes = input.readAll();
			input.close();
			const auto modified = QFileInfo(file).lastModified();
			age = std::max(
				modified.msecsTo(QDateTime::currentDateTime()),
				qint64(0));
			if (preview) {
				image = DecodeImage(bytes);
				if (image.isNull()) {
					bytes = QByteArray();
				}
			} else if (validate && !validate(bytes)) {
				// Left by an older build or damaged, ask the server.
				bytes = QByteArray();
			}
		}
		crl::on_main(weak, [=] {
			weak->diskRead(path, bytes, image, age);
		});
	});
}

void Client::diskRead(
		const QString &path,
		QByteArray bytes,
		QImage image,
		crl::time age) {
	const auto i = _pending.find(path);
	if (i == end(_pending)) {
		return;
	}
	const auto fresh = !bytes.isEmpty()
		&& (age >= 0)
		&& ((i->second.cache != Cache::List) || (age < kListTtl));
	if (fresh) {
		finish(path, std::move(bytes), std::move(image), age);
		return;
	}
	i->second.stale = std::move(bytes);
	i->second.staleImage = std::move(image);
	i->second.staleAge = age;
	enqueue(path);
}

void Client::enqueue(const QString &path) {
	const auto i = _pending.find(path);
	if (i == end(_pending)) {
		return;
	}
	if (!i->second.wanted) {
		i->second.wanted = crl::now();
	}
	_queue.push_back(path);
	processQueue();
}

void Client::processQueue() {
	const auto now = crl::now();
	auto dropped = false;
	while (_active < kMaxParallelRequests && !_queue.empty()) {
		auto best = -1;
		auto bestWanted = crl::time(-1);
		for (auto k = 0; k != int(_queue.size());) {
			const auto i = _pending.find(_queue[k]);
			if (i == end(_pending)) {
				_queue.erase(begin(_queue) + k);
				continue;
			}
			const auto &entry = i->second;
			const auto previewOnly = entry.preview
				&& ranges::none_of(entry.done, [](const auto &done) {
					return done != nullptr;
				});
			if (!previewOnly) {
				best = k;
				break;
			} else if (now - entry.wanted > kPreviewForget) {
				_pending.erase(i);
				_queue.erase(begin(_queue) + k);
				dropped = true;
				continue;
			} else if (entry.wanted > bestWanted) {
				best = k;
				bestWanted = entry.wanted;
			}
			++k;
		}
		if (best < 0) {
			break;
		}
		const auto path = _queue[best];
		_queue.erase(begin(_queue) + best);
		send(path);
	}
	if (dropped) {
		_previewsUpdated.fire({});
	}
}

void Client::send(const QString &path) {
	const auto url = QUrl::fromEncoded((kBaseUrl.utf16() + path).toUtf8());
	auto request = QNetworkRequest(url);
	request.setTransferTimeout(std::chrono::milliseconds(kRequestTimeout));
	const auto reply = _manager->get(request);
	const auto weak = base::make_weak(this);
	++_active;
	QObject::connect(reply, &QNetworkReply::finished, _context.get(), [=] {
		--_active;
		reply->deleteLater();
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto error = reply->error();
		if (error == QNetworkReply::NoError
			&& (!status || (status >= 200 && status < 300))) {
			received(path, reply->readAll());
		} else {
			failed(path, MapError(error, status));
		}
		if (weak) {
			processQueue();
		}
	});
}

void Client::received(const QString &path, QByteArray bytes) {
	const auto i = _pending.find(path);
	if (i == end(_pending)) {
		return;
	}
	const auto preview = i->second.preview;
	const auto validate = i->second.validate;
	const auto file = (i->second.cache != Cache::None)
		? cachePath(path)
		: QString();
	crl::async([=, weak = base::make_weak(this)] {
		auto image = QImage();
		const auto valid = [&] {
			if (!preview) {
				return ValidPayload(path, bytes)
					&& (!validate || validate(bytes));
			}
			image = DecodeImage(bytes);
			return !image.isNull();
		}();
		if (valid && !file.isEmpty()) {
			WriteCacheFile(file, bytes);
		}
		crl::on_main(weak, [=] {
			if (valid) {
				weak->finish(path, bytes, image, 0);
			} else {
				weak->failed(path, Error::Data);
			}
		});
	});
}

void Client::finish(
		const QString &path,
		QByteArray bytes,
		QImage image,
		crl::time age) {
	const auto i = _pending.find(path);
	if (i == end(_pending)) {
		return;
	}
	auto entry = std::move(i->second);
	_pending.erase(i);
	_failedPreviews.remove(path);
	if (entry.preview && !image.isNull()) {
		storePreview(path, std::move(image));
	}
	if (entry.cache == Cache::List) {
		_memory[path] = Memory{ bytes, crl::now() - age };
	}
	for (const auto &done : entry.done) {
		if (done) {
			done(bytes);
		}
	}
}

void Client::failed(const QString &path, Error error) {
	const auto i = _pending.find(path);
	if (i == end(_pending)) {
		return;
	}
	if (!i->second.stale.isEmpty()) {
		const auto bytes = i->second.stale;
		const auto image = i->second.staleImage;
		const auto age = i->second.staleAge;
		finish(path, bytes, image, age);
		return;
	}
	auto entry = std::move(i->second);
	_pending.erase(i);
	if (entry.preview) {
		_failedPreviews[path] = crl::now();
		if (!_failedPreviewsTimer.isActive()) {
			_failedPreviewsTimer.callOnce(kFailedPreviewRetry);
		}
	}
	for (const auto &fail : entry.fail) {
		if (fail) {
			fail(error);
		}
	}
}

void Client::failedPreviewsTimeout() {
	const auto now = crl::now();
	auto expired = false;
	auto next = crl::time(0);
	for (auto i = begin(_failedPreviews); i != end(_failedPreviews);) {
		const auto left = i->second + kFailedPreviewRetry - now;
		if (left <= 0) {
			i = _failedPreviews.erase(i);
			expired = true;
		} else {
			next = next ? std::min(next, left) : left;
			++i;
		}
	}
	if (next) {
		_failedPreviewsTimer.callOnce(next);
	}
	if (expired) {
		// Nothing else repaints a list that is just being looked at, so
		// let the visible tiles ask for these previews again.
		_previewsUpdated.fire({});
	}
}

void Client::storePreview(const QString &path, QImage image) {
	_previews[path] = Preview{ std::move(image), ++_previewsUsed };
	if (int(_previews.size()) > kMaxPreviews) {
		auto stamps = std::vector<uint64>();
		stamps.reserve(_previews.size());
		for (const auto &[key, value] : _previews) {
			stamps.push_back(value.used);
		}
		const auto nth = begin(stamps) + kEvictPreviews;
		std::nth_element(begin(stamps), nth, end(stamps));
		const auto threshold = *nth;
		for (auto i = begin(_previews); i != end(_previews);) {
			if (i->second.used < threshold) {
				i = _previews.erase(i);
			} else {
				++i;
			}
		}
	}
	_previewsUpdated.fire({});
}

enum class AssetType : uchar {
	Original,
	Model,
	Symbol,
};

struct Asset {
	AssetType type = AssetType::Original;
	QString gift;
	QString name;
};

[[nodiscard]] QString Encode(const QString &name) {
	return QString::fromLatin1(QUrl::toPercentEncoding(name));
}

[[nodiscard]] QString GiftPath(const QString &gift) {
	return u"/gift/"_q + Encode(gift);
}

[[nodiscard]] QString SectionPath(int tab, const QString &gift) {
	const auto prefix = (tab == kModelsTab)
		? u"/models/"_q
		: (tab == kPatternsTab)
		? u"/symbols/"_q
		: u"/backdrops/"_q;
	return prefix + Encode(gift) + u"?sorted"_q;
}

[[nodiscard]] QString AssetBase(const Asset &asset) {
	switch (asset.type) {
	case AssetType::Model:
		return u"/model/"_q + Encode(asset.gift) + '/' + Encode(asset.name);
	case AssetType::Symbol:
		return u"/symbol/"_q + Encode(asset.gift) + '/' + Encode(asset.name);
	case AssetType::Original:
		break;
	}
	return u"/original/"_q + Encode(asset.gift);
}

[[nodiscard]] QString AssetPng(const Asset &asset, int size) {
	return AssetBase(asset) + u".png?size="_q + QString::number(size);
}

[[nodiscard]] QString AssetPreview(const Asset &asset) {
	return AssetPng(asset, kPreviewSize);
}

[[nodiscard]] QString SafeFileName(QString name) {
	const auto forbidden = u"\\/:*?\"<>|"_q;
	for (auto &ch : name) {
		if (ch.unicode() < 0x20 || forbidden.contains(ch)) {
			ch = QChar('_');
		}
	}
	name = name.trimmed();
	while (name.startsWith('.')) {
		name.remove(0, 1);
	}
	return name.isEmpty() ? u"gift"_q : name;
}

[[nodiscard]] QString AssetFileName(const Asset &asset) {
	return SafeFileName(asset.name.isEmpty()
		? asset.gift
		: (asset.gift + u" - "_q + asset.name));
}

[[nodiscard]] QString SuggestedPath(const QString &fileName) {
	if (cDialogLastPath().isEmpty()) {
		Platform::FileDialog::InitLastPath();
	}
	return filedialogNextFilename(fileName, QString());
}

[[nodiscard]] QString FileFilter(const QString &name, const QString &mask) {
	return name + u" ("_q + mask + u")"_q;
}

[[nodiscard]] QString SearchKey(const QString &text) {
	auto result = QString();
	result.reserve(text.size());
	for (const auto &ch : text) {
		if (ch.isLetterOrNumber()) {
			result.append(ch.toLower());
		}
	}
	return result;
}

[[nodiscard]] QString RarityText(int rarity) {
	return Data::UniqueGiftAttributeText(Data::UniqueGiftAttribute{
		.rarityValue = rarity,
	});
}

[[nodiscard]] QColor RarityColor(int rarity) {
	return (rarity < 0 && rarity >= int(Data::UniqueGiftRarity::Legendary))
		? Data::UniqueGiftRarityBadgeColors(Data::UniqueGiftRarity(rarity)).fg
		: QColor();
}

[[nodiscard]] int RarityOrder(int rarity) {
	return (rarity < 0) ? (rarity - 1000) : rarity;
}

struct Attribute {
	QString name;
	int rarity = 0;
	QString search;
};

struct Backdrop {
	QString name;
	int rarity = 0;
	std::array<QColor, 4> colors;
	QString search;
};

struct GiftInfo {
	QString name;
	TimeId released = 0;
};

[[nodiscard]] std::optional<QJsonDocument> ParseJson(
		const QByteArray &bytes) {
	auto error = QJsonParseError();
	auto result = QJsonDocument::fromJson(bytes, &error);
	if (error.error != QJsonParseError::NoError) {
		return std::nullopt;
	}
	return result;
}

[[nodiscard]] int ParseRarity(const QJsonObject &object) {
	const auto value = object.value(u"rarityPermille"_q);
	return value.isDouble() ? int(std::lround(value.toDouble())) : 0;
}

[[nodiscard]] std::optional<std::vector<QString>> ParseNames(
		const QByteArray &bytes) {
	const auto document = ParseJson(bytes);
	if (!document || !document->isArray()) {
		return std::nullopt;
	}
	auto result = std::vector<QString>();
	for (const auto &value : document->array()) {
		const auto name = value.toString().trimmed();
		if (!name.isEmpty()) {
			result.push_back(name);
		}
	}
	return result;
}

[[nodiscard]] std::optional<std::vector<Attribute>> ParseAttributes(
		const QByteArray &bytes) {
	const auto document = ParseJson(bytes);
	if (!document || !document->isArray()) {
		return std::nullopt;
	}
	auto result = std::vector<Attribute>();
	for (const auto &value : document->array()) {
		const auto object = value.toObject();
		const auto name = object.value(u"name"_q).toString().trimmed();
		if (!name.isEmpty()) {
			result.push_back({
				.name = name,
				.rarity = ParseRarity(object),
				.search = SearchKey(name),
			});
		}
	}
	ranges::stable_sort(result, ranges::less(), [](const Attribute &a) {
		return RarityOrder(a.rarity);
	});
	return result;
}

[[nodiscard]] QColor ParseColor(
		const QJsonObject &object,
		const QString &key) {
	const auto value = object.value(key);
	if (value.isDouble()) {
		return QColor::fromRgb(QRgb(uint32(value.toDouble())));
	}
	const auto hex = object.value(u"hex"_q).toObject().value(key).toString();
	const auto parsed = QColor::fromString(hex);
	return parsed.isValid() ? parsed : QColor(0, 0, 0);
}

[[nodiscard]] std::optional<std::vector<Backdrop>> ParseBackdrops(
		const QByteArray &bytes) {
	const auto document = ParseJson(bytes);
	if (!document || !document->isArray()) {
		return std::nullopt;
	}
	auto result = std::vector<Backdrop>();
	for (const auto &value : document->array()) {
		const auto object = value.toObject();
		const auto name = object.value(u"name"_q).toString().trimmed();
		if (name.isEmpty()) {
			continue;
		}
		result.push_back({
			.name = name,
			.rarity = ParseRarity(object),
			.colors = {
				ParseColor(object, u"centerColor"_q),
				ParseColor(object, u"edgeColor"_q),
				ParseColor(object, u"patternColor"_q),
				ParseColor(object, u"textColor"_q),
			},
			.search = SearchKey(name),
		});
	}
	ranges::stable_sort(result, ranges::less(), [](const Backdrop &b) {
		return RarityOrder(b.rarity);
	});
	return result;
}

[[nodiscard]] std::optional<GiftInfo> ParseGift(const QByteArray &bytes) {
	const auto document = ParseJson(bytes);
	if (!document || !document->isObject()) {
		return std::nullopt;
	}
	const auto gift = document->object().value(u"gift"_q).toObject();
	auto result = GiftInfo{
		.name = gift.value(u"name"_q).toString().trimmed(),
		.released = TimeId(gift.value(u"releasedAt"_q).toDouble()),
	};
	if (result.name.isEmpty()) {
		return std::nullopt;
	}
	return result;
}

[[nodiscard]] QImage Colorize(const QImage &image, const QColor &color) {
	auto result = QImage(image.size(), QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);
	{
		auto p = QPainter(&result);
		p.drawImage(0, 0, image);
		p.setCompositionMode(QPainter::CompositionMode_SourceIn);
		p.fillRect(result.rect(), color);
	}
	return result;
}

enum class ListLayout : uchar {
	Grid,
	Rows,
};

struct ListItem {
	int id = 0;
	QString title;
	QString subtitle;
	QColor subtitleColor;
	QString preview;
	bool mask = false;
	std::array<QColor, 4> colors;
};

struct ListActivation {
	int id = 0;
	bool context = false;
};

class ItemsList final : public Ui::RpWidget {
public:
	ItemsList(QWidget *parent, std::shared_ptr<Client> client);

	void setLoading();
	void setError(const QString &text);
	void setItems(
		ListLayout layout,
		std::vector<ListItem> items,
		const QString &empty);

	// The loading / error / empty message takes at least this height and
	// is centered in it, so it stays in the middle of the visible area.
	void setFillHeight(int height);

	[[nodiscard]] rpl::producer<ListActivation> activations() const {
		return _activations.events();
	}
	[[nodiscard]] rpl::producer<> retryRequests() const {
		return _retryRequests.events();
	}

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	struct Colorized {
		QImage image;
		int palette = -1;
	};

	void refresh();
	[[nodiscard]] int gridTop() const;
	[[nodiscard]] int gridSkip() const;
	[[nodiscard]] int rowHeight() const;
	[[nodiscard]] QRect itemRect(int index) const;
	[[nodiscard]] int itemAt(QPoint point) const;
	void setSelected(int index);
	void paintTile(QPainter &p, const ListItem &item, QRect rect, bool over);
	void paintRow(QPainter &p, const ListItem &item, QRect rect, bool over);
	void paintPreview(QPainter &p, const ListItem &item, QRect rect);
	void paintMessage(QPainter &p);

	const std::shared_ptr<Client> _client;
	const object_ptr<Ui::RoundButton> _retry;
	std::vector<ListItem> _items;
	ListLayout _layout = ListLayout::Grid;
	QString _message;
	bool _error = false;
	bool _subtitles = false;
	int _gridLeft = 0;
	int _tileWidth = 1;
	int _tileHeight = 1;
	int _messageTop = 0;
	int _messageHeight = 0;
	int _fillHeight = 0;
	int _selected = -1;
	int _pressed = -1;
	base::flat_map<QString, Colorized> _colorized;
	rpl::event_stream<ListActivation> _activations;
	rpl::event_stream<> _retryRequests;

};

ItemsList::ItemsList(QWidget *parent, std::shared_ptr<Client> client)
: RpWidget(parent)
, _client(std::move(client))
, _retry(
	this,
	tr::lng_oblivion_gifts_retry(),
	st::defaultActiveButton)
, _message(tr::lng_oblivion_gifts_loading(tr::now)) {
	setMouseTracking(true);
	_retry->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	_retry->setClickedCallback([=] {
		_retryRequests.fire({});
	});
	_retry->hide();
	_client->previewsUpdated() | rpl::on_next([=] {
		update();
	}, lifetime());
}

void ItemsList::setLoading() {
	_items.clear();
	_error = false;
	_message = tr::lng_oblivion_gifts_loading(tr::now);
	refresh();
}

void ItemsList::setError(const QString &text) {
	_items.clear();
	_error = true;
	_message = text;
	refresh();
}

void ItemsList::setItems(
		ListLayout layout,
		std::vector<ListItem> items,
		const QString &empty) {
	_layout = layout;
	_items = std::move(items);
	_error = false;
	_message = empty;
	_subtitles = ranges::any_of(_items, [](const ListItem &item) {
		return !item.subtitle.isEmpty();
	});
	refresh();
}

void ItemsList::setFillHeight(int height) {
	if (_fillHeight == height) {
		return;
	}
	_fillHeight = height;
	if (_items.empty()) {
		resizeToWidth(width());
		update();
	}
}

void ItemsList::refresh() {
	_selected = _pressed = -1;
	setCursor(style::cur_default);
	_retry->setVisible(_items.empty() && _error);
	resizeToWidth(width());
	update();
}

int ItemsList::gridTop() const {
	return Px(8);
}

int ItemsList::gridSkip() const {
	return Px(8);
}

int ItemsList::rowHeight() const {
	return Px(56);
}

int ItemsList::resizeGetHeight(int newWidth) {
	if (_items.empty()) {
		const auto textWidth = std::max(newWidth - 2 * Px(24), 1);
		const auto bounds = st::normalFont->metrics().boundingRect(
			QRectF(0, 0, textWidth, 1 << 20),
			Qt::AlignHCenter | Qt::TextWordWrap,
			_message);
		_messageHeight = int(std::ceil(bounds.height()));
		const auto retrySkip = Px(16);
		const auto block = _messageHeight
			+ (_error ? (retrySkip + _retry->height()) : 0);
		const auto result = std::max(Px(48) + block + Px(48), _fillHeight);
		_messageTop = (result - block) / 2;
		if (_error) {
			_retry->moveToLeft(
				(newWidth - _retry->width()) / 2,
				_messageTop + _messageHeight + retrySkip,
				newWidth);
		}
		return result;
	}
	const auto count = int(_items.size());
	if (_layout == ListLayout::Rows) {
		return Px(4) + count * rowHeight() + Px(8);
	}
	const auto skip = gridSkip();
	_tileWidth = std::max(
		(newWidth - 2 * Px(12) - (kColumns - 1) * skip) / kColumns,
		1);
	// The pixels left after the integer division go to both sides
	// equally, so the grid stays centered.
	_gridLeft = std::max(
		(newWidth - kColumns * _tileWidth - (kColumns - 1) * skip) / 2,
		0);
	_tileHeight = Px(10)
		+ Px(64)
		+ Px(6)
		+ st::normalFont->height * (_subtitles ? 2 : 1)
		+ Px(10);
	const auto rows = (count + kColumns - 1) / kColumns;
	return gridTop() + rows * _tileHeight + (rows - 1) * skip + Px(12);
}

QRect ItemsList::itemRect(int index) const {
	if (_layout == ListLayout::Rows) {
		return QRect(0, Px(4) + index * rowHeight(), width(), rowHeight());
	}
	const auto row = index / kColumns;
	const auto column = index % kColumns;
	return QRect(
		_gridLeft + column * (_tileWidth + gridSkip()),
		gridTop() + row * (_tileHeight + gridSkip()),
		_tileWidth,
		_tileHeight);
}

int ItemsList::itemAt(QPoint point) const {
	const auto count = int(_items.size());
	if (!count) {
		return -1;
	}
	auto index = -1;
	if (_layout == ListLayout::Rows) {
		const auto top = point.y() - Px(4);
		index = (top >= 0) ? (top / rowHeight()) : -1;
	} else {
		const auto top = point.y() - gridTop();
		const auto left = point.x() - _gridLeft;
		if (top >= 0 && left >= 0) {
			const auto row = top / (_tileHeight + gridSkip());
			const auto column = left / (_tileWidth + gridSkip());
			index = (column < kColumns) ? (row * kColumns + column) : -1;
		}
	}
	return (index >= 0 && index < count && itemRect(index).contains(point))
		? index
		: -1;
}

void ItemsList::setSelected(int index) {
	if (_selected == index) {
		return;
	}
	if (_selected >= 0) {
		update(itemRect(_selected));
	}
	_selected = index;
	if (_selected >= 0) {
		update(itemRect(_selected));
	}
	setCursor((_selected >= 0) ? style::cur_pointer : style::cur_default);
}

void ItemsList::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	if (_items.empty()) {
		paintMessage(p);
		return;
	}
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	const auto clip = e->rect();
	const auto count = int(_items.size());
	if (_layout == ListLayout::Rows) {
		const auto height = rowHeight();
		const auto from = std::clamp((clip.y() - Px(4)) / height, 0, count);
		const auto till = std::clamp(
			(clip.y() + clip.height() - Px(4) + height - 1) / height,
			0,
			count);
		for (auto i = from; i < till; ++i) {
			paintRow(p, _items[i], itemRect(i), (i == _selected));
		}
		return;
	}
	const auto step = _tileHeight + gridSkip();
	const auto rows = (count + kColumns - 1) / kColumns;
	const auto from = std::clamp((clip.y() - gridTop()) / step, 0, rows);
	const auto till = std::clamp(
		(clip.y() + clip.height() - gridTop()) / step + 1,
		0,
		rows);
	for (auto row = from; row < till; ++row) {
		for (auto column = 0; column != kColumns; ++column) {
			const auto index = row * kColumns + column;
			if (index >= count) {
				break;
			}
			const auto rect = itemRect(index);
			if (rect.intersects(clip)) {
				paintTile(p, _items[index], rect, (index == _selected));
			}
		}
	}
}

void ItemsList::paintMessage(QPainter &p) {
	p.setFont(st::normalFont);
	p.setPen(st::windowSubTextFg);
	p.drawText(
		QRect(Px(24), _messageTop, width() - 2 * Px(24), _messageHeight),
		Qt::AlignHCenter | Qt::TextWordWrap,
		_message);
}

void ItemsList::paintPreview(
		QPainter &p,
		const ListItem &item,
		QRect rect) {
	if (item.preview.isEmpty()) {
		return;
	}
	const auto image = _client->preview(item.preview);
	if (!image) {
		const auto inner = Px(10);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowSubTextFg);
		p.setOpacity(0.12);
		p.drawEllipse(rect.marginsRemoved({ inner, inner, inner, inner }));
		p.setOpacity(1.);
		return;
	} else if (!item.mask) {
		p.drawImage(rect, *image);
		return;
	}
	if (!_colorized.contains(item.preview)
		&& int(_colorized.size()) >= kMaxColorized) {
		_colorized.clear();
	}
	auto &colorized = _colorized[item.preview];
	const auto palette = style::PaletteVersion();
	if (colorized.image.isNull() || colorized.palette != palette) {
		colorized.image = Colorize(*image, st::windowFg->c);
		colorized.palette = palette;
	}
	p.drawImage(rect, colorized.image);
}

void ItemsList::paintTile(
		QPainter &p,
		const ListItem &item,
		QRect rect,
		bool over) {
	const auto radius = Px(10);
	p.setPen(Qt::NoPen);
	p.setBrush(over ? st::windowBgRipple : st::windowBgOver);
	p.drawRoundedRect(rect, radius, radius);

	const auto size = Px(64);
	paintPreview(p, item, QRect(
		rect.x() + (rect.width() - size) / 2,
		rect.y() + Px(10),
		size,
		size));

	const auto &font = st::normalFont;
	const auto available = rect.width() - 2 * Px(6);
	auto top = rect.y() + Px(10) + size + Px(6);
	p.setFont(font);
	p.setPen(st::windowFg);
	p.drawText(
		QRect(rect.x(), top, rect.width(), font->height),
		Qt::AlignHCenter | Qt::AlignTop,
		font->elided(item.title, available));
	if (!item.subtitle.isEmpty()) {
		top += font->height;
		if (item.subtitleColor.isValid()) {
			p.setPen(item.subtitleColor);
		} else {
			p.setPen(st::windowSubTextFg);
		}
		p.drawText(
			QRect(rect.x(), top, rect.width(), font->height),
			Qt::AlignHCenter | Qt::AlignTop,
			font->elided(item.subtitle, available));
	}
}

void ItemsList::paintRow(
		QPainter &p,
		const ListItem &item,
		QRect rect,
		bool over) {
	if (over) {
		p.fillRect(rect, st::windowBgOver);
	}
	const auto left = st::boxRowPadding.left();
	const auto right = st::boxRowPadding.right();
	const auto size = Px(40);
	const auto circle = QRectF(
		left,
		rect.y() + (rect.height() - size) / 2.,
		size,
		size);
	// The same thin outline as the swatches have, so that a dark
	// backdrop stays visible on the night background.
	auto border = st::windowSubTextFg->c;
	border.setAlphaF(0.35);
	const auto stroke = double(st::lineWidth);
	auto gradient = QRadialGradient(circle.center(), size / 2.);
	gradient.setColorAt(0., item.colors[0]);
	gradient.setColorAt(1., item.colors[1]);
	p.setPen(QPen(border, stroke));
	p.setBrush(gradient);
	p.drawEllipse(circle.marginsRemoved(QMarginsF(
		stroke / 2.,
		stroke / 2.,
		stroke / 2.,
		stroke / 2.)));
	const auto dot = Px(12);
	p.setPen(Qt::NoPen);
	p.setBrush(item.colors[2]);
	p.drawEllipse(QRectF(
		circle.center().x() - dot / 2.,
		circle.center().y() - dot / 2.,
		dot,
		dot));

	const auto swatch = Px(16);
	const auto gap = Px(6);
	const auto swatchesLeft = rect.width()
		- right
		- 4 * swatch
		- 3 * gap;
	const auto swatchTop = rect.y() + (rect.height() - swatch) / 2;
	p.setPen(border);
	for (auto i = 0; i != 4; ++i) {
		p.setBrush(item.colors[i]);
		p.drawRoundedRect(
			QRect(
				swatchesLeft + i * (swatch + gap),
				swatchTop,
				swatch,
				swatch),
			Px(4),
			Px(4));
	}

	const auto textLeft = left + size + Px(12);
	const auto textWidth = std::max(swatchesLeft - Px(12) - textLeft, 1);
	p.setFont(st::semiboldFont);
	p.setPen(st::windowFg);
	p.drawText(
		textLeft,
		rect.y() + Px(9) + st::semiboldFont->ascent,
		st::semiboldFont->elided(item.title, textWidth));
	p.setFont(st::normalFont);
	if (item.subtitleColor.isValid()) {
		p.setPen(item.subtitleColor);
	} else {
		p.setPen(st::windowSubTextFg);
	}
	p.drawText(
		textLeft,
		rect.y() + Px(29) + st::normalFont->ascent,
		st::normalFont->elided(item.subtitle, textWidth));
}

void ItemsList::mouseMoveEvent(QMouseEvent *e) {
	setSelected(itemAt(e->pos()));
}

void ItemsList::leaveEventHook(QEvent *e) {
	setSelected(-1);
}

void ItemsList::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = itemAt(e->pos());
	}
}

void ItemsList::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, -1);
	if (e->button() != Qt::LeftButton
		|| pressed < 0
		|| pressed != itemAt(e->pos())) {
		return;
	}
	_activations.fire({ .id = _items[pressed].id });
}

void ItemsList::contextMenuEvent(QContextMenuEvent *e) {
	const auto index = itemAt(e->pos());
	if (index >= 0) {
		_activations.fire({ .id = _items[index].id, .context = true });
	}
}

class HeaderButton final : public Ui::AbstractButton {
public:
	using AbstractButton::AbstractButton;

protected:
	void onStateChanged(State was, StateChangeSource source) override {
		update();
	}

};

using MenuHolder = base::unique_qptr<Ui::PopupMenu>;

// What the catalog boxes need from outside: where to show toasts and
// boxes, the data source and the session-bound sticker editor.
struct Catalog {
	std::shared_ptr<Ui::Show> show;
	std::shared_ptr<Client> client;
	Fn<void(QByteArray bytes, QString name)> openEditor;
};

not_null<Ui::RpWidget*> AddAttribution(not_null<Ui::GenericBox*> box) {
	const auto st = box->lifetime().make_state<style::FlatLabel>(
		st::boxDividerLabel);
	st->align = style::al_top;
	st->minWidth = 0;
	auto label = object_ptr<Ui::FlatLabel>(
		box,
		tr::lng_oblivion_gifts_attribution(
			lt_link,
			rpl::single(tr::link(
				kAttributionName.utf16(),
				kAttributionUrl.utf16())),
			tr::marked),
		*st);
	// The buttons row below has its own top padding and the button text
	// sits in the middle of a tall button, so most of the skip goes above
	// the line to keep it in the middle between the list and the button.
	return box->setPinnedToBottomContent(
		object_ptr<Ui::PaddingWrap<Ui::FlatLabel>>(
			box,
			std::move(label),
			QMargins(
				st::boxRowPadding.left(),
				Px(12),
				st::boxRowPadding.right(),
				0)));
}

void OpenInEditor(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<Catalog> catalog,
		const Asset &asset) {
	const auto show = catalog->show;
	const auto open = catalog->openEditor;
	const auto name = AssetFileName(asset);
	const auto answered = std::make_shared<bool>(false);
	catalog->client->load(
		AssetBase(asset) + u".tgs"_q,
		Cache::Asset,
		crl::guard(box, [=](QByteArray bytes) {
			*answered = true;
			if (open) {
				open(bytes, name);
			}
		}),
		crl::guard(box, [=](Error error) {
			*answered = true;
			show->showToast(DownloadErrorText(error));
		}));
	if (!*answered) {
		show->showToast(tr::lng_oblivion_gifts_downloading(tr::now));
	}
}

enum class SaveFormat : uchar {
	Tgs,
	Json,
	Png,
};

void SaveAsset(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<Catalog> catalog,
		const Asset &asset,
		SaveFormat format,
		int size) {
	const auto show = catalog->show;
	const auto client = catalog->client;
	const auto base = AssetFileName(asset);
	const auto path = (format == SaveFormat::Tgs)
		? (AssetBase(asset) + u".tgs"_q)
		: (format == SaveFormat::Json)
		? (AssetBase(asset) + u".json"_q)
		: AssetPng(asset, size);
	const auto cache = (format == SaveFormat::Tgs)
		? Cache::Asset
		: Cache::None;
	const auto fileName = (format == SaveFormat::Tgs)
		? (base + u".tgs"_q)
		: (format == SaveFormat::Json)
		? (base + u".json"_q)
		: (base + '_' + QString::number(size) + u".png"_q);
	const auto caption = (format == SaveFormat::Tgs)
		? tr::lng_oblivion_gifts_save_tgs(tr::now)
		: (format == SaveFormat::Json)
		? tr::lng_oblivion_gifts_save_json(tr::now)
		: tr::lng_oblivion_gifts_save_png(tr::now);
	const auto filter = (format == SaveFormat::Tgs)
		? FileFilter(tr::lng_oblivion_file_tgs(tr::now), u"*.tgs"_q)
		: (format == SaveFormat::Json)
		? FileFilter(tr::lng_oblivion_file_lottie(tr::now), u"*.json"_q)
		: FileFilter(tr::lng_oblivion_file_png(tr::now), u"*.png"_q);
	FileDialog::GetWritePath(
		box.get(),
		caption,
		filter,
		SuggestedPath(fileName),
		crl::guard(box, [=](QString &&result) {
			const auto target = result;
			if (target.isEmpty()) {
				return;
			}
			show->showToast(tr::lng_oblivion_gifts_downloading(tr::now));
			client->load(path, cache, crl::guard(box, [=](QByteArray bytes) {
				auto file = QFile(target);
				if (file.open(QIODevice::WriteOnly)
					&& (file.write(bytes) == bytes.size())) {
					file.close();
					show->showToast(tr::lng_oblivion_saved_to(
						tr::now,
						lt_path,
						QDir::toNativeSeparators(target)));
				} else {
					show->showToast(tr::lng_oblivion_write_failed(tr::now));
				}
			}), crl::guard(box, [=](Error error) {
				show->showToast(DownloadErrorText(error));
			}));
		}));
}

void CopyText(
		const std::shared_ptr<Ui::Show> &show,
		const QString &text,
		const QString &toast) {
	TextUtilities::SetClipboardText(TextForMimeData::Simple(text));
	show->showToast(toast);
}

void ShowAssetMenu(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<Catalog> catalog,
		not_null<MenuHolder*> holder,
		const Asset &asset,
		Fn<void(not_null<Ui::PopupMenu*>)> prepend = nullptr) {
	*holder = base::make_unique_q<Ui::PopupMenu>(
		box,
		st::popupMenuWithIcons);
	const auto menu = holder->get();
	if (prepend) {
		prepend(menu);
	}
	menu->addAction(tr::lng_oblivion_gifts_open_editor(tr::now), [=] {
		OpenInEditor(box, catalog, asset);
	}, &st::menuIconStickers);
	menu->addAction(tr::lng_oblivion_gifts_save_tgs(tr::now), [=] {
		SaveAsset(box, catalog, asset, SaveFormat::Tgs, 0);
	}, &st::menuIconDownload);
	menu->addAction(tr::lng_oblivion_gifts_save_json(tr::now), [=] {
		SaveAsset(box, catalog, asset, SaveFormat::Json, 0);
	}, &st::menuIconFile);
	const auto addAction = Ui::Menu::CreateAddActionCallback(menu);
	addAction({
		.text = tr::lng_oblivion_gifts_save_png(tr::now),
		.handler = nullptr,
		.icon = &st::menuIconSaveImage,
		.fillSubmenu = [=](not_null<Ui::PopupMenu*> submenu) {
			for (const auto size : kPngSizes) {
				const auto label = QString::number(size)
					+ QChar(0x00D7)
					+ QString::number(size);
				submenu->addAction(label, [=] {
					SaveAsset(box, catalog, asset, SaveFormat::Png, size);
				});
			}
		},
	});
	const auto name = asset.name.isEmpty() ? asset.gift : asset.name;
	const auto show = catalog->show;
	menu->addAction(tr::lng_oblivion_gifts_copy_name(tr::now), [=] {
		CopyText(
			show,
			name,
			tr::lng_oblivion_gifts_name_copied(tr::now));
	}, &st::menuIconCopy);
	menu->popup(QCursor::pos());
}

void ShowBackdropMenu(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<Ui::Show> show,
		not_null<MenuHolder*> holder,
		const Backdrop &backdrop) {
	*holder = base::make_unique_q<Ui::PopupMenu>(
		box,
		st::popupMenuWithIcons);
	const auto menu = holder->get();
	auto hexes = QStringList();
	for (const auto &color : backdrop.colors) {
		hexes.push_back(color.name());
	}
	menu->addAction(tr::lng_oblivion_gifts_copy_colors(tr::now), [=] {
		CopyText(
			show,
			hexes.join('\n'),
			tr::lng_oblivion_backdrop_copied(tr::now));
	}, &st::menuIconPalette);
	const auto labels = std::array{
		tr::lng_oblivion_gifts_color_center(tr::now),
		tr::lng_oblivion_gifts_color_edge(tr::now),
		tr::lng_oblivion_gifts_color_pattern(tr::now),
		tr::lng_oblivion_gifts_color_text(tr::now),
	};
	for (auto i = 0; i != 4; ++i) {
		const auto hex = hexes[i];
		menu->addAction(labels[i] + u": "_q + hex, [=] {
			CopyText(
				show,
				hex,
				tr::lng_oblivion_gifts_color_copied(
					tr::now,
					lt_color,
					hex));
		}, &st::menuIconCopy);
	}
	const auto name = backdrop.name;
	menu->addAction(tr::lng_oblivion_gifts_copy_name(tr::now), [=] {
		CopyText(
			show,
			name,
			tr::lng_oblivion_gifts_name_copied(tr::now));
	}, &st::menuIconCopy);
	menu->popup(QCursor::pos());
}

// A bit wider than st::boxWideWidth, so that most gift and model names
// fit a tile of the three column grid without eliding.
[[nodiscard]] int CatalogWidth() {
	return st::boxWideWidth + Px(56);
}

// Returns the attribution line pinned to the bottom.
not_null<Ui::RpWidget*> PrepareCatalogBox(not_null<Ui::GenericBox*> box) {
	box->setWidth(CatalogWidth());
	box->setMinHeight(st::boxMaxListHeight);
	box->setMaxHeight(st::boxMaxListHeight);
	const auto result = AddAttribution(box);
	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
	});
	return result;
}

// The loading / error / empty message of the list is centered in the
// part of the box scroll area below the widgets above the list.
void FillScrollWithList(
		not_null<Ui::GenericBox*> box,
		not_null<ItemsList*> list,
		not_null<Ui::RpWidget*> pinnedTop,
		not_null<Ui::RpWidget*> pinnedBottom,
		rpl::producer<int> aboveList) {
	rpl::combine(
		box->heightValue(),
		pinnedTop->heightValue(),
		pinnedBottom->heightValue(),
		std::move(aboveList)
	) | rpl::on_next([=](int height, int top, int bottom, int above) {
		list->setFillHeight(height - top - bottom - above);
	}, list->lifetime());
}

void SetupSearch(
		not_null<Ui::GenericBox*> box,
		not_null<Ui::MultiSelect*> search,
		Fn<void(QString)> changed) {
	search->setQueryChangedCallback([=](const QString &query) {
		changed(query);
		box->scrollToY(0);
	});
	search->setCancelledCallback([=] {
		if (search->getQuery().isEmpty()) {
			box->closeBox();
		} else {
			search->clearQuery();
		}
	});
	box->setFocusCallback([=] {
		search->setInnerFocus();
	});
}

void GiftDetailsBox(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<Catalog> catalog,
		QString key,
		bool resolved,
		int tab) {
	struct Section {
		bool loading = false;
		bool loaded = false;
		std::optional<Error> error;
	};
	struct State {
		std::shared_ptr<Client> client;
		rpl::variable<QString> title;
		QString gift;
		TimeId released = 0;
		std::optional<Error> resolveError;
		std::vector<Attribute> models;
		std::vector<Attribute> patterns;
		std::vector<Backdrop> backdrops;
		std::array<Section, 3> sections;
		int tab = kModelsTab;
		QString query;
		MenuHolder menu;
	};
	const auto state = box->lifetime().make_state<State>();
	state->client = catalog->client;
	state->title = resolved
		? key
		: tr::lng_oblivion_tools_gift_catalog(tr::now);
	state->tab = std::clamp(tab, kModelsTab, kBackdropsTab);

	const auto attribution = PrepareCatalogBox(box);
	box->setTitle(state->title.value());

	// The search goes right under the title, at the same place as in the
	// gift list box, and the tabs go last, so that the active tab bar
	// lies on the line the box draws under the pinned part (as the
	// stickers box tabs do), instead of hanging above the search field.
	const auto top = box->setPinnedToTopContent(
		object_ptr<Ui::VerticalLayout>(box));
	const auto search = top->add(object_ptr<Ui::MultiSelect>(
		top,
		st::defaultMultiSelect,
		tr::lng_oblivion_gifts_search_items()));
	const auto tabs = top->add(object_ptr<Ui::SettingsSlider>(
		top,
		st::defaultTabsSlider));
	tabs->setSections(std::vector<QString>{
		tr::lng_oblivion_gifts_tab_models(tr::now),
		tr::lng_oblivion_gifts_tab_patterns(tr::now),
		tr::lng_oblivion_gifts_tab_backdrops(tr::now),
	});
	tabs->setActiveSectionFast(state->tab);
	top->resizeToWidth(CatalogWidth());

	// Collapsed while the gift is not resolved yet, so the loading and
	// error messages are centered in the whole list area.
	const auto header = box->addRow(
		object_ptr<HeaderButton>(box),
		QMargins());
	header->resize(CatalogWidth(), resolved ? Px(72) : 0);
	header->setAcceptBoth(true);
	const auto list = box->addRow(
		object_ptr<ItemsList>(box, state->client),
		QMargins());
	FillScrollWithList(box, list, top, attribution, header->heightValue());

	const auto originalAsset = [=] {
		return Asset{ .type = AssetType::Original, .gift = state->gift };
	};
	header->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(header);
		p.setRenderHint(QPainter::SmoothPixmapTransform);
		if (header->isOver() && !state->gift.isEmpty()) {
			p.fillRect(header->rect(), st::windowBgOver);
		}
		const auto size = Px(52);
		const auto left = st::boxRowPadding.left();
		const auto imageTop = (header->height() - size) / 2;
		if (!state->gift.isEmpty()) {
			const auto path = AssetPreview(originalAsset());
			if (const auto image = state->client->preview(path)) {
				p.drawImage(QRect(left, imageTop, size, size), *image);
			}
		}
		const auto textLeft = left + size + Px(14);
		const auto textWidth = std::max(
			header->width() - textLeft - st::boxRowPadding.right(),
			1);
		const auto first = state->released
			? tr::lng_oblivion_gifts_released(
				tr::now,
				lt_date,
				langDayOfMonthFull(
					base::unixtime::parse(state->released).date()))
			: state->gift;
		const auto count = [&](int index, int value) {
			return state->sections[index].loaded
				? QString::number(value)
				: QString(QChar(0x2026));
		};
		const auto second = state->gift.isEmpty()
			? QString()
			: tr::lng_oblivion_gifts_counts(
				tr::now,
				lt_models,
				count(kModelsTab, int(state->models.size())),
				lt_patterns,
				count(kPatternsTab, int(state->patterns.size())),
				lt_backdrops,
				count(kBackdropsTab, int(state->backdrops.size())));
		p.setFont(st::semiboldFont);
		p.setPen(st::windowFg);
		p.drawText(
			textLeft,
			imageTop + Px(6) + st::semiboldFont->ascent,
			st::semiboldFont->elided(first, textWidth));
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			textLeft,
			imageTop + Px(28) + st::normalFont->ascent,
			st::normalFont->elided(second, textWidth));
	}, header->lifetime());
	state->client->previewsUpdated() | rpl::on_next([=] {
		header->update();
	}, header->lifetime());
	header->clicks() | rpl::on_next([=](Qt::MouseButton) {
		if (!state->gift.isEmpty()) {
			ShowAssetMenu(box, catalog, &state->menu, originalAsset());
		}
	}, header->lifetime());

	const auto refresh = [=] {
		header->resize(
			header->width(),
			state->gift.isEmpty() ? 0 : Px(72));
		header->update();
		if (state->gift.isEmpty()) {
			if (state->resolveError) {
				list->setError(ErrorText(*state->resolveError));
			} else {
				list->setLoading();
			}
			return;
		}
		const auto &section = state->sections[state->tab];
		if (section.error) {
			list->setError(ErrorText(*section.error));
			return;
		} else if (!section.loaded) {
			list->setLoading();
			return;
		}
		const auto query = SearchKey(state->query);
		const auto empty = query.isEmpty()
			? tr::lng_oblivion_gifts_list_empty(tr::now)
			: tr::lng_oblivion_gifts_nothing_found(tr::now);
		auto items = std::vector<ListItem>();
		if (state->tab == kBackdropsTab) {
			const auto &backdrops = state->backdrops;
			for (auto i = 0; i != int(backdrops.size()); ++i) {
				const auto &backdrop = backdrops[i];
				if (!query.isEmpty() && !backdrop.search.contains(query)) {
					continue;
				}
				items.push_back({
					.id = i,
					.title = backdrop.name,
					.subtitle = RarityText(backdrop.rarity),
					.subtitleColor = RarityColor(backdrop.rarity),
					.colors = backdrop.colors,
				});
			}
			list->setItems(ListLayout::Rows, std::move(items), empty);
			return;
		}
		const auto patterns = (state->tab == kPatternsTab);
		const auto &attributes = patterns
			? state->patterns
			: state->models;
		const auto type = patterns ? AssetType::Symbol : AssetType::Model;
		for (auto i = 0; i != int(attributes.size()); ++i) {
			const auto &attribute = attributes[i];
			if (!query.isEmpty() && !attribute.search.contains(query)) {
				continue;
			}
			items.push_back({
				.id = i,
				.title = attribute.name,
				.subtitle = RarityText(attribute.rarity),
				.subtitleColor = RarityColor(attribute.rarity),
				.preview = AssetPreview({
					.type = type,
					.gift = state->gift,
					.name = attribute.name,
				}),
				.mask = patterns,
			});
		}
		list->setItems(ListLayout::Grid, std::move(items), empty);
	};

	const auto sectionUpdated = [=](int index) {
		if (index == state->tab) {
			refresh();
		} else {
			header->update();
		}
	};
	const auto loadSection = [=](int index) {
		auto &section = state->sections[index];
		if (section.loading || section.loaded || state->gift.isEmpty()) {
			return;
		}
		section.loading = true;
		section.error = std::nullopt;
		state->client->load(
			SectionPath(index, state->gift),
			Cache::List,
			crl::guard(box, [=](QByteArray bytes) {
				auto &section = state->sections[index];
				section.loading = false;
				if (index == kBackdropsTab) {
					if (auto parsed = ParseBackdrops(bytes)) {
						state->backdrops = std::move(*parsed);
						section.loaded = true;
					} else {
						section.error = Error::Data;
					}
				} else if (auto parsed = ParseAttributes(bytes)) {
					((index == kModelsTab)
						? state->models
						: state->patterns) = std::move(*parsed);
					section.loaded = true;
				} else {
					section.error = Error::Data;
				}
				sectionUpdated(index);
			}),
			crl::guard(box, [=](Error error) {
				auto &section = state->sections[index];
				section.loading = false;
				section.error = error;
				sectionUpdated(index);
			}),
			((index == kBackdropsTab)
				? Client::Validator([](const QByteArray &bytes) {
					return ParseBackdrops(bytes).has_value();
				})
				: Client::Validator([](const QByteArray &bytes) {
					return ParseAttributes(bytes).has_value();
				})));
	};
	const auto loadSections = [=] {
		for (auto i = 0; i != int(state->sections.size()); ++i) {
			loadSection(i);
		}
	};
	const auto resolve = [=] {
		state->resolveError = std::nullopt;
		state->client->load(
			GiftPath(key),
			Cache::List,
			crl::guard(box, [=](QByteArray bytes) {
				const auto info = ParseGift(bytes);
				if (info) {
					state->released = info->released;
				}
				if (!state->gift.isEmpty()) {
					header->update();
					return;
				} else if (!info) {
					state->resolveError = Error::Data;
					refresh();
					return;
				}
				state->gift = info->name;
				state->title = info->name;
				refresh();
				loadSections();
			}),
			crl::guard(box, [=](Error error) {
				if (state->gift.isEmpty()) {
					state->resolveError = error;
					refresh();
				}
			}),
			[](const QByteArray &bytes) {
				return ParseGift(bytes).has_value();
			});
	};

	tabs->sectionActivated() | rpl::on_next([=](int index) {
		state->tab = index;
		refresh();
		box->scrollToY(0);
	}, tabs->lifetime());
	SetupSearch(box, search, [=](QString query) {
		state->query = std::move(query);
		refresh();
	});
	list->retryRequests() | rpl::on_next([=] {
		if (state->gift.isEmpty()) {
			resolve();
			refresh();
		} else {
			for (auto &section : state->sections) {
				section.error = std::nullopt;
			}
			refresh();
			loadSections();
		}
	}, list->lifetime());
	list->activations() | rpl::on_next([=](ListActivation activation) {
		if (state->tab == kBackdropsTab) {
			if (activation.id < int(state->backdrops.size())) {
				ShowBackdropMenu(
					box,
					catalog->show,
					&state->menu,
					state->backdrops[activation.id]);
			}
			return;
		}
		const auto patterns = (state->tab == kPatternsTab);
		const auto &attributes = patterns
			? state->patterns
			: state->models;
		if (activation.id >= int(attributes.size())) {
			return;
		}
		ShowAssetMenu(box, catalog, &state->menu, {
			.type = patterns ? AssetType::Symbol : AssetType::Model,
			.gift = state->gift,
			.name = attributes[activation.id].name,
		});
	}, list->lifetime());

	if (resolved) {
		state->gift = key;
	}
	refresh();
	resolve();
	loadSections();
}

void GiftListBox(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<Catalog> catalog) {
	struct State {
		std::shared_ptr<Client> client;
		std::vector<QString> names;
		std::vector<QString> keys;
		std::vector<int> shown;
		bool loaded = false;
		QString query;
		rpl::variable<QString> count;
		MenuHolder menu;
	};
	const auto state = box->lifetime().make_state<State>();
	state->client = catalog->client;

	const auto attribution = PrepareCatalogBox(box);
	box->setTitle(tr::lng_oblivion_tools_gift_catalog());
	box->setAdditionalTitle(state->count.value());

	const auto search = box->setPinnedToTopContent(
		object_ptr<Ui::MultiSelect>(
			box,
			st::defaultMultiSelect,
			tr::lng_oblivion_gifts_search()));
	const auto list = box->addRow(
		object_ptr<ItemsList>(box, state->client),
		QMargins());
	FillScrollWithList(box, list, search, attribution, rpl::single(0));

	const auto refresh = [=] {
		if (!state->loaded) {
			return;
		}
		const auto query = SearchKey(state->query);
		auto items = std::vector<ListItem>();
		state->shown.clear();
		for (auto i = 0; i != int(state->names.size()); ++i) {
			if (!query.isEmpty() && !state->keys[i].contains(query)) {
				continue;
			}
			state->shown.push_back(i);
			items.push_back({
				.id = i,
				.title = state->names[i],
				.preview = AssetPreview({ .gift = state->names[i] }),
			});
		}
		list->setItems(
			ListLayout::Grid,
			std::move(items),
			(query.isEmpty()
				? tr::lng_oblivion_gifts_list_empty(tr::now)
				: tr::lng_oblivion_gifts_nothing_found(tr::now)));
		state->count = state->names.empty()
			? QString()
			: QString::number(state->names.size());
	};
	const auto load = [=] {
		list->setLoading();
		state->client->load(
			u"/gifts"_q,
			Cache::List,
			crl::guard(box, [=](QByteArray bytes) {
				auto names = ParseNames(bytes);
				if (!names) {
					list->setError(ErrorText(Error::Data));
					return;
				}
				state->names = std::move(*names);
				state->keys = state->names | ranges::views::transform(
					SearchKey
				) | ranges::to_vector;
				state->loaded = true;
				refresh();
			}),
			crl::guard(box, [=](Error error) {
				list->setError(ErrorText(error));
			}),
			[](const QByteArray &bytes) {
				return ParseNames(bytes).has_value();
			});
	};
	const auto open = [=](const QString &name) {
		catalog->show->showBox(
			Box(GiftDetailsBox, catalog, name, true, kModelsTab));
	};

	SetupSearch(box, search, [=](QString query) {
		state->query = std::move(query);
		refresh();
	});
	search->setSubmittedCallback([=](Qt::KeyboardModifiers) {
		if (!state->shown.empty()) {
			open(state->names[state->shown.front()]);
		}
	});
	list->retryRequests() | rpl::on_next(load, list->lifetime());
	list->activations() | rpl::on_next([=](ListActivation activation) {
		if (activation.id >= int(state->names.size())) {
			return;
		}
		const auto name = state->names[activation.id];
		if (!activation.context) {
			open(name);
			return;
		}
		ShowAssetMenu(
			box,
			catalog,
			&state->menu,
			{ .gift = name },
			[=](not_null<Ui::PopupMenu*> menu) {
				menu->addAction(
					tr::lng_oblivion_gifts_all_models(tr::now),
					[=] { open(name); },
					&st::menuIconUnique);
			});
	}, list->lifetime());

	load();
}

// UI snapshots (see oblivion_ui_snapshots.h): the catalog boxes over a
// small built-in sample of api.changes.tg responses in the same JSON
// shapes, with previews rendered from the Lottie animations bundled with
// the app and drawn symbol shapes, so the scenes need no network.

constexpr auto kSampleSceneWidth = 520;
constexpr auto kSampleSceneWait = crl::time(600);
constexpr auto kSampleGift = "Plush Pepe"_cs;
constexpr auto kSampleGiftId = "5936013938331222567"_cs;
constexpr auto kSampleGiftAnimation = "my_gifts_empty"_cs;
constexpr auto kSampleReleased = 1735732800; // 2025-01-01 12:00 UTC.
constexpr auto kSampleMinCoverage = 0.04;
constexpr auto kSamplePi = 3.14159265358979323846;

struct SampleGift {
	const char *name = nullptr;
	const char *animation = nullptr; // In :/animations/, without ".tgs".
};

const SampleGift kSampleGifts[] = {
	{ "Plush Pepe", "my_gifts_empty" },
	{ "Homemade Cake", "cake" },
	{ "Diamond Ring", "diamond" },
	{ "Party Sparkler", "chat/sparkles_emoji" },
	{ "Swiss Watch", "hours" },
	{ "Input Key", "passkeys" },
	{ "Tama Gadget", "phone" },
	{ "Jolly Chimp", "greeting" },
	{ "Magic Potion", "palette" },
	{ "Lunar Snake", "location" },
	{ "Sleigh Bell", "sleep" },
	{ "Record Player", "stats_earn" },
	{ "Light Sword", "winners" },
	{ "Stellar Rocket", "stats" },
	{ "Scared Cat", "media_forbidden" },
};

// The models are the sample gift animation recolored.
struct SampleModel {
	const char *name = nullptr;
	int rarity = 0; // Permille.
	int hue = 0;
	int saturation = 0;
	int lightness = 0;
};

const SampleModel kSampleModels[] = {
	{ "Cozy Galaxy", 5, 220, 10, 0 },
	{ "Ninja Mike", 8, 0, -100, -25 },
	{ "Emerald Plush", 10, 110, 0, 0 },
	{ "Pink Latex", 12, 290, 10, 5 },
	{ "Frozen", 15, 170, -20, 25 },
	{ "Amalgam", 15, 0, -100, 20 },
	{ "Midnight", 20, 230, 0, -25 },
	{ "Sunset", 20, 330, 20, 0 },
	{ "Gummy Frog", 25, 70, 10, 0 },
	{ "Original", 30, 0, 0, 0 },
	{ "Spring Bloom", 30, 40, -10, 15 },
	{ "Coral Reef", 30, 345, 0, 10 },
};

// The shape of a symbol is chosen by its index here, see SymbolShape().
struct SampleSymbol {
	const char *name = nullptr;
	int rarity = 0;
};

const SampleSymbol kSampleSymbols[] = {
	{ "Star", 2 },
	{ "Heart", 3 },
	{ "Gem", 4 },
	{ "Moon", 5 },
	{ "Lightning", 6 },
	{ "Drop", 8 },
	{ "Clover", 8 },
	{ "Crown", 10 },
	{ "Ring", 10 },
	{ "Flower", 12 },
	{ "Sparkle", 12 },
	{ "Paw", 15 },
};

struct SampleBackdrop {
	const char *name = nullptr;
	int rarity = 0;
	uint32 center = 0;
	uint32 edge = 0;
	uint32 pattern = 0;
	uint32 text = 0;
};

const SampleBackdrop kSampleBackdrops[] = {
	{ "Onyx Black", 5, 0x4D4D4D, 0x232323, 0x0B0B0B, 0xC6C6C6 },
	{ "English Violet", 10, 0xB186BB, 0x875A91, 0x54225F, 0xE6C7ED },
	{ "Midnight Blue", 10, 0x5C6FB3, 0x2D3A73, 0x16204A, 0xCBD5FF },
	{ "Emerald", 12, 0x78C585, 0x3D8B4D, 0x1F5A2B, 0xD8F5DD },
	{ "Coral Red", 12, 0xE8837A, 0xB04D45, 0x7A2620, 0xFFD9D4 },
	{ "Pure Gold", 15, 0xCCAB72, 0xA0793D, 0x6B4D19, 0xFDEFD2 },
	{ "Sky Blue", 15, 0x7FB3E3, 0x4D82B8, 0x25577F, 0xDCEFFF },
	{ "Pistachio", 20, 0x97B862, 0x6A8C38, 0x445F17, 0xEAF7D2 },
	{ "Rose Gold", 20, 0xD9A3A0, 0xAD7470, 0x7D4A47, 0xFBE4E2 },
	{ "Silver Blue", 20, 0x8FA3B8, 0x637689, 0x3B4C5C, 0xE3EDF7 },
};

[[nodiscard]] QByteArray SampleJson(const QJsonArray &array) {
	return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

[[nodiscard]] QByteArray SampleJson(const QJsonObject &object) {
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

[[nodiscard]] QImage SamplePreviewImage() {
	auto result = QImage(
		QSize(kPreviewSize, kPreviewSize),
		QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);
	return result;
}

// The part of the image that is not (almost) transparent.
[[nodiscard]] double Coverage(const QImage &image) {
	if (image.isNull()) {
		return 0.;
	}
	const auto argb = image.convertToFormat(
		QImage::Format_ARGB32_Premultiplied);
	auto opaque = 0;
	for (auto y = 0; y != argb.height(); ++y) {
		const auto line = reinterpret_cast<const QRgb*>(
			argb.constScanLine(y));
		for (auto x = 0; x != argb.width(); ++x) {
			if (qAlpha(line[x]) > 32) {
				++opaque;
			}
		}
	}
	return opaque / double(argb.width() * argb.height());
}

// A colored circle with the first letter, if an animation can't be used.
[[nodiscard]] QImage PlaceholderPreview(const QString &name) {
	auto hash = uint32(0);
	for (const auto &ch : name) {
		hash = hash * 31 + ch.unicode();
	}
	const auto hue = int(hash % 360);
	auto result = SamplePreviewImage();
	auto p = QPainter(&result);
	p.setRenderHint(QPainter::Antialiasing);
	const auto rect = QRectF(result.rect()).marginsRemoved(
		{ 12., 12., 12., 12. });
	auto gradient = QRadialGradient(rect.center(), rect.width() / 2.);
	gradient.setColorAt(0., QColor::fromHsl(hue, 170, 165));
	gradient.setColorAt(1., QColor::fromHsl(hue, 150, 100));
	p.setPen(Qt::NoPen);
	p.setBrush(gradient);
	p.drawEllipse(rect);
	auto font = st::semiboldFont->f;
	font.setPixelSize(kPreviewSize / 2);
	p.setFont(font);
	p.setPen(QColor(255, 255, 255));
	p.drawText(rect, Qt::AlignCenter, name.left(1).toUpper());
	p.end();
	return result;
}

// A frame from the middle of a bundled animation, optionally recolored.
[[nodiscard]] QImage LottiePreview(
		const QString &animation,
		const QString &name,
		int hue = 0,
		int saturation = 0,
		int lightness = 0) {
	static auto cache = base::flat_map<QString, QImage>();
	const auto key = u"%1:%2:%3:%4"_q
		.arg(animation)
		.arg(hue)
		.arg(saturation)
		.arg(lightness);
	if (const auto i = cache.find(key); i != end(cache)) {
		return i->second;
	}
	auto bytes = QByteArray();
	auto file = QFile(u":/animations/"_q + animation + u".tgs"_q);
	if (file.open(QIODevice::ReadOnly)) {
		bytes = file.readAll();
	}
	if (!bytes.isEmpty() && (hue || saturation || lightness)) {
		bytes = Oblivion::Lottie::AdjustColors(
			Oblivion::Lottie::Unpack(bytes),
			hue,
			saturation,
			lightness);
	}
	auto result = QImage();
	if (!bytes.isEmpty()) {
		auto renderer = Oblivion::Lottie::Renderer(bytes);
		if (renderer.valid()) {
			const auto size = QSize(kPreviewSize, kPreviewSize);
			const auto frames = renderer.info().frames;
			for (const auto frame : { frames / 2, 0 }) {
				result = renderer.render(frame, size);
				if (Coverage(result) >= kSampleMinCoverage) {
					break;
				}
				result = QImage();
			}
		}
	}
	if (result.isNull()) {
		result = PlaceholderPreview(name);
	}
	return cache.emplace(key, result).first->second;
}

[[nodiscard]] QPainterPath SampleCircle(double x, double y, double radius) {
	auto result = QPainterPath();
	result.addEllipse(QPointF(x, y), radius, radius);
	return result;
}

[[nodiscard]] QPainterPath SamplePolygon(QPolygonF points) {
	auto result = QPainterPath();
	result.addPolygon(points);
	result.closeSubpath();
	return result;
}

// Symbol silhouettes in about [-1, 1] coordinates, see kSampleSymbols.
[[nodiscard]] QPainterPath SymbolShape(int index) {
	switch (index) {
	case 0: { // Star.
		auto points = QPolygonF();
		for (auto i = 0; i != 10; ++i) {
			const auto radius = (i % 2) ? 0.45 : 1.;
			const auto angle = (-90. + 36. * i) * kSamplePi / 180.;
			points.push_back(QPointF(
				radius * std::cos(angle),
				radius * std::sin(angle)));
		}
		return SamplePolygon(points);
	}
	case 1: { // Heart.
		auto points = QPolygonF();
		constexpr auto kSteps = 96;
		for (auto i = 0; i != kSteps; ++i) {
			const auto t = 2. * kSamplePi * i / kSteps;
			const auto s = std::sin(t);
			points.push_back(QPointF(
				16. * s * s * s,
				-(13. * std::cos(t)
					- 5. * std::cos(2. * t)
					- 2. * std::cos(3. * t)
					- std::cos(4. * t))));
		}
		return SamplePolygon(points);
	}
	case 2: { // Gem.
		auto result = SamplePolygon({
			QPointF(-1., -0.35),
			QPointF(-0.55, -0.85),
			QPointF(0.55, -0.85),
			QPointF(1., -0.35),
			QPointF(0., 1.),
		});
		auto facet = QPainterPath();
		facet.addRect(QRectF(-1.1, -0.42, 2.2, 0.1));
		return result.subtracted(facet);
	}
	case 3: // Moon.
		return SampleCircle(0., 0., 1.).subtracted(
			SampleCircle(0.5, -0.3, 0.85));
	case 4: // Lightning.
		return SamplePolygon({
			QPointF(0.1, -1.),
			QPointF(-0.5, 0.15),
			QPointF(0., 0.15),
			QPointF(-0.15, 1.),
			QPointF(0.5, -0.2),
			QPointF(0., -0.2),
			QPointF(0.35, -1.),
		});
	case 5: // Drop.
		return SampleCircle(0., 0.3, 0.7).united(SamplePolygon({
			QPointF(0., -1.),
			QPointF(0.59, -0.077),
			QPointF(0., 0.3),
			QPointF(-0.59, -0.077),
		}));
	case 6: { // Clover.
		auto result = SamplePolygon({
			QPointF(0., 0.),
			QPointF(0.1, 0.),
			QPointF(0.5, 1.05),
			QPointF(0.38, 1.1),
		});
		const auto leaves = std::array{
			QPointF(0., -0.45),
			QPointF(0.45, 0.),
			QPointF(0., 0.45),
			QPointF(-0.45, 0.),
		};
		for (const auto &leaf : leaves) {
			result = result.united(SampleCircle(leaf.x(), leaf.y(), 0.42));
		}
		return result;
	}
	case 7: { // Crown.
		auto result = SamplePolygon({
			QPointF(-1., 0.55),
			QPointF(-1., -0.55),
			QPointF(-0.5, 0.),
			QPointF(0., -0.85),
			QPointF(0.5, 0.),
			QPointF(1., -0.55),
			QPointF(1., 0.55),
		});
		auto band = QPainterPath();
		band.addRect(QRectF(-1., 0.67, 2., 0.3));
		return result.united(band)
			.united(SampleCircle(-1., -0.62, 0.14))
			.united(SampleCircle(0., -0.92, 0.14))
			.united(SampleCircle(1., -0.62, 0.14));
	}
	case 8: // Ring.
		return SampleCircle(0., 0.2, 0.8).subtracted(
			SampleCircle(0., 0.2, 0.52)
		).united(SamplePolygon({
			QPointF(-0.3, -0.62),
			QPointF(-0.15, -0.82),
			QPointF(0.15, -0.82),
			QPointF(0.3, -0.62),
			QPointF(0., -0.45),
		}));
	case 9: { // Flower.
		auto result = SampleCircle(0., 0., 0.35);
		for (auto i = 0; i != 6; ++i) {
			const auto angle = (60. * i - 90.) * kSamplePi / 180.;
			result = result.united(SampleCircle(
				0.6 * std::cos(angle),
				0.6 * std::sin(angle),
				0.4));
		}
		return result.subtracted(SampleCircle(0., 0., 0.18));
	}
	case 10: { // Sparkle.
		auto result = QPainterPath();
		result.moveTo(0., -1.);
		result.quadTo(0.15, -0.15, 1., 0.);
		result.quadTo(0.15, 0.15, 0., 1.);
		result.quadTo(-0.15, 0.15, -1., 0.);
		result.quadTo(-0.15, -0.15, 0., -1.);
		result.closeSubpath();
		return result;
	}
	}
	// Paw.
	auto pad = QPainterPath();
	pad.addEllipse(QPointF(0., 0.4), 0.55, 0.45);
	return pad
		.united(SampleCircle(-0.72, -0.15, 0.22))
		.united(SampleCircle(-0.3, -0.6, 0.24))
		.united(SampleCircle(0.3, -0.6, 0.24))
		.united(SampleCircle(0.72, -0.15, 0.22));
}

// Symbols come as black silhouettes, the list recolors them.
[[nodiscard]] QImage SymbolPreview(int index) {
	auto result = SamplePreviewImage();
	const auto path = SymbolShape(index);
	const auto bounds = path.boundingRect();
	if (bounds.isEmpty()) {
		return result;
	}
	const auto padding = kPreviewSize / 8.;
	const auto target = QRectF(result.rect()).marginsRemoved(
		{ padding, padding, padding, padding });
	const auto scale = std::min(
		target.width() / bounds.width(),
		target.height() / bounds.height());
	auto transform = QTransform();
	transform.translate(target.center().x(), target.center().y());
	transform.scale(scale, scale);
	transform.translate(-bounds.center().x(), -bounds.center().y());
	auto p = QPainter(&result);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0, 0, 0));
	p.drawPath(transform.map(path));
	p.end();
	return result;
}

// The responses and previews of the gift list.
[[nodiscard]] const Client::Fixture &SampleListFixture() {
	static const auto result = [] {
		auto fixture = Client::Fixture();
		auto names = QJsonArray();
		for (const auto &gift : kSampleGifts) {
			const auto name = QString::fromUtf8(gift.name);
			names.push_back(name);
			fixture.previews.emplace(
				AssetPreview({ .gift = name }),
				LottiePreview(QString::fromUtf8(gift.animation), name));
		}
		fixture.responses.emplace(u"/gifts"_q, SampleJson(names));
		return fixture;
	}();
	return result;
}

// The responses and previews of the kSampleGift page.
[[nodiscard]] const Client::Fixture &SampleGiftFixture() {
	static const auto result = [] {
		const auto gift = kSampleGift.utf16();
		const auto animation = kSampleGiftAnimation.utf16();
		auto fixture = Client::Fixture();
		fixture.previews.emplace(
			AssetPreview({ .gift = gift }),
			LottiePreview(animation, gift));

		auto models = QJsonArray();
		auto giftModels = QJsonArray();
		for (const auto &model : kSampleModels) {
			const auto name = QString::fromUtf8(model.name);
			models.push_back(QJsonObject{
				{ u"name"_q, name },
				{ u"rarityPermille"_q, model.rarity },
			});
			giftModels.push_back(QJsonObject{
				{ u"name"_q, name },
				{ u"rarity"_q, model.rarity },
			});
			fixture.previews.emplace(
				AssetPreview({
					.type = AssetType::Model,
					.gift = gift,
					.name = name,
				}),
				LottiePreview(
					animation,
					name,
					model.hue,
					model.saturation,
					model.lightness));
		}

		auto symbols = QJsonArray();
		for (auto i = 0; i != int(std::size(kSampleSymbols)); ++i) {
			const auto name = QString::fromUtf8(kSampleSymbols[i].name);
			symbols.push_back(QJsonObject{
				{ u"name"_q, name },
				{ u"rarityPermille"_q, kSampleSymbols[i].rarity },
			});
			fixture.previews.emplace(
				AssetPreview({
					.type = AssetType::Symbol,
					.gift = gift,
					.name = name,
				}),
				SymbolPreview(i));
		}

		auto backdrops = QJsonArray();
		for (const auto &backdrop : kSampleBackdrops) {
			const auto hex = [](uint32 rgb) {
				return QColor::fromRgb(QRgb(rgb)).name();
			};
			backdrops.push_back(QJsonObject{
				{ u"name"_q, QString::fromUtf8(backdrop.name) },
				{ u"centerColor"_q, qint64(backdrop.center) },
				{ u"edgeColor"_q, qint64(backdrop.edge) },
				{ u"patternColor"_q, qint64(backdrop.pattern) },
				{ u"textColor"_q, qint64(backdrop.text) },
				{ u"rarityPermille"_q, backdrop.rarity },
				{ u"hex"_q, QJsonObject{
					{ u"centerColor"_q, hex(backdrop.center) },
					{ u"edgeColor"_q, hex(backdrop.edge) },
					{ u"patternColor"_q, hex(backdrop.pattern) },
					{ u"textColor"_q, hex(backdrop.text) },
				} },
			});
		}

		fixture.responses.emplace(GiftPath(gift), SampleJson(QJsonObject{
			{ u"gift"_q, QJsonObject{
				{ u"name"_q, gift },
				{ u"id"_q, kSampleGiftId.utf16() },
				{ u"releasedAt"_q, kSampleReleased },
				{ u"upgradable"_q, true },
				{ u"auction"_q, false },
			} },
			{ u"models"_q, giftModels },
		}));
		fixture.responses.emplace(
			SectionPath(kModelsTab, gift),
			SampleJson(models));
		fixture.responses.emplace(
			SectionPath(kPatternsTab, gift),
			SampleJson(symbols));
		fixture.responses.emplace(
			SectionPath(kBackdropsTab, gift),
			SampleJson(backdrops));
		return fixture;
	}();
	return result;
}

[[nodiscard]] std::shared_ptr<Catalog> SampleCatalog(
		std::shared_ptr<Ui::Show> show,
		Client::Fixture fixture) {
	return std::make_shared<Catalog>(Catalog{
		.show = std::move(show),
		.client = std::make_shared<Client>(std::move(fixture)),
	});
}

void RegisterSampleScene(
		QString name,
		SelfTest::BoxScene box,
		SelfTest::ScenePrepare prepare = nullptr) {
	SelfTest::RegisterScene({
		.name = std::move(name),
		.size = QSize(Px(kSampleSceneWidth), 0),
		.box = std::move(box),
		.prepare = std::move(prepare),
		// The sample is served synchronously, the wait only lets
		// the layout and the short show animations finish.
		.wait = kSampleSceneWait,
	});
}

// Types a query into the search field of the box.
[[nodiscard]] SelfTest::ScenePrepare SampleSearch(QString query) {
	return [=](not_null<QWidget*> widget) {
		// Ui::MultiSelect has no Q_OBJECT, so findChild can't look for it.
		for (const auto child : widget->findChildren<QWidget*>()) {
			if (const auto search = dynamic_cast<Ui::MultiSelect*>(child)) {
				search->setQuery(query);
				return;
			}
		}
	};
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	RegisterSampleScene(u"catalog_list"_q, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(GiftListBox, SampleCatalog(show, SampleListFixture()));
	});
	const auto details = [](int tab) {
		return [=](std::shared_ptr<Ui::Show> show) {
			return Box(
				GiftDetailsBox,
				SampleCatalog(show, SampleGiftFixture()),
				kSampleGift.utf16(),
				true,
				tab);
		};
	};
	RegisterSampleScene(u"catalog_models"_q, details(kModelsTab));
	RegisterSampleScene(u"catalog_patterns"_q, details(kPatternsTab));
	RegisterSampleScene(u"catalog_backdrops"_q, details(kBackdropsTab));
	RegisterSampleScene(u"catalog_error"_q, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(GiftListBox, SampleCatalog(show, Client::Fixture{
			.error = Error::Network,
		}));
	});
	RegisterSampleScene(u"catalog_loading"_q, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(GiftListBox, SampleCatalog(show, Client::Fixture{
			.hang = true,
		}));
	});
	RegisterSampleScene(u"catalog_search_empty"_q, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(GiftListBox, SampleCatalog(show, SampleListFixture()));
	}, SampleSearch(u"xyz"_q));
	RegisterSampleScene(
		u"catalog_patterns_search"_q,
		details(kPatternsTab),
		SampleSearch(u"o"_q));
	// Opened by a gift name that is not in the catalog.
	RegisterSampleScene(u"catalog_not_found"_q, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(
			GiftDetailsBox,
			SampleCatalog(show, SampleGiftFixture()),
			u"Unknown Gift"_q,
			false,
			kModelsTab);
	});
});

} // namespace

void ShowGiftCatalog(
		not_null<Window::SessionController*> controller,
		const QString &gift) {
	const auto weak = base::make_weak(controller);
	const auto catalog = std::make_shared<Catalog>(Catalog{
		.show = controller->uiShow(),
		.client = Client::Instance(),
		.openEditor = [=](QByteArray bytes, QString name) {
			if (const auto strong = weak.get()) {
				ShowStickerStudio(strong, std::move(bytes), std::move(name));
			}
		},
	});
	const auto key = gift.trimmed();
	if (key.isEmpty()) {
		controller->show(Box(GiftListBox, catalog));
	} else {
		controller->show(
			Box(GiftDetailsBox, catalog, key, false, kModelsTab));
	}
}

} // namespace Oblivion
