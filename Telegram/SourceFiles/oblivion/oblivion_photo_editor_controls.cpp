/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_photo_editor_controls.h"

#include "lang/lang_keys.h"
#include "ui/effects/animation_value.h"
#include "ui/effects/ripple_animation.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "styles/style_basic.h"
#include "styles/style_calls.h"
#include "styles/style_widgets.h"

#include <QtGui/QCursor>
#include <QtGui/QPainterPath>
#include <QtGui/QWheelEvent>

namespace Oblivion::Photo::EditorUi {
namespace {

constexpr auto kSmallFontSize = 12;
constexpr auto kTitleFontSize = 15;
constexpr auto kTooltipDelay = 800;

constexpr auto kRowHeight = 44;
constexpr auto kRowIconLeft = 12;
constexpr auto kRowIconSize = 24;
constexpr auto kRowTextLeft = 48;
constexpr auto kRowRadius = 8;

constexpr auto kPanelButtonHeight = 40;
constexpr auto kPanelButtonRadius = 10;

constexpr auto kSectionHeight = 38;
constexpr auto kSectionTextBottom = 10;

constexpr auto kSliderHeight = 50;
constexpr auto kSliderTextTop = 5;
constexpr auto kSliderTrackTop = 34;
constexpr auto kSliderTrackHeight = 4;
constexpr auto kSliderKnobRadius = 7;
constexpr auto kSliderKnobOverGrow = 2;
constexpr auto kSliderTickHeight = 10;

constexpr auto kChipHeight = 30;
constexpr auto kChipPadding = 14;
constexpr auto kChipSkip = 8;

constexpr auto kTabHeight = 60;
constexpr auto kTabIconTop = 8;
constexpr auto kTabTextTop = 36;
constexpr auto kTabInset = 4;
constexpr auto kTabTextSkip = 2;
constexpr auto kTabTextPadding = 8;
constexpr auto kTabMinContent = 32;
constexpr auto kTabRadius = 10;
constexpr auto kTabSlideDuration = crl::time(200);

constexpr auto kSwitchHeaderHeight = 44;
constexpr auto kSwitchLeft = 12;
constexpr auto kSwitchWidth = 34;
constexpr auto kSwitchHeight = 20;
constexpr auto kSwitchTextLeft = 58;
constexpr auto kSwitchMenuSize = 32;
constexpr auto kSwitchMenuRight = 6;
constexpr auto kSwitchDotRadius = 2;
constexpr auto kSwitchDotSkip = 6;
constexpr auto kSwitchDuration = crl::time(150);

constexpr auto kSwatchTop = 6;
constexpr auto kSwatchLabelHeight = 22;
constexpr auto kSwatchRowHeight = 34;
constexpr auto kSwatchSize = 24;
constexpr auto kSwatchSkip = 8;
constexpr auto kSwatchRing = 2;
constexpr auto kSwatchPadding = 5;

constexpr auto kThumbSide = 64;
constexpr auto kThumbRadius = 10;
constexpr auto kTileExtra = 28;
constexpr auto kStripPadding = 6; // The first thumbnail at the photo left edge.
constexpr auto kStripTop = 12;
constexpr auto kStripNameSkip = 6;
constexpr auto kStripBottom = 10;
constexpr auto kStripRing = 2;
constexpr auto kStripRingSkip = 3;
constexpr auto kStripFade = 24;
constexpr auto kStripDragThreshold = 6;
constexpr auto kStripScrollDuration = crl::time(220);

constexpr auto kHintMinWidth = 100;
constexpr auto kDimmedOpacity = 0.4;
constexpr auto kHintOpacity = 0.8;

[[nodiscard]] QColor TextColor() {
	return st::groupCallMembersFg->c;
}

[[nodiscard]] QColor SubTextColor() {
	return st::groupCallMemberNotJoinedStatus->c;
}

[[nodiscard]] QColor AccentColor() {
	return st::groupCallActiveFg->c;
}

[[nodiscard]] QColor TrackColor() {
	return st::mediaviewPipPlaybackInactive->c;
}

[[nodiscard]] style::font MakeFont(int size, const style::font &base) {
	return style::font(Px(size), base->flags(), base->family());
}

} // namespace

int Px(int value) {
	return style::ConvertScale(value);
}

const style::font &SmallFont() {
	static const auto result = MakeFont(kSmallFontSize, st::normalFont);
	return result;
}

const style::font &SmallSemiboldFont() {
	static const auto result = MakeFont(kSmallFontSize, st::semiboldFont);
	return result;
}

const style::font &TitleFont() {
	static const auto result = MakeFont(kTitleFontSize, st::semiboldFont);
	return result;
}

const style::ScrollArea &PanelScrollStyle() {
	static const auto result = [] {
		auto result = st::defaultScrollArea;
		result.bg = st::transparent;
		result.bgOver = st::mediaviewPipPlaybackInactive;
		result.barBg = st::groupCallMemberInactiveIcon;
		result.barBgOver = st::groupCallMemberNotJoinedStatus;
		return result;
	}();
	return result;
}

const style::FlatLabel &HintLabelStyle() {
	static const auto result = [] {
		auto result = st::defaultFlatLabel;
		result.textFg = st::groupCallMemberNotJoinedStatus;
		result.minWidth = Px(kHintMinWidth);
		return result;
	}();
	return result;
}

QString FormatSigned(int value) {
	if (value > 0) {
		return u"+"_q + QString::number(value);
	} else if (value < 0) {
		return QString(QChar(0x2212)) + QString::number(-value);
	}
	return u"0"_q;
}

QString FormatDecimal(double value, int decimals) {
	auto result = QString::number(std::abs(value), 'f', decimals);
	const auto separator = tr::lng_oblivion_photo_ui_decimal_separator(
		tr::now);
	if (!separator.isEmpty()) {
		result.replace(QChar('.'), separator);
	}
	const auto zero = (std::round(std::abs(value) * std::pow(10., decimals))
		== 0.);
	if (value < 0. && !zero) {
		result = QString(QChar(0x2212)) + result;
	}
	return result;
}

QString WithShortcut(const QString &text, const QKeySequence &keys) {
	return tr::lng_oblivion_photo_ui_shortcut(
		tr::now,
		lt_action,
		text,
		lt_keys,
		keys.toString(QKeySequence::NativeText));
}

void PaintIcon(QPainter &p, const IconRef &icon, QRect rect, QColor color) {
	if (!icon.icon) {
		return;
	}
	if (!icon.mirrored && !icon.rotation) {
		icon.icon->paintInCenter(p, rect, color);
		return;
	}
	p.save();
	const auto center = QRectF(rect).center();
	p.translate(center);
	if (icon.rotation) {
		p.rotate(icon.rotation);
	}
	if (icon.mirrored) {
		p.scale(-1., 1.);
	}
	p.translate(-center);
	icon.icon->paintInCenter(p, rect, color);
	p.restore();
}

ToolButton::ToolButton(QWidget *parent, IconRef icon, int size)
: RippleButton(parent, st::groupCallRipple)
, _icon(icon) {
	resize(size, size);
}

void ToolButton::setTooltip(QString text) {
	_tooltip = std::move(text);
}

void ToolButton::setAvailable(bool available) {
	if (_available == available) {
		return;
	}
	_available = available;
	setDisabled(!available);
	if (!available) {
		Ui::Tooltip::Hide();
	}
	update();
}

void ToolButton::setActive(bool active) {
	if (_active != active) {
		_active = active;
		update();
	}
}

QString ToolButton::tooltipText() const {
	return _tooltip;
}

QPoint ToolButton::tooltipPos() const {
	return QCursor::pos();
}

bool ToolButton::tooltipWindowActive() const {
	return Ui::AppInFocus() && Ui::InFocusChain(window());
}

void ToolButton::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	if (_active) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(anim::with_alpha(AccentColor(), 0.18));
		p.drawEllipse(rect());
	}
	paintRipple(p, 0, 0);
	const auto color = !_available
		? anim::with_alpha(TextColor(), kDimmedOpacity)
		: _active
		? AccentColor()
		: isOver()
		? st::mediaviewPipControlsFgOver->c
		: st::mediaviewPipControlsFg->c;
	PaintIcon(p, _icon, rect(), color);
}

void ToolButton::enterEventHook(QEnterEvent *e) {
	if (!_tooltip.isEmpty() && _available) {
		Ui::Tooltip::Show(kTooltipDelay, this);
	}
	RippleButton::enterEventHook(e);
}

void ToolButton::leaveEventHook(QEvent *e) {
	Ui::Tooltip::Hide();
	RippleButton::leaveEventHook(e);
}

QImage ToolButton::prepareRippleMask() const {
	return Ui::RippleAnimation::EllipseMask(size());
}

RowButton::RowButton(
	QWidget *parent,
	rpl::producer<QString> text,
	IconRef icon)
: RippleButton(parent, st::groupCallRipple)
, _icon(icon) {
	std::move(text) | rpl::on_next([=](QString value) {
		_text = std::move(value);
		update();
	}, lifetime());
}

void RowButton::setAvailable(bool available) {
	if (_available != available) {
		_available = available;
		setDisabled(!available);
		update();
	}
}

int RowButton::resizeGetHeight(int newWidth) {
	return Px(kRowHeight);
}

void RowButton::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	if (isOver() && _available) {
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::groupCallMembersBgOver);
		p.drawRoundedRect(rect(), Px(kRowRadius), Px(kRowRadius));
	}
	paintRipple(p, 0, 0);
	const auto color = _available
		? TextColor()
		: anim::with_alpha(TextColor(), kDimmedOpacity);
	const auto iconSize = Px(kRowIconSize);
	PaintIcon(
		p,
		_icon,
		QRect(
			Px(kRowIconLeft),
			(height() - iconSize) / 2,
			iconSize,
			iconSize),
		color);
	p.setPen(color);
	p.setFont(st::normalFont);
	const auto left = Px(kRowTextLeft);
	const auto available = width() - left - Px(kRowIconLeft);
	p.drawText(
		QRect(left, 0, available, height()),
		Qt::AlignLeft | Qt::AlignVCenter,
		st::normalFont->elided(_text, available));
}

QImage RowButton::prepareRippleMask() const {
	return Ui::RippleAnimation::RoundRectMask(size(), Px(kRowRadius));
}

PanelButton::PanelButton(
	QWidget *parent,
	rpl::producer<QString> text,
	bool primary)
: RippleButton(parent, primary ? st::defaultActiveButton.ripple : st::groupCallRipple)
, _primary(primary) {
	std::move(text) | rpl::on_next([=](QString value) {
		_text = std::move(value);
		update();
	}, lifetime());
}

void PanelButton::setAvailable(bool available) {
	if (_available != available) {
		_available = available;
		setDisabled(!_available || _busy);
		update();
	}
}

void PanelButton::setBusy(bool busy) {
	if (_busy != busy) {
		_busy = busy;
		setDisabled(!_available || _busy);
		update();
	}
}

int PanelButton::resizeGetHeight(int newWidth) {
	return Px(kPanelButtonHeight);
}

void PanelButton::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto enabled = _available && !_busy;
	const auto radius = Px(kPanelButtonRadius);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		// A secondary button is a darker well on the panel, like the chips
		// and the tab highlight: the lighter menu color was barely visible.
		auto bg = _primary
			? (isOver() ? st::activeButtonBgOver : st::activeButtonBg)->c
			: isOver()
			? anim::color(st::groupCallBg, st::groupCallMembersBg, 0.4)
			: st::groupCallBg->c;
		if (!enabled) {
			bg = anim::with_alpha(bg, kDimmedOpacity);
		}
		p.setBrush(bg);
		p.drawRoundedRect(rect(), radius, radius);
	}
	paintRipple(p, 0, 0);
	auto fg = _primary ? st::activeButtonFg->c : TextColor();
	if (!enabled) {
		fg = anim::with_alpha(fg, kHintOpacity);
	}
	p.setPen(fg);
	p.setFont(st::semiboldFont);
	p.drawText(
		rect(),
		Qt::AlignCenter,
		st::semiboldFont->elided(_text, width() - 2 * radius));
}

QImage PanelButton::prepareRippleMask() const {
	return Ui::RippleAnimation::RoundRectMask(
		size(),
		Px(kPanelButtonRadius));
}

SectionTitle::SectionTitle(QWidget *parent, rpl::producer<QString> text)
: RpWidget(parent) {
	std::move(text) | rpl::on_next([=](QString value) {
		_text = std::move(value);
		update();
	}, lifetime());
	setMouseTracking(true);
}

void SectionTitle::setAction(
		rpl::producer<QString> text,
		Fn<void()> callback) {
	_callback = std::move(callback);
	std::move(text) | rpl::on_next([=](QString value) {
		_action = std::move(value);
		update();
	}, lifetime());
}

void SectionTitle::setActionVisible(bool visible) {
	if (_actionVisible != visible) {
		_actionVisible = visible;
		if (!visible) {
			setActionOver(false);
			_actionDown = false;
		}
		update();
	}
}

int SectionTitle::resizeGetHeight(int newWidth) {
	return Px(kSectionHeight);
}

QRect SectionTitle::actionRect() const {
	if (!_actionVisible || _action.isEmpty()) {
		return QRect();
	}
	const auto font = st::normalFont;
	const auto width = font->width(_action);
	const auto bottom = height() - Px(kSectionTextBottom);
	return QRect(
		this->width() - width,
		bottom - font->height,
		width,
		font->height);
}

void SectionTitle::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto bottom = height() - Px(kSectionTextBottom);
	const auto action = actionRect();
	const auto available = action.isEmpty()
		? width()
		: (action.x() - Px(kChipSkip));
	p.setFont(st::semiboldFont);
	p.setPen(TextColor());
	p.drawText(
		QRect(0, bottom - st::semiboldFont->height, available, st::semiboldFont->height),
		Qt::AlignLeft | Qt::AlignVCenter,
		st::semiboldFont->elided(_text, available));
	if (!action.isEmpty()) {
		auto font = st::normalFont;
		if (_actionOver) {
			font = font->underline();
		}
		p.setFont(font);
		p.setPen(AccentColor());
		p.drawText(action, Qt::AlignLeft | Qt::AlignVCenter, _action);
	}
}

void SectionTitle::setActionOver(bool over) {
	if (_actionOver != over) {
		_actionOver = over;
		setCursor(over ? style::cur_pointer : style::cur_default);
		update();
	}
}

void SectionTitle::mouseMoveEvent(QMouseEvent *e) {
	setActionOver(actionRect().contains(e->pos()));
}

void SectionTitle::mousePressEvent(QMouseEvent *e) {
	_actionDown = (e->button() == Qt::LeftButton)
		&& actionRect().contains(e->pos());
}

void SectionTitle::mouseReleaseEvent(QMouseEvent *e) {
	if (base::take(_actionDown)
		&& actionRect().contains(e->pos())
		&& _callback) {
		_callback();
	}
}

void SectionTitle::leaveEventHook(QEvent *e) {
	setActionOver(false);
}

ValueSlider::ValueSlider(QWidget *parent, SliderArgs &&args)
: RpWidget(parent)
, _min(args.min)
, _max(std::max(args.max, args.min + 1))
, _default(std::clamp(args.defaultValue, _min, _max))
, _value(std::clamp(args.value, _min, _max))
, _format(std::move(args.format))
, _track(args.track) {
	std::move(args.label) | rpl::on_next([=](QString value) {
		_label = std::move(value);
		update();
	}, lifetime());
	setMouseTracking(true);
	setCursor(style::cur_pointer);
}

void ValueSlider::setValue(int value) {
	value = std::clamp(value, _min, _max);
	if (_value != value && !_pressed) {
		_value = value;
		update();
	}
}

int ValueSlider::value() const {
	return _value;
}

void ValueSlider::setDimmed(bool dimmed) {
	if (_dimmed != dimmed) {
		_dimmed = dimmed;
		update();
	}
}

bool ValueSlider::dragging() const {
	return _pressed;
}

rpl::producer<SliderChange> ValueSlider::changes() const {
	return _changes.events();
}

int ValueSlider::resizeGetHeight(int newWidth) {
	return Px(kSliderHeight);
}

QRect ValueSlider::trackRect() const {
	const auto radius = Px(kSliderKnobRadius + kSliderKnobOverGrow);
	const auto height = Px(kSliderTrackHeight);
	return QRect(
		radius,
		Px(kSliderTrackTop) - height / 2,
		std::max(width() - 2 * radius, 1),
		height);
}

int ValueSlider::valueFromX(int x) const {
	const auto track = trackRect();
	const auto ratio = std::clamp(
		(x - track.x()) / float64(track.width()),
		0.,
		1.);
	return _min + int(std::round(ratio * (_max - _min)));
}

int ValueSlider::xFromValue(int value) const {
	const auto track = trackRect();
	const auto ratio = (value - _min) / float64(_max - _min);
	return track.x() + int(std::round(ratio * track.width()));
}

void ValueSlider::updateFromX(int x, bool finished) {
	const auto value = valueFromX(x);
	const auto changed = (value != _value);
	_value = value;
	if (changed) {
		update();
	}
	if (changed || finished) {
		_changes.fire({ .value = _value, .finished = finished });
	}
}

void ValueSlider::paintTrack(QPainter &p, QRect track, float64 alpha) {
	const auto radius = track.height() / 2.;
	p.setPen(Qt::NoPen);
	if (_track != SliderTrack::Plain) {
		auto gradient = QLinearGradient(track.topLeft(), track.topRight());
		const auto temperature = (_track == SliderTrack::Temperature);
		gradient.setColorAt(0., temperature
			? QColor(74, 144, 226)
			: QColor(76, 187, 99));
		gradient.setColorAt(0.5, QColor(160, 160, 160));
		gradient.setColorAt(1., temperature
			? QColor(245, 196, 66)
			: QColor(212, 82, 200));
		p.setOpacity(alpha);
		p.setBrush(gradient);
		p.drawRoundedRect(track, radius, radius);
		p.setOpacity(1.);
		return;
	}
	p.setBrush(anim::with_alpha(TrackColor(), alpha));
	p.drawRoundedRect(track, radius, radius);

	const auto origin = (_min < 0 && _max > 0) ? 0 : _min;
	const auto from = xFromValue(origin);
	const auto till = xFromValue(_value);
	if (from != till) {
		const auto fill = QRect(
			std::min(from, till),
			track.y(),
			std::abs(till - from),
			track.height());
		p.setBrush(anim::with_alpha(AccentColor(), alpha));
		p.drawRoundedRect(fill, radius, radius);
	}
}

void ValueSlider::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto alpha = _dimmed ? kDimmedOpacity : 1.;
	const auto track = trackRect();
	const auto textTop = Px(kSliderTextTop);
	const auto font = st::normalFont;
	const auto valueText = _format
		? _format(_value)
		: QString::number(_value);
	const auto valueWidth = font->width(valueText);
	p.setFont(font);
	p.setPen(anim::with_alpha(
		(_value != _default) ? AccentColor() : SubTextColor(),
		alpha));
	p.drawText(
		QRect(width() - valueWidth, textTop, valueWidth, font->height),
		Qt::AlignRight | Qt::AlignVCenter,
		valueText);
	const auto labelWidth = width() - valueWidth - Px(kChipSkip);
	p.setPen(anim::with_alpha(TextColor(), alpha));
	p.drawText(
		QRect(0, textTop, labelWidth, font->height),
		Qt::AlignLeft | Qt::AlignVCenter,
		font->elided(_label, labelWidth));

	paintTrack(p, track, alpha);

	// The center mark only on bipolar sliders, where the fill starts from
	// it: on a one-sided slider a mark at the default value looked like a
	// stray glitch next to the knob.
	const auto origin = (_min < 0 && _max > 0) ? 0 : _min;
	if (_default == origin && _default > _min && _default < _max) {
		const auto x = xFromValue(_default);
		const auto tick = Px(kSliderTickHeight);
		p.setPen(Qt::NoPen);
		p.setBrush(anim::with_alpha(SubTextColor(), alpha));
		const auto line = std::max(Px(1), 1);
		p.drawRect(QRect(
			x - line / 2,
			track.y() + track.height() / 2 - tick / 2,
			line,
			tick));
	}

	const auto over = _overAnimation.value((_over || _pressed) ? 1. : 0.);
	const auto knob = Px(kSliderKnobRadius)
		+ Px(kSliderKnobOverGrow) * over;
	const auto center = QPointF(
		xFromValue(_value),
		track.y() + track.height() / 2.);
	p.setPen(Qt::NoPen);
	if (_pressed) {
		p.setBrush(anim::with_alpha(AccentColor(), 0.25));
		p.drawEllipse(center, knob * 1.8, knob * 1.8);
	}
	p.setBrush(_dimmed
		? anim::color(st::groupCallMembersBg, st::groupCallMembersFg, 0.6)
		: TextColor());
	p.drawEllipse(center, knob, knob);
}

void ValueSlider::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	_pressed = true;
	_pressValue = _value;
	// Grabbing the knob keeps the value (and the grab point under the
	// cursor), so a double click on the knob resets it without first
	// committing a jump. A click elsewhere on the track jumps there.
	const auto knobX = xFromValue(_value);
	const auto grab = Px(kSliderKnobRadius + kSliderKnobOverGrow + 2);
	_pressX = e->pos().x();
	_moved = false;
	_grabbed = (std::abs(_pressX - knobX) <= grab);
	_grabOffset = _grabbed ? (_pressX - knobX) : 0;
	if (!_grabbed) {
		updateFromX(_pressX, false);
	}
	update();
}

void ValueSlider::mouseMoveEvent(QMouseEvent *e) {
	if (!_pressed) {
		return;
	} else if (!_moved && e->pos().x() == _pressX) {
		return;
	}
	_moved = true;
	updateFromX(e->pos().x() - _grabOffset, false);
}

void ValueSlider::mouseReleaseEvent(QMouseEvent *e) {
	if (!_pressed || e->button() != Qt::LeftButton) {
		return;
	}
	if (_grabbed && !_moved && e->pos().x() == _pressX) {
		// A click on the knob: nothing changed.
		_changes.fire({ .value = _value, .finished = true });
	} else {
		updateFromX(e->pos().x() - _grabOffset, true);
	}
	_grabOffset = 0;
	_grabbed = false;
	_pressed = false;
	if (!_over) {
		_overAnimation.start([=] { update(); }, 1., 0., kSwitchDuration);
	}
	update();
}

void ValueSlider::mouseDoubleClickEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	_pressed = false;
	_value = _default;
	update();
	_changes.fire({ .value = _value, .finished = true });
}

void ValueSlider::enterEventHook(QEnterEvent *e) {
	_over = true;
	_overAnimation.start([=] { update(); }, 0., 1., kSwitchDuration);
}

void ValueSlider::leaveEventHook(QEvent *e) {
	_over = false;
	if (!_pressed) {
		_overAnimation.start([=] { update(); }, 1., 0., kSwitchDuration);
	}
}

class ChipsFlow::Chip final : public Ui::RippleButton {
public:
	Chip(QWidget *parent, rpl::producer<QString> text, Fn<void()> changed);

	void setSelected(bool selected);
	void setAvailable(bool available);

protected:
	void paintEvent(QPaintEvent *e) override;
	QImage prepareRippleMask() const override;

private:
	QString _text;
	bool _selected = false;
	bool _available = true;

};

ChipsFlow::Chip::Chip(
	QWidget *parent,
	rpl::producer<QString> text,
	Fn<void()> changed)
: RippleButton(parent, st::groupCallRipple) {
	std::move(text) | rpl::on_next([=](QString value) {
		_text = std::move(value);
		resize(
			st::normalFont->width(_text) + 2 * Px(kChipPadding),
			Px(kChipHeight));
		update();
		if (changed) {
			changed();
		}
	}, lifetime());
}

void ChipsFlow::Chip::setSelected(bool selected) {
	if (_selected != selected) {
		_selected = selected;
		update();
	}
}

void ChipsFlow::Chip::setAvailable(bool available) {
	if (_available != available) {
		_available = available;
		setDisabled(!available);
		update();
	}
}

void ChipsFlow::Chip::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto radius = height() / 2.;
	{
		auto hq = PainterHighQualityEnabler(p);
		if (_selected) {
			p.setPen(QPen(AccentColor(), std::max(Px(1), 1)));
			p.setBrush(anim::with_alpha(AccentColor(), 0.16));
			const auto half = std::max(Px(1), 1) / 2.;
			p.drawRoundedRect(
				QRectF(rect()).marginsRemoved({ half, half, half, half }),
				radius - half,
				radius - half);
		} else {
			p.setPen(Qt::NoPen);
			p.setBrush((isOver() && _available)
				? st::groupCallMembersBgRipple
				: st::groupCallBg);
			p.drawRoundedRect(rect(), radius, radius);
		}
	}
	paintRipple(p, 0, 0);
	const auto color = _selected
		? AccentColor()
		: _available
		? TextColor()
		: anim::with_alpha(TextColor(), kDimmedOpacity);
	p.setPen(color);
	p.setFont(st::normalFont);
	p.drawText(rect(), Qt::AlignCenter, _text);
}

QImage ChipsFlow::Chip::prepareRippleMask() const {
	return Ui::RippleAnimation::RoundRectMask(size(), height() / 2);
}

ChipsFlow::ChipsFlow(QWidget *parent) : RpWidget(parent) {
}

int ChipsFlow::addChip(rpl::producer<QString> text, Fn<void()> callback) {
	const auto index = int(_chips.size());
	const auto chip = Ui::CreateChild<Chip>(this, std::move(text), [=] {
		if (width() > 0) {
			resizeToWidth(width());
		}
	});
	chip->setClickedCallback(std::move(callback));
	chip->show();
	_chips.push_back(chip);
	if (width() > 0) {
		resizeToWidth(width());
	}
	return index;
}

void ChipsFlow::setSelected(int index) {
	_selected = index;
	for (auto i = 0; i != int(_chips.size()); ++i) {
		_chips[i]->setSelected(i == index);
	}
}

void ChipsFlow::setChipAvailable(int index, bool available) {
	if (index >= 0 && index < int(_chips.size())) {
		_chips[index]->setAvailable(available);
	}
}

int ChipsFlow::resizeGetHeight(int newWidth) {
	const auto skip = Px(kChipSkip);
	auto x = 0;
	auto y = 0;
	auto rowHeight = 0;
	for (const auto chip : _chips) {
		if (x > 0 && x + chip->width() > newWidth) {
			x = 0;
			y += rowHeight + skip;
			rowHeight = 0;
		}
		chip->moveToLeft(x, y, newWidth);
		x += chip->width() + skip;
		rowHeight = std::max(rowHeight, chip->height());
	}
	return _chips.empty() ? 0 : (y + rowHeight);
}

TabBar::TabBar(QWidget *parent, std::vector<TabInfo> tabs)
: RpWidget(parent) {
	_tabs.reserve(tabs.size());
	for (auto i = 0; i != int(tabs.size()); ++i) {
		_tabs.push_back({ .icon = tabs[i].icon });
	}
	for (auto i = 0; i != int(tabs.size()); ++i) {
		std::move(tabs[i].text) | rpl::on_next([=](QString value) {
			_tabs[i].text = std::move(value);
			refreshWidths();
			update();
		}, lifetime());
	}
	setMouseTracking(true);
}

void TabBar::resizeEvent(QResizeEvent *e) {
	refreshWidths();
}

void TabBar::refreshWidths() {
	const auto count = int(_tabs.size());
	_lefts.assign(count + 1, 0);
	if (!count) {
		return;
	}
	const auto total = std::max(width(), 0);
	const auto &font = SmallFont();
	const auto padding = Px(kTabTextPadding);
	const auto minimal = Px(kTabMinContent);
	auto natural = std::vector<int>(count);
	auto sum = 0;
	for (auto i = 0; i != count; ++i) {
		natural[i] = std::max(font->width(_tabs[i].text), minimal)
			+ 2 * padding;
		sum += natural[i];
	}
	auto widths = std::vector<int>(count);
	if (sum >= total) {
		// Not enough space: shrink proportionally, labels get elided.
		for (auto i = 0; i != count; ++i) {
			widths[i] = sum ? (natural[i] * total / sum) : (total / count);
		}
	} else {
		// Every tab gets an equal share of the free space.
		const auto extra = total - sum;
		for (auto i = 0; i != count; ++i) {
			widths[i] = natural[i]
				+ extra / count
				+ ((i < extra % count) ? 1 : 0);
		}
	}
	for (auto i = 0; i != count; ++i) {
		_lefts[i + 1] = _lefts[i] + widths[i];
	}
	_lefts[count] = total; // Rounding leftovers go to the last tab.
}

void TabBar::setActive(int index, anim::type animated) {
	index = std::clamp(index, 0, std::max(int(_tabs.size()) - 1, 0));
	if (_active == index) {
		return;
	}
	const auto from = _slide.value(float64(_active));
	_active = index;
	if (animated == anim::type::normal) {
		_slide.start(
			[=] { update(); },
			from,
			float64(index),
			kTabSlideDuration,
			anim::easeOutCirc);
	} else {
		_slide.stop();
	}
	update();
	_activeChanges.fire_copy(index);
}

int TabBar::active() const {
	return _active;
}

rpl::producer<int> TabBar::activeChanges() const {
	return _activeChanges.events();
}

int TabBar::resizeGetHeight(int newWidth) {
	return Px(kTabHeight);
}

QRect TabBar::tabRect(int index) const {
	const auto count = int(_tabs.size());
	if (index < 0 || index >= count || int(_lefts.size()) != count + 1) {
		return QRect();
	}
	return QRect(
		_lefts[index],
		0,
		_lefts[index + 1] - _lefts[index],
		height());
}

QRectF TabBar::highlightRect(float64 position) const {
	const auto count = int(_tabs.size());
	if (!count) {
		return QRectF();
	}
	position = std::clamp(position, 0., float64(count - 1));
	const auto from = int(std::floor(position));
	const auto till = std::min(from + 1, count - 1);
	const auto progress = position - from;
	const auto a = QRectF(tabRect(from));
	const auto b = QRectF(tabRect(till));
	const auto left = a.x() + (b.x() - a.x()) * progress;
	const auto right = a.x() + a.width()
		+ ((b.x() + b.width()) - (a.x() + a.width())) * progress;
	const auto inset = Px(kTabInset);
	return QRectF(
		left + inset,
		inset,
		std::max(right - left - 2 * inset, 0.),
		height() - 2 * inset);
}

int TabBar::tabAt(QPoint point) const {
	if (!rect().contains(point)) {
		return -1;
	}
	for (auto i = 0; i != int(_tabs.size()); ++i) {
		if (tabRect(i).contains(point)) {
			return i;
		}
	}
	return -1;
}

void TabBar::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	if (_tabs.empty()) {
		return;
	}
	const auto position = _slide.value(float64(_active));
	const auto count = int(_tabs.size());
	const auto highlight = highlightRect(position);
	p.setPen(Qt::NoPen);
	p.setBrush(st::groupCallBg);
	p.drawRoundedRect(highlight, Px(kTabRadius), Px(kTabRadius));

	const auto iconSize = Px(24);
	for (auto i = 0; i != count; ++i) {
		const auto rect = tabRect(i);
		const auto activeness = std::clamp(
			1. - std::abs(position - i),
			0.,
			1.);
		const auto base = (i == _over) ? TextColor() : SubTextColor();
		const auto color = anim::color(base, AccentColor(), activeness);
		PaintIcon(
			p,
			_tabs[i].icon,
			QRect(
				rect.x() + (rect.width() - iconSize) / 2,
				Px(kTabIconTop),
				iconSize,
				iconSize),
			color);
		const auto &font = SmallFont();
		p.setFont(font);
		p.setPen(color);
		p.drawText(
			QRect(rect.x(), Px(kTabTextTop), rect.width(), font->height),
			Qt::AlignHCenter | Qt::AlignTop,
			font->elided(
				_tabs[i].text,
				rect.width() - 2 * Px(kTabInset) - Px(kTabTextSkip)));
	}
}

void TabBar::mouseMoveEvent(QMouseEvent *e) {
	const auto over = tabAt(e->pos());
	if (_over != over) {
		_over = over;
		setCursor((over >= 0 && over != _active)
			? style::cur_pointer
			: style::cur_default);
		update();
	}
}

void TabBar::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = tabAt(e->pos());
	}
}

void TabBar::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, -1);
	if (pressed >= 0 && pressed == tabAt(e->pos())) {
		setActive(pressed);
	}
}

void TabBar::leaveEventHook(QEvent *e) {
	if (_over >= 0) {
		_over = -1;
		update();
	}
}

SwitchHeader::SwitchHeader(
	QWidget *parent,
	QString title,
	bool checked,
	bool withMenu)
: RpWidget(parent)
, _title(std::move(title))
, _checked(checked)
, _withMenu(withMenu) {
	setMouseTracking(true);
}

void SwitchHeader::setChecked(bool checked, anim::type animated) {
	if (_checked == checked) {
		return;
	}
	_checked = checked;
	if (animated == anim::type::normal) {
		_toggle.start(
			[=] { update(); },
			checked ? 0. : 1.,
			checked ? 1. : 0.,
			kSwitchDuration);
	} else {
		_toggle.stop();
	}
	update();
}

bool SwitchHeader::checked() const {
	return _checked;
}

rpl::producer<bool> SwitchHeader::toggles() const {
	return _toggles.events();
}

rpl::producer<QPoint> SwitchHeader::menuRequests() const {
	return _menuRequests.events();
}

int SwitchHeader::resizeGetHeight(int newWidth) {
	return Px(kSwitchHeaderHeight);
}

QRect SwitchHeader::menuRect() const {
	if (!_withMenu) {
		return QRect();
	}
	const auto size = Px(kSwitchMenuSize);
	return QRect(
		width() - size - Px(kSwitchMenuRight),
		(height() - size) / 2,
		size,
		size);
}

SwitchHeader::Part SwitchHeader::partAt(QPoint point) const {
	if (!rect().contains(point)) {
		return Part::None;
	} else if (menuRect().contains(point)) {
		return Part::Menu;
	}
	return Part::Toggle;
}

void SwitchHeader::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto progress = _toggle.value(_checked ? 1. : 0.);

	const auto switchRect = QRectF(
		Px(kSwitchLeft),
		(height() - Px(kSwitchHeight)) / 2.,
		Px(kSwitchWidth),
		Px(kSwitchHeight));
	const auto radius = switchRect.height() / 2.;
	p.setPen(Qt::NoPen);
	p.setBrush(anim::color(TrackColor(), AccentColor(), progress));
	p.drawRoundedRect(switchRect, radius, radius);
	const auto knob = radius - Px(2);
	const auto knobX = switchRect.x() + radius
		+ (switchRect.width() - 2 * radius) * progress;
	p.setBrush(TextColor());
	p.drawEllipse(QPointF(knobX, switchRect.center().y()), knob, knob);

	const auto menu = menuRect();
	if (_withMenu) {
		if (_over == Part::Menu) {
			p.setBrush(st::groupCallMembersBgOver);
			p.drawEllipse(menu);
		}
		p.setBrush((_over == Part::Menu) ? TextColor() : SubTextColor());
		const auto dot = Px(kSwitchDotRadius);
		const auto skip = Px(kSwitchDotSkip);
		const auto center = QPointF(menu.center()) + QPointF(0.5, 0.5);
		for (auto i = -1; i != 2; ++i) {
			p.drawEllipse(center + QPointF(0., i * skip), dot, dot);
		}
	}

	const auto left = Px(kSwitchTextLeft);
	const auto right = _withMenu ? menu.x() : width();
	const auto available = right - left - Px(kChipSkip);
	const auto &font = _withMenu ? st::semiboldFont : st::normalFont;
	p.setFont(font);
	p.setPen(_withMenu
		? anim::color(SubTextColor(), TextColor(), progress)
		: TextColor());
	p.drawText(
		QRect(left, 0, available, height()),
		Qt::AlignLeft | Qt::AlignVCenter,
		font->elided(_title, available));
}

void SwitchHeader::mouseMoveEvent(QMouseEvent *e) {
	const auto over = partAt(e->pos());
	if (_over != over) {
		_over = over;
		setCursor((over != Part::None) ? style::cur_pointer : style::cur_default);
		update();
	}
}

void SwitchHeader::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = partAt(e->pos());
	}
}

void SwitchHeader::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, Part::None);
	if (pressed == Part::None || pressed != partAt(e->pos())) {
		return;
	}
	if (pressed == Part::Menu) {
		const auto menu = menuRect();
		_menuRequests.fire(mapToGlobal(menu.bottomLeft()));
	} else {
		setChecked(!_checked, anim::type::normal);
		_toggles.fire_copy(_checked);
	}
}

void SwitchHeader::leaveEventHook(QEvent *e) {
	if (_over != Part::None) {
		_over = Part::None;
		update();
	}
}

ColorSwatches::ColorSwatches(
	QWidget *parent,
	rpl::producer<QString> label,
	std::vector<QColor> presets,
	QColor current)
: RpWidget(parent)
, _presets(std::move(presets))
, _current(current) {
	std::move(label) | rpl::on_next([=](QString value) {
		_label = std::move(value);
		update();
	}, lifetime());
	setMouseTracking(true);
}

void ColorSwatches::setColor(QColor color) {
	if (_current != color) {
		_current = color;
		update();
	}
}

rpl::producer<QColor> ColorSwatches::chosen() const {
	return _chosen.events();
}

rpl::producer<> ColorSwatches::customRequests() const {
	return _customRequests.events();
}

int ColorSwatches::resizeGetHeight(int newWidth) {
	return Px(kSwatchTop + kSwatchLabelHeight + kSwatchRowHeight);
}

int ColorSwatches::count() const {
	const auto step = Px(kSwatchSize + kSwatchSkip);
	const auto padding = Px(kSwatchPadding);
	const auto available = width() - Px(kSwatchSize) - 2 * padding;
	return std::clamp(available / step, 0, int(_presets.size()));
}

QRect ColorSwatches::swatchRect(int index) const {
	const auto size = Px(kSwatchSize);
	const auto step = Px(kSwatchSize + kSwatchSkip);
	return QRect(
		Px(kSwatchPadding) + index * step,
		Px(kSwatchTop + kSwatchLabelHeight)
			+ (Px(kSwatchRowHeight) - size) / 2,
		size,
		size);
}

int ColorSwatches::swatchAt(QPoint point) const {
	const auto total = count();
	for (auto i = 0; i <= total; ++i) {
		const auto ring = Px(kSwatchRing) * 2;
		if (swatchRect(i).marginsAdded({ ring, ring, ring, ring }).contains(
				point)) {
			return i;
		}
	}
	return -1;
}

void ColorSwatches::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	p.setFont(st::normalFont);
	p.setPen(TextColor());
	p.drawText(
		QRect(0, Px(kSwatchTop), width(), Px(kSwatchLabelHeight)),
		Qt::AlignLeft | Qt::AlignVCenter,
		st::normalFont->elided(_label, width()));

	const auto total = count();
	auto matched = false;
	const auto ring = Px(kSwatchRing);
	const auto paintRing = [&](QRect rect, QColor color) {
		auto pen = QPen(color, ring);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		const auto outer = QRectF(rect).marginsAdded(
			{ ring * 1.5, ring * 1.5, ring * 1.5, ring * 1.5 });
		p.drawEllipse(outer);
	};
	// A faint hairline keeps the dark presets (black, navy) visible on the
	// dark panel background.
	auto outline = TextColor();
	outline.setAlphaF(0.2);
	const auto hairline = style::ConvertScaleExact(1.);
	for (auto i = 0; i != total; ++i) {
		const auto rect = swatchRect(i);
		p.setPen(Qt::NoPen);
		p.setBrush(_presets[i]);
		p.drawEllipse(rect);
		p.setPen(QPen(outline, hairline));
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(QRectF(rect).marginsRemoved({
			hairline / 2.,
			hairline / 2.,
			hairline / 2.,
			hairline / 2. }));
		const auto selected = (_presets[i].rgb() == _current.rgb());
		if (selected) {
			matched = true;
			paintRing(rect, TextColor());
		} else if (i == _over) {
			paintRing(rect, SubTextColor());
		}
	}
	const auto custom = swatchRect(total);
	auto gradient = QConicalGradient(QRectF(custom).center(), 90.);
	const auto stops = 6;
	for (auto i = 0; i <= stops; ++i) {
		gradient.setColorAt(
			i / float64(stops),
			QColor::fromHsv((i * 360 / stops) % 360, 200, 255));
	}
	p.setPen(Qt::NoPen);
	p.setBrush(gradient);
	p.drawEllipse(custom);
	const auto inner = Px(kSwatchSize) / 4.;
	p.setBrush(matched ? st::groupCallMembersBg->c : _current);
	p.drawEllipse(QRectF(custom).marginsRemoved(
		{ inner, inner, inner, inner }));
	if (!matched) {
		paintRing(custom, TextColor());
	} else if (_over == total) {
		paintRing(custom, SubTextColor());
	}
}

void ColorSwatches::mouseMoveEvent(QMouseEvent *e) {
	const auto over = swatchAt(e->pos());
	if (_over != over) {
		_over = over;
		setCursor((over >= 0) ? style::cur_pointer : style::cur_default);
		update();
	}
}

void ColorSwatches::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = swatchAt(e->pos());
	}
}

void ColorSwatches::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, -1);
	if (pressed < 0 || pressed != swatchAt(e->pos())) {
		return;
	}
	if (pressed == count()) {
		_customRequests.fire({});
	} else {
		_current = _presets[pressed];
		update();
		_chosen.fire_copy(_current);
	}
}

void ColorSwatches::leaveEventHook(QEvent *e) {
	if (_over >= 0) {
		_over = -1;
		update();
	}
}

FilterStrip::FilterStrip(QWidget *parent) : RpWidget(parent) {
	setMouseTracking(true);
}

int FilterStrip::ThumbnailSide() {
	return Px(kThumbSide);
}

int FilterStrip::StripHeight() {
	return Px(kStripTop + kThumbSide + kStripNameSkip + kStripBottom)
		+ SmallFont()->height;
}

void FilterStrip::setItems(std::vector<StripItem> items) {
	_items = std::move(items);
	_thumbnails.resize(_items.size());
	scrollTo(std::clamp(_scroll, 0, maxScroll()), anim::type::instant);
	update();
}

void FilterStrip::setThumbnails(std::vector<QImage> thumbnails) {
	_thumbnails = std::move(thumbnails);
	_thumbnails.resize(_items.size());
	update();
}

bool FilterStrip::thumbnailsReady() const {
	return !_thumbnails.empty()
		&& ranges::none_of(_thumbnails, &QImage::isNull);
}

void FilterStrip::setSelected(const QString &id, anim::type animated) {
	if (_selected == id) {
		return;
	}
	_selected = id;
	const auto i = ranges::find(_items, id, &StripItem::id);
	if (i != end(_items)) {
		ensureVisible(int(i - begin(_items)), animated);
	}
	update();
}

rpl::producer<QString> FilterStrip::selections() const {
	return _selections.events();
}

int FilterStrip::tileWidth() const {
	return Px(kThumbSide + kTileExtra);
}

int FilterStrip::contentWidth() const {
	return 2 * Px(kStripPadding) + int(_items.size()) * tileWidth();
}

int FilterStrip::maxScroll() const {
	return std::max(contentWidth() - width(), 0);
}

QRect FilterStrip::tileRect(int index) const {
	return QRect(
		Px(kStripPadding) + index * tileWidth() - _scroll,
		0,
		tileWidth(),
		height());
}

int FilterStrip::tileAt(QPoint point) const {
	if (!rect().contains(point)) {
		return -1;
	}
	const auto x = point.x() + _scroll - Px(kStripPadding);
	if (x < 0) {
		return -1;
	}
	const auto index = x / tileWidth();
	return (index < int(_items.size())) ? index : -1;
}

void FilterStrip::scrollTo(int scroll, anim::type animated) {
	scroll = std::clamp(scroll, 0, maxScroll());
	if (animated == anim::type::normal && isVisible()) {
		_scrollFrom = _scroll;
		_scrollTo = scroll;
		_scrollAnimation.start([=] {
			_scroll = anim::interpolate(
				_scrollFrom,
				_scrollTo,
				_scrollAnimation.value(1.));
			update();
		}, 0., 1., kStripScrollDuration, anim::easeOutCirc);
	} else {
		_scrollAnimation.stop();
		_scroll = scroll;
		update();
	}
}

void FilterStrip::ensureVisible(int index, anim::type animated) {
	if (width() <= 0) {
		return;
	}
	const auto target = _scrollAnimation.animating() ? _scrollTo : _scroll;
	const auto left = Px(kStripPadding) + index * tileWidth();
	const auto right = left + tileWidth();
	// Keep at least half a neighbour tile visible on both sides, and when
	// the tile is out of that zone bring it to the middle of the strip, so
	// the strip never stops with a tile cut in half next to the selection.
	const auto padding = Px(kStripPadding) + tileWidth() / 2;
	if (left - padding < target || right + padding > target + width()) {
		scrollTo(left + tileWidth() / 2 - width() / 2, animated);
	}
}

void FilterStrip::resizeEvent(QResizeEvent *e) {
	scrollTo(_scroll, anim::type::instant);
	const auto i = ranges::find(_items, _selected, &StripItem::id);
	if (i != end(_items)) {
		ensureVisible(int(i - begin(_items)), anim::type::instant);
	}
}

void FilterStrip::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto side = Px(kThumbSide);
	const auto radius = Px(kThumbRadius);
	const auto &font = SmallFont();
	const auto &selectedFont = SmallSemiboldFont();
	const auto clip = e->rect();
	for (auto i = 0; i != int(_items.size()); ++i) {
		const auto tile = tileRect(i);
		if (!tile.intersects(clip)) {
			continue;
		}
		const auto selected = (_items[i].id == _selected);
		const auto thumb = QRect(
			tile.x() + (tile.width() - side) / 2,
			Px(kStripTop),
			side,
			side);
		if (selected) {
			const auto ring = Px(kStripRing);
			const auto skip = Px(kStripRingSkip);
			p.setPen(QPen(AccentColor(), ring));
			p.setBrush(Qt::NoBrush);
			const auto outer = QRectF(thumb).marginsAdded(
				{ skip - ring / 2., skip - ring / 2., skip - ring / 2., skip - ring / 2. });
			p.drawRoundedRect(outer, radius + skip, radius + skip);
		}
		const auto &image = (i < int(_thumbnails.size()))
			? _thumbnails[i]
			: QImage();
		if (image.isNull()) {
			p.setPen(Qt::NoPen);
			p.setBrush(TrackColor());
			p.drawRoundedRect(thumb, radius, radius);
		} else {
			auto path = QPainterPath();
			path.addRoundedRect(QRectF(thumb), radius, radius);
			p.save();
			p.setClipPath(path);
			p.drawImage(thumb, image);
			p.restore();
		}
		if (i == _over && !selected) {
			p.setPen(Qt::NoPen);
			p.setBrush(anim::with_alpha(TextColor(), 0.12));
			p.drawRoundedRect(thumb, radius, radius);
		}
		const auto &nameFont = selected ? selectedFont : font;
		p.setFont(nameFont);
		p.setPen(selected
			? AccentColor()
			: (i == _over)
			? TextColor()
			: SubTextColor());
		p.drawText(
			QRect(
				tile.x(),
				thumb.y() + side + Px(kStripNameSkip),
				tile.width(),
				nameFont->height),
			Qt::AlignHCenter | Qt::AlignTop,
			nameFont->elided(_items[i].name, tile.width() - Px(4)));
	}
	const auto fade = Px(kStripFade);
	const auto paintFade = [&](int x, bool left) {
		auto gradient = QLinearGradient(x, 0, x + fade, 0);
		const auto bg = st::groupCallBg->c;
		gradient.setColorAt(left ? 0. : 1., bg);
		gradient.setColorAt(left ? 1. : 0., anim::with_alpha(bg, 0.));
		p.fillRect(QRect(x, 0, fade, height()), gradient);
	};
	if (_scroll > 0) {
		paintFade(0, true);
	}
	if (_scroll < maxScroll()) {
		paintFade(width() - fade, false);
	}
}

void FilterStrip::wheelEvent(QWheelEvent *e) {
	const auto pixel = e->pixelDelta();
	const auto angle = e->angleDelta();
	auto delta = 0;
	if (!pixel.isNull()) {
		delta = (std::abs(pixel.x()) > std::abs(pixel.y()))
			? pixel.x()
			: pixel.y();
	} else {
		const auto value = (std::abs(angle.x()) > std::abs(angle.y()))
			? angle.x()
			: angle.y();
		delta = (value * tileWidth()) / 120;
	}
	if (delta) {
		scrollTo(_scroll - delta, anim::type::instant);
	}
	e->accept();
}

void FilterStrip::mouseMoveEvent(QMouseEvent *e) {
	if (_pressed >= 0 || _dragging) {
		const auto shift = e->pos().x() - _pressPoint.x();
		if (!_dragging && std::abs(shift) >= Px(kStripDragThreshold)) {
			_dragging = true;
			setCursor(style::cur_default);
		}
		if (_dragging) {
			scrollTo(_pressScroll - shift, anim::type::instant);
			return;
		}
	}
	const auto over = tileAt(e->pos());
	if (_over != over) {
		_over = over;
		setCursor((over >= 0) ? style::cur_pointer : style::cur_default);
		update();
	}
}

void FilterStrip::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	_pressed = tileAt(e->pos());
	_pressPoint = e->pos();
	_pressScroll = _scroll;
	_dragging = false;
	if (_pressed < 0) {
		_pressed = int(_items.size());
	}
}

void FilterStrip::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, -1);
	const auto dragging = std::exchange(_dragging, false);
	if (dragging || pressed < 0 || pressed >= int(_items.size())) {
		return;
	} else if (pressed == tileAt(e->pos())) {
		const auto id = _items[pressed].id;
		setSelected(id, anim::type::normal);
		_selections.fire_copy(id);
	}
}

void FilterStrip::leaveEventHook(QEvent *e) {
	if (_over >= 0) {
		_over = -1;
		update();
	}
}

} // namespace Oblivion::Photo::EditorUi
