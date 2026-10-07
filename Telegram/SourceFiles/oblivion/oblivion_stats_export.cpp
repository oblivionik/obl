/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_stats_export.h"

#include "base/unixtime.h"
#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "lang/lang_tag.h"
#include "oblivion/oblivion_chat_stats.h"
#include "oblivion/oblivion_lang.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/text/format_values.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QDate>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
#include <QtCore/QSaveFile>
#include <QtCore/QXmlStreamReader>

namespace Oblivion::StatsExport {
namespace {

using namespace ChatStats;

constexpr auto kMaxPeople = 12;
constexpr auto kMaxWords = 24;
constexpr auto kMaxEmoji = 18;
constexpr auto kCalendarWeeks = 53;
constexpr auto kCalendarMinDays = 14;
constexpr auto kMaxNameLength = 64;
constexpr auto kMaxFileNameLength = 60;

// What the page shows, already in words. The page is drawn from this
// alone, see RenderPage().
struct Tile {
	QString label;
	QString value;
};

struct Column {
	QString label; // Under the column, may be empty.
	QString name; // In the tooltip and in the table.
	int64 value = 0;
};

struct Row {
	QString name;
	QString value;
	QString note;
	double part = 0.; // Of the longest bar, 0..1.
};

struct Chip {
	QString text;
	QString count;
};

struct Fact {
	QString label;
	QString value;
};

struct HeatCell {
	int level = -1; // -1: no such day in the picture.
	QString tip;
};

struct Heat {
	int weeks = 0;
	std::vector<HeatCell> cells; // By weeks, seven in each, from Monday.
	std::vector<std::pair<int, QString>> months; // The week and the name.
};

struct PageData {
	QString chat;
	QString period;
	bool incomplete = false;
	std::vector<Tile> tiles;
	QString peopleTitle;
	std::vector<Row> people;
	QString peopleMore;
	std::vector<Column> months;
	Heat heat;
	std::vector<Column> hours;
	std::vector<Column> weekdays;
	std::vector<Row> kinds;
	std::vector<Chip> words;
	std::vector<Chip> emoji;
	std::vector<Fact> facts;
};

[[nodiscard]] QString Num(double value) {
	auto result = QString::number(value, 'f', 1);
	if (result.endsWith(u".0"_q)) {
		result.chop(2);
	}
	return result;
}

[[nodiscard]] QString Count(const Labels &labels, int64 value) {
	return labels.number ? labels.number(value) : QString::number(value);
}

[[nodiscard]] QString Fraction(const Labels &labels, double value) {
	if (value >= 100.) {
		return Count(labels, int64(std::llround(value)));
	}
	auto result = QString::number(value, 'f', 1);
	if (result.endsWith(u".0"_q)) {
		result.chop(2);
	}
	return result.replace(QChar('.'), labels.decimal);
}

[[nodiscard]] QString Percent(int64 part, int64 whole) {
	if (whole <= 0 || part <= 0) {
		return QString();
	}
	const auto value = part * 100. / whole;
	return (value < 1.)
		? u"<1%"_q
		: (QString::number(int(std::lround(std::min(value, 100.)))) + '%');
}

[[nodiscard]] QString WithPercent(
		const Labels &labels,
		int64 part,
		int64 whole) {
	const auto percent = Percent(part, whole);
	return percent.isEmpty()
		? Count(labels, part)
		: (Count(labels, part) + u" · "_q + percent);
}

// The top of the scale: a round number that has a round half.
[[nodiscard]] int64 NiceMax(int64 value) {
	value = std::max(value, int64(1));
	auto magnitude = int64(1);
	while (magnitude * 10 <= value) {
		magnitude *= 10;
	}
	for (const auto factor : { 1, 2, 4, 5, 6, 8, 10 }) {
		const auto result = factor * magnitude;
		if (result >= value) {
			return (result % 2) ? (result + 1) : result;
		}
	}
	return magnitude * 10;
}

[[nodiscard]] QString Shorten(QString text, int limit) {
	text = text.simplified();
	if (text.size() <= limit) {
		return text;
	}
	auto length = std::max(limit - 1, 1);
	if (text[length - 1].isHighSurrogate()) {
		--length;
	}
	return text.left(length).trimmed() + QChar(0x2026);
}

[[nodiscard]] QString DateText(const Labels &labels, int32 day) {
	if (labels.date) {
		return labels.date(day);
	}
	const auto date = DateOfDay(day);
	return u"%1-%2-%3"_q
		.arg(date.year)
		.arg(date.month, 2, 10, QChar('0'))
		.arg(date.day, 2, 10, QChar('0'));
}

[[nodiscard]] QString MessagesText(const Labels &labels, int count) {
	return labels.messages
		? labels.messages(count)
		: QString::number(count);
}

[[nodiscard]] QString MonthName(const Labels &labels, int year, int month) {
	const auto index = std::clamp(month, 1, 12) - 1;
	return labels.monthsLong[index] + ' ' + QString::number(year);
}

[[nodiscard]] Heat MakeHeat(const Report &report, const Labels &labels) {
	auto result = Heat();
	const auto first = report.firstDay;
	const auto last = report.lastDay;
	const auto size = int(report.days.size());
	if (size < kCalendarMinDays || (last - first + 1) != size) {
		return result;
	}
	const auto start = last
		- WeekdayOfDay(last)
		- (kCalendarWeeks - 1) * 7;
	const auto countOf = [&](int32 day) {
		return (day >= first && day <= last)
			? report.days[day - first]
			: 0;
	};
	auto sorted = std::vector<int>();
	for (auto day = std::max(start, first); day <= last; ++day) {
		if (const auto count = countOf(day); count > 0) {
			sorted.push_back(count);
		}
	}
	if (sorted.empty()) {
		return result;
	}
	std::sort(begin(sorted), end(sorted));
	const auto quantile = [&](double part) {
		const auto index = std::min(
			int(sorted.size()) - 1,
			int(part * sorted.size()));
		return sorted[index];
	};
	const auto q1 = quantile(0.25);
	const auto q2 = quantile(0.5);
	const auto q3 = quantile(0.75);

	result.weeks = kCalendarWeeks;
	result.cells.resize(kCalendarWeeks * 7);
	auto month = 0;
	for (auto week = 0; week != kCalendarWeeks; ++week) {
		const auto monday = DateOfDay(start + week * 7);
		const auto sunday = DateOfDay(start + week * 7 + 6);
		if (week > 0 && monday.month != month && monday.day <= 7) {
			result.months.emplace_back(
				week,
				labels.monthsShort[monday.month - 1]);
		} else if (!week && sunday.month == monday.month) {
			result.months.emplace_back(
				week,
				labels.monthsShort[monday.month - 1]);
		}
		month = monday.month;
	}
	for (auto day = start; day <= last; ++day) {
		const auto count = countOf(day);
		auto &cell = result.cells[day - start];
		cell.level = (count <= 0)
			? 0
			: (count > q3)
			? 4
			: (count > q2)
			? 3
			: (count > q1)
			? 2
			: 1;
		cell.tip = DateText(labels, day)
			+ u": "_q
			+ MessagesText(labels, count);
	}
	return result;
}

[[nodiscard]] PageData MakePageData(
		const Input &input,
		const Labels &labels) {
	const auto &report = *input.report;
	const auto personOf = [&](uint64 id, const QString &name) {
		auto result = input.person
			? input.person(id, name)
			: Person{ .name = name };
		result.name = Shorten(result.name, kMaxNameLength);
		if (result.name.isEmpty()) {
			result.name = labels.unknown;
		}
		return result;
	};

	auto result = PageData();
	result.chat = Shorten(input.chat, 2 * kMaxNameLength);
	result.incomplete = !report.complete;
	if (report.messages > 0) {
		result.period = (report.firstDay == report.lastDay)
			? DateText(labels, report.firstDay)
			: (DateText(labels, report.firstDay)
				+ u" — "_q
				+ DateText(labels, report.lastDay));
	}

	const auto tile = [&](const QString &label, const QString &value) {
		result.tiles.push_back({ label, value });
	};
	tile(labels.cellMessages, Count(labels, report.messages));
	tile(labels.cellWords, Count(labels, report.words));
	if (report.group) {
		tile(labels.cellPeople, Count(labels, int(report.persons.size())));
	} else {
		tile(labels.cellEmoji, Count(labels, report.emoji));
	}
	tile(labels.cellDays, Count(labels, report.activeDays));
	if (report.activeDays > 0) {
		tile(
			labels.cellPerDay,
			Fraction(labels, report.messages / double(report.activeDays)));
	}
	if (report.textMessages > 0) {
		tile(
			labels.cellLength,
			Fraction(labels, report.words / double(report.textMessages)));
	}

	result.peopleTitle = report.group
		? labels.sectionPeople
		: labels.sectionPeoplePrivate;
	const auto peopleCount = int(report.persons.size());
	auto peopleMax = 0;
	for (const auto &person : report.persons) {
		peopleMax = std::max(peopleMax, person.messages);
	}
	for (auto i = 0; i != std::min(peopleCount, kMaxPeople); ++i) {
		const auto &person = report.persons[i];
		const auto shown = personOf(person.id, person.name);
		auto notes = QStringList();
		if (person.words > 0 && labels.words) {
			notes.push_back(labels.words(person.words));
		}
		if (!report.group
			&& person.answers > 0
			&& person.answerTime > 0
			&& labels.answer) {
			notes.push_back(labels.answer(shown.self, person.answerTime));
		}
		result.people.push_back({
			.name = shown.name,
			.value = WithPercent(labels, person.messages, report.messages),
			.note = notes.join(u" · "_q),
			.part = peopleMax ? (person.messages / double(peopleMax)) : 0.,
		});
	}
	if (peopleCount > kMaxPeople && labels.peopleMore) {
		result.peopleMore = labels.peopleMore(peopleCount - kMaxPeople);
	}

	const auto monthsCount = int(report.months.size());
	const auto monthStep = (monthsCount <= 12)
		? 1
		: (monthsCount <= 30)
		? 3
		: 12;
	for (const auto &month : report.months) {
		const auto index = std::clamp(month.month, 1, 12) - 1;
		const auto labeled = !(index % monthStep);
		result.months.push_back({
			.label = !labeled
				? QString()
				: (!index && monthsCount > 12)
				? QString::number(month.year)
				: labels.monthsShort[index],
			.name = MonthName(labels, month.year, month.month),
			.value = month.count,
		});
	}

	result.heat = MakeHeat(report, labels);

	for (auto hour = 0; hour != 24; ++hour) {
		const auto two = [](int value) {
			return u"%1"_q.arg(value, 2, 10, QChar('0'));
		};
		result.hours.push_back({
			.label = (hour % 3) ? QString() : two(hour),
			.name = two(hour) + u":00–"_q + two((hour + 1) % 24) + u":00"_q,
			.value = report.hours[hour],
		});
	}
	for (auto weekday = 0; weekday != 7; ++weekday) {
		result.weekdays.push_back({
			.label = labels.weekdaysShort[weekday],
			.name = labels.weekdaysLong[weekday],
			.value = report.weekdays[weekday],
		});
	}

	auto kinds = std::vector<std::pair<int, int>>();
	for (auto kind = 0; kind != kKindCount; ++kind) {
		if (report.kinds[kind] > 0) {
			kinds.emplace_back(report.kinds[kind], kind);
		}
	}
	std::sort(begin(kinds), end(kinds), [](const auto &a, const auto &b) {
		return (a.first > b.first)
			|| (a.first == b.first && a.second < b.second);
	});
	for (const auto &[count, kind] : kinds) {
		result.kinds.push_back({
			.name = labels.kinds[kind],
			.value = WithPercent(labels, count, report.messages),
			.part = count / double(kinds.front().first),
		});
	}

	for (const auto &word : report.topWords) {
		if (int(result.words.size()) >= kMaxWords) {
			break;
		}
		result.words.push_back({
			Shorten(word.text, kMaxNameLength),
			Count(labels, word.count),
		});
	}
	for (const auto &emoji : report.topEmoji) {
		if (int(result.emoji.size()) >= kMaxEmoji) {
			break;
		}
		result.emoji.push_back({
			Shorten(emoji.text, 16),
			Count(labels, emoji.count),
		});
	}

	const auto fact = [&](const QString &label, const QString &value) {
		if (!value.isEmpty()) {
			result.facts.push_back({ label, value });
		}
	};
	fact(labels.factPeriod, result.period);
	if (report.firstDate > 0 && input.zone) {
		const auto local = int64(report.firstDate)
			+ input.zone->offset(report.firstDate);
		const auto seconds = int(((local % 86400) + 86400) % 86400);
		auto value = DateText(labels, input.zone->day(report.firstDate))
			+ u", %1:%2"_q
				.arg(seconds / 3600, 2, 10, QChar('0'))
				.arg((seconds % 3600) / 60, 2, 10, QChar('0'));
		if (report.firstSender) {
			auto name = QString();
			for (const auto &person : report.persons) {
				if (person.id == report.firstSender) {
					name = person.name;
					break;
				}
			}
			value += u" · "_q + personOf(report.firstSender, name).name;
		}
		fact(labels.factFirst, value);
	}
	const auto streak = [&](const Streak &value) {
		return (value.days > 0 && labels.days)
			? (labels.days(value.days)
				+ u" · "_q
				+ DateText(labels, value.fromDay)
				+ ((value.days > 1)
					? (u" — "_q
						+ DateText(labels, value.fromDay + value.days - 1))
					: QString()))
			: QString();
	};
	fact(labels.factStreak, streak(report.longest));
	fact(labels.factStreakNow, streak(report.current));
	auto topDays = QStringList();
	for (const auto &day : report.topDays) {
		topDays.push_back(DateText(labels, day.day)
			+ u" · "_q
			+ Count(labels, day.count));
	}
	fact(labels.factTopDays, topDays.join(u"\n"_q));
	const auto share = [&](int count) {
		return (count > 0)
			? WithPercent(labels, count, report.messages)
			: QString();
	};
	fact(labels.factReplies, share(report.replies));
	fact(labels.factForwards, share(report.forwards));
	fact(labels.factLinks, share(report.links));
	return result;
}

// Page start. Only QtCore from here to "Page end".

constexpr auto kStyle = R"css(
:root{color-scheme:light dark;--page:#f6f6f4;--card:#fcfcfb;--ink:#0b0b0b;--ink2:#52514e;--muted:#898781;--grid:#e1e0d9;--axis:#c3c2b7;--border:rgba(11,11,11,.10);--accent:#2a78d6;--wash:rgba(42,120,214,.12);--h0:#ecebe6;--h1:#b7d3f6;--h2:#6da7ec;--h3:#2a78d6;--h4:#104281;--warn:#fab219}
@media (prefers-color-scheme:dark){:root{--page:#0d0d0d;--card:#1a1a19;--ink:#ffffff;--ink2:#c3c2b7;--muted:#898781;--grid:#2c2c2a;--axis:#383835;--border:rgba(255,255,255,.10);--accent:#3987e5;--wash:rgba(57,135,229,.16);--h0:#262624;--h1:#104281;--h2:#1c5cab;--h3:#3987e5;--h4:#9ec5f4}}
*{box-sizing:border-box}
html{-webkit-text-size-adjust:100%}
body{margin:0;background:var(--page);color:var(--ink);font:15px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
.wrap{max-width:980px;margin:0 auto;padding:36px 20px 48px}
header{margin:0 4px 24px}
.over{font-size:13px;letter-spacing:.06em;text-transform:uppercase;color:var(--ink2);font-weight:600}
h1{font-size:34px;line-height:1.15;margin:6px 0 8px;font-weight:700;overflow-wrap:anywhere}
.sub{color:var(--ink2)}
.warn{display:flex;gap:10px;margin:16px 0 0;padding:10px 14px;border-radius:12px;background:var(--card);border:1px solid var(--border);border-left:3px solid var(--warn)}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:12px;margin:0 0 12px}
.tile,.card{background:var(--card);border:1px solid var(--border);border-radius:16px}
.tile{padding:16px 18px}
.tile .l{color:var(--ink2);font-size:13px}
.tile .v{font-size:28px;font-weight:600;line-height:1.2;margin-top:2px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:12px}
.card{padding:18px 20px 20px;min-width:0}
.card.wide{grid-column:1/-1}
h2{font-size:17px;margin:0 0 14px;font-weight:600}
.note{color:var(--muted);font-size:13px;margin:-8px 0 12px}
.scroll{overflow-x:auto}
svg{display:block;width:100%;height:auto}
svg.wide{min-width:560px}
svg.half{min-width:300px}
.bar{fill:var(--accent)}
.hit{fill:transparent}
.col:hover .bar{opacity:.72}
.gridl{stroke:var(--grid);stroke-width:1}
.axis{stroke:var(--axis);stroke-width:1}
.tick{fill:var(--muted);font-size:11px}
.cap{fill:var(--ink2);font-size:11px;font-weight:600}
.h0{fill:var(--h0)}.h1{fill:var(--h1)}.h2{fill:var(--h2)}.h3{fill:var(--h3)}.h4{fill:var(--h4)}
.legend{display:flex;gap:5px;align-items:center;color:var(--muted);font-size:12px;margin-top:10px}
.legend i{width:11px;height:11px;border-radius:3px;display:inline-block}
.legend .h0{background:var(--h0)}.legend .h1{background:var(--h1)}.legend .h2{background:var(--h2)}.legend .h3{background:var(--h3)}.legend .h4{background:var(--h4)}
.rows{list-style:none;margin:0;padding:0;display:grid;gap:12px}
.row{display:grid;grid-template-columns:minmax(0,1fr) auto;gap:3px 12px;align-items:baseline}
.row .n{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.row .v{color:var(--ink2);font-variant-numeric:tabular-nums;white-space:nowrap}
.row .t{grid-column:1/-1;height:8px;border-radius:4px;background:var(--wash);overflow:hidden}
.row .f{display:block;height:100%;min-width:2px;background:var(--accent);border-radius:0 4px 4px 0}
.row .s{grid-column:1/-1;color:var(--muted);font-size:13px}
.more{color:var(--muted);font-size:13px;margin:12px 0 0}
.chips{list-style:none;margin:0;padding:0;display:flex;flex-wrap:wrap;gap:8px}
.chips li{display:flex;gap:8px;align-items:baseline;padding:6px 12px;border-radius:999px;background:var(--wash)}
.chips .c{color:var(--ink2);font-size:13px;font-variant-numeric:tabular-nums}
.chips.em li{font-size:20px;align-items:center}
.facts{margin:0;display:grid;grid-template-columns:minmax(0,auto) minmax(0,1fr);gap:10px 16px}
.facts dt{color:var(--ink2)}
.facts dd{margin:0;text-align:right;font-weight:600;white-space:pre-line;overflow-wrap:anywhere}
details{margin-top:12px}
summary{cursor:pointer;color:var(--ink2);font-size:13px}
table{border-collapse:collapse;margin-top:8px;font-size:13px;font-variant-numeric:tabular-nums}
th,td{padding:4px 16px 4px 0;text-align:left;border-bottom:1px solid var(--grid);font-weight:400}
th{color:var(--muted)}
.r{text-align:right}
footer{margin:28px 4px 0;color:var(--muted);font-size:13px}
@media print{body{background:#ffffff}.card,.tile{break-inside:avoid}}
)css";

[[nodiscard]] QString ColumnChart(
		const std::vector<Column> &columns,
		const Labels &labels,
		const QString &title,
		bool wide) {
	const auto count = int(columns.size());
	if (!count) {
		return QString();
	}
	const auto width = wide ? 900. : 440.;
	const auto height = wide ? 230. : 210.;
	const auto left = 46.;
	const auto right = 8.;
	const auto top = 22.;
	const auto bottom = 26.;
	const auto plotWidth = width - left - right;
	const auto plotHeight = height - top - bottom;
	auto max = int64(0);
	auto maxIndex = -1;
	for (auto i = 0; i != count; ++i) {
		if (columns[i].value > max) {
			max = columns[i].value;
			maxIndex = i;
		}
	}
	const auto scale = NiceMax(max);
	const auto slot = plotWidth / count;
	const auto barWidth = std::clamp(slot - 2., 1., 24.);
	const auto base = top + plotHeight;

	auto out = QString();
	out += u"<div class=\"scroll\"><svg class=\"%1\" viewBox=\"0 0 %2 %3\""
		u" role=\"img\" aria-label=\"%4\">\n"_q.arg(
			wide ? u"wide"_q : u"half"_q,
			Num(width),
			Num(height),
			Escape(title));
	for (auto k = 0; k != 3; ++k) {
		const auto y = base - plotHeight * k / 2.;
		out += u"<line class=\"%1\" x1=\"%2\" y1=\"%3\" x2=\"%4\" y2=\"%3\"/>"
			u"<text class=\"tick\" x=\"%5\" y=\"%6\" text-anchor=\"end\">%7"
			u"</text>\n"_q.arg(
				k ? u"gridl"_q : u"axis"_q,
				Num(left),
				Num(y),
				Num(width - right),
				Num(left - 8),
				Num(y + 4),
				Escape(Count(labels, scale * k / 2)));
	}
	for (auto i = 0; i != count; ++i) {
		const auto &column = columns[i];
		const auto x0 = left + i * slot;
		const auto x = x0 + (slot - barWidth) / 2.;
		const auto barHeight = (column.value > 0)
			? std::max(column.value * plotHeight / scale, 1.5)
			: 0.;
		const auto y = base - barHeight;
		out += u"<g class=\"col\"><title>%1: %2</title>"
			u"<rect class=\"hit\" x=\"%3\" y=\"%4\" width=\"%5\""
			u" height=\"%6\"/>"_q.arg(
				Escape(column.name),
				Escape(MessagesText(labels, int(column.value))),
				Num(x0),
				Num(top),
				Num(slot),
				Num(plotHeight));
		if (barHeight > 0.) {
			// The data end is rounded, the baseline end is square.
			const auto radius = std::min({ 4., barWidth / 2., barHeight });
			out += u"<path class=\"bar\" d=\"M%1 %2V%3Q%1 %4 %5 %4H%6"
				u"Q%7 %4 %7 %3V%2Z\"/>"_q.arg(
					Num(x),
					Num(base),
					Num(y + radius),
					Num(y),
					Num(x + radius),
					Num(x + barWidth - radius),
					Num(x + barWidth));
		}
		out += u"</g>\n"_q;
		if (!column.label.isEmpty()) {
			out += u"<text class=\"tick\" x=\"%1\" y=\"%2\""
				u" text-anchor=\"middle\">%3</text>\n"_q.arg(
					Num(x0 + slot / 2.),
					Num(height - 8),
					Escape(column.label));
		}
	}
	if (maxIndex >= 0 && max > 0) {
		// Only the highest column is labelled, the rest is in the
		// tooltips and in the table.
		const auto center = left + maxIndex * slot + slot / 2.;
		const auto y = base - std::max(max * plotHeight / scale, 1.5);
		out += u"<text class=\"cap\" x=\"%1\" y=\"%2\""
			u" text-anchor=\"middle\">%3</text>\n"_q.arg(
				Num(std::clamp(center, left + 16., width - right - 16.)),
				Num(y - 6),
				Escape(Count(labels, max)));
	}
	out += u"</svg></div>\n"_q;
	return out;
}

[[nodiscard]] QString ColumnTable(
		const std::vector<Column> &columns,
		const Labels &labels,
		const QString &head) {
	auto out = u"<details><summary>%1</summary><table><thead><tr>"
		u"<th>%2</th><th class=\"r\">%3</th></tr></thead><tbody>\n"_q.arg(
			Escape(labels.table),
			Escape(head),
			Escape(labels.cellMessages));
	for (const auto &column : columns) {
		out += u"<tr><td>%1</td><td class=\"r\">%2</td></tr>\n"_q.arg(
			Escape(column.name),
			Escape(Count(labels, column.value)));
	}
	out += u"</tbody></table></details>\n"_q;
	return out;
}

[[nodiscard]] QString HeatChart(
		const Heat &heat,
		const Labels &labels,
		const QString &title) {
	const auto step = 14.;
	const auto cell = 11.;
	const auto left = 30.;
	const auto top = 18.;
	const auto width = left + heat.weeks * step;
	const auto height = top + 7 * step;
	auto out = u"<div class=\"scroll\"><svg class=\"wide\""
		u" viewBox=\"0 0 %1 %2\" role=\"img\" aria-label=\"%3\">\n"_q.arg(
			Num(width),
			Num(height),
			Escape(title));
	for (const auto &[week, name] : heat.months) {
		out += u"<text class=\"tick\" x=\"%1\" y=\"11\">%2</text>\n"_q.arg(
			Num(left + week * step),
			Escape(name));
	}
	for (const auto weekday : { 0, 2, 4 }) {
		out += u"<text class=\"tick\" x=\"0\" y=\"%1\">%2</text>\n"_q.arg(
			Num(top + weekday * step + 9.5),
			Escape(labels.weekdaysShort[weekday]));
	}
	for (auto i = 0, count = int(heat.cells.size()); i != count; ++i) {
		const auto &value = heat.cells[i];
		if (value.level < 0) {
			continue;
		}
		out += u"<rect class=\"h%1\" x=\"%2\" y=\"%3\" width=\"%4\""
			u" height=\"%4\" rx=\"2.5\"><title>%5</title></rect>\n"_q.arg(
				QString::number(std::min(value.level, 4)),
				Num(left + (i / 7) * step),
				Num(top + (i % 7) * step),
				Num(cell),
				Escape(value.tip));
	}
	out += u"</svg></div>\n<div class=\"legend\"><span>%1</span>"
		u"<i class=\"h0\"></i><i class=\"h1\"></i><i class=\"h2\"></i>"
		u"<i class=\"h3\"></i><i class=\"h4\"></i><span>%2</span></div>\n"_q
		.arg(Escape(labels.less), Escape(labels.more));
	return out;
}

[[nodiscard]] QString RowsList(const std::vector<Row> &rows) {
	auto out = u"<ul class=\"rows\">\n"_q;
	for (const auto &row : rows) {
		out += u"<li class=\"row\"><span class=\"n\"><bdi>%1</bdi></span>"
			u"<span class=\"v\">%2</span><span class=\"t\">"
			u"<span class=\"f\" style=\"width:%3%\"></span></span>"_q.arg(
				Escape(row.name),
				Escape(row.value),
				Num(std::clamp(row.part, 0., 1.) * 100.));
		if (!row.note.isEmpty()) {
			out += u"<span class=\"s\">%1</span>"_q.arg(Escape(row.note));
		}
		out += u"</li>\n"_q;
	}
	out += u"</ul>\n"_q;
	return out;
}

[[nodiscard]] QString ChipsList(const std::vector<Chip> &chips, bool emoji) {
	auto out = emoji
		? u"<ul class=\"chips em\">\n"_q
		: u"<ul class=\"chips\">\n"_q;
	for (const auto &chip : chips) {
		out += u"<li><bdi>%1</bdi><span class=\"c\">%2</span></li>\n"_q.arg(
			Escape(chip.text),
			Escape(chip.count));
	}
	out += u"</ul>\n"_q;
	return out;
}

// A value of several lines: every line is escaped by itself.
[[nodiscard]] QString EscapeLines(const QString &text) {
	auto lines = QStringList();
	for (const auto &line : text.split(QChar('\n'))) {
		lines.push_back(Escape(line));
	}
	return lines.join(u"<br/>"_q);
}

[[nodiscard]] QString Card(
		const QString &title,
		const QString &body,
		bool wide,
		const QString &note = QString()) {
	return u"<section class=\"%1\"><h2>%2</h2>\n%3%4</section>\n"_q.arg(
		wide ? u"card wide"_q : u"card"_q,
		Escape(title),
		note.isEmpty()
			? QString()
			: u"<p class=\"note\">%1</p>\n"_q.arg(Escape(note)),
		body);
}

[[nodiscard]] bool HasValues(const std::vector<Column> &columns) {
	for (const auto &column : columns) {
		if (column.value > 0) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] QString RenderPage(const PageData &page, const Labels &labels) {
	auto out = QString();
	out.reserve(96 * 1024);
	const auto title = page.chat.isEmpty()
		? labels.pageTitle
		: (labels.pageTitle + u" — "_q + page.chat);
	out += u"<!DOCTYPE html>\n<html lang=\"%1\">\n<head>\n"
		u"<meta charset=\"utf-8\"/>\n"
		u"<meta name=\"viewport\""
		u" content=\"width=device-width, initial-scale=1\"/>\n"
		u"<meta http-equiv=\"Content-Security-Policy\""
		u" content=\"default-src 'none'; style-src 'unsafe-inline'\"/>\n"
		u"<meta name=\"referrer\" content=\"no-referrer\"/>\n"
		u"<meta name=\"color-scheme\" content=\"light dark\"/>\n"
		u"<meta name=\"generator\" content=\"Oblivion\"/>\n"
		u"<title>%2</title>\n<style>"_q.arg(
			Escape(labels.lang.isEmpty() ? u"en"_q : labels.lang),
			Escape(title));
	out += QString::fromLatin1(kStyle);
	out += u"</style>\n</head>\n<body>\n<div class=\"wrap\">\n"_q;

	out += u"<header>\n<div class=\"over\">%1</div>\n"
		u"<h1><bdi>%2</bdi></h1>\n"_q.arg(
			Escape(labels.pageTitle),
			Escape(page.chat.isEmpty() ? labels.pageTitle : page.chat));
	if (!page.period.isEmpty()) {
		out += u"<div class=\"sub\">%1</div>\n"_q.arg(Escape(page.period));
	}
	if (page.incomplete) {
		out += u"<div class=\"warn\"><span>⚠</span><span>%1</span></div>\n"_q
			.arg(Escape(labels.incomplete));
	}
	out += u"</header>\n"_q;

	out += u"<section class=\"tiles\">\n"_q;
	for (const auto &tile : page.tiles) {
		out += u"<div class=\"tile\"><div class=\"l\">%1</div>"
			u"<div class=\"v\">%2</div></div>\n"_q.arg(
				Escape(tile.label),
				Escape(tile.value));
	}
	out += u"</section>\n<div class=\"grid\">\n"_q;

	if (page.months.size() > 1 && HasValues(page.months)) {
		out += Card(
			labels.sectionMonths,
			ColumnChart(page.months, labels, labels.sectionMonths, true)
				+ ColumnTable(page.months, labels, labels.colMonth),
			true);
	}
	if (page.heat.weeks > 0) {
		out += Card(
			labels.sectionCalendar,
			HeatChart(page.heat, labels, labels.sectionCalendar),
			true,
			labels.calendarNote);
	}
	if (!page.people.empty()) {
		auto body = RowsList(page.people);
		if (!page.peopleMore.isEmpty()) {
			body += u"<p class=\"more\">%1</p>\n"_q.arg(
				Escape(page.peopleMore));
		}
		out += Card(page.peopleTitle, body, false);
	}
	if (!page.kinds.empty()) {
		out += Card(labels.sectionKinds, RowsList(page.kinds), false);
	}
	if (HasValues(page.hours)) {
		out += Card(
			labels.sectionHours,
			ColumnChart(page.hours, labels, labels.sectionHours, false)
				+ ColumnTable(page.hours, labels, labels.colHour),
			false);
	}
	if (HasValues(page.weekdays)) {
		out += Card(
			labels.sectionWeekdays,
			ColumnChart(page.weekdays, labels, labels.sectionWeekdays, false)
				+ ColumnTable(page.weekdays, labels, labels.colDay),
			false);
	}
	if (!page.words.empty()) {
		out += Card(labels.sectionWords, ChipsList(page.words, false), true);
	}
	if (!page.emoji.empty()) {
		out += Card(labels.sectionEmoji, ChipsList(page.emoji, true), false);
	}
	if (!page.facts.empty()) {
		auto body = u"<dl class=\"facts\">\n"_q;
		for (const auto &fact : page.facts) {
			body += u"<dt>%1</dt><dd>%2</dd>\n"_q.arg(
				Escape(fact.label),
				EscapeLines(fact.value));
		}
		body += u"</dl>\n"_q;
		out += Card(labels.sectionFacts, body, false);
	}

	out += u"</div>\n<footer>%1<br/>%2</footer>\n</div>\n</body>\n</html>\n"_q
		.arg(Escape(labels.made), Escape(labels.privacy));
	return out;
}

// Page end.

[[nodiscard]] QString FileNamePart(QString text) {
	static const auto kBad = u"\\/:*?\"<>|"_q;
	auto result = QString();
	for (const auto ch : text.simplified()) {
		if (ch.unicode() < 0x20 || kBad.contains(ch)) {
			result.append(QChar('_'));
		} else {
			result.append(ch);
		}
	}
	while (result.endsWith(QChar('.')) || result.endsWith(QChar(' '))) {
		result.chop(1);
	}
	if (result.size() > kMaxFileNameLength) {
		auto length = kMaxFileNameLength;
		if (result[length - 1].isHighSurrogate()) {
			--length;
		}
		result = result.left(length).trimmed();
	}
	return result;
}

struct DoneArgs {
	QString path;
	int64 size = 0;
	Fn<void()> open;
	Fn<void()> folder;
};

void DoneBox(not_null<Ui::GenericBox*> box, DoneArgs &&args) {
	box->setTitle(tr::lng_oblivion_statsexp_done_title());
	const auto info = QFileInfo(args.path);
	auto name = tr::bold(info.fileName());
	if (args.size > 0) {
		name.append(u" · "_q + Ui::FormatSizeText(args.size));
	}
	const auto nameLabel = box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		rpl::single(name),
		st::boxLabel));
	nameLabel->setBreakEverywhere(true);
	const auto folderLabel = box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			QDir::toNativeSeparators(info.absolutePath()),
			st::boxDividerLabel),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
	folderLabel->setBreakEverywhere(true);
	folderLabel->setSelectable(true);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_oblivion_statsexp_done_about(),
			st::boxLabel),
		st::boxRowPadding + QMargins(0, st::boxMediumSkip, 0, 0));

	const auto open = args.open;
	const auto folder = args.folder;
	box->addButton(tr::lng_oblivion_statsexp_done_open(), [=] {
		if (open) {
			open();
		}
		box->closeBox();
	});
	box->addButton(tr::lng_oblivion_statsexp_done_folder(), [=] {
		if (folder) {
			folder();
		}
	});
	box->addLeftButton(tr::lng_close(), [=] { box->closeBox(); });
}

void SavePage(
		std::shared_ptr<Ui::Show> show,
		const Input &input,
		QString path) {
	if (!path.endsWith(u".html"_q, Qt::CaseInsensitive)
		&& !path.endsWith(u".htm"_q, Qt::CaseInsensitive)) {
		path += u".html"_q;
	}
	// The names are taken from the session, so the page is made here,
	// only the file is written on another thread.
	auto bytes = Generate(input, LangLabels(base::unixtime::now()));
	if (bytes.isEmpty()) {
		show->showToast(tr::lng_oblivion_statsexp_failed(tr::now));
		return;
	}
	crl::async([=, show = show, bytes = std::move(bytes)]() mutable {
		auto file = QSaveFile(path);
		const auto saved = file.open(QIODevice::WriteOnly)
			&& (file.write(bytes) == bytes.size())
			&& file.commit();
		const auto size = int64(bytes.size());

		// The show goes back to the main thread and is let go there.
		crl::on_main([=, show = std::move(show)] {
			if (!show->valid()) {
				return;
			} else if (!saved) {
				show->showToast(tr::lng_oblivion_statsexp_failed(tr::now));
				return;
			}
			show->showBox(Box(DoneBox, DoneArgs{
				.path = path,
				.size = size,
				.open = [=] { File::Launch(path); },
				.folder = [=] { File::ShowInFolder(path); },
			}));
		});
	});
}

} // namespace

QString Escape(const QString &text) {
	auto result = QString();
	result.reserve(text.size() + 16);
	for (auto i = 0, count = int(text.size()); i != count; ++i) {
		const auto ch = text[i];
		const auto code = ch.unicode();
		if (ch.isHighSurrogate()) {
			if (i + 1 < count && text[i + 1].isLowSurrogate()) {
				result.append(ch).append(text[i + 1]);
				++i;
			}
			continue;
		} else if (ch.isLowSurrogate()) {
			continue;
		} else if (code < 0x20) {
			if (code == '\n' || code == '\t') {
				result.append(QChar(' '));
			}
			continue;
		} else if ((code >= 0x7F && code < 0xA0)
			|| (code >= 0x202A && code <= 0x202E)
			|| (code >= 0x2066 && code <= 0x2069)
			|| (code == 0x200E)
			|| (code == 0x200F)
			|| (code == 0x2028)
			|| (code == 0x2029)
			|| (code == 0xFEFF)
			|| (code >= 0xFFFE)) {
			continue;
		}
		switch (code) {
		case '&': result.append(u"&amp;"_q); break;
		case '<': result.append(u"&lt;"_q); break;
		case '>': result.append(u"&gt;"_q); break;
		case '"': result.append(u"&quot;"_q); break;
		case '\'': result.append(u"&#39;"_q); break;
		default: result.append(ch); break;
		}
	}
	return result;
}

QByteArray Generate(const Input &input, const Labels &labels) {
	if (!input.report) {
		return QByteArray();
	}
	return RenderPage(MakePageData(input, labels), labels).toUtf8();
}

Labels LangLabels(TimeId now) {
	const auto russian = CurrentLanguageIsRussian();
	const auto locale = QLocale(russian
		? QLocale::Russian
		: QLocale::English);
	const auto capitalized = [](QString text) {
		if (!text.isEmpty()) {
			text[0] = text[0].toUpper();
		}
		return text;
	};
	const auto withoutDot = [](QString text) {
		if (text.endsWith(QChar('.'))) {
			text.chop(1);
		}
		return text;
	};
	const auto dateOf = [=](int32 day) {
		const auto date = DateOfDay(day);
		return locale.toString(
			QDate(date.year, date.month, date.day),
			u"d MMMM yyyy"_q);
	};
	const auto span = [](TimeId seconds) {
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
	};

	auto result = Labels();
	result.lang = russian ? u"ru"_q : u"en"_q;
	result.decimal = QLocale().decimalPoint().isEmpty()
		? QChar('.')
		: QLocale().decimalPoint().at(0);
	result.pageTitle = tr::lng_oblivion_statsexp_page_title(tr::now);
	result.made = tr::lng_oblivion_statsexp_page_made(
		tr::now,
		lt_date,
		locale.toString(
			base::unixtime::parse(now).date(),
			u"d MMMM yyyy"_q));
	result.privacy = tr::lng_oblivion_statsexp_page_privacy(tr::now);
	result.incomplete = tr::lng_oblivion_statsexp_page_incomplete(tr::now);
	result.table = tr::lng_oblivion_statsexp_page_table(tr::now);
	result.colMonth = tr::lng_oblivion_statsexp_page_month(tr::now);
	result.colHour = tr::lng_oblivion_statsexp_page_hour(tr::now);
	result.colDay = tr::lng_oblivion_statsexp_page_day(tr::now);
	result.calendarNote = tr::lng_oblivion_statsexp_page_calendar_note(
		tr::now);

	result.cellMessages = tr::lng_oblivion_stats_cell_messages(tr::now);
	result.cellWords = tr::lng_oblivion_stats_cell_words(tr::now);
	result.cellPeople = tr::lng_oblivion_stats_cell_people(tr::now);
	result.cellEmoji = tr::lng_oblivion_stats_cell_emoji(tr::now);
	result.cellDays = tr::lng_oblivion_stats_cell_days(tr::now);
	result.cellPerDay = tr::lng_oblivion_stats_cell_per_day(tr::now);
	result.cellLength = tr::lng_oblivion_stats_cell_length(tr::now);

	result.sectionPeople = tr::lng_oblivion_stats_section_people(tr::now);
	result.sectionPeoplePrivate
		= tr::lng_oblivion_stats_section_people_private(tr::now);
	result.sectionMonths = tr::lng_oblivion_stats_section_months(tr::now);
	result.sectionCalendar = tr::lng_oblivion_stats_section_calendar(tr::now);
	result.sectionHours = tr::lng_oblivion_stats_section_hours(tr::now);
	result.sectionWeekdays = tr::lng_oblivion_stats_section_weekdays(tr::now);
	result.sectionKinds = tr::lng_oblivion_stats_section_media(tr::now);
	result.sectionWords = tr::lng_oblivion_stats_section_words(tr::now);
	result.sectionEmoji = tr::lng_oblivion_stats_section_emoji(tr::now);
	result.sectionFacts = tr::lng_oblivion_stats_section_facts(tr::now);

	result.factPeriod = tr::lng_oblivion_stats_fact_period(tr::now);
	result.factFirst = tr::lng_oblivion_stats_fact_first(tr::now);
	result.factStreak = tr::lng_oblivion_stats_fact_streak(tr::now);
	result.factStreakNow = tr::lng_oblivion_stats_fact_streak_now(tr::now);
	result.factTopDays = tr::lng_oblivion_stats_fact_top_days(tr::now);
	result.factReplies = tr::lng_oblivion_stats_fact_replies(tr::now);
	result.factForwards = tr::lng_oblivion_stats_fact_forwards(tr::now);
	result.factLinks = tr::lng_oblivion_stats_kind_link(tr::now);

	result.less = tr::lng_oblivion_stats_less(tr::now);
	result.more = tr::lng_oblivion_stats_more(tr::now);
	result.unknown = tr::lng_oblivion_stats_unknown(tr::now);

	result.kinds = {
		tr::lng_oblivion_stats_kind_text(tr::now),
		tr::lng_oblivion_stats_kind_photo(tr::now),
		tr::lng_oblivion_stats_kind_video(tr::now),
		tr::lng_oblivion_stats_kind_round(tr::now),
		tr::lng_oblivion_stats_kind_voice(tr::now),
		tr::lng_oblivion_stats_kind_music(tr::now),
		tr::lng_oblivion_stats_kind_file(tr::now),
		tr::lng_oblivion_stats_kind_sticker(tr::now),
		tr::lng_oblivion_stats_kind_gif(tr::now),
		tr::lng_oblivion_stats_kind_poll(tr::now),
		tr::lng_oblivion_stats_kind_location(tr::now),
		tr::lng_oblivion_stats_kind_contact(tr::now),
		tr::lng_oblivion_stats_kind_call(tr::now),
		tr::lng_oblivion_stats_kind_other(tr::now),
	};
	for (auto month = 0; month != 12; ++month) {
		result.monthsShort[month] = locale.standaloneMonthName(
			month + 1,
			QLocale::ShortFormat).left(3);
		result.monthsLong[month] = capitalized(locale.standaloneMonthName(
			month + 1,
			QLocale::LongFormat));
	}
	for (auto weekday = 0; weekday != 7; ++weekday) {
		result.weekdaysShort[weekday] = capitalized(withoutDot(
			locale.standaloneDayName(weekday + 1, QLocale::ShortFormat)));
		result.weekdaysLong[weekday] = capitalized(
			locale.standaloneDayName(weekday + 1, QLocale::LongFormat));
	}

	result.number = [](int64 value) {
		return Lang::FormatCountDecimal(value);
	};
	result.messages = [](int count) {
		return tr::lng_oblivion_stats_messages(
			tr::now,
			lt_count_decimal,
			count);
	};
	result.words = [](int64 count) {
		return tr::lng_oblivion_stats_words(
			tr::now,
			lt_count_decimal,
			float64(count));
	};
	result.days = [](int count) {
		return tr::lng_oblivion_stats_days(
			tr::now,
			lt_count_decimal,
			count);
	};
	result.peopleMore = [](int count) {
		return tr::lng_oblivion_stats_people_more(
			tr::now,
			lt_count_decimal,
			count);
	};
	result.date = dateOf;
	result.answer = [=](bool self, TimeId seconds) {
		return self
			? tr::lng_oblivion_stats_answer_you(
				tr::now,
				lt_time,
				span(seconds))
			: tr::lng_oblivion_stats_answer_other(
				tr::now,
				lt_time,
				span(seconds));
	};
	return result;
}

void AddSaveButton(
		not_null<Ui::VerticalLayout*> container,
		SaveButtonArgs &&args) {
	const auto st = container->lifetime().make_state<style::SettingsButton>(
		st::settingsButtonNoIcon);
	st->padding.setLeft(st::boxRowPadding.left());
	st->padding.setRight(st::boxRowPadding.right());
	const auto wrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::SettingsButton>>(
			container,
			object_ptr<Ui::SettingsButton>(
				container,
				tr::lng_oblivion_statsexp_save(),
				*st)),
		style::margins());
	wrap->toggle(false, anim::type::instant);
	std::move(
		args.shown
	) | rpl::on_next([=](bool shown) {
		wrap->toggle(shown, anim::type::instant);
	}, wrap->lifetime());

	const auto button = wrap->entity();
	const auto show = args.show;
	const auto collect = args.collect;
	button->setClickedCallback([=] {
		auto input = collect ? collect() : Input();
		if (!input.report || !show) {
			return;
		}
		auto prefix = tr::lng_oblivion_statsexp_file(tr::now);
		const auto chat = FileNamePart(input.chat);
		if (!chat.isEmpty()) {
			prefix += u" — "_q + chat;
		}
		auto caption = tr::lng_oblivion_statsexp_save(tr::now);
		while (caption.endsWith(QChar(0x2026)) || caption.endsWith('.')) {
			caption.chop(1);
		}
		FileDialog::GetWritePath(
			button->window(),
			caption,
			tr::lng_oblivion_statsexp_filter(tr::now) + u" (*.html)"_q,
			filedialogDefaultName(prefix, u".html"_q),
			crl::guard(button, [=](QString &&path) {
				if (!path.isEmpty()) {
					SavePage(show, input, std::move(path));
				}
			}));
	});
}

namespace {

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

[[nodiscard]] Labels TestLabels() {
	auto result = Labels();
	result.lang = u"en"_q;
	result.pageTitle = u"Chat statistics"_q;
	result.made = u"Made in Oblivion on 2026-10-07"_q;
	result.privacy = u"Only counters."_q;
	result.incomplete = u"Partial numbers."_q;
	result.table = u"Show as a table"_q;
	result.colMonth = u"Month"_q;
	result.colHour = u"Hour"_q;
	result.colDay = u"Day"_q;
	result.calendarNote = u"One square is one day."_q;
	result.cellMessages = u"Messages"_q;
	result.cellWords = u"Words"_q;
	result.cellPeople = u"Participants"_q;
	result.cellEmoji = u"Emoji"_q;
	result.cellDays = u"Active days"_q;
	result.cellPerDay = u"Per active day"_q;
	result.cellLength = u"Words per message"_q;
	result.sectionPeople = u"Participants"_q;
	result.sectionPeoplePrivate = u"Who writes more"_q;
	result.sectionMonths = u"By month"_q;
	result.sectionCalendar = u"Calendar"_q;
	result.sectionHours = u"By hour of day"_q;
	result.sectionWeekdays = u"By day of week"_q;
	result.sectionKinds = u"By type of message"_q;
	result.sectionWords = u"Frequent words"_q;
	result.sectionEmoji = u"Frequent emoji"_q;
	result.sectionFacts = u"Facts"_q;
	result.factPeriod = u"Dates of the messages"_q;
	result.factFirst = u"First message"_q;
	result.factStreak = u"Longest streak of days"_q;
	result.factStreakNow = u"Current streak"_q;
	result.factTopDays = u"Most active days"_q;
	result.factReplies = u"Replies"_q;
	result.factForwards = u"Forwarded"_q;
	result.factLinks = u"With links"_q;
	result.less = u"less"_q;
	result.more = u"more"_q;
	result.unknown = u"Unknown participant"_q;
	for (auto i = 0; i != 14; ++i) {
		result.kinds[i] = u"Kind %1"_q.arg(i);
	}
	for (auto i = 0; i != 12; ++i) {
		result.monthsShort[i] = u"M%1"_q.arg(i + 1);
		result.monthsLong[i] = u"Month %1"_q.arg(i + 1);
	}
	for (auto i = 0; i != 7; ++i) {
		result.weekdaysShort[i] = u"D%1"_q.arg(i + 1);
		result.weekdaysLong[i] = u"Weekday %1"_q.arg(i + 1);
	}
	result.number = [](int64 value) { return QString::number(value); };
	result.messages = [](int count) {
		return QString::number(count) + u" messages"_q;
	};
	result.words = [](int64 count) {
		return QString::number(count) + u" words"_q;
	};
	result.days = [](int count) {
		return QString::number(count) + u" days"_q;
	};
	result.peopleMore = [](int count) {
		return u"and %1 more"_q.arg(count);
	};
	result.answer = [](bool self, TimeId seconds) {
		return (self ? u"you answer in %1 s"_q : u"answers in %1 s"_q).arg(
			seconds);
	};
	return result;
}

// A report made by hand: a year and a half of a chat, every day has
// something, the numbers only have to be plausible.
[[nodiscard]] std::shared_ptr<const Report> TestReport(
		bool group,
		const std::vector<QString> &names) {
	auto result = std::make_shared<Report>();
	const auto last = DayOfDate({ 2026, 9, 30 });
	const auto first = last - 539;
	result->group = group;
	result->complete = true;
	result->wholeHistory = true;
	result->firstDay = first;
	result->lastDay = last;
	result->from = 0;
	result->till = TimeId(int64(last) * 86400 + 17 * 3600);
	result->firstDate = TimeId(int64(first) * 86400 + 9 * 3600 + 5 * 60);
	auto total = 0;
	for (auto day = first; day <= last; ++day) {
		const auto index = day - first;
		const auto count = ((index * 37) % 11) ? ((index * 53) % 97) : 0;
		result->days.push_back(count);
		total += count;
		if (count > 0) {
			++result->activeDays;
		}
		result->weekdays[WeekdayOfDay(day)] += count;
		result->hours[(index * 7) % 24] += count;
		const auto date = DateOfDay(day);
		if (result->months.empty()
			|| result->months.back().month != date.month) {
			result->months.push_back({ date.year, date.month, 0 });
		}
		result->months.back().count += count;
	}
	result->messages = total;
	result->textMessages = total * 4 / 5;
	result->words = int64(total) * 6;
	result->chars = int64(total) * 31;
	result->emoji = total / 3;
	auto left = total;
	auto id = uint64(1000001);
	for (const auto &name : names) {
		const auto mine = (&name == &names.back()) ? left : (left / 2);
		left -= mine;
		result->persons.push_back({
			.id = id++,
			.name = name,
			.messages = mine,
			.textMessages = mine * 4 / 5,
			.media = mine / 5,
			.words = int64(mine) * 6,
			.chars = int64(mine) * 31,
			.answers = group ? 0 : 40,
			.answerTime = group ? 0 : 95,
		});
	}
	result->firstSender = 1000001;
	result->kinds[int(Kind::Text)] = total * 4 / 5;
	result->kinds[int(Kind::Photo)] = total / 10;
	result->kinds[int(Kind::Sticker)] = total / 20;
	result->kinds[int(Kind::Voice)] = total / 20;
	result->links = total / 25;
	result->forwards = total / 40;
	result->replies = total / 6;
	result->topWords = {
		{ u"привет"_q, 412 },
		{ u"<b>bold</b>"_q, 77 },
		{ u"a&b"_q, 31 },
	};
	result->topEmoji = {
		{ QString::fromUtf8("\xF0\x9F\x98\x82"), 240 },
		{ QString::fromUtf8("\xE2\x9D\xA4\xEF\xB8\x8F"), 118 },
	};
	result->longest = { .fromDay = first + 12, .days = 9 };
	result->current = { .fromDay = last - 2, .days = 3 };
	result->topDays = { { last - 40, 96 }, { last - 7, 95 } };
	return result;
}

struct Scan {
	bool wellFormed = false;
	QString error;
	QStringList problems;
	QString style;
	QString text;
	int styles = 0;
	int charts = 0;
	int tables = 0;
};

// Reads the page as a strict document: every tag is closed, every
// attribute is quoted. Elements and attributes outside of the short
// lists the page is made of (anything that could load or run something)
// are reported.
[[nodiscard]] Scan ScanPage(const QByteArray &bytes) {
	static const auto kElements = QSet<QString>{
		u"html"_q, u"head"_q, u"meta"_q, u"title"_q, u"style"_q,
		u"body"_q, u"div"_q, u"header"_q, u"footer"_q, u"section"_q,
		u"h1"_q, u"h2"_q, u"p"_q, u"span"_q, u"bdi"_q, u"i"_q, u"br"_q,
		u"ul"_q, u"li"_q, u"dl"_q, u"dt"_q, u"dd"_q, u"details"_q,
		u"summary"_q, u"table"_q, u"thead"_q, u"tbody"_q, u"tr"_q,
		u"th"_q, u"td"_q, u"svg"_q, u"g"_q, u"rect"_q, u"path"_q,
		u"line"_q, u"text"_q,
	};
	static const auto kAttributes = QSet<QString>{
		u"lang"_q, u"charset"_q, u"name"_q, u"content"_q,
		u"http-equiv"_q, u"class"_q, u"viewBox"_q, u"role"_q,
		u"aria-label"_q, u"x"_q, u"y"_q, u"x1"_q, u"y1"_q, u"x2"_q,
		u"y2"_q, u"width"_q, u"height"_q, u"rx"_q, u"d"_q,
		u"text-anchor"_q, u"style"_q,
	};
	auto result = Scan();
	auto reader = QXmlStreamReader(bytes);
	while (!reader.atEnd()) {
		const auto token = reader.readNext();
		if (token == QXmlStreamReader::StartElement) {
			const auto name = reader.name().toString();
			if (!kElements.contains(name)) {
				result.problems.push_back(u"element "_q + name);
			}
			const auto attributes = reader.attributes();
			for (const auto &attribute : attributes) {
				const auto key = attribute.name().toString();
				const auto value = attribute.value().toString();
				if (!kAttributes.contains(key)) {
					result.problems.push_back(u"attribute "_q + key);
				} else if (key == u"style"_q
					&& (!value.startsWith(u"width:"_q)
						|| value.contains(QChar('(')))) {
					result.problems.push_back(u"style "_q + value);
				}
			}
			if (name == u"style"_q) {
				++result.styles;
				result.style += reader.readElementText();
			} else if (name == u"svg"_q) {
				++result.charts;
			} else if (name == u"table"_q) {
				++result.tables;
			}
		} else if (token == QXmlStreamReader::Characters) {
			result.text += reader.text().toString();
		}
	}
	result.wellFormed = !reader.hasError();
	result.error = reader.errorString();
	return result;
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	RegisterBoxScene(u"stats_export_done"_q, QSize(st::boxWideWidth * 2, 0), [](
			std::shared_ptr<Ui::Show> show) {
		const auto russian = CurrentLanguageIsRussian();
		return Box(DoneBox, DoneArgs{
			.path = (russian
				? u"/Users/anna/Documents/Статистика — Походы и горы"
					u"_2026-10-07_09-41-00.html"_q
				: u"/Users/anna/Documents/Statistics — Hiking club"
					u"_2026-10-07_09-41-00.html"_q),
			.size = 148 * 1024,
		});
	});
});

} // namespace

bool RunSelfTest(QStringList &log) {
	auto check = Checker(log);
	const auto labels = TestLabels();
	const auto zone = Zone::Fixed(3 * 3600);

	check(NiceMax(0) == 2, "scale: nothing");
	check(NiceMax(3) == 4, "scale: 3");
	check(NiceMax(5) == 6, "scale: 5");
	check(NiceMax(37) == 40, "scale: 37");
	check(NiceMax(75) == 80, "scale: 75");
	check(NiceMax(1234) == 2000, "scale: 1234");
	check(NiceMax(100) == 100, "scale: 100");
	check(Percent(1, 2) == u"50%"_q, "percent: half");
	check(Percent(1, 1000) == u"<1%"_q, "percent: small");
	check(Percent(0, 10).isEmpty(), "percent: none");
	check(Percent(5, 0).isEmpty(), "percent: no whole");
	check(Fraction(labels, 3.04) == u"3"_q, "fraction: whole");
	check(Fraction(labels, 3.26) == u"3.3"_q, "fraction: tenths");
	check(Num(12.) == u"12"_q && Num(12.25) == u"12.3"_q, "coordinates");
	check.section("numbers");

	check(Escape(u"a<b>&\"c'"_q) == u"a&lt;b&gt;&amp;&quot;c&#39;"_q,
		"escape: markup");
	check(Escape(QString::fromUtf8("x\xE2\x80\xAEy")) == u"xy"_q,
		"escape: bidi override is dropped");
	check(Escape(QString(QChar(0x01)) + u"a\nb"_q) == u"a b"_q,
		"escape: control characters");
	check(Escape(QString(QChar(0xD83D)) + u"a"_q) == u"a"_q,
		"escape: half of a pair");
	check(Escape(QString::fromUtf8("\xF0\x9F\x98\x82"))
		== QString::fromUtf8("\xF0\x9F\x98\x82"),
		"escape: emoji is kept");
	check.section("escape");

	const auto hostile = std::vector<QString>{
		u"<script>alert(1)</script>"_q,
		u"\"quoted\" & 'single' https://evil.example/a.js"_q,
		QString::fromUtf8("Аня \xE2\x80\xAE\xD0\x9A\xD0\xBE\xD1\x82"),
		QString(),
	};
	const auto person = [](uint64 id, const QString &name) {
		return Person{ .name = name, .self = (id == 1000002) };
	};
	const auto groupBytes = Generate({
		.report = TestReport(true, hostile),
		.zone = zone,
		.chat = u"<img src=x onerror=alert(1)> & \"friends\""_q,
		.person = person,
	}, labels);
	const auto groupPage = QString::fromUtf8(groupBytes);
	const auto group = ScanPage(groupBytes);
	if (!group.wellFormed) {
		log.push_back(u"group page: "_q + group.error);
	}
	for (const auto &problem : group.problems) {
		log.push_back(u"group page: "_q + problem);
	}
	check(groupBytes.startsWith("<!DOCTYPE html>"), "page: doctype");
	check(group.wellFormed, "page: well-formed");
	check(group.problems.isEmpty(), "page: only known elements");
	check(group.styles == 1, "page: one style block");
	check(group.charts == 4, "page: four charts");
	check(group.tables == 3, "page: three tables");
	check(!group.style.contains(u"url("_q)
		&& !group.style.contains(u"@import"_q)
		&& !group.style.contains(u"http"_q)
		&& !group.style.contains(u"//"_q)
		&& !group.style.contains(u"expression"_q),
		"page: the styles load nothing");
	check(group.style.contains(u"prefers-color-scheme:dark"_q),
		"page: night colors");
	check(groupPage.contains(u"default-src 'none'"_q),
		"page: the policy forbids loading");
	check(!groupPage.contains(u"<script"_q, Qt::CaseInsensitive)
		&& !groupPage.contains(u"<img"_q, Qt::CaseInsensitive)
		&& !groupPage.contains(u"<link"_q, Qt::CaseInsensitive)
		&& !groupPage.contains(u"<iframe"_q, Qt::CaseInsensitive),
		"page: nothing that loads or runs");
	check(groupPage.contains(u"&lt;script&gt;alert(1)&lt;/script&gt;"_q),
		"page: a name is escaped");
	check(groupPage.contains(u"&lt;img src=x onerror=alert(1)&gt;"_q),
		"page: the chat name is escaped");
	check(groupPage.contains(u"&quot;quoted&quot; &amp; &#39;single&#39;"_q),
		"page: quotes are escaped");
	check(!groupPage.contains(QChar(0x202E)), "page: no bidi override");
	check(group.text.contains(u"Unknown participant"_q),
		"page: a participant without a name");
	check(group.text.contains(u"<b>bold</b>"_q)
		&& group.text.contains(u"a&b"_q),
		"page: words are text, not markup");
	check(group.text.contains(u"Only counters."_q), "page: privacy note");
	check(group.text.contains(u"Participants"_q), "page: people");
	check(!group.text.contains(u"Partial numbers."_q),
		"page: no warning for a complete report");
	check(Generate({
		.report = TestReport(true, hostile),
		.zone = zone,
		.chat = u"<img src=x onerror=alert(1)> & \"friends\""_q,
		.person = person,
	}, labels) == groupBytes, "page: the same for the same data");
	check.section("group page");

	const auto directBytes = Generate({
		.report = TestReport(false, { u"Anna"_q, u"Boris"_q }),
		.zone = zone,
		.chat = u"Boris"_q,
		.person = person,
	}, labels);
	const auto direct = ScanPage(directBytes);
	check(direct.wellFormed && direct.problems.isEmpty(),
		"private page: well-formed");
	check(direct.text.contains(u"Who writes more"_q),
		"private page: the title of the people");
	check(direct.text.contains(u"you answer in 95 s"_q)
		&& direct.text.contains(u"answers in 95 s"_q),
		"private page: answer times");
	check(direct.text.contains(u"First message"_q)
		&& direct.text.contains(u"12:05"_q),
		"private page: the first message in the local time");
	check.section("private page");

	auto empty = std::make_shared<Report>();
	const auto emptyBytes = Generate({ .report = empty, .zone = zone }, labels);
	const auto emptyScan = ScanPage(emptyBytes);
	check(emptyScan.wellFormed && emptyScan.problems.isEmpty(),
		"empty page: well-formed");
	check(emptyScan.charts == 0, "empty page: no charts");
	check(emptyScan.text.contains(u"Partial numbers."_q),
		"empty page: says that it is incomplete");
	check(Generate({}, labels).isEmpty(), "no report: no page");

	auto single = std::make_shared<Report>();
	single->messages = 1;
	single->complete = true;
	single->firstDay = single->lastDay = DayOfDate({ 2026, 9, 30 });
	single->days = { 1 };
	single->months = { { 2026, 9, 1 } };
	single->activeDays = 1;
	single->hours[13] = 1;
	single->weekdays[2] = 1;
	single->kinds[0] = 1;
	single->persons.push_back({ .id = 5, .name = u"A"_q, .messages = 1 });
	const auto singleScan = ScanPage(Generate({
		.report = single,
		.zone = zone,
		.chat = u"A"_q,
	}, labels));
	check(singleScan.wellFormed && singleScan.problems.isEmpty(),
		"one message: well-formed");
	check(singleScan.charts == 2, "one message: hours and weekdays");
	check.section("edge pages");

	check(FileNamePart(u"a/b\\c:d*e?f\"g<h>i|j"_q) == u"a_b_c_d_e_f_g_h_i_j"_q,
		"file name: bad characters");
	check(FileNamePart(u"  name.  "_q) == u"name"_q, "file name: the end");
	check(FileNamePart(QString(200, QChar('x'))).size() == kMaxFileNameLength,
		"file name: length");
	check.section("file name");

	return !check.failed();
}

} // namespace Oblivion::StatsExport
