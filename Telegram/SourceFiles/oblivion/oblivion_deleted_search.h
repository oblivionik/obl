/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <array>
#include <atomic>

class PeerData;

namespace Window {
class SessionController;
} // namespace Window

// Round 5: search in the deleted and edited messages, the ones kept by
// the store of oblivion_deleted_store.h: a query, filters by chat, period
// and kind, the matches highlighted, a result opens the saved message.
//
// Everything stays on this device. The store is copied once on the main
// thread (the texts are shared, not duplicated), the search itself runs
// on another thread and can be cancelled by the next query.
namespace Oblivion::DeletedSearch {

enum class Source : uchar {
	Deleted,
	Edited, // A text a message had before it was edited.
};

// One saved text, a plain copy that may be read on any thread.
struct Entry {
	Source source = Source::Deleted;
	uint64 peerId = 0;
	int64 messageId = 0;
	QString chat;
	QString sender;
	QString text;
	QString media; // "Photo · name.jpg", empty when there is none.
	TimeId date = 0; // Of the message, of the version for an edit.
	TimeId changed = 0; // When it was deleted or replaced.
};

struct Query {
	QString text;
	uint64 peerId = 0; // 0: every chat.
	TimeId from = 0; // 0: no limit.
	TimeId till = 0; // 0: no limit, otherwise exclusive.
	bool deleted = true;
	bool edited = true;

	friend inline bool operator==(const Query &, const Query &) = default;
};

struct Match {
	int index = 0; // In the entries.
	int score = 0;

	friend inline bool operator==(const Match &, const Match &) = default;
};

// Lower case, "ё" as "е", every kind of a space as a space. The result
// has exactly the length of the text, so the offsets are the same.
[[nodiscard]] QString Fold(const QString &text);

// The folded words of a query, a part in quotes is one "word" with its
// spaces. All of them must be found (in any of the fields).
[[nodiscard]] std::vector<QString> ParseWords(const QString &query);

// The entries with their folded fields, made once for many queries.
struct Corpus {
	std::vector<Entry> entries;
	std::vector<std::array<QString, 4>> folded; // text, media, sender, chat
};
[[nodiscard]] std::shared_ptr<const Corpus> MakeCorpus(
	std::vector<Entry> entries);

// The best first: a match in the text is worth more than one in a file
// name or in a name, the start of a word more than its middle, the whole
// phrase more than its words apart; equal ones go from the newest.
// An empty query gives everything that passes the filters, the newest
// first. A set `cancel` stops the search, the result is then empty.
[[nodiscard]] std::vector<Match> Search(
	const Corpus &corpus,
	const Query &query,
	const std::atomic<bool> *cancel = nullptr);

struct Range {
	int offset = 0;
	int length = 0;

	friend inline bool operator==(const Range &, const Range &) = default;
};

// A piece of the text around the first match, in one line, with the
// places of all the matches in it.
struct Snippet {
	QString text;
	std::vector<Range> ranges;
};
[[nodiscard]] Snippet MakeSnippet(
	const QString &text,
	const std::vector<QString> &words,
	int limit);

// «Поиск и фильтры» of the «Удалённые» screen. With a peer the chat
// filter starts on that chat.
void Show(
	not_null<Window::SessionController*> controller,
	PeerData *peer);

// OBLIVION_SELFTEST=deleted_search, pure logic.
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::DeletedSearch
