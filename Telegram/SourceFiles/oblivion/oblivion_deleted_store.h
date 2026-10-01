/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_types.h"

#include <map>
#include <set>

class HistoryItem;
struct HistoryMessageEdition;

namespace Main {
class Session;
} // namespace Main

namespace Oblivion {

struct DeletedMedia {
	QString type;
	QString name;
	QString mime;
	QString file;
	QString thumb;
	int64 size = 0;
	int duration = 0;
};

struct DeletedRecord {
	uint64 peerId = 0;
	int64 messageId = 0;
	int64 topicRootId = 0;
	uint64 senderId = 0;
	QString senderName;
	QString chatName;
	TimeId date = 0;
	TimeId deleted = 0;
	TextWithEntities text;
	DeletedMedia media;
	bool out = false;
};

struct EditVersion {
	TimeId since = 0;
	TimeId replaced = 0;
	TextWithEntities text;
};

struct DeletedChange {
	uint64 peerId = 0; // Zero for all chats.
	bool removed = false; // Records were removed, not only appended.
};

class DeletedStore final {
public:
	explicit DeletedStore(QString folder);

	[[nodiscard]] QString folder() const;
	[[nodiscard]] QString absolutePath(const QString &relative) const;

	void addDeleted(DeletedRecord &&record);
	[[nodiscard]] bool hasDeleted(uint64 peerId);
	[[nodiscard]] bool hasDeletedMessage(uint64 peerId, int64 messageId);
	[[nodiscard]] int deletedCount(uint64 peerId);
	[[nodiscard]] const std::vector<DeletedRecord> &deleted();
	void clearDeleted(uint64 peerId);
	[[nodiscard]] rpl::producer<DeletedChange> deletedChanges() const;

	void addEdit(uint64 peerId, int64 messageId, EditVersion &&version);
	[[nodiscard]] std::vector<EditVersion> edits(
		uint64 peerId,
		int64 messageId);

	// Drops everything saved for the account, both in memory and on disk.
	void forget();

private:
	using Key = std::pair<uint64, int64>;

	void ensureDeletedLoaded();
	void ensureEditsLoaded();
	void rewriteDeleted();
	void rewriteEdits();
	void trimDeleted();
	void trimEdits();
	void removeMediaFiles(const DeletedRecord &record);
	void appendLine(const QString &name, const QByteArray &line);

	const QString _folder;

	std::vector<DeletedRecord> _deleted;
	std::set<Key> _deletedKeys;
	base::flat_map<uint64, int> _deletedPerPeer;
	bool _deletedLoaded = false;

	std::map<Key, std::vector<EditVersion>> _edits;
	int _editsTotal = 0;
	int _editsLines = 0;
	bool _editsLoaded = false;

	rpl::event_stream<DeletedChange> _deletedChanges;

};

[[nodiscard]] DeletedStore &StoreFor(not_null<Main::Session*> session);

void RememberDeleted(not_null<HistoryItem*> item);

// Messages deleted on the server that are still shown here because of
// "keep deleted messages" (see MarkItemDeletedLocally in data_session.cpp).
// The server doesn't know them any more, so anything that would send them
// or refer to them there is not offered: forward, share, reply, edit, pin,
// reactions and report.
// MarkKeptDeleted() returns false if the message was marked already.
bool MarkKeptDeleted(not_null<HistoryItem*> item);
[[nodiscard]] bool IsKeptDeleted(not_null<const HistoryItem*> item);

// A list of messages to forward or share without such messages,
// as if they were destroyed on deletion like upstream does.
[[nodiscard]] HistoryItemsList WithoutKeptDeleted(HistoryItemsList items);

// Called on logout, like the upstream local storage wipe.
void ForgetDeleted(not_null<Main::Session*> session);

class EditHistory final {
public:
	[[nodiscard]] static EditHistory &Instance();

	void remember(
		not_null<HistoryItem*> item,
		const HistoryMessageEdition &edition);
	[[nodiscard]] std::vector<EditVersion> versions(
		not_null<HistoryItem*> item) const;

};

} // namespace Oblivion
