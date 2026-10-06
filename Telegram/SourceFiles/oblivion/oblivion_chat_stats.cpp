/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_chat_stats.h"

#include "base/random.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "main/main_session.h"
#include "mtproto/mtproto_auth_key.h"
#include "mtproto/sender.h"
#include "storage/storage_account.h"
#include "settings.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRandomGenerator>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QThread>

#include <algorithm>
#include <limits>
#include <numeric>
#include <set>

namespace Oblivion::ChatStats {
namespace {

// Pure logic begin: nothing here needs an account or the network.

constexpr auto kDaySeconds = 86400;
constexpr auto kFileMagic = uint32(0x5453434F);
constexpr auto kFileVersion = uchar(3);
constexpr auto kFileVersionAt = 4;
constexpr auto kFileSizeAt = 5;
constexpr auto kFileChecksumAt = 9;
constexpr auto kFileHeaderSize = 13;
constexpr auto kMaxFileSize = qint64(256) * 1024 * 1024;
constexpr auto kSaltSize = 16;
constexpr auto kReadAttempts = 4;
constexpr auto kReadRetryPause = 80;

// The file is written anew after this many answers, for a large cache
// after a share of what is in it: a long reading does not write the same
// megabytes every minute, a crash loses a little of it.
constexpr auto kSaveAnswers = 20;
constexpr auto kSaveShare = 32;

// A name, a word or an emoji never takes more bytes of UTF-8 than the
// reader of the cache accepts: three for a UTF-16 unit at most.
constexpr auto kMaxNameBytes = 1024;
constexpr auto kMaxNameUnits = kMaxNameBytes / 4;
constexpr auto kMaxWordBytes = 256;
constexpr auto kMaxWordUnits = 64;
constexpr auto kMaxStickerEmojiUnits = 16;
constexpr auto kMinWordLength = 3;
constexpr auto kMaxWordLength = 32;
constexpr auto kMaxMarksInRow = 4;
constexpr auto kMinTopWordCount = uint32(2);
constexpr auto kAnswerMax = TimeId(12 * 3600);
constexpr auto kAnswerExact = 60;

// The days a date of a message can fall on, with any time zone, and the
// months of those days.
constexpr auto kMinDay = int32(-2);
constexpr auto kMaxDay = int32(24858);
constexpr auto kMinMonth = int32(1969 * 12);
constexpr auto kMaxMonth = int32(2039 * 12);
constexpr auto kMaxStep = int64(1) << 20;
constexpr auto kFloodFallback = 30;
constexpr auto kFloodMax = 24 * 3600;

constexpr auto kJoiner = char32_t(0x200D);
constexpr auto kVariation = char32_t(0xFE0F);
constexpr auto kKeycap = char32_t(0x20E3);

[[nodiscard]] int64 FloorDiv(int64 value, int64 by) {
	const auto result = value / by;
	return ((value % by) < 0) ? (result - 1) : result;
}

// Words that say nothing about a chat: pronouns, prepositions, particles
// and the like. Lower case, with "е" in place of "ё". The filler words
// people really type ("короче", "типа") are left to be counted.
constexpr auto kStopWordsRu = ""
	"без более больше будем будет будете будешь будто буду будут бы был "
	"была были было быть вам вами вас ваш ваша ваше ваши вдруг ведь весь "
	"вместо вне во вот впрочем все всегда всего всей всем всеми всему "
	"всех всю вся вы где да даже для до его ее ей ему если есть еще же за "
	"зачем здесь и из или им ими именно иногда их к как какая какие каким "
	"каких какое какой кем когда кого ком кому которая которого которое "
	"которой котором которому которую которые который которым которых "
	"кроме кто куда ли либо лишь между меня мне мной мною мог могла могли "
	"могу могут мое моего моей моем моему может можем можете можешь можно "
	"мои моим моими моих мой мою моя мы на над надо нам нами нас наш наша "
	"наше наши него нее ней нельзя нем нет ни нибудь никогда ним ними них "
	"ничего но ну нужно о об один одна одни одно около он она они оно "
	"опять от очень по под пока после потом потому почему почти при про "
	"просто раз разве с сам сама сами само свое своего своей своем своему "
	"свои своим своими своих свой свою своя себе себя сейчас со собой "
	"собою совсем так такая также такие таким такими таких такого такое "
	"такой такому такую там твое твоего твоей твоем твоему твои твоим "
	"твоих твой твою твоя те тебе тебя тем теми теперь тех то тобой тобою "
	"тогда того тоже той только том тому тот тою ту тут ты у уж уже хоть "
	"хотя чего чей чем через что чтоб чтобы чуть эта эти этим этими этих "
	"это этого этой этом этому этот эту я";

constexpr auto kStopWordsEn = ""
	"a about above after again against all am an and any are aren't as at "
	"be because been before being below between both but by can can't "
	"cannot could couldn't did didn't do does doesn't doing don't down "
	"during each few for from further had hadn't has hasn't have haven't "
	"having he he'd he'll he's her here here's hers herself him himself "
	"his how how's i i'd i'll i'm i've if in into is isn't it it's its "
	"itself let's me more most mustn't my myself no nor not of off on once "
	"only or other ought our ours ourselves out over own same shan't she "
	"she'd she'll she's should shouldn't so some such than that that's the "
	"their theirs them themselves then there there's these they they'd "
	"they'll they're they've this those through to too under until up very "
	"was wasn't we we'd we'll we're we've were weren't what what's when "
	"when's where where's which while who who's whom why why's will with "
	"won't would wouldn't you you'd you'll you're you've your yours "
	"yourself yourselves just also http https www com org net html";

[[nodiscard]] const QSet<QString> &StopWords() {
	static std::once_flag once;
	static QSet<QString> *result = nullptr;
	std::call_once(once, [] {
		auto set = new QSet<QString>();
		for (const auto list : { kStopWordsRu, kStopWordsEn }) {
			const auto words = QString::fromUtf8(list).split(
				QChar(' '),
				Qt::SkipEmptyParts);
			for (const auto &word : words) {
				set->insert(word);
			}
		}
		result = set;
	});
	return *result;
}

struct CodePoint {
	char32_t code = 0;
	int units = 0;
};

[[nodiscard]] CodePoint ReadCode(const QChar *from, const QChar *till) {
	if (from >= till) {
		return {};
	}
	const auto first = from->unicode();
	if (QChar::isHighSurrogate(first)
		&& (from + 1 < till)
		&& QChar::isLowSurrogate(from[1].unicode())) {
		return { QChar::surrogateToUcs4(first, from[1].unicode()), 2 };
	}
	return { char32_t(first), 1 };
}

void AppendCode(QString &to, char32_t code) {
	if (QChar::requiresSurrogates(code)) {
		to.append(QChar(QChar::highSurrogate(code)));
		to.append(QChar(QChar::lowSurrogate(code)));
	} else {
		to.append(QChar(char16_t(code)));
	}
}

// A symbol that takes two code units is not cut in halves: one half is
// written to the cache as another symbol and is read back different.
[[nodiscard]] QString Cut(const QString &text, int units) {
	if (text.size() <= units) {
		return text;
	}
	return text.left(text[units - 1].isHighSurrogate() ? (units - 1) : units);
}

[[nodiscard]] bool Fits(const QString &text, int bytes) {
	return (text.size() * 3 <= bytes) || (text.toUtf8().size() <= bytes);
}

[[nodiscard]] bool IsSkinTone(char32_t code) {
	return (code >= 0x1F3FB) && (code <= 0x1F3FF);
}

[[nodiscard]] bool IsRegional(char32_t code) {
	return (code >= 0x1F1E6) && (code <= 0x1F1FF);
}

[[nodiscard]] bool IsTag(char32_t code) {
	return (code >= 0xE0020) && (code <= 0xE007F);
}

[[nodiscard]] bool IsKeycapBase(char32_t code) {
	return (code >= '0' && code <= '9') || (code == '#') || (code == '*');
}

// Shown as a picture even without the variation selector.
[[nodiscard]] bool IsPictograph(char32_t code) {
	return (code >= 0x1F170 && code <= 0x1FAFF)
		|| (code >= 0x1F000 && code <= 0x1F0FF)
		|| (code >= 0x2600 && code <= 0x27BF)
		|| (code == 0x231A)
		|| (code == 0x231B)
		|| (code >= 0x23E9 && code <= 0x23F3)
		|| (code >= 0x23F8 && code <= 0x23FA)
		|| (code >= 0x2B05 && code <= 0x2B07)
		|| (code == 0x2B1B)
		|| (code == 0x2B1C)
		|| (code == 0x2B50)
		|| (code == 0x2B55);
}

// A text symbol that becomes an emoji with the variation selector.
[[nodiscard]] bool IsTextSymbol(char32_t code) {
	return (code == 0x00A9)
		|| (code == 0x00AE)
		|| (code == 0x203C)
		|| (code == 0x2049)
		|| (code == 0x2122)
		|| (code == 0x2139)
		|| (code >= 0x2194 && code <= 0x2199)
		|| (code == 0x21A9)
		|| (code == 0x21AA)
		|| (code == 0x2328)
		|| (code == 0x23CF)
		|| (code == 0x24C2)
		|| (code == 0x25AA)
		|| (code == 0x25AB)
		|| (code == 0x25B6)
		|| (code == 0x25C0)
		|| (code >= 0x25FB && code <= 0x25FE)
		|| (code == 0x2934)
		|| (code == 0x2935)
		|| (code == 0x3030)
		|| (code == 0x303D)
		|| (code == 0x3297)
		|| (code == 0x3299)
		|| (code >= 0x1F100 && code <= 0x1F16F);
}

// The length in UTF-16 units of the emoji that starts here, or zero.
[[nodiscard]] int EmojiLength(const QChar *from, const QChar *till) {
	const auto first = ReadCode(from, till);
	if (!first.units) {
		return 0;
	}
	auto at = from + first.units;
	if (IsKeycapBase(first.code)) {
		auto next = ReadCode(at, till);
		if (next.units && next.code == kVariation) {
			at += next.units;
			next = ReadCode(at, till);
		}
		return (next.units && next.code == kKeycap)
			? int(at + next.units - from)
			: 0;
	} else if (IsRegional(first.code)) {
		const auto next = ReadCode(at, till);
		return (next.units && IsRegional(next.code))
			? int(at + next.units - from)
			: 0;
	}
	const auto second = ReadCode(at, till);
	const auto varied = second.units && (second.code == kVariation);
	if (!IsPictograph(first.code)
		&& !(varied && IsTextSymbol(first.code))) {
		return 0;
	}
	while (true) {
		while (true) {
			const auto next = ReadCode(at, till);
			if (next.units
				&& (next.code == kVariation
					|| next.code == kKeycap
					|| IsSkinTone(next.code)
					|| IsTag(next.code))) {
				at += next.units;
			} else {
				break;
			}
		}
		const auto joiner = ReadCode(at, till);
		if (!joiner.units || joiner.code != kJoiner) {
			break;
		}
		const auto after = ReadCode(at + joiner.units, till);
		if (!after.units
			|| !(IsPictograph(after.code) || IsTextSymbol(after.code))) {
			break;
		}
		at += joiner.units + after.units;
	}
	return int(at - from);
}

[[nodiscard]] QString WithoutCodes(
		const QString &text,
		bool (*drop)(char32_t)) {
	auto result = QString();
	result.reserve(text.size());
	const auto till = text.constData() + text.size();
	for (auto at = text.constData(); at < till;) {
		const auto code = ReadCode(at, till);
		if (!drop(code.code)) {
			result.append(at, code.units);
		}
		at += code.units;
	}
	return result;
}

// People of any skin tone are one emoji in the top.
[[nodiscard]] QString WithoutSkinTones(const QString &emoji) {
	auto result = WithoutCodes(emoji, &IsSkinTone);
	return result.isEmpty() ? emoji : result;
}

// "ахах", "хахаха", "ахахах" are one word, the same for "haha".
[[nodiscard]] QString NormalizeLaughter(const QString &word) {
	const auto only = [&](QChar a, QChar b, int minimal) {
		if (word.size() < minimal) {
			return false;
		}
		auto hasA = false;
		auto hasB = false;
		for (const auto ch : word) {
			if (ch == a) {
				hasA = true;
			} else if (ch == b) {
				hasB = true;
			} else {
				return false;
			}
		}
		return hasA && hasB;
	};
	if (only(QChar(0x0430), QChar(0x0445), 3)) {
		return QString::fromUtf8("ахаха");
	} else if (only(QChar('h'), QChar('a'), 4)) {
		return u"haha"_q;
	}
	return word;
}

[[nodiscard]] int CountCodes(const QString &text) {
	auto result = 0;
	for (const auto ch : text) {
		if (!ch.isLowSurrogate()) {
			++result;
		}
	}
	return result;
}

void UpdateRange(Range &range, const RawPage &page) {
	if (page.total > 0) {
		range.total = page.total;
	}
	switch (page.type) {
	case PageType::First:
		if (range.started) {
			return;
		}
		range.started = true;
		if (!page.returned || page.maxId <= 0) {
			range.reachedStart = true;
		} else {
			range.low = page.minId;
			range.high = page.maxId;
			range.lowDate = page.minDate;
			range.highDate = page.maxDate;
		}
		break;
	case PageType::Newer:
		if (!range.started || !page.returned || page.maxId <= range.high) {
			return;
		}
		if (range.low <= 0) {
			// The history was empty when it was read.
			range.low = std::max(page.minId, range.high + 1);
			range.lowDate = page.minDate;
		}
		range.high = page.maxId;
		range.highDate = std::max(range.highDate, page.maxDate);
		break;
	case PageType::Older:
		if (!range.started || range.reachedStart) {
			return;
		} else if (!page.returned) {
			range.reachedStart = true;
			return;
		} else if (page.minId <= 0 || page.minId >= range.low) {
			return;
		}
		range.low = page.minId;
		if (page.minDate > 0) {
			range.lowDate = (range.lowDate > 0)
				? std::min(range.lowDate, page.minDate)
				: page.minDate;
		}
		break;
	}
}

template <typename Id>
void MergeCounts(Counts<Id> &into, const Counts<Id> &add) {
	if (add.empty()) {
		return;
	} else if (into.empty()) {
		into = add;
		return;
	} else if (into.back().first < add.front().first) {
		into.insert(end(into), begin(add), end(add));
		return;
	}
	auto merged = Counts<Id>();
	merged.reserve(into.size() + add.size());
	auto a = begin(into);
	auto b = begin(add);
	while (a != end(into) && b != end(add)) {
		if (a->first < b->first) {
			merged.push_back(*a++);
		} else if (b->first < a->first) {
			merged.push_back(*b++);
		} else {
			merged.emplace_back(a->first, a->second + b->second);
			++a;
			++b;
		}
	}
	merged.insert(end(merged), a, end(into));
	merged.insert(end(merged), b, end(add));
	into = std::move(merged);
}

class Writer final {
public:
	void byte(uchar value) {
		_data.append(char(value));
	}
	void varint(uint64 value) {
		while (value >= 0x80) {
			byte(uchar(value & 0x7F) | uchar(0x80));
			value >>= 7;
		}
		byte(uchar(value));
	}
	void zigzag(int64 value) {
		varint((uint64(value) << 1) ^ uint64(value >> 63));
	}
	void text(const QString &value) {
		const auto utf8 = value.toUtf8();
		varint(uint64(utf8.size()));
		_data.append(utf8);
	}
	void raw(const QByteArray &value) {
		_data.append(value);
	}
	void fixed(uint64 value) {
		for (auto i = 0; i != 8; ++i) {
			byte(uchar(value >> (i * 8)));
		}
	}
	[[nodiscard]] QByteArray take() {
		return std::move(_data);
	}

private:
	QByteArray _data;

};

class Reader final {
public:
	Reader(const char *data, qsizetype size)
	: _at(reinterpret_cast<const uchar*>(data))
	, _end(_at + size) {
	}

	[[nodiscard]] bool ok() const {
		return _ok;
	}
	[[nodiscard]] bool finished() const {
		return _ok && (_at == _end);
	}
	void fail() {
		_ok = false;
	}
	[[nodiscard]] uchar byte() {
		if (!_ok || _at == _end) {
			_ok = false;
			return 0;
		}
		return *_at++;
	}
	[[nodiscard]] uint64 varint() {
		auto result = uint64(0);
		for (auto shift = 0; shift < 64; shift += 7) {
			const auto value = byte();
			if (!_ok) {
				return 0;
			}
			result |= uint64(value & 0x7F) << shift;
			if (!(value & 0x80)) {
				return result;
			}
		}
		_ok = false;
		return 0;
	}
	[[nodiscard]] int64 zigzag() {
		const auto value = varint();
		return int64(value >> 1) ^ -int64(value & 1);
	}

	// How many items follow, each of them takes at least `least` bytes.
	[[nodiscard]] int count(int least = 1) {
		const auto value = varint();
		if (!_ok || value > uint64(_end - _at) / uint64(least)) {
			_ok = false;
			return 0;
		}
		return int(value);
	}
	[[nodiscard]] QString text(int limit) {
		const auto size = varint();
		if (!_ok || size > uint64(_end - _at) || size > uint64(limit)) {
			_ok = false;
			return QString();
		}
		const auto result = QString::fromUtf8(
			reinterpret_cast<const char*>(_at),
			qsizetype(size));
		_at += size;
		return result;
	}
	[[nodiscard]] QByteArray raw(int size) {
		if (!_ok || size < 0 || size > _end - _at) {
			_ok = false;
			return QByteArray();
		}
		const auto result = QByteArray(
			reinterpret_cast<const char*>(_at),
			qsizetype(size));
		_at += size;
		return result;
	}
	[[nodiscard]] uint64 fixed() {
		auto result = uint64(0);
		for (auto i = 0; i != 8; ++i) {
			result |= uint64(byte()) << (i * 8);
		}
		return _ok ? result : 0;
	}

	template <typename Type>
	[[nodiscard]] Type integer() {
		const auto value = zigzag();
		if (value < int64(std::numeric_limits<Type>::min())
			|| value > int64(std::numeric_limits<Type>::max())) {
			_ok = false;
			return Type();
		}
		return Type(value);
	}
	template <typename Type>
	[[nodiscard]] Type natural() {
		const auto value = varint();
		if (value > uint64(std::numeric_limits<Type>::max())) {
			_ok = false;
			return Type();
		}
		return Type(value);
	}

private:
	const uchar *_at = nullptr;
	const uchar *_end = nullptr;
	bool _ok = true;

};

[[nodiscard]] uint32 Checksum(const char *data, qsizetype size) {
	auto result = uint32(2166136261U);
	for (auto i = qsizetype(0); i != size; ++i) {
		result ^= uchar(data[i]);
		result *= uint32(16777619U);
	}
	return result;
}

void WriteUint32(QByteArray &to, uint32 value) {
	for (auto i = 0; i != 4; ++i) {
		to.append(char(uchar(value >> (i * 8))));
	}
}

[[nodiscard]] uint32 ReadUint32(const char *data) {
	auto result = uint32(0);
	for (auto i = 0; i != 4; ++i) {
		result |= uint32(uchar(data[i])) << (i * 8);
	}
	return result;
}

void WriteRange(Writer &to, const Range &range) {
	to.zigzag(range.low);
	to.zigzag(range.high);
	to.zigzag(range.lowDate);
	to.zigzag(range.highDate);
	to.varint(uint64(std::max(range.total, 0)));
	to.byte(uchar((range.started ? 1 : 0) | (range.reachedStart ? 2 : 0)));
}

[[nodiscard]] Range ReadRange(Reader &from) {
	auto result = Range();
	result.low = from.integer<int32>();
	result.high = from.integer<int32>();
	result.lowDate = from.integer<TimeId>();
	result.highDate = from.integer<TimeId>();
	result.total = from.natural<int>();
	const auto flags = from.byte();
	result.started = (flags & 1) != 0;
	result.reachedStart = (flags & 2) != 0;
	return result;
}

template <typename Id>
void WriteCounts(Writer &to, const Counts<Id> &list) {
	to.varint(uint64(list.size()));
	auto previous = Id(0);
	for (const auto &[id, count] : list) {
		to.varint(uint64(id - previous));
		to.varint(count);
		previous = id;
	}
}

template <typename Id>
[[nodiscard]] Counts<Id> ReadCounts(Reader &from) {
	auto result = Counts<Id>();
	const auto count = from.count(2);
	result.reserve(count);
	auto previous = uint64(0);
	for (auto i = 0; i != count && from.ok(); ++i) {
		const auto step = from.varint();
		const auto value = from.natural<uint32>();
		const auto id = previous + step;
		if (id < previous
			|| id > uint64(std::numeric_limits<Id>::max())
			|| (i && !step)) {
			from.fail();
			return Counts<Id>();
		}
		result.emplace_back(Id(id), value);
		previous = id;
	}
	return result;
}

// Only the counters that are not zero, each one with its place.
template <size_t Size>
void WriteSparse(Writer &to, const std::array<uint32, Size> &list) {
	auto count = 0;
	for (const auto value : list) {
		if (value) {
			++count;
		}
	}
	to.byte(uchar(count));
	for (auto i = size_t(0); i != Size; ++i) {
		if (list[i]) {
			to.byte(uchar(i));
			to.varint(list[i]);
		}
	}
}

template <size_t Size>
void ReadSparse(Reader &from, std::array<uint32, Size> &list) {
	const auto count = int(from.byte());
	auto previous = -1;
	for (auto i = 0; i != count && from.ok(); ++i) {
		const auto place = int(from.byte());
		const auto value = from.natural<uint32>();
		if (place <= previous || place >= int(Size) || !value) {
			from.fail();
			return;
		}
		list[place] = value;
		previous = place;
	}
}

void WriteEdge(Writer &to, const Edge &edge) {
	to.byte(edge.known ? 1 : 0);
	if (edge.known) {
		to.zigzag(edge.date);
		to.varint(edge.sender);
	}
}

[[nodiscard]] Edge ReadEdge(Reader &from, size_t senders) {
	auto result = Edge();
	const auto known = from.byte();
	if (known > 1) {
		from.fail();
	} else if (known) {
		result.known = true;
		result.date = from.integer<TimeId>();
		result.sender = from.natural<uint32>();
		if (result.date < 0 || result.sender >= senders) {
			from.fail();
		}
	}
	return from.ok() ? result : Edge();
}

void WriteDay(Writer &to, const Day &day) {
	WriteSparse(to, day.hours);
	WriteSparse(to, day.kinds);
	to.varint(day.links);
	to.varint(day.forwards);
	to.varint(day.replies);
	to.varint(day.emoji);
	to.varint(uint64(day.persons.size()));
	auto previous = uint32(0);
	for (const auto &person : day.persons) {
		to.varint(person.sender - previous);
		to.varint(person.messages);
		to.varint(person.textMessages);
		to.varint(person.media);
		to.varint(person.words);
		to.varint(person.chars);
		WriteCounts(to, person.answers);
		previous = person.sender;
	}
}

[[nodiscard]] Day ReadDay(Reader &from, size_t senders) {
	auto result = Day();
	ReadSparse(from, result.hours);
	ReadSparse(from, result.kinds);
	result.links = from.natural<uint32>();
	result.forwards = from.natural<uint32>();
	result.replies = from.natural<uint32>();
	result.emoji = from.natural<uint32>();
	const auto persons = from.count(7);
	result.persons.reserve(persons);
	auto previous = uint64(0);
	for (auto i = 0; i != persons && from.ok(); ++i) {
		const auto step = from.varint();
		const auto sender = previous + step;
		if (sender < previous || sender >= senders || (i && !step)) {
			from.fail();
			break;
		}
		auto person = PersonDay();
		person.sender = uint32(sender);
		person.messages = from.natural<uint32>();
		person.textMessages = from.natural<uint32>();
		person.media = from.natural<uint32>();
		person.words = from.natural<uint32>();
		person.chars = from.natural<uint32>();
		person.answers = ReadCounts<uint16>(from);
		if (!person.answers.empty()
			&& person.answers.back().first >= kAnswerSlots) {
			from.fail();
			break;
		}
		result.persons.push_back(std::move(person));
		previous = sender;
	}
	return from.ok() ? result : Day();
}

[[nodiscard]] PersonDay *FindPerson(Day &day, uint32 sender) {
	const auto i = std::lower_bound(
		begin(day.persons),
		end(day.persons),
		sender,
		[](const PersonDay &person, uint32 value) {
			return person.sender < value;
		});
	return (i != end(day.persons) && i->sender == sender) ? &*i : nullptr;
}

[[nodiscard]] PersonDay &PersonOf(Day &day, uint32 sender) {
	const auto i = std::lower_bound(
		begin(day.persons),
		end(day.persons),
		sender,
		[](const PersonDay &person, uint32 value) {
			return person.sender < value;
		});
	if (i != end(day.persons) && i->sender == sender) {
		return *i;
	}
	auto added = PersonDay();
	added.sender = sender;
	return *day.persons.insert(i, std::move(added));
}

// A wait longer than kAnswerMax is not an answer, it is a new talk.
void AddAnswer(PersonDay &person, TimeId waited) {
	if (waited < 0 || waited > kAnswerMax) {
		return;
	}
	const auto slot = uint16(AnswerSlot(waited));
	const auto i = std::lower_bound(
		begin(person.answers),
		end(person.answers),
		slot,
		[](const std::pair<uint16, uint32> &entry, uint16 value) {
			return entry.first < value;
		});
	if (i != end(person.answers) && i->first == slot) {
		++i->second;
	} else {
		person.answers.insert(i, std::pair<uint16, uint32>(slot, 1));
	}
}

[[nodiscard]] uint32 Plus(uint32 a, uint32 b) {
	const auto limit = std::numeric_limits<uint32>::max();
	return (b > limit - a) ? limit : (a + b);
}

using HashKey = std::pair<uint64, uint64>;

[[nodiscard]] uint64 ReadUint64(const char *data) {
	auto result = uint64(0);
	for (auto i = 0; i != 8; ++i) {
		result |= uint64(uchar(data[i])) << (i * 8);
	}
	return result;
}

[[nodiscard]] QByteArray RandomSalt() {
	auto to = Writer();
	for (auto i = 0; i != kSaltSize / 8; ++i) {
		to.fixed(QRandomGenerator::system()->generate64());
	}
	return to.take();
}

[[nodiscard]] HashKey MakeHashKey(
		const QByteArray &secret,
		const QByteArray &salt) {
	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	hash.addData(QByteArray("oblivion chat stats keys"));
	hash.addData(salt);
	hash.addData(secret);
	const auto digest = hash.result();
	return {
		ReadUint64(digest.constData()),
		ReadUint64(digest.constData() + 8),
	};
}

[[nodiscard]] uint64 RotateLeft(uint64 value, int bits) {
	return (value << bits) | (value >> (64 - bits));
}

// SipHash-2-4: without the key the result can't be told for any input,
// the short ones as well.
[[nodiscard]] uint64 KeyedHash(HashKey key, const QByteArray &bytes) {
	auto v0 = key.first ^ 0x736F6D6570736575ULL;
	auto v1 = key.second ^ 0x646F72616E646F6DULL;
	auto v2 = key.first ^ 0x6C7967656E657261ULL;
	auto v3 = key.second ^ 0x7465646279746573ULL;
	const auto mix = [&] {
		v0 += v1;
		v1 = RotateLeft(v1, 13);
		v1 ^= v0;
		v0 = RotateLeft(v0, 32);
		v2 += v3;
		v3 = RotateLeft(v3, 16);
		v3 ^= v2;
		v0 += v3;
		v3 = RotateLeft(v3, 21);
		v3 ^= v0;
		v2 += v1;
		v1 = RotateLeft(v1, 17);
		v1 ^= v2;
		v2 = RotateLeft(v2, 32);
	};
	const auto data = bytes.constData();
	const auto size = bytes.size();
	const auto part = [&](qsizetype from, qsizetype count) {
		auto result = uint64(0);
		for (auto i = qsizetype(0); i != count; ++i) {
			result |= uint64(uchar(data[from + i])) << (i * 8);
		}
		return result;
	};
	auto at = qsizetype(0);
	for (; at + 8 <= size; at += 8) {
		const auto block = part(at, 8);
		v3 ^= block;
		mix();
		mix();
		v0 ^= block;
	}
	const auto last = part(at, size - at) | (uint64(size & 0xFF) << 56);
	v3 ^= last;
	mix();
	mix();
	v0 ^= last;
	v2 ^= 0xFF;
	mix();
	mix();
	mix();
	mix();
	return v0 ^ v1 ^ v2 ^ v3;
}

// The place of the key in the list, a new one is added with nothing
// counted yet.
template <typename Name>
[[nodiscard]] uint32 IndexOf(Counted<Name> &list, uint64 key) {
	const auto i = list.index.find(key);
	if (i != end(list.index)) {
		return i->second;
	}
	const auto index = uint32(list.keys.size());
	list.index.emplace(key, index);
	list.keys.push_back(key);
	list.totals.push_back(0);
	list.names.push_back(Name());
	return index;
}

// One more is met. True if that many are enough for a name.
template <typename Name>
[[nodiscard]] bool CountOne(Counted<Name> &list, uint32 index) {
	auto &total = list.totals[index];
	total = Plus(total, 1);
	return (total >= kNamedAfter);
}

// In memory a word has the number it was met under, and the first answers
// are mostly new words. The file gets the keys sorted: their order says
// nothing.
template <typename Name>
[[nodiscard]] std::vector<uint32> KeyOrder(const Counted<Name> &list) {
	auto order = std::vector<uint32>(list.keys.size());
	std::iota(begin(order), end(order), uint32(0));
	std::sort(begin(order), end(order), [&](uint32 a, uint32 b) {
		return list.keys[a] < list.keys[b];
	});
	return order;
}

[[nodiscard]] std::vector<uint32> Places(const std::vector<uint32> &order) {
	auto result = std::vector<uint32>(order.size());
	for (auto i = uint32(0); i != order.size(); ++i) {
		result[order[i]] = i;
	}
	return result;
}

[[nodiscard]] Counts<uint32> Moved(
		const Counts<uint32> &list,
		const std::vector<uint32> &place) {
	auto result = Counts<uint32>();
	result.reserve(list.size());
	for (const auto &[id, count] : list) {
		if (id < place.size()) {
			result.emplace_back(place[id], count);
		}
	}
	std::sort(begin(result), end(result));
	return result;
}

template <typename Name>
void WriteKeys(
		Writer &to,
		const Counted<Name> &list,
		const std::vector<uint32> &order) {
	to.varint(uint64(order.size()));
	for (const auto index : order) {
		to.fixed(list.keys[index]);
	}
}

template <typename Name>
void ReadKeys(Reader &from, Counted<Name> &list) {
	const auto count = from.count(8);
	list.keys.reserve(count);
	list.totals.reserve(count);
	list.names.reserve(count);
	for (auto i = 0; i != count && from.ok(); ++i) {
		const auto key = from.fixed();
		if (i && key <= list.keys.back()) {
			from.fail();
			return;
		}
		list.index.emplace(key, uint32(i));
		list.keys.push_back(key);
		list.totals.push_back(0);
		list.names.push_back(Name());
	}
}

// The counters of one month for a list, read and added to its totals.
template <typename Name>
[[nodiscard]] Counts<uint32> ReadCounted(Reader &from, Counted<Name> &list) {
	auto result = ReadCounts<uint32>(from);
	if (!result.empty() && result.back().first >= list.keys.size()) {
		from.fail();
		return Counts<uint32>();
	}
	for (const auto &[id, count] : result) {
		list.totals[id] = Plus(list.totals[id], count);
	}
	return result;
}

void WriteStickerId(QByteArray &to, uint64 id) {
	for (auto i = 0; i != 8; ++i) {
		to.append(char(uchar(id >> (i * 8))));
	}
}

// The file: what it is, its version, the size and the checksum of the
// rest.
[[nodiscard]] QByteArray WrapFile(const QByteArray &payload) {
	auto result = QByteArray();
	result.reserve(kFileHeaderSize + payload.size());
	WriteUint32(result, kFileMagic);
	result.append(char(kFileVersion));
	WriteUint32(result, uint32(payload.size()));
	WriteUint32(result, Checksum(payload.constData(), payload.size()));
	result.append(payload);
	return result;
}

// The caches of the versions before this one had a row for every message
// and the words as they are (the first one in the order of the
// messages). Their files are removed even if their chats are never opened
// again, nothing else in the folder is touched.
void RemoveOutdatedCaches(const QString &folder) {
	const auto directory = QDir(folder);
	const auto names = directory.entryList(
		QStringList{ u"*.dat"_q },
		QDir::Files);
	for (const auto &name : names) {
		const auto path = directory.filePath(name);
		auto file = QFile(path);
		if (!file.open(QIODevice::ReadOnly)) {
			continue;
		}
		const auto header = file.read(kFileVersionAt + 1);
		file.close();
		if (header.size() == kFileVersionAt + 1
			&& ReadUint32(header.constData()) == kFileMagic
			&& uchar(header[kFileVersionAt]) < kFileVersion) {
			QFile::remove(path);
		}
	}
}

[[nodiscard]] int MonthDays(int year, int month) {
	constexpr int kDays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	const auto leap = (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
	return (month == 2 && leap) ? 29 : kDays[std::clamp(month, 1, 12) - 1];
}

} // namespace

Zone::Zone(QTimeZone zone)
: _zone(std::move(zone)) {
}

std::shared_ptr<Zone> Zone::System() {
	return std::make_shared<Zone>(QTimeZone::systemTimeZone());
}

std::shared_ptr<Zone> Zone::Fixed(int secondsAheadOfUtc) {
	return std::make_shared<Zone>(
		QTimeZone::fromSecondsAheadOfUtc(secondsAheadOfUtc));
}

Zone::Offsets Zone::offsets(int32 utcDay) const {
	const auto lock = std::unique_lock(_mutex);
	const auto i = _cache.find(utcDay);
	if (i != end(_cache)) {
		return i->second;
	}
	const auto at = [&](qint64 seconds) {
		return _zone.isValid()
			? _zone.offsetFromUtc(
				QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::utc()))
			: 0;
	};
	const auto start = qint64(utcDay) * kDaySeconds;
	const auto result = Offsets{ at(start), at(start + kDaySeconds - 1) };
	_cache.emplace(utcDay, result);
	return result;
}

int Zone::offset(TimeId when) const {
	const auto known = offsets(int32(FloorDiv(when, kDaySeconds)));
	if (known.start == known.end) {
		return known.start;
	}
	// The clocks are changed on this day.
	const auto lock = std::unique_lock(_mutex);
	return _zone.offsetFromUtc(
		QDateTime::fromSecsSinceEpoch(when, QTimeZone::utc()));
}

int32 Zone::day(TimeId when) const {
	return int32(FloorDiv(int64(when) + offset(when), kDaySeconds));
}

TimeId Zone::dayStart(int32 day) const {
	const auto local = int64(day) * kDaySeconds;
	const auto limit = [](int64 value) {
		return TimeId(std::clamp(
			value,
			int64(0),
			int64(std::numeric_limits<TimeId>::max())));
	};
	const auto guess = limit(local - offset(limit(local)));
	return limit(local - offset(guess));
}

CivilDate DateOfDay(int32 day) {
	const auto z = int64(day) + 719468;
	const auto era = ((z >= 0) ? z : (z - 146096)) / 146097;
	const auto doe = z - era * 146097;
	const auto yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const auto mp = (5 * doy + 2) / 153;
	const auto month = (mp < 10) ? (mp + 3) : (mp - 9);
	return {
		.year = int(yoe + era * 400 + ((month <= 2) ? 1 : 0)),
		.month = int(month),
		.day = int(doy - (153 * mp + 2) / 5 + 1),
	};
}

int32 DayOfDate(CivilDate date) {
	const auto y = int64(date.year) - ((date.month <= 2) ? 1 : 0);
	const auto era = ((y >= 0) ? y : (y - 399)) / 400;
	const auto yoe = y - era * 400;
	const auto mp = date.month + ((date.month > 2) ? -3 : 9);
	const auto doy = (153 * mp + 2) / 5 + date.day - 1;
	const auto doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return int32(era * 146097 + doe - 719468);
}

int WeekdayOfDay(int32 day) {
	// 1970-01-01 was a Thursday.
	return int((((int64(day) + 3) % 7) + 7) % 7);
}

int32 MonthOfDay(int32 day) {
	const auto date = DateOfDay(day);
	return int32(date.year * 12 + (date.month - 1));
}

int32 FirstDayOfMonth(int32 month) {
	const auto year = FloorDiv(month, 12);
	return DayOfDate({
		.year = int(year),
		.month = int(month - year * 12) + 1,
		.day = 1,
	});
}

bool IsStopWord(const QString &word) {
	return StopWords().contains(word);
}

QString EmojiKey(const QString &emoji) {
	return WithoutCodes(emoji, [](char32_t code) {
		return (code == kVariation);
	});
}

TextStats ParseText(const QString &text, const std::vector<TextSkip> &skip) {
	auto result = TextStats();
	const auto size = int(text.size());
	const auto data = text.constData();
	const auto till = data + size;

	auto ranges = skip;
	std::sort(begin(ranges), end(ranges), [](TextSkip a, TextSkip b) {
		return a.offset < b.offset;
	});
	auto range = begin(ranges);
	const auto skipped = [&](int index) {
		while (range != end(ranges)
			&& range->offset + range->length <= index) {
			++range;
		}
		return (range != end(ranges)) && (range->offset <= index);
	};

	auto token = QString();
	auto tokenCodes = 0;
	auto tokenMarks = 0;
	const auto flush = [&] {
		while (token.endsWith(QChar('\''))) {
			token.chop(1);
			--tokenCodes;
		}
		tokenMarks = 0;
		if (token.isEmpty()) {
			tokenCodes = 0;
			return;
		}
		++result.words;

		// The marks above and below the letters are not counted as
		// letters, so the size is checked as well: a counted word is
		// always one the cache can keep.
		if (tokenCodes >= kMinWordLength
			&& tokenCodes <= kMaxWordLength
			&& token.size() <= kMaxWordUnits) {
			auto word = NormalizeLaughter(token);
			if (!IsStopWord(word)) {
				result.tokens.push_back(std::move(word));
			}
		}
		token = QString();
		tokenCodes = 0;
	};
	const auto letterAt = [&](int index) {
		const auto code = ReadCode(data + index, till);
		return code.units && QChar::isLetter(code.code);
	};
	for (auto i = 0; i < size;) {
		const auto code = ReadCode(data + i, till);
		if (skipped(i)) {
			flush();
			++result.length;
			i += code.units;
			continue;
		}
		if (const auto length = EmojiLength(data + i, till)) {
			flush();
			++result.length;
			result.emoji.push_back(WithoutSkinTones(text.mid(i, length)));
			i += length;
			continue;
		}
		++result.length;
		if (QChar::isLetter(code.code)) {
			auto lower = QChar::toLower(code.code);
			if (lower == 0x0451) {
				lower = 0x0435;
			}
			AppendCode(token, lower);
			++tokenCodes;
			tokenMarks = 0;
		} else if (!token.isEmpty() && QChar::isMark(code.code)) {
			// A letter of a real word has a few marks, the heaps of them
			// ("zalgo" texts) are dropped: the word stays a word.
			if (tokenMarks < kMaxMarksInRow) {
				AppendCode(token, code.code);
				++tokenMarks;
			}
		} else if ((code.code == '\'' || code.code == 0x2019)
			&& !token.isEmpty()
			&& !token.endsWith(QChar('\''))
			&& letterAt(i + code.units)) {
			token.append(QChar('\''));
			++tokenCodes;
			tokenMarks = 0;
		} else {
			flush();
		}
		i += code.units;
	}
	flush();
	return result;
}

Request NextRequest(const Coverage &coverage, const PlanArgs &args) {
	const auto limit = std::clamp(args.limit, 1, kPageLimit);
	const auto first = [&](bool old) {
		return Request{
			.valid = true,
			.old = old,
			.type = PageType::First,
			.offsetId = 0,
			.addOffset = 0,
			.limit = limit,
		};
	};
	const auto older = [&](bool old, const Range &range) {
		return Request{
			.valid = true,
			.old = old,
			.type = PageType::Older,
			.offsetId = range.low,
			.addOffset = 0,
			.limit = limit,
		};
	};
	const auto wanted = [&](const Range &range) {
		return range.started
			&& !range.reachedStart
			&& (args.from <= 0
				|| range.lowDate <= 0
				|| range.lowDate >= args.from);
	};
	const auto &main = coverage.main;
	if (!main.started) {
		return first(false);
	} else if (!args.headChecked) {
		return Request{
			.valid = true,
			.old = false,
			.type = PageType::Newer,
			.offsetId = main.high + 1,
			.addOffset = -limit,
			.limit = limit,
		};
	} else if (wanted(main)) {
		return older(false, main);
	} else if (!args.hasOld || !main.reachedStart) {
		return Request();
	} else if (args.from > 0
		&& main.lowDate > 0
		&& main.lowDate < args.from) {
		return Request();
	}
	const auto &old = coverage.old;
	if (!old.started) {
		return first(true);
	} else if (wanted(old)) {
		return older(true, old);
	}
	return Request();
}

bool Covers(const Coverage &coverage, TimeId from, bool hasOld) {
	const auto before = [&](const Range &range) {
		return (from > 0) && (range.lowDate > 0) && (range.lowDate < from);
	};
	const auto enough = [&](const Range &range) {
		return range.started && (range.reachedStart || before(range));
	};
	const auto &main = coverage.main;
	if (!enough(main)) {
		return false;
	} else if (!hasOld || !main.reachedStart || before(main)) {
		return true;
	}
	return enough(coverage.old);
}

bool ReadingMoved(const Coverage &before, const Coverage &now) {
	const auto moved = [](const Range &a, const Range &b) {
		return (a.low != b.low)
			|| (a.high != b.high)
			|| (a.started != b.started)
			|| (a.reachedStart != b.reachedStart);
	};
	return moved(before.main, now.main) || moved(before.old, now.old);
}

bool HeadChecked(const Request &request, int returned, bool advanced) {
	return !request.old
		&& ((request.type == PageType::First)
			|| (request.type == PageType::Newer
				&& (returned < request.limit || !advanced)));
}

bool ReadingStuck(const Request &request, int returned, bool advanced) {
	return (request.type == PageType::Older) && (returned > 0) && !advanced;
}

float64 ReadingProgress(
		const Coverage &coverage,
		TimeId from,
		bool hasOld,
		int messages) {
	const auto &main = coverage.main;
	const auto &old = coverage.old;
	if (Covers(coverage, from, hasOld)) {
		return 1.;
	} else if (!main.started) {
		return 0.;
	} else if (from > 0) {
		const auto newest = main.highDate;
		const auto oldest = (hasOld && old.started && old.lowDate > 0)
			? old.lowDate
			: main.lowDate;
		if (newest <= from || oldest <= 0) {
			return 0.;
		}
		return std::clamp(
			(newest - oldest) / float64(newest - from),
			0.,
			0.99);
	}
	const auto total = int64(main.total) + (hasOld ? old.total : 0);
	if (total <= 0) {
		return 0.;
	}
	return std::clamp(messages / float64(total), 0., 0.99);
}

static_assert(kAnswerSlots == kAnswerExact + kAnswerMax / 60);

int AnswerSlot(TimeId seconds) {
	const auto value = std::clamp(seconds, TimeId(0), kAnswerMax);
	return (value < kAnswerExact)
		? int(value)
		: (kAnswerExact - 1 + int((value + 30) / 60));
}

TimeId AnswerSlotTime(int slot) {
	const auto value = std::clamp(slot, 0, kAnswerSlots - 1);
	return (value < kAnswerExact)
		? TimeId(value)
		: TimeId((value - kAnswerExact + 1) * 60);
}

int DayMessages(const Day &day) {
	auto result = int64(0);
	for (const auto count : day.hours) {
		result += count;
	}
	return int(std::min(result, int64(std::numeric_limits<int>::max())));
}

Model::Model(QByteArray secret)
: _secret(std::move(secret))
, _salt(RandomSalt())
, _key(MakeHashKey(_secret, _salt)) {
}

void Model::reset() {
	*this = Model(_secret);
}

uint64 Model::key(char type, const QByteArray &bytes) const {
	return KeyedHash(_key, QByteArray(1, type) + bytes);
}

uint64 Model::wordKey(const QString &word) const {
	return key('w', word.toUtf8());
}

// The same emoji typed with and without the variation selector is one
// emoji.
uint64 Model::emojiKey(const QString &emoji) const {
	return key('e', EmojiKey(emoji).toUtf8());
}

uint64 Model::stickerKey(uint64 id) const {
	auto bytes = QByteArray();
	WriteStickerId(bytes, id);
	return key('s', bytes);
}

// Tells a file made with another secret from a file of this one.
uint32 Model::checkValue() const {
	return uint32(key('c', QByteArray()) & 0xFFFFFFFFU);
}

int Model::add(const RawPage &page, const Zone &zone) {
	auto &range = page.old ? _coverage.old : _coverage.main;
	const auto before = range;
	UpdateRange(range, page);

	const auto accepted = [&](int32 id) {
		if (id <= 0) {
			return false;
		}
		switch (page.type) {
		case PageType::First: return !before.started;
		case PageType::Newer: return before.started && (id > before.high);
		case PageType::Older:
			return before.started && (before.low > 0) && (id < before.low);
		}
		return false;
	};
	auto list = std::vector<const RawMessage*>();
	list.reserve(page.messages.size());
	for (const auto &message : page.messages) {
		if (accepted(message.id)) {
			list.push_back(&message);
		}
	}
	std::sort(begin(list), end(list), [](
			const RawMessage *a,
			const RawMessage *b) {
		return a->id < b->id;
	});
	list.erase(
		std::unique(begin(list), end(list), [](
				const RawMessage *a,
				const RawMessage *b) {
			return a->id == b->id;
		}),
		end(list));
	if (list.empty()) {
		return 0;
	}

	auto names = std::unordered_map<uint64, QString>();
	for (const auto &name : page.names) {
		if (!name.name.isEmpty()) {
			names[name.id] = Cut(name.name, kMaxNameUnits);
		}
	}
	const auto senderIndex = [&](uint64 id) {
		const auto name = names.find(id);
		const auto known = _senderIndex.find(id);
		if (known != end(_senderIndex)) {
			if (name != end(names)) {
				_senders[known->second].name = name->second;
			}
			return known->second;
		}
		const auto index = uint32(_senders.size());
		_senderIndex.emplace(id, index);
		_senders.push_back({
			id,
			(name != end(names)) ? name->second : QString(),
		});
		return index;
	};

	struct Bag {
		std::map<uint32, uint32> words;
		std::map<uint32, uint32> emoji;
		std::map<uint32, uint32> stickers;
	};
	auto bags = std::map<int32, Bag>();
	auto &edges = page.old ? _oldEdges : _mainEdges;

	// The messages go from the oldest one. The first of the newer ones
	// may be an answer to the newest message counted before.
	const auto answers = page.direct;
	auto previous = (answers && page.type == PageType::Newer)
		? edges.newest
		: Edge();
	auto first = Edge();
	auto last = Edge();
	for (const auto raw : list) {
		// Only the numbers are taken from the text, and its words one by
		// one, to the counters of the month.
		const auto stats = ParseText(raw->text, raw->skip);
		const auto sender = senderIndex(raw->sender);
		const auto date = std::max(raw->date, TimeId(0));
		const auto kind = (uchar(raw->kind) < kKindCount)
			? raw->kind
			: Kind::Other;
		const auto local = zone.day(date);
		const auto seconds = int64(date)
			+ zone.offset(date)
			- int64(local) * kDaySeconds;
		const auto hour = int(std::clamp(seconds / 3600, int64(0), int64(23)));

		auto &day = _days[local];
		++day.hours[hour];
		++day.kinds[int(kind)];
		if (raw->flags & kLink) {
			++day.links;
		}
		if (raw->flags & kForwarded) {
			++day.forwards;
		}
		if (raw->flags & kReply) {
			++day.replies;
		}
		day.emoji = Plus(day.emoji, uint32(stats.emoji.size()));
		auto &person = PersonOf(day, sender);
		++person.messages;
		if (stats.length > 0) {
			++person.textMessages;
		}
		if (kind != Kind::Text) {
			++person.media;
		}
		person.words = Plus(person.words, uint32(stats.words));
		person.chars = Plus(person.chars, uint32(stats.length));
		if (answers && previous.known && previous.sender != sender) {
			AddAnswer(person, date - previous.date);
		}
		previous = Edge{ .date = date, .sender = sender, .known = true };
		if (!first.known) {
			first = previous;
		}
		last = previous;
		if (_count < std::numeric_limits<int>::max()) {
			++_count;
		}

		const auto sticker = (kind == Kind::Sticker) && raw->sticker;
		if (stats.tokens.empty() && stats.emoji.empty() && !sticker) {
			continue;
		}
		auto &bag = bags[MonthOfDay(local)];
		for (const auto &word : stats.tokens) {
			if (!Fits(word, kMaxWordBytes)) {
				continue;
			}
			const auto index = IndexOf(_words, wordKey(word));
			++bag.words[index];
			if (CountOne(_words, index) && _words.names[index].isEmpty()) {
				_words.names[index] = word;
			}
		}
		for (const auto &emoji : stats.emoji) {
			if (!Fits(emoji, kMaxWordBytes)) {
				continue;
			}
			const auto index = IndexOf(_emoji, emojiKey(emoji));
			++bag.emoji[index];
			if (CountOne(_emoji, index) && _emoji.names[index].isEmpty()) {
				_emoji.names[index] = emoji;
			}
		}
		if (sticker) {
			const auto index = IndexOf(_stickers, stickerKey(raw->sticker));
			++bag.stickers[index];
			if (CountOne(_stickers, index)) {
				// The message to load the picture from, kept from the
				// moment the sticker is named: of the supergroup rather
				// than of the old chat, then a newer one.
				const auto i = _stickerInfos.find(raw->sticker);
				const auto fresh = (i == end(_stickerInfos))
					|| ((i->second.old != page.old)
						? i->second.old
						: (raw->id > i->second.msgId));
				_stickers.names[index] = raw->sticker;
				if (fresh) {
					_stickerInfos[raw->sticker] = StickerInfo{
						.msgId = raw->id,
						.old = page.old,
						.emoji = Cut(
							raw->stickerEmoji,
							kMaxStickerEmojiUnits),
					};
				}
			}
		}
	}

	// The oldest message counted before may be an answer to the last of
	// these older ones.
	if (answers
		&& page.type == PageType::Older
		&& edges.oldest.known
		&& edges.oldest.sender != last.sender) {
		const auto i = _days.find(zone.day(edges.oldest.date));
		const auto person = (i != end(_days))
			? FindPerson(i->second, edges.oldest.sender)
			: nullptr;
		if (person) {
			AddAnswer(*person, edges.oldest.date - last.date);
		}
	}
	if (page.type != PageType::Newer || !edges.oldest.known) {
		edges.oldest = first;
	}
	if (page.type != PageType::Older || !edges.newest.known) {
		edges.newest = last;
	}

	for (const auto &[month, bag] : bags) {
		auto &mine = _months[month];
		MergeCounts(
			mine.words,
			Counts<uint32>(begin(bag.words), end(bag.words)));
		MergeCounts(
			mine.emoji,
			Counts<uint32>(begin(bag.emoji), end(bag.emoji)));
		MergeCounts(
			mine.stickers,
			Counts<uint32>(begin(bag.stickers), end(bag.stickers)));
	}
	return int(list.size());
}

void Model::setChecked(TimeId checked) {
	_coverage.checked = checked;
}

void Model::setPeriod(Period period) {
	_coverage.period = period;
}

void Model::setStopped(bool stopped) {
	_coverage.stopped = stopped;
}

QByteArray Model::serialize() const {
	const auto wordOrder = KeyOrder(_words);
	const auto emojiOrder = KeyOrder(_emoji);
	const auto stickerOrder = KeyOrder(_stickers);
	const auto wordPlace = Places(wordOrder);
	const auto emojiPlace = Places(emojiOrder);
	const auto stickerPlace = Places(stickerOrder);

	auto to = Writer();
	to.raw(_salt);
	to.varint(checkValue());
	WriteRange(to, _coverage.main);
	WriteRange(to, _coverage.old);
	to.zigzag(_coverage.checked);
	to.byte(uchar(_coverage.period));
	to.byte(_coverage.stopped ? 1 : 0);

	to.varint(uint64(_senders.size()));
	for (const auto &name : _senders) {
		to.varint(name.id);
		to.text(Cut(name.name, kMaxNameUnits));
	}
	WriteEdge(to, _mainEdges.oldest);
	WriteEdge(to, _mainEdges.newest);
	WriteEdge(to, _oldEdges.oldest);
	WriteEdge(to, _oldEdges.newest);

	to.varint(uint64(_days.size()));
	auto previousDay = int64(0);
	for (const auto &[day, data] : _days) {
		to.zigzag(day - previousDay);
		WriteDay(to, data);
		previousDay = day;
	}

	WriteKeys(to, _words, wordOrder);
	WriteKeys(to, _emoji, emojiOrder);
	WriteKeys(to, _stickers, stickerOrder);
	to.varint(uint64(_months.size()));
	auto previousMonth = int64(0);
	for (const auto &[month, bag] : _months) {
		to.zigzag(month - previousMonth);
		WriteCounts(to, Moved(bag.words, wordPlace));
		WriteCounts(to, Moved(bag.emoji, emojiPlace));
		WriteCounts(to, Moved(bag.stickers, stickerPlace));
		previousMonth = month;
	}

	// The names stand apart from the keys, in the order of the alphabet:
	// which counters are whose is found only with the secret.
	const auto named = [&](const Counted<QString> &list) {
		auto names = std::vector<QString>();
		for (const auto &name : list.names) {
			if (!name.isEmpty()) {
				names.push_back(name);
			}
		}
		std::sort(begin(names), end(names));
		to.varint(uint64(names.size()));
		for (const auto &name : names) {
			to.text(name);
		}
	};
	named(_words);
	named(_emoji);
	to.varint(uint64(_stickerInfos.size()));
	for (const auto &[id, info] : _stickerInfos) {
		to.varint(id);
		to.zigzag(info.msgId);
		to.byte(info.old ? 1 : 0);
		to.text(info.emoji);
	}

	return WrapFile(to.take());
}

bool Model::read(const char *data, qsizetype size) {
	auto from = Reader(data, size);
	_salt = from.raw(kSaltSize);
	const auto check = from.natural<uint32>();
	if (!from.ok()) {
		return false;
	}
	_key = MakeHashKey(_secret, _salt);
	if (check != checkValue()) {
		return false;
	}

	_coverage.main = ReadRange(from);
	_coverage.old = ReadRange(from);
	_coverage.checked = from.integer<TimeId>();
	const auto period = from.byte();
	const auto stopped = from.byte();
	if (!from.ok() || period > uchar(Period::All) || stopped > 1) {
		return false;
	}
	_coverage.period = Period(period);
	_coverage.stopped = (stopped != 0);

	const auto senders = from.count(2);
	_senders.reserve(senders);
	for (auto i = 0; i != senders && from.ok(); ++i) {
		auto name = RawName();
		name.id = from.varint();
		name.name = Cut(from.text(kMaxNameBytes), kMaxNameUnits);
		if (!_senderIndex.emplace(name.id, uint32(i)).second) {
			return false;
		}
		_senders.push_back(std::move(name));
	}
	_mainEdges.oldest = ReadEdge(from, _senders.size());
	_mainEdges.newest = ReadEdge(from, _senders.size());
	_oldEdges.oldest = ReadEdge(from, _senders.size());
	_oldEdges.newest = ReadEdge(from, _senders.size());

	const auto days = from.count(8);
	auto previousDay = int64(0);
	auto total = int64(0);
	for (auto i = 0; i != days && from.ok(); ++i) {
		const auto step = from.zigzag();
		const auto day = previousDay + std::clamp(step, -kMaxStep, kMaxStep);
		if (day < kMinDay || day > kMaxDay || (i && day <= previousDay)) {
			return false;
		}
		auto data = ReadDay(from, _senders.size());
		total += DayMessages(data);
		_days.emplace_hint(end(_days), int32(day), std::move(data));
		previousDay = day;
	}
	_count = int(std::min(total, int64(std::numeric_limits<int>::max())));

	ReadKeys(from, _words);
	ReadKeys(from, _emoji);
	ReadKeys(from, _stickers);
	const auto months = from.count(4);
	auto previousMonth = int64(0);
	for (auto i = 0; i != months && from.ok(); ++i) {
		const auto step = from.zigzag();
		const auto month = previousMonth
			+ std::clamp(step, -kMaxStep, kMaxStep);
		if (month < kMinMonth
			|| month > kMaxMonth
			|| (i && month <= previousMonth)) {
			return false;
		}
		auto bag = MonthBag();
		bag.words = ReadCounted(from, _words);
		bag.emoji = ReadCounted(from, _emoji);
		bag.stickers = ReadCounted(from, _stickers);
		_months.emplace_hint(end(_months), int32(month), std::move(bag));
		previousMonth = month;
	}

	// A name without counters, or two names for the same ones: not a
	// file this app has written.
	const auto named = [&](Counted<QString> &list, auto &&keyOf) {
		const auto count = from.count(2);
		for (auto i = 0; i != count && from.ok(); ++i) {
			const auto name = from.text(kMaxWordBytes);
			if (!from.ok() || name.isEmpty()) {
				return false;
			}
			const auto found = list.index.find(keyOf(name));
			if (found == end(list.index)
				|| !list.names[found->second].isEmpty()) {
				return false;
			}
			list.names[found->second] = name;
		}
		return from.ok();
	};
	if (!named(_words, [&](const QString &name) { return wordKey(name); })
		|| !named(_emoji, [&](const QString &name) {
			return emojiKey(name);
		})) {
		return false;
	}
	const auto stickers = from.count(4);
	for (auto i = 0; i != stickers && from.ok(); ++i) {
		const auto id = from.varint();
		auto info = StickerInfo();
		info.msgId = from.integer<int32>();
		const auto old = from.byte();
		info.old = (old != 0);
		info.emoji = Cut(from.text(kMaxWordBytes), kMaxStickerEmojiUnits);
		const auto found = _stickers.index.find(stickerKey(id));
		if (!from.ok()
			|| !id
			|| old > 1
			|| found == end(_stickers.index)
			|| _stickers.names[found->second] != 0) {
			return false;
		}
		_stickers.names[found->second] = id;
		_stickerInfos.emplace(id, std::move(info));
	}
	return from.finished();
}

ParseResult Model::parse(const QByteArray &bytes) {
	reset();
	auto result = ParseResult();
	if (bytes.size() > kFileVersionAt
		&& ReadUint32(bytes.constData()) == kFileMagic
		&& uchar(bytes[kFileVersionAt]) != kFileVersion) {
		result.damaged = true;
		result.outdated = true;
		return result;
	} else if (bytes.size() < kFileHeaderSize
		|| ReadUint32(bytes.constData()) != kFileMagic) {
		result.damaged = !bytes.isEmpty();
		return result;
	}
	result.valid = true;

	// All of the file or nothing: a part of it is not a smaller cache, it
	// is a wrong one. The same for a file made with another secret.
	const auto size = ReadUint32(bytes.constData() + kFileSizeAt);
	const auto checksum = ReadUint32(bytes.constData() + kFileChecksumAt);
	const auto data = bytes.constData() + kFileHeaderSize;
	auto parsed = Model(_secret);
	if (qint64(size) != qint64(bytes.size()) - kFileHeaderSize
		|| Checksum(data, qsizetype(size)) != checksum
		|| !parsed.read(data, qsizetype(size))) {
		result.damaged = true;
	} else {
		*this = std::move(parsed);
	}
	return result;
}

Report Analyze(
		const Model &model,
		const Zone &zone,
		const Options &options) {
	auto result = Report();
	result.from = options.from;
	result.till = options.till;
	result.group = options.group;
	const auto &coverage = model.coverage();
	result.complete = Covers(coverage, options.from, options.hasOld);
	result.wholeHistory = Covers(coverage, 0, options.hasOld);
	result.cached = model.count();

	// Everything is counted from the counters of the days, a period
	// starts with a day.
	const auto &senders = model.senders();
	const auto fromDay = (options.from > 0)
		? zone.day(options.from)
		: std::numeric_limits<int32>::min();
	const auto limited = [](int64 value) {
		return int(std::clamp(
			value,
			int64(0),
			int64(std::numeric_limits<int>::max())));
	};

	struct Person {
		int64 messages = 0;
		int64 textMessages = 0;
		int64 media = 0;
		int64 words = 0;
		int64 chars = 0;
		int64 answers = 0;
		std::vector<int64> waits; // By AnswerSlot().
	};
	auto persons = std::vector<Person>(senders.size());
	auto days = std::vector<DayValue>();
	auto messages = int64(0);
	auto textMessages = int64(0);
	auto links = int64(0);
	auto forwards = int64(0);
	auto replies = int64(0);
	auto hours = std::array<int64, 24>();
	auto weekdays = std::array<int64, 7>();
	auto kinds = std::array<int64, kKindCount>();
	const auto &all = model.days();
	for (auto i = all.lower_bound(fromDay); i != end(all); ++i) {
		const auto &[day, data] = *i;
		const auto count = DayMessages(data);
		if (count <= 0) {
			continue;
		}
		days.push_back({ day, count });
		messages += count;
		weekdays[WeekdayOfDay(day)] += count;
		for (auto hour = 0; hour != 24; ++hour) {
			hours[hour] += data.hours[hour];
		}
		for (auto kind = 0; kind != kKindCount; ++kind) {
			kinds[kind] += data.kinds[kind];
		}
		links += data.links;
		forwards += data.forwards;
		replies += data.replies;
		result.emoji += int64(data.emoji);
		for (const auto &one : data.persons) {
			if (one.sender >= persons.size()) {
				continue;
			}
			auto &person = persons[one.sender];
			person.messages += one.messages;
			person.textMessages += one.textMessages;
			person.media += one.media;
			person.words += int64(one.words);
			person.chars += int64(one.chars);
			textMessages += one.textMessages;
			result.words += int64(one.words);
			result.chars += int64(one.chars);
			if (options.group) {
				continue;
			}
			for (const auto &[slot, waited] : one.answers) {
				if (slot >= kAnswerSlots) {
					continue;
				} else if (person.waits.empty()) {
					person.waits.resize(kAnswerSlots, 0);
				}
				person.waits[slot] += waited;
				person.answers += waited;
			}
		}
	}
	result.messages = limited(messages);
	result.textMessages = limited(textMessages);
	result.links = limited(links);
	result.forwards = limited(forwards);
	result.replies = limited(replies);
	for (auto hour = 0; hour != 24; ++hour) {
		result.hours[hour] = limited(hours[hour]);
	}
	for (auto weekday = 0; weekday != 7; ++weekday) {
		result.weekdays[weekday] = limited(weekdays[weekday]);
	}
	for (auto kind = 0; kind != kKindCount; ++kind) {
		result.kinds[kind] = limited(kinds[kind]);
	}

	if (!days.empty()) {
		result.firstDay = days.front().day;
		result.lastDay = days.back().day;
		result.activeDays = int(days.size());
		result.days.resize(result.lastDay - result.firstDay + 1, 0);
		for (const auto &value : days) {
			result.days[value.day - result.firstDay] = value.count;
		}
	}

	// Of single messages only the ends of what is read are known.
	const auto inside = [&](const Edge &edge) {
		return edge.known
			&& (edge.sender < senders.size())
			&& (zone.day(edge.date) >= fromDay);
	};
	const auto &mainEdges = model.edges(false);
	const auto &oldEdges = model.edges(true);
	const auto &oldest = (oldEdges.oldest.known
		&& (!mainEdges.oldest.known
			|| oldEdges.oldest.date <= mainEdges.oldest.date))
		? oldEdges.oldest
		: mainEdges.oldest;
	const auto &newest = mainEdges.newest.known
		? mainEdges.newest
		: oldEdges.newest;
	if (!days.empty() && result.complete && inside(oldest)) {
		result.firstDate = oldest.date;
		result.firstSender = senders[oldest.sender].id;
	}
	if (!days.empty() && inside(newest)) {
		result.lastDate = newest.date;
	}

	for (const auto &value : days) {
		const auto date = DateOfDay(value.day);
		while (!result.months.empty()
			&& (result.months.back().year != date.year
				|| result.months.back().month != date.month)) {
			auto next = result.months.back();
			next.count = 0;
			if (++next.month > 12) {
				next.month = 1;
				++next.year;
			}
			result.months.push_back(next);
		}
		if (result.months.empty()) {
			result.months.push_back({ date.year, date.month, 0 });
		}
		auto &count = result.months.back().count;
		count = limited(int64(count) + value.count);
	}

	auto run = Streak();
	for (auto i = size_t(0); i != days.size(); ++i) {
		if (i && days[i].day == days[i - 1].day + 1) {
			++run.days;
		} else {
			run = { days[i].day, 1 };
		}
		if (run.days > result.longest.days) {
			result.longest = run;
		}
	}
	if (options.till > 0 && run.days > 0) {
		const auto today = zone.day(options.till);
		if (run.fromDay + run.days - 1 >= today - 1) {
			result.current = run;
		}
	}

	auto top = days;
	const auto topDays = std::min(
		top.size(),
		size_t(std::max(options.topDays, 0)));
	std::partial_sort(begin(top), begin(top) + topDays, end(top), [](
			const DayValue &a,
			const DayValue &b) {
		return (a.count != b.count) ? (a.count > b.count) : (a.day < b.day);
	});
	top.resize(topDays);
	result.topDays = std::move(top);

	for (auto i = size_t(0); i != persons.size(); ++i) {
		auto &person = persons[i];
		if (!person.messages) {
			continue;
		}
		auto stats = PersonStats();
		stats.id = senders[i].id;
		stats.name = senders[i].name;
		stats.messages = limited(person.messages);
		stats.textMessages = limited(person.textMessages);
		stats.media = limited(person.media);
		stats.words = person.words;
		stats.chars = person.chars;
		stats.answers = limited(person.answers);

		// The median: the slot the middle one of the answers is in.
		const auto middle = person.answers / 2;
		auto passed = int64(0);
		for (auto slot = 0; slot != int(person.waits.size()); ++slot) {
			passed += person.waits[slot];
			if (passed > middle) {
				stats.answerTime = AnswerSlotTime(slot);
				break;
			}
		}
		result.persons.push_back(std::move(stats));
	}
	std::sort(begin(result.persons), end(result.persons), [](
			const PersonStats &a,
			const PersonStats &b) {
		return (a.messages != b.messages)
			? (a.messages > b.messages)
			: (a.words != b.words)
			? (a.words > b.words)
			: (a.id < b.id);
	});

	// The counters of words, emoji and stickers are kept by the month.
	// Only the named ones are shown: of the others nothing is known but
	// the numbers.
	const auto fromMonth = FirstCountedMonth(model, zone, options.from);
	const auto &words = model.words().names;
	const auto &emoji = model.emoji().names;
	const auto &stickers = model.stickers().names;
	auto wordCounts = std::vector<uint32>(words.size(), 0);
	auto emojiCounts = std::vector<uint32>(emoji.size(), 0);
	auto stickerCounts = std::vector<uint32>(stickers.size(), 0);
	const auto sum = [](
			std::vector<uint32> &counts,
			const Counts<uint32> &list) {
		for (const auto &[id, count] : list) {
			if (id < counts.size()) {
				counts[id] = Plus(counts[id], count);
			}
		}
	};
	const auto &bags = model.months();
	for (auto i = bags.lower_bound(fromMonth); i != end(bags); ++i) {
		sum(wordCounts, i->second.words);
		sum(emojiCounts, i->second.emoji);
		sum(stickerCounts, i->second.stickers);
	}

	auto wordIds = std::vector<uint32>();
	for (auto i = uint32(0); i != wordCounts.size(); ++i) {
		if (wordCounts[i] >= kMinTopWordCount && !words[i].isEmpty()) {
			wordIds.push_back(i);
		}
	}
	const auto topWords = std::min(
		wordIds.size(),
		size_t(std::max(options.topWords, 0)));
	std::partial_sort(
		begin(wordIds),
		begin(wordIds) + topWords,
		end(wordIds),
		[&](uint32 a, uint32 b) {
			return (wordCounts[a] != wordCounts[b])
				? (wordCounts[a] > wordCounts[b])
				: (words[a] < words[b]);
		});
	for (auto i = size_t(0); i != topWords; ++i) {
		result.topWords.push_back({
			words[wordIds[i]],
			int(std::min(wordCounts[wordIds[i]], uint32(0x7FFFFFFF))),
		});
	}

	auto emojiTop = std::vector<TopText>();
	for (auto i = uint32(0); i != emojiCounts.size(); ++i) {
		if (emojiCounts[i] && !emoji[i].isEmpty()) {
			emojiTop.push_back({
				emoji[i],
				int(std::min(emojiCounts[i], uint32(0x7FFFFFFF))),
			});
		}
	}
	const auto topEmoji = std::min(
		emojiTop.size(),
		size_t(std::max(options.topEmoji, 0)));
	std::partial_sort(
		begin(emojiTop),
		begin(emojiTop) + topEmoji,
		end(emojiTop),
		[](const TopText &a, const TopText &b) {
			return (a.count != b.count)
				? (a.count > b.count)
				: (a.text < b.text);
		});
	emojiTop.resize(topEmoji);
	result.topEmoji = std::move(emojiTop);

	auto stickerTop = std::vector<TopSticker>();
	for (auto i = uint32(0); i != stickerCounts.size(); ++i) {
		if (stickerCounts[i] && stickers[i] != 0) {
			auto sticker = TopSticker();
			sticker.id = stickers[i];
			sticker.count = int(
				std::min(stickerCounts[i], uint32(0x7FFFFFFF)));
			stickerTop.push_back(std::move(sticker));
		}
	}
	const auto topStickers = std::min(
		stickerTop.size(),
		size_t(std::max(options.topStickers, 0)));
	std::partial_sort(
		begin(stickerTop),
		begin(stickerTop) + topStickers,
		end(stickerTop),
		[](const TopSticker &a, const TopSticker &b) {
			return (a.count != b.count) ? (a.count > b.count) : (a.id < b.id);
		});
	stickerTop.resize(topStickers);
	for (auto &sticker : stickerTop) {
		const auto i = model.stickerInfos().find(sticker.id);
		if (i != end(model.stickerInfos())) {
			sticker.info = i->second;
		}
	}
	result.topStickers = std::move(stickerTop);
	return result;
}

int32 FirstCountedMonth(const Model &model, const Zone &zone, TimeId from) {
	if (from <= 0) {
		return std::numeric_limits<int32>::min();
	}
	const auto fromDay = zone.day(from);
	const auto month = MonthOfDay(fromDay);
	const auto &days = model.days();
	const auto till = days.lower_bound(FirstDayOfMonth(month + 1));
	auto before = int64(0);
	auto inside = int64(0);
	for (auto i = days.lower_bound(FirstDayOfMonth(month)); i != till; ++i) {
		((i->first < fromDay) ? before : inside) += DayMessages(i->second);
	}
	return (inside >= before) ? month : (month + 1);
}

TimeId PeriodStart(Period period, TimeId now, const Zone &zone) {
	if (period == Period::All) {
		return 0;
	}
	auto date = DateOfDay(zone.day(now));
	if (period == Period::Year) {
		--date.year;
	} else {
		date.month -= 3;
		if (date.month < 1) {
			date.month += 12;
			--date.year;
		}
	}
	date.day = std::min(date.day, MonthDays(date.year, date.month));
	return zone.dayStart(DayOfDate(date));
}

int FloodSeconds(const QString &type) {
	const auto prefixes = { u"FLOOD_WAIT_"_q, u"FLOOD_PREMIUM_WAIT_"_q };
	for (const auto &prefix : prefixes) {
		if (type.startsWith(prefix)) {
			auto ok = false;
			const auto value = type.mid(prefix.size()).toLongLong(&ok);
			return (!ok || value <= 0)
				? kFloodFallback
				: int(std::min(value, qlonglong(kFloodMax)));
		}
	}
	return 0;
}

Storage::Storage(
	QString path,
	std::shared_ptr<Zone> zone,
	QByteArray secret)
: _path(std::move(path))
, _zone(std::move(zone))
, _secret(std::move(secret))
, _model(_secret) {
}

Storage::State Storage::state() const {
	return {
		.coverage = _model.coverage(),
		.messages = _model.count(),
		.failed = _unreadable,
	};
}

Storage::State Storage::load() {
	if (!_loaded) {
		_loaded = true;
		read();
	}
	return state();
}

Storage::State Storage::reload() {
	if (_unreadable) {
		// Whatever was done meanwhile was done to nothing.
		_model = Model(_secret);
		_unreadable = false;
		_unsaved = false;
		_answers = 0;
		_loaded = false;
	}
	return load();
}

// "No file" and "a file that could not be read" are not the same: over
// the second one nothing is written. Another program (an antivirus, a
// backup) may hold the file for a moment, so it is tried a few times.
void Storage::read() {
	for (auto attempt = 0; attempt != kReadAttempts; ++attempt) {
		if (attempt) {
			QThread::msleep(kReadRetryPause);
		}
		auto file = QFile(_path);
		if (!file.exists()) {
			return;
		} else if (file.size() > kMaxFileSize) {
			break;
		} else if (!file.open(QIODevice::ReadOnly)) {
			continue;
		}
		const auto bytes = file.readAll();
		const auto whole = (file.error() == QFileDevice::NoError)
			&& (bytes.size() == file.size());
		file.close();
		if (!whole) {
			continue;
		}
		const auto parsed = _model.parse(bytes);
		if (parsed.valid && !parsed.damaged) {
			return;
		} else if (bytes.isEmpty() || parsed.outdated) {
			// Nothing to keep. The files of the versions before had a
			// row for every message and the words as they are: such a
			// file is not left on the disk till the next write.
			QFile::remove(_path);
			return;
		}

		// A broken file of this version, or one made with another
		// secret. Thrown away without a word, it would have the whole
		// history asked for again, on every opening if it gets broken
		// again: the user is told and decides.
		break;
	}
	_unreadable = true;
}

int Storage::saveEvery() const {
	const auto large = _model.count() / (kPageLimit * kSaveShare);
	return std::max(kSaveAnswers, large);
}

void Storage::save() {
	if (_unreadable) {
		return;
	}
	_answers = 0;
	const auto bytes = _model.serialize();
	QDir().mkpath(QFileInfo(_path).absolutePath());
	auto file = QSaveFile(_path);
	if (file.open(QIODevice::WriteOnly)
		&& (file.write(bytes) == bytes.size())
		&& file.commit()) {
		_unsaved = false;
		++_writes;
	}
}

void Storage::flush() {
	if (_loaded && _unsaved) {
		save();
	}
}

Storage::PageResult Storage::add(const RawPage &page) {
	load();
	auto result = PageResult();
	const auto before = _model.coverage();
	result.accepted = _model.add(page, *_zone);
	result.advanced = ReadingMoved(before, _model.coverage());
	if (result.accepted > 0 || _model.coverage() != before) {
		_unsaved = true;
		if (++_answers >= saveEvery()) {
			save();
		}
	}
	result.state = state();
	return result;
}

Storage::State Storage::finish(TimeId checked) {
	load();
	_model.setChecked(checked);
	_unsaved = true;
	save();
	return state();
}

void Storage::setPeriod(Period period) {
	load();
	if (_model.coverage().period != period) {
		_model.setPeriod(period);
		_unsaved = true;
		save();
	}
}

void Storage::setStopped(bool stopped) {
	load();
	if (_model.coverage().stopped != stopped) {
		_model.setStopped(stopped);
		_unsaved = true;
		save();
	}
}

Report Storage::analyze(const Options &options) {
	load();
	return Analyze(_model, *_zone, options);
}

void Storage::clear() {
	_model = Model(_secret);
	_loaded = true;
	_unreadable = false;
	_answers = 0;

	// A file that could not be removed now is replaced with an empty one
	// later: cleared data does not come back.
	_unsaved = !QFile::remove(_path) && QFile::exists(_path);
}

// Pure logic end.

namespace {

// A pause after every answer, a longer one after the server has asked to
// wait once. With a hundred of messages in an answer it is about four
// thousands of messages in a minute.
constexpr auto kPagePause = crl::time(1200);
constexpr auto kPagePauseJitter = 500;
constexpr auto kSlowPause = crl::time(3000);
constexpr auto kSlowFor = crl::time(600) * 1000;
constexpr auto kFloodMargin = 2;
constexpr auto kFreshSeconds = TimeId(120);
constexpr auto kAnalyzeEvery = crl::time(4000);
constexpr auto kAnalyzeSpentFactor = 20;

// Everything heavy is done here, one thing at a time: reading and
// writing the caches, counting the answers and the statistics.
[[nodiscard]] crl::queue &Queue() {
	static const auto result = new crl::queue();
	return *result;
}

[[nodiscard]] QString CacheFolder(not_null<Main::Session*> session) {
	return cWorkingDir()
		+ u"tdata/oblivion/"_q
		+ (session->isTestMode() ? u"test_"_q : QString())
		+ QString::number(session->userId().bare)
		+ u"/stats/"_q;
}

[[nodiscard]] QString CachePath(not_null<PeerData*> peer) {
	return CacheFolder(&peer->session())
		+ QString::number(peer->id.value)
		+ u".dat"_q;
}

// The secret of the caches (see Model): made of the key the local data
// of the app is encrypted with. It is the same on every run, lives in the
// memory only and is not written anywhere, the key itself is not changed
// and not kept. Who can't open the local data can't match the counters
// of a cache to the words.
[[nodiscard]] QByteArray CacheSecret(not_null<Main::Session*> session) {
	const auto key = session->local().peekLegacyLocalKey();
	if (!key) {
		return QByteArray();
	}
	const auto data = key->data();
	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	hash.addData(QByteArray("oblivion chat stats secret"));
	hash.addData(QByteArray::fromRawData(
		reinterpret_cast<const char*>(data.data()),
		qsizetype(data.size())));
	return hash.result();
}

[[nodiscard]] bool IsReply(const MTPMessageReplyHeader &header) {
	return header.match([](const MTPDmessageReplyHeader &data) {
		// In a topic every message "replies" to the topic itself.
		const auto to = data.vreply_to_msg_id();
		const auto top = data.vreply_to_top_id();
		return to && (!data.is_forum_topic() || top);
	}, [](const MTPDmessageReplyStoryHeader &) {
		return false;
	});
}

void FillEntities(const QVector<MTPMessageEntity> &list, RawMessage &raw) {
	for (const auto &entity : list) {
		auto offset = 0;
		auto length = 0;
		entity.match([&](const auto &data) {
			offset = data.voffset().v;
			length = data.vlength().v;
		});
		switch (entity.type()) {
		case mtpc_messageEntityUrl:
		case mtpc_messageEntityTextUrl:
			raw.flags = uchar(raw.flags | kLink);
			break;
		default: break;
		}
		switch (entity.type()) {
		case mtpc_messageEntityUrl:
		case mtpc_messageEntityMention:
		case mtpc_messageEntityMentionName:
		case mtpc_inputMessageEntityMentionName:
		case mtpc_messageEntityHashtag:
		case mtpc_messageEntityCashtag:
		case mtpc_messageEntityBotCommand:
		case mtpc_messageEntityEmail:
		case mtpc_messageEntityPhone:
		case mtpc_messageEntityBankCard:
		case mtpc_messageEntityCode:
		case mtpc_messageEntityPre:
			if (offset >= 0 && length > 0) {
				raw.skip.push_back({ offset, length });
			}
			break;
		default: break;
		}
	}
}

void FillDocument(const MTPDdocument &document, RawMessage &raw) {
	auto sticker = false;
	auto animated = false;
	auto video = false;
	auto round = false;
	auto audio = false;
	auto voice = false;
	auto alt = QString();
	for (const auto &attribute : document.vattributes().v) {
		attribute.match([&](const MTPDdocumentAttributeSticker &data) {
			sticker = true;
			alt = qs(data.valt());
		}, [&](const MTPDdocumentAttributeAnimated &) {
			animated = true;
		}, [&](const MTPDdocumentAttributeVideo &data) {
			video = true;
			round = data.is_round_message();
		}, [&](const MTPDdocumentAttributeAudio &data) {
			audio = true;
			voice = data.is_voice();
		}, [](const auto &) {
		});
	}
	if (sticker) {
		raw.kind = Kind::Sticker;
		raw.sticker = uint64(document.vid().v);
		raw.stickerEmoji = alt;
	} else if (round) {
		raw.kind = Kind::Round;
	} else if (voice) {
		raw.kind = Kind::Voice;
	} else if (animated) {
		raw.kind = Kind::Gif;
	} else if (video) {
		raw.kind = Kind::Video;
	} else if (audio) {
		raw.kind = Kind::Music;
	}
}

void FillMedia(const MTPMessageMedia &media, RawMessage &raw) {
	media.match([&](const MTPDmessageMediaEmpty &) {
	}, [&](const MTPDmessageMediaPhoto &) {
		raw.kind = Kind::Photo;
	}, [&](const MTPDmessageMediaDocument &data) {
		raw.kind = data.is_round()
			? Kind::Round
			: data.is_voice()
			? Kind::Voice
			: data.is_video()
			? Kind::Video
			: Kind::File;
		if (const auto document = data.vdocument()) {
			document->match([&](const MTPDdocument &data) {
				FillDocument(data, raw);
			}, [](const MTPDdocumentEmpty &) {
			});
		}
	}, [&](const MTPDmessageMediaWebPage &) {
		raw.flags = uchar(raw.flags | kLink);
	}, [&](const MTPDmessageMediaGeo &) {
		raw.kind = Kind::Location;
	}, [&](const MTPDmessageMediaGeoLive &) {
		raw.kind = Kind::Location;
	}, [&](const MTPDmessageMediaVenue &) {
		raw.kind = Kind::Location;
	}, [&](const MTPDmessageMediaContact &) {
		raw.kind = Kind::Contact;
	}, [&](const MTPDmessageMediaPoll &) {
		raw.kind = Kind::Poll;
	}, [&](const auto &) {
		raw.kind = Kind::Other;
	});
}

// Only what is counted is taken from the answer. The users and the chats
// of the answer are given to the account as with any loaded history, the
// messages themselves are not: they are not added to the chat.
[[nodiscard]] RawPage ConvertPage(
		not_null<PeerData*> history,
		const MTPmessages_Messages &result,
		const Request &request) {
	using List = QVector<MTPMessage>;

	auto page = RawPage();
	page.old = request.old;
	page.direct = history->isUser();
	page.type = request.type;
	const auto owner = &history->owner();
	const auto self = history->session().userPeerId();
	const auto list = result.match([&](
			const MTPDmessages_messagesNotModified &) -> const List* {
		return nullptr;
	}, [&](const MTPDmessages_messages &data) -> const List* {
		owner->processUsers(data.vusers());
		owner->processChats(data.vchats());
		if (request.type == PageType::First) {
			page.total = int(data.vmessages().v.size());
		}
		return &data.vmessages().v;
	}, [&](const MTPDmessages_messagesSlice &data) -> const List* {
		owner->processUsers(data.vusers());
		owner->processChats(data.vchats());
		page.total = data.vcount().v;
		return &data.vmessages().v;
	}, [&](const MTPDmessages_channelMessages &data) -> const List* {
		owner->processUsers(data.vusers());
		owner->processChats(data.vchats());
		page.total = data.vcount().v;
		return &data.vmessages().v;
	});
	if (!list) {
		return page;
	}

	auto senders = std::set<uint64>();
	const auto track = [&](int32 id, TimeId date) {
		++page.returned;
		if (id > 0) {
			page.minId = page.minId ? std::min(page.minId, id) : id;
			page.maxId = std::max(page.maxId, id);
		}
		if (date > 0) {
			page.minDate = page.minDate ? std::min(page.minDate, date) : date;
			page.maxDate = std::max(page.maxDate, date);
		}
	};
	const auto sender = [&](const MTPPeer *from, bool out) {
		return (from
			? peerFromMTP(*from)
			: (out && history->isUser())
			? self
			: history->id).value;
	};
	page.messages.reserve(list->size());
	for (const auto &message : *list) {
		message.match([&](const MTPDmessage &data) {
			track(data.vid().v, data.vdate().v);
			auto raw = RawMessage();
			raw.id = data.vid().v;
			raw.date = data.vdate().v;
			const auto from = data.vfrom_id();
			raw.sender = sender(from ? &*from : nullptr, data.is_out());
			if (data.vfwd_from()) {
				raw.flags = uchar(raw.flags | kForwarded);
			}
			if (const auto reply = data.vreply_to()) {
				if (IsReply(*reply)) {
					raw.flags = uchar(raw.flags | kReply);
				}
			}
			raw.text = qs(data.vmessage());
			if (const auto entities = data.ventities()) {
				FillEntities(entities->v, raw);
			}
			if (const auto media = data.vmedia()) {
				FillMedia(*media, raw);
			}
			senders.insert(raw.sender);
			page.messages.push_back(std::move(raw));
		}, [&](const MTPDmessageService &data) {
			// Joins, pins and the like are not messages, the calls are.
			track(data.vid().v, data.vdate().v);
			if (data.vaction().type() != mtpc_messageActionPhoneCall) {
				return;
			}
			auto raw = RawMessage();
			raw.id = data.vid().v;
			raw.date = data.vdate().v;
			const auto from = data.vfrom_id();
			raw.sender = sender(from ? &*from : nullptr, data.is_out());
			raw.kind = Kind::Call;
			senders.insert(raw.sender);
			page.messages.push_back(std::move(raw));
		}, [&](const MTPDmessageEmpty &data) {
			track(data.vid().v, 0);
		});
	}
	page.names.reserve(senders.size());
	for (const auto id : senders) {
		if (const auto peer = owner->peerLoaded(PeerId(id))) {
			page.names.push_back({ id, peer->name() });
		}
	}
	return page;
}

} // namespace

bool Supported(not_null<PeerData*> peer) {
	if (const auto user = peer->asUser()) {
		return !user->isSelf()
			&& !user->isBot()
			&& !user->isServiceUser()
			&& !user->isRepliesChat()
			&& !user->isVerifyCodes();
	} else if (const auto chat = peer->asChat()) {
		return !chat->isForbidden();
	} else if (const auto channel = peer->asChannel()) {
		return channel->isMegagroup()
			&& !channel->isMonoforum()
			&& channel->amIn();
	}
	return false;
}

class Tracker::Requests final {
public:
	explicit Requests(not_null<Main::Session*> session)
	: api(&session->mtp()) {
	}

	MTP::Sender api;
	mtpRequestId request = 0;
	base::Timer pause;
	base::Timer flood;

};

Tracker::Tracker(not_null<PeerData*> peer, TrackerHost host)
: _peer(peer)
, _host(std::move(host))
, _zone(Zone::System())
, _storage(std::make_shared<Storage>(
	CachePath(peer),
	_zone,
	CacheSecret(&peer->session())))
, _requests(std::make_unique<Requests>(&peer->session())) {
	_requests->pause.setCallback([=] { next(); });
	_requests->flood.setCallback([=] { floodEnded(); });
	load(false);
}

// The answers counted after the last write of the cache are written now,
// on the same queue, after everything that still counts them.
Tracker::~Tracker() {
	Queue().async([storage = _storage] {
		storage->flush();
	});
}

void Tracker::load(bool again) {
	const auto weak = base::make_weak(this);
	Queue().async([=, storage = _storage] {
		const auto state = again ? storage->reload() : storage->load();
		crl::on_main(weak, [=] {
			loaded(state);
		});
	});
}

rpl::producer<Status> Tracker::status() {
	const auto weak = base::make_weak(this);
	return rpl::single(
		rpl::empty
	) | rpl::then(
		_statusChanges.events() | rpl::to_empty
	) | rpl::map([=] {
		const auto strong = weak.get();
		return strong ? strong->_status : Status();
	});
}

auto Tracker::reports()
-> rpl::producer<std::shared_ptr<const Report>> {
	const auto weak = base::make_weak(this);
	return rpl::single(
		rpl::empty
	) | rpl::then(
		_reportChanges.events() | rpl::to_empty
	) | rpl::map([=] {
		const auto strong = weak.get();
		return strong ? strong->_report : nullptr;
	});
}

TimeId Tracker::periodFrom() const {
	return PeriodStart(_status.period, base::unixtime::now(), *_zone);
}

bool Tracker::hasOld() const {
	return (_peer->migrateFrom() != nullptr);
}

bool Tracker::busy() const {
	return (_status.stage == Stage::Waiting)
		|| (_status.stage == Stage::Reading)
		|| (_status.stage == Stage::Flood);
}

rpl::lifetime Tracker::view() {
	++_views;
	auto result = rpl::lifetime();
	result.add([weak = base::make_weak(this)] {
		if (const auto strong = weak.get()) {
			--strong->_views;
			strong->_host.idle();
		}
	});
	return result;
}

void Tracker::push() {
	const auto old = hasOld();
	_status.from = periodFrom();
	_status.complete = Covers(_coverage, _status.from, old);
	_status.total = _coverage.main.total + (old ? _coverage.old.total : 0);
	_status.oldest = (old && _coverage.old.started && _coverage.old.lowDate)
		? _coverage.old.lowDate
		: _coverage.main.lowDate;
	_status.checked = _coverage.checked;
	_status.progress = ReadingProgress(
		_coverage,
		_status.from,
		old,
		_status.messages);
	_statusChanges.fire_copy(_status);
}

void Tracker::loaded(Storage::State state) {
	if (_status.stage != Stage::Loading) {
		return;
	} else if (state.failed) {
		// The file is there and could not be read. Reading the history
		// from its newest message now would end with the file replaced:
		// nothing is asked till the user tries again or clears the data.
		_wantRefresh = false;
		_wantManual = false;
		_status.cacheFailed = true;
		_status.stage = Stage::Failed;
		push();
		return;
	}
	_coverage = state.coverage;
	_status.messages = state.messages;
	if (_periodChosen) {
		const auto period = _status.period;
		if (_coverage.period != period) {
			_coverage.period = period;
			Queue().async([=, storage = _storage] {
				storage->setPeriod(period);
			});
		}
	} else {
		_status.period = _coverage.period;
	}

	// A reading stopped by the user stays stopped when the statistics
	// are opened again, the button continues it.
	_stoppedByUser = _coverage.stopped
		&& !Covers(_coverage, periodFrom(), hasOld());
	_status.stage = _stoppedByUser ? Stage::Stopped : Stage::Idle;
	push();
	analyze();
	if (base::take(_wantRefresh)) {
		refresh(base::take(_wantManual));
	}
}

void Tracker::refresh(bool manual) {
	if (_status.stage == Stage::Loading) {
		_wantRefresh = true;
		_wantManual = _wantManual || manual;
		return;
	} else if (busy()) {
		return;
	} else if (_status.cacheFailed) {
		if (manual) {
			// One more try to read the cache, then what is missing in it
			// is read as on any opening of the statistics.
			_status.cacheFailed = false;
			_status.stage = Stage::Loading;
			_wantRefresh = true;
			push();
			load(true);
		}
		return;
	}
	if (manual) {
		_headChecked = false;
	} else {
		if (_stoppedByUser || _status.stage == Stage::Failed) {
			return;
		}
		const auto now = base::unixtime::now();
		const auto checked = _coverage.checked;
		const auto fresh = (checked > 0)
			&& (now >= checked)
			&& (now - checked < kFreshSeconds);
		if (fresh && Covers(_coverage, periodFrom(), hasOld())) {
			return;
		}
		_headChecked = fresh;
		_headCheckedAt = checked;
	}
	_headRechecked = false;
	_stoppedByUser = false;
	if (_coverage.stopped) {
		_coverage.stopped = false;
		Queue().async([storage = _storage] {
			storage->setStopped(false);
		});
	}
	_host.acquire();
}

void Tracker::run() {
	_status.stage = Stage::Reading;
	_status.error = QString();
	_status.waitingFor = QString();
	_status.floodTill = 0;
	push();
	next();
}

void Tracker::wait(const QString &title) {
	_status.stage = Stage::Waiting;
	_status.waitingFor = title;
	push();
}

Request Tracker::plan() {
	const auto request = [&] {
		return NextRequest(_coverage, {
			.from = periodFrom(),
			.hasOld = hasOld(),
			.headChecked = _headChecked,
		});
	};
	auto result = request();
	if (!result.valid && _headChecked && !_headRechecked) {
		// A long reading ends: the messages that came while it went on
		// are asked for (once) before it is called complete.
		const auto now = base::unixtime::now();
		if (now < _headCheckedAt || now - _headCheckedAt >= kFreshSeconds) {
			_headRechecked = true;
			_headChecked = false;
			result = request();
		}
	}
	return result;
}

// The only place a request is sent from: never while another one is in
// flight or its answer is being counted.
void Tracker::next() {
	if (_status.stage != Stage::Reading
		|| _requests->request
		|| _counting) {
		return;
	}
	const auto request = plan();
	const auto history = !request.valid
		? nullptr
		: request.old
		? static_cast<PeerData*>(_peer->migrateFrom())
		: _peer.get();
	if (!history) {
		finished();
		return;
	} else if (waitForFlood()) {
		return;
	}
	const auto pace = _host.pace;
	const auto since = crl::now() - pace->sentAt;
	const auto gap = pause();
	if (pace->sentAt && since >= 0 && since < gap) {
		// Stopping and continuing, or another chat taking its turn,
		// must not bring two requests closer than the usual pause.
		_requests->pause.callOnce(gap - since);
		return;
	}
	pace->sentAt = crl::now();
	const auto generation = _generation;
	_requests->request = _requests->api.request(MTPmessages_GetHistory(
		history->input(),
		MTP_int(request.offsetId),
		MTP_int(0), // offset_date
		MTP_int(request.addOffset),
		MTP_int(request.limit),
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([=](const MTPmessages_Messages &result) {
		_requests->request = 0;
		if (generation != _generation) {
			return;
		}
		auto page = ConvertPage(history, result, request);
		const auto returned = page.returned;
		const auto weak = base::make_weak(this);
		_counting = true;
		Queue().async([=, storage = _storage, page = std::move(page)] {
			const auto added = storage->add(page);
			crl::on_main(weak, [=] {
				applied(generation, request, returned, added);
			});
		});
	}).fail([=](const MTP::Error &error) {
		// FLOOD_WAIT comes here as well (handleFloodErrors), instead of
		// being waited out and repeated inside of MTP: the wait is shown
		// and nothing is asked till it ends.
		_requests->request = 0;
		failed(error.type());
	}).handleFloodErrors().send();
}

void Tracker::applied(
		int generation,
		Request request,
		int returned,
		Storage::PageResult result) {
	_counting = false;
	if (generation != _generation) {
		// The cache was cleared meanwhile. If the reading has started
		// anew already, it was waiting for this answer to be counted.
		next();
		return;
	}
	_coverage.main = result.state.coverage.main;
	_coverage.old = result.state.coverage.old;
	_status.messages = result.state.messages;
	if (HeadChecked(request, returned, result.advanced)) {
		_headChecked = true;
		_headCheckedAt = base::unixtime::now();
	}
	if (_status.stage != Stage::Reading) {
		push();
		analyze();
		return;
	} else if (ReadingStuck(request, returned, result.advanced)) {
		// Never asked again by itself: no loops.
		failed(u"OBLIVION_NO_PROGRESS"_q);
		return;
	}
	push();
	const auto every = std::max(
		kAnalyzeEvery,
		_analyzeSpent * kAnalyzeSpentFactor);
	if (!_analyzedAt || (crl::now() - _analyzedAt >= every)) {
		analyze();
	}
	_requests->pause.callOnce(pause()
		+ base::RandomIndex(kPagePauseJitter));
}

// Longer for some time after the server has asked to wait.
crl::time Tracker::pause() const {
	return (crl::now() < _host.pace->slowTill) ? kSlowPause : kPagePause;
}

void Tracker::failed(const QString &type) {
	if (const auto wait = FloodSeconds(type)) {
		const auto pace = _host.pace;
		pace->floodTill = base::unixtime::now() + wait + kFloodMargin;
		pace->slowTill = crl::now()
			+ (wait + kFloodMargin) * crl::time(1000)
			+ kSlowFor;
		if (_status.stage == Stage::Reading && !waitForFlood()) {
			_requests->pause.callOnce(kSlowPause);
		}
		return;
	}
	_status.error = type;
	released(Stage::Failed);
}

// The wait is kept as a moment of time, one for the account: neither a
// sleep nor stopping and continuing nor another chat shortens it.
bool Tracker::waitForFlood() {
	const auto pace = _host.pace;
	const auto now = base::unixtime::now();
	if (pace->floodTill <= now) {
		return false;
	} else if (pace->floodTill - now > kFloodMax + kFloodMargin) {
		// The clock was set back.
		pace->floodTill = now + kFloodMax + kFloodMargin;
	}
	_status.stage = Stage::Flood;
	_status.floodTill = pace->floodTill;
	_requests->flood.callOnce((pace->floodTill - now) * crl::time(1000));

	// The wait may be a long one: what is counted is written before it.
	Queue().async([storage = _storage] {
		storage->flush();
	});
	push();
	return true;
}

void Tracker::floodEnded() {
	if (_status.stage != Stage::Flood) {
		return;
	}
	_status.stage = Stage::Reading;
	_status.floodTill = 0;
	if (!waitForFlood()) {
		push();
		next();
	}
}

void Tracker::finished() {
	const auto now = base::unixtime::now();
	_coverage.checked = now;
	Queue().async([=, storage = _storage] {
		storage->finish(now);
	});
	released(Stage::Done);
}

void Tracker::released(Stage stage) {
	const auto was = busy();
	_requests->pause.cancel();
	_requests->flood.cancel();
	_status.stage = stage;
	_status.floodTill = 0;
	_status.waitingFor = QString();
	if (stage != Stage::Done) {
		// A reading that has not come to its end: what it has counted is
		// written, the app may be closed before it is continued.
		Queue().async([storage = _storage] {
			storage->flush();
		});
	}
	push();
	analyze();
	if (was) {
		_host.release();
	}
}

void Tracker::stop() {
	if (!busy()) {
		return;
	}
	_stoppedByUser = true;
	if (!_coverage.stopped) {
		_coverage.stopped = true;
		Queue().async([storage = _storage] {
			storage->setStopped(true);
		});
	}
	if (const auto id = base::take(_requests->request)) {
		_requests->api.request(id).cancel();
	}
	released(Stage::Stopped);
}

void Tracker::clear() {
	if (_status.stage == Stage::Loading) {
		return;
	}
	if (const auto id = base::take(_requests->request)) {
		_requests->api.request(id).cancel();
	}
	const auto was = busy();
	_requests->pause.cancel();
	_requests->flood.cancel();
	++_generation;
	_stoppedByUser = true;
	_headChecked = false;

	const auto period = _status.period;
	_coverage = Coverage();
	_coverage.period = period;
	_status.messages = 0;
	_status.error = QString();
	_status.cacheFailed = false;
	_status.waitingFor = QString();
	_status.floodTill = 0;
	_status.stage = Stage::Idle;

	// The chosen period is not a part of the cleared data: it is written
	// again (for the default one there is nothing to write).
	Queue().async([=, storage = _storage] {
		storage->clear();
		storage->setPeriod(period);
	});
	push();
	analyze();
	if (was) {
		_host.release();
	}
}

void Tracker::setPeriod(Period period) {
	if (_status.period == period) {
		return;
	}
	_status.period = period;
	_coverage.period = period;
	_periodChosen = true;
	if (_status.stage == Stage::Loading) {
		_statusChanges.fire_copy(_status);
		return;
	}
	Queue().async([=, storage = _storage] {
		storage->setPeriod(period);
	});
	push();
	analyze();
	if (!busy()) {
		// A longer period needs older messages.
		refresh(false);
	}
}

void Tracker::analyze() {
	if (_status.stage == Stage::Loading) {
		return;
	} else if (_analyzing) {
		_analyzeAgain = true;
		return;
	}
	_analyzing = true;
	const auto options = Options{
		.from = periodFrom(),
		.till = base::unixtime::now(),
		.group = !_peer->isUser(),
		.hasOld = hasOld(),
	};
	const auto generation = _generation;
	const auto weak = base::make_weak(this);
	Queue().async([=, storage = _storage] {
		const auto started = crl::now();
		auto report = std::make_shared<const Report>(
			storage->analyze(options));
		const auto spent = crl::now() - started;
		crl::on_main(weak, [=] {
			analyzed(generation, report, spent);
		});
	});
}

void Tracker::analyzed(
		int generation,
		std::shared_ptr<const Report> report,
		crl::time spent) {
	_analyzing = false;
	_analyzedAt = crl::now();
	_analyzeSpent = spent;
	if (generation == _generation) {
		_report = std::move(report);
		_reportChanges.fire_copy(_report);
	}
	if (base::take(_analyzeAgain)) {
		analyze();
	}
}

namespace {

// The chats of one account: one of them reads its history at a time, the
// others wait in a line.
class Account final : public base::has_weak_ptr {
public:
	explicit Account(not_null<Main::Session*> session);
	~Account();

	[[nodiscard]] Tracker *tracker(not_null<PeerData*> peer);
	void forget();

private:
	void acquire(not_null<Tracker*> tracker);
	void release(not_null<Tracker*> tracker);
	void checkLater();
	void check();

	const not_null<Main::Session*> _session;
	const std::shared_ptr<Pace> _pace;
	std::map<not_null<PeerData*>, std::unique_ptr<Tracker>> _trackers;
	Tracker *_active = nullptr;
	std::vector<not_null<Tracker*>> _line;
	bool _checkScheduled = false;

};

Account::Account(not_null<Main::Session*> session)
: _session(session)
, _pace(std::make_shared<Pace>()) {
	// On the queue before the cache of any chat is read.
	Queue().async([folder = CacheFolder(session)] {
		RemoveOutdatedCaches(folder);
	});
}

Account::~Account() {
	_line.clear();
	_active = nullptr;
	_trackers.clear();

	// The account goes away with the app or on a logout: what was
	// received is written before that. Nothing on the queue waits for
	// the main thread.
	Queue().sync([] {});
}

Tracker *Account::tracker(not_null<PeerData*> peer) {
	const auto i = _trackers.find(peer);
	if (i != end(_trackers)) {
		return i->second.get();
	}
	const auto holder = std::make_shared<Tracker*>(nullptr);
	auto created = std::make_unique<Tracker>(peer, TrackerHost{
		.acquire = [=] { acquire(*holder); },
		.release = [=] { release(*holder); },
		.idle = [=] { checkLater(); },
		.pace = _pace,
	});
	*holder = created.get();
	return _trackers.emplace(peer, std::move(created)).first->second.get();
}

void Account::acquire(not_null<Tracker*> tracker) {
	if (_active == tracker.get() || ranges::contains(_line, tracker)) {
		return;
	} else if (!_active) {
		_active = tracker;
		tracker->run();
	} else {
		_line.push_back(tracker);
		tracker->wait(_active->peer()->name());
	}
}

void Account::release(not_null<Tracker*> tracker) {
	_line.erase(ranges::remove(_line, tracker), end(_line));
	if (_active == tracker.get()) {
		_active = nullptr;
		if (!_line.empty()) {
			const auto next = _line.front();
			_line.erase(begin(_line));
			_active = next;
			next->run();
			if (_active == next.get()) {
				const auto title = next->peer()->name();
				for (const auto &waiting : _line) {
					waiting->wait(title);
				}
			}
		}
	}
	checkLater();
}

// A tracker may ask for this from inside of its own method, so nothing
// is destroyed right away.
void Account::checkLater() {
	if (_checkScheduled) {
		return;
	}
	_checkScheduled = true;
	crl::on_main(this, [=] {
		_checkScheduled = false;
		check();
	});
}

void Account::check() {
	for (auto i = begin(_trackers); i != end(_trackers);) {
		const auto raw = i->second.get();
		if (raw->busy() || raw->viewed() || raw == _active) {
			++i;
		} else {
			i = _trackers.erase(i);
		}
	}
}

void Account::forget() {
	_line.clear();
	_active = nullptr;
	_trackers.clear();
}

using Accounts = std::map<
	not_null<Main::Session*>,
	std::unique_ptr<Account>>;

// Never destroyed: an account is removed together with its session,
// nothing here should run during the static destruction.
[[nodiscard]] Accounts &AllAccounts() {
	static const auto result = new Accounts();
	return *result;
}

} // namespace

Tracker *TrackerFor(not_null<PeerData*> peer) {
	const auto real = peer->migrateToOrMe();
	if (!Supported(real)) {
		return nullptr;
	}
	const auto session = &real->session();
	auto &accounts = AllAccounts();
	auto i = accounts.find(session);
	if (i == end(accounts)) {
		i = accounts.emplace(
			session,
			std::make_unique<Account>(session)).first;
		session->lifetime().add([=] {
			AllAccounts().erase(session);
		});
	}
	return i->second->tracker(real);
}

namespace {

// Self-test begin.

class Checker final {
public:
	explicit Checker(QStringList &log)
	: _log(log) {
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
		_log.push_back(QString::fromUtf8(name)
			+ u": "_q
			+ QString::number(_passed)
			+ u" checks passed so far"_q);
	}
	[[nodiscard]] int passed() const {
		return _passed;
	}
	[[nodiscard]] int failed() const {
		return _failed;
	}

private:
	QStringList &_log;
	int _passed = 0;
	int _failed = 0;

};

constexpr auto kTestZone = 3 * 3600;
constexpr auto kTestMe = uint64(100);
constexpr auto kTestOther = uint64(200);

[[nodiscard]] TimeId At(
		int year,
		int month,
		int day,
		int hour = 12,
		int minute = 0,
		int second = 0) {
	return TimeId(int64(DayOfDate({ year, month, day })) * kDaySeconds
		+ hour * 3600
		+ minute * 60
		+ second
		- kTestZone);
}

[[nodiscard]] QString U(const char *text) {
	return QString::fromUtf8(text);
}

[[nodiscard]] QString E(std::initializer_list<char32_t> codes) {
	auto result = QString();
	for (const auto code : codes) {
		AppendCode(result, code);
	}
	return result;
}

[[nodiscard]] RawMessage TestMessage(
		int32 id,
		TimeId date,
		uint64 sender,
		const QString &text = QString(),
		Kind kind = Kind::Text,
		uchar flags = 0) {
	auto result = RawMessage();
	result.id = id;
	result.date = date;
	result.sender = sender;
	result.text = text;
	result.kind = kind;
	result.flags = flags;
	return result;
}

[[nodiscard]] RawMessage TestSticker(
		int32 id,
		TimeId date,
		uint64 sender,
		uint64 sticker,
		const QString &emoji) {
	auto result = TestMessage(id, date, sender, QString(), Kind::Sticker);
	result.sticker = sticker;
	result.stickerEmoji = emoji;
	return result;
}

// Answers the requests the way the server does.
struct FakeHistory {
	std::vector<RawMessage> list; // By id.
	std::set<int32> service; // Returned, but not counted.
	bool shifted = false; // A short "newer" answer is filled with older.
	bool broken = false; // Every "older" answer is the newest messages.
	bool direct = true; // A private chat.

	[[nodiscard]] RawPage answer(const Request &request) const {
		auto page = RawPage();
		page.old = request.old;
		page.direct = direct;
		page.type = request.type;
		page.total = int(list.size());
		const auto count = int(list.size());
		const auto index = [&](int32 id) {
			return int(std::lower_bound(
				begin(list),
				end(list),
				id,
				[](const RawMessage &message, int32 id) {
					return message.id < id;
				}) - begin(list));
		};
		auto from = 0;
		auto till = 0;
		if (request.type == PageType::Newer) {
			from = index(request.offsetId);
			till = std::min(count, from + request.limit);
			if (shifted) {
				from = std::max(0, till - request.limit);
			}
		} else {
			till = (request.offsetId && !broken)
				? index(request.offsetId)
				: count;
			from = std::max(0, till - request.limit);
		}
		auto senders = std::set<uint64>();
		for (auto i = from; i < till; ++i) {
			const auto &message = list[i];
			++page.returned;
			page.minId = page.minId
				? std::min(page.minId, message.id)
				: message.id;
			page.maxId = std::max(page.maxId, message.id);
			page.minDate = page.minDate
				? std::min(page.minDate, message.date)
				: message.date;
			page.maxDate = std::max(page.maxDate, message.date);
			if (!service.contains(message.id)) {
				page.messages.push_back(message);
				senders.insert(message.sender);
			}
		}
		for (const auto id : senders) {
			page.names.push_back({ id, u"User %1"_q.arg(id) });
		}
		return page;
	}
};

struct RunResult {
	int requests = 0;
	bool stuck = false;
	bool looped = false;
};

// The same steps as Tracker makes, without the network and the pauses.
// With `reloaded` the cache is written and read back after every answer,
// as if the app was restarted each time.
RunResult RunReading(
		Model &model,
		const Zone &zone,
		const FakeHistory &main,
		const FakeHistory *old,
		TimeId from,
		bool headChecked,
		int limit,
		bool reloaded = false) {
	auto result = RunResult();
	while (true) {
		const auto request = NextRequest(model.coverage(), {
			.from = from,
			.hasOld = (old != nullptr),
			.headChecked = headChecked,
			.limit = limit,
		});
		if (!request.valid) {
			break;
		} else if (++result.requests > 2000) {
			result.looped = true;
			break;
		}
		const auto page = (request.old ? *old : main).answer(request);
		const auto before = model.coverage();
		model.add(page, zone);
		if (reloaded && model.parse(model.serialize()).damaged) {
			result.stuck = true;
			break;
		}
		const auto advanced = ReadingMoved(before, model.coverage());
		if (HeadChecked(request, page.returned, advanced)) {
			headChecked = true;
		}
		if (ReadingStuck(request, page.returned, advanced)) {
			result.stuck = true;
			break;
		}
	}
	return result;
}

struct NamedMonth {
	std::map<QString, uint32> words;
	std::map<QString, uint32> emoji;
	std::map<uint64, uint32> stickers;
	uint32 nameless = 0; // Counted under the keys that have no names.

	friend inline bool operator==(
		const NamedMonth &,
		const NamedMonth &) = default;
};

// The counters of the months by the names, the way the statistics see
// them.
[[nodiscard]] std::map<int32, NamedMonth> Named(const Model &model) {
	auto result = std::map<int32, NamedMonth>();
	for (const auto &[month, bag] : model.months()) {
		auto &named = result[month];
		for (const auto &[id, count] : bag.words) {
			const auto &name = model.words().names[id];
			if (name.isEmpty()) {
				named.nameless += count;
			} else {
				named.words[name] += count;
			}
		}
		for (const auto &[id, count] : bag.emoji) {
			const auto &name = model.emoji().names[id];
			if (name.isEmpty()) {
				named.nameless += count;
			} else {
				named.emoji[name] += count;
			}
		}
		for (const auto &[id, count] : bag.stickers) {
			const auto name = model.stickers().names[id];
			if (!name) {
				named.nameless += count;
			} else {
				named.stickers[name] += count;
			}
		}
	}
	return result;
}

struct KeyedMonth {
	std::map<uint64, uint32> words;
	std::map<uint64, uint32> emoji;
	std::map<uint64, uint32> stickers;

	friend inline bool operator==(
		const KeyedMonth &,
		const KeyedMonth &) = default;
};

// The numbers of the words are the order they were met in, the file has
// another one: everything is compared by the keys.
template <typename Name>
[[nodiscard]] std::map<uint64, std::pair<Name, uint32>> ByKey(
		const Counted<Name> &list) {
	auto result = std::map<uint64, std::pair<Name, uint32>>();
	for (auto i = size_t(0); i != list.keys.size(); ++i) {
		result[list.keys[i]] = { list.names[i], list.totals[i] };
	}
	return result;
}

[[nodiscard]] std::map<int32, KeyedMonth> MonthsByKey(const Model &model) {
	auto result = std::map<int32, KeyedMonth>();
	for (const auto &[month, bag] : model.months()) {
		auto &keyed = result[month];
		for (const auto &[id, count] : bag.words) {
			keyed.words[model.words().keys[id]] += count;
		}
		for (const auto &[id, count] : bag.emoji) {
			keyed.emoji[model.emoji().keys[id]] += count;
		}
		for (const auto &[id, count] : bag.stickers) {
			keyed.stickers[model.stickers().keys[id]] += count;
		}
	}
	return result;
}

[[nodiscard]] bool Same(const Model &a, const Model &b) {
	return (a.count() == b.count())
		&& (a.senders() == b.senders())
		&& (a.days() == b.days())
		&& (a.edges(false) == b.edges(false))
		&& (a.edges(true) == b.edges(true))
		&& (ByKey(a.words()) == ByKey(b.words()))
		&& (ByKey(a.emoji()) == ByKey(b.emoji()))
		&& (ByKey(a.stickers()) == ByKey(b.stickers()))
		&& (MonthsByKey(a) == MonthsByKey(b))
		&& (a.stickerInfos() == b.stickerInfos())
		&& (a.coverage() == b.coverage());
}

// The messages a model has counted, by the local day.
[[nodiscard]] std::map<int32, int> CountedByDay(const Model &model) {
	auto result = std::map<int32, int>();
	for (const auto &[day, data] : model.days()) {
		result[day] = DayMessages(data);
	}
	return result;
}

// Whether a word is anywhere in the bytes as it is.
[[nodiscard]] bool Written(const QByteArray &bytes, const QString &word) {
	return bytes.contains(word.toUtf8());
}

// Whether `second` stands in the bytes right after `first`, the way two
// strings written one after another do: with one byte of a length.
[[nodiscard]] bool Follows(
		const QByteArray &bytes,
		const QString &first,
		const QString &second) {
	const auto a = first.toUtf8();
	const auto b = second.toUtf8();
	for (auto at = bytes.indexOf(a); at >= 0; at = bytes.indexOf(a, at + 1)) {
		const auto next = at + a.size();
		if (bytes.mid(next, b.size()) == b
			|| bytes.mid(next + 1, b.size()) == b) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] QByteArray FileBytes(const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return QByteArray();
	}
	const auto result = file.readAll();
	file.close();
	return result;
}

// A word with a heap of marks above every letter.
[[nodiscard]] QString Zalgo(const QString &word, int marks, char32_t mark) {
	auto result = QString();
	for (const auto ch : word) {
		result.append(ch);
		for (auto i = 0; i != marks; ++i) {
			AppendCode(result, mark);
		}
	}
	return result;
}

// `count` messages an hour apart, every tenth one is a service message.
[[nodiscard]] FakeHistory TestHistory(
		int count,
		TimeId start,
		int32 firstId = 2,
		bool withService = true) {
	const auto texts = std::vector<QString>{
		U("Привет, как дела? Потому что давно не виделись"),
		U("Нормально, работаю над проектом"),
		U("фраза секретная целиком") + E({ 0x1F525 }),
		U("короче, проект почти готов") + E({ 0x1F44D, 0x1F3FD }),
		U("ахахах, отлично"),
		U("Проект готов! ") + E({ 0x2764, 0xFE0F }),
	};
	auto result = FakeHistory();
	for (auto i = 0; i != count; ++i) {
		const auto id = firstId + i * 2;
		const auto date = start + i * 3600;
		const auto sender = (i % 3) ? kTestOther : kTestMe;
		if (withService && (i % 10 == 9)) {
			result.service.insert(id);
			result.list.push_back(TestMessage(id, date, sender));
		} else if (i % 7 == 3) {
			result.list.push_back(TestSticker(
				id,
				date,
				sender,
				uint64(5000000000ULL) + (i % 3),
				E({ 0x1F600 })));
		} else {
			result.list.push_back(TestMessage(
				id,
				date,
				sender,
				texts[i % texts.size()],
				(i % 11 == 5) ? Kind::Photo : Kind::Text,
				uchar((i % 5 == 1) ? kReply : 0)));
		}
	}
	return result;
}

void TestDates(Checker &check) {
	check(DateOfDay(0) == CivilDate{ 1970, 1, 1 }, "day 0 is 1970-01-01");
	check(DayOfDate({ 1970, 1, 1 }) == 0, "1970-01-01 is day 0");
	check(DayOfDate({ 2000, 3, 1 }) == 11017, "2000-03-01");
	check(DayOfDate({ 2024, 2, 29 }) == 19782, "2024-02-29");
	check(DateOfDay(19782) == CivilDate{ 2024, 2, 29 }, "day 19782");
	check(DateOfDay(-1) == CivilDate{ 1969, 12, 31 }, "day -1");
	auto roundTrip = true;
	for (auto day = -1000; day < 30000; day += 37) {
		roundTrip = roundTrip && (DayOfDate(DateOfDay(day)) == day);
	}
	check(roundTrip, "days <-> dates round trip");
	check(WeekdayOfDay(0) == 3, "1970-01-01 is a Thursday");
	check(
		WeekdayOfDay(DayOfDate({ 2026, 10, 6 })) == 1,
		"2026-10-06 is a Tuesday");
	check(
		WeekdayOfDay(DayOfDate({ 2024, 2, 29 })) == 3,
		"2024-02-29 is a Thursday");
	check(WeekdayOfDay(-1) == 2, "1969-12-31 is a Wednesday");
	check(
		MonthOfDay(DayOfDate({ 2026, 3, 31 })) == 2026 * 12 + 2
			&& MonthOfDay(DayOfDate({ 2026, 4, 1 })) == 2026 * 12 + 3,
		"months in a row");
	check(
		FirstDayOfMonth(2026 * 12 + 2) == DayOfDate({ 2026, 3, 1 })
			&& FirstDayOfMonth(2026 * 12 + 12) == DayOfDate({ 2027, 1, 1 })
			&& FirstDayOfMonth(MonthOfDay(-1)) == DayOfDate({ 1969, 12, 1 }),
		"the first day of a month");

	const auto zone = Zone::Fixed(kTestZone);
	const auto day = DayOfDate({ 2026, 10, 6 });
	check(zone->offset(At(2026, 10, 6)) == kTestZone, "fixed zone offset");
	check(zone->day(At(2026, 10, 6, 0, 0)) == day, "local midnight: new day");
	check(
		zone->day(At(2026, 10, 6, 0, 0) - 1) == day - 1,
		"a second before midnight: previous day");
	check(zone->day(At(2026, 10, 6, 23, 59, 59)) == day, "end of the day");
	check(
		zone->dayStart(day) == At(2026, 10, 6, 0, 0),
		"day start is the local midnight");
	check(Zone::Fixed(0)->day(86399) == 0, "UTC: day 0");
	check(Zone::Fixed(-5 * 3600)->day(3600) == -1, "west of UTC");

	// Zones that change the clocks, if the system knows them: the cached
	// offsets must be the exact ones on every day of a year.
	for (const auto id : { "Europe/Berlin", "America/New_York" }) {
		const auto system = QTimeZone(QByteArray(id));
		if (!system.isValid()) {
			continue;
		}
		const auto changing = std::make_shared<Zone>(system);
		auto offsets = true;
		auto days = true;
		const auto till = At(2027, 1, 1);
		for (auto when = At(2026, 1, 1); when < till; when += 11820) {
			const auto exact = system.offsetFromUtc(
				QDateTime::fromSecsSinceEpoch(when, QTimeZone::utc()));
			offsets = offsets && (changing->offset(when) == exact);
			const auto local = changing->day(when);
			days = days
				&& (changing->dayStart(local) <= when)
				&& (when < changing->dayStart(local + 1))
				&& (changing->day(changing->dayStart(local)) == local);
		}
		check(offsets, "a zone that changes the clocks: exact offsets");
		check(days, "a zone that changes the clocks: bounds of the days");
	}

	check(
		PeriodStart(Period::Months3, At(2026, 5, 31), *zone)
			== At(2026, 2, 28, 0, 0),
		"3 months before May 31 is Feb 28");
	check(
		PeriodStart(Period::Months3, At(2026, 2, 10, 0, 30), *zone)
			== At(2025, 11, 10, 0, 0),
		"3 months across a new year");
	check(
		PeriodStart(Period::Year, At(2024, 2, 29), *zone)
			== At(2023, 2, 28, 0, 0),
		"a year before Feb 29");
	check(
		PeriodStart(Period::Year, At(2026, 10, 6, 23, 59), *zone)
			== At(2025, 10, 6, 0, 0),
		"a year back, local midnight");
	check(PeriodStart(Period::All, At(2026, 10, 6), *zone) == 0, "all time");
}

void TestText(Checker &check) {
	const auto list = [](std::initializer_list<const char*> words) {
		auto result = std::vector<QString>();
		for (const auto word : words) {
			result.push_back(U(word));
		}
		return result;
	};
	{
		const auto stats = ParseText(U("Привет, как дела? Всё хорошо!"));
		check(stats.words == 5, "ru: every word is counted");
		check(stats.length == 29, "ru: length in characters");
		check(
			stats.tokens == list({ "привет", "дела", "хорошо" }),
			"ru: stop words are dropped, lower case");
		check(stats.emoji.empty(), "ru: no emoji");
	}
	{
		const auto stats = ParseText(U("Ёлка ещё зелёная"));
		check(
			stats.tokens == list({ "елка", "зеленая" }),
			"ru: yo is counted as ye");
	}
	{
		const auto stats = ParseText(
			U("Don't stop believing, it\xE2\x80\x99s OK"));
		check(stats.words == 5, "en: words with apostrophes");
		check(
			stats.tokens == list({ "stop", "believing" }),
			"en: contractions are stop words, short words are dropped");
	}
	{
		const auto stats = ParseText(U("кто-то что-то 'сделал'"));
		check(stats.words == 5, "a hyphen splits words");
		check(stats.tokens == list({ "сделал" }), "quotes are not letters");
	}
	{
		const auto stats = ParseText(
			U("ахахах хаха АХАХАХАХ haha Hahaha ха ааа"));
		check(stats.words == 7, "laughter: words");
		check(
			stats.tokens == list({
				"ахаха",
				"ахаха",
				"ахаха",
				"haha",
				"haha",
				"ааа",
			}),
			"laughter of any length is one word");
	}
	{
		const auto text = U("смотри https://example.com/page тут");
		const auto stats = ParseText(text, { { 7, 24 } });
		check(stats.words == 2, "a link is not words");
		check(stats.tokens == list({ "смотри" }), "a link is not counted");
		check(stats.length == int(text.size()), "a link has a length");
		const auto plain = ParseText(U("www example http"));
		check(
			plain.tokens == list({ "example" }),
			"parts of links are stop words");
	}
	{
		const auto stats = ParseText(U("в 2025 году 100500 раз, mp3"));
		check(stats.words == 4, "numbers are not words");
		check(stats.tokens == list({ "году" }), "digits split words");
	}
	{
		const auto stats = ParseText(
			U("слово ") + QString(40, QChar('z')) + U(" конец"));
		check(stats.words == 3, "a very long word is a word");
		check(
			stats.tokens == list({ "слово", "конец" }),
			"a very long word is not counted");
	}
	{
		const auto heavy = Zalgo(u"zalgo"_q, 30, 0x0301);
		const auto stats = ParseText(U("слово ") + heavy + U(" конец"));
		check(
			stats.words == 3 && stats.length == 6 + 5 * 31 + 6,
			"marks: a word under a heap of marks is one word");
		check(
			stats.tokens == std::vector<QString>{
				U("слово"),
				Zalgo(u"zalgo"_q, kMaxMarksInRow, 0x0301),
				U("конец"),
			},
			"marks: only a few of them stay above a letter");
		const auto typed = U("и\xCC\x86од");
		check(
			ParseText(typed).tokens == std::vector<QString>{ typed },
			"marks: a letter typed with its mark is a letter");
		const auto wide = ParseText(
			Zalgo(QString(30, QChar(0x0434)), 4, 0x1AB0));
		check(
			wide.words == 1 && wide.tokens.empty(),
			"marks: a word too long for the cache is not counted");
		check(
			Cut(E({ 0x1F600, 0x1F600 }), 3) == E({ 0x1F600 })
				&& Cut(u"abc"_q, 3) == u"abc"_q
				&& Cut(u"abcd"_q, 3) == u"abc"_q,
			"a text is not cut in the middle of a symbol");
	}
	{
		const auto party = E({ 0x1F389 });
		const auto thumb = E({ 0x1F44D });
		const auto heart = E({ 0x2764 });
		const auto heartVaried = E({ 0x2764, 0xFE0F });
		const auto family = E({
			0x1F468,
			0x200D,
			0x1F469,
			0x200D,
			0x1F467,
		});
		const auto flag = E({ 0x1F1F7, 0x1F1FA });
		const auto keycap = E({ '1', 0xFE0F, 0x20E3 });
		const auto text = U("Ура ")
			+ party
			+ party
			+ U(" ")
			+ E({ 0x1F44D, 0x1F3FD })
			+ U(" ")
			+ heartVaried
			+ U(" ")
			+ heart
			+ U(" ")
			+ family
			+ U(" ")
			+ flag
			+ U(" ")
			+ keycap
			+ U(" ок");
		const auto stats = ParseText(text);
		check(
			stats.emoji == std::vector<QString>{
				party,
				party,
				thumb,
				heartVaried,
				heart,
				family,
				flag,
				keycap,
			},
			"emoji: sequences are whole, skin tones are dropped");
		check(stats.length == 21, "emoji: one character each");
		check(stats.words == 2, "emoji are not words");
		check(stats.tokens == list({ "ура" }), "emoji: the words around");
		check(
			EmojiKey(heartVaried) == heart && EmojiKey(heart) == heart,
			"emoji key ignores the variation selector");
	}
	{
		const auto stats = ParseText(
			U("5 минут, #тег * ")
				+ E({ 0x00A9 })
				+ U(" ")
				+ E({ 0x1F1F7 })
				+ U(" 2*2"));
		check(stats.emoji.empty(), "digits and signs are not emoji");
		const auto varied = ParseText(E({ 0x00A9, 0xFE0F }));
		check(varied.emoji.size() == 1, "a sign with the selector is emoji");
		const auto tone = ParseText(E({ 0x1F3FD }));
		check(
			tone.emoji == std::vector<QString>{ E({ 0x1F3FD }) },
			"a lone skin tone stays");
	}
	check(IsStopWord(U("что")), "stop word: ru");
	check(IsStopWord(U("потому")), "stop word: ru, long");
	check(IsStopWord(u"the"_q), "stop word: en");
	check(IsStopWord(u"don't"_q), "stop word: en contraction");
	check(!IsStopWord(U("привет")), "not a stop word: ru");
	check(!IsStopWord(U("короче")), "filler words are counted");
	check(!IsStopWord(u"telegram"_q), "not a stop word: en");
	check(ParseText(QString()).words == 0, "empty text");
}

void TestPlan(Checker &check) {
	const auto zone = Zone::Fixed(kTestZone);
	const auto start = At(2026, 1, 1, 0, 0);
	// The cache has no rows of the messages: that every message is
	// counted once is seen by the counters of the days.
	const auto expected = [&](std::initializer_list<const FakeHistory*> list) {
		auto result = std::map<int32, int>();
		for (const auto history : list) {
			for (const auto &message : history->list) {
				if (!history->service.contains(message.id)) {
					++result[zone->day(message.date)];
				}
			}
		}
		return result;
	};

	check(
		NextRequest(Coverage(), PlanArgs()) == Request{
			.valid = true,
			.type = PageType::First,
			.limit = kPageLimit,
		},
		"the first request asks for the newest messages");

	for (const auto shifted : { false, true }) {
		auto history = TestHistory(250, start);
		history.shifted = shifted;
		auto model = Model();
		const auto first = RunReading(
			model,
			*zone,
			history,
			nullptr,
			0,
			false,
			100);
		check(first.requests == 4, "250 messages: 3 answers and an empty one");
		check(!first.stuck && !first.looped, "250: clean");
		check(model.count() == 225, "250: service is not counted");
		check(
			CountedByDay(model) == expected({ &history }),
			"250: every message once");
		const auto &main = model.coverage().main;
		check(
			main.started && main.reachedStart && main.low == 2
				&& main.high == 500 && main.total == 250,
			"250: the whole range is covered");
		check(main.lowDate == start, "250: the date of the oldest");
		check(Covers(model.coverage(), 0, false), "250: covers everything");
		check(
			ReadingProgress(model.coverage(), 0, false, 225) == 1.,
			"250: progress is complete");

		const auto again = RunReading(
			model,
			*zone,
			history,
			nullptr,
			0,
			false,
			100);
		check(again.requests == 1, "nothing new: one request");
		check(model.count() == 225, "nothing new: nothing added");
		check(
			CountedByDay(model) == expected({ &history }),
			"nothing new: nothing counted twice");

		auto more = TestHistory(130, start + 250 * 3600, 502, false);
		history.list.insert(
			end(history.list),
			begin(more.list),
			end(more.list));
		const auto update = RunReading(
			model,
			*zone,
			history,
			nullptr,
			0,
			false,
			100);
		check(
			update.requests == (shifted ? 3 : 2),
			"130 new: two requests, one more if answers are filled up");
		check(model.count() == 355, "130 new: all added");
		check(
			CountedByDay(model) == expected({ &history }),
			"130 new: every message once");
		check(model.coverage().main.high == 760, "130 new: the range grew");
		check(
			Covers(model.coverage(), 0, false),
			"130 new: still covers everything");
	}
	{
		// A period: the newest 70 messages are inside of it.
		const auto history = TestHistory(250, start);
		const auto from = start + 180 * 3600;
		auto model = Model();
		const auto limited = RunReading(
			model,
			*zone,
			history,
			nullptr,
			from,
			false,
			100);
		check(limited.requests == 1, "period: one answer is enough");
		check(
			!model.coverage().main.reachedStart,
			"period: the start is not reached");
		check(Covers(model.coverage(), from, false), "period: covered");
		check(!Covers(model.coverage(), 0, false), "period: not everything");
		check(
			!NextRequest(model.coverage(), {
				.from = from,
				.headChecked = true,
			}).valid,
			"period: nothing more to ask");
		const auto progress = ReadingProgress(model.coverage(), 0, false, 90);
		check(
			progress > 0.3 && progress < 0.4,
			"period: progress of everything by the count");
		const auto earlier = start + 100 * 3600;
		const auto byDate = ReadingProgress(
			model.coverage(),
			earlier,
			false,
			90);
		check(
			byDate > 0.6 && byDate < 0.7,
			"period: progress of a longer period by the dates");
		check(
			NextRequest(model.coverage(), {
				.from = earlier,
				.headChecked = true,
			}) == Request{
				.valid = true,
				.type = PageType::Older,
				.offsetId = 302,
				.limit = kPageLimit,
			},
			"a longer period asks for older messages");
		check(
			NextRequest(model.coverage(), { .from = from }) == Request{
				.valid = true,
				.type = PageType::Newer,
				.offsetId = 501,
				.addOffset = -kPageLimit,
				.limit = kPageLimit,
			},
			"a new run asks for newer messages first");

		const auto rest = RunReading(
			model,
			*zone,
			history,
			nullptr,
			0,
			true,
			100);
		check(rest.requests == 3, "everything: two answers and an empty one");
		check(
			model.count() == 225
				&& CountedByDay(model) == expected({ &history }),
			"everything: every message");
		check(Covers(model.coverage(), 0, false), "everything: covered");
	}
	{
		// A supergroup made of a basic group.
		const auto oldStart = At(2025, 6, 1, 0, 0);
		const auto old = TestHistory(150, oldStart);
		const auto main = TestHistory(120, start);
		auto model = Model();
		const auto inMain = start + 60 * 3600;
		const auto recent = RunReading(
			model,
			*zone,
			main,
			&old,
			inMain,
			false,
			100);
		check(recent.requests == 1, "old chat: not needed for a period");
		check(!model.coverage().old.started, "old chat: not touched");
		check(Covers(model.coverage(), inMain, true), "old chat: period covered");

		const auto inOld = oldStart + 100 * 3600;
		const auto part = RunReading(model, *zone, main, &old, inOld, true, 100);
		check(part.requests == 3, "old chat: read till the period start");
		check(
			model.coverage().main.reachedStart
				&& model.coverage().old.started
				&& !model.coverage().old.reachedStart,
			"old chat: the old one is read partly");
		check(Covers(model.coverage(), inOld, true), "old chat: covered");
		check(!Covers(model.coverage(), 0, true), "old chat: not everything");

		const auto all = RunReading(model, *zone, main, &old, 0, true, 100);
		check(all.requests == 2, "old chat: the rest and an empty answer");
		check(
			CountedByDay(model) == expected({ &main, &old }),
			"old chat: the supergroup and the basic group");
		check(
			model.count() == 108 + 135
				&& model.edges(true).oldest.known
				&& model.edges(true).oldest.date == oldStart
				&& model.edges(false).oldest.date == start,
			"old chat: both are counted, each one has its ends");
		check(Covers(model.coverage(), 0, true), "old chat: everything");
		check(
			!Covers(model.coverage(), 0, true)
				== NextRequest(model.coverage(), {
					.hasOld = true,
					.headChecked = true,
				}).valid,
			"nothing to ask exactly when everything is covered");
	}
	{
		auto history = FakeHistory();
		auto model = Model();
		const auto empty = RunReading(
			model,
			*zone,
			history,
			nullptr,
			0,
			false,
			100);
		check(empty.requests == 1, "empty chat: one request");
		check(
			model.coverage().main.started
				&& model.coverage().main.reachedStart
				&& Covers(model.coverage(), 0, false),
			"empty chat: covered");
		history = TestHistory(3, start, 2, false);
		const auto filled = RunReading(
			model,
			*zone,
			history,
			nullptr,
			0,
			false,
			100);
		check(filled.requests == 1, "first messages: one request");
		check(model.count() == 3, "first messages: all added");
		check(
			model.coverage().main.low == 2
				&& model.coverage().main.high == 6
				&& model.coverage().main.reachedStart,
			"first messages: the range starts with them");
	}
	{
		auto history = TestHistory(250, start);
		history.broken = true;
		auto model = Model();
		const auto broken = RunReading(
			model,
			*zone,
			history,
			nullptr,
			0,
			false,
			100);
		check(
			broken.stuck && broken.requests == 2,
			"an answer that does not move the reading stops it");
		check(model.count() == 90, "stuck: nothing is added twice");
	}
	check(
		HeadChecked({ .valid = true, .type = PageType::First }, 100, true),
		"the newest answer has the newest messages");
	check(
		!HeadChecked({
			.valid = true,
			.type = PageType::Newer,
			.limit = 100,
		}, 100, true),
		"a full answer with newer messages: there may be more");
	check(
		HeadChecked({
			.valid = true,
			.type = PageType::Newer,
			.limit = 100,
		}, 99, true),
		"a short answer with newer messages: no more");
	check(
		HeadChecked({
			.valid = true,
			.type = PageType::Newer,
			.limit = 100,
		}, 100, false),
		"a full answer with nothing newer: no more");
	check(
		!HeadChecked({
			.valid = true,
			.old = true,
			.type = PageType::First,
		}, 5, true),
		"the old chat says nothing about the newer messages");
	{
		// A full answer of newer messages where some are service ones:
		// the newer messages after it must still be asked for.
		auto history = TestHistory(100, start);
		auto model = Model();
		RunReading(model, *zone, history, nullptr, 0, false, 100);
		const auto more = TestHistory(150, start + 100 * 3600, 202);
		history.list.insert(
			end(history.list),
			begin(more.list),
			end(more.list));
		history.service.insert(begin(more.service), end(more.service));
		const auto update = RunReading(
			model,
			*zone,
			history,
			nullptr,
			0,
			false,
			100);
		check(
			update.requests == 2
				&& CountedByDay(model) == expected({ &history }),
			"service messages in a full answer don't end the reading");
	}
	{
		auto before = Coverage();
		before.main.started = true;
		before.main.low = 10;
		before.main.high = 90;
		auto after = before;
		after.main.total = 500;
		after.checked = 77;
		check(
			!ReadingMoved(before, after),
			"another count of messages does not move the reading");
		after.main.low = 5;
		check(ReadingMoved(before, after), "older messages move it");
		after = before;
		after.main.reachedStart = true;
		check(ReadingMoved(before, after), "the start of the history too");
		after = before;
		after.old.started = true;
		check(ReadingMoved(before, after), "and the old chat");
		check(
			ReadingStuck({ .type = PageType::Older }, 100, false)
				&& !ReadingStuck({ .type = PageType::Older }, 0, false)
				&& !ReadingStuck({ .type = PageType::Older }, 100, true)
				&& !ReadingStuck({ .type = PageType::Newer }, 100, false),
			"only an answer with older messages can be stuck");
	}
	check(FloodSeconds(u"FLOOD_WAIT_17"_q) == 17, "flood wait seconds");
	check(FloodSeconds(u"FLOOD_PREMIUM_WAIT_5"_q) == 5, "premium flood wait");
	check(FloodSeconds(u"FLOOD_WAIT_"_q) == kFloodFallback, "flood: no number");
	check(
		FloodSeconds(u"FLOOD_WAIT_999999999"_q) == kFloodMax,
		"flood: the wait is limited");
	check(FloodSeconds(u"CHANNEL_PRIVATE"_q) == 0, "not a flood error");
}

void TestCache(Checker &check, const QString &folder) {
	const auto zone = Zone::Fixed(kTestZone);
	const auto start = At(2026, 1, 1, 0, 0);
	const auto history = TestHistory(250, start);

	const auto secret = QByteArray("the secret of the test");
	{
		// The key and the inputs of the reference of SipHash-2-4.
		auto input = QByteArray();
		for (auto i = 0; i != 16; ++i) {
			input.append(char(i));
		}
		const auto key = HashKey{
			ReadUint64(input.constData()),
			ReadUint64(input.constData() + 8),
		};
		check(
			KeyedHash(key, QByteArray()) == 0x726FDB47DD0E0E31ULL
				&& KeyedHash(key, input.left(1)) == 0x74F839C593DC67FDULL
				&& KeyedHash(key, input.left(15)) == 0xA129CA6149BE45E5ULL,
			"cache: the keys are made with SipHash");
		const auto salt = QByteArray(kSaltSize, 's');
		check(
			MakeHashKey(secret, salt) == MakeHashKey(secret, salt)
				&& MakeHashKey(secret, salt)
					!= MakeHashKey(QByteArray("another"), salt)
				&& MakeHashKey(secret, salt)
					!= MakeHashKey(secret, QByteArray(kSaltSize, 't')),
			"cache: with a key made of the secret and of the salt");
		check(
			RandomSalt().size() == kSaltSize && RandomSalt() != RandomSalt(),
			"cache: the salt is random");
	}

	auto model = Model(secret);
	const auto run = RunReading(model, *zone, history, nullptr, 0, false, 100);
	check(run.requests == 4 && model.count() == 225, "cache: read");
	check(
		!model.words().keys.empty()
			&& !model.emoji().keys.empty()
			&& model.stickers().keys.size() == 3
			&& model.stickerInfos().size() == 3
			&& model.senders().size() == 2,
		"cache: words, emoji, stickers and senders are collected");
	check(
		model.senders()[0].name.startsWith(u"User "_q),
		"cache: the names of the senders are kept");

	const auto bytes = model.serialize();
	auto parsed = Model(secret);
	const auto result = parsed.parse(bytes);
	check(result.valid && !result.damaged, "cache: the file is parsed");
	check(Same(model, parsed), "cache: the same after a round trip");
	check(
		Named(model) == Named(parsed)
			&& Named(parsed).begin()->second.words[U("секретная")] > 30,
		"cache: the counters find their words again");
	check(parsed.serialize() == bytes, "cache: written the same again");

	// Nothing of the texts: neither the phrases nor the order of words.
	check(
		!Written(bytes, U("фраза секретная")),
		"cache: the texts of the messages are not written");
	check(
		!Written(bytes, U("потому")) && !Written(bytes, U("давно не")),
		"cache: stop words and phrases are not written");
	check(
		Written(bytes, U("секретная")),
		"cache: a word met many times is written");
	check(
		model.words().keys != parsed.words().keys
			&& std::is_sorted(
				begin(parsed.words().keys),
				end(parsed.words().keys))
			&& std::is_sorted(
				begin(parsed.emoji().keys),
				end(parsed.emoji().keys)),
		"cache: the keys are written sorted, not as the words were met");
	{
		auto other = Model(QByteArray("another secret"));
		const auto otherResult = other.parse(bytes);
		check(
			otherResult.valid && otherResult.damaged && !otherResult.outdated,
			"cache: a file made with another secret is not read");
		check(
			other.count() == 0
				&& other.days().empty()
				&& other.words().keys.empty(),
			"cache: and nothing is taken from it");
	}
	{
		auto ordered = false;
		for (const auto &message : history.list) {
			const auto tokens = ParseText(message.text).tokens;
			for (auto i = size_t(1); i < tokens.size(); ++i) {
				if (tokens[i - 1] != tokens[i]
					&& Follows(bytes, tokens[i - 1], tokens[i])) {
					ordered = true;
				}
			}
		}
		check(
			!ordered,
			"cache: no two words stand in the order of a message");
		check(
			Follows(bytes, U("секретная"), U("фраза")),
			"cache: the check of the order sees two words in a row");
	}
	check(
		model.months().size() == 1
			&& model.months().begin()->first == 2026 * 12
			&& !model.months().begin()->second.words.empty(),
		"cache: the counters are kept by the month, not by the day");
	{
		auto cut = Model(secret);
		const auto cutResult = cut.parse(bytes.left(bytes.size() - 5));
		check(
			cutResult.valid && cutResult.damaged,
			"cache: a cut file is damaged");
		check(
			cut.count() == 0
				&& cut.days().empty()
				&& cut.words().keys.empty()
				&& cut.months().empty()
				&& !cut.coverage().main.started,
			"cache: nothing is taken from a cut file");
	}
	{
		auto broken = bytes;
		broken[kFileHeaderSize + kSaltSize + 9] ^= 0x55;
		auto damaged = Model(secret);
		const auto damagedResult = damaged.parse(broken);
		check(
			damagedResult.damaged && damaged.count() == 0,
			"cache: nothing is taken from a damaged file");
	}
	{
		auto longer = Model(secret);
		const auto longerResult = longer.parse(bytes + QByteArray(1, char(0)));
		check(
			longerResult.damaged && longer.count() == 0,
			"cache: a file longer than its header says is not taken");
	}
	{
		// Wrong bytes under a right checksum: taken as other counters or
		// not taken at all, and what is taken can be counted and written.
		const auto payload = bytes.mid(kFileHeaderSize);
		auto taken = 0;
		auto sound = 0;
		for (auto i = qsizetype(0); i < payload.size(); ++i) {
			auto changed = payload;
			changed[i] = char(uchar(changed[i]) ^ uchar(1 << (i % 8)));
			auto wrong = Model(secret);
			if (wrong.parse(WrapFile(changed)).damaged) {
				continue;
			}
			++taken;
			const auto report = Analyze(wrong, *zone, {
				.till = start + 300 * 3600,
			});
			auto again = Model(secret);
			if (report.cached == wrong.count()
				&& !again.parse(wrong.serialize()).damaged
				&& Same(wrong, again)) {
				++sound;
			}
		}
		check(
			WrapFile(payload) == bytes && taken == sound,
			"cache: wrong bytes do not break the reading");
	}
	{
		// Fifty thousands of messages of one day are a few counters.
		const auto count = 50000;
		auto page = RawPage();
		page.direct = true;
		page.messages.reserve(count);
		for (auto i = 0; i != count; ++i) {
			page.messages.push_back(TestMessage(
				i + 1,
				start + i,
				(i % 2) ? kTestMe : kTestOther));
		}
		page.names.push_back({ kTestMe, u"User"_q });
		page.returned = count;
		page.minId = 1;
		page.maxId = count;
		page.minDate = start;
		page.maxDate = start + count - 1;
		page.total = count;
		auto large = Model(secret);
		const auto added = large.add(page, *zone);
		const auto largeBytes = large.serialize();
		auto read = Model(secret);
		const auto readResult = read.parse(largeBytes);
		check(
			added == count
				&& large.count() == count
				&& readResult.valid
				&& !readResult.damaged
				&& Same(large, read),
			"cache: a day of many messages is read back");
		check(
			large.days().size() == 1 && largeBytes.size() < 512,
			"cache: there are counters in it, not a row for every message");
		const auto &persons = large.days().begin()->second.persons;
		check(
			persons.size() == 2
				&& persons[0].answers == Counts<uint16>{ { 1, 24999 } }
				&& persons[1].answers == Counts<uint16>{ { 1, 25000 } },
			"cache: the answer times are counters as well");
	}
	{
		auto none = Model(secret);
		const auto noneResult = none.parse(QByteArray());
		check(
			!noneResult.valid && !noneResult.damaged && none.count() == 0,
			"cache: no file is an empty cache");
		const auto junk = none.parse(QByteArray("not a cache at all"));
		check(
			!junk.valid && junk.damaged && !junk.outdated,
			"cache: a foreign file");
		auto future = bytes;
		future[kFileVersionAt] = char(kFileVersion + 1);
		const auto futureResult = none.parse(future);
		check(
			!futureResult.valid && futureResult.outdated && none.count() == 0,
			"cache: another version");
		for (const auto version : { 1, 2 }) {
			auto old = bytes;
			old[kFileVersionAt] = char(version);
			const auto oldResult = none.parse(old);
			check(
				!oldResult.valid && oldResult.outdated && none.count() == 0,
				"cache: the versions with rows of messages are not read");
		}
		check(
			!none.parse(bytes.left(bytes.size() - 5)).outdated,
			"cache: a broken file of this version is not an old one");
	}
	{
		// A quiet chat: a month with one message, words met once or
		// twice, and one word met in several months. The cache is
		// written and read back after every answer.
		auto quiet = FakeHistory();
		quiet.list = {
			TestMessage(1, At(2026, 1, 10), kTestMe, U("частое утро туман")),
			TestMessage(2, At(2026, 1, 11), kTestOther, U("частое небо туман")),
			TestMessage(
				3,
				At(2026, 2, 5),
				kTestMe,
				U("одинокое тайное послание") + E({ 0x1F525 })),
			TestSticker(4, At(2026, 2, 6), kTestOther, 4242, E({ 0x1F600 })),
			TestMessage(5, At(2026, 3, 1), kTestOther, U("частое море")),
			TestMessage(6, At(2026, 4, 2), kTestMe, U("Частое")),
		};
		const auto rare = {
			U("утро"),
			U("туман"),
			U("небо"),
			U("одинокое"),
			U("тайное"),
			U("послание"),
			U("море"),
		};
		const auto written = [&](const QByteArray &bytes) {
			for (const auto &word : rare) {
				if (Written(bytes, word)) {
					return true;
				}
			}
			return Written(bytes, E({ 0x1F525 }))
				|| Written(bytes, E({ 0x1F600 }));
		};
		auto paged = Model(secret);
		const auto read = RunReading(
			paged,
			*zone,
			quiet,
			nullptr,
			0,
			false,
			1,
			true);
		const auto quietBytes = paged.serialize();
		check(
			read.requests == 7 && !read.stuck && paged.count() == 6,
			"rare: read by one message with a restart after each");
		check(
			!written(quietBytes),
			"rare: what is met once or twice is not written as it is");
		check(
			Written(quietBytes, U("частое")),
			"rare: a word met three times is");
		const auto named = Named(paged);
		const auto month = [&](int number) {
			const auto i = named.find(2026 * 12 + number - 1);
			return (i != end(named)) ? i->second : NamedMonth();
		};
		check(
			named.size() == 4
				&& month(1).words == std::map<QString, uint32>{
					{ U("частое"), 2 },
				}
				&& month(1).nameless == 4
				&& month(3).words == std::map<QString, uint32>{
					{ U("частое"), 1 },
				}
				&& month(3).nameless == 1
				&& month(4).words == std::map<QString, uint32>{
					{ U("частое"), 1 },
				}
				&& month(4).nameless == 0,
			"rare: the word has its counters of every month");
		check(
			month(2).words.empty()
				&& month(2).emoji.empty()
				&& month(2).stickers.empty()
				&& month(2).nameless == 5,
			"rare: of a month with one message only numbers are kept");
		check(
			paged.stickers().keys.size() == 1
				&& paged.stickers().names[0] == 0
				&& paged.stickerInfos().empty(),
			"rare: neither the id nor the message of a sticker met once");
		const auto report = Analyze(paged, *zone, {});
		check(
			report.topWords == std::vector<TopText>{ { U("частое"), 4 } }
				&& report.topEmoji.empty()
				&& report.topStickers.empty(),
			"rare: only the named ones get to the statistics");

		// The same history read at once gives the same counters.
		auto whole = Model(secret);
		RunReading(whole, *zone, quiet, nullptr, 0, false, 100);
		check(
			whole.days() == paged.days()
				&& Named(whole) == named
				&& !written(whole.serialize()),
			"rare: however the history is cut into the answers");

		// The counters of a month can't be told from the file: the same
		// history under another secret has other keys for the same words.
		auto foreign = Model(QByteArray("another secret"));
		RunReading(foreign, *zone, quiet, nullptr, 0, false, 100);
		auto common = 0;
		for (const auto key : foreign.words().keys) {
			common += paged.words().index.contains(key) ? 1 : 0;
		}
		check(
			foreign.words().keys.size() == paged.words().keys.size()
				&& !common,
			"rare: the keys of the words are made with the secret");
	}
	const auto heavy = Zalgo(u"zalgotext"_q, 20, 0x0301);
	const auto wide = Zalgo(QString(30, QChar(0x0434)), 4, 0x1AB0);
	const auto marksPage = [&] {
		auto page = RawPage();
		page.messages.push_back(TestMessage(
			1,
			start,
			kTestMe,
			heavy + U(" привет ") + wide + U(" привет")));
		page.returned = 1;
		page.minId = 1;
		page.maxId = 1;
		page.minDate = start;
		page.maxDate = start;
		return page;
	}();
	{
		// Heaps of marks above the letters: the words take more bytes
		// than the cache reads back.
		check(
			heavy.toUtf8().size() > kMaxWordBytes
				&& wide.toUtf8().size() > kMaxWordBytes,
			"cache: the words with the marks are long");
		auto marked = Model(secret);
		auto thrice = RawPage(marksPage);
		for (const auto id : { 2, 3 }) {
			thrice.messages.push_back(thrice.messages.front());
			thrice.messages.back().id = id;
		}
		thrice.returned = 3;
		thrice.maxId = 3;
		const auto taken = marked.add(thrice, *zone);
		auto back = Model(secret);
		const auto backResult = back.parse(marked.serialize());
		check(
			taken == 3 && backResult.valid && !backResult.damaged,
			"cache: a text with heaps of marks is written and read back");
		auto totals = back.words().totals;
		std::sort(begin(totals), end(totals));
		check(
			Same(marked, back)
				&& back.count() == 3
				&& totals == std::vector<uint32>{ 3, 6 },
			"cache: with the words short enough to be kept");
		auto longest = 0;
		for (const auto &name : back.words().names) {
			longest = std::max(longest, int(name.toUtf8().size()));
		}
		check(
			longest > 0 && longest <= kMaxWordBytes,
			"cache: and no name longer than the file takes");
		auto name = RawPage(marksPage);
		name.names.push_back({
			kTestMe,
			QString(kMaxNameUnits - 1, QChar('n')) + E({ 0x1F600 }),
		});
		auto named = Model(secret);
		const auto namedTaken = named.add(name, *zone);
		auto namedBack = Model(secret);
		const auto namedResult = namedBack.parse(named.serialize());
		check(
			namedTaken == 1 && !namedResult.damaged
				&& named.senders()[0].name.size() == kMaxNameUnits - 1
				&& Same(named, namedBack),
			"cache: a long name is cut between its symbols");
	}

	// The file.
	QDir(folder).removeRecursively();
	const auto path = folder + u"12345.dat"_q;
	const auto pageOf = [&](const Model &model, bool headChecked) {
		return history.answer(NextRequest(model.coverage(), {
			.headChecked = headChecked,
		}));
	};
	auto firstSaved = QByteArray();
	{
		auto storage = Storage(path, zone);
		const auto nothing = storage.load();
		check(
			nothing.messages == 0 && !nothing.failed && !QFile::exists(path),
			"file: nothing at first");
		const auto first = storage.add(pageOf(storage.model(), false));
		check(
			first.accepted == 90 && first.advanced
				&& first.state.messages == 90,
			"file: the first answer");
		check(
			storage.unsaved() && !storage.writes() && !QFile::exists(path),
			"file: an answer alone is not written");
		const auto second = storage.add(pageOf(storage.model(), true));
		check(second.accepted == 90, "file: the second answer");
		const auto finished = storage.finish(1234567);
		check(
			finished.coverage.checked == 1234567
				&& !storage.unsaved()
				&& storage.writes() == 1,
			"file: written when the reading ends");
		firstSaved = FileBytes(path);
		check(
			!firstSaved.isEmpty()
				&& firstSaved == storage.model().serialize(),
			"file: it is the whole cache");
		storage.setPeriod(Period::Year);
		storage.setStopped(true);
		storage.setStopped(true);
		storage.setPeriod(Period::Year);
		storage.flush();
		check(storage.writes() == 3, "file: written on a change only");

		auto reopened = Storage(path, zone);
		const auto state = reopened.load();
		check(
			state.messages == 180
				&& !state.failed
				&& state.coverage.checked == 1234567
				&& state.coverage.period == Period::Year
				&& state.coverage.stopped,
			"file: the state is read back");
		check(Same(storage.model(), reopened.model()), "file: the same data");
		check(
			!reopened.writes() && !reopened.unsaved(),
			"file: reading does not write");
		check(
			QDir(folder).entryList(QDir::Files).size() == 1,
			"file: nothing is left beside the cache");
		reopened.setStopped(false);
		auto continued = Storage(path, zone);
		check(
			!continued.load().coverage.stopped,
			"file: a continued reading is not stopped any more");
	}
	{
		auto storage = Storage(path, zone);
		storage.load();
		const auto before = FileBytes(path);
		const auto third = storage.add(pageOf(storage.model(), true));
		const auto last = storage.add(pageOf(storage.model(), true));
		check(
			third.accepted == 45 && last.accepted == 0 && last.advanced
				&& last.state.coverage.main.reachedStart,
			"file: the rest and the end of the history");
		const auto repeated = storage.add(pageOf(storage.model(), false));
		check(
			repeated.accepted == 0 && !repeated.advanced,
			"file: an answer with nothing new changes nothing");
		check(
			!storage.writes() && FileBytes(path) == before,
			"file: not touched between the writes");

		// A crash at this moment: the answers counted after the last
		// write are lost, what is in the file is whole.
		auto crashed = Storage(path, zone);
		const auto old = crashed.load();
		check(
			old.messages == 180
				&& !old.failed
				&& !old.coverage.main.reachedStart,
			"file: after a crash the cache is the last one written");
		check(
			NextRequest(old.coverage, { .headChecked = true }) == Request{
				.valid = true,
				.type = PageType::Older,
				.offsetId = old.coverage.main.low,
				.limit = kPageLimit,
			},
			"file: the lost answers are asked for again");

		storage.flush();
		const auto after = FileBytes(path);
		check(
			storage.writes() == 1
				&& !storage.unsaved()
				&& after == storage.model().serialize(),
			"file: a flush writes what is not written");
		check(
			!after.startsWith(before),
			"file: written anew, not appended to");
		storage.flush();
		check(storage.writes() == 1, "file: nothing is written twice");

		auto again = Storage(path, zone);
		again.load();
		check(Same(storage.model(), again.model()), "file: read back");

		const auto report = again.analyze({ .till = start + 300 * 3600 });
		check(
			report.messages == 225 && report.wholeHistory && report.complete,
			"file: the statistics of the read cache");

		again.clear();
		check(!QFile::exists(path) && !again.unsaved(), "file: cleared");
		again.setPeriod(Period::Months3);
		again.flush();
		check(
			!QFile::exists(path),
			"file: an empty cache with the usual period is not written");

		// "Clear data" with another period chosen.
		again.clear();
		again.setPeriod(Period::All);
		auto empty = Storage(path, zone);
		const auto state = empty.load();
		check(
			state.messages == 0
				&& !state.coverage.main.started
				&& !state.coverage.stopped
				&& state.coverage.period == Period::All,
			"file: the chosen period stays after clearing");
		empty.clear();
		check(!QFile::exists(path), "file: cleared again");
	}
	{
		// A long reading writes the file once in a while.
		const auto single = TestHistory(300, start, 2, false);
		auto storage = Storage(path, zone);
		auto headChecked = false;
		auto answers = 0;
		for (auto i = 0; i != 400; ++i) {
			const auto request = NextRequest(storage.model().coverage(), {
				.headChecked = headChecked,
				.limit = 1,
			});
			if (!request.valid) {
				break;
			}
			++answers;
			storage.add(single.answer(request));
			headChecked = true;
		}
		check(
			answers == 301 && storage.model().count() == 300,
			"file: an answer for every message and an empty one");
		check(
			storage.writes() == answers / kSaveAnswers && storage.unsaved(),
			"file: written once in twenty answers");
		auto partial = Storage(path, zone);
		const auto state = partial.load();
		check(
			!state.failed
				&& state.messages == 300
				&& !state.coverage.main.reachedStart,
			"file: a crash loses less than twenty answers");
		storage.flush();
		auto whole = Storage(path, zone);
		whole.load();
		check(
			whole.model().coverage().main.reachedStart
				&& Same(storage.model(), whole.model()),
			"file: all of it after a flush");
	}
	{
		// A file that is there and can't be read: a folder in its place
		// does that on every system.
		const auto good = FileBytes(path);
		const auto aside = folder + u"aside.dat"_q;
		check(
			!good.isEmpty() && QFile::rename(path, aside) && QDir().mkpath(path),
			"file: something unreadable in the place of the cache");
		auto storage = Storage(path, zone);
		const auto failed = storage.load();
		check(
			failed.failed && failed.messages == 0,
			"file: not read is not the same as empty");
		storage.add(pageOf(storage.model(), false));
		storage.finish(42);
		storage.setPeriod(Period::Year);
		storage.setStopped(true);
		storage.flush();
		check(
			!storage.writes() && QFileInfo(path).isDir(),
			"file: nothing is written over what could not be read");
		check(
			QDir().rmdir(path) && QFile::rename(aside, path),
			"file: the cache is in its place again");
		const auto state = storage.reload();
		check(
			!state.failed
				&& state.messages == 300
				&& state.coverage.checked != 42
				&& !storage.unsaved(),
			"file: read at the next try");
		storage.flush();
		check(
			!storage.writes() && FileBytes(path) == good,
			"file: and it is what it was");
	}
	{
		// A cache made with another secret, by another copy of the app:
		// its counters can't be matched to the words.
		const auto good = FileBytes(path);
		auto storage = Storage(path, zone, secret);
		const auto state = storage.load();
		storage.add(pageOf(storage.model(), false));
		storage.finish(42);
		check(
			state.failed && state.messages == 0 && !storage.writes(),
			"file: a cache made with another secret is reported");
		check(
			!good.isEmpty() && FileBytes(path) == good,
			"file: and is neither replaced nor removed");
	}
	for (const auto version : { 1, 2 }) {
		// The versions before had a row for every message and the words
		// as they are, the first one in the order of the messages.
		auto old = firstSaved;
		old[kFileVersionAt] = char(version);
		auto file = QFile(path);
		const auto written = file.open(QIODevice::WriteOnly)
			&& (file.write(old) == old.size());
		file.close();
		auto storage = Storage(path, zone);
		const auto state = storage.load();
		check(
			written && state.messages == 0 && !state.failed,
			"file: a cache of a version before is not read");
		check(!QFile::exists(path), "file: and is removed");
	}
	{
		// A broken file of this version is not thrown away without a
		// word: the history would be asked for again by itself.
		auto broken = firstSaved;
		broken[broken.size() - 3] ^= 0x55;
		auto file = QFile(path);
		const auto written = file.open(QIODevice::WriteOnly)
			&& (file.write(broken) == broken.size());
		file.close();
		auto storage = Storage(path, zone);
		const auto state = storage.load();
		storage.finish(42);
		check(
			written && state.failed && state.messages == 0,
			"file: a broken cache is reported");
		check(
			storage.reload().failed && FileBytes(path) == broken,
			"file: and is neither replaced nor removed");
		storage.clear();
		check(
			!QFile::exists(path) && !storage.load().failed,
			"file: till the data is cleared");
	}
	{
		auto storage = Storage(path, zone, secret);
		const auto added = storage.add(marksPage);
		storage.finish(7);
		auto reopened = Storage(path, zone, secret);
		const auto state = reopened.load();
		check(
			added.accepted == 1 && state.messages == 1 && !state.failed,
			"file: a word with heaps of marks does not break the cache");
		check(
			Same(storage.model(), reopened.model())
				&& QFile::exists(path),
			"file: it is read back as it was counted");
	}
	{
		// The old caches of the chats that are never opened again.
		const auto write = [&](const QString &name, const QByteArray &bytes) {
			auto file = QFile(folder + name);
			const auto done = file.open(QIODevice::WriteOnly)
				&& (file.write(bytes) == bytes.size());
			file.close();
			return done;
		};
		auto old = firstSaved;
		old[kFileVersionAt] = char(1);
		auto second = firstSaved;
		second[kFileVersionAt] = char(2);
		const auto written = write(u"1.dat"_q, old)
			&& write(u"2.dat"_q, firstSaved)
			&& write(u"3.dat"_q, QByteArray("something else"))
			&& write(u"4.txt"_q, old)
			&& write(u"5.dat"_q, second);
		RemoveOutdatedCaches(folder);
		check(
			written
				&& !QFile::exists(folder + u"1.dat"_q)
				&& !QFile::exists(folder + u"5.dat"_q),
			"file: the caches of the versions before are removed");
		check(
			QFile::exists(folder + u"2.dat"_q)
				&& QFile::exists(folder + u"3.dat"_q)
				&& QFile::exists(folder + u"4.txt"_q)
				&& QFile::exists(path),
			"file: nothing else in the folder is touched");
	}
	QDir(folder).removeRecursively();
}

void TestAnalyze(Checker &check) {
	const auto zone = Zone::Fixed(kTestZone);
	const auto thumb = E({ 0x1F44D });
	const auto party = E({ 0x1F389 });
	const auto smile = E({ 0x1F600 });
	const auto build = [&](
			std::vector<RawMessage> list,
			int limit = 100,
			bool reloaded = false) {
		auto model = Model();
		auto history = FakeHistory();
		history.list = std::move(list);
		RunReading(model, *zone, history, nullptr, 0, false, limit, reloaded);
		return model;
	};
	const auto messages = std::vector<RawMessage>{
		TestMessage(1, At(2026, 3, 2, 9, 0), kTestOther, U("Привет! Как дела?")),
		TestMessage(
			2,
			At(2026, 3, 2, 9, 2),
			kTestMe,
			U("Привет, отлично ") + thumb,
			Kind::Text,
			kReply),
		TestMessage(3, At(2026, 3, 2, 9, 10), kTestOther, QString(), Kind::Photo),
		TestMessage(4, At(2026, 3, 2, 9, 11), kTestOther, U("смотри фото")),
		TestMessage(
			5,
			At(2026, 3, 2, 23, 30),
			kTestMe,
			U("супер, привет Маше ") + E({ 0x1F44D, 0x1F3FB }) + party),
		TestSticker(6, At(2026, 3, 3, 0, 15), kTestOther, 777, smile),
		TestMessage(
			7,
			At(2026, 3, 3, 10, 0),
			kTestMe,
			U("новости дня"),
			Kind::Text,
			uchar(kForwarded | kLink)),
		TestMessage(8, At(2026, 3, 5, 12, 0), kTestOther, QString(), Kind::Voice),
		TestMessage(
			9,
			At(2026, 3, 5, 12, 0, 30),
			kTestMe,
			U("ок привет ") + thumb),
		TestSticker(10, At(2026, 3, 5, 12, 5), kTestOther, 777, smile),
		TestSticker(11, At(2026, 3, 5, 12, 6), kTestOther, 777, smile),
		TestMessage(12, At(2026, 3, 6, 8, 0), kTestMe, U("доброе утро")),
	};
	const auto model = build(messages);
	const auto dayOf = [](int month, int day) {
		return DayOfDate({ 2026, month, day });
	};
	{
		// The answers and the counters do not depend on the portions the
		// history comes in, with a restart of the app between them too.
		const auto paged = build(messages, 2, true);
		check(
			paged.count() == 12
				&& paged.days() == model.days()
				&& Named(paged) == Named(model)
				&& paged.edges(false) == model.edges(false),
			"portions: older messages by two give the same counters");
		auto history = FakeHistory();
		history.list.assign(begin(messages), begin(messages) + 6);
		auto grown = Model();
		RunReading(grown, *zone, history, nullptr, 0, false, 100, true);
		history.list = messages;
		const auto update = RunReading(
			grown,
			*zone,
			history,
			nullptr,
			0,
			false,
			100,
			true);
		check(
			update.requests == 1
				&& grown.days() == model.days()
				&& grown.edges(false) == model.edges(false),
			"portions: newer messages give the same counters");
		check(
			model.edges(false).oldest == Edge{ At(2026, 3, 2, 9, 0), 0, true }
				&& model.edges(false).newest
					== Edge{ At(2026, 3, 6, 8, 0), 1, true }
				&& !model.edges(true).oldest.known,
			"portions: the ends of what is counted");
		check(
			AnswerSlot(0) == 0
				&& AnswerSlot(59) == 59
				&& AnswerSlot(60) == 60
				&& AnswerSlot(89) == 60
				&& AnswerSlot(90) == 61
				&& AnswerSlot(kAnswerMax) == kAnswerSlots - 1
				&& AnswerSlotTime(59) == 59
				&& AnswerSlotTime(60) == 60
				&& AnswerSlotTime(61) == 120
				&& AnswerSlotTime(kAnswerSlots - 1) == kAnswerMax,
			"portions: the time of an answer is kept to the minute");
	}
	{
		const auto report = Analyze(model, *zone, {
			.till = At(2026, 3, 6, 20, 0),
		});
		check(report.messages == 12 && report.cached == 12, "all: messages");
		check(report.textMessages == 7, "all: messages with a text");
		check(report.words == 16, "all: words");
		check(report.emoji == 4, "all: emoji");
		check(report.chars > 0, "all: characters");
		check(report.complete && report.wholeHistory, "all: complete");
		check(
			report.persons.size() == 2
				&& report.persons[0].id == kTestOther
				&& report.persons[0].messages == 7
				&& report.persons[1].id == kTestMe
				&& report.persons[1].messages == 5,
			"all: messages per person, the most active first");
		check(
			report.persons[0].media == 5 && report.persons[1].media == 0,
			"all: media per person");
		check(
			report.persons[0].name == u"User 200"_q,
			"all: the names of the persons");
		check(
			report.persons[1].answers == 3
				&& report.persons[1].answerTime == 120,
			"all: my answer time is the median, long waits are skipped");
		check(
			report.persons[0].answers == 3
				&& report.persons[0].answerTime == 480,
			"all: the answer time of the other one");
		auto hours = std::array<int, 24>();
		hours[9] = 4;
		hours[23] = 1;
		hours[0] = 1;
		hours[10] = 1;
		hours[12] = 4;
		hours[8] = 1;
		check(report.hours == hours, "all: by local hour");
		check(
			report.weekdays == std::array<int, 7>{ 5, 2, 0, 4, 1, 0, 0 },
			"all: by weekday from Monday");
		check(
			report.firstDay == dayOf(3, 2)
				&& report.lastDay == dayOf(3, 6)
				&& report.days == std::vector<int>{ 5, 2, 0, 4, 1 }
				&& report.activeDays == 4,
			"all: by local day, with the empty ones");
		check(
			report.months == std::vector<MonthValue>{ { 2026, 3, 12 } },
			"all: by month");
		check(
			report.longest == Streak{ dayOf(3, 2), 2 },
			"all: the longest streak, the first of the equal ones");
		check(
			report.current == Streak{ dayOf(3, 5), 2 },
			"all: the current streak");
		check(
			report.topDays == std::vector<DayValue>{
				{ dayOf(3, 2), 5 },
				{ dayOf(3, 5), 4 },
				{ dayOf(3, 3), 2 },
			},
			"all: the most active days");
		auto kinds = std::array<int, kKindCount>();
		kinds[int(Kind::Text)] = 7;
		kinds[int(Kind::Photo)] = 1;
		kinds[int(Kind::Sticker)] = 3;
		kinds[int(Kind::Voice)] = 1;
		check(report.kinds == kinds, "all: by kind");
		check(
			report.links == 1 && report.forwards == 1 && report.replies == 1,
			"all: links, forwards, replies");
		check(
			report.words == 16
				&& report.words == report.persons[0].words
					+ report.persons[1].words,
			"all: the words of the persons add up");
		check(
			report.firstDate == At(2026, 3, 2, 9, 0)
				&& report.firstSender == kTestOther
				&& report.lastDate == At(2026, 3, 6, 8, 0),
			"all: the first and the last message");
		check(
			report.topWords == std::vector<TopText>{ { U("привет"), 4 } },
			"all: top words, the ones met once are not known");
		check(
			report.topEmoji == std::vector<TopText>{ { thumb, 3 } },
			"all: top emoji, skin tones together, without the one met once");
		check(
			report.topStickers.size() == 1
				&& report.topStickers[0].id == 777
				&& report.topStickers[0].count == 3
				&& report.topStickers[0].info.msgId == 11
				&& !report.topStickers[0].info.old
				&& report.topStickers[0].info.emoji == smile,
			"all: top stickers with a message of each");
	}
	{
		const auto report = Analyze(model, *zone, {});
		check(
			report.messages == 12 && report.current.days == 0,
			"no current streak without the current time");
		const auto later = Analyze(model, *zone, {
			.till = At(2026, 3, 8, 9, 0),
		});
		check(
			later.current.days == 0 && later.longest.days == 2,
			"a streak that ended two days ago is not current");
		const auto nextDay = Analyze(model, *zone, {
			.till = At(2026, 3, 7, 23, 0),
		});
		check(
			nextDay.current == Streak{ dayOf(3, 5), 2 },
			"a streak that ended yesterday is still current");
	}
	{
		const auto report = Analyze(model, *zone, {
			.from = At(2026, 3, 5, 0, 0),
			.till = At(2026, 3, 6, 20, 0),
		});
		check(
			report.messages == 5 && report.cached == 12,
			"period: only its messages");
		check(
			report.persons.size() == 2
				&& report.persons[0].id == kTestOther
				&& report.persons[0].messages == 3
				&& report.persons[1].messages == 2,
			"period: persons");
		check(
			report.firstDay == dayOf(3, 5)
				&& report.days == std::vector<int>{ 4, 1 },
			"period: days");
		// Words, emoji and stickers are counted by the month, and most
		// of the messages of March are before this period.
		check(
			FirstCountedMonth(model, *zone, At(2026, 3, 5, 0, 0))
				== 2026 * 12 + 3,
			"period: a month mostly outside of it is not counted");
		check(
			report.topWords.empty()
				&& report.topEmoji.empty()
				&& report.topStickers.empty(),
			"period: no words, emoji and stickers of that month");
		check(
			report.firstDate == 0
				&& report.firstSender == 0
				&& report.lastDate == At(2026, 3, 6, 8, 0),
			"period: the first message of the chat is not in it");
		check(report.complete && report.wholeHistory, "period: complete");
		check(
			report.persons[1].answers == 1
				&& report.persons[1].answerTime == 30
				&& report.persons[0].answers == 1
				&& report.persons[0].answerTime == 300,
			"period: answers inside of it");
		const auto early = Analyze(model, *zone, {
			.from = At(2026, 3, 1, 0, 0),
		});
		check(
			early.messages == 12
				&& early.firstDate == At(2026, 3, 2, 9, 0)
				&& early.firstSender == kTestOther,
			"period: the first message when the period starts before it");
	}
	{
		// Most of the messages of March are inside of this period: the
		// counters of the whole month are taken.
		const auto from = At(2026, 3, 3, 0, 0);
		const auto report = Analyze(model, *zone, { .from = from });
		check(
			FirstCountedMonth(model, *zone, from) == 2026 * 12 + 2,
			"months: a month mostly inside of the period is counted");
		check(
			report.messages == 7
				&& report.topWords == std::vector<TopText>{
					{ U("привет"), 4 },
				}
				&& report.topEmoji == std::vector<TopText>{ { thumb, 3 } },
			"months: with all the words and emoji of that month");
		check(
			report.topStickers.size() == 1
				&& report.topStickers[0].id == 777
				&& report.topStickers[0].count == 3,
			"months: and all its stickers");
		check(
			FirstCountedMonth(model, *zone, 0)
				== std::numeric_limits<int32>::min(),
			"months: all of them without a period");
		check(
			FirstCountedMonth(model, *zone, At(2026, 2, 20, 0, 0))
				== 2026 * 12 + 1,
			"months: a month without messages changes nothing");

		// Nothing older than the period was read: its first month is
		// counted even when the period starts at the end of it.
		const auto late = build({
			TestMessage(1, At(2026, 3, 29, 10, 0), kTestMe, U("привет привет")),
			TestMessage(2, At(2026, 4, 2, 10, 0), kTestOther, U("привет")),
		});
		const auto lateReport = Analyze(late, *zone, {
			.from = At(2026, 3, 28, 0, 0),
		});
		check(
			lateReport.topWords == std::vector<TopText>{ { U("привет"), 3 } },
			"months: the first month of a period with nothing before it");

		// 00:30 of April 1 here is still March in UTC.
		const auto night = build({
			TestMessage(1, At(2026, 4, 1, 0, 30), kTestMe, U("полночь")),
		});
		check(
			night.months().size() == 1
				&& night.months().begin()->first == 2026 * 12 + 3,
			"months: by the local time");
	}
	{
		const auto report = Analyze(model, *zone, {
			.group = true,
		});
		check(
			report.group
				&& report.persons[0].answers == 0
				&& report.persons[1].answers == 0,
			"group: no answer time");
	}
	{
		const auto report = Analyze(model, *zone, {
			.from = At(2026, 4, 1, 0, 0),
			.till = At(2026, 4, 2, 0, 0),
		});
		check(
			report.messages == 0
				&& report.persons.empty()
				&& report.days.empty()
				&& report.months.empty()
				&& report.topDays.empty()
				&& report.longest.days == 0
				&& report.cached == 12,
			"an empty period");
	}
	{
		const auto heart = E({ 0x2764 });
		const auto heartVaried = E({ 0x2764, 0xFE0F });
		const auto hearts = build({
			TestMessage(1, At(2025, 12, 31, 23, 0), kTestMe, heartVaried),
			TestMessage(2, At(2026, 2, 1, 1, 0), kTestOther, heart),
			TestMessage(
				3,
				At(2026, 2, 1, 1, 0),
				kTestOther,
				heartVaried + U(" текст ") + thumb),
		});
		const auto report = Analyze(hearts, *zone, {
			.till = At(2026, 2, 2, 10, 0),
		});
		check(
			report.topEmoji == std::vector<TopText>{ { heartVaried, 3 } },
			"emoji with and without the selector are one");
		check(
			hearts.emoji().keys.size() == 2
				&& Named(hearts)[2026 * 12 + 1].emoji
					== std::map<QString, uint32>{ { heartVaried, 2 } }
				&& Named(hearts)[2026 * 12 + 1].nameless == 2,
			"emoji: by the month, the one met once has no name");
		check(
			report.months == std::vector<MonthValue>{
				{ 2025, 12, 1 },
				{ 2026, 1, 0 },
				{ 2026, 2, 2 },
			},
			"months without gaps");
		check(
			report.days.size() == 33
				&& report.days.front() == 1
				&& report.days.back() == 2
				&& report.activeDays == 2,
			"days across a new year");
		check(
			report.longest == Streak{ DayOfDate({ 2025, 12, 31 }), 1 }
				&& report.current == Streak{ dayOf(2, 1), 1 },
			"streaks of single days");
		check(
			report.weekdays[2] == 1 && report.weekdays[6] == 2,
			"a Wednesday and a Sunday");
	}
	{
		// Read only partly: not complete for a longer period.
		auto partial = Model();
		const auto history = TestHistory(250, At(2026, 1, 1, 0, 0));
		const auto from = At(2026, 1, 1, 0, 0) + 180 * 3600;
		RunReading(partial, *zone, history, nullptr, from, false, 100);
		const auto inside = Analyze(partial, *zone, { .from = from });
		const auto longer = Analyze(partial, *zone, {});
		check(
			inside.complete && !inside.wholeHistory,
			"partly read: the period is complete");
		check(
			!longer.complete && !longer.wholeHistory && longer.messages == 90,
			"partly read: everything is not");
		check(
			inside.firstDate == 0 && longer.firstDate == 0,
			"partly read: the oldest read message is not called the first");
	}
}

} // namespace

bool RunSelfTest(QStringList &log) {
	auto check = Checker(log);
	TestDates(check);
	check.section("dates");
	TestText(check);
	check.section("text");
	TestPlan(check);
	check.section("plan");
	TestCache(check, cWorkingDir() + u"oblivion_selftest_stats/"_q);
	check.section("cache");
	TestAnalyze(check);
	check.section("analyze");
	log.push_back(u"chat_stats: %1 passed, %2 failed"_q.arg(
		QString::number(check.passed()),
		QString::number(check.failed())));
	return !check.failed();
}

} // namespace Oblivion::ChatStats

namespace Oblivion {

void ForgetChatStats(not_null<Main::Session*> session) {
	using namespace ChatStats;

	auto &accounts = AllAccounts();
	const auto i = accounts.find(session);
	if (i != end(accounts)) {
		i->second->forget();
	}
	Queue().async([folder = CacheFolder(session)] {
		QDir(folder).removeRecursively();
	});
}

} // namespace Oblivion
