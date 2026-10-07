/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <array>

namespace Ui {
class BoxContent;
class Show;
class VerticalLayout;
} // namespace Ui

namespace Oblivion::ChatStats {
struct Report;
class Zone;
} // namespace Oblivion::ChatStats

// Round 5: the chat statistics as a self-contained HTML page
// («Сохранить страницу…» of the statistics box).
//
// The page is one file: the styles and the charts (SVG) are inside of it,
// nothing is loaded from anywhere (a Content-Security-Policy in the file
// forbids it as well), there are no scripts. It follows the day / night
// setting of the system (prefers-color-scheme).
//
// The privacy rule is the one of the statistics themselves: the page is
// made from a ChatStats::Report, so it has only what the box shows, the
// counters, and no message texts.
namespace Oblivion::StatsExport {

// Everything the page says in words, see LangLabels(). The generator is
// pure: the self-test gives it fixed labels.
struct Labels {
	QString lang; // "ru", "en": the lang attribute of the page.
	QChar decimal = QChar('.');

	QString pageTitle;
	QString made; // With the date already inside.
	QString privacy;
	QString incomplete;
	QString table;
	QString colMonth;
	QString colHour;
	QString colDay;
	QString calendarNote;

	QString cellMessages;
	QString cellWords;
	QString cellPeople;
	QString cellEmoji;
	QString cellDays;
	QString cellPerDay;
	QString cellLength;

	QString sectionPeople;
	QString sectionPeoplePrivate;
	QString sectionMonths;
	QString sectionCalendar;
	QString sectionHours;
	QString sectionWeekdays;
	QString sectionKinds;
	QString sectionWords;
	QString sectionEmoji;
	QString sectionFacts;

	QString factPeriod;
	QString factFirst;
	QString factStreak;
	QString factStreakNow;
	QString factTopDays;
	QString factReplies;
	QString factForwards;
	QString factLinks;

	QString less;
	QString more;
	QString unknown; // A participant without a name.

	std::array<QString, 14> kinds; // By ChatStats::Kind.
	std::array<QString, 12> monthsShort;
	std::array<QString, 12> monthsLong;
	std::array<QString, 7> weekdaysShort; // From Monday.
	std::array<QString, 7> weekdaysLong;

	Fn<QString(int64)> number; // "12 345"
	Fn<QString(int)> messages; // "5 messages"
	Fn<QString(int64)> words; // "5 words"
	Fn<QString(int)> days; // "5 days"
	Fn<QString(int)> peopleMore; // "and 5 more participants"
	Fn<QString(int32 day)> date; // "7 October 2026"
	Fn<QString(bool self, TimeId seconds)> answer; // "answers in 5 min"
};

// The labels in the language of the app, needs Lang.
[[nodiscard]] Labels LangLabels(TimeId now);

struct Person {
	QString name;
	bool self = false;
};

struct Input {
	std::shared_ptr<const ChatStats::Report> report;
	std::shared_ptr<ChatStats::Zone> zone;
	QString chat;

	// The name to show: of the account ("You"), the one the chat list
	// has now, or the one saved with the statistics.
	Fn<Person(uint64 id, const QString &name)> person;
};

// For text and attribute values: & < > " ' are escaped, the characters
// a document can't have and the ones that reorder the text around them
// (bidi overrides) are dropped.
[[nodiscard]] QString Escape(const QString &text);

// The whole page, UTF-8. Empty when there is no report.
[[nodiscard]] QByteArray Generate(const Input &input, const Labels &labels);

// The row of the statistics box: asks for the place, writes the page
// off the main thread and shows the "saved" box. Nothing leaves the
// device. collect is called on a click, on the main thread.
struct SaveButtonArgs {
	std::shared_ptr<Ui::Show> show;
	rpl::producer<bool> shown;
	Fn<Input()> collect;
};
void AddSaveButton(
	not_null<Ui::VerticalLayout*> container,
	SaveButtonArgs &&args);

// OBLIVION_SELFTEST=stats_export, pure logic.
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::StatsExport
