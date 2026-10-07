/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_deleted_search.h"

#include "base/timer.h"
#include "base/unixtime.h"
#include "core/ui_integration.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "oblivion/oblivion_deleted.h"
#include "oblivion/oblivion_deleted_store.h"
#include "oblivion/oblivion_lang.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/abstract_button.h"
#include "ui/boxes/calendar_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/text/text.h"
#include "ui/text/text_utilities.h"
#include "ui/vertical_list.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/multi_select.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/shadow.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QDateTime>
#include <QtCore/QLocale>

namespace Oblivion::DeletedSearch {
namespace {

constexpr auto kMaxWords = 8;
constexpr auto kMaxWordLength = 64;
constexpr auto kTextScore = 30;
constexpr auto kWordStartBonus = 20;
constexpr auto kWholeWordBonus = 10;
constexpr auto kMediaScore = 15;
constexpr auto kSenderScore = 10;
constexpr auto kChatScore = 6;
constexpr auto kPhraseBonus = 40;
constexpr auto kMaxOccurrences = 8;
constexpr auto kSnippetBefore = 28;
constexpr auto kSnippetLimit = 220;
constexpr auto kMaxChatOptions = 14;
constexpr auto kMaxRowViews = 400;
constexpr auto kRebuildDelay = crl::time(400);

enum Field {
	kFieldText,
	kFieldMedia,
	kFieldSender,
	kFieldChat,
};

[[nodiscard]] bool IsQuote(QChar ch) {
	switch (ch.unicode()) {
	case '"':
	case 0xAB:
	case 0xBB:
	case 0x201C:
	case 0x201D:
	case 0x201E:
		return true;
	}
	return false;
}

[[nodiscard]] TimeId When(const Entry &entry) {
	return entry.changed ? entry.changed : entry.date;
}

// How well a word is found in the fields of an entry, zero if it is not.
[[nodiscard]] int WordScore(
		const std::array<QString, 4> &fields,
		const QString &word) {
	const auto &text = fields[kFieldText];
	const auto length = int(word.size());
	auto best = 0;
	auto from = 0;
	for (auto i = 0; i != kMaxOccurrences; ++i) {
		const auto at = int(text.indexOf(word, from));
		if (at < 0) {
			break;
		}
		const auto end = at + length;
		const auto start = !at || !text[at - 1].isLetterOrNumber();
		const auto whole = start
			&& (end >= text.size() || !text[end].isLetterOrNumber());
		best = std::max(
			best,
			kTextScore
				+ (start ? kWordStartBonus : 0)
				+ (whole ? kWholeWordBonus : 0));
		if (whole) {
			break;
		}
		from = at + 1;
	}
	if (best) {
		return best;
	} else if (fields[kFieldMedia].contains(word)) {
		return kMediaScore;
	} else if (fields[kFieldSender].contains(word)) {
		return kSenderScore;
	} else if (fields[kFieldChat].contains(word)) {
		return kChatScore;
	}
	return 0;
}

} // namespace

QString Fold(const QString &text) {
	auto result = text;
	const auto data = result.data();
	for (auto i = 0, count = int(result.size()); i != count; ++i) {
		const auto ch = data[i];
		if (ch.isSpace() || ch.unicode() == 0xA0 || ch.unicode() < 0x20) {
			data[i] = QChar(' ');
		} else {
			const auto lower = ch.toLower();
			data[i] = (lower.unicode() == 0x451) ? QChar(0x435) : lower;
		}
	}
	return result;
}

std::vector<QString> ParseWords(const QString &query) {
	auto result = std::vector<QString>();
	const auto folded = Fold(query);
	auto current = QString();
	auto quoted = false;
	const auto push = [&] {
		auto word = quoted ? current.simplified() : current;
		current = QString();
		if (word.size() > kMaxWordLength) {
			word = word.left(kMaxWordLength);
		}
		if (!word.isEmpty()
			&& int(result.size()) < kMaxWords
			&& !ranges::contains(result, word)) {
			result.push_back(word);
		}
	};
	for (const auto ch : folded) {
		if (IsQuote(ch)) {
			push();
			quoted = !quoted;
		} else if (!quoted && ch == QChar(' ')) {
			push();
		} else {
			current.append(ch);
		}
	}
	push();
	return result;
}

std::shared_ptr<const Corpus> MakeCorpus(std::vector<Entry> entries) {
	auto result = std::make_shared<Corpus>();
	result->entries = std::move(entries);
	result->folded.reserve(result->entries.size());
	for (const auto &entry : result->entries) {
		result->folded.push_back({
			Fold(entry.text),
			Fold(entry.media),
			Fold(entry.sender),
			Fold(entry.chat),
		});
	}
	return result;
}

std::vector<Match> Search(
		const Corpus &corpus,
		const Query &query,
		const std::atomic<bool> *cancel) {
	const auto words = ParseWords(query.text);
	const auto phrase = (words.size() > 1)
		? Fold(query.text).simplified()
		: QString();
	auto result = std::vector<Match>();
	const auto count = int(corpus.entries.size());
	for (auto i = 0; i != count; ++i) {
		if (cancel
			&& !(i & 0xFF)
			&& cancel->load(std::memory_order_relaxed)) {
			return {};
		}
		const auto &entry = corpus.entries[i];
		if ((entry.source == Source::Deleted)
			? !query.deleted
			: !query.edited) {
			continue;
		} else if (query.peerId && entry.peerId != query.peerId) {
			continue;
		}
		const auto when = When(entry);
		if ((query.from && when < query.from)
			|| (query.till && when >= query.till)) {
			continue;
		}
		auto score = 0;
		if (!words.empty()) {
			const auto &fields = corpus.folded[i];
			auto all = true;
			for (const auto &word : words) {
				const auto value = WordScore(fields, word);
				if (!value) {
					all = false;
					break;
				}
				score += value;
			}
			if (!all) {
				continue;
			} else if (!phrase.isEmpty()
				&& fields[kFieldText].contains(phrase)) {
				score += kPhraseBonus;
			}
		}
		result.push_back({ .index = i, .score = score });
	}
	if (cancel && cancel->load(std::memory_order_relaxed)) {
		return {};
	}
	const auto &entries = corpus.entries;
	std::sort(begin(result), end(result), [&](
			const Match &a,
			const Match &b) {
		if (a.score != b.score) {
			return a.score > b.score;
		}
		const auto whenA = When(entries[a.index]);
		const auto whenB = When(entries[b.index]);
		return (whenA != whenB) ? (whenA > whenB) : (a.index > b.index);
	});
	return result;
}

Snippet MakeSnippet(
		const QString &text,
		const std::vector<QString> &words,
		int limit) {
	auto result = Snippet();
	const auto size = int(text.size());
	if (!size || limit <= 0) {
		return result;
	}
	const auto folded = Fold(text);
	auto first = -1;
	for (const auto &word : words) {
		const auto at = word.isEmpty() ? -1 : int(folded.indexOf(word));
		if (at >= 0 && (first < 0 || at < first)) {
			first = at;
		}
	}
	auto from = 0;
	if (first > kSnippetBefore) {
		from = first - kSnippetBefore;
		const auto space = int(folded.indexOf(QChar(' '), from));
		if (space >= 0 && space < first) {
			from = space + 1;
		}
		if (from < size && text[from].isLowSurrogate()) {
			++from;
		}
	}
	auto till = std::min(size, from + limit);
	if (till < size && till > from && text[till - 1].isHighSurrogate()) {
		--till;
	}
	const auto prefix = (from > 0) ? QString(QChar(0x2026)) : QString();
	auto piece = text.mid(from, till - from);
	for (auto i = 0, count = int(piece.size()); i != count; ++i) {
		if (folded[from + i] == QChar(' ')) {
			piece[i] = QChar(' ');
		}
	}
	result.text = prefix
		+ piece
		+ ((till < size) ? QString(QChar(0x2026)) : QString());

	const auto shift = int(prefix.size()) - from;
	auto found = std::vector<Range>();
	for (const auto &word : words) {
		const auto length = int(word.size());
		if (!length) {
			continue;
		}
		auto at = int(folded.indexOf(word, from));
		while (at >= 0 && at + length <= till) {
			found.push_back({ at + shift, length });
			at = int(folded.indexOf(word, at + length));
		}
	}
	std::sort(begin(found), end(found), [](const Range &a, const Range &b) {
		return (a.offset < b.offset)
			|| (a.offset == b.offset && a.length > b.length);
	});
	for (const auto &range : found) {
		if (!result.ranges.empty()) {
			auto &last = result.ranges.back();
			const auto end = last.offset + last.length;
			if (range.offset <= end) {
				last.length = std::max(end, range.offset + range.length)
					- last.offset;
				continue;
			}
		}
		result.ranges.push_back(range);
	}
	return result;
}

namespace {

enum class PeriodType {
	All,
	Today,
	Week,
	Month,
	Year,
	Day,
};

struct PeriodChoice {
	PeriodType type = PeriodType::All;
	QDate day;
};

struct ChatOption {
	uint64 peerId = 0;
	QString name;
	int count = 0;
};

struct VersionsArgs {
	QString chat;
	QString sender;
	std::vector<EditVersion> versions;
	std::optional<TextWithEntities> current;
	TimeId found = 0; // When the version that was found was replaced.
	TimeId now = 0; // A fixed "now" for the snapshots.
	Ui::Text::MarkedContext context;
	Fn<void()> jump;
};

struct BoxArgs {
	std::shared_ptr<Ui::Show> show;
	Fn<std::vector<Entry>()> collect;

	// The store has changed. True: records were removed (the results may
	// show what is not there any more), false: only added.
	rpl::producer<bool> changes;
	uint64 peerId = 0;
	QString peerName; // Of that chat, it may have no records yet.
	QString query;
	bool deleted = true;
	bool edited = true;
	Fn<void(const Entry&)> open;
	TimeId now = 0; // A fixed "now" for the snapshots.
	bool sync = false; // Search right in the call (the snapshots).
};

[[nodiscard]] int Scaled(int value) {
	return style::ConvertScale(value);
}

[[nodiscard]] QLocale DateLocale() {
	return QLocale(CurrentLanguageIsRussian()
		? QLocale::Russian
		: QLocale::English);
}

[[nodiscard]] QString ShortDate(TimeId when, TimeId now) {
	if (when <= 0) {
		return QString();
	}
	const auto moment = QDateTime::fromSecsSinceEpoch(when);
	const auto today = QDateTime::fromSecsSinceEpoch(now).date();
	if (moment.date() == today) {
		return moment.time().toString(u"HH:mm"_q);
	}
	return DateLocale().toString(
		moment.date(),
		(moment.date().year() == today.year())
			? u"d MMM"_q
			: u"d MMM yyyy"_q).remove(QChar('.'));
}

// The day with the whole name of the month and the time: "6 октября, 11:41".
[[nodiscard]] QString FullDate(TimeId when, TimeId now) {
	if (when <= 0) {
		return QString();
	}
	const auto moment = QDateTime::fromSecsSinceEpoch(when);
	const auto today = QDateTime::fromSecsSinceEpoch(now).date();
	return DateLocale().toString(
		moment.date(),
		(moment.date().year() == today.year())
			? u"d MMMM"_q
			: u"d MMMM yyyy"_q)
		+ u", "_q
		+ moment.time().toString(u"HH:mm"_q);
}

[[nodiscard]] TimeId DayStart(const QDate &date) {
	return TimeId(QDateTime(date, QTime(0, 0)).toSecsSinceEpoch());
}

void ApplyPeriod(Query &query, const PeriodChoice &period, TimeId now) {
	query.from = query.till = 0;
	const auto today = QDateTime::fromSecsSinceEpoch(now).date();
	switch (period.type) {
	case PeriodType::All: break;
	case PeriodType::Today: query.from = DayStart(today); break;
	case PeriodType::Week: query.from = now - 7 * 86400; break;
	case PeriodType::Month: query.from = now - 30 * 86400; break;
	case PeriodType::Year: query.from = now - 365 * 86400; break;
	case PeriodType::Day:
		query.from = DayStart(period.day);
		query.till = DayStart(period.day.addDays(1));
		break;
	}
}

[[nodiscard]] QString PeriodName(const PeriodChoice &period) {
	switch (period.type) {
	case PeriodType::All: return tr::lng_oblivion_dsearch_period_all(tr::now);
	case PeriodType::Today:
		return tr::lng_oblivion_dsearch_period_today(tr::now);
	case PeriodType::Week:
		return tr::lng_oblivion_dsearch_period_week(tr::now);
	case PeriodType::Month:
		return tr::lng_oblivion_dsearch_period_month(tr::now);
	case PeriodType::Year:
		return tr::lng_oblivion_dsearch_period_year(tr::now);
	case PeriodType::Day:
		// The chip is short: the year is there only when it is not this one.
		return DateLocale().toString(
			period.day,
			(period.day.year() == QDate::currentDate().year())
				? u"d MMM"_q
				: u"d MMM yyyy"_q).remove(QChar('.'));
	}
	return QString();
}

[[nodiscard]] QString KindName(bool deleted, bool edited) {
	return (deleted && edited)
		? tr::lng_oblivion_dsearch_kind_all(tr::now)
		: deleted
		? tr::lng_oblivion_dsearch_kind_deleted(tr::now)
		: tr::lng_oblivion_dsearch_kind_edited(tr::now);
}

[[nodiscard]] std::vector<ChatOption> CollectChats(
		const Corpus &corpus,
		uint64 always) {
	auto map = base::flat_map<uint64, ChatOption>();
	for (const auto &entry : corpus.entries) {
		auto &option = map[entry.peerId];
		if (!option.count++) {
			option.peerId = entry.peerId;
			option.name = entry.chat;
		}
	}
	auto result = std::vector<ChatOption>();
	result.reserve(map.size());
	for (const auto &[id, option] : map) {
		result.push_back(option);
	}
	ranges::sort(result, [](const ChatOption &a, const ChatOption &b) {
		return (a.count > b.count)
			|| (a.count == b.count && a.peerId < b.peerId);
	});
	if (int(result.size()) > kMaxChatOptions) {
		const auto i = ranges::find(result, always, &ChatOption::peerId);
		if (always && i != end(result) && (i - begin(result)) >= kMaxChatOptions) {
			result[kMaxChatOptions - 1] = *i;
		}
		result.resize(kMaxChatOptions);
	}
	return result;
}

[[nodiscard]] bool SameRecord(const Entry &a, const Entry &b) {
	return (a.source == b.source)
		&& (a.peerId == b.peerId)
		&& (a.messageId == b.messageId)
		&& (a.date == b.date)
		&& (a.changed == b.changed);
}

// The chat the box was opened from may have nothing saved yet: it still
// has its name on the chip and its place in the menu.
void AddPresetChat(
		std::vector<ChatOption> &chats,
		uint64 peerId,
		const QString &name) {
	if (peerId
		&& !name.isEmpty()
		&& !ranges::contains(chats, peerId, &ChatOption::peerId)) {
		chats.push_back({ .peerId = peerId, .name = name });
	}
}

// The chip: its text, the arrow and what is around them. Three chips with
// their usual texts have to fit in one row of the box.
constexpr auto kChipLeft = 10;
constexpr auto kChipTextSkip = 5;
constexpr auto kChipArrow = 8;
constexpr auto kChipRight = 9;
constexpr auto kChipsSkip = 6;

// A rounded button with a text and a small arrow that opens a menu.
class Chip final : public Ui::AbstractButton {
public:
	explicit Chip(QWidget *parent);

	void setContent(const QString &text, bool active);
	[[nodiscard]] int naturalWidth() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;

private:
	QString _text;
	bool _active = false;

};

Chip::Chip(QWidget *parent)
: AbstractButton(parent) {
	resize(Scaled(60), Scaled(28));
}

void Chip::setContent(const QString &text, bool active) {
	if (_text != text || _active != active) {
		_text = text;
		_active = active;
		update();
	}
}

int Chip::naturalWidth() const {
	return Scaled(kChipLeft)
		+ st::normalFont->width(_text)
		+ Scaled(kChipTextSkip)
		+ Scaled(kChipArrow)
		+ Scaled(kChipRight);
}

void Chip::onStateChanged(State was, StateChangeSource source) {
	update();
}

void Chip::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto radius = height() / 2.;
	p.setPen(Qt::NoPen);
	p.setBrush(_active
		? st::windowBgActive
		: isOver()
		? st::windowBgRipple
		: st::windowBgOver);
	p.drawRoundedRect(rect(), radius, radius);

	const auto &font = st::normalFont;
	const auto left = Scaled(kChipLeft);
	const auto arrow = Scaled(kChipArrow);
	const auto right = Scaled(kChipRight);
	const auto available = width()
		- left
		- Scaled(kChipTextSkip)
		- arrow
		- right;
	const auto color = _active ? st::windowFgActive->c : st::windowFg->c;
	p.setFont(font);
	p.setPen(color);
	p.drawText(
		left,
		(height() - font->height) / 2 + font->ascent,
		(font->width(_text) > available)
			? font->elided(_text, std::max(available, 0))
			: _text);

	auto pen = QPen(
		_active ? st::windowFgActive->c : st::windowSubTextFg->c,
		style::ConvertScaleExact(1.5));
	pen.setCapStyle(Qt::RoundCap);
	pen.setJoinStyle(Qt::RoundJoin);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	const auto x = width() - right - arrow;
	const auto y = height() / 2. - arrow / 5.;
	p.drawPolyline(QPolygonF({
		QPointF(x, y),
		QPointF(x + arrow / 2., y + arrow / 2.),
		QPointF(x + arrow, y),
	}));
}

class Filters final : public Ui::RpWidget {
public:
	explicit Filters(QWidget *parent);

	[[nodiscard]] not_null<Chip*> chat() const {
		return _chat;
	}
	[[nodiscard]] not_null<Chip*> period() const {
		return _period;
	}
	[[nodiscard]] not_null<Chip*> kind() const {
		return _kind;
	}
	void relayout() {
		resizeToWidth(width());
	}

protected:
	int resizeGetHeight(int newWidth) override;

private:
	const not_null<Chip*> _chat;
	const not_null<Chip*> _period;
	const not_null<Chip*> _kind;

};

Filters::Filters(QWidget *parent)
: RpWidget(parent)
, _chat(Ui::CreateChild<Chip>(this))
, _period(Ui::CreateChild<Chip>(this))
, _kind(Ui::CreateChild<Chip>(this)) {
}

int Filters::resizeGetHeight(int newWidth) {
	const auto &padding = st::boxRowPadding;
	const auto skip = Scaled(kChipsSkip);
	const auto height = _chat->height();
	const auto available = std::max(
		newWidth - padding.left() - padding.right(),
		0);

	// The chips keep their texts whole: with the long name of a chat the
	// one that does not fit goes to the next row. Only a chip that is
	// wider than the whole row is cut.
	auto left = 0;
	auto top = Scaled(6);
	for (const auto &chip : { _chat, _period, _kind }) {
		const auto width = std::min(chip->naturalWidth(), available);
		if (left > 0 && available > 0 && left + width > available) {
			left = 0;
			top += height + skip;
		}
		chip->setGeometry(padding.left() + left, top, width, height);
		left += width + skip;
	}
	return top + height + Scaled(10);
}

// The results: rows of one height, only the visible ones are painted.
class ResultsList final : public Ui::RpWidget {
public:
	ResultsList(QWidget *parent, TimeId now);

	void setResults(
		std::shared_ptr<const Corpus> corpus,
		std::vector<Match> &&matches,
		std::vector<QString> words);
	void setSearching(bool searching);

	// The entry itself, not its place: the box may have newer entries
	// already while this list still shows the results for the old ones.
	[[nodiscard]] rpl::producer<Entry> activated() const {
		return _activated.events();
	}

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	struct RowView {
		QString chat;
		QString date;
		QString badge;
		QString sender;
		Ui::Text::String snippet;
		bool deleted = false;
	};

	[[nodiscard]] int rowHeight() const;
	[[nodiscard]] int rowAt(QPoint position) const;
	[[nodiscard]] RowView &viewFor(int row);
	void setHovered(int row);
	void paintEmpty(Painter &p);

	const TimeId _now = 0;
	std::shared_ptr<const Corpus> _corpus;
	std::vector<Match> _matches;
	std::vector<QString> _words;
	base::flat_map<int, std::unique_ptr<RowView>> _views;
	rpl::event_stream<Entry> _activated;
	int _hovered = -1;
	int _pressed = -1;
	bool _searching = false;

};

ResultsList::ResultsList(QWidget *parent, TimeId now)
: RpWidget(parent)
, _now(now) {
	setMouseTracking(true);
}

int ResultsList::rowHeight() const {
	return Scaled(8)
		+ st::semiboldFont->height
		+ st::normalFont->height
		+ Scaled(2)
		+ 2 * st::defaultTextStyle.font->height
		+ Scaled(9)
		+ st::lineWidth;
}

int ResultsList::resizeGetHeight(int newWidth) {
	return std::max(int(_matches.size()) * rowHeight(), Scaled(300));
}

void ResultsList::setResults(
		std::shared_ptr<const Corpus> corpus,
		std::vector<Match> &&matches,
		std::vector<QString> words) {
	// The results may be replaced under the cursor (the store has
	// changed). A row that is being clicked stays so only while it shows
	// the very same record, a click never opens another one.
	const auto entryAt = [&](int row) {
		return (row >= 0 && row < int(_matches.size()) && _corpus)
			? &_corpus->entries[_matches[row].index]
			: nullptr;
	};
	const auto pressed = _pressed;
	const auto was = entryAt(pressed);
	const auto wasEntry = was ? std::make_optional(*was) : std::nullopt;

	_corpus = std::move(corpus);
	_matches = std::move(matches);
	_words = std::move(words);

	// A widget can't be taller than QWIDGETSIZE_MAX, with a full store
	// and a large interface scale the rows would not fit. What is cut is
	// the end of the list: the weakest matches, the oldest records.
	const auto limit = std::max(QWIDGETSIZE_MAX / rowHeight() - 1, 1);
	if (int(_matches.size()) > limit) {
		_matches.resize(limit);
	}
	_views.clear();
	_searching = false;
	_hovered = _pressed = -1;
	resizeToWidth(width());

	const auto now = entryAt(pressed);
	if (wasEntry && now && SameRecord(*wasEntry, *now)) {
		_pressed = pressed;
	}
	const auto under = underMouse()
		? rowAt(mapFromGlobal(QCursor::pos()))
		: -1;
	if (under >= 0) {
		setHovered(under);
	} else {
		setCursor(style::cur_default);
	}
	update();
}

void ResultsList::setSearching(bool searching) {
	if (_searching != searching) {
		_searching = searching;
		if (_matches.empty()) {
			update();
		}
	}
}

int ResultsList::rowAt(QPoint position) const {
	const auto row = (position.y() >= 0) ? (position.y() / rowHeight()) : -1;
	return (row >= 0 && row < int(_matches.size())) ? row : -1;
}

void ResultsList::setHovered(int row) {
	if (_hovered == row) {
		return;
	}
	const auto height = rowHeight();
	if (_hovered >= 0) {
		update(0, _hovered * height, width(), height);
	}
	_hovered = row;
	if (_hovered >= 0) {
		update(0, _hovered * height, width(), height);
	}
	setCursor((_hovered >= 0) ? style::cur_pointer : style::cur_default);
}

void ResultsList::mouseMoveEvent(QMouseEvent *e) {
	setHovered(rowAt(e->pos()));
}

void ResultsList::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = rowAt(e->pos());
	}
}

void ResultsList::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, -1);
	if (e->button() == Qt::LeftButton
		&& pressed >= 0
		&& pressed == rowAt(e->pos())
		&& _corpus) {
		const auto index = _matches[pressed].index;
		if (index >= 0 && index < int(_corpus->entries.size())) {
			_activated.fire_copy(_corpus->entries[index]);
		}
	}
}

void ResultsList::leaveEventHook(QEvent *e) {
	setHovered(-1);
}

ResultsList::RowView &ResultsList::viewFor(int row) {
	// The found words are bold and colored: both kinds of marks are kept
	// only together with TextParseMarkdown (nothing is parsed from the
	// text itself, the marks come ready).
	static const auto kOptions = TextParseOptions{
		TextParseColorized | TextParseMarkdown,
		0,
		0,
		Qt::LayoutDirectionAuto,
	};
	const auto i = _views.find(row);
	if (i != end(_views)) {
		return *i->second;
	} else if (int(_views.size()) >= kMaxRowViews) {
		_views.clear();
	}
	const auto &entry = _corpus->entries[_matches[row].index];
	auto view = std::make_unique<RowView>();
	view->deleted = (entry.source == Source::Deleted);
	view->chat = entry.chat;
	view->date = ShortDate(entry.date, _now);
	view->badge = view->deleted
		? tr::lng_oblivion_dsearch_badge_deleted(tr::now)
		: tr::lng_oblivion_dsearch_badge_edited(tr::now);
	// The day (or the minute) the row shows on the right is not repeated.
	const auto changed = ShortDate(entry.changed, _now);
	if (!changed.isEmpty() && changed != view->date) {
		view->badge += ' ' + changed;
	}
	view->sender = entry.sender;

	const auto snippet = MakeSnippet(
		entry.text.isEmpty() ? entry.media : entry.text,
		_words,
		kSnippetLimit);
	auto marked = TextWithEntities();
	auto from = 0;
	for (const auto &range : snippet.ranges) {
		marked.append(snippet.text.mid(from, range.offset - from));
		marked.append(Ui::Text::Colorized(
			tr::bold(snippet.text.mid(range.offset, range.length))));
		from = range.offset + range.length;
	}
	marked.append(snippet.text.mid(from));
	if (!entry.text.isEmpty() && !entry.media.isEmpty()) {
		marked.append(u" · "_q + entry.media);
	}
	view->snippet.setMarkedText(st::defaultTextStyle, marked, kOptions);
	return *_views.emplace(row, std::move(view)).first->second;
}

void ResultsList::paintEmpty(Painter &p) {
	const auto empty = !_corpus || _corpus->entries.empty();
	const auto title = (_searching || !_corpus)
		? tr::lng_oblivion_dsearch_searching(tr::now)
		: empty
		? tr::lng_oblivion_dsearch_nothing(tr::now)
		: tr::lng_oblivion_dsearch_empty(tr::now);
	const auto hint = (_searching || !_corpus)
		? QString()
		: empty
		? tr::lng_oblivion_dsearch_nothing_hint(tr::now)
		: tr::lng_oblivion_dsearch_empty_hint(tr::now);
	const auto &padding = st::boxRowPadding;
	const auto available = width() - padding.left() - padding.right();
	auto top = Scaled(72);
	p.setFont(st::semiboldFont);
	p.setPen(st::windowBoldFg);
	p.drawText(
		QRect(padding.left(), top, available, st::semiboldFont->height),
		Qt::AlignHCenter | Qt::AlignTop,
		title);
	if (hint.isEmpty()) {
		return;
	}
	top += st::semiboldFont->height + Scaled(8);
	p.setFont(st::normalFont);
	p.setPen(st::windowSubTextFg);
	p.drawText(
		QRect(padding.left(), top, available, 4 * st::normalFont->height),
		Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
		hint);
}

void ResultsList::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto clip = e->rect();
	if (_matches.empty()) {
		paintEmpty(p);
		return;
	}
	const auto height = rowHeight();
	const auto &padding = st::boxRowPadding;
	const auto left = padding.left();
	const auto right = padding.right();
	const auto available = width() - left - right;
	const auto from = std::max(clip.top() / height, 0);
	const auto till = std::min(
		(clip.top() + clip.height() + height - 1) / height,
		int(_matches.size()));
	const auto &name = st::semiboldFont;
	const auto &plain = st::normalFont;
	const auto count = int(_matches.size());
	for (auto row = from; row < till; ++row) {
		const auto &view = viewFor(row);
		const auto y = row * height;
		if (row == _hovered) {
			p.fillRect(0, y, width(), height - st::lineWidth, st::windowBgOver);
		}
		auto top = y + Scaled(8);

		const auto dateWidth = plain->width(view.date);
		const auto chatWidth = available - dateWidth - Scaled(12);
		p.setFont(name);
		p.setPen(st::windowBoldFg);
		p.drawTextLeft(
			left,
			top,
			width(),
			(name->width(view.chat) > chatWidth)
				? name->elided(view.chat, std::max(chatWidth, 0))
				: view.chat);
		p.setFont(plain);
		p.setPen(st::windowSubTextFg);
		p.drawTextRight(right, top, width(), view.date, dateWidth);
		top += name->height;

		p.setPen(view.deleted
			? st::attentionButtonFg
			: st::windowActiveTextFg);
		p.drawTextLeft(left, top, width(), view.badge);
		if (!view.sender.isEmpty()) {
			const auto badgeWidth = plain->width(view.badge);
			const auto sender = u" · "_q + view.sender;
			const auto senderWidth = available - badgeWidth;
			p.setPen(st::windowSubTextFg);
			p.drawTextLeft(
				left + badgeWidth,
				top,
				width(),
				(plain->width(sender) > senderWidth)
					? plain->elided(sender, std::max(senderWidth, 0))
					: sender);
		}
		top += plain->height + Scaled(2);

		p.setPen(st::windowFg);
		view.snippet.draw(p, {
			.position = QPoint(left, top),
			.outerWidth = width(),
			.availableWidth = available,
			.elisionLines = 2,
		});

		// The lines are between the rows, not under the last one.
		if (row + 1 < count) {
			p.fillRect(
				left,
				y + height - st::lineWidth,
				available,
				st::lineWidth,
				st::shadowFg);
		}
	}
}

void VersionsBox(not_null<Ui::GenericBox*> box, VersionsArgs &&args) {
	box->setWidth(st::boxWideWidth);
	box->setMaxHeight(st::boxMaxListHeight);
	box->setTitle(tr::lng_oblivion_dsearch_versions_title());

	const auto content = box->verticalLayout();
	const auto &padding = st::boxRowPadding;
	auto header = tr::bold(args.chat);
	if (!args.sender.isEmpty() && args.sender != args.chat) {
		header.append(u" · "_q + args.sender);
	}
	// The texts are as long as messages are: they take the lines they need
	// (a label of a style without the minimal width is one line tall
	// whatever it is given, the rest of its text is cut away).
	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			rpl::single(header),
			st::boxLabel),
		padding);

	const auto now = args.now ? args.now : base::unixtime::now();
	auto first = true;
	const auto addVersion = [&](
			const QString &title,
			const TextWithEntities &text,
			bool found) {
		// The lines are between the versions, not under the last one.
		if (!std::exchange(first, false)) {
			auto line = object_ptr<Ui::PlainShadow>(content);
			line->resize(line->width(), st::lineWidth);
			content->add(std::move(line), padding);
		}
		Ui::AddSkip(content, st::boxLittleSkip);
		const auto label = content->add(
			object_ptr<Ui::FlatLabel>(
				content,
				title,
				st::defaultSubTextLabel),
			padding);
		if (found) {
			label->setTextColorOverride(st::windowActiveTextFg->c);
		}
		const auto body = content->add(
			object_ptr<Ui::FlatLabel>(content, st::boxLabel),
			style::margins(
				padding.left(),
				st::boxLittleSkip / 4,
				padding.right(),
				st::boxLittleSkip));
		body->setMarkedText(text, args.context);
		body->setSelectable(true);
	};
	for (const auto &version : args.versions) {
		addVersion(
			tr::lng_oblivion_dsearch_version_was(
				tr::now,
				lt_date,
				FullDate(version.replaced, now)),
			version.text,
			args.found && (version.replaced == args.found));
	}
	if (args.current) {
		addVersion(
			tr::lng_oblivion_dsearch_version_now(tr::now),
			*args.current,
			false);
	}

	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	if (const auto jump = args.jump) {
		box->addLeftButton(tr::lng_oblivion_dsearch_jump(), jump);
	}
}

void SearchBox(not_null<Ui::GenericBox*> box, BoxArgs &&args) {
	struct State {
		std::shared_ptr<const Corpus> corpus;
		std::vector<ChatOption> chats;
		Query query;
		PeriodChoice period;
		std::shared_ptr<std::atomic<bool>> cancel;
		base::unique_qptr<Ui::PopupMenu> menu;
		rpl::variable<QString> count;
		base::Timer rebuild;
		int corpusGeneration = 0;
		int searchGeneration = 0;

		// The store got new records while the list was scrolled down:
		// they are shown when it is back at the top or the search is
		// changed, the list is not rebuilt under the reader.
		bool stale = false;

		// The user has changed the search: its results start from the
		// top. A refresh in the background leaves the list where it is.
		bool scrollToTop = false;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto show = args.show;
	const auto collect = args.collect;
	const auto open = args.open;
	const auto sync = args.sync;
	const auto fixedNow = args.now;
	const auto presetId = args.peerId;
	const auto presetName = args.peerName;
	const auto now = [=] {
		return fixedNow ? fixedNow : base::unixtime::now();
	};
	state->query.text = args.query.trimmed();
	state->query.peerId = args.peerId;
	state->query.deleted = args.deleted || !args.edited;
	state->query.edited = args.edited;

	box->setWidth(st::boxWideWidth);
	box->setMaxHeight(st::boxMaxListHeight);
	box->setTitle(tr::lng_oblivion_dsearch_title());
	box->setAdditionalTitle(state->count.value());

	const auto top = box->setPinnedToTopContent(
		object_ptr<Ui::VerticalLayout>(box));
	const auto field = top->add(object_ptr<Ui::MultiSelect>(
		top,
		st::defaultMultiSelect,
		tr::lng_oblivion_dsearch_placeholder()));
	const auto filters = top->add(object_ptr<Filters>(top));
	box->setFocusCallback([=] {
		field->setInnerFocus();
	});

	const auto list = box->verticalLayout()->add(
		object_ptr<ResultsList>(box->verticalLayout(), now()));
	list->setSearching(true);

	box->lifetime().add([=] {
		if (const auto cancel = state->cancel) {
			cancel->store(true);
		}
	});

	// The guard for what comes back from the other thread is made here,
	// on the main one.
	const auto weak = base::make_weak(box.get());
	const auto runSearch = [=] {
		if (const auto cancel = base::take(state->cancel)) {
			cancel->store(true);
		}
		const auto corpus = state->corpus;
		if (!corpus) {
			return;
		}
		auto query = state->query;
		ApplyPeriod(query, state->period, now());
		const auto generation = ++state->searchGeneration;
		const auto words = ParseWords(query.text);
		const auto apply = [=](std::vector<Match> &&matches) {
			if (state->searchGeneration != generation) {
				return;
			}
			state->count = matches.empty()
				? QString()
				: QString::number(matches.size());
			list->setResults(corpus, std::move(matches), words);
			if (base::take(state->scrollToTop)) {
				box->scrollToY(0);
			}
		};
		if (sync) {
			apply(Search(*corpus, query));
			return;
		}
		const auto cancel = std::make_shared<std::atomic<bool>>(false);
		state->cancel = cancel;
		list->setSearching(true);
		crl::async([=] {
			auto matches = Search(*corpus, query, cancel.get());
			crl::on_main(weak, [=, matches = std::move(matches)]() mutable {
				apply(std::move(matches));
			});
		});
	};
	const auto refreshChips = [=] {
		const auto peerId = state->query.peerId;
		auto chat = tr::lng_oblivion_dsearch_chat_all(tr::now);
		if (peerId) {
			const auto i = ranges::find(
				state->chats,
				peerId,
				&ChatOption::peerId);
			// The chat the search was opened from is "this chat", the way
			// the search of Telegram calls it: the three chips with their
			// short texts stay in one row. The name is in the menu.
			chat = (peerId == presetId)
				? tr::lng_oblivion_dsearch_chat_this(tr::now)
				: (i != end(state->chats) && !i->name.isEmpty())
				? i->name
				: tr::lng_oblivion_dsearch_unknown_chat(tr::now);
		}
		filters->chat()->setContent(chat, peerId != 0);
		filters->period()->setContent(
			PeriodName(state->period),
			state->period.type != PeriodType::All);
		filters->kind()->setContent(
			KindName(state->query.deleted, state->query.edited),
			!state->query.deleted || !state->query.edited);
		filters->relayout();
	};
	const auto applyCorpus = [=](
			std::shared_ptr<const Corpus> corpus,
			int generation) {
		if (state->corpusGeneration != generation) {
			return;
		}
		state->chats = CollectChats(*corpus, state->query.peerId);
		AddPresetChat(state->chats, presetId, presetName);
		state->corpus = std::move(corpus);
		refreshChips();
		runSearch();
	};
	const auto rebuild = [=] {
		if (!collect) {
			return;
		}
		state->stale = false;
		auto entries = collect();
		const auto generation = ++state->corpusGeneration;
		if (sync) {
			applyCorpus(MakeCorpus(std::move(entries)), generation);
			return;
		}
		crl::async([=, entries = std::move(entries)]() mutable {
			auto corpus = MakeCorpus(std::move(entries));
			crl::on_main(weak, [=, corpus = std::move(corpus)] {
				applyCorpus(corpus, generation);
			});
		});
	};
	state->rebuild.setCallback(rebuild);

	// The search was changed by the user: the results start from the
	// top, and it is the moment to take in what the store got meanwhile.
	const auto searchAnew = [=] {
		state->scrollToTop = true;
		if (state->stale) {
			state->rebuild.cancel();
			rebuild();
		} else {
			runSearch();
		}
	};

	const auto showMenu = [=](Fn<void(not_null<Ui::PopupMenu*>)> fill) {
		state->menu = base::make_unique_q<Ui::PopupMenu>(
			box.get(),
			st::defaultPopupMenu);
		fill(state->menu.get());
		state->menu->popup(QCursor::pos());
	};
	filters->chat()->setClickedCallback([=] {
		showMenu([=](not_null<Ui::PopupMenu*> menu) {
			const auto choose = [=](uint64 peerId) {
				return [=] {
					state->query.peerId = peerId;
					refreshChips();
					searchAnew();
				};
			};
			menu->addAction(
				tr::lng_oblivion_dsearch_chat_all(tr::now),
				choose(0));
			for (const auto &option : state->chats) {
				menu->addAction(
					(option.name.isEmpty()
						? tr::lng_oblivion_dsearch_unknown_chat(tr::now)
						: option.name)
						+ u" · "_q
						+ QString::number(option.count),
					choose(option.peerId));
			}
		});
	});
	filters->period()->setClickedCallback([=] {
		showMenu([=](not_null<Ui::PopupMenu*> menu) {
			const auto choose = [=](PeriodType type) {
				return [=] {
					state->period = { .type = type };
					refreshChips();
					searchAnew();
				};
			};
			for (const auto type : {
				PeriodType::All,
				PeriodType::Today,
				PeriodType::Week,
				PeriodType::Month,
				PeriodType::Year,
			}) {
				menu->addAction(PeriodName({ .type = type }), choose(type));
			}
			menu->addAction(tr::lng_oblivion_dsearch_period_day(tr::now), [=] {
				const auto today = QDateTime::fromSecsSinceEpoch(
					now()).date();
				const auto current = (state->period.type == PeriodType::Day)
					? state->period.day
					: today;
				show->showBox(Box<Ui::CalendarBox>(Ui::CalendarBoxArgs{
					.month = current,
					.highlighted = current,
					.callback = crl::guard(box, [=](
							QDate date,
							Fn<void()> close) {
						state->period = {
							.type = PeriodType::Day,
							.day = date,
						};
						refreshChips();
						searchAnew();
						close();
					}),
					.maxDate = today,
				}));
			});
		});
	});
	filters->kind()->setClickedCallback([=] {
		showMenu([=](not_null<Ui::PopupMenu*> menu) {
			const auto choose = [=](bool deleted, bool edited) {
				return [=] {
					state->query.deleted = deleted;
					state->query.edited = edited;
					refreshChips();
					searchAnew();
				};
			};
			menu->addAction(KindName(true, true), choose(true, true));
			menu->addAction(KindName(true, false), choose(true, false));
			menu->addAction(KindName(false, true), choose(false, true));
		});
	});

	if (!state->query.text.isEmpty()) {
		field->setQuery(state->query.text);
	}
	field->setQueryChangedCallback([=](const QString &query) {
		const auto text = query.trimmed();
		if (state->query.text != text) {
			state->query.text = text;
			searchAnew();
		}
	});
	field->setSubmittedCallback([](Qt::KeyboardModifiers) {
	});

	list->activated(
	) | rpl::on_next([=](const Entry &entry) {
		if (open) {
			open(entry);
		}
	}, list->lifetime());

	const auto rebuildSoon = [=] {
		if (!state->rebuild.isActive()) {
			state->rebuild.callOnce(kRebuildDelay);
		}
	};
	std::move(
		args.changes
	) | rpl::on_next([=](bool removed) {
		if (!removed && box->scrollTop() > 0) {
			// Messages are deleted somewhere all the time. The list is not
			// rebuilt under the reader for that: the new records appear
			// when it is back at the top or the search is changed.
			state->stale = true;
			return;
		}
		rebuildSoon();
	}, box->lifetime());
	box->setInitScrollCallback([=] {
		box->scrolls(
		) | rpl::on_next([=] {
			if (state->stale && box->scrollTop() <= 0) {
				rebuildSoon();
			}
		}, box->lifetime());
	});

	refreshChips();
	rebuild();

	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

[[nodiscard]] std::vector<Entry> CollectEntries(
		not_null<Main::Session*> session,
		not_null<DeletedStore*> store) {
	const auto owner = &session->data();
	const auto self = session->userPeerId().value;
	const auto unknown = tr::lng_oblivion_dsearch_unknown_chat(tr::now);
	auto chats = base::flat_map<uint64, QString>();
	const auto chatName = [&](uint64 peerId, const QString &saved) {
		const auto i = chats.find(peerId);
		if (i != end(chats)) {
			return i->second;
		}
		const auto peer = owner->peerLoaded(PeerId(peerId));
		auto name = !peer
			? saved
			: peer->isSelf()
			? tr::lng_saved_messages(tr::now)
			: peer->name();
		if (name.isEmpty()) {
			name = unknown;
		}
		chats.emplace(peerId, name);
		return name;
	};
	const auto you = tr::lng_from_you(tr::now);
	const auto senderName = [&](uint64 senderId, const QString &saved) {
		if (!senderId) {
			return saved;
		} else if (senderId == self) {
			return you;
		} else if (const auto peer = owner->peerLoaded(PeerId(senderId))) {
			return peer->name();
		}
		return saved;
	};

	auto result = std::vector<Entry>();
	const auto &deleted = store->deleted();
	const auto edited = store->edited();
	auto versions = size_t(0);
	for (const auto &message : edited) {
		versions += message.versions.size();
	}
	result.reserve(deleted.size() + versions);
	for (const auto &record : deleted) {
		result.push_back({
			.source = Source::Deleted,
			.peerId = record.peerId,
			.messageId = record.messageId,
			.chat = chatName(record.peerId, record.chatName),
			.sender = senderName(record.senderId, record.senderName),
			.text = record.text.text,
			.media = record.media.type.isEmpty()
				? QString()
				: DeletedMediaLabel(record.media),
			.date = record.date,
			.changed = record.deleted,
		});
	}
	for (const auto &message : edited) {
		const auto item = owner->message(
			PeerId(message.peerId),
			MsgId(message.messageId));
		const auto chat = chatName(message.peerId, QString());
		const auto sender = !item
			? QString()
			: item->out()
			? you
			: item->from()->name();
		for (const auto &version : message.versions) {
			result.push_back({
				.source = Source::Edited,
				.peerId = message.peerId,
				.messageId = message.messageId,
				.chat = chat,
				.sender = sender,
				.text = version.text.text,
				.date = version.since,
				.changed = version.replaced,
			});
		}
	}
	return result;
}

class Checker final {
public:
	explicit Checker(QStringList &log) : _log(log) {
	}

	void operator()(bool condition, const char *what) {
		if (condition) {
			++_passed;
		} else {
			++_failed;
			_log.push_back(u"FAILED: "_q + QString::fromUtf8(what));
		}
	}
	void section(const char *name) {
		_log.push_back(u"%1: %2 passed, %3 failed"_q.arg(
			QString::fromUtf8(name),
			QString::number(_passed - _sectionPassed),
			QString::number(_failed - _sectionFailed)));
		_sectionPassed = _passed;
		_sectionFailed = _failed;
	}
	[[nodiscard]] int failed() const {
		return _failed;
	}

private:
	QStringList &_log;
	int _passed = 0;
	int _failed = 0;
	int _sectionPassed = 0;
	int _sectionFailed = 0;

};

[[nodiscard]] QString SampleText(const char *ru, const char *en) {
	return QString::fromUtf8(CurrentLanguageIsRussian() ? ru : en);
}

constexpr auto kSampleNow = TimeId(1791362460); // 7 October 2026.

[[nodiscard]] std::vector<Entry> SampleEntries() {
	const auto now = kSampleNow;
	const auto hour = TimeId(3600);
	const auto day = TimeId(86400);
	const auto anna = SampleText("Аня Смирнова", "Anna Smirnova");
	const auto club = SampleText("Походы и горы", "Hiking club");
	const auto boris = SampleText("Борис", "Boris");
	const auto you = SampleText("Вы", "You");
	return {
		{
			.source = Source::Deleted,
			.peerId = 101,
			.messageId = 5001,
			.chat = anna,
			.sender = anna,
			.text = SampleText(
				"Купила билеты на субботу, вагон 7, места 21 и 22. "
				"Только никому пока не говори, это сюрприз!",
				"Got the tickets for Saturday, car 7, seats 21 and 22. "
				"Just don't tell anyone yet, it's a surprise!"),
			.date = now - 2 * hour,
			.changed = now - hour,
		},
		{
			.source = Source::Edited,
			.peerId = 102,
			.messageId = 7310,
			.chat = club,
			.sender = boris,
			.text = SampleText(
				"Сбор в 6:30 у касс, билеты у меня. Кто опоздает, "
				"догоняет на следующей электричке.",
				"We meet at 6:30 at the ticket office, I have the "
				"tickets. Whoever is late takes the next train."),
			.date = now - day - 3 * hour,
			.changed = now - day - 2 * hour,
		},
		{
			.source = Source::Deleted,
			.peerId = 102,
			.messageId = 7302,
			.chat = club,
			.sender = you,
			.text = SampleText(
				"Скинул маршрут и билет в PDF, посмотрите до вечера.",
				"Sent the route and the ticket as a PDF, have a look "
				"before the evening."),
			.media = SampleText(
				"Файл · Маршрут и билет.pdf · 1,2 МБ",
				"File · Route and ticket.pdf · 1.2 MB"),
			.date = now - 3 * day,
			.changed = now - 3 * day + 600,
		},
		{
			.source = Source::Deleted,
			.peerId = 103,
			.messageId = 911,
			.chat = boris,
			.sender = boris,
			.text = SampleText(
				"Ладно, забудь, сам разберусь.",
				"Never mind, I'll sort it out myself."),
			.date = now - 9 * day,
			.changed = now - 9 * day + 40,
		},
		{
			.source = Source::Edited,
			.peerId = 101,
			.messageId = 4990,
			.chat = anna,
			.sender = you,
			.text = SampleText(
				"Обратный билет возьму сам, не переживай.",
				"I'll get the return ticket myself, don't worry."),
			.date = now - 40 * day,
			.changed = now - 40 * day + 120,
		},
	};
}

[[nodiscard]] object_ptr<Ui::BoxContent> SampleSearchBox(
		std::shared_ptr<Ui::Show> show,
		const QString &query,
		bool withEntries,
		uint64 peerId = 0,
		const QString &peerName = QString()) {
	return Box(SearchBox, BoxArgs{
		.show = show,
		.collect = [=] {
			return withEntries ? SampleEntries() : std::vector<Entry>();
		},
		.changes = rpl::never<bool>(),
		.peerId = peerId,
		.peerName = peerName,
		.query = query,
		.now = kSampleNow,
		.sync = true,
	});
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	const auto size = QSize(st::boxWideWidth * 2, 0);
	RegisterBoxScene(u"deleted_search_results"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return SampleSearchBox(show, SampleText("билет", "ticket"), true);
	});
	RegisterBoxScene(u"deleted_search_browse"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return SampleSearchBox(show, QString(), true, 102);
	});
	RegisterBoxScene(u"deleted_search_not_found"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return SampleSearchBox(
			show,
			SampleText("паспорт", "passport"),
			true);
	});
	RegisterBoxScene(u"deleted_search_nothing_saved"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return SampleSearchBox(show, QString(), false);
	});
	RegisterBoxScene(u"deleted_search_chat_without_records"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return SampleSearchBox(
			show,
			QString(),
			true,
			104,
			SampleText("Мама", "Mom"));
	});
	RegisterBoxScene(u"deleted_search_versions"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		const auto now = kSampleNow;
		return Box(VersionsBox, VersionsArgs{
			.chat = SampleText("Походы и горы", "Hiking club"),
			.sender = SampleText("Борис", "Boris"),
			.versions = {
				{
					.since = now - 90000,
					.replaced = now - 88200,
					.text = { SampleText(
						"Сбор в 7:00 у касс.",
						"We meet at 7:00 at the ticket office.") },
				},
				{
					.since = now - 88200,
					.replaced = now - 86400,
					.text = { SampleText(
						"Сбор в 6:30 у касс, билеты у меня. Кто "
						"опоздает, догоняет на следующей электричке.",
						"We meet at 6:30 at the ticket office, I have "
						"the tickets. Whoever is late takes the next "
						"train.") },
				},
			},
			.current = TextWithEntities{ SampleText(
				"Сбор в 6:30 у касс, билеты у меня.",
				"We meet at 6:30 at the ticket office, I have the "
				"tickets.") },
			.found = now - 86400,
			.now = now,
			.jump = [] {},
		});
	});
});

} // namespace

void Show(
		not_null<Window::SessionController*> controller,
		PeerData *peer) {
	const auto session = &controller->session();
	const auto store = &StoreFor(session);
	const auto weak = base::make_weak(controller);
	const auto gone = [=] {
		if (const auto strong = weak.get()) {
			strong->showToast(tr::lng_oblivion_dsearch_gone(tr::now));
		}
	};
	const auto open = [=](const Entry &entry) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		const auto peerId = PeerId(entry.peerId);
		const auto messageId = MsgId(entry.messageId);
		if (entry.source == Source::Deleted) {
			for (const auto &record : store->deleted()) {
				if (record.peerId == entry.peerId
					&& record.messageId == entry.messageId) {
					ShowDeletedRecord(strong, record);
					return;
				}
			}
			gone();
			return;
		}
		auto versions = store->edits(entry.peerId, entry.messageId);
		if (versions.empty()) {
			gone();
			return;
		}
		const auto item = session->data().message(peerId, messageId);
		auto jump = Fn<void()>();
		if (session->data().peerLoaded(peerId)) {
			jump = [=] {
				if (const auto strong = weak.get()) {
					strong->hideLayer();
					strong->showPeerHistory(
						peerId,
						Window::SectionShow::Way::Forward,
						messageId);
				}
			};
		}
		strong->show(Box(VersionsBox, VersionsArgs{
			.chat = entry.chat,
			.sender = entry.sender,
			.versions = std::move(versions),
			.current = (item
				? std::make_optional(item->originalText())
				: std::nullopt),
			.found = entry.changed,
			.context = Core::TextContext({ .session = session }),
			.jump = std::move(jump),
		}));
	};
	controller->show(Box(SearchBox, BoxArgs{
		.show = controller->uiShow(),
		.collect = [=] { return CollectEntries(session, store); },
		.changes = store->deletedChanges(
		) | rpl::map([](const DeletedChange &change) {
			return change.removed;
		}),
		.peerId = peer ? peer->id.value : uint64(),
		.peerName = (!peer
			? QString()
			: peer->isSelf()
			? tr::lng_saved_messages(tr::now)
			: peer->name()),
		.open = open,
	}));
}

bool RunSelfTest(QStringList &log) {
	auto check = Checker(log);

	check(Fold(u"ПрИвЕт Ёжик"_q) == u"привет ежик"_q, "fold: case and yo");
	check(Fold(u"a\tb\nc"_q) == u"a b c"_q, "fold: spaces");
	check(Fold(u"Straße"_q).size() == 6, "fold: the length is kept");
	{
		const auto words = ParseWords(u"  Билет  на \"В Субботу\" билет"_q);
		check(words.size() == 3, "words: count");
		check(words.size() == 3
			&& words[0] == u"билет"_q
			&& words[1] == u"на"_q
			&& words[2] == u"в субботу"_q,
			"words: a phrase in quotes, no repeats");
		check(ParseWords(u"   "_q).empty(), "words: only spaces");
		check(ParseWords(u"«ёлка»"_q) == std::vector<QString>{ u"елка"_q },
			"words: other quotes");
		check(ParseWords(u"\"не закрыта"_q)
			== std::vector<QString>{ u"не закрыта"_q },
			"words: a quote that is not closed");
		check(int(ParseWords(u"a b c d e f g h i j k"_q).size()) == kMaxWords,
			"words: limit");
	}
	check.section("query");

	auto entries = std::vector<Entry>{
		{ // 0
			.source = Source::Deleted,
			.peerId = 1,
			.messageId = 10,
			.chat = u"Anna"_q,
			.sender = u"Anna"_q,
			.text = u"Купила билет на субботу"_q,
			.date = 1000,
			.changed = 1100,
		},
		{ // 1: the word in the middle of another one.
			.source = Source::Deleted,
			.peerId = 1,
			.messageId = 11,
			.chat = u"Anna"_q,
			.sender = u"Anna"_q,
			.text = u"Это безбилетник"_q,
			.date = 2000,
			.changed = 2100,
		},
		{ // 2: only in the file name.
			.source = Source::Deleted,
			.peerId = 2,
			.messageId = 20,
			.chat = u"Club"_q,
			.sender = u"Boris"_q,
			.text = u"Смотрите файл"_q,
			.media = u"File · Билет.pdf"_q,
			.date = 3000,
			.changed = 3100,
		},
		{ // 3: an old text of an edited message, newer than 0.
			.source = Source::Edited,
			.peerId = 2,
			.messageId = 21,
			.chat = u"Club"_q,
			.sender = u"Boris"_q,
			.text = u"БИЛЕТ НА СУББОТУ у меня, второй билет у Ани"_q,
			.date = 4000,
			.changed = 4100,
		},
		{ // 4: nothing about it.
			.source = Source::Deleted,
			.peerId = 3,
			.messageId = 30,
			.chat = u"Билетная касса"_q,
			.sender = u"Bot"_q,
			.text = u"Добро пожаловать"_q,
			.date = 5000,
			.changed = 5100,
		},
	};
	const auto corpus = MakeCorpus(entries);
	const auto indexes = [&](const Query &query) {
		auto result = std::vector<int>();
		for (const auto &match : Search(*corpus, query)) {
			result.push_back(match.index);
		}
		return result;
	};
	check(indexes({ .text = u"билет"_q })
		== std::vector<int>{ 3, 0, 1, 2, 4 },
		"rank: whole word, the newest, a part, a file, a chat name");
	check(indexes({ .text = u"билет на субботу"_q })
		== std::vector<int>{ 3, 0 },
		"rank: all the words are needed");
	check(indexes({ .text = u"субботу билет"_q })
		== std::vector<int>{ 3, 0 },
		"match: the order of the words does not matter");
	check(indexes({ .text = u"\"билет у\""_q }) == std::vector<int>{ 3 },
		"match: a phrase in quotes");
	check(indexes({ .text = u"anna билет"_q }) == std::vector<int>{ 0, 1 },
		"match: a word may be in the name of the sender");
	check(indexes({ .text = u"pdf"_q }) == std::vector<int>{ 2 },
		"match: the file name");
	check(indexes({ .text = u"нет такого"_q }).empty(), "match: nothing");
	check(indexes({}) == std::vector<int>{ 4, 3, 2, 1, 0 },
		"empty query: everything, the newest first");
	check(indexes({ .peerId = 2 }) == std::vector<int>{ 3, 2 },
		"filter: chat");
	check(indexes({ .edited = false }) == std::vector<int>{ 4, 2, 1, 0 },
		"filter: deleted only");
	check(indexes({ .deleted = false }) == std::vector<int>{ 3 },
		"filter: edited only");
	check(indexes({ .from = 2100, .till = 4100 })
		== std::vector<int>{ 2, 1 },
		"filter: period, the end is not included");
	check(indexes({ .text = u"билет"_q, .peerId = 1, .from = 2000 })
		== std::vector<int>{ 1 },
		"filter: together with the words");
	{
		const auto matches = Search(*corpus, { .text = u"билет на субботу"_q });
		check(matches.size() == 2
			&& matches[0].score == matches[1].score
			&& matches[0].score > 3 * (kTextScore + kWordStartBonus),
			"rank: the phrase is worth more than the words");
		auto cancel = std::atomic<bool>(true);
		check(Search(*corpus, {}, &cancel).empty(), "cancel: empty result");
	}
	check.section("search");

	{
		const auto text = u"Первая строка\nВторая строка про билет и "
			u"ещё один БИЛЕТ в конце"_q;
		const auto words = std::vector<QString>{ u"билет"_q };
		const auto full = MakeSnippet(text, words, 200);
		check(!full.text.contains(QChar('\n')), "snippet: one line");
		check(full.ranges.size() == 2, "snippet: both matches");
		check(full.ranges.size() == 2
			&& full.text.mid(full.ranges[0].offset, full.ranges[0].length)
				== u"билет"_q
			&& full.text.mid(full.ranges[1].offset, full.ranges[1].length)
				== u"БИЛЕТ"_q,
			"snippet: the ranges point at the matches");
		const auto cut = MakeSnippet(
			QString(100, QChar('a')) + u" нужное слово "_q
				+ QString(100, QChar('b')),
			{ u"слово"_q },
			40);
		check(cut.text.startsWith(QChar(0x2026))
			&& cut.text.endsWith(QChar(0x2026)),
			"snippet: cut on both sides");
		check(cut.ranges.size() == 1
			&& cut.text.mid(cut.ranges[0].offset, cut.ranges[0].length)
				== u"слово"_q,
			"snippet: the match is inside of the cut");
		const auto merged = MakeSnippet(
			u"абвгде"_q,
			{ u"абвг"_q, u"вгде"_q },
			50);
		check(merged.ranges == std::vector<Range>{ { 0, 6 } },
			"snippet: overlapping matches are merged");
		const auto none = MakeSnippet(u"просто текст"_q, {}, 6);
		check(none.ranges.empty() && none.text == u"просто…"_q,
			"snippet: no words");
		check(MakeSnippet(QString(), words, 10).text.isEmpty(),
			"snippet: no text");
		const auto emoji = QString::fromUtf8("\xF0\x9F\x98\x82");
		const auto pair = MakeSnippet(emoji + emoji + emoji, {}, 3);
		check(!pair.text.isEmpty()
			&& !pair.text[pair.text.size() - 2].isHighSurrogate(),
			"snippet: a pair is not cut in two");
	}
	check.section("snippet");

	{
		auto query = Query();
		const auto now = TimeId(1791362460);
		ApplyPeriod(query, { .type = PeriodType::Week }, now);
		check(query.from == now - 7 * 86400 && !query.till, "period: week");
		const auto day = QDate(2026, 10, 1);
		ApplyPeriod(query, { .type = PeriodType::Day, .day = day }, now);
		check(query.till - query.from >= 23 * 3600
			&& query.till - query.from <= 25 * 3600
			&& QDateTime::fromSecsSinceEpoch(query.from).date() == day,
			"period: one day");
		ApplyPeriod(query, {}, now);
		check(!query.from && !query.till, "period: all");
		auto chats = CollectChats(*corpus, 0);
		check(chats.size() == 3
			&& chats[0].peerId == 1
			&& chats[0].count == 2
			&& chats[2].peerId == 3,
			"chats: sorted by the number of the records");
		AddPresetChat(chats, 2, u"Renamed"_q);
		AddPresetChat(chats, 0, u"Nobody"_q);
		AddPresetChat(chats, 9, QString());
		check(chats.size() == 3 && chats[1].name == u"Club"_q,
			"chats: a chat with records is not added twice");
		AddPresetChat(chats, 9, u"New chat"_q);
		check(chats.size() == 4
			&& chats[3].peerId == 9
			&& chats[3].name == u"New chat"_q
			&& !chats[3].count,
			"chats: the opened chat has its name with nothing saved");
	}
	check.section("filters");

	return !check.failed();
}

} // namespace Oblivion::DeletedSearch
