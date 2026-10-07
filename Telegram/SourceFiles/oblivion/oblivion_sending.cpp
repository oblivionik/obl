/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_sending.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/unixtime.h"
#include "data/components/ephemeral_messages.h"
#include "data/components/scheduled_messages.h"
#include "data/data_channel.h"
#include "data/data_histories.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "main/main_session.h"
#include "oblivion/oblivion_settings.h"
#include "storage/localimageloader.h"

namespace Oblivion {
namespace {

// A scheduled message is sent by the server itself and doesn't make the
// sender appear online. Dates closer than ~10 seconds are treated as
// "send right now" (the schedule box and TDLib use the same 10 second
// minimum), so keep a small margin for clock drift and request travel.
constexpr auto kOfflineSendDelay = TimeId(12);

// How late after the planned date a delivered message is still
// recognized as ours: server lag, a long queue of messages.
constexpr auto kDeliverySlack = TimeId(60);

// Messages that came this little before the send could still be on
// the way at that moment, so they may be unread on the server.
constexpr auto kReadOnSendMargin = TimeId(3);

// Delivery windows are forgotten after that.
constexpr auto kForgetAfter = TimeId(3600);

// Enough to remember the last deliveries until their notifications
// are processed, those are checked right when the message arrives.
constexpr auto kDeliveredLimit = 64;

// The server keeps at most that many scheduled messages in a chat and
// fails a send that doesn't fit with SCHEDULE_TOO_MUCH.
constexpr auto kScheduledLimit = 100;

using ChatKey = std::pair<uint64, PeerId>; // (session unique id, peer id)

struct DeliveryWindow {
	TimeId from = 0; // When the first message was sent, 0 if unknown.
	TimeId till = 0; // The latest offline date in the window.
};

struct Delivered {
	uint64 session = 0;
	FullMsgId itemId;
};

// Offline-sent messages that the server may not have told about yet.
struct Pending {
	ChatKey key;
	TimeId date = 0;
	int count = 0;
	int known = 0; // The local scheduled count right before the send.
};

// A delivered offline-sent message that may be counted as unread.
struct Unread {
	FullMsgId itemId;
	TimeId sentAt = 0;
};

struct State {
	// When offline-sent messages are expected to arrive in a chat.
	base::flat_map<ChatKey, DeliveryWindow> windows;

	// Offline-sent messages that were delivered recently.
	std::vector<Delivered> delivered;

	// Delivered messages to read away together, once per chat.
	base::flat_map<ChatKey, std::vector<Unread>> unread;

	// Every offline-sent request (see AdjustSendAction). Some create no
	// local messages at all, the scheduled messages list learns about
	// them only from the server. Kept until their offline date passes,
	// in the order they were sent.
	std::vector<Pending> pending;

	// How far a chat was read after an offline-sent message arrived.
	base::flat_map<ChatKey, MsgId> readTill;

	// While a send decided already to go as is (see SendShareComment).
	int keepOnline = 0;

	// Sessions where new messages are watched for deliveries.
	base::flat_set<uint64> tracked;
};

[[nodiscard]] State &Current() {
	static auto result = State();
	return result;
}

[[nodiscard]] ChatKey KeyFor(not_null<PeerData*> peer) {
	return { peer->session().uniqueId(), peer->id };
}

// How many messages wait on the server among the scheduled ones.
// known is how many the local list has now, it learns about the pending
// ones only from the server, some of them may be there already. Each
// pending send went on top of the list it saw, so the server has at
// least that list and everything sent since then, count them only once.
[[nodiscard]] int WaitingCount(const ChatKey &key, int known) {
	auto &list = Current().pending;
	const auto now = base::unixtime::now();
	list.erase(ranges::remove_if(list, [&](const Pending &entry) {
		return (entry.date < now);
	}), end(list));
	auto result = known;
	auto sentSince = 0;
	for (auto i = list.rbegin(); i != list.rend(); ++i) {
		if (i->key == key) {
			sentSince += i->count;
			result = std::max(result, i->known + sentSince);
		}
	}
	return result;
}

// Marks the chat as read on the server, bypassing the ghost filter.
void ForceReadTill(not_null<History*> history, MsgId tillId) {
	const auto peer = history->peer;
	auto &readTill = Current().readTill[KeyFor(peer)];
	if (!IsServerMsgId(tillId) || readTill >= tillId) {
		return;
	}
	readTill = tillId;

	const auto api = &history->session().api();
	if (const auto channel = peer->asChannel()) {
		api->request(MTPchannels_ReadHistory(
			channel->inputChannel(),
			MTP_int(tillId)
		)).send();
	} else {
		api->request(MTPmessages_ReadHistory(
			peer->input(),
			MTP_int(tillId)
		)).done([=](const MTPmessages_AffectedMessages &result) {
			api->applyAffectedMessages(peer, result);
		}).send();
	}
}

// Whether someone else wrote in the chat between the moment the user
// read it (by id or by date) and tillId, judging by the loaded messages.
// Unknown is treated as "yes".
[[nodiscard]] bool MaybeIncomingBefore(
		not_null<HistoryItem*> till,
		MsgId readTillId,
		TimeId readAt) {
	const auto history = till->history();
	if (!till->mainView() || !history->loadedAtBottom()) {
		return true;
	}
	for (auto b = history->blocks.rbegin(); b != history->blocks.rend(); ++b) {
		const auto &messages = (*b)->messages;
		for (auto m = messages.rbegin(); m != messages.rend(); ++m) {
			const auto item = (*m)->data();
			if (!item->isRegular() || item->id >= till->id) {
				continue;
			} else if ((readTillId && item->id <= readTillId)
				|| (readAt && item->date() < readAt)) {
				return false;
			} else if (!item->out()) {
				return true;
			}
		}
	}
	return !history->loadedAtTop();
}

// A delivered scheduled message counts as unread for its sender, like
// a reminder. Read it, if nothing else gets read by the way.
// Returns whether the chat was read till this message.
bool ReadDelivered(not_null<HistoryItem*> item, TimeId sentAt) {
	const auto history = item->history();
	const auto peer = history->peer;
	if (item->topic()
		|| item->savedSublist()
		|| !history->trackUnreadMessages()) {
		// Forum topics keep their own unread state, leave them as is.
		return false;
	} else if (!Get().ghostReadFor(peer->id.value)) {
		// The chat is read normally, so the local read state is exact.
		const auto readTillId = history->inboxReadTillId();
		if (!MaybeIncomingBefore(item, readTillId, TimeId())) {
			history->owner().histories().readInboxTill(item);
			return true;
		}
	} else if (Get().readOnSend() && sentAt) {
		// Read invisibly, but the chat was read on the server when this
		// message was sent (see ReadOnSend() in oblivion_ghost.h).
		const auto readAt = sentAt - kReadOnSendMargin;
		if (!MaybeIncomingBefore(item, MsgId(), readAt)) {
			// Locally (the ghost filter keeps it on this device)...
			history->owner().histories().readInboxTill(item);
			// ...and on the server.
			ForceReadTill(history, item->id);
			return true;
		}
	}
	return false;
}

// Reads away all the messages delivered to the chat together (an album,
// a long text, a forward of many messages) with a single read request.
void ClearDeliveredUnread(not_null<Main::Session*> session, ChatKey key) {
	auto &map = Current().unread;
	const auto i = map.find(key);
	if (i == end(map)) {
		return;
	}
	auto list = std::move(i->second);
	map.erase(i);

	// Reading till a message reads everything before it as well, so find
	// the newest one that can be read: if someone wrote in the chat before
	// it, an older one still may be.
	ranges::sort(list, ranges::greater(), [](const Unread &entry) {
		return entry.itemId.msg;
	});
	for (const auto &entry : list) {
		if (const auto item = session->data().message(entry.itemId)) {
			if (ReadDelivered(item, entry.sentAt)) {
				return;
			}
		}
	}
}

void CheckDelivered(not_null<HistoryItem*> item) {
	if (!item->out() || !item->isFromScheduled() || !item->isRegular()) {
		return;
	}
	const auto history = item->history();
	const auto session = &history->session();
	const auto key = KeyFor(history->peer);
	auto &state = Current();
	const auto i = state.windows.find(key);
	if (i == end(state.windows)) {
		return;
	}
	const auto window = i->second;
	const auto date = item->date();
	if (date < window.from || date > window.till + kDeliverySlack) {
		return;
	}
	const auto itemId = item->fullId();
	auto &list = state.delivered;
	if (int(list.size()) >= kDeliveredLimit) {
		list.erase(begin(list));
	}
	list.push_back({ .session = session->uniqueId(), .itemId = itemId });

	// History adds this message to the unread counter right after
	// notifying about it, so fix the counter a bit later, once for all
	// the messages that arrive in this chat together.
	auto &unread = state.unread[key];
	unread.push_back({ .itemId = itemId, .sentAt = window.from });
	if (unread.size() == 1) {
		crl::on_main(session, [=] {
			ClearDeliveredUnread(session, key);
		});
	}
}

void TrackDeliveries(not_null<Main::Session*> session) {
	const auto sessionId = session->uniqueId();
	if (!Current().tracked.emplace(sessionId).second) {
		return;
	}
	session->data().newItemAdded(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		CheckDelivered(item);
	}, session->lifetime());

	session->lifetime().add([=] {
		auto &state = Current();
		state.tracked.remove(sessionId);
		const auto removeFrom = [&](auto &map) {
			for (auto i = begin(map); i != end(map);) {
				if (i->first.first == sessionId) {
					i = map.erase(i);
				} else {
					++i;
				}
			}
		};
		removeFrom(state.windows);
		removeFrom(state.unread);
		removeFrom(state.readTill);
		state.delivered.erase(ranges::remove(
			state.delivered,
			sessionId,
			&Delivered::session
		), end(state.delivered));
		const auto fromSession = [&](const Pending &entry) {
			return (entry.key.first == sessionId);
		};
		state.pending.erase(
			ranges::remove_if(state.pending, fromSession),
			end(state.pending));
	});
}

// sentAt is when the user has sent the message, 0 if it's not known
// (the date is moved after an upload).
//
// Round 5, «отправить, когда будет в сети» relies on two things here, see
// SendOnline Account::send() in oblivion_send_online.cpp before changing
// either of them: with sentAt == 0 a window that exists is never replaced
// by a new one, and a window whose till is still ahead keeps its from.
// RefreshSendOptions() below always comes here with sentAt == 0.
void ExpectDelivery(not_null<PeerData*> peer, TimeId date, TimeId sentAt) {
	TrackDeliveries(&peer->session());

	auto &windows = Current().windows;
	const auto now = base::unixtime::now();
	for (auto i = begin(windows); i != end(windows);) {
		if (i->second.till + kForgetAfter < now) {
			i = windows.erase(i);
		} else {
			++i;
		}
	}
	const auto key = KeyFor(peer);
	const auto i = windows.find(key);
	if (i == end(windows)) {
		windows.emplace(key, DeliveryWindow{ .from = sentAt, .till = date });
	} else if (sentAt && (i->second.till + kDeliverySlack < sentAt)) {
		// Everything expected before is delivered, start a new window.
		i->second = DeliveryWindow{ .from = sentAt, .till = date };
	} else {
		i->second.till = std::max(i->second.till, date);
	}
}

[[nodiscard]] bool CanSendOffline(
		const Api::SendAction &action,
		const QString &text,
		int count) {
	const auto &options = action.options;
	if (!Get().offlineSend()
		|| options.scheduled // Also "send when online".
		|| options.scheduleRepeatPeriod
		|| options.shortcutId // Business quick replies.
		|| options.suggest // Suggested posts have their own dates.
		|| !options.stakeSeedHash.isEmpty()
		|| options.ttlSeconds // View once media.
		|| action.replaceMediaOf) {
		return false;
	}
	const auto history = action.history;
	const auto peer = history->peer;
	if (peer->isSelf() // Would become a reminder with a notification.
		|| peer->isMonoforum()
		|| peer->starsPerMessageChecked() > 0) { // Can't be scheduled.
		return false;
	}
	const auto &replyTo = action.replyTo;
	if (replyTo.storyId || replyTo.monoforumPeerId) {
		// Story replies and channel direct messages can't be scheduled.
		return false;
	} else if (replyTo.topicRootId
		&& (!peer->isForum()
			|| peer->isBot()
			|| !IsServerMsgId(replyTo.topicRootId))) {
		// Comments can't be scheduled, a new topic isn't created yet,
		// bot topics are created on the fly.
		return false;
	}
	// Offline-sent messages wait on the server among the scheduled ones,
	// the local list has the loaded ones and our local messages.
	const auto scheduled = history->session().scheduledMessages().count(
		history);
	if (WaitingCount(KeyFor(peer), scheduled) + count > kScheduledLimit) {
		return false;
	}
	// Bot commands in groups that are answered with ephemeral messages.
	return !history->session().ephemeralMessages().wouldSendMedia(
		peer,
		replyTo,
		text);
}

// Moves an offline date forward, if it got too close or passed already.
void Refresh(
		Api::SendOptions &options,
		not_null<PeerData*> peer,
		TimeId sentAt) {
	const auto min = base::unixtime::now() + kOfflineSendDelay;
	options.scheduled = std::max(options.scheduled, min);
	ExpectDelivery(peer, options.scheduled, sentAt);
}

} // namespace

void AdjustSendAction(
		Api::SendAction &action,
		const QString &text,
		int count) {
	const auto peer = action.history->peer;
	const auto now = base::unixtime::now();
	if (action.options.oblivionOffline) {
		// Comes from another offline send: forwards sent together with
		// a message, copies of protected messages sent a bit later.
		// Those were counted when that send went offline.
		Refresh(action.options, peer, now);
	} else if (!Current().keepOnline && CanSendOffline(action, text, count)) {
		const auto date = now + kOfflineSendDelay;
		const auto history = action.history;
		// Every offline decision is counted until the server surely has
		// it: some sends create no local messages at all (share box,
		// forwards of several messages), others create them a bit later.
		Current().pending.push_back({
			.key = KeyFor(peer),
			.date = date,
			.count = count,
			.known = history->session().scheduledMessages().count(history),
		});
		ExpectDelivery(peer, date, now);
		action.options.scheduled = date;
		action.options.oblivionOffline = true;
	}
}

void AdjustConfirmedFile(
		Api::SendAction &action,
		const std::shared_ptr<FilePrepareResult> &file) {
	const auto album = file->album.get();
	if (album && album->options.oblivionOffline) {
		// Another file of this album goes offline, keep them together.
		action.options.scheduled = album->options.scheduled;
		action.options.oblivionOffline = true;
		Refresh(action.options, action.history->peer, TimeId());
		album->options.scheduled = action.options.scheduled;
	} else {
		// The whole album goes in one request, count all of its files.
		const auto count = album ? int(album->items.size()) : 1;
		AdjustSendAction(action, file->caption.text, count);
		if (album && action.options.oblivionOffline) {
			album->options.scheduled = action.options.scheduled;
			album->options.oblivionOffline = true;
		}
	}
	file->to.options.scheduled = action.options.scheduled;
	file->to.options.oblivionOffline = action.options.oblivionOffline;
}

void RefreshSendOptions(
		Api::SendOptions &options,
		not_null<PeerData*> peer) {
	if (options.oblivionOffline) {
		Refresh(options, peer, TimeId());
	}
}

bool IsOfflineSchedule(const Api::SendOptions &options) {
	return options.oblivionOffline;
}

Api::SendOptions OfflineShareOptions(
		not_null<Data::Thread*> thread,
		const Api::SendOptions &options,
		const QString &comment,
		int count) {
	auto action = Api::SendAction(thread, options);
	const auto history = action.history;
	if (!comment.isEmpty()) {
		// Same as ApiWrap::sendMessage() counts the comment.
		const auto &draft = history->forwardDraft(
			action.replyTo.topicRootId,
			action.replyTo.monoforumPeerId);
		count += 1 + int(draft.ids.size());
	}
	// The request creates no local messages, AdjustSendAction() counts
	// everything (comment included) as pending while it may wait on
	// the server. The comment's local message is counted only once.
	AdjustSendAction(action, comment, count);
	return action.options;
}

void SendShareComment(
		not_null<ApiWrap*> api,
		Api::MessageToSend &&message) {
	if (IsOfflineSchedule(message.action.options)) {
		api->sendMessage(std::move(message));
		return;
	}
	// The whole send goes as is. The comment alone (with the forward
	// draft it takes along) may still fit the scheduled limit, but then
	// it would be delivered after the request that follows it.
	auto &keepOnline = Current().keepOnline;
	++keepOnline;
	api->sendMessage(std::move(message));
	--keepOnline;
}

TimeId OfflineScheduleFor(
		not_null<Data::Thread*> thread,
		const Api::SendOptions &options) {
	if (!options.oblivionOffline) {
		return TimeId();
	}
	auto copy = options;
	Refresh(copy, thread->peer(), TimeId());
	return copy.scheduled;
}

bool IsOfflineSentDelivery(not_null<const HistoryItem*> item) {
	const auto &list = Current().delivered;
	if (list.empty() || !item->out()) {
		return false;
	}
	const auto sessionId = item->history()->session().uniqueId();
	const auto itemId = item->fullId();
	return ranges::any_of(list, [&](const Delivered &entry) {
		return (entry.session == sessionId) && (entry.itemId == itemId);
	});
}

} // namespace Oblivion
