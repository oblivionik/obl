/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_ghost.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "data/data_channel.h"
#include "data/data_forum_topic.h"
#include "data/data_histories.h"
#include "data/data_peer.h"
#include "data/data_replies_list.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "oblivion/oblivion_sending.h"
#include "oblivion/oblivion_settings.h"
#include "settings.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

namespace Oblivion {
namespace {

constexpr auto kSaveReadTillsDelay = crl::time(1000);
constexpr auto kReadTills = "read_tills";
constexpr auto kUser = "user";
constexpr auto kPeer = "peer";
constexpr auto kTill = "till";

// (account user id, peer id) -> message id read invisibly till.
using ReadTillKey = std::pair<uint64, uint64>;

struct ReadTills {
	base::flat_map<ReadTillKey, int64> map;
	bool saveScheduled = false;
	bool saveOnQuit = false;
};

[[nodiscard]] QString ReadTillsPath() {
	return cWorkingDir() + u"tdata/oblivion_ghost.json"_q;
}

void LoadReadTills(ReadTills &tills) {
	auto file = QFile(ReadTillsPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	const auto list = document.object().value(
		QString::fromUtf8(kReadTills)).toArray();
	for (const auto &entry : list) {
		const auto object = entry.toObject();
		const auto user = object.value(
			QString::fromUtf8(kUser)).toString().toULongLong();
		const auto peer = object.value(
			QString::fromUtf8(kPeer)).toString().toULongLong();
		const auto till = int64(object.value(
			QString::fromUtf8(kTill)).toDouble());
		if (user && peer && till > 0) {
			tills.map[ReadTillKey(user, peer)] = till;
		}
	}
}

[[nodiscard]] ReadTills &LocalReadTills() {
	static auto result = [] {
		auto tills = ReadTills();
		LoadReadTills(tills);
		return tills;
	}();
	return result;
}

void SaveReadTills() {
	auto &tills = LocalReadTills();
	tills.saveScheduled = false;

	auto list = QJsonArray();
	for (const auto &[key, till] : tills.map) {
		auto object = QJsonObject();
		// Peer ids don't fit into double, so they are saved as strings.
		object.insert(
			QString::fromUtf8(kUser),
			QString::number(key.first));
		object.insert(
			QString::fromUtf8(kPeer),
			QString::number(key.second));
		object.insert(QString::fromUtf8(kTill), double(till));
		list.push_back(object);
	}
	auto object = QJsonObject();
	object.insert(QString::fromUtf8(kReadTills), list);

	// QSaveFile replaces the old file only after a complete write,
	// so a crash while saving can't leave a truncated file behind.
	auto file = QSaveFile(ReadTillsPath());
	if (!file.open(QIODevice::WriteOnly)) {
		LOG(("Oblivion Error: "
			"Could not open ghost read tills for writing."));
		return;
	}
	file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
	if (!file.commit()) {
		LOG(("Oblivion Error: Could not save ghost read tills: %1."
			).arg(file.errorString()));
	}
}

void ScheduleSaveReadTills() {
	auto &tills = LocalReadTills();
	if (!tills.saveOnQuit) {
		// The delayed save doesn't happen if the app quits before it.
		if (const auto app = QCoreApplication::instance()) {
			tills.saveOnQuit = true;
			QObject::connect(app, &QCoreApplication::aboutToQuit, [] {
				if (LocalReadTills().saveScheduled) {
					SaveReadTills();
				}
			});
		}
	}
	if (tills.saveScheduled) {
		return;
	}
	tills.saveScheduled = true;
	base::call_delayed(kSaveReadTillsDelay, [] { SaveReadTills(); });
}

[[nodiscard]] ReadTillKey ReadTillKeyFor(not_null<History*> history) {
	return { history->session().userId().bare, history->peer->id.value };
}

[[nodiscard]] bool RememberReadTillFor(not_null<History*> history) {
	// Forums and monoforums keep unread counts in topics / sublists,
	// a single read till for the whole chat is not enough there.
	return !history->isForum() && !history->amMonoforumAdmin();
}

void RememberReadTill(not_null<History*> history, MsgId tillId) {
	if (!RememberReadTillFor(history)) {
		return;
	}
	auto &till = LocalReadTills().map[ReadTillKeyFor(history)];
	if (till < tillId.bare) {
		till = tillId.bare;
		ScheduleSaveReadTills();
	}
}

[[nodiscard]] bool AlwaysReadNormally(not_null<PeerData*> peer) {
	return peer->isSelf()
		|| peer->isRepliesChat()
		|| peer->isVerifyCodes()
		|| peer->isServiceUser();
}

// How many loaded incoming messages from the first unread one till tillId.
// Unloaded messages are not counted, so the result is never too big.
[[nodiscard]] int CountLoadedUnreadTill(
		not_null<History*> history,
		MsgId tillId) {
	const auto from = history->inboxReadTillId() + 1;
	auto result = 0;
	for (const auto &block : history->blocks) {
		for (const auto &message : block->messages) {
			const auto item = message->data();
			if (!item->isRegular()
				|| (item->out() && !item->isFromScheduled())
				|| item->id < from) {
				continue;
			} else if (item->id > tillId) {
				return result;
			}
			++result;
		}
	}
	return result;
}

[[nodiscard]] std::optional<int> CountStillUnread(
		not_null<History*> history,
		MsgId tillId) {
	if (history->lastServerMessageKnown()) {
		const auto last = history->lastServerMessage();
		if (last && last->id <= tillId) {
			return 0;
		}
	}
	if (const auto result = history->countStillUnreadLocal(tillId)) {
		return result;
	} else if (!history->unreadCountKnown()) {
		return std::nullopt;
	}
	// The server would tell us the exact number, but we don't ask it.
	// Better to leave a slightly bigger badge than a too small one.
	return std::max(
		history->unreadCount() - CountLoadedUnreadTill(history, tillId),
		0);
}

// Local state is already read here, only the server lags behind.
void SendServerRead(
		not_null<History*> history,
		Data::ForumTopic *topic,
		MsgId tillId) {
	if (!IsServerMsgId(tillId)) {
		return;
	}

	// Don't repeat the same request for every message sent in a row.
	using SentKey = std::tuple<uint64, uint64, int64>;
	static auto sent = base::flat_map<SentKey, int64>();
	const auto peer = history->peer;
	auto &sentTill = sent[SentKey(
		history->session().userId().bare,
		peer->id.value,
		topic ? topic->rootId().bare : int64())];
	if (sentTill >= tillId.bare) {
		return;
	}
	sentTill = tillId.bare;

	const auto api = &history->session().api();
	if (topic) {
		api->request(MTPmessages_ReadDiscussion(
			peer->input(),
			MTP_int(topic->rootId()),
			MTP_int(tillId)
		)).send();
	} else if (const auto channel = peer->asChannel()) {
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

// The chat has just become read normally: tell the server what was
// already read here invisibly, like a usual read would do.
void SyncServerRead(not_null<PeerData*> peer) {
	const auto history = peer->owner().historyLoaded(peer);
	if (!history
		|| !RememberReadTillFor(history)
		|| !history->trackUnreadMessages()) {
		return;
	}
	const auto &map = LocalReadTills().map;
	const auto i = map.find(ReadTillKeyFor(history));
	if (i == end(map)) {
		// Nothing was read invisibly in this chat.
		return;
	}
	SendServerRead(
		history,
		nullptr,
		std::max(MsgId(i->second), history->inboxReadTillId()));
}

struct ReadOnSendTarget {
	not_null<History*> history;
	Data::ForumTopic *topic = nullptr;
};

[[nodiscard]] std::optional<ReadOnSendTarget> LookupReadOnSend(
		const Api::SendAction &action) {
	const auto history = action.history;
	const auto peer = history->peer;
	const auto scheduled = action.options.scheduled;
	if (!Get().readOnSend()
		|| (scheduled && !IsOfflineSchedule(action.options))
		|| action.options.shortcutId
		|| action.replaceMediaOf
		|| !GhostReadFor(peer)
		|| !history->trackUnreadMessages()) {
		return std::nullopt;
	} else if (peer->monoforumSublistFor(action.replyTo.monoforumPeerId)) {
		// Sublists are read by messages.readSavedHistory, not here.
		return std::nullopt;
	}
	const auto topicRootId = action.replyTo.topicRootId;
	const auto topic = topicRootId
		? peer->forumTopicFor(topicRootId)
		: nullptr;
	if (!topic && (history->isForum() || history->amMonoforumAdmin())) {
		// Unread counts live in topics / sublists there, a read request
		// for the whole chat would read all of them on the server.
		return std::nullopt;
	}
	return ReadOnSendTarget{ .history = history, .topic = topic };
}

void SendReadOnSend(const ReadOnSendTarget &target) {
	const auto history = target.history;
	const auto topic = target.topic;
	const auto lastServerId = [&] {
		const auto last = history->lastServerMessageKnown()
			? history->lastServerMessage()
			: nullptr;
		return last ? last->id : MsgId();
	};
	const auto tillId = topic
		? std::max(
			topic->replies()->computeInboxReadTillFull(),
			topic->lastKnownServerMessageId())
		: std::max(history->inboxReadTillId(), lastServerId());
	SendServerRead(history, topic, tillId);
}

} // namespace

bool GhostReadFor(not_null<PeerData*> peer) {
	return !AlwaysReadNormally(peer) && Get().ghostReadFor(peer->id.value);
}

bool CanToggleGhostRead(not_null<PeerData*> peer) {
	if (AlwaysReadNormally(peer)) {
		return false;
	} else if (const auto channel = peer->asChannel()) {
		return !channel->isCommunity();
	}
	return true;
}

bool ReadInboxLocally(not_null<History*> history, MsgId tillId) {
	if (!GhostReadFor(history->peer)) {
		return false;
	} else if (!IsServerMsgId(tillId)) {
		return true;
	}
	// Count before the read till is moved, the local count depends on it.
	const auto stillUnread = CountStillUnread(history, tillId);

	// Moving the read till forward also makes History ignore the older
	// server read state from dialog entries requested later, so the badge
	// won't come back while the app is running.
	history->setInboxReadTill(tillId);
	if (stillUnread && history->folderKnown()) {
		history->setUnreadCount(*stillUnread);
	}
	history->validateMonoAndForumUnread(tillId);
	history->updateChatListEntry();
	RememberReadTill(history, tillId);
	return true;
}

void ApplyLocalReadTill(
		not_null<History*> history,
		MsgId serverReadTill,
		MsgId topMessageId) {
	if (!RememberReadTillFor(history)) {
		return;
	}
	auto &tills = LocalReadTills();
	const auto i = tills.map.find(ReadTillKeyFor(history));
	if (i == end(tills.map)) {
		return;
	}
	const auto till = MsgId(i->second);
	if (serverReadTill >= till) {
		// The chat was read normally since then, forget it.
		tills.map.erase(i);
		ScheduleSaveReadTills();
		return;
	} else if (!GhostReadFor(history->peer) || !history->folderKnown()) {
		return;
	}
	history->setInboxReadTill(till);
	if (topMessageId <= till) {
		history->setUnreadCount(0);
	}
	// Otherwise we don't know how many of the newer messages are unread,
	// the server count is bigger, but at least the unread bar is right.
}

void ReadOnSend(const Api::SendAction &action) {
	if (const auto target = LookupReadOnSend(action)) {
		SendReadOnSend(*target);
	}
}

void ReadOnSendFor(
		not_null<Data::Thread*> thread,
		const Api::SendOptions &options) {
	auto action = Api::SendAction(thread, options);
	action.replyTo.monoforumPeerId = thread->monoforumPeerId();
	const auto target = LookupReadOnSend(action);
	if (!target) {
		return;
	} else if (const auto topic = target->topic) {
		if (topic->creating()) {
			return;
		}
		topic->readTillEnd();
	} else {
		const auto history = target->history;
		history->owner().histories().readInbox(history);
	}
	SendReadOnSend(*target);
}

bool HideContentsRead(not_null<HistoryItem*> item) {
	return item->isIncomingUnreadMedia()
		&& GhostReadFor(item->history()->peer);
}

bool GhostStories() {
	return Get().ghostStories();
}

void AddGhostReadAction(
		const Ui::Menu::MenuCallback &addAction,
		not_null<Window::SessionController*> controller,
		PeerData *peer) {
	if (!peer || !CanToggleGhostRead(peer)) {
		return;
	}
	const auto peerId = peer->id.value;
	const auto weak = base::make_weak(controller);
	const auto invisible = Get().ghostReadFor(peerId);
	addAction(
		(invisible
			? tr::lng_oblivion_menu_ghost_read_off(tr::now)
			: tr::lng_oblivion_menu_ghost_read_on(tr::now)),
		[=] {
			auto &settings = Get();
			settings.setGhostReadException(
				peerId,
				!settings.isGhostReadException(peerId));
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			const auto nowInvisible = settings.ghostReadFor(peerId);
			if (!nowInvisible) {
				SyncServerRead(peer);
			}
			strong->showToast(nowInvisible
				? tr::lng_oblivion_ghost_read_on_toast(tr::now)
				: tr::lng_oblivion_ghost_read_off_toast(tr::now));
		},
		&st::menuIconStealth);
}

} // namespace Oblivion
