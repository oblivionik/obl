/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_badge.h"

#include "base/call_delayed.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "data/data_peer.h"
#include "data/data_premium_limits.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "mtproto/sender.h"
#include "oblivion/oblivion_cloud_social.h"
#include "oblivion/oblivion_lang.h"
#include "oblivion/oblivion_settings.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "settings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/empty_userpic.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/text/text.h"
#include "ui/text/text_options.h"
#include "ui/userpic_view.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/tooltip.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat.h"
#include "styles/style_dialogs.h"
#include "styles/style_info.h"
#include "styles/style_info_profile_top_bar.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtGui/QPainterPath>

#include <mutex>

namespace Oblivion::Badge {
namespace {

// The marker: WORD JOINER, ZERO WIDTH NON-JOINER, ZERO WIDTH JOINER,
// WORD JOINER. All four are invisible in every Telegram app, take no
// width, don't change the direction of the text and are not trimmed
// by the text code of the app (Ui::Text::IsTrimmed drops spaces and
// ZERO WIDTH SPACE, so U+200B is not used). The joiners are a part of
// ordinary Persian text and of emoji sequences, so a server is not
// expected to drop them; the word joiners at both ends keep the pair
// from sticking to an emoji before it and may keep a bio made of the
// marker alone from being taken for an empty one (U+200B..U+200F are
// known to count as nothing in names).
// This exact sequence does not occur in a text typed by a person.
//
// Since round 5 the marker is only recognized and removed, never
// written (the badge is published through Oblivion Cloud): the code
// that builds a bio with it, AppendMarker(), is kept for the self-test,
// which checks that what an older Oblivion has written is still read
// and cut off the right way.
//
// After a text the marker is appended with a space before it. Without
// the space a link at the end of the bio would take the marker in: the
// apps that know nothing about it (Telegram Desktop among them) end a
// link at a space and not at an invisible character, and the link of
// the user would stop working there. A space at the end shows nothing.
constexpr char16_t kMarkerChars[] = { 0x2060, 0x200C, 0x200D, 0x2060 };
constexpr auto kMarkerLength = int(std::size(kMarkerChars));

// What the marker takes from the length limit of a bio with a text.
constexpr auto kMarkerCost = kMarkerLength + 1;

constexpr auto kVersion = 1;
constexpr auto kMaxKnown = 5000;
constexpr auto kMaxFileSize = 1024 * 1024;
constexpr auto kMaxRemoveTries = 3;
constexpr auto kMaxExactDouble = 9007199254740992.;
constexpr auto kHashLength = 64;

// After kMaxRemoveTries failures in a row the removal of a marker is
// tried again not more often than this (in seconds).
constexpr auto kRemoveRetryPause = int64(24 * 60 * 60);
constexpr auto kMaxFloodWait = 24 * 60 * 60;

constexpr auto kSaveDelay = crl::time(1500);
constexpr auto kRequestTimeout = crl::time(20000);
constexpr auto kFlowPause = crl::time(3000);

// How long after Oblivion or the user has changed the bio an own
// profile that arrives may still be an answer from before the change:
// longer than kRequestTimeout, such a request was sent before the change.
constexpr auto kSettleTime = crl::time(30000);

constexpr auto kNoticeDelay = crl::time(3000);
constexpr auto kToastMin = crl::time(3000);
constexpr auto kToastMax = crl::time(9000);
constexpr auto kToastPerChar = crl::time(60);
constexpr auto kTooltipDelay = 600;

// The mark is drawn on a 16x16 grid: a square with rounded corners
// turned by 45 degrees (kGlyphTip from the center to a corner before
// the rounding) with a round hole in the middle.
constexpr auto kGlyphGrid = 16.;
constexpr auto kGlyphTip = 7.;
constexpr auto kGlyphRound = 2.2;
constexpr auto kGlyphHole = 2.3;
constexpr auto kSqrt2 = 1.4142135623730951;

constexpr auto kSceneWidth = 340;
constexpr auto kSceneBoxWidth = 480;

[[nodiscard]] const QString &Marker() {
	static const auto result = QString::fromUtf16(
		kMarkerChars,
		kMarkerLength);
	return result;
}

[[nodiscard]] bool HasMarker(const QString &about) {
	return about.contains(Marker());
}

// The last occurrence goes first: a text that ends with a part of the
// marker gets back exactly what it was before the marker was appended.
// The space the marker was appended with goes away with it: the one
// before a marker at the very end, or one of the two around a marker
// that got into the middle (a text typed after it in another app).
[[nodiscard]] QString StripMarker(QString about) {
	const auto &marker = Marker();
	const auto separator = QChar(QChar::Space);
	auto index = about.lastIndexOf(marker);
	while (index >= 0) {
		const auto after = index + marker.size();
		const auto last = (after == about.size());
		const auto withSeparator = (index > 0)
			&& (about[index - 1] == separator)
			&& (last || about[after] == separator);
		const auto from = withSeparator ? (index - 1) : index;
		about.remove(from, after - from);
		index = about.lastIndexOf(marker);
	}
	return about;
}

// The way the bio field and the server count the length limit: by code
// points, a surrogate pair is one character.
[[nodiscard]] int BioLength(const QString &text) {
	auto result = int(text.size());
	for (const auto ch : text) {
		if (ch.isHighSurrogate()) {
			--result;
		}
	}
	return result;
}

// Spaces and the characters that take no place on the screen: the
// marker is made of such ones, and so are the blank letters people fill
// an empty name or bio with.
[[nodiscard]] bool IsInvisible(QChar ch) {
	const auto category = ch.category();
	if (ch.isSpace()
		|| (category == QChar::Other_Format)
		|| (category == QChar::Other_Control)
		|| (category == QChar::Separator_Space)
		|| (category == QChar::Separator_Line)
		|| (category == QChar::Separator_Paragraph)) {
		return true;
	}
	const auto code = ch.unicode();
	return (code == 0x115F) // The Hangul fillers.
		|| (code == 0x1160)
		|| (code == 0x3164)
		|| (code == 0xFFA0)
		|| (code == 0x2800) // The blank Braille pattern.
		|| (code >= 0xFE00 && code <= 0xFE0F); // Variation selectors.
}

// Whether a person sees anything in the text.
[[nodiscard]] bool HasVisibleText(const QString &text) {
	return ranges::any_of(text, [](QChar ch) {
		return !IsInvisible(ch);
	});
}

struct Appended {
	QString text;
	int lacking = 0;
	bool blank = false;
};

// The text of the user is never cut: without the room for the marker it
// is returned as it is (without a marker) together with the number of
// characters that don't fit.
//
// A text with nothing to see in it (an empty bio first of all) gets no
// marker either and is returned as it is: a bio made of invisible
// characters alone is never written, a regular Telegram app would show
// it as a bio with an empty text.
[[nodiscard]] Appended AppendMarker(const QString &about, int limit) {
	auto clean = StripMarker(about);
	if (!HasVisibleText(clean)) {
		return { .text = std::move(clean), .blank = true };
	}
	const auto lacking = BioLength(clean) + kMarkerCost - limit;
	if (lacking > 0) {
		return { .text = std::move(clean), .lacking = lacking };
	}
	return { .text = clean + QChar(QChar::Space) + Marker() };
}

[[nodiscard]] bool IsMarkerPart(QChar ch) {
	return (ch == QChar(QChar::Space))
		|| ranges::contains(kMarkerChars, ch.unicode());
}

// What the server has kept of the space and the marker appended to a
// bio, when it has kept only a part of them: such a bio has no marker
// in it, but it is not the text of the user any more either. Nothing of
// that text is kept on disk, only its hash.
struct Leftover {
	QString tail; // The characters left after the text of the user.
	QByteArray hash; // Of the whole bio they were seen in.
	int tries = 0;

	[[nodiscard]] bool empty() const {
		return tail.isEmpty();
	}
};

// The end of "now" if it is "was" with a part of what Oblivion appends
// after it, nothing for any other difference between the two.
[[nodiscard]] QString LeftoverTail(const QString &was, const QString &now) {
	const auto extra = now.size() - was.size();
	if (extra <= 0 || extra > kMarkerCost || !now.startsWith(was)) {
		return QString();
	}
	auto tail = now.mid(was.size());
	return ranges::all_of(tail, IsMarkerPart) ? tail : QString();
}

[[nodiscard]] QByteArray BioHash(const QString &text) {
	return QCryptographicHash::hash(
		text.toUtf8(),
		QCryptographicHash::Sha256).toHex();
}

[[nodiscard]] Leftover MakeLeftover(const QString &was, const QString &now) {
	auto tail = LeftoverTail(was, now);
	if (tail.isEmpty()) {
		return {};
	}
	return { .tail = std::move(tail), .hash = BioHash(now) };
}

[[nodiscard]] bool ValidLeftover(const Leftover &leftover) {
	return !leftover.tail.isEmpty()
		&& (leftover.tail.size() <= kMarkerCost)
		&& ranges::all_of(leftover.tail, IsMarkerPart)
		&& (leftover.hash.size() == kHashLength);
}

// The text of the user, if this is exactly the bio the leftover was seen
// in. A bio that was changed since then (in another app, for example) is
// none of Oblivion's business any more and is never touched.
[[nodiscard]] std::optional<QString> WithoutLeftover(
		const Leftover &leftover,
		const QString &about) {
	if (leftover.empty()
		|| !about.endsWith(leftover.tail)
		|| (BioHash(about) != leftover.hash)) {
		return std::nullopt;
	}
	return about.left(about.size() - leftover.tail.size());
}

// How long Telegram asks to wait, 0 if the error is not about that.
[[nodiscard]] crl::time FloodWait(const QString &type) {
	if (!MTP::IsFloodError(type)) {
		return 0;
	}
	const auto seconds = type.mid(type.lastIndexOf(u'_') + 1).toInt();
	return std::clamp(seconds, 1, kMaxFloodWait) * crl::time(1000);
}

// What the bio of the own account, loaded by the app for its own needs,
// means for the badge of that account.
enum class SelfAction {
	Keep,
	Adopt, // The marker is there, set by another copy of Oblivion.
	Lost, // The badge was on, the marker is gone.
	Remove, // The badge was switched off, the marker is still to remove.
	Removed, // It was to remove and is not there any more.
	Stuck, // It is to remove, but the removal keeps failing.
};

// Whether a removal that has failed "tries" times in a row, the last
// time at "last", may be tried once more at "now" (both in unixtime): in
// a few launches in a row, after that not more often than once in
// kRemoveRetryPause. A clock that was set back does not block it.
[[nodiscard]] bool RemovalDue(int tries, int64 last, int64 now) {
	return (tries < kMaxRemoveTries)
		|| (now < last)
		|| (now - last >= kRemoveRetryPause);
}

// Whether a wait Telegram has asked for, till "till", still holds at
// "now" (both in unixtime). Such a wait is never longer than
// kMaxFloodWait, so a time further away than that comes from a clock
// that was set back and blocks nothing.
[[nodiscard]] bool WaitHolds(int64 till, int64 now) {
	return (till > now) && (till - now <= kMaxFloodWait);
}

// A marker that is gone does not switch the badge off while the bio the
// user has emptied here stays without a visible text: the marker goes
// back into it with the next text the user saves, see bioForSaving().
// A bio with a text and without the marker was written somewhere else.
[[nodiscard]] bool BlankKeepsBadge(bool blankSaved, const QString &about) {
	return blankSaved && !HasVisibleText(about);
}

// A marker that is still to remove never switches the badge on, however
// many times its removal has failed: the user has switched the badge off
// and only the user switches it on again.
[[nodiscard]] SelfAction DecideSelf(
		bool own,
		bool removePending,
		bool removalDue,
		bool marker) {
	if (own) {
		return marker ? SelfAction::Keep : SelfAction::Lost;
	} else if (removePending) {
		return !marker
			? SelfAction::Removed
			: removalDue
			? SelfAction::Remove
			: SelfAction::Stuck;
	}
	return marker ? SelfAction::Adopt : SelfAction::Keep;
}

class KnownIds final {
public:
	bool add(uint64 id);
	bool remove(uint64 id);

	[[nodiscard]] bool contains(uint64 id) const;
	[[nodiscard]] bool empty() const;
	[[nodiscard]] int count() const;
	[[nodiscard]] const std::vector<uint64> &list() const;

private:
	std::vector<uint64> _order;
	base::flat_set<uint64> _ids;

};

bool KnownIds::add(uint64 id) {
	if (!id || !_ids.emplace(id).second) {
		return false;
	}
	_order.push_back(id);
	if (int(_order.size()) > kMaxKnown) {
		_ids.remove(_order.front());
		_order.erase(begin(_order));
	}
	return true;
}

bool KnownIds::remove(uint64 id) {
	if (!_ids.remove(id)) {
		return false;
	}
	_order.erase(ranges::remove(_order, id), end(_order));
	return true;
}

bool KnownIds::contains(uint64 id) const {
	return _ids.contains(id);
}

bool KnownIds::empty() const {
	return _order.empty();
}

int KnownIds::count() const {
	return int(_order.size());
}

const std::vector<uint64> &KnownIds::list() const {
	return _order;
}

// What is still to be told to the user, by the first window that can.
enum class Notice {
	None,
	Lost, // The badge was on and the marker is gone: it is off now.
	Adopted, // The marker was found in the bio: the badge is on now.
	Stuck, // The marker could not be removed, the badge stays off.
};

[[nodiscard]] QString NoticeName(Notice notice) {
	switch (notice) {
	case Notice::None: return QString();
	case Notice::Lost: return u"lost"_q;
	case Notice::Adopted: return u"adopted"_q;
	case Notice::Stuck: return u"stuck"_q;
	}
	Unexpected("Notice in Oblivion::Badge::NoticeName.");
}

// What is kept on disk for one account, tdata/oblivion/badge/<id>.json.
struct Stored {
	bool own = false; // The badge is on: the marker is in the bio.
	bool asked = false; // The consent was given for this account.

	// The user has switched the badge off and the marker is not known
	// to be gone yet. While this is set the badge is never switched on
	// by a marker found in the bio.
	bool removePending = false;
	int removeTries = 0;
	int64 removeLast = 0; // When the removal has failed the last time.

	// Telegram has asked to wait (FLOOD_WAIT) till this time, in
	// unixtime: Oblivion changes nothing in the profile before it, in
	// this launch or in the next ones.
	int64 waitTill = 0;

	Notice notice = Notice::None;
	Leftover leftover;
	KnownIds known;

	[[nodiscard]] bool empty() const {
		return !own
			&& !asked
			&& !removePending
			&& !waitTill
			&& (notice == Notice::None)
			&& leftover.empty()
			&& known.empty();
	}
};

// One more failure to remove the marker: the removal stays pending (the
// badge was switched off by the user), only the next try is put off.
void RemovalFailed(Stored &data, int64 now) {
	const auto was = data.removeTries;
	data.removeTries = std::min(was + 1, kMaxRemoveTries);
	data.removeLast = now;
	if (was < kMaxRemoveTries && data.removeTries == kMaxRemoveTries) {
		data.notice = Notice::Stuck;
	}
}

// Telegram has answered with a wait of this many seconds. That is not a
// failure: nothing is counted and nobody is told that the marker can't
// be removed, the next change of the profile is only put off.
void NoteWait(Stored &data, int64 now, int64 seconds) {
	data.waitTill = now + std::clamp(
		seconds,
		int64(1),
		int64(kMaxFloodWait));
}

[[nodiscard]] bool RemovalDue(const Stored &data, int64 now) {
	return !WaitHolds(data.waitTill, now)
		&& RemovalDue(data.removeTries, data.removeLast, now);
}

// What stays on disk after the account has logged out: nothing, except
// for a badge the user has switched off while its marker is still in
// the bio. Without that the same account signing in again would find
// the marker, take it for a badge set in another copy of Oblivion and
// switch the badge on, against what the user has chosen. Neither the
// known users nor the consent nor anything about the bio is kept, the
// consent is asked again.
[[nodiscard]] Stored AfterLogout(const Stored &data) {
	auto result = Stored();
	if (!data.own && data.removePending) {
		result.removePending = true;
		result.removeTries = data.removeTries;
		result.removeLast = data.removeLast;
		result.waitTill = data.waitTill;
	}
	return result;
}

// The marker is not in the bio any more, or the user wants it there.
void RemovalFinished(Stored &data) {
	data.removePending = false;
	data.removeTries = 0;
	data.removeLast = 0;
	if (data.notice == Notice::Stuck) {
		data.notice = Notice::None;
	}
}

// Nothing to keep is an empty array: the file is removed then.
[[nodiscard]] QByteArray Serialize(const Stored &data) {
	if (data.empty()) {
		return QByteArray();
	}
	auto known = QJsonArray();
	for (const auto id : data.known.list()) {
		known.push_back(QString::number(id));
	}
	auto object = QJsonObject();
	object.insert(u"version"_q, kVersion);
	object.insert(u"own"_q, data.own);
	object.insert(u"asked"_q, data.asked);
	object.insert(u"remove_pending"_q, data.removePending);
	object.insert(u"remove_tries"_q, data.removeTries);
	object.insert(u"remove_last"_q, qint64(data.removeLast));
	if (data.waitTill) {
		object.insert(u"wait_till"_q, qint64(data.waitTill));
	}
	object.insert(u"notice"_q, NoticeName(data.notice));
	if (!data.leftover.empty()) {
		object.insert(u"leftover_tail"_q, data.leftover.tail);
		object.insert(
			u"leftover_hash"_q,
			QString::fromLatin1(data.leftover.hash));
		object.insert(u"leftover_tries"_q, data.leftover.tries);
	}
	object.insert(u"known"_q, known);
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

[[nodiscard]] Stored Parse(const QByteArray &bytes) {
	auto result = Stored();
	if (bytes.isEmpty()) {
		return result;
	}
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(bytes, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return result;
	}
	const auto object = document.object();
	if (object.value(u"version"_q).toInt() != kVersion) {
		return result;
	}
	result.own = object.value(u"own"_q).toBool();
	result.asked = object.value(u"asked"_q).toBool();
	result.removePending = !result.own
		&& object.value(u"remove_pending"_q).toBool();
	result.removeTries = std::clamp(
		object.value(u"remove_tries"_q).toInt(),
		0,
		kMaxRemoveTries);
	const auto last = object.value(u"remove_last"_q).toDouble();
	if (result.removePending && last >= 1. && last < kMaxExactDouble) {
		result.removeLast = int64(last);
	}
	const auto till = object.value(u"wait_till"_q).toDouble();
	if (till >= 1. && till < kMaxExactDouble) {
		result.waitTill = int64(till);
	}

	// Each notice makes sense in one state only. "notice_lost" is what
	// the first version of the file had instead of "notice".
	const auto notice = object.value(u"notice"_q).toString();
	if (notice == NoticeName(Notice::Stuck)) {
		if (result.removePending) {
			result.notice = Notice::Stuck;
		}
	} else if (notice == NoticeName(Notice::Adopted)) {
		if (result.own) {
			result.notice = Notice::Adopted;
		}
	} else if (notice == NoticeName(Notice::Lost)
		|| object.value(u"notice_lost"_q).toBool()) {
		if (!result.own) {
			result.notice = Notice::Lost;
		}
	}
	if (!result.own) {
		auto leftover = Leftover{
			.tail = object.value(u"leftover_tail"_q).toString(),
			.hash = object.value(u"leftover_hash"_q).toString().toLatin1(),
			.tries = std::clamp(
				object.value(u"leftover_tries"_q).toInt(),
				0,
				kMaxRemoveTries),
		};
		if (ValidLeftover(leftover)) {
			result.leftover = std::move(leftover);
		}
	}
	const auto known = object.value(u"known"_q).toArray();
	for (const auto &value : known) {
		if (value.isString()) {
			result.known.add(value.toString().toULongLong());
		} else if (value.isDouble()) {
			const auto number = value.toDouble();
			if (number >= 1. && number < kMaxExactDouble) {
				result.known.add(uint64(number));
			}
		}
	}
	return result;
}

// A write made later must never be replaced by one that was scheduled
// earlier and ran after it (the last one is made on the main thread
// while the session is destroyed): each of them takes a number on the
// main thread and only the latest for its file touches the disk.
struct Disk {
	std::mutex mutex;
	base::flat_map<QString, uint64> generations;
};

[[nodiscard]] Disk &DiskState() {
	static const auto result = new Disk();
	return *result;
}

[[nodiscard]] uint64 NextGeneration(const QString &path) {
	auto &disk = DiskState();
	const auto lock = std::unique_lock(disk.mutex);
	return ++disk.generations[path];
}

[[nodiscard]] QByteArray ReadBytes(const QString &path) {
	const auto lock = std::unique_lock(DiskState().mutex);
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxFileSize) {
		return QByteArray();
	}
	return file.readAll();
}

void WriteBytes(
		const QString &path,
		const QByteArray &bytes,
		uint64 generation) {
	auto &disk = DiskState();
	const auto lock = std::unique_lock(disk.mutex);
	const auto i = disk.generations.find(path);
	if (i == end(disk.generations) || i->second != generation) {
		return;
	} else if (bytes.isEmpty()) {
		QFile::remove(path);
		return;
	}
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QSaveFile(path);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(bytes);
		file.commit();
	}
}

[[nodiscard]] QString AccountKey(not_null<Main::Session*> session) {
	return (session->isTestMode() ? u"test_"_q : QString())
		+ QString::number(session->userId().bare);
}

// Not in the folder of the account, tdata/oblivion/<id>/: that one is
// removed as a whole when the account logs out, and what AfterLogout()
// keeps has to outlive that.
[[nodiscard]] QString StoredPath(not_null<Main::Session*> session) {
	return cWorkingDir()
		+ u"tdata/oblivion/badge/"_q
		+ AccountKey(session)
		+ u".json"_q;
}

[[nodiscard]] QString LegacyStoredPath(not_null<Main::Session*> session) {
	return cWorkingDir()
		+ u"tdata/oblivion/"_q
		+ AccountKey(session)
		+ u"/badge.json"_q;
}

// The first builds with the badge kept the file in the folder of the
// account. A file found there is moved: written to the new place first
// and only then removed from the old one, so the state of the badge
// (a marker that is still to remove first of all) is never lost on the
// way. A file that is already at the new place is the newer of the two.
[[nodiscard]] QByteArray ReadMovingLegacy(
		const QString &path,
		const QString &legacyPath) {
	auto bytes = ReadBytes(path);
	if (!QFile::exists(legacyPath)) {
		return bytes;
	}
	if (bytes.isEmpty()) {
		bytes = ReadBytes(legacyPath);
		if (Parse(bytes).empty()) {
			bytes = QByteArray();
		} else {
			WriteBytes(path, bytes, NextGeneration(path));
			if (!QFile::exists(path)) {
				return bytes;
			}
		}
	}
	QFile::remove(legacyPath);
	return bytes;
}

[[nodiscard]] QPainterPath GlyphPath() {
	const auto edge = kGlyphTip * kSqrt2;
	auto square = QPainterPath();
	square.addRoundedRect(
		QRectF(-edge / 2., -edge / 2., edge, edge),
		kGlyphRound,
		kGlyphRound);
	auto result = QTransform().rotate(45.).map(square);
	result.addEllipse(QPointF(), kGlyphHole, kGlyphHole);
	result.setFillRule(Qt::OddEvenFill);
	return result;
}

// Where the mark box starts in a name place of this width.
[[nodiscard]] int MarkLeft(int available, int nameWidth, int markWidth) {
	return std::min(nameWidth, available - markWidth);
}

[[nodiscard]] int GlyphSide() {
	return st::dialogsVerifiedIcon.height();
}

int PaintMarkAfterName(
		QPainter &p,
		QRect rectForName,
		int nameWidth,
		int outerWidth,
		QColor color) {
	const auto width = Width();
	const auto side = std::min(GlyphSide(), rectForName.height());
	const auto left = rectForName.x()
		+ MarkLeft(rectForName.width(), nameWidth, width)
		+ (width - side);
	const auto top = rectForName.y() + (rectForName.height() - side) / 2;
	const auto x = style::RightToLeft()
		? (outerWidth - left - side)
		: left;
	Paint(p, QRect(x, top, side, side), color);
	return width;
}

void ScheduleChanges();
void SyncSetting();

// Whether Qt leaves this widget out when a parent of it is updated. A
// widget that says it paints all of its pixels (Qt::WA_OpaquePaintEvent:
// the chats list, the lists of messages and of peers), one that fills its
// background or paints on screen, and a window of its own are taken out
// of what QWidget::update() of a parent repaints: each of them has to be
// asked by itself.
[[nodiscard]] bool PaintsByItself(not_null<QWidget*> widget) {
	return widget->isWindow()
		|| widget->testAttribute(Qt::WA_OpaquePaintEvent)
		|| widget->testAttribute(Qt::WA_PaintOnScreen)
		|| widget->autoFillBackground();
}

// Everything inside root, at any depth, that update() of root leaves out.
[[nodiscard]] std::vector<not_null<QWidget*>> SelfPainted(
		not_null<QWidget*> root) {
	auto result = std::vector<not_null<QWidget*>>();
	for (const auto child : root->findChildren<QWidget*>()) {
		if (PaintsByItself(child)) {
			result.push_back(child);
		}
	}
	return result;
}

// The whole window is painted anew, the chats list and the other lists
// that paint opaquely included. Only paint events come of it: the helper
// Ui::ForceFullRepaint() is not used on purpose, it shows a widget over
// the window for a moment and the list under the mouse gets a leave
// event (the chats list then drops the hover and stops holding its order
// under the pointer, see Dialogs::InnerWidget::leaveEventHook).
void RepaintWhole(not_null<QWidget*> window) {
	window->update();
	for (const auto &widget : SelfPainted(window)) {
		if (widget->isVisible()) {
			widget->update();
		}
	}
}

// The badge of one account: the users known by a marker in the bio, the
// old marker in the own bio and the requests that remove it. The marker
// is never written any more: the badge is published through Oblivion
// Cloud (see oblivion_cloud_social.h), a marker is only recognized.
class State final : public base::has_weak_ptr {
public:
	explicit State(not_null<Main::Session*> session);
	~State();

	[[nodiscard]] bool own() const;
	[[nodiscard]] rpl::producer<bool> ownValue() const;
	[[nodiscard]] bool known(uint64 userId) const;

	// The changes for the widgets of this account: unlike the global
	// ones they end together with the session, so nothing that looks at
	// its peers is called after the session is gone.
	[[nodiscard]] rpl::producer<> changes() const;
	void fireChanges();

	[[nodiscard]] QString aboutLoaded(
		not_null<UserData*> user,
		const QString &about);
	void removeMarker(not_null<Window::SessionController*> controller);
	[[nodiscard]] QString bioForSaving(const QString &text);
	void bioSaveFinished();
	[[nodiscard]] int reservedBioLength() const;
	void forget();

private:
	enum class Flow {
		None,
		Disable,
		Remove,
		Restore,
	};
	enum class Failure {
		Timeout,
		Flood,
		Rejected,
		Other,
	};

	[[nodiscard]] static Failure FailureFrom(const MTP::Error &error);
	[[nodiscard]] static bool Refused(Failure failure);

	void selfAboutLoaded(const QString &about, bool marker);
	void removeLeftover(const QString &about);
	void restoreLeftover(const QString &about);
	void dropLeftover();
	void disable();
	void switchOff();
	void disabledLater();
	[[nodiscard]] bool flowBusy() const;
	[[nodiscard]] bool busy() const;
	[[nodiscard]] bool settled() const;
	[[nodiscard]] bool waiting() const;
	[[nodiscard]] bool removalDue() const;

	void begin(Flow flow);
	void finish(const QString &text = QString(), bool important = false);
	void read(Fn<void(const QString&)> done, Fn<void(Failure)> fail);
	void requestTimedOut();
	void write(
		const QString &about,
		Fn<void()> done,
		Fn<void(Failure)> fail);
	void noteFlood(const MTP::Error &error);

	void setOwn(bool value);
	void saveSoon();
	void save();
	void report(const QString &text, bool important = false);
	void flushNotice();
	[[nodiscard]] Window::SessionController *resolveWindow() const;
	[[nodiscard]] bool bioSavePending() const;

	const not_null<Main::Session*> _session;
	const QString _path;
	MTP::Sender _api;
	Stored _data;
	rpl::variable<bool> _own = false;
	rpl::event_stream<> _changes;

	Flow _flow = Flow::None;
	bool _written = false;
	bool _forgotten = false;
	bool _dirty = false;
	int _updatesSent = 0;
	mtpRequestId _requestId = 0;
	Fn<void(Failure)> _requestFail;
	base::weak_ptr<Window::SessionController> _window;

	// What has happened in this launch, never reset: the writes Oblivion
	// makes on its own (removeLeftover, restoreLeftover) are only for a
	// launch in which nothing has touched the bio here before them.
	bool _touched = false; // A flow has run or the bio was saved.
	bool _bioSaved = false; // The user has saved the bio in Settings.
	bool _markerSent = false; // A bio with the marker was sent.

	// The removal of the marker was asked here in this launch (or it was
	// still to remove when the launch began): a marker seen in the bio
	// after that is not taken for one that is back, it may be an answer
	// from before the removal. The next launch looks at the bio anew.
	bool _offHere = false;

	// The text of the user in a bio that was written with the marker and
	// could not be read back.
	std::optional<QString> _unverified;

	bool _bioSaving = false;
	crl::time _settleFrom = 0;
	crl::time _waitTill = 0;
	crl::time _flowFinishedAt = 0;

	base::Timer _saveTimer;
	base::Timer _requestTimer;

};

// Never destroyed: the states leave it together with their sessions, and
// the subscribers of the changes are widgets that are gone before it.
struct Registry {
	base::flat_map<not_null<Main::Session*>, std::unique_ptr<State>> states;
	rpl::event_stream<> changes;
	bool changesScheduled = false;
};

[[nodiscard]] Registry &Global() {
	static const auto result = new Registry();
	return *result;
}

[[nodiscard]] State *Lookup(not_null<Main::Session*> session) {
	const auto &states = Global().states;
	const auto i = states.find(session);
	return (i != end(states)) ? i->second.get() : nullptr;
}

// Oblivion::Get().badgeEnabled() mirrors "the old marker is in the bio of
// some account" (it used to mean the badge of round 4).
void SyncSetting() {
	const auto any = ranges::any_of(Global().states, [](const auto &pair) {
		return pair.second->own();
	});
	if (Get().badgeEnabled() != any) {
		Get().setBadgeEnabled(any);
	}
}

// Has() may change while an update from the server is being applied or
// while a session is logging out: the setting is synced, the names are
// repainted and the widgets are shown from the event loop, after that.
void ScheduleChanges() {
	auto &global = Global();
	if (global.changesScheduled) {
		return;
	}
	global.changesScheduled = true;
	crl::on_main([] {
		auto &global = Global();
		global.changesScheduled = false;
		if (Core::Quitting()) {
			// The sessions are going away without logging out.
			return;
		}
		SyncSetting();
		auto states = std::vector<base::weak_ptr<State>>();
		states.reserve(global.states.size());
		for (const auto &pair : global.states) {
			states.push_back(base::make_weak(pair.second.get()));
		}
		for (const auto &weak : states) {
			if (const auto strong = weak.get()) {
				strong->fireChanges();
			}
		}
		global.changes.fire({});
	});
}

State::State(not_null<Main::Session*> session)
: _session(session)
, _path(StoredPath(session))
, _api(&session->mtp())
, _data(Parse(ReadMovingLegacy(_path, LegacyStoredPath(session))))
, _own(_data.own)
, _offHere(_data.removePending)
, _saveTimer([=] { save(); })
, _requestTimer([=] { requestTimedOut(); }) {
	if (_data.own != Get().badgeEnabled()) {
		ScheduleChanges();
	}
	if (_data.waitTill
		&& !WaitHolds(_data.waitTill, base::unixtime::now())) {
		_data.waitTill = 0;
		saveSoon();
	}
	if (_data.notice != Notice::None) {
		base::call_delayed(kNoticeDelay, this, [=] {
			flushNotice();
		});
	}
}

State::~State() {
	_saveTimer.cancel();
	_requestTimer.cancel();
	if (_dirty && !_forgotten) {
		WriteBytes(_path, Serialize(_data), NextGeneration(_path));
	}
}

bool State::own() const {
	return _data.own;
}

rpl::producer<bool> State::ownValue() const {
	return _own.value();
}

bool State::known(uint64 userId) const {
	return _data.known.contains(userId);
}

rpl::producer<> State::changes() const {
	return _changes.events();
}

// The widgets that follow the badge by themselves (the mark of a profile,
// the top bar of a chat) are told. The rows of the chats list and of the
// other lists paint the mark together with a name (Ui::PeerBadge) and
// learn that a peer has got or lost the badge only when they are painted,
// so the windows of the account are repainted as a whole: without it a
// badge list that arrives from the cloud would show up row by row, as
// the mouse moves over the list. update() of the window alone is not
// enough for that: Qt leaves out exactly those lists, they paint
// opaquely (see RepaintWhole). This happens when the list or a setting
// has changed, not often.
void State::fireChanges() {
	_changes.fire({});
	if (_forgotten) {
		return;
	}
	for (const auto &window : _session->windows()) {
		RepaintWhole(window->widget());
	}
}

QString State::aboutLoaded(
		not_null<UserData*> user,
		const QString &about) {
	const auto marker = HasMarker(about);
	auto result = marker ? StripMarker(about) : about;
	if (_forgotten) {
		return result;
	} else if (user->isSelf()) {
		selfAboutLoaded(about, marker);

		// What the server has kept of a marker is not shown either.
		if (auto clean = WithoutLeftover(_data.leftover, about)) {
			result = std::move(*clean);
		}
	} else if (!user->isBot()) {
		const auto id = peerToUser(user->id).bare;
		const auto changed = marker
			? _data.known.add(id)
			: _data.known.remove(id);
		if (changed) {
			saveSoon();
			ScheduleChanges();
		}
	}
	if (_data.notice != Notice::None) {
		crl::on_main(this, [=] {
			flushNotice();
		});
	}
	return result;
}

// Not while Oblivion or the user is changing the bio here, and not right
// after that: an answer to a request sent before such a change would tell
// a state that is not true any more (the flows read the bio by their own
// requests). Later the own profile is looked at again, so a marker that
// another copy of Oblivion has removed or written is noticed without a
// restart, before the next save of the bio could put it back.
void State::selfAboutLoaded(const QString &about, bool marker) {
	if (!settled()) {
		return;
	}
	if (const auto original = base::take(_unverified)) {
		// The first look at the bio written by a flow that could not read
		// it back: without the marker it may have a part of it.
		if (!marker) {
			_data.leftover = MakeLeftover(*original, about);
			saveSoon();
		}
	}
	if (!_data.leftover.empty()) {
		if (!marker && WithoutLeftover(_data.leftover, about)) {
			crl::on_main(this, [=] {
				restoreLeftover(about);
			});
		} else if (marker || !_touched) {
			// The bio was changed since then, nothing in it is ours. In
			// the launch that has written it an old answer proves nothing.
			dropLeftover();
		}
	}
	const auto due = removalDue();
	switch (DecideSelf(_data.own, _data.removePending, due, marker)) {
	case SelfAction::Keep: break;
	case SelfAction::Stuck: break;
	case SelfAction::Adopt: {
		if (_offHere) {
			// Its removal was asked here in this launch: this may be an
			// old answer. The next launch looks at the bio again.
			break;
		}

		// An old marker is in the bio (written by an older Oblivion on
		// some device): it is only remembered, Settings > Oblivion
		// offers to remove it. Nothing is changed and nothing is told.
		setOwn(true);
		saveSoon();
	} break;
	case SelfAction::Lost: {
		// The marker is gone (the bio was saved here or somewhere else).
		setOwn(false);
		saveSoon();
	} break;
	case SelfAction::Removed: {
		if (_markerSent) {
			// The marker was written here in this launch, an old answer
			// must not make the removal look done.
			break;
		}
		RemovalFinished(_data);
		saveSoon();
	} break;
	case SelfAction::Remove: {
		crl::on_main(this, [=] {
			removeLeftover(about);
		});
	} break;
	}
}

// The badge was switched off when the update of that launch was already
// spent or the server could not be reached: the marker is removed now,
// silently, with the one update of this launch.
//
// A failure never switches the badge on again: the removal stays pending
// and is tried in the next launches, after kMaxRemoveTries failures in a
// row the user is told how to remove the marker by hand and the next
// tries are rare (see RemovalDue()).
//
// Only a refusal of the server is such a failure. A lost connection is
// not, and neither is a wait Telegram has asked for: noteFlood() keeps
// that one in the file and nothing is sent before it is over, in this
// launch or in the next ones.
//
// Not after the user has saved the bio in this launch: with the badge
// off that save has removed the marker already, and the text loaded
// before it must not be written over the new one.
void State::removeLeftover(const QString &about) {
	if (_flow != Flow::None
		|| _touched
		|| _forgotten
		|| _data.own
		|| !_data.removePending
		|| _updatesSent > 0
		|| _bioSaved
		|| !HasMarker(about)
		|| !removalDue()) {
		return;
	}
	_offHere = true;
	begin(Flow::Remove);
	write(StripMarker(about), [=] {
		RemovalFinished(_data);
		finish();
	}, [=](Failure failure) {
		if (Refused(failure)) {
			RemovalFailed(_data, base::unixtime::now());
		}
		finish();
		crl::on_main(this, [=] {
			flushNotice();
		});
	});
}

// The server has kept a part of the marker when the round 4 badge was
// switched on (the user was told then that those few invisible
// characters are removed in a next launch): the bio is put back to the
// text of the user, with the one update of a launch in which nothing
// else has touched the bio, and only while it is exactly the bio that
// was seen then. Nothing new gets into this state any more.
void State::restoreLeftover(const QString &about) {
	const auto clean = WithoutLeftover(_data.leftover, about);
	if (!clean
		|| _flow != Flow::None
		|| _touched
		|| _forgotten
		|| _data.own
		|| _updatesSent > 0
		|| _bioSaved
		|| waiting()
		|| (_data.leftover.tries >= kMaxRemoveTries)) {
		return;
	}
	begin(Flow::Restore);
	write(*clean, [=] {
		_data.leftover = Leftover();
		finish();
	}, [=](Failure failure) {
		if (Refused(failure)) {
			++_data.leftover.tries;
		}
		finish();
	});
}

void State::dropLeftover() {
	if (!_data.leftover.empty()) {
		_data.leftover = Leftover();
		saveSoon();
	}
}

// «Убрать старую метку из «О себе»» in Settings > Oblivion: the only
// way the bio is changed by Oblivion now, and only by that click.
void State::removeMarker(not_null<Window::SessionController*> controller) {
	if (_forgotten) {
		return;
	}
	flushNotice();
	if (flowBusy()) {
		controller->showToast(tr::lng_oblivion_badge_busy(tr::now));
		return;
	} else if (!_data.own && !_data.removePending) {
		return;
	}
	_window = base::make_weak(controller);

	// With the update of this launch spent, while Telegram asks to wait
	// or while the bio is being saved nothing is even asked: the marker
	// is removed in the next launch.
	if (_updatesSent > 0 || waiting() || bioSavePending()) {
		disabledLater();
	} else {
		disable();
	}
}

void State::disable() {
	switchOff();
	begin(Flow::Disable);
	read([=](const QString &about) {
		if (!HasMarker(about)) {
			RemovalFinished(_data);
			finish(tr::lng_oblivion_social_marker_none(tr::now));
		} else if (_updatesSent > 0 || bioSavePending()) {
			disabledLater();
		} else {
			write(StripMarker(about), [=] {
				RemovalFinished(_data);
				finish(tr::lng_oblivion_social_marker_removed(tr::now));
			}, [=](Failure) {
				disabledLater();
			});
		}
	}, [=](Failure) {
		disabledLater();
	});
}

// The marker is to remove from the click of the user, before anything is
// asked from the server and whatever happens next (the app may be closed
// with the requests on the way): till it is known to be gone a marker
// seen in the bio is not taken for a new one, see DecideSelf().
void State::switchOff() {
	_touched = true;
	_offHere = true;
	_unverified = std::nullopt;
	_data.removePending = true;
	_data.removeTries = 0;
	_data.removeLast = 0;
	setOwn(false);
	saveSoon();
}

void State::disabledLater() {
	switchOff();
	_settleFrom = crl::now();
	finish(waiting()
		? tr::lng_oblivion_social_marker_wait(tr::now)
		: tr::lng_oblivion_social_marker_later(tr::now));
}

// One flow at a time, with a pause after it: clicking the toggle again
// and again must not turn into a stream of requests.
bool State::flowBusy() const {
	return (_flow != Flow::None)
		|| (_flowFinishedAt
			&& (crl::now() - _flowFinishedAt < kFlowPause));
}

bool State::busy() const {
	return flowBusy() || bioSavePending();
}

// Whether an own profile that arrives tells the bio as it is now.
bool State::settled() const {
	return (_flow == Flow::None)
		&& !_bioSaving
		&& (!_settleFrom || (crl::now() - _settleFrom >= kSettleTime));
}

// Telegram has asked to wait before the next request: till then nothing
// is sent, a click on the toggle is answered at once. The wait is kept
// in the file as well, a restart does not end it.
bool State::waiting() const {
	return (_waitTill && (crl::now() < _waitTill))
		|| WaitHolds(_data.waitTill, base::unixtime::now());
}

bool State::removalDue() const {
	return !waiting() && RemovalDue(_data, base::unixtime::now());
}

State::Failure State::FailureFrom(const MTP::Error &error) {
	const auto code = error.code();
	return MTP::IsFloodError(error)
		? Failure::Flood
		: (code >= 400 && code < 500)
		? Failure::Rejected
		: Failure::Other;
}

bool State::Refused(Failure failure) {
	return (failure != Failure::Timeout) && (failure != Failure::Flood);
}

void State::begin(Flow flow) {
	_flow = flow;
	_written = false;
	_touched = true;
}

void State::finish(const QString &text, bool important) {
	if (_flow != Flow::None) {
		_flowFinishedAt = _settleFrom = crl::now();
	}
	_flow = Flow::None;
	_written = false;
	saveSoon();
	report(text, important);
	_window = base::weak_ptr<Window::SessionController>();
}

// Every error ends the flow, nothing is sent again by itself: neither
// after FLOOD_WAIT nor after a server error.
void State::read(Fn<void(const QString&)> done, Fn<void(Failure)> fail) {
	_requestFail = fail;
	_requestTimer.callOnce(kRequestTimeout);
	_requestId = _api.request(MTPusers_GetFullUser(
		MTP_inputUserSelf()
	)).done([=](const MTPusers_UserFull &result) {
		_requestId = 0;
		_requestTimer.cancel();
		_requestFail = nullptr;
		const auto &full = result.data().vfull_user().data();
		done(qs(full.vabout().value_or_empty()));
	}).fail([=](const MTP::Error &error) {
		_requestId = 0;
		_requestTimer.cancel();
		_requestFail = nullptr;
		noteFlood(error);
		fail(MTP::IsFloodError(error) ? Failure::Flood : Failure::Other);
	}).handleAllErrors().send();
}

void State::requestTimedOut() {
	_api.request(base::take(_requestId)).cancel();
	if (const auto fail = base::take(_requestFail)) {
		fail(Failure::Timeout);
	}
}

// A write that waits for the connection is taken back after the same
// time as a read: sent much later it would put into the bio a text read
// long ago, over whatever the user has written from another device since
// then. Whether the server has got a request that was taken back is not
// known, the flows count it as a failure and the own profile the app
// loads later tells the rest.
void State::write(
		const QString &about,
		Fn<void()> done,
		Fn<void(Failure)> fail) {
	++_updatesSent;
	_written = true;
	_requestFail = fail;
	_requestTimer.callOnce(kRequestTimeout);
	_requestId = _api.request(MTPaccount_UpdateProfile(
		MTP_flags(MTPaccount_UpdateProfile::Flag::f_about),
		MTPstring(),
		MTPstring(),
		MTP_string(about)
	)).done([=](const MTPUser &result) {
		_requestId = 0;
		_requestTimer.cancel();
		_requestFail = nullptr;
		_session->data().processUser(result);
		done();
	}).fail([=](const MTP::Error &error) {
		_requestId = 0;
		_requestTimer.cancel();
		_requestFail = nullptr;
		noteFlood(error);
		fail(FailureFrom(error));
	}).handleAllErrors().send();
}

void State::noteFlood(const MTP::Error &error) {
	if (const auto wait = FloodWait(error.type())) {
		_waitTill = crl::now() + wait;
		NoteWait(_data, base::unixtime::now(), wait / crl::time(1000));
		saveSoon();
	}
}

QString State::bioForSaving(const QString &text) {
	auto clean = HasMarker(text) ? StripMarker(text) : text;
	if (_forgotten) {
		return clean;
	}

	// From now on and till some time after bioSaveFinished() a bio that
	// arrives may be the one from before this save and tells nothing.
	_bioSaving = true;
	_bioSaved = true;
	_touched = true;

	// The whole bio is replaced by what the user has typed.
	_unverified = std::nullopt;
	dropLeftover();

	// The marker is not written any more: the bio is saved exactly as it
	// was typed. An old marker goes away with that, the own profile that
	// arrives after the save tells so.
	return clean;
}

void State::bioSaveFinished() {
	if (_bioSaving) {
		_bioSaving = false;
		_settleFrom = crl::now();
	}
}

// Nothing is appended to the bio any more, so nothing is reserved.
int State::reservedBioLength() const {
	return 0;
}

void State::forget() {
	_forgotten = true;
	_saveTimer.cancel();
	_requestTimer.cancel();
	_api.request(base::take(_requestId)).cancel();
	_requestFail = nullptr;
	_flow = Flow::None;
	_bioSaving = false;
	_window = base::weak_ptr<Window::SessionController>();
	_dirty = false;
	const auto kept = Serialize(AfterLogout(_data));
	_data = Stored();
	_own = false;
	WriteBytes(_path, kept, NextGeneration(_path));
	ScheduleChanges();
}

void State::setOwn(bool value) {
	if (_data.own == value) {
		return;
	}
	_data.own = value;
	_own = value;
	ScheduleChanges();
}

void State::saveSoon() {
	if (_forgotten) {
		return;
	}
	_dirty = true;
	if (!_saveTimer.isActive()) {
		_saveTimer.callOnce(kSaveDelay);
	}
}

void State::save() {
	if (!_dirty || _forgotten) {
		return;
	}
	_dirty = false;
	crl::async([
		path = _path,
		bytes = Serialize(_data),
		generation = NextGeneration(_path)
	] {
		WriteBytes(path, bytes, generation);
	});
}

void State::report(const QString &text, bool important) {
	if (text.isEmpty() || _forgotten) {
		return;
	}
	const auto window = resolveWindow();
	if (!window) {
		return;
	} else if (important) {
		window->show(Ui::MakeInformBox(text));
	} else {
		window->showToast(text, std::clamp(
			crl::time(text.size()) * kToastPerChar,
			kToastMin,
			kToastMax));
	}
}

// A removal the user has asked for and that keeps failing is told as
// soon as there is a window to tell it in (kept on disk till then). The
// notices of the round 4 badge ("the badge was switched on / off by a
// marker") mean nothing any more: one left in the file is dropped.
void State::flushNotice() {
	if ((_data.notice == Notice::None)
		|| (_flow != Flow::None)
		|| _forgotten
		|| Core::App().passcodeLocked()) {
		return;
	} else if (_data.notice != Notice::Stuck) {
		_data.notice = Notice::None;
		saveSoon();
		return;
	}
	const auto window = resolveWindow();
	if (!window) {
		return;
	}
	_data.notice = Notice::None;
	saveSoon();
	window->show(
		Ui::MakeInformBox(tr::lng_oblivion_social_marker_stuck(tr::now)));
}

// Session::tryResolveWindow() is not used: without a window it switches
// the active account, which a notice must never do.
Window::SessionController *State::resolveWindow() const {
	if (const auto strong = _window.get()) {
		return strong;
	}
	const auto &windows = _session->windows();
	for (const auto &window : windows) {
		if (window->isPrimary()) {
			return window;
		}
	}
	return windows.empty() ? nullptr : windows.front().get();
}

// The user has saved the bio in Settings and that request is still on
// the way (ApiWrap::saveSelfBio() tells when it is answered): a bio read
// now may be the old one, writing it back with or without the marker
// would undo the edit.
bool State::bioSavePending() const {
	return _bioSaving;
}

// The mark in the profile: the skip before it is a part of the widget.
class MarkWidget final
	: public Ui::RpWidget
	, public Ui::AbstractTooltipShower {
public:
	MarkWidget(
		QWidget *parent,
		rpl::producer<bool> shown,
		QSize glyph,
		int skip,
		const style::color &color);

	void setColorOverride(std::optional<QColor> color);

	// Ui::AbstractTooltipShower interface.
	QString tooltipText() const override;
	QPoint tooltipPos() const override;
	bool tooltipWindowActive() const override;

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	const QSize _glyph;
	const int _skip = 0;
	const style::color &_color;
	std::optional<QColor> _override;

};

MarkWidget::MarkWidget(
	QWidget *parent,
	rpl::producer<bool> shown,
	QSize glyph,
	int skip,
	const style::color &color)
: RpWidget(parent)
, _glyph(glyph)
, _skip(skip)
, _color(color) {
	resize(_skip + _glyph.width(), _glyph.height());
	setAccessibleName(tr::lng_oblivion_badge_tooltip(tr::now));
	hide();

	std::move(
		shown
	) | rpl::on_next([=](bool visible) {
		setVisible(visible);
	}, lifetime());

	events(
	) | rpl::on_next([=](not_null<QEvent*> e) {
		if (e->type() == QEvent::Enter) {
			Ui::Tooltip::Show(kTooltipDelay, this);
		} else if (e->type() == QEvent::Leave) {
			Ui::Tooltip::Hide();
		}
	}, lifetime());
}

void MarkWidget::setColorOverride(std::optional<QColor> color) {
	if (_override != color) {
		_override = color;
		update();
	}
}

void MarkWidget::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto left = style::RightToLeft() ? 0 : _skip;
	Paint(
		p,
		QRect(QPoint(left, 0), _glyph),
		_override.value_or(_color->c));
}

QString MarkWidget::tooltipText() const {
	return tr::lng_oblivion_badge_tooltip(tr::now);
}

QPoint MarkWidget::tooltipPos() const {
	return QCursor::pos();
}

bool MarkWidget::tooltipWindowActive() const {
	return window() && window()->isActiveWindow();
}

// The square of the verified star of a profile, with a half of its skip
// before it: the diamond has an empty margin of its own inside the square
// (about an eighth of the side at each tip), while the star fills it. With
// the whole skip the mark stands visibly farther from the name, and from
// a verified star before it, than it does in a row of the chats list.
[[nodiscard]] object_ptr<MarkWidget> CreateProfileMark(
		not_null<QWidget*> parent,
		rpl::producer<bool> shown) {
	return object_ptr<MarkWidget>(
		parent,
		std::move(shown),
		st::infoVerifiedStar.size(),
		st::infoVerifiedCheckPosition.x() / 2,
		st::infoPeerBadge.premiumFg);
}

[[nodiscard]] QString SampleText(const char *ru, const char *en) {
	return QString::fromUtf8(CurrentLanguageIsRussian() ? ru : en);
}

// A piece of the chats list for the snapshots: the same metrics, fonts
// and colours as in Dialogs::Ui::PaintRow, the mark in the three states
// of a row, after a verified check and after a name that does not fit.
class RowsSample final : public Ui::RpWidget {
public:
	explicit RowsSample(QWidget *parent);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	enum class Mode {
		Normal,
		Over,
		Active,
	};
	struct Row {
		Ui::Text::String name;
		QString text;
		QString date;
		std::unique_ptr<Ui::EmptyUserpic> userpic;
		Mode mode = Mode::Normal;
		bool verified = false;
	};

	void add(
		const QString &name,
		const QString &text,
		const QString &date,
		Mode mode,
		bool verified = false);

	std::vector<Row> _rows;

};

RowsSample::RowsSample(QWidget *parent)
: RpWidget(parent) {
	add(
		SampleText("Аня Смирнова", "Anna Smirnova"),
		SampleText("Тогда до завтра!", "See you tomorrow then!"),
		u"14:05"_q,
		Mode::Normal);
	add(
		SampleText("Максим Орлов", "Max Orlov"),
		SampleText("Фото", "Photo"),
		u"13:42"_q,
		Mode::Over);
	add(
		SampleText("Вера", "Vera"),
		SampleText("Отправила файл, посмотрите", "Sent you the file"),
		u"12:30"_q,
		Mode::Active);
	add(
		SampleText("Мария Иванова", "Maria Ivanova"),
		SampleText("Спасибо, получила", "Thanks, got it"),
		u"11:18"_q,
		Mode::Normal,
		true);
	add(
		SampleText(
			"Константин Константинопольский-Задунайский",
			"Constantine Constantinopolsky-Zadunaisky"),
		SampleText("Хорошо, договорились", "Fine, deal"),
		u"09:57"_q,
		Mode::Normal);
}

void RowsSample::add(
		const QString &name,
		const QString &text,
		const QString &date,
		Mode mode,
		bool verified) {
	const auto index = uint64(_rows.size()) + 1;
	_rows.push_back({
		.name = Ui::Text::String(
			st::semiboldTextStyle,
			name,
			Ui::NameTextOptions()),
		.text = text,
		.date = date,
		.userpic = std::make_unique<Ui::EmptyUserpic>(
			Ui::EmptyUserpic::UserpicColor(
				Ui::EmptyUserpic::ColorIndex(index)),
			name),
		.mode = mode,
		.verified = verified,
	});
}

int RowsSample::resizeGetHeight(int newWidth) {
	return int(_rows.size()) * st::defaultDialogRow.height;
}

void RowsSample::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto &row = st::defaultDialogRow;
	auto top = 0;
	for (const auto &entry : _rows) {
		const auto active = (entry.mode == Mode::Active);
		const auto over = (entry.mode == Mode::Over);
		p.fillRect(
			0,
			top,
			width(),
			row.height,
			(active
				? st::dialogsBgActive
				: over
				? st::dialogsBgOver
				: st::dialogsBg));
		entry.userpic->paintCircle(
			p,
			row.padding.left(),
			top + row.padding.top(),
			width(),
			row.photoSize);

		p.setFont(st::dialogsDateFont);
		p.setPen(active
			? st::dialogsDateFgActive
			: over
			? st::dialogsDateFgOver
			: st::dialogsDateFg);
		const auto dateWidth = st::dialogsDateFont->width(entry.date);
		p.drawText(
			width() - row.padding.right() - dateWidth,
			top + row.nameTop + st::dialogsDateFont->ascent,
			entry.date);

		const auto place = QRect(
			row.nameLeft,
			top + row.nameTop,
			(width()
				- row.nameLeft
				- row.padding.right()
				- dateWidth
				- st::dialogsDateSkip),
			st::semiboldFont->height);
		const auto mark = Width();
		auto upstream = 0;
		if (entry.verified) {
			const auto &icon = active
				? st::dialogsVerifiedIconActive
				: over
				? st::dialogsVerifiedIconOver
				: st::dialogsVerifiedIcon;
			icon.paint(
				p,
				place.x() + MarkLeft(
					place.width() - mark,
					entry.name.maxWidth(),
					icon.width()),
				place.y(),
				width());
			upstream = icon.width();
		}
		PaintMarkAfterName(
			p,
			place,
			entry.name.maxWidth() + upstream,
			width(),
			(active
				? st::dialogsVerifiedIconBgActive
				: over
				? st::dialogsVerifiedIconBgOver
				: st::dialogsVerifiedIconBg)->c);
		p.setPen(active
			? st::dialogsNameFgActive
			: over
			? st::dialogsNameFgOver
			: st::dialogsNameFg);
		entry.name.draw(p, {
			.position = place.topLeft(),
			.availableWidth = place.width() - upstream - mark,
			.elisionLines = 1,
		});

		p.setFont(st::dialogsTextFont);
		p.setPen(active
			? st::dialogsTextFgActive
			: over
			? st::dialogsTextFgOver
			: st::dialogsTextFg);
		p.drawText(
			row.textLeft,
			top + row.textTop + st::dialogsTextFont->ascent,
			st::dialogsTextFont->elided(
				entry.text,
				width() - row.textLeft - row.padding.right()));
		top += row.height;
	}
}

// The top bar of a chat for the snapshots, the geometry of the name and
// of the status is the one of HistoryView::TopBarWidget.
class TopBarSample final : public Ui::RpWidget {
public:
	explicit TopBarSample(QWidget *parent);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	const not_null<Ui::IconButton*> _search;
	const not_null<Ui::IconButton*> _menu;
	Ui::Text::String _name;
	QString _status;

};

TopBarSample::TopBarSample(QWidget *parent)
: RpWidget(parent)
, _search(Ui::CreateChild<Ui::IconButton>(this, st::topBarSearch))
, _menu(Ui::CreateChild<Ui::IconButton>(this, st::topBarMenuToggle))
, _name(
	st::msgNameStyle,
	SampleText("Аня Смирнова", "Anna Smirnova"),
	Ui::NameTextOptions())
, _status(SampleText("в сети", "online")) {
}

int TopBarSample::resizeGetHeight(int newWidth) {
	const auto top = (st::topBarHeight - _menu->height()) / 2;
	_menu->moveToRight(0, top, newWidth);
	_search->moveToRight(_menu->width(), top, newWidth);
	return st::topBarHeight;
}

void TopBarSample::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(rect(), st::topBarBg);
	p.fillRect(
		0,
		height() - st::lineWidth,
		width(),
		st::lineWidth,
		st::shadowFg);

	const auto left = st::topBarArrowPadding.right();
	const auto place = QRect(
		left,
		st::topBarArrowPadding.top(),
		(width()
			- _menu->width()
			- _search->width()
			- left
			- st::topBarNameRightPadding),
		st::msgNameStyle.font->height);
	const auto mark = PaintMarkAfterName(
		p,
		place,
		_name.maxWidth(),
		width(),
		st::dialogsVerifiedIconBg->c);
	p.setPen(st::dialogsNameFg);
	_name.draw(p, {
		.position = place.topLeft(),
		.availableWidth = place.width() - mark,
		.elisionLines = 1,
	});

	p.setFont(st::dialogsTextFont);
	p.setPen(st::historyStatusFgActive);
	p.drawText(
		left,
		(st::topBarHeight
			- st::topBarArrowPadding.bottom()
			- st::dialogsTextFont->height
			+ st::dialogsTextFont->ascent),
		_status);
}

// The cover of a profile for the snapshots: the name in the middle with
// the real widget of the mark after it, placed the way
// Info::Profile::TopBar places it.
class ProfileSample final : public Ui::RpWidget {
public:
	enum class Kind {
		Plain,
		Verified, // The verified star of Telegram before the mark.
		Colored, // A cover with a background of its own.
	};

	ProfileSample(QWidget *parent, Kind kind);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	const Kind _kind = Kind::Plain;
	const QString _nameText;
	const not_null<Ui::FlatLabel*> _name;
	const not_null<Ui::FlatLabel*> _status;
	const not_null<MarkWidget*> _mark;
	const Ui::EmptyUserpic _userpic;
	QPoint _verified;

};

ProfileSample::ProfileSample(QWidget *parent, Kind kind)
: RpWidget(parent)
, _kind(kind)
, _nameText(SampleText("Аня Смирнова", "Anna Smirnova"))
, _name(Ui::CreateChild<Ui::FlatLabel>(
	this,
	_nameText,
	st::infoTopBar.title))
, _status(Ui::CreateChild<Ui::FlatLabel>(
	this,
	SampleText("в сети", "online"),
	st::infoTopBar.subtitle))
, _mark(CreateProfileMark(this, rpl::single(true)).release())
, _userpic(
	Ui::EmptyUserpic::UserpicColor(Ui::EmptyUserpic::ColorIndex(1)),
	_nameText) {
	if (_kind == Kind::Colored) {
		// The colours TopBar::adjustColors() sets on such a cover.
		_name->setTextColorOverride(st::groupCallMembersFg->c);
		_status->setTextColorOverride(st::groupCallVideoSubTextFg->c);
		_mark->setColorOverride(st::groupCallMembersFg->c);
	}
}

int ProfileSample::resizeGetHeight(int newWidth) {
	const auto padding = st::boxRowPadding.left();
	const auto verified = (_kind == Kind::Verified)
		? (st::infoVerifiedCheckPosition.x() + st::infoVerifiedStar.width())
		: 0;
	const auto badges = verified + _mark->width();
	const auto available = newWidth - 2 * padding - badges;
	if (available > 0) {
		_name->resizeToNaturalWidth(available);
	}
	const auto left = (newWidth - _name->width() - badges) / 2;
	const auto top = st::infoProfileTopBarTitleTop;
	_name->moveToLeft(left, top, newWidth);
	_verified = QPoint(
		left + _name->width() + st::infoVerifiedCheckPosition.x(),
		top + st::infoVerifiedCheckPosition.y());
	_mark->moveToLeft(
		left + _name->width() + verified,
		top + (_name->height() - _mark->height()) / 2,
		newWidth);
	_status->resizeToNaturalWidth(newWidth - 2 * padding);
	_status->moveToLeft(
		(newWidth - _status->width()) / 2,
		st::infoProfileTopBarStatusTop,
		newWidth);
	return st::infoProfileTopBarStatusTop
		+ _status->height()
		+ st::infoProfileTopBarPhotoTop;
}

void ProfileSample::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	if (_kind == Kind::Colored) {
		// A sample of a profile colour, made of the palette.
		auto gradient = QLinearGradient(0, 0, 0, height());
		gradient.setColorAt(0., st::historyPeer5UserpicBg->c);
		gradient.setColorAt(1., st::historyPeer5UserpicBg2->c);
		p.fillRect(rect(), gradient);
	} else {
		p.fillRect(rect(), st::infoTopBar.bg);
	}
	const auto size = st::infoProfileTopBarPhotoSize;
	_userpic.paintCircle(
		p,
		(width() - size) / 2,
		st::infoProfileTopBarPhotoTop,
		width(),
		size);
	if (_kind == Kind::Verified) {
		st::infoVerifiedStar.paint(p, _verified.x(), _verified.y(), width());
		st::infoVerifiedCheck.paint(p, _verified.x(), _verified.y(), width());
	}
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	const auto width = style::ConvertScale(kSceneWidth);
	const auto boxSize = QSize(style::ConvertScale(kSceneBoxWidth), 0);

	// The notice about an old marker that could not be removed. (The
	// consent box of the round 4 badge and its notices are gone together
	// with the marker being written; the settings rows of the badge are
	// the scene "social_badge_settings" in oblivion_cloud_social_ui.cpp.)
	RegisterBoxScene(
		u"badge_notice_stuck"_q,
		boxSize,
		[](std::shared_ptr<Ui::Show> show) {
			return Ui::MakeInformBox(
				tr::lng_oblivion_social_marker_stuck(tr::now));
		});
	RegisterScene(
		u"badge_rows"_q,
		QSize(width, 0),
		[](not_null<Ui::RpWidget*> parent) {
			return Ui::CreateChild<RowsSample>(parent.get());
		});
	RegisterScene(
		u"badge_top_bar"_q,
		QSize(width, 0),
		[](not_null<Ui::RpWidget*> parent) {
			return Ui::CreateChild<TopBarSample>(parent.get());
		});
	const auto profile = [&](const QString &name, ProfileSample::Kind kind) {
		RegisterScene(
			name,
			QSize(width, 0),
			[=](not_null<Ui::RpWidget*> parent) {
				return Ui::CreateChild<ProfileSample>(parent.get(), kind);
			});
	};
	profile(u"badge_profile"_q, ProfileSample::Kind::Plain);
	profile(u"badge_profile_verified"_q, ProfileSample::Kind::Verified);
	profile(u"badge_profile_colored"_q, ProfileSample::Kind::Colored);
});

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

void TestMarker(Checker &check) {
	const auto &marker = Marker();
	check(marker.size() == kMarkerLength, "marker: four characters");
	check(BioLength(marker) == kMarkerLength, "marker: no surrogates");
	check(
		ranges::all_of(marker, [](QChar ch) {
			const auto code = ch.unicode();
			return (code == 0x2060) || (code == 0x200C) || (code == 0x200D);
		}),
		"marker: only word joiners and zero width joiners");
	check(
		ranges::none_of(marker, [](QChar ch) {
			return Ui::Text::IsTrimmed(ch)
				|| Ui::Text::IsBad(ch)
				|| Ui::Text::IsSpace(ch)
				|| Ui::Text::IsReplacedBySpace(ch)
				|| ch.isSpace()
				|| ch.isSurrogate();
		}),
		"marker: nothing the text code of the app trims or replaces");
	check(
		(marker.front().unicode() != 0x200D)
			&& (marker.back().unicode() != 0x200D),
		"marker: no joiner at the ends");
	check(
		(u"abc"_q + marker).trimmed() == (u"abc"_q + marker),
		"marker: survives QString::trimmed");
	check(
		(u"a  b"_q + marker).simplified() == (u"a b"_q + marker),
		"marker: survives QString::simplified");
	const auto utf8 = (u"abc"_q + marker).toUtf8();
	check(
		QString::fromUtf8(utf8) == (u"abc"_q + marker),
		"marker: survives the UTF-8 round trip");
	check(utf8.size() == 3 + 3 * kMarkerLength, "marker: 12 bytes in UTF-8");

	check(!HasMarker(QString()), "detect: an empty bio has no marker");
	check(HasMarker(marker), "detect: the marker alone");
	check(StripMarker(marker).isEmpty(), "strip: the marker alone");
	check(!HasMarker(u"Hello"_q), "detect: a plain text has no marker");
	check(
		!HasMarker(marker.left(kMarkerLength - 1))
			&& !HasMarker(marker.mid(1)),
		"detect: a part of the marker is not the marker");
	check(
		!HasMarker(u"a\u2060b\u200Cc\u200Dd\u2060"_q),
		"detect: the same characters apart are not the marker");
	check(
		!HasMarker(u"\u2060\u200D\u200C\u2060"_q),
		"detect: another order is not the marker");

	const auto samples = std::vector<QString>{
		u"Hello"_q,
		u" spaces around "_q,
		u"Привет, мир"_q,
		// A family emoji: three people joined with zero width joiners.
		u"\U0001F468\u200D\U0001F469\u200D\U0001F467"_q,
		// An emoji with a joiner left hanging after it.
		u"ok \U0001F44D\u200D"_q,
		// A flag and a keycap with a variation selector.
		u"\U0001F1F7\U0001F1FA 1\uFE0F\u20E3"_q,
		// Persian with a zero width non-joiner inside and at the end.
		u"\u0645\u06CC\u200C\u062E\u0648\u0627\u0647\u0645\u200C"_q,
		// Hebrew with right-to-left and left-to-right marks.
		u"\u05E9\u05DC\u05D5\u05DD\u200F abc\u200E"_q,
		// Arabic and Latin mixed, a right-to-left override.
		u"\u0645\u0631\u062D\u0628\u0627 hello \u202Eabc"_q,
		u"zero width space\u200B"_q,
		u"word joiner\u2060"_q,
		u"both joiners\u200C\u200D"_q,
		u"byte order mark\uFEFF"_q,
		// The beginning of the marker at the very end.
		u"almost\u2060\u200C\u200D"_q,
		// The end of the marker at the very beginning.
		u"\u200C\u200D\u2060 tail"_q,
		// One letter among invisible characters.
		u"\u2060\u200B a \u200D"_q,
	};
	for (const auto &sample : samples) {
		check(!HasMarker(sample), "tricky: no marker before it is added");
		check(HasVisibleText(sample), "tricky: there is a text to see");
		check(
			StripMarker(sample) == sample,
			"tricky: stripping changes nothing without a marker");
		const auto appended = AppendMarker(sample, 140);
		check(
			!appended.lacking && !appended.blank && HasMarker(appended.text),
			"tricky: the marker is added and found");
		check(
			appended.text.endsWith(marker)
				&& appended.text.startsWith(sample),
			"tricky: the marker goes to the end, the text is kept");
		check(
			(BioLength(appended.text) - BioLength(sample)) == kMarkerCost,
			"tricky: the marker takes four characters and a space");
		check(
			appended.text.at(sample.size()) == QChar(QChar::Space),
			"tricky: a space stands between the text and the marker");
		check(
			StripMarker(appended.text) == sample,
			"tricky: stripping gives the text back");
		check(
			AppendMarker(appended.text, 140).text == appended.text,
			"tricky: adding the marker twice changes nothing");
	}

	// A bio with nothing to see in it is never given the marker: a bio
	// made of invisible characters alone must not be written.
	const auto blanks = std::vector<QString>{
		QString(),
		u" "_q,
		u" \n\t "_q,
		u"\u00A0\u2003\u3000"_q,
		u"\u200B"_q,
		u"\u2060\u2060\u2060"_q,
		u"\u200E\u200F\u202E\uFEFF\u00AD"_q,
		u"\u3164\u115F\u1160\uFFA0\u2800"_q,
		u"\uFE0F"_q,
		marker,
		u" "_q + marker,
		marker + u" "_q + marker,
		marker.left(kMarkerLength - 1),
		u" "_q + marker.mid(1),
	};
	for (const auto &blank : blanks) {
		check(!HasVisibleText(blank), "blank: nothing to see in it");
		const auto appended = AppendMarker(blank, 140);
		check(
			appended.blank && !appended.lacking,
			"blank: told apart from a text without the room");
		check(
			!HasMarker(appended.text) && !HasVisibleText(appended.text),
			"blank: no marker is added");
		check(
			appended.text == StripMarker(blank),
			"blank: returned as it is, only an old marker is cut off");
	}
	check(
		AppendMarker(QString(), 140).text.isEmpty()
			&& AppendMarker(marker, 140).text.isEmpty()
			&& AppendMarker(u" "_q + marker, 140).text.isEmpty(),
		"blank: an empty bio stays empty");
	const auto unlimited = AppendMarker(QString(), 0);
	check(
		unlimited.blank && !unlimited.lacking && unlimited.text.isEmpty(),
		"blank: whatever the limit is");
	check(
		HasVisibleText(u"a"_q)
			&& HasVisibleText(u"."_q)
			&& HasVisibleText(u"\U0001F600"_q)
			&& HasVisibleText(u"\u2060\u0645\u200C"_q),
		"blank: a letter, a dot or an emoji is a text");
	check(
		StripAbout(u"Hello "_q + marker) == u"Hello"_q
			&& StripAbout(u"Hello"_q) == u"Hello"_q
			&& StripAbout(marker).isEmpty()
			&& StripAbout(u"almost\u2060\u200C\u200D"_q)
				== u"almost\u2060\u200C\u200D"_q,
		"strip: the helper for the bios that come aside from a profile");

	check(
		HasMarker(u"Hello"_q + marker + u" world"_q)
			&& (StripMarker(u"Hello"_q + marker + u" world"_q)
				== u"Hello world"_q),
		"anywhere: a marker in the middle (text typed after it)");
	check(
		HasMarker(u"abc"_q + marker + u" \n"_q)
			&& HasMarker(u"abc"_q + marker + u"\u200B\u2060"_q)
			&& HasMarker(u"abc\u200D"_q + marker + u"\u200C"_q),
		"anywhere: a marker followed by spaces or other invisible ones");
	check(
		StripMarker(marker + u"a"_q + marker + marker) == u"a"_q,
		"anywhere: every marker is removed");
	check(
		AppendMarker(u"Hello"_q + marker + u" world"_q, 70).text
			== (u"Hello world "_q + marker),
		"anywhere: saving moves the marker back to the end");
	check(
		StripMarker(u"Hello "_q + marker + u" world"_q) == u"Hello world"_q,
		"space: one of the two around a marker in the middle goes away");
	check(
		StripMarker(u"Hello "_q + marker + u"world"_q) == u"Hello world"_q,
		"space: the only one between two words stays");
	check(
		StripMarker(u"see t.me/name "_q + marker) == u"see t.me/name"_q,
		"space: the one before a marker at the end goes away");
	check(
		StripMarker(u" "_q + marker).isEmpty()
			&& (StripMarker(marker + u" text"_q) == u" text"_q),
		"space: at the very beginning of the text");
	const auto link = AppendMarker(u"see t.me/name"_q, 70).text;
	check(
		link == (u"see t.me/name "_q + marker),
		"space: a link at the end is closed by a space");
	check(
		Ui::Text::IsLinkEnd(link.at(link.size() - kMarkerCost)),
		"space: the text code of the app ends a link before the marker");
	check(
		ranges::none_of(marker, [](QChar ch) {
			return Ui::Text::IsLinkEnd(ch);
		}),
		"space: it is needed, the marker alone does not end a link");
	check(
		!HasMarker(StripMarker(
			u"\u2060\u200C"_q + marker + u"\u200D\u2060"_q)),
		"anywhere: a marker formed by the removal is removed too");
}

void TestLimits(Checker &check) {
	const auto &marker = Marker();
	const auto text = [](int length) {
		return QString(length, QChar(u'a'));
	};
	check(BioLength(QString()) == 0, "length: empty");
	check(BioLength(u"abc"_q) == 3, "length: plain");
	check(
		BioLength(u"\U0001F468\u200D\U0001F469"_q) == 3,
		"length: a surrogate pair is one character");

	const auto exact = AppendMarker(text(65), 70);
	check(
		!exact.lacking && (BioLength(exact.text) == 70),
		"limit: 65 characters, a space and the marker fill 70 exactly");
	const auto one = AppendMarker(text(66), 70);
	check(
		(one.lacking == 1) && (one.text == text(66)),
		"limit: 66 characters lack one, the text is not cut");
	const auto full = AppendMarker(text(70), 70);
	check(
		(full.lacking == kMarkerCost) && (full.text == text(70)),
		"limit: a full bio lacks five, the text is not cut");
	const auto over = AppendMarker(text(100), 70);
	check(
		(over.lacking == 35) && (over.text == text(100)),
		"limit: a bio over the limit is returned as it is");
	check(
		!AppendMarker(text(135), 140).lacking
			&& (AppendMarker(text(136), 140).lacking == 1),
		"limit: the Premium limit");
	check(
		AppendMarker(QString(), 70).blank
			&& AppendMarker(QString(), 70).text.isEmpty()
			&& !AppendMarker(u"a"_q, kMarkerCost + 1).lacking
			&& (AppendMarker(u"a"_q, kMarkerCost).lacking == 1),
		"limit: an empty bio gets nothing, one letter needs five more");

	auto emoji = QString();
	for (auto i = 0; i != 65; ++i) {
		emoji += u"\U0001F600"_q;
	}
	check(
		(emoji.size() == 130) && !AppendMarker(emoji, 70).lacking,
		"limit: 65 emoji fit, they are counted by code points");
	check(
		AppendMarker(emoji + u"\U0001F600"_q, 70).lacking == 1,
		"limit: 66 emoji lack one");

	const auto marked = AppendMarker(text(65) + u" "_q + marker, 70);
	check(
		!marked.lacking && (marked.text == text(65) + u" "_q + marker),
		"limit: a marker already there is not counted twice");
	const auto old = AppendMarker(text(65) + marker, 70);
	check(
		!old.lacking && (old.text == text(65) + u" "_q + marker),
		"limit: a marker without the space gets it");
	const auto moved = AppendMarker(marker + text(70), 70);
	check(
		(moved.lacking == kMarkerCost) && (moved.text == text(70)),
		"limit: without the room the old marker is removed too");
}

void TestDecisions(Checker &check) {
	using Action = SelfAction;
	check(
		DecideSelf(true, false, true, true) == Action::Keep,
		"self: the badge is on and the marker is there");
	check(
		DecideSelf(true, false, true, false) == Action::Lost,
		"self: the badge is on and the marker is gone");
	check(
		DecideSelf(true, true, true, false) == Action::Lost,
		"self: the badge that is on wins over a stale removal");
	check(
		DecideSelf(false, false, true, false) == Action::Keep,
		"self: the badge is off and there is no marker");
	check(
		DecideSelf(false, false, true, true) == Action::Adopt,
		"self: a marker set elsewhere switches the badge on");
	check(
		DecideSelf(false, true, true, true) == Action::Remove,
		"self: a marker left after switching off is removed");
	check(
		DecideSelf(false, true, true, false) == Action::Removed,
		"self: nothing is left to remove");
	check(
		DecideSelf(false, true, false, true) == Action::Stuck,
		"self: a removal that keeps failing waits, the badge stays off");
	check(
		DecideSelf(false, true, false, false) == Action::Removed,
		"self: a marker that has gone by itself ends the waiting");
	for (const auto due : { false, true }) {
		for (const auto marker : { false, true }) {
			const auto action = DecideSelf(false, true, due, marker);
			check(
				(action != Action::Adopt) && (action != Action::Keep),
				"self: a badge that is to remove is never switched on");
		}
	}

	constexpr auto kNow = int64(1700000000);
	for (auto tries = 0; tries != kMaxRemoveTries; ++tries) {
		check(
			RemovalDue(tries, kNow, kNow),
			"removal: the first tries go launch after launch");
	}
	check(
		!RemovalDue(kMaxRemoveTries, kNow, kNow)
			&& !RemovalDue(kMaxRemoveTries, kNow, kNow + 3600)
			&& !RemovalDue(
				kMaxRemoveTries,
				kNow,
				kNow + kRemoveRetryPause - 1),
		"removal: after them nothing is sent for a day");
	check(
		RemovalDue(kMaxRemoveTries, kNow, kNow + kRemoveRetryPause)
			&& RemovalDue(kMaxRemoveTries, kNow, kNow - 5)
			&& RemovalDue(kMaxRemoveTries, 0, kNow),
		"removal: then one more try, a clock set back does not block it");

	// The badge was switched off and every removal fails, launch after
	// launch: a request goes only when it is due, the removal stays
	// pending whatever happens and the badge is never switched on.
	auto data = Stored();
	data.asked = true;
	data.removePending = true;
	auto sent = 0;
	auto notices = 0;
	auto adopted = false;
	auto forgotten = false;
	for (auto launch = 0; launch != 40; ++launch) {
		// A launch every six hours, each one through the file on disk.
		const auto now = kNow + launch * int64(6 * 3600);
		data = Parse(Serialize(data));
		const auto action = DecideSelf(
			data.own,
			data.removePending,
			RemovalDue(data.removeTries, data.removeLast, now),
			true);
		if (action == Action::Remove) {
			++sent;
			RemovalFailed(data, now);
		} else if (action != Action::Stuck) {
			adopted = true;
		}
		if (data.notice == Notice::Stuck) {
			++notices;
			data.notice = Notice::None;
		}
		if (data.own || !data.removePending) {
			forgotten = true;
		}
	}
	check(!adopted && !forgotten, "removal: failures never switch it on");
	check(
		data.removeTries == kMaxRemoveTries,
		"removal: the count of the failures stops at the limit");
	check(notices == 1, "removal: the user is told once");

	// Three launches in a row, then one in four of the rest (once in 24
	// hours with a launch every six): 3 + 9 of the 40.
	check(sent == 12, "removal: the requests are few");

	const auto gone = DecideSelf(
		data.own,
		data.removePending,
		RemovalDue(data.removeTries, data.removeLast, kNow),
		false);
	check(gone == Action::Removed, "removal: a marker that is gone ends it");
	data.notice = Notice::Stuck;
	RemovalFinished(data);
	check(
		!data.removePending
			&& !data.removeTries
			&& !data.removeLast
			&& (data.notice == Notice::None),
		"removal: nothing is left of it then, the notice is dropped too");
	check(
		DecideSelf(false, data.removePending, true, true) == Action::Adopt,
		"removal: only then a new marker (another copy) is adopted");

	check(
		!WaitHolds(0, kNow)
			&& !WaitHolds(kNow - 1, kNow)
			&& !WaitHolds(kNow, kNow)
			&& WaitHolds(kNow + 1, kNow),
		"wait: it holds till its time and not a second longer");
	check(
		WaitHolds(kNow + kMaxFloodWait, kNow)
			&& !WaitHolds(kNow + kMaxFloodWait + 1, kNow)
			&& !WaitHolds(kNow, kNow - 30 * int64(86400)),
		"wait: a time too far away (a clock set back) blocks nothing");

	// Telegram answers the removal with FLOOD_WAIT_3600: nothing is sent
	// before the hour is over, whatever the number of launches in it,
	// and the wait is not a failure of the removal.
	const auto hour = int64(FloodWait(u"FLOOD_WAIT_3600"_q) / 1000);
	auto flooded = Stored();
	flooded.removePending = true;
	NoteWait(flooded, kNow, hour);
	flooded = Parse(Serialize(flooded));
	check(
		flooded.removePending
			&& (flooded.waitTill == kNow + 3600)
			&& !flooded.removeTries
			&& !flooded.removeLast
			&& (flooded.notice == Notice::None),
		"wait: kept in the file and not counted as a failed removal");
	check(
		!RemovalDue(flooded, kNow)
			&& !RemovalDue(flooded, kNow + 60)
			&& !RemovalDue(flooded, kNow + 3599),
		"wait: the removal is not due while Telegram asks to wait");
	check(
		RemovalDue(flooded, kNow + 3600)
			&& RemovalDue(flooded, kNow + 86400),
		"wait: it is due again when the wait is over");
	check(
		DecideSelf(false, true, RemovalDue(flooded, kNow + 60), true)
			== Action::Stuck,
		"wait: till then nothing is sent and the badge stays off");
	auto both = flooded;
	both.removeTries = kMaxRemoveTries;
	both.removeLast = kNow;
	check(
		!RemovalDue(both, kNow + 3600)
			&& RemovalDue(both, kNow + kRemoveRetryPause),
		"wait: the pause after the failures is kept apart from it");
	auto zero = Stored();
	NoteWait(zero, kNow, 0);
	auto endless = Stored();
	NoteWait(endless, kNow, int64(1) << 40);
	check(
		(zero.waitTill == kNow + 1)
			&& (endless.waitTill == kNow + kMaxFloodWait),
		"wait: never nothing and never longer than a day");

	auto waits = Stored();
	waits.asked = true;
	waits.removePending = true;
	auto asked = 0;
	auto wrong = false;
	for (auto launch = 0; launch != 36; ++launch) {
		// A launch every ten minutes, each one through the file on disk.
		const auto now = kNow + launch * int64(600);
		waits = Parse(Serialize(waits));
		const auto action = DecideSelf(
			waits.own,
			waits.removePending,
			RemovalDue(waits, now),
			true);
		if (action == Action::Remove) {
			++asked;
			NoteWait(waits, now, hour);
		} else if (action != Action::Stuck) {
			wrong = true;
		}
		if (waits.own
			|| !waits.removePending
			|| waits.removeTries
			|| (waits.notice != Notice::None)) {
			wrong = true;
		}
	}
	check(asked == 6, "wait: one request an hour in six hours of launches");
	check(
		!wrong,
		"wait: the user is not told that the marker can't be removed");

	// Rewriting the bio with the badge on: the old text is deleted, the
	// field saves the empty bio, the new text is typed and saved.
	const auto emptied = AppendMarker(QString(), 70);
	const auto retyped = AppendMarker(u"New bio"_q, 70);
	check(
		emptied.blank
			&& emptied.text.isEmpty()
			&& !retyped.blank
			&& !retyped.lacking
			&& HasMarker(retyped.text),
		"blank: an emptied bio goes as it is, the next text takes the marker");
	check(
		BlankKeepsBadge(true, QString())
			&& BlankKeepsBadge(true, u" \n"_q)
			&& BlankKeepsBadge(true, u"⁠‌"_q),
		"blank: the badge stays on while that bio has nothing to see");
	check(
		!BlankKeepsBadge(true, u"Hello"_q)
			&& !BlankKeepsBadge(true, u"."_q)
			&& !BlankKeepsBadge(false, QString())
			&& !BlankKeepsBadge(false, u"Hello"_q),
		"blank: a text without the marker still switches the badge off");
	check(
		DecideSelf(true, false, true, false) == Action::Lost,
		"blank: and so does a bio still empty in the next launch");
}

void TestLeftover(Checker &check) {
	const auto &marker = Marker();
	const auto was = u"Hello"_q;
	const auto full = was + u" "_q + marker;

	check(
		LeftoverTail(was, was).isEmpty()
			&& LeftoverTail(was, QString()).isEmpty()
			&& LeftoverTail(was, u"Hell"_q).isEmpty(),
		"leftover: the same or a shorter bio has none");
	check(
		LeftoverTail(was, was + u" ‌‍"_q) == u" ‌‍"_q,
		"leftover: the joiners the server has kept after the space");
	check(
		LeftoverTail(was, was + u"⁠"_q) == u"⁠"_q,
		"leftover: one character without the space");
	check(
		LeftoverTail(was, full) == (u" "_q + marker),
		"leftover: all that is appended is the longest one");
	check(
		LeftoverTail(was, full + u"⁠"_q).isEmpty(),
		"leftover: more than was appended is not ours");
	check(
		LeftoverTail(was, was + u" x"_q).isEmpty()
			&& LeftoverTail(was, was + u"​"_q).isEmpty()
			&& LeftoverTail(was, was + u"!"_q).isEmpty(),
		"leftover: a text typed after the bio is not ours");
	check(
		LeftoverTail(was, u"Hallo ‌"_q).isEmpty()
			&& LeftoverTail(was, u"New bio‌‍"_q).isEmpty(),
		"leftover: a bio changed elsewhere is not ours");

	const auto kept = was + u" ‌‍"_q;
	const auto leftover = MakeLeftover(was, kept);
	check(
		ValidLeftover(leftover)
			&& (leftover.tail == u" ‌‍"_q)
			&& (leftover.hash.size() == kHashLength)
			&& !leftover.tries,
		"leftover: remembered by the tail and the hash of the bio");
	check(
		MakeLeftover(was, was).empty()
			&& MakeLeftover(was, u"Other"_q).empty()
			&& !ValidLeftover(Leftover()),
		"leftover: nothing to remember without a tail");
	check(
		WithoutLeftover(leftover, kept) == was,
		"leftover: the bio it was seen in gives the text back");
	check(
		!WithoutLeftover(leftover, was)
			&& !WithoutLeftover(leftover, full)
			&& !WithoutLeftover(leftover, u"Hallo ‌‍"_q)
			&& !WithoutLeftover(leftover, kept + u" "_q)
			&& !WithoutLeftover(leftover, u" "_q + kept)
			&& !WithoutLeftover(leftover, QString())
			&& !WithoutLeftover(Leftover(), kept),
		"leftover: any other bio is left alone");
	check(
		!HasMarker(kept) && (StripAbout(kept) == kept),
		"leftover: a part of the marker is not cut off anybody's bio");

	// A new marker goes after the text, not after the part of the old.
	const auto again = AppendMarker(*WithoutLeftover(leftover, kept), 70);
	check(
		again.text == full,
		"leftover: switching on again does not pile the parts up");

	auto data = Stored();
	data.leftover = leftover;
	data.leftover.tries = 2;
	check(!data.empty(), "leftover: it is a reason to keep the file");
	const auto parsed = Parse(Serialize(data));
	check(
		(parsed.leftover.tail == leftover.tail)
			&& (parsed.leftover.hash == leftover.hash)
			&& (parsed.leftover.tries == 2)
			&& (WithoutLeftover(parsed.leftover, kept) == was),
		"leftover: the round trip through the file keeps it");
	check(
		!Serialize(data).contains("Hello"),
		"leftover: the file has nothing of the text");
	const auto hash = QString::fromLatin1(leftover.hash);
	const auto json = [](const QString &tail, const QString &digest) {
		return (u"{\"version\":1,\"asked\":true,\"leftover_tail\":\""_q
			+ tail
			+ u"\",\"leftover_hash\":\""_q
			+ digest
			+ u"\",\"leftover_tries\":99}"_q).toUtf8();
	};
	const auto loaded = Parse(json(u" ‌"_q, hash)).leftover;
	check(
		(loaded.tries == kMaxRemoveTries) && (loaded.tail == u" ‌"_q),
		"leftover: a valid one is read, the tries are capped");
	check(
		Parse(json(u"abc"_q, hash)).leftover.empty()
			&& Parse(json(QString(), hash)).leftover.empty()
			&& Parse(json(u" ‌"_q, u"1234"_q)).leftover.empty()
			&& Parse(json(u"      ‌"_q, hash)).leftover.empty(),
		"leftover: a wrong tail or hash in the file is dropped");
	data.own = true;
	check(
		Parse(Serialize(data)).leftover.empty(),
		"leftover: a badge that is on has none");

	check(
		(FloodWait(u"FLOOD_WAIT_30"_q) == 30 * crl::time(1000))
			&& (FloodWait(u"FLOOD_PREMIUM_WAIT_7"_q) == 7 * crl::time(1000)),
		"wait: the seconds Telegram asks for");
	check(
		!FloodWait(u"ABOUT_TOO_LONG"_q)
			&& !FloodWait(QString())
			&& !FloodWait(u"INTERNAL_500"_q),
		"wait: other errors ask for nothing");
	check(
		(FloodWait(u"FLOOD_WAIT_0"_q) == crl::time(1000))
			&& (FloodWait(u"FLOOD_WAIT_x"_q) == crl::time(1000))
			&& (FloodWait(u"FLOOD_WAIT_99999999"_q)
				== kMaxFloodWait * crl::time(1000)),
		"wait: never zero for a flood error and never endless");
}

void TestKnown(Checker &check) {
	auto known = KnownIds();
	check(known.empty() && !known.contains(1), "known: empty at first");
	check(!known.add(0), "known: zero is not an id");
	check(known.add(42) && known.contains(42), "known: an id is added");
	check(!known.add(42) && (known.count() == 1), "known: only once");
	check(known.add(7) && known.add(1234567890123ULL), "known: more ids");
	check(
		known.list() == std::vector<uint64>{ 42, 7, 1234567890123ULL },
		"known: the order of adding is kept");
	check(known.remove(7) && !known.contains(7), "known: an id is removed");
	check(!known.remove(7) && !known.remove(99), "known: only once removed");
	check(
		known.list() == std::vector<uint64>{ 42, 1234567890123ULL },
		"known: the rest stays in order");

	auto many = KnownIds();
	for (auto i = 1; i <= kMaxKnown + 10; ++i) {
		many.add(uint64(i));
	}
	check(many.count() == kMaxKnown, "known: the list is capped");
	check(
		!many.contains(1) && !many.contains(10) && many.contains(11),
		"known: the oldest ids are dropped first");
	check(
		many.contains(uint64(kMaxKnown) + 10)
			&& (many.list().front() == uint64(11)),
		"known: the newest ids stay");

	auto data = Stored();
	check(data.empty(), "stored: nothing to keep at first");
	check(Serialize(data).isEmpty(), "stored: nothing is written then");
	data.own = true;
	data.asked = true;
	data.known = known;
	const auto bytes = Serialize(data);
	const auto parsed = Parse(bytes);
	check(
		parsed.own
			&& parsed.asked
			&& !parsed.removePending
			&& (parsed.notice == Notice::None)
			&& parsed.leftover.empty()
			&& (parsed.removeTries == 0)
			&& (parsed.removeLast == 0)
			&& (parsed.known.list() == known.list()),
		"stored: the round trip keeps everything");
	check(Serialize(parsed) == bytes, "stored: serialization is stable");

	auto pending = Stored();
	pending.removePending = true;
	pending.removeTries = 2;
	pending.removeLast = int64(1700000000);
	pending.notice = Notice::Lost;
	const auto reparsed = Parse(Serialize(pending));
	check(
		!reparsed.own
			&& reparsed.removePending
			&& (reparsed.notice == Notice::Lost)
			&& (reparsed.removeTries == 2)
			&& (reparsed.removeLast == int64(1700000000)),
		"stored: the pending removal and the notice are kept");

	// A removal that has failed too many times is still pending in the
	// next launch: a marker in the bio is never taken for a new one.
	auto stuck = Stored();
	stuck.removePending = true;
	stuck.removeTries = kMaxRemoveTries;
	stuck.removeLast = int64(1700000000);
	stuck.notice = Notice::Stuck;
	const auto restuck = Parse(Serialize(stuck));
	check(
		!restuck.own
			&& restuck.removePending
			&& (restuck.removeTries == kMaxRemoveTries)
			&& (restuck.notice == Notice::Stuck),
		"stored: a removal that keeps failing stays pending");

	const auto notice = [](Notice value, bool own, bool removePending) {
		auto data = Stored();
		data.own = own;
		data.asked = true;
		data.removePending = removePending;
		data.notice = value;
		return Parse(Serialize(data)).notice;
	};
	check(
		(notice(Notice::Adopted, true, false) == Notice::Adopted)
			&& (notice(Notice::Lost, false, false) == Notice::Lost)
			&& (notice(Notice::Stuck, false, true) == Notice::Stuck)
			&& (notice(Notice::None, true, false) == Notice::None),
		"stored: each notice is kept in its state");
	check(
		(notice(Notice::Adopted, false, false) == Notice::None)
			&& (notice(Notice::Lost, true, false) == Notice::None)
			&& (notice(Notice::Stuck, false, false) == Notice::None)
			&& (notice(Notice::Stuck, true, true) == Notice::None),
		"stored: a notice that does not fit the state is dropped");
	check(
		Parse("{\"version\":1,\"notice_lost\":true}").notice == Notice::Lost,
		"parse: the notice of the first version of the file");
	check(
		Parse("{\"version\":1,\"asked\":true,\"notice\":\"what\"}").notice
			== Notice::None,
		"parse: an unknown notice is nothing");

	check(Parse(QByteArray()).empty(), "parse: an empty file");
	check(Parse("not json at all").empty(), "parse: garbage");
	check(Parse("[1,2,3]").empty(), "parse: not an object");
	check(Parse("{\"own\":true").empty(), "parse: a cut file");
	check(
		Parse("{\"version\":2,\"own\":true,\"known\":[\"5\"]}").empty(),
		"parse: an unknown version is not trusted");
	const auto mixed = Parse(
		"{\"version\":1,\"own\":\"yes\",\"remove_tries\":99,"
		"\"known\":[\"5\",6,\"5\",0,-3,\"x\",null,7.0,1e300,"
		"\"18446744073709551615\",{\"a\":1}]}");
	check(
		!mixed.own && (mixed.removeTries == kMaxRemoveTries),
		"parse: wrong types and values out of range");
	check(
		mixed.known.list()
			== std::vector<uint64>{ 5, 6, 7, 18446744073709551615ULL },
		"parse: only valid ids, each one once");
	const auto both = Parse(
		"{\"version\":1,\"own\":true,\"remove_pending\":true,"
		"\"remove_last\":1700000000,\"notice_lost\":true}");
	check(
		both.own
			&& !both.removePending
			&& !both.removeLast
			&& (both.notice == Notice::None),
		"parse: a badge that is on has nothing pending");
	check(
		!Parse("{\"version\":1,\"remove_pending\":true,"
			"\"remove_last\":-5}").removeLast
			&& !Parse("{\"version\":1,\"remove_pending\":true,"
				"\"remove_last\":1e300}").removeLast
			&& !Parse("{\"version\":1,\"remove_pending\":true,"
				"\"remove_last\":\"soon\"}").removeLast,
		"parse: a wrong time of the last removal is no time");

	auto waiting = Stored();
	waiting.waitTill = int64(1700003600);
	check(!waiting.empty(), "stored: a wait is a reason to keep the file");
	check(
		Parse(Serialize(waiting)).waitTill == int64(1700003600),
		"stored: the wait is kept with nothing else in the file");
	waiting.own = true;
	waiting.asked = true;
	check(
		Parse(Serialize(waiting)).waitTill == int64(1700003600),
		"stored: and with the badge on");
	check(
		!Serialize(data).contains("wait_till"),
		"stored: no wait is not written");
	check(
		!Parse("{\"version\":1,\"asked\":true,\"wait_till\":-5}").waitTill
			&& !Parse("{\"version\":1,\"asked\":true,"
				"\"wait_till\":1e300}").waitTill
			&& !Parse("{\"version\":1,\"asked\":true,"
				"\"wait_till\":\"soon\"}").waitTill,
		"parse: a wrong time of the wait is no wait");

	// Logging out: a badge that was switched off while its marker is
	// still in the bio leaves a record with nothing else in it, so that
	// the marker is removed and not adopted after signing in again.
	auto off = Stored();
	off.asked = true;
	off.removePending = true;
	off.removeTries = 2;
	off.removeLast = int64(1700000000);
	off.waitTill = int64(1700003600);
	off.notice = Notice::Stuck;
	off.leftover = MakeLeftover(u"Hello"_q, u"Hello ‌"_q);
	off.known.add(7777777);
	const auto left = Serialize(AfterLogout(off));
	const auto again = Parse(left);
	check(
		!left.isEmpty() && again.removePending && !again.own,
		"logout: a badge that is still to remove leaves a record");
	check(
		!again.asked
			&& again.known.empty()
			&& again.leftover.empty()
			&& (again.notice == Notice::None)
			&& !left.contains("7777777")
			&& !left.contains("leftover"),
		"logout: without the consent, the known users or the bio");
	check(
		(again.removeTries == 2)
			&& (again.removeLast == int64(1700000000))
			&& (again.waitTill == int64(1700003600)),
		"logout: when to try the removal next is kept with it");
	for (const auto due : { false, true }) {
		const auto action = DecideSelf(
			again.own,
			again.removePending,
			due,
			true);
		check(
			(action == SelfAction::Remove) || (action == SelfAction::Stuck),
			"logout: signing in again never switches that badge on");
	}
	check(
		DecideSelf(false, false, true, true) == SelfAction::Adopt,
		"logout: which is what an empty file would do");
	auto on = Stored();
	on.own = true;
	on.asked = true;
	on.waitTill = int64(1700003600);
	on.known.add(5);
	auto idle = Stored();
	idle.asked = true;
	idle.notice = Notice::Lost;
	idle.waitTill = int64(1700003600);
	idle.leftover = off.leftover;
	idle.known.add(5);
	auto stale = on;
	stale.removePending = true;
	check(
		Serialize(AfterLogout(on)).isEmpty()
			&& Serialize(AfterLogout(idle)).isEmpty()
			&& Serialize(AfterLogout(stale)).isEmpty()
			&& Serialize(AfterLogout(Stored())).isEmpty(),
		"logout: in every other state nothing is left on disk");

	auto huge = QByteArray("{\"version\":1,\"known\":[");
	for (auto i = 1; i <= kMaxKnown + 500; ++i) {
		huge += (i > 1 ? "," : "") + QByteArray::number(i);
	}
	huge += "]}";
	check(
		Parse(huge).known.count() == kMaxKnown,
		"parse: a file with too many ids is capped");
}

void TestDisk(Checker &check) {
	const auto folder = cWorkingDir() + u"oblivion_selftest_badge/"_q;
	QDir(folder).removeRecursively();
	const auto path = folder + u"12345/badge.json"_q;

	check(ReadBytes(path).isEmpty(), "disk: no file at first");
	auto data = Stored();
	data.own = true;
	data.known.add(777);
	const auto first = NextGeneration(path);
	WriteBytes(path, Serialize(data), first);
	const auto loaded = Parse(ReadBytes(path));
	check(
		loaded.own && loaded.known.contains(777),
		"disk: written (the folder is made) and read back");

	data.known.add(888);
	const auto stale = NextGeneration(path);
	const auto fresh = NextGeneration(path);
	data.known.add(999);
	WriteBytes(path, Serialize(data), fresh);
	data.known.remove(999);
	WriteBytes(path, Serialize(data), stale);
	check(
		Parse(ReadBytes(path)).known.contains(999),
		"disk: a write scheduled earlier does not replace a later one");
	WriteBytes(path, Serialize(data), first);
	check(
		Parse(ReadBytes(path)).known.contains(999),
		"disk: neither does a much older one");

	WriteBytes(path, Serialize(Stored()), NextGeneration(path));
	check(
		!QFile::exists(path) && ReadBytes(path).isEmpty(),
		"disk: nothing to keep removes the file");
	WriteBytes(path, QByteArray(), NextGeneration(path));
	check(!QFile::exists(path), "disk: removing twice is fine");

	// The file of the first builds, in the folder of the account, is
	// moved to the new place the first time it is found.
	const auto moved = folder + u"badge/12345.json"_q;
	check(
		ReadMovingLegacy(moved, path).isEmpty() && !QFile::exists(moved),
		"move: nothing to move");
	auto old = Stored();
	old.asked = true;
	old.removePending = true;
	old.known.add(777);
	WriteBytes(path, Serialize(old), NextGeneration(path));
	const auto taken = Parse(ReadMovingLegacy(moved, path));
	check(
		taken.removePending && taken.asked && taken.known.contains(777),
		"move: the old file is read");
	check(
		!QFile::exists(path)
			&& Parse(ReadBytes(moved)).removePending
			&& Parse(ReadBytes(moved)).known.contains(777),
		"move: and is at the new place only after that");
	check(
		Parse(ReadMovingLegacy(moved, path)).removePending,
		"move: the next launch reads the new place");

	auto newer = Stored();
	newer.own = true;
	newer.asked = true;
	WriteBytes(moved, Serialize(newer), NextGeneration(moved));
	WriteBytes(path, Serialize(old), NextGeneration(path));
	const auto kept = Parse(ReadMovingLegacy(moved, path));
	check(
		kept.own && !kept.removePending && !QFile::exists(path),
		"move: a file at the new place wins, the old one is removed");

	WriteBytes(moved, QByteArray(), NextGeneration(moved));
	QDir().mkpath(QFileInfo(path).absolutePath());
	{
		auto garbage = QFile(path);
		if (garbage.open(QIODevice::WriteOnly)) {
			garbage.write("not json at all");
		}
	}
	check(
		ReadMovingLegacy(moved, path).isEmpty()
			&& !QFile::exists(path)
			&& !QFile::exists(moved),
		"move: an old file with nothing in it is only removed");

	// What a logout leaves is written to the same place and read back
	// by the next session of that account.
	WriteBytes(moved, Serialize(AfterLogout(old)), NextGeneration(moved));
	const auto relogin = Parse(ReadMovingLegacy(moved, path));
	check(
		relogin.removePending && !relogin.asked && relogin.known.empty(),
		"logout: the record of a badge to remove is read after signing in");
	WriteBytes(
		moved,
		Serialize(AfterLogout(newer)),
		NextGeneration(moved));
	check(
		!QFile::exists(moved),
		"logout: a badge that is on leaves no file");

	QDir(folder).removeRecursively();
}

void TestGlyph(Checker &check) {
	check(MarkLeft(100, 40, 18) == 40, "place: right after a short name");
	check(MarkLeft(100, 82, 18) == 82, "place: a name that just fits");
	check(MarkLeft(100, 95, 18) == 82, "place: at the right edge otherwise");
	check(MarkLeft(100, 500, 18) == 82, "place: never outside");

	for (const auto side : { 16, 32, 18, 54 }) {
		auto image = QImage(
			QSize(side, side),
			QImage::Format_ARGB32_Premultiplied);
		image.fill(Qt::transparent);
		{
			auto p = QPainter(&image);
			Paint(p, QRect(0, 0, side, side), Qt::white);
		}
		const auto alpha = [&](float64 x, float64 y) {
			return qAlpha(image.pixel(
				std::clamp(int(x * side / kGlyphGrid), 0, side - 1),
				std::clamp(int(y * side / kGlyphGrid), 0, side - 1)));
		};
		check(
			(alpha(8, 3.5) > 200)
				&& (alpha(8, 12.5) > 200)
				&& (alpha(3.5, 8) > 200)
				&& (alpha(12.5, 8) > 200),
			"glyph: a solid body around the middle");
		check(
			(alpha(7.6, 7.6) < 40) && (alpha(8.4, 8.4) < 40),
			"glyph: a hole in the middle");
		check(
			!alpha(0.5, 0.5)
				&& !alpha(15.5, 0.5)
				&& !alpha(0.5, 15.5)
				&& !alpha(15.5, 15.5)
				&& !alpha(3, 3)
				&& !alpha(13, 13),
			"glyph: a diamond, the corners of the square are empty");
		check(
			(alpha(0.4, 8) < 40) && (alpha(8, 15.6) < 40),
			"glyph: it stays inside the square");

		// The curves are flattened in different directions on the two
		// sides, so the edge pixels may differ a little.
		constexpr auto kTolerance = 48;
		auto symmetric = true;
		for (auto y = 0; y != side; ++y) {
			for (auto x = 0; x != side; ++x) {
				const auto value = qAlpha(image.pixel(x, y));
				const auto mirrored = qAlpha(image.pixel(side - 1 - x, y));
				const auto flipped = qAlpha(image.pixel(x, side - 1 - y));
				if (std::abs(value - mirrored) > kTolerance
					|| std::abs(value - flipped) > kTolerance) {
					symmetric = false;
				}
			}
		}
		check(symmetric, "glyph: symmetric both ways");
	}

	auto wide = QImage(QSize(40, 16), QImage::Format_ARGB32_Premultiplied);
	wide.fill(Qt::transparent);
	{
		auto p = QPainter(&wide);
		Paint(p, QRect(0, 0, 40, 16), Qt::white);
	}
	check(
		!qAlpha(wide.pixel(5, 8))
			&& !qAlpha(wide.pixel(34, 8))
			&& (qAlpha(wide.pixel(16, 8)) > 200)
			&& (qAlpha(wide.pixel(23, 8)) > 200),
		"glyph: centered in a wide rectangle");

	auto none = QImage(QSize(4, 4), QImage::Format_ARGB32_Premultiplied);
	none.fill(Qt::transparent);
	{
		auto p = QPainter(&none);
		Paint(p, QRect(0, 0, 0, 4), Qt::white);
		Paint(p, QRect(2, 2, 4, -3), Qt::white);
	}
	check(!qAlpha(none.pixel(1, 1)), "glyph: an empty rectangle is skipped");
}

// Which widgets of a window are asked to repaint by themselves after the
// badge list has changed. The widgets are never shown here: only the
// choice is checked (the chats list is such a widget inside plain ones).
void TestRepaint(Checker &check) {
	QWidget root; // Deletes the rest with itself.
	const auto plain = new QWidget(&root);
	const auto list = new QWidget(plain);
	list->setAttribute(Qt::WA_OpaquePaintEvent);
	const auto row = new QWidget(list);
	const auto nested = new QWidget(row);
	nested->setAttribute(Qt::WA_OpaquePaintEvent);
	const auto filled = new QWidget(&root);
	filled->setAutoFillBackground(true);

	const auto found = SelfPainted(&root);
	const auto has = [&](QWidget *widget) {
		return ranges::contains(found, not_null<QWidget*>(widget));
	};
	check(has(list), "repaint: a list that paints opaquely is asked itself");
	check(has(nested), "repaint: also one inside another such list");
	check(has(filled), "repaint: and a widget that fills its background");
	check(
		!has(plain) && !has(row),
		"repaint: the plain widgets are painted with their parents");
	check(found.size() == 3, "repaint: nobody is asked twice");
	check(PaintsByItself(&root), "repaint: a window is asked by itself");
	check(
		SelfPainted(row).size() == 1,
		"repaint: only what is inside the widget is looked at");
}

} // namespace

bool Has(not_null<PeerData*> peer) {
	const auto user = peer->asUser();
	if (!user) {
		return false;
	}
	const auto session = &user->session();
	const auto id = peerToUser(user->id).bare;
	const auto state = Lookup(session);
	if (user->isSelf()) {
		// The own badge: published through the cloud, or an old marker
		// that is still in the bio (the others see the badge by it).
		return Social::BadgeListed(session, id) || (state && state->own());
	} else if (!Get().cloudBadgeShow() || user->isBot()) {
		return false;
	}
	return Social::BadgeListed(session, id) || (state && state->known(id));
}

rpl::producer<> Changes() {
	return Global().changes.events();
}

void Refresh() {
	ScheduleChanges();
}

QString AboutLoaded(not_null<UserData*> user, const QString &about) {
	if (const auto state = Lookup(&user->session())) {
		return state->aboutLoaded(user, about);
	}
	return StripAbout(about);
}

QString StripAbout(const QString &about) {
	return HasMarker(about) ? StripMarker(about) : about;
}

QString BioForSaving(
		not_null<Main::Session*> session,
		const QString &text) {
	if (const auto state = Lookup(session)) {
		return state->bioForSaving(text);
	}
	return text;
}

void BioSaveFinished(not_null<Main::Session*> session) {
	if (const auto state = Lookup(session)) {
		state->bioSaveFinished();
	}
}

int ReservedBioLength(not_null<UserData*> self) {
	const auto state = Lookup(&self->session());
	return state ? state->reservedBioLength() : 0;
}

void Paint(QPainter &p, QRect rect, QColor color) {
	const auto side = std::min(rect.width(), rect.height());
	if (side <= 0) {
		return;
	}
	static const auto path = GlyphPath();
	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	p.translate(QRectF(rect).center());
	p.scale(side / kGlyphGrid, side / kGlyphGrid);
	p.fillPath(path, color);
	p.restore();
}

int Width() {
	return st::dialogsVerifiedIcon.width();
}

int WidthFor(not_null<PeerData*> peer, int available) {
	if (!Has(peer)) {
		return 0;
	}
	const auto width = Width();
	return (available >= 2 * width) ? width : 0;
}

int PaintAfterName(
		QPainter &p,
		not_null<PeerData*> peer,
		QRect rectForName,
		int nameWidth,
		int outerWidth,
		QColor color) {
	if (!WidthFor(peer, rectForName.width())) {
		return 0;
	}
	return PaintMarkAfterName(p, rectForName, nameWidth, outerWidth, color);
}

object_ptr<Ui::RpWidget> CreateWidget(
		not_null<QWidget*> parent,
		not_null<PeerData*> peer) {
	auto shown = rpl::producer<bool>(rpl::single(false));
	const auto user = peer->asUser();
	if (const auto state = user ? Lookup(&user->session()) : nullptr) {
		shown = rpl::single(
			rpl::empty
		) | rpl::then(
			state->changes()
		) | rpl::map([=] {
			return Has(peer);
		}) | rpl::distinct_until_changed();
	}
	return CreateProfileMark(parent, std::move(shown));
}

void SetWidgetColor(
		not_null<Ui::RpWidget*> widget,
		std::optional<QColor> color) {
	if (const auto mark = dynamic_cast<MarkWidget*>(widget.get())) {
		mark->setColorOverride(color);
	}
}

rpl::producer<bool> OldMarkerValue(not_null<Main::Session*> session) {
	if (const auto state = Lookup(session)) {
		return state->ownValue();
	}
	return rpl::single(false);
}

void RemoveOldMarker(not_null<Window::SessionController*> controller) {
	if (const auto state = Lookup(&controller->session())) {
		state->removeMarker(controller);
	}
}

void Start(not_null<Main::Session*> session) {
	auto &states = Global().states;
	if (states.contains(session)) {
		return;
	}
	states.emplace(session, std::make_unique<State>(session));
	session->lifetime().add([=] {
		Global().states.remove(session);
	});

	// The own bio is kept on disk between the launches, and an earlier
	// build could have kept it there with the marker.
	const auto self = session->user();
	if (HasMarker(self->about())) {
		self->setAbout(StripMarker(self->about()));
	}
}

void Forget(not_null<Main::Session*> session) {
	if (const auto state = Lookup(session)) {
		state->forget();
		return;
	}
	const auto path = StoredPath(session);
	const auto data = Parse(
		ReadMovingLegacy(path, LegacyStoredPath(session)));
	WriteBytes(path, Serialize(AfterLogout(data)), NextGeneration(path));
}

bool RunSelfTest(QStringList &log) {
	auto check = Checker(log);
	TestMarker(check);
	check.section("marker");
	TestLimits(check);
	check.section("limits");
	TestDecisions(check);
	check.section("decisions");
	TestLeftover(check);
	check.section("leftover");
	TestKnown(check);
	check.section("known users");
	TestDisk(check);
	check.section("disk");
	TestGlyph(check);
	check.section("glyph");
	TestRepaint(check);
	check.section("repaint");
	log.push_back(u"badge: %1 checks passed, %2 failed"_q.arg(
		QString::number(check.passed()),
		QString::number(check.failed())));
	return !check.failed();
}

} // namespace Oblivion::Badge
