/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_look_ui.h"

#include "lang/lang_keys.h"
#include "oblivion/oblivion_look.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/abstract_button.h"
#include "ui/effects/animation_value.h"
#include "ui/effects/animations.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/text/text.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/themes/window_theme.h"
#include "window/window_session_controller.h"
#include "styles/style_basic.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

namespace Oblivion::Look {
namespace {

// At the 100% scale.
constexpr auto kTilePadding = 8;
constexpr auto kTileGap = 6;
constexpr auto kTileRadius = 12;
constexpr auto kPreviewRatio = 0.64;
constexpr auto kPreviewRadius = 10;
constexpr auto kNameSkip = 8;
constexpr auto kAboutSkip = 2;
constexpr auto kAboutLines = 3;
// A text counts its height for a width not less than this one, and the
// default is "never wrap": the tiles are narrower than a usual text.
constexpr auto kAboutMinWidth = 40;
constexpr auto kCheckSize = 20;
constexpr auto kCheckSkip = 6;
constexpr auto kRingSkip = 2;
// The preview is drawn on a grid 100 units high.
constexpr auto kPreviewUnits = 100.;
constexpr auto kKitSize = QSize(520, 380);

[[nodiscard]] int Scaled(int value) {
	return style::ConvertScale(value);
}

[[nodiscard]] rpl::producer<QString> AboutValue(int look) {
	switch (look) {
	case kNative: return tr::lng_oblivion_look_native_about();
	case kNightAir: return tr::lng_oblivion_look_night_air_about();
	case kSilence: return tr::lng_oblivion_look_silence_about();
	}
	return tr::lng_oblivion_look_plain_about();
}

// radial-gradient(rx ry at center, color, transparent 70%).
void PaintSpot(
		QPainter &p,
		QPointF center,
		float64 rx,
		float64 ry,
		const QColor &color) {
	if (!color.alpha() || rx <= 0. || ry <= 0.) {
		return;
	}
	auto gradient = QRadialGradient(QPointF(), 1.);
	gradient.setColorAt(0., color);
	gradient.setColorAt(0.7, anim::with_alpha(color, 0.));
	gradient.setColorAt(1., anim::with_alpha(color, 0.));
	p.save();
	p.translate(center);
	p.scale(rx, ry);
	p.fillRect(QRectF(-1., -1., 2., 2.), gradient);
	p.restore();
}

void PaintBar(
		QPainter &p,
		float64 x,
		float64 y,
		float64 width,
		float64 height,
		const QColor &color) {
	if (width <= 0. || height <= 0.) {
		return;
	}
	p.setPen(Qt::NoPen);
	p.setBrush(color);
	p.drawRoundedRect(QRectF(x, y, width, height), height / 2., height / 2.);
}

void PaintStroke(
		QPainter &p,
		const QRectF &rect,
		float64 radius,
		const QColor &color,
		float64 width) {
	if (!color.alpha()) {
		return;
	}
	const auto half = width / 2.;
	p.setPen(QPen(color, width));
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(
		rect.marginsRemoved({ half, half, half, half }),
		std::max(radius - half, 0.),
		std::max(radius - half, 0.));
}

class Tile final : public Ui::AbstractButton {
public:
	Tile(QWidget *parent, int look);

	void setSelected(bool selected, anim::type animated);
	[[nodiscard]] int heightFor(int width) const;

private:
	void paintEvent(QPaintEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;

	[[nodiscard]] QRect previewRect(int width) const;

	const int _look = 0;
	Ui::Text::String _name;
	Ui::Text::String _about;
	Ui::Animations::Simple _selectedAnimation;
	bool _selected = false;

};

Tile::Tile(QWidget *parent, int look)
: AbstractButton(parent)
, _look(look)
, _about(Scaled(kAboutMinWidth)) {
	NameValue(look) | rpl::on_next([=](const QString &value) {
		_name.setText(st::semiboldTextStyle, value);
		update();
	}, lifetime());
	AboutValue(look) | rpl::on_next([=](const QString &value) {
		_about.setText(st::defaultTextStyle, value);
		update();
	}, lifetime());
}

void Tile::setSelected(bool selected, anim::type animated) {
	if (_selected == selected) {
		if (animated == anim::type::instant) {
			_selectedAnimation.stop();
			update();
		}
		return;
	}
	_selected = selected;
	if (animated == anim::type::normal) {
		_selectedAnimation.start(
			[=] { update(); },
			_selected ? 0. : 1.,
			_selected ? 1. : 0.,
			st::universalDuration);
	} else {
		_selectedAnimation.stop();
		update();
	}
}

QRect Tile::previewRect(int width) const {
	const auto padding = Scaled(kTilePadding);
	const auto inner = std::max(width - 2 * padding, 1);
	return QRect(padding, padding, inner, qRound(inner * kPreviewRatio));
}

int Tile::heightFor(int width) const {
	const auto padding = Scaled(kTilePadding);
	const auto inner = std::max(width - 2 * padding, 1);
	const auto line = st::defaultTextStyle.font->height;
	return padding
		+ previewRect(width).height()
		+ Scaled(kNameSkip)
		+ st::semiboldTextStyle.font->height
		+ Scaled(kAboutSkip)
		+ std::min(_about.countHeight(inner), kAboutLines * line)
		+ padding;
}

void Tile::onStateChanged(State was, StateChangeSource source) {
	update();
}

void Tile::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	auto hq = PainterHighQualityEnabler(p);

	const auto selected = _selectedAnimation.value(_selected ? 1. : 0.);
	if (isOver() || isDown()) {
		const auto radius = Scaled(kTileRadius);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(rect(), radius, radius);
	}

	const auto preview = previewRect(width());
	const auto radius = float64(Scaled(kPreviewRadius));
	PaintPreview(p, preview, _look, ThemeDark());
	// A preview may be of the colour of the box itself.
	PaintStroke(
		p,
		QRectF(preview),
		radius,
		st::shadowFg->c,
		style::ConvertScaleExact(1.));
	if (selected > 0.) {
		const auto stroke = style::ConvertScaleExact(2.);
		const auto skip = Scaled(kRingSkip) + stroke;
		p.setOpacity(selected);
		PaintStroke(
			p,
			QRectF(preview).marginsAdded({ skip, skip, skip, skip }),
			radius + skip,
			st::windowBgActive->c,
			stroke);

		// The mark of the current look.
		const auto size = Scaled(kCheckSize);
		const auto check = QRectF(
			preview.x() + preview.width() - Scaled(kCheckSkip) - size,
			preview.y() + Scaled(kCheckSkip),
			size,
			size);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgActive);
		p.drawEllipse(check);
		auto pen = QPen(st::windowFgActive->c, style::ConvertScaleExact(2.));
		pen.setCapStyle(Qt::RoundCap);
		pen.setJoinStyle(Qt::RoundJoin);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		const auto points = std::array{
			QPointF(
				check.x() + size * 0.28,
				check.y() + size * 0.52),
			QPointF(
				check.x() + size * 0.44,
				check.y() + size * 0.68),
			QPointF(
				check.x() + size * 0.73,
				check.y() + size * 0.36),
		};
		p.drawPolyline(points.data(), int(points.size()));
		p.setOpacity(1.);
	}

	const auto padding = Scaled(kTilePadding);
	const auto inner = std::max(width() - 2 * padding, 1);
	auto top = preview.y() + preview.height() + Scaled(kNameSkip);
	p.setPen(anim::color(st::windowFg, st::windowActiveTextFg, selected));
	_name.drawLeftElided(p, padding, top, inner, width());
	top += st::semiboldTextStyle.font->height + Scaled(kAboutSkip);
	p.setPen(st::windowSubTextFg);
	_about.drawLeftElided(p, padding, top, inner, width(), kAboutLines);
}

// Everything the box shows and does, it can be created without a window
// (see oblivion_ui_snapshots.h).
struct BoxArgs {
	rpl::producer<int> chosen;
	Fn<void(int look)> choose;
	bool paused = false;
};

void LookBox(not_null<Ui::GenericBox*> box, BoxArgs &&args) {
	box->setTitle(tr::lng_oblivion_look_title());
	box->setWidth(st::boxWideWidth);

	if (args.paused) {
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_oblivion_look_paused(),
				st::boxLabel),
			style::margins(
				st::boxRowPadding.left(),
				0,
				st::boxRowPadding.right(),
				Scaled(kNameSkip)));
	}

	// The tiles have a padding of their own for the hover and the ring.
	const auto side = std::max(
		st::boxRowPadding.left() - Scaled(kTilePadding),
		0);
	const auto grid = box->addRow(
		object_ptr<Ui::RpWidget>(box),
		style::margins(side, 0, side, 0));
	auto tiles = std::vector<not_null<Tile*>>();
	tiles.reserve(kCount);
	for (auto look = 0; look != kCount; ++look) {
		const auto tile = Ui::CreateChild<Tile>(grid, look);
		tile->setClickedCallback([=, choose = args.choose] {
			if (choose) {
				choose(look);
			}
		});
		tiles.push_back(tile);
	}
	grid->widthValue() | rpl::on_next([=](int width) {
		if (width <= 0) {
			return;
		}
		const auto gap = Scaled(kTileGap);
		const auto column = (width - gap) / 2;
		const auto count = int(tiles.size());
		auto top = 0;
		for (auto from = 0; from < count; from += 2) {
			const auto till = std::min(from + 2, count);
			auto height = 0;
			for (auto i = from; i != till; ++i) {
				height = std::max(height, tiles[i]->heightFor(column));
			}
			for (auto i = from; i != till; ++i) {
				tiles[i]->setGeometry(
					(i == from) ? 0 : (width - column),
					top,
					column,
					height);
			}
			top += height + gap;
		}
		grid->resize(width, std::max(top - gap, 0));
	}, grid->lifetime());

	const auto initialized = box->lifetime().make_state<bool>(false);
	std::move(
		args.chosen
	) | rpl::on_next([=](int look) {
		// The current choice is shown without an animation on open.
		const auto animated = *initialized
			? anim::type::normal
			: anim::type::instant;
		*initialized = true;
		for (auto i = 0; i != int(tiles.size()); ++i) {
			tiles[i]->setSelected(i == look, animated);
		}
	}, box->lifetime());

	Ui::AddSkip(box->verticalLayout());
	Ui::AddDividerText(
		box->verticalLayout(),
		tr::lng_oblivion_look_note());

	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
}

// A sheet with everything the painting helpers of oblivion_look.h draw,
// to see a look without the screens that use it.
class Kit final : public Ui::RpWidget {
public:
	using RpWidget::RpWidget;

private:
	void paintEvent(QPaintEvent *e) override;

};

void Kit::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	if (!PaintGround(p, rect(), size())) {
		p.fillRect(rect(), st::boxDividerBg);
	}
	const auto accent = st::windowActiveTextFg->c;
	const auto text = [&](
			int x,
			int y,
			const QString &value,
			const QColor &color,
			const style::font &font = st::normalFont) {
		p.setFont(font);
		p.setPen(color);
		p.drawText(x, y + font->ascent, value);
		return font->width(value);
	};
	const auto label = [&](int x, int y, QString value) {
		if (CapsLabels()) {
			value = value.toUpper();
		}
		text(x, y, value, Color(Role::SubText));
	};
	const auto chip = [&](int x, int y, const QString &value, Chip kind) {
		const auto height = Scaled(26);
		const auto padding = HasCards() ? Scaled(11) : 0;
		const auto width = st::normalFont->width(value) + 2 * padding;
		PaintChip(
			p,
			QRectF(x, y, width, height),
			anim::with_alpha(accent, 0.14),
			ChipRadius(height, height / 2),
			kind);
		text(
			x + padding,
			y + (height - st::normalFont->height) / 2,
			value,
			ChipText(kind, accent));
		return width + Scaled(8);
	};

	// A card that lies on the ground of a window of the mod.
	const auto card = QRect(Scaled(16), Scaled(16), Scaled(236), Scaled(232));
	PaintCard(p, card, st::windowBg->c, Scaled(8), Surface::Ground);
	const auto left = card.x() + Scaled(14);
	auto top = card.y() + Scaled(12);
	label(left, top, u"Карточка на фоне"_q);
	top += Scaled(28);
	auto x = left;
	x += chip(x, top, u"слушает: Кино"_q, Chip::Accent);
	chip(x, top, u"КЕНТЫ"_q, Chip::Neutral);
	top += Scaled(34);
	chip(left, top, u"в комнате Ночной эфир"_q, Chip::Room);
	top += Scaled(40);

	const auto button = QRectF(left, top, Scaled(92), Scaled(32));
	PaintAccentGradient(p, button, Scaled(16), st::activeButtonBg->c);
	p.setFont(st::semiboldFont);
	p.setPen(Color(Role::OnAccent, st::activeButtonFg->c));
	p.drawText(button, u"Войти"_q, style::al_center);
	const auto loud = QRectF(
		left + Scaled(104),
		top + Scaled(5),
		Scaled(34),
		Scaled(22));
	p.setPen(Qt::NoPen);
	p.setBrush(Color(Role::Highlight));
	{
		auto hq = PainterHighQualityEnabler(p);
		p.drawRoundedRect(loud, loud.height() / 2., loud.height() / 2.);
	}
	p.setPen(Color(Role::OnHighlight));
	p.drawText(loud, u"12"_q, style::al_center);
	top += Scaled(44);

	const auto row = QRectF(
		card.x() + Scaled(8),
		top,
		card.width() - Scaled(16),
		Scaled(40));
	PaintSelected(p, row, st::windowBgOver->c, Scaled(8));
	text(
		left + Scaled(4),
		top + (Scaled(40) - st::normalFont->height) / 2,
		u"Выбранная строка"_q,
		Color(Role::Text));
	top += Scaled(48);
	PaintDivider(
		p,
		QRectF(left, top, card.width() - Scaled(28), Scaled(1)),
		st::shadowFg->c);

	// The big «сейчас слушает» block.
	const auto live = QRectF(
		Scaled(16),
		card.y() + card.height() + Scaled(16),
		card.width(),
		Scaled(56));
	PaintChip(
		p,
		live,
		anim::with_alpha(accent, 0.14),
		Scaled(12),
		Chip::Live);
	label(
		int(live.x()) + Scaled(14),
		int(live.y()) + Scaled(10),
		u"Слушает"_q);
	text(
		int(live.x()) + Scaled(14),
		int(live.y()) + Scaled(28),
		u"Кино — Звезда по имени Солнце"_q,
		ChipText(Chip::Live, accent),
		st::semiboldFont);

	// A surface of Telegram (a box, the profile): the cover of a profile
	// and a card on it.
	const auto panel = QRect(
		Scaled(268),
		Scaled(16),
		Scaled(236),
		Scaled(348));
	p.fillRect(panel, st::windowBg);
	const auto cover = QRect(panel.x(), panel.y(), panel.width(), Scaled(92));
	if (!PaintCover(p, cover)) {
		p.fillRect(cover, st::windowBgOver);
	}
	const auto raised = QRectF(
		panel.x() + Scaled(12),
		cover.y() + cover.height() + Scaled(14),
		panel.width() - Scaled(24),
		Scaled(76));
	PaintCard(p, raised, st::windowBgOver->c, Scaled(10), Surface::Window);
	label(
		int(raised.x()) + Scaled(14),
		int(raised.y()) + Scaled(12),
		u"Oblivion"_q);
	text(
		int(raised.x()) + Scaled(14),
		int(raised.y()) + Scaled(36),
		u"ночью на связи, днём сплю"_q,
		Color(Role::Text),
		st::semiboldFont);

	// The cover of a track in the room.
	const auto side = Scaled(96);
	const auto track = QRectF(
		panel.x() + (panel.width() - side) / 2.,
		raised.y() + raised.height() + Scaled(30),
		side,
		side);
	auto hq = PainterHighQualityEnabler(p);
	const auto ringed = PaintCoverRing(p, track) > 0;
	const auto corner = ringed
		? (side / 2.)
		: float64(AvatarRadius(side) / 3);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(15, 15, 19));
	p.drawRoundedRect(track, corner, corner);
	p.setBrush(QColor(255, 224, 122));
	p.drawEllipse(track.center(), side * 0.26, side * 0.26);
	p.setBrush(QColor(7, 7, 10));
	p.drawEllipse(
		track.center() - QPointF(side * 0.05, -side * 0.04),
		side * 0.22,
		side * 0.22);
}

[[nodiscard]] QString SceneName(const QString &name, int look) {
	return name + u"_look%1"_q.arg(look);
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	// The chooser: the plain look and each of the three, with that one
	// marked as the current.
	const auto size = QSize(st::boxWideWidth * 2, 0);
	const auto chooser = [](int look) {
		return [=](std::shared_ptr<Ui::Show> show)
		-> object_ptr<Ui::BoxContent> {
			return Box(LookBox, BoxArgs{
				.chosen = rpl::single(look),
				.choose = [](int) {},
			});
		};
	};
	RegisterBoxScene(u"look_box"_q, size, chooser(kPlain));
	for (auto look = 1; look != kCount; ++look) {
		RegisterBoxScene(SceneName(u"look_box"_q, look), size, [=](
				std::shared_ptr<Ui::Show> show) {
			ShowInScene(look, show->toastParent());
			return chooser(look)(show);
		});
	}
	RegisterBoxScene(u"look_box_paused"_q, size, [](
			std::shared_ptr<Ui::Show> show) -> object_ptr<Ui::BoxContent> {
		return Box(LookBox, BoxArgs{
			.chosen = rpl::single(int(kNightAir)),
			.choose = [](int) {},
			.paused = true,
		});
	});

	// The painting helpers.
	const auto kit = [](not_null<Ui::RpWidget*> parent) -> QWidget* {
		return Ui::CreateChild<Kit>(parent.get());
	};
	RegisterScene(u"look_kit"_q, kKitSize, kit);
	RegisterScenes(u"look_kit"_q, kKitSize, kit);
});

} // namespace

void ShowBox(not_null<Window::SessionController*> controller) {
	controller->show(Box(LookBox, BoxArgs{
		.chosen = ChosenValue(),
		.choose = [](int look) { Set(look); },
		.paused = Window::Theme::Background()->editingTheme().has_value(),
	}));
}

rpl::producer<QString> NameValue(int look) {
	switch (look) {
	case kNative: return tr::lng_oblivion_look_native();
	case kNightAir: return tr::lng_oblivion_look_night_air();
	case kSilence: return tr::lng_oblivion_look_silence();
	}
	return tr::lng_oblivion_look_plain();
}

rpl::producer<QString> ChosenNameValue() {
	return ChosenValue(
	) | rpl::map([](int look) {
		return NameValue(look);
	}) | rpl::flatten_latest();
}

void PaintPreview(QPainter &p, const QRect &rect, int look, bool dark) {
	if (rect.isEmpty()) {
		return;
	}
	const auto c = PreviewFor(look, dark);
	const auto area = QRectF(rect);
	const auto x = area.x();
	const auto y = area.y();
	const auto w = area.width();
	const auto h = area.height();
	const auto u = h / kPreviewUnits;
	const auto line = std::max(u, 1.);

	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	auto clip = QPainterPath();
	clip.addRoundedRect(area, 10. * u, 10. * u);
	p.setClipPath(clip, Qt::IntersectClip);

	// The chats list at the left, the chat at the right.
	const auto listWidth = std::round(w * 0.43);
	const auto list = QRectF(x, y, listWidth, h);
	const auto chat = QRectF(x + listWidth, y, w - listWidth, h);
	p.fillRect(area, c.ground);
	if (c.gradients) {
		PaintSpot(
			p,
			QPointF(x + w * 0.10, y - h * 0.06),
			w * 0.66,
			h * 0.55,
			c.glow[0]);
		PaintSpot(
			p,
			QPointF(x + w * 1.06, y + h * 0.20),
			w * 0.61,
			h * 0.52,
			c.glow[1]);
		PaintSpot(
			p,
			QPointF(x + w * 0.58, y + h * 1.12),
			w * 0.73,
			h * 0.60,
			c.glow[2]);
	} else {
		p.fillRect(chat, c.chat);
		p.fillRect(list, c.list);
		p.fillRect(QRectF(chat.x(), y, line, h), c.divider);
	}

	const auto rowHeight = h / 3.;
	const auto avatar = 15. * u;
	const auto avatarRadius = avatar * c.avatarPercent / 100.;
	for (auto i = 0; i != 3; ++i) {
		const auto top = y + i * rowHeight;
		const auto selected = (i == 1);
		const auto row = QRectF(x, top, listWidth, rowHeight);
		if (selected) {
			if (!c.cards) {
				p.fillRect(row, c.selected);
				p.fillRect(QRectF(x, top, 2.5 * u, rowHeight), c.bar);
			} else if (c.rowRadius > 0) {
				const auto inner = row.marginsRemoved(
					{ 3. * u, 2. * u, 3. * u, 2. * u });
				const auto radius = c.rowRadius * u * 0.5;
				p.setPen(Qt::NoPen);
				p.setBrush(c.selected);
				p.drawRoundedRect(inner, radius, radius);
				PaintStroke(p, inner, radius, c.selectedStroke, line);
			} else {
				p.fillRect(row, c.selected);
			}
		}
		const auto userpic = QRectF(
			x + 7. * u,
			top + (rowHeight - avatar) / 2.,
			avatar,
			avatar);
		p.setPen(Qt::NoPen);
		p.setBrush(c.avatars[i]);
		p.drawRoundedRect(userpic, avatarRadius, avatarRadius);

		const auto textLeft = userpic.x() + avatar + 6. * u;
		const auto textWidth = x + listWidth - textLeft - 7. * u;
		const auto middle = top + rowHeight / 2.;
		const auto nameWidth = textWidth * ((i == 0)
			? 0.55
			: (i == 1)
			? 0.72
			: 0.46);
		const auto messageWidth = textWidth * ((i == 0) ? 0.62 : 0.9);
		PaintBar(
			p,
			textLeft,
			middle - 6.5 * u,
			nameWidth,
			4. * u,
			selected ? c.selectedText : c.text);
		PaintBar(
			p,
			textLeft,
			middle + 2.5 * u,
			messageWidth,
			3. * u,
			anim::with_alpha(
				selected ? c.selectedSubText : c.subText,
				0.7));
		if (i == 0) {
			const auto size = 9. * u;
			const auto badge = QRectF(
				x + listWidth - 7. * u - size,
				middle - 0.5 * u,
				size,
				size);
			p.setPen(Qt::NoPen);
			if (c.gradients) {
				auto gradient = QLinearGradient(
					badge.topLeft(),
					badge.bottomRight());
				gradient.setColorAt(0., c.gradient[0]);
				gradient.setColorAt(1., c.gradient[1]);
				p.setBrush(gradient);
			} else {
				p.setBrush(c.badge);
			}
			p.drawEllipse(badge);
		}
		if (!c.cards && i != 2) {
			p.fillRect(
				QRectF(
					textLeft,
					top + rowHeight - line,
					x + listWidth - textLeft,
					line),
				c.divider);
		}
	}

	// An incoming and an outgoing bubble.
	const auto pad = 8. * u;
	const auto bubble = c.bubbleRadius * u * 0.5;
	const auto in = QRectF(
		chat.x() + pad,
		y + 12. * u,
		chat.width() * 0.62,
		24. * u);
	p.setPen(Qt::NoPen);
	p.setBrush(c.bubbleIn);
	p.drawRoundedRect(in, bubble, bubble);
	PaintStroke(p, in, bubble, c.bubbleInStroke, line);
	const auto inText = anim::with_alpha(c.bubbleInText, 0.85);
	PaintBar(
		p,
		in.x() + 6. * u,
		in.y() + 6.5 * u,
		in.width() * 0.7,
		3.4 * u,
		inText);
	PaintBar(
		p,
		in.x() + 6. * u,
		in.y() + 14. * u,
		in.width() * 0.45,
		3.4 * u,
		inText);

	const auto outWidth = chat.width() * 0.54;
	const auto out = QRectF(
		chat.x() + chat.width() - pad - outWidth,
		in.y() + in.height() + 7. * u,
		outWidth,
		17. * u);
	p.setPen(Qt::NoPen);
	if (c.bubbleOut != c.bubbleOutTo) {
		auto gradient = QLinearGradient(out.topLeft(), out.bottomRight());
		gradient.setColorAt(0., c.bubbleOut);
		gradient.setColorAt(1., c.bubbleOutTo);
		p.setBrush(gradient);
	} else {
		p.setBrush(c.bubbleOut);
	}
	p.drawRoundedRect(out, bubble, bubble);
	PaintBar(
		p,
		out.x() + 6. * u,
		out.y() + 6.8 * u,
		out.width() * 0.62,
		3.4 * u,
		anim::with_alpha(c.bubbleOutText, 0.9));

	// An activity chip: a pill, or a row with a label in look 3.
	const auto chipHeight = 13. * u;
	const auto chip = QRectF(
		chat.x() + pad,
		y + h - pad - chipHeight,
		chat.width() * 0.6,
		chipHeight);
	if (c.cards) {
		p.setPen(Qt::NoPen);
		p.setBrush(c.chip);
		p.drawRoundedRect(chip, chipHeight / 2., chipHeight / 2.);
		PaintStroke(p, chip, chipHeight / 2., c.chipStroke, line);
		const auto dot = QRectF(
			chip.x() + 4. * u,
			chip.y() + (chipHeight - 5. * u) / 2.,
			5. * u,
			5. * u);
		p.setPen(Qt::NoPen);
		if (c.gradients) {
			auto gradient = QLinearGradient(
				dot.topLeft(),
				dot.bottomRight());
			gradient.setColorAt(0., c.gradient[0]);
			gradient.setColorAt(1., c.gradient[2]);
			p.setBrush(gradient);
		} else {
			p.setBrush(c.accent);
		}
		p.drawEllipse(dot);
		PaintBar(
			p,
			chip.x() + 12. * u,
			chip.y() + (chipHeight - 3. * u) / 2.,
			chip.width() - 18. * u,
			3. * u,
			c.chipText);
	} else {
		const auto width = chat.width() - 2 * pad;
		p.fillRect(
			QRectF(chip.x(), chip.y() - 2. * u, width, line),
			c.divider);
		p.fillRect(
			QRectF(chip.x(), chip.y() + chipHeight + 2. * u, width, line),
			c.divider);
		PaintBar(
			p,
			chip.x(),
			chip.y() + 1.5 * u,
			width * 0.3,
			2.4 * u,
			anim::with_alpha(c.subText, 0.8));
		PaintBar(
			p,
			chip.x(),
			chip.y() + 7.5 * u,
			width * 0.62,
			3.4 * u,
			c.chipText);
	}
	p.restore();
}

void RegisterScenes(
		QString name,
		QSize size,
		Fn<QWidget*(not_null<Ui::RpWidget*> parent)> create,
		Fn<void(not_null<QWidget*> widget)> prepare) {
	for (auto look = 1; look != kCount; ++look) {
		SelfTest::RegisterScene(SceneName(name, look), size, [=](
				not_null<Ui::RpWidget*> parent) {
			ShowInScene(look, parent);
			return create(parent);
		}, prepare);
	}
}

void RegisterBoxScenes(
		QString name,
		QSize size,
		Fn<object_ptr<Ui::BoxContent>(std::shared_ptr<Ui::Show> show)> create,
		Fn<void(not_null<QWidget*> widget)> prepare) {
	for (auto look = 1; look != kCount; ++look) {
		SelfTest::RegisterBoxScene(SceneName(name, look), size, [=](
				std::shared_ptr<Ui::Show> show) {
			ShowInScene(look, show->toastParent());
			return create(show);
		}, prepare);
	}
}

void ShowInScene(int look, not_null<QWidget*> owner) {
	ForceForTests(look);
	// The children of the owner are gone by the time this is called, the
	// scenes that come next are painted the usual way.
	QObject::connect(owner.get(), &QObject::destroyed, [] {
		ForceForTests(-1);
	});
}

} // namespace Oblivion::Look
