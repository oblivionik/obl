/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"

#include <QtCore/QTimeZone>

#include <array>
#include <map>
#include <mutex>
#include <unordered_map>

class PeerData;

namespace Main {
class Session;
} // namespace Main

// Chat statistics, the part without the UI (oblivion_chat_stats_ui.h
// shows it): totals, messages per participant, activity by hour of day,
// weekday and calendar, a timeline by day and month, top words (with
// stop words for Russian and English), top emoji and stickers, media
// counts by type, the average length, the reply time (private chats),
// the longest streak, the first message, the most active days, the share
// of forwards and replies.
//
// The history is read with paged requests at a gentle pace (one request
// in flight for the whole account, a pause after every answer,
// FLOOD_WAIT waited out in full) with progress, cancel and resume, cached
// on disk per chat and updated incrementally, with a period limit
// (3 months / a year / everything). Everything is computed on this
// device, nothing is sent anywhere.
//
// The cache: tdata/oblivion/<user id>/stats/<peer id>.dat, not encrypted.
// It holds counters, nothing about a single message: neither a text nor a
// row with a date and a length. For every local day: how many messages
// there were at every hour and of every kind, how many of them were
// replies, forwards and had links, and for every sender of that day the
// numbers of messages, words and characters and (in a private chat) how
// many answers took how long. For every calendar month: how many times
// every word (without the stop words), emoji and sticker was met.
//
// Besides the counters the file has the names of the senders, the moment
// and the sender of the oldest and of the newest counted message, how far
// the history is read, and for a sticker that is named its emoji and the
// number of one message with it, to show its picture.
//
// A word, an emoji or a sticker is named (written as it is) only after it
// was met kNamedAfter times in the chat, in a list sorted by the alphabet.
// The counters of the months do not point at that list: they are kept
// under numbers made from the word with a key that is not in the file
// (see Model), so a month with one message does not tell which words were
// in it, and a word met once or twice is not written anywhere.
//
// A message deleted or edited after it was counted stays counted as it
// was, till the cache of the chat is cleared.
namespace Oblivion::ChatStats {

enum class Kind : uchar {
	Text,
	Photo,
	Video,
	Round,
	Voice,
	Music,
	File,
	Sticker,
	Gif,
	Poll,
	Location,
	Contact,
	Call,
	Other,
};
inline constexpr auto kKindCount = 14;

enum MessageFlag : uchar {
	kForwarded = 0x01,
	kReply = 0x02,
	kLink = 0x04,
};

enum class Period : uchar {
	Months3,
	Year,
	All,
};

// A word, an emoji or a sticker is named in the cache once it was met
// this many times in the chat.
inline constexpr auto kNamedAfter = uint32(3);

// The local time of the user: offsets are asked from the system once per
// day of the history. Thread safe.
class Zone final {
public:
	explicit Zone(QTimeZone zone);

	[[nodiscard]] static std::shared_ptr<Zone> System();
	[[nodiscard]] static std::shared_ptr<Zone> Fixed(int secondsAheadOfUtc);

	// Seconds to add to a unixtime to get the local wall clock.
	[[nodiscard]] int offset(TimeId when) const;

	// Whole local days since 1970-01-01.
	[[nodiscard]] int32 day(TimeId when) const;

	// The unixtime of the local midnight that starts this day.
	[[nodiscard]] TimeId dayStart(int32 day) const;

private:
	struct Offsets {
		int start = 0;
		int end = 0;
	};
	[[nodiscard]] Offsets offsets(int32 utcDay) const;

	const QTimeZone _zone;
	mutable std::mutex _mutex;
	mutable std::unordered_map<int32, Offsets> _cache;

};

struct CivilDate {
	int year = 1970;
	int month = 1; // 1..12
	int day = 1; // 1..31

	friend inline bool operator==(
		const CivilDate &,
		const CivilDate &) = default;
};
[[nodiscard]] CivilDate DateOfDay(int32 day);
[[nodiscard]] int32 DayOfDate(CivilDate date);

// 0 is Monday.
[[nodiscard]] int WeekdayOfDay(int32 day);

// Calendar months in a row: twelve of them in every year.
[[nodiscard]] int32 MonthOfDay(int32 day);
[[nodiscard]] int32 FirstDayOfMonth(int32 month);

// The text of one message taken apart.
struct TextSkip {
	int offset = 0;
	int length = 0;
};
struct TextStats {
	int length = 0; // Characters, a surrogate pair is one.
	int words = 0; // Everything that has a letter, the stop words too.
	std::vector<QString> tokens; // Lower case words worth counting.
	std::vector<QString> emoji; // Without the skin tones.
};
[[nodiscard]] TextStats ParseText(
	const QString &text,
	const std::vector<TextSkip> &skip = {});
[[nodiscard]] bool IsStopWord(const QString &word);

// The same emoji typed with and without the variation selector.
[[nodiscard]] QString EmojiKey(const QString &emoji);

// What the server gave about one message, before it is counted. Lives
// only in memory, between the answer and the counting.
struct RawMessage {
	int32 id = 0;
	TimeId date = 0;
	uint64 sender = 0; // PeerId value.
	Kind kind = Kind::Text;
	uchar flags = 0; // kForwarded | kReply | kLink.
	QString text;
	std::vector<TextSkip> skip; // Links, mentions, code.
	uint64 sticker = 0; // Document id for Kind::Sticker.
	QString stickerEmoji;
};

struct RawName {
	uint64 id = 0;
	QString name;

	friend inline bool operator==(const RawName &, const RawName &) = default;
};

enum class PageType : uchar {
	First, // The newest messages, nothing is cached yet.
	Newer, // After the newest cached one.
	Older, // Before the oldest cached one.
};

struct RawPage {
	bool old = false; // From the basic group the supergroup was made of.
	bool direct = false; // A private chat: the answer times are counted.
	PageType type = PageType::First;
	std::vector<RawMessage> messages; // The counted ones.
	std::vector<RawName> names; // Of their senders.

	// Of everything the server returned, the service messages too.
	int returned = 0;
	int32 minId = 0;
	int32 maxId = 0;
	TimeId minDate = 0;
	TimeId maxDate = 0;

	int total = 0; // Messages in the history as the server says, or 0.
};

// The part of one history that is in the cache: every message with an
// id from low to high.
struct Range {
	int32 low = 0;
	int32 high = 0;
	TimeId lowDate = 0;
	TimeId highDate = 0;
	int total = 0;
	bool started = false; // An answer was applied.
	bool reachedStart = false; // Nothing is older than low.

	friend inline bool operator==(const Range &, const Range &) = default;
};

struct Coverage {
	Range main;
	Range old;
	TimeId checked = 0; // When the newer messages were asked about.
	Period period = Period::Months3; // The last one chosen for this chat.

	// The user has stopped the reading: it is not continued by itself,
	// neither when the statistics are opened again nor after a restart.
	bool stopped = false;

	friend inline bool operator==(
		const Coverage &,
		const Coverage &) = default;
};

inline constexpr auto kPageLimit = 100;

struct Request {
	bool valid = false;
	bool old = false;
	PageType type = PageType::First;
	int32 offsetId = 0;
	int addOffset = 0;
	int limit = 0;

	friend inline bool operator==(const Request &, const Request &) = default;
};

struct PlanArgs {
	TimeId from = 0; // The start of the period, 0 for everything.
	bool hasOld = false; // There is a basic group before the supergroup.
	bool headChecked = false; // Newer messages were asked in this run.
	int limit = kPageLimit;
};

// The next request of a run, not valid when everything is read.
[[nodiscard]] Request NextRequest(
	const Coverage &coverage,
	const PlanArgs &args);

// Whether everything from this moment on is in the cache.
[[nodiscard]] bool Covers(
	const Coverage &coverage,
	TimeId from,
	bool hasOld);

struct StickerInfo {
	int32 msgId = 0; // A message with it, to load the image.
	bool old = false;
	QString emoji;

	friend inline bool operator==(
		const StickerInfo &,
		const StickerInfo &) = default;
};

template <typename Id>
using Counts = std::vector<std::pair<Id, uint32>>; // Sorted by the id.

// The time an answer took is kept as one of these slots: every second of
// the first minute, then every minute up to the longest wait that is
// still an answer.
inline constexpr auto kAnswerSlots = 780;
[[nodiscard]] int AnswerSlot(TimeId seconds);
[[nodiscard]] TimeId AnswerSlotTime(int slot);

// The counters of one sender on one local day.
struct PersonDay {
	uint32 sender = 0; // Index in Model::senders().
	uint32 messages = 0;
	uint32 textMessages = 0;
	uint32 media = 0;
	uint32 words = 0;
	uint32 chars = 0;
	Counts<uint16> answers; // By AnswerSlot().

	friend inline bool operator==(
		const PersonDay &,
		const PersonDay &) = default;
};

// The counters of one local day.
struct Day {
	std::array<uint32, 24> hours = {};
	std::array<uint32, kKindCount> kinds = {};
	uint32 links = 0;
	uint32 forwards = 0;
	uint32 replies = 0;
	uint32 emoji = 0;
	std::vector<PersonDay> persons; // Sorted by the sender.

	friend inline bool operator==(const Day &, const Day &) = default;
};
[[nodiscard]] int DayMessages(const Day &day);

// The oldest or the newest counted message of a history. An answer is
// measured from the last message of one portion of the history to the
// first message of the next one, so the ends of what is read are kept.
struct Edge {
	TimeId date = 0;
	uint32 sender = 0; // Index in Model::senders().
	bool known = false;

	friend inline bool operator==(const Edge &, const Edge &) = default;
};

struct Edges {
	Edge oldest;
	Edge newest;

	friend inline bool operator==(const Edges &, const Edges &) = default;
};

// The words, the emoji or the stickers of a chat. Each one has a key: a
// number made from it with the key of the model. A name is the word
// itself (the id of the sticker), empty (zero) till it is met
// kNamedAfter times.
template <typename Name>
struct Counted {
	std::vector<uint64> keys;
	std::vector<uint32> totals; // In all the months.
	std::vector<Name> names;
	std::unordered_map<uint64, uint32> index; // By the key.
};

// The counters of one calendar month (local time), by the index in
// Counted. Nothing finer is kept.
struct MonthBag {
	Counts<uint32> words;
	Counts<uint32> emoji;
	Counts<uint32> stickers;

	friend inline bool operator==(
		const MonthBag &,
		const MonthBag &) = default;
};

struct ParseResult {
	bool valid = false; // The header is fine.
	bool damaged = false; // The file is broken, nothing of it is taken.
	bool outdated = false; // A cache, but of another version of the file.
};

// Everything known about one chat.
class Model final {
public:
	// The keys of the words are made with this secret and with random
	// bytes kept in the file. The secret is never written: without it the
	// counters of the months can't be matched to the words, and a file
	// made with another secret is not read.
	explicit Model(QByteArray secret = QByteArray());

	// Counts the messages of an answer that are not counted yet, returns
	// how many of them there were.
	int add(const RawPage &page, const Zone &zone);

	void setChecked(TimeId checked);
	void setPeriod(Period period);
	void setStopped(bool stopped);

	[[nodiscard]] const Coverage &coverage() const {
		return _coverage;
	}
	[[nodiscard]] int count() const { // Of the counted messages.
		return _count;
	}
	[[nodiscard]] const std::vector<RawName> &senders() const {
		return _senders;
	}
	[[nodiscard]] const std::map<int32, Day> &days() const {
		return _days;
	}
	[[nodiscard]] const Edges &edges(bool old) const {
		return old ? _oldEdges : _mainEdges;
	}
	[[nodiscard]] const Counted<QString> &words() const {
		return _words;
	}
	[[nodiscard]] const Counted<QString> &emoji() const {
		return _emoji;
	}
	[[nodiscard]] const Counted<uint64> &stickers() const {
		return _stickers;
	}
	[[nodiscard]] const std::map<int32, MonthBag> &months() const {
		return _months;
	}

	// Of the named stickers, by the id.
	[[nodiscard]] const std::map<uint64, StickerInfo> &stickerInfos() const {
		return _stickerInfos;
	}

	// The cache file: a header with the size and the checksum of the
	// rest. The file is the whole model, serialize() makes all of it. The
	// keys are written sorted (in memory the words have the order they
	// were met in), the names in the order of the alphabet, apart from
	// the keys.
	[[nodiscard]] QByteArray serialize() const;

	// All or nothing: a broken file leaves the model empty.
	[[nodiscard]] ParseResult parse(const QByteArray &bytes);

private:
	void reset();
	[[nodiscard]] bool read(const char *data, qsizetype size);
	[[nodiscard]] uint64 key(char type, const QByteArray &bytes) const;
	[[nodiscard]] uint64 wordKey(const QString &word) const;
	[[nodiscard]] uint64 emojiKey(const QString &emoji) const;
	[[nodiscard]] uint64 stickerKey(uint64 id) const;
	[[nodiscard]] uint32 checkValue() const;

	QByteArray _secret;
	QByteArray _salt;
	std::pair<uint64, uint64> _key;

	int _count = 0;
	std::vector<RawName> _senders;
	std::unordered_map<uint64, uint32> _senderIndex;
	std::map<int32, Day> _days;
	Edges _mainEdges;
	Edges _oldEdges;
	Counted<QString> _words;
	Counted<QString> _emoji;
	Counted<uint64> _stickers;
	std::map<int32, MonthBag> _months;
	std::map<uint64, StickerInfo> _stickerInfos;
	Coverage _coverage;

};

struct Options {
	// The start of the period, a local midnight (see PeriodStart), the
	// counters are kept by the day. 0: everything.
	TimeId from = 0;
	TimeId till = 0; // Now.
	bool group = false;
	bool hasOld = false;
	int topWords = 20;
	int topEmoji = 16;
	int topStickers = 8;
	int topDays = 3;
};

struct PersonStats {
	uint64 id = 0;
	QString name; // As it was when the messages were read, may be empty.
	int messages = 0;
	int textMessages = 0;
	int media = 0;
	int64 words = 0;
	int64 chars = 0;
	int answers = 0; // Private chats: how many answers were measured.

	// Their median, in seconds: exact within the first minute, to the
	// minute after it (see AnswerSlot).
	TimeId answerTime = 0;
};

struct TopText {
	QString text;
	int count = 0;

	friend inline bool operator==(const TopText &, const TopText &) = default;
};

struct TopSticker {
	uint64 id = 0;
	int count = 0;
	StickerInfo info;
};

struct DayValue {
	int32 day = 0;
	int count = 0;

	friend inline bool operator==(
		const DayValue &,
		const DayValue &) = default;
};

struct MonthValue {
	int year = 0;
	int month = 0;
	int count = 0;

	friend inline bool operator==(
		const MonthValue &,
		const MonthValue &) = default;
};

struct Streak {
	int32 fromDay = 0;
	int days = 0;

	friend inline bool operator==(const Streak &, const Streak &) = default;
};

struct Report {
	TimeId from = 0;
	TimeId till = 0;
	bool group = false;
	bool complete = false; // The whole period is read.
	bool wholeHistory = false; // The history is read to its start.
	int cached = 0; // Messages in the cache, of any period.

	int messages = 0;
	int textMessages = 0;
	int64 words = 0;
	int64 chars = 0;
	int64 emoji = 0;
	int activeDays = 0;
	int32 firstDay = 0;
	int32 lastDay = 0;

	// The oldest counted message, if the period is read whole and the
	// message is inside of it: the first one of the chat when the whole
	// history is read. Otherwise zeros, only the days are known.
	TimeId firstDate = 0;
	uint64 firstSender = 0;
	TimeId lastDate = 0; // Of the newest counted message.

	std::array<int, 24> hours = {};
	std::array<int, 7> weekdays = {}; // From Monday.
	std::vector<int> days; // From firstDay to lastDay.
	std::vector<MonthValue> months; // Without gaps.
	std::vector<PersonStats> persons; // The most active first.

	// These three are counted by whole months, see FirstCountedMonth().
	std::vector<TopText> topWords;
	std::vector<TopText> topEmoji;
	std::vector<TopSticker> topStickers;
	std::array<int, kKindCount> kinds = {};
	int links = 0;
	int forwards = 0;
	int replies = 0;
	Streak longest;
	Streak current; // Ends today or yesterday, otherwise empty.
	std::vector<DayValue> topDays;
};

[[nodiscard]] Report Analyze(
	const Model &model,
	const Zone &zone,
	const Options &options);

// The words, the emoji and the stickers are counted by months, a period
// may start in the middle of one. That month counts if most of its
// messages in the cache are inside of the period: always when nothing
// older was read, otherwise when the period starts early in the month.
[[nodiscard]] int32 FirstCountedMonth(
	const Model &model,
	const Zone &zone,
	TimeId from);

// The start of a period that ends at `now`: a local midnight.
[[nodiscard]] TimeId PeriodStart(Period period, TimeId now, const Zone &zone);

// The seconds to wait from a FLOOD_WAIT_N error, zero for another one.
[[nodiscard]] int FloodSeconds(const QString &type);

// Whether an answer has moved the reading: a range has grown or got to
// an end. The count of the messages on the server does not matter.
[[nodiscard]] bool ReadingMoved(const Coverage &before, const Coverage &now);

// Whether the newer messages are all read after this answer: it had less
// messages than were asked for (the service ones count), or nothing in it
// was newer than what is read already.
[[nodiscard]] bool HeadChecked(
	const Request &request,
	int returned,
	bool advanced);

// An answer with messages that has not moved the reading: it must stop.
[[nodiscard]] bool ReadingStuck(
	const Request &request,
	int returned,
	bool advanced);

// How much of the reading for a period is done, from 0 to 1.
[[nodiscard]] float64 ReadingProgress(
	const Coverage &coverage,
	TimeId from,
	bool hasOld,
	int messages);

// The cache of one chat with its file. Not thread safe: it is used either
// on the queue of the statistics or (in the self-test) directly.
//
// The file is never appended to: it is replaced as a whole once in a
// while during a reading, when a reading ends and on flush(). The answers
// counted after the last write live only in memory, after a crash they
// are asked for again.
class Storage final {
public:
	// The secret: see Model.
	Storage(
		QString path,
		std::shared_ptr<Zone> zone,
		QByteArray secret = QByteArray());

	struct State {
		Coverage coverage;
		int messages = 0;

		// The file is there, but it could not be read. Nothing is written
		// till reload() reads it or clear() removes it: a cache that took
		// hours to make is not replaced with an empty one.
		bool failed = false;
	};
	struct PageResult {
		State state;
		int accepted = 0; // Messages that were new.
		bool advanced = false; // The range has grown or reached an end.
	};

	State load();
	State reload(); // One more try after a failed load().
	PageResult add(const RawPage &page);
	State finish(TimeId checked);
	void setPeriod(Period period);
	void setStopped(bool stopped);
	void flush(); // Writes what is not in the file yet.
	[[nodiscard]] Report analyze(const Options &options);
	void clear();

	[[nodiscard]] const Model &model() const {
		return _model;
	}
	[[nodiscard]] int writes() const { // Of the whole file, so far.
		return _writes;
	}
	[[nodiscard]] bool unsaved() const {
		return _unsaved;
	}

private:
	[[nodiscard]] State state() const;
	[[nodiscard]] int saveEvery() const;
	void read();
	void save();

	const QString _path;
	const std::shared_ptr<Zone> _zone;
	const QByteArray _secret;
	Model _model;
	int _writes = 0;
	int _answers = 0; // Counted after the last write.
	bool _loaded = false;
	bool _unreadable = false;
	bool _unsaved = false;

};

// Pure logic end: everything below needs an account.

// Whether the statistics can be counted for this chat: a private chat
// with a person or a group the user is in.
[[nodiscard]] bool Supported(not_null<PeerData*> peer);

enum class Stage : uchar {
	Loading, // The cache is being read.
	Idle, // Nothing was asked yet.
	Waiting, // Another chat of this account is read first.
	Reading,
	Flood, // The server asked to wait.
	Stopped, // By the user.
	Failed,
	Done,
};

struct Status {
	Stage stage = Stage::Loading;
	Period period = Period::Months3;
	TimeId from = 0; // The start of the period.
	int messages = 0; // In the cache.
	int total = 0; // In the history as the server says, or 0.
	TimeId oldest = 0; // The date the reading has come to.
	TimeId checked = 0; // When it was last complete.
	float64 progress = 0.; // Of the reading, from 0 to 1.
	bool complete = false; // Everything of the period is in the cache.
	TimeId floodTill = 0;
	QString error; // The type of the error from the server.
	QString waitingFor; // The title of the chat that is read first.

	// Stage::Failed without an error: the cache is there, but it could
	// not be read. A manual refresh() tries again, clear() removes it.
	bool cacheFailed = false;
};

// One for all the chats of an account: whichever of them reads, the
// requests are never closer to each other than the pause between them,
// and a wait asked by the server is kept by all of them.
struct Pace {
	TimeId floodTill = 0; // Unixtime.
	crl::time sentAt = 0; // When the last request was sent.
	crl::time slowTill = 0; // Longer pauses after a wait was asked.
};

// What a tracker asks from the account that owns it.
struct TrackerHost {
	Fn<void()> acquire; // The turn to read: run() or wait() follows.
	Fn<void()> release; // The turn is not needed any more.
	Fn<void()> idle; // Maybe nothing needs this tracker now.
	std::shared_ptr<Pace> pace;
};

// The statistics of one chat of one account: reads the history, keeps
// the cache and counts. Owned by its account, lives while it is shown or
// read. Main thread only.
class Tracker final : public base::has_weak_ptr {
public:
	Tracker(not_null<PeerData*> peer, TrackerHost host);
	~Tracker();

	[[nodiscard]] not_null<PeerData*> peer() const {
		return _peer;
	}
	[[nodiscard]] std::shared_ptr<Zone> zone() const {
		return _zone;
	}

	// Both start with the value of the moment they are subscribed to.
	// A null report: not counted yet.
	[[nodiscard]] rpl::producer<Status> status();
	[[nodiscard]] auto reports()
		-> rpl::producer<std::shared_ptr<const Report>>;

	// Reads what is missing for the chosen period. Without `manual` it
	// does nothing if the user has stopped the reading (now or in an
	// earlier run of the app), if it has failed or if everything was read
	// a moment ago.
	void refresh(bool manual);
	void stop();
	void clear();
	void setPeriod(Period period);

	// While something shows this chat it is kept in memory.
	[[nodiscard]] rpl::lifetime view();

	[[nodiscard]] bool busy() const; // Reading or in the line for it.
	[[nodiscard]] bool viewed() const {
		return _views > 0;
	}

	// Called by the account that owns the tracker.
	void run();
	void wait(const QString &title);

private:
	class Requests;

	[[nodiscard]] TimeId periodFrom() const;
	[[nodiscard]] bool hasOld() const;
	void load(bool again);
	void loaded(Storage::State state);
	[[nodiscard]] Request plan();
	void next();
	void applied(
		int generation,
		Request request,
		int returned,
		Storage::PageResult result);
	void failed(const QString &type);
	[[nodiscard]] crl::time pause() const;
	[[nodiscard]] bool waitForFlood();
	void floodEnded();
	void finished();
	void released(Stage stage);
	void analyze();
	void analyzed(
		int generation,
		std::shared_ptr<const Report> report,
		crl::time spent);
	void push();

	const not_null<PeerData*> _peer;
	const TrackerHost _host;
	const std::shared_ptr<Zone> _zone;
	const std::shared_ptr<Storage> _storage;
	const std::unique_ptr<Requests> _requests;

	Status _status;
	Coverage _coverage;
	rpl::event_stream<Status> _statusChanges;
	std::shared_ptr<const Report> _report;
	rpl::event_stream<std::shared_ptr<const Report>> _reportChanges;

	int _generation = 0;
	int _views = 0;
	bool _wantRefresh = false;
	bool _wantManual = false;
	bool _periodChosen = false;
	bool _stoppedByUser = false;
	bool _headChecked = false;
	bool _headRechecked = false; // Once more at the end of a long run.
	TimeId _headCheckedAt = 0;
	bool _counting = false; // An answer is being counted.
	bool _analyzing = false;
	bool _analyzeAgain = false;
	crl::time _analyzedAt = 0;
	crl::time _analyzeSpent = 0;

};

// The tracker of a chat, created if needed. Null if not Supported().
// For a basic group that became a supergroup it is the supergroup's.
[[nodiscard]] Tracker *TrackerFor(not_null<PeerData*> peer);

// Self-checks for OBLIVION_SELFTEST=chat_stats, see oblivion_selftest.h.
// No Core::App(), no session, no network: pure logic only (counting,
// words, streaks, the cache format).
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::ChatStats

namespace Oblivion {

// Called from Main::Session::finishLogout(): the caches of this account
// are removed.
void ForgetChatStats(not_null<Main::Session*> session);

} // namespace Oblivion
