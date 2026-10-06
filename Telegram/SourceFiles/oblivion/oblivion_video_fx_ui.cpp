/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_video_fx_ui.h"

#include "base/random.h"
#include "lang/lang_keys.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/color_editor.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/tooltip.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_media_player.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QPointer>
#include <QtGui/QConicalGradient>
#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>
#include <QtGui/QScreen>

namespace Oblivion::VideoFx {
namespace {

constexpr auto kHeaderHeight = 40;
constexpr auto kHeaderRadius = 6;
constexpr auto kHeaderSkip = 6; // Between the cards.
constexpr auto kHeaderPadding = 6;
constexpr auto kIconButton = 28;
constexpr auto kSwitchWidth = 30;
constexpr auto kSwitchHeight = 18;
constexpr auto kSwitchSkip = 10;
constexpr auto kRowHeight = 34;
constexpr auto kRowIndent = 12;
constexpr auto kRowOutdent = 14; // The values end where the cross does.
constexpr auto kNameSkip = 10; // Between a name and its control.
constexpr auto kValueWidth = 66;
constexpr auto kPillHeight = 26;
constexpr auto kPillPadding = 10;
constexpr auto kSwatch = 20;
constexpr auto kSwatchSkip = 8;
constexpr auto kSeedWidth = 48;
constexpr auto kSnapSections = 2000; // More steps than that are not felt.
constexpr auto kTooltipDelay = 600;
constexpr auto kDisabledOpacity = 0.45;
constexpr auto kMenuScreenSkip = 32; // Left of a screen around a menu.
constexpr auto kMenuMinHeight = 200;
constexpr auto kMenuDefaultHeight = 400; // The screen is not known.

// What is offered at once, any other colour is chosen in a box.
constexpr auto kColors = std::array<uint32, 9>{ {
	0xFFFFFFU,
	0x000000U,
	0xFF3B30U,
	0xFF9500U,
	0xFFD60AU,
	0x34C759U,
	0x35E0FFU,
	0x0A84FFU,
	0xBF5AF2U,
} };

[[nodiscard]] int Px(int value) {
	return style::ConvertScale(value);
}

// The width of the lines of the painted icons.
[[nodiscard]] float64 Stroke() {
	return style::ConvertScale(160) / 100.;
}

[[nodiscard]] QPen RoundPen(QColor color) {
	auto result = QPen(color);
	result.setWidthF(Stroke());
	result.setCapStyle(Qt::RoundCap);
	result.setJoinStyle(Qt::RoundJoin);
	return result;
}

[[nodiscard]] QColor Faded(QColor color, float64 opacity) {
	color.setAlphaF(color.alphaF() * opacity);
	return color;
}

enum class Direction : uchar {
	Down,
	Right,
};

// An arrow head inside of a square with the given half side.
void PaintChevron(
		QPainter &p,
		QPointF center,
		float64 half,
		Direction direction,
		QColor color) {
	auto path = QPainterPath();
	switch (direction) {
	case Direction::Down:
		path.moveTo(center + QPointF(-half, -half / 2.));
		path.lineTo(center + QPointF(0., half / 2.));
		path.lineTo(center + QPointF(half, -half / 2.));
		break;
	case Direction::Right:
		path.moveTo(center + QPointF(-half / 2., -half));
		path.lineTo(center + QPointF(half / 2., 0.));
		path.lineTo(center + QPointF(-half / 2., half));
		break;
	}
	p.setPen(RoundPen(color));
	p.setBrush(Qt::NoBrush);
	p.drawPath(path);
}

// An arrow with a stem, as on the buttons of the editor that move a clip:
// "move the effect up or down". A plain arrow head next to the one that
// shows the parameters would look like one more of those.
void PaintArrow(
		QPainter &p,
		QPointF center,
		float64 half,
		bool up,
		QColor color) {
	const auto sign = up ? -1. : 1.;
	const auto reach = half * 1.25;
	const auto tip = center + QPointF(0., sign * reach);
	auto path = QPainterPath();
	path.moveTo(center + QPointF(0., -sign * reach));
	path.lineTo(tip);
	path.moveTo(tip + QPointF(-half, -sign * half));
	path.lineTo(tip);
	path.lineTo(tip + QPointF(half, -sign * half));
	p.setPen(RoundPen(color));
	p.setBrush(Qt::NoBrush);
	p.drawPath(path);
}

void PaintCross(QPainter &p, QPointF center, float64 half, QColor color) {
	p.setPen(RoundPen(color));
	p.drawLine(
		center + QPointF(-half, -half),
		center + QPointF(half, half));
	p.drawLine(
		center + QPointF(half, -half),
		center + QPointF(-half, half));
}

void PaintSwitch(QPainter &p, QRectF rect, bool on) {
	const auto radius = rect.height() / 2.;
	p.setPen(Qt::NoPen);

	// The colours of the toggles in the settings.
	p.setBrush(on ? st::activeButtonBg : st::checkboxFg);
	p.drawRoundedRect(rect, radius, radius);
	const auto inset = float64(Px(2));
	const auto knob = rect.height() - 2 * inset;
	p.setBrush(on ? st::activeButtonFg->c : st::windowBg->c);
	p.drawEllipse(QRectF(
		on ? (rect.right() - inset - knob) : (rect.left() + inset),
		rect.top() + inset,
		knob,
		knob));
}

// A rounded button with a text, as wide as the rect.
void PaintPill(
		QPainter &p,
		QRect rect,
		const QString &text,
		bool over,
		int rightSkip = 0) {
	const auto radius = rect.height() / 2.;
	p.setPen(Qt::NoPen);
	p.setBrush(over ? OverBg() : st::windowBgOver->c);
	p.drawRoundedRect(rect, radius, radius);
	const auto padding = Px(kPillPadding);
	const auto available = rect.width() - 2 * padding - rightSkip;
	p.setPen(st::windowFg);
	p.setFont(st::normalFont);
	p.drawText(
		QRect(rect.x() + padding, rect.y(), available, rect.height()),
		Qt::AlignLeft | Qt::AlignVCenter,
		st::normalFont->elided(text, std::max(available, 0)));
}

[[nodiscard]] int ControlLeft(int width) {
	return std::clamp(width * 38 / 100, Px(96), Px(230)) + Px(kNameSkip);
}

// How tall the list of a menu may be on a screen with that much place.
[[nodiscard]] int MenuMaxHeight(int screenHeight) {
	return (screenHeight > 0)
		? std::max(screenHeight - Px(kMenuScreenSkip), Px(kMenuMinHeight))
		: Px(kMenuDefaultHeight);
}

// The header of a card: the name of the effect with its switch and the
// buttons that move and remove it. A click anywhere else shows or hides
// the parameters.
class Header final
	: public Ui::RpWidget
	, public Ui::AbstractTooltipShower {
public:
	struct Handlers {
		Fn<void()> toggle;
		Fn<void()> expand;
		Fn<void()> up;
		Fn<void()> down;
		Fn<void()> remove;
	};

	Header(
		QWidget *parent,
		QString name,
		bool enabled,
		bool expanded,
		bool canUp,
		bool canDown,
		Handlers handlers);

	QString tooltipText() const override;
	QPoint tooltipPos() const override;
	bool tooltipWindowActive() const override;

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	enum class Zone : uchar {
		None,
		Row,
		Switch,
		Up,
		Down,
		Remove,
	};

	[[nodiscard]] QRect zoneRect(Zone zone) const;
	[[nodiscard]] Zone zoneAt(QPoint point) const;
	[[nodiscard]] bool available(Zone zone) const;
	void setOver(Zone zone);

	const QString _name;
	const bool _enabled = false;
	const bool _expanded = false;
	const bool _canUp = false;
	const bool _canDown = false;
	const Handlers _handlers;
	Zone _over = Zone::None;
	Zone _pressed = Zone::None;

};

Header::Header(
	QWidget *parent,
	QString name,
	bool enabled,
	bool expanded,
	bool canUp,
	bool canDown,
	Handlers handlers)
: RpWidget(parent)
, _name(std::move(name))
, _enabled(enabled)
, _expanded(expanded)
, _canUp(canUp)
, _canDown(canDown)
, _handlers(std::move(handlers)) {
	setMouseTracking(true);
	resize(width(), Px(kHeaderHeight));
}

QString Header::tooltipText() const {
	switch (_over) {
	case Zone::Up: return tr::lng_oblivion_vfx_move_up(tr::now);
	case Zone::Down: return tr::lng_oblivion_vfx_move_down(tr::now);
	case Zone::Remove: return tr::lng_oblivion_vfx_remove(tr::now);
	case Zone::None:
	case Zone::Row:
	case Zone::Switch: break;
	}
	return QString();
}

QPoint Header::tooltipPos() const {
	return QCursor::pos();
}

bool Header::tooltipWindowActive() const {
	return Ui::AppInFocus() && Ui::InFocusChain(window());
}

int Header::resizeGetHeight(int newWidth) {
	return Px(kHeaderHeight);
}

QRect Header::zoneRect(Zone zone) const {
	const auto side = Px(kIconButton);
	const auto top = (height() - side) / 2;
	const auto remove = QRect(
		width() - Px(kHeaderPadding) - side,
		top,
		side,
		side);
	switch (zone) {
	case Zone::Remove: return remove;
	case Zone::Down: return remove.translated(-side, 0);
	case Zone::Up: return remove.translated(-2 * side, 0);
	case Zone::Switch:
		return QRect(
			remove.x() - 2 * side - Px(kSwitchSkip) - Px(kSwitchWidth),
			(height() - Px(kSwitchHeight)) / 2,
			Px(kSwitchWidth),
			Px(kSwitchHeight));
	case Zone::Row: return rect();
	case Zone::None: break;
	}
	return QRect();
}

Header::Zone Header::zoneAt(QPoint point) const {
	if (!rect().contains(point)) {
		return Zone::None;
	}
	for (const auto zone : { Zone::Remove, Zone::Down, Zone::Up }) {
		if (zoneRect(zone).contains(point)) {
			return zone;
		}
	}
	const auto slop = Px(6);
	const auto around = zoneRect(Zone::Switch).marginsAdded(
		{ slop, slop, slop, slop });
	return around.contains(point) ? Zone::Switch : Zone::Row;
}

bool Header::available(Zone zone) const {
	return (zone == Zone::Up)
		? _canUp
		: (zone == Zone::Down)
		? _canDown
		: (zone != Zone::None);
}

void Header::setOver(Zone zone) {
	if (_over == zone) {
		return;
	}
	_over = zone;
	setCursor(available(zone) ? style::cur_pointer : style::cur_default);
	if (!tooltipText().isEmpty()) {
		Ui::Tooltip::Show(kTooltipDelay, this);
	} else {
		Ui::Tooltip::Hide();
	}
	update();
}

void Header::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);

	const auto radius = Px(kHeaderRadius);
	p.setPen(Qt::NoPen);
	p.setBrush((_over == Zone::Row) ? OverBg() : st::windowBgOver->c);
	p.drawRoundedRect(rect(), radius, radius);

	const auto left = Px(kHeaderPadding) + Px(8);
	PaintChevron(
		p,
		QPointF(left + Px(2), height() / 2.),
		Px(4),
		_expanded ? Direction::Down : Direction::Right,
		st::windowSubTextFg->c);

	const auto toggle = zoneRect(Zone::Switch);
	const auto nameLeft = left + Px(14);
	const auto nameWidth = toggle.x() - Px(kSwitchSkip) - nameLeft;
	if (nameWidth > 0) {
		p.setOpacity(_enabled ? 1. : kDisabledOpacity);
		p.setFont(st::semiboldFont);
		p.setPen(st::windowFg);
		p.drawText(
			QRect(nameLeft, 0, nameWidth, height()),
			Qt::AlignLeft | Qt::AlignVCenter,
			st::semiboldFont->elided(_name, nameWidth));
		p.setOpacity(1.);
	}
	PaintSwitch(p, QRectF(toggle), _enabled);

	const auto button = [&](Zone zone) {
		const auto rect = QRectF(zoneRect(zone));
		const auto can = available(zone);
		const auto over = can && (_over == zone);
		if (over) {
			p.setPen(Qt::NoPen);
			p.setBrush(OverBg());
			p.drawEllipse(rect);
		}
		return !can
			? Faded(st::windowSubTextFg->c, 0.35)
			: !over
			? st::windowSubTextFg->c
			: (zone == Zone::Remove)
			? st::attentionButtonFg->c
			: st::windowFg->c;
	};
	const auto half = float64(Px(4));
	PaintArrow(
		p,
		QRectF(zoneRect(Zone::Up)).center(),
		half,
		true,
		button(Zone::Up));
	PaintArrow(
		p,
		QRectF(zoneRect(Zone::Down)).center(),
		half,
		false,
		button(Zone::Down));
	PaintCross(
		p,
		QRectF(zoneRect(Zone::Remove)).center(),
		half,
		button(Zone::Remove));
}

void Header::mouseMoveEvent(QMouseEvent *e) {
	setOver(zoneAt(e->pos()));
}

void Header::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = zoneAt(e->pos());
	}
}

void Header::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, Zone::None);
	if (e->button() != Qt::LeftButton
		|| pressed != zoneAt(e->pos())
		|| !available(pressed)) {
		return;
	}
	Ui::Tooltip::Hide();

	// The handlers may ask for the list to be rebuilt, that is done later.
	const auto call = [&](const Fn<void()> &handler) {
		if (const auto onstack = handler) {
			onstack();
		}
	};
	switch (pressed) {
	case Zone::Row: call(_handlers.expand); break;
	case Zone::Switch: call(_handlers.toggle); break;
	case Zone::Up: call(_handlers.up); break;
	case Zone::Down: call(_handlers.down); break;
	case Zone::Remove: call(_handlers.remove); break;
	case Zone::None: break;
	}
}

void Header::leaveEventHook(QEvent *e) {
	setOver(Zone::None);
}

// A parameter: its name on the left, the control after it.
class Row : public Ui::RpWidget {
public:
	Row(QWidget *parent, QString name);

protected:
	int resizeGetHeight(int newWidth) override;

	[[nodiscard]] int controlLeft() const {
		return ControlLeft(width());
	}
	void paintName(QPainter &p) const;

private:
	const QString _name;

};

Row::Row(QWidget *parent, QString name)
: RpWidget(parent)
, _name(std::move(name)) {
	resize(width(), Px(kRowHeight));
}

int Row::resizeGetHeight(int newWidth) {
	return Px(kRowHeight);
}

void Row::paintName(QPainter &p) const {
	const auto available = controlLeft() - Px(kNameSkip);
	p.setFont(st::normalFont);
	p.setPen(st::windowFg);
	p.drawText(
		QRect(0, 0, available, height()),
		Qt::AlignLeft | Qt::AlignVCenter,
		st::normalFont->elided(_name, std::max(available, 0)));
}

class SliderRow final : public Row {
public:
	SliderRow(
		QWidget *parent,
		const Param &param,
		float64 value,
		Fn<void()> started,
		Fn<void(float64 value, bool tuning)> changed);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;

private:
	const Param _param;
	const not_null<Ui::MediaSliderWheelless*> _slider;
	float64 _value = 0.;
	bool _dragging = false;

};

SliderRow::SliderRow(
	QWidget *parent,
	const Param &param,
	float64 value,
	Fn<void()> started,
	Fn<void(float64 value, bool tuning)> changed)
: Row(parent, param.name(tr::now))
, _param(param)
, _slider(Ui::CreateChild<Ui::MediaSliderWheelless>(this, st::settingsScale))
, _value(SanitizedValue(param, value)) {
	const auto min = _param.min;
	const auto range = _param.max - _param.min;
	const auto step = _param.step;
	const auto sections = (step > 0. && range > 0.)
		? int(std::min(std::round(range / step), 1e6))
		: 0;
	const auto snap = (sections > 0) && (sections <= kSnapSections);
	const auto toValue = [=](float64 position) {
		position = std::clamp(position, 0., 1.);
		return SanitizedValue(_param, snap
			? (min + std::round(position * sections) * step)
			: (min + position * range));
	};
	const auto toPosition = [=](float64 now) {
		return (range > 0.) ? std::clamp((now - min) / range, 0., 1.) : 0.;
	};
	_slider->setAlwaysDisplayMarker(true);
	_slider->setValue(toPosition(_value));
	_slider->setAdjustCallback([=](float64 position) {
		return toPosition(toValue(position));
	});

	// started() is called before the first change of a drag, the last
	// change of it is reported as not a tuning one.
	const auto apply = [=](float64 position, bool finished) {
		const auto now = toValue(position);
		if (now != _value) {
			if (!std::exchange(_dragging, true)) {
				started();
			}
			_value = now;
			update();
			changed(now, !finished);
		} else if (finished && _dragging) {
			changed(now, false);
		}
		if (finished) {
			_dragging = false;
		}
	};
	_slider->setChangeProgressCallback([=](float64 position) {
		apply(position, false);
	});
	_slider->setChangeFinishedCallback([=](float64 position) {
		apply(position, true);
	});
}

int SliderRow::resizeGetHeight(int newWidth) {
	const auto height = Px(kRowHeight);
	const auto left = ControlLeft(newWidth);
	const auto sliderHeight = st::settingsScale.seekSize.height();
	_slider->setGeometry(
		left,
		(height - sliderHeight) / 2,
		std::max(newWidth - left - Px(kValueWidth), Px(24)),
		sliderHeight);
	return height;
}

void SliderRow::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	paintName(p);
	p.setPen(st::windowSubTextFg);
	p.drawText(
		QRect(width() - Px(kValueWidth), 0, Px(kValueWidth), height()),
		Qt::AlignRight | Qt::AlignVCenter,
		FormatValue(_param, _value));
}

class ToggleRow final : public Row {
public:
	ToggleRow(
		QWidget *parent,
		QString name,
		bool value,
		Fn<void(bool)> changed);

protected:
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;

private:
	const Fn<void(bool)> _changed;
	bool _value = false;
	bool _pressed = false;

};

ToggleRow::ToggleRow(
	QWidget *parent,
	QString name,
	bool value,
	Fn<void(bool)> changed)
: Row(parent, std::move(name))
, _changed(std::move(changed))
, _value(value) {
	setCursor(style::cur_pointer);
}

void ToggleRow::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	paintName(p);
	PaintSwitch(
		p,
		QRectF(
			controlLeft(),
			(height() - Px(kSwitchHeight)) / 2,
			Px(kSwitchWidth),
			Px(kSwitchHeight)),
		_value);
}

void ToggleRow::mousePressEvent(QMouseEvent *e) {
	_pressed = (e->button() == Qt::LeftButton);
}

void ToggleRow::mouseReleaseEvent(QMouseEvent *e) {
	if (std::exchange(_pressed, false)
		&& e->button() == Qt::LeftButton
		&& rect().contains(e->pos())) {
		_value = !_value;
		update();
		_changed(_value);
	}
}

class ChoiceRow final : public Row {
public:
	ChoiceRow(
		QWidget *parent,
		QString name,
		std::vector<QString> options,
		int value,
		Fn<void(int)> changed);

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	[[nodiscard]] QString current() const;
	[[nodiscard]] QRect pill() const;
	void setOver(bool over);
	void showMenu();

	const std::vector<QString> _options;
	const Fn<void(int)> _changed;
	int _value = 0;
	bool _over = false;
	bool _pressed = false;
	base::unique_qptr<Ui::PopupMenu> _menu;

};

ChoiceRow::ChoiceRow(
	QWidget *parent,
	QString name,
	std::vector<QString> options,
	int value,
	Fn<void(int)> changed)
: Row(parent, std::move(name))
, _options(std::move(options))
, _changed(std::move(changed))
, _value(value) {
	setMouseTracking(true);
}

QString ChoiceRow::current() const {
	return (_value >= 0 && _value < int(_options.size()))
		? _options[_value]
		: QString();
}

QRect ChoiceRow::pill() const {
	const auto left = controlLeft();
	const auto height = Px(kPillHeight);
	const auto arrow = Px(16);
	const auto wanted = st::normalFont->width(current())
		+ 2 * Px(kPillPadding)
		+ arrow;
	return QRect(
		left,
		(this->height() - height) / 2,
		std::clamp(wanted, Px(48), std::max(width() - left, Px(48))),
		height);
}

void ChoiceRow::setOver(bool over) {
	if (_over != over) {
		_over = over;
		setCursor(over ? style::cur_pointer : style::cur_default);
		update();
	}
}

void ChoiceRow::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	paintName(p);
	const auto rect = pill();
	const auto arrow = Px(16);
	PaintPill(p, rect, current(), _over, arrow);
	PaintChevron(
		p,
		QPointF(
			rect.x() + rect.width() - Px(kPillPadding) - Px(4),
			rect.y() + rect.height() / 2.),
		Px(4),
		Direction::Down,
		st::windowSubTextFg->c);
}

void ChoiceRow::mouseMoveEvent(QMouseEvent *e) {
	setOver(pill().contains(e->pos()));
}

void ChoiceRow::mousePressEvent(QMouseEvent *e) {
	_pressed = (e->button() == Qt::LeftButton) && pill().contains(e->pos());
}

void ChoiceRow::mouseReleaseEvent(QMouseEvent *e) {
	if (std::exchange(_pressed, false)
		&& e->button() == Qt::LeftButton
		&& pill().contains(e->pos())) {
		showMenu();
	}
}

void ChoiceRow::leaveEventHook(QEvent *e) {
	setOver(false);
}

void ChoiceRow::showMenu() {
	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuWithIcons);
	const auto count = int(_options.size());
	for (auto i = 0; i != count; ++i) {
		_menu->addAction(_options[i], [=] {
			if (_value != i) {
				_value = i;
				update();
				_changed(i);
			}
		}, (i == _value) ? &st::mediaPlayerMenuCheck : nullptr);
	}
	const auto rect = pill();
	_menu->popup(mapToGlobal(QPoint(rect.x(), rect.y() + rect.height())));
}

class ColorRow final : public Row {
public:
	ColorRow(
		QWidget *parent,
		QString name,
		uint32 value,
		Fn<void(uint32)> changed,
		Fn<void()> custom);

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	// How many of the ready colours fit, the custom one goes after them.
	[[nodiscard]] int shown() const;
	[[nodiscard]] QRect swatch(int index) const;
	[[nodiscard]] int swatchAt(QPoint point) const;
	void setOver(int index);

	const Fn<void(uint32)> _changed;
	const Fn<void()> _custom;
	uint32 _value = 0;
	int _over = -1;
	int _pressed = -1;

};

ColorRow::ColorRow(
	QWidget *parent,
	QString name,
	uint32 value,
	Fn<void(uint32)> changed,
	Fn<void()> custom)
: Row(parent, std::move(name))
, _changed(std::move(changed))
, _custom(std::move(custom))
, _value(value & 0xFFFFFFU) {
	setMouseTracking(true);
}

int ColorRow::shown() const {
	const auto step = Px(kSwatch) + Px(kSwatchSkip);
	const auto fit = (width() - controlLeft() + Px(kSwatchSkip)) / step;
	return std::clamp(fit - 1, 0, int(kColors.size()));
}

QRect ColorRow::swatch(int index) const {
	const auto side = Px(kSwatch);
	return QRect(
		controlLeft() + Px(2) + index * (side + Px(kSwatchSkip)),
		(height() - side) / 2,
		side,
		side);
}

int ColorRow::swatchAt(QPoint point) const {
	const auto count = shown() + 1;
	const auto slop = Px(kSwatchSkip) / 2;
	for (auto i = 0; i != count; ++i) {
		const auto rect = swatch(i).marginsAdded({ slop, slop, slop, slop });
		if (rect.contains(point)) {
			return i;
		}
	}
	return -1;
}

void ColorRow::setOver(int index) {
	if (_over != index) {
		_over = index;
		setCursor((index >= 0) ? style::cur_pointer : style::cur_default);
		update();
	}
}

void ColorRow::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	paintName(p);

	const auto count = shown();
	const auto outline = QColor(128, 128, 128, 110);
	const auto ring = [&](QRectF rect) {
		const auto skip = float64(Px(3));
		auto pen = QPen(st::windowActiveTextFg->c);
		pen.setWidthF(Stroke());
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(rect.marginsAdded({ skip, skip, skip, skip }));
	};
	auto ready = false;
	for (auto i = 0; i != count; ++i) {
		const auto rect = QRectF(swatch(i));
		p.setPen(QPen(outline, 1.));
		p.setBrush(QColor::fromRgb(QRgb(kColors[i] | 0xFF000000U)));
		p.drawEllipse(rect);
		if (kColors[i] == _value) {
			ready = true;
			ring(rect);
		} else if (i == _over) {
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(128, 128, 128, 60));
			p.drawEllipse(rect);
		}
	}

	// Any other colour: a colour wheel, with the colour itself in the middle
	// when it is not one of the ready ones.
	const auto rect = QRectF(swatch(count));
	auto wheel = QConicalGradient(rect.center(), 0.);
	for (auto i = 0; i <= 6; ++i) {
		wheel.setColorAt(i / 6., QColor::fromHsv((i * 60) % 360, 230, 255));
	}
	p.setPen(QPen(outline, 1.));
	p.setBrush(wheel);
	p.drawEllipse(rect);
	const auto inner = float64(Px(4));
	p.setPen(Qt::NoPen);
	p.setBrush(ready
		? st::windowBg->c
		: QColor::fromRgb(QRgb(_value | 0xFF000000U)));
	p.drawEllipse(rect.marginsRemoved({ inner, inner, inner, inner }));
	if (!ready) {
		ring(rect);
	} else if (count == _over) {
		p.setBrush(QColor(128, 128, 128, 60));
		p.drawEllipse(rect);
	}
}

void ColorRow::mouseMoveEvent(QMouseEvent *e) {
	setOver(swatchAt(e->pos()));
}

void ColorRow::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = swatchAt(e->pos());
	}
}

void ColorRow::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, -1);
	if (e->button() != Qt::LeftButton
		|| pressed < 0
		|| pressed != swatchAt(e->pos())) {
		return;
	} else if (pressed >= shown()) {
		_custom();
	} else if (kColors[pressed] != _value) {
		_value = kColors[pressed];
		update();
		_changed(_value);
	}
}

void ColorRow::leaveEventHook(QEvent *e) {
	setOver(-1);
}

// A seed: the number and a button that takes another one at random.
class SeedRow final : public Row {
public:
	SeedRow(
		QWidget *parent,
		QString name,
		int value,
		int limit,
		Fn<void(int)> changed);

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	[[nodiscard]] QRect pill() const;
	void setOver(bool over);

	const QString _text;
	const int _limit = 0;
	const Fn<void(int)> _changed;
	int _value = 0;
	bool _over = false;
	bool _pressed = false;

};

SeedRow::SeedRow(
	QWidget *parent,
	QString name,
	int value,
	int limit,
	Fn<void(int)> changed)
: Row(parent, std::move(name))
, _text(tr::lng_oblivion_vfx_randomize(tr::now))
, _limit(std::max(limit, 1))
, _changed(std::move(changed))
, _value(value) {
	setMouseTracking(true);
}

QRect SeedRow::pill() const {
	const auto left = controlLeft() + Px(kSeedWidth);
	const auto height = Px(kPillHeight);
	const auto icon = Px(20);
	const auto wanted = st::normalFont->width(_text)
		+ 2 * Px(kPillPadding)
		+ icon;
	return QRect(
		left,
		(this->height() - height) / 2,
		std::clamp(wanted, Px(48), std::max(width() - left, Px(48))),
		height);
}

void SeedRow::setOver(bool over) {
	if (_over != over) {
		_over = over;
		setCursor(over ? style::cur_pointer : style::cur_default);
		update();
	}
}

void SeedRow::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	paintName(p);
	p.setPen(st::windowSubTextFg);
	p.setFont(st::normalFont);
	p.drawText(
		QRect(controlLeft(), 0, Px(kSeedWidth), height()),
		Qt::AlignLeft | Qt::AlignVCenter,
		QString::number(_value));

	// A die before the text.
	const auto rect = pill();
	const auto icon = Px(20);
	const auto radius = rect.height() / 2.;
	p.setPen(Qt::NoPen);
	p.setBrush(_over ? OverBg() : st::windowBgOver->c);
	p.drawRoundedRect(rect, radius, radius);
	const auto side = float64(Px(12));
	const auto die = QRectF(
		rect.x() + Px(kPillPadding),
		rect.y() + (rect.height() - side) / 2.,
		side,
		side);
	p.setPen(RoundPen(st::windowFg->c));
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(die, side / 4., side / 4.);
	p.setPen(Qt::NoPen);
	p.setBrush(st::windowFg);
	const auto dot = side / 12.;
	for (const auto part : { 0.28, 0.5, 0.72 }) {
		p.drawEllipse(
			QPointF(die.x() + side * part, die.y() + side * (1. - part)),
			dot,
			dot);
	}
	const auto available = rect.width() - 2 * Px(kPillPadding) - icon;
	p.setPen(st::windowFg);
	p.drawText(
		QRect(
			rect.x() + Px(kPillPadding) + icon,
			rect.y(),
			std::max(available, 0),
			rect.height()),
		Qt::AlignLeft | Qt::AlignVCenter,
		st::normalFont->elided(_text, std::max(available, 0)));
}

void SeedRow::mouseMoveEvent(QMouseEvent *e) {
	setOver(pill().contains(e->pos()));
}

void SeedRow::mousePressEvent(QMouseEvent *e) {
	_pressed = (e->button() == Qt::LeftButton) && pill().contains(e->pos());
}

void SeedRow::mouseReleaseEvent(QMouseEvent *e) {
	if (!std::exchange(_pressed, false)
		|| e->button() != Qt::LeftButton
		|| !pill().contains(e->pos())) {
		return;
	}
	// Always another one.
	auto next = base::RandomIndex(_limit);
	if (next >= _value) {
		++next;
	}
	_value = next;
	update();
	_changed(_value);
}

void SeedRow::leaveEventHook(QEvent *e) {
	setOver(false);
}

} // namespace

Panel::Panel(QWidget *parent, PanelArgs &&args)
: RpWidget(parent)
, _show(std::move(args.show))
, _started(std::move(args.started))
, _changed(std::move(args.changed))
, _revealed(std::move(args.revealed))
, _list(Ui::CreateChild<Ui::VerticalLayout>(this))
, _expanded(args.expanded) {
	_stack = std::move(args.stack);
	for (auto &entry : _stack) {
		entry = Sanitized(std::move(entry));
	}
	if (_expanded >= int(_stack.size())) {
		_expanded = -1;
	}
	_list->heightValue(
	) | rpl::on_next([=](int height) {
		if (!_rebuilding && this->height() != height) {
			resize(width(), height);
		}
	}, _list->lifetime());
	rebuild();
}

Panel::~Panel() = default;

int Panel::resizeGetHeight(int newWidth) {
	_list->resizeToWidth(newWidth);
	_list->moveToLeft(0, 0, newWidth);
	return _list->height();
}

void Panel::setStack(const Stack &stack) {
	auto sanitized = stack;
	for (auto &entry : sanitized) {
		entry = Sanitized(std::move(entry));
	}
	if (_stack == sanitized) {
		return;
	}
	_expanded = FollowEffect(_stack, _expanded, sanitized);
	_stack = std::move(sanitized);
	scheduleRebuild();
}

void Panel::started() {
	if (const auto onstack = _started) {
		onstack();
	}
}

void Panel::changed(bool tuning) {
	if (const auto onstack = _changed) {
		onstack(_stack, tuning);
	}
}

bool Panel::valid(int generation, int index, Type type) const {
	return (generation == _generation)
		&& (index >= 0)
		&& (index < int(_stack.size()))
		&& (_stack[index].type == type);
}

// The rows that are there stop working at once and are replaced a moment
// later: never inside of a click on one of them.
void Panel::scheduleRebuild() {
	++_generation;
	if (std::exchange(_rebuildPending, true)) {
		return;
	}
	crl::on_main(this, [=] {
		if (_rebuildPending) {
			rebuild();
		}
	});
}

void Panel::rebuild() {
	_rebuildPending = false;
	++_generation;
	_rebuilding = true;
	_list->clear();

	const auto count = int(_stack.size());
	if (!count) {
		_list->add(
			object_ptr<Ui::FlatLabel>(
				_list,
				tr::lng_oblivion_vfx_empty(tr::now),
				st::boxDividerLabel),
			QMargins(0, Px(6), 0, Px(6)));
	}
	const auto generation = _generation;
	auto expandedHeader = (Header*)(nullptr);
	auto nextHeader = (Header*)(nullptr);
	for (auto i = 0; i != count; ++i) {
		const auto &entry = _stack[i];
		const auto type = entry.type;
		const auto guarded = [=](Fn<void()> callback) {
			return [=] {
				if (valid(generation, i, type)) {
					callback();
				}
			};
		};
		auto handlers = Header::Handlers();
		handlers.toggle = guarded([=] { toggle(i); });
		handlers.expand = guarded([=] { expand(i); });
		handlers.up = guarded([=] { move(i, -1); });
		handlers.down = guarded([=] { move(i, 1); });
		handlers.remove = guarded([=] { remove(i); });
		const auto header = _list->add(
			object_ptr<Header>(
				_list,
				EffectInfo(type).name(tr::now),
				entry.enabled,
				(i == _expanded),
				(i > 0),
				(i + 1 < count),
				std::move(handlers)),
			QMargins(0, i ? Px(kHeaderSkip) : 0, 0, 0));
		if (i == _expanded) {
			expandedHeader = header;
			buildBody(i);
		} else if (i == _expanded + 1) {
			nextHeader = header;
		}
	}
	if (width() > 0) {
		_list->resizeToWidth(width());
	}
	_rebuilding = false;
	if (height() != _list->height()) {
		resize(width(), _list->height());
	}

	// The card that was added or opened may be out of the visible part.
	if (std::exchange(_revealPending, false) && expandedHeader && _revealed) {
		_revealed(
			expandedHeader->y(),
			nextHeader
				? (nextHeader->y() - Px(kHeaderSkip))
				: _list->height());
	}
}

void Panel::buildBody(int index) {
	const auto &entry = _stack[index];
	const auto &info = EffectInfo(entry.type);
	const auto generation = _generation;
	const auto type = entry.type;
	const auto margins = QMargins(Px(kRowIndent), 0, Px(kRowOutdent), 0);
	const auto begin = [=] {
		if (valid(generation, index, type)) {
			started();
		}
	};
	const auto set = [=](int param, float64 value, bool tuning) {
		if (valid(generation, index, type)) {
			setValue(index, param, value, tuning);
		}
	};
	// Changed at once, by a click.
	const auto choose = [=](int param, float64 value) {
		if (valid(generation, index, type)) {
			started();
			setValue(index, param, value, false);
		}
	};

	// How much of the effect is seen, the same for all of them.
	auto mix = Param();
	mix.id = "mix";
	mix.name = tr::lng_oblivion_vfx_mix;
	mix.kind = ParamKind::Slider;
	mix.unit = Unit::Percent;
	mix.min = 0.;
	mix.max = 100.;
	mix.value = 100.;
	mix.step = 1.;
	_list->add(
		object_ptr<SliderRow>(
			_list,
			mix,
			entry.mix * 100.,
			begin,
			[=](float64 value, bool tuning) {
				if (valid(generation, index, type)) {
					setMix(index, value, tuning);
				}
			}),
		margins + QMargins(0, Px(4), 0, 0));

	const auto count = int(info.params.size());
	for (auto i = 0; i != count; ++i) {
		const auto &param = info.params[i];
		const auto value = (i < int(entry.values.size()))
			? entry.values[i]
			: param.value;
		const auto name = param.name(tr::now);
		switch (param.kind) {
		case ParamKind::Slider:
		case ParamKind::Integer:
			_list->add(
				object_ptr<SliderRow>(
					_list,
					param,
					value,
					begin,
					[=](float64 now, bool tuning) {
						set(i, now, tuning);
					}),
				margins);
			break;
		case ParamKind::Toggle:
			_list->add(
				object_ptr<ToggleRow>(
					_list,
					name,
					(value >= 0.5),
					[=](bool now) {
						choose(i, now ? 1. : 0.);
					}),
				margins);
			break;
		case ParamKind::Choice: {
			auto options = std::vector<QString>();
			options.reserve(param.options.size());
			for (const auto &option : param.options) {
				options.push_back(option(tr::now));
			}
			_list->add(
				object_ptr<ChoiceRow>(
					_list,
					name,
					std::move(options),
					int(std::lround(value)),
					[=](int now) {
						choose(i, now);
					}),
				margins);
		} break;
		case ParamKind::Color:
			_list->add(
				object_ptr<ColorRow>(
					_list,
					name,
					uint32(std::llround(value)) & 0xFFFFFFU,
					[=](uint32 now) {
						choose(i, float64(now));
					},
					[=] {
						if (valid(generation, index, type)) {
							chooseColor(index, i);
						}
					}),
				margins);
			break;
		case ParamKind::Seed:
			_list->add(
				object_ptr<SeedRow>(
					_list,
					name,
					int(std::lround(value)),
					int(std::lround(param.max)),
					[=](int now) {
						choose(i, now);
					}),
				margins);
			break;
		}
	}
	if (type == Type::Strobe) {
		_list->add(
			object_ptr<Ui::FlatLabel>(
				_list,
				tr::lng_oblivion_vfx_strobe_warning(tr::now),
				st::boxDividerLabel),
			margins + QMargins(0, Px(4), 0, 0));
	}
	_list->add(
		object_ptr<Ui::FixedHeightWidget>(_list, Px(4)),
		QMargins());
}

void Panel::add(Type type) {
	if (int(_stack.size()) >= kMaxEffects) {
		if (_show) {
			_show->showToast(tr::lng_oblivion_vfx_limit(
				tr::now,
				lt_max,
				QString::number(kMaxEffects)));
		}
		return;
	}
	started();
	_stack.push_back(MakeEntry(type));
	_expanded = int(_stack.size()) - 1;
	_revealPending = true;
	scheduleRebuild();
	changed(false);
}

void Panel::applyPreset(int index) {
	const auto &presets = Presets();
	if (index < 0 || index >= int(presets.size())) {
		return;
	}
	auto stack = presets[index].stack;
	for (auto &entry : stack) {
		entry = Sanitized(std::move(entry));
	}
	if (stack == _stack) {
		return;
	}
	started();
	_stack = std::move(stack);
	_expanded = -1;
	scheduleRebuild();
	changed(false);
}

void Panel::removeAll() {
	if (_stack.empty()) {
		return;
	}
	started();
	_stack.clear();
	_expanded = -1;
	scheduleRebuild();
	changed(false);
}

void Panel::remove(int index) {
	started();
	_stack.erase(begin(_stack) + index);
	if (_expanded == index) {
		_expanded = -1;
	} else if (_expanded > index) {
		--_expanded;
	}
	scheduleRebuild();
	changed(false);
}

void Panel::move(int index, int delta) {
	const auto target = index + delta;
	if (target < 0 || target >= int(_stack.size())) {
		return;
	}
	started();
	std::swap(_stack[index], _stack[target]);
	if (_expanded == index) {
		_expanded = target;
	} else if (_expanded == target) {
		_expanded = index;
	}
	scheduleRebuild();
	changed(false);
}

void Panel::toggle(int index) {
	started();
	_stack[index].enabled = !_stack[index].enabled;
	scheduleRebuild();
	changed(false);
}

// Not a change of the stack: nothing is reported.
void Panel::expand(int index) {
	_expanded = (_expanded == index) ? -1 : index;
	_revealPending = (_expanded >= 0);
	scheduleRebuild();
}

void Panel::setValue(int index, int param, float64 value, bool tuning) {
	auto &entry = _stack[index];
	const auto &params = EffectInfo(entry.type).params;
	if (param < 0
		|| param >= int(params.size())
		|| param >= int(entry.values.size())) {
		return;
	}
	entry.values[param] = SanitizedValue(params[param], value);
	changed(tuning);
}

void Panel::setMix(int index, float64 percent, bool tuning) {
	_stack[index].mix = std::clamp(percent / 100., 0., 1.);
	changed(tuning);
}

void Panel::chooseColor(int index, int param) {
	if (!_show || param >= int(_stack[index].values.size())) {
		return;
	}
	const auto generation = _generation;
	const auto type = _stack[index].type;
	const auto initial = QColor::fromRgb(QRgb(
		(uint32(std::llround(_stack[index].values[param])) & 0xFFFFFFU)
			| 0xFF000000U));
	const auto weak = QPointer<Panel>(this);
	_show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_oblivion_vfx_color_title());

		// A click outside closes all the boxes: the one with the effects
		// and the editor under it too.
		box->setCloseByOutsideClick(false);
		const auto editor = box->addRow(
			object_ptr<ColorEditor>(box, ColorEditor::Mode::HSL, initial),
			style::margins());
		box->setWidth(editor->width());
		const auto save = [=] {
			const auto color = editor->color();
			box->closeBox();
			const auto strong = weak.data();
			if (strong && strong->valid(generation, index, type)) {
				strong->started();
				strong->setValue(
					index,
					param,
					float64(color.rgb() & 0xFFFFFFU),
					false);

				// The row shows the new colour.
				strong->scheduleRebuild();
			}
		};
		editor->submitRequests() | rpl::on_next(save, editor->lifetime());
		box->setFocusCallback([=] {
			editor->setInnerFocus();
		});
		box->addButton(tr::lng_settings_save(), save);
		box->addButton(tr::lng_cancel(), [=] {
			box->closeBox();
		});
	}));
}

void Panel::showAddMenu(QPoint globalPosition) {
	// All the effects are taller than a low screen (a laptop with a big
	// interface scale): a popup menu is only moved to fit, never made
	// shorter, unless its style has a height limit. Then it scrolls.
	_menu = nullptr;
	if (!_addMenuSt) {
		_addMenuSt = std::make_unique<style::PopupMenu>(st::defaultPopupMenu);
	}
	const auto under = QGuiApplication::screenAt(globalPosition);
	const auto screen = under ? under : window()->screen();
	_addMenuSt->maxHeight = MenuMaxHeight(
		screen ? screen->availableGeometry().height() : 0);
	_menu = base::make_unique_q<Ui::PopupMenu>(this, *_addMenuSt);

	// The groups one after another, with a line between them.
	auto empty = true;
	for (auto group = 0; group != int(Group::kCount); ++group) {
		auto opened = false;
		for (const auto &info : Effects()) {
			if (int(info.group) != group) {
				continue;
			} else if (!std::exchange(opened, true) && !empty) {
				_menu->addSeparator();
			}
			empty = false;
			const auto type = info.type;
			_menu->addAction(info.name(tr::now), [=] {
				add(type);
			});
		}
	}
	_menu->popup(globalPosition);
}

void Panel::showPresetsMenu(QPoint globalPosition) {
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::defaultPopupMenu);
	const auto &presets = Presets();
	const auto count = int(presets.size());
	for (auto i = 0; i != count; ++i) {
		_menu->addAction(presets[i].name(tr::now), [=] {
			applyPreset(i);
		});
	}
	_menu->popup(globalPosition);
}

QString Summary(const Stack &stack) {
	auto result = QStringList();
	for (const auto &entry : stack) {
		if (entry.enabled && entry.mix > 0.) {
			result.push_back(EffectInfo(entry.type).name(tr::now));
		}
	}
	return result.join(u", "_q);
}

int FollowEffect(const Stack &was, int index, const Stack &now) {
	const auto wasCount = int(was.size());
	const auto nowCount = int(now.size());
	if (index < 0 || index >= wasCount) {
		return -1;
	}

	// One step of an undo changes one place of the stack: the effects
	// before it stay where they are, the ones after it move by as many
	// places as were added or removed. Inside of the changed part the
	// effect is looked for as it was (two effects have changed places),
	// otherwise it is the one at the same place that has got other
	// parameters, or it was removed. Two equal effects are not mixed up
	// this way when only one of them was changed.
	const auto limit = std::min(wasCount, nowCount);
	auto head = 0;
	while (head != limit && was[head] == now[head]) {
		++head;
	}
	auto tail = 0;
	while (tail != limit - head
		&& was[wasCount - 1 - tail] == now[nowCount - 1 - tail]) {
		++tail;
	}
	if (index < head) {
		return index;
	} else if (index >= wasCount - tail) {
		return index + nowCount - wasCount;
	}
	const auto &entry = was[index];
	const auto till = nowCount - tail;
	auto moved = -1;
	for (auto i = head; i != till; ++i) {
		if (now[i] == entry
			&& (moved < 0 || std::abs(i - index) < std::abs(moved - index))) {
			moved = i;
		}
	}
	const auto changed = (wasCount == nowCount)
		&& (now[index].type == entry.type);
	return (moved >= 0) ? moved : changed ? index : -1;
}

QColor OverBg() {
	constexpr auto kPart = 0.06f;
	const auto bg = st::windowBgOver->c;
	const auto fg = st::windowFg->c;
	return QColor::fromRgbF(
		bg.redF() + (fg.redF() - bg.redF()) * kPart,
		bg.greenF() + (fg.greenF() - bg.greenF()) * kPart,
		bg.blueF() + (fg.blueF() - bg.blueF()) * kPart);
}

} // namespace Oblivion::VideoFx
