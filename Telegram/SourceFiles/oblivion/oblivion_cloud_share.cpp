/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_cloud_share.h"

#include "apiwrap.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "core/file_location.h"
#include "core/mime_type.h"
#include "data/data_audio_msg_id.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_list_widget.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_instance.h"
#include "oblivion/oblivion_cloud_share_ui.h"
#include "oblivion/oblivion_photo_fx.h"
#include "oblivion/oblivion_video_fx.h"
#include "storage/file_download.h"
#include "ui/chat/attach/attach_prepare.h"
#include "settings.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QTemporaryDir>
#include <QtGui/QLinearGradient>
#include <QtGui/QRadialGradient>

namespace Oblivion::Share {
namespace {

constexpr auto kFileVersion = 1;
constexpr auto kPresetFormat = 1;
constexpr auto kMaxSaved = 200;
// How many deleted presets are remembered (for the settings sync) and
// for how long: a device that was not used for longer than that may
// bring a deleted preset back.
constexpr auto kMaxRemovedPresets = 2000;
constexpr auto kRemovedLife = int64(180) * 24 * 60 * 60;
constexpr auto kMaxKnownMedia = 5000;
constexpr auto kMaxPublished = 500;
constexpr auto kMaxOwnerName = 64;
constexpr auto kMaxTagLength = 40;
constexpr auto kMaxTags = 16;
constexpr auto kMaxTagsLine = 140;
constexpr auto kDefaultAudioLimit = int64(83886080);
constexpr auto kMaxMediaSize = int64(1) << 40;
constexpr auto kResolveTimeout = crl::time(15000);
constexpr auto kPreviewSourceWidth = 1600;
constexpr auto kKeepResumeDelay = crl::time(10'000);
constexpr auto kKeepGoneRun = 5;

// «Добавить к себе»: the pause before the files that could not be taken
// are tried again. It grows and stays at an hour, so a computer without
// connection asks once an hour at most.
[[nodiscard]] crl::time KeepRetryDelay(int failures) {
	constexpr auto kMinute = crl::time(60'000);
	return (failures <= 0)
		? kMinute
		: (failures == 1)
		? (5 * kMinute)
		: (failures == 2)
		? (15 * kMinute)
		: (60 * kMinute);
}

// The file is not on the server any more (or not for this user), or what
// came does not pass the check: asking again changes nothing. Anything
// else is taken for a lost connection.
[[nodiscard]] bool KeepGoneError(const Cloud::Error &error) {
	using Type = Cloud::Error::Type;
	return (error.type == Type::File)
		|| (error.type == Type::Protocol)
		|| (error.type == Type::Http
			&& (error.status == 403
				|| error.status == 404
				|| error.status == 410
				|| error.status == 416));
}

// A track the server does not take as it is (a field of its "tracks"
// entry): the upload goes on with the next one instead of stopping at
// the same track with every «Повторить».
[[nodiscard]] bool TrackRefused(const Cloud::Error &error) {
	return (error.type == Cloud::Error::Type::Http)
		&& (error.status == 400)
		&& error.detail("field").startsWith(u"tracks["_q);
}

// Tracks whose files the server has already are added one right after
// another, and the server may ask to slow down (429). The pause before
// the same track is added again, 0 when the upload should stop instead:
// a long wait or a server that refuses again and again.
[[nodiscard]] crl::time AddAgainDelay(const Cloud::Error &error, int retries) {
	constexpr auto kMaxRetries = 5;
	constexpr auto kMinWait = crl::time(1'000);
	constexpr auto kMaxWait = crl::time(30'000);
	if (error.type != Cloud::Error::Type::Http
		|| error.status != 429
		|| retries >= kMaxRetries
		|| error.retryAfter > kMaxWait) {
		return 0;
	}
	// A little longer every time, in case the server gave no number.
	return std::clamp(
		std::max(error.retryAfter, kMinWait * (retries + 1)),
		kMinWait,
		kMaxWait);
}

[[nodiscard]] QString TagsPrefix() {
	return u"fx: "_q;
}

[[nodiscard]] int ClampInt(int64 value, int64 low, int64 high) {
	return int(std::clamp(value, low, high));
}

[[nodiscard]] bool ValidTrackId(const QString &id) {
	if (id.isEmpty() || id.size() > 32) {
		return false;
	}
	for (const auto ch : id) {
		const auto code = ch.unicode();
		const auto good = (code >= 'a' && code <= 'z')
			|| (code >= 'A' && code <= 'Z')
			|| (code >= '0' && code <= '9')
			|| (code == '_')
			|| (code == '-');
		if (!good) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool ValidTag(const QString &tag) {
	if (tag.isEmpty() || tag.size() > kMaxTagLength) {
		return false;
	}
	for (const auto ch : tag) {
		const auto code = ch.unicode();
		const auto good = (code >= 'a' && code <= 'z')
			|| (code >= '0' && code <= '9')
			|| (code == '_')
			|| (code == '-')
			|| (code == '.');
		if (!good) {
			return false;
		}
	}
	return true;
}

// "audio/mpeg" and the like: what is safe to hand to the player and to
// send in a header.
[[nodiscard]] QString CleanMime(const QString &mime) {
	const auto lower = mime.trimmed().toLower();
	if (lower.isEmpty() || lower.size() > 64) {
		return QString();
	}
	auto slashes = 0;
	for (const auto ch : lower) {
		const auto code = ch.unicode();
		if (code == '/') {
			++slashes;
		} else if (!((code >= 'a' && code <= 'z')
			|| (code >= '0' && code <= '9')
			|| (code == '.')
			|| (code == '+')
			|| (code == '-'))) {
			return QString();
		}
	}
	return (slashes == 1) ? lower : QString();
}

// What the media relay takes for the kind "audio".
[[nodiscard]] QString UploadMime(const QString &mime) {
	const auto clean = CleanMime(mime);
	return (clean.startsWith(u"audio/"_q)
		|| clean == u"video/mp4"_q
		|| clean == u"application/ogg"_q)
		? clean
		: u"application/octet-stream"_q;
}

[[nodiscard]] std::optional<Track> ParseTrack(const QJsonObject &object) {
	auto result = Track();
	result.id = object.value(u"id"_q).toString();
	result.media = object.value(u"media"_q).toString();
	if (!ValidTrackId(result.id) || !Cloud::ValidMediaId(result.media)) {
		return std::nullopt;
	}
	result.size = std::clamp(
		Cloud::JsonInt(object.value(u"size"_q)),
		int64(0),
		kMaxMediaSize);
	result.mime = CleanMime(object.value(u"mime"_q).toString());
	result.title = Cloud::JsonText(object.value(u"title"_q), kMaxTrackText);
	result.performer = Cloud::JsonText(
		object.value(u"performer"_q),
		kMaxTrackText);
	result.fileName = Cloud::JsonText(
		object.value(u"file_name"_q),
		kMaxTrackText).replace(QChar('/'), QChar('_')).replace(
			QChar('\\'),
			QChar('_'));
	result.duration = std::clamp(
		Cloud::JsonInt(object.value(u"duration_ms"_q)),
		int64(0),
		kMaxTrackDuration);
	result.addedBy = Cloud::JsonUserId(object.value(u"added_by"_q));
	return result;
}

[[nodiscard]] QJsonObject TrackToJson(const Track &track) {
	auto result = QJsonObject();
	result.insert(u"id"_q, track.id);
	result.insert(u"media"_q, track.media);
	result.insert(u"size"_q, double(track.size));
	result.insert(u"mime"_q, track.mime);
	result.insert(u"title"_q, track.title);
	result.insert(u"performer"_q, track.performer);
	result.insert(u"file_name"_q, track.fileName);
	result.insert(u"duration_ms"_q, double(track.duration));
	result.insert(u"added_by"_q, double(track.addedBy));
	return result;
}

[[nodiscard]] QString AccountFolder(not_null<Main::Session*> session) {
	return cWorkingDir()
		+ u"tdata/oblivion/"_q
		+ (session->isTestMode() ? u"test_"_q : QString())
		+ QString::number(session->userId().bare)
		+ '/';
}

[[nodiscard]] QJsonObject ReadJsonFile(const QString &path) {
	if (path.isEmpty()) {
		return QJsonObject();
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return QJsonObject();
	}
	return QJsonDocument::fromJson(file.readAll()).object();
}

void WriteJsonFile(const QString &path, const QJsonObject &object) {
	if (path.isEmpty()) {
		return;
	}
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		LOG(("Oblivion Share: could not open %1 for writing.").arg(path));
		return;
	}
	file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
	if (!file.commit()) {
		LOG(("Oblivion Share: could not save %1.").arg(path));
	}
}

// ---- Stacks of effects.

[[nodiscard]] std::vector<Photo::FxInstance> SanePhotoStack(
		const QByteArray &stack) {
	auto result = std::vector<Photo::FxInstance>();
	for (auto &instance : Photo::DeserializeFxStack(stack)) {
		const auto descriptor = Photo::FindFx(instance.id);
		if (!descriptor) {
			continue;
		} else if (int(result.size()) >= kMaxPhotoEffects) {
			break;
		}
		instance.params = Photo::NormalizedFxParams(
			*descriptor,
			instance.params);
		instance.uid = 0;
		result.push_back(std::move(instance));
	}
	return result;
}

[[nodiscard]] VideoFx::Stack SaneVideoStack(const QByteArray &stack) {
	auto result = VideoFx::Deserialize(stack);
	if (int(result.size()) > kMaxVideoEffects) {
		result.resize(kMaxVideoEffects);
	}
	for (auto &entry : result) {
		entry = VideoFx::Sanitized(std::move(entry));
	}
	return result;
}

[[nodiscard]] QImage RenderPhoto(const QByteArray &stack, QImage sample) {
	auto image = sample.convertToFormat(
		QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(1.);
	image.detach();
	if (image.isNull() || image.width() <= 0) {
		return sample;
	}
	auto context = Photo::FxContext();
	context.scale = image.width() / float64(kPreviewSourceWidth);
	context.seed = 0x0B11F10FU;
	context.fullSize = QSize(
		kPreviewSourceWidth,
		kPreviewSourceWidth * image.height() / image.width());
	context.preview = true;
	return Photo::ApplyFxStack(image, SanePhotoStack(stack), context)
		? image
		: sample;
}

// ---- The sample picture: an evening by the sea. Flat colours next to
// soft gradients and a few small bright things, so that blurs, colour
// shifts, grain and glitches all have something to show.

void PaintSample(QPainter &p, QSize size) {
	const auto w = float64(size.width());
	const auto h = float64(size.height());
	const auto horizon = h * 0.62;

	auto sky = QLinearGradient(0., 0., 0., horizon);
	sky.setColorAt(0., QColor(0x1b, 0x1f, 0x4b));
	sky.setColorAt(0.45, QColor(0x6a, 0x3f, 0x8f));
	sky.setColorAt(0.8, QColor(0xf2, 0x7a, 0x5e));
	sky.setColorAt(1., QColor(0xff, 0xc9, 0x6b));
	p.fillRect(QRectF(0., 0., w, horizon), sky);

	const auto sun = QPointF(w * 0.68, horizon - h * 0.08);
	auto glow = QRadialGradient(sun, h * 0.42);
	glow.setColorAt(0., QColor(0xff, 0xf3, 0xc4, 230));
	glow.setColorAt(0.25, QColor(0xff, 0xb3, 0x6b, 120));
	glow.setColorAt(1., QColor(0xff, 0x8a, 0x5c, 0));
	p.fillRect(QRectF(0., 0., w, horizon), glow);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0xff, 0xf6, 0xd8));
	p.drawEllipse(sun, h * 0.075, h * 0.075);

	// Stars and clouds.
	p.setBrush(QColor(255, 255, 255, 210));
	for (auto i = 0; i != 26; ++i) {
		const auto x = w * (((i * 137) % 100) / 100.);
		const auto y = horizon * 0.5 * (((i * 61 + 17) % 100) / 100.);
		const auto r = h * (0.002 + ((i * 29) % 5) * 0.0008);
		p.drawEllipse(QPointF(x, y), r, r);
	}
	p.setBrush(QColor(0xff, 0xd6, 0xc2, 130));
	const auto cloud = [&](float64 x, float64 y, float64 scale) {
		p.drawEllipse(QPointF(w * x, h * y), w * 0.09 * scale, h * 0.022);
		p.drawEllipse(
			QPointF(w * (x + 0.05 * scale), h * (y - 0.018)),
			w * 0.06 * scale,
			h * 0.024);
		p.drawEllipse(
			QPointF(w * (x - 0.05 * scale), h * (y + 0.008)),
			w * 0.07 * scale,
			h * 0.018);
	};
	cloud(0.22, 0.3, 1.);
	cloud(0.48, 0.2, 0.7);
	cloud(0.86, 0.36, 0.9);

	// Far and near hills.
	auto hills = QPainterPath();
	hills.moveTo(0., horizon);
	hills.lineTo(0., horizon - h * 0.1);
	hills.cubicTo(
		w * 0.15, horizon - h * 0.24,
		w * 0.3, horizon - h * 0.02,
		w * 0.46, horizon - h * 0.12);
	hills.cubicTo(
		w * 0.55, horizon - h * 0.18,
		w * 0.6, horizon - h * 0.04,
		w * 0.7, horizon);
	hills.closeSubpath();
	p.setBrush(QColor(0x4a, 0x2e, 0x6b));
	p.drawPath(hills);

	auto sea = QLinearGradient(0., horizon, 0., h);
	sea.setColorAt(0., QColor(0xf0, 0x8f, 0x6a));
	sea.setColorAt(0.25, QColor(0x8a, 0x4f, 0x8e));
	sea.setColorAt(1., QColor(0x12, 0x1a, 0x3e));
	p.fillRect(QRectF(0., horizon, w, h - horizon), sea);

	// The path of the sun on the water.
	for (auto i = 0; i != 9; ++i) {
		const auto y = horizon + (h - horizon) * (0.05 + i * 0.1);
		const auto half = w * (0.03 + i * 0.012);
		p.setBrush(QColor(0xff, 0xe2, 0xa8, 190 - i * 17));
		p.drawRoundedRect(
			QRectF(sun.x() - half, y, half * 2., h * 0.008),
			h * 0.004,
			h * 0.004);
	}

	auto shore = QPainterPath();
	shore.moveTo(0., h);
	shore.lineTo(0., h * 0.8);
	shore.cubicTo(w * 0.12, h * 0.74, w * 0.26, h * 0.84, w * 0.4, h * 0.9);
	shore.cubicTo(w * 0.46, h * 0.93, w * 0.5, h * 0.97, w * 0.56, h);
	shore.closeSubpath();
	p.setBrush(QColor(0x14, 0x12, 0x2b));
	p.drawPath(shore);

	// A lighthouse with a lit window and a sail.
	p.setBrush(QColor(0xf4, 0xf1, 0xea));
	const auto tower = QRectF(w * 0.14, h * 0.5, w * 0.035, h * 0.29);
	p.drawRect(tower);
	p.setBrush(QColor(0xd8, 0x3a, 0x3a));
	p.drawRect(QRectF(tower.x(), h * 0.58, tower.width(), h * 0.05));
	p.drawRect(QRectF(tower.x(), h * 0.69, tower.width(), h * 0.05));
	p.setBrush(QColor(0x22, 0x1f, 0x3a));
	p.drawRect(QRectF(
		tower.x() - w * 0.006,
		h * 0.47,
		tower.width() + w * 0.012,
		h * 0.03));
	p.setBrush(QColor(0xff, 0xea, 0x6e));
	p.drawEllipse(
		QPointF(tower.center().x(), h * 0.485),
		w * 0.009,
		w * 0.009);

	auto sail = QPainterPath();
	sail.moveTo(w * 0.84, h * 0.7);
	sail.lineTo(w * 0.84, h * 0.56);
	sail.lineTo(w * 0.9, h * 0.7);
	sail.closeSubpath();
	p.setBrush(QColor(0xff, 0xff, 0xff));
	p.drawPath(sail);
	p.setBrush(QColor(0x2f, 0xc7, 0xc0));
	p.drawRoundedRect(
		QRectF(w * 0.81, h * 0.705, w * 0.11, h * 0.018),
		h * 0.008,
		h * 0.008);
}

// ---- Self-test.

class Checker final {
public:
	explicit Checker(QStringList &log);

	void operator()(bool condition, const char *what);
	void section(const char *name);
	[[nodiscard]] int failed() const;

private:
	QStringList &_log;
	int _failed = 0;
	int _sectionPassed = 0;
	int _sectionFailed = 0;

};

Checker::Checker(QStringList &log)
: _log(log) {
}

void Checker::operator()(bool condition, const char *what) {
	if (condition) {
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

int Checker::failed() const {
	return _failed;
}

[[nodiscard]] QJsonObject ThroughText(const QJsonObject &object) {
	return QJsonDocument::fromJson(
		QJsonDocument(object).toJson(QJsonDocument::Compact)).object();
}

[[nodiscard]] QJsonObject SamplePlaylistJson() {
	const auto media = QString(64, QChar('a'));
	auto owner = QJsonObject();
	owner.insert(u"id"_q, 123456789.);
	owner.insert(u"name"_q, u"Misha"_q);
	auto first = QJsonObject();
	first.insert(u"id"_q, u"t8Qz12abCD"_q);
	first.insert(u"media"_q, media);
	first.insert(u"size"_q, 9123456.);
	first.insert(u"mime"_q, u"audio/mpeg"_q);
	first.insert(u"title"_q, u"Song"_q);
	first.insert(u"performer"_q, u"Artist"_q);
	first.insert(u"duration_ms"_q, 215000.);
	first.insert(u"file_name"_q, u"../../song.mp3"_q);
	first.insert(u"added_by"_q, 123456789.);
	auto broken = first;
	broken.insert(u"media"_q, u"not a hash"_q);
	auto hostile = first;
	hostile.insert(u"id"_q, u"t2"_q);
	hostile.insert(u"title"_q, QString(5000, QChar('x')) + u"\n\tend"_q);
	hostile.insert(u"mime"_q, u"audio/mpeg\r\nX-Evil: 1"_q);
	hostile.insert(u"duration_ms"_q, 1e15);
	hostile.insert(u"size"_q, -5.);
	auto tracks = QJsonArray();
	tracks.push_back(first);
	tracks.push_back(broken);
	tracks.push_back(hostile);
	tracks.push_back(u"garbage"_q);
	auto result = QJsonObject();
	result.insert(u"id"_q, u"mK3abcdefghijklmnopq22"_q);
	result.insert(u"link"_q, u"https://evil.example/p/x"_q);
	result.insert(u"owner"_q, owner);
	result.insert(u"title"_q, u"Night"_q);
	result.insert(u"description"_q, u"line one\nline two"_q);
	result.insert(u"collab"_q, true);
	result.insert(u"public"_q, false);
	result.insert(u"rev"_q, 5.);
	result.insert(u"followers"_q, 3.);
	result.insert(u"can_edit"_q, false);
	result.insert(u"can_add"_q, true);
	result.insert(u"total_bytes"_q, 123456789.);
	result.insert(u"total_duration_ms"_q, 3600000.);
	result.insert(u"tracks"_q, tracks);
	return result;
}

void TestPlaylists(Checker &check) {
	const auto parsed = ParsePlaylist(ThroughText(SamplePlaylistJson()));
	check(parsed.valid(), "playlist: a valid object is parsed");
	check(parsed.full, "playlist: the tracks make it a full one");
	check(parsed.tracks.size() == 2, "playlist: broken tracks are dropped");
	check(parsed.trackCount == 2, "playlist: the count follows the tracks");
	check(parsed.link == Cloud::MakeLink(
		Cloud::LinkKind::Playlist,
		u"mK3abcdefghijklmnopq22"_q),
		"playlist: the link is made here, not taken from the server");
	check(parsed.ownerId == 123456789 && parsed.ownerName == u"Misha"_q,
		"playlist: the owner");
	check(parsed.collab && !parsed.listed && parsed.canAdd && !parsed.canEdit,
		"playlist: the flags");
	check(parsed.description.contains(QChar('\n')),
		"playlist: the description keeps its lines");
	if (parsed.tracks.size() == 2) {
		const auto &first = parsed.tracks[0];
		check(first.title == u"Song"_q
			&& first.performer == u"Artist"_q
			&& first.duration == 215000
			&& first.size == 9123456,
			"playlist: a track");
		check(!first.fileName.contains(QChar('/')),
			"playlist: no slashes in a file name");
		const auto &second = parsed.tracks[1];
		check(second.title.size() <= kMaxTrackText,
			"playlist: a long title is cut");
		check(!second.title.contains(QChar('\n'))
			&& !second.title.contains(QChar('\t')),
			"playlist: a title is one line");
		check(second.mime.isEmpty(), "playlist: a bad mime is dropped");
		check(second.duration == kMaxTrackDuration,
			"playlist: a duration is clamped");
		check(second.size == 0, "playlist: a negative size is zero");
	}

	const auto again = ParsePlaylist(ThroughText(PlaylistToJson(parsed)));
	check(again.valid()
		&& again.id == parsed.id
		&& again.title == parsed.title
		&& again.description == parsed.description
		&& again.ownerId == parsed.ownerId
		&& again.ownerName == parsed.ownerName
		&& again.collab == parsed.collab
		&& again.canAdd == parsed.canAdd
		&& again.rev == parsed.rev
		&& again.tracks.size() == parsed.tracks.size()
		&& again.totalDuration == parsed.totalDuration,
		"playlist: the saved copy reads back the same");
	if (again.tracks.size() == 2 && parsed.tracks.size() == 2) {
		check(again.tracks[1].id == parsed.tracks[1].id
			&& again.tracks[1].media == parsed.tracks[1].media
			&& again.tracks[1].title == parsed.tracks[1].title
			&& again.tracks[1].duration == parsed.tracks[1].duration,
			"playlist: the saved tracks read back the same");
	}

	auto summary = SamplePlaylistJson();
	summary.remove(u"tracks"_q);
	summary.insert(u"track_count"_q, 77.);
	const auto brief = ParsePlaylist(summary);
	check(brief.valid() && !brief.full && brief.trackCount == 77,
		"playlist: a summary has only the count");

	auto bad = SamplePlaylistJson();
	bad.insert(u"id"_q, u"../../etc/passwd"_q);
	check(!ParsePlaylist(bad).valid(), "playlist: a bad id is refused");
	check(!ParsePlaylist(QJsonObject()).valid(),
		"playlist: an empty object is refused");

	auto many = SamplePlaylistJson();
	auto list = QJsonArray();
	const auto track = many.value(u"tracks"_q).toArray().at(0).toObject();
	for (auto i = 0; i != kMaxPlaylistTracks + 50; ++i) {
		list.push_back(track);
	}
	many.insert(u"tracks"_q, list);
	check(int(ParsePlaylist(many).tracks.size()) == kMaxPlaylistTracks,
		"playlist: not more tracks than the limit");

	const auto input = TrackInputToJson({
		.media = QString(64, QChar('b')),
		.title = QString(),
		.performer = QString(1000, QChar('p')),
		.fileName = u"dir/name.mp3"_q,
		.duration = 0,
	});
	check(!input.value(u"title"_q).toString().isEmpty(),
		"input: the title is never empty");
	check(input.value(u"title"_q).toString() == u"name"_q,
		"input: the title falls back to the file name");
	check(input.value(u"performer"_q).toString().size() == kMaxTrackText,
		"input: the performer is cut");
	check(input.value(u"duration_ms"_q).toDouble() >= 1.,
		"input: the duration is at least a millisecond");
	check(!input.value(u"file_name"_q).toString().contains(QChar('/')),
		"input: no folder in the file name");
	const auto longInput = TrackInputToJson({
		.media = QString(64, QChar('b')),
		.title = u"  A  title\twith\nbreaks  "_q,
		.duration = int64(1) << 50,
	});
	check(longInput.value(u"title"_q).toString() == u"A title with breaks"_q,
		"input: the title is one clean line");
	check(longInput.value(u"duration_ms"_q).toDouble()
		== double(kMaxTrackDuration),
		"input: the duration is at most a day");

	check(TrackName(u"Title"_q, u"Artist"_q, u"file.mp3"_q)
		== QString::fromUtf8("Artist \xE2\x80\x94 Title"),
		"name: the performer and the title");
	check(TrackName(u"Title"_q, QString(), u"file.mp3"_q) == u"Title"_q,
		"name: only the title");
	check(TrackName(QString(), QString(), u"file.mp3"_q) == u"file.mp3"_q,
		"name: the file name");
	check.section("playlists");
}

void TestStacks(Checker &check) {
	check(ValidKind(u"photo"_q) && ValidKind(u"video"_q),
		"kind: photo and video");
	check(!ValidKind(u"audio"_q) && !ValidKind(QString()),
		"kind: nothing else");

	// The video editor.
	auto video = VideoFx::Stack();
	video.push_back(VideoFx::MakeEntry(VideoFx::Type::Glitch));
	video.push_back(VideoFx::MakeEntry(VideoFx::Type::Crt));
	video.push_back(VideoFx::MakeEntry(VideoFx::Type::Grain));
	video[1].enabled = false;
	video[2].mix = 0.5;
	const auto videoBytes = VideoFx::Serialize(video);
	const auto videoData = ThroughText(PresetData(u"video"_q, videoBytes));
	check(videoData.value(u"kind"_q).toString() == u"video"_q,
		"video: the data names its kind");
	const auto videoBack = StackFromData(u"video"_q, videoData);
	check(!videoBack.isEmpty(), "video: the data reads back");
	check(VideoFx::Deserialize(videoBack) == VideoFx::Deserialize(videoBytes),
		"video: the stack is the same after the round trip");
	check(SanitizeStack(u"video"_q, videoBack) == videoBack,
		"video: a sane stack stays as it is");
	check(StackFromData(u"photo"_q, videoData).isEmpty(),
		"video: the data is not taken for a photo one");
	const auto videoIds = StackEffects(u"video"_q, videoBytes);
	check(videoIds.size() == 2, "video: only the enabled effects are named");

	auto tooMany = VideoFx::Stack();
	for (auto i = 0; i != kMaxVideoEffects + 9; ++i) {
		tooMany.push_back(VideoFx::MakeEntry(VideoFx::Type::Grain));
	}
	const auto cut = SanitizeStack(u"video"_q, VideoFx::Serialize(tooMany));
	check(int(VideoFx::Deserialize(cut).size()) == kMaxVideoEffects,
		"video: not more effects than the editor takes");
	check(StackEffects(u"video"_q, cut).size() == 1,
		"video: an effect is named once");

	const auto hostile = QByteArray(
		"[{\"fx\":\"no_such_effect\",\"on\":true,\"p\":{}},"
		"{\"fx\":42},\"text\",null,"
		"{\"fx\":\"grain\",\"on\":true,\"mix\":1e300,"
		"\"p\":{\"amount\":-1e300,\"unknown\":[1,2,3]}}]");
	const auto cleaned = VideoFx::Deserialize(
		SanitizeStack(u"video"_q, hostile));
	check(cleaned.size() <= 1, "video: unknown effects are dropped");
	if (!cleaned.empty()) {
		check(cleaned[0].mix >= 0. && cleaned[0].mix <= 1.,
			"video: the mix is clamped");
		check(VideoFx::Sanitized(cleaned[0]) == cleaned[0],
			"video: the values are clamped");
	}
	check(SanitizeStack(u"video"_q, QByteArray("{\"a\":1}")).isEmpty(),
		"video: an object is not a stack");
	check(SanitizeStack(u"video"_q, QByteArray("not json")).isEmpty(),
		"video: garbage is not a stack");
	check(SanitizeStack(u"audio"_q, videoBytes).isEmpty(),
		"stack: an unknown kind gives nothing");

	// The photo editor: whatever effects this build has.
	auto photo = std::vector<Photo::FxInstance>();
	for (const auto descriptor : Photo::AllFx()) {
		if (photo.size() >= 3) {
			break;
		} else if (!(descriptor->flags & Photo::kFxHidden)) {
			photo.push_back(Photo::MakeFx(descriptor->id));
		}
	}
	check(!photo.empty(), "photo: the registry has effects");
	if (!photo.empty()) {
		photo.back().enabled = (photo.size() == 1);
		const auto photoBytes = Photo::SerializeFxStack(photo);
		const auto photoData = ThroughText(
			PresetData(u"photo"_q, photoBytes));
		const auto photoBack = StackFromData(u"photo"_q, photoData);
		check(!photoBack.isEmpty(), "photo: the data reads back");
		check(Photo::DeserializeFxStack(photoBack)
			== Photo::DeserializeFxStack(photoBytes),
			"photo: the stack is the same after the round trip");
		check(SanitizeStack(u"photo"_q, photoBack) == photoBack,
			"photo: a sane stack stays as it is");
		check(StackFromData(u"video"_q, photoData).isEmpty(),
			"photo: the data is not taken for a video one");
		const auto enabled = int(ranges::count_if(
			photo,
			&Photo::FxInstance::enabled));
		check(StackEffects(u"photo"_q, photoBytes).size() <= enabled,
			"photo: only the enabled effects are named");

		auto unknown = photo;
		unknown.push_back(Photo::MakeFx("no.such.effect"));
		const auto without = SanitizeStack(
			u"photo"_q,
			Photo::SerializeFxStack(unknown));
		check(Photo::DeserializeFxStack(without).size() == photo.size(),
			"photo: unknown effects are dropped");

		auto large = std::vector<Photo::FxInstance>();
		for (auto i = 0; i != kMaxPhotoEffects + 7; ++i) {
			large.push_back(photo.front());
		}
		check(int(Photo::DeserializeFxStack(SanitizeStack(
			u"photo"_q,
			Photo::SerializeFxStack(large))).size()) == kMaxPhotoEffects,
			"photo: not more effects than the editor takes");

		auto wrongFormat = photoData;
		wrongFormat.insert(u"format"_q, 999.);
		check(StackFromData(u"photo"_q, wrongFormat).isEmpty(),
			"photo: a newer format is refused");
		auto noStack = photoData;
		noStack.insert(u"stack"_q, u"text"_q);
		check(StackFromData(u"photo"_q, noStack).isEmpty(),
			"photo: a stack must be an array");

		const auto sample = SampleImage(QSize(160, 120));
		check(sample.size() == QSize(160, 120), "preview: the sample");
		const auto rendered = RenderPreview(u"photo"_q, photoBytes, sample);
		check(rendered.size() == sample.size(),
			"preview: a photo stack keeps the size");
		const auto frame = RenderPreview(u"video"_q, videoBytes, sample);
		check(frame.size() == sample.size(),
			"preview: a video stack keeps the size");
	}
	check(SanitizeStack(u"photo"_q, QByteArray("[]")).isEmpty(),
		"photo: an empty stack is nothing");

	// The description with the line of the effects.
	const auto tags = QStringList{ u"lofi.ccd"_q, u"glitch.rgb"_q };
	const auto composed = ComposeDescription(u"My look"_q, tags);
	const auto description = ParseDescription(composed);
	check(description.text == u"My look"_q && description.tags == tags,
		"description: the text and the effects come back");
	check(ParseDescription(u"Just a text"_q).tags.isEmpty()
		&& ParseDescription(u"Just a text"_q).text == u"Just a text"_q,
		"description: a text without effects");
	const auto onlyTags = ParseDescription(ComposeDescription(QString(), tags));
	check(onlyTags.text.isEmpty() && onlyTags.tags == tags,
		"description: only the effects");
	const auto longText = QString(1000, QChar('t'));
	const auto longComposed = ComposeDescription(longText, tags);
	check(longComposed.size() <= kMaxPresetDescription,
		"description: fits the limit of the server");
	check(ParseDescription(longComposed).tags == tags,
		"description: a long text does not push the effects out");
	const auto evil = ParseDescription(
		u"text\nfx: good.one, <b>bad</b>, ../../x, UPPER, ok-2"_q);
	check(evil.tags == QStringList{ u"good.one"_q, u"ok-2"_q },
		"description: only clean ids are taken");
	auto lots = QStringList();
	for (auto i = 0; i != 60; ++i) {
		lots.push_back(u"effect.number%1"_q.arg(i));
	}
	const auto limited = ParseDescription(ComposeDescription(u"x"_q, lots));
	check(!limited.tags.isEmpty() && limited.tags.size() <= kMaxTags,
		"description: not more effects than fit");

	auto owner = QJsonObject();
	owner.insert(u"id"_q, 42.);
	owner.insert(u"name"_q, u"Anna"_q);
	auto preset = QJsonObject();
	preset.insert(u"id"_q, u"Xy7abcdefghijklmnopq22"_q);
	preset.insert(u"kind"_q, u"video"_q);
	preset.insert(u"title"_q, u"Old tape"_q);
	preset.insert(u"description"_q, ComposeDescription(u"VHS"_q, videoIds));
	preset.insert(u"owner"_q, owner);
	preset.insert(u"public"_q, true);
	preset.insert(u"uses"_q, 12.);
	preset.insert(u"used"_q, true);
	preset.insert(u"app_build"_q, 5000000.);
	preset.insert(u"data"_q, videoData);
	const auto parsed = ParsePreset(ThroughText(preset));
	check(parsed.valid() && parsed.full, "preset: a full one is parsed");
	check(parsed.kind == u"video"_q
		&& parsed.title == u"Old tape"_q
		&& parsed.text == u"VHS"_q
		&& parsed.tags == videoIds
		&& parsed.ownerName == u"Anna"_q
		&& parsed.listed
		&& parsed.used
		&& parsed.uses == 12,
		"preset: the fields");
	check(VideoFx::Deserialize(parsed.stack) == VideoFx::Deserialize(videoBytes),
		"preset: the stack");
	preset.remove(u"data"_q);
	const auto brief = ParsePreset(preset);
	check(brief.valid() && !brief.full && brief.stack.isEmpty(),
		"preset: a summary has no stack");
	preset.insert(u"kind"_q, u"audio"_q);
	check(!ParsePreset(preset).valid(), "preset: an unknown kind is refused");
	preset.insert(u"kind"_q, u"video"_q);
	preset.insert(u"id"_q, u"short"_q);
	check(!ParsePreset(preset).valid(), "preset: a bad id is refused");
	check.section("stacks");
}

void TestFiles(Checker &check) {
	auto folder = QTemporaryDir();
	check(folder.isValid(), "files: a temporary folder");
	if (!folder.isValid()) {
		check.section("files");
		return;
	}
	const auto base = folder.path() + '/';
	auto video = VideoFx::Stack();
	video.push_back(VideoFx::MakeEntry(VideoFx::Type::Grain));
	const auto stack = VideoFx::Serialize(video);

	const auto libraryPath = base + u"presets.json"_q;
	auto firstId = uint64(0);
	{
		auto library = Library(libraryPath);
		check(library.list().empty(), "library: starts empty");
		firstId = library.add({
			.kind = u"video"_q,
			.title = u"  Grainy  "_q,
			.stack = stack,
		});
		check(firstId != 0, "library: a preset is added");
		check(!library.add({ .kind = u"video"_q, .title = u"Bad"_q }),
			"library: a preset without effects is refused");
		check(!library.add({ .kind = u"x"_q, .title = u"B"_q, .stack = stack }),
			"library: an unknown kind is refused");
		const auto cloudId = u"Xy7abcdefghijklmnopq22"_q;
		const auto second = library.add({
			.kind = u"video"_q,
			.title = u"From the gallery"_q,
			.stack = stack,
			.cloudId = cloudId,
		});
		const auto third = library.add({
			.kind = u"video"_q,
			.title = u"From the gallery again"_q,
			.stack = stack,
			.cloudId = cloudId,
		});
		check(second && second == third,
			"library: the same shared preset is kept once");
		check(library.list().size() == 2, "library: two presets");
		library.rename(firstId, u"Grain"_q);
		library.setCloudId(firstId, u"Ab1abcdefghijklmnopq22"_q);
	}
	{
		auto library = Library(libraryPath);
		check(library.list().size() == 2, "library: read from the disk");
		const auto first = library.find(firstId);
		check(first
			&& first->title == u"Grain"_q
			&& first->kind == u"video"_q
			&& first->cloudId == u"Ab1abcdefghijklmnopq22"_q
			&& VideoFx::Deserialize(first->stack) == video,
			"library: a preset reads back the same");
		check(library.findByCloudId(u"Xy7abcdefghijklmnopq22"_q) != nullptr,
			"library: found by the shared id");

		auto other = Library(QString());
		other.mergeJson(library.exportJson());
		check(other.list().size() == 2, "library: merged into an empty one");
		other.rename(firstId, u"Renamed elsewhere"_q);
		library.mergeJson(other.exportJson());
		check(library.list().size() == 2
			&& library.find(firstId)
			&& library.find(firstId)->title == u"Renamed elsewhere"_q,
			"library: a known preset is replaced by a merge");
		auto junk = QJsonArray();
		junk.push_back(u"text"_q);
		junk.push_back(QJsonObject());
		library.mergeJson(junk);
		check(library.list().size() == 2, "library: junk is not merged");

		library.remove(firstId);
		check(library.list().size() == 1 && !library.find(firstId),
			"library: a preset is removed");

		// The settings sync: a deletion stays a deletion. The other
		// device still has the preset and sends its list.
		const auto gone = library.exportRemovedJson();
		check(gone.size() == 1
			&& (gone.at(0).toObject().value(u"id"_q).toString()
				== QString::number(firstId)),
			"library: a removed preset is remembered");
		check(other.find(firstId) != nullptr
			&& !library.mergeChanges(
				other.exportJson(),
				other.exportRemovedJson()),
			"library: a list with the removed preset would change nothing");
		library.mergeJson(other.exportJson(), other.exportRemovedJson());
		check(library.list().size() == 1 && !library.find(firstId),
			"library: a removed preset does not come back");
		check(other.mergeChanges(library.exportJson(), gone)
			&& other.countRemovals(gone) == 1
			&& other.find(firstId) != nullptr,
			"library: asking what a merge would change changes nothing");
		other.mergeJson(library.exportJson(), gone);
		check(other.list().size() == 1 && !other.find(firstId),
			"library: a removal reaches the other device");
		check(other.countRemovals(gone) == 0,
			"library: nothing more to remove after that");
		check(other.exportRemovedJson() == gone,
			"library: and is passed on by it");
		check(!other.mergeChanges(library.exportJson(), gone),
			"library: the second time nothing changes");
		check(other.add({
			.kind = u"video"_q,
			.title = u"Again"_q,
			.stack = stack,
		}) != 0, "library: a new preset is still saved");

		// Too old to matter, broken, and from a clock that runs ahead.
		const auto lastId = library.list().empty()
			? uint64(0)
			: library.list().front().id;
		const auto now = int64(base::unixtime::now());
		auto stale = QJsonArray();
		stale.push_back(QJsonObject{
			{ u"id"_q, QString::number(lastId) },
			{ u"time"_q, double(now - int64(400) * 86400) },
		});
		stale.push_back(QJsonObject{
			{ u"id"_q, u"0"_q },
			{ u"time"_q, double(now) },
		});
		stale.push_back(QJsonObject{ { u"id"_q, QString::number(lastId) } });
		stale.push_back(u"text"_q);
		library.mergeJson(QJsonArray(), stale);
		check(lastId
			&& library.find(lastId) != nullptr
			&& library.exportRemovedJson().size() == 1,
			"library: an old or a broken removal removes nothing");
		auto ahead = QJsonArray();
		ahead.push_back(QJsonObject{
			{ u"id"_q, QString::number(lastId) },
			{ u"time"_q, double(now + int64(4000) * 86400) },
		});
		library.mergeJson(QJsonArray(), ahead);
		const auto noted = library.exportRemovedJson();
		check(library.list().empty() && noted.size() == 2,
			"library: a removal from a clock that runs ahead is taken");
		auto latest = int64(0);
		for (const auto &value : noted) {
			latest = std::max(
				latest,
				Cloud::JsonInt(value.toObject().value(u"time"_q)));
		}
		check(latest > 0 && latest <= int64(base::unixtime::now()),
			"library: but not with a time in the future");
	}
	{
		auto library = Library(libraryPath);
		check(library.list().empty()
			&& library.exportRemovedJson().size() == 2
			&& library.exportJson().isEmpty(),
			"library: the removals are on the disk");
		auto other = Library(QString());
		const auto id = other.add({
			.kind = u"video"_q,
			.title = u"Mine"_q,
			.stack = stack,
		});
		check(id && !library.find(id), "library: another device, a preset");
		library.mergeJson(other.exportJson(), other.exportRemovedJson());
		check(library.find(id) != nullptr,
			"library: a preset that was never removed is still added");
	}
	{
		auto file = QFile(libraryPath);
		if (file.open(QIODevice::WriteOnly)) {
			file.write("{ this is not json");
			file.close();
		}
		auto library = Library(libraryPath);
		check(library.list().empty(), "library: a broken file is empty");
	}

	const auto storePath = base + u"shared.json"_q;
	const auto playlist = ParsePlaylist(SamplePlaylistJson());
	{
		auto store = Store(storePath);
		check(store.publishedId(7).isEmpty(), "store: nothing published");
		store.setPublished(7, playlist.id);
		store.setPublished(8, u"bad id"_q);
		store.setKnownMedia(1001, QString(64, QChar('c')));
		store.setKnownMedia(1002, u"bad"_q);
		store.save(playlist);
		check(store.saved().size() == 1, "store: a playlist is kept");
		auto newer = playlist;
		newer.title = u"Renamed"_q;
		newer.rev = playlist.rev + 1;
		store.save(newer);
		check(store.saved().size() == 1
			&& store.saved().front().title == u"Renamed"_q,
			"store: a kept playlist is refreshed");
	}
	{
		auto store = Store(storePath);
		check(store.publishedId(7) == playlist.id,
			"store: the published id reads back");
		check(store.publishedId(8).isEmpty(), "store: a bad id is not kept");
		check(store.knownMedia(1001) == QString(64, QChar('c')),
			"store: a known file reads back");
		check(store.knownMedia(1002).isEmpty(), "store: a bad hash is not kept");
		const auto saved = store.findSaved(playlist.id);
		check(saved
			&& saved->title == u"Renamed"_q
			&& saved->tracks.size() == playlist.tracks.size(),
			"store: the kept playlist reads back");
		store.forgetPublished(playlist.id);
		check(store.publishedId(7).isEmpty(), "store: the id is forgotten");
		store.unsave(playlist.id);
		check(store.saved().empty(), "store: the playlist is dropped");
	}
	{
		auto store = Store(storePath);
		check(store.saved().empty() && store.publishedId(7).isEmpty(),
			"store: the changes are on the disk");
	}
	check.section("files");
}

void TestRules(Checker &check) {
	// «Набор применён» only when the editor has taken the stack.
	const auto editor = std::make_shared<QByteArray>("[]");
	const auto locked = std::make_shared<bool>(false);
	const auto calls = std::make_shared<int>(0);
	const auto host = PresetHost{
		.kind = u"video"_q,
		.current = [=] { return *editor; },
		.apply = [=](const QByteArray &stack) {
			++*calls;
			if (!*locked) {
				*editor = stack;
			}
		},
	};
	const auto apply = CheckedApply(host);
	check(apply != nullptr, "apply: a host that can apply");
	if (apply) {
		check(apply(QByteArray("[1]")) && *editor == QByteArray("[1]"),
			"apply: a stack the editor takes is applied");
		check(!apply(QByteArray("[1]")),
			"apply: the stack that is there already changes nothing");
		*locked = true;
		check(!apply(QByteArray("[2]")) && *editor == QByteArray("[1]"),
			"apply: a stack the editor refuses is not applied");
		check(*calls == 3, "apply: the editor is asked every time");
		*locked = false;
	}
	auto blind = host;
	blind.current = nullptr;
	const auto trusted = CheckedApply(blind);
	check(trusted && trusted(QByteArray("[3]")),
		"apply: a host without a stack to compare is trusted");
	auto viewer = host;
	viewer.apply = nullptr;
	check(!CheckedApply(viewer), "apply: a host that can't apply");

	// «Добавить к себе»: what is tried again and when.
	check(KeepRetryDelay(0) >= crl::time(60'000)
		&& KeepRetryDelay(-3) == KeepRetryDelay(0),
		"keep: not sooner than a minute after a failure");
	check(KeepRetryDelay(1) > KeepRetryDelay(0)
		&& KeepRetryDelay(2) > KeepRetryDelay(1)
		&& KeepRetryDelay(3) > KeepRetryDelay(2),
		"keep: the pause grows");
	check(KeepRetryDelay(3) == crl::time(3'600'000)
		&& KeepRetryDelay(1000) == KeepRetryDelay(3),
		"keep: and stays at an hour");
	using Type = Cloud::Error::Type;
	check(KeepGoneError({ .type = Type::Http, .status = 404 })
		&& KeepGoneError({ .type = Type::Http, .status = 403 })
		&& KeepGoneError({ .type = Type::File }),
		"keep: a file the server does not give is not asked for again");
	check(!KeepGoneError({ .type = Type::Network })
		&& !KeepGoneError({ .type = Type::Timeout })
		&& !KeepGoneError({ .type = Type::NotConnected })
		&& !KeepGoneError({ .type = Type::Http, .status = 429 })
		&& !KeepGoneError({ .type = Type::Http, .status = 503 }),
		"keep: without connection the file is tried again later");

	// The upload: one refused track does not stop the rest.
	const auto refused = Cloud::Error{
		.type = Type::Http,
		.status = 400,
		.code = u"bad_request"_q,
		.details = QJsonObject{ { u"field"_q, u"tracks[0].title"_q } },
	};
	check(TrackRefused(refused), "upload: a refused track is skipped");
	auto title = refused;
	title.details = QJsonObject{ { u"field"_q, u"title"_q } };
	check(!TrackRefused(title),
		"upload: a refused title of the playlist stops the upload");
	check(!TrackRefused({
		.type = Type::Http,
		.status = 422,
		.code = u"limit_reached"_q,
	}) && !TrackRefused({ .type = Type::Network }),
		"upload: a limit or a lost connection stops the upload");
	const auto slow = Cloud::Error{
		.type = Type::Http,
		.status = 429,
		.retryAfter = 2'000,
	};
	check(AddAgainDelay(slow, 0) == crl::time(2'000),
		"upload: asked to slow down, the track waits as long as asked");
	check(AddAgainDelay({ .type = Type::Http, .status = 429 }, 0)
		>= crl::time(1'000),
		"upload: at least a second without a number from the server");
	check(AddAgainDelay(slow, 4) > AddAgainDelay(slow, 0)
		&& AddAgainDelay(slow, 5) == 0,
		"upload: a few times, not again and again");
	check(AddAgainDelay({
		.type = Type::Http,
		.status = 429,
		.retryAfter = 600'000,
	}, 0) == 0, "upload: a long wait stops the upload instead");
	check(AddAgainDelay({ .type = Type::Http, .status = 422 }, 0) == 0
		&& AddAgainDelay({ .type = Type::Network }, 0) == 0,
		"upload: other failures are not waited out");
	check.section("rules");
}

} // namespace

Playlist ParsePlaylist(const QJsonObject &object) {
	auto result = Playlist();
	const auto id = object.value(u"id"_q).toString();
	if (!Cloud::ValidShareId(id)) {
		return result;
	}
	result.id = id;
	result.link = Cloud::MakeLink(Cloud::LinkKind::Playlist, id);
	const auto owner = object.value(u"owner"_q).toObject();
	result.ownerId = Cloud::JsonUserId(owner.value(u"id"_q));
	result.ownerName = Cloud::JsonText(owner.value(u"name"_q), kMaxOwnerName);
	result.title = Cloud::JsonText(
		object.value(u"title"_q),
		kMaxPlaylistTitle);
	result.description = Cloud::JsonText(
		object.value(u"description"_q),
		kMaxPlaylistDescription,
		false);
	result.collab = object.value(u"collab"_q).toBool();
	result.listed = object.value(u"public"_q).toBool();
	result.canEdit = object.value(u"can_edit"_q).toBool();
	result.canAdd = object.value(u"can_add"_q).toBool();
	result.rev = ClampInt(
		Cloud::JsonInt(object.value(u"rev"_q)),
		0,
		std::numeric_limits<int>::max());
	result.followers = ClampInt(
		Cloud::JsonInt(object.value(u"followers"_q)),
		0,
		std::numeric_limits<int>::max());
	result.totalBytes = std::clamp(
		Cloud::JsonInt(object.value(u"total_bytes"_q)),
		int64(0),
		kMaxMediaSize);
	result.totalDuration = std::clamp(
		Cloud::JsonInt(object.value(u"total_duration_ms"_q)),
		int64(0),
		kMaxTrackDuration * kMaxPlaylistTracks);
	const auto tracks = object.value(u"tracks"_q);
	if (tracks.isArray()) {
		result.full = true;
		for (const auto &value : tracks.toArray()) {
			if (int(result.tracks.size()) >= kMaxPlaylistTracks) {
				break;
			} else if (auto track = ParseTrack(value.toObject())) {
				result.tracks.push_back(std::move(*track));
			}
		}
		result.trackCount = int(result.tracks.size());
	} else {
		result.trackCount = ClampInt(
			Cloud::JsonInt(object.value(u"track_count"_q)),
			0,
			kMaxPlaylistTracks);
	}
	return result;
}

QJsonObject PlaylistToJson(const Playlist &playlist) {
	auto owner = QJsonObject();
	owner.insert(u"id"_q, double(playlist.ownerId));
	owner.insert(u"name"_q, playlist.ownerName);
	auto result = QJsonObject();
	result.insert(u"id"_q, playlist.id);
	result.insert(u"owner"_q, owner);
	result.insert(u"title"_q, playlist.title);
	result.insert(u"description"_q, playlist.description);
	result.insert(u"collab"_q, playlist.collab);
	result.insert(u"public"_q, playlist.listed);
	result.insert(u"can_edit"_q, playlist.canEdit);
	result.insert(u"can_add"_q, playlist.canAdd);
	result.insert(u"rev"_q, playlist.rev);
	result.insert(u"followers"_q, playlist.followers);
	result.insert(u"total_bytes"_q, double(playlist.totalBytes));
	result.insert(u"total_duration_ms"_q, double(playlist.totalDuration));
	if (playlist.full) {
		auto tracks = QJsonArray();
		for (const auto &track : playlist.tracks) {
			tracks.push_back(TrackToJson(track));
		}
		result.insert(u"tracks"_q, tracks);
	} else {
		result.insert(u"track_count"_q, playlist.trackCount);
	}
	return result;
}

QJsonObject TrackInputToJson(const TrackInput &input) {
	const auto clean = [](const QString &text) {
		return text.simplified().left(kMaxTrackText);
	};
	auto fileName = QFileInfo(
		QString(input.fileName).replace(QChar('\\'), QChar('/'))
	).fileName().simplified().left(kMaxTrackText);
	auto title = clean(input.title);
	if (title.isEmpty()) {
		title = QFileInfo(fileName).completeBaseName().left(kMaxTrackText);
	}
	if (title.isEmpty()) {
		title = u"Track"_q;
	}
	auto result = QJsonObject();
	result.insert(u"media"_q, input.media);
	result.insert(u"title"_q, title);
	result.insert(u"performer"_q, clean(input.performer));
	result.insert(
		u"duration_ms"_q,
		double(std::clamp(input.duration, int64(1), kMaxTrackDuration)));
	result.insert(u"file_name"_q, fileName);
	return result;
}

QString TrackName(
		const QString &title,
		const QString &performer,
		const QString &fileName) {
	const auto cleanTitle = title.trimmed();
	const auto cleanPerformer = performer.trimmed();
	return cleanTitle.isEmpty()
		? (cleanPerformer.isEmpty() ? fileName.trimmed() : cleanPerformer)
		: cleanPerformer.isEmpty()
		? cleanTitle
		: (cleanPerformer + QString::fromUtf8(" \xE2\x80\x94 ") + cleanTitle);
}

bool ValidKind(const QString &kind) {
	return (kind == u"photo"_q) || (kind == u"video"_q);
}

QByteArray SanitizeStack(const QString &kind, const QByteArray &stack) {
	if (stack.isEmpty() || stack.size() > kMaxPresetData) {
		return QByteArray();
	} else if (kind == u"photo"_q) {
		const auto sane = SanePhotoStack(stack);
		return sane.empty() ? QByteArray() : Photo::SerializeFxStack(sane);
	} else if (kind == u"video"_q) {
		const auto sane = SaneVideoStack(stack);
		return sane.empty() ? QByteArray() : VideoFx::Serialize(sane);
	}
	return QByteArray();
}

QStringList StackEffects(const QString &kind, const QByteArray &stack) {
	auto result = QStringList();
	const auto add = [&](const QString &id) {
		if (!id.isEmpty() && !result.contains(id)) {
			result.push_back(id);
		}
	};
	if (kind == u"photo"_q) {
		for (const auto &instance : SanePhotoStack(stack)) {
			if (instance.enabled) {
				add(QString::fromLatin1(instance.id));
			}
		}
	} else if (kind == u"video"_q) {
		for (const auto &entry : SaneVideoStack(stack)) {
			if (entry.enabled && entry.mix > 0.) {
				add(QString::fromLatin1(VideoFx::EffectInfo(entry.type).id));
			}
		}
	}
	return result;
}

QString EffectName(const QString &kind, const QString &id) {
	if (kind == u"photo"_q) {
		const auto descriptor = Photo::FindFx(id.toLatin1());
		return descriptor ? descriptor->name.now() : QString();
	} else if (kind == u"video"_q) {
		const auto info = VideoFx::FindEffect(id);
		return info ? info->name(tr::now) : QString();
	}
	return QString();
}

QString EffectNames(const QString &kind, const QStringList &ids) {
	auto names = QStringList();
	for (const auto &id : ids) {
		const auto name = EffectName(kind, id);
		if (!name.isEmpty() && !names.contains(name)) {
			names.push_back(name);
		}
	}
	return names.join(u", "_q);
}

QJsonObject PresetData(const QString &kind, const QByteArray &stack) {
	auto result = QJsonObject();
	result.insert(u"format"_q, kPresetFormat);
	result.insert(u"kind"_q, kind);
	result.insert(u"stack"_q, QJsonDocument::fromJson(stack).array());
	return result;
}

QByteArray StackFromData(const QString &kind, const QJsonObject &data) {
	const auto stack = data.value(u"stack"_q);
	if (!ValidKind(kind)
		|| data.value(u"kind"_q).toString() != kind
		|| data.value(u"format"_q).toInt() != kPresetFormat
		|| !stack.isArray()) {
		return QByteArray();
	}
	return SanitizeStack(
		kind,
		QJsonDocument(stack.toArray()).toJson(QJsonDocument::Compact));
}

QString ComposeDescription(const QString &text, const QStringList &tags) {
	auto line = QString();
	auto count = 0;
	for (const auto &tag : tags) {
		if (!ValidTag(tag)) {
			continue;
		}
		const auto added = line.isEmpty()
			? (TagsPrefix() + tag)
			: (u", "_q + tag);
		if (count >= kMaxTags || line.size() + added.size() > kMaxTagsLine) {
			break;
		}
		line += added;
		++count;
	}
	const auto room = kMaxPresetDescription
		- (line.isEmpty() ? 0 : int(line.size()) + 1);
	auto clean = QString(text).replace(u"\r\n"_q, u"\n"_q).replace(
		QChar('\r'),
		QChar('\n')).trimmed().left(std::max(room, 0)).trimmed();
	// A line of the user that looks like the line of the effects would
	// be read as one.
	while (clean.section(QChar('\n'), -1).startsWith(TagsPrefix())) {
		const auto index = clean.lastIndexOf(QChar('\n'));
		clean = (index < 0) ? QString() : clean.left(index).trimmed();
	}
	return line.isEmpty()
		? clean
		: clean.isEmpty()
		? line
		: (clean + QChar('\n') + line);
}

Description ParseDescription(const QString &description) {
	auto result = Description();
	const auto index = description.lastIndexOf(QChar('\n'));
	const auto last = (index < 0) ? description : description.mid(index + 1);
	if (!last.startsWith(TagsPrefix())) {
		result.text = description.trimmed();
		return result;
	}
	result.text = (index < 0) ? QString() : description.left(index).trimmed();
	const auto parts = last.mid(TagsPrefix().size()).split(QChar(','));
	for (const auto &part : parts) {
		const auto tag = part.trimmed();
		if (ValidTag(tag)
			&& !result.tags.contains(tag)
			&& result.tags.size() < kMaxTags) {
			result.tags.push_back(tag);
		}
	}
	return result;
}

Preset ParsePreset(const QJsonObject &object) {
	auto result = Preset();
	const auto id = object.value(u"id"_q).toString();
	const auto kind = object.value(u"kind"_q).toString();
	if (!Cloud::ValidShareId(id) || !ValidKind(kind)) {
		return result;
	}
	result.id = id;
	result.kind = kind;
	result.link = Cloud::MakeLink(Cloud::LinkKind::Preset, id);
	result.title = Cloud::JsonText(object.value(u"title"_q), kMaxPresetTitle);
	const auto description = ParseDescription(Cloud::JsonText(
		object.value(u"description"_q),
		kMaxPresetDescription,
		false));
	result.text = description.text;
	result.tags = description.tags;
	const auto owner = object.value(u"owner"_q).toObject();
	result.ownerId = Cloud::JsonUserId(owner.value(u"id"_q));
	result.ownerName = Cloud::JsonText(owner.value(u"name"_q), kMaxOwnerName);
	result.listed = object.value(u"public"_q).toBool();
	result.hidden = object.value(u"hidden"_q).toBool();
	result.canEdit = object.value(u"can_edit"_q).toBool();
	result.used = object.value(u"used"_q).toBool();
	result.uses = ClampInt(
		Cloud::JsonInt(object.value(u"uses"_q)),
		0,
		std::numeric_limits<int>::max());
	result.appBuild = ClampInt(
		Cloud::JsonInt(object.value(u"app_build"_q)),
		0,
		std::numeric_limits<int>::max());
	result.created = std::max(
		Cloud::JsonInt(object.value(u"created_at"_q)),
		int64(0));
	const auto data = object.value(u"data"_q);
	if (data.isObject()) {
		result.full = true;
		result.stack = StackFromData(kind, data.toObject());
		if (!result.stack.isEmpty()) {
			// What the stack really has, whatever the description says.
			result.tags = StackEffects(kind, result.stack);
		}
	}
	return result;
}

QImage SampleImage(QSize size) {
	const auto bounded = QSize(
		std::clamp(size.width(), 16, 2048),
		std::clamp(size.height(), 16, 2048));
	auto result = QImage(bounded, QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::black);
	{
		auto p = QPainter(&result);
		p.setRenderHint(QPainter::Antialiasing);
		PaintSample(p, bounded);
	}
	return result;
}

QImage RenderPreview(
		const QString &kind,
		const QByteArray &stack,
		QImage sample) {
	if (sample.isNull() || stack.isEmpty()) {
		return sample;
	} else if (kind == u"photo"_q) {
		return RenderPhoto(stack, std::move(sample));
	} else if (kind == u"video"_q) {
		const auto size = sample.size();
		auto result = VideoFx::Apply(
			SaneVideoStack(stack),
			std::move(sample),
			0);
		return (result.size() == size) ? result : SampleImage(size);
	}
	return sample;
}

Library::Library(QString path)
: _path(std::move(path)) {
}

void Library::ensureLoaded() {
	if (std::exchange(_loaded, true)) {
		return;
	}
	const auto object = ReadJsonFile(_path);
	_list.clear();
	_removed.clear();
	merge(
		object.value(u"presets"_q).toArray(),
		object.value(u"removed"_q).toArray());
}

void Library::save() {
	auto object = QJsonObject();
	object.insert(u"v"_q, kFileVersion);
	object.insert(u"presets"_q, exportJson());
	const auto removed = exportRemovedJson();
	if (!removed.isEmpty()) {
		object.insert(u"removed"_q, removed);
	}
	WriteJsonFile(_path, object);
}

const std::vector<LocalPreset> &Library::list() {
	ensureLoaded();
	return _list;
}

const LocalPreset *Library::find(uint64 id) {
	ensureLoaded();
	const auto i = ranges::find(_list, id, &LocalPreset::id);
	return (i != end(_list)) ? &*i : nullptr;
}

const LocalPreset *Library::findByCloudId(const QString &cloudId) {
	ensureLoaded();
	if (cloudId.isEmpty()) {
		return nullptr;
	}
	const auto i = ranges::find(_list, cloudId, &LocalPreset::cloudId);
	return (i != end(_list)) ? &*i : nullptr;
}

uint64 Library::add(LocalPreset preset) {
	ensureLoaded();
	preset.title = preset.title.simplified().left(kMaxPresetTitle);
	preset.stack = SanitizeStack(preset.kind, preset.stack);
	if (!Cloud::ValidShareId(preset.cloudId)) {
		preset.cloudId = QString();
	}
	if (preset.title.isEmpty() || preset.stack.isEmpty()) {
		return 0;
	}
	if (!preset.cloudId.isEmpty()) {
		const auto i = ranges::find(
			_list,
			preset.cloudId,
			&LocalPreset::cloudId);
		if (i != end(_list)) {
			i->kind = preset.kind;
			i->title = preset.title;
			i->stack = preset.stack;
			save();
			_changes.fire({});
			return i->id;
		}
	}
	if (int(_list.size()) >= kMaxLocalPresets) {
		return 0;
	}
	do {
		preset.id = base::RandomValue<uint64>() >> 12;
	} while (!preset.id || find(preset.id) || wasRemoved(preset.id));
	if (preset.created <= 0) {
		preset.created = base::unixtime::now();
	}
	const auto id = preset.id;
	_list.insert(begin(_list), std::move(preset));
	save();
	_changes.fire({});
	return id;
}

void Library::rename(uint64 id, const QString &title) {
	ensureLoaded();
	const auto clean = title.simplified().left(kMaxPresetTitle);
	const auto i = ranges::find(_list, id, &LocalPreset::id);
	if (i == end(_list) || clean.isEmpty() || i->title == clean) {
		return;
	}
	i->title = clean;
	save();
	_changes.fire({});
}

void Library::setCloudId(uint64 id, const QString &cloudId) {
	ensureLoaded();
	const auto i = ranges::find(_list, id, &LocalPreset::id);
	const auto value = Cloud::ValidShareId(cloudId) ? cloudId : QString();
	if (i == end(_list) || i->cloudId == value) {
		return;
	}
	i->cloudId = value;
	save();
	_changes.fire({});
}

void Library::remove(uint64 id) {
	ensureLoaded();
	const auto i = ranges::find(_list, id, &LocalPreset::id);
	if (i == end(_list)) {
		return;
	}
	_list.erase(i);
	// For the settings sync: the other devices delete it too instead of
	// sending it back.
	noteRemoved(id, int64(base::unixtime::now()));
	save();
	_changes.fire({});
}

bool Library::wasRemoved(uint64 id) const {
	return ranges::contains(_removed, id, &Removed::id);
}

// True if something new is remembered.
bool Library::noteRemoved(uint64 id, int64 time) {
	const auto i = ranges::find(_removed, id, &Removed::id);
	if (i != end(_removed)) {
		if (i->time >= time) {
			return false;
		}
		i->time = time;
		return true;
	} else if (int(_removed.size()) < kMaxRemovedPresets) {
		_removed.push_back({ .id = id, .time = time });
		return true;
	}
	// Full: the oldest one gives its place to a newer one.
	const auto oldest = ranges::min_element(
		_removed,
		ranges::less(),
		&Removed::time);
	if (oldest->time >= time) {
		return false;
	}
	*oldest = Removed{ .id = id, .time = time };
	return true;
}

rpl::producer<> Library::changes() const {
	return _changes.events();
}

QJsonArray Library::exportJson() {
	ensureLoaded();
	auto result = QJsonArray();
	for (const auto &preset : _list) {
		auto entry = QJsonObject();
		entry.insert(u"id"_q, QString::number(preset.id));
		entry.insert(u"kind"_q, preset.kind);
		entry.insert(u"title"_q, preset.title);
		entry.insert(
			u"stack"_q,
			QJsonDocument::fromJson(preset.stack).array());
		if (!preset.cloudId.isEmpty()) {
			entry.insert(u"cloud"_q, preset.cloudId);
		}
		entry.insert(u"time"_q, double(preset.created));
		result.push_back(entry);
	}
	return result;
}

QJsonArray Library::exportRemovedJson() {
	ensureLoaded();
	const auto now = int64(base::unixtime::now());
	auto result = QJsonArray();
	for (const auto &entry : _removed) {
		if (entry.time + kRemovedLife <= now) {
			continue;
		}
		auto object = QJsonObject();
		object.insert(u"id"_q, QString::number(entry.id));
		object.insert(u"time"_q, double(entry.time));
		result.push_back(object);
	}
	return result;
}

void Library::mergeJson(const QJsonArray &list, const QJsonArray &removed) {
	ensureLoaded();
	const auto result = merge(list, removed);
	if (result.list || result.removed) {
		save();
	}
	if (result.list) {
		_changes.fire({});
	}
}

bool Library::mergeChanges(
		const QJsonArray &list,
		const QJsonArray &removed) {
	ensureLoaded();
	auto keptList = _list;
	auto keptRemoved = _removed;
	const auto result = merge(list, removed);
	_list = std::move(keptList);
	_removed = std::move(keptRemoved);
	return result.list;
}

int Library::countRemovals(const QJsonArray &removed) {
	ensureLoaded();
	auto keptList = _list;
	auto keptRemoved = _removed;
	merge(QJsonArray(), removed);
	const auto result = int(keptList.size()) - int(_list.size());
	_list = std::move(keptList);
	_removed = std::move(keptRemoved);
	return std::max(result, 0);
}

Library::Merged Library::merge(
		const QJsonArray &list,
		const QJsonArray &removed) {
	auto result = Merged();
	const auto now = int64(base::unixtime::now());
	_removed.erase(
		ranges::remove_if(_removed, [&](const Removed &entry) {
			return (entry.time + kRemovedLife <= now);
		}),
		end(_removed));

	// What was deleted goes first: a deletion wins over a preset with
	// the same id in the list.
	auto count = 0;
	for (const auto &value : removed) {
		if (++count > kMaxRemovedPresets) {
			break;
		}
		const auto entry = value.toObject();
		const auto id = entry.value(u"id"_q).toString().toULongLong();
		// A clock that runs ahead does not make a deletion live longer.
		const auto time = std::min(
			Cloud::JsonInt(entry.value(u"time"_q)),
			now);
		if (!id || time <= 0 || time + kRemovedLife <= now) {
			continue;
		}
		result.removed = noteRemoved(id, time) || result.removed;
		const auto i = ranges::find(_list, id, &LocalPreset::id);
		if (i != end(_list)) {
			_list.erase(i);
			result.list = true;
		}
	}
	result.list = mergeList(list) || result.list;
	return result;
}

bool Library::mergeList(const QJsonArray &list) {
	auto changed = false;
	for (const auto &value : list) {
		const auto entry = value.toObject();
		auto preset = LocalPreset();
		preset.id = entry.value(u"id"_q).toString().toULongLong();
		preset.kind = entry.value(u"kind"_q).toString();
		preset.title = Cloud::JsonText(
			entry.value(u"title"_q),
			kMaxPresetTitle);
		const auto stack = entry.value(u"stack"_q);
		if (!preset.id
			|| wasRemoved(preset.id)
			|| !ValidKind(preset.kind)
			|| preset.title.isEmpty()
			|| !stack.isArray()) {
			continue;
		}
		preset.stack = SanitizeStack(
			preset.kind,
			QJsonDocument(stack.toArray()).toJson(QJsonDocument::Compact));
		if (preset.stack.isEmpty()) {
			continue;
		}
		const auto cloudId = entry.value(u"cloud"_q).toString();
		preset.cloudId = Cloud::ValidShareId(cloudId) ? cloudId : QString();
		preset.created = std::max(
			Cloud::JsonInt(entry.value(u"time"_q)),
			int64(0));
		const auto i = ranges::find(_list, preset.id, &LocalPreset::id);
		if (i != end(_list)) {
			if (i->kind != preset.kind
				|| i->title != preset.title
				|| i->stack != preset.stack
				|| i->cloudId != preset.cloudId) {
				*i = std::move(preset);
				changed = true;
			}
		} else if (int(_list.size()) < kMaxLocalPresets) {
			_list.push_back(std::move(preset));
			changed = true;
		}
	}
	return changed;
}

Library &PresetLibrary() {
	static const auto result = new Library(
		cWorkingDir() + u"tdata/oblivion/presets.json"_q);
	return *result;
}

Fn<bool(const QByteArray &stack)> CheckedApply(const PresetHost &host) {
	const auto apply = host.apply;
	const auto current = host.current;
	if (!apply) {
		return nullptr;
	}
	return [=](const QByteArray &stack) {
		if (!current) {
			// Nothing to compare with: taken on trust.
			apply(stack);
			return true;
		}
		const auto before = current();
		apply(stack);
		return (current() != before);
	};
}

Store::Store(QString path)
: _path(std::move(path)) {
}

void Store::ensureLoaded() {
	if (std::exchange(_loaded, true)) {
		return;
	}
	const auto object = ReadJsonFile(_path);
	const auto published = object.value(u"published"_q).toObject();
	for (auto i = published.begin(); i != published.end(); ++i) {
		const auto localId = i.key().toULongLong();
		const auto cloudId = i.value().toString();
		if (localId
			&& Cloud::ValidShareId(cloudId)
			&& int(_published.size()) < kMaxPublished) {
			_published.emplace(localId, cloudId);
		}
	}
	const auto media = object.value(u"media"_q).toObject();
	for (auto i = media.begin(); i != media.end(); ++i) {
		const auto document = i.key().toULongLong();
		const auto sha = i.value().toString();
		if (document
			&& Cloud::ValidMediaId(sha)
			&& int(_media.size()) < kMaxKnownMedia) {
			_media.emplace(document, sha);
		}
	}
	const auto saved = object.value(u"saved"_q).toArray();
	for (const auto &value : saved) {
		auto playlist = ParsePlaylist(value.toObject());
		if (playlist.valid()
			&& playlist.full
			&& int(_saved.size()) < kMaxSaved
			&& !ranges::contains(_saved, playlist.id, &Playlist::id)) {
			_saved.push_back(std::move(playlist));
		}
	}
}

void Store::write() {
	auto published = QJsonObject();
	for (const auto &[localId, cloudId] : _published) {
		published.insert(QString::number(localId), cloudId);
	}
	auto media = QJsonObject();
	for (const auto &[document, sha] : _media) {
		media.insert(QString::number(document), sha);
	}
	auto saved = QJsonArray();
	for (const auto &playlist : _saved) {
		saved.push_back(PlaylistToJson(playlist));
	}
	auto object = QJsonObject();
	object.insert(u"v"_q, kFileVersion);
	object.insert(u"published"_q, published);
	object.insert(u"media"_q, media);
	object.insert(u"saved"_q, saved);
	WriteJsonFile(_path, object);
}

QString Store::publishedId(uint64 localId) {
	ensureLoaded();
	const auto i = _published.find(localId);
	return (i != end(_published)) ? i->second : QString();
}

void Store::setPublished(uint64 localId, const QString &cloudId) {
	ensureLoaded();
	if (!localId || !Cloud::ValidShareId(cloudId)) {
		return;
	}
	const auto i = _published.find(localId);
	if (i != end(_published)) {
		if (i->second == cloudId) {
			return;
		}
		i->second = cloudId;
	} else if (int(_published.size()) >= kMaxPublished) {
		return;
	} else {
		_published.emplace(localId, cloudId);
	}
	write();
	_changes.fire({});
}

void Store::forgetPublished(const QString &cloudId) {
	ensureLoaded();
	auto changed = false;
	for (auto i = begin(_published); i != end(_published);) {
		if (i->second == cloudId) {
			i = _published.erase(i);
			changed = true;
		} else {
			++i;
		}
	}
	if (changed) {
		write();
		_changes.fire({});
	}
}

QString Store::knownMedia(uint64 document) {
	ensureLoaded();
	const auto i = _media.find(document);
	return (i != end(_media)) ? i->second : QString();
}

void Store::setKnownMedia(uint64 document, const QString &media) {
	ensureLoaded();
	if (!document) {
		return;
	}
	const auto i = _media.find(document);
	if (!Cloud::ValidMediaId(media)) {
		if (i == end(_media)) {
			return;
		}
		_media.erase(i);
	} else if (i != end(_media)) {
		if (i->second == media) {
			return;
		}
		i->second = media;
	} else {
		if (int(_media.size()) >= kMaxKnownMedia) {
			_media.erase(begin(_media));
		}
		_media.emplace(document, media);
	}
	write();
}

const std::vector<Playlist> &Store::saved() {
	ensureLoaded();
	return _saved;
}

const Playlist *Store::findSaved(const QString &id) {
	ensureLoaded();
	const auto i = ranges::find(_saved, id, &Playlist::id);
	return (i != end(_saved)) ? &*i : nullptr;
}

void Store::save(const Playlist &playlist) {
	ensureLoaded();
	if (!playlist.valid() || !playlist.full) {
		return;
	}
	const auto i = ranges::find(_saved, playlist.id, &Playlist::id);
	if (i != end(_saved)) {
		*i = playlist;
	} else if (int(_saved.size()) >= kMaxSaved) {
		return;
	} else {
		_saved.insert(begin(_saved), playlist);
	}
	write();
	_changes.fire({});
}

void Store::unsave(const QString &id) {
	ensureLoaded();
	const auto i = ranges::find(_saved, id, &Playlist::id);
	if (i == end(_saved)) {
		return;
	}
	_saved.erase(i);
	write();
	_changes.fire({});
}

rpl::producer<> Store::changes() const {
	return _changes.events();
}

Uploader::Uploader(
	not_null<Main::Session*> session,
	not_null<Store*> store,
	UploadRequest &&request)
: _session(session)
, _store(store)
, _account(base::make_weak(&Cloud::For(session)))
, _sender(&Cloud::For(session))
, _request(std::move(request))
, _watchdog([=] { skip(); })
, _addAgain([=] { add(_addMedia, _addKnown); }) {
	_status.count = int(_request.sources.size());
	_status.title = _request.title;
	_status.playlistId = _request.playlistId;
}

Uploader::~Uploader() {
	// The session may be going away: the document is not touched here,
	// cancel() does it while everything is alive.
	if (const auto account = _account.get()) {
		if (_transfer) {
			account->cancelTransfer(_transfer);
		}
	}
	if (!_tempPath.isEmpty()) {
		QFile::remove(_tempPath);
	}
}

rpl::producer<UploadStatus> Uploader::statusValue() const {
	return _changes.events_starting_with_copy(_status);
}

void Uploader::changed() {
	_changes.fire_copy(_status);
}

void Uploader::start() {
	if (_status.stage != UploadStatus::Stage::Idle) {
		return;
	}
	_status.stage = UploadStatus::Stage::Fetching;
	changed();
	schedule();
}

void Uploader::retry() {
	if (_status.stage != UploadStatus::Stage::Failed) {
		return;
	}
	_status.stage = UploadStatus::Stage::Fetching;
	_status.error = Cloud::Error();
	_status.part = 0.;
	changed();
	schedule();
}

void Uploader::cancel() {
	if (_status.finished()) {
		return;
	}
	stopFetching();
	finish(UploadStatus::Stage::Cancelled);
}

void Uploader::disconnected() {
	if (_status.finished() || _status.stage == UploadStatus::Stage::Idle) {
		return;
	}
	stopFetching();
	_status.error = { .type = Cloud::Error::Type::NotConnected };
	finish(UploadStatus::Stage::Failed);
}

bool Uploader::fetching(not_null<DocumentData*> document) const {
	return _fetching && _ownDownload && (_document == document.get());
}

// The download from Telegram this upload has started is stopped. It is
// not a track that could not be taken: the watcher of the download must
// not count it as skipped.
void Uploader::stopFetching() {
	if (_ownDownload && _fetching && _document && _document->loading()) {
		const auto document = _document;
		_fetching = false;
		_fetchLifetime.destroy();
		document->cancel();
	}
}

void Uploader::clearCurrent() {
	++_generation;
	_watchdog.cancel();
	_addAgain.cancel();
	_addRetries = 0;
	_fetchLifetime.destroy();
	_sender.cancelAll();
	if (_transfer) {
		if (const auto account = _account.get()) {
			account->cancelTransfer(_transfer);
		}
		_transfer = 0;
	}
	_media = nullptr;
	_document = nullptr;
	_documentId = 0;
	_fetching = false;
	_ownDownload = false;
	_requested = false;
	_mime = QString();
	_input = TrackInput();
	if (!_tempPath.isEmpty()) {
		QFile::remove(_tempPath);
		_tempPath = QString();
	}
}

void Uploader::finish(UploadStatus::Stage stage) {
	clearCurrent();
	_status.stage = stage;
	_status.part = 0.;
	changed();
}

void Uploader::schedule() {
	if (_scheduled) {
		return;
	}
	_scheduled = true;
	crl::on_main(this, [=] {
		_scheduled = false;
		process();
	});
}

void Uploader::skip() {
	if (_status.finished()) {
		return;
	}
	++_status.skipped;
	++_status.index;
	schedule();
}

void Uploader::fail(const Cloud::Error &error) {
	if (_status.finished()) {
		return;
	}
	using Type = Cloud::Error::Type;
	const auto perTrack = (error.type == Type::File)
		|| (error.type == Type::Http
			&& (error.status == 413
				|| error.status == 415
				|| error.is("hash_mismatch")));
	if (perTrack) {
		skip();
		return;
	}
	_status.error = error;
	finish(UploadStatus::Stage::Failed);
}

void Uploader::process() {
	if (_status.finished() || _status.stage == UploadStatus::Stage::Idle) {
		return;
	}
	clearCurrent();
	if (_status.index >= _status.count) {
		finish(UploadStatus::Stage::Done);
		return;
	}
	const auto &source = _request.sources[_status.index];
	_status.stage = UploadStatus::Stage::Fetching;
	_status.part = 0.;
	_status.track = TrackName(
		source.title,
		source.performer,
		source.fileName);
	changed();

	_input.title = source.title;
	_input.performer = source.performer;
	_input.fileName = source.fileName;
	_input.duration = source.duration;
	if (!source.path.isEmpty()) {
		processFile(source);
	} else {
		processMessage(source);
	}
}

void Uploader::processMessage(const UploadSource &source) {
	const auto item = _session->data().message(source.item);
	if (!item) {
		const auto peer = _requested
			? nullptr
			: _session->data().peerLoaded(source.item.peer);
		if (!peer) {
			skip();
			return;
		}
		// The message is not in the memory yet: asked once.
		_requested = true;
		const auto generation = _generation;
		_session->api().requestMessageData(
			peer,
			source.item.msg,
			crl::guard(this, [=] {
				if (_generation != generation
					|| _status.stage != UploadStatus::Stage::Fetching
					|| _status.index >= _status.count) {
					return;
				}
				_watchdog.cancel();
				processMessage(_request.sources[_status.index]);
			}));
		_watchdog.callOnce(kResolveTimeout);
		return;
	}
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	using HistoryView::CopyRestrictionType;
	if (!document
		|| !document->isAudioFile()
		|| media->ttlSeconds()
		|| (HistoryView::CopyMediaRestrictionTypeFor(
			item->history()->peer,
			item) != CopyRestrictionType::None)) {
		skip();
		return;
	}
	const auto account = _account.get();
	const auto limit = account
		? account->limit("audio_bytes", kDefaultAudioLimit)
		: kDefaultAudioLimit;
	if (document->size <= 0 || document->size > limit) {
		skip();
		return;
	}
	if (const auto song = document->song()) {
		if (_input.title.isEmpty()) {
			_input.title = song->title;
		}
		if (_input.performer.isEmpty()) {
			_input.performer = song->performer;
		}
	}
	if (_input.fileName.isEmpty()) {
		_input.fileName = document->filename();
	}
	if (_input.duration <= 0) {
		_input.duration = document->duration();
	}
	_mime = document->mimeString();
	_documentId = document->id;

	const auto known = _store->knownMedia(document->id);
	if (Cloud::ValidMediaId(known)) {
		add(known, true);
		return;
	}

	_document = document;
	_media = document->createMediaView();
	_fetching = true;
	if (documentReady()) {
		return;
	}
	const auto generation = _generation;
	_session->data().documentLoadProgress(
	) | rpl::filter([=](not_null<DocumentData*> loaded) {
		return (loaded == document);
	}) | rpl::on_next([=] {
		if (!_fetching || _generation != generation) {
			return;
		} else if (document->loading()) {
			_status.part = document->progress();
			changed();
		} else if (!documentReady()) {
			_fetching = false;
			skip();
		}
	}, _fetchLifetime);
	if (!document->loading()) {
		// Without the "save file" dialog: into the memory when the file
		// is small enough, a larger one goes to a temporary file.
		auto target = QString();
		if (document->size >= Storage::kMaxFileInMemory) {
			const auto folder = cWorkingDir() + u"tdata/oblivion/share_tmp/"_q;
			if (!QDir().mkpath(folder)) {
				_fetching = false;
				skip();
				return;
			}
			target = folder
				+ QString::number(_session->userId().bare)
				+ '_'
				+ QString::number(document->id)
				+ u".part"_q;
		}
		_ownDownload = true;
		_tempPath = target;
		document->save(source.item, target);
		if (_fetching && !document->loading() && !documentReady()) {
			_fetching = false;
			skip();
		}
	}
}

bool Uploader::documentReady() {
	if (!_fetching || !_media || !_document || !_media->loaded(true)) {
		return false;
	}
	const auto bytes = _media->bytes();
	const auto path = bytes.isEmpty()
		? _document->filepath(true)
		: QString();
	if (bytes.isEmpty() && path.isEmpty()) {
		return false;
	}
	_fetching = false;
	send(path, bytes);
	return true;
}

void Uploader::processFile(const UploadSource &source) {
	const auto path = source.path;
	const auto account = _account.get();
	const auto limit = account
		? account->limit("audio_bytes", kDefaultAudioLimit)
		: kDefaultAudioLimit;
	const auto generation = _generation;
	const auto weak = base::make_weak(this);
	crl::async([=] {
		const auto info = QFileInfo(path);
		const auto good = info.isFile()
			&& info.size() > 0
			&& info.size() <= limit;
		auto song = Ui::PreparedFileInformation::Song();
		auto mime = QString();
		if (good) {
			const auto prepared = Media::Player::PrepareForSending(
				path,
				QByteArray());
			if (const auto got = std::get_if<
					Ui::PreparedFileInformation::Song>(&prepared.media)) {
				song = *got;
				song.cover = QImage();
			}
			mime = Core::MimeTypeForFile(info).name();
		}
		crl::on_main(weak, [=] {
			const auto strong = weak.get();
			if (!strong
				|| strong->_generation != generation
				|| strong->_status.stage != UploadStatus::Stage::Fetching) {
				return;
			} else if (!good || song.duration <= 0) {
				// Not a file the player can read.
				strong->skip();
				return;
			}
			auto &input = strong->_input;
			if (input.title.isEmpty()) {
				input.title = song.title;
			}
			if (input.performer.isEmpty()) {
				input.performer = song.performer;
			}
			if (input.fileName.isEmpty()) {
				input.fileName = QFileInfo(path).fileName();
			}
			if (input.duration <= 0) {
				input.duration = song.duration;
			}
			strong->_mime = mime;
			strong->send(path, QByteArray());
		});
	});
}

void Uploader::send(const QString &path, const QByteArray &bytes) {
	const auto account = _account.get();
	if (!account) {
		fail({ .type = Cloud::Error::Type::NotConnected });
		return;
	}
	_status.stage = UploadStatus::Stage::Sending;
	_status.part = 0.;
	changed();
	const auto generation = _generation;
	_transfer = account->upload({
		.path = path,
		.bytes = bytes,
		.kind = u"audio"_q,
		.mime = UploadMime(_mime),
		.done = crl::guard(this, [=](const QString &sha256, int64 size) {
			if (_generation != generation) {
				return;
			}
			_transfer = 0;
			add(sha256, false);
		}),
		.fail = crl::guard(this, [=](const Cloud::Error &error) {
			if (_generation != generation) {
				return;
			}
			_transfer = 0;
			fail(error);
		}),
		.progress = crl::guard(this, [=](int64 ready, int64 total) {
			if (_generation != generation || total <= 0) {
				return;
			}
			_status.part = std::clamp(ready / float64(total), 0., 1.);
			changed();
		}),
	});
}

void Uploader::add(const QString &media, bool known) {
	_status.stage = UploadStatus::Stage::Adding;
	_status.part = 1.;
	changed();

	_addMedia = media;
	_addKnown = known;
	if (_addGap > 0) {
		const auto wait = _addLast + _addGap - crl::now();
		if (wait > 0) {
			_addAgain.callOnce(wait);
			return;
		}
	}
	_addLast = crl::now();
	_input.media = media;
	auto tracks = QJsonArray();
	tracks.push_back(TrackInputToJson(_input));
	auto body = QJsonObject();
	body.insert(u"tracks"_q, tracks);
	const auto creating = _status.playlistId.isEmpty();
	if (creating) {
		body.insert(
			u"title"_q,
			_request.title.simplified().left(kMaxPlaylistTitle));
		body.insert(u"description"_q, QString());
		body.insert(u"collab"_q, false);
		body.insert(u"public"_q, false);
	}
	const auto path = creating
		? u"/v1/playlists"_q
		: (u"/v1/playlists/"_q + _status.playlistId + u"/tracks"_q);
	const auto generation = _generation;
	const auto documentId = _documentId;
	_sender.request(Cloud::PostRequest(path, body), crl::guard(this, [=](
			const Cloud::Response &response) {
		if (_generation != generation) {
			return;
		}
		const auto playlist = ParsePlaylist(
			response.json.value(u"playlist"_q).toObject());
		if (!playlist.valid()) {
			fail({ .type = Cloud::Error::Type::Protocol });
			return;
		}
		if (creating) {
			_status.created = true;
		}
		added(playlist, media);
	}), crl::guard(this, [=](const Cloud::Error &error) {
		if (_generation != generation) {
			return;
		} else if (known && error.is("media_not_found")) {
			// The server has dropped the file since: sent again.
			_store->setKnownMedia(documentId, QString());
			schedule();
			return;
		} else if (TrackRefused(error)) {
			skip();
			return;
		} else if (const auto wait = AddAgainDelay(error, _addRetries)) {
			// Too many tracks in a row for the server: this one again
			// after the pause it asks for, the upload does not stop, and
			// the next tracks keep the distance.
			++_addRetries;
			_addGap = std::max(_addGap, wait);
			_addAgain.callOnce(wait);
			return;
		}
		fail(error);
	}));
}

void Uploader::added(const Playlist &playlist, const QString &media) {
	_status.playlistId = playlist.id;
	if (_request.localId) {
		_store->setPublished(_request.localId, playlist.id);
	}
	if (_documentId) {
		_store->setKnownMedia(_documentId, media);
	}
	if (_store->findSaved(playlist.id)) {
		_store->save(playlist);
	}
	++_status.added;
	++_status.index;
	schedule();
}

namespace {

using ServicesMap = base::flat_map<
	not_null<Main::Session*>,
	std::unique_ptr<Service>>;

[[nodiscard]] ServicesMap &Services() {
	static const auto result = new ServicesMap();
	return *result;
}

void DestroyService(not_null<Main::Session*> session) {
	auto &map = Services();
	const auto i = map.find(session);
	if (i != end(map)) {
		const auto taken = std::move(i->second);
		map.erase(i);
	}
}

} // namespace

Service::Service(not_null<Main::Session*> session)
: _session(session)
, _account(base::make_weak(&Cloud::For(session)))
, _folder(AccountFolder(session))
, _store(_folder + u"shared.json"_q)
, _keepRetry([=] { resumeKeeping(); }) {
	// The files of «Добавить к себе» that are not on this device yet are
	// taken when the account is connected (after a restart as well). And
	// when the account is switched off (by the user, by a ban, by a
	// protocol that is too old) the core drops what was on the way
	// without calling back: nothing here may go on waiting for it.
	Cloud::For(session).readyValue(
	) | rpl::on_next([=](bool ready) {
		if (ready) {
			_keepFailures = 0;
			_keepRetry.callOnce(kKeepResumeDelay);
		} else {
			disconnected();
		}
	}, _lifetime);
}

void Service::disconnected() {
	_keepRetry.cancel();
	_keepTransfer = 0;
	if (!_keepLocal) {
		_keepBusy = false;
	}
	// Made again from the kept playlists when the account is back.
	const auto keeping = !_keepQueue.empty();
	_keepQueue.clear();
	_keepGoneRun = 0;

	_loading = _preloading = 0;
	if (_playing.loading) {
		_playing.loading = false;
		_playing.failed = true;
		_playing.progress = 0.;
		_playingChanges.fire({});
	}
	if (_uploader) {
		_uploader->disconnected();
	}
	if (keeping) {
		_keptChanges.fire({});
	}
}

Service::~Service() {
	// The session may be going away: nothing of it is touched here, and
	// the account of the cloud is not created again if it is gone.
	_uploader = nullptr;
	if (const auto account = _account.get()) {
		for (const auto id : { _loading, _preloading, _keepTransfer }) {
			if (id) {
				account->cancelTransfer(id);
			}
		}
	}
}

Main::Session &Service::session() const {
	return *_session;
}

Cloud::Account &Service::account() const {
	return Cloud::For(_session);
}

Store &Service::store() {
	return _store;
}

Uploader *Service::uploader() const {
	return _uploader.get();
}

Uploader *Service::startUpload(UploadRequest &&request) {
	if (_uploader && !_uploader->status().finished()) {
		return nullptr;
	}
	_uploader = std::make_unique<Uploader>(
		_session,
		&_store,
		std::move(request));
	_uploader->start();
	return _uploader.get();
}

void Service::dropUpload() {
	if (_uploader && _uploader->status().finished()) {
		// Not from inside of its own callback.
		crl::on_main(this, [=] {
			if (_uploader && _uploader->status().finished()) {
				_uploader = nullptr;
			}
		});
	}
}

QString Service::keptPath(const QString &media) const {
	return _folder + u"shared_media/"_q + media;
}

DocumentData *Service::makeDocument(
		const Track &track,
		const QString &path) {
	const auto info = QFileInfo(path);
	if (!info.isFile() || info.size() <= 0) {
		return nullptr;
	}
	auto &id = _documents[track.media];
	if (!id) {
		id = base::RandomValue<DocumentId>();
	}
	const auto document = _session->data().document(id);
	const auto name = !track.fileName.isEmpty()
		? track.fileName
		: (track.title.isEmpty() ? u"audio"_q : track.title) + u".mp3"_q;
	// The attribute goes without the title and the performer: with both
	// of them the document asks Telegram for an album cover at once, and
	// this one is not a document of Telegram (nothing about a track of a
	// shared playlist is ever sent there). The texts for the player are
	// set right after, they are read when the player is painted.
	document->setattributes({
		MTP_documentAttributeFilename(MTP_string(name)),
		MTP_documentAttributeAudio(
			MTP_flags(0),
			MTP_int(int(std::max(track.duration / 1000, int64(1)))),
			MTPstring(),
			MTPstring(),
			MTPbytes()),
	});
	if (const auto song = document->song()) {
		song->title = track.title;
		song->performer = track.performer;
	}
	document->size = info.size();
	document->setMimeString(track.mime.startsWith(u"audio/"_q)
		? track.mime
		: u"audio/mpeg"_q);
	document->setLocation(Core::FileLocation(path));
	return document->filepath(true).isEmpty() ? nullptr : document.get();
}

void Service::setupPlayer() {
	if (std::exchange(_playerReady, true)) {
		return;
	}
	const auto instance = Media::Player::instance();
	instance->tracksFinished(
	) | rpl::filter([=](AudioMsgId::Type type) {
		return (type == AudioMsgId::Type::Song);
	}) | rpl::on_next([=] {
		const auto now = instance->current(AudioMsgId::Type::Song).audio();
		if (!_current || now != _current || _playing.index < 0) {
			return;
		}
		// The next one, after the player is done with this one.
		const auto index = _playing.index + 1;
		crl::on_main(this, [=] {
			if (_current && _playing.index + 1 == index) {
				if (index < int(_queue.size())) {
					startAt(index);
				} else {
					resetPlaying();
				}
			}
		});
	}, _lifetime);

	instance->trackChanged(
	) | rpl::filter([=](AudioMsgId::Type type) {
		return (type == AudioMsgId::Type::Song);
	}) | rpl::on_next([=] {
		const auto now = instance->current(AudioMsgId::Type::Song).audio();
		if (_playing.index >= 0 && now != _current) {
			// The user has started something else: ours is over.
			resetPlaying();
		}
	}, _lifetime);
}

void Service::resetPlaying() {
	stopLoading();
	_current = nullptr;
	_queue.clear();
	_playing = Playing();
	_playingChanges.fire({});
}

void Service::stopLoading() {
	for (const auto id : { _loading, _preloading }) {
		if (id) {
			account().cancelTransfer(id);
		}
	}
	_loading = _preloading = 0;
	if (_playing.loading) {
		_playing.loading = false;
		_playing.progress = 0.;
		_playingChanges.fire({});
	}
}

void Service::play(const Playlist &playlist, int index) {
	if (index < 0 || index >= int(playlist.tracks.size())) {
		return;
	}
	const auto same = (_playing.playlistId == playlist.id)
		&& (_playing.index == index)
		&& (index < int(_queue.size()))
		&& (_queue[index].media == playlist.tracks[index].media);
	if (same && _playing.loading) {
		return;
	} else if (same && _current && !_playing.failed) {
		// A click on the track that plays: pause and resume.
		const auto instance = Media::Player::instance();
		if (instance->current(AudioMsgId::Type::Song).audio() == _current) {
			instance->playPause(AudioMsgId::Type::Song);
			return;
		}
	}
	_queue = playlist.tracks;
	_playing.playlistId = playlist.id;
	startAt(index);
}

void Service::startAt(int index) {
	stopLoading();
	if (index < 0 || index >= int(_queue.size())) {
		resetPlaying();
		return;
	}
	const auto track = _queue[index];
	_playing.index = index;
	_playing.media = track.media;
	_playing.loading = true;
	_playing.failed = false;
	_playing.progress = 0.;
	_playingChanges.fire({});

	const auto kept = keptPath(track.media);
	if (QFileInfo(kept).isFile()) {
		playFile(index, kept);
		return;
	}
	const auto media = track.media;
	const auto connected = _account.get();
	if ((!connected || !connected->ready())
		&& !QFileInfo(Cloud::MediaCachePath(media)).isFile()) {
		// The account is switched off and the file is not on this device:
		// a download that is asked for now may never answer.
		_playing.loading = false;
		_playing.failed = true;
		_playingChanges.fire({});
		return;
	}
	const auto current = [=] {
		return (_playing.index == index) && (_playing.media == media);
	};
	_loading = account().downloadMedia(
		media,
		crl::guard(this, [=](const QString &path) {
			if (current()) {
				_loading = 0;
				playFile(index, path);
			}
		}),
		crl::guard(this, [=](const Cloud::Error &error) {
			if (current()) {
				_loading = 0;
				_playing.loading = false;
				_playing.failed = true;
				_playingChanges.fire({});
			}
		}),
		crl::guard(this, [=](int64 ready, int64 total) {
			if (current() && total > 0) {
				_playing.progress = std::clamp(
					ready / float64(total),
					0.,
					1.);
				_playingChanges.fire({});
			}
		}));
}

void Service::playFile(int index, const QString &path) {
	if (index < 0 || index >= int(_queue.size())) {
		return;
	}
	const auto document = makeDocument(_queue[index], path);
	_playing.loading = false;
	_playing.progress = 0.;
	if (!document) {
		_playing.failed = true;
		_playingChanges.fire({});
		return;
	}
	setupPlayer();
	_current = document;
	_playing.failed = false;
	_playingChanges.fire({});
	Media::Player::instance()->play(AudioMsgId(document, FullMsgId()));
	preload(index + 1);
}

void Service::preload(int index) {
	if (index < 0 || index >= int(_queue.size())) {
		return;
	}
	const auto media = _queue[index].media;
	if (QFileInfo(keptPath(media)).isFile()) {
		return;
	}
	if (_preloading) {
		account().cancelTransfer(_preloading);
	}
	_preloading = account().downloadMedia(
		media,
		crl::guard(this, [=](const QString &path) {
			_preloading = 0;
		}),
		crl::guard(this, [=](const Cloud::Error &error) {
			_preloading = 0;
		}));
}

const Playing &Service::playing() const {
	return _playing;
}

rpl::producer<> Service::playingChanges() const {
	return _playingChanges.events();
}

void Service::keep(const Playlist &playlist) {
	if (!playlist.valid() || !playlist.full) {
		return;
	}
	_store.save(playlist);
	// A click: everything is tried now, whatever has failed before.
	for (const auto &track : playlist.tracks) {
		_keepGone.remove(track.media);
	}
	queueMissing(playlist);
	_keptChanges.fire({});
	_keepFailures = 0;
	_keepGoneRun = 0;
	keepNext();
}

// The tracks of a kept playlist that have no file on this device yet.
bool Service::queueMissing(const Playlist &playlist) {
	auto added = false;
	for (const auto &track : playlist.tracks) {
		const auto &media = track.media;
		if (!Cloud::ValidMediaId(media)
			|| _keepGone.contains(media)
			|| ranges::contains(_keepQueue, media, &KeepTask::media)
			|| QFileInfo(keptPath(media)).isFile()) {
			continue;
		}
		_keepQueue.push_back({ playlist.id, media });
		added = true;
	}
	return added;
}

// What «Добавить к себе» has not taken yet: after a restart, after a
// time without connection, after the account was switched on again.
void Service::resumeKeeping() {
	const auto account = _account.get();
	if (!account || !account->ready()) {
		return;
	}
	auto added = false;
	for (const auto &playlist : _store.saved()) {
		added = queueMissing(playlist) || added;
	}
	if (added) {
		_keptChanges.fire({});
	}
	keepNext();
}

bool Service::neededMedia(const QString &media) {
	for (const auto &playlist : _store.saved()) {
		if (ranges::contains(playlist.tracks, media, &Track::media)) {
			return true;
		}
	}
	return false;
}

// The tasks for files no kept playlist needs any more.
void Service::pruneKeepQueue() {
	if (_keepQueue.empty()) {
		return;
	}
	auto needed = base::flat_set<QString>();
	for (const auto &playlist : _store.saved()) {
		for (const auto &track : playlist.tracks) {
			needed.emplace(track.media);
		}
	}
	if (_keepTransfer && !needed.contains(_keepQueue.front().media)) {
		// The file that is being taken right now.
		if (const auto account = _account.get()) {
			account->cancelTransfer(_keepTransfer);
		}
		_keepTransfer = 0;
		_keepBusy = false;
	}
	_keepQueue.erase(
		ranges::remove_if(_keepQueue, [&](const KeepTask &task) {
			return !needed.contains(task.media);
		}),
		end(_keepQueue));
}

// The files of these tracks no kept playlist needs any more, with what a
// download that was stopped half way has left.
void Service::dropUnneeded(const std::vector<Track> &tracks) {
	auto needed = base::flat_set<QString>();
	for (const auto &playlist : _store.saved()) {
		for (const auto &track : playlist.tracks) {
			needed.emplace(track.media);
		}
	}
	for (const auto &track : tracks) {
		const auto &media = track.media;
		if (needed.contains(media) || !Cloud::ValidMediaId(media)) {
			continue;
		}
		const auto path = keptPath(media);
		QFile::remove(path + u".part"_q);
		if (!_current || _playing.media != media) {
			QFile::remove(path);
		}
	}
}

void Service::forget(const Playlist &playlist) {
	const auto id = playlist.id;
	const auto saved = _store.findSaved(id);
	const auto tracks = saved ? saved->tracks : playlist.tracks;
	_store.unsave(id);
	pruneKeepQueue();
	dropUnneeded(tracks);
	_keptChanges.fire({});
	keepNext();
}

bool Service::kept(const QString &playlistId) {
	return _store.findSaved(playlistId) != nullptr;
}

int Service::keptTracks(const Playlist &playlist) const {
	auto result = 0;
	for (const auto &track : playlist.tracks) {
		if (QFileInfo(keptPath(track.media)).isFile()) {
			++result;
		}
	}
	return result;
}

bool Service::keeping(const QString &playlistId) const {
	return ranges::contains(_keepQueue, playlistId, &KeepTask::playlistId);
}

rpl::producer<> Service::keptChanges() const {
	return _keptChanges.events();
}

void Service::keepNext() {
	if (_keepBusy || _keepQueue.empty()) {
		return;
	}
	const auto account = _account.get();
	if (!account || !account->ready()) {
		// Goes on when the account is connected: a download that is
		// asked for now would never answer.
		return;
	}
	const auto media = _keepQueue.front().media;
	const auto target = keptPath(media);
	const auto finishLater = [=](KeepResult result) {
		_keepBusy = true;
		_keepLocal = true;
		crl::on_main(this, [=] {
			keepDone(media, result);
		});
	};
	if (!Cloud::ValidMediaId(media)
		|| !QDir().mkpath(QFileInfo(target).absolutePath())) {
		finishLater(KeepResult::Gone);
		return;
	} else if (QFileInfo(target).isFile()) {
		finishLater(KeepResult::Done);
		return;
	}
	_keepBusy = true;
	const auto cached = Cloud::MediaCachePath(media);
	if (QFileInfo(cached).isFile()) {
		// It was listened to already: a copy instead of a download.
		_keepLocal = true;
		const auto weak = base::make_weak(this);
		crl::async([=] {
			const auto temp = target + u".copy"_q;
			QFile::remove(temp);
			const auto copied = QFile::copy(cached, temp)
				&& QFile::rename(temp, target);
			if (!copied) {
				QFile::remove(temp);
			}
			crl::on_main(weak, [=] {
				if (const auto strong = weak.get()) {
					strong->keepDone(
						media,
						copied ? KeepResult::Done : KeepResult::Failed);
				}
			});
		});
		return;
	}
	_keepLocal = false;
	_keepTransfer = account->download({
		.media = media,
		.to = target,
		.done = crl::guard(this, [=](const QString &path) {
			_keepTransfer = 0;
			keepDone(media, KeepResult::Done);
		}),
		.fail = crl::guard(this, [=](const Cloud::Error &error) {
			_keepTransfer = 0;
			keepDone(
				media,
				KeepGoneError(error)
					? KeepResult::Gone
					: KeepResult::Failed);
		}),
	});
}

void Service::keepDone(const QString &media, KeepResult result) {
	_keepBusy = false;
	_keepLocal = false;

	// The task may be gone already: its playlist was removed meanwhile,
	// or the account was switched off.
	const auto current = !_keepQueue.empty()
		&& (_keepQueue.front().media == media);
	if (result == KeepResult::Failed && current) {
		// No connection, most likely. The queue stays as it is: tried
		// again after a pause that grows up to an hour, when the account
		// connects and when the user opens the playlist.
		_keepRetry.callOnce(KeepRetryDelay(_keepFailures++));
		_keptChanges.fire({});
		return;
	}
	if (current) {
		_keepQueue.erase(begin(_keepQueue));
	}
	if (result == KeepResult::Gone) {
		// Stays on the list of the playlist and is not asked for again
		// in this launch (or till the playlist is opened).
		_keepGone.emplace(media);
		if (++_keepGoneRun >= kKeepGoneRun) {
			// One refusal after another (the playlist is gone from the
			// server, or it is not shared with this account any more):
			// the rest is not asked for in one burst.
			_keepGoneRun = 0;
			_keepRetry.callOnce(KeepRetryDelay(_keepFailures++));
			_keptChanges.fire({});
			return;
		}
	} else if (result == KeepResult::Done) {
		_keepFailures = 0;
		_keepGoneRun = 0;
		if (!neededMedia(media)
			&& Cloud::ValidMediaId(media)
			&& (!_current || _playing.media != media)) {
			// «Убрать из моих» while the file was on its way.
			QFile::remove(keptPath(media));
		}
	}
	_keptChanges.fire({});
	keepNext();
}

void Service::remember(const Playlist &playlist) {
	if (!playlist.valid() || !playlist.full) {
		return;
	}
	if (const auto saved = _store.findSaved(playlist.id)) {
		// A kept playlist follows the one of the server: the files of
		// the tracks that were removed from it are deleted, the new
		// tracks are saved.
		const auto before = saved->tracks;
		_store.save(playlist);
		pruneKeepQueue();
		dropUnneeded(before);
		for (const auto &track : playlist.tracks) {
			_keepGone.remove(track.media);
		}
		if (queueMissing(playlist)) {
			_keptChanges.fire({});
		}
		// It was just asked from the server: the connection is there.
		_keepFailures = 0;
		keepNext();
	}
	_updates.fire_copy(playlist);
}

rpl::producer<Playlist> Service::updates() const {
	return _updates.events();
}

Service &ServiceFor(not_null<Main::Session*> session) {
	auto &map = Services();
	const auto i = map.find(session);
	if (i != end(map)) {
		return *i->second;
	}
	auto service = std::make_unique<Service>(session);
	const auto raw = service.get();
	map.emplace(session, std::move(service));
	session->lifetime().add([=] {
		DestroyService(session);
	});
	return *raw;
}

bool QuietLoad(not_null<DocumentData*> document) {
	for (const auto &[session, service] : Services()) {
		const auto uploader = service->uploader();
		if (uploader && uploader->fetching(document)) {
			return true;
		}
	}
	return false;
}

void Start(not_null<Main::Session*> session) {
	// An account that has kept or shared a playlist: the files «Добавить
	// к себе» has not taken yet are taken when the account is connected
	// (the Service does it). Nothing is created for the other accounts,
	// and nothing is asked from the server before the account is ready.
	const auto own = AccountFolder(session);
	if (QFileInfo::exists(own + u"shared.json"_q)) {
		(void)ServiceFor(session);

		// What a copy of an earlier launch has left half way. No copy of
		// this launch is made yet.
		const auto media = own + u"shared_media"_q;
		crl::async([=] {
			auto dir = QDir(media);
			if (dir.exists()) {
				const auto files = dir.entryList(
					QStringList{ u"*.copy"_q },
					QDir::Files);
				for (const auto &name : files) {
					dir.remove(name);
				}
			}
		});
	}

	// Files of an upload that did not live to its end, once per launch:
	// later another account may be in the middle of its own upload.
	static auto Cleaned = false;
	if (std::exchange(Cleaned, true)) {
		return;
	}
	const auto folder = cWorkingDir() + u"tdata/oblivion/share_tmp/"_q;
	crl::async([=] {
		auto dir = QDir(folder);
		if (dir.exists()) {
			const auto files = dir.entryList(QDir::Files);
			for (const auto &name : files) {
				dir.remove(name);
			}
		}
	});
}

void Forget(not_null<Main::Session*> session) {
	if (const auto i = Services().find(session); i != end(Services())) {
		if (const auto uploader = i->second->uploader()) {
			uploader->cancel();
		}
	}
	DestroyService(session);

	// Like the rest of what the account kept on this device.
	const auto folder = AccountFolder(session);
	QFile::remove(folder + u"shared.json"_q);
	const auto media = folder + u"shared_media"_q;
	crl::async([=] {
		QDir(media).removeRecursively();
	});
}

bool RunSelfTest(QStringList &log) {
	auto check = Checker(log);
	TestPlaylists(check);
	TestStacks(check);
	TestFiles(check);
	TestRules(check);
	return !check.failed();
}

} // namespace Oblivion::Share
