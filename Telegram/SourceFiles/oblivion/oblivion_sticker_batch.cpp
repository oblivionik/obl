/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_sticker_batch.h"

#include "base/event_filter.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "chat_helpers/emoji_picker_overlay.h"
#include "core/file_utilities.h"
#include "core/mime_type.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "menu/menu_action_with_thumbnail.h"
#include "oblivion/oblivion_lang.h"
#include "oblivion/oblivion_lottie.h"
#include "oblivion/oblivion_lottie_editor.h"
#include "oblivion/oblivion_photo_core.h"
#include "oblivion/oblivion_sticker_packs.h"
#include "oblivion/oblivion_sticker_packs_core.h"
#include "oblivion/oblivion_sticker_trim.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "oblivion/oblivion_video_core.h"
#include "settings/settings_common.h"
#include "ui/effects/ripple_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/text/text.h"
#include "ui/widgets/menu/menu.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/widgets/menu/menu_common.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/dynamic_image.h"
#include "ui/emoji_config.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/vertical_list.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_basic.h"
#include "styles/style_boxes.h"
#include "styles/style_emoji_picker_overlay.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <crl/crl_async.h>

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QPainterPath>

namespace Oblivion {
namespace StickerBatch {
namespace {

using StickerPacks::BatchBackend;
using StickerPacks::Format;
using StickerPacks::NewPack;
using StickerPacks::PackKind;
using StickerPacks::Prepared;
using PackInfo = StickerPacks::OwnPack;
using PacksState = StickerPacks::OwnPacks;
using Target = StickerPacks::PackTarget;

// The queue. More files than two full emoji sets take is never needed.
constexpr auto kMaxQueue = 400;

// The runs interrupted by the app that wait for a box, for one account.
// Each one keeps the thumbnails of its files.
constexpr auto kMaxInterruptedRuns = 4;

// The pace of the upload: one sticker (its file, messages.uploadMedia
// and the request that adds it to the set, one after another) and then
// a pause. The pause doubles after every FLOOD_WAIT and never shrinks
// back while the box is open.
constexpr auto kBaseDelay = crl::time(1200);
constexpr auto kMaxDelay = crl::time(10000);
constexpr auto kBurstCount = 20; // A longer pause after that many.
constexpr auto kBurstDelay = crl::time(4000);
constexpr auto kFloodSlack = crl::time(1000); // Over what was asked.
constexpr auto kDefaultFloodWait = 30; // Seconds, if no number is given.
constexpr auto kMaxFloodWait = 24 * 60 * 60;
constexpr auto kMaxFloodsInRow = 4; // Then the queue pauses itself.
constexpr auto kMaxFailuresInRow = 3; // The same.

// The converter.
constexpr auto kSniffSize = qint64(1024 * 1024);
constexpr auto kMaxLottieFile = qint64(8 * 1024 * 1024);
constexpr auto kThumbMaxPixels = 144;
constexpr auto kProgressStep = 0.03;

// The box.
constexpr auto kBoxWidth = 420;
constexpr auto kBoxMaxHeight = 620;
constexpr auto kThumbSize = 40;
constexpr auto kThumbInset = 3;
constexpr auto kThumbTextSkip = 12;
constexpr auto kBadgeSize = 16;
constexpr auto kBadgeShift = 4;
constexpr auto kRowEmojiMaxWidth = 66;
constexpr auto kRowEmojiSkip = 6;
constexpr auto kProgressHeight = 4;
constexpr auto kZonePadding = 20;
constexpr auto kZoneIconSize = 36;
constexpr auto kZoneSkip = 10;
constexpr auto kConverterStartDelay = crl::time(2000);
constexpr auto kTickInterval = crl::time(500);
constexpr auto kToastDuration = crl::time(4000);

// The buttons of the box change places with the state of the queue: the
// second click of a double click must not press the one that has come to
// the place of the first.
constexpr auto kRepeatedClickGuard = crl::time(500);
constexpr auto kPickerMaxWidth = 300;
constexpr auto kEmojiPreviewSide = 132;
constexpr auto kEmojiPreviewThumb = 96;
constexpr auto kEmojiPreviewSkip = 6;
constexpr auto kEmojiPreviewFiles = 8; // Shown for the emoji of all files.
constexpr auto kEmojiPreviewCell = 56;
constexpr auto kEmojiPreviewCellSkip = 12;
constexpr auto kEmojiPreviewTextSkip = 8;
constexpr auto kSceneWidth = 540;
constexpr auto kSceneEmojiWidth = 480;

// The set chosen the last time, while the app runs. Main thread only.
auto LastPackId = uint64(0);

// What the queue takes, by the extension: the file dialog and the drops
// offer only these, the content decides what a file really is.
constexpr auto kExtensions = std::array{
	"png", "jpg", "jpeg", "webp", "bmp", "heic", "heif", "avif", "tif",
	"tiff", "gif", "tgs", "json", "webm", "mp4", "mov", "m4v", "mkv",
};

// How many symbols of the text make the emoji it starts with, 0 if the
// text starts with anything else. Ui::Emoji in the app, a simple reader
// in the self-test: it runs before the emoji are loaded.
using EmojiLength = Fn<int(QStringView)>;

[[nodiscard]] int RealEmojiLength(QStringView text) {
	auto length = 0;
	const auto emoji = Ui::Emoji::Find(text, &length);
	return (emoji && length > 0) ? length : 0;
}

[[nodiscard]] QString DefaultEmoji() {
	// The one api/api_stickers_creator.cpp gives to a sticker without any.
	return QString::fromUtf8("\xF0\x9F\x99\x82");
}

// Only the emoji of the text, each of them once, not more than limit.
[[nodiscard]] QString CleanEmoji(
		const QString &text,
		const EmojiLength &length,
		int limit = StickerPacks::kMaxEmoji) {
	auto result = QString();
	if (!length) {
		return result;
	}
	auto found = QStringList();
	auto view = QStringView(text);
	while (!view.isEmpty() && found.size() < limit) {
		const auto count = length(view);
		if (count > 0 && count <= view.size()) {
			const auto one = view.left(count).toString();
			if (!found.contains(one)) {
				found.push_back(one);
				result.append(one);
			}
			view = view.mid(count);
		} else {
			view = view.mid(1);
		}
	}
	return result;
}

[[nodiscard]] QString ExtensionOf(const QString &path) {
	const auto slash = std::max(path.lastIndexOf('/'), path.lastIndexOf('\\'));
	const auto dot = path.lastIndexOf('.');
	return (dot > slash + 1) ? path.mid(dot + 1).toLower() : QString();
}

[[nodiscard]] bool AcceptsExtension(const QString &path) {
	const auto extension = ExtensionOf(path);
	if (extension.isEmpty()) {
		return false;
	}
	for (const auto known : kExtensions) {
		if (extension == QLatin1String(known)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] QString DialogFilter() {
	auto masks = QStringList();
	for (const auto known : kExtensions) {
		masks.push_back(u"*."_q + QLatin1String(known));
	}
	return tr::lng_oblivion_packs_filter(tr::now)
		+ u" ("_q
		+ masks.join(' ')
		+ u")"_q;
}

// The order of a file manager: the numbers inside the names are compared
// as numbers ("2.png" goes before "10.png"), the case is ignored.
[[nodiscard]] bool NaturalLess(const QString &a, const QString &b) {
	const auto digit = [](QChar ch) {
		return (ch.unicode() >= '0') && (ch.unicode() <= '9');
	};
	auto i = 0;
	auto j = 0;
	while (i < a.size() && j < b.size()) {
		if (digit(a[i]) && digit(b[j])) {
			auto fromA = i;
			auto fromB = j;
			while (fromA < a.size() - 1 && a[fromA] == '0' && digit(a[fromA + 1])) {
				++fromA;
			}
			while (fromB < b.size() - 1 && b[fromB] == '0' && digit(b[fromB + 1])) {
				++fromB;
			}
			auto tillA = fromA;
			auto tillB = fromB;
			while (tillA < a.size() && digit(a[tillA])) {
				++tillA;
			}
			while (tillB < b.size() && digit(b[tillB])) {
				++tillB;
			}
			const auto lengthA = tillA - fromA;
			const auto lengthB = tillB - fromB;
			if (lengthA != lengthB) {
				return lengthA < lengthB;
			}
			for (auto k = 0; k != lengthA; ++k) {
				if (a[fromA + k] != b[fromB + k]) {
					return a[fromA + k] < b[fromB + k];
				}
			}
			i = tillA;
			j = tillB;
		} else {
			const auto left = a[i].toLower();
			const auto right = b[j].toLower();
			if (left != right) {
				return left < right;
			}
			++i;
			++j;
		}
	}
	const auto restA = a.size() - i;
	const auto restB = b.size() - j;
	return (restA != restB) ? (restA < restB) : (a < b);
}

// The files as they are given, every folder replaced with the files of
// it that the queue takes (one level, sorted by the names).
[[nodiscard]] QStringList ExpandPaths(const QStringList &paths) {
	auto result = QStringList();
	for (const auto &path : paths) {
		if (result.size() >= kMaxQueue) {
			break;
		}
		const auto info = QFileInfo(path);
		if (!info.isDir()) {
			result.push_back(path);
			continue;
		}
		const auto dir = QDir(path);
		auto names = QStringList();
		for (const auto &name : dir.entryList(QDir::Files | QDir::Readable)) {
			if (AcceptsExtension(name)) {
				names.push_back(name);
			}
		}
		std::sort(names.begin(), names.end(), NaturalLess);
		for (const auto &name : names) {
			if (result.size() >= kMaxQueue) {
				break;
			}
			result.push_back(dir.absoluteFilePath(name));
		}
	}
	return result;
}

// "FLOOD_WAIT_17" and "FLOOD_PREMIUM_WAIT_5": the seconds to wait. 0 for
// an error that is not about flooding, the default for a flood error
// without a number.
[[nodiscard]] int FloodWaitSeconds(const QString &type) {
	if (!type.startsWith(u"FLOOD"_q)) {
		return 0;
	}
	const auto underscore = type.lastIndexOf('_');
	auto ok = false;
	const auto seconds = type.mid(underscore + 1).toInt(&ok);
	return (ok && seconds > 0)
		? std::min(seconds, kMaxFloodWait)
		: kDefaultFloodWait;
}

[[nodiscard]] bool IsFullError(const QString &type) {
	return type.contains(u"STICKERS_TOO_MUCH"_q);
}

[[nodiscard]] bool IsNameError(const QString &type) {
	return type.contains(u"SHORT_NAME"_q)
		|| type.contains(u"SHORTNAME"_q)
		|| (type == u"PACK_TITLE_INVALID"_q);
}

[[nodiscard]] bool IsTargetError(const QString &type) {
	return (type == u"STICKERSET_INVALID"_q);
}

enum class ItemState : uchar {
	Waiting, // For the converter.
	Converting,
	Ready, // Converted, waits for its turn to be uploaded.
	Invalid, // Can't become a sticker, the reason is in problem.
	Uploading,
	Done,
	Failed, // The upload failed, the error type is in error.

	// Its request was in flight when the box of a run was closed by the
	// app (see SavedQueue): it may be in the set or not. Never uploaded
	// again by itself, that could add the sticker twice.
	Unsure,
};

enum class Problem : uchar {
	None,
	Read, // The file can't be opened or is empty.
	Unknown, // Not an image, an animation or a video.
	Image,
	Lottie, // Not a Lottie or one that Telegram rejects and can't be fixed.
	Video,
};

enum class RunState : uchar {
	Idle,
	Running,
	Paused,
};

// Why the queue is not running.
enum class StopReason : uchar {
	None,
	User, // "Pause".
	Stopped, // "Stop".
	Finished, // Nothing is left to upload.
	Full, // The set takes no more stickers.
	Name, // The short name or the title of the new set is refused.
	Target, // The set doesn't exist.
	Errors, // Several failures in a row.
	Floods, // Several FLOOD_WAIT in a row.
};

// What the converter makes of a file.
struct Converted {
	Prepared prepared; // Not valid: see problem.
	QImage thumbnail;
	Format format = Format::Static;
	bool fromVideo = false; // A video that was cut: can be trimmed by hand.
	bool fixed = false; // An animation fixed automatically.
	Problem problem = Problem::None;
};

struct Item {
	uint64 id = 0;
	QString path;
	QString name; // Of the file, without the extension.
	ItemState state = ItemState::Waiting;
	int revision = 0; // Of the conversion, older results are dropped.

	// Its own emoji: found in the name of the file or chosen by hand.
	// Empty: the default emoji of the queue.
	QString emoji;
	bool emojiFromName = false;

	// Known after the conversion.
	bool converted = false;
	Format format = Format::Static;
	bool fromVideo = false;
	bool fixed = false;
	QSize size;
	crl::time duration = 0;
	int bytes = 0;
	QImage thumbnail;

	Prepared prepared; // Ready, Uploading and Failed.
	Problem problem = Problem::None; // Invalid.
	QString error; // Failed, empty if the file itself wasn't uploaded.
	QByteArray trimmed; // The fragment chosen in the trim box, a WebM.
	int attempts = 0;
	bool restored = false; // Came from a run that was interrupted.
};

struct Counts {
	int total = 0;
	int converting = 0; // Waiting and Converting.
	int ready = 0;
	int uploading = 0;
	int done = 0;
	int failed = 0;
	int invalid = 0;
	int unsure = 0;

	// What is still to be uploaded.
	[[nodiscard]] int pending() const {
		return converting + ready + uploading;
	}
	// Everything that could be a sticker.
	[[nodiscard]] int valid() const {
		return total - invalid;
	}
};

struct Step {
	enum class Type : uchar {
		None, // Nothing to do now: an upload or the converter is awaited.
		Upload, // Upload the item with this id (it is Uploading already).
		Wait, // Call next() again at this time.
		Finished, // The queue has stopped, see Queue::reason().
	};
	Type type = Type::None;
	uint64 id = 0;
	crl::time till = 0;
};

// What is kept of a queue when its box is closed in the middle of a run by
// something that is not the user: the app is locked, a chat is opened from
// a notification, another account is shown. Only values: the box, its
// window and the session may be gone when this is read. The next batch box
// of the account takes it, so the user sees what has got to the set and
// goes on with the rest instead of uploading everything once more.
enum class SavedState : uchar {
	Pending, // Converted and uploaded again.
	Done,
	Failed,
	Invalid,
	Unsure, // Was being added to the set: the set tells whether it is there.
	Unknown, // The same, but the set can't tell anymore: the user checks it.
};

struct SavedItem {
	QString path;
	QString name;
	QString emoji;
	bool emojiFromName = false;
	SavedState state = SavedState::Pending;
	Format format = Format::Static;
	bool fromVideo = false;
	QImage thumbnail; // Not of the Pending ones: they are converted again.
	QByteArray trimmed;
	Problem problem = Problem::None;
	QString error;
};

struct SavedQueue {
	std::vector<SavedItem> items;
	PackKind kind = PackKind::Stickers;
	QString defaultEmoji;
	int packCount = 0; // Without the Unsure item.
	crl::time notBefore = 0; // A FLOOD_WAIT doesn't end with the box.
	crl::time delay = kBaseDelay;
	bool flood = false;
	int sinceBreak = 0;
};

// Has the sticker that was being added when its box was closed got to the
// set: by the number of the stickers in the set before that and now.
enum class Guess : uchar {
	Unknown,
	Added,
	NotAdded,
};

[[nodiscard]] Guess GuessAdded(int before, int now) {
	return (now == before + 1)
		? Guess::Added
		: (now == before)
		? Guess::NotAdded
		: Guess::Unknown;
}

// The queue with all its rules and without a single widget, request or
// clock: the time is given by the caller, so it is checked by the
// self-test as it is.
//
// The items are uploaded strictly in their order (that is the order of
// the stickers in the set), one at a time: next() gives the next one only
// when nothing is being uploaded, the pause after the previous one is over
// and the converter is done with it. A FLOOD_WAIT puts the item back and
// makes next() wait for all the time that was asked, plus a second.
class Queue final {
public:
	explicit Queue(EmojiLength emojiLength = nullptr);

	[[nodiscard]] const std::vector<Item> &items() const {
		return _items;
	}
	[[nodiscard]] const Item *find(uint64 id) const;
	[[nodiscard]] Counts counts() const;

	// 0 if the file is in the queue already or the queue is full.
	uint64 add(const QString &path, QString name = QString());
	bool remove(uint64 id); // Not the one that is being uploaded.
	int clear(); // The same, returns how many are removed.

	// A change sends everything that is not uploaded to the converter
	// again: the files of stickers and of emoji differ.
	[[nodiscard]] PackKind kind() const {
		return _kind;
	}
	bool setKind(PackKind kind);

	// How many stickers the chosen set has.
	[[nodiscard]] int packCount() const {
		return _packCount;
	}
	void setPackCount(int count);

	[[nodiscard]] const QString &defaultEmoji() const {
		return _defaultEmoji;
	}
	bool setDefaultEmoji(const QString &emoji);
	bool setEmoji(uint64 id, const QString &emoji); // Empty: the default.
	[[nodiscard]] QString emojiOf(const Item &item) const;

	// One at a time: null while an item is Converting.
	[[nodiscard]] const Item *nextToConvert();
	bool converted(uint64 id, int revision, Converted &&result);
	bool setTrimmed(uint64 id, QByteArray webm);

	// False if there is nothing to upload.
	bool start();

	// Both let the sticker that is being uploaded get to the set: its
	// request may have reached the server, and an upload that is thrown
	// away and repeated later would add the sticker twice.
	void pause();
	void stop();

	[[nodiscard]] Step next(crl::time now);
	void uploaded(uint64 id, int packCount, crl::time now);
	void failed(uint64 id, const QString &error, crl::time now);

	int retryFailed(); // Every Failed item, returns how many.
	bool retry(uint64 id); // A Failed, an Invalid or an Unsure one.
	void clearReason();

	// A run that is interrupted by the app, see SavedQueue. adding: the
	// request that adds the item being uploaded to the set may have been
	// sent already. restore() fills only an empty queue.
	[[nodiscard]] SavedQueue save(bool adding) const;
	void restore(SavedQueue &&saved);

	// The restored item that may be in the set, 0 when that is decided: by
	// resolve() or by a change after which the set can't tell.
	[[nodiscard]] uint64 unresolved() const {
		return _unresolved;
	}
	void resolve(Guess guess);

	[[nodiscard]] RunState run() const {
		return _run;
	}
	[[nodiscard]] StopReason reason() const {
		return _reason;
	}
	[[nodiscard]] crl::time waitTill() const {
		return _notBefore;
	}
	[[nodiscard]] bool flood() const { // waitTill() is a FLOOD_WAIT.
		return _flood;
	}
	[[nodiscard]] crl::time delay() const {
		return _delay;
	}

private:
	[[nodiscard]] Item *lookup(uint64 id);
	void requeue(Item &item);
	void halt(StopReason reason);

	const EmojiLength _emojiLength;
	std::vector<Item> _items;
	uint64 _lastId = 0;
	PackKind _kind = PackKind::Stickers;
	int _packCount = 0;
	QString _defaultEmoji = DefaultEmoji();

	RunState _run = RunState::Idle;
	StopReason _reason = StopReason::None;
	crl::time _notBefore = 0;
	crl::time _delay = kBaseDelay;
	bool _flood = false;
	int _sinceBreak = 0;
	int _floodsInRow = 0;
	int _failuresInRow = 0;
	uint64 _unresolved = 0;

};

Queue::Queue(EmojiLength emojiLength)
: _emojiLength(std::move(emojiLength)) {
}

const Item *Queue::find(uint64 id) const {
	const auto i = ranges::find(_items, id, &Item::id);
	return (i != end(_items)) ? &*i : nullptr;
}

Item *Queue::lookup(uint64 id) {
	const auto i = ranges::find(_items, id, &Item::id);
	return (i != end(_items)) ? &*i : nullptr;
}

Counts Queue::counts() const {
	auto result = Counts();
	result.total = int(_items.size());
	for (const auto &item : _items) {
		switch (item.state) {
		case ItemState::Waiting:
		case ItemState::Converting: ++result.converting; break;
		case ItemState::Ready: ++result.ready; break;
		case ItemState::Invalid: ++result.invalid; break;
		case ItemState::Uploading: ++result.uploading; break;
		case ItemState::Done: ++result.done; break;
		case ItemState::Failed: ++result.failed; break;
		case ItemState::Unsure: ++result.unsure; break;
		}
	}
	return result;
}

uint64 Queue::add(const QString &path, QString name) {
	if (path.isEmpty() || int(_items.size()) >= kMaxQueue) {
		return 0;
	}
	const auto same = [&](const Item &item) {
		// A file that is uploaded can be added once more. Not one of a run
		// that was interrupted: its files are dropped again to continue
		// it, not to have in the set twice what is there already.
		return (item.path == path)
			&& (item.restored || item.state != ItemState::Done);
	};
	if (ranges::any_of(_items, same)) {
		return 0;
	}
	auto item = Item();
	item.id = ++_lastId;
	item.path = path;
	item.name = name.isEmpty() ? path : std::move(name);
	item.emoji = CleanEmoji(item.name, _emojiLength);
	item.emojiFromName = !item.emoji.isEmpty();
	_items.push_back(std::move(item));
	return _lastId;
}

bool Queue::remove(uint64 id) {
	const auto i = ranges::find(_items, id, &Item::id);
	if (i == end(_items) || i->state == ItemState::Uploading) {
		return false;
	}
	if (_unresolved == id) {
		_unresolved = 0;
	}
	_items.erase(i);
	return true;
}

int Queue::clear() {
	const auto was = int(_items.size());
	_items.erase(
		ranges::remove_if(_items, [](const Item &item) {
			return (item.state != ItemState::Uploading);
		}),
		end(_items));
	_unresolved = 0;
	return was - int(_items.size());
}

void Queue::requeue(Item &item) {
	if (_unresolved == item.id) {
		_unresolved = 0;
	}
	item.state = ItemState::Waiting;
	++item.revision;
	item.prepared = Prepared();
	item.problem = Problem::None;
	item.error = QString();
	item.converted = false;
}

bool Queue::setKind(PackKind kind) {
	if (_kind == kind) {
		return false;
	}
	_kind = kind;
	for (auto &item : _items) {
		if (item.state != ItemState::Done
			&& item.state != ItemState::Uploading) {
			requeue(item);
		}
	}
	return true;
}

void Queue::setPackCount(int count) {
	_packCount = std::max(count, 0);
}

bool Queue::setDefaultEmoji(const QString &emoji) {
	auto cleaned = CleanEmoji(emoji, _emojiLength);
	if (cleaned.isEmpty() || cleaned == _defaultEmoji) {
		return false;
	}
	_defaultEmoji = std::move(cleaned);
	return true;
}

bool Queue::setEmoji(uint64 id, const QString &emoji) {
	const auto item = lookup(id);
	if (!item
		|| item->state == ItemState::Done
		|| item->state == ItemState::Uploading) {
		return false;
	}
	auto cleaned = CleanEmoji(emoji, _emojiLength);
	if (cleaned == _defaultEmoji) {
		// Nothing of its own: it follows the default one from now on.
		cleaned = QString();
	}
	item->emoji = std::move(cleaned);
	item->emojiFromName = false;
	return true;
}

QString Queue::emojiOf(const Item &item) const {
	return item.emoji.isEmpty() ? _defaultEmoji : item.emoji;
}

const Item *Queue::nextToConvert() {
	if (ranges::contains(_items, ItemState::Converting, &Item::state)) {
		return nullptr;
	}
	const auto i = ranges::find(_items, ItemState::Waiting, &Item::state);
	if (i == end(_items)) {
		return nullptr;
	}
	i->state = ItemState::Converting;
	return &*i;
}

bool Queue::converted(uint64 id, int revision, Converted &&result) {
	const auto item = lookup(id);
	if (!item
		|| item->revision != revision
		|| item->state != ItemState::Converting) {
		return false;
	}
	item->converted = true;
	item->format = result.format;
	item->fromVideo = result.fromVideo;
	item->fixed = result.fixed;
	item->thumbnail = std::move(result.thumbnail);
	if (result.prepared.valid()) {
		item->size = result.prepared.size;
		item->duration = result.prepared.duration;
		item->bytes = int(result.prepared.bytes.size());
		item->prepared = std::move(result.prepared);
		item->problem = Problem::None;
		item->state = ItemState::Ready;
	} else {
		item->prepared = Prepared();
		item->problem = (result.problem == Problem::None)
			? Problem::Unknown
			: result.problem;
		item->state = ItemState::Invalid;
	}
	return true;
}

bool Queue::setTrimmed(uint64 id, QByteArray webm) {
	const auto item = lookup(id);
	if (!item
		|| webm.isEmpty()
		|| item->state == ItemState::Done
		|| item->state == ItemState::Uploading) {
		return false;
	}
	item->trimmed = std::move(webm);
	requeue(*item);
	return true;
}

bool Queue::start() {
	if (_run == RunState::Running) {
		return true;
	}
	const auto pending = [](const Item &item) {
		return (item.state == ItemState::Waiting)
			|| (item.state == ItemState::Converting)
			|| (item.state == ItemState::Ready);
	};
	if (!ranges::any_of(_items, pending)) {
		return false;
	}
	_run = RunState::Running;
	_reason = StopReason::None;
	_failuresInRow = 0;
	_floodsInRow = 0;
	return true;
}

void Queue::pause() {
	if (_run == RunState::Running) {
		_run = RunState::Paused;
		_reason = StopReason::User;
	}
}

void Queue::stop() {
	if (_run != RunState::Idle) {
		_run = RunState::Idle;
		_reason = StopReason::Stopped;
	}
}

void Queue::halt(StopReason reason) {
	_run = RunState::Idle;
	_reason = reason;
}

void Queue::clearReason() {
	if (_run == RunState::Idle) {
		_reason = StopReason::None;
	}
}

Step Queue::next(crl::time now) {
	if (_run == RunState::Idle
		|| ranges::contains(_items, ItemState::Uploading, &Item::state)) {
		return {};
	}
	const auto i = ranges::find_if(_items, [](const Item &item) {
		return (item.state == ItemState::Waiting)
			|| (item.state == ItemState::Converting)
			|| (item.state == ItemState::Ready);
	});
	if (i == end(_items)) {
		// A paused queue ends here as well: its last files have failed, are
		// uploaded or removed, and there is nothing left to continue with.
		halt(StopReason::Finished);
		return { .type = Step::Type::Finished };
	} else if (_run != RunState::Running) {
		return {};
	} else if (_packCount >= StickerPacks::MaxInSet(_kind)) {
		halt(StopReason::Full);
		return { .type = Step::Type::Finished };
	} else if (now < _notBefore) {
		return { .type = Step::Type::Wait, .till = _notBefore };
	} else if (i->state != ItemState::Ready) {
		// The converter is busy with it, the items after it wait: the
		// order of the queue is the order of the stickers in the set.
		return {};
	}
	_flood = false;
	_unresolved = 0; // The set changes now, it won't tell about that item.
	i->state = ItemState::Uploading;
	++i->attempts;
	return { .type = Step::Type::Upload, .id = i->id };
}

void Queue::uploaded(uint64 id, int packCount, crl::time now) {
	if (const auto item = lookup(id)) {
		item->state = ItemState::Done;
		item->prepared = Prepared();
		item->trimmed = QByteArray();
		item->error = QString();
	}
	_packCount = (packCount > 0) ? packCount : (_packCount + 1);
	_failuresInRow = 0;
	_floodsInRow = 0;
	_flood = false;
	auto wait = _delay;
	if (++_sinceBreak >= kBurstCount) {
		_sinceBreak = 0;
		wait += kBurstDelay;
	}
	_notBefore = std::max(_notBefore, now + wait);
}

void Queue::failed(uint64 id, const QString &error, crl::time now) {
	const auto item = lookup(id);
	const auto back = [&] {
		if (item) {
			item->state = item->prepared.valid()
				? ItemState::Ready
				: ItemState::Waiting;
		}
	};
	if (const auto seconds = FloodWaitSeconds(error)) {
		// Not a failure of the item: the same one is tried again when
		// all the time that was asked for has passed.
		back();
		_flood = true;
		_notBefore = std::max(
			_notBefore,
			now + seconds * crl::time(1000) + kFloodSlack);
		_delay = std::min(_delay * 2, kMaxDelay);
		if (++_floodsInRow > kMaxFloodsInRow && _run == RunState::Running) {
			_run = RunState::Paused;
			_reason = StopReason::Floods;
		}
		return;
	}
	_flood = false;
	if (IsFullError(error)) {
		back();
		halt(StopReason::Full);
		return;
	} else if (IsNameError(error)) {
		back();
		halt(StopReason::Name);
		return;
	} else if (IsTargetError(error)) {
		back();
		halt(StopReason::Target);
		return;
	}
	if (item) {
		item->state = ItemState::Failed;
		item->error = error;
	}
	_notBefore = std::max(_notBefore, now + _delay);
	if (++_failuresInRow >= kMaxFailuresInRow && _run == RunState::Running) {
		_run = RunState::Paused;
		_reason = StopReason::Errors;
	}
}

int Queue::retryFailed() {
	auto result = 0;
	for (auto &item : _items) {
		if (item.state == ItemState::Failed) {
			if (item.prepared.valid()) {
				item.state = ItemState::Ready;
				item.error = QString();
			} else {
				requeue(item);
			}
			++result;
		}
	}
	return result;
}

bool Queue::retry(uint64 id) {
	const auto item = lookup(id);
	if (!item) {
		return false;
	} else if (item->state == ItemState::Failed && item->prepared.valid()) {
		item->state = ItemState::Ready;
		item->error = QString();
		return true;
	} else if (item->state == ItemState::Failed
		|| item->state == ItemState::Invalid
		|| item->state == ItemState::Unsure) {
		requeue(*item);
		return true;
	}
	return false;
}

SavedQueue Queue::save(bool adding) const {
	auto result = SavedQueue{
		.kind = _kind,
		.defaultEmoji = _defaultEmoji,
		.packCount = _packCount,
		.notBefore = _notBefore,
		.delay = _delay,
		.flood = _flood,
		.sinceBreak = _sinceBreak,
	};
	result.items.reserve(_items.size());
	for (const auto &item : _items) {
		auto saved = SavedItem{
			.path = item.path,
			.name = item.name,
			.emoji = item.emoji,
			.emojiFromName = item.emojiFromName,
			.format = item.format,
			.fromVideo = item.fromVideo,
			.trimmed = item.trimmed,
		};
		switch (item.state) {
		case ItemState::Waiting:
		case ItemState::Converting:
		case ItemState::Ready:
			break;
		case ItemState::Uploading:
			// While only the file was being sent the sticker could not
			// get to the set: it is simply uploaded again.
			if (adding) {
				saved.state = SavedState::Unsure;
			}
			break;
		case ItemState::Done:
			saved.state = SavedState::Done;
			break;
		case ItemState::Failed:
			saved.state = SavedState::Failed;
			saved.error = item.error;
			break;
		case ItemState::Invalid:
			saved.state = SavedState::Invalid;
			saved.problem = item.problem;
			break;
		case ItemState::Unsure:
			saved.state = (item.id == _unresolved)
				? SavedState::Unsure
				: SavedState::Unknown;
			break;
		}
		if (saved.state != SavedState::Pending) {
			saved.thumbnail = item.thumbnail;
		}
		result.items.push_back(std::move(saved));
	}
	return result;
}

void Queue::restore(SavedQueue &&saved) {
	if (!_items.empty()) {
		return;
	}
	_kind = saved.kind;
	auto emoji = CleanEmoji(saved.defaultEmoji, _emojiLength);
	if (!emoji.isEmpty()) {
		_defaultEmoji = std::move(emoji);
	}
	_packCount = std::max(saved.packCount, 0);
	_notBefore = saved.notBefore;
	_delay = std::clamp(saved.delay, kBaseDelay, kMaxDelay);
	_flood = saved.flood;
	_sinceBreak = std::clamp(saved.sinceBreak, 0, kBurstCount - 1);
	_unresolved = 0;
	for (auto &from : saved.items) {
		if (from.path.isEmpty() || int(_items.size()) >= kMaxQueue) {
			continue;
		}
		auto item = Item();
		item.id = ++_lastId;
		item.path = std::move(from.path);
		item.name = from.name.isEmpty() ? item.path : std::move(from.name);
		item.emoji = std::move(from.emoji);
		item.emojiFromName = from.emojiFromName && !item.emoji.isEmpty();
		item.format = from.format;
		item.fromVideo = from.fromVideo;
		item.thumbnail = std::move(from.thumbnail);
		item.trimmed = std::move(from.trimmed);
		item.restored = true;
		switch (from.state) {
		case SavedState::Pending:
			break;
		case SavedState::Done:
			item.state = ItemState::Done;
			item.trimmed = QByteArray();
			break;
		case SavedState::Failed:
			item.state = ItemState::Failed;
			item.error = std::move(from.error);
			break;
		case SavedState::Invalid:
			item.state = ItemState::Invalid;
			item.problem = (from.problem == Problem::None)
				? Problem::Unknown
				: from.problem;
			break;
		case SavedState::Unsure:
			item.state = ItemState::Unsure;
			if (!_unresolved) {
				_unresolved = item.id;
			}
			break;
		case SavedState::Unknown:
			item.state = ItemState::Unsure;
			break;
		}
		_items.push_back(std::move(item));
	}
}

void Queue::resolve(Guess guess) {
	const auto item = lookup(base::take(_unresolved));
	if (!item || item->state != ItemState::Unsure) {
		return;
	} else if (guess == Guess::Added) {
		item->state = ItemState::Done;
		item->trimmed = QByteArray();
	} else if (guess == Guess::NotAdded) {
		requeue(*item);
	}
}

// The converter: everything heavy about one file, off the main thread.
struct ConvertRequest {
	QString path;
	QByteArray content; // Used when path is empty.
	QString name; // The name of the content, for its extension.
	QByteArray trimmed; // A WebM from the trim box, replaces the file.
	PackKind kind = PackKind::Stickers;
	int thumbnail = 0; // The longer side of the thumbnail in pixels.
	VideoCore::Cancel cancel;
};

[[nodiscard]] bool Cancelled(const ConvertRequest &request) {
	return request.cancel && request.cancel->load();
}

[[nodiscard]] QImage FitThumbnail(const QImage &image, int side) {
	if (image.isNull() || side <= 0) {
		return QImage();
	} else if (image.width() <= side && image.height() <= side) {
		return image;
	}
	return image.scaled(
		QSize(side, side),
		Qt::KeepAspectRatio,
		Qt::SmoothTransformation);
}

[[nodiscard]] Converted ConvertImage(
		const QImage &image,
		const ConvertRequest &request) {
	auto result = Converted{ .format = Format::Static };
	if (image.isNull()) {
		result.problem = Problem::Image;
		return result;
	}
	const auto emoji = (request.kind == PackKind::Emoji);
	const auto composed = emoji
		? StickerPacks::ComposeEmoji(image)
		: StickerPacks::ComposeSticker(image);
	result.prepared = StickerPacks::EncodeStatic(
		composed,
		(emoji
			? StickerPacks::kEmojiStaticMaxBytes
			: StickerPacks::kStaticMaxBytes));
	if (result.prepared.valid()) {
		result.thumbnail = FitThumbnail(composed, request.thumbnail);
	} else {
		result.problem = Problem::Image;
	}
	return result;
}

// An animated sticker and an animated emoji are the same file. What the
// validator can fix by itself (the size, the frame rate, the length) is
// fixed, the batch has no place for a question about every file.
[[nodiscard]] Converted ConvertLottie(
		const QByteArray &content,
		const ConvertRequest &request) {
	auto result = Converted{ .format = Format::Animated };
	auto check = StickerPacks::CheckLottie(content);
	if (check.parsed() && !check.acceptable() && check.fixable()) {
		auto fixed = StickerPacks::FixLottie(check);
		if (fixed.acceptable()) {
			check = std::move(fixed);
			result.fixed = true;
		}
	}
	result.prepared = StickerPacks::PrepareAnimated(check);
	if (!result.prepared.valid()) {
		result.problem = Problem::Lottie;
	}
	if (check.parsed() && request.thumbnail > 0) {
		auto renderer = Lottie::Renderer(check.document.toJson());
		const auto &info = renderer.info();
		if (renderer.valid() && info.valid()) {
			// The first frame is often empty, the middle one shows more.
			result.thumbnail = renderer.render(
				info.frames / 2,
				QSize(request.thumbnail, request.thumbnail));
		}
	}
	return result;
}

// The first three seconds of the whole frame. For an emoji the frame is
// centered in the 100x100 square by the encoder.
[[nodiscard]] Converted ConvertVideo(
		const ConvertRequest &request,
		const QString &path,
		const QByteArray &content,
		const Fn<void(float64)> &progress) {
	auto result = Converted{ .format = Format::Video, .fromVideo = true };
	const auto info = VideoCore::ReadClipInfo(path, content);
	if (!info.valid()) {
		result.problem = Problem::Video;
		return result;
	}
	const auto options = VideoCore::VideoStickerOptions{
		.path = path,
		.crop = QRect(QPoint(), info.size),
		.side = StickerPacks::SideFor(request.kind),
		.content = path.isEmpty() ? content : QByteArray(),
	};
	auto made = VideoCore::MakeVideoSticker(options, progress, request.cancel);
	if (!made.ok || made.webm.isEmpty() || made.size.isEmpty()) {
		result.problem = Problem::Video;
		return result;
	}
	result.prepared = Prepared{
		.format = Format::Video,
		.bytes = std::move(made.webm),
		.size = made.size,
		.duration = std::min(made.duration, VideoCore::kStickerMaxDuration),
	};
	return result;
}

// A WebM that may be the sticker or the emoji as it is.
[[nodiscard]] Converted ConvertWebm(
		const ConvertRequest &request,
		const QByteArray &webm,
		const Fn<void(float64)> &progress) {
	const auto check = StickerPacks::CheckVideoFor(request.kind, webm);
	if (!check.ok()) {
		return ConvertVideo(request, QString(), webm, progress);
	}
	auto result = Converted{ .format = Format::Video };
	result.prepared = StickerPacks::PrepareVideo(webm, check);
	if (!result.prepared.valid()) {
		result.problem = Problem::Video;
	}
	return result;
}

[[nodiscard]] Converted ConvertFile(
		const ConvertRequest &request,
		const Fn<void(float64)> &progress) {
	using Kind = StickerPacks::FileKind;
	auto result = Converted();
	auto content = request.content;
	auto size = qint64(content.size());
	auto file = QFile(request.path);
	if (!request.path.isEmpty()) {
		if (!file.open(QIODevice::ReadOnly)) {
			result.problem = Problem::Read;
			return result;
		}
		size = file.size();
		content = file.read(std::min(size, kSniffSize));
	}
	if (size <= 0 || content.isEmpty()) {
		result.problem = Problem::Read;
		return result;
	}
	const auto whole = (content.size() == size);
	const auto kind = StickerPacks::DetectFileKind(
		request.path.isEmpty() ? request.name : request.path,
		content);
	switch (kind) {
	case Kind::Image: {
		file.close();
		result = ConvertImage(
			(request.path.isEmpty()
				? Photo::LoadImage(content)
				: Photo::LoadImage(request.path)),
			request);
	} break;
	case Kind::Lottie: {
		if (!whole && size <= kMaxLottieFile) {
			content.append(file.readAll());
		}
		file.close();
		if (content.size() != size) {
			result.format = Format::Animated;
			result.problem = (size > kMaxLottieFile)
				? Problem::Lottie
				: Problem::Read;
		} else {
			result = ConvertLottie(content, request);
		}
	} break;
	case Kind::VideoSticker:
	case Kind::Video: {
		file.close();
		if (whole && size <= VideoCore::kStickerMaxBytes) {
			// May be ready as it is, for an emoji set too.
			result = request.path.isEmpty()
				? ConvertWebm(request, content, progress)
				: StickerPacks::CheckVideoFor(request.kind, content).ok()
				? ConvertWebm(request, content, progress)
				: ConvertVideo(request, request.path, QByteArray(), progress);
		} else {
			result = ConvertVideo(
				request,
				request.path,
				request.path.isEmpty() ? content : QByteArray(),
				progress);
		}
	} break;
	case Kind::Unknown: {
		result.problem = Problem::Unknown;
	} break;
	}
	return result;
}

// progress (0..1) is called on the calling thread and only for videos.
[[nodiscard]] Converted Convert(
		const ConvertRequest &request,
		const Fn<void(float64)> &progress) {
	auto result = request.trimmed.isEmpty()
		? ConvertFile(request, progress)
		: ConvertWebm(request, request.trimmed, progress);
	if (!request.trimmed.isEmpty()) {
		result.fromVideo = true; // The original can be trimmed again.
	}
	if (Cancelled(request)) {
		return result;
	}
	if (result.prepared.valid()
		&& result.format == Format::Video
		&& request.thumbnail > 0) {
		// A frame of the result itself: exactly what is uploaded.
		result.thumbnail = VideoCore::ReadFrame({
			.content = result.prepared.bytes,
			.size = result.prepared.size.scaled(
				QSize(request.thumbnail, request.thumbnail),
				Qt::KeepAspectRatio).expandedTo(QSize(1, 1)),
		});
	}
	return result;
}

// The texts.

[[nodiscard]] QString FormatPercent(float64 progress) {
	return QString::number(int(std::round(
		std::clamp(progress, 0., 1.) * 100))) + '%';
}

// "0:17", "12:05", "1:02:30": what is left of a FLOOD_WAIT.
[[nodiscard]] QString FormatWait(crl::time left) {
	const auto seconds = int(std::max((left + 999) / 1000, crl::time(0)));
	const auto two = [](int value) {
		return u"%1"_q.arg(value, 2, 10, QChar('0'));
	};
	const auto hours = seconds / 3600;
	const auto minutes = (seconds / 60) % 60;
	return hours
		? (QString::number(hours) + ':' + two(minutes) + ':' + two(seconds % 60))
		: (QString::number(minutes) + ':' + two(seconds % 60));
}

[[nodiscard]] QString FormatSeconds(crl::time duration) {
	auto value = QString::number(duration / 1000., 'f', 1);
	if (value.endsWith(u".0"_q)) {
		value.chop(2);
	}
	if (CurrentLanguageIsRussian()) {
		value.replace('.', ',');
	}
	return tr::lng_oblivion_sbatch_seconds(tr::now, lt_value, value);
}

[[nodiscard]] QString TypeText(Format format) {
	switch (format) {
	case Format::Static: return tr::lng_oblivion_sbatch_type_static(tr::now);
	case Format::Animated:
		return tr::lng_oblivion_sbatch_type_animated(tr::now);
	case Format::Video: return tr::lng_oblivion_sbatch_type_video(tr::now);
	}
	return QString();
}

// compact: without the type, for a row where the whole text doesn't fit.
[[nodiscard]] QString InfoText(const Item &item, bool compact = false) {
	const auto separator = QString::fromUtf8(" \xC2\xB7 ");
	auto parts = QStringList();
	if (!compact) {
		parts.push_back(TypeText(item.format));
	}
	if (item.format != Format::Animated && !item.size.isEmpty()) {
		parts.push_back(QString::fromUtf8("%1\xC3\x97%2").arg(
			item.size.width()
		).arg(item.size.height()));
	}
	if (item.format != Format::Static && item.duration > 0) {
		parts.push_back(FormatSeconds(item.duration));
	}
	if (item.bytes > 0) {
		// The same "6,7 KB" as under the sticker of the "add a sticker" box.
		parts.push_back(LottieEdit::FormatKilobytes(item.bytes));
	}
	if (item.fixed) {
		parts.push_back(tr::lng_oblivion_sbatch_row_fixed(tr::now));
	}
	return parts.join(separator);
}

[[nodiscard]] QString ProblemText(Problem problem) {
	switch (problem) {
	case Problem::Read: return tr::lng_oblivion_sbatch_problem_read(tr::now);
	case Problem::Image:
		return tr::lng_oblivion_sbatch_problem_image(tr::now);
	case Problem::Lottie:
		return tr::lng_oblivion_sbatch_problem_lottie(tr::now);
	case Problem::Video:
		return tr::lng_oblivion_sbatch_problem_video(tr::now);
	case Problem::None:
	case Problem::Unknown: break;
	}
	return tr::lng_oblivion_sbatch_problem_unknown(tr::now);
}

[[nodiscard]] QString ErrorText(const QString &type) {
	if (type.isEmpty()) {
		return tr::lng_oblivion_sbatch_error_upload(tr::now);
	} else if (type.startsWith(u"STICKER_EMOJI"_q)
		|| type.startsWith(u"EMOJI"_q)) {
		return tr::lng_oblivion_sbatch_error_emoji(tr::now);
	} else if (type.startsWith(u"STICKER_"_q)
		|| type.startsWith(u"FILE_"_q)
		|| type.startsWith(u"MEDIA_"_q)
		|| type.startsWith(u"PHOTO_"_q)
		|| type.startsWith(u"VIDEO_"_q)) {
		// The row is narrow: the words come first and the code of the
		// server goes without the beginning that all of them share, so
		// that "TGS_NOTGS" or "VIDEO_BIG" is not cut off.
		const auto common = u"STICKER_"_q;
		return tr::lng_oblivion_sbatch_error_file(
			tr::now,
			lt_error,
			type.startsWith(common) ? type.mid(common.size()) : type);
	}
	return tr::lng_oblivion_sbatch_error_generic(tr::now, lt_error, type);
}

[[nodiscard]] QString PackTitle(const PackInfo &pack) {
	return pack.emoji
		? tr::lng_oblivion_sbatch_target_emoji(
			tr::now,
			lt_title,
			pack.title)
		: pack.title;
}

[[nodiscard]] QString SummaryText(const Counts &counts) {
	auto parts = QStringList();
	if (counts.done > 0) {
		parts.push_back(tr::lng_oblivion_sbatch_status_done(
			tr::now,
			lt_count,
			counts.done));
	}
	if (counts.failed > 0) {
		parts.push_back(tr::lng_oblivion_sbatch_status_failed(
			tr::now,
			lt_count,
			counts.failed));
	}
	if (counts.invalid > 0) {
		parts.push_back(tr::lng_oblivion_sbatch_status_invalid(
			tr::now,
			lt_count,
			counts.invalid));
	}
	return parts.join(' ');
}

[[nodiscard]] std::vector<EmojiPtr> ParseEmoji(const QString &text) {
	auto result = std::vector<EmojiPtr>();
	auto view = QStringView(text);
	while (!view.isEmpty()
		&& int(result.size()) < StickerPacks::kMaxEmoji) {
		auto length = 0;
		const auto emoji = Ui::Emoji::Find(view, &length);
		if (emoji && length > 0) {
			if (!ranges::contains(result, emoji)) {
				result.push_back(emoji);
			}
			view = view.mid(length);
		} else {
			view = view.mid(1);
		}
	}
	return result;
}

// The rows of the settings buttons start two pixels to the left of the
// box rows, these start exactly with the thumbnails of the queue.
[[nodiscard]] const style::SettingsButton &RowButtonStyle() {
	static const auto result = [] {
		auto copy = st::settingsButtonNoIcon;
		copy.padding.setLeft(st::boxRowPadding.left());
		return copy;
	}();
	return result;
}

[[nodiscard]] const style::FlatLabel &CenteredLabelStyle() {
	static const auto result = [] {
		auto copy = st::boxDividerLabel;
		copy.align = style::al_top;
		return copy;
	}();
	return result;
}

// A check mark or an exclamation mark in a circle at the corner of
// a thumbnail.
void PaintBadge(QPainter &p, QRect cell, bool good, const QColor &around) {
	const auto size = style::ConvertScale(kBadgeSize);
	const auto shift = style::ConvertScale(kBadgeShift);
	const auto rect = QRectF(
		cell.x() + cell.width() - size + shift,
		cell.y() + cell.height() - size + shift,
		size,
		size);
	const auto unit = size / 16.;
	const auto center = rect.center();
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(QPen(around, 1.5 * unit));
	p.setBrush(good ? st::boxTextFgGood : st::boxTextFgError);
	p.drawEllipse(rect);

	auto pen = QPen(st::windowFgActive->c, 1.6 * unit);
	pen.setCapStyle(Qt::RoundCap);
	pen.setJoinStyle(Qt::RoundJoin);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	if (good) {
		auto path = QPainterPath();
		path.moveTo(center.x() - 3.4 * unit, center.y() + 0.2 * unit);
		path.lineTo(center.x() - 1.1 * unit, center.y() + 2.5 * unit);
		path.lineTo(center.x() + 3.5 * unit, center.y() - 2.4 * unit);
		p.drawPath(path);
	} else {
		p.drawLine(
			QPointF(center.x(), center.y() - 3.6 * unit),
			QPointF(center.x(), center.y() + 0.8 * unit));
		p.drawPoint(QPointF(center.x(), center.y() + 3.5 * unit));
	}
}

enum class Tone : uchar {
	Normal,
	Active,
	Good,
	Error,
};

enum class Badge : uchar {
	None,
	Done,
	Error,
};

struct RowData {
	QString title;
	QString status;
	QString statusShort; // Shown when status doesn't fit, may be empty.
	Tone tone = Tone::Normal;
	Badge badge = Badge::None;
	QString emoji;
	bool emojiOwn = false; // Not the default one: shown brighter.
	QImage thumbnail;

	friend bool operator==(const RowData &, const RowData &) = default;
};

// One file of the queue: the thumbnail of what will be uploaded, the name
// of the file, what happens to it, its emoji and a menu.
class Row final : public Ui::RippleButton {
public:
	explicit Row(QWidget *parent);

	void setData(RowData data);

	[[nodiscard]] rpl::producer<> menuRequests() const {
		return _menuRequests.events();
	}

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	QImage prepareRippleMask() const override;

private:
	const style::PeerListItem &_st;
	const not_null<Ui::IconButton*> _more;
	RowData _data;

	// The names of the files have emoji too (the files of an exported set
	// are named by them): as a text of the app they look the same as the
	// emoji of the row on every system.
	Ui::Text::String _title;
	Ui::Text::String _emoji;
	rpl::event_stream<> _menuRequests;

};

Row::Row(QWidget *parent)
: RippleButton(parent, st::defaultRippleAnimation)
, _st(st::defaultPeerListItem)
, _more(Ui::CreateChild<Ui::IconButton>(this, st::themesMenuToggle)) {
	_more->setClickedCallback([=] {
		_menuRequests.fire({});
	});
	resize(width(), _st.height);
}

void Row::setData(RowData data) {
	if (_data == data) {
		return;
	}
	if (_data.emoji != data.emoji) {
		_emoji.setText(st::defaultTextStyle, data.emoji);
	}
	if (_data.title != data.title) {
		_title.setText(st::semiboldTextStyle, data.title, kPlainTextOptions);
		setAccessibleName(data.title);
	}
	_data = std::move(data);
	update();
}

int Row::resizeGetHeight(int newWidth) {
	_more->moveToRight(
		std::max(st::boxTitleMenu.width / 2 - _more->width() / 2, 0),
		(_st.height - _more->height()) / 2,
		newWidth);
	return _st.height;
}

void Row::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);

	const auto over = isOver() || isDown();
	const auto &bg = over ? st::windowBgOver : st::windowBg;
	p.fillRect(e->rect(), bg);
	paintRipple(p, 0, 0);

	const auto size = style::ConvertScale(kThumbSize);
	const auto cell = QRect(
		st::boxRowPadding.left(),
		(height() - size) / 2,
		size,
		size);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(over ? st::windowBgRipple : st::windowBgOver);
		const auto radius = float64(st::roundRadiusLarge);
		p.drawRoundedRect(cell, radius, radius);
		if (_data.thumbnail.isNull()) {
			st::menuIconStickers.paintInCenter(p, cell);
		} else {
			const auto inset = style::ConvertScale(kThumbInset);
			const auto inner = cell.marginsRemoved(
				{ inset, inset, inset, inset });
			const auto fitted = _data.thumbnail.size().scaled(
				inner.size(),
				Qt::KeepAspectRatio);
			p.drawImage(
				QRect(
					inner.x() + (inner.width() - fitted.width()) / 2,
					inner.y() + (inner.height() - fitted.height()) / 2,
					fitted.width(),
					fitted.height()),
				_data.thumbnail);
		}
	}
	if (_data.badge != Badge::None) {
		PaintBadge(p, cell, (_data.badge == Badge::Done), bg->c);
	}

	const auto textLeft = cell.x()
		+ cell.width()
		+ style::ConvertScale(kThumbTextSkip);
	const auto emojiWidth = std::min(
		_emoji.maxWidth(),
		style::ConvertScale(kRowEmojiMaxWidth));

	// The emoji end where the round ripple of the menu button starts, not
	// at the edge of the button: its empty side gives the texts more room.
	const auto emojiLeft = _more->x()
		+ st::themesMenuToggle.rippleAreaPosition.x()
		- emojiWidth;
	const auto textWidth = emojiLeft
		- style::ConvertScale(kRowEmojiSkip)
		- textLeft;
	if (textWidth <= 0) {
		return;
	}
	p.setPen(st::contactsNameFg);
	_title.drawLeftElided(
		p,
		textLeft,
		_st.namePosition.y(),
		textWidth,
		width());

	p.setFont(st::normalFont);
	switch (_data.tone) {
	case Tone::Normal:
		p.setPen(over ? st::windowSubTextFgOver : st::windowSubTextFg);
		break;
	case Tone::Active: p.setPen(st::windowActiveTextFg); break;
	case Tone::Good: p.setPen(st::boxTextFgGood); break;
	case Tone::Error: p.setPen(st::boxTextFgError); break;
	}
	const auto &status = (!_data.statusShort.isEmpty()
		&& st::normalFont->width(_data.status) > textWidth)
		? _data.statusShort
		: _data.status;
	p.drawTextLeft(
		textLeft,
		_st.statusPosition.y(),
		width(),
		st::normalFont->elided(status, textWidth));

	if (emojiWidth > 0) {
		p.setPen(st::windowFg);
		p.setOpacity(_data.emojiOwn ? 1. : 0.55);

		// More emoji than fit are cut, what is left stays at the right
		// edge, where the single emoji of the other rows are.
		_emoji.drawLeftElided(
			p,
			emojiLeft,
			(height() - st::defaultTextStyle.font->height) / 2,
			emojiWidth,
			width(),
			1,
			style::al_right);
		p.setOpacity(1.);
	}
}

void Row::contextMenuEvent(QContextMenuEvent *e) {
	e->accept();
	_menuRequests.fire({});
}

QImage Row::prepareRippleMask() const {
	return Ui::RippleAnimation::RectMask(size());
}

class ProgressLine final : public Ui::RpWidget {
public:
	explicit ProgressLine(QWidget *parent) : RpWidget(parent) {
		resize(width(), style::ConvertScale(kProgressHeight));
	}

	void setValue(float64 value) {
		value = std::clamp(value, 0., 1.);
		if (_value != value) {
			_value = value;
			update();
		}
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return style::ConvertScale(kProgressHeight);
	}
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = height() / 2.;
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgRipple);
		p.drawRoundedRect(rect(), radius, radius);
		const auto filled = int(std::round(width() * _value));
		if (filled > 0) {
			p.setBrush(st::windowBgActive);
			p.drawRoundedRect(
				QRect(0, 0, std::max(filled, height()), height()),
				radius,
				radius);
		}
	}

private:
	float64 _value = 0.;

};

// The empty queue: where the files are dropped, a click opens the file
// dialog.
class DropZone final : public Ui::AbstractButton {
public:
	explicit DropZone(QWidget *parent);

	void setHighlighted(bool highlighted);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;

private:
	const QString _title;
	object_ptr<Ui::FlatLabel> _about;
	int _titleTop = 0;
	bool _highlighted = false;

};

DropZone::DropZone(QWidget *parent)
: AbstractButton(parent)
, _title(tr::lng_oblivion_sbatch_drop_title(tr::now))
, _about(
	this,
	tr::lng_oblivion_sbatch_drop_about(tr::now),
	CenteredLabelStyle()) {
	_about->setAttribute(Qt::WA_TransparentForMouseEvents);
	setAccessibleName(_title);
}

void DropZone::setHighlighted(bool highlighted) {
	if (_highlighted != highlighted) {
		_highlighted = highlighted;
		update();
	}
}

int DropZone::resizeGetHeight(int newWidth) {
	const auto padding = style::ConvertScale(kZonePadding);
	const auto skip = style::ConvertScale(kZoneSkip);
	_titleTop = padding + style::ConvertScale(kZoneIconSize) + skip;
	_about->resizeToWidth(std::max(newWidth - 2 * padding, 1));
	const auto aboutTop = _titleTop + st::semiboldFont->height + skip / 2;
	_about->moveToLeft(
		(newWidth - _about->width()) / 2,
		aboutTop,
		newWidth);
	return aboutTop + _about->height() + padding;
}

void DropZone::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);

	const auto line = style::ConvertFloatScale(1.5);
	const auto frame = QRectF(rect()).marginsRemoved(
		{ line, line, line, line });
	const auto radius = float64(st::roundRadiusLarge) * 2;
	const auto active = _highlighted || isOver();
	auto pen = QPen(
		(_highlighted ? st::windowBgActive : st::windowSubTextFg)->c,
		line,
		Qt::DashLine);
	p.setOpacity(_highlighted ? 1. : 0.6);
	p.setPen(pen);
	p.setBrush(active ? st::windowBgOver : st::windowBg);
	p.drawRoundedRect(frame, radius, radius);
	p.setOpacity(1.);

	// A plus in a circle, the same as the "create" buttons of the lists.
	const auto icon = style::ConvertScale(kZoneIconSize);
	const auto circle = QRect(
		(width() - icon) / 2,
		style::ConvertScale(kZonePadding),
		icon,
		icon);
	p.setPen(Qt::NoPen);
	p.setBrush(st::windowBgActive);
	p.drawEllipse(circle);
	st::settingsIconAdd.paintInCenter(p, circle);

	p.setFont(st::semiboldFont);
	p.setPen(st::windowFg);
	p.drawText(
		QRect(0, _titleTop, width(), st::semiboldFont->height),
		Qt::AlignHCenter | Qt::AlignTop,
		_title);
}

// The file the emoji are chosen for, under the bubble of the picker. For
// the emoji of all the files: the first of the files that get them, so
// the place under the bubble is not an empty plate.
class EmojiPreview final : public Ui::RpWidget {
public:
	EmojiPreview(
		QWidget *parent,
		QImage image,
		std::vector<QImage> files,
		QString placeholder)
	: RpWidget(parent)
	, _image(std::move(image))
	, _files(std::move(files))
	, _placeholder(std::move(placeholder)) {
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		const auto radius = float64(st::roundRadiusLarge);
		p.drawRoundedRect(rect(), radius, radius);

		const auto paintFitted = [&](const QImage &image, QRect cell) {
			const auto fitted = image.size().scaled(
				cell.size(),
				Qt::KeepAspectRatio);
			p.drawImage(
				QRect(
					cell.x() + (cell.width() - fitted.width()) / 2,
					cell.y() + (cell.height() - fitted.height()) / 2,
					fitted.width(),
					fitted.height()),
				image);
		};
		if (!_image.isNull()) {
			const auto side = style::ConvertScale(kEmojiPreviewThumb);
			paintFitted(
				_image,
				QRect((width() - side) / 2, (height() - side) / 2, side, side));
			return;
		} else if (_files.empty() && _placeholder.isEmpty()) {
			st::menuIconStickers.paintInCenter(p, rect());
			return;
		} else if (_files.empty()) {
			// Nothing to show yet: the icon and the reason under it, the
			// way the empty lists of the app look.
			const auto &icon = st::menuIconStickers;
			const auto &font = st::normalFont;
			const auto skip = style::ConvertScale(kEmojiPreviewTextSkip);
			const auto top = (height()
				- icon.height()
				- skip
				- font->height) / 2;
			icon.paintInCenter(p, QRect(0, top, width(), icon.height()));
			p.setFont(font);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				QRect(0, top + icon.height() + skip, width(), font->height),
				Qt::AlignHCenter | Qt::AlignTop,
				_placeholder);
			return;
		}

		// One row of up to four files or two rows of about the same length,
		// each of the rows is centered.
		const auto cell = style::ConvertScale(kEmojiPreviewCell);
		const auto skip = style::ConvertScale(kEmojiPreviewCellSkip);
		const auto count = int(_files.size());
		const auto perRow = (count <= 4) ? count : ((count + 1) / 2);
		const auto rows = (count + perRow - 1) / perRow;
		auto top = (height() - rows * cell - (rows - 1) * skip) / 2;
		for (auto row = 0; row != rows; ++row) {
			const auto from = row * perRow;
			const auto inRow = std::min(count - from, perRow);
			auto left = (width() - inRow * cell - (inRow - 1) * skip) / 2;
			for (auto i = 0; i != inRow; ++i) {
				paintFitted(_files[from + i], QRect(left, top, cell, cell));
				left += cell + skip;
			}
			top += cell + skip;
		}
	}

private:
	const QImage _image;
	const std::vector<QImage> _files;
	const QString _placeholder;

};

struct EmojiArgs {
	std::shared_ptr<Ui::Show> show;
	rpl::producer<QString> title;
	QString about;
	QImage preview; // Of the file, null for the emoji of all the files.
	std::vector<QImage> files; // The emoji of all the files: some of them.
	QString placeholder; // Said under the icon when there is no picture.
	QString emoji; // Chosen from the start.
	bool required = false; // Nothing chosen is not accepted.
	Fn<void(QString)> done;
};

// The picker of the "add a sticker" box (and of the app itself), over
// the thumbnail of the file the way that box has it over the sticker.
void EmojiBox(not_null<Ui::GenericBox*> box, EmojiArgs &&args) {
	const auto show = args.show;
	const auto done = std::move(args.done);
	const auto required = args.required;

	box->setTitle(std::move(args.title));
	box->setWidth(st::boxWideWidth);
	box->setCloseByOutsideClick(false);
	const auto inner = box->verticalLayout();

	auto descriptor = ChatHelpers::EmojiPickerOverlayDescriptor{
		.aboutText = args.about,
		.maxSelected = StickerPacks::kMaxEmoji,
		.allowExpand = true,
		.initialSelected = ParseEmoji(args.emoji),
	};
	const auto metrics = ChatHelpers::EmojiPickerOverlay::EstimateMetrics(
		descriptor.aboutText);
	const auto shadow = metrics.shadowExtent;
	const auto side = style::ConvertScale(kEmojiPreviewSide);
	const auto bubbleWidth = [=](int width) {
		return std::min(
			width
				- 2 * st::boxRowPadding.left()
				- shadow.left()
				- shadow.right(),
			style::ConvertScale(kPickerMaxWidth));
	};

	// The metrics are estimated for the about text in a single line,
	// in the bubble it is wrapped: the bubble is that much taller.
	const auto aboutExtra = [&] {
		const auto &padding = st::stickersEmojiPickerPadding;
		auto about = Ui::FlatLabel(
			nullptr,
			descriptor.aboutText,
			st::stickersEmojiPickerAbout);
		const auto single = about.height();
		about.resizeToWidth(std::max(
			bubbleWidth(st::boxWideWidth) - padding.left() - padding.right(),
			1));
		return std::max(about.height() - single, 0);
	}();
	const auto previewTop = shadow.top()
		+ metrics.collapsedHeight
		+ aboutExtra
		+ style::ConvertScale(kEmojiPreviewSkip);
	const auto pickerHeight = metrics.totalExpandedHeight
		+ aboutExtra
		- metrics.tailHeight;

	// The expanded bubble covers the preview, so the preview takes all
	// the place that is kept for it: nothing stays empty under it.
	const auto previewHeight = std::max(side, pickerHeight - previewTop);
	const auto holder = inner->add(
		object_ptr<Ui::FixedHeightWidget>(
			inner,
			previewTop + previewHeight),
		style::margins());
	if (int(args.files.size()) > kEmojiPreviewFiles) {
		args.files.resize(kEmojiPreviewFiles);
	}
	const auto preview = Ui::CreateChild<EmojiPreview>(
		holder,
		std::move(args.preview),
		std::move(args.files),
		std::move(args.placeholder));
	const auto picker = Ui::CreateChild<ChatHelpers::EmojiPickerOverlay>(
		holder,
		std::move(descriptor));
	holder->widthValue(
	) | rpl::on_next([=](int width) {
		const auto &padding = st::boxRowPadding;
		preview->setGeometry(
			padding.left(),
			previewTop,
			std::max(width - padding.left() - padding.right(), side),
			previewHeight);
		const auto total = bubbleWidth(width)
			+ shadow.left()
			+ shadow.right();
		picker->setGeometry((width - total) / 2, 0, total, pickerHeight);
		picker->raise();
	}, holder->lifetime());
	Ui::AddSkip(inner, st::boxLittleSkip);

	box->addButton(tr::lng_oblivion_sbatch_done(), [=] {
		auto emoji = QString();
		for (const auto one : picker->selected()) {
			emoji.append(one->text());
		}
		if (emoji.isEmpty() && required) {
			show->showToast(tr::lng_oblivion_sbatch_emoji_required(tr::now));
			return;
		}
		if (const auto onstack = done) {
			onstack(emoji);
		}
		box->closeBox();
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

// The queue of a box closed in the middle of a run together with where
// its files were going, see SavedQueue. The sets are kept without their
// thumbnails: those belong to the session.
struct SavedRun {
	SavedQueue queue;
	Target target;
	QString retryTitle;
	std::optional<PackKind> retryCreate;
	std::optional<PackInfo> uploadedTo;
};

using SavedRuns = std::vector<SavedRun>; // The latest one is the last.

// By Main::Session::uniqueId(), while the app runs. Main thread only.
[[nodiscard]] base::flat_map<uint64, SavedRuns> &InterruptedRuns() {
	static auto result = base::flat_map<uint64, SavedRuns>();
	return result;
}

[[nodiscard]] std::optional<PackInfo> WithoutThumbnail(
		std::optional<PackInfo> pack) {
	if (pack) {
		pack->thumbnail = nullptr;
	}
	return pack;
}

// A run never takes the place of one that waits, not even of a run into
// the same set: each is the only list of what has got to the set from its
// files. Over the limit the oldest one goes.
void KeepInterruptedRun(uint64 sessionKey, SavedRun &&run) {
	if (!sessionKey) {
		return;
	}
	auto &list = InterruptedRuns()[sessionKey];
	const auto extra = int(list.size()) - (kMaxInterruptedRuns - 1);
	if (extra > 0) {
		list.erase(begin(list), begin(list) + extra);
	}
	list.push_back(std::move(run));
}

// A box opened for a set takes only a run that was filling that very
// set: what is chosen now is not replaced with what was chosen before.
// A box that asks for no set takes any. The latest of the runs that fit
// is taken, the others wait for the next box.
[[nodiscard]] std::optional<SavedRun> TakeInterruptedRun(
		uint64 sessionKey,
		uint64 packId) {
	auto &runs = InterruptedRuns();
	const auto i = sessionKey ? runs.find(sessionKey) : end(runs);
	if (i == end(runs)) {
		return std::nullopt;
	}
	auto &list = i->second;
	auto index = int(list.size()) - 1;
	for (; index >= 0; --index) {
		const auto &pack = list[index].target.pack;
		if (!packId || (pack && pack->id == packId)) {
			break;
		}
	}
	if (index < 0) {
		return std::nullopt;
	}
	auto result = std::move(list[index]);
	list.erase(begin(list) + index);
	if (list.empty()) {
		runs.erase(i);
	}
	return result;
}

// The state of the box that can't be made by the queue alone, for the
// UI snapshots: nothing is converted or uploaded in such a box.
struct Preset {
	Fn<void(Queue &queue, crl::time now)> fill;
	float64 uploadProgress = 0.;
	float64 convertProgress = 0.;
	std::optional<PackInfo> uploadedTo; // "Open pack" is shown.
	std::optional<NewPack> create; // The files go to a set to be created.
	bool restored = false; // The queue is of a run that was interrupted.
};

struct BoxArgs {
	std::shared_ptr<Ui::Show> show;
	BatchBackend backend;
	QStringList paths;
	uint64 packId = 0;
	uint64 sessionKey = 0; // Of the account, for InterruptedRuns().
	std::optional<Preset> preset;
};

// Wider than the usual boxes, but not wider than a narrow window.
[[nodiscard]] int BoxWidth(const std::shared_ptr<Ui::Show> &show) {
	const auto wide = style::ConvertScale(kBoxWidth);
	const auto shadow = st::boxRoundShadow.extend;
	const auto available = show
		? (show->toastParent()->width() - shadow.left() - shadow.right())
		: 0;
	return (available > 0)
		? std::clamp(available, st::boxWideWidth, wide)
		: wide;
}

enum class ButtonsMode : uchar {
	None,
	Close, // Nothing to upload: "Close".
	Upload, // "Upload", "Close".
	Retry, // "Retry", "Close".
	Running, // "Pause", "Stop".
	Paused, // "Continue", "Stop".
};

void BatchBox(not_null<Ui::GenericBox*> box, BoxArgs &&args) {
	struct State : base::has_weak_ptr {
		State() : queue(RealEmojiLength) {
		}
		~State() {
			remember();
			if (convertCancel) {
				convertCancel->store(true);
			}
			if (const auto onstack = base::take(cancelUpload)) {
				onstack();
			}
		}

		// The box is being closed. In the middle of a run and not by the
		// user that is the app itself: it was locked, it shows a chat or
		// another account. The queue is kept for the next box then. Only
		// values are copied, this is called under the passcode screen and
		// while a window is being destroyed as well.
		void remember() {
			const auto key = base::take(sessionKey);
			if (!key || frozen || closedByUser || !runStarted) {
				return;
			}
			const auto counts = queue.counts();
			const auto busy = (queue.run() != RunState::Idle) || uploadingId;
			if (!busy && counts.done >= counts.valid()) {
				return;
			}
			KeepInterruptedRun(key, SavedRun{
				.queue = queue.save(uploadingId && uploadAdding),
				.target = Target{
					.pack = WithoutThumbnail(target.pack),
					.create = target.create,
				},
				.retryTitle = retryTitle,
				.retryCreate = retryCreate,
				.uploadedTo = WithoutThumbnail(uploadedTo),
			});
		}

		Queue queue;
		BatchBackend backend;
		PacksState packs;
		Target target;
		bool targetChosen = false;
		QString retryTitle; // Of a new set with a name that was refused.
		std::optional<PackKind> retryCreate;
		std::optional<PackInfo> uploadedTo;
		bool changed = false; // Uploaded since the last backend.finished().
		bool frozen = false; // The UI snapshots.
		bool dragging = false;

		// See remember().
		uint64 sessionKey = 0;
		bool runStarted = false;
		bool closedByUser = false;
		bool restored = false; // The queue of a run that was interrupted.

		// See kRepeatedClickGuard.
		int clickedButton = -1;
		crl::time clickedAt = 0;

		// The converter: one file at a time.
		bool converterAllowed = false;
		uint64 convertingId = 0;
		float64 convertProgress = 0.;
		VideoCore::Cancel convertCancel;

		// The uploader: one sticker at a time.
		uint64 uploadingId = 0;
		float64 uploadProgress = 0.;
		bool uploadAdding = false;
		Fn<void()> cancelUpload;

		base::flat_map<uint64, not_null<Row*>> rows;
		ButtonsMode buttons = ButtonsMode::None;
		bool buttonsRefreshQueued = false;
		rpl::variable<QString> targetText;
		rpl::variable<QString> emojiText;
		rpl::variable<QString> statusText;
		base::unique_qptr<Ui::PopupMenu> menu;
		base::Timer waitTimer;
		base::Timer tickTimer;
		base::Timer converterTimer;

		Fn<void()> pump;
		Fn<void()> refreshRows;
		Fn<void(uint64)> refreshRow;
		Fn<void()> refreshStatus;
		Fn<void(Target)> applyTarget;
		Fn<void(PackKind, bool)> createNew;
		Fn<void()> start;
		Fn<void()> chooseFiles;
		Fn<void(QStringList)> addPaths;
		Fn<void(uint64)> editEmoji;
		Fn<void(uint64)> showRowMenu;
		Fn<void()> showTargetMenu;
		Fn<void(float64)> convertProgressed;
		Fn<void(uint64, int, Converted&&)> convertDone;
	};
	const auto show = args.show;
	const auto state = box->lifetime().make_state<State>();
	const auto weak = base::make_weak(state);
	state->backend = std::move(args.backend);
	state->frozen = args.preset.has_value();
	state->sessionKey = state->frozen ? 0 : args.sessionKey;
	const auto presetPackId = args.packId;
	const auto thumbPixels = std::clamp(
		style::ConvertScale(kEmojiPreviewThumb) * style::DevicePixelRatio(),
		style::ConvertScale(kThumbSize),
		kThumbMaxPixels);

	box->setTitle(tr::lng_oblivion_sbatch_settings());
	box->setWidth(BoxWidth(show));
	box->setMaxHeight(style::ConvertScale(kBoxMaxHeight));

	// The queue, the emoji and a running upload are not lost by a click
	// past the box.
	box->setCloseByOutsideClick(false);

	// Escape, when the box takes it, is its "Close" button.
	box->events(
	) | rpl::filter([=](not_null<QEvent*> e) {
		return (e->type() == QEvent::KeyPress)
			&& (static_cast<QKeyEvent*>(e.get())->key() == Qt::Key_Escape)
			&& box->closeByEscape();
	}) | rpl::on_next([=] {
		state->closedByUser = !state->uploadingId;

		// The box is closed right by this key press or not by it at all.
		crl::on_main(box, [=] {
			state->closedByUser = false;
		});
	}, box->lifetime());

	const auto top = box->setPinnedToTopContent(
		object_ptr<Ui::VerticalLayout>(box));
	const auto targetButton = ::Settings::AddButtonWithLabel(
		top,
		tr::lng_oblivion_sbatch_target(),
		state->targetText.value(),
		RowButtonStyle());
	const auto emojiButton = ::Settings::AddButtonWithLabel(
		top,
		tr::lng_oblivion_sbatch_emoji_default(),
		state->emojiText.value(),
		RowButtonStyle());

	const auto inner = box->verticalLayout();
	const auto zone = inner->add(
		object_ptr<Ui::SlideWrap<DropZone>>(
			inner,
			object_ptr<DropZone>(inner),
			st::boxRowPadding + QMargins(
				0,
				st::boxLittleSkip,
				0,
				st::boxLittleSkip)),
		style::margins());
	const auto list = inner->add(
		object_ptr<Ui::VerticalLayout>(inner),
		style::margins());

	const auto bottom = box->setPinnedToBottomContent(
		object_ptr<Ui::VerticalLayout>(box));
	const auto progress = bottom->add(
		object_ptr<Ui::SlideWrap<ProgressLine>>(
			bottom,
			object_ptr<ProgressLine>(bottom),
			st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0)),
		style::margins());
	const auto status = bottom->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			bottom,
			object_ptr<Ui::FlatLabel>(
				bottom,
				state->statusText.value(),
				st::boxDividerLabel),
			st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0)),
		style::margins());
	const auto open = bottom->add(
		object_ptr<Ui::SlideWrap<Ui::LinkButton>>(
			bottom,
			object_ptr<Ui::LinkButton>(
				bottom,
				tr::lng_oblivion_packs_open(tr::now),
				st::boxLinkButton),
			st::boxRowPadding + QMargins(0, st::boxLittleSkip / 2, 0, 0)),
		style::margins());
	Ui::AddSkip(bottom, st::boxLittleSkip);
	progress->toggle(false, anim::type::instant);
	status->toggle(false, anim::type::instant);
	open->toggle(false, anim::type::instant);
	open->entity()->setClickedCallback([=] {
		if (state->uploadedTo && state->backend.openPack) {
			state->backend.openPack(*state->uploadedTo);
		}
	});

	const auto busy = [=] {
		return (state->queue.run() != RunState::Idle)
			|| (state->uploadingId != 0);
	};
	const auto rowData = [=](const Item &item) {
		auto result = RowData{
			.title = item.name,
			.emoji = state->queue.emojiOf(item),
			.emojiOwn = !item.emoji.isEmpty(),
			.thumbnail = item.thumbnail,
		};
		switch (item.state) {
		case ItemState::Waiting:
			result.status = tr::lng_oblivion_sbatch_row_waiting(tr::now);
			break;
		case ItemState::Converting:
			result.tone = Tone::Active;
			result.status = (state->convertingId == item.id
				&& state->convertProgress > 0.)
				? tr::lng_oblivion_sbatch_row_converting_percent(
					tr::now,
					lt_percent,
					FormatPercent(state->convertProgress))
				: tr::lng_oblivion_sbatch_row_converting(tr::now);
			break;
		case ItemState::Ready:
			result.status = InfoText(item);
			result.statusShort = InfoText(item, true);
			break;
		case ItemState::Invalid:
			result.tone = Tone::Error;
			result.badge = Badge::Error;
			result.status = ProblemText(item.problem);
			break;
		case ItemState::Unsure:
			if (state->queue.unresolved() == item.id) {
				// The list of the sets is on its way: it tells.
				result.tone = Tone::Active;
				result.status = tr::lng_oblivion_sbatch_row_checking(tr::now);
			} else {
				result.tone = Tone::Error;
				result.badge = Badge::Error;
				result.status = tr::lng_oblivion_sbatch_row_unsure(tr::now);
			}
			break;
		case ItemState::Uploading:
			result.tone = Tone::Active;
			result.status = (state->uploadingId == item.id
				&& state->uploadAdding)
				? tr::lng_oblivion_sbatch_row_adding(tr::now)
				: tr::lng_oblivion_sbatch_row_uploading(
					tr::now,
					lt_percent,
					FormatPercent((state->uploadingId == item.id)
						? state->uploadProgress
						: 0.));
			break;
		case ItemState::Done:
			result.tone = Tone::Good;
			result.badge = Badge::Done;
			result.status = tr::lng_oblivion_sbatch_row_done(tr::now);
			break;
		case ItemState::Failed:
			result.tone = Tone::Error;
			result.badge = Badge::Error;
			result.status = ErrorText(item.error);
			break;
		}
		return result;
	};
	state->refreshRow = [=](uint64 id) {
		const auto i = state->rows.find(id);
		const auto item = state->queue.find(id);
		if (item && i != end(state->rows)) {
			i->second->setData(rowData(*item));
		}
	};
	state->refreshRows = [=] {
		for (auto i = begin(state->rows); i != end(state->rows);) {
			if (state->queue.find(i->first)) {
				++i;
			} else {
				delete i->second.get();
				i = state->rows.erase(i);
			}
		}
		for (const auto &item : state->queue.items()) {
			const auto id = item.id;
			auto i = state->rows.find(id);
			if (i == end(state->rows)) {
				const auto row = list->add(object_ptr<Row>(list));
				row->setClickedCallback([=] {
					// The quick edit of the emoji. A file with a problem
					// needs something else: to be retried or removed.
					const auto item = state->queue.find(id);
					if (item
						&& (item->state == ItemState::Failed
							|| item->state == ItemState::Invalid
							|| item->state == ItemState::Unsure)) {
						state->showRowMenu(id);
					} else {
						state->editEmoji(id);
					}
				});
				row->menuRequests() | rpl::on_next([=] {
					state->showRowMenu(id);
				}, row->lifetime());
				i = state->rows.emplace(id, row).first;
			}
			i->second->setData(rowData(item));
		}
	};

	const auto refreshTarget = [=] {
		const auto &target = state->target;
		state->targetText = target.pack
			? PackTitle(*target.pack)
			: target.create
			? tr::lng_oblivion_sbatch_target_new(
				tr::now,
				lt_title,
				target.create->title)
			: !state->packs.loaded
			? tr::lng_oblivion_sbatch_target_loading(tr::now)
			: tr::lng_oblivion_sbatch_target_choose(tr::now);
	};
	const auto rebuildButtons = [=](ButtonsMode mode) {
		box->clearButtons();

		// place: of the button, from the right. See kRepeatedClickGuard.
		const auto guarded = [=](int place, Fn<void()> callback) {
			return [=] {
				const auto now = crl::now();
				const auto repeated = (state->clickedButton == place)
					&& (now - state->clickedAt < kRepeatedClickGuard);
				state->clickedButton = place;
				state->clickedAt = now;
				if (!repeated) {
					callback();
				}
			};
		};
		const auto close = [=] {
			// With a sticker on its way this cancels its request, nobody
			// knows then whether it is in the set: see State::remember().
			state->closedByUser = !state->uploadingId;
			box->closeBox();
		};
		const auto upload = [=] {
			state->start();
		};
		const auto stop = [=] {
			state->queue.stop();
			state->pump();
		};
		switch (mode) {
		case ButtonsMode::None:
		case ButtonsMode::Close:
			box->addButton(tr::lng_close(), guarded(0, close));
			break;
		case ButtonsMode::Upload:
			box->addButton(
				tr::lng_oblivion_sbatch_upload(),
				guarded(0, upload));
			box->addButton(tr::lng_close(), guarded(1, close));
			break;
		case ButtonsMode::Retry:
			box->addButton(tr::lng_oblivion_sbatch_retry(), guarded(0, [=] {
				state->queue.retryFailed();
				state->start();
			}));
			box->addButton(tr::lng_close(), guarded(1, close));
			break;
		case ButtonsMode::Running:
			box->addButton(tr::lng_oblivion_sbatch_pause(), guarded(0, [=] {
				state->queue.pause();
				state->pump();
			}));
			box->addButton(tr::lng_oblivion_sbatch_stop(), guarded(1, stop));
			break;
		case ButtonsMode::Paused:
			box->addButton(
				tr::lng_oblivion_sbatch_resume(),
				guarded(0, upload));
			box->addButton(tr::lng_oblivion_sbatch_stop(), guarded(1, stop));
			break;
		}
		box->addLeftButton(tr::lng_oblivion_sbatch_add_files(), [=] {
			state->chooseFiles();
		});

		// Escape doesn't throw away a running upload either.
		box->setCloseByEscape(mode != ButtonsMode::Running
			&& mode != ButtonsMode::Paused);
	};
	const auto refreshButtons = [=](const Counts &counts) {
		const auto run = state->queue.run();
		const auto mode = (run == RunState::Running)
			? ButtonsMode::Running
			: (run == RunState::Paused)
			? ButtonsMode::Paused
			: ((counts.converting + counts.ready) > 0)
			? ButtonsMode::Upload
			: (counts.failed > 0)
			? ButtonsMode::Retry
			: ButtonsMode::Close;
		if (state->buttons == mode) {
			return;
		}
		const auto first = (state->buttons == ButtonsMode::None);
		state->buttons = mode;
		if (first) {
			rebuildButtons(mode);
		} else if (!state->buttonsRefreshQueued) {
			// Never from a click on one of the buttons that are replaced.
			state->buttonsRefreshQueued = true;
			crl::on_main(box, [=] {
				state->buttonsRefreshQueued = false;
				rebuildButtons(state->buttons);
			});
		}
	};
	state->refreshStatus = [=] {
		const auto &queue = state->queue;
		const auto counts = queue.counts();
		const auto run = queue.run();
		const auto reason = queue.reason();
		const auto now = crl::now();
		const auto valid = counts.valid();
		const auto processed = counts.done + counts.failed + counts.unsure;
		const auto index = QString::number(
			std::clamp(processed + 1, 1, std::max(valid, 1)));
		const auto total = QString::number(valid);
		const auto done = QString::number(counts.done);
		const auto waiting = (run == RunState::Running)
			&& !counts.uploading
			&& queue.flood()
			&& (now < queue.waitTill());
		const auto uploading = [&] {
			return tr::lng_oblivion_sbatch_status_uploading(
				tr::now,
				lt_index,
				index,
				lt_total,
				total);
		};
		auto text = QString();
		if (state->dragging) {
			text = tr::lng_oblivion_sbatch_status_drop(tr::now);
		} else if (!counts.total) {
		} else if (run == RunState::Running) {
			text = waiting
				? tr::lng_oblivion_sbatch_status_flood(
					tr::now,
					lt_time,
					FormatWait(queue.waitTill() - now))
				: (!counts.uploading && !counts.ready && counts.converting)
				? tr::lng_oblivion_sbatch_status_converting(tr::now)
				: uploading();
		} else if (run == RunState::Paused) {
			text = (reason == StopReason::Errors)
				? tr::lng_oblivion_sbatch_status_errors(tr::now)
				: (reason == StopReason::Floods)
				? tr::lng_oblivion_sbatch_status_floods(tr::now)
				: counts.uploading
				? tr::lng_oblivion_sbatch_status_pausing(tr::now)
				: tr::lng_oblivion_sbatch_status_paused(
					tr::now,
					lt_done,
					done,
					lt_total,
					total);
		} else if (state->restored && counts.done < valid) {
			// Till the user goes on with the queue or clears it. Not when
			// the only file left has turned out to be in the set.
			text = tr::lng_oblivion_sbatch_status_restored(
				tr::now,
				lt_done,
				done,
				lt_total,
				total);
		} else if (reason == StopReason::Full) {
			text = tr::lng_oblivion_sbatch_status_full(
				tr::now,
				lt_max,
				QString::number(StickerPacks::MaxInSet(queue.kind())));
		} else if (reason == StopReason::Name) {
			text = tr::lng_oblivion_sbatch_status_name(tr::now);
		} else if (reason == StopReason::Target) {
			text = tr::lng_oblivion_sbatch_status_target(tr::now);
		} else if (counts.uploading) {
			text = tr::lng_oblivion_sbatch_status_pausing(tr::now);
		} else if (reason == StopReason::Stopped && counts.pending()) {
			text = tr::lng_oblivion_sbatch_status_stopped(
				tr::now,
				lt_done,
				done,
				lt_total,
				total);
		} else if (counts.converting) {
			text = tr::lng_oblivion_sbatch_status_preparing(
				tr::now,
				lt_ready,
				QString::number(counts.total - counts.converting),
				lt_total,
				QString::number(counts.total));
		} else if (counts.ready) {
			text = tr::lng_oblivion_sbatch_status_ready(
				tr::now,
				lt_count,
				counts.ready);
			if (counts.invalid) {
				text += ' ' + tr::lng_oblivion_sbatch_status_invalid(
					tr::now,
					lt_count,
					counts.invalid);
			}
		} else {
			text = SummaryText(counts);
		}
		state->statusText = text;
		status->toggle(!text.isEmpty(), anim::type::instant);

		const auto started = (run != RunState::Idle)
			|| counts.uploading
			|| (processed > 0);
		const auto current = !counts.uploading
			? 0.
			: state->uploadAdding
			? 0.9
			: (0.8 * state->uploadProgress);
		progress->entity()->setValue((valid > 0)
			? ((processed + current) / valid)
			: 0.);
		progress->toggle(started && counts.total, anim::type::instant);
		open->toggle(
			(state->uploadedTo.has_value()
				&& run == RunState::Idle
				&& !counts.uploading
				&& counts.total > 0),
			anim::type::instant);
		zone->toggle(!counts.total, anim::type::instant);
		state->emojiText = queue.defaultEmoji();
		refreshTarget();
		refreshButtons(counts);

		if (waiting && !state->frozen) {
			if (!state->tickTimer.isActive()) {
				state->tickTimer.callEach(kTickInterval);
			}
		} else {
			state->tickTimer.cancel();
		}
	};
	state->tickTimer.setCallback([=] {
		state->refreshStatus();
	});

	// The converter.
	state->convertProgressed = [=](float64 value) {
		state->convertProgress = value;
		state->refreshRow(state->convertingId);
	};
	state->convertDone = [=](uint64 id, int revision, Converted &&result) {
		if (state->convertingId == id) {
			state->convertingId = 0;
			state->convertProgress = 0.;
			state->convertCancel = nullptr;
		}
		state->queue.converted(id, revision, std::move(result));
		state->refreshRow(id);
		state->pump();
	};
	const auto startConvert = [=](const Item &item) {
		const auto id = item.id;
		const auto revision = item.revision;
		state->convertingId = id;
		state->convertProgress = 0.;
		state->convertCancel = std::make_shared<std::atomic<bool>>(false);
		auto request = ConvertRequest{
			.path = item.path,
			.trimmed = item.trimmed,
			.kind = state->queue.kind(),
			.thumbnail = thumbPixels,
			.cancel = state->convertCancel,
		};

		// The worker gets only the data and the weak pointer: nothing
		// that the box holds may be released on its thread.
		crl::async([=, request = std::move(request)] {
			auto reported = 0.;
			auto result = Convert(request, [&](float64 value) {
				if (value < reported + kProgressStep) {
					return;
				}
				reported = value;
				crl::on_main(weak, [=] {
					if (state->convertingId == id) {
						state->convertProgressed(value);
					}
				});
			});
			crl::on_main(weak, [=, result = std::move(result)]() mutable {
				state->convertDone(id, revision, std::move(result));
			});
		});
	};
	const auto cancelConvertOf = [=](uint64 id) {
		if (state->convertingId == id && state->convertCancel) {
			state->convertCancel->store(true);
		}
	};

	// The uploader.
	const auto startUpload = [=](uint64 id) {
		const auto item = state->queue.find(id);
		if (!item || !state->backend.upload || !state->target) {
			// Can't be, but the queue must never be left waiting for
			// an upload that was not started: it stops and says why.
			state->queue.failed(id, u"STICKERSET_INVALID"_q, crl::now());
			return;
		}
		state->uploadingId = id;
		state->uploadProgress = 0.;
		state->uploadAdding = false;
		const auto creating = state->target.create;
		auto handlers = StickerPacks::BatchHandlers{
			.progress = crl::guard(weak, [=](float64 value) {
				if (state->uploadingId == id) {
					state->uploadProgress = value;
					state->refreshRow(id);
					state->refreshStatus();
				}
			}),
			.finishing = crl::guard(weak, [=] {
				if (state->uploadingId == id) {
					state->uploadAdding = true;
					state->refreshRow(id);
					state->refreshStatus();
				}
			}),
			.done = crl::guard(weak, [=](PackInfo pack, bool) {
				state->uploadingId = 0;
				state->cancelUpload = nullptr;
				state->changed = true;
				state->queue.uploaded(id, pack.count, crl::now());

				// A new set exists now, the rest goes into it.
				LastPackId = pack.id;
				state->uploadedTo = pack;
				state->target = Target{ .pack = pack };
				state->targetChosen = true;
				state->retryCreate = std::nullopt;
				state->refreshRow(id);
				state->pump();
			}),
			.fail = crl::guard(weak, [=](QString error) {
				state->uploadingId = 0;
				state->cancelUpload = nullptr;
				state->queue.failed(id, error, crl::now());
				if (state->queue.reason() == StopReason::Name && creating) {
					// The next "Upload" asks for another name. The choice
					// stays with the user: the files are not put into
					// some existing set instead.
					state->retryTitle = creating->title;
					state->retryCreate = creating->emoji
						? PackKind::Emoji
						: PackKind::Stickers;
					state->target = Target();
					state->targetChosen = true;
				} else if (state->queue.reason() == StopReason::Target) {
					state->target = Target();
					state->targetChosen = true;
					if (state->backend.reload) {
						state->backend.reload();
					}
				}
				state->refreshRow(id);
				state->pump();
			}),
		};
		state->cancelUpload = state->backend.upload(
			StickerPacks::BatchUpload{
				.id = id,
				.sticker = item->prepared,
				.emoji = state->queue.emojiOf(*item),
				.target = state->target,
			},
			std::move(handlers));
	};

	state->pump = [=] {
		if (state->frozen) {
			state->refreshStatus();
			return;
		}
		if (!state->convertingId && state->converterAllowed) {
			if (const auto item = state->queue.nextToConvert()) {
				startConvert(*item);
				state->refreshRow(item->id);
			}
		}
		const auto now = crl::now();
		const auto step = state->queue.next(now);
		switch (step.type) {
		case Step::Type::Upload:
			startUpload(step.id);
			state->refreshRow(step.id);
			break;
		case Step::Type::Wait:
			state->waitTimer.callOnce(
				std::max(step.till - now, crl::time(1)));
			break;
		case Step::Type::Finished:
		case Step::Type::None:
			break;
		}

		// Nothing is in flight and nothing will be by itself: the lists
		// of the sets and the sticker panel are refreshed now, once.
		if (state->changed
			&& !state->uploadingId
			&& state->queue.run() != RunState::Running) {
			state->changed = false;
			if (state->backend.finished) {
				state->backend.finished();
			}
		}
		state->refreshStatus();
	};
	state->waitTimer.setCallback([=] {
		state->pump();
	});
	state->converterTimer.setCallback([=] {
		// The list of the sets doesn't come: the files are converted
		// into stickers, and once more if an emoji set is chosen later.
		state->converterAllowed = true;
		state->pump();
	});

	state->applyTarget = [=](Target target) {
		const auto kind = target.pack
			? target.pack->kind()
			: (target.create && target.create->emoji)
			? PackKind::Emoji
			: target.create
			? PackKind::Stickers
			: state->queue.kind();
		const auto count = target.pack ? target.pack->count : 0;
		if (target.pack) {
			LastPackId = target.pack->id;
		}

		// Only the set a sticker was on its way to tells whether it is
		// there, and that set is not the target anymore.
		state->queue.resolve(Guess::Unknown);
		state->target = std::move(target);
		state->targetChosen = true;
		state->retryCreate = std::nullopt;
		if (state->queue.setKind(kind)) {
			// The file that is being converted is for the other kind.
			cancelConvertOf(state->convertingId);
		}
		state->queue.setPackCount(count);
		state->queue.clearReason();
		state->refreshRows();
		state->pump();
	};
	state->createNew = [=](PackKind kind, bool thenStart) {
		if (!state->backend.createPack || busy()) {
			return;
		}
		const auto title = state->retryTitle;
		state->backend.createPack(kind, title, crl::guard(weak, [=](
				NewPack pack) {
			if (busy()) {
				return;
			}
			state->retryTitle = QString();
			state->applyTarget(Target{ .create = std::move(pack) });
			if (thenStart) {
				state->start();
			}
		}));
	};
	state->showTargetMenu = [=] {
		if (busy()) {
			show->showToast(tr::lng_oblivion_sbatch_toast_busy(tr::now));
			return;
		}
		state->menu = base::make_unique_q<Ui::PopupMenu>(
			box,
			st::popupMenuWithIcons);
		const auto menu = state->menu.get();
		const auto addPacks = [&](bool emoji) {
			auto added = false;
			for (const auto &pack : state->packs.list) {
				if (pack.masks || pack.emoji != emoji) {
					continue;
				} else if (!added && !menu->empty()) {
					menu->addSeparator();
				}
				added = true;
				const auto action = Ui::Menu::CreateAction(
					menu,
					PackTitle(pack),
					[=] { state->applyTarget(Target{ .pack = pack }); });
				menu->addAction(base::make_unique_q<Menu::ActionWithThumbnail>(
					menu->menu(),
					menu->menu()->st(),
					action,
					pack.thumbnail ? pack.thumbnail->clone() : nullptr,
					st::menuIconStickerAdd.width()));
			}
		};
		addPacks(false);
		addPacks(true);
		if (state->backend.createPack) {
			if (!menu->empty()) {
				menu->addSeparator();
			}
			menu->addAction(
				tr::lng_oblivion_sbatch_menu_new_stickers(tr::now),
				[=] { state->createNew(PackKind::Stickers, false); },
				&st::menuIconStickerAdd);
			menu->addAction(
				tr::lng_oblivion_sbatch_menu_new_emoji(tr::now),
				[=] { state->createNew(PackKind::Emoji, false); },
				&st::menuIconEmoji);
		}
		if (menu->empty()) {
			state->menu = nullptr;
		} else {
			menu->popup(QCursor::pos());
		}
	};
	targetButton->setClickedCallback([=] {
		state->showTargetMenu();
	});

	state->start = [=] {
		if (state->frozen) {
			return;
		}
		const auto counts = state->queue.counts();
		if (state->queue.unresolved()) {
			// The list of the sets tells whether the sticker that was on
			// its way when the last box was closed is in the set. Another
			// sticker added before that would hide the answer.
			show->showToast(tr::lng_oblivion_sbatch_toast_loading(tr::now));
			return;
		} else if (!(counts.converting + counts.ready)) {
			show->showToast(tr::lng_oblivion_sbatch_toast_nothing(tr::now));
			return;
		} else if (!state->target) {
			const auto usable = [](const PackInfo &pack) {
				return !pack.masks;
			};
			if (state->retryCreate) {
				state->createNew(*state->retryCreate, true);
			} else if (!state->packs.loaded) {
				show->showToast(
					tr::lng_oblivion_sbatch_toast_loading(tr::now));
			} else if (ranges::none_of(state->packs.list, usable)) {
				state->createNew(state->queue.kind(), true);
			} else {
				show->showToast(
					tr::lng_oblivion_sbatch_toast_target(tr::now));
				state->showTargetMenu();
			}
			return;
		}
		if (state->queue.start()) {
			state->runStarted = true;
			state->restored = false;
		}
		state->pump();
	};

	// The emoji.
	const auto editDefaultEmoji = [=] {
		// The files that get these emoji: the ones without their own that
		// are not in the set yet.
		auto files = std::vector<QImage>();
		for (const auto &item : state->queue.items()) {
			if (int(files.size()) >= kEmojiPreviewFiles) {
				break;
			} else if (item.emoji.isEmpty()
				&& item.state != ItemState::Done
				&& !item.thumbnail.isNull()) {
				files.push_back(item.thumbnail);
			}
		}
		show->showBox(Box(EmojiBox, EmojiArgs{
			.show = show,
			.title = tr::lng_oblivion_sbatch_emoji_default_title(),
			.about = tr::lng_oblivion_sbatch_emoji_default_about(tr::now),
			.files = std::move(files),
			.placeholder = (state->queue.items().empty()
				? tr::lng_oblivion_sbatch_emoji_default_empty(tr::now)
				: QString()),
			.emoji = state->queue.defaultEmoji(),
			.required = true,
			.done = crl::guard(weak, [=](QString emoji) {
				if (state->queue.setDefaultEmoji(emoji)) {
					state->refreshRows();
					state->refreshStatus();
				}
			}),
		}));
	};
	emojiButton->setClickedCallback(editDefaultEmoji);
	state->editEmoji = [=](uint64 id) {
		const auto item = state->queue.find(id);
		if (!item
			|| item->state == ItemState::Done
			|| item->state == ItemState::Uploading
			|| state->frozen) {
			return;
		}
		show->showBox(Box(EmojiBox, EmojiArgs{
			.show = show,
			.title = tr::lng_oblivion_sbatch_emoji_title(),
			.about = tr::lng_oblivion_sbatch_emoji_about(tr::now),
			.preview = item->thumbnail,
			.emoji = state->queue.emojiOf(*item),
			.done = crl::guard(weak, [=](QString emoji) {
				if (state->queue.setEmoji(id, emoji)) {
					state->refreshRow(id);
				}
			}),
		}));
	};

	state->showRowMenu = [=](uint64 id) {
		const auto item = state->queue.find(id);
		if (!item) {
			return;
		}
		const auto itemState = item->state;
		const auto editable = (itemState != ItemState::Done)
			&& (itemState != ItemState::Uploading);
		state->menu = base::make_unique_q<Ui::PopupMenu>(
			box,
			st::popupMenuWithIcons);
		const auto menu = state->menu.get();
		const auto addAction = Ui::Menu::CreateAddActionCallback(menu);
		if (editable) {
			addAction(
				tr::lng_oblivion_sbatch_menu_emoji(tr::now),
				[=] { state->editEmoji(id); },
				&st::menuIconEmoji);
			if (!item->emoji.isEmpty()) {
				addAction(
					tr::lng_oblivion_sbatch_menu_emoji_reset(tr::now),
					[=] {
						if (state->queue.setEmoji(id, QString())) {
							state->refreshRow(id);
						}
					},
					&st::menuIconRestore);
			}
		}
		if (editable
			&& item->fromVideo
			&& !item->path.isEmpty()
			&& !state->frozen) {
			const auto path = item->path;
			addAction(
				tr::lng_oblivion_sbatch_menu_trim(tr::now),
				[=] {
					ShowVideoStickerTrim(
						show,
						path,
						QByteArray(),
						crl::guard(weak, [=](QByteArray webm) {
							if (state->queue.setTrimmed(id, std::move(webm))) {
								cancelConvertOf(id);
								state->refreshRow(id);
								state->pump();
							}
						}));
				},
				&st::menuIconEdit);
		}
		if (itemState == ItemState::Failed
			|| itemState == ItemState::Invalid
			|| itemState == ItemState::Unsure) {
			addAction(
				tr::lng_oblivion_sbatch_menu_retry(tr::now),
				[=] {
					if (state->queue.retry(id)) {
						state->refreshRow(id);
						state->pump();
					}
				},
				&st::menuIconRestore);
		}
		if (itemState != ItemState::Uploading) {
			addAction(
				tr::lng_oblivion_sbatch_menu_remove(tr::now),
				[=] {
					cancelConvertOf(id);
					if (state->queue.remove(id)) {
						state->refreshRows();
						state->pump();
					}
				},
				&st::menuIconRemove);
		}
		if (!busy() && state->queue.items().size() > 1) {
			addAction({
				.text = tr::lng_oblivion_sbatch_menu_clear(tr::now),
				.handler = [=] {
					cancelConvertOf(state->convertingId);
					state->queue.clear();
					state->queue.clearReason();
					state->restored = false;
					state->refreshRows();
					state->pump();
				},
				.icon = &st::menuIconDeleteAttention,
				.isAttention = true,
			});
		}
		if (menu->empty()) {
			state->menu = nullptr;
		} else {
			menu->popup(QCursor::pos());
		}
	};

	// The files.
	state->addPaths = [=](QStringList paths) {
		auto added = 0;
		auto skipped = 0;
		auto full = false;
		for (const auto &path : ExpandPaths(paths)) {
			if (!AcceptsExtension(path)) {
				++skipped;
			} else if (int(state->queue.items().size()) >= kMaxQueue) {
				full = true;
				break;
			} else if (state->queue.add(
					path,
					QFileInfo(path).completeBaseName())) {
				++added;
			}
		}
		if (full) {
			show->showToast(tr::lng_oblivion_sbatch_toast_limit(
				tr::now,
				lt_max,
				QString::number(kMaxQueue)));
		} else if (skipped > 0) {
			show->showToast(
				tr::lng_oblivion_sbatch_toast_skipped(
					tr::now,
					lt_count,
					skipped),
				kToastDuration);
		}
		if (added > 0) {
			state->refreshRows();
			state->pump();
		}
	};
	state->chooseFiles = [=] {
		if (state->frozen) {
			return;
		}
		FileDialog::GetOpenPaths(
			box.get(),
			tr::lng_oblivion_sbatch_choose_files(tr::now),
			DialogFilter(),
			crl::guard(box, [=](FileDialog::OpenResult &&result) {
				state->addPaths(result.paths);
			}));
	};
	zone->entity()->setClickedCallback([=] {
		state->chooseFiles();
	});

	box->setAcceptDrops(true);
	const auto setDragging = [=](bool dragging) {
		zone->entity()->setHighlighted(dragging);
		if (state->dragging != dragging) {
			state->dragging = dragging;
			state->refreshStatus();
		}
	};
	base::install_event_filter(box, [=](not_null<QEvent*> e) {
		using Result = base::EventFilterResult;
		const auto type = e->type();
		if (type == QEvent::DragEnter || type == QEvent::DragMove) {
			const auto drag = static_cast<QDragMoveEvent*>(e.get());
			if (state->frozen || !MimeHasFiles(drag->mimeData())) {
				setDragging(false);
				drag->ignore();
			} else {
				setDragging(true);
				drag->setDropAction(Qt::CopyAction);
				drag->accept();
			}
			return Result::Cancel;
		} else if (type == QEvent::DragLeave) {
			setDragging(false);
			return Result::Cancel;
		} else if (type == QEvent::Drop) {
			const auto drop = static_cast<QDropEvent*>(e.get());
			setDragging(false);
			const auto paths = state->frozen
				? QStringList()
				: PathsFromMime(drop->mimeData());
			if (paths.isEmpty()) {
				drop->ignore();
			} else {
				drop->setDropAction(Qt::CopyAction);
				drop->accept();
				state->addPaths(paths);
			}
			return Result::Cancel;
		}
		return Result::Continue;
	});

	// What a run that was interrupted has left: the queue is as it was,
	// with the files that are in the set and the ones that are not.
	if (auto resumed = TakeInterruptedRun(state->sessionKey, presetPackId)) {
		state->target = std::move(resumed->target);
		state->targetChosen = true;
		state->retryTitle = std::move(resumed->retryTitle);
		state->retryCreate = resumed->retryCreate;
		state->uploadedTo = std::move(resumed->uploadedTo);
		state->queue.restore(std::move(resumed->queue));
		state->runStarted = true;
		state->restored = true;
	}

	// A sticker was on its way to the set when that run was interrupted.
	// The fresh list of the sets tells whether it has got there: by the
	// number of the stickers in the set, or by the new set being there.
	const auto resolveUnsure = [=](const PacksState &packs) {
		const auto id = state->queue.unresolved();
		auto guess = Guess::Unknown;
		if (packs.failed) {
		} else if (const auto &pack = state->target.pack) {
			const auto i = ranges::find(packs.list, pack->id, &PackInfo::id);
			if (i != end(packs.list)) {
				guess = GuessAdded(state->queue.packCount(), i->count);
				if (guess == Guess::Added) {
					state->uploadedTo = *i;
				}
			}
		} else if (const auto create = state->target.create) {
			const auto made = [&](const PackInfo &own) {
				return !own.masks
					&& (own.emoji == create->emoji)
					&& !own.shortName.compare(
						create->shortName,
						Qt::CaseInsensitive);
			};
			const auto i = ranges::find_if(packs.list, made);
			if (i == end(packs.list)) {
				guess = Guess::NotAdded;
			} else {
				// The set was made, only the answer never came: the rest
				// goes into it, and it gets to the panel of the account
				// the way every new set does.
				guess = Guess::Added;
				LastPackId = i->id;
				state->target = Target{ .pack = *i };
				state->uploadedTo = *i;
				state->queue.setPackCount(i->count);
				if (state->backend.install) {
					state->backend.install(*i);
				}
			}
		}
		state->queue.resolve(guess);
		state->refreshRow(id);
	};

	// The sets of the account.
	if (state->backend.packs) {
		state->backend.packs(
		) | rpl::on_next([=](const PacksState &packs) {
			state->packs = packs;
			if (packs.loaded && state->queue.unresolved()) {
				resolveUnsure(packs);
			}
			const auto find = [&](uint64 id) -> const PackInfo* {
				const auto i = ranges::find(packs.list, id, &PackInfo::id);
				return (i != end(packs.list) && !i->masks) ? &*i : nullptr;
			};
			if (state->target.create) {
			} else if (state->targetChosen) {
				const auto fresh = state->target.pack
					? find(state->target.pack->id)
					: nullptr;
				if (fresh) {
					state->target.pack = *fresh;
					if (!busy()) {
						state->queue.setPackCount(fresh->count);
					}
				}
			} else if (packs.loaded) {
				const auto preset = presetPackId ? find(presetPackId) : nullptr;
				const auto last = LastPackId ? find(LastPackId) : nullptr;
				const auto first = ranges::find_if(
					packs.list,
					&PackInfo::regular);
				const auto chosen = preset
					? preset
					: last
					? last
					: (first != end(packs.list))
					? &*first
					: nullptr;
				if (chosen) {
					state->applyTarget(Target{ .pack = *chosen });
				}
			}
			if (packs.loaded && !state->converterAllowed) {
				state->converterAllowed = true;
				state->converterTimer.cancel();
			}
			state->pump();
		}, box->lifetime());
	}

	// The box is being closed, by the user or by the app (it was locked,
	// it shows a chat or another account): a run that is not over is kept
	// for the next box and what was put off is done now. Not from the
	// destructor of the state, that may be a window or an account being
	// destroyed.
	box->boxClosing(
	) | rpl::on_next([=] {
		state->remember();
		if (base::take(state->changed) && state->backend.finished) {
			state->backend.finished();
		}
	}, box->lifetime());

	if (const auto &preset = args.preset) {
		if (preset->create) {
			state->target = Target{ .create = *preset->create };
			state->targetChosen = true;
			state->queue.setPackCount(0);
		}
		if (preset->fill) {
			preset->fill(state->queue, crl::now());
		}
		state->uploadProgress = preset->uploadProgress;
		state->convertProgress = preset->convertProgress;
		state->uploadedTo = preset->uploadedTo;
		state->restored = preset->restored;
		for (const auto &item : state->queue.items()) {
			if (item.state == ItemState::Uploading) {
				state->uploadingId = item.id;
			} else if (item.state == ItemState::Converting) {
				state->convertingId = item.id;
			}
		}
		state->refreshRows();
		state->refreshStatus();
		return;
	}

	state->converterTimer.callOnce(kConverterStartDelay);
	if (state->backend.reload) {
		state->backend.reload();
	}
	state->refreshRows();
	state->refreshStatus();
	if (!args.paths.isEmpty()) {
		state->addPaths(std::move(args.paths));
	}
}

// Snapshot scenes (OBLIVION_SELFTEST=ui), see oblivion_ui_snapshots.h.

[[nodiscard]] QImage SampleSticker(QColor body, QColor head, int side) {
	auto image = QImage(
		QSize(360, 360),
		QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	{
		auto p = QPainter(&image);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(body);
		p.drawRoundedRect(QRect(80, 170, 200, 190), 80, 80);
		p.setBrush(head);
		p.drawEllipse(QPoint(180, 120), 100, 100);
		p.setBrush(QColor(0x2d, 0x1f, 0x1a));
		p.drawEllipse(QPoint(145, 108), 11, 14);
		p.drawEllipse(QPoint(215, 108), 11, 14);
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(QColor(0x2d, 0x1f, 0x1a), 8, Qt::SolidLine, Qt::RoundCap));
		p.drawArc(QRect(140, 120, 80, 52), 200 * 16, 140 * 16);
	}
	return image.scaled(
		QSize(side, side),
		Qt::KeepAspectRatio,
		Qt::SmoothTransformation);
}

class SampleThumbnail final : public Ui::DynamicImage {
public:
	explicit SampleThumbnail(QImage image) : _image(std::move(image)) {
	}

	std::shared_ptr<DynamicImage> clone() override {
		return std::make_shared<SampleThumbnail>(_image);
	}
	QImage image(int size) override {
		const auto ratio = style::DevicePixelRatio();
		auto result = _image.scaled(
			QSize(size, size) * ratio,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
		result.setDevicePixelRatio(ratio);
		return result;
	}
	void subscribeToUpdates(Fn<void()> callback) override {
	}

private:
	const QImage _image;

};

[[nodiscard]] PacksState SamplePacks() {
	auto result = PacksState{ .loaded = true };
	result.list.push_back(PackInfo{
		.id = 1,
		.title = u"Коты Oblivion"_q,
		.shortName = u"oblivion_cats"_q,
		.count = 24,
		.thumbnail = std::make_shared<SampleThumbnail>(SampleSticker(
			QColor(0x6c, 0x5c, 0xe7),
			QColor(0xff, 0xd7, 0xa8),
			kThumbMaxPixels)),
	});
	result.list.push_back(PackInfo{
		.id = 2,
		.title = u"Мини-коты"_q,
		.shortName = u"my_tiny_emoji"_q,
		.count = 41,
		.emoji = true,
		.thumbnail = std::make_shared<SampleThumbnail>(SampleSticker(
			QColor(0x09, 0x84, 0xe3),
			QColor(0xdf, 0xe6, 0xe9),
			kThumbMaxPixels)),
	});
	return result;
}

struct SampleFile {
	QString path;
	Format format = Format::Static;
	QSize size;
	crl::time duration = 0;
	int bytes = 0;
	QColor body;
	QColor head;
	bool fromVideo = false;
	bool fixed = false;
	Problem problem = Problem::None;
};

[[nodiscard]] Converted SampleConverted(const SampleFile &file) {
	auto result = Converted{
		.format = file.format,
		.fromVideo = file.fromVideo,
		.fixed = file.fixed,
		.problem = file.problem,
	};
	if (file.problem == Problem::None) {
		result.prepared = Prepared{
			.format = file.format,
			.bytes = QByteArray(file.bytes, 'x'),
			.size = file.size,
			.duration = file.duration,
		};
		result.thumbnail = SampleSticker(
			file.body,
			file.head,
			kThumbMaxPixels);
	}
	return result;
}

// Adds the files and converts the first ones of them, the rest stays
// in the line for the converter: the next one "is being converted".
std::vector<uint64> FillSample(
		Queue &queue,
		const std::vector<SampleFile> &files,
		int convert) {
	auto ids = std::vector<uint64>();
	for (const auto &file : files) {
		ids.push_back(queue.add(
			file.path,
			QFileInfo(file.path).completeBaseName()));
	}
	for (auto i = 0; i != int(files.size()); ++i) {
		const auto item = queue.nextToConvert();
		if (!item || i >= convert) {
			break;
		}
		queue.converted(item->id, item->revision, SampleConverted(files[i]));
	}
	return ids;
}

[[nodiscard]] std::vector<SampleFile> SampleFiles(bool emoji) {
	const auto side = emoji ? 100 : 512;
	const auto party = QString::fromUtf8("\xF0\x9F\x8E\x89");
	return {
		{
			.path = u"/stickers/кот в очках.png"_q,
			.size = QSize(side, side),
			.bytes = emoji ? 6200 : 48200,
			.body = QColor(0x6c, 0x5c, 0xe7),
			.head = QColor(0xff, 0xd7, 0xa8),
		},
		{
			.path = u"/stickers/праздник "_q + party + u".webp"_q,
			.size = emoji ? QSize(100, 100) : QSize(512, 384),
			.bytes = emoji ? 5100 : 37900,
			.body = QColor(0x00, 0x9e, 0x8e),
			.head = QColor(0xf2, 0xa1, 0x6b),
		},
		{
			.path = u"/stickers/танец.tgs"_q,
			.format = Format::Animated,
			.size = QSize(512, 512),
			.duration = 3000,
			.bytes = 18400,
			.body = QColor(0xe1, 0x70, 0x55),
			.head = QColor(0xff, 0xea, 0xa7),
			.fixed = true,
		},
		{
			.path = u"/stickers/отчёт за квартал.json"_q,
			.format = Format::Animated,
			.problem = Problem::Lottie,
		},
		{
			.path = u"/stickers/прыжок.mp4"_q,
			.format = Format::Video,
			.size = emoji ? QSize(100, 100) : QSize(512, 288),
			.duration = 3000,
			.bytes = emoji ? 41300 : 214000,
			.body = QColor(0x09, 0x84, 0xe3),
			.head = QColor(0xdf, 0xe6, 0xe9),
			.fromVideo = true,
		},
		{
			.path = u"/stickers/очень длинное название файла с отпуска "_q
				+ u"в горах прошлым летом.mov"_q,
			.format = Format::Video,
			.size = emoji ? QSize(100, 100) : QSize(512, 512),
			.duration = 2400,
			.bytes = emoji ? 38800 : 187000,
			.body = QColor(0xd6, 0x30, 0x31),
			.head = QColor(0xfa, 0xb1, 0xa0),
			.fromVideo = true,
		},
	};
}

[[nodiscard]] BatchBackend SampleBackend(PacksState packs) {
	return {
		.packs = [=] {
			return rpl::single(packs);
		},
		.reload = [] {},
		.createPack = [](PackKind, QString, Fn<void(NewPack)>) {},
		.upload = [](StickerPacks::BatchUpload, StickerPacks::BatchHandlers) {
			return Fn<void()>();
		},
		.finished = [] {},
		.openPack = [](PackInfo) {},
	};
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;
	const auto size = QSize(style::ConvertScale(kSceneWidth), 0);
	const auto scene = [=](
			const QString &name,
			uint64 packId,
			Preset preset,
			PacksState packs = SamplePacks()) {
		RegisterBoxScene(name, size, [=](std::shared_ptr<Ui::Show> show) {
			return Box(BatchBox, BoxArgs{
				.show = show,
				.backend = SampleBackend(packs),
				.packId = packId,
				.preset = preset,
			});
		});
	};

	// Nothing is added yet: where to drop the files.
	scene(u"sticker_batch_empty"_q, 1, Preset());

	// The first run on an account without sets of its own.
	scene(
		u"sticker_batch_empty_no_packs"_q,
		0,
		Preset(),
		PacksState{ .loaded = true });

	// Images, an animation fixed automatically, a file that is refused,
	// a video that is being converted and one that waits for it.
	scene(u"sticker_batch_queue"_q, 1, Preset{
		.fill = [](Queue &queue, crl::time) {
			const auto ids = FillSample(queue, SampleFiles(false), 4);
			queue.setEmoji(ids[2], QString::fromUtf8(
				"\xF0\x9F\x92\x83\xF0\x9F\x95\xBA\xF0\x9F\x8E\xB6"));
		},
		.convertProgress = 0.4,
	});

	// The same files for a custom emoji set: everything is 100x100.
	scene(u"sticker_batch_queue_emoji"_q, 2, Preset{
		.fill = [](Queue &queue, crl::time) {
			queue.setKind(PackKind::Emoji);
			queue.setDefaultEmoji(QString::fromUtf8("\xF0\x9F\x98\xBA"));
			FillSample(queue, SampleFiles(true), 6);
		},
	});

	// The upload: two are in the set, one failed, one is on its way.
	scene(u"sticker_batch_uploading"_q, 1, Preset{
		.fill = [](Queue &queue, crl::time now) {
			auto files = SampleFiles(false);
			files.erase(begin(files) + 3);
			FillSample(queue, files, 5);
			queue.setPackCount(24);
			queue.start();
			auto time = now;
			const auto upload = [&] {
				time += kMaxDelay + kBurstDelay;
				return queue.next(time).id;
			};
			queue.uploaded(upload(), 25, time);
			queue.uploaded(upload(), 26, time);
			queue.failed(upload(), u"STICKER_TGS_NOTGS"_q, time);
			[[maybe_unused]] const auto current = upload();
		},
		.uploadProgress = 0.6,
	});

	// Telegram asked to wait: the countdown, nothing is sent.
	scene(u"sticker_batch_flood"_q, 1, Preset{
		.fill = [](Queue &queue, crl::time now) {
			auto files = SampleFiles(false);
			files.erase(begin(files) + 3);
			FillSample(queue, files, 5);
			queue.setPackCount(24);
			queue.start();
			queue.uploaded(queue.next(now).id, 25, now - kMaxDelay);
			queue.failed(queue.next(now).id, u"FLOOD_WAIT_17"_q, now);
		},
	});

	// The queue is done: what was added, what was not, the link.
	scene(u"sticker_batch_finished"_q, 1, Preset{
		.fill = [](Queue &queue, crl::time now) {
			FillSample(queue, SampleFiles(false), 6);
			queue.setPackCount(24);
			queue.start();
			auto time = now;
			auto count = 24;
			for (auto i = 0; i != 5; ++i) {
				time += kMaxDelay + kBurstDelay;
				const auto step = queue.next(time);
				if (step.type != Step::Type::Upload) {
					break;
				} else if (i == 2) {
					queue.failed(step.id, QString(), time);
				} else {
					queue.uploaded(step.id, ++count, time);
				}
			}
			time += kMaxDelay + kBurstDelay;
			[[maybe_unused]] const auto last = queue.next(time);
		},
		.uploadedTo = SamplePacks().list.front(),
	});

	// Three uploads in a row have failed: the queue has paused itself and
	// says what to do. The files go to a set that doesn't exist yet, its
	// long title has to be cut in the row of the set. The last file has
	// more emoji than its row shows.
	scene(u"sticker_batch_paused"_q, 0, Preset{
		.fill = [](Queue &queue, crl::time now) {
			auto files = SampleFiles(false);
			files.erase(begin(files) + 3);
			const auto ids = FillSample(queue, files, 5);
			queue.setEmoji(ids.back(), QString::fromUtf8(
				"\xF0\x9F\x9A\x80\xF0\x9F\x94\xA5\xF0\x9F\x8E\x89"
				"\xF0\x9F\x98\x8E\xF0\x9F\x91\x8D"));
			queue.start();
			auto time = now;
			for (auto i = 0; i != kMaxFailuresInRow; ++i) {
				time += kMaxDelay + kBurstDelay;
				const auto step = queue.next(time);
				if (step.type != Step::Type::Upload) {
					break;
				}
				queue.failed(step.id, QString(), time);
			}
		},
		.create = NewPack{
			.title = u"Видеостикеры из поездки в горы прошлым летом"_q,
			.shortName = u"mountain_trip_video"_q,
		},
	});

	// The box after a run that was interrupted by the app: two files are
	// in the set, about one nobody knows, the rest is in the queue again.
	scene(u"sticker_batch_restored"_q, 1, Preset{
		.fill = [](Queue &queue, crl::time) {
			auto files = SampleFiles(false);
			files.erase(begin(files) + 3);
			auto saved = SavedQueue{ .packCount = 26 };
			for (auto i = 0; i != int(files.size()); ++i) {
				const auto &file = files[i];
				const auto name = QFileInfo(file.path).completeBaseName();
				auto item = SavedItem{
					.path = file.path,
					.name = name,
					.emoji = CleanEmoji(name, RealEmojiLength),
					.emojiFromName = true,
					.state = (i < 2)
						? SavedState::Done
						: (i == 2)
						? SavedState::Unknown
						: SavedState::Pending,
					.format = file.format,
					.fromVideo = file.fromVideo,
				};
				if (item.state != SavedState::Pending) {
					item.thumbnail = SampleSticker(
						file.body,
						file.head,
						kThumbMaxPixels);
				}
				saved.items.push_back(std::move(item));
			}
			queue.restore(std::move(saved));
			for (auto i = 3; i < int(files.size()); ++i) {
				const auto item = queue.nextToConvert();
				if (!item) {
					break;
				}
				queue.converted(
					item->id,
					item->revision,
					SampleConverted(files[i]));
			}
		},
		.uploadedTo = SamplePacks().list.front(),
		.restored = true,
	});

	// The emoji of one file.
	RegisterBoxScene(
		u"sticker_batch_emoji"_q,
		QSize(style::ConvertScale(kSceneEmojiWidth), 0),
		[](std::shared_ptr<Ui::Show> show) {
			return Box(EmojiBox, EmojiArgs{
				.show = show,
				.title = tr::lng_oblivion_sbatch_emoji_title(),
				.about = tr::lng_oblivion_sbatch_emoji_about(tr::now),
				.preview = SampleSticker(
					QColor(0x6c, 0x5c, 0xe7),
					QColor(0xff, 0xd7, 0xa8),
					kThumbMaxPixels),
				.emoji = QString::fromUtf8(
					"\xF0\x9F\x98\x8E\xF0\x9F\x98\xBA"),
			});
		});

	// The emoji of all the files, over the files that get them.
	RegisterBoxScene(
		u"sticker_batch_emoji_default"_q,
		QSize(style::ConvertScale(kSceneEmojiWidth), 0),
		[](std::shared_ptr<Ui::Show> show) {
			auto files = std::vector<QImage>();
			for (const auto &file : SampleFiles(false)) {
				if (file.problem == Problem::None) {
					files.push_back(SampleSticker(
						file.body,
						file.head,
						kThumbMaxPixels));
				}
			}
			return Box(EmojiBox, EmojiArgs{
				.show = show,
				.title = tr::lng_oblivion_sbatch_emoji_default_title(),
				.about = tr::lng_oblivion_sbatch_emoji_default_about(tr::now),
				.files = std::move(files),
				.emoji = DefaultEmoji(),
				.required = true,
			});
		});

	// The same with an empty queue: nothing to show under the bubble yet.
	RegisterBoxScene(
		u"sticker_batch_emoji_default_empty"_q,
		QSize(style::ConvertScale(kSceneEmojiWidth), 0),
		[](std::shared_ptr<Ui::Show> show) {
			return Box(EmojiBox, EmojiArgs{
				.show = show,
				.title = tr::lng_oblivion_sbatch_emoji_default_title(),
				.about = tr::lng_oblivion_sbatch_emoji_default_about(tr::now),
				.placeholder = tr::lng_oblivion_sbatch_emoji_default_empty(
					tr::now),
				.emoji = DefaultEmoji(),
				.required = true,
			});
		});
});

// The self-test.

// One symbol of the emoji planes with an optional variation selector:
// enough to check the rules, the app uses the emoji of Telegram.
[[nodiscard]] int TestEmojiLength(QStringView text) {
	if (text.size() < 2
		|| !text[0].isHighSurrogate()
		|| !text[1].isLowSurrogate()) {
		return 0;
	}
	const auto code = QChar::surrogateToUcs4(text[0], text[1]);
	if (code < 0x1F300 || code > 0x1FAFF) {
		return 0;
	}
	return (text.size() > 2 && text[2].unicode() == 0xFE0F) ? 3 : 2;
}

} // namespace

bool MimeHasFiles(const QMimeData *data) {
	if (!data || !data->hasUrls()) {
		return false;
	}
	for (const auto &url : Core::ReadMimeUrls(data)) {
		if (!url.isLocalFile()) {
			continue;
		}
		const auto path = url.toLocalFile();
		if (AcceptsExtension(path) || QFileInfo(path).isDir()) {
			return true;
		}
	}
	return false;
}

QStringList PathsFromMime(const QMimeData *data) {
	auto given = QStringList();
	if (!data || !data->hasUrls()) {
		return given;
	}
	for (const auto &url : Core::ReadMimeUrls(data)) {
		if (!url.isLocalFile()) {
			continue;
		}
		const auto path = url.toLocalFile();
		if (AcceptsExtension(path) || QFileInfo(path).isDir()) {
			given.push_back(path);
		}
	}
	return ExpandPaths(given);
}

bool RunSelfTest(QStringList &log) {
	auto passed = true;
	const auto check = [&](bool condition, const QString &what) {
		if (!condition) {
			passed = false;
			log.push_back(u"FAIL: "_q + what);
		}
		return condition;
	};
	const auto sizeText = [](QSize size) {
		return u"%1x%2"_q.arg(size.width()).arg(size.height());
	};
	const auto smile = QString::fromUtf8("\xF0\x9F\x99\x82");
	const auto cat = QString::fromUtf8("\xF0\x9F\x98\xBA");
	const auto party = QString::fromUtf8("\xF0\x9F\x8E\x89");
	const auto heart = QString::fromUtf8("\xF0\x9F\x92\x9C");

	// The emoji of a text.
	{
		const auto length = EmojiLength(TestEmojiLength);
		check(
			CleanEmoji(u"кот "_q + cat + u" и "_q + party, length)
				== cat + party,
			u"CleanEmoji keeps only the emoji"_q);
		check(
			CleanEmoji(cat + cat + party + cat, length) == cat + party,
			u"CleanEmoji keeps an emoji once"_q);
		check(
			CleanEmoji(u"IMG_1234 (2)"_q, length).isEmpty(),
			u"CleanEmoji of a name without emoji"_q);
		check(
			CleanEmoji(cat + party + heart, length, 2) == cat + party,
			u"CleanEmoji keeps the limit"_q);
		check(
			CleanEmoji(cat, nullptr).isEmpty(),
			u"CleanEmoji without a reader"_q);
		auto many = QString();
		for (auto i = 0; i != 40; ++i) {
			many.append(QString::fromUcs4(
				std::array<char32_t, 1>{ char32_t(0x1F600 + i) }.data(),
				1));
		}
		check(
			CleanEmoji(many, length).size() == 2 * StickerPacks::kMaxEmoji,
			u"CleanEmoji gives at most 20 emoji"_q);
		// A broken surrogate pair is not an emoji and breaks nothing.
		check(
			CleanEmoji(cat.left(1) + u"x"_q + party, length) == party,
			u"CleanEmoji of half a symbol"_q);
	}

	// The emoji of the items: the name, the default, a choice by hand.
	{
		auto queue = Queue(TestEmojiLength);
		check(queue.defaultEmoji() == smile, u"The default emoji"_q);
		const auto plain = queue.add(u"/a/plain.png"_q, u"plain"_q);
		const auto named = queue.add(
			u"/a/001 "_q + party + u".png"_q,
			u"001 "_q + party);
		const auto item = [&](uint64 id) {
			return *queue.find(id);
		};
		check(
			plain && named && (plain != named),
			u"Items get different ids"_q);
		check(
			queue.emojiOf(item(plain)) == smile
				&& item(plain).emoji.isEmpty(),
			u"An item without emoji takes the default"_q);
		check(
			queue.emojiOf(item(named)) == party
				&& item(named).emojiFromName,
			u"The emoji of the file name is taken"_q);
		check(
			queue.setDefaultEmoji(u" "_q + cat + u" "_q)
				&& queue.defaultEmoji() == cat,
			u"The default emoji is changed"_q);
		check(
			queue.emojiOf(item(plain)) == cat
				&& queue.emojiOf(item(named)) == party,
			u"A new default changes only the items without their own"_q);
		check(
			!queue.setDefaultEmoji(u"no emoji"_q)
				&& queue.defaultEmoji() == cat,
			u"The default emoji can't be empty"_q);
		check(
			queue.setEmoji(plain, heart + party)
				&& queue.emojiOf(item(plain)) == heart + party
				&& !item(plain).emojiFromName,
			u"An emoji chosen by hand"_q);
		queue.setDefaultEmoji(smile);
		check(
			queue.emojiOf(item(plain)) == heart + party,
			u"A chosen emoji doesn't follow the default"_q);
		check(
			queue.setEmoji(plain, QString())
				&& queue.emojiOf(item(plain)) == smile,
			u"The emoji of an item is reset to the default"_q);
		check(
			queue.setEmoji(named, smile)
				&& item(named).emoji.isEmpty()
				&& !item(named).emojiFromName,
			u"Choosing the default emoji keeps nothing of its own"_q);
		check(!queue.setEmoji(12345, cat), u"setEmoji of nothing"_q);
	}

	// The files.
	{
		check(
			AcceptsExtension(u"/a/b.PNG"_q)
				&& AcceptsExtension(u"C:\\x\\clip.Mp4"_q)
				&& AcceptsExtension(u"a.tgs"_q)
				&& AcceptsExtension(u"a.b.json"_q),
			u"AcceptsExtension of the known files"_q);
		check(
			!AcceptsExtension(u"/a/b.txt"_q)
				&& !AcceptsExtension(u"/a/png"_q)
				&& !AcceptsExtension(u"/a.png/b"_q)
				&& !AcceptsExtension(u"/a/.png"_q)
				&& !AcceptsExtension(QString()),
			u"AcceptsExtension of the others"_q);
		auto names = QStringList{
			u"10.png"_q,
			u"2.png"_q,
			u"b1.png"_q,
			u"A3.png"_q,
			u"1.png"_q,
			u"a12.png"_q,
			u"002 x.png"_q,
		};
		std::sort(names.begin(), names.end(), NaturalLess);
		check(
			names == QStringList{
				u"1.png"_q,
				u"002 x.png"_q,
				u"2.png"_q,
				u"10.png"_q,
				u"A3.png"_q,
				u"a12.png"_q,
				u"b1.png"_q,
			},
			u"NaturalLess: "_q + names.join(' '));
		check(
			!NaturalLess(u"a.png"_q, u"a.png"_q)
				&& (NaturalLess(u"a"_q, u"A"_q) != NaturalLess(u"A"_q, u"a"_q)),
			u"NaturalLess is a strict order"_q);
	}

	// FLOOD_WAIT and the other errors.
	{
		check(
			FloodWaitSeconds(u"FLOOD_WAIT_17"_q) == 17,
			u"FLOOD_WAIT_17"_q);
		check(
			FloodWaitSeconds(u"FLOOD_PREMIUM_WAIT_5"_q) == 5,
			u"FLOOD_PREMIUM_WAIT_5"_q);
		check(
			FloodWaitSeconds(u"FLOOD_WAIT_999999999"_q) == kMaxFloodWait,
			u"A FLOOD_WAIT is at most a day"_q);
		check(
			FloodWaitSeconds(u"FLOOD"_q) == kDefaultFloodWait
				&& FloodWaitSeconds(u"FLOOD_WAIT_X"_q) == kDefaultFloodWait
				&& FloodWaitSeconds(u"FLOOD_WAIT_0"_q) == kDefaultFloodWait,
			u"A flood error without a number"_q);
		check(
			!FloodWaitSeconds(u"STICKER_PNG_DIMENSIONS"_q)
				&& !FloodWaitSeconds(QString())
				&& !FloodWaitSeconds(u"STICKERS_TOO_MUCH"_q),
			u"Other errors are not a flood"_q);
		check(
			IsFullError(u"STICKERS_TOO_MUCH"_q)
				&& IsNameError(u"PACK_SHORT_NAME_OCCUPIED"_q)
				&& IsNameError(u"SHORTNAME_OCCUPY_FAILED"_q)
				&& IsNameError(u"PACK_TITLE_INVALID"_q)
				&& IsTargetError(u"STICKERSET_INVALID"_q)
				&& !IsNameError(u"STICKER_PNG_NOPNG"_q)
				&& !IsFullError(QString()),
			u"The errors that stop the queue"_q);
		check(FormatWait(17000) == u"0:17"_q, u"FormatWait 17 s"_q);
		check(FormatWait(16001) == u"0:17"_q, u"FormatWait rounds up"_q);
		check(FormatWait(725000) == u"12:05"_q, u"FormatWait 12:05"_q);
		check(FormatWait(3750000) == u"1:02:30"_q, u"FormatWait 1:02:30"_q);
		check(FormatWait(-5) == u"0:00"_q, u"FormatWait of the past"_q);
	}

	// The sizes: 512 for a sticker, the 100x100 square for an emoji.
	{
		using StickerPacks::CanvasSize;
		using StickerPacks::FitSize;
		const auto fits = [&](
				QSize source,
				QSize sticker,
				QSize emoji) {
			const auto gotSticker = FitSize(source, PackKind::Stickers);
			const auto gotEmoji = FitSize(source, PackKind::Emoji);
			check(
				(gotSticker == sticker) && (gotEmoji == emoji),
				u"FitSize of %1: %2 and %3"_q.arg(
					sizeText(source),
					sizeText(gotSticker),
					sizeText(gotEmoji)));
			check(
				CanvasSize(source, PackKind::Stickers) == sticker,
				u"CanvasSize of a sticker "_q + sizeText(source));
			check(
				CanvasSize(source, PackKind::Emoji) == QSize(100, 100),
				u"CanvasSize of an emoji "_q + sizeText(source));
		};
		fits(QSize(1200, 800), QSize(512, 341), QSize(100, 66));
		fits(QSize(100, 300), QSize(170, 512), QSize(33, 100));
		fits(QSize(512, 512), QSize(512, 512), QSize(100, 100));
		fits(QSize(40, 40), QSize(512, 512), QSize(100, 100));
		fits(QSize(5000, 3), QSize(512, 1), QSize(100, 1));
		fits(QSize(4000, 3000), QSize(512, 384), QSize(100, 75));
		check(
			FitSize(QSize(), PackKind::Stickers).isEmpty()
				&& FitSize(QSize(0, 10), PackKind::Emoji).isEmpty()
				&& CanvasSize(QSize(), PackKind::Emoji).isEmpty(),
			u"FitSize of nothing"_q);
		check(
			StickerPacks::SideFor(PackKind::Stickers) == 512
				&& StickerPacks::SideFor(PackKind::Emoji) == 100
				&& StickerPacks::MaxInSet(PackKind::Stickers) == 120
				&& StickerPacks::MaxInSet(PackKind::Emoji) == 200,
			u"The sides and the limits of the sets"_q);
	}

	// A disc on a big transparent canvas and an opaque photo.
	auto disc = QImage(900, 700, QImage::Format_ARGB32_Premultiplied);
	disc.fill(Qt::transparent);
	{
		auto p = QPainter(&disc);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(220, 40, 60));
		p.drawEllipse(QRect(300, 200, 300, 300));
	}
	auto photo = QImage(1200, 800, QImage::Format_RGB32);
	photo.fill(QColor(40, 120, 200));
	{
		const auto emoji = StickerPacks::ComposeEmoji(disc);
		check(
			(emoji.size() == QSize(100, 100))
				&& (emoji.format() == QImage::Format_ARGB32_Premultiplied),
			u"ComposeEmoji of the disc: "_q + sizeText(emoji.size()));
		if (emoji.size() == QSize(100, 100)) {
			check(
				(qAlpha(emoji.pixel(50, 50)) == 255)
					&& (qRed(emoji.pixel(50, 50)) > 200)
					&& (qAlpha(emoji.pixel(50, 2)) > 200)
					&& (qAlpha(emoji.pixel(2, 50)) > 200),
				u"The margins are cropped: the disc fills the emoji"_q);
			check(
				(qAlpha(emoji.pixel(1, 1)) == 0)
					&& (qAlpha(emoji.pixel(98, 98)) == 0),
				u"The corners of the emoji stay transparent"_q);
		}
		const auto wide = StickerPacks::ComposeEmoji(photo);
		check(
			wide.size() == QSize(100, 100),
			u"ComposeEmoji of a photo: "_q + sizeText(wide.size()));
		if (wide.size() == QSize(100, 100)) {
			// 100x66 in the middle: 17 transparent rows above and below.
			check(
				(qAlpha(wide.pixel(50, 5)) == 0)
					&& (qAlpha(wide.pixel(50, 94)) == 0)
					&& (qAlpha(wide.pixel(50, 16)) == 0)
					&& (qAlpha(wide.pixel(50, 17)) == 255)
					&& (qAlpha(wide.pixel(50, 82)) == 255)
					&& (qAlpha(wide.pixel(50, 83)) == 0),
				u"A photo is centered in the square"_q);
			const auto color = wide.pixel(50, 50);
			check(
				(std::abs(qRed(color) - 40) <= 2)
					&& (std::abs(qGreen(color) - 120) <= 2)
					&& (std::abs(qBlue(color) - 200) <= 2)
					&& (qAlpha(wide.pixel(0, 50)) == 255)
					&& (qAlpha(wide.pixel(99, 50)) == 255),
				u"A photo takes the whole width of the emoji"_q);
		}
		check(
			StickerPacks::ComposeEmoji(QImage()).isNull(),
			u"ComposeEmoji of null"_q);
		auto empty = QImage(64, 32, QImage::Format_ARGB32_Premultiplied);
		empty.fill(Qt::transparent);
		check(
			StickerPacks::ComposeEmoji(empty).size() == QSize(100, 100),
			u"ComposeEmoji of a transparent image"_q);
	}

	// The converter: the same file for a sticker set and an emoji set.
	const auto convert = [](ConvertRequest request) {
		request.thumbnail = 64;
		return Convert(request, nullptr);
	};
	if (check(StickerPacks::WebpSupported(), u"WebP is supported"_q)) {
		auto png = QByteArray();
		{
			auto buffer = QBuffer(&png);
			buffer.open(QIODevice::WriteOnly);
			disc.save(&buffer, "PNG");
		}
		const auto sticker = convert({
			.content = png,
			.name = u"disc.png"_q,
		});
		check(
			sticker.prepared.valid()
				&& (sticker.problem == Problem::None)
				&& (sticker.format == Format::Static)
				&& (sticker.prepared.format == Format::Static)
				&& (sticker.prepared.size == QSize(512, 398))
				&& (sticker.prepared.bytes.size()
					<= StickerPacks::kStaticMaxBytes)
				&& !sticker.fromVideo,
			u"An image becomes a 512 px sticker: %1, %2 bytes"_q.arg(
				sizeText(sticker.prepared.size)
			).arg(sticker.prepared.bytes.size()));
		check(
			!sticker.thumbnail.isNull()
				&& (sticker.thumbnail.width() <= 64)
				&& (sticker.thumbnail.height() <= 64),
			u"The thumbnail of an image: "_q
				+ sizeText(sticker.thumbnail.size()));
		const auto emoji = convert({
			.content = png,
			.name = u"disc.png"_q,
			.kind = PackKind::Emoji,
		});
		check(
			emoji.prepared.valid()
				&& (emoji.prepared.format == Format::Static)
				&& (emoji.prepared.size == QSize(100, 100))
				&& (emoji.prepared.bytes.size()
					<= StickerPacks::kEmojiStaticMaxBytes),
			u"An image becomes a 100x100 emoji: %1, %2 bytes"_q.arg(
				sizeText(emoji.prepared.size)
			).arg(emoji.prepared.bytes.size()));
		const auto decoded = QImage::fromData(emoji.prepared.bytes, "webp");
		check(
			(decoded.size() == QSize(100, 100))
				&& decoded.hasAlphaChannel()
				&& (qAlpha(decoded.pixel(1, 1)) == 0)
				&& (qAlpha(decoded.pixel(50, 50)) == 255),
			u"The emoji is a WebP with the transparency"_q);

		// Noise is the worst case for the size.
		auto noise = QImage(700, 700, QImage::Format_ARGB32);
		auto seed = uint32(0x9E3779B9U);
		for (auto y = 0; y != noise.height(); ++y) {
			const auto line = reinterpret_cast<QRgb*>(noise.scanLine(y));
			for (auto x = 0; x != noise.width(); ++x) {
				seed = seed * 1664525U + 1013904223U;
				line[x] = (seed | 0xFF000000U);
			}
		}
		const auto heavy = StickerPacks::PrepareEmoji(noise);
		check(
			heavy.valid()
				&& (heavy.size == QSize(100, 100))
				&& (heavy.bytes.size() <= StickerPacks::kEmojiStaticMaxBytes),
			u"An emoji of noise fits the limit: %1 bytes"_q.arg(
				heavy.bytes.size()));
	}
	{
		// 256x256, 25 fps, 6 seconds: fixed without a question.
		const auto json = LottieEdit::Document::Blank(
			QSize(256, 256),
			25.,
			150).toJson();
		const auto sticker = convert({
			.content = json,
			.name = u"wrong.json"_q,
		});
		check(
			sticker.prepared.valid()
				&& sticker.fixed
				&& (sticker.format == Format::Animated)
				&& (sticker.prepared.size == QSize(512, 512))
				&& (sticker.prepared.duration <= 3000),
			u"An animation is fixed automatically: %1, %2 ms"_q.arg(
				sizeText(sticker.prepared.size)
			).arg(sticker.prepared.duration));
		const auto emoji = convert({
			.content = json,
			.name = u"wrong.json"_q,
			.kind = PackKind::Emoji,
		});
		check(
			emoji.prepared.valid()
				&& emoji.fixed
				&& (emoji.prepared.format == Format::Animated)
				&& (emoji.prepared.size == sticker.prepared.size)
				&& (emoji.prepared.duration == sticker.prepared.duration),
			u"An animated emoji is the file of an animated sticker"_q);
		const auto good = convert({
			.content = LottieEdit::Document::Blank().toTgs(),
			.name = u"good.tgs"_q,
		});
		check(
			good.prepared.valid() && !good.fixed,
			u"A good .tgs is taken as it is"_q);
		const auto broken = convert({
			.content = QByteArray("{ \"not\": \"a lottie\" }"),
			.name = u"broken.json"_q,
		});
		check(
			!broken.prepared.valid()
				&& (broken.problem == Problem::Lottie)
				&& (broken.format == Format::Animated),
			u"A JSON that is not an animation: problem %1"_q.arg(
				int(broken.problem)));
	}
	{
		const auto text = convert({
			.content = QByteArray("just a text"),
			.name = u"notes.txt"_q,
		});
		check(
			!text.prepared.valid() && (text.problem == Problem::Unknown),
			u"A text file is not a sticker: problem %1"_q.arg(
				int(text.problem)));
		const auto missing = convert({
			.path = u":/oblivion/no/such/file.png"_q,
		});
		check(
			!missing.prepared.valid() && (missing.problem == Problem::Read),
			u"A file that can't be opened: problem %1"_q.arg(
				int(missing.problem)));
		const auto nothing = convert({ .name = u"empty.png"_q });
		check(
			!nothing.prepared.valid() && (nothing.problem == Problem::Read),
			u"An empty file: problem %1"_q.arg(int(nothing.problem)));
	}
	{
		auto frames = std::vector<QImage>();
		for (auto i = 0; i != 12; ++i) {
			auto frame = QImage(320, 240, QImage::Format_ARGB32_Premultiplied);
			frame.fill(QColor(20 + i * 10, 90, 200 - i * 8));
			auto p = QPainter(&frame);
			p.setPen(Qt::NoPen);
			p.setBrush(Qt::white);
			p.drawEllipse(QPoint(40 + i * 12, 120), 30, 30);
			p.end();
			frames.push_back(std::move(frame));
		}
		const auto ready = VideoCore::EncodeVideoSticker(frames, 50);
		if (check(
				ready.ok && !ready.webm.isEmpty(),
				u"EncodeVideoSticker: "_q + ready.error)) {
			const auto sticker = convert({
				.content = ready.webm,
				.name = u"clip.webm"_q,
			});
			check(
				sticker.prepared.valid()
					&& (sticker.format == Format::Video)
					&& (sticker.prepared.bytes == ready.webm)
					&& (sticker.prepared.size == QSize(512, 384))
					&& !sticker.fromVideo,
				u"A ready video sticker is taken as it is: %1"_q.arg(
					sizeText(sticker.prepared.size)));
			check(
				!sticker.thumbnail.isNull()
					&& (sticker.thumbnail.width() <= 64),
				u"The thumbnail of a video: "_q
					+ sizeText(sticker.thumbnail.size()));
			const auto emoji = convert({
				.content = ready.webm,
				.name = u"clip.webm"_q,
				.kind = PackKind::Emoji,
			});
			check(
				emoji.prepared.valid()
					&& (emoji.prepared.size == QSize(100, 100))
					&& (emoji.prepared.bytes.size()
						<= VideoCore::kEmojiMaxBytes)
					&& emoji.fromVideo
					&& StickerPacks::CheckVideoEmoji(
						emoji.prepared.bytes).ok(),
				u"A video sticker becomes a 100x100 emoji: %1, %2 bytes, "
				"problem %3"_q.arg(
					sizeText(emoji.prepared.size)
				).arg(emoji.prepared.bytes.size()).arg(
					int(StickerPacks::CheckVideoEmoji(
						emoji.prepared.bytes).problem)));
			check(
				StickerPacks::CheckVideoEmoji(ready.webm).problem
					== StickerPacks::VideoProblem::Dimensions,
				u"A 512 px WebM is not an emoji"_q);
			if (emoji.prepared.valid()) {
				const auto again = convert({
					.content = emoji.prepared.bytes,
					.name = u"emoji.webm"_q,
					.kind = PackKind::Emoji,
				});
				check(
					again.prepared.valid()
						&& (again.prepared.bytes == emoji.prepared.bytes),
					u"A ready video emoji is taken as it is"_q);
			}

			// What the trim box gives replaces the file.
			const auto trimmed = convert({
				.path = u":/oblivion/no/such/file.mp4"_q,
				.trimmed = ready.webm,
			});
			check(
				trimmed.prepared.valid()
					&& (trimmed.prepared.bytes == ready.webm)
					&& trimmed.fromVideo,
				u"A trimmed fragment is used instead of the file"_q);
		}
		const auto gif = VideoCore::EncodeGif(frames, 50);
		if (check(!gif.isEmpty(), u"EncodeGif"_q)) {
			const auto sticker = convert({
				.content = gif,
				.name = u"anim.gif"_q,
			});
			check(
				sticker.prepared.valid()
					&& (sticker.format == Format::Video)
					&& (sticker.prepared.size == QSize(512, 384))
					&& sticker.fromVideo
					&& StickerPacks::CheckVideoSticker(
						sticker.prepared.bytes).ok(),
				u"A GIF becomes a video sticker: %1, %2 bytes"_q.arg(
					sizeText(sticker.prepared.size)
				).arg(sticker.prepared.bytes.size()));
		}
		auto cancelled = std::make_shared<std::atomic<bool>>(true);
		const auto stopped = Convert({
			.content = gif,
			.name = u"anim.gif"_q,
			.thumbnail = 64,
			.cancel = cancelled,
		}, nullptr);
		check(
			!stopped.prepared.valid() && stopped.thumbnail.isNull(),
			u"A cancelled conversion gives nothing"_q);
	}

	// The queue.
	const auto ready = [](Format format = Format::Static) {
		return Converted{
			.prepared = Prepared{
				.format = format,
				.bytes = QByteArray(100, 'x'),
				.size = QSize(512, 512),
			},
			.format = format,
		};
	};
	const auto convertAll = [&](Queue &queue) {
		while (const auto item = queue.nextToConvert()) {
			queue.converted(item->id, item->revision, ready());
		}
	};
	const auto stateOf = [](const Queue &queue, uint64 id) {
		const auto item = queue.find(id);
		return item ? item->state : ItemState::Invalid;
	};
	{
		auto queue = Queue(TestEmojiLength);
		const auto a = queue.add(u"/q/a.png"_q, u"a"_q);
		const auto b = queue.add(u"/q/b.png"_q, u"b"_q);
		const auto c = queue.add(u"/q/c.png"_q, u"c"_q);
		check(a && b && c, u"Three files are added"_q);
		check(!queue.add(u"/q/a.png"_q, u"a"_q), u"A file is added once"_q);
		check(!queue.add(QString()), u"An empty path is not added"_q);
		check(
			(queue.counts().total == 3) && (queue.counts().converting == 3),
			u"New items wait for the converter"_q);
		check(
			queue.next(1000).type == Step::Type::None,
			u"Nothing is uploaded before the start"_q);

		// The converter: one file at a time, old results are dropped.
		const auto first = queue.nextToConvert();
		check(
			first && (first->id == a) && (first->state == ItemState::Converting),
			u"The converter takes the first file"_q);
		check(
			!queue.nextToConvert(),
			u"The converter takes one file at a time"_q);
		check(
			!queue.converted(a, 7, ready()) && !queue.converted(b, 0, ready()),
			u"A result of another conversion is dropped"_q);

		// The upload starts while the files are being converted.
		check(queue.start(), u"The queue starts"_q);
		check(
			(queue.run() == RunState::Running)
				&& (queue.next(1000).type == Step::Type::None),
			u"The upload waits for the converter"_q);
		check(queue.converted(a, 0, ready()), u"The first file is ready"_q);
		const auto second = queue.nextToConvert();
		check(second && (second->id == b), u"The second file is converted"_q);

		auto step = queue.next(1000);
		check(
			(step.type == Step::Type::Upload) && (step.id == a)
				&& (stateOf(queue, a) == ItemState::Uploading),
			u"The first file is uploaded"_q);
		check(
			queue.next(1000).type == Step::Type::None,
			u"One upload at a time"_q);
		check(!queue.remove(a), u"An uploading item can't be removed"_q);
		check(
			!queue.setEmoji(a, cat),
			u"The emoji of an uploading item can't be changed"_q);

		// b is still being converted, c is not even started: the order
		// is kept, nothing jumps the line.
		queue.uploaded(a, 25, 1000);
		check(
			(stateOf(queue, a) == ItemState::Done)
				&& (queue.packCount() == 25)
				&& !queue.find(a)->prepared.valid(),
			u"The uploaded file is done and its bytes are freed"_q);
		step = queue.next(1000);
		check(
			(step.type == Step::Type::Wait)
				&& (step.till == 1000 + kBaseDelay),
			u"A pause after a sticker: till %1"_q.arg(step.till));
		check(
			queue.next(1000 + kBaseDelay).type == Step::Type::None,
			u"The next file is not converted yet"_q);
		check(queue.converted(b, 0, ready()), u"The second file is ready"_q);
		check(
			queue.next(1000 + kBaseDelay - 1).type == Step::Type::Wait,
			u"Not before the pause is over"_q);
		step = queue.next(1000 + kBaseDelay);
		check(
			(step.type == Step::Type::Upload) && (step.id == b),
			u"The second file is uploaded after the pause"_q);

		// FLOOD_WAIT: the same file again, after all the time asked.
		const auto floodAt = crl::time(5000);
		queue.failed(b, u"FLOOD_WAIT_17"_q, floodAt);
		check(
			(stateOf(queue, b) == ItemState::Ready)
				&& queue.flood()
				&& (queue.run() == RunState::Running)
				&& (queue.counts().failed == 0),
			u"A FLOOD_WAIT is not a failure of the file"_q);
		step = queue.next(floodAt);
		const auto floodTill = floodAt + 17000 + kFloodSlack;
		check(
			(step.type == Step::Type::Wait) && (step.till == floodTill),
			u"FLOOD_WAIT_17 is waited in full: till %1"_q.arg(step.till));
		check(
			queue.next(floodTill - 1).type == Step::Type::Wait,
			u"Nothing is sent a millisecond before"_q);
		check(
			queue.delay() == 2 * kBaseDelay,
			u"The pace slows down after a FLOOD_WAIT"_q);

		// A pause keeps the wait.
		queue.pause();
		check(
			(queue.run() == RunState::Paused)
				&& (queue.reason() == StopReason::User)
				&& (queue.next(floodTill + 1).type == Step::Type::None),
			u"Nothing is uploaded on pause"_q);
		check(queue.start(), u"The queue continues"_q);
		check(
			queue.next(floodTill - 1).type == Step::Type::Wait,
			u"Continuing doesn't skip the FLOOD_WAIT"_q);
		step = queue.next(floodTill);
		check(
			(step.type == Step::Type::Upload) && (step.id == b)
				&& !queue.flood(),
			u"The same file is uploaded after the FLOOD_WAIT"_q);
		check(
			queue.find(b)->attempts == 2,
			u"The attempts are counted"_q);

		// A pause lets the current sticker get to the set.
		queue.pause();
		check(
			stateOf(queue, b) == ItemState::Uploading,
			u"A pause doesn't throw the current upload away"_q);
		queue.uploaded(b, 26, floodTill);
		check(
			(stateOf(queue, b) == ItemState::Done)
				&& (queue.run() == RunState::Paused),
			u"The current sticker is added on pause"_q);
		queue.stop();
		check(
			(queue.run() == RunState::Idle)
				&& (queue.reason() == StopReason::Stopped),
			u"The queue is stopped"_q);

		// A file that failed is skipped, the others go on.
		convertAll(queue);
		check(queue.start(), u"The queue starts again"_q);
		const auto later = floodTill + kMaxDelay;
		step = queue.next(later);
		check(
			(step.type == Step::Type::Upload) && (step.id == c),
			u"The third file is uploaded"_q);
		queue.failed(c, u"STICKER_PNG_DIMENSIONS"_q, later);
		check(
			(stateOf(queue, c) == ItemState::Failed)
				&& (queue.find(c)->error == u"STICKER_PNG_DIMENSIONS"_q)
				&& (queue.run() == RunState::Running),
			u"A refused file is failed"_q);
		step = queue.next(later + kMaxDelay);
		check(
			(step.type == Step::Type::Finished)
				&& (queue.run() == RunState::Idle)
				&& (queue.reason() == StopReason::Finished),
			u"The queue finishes when nothing is left"_q);
		const auto counts = queue.counts();
		check(
			(counts.done == 2) && (counts.failed == 1)
				&& (counts.pending() == 0) && (counts.valid() == 3),
			u"The counts of a finished queue"_q);
		check(!queue.start(), u"Nothing to start with"_q);

		// Retry.
		check(
			(queue.retryFailed() == 1)
				&& (stateOf(queue, c) == ItemState::Ready)
				&& queue.find(c)->error.isEmpty(),
			u"The failed file is retried"_q);
		check(queue.start(), u"The queue starts for the retry"_q);
		step = queue.next(later + 2 * kMaxDelay);
		check(
			(step.type == Step::Type::Upload) && (step.id == c),
			u"The retried file is uploaded"_q);
		queue.uploaded(c, 0, later + 2 * kMaxDelay);
		check(
			queue.packCount() == 27,
			u"The count of the set grows without an answer too"_q);
		check(
			queue.remove(c) && !queue.find(c) && !queue.remove(c),
			u"A done item is removed"_q);
		check(
			queue.add(u"/q/a.png"_q, u"a"_q) != 0,
			u"An uploaded file can be added again"_q);
	}
	{
		// The converter says no: the item is skipped by the upload.
		auto queue = Queue(TestEmojiLength);
		const auto bad = queue.add(u"/q/bad.txt"_q);
		const auto good = queue.add(u"/q/good.png"_q);
		const auto first = queue.nextToConvert();
		check(
			queue.converted(first->id, first->revision, Converted{
				.problem = Problem::Unknown,
			}) && (stateOf(queue, bad) == ItemState::Invalid),
			u"A file that is not a sticker is invalid"_q);
		convertAll(queue);
		check(queue.start(), u"The queue with an invalid file starts"_q);
		const auto step = queue.next(0);
		check(
			(step.type == Step::Type::Upload) && (step.id == good),
			u"An invalid file is skipped"_q);
		queue.uploaded(good, 1, 0);
		check(
			queue.retry(bad) && (stateOf(queue, bad) == ItemState::Waiting)
				&& (queue.find(bad)->revision == 1),
			u"An invalid file is converted again by a retry"_q);
		check(!queue.retry(good), u"A done file is not retried"_q);
	}
	{
		// Several failures in a row pause the queue, it never loops.
		auto queue = Queue(TestEmojiLength);
		for (auto i = 0; i != 6; ++i) {
			queue.add(u"/q/%1.png"_q.arg(i));
		}
		convertAll(queue);
		queue.start();
		auto time = crl::time(0);
		auto failures = 0;
		while (queue.run() == RunState::Running && failures < 10) {
			time += kMaxDelay;
			const auto step = queue.next(time);
			if (step.type != Step::Type::Upload) {
				break;
			}
			queue.failed(step.id, QString(), time);
			++failures;
		}
		check(
			(failures == kMaxFailuresInRow)
				&& (queue.run() == RunState::Paused)
				&& (queue.reason() == StopReason::Errors)
				&& (queue.counts().failed == kMaxFailuresInRow),
			u"%1 failures in a row pause the queue"_q.arg(failures));

		// A success in between resets the row.
		queue.start();
		time += kMaxDelay;
		auto step = queue.next(time);
		queue.uploaded(step.id, 1, time);
		time += kMaxDelay;
		step = queue.next(time);
		queue.failed(step.id, u"STICKER_FILE_INVALID"_q, time);
		check(
			queue.run() == RunState::Running,
			u"One failure after a success doesn't pause"_q);
	}
	{
		// The failures are the last files of the queue: it is not left
		// paused with a "Continue" that has nothing to continue.
		auto queue = Queue(TestEmojiLength);
		for (auto i = 0; i != kMaxFailuresInRow; ++i) {
			queue.add(u"/q/%1.png"_q.arg(i));
		}
		convertAll(queue);
		queue.start();
		auto time = crl::time(0);
		for (auto i = 0; i != kMaxFailuresInRow; ++i) {
			time += kMaxDelay;
			const auto step = queue.next(time);
			if (step.type != Step::Type::Upload) {
				break;
			}
			queue.failed(step.id, QString(), time);
		}
		check(
			(queue.run() == RunState::Paused)
				&& (queue.reason() == StopReason::Errors)
				&& (queue.counts().failed == kMaxFailuresInRow),
			u"The last files fail and pause the queue"_q);
		time += kMaxDelay;
		check(
			(queue.next(time).type == Step::Type::Finished)
				&& (queue.run() == RunState::Idle)
				&& (queue.reason() == StopReason::Finished),
			u"A paused queue with nothing left to upload finishes"_q);
		check(
			(queue.retryFailed() == kMaxFailuresInRow) && queue.start(),
			u"Its failed files are retried"_q);

		// The same for a pause that catches the last file on its way.
		auto single = Queue(TestEmojiLength);
		const auto only = single.add(u"/q/only.png"_q);
		convertAll(single);
		single.start();
		const auto step = single.next(0);
		single.pause();
		check(
			(step.type == Step::Type::Upload)
				&& (single.next(kMaxDelay).type == Step::Type::None)
				&& (single.run() == RunState::Paused),
			u"A pause waits for the file on its way"_q);
		single.uploaded(only, 1, 0);
		check(
			(single.next(kMaxDelay).type == Step::Type::Finished)
				&& (single.run() == RunState::Idle)
				&& (single.counts().done == 1),
			u"A queue paused on its last file finishes"_q);

		// And for the files that are removed from a paused queue.
		auto removed = Queue(TestEmojiLength);
		const auto first = removed.add(u"/q/1.png"_q);
		const auto second = removed.add(u"/q/2.png"_q);
		convertAll(removed);
		removed.start();
		removed.pause();
		check(
			(removed.next(0).type == Step::Type::None)
				&& (removed.run() == RunState::Paused),
			u"A paused queue with files stays paused"_q);
		removed.remove(first);
		removed.remove(second);
		check(
			(removed.next(0).type == Step::Type::Finished)
				&& (removed.run() == RunState::Idle),
			u"A paused queue that is emptied finishes"_q);
	}
	{
		// The same for FLOOD_WAIT after FLOOD_WAIT.
		auto queue = Queue(TestEmojiLength);
		queue.add(u"/q/a.png"_q);
		convertAll(queue);
		queue.start();
		auto time = crl::time(0);
		auto floods = 0;
		while (queue.run() == RunState::Running && floods < 20) {
			const auto step = queue.next(time);
			if (step.type == Step::Type::Wait) {
				time = step.till;
				continue;
			} else if (step.type != Step::Type::Upload) {
				break;
			}
			queue.failed(step.id, u"FLOOD_WAIT_3"_q, time);
			++floods;
		}
		check(
			(floods == kMaxFloodsInRow + 1)
				&& (queue.run() == RunState::Paused)
				&& (queue.reason() == StopReason::Floods)
				&& (queue.counts().ready == 1),
			u"%1 FLOOD_WAIT in a row pause the queue"_q.arg(floods));
		check(
			queue.delay() == kMaxDelay,
			u"The pause between stickers has a limit: %1"_q.arg(
				queue.delay()));
		check(
			time >= (kMaxFloodsInRow * (3000 + kFloodSlack)),
			u"Every FLOOD_WAIT was waited: %1 ms"_q.arg(time));
	}
	{
		// The set is full: before the upload and by the server.
		auto queue = Queue(TestEmojiLength);
		const auto a = queue.add(u"/q/a.png"_q);
		queue.add(u"/q/b.png"_q);
		convertAll(queue);
		queue.setPackCount(StickerPacks::kMaxStickers);
		queue.start();
		auto step = queue.next(0);
		check(
			(step.type == Step::Type::Finished)
				&& (queue.reason() == StopReason::Full)
				&& (queue.run() == RunState::Idle)
				&& (queue.counts().ready == 2),
			u"Nothing is sent to a full set"_q);

		// An emoji set takes 200, and the files are converted again.
		check(
			queue.setKind(PackKind::Emoji) && !queue.setKind(PackKind::Emoji),
			u"The kind of the set is changed"_q);
		check(
			(queue.counts().converting == 2)
				&& (queue.find(a)->revision == 1)
				&& !queue.find(a)->prepared.valid(),
			u"Another kind of a set converts the files again"_q);
		convertAll(queue);
		queue.clearReason();
		check(
			queue.start() && (queue.next(0).type == Step::Type::Upload),
			u"120 emoji is not a full emoji set"_q);
		queue.failed(a, u"STICKERS_TOO_MUCH"_q, 0);
		check(
			(queue.reason() == StopReason::Full)
				&& (queue.run() == RunState::Idle)
				&& (stateOf(queue, a) == ItemState::Ready)
				&& (queue.counts().failed == 0),
			u"STICKERS_TOO_MUCH stops the queue, the file stays"_q);
		queue.setPackCount(0);
		queue.start();
		step = queue.next(kMaxDelay);
		queue.failed(step.id, u"PACK_SHORT_NAME_OCCUPIED"_q, kMaxDelay);
		check(
			(queue.reason() == StopReason::Name)
				&& (queue.run() == RunState::Idle)
				&& (stateOf(queue, a) == ItemState::Ready),
			u"A taken short name stops the queue"_q);
		queue.start();
		step = queue.next(2 * kMaxDelay);
		queue.failed(step.id, u"STICKERSET_INVALID"_q, 2 * kMaxDelay);
		check(
			queue.reason() == StopReason::Target,
			u"A set that is gone stops the queue"_q);
		queue.clearReason();
		check(queue.reason() == StopReason::None, u"clearReason"_q);
	}
	{
		// A longer pause after every kBurstCount stickers.
		auto queue = Queue(TestEmojiLength);
		for (auto i = 0; i != kBurstCount + 1; ++i) {
			queue.add(u"/q/%1.png"_q.arg(i));
		}
		convertAll(queue);
		queue.start();
		auto time = crl::time(0);
		auto longest = crl::time(0);
		auto uploads = 0;
		while (uploads <= kBurstCount) {
			const auto step = queue.next(time);
			if (step.type == Step::Type::Wait) {
				longest = std::max(longest, step.till - time);
				time = step.till;
			} else if (step.type == Step::Type::Upload) {
				queue.uploaded(step.id, 0, time);
				++uploads;
			} else {
				break;
			}
		}
		check(
			(uploads == kBurstCount + 1)
				&& (longest == kBaseDelay + kBurstDelay)
				&& (time == kBurstCount * kBaseDelay + kBurstDelay),
			u"The pace of %1 stickers: %2 ms, the longest pause %3"_q.arg(
				uploads
			).arg(time).arg(longest));
		check(
			(queue.next(time).type == Step::Type::Finished)
				&& (queue.packCount() == kBurstCount + 1),
			u"The queue finishes right after its last sticker"_q);
	}
	{
		// A fragment from the trim box and the removal.
		auto queue = Queue(TestEmojiLength);
		const auto a = queue.add(u"/q/a.mp4"_q);
		const auto b = queue.add(u"/q/b.png"_q);
		convertAll(queue);
		check(
			queue.setTrimmed(a, QByteArray("webm"))
				&& (stateOf(queue, a) == ItemState::Waiting)
				&& (queue.find(a)->revision == 1)
				&& (queue.find(a)->trimmed == QByteArray("webm")),
			u"A trimmed video is converted again"_q);
		check(
			!queue.setTrimmed(a, QByteArray()),
			u"An empty fragment is not taken"_q);
		queue.start();
		check(
			queue.next(0).type == Step::Type::None,
			u"The files after it wait for the conversion"_q);
		check(queue.remove(a), u"A waiting item is removed"_q);
		const auto step = queue.next(0);
		check(
			(step.type == Step::Type::Upload) && (step.id == b),
			u"The upload goes on without the removed one"_q);
		check(
			(queue.clear() == 0) && (queue.counts().total == 1),
			u"clear() keeps what is being uploaded"_q);
		queue.uploaded(b, 1, 0);
		check(
			(queue.clear() == 1) && queue.items().empty(),
			u"clear() removes the rest"_q);
		check(
			queue.next(kMaxDelay).type == Step::Type::Finished,
			u"An emptied queue finishes"_q);
	}
	{
		// A run interrupted by the app: what is kept of the queue and
		// what the next box gets.
		auto queue = Queue(TestEmojiLength);
		const auto a = queue.add(u"/q/a.png"_q, u"a"_q);
		const auto b = queue.add(
			u"/q/b "_q + party + u".png"_q,
			u"b "_q + party);
		const auto c = queue.add(u"/q/c.mp4"_q, u"c"_q);
		const auto d = queue.add(u"/q/d.png"_q, u"d"_q);
		const auto e = queue.add(u"/q/e.txt"_q, u"e"_q);
		queue.add(u"/q/f.png"_q, u"f"_q);
		const auto convertWithThumbnails = [&] {
			while (const auto item = queue.nextToConvert()) {
				auto result = Converted{ .problem = Problem::Lottie };
				if (item->id != e) {
					result = ready();
					result.thumbnail = QImage(
						4,
						4,
						QImage::Format_ARGB32_Premultiplied);
					result.thumbnail.fill(Qt::red);
				}
				queue.converted(item->id, item->revision, std::move(result));
			}
		};
		convertWithThumbnails();
		queue.setPackCount(10);
		queue.setDefaultEmoji(cat);
		queue.setEmoji(d, heart);
		queue.setTrimmed(c, QByteArray("webm"));
		convertWithThumbnails();
		queue.start();
		auto time = crl::time(1000);
		auto step = queue.next(time);
		queue.uploaded(step.id, 11, time);
		time += kMaxDelay;
		step = queue.next(time);
		queue.failed(step.id, u"STICKER_PNG_DIMENSIONS"_q, time);
		time += kMaxDelay;
		step = queue.next(time);
		check(
			(step.type == Step::Type::Upload) && (step.id == c)
				&& (stateOf(queue, a) == ItemState::Done)
				&& (stateOf(queue, b) == ItemState::Failed),
			u"The run to interrupt: one done, one failed, one on its way"_q);

		const auto savedState = [](const SavedQueue &from, int index) {
			return (index < int(from.items.size()))
				? from.items[index].state
				: SavedState::Pending;
		};
		const auto early = queue.save(false);
		check(
			savedState(early, 2) == SavedState::Pending,
			u"A file that was only being sent is uploaded again"_q);
		const auto saved = queue.save(true);
		check(
			(saved.items.size() == 6)
				&& (savedState(saved, 0) == SavedState::Done)
				&& (savedState(saved, 1) == SavedState::Failed)
				&& (savedState(saved, 2) == SavedState::Unsure)
				&& (savedState(saved, 3) == SavedState::Pending)
				&& (savedState(saved, 4) == SavedState::Invalid)
				&& (savedState(saved, 5) == SavedState::Pending),
			u"The states of an interrupted queue are kept"_q);
		check(
			(saved.kind == PackKind::Stickers)
				&& (saved.defaultEmoji == cat)
				&& (saved.packCount == 11)
				&& (saved.notBefore == queue.waitTill())
				&& (saved.delay == queue.delay()),
			u"The set and the pace of an interrupted queue are kept"_q);
		if (saved.items.size() == 6) {
			check(
				(saved.items[1].error == u"STICKER_PNG_DIMENSIONS"_q)
					&& (saved.items[1].emoji == party)
					&& saved.items[1].emojiFromName
					&& (saved.items[2].trimmed == QByteArray("webm"))
					&& (saved.items[3].emoji == heart)
					&& (saved.items[4].problem == Problem::Lottie),
				u"The emoji, the errors and the fragments are kept"_q);
			check(
				!saved.items[0].thumbnail.isNull()
					&& !saved.items[2].thumbnail.isNull()
					&& saved.items[3].thumbnail.isNull()
					&& saved.items[5].thumbnail.isNull(),
				u"The thumbnails are kept of what is not converted again"_q);
		}

		const auto idAt = [](const Queue &from, int index) {
			return (index < int(from.items().size()))
				? from.items()[index].id
				: uint64(0);
		};
		auto next = Queue(TestEmojiLength);
		next.restore(SavedQueue(saved));
		const auto counts = next.counts();
		check(
			(counts.total == 6) && (counts.done == 1)
				&& (counts.failed == 1) && (counts.unsure == 1)
				&& (counts.invalid == 1) && (counts.converting == 2)
				&& (counts.pending() == 2) && (counts.valid() == 5),
			u"The next queue gets the files as they were"_q);
		const auto unsure = idAt(next, 2);
		check(
			unsure
				&& (next.unresolved() == unsure)
				&& (stateOf(next, unsure) == ItemState::Unsure)
				&& (next.find(unsure)->trimmed == QByteArray("webm")),
			u"The file that was on its way waits for the answer"_q);
		check(
			(next.kind() == PackKind::Stickers)
				&& (next.defaultEmoji() == cat)
				&& (next.packCount() == 11)
				&& (next.waitTill() == queue.waitTill())
				&& (next.delay() == queue.delay())
				&& (next.run() == RunState::Idle)
				&& (next.reason() == StopReason::None),
			u"The next queue gets the set and the pace"_q);
		if (counts.total == 6) {
			const auto &items = next.items();
			check(
				(next.emojiOf(items[1]) == party)
					&& (next.emojiOf(items[3]) == heart)
					&& (next.emojiOf(items[5]) == cat)
					&& (items[1].error == u"STICKER_PNG_DIMENSIONS"_q)
					&& (items[4].problem == Problem::Lottie)
					&& !items[0].thumbnail.isNull(),
				u"The next queue gets the emoji and the errors"_q);
		}
		check(
			!next.add(u"/q/a.png"_q, u"a"_q)
				&& !next.add(u"/q/c.mp4"_q, u"c"_q)
				&& !next.add(u"/q/d.png"_q, u"d"_q)
				&& (next.counts().total == 6),
			u"The files of an interrupted run are not added again"_q);
		check(
			next.next(time).type == Step::Type::None,
			u"A restored queue waits for the user"_q);
		check(
			(next.retryFailed() == 1)
				&& (stateOf(next, unsure) == ItemState::Unsure),
			u"Retrying the failed files leaves the unsure one"_q);

		// The set has one sticker more: that file is there.
		check(
			(GuessAdded(11, 12) == Guess::Added)
				&& (GuessAdded(11, 11) == Guess::NotAdded)
				&& (GuessAdded(11, 13) == Guess::Unknown)
				&& (GuessAdded(11, 10) == Guess::Unknown)
				&& (GuessAdded(0, 1) == Guess::Added),
			u"The set tells whether the last sticker is there"_q);
		next.resolve(Guess::Added);
		check(
			!next.unresolved()
				&& (stateOf(next, unsure) == ItemState::Done)
				&& (next.counts().done == 2)
				&& next.find(unsure)->trimmed.isEmpty()
				&& !next.add(u"/q/c.mp4"_q, u"c"_q),
			u"The file that got to the set is done"_q);

		// The set is as it was: the file is uploaded again.
		auto again = Queue(TestEmojiLength);
		again.restore(SavedQueue(saved));
		const auto pending = idAt(again, 2);
		again.resolve(Guess::NotAdded);
		check(
			!again.unresolved()
				&& (stateOf(again, pending) == ItemState::Waiting)
				&& (again.find(pending)->trimmed == QByteArray("webm"))
				&& (again.counts().converting == 3),
			u"The file that didn't get to the set is uploaded again"_q);
		convertAll(again);
		check(again.start(), u"A restored queue starts"_q);
		check(
			again.next(saved.notBefore - 1).type == Step::Type::Wait,
			u"A restored queue keeps the pause of the old one"_q);
		step = again.next(saved.notBefore);
		check(
			(step.type == Step::Type::Upload) && (step.id == pending),
			u"A restored queue goes on in the same order"_q);

		// The set can't tell: the file is left for the user.
		auto unknown = Queue(TestEmojiLength);
		unknown.restore(SavedQueue(saved));
		const auto left = idAt(unknown, 2);
		unknown.resolve(Guess::Unknown);
		check(
			!unknown.unresolved()
				&& (stateOf(unknown, left) == ItemState::Unsure),
			u"The file the set can't tell about stays unsure"_q);
		convertAll(unknown);
		unknown.start();
		step = unknown.next(saved.notBefore);
		check(
			(step.type == Step::Type::Upload)
				&& (step.id == idAt(unknown, 3))
				&& (stateOf(unknown, left) == ItemState::Unsure),
			u"An unsure file is never uploaded by itself"_q);
		const auto twice = unknown.save(true);
		check(
			(savedState(twice, 2) == SavedState::Unknown)
				&& (savedState(twice, 3) == SavedState::Unsure),
			u"An unsure file stays unsure for one more box"_q);
		auto third = Queue(TestEmojiLength);
		third.restore(SavedQueue(twice));
		check(
			(third.unresolved() == idAt(third, 3))
				&& (stateOf(third, idAt(third, 2)) == ItemState::Unsure)
				&& third.retry(idAt(third, 2))
				&& (stateOf(third, idAt(third, 2)) == ItemState::Waiting),
			u"An unsure file is uploaded again when the user says so"_q);
		check(
			third.remove(idAt(third, 3)) && !third.unresolved(),
			u"A removed file needs no answer"_q);

		// An upload that starts changes the set: no answer after that.
		auto started = Queue(TestEmojiLength);
		started.restore(SavedQueue(saved));
		const auto waiting = idAt(started, 2);
		convertAll(started);
		started.start();
		step = started.next(saved.notBefore);
		check(
			(step.type == Step::Type::Upload)
				&& !started.unresolved()
				&& (stateOf(started, waiting) == ItemState::Unsure),
			u"An upload leaves the unsure file to the user"_q);

		// A queue with files takes nothing.
		auto busy = Queue(TestEmojiLength);
		busy.add(u"/q/own.png"_q);
		busy.restore(SavedQueue(saved));
		check(
			(busy.counts().total == 1) && !busy.unresolved(),
			u"Only an empty queue is restored"_q);
	}
	{
		// A FLOOD_WAIT doesn't end with the box.
		auto queue = Queue(TestEmojiLength);
		queue.add(u"/q/a.png"_q);
		queue.add(u"/q/b.png"_q);
		convertAll(queue);
		queue.start();
		auto step = queue.next(0);
		queue.uploaded(step.id, 1, 0);
		step = queue.next(kMaxDelay);
		queue.failed(step.id, u"FLOOD_WAIT_300"_q, kMaxDelay);
		const auto till = kMaxDelay + 300 * crl::time(1000) + kFloodSlack;
		auto next = Queue(TestEmojiLength);
		next.restore(queue.save(true));
		check(
			(next.counts().done == 1) && (next.counts().converting == 1)
				&& !next.unresolved()
				&& next.flood()
				&& (next.waitTill() == till)
				&& (next.delay() == 2 * kBaseDelay),
			u"A FLOOD_WAIT is kept for the next queue"_q);
		convertAll(next);
		next.start();
		step = next.next(till - 1);
		check(
			(step.type == Step::Type::Wait) && (step.till == till),
			u"The next queue waits out the FLOOD_WAIT of the old one"_q);
		check(
			next.next(till).type == Step::Type::Upload,
			u"The next queue goes on after the FLOOD_WAIT"_q);
	}
	{
		// A box opened for a set takes only the run of that set, a box
		// that asks for no set takes any, and only once.
		const auto key = uint64(0xFFFF'FFFF'FFFF'FFF1ULL);
		const auto remember = [&](uint64 packId, int packCount = 7) {
			auto run = SavedRun();
			if (packId) {
				run.target.pack = PackInfo{ .id = packId };
			} else {
				run.target.create = NewPack{ .shortName = u"fresh"_q };
			}
			run.queue.packCount = packCount;
			KeepInterruptedRun(key, std::move(run));
		};
		const auto takenFor = [&](uint64 packId, uint64 expected) {
			const auto run = TakeInterruptedRun(key, packId);
			return run
				&& run->target.pack
				&& (run->target.pack->id == expected);
		};
		KeepInterruptedRun(0, SavedRun());
		check(
			!InterruptedRuns().contains(0),
			u"A run of no account is not kept"_q);
		remember(5);
		check(
			!TakeInterruptedRun(0, 0)
				&& !TakeInterruptedRun(key - 1, 0)
				&& !TakeInterruptedRun(key, 6),
			u"A run is not given to another account or another set"_q);
		const auto same = TakeInterruptedRun(key, 5);
		check(
			same && same->target.pack && (same->target.pack->id == 5)
				&& (same->queue.packCount == 7)
				&& !TakeInterruptedRun(key, 5)
				&& !TakeInterruptedRun(key, 0),
			u"A run is taken once by a box of its set"_q);
		remember(5);
		check(
			TakeInterruptedRun(key, 0).has_value()
				&& !TakeInterruptedRun(key, 0),
			u"A run is taken by a box without a set"_q);
		remember(0);
		check(
			!TakeInterruptedRun(key, 5),
			u"A run into a new set is not given to a box of a set"_q);
		const auto fresh = TakeInterruptedRun(key, 0);
		check(
			fresh && fresh->target.create
				&& (fresh->target.create->shortName == u"fresh"_q)
				&& !InterruptedRuns().contains(key),
			u"A run into a new set is taken by a box without a set"_q);

		// A box closed by the app while another run waits: both are kept.
		remember(5);
		remember(6);
		check(
			takenFor(5, 5)
				&& !TakeInterruptedRun(key, 5)
				&& takenFor(6, 6)
				&& !InterruptedRuns().contains(key),
			u"A run of another set doesn't replace the one that waits"_q);
		remember(5);
		remember(0);
		remember(6);
		check(
			takenFor(0, 6)
				&& takenFor(5, 5)
				&& !TakeInterruptedRun(key, 6)
				&& !TakeInterruptedRun(key, 5),
			u"A box without a set takes the latest run, the others wait"_q);
		const auto waiting = TakeInterruptedRun(key, 0);
		check(
			waiting && waiting->target.create
				&& !InterruptedRuns().contains(key),
			u"The run that has waited is taken by the next box"_q);
		remember(5, 1);
		remember(5, 2);
		const auto later = TakeInterruptedRun(key, 5);
		const auto earlier = TakeInterruptedRun(key, 5);
		check(
			later && (later->queue.packCount == 2)
				&& earlier && (earlier->queue.packCount == 1)
				&& !InterruptedRuns().contains(key),
			u"Two runs into one set are both kept, the latest goes first"_q);
		const auto extraRuns = uint64(2);
		const auto firstId = uint64(10);
		const auto lastId = firstId
			+ uint64(kMaxInterruptedRuns)
			+ extraRuns
			- 1;
		for (auto id = firstId; id <= lastId; ++id) {
			remember(id);
		}
		const auto kept = [&] {
			auto &runs = InterruptedRuns();
			const auto i = runs.find(key);
			return (i != end(runs)) ? int(i->second.size()) : 0;
		};
		check(
			(kept() == kMaxInterruptedRuns)
				&& !TakeInterruptedRun(key, firstId)
				&& !TakeInterruptedRun(key, firstId + extraRuns - 1)
				&& takenFor(firstId + extraRuns, firstId + extraRuns)
				&& takenFor(0, lastId)
				&& (kept() == kMaxInterruptedRuns - 2),
			u"Only the oldest runs go when too many wait"_q);
		while (TakeInterruptedRun(key, 0)) {
		}
		check(
			!InterruptedRuns().contains(key),
			u"Nothing waits when every run is taken"_q);
	}
	{
		// The queue has a limit.
		auto queue = Queue(TestEmojiLength);
		auto added = 0;
		for (auto i = 0; i != kMaxQueue + 5; ++i) {
			if (queue.add(u"/q/%1.png"_q.arg(i))) {
				++added;
			}
		}
		check(added == kMaxQueue, u"The queue takes %1 files"_q.arg(added));
	}

	if (passed) {
		log.push_back(u"Sticker batch: all checks passed."_q);
	}
	return passed;
}

} // namespace StickerBatch

void ShowStickerBatch(
		not_null<Window::SessionController*> controller,
		QStringList paths,
		uint64 packId) {
	// A file dialog or a drop may end after the app was locked: the box
	// would be shown above the passcode screen.
	if (controller->window().locked()) {
		return;
	}
	controller->show(Box(StickerBatch::BatchBox, StickerBatch::BoxArgs{
		.show = controller->uiShow(),
		.backend = MakeStickerBatchBackend(controller),
		.paths = std::move(paths),
		.packId = packId,
		.sessionKey = controller->session().uniqueId(),
	}));
}

} // namespace Oblivion
