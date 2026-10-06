/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_chat_stats_ui.h"

#include "apiwrap.h"
#include "base/timer_rpl.h"
#include "base/unixtime.h"
#include "data/data_chat.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "dialogs/dialogs_key.h"
#include "lang/lang_keys.h"
#include "lang/lang_tag.h"
#include "main/main_session.h"
#include "oblivion/oblivion_chat_stats.h"
#include "oblivion/oblivion_lang.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/boxes/confirm_box.h"
#include "ui/effects/animation_value.h"
#include "ui/emoji_config.h"
#include "ui/empty_userpic.h"
#include "ui/image/image.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/text/text_utilities.h"
#include "ui/ui_utility.h"
#include "ui/userpic_view.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/tooltip.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QDate>
#include <QtCore/QDateTime>
#include <QtCore/QLocale>
#include <QtGui/QPainterPath>

namespace Oblivion {
namespace {

using namespace ChatStats;

constexpr auto kBoxWidth = 520;
constexpr auto kBoxMaxHeight = 720;
constexpr auto kTooltipDelay = 300;
constexpr auto kAxisFontSize = 11;
constexpr auto kTopPersons = 10;
constexpr auto kSectionSkip = 18;
constexpr auto kTitleSkip = 10;
constexpr auto kColumnSkip = 12;
constexpr auto kBarsHeight = 88;
constexpr auto kBarsAxis = 22;
constexpr auto kBarRadius = 2;
constexpr auto kBarMaxWidth = 56;
constexpr auto kPersonHeight = 54;
constexpr auto kPersonPhoto = 36;
constexpr auto kPersonBar = 3;
constexpr auto kPersonTail = 6;
constexpr auto kGridRowSkip = 10;
constexpr auto kListRow = 28;
constexpr auto kListRadius = 4;
constexpr auto kEmojiColumns = 8;
constexpr auto kStickerColumns = 4;
constexpr auto kStickerSize = 76;
constexpr auto kFactRow = 28;
constexpr auto kHeatMaxStep = 22;
constexpr auto kHeatLegendEdge = 40;
constexpr auto kHeatYearColumns = 54;
constexpr auto kHeatSingleDays = 371;
constexpr auto kHeatLegendCell = 10;
constexpr auto kDaysChartLimit = 366;
constexpr auto kProgressHeight = 4;
constexpr auto kJustNow = TimeId(90);
constexpr auto kWaitingNameLimit = 40;
constexpr auto kSampleZone = 3 * 3600;

using UserpicPainter = Fn<void(
	Painter &p,
	int x,
	int y,
	int outerWidth,
	int size)>;

struct PersonView {
	QString name;
	UserpicPainter paintUserpic;
	bool self = false;
};

// Paints the sticker into the rect, false while there is no image.
struct StickerView {
	Fn<bool(QPainter &p, QRect rect)> paint;
	QString emoji;
};

struct BoxArgs {
	std::shared_ptr<Ui::Show> show;
	QString title;
	bool group = false;
	std::shared_ptr<Zone> zone;
	rpl::producer<Status> status;
	rpl::producer<std::shared_ptr<const Report>> reports;
	rpl::producer<> repaint;
	Fn<PersonView(uint64 id, const QString &name)> person;
	Fn<StickerView(const TopSticker &sticker)> sticker;
	Fn<void(Period)> setPeriod;
	Fn<void()> refresh;
	Fn<void()> stop;
	Fn<void()> clear;
	TimeId now = 0; // A fixed "now" for the snapshots.
	rpl::lifetime keep;
};

[[nodiscard]] int Scaled(int value) {
	return style::ConvertScale(value);
}

[[nodiscard]] QLocale DateLocale() {
	return QLocale(CurrentLanguageIsRussian()
		? QLocale::Russian
		: QLocale::English);
}

[[nodiscard]] QDate DateOf(int32 day) {
	const auto date = DateOfDay(day);
	return QDate(date.year, date.month, date.day);
}

[[nodiscard]] QString Number(int64 value) {
	return Lang::FormatCountDecimal(value);
}

[[nodiscard]] QString Messages(int count) {
	return tr::lng_oblivion_stats_messages(
		tr::now,
		lt_count_decimal,
		count);
}

[[nodiscard]] QString FormatDay(int32 day) {
	return DateLocale().toString(DateOf(day), u"d MMMM yyyy"_q);
}

[[nodiscard]] QString FormatDayCompact(int32 day) {
	return DateLocale().toString(DateOf(day), u"d MMM yyyy"_q);
}

// "5 May — 12 June 2024": the year is written once when it is the same.
// Two full dates are long, `compact` shortens the names of their months.
[[nodiscard]] QString FormatDays(int32 from, int32 till, bool compact) {
	const auto dash = u" — "_q;
	if (DateOfDay(from).year == DateOfDay(till).year) {
		return DateLocale().toString(DateOf(from), u"d MMMM"_q)
			+ dash
			+ FormatDay(till);
	}
	return compact
		? (FormatDayCompact(from) + dash + FormatDayCompact(till))
		: (FormatDay(from) + dash + FormatDay(till));
}

[[nodiscard]] QString FormatMoment(TimeId when, const Zone &zone) {
	const auto local = QDateTime::fromSecsSinceEpoch(
		qint64(when) + zone.offset(when),
		QTimeZone::utc());
	return DateLocale().toString(local.date(), u"d MMMM yyyy"_q)
		+ u", "_q
		+ local.time().toString(u"HH:mm"_q);
}

[[nodiscard]] QString WithoutDot(QString text) {
	if (text.endsWith(QChar('.'))) {
		text.chop(1);
	}
	return text;
}

// A name inside a sentence: a very long one is cut, not in the middle of
// a symbol that takes two code units.
[[nodiscard]] QString Shortened(const QString &text, int limit) {
	if (text.size() <= limit) {
		return text;
	}
	auto length = std::max(limit - 1, 1);
	if (text[length - 1].isHighSurrogate()) {
		--length;
	}
	return text.left(length).trimmed() + QChar(0x2026);
}

[[nodiscard]] QString Capitalized(QString text) {
	if (!text.isEmpty()) {
		text[0] = text[0].toUpper();
	}
	return text;
}

// Three letters for every month: "сен", not "сент.", so that the names
// above the narrow columns of a chart are all of one width.
[[nodiscard]] QString MonthShort(int month) {
	return DateLocale().standaloneMonthName(
		month,
		QLocale::ShortFormat).left(3);
}

[[nodiscard]] QString MonthLong(int month) {
	return Capitalized(
		DateLocale().standaloneMonthName(month, QLocale::LongFormat));
}

// 0 is Monday, the same as in the report.
[[nodiscard]] QString WeekdayShort(int weekday) {
	return Capitalized(WithoutDot(DateLocale().standaloneDayName(
		weekday + 1,
		QLocale::ShortFormat)));
}

[[nodiscard]] QString WeekdayLong(int weekday) {
	return Capitalized(DateLocale().standaloneDayName(
		weekday + 1,
		QLocale::LongFormat));
}

[[nodiscard]] QString FormatSpan(TimeId seconds) {
	const auto value = [](TimeId number) {
		return QString::number(number);
	};
	if (seconds < 60) {
		return tr::lng_oblivion_stats_span_s(
			tr::now,
			lt_value,
			value(std::max(seconds, TimeId(1))));
	}
	const auto minutes = (seconds + 30) / 60;
	if (minutes < 60) {
		return tr::lng_oblivion_stats_span_m(
			tr::now,
			lt_value,
			value(minutes));
	} else if (!(minutes % 60)) {
		return tr::lng_oblivion_stats_span_h(
			tr::now,
			lt_value,
			value(minutes / 60));
	}
	return tr::lng_oblivion_stats_span_hm(
		tr::now,
		lt_hours,
		value(minutes / 60),
		lt_minutes,
		value(minutes % 60));
}

// The locale of Lang::FormatCountDecimal, not the one of the dates: in
// "2,915" next to "3,9" the same mark would mean two different things.
[[nodiscard]] QString Fraction(float64 value) {
	return QLocale().toString(value, 'f', 1);
}

[[nodiscard]] QString Percent(int64 part, int64 whole) {
	if (whole <= 0) {
		return QString();
	}
	const auto value = part * 100. / whole;

	// "9%", not "9.0%": the tenths are shown only when there are some.
	const auto tenths = int64(base::SafeRound(value * 10.));
	return ((value >= 10. || value <= 0. || !(tenths % 10))
		? QString::number(int(base::SafeRound(value)))
		: Fraction(value)) + QChar('%');
}

[[nodiscard]] style::font AxisFont() {
	return style::font(Scaled(kAxisFontSize), 0, st::normalFont->family());
}

[[nodiscard]] QColor FaintColor(float64 alpha) {
	return anim::with_alpha(st::windowSubTextFg->c, alpha);
}

[[nodiscard]] UserpicPainter EmptyUserpicPainter(
		uint64 id,
		const QString &name) {
	const auto userpic = std::make_shared<Ui::EmptyUserpic>(
		Ui::EmptyUserpic::UserpicColor(Ui::EmptyUserpic::ColorIndex(id)),
		name);
	return [=](Painter &p, int x, int y, int outerWidth, int size) {
		userpic->paintCircle(p, x, y, outerWidth, size);
	};
}

void PaintEmoji(QPainter &p, const QString &text, QRect rect) {
	auto length = 0;
	auto emoji = Ui::Emoji::Find(QStringView(text), &length);
	if (!emoji) {
		const auto varied = text + QChar(0xFE0F);
		emoji = Ui::Emoji::Find(QStringView(varied), &length);
	}
	if (emoji) {
		const auto size = Ui::Emoji::GetSizeLarge();
		const auto shown = size / style::DevicePixelRatio();
		Ui::Emoji::Draw(
			p,
			emoji,
			size,
			rect.x() + (rect.width() - shown) / 2,
			rect.y() + (rect.height() - shown) / 2);
		return;
	}
	// A sequence the app has no picture of: the system font has it.
	p.setFont(style::font(Scaled(20), 0, st::normalFont->family()));
	p.setPen(st::boxTextFg);
	p.drawText(rect, text, style::al_center);
}

// A widget with a tooltip that depends on the place under the cursor.
class TipWidget
	: public Ui::RpWidget
	, public Ui::AbstractTooltipShower {
public:
	explicit TipWidget(QWidget *parent)
	: RpWidget(parent) {
		setMouseTracking(true);
	}

	QString tooltipText() const override {
		return _tip;
	}
	QPoint tooltipPos() const override {
		return _tipPoint;
	}
	bool tooltipWindowActive() const override {
		return Ui::AppInFocus() && Ui::InFocusChain(window());
	}

protected:
	[[nodiscard]] virtual QString tipAt(QPoint point) const = 0;

	void mouseMoveEvent(QMouseEvent *e) override {
		_tipPoint = e->globalPosition().toPoint();
		updateTip(tipAt(e->pos()));
	}
	void leaveEventHook(QEvent *e) override {
		updateTip(QString());
	}
	void dropTip() {
		updateTip(QString());
	}

private:
	void updateTip(const QString &tip) {
		if (_tip == tip) {
			return;
		}
		_tip = tip;
		if (_tip.isEmpty()) {
			Ui::Tooltip::Hide();
		} else {
			Ui::Tooltip::Show(kTooltipDelay, this);
		}
	}

	QString _tip;
	QPoint _tipPoint;

};

struct Cell {
	QString value;
	QString caption;
};

// Big numbers with captions, three in a row.
class Cells final : public Ui::RpWidget {
public:
	using RpWidget::RpWidget;

	void setCells(std::vector<Cell> cells) {
		_cells = std::move(cells);
		resizeToWidth(width());
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto rows = (int(_cells.size()) + kColumns - 1) / kColumns;
		return rows * rowHeight() - (rows ? Scaled(kColumnSkip) : 0);
	}
	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		const auto skip = Scaled(kColumnSkip);
		for (auto i = 0; i != int(_cells.size()); ++i) {
			const auto column = i % kColumns;
			const auto left = width() * column / kColumns;
			const auto top = (i / kColumns) * rowHeight();
			const auto available = (width() * (column + 1) / kColumns)
				- left
				- skip;
			if (available <= 0) {
				continue;
			}
			p.setFont(st::boxTitleFont);
			p.setPen(st::boxTextFg);
			p.drawTextLeft(
				left,
				top,
				width(),
				st::boxTitleFont->elided(_cells[i].value, available));
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawTextLeft(
				left,
				top + st::boxTitleFont->height + Scaled(2),
				width(),
				st::normalFont->elided(_cells[i].caption, available));
		}
	}

private:
	static constexpr auto kColumns = 3;

	[[nodiscard]] int rowHeight() const {
		return st::boxTitleFont->height
			+ Scaled(2)
			+ st::normalFont->height
			+ Scaled(kColumnSkip);
	}

	std::vector<Cell> _cells;

};

struct BarsData {
	std::vector<int> values;
	std::vector<std::pair<int, QString>> labels; // Under these bars.
	Fn<QString(int index)> tip;
};

// A bar chart: the largest value is the full height, its number is
// written above the chart.
class Bars final : public TipWidget {
public:
	using TipWidget::TipWidget;

	void setData(BarsData data) {
		_data = std::move(data);
		_maximum = 1;
		for (const auto value : _data.values) {
			_maximum = std::max(_maximum, value);
		}
		dropTip();
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return top() + Scaled(kBarsHeight) + Scaled(kBarsAxis);
	}
	void paintEvent(QPaintEvent *e) override {
		const auto count = int(_data.values.size());
		if (!count || width() <= 0) {
			return;
		}
		auto p = Painter(this);
		const auto chart = Scaled(kBarsHeight);
		const auto base = top() + chart;
		const auto step = width() / float64(count);
		const auto gap = (step >= 8.) ? 2. : (step >= 3.) ? 1. : 0.;

		// A few bars don't grow into slabs: each one stays in the
		// middle of its place.
		const auto bar = std::clamp(
			step - gap,
			1.,
			float64(Scaled(kBarMaxWidth)));
		const auto shift = (step - bar) / 2.;
		const auto radius = float64(Scaled(kBarRadius));
		const auto rounded = (bar >= 3 * radius);
		const auto color = st::windowBgActive->c;

		// The level of the number written above the chart: the largest
		// bar reaches this line.
		p.fillRect(
			0,
			top() - st::lineWidth,
			width(),
			st::lineWidth,
			FaintColor(0.16));

		// Wide bars stand on whole pixels and stay sharp, the narrow
		// ones of a long chart are smoothed.
		const auto sharp = (step >= 3.);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		for (auto i = 0; i != count; ++i) {
			const auto value = _data.values[i];
			if (value <= 0) {
				continue;
			}
			const auto size = std::max(
				chart * (value / float64(_maximum)),
				float64(Scaled(2)));
			auto rect = QRectF(i * step + shift, base - size, bar, size);
			if (sharp) {
				const auto left = std::round(rect.left());
				const auto right = std::round(rect.right());
				rect = QRectF(
					left,
					std::round(rect.top()),
					std::max(right - left, 1.),
					base - std::round(rect.top()));
			}
			if (rounded && rect.height() > radius) {
				p.drawRoundedRect(rect, radius, radius);
				p.drawRect(rect.adjusted(0., radius, 0., 0.));
			} else {
				p.drawRect(rect);
			}
		}
		p.fillRect(0, base, width(), st::lineWidth, FaintColor(0.3));

		const auto font = AxisFont();
		p.setFont(font);
		p.setPen(st::windowSubTextFg);
		p.drawTextLeft(0, 0, width(), Number(_maximum));
		auto right = -Scaled(8);
		for (const auto &[index, text] : _data.labels) {
			const auto textWidth = font->width(text);
			const auto center = (index + 0.5) * step;
			const auto x = std::clamp(
				int(base::SafeRound(center - textWidth / 2.)),
				0,
				std::max(width() - textWidth, 0));
			if (x < right + Scaled(6)) {
				continue;
			}
			p.drawTextLeft(x, base + Scaled(5), width(), text);
			right = x + textWidth;
		}
	}
	QString tipAt(QPoint point) const override {
		const auto count = int(_data.values.size());
		if (!count
			|| !_data.tip
			|| width() <= 0
			|| point.y() < top()
			|| point.y() > top() + Scaled(kBarsHeight)) {
			return QString();
		}
		const auto index = std::clamp(point.x() * count / width(), 0, count - 1);
		return _data.tip(index);
	}

private:
	[[nodiscard]] int top() const {
		return AxisFont()->height + Scaled(4);
	}

	BarsData _data;
	int _maximum = 1;

};

// The calendar: a column for a week, a row for a day of the week, the
// more messages on a day the brighter it is. A long history is shown
// year by year, the last one first.
class Heatmap final : public TipWidget {
public:
	using TipWidget::TipWidget;

	void setData(
			int32 firstDay,
			std::vector<int> days,
			int32 fromDay,
			int32 tillDay) {
		_firstDay = firstDay;
		_days = std::move(days);
		_blocks.clear();
		if (tillDay >= fromDay) {
			if (tillDay - fromDay < kHeatSingleDays) {
				addBlock(fromDay, tillDay, QString());
			} else {
				const auto last = DateOfDay(tillDay).year;
				const auto first = DateOfDay(fromDay).year;
				for (auto year = last; year >= first; --year) {
					addBlock(
						std::max(fromDay, DayOfDate({ year, 1, 1 })),
						std::min(tillDay, DayOfDate({ year, 12, 31 })),
						QString::number(year));
				}
			}
		}

		// A few very busy days must not make all the others pale.
		auto counts = std::vector<int>();
		for (const auto count : _days) {
			if (count > 0) {
				counts.push_back(count);
			}
		}
		_cap = 1;
		if (!counts.empty()) {
			const auto index = (counts.size() - 1) * 95 / 100;
			std::nth_element(
				begin(counts),
				begin(counts) + index,
				end(counts));
			_cap = std::max(counts[index], 1);
		}
		dropTip();
		resizeToWidth(width());
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto font = AxisFont();
		_left = 0;
		for (const auto row : { 0, 2, 4 }) {
			_left = std::max(_left, font->width(WeekdayShort(row)));
		}
		_left += Scaled(8);
		auto columns = 1;
		for (const auto &block : _blocks) {
			columns = std::max(columns, block.columns);
		}
		if (_blocks.size() > 1) {
			columns = std::max(columns, kHeatYearColumns);
		}
		// A whole number of pixels: the cells are sharp and even.
		_step = std::clamp(
			std::floor((newWidth - _left) / float64(columns)),
			4.,
			float64(Scaled(kHeatMaxStep)));
		_columns = columns;
		auto top = 0;
		for (auto &block : _blocks) {
			block.top = top;
			top += blockHeight(block);
		}
		_legendTop = top;
		return top + (_blocks.empty() ? 0 : Scaled(kHeatLegendCell + 8));
	}
	void paintEvent(QPaintEvent *e) override {
		if (_blocks.empty()) {
			return;
		}
		auto p = Painter(this);
		const auto clip = e->rect();
		for (const auto &block : _blocks) {
			if (clip.intersects(
					QRect(0, block.top, width(), blockHeight(block)))) {
				paintBlock(p, block);
			}
		}
		if (clip.y() + clip.height() > _legendTop) {
			paintLegend(p);
		}
	}
	QString tipAt(QPoint point) const override {
		for (const auto &block : _blocks) {
			const auto top = gridTop(block);
			if (point.y() < top || point.x() < _left) {
				continue;
			}
			const auto row = int((point.y() - top) / _step);
			const auto column = int((point.x() - _left) / _step);
			if (row > 6 || column >= block.columns) {
				continue;
			}
			const auto day = block.weekStart + column * 7 + row;
			if (day < block.from || day > block.till) {
				return QString();
			}
			const auto count = countOf(day);
			return FormatDay(day) + u" — "_q + (count
				? Messages(count)
				: tr::lng_oblivion_stats_tip_none(tr::now));
		}
		return QString();
	}

private:
	struct Block {
		int32 from = 0;
		int32 till = 0;
		int32 weekStart = 0; // The Monday of the first column.
		int columns = 0;
		int top = 0;
		QString title;
	};

	void addBlock(int32 from, int32 till, const QString &title) {
		if (till < from) {
			return;
		}
		auto block = Block();
		block.from = from;
		block.till = till;
		block.weekStart = from - WeekdayOfDay(from);
		block.columns = (till - block.weekStart) / 7 + 1;
		block.title = title;
		_blocks.push_back(std::move(block));
	}
	[[nodiscard]] int titleHeight(const Block &block) const {
		return block.title.isEmpty()
			? 0
			: (st::semiboldFont->height + Scaled(4));
	}
	[[nodiscard]] int monthsHeight() const {
		return AxisFont()->height + Scaled(4);
	}
	[[nodiscard]] int gridTop(const Block &block) const {
		return block.top + titleHeight(block) + monthsHeight();
	}
	[[nodiscard]] int blockHeight(const Block &block) const {
		return titleHeight(block)
			+ monthsHeight()
			+ int(std::ceil(7 * _step))
			+ Scaled(14);
	}
	[[nodiscard]] int countOf(int32 day) const {
		const auto index = int64(day) - _firstDay;
		return (index >= 0 && index < int64(_days.size()))
			? _days[index]
			: 0;
	}
	[[nodiscard]] QColor levelColor(int level) const {
		constexpr float64 kRatios[] = { 0., 0.3, 0.5, 0.75, 1. };
		return level
			? anim::color(st::boxBg, st::windowBgActive, kRatios[level])
			: FaintColor(0.16);
	}
	[[nodiscard]] float64 cellGap() const {
		return std::max(1., std::floor(_step * 0.14));
	}
	[[nodiscard]] int levelOf(int count) const {
		return (count <= 0)
			? 0
			: std::clamp((count * 4 + _cap - 1) / _cap, 1, 4);
	}
	void paintCell(QPainter &p, QRectF rect, int level) const {
		const auto color = levelColor(level);
		if (rect.width() >= 6.) {
			// The corners grow with the cells of a short calendar.
			const auto radius = std::clamp(rect.width() * 0.16, 2., 4.);
			p.setBrush(color);
			p.drawRoundedRect(rect, radius, radius);
		} else {
			p.fillRect(rect, color);
		}
	}
	void paintBlock(Painter &p, const Block &block) const {
		const auto font = AxisFont();
		if (!block.title.isEmpty()) {
			p.setFont(st::semiboldFont);
			p.setPen(st::boxTextFg);
			p.drawTextLeft(0, block.top, width(), block.title);
		}
		p.setFont(font);
		p.setPen(st::windowSubTextFg);
		const auto top = gridTop(block);
		for (const auto row : { 0, 2, 4 }) {
			p.drawTextLeft(
				0,
				top + int(row * _step + (_step - font->height) / 2.),
				width(),
				WeekdayShort(row));
		}

		// The names of the months above the weeks they start in.
		const auto monthsTop = block.top + titleHeight(block);
		auto right = -Scaled(8);
		auto date = DateOfDay(block.from);
		date.day = 1;
		while (true) {
			const auto start = DayOfDate(date);
			if (start > block.till) {
				break;
			}
			auto next = date;
			if (++next.month > 12) {
				next.month = 1;
				++next.year;
			}
			const auto shown = std::max(start, block.from);
			const auto column = (shown - block.weekStart) / 7;
			const auto nextColumn = (std::min(DayOfDate(next), block.till + 1)
				- block.weekStart) / 7;
			const auto x = _left + int(column * _step);
			const auto text = MonthShort(date.month);
			const auto textWidth = font->width(text);

			// A month cut by the start of the block gets its name only
			// if there is a place for it: the name must not stand above
			// the weeks of the next month.
			const auto fits = (start >= block.from)
				|| ((nextColumn - column) * _step >= textWidth + Scaled(6));
			if (fits
				&& x >= right + Scaled(6)
				&& x + textWidth <= width()) {
				p.drawTextLeft(x, monthsTop, width(), text);
				right = x + textWidth;
			}
			date = next;
		}

		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		const auto gap = cellGap();
		for (auto day = block.from; day <= block.till; ++day) {
			const auto column = (day - block.weekStart) / 7;
			const auto row = WeekdayOfDay(day);
			paintCell(
				p,
				QRectF(
					_left + column * _step,
					top + row * _step,
					_step - gap,
					_step - gap),
				levelOf(countOf(day)));
		}
	}
	void paintLegend(Painter &p) const {
		const auto font = AxisFont();
		const auto cell = Scaled(kHeatLegendCell);
		const auto skip = Scaled(3);
		const auto less = tr::lng_oblivion_stats_less(tr::now);
		const auto more = tr::lng_oblivion_stats_more(tr::now);
		const auto top = _legendTop;

		// Under the end of the weeks: the calendar of a short period is
		// narrow, the legend in the opposite corner would be on its own.
		const auto legendWidth = font->width(less)
			+ Scaled(3)
			+ 5 * (cell + skip)
			+ Scaled(6)
			+ font->width(more);
		const auto gridRight = _left
			+ int(std::ceil(_columns * _step - cellGap()));
		const auto right = (gridRight + Scaled(kHeatLegendEdge) >= width())
			? width()
			: std::min(std::max(gridRight, legendWidth), width());
		auto x = right - font->width(more);
		p.setFont(font);
		p.setPen(st::windowSubTextFg);
		p.drawTextLeft(x, top + (cell - font->height) / 2, width(), more);
		x -= Scaled(6);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			for (auto level = 4; level >= 0; --level) {
				x -= cell;
				paintCell(p, QRectF(x, top, cell, cell), level);
				x -= skip;
			}
		}
		x -= Scaled(3) + font->width(less);
		p.setPen(st::windowSubTextFg);
		p.drawTextLeft(x, top + (cell - font->height) / 2, width(), less);
	}

	int32 _firstDay = 0;
	std::vector<int> _days;
	std::vector<Block> _blocks;
	int _cap = 1;
	int _left = 0;
	int _legendTop = 0;
	int _columns = 1;
	float64 _step = 8.;

};

// Every number in `about` has its unit, the share of all the messages
// stands alone on the right.
struct PersonRow {
	PersonView view;
	QString percent;
	QString about;
	float64 share = 0.; // Of the most active one.
};

// The participants: a photo, a name, how much of everything is theirs.
class Ranking final : public Ui::RpWidget {
public:
	using RpWidget::RpWidget;

	void setRows(std::vector<PersonRow> rows, QString more) {
		_rows = std::move(rows);
		_more = std::move(more);
		resizeToWidth(width());
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		// The air under the last row is a part of the skip between the
		// sections, which is the same after all of them.
		const auto rows = int(_rows.size());
		return rows * Scaled(kPersonHeight)
			+ (!_more.isEmpty()
				? (st::normalFont->height + Scaled(6))
				: rows
				? -Scaled(kPersonTail)
				: 0);
	}
	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		const auto height = Scaled(kPersonHeight);
		const auto photo = Scaled(kPersonPhoto);
		const auto left = photo + Scaled(kColumnSkip);
		const auto clip = e->rect();
		for (auto i = 0; i != int(_rows.size()); ++i) {
			const auto top = i * height;
			if (top + height <= clip.y() || top >= clip.y() + clip.height()) {
				continue;
			}
			const auto &row = _rows[i];
			if (row.view.paintUserpic) {
				row.view.paintUserpic(
					p,
					0,
					top + Scaled(3),
					width(),
					photo);
			}
			const auto percentWidth = st::normalFont->width(row.percent);
			const auto available = width()
				- left
				- percentWidth
				- Scaled(kColumnSkip);
			p.setFont(st::normalFont);
			p.setPen(st::boxTextFg);
			p.drawTextRight(0, top + Scaled(2), width(), row.percent);
			if (available > 0) {
				p.setFont(st::semiboldFont);
				p.setPen(st::boxTextFg);
				p.drawTextLeft(
					left,
					top + Scaled(2),
					width(),
					st::semiboldFont->elided(row.view.name, available));
			}
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawTextLeft(
				left,
				top + Scaled(2) + st::semiboldFont->height,
				width(),
				st::normalFont->elided(row.about, width() - left));

			const auto barTop = top
				+ Scaled(2)
				+ st::semiboldFont->height
				+ st::normalFont->height
				+ Scaled(4);
			const auto barHeight = float64(Scaled(kPersonBar));
			const auto barWidth = float64(width() - left);
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(FaintColor(0.14));
			p.drawRoundedRect(
				QRectF(left, barTop, barWidth, barHeight),
				barHeight / 2.,
				barHeight / 2.);
			p.setBrush(st::windowBgActive);
			p.drawRoundedRect(
				QRectF(
					left,
					barTop,
					std::max(
						barWidth * std::clamp(row.share, 0., 1.),
						barHeight),
					barHeight),
				barHeight / 2.,
				barHeight / 2.);
		}
		if (!_more.isEmpty()) {
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawTextLeft(
				left,
				int(_rows.size()) * height + Scaled(2),
				width(),
				st::normalFont->elided(_more, width() - left));
		}
	}

private:
	std::vector<PersonRow> _rows;
	QString _more;

};

struct ListItem {
	QString text;
	QString value;
	float64 share = 0.; // Of the first one.
};

// Words or kinds of messages with their counts, in two columns, with a
// pale bar of the share behind every row.
class TopList final : public Ui::RpWidget {
public:
	TopList(QWidget *parent, bool ranked)
	: RpWidget(parent)
	, _ranked(ranked) {
	}

	void setItems(std::vector<ListItem> items) {
		_items = std::move(items);
		resizeToWidth(width());
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return rows() * Scaled(kListRow);
	}
	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		const auto count = int(_items.size());
		const auto perColumn = rows();
		if (!perColumn) {
			return;
		}
		const auto skip = Scaled(kColumnSkip);
		const auto columnWidth = (width() - skip) / kColumns;
		const auto height = Scaled(kListRow);
		const auto inner = height - Scaled(4);
		const auto padding = Scaled(8);
		const auto rankWidth = _ranked
			? (st::normalFont->width(QString::number(count) + u"."_q)
				+ Scaled(6))
			: 0;
		const auto radius = float64(Scaled(kListRadius));
		const auto textTop = (inner - st::normalFont->height) / 2;
		for (auto i = 0; i != count; ++i) {
			const auto &item = _items[i];
			const auto left = (i / perColumn) * (columnWidth + skip);
			const auto top = (i % perColumn) * height;
			{
				// A pale row with its share filled, cut by the rounded
				// corners of the row.
				const auto row = QRectF(left, top, columnWidth, inner);
				auto path = QPainterPath();
				path.addRoundedRect(row, radius, radius);
				auto hq = PainterHighQualityEnabler(p);
				p.setPen(Qt::NoPen);
				p.setBrush(FaintColor(0.08));
				p.drawPath(path);
				p.save();
				p.setClipPath(path);
				p.fillRect(
					QRectF(
						left,
						top,
						columnWidth * std::clamp(item.share, 0., 1.),
						inner),
					anim::with_alpha(st::windowBgActive->c, 0.2));
				p.restore();
			}
			const auto valueWidth = st::normalFont->width(item.value);
			auto x = left + padding;
			p.setFont(st::normalFont);
			if (_ranked) {
				// The numbers stand in a column by their dots.
				const auto rank = QString::number(i + 1) + u"."_q;
				p.setPen(st::windowSubTextFg);
				p.drawTextLeft(
					x + rankWidth - Scaled(6) - st::normalFont->width(rank),
					top + textTop,
					width(),
					rank);
				x += rankWidth;
			}
			const auto available = left
				+ columnWidth
				- padding
				- valueWidth
				- padding
				- x;
			p.setPen(st::windowSubTextFg);
			p.drawTextLeft(
				left + columnWidth - padding - valueWidth,
				top + textTop,
				width(),
				item.value);
			if (available > 0) {
				p.setPen(st::boxTextFg);
				p.drawTextLeft(
					x,
					top + textTop,
					width(),
					st::normalFont->elided(item.text, available));
			}
		}
	}

private:
	static constexpr auto kColumns = 2;

	[[nodiscard]] int rows() const {
		return (int(_items.size()) + kColumns - 1) / kColumns;
	}

	const bool _ranked = false;
	std::vector<ListItem> _items;

};

// Emoji with the number of uses under each.
class EmojiGrid final : public Ui::RpWidget {
public:
	using RpWidget::RpWidget;

	void setItems(std::vector<TopText> items) {
		_items = std::move(items);
		resizeToWidth(width());
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto rows = (int(_items.size()) + kEmojiColumns - 1)
			/ kEmojiColumns;
		return rows * cellHeight() - (rows ? Scaled(kGridRowSkip) : 0);
	}
	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		const auto font = AxisFont();
		const auto emoji = emojiHeight();
		for (auto i = 0; i != int(_items.size()); ++i) {
			const auto column = i % kEmojiColumns;
			const auto left = width() * column / kEmojiColumns;
			const auto cell = (width() * (column + 1) / kEmojiColumns) - left;
			const auto top = (i / kEmojiColumns) * cellHeight();
			PaintEmoji(p, _items[i].text, QRect(left, top, cell, emoji));
			p.setFont(font);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				QRect(left, top + emoji + Scaled(2), cell, font->height),
				Number(_items[i].count),
				style::al_top);
		}
	}

private:
	[[nodiscard]] int emojiHeight() const {
		return (Ui::Emoji::GetSizeLarge() / style::DevicePixelRatio())
			+ Scaled(6);
	}
	[[nodiscard]] int cellHeight() const {
		return emojiHeight()
			+ Scaled(2)
			+ AxisFont()->height
			+ Scaled(kGridRowSkip);
	}

	std::vector<TopText> _items;

};

struct StickerItem {
	StickerView view;
	QString count;
};

// Stickers with the number of uses under each. Where the image is not
// loaded (or can't be) the emoji of the sticker is shown.
class Stickers final : public Ui::RpWidget {
public:
	using RpWidget::RpWidget;

	void setItems(std::vector<StickerItem> items) {
		_items = std::move(items);
		resizeToWidth(width());
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto rows = (int(_items.size()) + kStickerColumns - 1)
			/ kStickerColumns;
		return rows * cellHeight() - (rows ? Scaled(kGridRowSkip) : 0);
	}
	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		const auto size = Scaled(kStickerSize);
		for (auto i = 0; i != int(_items.size()); ++i) {
			const auto &item = _items[i];
			const auto column = i % kStickerColumns;
			const auto left = width() * column / kStickerColumns;
			const auto cell = (width() * (column + 1) / kStickerColumns)
				- left;
			const auto top = (i / kStickerColumns) * cellHeight();
			const auto rect = QRect(
				left + (cell - size) / 2,
				top,
				size,
				size);
			if (!item.view.paint || !item.view.paint(p, rect)) {
				{
					auto hq = PainterHighQualityEnabler(p);
					p.setPen(Qt::NoPen);
					p.setBrush(FaintColor(0.1));
					p.drawRoundedRect(rect, Scaled(12), Scaled(12));
				}
				if (!item.view.emoji.isEmpty()) {
					PaintEmoji(p, item.view.emoji, rect);
				}
			}
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				QRect(
					left,
					top + size + Scaled(4),
					cell,
					st::normalFont->height),
				item.count,
				style::al_top);
		}
	}

private:
	[[nodiscard]] int cellHeight() const {
		return Scaled(kStickerSize)
			+ Scaled(4)
			+ st::normalFont->height
			+ Scaled(kGridRowSkip);
	}

	std::vector<StickerItem> _items;

};

struct Fact {
	QString label;
	QString value;
};

// A name on the left, a value on the right.
class Facts final : public Ui::RpWidget {
public:
	using RpWidget::RpWidget;

	void setFacts(std::vector<Fact> facts) {
		_facts = std::move(facts);
		resizeToWidth(width());
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return int(_facts.size()) * Scaled(kFactRow);
	}
	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		const auto height = Scaled(kFactRow);
		const auto skip = Scaled(kColumnSkip);
		const auto textTop = (height - st::normalFont->height) / 2;
		p.setFont(st::normalFont);
		for (auto i = 0; i != int(_facts.size()); ++i) {
			const auto &fact = _facts[i];
			const auto top = i * height;
			// Only a text that does not fit is elided: asked to fit
			// exactly its own width it may lose its end to rounding.
			const auto labelFull = st::normalFont->width(fact.label);
			const auto labelWidth = std::min(labelFull, width() / 2);
			p.setPen(st::windowSubTextFg);
			p.drawTextLeft(
				0,
				top + textTop,
				width(),
				(labelFull > labelWidth)
					? st::normalFont->elided(fact.label, labelWidth)
					: fact.label);
			const auto available = width()
				- labelWidth
				- (labelWidth ? skip : 0);
			if (available > 0) {
				p.setPen(st::boxTextFg);
				p.drawTextRight(
					0,
					top + textTop,
					width(),
					(st::normalFont->width(fact.value) > available)
						? st::normalFont->elided(fact.value, available)
						: fact.value);
			}
		}
	}

private:
	std::vector<Fact> _facts;

};

// The progress of the reading.
class ProgressLine final : public Ui::RpWidget {
public:
	using RpWidget::RpWidget;

	void setValue(float64 value, bool active) {
		_value = std::clamp(value, 0., 1.);
		_active = active;
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return Scaled(kProgressHeight);
	}
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = height() / 2.;
		p.setPen(Qt::NoPen);
		p.setBrush(FaintColor(0.16));
		p.drawRoundedRect(QRectF(rect()), radius, radius);
		if (_value > 0.) {
			p.setBrush(_active ? st::windowBgActive->c : FaintColor(0.6));
			p.drawRoundedRect(
				QRectF(0, 0, std::max(width() * _value, 2 * radius), height()),
				radius,
				radius);
		}
	}

private:
	float64 _value = 0.;
	bool _active = true;

};

template <typename Widget>
struct Section {
	not_null<Ui::SlideWrap<Ui::VerticalLayout>*> wrap;
	not_null<Widget*> widget;

	void toggle(bool shown) const {
		wrap->toggle(shown, anim::type::instant);
	}
};

template <typename Widget, typename ...Args>
[[nodiscard]] Section<Widget> AddSection(
		not_null<Ui::VerticalLayout*> content,
		rpl::producer<QString> title,
		Args &&...args) {
	const auto wrap = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)),
		style::margins());
	const auto inner = wrap->entity();
	const auto &padding = st::boxRowPadding;
	inner->add(
		object_ptr<Ui::FlatLabel>(
			inner,
			std::move(title),
			st::defaultSubsectionTitle),
		padding + QMargins(0, Scaled(kSectionSkip), 0, Scaled(kTitleSkip)));
	const auto widget = inner->add(
		object_ptr<Widget>(inner, std::forward<Args>(args)...),
		padding);
	wrap->toggle(false, anim::type::instant);
	return { wrap, widget };
}

[[nodiscard]] QString KindName(Kind kind) {
	switch (kind) {
	case Kind::Text: return tr::lng_oblivion_stats_kind_text(tr::now);
	case Kind::Photo: return tr::lng_oblivion_stats_kind_photo(tr::now);
	case Kind::Video: return tr::lng_oblivion_stats_kind_video(tr::now);
	case Kind::Round: return tr::lng_oblivion_stats_kind_round(tr::now);
	case Kind::Voice: return tr::lng_oblivion_stats_kind_voice(tr::now);
	case Kind::Music: return tr::lng_oblivion_stats_kind_music(tr::now);
	case Kind::File: return tr::lng_oblivion_stats_kind_file(tr::now);
	case Kind::Sticker: return tr::lng_oblivion_stats_kind_sticker(tr::now);
	case Kind::Gif: return tr::lng_oblivion_stats_kind_gif(tr::now);
	case Kind::Poll: return tr::lng_oblivion_stats_kind_poll(tr::now);
	case Kind::Location:
		return tr::lng_oblivion_stats_kind_location(tr::now);
	case Kind::Contact: return tr::lng_oblivion_stats_kind_contact(tr::now);
	case Kind::Call: return tr::lng_oblivion_stats_kind_call(tr::now);
	case Kind::Other: break;
	}
	return tr::lng_oblivion_stats_kind_other(tr::now);
}

[[nodiscard]] QString ErrorText(const Status &status) {
	const auto &type = status.error;
	if (status.cacheFailed) {
		return tr::lng_oblivion_stats_status_failed_cache(tr::now);
	} else if (type == u"OBLIVION_NO_PROGRESS"_q) {
		return tr::lng_oblivion_stats_status_failed_stuck(tr::now);
	}
	const auto access = {
		u"CHANNEL_PRIVATE"_q,
		u"CHANNEL_INVALID"_q,
		u"CHANNEL_PUBLIC_GROUP_NA"_q,
		u"CHAT_FORBIDDEN"_q,
		u"CHAT_ID_INVALID"_q,
		u"PEER_ID_INVALID"_q,
		u"USER_BANNED_IN_CHANNEL"_q,
	};
	for (const auto &known : access) {
		if (type == known) {
			return tr::lng_oblivion_stats_status_failed_access(tr::now);
		}
	}
	return tr::lng_oblivion_stats_status_failed(tr::now, lt_error, type);
}

struct ContentArgs {
	std::shared_ptr<Zone> zone;
	bool group = false;
	Fn<PersonView(uint64 id, const QString &name)> person;
	Fn<StickerView(const TopSticker &sticker)> sticker;
	rpl::producer<> repaint;
};

using ContentUpdate = Fn<void(
	std::shared_ptr<const Report> report,
	const Status &status)>;

[[nodiscard]] bool Busy(Stage stage) {
	return (stage == Stage::Waiting)
		|| (stage == Stage::Reading)
		|| (stage == Stage::Flood);
}

[[nodiscard]] QString EmptyText(const Report *report, const Status &status) {
	const auto stage = status.stage;

	// Only a reading that goes on gives numbers in seconds: in the line
	// behind another chat or after the server asked to wait it is not
	// known when they come.
	const auto awaited = [&] {
		return (stage == Stage::Reading)
			? tr::lng_oblivion_stats_empty_reading(tr::now)
			: (stage == Stage::Waiting || stage == Stage::Flood)
			? tr::lng_oblivion_stats_empty_waiting(tr::now)
			: QString();
	};
	if (stage == Stage::Loading) {
		return QString();
	} else if (!report) {
		// Not counted yet: what is in the cache is shown in a moment.
		return (status.messages > 0) ? QString() : awaited();
	} else if (report->messages > 0) {
		return QString();
	} else if (Busy(stage)) {
		return awaited();
	} else if (!status.complete) {
		// Stopped or failed: the line above the numbers says so, and
		// the button continues instead of "Update".
		return (status.stage == Stage::Idle || status.stage == Stage::Done)
			? tr::lng_oblivion_stats_empty_cleared(tr::now)
			: QString();
	} else if (!report->cached) {
		return tr::lng_oblivion_stats_empty_chat(tr::now);
	}
	return tr::lng_oblivion_stats_empty_period(tr::now);
}

// Fills the scrolled part of the box, returns what updates it.
[[nodiscard]] ContentUpdate SetupContent(
		not_null<Ui::VerticalLayout*> content,
		ContentArgs &&args) {
	const auto zone = args.zone;
	const auto group = args.group;
	const auto person = args.person;
	const auto sticker = args.sticker;
	const auto &padding = st::boxRowPadding;

	struct State {
		rpl::variable<QString> empty;
		std::shared_ptr<const Report> shown;
		bool filled = false;
	};
	const auto state = content->lifetime().make_state<State>();

	const auto empty = content->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			content,
			object_ptr<Ui::FlatLabel>(
				content,
				state->empty.value(),
				st::membersAbout),
			padding + QMargins(0, Scaled(36), 0, Scaled(36))),
		style::margins(),
		style::al_top);
	empty->toggle(false, anim::type::instant);

	const auto totals = content->add(
		object_ptr<Ui::SlideWrap<Cells>>(
			content,
			object_ptr<Cells>(content),
			padding + QMargins(0, Scaled(kSectionSkip), 0, 0)),
		style::margins());
	totals->toggle(false, anim::type::instant);

	const auto people = AddSection<Ranking>(
		content,
		(group
			? tr::lng_oblivion_stats_section_people
			: tr::lng_oblivion_stats_section_people_private)());
	const auto hours = AddSection<Bars>(
		content,
		tr::lng_oblivion_stats_section_hours());
	const auto weekdays = AddSection<Bars>(
		content,
		tr::lng_oblivion_stats_section_weekdays());
	const auto calendar = AddSection<Heatmap>(
		content,
		tr::lng_oblivion_stats_section_calendar());
	const auto byDays = AddSection<Bars>(
		content,
		tr::lng_oblivion_stats_section_days());
	const auto byMonths = AddSection<Bars>(
		content,
		tr::lng_oblivion_stats_section_months());
	const auto words = AddSection<TopList>(
		content,
		tr::lng_oblivion_stats_section_words(),
		true);
	const auto emoji = AddSection<EmojiGrid>(
		content,
		tr::lng_oblivion_stats_section_emoji());
	const auto stickers = AddSection<Stickers>(
		content,
		tr::lng_oblivion_stats_section_stickers());
	const auto media = AddSection<TopList>(
		content,
		tr::lng_oblivion_stats_section_media(),
		false);
	const auto facts = AddSection<Facts>(
		content,
		tr::lng_oblivion_stats_section_facts());

	const auto about = content->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			content,
			object_ptr<Ui::FlatLabel>(
				content,
				tr::lng_oblivion_stats_about(),
				st::boxDividerLabel),
			padding + QMargins(0, Scaled(kSectionSkip), 0, Scaled(8))),
		style::margins());
	about->toggle(false, anim::type::instant);

	if (args.repaint) {
		// Photos of the participants and images of the stickers come
		// later than the numbers.
		std::move(
			args.repaint
		) | rpl::on_next([=] {
			people.widget->update();
			stickers.widget->update();
		}, content->lifetime());
	}

	return [=](std::shared_ptr<const Report> report, const Status &status) {
		const auto text = EmptyText(report.get(), status);
		const auto has = report && (report->messages > 0);
		state->empty = text;
		empty->toggle(!text.isEmpty(), anim::type::instant);
		if (state->filled && state->shown == report) {
			// Only the state of the reading has changed: the charts
			// (and the tooltip over them) stay as they are.
			return;
		}
		state->filled = true;
		state->shown = report;
		totals->toggle(has, anim::type::instant);
		about->toggle(has, anim::type::instant);
		if (!has) {
			people.toggle(false);
			hours.toggle(false);
			weekdays.toggle(false);
			calendar.toggle(false);
			byDays.toggle(false);
			byMonths.toggle(false);
			words.toggle(false);
			emoji.toggle(false);
			stickers.toggle(false);
			media.toggle(false);
			facts.toggle(false);
			return;
		}
		const auto &data = *report;

		// Totals.
		const auto perDay = data.activeDays
			? int(base::SafeRound(data.messages / float64(data.activeDays)))
			: 0;
		const auto perMessage = data.textMessages
			? (data.words / float64(data.textMessages))
			: 0.;
		totals->entity()->setCells({
			{
				Number(data.messages),
				tr::lng_oblivion_stats_cell_messages(tr::now),
			},
			{
				Number(data.words),
				tr::lng_oblivion_stats_cell_words(tr::now),
			},
			(group
				? Cell{
					Number(int(data.persons.size())),
					tr::lng_oblivion_stats_cell_people(tr::now),
				}
				: Cell{
					Number(data.emoji),
					tr::lng_oblivion_stats_cell_emoji(tr::now),
				}),
			{
				Number(data.activeDays),
				tr::lng_oblivion_stats_cell_days(tr::now),
			},
			{
				Number(perDay),
				tr::lng_oblivion_stats_cell_per_day(tr::now),
			},
			{
				Fraction(perMessage),
				tr::lng_oblivion_stats_cell_length(tr::now),
			},
		});

		// Participants.
		auto rows = std::vector<PersonRow>();
		const auto shown = std::min(int(data.persons.size()), kTopPersons);
		const auto most = data.persons.empty()
			? 1
			: std::max(data.persons.front().messages, 1);
		auto firstName = QString();
		for (auto i = 0; i != int(data.persons.size()); ++i) {
			const auto &stats = data.persons[i];
			const auto first = (stats.id == data.firstSender);
			if (i >= shown && !first) {
				continue;
			}
			auto view = person
				? person(stats.id, stats.name)
				: PersonView{ .name = stats.name };
			if (first) {
				firstName = view.name;
			}
			if (i >= shown) {
				continue;
			}
			auto parts = QStringList();
			parts.push_back(Messages(stats.messages));
			parts.push_back(tr::lng_oblivion_stats_words(
				tr::now,
				lt_count_decimal,
				stats.words));
			if (!group && stats.answers > 0) {
				parts.push_back((view.self
					? tr::lng_oblivion_stats_answer_you
					: tr::lng_oblivion_stats_answer_other)(
						tr::now,
						lt_time,
						FormatSpan(stats.answerTime)));
			}
			rows.push_back({
				.view = std::move(view),
				.percent = Percent(stats.messages, data.messages),
				.about = parts.join(u" · "_q),
				.share = stats.messages / float64(most),
			});
		}
		const auto hidden = int(data.persons.size()) - shown;
		people.widget->setRows(std::move(rows), (hidden > 0)
			? tr::lng_oblivion_stats_people_more(
				tr::now,
				lt_count_decimal,
				hidden)
			: QString());
		people.toggle(!data.persons.empty());

		// By hour of day and by day of week.
		auto hourLabels = std::vector<std::pair<int, QString>>();
		for (auto hour = 0; hour < 24; hour += 3) {
			hourLabels.emplace_back(
				hour,
				u"%1"_q.arg(hour, 2, 10, QChar('0')));
		}
		const auto hourValues = data.hours;
		hours.widget->setData({
			.values = std::vector<int>(begin(hourValues), end(hourValues)),
			.labels = std::move(hourLabels),
			.tip = [=](int index) {
				return u"%1:00–%2:00 — "_q.arg(
					index,
					2,
					10,
					QChar('0')
				).arg((index + 1) % 24, 2, 10, QChar('0'))
					+ Messages(hourValues[index]);
			},
		});
		hours.toggle(true);

		auto weekdayLabels = std::vector<std::pair<int, QString>>();
		for (auto weekday = 0; weekday != 7; ++weekday) {
			weekdayLabels.emplace_back(weekday, WeekdayShort(weekday));
		}
		const auto weekdayValues = data.weekdays;
		weekdays.widget->setData({
			.values = std::vector<int>(
				begin(weekdayValues),
				end(weekdayValues)),
			.labels = std::move(weekdayLabels),
			.tip = [=](int index) {
				return WeekdayLong(index)
					+ u" — "_q
					+ Messages(weekdayValues[index]);
			},
		});
		weekdays.toggle(true);

		// The calendar and the chart by day cover the same days: from
		// the start of the period (or the first message) till today.
		const auto fromDay = (data.from > 0)
			? std::min(zone->day(data.from), data.firstDay)
			: data.firstDay;
		const auto tillDay = (data.till > 0)
			? std::max(zone->day(data.till), data.lastDay)
			: data.lastDay;
		calendar.widget->setData(data.firstDay, data.days, fromDay, tillDay);
		calendar.toggle(true);

		const auto daysCount = tillDay - fromDay + 1;
		if (daysCount >= 2 && daysCount <= kDaysChartLimit) {
			auto values = std::vector<int>(daysCount, 0);
			for (auto i = 0; i != int(data.days.size()); ++i) {
				const auto index = data.firstDay + i - fromDay;
				if (index >= 0 && index < daysCount) {
					values[index] = data.days[i];
				}
			}
			auto labels = std::vector<std::pair<int, QString>>();
			for (auto i = 0; i != daysCount; ++i) {
				const auto date = DateOfDay(fromDay + i);
				if (date.day == 1) {
					labels.emplace_back(i, MonthShort(date.month));
				}
			}
			if (labels.empty()) {
				labels.emplace_back(0, FormatDayCompact(fromDay));
			}
			const auto shared = std::make_shared<std::vector<int>>(values);
			byDays.widget->setData({
				.values = std::move(values),
				.labels = std::move(labels),
				.tip = [=](int index) {
					return FormatDay(fromDay + index)
						+ u" — "_q
						+ ((*shared)[index]
							? Messages((*shared)[index])
							: tr::lng_oblivion_stats_tip_none(tr::now));
				},
			});
			byDays.toggle(true);
		} else {
			byDays.toggle(false);
		}

		if (data.months.size() >= 2) {
			const auto months = std::make_shared<std::vector<MonthValue>>(
				data.months);
			const auto count = int(months->size());
			auto values = std::vector<int>();
			auto labels = std::vector<std::pair<int, QString>>();
			for (auto i = 0; i != count; ++i) {
				const auto &month = (*months)[i];
				values.push_back(month.count);
				if (count <= 14) {
					labels.emplace_back(i, MonthShort(month.month));
				} else if (month.month == 1) {
					labels.emplace_back(i, QString::number(month.year));
				}
			}
			if (labels.empty()) {
				labels.emplace_back(0, QString::number(months->front().year));
			}
			byMonths.widget->setData({
				.values = std::move(values),
				.labels = std::move(labels),
				.tip = [=](int index) {
					const auto &month = (*months)[index];
					return MonthLong(month.month)
						+ QChar(' ')
						+ QString::number(month.year)
						+ u" — "_q
						+ (month.count
							? Messages(month.count)
							: tr::lng_oblivion_stats_tip_none(tr::now));
				},
			});
			byMonths.toggle(true);
		} else {
			byMonths.toggle(false);
		}

		// Words, emoji, stickers.
		auto wordItems = std::vector<ListItem>();
		for (const auto &word : data.topWords) {
			wordItems.push_back({
				.text = word.text,
				.value = Number(word.count),
				.share = word.count
					/ float64(std::max(data.topWords.front().count, 1)),
			});
		}
		words.widget->setItems(std::move(wordItems));
		words.toggle(!data.topWords.empty());

		emoji.widget->setItems(data.topEmoji);
		emoji.toggle(!data.topEmoji.empty());

		auto stickerItems = std::vector<StickerItem>();
		for (const auto &top : data.topStickers) {
			stickerItems.push_back({
				.view = (sticker
					? sticker(top)
					: StickerView{ .emoji = top.info.emoji }),
				.count = Number(top.count),
			});
		}
		stickers.widget->setItems(std::move(stickerItems));
		stickers.toggle(!data.topStickers.empty());

		// Kinds of messages.
		auto kinds = std::vector<std::pair<QString, int>>();
		for (auto i = 0; i != kKindCount; ++i) {
			if (data.kinds[i] > 0) {
				kinds.emplace_back(KindName(Kind(i)), data.kinds[i]);
			}
		}
		if (data.links > 0) {
			kinds.emplace_back(
				tr::lng_oblivion_stats_kind_link(tr::now),
				data.links);
		}
		ranges::stable_sort(kinds, std::greater<>(), [](const auto &pair) {
			return pair.second;
		});
		auto kindItems = std::vector<ListItem>();
		for (const auto &[name, count] : kinds) {
			kindItems.push_back({
				.text = name,
				.value = Number(count),
				.share = count / float64(std::max(kinds.front().second, 1)),
			});
		}
		media.widget->setItems(std::move(kindItems));
		media.toggle(!kinds.empty());

		// Facts.
		auto list = std::vector<Fact>();
		const auto dash = u" — "_q;
		list.push_back({
			tr::lng_oblivion_stats_fact_period(tr::now),
			(data.firstDay == data.lastDay)
				? FormatDay(data.firstDay)
				: FormatDays(data.firstDay, data.lastDay, false),
		});
		if (data.firstDate > 0) {
			auto value = FormatMoment(data.firstDate, *zone);
			if (!firstName.isEmpty()) {
				value += u", "_q + firstName;
			}
			// It is the oldest message that was counted: the first one of
			// the chat if the history is read to its start.
			list.push_back({
				(data.wholeHistory
					? tr::lng_oblivion_stats_fact_first
					: tr::lng_oblivion_stats_fact_first_period)(tr::now),
				value,
			});
		}
		if (data.longest.days > 0) {
			auto value = tr::lng_oblivion_stats_days(
				tr::now,
				lt_count_decimal,
				data.longest.days);
			if (data.longest.days > 1) {
				value += u", "_q
					+ FormatDays(
						data.longest.fromDay,
						data.longest.fromDay + data.longest.days - 1,
						true);
			}
			list.push_back({
				tr::lng_oblivion_stats_fact_streak(tr::now),
				value,
			});
		}
		if (data.current.days > 0) {
			list.push_back({
				tr::lng_oblivion_stats_fact_streak_now(tr::now),
				tr::lng_oblivion_stats_days(
					tr::now,
					lt_count_decimal,
					data.current.days),
			});
		}
		for (auto i = 0; i != int(data.topDays.size()); ++i) {
			const auto &day = data.topDays[i];
			list.push_back({
				(i
					? QString()
					: (data.topDays.size() > 1)
					? tr::lng_oblivion_stats_fact_top_days(tr::now)
					: tr::lng_oblivion_stats_fact_top_day(tr::now)),
				FormatDay(day.day) + dash + Messages(day.count),
			});
		}
		const auto share = [&](int count) {
			return Number(count)
				+ u" ("_q
				+ Percent(count, data.messages)
				+ QChar(')');
		};
		list.push_back({
			tr::lng_oblivion_stats_fact_replies(tr::now),
			share(data.replies),
		});
		list.push_back({
			tr::lng_oblivion_stats_fact_forwards(tr::now),
			share(data.forwards),
		});
		if (data.textMessages > 0) {
			list.push_back({
				tr::lng_oblivion_stats_fact_length(tr::now),
				tr::lng_oblivion_stats_words(
					tr::now,
					lt_count_decimal,
					int(base::SafeRound(perMessage)))
					+ u", "_q
					+ tr::lng_oblivion_stats_chars(
						tr::now,
						lt_count_decimal,
						int(base::SafeRound(
							data.chars / float64(data.textMessages)))),
			});
		}
		facts.widget->setFacts(std::move(list));
		facts.toggle(true);
	};
}

[[nodiscard]] int PeriodIndex(Period period) {
	switch (period) {
	case Period::Months3: return 0;
	case Period::Year: return 1;
	case Period::All: return 2;
	}
	return 0;
}

[[nodiscard]] Period PeriodOfIndex(int index) {
	return (index == 1)
		? Period::Year
		: (index == 2)
		? Period::All
		: Period::Months3;
}

void StatsBox(not_null<Ui::GenericBox*> box, BoxArgs &&args) {
	struct State {
		Status status;
		std::optional<Period> sliderPeriod; // The last one the switch got.
		std::shared_ptr<const Report> report;
		rpl::variable<QString> updated;
		rpl::variable<QString> progressText;
		rpl::variable<QString> progressNote;
		rpl::variable<QString> button;
		ContentUpdate updateContent;
		Fn<void()> refreshStatus;
	};
	const auto state = box->lifetime().make_state<State>();
	box->lifetime().add(std::move(args.keep));
	const auto show = args.show;
	const auto zone = args.zone;
	const auto fixedNow = args.now;
	const auto setPeriod = args.setPeriod;
	const auto refresh = args.refresh;
	const auto stop = args.stop;
	const auto clear = args.clear;
	const auto now = [=] {
		return fixedNow ? fixedNow : base::unixtime::now();
	};

	box->setWidth(Scaled(kBoxWidth));
	box->setMaxHeight(Scaled(kBoxMaxHeight));
	box->setTitle(tr::lng_oblivion_stats_menu());

	// The name, the period and the progress stay in sight above the
	// scrolled numbers, the box draws a line under the pinned content.
	const auto top = box->setPinnedToTopContent(
		object_ptr<Ui::VerticalLayout>(box));
	const auto &padding = st::boxRowPadding;

	// One line: a long name of a chat is cut with an ellipsis, it does
	// not push the numbers down.
	const auto titleSt = box->lifetime().make_state<style::FlatLabel>(
		st::boxLabel);
	titleSt->maxHeight = std::max(
		titleSt->style.lineHeight,
		titleSt->style.font->height);
	top->add(
		object_ptr<Ui::FlatLabel>(
			top,
			rpl::single(tr::bold(args.title)),
			*titleSt),
		padding);
	const auto updated = top->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			top,
			object_ptr<Ui::FlatLabel>(
				top,
				state->updated.value(),
				st::boxDividerLabel),
			padding + QMargins(0, Scaled(2), 0, 0)),
		style::margins());
	updated->toggle(false, anim::type::instant);

	const auto slider = top->add(
		object_ptr<Ui::SettingsSlider>(top, st::settingsSlider),
		padding + QMargins(0, Scaled(10), 0, 0));
	slider->setSections(std::vector<QString>{
		tr::lng_oblivion_stats_period_3m(tr::now),
		tr::lng_oblivion_stats_period_year(tr::now),
		tr::lng_oblivion_stats_period_all(tr::now),
	});

	const auto progress = top->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			top,
			object_ptr<Ui::VerticalLayout>(top)),
		style::margins());
	const auto progressInner = progress->entity();
	const auto progressLabel = progressInner->add(
		object_ptr<Ui::FlatLabel>(
			progressInner,
			state->progressText.value(),
			st::boxLabel),
		padding + QMargins(0, Scaled(12), 0, 0));
	const auto line = progressInner->add(
		object_ptr<Ui::SlideWrap<ProgressLine>>(
			progressInner,
			object_ptr<ProgressLine>(progressInner),
			padding + QMargins(0, Scaled(8), 0, 0)),
		style::margins());
	const auto note = progressInner->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			progressInner,
			object_ptr<Ui::FlatLabel>(
				progressInner,
				state->progressNote.value(),
				st::boxDividerLabel),
			padding + QMargins(0, Scaled(6), 0, 0)),
		style::margins());
	progress->toggle(false, anim::type::instant);
	Ui::AddSkip(top, Scaled(12));

	const auto content = box->verticalLayout();
	state->updateContent = SetupContent(content, ContentArgs{
		.zone = zone,
		.group = args.group,
		.person = args.person,
		.sticker = args.sticker,
		.repaint = std::move(args.repaint),
	});

	// In one column with the rest of the box, not as wide as the rows
	// of the settings.
	const auto clearSt = box->lifetime().make_state<style::SettingsButton>(
		st::settingsAttentionButton);
	clearSt->padding.setLeft(padding.left());
	clearSt->padding.setRight(padding.right());
	const auto clearWrap = content->add(
		object_ptr<Ui::SlideWrap<Ui::SettingsButton>>(
			content,
			object_ptr<Ui::SettingsButton>(
				content,
				tr::lng_oblivion_stats_clear(),
				*clearSt)),
		style::margins());
	clearWrap->toggle(false, anim::type::instant);
	clearWrap->entity()->setClickedCallback([=] {
		show->showBox(Ui::MakeConfirmBox({
			.text = tr::lng_oblivion_stats_clear_sure(tr::now),
			.confirmed = [=](Fn<void()> close) {
				if (clear) {
					clear();
				}
				close();
			},
			.confirmText = tr::lng_oblivion_stats_clear(),
			.confirmStyle = &st::attentionBoxButton,
		}));
	});
	Ui::AddSkip(content, Scaled(8));

	// An error does not look like the usual line about the loading. The
	// color is set by hand, so it is taken again from a changed palette.
	const auto refreshErrorColor = [=] {
		progressLabel->setTextColorOverride(
			(state->status.stage == Stage::Failed)
				? std::optional<QColor>(st::boxTextFgError->c)
				: std::optional<QColor>());
	};
	style::PaletteChanged(
	) | rpl::on_next(refreshErrorColor, box->lifetime());

	state->refreshStatus = [=] {
		const auto &status = state->status;
		const auto stage = status.stage;
		const auto busy = Busy(stage);
		const auto incomplete = !status.complete
			&& (stage == Stage::Idle
				|| stage == Stage::Done
				|| stage == Stage::Stopped)
			&& (status.messages > 0 || stage == Stage::Stopped);
		const auto shown = busy
			|| incomplete
			|| (stage == Stage::Loading)
			|| (stage == Stage::Failed);

		auto text = QString();
		switch (stage) {
		case Stage::Loading:
			text = tr::lng_oblivion_stats_status_loading(tr::now);
			break;
		case Stage::Waiting:
			text = tr::lng_oblivion_stats_status_waiting(
				tr::now,
				lt_chat,
				Shortened(status.waitingFor, kWaitingNameLimit));
			break;
		case Stage::Reading:
			text = (status.oldest > 0)
				? tr::lng_oblivion_stats_status_reading_till(
					tr::now,
					lt_date,
					FormatDay(zone->day(status.oldest)))
				: tr::lng_oblivion_stats_status_reading(tr::now);
			break;
		case Stage::Flood:
			text = tr::lng_oblivion_stats_status_flood(
				tr::now,
				lt_time,
				FormatSpan(std::max(status.floodTill - now(), TimeId(1))));
			break;
		case Stage::Stopped:
			text = tr::lng_oblivion_stats_status_stopped(tr::now);
			break;
		case Stage::Failed:
			text = ErrorText(status);
			break;
		case Stage::Idle:
		case Stage::Done:
			text = tr::lng_oblivion_stats_status_incomplete(tr::now);
			break;
		}
		state->progressText = text;

		refreshErrorColor();

		auto notes = QStringList();
		if (status.messages > 0) {
			notes.push_back(tr::lng_oblivion_stats_status_count(
				tr::now,
				lt_count_decimal,
				status.messages));
		}
		if (busy) {
			notes.push_back(
				tr::lng_oblivion_stats_status_background(tr::now));
		}

		// Each on its own line: together they are a little longer than
		// the box is wide and would leave one word on the second line.
		state->progressNote = notes.join(QChar('\n'));
		note->toggle(!notes.isEmpty(), anim::type::instant);
		line->toggle(
			(stage != Stage::Loading)
				&& (busy || status.messages > 0),
			anim::type::instant);
		line->entity()->setValue(
			status.progress,
			(stage == Stage::Reading));
		progress->toggle(shown, anim::type::instant);

		const auto checked = status.checked;
		const auto fresh = (checked > 0) && status.complete && !busy;
		if (fresh) {
			const auto passed = now() - checked;
			state->updated = (passed >= 0 && passed < kJustNow)
				? tr::lng_oblivion_stats_updated_now(tr::now)
				: tr::lng_oblivion_stats_updated(
					tr::now,
					lt_date,
					FormatMoment(checked, *zone));
		}
		updated->toggle(fresh, anim::type::instant);

		state->button = busy
			? tr::lng_oblivion_stats_stop(tr::now)
			: (stage == Stage::Stopped)
			? tr::lng_oblivion_stats_resume(tr::now)
			: (stage == Stage::Failed)
			? tr::lng_oblivion_stats_retry(tr::now)
			: tr::lng_oblivion_stats_refresh(tr::now);

		// A cache that can't be read can still be removed.
		clearWrap->toggle(
			(status.messages > 0) || status.cacheFailed,
			anim::type::instant);

		// The switch follows the period only when the period changes.
		// It shows a click at once and reports it a moment later: moved
		// by every update of the progress, it would jump back in between
		// and the click would be lost.
		if (state->sliderPeriod != status.period) {
			state->sliderPeriod = status.period;
			const auto index = PeriodIndex(status.period);
			if (slider->activeSection() != index) {
				slider->setActiveSectionFast(index);
			}
		}
	};

	std::move(
		args.status
	) | rpl::on_next([=](Status &&status) {
		state->status = std::move(status);
		state->refreshStatus();
		state->updateContent(state->report, state->status);
	}, box->lifetime());

	std::move(
		args.reports
	) | rpl::on_next([=](std::shared_ptr<const Report> &&report) {
		state->report = std::move(report);
		state->updateContent(state->report, state->status);
	}, box->lifetime());

	// The wait asked by the server is counted down.
	base::timer_each(
		crl::time(1000)
	) | rpl::filter([=] {
		return (state->status.stage == Stage::Flood);
	}) | rpl::on_next([=] {
		state->refreshStatus();
	}, box->lifetime());

	slider->sectionActivated(
	) | rpl::on_next([=](int index) {
		const auto period = PeriodOfIndex(index);
		if (state->status.period != period && setPeriod) {
			setPeriod(period);
		}
	}, slider->lifetime());

	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	box->addLeftButton(state->button.value(), [=] {
		if (Busy(state->status.stage)) {
			if (stop) {
				stop();
			}
		} else if (refresh) {
			refresh();
		}
	});
}

// What shows the stickers of the top: the images of the documents the
// account knows, the others are asked for by their messages, all of a
// chat in one request, once.
struct StickerSource {
	not_null<Main::Session*> session;
	not_null<PeerData*> peer;
	std::map<uint64, std::shared_ptr<Data::DocumentMedia>> media;
	std::set<uint64> asked;
	rpl::event_stream<> updated;
};

[[nodiscard]] Image *StickerImage(
		not_null<StickerSource*> source,
		const TopSticker &sticker) {
	const auto i = source->media.find(sticker.id);
	if (i != end(source->media)) {
		return i->second->thumbnail();
	}
	const auto document = source->session->data().document(sticker.id);
	if (!document->sticker() || !document->hasThumbnail()) {
		return nullptr;
	}
	const auto history = sticker.info.old
		? static_cast<PeerData*>(source->peer->migrateFrom())
		: source->peer.get();
	auto origin = document->stickerSetOrigin();
	if (!origin && history && sticker.info.msgId) {
		origin = Data::FileOrigin(
			FullMsgId(history->id, MsgId(sticker.info.msgId)));
	}
	const auto view = document->createMediaView();
	view->thumbnailWanted(origin);
	source->media.emplace(sticker.id, view);
	return view->thumbnail();
}

[[nodiscard]] StickerView MakeStickerView(
		const std::shared_ptr<StickerSource> &source,
		const TopSticker &sticker) {
	const auto session = source->session;
	const auto document = session->data().document(sticker.id);
	if (!document->sticker()
		&& sticker.info.msgId
		&& source->asked.emplace(sticker.id).second) {
		const auto history = sticker.info.old
			? static_cast<PeerData*>(source->peer->migrateFrom())
			: source->peer.get();
		if (history) {
			const auto weak = std::weak_ptr<StickerSource>(source);
			session->api().requestMessageData(
				history,
				MsgId(sticker.info.msgId),
				[=] {
					if (const auto strong = weak.lock()) {
						strong->updated.fire({});
					}
				});
		}
	}
	return {
		.paint = [=](QPainter &p, QRect rect) {
			const auto image = StickerImage(source.get(), sticker);
			if (!image) {
				return false;
			}
			const auto size = image->size().scaled(
				rect.size(),
				Qt::KeepAspectRatio);
			auto hq = PainterHighQualityEnabler(p);
			p.drawImage(
				QRect(
					rect.x() + (rect.width() - size.width()) / 2,
					rect.y() + (rect.height() - size.height()) / 2,
					size.width(),
					size.height()),
				image->original());
			return true;
		},
		.emoji = sticker.info.emoji,
	};
}

// Snapshot scenes (OBLIVION_SELFTEST=ui, see oblivion_ui_snapshots.h):
// a generated history counted by the real code, at a fixed moment in
// a fixed time zone. Nothing is read or saved.

[[nodiscard]] QString SampleText(const char *ru, const char *en) {
	return QString::fromUtf8(CurrentLanguageIsRussian() ? ru : en);
}

[[nodiscard]] TimeId SampleNow() {
	return TimeId(int64(DayOfDate({ 2026, 9, 30 })) * 86400
		+ 17 * 3600
		+ 42 * 60
		- kSampleZone);
}

struct SamplePerson {
	uint64 id = 0;
	const char *ru = nullptr;
	const char *en = nullptr;
	int weight = 0;
};

constexpr auto kSampleSelf = uint64(1000001);

[[nodiscard]] std::vector<SamplePerson> SamplePersons(bool group) {
	if (!group) {
		return {
			{ kSampleSelf, "Вы", "You", 46 },
			{ 1000003, "Аня Смирнова", "Anna Smirnova", 54 },
		};
	}

	// The ids give the photos without a picture different colors. One
	// name is longer than its row: contacts are saved with notes.
	return {
		{ 1000003, "Аня Смирнова", "Anna Smirnova", 27 },
		{ kSampleSelf, "Вы", "You", 21 },
		{ 1000005, "Борис Ковалёв", "Boris Kovalev", 17 },
		{ 1000007, "Вера Орлова", "Vera Orlova", 12 },
		{ 1000002, "Глеб Назаров", "Gleb Nazarov", 9 },
		{ 1000004, "Даша Ким", "Dasha Kim", 6 },
		{
			1000006,
			"Константин Константинопольский, сантехник из соседнего "
			"подъезда",
			"Konstantin Konstantinopolsky, the plumber from the next "
			"entrance",
			4,
		},
		{ 1000010, "Женя Белова", "Zhenya Belova", 2 },
		{ 1000012, "Игорь Соколов", "Igor Sokolov", 1 },
		{ 1000014, "Катя Морозова", "Katya Morozova", 1 },
		{ 1000016, "Лев Зайцев", "Lev Zaitsev", 1 },
		{ 1000018, "Марина Юдина", "Marina Yudina", 1 },
	};
}

[[nodiscard]] QString SampleEmoji(int index) {
	constexpr char32_t kCodes[] = {
		0x1F602,
		0x2764,
		0x1F44D,
		0x1F525,
		0x1F60D,
		0x1F389,
		0x1F914,
		0x1F62D,
		0x1F64F,
		0x1F60E,
		0x1F631,
		0x1F37F,
	};
	const auto code = kCodes[index % int(std::size(kCodes))];
	auto result = QString();
	result.append(QChar(QChar::requiresSurrogates(code)
		? QChar::highSurrogate(code)
		: char16_t(code)));
	if (QChar::requiresSurrogates(code)) {
		result.append(QChar(QChar::lowSurrogate(code)));
	} else {
		result.append(QChar(0xFE0F));
	}
	return result;
}

// `complete`: the whole history is read. Otherwise only the last months
// are, as in the middle of the first reading.
struct Sample {
	std::shared_ptr<const Report> report;
	int messages = 0;
	TimeId oldest = 0;
};

[[nodiscard]] Sample GenerateSample(bool group, Period period, bool complete) {
	const auto ru = CurrentLanguageIsRussian();
	const auto phrasesRu = std::vector<const char*>{
		"Привет! Как дела?",
		"Короче, я сегодня не успею, давай завтра",
		"ахахах, точно",
		"Спасибо большое, очень выручил",
		"Смотри, какая погода сегодня",
		"Давай созвонимся вечером, обсудим поездку",
		"Хорошо, договорились",
		"Я уже выхожу, буду через полчаса",
		"Блин, опять забыл ключи",
		"Кто идёт в кино в субботу?",
		"Отправил фото с поездки, посмотри",
		"Завтра работа до вечера, потом свободен",
		"Купи, пожалуйста, хлеб и молоко",
		"Спокойной ночи",
		"Доброе утро!",
		"Понял, спасибо",
		"Это просто супер",
		"Поездка получилась отличная, надо повторить",
		"Короче, встречаемся у метро",
		"Ладно, напишу позже",
	};
	const auto phrasesEn = std::vector<const char*>{
		"Hello! How are you?",
		"Anyway, I can't make it today, let's meet tomorrow",
		"hahaha, exactly",
		"Thanks a lot, you really helped",
		"Look at the weather today",
		"Let's call in the evening and discuss the trip",
		"Okay, deal",
		"I'm leaving now, will be there in half an hour",
		"Damn, forgot the keys again",
		"Who is going to the cinema on Saturday?",
		"Sent the photos from the trip, take a look",
		"Work till the evening tomorrow, free after that",
		"Please buy some bread and milk",
		"Good night",
		"Good morning!",
		"Got it, thanks",
		"This is just great",
		"The trip was great, we should repeat it",
		"Anyway, meet me at the station",
		"Fine, will write later",
	};
	const auto &phrases = ru ? phrasesRu : phrasesEn;
	const auto persons = SamplePersons(group);
	auto weights = 0;
	for (const auto &one : persons) {
		weights += one.weight;
	}

	auto seed = uint32(group ? 20260930 : 20240214);
	const auto next = [&](int limit) {
		seed = seed * 1664525U + 1013904223U;
		return int((seed >> 8) % uint32(std::max(limit, 1)));
	};
	const auto zone = Zone::Fixed(kSampleZone);
	const auto now = SampleNow();
	const auto today = zone->day(now);
	const auto firstDay = complete
		? DayOfDate(group ? CivilDate{ 2023, 11, 5 } : CivilDate{ 2024, 2, 14 })
		: (today - 150);
	constexpr int kHourWeights[] = {
		2, 1, 0, 0, 0, 0, 1, 3, 6, 8, 9, 8,
		9, 10, 8, 7, 8, 10, 13, 16, 18, 17, 12, 6,
	};
	auto hourTotal = 0;
	for (const auto weight : kHourWeights) {
		hourTotal += weight;
	}

	auto page = RawPage();
	page.direct = !group;
	auto id = int32(1000);
	for (auto day = firstDay; day <= today; ++day) {
		const auto weekday = WeekdayOfDay(day);
		const auto age = (day - firstDay) / float64(today - firstDay + 1);
		auto count = (group ? 10 : 7)
			+ next(group ? 22 : 14)
			+ ((weekday >= 5) ? next(12) : 0)
			+ int(age * (group ? 14 : 6));
		if (!next(9) || (next(40) == 7)) {
			count = 0;
		} else if (!next(45)) {
			count = count * 5 / 2;
		}
		const auto dayStart = zone->dayStart(day);
		auto moments = std::vector<TimeId>();
		for (auto i = 0; i != count; ++i) {
			auto pick = next(hourTotal);
			auto hour = 0;
			while (pick >= kHourWeights[hour]) {
				pick -= kHourWeights[hour++];
			}
			const auto moment = dayStart + hour * 3600 + next(3600);
			if (moment <= now) {
				moments.push_back(moment);
			}
		}
		std::sort(begin(moments), end(moments));
		for (const auto moment : moments) {
			auto pick = next(weights);
			auto sender = persons.front().id;
			for (const auto &one : persons) {
				if (pick < one.weight) {
					sender = one.id;
					break;
				}
				pick -= one.weight;
			}
			auto raw = RawMessage();
			raw.id = ++id;
			raw.date = moment;
			raw.sender = sender;
			const auto kind = next(100);
			if (kind < 8) {
				raw.kind = Kind::Photo;
			} else if (kind < 14) {
				raw.kind = Kind::Sticker;
				const auto index = (next(6) * next(6)) / 5;
				raw.sticker = uint64(7000) + index;
				raw.stickerEmoji = SampleEmoji(index + 2);
			} else if (kind < 18) {
				raw.kind = Kind::Voice;
			} else if (kind < 20) {
				raw.kind = Kind::Video;
			} else if (kind < 21) {
				raw.kind = Kind::Round;
			} else if (kind < 22) {
				raw.kind = Kind::Gif;
			} else if (kind < 23) {
				raw.kind = next(2) ? Kind::File : Kind::Music;
			} else if (kind == 23 && !next(4)) {
				raw.kind = next(2) ? Kind::Poll : Kind::Location;
			} else if (kind == 24 && !group && !next(3)) {
				raw.kind = Kind::Call;
			}
			if (raw.kind == Kind::Text
				|| (raw.kind == Kind::Photo && !next(3))) {
				raw.text = QString::fromUtf8(
					phrases[(next(int(phrases.size())) * next(7)) / 6
						% int(phrases.size())]);
				if (!next(4)) {
					raw.text += QChar(' ')
						+ SampleEmoji((next(12) * next(12)) / 11);
				}
			}
			if (!next(6)) {
				raw.flags = uchar(raw.flags | kReply);
			}
			if (!next(30)) {
				raw.flags = uchar(raw.flags | kForwarded);
			}
			if (!next(40)) {
				raw.flags = uchar(raw.flags | kLink);
			}
			page.minId = page.minId ? page.minId : raw.id;
			page.maxId = raw.id;
			page.minDate = page.minDate ? page.minDate : raw.date;
			page.maxDate = raw.date;
			page.messages.push_back(std::move(raw));
		}
	}
	page.returned = int(page.messages.size());
	page.total = complete ? page.returned : (page.returned * 5);
	for (const auto &one : persons) {
		page.names.push_back({ one.id, SampleText(one.ru, one.en) });
	}

	auto model = Model();
	model.add(page, *zone);
	if (complete) {
		auto end = RawPage();
		end.type = PageType::Older;
		model.add(end, *zone);
	}
	auto result = Sample();
	result.messages = model.count();
	result.oldest = page.minDate;
	result.report = std::make_shared<const Report>(Analyze(model, *zone, {
		.from = PeriodStart(period, now, *zone),
		.till = now,
		.group = group,
	}));
	return result;
}

// The scenes are created again for every theme, the same history is
// generated and counted once.
[[nodiscard]] Sample MakeSample(bool group, Period period, bool complete) {
	using Key = std::tuple<bool, int, bool, bool>;
	static auto Cache = std::map<Key, Sample>();
	const auto key = Key(
		group,
		int(period),
		complete,
		CurrentLanguageIsRussian());
	const auto i = Cache.find(key);
	return (i != end(Cache))
		? i->second
		: Cache.emplace(
			key,
			GenerateSample(group, period, complete)).first->second;
}

[[nodiscard]] PersonView SamplePersonView(uint64 id, const QString &name) {
	const auto self = (id == kSampleSelf);
	const auto shown = self ? tr::lng_oblivion_stats_you(tr::now) : name;
	return {
		.name = shown,
		.paintUserpic = EmptyUserpicPainter(id, shown),
		.self = self,
	};
}

struct SampleArgs {
	bool group = false;
	Period period = Period::All;
	Stage stage = Stage::Done;
	bool complete = true; // The history is read to its start.
	bool counted = true; // There is a report.
	bool noMessages = false;
	QString error;
	QString title; // Empty: the usual name of the sample chat.
	bool cacheFailed = false; // The saved data could not be read.
};

[[nodiscard]] Status SampleStatus(const SampleArgs &args, const Sample &sample) {
	const auto now = SampleNow();
	const auto zone = Zone::Fixed(kSampleZone);
	auto status = Status();
	status.stage = args.stage;
	status.period = args.period;
	status.from = PeriodStart(args.period, now, *zone);
	status.messages = sample.messages;
	status.total = args.complete ? sample.messages : (sample.messages * 5);
	status.oldest = sample.oldest;
	status.complete = args.complete;
	status.checked = args.complete ? (now - 40) : 0;
	status.progress = args.complete ? 1. : 0.2;
	status.error = args.error;
	status.cacheFailed = args.cacheFailed;
	if (args.stage == Stage::Flood) {
		status.floodTill = now + 27;
	} else if (args.stage == Stage::Waiting) {
		status.waitingFor = SampleText(
			"Семья Смирновых: дача, дни рождения и все-все-все",
			"The Smirnov family: the dacha, birthdays and everyone");
	}
	return status;
}

[[nodiscard]] QString SampleTitle(bool group) {
	return group
		? SampleText("Друзья и поездки", "Friends and trips")
		: SampleText("Аня Смирнова", "Anna Smirnova");
}

[[nodiscard]] object_ptr<Ui::BoxContent> SampleBox(
		std::shared_ptr<Ui::Show> show,
		SampleArgs args) {
	auto sample = args.noMessages
		? Sample{ .report = std::make_shared<const Report>() }
		: MakeSample(args.group, args.period, args.complete);
	if (!args.counted) {
		sample.report = nullptr;
		sample.messages = 0;
		sample.oldest = 0;
	}
	return Box(StatsBox, BoxArgs{
		.show = std::move(show),
		.title = args.title.isEmpty() ? SampleTitle(args.group) : args.title,
		.group = args.group,
		.zone = Zone::Fixed(kSampleZone),
		.status = rpl::single(SampleStatus(args, sample)),
		.reports = rpl::single(sample.report),
		.person = SamplePersonView,
		.now = SampleNow(),
	});
}

// The scrolled part alone, at its full height.
[[nodiscard]] QWidget *SampleContent(
		not_null<Ui::RpWidget*> parent,
		bool group,
		Period period) {
	const auto result = Ui::CreateChild<Ui::VerticalLayout>(parent.get());
	const auto args = SampleArgs{ .group = group, .period = period };
	const auto sample = MakeSample(group, period, true);
	const auto update = SetupContent(result, ContentArgs{
		.zone = Zone::Fixed(kSampleZone),
		.group = group,
		.person = SamplePersonView,
	});
	update(sample.report, SampleStatus(args, sample));
	result->paintRequest(
	) | rpl::on_next([=](QRect clip) {
		QPainter(result).fillRect(clip, st::boxBg);
	}, result->lifetime());
	return result;
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	const auto size = QSize(Scaled(kBoxWidth + 80), 0);
	RegisterBoxScene(u"chat_stats_private"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return SampleBox(std::move(show), {});
	});
	RegisterBoxScene(u"chat_stats_group"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return SampleBox(std::move(show), {
			.group = true,
			.period = Period::Year,
		});
	});
	RegisterBoxScene(u"chat_stats_collecting"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return SampleBox(std::move(show), {
			.group = true,
			.stage = Stage::Reading,
			.complete = false,
		});
	});
	RegisterBoxScene(u"chat_stats_starting"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return SampleBox(std::move(show), {
			.period = Period::Months3,
			.stage = Stage::Reading,
			.complete = false,
			.counted = false,
		});
	});
	RegisterBoxScene(u"chat_stats_empty"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return SampleBox(std::move(show), {
			.period = Period::Months3,
			.noMessages = true,
		});
	});
	RegisterBoxScene(u"chat_stats_flood"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return SampleBox(std::move(show), {
			.period = Period::Year,
			.stage = Stage::Flood,
			.complete = false,
		});
	});
	RegisterBoxScene(u"chat_stats_stopped"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return SampleBox(std::move(show), {
			.group = true,
			.stage = Stage::Stopped,
			.complete = false,
		});
	});
	RegisterBoxScene(u"chat_stats_failed"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return SampleBox(std::move(show), {
			.group = true,
			.period = Period::Months3,
			.stage = Stage::Failed,
			.complete = false,
			.counted = false,
			.error = u"CHANNEL_PRIVATE"_q,
		});
	});

	// An error in the middle of the reading: what was read stays.
	RegisterBoxScene(u"chat_stats_failed_partial"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return SampleBox(std::move(show), {
			.stage = Stage::Failed,
			.complete = false,
			.error = u"RPC_CALL_FAIL"_q,
		});
	});

	// Another chat is being read first. The name of this one is longer
	// than the box is wide.
	RegisterBoxScene(u"chat_stats_waiting"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return SampleBox(std::move(show), {
			.group = true,
			.period = Period::Year,
			.stage = Stage::Waiting,
			.complete = false,
			.title = SampleText(
				"Поездка на Алтай, август 2026: билеты, маршрут, список "
				"вещей, фото и видео",
				"The Altai trip, August 2026: tickets, the route, the "
				"packing list, photos and videos"),
		});
	});

	// In the line with nothing read yet: only a hint under the progress.
	RegisterBoxScene(u"chat_stats_waiting_empty"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return SampleBox(std::move(show), {
			.period = Period::Months3,
			.stage = Stage::Waiting,
			.complete = false,
			.counted = false,
		});
	});

	// The file of the cache is there, but it could not be read: the
	// button tries again, "Clear data" removes the file.
	RegisterBoxScene(u"chat_stats_cache_failed"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return SampleBox(std::move(show), {
			.period = Period::Months3,
			.stage = Stage::Failed,
			.complete = false,
			.counted = false,
			.cacheFailed = true,
		});
	});

	// Right after "Clear data".
	RegisterBoxScene(u"chat_stats_cleared"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return SampleBox(std::move(show), {
			.stage = Stage::Idle,
			.complete = false,
			.noMessages = true,
		});
	});

	// The end of the scrolled part: the facts, the note, "Clear data".
	RegisterScene({
		.name = u"chat_stats_bottom"_q,
		.size = size,
		.box = [](std::shared_ptr<Ui::Show> show) {
			return SampleBox(std::move(show), {});
		},
		.ready = [](not_null<QWidget*> widget) {
			static_cast<Ui::BoxContent*>(widget.get())->scrollToY(
				QWIDGETSIZE_MAX);
			return true;
		},
	});

	const auto full = QSize(Scaled(kBoxWidth), 0);
	RegisterScene(u"chat_stats_private_full"_q, full, [](
			not_null<Ui::RpWidget*> parent) {
		return SampleContent(parent, false, Period::Months3);
	});
	RegisterScene(u"chat_stats_group_full"_q, full, [](
			not_null<Ui::RpWidget*> parent) {
		return SampleContent(parent, true, Period::All);
	});
});

} // namespace

void ShowChatStats(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	const auto tracker = TrackerFor(peer);
	if (!tracker) {
		controller->showToast(tr::lng_oblivion_stats_unsupported(tr::now));
		return;
	}
	const auto target = tracker->peer();
	const auto session = &target->session();
	const auto weak = base::make_weak(tracker);
	const auto views = std::make_shared<
		std::map<uint64, Ui::PeerUserpicView>>();
	const auto source = std::make_shared<StickerSource>(StickerSource{
		.session = session,
		.peer = target,
	});
	auto person = [=](uint64 id, const QString &name) {
		auto result = PersonView();
		if (const auto loaded = session->data().peerLoaded(PeerId(id))) {
			result.self = loaded->isSelf();
			result.name = result.self
				? tr::lng_oblivion_stats_you(tr::now)
				: loaded->name();
			result.paintUserpic = [=](
					Painter &p,
					int x,
					int y,
					int outerWidth,
					int size) {
				loaded->paintUserpicLeft(
					p,
					(*views)[id],
					x,
					y,
					outerWidth,
					size);
			};
		} else {
			result.name = name.isEmpty()
				? tr::lng_oblivion_stats_unknown(tr::now)
				: name;
			result.paintUserpic = EmptyUserpicPainter(id, result.name);
		}
		return result;
	};
	auto box = Box(StatsBox, BoxArgs{
		.show = controller->uiShow(),
		.title = target->name(),
		.group = !target->isUser(),
		.zone = tracker->zone(),
		.status = tracker->status(),
		.reports = tracker->reports(),
		.repaint = rpl::merge(
			session->downloaderTaskFinished(),
			source->updated.events()),
		.person = std::move(person),
		.sticker = [=](const TopSticker &sticker) {
			return MakeStickerView(source, sticker);
		},
		.setPeriod = [=](Period period) {
			if (const auto strong = weak.get()) {
				strong->setPeriod(period);
			}
		},
		.refresh = [=] {
			if (const auto strong = weak.get()) {
				strong->refresh(true);
			}
		},
		.stop = [=] {
			if (const auto strong = weak.get()) {
				strong->stop();
			}
		},
		.clear = [=] {
			if (const auto strong = weak.get()) {
				strong->clear();
			}
		},
		.keep = tracker->view(),
	});
	controller->show(std::move(box));

	// What is not in the cache yet starts being read at once.
	if (const auto strong = weak.get()) {
		strong->refresh(false);
	}
}

void AddChatStatsAction(
		not_null<Window::SessionController*> controller,
		const Dialogs::Key &key,
		const Ui::Menu::MenuCallback &addAction) {
	if (key.topic() || key.sublist()) {
		return;
	}
	const auto peer = key.peer();
	if (!peer) {
		return;
	}
	const auto target = peer->migrateToOrMe();
	if (!ChatStats::Supported(target)) {
		return;
	}
	const auto weak = base::make_weak(controller);
	addAction(
		tr::lng_oblivion_stats_menu(tr::now),
		[=] {
			if (const auto strong = weak.get()) {
				ShowChatStats(strong, target);
			}
		},
		&st::menuIconStats);
}

} // namespace Oblivion
