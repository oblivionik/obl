/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_room_extras.h"

#include "api/api_chat_invite.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/event_filter.h"
#include "base/invoke_queued.h"
#include "base/random.h"
#include "base/timer.h"
#include "base/unique_qptr.h"
#include "calls/calls_instance.h"
#include "chat_helpers/compose/compose_show.h"
#include "chat_helpers/stickers_lottie.h"
#include "chat_helpers/tabbed_panel.h"
#include "chat_helpers/tabbed_selector.h"
#include "core/application.h"
#include "core/click_handler_types.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_folder.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/stickers/data_custom_emoji.h"
#include "data/stickers/data_stickers.h"
#include "data/stickers/data_stickers_set.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "history/view/media/history_view_sticker_player.h"
#include "lang/lang_keys.h"
#include "lottie/lottie_single_player.h"
#include "main/main_session.h"
#include "menu/menu_send_details.h"
#include "oblivion/oblivion_lang.h"
#include "oblivion/oblivion_room_canvas.h"
#include "oblivion/oblivion_room_window.h"
#include "oblivion/oblivion_settings.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/abstract_button.h"
#include "ui/basic_click_handlers.h"
#include "ui/boxes/confirm_box.h"
#include "ui/effects/animations.h"
#include "ui/emoji_config.h"
#include "ui/empty_userpic.h"
#include "ui/layers/box_content.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/layer_widget.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/ui_utility.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"
#include "styles/style_window.h"

#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>

#include <deque>

namespace Oblivion::Rooms {
namespace {

constexpr auto kParticleDuration = crl::time(2800);
constexpr auto kParticlesLimit = 48;
constexpr auto kMomentDuration = crl::time(2600);
constexpr auto kMomentAfterReady = crl::time(2200);
constexpr auto kMomentMax = crl::time(7000);
constexpr auto kMomentAppear = crl::time(200);
constexpr auto kMomentDisappear = crl::time(220);
constexpr auto kMomentQueueLimit = 3;
constexpr auto kOwnEchoTimeout = crl::time(10'000);
constexpr auto kOwnEchoLimit = 64;
constexpr auto kStickerCooldown = crl::time(700);
constexpr auto kGroupsLimit = 200;
constexpr auto kTitleLimit = 64;
constexpr auto kMaxChatId = (uint64(1) << 47);
constexpr auto kPanelDuration = crl::time(140);

// The server allows 15 at once and 5 a second for a device (reactions and
// stickers together), the client stays well inside of it.
constexpr auto kOutgoingBurst = 8.;
constexpr auto kOutgoingPerSecond = 3.;
// Documents the app does not know yet are asked from Telegram: a member
// who throws a new sticker every second must not turn into a flood of
// requests of this account.
constexpr auto kResolveBurst = 8.;
constexpr auto kResolvePerSecond = 0.2;

const char *const kQuickEmoji[] = {
	"👍",
	"❤️",
	"😂",
	"🔥",
	"👏",
	"😮",
	"😢",
	"🎉",
};

[[nodiscard]] int Scaled(int value) {
	return style::ConvertScale(value);
}

[[nodiscard]] const style::font &SmallFont() {
	static const auto result = style::font(
		Scaled(11),
		st::semiboldFont->flags(),
		st::semiboldFont->family());
	return result;
}

[[nodiscard]] double EaseOutBack(double value) {
	const auto c1 = 1.70158;
	const auto c3 = c1 + 1.;
	const auto shifted = std::clamp(value, 0., 1.) - 1.;
	return 1. + c3 * shifted * shifted * shifted + c1 * shifted * shifted;
}

[[nodiscard]] bool FeatureOff(not_null<Room*> room, const char *name) {
	const auto account = room->account();
	return account && account->hello().valid && !account->feature(name);
}

void Toast(const std::shared_ptr<Ui::Show> &show, const QString &text) {
	if (show && show->valid() && !text.isEmpty()) {
		show->showToast(text);
	}
}

[[nodiscard]] bool OnlyOf(const QString &text, bool (*good)(ushort)) {
	for (const auto ch : text) {
		if (!good(ch.unicode())) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool NameChar(ushort code) {
	return (code >= 'a' && code <= 'z')
		|| (code >= 'A' && code <= 'Z')
		|| (code >= '0' && code <= '9')
		|| (code == '_');
}

[[nodiscard]] bool HashChar(ushort code) {
	return NameChar(code) || (code == '-');
}

[[nodiscard]] bool DigitChar(ushort code) {
	return (code >= '0' && code <= '9');
}

[[nodiscard]] uint64 ParseDigits(const QJsonValue &value) {
	const auto text = value.toString();
	if (text.isEmpty() || text.size() > 20 || !OnlyOf(text, DigitChar)) {
		return 0;
	}
	auto ok = false;
	const auto result = text.toULongLong(&ok);
	return ok ? uint64(result) : 0;
}

[[nodiscard]] uint64 ParseSignedDigits(const QJsonValue &value) {
	const auto text = value.toString();
	const auto digits = text.startsWith(QChar('-')) ? text.mid(1) : text;
	if (digits.isEmpty() || digits.size() > 20 || !OnlyOf(digits, DigitChar)) {
		return 0;
	}
	auto ok = false;
	const auto result = text.toLongLong(&ok);
	return ok ? uint64(int64(result)) : 0;
}

// The picture of an emoji in its large size, device pixels.
[[nodiscard]] QImage EmojiImage(EmojiPtr emoji) {
	// Never destroyed: nothing of Qt is touched at the exit.
	static const auto Cache = new base::flat_map<EmojiPtr, QImage>();
	static auto CacheSet = -1;
	if (const auto set = Ui::Emoji::CurrentSetId(); CacheSet != set) {
		CacheSet = set;
		Cache->clear();
	}
	const auto i = Cache->find(emoji);
	if (i != end(*Cache)) {
		return i->second;
	}
	const auto size = Ui::Emoji::GetSizeLarge();
	if (size <= 0 || !emoji) {
		return QImage();
	}
	auto result = QImage(size, size, QImage::Format_ARGB32_Premultiplied);
	if (result.isNull()) {
		return result;
	}
	result.fill(Qt::transparent);
	result.setDevicePixelRatio(style::DevicePixelRatio());
	{
		auto p = QPainter(&result);
		Ui::Emoji::Draw(p, emoji, size, 0, 0);
	}
	result.setDevicePixelRatio(1.);
	if (Cache->size() > 256) {
		Cache->clear();
	}
	Cache->emplace(emoji, result);
	return result;
}

void PaintEmoji(QPainter &p, EmojiPtr emoji, QRectF rect) {
	const auto image = EmojiImage(emoji);
	if (!image.isNull()) {
		p.setRenderHint(QPainter::SmoothPixmapTransform);
		p.drawImage(rect, image);
	}
}

void PaintNameTag(
		QPainter &p,
		QPointF centerTop,
		const QString &name,
		uint64 userId,
		double opacity) {
	if (name.isEmpty() || opacity <= 0.) {
		return;
	}
	const auto &font = SmallFont();
	const auto text = font->elided(name, Scaled(110));
	const auto width = font->width(text) + Scaled(12);
	const auto height = font->height + Scaled(4);
	const auto rect = QRectF(
		centerTop.x() - width / 2.,
		centerTop.y(),
		width,
		height);
	const auto color = Ui::EmptyUserpic::UserpicColor(
		Ui::EmptyUserpic::ColorIndex(userId));
	const auto was = p.opacity();
	p.setOpacity(was * opacity);
	p.setPen(Qt::NoPen);
	p.setBrush(color.color2);
	p.drawRoundedRect(rect, height / 2., height / 2.);
	p.setFont(font);
	p.setPen(QColor(255, 255, 255));
	p.drawText(rect, Qt::AlignCenter, text);
	p.setOpacity(was);
}

[[nodiscard]] QString VoiceGroupTitle(const QString &roomTitle) {
	const auto title = roomTitle.trimmed().isEmpty()
		? tr::lng_oblivion_room_title_default(tr::now)
		: roomTitle.trimmed();
	return tr::lng_oblivion_rextra_voice_group_title(
		tr::now,
		lt_title,
		title).left(kTitleLimit);
}

[[nodiscard]] bool CallActive(not_null<PeerData*> peer) {
	if (const auto channel = peer->asChannel()) {
		return (channel->flags() & ChannelDataFlag::CallActive);
	} else if (const auto chat = peer->asChat()) {
		return (chat->flags() & ChatDataFlag::CallActive);
	}
	return false;
}

// The group of the voice chat if this account knows it and is in it.
[[nodiscard]] PeerData *VoicePeer(
		not_null<Main::Session*> session,
		const QString &chatId) {
	if (chatId.isEmpty() || chatId.size() > 20 || !OnlyOf(chatId, DigitChar)) {
		return nullptr;
	}
	auto ok = false;
	const auto id = uint64(chatId.toULongLong(&ok));
	if (!ok || !id || id >= kMaxChatId) {
		return nullptr;
	}
	const auto channel = session->data().channelLoaded(ChannelId(id));
	if (channel && channel->isMegagroup() && channel->amIn()) {
		return channel;
	}
	const auto chat = session->data().chatLoaded(ChatId(id));
	if (chat && chat->amIn()) {
		return chat;
	}
	return nullptr;
}

[[nodiscard]] bool GroupFits(not_null<PeerData*> peer) {
	if (const auto chat = peer->asChat()) {
		return chat->amIn() && chat->canHaveInviteLink();
	} else if (const auto channel = peer->asChannel()) {
		return channel->isMegagroup()
			&& channel->amIn()
			&& (channel->hasUsername() || channel->canHaveInviteLink());
	}
	return false;
}

[[nodiscard]] QString GroupBareId(not_null<PeerData*> peer) {
	return peer->isChannel()
		? QString::number(quint64(peerToChannel(peer->id).bare))
		: peer->isChat()
		? QString::number(quint64(peerToChat(peer->id).bare))
		: QString();
}

[[nodiscard]] ChannelData *CreatedChannel(
		not_null<Main::Session*> session,
		const MTPUpdates &result) {
	const auto chats = [&]() -> const QVector<MTPChat>* {
		switch (result.type()) {
		case mtpc_updates:
			return &result.c_updates().vchats().v;
		case mtpc_updatesCombined:
			return &result.c_updatesCombined().vchats().v;
		}
		return nullptr;
	}();
	if (!chats
		|| chats->isEmpty()
		|| chats->front().type() != mtpc_channel) {
		return nullptr;
	}
	return session->data().channel(chats->front().c_channel().vid());
}

[[nodiscard]] RoomSticker StickerOf(not_null<DocumentData*> document) {
	auto result = RoomSticker();
	const auto info = document->sticker();
	if (!info) {
		return result;
	}
	result.customEmoji = (info->setType == Data::StickersType::Emoji);
	result.documentId = document->id;
	result.setId = info->set.id;
	result.setAccessHash = info->set.accessHash;
	if (!info->set.shortName.isEmpty()
		&& info->set.shortName.size() <= 64
		&& OnlyOf(info->set.shortName, NameChar)) {
		result.setShortName = info->set.shortName;
	}
	auto alt = QJsonValue(info->alt);
	result.emoji = CleanReactionEmoji(alt);
	result.format = info->isLottie()
		? u"animated"_q
		: info->isWebm()
		? u"video"_q
		: u"static"_q;
	return result;
}

// ---- What the widgets of one room window share.

class Extras final : public base::has_weak_ptr {
public:
	struct Reaction {
		uint64 userId = 0;
		QString name;
		EmojiPtr emoji = nullptr;
		bool own = false;
	};
	struct Thrown {
		uint64 userId = 0;
		QString name;
		RoomSticker sticker;
		bool own = false;
	};

	explicit Extras(not_null<Room*> room);

	[[nodiscard]] not_null<Room*> room() const {
		return _room;
	}
	// The user may send reactions and stickers now.
	[[nodiscard]] bool available() const;
	[[nodiscard]] rpl::producer<Reaction> reactions() const {
		return _reactions.events();
	}
	[[nodiscard]] rpl::producer<Thrown> stickers() const {
		return _stickers.events();
	}
	// The account is going away: everything of the session is dropped.
	[[nodiscard]] rpl::producer<> sessionGone() const {
		return _sessionGone.events();
	}
	// What the buttons show may have changed.
	[[nodiscard]] rpl::producer<> refreshes() const {
		return _refreshes.events();
	}

	void react(EmojiPtr emoji);
	void throwSticker(const RoomSticker &sticker);
	void throwDocument(not_null<DocumentData*> document);
	// nullptr: show the emoji of the sticker instead.
	void resolve(const RoomSticker &sticker, Fn<void(DocumentData*)> done);

	void openVoice(std::shared_ptr<Ui::Show> show);
	void createVoice(std::shared_ptr<Ui::Show> show);
	void attachVoice(std::shared_ptr<Ui::Show> show);
	void detachVoice(std::shared_ptr<Ui::Show> show);

	// For the snapshot scenes.
	void showSample(Reaction &&reaction) {
		_reactions.fire(std::move(reaction));
	}

private:
	struct Echo {
		QString key;
		crl::time when = 0;
	};

	void handle(const Cloud::Event &event);
	void detached();
	void expectEcho(const QString &key);
	[[nodiscard]] bool takeEcho(const QString &key);

	void startCreate(std::shared_ptr<Ui::Show> show);
	void exportLink(
		not_null<PeerData*> peer,
		std::shared_ptr<Ui::Show> show,
		bool startCall);
	void saveVoice(
		not_null<PeerData*> peer,
		const QString &link,
		std::shared_ptr<Ui::Show> show,
		bool startCall);
	void attachChosen(uint64 peerId, std::shared_ptr<Ui::Show> show);
	void voiceFailed(std::shared_ptr<Ui::Show> show, const QString &text);

	const not_null<Room*> _room;
	rpl::event_stream<Reaction> _reactions;
	rpl::event_stream<Thrown> _stickers;
	rpl::event_stream<> _sessionGone;
	rpl::event_stream<> _refreshes;
	ReactionLimiter _outgoing;
	ReactionLimiter _resolveBudget;
	IncomingGuard _incomingReactions;
	IncomingGuard _incomingStickers;
	std::vector<Echo> _echoes;
	crl::time _lastSticker = 0;
	bool _voiceBusy = false;
	rpl::lifetime _sessionLifetime;
	rpl::lifetime _lifetime;

};

Extras::Extras(not_null<Room*> room)
: _room(room)
, _outgoing(kOutgoingBurst, kOutgoingPerSecond)
, _resolveBudget(kResolveBurst, kResolvePerSecond)
, _incomingReactions(6., 3., 24., 12.)
, _incomingStickers(2., 0.5, 4., 1.5) {
	_room->events(
	) | rpl::on_next([=](const Cloud::Event &event) {
		handle(event);
	}, _lifetime);

	_room->changes(
	) | rpl::on_next([=](Changes changes) {
		const auto mine = Changes(Change::Rights)
			| Change::Voice
			| Change::Gone
			| Change::Room
			| Change::Reloaded;
		if (changes & mine) {
			_refreshes.fire({});
		}
	}, _lifetime);
	Oblivion::Get().changes(
	) | rpl::on_next([=] {
		_refreshes.fire({});
	}, _lifetime);

	const auto weak = base::make_weak(this);
	_room->lifetime().add([=] {
		if (const auto strong = weak.get()) {
			strong->detached();
		}
	});
}

void Extras::detached() {
	_sessionLifetime.destroy();
	_voiceBusy = false;
	_sessionGone.fire({});
	_refreshes.fire({});
}

bool Extras::available() const {
	return Oblivion::Get().roomReactions()
		&& _room->can(Right::Chat)
		&& (_room->state().gone == Gone::No)
		&& !FeatureOff(_room, "reactions");
}

void Extras::expectEcho(const QString &key) {
	const auto now = crl::now();
	_echoes.erase(
		ranges::remove_if(_echoes, [&](const Echo &echo) {
			return (now - echo.when) > kOwnEchoTimeout;
		}),
		end(_echoes));
	if (int(_echoes.size()) < kOwnEchoLimit) {
		_echoes.push_back({ .key = key, .when = now });
	}
}

bool Extras::takeEcho(const QString &key) {
	const auto now = crl::now();
	const auto i = ranges::find_if(_echoes, [&](const Echo &echo) {
		return (echo.key == key) && (now - echo.when) <= kOwnEchoTimeout;
	});
	if (i == end(_echoes)) {
		return false;
	}
	_echoes.erase(i);
	return true;
}

void Extras::handle(const Cloud::Event &event) {
	const auto reaction = (event.type == u"room.reaction"_q);
	if (!reaction && event.type != u"room.sticker"_q) {
		return;
	} else if (!Oblivion::Get().roomReactions()) {
		return;
	}
	const auto &data = event.data;
	const auto userId = Cloud::JsonUserId(data.value(u"user_id"_q));
	const auto member = _room->state().member(userId);
	if (!member) {
		return;
	}
	const auto own = (userId == _room->selfId());
	const auto now = crl::now();
	if (reaction) {
		const auto text = CleanReactionEmoji(data.value(u"emoji"_q));
		auto emoji = text.isEmpty() ? nullptr : Ui::Emoji::Find(text);
		if (!emoji && !data.value(u"custom_emoji_id"_q).toString().isEmpty()) {
			// A custom emoji reaction of a newer client: its usual emoji
			// was not sent, a sparkle stands for it.
			emoji = Ui::Emoji::Find(QString::fromUtf8("✨"));
		}
		if (!emoji) {
			return;
		} else if (own && takeEcho(emoji->text())) {
			// Shown already when it was sent.
			return;
		} else if (!_incomingReactions.accept(userId, now)) {
			return;
		}
		_reactions.fire({
			.userId = userId,
			.name = member->name,
			.emoji = emoji,
			.own = own,
		});
	} else {
		const auto sticker = ParseRoomSticker(
			data.value(u"sticker"_q).toObject());
		if (!sticker) {
			return;
		} else if (own
			&& takeEcho(u"s"_q + QString::number(sticker->documentId))) {
			return;
		} else if (!_incomingStickers.accept(userId, now)) {
			return;
		}
		_stickers.fire({
			.userId = userId,
			.name = member->name,
			.sticker = *sticker,
			.own = own,
		});
	}
}

void Extras::react(EmojiPtr emoji) {
	if (!emoji || !available()) {
		return;
	}
	const auto sample = _room->sample();
	if (!sample && !_room->connected()) {
		return;
	} else if (!_outgoing.take(crl::now())) {
		return;
	}
	const auto self = _room->state().member(_room->selfId());
	_reactions.fire({
		.userId = _room->selfId(),
		.name = self ? self->name : QString(),
		.emoji = emoji,
		.own = true,
	});
	if (sample) {
		return;
	}
	auto body = QJsonObject();
	body.insert(u"emoji"_q, emoji->text());
	body.insert(u"target"_q, u"room"_q);
	const auto sent = _room->send(
		Cloud::PostRequest(u"/react"_q, std::move(body)),
		nullptr,
		[](const Cloud::Error &) {});
	if (sent) {
		expectEcho(emoji->text());
	}
}

void Extras::throwDocument(not_null<DocumentData*> document) {
	const auto sticker = StickerOf(document);
	if (sticker.documentId) {
		throwSticker(sticker);
	}
}

void Extras::throwSticker(const RoomSticker &sticker) {
	if (!sticker.documentId
		|| !available()
		|| FeatureOff(_room, "stickers")) {
		return;
	}
	const auto sample = _room->sample();
	const auto now = crl::now();
	if (!sample && !_room->connected()) {
		return;
	} else if (_lastSticker && now - _lastSticker < kStickerCooldown) {
		return;
	} else if (!_outgoing.take(now)) {
		return;
	}
	_lastSticker = now;
	const auto self = _room->state().member(_room->selfId());
	_stickers.fire({
		.userId = _room->selfId(),
		.name = self ? self->name : QString(),
		.sticker = sticker,
		.own = true,
	});
	if (sample) {
		return;
	}
	auto body = QJsonObject();
	body.insert(u"sticker"_q, SerializeRoomSticker(sticker));
	const auto sent = _room->send(
		Cloud::PostRequest(u"/sticker"_q, std::move(body)),
		nullptr,
		[](const Cloud::Error &) {});
	if (sent) {
		expectEcho(u"s"_q + QString::number(sticker.documentId));
	}
}

void Extras::resolve(
		const RoomSticker &sticker,
		Fn<void(DocumentData*)> done) {
	const auto session = _room->session();
	if (!session || !sticker.documentId) {
		done(nullptr);
		return;
	}
	const auto known = session->data().document(
		DocumentId(sticker.documentId));
	if (known->sticker()) {
		done(known);
		return;
	} else if (!_resolveBudget.take(crl::now())) {
		done(nullptr);
		return;
	}
	if (sticker.customEmoji) {
		// Batched by the manager into messages.getCustomEmojiDocuments.
		session->data().customEmojiManager().resolve(
			sticker.documentId
		) | rpl::take(
			1
		) | rpl::on_next_error([=](not_null<DocumentData*> document) {
			done(document->sticker() ? document.get() : nullptr);
		}, [=] {
			done(nullptr);
		}, _sessionLifetime);
		return;
	} else if (!sticker.setId && sticker.setShortName.isEmpty()) {
		done(nullptr);
		return;
	}
	const auto documentId = sticker.documentId;
	const auto weak = base::make_weak(this);
	session->api().request(MTPmessages_GetStickerSet(
		Data::InputStickerSet(StickerSetIdentifier{
			.id = sticker.setId,
			.accessHash = sticker.setAccessHash,
			.shortName = sticker.setShortName,
		}),
		MTP_int(0)
	)).done([=](const MTPmessages_StickerSet &result) {
		const auto strong = weak.get();
		const auto alive = strong ? strong->_room->session() : nullptr;
		if (!alive) {
			return;
		}
		auto found = (DocumentData*)nullptr;
		result.match([&](const MTPDmessages_stickerSet &data) {
			for (const auto &document : data.vdocuments().v) {
				if (document.type() == mtpc_document
					&& uint64(document.c_document().vid().v) == documentId) {
					found = alive->data().processDocument(document);
					break;
				}
			}
		}, [](const MTPDmessages_stickerSetNotModified &) {
		});
		done((found && found->sticker()) ? found : nullptr);
	}).fail([=] {
		if (weak) {
			done(nullptr);
		}
	}).send();
}

// ---- Voice.

struct VoiceCreateArgs {
	QString groupTitle;
	Fn<void()> confirmed;
};

void VoiceCreateBox(not_null<Ui::GenericBox*> box, VoiceCreateArgs &&args) {
	box->setTitle(tr::lng_oblivion_rextra_voice_create_title());
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		tr::lng_oblivion_rextra_voice_create_about(
			lt_title,
			rpl::single(args.groupTitle)),
		st::boxLabel));
	const auto confirmed = args.confirmed;
	box->addButton(tr::lng_oblivion_rextra_voice_create_button(), [=] {
		const auto callback = confirmed;
		box->closeBox();
		if (callback) {
			callback();
		}
	});
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
}

struct VoiceGroupRow {
	uint64 id = 0; // SerializePeerId().
	QString title;
	int members = 0;
	bool open = false; // A public group: its own link is used.
};

class GroupsList final : public Ui::RpWidget {
public:
	GroupsList(QWidget *parent, std::vector<VoiceGroupRow> rows)
	: RpWidget(parent)
	, _rows(std::move(rows)) {
		setMouseTracking(true);
	}

	[[nodiscard]] rpl::producer<VoiceGroupRow> chosen() const {
		return _chosen.events();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return int(_rows.size()) * rowHeight();
	}
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto height = rowHeight();
		const auto left = st::boxRowPadding.left();
		const auto userpic = Scaled(38);
		const auto from = std::max(e->rect().y() / height, 0);
		const auto till = std::min(
			(e->rect().y() + e->rect().height()) / height + 1,
			int(_rows.size()));
		for (auto i = from; i < till; ++i) {
			const auto &row = _rows[i];
			const auto top = i * height;
			if (i == _over) {
				p.fillRect(0, top, width(), height, st::windowBgOver);
			}
			PaintUserpic(
				p,
				QRect(left, top + (height - userpic) / 2, userpic, userpic),
				row.id,
				row.title);
			const auto textLeft = left + userpic + Scaled(12);
			const auto textWidth = width() - textLeft - left;
			p.setFont(st::semiboldFont);
			p.setPen(st::windowFg);
			p.drawText(
				textLeft,
				top + Scaled(9) + st::semiboldFont->ascent,
				st::semiboldFont->elided(row.title, textWidth));
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				textLeft,
				top + Scaled(29) + st::normalFont->ascent,
				st::normalFont->elided(
					(row.open
						? tr::lng_oblivion_rextra_voice_attach_public(tr::now)
						: tr::lng_oblivion_room_members_count(
							tr::now,
							lt_count,
							std::max(row.members, 1))),
					textWidth));
		}
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		setOver(e->pos().y() / rowHeight());
	}
	void leaveEventHook(QEvent *e) override {
		setOver(-1);
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		const auto index = e->pos().y() / rowHeight();
		if (e->button() == Qt::LeftButton
			&& index >= 0
			&& index < int(_rows.size())) {
			_chosen.fire_copy(_rows[index]);
		}
	}

private:
	[[nodiscard]] int rowHeight() const {
		return Scaled(54);
	}
	void setOver(int index) {
		const auto value = (index >= 0 && index < int(_rows.size()))
			? index
			: -1;
		if (_over != value) {
			_over = value;
			setCursor((value >= 0) ? style::cur_pointer : style::cur_default);
			update();
		}
	}

	const std::vector<VoiceGroupRow> _rows;
	rpl::event_stream<VoiceGroupRow> _chosen;
	int _over = -1;

};

struct VoiceAttachArgs {
	std::vector<VoiceGroupRow> rows;
	Fn<void(VoiceGroupRow)> chosen;
};

void VoiceAttachBox(not_null<Ui::GenericBox*> box, VoiceAttachArgs &&args) {
	box->setTitle(tr::lng_oblivion_rextra_voice_attach_title());
	const auto empty = args.rows.empty();
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		(empty
			? tr::lng_oblivion_rextra_voice_attach_empty()
			: tr::lng_oblivion_rextra_voice_attach_about()),
		st::boxLabel));
	if (!empty) {
		const auto list = box->addRow(
			object_ptr<GroupsList>(box, std::move(args.rows)),
			QMargins(0, Scaled(8), 0, 0));
		const auto chosen = args.chosen;
		list->chosen(
		) | rpl::on_next([=](const VoiceGroupRow &row) {
			const auto callback = chosen;
			const auto copy = row;
			box->closeBox();
			if (callback) {
				callback(copy);
			}
		}, list->lifetime());
	}
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
}

void Extras::voiceFailed(
		std::shared_ptr<Ui::Show> show,
		const QString &text) {
	_voiceBusy = false;
	Toast(show, text);
}

void Extras::openVoice(std::shared_ptr<Ui::Show> show) {
	const auto &voice = _room->state().voice;
	const auto session = _room->session();
	if (!voice.valid() || !session || _room->state().gone != Gone::No) {
		return;
	}
	if (const auto peer = VoicePeer(session, voice.chatId)) {
		// The user is in the group already.
		using Confirm = Calls::StartGroupCallArgs::JoinConfirm;
		const auto active = CallActive(peer);
		if (peer->groupCall()) {
			// Joining a video chat asks in the usual box of Telegram.
			auto args = Calls::StartGroupCallArgs();
			args.confirm = Confirm::Always;
			Core::App().calls().startOrJoinGroupCall(show, peer, args);
		} else if (active) {
			// The video chat is on, but the app has not loaded it yet:
			// the group is opened, its own bar joins.
			session->api().requestFullPeer(peer);
			if (const auto window = session->tryResolveWindow()) {
				window->showPeerHistory(peer);
				window->window().activate();
			} else {
				Toast(show, tr::lng_oblivion_rextra_voice_no_window(tr::now));
			}
		} else if (_room->owner() && peer->canManageGroupCall()) {
			// Only the owner of the room (who attached the group) starts
			// a new video chat from here.
			Core::App().calls().startOrJoinGroupCall(
				show,
				peer,
				Calls::StartGroupCallArgs());
		} else {
			Toast(show, (_room->owner()
				? tr::lng_oblivion_rextra_voice_no_call
				: tr::lng_oblivion_rextra_voice_no_call_member)(
					tr::now,
					lt_title,
					peer->name()));
		}
		return;
	}
	const auto window = session->tryResolveWindow();
	if (!window) {
		Toast(show, tr::lng_oblivion_rextra_voice_no_window(tr::now));
		return;
	}
	// Not in the group yet: the standard «Вступить в группу» of Telegram,
	// in the window of this account.
	const auto hash = VoiceInviteHash(voice.link);
	if (!hash.isEmpty()) {
		Api::CheckChatInvite(window, hash);
	} else {
		UrlClickHandler::Open(
			voice.link,
			QVariant::fromValue(ClickHandlerContext{
				.sessionWindow = base::make_weak(window),
			}));
	}
	window->window().activate();
}

void Extras::createVoice(std::shared_ptr<Ui::Show> show) {
	if (!_room->owner() || !_room->session() || !show || !show->valid()) {
		return;
	} else if (_voiceBusy) {
		Toast(show, tr::lng_oblivion_rextra_voice_busy(tr::now));
		return;
	}
	const auto weak = base::make_weak(this);
	show->showBox(Box(VoiceCreateBox, VoiceCreateArgs{
		.groupTitle = VoiceGroupTitle(_room->state().title),
		.confirmed = [=] {
			if (const auto strong = weak.get()) {
				strong->startCreate(show);
			}
		},
	}));
}

void Extras::startCreate(std::shared_ptr<Ui::Show> show) {
	const auto session = _room->session();
	if (!session || _voiceBusy || !_room->owner()) {
		return;
	}
	_voiceBusy = true;
	Toast(show, tr::lng_oblivion_rextra_voice_creating(tr::now));
	const auto weak = base::make_weak(this);
	using Flag = MTPchannels_CreateChannel::Flag;
	session->api().request(MTPchannels_CreateChannel(
		MTP_flags(Flag::f_megagroup),
		MTP_string(VoiceGroupTitle(_room->state().title)),
		MTP_string(tr::lng_oblivion_rextra_voice_group_about(tr::now)),
		MTPInputGeoPoint(), // geo_point
		MTPstring(), // address
		MTP_int(0) // ttl_period
	)).done([=](const MTPUpdates &result) {
		const auto strong = weak.get();
		const auto alive = strong ? strong->_room->session() : nullptr;
		if (!alive) {
			if (strong) {
				strong->_voiceBusy = false;
			}
			return;
		}
		alive->api().applyUpdates(result);
		if (const auto channel = CreatedChannel(alive, result)) {
			strong->exportLink(channel, show, true);
		} else {
			strong->voiceFailed(
				show,
				tr::lng_oblivion_rextra_voice_create_failed(
					tr::now,
					lt_error,
					u"NO_CHAT"_q));
		}
	}).fail([=](const MTP::Error &error) {
		if (const auto strong = weak.get()) {
			strong->voiceFailed(
				show,
				tr::lng_oblivion_rextra_voice_create_failed(
					tr::now,
					lt_error,
					error.type()));
		}
	}).send();
}

void Extras::exportLink(
		not_null<PeerData*> peer,
		std::shared_ptr<Ui::Show> show,
		bool startCall) {
	const auto session = _room->session();
	if (!session) {
		_voiceBusy = false;
		return;
	}
	if (const auto channel = peer->asChannel()) {
		if (channel->hasUsername()) {
			saveVoice(
				peer,
				u"https://t.me/"_q + channel->username(),
				show,
				startCall);
			return;
		}
	}
	// A separate link with its own name: the owner can revoke it in the
	// group later without touching the main link.
	const auto weak = base::make_weak(this);
	using Flag = MTPmessages_ExportChatInvite::Flag;
	session->api().request(MTPmessages_ExportChatInvite(
		MTP_flags(Flag::f_title),
		peer->input(),
		MTPint(), // expire_date
		MTPint(), // usage_limit
		MTP_string(u"Oblivion"_q),
		MTPStarsSubscriptionPricing()
	)).done([=](const MTPExportedChatInvite &result) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		auto link = QString();
		result.match([&](const MTPDchatInviteExported &data) {
			link = qs(data.vlink());
		}, [](const MTPDchatInvitePublicJoinRequests &) {
		});
		if (!strong->_room->session()) {
			strong->_voiceBusy = false;
		} else if (!link.startsWith(u"https://t.me/"_q)) {
			strong->voiceFailed(
				show,
				tr::lng_oblivion_rextra_voice_link_failed(
					tr::now,
					lt_error,
					u"NO_LINK"_q));
		} else {
			strong->saveVoice(peer, link, show, startCall);
		}
	}).fail([=](const MTP::Error &error) {
		if (const auto strong = weak.get()) {
			strong->voiceFailed(
				show,
				tr::lng_oblivion_rextra_voice_link_failed(
					tr::now,
					lt_error,
					error.type()));
		}
	}).send();
}

void Extras::saveVoice(
		not_null<PeerData*> peer,
		const QString &link,
		std::shared_ptr<Ui::Show> show,
		bool startCall) {
	auto body = QJsonObject();
	body.insert(u"link"_q, link);
	body.insert(u"title"_q, peer->name().left(kTitleLimit));
	body.insert(u"tg_chat_id"_q, GroupBareId(peer));
	const auto sent = _room->send(
		Cloud::PutRequest(u"/voice"_q, std::move(body)),
		crl::guard(this, [=](const Cloud::Response &) {
			_voiceBusy = false;
			Toast(show, tr::lng_oblivion_rextra_voice_attached(tr::now));
			if (startCall && _room->session()) {
				// The standard start of a video chat: the owner joins it
				// with the microphone off, as the box has said.
				Core::App().calls().startOrJoinGroupCall(
					show,
					peer,
					Calls::StartGroupCallArgs());
			}
		}),
		crl::guard(this, [=](const Cloud::Error &error) {
			_voiceBusy = false;
			Cloud::ShowError(show, error);
		}));
	if (!sent) {
		_voiceBusy = false;
	}
}

void Extras::attachVoice(std::shared_ptr<Ui::Show> show) {
	const auto session = _room->session();
	if (!_room->owner() || !session || !show || !show->valid()) {
		return;
	} else if (_voiceBusy) {
		Toast(show, tr::lng_oblivion_rextra_voice_busy(tr::now));
		return;
	}
	// Only what the chats list of the app has already: nothing is asked
	// from Telegram for this box.
	auto rows = std::vector<VoiceGroupRow>();
	const auto add = [&](not_null<const Dialogs::IndexedList*> list) {
		for (const auto &row : list->all()) {
			if (int(rows.size()) >= kGroupsLimit) {
				return;
			}
			const auto history = row->history();
			if (!history || !GroupFits(history->peer)) {
				continue;
			}
			const auto peer = history->peer;
			const auto channel = peer->asChannel();
			const auto chat = peer->asChat();
			rows.push_back({
				.id = SerializePeerId(peer->id),
				.title = peer->name(),
				.members = channel
					? channel->membersCount()
					: chat
					? chat->count
					: 0,
				.open = channel && channel->hasUsername(),
			});
		}
	};
	const auto owner = &session->data();
	add(owner->chatsList()->indexed());
	if (const auto folder = owner->folderLoaded(Data::Folder::kId)) {
		add(folder->chatsList()->indexed());
	}
	const auto weak = base::make_weak(this);
	show->showBox(Box(VoiceAttachBox, VoiceAttachArgs{
		.rows = std::move(rows),
		.chosen = [=](VoiceGroupRow row) {
			if (const auto strong = weak.get()) {
				strong->attachChosen(row.id, show);
			}
		},
	}));
}

void Extras::attachChosen(uint64 peerId, std::shared_ptr<Ui::Show> show) {
	const auto session = _room->session();
	if (!session || !show || !show->valid()) {
		return;
	}
	const auto peer = session->data().peerLoaded(DeserializePeerId(peerId));
	if (!peer || !GroupFits(peer)) {
		return;
	}
	const auto channel = peer->asChannel();
	const auto open = channel && channel->hasUsername();
	const auto weak = base::make_weak(this);
	show->showBox(Ui::MakeConfirmBox({
		.text = (open
			? tr::lng_oblivion_rextra_voice_attach_sure_public
			: tr::lng_oblivion_rextra_voice_attach_sure)(
				tr::now,
				lt_title,
				peer->name()),
		.confirmed = [=](Fn<void()> close) {
			close();
			const auto strong = weak.get();
			const auto alive = strong ? strong->_room->session() : nullptr;
			if (!alive || strong->_voiceBusy) {
				return;
			}
			const auto group = alive->data().peerLoaded(
				DeserializePeerId(peerId));
			if (group && GroupFits(group)) {
				strong->_voiceBusy = true;
				strong->exportLink(group, show, false);
			}
		},
		.confirmText = tr::lng_oblivion_rextra_voice_attach_button(tr::now),
	}));
}

void Extras::detachVoice(std::shared_ptr<Ui::Show> show) {
	if (!_room->owner() || !show || !show->valid()) {
		return;
	}
	const auto weak = base::make_weak(this);
	show->showBox(Ui::MakeConfirmBox({
		.text = tr::lng_oblivion_rextra_voice_detach_sure(tr::now),
		.confirmed = [=](Fn<void()> close) {
			if (const auto strong = weak.get()) {
				// The button goes away when the event comes, a failure is
				// one toast of the room window.
				strong->_room->send(Cloud::DeleteRequest(u"/voice"_q));
			}
			close();
		},
		.confirmText = tr::lng_oblivion_rextra_voice_detach_button(tr::now),
	}));
}

[[nodiscard]] std::shared_ptr<Extras> ExtrasFor(not_null<Room*> room) {
	// One object for the overlay, the buttons and the menu of a window.
	// It lives while any of them does, never longer than the room.
	using Entry = std::pair<Room*, std::weak_ptr<Extras>>;
	static const auto Map = new std::vector<Entry>();
	Map->erase(
		ranges::remove_if(*Map, [](const Entry &entry) {
			return entry.second.expired();
		}),
		end(*Map));
	for (const auto &entry : *Map) {
		if (entry.first == room.get()) {
			if (auto strong = entry.second.lock()) {
				return strong;
			}
		}
	}
	auto result = std::make_shared<Extras>(room);
	Map->emplace_back(room.get(), result);
	return result;
}

// ---- The pickers of Telegram inside the room window.

class PickerShow final : public ChatHelpers::Show {
public:
	PickerShow(
		not_null<Main::Session*> session,
		std::shared_ptr<Ui::Show> base)
	: _session(base::make_weak(session))
	, _base(std::move(base)) {
	}

	void activate() override {
	}
	void showOrHideBoxOrLayer(
			std::variant<
				v::null_t,
				object_ptr<Ui::BoxContent>,
				std::unique_ptr<Ui::LayerWidget>> &&layer,
			Ui::LayerOptions options,
			anim::type animated) const override {
		_base->showOrHideBoxOrLayer(std::move(layer), options, animated);
	}
	not_null<QWidget*> toastParent() const override {
		return _base->toastParent();
	}
	bool valid() const override {
		return (_session.get() != nullptr) && _base->valid();
	}
	operator bool() const override {
		return valid();
	}

	Main::Session &session() const override {
		const auto strong = _session.get();
		Assert(strong != nullptr);
		return *strong;
	}
	bool paused(ChatHelpers::PauseReason reason) const override {
		return false;
	}
	rpl::producer<> pauseChanged() const override {
		return rpl::never<>();
	}
	rpl::producer<bool> adjustShadowLeft() const override {
		return rpl::single(false);
	}
	SendMenu::Details sendMenuDetails() const override {
		return { SendMenu::Type::Disabled };
	}
	bool showMediaPreview(
			Data::FileOrigin origin,
			not_null<DocumentData*> document) const override {
		return false;
	}
	bool showMediaPreview(
			Data::FileOrigin origin,
			not_null<PhotoData*> photo) const override {
		return false;
	}
	void processChosenSticker(
			ChatHelpers::FileChosen &&chosen) const override {
	}

private:
	const base::weak_ptr<Main::Session> _session;
	const std::shared_ptr<Ui::Show> _base;

};

// ---- The buttons of the header.

enum class HeaderIcon {
	Smile,
	Voice,
};

void PaintHeaderIcon(
		QPainter &p,
		HeaderIcon icon,
		QRectF rect,
		const QColor &color) {
	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	const auto s = std::min(rect.width(), rect.height());
	p.translate(
		rect.x() + (rect.width() - s) / 2.,
		rect.y() + (rect.height() - s) / 2.);
	p.scale(s, s);
	auto stroke = QPen(color, 0.085);
	stroke.setCapStyle(Qt::RoundCap);
	stroke.setJoinStyle(Qt::RoundJoin);
	switch (icon) {
	case HeaderIcon::Smile: {
		p.setPen(stroke);
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(QPointF(0.5, 0.5), 0.4, 0.4);
		auto smile = QPainterPath();
		smile.moveTo(0.31, 0.57);
		smile.cubicTo(0.38, 0.76, 0.62, 0.76, 0.69, 0.57);
		p.drawPath(smile);
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawEllipse(QPointF(0.36, 0.4), 0.055, 0.055);
		p.drawEllipse(QPointF(0.64, 0.4), 0.055, 0.055);
	} break;
	case HeaderIcon::Voice: {
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawRoundedRect(QRectF(0.38, 0.08, 0.24, 0.5), 0.12, 0.12);
		p.setPen(stroke);
		p.setBrush(Qt::NoBrush);
		auto holder = QPainterPath();
		holder.moveTo(0.24, 0.44);
		holder.cubicTo(0.24, 0.8, 0.76, 0.8, 0.76, 0.44);
		p.drawPath(holder);
		p.drawLine(QPointF(0.5, 0.72), QPointF(0.5, 0.9));
		p.drawLine(QPointF(0.36, 0.9), QPointF(0.64, 0.9));
	} break;
	}
	p.restore();
}

// A header button that is shown only while it has something to offer:
// the header of the room window follows its visibility.
class HeaderButton : public Ui::AbstractButton {
public:
	HeaderButton(QWidget *parent, HeaderIcon icon)
	: AbstractButton(parent)
	, _icon(icon) {
		resize(Scaled(34), Scaled(34));
	}

	void setWanted(bool wanted) {
		if (_wanted != wanted) {
			_wanted = wanted;
			setVisible(wanted);
		}
	}
	void setHighlighted(bool highlighted) {
		if (_highlighted != highlighted) {
			_highlighted = highlighted;
			update();
		}
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto full = QRectF(rect());
		if (isOver() || isDown() || _highlighted) {
			p.setPen(Qt::NoPen);
			p.setBrush(isDown() ? st::windowBgRipple : st::windowBgOver);
			p.drawEllipse(full);
		}
		const auto skip = full.width() * 0.2;
		PaintHeaderIcon(
			p,
			_icon,
			full.marginsRemoved(QMarginsF(skip, skip, skip, skip)),
			_highlighted ? st::windowActiveTextFg->c : st::windowFg->c);
	}
	void onStateChanged(State was, StateChangeSource source) override {
		update();
	}
	void setVisibleHook(bool visible) override {
		AbstractButton::setVisibleHook(visible && _wanted);
	}

private:
	const HeaderIcon _icon;
	bool _wanted = false;
	bool _highlighted = false;

};

// ---- The panel of the reactions under the smile button.

class ReactionPanel final : public Ui::RpWidget {
public:
	ReactionPanel(
		QWidget *parent,
		not_null<HeaderButton*> anchor,
		std::shared_ptr<Extras> extras,
		std::shared_ptr<Ui::Show> show);

	[[nodiscard]] bool shown() const {
		return _shown;
	}
	void toggle();
	void showAnimated();
	void hideAnimated();

protected:
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	enum class Part {
		None,
		Emoji,
		AllEmoji,
		Stickers,
	};
	struct Hit {
		Part part = Part::None;
		int index = 0;

		friend inline bool operator==(const Hit &, const Hit &) = default;
	};

	void updateGeometry();
	[[nodiscard]] Hit hitAt(QPoint position) const;
	[[nodiscard]] QRect cellRect(int index) const;
	[[nodiscard]] QRect buttonRect(bool stickers) const;
	[[nodiscard]] bool pickerContains(QPoint position) const;
	void setOver(Hit hit);
	void openPicker(bool stickers);
	void dropPickers();

	const QPointer<HeaderButton> _anchor;
	const std::shared_ptr<Extras> _extras;
	const std::shared_ptr<Ui::Show> _show;
	std::vector<EmojiPtr> _emoji;
	base::unique_qptr<ChatHelpers::TabbedPanel> _emojiPicker;
	base::unique_qptr<ChatHelpers::TabbedPanel> _stickerPicker;
	Ui::Animations::Simple _animation;
	QRect _card;
	Hit _over;
	Hit _pressed;
	bool _shown = false;

};

ReactionPanel::ReactionPanel(
	QWidget *parent,
	not_null<HeaderButton*> anchor,
	std::shared_ptr<Extras> extras,
	std::shared_ptr<Ui::Show> show)
: RpWidget(parent)
, _anchor(anchor.get())
, _extras(std::move(extras))
, _show(std::move(show)) {
	setMouseTracking(true);
	setFocusPolicy(Qt::ClickFocus);
	for (const auto text : kQuickEmoji) {
		if (const auto emoji = Ui::Emoji::Find(QString::fromUtf8(text))) {
			_emoji.push_back(emoji);
		}
	}
	if (parent) {
		base::install_event_filter(this, parent, [=](not_null<QEvent*> e) {
			if (e->type() == QEvent::Resize && !isHidden()) {
				updateGeometry();
			}
			return base::EventFilterResult::Continue;
		});
	}
	_extras->sessionGone(
	) | rpl::on_next([=] {
		dropPickers();
		_animation.stop();
		_shown = false;
		hide();
	}, lifetime());
	_extras->refreshes(
	) | rpl::on_next([=] {
		if (_shown && !_extras->available()) {
			hideAnimated();
		}
	}, lifetime());
	hide();
}

void ReactionPanel::dropPickers() {
	_emojiPicker = nullptr;
	_stickerPicker = nullptr;
}

void ReactionPanel::updateGeometry() {
	const auto host = parentWidget();
	if (!host) {
		return;
	}
	setGeometry(host->rect());
	const auto cell = Scaled(40);
	const auto pad = Scaled(8);
	const auto count = std::max(int(_emoji.size()), 1);
	const auto margin = Scaled(8);
	const auto cardWidth = std::min(
		count * cell + 2 * pad,
		std::max(width() - 2 * margin, cell));
	const auto cardHeight = pad + cell + Scaled(6) + Scaled(36) + pad / 2;
	auto anchor = QPoint(width() - margin, Scaled(56));
	if (const auto button = _anchor.data()) {
		anchor = button->mapTo(
			host,
			QPoint(button->width(), button->height()));
	}
	_card = QRect(
		std::clamp(
			anchor.x() - cardWidth + Scaled(10),
			margin,
			std::max(width() - margin - cardWidth, margin)),
		anchor.y() + Scaled(6),
		cardWidth,
		cardHeight);
	const auto right = _card.x() + _card.width();
	const auto top = _card.y() + _card.height() + Scaled(2);
	for (const auto &picker : { &_emojiPicker, &_stickerPicker }) {
		if (const auto panel = picker->get()) {
			panel->moveTopRight(top, right);
		}
	}
	update();
}

QRect ReactionPanel::cellRect(int index) const {
	const auto pad = Scaled(8);
	const auto count = std::max(int(_emoji.size()), 1);
	const auto cell = std::min(Scaled(40), (_card.width() - 2 * pad) / count);
	return QRect(
		_card.x() + pad + index * cell,
		_card.y() + pad,
		cell,
		Scaled(40));
}

QRect ReactionPanel::buttonRect(bool stickers) const {
	const auto pad = Scaled(8);
	const auto top = _card.y() + pad + Scaled(40) + Scaled(6);
	const auto half = (_card.width() - 2 * pad) / 2;
	return QRect(
		_card.x() + pad + (stickers ? half : 0),
		top,
		half,
		Scaled(36) - pad / 2);
}

ReactionPanel::Hit ReactionPanel::hitAt(QPoint position) const {
	if (!_card.contains(position)) {
		return {};
	}
	for (auto i = 0, count = int(_emoji.size()); i != count; ++i) {
		if (cellRect(i).contains(position)) {
			return { .part = Part::Emoji, .index = i };
		}
	}
	if (buttonRect(false).contains(position)) {
		return { .part = Part::AllEmoji };
	} else if (buttonRect(true).contains(position)) {
		return { .part = Part::Stickers };
	}
	return {};
}

bool ReactionPanel::pickerContains(QPoint position) const {
	for (const auto &picker : { &_emojiPicker, &_stickerPicker }) {
		const auto panel = picker->get();
		if (panel && !panel->isHidden() && panel->geometry().contains(position)) {
			return true;
		}
	}
	return false;
}

void ReactionPanel::setOver(Hit hit) {
	if (_over != hit) {
		_over = hit;
		setCursor((hit.part != Part::None)
			? style::cur_pointer
			: style::cur_default);
		update(_card);
	}
}

void ReactionPanel::toggle() {
	if (_shown) {
		hideAnimated();
	} else {
		showAnimated();
	}
}

void ReactionPanel::showAnimated() {
	if (_shown || !_extras->available()) {
		return;
	}
	_shown = true;
	updateGeometry();
	show();
	raise();
	_animation.start([=] { update(); }, 0., 1., kPanelDuration);
}

void ReactionPanel::hideAnimated() {
	if (!_shown) {
		return;
	}
	_shown = false;
	for (const auto &picker : { &_emojiPicker, &_stickerPicker }) {
		if (const auto panel = picker->get()) {
			panel->hideFast();
		}
	}
	_animation.start([=] {
		update();
	}, 1., 0., kPanelDuration);
	// The widget covers the whole window: it must not stay there
	// invisible if no paint comes (a minimized window).
	base::call_delayed(kPanelDuration + 60, this, [=] {
		if (!_shown) {
			_animation.stop();
			hide();
		}
	});
}

void ReactionPanel::paintEvent(QPaintEvent *e) {
	const auto value = _animation.value(_shown ? 1. : 0.);
	if (value <= 0.) {
		if (!_shown && !_animation.animating()) {
			// Hidden from the next turn of the loop, not from a paint.
			crl::on_main(this, [=] {
				if (!_shown && !_animation.animating()) {
					hide();
				}
			});
		}
		return;
	}
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	p.setOpacity(value);
	p.translate(0, -(1. - value) * Scaled(6));

	const auto card = QRectF(_card);
	const auto radius = double(Scaled(14));
	p.setPen(Qt::NoPen);
	for (auto i = 0; i != 4; ++i) {
		const auto grow = (4 - i) * 1.5;
		p.setBrush(QColor(0, 0, 0, 6 + i * 5));
		p.drawRoundedRect(
			card.marginsAdded(QMarginsF(grow, grow, grow, grow)).translated(
				0,
				Scaled(2)),
			radius + grow,
			radius + grow);
	}
	p.setBrush(st::windowBg);
	p.setPen(QPen(st::shadowFg->c, 1.));
	p.drawRoundedRect(card, radius, radius);

	for (auto i = 0, count = int(_emoji.size()); i != count; ++i) {
		const auto cell = QRectF(cellRect(i));
		const auto over = (_over == Hit{ .part = Part::Emoji, .index = i });
		const auto down = over && (_pressed == _over);
		if (over) {
			p.setPen(Qt::NoPen);
			p.setBrush(down ? st::windowBgRipple : st::windowBgOver);
			const auto side = std::min(cell.width(), cell.height()) - 2.;
			p.drawEllipse(cell.center(), side / 2., side / 2.);
		}
		const auto size = Scaled(over ? 30 : 26) * (down ? 0.9 : 1.);
		PaintEmoji(p, _emoji[i], QRectF(
			cell.center().x() - size / 2.,
			cell.center().y() - size / 2.,
			size,
			size));
	}
	const auto line = _card.y() + Scaled(8) + Scaled(40) + Scaled(2);
	p.fillRect(
		_card.x() + Scaled(12),
		line,
		_card.width() - Scaled(24),
		st::lineWidth,
		st::shadowFg);
	const auto session = (_extras->room()->session() != nullptr)
		|| _extras->room()->sample();
	const auto button = [&](bool stickers, const QString &text) {
		const auto rect = buttonRect(stickers);
		const auto part = stickers ? Part::Stickers : Part::AllEmoji;
		if (_over.part == part) {
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgOver);
			p.drawRoundedRect(
				QRectF(rect).marginsRemoved(QMarginsF(2., 3., 2., 1.)),
				Scaled(8),
				Scaled(8));
		}
		p.setFont(st::semiboldFont);
		p.setPen(session ? st::windowActiveTextFg : st::windowSubTextFg);
		p.drawText(
			rect,
			Qt::AlignCenter,
			st::semiboldFont->elided(text, rect.width() - Scaled(8)));
	};
	button(false, tr::lng_oblivion_rextra_panel_emoji(tr::now));
	button(true, tr::lng_oblivion_rextra_panel_stickers(tr::now));
}

void ReactionPanel::mousePressEvent(QMouseEvent *e) {
	const auto position = e->pos();
	if (pickerContains(position)) {
		return;
	} else if (!_card.contains(position)) {
		hideAnimated();
		return;
	}
	if (e->button() == Qt::LeftButton) {
		_pressed = hitAt(position);
		update(_card);
	}
}

void ReactionPanel::mouseMoveEvent(QMouseEvent *e) {
	setOver(hitAt(e->pos()));
}

void ReactionPanel::leaveEventHook(QEvent *e) {
	setOver({});
}

void ReactionPanel::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = base::take(_pressed);
	const auto hit = hitAt(e->pos());
	update(_card);
	if (e->button() != Qt::LeftButton
		|| hit != pressed
		|| hit.part == Part::None) {
		return;
	}
	switch (hit.part) {
	case Part::Emoji:
		if (hit.index >= 0 && hit.index < int(_emoji.size())) {
			_extras->react(_emoji[hit.index]);
		}
		break;
	case Part::AllEmoji: openPicker(false); break;
	case Part::Stickers: openPicker(true); break;
	case Part::None: break;
	}
}

void ReactionPanel::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Escape) {
		hideAnimated();
	} else {
		RpWidget::keyPressEvent(e);
	}
}

void ReactionPanel::openPicker(bool stickers) {
	const auto session = _extras->room()->session();
	if (!session) {
		return;
	}
	auto &mine = stickers ? _stickerPicker : _emojiPicker;
	auto &other = stickers ? _emojiPicker : _stickerPicker;
	if (other) {
		other->hideFast();
	}
	if (!mine) {
		using Selector = ChatHelpers::TabbedSelector;
		mine = base::make_unique_q<ChatHelpers::TabbedPanel>(
			this,
			ChatHelpers::TabbedPanelDescriptor{
				.ownedSelector = object_ptr<Selector>(
					nullptr,
					ChatHelpers::TabbedSelectorDescriptor{
						.show = std::make_shared<PickerShow>(session, _show),
						.st = st::defaultEmojiPan,
						.level = ChatHelpers::PauseReason::Layer,
						.mode = (stickers
							? Selector::Mode::StickersOnly
							: Selector::Mode::EmojiOnly),
						.features = {
							.stickersSettings = false,
							.openStickerSets = false,
						},
					}),
			});
		const auto panel = mine.get();
		panel->setDropDown(true);
		panel->setDesiredHeightValues(
			1.,
			st::emojiPanMinHeight / 2,
			st::emojiPanMinHeight);
		const auto selector = panel->selector();
		selector->setAllowEmojiWithoutPremium(true);
		const auto extras = _extras;
		if (stickers) {
			selector->fileChosen(
			) | rpl::on_next([=](const ChatHelpers::FileChosen &data) {
				extras->throwDocument(data.document);
				panel->hideAnimated();
			}, panel->lifetime());
		} else {
			selector->emojiChosen(
			) | rpl::on_next([=](const ChatHelpers::EmojiChosen &data) {
				extras->react(data.emoji);
			}, panel->lifetime());
			selector->customEmojiChosen(
			) | rpl::on_next([=](const ChatHelpers::FileChosen &data) {
				extras->throwDocument(data.document);
			}, panel->lifetime());
		}
		updateGeometry();
	}
	mine->toggleAnimated();
}

bool ScenePanelOpen = false;

class ReactButton final : public HeaderButton {
public:
	ReactButton(QWidget *parent, TabContext context)
	: HeaderButton(parent, HeaderIcon::Smile)
	, _extras(ExtrasFor(context.room))
	, _show(context.show) {
		setClickedCallback([=] {
			toggle();
		});
		_extras->refreshes(
		) | rpl::on_next([=] {
			setWanted(_extras->available());
		}, lifetime());
		setWanted(_extras->available());
		if (ScenePanelOpen && _extras->room()->sample()) {
			// After the scene is laid out.
			InvokeQueued(this, [=] {
				toggle();
			});
		}
	}
	~ReactButton() {
		// The panel is a child of the widget above the header: it is not
		// touched here, only asked to go.
		if (const auto panel = _panel.data()) {
			panel->deleteLater();
		}
	}

private:
	void toggle() {
		if (!_panel) {
			// Above everything of the room window: the header belongs to
			// the widget that fills it.
			const auto header = parentWidget();
			const auto host = (header && header->parentWidget())
				? header->parentWidget()
				: header;
			if (!host) {
				return;
			}
			_panel = Ui::CreateChild<ReactionPanel>(
				host,
				this,
				_extras,
				_show);
			_panel->shownValue(
			) | rpl::on_next([=](bool shown) {
				setHighlighted(shown && _panel && _panel->shown());
			}, lifetime());
		}
		_panel->toggle();
		setHighlighted(_panel->shown());
	}

	const std::shared_ptr<Extras> _extras;
	const std::shared_ptr<Ui::Show> _show;
	QPointer<ReactionPanel> _panel;

};

class VoiceButton final : public HeaderButton {
public:
	VoiceButton(QWidget *parent, TabContext context)
	: HeaderButton(parent, HeaderIcon::Voice)
	, _extras(ExtrasFor(context.room))
	, _show(context.show)
	, _had(context.room->state().voice.valid()) {
		setHighlighted(true);
		setClickedCallback([=] {
			_extras->openVoice(_show);
		});
		_extras->refreshes(
		) | rpl::on_next([=] {
			refresh();
		}, lifetime());
		refresh();
	}

private:
	void refresh() {
		const auto room = _extras->room();
		const auto &voice = room->state().voice;
		const auto has = voice.valid() && (room->state().gone == Gone::No);
		setWanted(has);
		if (has && !_had && voice.setBy != room->selfId()) {
			Toast(_show, tr::lng_oblivion_rextra_voice_appeared(tr::now));
		}
		_had = has;
	}

	const std::shared_ptr<Extras> _extras;
	const std::shared_ptr<Ui::Show> _show;
	bool _had = false;

};

// ---- What flies over the room.

class ReactionsOverlay;
Fn<void(not_null<ReactionsOverlay*>)> OverlaySceneSetup;

class ReactionsOverlay final : public Ui::RpWidget {
public:
	ReactionsOverlay(QWidget *parent, TabContext context);

	// For the snapshot scenes: the time stands still.
	void freeze();
	void addSample(
		uint64 userId,
		const char *emoji,
		double x,
		double age,
		bool own = false);
	void setSampleMoment(uint64 userId, const char *emoji, double age);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	struct Particle {
		EmojiPtr emoji = nullptr;
		QString name;
		uint64 userId = 0;
		crl::time start = 0;
		double x = 0.5; // Part of the width.
		double sway = 0.; // Pixels.
		double phase = 0.;
		double rise = 0.7; // Part of the height.
		double size = 36.;
		bool own = false;
	};
	struct Moment {
		uint64 userId = 0;
		QString name;
		RoomSticker sticker;
		EmojiPtr fallback = nullptr;
		crl::time start = 0;
		crl::time end = 0;
		bool waiting = true; // For the document.
		std::shared_ptr<Data::DocumentMedia> media;
		std::unique_ptr<HistoryView::StickerPlayer> player;
		rpl::lifetime loading;
	};

	[[nodiscard]] crl::time now() const;
	[[nodiscard]] QString nameOf(uint64 userId, const QString &name) const;
	[[nodiscard]] double random();
	void add(const Extras::Reaction &reaction);
	void push(Extras::Thrown &&thrown);
	void startMoment(Extras::Thrown &&thrown);
	void setDocument(DocumentData *document);
	void checkLoaded();
	void finishMoment();
	void dropSession();
	[[nodiscard]] bool step(crl::time now);
	void paintParticle(QPainter &p, const Particle &particle, crl::time now);
	void paintMoment(QPainter &p, crl::time now);
	[[nodiscard]] int stickerSide() const;

	const not_null<Room*> _room;
	const std::shared_ptr<Extras> _extras;
	std::vector<Particle> _particles;
	std::unique_ptr<Moment> _moment;
	std::deque<Extras::Thrown> _queue;
	Ui::Animations::Basic _animation;
	int _momentId = 0;
	uint32 _seed = 0;
	bool _frozen = false;
	crl::time _frozenNow = 0;

};

ReactionsOverlay::ReactionsOverlay(QWidget *parent, TabContext context)
: RpWidget(parent)
, _room(context.room)
, _extras(ExtrasFor(context.room))
, _animation([=](crl::time now) { return step(now); })
, _seed(base::RandomValue<uint32>()) {
	setAttribute(Qt::WA_TransparentForMouseEvents);

	_extras->reactions(
	) | rpl::on_next([=](const Extras::Reaction &reaction) {
		add(reaction);
	}, lifetime());
	_extras->stickers(
	) | rpl::on_next([=](Extras::Thrown thrown) {
		push(std::move(thrown));
	}, lifetime());
	_extras->sessionGone(
	) | rpl::on_next([=] {
		dropSession();
	}, lifetime());

	if (_room->sample() && OverlaySceneSetup) {
		OverlaySceneSetup(this);
	}
}

crl::time ReactionsOverlay::now() const {
	return _frozen ? _frozenNow : crl::now();
}

double ReactionsOverlay::random() {
	_seed = _seed * 1664525U + 1013904223U;
	return ((_seed >> 8) & 0xFFFF) / 65535.;
}

QString ReactionsOverlay::nameOf(uint64 userId, const QString &name) const {
	return (userId == _room->selfId()) ? QString() : name;
}

void ReactionsOverlay::freeze() {
	_frozen = true;
	_frozenNow = 1'000'000;
	_seed = 20261007;
	_animation.stop();
}

void ReactionsOverlay::addSample(
		uint64 userId,
		const char *emoji,
		double x,
		double age,
		bool own) {
	const auto member = _room->state().member(userId);
	add({
		.userId = userId,
		.name = member ? member->name : QString(),
		.emoji = Ui::Emoji::Find(QString::fromUtf8(emoji)),
		.own = own,
	});
	if (!_particles.empty()) {
		auto &particle = _particles.back();
		particle.x = x;
		particle.start = _frozenNow - crl::time(age * kParticleDuration);
	}
	update();
}

void ReactionsOverlay::setSampleMoment(
		uint64 userId,
		const char *emoji,
		double age) {
	const auto member = _room->state().member(userId);
	auto sticker = RoomSticker();
	sticker.documentId = 1;
	sticker.emoji = QString::fromUtf8(emoji);
	startMoment({
		.userId = userId,
		.name = member ? member->name : QString(),
		.sticker = sticker,
	});
	if (_moment) {
		_moment->start = _frozenNow - crl::time(age * kMomentDuration);
		_moment->end = _moment->start + kMomentDuration;
	}
	_animation.stop();
	update();
}

void ReactionsOverlay::add(const Extras::Reaction &reaction) {
	if (!reaction.emoji || int(_particles.size()) >= kParticlesLimit) {
		return;
	}
	auto particle = Particle();
	particle.emoji = reaction.emoji;
	particle.userId = reaction.userId;
	particle.name = reaction.own
		? QString()
		: nameOf(reaction.userId, reaction.name);
	particle.own = reaction.own;
	particle.start = now();
	particle.x = 0.14 + 0.72 * random();
	particle.sway = Scaled(10) + Scaled(14) * random();
	particle.phase = 6.28 * random();
	particle.rise = 0.55 + 0.35 * random();
	particle.size = Scaled(32) + Scaled(10) * random();
	_particles.push_back(std::move(particle));
	if (!_frozen && !_animation.animating()) {
		_animation.start();
	}
	update();
}

void ReactionsOverlay::push(Extras::Thrown &&thrown) {
	if (!_moment) {
		startMoment(std::move(thrown));
	} else if (int(_queue.size()) < kMomentQueueLimit) {
		_queue.push_back(std::move(thrown));
	}
}

void ReactionsOverlay::startMoment(Extras::Thrown &&thrown) {
	_moment = std::make_unique<Moment>();
	_moment->userId = thrown.userId;
	_moment->name = nameOf(thrown.userId, thrown.name);
	_moment->sticker = thrown.sticker;
	_moment->fallback = thrown.sticker.emoji.isEmpty()
		? nullptr
		: Ui::Emoji::Find(thrown.sticker.emoji);
	_moment->start = now();
	_moment->end = _moment->start + kMomentDuration;
	const auto id = ++_momentId;
	if (_room->sample() || !_room->session()) {
		_moment->waiting = false;
	} else {
		_extras->resolve(
			thrown.sticker,
			crl::guard(this, [=](DocumentData *document) {
				if (_moment && _momentId == id) {
					setDocument(document);
				}
			}));
	}
	if (!_frozen && !_animation.animating()) {
		_animation.start();
	}
	update();
}

void ReactionsOverlay::setDocument(DocumentData *document) {
	if (!_moment) {
		return;
	}
	_moment->waiting = false;
	if (!document || !document->sticker() || !_room->session()) {
		if (!_moment->fallback) {
			// Nothing to show at all.
			_moment->end = std::min(_moment->end, now() + kMomentDisappear);
		}
		return;
	}
	_moment->media = document->createMediaView();
	_moment->media->checkStickerLarge();
	_moment->media->goodThumbnailWanted();
	rpl::single(
		rpl::empty
	) | rpl::then(
		document->session().downloaderTaskFinished()
	) | rpl::on_next([=] {
		checkLoaded();
	}, _moment->loading);
}

int ReactionsOverlay::stickerSide() const {
	return Scaled(168);
}

void ReactionsOverlay::checkLoaded() {
	if (!_moment
		|| _moment->player
		|| !_moment->media
		|| !_moment->media->loaded()) {
		return;
	}
	const auto document = _moment->media->owner();
	const auto info = document->sticker();
	if (!info) {
		return;
	}
	// One size for every sticker: the frames cache of the app is keyed by
	// the size tag.
	const auto size = QSize(stickerSide(), stickerSide())
		* style::DevicePixelRatio();
	if (info->isLottie()) {
		_moment->player = std::make_unique<HistoryView::LottiePlayer>(
			ChatHelpers::LottiePlayerFromDocument(
				_moment->media.get(),
				ChatHelpers::StickerLottieSize::EmojiInteractionReserved4,
				size,
				Lottie::Quality::High));
	} else if (info->isWebm()) {
		_moment->player = std::make_unique<HistoryView::WebmPlayer>(
			document->location(),
			_moment->media->bytes(),
			size);
	} else {
		_moment->player = std::make_unique<HistoryView::StaticStickerPlayer>(
			document->location(),
			_moment->media->bytes(),
			size);
	}
	_moment->player->setRepaintCallback(crl::guard(this, [=] {
		update();
	}));
	const auto ready = now();
	if (!_moment->fallback) {
		// Nothing was shown while it was loading: it appears now.
		_moment->start = ready;
		_moment->end = ready + kMomentDuration;
	} else {
		_moment->end = std::min(
			std::max(_moment->end, ready + kMomentAfterReady),
			_moment->start + kMomentMax);
	}
	update();
}

void ReactionsOverlay::finishMoment() {
	_moment = nullptr;
	if (!_queue.empty()) {
		auto next = std::move(_queue.front());
		_queue.pop_front();
		startMoment(std::move(next));
	}
}

void ReactionsOverlay::dropSession() {
	// The players and the media belong to the session that goes away.
	_queue.clear();
	_moment = nullptr;
	update();
}

bool ReactionsOverlay::step(crl::time now) {
	_particles.erase(
		ranges::remove_if(_particles, [&](const Particle &particle) {
			return (now - particle.start) >= kParticleDuration;
		}),
		end(_particles));
	if (_moment && now >= _moment->end) {
		finishMoment();
	}
	update();
	return !_particles.empty() || (_moment != nullptr);
}

void ReactionsOverlay::paintParticle(
		QPainter &p,
		const Particle &particle,
		crl::time now) {
	const auto t = std::clamp(
		(now - particle.start) / double(kParticleDuration),
		0.,
		1.);
	const auto w = double(width());
	const auto h = double(height());
	const auto eased = 1. - std::pow(1. - t, 2.2);
	const auto half = particle.size / 2.;
	const auto y = h - Scaled(34) - eased * particle.rise * (h - Scaled(70));
	const auto x = std::clamp(
		w * particle.x + particle.sway * std::sin(particle.phase + t * 7.),
		half + Scaled(4),
		std::max(w - half - Scaled(4), half + Scaled(4)));
	const auto scale = (t < 0.1)
		? (0.3 + 0.85 * (t / 0.1))
		: (t < 0.18)
		? (1.15 - 0.15 * ((t - 0.1) / 0.08))
		: 1.;
	const auto opacity = (t < 0.75) ? 1. : std::max(1. - (t - 0.75) / 0.25, 0.);
	const auto side = particle.size * scale;
	p.setOpacity(opacity);
	PaintEmoji(p, particle.emoji, QRectF(
		x - side / 2.,
		y - side / 2.,
		side,
		side));
	const auto tag = (t < 0.5) ? 1. : std::max(1. - (t - 0.5) / 0.15, 0.);
	PaintNameTag(
		p,
		QPointF(x, y + half + Scaled(2)),
		particle.name,
		particle.userId,
		tag);
	p.setOpacity(1.);
}

void ReactionsOverlay::paintMoment(QPainter &p, crl::time now) {
	const auto &moment = *_moment;
	const auto passed = now - moment.start;
	const auto appear = std::clamp(passed / double(kMomentAppear), 0., 1.);
	const auto vanish = std::clamp(
		(moment.end - now) / double(kMomentDisappear),
		0.,
		1.);
	const auto opacity = std::min(appear, vanish);
	const auto playing = moment.player
		&& moment.player->ready()
		&& moment.media;
	if (opacity <= 0. || (!playing && !moment.fallback)) {
		return;
	}
	auto dim = st::windowBg->c;
	dim.setAlphaF(0.55 * opacity);
	p.fillRect(rect(), dim);

	const auto limit = std::min(width(), height()) - Scaled(72);
	const auto side = std::clamp(stickerSide(), Scaled(48), std::max(limit, Scaled(48)));
	const auto scale = 0.6 + 0.4 * EaseOutBack(appear);
	const auto center = QPointF(width() / 2., height() / 2. - Scaled(14));
	const auto box = QRectF(
		center.x() - side * scale / 2.,
		center.y() - side * scale / 2.,
		side * scale,
		side * scale);
	p.setOpacity(opacity);
	auto painted = false;
	if (playing) {
		const auto document = moment.media->owner();
		auto frame = moment.player->frame(
			QSize(stickerSide(), stickerSide()),
			(document->emojiUsesTextColor()
				? st::windowFg->c
				: QColor(0, 0, 0, 0)),
			false,
			crl::now(),
			false);
		if (!frame.image.isNull()) {
			const auto fitted = QSizeF(frame.image.size()).scaled(
				box.size(),
				Qt::KeepAspectRatio);
			p.setRenderHint(QPainter::SmoothPixmapTransform);
			p.drawImage(
				QRectF(
					center.x() - fitted.width() / 2.,
					center.y() - fitted.height() / 2.,
					fitted.width(),
					fitted.height()),
				frame.image);
			moment.player->markFrameShown();
			painted = true;
		}
	}
	if (!painted && moment.fallback) {
		// The pictures of emoji are small: not stretched to the size of
		// a sticker.
		const auto size = std::min(box.width() * 0.62, Scaled(84) * scale);
		PaintEmoji(p, moment.fallback, QRectF(
			center.x() - size / 2.,
			center.y() - size / 2.,
			size,
			size));
	}
	PaintNameTag(
		p,
		QPointF(center.x(), center.y() + side / 2. + Scaled(6)),
		moment.name,
		moment.userId,
		1.);
	p.setOpacity(1.);
}

void ReactionsOverlay::paintEvent(QPaintEvent *e) {
	if (_particles.empty() && !_moment) {
		return;
	}
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto time = now();
	if (_moment) {
		paintMoment(p, time);
	}
	for (const auto &particle : _particles) {
		paintParticle(p, particle, time);
	}
}

// ---- The registration in the room window.

const auto OverlayRegistration = OverlayRegistrar([] {
	return OverlayDescriptor{
		.id = u"reactions"_q,
		.create = [](QWidget *parent, TabContext context) {
			return object_ptr<Ui::RpWidget>::fromRaw(
				Ui::CreateChild<ReactionsOverlay>(parent, std::move(context)));
		},
	};
});

const auto ReactRegistration = HeaderButtonRegistrar([] {
	return HeaderButtonDescriptor{
		.id = u"react"_q,
		.order = 100,
		.create = [](QWidget *parent, TabContext context) {
			return object_ptr<Ui::RpWidget>::fromRaw(
				Ui::CreateChild<ReactButton>(parent, std::move(context)));
		},
	};
});

const auto VoiceRegistration = HeaderButtonRegistrar([] {
	return HeaderButtonDescriptor{
		.id = u"voice"_q,
		.order = 200,
		.create = [](QWidget *parent, TabContext context) {
			return object_ptr<Ui::RpWidget>::fromRaw(
				Ui::CreateChild<VoiceButton>(parent, std::move(context)));
		},
	};
});

const auto MenuRegistration = MenuRegistrar([] {
	return MenuDescriptor{
		.fill = [](not_null<Ui::PopupMenu*> menu, TabContext context) {
			const auto room = context.room;
			if (room->sample()
				|| !room->session()
				|| room->state().gone != Gone::No
				|| FeatureOff(room, "voice_link")) {
				return;
			}
			const auto extras = ExtrasFor(room);
			const auto show = context.show;
			if (room->state().voice.valid()) {
				menu->addAction(
					tr::lng_oblivion_rextra_voice_open(tr::now),
					[=] { extras->openVoice(show); },
					&st::menuIconVideoChat);
				if (room->owner()) {
					menu->addAction(
						tr::lng_oblivion_rextra_voice_detach(tr::now),
						[=] { extras->detachVoice(show); },
						&st::menuIconRemove);
				}
			} else if (room->owner()) {
				menu->addAction(
					tr::lng_oblivion_rextra_voice_create(tr::now),
					[=] { extras->createVoice(show); },
					&st::menuIconVideoChat);
				menu->addAction(
					tr::lng_oblivion_rextra_voice_attach(tr::now),
					[=] { extras->attachVoice(show); },
					&st::menuIconGroups);
			}
		},
	};
});

// ---- The self-test.

class Checker final {
public:
	explicit Checker(QStringList &log) : _log(log) {
	}

	void operator()(bool condition, const char *what) {
		if (condition) {
			++_passed;
			++_sectionPassed;
		} else {
			++_failed;
			++_sectionFailed;
			_log.push_back(u"FAILED: "_q + QString::fromUtf8(what));
		}
	}
	void section(const char *name) {
		_log.push_back(u"%1: %2 passed, %3 failed"_q.arg(
			QString::fromUtf8(name),
			QString::number(_sectionPassed),
			QString::number(_sectionFailed)));
		_sectionPassed = _sectionFailed = 0;
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
	int _sectionPassed = 0;
	int _sectionFailed = 0;

};

void TestLimits(Checker &check) {
	auto limiter = ReactionLimiter(3., 2.);
	auto now = crl::time(10'000);
	check(limiter.take(now) && limiter.take(now) && limiter.take(now),
		"the burst is allowed at once");
	check(!limiter.take(now), "then the bucket is empty");
	check(!limiter.take(now + 400), "less than one token has come");
	check(limiter.take(now + 520), "half a second is one token");
	check(!limiter.take(now + 530), "and only one");
	check(limiter.take(now + 1030), "the refill goes on");
	now += 60'000;
	auto taken = 0;
	for (auto i = 0; i != 10; ++i) {
		taken += limiter.take(now) ? 1 : 0;
	}
	check(taken == 3, "a long pause gives the burst, not more");
	check(!limiter.take(now - 5'000), "a clock that went back gives nothing");
	check(limiter.take(now + 500), "and does not break the refill");

	// A minute of clicking as fast as possible stays under what the
	// server allows (15 at once, 5 a second).
	auto own = ReactionLimiter(kOutgoingBurst, kOutgoingPerSecond);
	auto server = ReactionLimiter(15., 5.);
	auto refused = false;
	auto sent = 0;
	for (auto time = crl::time(0); time != 60'000; time += 20) {
		if (own.take(time)) {
			++sent;
			refused = refused || !server.take(time);
		}
	}
	check(!refused, "the own limit is inside the limit of the server");
	check(sent >= 180 && sent <= 190, "about three a second go out");

	auto guard = IncomingGuard(2., 1., 5., 100.);
	now = 1000;
	check(guard.accept(1, now) && guard.accept(1, now),
		"a member is shown up to the burst");
	check(!guard.accept(1, now), "a flooding member is cut");
	check(guard.accept(2, now) && guard.accept(2, now),
		"the others are still shown");
	check(guard.accept(3, now), "up to the limit of all");
	check(!guard.accept(4, now), "then nobody");
	check(guard.accept(1, now + 1000), "a second later the member is back");
	auto many = IncomingGuard(1., 0.001, 1000., 1000.);
	auto accepted = 0;
	for (auto user = uint64(1); user != 300; ++user) {
		accepted += many.accept(user, now) ? 1 : 0;
	}
	check(accepted == 299, "a lot of members do not break the guard");
}

void TestCodec(Checker &check) {
	check(CleanReactionEmoji(QString::fromUtf8("🔥"))
		== QString::fromUtf8("🔥"), "an emoji stays");
	check(CleanReactionEmoji(QString::fromUtf8("❤️"))
		== QString::fromUtf8("❤️"), "an emoji with a selector stays");
	check(CleanReactionEmoji(u"#️⃣"_q) == u"#️⃣"_q,
		"a keycap stays");
	check(CleanReactionEmoji(u"hello"_q).isEmpty(), "a word is dropped");
	check(CleanReactionEmoji(u"<b>"_q).isEmpty(), "markup is dropped");
	check(CleanReactionEmoji(QString::fromUtf8("🔥 🔥")).isEmpty(),
		"a space is dropped");
	check(CleanReactionEmoji(QString::fromUtf8("привет")).isEmpty(),
		"letters of any alphabet are dropped");
	check(CleanReactionEmoji(QString()).isEmpty()
		&& CleanReactionEmoji(QJsonValue(5)).isEmpty()
		&& CleanReactionEmoji(QJsonValue()).isEmpty(),
		"not a text: nothing");
	check(CleanReactionEmoji(QString(40, QChar(0x2764))).size() <= 16,
		"a long text is cut");

	auto sticker = RoomSticker();
	sticker.documentId = 5368324170671202286ULL;
	sticker.setId = 1234567890123456789ULL;
	sticker.setAccessHash = uint64(int64(-987654321012345678LL));
	sticker.setShortName = u"Animals_pack"_q;
	sticker.emoji = QString::fromUtf8("😀");
	sticker.format = u"animated"_q;
	const auto object = SerializeRoomSticker(sticker);
	check(object.value(u"kind"_q).toString() == u"sticker"_q
		&& object.value(u"doc_id"_q).toString() == u"5368324170671202286"_q
		&& object.value(u"set_id"_q).toString() == u"1234567890123456789"_q
		&& object.value(u"set_access_hash"_q).toString()
			== u"-987654321012345678"_q
		&& object.value(u"set_short_name"_q).toString() == u"Animals_pack"_q
		&& object.value(u"format"_q).toString() == u"animated"_q,
		"a sticker as the protocol wants it: ids are decimal texts");
	const auto parsed = ParseRoomSticker(object);
	check(parsed && *parsed == sticker, "a sticker round trip");

	auto custom = RoomSticker();
	custom.customEmoji = true;
	custom.documentId = 18446744073709551615ULL;
	custom.format = u"video"_q;
	const auto customObject = SerializeRoomSticker(custom);
	check(customObject.value(u"kind"_q).toString() == u"custom_emoji"_q
		&& customObject.value(u"set_id"_q).toString().isEmpty()
		&& customObject.value(u"set_access_hash"_q).toString().isEmpty(),
		"a custom emoji needs only its document");
	const auto customParsed = ParseRoomSticker(customObject);
	check(customParsed && *customParsed == custom,
		"a custom emoji round trip, the largest id");

	auto bad = object;
	bad.insert(u"kind"_q, u"gif"_q);
	check(!ParseRoomSticker(bad), "an unknown kind is dropped");
	bad = object;
	bad.insert(u"doc_id"_q, 123);
	check(!ParseRoomSticker(bad), "a number for an id is dropped");
	bad.insert(u"doc_id"_q, u"12a"_q);
	check(!ParseRoomSticker(bad), "an id with a letter is dropped");
	bad.insert(u"doc_id"_q, u"0"_q);
	check(!ParseRoomSticker(bad), "a zero id is dropped");
	bad.insert(u"doc_id"_q, u"99999999999999999999999"_q);
	check(!ParseRoomSticker(bad), "a too long id is dropped");
	bad.remove(u"doc_id"_q);
	check(!ParseRoomSticker(bad), "no id: dropped");
	bad = object;
	bad.insert(u"set_short_name"_q, u"../etc"_q);
	bad.insert(u"set_id"_q, u"x"_q);
	bad.insert(u"set_access_hash"_q, u"--5"_q);
	bad.insert(u"format"_q, u"exe"_q);
	bad.insert(u"emoji"_q, u"<script>"_q);
	const auto cleaned = ParseRoomSticker(bad);
	check(cleaned
		&& cleaned->setShortName.isEmpty()
		&& !cleaned->setId
		&& !cleaned->setAccessHash
		&& cleaned->format == u"static"_q
		&& cleaned->emoji.isEmpty()
		&& cleaned->documentId == sticker.documentId,
		"wrong fields are emptied, the sticker stays");
	check(!ParseRoomSticker(QJsonObject()), "an empty object is dropped");
}

void TestVoice(Checker &check) {
	check(VoiceInviteHash(u"https://t.me/+AbCdEf_12-xyz"_q)
		== u"AbCdEf_12-xyz"_q, "a + link");
	check(VoiceInviteHash(u"https://t.me/joinchat/AbCdEf_12-xyz"_q)
		== u"AbCdEf_12-xyz"_q, "a joinchat link");
	check(VoiceInviteHash(u"https://t.me/+AbCdEf_12-xyz?videochat=1"_q)
		== u"AbCdEf_12-xyz"_q, "a query is cut");
	check(VoiceInviteHash(u"https://t.me/+79991234567"_q).isEmpty(),
		"a phone number is not an invite");
	check(VoiceInviteHash(u"https://t.me/durov"_q).isEmpty(),
		"a public name is not an invite");
	check(VoiceInviteHash(u"https://t.me/+short"_q).isEmpty(),
		"a too short hash");
	check(VoiceInviteHash(u"https://t.me/+AbCd/../EfGh12345"_q).isEmpty(),
		"a path is not a hash");
	check(VoiceInviteHash(u"https://evil.example/+AbCdEf_12-xyz"_q).isEmpty()
		&& VoiceInviteHash(u"http://t.me/+AbCdEf_12-xyz"_q).isEmpty()
		&& VoiceInviteHash(u"https://t.me.evil.example/+AbCdEf_12"_q).isEmpty(),
		"only https://t.me/");
	check(VoiceInviteHash(QString()).isEmpty(), "nothing: nothing");
}

// ---- Snapshot scenes (OBLIVION_SELFTEST=ui).

constexpr auto kSampleSelf = uint64(9000000000000101ULL);
constexpr auto kSampleOwner = uint64(9000000000000100ULL);
constexpr auto kSampleThird = uint64(9000000000000102ULL);

[[nodiscard]] QString SampleText(const char *ru, const char *en) {
	return QString::fromUtf8(CurrentLanguageIsRussian() ? ru : en);
}

void SampleBurst(not_null<ReactionsOverlay*> overlay) {
	overlay->freeze();
	overlay->addSample(kSampleOwner, "🔥", 0.2, 0.16);
	overlay->addSample(kSampleOwner, "🔥", 0.34, 0.42);
	overlay->addSample(kSampleThird, "❤️", 0.5, 0.1);
	overlay->addSample(kSampleThird, "😂", 0.66, 0.3);
	overlay->addSample(kSampleSelf, "👏", 0.8, 0.22, true);
	overlay->addSample(kSampleOwner, "🎉", 0.6, 0.55);
	overlay->addSample(kSampleThird, "👍", 0.26, 0.66);
	overlay->addSample(kSampleSelf, "❤️", 0.44, 0.74, true);
	overlay->addSample(kSampleOwner, "😮", 0.76, 0.84);
	overlay->addSample(kSampleThird, "🔥", 0.14, 0.9);
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	const auto scene = [](
			const QString &name,
			QSize size,
			bool owner,
			bool panel,
			Fn<void(Room::Descriptor&)> adjust,
			Fn<void(not_null<ReactionsOverlay*>)> setup,
			const QString &tab = QString()) {
		RegisterScene(name, size, [=](not_null<Ui::RpWidget*> parent) {
			auto descriptor = SampleRoomDescriptor(owner);
			if (adjust) {
				adjust(descriptor);
			}
			OverlaySceneSetup = setup;
			ScenePanelOpen = panel;
			const auto result = CreateSampleRoomScene(
				parent,
				std::move(descriptor),
				tab);
			OverlaySceneSetup = nullptr;
			ScenePanelOpen = false;
			return result;
		});
	};
	const auto withVoice = [](Room::Descriptor &descriptor) {
		descriptor.state.voice.link = u"https://t.me/+AbCdEfGh1234"_q;
		descriptor.state.voice.title = SampleText(
			"Голос · Ночной эфир",
			"Voice · Night air");
		descriptor.state.voice.setBy = kSampleOwner;
	};
	const auto size = QSize(Scaled(460), Scaled(720));
	const auto narrow = QSize(Scaled(380), Scaled(520));
	scene(u"room_extras_reactions"_q, size, false, false, nullptr, SampleBurst);
	scene(u"room_extras_panel"_q, size, true, true, nullptr, [](
			not_null<ReactionsOverlay*> overlay) {
		overlay->freeze();
		overlay->addSample(kSampleSelf, "👍", 0.7, 0.12, true);
		overlay->addSample(kSampleOwner, "😂", 0.3, 0.4);
	});
	scene(u"room_extras_panel_narrow"_q, narrow, true, true, withVoice, nullptr);
	scene(u"room_extras_sticker"_q, size, false, false, nullptr, [](
			not_null<ReactionsOverlay*> overlay) {
		overlay->freeze();
		overlay->setSampleMoment(kSampleOwner, "🥳", 0.4);
		overlay->addSample(kSampleThird, "❤️", 0.24, 0.3);
	});
	scene(u"room_extras_sticker_chat"_q, size, true, false, nullptr, [](
			not_null<ReactionsOverlay*> overlay) {
		overlay->freeze();
		overlay->setSampleMoment(kSampleThird, "😎", 0.5);
	}, u"chat"_q);
	scene(u"room_extras_voice"_q, size, false, false, withVoice, nullptr);
	scene(u"room_extras_voice_owner"_q, narrow, true, false, withVoice, nullptr);

	const auto boxSize = QSize(st::boxWideWidth * 2, 0);
	RegisterBoxScene(u"room_extras_voice_create_box"_q, boxSize, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(VoiceCreateBox, VoiceCreateArgs{
			.groupTitle = VoiceGroupTitle(
				SampleText("Ночной эфир", "Night air")),
		});
	});
	RegisterBoxScene(u"room_extras_voice_attach_box"_q, boxSize, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(VoiceAttachBox, VoiceAttachArgs{
			.rows = {
				{
					.id = 11,
					.title = SampleText("Кино по пятницам", "Friday movies"),
					.members = 12,
				},
				{
					.id = 12,
					.title = SampleText("Соседи, 3 подъезд", "Neighbours"),
					.members = 48,
				},
				{
					.id = 13,
					.title = SampleText("Клуб настолок", "Board games club"),
					.members = 230,
					.open = true,
				},
				{
					.id = 14,
					.title = SampleText("Семья", "Family"),
					.members = 5,
				},
			},
		});
	});
	RegisterBoxScene(u"room_extras_voice_attach_empty"_q, boxSize, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(VoiceAttachBox, VoiceAttachArgs());
	});
});

} // namespace

ReactionLimiter::ReactionLimiter(double burst, double perSecond)
: _burst(std::max(burst, 1.))
, _rate(std::max(perSecond, 0.0001))
, _tokens(_burst) {
}

bool ReactionLimiter::take(crl::time now) {
	if (_started && now > _last) {
		_tokens = std::min(_burst, _tokens + (now - _last) * _rate / 1000.);
	}
	// A clock that went back must not give tokens for nothing.
	_last = _started ? std::max(_last, now) : now;
	_started = true;
	if (_tokens < 1.) {
		return false;
	}
	_tokens -= 1.;
	return true;
}

IncomingGuard::IncomingGuard(
	double userBurst,
	double userPerSecond,
	double allBurst,
	double allPerSecond)
: _userBurst(userBurst)
, _userRate(userPerSecond)
, _all(allBurst, allPerSecond) {
}

bool IncomingGuard::accept(uint64 userId, crl::time now) {
	auto i = _users.find(userId);
	if (i == end(_users)) {
		if (_users.size() >= 256) {
			// More than a room can hold: start over.
			_users.clear();
		}
		i = _users.emplace(
			userId,
			ReactionLimiter(_userBurst, _userRate)).first;
	}
	// The limit of a member is checked first: who floods does not take
	// the place of the others.
	return i->second.take(now) && _all.take(now);
}

QString CleanReactionEmoji(const QJsonValue &value) {
	const auto text = Cloud::JsonText(value, 16);
	if (text.isEmpty()) {
		return QString();
	}
	for (const auto ch : text) {
		const auto code = ch.unicode();
		const auto keycap = (code >= '0' && code <= '9')
			|| (code == '#')
			|| (code == '*');
		// The "information" sign and its neighbours are letters for
		// Unicode and emoji for everybody else.
		const auto letterlike = (code >= 0x2100 && code <= 0x214F);
		if ((ch.isLetter() && !letterlike)
			|| ch.isSpace()
			|| (code < 0x80 && !keycap)) {
			return QString();
		}
	}
	return text;
}

std::optional<RoomSticker> ParseRoomSticker(const QJsonObject &sticker) {
	const auto kind = sticker.value(u"kind"_q).toString();
	if (kind != u"sticker"_q && kind != u"custom_emoji"_q) {
		return std::nullopt;
	}
	auto result = RoomSticker();
	result.customEmoji = (kind == u"custom_emoji"_q);
	result.documentId = ParseDigits(sticker.value(u"doc_id"_q));
	if (!result.documentId) {
		return std::nullopt;
	}
	result.setId = ParseDigits(sticker.value(u"set_id"_q));
	result.setAccessHash = ParseSignedDigits(
		sticker.value(u"set_access_hash"_q));
	const auto name = sticker.value(u"set_short_name"_q).toString();
	if (!name.isEmpty() && name.size() <= 64 && OnlyOf(name, NameChar)) {
		result.setShortName = name;
	}
	result.emoji = CleanReactionEmoji(sticker.value(u"emoji"_q));
	const auto format = sticker.value(u"format"_q).toString();
	result.format = (format == u"animated"_q || format == u"video"_q)
		? format
		: u"static"_q;
	return result;
}

QJsonObject SerializeRoomSticker(const RoomSticker &sticker) {
	auto result = QJsonObject();
	result.insert(
		u"kind"_q,
		sticker.customEmoji ? u"custom_emoji"_q : u"sticker"_q);
	result.insert(u"doc_id"_q, QString::number(quint64(sticker.documentId)));
	result.insert(
		u"set_id"_q,
		sticker.setId ? QString::number(quint64(sticker.setId)) : QString());
	result.insert(
		u"set_access_hash"_q,
		(sticker.setAccessHash
			? QString::number(qint64(sticker.setAccessHash))
			: QString()));
	result.insert(u"set_short_name"_q, sticker.setShortName);
	result.insert(u"emoji"_q, sticker.emoji);
	result.insert(u"format"_q, sticker.format);
	return result;
}

QString VoiceInviteHash(const QString &link) {
	const auto prefix = u"https://t.me/"_q;
	if (!link.startsWith(prefix)) {
		return QString();
	}
	auto rest = link.mid(prefix.size());
	const auto query = rest.indexOf(QChar('?'));
	if (query >= 0) {
		rest = rest.left(query);
	}
	const auto hash = rest.startsWith(QChar('+'))
		? rest.mid(1)
		: rest.startsWith(u"joinchat/"_q)
		? rest.mid(9)
		: QString();
	if (hash.size() < 8
		|| hash.size() > 64
		|| !OnlyOf(hash, HashChar)
		|| OnlyOf(hash, DigitChar)) {
		return QString();
	}
	return hash;
}

bool RunExtrasSelfTest(QStringList &log) {
	auto check = Checker(log);
	TestLimits(check);
	check.section("reaction rate limits");
	TestCodec(check);
	check.section("reaction and sticker codec");
	TestVoice(check);
	check.section("voice links");
	log.push_back(u"room_extras: %1 checks, %2 failed"_q.arg(
		QString::number(check.passed() + check.failed()),
		QString::number(check.failed())));
	return !check.failed();
}

} // namespace Oblivion::Rooms
