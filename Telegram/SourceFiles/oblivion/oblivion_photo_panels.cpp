/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_photo_panels.h"

#include "base/flat_map.h"
#include "base/unique_qptr.h"
#include "lang/lang_keys.h"
#include "oblivion/oblivion_photo_editor_controls.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/effects/animation_value.h"
#include "ui/effects/ripple_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/color_editor.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_basic.h"
#include "styles/style_calls.h"
#include "styles/style_layers.h"
#include "styles/style_media_view.h"
#include "styles/style_widgets.h"

#include <QtCore/QRandomGenerator>
#include <QtGui/QPainterPath>

namespace Oblivion::Photo {
namespace {

using namespace EditorUi;

constexpr auto kPadding = 16;
constexpr auto kSkip = 8;
constexpr auto kCardRadius = 12;
constexpr auto kCardPadding = 12;
constexpr auto kCardSkip = 10;
constexpr auto kRowHeight = 40;
constexpr auto kRowRadius = 8;
constexpr auto kChevronSize = 5;
constexpr auto kFieldHeight = 26;
constexpr auto kFieldMinWidth = 76;
constexpr auto kSeedButton = 30;
constexpr auto kHandleRadius = 9;
constexpr auto kHandleCross = 16;
constexpr auto kMaxSliderSteps = 100000;
constexpr auto kLayerRowHeight = 40;
constexpr auto kLayerEyeWidth = 40;
constexpr auto kLayerThumb = 28;
constexpr auto kLayersHeaderHeight = 34;
constexpr auto kDimmedOpacity = 0.4;
constexpr auto kMaxStackEffects = 48;

[[nodiscard]] QColor TextColor() {
	return st::groupCallMembersFg->c;
}

[[nodiscard]] QColor SubTextColor() {
	return st::groupCallMemberNotJoinedStatus->c;
}

[[nodiscard]] QColor AccentColor() {
	return st::groupCallActiveFg->c;
}

[[nodiscard]] style::margins RowMargins(int top = 0) {
	return style::margins(Px(kPadding), top, Px(kPadding), 0);
}

// The first eight are the palette of the drawing tools in its order, so
// two color rows of one panel (a text and its plate) look alike.
[[nodiscard]] std::vector<QColor> SwatchPresets(QColor first) {
	auto result = std::vector<QColor>{
		QColor(0xFF, 0xFF, 0xFF),
		QColor(0x00, 0x00, 0x00),
		QColor(0xE5, 0x39, 0x35),
		QColor(0xFB, 0x8C, 0x00),
		QColor(0xFD, 0xD8, 0x35),
		QColor(0x7C, 0xB3, 0x42),
		QColor(0x1E, 0x88, 0xE5),
		QColor(0x8E, 0x24, 0xAA),
		QColor(0xFF, 0x80, 0xAB),
		QColor(0x00, 0x89, 0x7B),
	};
	if (first.isValid()) {
		first.setAlpha(255);
		if (!ranges::contains(result, first)) {
			result.insert(begin(result), first);
		}
	}
	return result;
}

[[nodiscard]] const style::InputField &FieldStyle() {
	static const auto result = [] {
		auto st = st::defaultInputField;
		st.textBg = st::groupCallBg;
		st.textBgActive = st::groupCallBg;
		st.textFg = st::groupCallMembersFg;
		st.textMargins = QMargins(Px(8), Px(4), Px(8), Px(2));
		st.textAlign = style::al_right;
		st.placeholderMargins = QMargins();
		st.placeholderScale = 0.;
		st.placeholderShift = 0;
		st.placeholderFont = st::normalFont;
		st.borderFg = st::groupCallMemberInactiveIcon;
		st.borderFgActive = st::groupCallActiveFg;
		st.borderFgError = st::groupCallActiveFg;
		st.border = std::max(Px(1), 1);
		st.borderActive = st.border;
		st.borderRadius = Px(6);
		st.borderDenominator = 1;
		st.style = st::defaultTextStyle;
		st.width = 0;
		st.widthMin = 0;
		st.heightMin = Px(kFieldHeight);
		st.heightMax = Px(kFieldHeight);
		return st;
	}();
	return result;
}

// The parent of an inline field. What is typed into the field stays with
// it: Escape and Enter that the field has handled, and the keys it had
// nothing to do with (Backspace in an empty field, an arrow at the end of
// the text), must not reach the shortcuts of the editor and of its tools,
// where Escape closes the editor and Backspace deletes a layer or a photo
// of a collage. Only the shortcuts with Cmd / Ctrl (save, zoom) go on.
class FieldHost final : public Ui::RpWidget {
public:
	using RpWidget::RpWidget;

protected:
	void keyPressEvent(QKeyEvent *e) override {
		const auto key = e->key();
		if (key == Qt::Key_Escape
			|| key == Qt::Key_Return
			|| key == Qt::Key_Enter
			|| !(e->modifiers() & Qt::ControlModifier)) {
			e->accept();
			return;
		}
		RpWidget::keyPressEvent(e);
	}

};

void FocusEditor(not_null<QWidget*> from) {
	for (auto parent = from->parentWidget()
		; parent
		; parent = parent->parentWidget()) {
		if (parent->focusPolicy() == Qt::StrongFocus) {
			parent->setFocus();
			return;
		}
	}
}

struct ParamChange {
	FxValue value;
	bool finished = false;
};

struct SliderMap {
	double step = 1.;
	int min = 0;
	int max = 100;
	int value = 0;
};

[[nodiscard]] SliderMap MapFor(const FxParam &param) {
	auto step = (param.kind == FxParamKind::Float
		|| param.kind == FxParamKind::Angle)
		? ((param.step > 0.) ? param.step : 1.)
		: 1.;
	const auto range = std::max(param.max - param.min, step);
	if (range / step > kMaxSliderSteps) {
		step = range / kMaxSliderSteps;
	}
	return {
		.step = step,
		.min = int(std::lround(param.min / step)),
		.max = int(std::lround(param.max / step)),
		.value = int(std::lround(param.defaultValue().number() / step)),
	};
}

[[nodiscard]] QString NumberText(const FxParam &param, double value) {
	const auto whole = (param.kind == FxParamKind::Int)
		|| (param.kind == FxParamKind::Seed)
		|| (param.decimals <= 0);
	auto result = QString();
	if (whole) {
		const auto rounded = int(std::lround(value));
		result = (param.min < 0. && param.kind != FxParamKind::Seed)
			? FormatSigned(rounded)
			: QString::number(rounded);
	} else {
		result = FormatDecimal(value, param.decimals);
		if (param.min < 0. && value > 0. && result != FormatDecimal(0., param.decimals)) {
			result = u"+"_q + result;
		}
	}
	return result + param.suffix;
}

[[nodiscard]] std::optional<double> ParseNumber(QString text) {
	auto cleaned = QString();
	for (const auto ch : text) {
		if (ch.isDigit() || ch == '.') {
			cleaned.push_back(ch);
		} else if (ch == ',') {
			cleaned.push_back(QChar('.'));
		} else if (ch == '-' || ch == QChar(0x2212)) {
			if (cleaned.isEmpty()) {
				cleaned.push_back(QChar('-'));
			}
		}
	}
	auto ok = false;
	const auto result = cleaned.toDouble(&ok);
	return (ok && std::isfinite(result))
		? std::make_optional(result)
		: std::nullopt;
}

// A slider with a value that can be typed: Float, Int and Angle.
class SliderRow final : public Ui::RpWidget {
public:
	SliderRow(
		QWidget *parent,
		const FxParam &param,
		const FxValue &value,
		bool commitOnRelease,
		bool logarithmic);

	void setValue(const FxValue &value);
	void setDimmed(bool dimmed);
	[[nodiscard]] rpl::producer<ParamChange> changes() const;

protected:
	int resizeGetHeight(int newWidth) override;

private:
	[[nodiscard]] FxValue fromSlider(int value) const;
	[[nodiscard]] int toSlider(const FxValue &value) const;
	void startEdit();
	void finishEdit(bool apply, bool refocus);
	void placeField();

	const FxParam _param;
	const SliderMap _map;
	const bool _commitOnRelease = false;
	FxValue _value;
	ValueSlider *_slider = nullptr;
	FieldHost *_host = nullptr;
	Ui::InputField *_field = nullptr;
	bool _editing = false;
	rpl::event_stream<ParamChange> _changes;

};

SliderRow::SliderRow(
	QWidget *parent,
	const FxParam &param,
	const FxValue &value,
	bool commitOnRelease,
	bool logarithmic)
: RpWidget(parent)
, _param(param)
, _map(MapFor(param))
, _commitOnRelease(commitOnRelease)
, _value(param.normalized(value)) {
	const auto map = _map;
	const auto copy = _param;
	_slider = Ui::CreateChild<ValueSlider>(this, SliderArgs{
		.label = rpl::single(_param.name.now()),
		.min = _map.min,
		.max = _map.max,
		.defaultValue = _map.value,
		.value = toSlider(_value),
		.format = [=](int value) {
			return NumberText(copy, value * map.step);
		},
		.track = (_param.look == FxSliderLook::Temperature)
			? SliderTrack::Temperature
			: (_param.look == FxSliderLook::Tint)
			? SliderTrack::Tint
			: SliderTrack::Plain,
		.logarithmic = logarithmic,
	});
	_slider->setValueClickable(true);
	_slider->show();
	_slider->changes() | rpl::on_next([=](SliderChange change) {
		// A typed value between the slider steps stays until the slider
		// really moves.
		if (change.value != toSlider(_value)) {
			_value = fromSlider(change.value);
		}
		if (change.finished || !_commitOnRelease) {
			_changes.fire({ _value, change.finished });
		}
	}, _slider->lifetime());
	_slider->valueClicks() | rpl::on_next([=] {
		startEdit();
	}, _slider->lifetime());
}

FxValue SliderRow::fromSlider(int value) const {
	return _param.normalized(FxValue::Number(value * _map.step));
}

int SliderRow::toSlider(const FxValue &value) const {
	return std::clamp(
		int(std::lround(value.number() / _map.step)),
		_map.min,
		_map.max);
}

void SliderRow::setValue(const FxValue &value) {
	const auto normalized = _param.normalized(value);
	if (_slider->dragging() || _editing || normalized == _value) {
		return;
	}
	_value = normalized;
	_slider->setValue(toSlider(_value));
}

void SliderRow::setDimmed(bool dimmed) {
	_slider->setDimmed(dimmed);
}

rpl::producer<ParamChange> SliderRow::changes() const {
	return _changes.events();
}

int SliderRow::resizeGetHeight(int newWidth) {
	_slider->resizeToWidth(newWidth);
	_slider->moveToLeft(0, 0, newWidth);
	placeField();
	return _slider->height();
}

void SliderRow::placeField() {
	if (!_host) {
		return;
	}
	const auto rect = _slider->valueRect();
	const auto fieldWidth = std::max(rect.width(), Px(kFieldMinWidth));
	const auto fieldHeight = Px(kFieldHeight);
	_host->setGeometry(
		width() - fieldWidth,
		std::max(rect.center().y() - fieldHeight / 2, 0),
		fieldWidth,
		fieldHeight);
	_field->setGeometry(0, 0, fieldWidth, fieldHeight);
}

void SliderRow::startEdit() {
	if (_editing) {
		return;
	}
	if (!_host) {
		_host = Ui::CreateChild<FieldHost>(this);
		_field = Ui::CreateChild<Ui::InputField>(
			_host,
			FieldStyle(),
			Ui::InputField::Mode::SingleLine,
			nullptr,
			QString());
		_field->setMaxLength(16);
		_field->submits() | rpl::on_next([=] {
			finishEdit(true, true);
		}, _field->lifetime());
		_field->cancelled() | rpl::on_next([=] {
			finishEdit(false, true);
		}, _field->lifetime());
		// The focus went somewhere else (another field, a click on the
		// photo): keep the typed value and leave the focus where it is.
		_field->focusedChanges() | rpl::filter(
			!rpl::mappers::_1
		) | rpl::on_next([=] {
			finishEdit(true, false);
		}, _field->lifetime());
	}
	_editing = true;
	auto suffixless = _param;
	suffixless.suffix = QString();
	_field->setText(NumberText(suffixless, _value.number()));
	placeField();
	_host->show();
	_host->raise();
	_field->selectAll();
	_field->setFocusFast();
}

void SliderRow::finishEdit(bool apply, bool refocus) {
	if (!_editing) {
		return;
	}
	_editing = false;
	const auto parsed = apply
		? ParseNumber(_field->getLastText())
		: std::nullopt;
	if (refocus) {
		FocusEditor(this);
	}
	_host->hide();
	if (parsed) {
		const auto value = _param.normalized(FxValue::Number(*parsed));
		if (!(value == _value)) {
			_value = value;
			_slider->setValue(toSlider(_value));
			_changes.fire({ _value, true });
		}
	}
}

// "Label ............ Value v": opens a menu with the choices.
class ChoiceRow final : public Ui::RippleButton {
public:
	ChoiceRow(
		QWidget *parent,
		QString label,
		std::vector<QString> choices,
		int value);

	void setValue(int value);
	void setDimmed(bool dimmed);
	[[nodiscard]] rpl::producer<int> chosen() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	QImage prepareRippleMask() const override;

private:
	void showMenu();

	const QString _label;
	const std::vector<QString> _choices;
	int _value = 0;
	bool _dimmed = false;
	base::unique_qptr<Ui::PopupMenu> _menu;
	rpl::event_stream<int> _chosen;

};

ChoiceRow::ChoiceRow(
	QWidget *parent,
	QString label,
	std::vector<QString> choices,
	int value)
: RippleButton(parent, st::groupCallRipple)
, _label(std::move(label))
, _choices(std::move(choices))
, _value(value) {
	setClickedCallback([=] { showMenu(); });
}

void ChoiceRow::setValue(int value) {
	if (_value != value) {
		_value = value;
		update();
	}
}

void ChoiceRow::setDimmed(bool dimmed) {
	if (_dimmed != dimmed) {
		_dimmed = dimmed;
		update();
	}
}

rpl::producer<int> ChoiceRow::chosen() const {
	return _chosen.events();
}

int ChoiceRow::resizeGetHeight(int newWidth) {
	return Px(kRowHeight);
}

QImage ChoiceRow::prepareRippleMask() const {
	return Ui::RippleAnimation::RoundRectMask(size(), Px(kRowRadius));
}

void ChoiceRow::showMenu() {
	if (_choices.empty()) {
		return;
	}
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::groupCallPopupMenu);
	for (auto i = 0; i != int(_choices.size()); ++i) {
		_menu->addAction(_choices[i], [=] {
			if (_value != i) {
				_value = i;
				update();
				_chosen.fire_copy(i);
			}
		});
	}
	_menu->popup(mapToGlobal(QPoint(width() / 3, height())));
}

void ChoiceRow::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto alpha = _dimmed ? kDimmedOpacity : 1.;
	const auto radius = Px(kRowRadius);
	if (isOver()) {
		p.setPen(Qt::NoPen);
		p.setBrush(anim::with_alpha(st::groupCallMembersBgOver->c, alpha));
		p.drawRoundedRect(rect(), radius, radius);
	}
	paintRipple(p, 0, 0);
	const auto &font = st::normalFont;
	// The chevron ends where the values of the sliders around the row do.
	const auto chevron = Px(kChevronSize);
	const auto right = width() - chevron * 2 - Px(1);
	const auto value = (_value >= 0 && _value < int(_choices.size()))
		? _choices[_value]
		: QString();
	const auto available = right - Px(kSkip);
	const auto labelWidth = std::min(font->width(_label), available * 5 / 10);
	const auto valueWidth = std::min(
		font->width(value),
		available - labelWidth - Px(kSkip));
	p.setFont(font);
	p.setPen(anim::with_alpha(TextColor(), alpha));
	p.drawText(
		QRect(0, 0, labelWidth, height()),
		Qt::AlignLeft | Qt::AlignVCenter,
		font->elided(_label, labelWidth));
	p.setPen(anim::with_alpha(AccentColor(), alpha));
	p.drawText(
		QRect(right - Px(kSkip) - valueWidth, 0, valueWidth, height()),
		Qt::AlignRight | Qt::AlignVCenter,
		font->elided(value, valueWidth));
	auto pen = QPen(anim::with_alpha(SubTextColor(), alpha), Px(2) * 0.75);
	pen.setCapStyle(Qt::RoundCap);
	pen.setJoinStyle(Qt::RoundJoin);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	const auto center = QPointF(right + chevron, height() / 2. + chevron / 4.);
	auto path = QPainterPath();
	path.moveTo(center + QPointF(-chevron, -chevron / 2.));
	path.lineTo(center + QPointF(0., chevron / 2.));
	path.lineTo(center + QPointF(chevron, -chevron / 2.));
	p.drawPath(path);
}

// A random variant: the number and a button that rolls another one.
class SeedRow final : public Ui::RpWidget {
public:
	SeedRow(QWidget *parent, QString label, int value);

	void setValue(int value);
	void setDimmed(bool dimmed);
	[[nodiscard]] rpl::producer<int> chosen() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	const QString _label;
	int _value = 0;
	bool _dimmed = false;
	ToolButton *_button = nullptr;
	rpl::event_stream<int> _chosen;

};

void PaintDiceIcon(QPainter &p, QRectF rect, QColor color) {
	p.translate(rect.topLeft());
	p.scale(rect.width() / 24., rect.height() / 24.);
	p.setPen(QPen(color, 1.6));
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(QRectF(4.5, 4.5, 15., 15.), 3.5, 3.5);
	p.setPen(Qt::NoPen);
	p.setBrush(color);
	for (const auto &dot : {
		QPointF(8.5, 8.5),
		QPointF(15.5, 8.5),
		QPointF(12., 12.),
		QPointF(8.5, 15.5),
		QPointF(15.5, 15.5),
	}) {
		p.drawEllipse(dot, 1.3, 1.3);
	}
}

SeedRow::SeedRow(QWidget *parent, QString label, int value)
: RpWidget(parent)
, _label(std::move(label))
, _value(value) {
	_button = Ui::CreateChild<ToolButton>(
		this,
		IconRef{ .paint = PaintDiceIcon },
		Px(kSeedButton));
	_button->setTooltip(tr::lng_oblivion_photo_panel_randomize(tr::now));
	_button->setClickedCallback([=] {
		auto next = _value;
		while (next == _value) {
			next = QRandomGenerator::global()->bounded(kFxSeedMax + 1);
		}
		_value = next;
		update();
		_chosen.fire_copy(next);
	});
	_button->show();
}

void SeedRow::setValue(int value) {
	if (_value != value) {
		_value = value;
		update();
	}
}

void SeedRow::setDimmed(bool dimmed) {
	if (_dimmed != dimmed) {
		_dimmed = dimmed;
		update();
	}
}

rpl::producer<int> SeedRow::chosen() const {
	return _chosen.events();
}

int SeedRow::resizeGetHeight(int newWidth) {
	const auto height = Px(kRowHeight);
	_button->moveToLeft(
		newWidth - _button->width(),
		(height - _button->height()) / 2,
		newWidth);
	return height;
}

void SeedRow::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto alpha = _dimmed ? kDimmedOpacity : 1.;
	const auto &font = st::normalFont;
	const auto right = width() - _button->width() - Px(kSkip);
	const auto value = QString::number(_value);
	const auto valueWidth = font->width(value);
	p.setFont(font);
	p.setPen(anim::with_alpha(TextColor(), alpha));
	p.drawText(
		QRect(0, 0, std::max(right - valueWidth - Px(kSkip), 0), height()),
		Qt::AlignLeft | Qt::AlignVCenter,
		font->elided(_label, std::max(right - valueWidth - Px(kSkip), 0)));
	p.setPen(anim::with_alpha(_value ? AccentColor() : SubTextColor(), alpha));
	p.drawText(
		QRect(right - valueWidth, 0, valueWidth, height()),
		Qt::AlignRight | Qt::AlignVCenter,
		value);
}

// The temporary canvas tool of a Point parameter: the point is shown as
// a handle over the layer, a click or a drag anywhere moves it.
class PointTool final : public Tool {
public:
	PointTool(
		not_null<Controller*> controller,
		Fn<LayerId()> layer,
		Fn<QPointF()> value,
		Fn<void(QPointF point, bool finished)> changed);

	bool mousePress(const ToolMouseEvent &e) override;
	void mouseMove(const ToolMouseEvent &e) override;
	void mouseRelease(const ToolMouseEvent &e) override;
	void paint(QPainter &p, const ToolPaintContext &context) override;
	QCursor cursor(const ToolMouseEvent &e) override;

private:
	[[nodiscard]] const Layer *layer() const;
	[[nodiscard]] std::optional<QPointF> normalized(QPointF document) const;

	const not_null<Controller*> _controller;
	const Fn<LayerId()> _layer;
	const Fn<QPointF()> _value;
	const Fn<void(QPointF point, bool finished)> _changed;
	bool _dragging = false;

};

PointTool::PointTool(
	not_null<Controller*> controller,
	Fn<LayerId()> layer,
	Fn<QPointF()> value,
	Fn<void(QPointF point, bool finished)> changed)
: _controller(controller)
, _layer(std::move(layer))
, _value(std::move(value))
, _changed(std::move(changed)) {
}

const Layer *PointTool::layer() const {
	const auto id = _layer ? _layer() : LayerId(0);
	return id
		? _controller->document().find(id)
		: _controller->activeLayer();
}

std::optional<QPointF> PointTool::normalized(QPointF document) const {
	const auto target = layer();
	if (!target || target->size().isEmpty()) {
		return std::nullopt;
	}
	const auto local = LayerPoint(*target, document);
	if (!local) {
		return std::nullopt;
	}
	const auto size = target->size();
	return QPointF(
		std::clamp(local->x() / size.width(), 0., 1.),
		std::clamp(local->y() / size.height(), 0., 1.));
}

bool PointTool::mousePress(const ToolMouseEvent &e) {
	const auto point = normalized(e.document);
	if (!point) {
		return false;
	}
	_dragging = true;
	_changed(*point, false);
	_controller->updateCanvas();
	return true;
}

void PointTool::mouseMove(const ToolMouseEvent &e) {
	if (!_dragging) {
		return;
	} else if (const auto point = normalized(e.document)) {
		_changed(*point, false);
		_controller->updateCanvas();
	}
}

void PointTool::mouseRelease(const ToolMouseEvent &e) {
	if (!_dragging) {
		return;
	}
	_dragging = false;
	const auto point = normalized(e.document);
	_changed(point ? *point : _value(), true);
	_controller->updateCanvas();
}

void PointTool::paint(QPainter &p, const ToolPaintContext &context) {
	const auto target = layer();
	if (!target || target->size().isEmpty()) {
		return;
	}
	const auto value = _value();
	const auto size = target->size();
	const auto center = context.documentToWidget.map(target->transform.map(
		QPointF(value.x() * size.width(), value.y() * size.height())));
	const auto radius = float64(Px(kHandleRadius));
	const auto cross = float64(Px(kHandleCross));
	const auto line = std::max(Px(2), 2);
	for (const auto outline : { true, false }) {
		auto pen = QPen(
			outline ? QColor(0, 0, 0, 150) : QColor(255, 255, 255),
			outline ? (line + 2.) : float64(line));
		pen.setCapStyle(Qt::RoundCap);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(center, radius, radius);
		p.drawLine(
			center - QPointF(cross, 0.),
			center - QPointF(radius + line, 0.));
		p.drawLine(
			center + QPointF(radius + line, 0.),
			center + QPointF(cross, 0.));
		p.drawLine(
			center - QPointF(0., cross),
			center - QPointF(0., radius + line));
		p.drawLine(
			center + QPointF(0., radius + line),
			center + QPointF(0., cross));
	}
	p.setPen(Qt::NoPen);
	p.setBrush(st::groupCallActiveFg);
	p.drawEllipse(center, line * 1.5, line * 1.5);
}

QCursor PointTool::cursor(const ToolMouseEvent &e) {
	return QCursor(Qt::CrossCursor);
}

// A Point parameter: the coordinates and the button that turns picking
// on the canvas on and off.
class PointRow final : public Ui::RippleButton {
public:
	PointRow(
		QWidget *parent,
		QString label,
		QPointF value,
		Controller *controller,
		Fn<LayerId()> layer);
	~PointRow();

	void setValue(QPointF value);
	void setDimmed(bool dimmed);
	[[nodiscard]] rpl::producer<ParamChange> changes() const;

	// The row that can pick its point on the canvas of this editor now
	// (shown and not picking yet), null if there is none.
	[[nodiscard]] static PointRow *FindFor(
		not_null<const Controller*> controller);
	void startPicking();

protected:
	void paintEvent(QPaintEvent *e) override;
	void hideEvent(QHideEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	QImage prepareRippleMask() const override;

private:
	[[nodiscard]] static std::vector<not_null<PointRow*>> &All();
	void toggle();
	void stopPicking();

	const QString _label;
	QPointF _value;
	// The panel may be shown where it outlives the editor.
	const base::weak_ptr<Controller> _controller;
	const bool _pickable = false;
	const Fn<LayerId()> _layer;
	bool _picking = false;
	bool _dimmed = false;
	rpl::event_stream<ParamChange> _changes;

};

PointRow::PointRow(
	QWidget *parent,
	QString label,
	QPointF value,
	Controller *controller,
	Fn<LayerId()> layer)
: RippleButton(parent, st::groupCallRipple)
, _label(std::move(label))
, _value(value)
, _controller(controller)
, _pickable(controller != nullptr)
, _layer(std::move(layer)) {
	setClickedCallback([=] { toggle(); });
	if (!_pickable) {
		setAttribute(Qt::WA_TransparentForMouseEvents);
	}
	All().push_back(this);
}

PointRow::~PointRow() {
	auto &all = All();
	all.erase(ranges::remove(all, not_null<PointRow*>(this)), end(all));
	const auto controller = _controller.get();
	if (_picking && controller) {
		_picking = false;
		controller->clearTemporaryTool();
	}
}

std::vector<not_null<PointRow*>> &PointRow::All() {
	static auto result = std::vector<not_null<PointRow*>>();
	return result;
}

PointRow *PointRow::FindFor(not_null<const Controller*> controller) {
	for (const auto &row : All()) {
		if (row->_controller.get() == controller.get()
			&& !row->_picking
			&& row->isVisibleTo(row->window())
			&& (row->height() > 0)) {
			return row;
		}
	}
	return nullptr;
}

void PointRow::startPicking() {
	if (!_picking) {
		toggle();
	}
}

void PointRow::toggle() {
	const auto controller = _controller.get();
	if (!controller) {
		return;
	} else if (_picking) {
		controller->clearTemporaryTool();
		return;
	} else if (_dimmed) {
		// The effect is switched off: the point would be moved blindly.
		controller->showToast(
			tr::lng_oblivion_photo_distort_switched_off(tr::now));
		return;
	}
	const auto weak = base::make_weak(this);
	controller->setTemporaryTool(
		std::make_unique<PointTool>(
			controller,
			_layer,
			[=] {
				const auto strong = weak.get();
				return strong ? strong->_value : QPointF(0.5, 0.5);
			},
			[=](QPointF point, bool finished) {
				if (const auto strong = weak.get()) {
					strong->_value = point;
					strong->update();
					strong->_changes.fire({
						FxValue::Point(point),
						finished,
					});
				}
			}),
		[=] {
			if (const auto strong = weak.get()) {
				if (strong->_picking) {
					strong->_picking = false;
					strong->update();
				}
			}
		});
	if (!controller->hasTemporaryTool()) {
		return;
	}
	_picking = true;
	update();
	controller->showToast(tr::lng_oblivion_photo_panel_point_hint(tr::now));
}

void PointRow::setValue(QPointF value) {
	if (_value != value) {
		_value = value;
		update();
		const auto controller = _controller.get();
		if (_picking && controller) {
			controller->updateCanvas();
		}
	}
}

void PointRow::setDimmed(bool dimmed) {
	if (_dimmed != dimmed) {
		_dimmed = dimmed;
		update();
		if (dimmed) {
			stopPicking();
		}
	}
}

// While _picking is set the temporary tool of the editor is the one of
// this row: any other tool that takes its place resets the flag first.
void PointRow::stopPicking() {
	const auto controller = _controller.get();
	if (_picking && controller) {
		controller->clearTemporaryTool();
	}
}

// The row went out of sight together with its "Done" (another tab of the
// panel): the photo doesn't take the point anymore. A minimized window is
// not that.
void PointRow::hideEvent(QHideEvent *e) {
	RippleButton::hideEvent(e);
	if (!e->spontaneous()) {
		stopPicking();
	}
}

rpl::producer<ParamChange> PointRow::changes() const {
	return _changes.events();
}

int PointRow::resizeGetHeight(int newWidth) {
	return Px(kRowHeight);
}

QImage PointRow::prepareRippleMask() const {
	return Ui::RippleAnimation::RoundRectMask(size(), Px(kRowRadius));
}

void PointRow::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto alpha = _dimmed ? kDimmedOpacity : 1.;
	const auto radius = Px(kRowRadius);
	if (_picking || isOver()) {
		p.setPen(Qt::NoPen);
		p.setBrush(_picking
			? anim::with_alpha(AccentColor(), 0.18)
			: anim::with_alpha(st::groupCallMembersBgOver->c, alpha));
		p.drawRoundedRect(rect(), radius, radius);
	}
	paintRipple(p, 0, 0);
	const auto &font = st::normalFont;
	const auto value = _picking
		? tr::lng_oblivion_photo_panel_point_done(tr::now)
		: _pickable
		? tr::lng_oblivion_photo_panel_point_pick(tr::now)
		: FormatParamValue(
			FxPoint("p", FxText()),
			FxValue::Point(_value));
	const auto valueWidth = std::min(font->width(value), width() / 2);
	const auto labelWidth = std::max(width() - valueWidth - Px(kSkip), 0);
	p.setFont(font);
	p.setPen(anim::with_alpha(TextColor(), alpha));
	p.drawText(
		QRect(0, 0, labelWidth, height()),
		Qt::AlignLeft | Qt::AlignVCenter,
		font->elided(_label, labelWidth));
	p.setPen(anim::with_alpha(AccentColor(), alpha));
	p.drawText(
		QRect(width() - valueWidth, 0, valueWidth, height()),
		Qt::AlignRight | Qt::AlignVCenter,
		font->elided(value, valueWidth));
}

// Fades what is under it to the color of the plate it lies on, the mouse
// goes through.
class Veil final : public Ui::RpWidget {
public:
	Veil(QWidget *parent, style::color bg, float64 opacity)
	: RpWidget(parent)
	, _bg(bg)
	, _opacity(opacity) {
		setAttribute(Qt::WA_TransparentForMouseEvents);
		hide();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		QPainter(this).fillRect(
			e->rect(),
			anim::with_alpha(_bg->c, _opacity));
	}

private:
	const style::color _bg;
	const float64 _opacity = 0.;

};

// The widget of a custom editor with a veil over it while its effect is
// switched off, so every editor looks dimmed the way the plain rows do
// without painting that itself.
class DimmedEditor final : public Ui::RpWidget {
public:
	DimmedEditor(QWidget *parent, object_ptr<Ui::RpWidget> editor);

	void setDimmed(bool dimmed);

protected:
	int resizeGetHeight(int newWidth) override;

private:
	const not_null<Ui::RpWidget*> _editor;
	const not_null<Veil*> _veil;
	bool _resizing = false;

};

DimmedEditor::DimmedEditor(QWidget *parent, object_ptr<Ui::RpWidget> editor)
: RpWidget(parent)
, _editor(editor.release())
, _veil(Ui::CreateChild<Veil>(this, st::groupCallBg, 1. - kDimmedOpacity)) {
	_editor->setParent(this);
	_editor->move(0, 0);
	_editor->show();
	_veil->raise();
	_editor->heightValue() | rpl::on_next([=](int height) {
		if (!_resizing && height != this->height() && width() > 0) {
			resizeToWidth(width());
		}
	}, lifetime());
}

void DimmedEditor::setDimmed(bool dimmed) {
	_veil->setVisible(dimmed);
	if (dimmed) {
		_veil->raise();
	}
}

int DimmedEditor::resizeGetHeight(int newWidth) {
	_resizing = true;
	_editor->resizeToWidth(newWidth);
	_editor->moveToLeft(0, 0, newWidth);
	_resizing = false;
	const auto result = _editor->height();
	_veil->setGeometry(0, 0, newWidth, result);
	return result;
}

// The generic parameter panel.
class ParamsPanel final : public Ui::VerticalLayout {
public:
	ParamsPanel(QWidget *parent, ParamsPanelArgs &&args);

private:
	struct Row {
		QByteArray id;
		Ui::SlideWrap<Ui::RpWidget> *wrap = nullptr;
		Fn<void(const FxValue &value)> set;
		Fn<void(bool dimmed)> dim;
		Fn<bool(const FxParams&)> visible;
	};

	void addRow(const FxParam &param);
	void edited(const QByteArray &id, FxValue value, bool finished);
	void showValues(const FxParams &values);
	void refreshVisibility(anim::type animated);
	[[nodiscard]] LayerId pointLayer() const;

	const std::vector<FxParam> _params;
	Controller * const _controller = nullptr;
	const LayerId _layerId = 0;
	const uint64 _fxUid = 0;
	const bool _commitOnRelease = false;
	const std::vector<QByteArray> _logarithmic;
	const Fn<void(const QByteArray &id, FxValue value, bool finished)> _changed;
	FxParams _values;
	rpl::variable<bool> _dimmed = false;
	std::vector<Row> _rows;

};

ParamsPanel::ParamsPanel(QWidget *parent, ParamsPanelArgs &&args)
: VerticalLayout(parent)
, _params(std::move(args.params))
, _controller(args.controller)
, _layerId(args.layerId)
, _fxUid(args.fxUid)
, _commitOnRelease(args.commitOnRelease)
, _logarithmic(std::move(args.logarithmic))
, _changed(std::move(args.changed)) {
	for (const auto &param : _params) {
		_values.set(param.id, param.normalized(args.values.value(param.id)));
	}
	for (const auto &param : _params) {
		addRow(param);
	}
	refreshVisibility(anim::type::instant);
	if (args.updates) {
		std::move(args.updates) | rpl::on_next([=](const FxParams &values) {
			showValues(values);
		}, lifetime());
	}
	if (args.dimmed) {
		_dimmed = std::move(args.dimmed);
	}
	_dimmed.value() | rpl::on_next([=](bool dimmed) {
		for (const auto &row : _rows) {
			if (row.dim) {
				row.dim(dimmed);
			}
		}
	}, lifetime());
}

LayerId ParamsPanel::pointLayer() const {
	return _layerId;
}

void ParamsPanel::addRow(const FxParam &param) {
	const auto id = param.id;
	const auto &value = _values.value(id);
	auto row = Row{ .id = id, .visible = param.visible };
	auto widget = object_ptr<Ui::RpWidget>(nullptr);
	const auto report = [=](FxValue value, bool finished) {
		edited(id, std::move(value), finished);
	};
	if (!param.section.empty()) {
		// A subtitle follows the visibility of its first parameter.
		const auto title = add(
			object_ptr<Ui::SlideWrap<SectionTitle>>(
				this,
				object_ptr<SectionTitle>(
					this,
					rpl::single(param.section.now()))));
		const auto visible = param.visible;
		_rows.push_back({
			.visible = [=](const FxParams &values) {
				title->toggle(
					!visible || visible(values),
					anim::type::instant);
				return true;
			},
		});
	}
	switch (param.kind) {
	case FxParamKind::Float:
	case FxParamKind::Int:
	case FxParamKind::Angle: {
		auto slider = object_ptr<SliderRow>(
			this,
			param,
			value,
			_commitOnRelease,
			ranges::contains(_logarithmic, id));
		const auto raw = slider.data();
		raw->changes() | rpl::on_next([=](const ParamChange &change) {
			report(change.value, change.finished);
		}, raw->lifetime());
		row.set = [=](const FxValue &value) { raw->setValue(value); };
		row.dim = [=](bool dimmed) { raw->setDimmed(dimmed); };
		widget = std::move(slider);
	} break;
	case FxParamKind::Bool: {
		auto toggle = object_ptr<SwitchHeader>(
			this,
			param.name.now(),
			value.boolean(),
			false);
		const auto raw = toggle.data();
		raw->toggles() | rpl::on_next([=](bool checked) {
			report(FxValue::Boolean(checked), true);
		}, raw->lifetime());
		row.set = [=](const FxValue &value) {
			raw->setChecked(value.boolean(), anim::type::normal);
		};
		widget = std::move(toggle);
	} break;
	case FxParamKind::Choice: {
		auto names = std::vector<QString>();
		for (const auto &choice : param.choices) {
			names.push_back(choice.now());
		}
		auto choice = object_ptr<ChoiceRow>(
			this,
			param.name.now(),
			std::move(names),
			value.integer());
		const auto raw = choice.data();
		raw->chosen() | rpl::on_next([=](int index) {
			report(FxValue::Integer(index), true);
		}, raw->lifetime());
		row.set = [=](const FxValue &value) {
			raw->setValue(value.integer());
		};
		row.dim = [=](bool dimmed) { raw->setDimmed(dimmed); };
		widget = std::move(choice);
	} break;
	case FxParamKind::Color: {
		auto swatches = object_ptr<ColorSwatches>(
			this,
			rpl::single(param.name.now()),
			SwatchPresets(param.color),
			value.color());
		const auto raw = swatches.data();
		const auto controller = _controller;
		const auto opacity = param.alpha;
		raw->chosen() | rpl::on_next([=](QColor color) {
			if (opacity) {
				color.setAlpha(_values.color(id).alpha());
			}
			report(FxValue::Color(color), true);
		}, raw->lifetime());
		raw->customRequests() | rpl::on_next([=] {
			const auto show = controller ? controller->uiShow() : nullptr;
			if (!show) {
				return;
			}
			const auto weak = base::make_weak(raw);
			ShowColorPickerBox(show, _values.color(id), [=](QColor color) {
				if (weak.get()) {
					report(FxValue::Color(color), true);
				}
			});
		}, raw->lifetime());
		row.set = [=](const FxValue &value) {
			auto color = value.color();
			color.setAlpha(255);
			raw->setColor(color);
		};
		widget = std::move(swatches);
	} break;
	case FxParamKind::Point: {
		auto point = object_ptr<PointRow>(
			this,
			param.name.now(),
			value.point(),
			_controller,
			[=] { return pointLayer(); });
		const auto raw = point.data();
		raw->changes() | rpl::on_next([=](const ParamChange &change) {
			report(change.value, change.finished);
		}, raw->lifetime());
		row.set = [=](const FxValue &value) {
			raw->setValue(value.point());
		};
		row.dim = [=](bool dimmed) { raw->setDimmed(dimmed); };
		widget = std::move(point);
	} break;
	case FxParamKind::Seed: {
		auto seed = object_ptr<SeedRow>(
			this,
			param.name.now(),
			value.integer());
		const auto raw = seed.data();
		raw->chosen() | rpl::on_next([=](int next) {
			report(FxValue::Integer(next), true);
		}, raw->lifetime());
		row.set = [=](const FxValue &value) {
			raw->setValue(value.integer());
		};
		row.dim = [=](bool dimmed) { raw->setDimmed(dimmed); };
		widget = std::move(seed);
	} break;
	case FxParamKind::Custom: {
		const auto editor = FindFxCustomEditor(param.customType);
		if (!editor || !editor->create) {
			break;
		}
		const auto stream = lifetime().make_state<
			rpl::event_stream<QByteArray>>();
		auto created = editor->create(this, FxCustomEditorArgs{
			.value = value.data(),
			.values = stream->events(),
			.changed = [=](QByteArray data, bool finished) {
				report(FxValue::Data(std::move(data)), finished);
			},
			.controller = _controller,
			.layerId = _layerId,
			.fxUid = _fxUid,
			.paramId = id,
			.dimmed = _dimmed.value(),
		});
		if (!created) {
			break;
		}
		auto dimmer = object_ptr<DimmedEditor>(this, std::move(created));
		const auto raw = dimmer.data();
		row.set = [=](const FxValue &value) {
			stream->fire_copy(value.data());
		};
		row.dim = [=](bool dimmed) { raw->setDimmed(dimmed); };
		widget = std::move(dimmer);
	} break;
	}
	if (!widget) {
		return;
	}
	row.wrap = add(object_ptr<Ui::SlideWrap<Ui::RpWidget>>(
		this,
		std::move(widget)));
	_rows.push_back(std::move(row));
}

void ParamsPanel::edited(
		const QByteArray &id,
		FxValue value,
		bool finished) {
	_values.set(id, value);
	refreshVisibility(anim::type::normal);
	if (_changed) {
		_changed(id, std::move(value), finished);
	}
}

void ParamsPanel::showValues(const FxParams &values) {
	for (const auto &param : _params) {
		const auto value = param.normalized(values.value(param.id));
		if (value == _values.value(param.id)) {
			continue;
		}
		_values.set(param.id, value);
		for (const auto &row : _rows) {
			if (row.id == param.id && row.set) {
				row.set(value);
			}
		}
	}
	refreshVisibility(anim::type::instant);
}

void ParamsPanel::refreshVisibility(anim::type animated) {
	for (const auto &row : _rows) {
		if (!row.wrap) {
			if (row.visible) {
				row.visible(_values);
			}
			continue;
		}
		row.wrap->toggle(!row.visible || row.visible(_values), animated);
	}
}

struct StackEntry {
	uint64 uid = 0;
	QByteArray id;

	friend bool operator==(const StackEntry &a, const StackEntry &b) = default;
};

// The effects of one layer.
class FxStack final : public Ui::VerticalLayout {
public:
	FxStack(
		QWidget *parent,
		not_null<Controller*> controller,
		rpl::producer<LayerId> layer);

	// The top of the last card in the coordinates of the stack, -1 if
	// there are no cards.
	[[nodiscard]] int lastCardTop() const;

private:
	void refresh();
	void rebuild(const Layer *layer);
	void addCard(const Layer &layer, int index);
	void showCardMenu(uint64 uid, QPoint globalPosition);

	const not_null<Controller*> _controller;
	LayerId _layerId = 0;
	std::vector<StackEntry> _built;
	bool _builtOnce = false;
	SectionTitle *_title = nullptr;
	Ui::SlideWrap<Ui::FlatLabel> *_empty = nullptr;
	Ui::SlideWrap<Ui::FlatLabel> *_locked = nullptr;
	Ui::VerticalLayout *_list = nullptr;
	Veil *_listVeil = nullptr;
	PanelButton *_addButton = nullptr;
	rpl::event_stream<> _refreshCards;
	base::unique_qptr<Ui::PopupMenu> _menu;

};

FxStack::FxStack(
	QWidget *parent,
	not_null<Controller*> controller,
	rpl::producer<LayerId> layer)
: VerticalLayout(parent)
, _controller(controller) {
	_title = VerticalLayout::add(
		object_ptr<SectionTitle>(
			this,
			tr::lng_oblivion_photo_panel_effects()),
		RowMargins());
	_title->setAction(tr::lng_oblivion_photo_ui_effects_clear(), [=] {
		_controller->changeLayer(_layerId, [](Layer &layer) {
			layer.effects.clear();
		});
	});
	_empty = VerticalLayout::add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			this,
			object_ptr<Ui::FlatLabel>(
				this,
				tr::lng_oblivion_photo_panel_effects_empty(),
				HintLabelStyle()),
			RowMargins()));
	_locked = VerticalLayout::add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			this,
			object_ptr<Ui::FlatLabel>(
				this,
				tr::lng_oblivion_photo_panel_locked_about(),
				HintLabelStyle()),
			RowMargins()));
	_locked->toggle(false, anim::type::instant);
	_list = VerticalLayout::add(object_ptr<Ui::VerticalLayout>(this));
	// The effects of a locked layer are shown, but can't be changed.
	_listVeil = Ui::CreateChild<Veil>(
		this,
		st::groupCallMembersBg,
		1. - kDimmedOpacity);
	_list->geometryValue() | rpl::on_next([=](QRect geometry) {
		_listVeil->setGeometry(geometry);
	}, _listVeil->lifetime());
	_addButton = VerticalLayout::add(
		object_ptr<PanelButton>(
			this,
			tr::lng_oblivion_photo_panel_add_effect(),
			false),
		RowMargins(Px(kCardSkip)));
	_addButton->setClickedCallback([=] {
		const auto weak = base::make_weak(this);
		ShowAddFxMenu(
			this,
			_addButton->mapToGlobal(QPoint(0, _addButton->height())),
			[=](std::vector<FxInstance> stack) {
				if (const auto strong = weak.get()) {
					AppendLayerFx(
						strong->_controller,
						strong->_layerId,
						std::move(stack));
				}
			});
	});

	std::move(layer) | rpl::on_next([=](LayerId id) {
		_layerId = id;
		refresh();
	}, lifetime());
	_controller->documentChanges() | rpl::on_next([=] {
		refresh();
	}, lifetime());
}

int FxStack::lastCardTop() const {
	const auto count = _list->count();
	return count
		? (_list->y() + _list->widgetAt(count - 1)->y())
		: -1;
}

void FxStack::refresh() {
	const auto layer = _controller->hasDocument()
		? _controller->document().find(_layerId)
		: nullptr;
	auto entries = std::vector<StackEntry>();
	if (layer) {
		for (const auto &instance : layer->effects) {
			entries.push_back({ instance.uid, instance.id });
		}
	}
	if (!_builtOnce || entries != _built) {
		_builtOnce = true;
		_built = std::move(entries);
		rebuild(layer);
	}
	const auto locked = layer && layer->locked;
	const auto fixed = locked && !layer->effects.empty();
	_title->setActionVisible(layer && !locked && !layer->effects.empty());
	_addButton->setAvailable(layer && !locked);
	_locked->toggle(locked, anim::type::instant);
	_list->setAttribute(Qt::WA_TransparentForMouseEvents, fixed);
	_listVeil->setVisible(fixed);
	if (fixed) {
		_listVeil->raise();
	}
	_refreshCards.fire({});
}

void FxStack::rebuild(const Layer *layer) {
	_list->clear();
	const auto count = layer ? int(layer->effects.size()) : 0;
	for (auto i = 0; i != count; ++i) {
		addCard(*layer, i);
	}
	_empty->toggle(!count, anim::type::instant);
	_list->resizeToWidth(width());
}

void FxStack::addCard(const Layer &layer, int index) {
	const auto &instance = layer.effects[index];
	const auto uid = instance.uid;
	const auto layerId = layer.id;
	const auto descriptor = FindFx(instance.id);
	const auto card = _list->add(
		object_ptr<Ui::VerticalLayout>(_list),
		style::margins(
			Px(kPadding),
			index ? Px(kCardSkip) : Px(kSkip),
			Px(kPadding),
			0));
	MarkAsCard(card);
	card->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(card);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::groupCallBg);
		const auto radius = Px(kCardRadius);
		p.drawRoundedRect(card->rect(), radius, radius);
	}, card->lifetime());

	const auto controller = _controller;
	const auto current = [=]() -> const FxInstance* {
		return controller->hasDocument()
			? LayerFx(controller->document(), layerId, uid)
			: nullptr;
	};
	const auto header = card->add(object_ptr<SwitchHeader>(
		card,
		descriptor
			? descriptor->name.now()
			: tr::lng_oblivion_photo_panel_effect_unknown(tr::now),
		instance.enabled));
	header->toggles() | rpl::on_next([=](bool enabled) {
		controller->changeFx(layerId, uid, [=](FxInstance &instance) {
			instance.enabled = enabled;
		});
	}, header->lifetime());
	header->menuRequests() | rpl::on_next([=](QPoint position) {
		showCardMenu(uid, position);
	}, header->lifetime());

	const auto enabled = card->lifetime().make_state<rpl::variable<bool>>(
		instance.enabled);
	const auto values = card->lifetime().make_state<
		rpl::event_stream<FxParams>>();
	if (descriptor && !descriptor->params.empty()) {
		card->add(
			object_ptr<ParamsPanel>(card, ParamsPanelArgs{
				.params = descriptor->params,
				.values = instance.params,
				.updates = values->events(),
				.changed = [=](
						const QByteArray &id,
						FxValue value,
						bool finished) {
					controller->setFxParam(
						layerId,
						uid,
						id,
						std::move(value),
						finished);
				},
				.controller = controller,
				.layerId = layerId,
				.fxUid = uid,
				.commitOnRelease = (descriptor->flags & kFxSlow) != 0,
				.dimmed = enabled->value() | rpl::map(!rpl::mappers::_1),
			}),
			style::margins(Px(kCardPadding), 0, Px(kCardPadding), 0));
	}
	card->add(object_ptr<Ui::FixedHeightWidget>(card, Px(kSkip)));

	_refreshCards.events() | rpl::on_next([=] {
		if (const auto now = current()) {
			header->setChecked(now->enabled, anim::type::normal);
			*enabled = now->enabled;
			values->fire_copy(now->params);
		}
	}, card->lifetime());
}

void FxStack::showCardMenu(uint64 uid, QPoint globalPosition) {
	const auto layer = _controller->document().find(_layerId);
	if (!layer) {
		return;
	}
	const auto i = ranges::find(layer->effects, uid, &FxInstance::uid);
	if (i == end(layer->effects)) {
		return;
	}
	const auto index = int(i - begin(layer->effects));
	const auto count = int(layer->effects.size());
	const auto layerId = _layerId;
	const auto controller = _controller;
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::groupCallPopupMenu);
	if (index > 0) {
		_menu->addAction(tr::lng_oblivion_photo_ui_effect_up(tr::now), [=] {
			controller->change([=](Document &document) {
				MoveLayerFx(document, layerId, uid, index - 1);
			});
		});
	}
	if (index + 1 < count) {
		_menu->addAction(tr::lng_oblivion_photo_ui_effect_down(tr::now), [=] {
			controller->change([=](Document &document) {
				MoveLayerFx(document, layerId, uid, index + 1);
			});
		});
	}
	_menu->addAction(tr::lng_oblivion_photo_ui_effect_reset(tr::now), [=] {
		controller->changeFx(layerId, uid, [](FxInstance &instance) {
			if (const auto descriptor = FindFx(instance.id)) {
				instance.params = DefaultFxParams(*descriptor);
			}
		});
	});
	if (count < kMaxStackEffects) {
		_menu->addAction(
			tr::lng_oblivion_photo_ui_effect_duplicate(tr::now),
			[=] {
				controller->change([=](Document &document) {
					if (const auto now = LayerFx(document, layerId, uid)) {
						auto copy = *now;
						AddLayerFx(
							document,
							layerId,
							std::move(copy),
							index + 1);
					}
				});
			});
	}
	_menu->addAction(tr::lng_oblivion_photo_ui_effect_remove(tr::now), [=] {
		controller->change([=](Document &document) {
			RemoveLayerFx(document, layerId, uid);
		});
	});
	_menu->popup(globalPosition);
}

// The Layer tab: what the active layer is and how it is blended, the
// sections other modules registered for it, its effects.
class LayerPanel final : public Ui::VerticalLayout {
public:
	LayerPanel(QWidget *parent, not_null<Controller*> controller);

	// The top of the last effect card in the coordinates of the panel,
	// -1 while there is none to show.
	[[nodiscard]] int lastFxTop() const;

private:
	struct Section {
		const PanelDescriptor *descriptor = nullptr;
		Ui::SlideWrap<Ui::VerticalLayout> *wrap = nullptr;
		bool created = false;
	};

	void refresh();

	const not_null<Controller*> _controller;
	rpl::variable<QString> _name;
	Ui::SlideWrap<Ui::FlatLabel> *_none = nullptr;
	Ui::SlideWrap<Ui::VerticalLayout> *_content = nullptr;
	ValueSlider *_opacity = nullptr;
	ChoiceRow *_blend = nullptr;
	FxStack *_stack = nullptr;
	std::vector<Section> _sections;

};

LayerPanel::LayerPanel(QWidget *parent, not_null<Controller*> controller)
: VerticalLayout(parent)
, _controller(controller) {
	_none = add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			this,
			object_ptr<Ui::FlatLabel>(
				this,
				tr::lng_oblivion_photo_panel_no_layer(),
				HintLabelStyle()),
			RowMargins(Px(kSkip))));
	_content = add(object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
		this,
		object_ptr<Ui::VerticalLayout>(this)));
	const auto content = _content->entity();

	content->add(
		object_ptr<SectionTitle>(content, _name.value()),
		RowMargins());
	_opacity = content->add(
		object_ptr<ValueSlider>(content, SliderArgs{
			.label = tr::lng_oblivion_photo_panel_opacity(),
			.min = 0,
			.max = 100,
			.defaultValue = 100,
			.value = 100,
			.format = [](int value) {
				return QString::number(value) + '%';
			},
		}),
		RowMargins());
	_opacity->changes() | rpl::on_next([=](SliderChange change) {
		_controller->changeLayer(
			_controller->activeLayerId(),
			[=](Layer &layer) { layer.opacity = change.value / 100.; },
			change.finished);
	}, _opacity->lifetime());

	auto names = std::vector<QString>();
	for (const auto mode : BlendModes()) {
		names.push_back(BlendModeName(mode));
	}
	_blend = content->add(
		object_ptr<ChoiceRow>(
			content,
			tr::lng_oblivion_photo_panel_blend(tr::now),
			std::move(names),
			0),
		RowMargins());
	_blend->chosen() | rpl::on_next([=](int index) {
		const auto &modes = BlendModes();
		if (index < 0 || index >= int(modes.size())) {
			return;
		}
		const auto mode = modes[index];
		_controller->changeLayer(
			_controller->activeLayerId(),
			[=](Layer &layer) { layer.blend = mode; });
	}, _blend->lifetime());

	for (const auto descriptor : AllPanels()) {
		if (descriptor->slot != PanelSlot::LayerProperties) {
			continue;
		}
		const auto wrap = content->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				content,
				object_ptr<Ui::VerticalLayout>(content)));
		wrap->toggle(false, anim::type::instant);
		_sections.push_back({ descriptor, wrap });
	}

	_stack = content->add(
		object_ptr<FxStack>(
			content,
			_controller,
			_controller->activeLayerValue()),
		style::margins(0, Px(kSkip), 0, 0));

	rpl::merge(
		_controller->documentChanges(),
		_controller->activeLayerValue() | rpl::to_empty
	) | rpl::on_next([=] {
		refresh();
	}, lifetime());
	refresh();
}

int LayerPanel::lastFxTop() const {
	const auto top = _stack->lastCardTop();
	return (top < 0 || !_content->toggled())
		? -1
		: _stack->mapTo(this, QPoint(0, top)).y();
}

void LayerPanel::refresh() {
	const auto layer = _controller->hasDocument()
		? _controller->activeLayer()
		: nullptr;
	_none->toggle(!layer, anim::type::instant);
	_content->toggle(layer != nullptr, anim::type::instant);
	if (!layer) {
		return;
	}
	_name = layer->name;
	_opacity->setValue(int(std::lround(layer->opacity * 100.)));
	const auto &modes = BlendModes();
	_blend->setValue(int(ranges::find(modes, layer->blend) - begin(modes)));
	for (auto &section : _sections) {
		const auto descriptor = section.descriptor;
		const auto shown = !descriptor->visible
			|| descriptor->visible(_controller.get());
		if (shown && !section.created) {
			section.created = true;
			const auto inner = section.wrap->entity();
			if (!descriptor->title.empty()) {
				inner->add(
					object_ptr<SectionTitle>(
						inner,
						rpl::single(descriptor->title.now())),
					RowMargins(Px(kSkip)));
			}
			if (auto widget = descriptor->create(inner, _controller)) {
				inner->add(std::move(widget));
			}
			inner->resizeToWidth(width());
		}
		section.wrap->toggle(shown, anim::type::instant);
	}
}

// What the layers slot shows while no module registered its own panel:
// the layers from the top one down, a click makes a layer active, the
// eye hides and shows it.
class DefaultLayersPanel final : public Ui::RpWidget {
public:
	DefaultLayersPanel(QWidget *parent, not_null<Controller*> controller);

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	class List;

	const not_null<Controller*> _controller;
	Ui::ScrollArea *_scroll = nullptr;
	List *_list = nullptr;

};

class DefaultLayersPanel::List final : public Ui::RpWidget {
public:
	List(QWidget *parent, not_null<Controller*> controller);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	[[nodiscard]] int rowAt(QPoint point) const;
	[[nodiscard]] const Layer *layerAt(int row) const;
	void refreshThumbnails();

	const not_null<Controller*> _controller;
	base::flat_map<LayerId, QImage> _thumbnails;
	uint64 _thumbnailsRevision = 0;
	int _over = -1;

};

DefaultLayersPanel::List::List(
	QWidget *parent,
	not_null<Controller*> controller)
: RpWidget(parent)
, _controller(controller) {
	setMouseTracking(true);
	rpl::merge(
		_controller->documentChanges(),
		_controller->activeLayerValue() | rpl::to_empty
	) | rpl::on_next([=] {
		resizeToWidth(width());
		refreshThumbnails();
		update();
	}, lifetime());
	refreshThumbnails();
}

void DefaultLayersPanel::List::refreshThumbnails() {
	if (!_controller->hasDocument()
		|| _thumbnailsRevision == _controller->layersRevision()) {
		return;
	}
	_thumbnailsRevision = _controller->layersRevision();
	const auto ratio = style::DevicePixelRatio();
	const auto side = Px(kLayerThumb) * ratio;
	for (const auto &layer : _controller->document().layers) {
		const auto id = layer.id;
		_controller->requestLayerThumbnail(
			id,
			QSize(side, side),
			crl::guard(this, [=](QImage image) {
				image.setDevicePixelRatio(ratio);
				_thumbnails[id] = std::move(image);
				update();
			}));
	}
}

int DefaultLayersPanel::List::resizeGetHeight(int newWidth) {
	const auto count = _controller->hasDocument()
		? int(_controller->document().layers.size())
		: 0;
	return count * Px(kLayerRowHeight);
}

const Layer *DefaultLayersPanel::List::layerAt(int row) const {
	if (!_controller->hasDocument()) {
		return nullptr;
	}
	const auto &layers = _controller->document().layers;
	const auto count = int(layers.size());
	return (row >= 0 && row < count) ? &layers[count - 1 - row] : nullptr;
}

int DefaultLayersPanel::List::rowAt(QPoint point) const {
	return rect().contains(point) ? (point.y() / Px(kLayerRowHeight)) : -1;
}

void DefaultLayersPanel::List::mouseMoveEvent(QMouseEvent *e) {
	const auto over = rowAt(e->pos());
	if (_over != over) {
		_over = over;
		setCursor(layerAt(over) ? style::cur_pointer : style::cur_default);
		update();
	}
}

void DefaultLayersPanel::List::leaveEventHook(QEvent *e) {
	if (_over >= 0) {
		_over = -1;
		update();
	}
}

void DefaultLayersPanel::List::mousePressEvent(QMouseEvent *e) {
	const auto layer = layerAt(rowAt(e->pos()));
	if (!layer || e->button() != Qt::LeftButton) {
		return;
	}
	const auto id = layer->id;
	if (e->pos().x() >= width() - Px(kLayerEyeWidth)) {
		_controller->changeLayer(id, [](Layer &layer) {
			layer.visible = !layer.visible;
		});
	} else {
		_controller->setActiveLayer(id);
	}
}

void DefaultLayersPanel::List::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	if (!_controller->hasDocument()) {
		return;
	}
	const auto &layers = _controller->document().layers;
	const auto count = int(layers.size());
	const auto rowHeight = Px(kLayerRowHeight);
	const auto active = _controller->activeLayerId();
	const auto thumb = Px(kLayerThumb);
	const auto left = Px(kPadding);
	const auto eye = Px(kLayerEyeWidth);
	const auto &font = st::normalFont;
	for (auto row = 0; row != count; ++row) {
		const auto &layer = layers[count - 1 - row];
		const auto rect = QRect(0, row * rowHeight, width(), rowHeight);
		if (!rect.intersects(e->rect())) {
			continue;
		}
		const auto selected = (layer.id == active);
		if (selected || row == _over) {
			p.setPen(Qt::NoPen);
			p.setBrush(selected
				? anim::with_alpha(AccentColor(), 0.16)
				: st::groupCallMembersBgOver->c);
			p.drawRoundedRect(
				rect.marginsRemoved(QMargins(Px(6), Px(2), Px(6), Px(2))),
				Px(kRowRadius),
				Px(kRowRadius));
		}
		const auto alpha = layer.visible ? 1. : kDimmedOpacity;
		const auto thumbRect = QRect(
			left,
			rect.y() + (rowHeight - thumb) / 2,
			thumb,
			thumb);
		p.setPen(Qt::NoPen);
		p.setBrush(st::groupCallBg);
		p.drawRoundedRect(thumbRect, Px(4), Px(4));
		const auto i = _thumbnails.find(layer.id);
		if (i != end(_thumbnails) && !i->second.isNull()) {
			const auto size = i->second.size() / i->second.devicePixelRatio();
			p.setOpacity(alpha);
			p.drawImage(
				QPointF(
					thumbRect.x() + (thumb - size.width()) / 2.,
					thumbRect.y() + (thumb - size.height()) / 2.),
				i->second);
			p.setOpacity(1.);
		}
		const auto textLeft = left + thumb + Px(kSkip) + Px(2);
		const auto textWidth = std::max(width() - eye - textLeft, 0);
		p.setFont(font);
		p.setPen(anim::with_alpha(
			selected ? AccentColor() : TextColor(),
			alpha));
		p.drawText(
			QRect(textLeft, rect.y(), textWidth, rowHeight),
			Qt::AlignLeft | Qt::AlignVCenter,
			font->elided(layer.name, textWidth));

		// The eye: an outline with a pupil, crossed out when hidden.
		const auto center = QPointF(
			width() - eye / 2.,
			rect.y() + rowHeight / 2.);
		const auto unit = Px(24) / 24.;
		auto pen = QPen(
			anim::with_alpha(SubTextColor(), layer.visible ? 1. : 0.6),
			1.5 * unit);
		pen.setCapStyle(Qt::RoundCap);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		auto shape = QPainterPath();
		shape.moveTo(center + QPointF(-8. * unit, 0.));
		shape.quadTo(
			center + QPointF(0., -7. * unit),
			center + QPointF(8. * unit, 0.));
		shape.quadTo(
			center + QPointF(0., 7. * unit),
			center + QPointF(-8. * unit, 0.));
		p.drawPath(shape);
		if (layer.visible) {
			p.setBrush(SubTextColor());
			p.setPen(Qt::NoPen);
			p.drawEllipse(center, 2.2 * unit, 2.2 * unit);
		} else {
			p.drawLine(
				center + QPointF(-6. * unit, 6. * unit),
				center + QPointF(6. * unit, -6. * unit));
		}
	}
}

DefaultLayersPanel::DefaultLayersPanel(
	QWidget *parent,
	not_null<Controller*> controller)
: RpWidget(parent)
, _controller(controller) {
	_scroll = Ui::CreateChild<Ui::ScrollArea>(this, PanelScrollStyle());
	_list = _scroll->setOwnedWidget(
		object_ptr<List>(_scroll, controller)).data();
	_scroll->show();
}

void DefaultLayersPanel::resizeEvent(QResizeEvent *e) {
	const auto top = Px(kLayersHeaderHeight);
	_scroll->setGeometry(0, top, width(), std::max(height() - top, 0));
	_list->resizeToWidth(width());
}

void DefaultLayersPanel::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto &font = SmallSemiboldFont();
	p.setFont(font);
	p.setPen(SubTextColor());
	p.drawText(
		QRect(
			Px(kPadding),
			0,
			std::max(width() - 2 * Px(kPadding), 0),
			Px(kLayersHeaderHeight)),
		Qt::AlignLeft | Qt::AlignVCenter,
		tr::lng_oblivion_photo_layers_title(tr::now));
}

[[nodiscard]] std::vector<FxParam> SceneParams() {
	return {
		FxFloat(
			"amount",
			tr::lng_oblivion_photo_ui_intensity,
			0.,
			100.,
			65.,
			0,
			u"%"_q),
		FxFloat(
			"balance",
			tr::lng_oblivion_photo_layers_tf_skew,
			-1.,
			1.,
			0.25,
			2),
		FxAngle(
			"angle",
			FxText::Custom([] { return EffectParamName(EffectParam::Angle); }),
			30.),
		FxBool(
			"invert",
			FxText::Custom([] { return EffectName(EffectType::Invert); }),
			true),
		FxChoice(
			"mode",
			tr::lng_oblivion_photo_panel_blend,
			{
				tr::lng_oblivion_photo_panel_blend_normal,
				tr::lng_oblivion_photo_panel_blend_multiply,
				tr::lng_oblivion_photo_panel_blend_screen,
			},
			1),
		FxColor(
			"tint",
			FxText::Custom([] { return AdjustName(Adjust::Tint); }),
			QColor(0x1E, 0x88, 0xE5)),
		FxPoint(
			"center",
			FxText::Custom([] {
				return EffectParamName(EffectParam::Position);
			})),
		FxSeed(),
	};
}

// An effect whose Point parameter is shown with the default values.
[[nodiscard]] const FxDescriptor *PointSceneFx() {
	const auto fits = [](const FxDescriptor *descriptor) {
		if (!descriptor || (descriptor->flags & kFxHidden)) {
			return false;
		}
		const auto values = DefaultFxParams(*descriptor);
		return ranges::any_of(descriptor->params, [&](const FxParam &param) {
			return (param.kind == FxParamKind::Point)
				&& (!param.visible || param.visible(values));
		});
	};
	const auto preferred = FindFx("blur.tilt_shift");
	if (fits(preferred)) {
		return preferred;
	}
	for (const auto descriptor : AllFx()) {
		if (fits(descriptor)) {
			return descriptor;
		}
	}
	return nullptr;
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	const auto width = Px(340);
	// A Point parameter while it is picked on the photo: the row offers
	// "Done", the handle of the point is over the canvas.
	RegisterEditorScene({
		.name = u"photo_panel_point_pick"_q,
		.document = [] {
			auto document = DocumentFromImage(
				SampleSceneImage(),
				EditState(),
				tr::lng_oblivion_photo_panel_layer_photo(tr::now));
			const auto fx = PointSceneFx();
			if (fx && !document.empty()) {
				AddLayerFx(
					document,
					document.layers.front().id,
					MakeFx(fx->id));
			}
			return document;
		},
		.tab = PhotoEditorTab::Layer,
		.prepare = [](not_null<Controller*> controller) {
			if (const auto row = PointRow::FindFor(controller)) {
				row->startPicking();
			}
		},
	});
	RegisterPanelScene({
		.name = u"photo_panel_params"_q,
		.size = QSize(width, 0),
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			auto result = object_ptr<Ui::VerticalLayout>(parent);
			// Some of them changed: the values in the accent color, the
			// fill of a two-sided slider on the other side of its mark.
			auto values = FxParams();
			values.set("amount", FxValue::Number(40.));
			values.set("angle", FxValue::Number(-45.));
			values.set("seed", FxValue::Integer(4821));
			result->add(
				CreateParamsPanel(result.data(), ParamsPanelArgs{
					.params = SceneParams(),
					.values = std::move(values),
					.controller = controller,
				}),
				RowMargins(Px(kSkip)));
			result->add(object_ptr<Ui::FixedHeightWidget>(
				result.data(),
				Px(kPadding)));
			return object_ptr<Ui::RpWidget>(std::move(result));
		},
	});
	RegisterPanelScene({
		.name = u"photo_panel_layer"_q,
		.size = QSize(width, 0),
		.prepare = [](not_null<Controller*> controller) {
			const auto &layers = controller->document().layers;
			if (layers.size() > 1) {
				controller->setActiveLayer(layers[1].id);
			}
		},
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			auto result = object_ptr<Ui::VerticalLayout>(parent);
			result->add(CreateLayerPanel(result.data(), controller));
			result->add(object_ptr<Ui::FixedHeightWidget>(
				result.data(),
				Px(kPadding)));
			return object_ptr<Ui::RpWidget>(std::move(result));
		},
	});
	// A locked layer: its effects are shown faded under the hint.
	RegisterPanelScene({
		.name = u"photo_panel_layer_locked"_q,
		.size = QSize(width, 0),
		.prepare = [](not_null<Controller*> controller) {
			const auto &layers = controller->document().layers;
			if (layers.size() > 1) {
				const auto id = layers[1].id;
				controller->setActiveLayer(id);
				controller->changeLayer(id, [](Layer &layer) {
					layer.locked = true;
				});
			}
		},
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			auto result = object_ptr<Ui::VerticalLayout>(parent);
			result->add(CreateLayerPanel(result.data(), controller));
			result->add(object_ptr<Ui::FixedHeightWidget>(
				result.data(),
				Px(kPadding)));
			return object_ptr<Ui::RpWidget>(std::move(result));
		},
	});
	RegisterPanelScene({
		.name = u"photo_panel_layers"_q,
		.size = QSize(width, Px(220)),
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			return CreateLayersPanel(parent, controller);
		},
	});
});

} // namespace

object_ptr<Ui::RpWidget> CreateParamsPanel(
		not_null<QWidget*> parent,
		ParamsPanelArgs &&args) {
	return object_ptr<ParamsPanel>(parent, std::move(args));
}

QString FormatParamValue(const FxParam &param, const FxValue &value) {
	const auto normalized = param.normalized(value);
	switch (param.kind) {
	case FxParamKind::Float:
	case FxParamKind::Int:
	case FxParamKind::Angle:
	case FxParamKind::Seed:
		return NumberText(param, normalized.number());
	case FxParamKind::Bool:
		return normalized.boolean()
			? tr::lng_oblivion_photo_panel_on(tr::now)
			: tr::lng_oblivion_photo_panel_off(tr::now);
	case FxParamKind::Choice: {
		const auto index = normalized.integer();
		return (index >= 0 && index < int(param.choices.size()))
			? param.choices[index].now()
			: QString();
	}
	case FxParamKind::Color:
		return normalized.color().name(QColor::HexRgb).toUpper();
	case FxParamKind::Point: {
		const auto point = normalized.point();
		return u"%1%, %2%"_q.arg(
			int(std::lround(point.x() * 100.))
		).arg(int(std::lround(point.y() * 100.)));
	}
	case FxParamKind::Custom: break;
	}
	return QString();
}

void ShowAddFxMenu(
		not_null<QWidget*> parent,
		QPoint globalPosition,
		Fn<void(std::vector<FxInstance> stack)> chosen,
		std::vector<FxGroup> groups,
		bool flat) {
	if (!chosen) {
		return;
	}
	// One menu at a time, the previous one (hidden long ago) is not kept
	// till its parent dies.
	static auto Last = QPointer<Ui::PopupMenu>();
	if (const auto previous = Last.data()) {
		previous->hide();
		previous->deleteLater();
	}
	const auto menu = Ui::CreateChild<Ui::PopupMenu>(
		parent.get(),
		st::groupCallPopupMenu);
	Last = menu;
	const auto &presets = AllFxPresets();
	// The one-click presets of a group go first: they are what a person
	// who wants "that old camera look" is after, the separate effects
	// under them are for building a look by hand.
	const auto fill = [&](not_null<Ui::PopupMenu*> to, FxGroup group) {
		auto count = 0;
		for (const auto preset : presets) {
			if (preset->group != group) {
				continue;
			}
			const auto stack = preset->stack;
			to->addAction(preset->name.now(), [=] {
				chosen(stack);
			});
			++count;
		}
		auto separated = !count;
		for (const auto descriptor : FxInGroup(group)) {
			if (descriptor->flags & kFxHidden) {
				continue;
			}
			if (!separated) {
				separated = true;
				to->addSeparator();
			}
			const auto id = descriptor->id;
			to->addAction(descriptor->name.now(), [=] {
				chosen({ MakeFx(id) });
			});
			++count;
		}
		return count;
	};
	const auto has = [&](FxGroup group) {
		if (ranges::any_of(presets, [&](const FxPreset *preset) {
			return preset->group == group;
		})) {
			return true;
		}
		return ranges::any_of(FxInGroup(group), [](const FxDescriptor *fx) {
			return !(fx->flags & kFxHidden);
		});
	};
	auto added = 0;
	for (const auto group : FxGroups()) {
		if ((!groups.empty() && !ranges::contains(groups, group))
			|| !has(group)) {
			continue;
		}
		if (flat) {
			if (added) {
				menu->addSeparator();
			}
			fill(menu, group);
		} else {
			auto submenu = std::make_unique<Ui::PopupMenu>(
				menu,
				st::groupCallPopupMenu);
			fill(submenu.get(), group);
			menu->addAction(FxGroupName(group), std::move(submenu));
		}
		++added;
	}
	if (!added) {
		Last = nullptr;
		delete menu;
		return;
	}
	menu->popup(globalPosition);
}

bool AppendLayerFx(
		not_null<Controller*> controller,
		LayerId id,
		std::vector<FxInstance> stack) {
	if (!controller->hasDocument() || stack.empty()) {
		return false;
	}
	const auto layer = controller->document().find(id);
	if (!layer) {
		controller->showToast(tr::lng_oblivion_photo_panel_no_layer(tr::now));
		return false;
	} else if (layer->locked) {
		controller->showToast(
			tr::lng_oblivion_photo_panel_layer_locked(tr::now));
		return false;
	} else if (layer->effects.size() + stack.size() > kMaxStackEffects) {
		controller->showToast(tr::lng_oblivion_photo_ui_effects_limit(
			tr::now,
			lt_max,
			QString::number(kMaxStackEffects)));
		return false;
	}
	controller->change([&](Document &document) {
		for (auto &instance : stack) {
			AddLayerFx(document, id, std::move(instance));
		}
	});
	return true;
}

object_ptr<Ui::RpWidget> CreateFxStackPanel(
		not_null<QWidget*> parent,
		not_null<Controller*> controller,
		rpl::producer<LayerId> layer) {
	return object_ptr<FxStack>(parent, controller, std::move(layer));
}

object_ptr<Ui::RpWidget> CreateLayerPanel(
		not_null<QWidget*> parent,
		not_null<Controller*> controller) {
	return object_ptr<LayerPanel>(parent, controller);
}

int LayerPanelLastFxTop(not_null<Ui::RpWidget*> layerPanel) {
	return static_cast<LayerPanel*>(layerPanel.get())->lastFxTop();
}

object_ptr<Ui::RpWidget> CreateLayersPanel(
		not_null<QWidget*> parent,
		not_null<Controller*> controller) {
	auto chosen = (const PanelDescriptor*)(nullptr);
	for (const auto descriptor : AllPanels()) {
		if (descriptor->slot == PanelSlot::Layers) {
			chosen = descriptor;
		}
	}
	if (chosen) {
		if (auto result = chosen->create(parent, controller)) {
			return result;
		}
	}
	return object_ptr<DefaultLayersPanel>(parent, controller);
}

void ShowColorPickerBox(
		std::shared_ptr<Ui::Show> show,
		QColor color,
		Fn<void(QColor color)> done) {
	if (!show || !show->valid()) {
		return;
	}
	if (!color.isValid()) {
		color = QColor(255, 255, 255);
	}
	const auto alpha = color.alpha();
	color.setAlpha(255);
	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_oblivion_photo_ui_color_title());
		const auto editor = box->addRow(
			object_ptr<ColorEditor>(box, ColorEditor::Mode::HSL, color),
			style::margins());
		box->setWidth(editor->width());
		const auto save = [=] {
			auto result = editor->color();
			result.setAlpha(alpha);
			box->closeBox();
			if (done) {
				done(result);
			}
		};
		editor->submitRequests() | rpl::on_next(save, editor->lifetime());
		box->setFocusCallback([=] { editor->setInnerFocus(); });
		box->addButton(tr::lng_settings_save(), save);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

} // namespace Oblivion::Photo
