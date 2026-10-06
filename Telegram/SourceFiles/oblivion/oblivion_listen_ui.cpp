/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_listen_ui.h"

#include "base/timer.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "oblivion/oblivion_listen.h"
#include "oblivion/oblivion_settings.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/boxes/confirm_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/ui_utility.h"
#include "window/window_session_controller.h"

#include "styles/style_calls.h"
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_media_player.h"
#include "styles/style_widgets.h"

namespace Oblivion::Listen {
namespace {

constexpr auto kEndedShown = crl::time(5000);
constexpr auto kSceneWidth = 520;
constexpr auto kSceneNarrowWidth = 380;

// The speaker is one of the icons of the volume button of the player,
// a box of 24px with about 3px of air around the drawing.
struct Metrics {
	int icon = 0;
	int iconSkip = 0;
	int speaker = 0;
	int speakerSkip = 0;
	int controlsSkip = 0;
	int minText = 0;
	int minTrack = 0;
};

[[nodiscard]] Metrics ComputeMetrics() {
	return {
		.icon = style::ConvertScale(22),
		.iconSkip = style::ConvertScale(9),
		.speaker = st::mediaPlayerVolumeToggle.icon.width(),
		.speakerSkip = style::ConvertScale(3),
		.controlsSkip = style::ConvertScale(14),
		.minText = style::ConvertScale(200),
		.minTrack = style::ConvertScale(48),
	};
}

// The same three steps as the volume button of the player has.
[[nodiscard]] not_null<const style::icon*> SpeakerIcon(float64 volume) {
	return (volume <= 0.)
		? &st::mediaPlayerVolumeIcon0
		: (volume < 0.66)
		? &st::mediaPlayerVolumeIcon1
		: &st::mediaPlayerVolumeToggle.icon;
}

// status is the track (or a whole phrase), state is what happens to it
// right now ("paused"), shown after it and never elided away by it.
// action is the second button, to the left of the main one: what the
// host may do about the state ("Send to chat").
struct BarContent {
	bool shown = false;
	bool ended = false;
	QString title;
	QString status;
	QString state;
	QString button;
	QString action;
	bool volume = false;

	friend inline bool operator==(
		const BarContent &,
		const BarContent &) = default;
};

void PaintHeadphones(QPainter &p, QRectF rect, const QColor &color) {
	auto hq = PainterHighQualityEnabler(p);
	const auto size = rect.width();
	const auto x = rect.x();
	const auto y = rect.y();

	auto pen = QPen(color, size * 0.085);
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	p.drawArc(
		QRectF(x + size * 0.16, y + size * 0.14, size * 0.68, size * 0.72),
		0,
		180 * 16);

	const auto cup = QSizeF(size * 0.2, size * 0.34);
	const auto radius = cup.width() * 0.4;
	const auto top = y + size * 0.5;
	p.setPen(Qt::NoPen);
	p.setBrush(color);
	p.drawRoundedRect(
		QRectF(QPointF(x + size * 0.08, top), cup),
		radius,
		radius);
	p.drawRoundedRect(
		QRectF(QPointF(x + size * 0.72, top), cup),
		radius,
		radius);
}

// The bar itself: only shows what it is given and says what was
// pressed, so it can be rendered without a session.
class Bar final : public Ui::RpWidget {
public:
	explicit Bar(QWidget *parent);

	void setContent(BarContent content);
	void setVolume(float64 value);

	[[nodiscard]] rpl::producer<> clicks() const;
	[[nodiscard]] rpl::producer<> actionClicks() const;
	[[nodiscard]] rpl::producer<float64> volumeChanges() const {
		return _volumeChanges.events();
	}
	[[nodiscard]] rpl::producer<float64> volumeSaves() const {
		return _volumeSaves.events();
	}

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;

private:
	void updateControlsGeometry(int outer);

	BarContent _content;
	rpl::variable<QString> _buttonText;
	rpl::variable<QString> _actionText;
	const not_null<Ui::RoundButton*> _button;
	const not_null<Ui::RoundButton*> _action;
	const not_null<Ui::MediaSlider*> _volume;
	rpl::event_stream<float64> _volumeChanges;
	rpl::event_stream<float64> _volumeSaves;
	not_null<const style::icon*> _speakerIcon;
	QRect _speaker;
	int _textRight = 0;

};

Bar::Bar(QWidget *parent)
: RpWidget(parent)
, _button(Ui::CreateChild<Ui::RoundButton>(
	this,
	_buttonText.value(),
	st::groupCallTopBarJoin))
, _action(Ui::CreateChild<Ui::RoundButton>(
	this,
	_actionText.value(),
	st::groupCallTopBarJoin))
, _volume(Ui::CreateChild<Ui::MediaSlider>(
	this,
	st::mediaPlayerPanelPlayback))
, _speakerIcon(SpeakerIcon(1.)) {
	resize(width(), st::historyReplyHeight);
	setAttribute(Qt::WA_OpaquePaintEvent);

	for (const auto button : { _button, _action }) {
		button->setFullRadius(true);
		button->hide();
		button->widthValue(
		) | rpl::on_next([=] {
			updateControlsGeometry(width());
		}, button->lifetime());
	}

	_volume->hide();
	_volume->setMoveByWheel(true);
	_volume->setChangeProgressCallback([=](float64 value) {
		_volumeChanges.fire_copy(value);
	});
	_volume->setChangeFinishedCallback([=](float64 value) {
		_volumeSaves.fire_copy(value);
	});
}

void Bar::setContent(BarContent content) {
	if (_content == content) {
		return;
	}
	_content = std::move(content);
	_buttonText = _content.button;
	_actionText = _content.action;
	_button->setVisible(!_content.button.isEmpty());
	_action->setVisible(!_content.action.isEmpty());
	updateControlsGeometry(width());
	update();
}

void Bar::setVolume(float64 value) {
	if (!_volume->isChanging()) {
		_volume->setValue(value);
	}
	const auto icon = SpeakerIcon(value);
	if (_speakerIcon != icon) {
		_speakerIcon = icon;
		update(_speaker);
	}
}

rpl::producer<> Bar::clicks() const {
	return _button->clicks() | rpl::to_empty;
}

rpl::producer<> Bar::actionClicks() const {
	return _action->clicks() | rpl::to_empty;
}

int Bar::resizeGetHeight(int newWidth) {
	updateControlsGeometry(newWidth);
	return st::historyReplyHeight;
}

void Bar::updateControlsGeometry(int outer) {
	const auto metrics = ComputeMetrics();
	const auto line = st::lineWidth;
	const auto inner = st::historyReplyHeight - 2 * line;
	const auto top = line + (inner - _button->height()) / 2;
	auto right = outer - top;
	if (!_content.button.isEmpty()) {
		_button->moveToRight(top, top, outer);
		right -= _button->width() + metrics.controlsSkip;
	}
	if (!_content.action.isEmpty()) {
		_action->moveToRight(outer - right, top, outer);
		right -= _action->width() + metrics.controlsSkip;
	}
	const auto textLeft = st::topBarArrowPadding.right()
		+ metrics.icon
		+ metrics.iconSkip;
	const auto sliderWidth = st::mediaPlayerPanelVolumeWidth;
	const auto volumeLeft = right
		- sliderWidth
		- metrics.speakerSkip
		- metrics.speaker;
	const auto volume = _content.volume
		&& (volumeLeft - metrics.controlsSkip - textLeft >= metrics.minText);
	if (volume) {
		_volume->setGeometry(
			right - sliderWidth,
			top,
			sliderWidth,
			_button->height());
		const auto speakerHeight = _speakerIcon->height();
		_speaker = QRect(
			volumeLeft,
			line + (inner - speakerHeight) / 2,
			metrics.speaker,
			speakerHeight);
		right = volumeLeft - metrics.controlsSkip;
	} else {
		_speaker = QRect();
	}
	_volume->setVisible(volume);
	if (_textRight != right) {
		_textRight = right;
		update();
	}
}

void Bar::paintEvent(QPaintEvent *e) {
	// The bars above draw their shadow over the first row of what is
	// below them, this one is the last and keeps both lines inside.
	auto p = Painter(this);
	const auto metrics = ComputeMetrics();
	const auto line = st::lineWidth;
	const auto inner = height() - 2 * line;
	p.fillRect(e->rect(), st::historyComposeAreaBg);
	p.fillRect(0, 0, width(), line, st::shadowFg);
	p.fillRect(0, height() - line, width(), line, st::shadowFg);

	const auto iconLeft = st::topBarArrowPadding.right();
	PaintHeadphones(
		p,
		QRectF(
			iconLeft,
			line + (inner - metrics.icon) / 2.,
			metrics.icon,
			metrics.icon),
		(_content.ended
			? st::historyStatusFg
			: st::defaultMessageBar.titleFg)->c);
	if (!_speaker.isEmpty()) {
		_speakerIcon->paint(
			p,
			_speaker.topLeft(),
			width(),
			st::historyStatusFg->c);
	}

	const auto left = iconLeft + metrics.icon + metrics.iconSkip;
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
	p.setPen(_content.ended
		? st::historyStatusFg
		: st::defaultMessageBar.titleFg);
	p.drawTextLeft(
		left,
		titleTop,
		width(),
		elided(titleFont, _content.title));

	// A long track name gives way to the state: "Artist – Tit… · paused"
	// and not "Artist – Title · pau…", the state is what changes.
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
		if (statusWidth + stateWidth <= available) {
			stateLeft += statusWidth;
		} else if (available - stateWidth >= metrics.minTrack) {
			status = statusFont->elided(status, available - stateWidth);
			stateLeft += statusFont->width(status);
		} else {
			status = QString();
			state = elided(statusFont, _content.state);
		}
	}
	p.setFont(statusFont);
	if (!status.isEmpty()) {
		p.setPen(_content.ended
			? st::historyStatusFg
			: st::defaultMessageBar.textFg);
		p.drawTextLeft(left, statusTop, width(), status);
	}
	if (!state.isEmpty()) {
		p.setPen(st::historyStatusFg);
		p.drawTextLeft(stateLeft, statusTop, width(), state);
	}
}

[[nodiscard]] QString TrackText(const ChatView &view) {
	return view.track.isEmpty()
		? tr::lng_oblivion_listen_bar_track_unknown(tr::now)
		: view.track;
}

struct StatusParts {
	QString text;
	QString state;
};

[[nodiscard]] StatusParts StatusFor(const ChatView &view) {
	using Sync = ChatView::Sync;

	const auto track = TrackText(view);
	if (view.sync == Sync::Away) {
		// The host sees the track that the chat does not have and what
		// is done about it, the others see why they hear nothing.
		if (view.kind != ChatView::Kind::Hosting) {
			return {
				.text = tr::lng_oblivion_listen_bar_host_away(tr::now),
			};
		}
		return {
			.text = track,
			.state = (view.sending
				? tr::lng_oblivion_listen_bar_sending(tr::now)
				: tr::lng_oblivion_listen_bar_away(tr::now)),
		};
	} else if (view.kind == ChatView::Kind::Joined) {
		switch (view.sync) {
		case Sync::Unavailable:
			return {
				.text = tr::lng_oblivion_listen_bar_unavailable(tr::now),
			};
		case Sync::Loading:
			return {
				.text = track,
				.state = tr::lng_oblivion_listen_bar_loading(tr::now),
			};
		case Sync::OwnPause:
			return {
				.text = track,
				.state = tr::lng_oblivion_listen_bar_own_pause(tr::now),
			};
		case Sync::Waiting:
			return {
				.text = tr::lng_oblivion_listen_bar_waiting(tr::now),
			};
		case Sync::Away:
		case Sync::Fine:
			break;
		}
	}
	return {
		.text = track,
		.state = (view.playing
			? QString()
			: tr::lng_oblivion_listen_bar_paused(tr::now)),
	};
}

[[nodiscard]] BarContent ContentFor(const ChatView &view) {
	using Kind = ChatView::Kind;

	auto status = StatusFor(view);
	switch (view.kind) {
	case Kind::None:
		break;
	case Kind::Offer:
		return {
			.shown = true,
			.title = (view.host.isEmpty()
				? tr::lng_oblivion_listen_bar_title(tr::now)
				: tr::lng_oblivion_listen_bar_title_offer(
					tr::now,
					lt_name,
					view.host)),
			.status = std::move(status.text),
			.state = std::move(status.state),
			.button = tr::lng_oblivion_listen_join(tr::now),
		};
	case Kind::Joined:
		return {
			.shown = true,
			.title = (view.host.isEmpty()
				? tr::lng_oblivion_listen_bar_title(tr::now)
				: tr::lng_oblivion_listen_bar_title_joined(
					tr::now,
					lt_name,
					view.host)),
			.status = std::move(status.text),
			.state = std::move(status.state),
			.button = tr::lng_oblivion_listen_leave(tr::now),
			.volume = true,
		};
	case Kind::Hosting:
		return {
			.shown = true,
			.title = tr::lng_oblivion_listen_bar_title_host(tr::now),
			.status = std::move(status.text),
			.state = std::move(status.state),
			.button = tr::lng_oblivion_listen_end(tr::now),
			.action = (view.canSend
				? tr::lng_oblivion_listen_send(tr::now)
				: QString()),
		};
	case Kind::Starting:
		return {
			.shown = true,
			.title = tr::lng_oblivion_listen_bar_title_starting(tr::now),
			.status = TrackText(view),
			.button = tr::lng_cancel(tr::now),
		};
	}
	return {};
}

[[nodiscard]] BarContent EndedContent(const ChatView &last) {
	return {
		.shown = true,
		.ended = true,
		.title = tr::lng_oblivion_listen_bar_title_ended(tr::now),
		.status = TrackText(last),
	};
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;
	using Kind = ChatView::Kind;
	using Sync = ChatView::Sync;

	// The narrow ones are a chat column of the minimal width: the volume
	// gives its place to the text there and both lines are elided.
	const auto bar = [](
			const QString &name,
			BarContent content,
			float64 volume,
			int width = kSceneWidth) {
		RegisterScene(
			name,
			QSize(style::ConvertScale(width), 0),
			[=](not_null<Ui::RpWidget*> parent) {
				const auto result = Ui::CreateChild<Bar>(parent.get());
				result->setContent(content);
				result->setVolume(volume);
				return result;
			});
	};
	const auto track = u"Кино – Звезда по имени Солнце"_q;
	const auto longTrack = u"Pink Floyd – Shine On You Crazy Diamond"_q
		+ u" (Parts I–V), 2011 Remaster"_q;
	const auto host = u"Марина"_q;
	const auto longHost = u"Александр Константинопольский"_q;
	bar(u"listen_bar_host"_q, ContentFor(ChatView{
		.kind = Kind::Hosting,
		.track = track,
		.playing = true,
	}), 0.8);
	bar(u"listen_bar_host_paused"_q, ContentFor(ChatView{
		.kind = Kind::Hosting,
		.track = track,
	}), 0.8);
	bar(u"listen_bar_host_starting"_q, ContentFor(ChatView{
		.kind = Kind::Starting,
		.track = track,
	}), 0.8);
	bar(u"listen_bar_offer"_q, ContentFor(ChatView{
		.kind = Kind::Offer,
		.track = track,
		.host = host,
		.playing = true,
	}), 0.8);
	bar(u"listen_bar_offer_paused"_q, ContentFor(ChatView{
		.kind = Kind::Offer,
		.host = host,
	}), 0.8);
	bar(u"listen_bar_joined"_q, ContentFor(ChatView{
		.kind = Kind::Joined,
		.track = track,
		.host = host,
		.playing = true,
	}), 0.8);
	bar(u"listen_bar_joined_loading"_q, ContentFor(ChatView{
		.kind = Kind::Joined,
		.sync = Sync::Loading,
		.track = track,
		.host = host,
		.playing = true,
	}), 0.4);
	bar(u"listen_bar_joined_own_pause"_q, ContentFor(ChatView{
		.kind = Kind::Joined,
		.sync = Sync::OwnPause,
		.track = longTrack,
		.host = longHost,
		.playing = true,
	}), 0.);
	bar(u"listen_bar_joined_waiting"_q, ContentFor(ChatView{
		.kind = Kind::Joined,
		.sync = Sync::Waiting,
		.track = track,
		.host = host,
		.playing = true,
	}), 0.8);
	bar(u"listen_bar_joined_unavailable"_q, ContentFor(ChatView{
		.kind = Kind::Joined,
		.sync = Sync::Unavailable,
		.track = track,
		.host = host,
		.playing = true,
	}), 0.8);
	bar(u"listen_bar_ended"_q, EndedContent(ChatView{
		.kind = Kind::Joined,
		.track = track,
		.host = host,
	}), 0.8);
	bar(u"listen_bar_offer_narrow"_q, ContentFor(ChatView{
		.kind = Kind::Offer,
		.track = longTrack,
		.host = longHost,
	}), 0.8, kSceneNarrowWidth);
	bar(u"listen_bar_joined_narrow"_q, ContentFor(ChatView{
		.kind = Kind::Joined,
		.sync = Sync::Loading,
		.track = track,
		.host = host,
		.playing = true,
	}), 0.8, kSceneNarrowWidth);

	// The host plays a track that is not a message of the chat.
	bar(u"listen_bar_host_away"_q, ContentFor(ChatView{
		.kind = Kind::Hosting,
		.sync = Sync::Away,
		.track = track,
		.canSend = true,
	}), 0.8);
	bar(u"listen_bar_host_away_sending"_q, ContentFor(ChatView{
		.kind = Kind::Hosting,
		.sync = Sync::Away,
		.track = track,
		.sending = true,
	}), 0.8);
	bar(u"listen_bar_host_away_locked"_q, ContentFor(ChatView{
		.kind = Kind::Hosting,
		.sync = Sync::Away,
		.track = longTrack,
	}), 0.8);
	bar(u"listen_bar_host_away_narrow"_q, ContentFor(ChatView{
		.kind = Kind::Hosting,
		.sync = Sync::Away,
		.track = longTrack,
		.canSend = true,
	}), 0.8, kSceneNarrowWidth);
	bar(u"listen_bar_joined_away"_q, ContentFor(ChatView{
		.kind = Kind::Joined,
		.sync = Sync::Away,
		.track = track,
		.host = host,
	}), 0.8);
	bar(u"listen_bar_offer_away"_q, ContentFor(ChatView{
		.kind = Kind::Offer,
		.sync = Sync::Away,
		.track = track,
		.host = host,
	}), 0.8);

	const auto box = [](
			const QString &name,
			const QString &chatName,
			const QString &trackName,
			bool sendsTrack,
			bool onlineNote = false) {
		RegisterBoxScene(name, QSize(st::boxWideWidth * 2, 0), [=](
				std::shared_ptr<Ui::Show>) {
			return MakeStartBox({
				.chat = chatName,
				.track = trackName,
				.sendsTrack = sendsTrack,
				.onlineNote = onlineNote,
			});
		});
	};
	box(u"listen_start_box"_q, u"Друзья"_q, track, false);
	box(u"listen_start_box_sends_track"_q, u"Друзья"_q, track, true);
	box(
		u"listen_start_box_long"_q,
		u"Клуб любителей винила и хорошего звука"_q,
		longTrack,
		true,
		true);
});

} // namespace

object_ptr<Ui::RpWidget> CreateBar(
		not_null<QWidget*> parent,
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	using Kind = ChatView::Kind;

	if (!Get().listenTogether() || !ChatFits(peer)) {
		return { nullptr };
	}
	struct State {
		ChatView view;
		base::Timer timer;
		bool ended = false;
	};
	auto result = object_ptr<Ui::SlideWrap<Bar>>(
		parent,
		object_ptr<Bar>(parent));
	const auto raw = result.data();
	const auto bar = raw->entity();
	const auto state = raw->lifetime().make_state<State>();
	raw->hide(anim::type::instant);

	const auto refresh = [=](anim::type animated) {
		auto view = Lookup(peer);
		if (view.kind != Kind::None) {
			state->ended = false;
			if (view.staleIn > 0) {
				state->timer.callOnce(view.staleIn);
			} else {
				state->timer.cancel();
			}
			bar->setContent(ContentFor(view));
			state->view = std::move(view);
			raw->toggle(true, animated);
		} else if (state->view.kind == Kind::Starting) {
			state->view = ChatView();
			state->timer.cancel();
			raw->toggle(false, animated);
		} else if (state->view.kind != Kind::None) {
			state->ended = true;
			bar->setContent(EndedContent(state->view));
			state->view = ChatView();
			state->timer.callOnce(kEndedShown);
			raw->toggle(true, animated);
		} else if (!state->ended) {
			raw->toggle(false, animated);
		}
	};
	state->timer.setCallback([=] {
		if (state->ended) {
			state->ended = false;
			raw->toggle(false, anim::type::normal);
		} else {
			refresh(anim::type::normal);
		}
	});

	Changes(
	) | rpl::on_next([=] {
		refresh(anim::type::normal);
	}, raw->lifetime());

	peer->owner().historyChanged(
	) | rpl::filter([=](not_null<History*> history) {
		return (history->peer == peer);
	}) | rpl::on_next([=] {
		if (Rescan(peer)) {
			refresh(anim::type::normal);
		}
	}, raw->lifetime());

	bar->clicks(
	) | rpl::on_next([=] {
		switch (state->view.kind) {
		case Kind::Offer: Join(controller, peer); break;
		case Kind::Joined: Leave(); break;
		case Kind::Hosting:
		case Kind::Starting: End(); break;
		case Kind::None: break;
		}
	}, raw->lifetime());

	bar->actionClicks(
	) | rpl::on_next([=] {
		if (state->view.kind == Kind::Hosting && state->view.canSend) {
			SendTrack(state->view.sendId);
		}
	}, raw->lifetime());

	const auto settings = &Core::App().settings();
	const auto applyVolume = [=](float64 value) {
		if (value != settings->songVolume()) {
			Media::Player::mixer()->setSongVolume(value);
			settings->setSongVolume(value);
		}
	};
	bar->setVolume(settings->songVolume());
	settings->songVolumeChanges(
	) | rpl::on_next([=](float64 value) {
		bar->setVolume(value);
	}, raw->lifetime());
	bar->volumeChanges(
	) | rpl::on_next(applyVolume, raw->lifetime());
	bar->volumeSaves(
	) | rpl::on_next([=](float64 value) {
		if (value > 0.) {
			settings->setRememberedSongVolume(value);
		}
		applyVolume(value);
		Core::App().saveSettingsDelayed();
	}, raw->lifetime());

	[[maybe_unused]] const auto found = Rescan(peer);
	refresh(anim::type::instant);
	return result;
}

object_ptr<Ui::BoxContent> MakeStartBox(StartBoxArgs &&args) {
	auto text = tr::lng_oblivion_listen_start_text(
		tr::now,
		lt_track,
		tr::bold(args.track),
		lt_chat,
		tr::bold(args.chat),
		tr::marked);
	text.append(u"\n\n"_q).append(
		tr::lng_oblivion_listen_start_text_about(tr::now));
	if (args.sendsTrack) {
		text.append(u"\n\n"_q).append(
			tr::lng_oblivion_listen_start_text_send(tr::now));
	}
	if (args.onlineNote) {
		text.append(u"\n\n"_q).append(
			tr::lng_oblivion_listen_start_text_online(tr::now));
	}
	return Ui::MakeConfirmBox({
		.text = std::move(text),
		.confirmed = [confirmed = std::move(args.confirmed)](
				Fn<void()> close) {
			close();
			if (confirmed) {
				confirmed();
			}
		},
		.confirmText = (args.sendsTrack
			? tr::lng_oblivion_listen_start_button_send()
			: tr::lng_oblivion_listen_start_button()),
		.title = tr::lng_oblivion_listen_start_title(),
	});
}

} // namespace Oblivion::Listen
