/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_send_online.h"

#include "api/api_common.h"
#include "api/api_text_entities.h"
#include "apiwrap.h"
#include "base/random.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "data/data_changes.h"
#include "data/data_chat_participant_status.h"
#include "data/data_histories.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/history_item_helpers.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mtproto/facade.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/mtproto_response.h"
#include "oblivion/oblivion_lang.h"
#include "oblivion/oblivion_online.h"
#include "oblivion/oblivion_sending.h"
#include "oblivion/oblivion_settings.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/boxes/confirm_box.h"
#include "ui/item_text_options.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/text/text_utilities.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/shadow.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_calls.h"
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QSaveFile>

namespace Oblivion::SendOnline {
namespace {

constexpr auto kVersion = 1;
constexpr auto kMaxPerPeer = 20;
constexpr auto kMaxTotal = 200;
constexpr auto kMaxTextLength = 4096;
constexpr auto kMaxAttempts = 5;
constexpr auto kMaxFileSize = qint64(8) * 1024 * 1024;

// A message found as "sending" after a restart goes out again by itself
// (with its random id) only this soon after the attempt.
constexpr auto kResumeWindow = TimeId(600);

// A message that came from the person this long ago at most means that
// they are here now.
constexpr auto kFreshMessage = TimeId(120);

// A message from the person is a sign of them for this long: the whole
// queue for them has the time to follow, one message after another.
constexpr auto kBurst = TimeId(90);

constexpr auto kTick = crl::time(20) * 1000;
constexpr auto kRetryStep = TimeId(60);
constexpr auto kFloodMargin = TimeId(5);
constexpr auto kMaxFloodWait = TimeId(86400);
constexpr auto kPreviewLength = 140;
constexpr auto kListTextLength = 600;

const auto kErrorUnavailable = u"OBLIVION_UNAVAILABLE"_q;
const auto kErrorPaid = u"OBLIVION_PAID"_q;
const auto kErrorEmpty = u"MESSAGE_EMPTY"_q;
const auto kErrorSave = u"OBLIVION_NOT_SAVED"_q;
const auto kErrorChatDeleted = u"OBLIVION_CHAT_DELETED"_q;
const auto kErrorBlocked = u"OBLIVION_BLOCKED"_q;

enum class State : uchar {
	Waiting,
	Sending,
	Expired, // The time limit has passed, the user decides.
	Failed, // The server refused or the chat is not available.
	Unconfirmed, // Was being sent when the app stopped, long ago.
};
constexpr auto kStateCount = 5;

struct Item {
	uint64 id = 0; // Never reused in a queue.
	uint64 peerId = 0;
	uint64 randomId = 0; // The same for every attempt.
	TextWithTags text;
	QString peerName; // For the list while the peer is not loaded.
	TimeId created = 0;
	TimeId deadline = 0;
	TimeId attempted = 0; // When the last attempt started.
	TimeId retryAt = 0; // Not before, after a temporary error.
	int attempts = 0;
	State state = State::Waiting;
	QString error;
	bool forced = false; // Send without waiting for the person.
	bool asked = false; // The user was told about the problem.

	// Forced not by a click, but by an attempt that did not tell whether
	// the message got through (see unsure): it is repeated by itself,
	// without waiting for the person once more, but only within
	// kResumeWindow of that attempt. Later the user decides.
	bool resumed = false;

	// An attempt ended in a way that does not tell whether the message
	// got through (no answer, a server error, the app stopped). The next
	// attempt repeats the same random id with the same text, so the text
	// is not changed any more.
	bool unsure = false;

	// The chat was deleted or the person blocked while the message was
	// being sent: whatever the answer is, it is not tried again.
	QString stop;
};

// The queue of one account: only the rules, no session and no files.
class Queue final {
public:
	// A message that was being sent when the app stopped is either put
	// back (soon after the attempt) or marked as not confirmed.
	[[nodiscard]] static Queue Parse(const QByteArray &bytes, TimeId now);
	[[nodiscard]] QByteArray serialize() const;

	[[nodiscard]] const std::vector<Item> &items() const {
		return _items;
	}
	[[nodiscard]] bool empty() const {
		return _items.empty();
	}
	[[nodiscard]] const Item *find(uint64 id) const;
	[[nodiscard]] int count(uint64 peerId) const;

	// The id of the new message, zero if it is not taken: no text, too
	// long, too many of them, the random id is in the queue already.
	uint64 add(
		uint64 peerId,
		const QString &peerName,
		TextWithTags text,
		uint64 randomId,
		TimeId now,
		int hours);
	[[nodiscard]] bool canEdit(uint64 id) const;
	bool edit(uint64 id, TextWithTags text);
	bool remove(uint64 id); // Not while it is being sent.

	// «Отправить сейчас»: goes out without waiting for the person.
	bool resend(uint64 id);

	// «Ждать ещё»: an expired one waits again.
	bool extend(uint64 id, TimeId now, int hours);

	// The waiting ones whose time is over become expired, the ones a
	// restart was going to send again too late become not confirmed.
	std::vector<uint64> expire(TimeId now);

	// The next message for the person, marked as being sent. Nothing
	// while another one is on its way, while the first in line waits
	// after an error and while the person is not online (a forced one
	// does not wait for that). A held message (it is being edited or
	// its removal is being confirmed) does not go, and to keep the order
	// neither do the ones that wait behind it.
	[[nodiscard]] std::optional<Item> begin(
		uint64 peerId,
		TimeId now,
		bool online,
		const base::flat_set<uint64> &held = {});
	void sent(uint64 id);
	void failed(
		uint64 id,
		const QString &error,
		bool permanent,
		TimeId retryAt,
		bool unsure = false);

	// The chat was deleted or the person blocked: nothing for them is
	// sent by itself any more, the waiting ones stay in the list as not
	// sent, with this reason. False if there was nothing to stop.
	bool cancel(uint64 peerId, const QString &error);

	// The persons something waits for, forced or not.
	[[nodiscard]] base::flat_set<uint64> waitedPeers() const;
	[[nodiscard]] std::vector<uint64> readyPeers(TimeId now) const;
	[[nodiscard]] int unasked() const;
	void markAsked();
	void clear();

private:
	[[nodiscard]] Item *lookup(uint64 id);

	std::vector<Item> _items;
	uint64 _lastId = 0;

};

[[nodiscard]] bool ValidText(const TextWithTags &text) {
	return !text.text.trimmed().isEmpty()
		&& (text.text.size() <= kMaxTextLength);
}

[[nodiscard]] QJsonObject SerializeItem(const Item &item) {
	auto tags = QJsonArray();
	for (const auto &tag : item.text.tags) {
		tags.push_back(QJsonObject{
			{ u"o"_q, tag.offset },
			{ u"l"_q, tag.length },
			{ u"id"_q, tag.id },
		});
	}
	auto result = QJsonObject{
		{ u"id"_q, QString::number(item.id) },
		{ u"peer"_q, QString::number(item.peerId) },
		{ u"random"_q, QString::number(item.randomId) },
		{ u"text"_q, item.text.text },
		{ u"name"_q, item.peerName },
		{ u"created"_q, double(item.created) },
		{ u"deadline"_q, double(item.deadline) },
		{ u"attempted"_q, double(item.attempted) },
		{ u"retry"_q, double(item.retryAt) },
		{ u"attempts"_q, item.attempts },
		{ u"state"_q, int(item.state) },
		{ u"error"_q, item.error },
		{ u"forced"_q, item.forced },
		{ u"asked"_q, item.asked },
	};
	if (!tags.isEmpty()) {
		result.insert(u"tags"_q, tags);
	}
	if (item.resumed) {
		result.insert(u"resumed"_q, true);
	}
	if (item.unsure) {
		result.insert(u"unsure"_q, true);
	}
	if (!item.stop.isEmpty()) {
		result.insert(u"stop"_q, item.stop);
	}
	return result;
}

[[nodiscard]] std::optional<Item> ParseItem(const QJsonObject &data) {
	const auto state = data.value(u"state"_q).toInt(-1);
	auto result = Item{
		.id = data.value(u"id"_q).toString().toULongLong(),
		.peerId = data.value(u"peer"_q).toString().toULongLong(),
		.randomId = data.value(u"random"_q).toString().toULongLong(),
		.peerName = data.value(u"name"_q).toString(),
		.created = TimeId(data.value(u"created"_q).toDouble()),
		.deadline = TimeId(data.value(u"deadline"_q).toDouble()),
		.attempted = TimeId(data.value(u"attempted"_q).toDouble()),
		.retryAt = TimeId(data.value(u"retry"_q).toDouble()),
		.attempts = std::clamp(data.value(u"attempts"_q).toInt(), 0, 1000),
		.error = data.value(u"error"_q).toString(),
		.forced = data.value(u"forced"_q).toBool(),
		.asked = data.value(u"asked"_q).toBool(),
		.resumed = data.value(u"resumed"_q).toBool(),
		.unsure = data.value(u"unsure"_q).toBool(),
		.stop = data.value(u"stop"_q).toString(),
	};
	result.text.text = data.value(u"text"_q).toString();
	const auto size = int(result.text.text.size());
	const auto tags = data.value(u"tags"_q).toArray();
	for (const auto &value : tags) {
		const auto tag = value.toObject();
		const auto offset = tag.value(u"o"_q).toInt(-1);
		const auto length = tag.value(u"l"_q).toInt(-1);
		if (offset < 0 || length <= 0 || offset > size - length) {
			continue;
		}
		result.text.tags.push_back({
			.offset = offset,
			.length = length,
			.id = tag.value(u"id"_q).toString(),
		});
	}
	if (!result.id
		|| !result.peerId
		|| !result.randomId
		|| state < 0
		|| state >= kStateCount
		|| !ValidText(result.text)) {
		return std::nullopt;
	}
	result.state = State(state);
	return result;
}

Queue Queue::Parse(const QByteArray &bytes, TimeId now) {
	auto result = Queue();
	const auto document = QJsonDocument::fromJson(bytes);
	if (!document.isObject()) {
		return result;
	}
	const auto object = document.object();
	if (object.value(u"version"_q).toInt() != kVersion) {
		return result;
	}
	result._lastId = object.value(u"last"_q).toString().toULongLong();
	auto ids = base::flat_set<uint64>();
	auto randoms = base::flat_set<uint64>();
	const auto list = object.value(u"items"_q).toArray();
	for (const auto &value : list) {
		auto parsed = ParseItem(value.toObject());
		if (!parsed
			|| !ids.emplace(parsed->id).second
			|| !randoms.emplace(parsed->randomId).second) {
			// A broken record or a copy of one: a second message with
			// the same ids must never appear.
			continue;
		}
		auto &item = *parsed;
		if (item.state == State::Sending) {
			// Nobody knows whether it got through. The chat that was
			// deleted meanwhile gets nothing by itself any more.
			item.unsure = true;
			if (item.stop.isEmpty()
				&& now >= item.attempted
				&& now - item.attempted <= kResumeWindow) {
				item.state = State::Waiting;
				item.forced = true;
				item.resumed = true;
			} else {
				item.state = State::Unconfirmed;
				item.asked = false;
				item.stop = QString();
			}
		} else {
			item.stop = QString();
			if (item.state != State::Waiting || !item.forced) {
				item.resumed = false;
			}
		}
		result._lastId = std::max(result._lastId, item.id);
		result._items.push_back(std::move(item));
		if (int(result._items.size()) >= kMaxTotal) {
			break;
		}
	}
	return result;
}

QByteArray Queue::serialize() const {
	auto list = QJsonArray();
	for (const auto &item : _items) {
		list.push_back(SerializeItem(item));
	}
	return QJsonDocument(QJsonObject{
		{ u"version"_q, kVersion },
		{ u"last"_q, QString::number(_lastId) },
		{ u"items"_q, list },
	}).toJson(QJsonDocument::Compact);
}

const Item *Queue::find(uint64 id) const {
	const auto i = ranges::find(_items, id, &Item::id);
	return (i != end(_items)) ? &*i : nullptr;
}

Item *Queue::lookup(uint64 id) {
	const auto i = ranges::find(_items, id, &Item::id);
	return (i != end(_items)) ? &*i : nullptr;
}

int Queue::count(uint64 peerId) const {
	return int(ranges::count(_items, peerId, &Item::peerId));
}

uint64 Queue::add(
		uint64 peerId,
		const QString &peerName,
		TextWithTags text,
		uint64 randomId,
		TimeId now,
		int hours) {
	if (!peerId
		|| !randomId
		|| !ValidText(text)
		|| int(_items.size()) >= kMaxTotal
		|| count(peerId) >= kMaxPerPeer
		|| ranges::contains(_items, randomId, &Item::randomId)) {
		return 0;
	}
	const auto id = ++_lastId;
	_items.push_back({
		.id = id,
		.peerId = peerId,
		.randomId = randomId,
		.text = std::move(text),
		.peerName = peerName,
		.created = now,
		.deadline = now + std::clamp(hours, 1, 168) * 3600,
	});
	return id;
}

bool Queue::canEdit(uint64 id) const {
	const auto item = find(id);
	return item
		&& (item->state == State::Waiting
			|| item->state == State::Expired
			|| item->state == State::Failed)
		&& !item->unsure
		&& !(item->forced && item->attempts > 0);
}

bool Queue::edit(uint64 id, TextWithTags text) {
	if (!canEdit(id) || !ValidText(text)) {
		return false;
	}
	lookup(id)->text = std::move(text);
	return true;
}

bool Queue::remove(uint64 id) {
	const auto i = ranges::find(_items, id, &Item::id);
	if (i == end(_items) || i->state == State::Sending) {
		return false;
	}
	_items.erase(i);
	return true;
}

bool Queue::resend(uint64 id) {
	const auto item = lookup(id);
	if (!item || item->state == State::Sending) {
		return false;
	}
	item->state = State::Waiting;
	item->forced = true;
	item->resumed = false;
	item->retryAt = 0;
	item->error = QString();
	item->asked = false;
	return true;
}

bool Queue::extend(uint64 id, TimeId now, int hours) {
	const auto item = lookup(id);
	if (!item || item->state != State::Expired) {
		return false;
	}
	item->state = State::Waiting;
	item->deadline = now + std::clamp(hours, 1, 168) * 3600;
	item->forced = false;
	item->asked = false;
	return true;
}

// After an attempt that did not tell whether the message got through (the
// app stopped, the server answered with its own error or not at all) the
// message goes again by itself only soon after that attempt, while the
// server surely remembers its random id and refuses a second copy. Later
// nobody can tell whether a repeat would be a second message, so the
// user decides. A clock that went back is not trusted.
[[nodiscard]] bool ResumeIsOver(const Item &item, TimeId now) {
	return item.resumed
		&& (now < item.attempted || now - item.attempted > kResumeWindow);
}

void MarkUnconfirmed(Item &item) {
	item.state = State::Unconfirmed;
	item.forced = false;
	item.resumed = false;
	item.retryAt = 0;
	item.asked = false;
}

std::vector<uint64> Queue::expire(TimeId now) {
	auto result = std::vector<uint64>();
	for (auto &item : _items) {
		if (item.state != State::Waiting) {
			continue;
		} else if (item.forced) {
			if (ResumeIsOver(item, now)) {
				MarkUnconfirmed(item);
				result.push_back(item.id);
			}
		} else if (item.deadline <= now) {
			item.state = State::Expired;
			item.asked = false;
			result.push_back(item.id);
		}
	}
	return result;
}

std::optional<Item> Queue::begin(
		uint64 peerId,
		TimeId now,
		bool online,
		const base::flat_set<uint64> &held) {
	auto chosen = (Item*)nullptr;
	for (auto &item : _items) {
		if (item.peerId != peerId) {
			continue;
		} else if (item.state == State::Sending) {
			return std::nullopt;
		} else if (item.state != State::Waiting || !item.forced) {
			continue;
		} else if (ResumeIsOver(item, now)) {
			MarkUnconfirmed(item);
		} else if (!chosen
			&& !held.contains(item.id)
			&& item.retryAt <= now) {
			chosen = &item;
		}
	}
	if (!chosen && online) {
		for (auto &item : _items) {
			if (item.peerId != peerId || item.state != State::Waiting) {
				continue;
			} else if (item.forced || held.contains(item.id)) {
				// A held one, or one that is repeated a bit later. The
				// order is kept: the ones after it wait as well.
				return std::nullopt;
			} else if (item.deadline <= now) {
				item.state = State::Expired;
				item.asked = false;
				continue;
			} else if (item.retryAt > now) {
				// The order is kept here too.
				return std::nullopt;
			}
			chosen = &item;
			break;
		}
	}
	if (!chosen) {
		return std::nullopt;
	}
	chosen->state = State::Sending;
	chosen->attempted = now;
	chosen->forced = false;
	chosen->resumed = false;
	++chosen->attempts;
	return *chosen;
}

void Queue::sent(uint64 id) {
	const auto i = ranges::find(_items, id, &Item::id);
	if (i != end(_items)) {
		_items.erase(i);
	}
}

void Queue::failed(
		uint64 id,
		const QString &error,
		bool permanent,
		TimeId retryAt,
		bool unsure) {
	const auto item = lookup(id);
	if (!item || item->state != State::Sending) {
		return;
	}
	item->unsure = item->unsure || unsure;
	const auto stopped = !item->stop.isEmpty();
	item->error = stopped ? base::take(item->stop) : error;
	if (stopped || permanent) {
		item->state = State::Failed;
		item->retryAt = 0;
		item->asked = false;
	} else if (item->attempts >= kMaxAttempts) {
		// Given up. After an attempt with no clear answer the message
		// may be there already, the user is told exactly that.
		item->state = item->unsure ? State::Unconfirmed : State::Failed;
		item->retryAt = 0;
		item->asked = false;
	} else {
		item->state = State::Waiting;
		item->retryAt = retryAt;
		if (item->unsure) {
			// The repeat does not wait for the person to come online once
			// more: hours later the server may not know the random id any
			// more and a message that did get through would come twice.
			// That holds for every later attempt of this message too.
			item->forced = true;
			item->resumed = true;
		}
	}
}

bool Queue::cancel(uint64 peerId, const QString &error) {
	auto result = false;
	for (auto &item : _items) {
		if (item.peerId != peerId) {
			continue;
		} else if (item.state == State::Waiting) {
			item.state = State::Failed;
			item.error = error;
			item.forced = false;
			item.resumed = false;
			item.retryAt = 0;
			item.asked = false;
			result = true;
		} else if (item.state == State::Sending && item.stop != error) {
			// The request has left already, its answer is waited for.
			item.stop = error;
			result = true;
		}
	}
	return result;
}

base::flat_set<uint64> Queue::waitedPeers() const {
	auto result = base::flat_set<uint64>();
	for (const auto &item : _items) {
		if (item.state == State::Waiting) {
			result.emplace(item.peerId);
		}
	}
	return result;
}

std::vector<uint64> Queue::readyPeers(TimeId now) const {
	auto result = std::vector<uint64>();
	auto decided = base::flat_set<uint64>();
	const auto add = [&](uint64 peerId) {
		if (!ranges::contains(result, peerId)) {
			result.push_back(peerId);
		}
	};
	for (const auto &item : _items) {
		if (item.state != State::Waiting) {
			continue;
		} else if (item.forced) {
			add(item.peerId);
		} else if (decided.emplace(item.peerId).second
			&& item.retryAt <= now) {
			// Only the first one in line decides, see begin().
			add(item.peerId);
		}
	}
	return result;
}

int Queue::unasked() const {
	auto result = 0;
	for (const auto &item : _items) {
		if (!item.asked
			&& (item.state == State::Expired
				|| item.state == State::Failed
				|| item.state == State::Unconfirmed)) {
			++result;
		}
	}
	return result;
}

void Queue::markAsked() {
	for (auto &item : _items) {
		if (item.state == State::Expired
			|| item.state == State::Failed
			|| item.state == State::Unconfirmed) {
			item.asked = true;
		}
	}
}

void Queue::clear() {
	_items.clear();
}

// The seconds to wait from FLOOD_WAIT_N / FLOOD_PREMIUM_WAIT_N.
[[nodiscard]] TimeId FloodSeconds(const QString &type) {
	for (const auto &prefix : { u"FLOOD_WAIT_"_q, u"FLOOD_PREMIUM_WAIT_"_q }) {
		if (type.startsWith(prefix)) {
			const auto seconds = type.mid(prefix.size()).toLongLong();
			return TimeId(std::clamp(
				seconds,
				qlonglong(1),
				qlonglong(kMaxFloodWait)));
		}
	}
	return 0;
}

// What an answer of the server means for the message.
struct Verdict {
	bool sent = false;
	bool permanent = false;
	TimeId retryAt = 0;
	bool unsure = false; // The message may have got through.
};

[[nodiscard]] Verdict Judge(
		const QString &type,
		int code,
		int attempts,
		TimeId now) {
	if (type == u"RANDOM_ID_DUPLICATE"_q) {
		// The server has a message with this random id already: an
		// earlier attempt got through, only its answer was lost.
		return { .sent = true };
	} else if (const auto flood = FloodSeconds(type)) {
		return { .retryAt = now + flood + kFloodMargin };
	} else if (code <= 0 || code >= 500) {
		// No answer of the server about the message itself: a server
		// error, a lost connection, an answer that could not be read (the
		// errors made on this device have the code 0). It is repeated
		// with the same random id, the server refuses a second copy.
		return {
			.retryAt = now + kRetryStep * std::max(attempts, 1),
			.unsure = true,
		};
	}
	return { .permanent = true };
}

[[nodiscard]] bool Eligible(not_null<PeerData*> peer) {
	const auto user = peer->asUser();
	return user
		&& !user->isSelf()
		&& !user->isBot()
		&& !user->isServiceUser()
		&& !user->isNotificationsUser()
		&& !user->isInaccessible();
}

// Why nothing can be sent to the chat right now, empty if it can. A
// person who is not loaded yet is not a question for this function: that
// is a reason to wait, not to give up (see Account::flush).
[[nodiscard]] QString Unavailable(not_null<UserData*> user) {
	if (!Eligible(user)) {
		return kErrorUnavailable;
	} else if (user->isBlocked()) {
		return kErrorBlocked;
	} else if (user->starsPerMessageChecked() > 0) {
		return kErrorPaid;
	} else if (!Data::CanSendTexts(user)) {
		return kErrorUnavailable;
	}
	return QString();
}

[[nodiscard]] QString ReasonText(const Item &item) {
	static const auto kGone = std::array{
		u"USER_IS_BLOCKED"_q,
		u"YOU_BLOCKED_USER"_q,
		u"INPUT_USER_DEACTIVATED"_q,
		u"USER_DEACTIVATED"_q,
		u"PEER_ID_INVALID"_q,
		u"CHAT_WRITE_FORBIDDEN"_q,
		u"PRIVACY_PREMIUM_REQUIRED"_q,
	};
	switch (item.state) {
	case State::Expired:
		return tr::lng_oblivion_sendonline_reason_expired(tr::now);
	case State::Unconfirmed:
		return tr::lng_oblivion_sendonline_reason_unconfirmed(tr::now);
	case State::Failed:
		if (item.error == kErrorPaid
			|| item.error.startsWith(u"ALLOW_PAYMENT_REQUIRED"_q)) {
			return tr::lng_oblivion_sendonline_reason_paid(tr::now);
		} else if (item.error == kErrorChatDeleted) {
			return tr::lng_oblivion_sendonline_reason_chat_deleted(tr::now);
		} else if (item.error == kErrorBlocked
			|| item.error == u"YOU_BLOCKED_USER"_q) {
			return tr::lng_oblivion_sendonline_reason_blocked(tr::now);
		} else if (item.error == kErrorUnavailable
			|| ranges::contains(kGone, item.error)) {
			return tr::lng_oblivion_sendonline_reason_unavailable(tr::now);
		}
		return tr::lng_oblivion_sendonline_reason_server(
			tr::now,
			lt_error,
			item.error);
	case State::Waiting:
	case State::Sending:
		break;
	}
	return QString();
}

[[nodiscard]] bool Problem(State state) {
	return (state == State::Expired)
		|| (state == State::Failed)
		|| (state == State::Unconfirmed);
}

// For the user it is being sent: the request is out, or it goes as soon
// as it can without waiting for the person (after «Отправить сейчас»,
// between the repeats of an attempt that got no clear answer).
[[nodiscard]] bool OnItsWay(const Item &item) {
	return (item.state == State::Sending)
		|| (item.state == State::Waiting && item.forced);
}

[[nodiscard]] QString Preview(const QString &text, int limit) {
	auto result = text.simplified();
	if (result.size() > limit) {
		auto length = limit - 1;
		if (result[length - 1].isHighSurrogate()) {
			--length;
		}
		result = result.left(length).trimmed() + QChar(0x2026);
	}
	return result;
}

[[nodiscard]] QString MomentText(TimeId when, TimeId now) {
	const auto moment = QDateTime::fromSecsSinceEpoch(when);
	const auto time = moment.time().toString(u"HH:mm"_q);
	if (moment.date() == QDateTime::fromSecsSinceEpoch(now).date()) {
		return time;
	}
	const auto locale = QLocale(CurrentLanguageIsRussian()
		? QLocale::Russian
		: QLocale::English);
	return locale.toString(moment.date(), u"d MMM"_q).remove(QChar('.'))
		+ u", "_q
		+ time;
}

struct BarContent {
	bool shown = false;
	bool problem = false;
	QString title;
	QString status;
	QString state; // After the status, never elided away by it.
	QString button;

	friend inline bool operator==(
		const BarContent &,
		const BarContent &) = default;
};

[[nodiscard]] BarContent ContentFor(
		const std::vector<Item> &items,
		uint64 peerId) {
	auto waiting = 0;
	auto problems = 0;
	auto firstWaiting = (const Item*)nullptr;
	auto firstProblem = (const Item*)nullptr;
	auto sending = (const Item*)nullptr;
	for (const auto &item : items) {
		if (item.peerId != peerId) {
			continue;
		} else if (OnItsWay(item)) {
			sending = sending ? sending : &item;
		} else if (Problem(item.state)) {
			++problems;
			firstProblem = firstProblem ? firstProblem : &item;
		} else {
			++waiting;
			firstWaiting = firstWaiting ? firstWaiting : &item;
		}
	}
	auto result = BarContent();
	if (!sending && !waiting && !problems) {
		return result;
	}
	result.shown = true;
	result.button = tr::lng_oblivion_sendonline_bar_open(tr::now);
	if (problems) {
		result.problem = true;
		result.title = tr::lng_oblivion_sendonline_bar_problem(
			tr::now,
			lt_count,
			problems);
		result.status = ReasonText(*firstProblem);
	} else if (sending) {
		result.title = tr::lng_oblivion_sendonline_bar_sending(tr::now);
		result.status = Preview(sending->text.text, kPreviewLength);
	} else {
		result.title = tr::lng_oblivion_sendonline_bar_waiting(
			tr::now,
			lt_count,
			waiting);
		result.status = Preview(firstWaiting->text.text, kPreviewLength);
		result.state = tr::lng_oblivion_sendonline_bar_state(tr::now);
	}
	return result;
}

// A clock with a small "online" dot: waits for someone to come online.
void PaintClock(QPainter &p, QRectF rect, const QColor &color, bool dot) {
	auto hq = PainterHighQualityEnabler(p);
	const auto size = rect.width();
	auto pen = QPen(color, size * 0.085);
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	const auto skip = size * 0.12;
	const auto circle = rect.marginsRemoved({ skip, skip, skip, skip });
	p.drawEllipse(circle);
	const auto center = circle.center();
	p.drawLine(center, center + QPointF(0., -size * 0.22));
	p.drawLine(center, center + QPointF(size * 0.15, size * 0.09));
	if (dot) {
		const auto radius = size * 0.15;
		const auto at = QPointF(
			circle.right() - radius * 0.6,
			circle.bottom() - radius * 0.6);
		p.setPen(Qt::NoPen);
		p.setBrush(st::historyComposeAreaBg);
		p.drawEllipse(at, radius * 1.45, radius * 1.45);
		p.setBrush(st::dialogsOnlineBadgeFg);
		p.drawEllipse(at, radius, radius);
	}
}

// The bar of a chat: shows what it is given, a click anywhere opens
// the list.
class Bar final : public Ui::RpWidget {
public:
	explicit Bar(QWidget *parent);

	void setContent(BarContent content);
	void setTopLine(bool shown);

	[[nodiscard]] rpl::producer<> clicks() const {
		return _clicks.events();
	}

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;

private:
	void updateControlsGeometry(int outer);

	BarContent _content;
	rpl::variable<QString> _buttonText;
	const not_null<Ui::RoundButton*> _button;
	rpl::event_stream<> _clicks;
	int _textRight = 0;
	bool _topLine = true;
	bool _pressed = false;

};

Bar::Bar(QWidget *parent)
: RpWidget(parent)
, _button(Ui::CreateChild<Ui::RoundButton>(
	this,
	_buttonText.value(),
	st::groupCallTopBarJoin)) {
	resize(width(), st::historyReplyHeight);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setCursor(style::cur_pointer);

	_button->setFullRadius(true);
	_button->hide();
	_button->widthValue(
	) | rpl::on_next([=] {
		updateControlsGeometry(width());
	}, _button->lifetime());
	_button->setClickedCallback([=] {
		_clicks.fire({});
	});
}

void Bar::setContent(BarContent content) {
	if (_content == content) {
		return;
	}
	_content = std::move(content);
	_buttonText = _content.button;
	_button->setVisible(!_content.button.isEmpty());
	updateControlsGeometry(width());
	update();
}

void Bar::setTopLine(bool shown) {
	if (_topLine != shown) {
		_topLine = shown;
		update();
	}
}

int Bar::resizeGetHeight(int newWidth) {
	updateControlsGeometry(newWidth);
	return st::historyReplyHeight;
}

void Bar::updateControlsGeometry(int outer) {
	const auto line = st::lineWidth;
	const auto inner = st::historyReplyHeight - 2 * line;
	const auto top = line + (inner - _button->height()) / 2;
	auto right = outer - top;
	if (!_content.button.isEmpty()) {
		_button->moveToRight(top, top, outer);
		right -= _button->width() + style::ConvertScale(14);
	}
	if (_textRight != right) {
		_textRight = right;
		update();
	}
}

void Bar::mousePressEvent(QMouseEvent *e) {
	_pressed = (e->button() == Qt::LeftButton);
}

void Bar::mouseReleaseEvent(QMouseEvent *e) {
	if (base::take(_pressed)
		&& e->button() == Qt::LeftButton
		&& rect().contains(e->pos())) {
		_clicks.fire({});
	}
}

void Bar::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto line = st::lineWidth;
	const auto inner = height() - 2 * line;
	p.fillRect(e->rect(), st::historyComposeAreaBg);
	if (_topLine) {
		p.fillRect(0, 0, width(), line, st::shadowFg);
	}
	p.fillRect(0, height() - line, width(), line, st::shadowFg);

	const auto icon = style::ConvertScale(22);
	const auto iconLeft = st::topBarArrowPadding.right();
	const auto &titleColor = _content.problem
		? st::attentionButtonFg
		: st::defaultMessageBar.titleFg;
	PaintClock(
		p,
		QRectF(iconLeft, line + (inner - icon) / 2., icon, icon),
		titleColor->c,
		!_content.problem);

	const auto left = iconLeft + icon + style::ConvertScale(9);
	const auto available = _textRight - left;
	if (available <= 0) {
		return;
	}
	const auto titleTop = st::msgReplyPadding.top();
	const auto statusTop = titleTop + st::msgServiceNameFont->height;
	const auto elided = [&](const style::font &font, const QString &text) {
		return (font->width(text) > available)
			? font->elided(text, available)
			: text;
	};
	const auto &titleFont = st::defaultMessageBar.title.font;
	p.setFont(titleFont);
	p.setPen(titleColor);
	p.drawTextLeft(left, titleTop, width(), elided(titleFont, _content.title));

	const auto &statusFont = st::defaultMessageBar.text.font;
	auto status = _content.status;
	auto state = QString();
	auto stateLeft = left;
	if (_content.state.isEmpty()) {
		status = elided(statusFont, status);
	} else {
		state = u" · "_q + _content.state;
		const auto stateWidth = statusFont->width(state);
		const auto statusWidth = statusFont->width(status);

		// In a narrow chat the message itself is worth more than the
		// words after it: they stay only while a readable part of the
		// message fits before them.
		const auto minimal = style::ConvertScale(140);
		if (statusWidth + stateWidth <= available) {
			stateLeft += statusWidth;
		} else if (available - stateWidth >= minimal) {
			status = statusFont->elided(status, available - stateWidth);
			stateLeft += statusFont->width(status);
		} else {
			state = QString();
			status = elided(statusFont, status);
		}
	}
	p.setFont(statusFont);
	if (!status.isEmpty()) {
		p.setPen(st::defaultMessageBar.textFg);
		p.drawTextLeft(left, statusTop, width(), status);
	}
	if (!state.isEmpty()) {
		p.setPen(st::historyStatusFg);
		p.drawTextLeft(stateLeft, statusTop, width(), state);
	}
}

// Two bars of a chat in one widget, see WithChatBar().
class Stack final : public Ui::RpWidget {
public:
	explicit Stack(QWidget *parent);

	void setBars(
		object_ptr<Ui::RpWidget> above,
		object_ptr<Ui::RpWidget> below,
		Fn<void(bool aboveShown)> aboveChanged);

protected:
	int resizeGetHeight(int newWidth) override;

private:
	object_ptr<Ui::RpWidget> _above = { nullptr };
	object_ptr<Ui::RpWidget> _below = { nullptr };
	bool _resizing = false;

};

Stack::Stack(QWidget *parent)
: RpWidget(parent) {
}

void Stack::setBars(
		object_ptr<Ui::RpWidget> above,
		object_ptr<Ui::RpWidget> below,
		Fn<void(bool aboveShown)> aboveChanged) {
	_above = std::move(above);
	_below = std::move(below);
	const auto relayout = [=] {
		if (!_resizing) {
			resizeToWidth(width());
		}
	};
	if (const auto raw = _above.data()) {
		raw->heightValue(
		) | rpl::on_next([=](int height) {
			if (aboveChanged) {
				aboveChanged(height > 0);
			}
			relayout();
		}, raw->lifetime());
	}
	if (const auto raw = _below.data()) {
		raw->heightValue(
		) | rpl::on_next(relayout, raw->lifetime());
	}
	relayout();
}

int Stack::resizeGetHeight(int newWidth) {
	_resizing = true;
	auto top = 0;
	for (const auto raw : { _above.data(), _below.data() }) {
		if (raw) {
			if (newWidth > 0) {
				raw->resizeToWidth(newWidth);
			}
			raw->moveToLeft(0, top, newWidth);
			top += raw->height();
		}
	}
	_resizing = false;
	return top;
}

struct ListRow {
	uint64 id = 0;
	QString chat;
	QString text;
	QString status;
	bool problem = false;
	bool sending = false;
	bool canEdit = false;
	bool canSend = false;
	bool canWait = false;
	bool canRemove = false;
};

struct ListArgs {
	rpl::producer<std::vector<ListRow>> rows;
	bool showChat = true;
	Fn<void(uint64)> edit;
	Fn<void(uint64)> sendNow;
	Fn<void(uint64)> waitMore;
	Fn<void(uint64)> remove;
};

struct EditArgs {
	TextWithTags text;
	Fn<bool(TextWithTags)> save;
};

struct RowAction {
	QString text;
	Fn<void()> callback;
	bool attention = false; // Takes the message away: the link is red.
};

[[nodiscard]] const style::LinkButton &AttentionLinkStyle() {
	static const auto result = [] {
		auto st = st::defaultLinkButton;
		st.color = st::attentionButtonFg;
		st.overColor = st::attentionButtonFgOver;
		return st;
	}();
	return result;
}

// The links of a row, one after another. The ones that don't fit in the
// width (another language, a large interface scale) go to the next line,
// none of them is cut by the edge of the box.
class ActionLinks final : public Ui::RpWidget {
public:
	using RpWidget::RpWidget;

	void addLink(not_null<Ui::LinkButton*> link) {
		_links.push_back(link);
	}

protected:
	int resizeGetHeight(int newWidth) override;

private:
	std::vector<not_null<Ui::LinkButton*>> _links;

};

int ActionLinks::resizeGetHeight(int newWidth) {
	const auto skip = st::boxLittleSkip * 2;
	const auto lineSkip = st::boxLittleSkip / 2;
	auto left = 0;
	auto top = 0;
	auto bottom = 0;
	for (const auto &link : _links) {
		if (left > 0 && newWidth > 0 && left + link->width() > newWidth) {
			left = 0;
			top = bottom + lineSkip;
		}
		link->moveToLeft(left, top, newWidth);
		left += link->width() + skip;
		bottom = std::max(bottom, top + link->height());
	}
	return bottom;
}

void AddActions(
		not_null<Ui::VerticalLayout*> container,
		not_null<QWidget*> guard,
		std::vector<RowAction> actions) {
	if (actions.empty()) {
		return;
	}
	auto links = object_ptr<ActionLinks>(container);
	const auto raw = links.data();
	const auto alive = guard.get();
	for (auto &action : actions) {
		const auto link = Ui::CreateChild<Ui::LinkButton>(
			raw,
			action.text,
			action.attention ? AttentionLinkStyle() : st::defaultLinkButton);

		// An action may rebuild the list at once and destroy this very
		// link, so it never runs inside of the click. It is kept by the
		// list, not by the link: a click made right before the list was
		// rebuilt by something else is not lost, the action looks its
		// message up again by the id.
		link->setClickedCallback([=, callback = std::move(action.callback)] {
			crl::on_main(alive, callback);
		});
		raw->addLink(link);
	}
	raw->resizeToWidth(raw->width());
	const auto &padding = st::boxRowPadding;
	container->add(
		std::move(links),
		style::margins(padding.left(), st::boxLittleSkip / 2, padding.right(), 0),
		style::al_left);
}

void AddListRow(
		not_null<Ui::VerticalLayout*> list,
		const ListRow &data,
		const ListArgs &args,
		bool showChat,
		bool last) {
	const auto row = list->add(object_ptr<Ui::VerticalLayout>(list));
	const auto &padding = st::boxRowPadding;
	const auto skip = st::boxLittleSkip;
	Ui::AddSkip(row, skip);
	if (showChat) {
		row->add(
			object_ptr<Ui::FlatLabel>(
				row,
				rpl::single(tr::bold(data.chat)),
				st::defaultFlatLabel),
			padding);
	}

	// The reason and the message take the lines they need: a label of
	// a style without the minimal width stays one line tall whatever its
	// text is, the rest of the message would be cut away.
	const auto status = row->add(
		object_ptr<Ui::FlatLabel>(row, data.status, st::boxDividerLabel),
		padding);
	if (data.problem) {
		status->setTextColorOverride(st::attentionButtonFg->c);
	} else if (data.sending) {
		status->setTextColorOverride(st::windowActiveTextFg->c);
	}
	row->add(
		object_ptr<Ui::FlatLabel>(row, data.text, st::boxLabel),
		style::margins(padding.left(), skip / 4, padding.right(), 0));

	const auto id = data.id;
	auto actions = std::vector<RowAction>();
	const auto action = [&](
			bool allowed,
			const QString &text,
			const Fn<void(uint64)> &callback,
			bool attention = false) {
		if (allowed && callback) {
			actions.push_back({
				.text = text,
				.callback = [=] { callback(id); },
				.attention = attention,
			});
		}
	};
	action(
		data.canWait,
		tr::lng_oblivion_sendonline_action_wait(tr::now),
		args.waitMore);
	action(
		data.canSend,
		tr::lng_oblivion_sendonline_action_send(tr::now),
		args.sendNow);
	action(
		data.canEdit,
		tr::lng_oblivion_sendonline_action_edit(tr::now),
		args.edit);
	action(
		data.canRemove,
		tr::lng_oblivion_sendonline_action_cancel(tr::now),
		args.remove,
		true);
	AddActions(row, list, std::move(actions));
	Ui::AddSkip(row, skip);

	// The lines are between the messages, not under the last one.
	if (!last) {
		auto line = object_ptr<Ui::PlainShadow>(row);
		line->resize(line->width(), st::lineWidth);
		row->add(std::move(line), padding);
	}
}

void ListBox(not_null<Ui::GenericBox*> box, ListArgs &&args) {
	box->setWidth(st::boxWideWidth);
	box->setMaxHeight(st::boxMaxListHeight);
	box->setTitle(tr::lng_oblivion_sendonline_settings_list());

	const auto content = box->verticalLayout();
	const auto &padding = st::boxRowPadding;
	const auto empty = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)),
		style::margins());
	empty->entity()->add(
		object_ptr<Ui::FlatLabel>(
			empty->entity(),
			tr::lng_oblivion_sendonline_list_empty(),
			st::boxLabel),
		style::margins(
			padding.left(),
			st::boxMediumSkip,
			padding.right(),
			0));
	empty->entity()->add(
		object_ptr<Ui::FlatLabel>(
			empty->entity(),
			tr::lng_oblivion_sendonline_list_empty_about(),
			st::boxDividerLabel),
		style::margins(
			padding.left(),
			st::boxLittleSkip,
			padding.right(),
			st::boxMediumSkip));
	const auto list = content->add(
		object_ptr<Ui::VerticalLayout>(content),
		style::margins());

	const auto shared = std::make_shared<ListArgs>(std::move(args));
	const auto showChat = shared->showChat;
	std::move(
		shared->rows
	) | rpl::on_next([=](const std::vector<ListRow> &rows) {
		list->clear();
		for (auto i = 0, count = int(rows.size()); i != count; ++i) {
			AddListRow(list, rows[i], *shared, showChat, (i + 1 == count));
		}
		list->resizeToWidth(content->width());
		empty->toggle(rows.empty(), anim::type::instant);
		box->setAdditionalTitle(rpl::single(rows.empty()
			? QString()
			: QString::number(rows.size())));
	}, box->lifetime());

	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

void EditBox(not_null<Ui::GenericBox*> box, EditArgs &&args) {
	box->setTitle(tr::lng_oblivion_sendonline_edit_title());
	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::newGroupDescription,
		Ui::InputField::Mode::MultiLine,
		tr::lng_oblivion_sendonline_edit_placeholder(),
		args.text));
	field->setMaxLength(kMaxTextLength);
	field->setSubmitSettings(Ui::InputField::SubmitSettings::CtrlEnter);
	field->setMarkdownReplacesEnabled(true);
	box->setFocusCallback([=] {
		field->setFocusFast();
	});
	const auto save = args.save;
	const auto submit = [=] {
		auto text = field->getTextWithAppliedMarkdown();
		if (!ValidText(text) || !save) {
			field->showError();
		} else if (save(std::move(text))) {
			box->closeBox();
		} else {
			field->showError();
		}
	};
	field->submits(
	) | rpl::on_next([=] {
		submit();
	}, field->lifetime());
	box->addButton(tr::lng_settings_save(), submit);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

[[nodiscard]] QString FilePath(not_null<Main::Session*> session) {
	const auto id = (session->isTestMode() ? u"test_"_q : QString())
		+ QString::number(session->userId().bare);
	return cWorkingDir() + u"tdata/oblivion/"_q + id + u"/send_online.json"_q;
}

[[nodiscard]] rpl::event_stream<not_null<Main::Session*>> &Changes() {
	static const auto result
		= new rpl::event_stream<not_null<Main::Session*>>();
	return *result;
}

enum class SendNowResult {
	Started, // Or on its way already.
	Gone, // Not in the queue any more.
	NotLoaded, // The person is not loaded yet, nothing was changed.
};

// The queue of one account with its file, the triggers and the sending.
class Account final : public base::has_weak_ptr {
public:
	explicit Account(not_null<Main::Session*> session);

	[[nodiscard]] const Queue &queue() const {
		return _queue;
	}
	[[nodiscard]] bool onlineNow(uint64 peerId) const;

	// Zero if the message was not taken, notSaved tells that the reason
	// is the file that could not be written.
	uint64 enqueue(
		not_null<UserData*> user,
		const TextWithTags &text,
		bool &notSaved);
	bool edit(uint64 id, const TextWithTags &text);
	bool remove(uint64 id);
	SendNowResult sendNow(uint64 id);
	void waitMore(uint64 id);
	void forget();

	// While a message is being edited or its removal is being confirmed
	// it is not sent, see Queue::begin(). Every hold() is followed by a
	// release(), from the lifetime of the box that asked for it.
	void hold(uint64 id);
	void release(uint64 id);

private:
	struct Hold {
		uint64 peerId = 0;
		int count = 0;
	};

	void load();
	bool save();
	void changed();
	void tick();
	void flush(uint64 peerId);
	void flushLater(uint64 peerId);
	void send(const Item &item);
	void sendDone(uint64 id);
	void sendFailed(uint64 id, const QString &type, int code);
	void stopFor(uint64 peerId, const QString &error);
	void pushWaited();
	void ask();
	[[nodiscard]] bool onlineAt(uint64 peerId, TimeId now) const;
	[[nodiscard]] UserData *loadedUser(uint64 peerId) const;
	[[nodiscard]] base::flat_set<uint64> heldIds() const;

	const not_null<Main::Session*> _session;
	const QString _path;
	Queue _queue;
	base::Timer _timer;
	base::flat_map<uint64, TimeId> _seen; // A message came, see kBurst.
	base::flat_map<uint64, Hold> _holds;
	bool _forgotten = false;

	rpl::lifetime _lifetime;

};

using AccountsMap = base::flat_map<
	not_null<Main::Session*>,
	std::unique_ptr<Account>>;

[[nodiscard]] AccountsMap &Accounts() {
	static const auto result = new AccountsMap();
	return *result;
}

[[nodiscard]] Account *Lookup(not_null<Main::Session*> session) {
	const auto &accounts = Accounts();
	const auto i = accounts.find(session);
	return (i != end(accounts)) ? i->second.get() : nullptr;
}

Account::Account(not_null<Main::Session*> session)
: _session(session)
, _path(FilePath(session))
, _timer([=] { tick(); }) {
	load();

	session->changes().peerUpdates(
		Data::PeerUpdate::Flag::OnlineStatus
	) | rpl::on_next([=](const Data::PeerUpdate &update) {
		const auto peerId = update.peer->id.value;
		if (!_queue.empty() && _queue.count(peerId) > 0) {
			flushLater(peerId);
		}
	}, _lifetime);

	session->changes().messageUpdates(
		Data::MessageUpdate::Flag::NewAdded
	) | rpl::on_next([=](const Data::MessageUpdate &update) {
		if (_queue.empty()) {
			return;
		}
		const auto item = update.item;
		const auto peerId = item->history()->peer->id.value;
		if (item->out()
			|| item->isService()
			|| !item->isRegular()
			|| !_queue.count(peerId)) {
			return;
		}

		// An answer of the person's business bot is not a sign of them.
		// A message marked as a scheduled one still is: that is how
		// "send without going online" of another Oblivion looks from
		// here, and those people often hide their last seen time.
		const auto author = item->Get<HistoryMessageSigned>();
		if (author && author->viaBusinessBot) {
			return;
		}
		const auto now = base::unixtime::now();
		if (item->date() + kFreshMessage >= now) {
			_seen[peerId] = now;
			flushLater(peerId);
		}
	}, _lifetime);

	// «Удалить чат» on this device: what waits for the person is not
	// sent any more, it stays in the list as not sent. The chat has left
	// the chats list by this moment, after «Очистить историю» it is
	// still there and the bar with the waiting messages stays in sight.
	// The event comes from the middle of History::clear(), so only the
	// queue and its file are touched right here.
	session->data().historyCleared(
	) | rpl::on_next([=](not_null<const History*> history) {
		const auto peer = history->peer;
		if (peer->isUser() && !history->inChatList()) {
			stopFor(peer->id.value, kErrorChatDeleted);
		}
	}, _lifetime);

	session->changes().peerUpdates(
		Data::PeerUpdate::Flag::IsBlocked
	) | rpl::on_next([=](const Data::PeerUpdate &update) {
		if (update.peer->isBlocked()) {
			stopFor(update.peer->id.value, kErrorBlocked);
		}
	}, _lifetime);

	if (!_queue.empty()) {
		_timer.callEach(kTick);
		crl::on_main(this, [=] {
			tick();
		});
	}
}

void Account::load() {
	auto file = QFile(_path);
	if (!file.exists()
		|| file.size() > kMaxFileSize
		|| !file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto bytes = file.readAll();
	file.close();
	_queue = Queue::Parse(bytes, base::unixtime::now());

	// What the restart has changed (a message that was being sent) is
	// written at once: the next start must not decide it again. A file
	// that was not understood is left as it is.
	if (!_queue.empty()) {
		save();
	}
}

bool Account::save() {
	if (_forgotten) {
		return false;
	} else if (_queue.empty()) {
		QFile::remove(_path);
		return true;
	}
	QDir().mkpath(QFileInfo(_path).absolutePath());
	auto file = QSaveFile(_path);
	const auto bytes = _queue.serialize();
	return file.open(QIODevice::WriteOnly)
		&& (file.write(bytes) == bytes.size())
		&& file.commit();
}

void Account::changed() {
	if (_queue.empty()) {
		_timer.cancel();
		_seen.clear();
	} else if (!_timer.isActive()) {
		_timer.callEach(kTick);
	}
	pushWaited();
	Changes().fire_copy(_session);
	if (_queue.unasked() > 0) {
		crl::on_main(this, [=] {
			ask();
		});
	}
}

void Account::pushWaited() {
	// Those who hide their last seen time are not asked about: the
	// answer says nothing, only a message from them is a sign of them.
	auto ids = base::flat_set<uint64>();
	if (!_forgotten) {
		for (const auto peerId : _queue.waitedPeers()) {
			const auto user = loadedUser(peerId);
			if (user && !user->lastseen().isHidden()) {
				ids.emplace(peerId);
			}
		}
	}
	Online::SetWaitedUsers(_session, std::move(ids));
}

UserData *Account::loadedUser(uint64 peerId) const {
	const auto id = PeerId(peerId);
	return peerIsUser(id)
		? _session->data().userLoaded(peerToUser(id))
		: nullptr;
}

bool Account::onlineAt(uint64 peerId, TimeId now) const {
	const auto i = _seen.find(peerId);
	if (i != end(_seen) && i->second <= now && i->second + kBurst >= now) {
		return true;
	}
	const auto user = loadedUser(peerId);
	return user && user->lastseen().isOnline(now);
}

base::flat_set<uint64> Account::heldIds() const {
	auto result = base::flat_set<uint64>();
	for (const auto &[id, hold] : _holds) {
		result.emplace(id);
	}
	return result;
}

void Account::hold(uint64 id) {
	const auto item = _queue.find(id);
	auto &hold = _holds[id];
	if (item) {
		hold.peerId = item->peerId;
	}
	++hold.count;
}

void Account::release(uint64 id) {
	const auto i = _holds.find(id);
	if (i == end(_holds)) {
		return;
	} else if (--i->second.count > 0) {
		return;
	}
	const auto peerId = i->second.peerId;
	_holds.erase(i);
	if (peerId && !_forgotten && _queue.count(peerId) > 0) {
		// The person may have come online meanwhile: what was held (with
		// its new text) and what waited behind it goes now.
		flushLater(peerId);
	}
}

void Account::stopFor(uint64 peerId, const QString &error) {
	if (_forgotten || !_queue.cancel(peerId, error)) {
		return;
	}
	save();
	crl::on_main(this, [=] {
		changed();
	});
}

bool Account::onlineNow(uint64 peerId) const {
	return onlineAt(peerId, base::unixtime::now());
}

void Account::tick() {
	if (_forgotten) {
		return;
	}
	const auto now = base::unixtime::now();
	if (!_queue.expire(now).empty()) {
		save();
		changed();
	}
	for (const auto peerId : _queue.readyPeers(now)) {
		flush(peerId);
	}
	pushWaited();
	ask();
	if (_queue.empty()) {
		_timer.cancel();
	}
}

void Account::flushLater(uint64 peerId) {
	crl::on_main(this, [=] {
		flush(peerId);
	});
}

void Account::flush(uint64 peerId) {
	if (_forgotten || Core::Quitting()) {
		return;
	}
	const auto now = base::unixtime::now();
	const auto expired = !_queue.expire(now).empty();

	// Two reasons to wait and never to give up: the person is not loaded
	// yet (right after the start, before the chats arrive), and there is
	// no connection (a request made now would leave whenever it comes
	// back, maybe long after the person was online). The timer tries
	// again, what a restart left to be sent again has its own time limit
	// in Queue::expire().
	const auto ready = loadedUser(peerId)
		&& (_session->mtp().dcstate() == MTP::ConnectedState);
	const auto item = ready
		? _queue.begin(peerId, now, onlineAt(peerId, now), heldIds())
		: std::optional<Item>();
	if (!item) {
		if (expired) {
			save();
			changed();
		}
		return;
	} else if (!save()) {
		// Nothing leaves while "sending" can't be written down.
		_queue.failed(item->id, kErrorSave, false, now + kRetryStep);
		changed();
		return;
	}
	changed();
	send(*item);
}

void Account::send(const Item &item) {
	const auto id = item.id;
	const auto user = loadedUser(item.peerId);
	const auto refuse = [&](const QString &error) {
		_queue.failed(id, error, true, 0);
		save();
		changed();
	};
	if (!user) {
		// flush() has just seen the person loaded. Still, "not loaded" is
		// never a refusal: the message waits and is tried again.
		_queue.failed(
			id,
			QString(),
			false,
			base::unixtime::now() + kRetryStep);
		save();
		changed();
		return;
	} else if (const auto reason = Unavailable(user); !reason.isEmpty()) {
		refuse(reason);
		return;
	}
	const auto session = _session;
	const auto history = session->data().history(user);
	const auto peer = history->peer;
	auto sending = TextWithEntities{
		item.text.text,
		TextUtilities::ConvertTextTagsToEntities(item.text.tags),
	};
	TextUtilities::PrepareForSending(
		sending,
		Ui::ItemTextOptions(history, session->user()).flags);
	TextUtilities::Trim(sending);
	const auto randomId = item.randomId;
	if (sending.empty()) {
		refuse(kErrorEmpty);
		return;
	} else if (session->data().messageIdByRandomId(randomId)) {
		// An attempt of this run is still on its way: its answer decides,
		// a repeat with the same random id is harmless.
		_queue.failed(
			id,
			QString(),
			false,
			base::unixtime::now() + kRetryStep);
		save();
		changed();
		return;
	}

	// "Send without going online" of the ghost mode, as for any text.
	//
	// One difference. A send made by the user reads the chat on the
	// server at that moment (ReadOnSend), and when the message is
	// delivered everything before it is marked as read, here and for the
	// person, on that ground (ReadDelivered in oblivion_sending.cpp).
	// This send reads nothing, the user may be away and may not have seen
	// what the person wrote. So the delivery is first announced with an
	// unknown moment of sending (RefreshSendOptions), and the send itself
	// joins that announcement: the delivery then reads nothing in a chat
	// that is read invisibly, unless the user has sent something there
	// by hand shortly before and nobody wrote since.
	auto action = Api::SendAction(history, Api::SendOptions());
	if (Get().offlineSend()) {
		auto announce = Api::SendOptions();
		announce.oblivionOffline = true;
		RefreshSendOptions(announce, peer);
	}
	AdjustSendAction(action, sending.text, 1);
	const auto scheduled = action.options.scheduled;

	const auto newId = FullMsgId(
		peer->id,
		session->data().nextLocalMessageId());
	session->data().registerMessageRandomId(randomId, newId);
	session->data().registerMessageSentData(
		randomId,
		peer->id,
		sending.text);

	auto flags = NewMessageFlags(peer);
	auto sendFlags = MTPmessages_SendMessage::Flags(0);
	if (ShouldSendSilent(peer, action.options)) {
		sendFlags |= MTPmessages_SendMessage::Flag::f_silent;
	}
	const auto sentEntities = Api::EntitiesToMTP(
		session,
		sending.entities,
		Api::ConvertOption::SkipLocal);
	if (!sentEntities.v.isEmpty()) {
		sendFlags |= MTPmessages_SendMessage::Flag::f_entities;
	}
	if (scheduled) {
		flags |= MessageFlag::IsOrWasScheduled;
		sendFlags |= MTPmessages_SendMessage::Flag::f_schedule_date;
	}
	history->addNewLocalMessage({
		.id = newId.msg,
		.flags = flags,
		.from = NewMessageFromId(action),
		.date = NewMessageDate(action.options),
	}, sending, MTP_messageMediaEmpty());

	const auto api = &session->api();
	const auto weak = base::make_weak(this);
	const auto text = sending.text;
	const auto dropLocal = [=] {
		session->data().unregisterMessageRandomId(randomId);
		session->data().unregisterMessageSentData(randomId);
		if (const auto local = session->data().message(newId)) {
			local->destroy();
		}
	};
	auto &histories = session->data().histories();
	histories.sendRequest(history, Data::Histories::RequestType::Send, [=](
			Fn<void()> finish) {
		history->sendRequestId = api->request(MTPmessages_SendMessage(
			MTP_flags(sendFlags),
			peer->input(),
			MTPInputReplyTo(),
			MTP_string(text),
			MTP_long(randomId),
			MTPReplyMarkup(),
			sentEntities,
			MTP_int(scheduled),
			MTP_int(0), // schedule_repeat_period
			MTP_inputPeerEmpty(), // send_as
			MTPInputQuickReplyShortcut(),
			MTP_long(0), // effect
			MTP_long(0), // allow_paid_stars
			MTPSuggestedPost(),
			MTPInputRichMessage()
		)).done([=](const MTPUpdates &result) {
			api->applyUpdates(result, randomId);
			finish();
			if (const auto strong = weak.get()) {
				strong->sendDone(id);
			}
		}).fail([=](const MTP::Error &error) {
			// All the errors come here (handleAllErrors): the default
			// handling repeats a request after an error 500 for ever,
			// and RANDOM_ID_DUPLICATE is one of those.
			dropLocal();
			finish();
			if (const auto strong = weak.get()) {
				strong->sendFailed(id, error.type(), error.code());
			}
		}).handleAllErrors().afterRequest(
			history->sendRequestId
		).send();
		return history->sendRequestId;
	});
}

void Account::sendDone(uint64 id) {
	const auto item = _queue.find(id);
	const auto peerId = item ? item->peerId : uint64();
	_queue.sent(id);
	save();
	changed();
	if (peerId && !_forgotten) {
		// The next one for this person, if they are still here: a
		// message sent by «Отправить сейчас» takes nothing along.
		flushLater(peerId);
	}
}

void Account::sendFailed(uint64 id, const QString &type, int code) {
	const auto item = _queue.find(id);
	if (!item) {
		return;
	}
	const auto now = base::unixtime::now();
	const auto verdict = Judge(type, code, item->attempts, now);
	if (verdict.sent) {
		sendDone(id);
		return;
	}
	_queue.failed(
		id,
		type,
		verdict.permanent,
		verdict.retryAt,
		verdict.unsure);
	save();
	changed();
}

uint64 Account::enqueue(
		not_null<UserData*> user,
		const TextWithTags &text,
		bool &notSaved) {
	notSaved = false;
	if (_forgotten) {
		return 0;
	}
	const auto peerId = user->id.value;
	const auto now = base::unixtime::now();
	auto id = uint64();
	for (auto i = 0; !id && i != 4; ++i) {
		const auto randomId = base::RandomValue<uint64>();
		if (!randomId) {
			continue;
		}
		id = _queue.add(
			peerId,
			user->name(),
			text,
			randomId,
			now,
			Get().sendWhenOnlineHours());
		if (!id && (_queue.count(peerId) >= kMaxPerPeer
			|| int(_queue.items().size()) >= kMaxTotal
			|| !ValidText(text))) {
			break;
		}
	}
	if (!id) {
		return 0;
	} else if (!save()) {
		_queue.remove(id);
		notSaved = true;
		return 0;
	}
	changed();
	flushLater(peerId);
	return id;
}

bool Account::edit(uint64 id, const TextWithTags &text) {
	if (_forgotten || !_queue.edit(id, text)) {
		return false;
	}
	save();
	changed();
	return true;
}

bool Account::remove(uint64 id) {
	if (_forgotten || !_queue.remove(id)) {
		return false;
	}
	save();
	changed();
	return true;
}

SendNowResult Account::sendNow(uint64 id) {
	const auto item = _queue.find(id);
	if (_forgotten || !item) {
		return SendNowResult::Gone;
	} else if (item->state == State::Sending) {
		return SendNowResult::Started;
	}
	const auto peerId = item->peerId;
	if (!loadedUser(peerId)) {
		// Otherwise the click would work nobody knows when: at the moment
		// the chat happens to get loaded.
		return SendNowResult::NotLoaded;
	}
	_queue.resend(id);
	save();
	changed();
	flushLater(peerId);
	return SendNowResult::Started;
}

void Account::waitMore(uint64 id) {
	const auto item = _queue.find(id);
	const auto peerId = item ? item->peerId : uint64();
	if (!_forgotten
		&& _queue.extend(
			id,
			base::unixtime::now(),
			Get().sendWhenOnlineHours())) {
		save();
		changed();
		flushLater(peerId);
	}
}

void Account::forget() {
	_queue.clear();
	_timer.cancel();
	_seen.clear();
	_holds.clear();
	QFile::remove(_path);
	pushWaited();
	_forgotten = true;
	Changes().fire_copy(_session);
}

void Account::ask() {
	const auto count = _queue.unasked();
	if (!count || _forgotten || Core::Quitting()) {
		return;
	}
	const auto window = Core::App().activeWindow();
	const auto controller = window ? window->sessionController() : nullptr;
	if (!controller
		|| (&controller->session() != _session)
		|| Core::App().passcodeLocked()) {
		// Asked later, by the timer: when this account is in front.
		return;
	}
	_queue.markAsked();
	save();
	const auto weak = base::make_weak(controller);
	controller->show(Ui::MakeConfirmBox({
		.text = tr::lng_oblivion_sendonline_ask_text(
			tr::now,
			lt_count,
			count),
		.confirmed = [=](Fn<void()> close) {
			close();
			if (const auto strong = weak.get()) {
				ShowList(strong);
			}
		},
		.confirmText = tr::lng_oblivion_sendonline_ask_open(),
		.cancelText = tr::lng_oblivion_sendonline_ask_later(),
		.title = tr::lng_oblivion_sendonline_ask_title(),
	}), Ui::LayerOption::KeepOther);
}

[[nodiscard]] QString ChatName(
		not_null<Main::Session*> session,
		const Item &item) {
	const auto peer = session->data().peerLoaded(PeerId(item.peerId));
	return peer ? peer->name() : item.peerName;
}

[[nodiscard]] ListRow MakeRow(
		const Queue &queue,
		const Item &item,
		const QString &chat,
		TimeId now) {
	const auto problem = Problem(item.state);
	const auto sending = OnItsWay(item);
	return {
		.id = item.id,
		.chat = chat,
		.text = Preview(item.text.text, kListTextLength),
		.status = sending
			? tr::lng_oblivion_sendonline_bar_sending(tr::now)
			: problem
			? ReasonText(item)
			: (item.attempts > 0 && !item.error.isEmpty())
			? tr::lng_oblivion_sendonline_status_retry(tr::now)
			: tr::lng_oblivion_sendonline_status_waiting(
				tr::now,
				lt_date,
				MomentText(item.deadline, now)),
		.problem = problem,
		.sending = sending,
		.canEdit = queue.canEdit(item.id),
		.canSend = (item.state != State::Sending),
		.canWait = (item.state == State::Expired),
		.canRemove = (item.state != State::Sending),
	};
}

[[nodiscard]] std::vector<ListRow> CollectRows(
		not_null<Main::Session*> session,
		uint64 peerId) {
	auto result = std::vector<ListRow>();
	const auto account = Lookup(session);
	if (!account) {
		return result;
	}
	const auto now = base::unixtime::now();
	const auto &queue = account->queue();
	for (const auto &item : queue.items()) {
		if (!peerId || item.peerId == peerId) {
			result.push_back(
				MakeRow(queue, item, ChatName(session, item), now));
		}
	}
	return result;
}

void ShowListFor(
		not_null<Window::SessionController*> controller,
		uint64 peerId) {
	const auto session = &controller->session();
	Start(session);
	const auto weak = base::make_weak(controller);
	const auto show = controller->uiShow();

	// The list may be a moment behind the queue: the message of a row
	// that was clicked may be gone or on its way already.
	const auto tellLost = [=](uint64 id) {
		const auto account = Lookup(session);
		const auto item = account ? account->queue().find(id) : nullptr;
		show->showToast(!item
			? tr::lng_oblivion_sendonline_gone(tr::now)
			: (item->state == State::Sending)
			? tr::lng_oblivion_sendonline_busy(tr::now)
			: tr::lng_oblivion_sendonline_reason_unconfirmed(tr::now));
	};
	const auto with = [=](uint64 id, Fn<void(
			not_null<Window::SessionController*>,
			not_null<Account*>,
			const Item &)> callback) {
		const auto strong = weak.get();
		const auto account = Lookup(session);
		const auto item = account ? account->queue().find(id) : nullptr;
		if (!strong) {
			return;
		} else if (!item) {
			tellLost(id);
			return;
		}
		callback(strong, account, *item);
	};

	// While the box is there the message is not sent, see Account::hold.
	// Whatever closes the box (a button, Escape, the passcode lock, the
	// window) releases it, and the account may be gone by then.
	const auto holdWhile = [=](
			not_null<Account*> account,
			uint64 id,
			not_null<Ui::RpWidget*> box) {
		account->hold(id);
		box->lifetime().add([=, account = base::make_weak(account)] {
			if (const auto strong = account.get()) {
				strong->release(id);
			}
		});
	};
	auto rows = rpl::single(
		rpl::empty_value()
	) | rpl::then(Changes().events(
	) | rpl::filter([=](not_null<Main::Session*> changed) {
		return (changed == session);
	}) | rpl::to_empty) | rpl::map([=] {
		return CollectRows(session, peerId);
	});
	controller->show(Box(ListBox, ListArgs{
		.rows = std::move(rows),
		.showChat = !peerId,
		.edit = [=](uint64 id) {
			with(id, [=](
					not_null<Window::SessionController*> strong,
					not_null<Account*> account,
					const Item &item) {
				if (!account->queue().canEdit(id)) {
					tellLost(id);
					return;
				}
				auto box = Box(EditBox, EditArgs{
					.text = item.text,
					.save = [=](TextWithTags text) {
						// The box closes in any case: a message that can't
						// be changed any more is not a mistake in the text.
						const auto account = Lookup(session);
						if (!account || !account->edit(id, text)) {
							tellLost(id);
						}
						return true;
					},
				});
				holdWhile(account, id, box.data());
				strong->show(std::move(box));
			});
		},
		.sendNow = [=](uint64 id) {
			with(id, [=](
					not_null<Window::SessionController*> strong,
					not_null<Account*> account,
					const Item &item) {
				const auto name = ChatName(session, item);
				const auto unsure = item.unsure
					|| (item.state == State::Unconfirmed);
				strong->show(Ui::MakeConfirmBox({
					.text = unsure
						? tr::lng_oblivion_sendonline_unconfirmed_sure(
							tr::now)
						: tr::lng_oblivion_sendonline_send_sure(
							tr::now,
							lt_name,
							name),
					.confirmed = [=](Fn<void()> close) {
						close();
						const auto account = Lookup(session);
						const auto result = account
							? account->sendNow(id)
							: SendNowResult::Gone;
						if (result == SendNowResult::Gone) {
							tellLost(id);
						} else if (result == SendNowResult::NotLoaded) {
							show->showToast(
								tr::lng_oblivion_sendonline_not_loaded(
									tr::now));
						}
					},
					.confirmText = tr::lng_send_button(),
				}));
			});
		},
		.waitMore = [=](uint64 id) {
			if (const auto account = Lookup(session)) {
				account->waitMore(id);
			}
		},
		.remove = [=](uint64 id) {
			with(id, [=](
					not_null<Window::SessionController*> strong,
					not_null<Account*> account,
					const Item &item) {
				if (item.state == State::Sending) {
					tellLost(id);
					return;
				}
				auto box = Ui::MakeConfirmBox({
					.text = tr::lng_oblivion_sendonline_cancel_sure(),
					.confirmed = [=](Fn<void()> close) {
						// Removed first: closing the box lets the queue go.
						const auto account = Lookup(session);
						if (!account || !account->remove(id)) {
							tellLost(id);
						}
						close();
					},
					.confirmText
						= tr::lng_oblivion_sendonline_action_cancel(),
					.confirmStyle = &st::attentionBoxButton,
				});
				holdWhile(account, id, box.data());
				strong->show(std::move(box));
			});
		},
	}));
}

[[nodiscard]] object_ptr<Ui::SlideWrap<Bar>> CreateBar(
		not_null<QWidget*> parent,
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	auto result = object_ptr<Ui::SlideWrap<Bar>>(
		parent,
		object_ptr<Bar>(parent));
	const auto raw = result.data();
	const auto bar = raw->entity();
	raw->hide(anim::type::instant);

	const auto session = &peer->session();
	const auto peerId = peer->id.value;
	const auto refresh = [=](anim::type animated) {
		const auto account = Lookup(session);
		auto content = account
			? ContentFor(account->queue().items(), peerId)
			: BarContent();
		const auto shown = content.shown;
		if (shown) {
			bar->setContent(std::move(content));
		}
		raw->toggle(shown, animated);
	};
	Changes().events(
	) | rpl::filter([=](not_null<Main::Session*> changed) {
		return (changed == session);
	}) | rpl::on_next([=] {
		refresh(anim::type::normal);
	}, raw->lifetime());
	refresh(anim::type::instant);

	bar->clicks(
	) | rpl::on_next([=] {
		ShowListFor(controller, peerId);
	}, raw->lifetime());
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

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	const auto kWide = 640;
	const auto kNarrow = 380;
	const auto bar = [](const QString &name, int width, Fn<BarContent()> make) {
		RegisterScene(name, QSize(width, 0), [=](
				not_null<Ui::RpWidget*> parent) {
			const auto result = Ui::CreateChild<Bar>(parent.get());
			result->setContent(make());
			return result;
		});
	};
	const auto first = [] {
		return SampleText(
			"Привет! Как освободишься, набери меня, есть новости",
			"Hi! Call me when you are free, I have some news");
	};
	const auto items = [=](int count, State state, QString error = {}) {
		auto result = std::vector<Item>();
		for (auto i = 0; i != count; ++i) {
			result.push_back({
				.id = uint64(i + 1),
				.peerId = 7,
				.randomId = uint64(i + 100),
				.text = { first() },
				.state = state,
				.error = error,
			});
		}
		return result;
	};
	bar(u"sendonline_bar_waiting"_q, kWide, [=] {
		return ContentFor(items(1, State::Waiting), 7);
	});
	bar(u"sendonline_bar_waiting_many"_q, kWide, [=] {
		return ContentFor(items(3, State::Waiting), 7);
	});
	bar(u"sendonline_bar_waiting_narrow"_q, kNarrow, [=] {
		return ContentFor(items(2, State::Waiting), 7);
	});
	bar(u"sendonline_bar_sending"_q, kWide, [=] {
		return ContentFor(items(1, State::Sending), 7);
	});
	bar(u"sendonline_bar_expired"_q, kWide, [=] {
		return ContentFor(items(1, State::Expired), 7);
	});
	bar(u"sendonline_bar_failed"_q, kWide, [=] {
		return ContentFor(items(2, State::Failed, kErrorUnavailable), 7);
	});
	bar(u"sendonline_bar_chat_deleted"_q, kWide, [=] {
		return ContentFor(items(1, State::Failed, kErrorChatDeleted), 7);
	});
	bar(u"sendonline_bar_blocked"_q, kNarrow, [=] {
		return ContentFor(items(3, State::Failed, kErrorBlocked), 7);
	});

	const auto size = QSize(st::boxWideWidth * 2, 0);
	const auto anna = [] { return SampleText("Аня Смирнова", "Anna Smirnova"); };
	const auto boris = [] { return SampleText("Борис", "Boris"); };

	// The links of a row are there only for the actions the list was
	// given, so the scenes give all of them.
	const auto nothing = [](uint64) {};
	RegisterBoxScene(u"sendonline_list"_q, size, [=](
			std::shared_ptr<Ui::Show> show) {
		auto rows = std::vector<ListRow>{
			{
				.id = 1,
				.chat = anna(),
				.text = first(),
				.status = tr::lng_oblivion_sendonline_status_waiting(
					tr::now,
					lt_date,
					u"18:40"_q),
				.canEdit = true,
				.canSend = true,
				.canRemove = true,
			},
			{
				.id = 2,
				.chat = anna(),
				.text = SampleText(
					"И захвати, пожалуйста, зарядку от ноутбука",
					"And please bring the laptop charger"),
				.status = tr::lng_oblivion_sendonline_status_waiting(
					tr::now,
					lt_date,
					SampleText("8 окт, 18:41", "8 Oct, 18:41")),
				.canEdit = true,
				.canSend = true,
				.canRemove = true,
			},
			{
				.id = 3,
				.chat = boris(),
				.text = SampleText(
					"С днём рождения! Пусть всё получается.",
					"Happy birthday! May everything work out."),
				.status = tr::lng_oblivion_sendonline_reason_expired(
					tr::now),
				.problem = true,
				.canEdit = true,
				.canSend = true,
				.canWait = true,
				.canRemove = true,
			},
			{
				.id = 4,
				.chat = SampleText("Мама", "Mom"),
				.text = SampleText(
					"Доехал, всё хорошо, позвоню вечером.",
					"I've arrived, all is well, will call tonight."),
				.status = tr::lng_oblivion_sendonline_reason_unconfirmed(
					tr::now),
				.problem = true,
				.canSend = true,
				.canRemove = true,
			},
		};
		return Box(ListBox, ListArgs{
			.rows = rpl::single(std::move(rows)),
			.edit = nothing,
			.sendNow = nothing,
			.waitMore = nothing,
			.remove = nothing,
		});
	});
	RegisterBoxScene(u"sendonline_list_chat"_q, size, [=](
			std::shared_ptr<Ui::Show> show) {
		auto rows = std::vector<ListRow>{
			{
				.id = 1,
				.chat = anna(),
				.text = first(),
				.status = tr::lng_oblivion_sendonline_bar_sending(tr::now),
				.sending = true,
			},
			{
				.id = 2,
				.chat = anna(),
				.text = SampleText(
					"И захвати, пожалуйста, зарядку от ноутбука",
					"And please bring the laptop charger"),
				.status = tr::lng_oblivion_sendonline_status_retry(tr::now),
				.canEdit = true,
				.canSend = true,
				.canRemove = true,
			},
		};
		return Box(ListBox, ListArgs{
			.rows = rpl::single(std::move(rows)),
			.showChat = false,
			.edit = nothing,
			.sendNow = nothing,
			.waitMore = nothing,
			.remove = nothing,
		});
	});
	RegisterBoxScene(u"sendonline_list_empty"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(ListBox, ListArgs{
			.rows = rpl::single(std::vector<ListRow>()),
		});
	});
	RegisterBoxScene(u"sendonline_edit"_q, size, [=](
			std::shared_ptr<Ui::Show> show) {
		return Box(EditBox, EditArgs{
			.text = { first() },
			.save = [](TextWithTags) { return true; },
		});
	});
});

} // namespace

void Start(not_null<Main::Session*> session) {
	auto &accounts = Accounts();
	if (accounts.contains(session)) {
		return;
	}
	accounts.emplace(session, std::make_unique<Account>(session));
	session->lifetime().add([=] {
		Accounts().remove(session);
	});
}

void Forget(not_null<Main::Session*> session) {
	if (const auto account = Lookup(session)) {
		account->forget();
	} else {
		QFile::remove(FilePath(session));
	}
}

void ShowList(not_null<Window::SessionController*> controller) {
	ShowListFor(controller, 0);
}

bool Offered(not_null<PeerData*> peer) {
	return Get().sendWhenOnline()
		&& Eligible(peer)
		&& Unavailable(peer->asUser()).isEmpty();
}

bool Enqueue(
		not_null<Window::SessionController*> controller,
		not_null<History*> history,
		const TextWithTags &text) {
	const auto peer = history->peer;
	const auto user = peer->asUser();
	const auto session = &history->session();
	if (!user || !Eligible(peer) || text.text.trimmed().isEmpty()) {
		return false;
	} else if (const auto reason = Unavailable(user); !reason.isEmpty()) {
		controller->showToast((reason == kErrorPaid)
			? tr::lng_oblivion_sendonline_reason_paid(tr::now)
			: (reason == kErrorBlocked)
			? tr::lng_oblivion_sendonline_reason_blocked(tr::now)
			: tr::lng_oblivion_sendonline_reason_unavailable(tr::now));
		return false;
	} else if (text.text.size() > kMaxTextLength) {
		controller->showToast(tr::lng_oblivion_sendonline_too_long(tr::now));
		return false;
	}
	Start(session);
	const auto account = Lookup(session);
	auto notSaved = false;
	if (!account || !account->enqueue(user, text, notSaved)) {
		controller->showToast(notSaved
			? tr::lng_oblivion_sendonline_not_saved(tr::now)
			: tr::lng_oblivion_sendonline_too_many(tr::now));
		return false;
	}
	if (!account->onlineNow(peer->id.value)) {
		const auto name = user->shortName();
		controller->showToast(user->lastseen().isHidden()
			? tr::lng_oblivion_sendonline_queued_hidden(
				tr::now,
				lt_name,
				name)
			: tr::lng_oblivion_sendonline_queued(tr::now, lt_name, name));
	}
	return true;
}

object_ptr<Ui::RpWidget> WithChatBar(
		not_null<QWidget*> parent,
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		Fn<object_ptr<Ui::RpWidget>(not_null<QWidget*>)> makeAbove) {
	if (!Eligible(peer)) {
		return makeAbove ? makeAbove(parent) : nullptr;
	}
	auto result = object_ptr<Stack>(parent);
	const auto raw = result.data();
	auto mine = CreateBar(raw, controller, peer);
	const auto bar = mine->entity();
	raw->setBars(
		makeAbove ? makeAbove(raw) : nullptr,
		std::move(mine),
		[=](bool aboveShown) { bar->setTopLine(!aboveShown); });

	// Created in a chat that is on the screen already. The bars inside
	// show and hide themselves, with nothing in them the height is zero.
	raw->show();
	return result;
}

bool RunSelfTest(QStringList &log) {
	auto check = Checker(log);
	const auto text = [](const QString &value) {
		return TextWithTags{ value };
	};
	const auto hour = TimeId(3600);
	const auto t0 = TimeId(1791362460);

	{
		auto queue = Queue();
		const auto a = queue.add(7, u"Anna"_q, text(u"first"_q), 111, t0, 24);
		const auto b = queue.add(7, u"Anna"_q, text(u"second"_q), 112, t0, 24);
		const auto c = queue.add(8, u"Boris"_q, text(u"other"_q), 113, t0, 1);
		check(a && b && c && a != b && b != c, "add: ids");
		check(!queue.add(7, u"Anna"_q, text(u"copy"_q), 111, t0, 24),
			"add: a random id that is in the queue is refused");
		check(!queue.add(7, u"Anna"_q, text(u"   "_q), 114, t0, 24),
			"add: no text");
		check(!queue.add(7, u"Anna"_q, text(u"x"_q), 0, t0, 24),
			"add: no random id");
		check(!queue.add(
			7,
			u"Anna"_q,
			text(QString(kMaxTextLength + 1, QChar('x'))),
			115,
			t0,
			24), "add: too long");
		check(queue.count(7) == 2 && queue.count(8) == 1, "add: count");
		check(queue.find(a)->deadline == t0 + 24 * hour
			&& queue.find(c)->deadline == t0 + hour,
			"add: the time limit");

		check(!queue.begin(7, t0 + 10, false), "begin: not online");
		const auto first = queue.begin(7, t0 + 20, true);
		check(first
			&& first->id == a
			&& first->randomId == 111
			&& first->attempts == 1
			&& queue.find(a)->state == State::Sending
			&& queue.find(a)->attempted == t0 + 20,
			"begin: the first one, marked as being sent");
		check(!queue.begin(7, t0 + 21, true),
			"begin: one at a time for a person");
		check(!queue.edit(a, text(u"changed"_q)) && !queue.remove(a),
			"sending: neither edited nor removed");
		const auto other = queue.begin(8, t0 + 21, true);
		check(other && other->id == c, "begin: another person is not held");
		queue.sent(a);
		check(!queue.find(a) && queue.count(7) == 1, "sent: removed");
		const auto second = queue.begin(7, t0 + 22, true);
		check(second && second->id == b && second->randomId == 112,
			"begin: the next one in the order");
		queue.sent(b);
		queue.sent(c);
		check(queue.empty(), "sent: the queue is empty");
		check(queue.add(7, u"Anna"_q, text(u"again"_q), 116, t0, 24) > c,
			"add: ids are not reused");
	}
	check.section("queue");

	{
		auto queue = Queue();
		const auto a = queue.add(7, u"Anna"_q, text(u"first"_q), 211, t0, 24);
		const auto b = queue.add(7, u"Anna"_q, text(u"second"_q), 212, t0, 24);
		check(queue.begin(7, t0, true).has_value(), "retry: first attempt");
		queue.failed(a, u"FLOOD_WAIT_30"_q, false, t0 + 35);
		check(queue.find(a)->state == State::Waiting
			&& queue.find(a)->retryAt == t0 + 35
			&& queue.find(a)->randomId == 211,
			"retry: waits with the same random id");
		check(!queue.begin(7, t0 + 10, true),
			"retry: nothing goes before the time, the order is kept");
		check(queue.readyPeers(t0 + 10).empty()
			&& queue.readyPeers(t0 + 35) == std::vector<uint64>{ 7 },
			"retry: ready when the time comes");
		const auto again = queue.begin(7, t0 + 40, true);
		check(again
			&& again->id == a
			&& again->randomId == 211
			&& again->attempts == 2,
			"retry: the same message with the same random id");
		for (auto i = 2; i != kMaxAttempts; ++i) {
			queue.failed(a, u"INTERNAL"_q, false, t0 + 41);
			check(queue.begin(7, t0 + 50, true).has_value(), "retry: more");
		}
		queue.failed(a, u"INTERNAL"_q, false, t0 + 41);
		check(queue.find(a)->state == State::Failed
			&& !queue.find(a)->asked
			&& queue.unasked() == 1,
			"retry: gives up after the last attempt and asks");
		const auto next = queue.begin(7, t0 + 60, true);
		check(next && next->id == b, "failed: the next one is not held");
		queue.failed(b, u"USER_IS_BLOCKED"_q, true, 0);
		check(queue.find(b)->state == State::Failed
			&& queue.find(b)->attempts == 1,
			"failed: a refusal is final at once");
		check(!queue.begin(7, t0 + 70, true), "failed: nothing to send");
		queue.markAsked();
		check(!queue.unasked(), "asked: once");
		check(queue.resend(a)
			&& queue.find(a)->state == State::Waiting
			&& queue.find(a)->forced,
			"send now: waits as forced");
		const auto forced = queue.begin(7, t0 + 80, false);
		check(forced && forced->id == a && forced->randomId == 211,
			"send now: goes without the person online, the same id");
	}
	check.section("errors");

	{
		auto queue = Queue();
		const auto a = queue.add(7, u"Anna"_q, text(u"first"_q), 311, t0, 1);
		const auto b = queue.add(7, u"Anna"_q, text(u"second"_q), 312, t0, 2);
		check(queue.expire(t0 + hour - 1).empty(), "limit: not yet");
		check(queue.expire(t0 + hour) == std::vector<uint64>{ a },
			"limit: the time is over");
		check(queue.find(a)->state == State::Expired
			&& queue.unasked() == 1,
			"limit: expired and not asked yet");
		const auto next = queue.begin(7, t0 + hour + 1, true);
		check(next && next->id == b, "limit: an expired one is not sent");
		queue.sent(b);
		check(!queue.begin(7, t0 + hour + 2, true),
			"limit: never by itself after the time");
		check(queue.canEdit(a)
			&& queue.edit(a, text(u"first, edited"_q))
			&& queue.find(a)->text.text == u"first, edited"_q
			&& queue.find(a)->randomId == 311,
			"edit: the text only");
		check(queue.extend(a, t0 + 2 * hour, 6)
			&& queue.find(a)->state == State::Waiting
			&& queue.find(a)->deadline == t0 + 8 * hour,
			"wait more: a new limit");
		check(!queue.extend(a, t0, 1), "wait more: only for an expired one");
		const auto late = queue.add(7, u"Anna"_q, text(u"late"_q), 313, t0, 1);
		check(queue.begin(7, t0 + 3 * hour, true)->id == a
			&& queue.find(late)->state == State::Waiting,
			"limit: checked when sending too");
		queue.sent(a);
		check(!queue.begin(7, t0 + 3 * hour, true)
			&& queue.find(late)->state == State::Expired,
			"limit: the one whose time is over is skipped");
		check(queue.remove(late) && queue.empty(), "remove");
	}
	check.section("time limit");

	{
		auto queue = Queue();
		auto tagged = TextWithTags{ u"bold and plain"_q };
		tagged.tags.push_back({ .offset = 0, .length = 4, .id = u"**"_q });
		const auto a = queue.add(7, u"Аня"_q, tagged, 411, t0, 24);
		const auto b = queue.add(8, u"Boris"_q, text(u"two"_q), 412, t0, 24);
		const auto bytes = queue.serialize();
		auto same = Queue::Parse(bytes, t0 + 5);
		check(same.serialize() == bytes, "file: round trip");
		check(same.find(a)
			&& same.find(a)->text == tagged
			&& same.find(a)->peerName == u"Аня"_q
			&& same.find(a)->randomId == 411,
			"file: the text with its formatting and the ids");
		check(same.add(9, u"C"_q, text(u"three"_q), 413, t0, 24) > b,
			"file: ids go on after a restart");

		// The app stops while a message is being sent.
		check(queue.begin(7, t0 + 100, true).has_value(), "restart: sending");
		const auto stopped = queue.serialize();
		auto soon = Queue::Parse(stopped, t0 + 100 + kResumeWindow);
		check(soon.find(a)->state == State::Waiting
			&& soon.find(a)->forced
			&& soon.find(a)->randomId == 411
			&& soon.find(a)->attempts == 1,
			"restart soon: waits to be sent again");
		check(!soon.canEdit(a),
			"restart soon: the text can't be changed any more");
		const auto resumed = soon.begin(7, t0 + 120, false);
		check(resumed
			&& resumed->randomId == 411
			&& resumed->attempts == 2,
			"restart soon: goes again with the same random id");
		check(Judge(u"RANDOM_ID_DUPLICATE"_q, 500, 2, t0).sent,
			"restart soon: a repeat the server has is taken as sent");
		soon.sent(a);
		check(!soon.find(a) && soon.find(b), "restart soon: done once");

		auto later = Queue::Parse(stopped, t0 + 101 + kResumeWindow);
		check(later.find(a)->state == State::Unconfirmed
			&& later.unasked() == 1,
			"restart later: not confirmed, the user is asked");
		check(!later.begin(7, t0 + 5000, true)
			&& later.readyPeers(t0 + 5000) == std::vector<uint64>{ 8 },
			"restart later: never sent by itself");
		check(!later.canEdit(a) && !later.extend(a, t0, 1),
			"restart later: only «send» or «remove»");
		auto again = Queue::Parse(later.serialize(), t0 + 9000);
		check(again.find(a)->state == State::Unconfirmed,
			"restart later: stays so after one more restart");
		check(later.resend(a)
			&& later.begin(7, t0 + 5001, false)->randomId == 411,
			"restart later: a click sends it, still with its random id");

		auto back = Queue::Parse(stopped, t0 + 50);
		check(back.find(a)->state == State::Unconfirmed,
			"restart: a clock that went back is not trusted");

		// The chat is not loaded for a while after the start, so the
		// message can't go at once: the time limit still holds.
		auto slow = Queue::Parse(stopped, t0 + 110);
		check(slow.find(a)->resumed
			&& slow.find(a)->unsure
			&& slow.expire(t0 + 100 + kResumeWindow).empty()
			&& slow.readyPeers(t0 + 100 + kResumeWindow)
				== std::vector<uint64>{ 7, 8 },
			"restart soon: waits for its chat, still in time");
		check(slow.expire(t0 + 101 + kResumeWindow)
				== std::vector<uint64>{ a }
			&& slow.find(a)->state == State::Unconfirmed
			&& !slow.find(a)->forced
			&& slow.unasked() == 1,
			"restart soon: too late to repeat by itself, the user is asked");
		check(!slow.begin(7, t0 + 102 + kResumeWindow, true),
			"restart soon: nothing goes by itself after that");
		auto twice = Queue::Parse(
			Queue::Parse(stopped, t0 + 110).serialize(),
			t0 + 120);
		check(twice.find(a)->state == State::Waiting
			&& twice.find(a)->forced
			&& twice.find(a)->resumed,
			"restart soon: one more restart keeps the time limit");
		check(!twice.begin(7, t0 + 101 + kResumeWindow, false)
			&& twice.find(a)->state == State::Unconfirmed,
			"restart soon: the limit is checked when sending too");
		auto clicked = Queue::Parse(stopped, t0 + 110);
		check(clicked.resend(a)
			&& !clicked.find(a)->resumed
			&& clicked.expire(t0 + 5000).empty()
			&& clicked.begin(7, t0 + 5000, false)->randomId == 411,
			"restart soon: a click has no time limit");
	}
	check.section("restart");

	{
		check(Queue::Parse("not json", t0).empty(), "broken: not json");
		check(Queue::Parse("[1,2]", t0).empty(), "broken: not an object");
		check(Queue::Parse("{\"version\":2,\"items\":[]}", t0).empty(),
			"broken: another version");
		auto queue = Queue();
		const auto a = queue.add(7, u"Anna"_q, text(u"one"_q), 511, t0, 24);
		auto object = QJsonDocument::fromJson(queue.serialize()).object();
		auto list = object.value(u"items"_q).toArray();
		const auto item = list.at(0).toObject();
		list.push_back(item); // The same record twice.
		auto twin = item; // Another id, the same random id.
		twin.insert(u"id"_q, u"99"_q);
		list.push_back(twin);
		auto bad = item;
		bad.insert(u"id"_q, u"100"_q);
		bad.insert(u"random"_q, u"512"_q);
		bad.insert(u"state"_q, 17);
		list.push_back(bad);
		auto empty = item;
		empty.insert(u"id"_q, u"101"_q);
		empty.insert(u"random"_q, u"513"_q);
		empty.insert(u"text"_q, u" "_q);
		list.push_back(empty);
		list.push_back(u"garbage"_q);
		object.insert(u"items"_q, list);
		const auto parsed = Queue::Parse(
			QJsonDocument(object).toJson(QJsonDocument::Compact),
			t0);
		check(parsed.items().size() == 1 && parsed.find(a),
			"broken: copies and bad records are dropped");
	}
	check.section("broken file");

	{
		check(FloodSeconds(u"FLOOD_WAIT_17"_q) == 17
			&& FloodSeconds(u"FLOOD_PREMIUM_WAIT_3"_q) == 3
			&& FloodSeconds(u"FLOOD_WAIT_0"_q) == 1
			&& !FloodSeconds(u"PEER_FLOOD"_q),
			"answers: flood wait");
		const auto flood = Judge(u"FLOOD_WAIT_17"_q, 420, 1, t0);
		check(!flood.sent
			&& !flood.permanent
			&& flood.retryAt == t0 + 17 + kFloodMargin,
			"answers: a flood wait is waited out");
		const auto internal = Judge(u"INTERNAL"_q, 500, 3, t0);
		check(!internal.permanent && internal.retryAt == t0 + 3 * kRetryStep,
			"answers: a server error is tried again, later each time");
		check(Judge(u"REQUEST_CANCELLED"_q, -400, 1, t0).retryAt > t0,
			"answers: a local error is tried again");
		check(Judge(u"USER_IS_BLOCKED"_q, 400, 1, t0).permanent
			&& Judge(u"PEER_FLOOD"_q, 400, 1, t0).permanent,
			"answers: a refusal is final");
		auto queue = Queue();
		queue.add(7, u"A"_q, text(u"one"_q), 611, t0, 24);
		queue.add(8, u"B"_q, text(u"two"_q), 612, t0, 24);
		queue.add(8, u"B"_q, text(u"three"_q), 613, t0, 24);
		check(queue.waitedPeers() == base::flat_set<uint64>{ 7, 8 },
			"waited: the persons");
		auto full = Queue();
		auto added = 0;
		for (auto i = 0; i != kMaxPerPeer + 5; ++i) {
			if (full.add(7, u"A"_q, text(u"x"_q), 700 + i, t0, 24)) {
				++added;
			}
		}
		check(added == kMaxPerPeer, "limit: messages for one person");
	}
	check.section("answers");

	{
		// A message that is being edited or whose removal is being
		// confirmed is held: it does not go, the ones behind it wait.
		auto queue = Queue();
		const auto a = queue.add(7, u"Anna"_q, text(u"first"_q), 811, t0, 24);
		const auto b = queue.add(7, u"Anna"_q, text(u"second"_q), 812, t0, 24);
		const auto c = queue.add(8, u"Boris"_q, text(u"other"_q), 813, t0, 24);
		check(!queue.begin(7, t0 + 1, true, { a })
			&& queue.find(a)->state == State::Waiting
			&& !queue.find(a)->attempts
			&& queue.find(b)->state == State::Waiting,
			"hold: the first in line is held, nothing goes for the person");
		const auto other = queue.begin(8, t0 + 1, true, { a });
		check(other && other->id == c, "hold: another person is not held");
		const auto first = queue.begin(7, t0 + 2, true, { b });
		check(first && first->id == a,
			"hold: the ones before the held one go");
		queue.sent(a);
		check(!queue.begin(7, t0 + 3, true, { b })
			&& queue.readyPeers(t0 + 3) == std::vector<uint64>{ 7 },
			"hold: the held one waits");
		check(queue.edit(b, text(u"second, corrected"_q)),
			"hold: edited meanwhile");
		const auto second = queue.begin(7, t0 + 4, true);
		check(second
			&& second->id == b
			&& second->randomId == 812
			&& second->text.text == u"second, corrected"_q,
			"hold: released, goes with the new text");

		auto forced = Queue();
		const auto f = forced.add(7, u"Anna"_q, text(u"now"_q), 821, t0, 24);
		const auto g = forced.add(7, u"Anna"_q, text(u"next"_q), 822, t0, 24);
		check(forced.resend(f)
			&& !forced.begin(7, t0 + 1, false, { f })
			&& !forced.begin(7, t0 + 1, true, { f })
			&& forced.find(f)->state == State::Waiting
			&& forced.find(f)->forced
			&& forced.find(g)->state == State::Waiting,
			"hold: «send now» is held too and keeps its place");
		check(forced.expire(t0 + 25 * hour) == std::vector<uint64>{ g }
			&& forced.find(f)->state == State::Waiting,
			"hold: the time limit goes on, a forced one has none");
		check(forced.remove(f) && !forced.begin(7, t0 + 26 * hour, true),
			"hold: removed while held, nothing goes");
	}
	check.section("held");

	{
		// The chat is deleted or the person blocked.
		auto queue = Queue();
		const auto a = queue.add(7, u"Anna"_q, text(u"first"_q), 911, t0, 24);
		const auto b = queue.add(7, u"Anna"_q, text(u"second"_q), 912, t0, 24);
		const auto c = queue.add(7, u"Anna"_q, text(u"third"_q), 913, t0, 1);
		const auto d = queue.add(7, u"Anna"_q, text(u"fourth"_q), 914, t0, 24);
		const auto e = queue.add(8, u"Boris"_q, text(u"other"_q), 915, t0, 24);
		check(queue.begin(7, t0 + 1, true)->id == a
			&& queue.resend(b)
			&& queue.expire(t0 + 2 * hour) == std::vector<uint64>{ c },
			"deleted chat: one on its way, one forced, one expired");
		queue.markAsked();
		check(queue.cancel(7, kErrorChatDeleted),
			"deleted chat: something was waiting");
		for (const auto id : { b, d }) {
			const auto item = queue.find(id);
			check(item->state == State::Failed
				&& item->error == kErrorChatDeleted
				&& !item->forced
				&& !item->asked,
				"deleted chat: a waiting one is not sent, with the reason");
		}
		check(queue.find(a)->state == State::Sending,
			"deleted chat: the one on its way is left to its answer");
		check(queue.find(c)->state == State::Expired && queue.find(c)->asked,
			"deleted chat: an expired one stays as it is");
		check(queue.find(e)->state == State::Waiting
			&& queue.waitedPeers() == base::flat_set<uint64>{ 8 },
			"deleted chat: only this person is not waited for any more");
		check(queue.unasked() == 2 && !queue.cancel(7, kErrorChatDeleted),
			"deleted chat: the user is told, once");
		queue.failed(a, u"FLOOD_WAIT_30"_q, false, t0 + 2 * hour + 40);
		check(queue.find(a)->state == State::Failed
			&& queue.find(a)->error == kErrorChatDeleted
			&& queue.find(a)->stop.isEmpty(),
			"deleted chat: a temporary error is not tried again");
		check(!queue.begin(7, t0 + 3 * hour, true)
			&& queue.readyPeers(t0 + 3 * hour) == std::vector<uint64>{ 8 },
			"deleted chat: nothing goes by itself");
		auto same = Queue::Parse(queue.serialize(), t0 + 3 * hour);
		check(same.serialize() == queue.serialize()
			&& !same.begin(7, t0 + 3 * hour, true),
			"deleted chat: stays so after a restart");
		check(queue.canEdit(b)
			&& queue.resend(b)
			&& queue.begin(7, t0 + 3 * hour, false)->id == b,
			"deleted chat: only a click sends it");

		auto blocked = Queue();
		const auto s = blocked.add(7, u"Anna"_q, text(u"one"_q), 921, t0, 24);
		check(blocked.begin(7, t0 + 1, true).has_value()
			&& blocked.cancel(7, kErrorBlocked)
			&& blocked.find(s)->state == State::Sending
			&& blocked.find(s)->stop == kErrorBlocked,
			"blocked: the one on its way is marked");
		auto restarted = Queue::Parse(blocked.serialize(), t0 + 5);
		check(restarted.find(s)->state == State::Unconfirmed
			&& restarted.find(s)->stop.isEmpty()
			&& !restarted.begin(7, t0 + 6, true)
			&& restarted.readyPeers(t0 + 6).empty(),
			"blocked: a restart does not send it again");
		blocked.failed(s, u"INTERNAL"_q, false, t0 + 60, true);
		check(blocked.find(s)->state == State::Failed
			&& blocked.find(s)->error == kErrorBlocked
			&& blocked.find(s)->unsure
			&& !blocked.canEdit(s),
			"blocked: no clear answer, not tried again");
	}
	check.section("deleted chat");

	{
		// An attempt with no clear answer: the message may be there.
		auto queue = Queue();
		const auto a = queue.add(7, u"Anna"_q, text(u"first"_q), 931, t0, 24);
		check(queue.begin(7, t0, true).has_value(), "unsure: first attempt");
		queue.failed(a, u"FLOOD_WAIT_5"_q, false, t0 + 10);
		check(queue.canEdit(a) && !queue.find(a)->unsure,
			"unsure: a flood wait is a clear «no», the text may change");
		check(queue.begin(7, t0 + 20, true).has_value(), "unsure: again");
		const auto lost = Judge(
			u"CLIENT_RESPONSE_PARSE_FAILED"_q,
			0,
			2,
			t0 + 20);
		check(!lost.sent
			&& !lost.permanent
			&& lost.unsure
			&& lost.retryAt == t0 + 20 + 2 * kRetryStep,
			"answers: an answer that could not be read is tried again");
		check(Judge(u"INTERNAL"_q, 500, 1, t0).unsure
			&& !Judge(u"FLOOD_WAIT_17"_q, 420, 1, t0).unsure
			&& !Judge(u"USER_IS_BLOCKED"_q, 400, 1, t0).unsure,
			"answers: which ones leave a doubt");
		queue.failed(
			a,
			u"INTERNAL"_q,
			lost.permanent,
			lost.retryAt,
			lost.unsure);
		check(queue.find(a)->state == State::Waiting
			&& queue.find(a)->unsure
			&& !queue.canEdit(a)
			&& !queue.edit(a, text(u"changed"_q))
			&& queue.find(a)->text.text == u"first"_q,
			"unsure: the text is not changed any more");
		const auto b = queue.add(7, u"Anna"_q, text(u"second"_q), 932, t0, 24);
		check(!queue.begin(7, lost.retryAt - 1, false)
			&& !queue.begin(7, lost.retryAt - 1, true)
			&& queue.find(b)->state == State::Waiting
			&& queue.readyPeers(lost.retryAt - 1) == std::vector<uint64>{ 7 },
			"unsure: repeated a bit later, the next one waits behind it");
		auto same = Queue::Parse(queue.serialize(), t0 + 30);
		check(same.serialize() == queue.serialize() && !same.canEdit(a),
			"unsure: remembered after a restart");
		check(same.expire(t0 + 21 + kResumeWindow) == std::vector<uint64>{ a }
			&& same.find(a)->state == State::Unconfirmed
			&& !same.begin(7, t0 + 22 + kResumeWindow, false),
			"unsure: no repeat by itself later than soon after the attempt");
		const auto next = same.begin(7, t0 + 23 + kResumeWindow, true);
		check(next && next->id == b,
			"unsure: the next one is not held by a not confirmed one");

		// The repeats do not wait for the person to come online again.
		auto at = lost.retryAt;
		for (auto i = 2; i != kMaxAttempts; ++i) {
			const auto again = queue.begin(7, at, false);
			check(again
				&& again->id == a
				&& again->randomId == 931
				&& again->text.text == u"first"_q,
				"unsure: the same message with the same random id");
			at += kRetryStep * (i + 1);
			queue.failed(a, u"INTERNAL"_q, false, at, true);
		}
		check(queue.find(a)->state == State::Unconfirmed
			&& queue.find(a)->attempts == kMaxAttempts
			&& queue.unasked() == 1,
			"unsure: given up as not confirmed, the user is asked");
		check(!queue.begin(7, at + 9000, false) && queue.remove(a),
			"unsure: never by itself after that, can be removed");

		auto clicked = Queue();
		const auto c = clicked.add(7, u"A"_q, text(u"one"_q), 941, t0, 24);
		check(clicked.begin(7, t0, true).has_value(), "unsure: attempt");
		clicked.failed(c, u"INTERNAL"_q, false, t0 + 60, true);
		check(clicked.resend(c)
			&& !clicked.find(c)->resumed
			&& clicked.find(c)->unsure
			&& clicked.begin(7, t0 + 1, false)->randomId == 941,
			"unsure: a click sends it at once, with the same random id");
		clicked.failed(c, u"FLOOD_WAIT_30"_q, false, t0 + 40);
		check(clicked.find(c)->state == State::Waiting
			&& clicked.find(c)->forced
			&& clicked.find(c)->resumed
			&& !clicked.begin(7, t0 + 39, true)
			&& clicked.begin(7, t0 + 40, false).has_value(),
			"unsure: a clear «no» later does not bring the long wait back");
		clicked.failed(c, u"FLOOD_WAIT_900"_q, false, t0 + 945);
		check(!clicked.begin(7, t0 + 945, true)
			&& clicked.find(c)->state == State::Unconfirmed
			&& clicked.unasked() == 1,
			"unsure: a wait that is too long ends with a question");
	}
	check.section("unsure");

	return !check.failed();
}

} // namespace Oblivion::SendOnline
