/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_lottie_editor_inspector.h"

#include "base/platform/base_platform_info.h"
#include "lang/lang_keys.h"
#include "oblivion/oblivion_lottie_doc.h"
#include "oblivion/oblivion_lottie_editor.h"
#include "oblivion/oblivion_lottie_editor_palette.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/abstract_button.h"
#include "ui/effects/animation_value.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/pill_tabs.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/tooltip.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>

#include <cmath>

namespace Oblivion::LottieEdit {
namespace {

constexpr auto kPadding = 12;
constexpr auto kRowHeight = 34;
constexpr auto kFieldHeight = 26;
// Values sit in a grid of two columns on the right: a single number takes
// the right column, a pair both, colors and dropdowns the whole area, so
// the left and right edges of the controls line up from row to row.
constexpr auto kFieldWidth = 64;
constexpr auto kFieldSkip = 4;
constexpr auto kValueAreaWidth = kFieldWidth * 2 + kFieldSkip;
constexpr auto kKeyframesWidth = 42;
constexpr auto kLabelSkip = 4;
constexpr auto kLabelMin = 64;
constexpr auto kSwatchSize = 22;
constexpr auto kSwatchSkip = 6;
constexpr auto kHexWidth = 84; // Enough for "#RRGGBB", when space is short.
constexpr auto kGradientBar = 14;
constexpr auto kGradientHandle = 12;
constexpr auto kGradientPointer = 3;
// Room for the handles on one side of the bar.
constexpr auto kGradientSide = kGradientHandle + kGradientPointer + 1;
// Above the bar when there are no opacity stops (handles only below).
constexpr auto kGradientCompactTop = 6;
constexpr auto kTooltipDelay = 800;
constexpr auto kDragThreshold = 3;
constexpr auto kKeyframeEpsilon = 0.01;

[[nodiscard]] int Scaled(int value) {
	return style::ConvertScale(value);
}

// Left edge of the property labels (after the keyframe controls), the
// hints that belong to a property start there too.
[[nodiscard]] int PropertyLabelLeft() {
	return Scaled(kPadding) / 2 + Scaled(kKeyframesWidth) + Scaled(kLabelSkip);
}

// The active tab gets a pill under its label (like the tabs of the emoji /
// stickers / GIFs panel), not only an accent text color; the "over" color
// of the pill reads in both the day and the night themes.
[[nodiscard]] const style::PillTabs &TabsStyle() {
	static const auto result = [] {
		auto st = st::defaultPillTabs;
		st.bgBorder = st::windowBg;
		st.bg = st::windowBg;
		st.bgActive = st::windowBgOver;
		st.borderWidth = Scaled(3);
		return st;
	}();
	return result;
}

[[nodiscard]] InspectorPanel::Tab &LastTab() {
	static auto result = InspectorPanel::Tab::Properties;
	return result;
}

// Composition edits options, kept for the app session.
[[nodiscard]] bool &ScaleContentOption() {
	static auto result = true;
	return result;
}

[[nodiscard]] bool &KeepDurationOption() {
	static auto result = true;
	return result;
}

// Moves the keyboard focus to the panel that holds the widget (panels
// have Qt::ClickFocus), used when an inline field goes away.
void FocusOwningPanel(not_null<QWidget*> widget) {
	for (auto parent = widget->parentWidget()
		; parent
		; parent = parent->parentWidget()) {
		if (parent->focusPolicy() != Qt::NoFocus) {
			parent->setFocus();
			return;
		} else if (parent->isWindow()) {
			return;
		}
	}
}

// Color numbers without alpha, so a stored alpha is kept.
[[nodiscard]] PropValue ColorValue(const QColor &color) {
	return PropValue{
		.numbers = { color.redF(), color.greenF(), color.blueF() },
	};
}

[[nodiscard]] QColor NumbersColor(const std::vector<double> &numbers, int at) {
	const auto get = [&](int index) {
		return (index < int(numbers.size()))
			? std::clamp(numbers[index], 0., 1.)
			: 0.;
	};
	return QColor::fromRgbF(get(at), get(at + 1), get(at + 2));
}

// Color stops by offset ({ offset, r, g, b } groups), then opacity stops
// ({ offset, alpha } pairs), like exporters write them.
void SortGradientStops(std::vector<double> &numbers, int colorStops) {
	const auto colors = std::min(colorStops, int(numbers.size()) / 4);
	auto color = std::vector<std::array<double, 4>>();
	for (auto i = 0; i != colors; ++i) {
		color.push_back({
			numbers[i * 4],
			numbers[i * 4 + 1],
			numbers[i * 4 + 2],
			numbers[i * 4 + 3],
		});
	}
	ranges::stable_sort(color, ranges::less(), [](const auto &stop) {
		return stop[0];
	});
	for (auto i = 0; i != colors; ++i) {
		for (auto j = 0; j != 4; ++j) {
			numbers[i * 4 + j] = color[i][j];
		}
	}
	const auto from = colors * 4;
	const auto pairs = (int(numbers.size()) - from) / 2;
	auto opacity = std::vector<std::array<double, 2>>();
	for (auto i = 0; i != pairs; ++i) {
		opacity.push_back({ numbers[from + i * 2], numbers[from + i * 2 + 1] });
	}
	ranges::stable_sort(opacity, ranges::less(), [](const auto &stop) {
		return stop[0];
	});
	for (auto i = 0; i != pairs; ++i) {
		numbers[from + i * 2] = opacity[i][0];
		numbers[from + i * 2 + 1] = opacity[i][1];
	}
}

// Value fields.

struct FieldOptions {
	QString suffix;
	double step = 1.; // Change per dragged pixel.
	double min = -1e6;
	double max = 1e6;
	int decimals = 2;
	bool integer = false;
	bool text = false; // Text (hex) instead of a number, no dragging.
	bool liveDrag = true; // false: the value is applied on release.
};

[[nodiscard]] FieldOptions OptionsFor(PropertyRole role) {
	const auto degrees = QString(QChar(0x00B0));
	const auto percent = u"%"_q;
	switch (role) {
	case PropertyRole::Opacity:
	case PropertyRole::MaskOpacity:
	case PropertyRole::StartOpacity:
	case PropertyRole::EndOpacity:
	case PropertyRole::TrimStart:
	case PropertyRole::TrimEnd:
		return { .suffix = percent, .step = 0.5, .min = 0., .max = 100., .decimals = 1 };
	case PropertyRole::Scale:
		return { .suffix = percent, .step = 0.5, .decimals = 1 };
	case PropertyRole::HighlightLength:
		return { .suffix = percent, .step = 0.5, .min = -100., .max = 100., .decimals = 1 };
	case PropertyRole::InnerRoundness:
	case PropertyRole::OuterRoundness:
		return { .suffix = percent, .step = 0.5, .decimals = 1 };
	case PropertyRole::Rotation:
	case PropertyRole::RotationX:
	case PropertyRole::RotationY:
	case PropertyRole::TrimOffset:
	case PropertyRole::HighlightAngle:
	case PropertyRole::Skew:
	case PropertyRole::SkewAxis:
		return { .suffix = degrees, .step = 1., .decimals = 1 };
	case PropertyRole::StrokeWidth:
	case PropertyRole::Dash:
		return { .step = 0.25, .min = 0., .decimals = 2 };
	case PropertyRole::Size:
	case PropertyRole::InnerRadius:
	case PropertyRole::OuterRadius:
		return { .step = 1., .min = 0., .decimals = 1 };
	case PropertyRole::Roundness:
	case PropertyRole::Radius:
		return { .step = 0.5, .min = 0., .decimals = 1 };
	case PropertyRole::Points:
		return { .step = 0.1, .min = 3., .max = 100., .decimals = 0, .integer = true };
	case PropertyRole::Copies:
		return { .step = 0.05, .min = 0., .max = 1000., .decimals = 1 };
	case PropertyRole::TimeRemap:
		return { .step = 0.01, .min = 0., .decimals = 2 };
	default:
		return { .step = 1., .decimals = 1 };
	}
}

class ValueField final : public Ui::RpWidget {
public:
	ValueField(QWidget *parent, FieldOptions options);

	void setNumber(double value);
	void setText(const QString &text);
	void setAccent(bool accent);
	[[nodiscard]] bool busy() const;
	void commitEditing();

	// Every value while dragging (liveDrag), the value at release.
	[[nodiscard]] rpl::producer<double> dragged() const;
	[[nodiscard]] rpl::producer<double> dragFinished() const;
	[[nodiscard]] rpl::producer<double> numberSubmitted() const;
	[[nodiscard]] rpl::producer<QString> textSubmitted() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void enterEventHook(QEnterEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	void startEditing();
	void finishEditing(bool apply);
	[[nodiscard]] double normalize(double value) const;
	[[nodiscard]] QString numberText() const;

	const FieldOptions _options;
	double _value = 0.;
	QString _text;
	bool _accent = false;
	bool _over = false;
	bool _pressed = false;
	bool _dragging = false;
	QPoint _pressPosition;
	double _pressValue = 0.;
	base::unique_qptr<FieldHost> _editHost;
	Ui::InputField *_edit = nullptr;
	rpl::event_stream<double> _dragged;
	rpl::event_stream<double> _dragFinished;
	rpl::event_stream<double> _numberSubmitted;
	rpl::event_stream<QString> _textSubmitted;

};

ValueField::ValueField(QWidget *parent, FieldOptions options)
: RpWidget(parent)
, _options(std::move(options)) {
	setCursor(_options.text ? style::cur_text : style::cur_sizehor);
	resize(Scaled(kFieldWidth), Scaled(kFieldHeight));
}

void ValueField::setNumber(double value) {
	if (busy()) {
		return;
	}
	value = normalize(value);
	if (_value != value) {
		_value = value;
		update();
	}
}

void ValueField::setText(const QString &text) {
	if (busy() || _text == text) {
		return;
	}
	_text = text;
	update();
}

void ValueField::setAccent(bool accent) {
	if (_accent != accent) {
		_accent = accent;
		update();
	}
}

bool ValueField::busy() const {
	return _dragging || (_editHost != nullptr);
}

rpl::producer<double> ValueField::dragged() const {
	return _dragged.events();
}

rpl::producer<double> ValueField::dragFinished() const {
	return _dragFinished.events();
}

rpl::producer<double> ValueField::numberSubmitted() const {
	return _numberSubmitted.events();
}

rpl::producer<QString> ValueField::textSubmitted() const {
	return _textSubmitted.events();
}

double ValueField::normalize(double value) const {
	if (!std::isfinite(value)) {
		value = 0.;
	}
	value = std::clamp(value, _options.min, _options.max);
	if (_options.integer) {
		return std::round(value);
	}
	const auto factor = std::pow(10., std::clamp(_options.decimals + 2, 0, 6));
	return std::round(value * factor) / factor;
}

QString ValueField::numberText() const {
	return FormatDecimal(_value, _options.integer ? 0 : _options.decimals);
}

void ValueField::paintEvent(QPaintEvent *e) {
	if (_editHost) {
		return;
	}
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto radius = Scaled(6);
	const auto active = _dragging || _pressed;
	p.setPen(Qt::NoPen);
	p.setBrush((_over || active) ? st::windowBgRipple : st::windowBgOver);
	p.drawRoundedRect(QRectF(rect()), radius, radius);
	if (_dragging) {
		auto pen = QPen(st::activeLineFg);
		pen.setWidthF(st::lineWidth);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(
			QRectF(rect()).marginsRemoved(QMarginsF(0.5, 0.5, 0.5, 0.5)),
			radius,
			radius);
	}
	const auto text = _options.text
		? _text
		: (numberText() + _options.suffix);
	const auto skip = Scaled(4);
	p.setFont(_options.text ? st::normalFont->monospace() : st::normalFont);
	p.setPen(_accent ? st::windowActiveTextFg : st::windowFg);
	const auto font = _options.text
		? st::normalFont->monospace()
		: st::normalFont;
	p.drawText(
		rect().marginsRemoved(QMargins(skip, 0, skip, 0)),
		Qt::AlignCenter,
		font->elided(text, width() - 2 * skip));
}

void ValueField::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton || _editHost) {
		return;
	}
	_pressed = true;
	_pressPosition = e->globalPosition().toPoint();
	_pressValue = _value;
	update();
}

void ValueField::mouseMoveEvent(QMouseEvent *e) {
	if (!_pressed || _options.text) {
		return;
	}
	const auto delta = e->globalPosition().toPoint().x() - _pressPosition.x();
	if (!_dragging) {
		if (std::abs(delta) < Scaled(kDragThreshold)) {
			return;
		}
		_dragging = true;
		Ui::Tooltip::Hide();
	}
	auto speed = _options.step;
	if (e->modifiers() & Qt::ShiftModifier) {
		speed *= 10.;
	} else if (e->modifiers() & Qt::AltModifier) {
		speed *= 0.1;
	}
	const auto value = normalize(_pressValue + delta * speed);
	if (value != _value) {
		_value = value;
		update();
		if (_options.liveDrag) {
			_dragged.fire_copy(value);
		}
	}
}

void ValueField::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton || !_pressed) {
		return;
	}
	_pressed = false;
	if (_dragging) {
		_dragging = false;
		update();
		_dragFinished.fire_copy(_value);
	} else {
		update();
		startEditing();
	}
}

void ValueField::enterEventHook(QEnterEvent *e) {
	_over = true;
	update();
}

void ValueField::leaveEventHook(QEvent *e) {
	_over = false;
	update();
}

void ValueField::startEditing() {
	if (_editHost) {
		return;
	}
	_editHost = base::make_unique_q<FieldHost>(this);
	_editHost->setGeometry(rect());
	_edit = Ui::CreateChild<Ui::InputField>(
		_editHost.get(),
		PanelFieldStyle(),
		Ui::InputField::Mode::SingleLine,
		nullptr,
		_options.text ? _text : numberText());
	_edit->setGeometry(_editHost->rect());
	_editHost->show();
	_edit->show();
	_edit->selectAll();
	_edit->setFocusFast();
	_edit->submits() | rpl::on_next([=] {
		finishEditing(true);
	}, _edit->lifetime());
	_edit->cancelled() | rpl::on_next([=] {
		finishEditing(false);
	}, _edit->lifetime());
	_edit->focusedChanges() | rpl::filter(
		!rpl::mappers::_1
	) | rpl::on_next([=] {
		finishEditing(true);
	}, _edit->lifetime());
	update();
}

void ValueField::commitEditing() {
	finishEditing(true);
}

void ValueField::finishEditing(bool apply) {
	if (!_editHost) {
		return;
	}
	auto host = std::move(_editHost);
	const auto edit = std::exchange(_edit, nullptr);
	auto text = edit->getLastText().trimmed();
	if (Ui::InFocusChain(host.get())) {
		FocusOwningPanel(this);
	}
	host->hide();
	host.release()->deleteLater();
	update();
	if (!apply) {
		return;
	} else if (_options.text) {
		_textSubmitted.fire_copy(text);
		return;
	}
	text.replace(',', '.');
	text.replace(QChar(0x2212), '-');
	if (!_options.suffix.isEmpty()) {
		text.remove(_options.suffix);
	}
	text.remove(' ');
	auto ok = false;
	const auto parsed = text.toDouble(&ok);
	if (!ok) {
		return;
	}
	const auto value = normalize(parsed);
	_value = value;
	update();
	_numberSubmitted.fire_copy(value);
}

// A color sample button.

class SwatchButton final
	: public Ui::AbstractButton
	, public Ui::AbstractTooltipShower {
public:
	explicit SwatchButton(QWidget *parent);

	void setColor(QColor color);

	QString tooltipText() const override;
	QPoint tooltipPos() const override;
	bool tooltipWindowActive() const override;

protected:
	void paintEvent(QPaintEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;
	void enterEventHook(QEnterEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	QColor _color;

};

SwatchButton::SwatchButton(QWidget *parent)
: AbstractButton(parent) {
	setPointerCursor(true);
	resize(Scaled(kSwatchSize), Scaled(kSwatchSize));
}

void SwatchButton::setColor(QColor color) {
	if (_color != color) {
		_color = color;
		update();
	}
}

QString SwatchButton::tooltipText() const {
	return tr::lng_oblivion_lottie_inspector_change_color(tr::now);
}

QPoint SwatchButton::tooltipPos() const {
	return QCursor::pos();
}

bool SwatchButton::tooltipWindowActive() const {
	return Ui::AppInFocus() && Ui::InFocusChain(window());
}

void SwatchButton::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto inset = isDown() ? 2. : isOver() ? 0. : 1.;
	PaintSwatch(
		p,
		QRectF(rect()).marginsRemoved(QMarginsF(inset, inset, inset, inset)),
		_color.isValid() ? _color : st::windowBgOver->c,
		Scaled(6));
}

void SwatchButton::onStateChanged(State was, StateChangeSource source) {
	update();
}

void SwatchButton::enterEventHook(QEnterEvent *e) {
	Ui::Tooltip::Show(kTooltipDelay, this);
	AbstractButton::enterEventHook(e);
}

void SwatchButton::leaveEventHook(QEvent *e) {
	Ui::Tooltip::Hide();
	AbstractButton::leaveEventHook(e);
}

// A dropdown for plain enum values (caps, corners, fill rule...).

class ChoiceButton final : public Ui::AbstractButton {
public:
	ChoiceButton(QWidget *parent, std::vector<QString> options);

	void setIndex(int index);
	[[nodiscard]] rpl::producer<int> chosen() const;
	[[nodiscard]] int fullWidth() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;

private:
	void showMenu();

	const std::vector<QString> _options;
	int _index = -1;
	base::unique_qptr<Ui::PopupMenu> _menu;
	rpl::event_stream<int> _chosen;

};

ChoiceButton::ChoiceButton(QWidget *parent, std::vector<QString> options)
: AbstractButton(parent)
, _options(std::move(options)) {
	setPointerCursor(true);
	resize(fullWidth(), Scaled(kFieldHeight));
	setClickedCallback([=] { showMenu(); });
}

void ChoiceButton::setIndex(int index) {
	if (_index != index) {
		_index = index;
		update();
	}
}

rpl::producer<int> ChoiceButton::chosen() const {
	return _chosen.events();
}

int ChoiceButton::fullWidth() const {
	auto result = 0;
	for (const auto &option : _options) {
		result = std::max(result, st::normalFont->width(option));
	}
	return result + Scaled(10) * 2 + Scaled(14);
}

void ChoiceButton::showMenu() {
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::popupMenuWithIcons);
	for (auto i = 0; i != int(_options.size()); ++i) {
		_menu->addAction(_options[i], [=] {
			if (_index != i) {
				_index = i;
				update();
				_chosen.fire_copy(i);
			}
		}, nullptr);
	}
	_menu->popup(mapToGlobal(QPoint(0, height())));
}

void ChoiceButton::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(isOver() ? st::windowBgRipple : st::windowBgOver);
	p.drawRoundedRect(QRectF(rect()), Scaled(6), Scaled(6));

	const auto skip = Scaled(10);
	const auto arrow = Scaled(14);
	const auto text = (_index >= 0 && _index < int(_options.size()))
		? _options[_index]
		: QString();
	p.setFont(st::normalFont);
	p.setPen(st::windowFg);
	p.drawText(
		QRect(skip, 0, std::max(width() - skip - arrow - skip / 2, 0), height()),
		Qt::AlignLeft | Qt::AlignVCenter,
		st::normalFont->elided(text, width() - skip - arrow - skip / 2));

	auto pen = QPen(st::windowSubTextFg);
	pen.setWidthF(Scaled(15) / 10.);
	pen.setCapStyle(Qt::RoundCap);
	pen.setJoinStyle(Qt::RoundJoin);
	p.setPen(pen);
	const auto center = QPointF(width() - skip - arrow / 4., height() / 2.);
	const auto size = Scaled(3) + 0.5;
	auto path = QPainterPath();
	path.moveTo(center.x() - size, center.y() - size / 2.);
	path.lineTo(center.x(), center.y() + size / 2.);
	path.lineTo(center.x() + size, center.y() - size / 2.);
	p.drawPath(path);
}

void ChoiceButton::onStateChanged(State was, StateChangeSource source) {
	update();
}

// Gradient stops editor: color stops below the bar, opacity stops above.

class GradientBar final : public Ui::RpWidget {
public:
	struct Move {
		bool opacity = false;
		int index = 0;
		double offset = 0.;
		bool finished = false;
	};

	explicit GradientBar(QWidget *parent);

	void setStops(
		std::vector<GradientStop> colors,
		std::vector<GradientStop> opacities);
	[[nodiscard]] bool busy() const;
	[[nodiscard]] rpl::producer<Move> moves() const;
	[[nodiscard]] rpl::producer<int> colorClicks() const;

	// Opacity handles go above the bar, so the room for them is taken
	// only when there are opacity stops.
	[[nodiscard]] int wantedHeight() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	struct Handle {
		bool opacity = false;
		int index = -1;

		friend inline bool operator==(
			const Handle &a,
			const Handle &b) = default;
	};

	[[nodiscard]] QRect barRect() const;
	[[nodiscard]] QRectF handleRect(bool opacity, double offset) const;
	[[nodiscard]] std::optional<Handle> handleAt(QPoint position) const;
	[[nodiscard]] double offsetAt(int x) const;
	[[nodiscard]] std::vector<GradientStop> &stops(bool opacity);

	std::vector<GradientStop> _colors;
	std::vector<GradientStop> _opacities;
	std::optional<Handle> _over;
	std::optional<Handle> _pressed;
	QPoint _pressPosition;
	bool _dragging = false;
	rpl::event_stream<Move> _moves;
	rpl::event_stream<int> _colorClicks;

};

GradientBar::GradientBar(QWidget *parent)
: RpWidget(parent) {
	setMouseTracking(true);
	resize(width(), wantedHeight());
}

int GradientBar::wantedHeight() const {
	return barRect().y() + Scaled(kGradientBar) + Scaled(kGradientSide);
}

void GradientBar::setStops(
		std::vector<GradientStop> colors,
		std::vector<GradientStop> opacities) {
	if (busy()) {
		return;
	}
	_colors = std::move(colors);
	_opacities = std::move(opacities);
	update();
}

bool GradientBar::busy() const {
	return _dragging;
}

rpl::producer<GradientBar::Move> GradientBar::moves() const {
	return _moves.events();
}

rpl::producer<int> GradientBar::colorClicks() const {
	return _colorClicks.events();
}

std::vector<GradientStop> &GradientBar::stops(bool opacity) {
	return opacity ? _opacities : _colors;
}

QRect GradientBar::barRect() const {
	const auto side = Scaled(kGradientHandle) / 2 + Scaled(2);
	return QRect(
		side,
		(_opacities.empty()
			? Scaled(kGradientCompactTop)
			: Scaled(kGradientSide)),
		std::max(width() - 2 * side, 1),
		Scaled(kGradientBar));
}

QRectF GradientBar::handleRect(bool opacity, double offset) const {
	const auto bar = barRect();
	const auto size = Scaled(kGradientHandle);
	const auto pointer = Scaled(kGradientPointer);
	const auto x = bar.x() + std::clamp(offset, 0., 1.) * bar.width();
	return QRectF(
		x - size / 2.,
		opacity
			? (bar.y() - pointer - size)
			: (bar.y() + bar.height() + pointer),
		size,
		size);
}

double GradientBar::offsetAt(int x) const {
	const auto bar = barRect();
	return std::clamp((x - bar.x()) / double(bar.width()), 0., 1.);
}

std::optional<GradientBar::Handle> GradientBar::handleAt(
		QPoint position) const {
	const auto extra = Scaled(3);
	for (const auto opacity : { false, true }) {
		const auto &list = opacity ? _opacities : _colors;
		for (auto i = int(list.size()); i != 0;) {
			--i;
			const auto rect = handleRect(opacity, list[i].offset).marginsAdded(
				QMarginsF(extra, extra, extra, extra));
			if (rect.contains(position)) {
				return Handle{ opacity, i };
			}
		}
	}
	return std::nullopt;
}

void GradientBar::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto bar = QRectF(barRect());
	const auto radius = Scaled(4);

	auto clip = QPainterPath();
	clip.addRoundedRect(bar, radius, radius);
	if (!_opacities.empty()) {
		// Transparency pattern, part of the image, not of the UI.
		p.save();
		p.setClipPath(clip);
		const auto cell = Scaled(4);
		p.fillRect(bar, QColor(255, 255, 255));
		for (auto y = 0; y * cell < bar.height(); ++y) {
			for (auto x = (y % 2); x * cell < bar.width(); x += 2) {
				p.fillRect(
					QRectF(bar.x() + x * cell, bar.y() + y * cell, cell, cell),
					QColor(204, 204, 204));
			}
		}
		p.restore();
	}
	const auto sorted = [](std::vector<GradientStop> list) {
		ranges::stable_sort(list, ranges::less(), &GradientStop::offset);
		return list;
	};
	const auto colors = sorted(_colors);
	const auto opacities = sorted(_opacities);
	const auto colorAt = [&](double offset) {
		if (colors.empty()) {
			return QColor(0, 0, 0);
		} else if (offset <= colors.front().offset) {
			return colors.front().color;
		}
		for (auto i = 1; i != int(colors.size()); ++i) {
			const auto &a = colors[i - 1];
			const auto &b = colors[i];
			if (offset <= b.offset) {
				const auto span = b.offset - a.offset;
				const auto t = (span > 0.) ? (offset - a.offset) / span : 1.;
				return anim::color(a.color, b.color, t);
			}
		}
		return colors.back().color;
	};
	const auto alphaAt = [&](double offset) -> double {
		if (opacities.empty()) {
			return 1.;
		} else if (offset <= opacities.front().offset) {
			return opacities.front().color.alphaF();
		}
		for (auto i = 1; i != int(opacities.size()); ++i) {
			const auto &a = opacities[i - 1];
			const auto &b = opacities[i];
			if (offset <= b.offset) {
				const auto span = b.offset - a.offset;
				const auto t = (span > 0.) ? (offset - a.offset) / span : 1.;
				return a.color.alphaF() * (1. - t) + b.color.alphaF() * t;
			}
		}
		return opacities.back().color.alphaF();
	};
	auto offsets = std::vector<double>{ 0., 1. };
	for (const auto &stop : colors) {
		offsets.push_back(std::clamp(stop.offset, 0., 1.));
	}
	for (const auto &stop : opacities) {
		offsets.push_back(std::clamp(stop.offset, 0., 1.));
	}
	ranges::sort(offsets);
	offsets.erase(ranges::unique(offsets), end(offsets));
	auto gradient = QLinearGradient(bar.left(), 0., bar.right(), 0.);
	for (const auto offset : offsets) {
		auto color = colorAt(offset);
		color.setAlphaF(float(std::clamp(alphaAt(offset), 0., 1.)));
		gradient.setColorAt(offset, color);
	}
	auto border = QPen(anim::with_alpha(st::windowFg->c, 0.16));
	border.setWidthF(st::lineWidth);
	p.setPen(border);
	p.setBrush(gradient);
	p.drawRoundedRect(bar, radius, radius);

	const auto paintHandle = [&](bool opacity, int index) {
		const auto &list = opacity ? _opacities : _colors;
		const auto &stop = list[index];
		const auto rect = handleRect(opacity, stop.offset);
		const auto handle = Handle{ opacity, index };
		const auto active = (_pressed && *_pressed == handle)
			|| (_over && *_over == handle);
		auto fill = stop.color;
		if (opacity) {
			const auto level = 1.
				- std::clamp(double(stop.color.alphaF()), 0., 1.);
			fill = QColor::fromRgbF(level, level, level);
		}
		auto pen = QPen(active
			? st::activeLineFg->c
			: anim::with_alpha(st::windowFg->c, 0.45));
		pen.setWidthF(active ? Scaled(2) : Scaled(15) / 10.);
		p.setPen(pen);
		p.setBrush(fill);
		const auto x = rect.center().x();
		const auto pointer = Scaled(kGradientPointer);
		auto path = QPainterPath();
		if (opacity) {
			path.moveTo(rect.left(), rect.top());
			path.lineTo(rect.right(), rect.top());
			path.lineTo(rect.right(), rect.bottom());
			path.lineTo(x + pointer, rect.bottom());
			path.lineTo(x, rect.bottom() + pointer);
			path.lineTo(x - pointer, rect.bottom());
			path.lineTo(rect.left(), rect.bottom());
		} else {
			path.moveTo(rect.left(), rect.bottom());
			path.lineTo(rect.left(), rect.top());
			path.lineTo(x - pointer, rect.top());
			path.lineTo(x, rect.top() - pointer);
			path.lineTo(x + pointer, rect.top());
			path.lineTo(rect.right(), rect.top());
			path.lineTo(rect.right(), rect.bottom());
		}
		path.closeSubpath();
		p.drawPath(path);
	};
	for (auto i = 0; i != int(_opacities.size()); ++i) {
		paintHandle(true, i);
	}
	for (auto i = 0; i != int(_colors.size()); ++i) {
		paintHandle(false, i);
	}
}

void GradientBar::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	_pressed = handleAt(e->pos());
	_pressPosition = e->pos();
	update();
}

void GradientBar::mouseMoveEvent(QMouseEvent *e) {
	if (!_pressed) {
		const auto over = handleAt(e->pos());
		if (_over != over) {
			_over = over;
			setCursor(over ? style::cur_pointer : style::cur_default);
			update();
		}
		return;
	}
	if (!_dragging) {
		if (std::abs(e->pos().x() - _pressPosition.x())
			< Scaled(kDragThreshold)) {
			return;
		}
		_dragging = true;
		setCursor(style::cur_sizehor);
	}
	auto &list = stops(_pressed->opacity);
	if (_pressed->index >= int(list.size())) {
		return;
	}
	const auto offset = offsetAt(e->pos().x());
	if (list[_pressed->index].offset != offset) {
		list[_pressed->index].offset = offset;
		update();
		_moves.fire({
			.opacity = _pressed->opacity,
			.index = _pressed->index,
			.offset = offset,
		});
	}
}

void GradientBar::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton || !_pressed) {
		return;
	}
	const auto pressed = *base::take(_pressed);
	if (_dragging) {
		_dragging = false;
		const auto &list = stops(pressed.opacity);
		if (pressed.index < int(list.size())) {
			_moves.fire({
				.opacity = pressed.opacity,
				.index = pressed.index,
				.offset = list[pressed.index].offset,
				.finished = true,
			});
		}
	} else if (!pressed.opacity && handleAt(e->pos()) == pressed) {
		_colorClicks.fire_copy(pressed.index);
	}
	_over = handleAt(e->pos());
	setCursor(_over ? style::cur_pointer : style::cur_default);
	update();
}

void GradientBar::leaveEventHook(QEvent *e) {
	if (_over && !_pressed) {
		_over = std::nullopt;
		update();
	}
}

// Keyframe controls: previous / add-remove / next.

class KeyframeControl final
	: public Ui::RpWidget
	, public Ui::AbstractTooltipShower {
public:
	KeyframeControl(
		QWidget *parent,
		not_null<EditorController*> controller,
		PropertyRef ref,
		std::vector<double> defaults);

	void refresh();
	void showMenu(QPoint globalPosition);

	QString tooltipText() const override;
	QPoint tooltipPos() const override;
	bool tooltipWindowActive() const override;

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	enum class Part : uchar {
		None,
		Previous,
		Toggle,
		Next,
	};

	[[nodiscard]] Part partAt(QPoint position) const;
	[[nodiscard]] QRect partRect(Part part) const;
	[[nodiscard]] std::optional<double> keyAtPlayhead() const;
	void setOver(Part part);
	void toggle();
	void jump(bool next);

	const not_null<EditorController*> _controller;
	const PropertyRef _ref;
	const std::vector<double> _defaults;
	bool _animated = false;
	bool _atPlayhead = false;
	bool _hasPrevious = false;
	bool _hasNext = false;
	Part _over = Part::None;
	Part _pressed = Part::None;
	base::unique_qptr<Ui::PopupMenu> _menu;

};

KeyframeControl::KeyframeControl(
	QWidget *parent,
	not_null<EditorController*> controller,
	PropertyRef ref,
	std::vector<double> defaults)
: RpWidget(parent)
, _controller(controller)
, _ref(std::move(ref))
, _defaults(std::move(defaults)) {
	setMouseTracking(true);
}

void KeyframeControl::refresh() {
	const auto &document = _controller->document();
	_animated = document.animated(_ref);
	_atPlayhead = _hasPrevious = _hasNext = false;
	if (_animated) {
		const auto local = _controller->localFrame(_ref.node);
		for (const auto time : document.keyframeTimes(_ref)) {
			if (std::abs(time - local) < kKeyframeEpsilon) {
				_atPlayhead = true;
			} else if (time < local) {
				_hasPrevious = true;
			} else {
				_hasNext = true;
			}
		}
	}
	update();
}

std::optional<double> KeyframeControl::keyAtPlayhead() const {
	const auto local = _controller->localFrame(_ref.node);
	for (const auto time : _controller->document().keyframeTimes(_ref)) {
		if (std::abs(time - local) < kKeyframeEpsilon) {
			return time;
		}
	}
	return std::nullopt;
}

QRect KeyframeControl::partRect(Part part) const {
	const auto third = width() / 3;
	switch (part) {
	case Part::Previous: return QRect(0, 0, third, height());
	case Part::Toggle: return QRect(third, 0, width() - 2 * third, height());
	case Part::Next: return QRect(width() - third, 0, third, height());
	default: return QRect();
	}
}

KeyframeControl::Part KeyframeControl::partAt(QPoint position) const {
	if (!rect().contains(position)) {
		return Part::None;
	}
	for (const auto part : { Part::Previous, Part::Toggle, Part::Next }) {
		if (partRect(part).contains(position)) {
			if (part != Part::Toggle && !_animated) {
				return Part::None;
			}
			return part;
		}
	}
	return Part::None;
}

void KeyframeControl::setOver(Part part) {
	if (_over == part) {
		return;
	}
	_over = part;
	setCursor((part == Part::Toggle
		|| (part == Part::Previous && _hasPrevious)
		|| (part == Part::Next && _hasNext))
		? style::cur_pointer
		: style::cur_default);
	update();
	if (part != Part::None) {
		Ui::Tooltip::Show(kTooltipDelay, this);
	} else {
		Ui::Tooltip::Hide();
	}
}

QString KeyframeControl::tooltipText() const {
	switch (_over) {
	case Part::Previous:
		return tr::lng_oblivion_lottie_inspector_key_prev(tr::now);
	case Part::Next:
		return tr::lng_oblivion_lottie_inspector_key_next(tr::now);
	case Part::Toggle:
		return _atPlayhead
			? tr::lng_oblivion_lottie_inspector_key_remove(tr::now)
			: _animated
			? tr::lng_oblivion_lottie_inspector_key_add(tr::now)
			: tr::lng_oblivion_lottie_inspector_animate(tr::now);
	default:
		return QString();
	}
}

QPoint KeyframeControl::tooltipPos() const {
	return QCursor::pos();
}

bool KeyframeControl::tooltipWindowActive() const {
	return Ui::AppInFocus() && Ui::InFocusChain(window());
}

void KeyframeControl::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto accent = st::windowActiveTextFg->c;
	const auto sub = st::windowSubTextFg->c;

	// Diamond.
	{
		const auto rect = partRect(Part::Toggle);
		const auto center = QPointF(rect.center()) + QPointF(0.5, 0.5);
		const auto size = Scaled(5) + 0.5;
		auto path = QPainterPath();
		path.moveTo(center.x(), center.y() - size);
		path.lineTo(center.x() + size, center.y());
		path.lineTo(center.x(), center.y() + size);
		path.lineTo(center.x() - size, center.y());
		path.closeSubpath();
		const auto over = (_over == Part::Toggle);
		auto color = _animated ? accent : sub;
		if (!_animated && !over) {
			color = anim::with_alpha(color, 0.65);
		}
		auto pen = QPen(color);
		pen.setWidthF(Scaled(14) / 10.);
		pen.setJoinStyle(Qt::MiterJoin);
		p.setPen(pen);
		p.setBrush(_atPlayhead
			? QBrush(color)
			: over
			? QBrush(anim::with_alpha(color, 0.25))
			: QBrush(Qt::NoBrush));
		p.drawPath(path);
	}
	if (!_animated) {
		return;
	}

	// Arrows.
	const auto paintArrow = [&](Part part, bool enabled, bool left) {
		const auto rect = partRect(part);
		const auto center = QPointF(rect.center()) + QPointF(0.5, 0.5);
		const auto size = Scaled(3) + 0.5;
		auto path = QPainterPath();
		if (left) {
			path.moveTo(center.x() + size / 2., center.y() - size);
			path.lineTo(center.x() - size / 2., center.y());
			path.lineTo(center.x() + size / 2., center.y() + size);
		} else {
			path.moveTo(center.x() - size / 2., center.y() - size);
			path.lineTo(center.x() + size / 2., center.y());
			path.lineTo(center.x() - size / 2., center.y() + size);
		}
		auto color = enabled
			? ((_over == part) ? accent : sub)
			: anim::with_alpha(sub, 0.3);
		auto pen = QPen(color);
		pen.setWidthF(Scaled(15) / 10.);
		pen.setCapStyle(Qt::RoundCap);
		pen.setJoinStyle(Qt::RoundJoin);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawPath(path);
	};
	paintArrow(Part::Previous, _hasPrevious, true);
	paintArrow(Part::Next, _hasNext, false);
}

void KeyframeControl::mouseMoveEvent(QMouseEvent *e) {
	setOver(partAt(e->pos()));
}

void KeyframeControl::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = partAt(e->pos());
	}
}

void KeyframeControl::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto pressed = std::exchange(_pressed, Part::None);
	if (pressed == Part::None || pressed != partAt(e->pos())) {
		return;
	}
	switch (pressed) {
	case Part::Toggle: toggle(); break;
	case Part::Previous: jump(false); break;
	case Part::Next: jump(true); break;
	default: break;
	}
}

void KeyframeControl::contextMenuEvent(QContextMenuEvent *e) {
	showMenu(e->globalPos());
}

void KeyframeControl::leaveEventHook(QEvent *e) {
	setOver(Part::None);
}

void KeyframeControl::toggle() {
	const auto &document = _controller->document();
	const auto local = _controller->localFrame(_ref.node);
	if (const auto time = keyAtPlayhead()) {
		_controller->removeKeyframes({ KeyframeRef{ _ref, *time } });
	} else if (document.property(_ref)) {
		_controller->addKeyframe(_ref);
	} else if (!_defaults.empty()) {
		// A transform value that is not stored yet: create it first.
		auto first = SetValueAt(
			document,
			_ref,
			PropValue{ .numbers = _defaults },
			local);
		if (!first) {
			return;
		}
		auto second = AddKeyframe(first.document, _ref, local);
		if (!second) {
			return;
		}
		for (const auto id : first.changed) {
			if (!ranges::contains(second.changed, id)) {
				second.changed.push_back(id);
			}
		}
		_controller->perform(Command::AddKeyframe, std::move(second));
	}
	_controller->setActiveProperty(_ref);
}

void KeyframeControl::jump(bool next) {
	const auto &document = _controller->document();
	auto times = document.keyframeTimes(_ref);
	ranges::sort(times);
	const auto local = _controller->localFrame(_ref.node);
	auto target = std::optional<double>();
	if (next) {
		for (const auto time : times) {
			if (time > local + kKeyframeEpsilon) {
				target = time;
				break;
			}
		}
	} else {
		for (auto i = times.rbegin(); i != times.rend(); ++i) {
			if (*i < local - kKeyframeEpsilon) {
				target = *i;
				break;
			}
		}
	}
	if (!target) {
		return;
	}
	_controller->setPlaying(false);
	_controller->setActiveProperty(_ref);
	const auto current = _controller->currentFrame();
	if (next) {
		for (auto frame = current + 1
			; frame <= _controller->lastFrame()
			; ++frame) {
			if (document.localFrame(_ref.node, frame)
				>= *target - kKeyframeEpsilon) {
				_controller->setCurrentFrame(frame);
				return;
			}
		}
	} else {
		for (auto frame = current - 1
			; frame >= _controller->firstFrame()
			; --frame) {
			if (document.localFrame(_ref.node, frame)
				<= *target + kKeyframeEpsilon) {
				_controller->setCurrentFrame(frame);
				return;
			}
		}
	}
}

void KeyframeControl::showMenu(QPoint globalPosition) {
	const auto &document = _controller->document();
	const auto exists = document.property(_ref).has_value();
	if (!exists && _defaults.empty()) {
		return;
	}
	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuWithIcons);
	if (!_animated) {
		_menu->addAction(
			tr::lng_oblivion_lottie_inspector_animate(tr::now),
			[=] { toggle(); },
			&st::menuIconAdd);
	} else {
		const auto atPlayhead = keyAtPlayhead().has_value();
		_menu->addAction(
			(atPlayhead
				? tr::lng_oblivion_lottie_inspector_key_remove(tr::now)
				: tr::lng_oblivion_lottie_inspector_key_add(tr::now)),
			[=] { toggle(); },
			atPlayhead ? &st::menuIconRemove : &st::menuIconAdd);
		if (_hasPrevious) {
			_menu->addAction(
				tr::lng_oblivion_lottie_inspector_key_prev(tr::now),
				[=] { jump(false); },
				nullptr);
		}
		if (_hasNext) {
			_menu->addAction(
				tr::lng_oblivion_lottie_inspector_key_next(tr::now),
				[=] { jump(true); },
				nullptr);
		}
		_menu->addSeparator();
		_menu->addAction(
			tr::lng_oblivion_lottie_inspector_unanimate(tr::now),
			[=] { _controller->setAnimated(_ref, false); },
			&st::menuIconDelete);
	}
	_menu->popup(globalPosition);
}

// Rows.

enum class SpecKind : uchar {
	Header,
	Info,
	Section,
	Property,
	Choice,
	Number,
	Check,
	Hint,
};

enum class NumberKind : uchar {
	InPoint,
	OutPoint,
	StartTime,
	Stretch,
	Width,
	Height,
	FrameRate,
	Duration,
	Member,
};

enum class CheckKind : uchar {
	ScaleContent,
	KeepDuration,
};

struct ChoiceOption {
	double value = 0.;
	QString text;
};

struct RowSpec {
	SpecKind kind = SpecKind::Section;
	QString text;
	NodeId node = 0;
	int count = 0; // Header: selected nodes.

	// Property.
	PropertyRef ref;
	PropertyType type = PropertyType::Scalar;
	PropertyRole role = PropertyRole::Other;
	int dimensions = 1;
	int colorStops = 0;
	bool animatable = true;
	std::vector<double> defaults; // Not stored transform values.

	// Choice, member number.
	QByteArray member;
	std::vector<ChoiceOption> options;
	double fallback = 0.;

	NumberKind number = NumberKind::InPoint;
	CheckKind check = CheckKind::ScaleContent;

	// Header: secondary lines (parent, track matte...).
	QStringList info;
	// Hint: starts at the property labels (explains the row above).
	bool indented = false;
};

[[nodiscard]] QByteArray Signature(const std::vector<RowSpec> &specs) {
	auto result = QByteArray();
	for (const auto &spec : specs) {
		result += QByteArray::number(int(spec.kind))
			+ ':' + QByteArray::number(spec.node)
			+ ':' + QByteArray::number(spec.count)
			+ ':' + QByteArray::number(spec.ref.node)
			+ ':' + spec.ref.path
			+ ':' + QByteArray::number(int(spec.type))
			+ ':' + QByteArray::number(spec.dimensions)
			+ ':' + QByteArray::number(spec.colorStops)
			+ ':' + spec.member
			+ ':' + QByteArray::number(int(spec.number))
			+ ':' + QByteArray::number(int(spec.check))
			+ ':' + spec.text.toUtf8()
			+ ':' + spec.info.join(QChar('\n')).toUtf8()
			+ ':' + (spec.indented ? '1' : '0')
			+ ';';
	}
	return result;
}

class InspectorRow : public Ui::RpWidget {
public:
	using RpWidget::RpWidget;

	virtual void refresh() {
	}
	virtual void commitEditing() {
	}

};

class HeaderRow final : public InspectorRow {
public:
	HeaderRow(
		QWidget *parent,
		not_null<EditorController*> controller,
		NodeId id,
		int count,
		QStringList info);

	void refresh() override;

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;

private:
	const not_null<EditorController*> _controller;
	const NodeId _id = 0;
	const int _count = 0;
	const QStringList _info;
	QString _name;
	QString _type;
	QString _extra;

};

HeaderRow::HeaderRow(
	QWidget *parent,
	not_null<EditorController*> controller,
	NodeId id,
	int count,
	QStringList info)
: InspectorRow(parent)
, _controller(controller)
, _id(id)
, _count(count)
, _info(std::move(info)) {
	refresh();
}

void HeaderRow::refresh() {
	const auto &document = _controller->document();
	const auto node = _id ? document.node(_id) : nullptr;
	if (node) {
		_name = NodeDisplayName(document, _id);
		_type = NodeTypeText(*node);
	} else {
		_name = NodeKindText(NodeKind::Composition);
		_type = DocumentInfoText(document);
	}
	_extra = (_count > 1)
		? tr::lng_oblivion_lottie_editor_selected(
			tr::now,
			lt_value,
			QString::number(_count))
		: QString();
	update();
}

int HeaderRow::resizeGetHeight(int newWidth) {
	return Scaled(10)
		+ st::semiboldFont->height
		+ Scaled(3)
		+ st::normalFont->height
		+ (_count > 1 ? st::normalFont->height : 0)
		+ int(_info.size()) * st::normalFont->height
		+ Scaled(8);
}

void HeaderRow::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto padding = Scaled(kPadding);
	const auto inner = std::max(width() - 2 * padding, 0);
	auto top = Scaled(10);
	p.setFont(st::semiboldFont);
	p.setPen(st::windowBoldFg);
	p.drawText(
		padding,
		top + st::semiboldFont->ascent,
		st::semiboldFont->elided(_name, inner));
	top += st::semiboldFont->height + Scaled(3);
	p.setFont(st::normalFont);
	p.setPen(st::windowSubTextFg);
	p.drawText(
		padding,
		top + st::normalFont->ascent,
		st::normalFont->elided(_type, inner));
	if (!_extra.isEmpty()) {
		top += st::normalFont->height;
		p.setPen(st::windowActiveTextFg);
		p.drawText(
			padding,
			top + st::normalFont->ascent,
			st::normalFont->elided(_extra, inner));
	}
	p.setPen(st::windowSubTextFg);
	for (const auto &line : _info) {
		top += st::normalFont->height;
		p.drawText(
			padding,
			top + st::normalFont->ascent,
			st::normalFont->elided(line, inner));
	}
}

class SectionRow final : public InspectorRow {
public:
	SectionRow(QWidget *parent, QString title);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;

private:
	const QString _title;

};

SectionRow::SectionRow(QWidget *parent, QString title)
: InspectorRow(parent)
, _title(std::move(title)) {
}

int SectionRow::resizeGetHeight(int newWidth) {
	return Scaled(12) + st::semiboldFont->height + Scaled(6);
}

void SectionRow::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto padding = Scaled(kPadding);
	p.fillRect(
		padding,
		Scaled(4),
		std::max(width() - 2 * padding, 0),
		st::lineWidth,
		st::shadowFg);
	p.setFont(st::semiboldFont);
	p.setPen(st::windowBoldFg);
	p.drawText(
		padding,
		Scaled(12) + st::semiboldFont->ascent,
		st::semiboldFont->elided(_title, width() - 2 * padding));
}

// indented: a hint for the property row above it, starts at the labels.
class InfoRow final : public InspectorRow {
public:
	InfoRow(QWidget *parent, QString text, bool hint, bool indented);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;

private:
	[[nodiscard]] const style::font &font() const;
	[[nodiscard]] QRect textRect(int width) const;

	const QString _text;
	const bool _hint = false;
	const bool _indented = false;

};

InfoRow::InfoRow(QWidget *parent, QString text, bool hint, bool indented)
: InspectorRow(parent)
, _text(BindShortWords(std::move(text)))
, _hint(hint)
, _indented(indented) {
}

const style::font &InfoRow::font() const {
	return _hint ? PanelSmallFont() : st::normalFont;
}

QRect InfoRow::textRect(int width) const {
	const auto padding = Scaled(kPadding);
	const auto left = _indented ? PropertyLabelLeft() : padding;
	const auto top = _indented ? 0 : _hint ? Scaled(6) : Scaled(2);
	return QRect(left, top, std::max(width - left - padding, 1), 1 << 20);
}

int InfoRow::resizeGetHeight(int newWidth) {
	const auto rect = textRect(newWidth);
	const auto height = QFontMetrics(font()->f).boundingRect(
		QRect(0, 0, rect.width(), 1 << 20),
		Qt::TextWordWrap,
		_text).height();
	return rect.y() + height + (_hint ? Scaled(_indented ? 8 : 4) : Scaled(2));
}

void InfoRow::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.setFont(font());
	p.setPen(st::windowSubTextFg);
	const auto rect = textRect(width());
	p.drawText(
		QRect(rect.x(), rect.y(), rect.width(), height() - rect.y()),
		Qt::TextWordWrap,
		_text);
}

class CheckRow final : public InspectorRow {
public:
	CheckRow(QWidget *parent, QString text, bool *value);

protected:
	int resizeGetHeight(int newWidth) override;

private:
	const not_null<Ui::Checkbox*> _checkbox;

};

CheckRow::CheckRow(QWidget *parent, QString text, bool *value)
: InspectorRow(parent)
, _checkbox(Ui::CreateChild<Ui::Checkbox>(
	this,
	BindShortWords(text),
	*value,
	st::defaultCheckbox)) {
	_checkbox->setAllowTextLines(0);
	_checkbox->checkedChanges() | rpl::on_next([=](bool checked) {
		*value = checked;
	}, _checkbox->lifetime());
}

int CheckRow::resizeGetHeight(int newWidth) {
	const auto padding = Scaled(kPadding);
	_checkbox->resizeToWidth(std::max(newWidth - 2 * padding, 1));
	_checkbox->moveToLeft(padding, Scaled(4), newWidth);
	return _checkbox->heightNoMargins() + Scaled(8);
}

// Label on the left, the editor on the right (or below when narrow).
// keyframes: the keyframe controls column is kept on the left (filled by
// PropertyRow, empty in other rows so that the labels line up).

class LabeledRow : public InspectorRow, public Ui::AbstractTooltipShower {
public:
	LabeledRow(QWidget *parent, QString label, bool keyframes);

	QString tooltipText() const override;
	QPoint tooltipPos() const override;
	bool tooltipWindowActive() const override;

protected:
	// The editor width wanted on the label line, 0 for a second line.
	[[nodiscard]] virtual int editorWidth() const = 0;
	// A narrower one for when the label doesn't fit next to editorWidth()
	// (the editor then doesn't line up with the others, the label is whole).
	[[nodiscard]] virtual int compactEditorWidth() const {
		return editorWidth();
	}
	// Places the editor in the area, returns the height it took.
	virtual int layoutEditor(QRect area, bool secondLine) = 0;
	[[nodiscard]] virtual QString extraTooltip() const {
		return QString();
	}
	virtual void labelClicked() {
	}
	[[nodiscard]] virtual bool highlighted() const {
		return false;
	}
	[[nodiscard]] virtual bool attention() const {
		return false;
	}

	[[nodiscard]] int keyframesLeft() const;
	[[nodiscard]] QString label() const {
		return _label;
	}

	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	const QString _label;
	const bool _keyframes = false;
	QRect _labelRect;
	bool _overLabel = false;

};

LabeledRow::LabeledRow(QWidget *parent, QString label, bool keyframes)
: InspectorRow(parent)
, _label(std::move(label))
, _keyframes(keyframes) {
	setMouseTracking(true);
}

int LabeledRow::keyframesLeft() const {
	return Scaled(kPadding) / 2;
}

int LabeledRow::resizeGetHeight(int newWidth) {
	const auto padding = Scaled(kPadding);
	const auto rowHeight = Scaled(kRowHeight);
	const auto labelLeft = _keyframes ? PropertyLabelLeft() : padding;
	const auto right = newWidth - padding;
	const auto available = std::max(right - labelLeft, 0);
	const auto labelWidth = st::normalFont->width(_label);
	auto wanted = editorWidth();
	if (wanted > 0 && available - wanted - Scaled(8) < labelWidth) {
		wanted = std::min(wanted, compactEditorWidth());
	}
	const auto secondLine = (wanted <= 0)
		|| (available - wanted - Scaled(8)
			< std::min(labelWidth, Scaled(kLabelMin)));
	if (!secondLine) {
		const auto fieldHeight = Scaled(kFieldHeight);
		layoutEditor(
			QRect(
				right - wanted,
				(rowHeight - fieldHeight) / 2,
				wanted,
				fieldHeight),
			false);
		_labelRect = QRect(
			labelLeft,
			0,
			std::max(available - wanted - Scaled(8), 0),
			rowHeight);
		return rowHeight;
	}
	_labelRect = QRect(labelLeft, 0, available, rowHeight);
	const auto top = rowHeight - Scaled(6);
	const auto taken = layoutEditor(
		QRect(labelLeft, top, available, Scaled(kFieldHeight)),
		true);
	return top + taken + Scaled(6);
}

void LabeledRow::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	if (highlighted()) {
		// The active property (shown in the timeline): an accent tint and
		// a bar, fields keep their own background visible.
		const auto accent = st::windowActiveTextFg->c;
		p.fillRect(rect(), anim::with_alpha(accent, 0.07));
		p.fillRect(0, 0, Scaled(2), height(), accent);
	}
	p.setFont(st::normalFont);
	p.setPen(attention()
		? st::attentionButtonFg
		: highlighted()
		? st::windowActiveTextFg
		: st::windowFg);
	p.drawText(
		_labelRect,
		Qt::AlignLeft | Qt::AlignVCenter,
		st::normalFont->elided(_label, _labelRect.width()));
}

void LabeledRow::mouseMoveEvent(QMouseEvent *e) {
	const auto over = _labelRect.contains(e->pos());
	if (_overLabel != over) {
		_overLabel = over;
		const auto elided = st::normalFont->width(_label)
			> _labelRect.width();
		if (over && (elided || !extraTooltip().isEmpty())) {
			Ui::Tooltip::Show(kTooltipDelay, this);
		} else {
			Ui::Tooltip::Hide();
		}
	}
}

void LabeledRow::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton && _labelRect.contains(e->pos())) {
		labelClicked();
	}
}

void LabeledRow::leaveEventHook(QEvent *e) {
	if (_overLabel) {
		_overLabel = false;
		Ui::Tooltip::Hide();
	}
}

QString LabeledRow::tooltipText() const {
	const auto extra = extraTooltip();
	return extra.isEmpty() ? _label : (_label + '\n' + extra);
}

QPoint LabeledRow::tooltipPos() const {
	return QCursor::pos();
}

bool LabeledRow::tooltipWindowActive() const {
	return Ui::AppInFocus() && Ui::InFocusChain(window());
}

// An animatable property at the current frame.

class PropertyRow final : public LabeledRow {
public:
	PropertyRow(
		QWidget *parent,
		not_null<EditorController*> controller,
		const RowSpec &spec);

	void refresh() override;
	void commitEditing() override;

protected:
	int editorWidth() const override;
	int compactEditorWidth() const override;
	int layoutEditor(QRect area, bool secondLine) override;
	QString extraTooltip() const override;
	void labelClicked() override;
	bool highlighted() const override;
	bool attention() const override;
	void paintEvent(QPaintEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;

private:
	[[nodiscard]] std::optional<PropValue> current() const;
	[[nodiscard]] std::vector<double> numbersIn(
		const Document &document,
		double frame) const;
	[[nodiscard]] int fieldCount() const;
	void setupEditors();
	void startDrag();
	void applyNumber(int index, double value, bool drag);
	void applyHex(const QString &text);
	void pickColor();
	void pickStopColor(int index);
	void moveStop(const GradientBar::Move &move);

	const not_null<EditorController*> _controller;
	const RowSpec _spec;
	KeyframeControl *_keyframes = nullptr;
	std::vector<not_null<ValueField*>> _fields;
	SwatchButton *_swatch = nullptr;
	ValueField *_hex = nullptr;
	GradientBar *_gradient = nullptr;
	QString _valueText;
	QRect _valueRect;
	LiveSession _drag; // One undo step per drag, whatever pauses.
	double _dragFrame = 0.;
	int _colorStops = 0;
	bool _expression = false;
	bool _active = false;

};

PropertyRow::PropertyRow(
	QWidget *parent,
	not_null<EditorController*> controller,
	const RowSpec &spec)
: LabeledRow(parent, spec.text, true)
, _controller(controller)
, _spec(spec)
, _drag(controller, Command::SetValue)
, _colorStops(spec.colorStops) {
	if (_spec.animatable) {
		_keyframes = Ui::CreateChild<KeyframeControl>(
			this,
			controller,
			_spec.ref,
			_spec.defaults);
	}
	setupEditors();
}

int PropertyRow::fieldCount() const {
	switch (_spec.type) {
	case PropertyType::Scalar: return 1;
	case PropertyType::Vector: return std::clamp(_spec.dimensions, 1, 2);
	default: return 0;
	}
}

void PropertyRow::setupEditors() {
	switch (_spec.type) {
	case PropertyType::Scalar:
	case PropertyType::Vector: {
		const auto options = OptionsFor(_spec.role);
		for (auto i = 0; i != fieldCount(); ++i) {
			const auto field = Ui::CreateChild<ValueField>(this, options);
			field->dragged() | rpl::on_next([=](double value) {
				applyNumber(i, value, true);
			}, field->lifetime());
			field->dragFinished() | rpl::on_next([=] {
				_drag.finish();
				refresh();
			}, field->lifetime());
			field->numberSubmitted() | rpl::on_next([=](double value) {
				applyNumber(i, value, false);
			}, field->lifetime());
			_fields.push_back(field);
		}
	} break;
	case PropertyType::Color: {
		_swatch = Ui::CreateChild<SwatchButton>(this);
		_swatch->setClickedCallback([=] { pickColor(); });
		_hex = Ui::CreateChild<ValueField>(
			this,
			FieldOptions{ .text = true });
		_hex->textSubmitted() | rpl::on_next([=](QString text) {
			applyHex(text);
		}, _hex->lifetime());
	} break;
	case PropertyType::Gradient: {
		_gradient = Ui::CreateChild<GradientBar>(this);
		_gradient->moves() | rpl::on_next([=](GradientBar::Move move) {
			moveStop(move);
		}, _gradient->lifetime());
		_gradient->colorClicks() | rpl::on_next([=](int index) {
			pickStopColor(index);
		}, _gradient->lifetime());
	} break;
	case PropertyType::Path:
		break;
	}
}

int PropertyRow::editorWidth() const {
	switch (_spec.type) {
	case PropertyType::Scalar:
		return Scaled(kFieldWidth);
	case PropertyType::Vector: {
		const auto count = fieldCount();
		return count * Scaled(kFieldWidth)
			+ (count - 1) * Scaled(kFieldSkip);
	}
	case PropertyType::Color:
		return Scaled(kValueAreaWidth);
	case PropertyType::Gradient:
		return 0;
	case PropertyType::Path:
		return std::max(st::normalFont->width(_valueText), Scaled(40));
	}
	return 0;
}

int PropertyRow::compactEditorWidth() const {
	return (_spec.type == PropertyType::Color)
		? (Scaled(kSwatchSize) + Scaled(kSwatchSkip) + Scaled(kHexWidth))
		: editorWidth();
}

int PropertyRow::layoutEditor(QRect area, bool secondLine) {
	if (_keyframes) {
		_keyframes->setGeometry(
			keyframesLeft(),
			0,
			Scaled(kKeyframesWidth),
			Scaled(kRowHeight));
	}
	const auto fieldHeight = Scaled(kFieldHeight);
	switch (_spec.type) {
	case PropertyType::Scalar:
	case PropertyType::Vector: {
		const auto count = int(_fields.size());
		const auto skip = Scaled(kFieldSkip);
		const auto natural = Scaled(kFieldWidth);
		const auto each = std::min(
			natural * 3 / 2,
			(area.width() - (count - 1) * skip) / std::max(count, 1));
		auto x = area.x() + area.width() - count * each - (count - 1) * skip;
		for (const auto field : _fields) {
			field->setGeometry(x, area.y(), each, fieldHeight);
			x += each + skip;
		}
		return fieldHeight;
	}
	case PropertyType::Color: {
		// The swatch starts the value area, the hex field fills the rest.
		const auto size = Scaled(kSwatchSize);
		const auto width = std::min(area.width(), Scaled(kValueAreaWidth));
		const auto left = area.x() + area.width() - width;
		const auto hexLeft = left + size + Scaled(kSwatchSkip);
		_swatch->setGeometry(
			left,
			area.y() + (fieldHeight - size) / 2,
			size,
			size);
		_hex->setGeometry(
			hexLeft,
			area.y(),
			std::max(area.x() + area.width() - hexLeft, 0),
			fieldHeight);
		return fieldHeight;
	}
	case PropertyType::Gradient: {
		const auto height = _gradient->wantedHeight();
		_gradient->setGeometry(area.x(), area.y(), area.width(), height);
		return height;
	}
	case PropertyType::Path:
		_valueRect = QRect(area.x(), area.y(), area.width(), fieldHeight);
		return fieldHeight;
	}
	return 0;
}

std::optional<PropValue> PropertyRow::current() const {
	const auto &document = _controller->document();
	if (auto value = document.valueAt(
			_spec.ref,
			_controller->localFrame(_spec.ref.node))) {
		return value;
	} else if (!_spec.defaults.empty()) {
		return PropValue{ .numbers = _spec.defaults };
	}
	return std::nullopt;
}

void PropertyRow::refresh() {
	const auto &document = _controller->document();
	const auto info = document.property(_spec.ref);
	const auto animated = info && info->animated;
	_expression = info && info->expression;
	if (info) {
		_colorStops = info->colorStops;
	}
	const auto active = _controller->activeProperty();
	_active = active && (*active == _spec.ref);
	if (_keyframes) {
		_keyframes->refresh();
	}
	const auto value = current();
	switch (_spec.type) {
	case PropertyType::Scalar:
	case PropertyType::Vector:
		for (auto i = 0; i != int(_fields.size()); ++i) {
			const auto number = (value && i < int(value->numbers.size()))
				? value->numbers[i]
				: (i < int(_spec.defaults.size()) ? _spec.defaults[i] : 0.);
			_fields[i]->setNumber(number);
			_fields[i]->setAccent(animated);
		}
		break;
	case PropertyType::Color: {
		const auto color = value ? value->color() : QColor();
		_swatch->setColor(color);
		_hex->setText(color.isValid() ? ColorHex(color) : QString());
		_hex->setAccent(animated);
	} break;
	case PropertyType::Gradient:
		if (value) {
			_gradient->setStops(
				ColorStops(*value, _colorStops),
				OpacityStops(*value, _colorStops));
			if (_gradient->height() != _gradient->wantedHeight()
				&& width() > 0) {
				resizeToWidth(width());
			}
		}
		break;
	case PropertyType::Path: {
		const auto points = (value && value->path)
			? int(value->path->vertices.size())
			: 0;
		const auto text = tr::lng_oblivion_lottie_inspector_path_points(
			tr::now,
			lt_value,
			QString::number(points));
		if (_valueText != text) {
			_valueText = text;
			if (width() > 0) {
				resizeToWidth(width());
			}
		}
	} break;
	}
	update();
}

void PropertyRow::commitEditing() {
	for (const auto field : _fields) {
		field->commitEditing();
	}
	if (_hex) {
		_hex->commitEditing();
	}
}

QString PropertyRow::extraTooltip() const {
	return _expression
		? tr::lng_oblivion_lottie_inspector_expression(tr::now)
		: QString();
}

void PropertyRow::labelClicked() {
	_controller->setActiveProperty(_spec.ref);
}

bool PropertyRow::highlighted() const {
	return _active;
}

bool PropertyRow::attention() const {
	return _expression;
}

void PropertyRow::paintEvent(QPaintEvent *e) {
	LabeledRow::paintEvent(e);
	if (_spec.type == PropertyType::Path && !_valueText.isEmpty()) {
		auto p = QPainter(this);
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			_valueRect,
			Qt::AlignRight | Qt::AlignVCenter,
			st::normalFont->elided(_valueText, _valueRect.width()));
	}
}

void PropertyRow::contextMenuEvent(QContextMenuEvent *e) {
	if (_keyframes) {
		_keyframes->showMenu(e->globalPos());
	}
}

std::vector<double> PropertyRow::numbersIn(
		const Document &document,
		double frame) const {
	if (const auto value = document.valueAt(_spec.ref, frame)) {
		return value->numbers;
	}
	return _spec.defaults;
}

void PropertyRow::startDrag() {
	if (!_drag.active()) {
		_controller->setPlaying(false);
		_controller->setActiveProperty(_spec.ref);
		_drag.begin();
		_dragFrame = _controller->localFrame(_spec.ref.node);
	}
}

void PropertyRow::applyNumber(int index, double value, bool drag) {
	if (drag) {
		// From the document before the drag, so the drag is one edit.
		startDrag();
		const auto &baseline = _drag.baseline();
		auto numbers = numbersIn(baseline, _dragFrame);
		if (int(numbers.size()) <= index) {
			numbers.resize(index + 1, 0.);
		}
		numbers[index] = value;
		_drag.apply(SetValueAt(
			baseline,
			_spec.ref,
			PropValue{ .numbers = std::move(numbers) },
			_dragFrame));
		return;
	}
	auto numbers = numbersIn(
		_controller->document(),
		_controller->localFrame(_spec.ref.node));
	if (int(numbers.size()) <= index) {
		numbers.resize(index + 1, 0.);
	}
	numbers[index] = value;
	_controller->setActiveProperty(_spec.ref);
	_controller->setValue(
		_spec.ref,
		PropValue{ .numbers = std::move(numbers) });
}

void PropertyRow::applyHex(const QString &text) {
	const auto color = ParseColorHex(text);
	if (!color.isValid()) {
		refresh();
		return;
	}
	_controller->setValue(_spec.ref, ColorValue(color));
}

void PropertyRow::pickColor() {
	const auto value = current();
	const auto color = value ? value->color() : QColor(255, 255, 255);
	_controller->setActiveProperty(_spec.ref);
	EditColorWithPicker(
		_controller,
		_spec.ref,
		label(),
		color,
		[](QColor color) { return ColorValue(color); });
}

void PropertyRow::pickStopColor(int index) {
	const auto value = current();
	if (!value) {
		return;
	}
	const auto numbers = value->numbers;
	const auto base = index * 4;
	if (index >= _colorStops || base + 3 >= int(numbers.size())) {
		return;
	}
	_controller->setActiveProperty(_spec.ref);
	EditColorWithPicker(
		_controller,
		_spec.ref,
		label(),
		NumbersColor(numbers, base + 1),
		[=](QColor color) {
			auto result = numbers;
			result[base + 1] = color.redF();
			result[base + 2] = color.greenF();
			result[base + 3] = color.blueF();
			return PropValue{ .numbers = std::move(result) };
		});
}

void PropertyRow::moveStop(const GradientBar::Move &move) {
	startDrag();
	const auto &baseline = _drag.baseline();
	auto numbers = numbersIn(baseline, _dragFrame);
	const auto index = move.opacity
		? (_colorStops * 4 + move.index * 2)
		: (move.index * 4);
	if (index >= 0 && index < int(numbers.size())) {
		numbers[index] = std::clamp(move.offset, 0., 1.);
		if (move.finished) {
			SortGradientStops(numbers, _colorStops);
		}
		_drag.apply(SetValueAt(
			baseline,
			_spec.ref,
			PropValue{ .numbers = std::move(numbers) },
			_dragFrame));
	}
	if (move.finished) {
		_drag.finish();
		refresh();
	}
}

// A plain number: layer timing, composition settings, JSON members.
// Edits are pure operations on a given document, so a drag is computed
// from the document before it (LiveSession) and ends up as one step.

class NumberRow final : public LabeledRow {
public:
	struct Descriptor {
		QString label;
		FieldOptions options;
		Command command = Command::SetValue;
		Fn<std::optional<double>(const Document &document)> value;
		Fn<Edit(const Document &document, double value)> edit;
	};

	// aligned: keep the keyframe column (labels line up with the
	// property rows of the same node).
	NumberRow(
		QWidget *parent,
		not_null<EditorController*> controller,
		Descriptor &&descriptor,
		bool aligned);

	void refresh() override;
	void commitEditing() override;

protected:
	int editorWidth() const override;
	int layoutEditor(QRect area, bool secondLine) override;

private:
	void apply(double value);

	const not_null<EditorController*> _controller;
	const Descriptor _descriptor;
	const not_null<ValueField*> _field;
	LiveSession _drag;

};

NumberRow::NumberRow(
	QWidget *parent,
	not_null<EditorController*> controller,
	Descriptor &&descriptor,
	bool aligned)
: LabeledRow(parent, descriptor.label, aligned)
, _controller(controller)
, _descriptor(std::move(descriptor))
, _field(Ui::CreateChild<ValueField>(this, _descriptor.options))
, _drag(controller, _descriptor.command) {
	_field->dragged() | rpl::on_next([=](double value) {
		if (!_descriptor.edit) {
			return;
		} else if (!_drag.active()) {
			_controller->setPlaying(false);
			_drag.begin();
		}
		_drag.apply(_descriptor.edit(_drag.baseline(), value));
	}, _field->lifetime());
	_field->dragFinished() | rpl::on_next([=](double value) {
		if (_descriptor.options.liveDrag) {
			_drag.finish();
		} else {
			apply(value);
		}
		refresh();
	}, _field->lifetime());
	_field->numberSubmitted() | rpl::on_next([=](double value) {
		apply(value);
		refresh();
	}, _field->lifetime());
}

void NumberRow::apply(double value) {
	if (_descriptor.edit) {
		_controller->perform(
			_descriptor.command,
			_descriptor.edit(_controller->document(), value));
	}
}

void NumberRow::refresh() {
	if (!_descriptor.value) {
		return;
	} else if (const auto value = _descriptor.value(_controller->document())) {
		_field->setNumber(*value);
	}
}

void NumberRow::commitEditing() {
	_field->commitEditing();
}

int NumberRow::editorWidth() const {
	return Scaled(kFieldWidth);
}

int NumberRow::layoutEditor(QRect area, bool secondLine) {
	const auto width = std::min(area.width(), Scaled(kFieldWidth) * 3 / 2);
	_field->setGeometry(
		area.x() + area.width() - width,
		area.y(),
		width,
		Scaled(kFieldHeight));
	return Scaled(kFieldHeight);
}

// A plain JSON enum member as a dropdown.

class ChoiceRow final : public LabeledRow {
public:
	ChoiceRow(
		QWidget *parent,
		not_null<EditorController*> controller,
		const RowSpec &spec);

	void refresh() override;

protected:
	int editorWidth() const override;
	int compactEditorWidth() const override;
	int layoutEditor(QRect area, bool secondLine) override;

private:
	const not_null<EditorController*> _controller;
	const RowSpec _spec;
	const not_null<ChoiceButton*> _button;

};

[[nodiscard]] std::vector<QString> OptionTexts(
		const std::vector<ChoiceOption> &options) {
	auto result = std::vector<QString>();
	for (const auto &option : options) {
		result.push_back(option.text);
	}
	return result;
}

ChoiceRow::ChoiceRow(
	QWidget *parent,
	not_null<EditorController*> controller,
	const RowSpec &spec)
: LabeledRow(parent, spec.text, true)
, _controller(controller)
, _spec(spec)
, _button(Ui::CreateChild<ChoiceButton>(this, OptionTexts(spec.options))) {
	_button->chosen() | rpl::on_next([=](int index) {
		if (index < 0 || index >= int(_spec.options.size())) {
			return;
		}
		_controller->perform(Command::SetValue, SetNodeMember(
			_controller->document(),
			_spec.node,
			_spec.member,
			Json::Value::FromNumber(_spec.options[index].value)));
	}, _button->lifetime());
}

void ChoiceRow::refresh() {
	const auto &document = _controller->document();
	if (!document.contains(_spec.node)) {
		return;
	}
	const auto value = document.json(_spec.node)
		.get(_spec.member)
		.toNumber(_spec.fallback);
	auto index = -1;
	auto distance = 0.;
	for (auto i = 0; i != int(_spec.options.size()); ++i) {
		const auto delta = std::abs(_spec.options[i].value - value);
		if (index < 0 || delta < distance) {
			index = i;
			distance = delta;
		}
	}
	_button->setIndex(index);
}

int ChoiceRow::editorWidth() const {
	// At least the value area, so dropdowns line up with the fields.
	return std::max(_button->fullWidth(), Scaled(kValueAreaWidth));
}

int ChoiceRow::compactEditorWidth() const {
	return _button->fullWidth();
}

int ChoiceRow::layoutEditor(QRect area, bool secondLine) {
	// On the label line the area is already the chosen width.
	const auto width = secondLine
		? std::min(area.width(), editorWidth())
		: area.width();
	_button->setGeometry(
		area.x() + area.width() - width,
		area.y(),
		width,
		Scaled(kFieldHeight));
	return Scaled(kFieldHeight);
}

// Specs.

void AppendProperty(
		std::vector<RowSpec> &result,
		const PropertyInfo &info,
		bool animatable = true) {
	result.push_back({
		.kind = SpecKind::Property,
		.text = PropertyText(info),
		.ref = info.ref,
		.type = info.type,
		.role = info.role,
		.dimensions = info.dimensions,
		.colorStops = info.colorStops,
		.animatable = animatable,
	});
}

void AppendSection(std::vector<RowSpec> &result, QString title) {
	result.push_back({
		.kind = SpecKind::Section,
		.text = std::move(title),
	});
}

// Goes to the header lines (same spacing as the type line under the
// name), a separate row only if there is no header.
void AppendInfo(std::vector<RowSpec> &result, QString text) {
	if (!result.empty() && result.front().kind == SpecKind::Header) {
		result.front().info.push_back(std::move(text));
		return;
	}
	result.push_back({
		.kind = SpecKind::Info,
		.text = std::move(text),
	});
}

void AppendChoice(
		std::vector<RowSpec> &result,
		NodeId node,
		QString label,
		QByteArray member,
		std::vector<ChoiceOption> options,
		double fallback) {
	result.push_back({
		.kind = SpecKind::Choice,
		.text = std::move(label),
		.node = node,
		.member = std::move(member),
		.options = std::move(options),
		.fallback = fallback,
	});
}

void AppendNumber(
		std::vector<RowSpec> &result,
		NodeId node,
		NumberKind kind,
		QByteArray member = QByteArray(),
		double fallback = 0.) {
	result.push_back({
		.kind = SpecKind::Number,
		.node = node,
		.member = std::move(member),
		.fallback = fallback,
		.number = kind,
	});
}

void AppendTransform(
		std::vector<RowSpec> &result,
		const Document &document,
		const NodeInfo &node) {
	const auto properties = document.properties(node.id);
	auto used = std::vector<bool>(properties.size(), false);
	const auto transform = [&](const PropertyInfo &info) {
		return (node.kind != NodeKind::Layer)
			|| info.ref.path.startsWith("ks.");
	};
	struct Canonical {
		TransformField field = TransformField::Anchor;
		std::vector<PropertyRole> roles;
		PropertyRole role = PropertyRole::Anchor;
		PropertyType type = PropertyType::Scalar;
		std::vector<double> defaults;
	};
	const auto canonical = std::vector<Canonical>{
		{
			TransformField::Anchor,
			{ PropertyRole::Anchor },
			PropertyRole::Anchor,
			PropertyType::Vector,
			{ 0., 0. },
		},
		{
			TransformField::Position,
			{
				PropertyRole::Position,
				PropertyRole::PositionX,
				PropertyRole::PositionY,
				PropertyRole::PositionZ,
			},
			PropertyRole::Position,
			PropertyType::Vector,
			{ 0., 0. },
		},
		{
			TransformField::Scale,
			{ PropertyRole::Scale },
			PropertyRole::Scale,
			PropertyType::Vector,
			{ 100., 100. },
		},
		{
			TransformField::Rotation,
			{ PropertyRole::Rotation },
			PropertyRole::Rotation,
			PropertyType::Scalar,
			{ 0. },
		},
		{
			TransformField::Opacity,
			{ PropertyRole::Opacity },
			PropertyRole::Opacity,
			PropertyType::Scalar,
			{ 100. },
		},
	};
	for (const auto &entry : canonical) {
		auto found = false;
		for (auto i = 0; i != int(properties.size()); ++i) {
			if (!used[i]
				&& transform(properties[i])
				&& ranges::contains(entry.roles, properties[i].role)) {
				AppendProperty(result, properties[i]);
				used[i] = true;
				found = true;
			}
		}
		if (found) {
			continue;
		}
		const auto ref = TransformProperty(document, node.id, entry.field);
		if (!ref || !document.propertyJson(*ref).isNull()) {
			// Not a transform, or stored in a form we don't show.
			continue;
		}
		result.push_back({
			.kind = SpecKind::Property,
			.text = PropertyRoleText(entry.role),
			.ref = *ref,
			.type = entry.type,
			.role = entry.role,
			.dimensions = int(entry.defaults.size()),
			.defaults = entry.defaults,
		});
	}
	for (auto i = 0; i != int(properties.size()); ++i) {
		if (!used[i] && transform(properties[i])) {
			AppendProperty(result, properties[i]);
		}
	}
}

void AppendStrokeStyle(
		std::vector<RowSpec> &result,
		const Document &document,
		const NodeInfo &node) {
	const auto &json = document.json(node.id);
	AppendChoice(
		result,
		node.id,
		tr::lng_oblivion_lottie_inspector_caps(tr::now),
		"lc",
		{
			{ 1., tr::lng_oblivion_lottie_inspector_cap_butt(tr::now) },
			{ 2., tr::lng_oblivion_lottie_inspector_cap_round(tr::now) },
			{ 3., tr::lng_oblivion_lottie_inspector_cap_square(tr::now) },
		},
		2.);
	AppendChoice(
		result,
		node.id,
		tr::lng_oblivion_lottie_inspector_joins(tr::now),
		"lj",
		{
			{ 1., tr::lng_oblivion_lottie_inspector_join_miter(tr::now) },
			{ 2., tr::lng_oblivion_lottie_inspector_join_round(tr::now) },
			{ 3., tr::lng_oblivion_lottie_inspector_join_bevel(tr::now) },
		},
		2.);
	if (json.get("lj").toInt(2) == 1) {
		AppendNumber(result, node.id, NumberKind::Member, "ml", 4.);
	}
}

void AppendFillRule(std::vector<RowSpec> &result, const NodeInfo &node) {
	AppendChoice(
		result,
		node.id,
		tr::lng_oblivion_lottie_inspector_fill_rule(tr::now),
		"r",
		{
			{ 1., tr::lng_oblivion_lottie_inspector_rule_nonzero(tr::now) },
			{ 2., tr::lng_oblivion_lottie_inspector_rule_evenodd(tr::now) },
		},
		1.);
}

void AppendLayer(
		std::vector<RowSpec> &result,
		const Document &document,
		const NodeInfo &node) {
	if (node.parentLayer) {
		AppendInfo(result, tr::lng_oblivion_lottie_layers_parent(
			tr::now,
			lt_name,
			NodeDisplayName(document, node.parentLayer)));
	}
	if (node.matte != MatteMode::None) {
		AppendInfo(result, MatteModeText(node.matte));
	}
	if (node.matteSource) {
		AppendInfo(
			result,
			tr::lng_oblivion_lottie_inspector_matte_source(tr::now));
	}
	AppendSection(result, tr::lng_oblivion_lottie_inspector_transform(tr::now));
	AppendTransform(result, document, node);

	AppendSection(result, tr::lng_oblivion_lottie_inspector_timing(tr::now));
	AppendNumber(result, node.id, NumberKind::InPoint);
	AppendNumber(result, node.id, NumberKind::OutPoint);
	if (node.layerType == LayerType::Precomp) {
		AppendNumber(result, node.id, NumberKind::StartTime);
		AppendNumber(result, node.id, NumberKind::Stretch);
	}
	const auto properties = document.properties(node.id);
	for (const auto &info : properties) {
		if (info.role == PropertyRole::TimeRemap) {
			AppendProperty(result, info);
		}
	}
	if (node.layerType == LayerType::Solid) {
		AppendSection(
			result,
			tr::lng_oblivion_lottie_inspector_parameters(tr::now));
		for (const auto &info : properties) {
			if (info.ref.path == "sc") {
				AppendProperty(result, info, false);
			}
		}
	}
}

void AppendShape(
		std::vector<RowSpec> &result,
		const Document &document,
		const NodeInfo &node) {
	if (node.shapeType == ShapeType::Group
		|| node.shapeType == ShapeType::Transform) {
		AppendSection(
			result,
			tr::lng_oblivion_lottie_inspector_transform(tr::now));
		AppendTransform(result, document, node);
		return;
	}
	AppendSection(
		result,
		tr::lng_oblivion_lottie_inspector_parameters(tr::now));
	const auto &json = document.json(node.id);
	const auto properties = document.properties(node.id);
	const auto append = [&](Fn<bool(const PropertyInfo&)> filter) {
		for (const auto &info : properties) {
			if (!filter || filter(info)) {
				AppendProperty(result, info);
			}
		}
	};
	switch (node.shapeType) {
	case ShapeType::Fill:
		append(nullptr);
		AppendFillRule(result, node);
		break;
	case ShapeType::Stroke:
		append(nullptr);
		AppendStrokeStyle(result, document, node);
		break;
	case ShapeType::GradientFill:
	case ShapeType::GradientStroke: {
		const auto radial = (json.get("t").toInt(1) == 2);
		AppendChoice(
			result,
			node.id,
			tr::lng_oblivion_lottie_inspector_gradient_type(tr::now),
			"t",
			{
				{
					1.,
					tr::lng_oblivion_lottie_inspector_gradient_linear(tr::now),
				},
				{
					2.,
					tr::lng_oblivion_lottie_inspector_gradient_radial(tr::now),
				},
			},
			1.);
		append([=](const PropertyInfo &info) {
			return radial
				|| (info.role != PropertyRole::HighlightLength
					&& info.role != PropertyRole::HighlightAngle);
		});
		// The hint explains the stops bar, so it goes right under it.
		const auto bar = ranges::find(
			result,
			PropertyType::Gradient,
			&RowSpec::type);
		const auto hint = RowSpec{
			.kind = SpecKind::Hint,
			.text = tr::lng_oblivion_lottie_inspector_gradient_hint(tr::now),
			.indented = (bar != end(result)),
		};
		if (bar != end(result)) {
			result.insert(bar + 1, hint);
		}
		if (node.shapeType == ShapeType::GradientStroke) {
			AppendStrokeStyle(result, document, node);
		} else {
			AppendFillRule(result, node);
		}
		if (!hint.indented) {
			result.push_back(hint);
		}
	} break;
	case ShapeType::TrimPaths:
		append(nullptr);
		AppendChoice(
			result,
			node.id,
			tr::lng_oblivion_lottie_inspector_trim_mode(tr::now),
			"m",
			{
				{
					1.,
					tr::lng_oblivion_lottie_inspector_trim_together(tr::now),
				},
				{
					2.,
					tr::lng_oblivion_lottie_inspector_trim_individually(
						tr::now),
				},
			},
			1.);
		break;
	case ShapeType::Star: {
		const auto polygon = (json.get("sy").toInt(1) == 2);
		AppendChoice(
			result,
			node.id,
			tr::lng_oblivion_lottie_inspector_star_type(tr::now),
			"sy",
			{
				{ 1., ShapeTypeText(ShapeType::Star) },
				{
					2.,
					tr::lng_oblivion_lottie_inspector_star_polygon(tr::now),
				},
			},
			1.);
		append([=](const PropertyInfo &info) {
			return !polygon
				|| (info.role != PropertyRole::InnerRadius
					&& info.role != PropertyRole::InnerRoundness);
		});
	} break;
	default:
		append(nullptr);
		break;
	}
}

// The text names the macOS key for the fine steps (Qt::AltModifier, see
// ValueField::mouseMoveEvent), on the other systems that key is Alt.
[[nodiscard]] QString EditHintText() {
	auto result = tr::lng_oblivion_lottie_inspector_edit_hint(tr::now);
	if (!Platform::IsMac()) {
		result.replace(u"Option"_q, u"Alt"_q);
	}
	return result;
}

void AppendComposition(
		std::vector<RowSpec> &result,
		const Document &document) {
	result.push_back({ .kind = SpecKind::Header });
	AppendSection(result, tr::lng_oblivion_lottie_editor_canvas(tr::now));
	AppendNumber(result, 0, NumberKind::Width);
	AppendNumber(result, 0, NumberKind::Height);
	result.push_back({
		.kind = SpecKind::Check,
		.text = tr::lng_oblivion_lottie_inspector_scale_content(tr::now),
		.check = CheckKind::ScaleContent,
	});
	AppendSection(result, tr::lng_oblivion_lottie_inspector_timing(tr::now));
	AppendNumber(result, 0, NumberKind::FrameRate);
	AppendNumber(result, 0, NumberKind::Duration);
	result.push_back({
		.kind = SpecKind::Check,
		.text = tr::lng_oblivion_lottie_inspector_keep_duration(tr::now),
		.check = CheckKind::KeepDuration,
	});
	result.push_back({
		.kind = SpecKind::Hint,
		.text = tr::lng_oblivion_lottie_editor_nothing_selected(tr::now),
	});
	result.push_back({
		.kind = SpecKind::Hint,
		.text = EditHintText(),
	});
}

[[nodiscard]] std::vector<RowSpec> CollectSpecs(
		not_null<EditorController*> controller) {
	const auto &document = controller->document();
	auto result = std::vector<RowSpec>();
	if (!document.valid()) {
		return result;
	}
	const auto node = document.node(controller->primarySelection());
	if (!node || node->kind == NodeKind::Composition) {
		AppendComposition(result, document);
		return result;
	}
	result.push_back({
		.kind = SpecKind::Header,
		.node = node->id,
		.count = int(controller->selection().size()),
	});
	switch (node->kind) {
	case NodeKind::Layer:
		AppendLayer(result, document, *node);
		break;
	case NodeKind::Shape:
		AppendShape(result, document, *node);
		break;
	default: {
		const auto properties = document.properties(node->id);
		if (!properties.empty()) {
			AppendSection(
				result,
				tr::lng_oblivion_lottie_inspector_parameters(tr::now));
			for (const auto &info : properties) {
				AppendProperty(result, info);
			}
		}
	} break;
	}
	return result;
}

[[nodiscard]] NumberRow::Descriptor NumberDescriptor(const RowSpec &spec) {
	const auto id = spec.node;
	const auto layerValue = [=](double NodeInfo::*field) {
		return [=](const Document &document) -> std::optional<double> {
			const auto node = document.node(id);
			return node ? std::make_optional(node->*field) : std::nullopt;
		};
	};
	const auto frames = FieldOptions{
		.step = 0.25,
		.min = -100000.,
		.max = 100000.,
		.decimals = 0,
		.integer = true,
	};
	const auto setMember = [=](QByteArray key, double min) {
		return [=](const Document &document, double value) {
			return SetNodeMember(
				document,
				id,
				key,
				Json::Value::FromNumber(std::max(value, min)));
		};
	};
	switch (spec.number) {
	case NumberKind::InPoint:
		return {
			.label = tr::lng_oblivion_lottie_inspector_in(tr::now),
			.options = frames,
			.command = Command::LayerTiming,
			.value = layerValue(&NodeInfo::inPoint),
			.edit = [=](const Document &document, double value) {
				const auto node = document.node(id);
				return node
					? SetLayerTiming(
						document,
						id,
						std::min(value, node->outPoint - 1.),
						node->outPoint)
					: Edit();
			},
		};
	case NumberKind::OutPoint:
		return {
			.label = tr::lng_oblivion_lottie_inspector_out(tr::now),
			.options = frames,
			.command = Command::LayerTiming,
			.value = layerValue(&NodeInfo::outPoint),
			.edit = [=](const Document &document, double value) {
				const auto node = document.node(id);
				return node
					? SetLayerTiming(
						document,
						id,
						node->inPoint,
						std::max(value, node->inPoint + 1.))
					: Edit();
			},
		};
	case NumberKind::StartTime:
		return {
			.label = tr::lng_oblivion_lottie_inspector_start(tr::now),
			.options = frames,
			.command = Command::LayerTiming,
			.value = layerValue(&NodeInfo::startTime),
			.edit = setMember("st", -100000.),
		};
	case NumberKind::Stretch:
		return {
			.label = tr::lng_oblivion_lottie_inspector_stretch(tr::now),
			.options = {
				.step = 0.01,
				.min = 0.01,
				.max = 100.,
				.decimals = 2,
			},
			.command = Command::LayerTiming,
			.value = layerValue(&NodeInfo::stretch),
			.edit = setMember("sr", 0.01),
		};
	case NumberKind::Member: {
		const auto member = spec.member;
		const auto fallback = spec.fallback;
		return {
			.label = tr::lng_oblivion_lottie_inspector_miter(tr::now),
			.options = { .step = 0.05, .min = 0., .max = 100., .decimals = 2 },
			.command = Command::SetValue,
			.value = [=](const Document &document) -> std::optional<double> {
				if (!document.contains(id)) {
					return std::nullopt;
				}
				return document.json(id).get(member).toNumber(fallback);
			},
			.edit = setMember(member, 0.),
		};
	}
	case NumberKind::Width:
	case NumberKind::Height: {
		const auto width = (spec.number == NumberKind::Width);
		return {
			.label = (width
				? tr::lng_oblivion_lottie_inspector_width(tr::now)
				: tr::lng_oblivion_lottie_inspector_height(tr::now)),
			.options = {
				.step = 1.,
				.min = 1.,
				.max = 4096.,
				.decimals = 0,
				.integer = true,
				.liveDrag = false,
			},
			.command = Command::CanvasSize,
			.value = [=](const Document &document) -> std::optional<double> {
				const auto size = document.size();
				return double(width ? size.width() : size.height());
			},
			.edit = [=](const Document &document, double value) {
				auto size = document.size();
				if (width) {
					size.setWidth(int(std::round(value)));
				} else {
					size.setHeight(int(std::round(value)));
				}
				return SetCanvasSize(document, size, ScaleContentOption());
			},
		};
	}
	case NumberKind::FrameRate:
		return {
			.label = tr::lng_oblivion_lottie_inspector_fps(tr::now),
			.options = {
				.step = 0.1,
				.min = 1.,
				.max = 120.,
				.decimals = 2,
				.liveDrag = false,
			},
			.command = Command::FrameRate,
			.value = [](const Document &document) -> std::optional<double> {
				return document.frameRate();
			},
			.edit = [](const Document &document, double value) {
				return SetFrameRate(document, value, KeepDurationOption());
			},
		};
	case NumberKind::Duration:
		return {
			.label = tr::lng_oblivion_lottie_inspector_duration(tr::now),
			.options = {
				.step = 0.25,
				.min = 1.,
				.max = 100000.,
				.decimals = 0,
				.integer = true,
				.liveDrag = false,
			},
			.command = Command::Duration,
			.value = [](const Document &document) -> std::optional<double> {
				return double(document.frames());
			},
			.edit = [](const Document &document, double value) {
				return SetDuration(document, int(std::round(value)));
			},
		};
	}
	return {};
}

} // namespace

// The properties tab.

class InspectorPanel::Content final : public Ui::VerticalLayout {
public:
	Content(QWidget *parent, not_null<EditorController*> controller);

	[[nodiscard]] rpl::producer<> scrollToTopRequests() const;

private:
	void scheduleSync();
	void sync();
	void rebuild(std::vector<RowSpec> &&specs);
	void refreshRows();
	[[nodiscard]] object_ptr<InspectorRow> createRow(const RowSpec &spec);

	const not_null<EditorController*> _controller;
	std::vector<InspectorRow*> _rows;
	QByteArray _signature;
	NodeId _shown = 0;
	bool _syncScheduled = false;
	rpl::event_stream<> _scrollToTopRequests;

};

InspectorPanel::Content::Content(
	QWidget *parent,
	not_null<EditorController*> controller)
: VerticalLayout(parent)
, _controller(controller) {
	rpl::merge(
		_controller->documentChanged() | rpl::to_empty,
		_controller->selectionChanged()
	) | rpl::on_next([=] {
		scheduleSync();
	}, lifetime());

	rpl::merge(
		_controller->currentFrameChanged() | rpl::to_empty,
		_controller->activePropertyChanged()
	) | rpl::on_next([=] {
		if (!_syncScheduled) {
			refreshRows();
		}
	}, lifetime());

	style::PaletteChanged() | rpl::on_next([=] {
		update();
	}, lifetime());

	sync();
}

rpl::producer<> InspectorPanel::Content::scrollToTopRequests() const {
	return _scrollToTopRequests.events();
}

void InspectorPanel::Content::scheduleSync() {
	if (_syncScheduled) {
		return;
	}
	_syncScheduled = true;
	// Deferred: an edit may come from a widget of a row that the sync
	// destroys, and one sync covers several signals.
	crl::on_main(this, [=] {
		_syncScheduled = false;
		sync();
	});
}

void InspectorPanel::Content::sync() {
	auto specs = CollectSpecs(_controller);
	auto signature = Signature(specs);
	const auto shown = _controller->primarySelection();
	if (signature != _signature) {
		_signature = std::move(signature);
		rebuild(std::move(specs));
	} else {
		refreshRows();
	}
	if (_shown != shown) {
		_shown = shown;
		_scrollToTopRequests.fire({});
	}
}

object_ptr<InspectorRow> InspectorPanel::Content::createRow(const RowSpec &spec) {
	switch (spec.kind) {
	case SpecKind::Header:
		return object_ptr<HeaderRow>(
			this,
			_controller,
			spec.node,
			spec.count,
			spec.info);
	case SpecKind::Info:
		return object_ptr<InfoRow>(this, spec.text, false, false);
	case SpecKind::Hint:
		return object_ptr<InfoRow>(this, spec.text, true, spec.indented);
	case SpecKind::Section:
		return object_ptr<SectionRow>(this, spec.text);
	case SpecKind::Property:
		return object_ptr<PropertyRow>(this, _controller, spec);
	case SpecKind::Choice:
		return object_ptr<ChoiceRow>(this, _controller, spec);
	case SpecKind::Number:
		return object_ptr<NumberRow>(
			this,
			_controller,
			NumberDescriptor(spec),
			spec.node != 0);
	case SpecKind::Check:
		return object_ptr<CheckRow>(
			this,
			spec.text,
			(spec.check == CheckKind::ScaleContent)
				? &ScaleContentOption()
				: &KeepDurationOption());
	}
	return object_ptr<InspectorRow>(nullptr);
}

void InspectorPanel::Content::rebuild(std::vector<RowSpec> &&specs) {
	// Typed values go to the node they were typed for.
	for (const auto row : base::take(_rows)) {
		row->commitEditing();
	}
	clear();
	for (const auto &spec : specs) {
		if (auto row = createRow(spec)) {
			const auto raw = row.data();
			add(std::move(row));
			_rows.push_back(raw);
		}
	}
	if (width() > 0) {
		resizeToWidth(width());
	}
	refreshRows();
}

void InspectorPanel::Content::refreshRows() {
	for (const auto row : _rows) {
		row->refresh();
	}
}

// InspectorPanel.

InspectorPanel::InspectorPanel(
	QWidget *parent,
	not_null<EditorController*> controller)
: RpWidget(parent)
, _controller(controller)
, _tabs(Ui::CreateChild<Ui::PillTabs>(
	this,
	std::vector<QString>{
		tr::lng_oblivion_lottie_editor_properties(tr::now),
		tr::lng_oblivion_lottie_palette_tab(tr::now),
	},
	(LastTab() == Tab::Palette) ? 1 : 0,
	TabsStyle()))
, _propertiesScroll(Ui::CreateChild<Ui::ScrollArea>(
	this,
	st::defaultScrollArea))
, _content(_propertiesScroll->setOwnedWidget(
	object_ptr<Content>(_propertiesScroll.get(), controller)).data())
, _paletteScroll(Ui::CreateChild<Ui::ScrollArea>(
	this,
	st::defaultScrollArea))
, _palette(_paletteScroll->setOwnedWidget(
	object_ptr<PalettePanel>(_paletteScroll.get(), controller)).data())
, _tab(LastTab()) {
	setFocusPolicy(Qt::ClickFocus);

	_tabs->activeIndexChanges() | rpl::on_next([=](int index) {
		if (!_switching) {
			showTab(index ? Tab::Palette : Tab::Properties, true);
		}
	}, lifetime());

	_propertiesScroll->widthValue() | rpl::on_next([=](int width) {
		_content->resizeToWidth(width);
	}, _content->lifetime());
	_paletteScroll->widthValue() | rpl::on_next([=](int width) {
		_palette->resizeToWidth(width);
	}, _palette->lifetime());

	_content->scrollToTopRequests() | rpl::on_next([=] {
		_propertiesScroll->scrollToY(0);
	}, lifetime());

	style::PaletteChanged() | rpl::on_next([=] {
		update();
	}, lifetime());

	showTab(_tab, false);
}

InspectorPanel::~InspectorPanel() = default;

void InspectorPanel::setTab(Tab tab) {
	showTab(tab, false);
}

InspectorPanel::Tab InspectorPanel::tab() const {
	return _tab;
}

void InspectorPanel::showTab(Tab tab, bool remember) {
	_tab = tab;
	if (remember) {
		LastTab() = tab;
	}
	const auto index = (tab == Tab::Palette) ? 1 : 0;
	_propertiesScroll->setVisible(tab == Tab::Properties);
	_paletteScroll->setVisible(tab == Tab::Palette);
	if (_tabs->activeIndex() != index) {
		_switching = true;
		_tabs->setActiveIndex(index);
		_switching = false;
	}
}

void InspectorPanel::updateGeometries() {
	const auto padding = Scaled(10);
	const auto tabsHeight = TabsStyle().height;
	_tabs->setGeometry(
		padding,
		padding,
		std::max(width() - 2 * padding, 0),
		tabsHeight);
	const auto top = padding + tabsHeight + Scaled(6);
	const auto rect = QRect(0, top, width(), std::max(height() - top, 0));
	_propertiesScroll->setGeometry(rect);
	_paletteScroll->setGeometry(rect);
}

void InspectorPanel::resizeEvent(QResizeEvent *e) {
	updateGeometries();
}

void InspectorPanel::paintEvent(QPaintEvent *e) {
	QPainter(this).fillRect(e->rect(), st::windowBg);
}

// Snapshot scenes (OBLIVION_SELFTEST=ui, see oblivion_ui_snapshots.h).

namespace {

[[nodiscard]] QWidget *CreateInspectorScene(not_null<Ui::RpWidget*> parent) {
	return Ui::CreateChild<PanelSceneHost>(
		parent.get(),
		u":/animations/palette.tgs"_q,
		[](QWidget *parent, not_null<EditorController*> controller) {
			return not_null<Ui::RpWidget*>(
				Ui::CreateChild<InspectorPanel>(parent, controller));
		});
}

[[nodiscard]] not_null<EditorController*> SceneController(
		not_null<QWidget*> widget) {
	const auto host = static_cast<PanelSceneHost*>(widget.get());
	static_cast<InspectorPanel*>(host->panel().get())->setTab(
		InspectorPanel::Tab::Properties);
	return host->controller();
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	RegisterScene(
		u"lottie_inspector_layer"_q,
		QSize(320, 720),
		CreateInspectorScene,
		[](not_null<QWidget*> widget) {
			const auto controller = SceneController(widget);
			const auto &document = controller->document();
			if (const auto layer = FindNodeByName(
					document,
					u"Purple"_q,
					NodeKind::Layer)) {
				controller->select(layer);
				controller->setActiveProperty(PropertyRef{ layer, "ks.o" });
			}
			controller->setCurrentFrame(controller->firstFrame() + 20);
		});

	RegisterScene(
		u"lottie_inspector_stroke"_q,
		QSize(320, 560),
		CreateInspectorScene,
		[](not_null<QWidget*> widget) {
			const auto controller = SceneController(widget);
			const auto stroke = FindShapeOfType(
				controller->document(),
				ShapeType::Stroke,
				3);
			if (stroke) {
				controller->select(stroke);
				controller->setActiveProperty(PropertyRef{ stroke, "w" });
			}
			controller->setCurrentFrame(controller->firstFrame() + 40);
		});

	RegisterScene(
		u"lottie_inspector_gradient"_q,
		QSize(320, 620),
		CreateInspectorScene,
		[](not_null<QWidget*> widget) {
			const auto controller = SceneController(widget);
			if (const auto gradient = FindShapeOfType(
					controller->document(),
					ShapeType::GradientFill)) {
				controller->select(gradient);
			}
		});

	RegisterScene(
		u"lottie_inspector_composition"_q,
		QSize(320, 560),
		CreateInspectorScene,
		[](not_null<QWidget*> widget) {
			SceneController(widget)->clearSelection();
		});

	RegisterScene(
		u"lottie_inspector_palette"_q,
		QSize(320, 720),
		CreateInspectorScene,
		[](not_null<QWidget*> widget) {
			const auto host = static_cast<PanelSceneHost*>(widget.get());
			static_cast<InspectorPanel*>(host->panel().get())->setTab(
				InspectorPanel::Tab::Palette);
		});

	// The whole palette tab with an active color: the hex field and the
	// links under the grid, the correction sliders, the checkbox, reset.
	RegisterScene(
		u"lottie_inspector_palette_active"_q,
		QSize(320, 1000),
		CreateInspectorScene,
		[](not_null<QWidget*> widget) {
			const auto host = static_cast<PanelSceneHost*>(widget.get());
			const auto inspector = static_cast<InspectorPanel*>(
				host->panel().get());
			inspector->setTab(InspectorPanel::Tab::Palette);
			for (const auto child : inspector->findChildren<QWidget*>()) {
				if (const auto palette = dynamic_cast<PalettePanel*>(child)) {
					palette->activateEntry(1);
					break;
				}
			}
		});
});

} // namespace

} // namespace Oblivion::LottieEdit
