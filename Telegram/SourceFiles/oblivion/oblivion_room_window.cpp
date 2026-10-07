/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_room_window.h"

#include "base/timer.h"
#include "base/unique_qptr.h"
#include "core/application.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "oblivion/oblivion_cloud_ui.h"
#include "oblivion/oblivion_lang.h"
#include "oblivion/oblivion_room_music.h"
#include "oblivion/oblivion_settings.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "settings/settings_common.h"
#include "ui/boxes/confirm_box.h"
#include "ui/empty_userpic.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/layer_manager.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/text/text.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/rp_window.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"
#include "styles/style_window.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QPointer>
#include <QtGui/QClipboard>
#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>
#include <QtGui/QScreen>

namespace Oblivion::Rooms {
namespace {

constexpr auto kWindowWidth = 460;
constexpr auto kWindowHeight = 720;
constexpr auto kWindowMinWidth = 380;
constexpr auto kWindowMinHeight = 520;
constexpr auto kTitleLimit = 64;
constexpr auto kChatLimit = 2000;
constexpr auto kStripLimit = 5;
constexpr auto kBuiltinMusic = 100;
constexpr auto kBuiltinChat = 400;
constexpr auto kBuiltinMembers = 500;

[[nodiscard]] int Scaled(int value) {
	return style::ConvertScale(value);
}

void Toast(const std::shared_ptr<Ui::Show> &show, const QString &text) {
	if (show && show->valid() && !text.isEmpty()) {
		show->showToast(text);
	}
}

[[nodiscard]] const style::font &TitleFont() {
	static const auto result = style::font(
		Scaled(17),
		st::semiboldFont->flags(),
		st::semiboldFont->family());
	return result;
}

[[nodiscard]] QString RoomErrorText(const Cloud::Error &error) {
	const auto limit = error.detail("limit");
	return error.is("room_full")
		? tr::lng_oblivion_room_join_full(tr::now)
		: error.is("room_banned")
		? tr::lng_oblivion_room_join_banned(tr::now)
		: error.is("invite_required")
		? tr::lng_oblivion_room_join_invite(tr::now)
		: error.is("empty_queue")
		? tr::lng_oblivion_room_error_empty_queue(tr::now)
		: error.is("feature_disabled")
		? tr::lng_oblivion_room_error_unavailable(tr::now)
		: (error.is("limit_reached") && limit == u"rooms_owned"_q)
		? tr::lng_oblivion_room_error_limit_owned(tr::now)
		: (error.is("limit_reached") && limit == u"rooms_joined"_q)
		? tr::lng_oblivion_room_error_limit_joined(tr::now)
		: (error.is("limit_reached") && limit == u"queue"_q)
		? tr::lng_oblivion_room_error_queue_full(tr::now)
		: (error.is("limit_reached") && limit == u"room_quota"_q)
		? tr::lng_oblivion_room_error_quota(tr::now)
		: (error.is("forbidden") && !error.detail("right").isEmpty())
		? tr::lng_oblivion_room_error_right(tr::now)
		: Cloud::ErrorText(error);
}

void ShowRoomError(
		const std::shared_ptr<Ui::Show> &show,
		const Cloud::Error &error) {
	if (error.type != Cloud::Error::Type::Cancelled) {
		Toast(show, RoomErrorText(error));
	}
}

[[nodiscard]] QString RightLabel(Right right) {
	switch (right) {
	case Right::Control: return tr::lng_oblivion_room_right_control(tr::now);
	case Right::Queue: return tr::lng_oblivion_room_right_queue(tr::now);
	case Right::Add: return tr::lng_oblivion_room_right_add(tr::now);
	case Right::Draw: return tr::lng_oblivion_room_right_draw(tr::now);
	case Right::Chat: return tr::lng_oblivion_room_right_chat(tr::now);
	case Right::Invite: return tr::lng_oblivion_room_right_invite(tr::now);
	}
	return QString();
}

[[nodiscard]] QString GoneText(Gone gone) {
	switch (gone) {
	case Gone::Kicked: return tr::lng_oblivion_room_gone_kicked(tr::now);
	case Gone::Banned: return tr::lng_oblivion_room_gone_banned(tr::now);
	case Gone::Closed: return tr::lng_oblivion_room_gone_closed(tr::now);
	case Gone::Idle: return tr::lng_oblivion_room_gone_idle(tr::now);
	case Gone::Admin: return tr::lng_oblivion_room_gone_admin(tr::now);
	case Gone::Rejected: return tr::lng_oblivion_room_gone_rejected(tr::now);
	case Gone::Left:
	case Gone::No: break;
	}
	return QString();
}

[[nodiscard]] QString MembersText(int count) {
	return tr::lng_oblivion_room_members_count(tr::now, lt_count, count);
}

// ---- The registries.

template <typename Descriptor>
[[nodiscard]] std::vector<Fn<Descriptor()>> &Makers() {
	static auto result = std::vector<Fn<Descriptor()>>();
	return result;
}

[[nodiscard]] object_ptr<Ui::RpWidget> CreateChatTab(
	QWidget *parent,
	TabContext context);
[[nodiscard]] object_ptr<Ui::RpWidget> CreateMembersTab(
	QWidget *parent,
	TabContext context);

[[nodiscard]] std::vector<TabDescriptor> CollectTabs() {
	auto result = std::vector<TabDescriptor>();
	result.push_back({
		.id = u"music"_q,
		.order = kBuiltinMusic,
		.title = [] { return tr::lng_oblivion_rmusic_tab(tr::now); },
		.create = [](QWidget *parent, TabContext context) {
			return CreateMusicTab(parent, context.room, context.show);
		},
	});
	result.push_back({
		.id = u"chat"_q,
		.order = kBuiltinChat,
		.title = [] { return tr::lng_oblivion_room_tab_chat(tr::now); },
		.create = CreateChatTab,
	});
	result.push_back({
		.id = u"members"_q,
		.order = kBuiltinMembers,
		.title = [] { return tr::lng_oblivion_room_tab_members(tr::now); },
		.create = CreateMembersTab,
	});
	for (const auto &make : Makers<TabDescriptor>()) {
		auto descriptor = make ? make() : TabDescriptor();
		if (!descriptor.id.isEmpty()
			&& descriptor.title
			&& descriptor.create
			&& !ranges::contains(result, descriptor.id, &TabDescriptor::id)) {
			result.push_back(std::move(descriptor));
		}
	}
	ranges::stable_sort(result, ranges::less(), &TabDescriptor::order);
	return result;
}

} // namespace

TabRegistrar::TabRegistrar(Fn<TabDescriptor()> make) {
	Makers<TabDescriptor>().push_back(std::move(make));
}

OverlayRegistrar::OverlayRegistrar(Fn<OverlayDescriptor()> make) {
	Makers<OverlayDescriptor>().push_back(std::move(make));
}

HeaderButtonRegistrar::HeaderButtonRegistrar(
		Fn<HeaderButtonDescriptor()> make) {
	Makers<HeaderButtonDescriptor>().push_back(std::move(make));
}

MenuRegistrar::MenuRegistrar(Fn<MenuDescriptor()> make) {
	Makers<MenuDescriptor>().push_back(std::move(make));
}

// ---- The painting kit.

void PaintGlyph(
		QPainter &p,
		Glyph glyph,
		QRectF rect,
		const QColor &color) {
	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	const auto s = std::min(rect.width(), rect.height());
	const auto x = rect.x() + (rect.width() - s) / 2.;
	const auto y = rect.y() + (rect.height() - s) / 2.;
	const auto at = [&](double fx, double fy) {
		return QPointF(x + s * fx, y + s * fy);
	};
	auto stroke = QPen(color, s * 0.1);
	stroke.setCapStyle(Qt::RoundCap);
	stroke.setJoinStyle(Qt::RoundJoin);
	const auto filled = [&] {
		p.setPen(Qt::NoPen);
		p.setBrush(color);
	};
	const auto outlined = [&] {
		p.setPen(stroke);
		p.setBrush(Qt::NoBrush);
	};
	const auto triangle = [&](QPointF a, QPointF b, QPointF c) {
		auto path = QPainterPath();
		path.moveTo(a);
		path.lineTo(b);
		path.lineTo(c);
		path.closeSubpath();
		auto pen = QPen(color, s * 0.08);
		pen.setJoinStyle(Qt::RoundJoin);
		p.setPen(pen);
		p.setBrush(color);
		p.drawPath(path);
	};
	switch (glyph) {
	case Glyph::Play:
		triangle(at(0.3, 0.2), at(0.3, 0.8), at(0.8, 0.5));
		break;
	case Glyph::Pause:
		filled();
		p.drawRoundedRect(
			QRectF(at(0.24, 0.2), at(0.42, 0.8)),
			s * 0.05,
			s * 0.05);
		p.drawRoundedRect(
			QRectF(at(0.58, 0.2), at(0.76, 0.8)),
			s * 0.05,
			s * 0.05);
		break;
	case Glyph::Previous:
		triangle(at(0.78, 0.24), at(0.78, 0.76), at(0.36, 0.5));
		filled();
		p.drawRoundedRect(
			QRectF(at(0.2, 0.22), at(0.3, 0.78)),
			s * 0.04,
			s * 0.04);
		break;
	case Glyph::Next:
		triangle(at(0.22, 0.24), at(0.22, 0.76), at(0.64, 0.5));
		filled();
		p.drawRoundedRect(
			QRectF(at(0.7, 0.22), at(0.8, 0.78)),
			s * 0.04,
			s * 0.04);
		break;
	case Glyph::Repeat:
	case Glyph::RepeatOne: {
		outlined();
		auto top = QPainterPath();
		top.moveTo(at(0.18, 0.5));
		top.lineTo(at(0.18, 0.42));
		top.quadTo(at(0.18, 0.3), at(0.3, 0.3));
		top.lineTo(at(0.78, 0.3));
		p.drawPath(top);
		auto bottom = QPainterPath();
		bottom.moveTo(at(0.82, 0.5));
		bottom.lineTo(at(0.82, 0.58));
		bottom.quadTo(at(0.82, 0.7), at(0.7, 0.7));
		bottom.lineTo(at(0.22, 0.7));
		p.drawPath(bottom);
		p.drawLine(at(0.68, 0.19), at(0.79, 0.3));
		p.drawLine(at(0.68, 0.41), at(0.79, 0.3));
		p.drawLine(at(0.32, 0.59), at(0.21, 0.7));
		p.drawLine(at(0.32, 0.81), at(0.21, 0.7));
		if (glyph == Glyph::RepeatOne) {
			p.drawLine(at(0.5, 0.42), at(0.5, 0.6));
			p.drawLine(at(0.45, 0.45), at(0.5, 0.42));
		}
	} break;
	case Glyph::Plus:
		outlined();
		p.drawLine(at(0.5, 0.22), at(0.5, 0.78));
		p.drawLine(at(0.22, 0.5), at(0.78, 0.5));
		break;
	case Glyph::More:
		filled();
		for (const auto fx : { 0.24, 0.5, 0.76 }) {
			p.drawEllipse(at(fx, 0.5), s * 0.07, s * 0.07);
		}
		break;
	case Glyph::Volume:
	case Glyph::Mute: {
		auto body = QPainterPath();
		body.moveTo(at(0.14, 0.4));
		body.lineTo(at(0.28, 0.4));
		body.lineTo(at(0.46, 0.24));
		body.lineTo(at(0.46, 0.76));
		body.lineTo(at(0.28, 0.6));
		body.lineTo(at(0.14, 0.6));
		body.closeSubpath();
		auto pen = QPen(color, s * 0.06);
		pen.setJoinStyle(Qt::RoundJoin);
		p.setPen(pen);
		p.setBrush(color);
		p.drawPath(body);
		outlined();
		if (glyph == Glyph::Volume) {
			p.drawArc(
				QRectF(at(0.44, 0.34), at(0.66, 0.66)),
				-60 * 16,
				120 * 16);
			p.drawArc(
				QRectF(at(0.42, 0.2), at(0.84, 0.8)),
				-55 * 16,
				110 * 16);
		} else {
			p.drawLine(at(0.6, 0.38), at(0.84, 0.62));
			p.drawLine(at(0.84, 0.38), at(0.6, 0.62));
		}
	} break;
	case Glyph::Link: {
		outlined();
		p.translate(at(0.5, 0.5));
		p.rotate(-45.);
		const auto w = s * 0.34;
		const auto h = s * 0.2;
		p.drawRoundedRect(
			QRectF(-w - s * 0.02, -h / 2., w, h),
			h / 2.,
			h / 2.);
		p.drawRoundedRect(QRectF(s * 0.02, -h / 2., w, h), h / 2., h / 2.);
		p.drawLine(QPointF(-s * 0.12, 0.), QPointF(s * 0.12, 0.));
	} break;
	case Glyph::Cross:
		outlined();
		p.drawLine(at(0.28, 0.28), at(0.72, 0.72));
		p.drawLine(at(0.72, 0.28), at(0.28, 0.72));
		break;
	case Glyph::Send: {
		auto path = QPainterPath();
		path.moveTo(at(0.18, 0.2));
		path.lineTo(at(0.86, 0.5));
		path.lineTo(at(0.18, 0.8));
		path.lineTo(at(0.3, 0.5));
		path.closeSubpath();
		auto pen = QPen(color, s * 0.06);
		pen.setJoinStyle(Qt::RoundJoin);
		p.setPen(pen);
		p.setBrush(color);
		p.drawPath(path);
	} break;
	case Glyph::Note: {
		filled();
		p.drawEllipse(at(0.34, 0.72), s * 0.13, s * 0.1);
		p.drawEllipse(at(0.7, 0.62), s * 0.13, s * 0.1);
		outlined();
		p.drawLine(at(0.44, 0.7), at(0.44, 0.26));
		p.drawLine(at(0.8, 0.6), at(0.8, 0.18));
		p.drawLine(at(0.44, 0.26), at(0.8, 0.18));
	} break;
	case Glyph::Crown: {
		auto path = QPainterPath();
		path.moveTo(at(0.16, 0.74));
		path.lineTo(at(0.12, 0.3));
		path.lineTo(at(0.34, 0.5));
		path.lineTo(at(0.5, 0.22));
		path.lineTo(at(0.66, 0.5));
		path.lineTo(at(0.88, 0.3));
		path.lineTo(at(0.84, 0.74));
		path.closeSubpath();
		auto pen = QPen(color, s * 0.06);
		pen.setJoinStyle(Qt::RoundJoin);
		p.setPen(pen);
		p.setBrush(color);
		p.drawPath(path);
	} break;
	case Glyph::Back:
		outlined();
		p.drawLine(at(0.58, 0.24), at(0.34, 0.5));
		p.drawLine(at(0.34, 0.5), at(0.58, 0.76));
		break;
	}
	p.restore();
}

void PaintUserpic(
		QPainter &p,
		QRect rect,
		uint64 userId,
		const QString &name) {
	const auto userpic = Ui::EmptyUserpic(
		Ui::EmptyUserpic::UserpicColor(Ui::EmptyUserpic::ColorIndex(userId)),
		name.isEmpty() ? u"?"_q : name);
	userpic.paintCircle(
		p,
		rect.x(),
		rect.y(),
		rect.x() + rect.width(),
		rect.width());
}

void PaintCover(
		QPainter &p,
		QRect rect,
		const QImage &cover,
		const QString &seed,
		int radius) {
	if (rect.isEmpty()) {
		return;
	}
	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	auto clip = QPainterPath();
	clip.addRoundedRect(QRectF(rect), radius, radius);
	p.setClipPath(clip, Qt::IntersectClip);
	if (!cover.isNull()) {
		const auto side = std::min(cover.width(), cover.height());
		p.drawImage(
			QRectF(rect),
			cover,
			QRectF(
				(cover.width() - side) / 2.,
				(cover.height() - side) / 2.,
				side,
				side));
	} else if (seed.isEmpty()) {
		p.fillRect(rect, st::windowBgOver);
		PaintGlyph(
			p,
			Glyph::Note,
			QRectF(rect).marginsRemoved(QMarginsF(
				rect.width() * 0.26,
				rect.height() * 0.26,
				rect.width() * 0.26,
				rect.height() * 0.26)),
			st::windowSubTextFg->c);
	} else {
		const auto hash = uint(qHash(seed));
		const auto hue = int(hash % 360);
		auto gradient = QLinearGradient(rect.topLeft(), rect.bottomRight());
		gradient.setColorAt(0., QColor::fromHsl(hue, 140, 128));
		gradient.setColorAt(1., QColor::fromHsl((hue + 50) % 360, 150, 82));
		p.fillRect(rect, gradient);
		PaintGlyph(
			p,
			Glyph::Note,
			QRectF(rect).marginsRemoved(QMarginsF(
				rect.width() * 0.26,
				rect.height() * 0.26,
				rect.width() * 0.26,
				rect.height() * 0.26)),
			QColor(255, 255, 255, 230));
	}
	p.restore();
}

GlyphButton::GlyphButton(QWidget *parent, Glyph glyph, int size, bool accent)
: AbstractButton(parent)
, _glyph(glyph)
, _accent(accent) {
	resize(size, size);
}

void GlyphButton::setGlyph(Glyph glyph) {
	if (_glyph != glyph) {
		_glyph = glyph;
		update();
	}
}

void GlyphButton::setHighlighted(bool highlighted) {
	if (_highlighted != highlighted) {
		_highlighted = highlighted;
		update();
	}
}

void GlyphButton::setDimmed(bool dimmed) {
	if (_dimmed != dimmed) {
		_dimmed = dimmed;
		update();
	}
}

void GlyphButton::onStateChanged(State was, StateChangeSource source) {
	update();
}

void GlyphButton::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto full = QRectF(rect());
	p.setOpacity(_dimmed ? 0.45 : 1.);
	if (_accent) {
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgActive);
		p.drawEllipse(full);
		if (isOver() || isDown()) {
			p.setBrush(QColor(255, 255, 255, isDown() ? 50 : 28));
			p.drawEllipse(full);
		}
	} else if (isOver() || isDown()) {
		p.setPen(Qt::NoPen);
		p.setBrush(isDown() ? st::windowBgRipple : st::windowBgOver);
		p.drawEllipse(full);
	}
	const auto skip = full.width() * (_accent ? 0.25 : 0.2);
	PaintGlyph(
		p,
		_glyph,
		full.marginsRemoved(QMarginsF(skip, skip, skip, skip)),
		_accent
			? st::windowFgActive->c
			: _highlighted
			? st::windowActiveTextFg->c
			: st::windowFg->c);
}

namespace {

// ---- The header: title, state, members strip, buttons.

class Header final : public Ui::RpWidget {
public:
	Header(QWidget *parent, TabContext context);

	[[nodiscard]] rpl::producer<> membersClicks() const {
		return _membersClicks.events();
	}
	[[nodiscard]] rpl::producer<> menuClicks() const {
		return _more->clicks() | rpl::to_empty;
	}
	[[nodiscard]] not_null<QWidget*> menuButton() const {
		return _more;
	}

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;

private:
	void updateLayout();
	[[nodiscard]] std::vector<const Member*> stripMembers() const;

	const not_null<Room*> _room;
	const std::shared_ptr<Ui::Show> _show;
	const not_null<GlyphButton*> _link;
	const not_null<GlyphButton*> _more;
	std::vector<base::unique_qptr<Ui::RpWidget>> _extra;
	rpl::event_stream<> _membersClicks;
	QRect _strip;
	int _textRight = 0;

};

Header::Header(QWidget *parent, TabContext context)
: RpWidget(parent)
, _room(context.room)
, _show(context.show)
, _link(Ui::CreateChild<GlyphButton>(this, Glyph::Link, Scaled(34)))
, _more(Ui::CreateChild<GlyphButton>(this, Glyph::More, Scaled(34))) {
	setMouseTracking(true);

	auto makers = Makers<HeaderButtonDescriptor>();
	auto descriptors = std::vector<HeaderButtonDescriptor>();
	for (const auto &make : makers) {
		auto descriptor = make ? make() : HeaderButtonDescriptor();
		if (descriptor.create) {
			descriptors.push_back(std::move(descriptor));
		}
	}
	ranges::stable_sort(
		descriptors,
		ranges::less(),
		&HeaderButtonDescriptor::order);
	for (const auto &descriptor : descriptors) {
		auto widget = descriptor.create(this, TabContext{
			.room = context.room,
			.show = context.show,
			.active = rpl::single(true),
			.switchTo = context.switchTo,
		});
		if (const auto raw = widget.release()) {
			raw->setParent(this);
			raw->show();
			rpl::merge(
				raw->widthValue() | rpl::to_empty,
				raw->shownValue() | rpl::to_empty
			) | rpl::on_next([=] {
				updateLayout();
			}, raw->lifetime());
			_extra.push_back(base::unique_qptr<Ui::RpWidget>(raw));
		}
	}

	const auto room = _room;
	const auto show = _show;
	_link->setClickedCallback([=] {
		QGuiApplication::clipboard()->setText(room->state().link);
		Toast(show, tr::lng_oblivion_room_link_copied(tr::now));
	});

	_room->changes(
	) | rpl::on_next([=](Changes changes) {
		const auto mine = Changes(Change::Room)
			| Change::Members
			| Change::Presence
			| Change::Rights
			| Change::Connection
			| Change::Reloaded;
		if (changes & mine) {
			updateLayout();
			update();
		}
	}, lifetime());
	updateLayout();
}

std::vector<const Member*> Header::stripMembers() const {
	auto result = std::vector<const Member*>();
	const auto &members = _room->state().members;
	for (const auto online : { true, false }) {
		for (const auto &member : members) {
			if (member.online == online) {
				result.push_back(&member);
			}
		}
	}
	return result;
}

void Header::updateLayout() {
	const auto w = width();
	if (w <= 0) {
		return;
	}
	const auto pad = Scaled(16);
	const auto middle = height() / 2;
	auto right = w - pad + Scaled(6);
	_more->move(right - _more->width(), middle - _more->height() / 2);
	right -= _more->width() + Scaled(2);
	const auto link = _room->can(Right::Invite)
		&& (_room->state().gone == Gone::No);
	_link->setVisible(link);
	if (link) {
		_link->move(right - _link->width(), middle - _link->height() / 2);
		right -= _link->width() + Scaled(2);
	}
	for (const auto &widget : _extra) {
		if (!widget->isHidden()) {
			widget->move(
				right - widget->width(),
				middle - widget->height() / 2);
			right -= widget->width() + Scaled(6);
		}
	}
	const auto size = Scaled(28);
	const auto step = size - Scaled(8);
	const auto limit = (w < Scaled(430)) ? 3 : kStripLimit;
	const auto count = std::min(int(_room->state().members.size()), limit);
	const auto stripWidth = count ? (size + (count - 1) * step) : 0;
	right -= Scaled(8);
	_strip = QRect(right - stripWidth, middle - size / 2, stripWidth, size);
	const auto hidden = int(_room->state().members.size()) - count;
	const auto more = (hidden > 0)
		? (st::normalFont->width(u"+%1"_q.arg(hidden)) + Scaled(6))
		: 0;
	_textRight = _strip.x() - more - Scaled(12);
}

void Header::resizeEvent(QResizeEvent *e) {
	updateLayout();
}

void Header::mouseMoveEvent(QMouseEvent *e) {
	setCursor(_strip.contains(e->pos())
		? style::cur_pointer
		: style::cur_default);
}

void Header::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton && _strip.contains(e->pos())) {
		_membersClicks.fire({});
	}
}

void Header::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	p.fillRect(e->rect(), st::windowBg);

	const auto &state = _room->state();
	const auto left = Scaled(20);
	const auto textWidth = std::max(_textRight - left, Scaled(40));
	const auto &titleFont = TitleFont();
	const auto total = titleFont->height + Scaled(3) + st::normalFont->height;
	auto top = (height() - total) / 2;
	p.setFont(titleFont);
	p.setPen(st::windowFg);
	p.drawText(
		left,
		top + titleFont->ascent,
		titleFont->elided(
			state.title.isEmpty()
				? tr::lng_oblivion_room_title_default(tr::now)
				: state.title,
			textWidth));
	top += titleFont->height + Scaled(3);
	const auto offline = !_room->connected() && (state.gone == Gone::No);
	p.setFont(st::normalFont);
	p.setPen(offline ? st::boxTextFgError : st::windowSubTextFg);
	p.drawText(
		left,
		top + st::normalFont->ascent,
		st::normalFont->elided(
			offline
				? tr::lng_oblivion_room_connecting(tr::now)
				: tr::lng_oblivion_room_header_status(
					tr::now,
					lt_members,
					MembersText(int(state.members.size())),
					lt_online,
					QString::number(state.onlineCount())),
			textWidth));

	// The members strip, the first one on top.
	const auto members = stripMembers();
	const auto size = _strip.height();
	const auto step = size - Scaled(8);
	const auto count = (size > 0 && _strip.width() > 0)
		? ((_strip.width() - size) / step + 1)
		: 0;
	for (auto i = std::min(count, int(members.size())) - 1; i >= 0; --i) {
		const auto member = members[i];
		const auto rect = QRect(_strip.x() + i * step, _strip.y(), size, size);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBg);
		p.drawEllipse(QRectF(rect).marginsAdded(QMarginsF(2, 2, 2, 2)));
		p.setOpacity(member->online ? 1. : 0.4);
		PaintUserpic(p, rect, member->id, member->name);
		p.setOpacity(1.);
	}
	const auto hidden = int(members.size()) - count;
	if (hidden > 0 && count > 0) {
		const auto text = u"+%1"_q.arg(hidden);
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			_strip.x() - st::normalFont->width(text) - Scaled(6),
			_strip.y() + (size - st::normalFont->height) / 2
				+ st::normalFont->ascent,
			text);
	}
	p.fillRect(0, height() - st::lineWidth, width(), st::lineWidth, st::shadowFg);
}

// ---- The tabs strip.

class TabsStrip final : public Ui::RpWidget {
public:
	explicit TabsStrip(QWidget *parent);

	void setTabs(std::vector<std::pair<QString, QString>> tabs);
	void setActive(const QString &id);
	void setDot(const QString &id, bool dot);

	[[nodiscard]] rpl::producer<QString> selected() const {
		return _selected.events();
	}

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	struct Tab {
		QString id;
		QString title;
		int width = 0;
		bool dot = false;
	};
	[[nodiscard]] QRect tabRect(int index) const;
	[[nodiscard]] int indexAt(QPoint point) const;

	std::vector<Tab> _tabs;
	QString _active;
	rpl::event_stream<QString> _selected;
	int _over = -1;

};

TabsStrip::TabsStrip(QWidget *parent) : RpWidget(parent) {
	setMouseTracking(true);
}

void TabsStrip::setTabs(std::vector<std::pair<QString, QString>> tabs) {
	_tabs.clear();
	for (auto &[id, title] : tabs) {
		_tabs.push_back({
			.id = id,
			.title = title,
			.width = st::semiboldFont->width(title),
		});
	}
	update();
}

void TabsStrip::setActive(const QString &id) {
	if (_active != id) {
		_active = id;
		update();
	}
}

void TabsStrip::setDot(const QString &id, bool dot) {
	const auto i = ranges::find(_tabs, id, &Tab::id);
	if (i != end(_tabs) && i->dot != dot) {
		i->dot = dot;
		update();
	}
}

QRect TabsStrip::tabRect(int index) const {
	const auto count = int(_tabs.size());
	if (index < 0 || index >= count) {
		return QRect();
	}
	const auto pad = Scaled(12);
	const auto available = width() - 2 * pad;
	auto natural = 0;
	for (const auto &tab : _tabs) {
		natural += tab.width + Scaled(28);
	}
	if (natural <= available) {
		auto left = pad;
		for (auto i = 0; i != index; ++i) {
			left += _tabs[i].width + Scaled(28);
		}
		return QRect(left, 0, _tabs[index].width + Scaled(28), height());
	}
	const auto each = available / count;
	return QRect(pad + index * each, 0, each, height());
}

int TabsStrip::indexAt(QPoint point) const {
	for (auto i = 0, count = int(_tabs.size()); i != count; ++i) {
		if (tabRect(i).contains(point)) {
			return i;
		}
	}
	return -1;
}

void TabsStrip::mouseMoveEvent(QMouseEvent *e) {
	const auto over = indexAt(e->pos());
	if (_over != over) {
		_over = over;
		setCursor((over >= 0) ? style::cur_pointer : style::cur_default);
		update();
	}
}

void TabsStrip::leaveEventHook(QEvent *e) {
	if (_over >= 0) {
		_over = -1;
		update();
	}
}

void TabsStrip::mouseReleaseEvent(QMouseEvent *e) {
	const auto index = indexAt(e->pos());
	if (e->button() == Qt::LeftButton && index >= 0) {
		_selected.fire_copy(_tabs[index].id);
	}
}

void TabsStrip::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	p.fillRect(e->rect(), st::windowBg);
	p.fillRect(0, height() - st::lineWidth, width(), st::lineWidth, st::shadowFg);
	for (auto i = 0, count = int(_tabs.size()); i != count; ++i) {
		const auto &tab = _tabs[i];
		const auto rect = tabRect(i);
		const auto active = (tab.id == _active);
		p.setFont(st::semiboldFont);
		p.setPen(active
			? st::windowActiveTextFg
			: (i == _over)
			? st::windowFg
			: st::windowSubTextFg);
		const auto text = st::semiboldFont->elided(
			tab.title,
			rect.width() - Scaled(8));
		const auto textWidth = st::semiboldFont->width(text);
		const auto left = rect.x() + (rect.width() - textWidth) / 2;
		p.drawText(
			left,
			(height() - st::semiboldFont->height) / 2
				+ st::semiboldFont->ascent,
			text);
		if (tab.dot && !active) {
			const auto dot = Scaled(6);
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgActive);
			p.drawEllipse(QRectF(
				left + textWidth + Scaled(3),
				(height() - st::semiboldFont->height) / 2,
				dot,
				dot));
		}
		if (active) {
			const auto line = Scaled(3);
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgActive);
			p.drawRoundedRect(
				QRectF(
					left - Scaled(2),
					height() - line,
					textWidth + Scaled(4),
					line + line),
				line,
				line);
		}
	}
}

// ---- The room chat.

class ChatList final : public Ui::RpWidget {
public:
	ChatList(QWidget *parent, not_null<Room*> room);

	// True if only new messages were added at the end.
	bool refresh();
	[[nodiscard]] rpl::producer<int64> menuRequests() const {
		return _menuRequests.events();
	}

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;

private:
	struct Entry {
		int64 id = 0;
		uint64 userId = 0;
		QString name;
		QString time;
		Ui::Text::String text;
		bool head = false;
		int top = 0;
		int height = 0;
	};
	void layout(int width);

	const not_null<Room*> _room;
	std::vector<Entry> _entries;
	rpl::event_stream<int64> _menuRequests;

};

ChatList::ChatList(QWidget *parent, not_null<Room*> room)
: RpWidget(parent)
, _room(room) {
}

bool ChatList::refresh() {
	const auto &chat = _room->state().chat;
	auto append = (chat.size() >= _entries.size());
	for (auto i = 0, count = int(_entries.size()); append && i != count; ++i) {
		append = (_entries[i].id == chat[i].id);
	}
	if (!append) {
		_entries.clear();
	}
	for (auto i = int(_entries.size()), count = int(chat.size());
		i != count;
		++i) {
		const auto &message = chat[i];
		const auto member = _room->state().member(message.userId);
		auto entry = Entry{
			.id = message.id,
			.userId = message.userId,
			.name = (member && !member->name.isEmpty())
				? member->name
				: message.name,
			.time = QDateTime::fromMSecsSinceEpoch(
				message.ts).toString(u"HH:mm"_q),
		};
		entry.text.setText(
			st::defaultTextStyle,
			message.text,
			kPlainTextOptions);
		entry.head = (i == 0)
			|| (chat[i - 1].userId != message.userId)
			|| (message.ts - chat[i - 1].ts > 5 * 60 * 1000);
		_entries.push_back(std::move(entry));
	}
	resizeToWidth(width());
	update();
	return append;
}

void ChatList::layout(int width) {
	const auto left = Scaled(20) + Scaled(34) + Scaled(10);
	const auto textWidth = std::max(width - left - Scaled(20), Scaled(80));
	auto top = Scaled(8);
	for (auto &entry : _entries) {
		entry.top = top;
		entry.height = (entry.head
			? (Scaled(10) + st::semiboldFont->height + Scaled(2))
			: Scaled(3))
			+ entry.text.countHeight(textWidth);
		top += entry.height;
	}
}

int ChatList::resizeGetHeight(int newWidth) {
	layout(newWidth);
	return _entries.empty()
		? Scaled(8)
		: (_entries.back().top + _entries.back().height + Scaled(12));
}

void ChatList::contextMenuEvent(QContextMenuEvent *e) {
	const auto y = e->pos().y();
	for (const auto &entry : _entries) {
		if (y >= entry.top && y < entry.top + entry.height) {
			_menuRequests.fire_copy(entry.id);
			return;
		}
	}
}

void ChatList::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto clip = e->rect();
	const auto userpic = Scaled(34);
	const auto userpicLeft = Scaled(20);
	const auto left = userpicLeft + userpic + Scaled(10);
	const auto textWidth = std::max(width() - left - Scaled(20), Scaled(80));
	for (const auto &entry : _entries) {
		if (entry.top + entry.height < clip.y()) {
			continue;
		} else if (entry.top > clip.y() + clip.height()) {
			break;
		}
		auto top = entry.top;
		if (entry.head) {
			top += Scaled(10);
			PaintUserpic(
				p,
				QRect(userpicLeft, top, userpic, userpic),
				entry.userId,
				entry.name);
			const auto timeWidth = st::normalFont->width(entry.time);
			const auto nameWidth = std::max(
				textWidth - timeWidth - Scaled(8),
				Scaled(40));
			const auto name = st::semiboldFont->elided(entry.name, nameWidth);
			p.setFont(st::semiboldFont);
			p.setPen((entry.userId == _room->selfId())
				? st::windowActiveTextFg
				: st::windowFg);
			p.drawText(left, top + st::semiboldFont->ascent, name);
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				left + st::semiboldFont->width(name) + Scaled(8),
				top + st::semiboldFont->ascent,
				entry.time);
			top += st::semiboldFont->height + Scaled(2);
		} else {
			top += Scaled(3);
		}
		p.setPen(st::windowFg);
		entry.text.draw(p, {
			.position = QPoint(left, top),
			.availableWidth = textWidth,
		});
	}
}

class ChatTab final : public Ui::RpWidget {
public:
	ChatTab(QWidget *parent, TabContext context);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

private:
	void updateLayout();
	void refresh();
	void send();
	void showMenu(int64 id);

	const not_null<Room*> _room;
	const not_null<Ui::ScrollArea*> _scroll;
	const not_null<Ui::InputField*> _field;
	const not_null<GlyphButton*> _send;
	QPointer<ChatList> _list;
	base::unique_qptr<Ui::PopupMenu> _menu;
	int _composeTop = 0;

};

ChatTab::ChatTab(QWidget *parent, TabContext context)
: RpWidget(parent)
, _room(context.room)
, _scroll(Ui::CreateChild<Ui::ScrollArea>(this, st::boxScroll))
, _field(Ui::CreateChild<Ui::InputField>(
	this,
	st::historyComposeField,
	Ui::InputField::Mode::MultiLine,
	tr::lng_oblivion_room_chat_placeholder()))
, _send(Ui::CreateChild<GlyphButton>(this, Glyph::Send, Scaled(36), true)) {
	_list = _scroll->setOwnedWidget(object_ptr<ChatList>(this, _room));
	_field->setMaxLength(kChatLimit);
	_field->setMaxHeight(Scaled(120));
	_field->setSubmitSettings(Ui::InputField::SubmitSettings::Enter);
	_field->submits(
	) | rpl::on_next([=](Qt::KeyboardModifiers) {
		send();
	}, _field->lifetime());
	_field->heightChanges(
	) | rpl::on_next([=] {
		updateLayout();
	}, _field->lifetime());
	_send->setClickedCallback([=] { send(); });

	_list->menuRequests(
	) | rpl::on_next([=](int64 id) {
		showMenu(id);
	}, lifetime());

	_room->changes(
	) | rpl::on_next([=](Changes changes) {
		const auto mine = Changes(Change::Chat)
			| Change::Rights
			| Change::Members
			| Change::Gone
			| Change::Reloaded;
		if (changes & mine) {
			refresh();
		}
	}, lifetime());

	std::move(
		context.active
	) | rpl::on_next([=](bool active) {
		if (active && !_field->isHidden()) {
			_field->setFocusFast();
		}
	}, lifetime());

	refresh();
}

void ChatTab::refresh() {
	const auto can = _room->can(Right::Chat)
		&& (_room->state().gone == Gone::No);
	_field->setVisible(can);
	_send->setVisible(can);
	const auto bottom = (_scroll->scrollTop() + Scaled(40)
		>= _scroll->scrollTopMax());
	if (_list) {
		_list->refresh();
	}
	updateLayout();
	if (bottom) {
		_scroll->scrollToY(_scroll->scrollTopMax());
	}
	update();
}

void ChatTab::send() {
	const auto text = _field->getLastText().trimmed();
	if (text.isEmpty() || !_room->can(Right::Chat)) {
		return;
	}
	_room->sendChat(text);
	_field->clear();
	_scroll->scrollToY(_scroll->scrollTopMax());
}

void ChatTab::showMenu(int64 id) {
	const auto &chat = _room->state().chat;
	const auto i = ranges::find(chat, id, &ChatMessage::id);
	if (i == end(chat)) {
		return;
	}
	const auto text = i->text;
	const auto mine = (i->userId == _room->selfId());
	const auto room = _room;
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::popupMenuWithIcons);
	_menu->addAction(tr::lng_oblivion_room_chat_copy(tr::now), [=] {
		QGuiApplication::clipboard()->setText(text);
	}, &st::menuIconCopy);
	if ((mine || _room->owner()) && (_room->state().gone == Gone::No)) {
		_menu->addAction(tr::lng_oblivion_room_chat_delete(tr::now), [=] {
			room->deleteChat(id);
		}, &st::menuIconDelete);
	}
	_menu->popup(QCursor::pos());
}

void ChatTab::updateLayout() {
	const auto w = width();
	if (w <= 0) {
		return;
	}
	const auto pad = Scaled(12);
	if (_field->isHidden()) {
		const auto note = Scaled(44);
		_composeTop = height() - note;
	} else {
		const auto fieldWidth = w - 2 * pad - _send->width() - Scaled(8);
		_field->resizeToWidth(std::max(fieldWidth, Scaled(80)));
		const auto block = std::max(_field->height(), _send->height())
			+ 2 * Scaled(8);
		_composeTop = height() - block;
		_field->moveToLeft(
			pad,
			_composeTop + (block - _field->height()) / 2,
			w);
		_send->moveToRight(
			pad,
			height() - Scaled(8) - _send->height()
				- (std::min(_field->height(), block) > _send->height()
					? Scaled(2)
					: 0),
			w);
	}
	_scroll->setGeometry(0, 0, w, std::max(_composeTop, 0));
	if (_list) {
		_list->resizeToWidth(w);
	}
}

void ChatTab::resizeEvent(QResizeEvent *e) {
	updateLayout();
}

void ChatTab::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(e->rect(), st::windowBg);
	p.fillRect(0, _composeTop, width(), st::lineWidth, st::shadowFg);
	if (_field->isHidden()) {
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(
				Scaled(16),
				_composeTop,
				width() - Scaled(32),
				height() - _composeTop),
			Qt::AlignCenter | Qt::TextWordWrap,
			tr::lng_oblivion_room_chat_no_right(tr::now));
	}
	if (_room->state().chat.empty()) {
		const auto area = QRect(
			Scaled(32),
			0,
			width() - Scaled(64),
			std::max(_composeTop, 0));
		const auto loading = !_room->chatLoaded();
		p.setFont(st::semiboldFont);
		p.setPen(st::windowFg);
		const auto middle = area.height() / 2 - Scaled(24);
		p.drawText(
			QRect(area.x(), middle, area.width(), st::semiboldFont->height),
			Qt::AlignHCenter | Qt::AlignTop,
			loading
				? tr::lng_oblivion_room_chat_loading(tr::now)
				: tr::lng_oblivion_room_chat_empty(tr::now));
		if (!loading) {
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			p.drawText(
				QRect(
					area.x(),
					middle + st::semiboldFont->height + Scaled(6),
					area.width(),
					Scaled(80)),
				Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
				tr::lng_oblivion_room_chat_empty_about(tr::now));
		}
	}
}

object_ptr<Ui::RpWidget> CreateChatTab(QWidget *parent, TabContext context) {
	return object_ptr<ChatTab>(parent, std::move(context));
}

// ---- «Участники».

class MembersList final : public Ui::RpWidget {
public:
	MembersList(
		QWidget *parent,
		not_null<Room*> room,
		std::shared_ptr<Ui::Show> show);

	void refresh();

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	struct Target {
		QRect rect;
		Fn<void()> action;
	};

	// Paints when p is not null; always collects the targets and returns
	// the full height.
	int pass(QPainter *p, int width);
	int chips(
		QPainter *p,
		int left,
		int top,
		int width,
		const Rights &rights,
		Fn<void(Right, bool)> toggle);
	int sectionTitle(QPainter *p, int top, int width, const QString &text);
	void addTarget(QRect rect, Fn<void()> action);
	[[nodiscard]] bool overNext() const;
	[[nodiscard]] int targetAt(QPoint point) const;
	void showMemberMenu(uint64 userId);
	void confirm(
		const QString &text,
		const QString &button,
		bool attention,
		Fn<void(not_null<Room*>)> action);
	[[nodiscard]] std::vector<const Member*> sorted() const;
	[[nodiscard]] QString memberStatus(const Member &member) const;

	const not_null<Room*> _room;
	const std::shared_ptr<Ui::Show> _show;
	std::vector<Target> _targets;
	base::unique_qptr<Ui::PopupMenu> _menu;
	int _over = -1;
	int _pressed = -1;

};

MembersList::MembersList(
	QWidget *parent,
	not_null<Room*> room,
	std::shared_ptr<Ui::Show> show)
: RpWidget(parent)
, _room(room)
, _show(std::move(show)) {
	setMouseTracking(true);
}

void MembersList::refresh() {
	resizeToWidth(width());
	update();
}

std::vector<const Member*> MembersList::sorted() const {
	auto result = std::vector<const Member*>();
	const auto self = _room->selfId();
	for (const auto &member : _room->state().members) {
		result.push_back(&member);
	}
	ranges::stable_sort(result, [&](const Member *a, const Member *b) {
		const auto rank = [&](const Member *member) {
			return member->owner
				? 0
				: (member->id == self)
				? 1
				: member->online
				? 2
				: 3;
		};
		return rank(a) < rank(b);
	});
	return result;
}

QString MembersList::memberStatus(const Member &member) const {
	auto parts = QStringList();
	if (member.id == _room->selfId()) {
		parts.push_back(tr::lng_oblivion_room_members_you(tr::now));
	}
	if (member.owner) {
		parts.push_back(tr::lng_oblivion_room_members_owner(tr::now));
	}
	const auto current = _room->player(Kind::Music).current();
	if (member.online
		&& current
		&& member.music.itemId == current->id
		&& !member.music.ready) {
		parts.push_back(tr::lng_oblivion_room_members_loading(
			tr::now,
			lt_percent,
			QString::number(member.music.buffered)));
	} else {
		parts.push_back(member.online
			? tr::lng_oblivion_room_members_online(tr::now)
			: tr::lng_oblivion_room_members_offline(tr::now));
	}
	return parts.join(u" · "_q);
}

void MembersList::addTarget(QRect rect, Fn<void()> action) {
	_targets.push_back({ rect, std::move(action) });
}

bool MembersList::overNext() const {
	return (_over == int(_targets.size()));
}

int MembersList::targetAt(QPoint point) const {
	for (auto i = 0, count = int(_targets.size()); i != count; ++i) {
		if (_targets[i].rect.contains(point)) {
			return i;
		}
	}
	return -1;
}

int MembersList::sectionTitle(
		QPainter *p,
		int top,
		int width,
		const QString &text) {
	if (p) {
		p->setFont(st::semiboldFont);
		p->setPen(st::windowActiveTextFg);
		p->drawText(
			Scaled(20),
			top + st::semiboldFont->ascent,
			st::semiboldFont->elided(text, width - Scaled(40)));
	}
	return top + st::semiboldFont->height + Scaled(8);
}

// A row (or several) of chips, one for a right. toggle == nullptr: only
// shown. Returns the bottom.
int MembersList::chips(
		QPainter *p,
		int left,
		int top,
		int width,
		const Rights &rights,
		Fn<void(Right, bool)> toggle) {
	const auto height = Scaled(24);
	const auto gap = Scaled(6);
	auto x = left;
	auto y = top;
	for (auto i = 0; i != kRightsCount; ++i) {
		const auto right = Right(i);
		const auto on = rights.has(right);
		const auto text = RightLabel(right);
		const auto w = st::normalFont->width(text) + 2 * Scaled(10);
		if (x > left && x + w > left + width) {
			x = left;
			y += height + gap;
		}
		const auto rect = QRect(x, y, w, height);
		const auto over = toggle && overNext();
		if (p) {
			auto bg = on ? st::windowBgActive->c : st::windowBgOver->c;
			if (on) {
				bg.setAlphaF(over ? 0.28 : 0.16);
			}
			p->setPen(Qt::NoPen);
			p->setBrush((over && !on) ? st::windowBgRipple->c : bg);
			p->drawRoundedRect(QRectF(rect), height / 2., height / 2.);
			p->setFont(st::normalFont);
			p->setPen(on ? st::windowActiveTextFg : st::windowSubTextFg);
			p->drawText(
				rect.x() + Scaled(10),
				rect.y() + (height - st::normalFont->height) / 2
					+ st::normalFont->ascent,
				text);
		}
		if (toggle) {
			addTarget(rect, [=] { toggle(right, !on); });
		}
		x += w + gap;
	}
	return y + height;
}

int MembersList::pass(QPainter *p, int width) {
	_targets.clear();
	const auto &state = _room->state();
	const auto owner = state.owner && (state.gone == Gone::No);
	const auto pad = Scaled(20);
	const auto inner = std::max(width - 2 * pad, Scaled(120));
	const auto room = _room;
	auto top = Scaled(16);

	if (owner) {
		top = sectionTitle(
			p,
			top,
			width,
			tr::lng_oblivion_room_members_defaults(tr::now));
		const auto defaults = state.settings.defaults;
		top = chips(p, pad, top, inner, defaults, [=](Right right, bool on) {
			auto changed = defaults;
			changed.set(right, on);
			room->setDefaults(changed, false);
		});
		top += Scaled(10);

		// The presets.
		const auto presetHeight = Scaled(30);
		auto x = pad;
		const auto preset = [&](
				const QString &text,
				const Rights &rights,
				const QString &sure) {
			const auto w = st::semiboldFont->width(text) + 2 * Scaled(14);
			if (x > pad && x + w > pad + inner) {
				x = pad;
				top += presetHeight + Scaled(6);
			}
			const auto rect = QRect(x, top, w, presetHeight);
			if (p) {
				p->setPen(Qt::NoPen);
				p->setBrush(overNext()
					? st::windowBgRipple
					: st::windowBgOver);
				p->drawRoundedRect(
					QRectF(rect),
					presetHeight / 2.,
					presetHeight / 2.);
				p->setFont(st::semiboldFont);
				p->setPen(st::windowActiveTextFg);
				p->drawText(
					rect.x() + Scaled(14),
					rect.y() + (presetHeight - st::semiboldFont->height) / 2
						+ st::semiboldFont->ascent,
					text);
			}
			addTarget(rect, [=] {
				confirm(
					sure,
					tr::lng_oblivion_room_preset_apply(tr::now),
					false,
					[=](not_null<Room*> strong) {
						strong->setDefaults(rights, true);
					});
			});
			x += w + Scaled(8);
		};
		preset(
			tr::lng_oblivion_room_preset_all(tr::now),
			Rights::Everything(),
			tr::lng_oblivion_room_preset_sure_all(tr::now));
		preset(
			tr::lng_oblivion_room_preset_owner(tr::now),
			Rights::OnlyOwner(),
			tr::lng_oblivion_room_preset_sure_owner(tr::now));
		top += presetHeight + Scaled(8);
		if (p) {
			p->setFont(st::normalFont);
			p->setPen(st::windowSubTextFg);
		}
		const auto about = tr::lng_oblivion_room_members_defaults_about(
			tr::now);
		const auto aboutRect = QFontMetrics(st::normalFont->f).boundingRect(
			QRect(0, 0, inner, 10000),
			Qt::TextWordWrap,
			about);
		if (p) {
			p->drawText(
				QRect(pad, top, inner, aboutRect.height()),
				Qt::TextWordWrap,
				about);
		}
		top += aboutRect.height() + Scaled(18);
	} else if (state.gone == Gone::No) {
		top = sectionTitle(
			p,
			top,
			width,
			tr::lng_oblivion_room_members_my_rights(tr::now));
		top = chips(p, pad, top, inner, state.rights, nullptr);
		top += Scaled(18);
	}

	top = sectionTitle(
		p,
		top,
		width,
		tr::lng_oblivion_room_members_list(tr::now)
			+ u" · "_q
			+ QString::number(state.members.size()));
	const auto userpic = Scaled(42);
	const auto textLeft = pad + userpic + Scaled(12);
	for (const auto member : sorted()) {
		const auto manage = owner && !member->owner;
		const auto rowTop = top;
		const auto menuSize = Scaled(30);
		const auto textWidth = width - textLeft - pad - (manage ? menuSize : 0);
		if (p) {
			p->setOpacity(member->online ? 1. : 0.55);
			PaintUserpic(
				*p,
				QRect(pad, rowTop + Scaled(4), userpic, userpic),
				member->id,
				member->name);
			p->setOpacity(1.);
			if (member->online) {
				const auto dot = Scaled(11);
				p->setPen(QPen(st::windowBg, Scaled(2)));
				p->setBrush(st::boxTextFgGood);
				p->drawEllipse(QRectF(
					pad + userpic - dot,
					rowTop + Scaled(4) + userpic - dot,
					dot,
					dot));
			}
			const auto name = member->name.isEmpty()
				? QString::number(member->id)
				: member->name;
			const auto crown = member->owner ? Scaled(20) : 0;
			const auto elided = st::semiboldFont->elided(
				name,
				std::max(textWidth - crown, Scaled(40)));
			p->setFont(st::semiboldFont);
			p->setPen(st::windowFg);
			p->drawText(
				textLeft,
				rowTop + Scaled(6) + st::semiboldFont->ascent,
				elided);
			if (member->owner) {
				PaintGlyph(
					*p,
					Glyph::Crown,
					QRectF(
						textLeft + st::semiboldFont->width(elided) + Scaled(5),
						rowTop + Scaled(6),
						Scaled(15),
						Scaled(15)),
					st::windowActiveTextFg->c);
			}
			p->setFont(st::normalFont);
			p->setPen(st::windowSubTextFg);
			p->drawText(
				textLeft,
				rowTop + Scaled(26) + st::normalFont->ascent,
				st::normalFont->elided(memberStatus(*member), textWidth));
		}
		if (manage) {
			const auto rect = QRect(
				width - pad - menuSize + Scaled(6),
				rowTop + Scaled(10),
				menuSize,
				menuSize);
			if (p) {
				if (overNext()) {
					p->setPen(Qt::NoPen);
					p->setBrush(st::windowBgOver);
					p->drawEllipse(QRectF(rect));
				}
				PaintGlyph(
					*p,
					Glyph::More,
					QRectF(rect).marginsRemoved(QMarginsF(
						Scaled(5),
						Scaled(5),
						Scaled(5),
						Scaled(5))),
					st::windowSubTextFg->c);
			}
			const auto id = member->id;
			addTarget(rect, [=] { showMemberMenu(id); });
		}
		top = rowTop + Scaled(52);
		if (member->owner) {
			if (p) {
				const auto text = tr::lng_oblivion_room_members_all_rights(
					tr::now);
				const auto height = Scaled(24);
				const auto rect = QRect(
					textLeft,
					top,
					st::normalFont->width(text) + 2 * Scaled(10),
					height);
				auto bg = st::windowBgActive->c;
				bg.setAlphaF(0.16);
				p->setPen(Qt::NoPen);
				p->setBrush(bg);
				p->drawRoundedRect(QRectF(rect), height / 2., height / 2.);
				p->setFont(st::normalFont);
				p->setPen(st::windowActiveTextFg);
				p->drawText(
					rect.x() + Scaled(10),
					rect.y() + (height - st::normalFont->height) / 2
						+ st::normalFont->ascent,
					text);
			}
			top += Scaled(24);
		} else {
			const auto id = member->id;
			const auto rights = member->rights;
			top = chips(
				p,
				textLeft,
				top,
				width - textLeft - pad,
				rights,
				manage ? Fn<void(Right, bool)>([=](Right right, bool on) {
					auto changed = rights;
					changed.set(right, on);
					room->setRights(id, changed);
				}) : nullptr);
		}
		top += Scaled(14);
	}

	if (owner && !state.banned.empty()) {
		top += Scaled(4);
		top = sectionTitle(
			p,
			top,
			width,
			tr::lng_oblivion_room_members_banned(tr::now));
		for (const auto &banned : state.banned) {
			const auto action = tr::lng_oblivion_room_members_unban(tr::now);
			const auto actionWidth = st::normalFont->width(action);
			const auto rect = QRect(
				width - pad - actionWidth,
				top,
				actionWidth,
				st::normalFont->height + Scaled(8));
			if (p) {
				const auto name = banned.name.isEmpty()
					? QString::number(banned.id)
					: banned.name;
				p->setFont(st::normalFont);
				p->setPen(st::windowFg);
				p->drawText(
					pad,
					top + Scaled(4) + st::normalFont->ascent,
					st::normalFont->elided(
						name,
						inner - actionWidth - Scaled(12)));
				p->setPen(overNext()
					? st::windowFg
					: st::windowActiveTextFg);
				p->drawText(
					rect.x(),
					top + Scaled(4) + st::normalFont->ascent,
					action);
			}
			const auto id = banned.id;
			addTarget(rect, [=] { room->unban(id); });
			top += rect.height() + Scaled(4);
		}
	}
	return top + Scaled(16);
}

int MembersList::resizeGetHeight(int newWidth) {
	return pass(nullptr, newWidth);
}

void MembersList::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	pass(&p, width());
}

void MembersList::mouseMoveEvent(QMouseEvent *e) {
	const auto over = targetAt(e->pos());
	if (_over != over) {
		_over = over;
		setCursor((over >= 0) ? style::cur_pointer : style::cur_default);
		update();
	}
}

void MembersList::leaveEventHook(QEvent *e) {
	if (_over >= 0) {
		_over = -1;
		update();
	}
}

void MembersList::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = targetAt(e->pos());
	}
}

void MembersList::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto index = targetAt(e->pos());
	const auto pressed = std::exchange(_pressed, -1);
	if (index >= 0 && index == pressed) {
		// The action may rebuild the targets.
		const auto action = _targets[index].action;
		if (action) {
			action();
		}
	}
}

void MembersList::confirm(
		const QString &text,
		const QString &button,
		bool attention,
		Fn<void(not_null<Room*>)> action) {
	if (!_show || !_show->valid()) {
		return;
	}
	const auto weak = base::make_weak(_room.get());
	_show->showBox(Ui::MakeConfirmBox({
		.text = text,
		.confirmed = [=](Fn<void()> close) {
			if (const auto strong = weak.get()) {
				action(strong);
			}
			close();
		},
		.confirmText = button,
		.confirmStyle = attention ? &st::attentionBoxButton : nullptr,
	}));
}

void MembersList::showMemberMenu(uint64 userId) {
	const auto member = _room->state().member(userId);
	if (!member || !_room->owner()) {
		return;
	}
	const auto name = member->name.isEmpty()
		? QString::number(userId)
		: member->name;
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::popupMenuWithIcons);
	_menu->addAction(tr::lng_oblivion_room_member_transfer(tr::now), [=] {
		confirm(
			tr::lng_oblivion_room_member_transfer_sure(
				tr::now,
				lt_name,
				name),
			tr::lng_oblivion_room_member_transfer(tr::now),
			false,
			[=](not_null<Room*> strong) { strong->transfer(userId); });
	}, &st::menuIconAdmin);
	const auto room = _room;
	_menu->addAction(tr::lng_oblivion_room_member_kick(tr::now), [=] {
		room->kick(userId, false);
	}, &st::menuIconRemove);
	_menu->addAction(tr::lng_oblivion_room_member_ban(tr::now), [=] {
		confirm(
			tr::lng_oblivion_room_member_ban_sure(tr::now, lt_name, name),
			tr::lng_oblivion_room_member_ban(tr::now),
			true,
			[=](not_null<Room*> strong) { strong->kick(userId, true); });
	}, &st::menuIconBlock);
	_menu->popup(QCursor::pos());
}

class MembersTab final : public Ui::RpWidget {
public:
	MembersTab(QWidget *parent, TabContext context);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

private:
	const not_null<Ui::ScrollArea*> _scroll;
	QPointer<MembersList> _list;

};

MembersTab::MembersTab(QWidget *parent, TabContext context)
: RpWidget(parent)
, _scroll(Ui::CreateChild<Ui::ScrollArea>(this, st::boxScroll)) {
	_list = _scroll->setOwnedWidget(
		object_ptr<MembersList>(this, context.room, context.show));
	context.room->changes(
	) | rpl::on_next([=](Changes changes) {
		const auto mine = Changes(Change::Room)
			| Change::Members
			| Change::Presence
			| Change::Status
			| Change::Rights
			| Change::MusicPlayer
			| Change::Gone
			| Change::Reloaded;
		if ((changes & mine) && _list) {
			_list->refresh();
		}
	}, lifetime());
}

void MembersTab::paintEvent(QPaintEvent *e) {
	QPainter(this).fillRect(e->rect(), st::windowBg);
}

void MembersTab::resizeEvent(QResizeEvent *e) {
	_scroll->setGeometry(rect());
	if (_list) {
		_list->resizeToWidth(width());
	}
}

object_ptr<Ui::RpWidget> CreateMembersTab(
		QWidget *parent,
		TabContext context) {
	return object_ptr<MembersTab>(parent, std::move(context));
}

// ---- Small boxes.

struct NameBoxArgs {
	rpl::producer<QString> title;
	rpl::producer<QString> about; // May be null.
	rpl::producer<QString> button;
	QString value;
	// finished(false) lets the user press the button again.
	Fn<void(QString name, Fn<void(bool ok)> finished)> done;
};

void NameBox(not_null<Ui::GenericBox*> box, NameBoxArgs &&args) {
	struct State {
		bool sent = false;
	};
	const auto state = box->lifetime().make_state<State>();
	box->setTitle(std::move(args.title));
	box->setWidth(st::boxWidth);
	if (args.about) {
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				std::move(args.about),
				st::boxLabel),
			st::boxRowPadding + QMargins(0, 0, 0, st::boxLittleSkip));
	}
	const auto field = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::lng_oblivion_room_name_placeholder(),
			args.value),
		st::boxRowPadding + QMargins(0, 0, 0, st::boxLittleSkip));
	field->setMaxLength(kTitleLimit);
	box->setFocusCallback([=] {
		field->setFocusFast();
	});
	const auto done = std::move(args.done);
	const auto submit = [=] {
		const auto name = field->getLastText().trimmed();
		if (state->sent) {
			return;
		} else if (name.isEmpty()) {
			field->showError();
			return;
		}
		state->sent = true;
		done(name, crl::guard(box, [=](bool ok) {
			state->sent = false;
			if (ok) {
				box->closeBox();
			}
		}));
	};
	field->submits(
	) | rpl::on_next([=](Qt::KeyboardModifiers) {
		submit();
	}, field->lifetime());
	box->addButton(std::move(args.button), submit);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

struct JoinBoxArgs {
	QString code;
	QString title;
	QString ownerName;
	uint64 ownerId = 0;
	int members = 0;
	int maxMembers = 0;
	QString reason; // "", "full", "banned", "invite_required".
	Fn<void(Fn<void(bool ok)> finished)> join;
};

class JoinPreview final : public Ui::RpWidget {
public:
	JoinPreview(QWidget *parent, const JoinBoxArgs &args);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;

private:
	const QString _code;
	const QString _title;
	const QString _ownerName;
	const QString _members;

};

JoinPreview::JoinPreview(QWidget *parent, const JoinBoxArgs &args)
: RpWidget(parent)
, _code(args.code)
, _title(args.title.isEmpty()
	? tr::lng_oblivion_room_title_default(tr::now)
	: args.title)
, _ownerName(args.ownerName)
, _members(tr::lng_oblivion_room_join_members(
	tr::now,
	lt_members,
	QString::number(args.members),
	lt_max,
	QString::number(std::max(args.maxMembers, args.members)))) {
}

int JoinPreview::resizeGetHeight(int newWidth) {
	return Scaled(84);
}

void JoinPreview::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto cover = Scaled(64);
	const auto top = (height() - cover) / 2;
	PaintCover(p, QRect(0, top, cover, cover), QImage(), _code, Scaled(16));
	const auto left = cover + Scaled(14);
	const auto textWidth = width() - left;
	const auto &titleFont = TitleFont();
	p.setFont(titleFont);
	p.setPen(st::windowFg);
	p.drawText(
		left,
		top + Scaled(2) + titleFont->ascent,
		titleFont->elided(_title, textWidth));
	p.setFont(st::normalFont);
	p.setPen(st::windowSubTextFg);
	auto y = top + Scaled(2) + titleFont->height + Scaled(3);
	if (!_ownerName.isEmpty()) {
		p.drawText(
			left,
			y + st::normalFont->ascent,
			st::normalFont->elided(
				tr::lng_oblivion_room_join_owner(
					tr::now,
					lt_name,
					_ownerName),
				textWidth));
		y += st::normalFont->height + Scaled(1);
	}
	p.drawText(
		left,
		y + st::normalFont->ascent,
		st::normalFont->elided(_members, textWidth));
}

void JoinBox(not_null<Ui::GenericBox*> box, JoinBoxArgs &&args) {
	struct State {
		bool sent = false;
	};
	const auto state = box->lifetime().make_state<State>();
	box->setTitle(tr::lng_oblivion_room_join_title());
	box->setWidth(st::boxWidth);
	box->addRow(
		object_ptr<JoinPreview>(box, args),
		st::boxRowPadding + QMargins(0, 0, 0, st::boxLittleSkip));
	const auto blocked = !args.reason.isEmpty();
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			(args.reason == u"full"_q)
				? tr::lng_oblivion_room_join_full()
				: (args.reason == u"banned"_q)
				? tr::lng_oblivion_room_join_banned()
				: blocked
				? tr::lng_oblivion_room_join_invite()
				: tr::lng_oblivion_room_join_about(),
			blocked ? st::boxLabel : st::boxDividerLabel),
		st::boxRowPadding + QMargins(0, 0, 0, st::boxLittleSkip));
	if (blocked) {
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		return;
	}
	const auto join = std::move(args.join);
	box->addButton(tr::lng_oblivion_room_join_button(), [=] {
		if (state->sent || !join) {
			return;
		}
		state->sent = true;
		join(crl::guard(box, [=](bool ok) {
			state->sent = false;
			if (ok) {
				box->closeBox();
			}
		}));
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

struct RoomsRow {
	QString code;
	QString title;
	bool owner = false;
	int members = 0;
	int online = 0;
};

struct RoomsBoxArgs {
	std::shared_ptr<Ui::Show> show;
	Fn<void(
		Fn<void(std::vector<RoomsRow>)> done,
		Fn<void(const Cloud::Error &)> fail)> load;
	Fn<void(const QString &code)> open;
	Fn<void()> create;
	Fn<void(const QString &typed)> join;
};

void RoomsBox(not_null<Ui::GenericBox*> box, RoomsBoxArgs &&args) {
	box->setTitle(tr::lng_oblivion_room_settings());
	box->setWidth(st::boxWideWidth);
	const auto shared = std::make_shared<RoomsBoxArgs>(std::move(args));
	const auto container = box->verticalLayout();
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			tr::lng_oblivion_room_settings_about(),
			st::boxDividerLabel),
		st::boxRowPadding + QMargins(0, 0, 0, st::boxLittleSkip));
	const auto list = container->add(object_ptr<Ui::VerticalLayout>(container));
	const auto status = [=](rpl::producer<QString> text) {
		list->clear();
		list->add(
			object_ptr<Ui::FlatLabel>(list, std::move(text), st::boxLabel),
			st::boxRowPadding + QMargins(
				0,
				st::boxLittleSkip,
				0,
				st::boxLittleSkip));
		list->resizeToWidth(box->width());
	};
	status(tr::lng_oblivion_room_list_loading());
	if (shared->load) {
		shared->load(crl::guard(box, [=](std::vector<RoomsRow> rows) {
			if (rows.empty()) {
				status(tr::lng_oblivion_room_list_empty());
				return;
			}
			list->clear();
			for (const auto &row : rows) {
				const auto title = row.title.isEmpty()
					? tr::lng_oblivion_room_title_default(tr::now)
					: row.title;
				const auto members = MembersText(row.members);
				const auto text = row.owner
					? tr::lng_oblivion_room_list_row_owner(
						tr::now,
						lt_title,
						title,
						lt_members,
						members)
					: tr::lng_oblivion_room_list_row(
						tr::now,
						lt_title,
						title,
						lt_members,
						members);
				const auto button = list->add(
					object_ptr<Ui::SettingsButton>(
						list,
						rpl::single(text),
						st::settingsButtonNoIcon));
				const auto code = row.code;
				button->setClickedCallback([=] {
					if (shared->open) {
						shared->open(code);
					}
					box->closeBox();
				});
			}
			list->resizeToWidth(box->width());
		}), crl::guard(box, [=](const Cloud::Error &error) {
			status(rpl::single(RoomErrorText(error)));
		}));
	}

	const auto field = container->add(
		object_ptr<Ui::InputField>(
			container,
			st::defaultInputField,
			tr::lng_oblivion_room_list_join_placeholder()),
		st::boxRowPadding + QMargins(
			0,
			st::boxLittleSkip,
			0,
			st::boxLittleSkip));
	field->setMaxLength(256);
	const auto join = [=] {
		const auto typed = field->getLastText().trimmed();
		if (typed.isEmpty()) {
			field->showError();
		} else if (ExtractCode(typed).isEmpty()) {
			field->showError();
			Toast(shared->show, tr::lng_oblivion_room_list_bad_code(tr::now));
		} else if (shared->join) {
			shared->join(typed);
			box->closeBox();
		}
	};
	field->submits(
	) | rpl::on_next([=](Qt::KeyboardModifiers) {
		join();
	}, field->lifetime());

	box->addButton(tr::lng_oblivion_room_list_create(), [=] {
		if (shared->create) {
			shared->create();
		}
		box->closeBox();
	});
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	box->addLeftButton(tr::lng_oblivion_room_list_join(), join);
}

// ---- The content of the window.

class RoomWidget final : public Ui::RpWidget {
public:
	RoomWidget(
		QWidget *parent,
		not_null<Room*> room,
		std::shared_ptr<Ui::Show> show,
		const QString &tab);

	[[nodiscard]] rpl::producer<> closeRequests() const {
		return _closeRequests.events();
	}
	void showTab(const QString &id);

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	struct Slot {
		TabDescriptor descriptor;
		base::unique_qptr<Ui::RpWidget> widget;
	};

	[[nodiscard]] TabContext contextFor(const QString &id);
	void updateLayout();
	void showMenu();
	void refreshGone();
	void refreshChatDot();
	void rename();
	void leave();
	void closeForAll();

	const not_null<Room*> _room;
	const std::shared_ptr<Ui::Show> _show;
	const not_null<Header*> _header;
	const not_null<TabsStrip*> _tabs;
	const not_null<Ui::RpWidget*> _content;
	std::vector<Slot> _slots;
	std::vector<base::unique_qptr<Ui::RpWidget>> _overlays;
	base::unique_qptr<Ui::RpWidget> _gone;
	base::unique_qptr<Ui::PopupMenu> _menu;
	rpl::variable<QString> _active;
	rpl::event_stream<> _closeRequests;
	int64 _chatSeen = 0;

};

RoomWidget::RoomWidget(
	QWidget *parent,
	not_null<Room*> room,
	std::shared_ptr<Ui::Show> show,
	const QString &tab)
: RpWidget(parent)
, _room(room)
, _show(std::move(show))
, _header(Ui::CreateChild<Header>(this, contextFor(QString())))
, _tabs(Ui::CreateChild<TabsStrip>(this))
, _content(Ui::CreateChild<Ui::RpWidget>(this)) {
	auto titles = std::vector<std::pair<QString, QString>>();
	for (auto &descriptor : CollectTabs()) {
		titles.emplace_back(descriptor.id, descriptor.title());
		_slots.push_back({ .descriptor = std::move(descriptor) });
	}
	_tabs->setTabs(std::move(titles));

	for (const auto &make : Makers<OverlayDescriptor>()) {
		const auto descriptor = make ? make() : OverlayDescriptor();
		if (!descriptor.create) {
			continue;
		}
		auto widget = descriptor.create(_content, contextFor(QString()));
		if (const auto raw = widget.release()) {
			raw->setParent(_content);
			raw->show();
			_overlays.push_back(base::unique_qptr<Ui::RpWidget>(raw));
		}
	}

	if (!_room->state().chat.empty()) {
		_chatSeen = _room->state().chat.back().id;
	} else {
		_chatSeen = _room->state().chatLastId;
	}

	_tabs->selected(
	) | rpl::on_next([=](const QString &id) {
		showTab(id);
	}, lifetime());
	_header->membersClicks(
	) | rpl::on_next([=] {
		showTab(u"members"_q);
	}, lifetime());
	_header->menuClicks(
	) | rpl::on_next([=] {
		showMenu();
	}, lifetime());

	_room->changes(
	) | rpl::on_next([=](Changes changes) {
		if (changes & Change::Gone) {
			refreshGone();
		}
		if (changes & (Changes(Change::Chat) | Change::Reloaded)) {
			refreshChatDot();
		}
	}, lifetime());
	_room->errors(
	) | rpl::on_next([=](const Cloud::Error &error) {
		ShowRoomError(_show, error);
	}, lifetime());
	_room->toasts(
	) | rpl::on_next([=](const QString &text) {
		Toast(_show, text);
	}, lifetime());

	const auto first = ranges::contains(_slots, tab, [](const Slot &slot) {
		return slot.descriptor.id;
	}) ? tab : _slots.empty() ? QString() : _slots.front().descriptor.id;
	showTab(first);
	refreshGone();
}

TabContext RoomWidget::contextFor(const QString &id) {
	auto active = id.isEmpty()
		? rpl::producer<bool>(rpl::single(true))
		: rpl::producer<bool>(_active.value(
		) | rpl::map([=](const QString &active) {
			return (active == id);
		}) | rpl::distinct_until_changed());
	return {
		.room = _room,
		.show = _show,
		.active = std::move(active),
		.switchTo = crl::guard(this, [=](const QString &other) {
			showTab(other);
		}),
	};
}

void RoomWidget::showTab(const QString &id) {
	const auto i = ranges::find(_slots, id, [](const Slot &slot) {
		return slot.descriptor.id;
	});
	if (i == end(_slots) || _active.current() == id) {
		return;
	}
	if (!i->widget) {
		auto widget = i->descriptor.create(_content, contextFor(id));
		const auto raw = widget.release();
		if (!raw) {
			return;
		}
		raw->setParent(_content);
		i->widget = base::unique_qptr<Ui::RpWidget>(raw);
	}
	for (auto &slot : _slots) {
		if (slot.widget) {
			slot.widget->setVisible(slot.descriptor.id == id);
		}
	}
	i->widget->setGeometry(_content->rect());
	for (const auto &overlay : _overlays) {
		overlay->raise();
	}
	_tabs->setActive(id);
	_active = id;
	refreshChatDot();
}

void RoomWidget::refreshChatDot() {
	const auto &chat = _room->state().chat;
	const auto last = chat.empty() ? int64(0) : chat.back().id;
	if (_active.current() == u"chat"_q) {
		_chatSeen = std::max(_chatSeen, last);
	}
	const auto unread = !chat.empty()
		&& (last > _chatSeen)
		&& (chat.back().userId != _room->selfId());
	_tabs->setDot(u"chat"_q, unread);
}

void RoomWidget::refreshGone() {
	const auto gone = _room->state().gone;
	if (gone == Gone::No) {
		return;
	} else if (gone == Gone::Left || (gone == Gone::Closed && _room->owner())) {
		// The user's own action: nothing to explain.
		crl::on_main(this, [=] {
			_closeRequests.fire({});
		});
		return;
	} else if (_gone) {
		return;
	}
	const auto text = GoneText(gone);
	_gone = base::make_unique_q<Ui::RpWidget>(this);
	const auto raw = _gone.get();
	const auto button = Ui::CreateChild<Ui::RoundButton>(
		raw,
		tr::lng_oblivion_room_gone_button(),
		st::defaultActiveButton);
	button->setFullRadius(true);
	button->setClickedCallback([=] {
		_closeRequests.fire({});
	});
	raw->paintRequest(
	) | rpl::on_next([=](QRect clip) {
		auto p = QPainter(raw);
		p.fillRect(clip, st::windowBg);
		p.setFont(TitleFont());
		p.setPen(st::windowFg);
		p.drawText(
			QRect(
				Scaled(32),
				0,
				raw->width() - Scaled(64),
				raw->height() / 2 - Scaled(12)),
			Qt::AlignHCenter | Qt::AlignBottom | Qt::TextWordWrap,
			text);
	}, raw->lifetime());
	raw->sizeValue(
	) | rpl::on_next([=](QSize size) {
		button->move(
			(size.width() - button->width()) / 2,
			size.height() / 2 + Scaled(12));
	}, raw->lifetime());
	raw->setGeometry(rect());
	raw->show();
	raw->raise();
}

void RoomWidget::updateLayout() {
	const auto w = width();
	const auto headerHeight = Scaled(68);
	const auto tabsHeight = Scaled(42);
	_header->setGeometry(0, 0, w, headerHeight);
	_tabs->setGeometry(0, headerHeight, w, tabsHeight);
	const auto top = headerHeight + tabsHeight;
	_content->setGeometry(0, top, w, std::max(height() - top, 0));
	for (const auto &slot : _slots) {
		if (slot.widget) {
			slot.widget->setGeometry(_content->rect());
		}
	}
	for (const auto &overlay : _overlays) {
		overlay->setGeometry(_content->rect());
	}
	if (_gone) {
		_gone->setGeometry(rect());
	}
}

void RoomWidget::resizeEvent(QResizeEvent *e) {
	updateLayout();
}

void RoomWidget::paintEvent(QPaintEvent *e) {
	QPainter(this).fillRect(e->rect(), st::windowBg);
}

void RoomWidget::showMenu() {
	if (_room->state().gone != Gone::No) {
		return;
	}
	const auto room = _room;
	const auto show = _show;
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::popupMenuWithIcons);
	if (_room->can(Right::Invite)) {
		_menu->addAction(tr::lng_oblivion_room_copy_link(tr::now), [=] {
			QGuiApplication::clipboard()->setText(room->state().link);
			Toast(show, tr::lng_oblivion_room_link_copied(tr::now));
		}, &st::menuIconLink);
		_menu->addAction(tr::lng_oblivion_room_copy_code(tr::now), [=] {
			QGuiApplication::clipboard()->setText(room->code());
			Toast(show, tr::lng_oblivion_room_code_copied(tr::now));
		}, &st::menuIconCopy);
	}
	if (_room->owner()) {
		_menu->addAction(
			tr::lng_oblivion_room_rename(tr::now),
			[=] { rename(); },
			&st::menuIconEdit);
	}
	for (const auto &make : Makers<MenuDescriptor>()) {
		const auto descriptor = make ? make() : MenuDescriptor();
		if (descriptor.fill) {
			descriptor.fill(_menu.get(), contextFor(QString()));
		}
	}
	_menu->addSeparator();
	_menu->addAction(
		tr::lng_oblivion_room_leave(tr::now),
		[=] { leave(); },
		&st::menuIconLeave);
	if (_room->owner()) {
		_menu->addAction(
			tr::lng_oblivion_room_close_all(tr::now),
			[=] { closeForAll(); },
			&st::menuIconDelete);
	}
	const auto button = _header->menuButton();
	_menu->popup(button->mapToGlobal(QPoint(0, button->height())));
}

void RoomWidget::rename() {
	if (!_show || !_show->valid()) {
		return;
	}
	const auto weak = base::make_weak(_room.get());
	_show->showBox(Box(NameBox, NameBoxArgs{
		.title = tr::lng_oblivion_room_rename(),
		.button = tr::lng_oblivion_room_rename_button(),
		.value = _room->state().title,
		.done = [=](QString name, Fn<void(bool)> finished) {
			if (const auto strong = weak.get()) {
				strong->rename(name);
			}
			finished(true);
		},
	}));
}

void RoomWidget::leave() {
	if (!_show || !_show->valid()) {
		return;
	}
	const auto weak = base::make_weak(_room.get());
	const auto alone = (_room->state().members.size() < 2);
	_show->showBox(Ui::MakeConfirmBox({
		.text = (_room->owner() && !alone)
			? tr::lng_oblivion_room_leave_sure_owner(tr::now)
			: tr::lng_oblivion_room_leave_sure(tr::now),
		.confirmed = [=](Fn<void()> close) {
			if (const auto strong = weak.get()) {
				strong->leave();
			}
			close();
		},
		.confirmText = tr::lng_oblivion_room_leave_button(tr::now),
	}));
}

void RoomWidget::closeForAll() {
	if (!_show || !_show->valid()) {
		return;
	}
	const auto weak = base::make_weak(_room.get());
	_show->showBox(Ui::MakeConfirmBox({
		.text = tr::lng_oblivion_room_close_all_sure(tr::now),
		.confirmed = [=](Fn<void()> close) {
			if (const auto strong = weak.get()) {
				strong->closeForAll();
			}
			close();
		},
		.confirmText = tr::lng_oblivion_room_close_all_button(tr::now),
		.confirmStyle = &st::attentionBoxButton,
	}));
}

// ---- The window.

class RoomWindow final : public Ui::RpWindow {
public:
	RoomWindow(not_null<Main::Session*> session, RoomState &&state);
	~RoomWindow();

	[[nodiscard]] not_null<Room*> room() const {
		return _room.get();
	}
	void detach();

protected:
	bool eventHook(QEvent *e) override;

private:
	void updateTitle();

	std::unique_ptr<Room> _room;
	const std::unique_ptr<Ui::LayerManager> _layers;
	base::unique_qptr<RoomWidget> _widget; // Destroyed before the room.

};

[[nodiscard]] std::vector<std::unique_ptr<RoomWindow>> &Windows() {
	static auto result = std::vector<std::unique_ptr<RoomWindow>>();
	return result;
}

struct LockState {
	bool locked = false;
	std::vector<QPointer<RoomWindow>> hidden;
};

[[nodiscard]] LockState &WindowsLock() {
	static auto result = LockState();
	return result;
}

void RemoveWindow(not_null<RoomWindow*> window) {
	auto &list = Windows();
	const auto i = ranges::find(
		list,
		window.get(),
		&std::unique_ptr<RoomWindow>::get);
	if (i != end(list)) {
		// The window is destroyed after it has left the list.
		auto taken = std::move(*i);
		list.erase(i);
	}
}

[[nodiscard]] RoomWindow *WindowFor(not_null<Main::Session*> session) {
	for (const auto &window : Windows()) {
		if (window->room()->session() == session) {
			return window.get();
		}
	}
	return nullptr;
}

void ActivateWindow(not_null<RoomWindow*> window) {
	if (WindowsLock().locked) {
		return;
	} else if (window->isMinimized()) {
		window->showNormal();
	} else if (window->isHidden()) {
		window->show();
	}
	window->raise();
	window->activateWindow();
}

[[nodiscard]] QRect DefaultGeometry() {
	const auto size = QSize(Scaled(kWindowWidth), Scaled(kWindowHeight));
	auto screen = QGuiApplication::primaryScreen();
	if (Core::IsAppLaunched()) {
		if (const auto window = Core::App().activeWindow()) {
			if (const auto other = window->widget()->screen()) {
				screen = other;
			}
		}
	}
	const auto available = screen ? screen->availableGeometry() : QRect();
	if (!available.isValid()) {
		return QRect(QPoint(100, 100), size);
	}
	const auto fitted = size.boundedTo(available.size());
	return QRect(
		available.x() + (available.width() - fitted.width()) * 2 / 3,
		available.y() + (available.height() - fitted.height()) / 2,
		fitted.width(),
		fitted.height());
}

RoomWindow::RoomWindow(not_null<Main::Session*> session, RoomState &&state)
: _room(std::make_unique<Room>(Room::Descriptor{
	.session = session,
	.selfId = Cloud::For(session).userId(),
	.state = std::move(state),
}))
, _layers(std::make_unique<Ui::LayerManager>(body())) {
	_layers->setHideByBackgroundClick(true);
	setMinimumSize(QSize(Scaled(kWindowMinWidth), Scaled(kWindowMinHeight)));
	setGeometry(DefaultGeometry());

	body()->paintRequest(
	) | rpl::on_next([=](QRect clip) {
		QPainter(body().get()).fillRect(clip, st::windowBg);
	}, body()->lifetime());

	_widget = base::make_unique_q<RoomWidget>(
		body().get(),
		_room.get(),
		_layers->uiShow(),
		QString());
	body()->sizeValue(
	) | rpl::on_next([=](QSize size) {
		_widget->setGeometry(QRect(QPoint(), size));
	}, _widget->lifetime());
	_widget->show();

	_widget->closeRequests(
	) | rpl::on_next([=] {
		close();
	}, _widget->lifetime());

	_room->changes(
	) | rpl::on_next([=](Changes changes) {
		if (changes & (Changes(Change::Room) | Change::Reloaded)) {
			updateTitle();
		}
	}, lifetime());
	updateTitle();

	// «Отключиться» or «Удалить мои данные» in the settings: the room
	// can't go on without the cloud.
	Cloud::For(session).readyValue(
	) | rpl::filter([](bool ready) {
		return !ready;
	}) | rpl::take(1) | rpl::on_next([=](bool) {
		crl::on_main(this, [=] {
			close();
		});
	}, lifetime());
}

RoomWindow::~RoomWindow() {
	_layers->hideAll(anim::type::instant);
	_widget = nullptr;
	_room = nullptr;
}

void RoomWindow::detach() {
	_layers->hideAll(anim::type::instant);
	_room->detach();
}

void RoomWindow::updateTitle() {
	const auto &title = _room->state().title;
	setTitle(tr::lng_oblivion_room_window_title(
		tr::now,
		lt_title,
		title.isEmpty() ? tr::lng_oblivion_room_title_default(tr::now) : title));
}

bool RoomWindow::eventHook(QEvent *e) {
	if (e->type() == QEvent::Close) {
		e->accept();
		crl::on_main(this, [=] {
			RemoveWindow(this);
		});
		return true;
	}
	return Ui::RpWindow::eventHook(e);
}

// The account is going away: its room is cut off at once, the window is
// destroyed from the event loop.
void DropSession(not_null<Main::Session*> session) {
	const auto window = WindowFor(session);
	if (!window) {
		return;
	}
	window->detach();
	window->hide();
	const auto weak = QPointer<RoomWindow>(window);
	crl::on_main([=] {
		if (const auto strong = weak.data()) {
			RemoveWindow(strong);
		}
	});
}

// ---- The entry points.

[[nodiscard]] std::vector<RoomsRow> ParseRows(const QJsonObject &json) {
	auto result = std::vector<RoomsRow>();
	for (const auto &entry : json.value(u"rooms"_q).toArray()) {
		const auto object = entry.toObject();
		const auto code = Cloud::NormalizeRoomCode(
			Cloud::JsonText(object.value(u"code"_q), 16));
		if (code.isEmpty() || result.size() >= 32) {
			continue;
		}
		result.push_back({
			.code = code,
			.title = Cloud::JsonText(object.value(u"title"_q), kTitleLimit),
			.owner = (object.value(u"role"_q).toString() == u"owner"_q),
			.members = int(std::clamp(
				Cloud::JsonInt(object.value(u"members"_q)),
				int64(0),
				int64(1000))),
			.online = int(std::clamp(
				Cloud::JsonInt(object.value(u"online"_q)),
				int64(0),
				int64(1000))),
		});
	}
	return result;
}

[[nodiscard]] bool RoomsAvailable(
		not_null<Cloud::Account*> account,
		const std::shared_ptr<Ui::Show> &show) {
	if (account->hello().valid && !account->feature("rooms")) {
		Toast(show, tr::lng_oblivion_room_error_unavailable(tr::now));
		return false;
	}
	return true;
}

// POST .../join and the window. finished(ok) is always called, unless
// the session has gone meanwhile.
void JoinAndOpen(
		not_null<Main::Session*> session,
		std::shared_ptr<Ui::Show> show,
		const QString &code,
		Fn<void(bool ok)> finished = nullptr) {
	if (const auto window = WindowFor(session)) {
		if (window->room()->code() == code
			&& window->room()->state().gone == Gone::No) {
			ActivateWindow(window);
			if (finished) {
				finished(true);
			}
			return;
		}
	}
	const auto account = &Cloud::For(session);
	const auto weak = base::make_weak(session);
	account->request(
		Cloud::PostRequest(u"/v1/rooms/"_q + code + u"/join"_q),
		[=](const Cloud::Response &response) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			auto state = ParseRoom(
				response.json.value(u"room"_q).toObject(),
				Cloud::For(strong).userId());
			const auto ok = state.valid();
			if (ok) {
				ShowRoomWindow(strong, std::move(state));
			} else {
				ShowRoomError(show, { .type = Cloud::Error::Type::Protocol });
			}
			if (finished) {
				finished(ok);
			}
		},
		[=](const Cloud::Error &error) {
			if (!weak) {
				return;
			}
			if (error.status == 404) {
				Toast(show, tr::lng_oblivion_room_join_not_found(tr::now));
			} else {
				ShowRoomError(show, error);
			}
			if (finished) {
				finished(false);
			}
		});
}

void CreateWithTitle(
		not_null<Main::Session*> session,
		std::shared_ptr<Ui::Show> show,
		const QString &title,
		Fn<void(bool ok)> finished = nullptr) {
	const auto account = &Cloud::For(session);
	const auto weak = base::make_weak(session);
	auto body = QJsonObject();
	body.insert(u"title"_q, title.trimmed().left(kTitleLimit));
	account->request(
		Cloud::PostRequest(u"/v1/rooms"_q, std::move(body)),
		[=](const Cloud::Response &response) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			auto state = ParseRoom(
				response.json.value(u"room"_q).toObject(),
				Cloud::For(strong).userId());
			const auto ok = state.valid();
			if (ok) {
				ShowRoomWindow(strong, std::move(state));
			} else {
				ShowRoomError(show, { .type = Cloud::Error::Type::Protocol });
			}
			if (finished) {
				finished(ok);
			}
		},
		[=](const Cloud::Error &error) {
			if (!weak) {
				return;
			}
			ShowRoomError(show, error);
			if (finished) {
				finished(false);
			}
		});
}

} // namespace

void ShowRoomWindow(not_null<Main::Session*> session, RoomState &&state) {
	if (!state.valid()) {
		return;
	}
	if (const auto window = WindowFor(session)) {
		if (window->room()->code() == state.code
			&& window->room()->state().gone == Gone::No) {
			ActivateWindow(window);
			return;
		}
		// One room at a time: the user stays a member of the other one.
		window->hide();
		RemoveWindow(window);
	}
	auto &list = Windows();
	list.push_back(std::make_unique<RoomWindow>(session, std::move(state)));
	const auto result = list.back().get();
	if (WindowsLock().locked) {
		WindowsLock().hidden.push_back(result);
		return;
	}
	result->show();
	ActivateWindow(result);
}

Room *ActiveRoom(not_null<Main::Session*> session) {
	const auto window = WindowFor(session);
	return (window && window->room()->state().gone == Gone::No)
		? window->room().get()
		: nullptr;
}

object_ptr<Ui::RpWidget> CreateRoomWidget(
		QWidget *parent,
		not_null<Room*> room,
		std::shared_ptr<Ui::Show> show,
		const QString &tab) {
	return object_ptr<RoomWidget>(parent, room, std::move(show), tab);
}

void Start(not_null<Main::Session*> session) {
	static auto Cleaned = false;
	if (!Cleaned) {
		Cleaned = true;
		CleanupRoomFolders();
	}
	// Registered after Cloud::For(session): runs before the account of
	// the cloud is destroyed.
	const auto raw = session.get();
	session->lifetime().add([=] {
		DropSession(raw);
	});
}

void Forget(not_null<Main::Session*> session) {
	DropSession(session);
	CleanupRoomFolders(session->userId().bare);
}

void HideWindowsForLock() {
	auto &lock = WindowsLock();
	lock.locked = true;
	for (const auto &window : Windows()) {
		if (!window->isHidden()) {
			window->hide();
			lock.hidden.push_back(window.get());
		}
	}
}

void RestoreWindowsAfterLock() {
	auto &lock = WindowsLock();
	lock.locked = false;
	for (const auto &window : base::take(lock.hidden)) {
		if (window) {
			window->show();
		}
	}
}

void CloseAllWindows() {
	auto windows = base::take(Windows());
	windows.clear();
}

void AddMainMenuEntry(
		not_null<Ui::VerticalLayout*> menu,
		not_null<Window::SessionController*> controller) {
	const auto wrap = menu->add(
		object_ptr<Ui::SlideWrap<Ui::SettingsButton>>(
			menu,
			::Settings::CreateButtonWithIcon(
				menu,
				tr::lng_oblivion_room_settings(),
				st::mainMenuButton,
				{ &st::menuIconGroups })));
	wrap->setDuration(0)->toggleOn(rpl::single(
		rpl::empty
	) | rpl::then(
		Oblivion::Get().changes()
	) | rpl::map([] {
		return Oblivion::Get().cloudRooms();
	}) | rpl::distinct_until_changed());
	wrap->entity()->setClickedCallback([=] {
		ShowRoomsBox(controller);
	});
}

void OpenAsRoom(
		not_null<Window::SessionController*> controller,
		const QString &title) {
	if (Oblivion::Get().cloudRooms()) {
		CreateRoom(controller, title);
	}
}

void ShowRoomsBox(not_null<Window::SessionController*> controller) {
	const auto weak = base::make_weak(controller);
	Cloud::RequireConsent(controller, crl::guard(controller, [=] {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		const auto session = &strong->session();
		const auto show = strong->uiShow();
		const auto account = &Cloud::For(session);
		if (!RoomsAvailable(account, show)) {
			return;
		}
		const auto weakAccount = base::make_weak(account);
		const auto weakSession = base::make_weak(session);
		strong->show(Box(RoomsBox, RoomsBoxArgs{
			.show = show,
			.load = [=](
					Fn<void(std::vector<RoomsRow>)> done,
					Fn<void(const Cloud::Error &)> fail) {
				const auto account = weakAccount.get();
				if (!account) {
					return;
				}
				account->request(
					Cloud::GetRequest(u"/v1/rooms"_q),
					[=](const Cloud::Response &response) {
						done(ParseRows(response.json));
					},
					fail);
			},
			.open = [=](const QString &code) {
				if (const auto session = weakSession.get()) {
					JoinAndOpen(session, show, code);
				}
			},
			.create = [=] {
				if (const auto strong = weak.get()) {
					CreateRoom(strong);
				}
			},
			.join = [=](const QString &typed) {
				const auto code = ExtractCode(typed);
				const auto strong = weak.get();
				if (strong && !code.isEmpty()) {
					OpenLink(strong, code);
				}
			},
		}));
	}));
}

void CreateRoom(
		not_null<Window::SessionController*> controller,
		const QString &title) {
	const auto weak = base::make_weak(controller);
	Cloud::RequireConsent(controller, crl::guard(controller, [=] {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		const auto session = &strong->session();
		const auto show = strong->uiShow();
		if (!RoomsAvailable(&Cloud::For(session), show)) {
			return;
		}
		const auto weakSession = base::make_weak(session);
		if (!title.trimmed().isEmpty()) {
			CreateWithTitle(session, show, title);
			return;
		}
		const auto name = Cloud::For(session).me().name;
		strong->show(Box(NameBox, NameBoxArgs{
			.title = tr::lng_oblivion_room_create_title(),
			.about = tr::lng_oblivion_room_create_about(),
			.button = tr::lng_oblivion_room_create_button(),
			.value = name.isEmpty()
				? tr::lng_oblivion_room_title_default(tr::now)
				: tr::lng_oblivion_room_create_default(
					tr::now,
					lt_name,
					name).left(kTitleLimit),
			.done = [=](QString value, Fn<void(bool)> finished) {
				if (const auto session = weakSession.get()) {
					CreateWithTitle(session, show, value, finished);
				}
			},
		}));
	}));
}

void OpenLink(
		not_null<Window::SessionController*> controller,
		const QString &code) {
	const auto normalized = Cloud::NormalizeRoomCode(code);
	if (normalized.isEmpty()) {
		return;
	}
	const auto weak = base::make_weak(controller);
	Cloud::RequireConsent(controller, crl::guard(controller, [=] {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		const auto session = &strong->session();
		const auto show = strong->uiShow();
		const auto account = &Cloud::For(session);
		if (!RoomsAvailable(account, show)) {
			return;
		}
		if (const auto window = WindowFor(session)) {
			if (window->room()->code() == normalized
				&& window->room()->state().gone == Gone::No) {
				ActivateWindow(window);
				return;
			}
		}
		const auto weakSession = base::make_weak(session);
		account->request(
			Cloud::GetRequest(u"/v1/rooms/"_q + normalized + u"/preview"_q),
			[=](const Cloud::Response &response) {
				const auto session = weakSession.get();
				const auto strong = weak.get();
				if (!session || !strong) {
					return;
				}
				const auto &json = response.json;
				if (json.value(u"member"_q).toBool()) {
					// Joined already (the room was just closed here).
					JoinAndOpen(session, show, normalized);
					return;
				}
				const auto owner = json.value(u"owner"_q).toObject();
				const auto reason = json.value(u"can_join"_q).toBool(true)
					? QString()
					: Cloud::JsonText(json.value(u"reason"_q), 32);
				strong->show(Box(JoinBox, JoinBoxArgs{
					.code = normalized,
					.title = Cloud::JsonText(
						json.value(u"title"_q),
						kTitleLimit),
					.ownerName = Cloud::JsonText(
						owner.value(u"name"_q),
						kTitleLimit),
					.ownerId = Cloud::JsonUserId(owner.value(u"id"_q)),
					.members = int(std::clamp(
						Cloud::JsonInt(json.value(u"members"_q)),
						int64(0),
						int64(1000))),
					.maxMembers = int(std::clamp(
						Cloud::JsonInt(json.value(u"max_members"_q)),
						int64(0),
						int64(1000))),
					.reason = reason,
					.join = [=](Fn<void(bool)> finished) {
						if (const auto session = weakSession.get()) {
							JoinAndOpen(session, show, normalized, finished);
						}
					},
				}));
			},
			[=](const Cloud::Error &error) {
				if (!weak) {
					return;
				} else if (error.status == 404) {
					Toast(show, tr::lng_oblivion_room_join_not_found(tr::now));
				} else {
					ShowRoomError(show, error);
				}
			});
	}));
}

// ---- Snapshot scenes (OBLIVION_SELFTEST=ui).

namespace {

constexpr auto kSampleNow = int64(1791327935000);
constexpr auto kSampleSelf = uint64(9000000000000101ULL);
constexpr auto kSampleOwner = uint64(9000000000000100ULL);

[[nodiscard]] QString SampleText(const char *ru, const char *en) {
	return QString::fromUtf8(CurrentLanguageIsRussian() ? ru : en);
}

[[nodiscard]] Member SampleMember(
		uint64 id,
		QString name,
		bool owner,
		bool online,
		Rights rights) {
	return {
		.id = id,
		.name = std::move(name),
		.owner = owner,
		.rights = owner ? Rights::Everything() : rights,
		.joinedAt = kSampleNow - 3'600'000,
		.online = online,
	};
}

[[nodiscard]] QueueItem SampleItem(
		const QString &id,
		char sha,
		const QString &title,
		const QString &performer,
		int64 duration,
		uint64 addedBy) {
	return {
		.id = id,
		.media = QString(64, QChar(sha)),
		.size = 8'000'000,
		.mime = u"audio/mpeg"_q,
		.title = title,
		.performer = performer,
		.duration = duration,
		.fileName = title + u".mp3"_q,
		.addedBy = addedBy,
		.addedAt = kSampleNow - 600'000,
	};
}

enum class SampleKind {
	Owner,
	Guest,
	Empty,
};

[[nodiscard]] RoomState SampleState(SampleKind kind) {
	auto state = RoomState();
	state.code = u"K7QM2XPA9Z"_q;
	state.link = Cloud::MakeLink(Cloud::LinkKind::Room, state.code);
	state.title = SampleText("Ночной эфир", "Night air");
	state.rev = 7;
	state.settings.defaults = Rights::Everything();
	const auto owner = (kind != SampleKind::Guest);
	const auto self = kSampleSelf;
	const auto other = kSampleOwner;
	state.ownerId = owner ? self : other;
	state.owner = owner;
	auto limited = Rights::OnlyOwner();
	limited.add = true;
	state.rights = owner ? Rights::Everything() : limited;
	const auto me = SampleText("Аня", "Anna");
	const auto misha = SampleText("Миша", "Michael");
	state.members.push_back(SampleMember(
		self,
		me,
		owner,
		true,
		limited));
	if (kind == SampleKind::Empty) {
		return state;
	}
	state.members.push_back(SampleMember(
		other,
		misha,
		!owner,
		true,
		Rights::Everything()));
	state.members.push_back(SampleMember(
		9000000000000102ULL,
		SampleText("Лера Соколова", "Valerie Falcon"),
		false,
		true,
		limited));
	state.members.push_back(SampleMember(
		9000000000000103ULL,
		SampleText("Костя", "Constantine"),
		false,
		false,
		Rights::Everything()));
	state.members[2].music = {
		.itemId = u"a1"_q,
		.ready = false,
		.buffered = 40,
	};
	state.music.queue = {
		SampleItem(
			u"a1"_q,
			'3',
			u"Midnight City"_q,
			u"M83"_q,
			243'000,
			other),
		SampleItem(
			u"b2"_q,
			'7',
			SampleText("Звезда по имени Солнце", "A Star Called Sun"),
			SampleText("Кино", "Kino"),
			225'000,
			self),
		SampleItem(
			u"c3"_q,
			'b',
			u"Instant Crush"_q,
			u"Daft Punk"_q,
			337'000,
			9000000000000102ULL),
		SampleItem(
			u"d4"_q,
			'e',
			SampleText("Запись с репетиции", "Rehearsal take"),
			QString(),
			95'000,
			self),
	};
	state.music.state = {
		.itemId = u"a1"_q,
		.playing = true,
		.position = 61'000,
		.anchor = kSampleNow - 20'000,
		.repeat = Repeat::All,
		.rev = 12,
		.updatedBy = other,
	};
	const auto message = [&](
			int64 id,
			uint64 user,
			const QString &name,
			const QString &text,
			int64 ago) {
		state.chat.push_back({
			.id = id,
			.userId = user,
			.name = name,
			.text = text,
			.ts = kSampleNow - ago,
		});
	};
	message(1, other, misha, SampleText(
		"Всем привет! Ставлю M83, подпевайте",
		"Hi all! Putting M83 on, sing along"), 900'000);
	message(2, other, misha, SampleText(
		"Кто следующий в очереди?",
		"Who is next in the queue?"), 880'000);
	message(3, self, me, SampleText(
		"Я добавила Кино, будет после этого трека 🎸",
		"I added Kino, it goes right after this one 🎸"), 600'000);
	message(4, 9000000000000102ULL, SampleText("Лера Соколова", "Valerie"),
		SampleText(
			"У меня всё синхронно, даже через VPN. Звук отличный, "
			"давайте потом ещё холст откроем и порисуем вместе.",
			"Everything is in sync for me, even over a VPN. Sounds "
			"great, let us open the canvas later and draw together."),
		120'000);
	state.chatLastId = 4;
	return state;
}

class SceneHost final : public Ui::RpWidget {
public:
	SceneHost(
		QWidget *parent,
		Room::Descriptor &&descriptor,
		const QString &tab)
	: RpWidget(parent)
	, _room(std::make_unique<Room>(std::move(descriptor))) {
		_content = base::unique_qptr<Ui::RpWidget>(CreateRoomWidget(
			this,
			_room.get(),
			SelfTest::SceneShow(this),
			tab).release());
		_content->show();
		sizeValue(
		) | rpl::on_next([=](QSize size) {
			if (_content) {
				_content->setGeometry(QRect(QPoint(), size));
			}
		}, lifetime());
	}
	~SceneHost() {
		_content = nullptr;
	}

private:
	const std::unique_ptr<Room> _room;
	base::unique_qptr<Ui::RpWidget> _content;

};

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	const auto window = [](
			const QString &name,
			QSize size,
			Fn<Room::Descriptor()> make,
			const QString &tab = QString()) {
		RegisterScene(name, size, [=](not_null<Ui::RpWidget*> parent) {
			return Ui::CreateChild<SceneHost>(parent.get(), make(), tab);
		});
	};
	const auto descriptor = [](SampleKind kind) {
		return Room::Descriptor{
			.selfId = kSampleSelf,
			.state = SampleState(kind),
			.now = [] { return kSampleNow; },
		};
	};
	const auto size = QSize(Scaled(kWindowWidth), Scaled(kWindowHeight));
	window(u"room_window_owner"_q, size, [=] {
		auto result = descriptor(SampleKind::Owner);
		result.uploads.push_back({
			.id = 1,
			.title = u"Tame Impala — Let It Happen"_q,
			.ready = 3'400'000,
			.total = 9'100'000,
		});
		return result;
	});
	window(u"room_window_guest"_q, size, [=] {
		return descriptor(SampleKind::Guest);
	});
	window(u"room_window_guest_paused"_q, size, [=] {
		auto result = descriptor(SampleKind::Guest);
		result.state.music.state.playing = false;
		result.state.music.state.repeat = Repeat::One;
		return result;
	});
	window(u"room_window_empty"_q, size, [=] {
		return descriptor(SampleKind::Empty);
	});
	window(u"room_window_members"_q, size, [=] {
		auto result = descriptor(SampleKind::Owner);
		result.state.banned.push_back({
			.id = 9000000000000109ULL,
			.name = SampleText("Спамер", "Spammer"),
		});
		return result;
	}, u"members"_q);
	window(u"room_window_members_guest"_q, size, [=] {
		return descriptor(SampleKind::Guest);
	}, u"members"_q);
	window(u"room_window_chat"_q, size, [=] {
		return descriptor(SampleKind::Owner);
	}, u"chat"_q);
	window(u"room_window_chat_empty"_q, size, [=] {
		auto result = descriptor(SampleKind::Guest);
		result.state.chat.clear();
		result.state.rights.chat = false;
		return result;
	}, u"chat"_q);
	window(u"room_window_offline"_q, size, [=] {
		auto result = descriptor(SampleKind::Owner);
		result.connected = false;
		return result;
	});
	window(u"room_window_gone"_q, size, [=] {
		auto result = descriptor(SampleKind::Guest);
		result.state.gone = Gone::Kicked;
		return result;
	});
	window(
		u"room_window_narrow"_q,
		QSize(Scaled(kWindowMinWidth), Scaled(kWindowMinHeight)),
		[=] { return descriptor(SampleKind::Owner); });
	window(
		u"room_window_wide"_q,
		QSize(Scaled(760), Scaled(640)),
		[=] { return descriptor(SampleKind::Owner); });

	const auto boxSize = QSize(st::boxWideWidth * 2, 0);
	const auto joinArgs = [](const QString &reason) {
		return JoinBoxArgs{
			.code = u"K7QM2XPA9Z"_q,
			.title = SampleText("Ночной эфир", "Night air"),
			.ownerName = SampleText("Миша", "Michael"),
			.ownerId = kSampleOwner,
			.members = (reason == u"full"_q) ? 30 : 3,
			.maxMembers = 30,
			.reason = reason,
		};
	};
	RegisterBoxScene(u"room_join_box"_q, boxSize, [=](
			std::shared_ptr<Ui::Show> show) {
		return Box(JoinBox, joinArgs(QString()));
	});
	RegisterBoxScene(u"room_join_box_full"_q, boxSize, [=](
			std::shared_ptr<Ui::Show> show) {
		return Box(JoinBox, joinArgs(u"full"_q));
	});
	RegisterBoxScene(u"room_join_box_invite"_q, boxSize, [=](
			std::shared_ptr<Ui::Show> show) {
		return Box(JoinBox, joinArgs(u"invite_required"_q));
	});
	RegisterBoxScene(u"room_create_box"_q, boxSize, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(NameBox, NameBoxArgs{
			.title = tr::lng_oblivion_room_create_title(),
			.about = tr::lng_oblivion_room_create_about(),
			.button = tr::lng_oblivion_room_create_button(),
			.value = tr::lng_oblivion_room_create_default(
				tr::now,
				lt_name,
				SampleText("Аня", "Anna")),
		});
	});
	const auto rooms = [=](
			const QString &name,
			Fn<void(
				Fn<void(std::vector<RoomsRow>)>,
				Fn<void(const Cloud::Error &)>)> load) {
		RegisterBoxScene(name, boxSize, [=](std::shared_ptr<Ui::Show> show) {
			return Box(RoomsBox, RoomsBoxArgs{ .show = show, .load = load });
		});
	};
	rooms(u"room_list_box"_q, [](
			Fn<void(std::vector<RoomsRow>)> done,
			Fn<void(const Cloud::Error &)> fail) {
		done({
			{
				.code = u"K7QM2XPA9Z"_q,
				.title = SampleText("Ночной эфир", "Night air"),
				.owner = true,
				.members = 4,
				.online = 3,
			},
			{
				.code = u"M3KQ8XTR2B"_q,
				.title = SampleText("Кино по пятницам", "Friday movies"),
				.members = 12,
				.online = 1,
			},
		});
	});
	rooms(u"room_list_box_empty"_q, [](
			Fn<void(std::vector<RoomsRow>)> done,
			Fn<void(const Cloud::Error &)> fail) {
		done({});
	});
	rooms(u"room_list_box_loading"_q, [](
			Fn<void(std::vector<RoomsRow>)> done,
			Fn<void(const Cloud::Error &)> fail) {
	});
	rooms(u"room_list_box_error"_q, [](
			Fn<void(std::vector<RoomsRow>)> done,
			Fn<void(const Cloud::Error &)> fail) {
		fail({ .type = Cloud::Error::Type::Network });
	});
});

} // namespace
} // namespace Oblivion::Rooms
