/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_photo_fx_adjust.h"

#include "lang/lang_keys.h"
#include "oblivion/oblivion_photo_editor_controls.h"
#include "oblivion/oblivion_photo_panels.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/effects/animation_value.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_basic.h"
#include "styles/style_calls.h"

#include <QtGui/QPainterPath>
#include <QtWidgets/QApplication>

// Photo editor: the widgets that edit the custom parameters of the
// adjustments of oblivion_photo_fx_adjust.h (the tone curve, the table of
// color ranges, the color grading wheels) and the snapshot scenes of the
// adjustments. The widgets are registered as custom parameter editors,
// the generic parameter panel of oblivion_photo_panels.h creates them.
namespace Oblivion::Photo {
namespace {

using namespace EditorUi;

constexpr auto kPi = 3.14159265358979323846;

constexpr auto kTopRowHeight = 38;
constexpr auto kChannelSize = 18;
constexpr auto kChannelSkip = 10;
constexpr auto kChannelRing = 3;
constexpr auto kChannelTop = 2;
constexpr auto kChannelMark = 4;
// The graph takes the whole width of an effect card (284), like the row
// of the channels above it and the hint below it. Only a much wider panel
// (the narrow layout of the editor) gets a centered graph.
constexpr auto kGraphMaxSide = 300;
constexpr auto kGraphRadius = 8;
constexpr auto kGraphInset = 10;
constexpr auto kPointRadius = 5;
constexpr auto kPointGrab = 12;
constexpr auto kPointSnap = 16;
constexpr auto kReadoutSkip = 4;
constexpr auto kReadoutPadding = 6;
constexpr auto kReadoutRadius = 4;
constexpr auto kHintSkip = 8;
constexpr auto kBottomSkip = 4;
constexpr auto kHistogramBins = 64;
constexpr auto kHistogramSide = 256.;

constexpr auto kSwatchSize = 22;
constexpr auto kSwatchRowHeight = 38;
constexpr auto kSwatchRing = 3;
constexpr auto kSwatchMark = 4;
constexpr auto kHslHueId = "hue";
constexpr auto kHslSaturationId = "saturation";
constexpr auto kHslLuminanceId = "luminance";

constexpr auto kWheelMaxSize = 104;
constexpr auto kWheelMargin = 8;
constexpr auto kWheelTop = 10;
constexpr auto kWheelTextSkip = 6;
constexpr auto kWheelHandle = 6;
constexpr auto kWheelSnap = 3; // Percent of the saturation.

// The cursor may shake this much during a click without starting a drag.
constexpr auto kDragSlop = 3;

// Clicks in the curve and in the wheels. A click that changed something
// (put a point on the curve, moved the dot of a wheel) is reported at once
// as a live change and not as a finished one: the second click of a double
// click takes it back, so it must not become an undo step of its own. The
// editor makes a step of a live change after a short pause, the next press
// in the widget or the cursor leaving it makes it right away. Only a real
// drag is finished by the release of the button.
[[nodiscard]] bool DragStarted(QPoint pressed, QPoint now) {
	return (now - pressed).manhattanLength() >= Px(kDragSlop);
}

// A value that is being typed in a field inside of the widget is applied
// and the focus goes back to the editor.
void FinishTyping(not_null<QWidget*> widget) {
	const auto focused = QApplication::focusWidget();
	if (!focused || !widget->isAncestorOf(focused)) {
		return;
	}
	for (auto parent = widget->parentWidget()
		; parent
		; parent = parent->parentWidget()) {
		if (parent->focusPolicy() == Qt::StrongFocus) {
			parent->setFocus();
			return;
		}
	}
	focused->clearFocus();
}

[[nodiscard]] QColor TextColor() {
	return st::groupCallMembersFg->c;
}

[[nodiscard]] QColor SubTextColor() {
	return st::groupCallMemberNotJoinedStatus->c;
}

[[nodiscard]] QColor AccentColor() {
	return st::groupCallActiveFg->c;
}

[[nodiscard]] QColor ChannelColor(int channel) {
	switch (channel) {
	case 1: return QColor(240, 86, 80);
	case 2: return QColor(92, 200, 100);
	case 3: return QColor(84, 134, 245);
	}
	return QColor(236, 236, 236);
}

// The circles of the channels with the place of the selection ring
// around them. The texts of the row are centered on the circles.
[[nodiscard]] int ChannelsHeight() {
	return 2 * (Px(kChannelRing) + Px(kChannelTop)) + Px(kChannelSize);
}

[[nodiscard]] QString ChannelName(int channel) {
	switch (channel) {
	case 1: return tr::lng_oblivion_photo_adj_red(tr::now);
	case 2: return tr::lng_oblivion_photo_adj_green(tr::now);
	case 3: return tr::lng_oblivion_photo_adj_blue(tr::now);
	}
	return tr::lng_oblivion_photo_adj_curve_all(tr::now);
}

[[nodiscard]] QString RangeName(int range) {
	switch (HslRange(std::clamp(range, 0, kHslRanges - 1))) {
	case HslRange::Red: return tr::lng_oblivion_photo_adj_red(tr::now);
	case HslRange::Orange: return tr::lng_oblivion_photo_adj_orange(tr::now);
	case HslRange::Yellow: return tr::lng_oblivion_photo_adj_yellow(tr::now);
	case HslRange::Green: return tr::lng_oblivion_photo_adj_green(tr::now);
	case HslRange::Aqua: return tr::lng_oblivion_photo_adj_aqua(tr::now);
	case HslRange::Blue: return tr::lng_oblivion_photo_adj_blue(tr::now);
	case HslRange::Purple: return tr::lng_oblivion_photo_adj_purple(tr::now);
	case HslRange::Magenta:
		return tr::lng_oblivion_photo_adj_magenta(tr::now);
	}
	return QString();
}

[[nodiscard]] QString WheelName(int index) {
	switch (index) {
	case 0: return tr::lng_oblivion_photo_adj_shadows(tr::now);
	case 1: return tr::lng_oblivion_photo_adj_midtones(tr::now);
	}
	return tr::lng_oblivion_photo_adj_highlights(tr::now);
}

//
// The tone curve.
//

// How many pixels have each level, for the luminance and the three
// channels: what is shown behind the curve.
struct CurveHistogram {
	std::array<std::array<float, kHistogramBins>, kCurveChannels> values = {};
	bool valid = false;
};

[[nodiscard]] CurveHistogram ComputeHistogram(QImage image) {
	auto result = CurveHistogram();
	if (!FxPrepare(image)) {
		return result;
	}
	auto counts = std::array<
		std::array<int, kHistogramBins>,
		kCurveChannels>();
	auto total = 0;
	const auto bin = [](float value) {
		return std::clamp(
			int(value * kHistogramBins),
			0,
			kHistogramBins - 1);
	};
	for (auto y = 0; y != image.height(); ++y) {
		const auto line = FxRow(image, y);
		for (auto x = 0; x != image.width(); ++x) {
			if ((line[x] >> 24) < 128) {
				continue;
			}
			const auto color = FxUnpack(line[x]);
			++counts[0][bin(
				0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b)];
			++counts[1][bin(color.r)];
			++counts[2][bin(color.g)];
			++counts[3][bin(color.b)];
			++total;
		}
	}
	if (!total) {
		return result;
	}
	for (auto channel = 0; channel != kCurveChannels; ++channel) {
		// Clipped blacks and whites must not flatten everything else.
		auto highest = 1;
		for (auto i = 1; i + 1 != kHistogramBins; ++i) {
			highest = std::max(highest, counts[channel][i]);
		}
		for (auto i = 0; i != kHistogramBins; ++i) {
			result.values[channel][i] = std::sqrt(
				std::min(counts[channel][i] / float(highest), 1.f));
		}
	}
	result.valid = true;
	return result;
}

class CurveEditor final : public Ui::RpWidget {
public:
	CurveEditor(QWidget *parent, FxCustomEditorArgs &&args);

	// For the snapshot scenes.
	void selectChannel(int channel);
	void selectPoint(int index);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	[[nodiscard]] QRect channelRect(int index) const;
	[[nodiscard]] int channelAt(QPoint position) const;
	[[nodiscard]] QRect resetRect() const;
	[[nodiscard]] bool resetShown() const;
	[[nodiscard]] QRect graphRect() const;
	[[nodiscard]] QRectF plotRect() const;
	[[nodiscard]] QPointF pointPosition(const CurvePoint &point) const;
	[[nodiscard]] CurvePoint pointFromPosition(QPointF position) const;
	[[nodiscard]] int pointAt(QPointF position) const;
	[[nodiscard]] CurvePoints &points();
	[[nodiscard]] const CurvePoints &points() const;
	[[nodiscard]] QPainterPath curvePath(const CurvePoints &points) const;
	[[nodiscard]] QPainterPath histogramPath() const;

	void refreshHistogram();
	void showValue(const QByteArray &value);
	void report(bool finished);
	void commitClick();
	[[nodiscard]] int addPoint(QPointF position);
	bool movePoint(int index, QPointF position);
	void removePoint(int index);
	void updateOver(QPoint position);

	const Fn<void(QByteArray value, bool finished)> _changed;
	const base::weak_ptr<Controller> _controller;
	const LayerId _layerId = 0;
	const uint64 _fxUid = 0;
	ToneCurve _curve;
	CurveHistogram _histogram;
	uint64 _histogramKey = 0;
	int _channel = 0;
	int _selected = -1;
	int _overPoint = -1;
	int _overChannel = -1;
	bool _overReset = false;
	bool _overGraph = false;
	bool _dragging = false;
	bool _moved = false;
	bool _clickPending = false;
	QPoint _pressPosition;
	QPointF _dragShift;
	Ui::FlatLabel *_hint = nullptr;
	int _graphSide = 0;

};

CurveEditor::CurveEditor(QWidget *parent, FxCustomEditorArgs &&args)
: RpWidget(parent)
, _changed(std::move(args.changed))
, _controller(args.controller)
, _layerId(args.layerId)
, _fxUid(args.fxUid)
, _curve(ParseToneCurve(args.value)) {
	_hint = Ui::CreateChild<Ui::FlatLabel>(
		this,
		tr::lng_oblivion_photo_adj_curve_hint(),
		HintLabelStyle());
	_hint->setAttribute(Qt::WA_TransparentForMouseEvents);
	_hint->show();
	// Start with the first channel that has a curve.
	for (auto i = 0; i != kCurveChannels; ++i) {
		if (_curve.channels[i] != DefaultCurvePoints()) {
			_channel = i;
			break;
		}
	}
	if (args.values) {
		std::move(args.values) | rpl::on_next([=](const QByteArray &value) {
			showValue(value);
		}, lifetime());
	}
	if (const auto controller = _controller.get()) {
		if (_layerId && _fxUid) {
			controller->documentChanges() | rpl::on_next([=] {
				refreshHistogram();
			}, lifetime());
			refreshHistogram();
		}
	}
	setMouseTracking(true);
}

// The histogram of what the curve gets: the layer with the effects that
// stand before this one. It is counted on a small render off the main
// thread, and only when something before the curve changes.
void CurveEditor::refreshHistogram() {
	const auto controller = _controller.get();
	if (!controller || !controller->hasDocument()) {
		return;
	}
	const auto layer = controller->document().find(_layerId);
	if (!layer || !layer->content) {
		return;
	}
	const auto i = ranges::find(layer->effects, _fxUid, &FxInstance::uid);
	if (i == end(layer->effects)) {
		return;
	}
	auto copy = *layer;
	copy.effects.erase(
		begin(copy.effects) + (i - begin(layer->effects)),
		end(copy.effects));
	copy.maskEnabled = false;
	const auto full = copy.size();
	if (full.isEmpty()) {
		return;
	}
	auto key = FxHashCombine(0x4157ULL, copy.content->serial());
	for (const auto &effect : copy.effects) {
		key = FxHashCombine(key, FxHash(effect));
	}
	if (key == _histogramKey) {
		return;
	}
	_histogramKey = key;
	const auto exact = std::min(
		kHistogramSide / std::max(full.width(), full.height()),
		1.);
	const auto scale = std::min(
		std::pow(2., std::ceil(std::log2(std::max(exact, 1e-4)))),
		1.);
	crl::async([
			weak = base::make_weak(this),
			compositor = controller->compositor(),
			copy = std::move(copy),
			scale,
			key] {
		const auto result = ComputeHistogram(
			compositor->layerPixels(copy, scale));
		crl::on_main(weak, [=] {
			if (const auto strong = weak.get()) {
				if (strong->_histogramKey == key) {
					strong->_histogram = result;
					strong->update();
				}
			}
		});
	});
}

void CurveEditor::selectChannel(int channel) {
	_channel = std::clamp(channel, 0, kCurveChannels - 1);
	_selected = -1;
	update();
}

void CurveEditor::selectPoint(int index) {
	_selected = (index >= 0 && index < int(points().size())) ? index : -1;
	update();
}

CurvePoints &CurveEditor::points() {
	return _curve.channels[_channel];
}

const CurvePoints &CurveEditor::points() const {
	return _curve.channels[_channel];
}

void CurveEditor::showValue(const QByteArray &value) {
	auto parsed = ParseToneCurve(value);
	if (parsed == _curve) {
		return;
	}
	_curve = std::move(parsed);
	_clickPending = false;
	_dragging = false;
	_moved = false;
	_selected = -1;
	_overPoint = -1;
	update();
}

void CurveEditor::report(bool finished) {
	if (_changed) {
		_changed(SerializeToneCurve(_curve), finished);
	}
}

void CurveEditor::commitClick() {
	if (base::take(_clickPending)) {
		report(true);
	}
}

int CurveEditor::resizeGetHeight(int newWidth) {
	_graphSide = std::max(std::min(newWidth, Px(kGraphMaxSide)), Px(60));
	const auto top = Px(kTopRowHeight) + _graphSide + Px(kHintSkip);
	// One column with the graph: the channels above it, the hint below.
	_hint->resizeToWidth(_graphSide);
	_hint->moveToLeft((newWidth - _graphSide) / 2, top, newWidth);
	return top + _hint->height() + Px(kBottomSkip);
}

QRect CurveEditor::channelRect(int index) const {
	const auto size = Px(kChannelSize);
	const auto step = size + Px(kChannelSkip);
	// At the top of the row: the marks of the channels that have a curve
	// stand under the circles. The row is as wide as the graph and stands
	// over it, also when the graph is centered in a wide panel.
	return QRect(
		graphRect().x() + Px(kChannelRing) + index * step,
		Px(kChannelRing) + Px(kChannelTop),
		size,
		size);
}

int CurveEditor::channelAt(QPoint position) const {
	const auto grow = Px(kChannelSkip) / 2;
	for (auto i = 0; i != kCurveChannels; ++i) {
		const auto rect = channelRect(i).marginsAdded(
			{ grow, grow, grow, grow });
		if (rect.contains(position)) {
			return i;
		}
	}
	return -1;
}

bool CurveEditor::resetShown() const {
	return points() != DefaultCurvePoints();
}

QRect CurveEditor::resetRect() const {
	// The same link as the "Reset" of a section title (the color ranges
	// have one right in the next card).
	const auto text = tr::lng_oblivion_photo_adj_reset(tr::now);
	const auto textWidth = st::normalFont->width(text);
	const auto right = graphRect().x() + _graphSide;
	return QRect(
		right - textWidth - Px(4),
		0,
		textWidth + Px(4),
		ChannelsHeight());
}

QRect CurveEditor::graphRect() const {
	return QRect(
		(width() - _graphSide) / 2,
		Px(kTopRowHeight),
		_graphSide,
		_graphSide);
}

QRectF CurveEditor::plotRect() const {
	const auto inset = Px(kGraphInset);
	return QRectF(graphRect()).marginsRemoved(
		{ qreal(inset), qreal(inset), qreal(inset), qreal(inset) });
}

QPointF CurveEditor::pointPosition(const CurvePoint &point) const {
	const auto plot = plotRect();
	return QPointF(
		plot.left() + plot.width() * point.x / kCurveUnit,
		plot.bottom() - plot.height() * point.y / kCurveUnit);
}

CurvePoint CurveEditor::pointFromPosition(QPointF position) const {
	const auto plot = plotRect();
	if (plot.width() <= 0. || plot.height() <= 0.) {
		return {};
	}
	return {
		int(std::lround(
			(position.x() - plot.left()) * kCurveUnit / plot.width())),
		int(std::lround(
			(plot.bottom() - position.y()) * kCurveUnit / plot.height())),
	};
}

int CurveEditor::pointAt(QPointF position) const {
	const auto &list = points();
	auto result = -1;
	auto best = float64(Px(kPointGrab)) * Px(kPointGrab);
	for (auto i = 0; i != int(list.size()); ++i) {
		const auto delta = pointPosition(list[i]) - position;
		const auto distance = delta.x() * delta.x() + delta.y() * delta.y();
		if (distance <= best) {
			best = distance;
			result = i;
		}
	}
	return result;
}

QPainterPath CurveEditor::curvePath(const CurvePoints &points) const {
	const auto plot = plotRect();
	const auto count = std::max(int(plot.width()), 2) + 1;
	auto table = std::vector<float>(count);
	FillCurveTable(points, table.data(), count);
	auto result = QPainterPath();
	for (auto i = 0; i != count; ++i) {
		const auto point = QPointF(
			plot.left() + plot.width() * i / (count - 1),
			plot.bottom() - plot.height() * table[i]);
		if (i) {
			result.lineTo(point);
		} else {
			result.moveTo(point);
		}
	}
	return result;
}

QPainterPath CurveEditor::histogramPath() const {
	const auto plot = plotRect();
	const auto &values = _histogram.values[_channel];
	auto result = QPainterPath();
	result.moveTo(plot.bottomLeft());
	for (auto i = 0; i != kHistogramBins; ++i) {
		const auto height = plot.height() * 0.92 * values[i];
		const auto left = plot.left() + plot.width() * i / kHistogramBins;
		const auto right = plot.left()
			+ plot.width() * (i + 1) / kHistogramBins;
		result.lineTo(left, plot.bottom() - height);
		result.lineTo(right, plot.bottom() - height);
	}
	result.lineTo(plot.bottomRight());
	result.closeSubpath();
	return result;
}

void CurveEditor::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto line = std::max(Px(1), 1);

	// The channels.
	for (auto i = 0; i != kCurveChannels; ++i) {
		const auto rect = QRectF(channelRect(i));
		const auto color = ChannelColor(i);
		if (i == _channel) {
			const auto ring = Px(kChannelRing) - line;
			p.setPen(QPen(color, line * 1.5));
			p.setBrush(Qt::NoBrush);
			p.drawEllipse(rect.marginsAdded(
				{ qreal(ring), qreal(ring), qreal(ring), qreal(ring) }));
		}
		p.setPen(Qt::NoPen);
		p.setBrush(anim::with_alpha(
			color,
			(i == _channel || i == _overChannel) ? 1. : 0.8));
		p.drawEllipse(rect);
		if (_curve.channels[i] != DefaultCurvePoints()) {
			// A mark under the channels that have a curve, the same as
			// the one under the changed color ranges.
			const auto mark = Px(kChannelMark) / 2.;
			p.setBrush(AccentColor());
			p.drawEllipse(
				QPointF(
					rect.center().x(),
					rect.bottom() + Px(kChannelRing) + mark + line),
				mark,
				mark);
		}
	}
	const auto &font = SmallFont();
	const auto rowHeight = ChannelsHeight();
	const auto nameLeft = channelRect(kCurveChannels - 1).right()
		+ Px(kChannelSkip)
		+ Px(kChannelRing);
	const auto reset = resetShown();
	const auto nameRight = reset
		? (resetRect().left() - Px(6))
		: (graphRect().x() + _graphSide);
	p.setFont(font);
	if (nameRight > nameLeft) {
		p.setPen(SubTextColor());
		p.drawText(
			QRect(nameLeft, 0, nameRight - nameLeft, rowHeight),
			Qt::AlignLeft | Qt::AlignVCenter,
			font->elided(ChannelName(_channel), nameRight - nameLeft));
	}
	if (reset) {
		auto resetFont = st::normalFont;
		if (_overReset) {
			resetFont = resetFont->underline();
		}
		p.setFont(resetFont);
		p.setPen(AccentColor());
		p.drawText(
			resetRect(),
			Qt::AlignRight | Qt::AlignVCenter,
			tr::lng_oblivion_photo_adj_reset(tr::now));
	}

	// The graph.
	const auto graph = graphRect();
	const auto plot = plotRect();
	const auto radius = Px(kGraphRadius);
	p.setPen(Qt::NoPen);
	p.setBrush(st::groupCallMembersBg);
	p.drawRoundedRect(graph, radius, radius);
	if (_histogram.valid) {
		p.setBrush(anim::with_alpha(
			ChannelColor(_channel),
			_channel ? 0.2 : 0.13));
		p.drawPath(histogramPath());
	}
	p.setPen(QPen(anim::with_alpha(TextColor(), 0.08), line));
	for (auto i = 0; i <= 4; ++i) {
		const auto x = plot.left() + plot.width() * i / 4.;
		const auto y = plot.top() + plot.height() * i / 4.;
		p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
		p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
	}
	p.setPen(QPen(anim::with_alpha(TextColor(), 0.16), line));
	p.drawLine(plot.bottomLeft(), plot.topRight());

	p.setBrush(Qt::NoBrush);
	for (auto i = 0; i != kCurveChannels; ++i) {
		if (i != _channel && _curve.channels[i] != DefaultCurvePoints()) {
			p.setPen(QPen(anim::with_alpha(ChannelColor(i), 0.5), line));
			p.drawPath(curvePath(_curve.channels[i]));
		}
	}
	const auto color = ChannelColor(_channel);
	p.setPen(QPen(
		color,
		line * 2.,
		Qt::SolidLine,
		Qt::RoundCap,
		Qt::RoundJoin));
	p.drawPath(curvePath(points()));

	const auto &list = points();
	for (auto i = 0; i != int(list.size()); ++i) {
		const auto center = pointPosition(list[i]);
		const auto active = (i == _selected) || (i == _overPoint);
		const auto size = Px(kPointRadius) + (active ? line : 0);
		p.setPen(QPen(color, line * 2.));
		p.setBrush((i == _selected) ? QBrush(color) : st::groupCallBg->b);
		p.drawEllipse(center, qreal(size), qreal(size));
	}

	// What the selected point does, in the corner the curve rarely uses.
	const auto shown = (_selected >= 0 && _selected < int(list.size()))
		? _selected
		: _overPoint;
	if (shown >= 0 && shown < int(list.size())) {
		const auto level = [](int value) {
			return QString::number(
				int(std::lround(value * 255. / kCurveUnit)));
		};
		const auto text = tr::lng_oblivion_photo_adj_curve_point(
			tr::now,
			lt_input,
			level(list[shown].x),
			lt_output,
			level(list[shown].y));
		// On a small plate inside the grid, so that the lines of the
		// grid, the histogram and the curves don't cross the digits.
		const auto skip = Px(kReadoutSkip);
		const auto padding = Px(kReadoutPadding);
		const auto available = int(plot.width()) - 2 * (skip + padding);
		if (available > 0) {
			const auto textWidth = std::min(font->width(text), available);
			const auto plateWidth = textWidth + 2 * padding;
			const auto plateHeight = font->height + Px(4);
			const auto above = (list[shown].y > kCurveUnit / 2)
				&& (list[shown].x < kCurveUnit / 2);
			const auto plate = QRectF(
				above
					? (plot.right() - skip - plateWidth)
					: (plot.left() + skip),
				above
					? (plot.bottom() - skip - plateHeight)
					: (plot.top() + skip),
				plateWidth,
				plateHeight);
			const auto plateRadius = Px(kReadoutRadius);
			p.setPen(Qt::NoPen);
			p.setBrush(anim::with_alpha(st::groupCallBg->c, 0.8));
			p.drawRoundedRect(plate, plateRadius, plateRadius);
			p.setFont(font);
			p.setPen(SubTextColor());
			p.drawText(
				plate,
				Qt::AlignCenter,
				font->elided(text, textWidth));
		}
	}
}

int CurveEditor::addPoint(QPointF position) {
	auto &list = points();
	auto point = pointFromPosition(position);
	point.x = std::clamp(point.x, 0, kCurveUnit);
	point.y = std::clamp(point.y, 0, kCurveUnit);
	// A click near the curve adds a point on it: nothing jumps.
	const auto onCurve = int(std::lround(
		CurveValue(list, point.x / double(kCurveUnit)) * kCurveUnit));
	const auto curvePosition = pointPosition({ point.x, onCurve });
	if (std::abs(curvePosition.y() - position.y()) <= Px(kPointSnap)) {
		point.y = onCurve;
	}
	for (auto i = 0; i != int(list.size()); ++i) {
		if (std::abs(list[i].x - point.x) < kCurveMinGap) {
			return i;
		}
	}
	if (int(list.size()) >= kCurveMaxPoints) {
		return -1;
	}
	const auto where = std::find_if(
		begin(list),
		end(list),
		[&](const CurvePoint &existing) { return existing.x > point.x; });
	const auto index = int(where - begin(list));
	list.insert(where, point);
	return index;
}

bool CurveEditor::movePoint(int index, QPointF position) {
	auto &list = points();
	if (index < 0 || index >= int(list.size())) {
		return false;
	}
	auto point = pointFromPosition(position);
	const auto from = (index > 0)
		? (list[index - 1].x + kCurveMinGap)
		: 0;
	const auto till = (index + 1 < int(list.size()))
		? (list[index + 1].x - kCurveMinGap)
		: kCurveUnit;
	point.x = std::clamp(point.x, from, std::max(from, till));
	point.y = std::clamp(point.y, 0, kCurveUnit);
	if (list[index] == point) {
		return false;
	}
	list[index] = point;
	return true;
}

void CurveEditor::removePoint(int index) {
	auto &list = points();
	if (index < 0 || index >= int(list.size())) {
		return;
	} else if (list.size() > 2) {
		list.erase(begin(list) + index);
	} else {
		list = DefaultCurvePoints();
	}
	_selected = -1;
	_overPoint = -1;
}

void CurveEditor::updateOver(QPoint position) {
	const auto channel = channelAt(position);
	const auto reset = resetShown() && resetRect().contains(position);
	const auto grab = Px(kPointGrab);
	const auto graph = graphRect().marginsAdded({ grab, 0, grab, 0 }).contains(
		position);
	const auto point = graph ? pointAt(position) : -1;
	if (_overChannel != channel
		|| _overReset != reset
		|| _overPoint != point
		|| _overGraph != graph) {
		_overChannel = channel;
		_overReset = reset;
		_overPoint = point;
		_overGraph = graph;
		update();
	}
	setCursor((channel >= 0 || reset || point >= 0)
		? style::cur_pointer
		: graph
		? style::cur_cross
		: style::cur_default);
}

void CurveEditor::mouseMoveEvent(QMouseEvent *e) {
	if (_dragging) {
		if (!_moved) {
			if (!DragStarted(_pressPosition, e->pos())) {
				return;
			}
			_moved = true;
		}
		if (movePoint(_selected, QPointF(e->pos()) + _dragShift)) {
			update();
			report(false);
		}
		return;
	}
	updateOver(e->pos());
}

void CurveEditor::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	commitClick();
	updateOver(e->pos());
	if (_overChannel >= 0) {
		selectChannel(_overChannel);
		updateOver(e->pos());
		return;
	} else if (_overReset) {
		points() = DefaultCurvePoints();
		_selected = -1;
		_overPoint = -1;
		report(true);
		updateOver(e->pos());
		update();
		return;
	} else if (!_overGraph) {
		return;
	}
	auto index = _overPoint;
	auto added = false;
	if (index < 0) {
		const auto before = points().size();
		index = addPoint(e->pos());
		added = (points().size() != before);
	}
	if (index < 0) {
		return;
	}
	_selected = index;
	_overPoint = index;
	_dragging = true;
	_moved = false;
	_pressPosition = e->pos();
	// The point keeps its place under the cursor: it doesn't jump when
	// the drag starts, also when it was put on the curve next to the click.
	_dragShift = pointPosition(points()[index]) - QPointF(e->pos());
	if (added) {
		_clickPending = true;
		report(false);
	}
	update();
}

void CurveEditor::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton || !_dragging) {
		return;
	}
	_dragging = false;
	if (base::take(_moved)) {
		_clickPending = false;
		report(true);
	}
	updateOver(e->pos());
	update();
}

void CurveEditor::mouseDoubleClickEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	_dragging = false;
	_moved = false;
	updateOver(e->pos());
	if (_overChannel >= 0 || _overReset) {
		mousePressEvent(e);
		return;
	}
	// The point that the first click of this double click has put on the
	// curve is taken back: a double click on the empty graph changes
	// nothing and leaves nothing in the history.
	const auto index = base::take(_clickPending) ? _selected : _overPoint;
	if (index >= 0) {
		removePoint(index);
		report(true);
		updateOver(e->pos());
		update();
	}
}

void CurveEditor::leaveEventHook(QEvent *e) {
	if (_dragging) {
		return;
	}
	// No double click can follow the click any more: what is done next
	// in another control is a step of its own.
	commitClick();
	_overChannel = -1;
	_overReset = false;
	_overGraph = false;
	_overPoint = -1;
	update();
}

//
// The color ranges.
//

class HslEditor final : public Ui::RpWidget {
public:
	HslEditor(QWidget *parent, FxCustomEditorArgs &&args);

	// For the snapshot scenes.
	void selectRange(int range);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	[[nodiscard]] QRect swatchRect(int index) const;
	[[nodiscard]] int swatchAt(QPoint position) const;
	[[nodiscard]] bool edited(int range) const;
	[[nodiscard]] FxParams rangeValues() const;
	void showValue(const QByteArray &value);
	void refresh();
	void resetRange(int range);
	void report(bool finished);

	const Fn<void(QByteArray value, bool finished)> _changed;
	HslTable _table;
	int _range = 0;
	int _over = -1;
	rpl::variable<QString> _name;
	rpl::event_stream<FxParams> _rangeValues;
	SectionTitle *_title = nullptr;
	Ui::RpWidget *_sliders = nullptr;
	bool _resizing = false;

};

HslEditor::HslEditor(QWidget *parent, FxCustomEditorArgs &&args)
: RpWidget(parent)
, _changed(std::move(args.changed))
, _table(ParseHslTable(args.value)) {
	for (auto i = 0; i != kHslRanges; ++i) {
		if (edited(i)) {
			_range = i;
			break;
		}
	}
	_name = RangeName(_range);
	_title = Ui::CreateChild<SectionTitle>(this, _name.value());
	_title->setAction(tr::lng_oblivion_photo_adj_reset(), [=] {
		resetRange(_range);
	});
	_title->show();
	// The rows of the parameter panel: like every other slider of an
	// effect they let the value be typed after a click on it.
	_sliders = CreateParamsPanel(this, ParamsPanelArgs{
		.params = {
			FxInt(kHslHueId, tr::lng_oblivion_photo_adj_hue, -100, 100, 0),
			FxInt(
				kHslSaturationId,
				tr::lng_oblivion_photo_adj_saturation,
				-100,
				100,
				0),
			FxInt(
				kHslLuminanceId,
				tr::lng_oblivion_photo_adj_luminance,
				-100,
				100,
				0),
		},
		.values = rangeValues(),
		.updates = _rangeValues.events(),
		.changed = [=](const QByteArray &id, FxValue value, bool finished) {
			auto &row = (id == kHslHueId)
				? _table.hue
				: (id == kHslSaturationId)
				? _table.saturation
				: _table.luminance;
			row[_range] = std::clamp(value.integer(), -100, 100);
			_title->setActionVisible(edited(_range));
			update();
			report(finished);
		},
	}).release();
	_sliders->show();
	_sliders->heightValue() | rpl::on_next([=] {
		if (!_resizing && width() > 0) {
			resizeToWidth(width());
		}
	}, lifetime());
	_title->setActionVisible(edited(_range));
	if (args.values) {
		std::move(args.values) | rpl::on_next([=](const QByteArray &value) {
			showValue(value);
		}, lifetime());
	}
	setMouseTracking(true);
}

bool HslEditor::edited(int range) const {
	return _table.hue[range]
		|| _table.saturation[range]
		|| _table.luminance[range];
}

FxParams HslEditor::rangeValues() const {
	auto result = FxParams();
	result.set(kHslHueId, FxValue::Integer(_table.hue[_range]));
	result.set(
		kHslSaturationId,
		FxValue::Integer(_table.saturation[_range]));
	result.set(
		kHslLuminanceId,
		FxValue::Integer(_table.luminance[_range]));
	return result;
}

void HslEditor::selectRange(int range) {
	// A value typed for the range that was selected belongs to it.
	FinishTyping(_sliders);
	_range = std::clamp(range, 0, kHslRanges - 1);
	refresh();
}

void HslEditor::showValue(const QByteArray &value) {
	const auto parsed = ParseHslTable(value);
	if (parsed == _table) {
		return;
	}
	_table = parsed;
	refresh();
}

void HslEditor::refresh() {
	_name = RangeName(_range);
	_rangeValues.fire(rangeValues());
	_title->setActionVisible(edited(_range));
	update();
}

void HslEditor::resetRange(int range) {
	FinishTyping(_sliders);
	if (range < 0 || range >= kHslRanges || !edited(range)) {
		return;
	}
	_table.hue[range] = 0;
	_table.saturation[range] = 0;
	_table.luminance[range] = 0;
	refresh();
	report(true);
}

void HslEditor::report(bool finished) {
	if (_changed) {
		_changed(SerializeHslTable(_table), finished);
	}
}

int HslEditor::resizeGetHeight(int newWidth) {
	auto top = Px(kSwatchRowHeight);
	const auto place = [&](not_null<Ui::RpWidget*> widget) {
		widget->resizeToWidth(newWidth);
		widget->moveToLeft(0, top, newWidth);
		top += widget->height();
	};
	_resizing = true;
	place(_title);
	place(_sliders);
	_resizing = false;
	return top + Px(kBottomSkip);
}

QRect HslEditor::swatchRect(int index) const {
	const auto size = Px(kSwatchSize);
	const auto ring = Px(kSwatchRing);
	const auto available = std::max(width() - 2 * ring - size, 0);
	return QRect(
		ring + (available * index) / (kHslRanges - 1),
		ring + Px(2),
		size,
		size);
}

int HslEditor::swatchAt(QPoint position) const {
	if (position.y() >= Px(kSwatchRowHeight)) {
		return -1;
	}
	const auto grow = Px(kSwatchRing);
	for (auto i = 0; i != kHslRanges; ++i) {
		const auto rect = swatchRect(i).marginsAdded(
			{ grow, grow, grow, Px(kSwatchRowHeight) });
		if (rect.contains(position)) {
			return i;
		}
	}
	return -1;
}

void HslEditor::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto line = std::max(Px(1), 1);
	for (auto i = 0; i != kHslRanges; ++i) {
		const auto rect = QRectF(swatchRect(i));
		const auto color = HslRangeColor(i);
		if (i == _range) {
			const auto ring = Px(kSwatchRing) - line;
			p.setPen(QPen(TextColor(), line * 1.5));
			p.setBrush(Qt::NoBrush);
			p.drawEllipse(rect.marginsAdded(
				{ qreal(ring), qreal(ring), qreal(ring), qreal(ring) }));
		}
		p.setPen(Qt::NoPen);
		p.setBrush(anim::with_alpha(
			color,
			(i == _range || i == _over) ? 1. : 0.8));
		p.drawEllipse(rect);
		if (edited(i)) {
			// A mark under the ranges that are changed.
			const auto mark = Px(kSwatchMark) / 2.;
			p.setBrush(AccentColor());
			p.drawEllipse(
				QPointF(
					rect.center().x(),
					rect.bottom() + Px(kSwatchRing) + mark + line),
				mark,
				mark);
		}
	}
}

void HslEditor::mouseMoveEvent(QMouseEvent *e) {
	const auto over = swatchAt(e->pos());
	if (_over != over) {
		_over = over;
		update();
	}
	setCursor((over >= 0) ? style::cur_pointer : style::cur_default);
}

void HslEditor::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto over = swatchAt(e->pos());
	if (over >= 0 && over != _range) {
		selectRange(over);
	}
}

void HslEditor::mouseDoubleClickEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto over = swatchAt(e->pos());
	if (over >= 0) {
		selectRange(over);
		resetRange(over);
	}
}

void HslEditor::leaveEventHook(QEvent *e) {
	if (_over >= 0) {
		_over = -1;
		update();
	}
}

//
// The color grading wheels.
//

class WheelsEditor final : public Ui::RpWidget {
public:
	WheelsEditor(QWidget *parent, FxCustomEditorArgs &&args);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	[[nodiscard]] QRectF wheelRect(int index) const;
	[[nodiscard]] int wheelAt(QPointF position) const;
	[[nodiscard]] QPointF handlePosition(int index) const;
	[[nodiscard]] const QImage &disk();
	void showValue(const QByteArray &value);
	bool setFromPosition(int index, QPointF position);
	void report(bool finished);
	void commitClick();

	const Fn<void(QByteArray value, bool finished)> _changed;
	GradeWheels _wheels;
	int _dragging = -1;
	int _over = -1;
	bool _moved = false;
	bool _clickPending = false;
	QPoint _pressPosition;
	QPointF _dragShift;
	int _size = 0;
	int _textTop = 0;
	QImage _disk;
	Ui::FlatLabel *_hint = nullptr;

};

WheelsEditor::WheelsEditor(QWidget *parent, FxCustomEditorArgs &&args)
: RpWidget(parent)
, _changed(std::move(args.changed))
, _wheels(ParseGradeWheels(args.value)) {
	_hint = Ui::CreateChild<Ui::FlatLabel>(
		this,
		tr::lng_oblivion_photo_adj_wheel_hint(),
		HintLabelStyle());
	_hint->setAttribute(Qt::WA_TransparentForMouseEvents);
	_hint->show();
	if (args.values) {
		std::move(args.values) | rpl::on_next([=](const QByteArray &value) {
			showValue(value);
		}, lifetime());
	}
	setMouseTracking(true);
}

void WheelsEditor::showValue(const QByteArray &value) {
	const auto parsed = ParseGradeWheels(value);
	if (parsed == _wheels) {
		return;
	}
	_wheels = parsed;
	_clickPending = false;
	_dragging = -1;
	_moved = false;
	update();
}

void WheelsEditor::report(bool finished) {
	if (_changed) {
		_changed(SerializeGradeWheels(_wheels), finished);
	}
}

void WheelsEditor::commitClick() {
	if (base::take(_clickPending)) {
		report(true);
	}
}

int WheelsEditor::resizeGetHeight(int newWidth) {
	// Three equal columns. The handle of a wheel may stand on its rim,
	// so it needs some place on both sides.
	_size = std::clamp(
		(newWidth / kGradeRanges) - 2 * Px(kWheelMargin),
		Px(24),
		Px(kWheelMaxSize));
	_textTop = Px(kWheelTop) + _size + Px(kWheelTextSkip);
	const auto top = _textTop
		+ SmallSemiboldFont()->height
		+ SmallFont()->height
		+ Px(kHintSkip);
	_hint->resizeToWidth(newWidth);
	_hint->moveToLeft(0, top, newWidth);
	return top + _hint->height() + Px(kBottomSkip);
}

QRectF WheelsEditor::wheelRect(int index) const {
	// Three equal columns, a wheel in the middle of each.
	const auto column = width() / float64(kGradeRanges);
	return QRectF(
		column * index + (column - _size) / 2.,
		Px(kWheelTop),
		_size,
		_size);
}

int WheelsEditor::wheelAt(QPointF position) const {
	const auto grow = float64(Px(kWheelHandle));
	for (auto i = 0; i != kGradeRanges; ++i) {
		const auto rect = wheelRect(i);
		const auto delta = position - rect.center();
		const auto limit = rect.width() / 2. + grow;
		if (delta.x() * delta.x() + delta.y() * delta.y() <= limit * limit) {
			return i;
		}
	}
	return -1;
}

QPointF WheelsEditor::handlePosition(int index) const {
	const auto rect = wheelRect(index);
	const auto &wheel = _wheels.ranges[index];
	const auto radians = wheel.hue * kPi / 180.;
	const auto length = rect.width() / 2. * wheel.saturation / 100.;
	return rect.center() + QPointF(
		std::cos(radians) * length,
		-std::sin(radians) * length);
}

bool WheelsEditor::setFromPosition(int index, QPointF position) {
	if (index < 0 || index >= kGradeRanges) {
		return false;
	}
	const auto rect = wheelRect(index);
	const auto delta = position - rect.center();
	const auto radius = std::max(rect.width() / 2., 1.);
	const auto length = std::sqrt(
		delta.x() * delta.x() + delta.y() * delta.y());
	auto saturation = std::clamp(
		int(std::lround(100. * length / radius)),
		0,
		100);
	if (saturation < kWheelSnap) {
		saturation = 0;
	}
	const auto degrees = std::atan2(-delta.y(), delta.x()) * 180. / kPi;
	const auto next = GradeWheel{
		.hue = saturation
			? (((int(std::lround(degrees)) % 360) + 360) % 360)
			: 0,
		.saturation = saturation,
	};
	if (_wheels.ranges[index] == next) {
		return false;
	}
	_wheels.ranges[index] = next;
	return true;
}

const QImage &WheelsEditor::disk() {
	const auto ratio = style::DevicePixelRatio();
	const auto side = _size * ratio;
	if (_disk.width() == side || side <= 0) {
		return _disk;
	}
	_disk = QImage(side, side, QImage::Format_ARGB32_Premultiplied);
	_disk.setDevicePixelRatio(ratio);
	const auto center = (side - 1) / 2.;
	const auto radius = side / 2.;
	for (auto y = 0; y != side; ++y) {
		const auto line = reinterpret_cast<uint32*>(_disk.scanLine(y));
		for (auto x = 0; x != side; ++x) {
			const auto dx = x - center;
			const auto dy = y - center;
			const auto length = std::sqrt(dx * dx + dy * dy);
			const auto alpha = std::clamp(radius - length, 0., 1.);
			if (alpha <= 0.) {
				line[x] = 0;
				continue;
			}
			auto degrees = std::atan2(-dy, dx) * 180. / kPi;
			if (degrees < 0.) {
				degrees += 360.;
			}
			// A little darker than the pure colors: the panel is dark.
			const auto color = QColor::fromHsvF(
				float(std::min(degrees / 360., 0.9999)),
				float(std::min(length / radius, 1.)),
				0.9f);
			line[x] = (uint32(std::lround(alpha * 255.)) << 24)
				| (uint32(std::lround(color.red() * alpha)) << 16)
				| (uint32(std::lround(color.green() * alpha)) << 8)
				| uint32(std::lround(color.blue() * alpha));
		}
	}
	return _disk;
}

void WheelsEditor::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto line = std::max(Px(1), 1);
	const auto &image = disk();
	const auto &nameFont = SmallSemiboldFont();
	const auto &valueFont = SmallFont();
	const auto column = width() / kGradeRanges;
	for (auto i = 0; i != kGradeRanges; ++i) {
		const auto rect = wheelRect(i);
		const auto &wheel = _wheels.ranges[i];
		if (!image.isNull()) {
			p.drawImage(rect.topLeft(), image);
		}
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(
			anim::with_alpha(
				TextColor(),
				(i == _over || i == _dragging) ? 0.5 : 0.18),
			line));
		p.drawEllipse(rect);

		const auto handle = handlePosition(i);
		const auto size = Px(kWheelHandle) + ((i == _dragging) ? line : 0);
		p.setPen(QPen(QColor(0, 0, 0, 96), line * 3.));
		p.drawEllipse(handle, qreal(size), qreal(size));
		p.setPen(QPen(QColor(255, 255, 255), line * 1.5));
		p.setBrush(wheel.saturation
			? GradeWheelColor(wheel.hue, wheel.saturation)
			: QColor(255, 255, 255));
		p.drawEllipse(handle, qreal(size), qreal(size));

		const auto textRect = QRect(column * i, _textTop, column, 0);
		p.setFont(nameFont);
		p.setPen(TextColor());
		p.drawText(
			textRect.adjusted(0, 0, 0, nameFont->height),
			Qt::AlignHCenter | Qt::AlignVCenter,
			nameFont->elided(WheelName(i), column));
		p.setFont(valueFont);
		p.setPen(wheel.saturation ? AccentColor() : SubTextColor());
		p.drawText(
			textRect.adjusted(
				0,
				nameFont->height,
				0,
				nameFont->height + valueFont->height),
			Qt::AlignHCenter | Qt::AlignVCenter,
			valueFont->elided(
				wheel.saturation
					? tr::lng_oblivion_photo_adj_wheel_value(
						tr::now,
						lt_hue,
						QString::number(wheel.hue),
						lt_saturation,
						QString::number(wheel.saturation))
					: tr::lng_oblivion_photo_adj_wheel_none(tr::now),
				column));
	}
}

void WheelsEditor::mouseMoveEvent(QMouseEvent *e) {
	if (_dragging >= 0) {
		if (!_moved) {
			if (!DragStarted(_pressPosition, e->pos())) {
				return;
			}
			_moved = true;
		}
		if (setFromPosition(_dragging, QPointF(e->pos()) + _dragShift)) {
			update();
			report(false);
		}
		return;
	}
	const auto over = wheelAt(e->pos());
	if (_over != over) {
		_over = over;
		update();
	}
	setCursor((over >= 0) ? style::cur_pointer : style::cur_default);
}

void WheelsEditor::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	commitClick();
	const auto over = wheelAt(e->pos());
	if (over < 0) {
		return;
	}
	_dragging = over;
	_over = over;
	_moved = false;
	_pressPosition = e->pos();
	// Taking the handle doesn't move it (it keeps its place under the
	// cursor while it is dragged), a click elsewhere puts it there.
	const auto delta = handlePosition(over) - QPointF(e->pos());
	const auto grab = float64(Px(kWheelHandle) + Px(2));
	const auto taken = (delta.x() * delta.x() + delta.y() * delta.y()
		<= grab * grab);
	_dragShift = taken ? delta : QPointF();
	if (!taken && setFromPosition(over, e->pos())) {
		_clickPending = true;
		report(false);
	}
	update();
}

void WheelsEditor::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton || _dragging < 0) {
		return;
	}
	_dragging = -1;
	if (base::take(_moved)) {
		_clickPending = false;
		report(true);
	}
	update();
}

void WheelsEditor::mouseDoubleClickEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	_dragging = -1;
	_moved = false;
	// The tint that the first click of this double click may have put
	// under the cursor is not a step of its own: only the reset is.
	const auto pending = base::take(_clickPending);
	const auto over = wheelAt(e->pos());
	if (over >= 0 && !(_wheels.ranges[over] == GradeWheel())) {
		_wheels.ranges[over] = GradeWheel();
		report(true);
	} else if (pending) {
		report(true);
	}
	update();
}

void WheelsEditor::leaveEventHook(QEvent *e) {
	if (_dragging >= 0) {
		return;
	}
	// No double click can follow the click any more: what is done next
	// in another control is a step of its own.
	commitClick();
	if (_over >= 0) {
		_over = -1;
		update();
	}
}

const auto Registered = FxRegistrar([] {
	RegisterFxCustomEditor({
		.type = kAdjustCurveType,
		.create = [](
				not_null<QWidget*> parent,
				FxCustomEditorArgs &&args) -> object_ptr<Ui::RpWidget> {
			return object_ptr<CurveEditor>(parent, std::move(args));
		},
		.normalize = NormalizeToneCurve,
	});
	RegisterFxCustomEditor({
		.type = kAdjustHslType,
		.create = [](
				not_null<QWidget*> parent,
				FxCustomEditorArgs &&args) -> object_ptr<Ui::RpWidget> {
			return object_ptr<HslEditor>(parent, std::move(args));
		},
		.normalize = NormalizeHslTable,
	});
	RegisterFxCustomEditor({
		.type = kAdjustWheelsType,
		.create = [](
				not_null<QWidget*> parent,
				FxCustomEditorArgs &&args) -> object_ptr<Ui::RpWidget> {
			return object_ptr<WheelsEditor>(parent, std::move(args));
		},
		.normalize = NormalizeGradeWheels,
	});
});

//
// Snapshot scenes.
//

[[nodiscard]] FxInstance SceneFx(
		const QByteArray &id,
		std::initializer_list<std::pair<const char*, double>> values,
		const char *dataId = nullptr,
		const QByteArray &data = QByteArray()) {
	auto result = MakeFx(id);
	for (const auto &[key, value] : values) {
		result.params.set(QByteArray(key), FxValue::Number(value));
	}
	if (dataId) {
		result.params.set(QByteArray(dataId), FxValue::Data(data));
	}
	return result;
}

[[nodiscard]] QByteArray SceneCurve() {
	auto curve = ToneCurve();
	curve.channels[0] = {
		{ 0, 0 },
		{ 250, 170 },
		{ 720, 800 },
		{ 1000, 1000 },
	};
	curve.channels[3] = { { 0, 70 }, { 500, 520 }, { 1000, 930 } };
	return SerializeToneCurve(curve);
}

// Curves in three channels. The middle point of the blue one stands in
// the top left quarter of the graph, where the readout of the selected
// point moves away to the opposite corner.
[[nodiscard]] QByteArray SceneChannelCurves() {
	auto curve = ToneCurve();
	curve.channels[0] = {
		{ 0, 0 },
		{ 250, 170 },
		{ 720, 800 },
		{ 1000, 1000 },
	};
	curve.channels[1] = { { 0, 0 }, { 520, 450 }, { 1000, 1000 } };
	curve.channels[3] = { { 0, 90 }, { 300, 540 }, { 1000, 940 } };
	return SerializeToneCurve(curve);
}

[[nodiscard]] QByteArray SceneTable() {
	auto table = HslTable();
	table.hue[int(HslRange::Orange)] = 12;
	table.saturation[int(HslRange::Orange)] = 35;
	table.luminance[int(HslRange::Orange)] = 10;
	table.hue[int(HslRange::Blue)] = -25;
	table.saturation[int(HslRange::Blue)] = 40;
	table.luminance[int(HslRange::Blue)] = -30;
	table.saturation[int(HslRange::Green)] = -45;
	return SerializeHslTable(table);
}

[[nodiscard]] QByteArray SceneWheels() {
	auto wheels = GradeWheels();
	wheels.ranges[0] = { 205, 45 };
	wheels.ranges[2] = { 38, 40 };
	return SerializeGradeWheels(wheels);
}

[[nodiscard]] FxInstance SceneLight() {
	return SceneFx("adjust.light", {
		{ "exposure", 0.35 },
		{ "contrast", 18. },
		{ "highlights", -45. },
		{ "shadows", 40. },
		{ "whites", 10. },
		{ "blacks", -8. },
	});
}

[[nodiscard]] FxInstance SceneGrading() {
	return SceneFx("adjust.grading", {
		{ "midtones_lum", 8. },
		{ "blending", 60. },
		{ "balance", -15. },
	}, "wheels", SceneWheels());
}

// The sample document of the editor with the effects on its photo.
[[nodiscard]] Fn<Document()> SceneDocument(
		Fn<std::vector<FxInstance>()> effects) {
	return [=] {
		auto document = SampleSceneDocument();
		if (!document.layers.empty()) {
			const auto id = document.layers.front().id;
			for (auto &effect : effects()) {
				AddLayerFx(document, id, std::move(effect));
			}
		}
		return document;
	};
}

void ChoosePhoto(not_null<Controller*> controller) {
	const auto &layers = controller->document().layers;
	if (!layers.empty()) {
		controller->setActiveLayer(layers.front().id);
	}
}

// The cards of the effects of the photo, as the Layer tab shows them:
// 340 is the width of the panel beside the photo, in a narrow window the
// panel stands under the photo and is as wide as the window.
void RegisterStackScene(
		const QString &name,
		Fn<std::vector<FxInstance>()> effects,
		int width = 340) {
	RegisterPanelScene({
		.name = name,
		.size = QSize(Px(width), 0),
		.document = SceneDocument(std::move(effects)),
		.prepare = ChoosePhoto,
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			auto result = object_ptr<Ui::VerticalLayout>(parent);
			result->add(CreateFxStackPanel(
				result.data(),
				controller,
				controller->activeLayerValue()));
			result->add(object_ptr<Ui::FixedHeightWidget>(
				result.data(),
				Px(16)));
			return object_ptr<Ui::RpWidget>(std::move(result));
		},
	});
}

// One editor alone, on a card like the one of an effect.
template <typename Editor, typename Prepare>
void RegisterEditorWidgetScene(
		const QString &name,
		Fn<QByteArray()> value,
		Prepare &&prepare) {
	RegisterPanelScene({
		.name = name,
		.size = QSize(Px(340), 0),
		.create = [=](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			auto result = object_ptr<Ui::VerticalLayout>(parent);
			const auto card = result->add(
				object_ptr<Ui::VerticalLayout>(result.data()),
				style::margins(Px(16), Px(12), Px(16), Px(12)));
			card->paintRequest() | rpl::on_next([=] {
				auto p = QPainter(card);
				auto hq = PainterHighQualityEnabler(p);
				p.setPen(Qt::NoPen);
				p.setBrush(st::groupCallBg);
				p.drawRoundedRect(card->rect(), Px(12), Px(12));
			}, card->lifetime());
			const auto editor = card->add(
				object_ptr<Editor>(card, FxCustomEditorArgs{
					.value = value(),
					.controller = controller,
				}),
				style::margins(Px(12), Px(8), Px(12), Px(8)));
			prepare(editor);
			return object_ptr<Ui::RpWidget>(std::move(result));
		},
	});
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	RegisterStackScene(u"photo_adjust_light"_q, [] {
		return std::vector<FxInstance>{
			SceneLight(),
			SceneFx("adjust.curve", {}, "curve", SceneCurve()),
		};
	});
	RegisterStackScene(u"photo_adjust_color"_q, [] {
		return std::vector<FxInstance>{
			SceneFx("adjust.color", {
				{ "temperature", 14. },
				{ "tint", -6. },
				{ "vibrance", 35. },
				{ "saturation", -5. },
			}),
			SceneFx("adjust.hsl", {}, "table", SceneTable()),
		};
	});
	RegisterStackScene(u"photo_adjust_grading"_q, [] {
		return std::vector<FxInstance>{
			SceneGrading(),
			SceneFx("adjust.bw", {
				{ "red", 20. },
				{ "orange", 35. },
				{ "blue", -50. },
			}),
		};
	});
	RegisterStackScene(u"photo_adjust_detail"_q, [] {
		return std::vector<FxInstance>{
			SceneFx("adjust.presence", {
				{ "texture", 25. },
				{ "clarity", 30. },
				{ "dehaze", 15. },
			}),
			SceneFx("adjust.sharpen", {
				{ "amount", 60. },
				{ "radius", 1.2 },
				{ "masking", 35. },
			}),
			SceneFx("adjust.denoise", {}),
			SceneFx("adjust.vignette", {}),
			SceneFx("adjust.grain", { { "seed", 4217. } }),
		};
	});
	// The three custom editors in switched off effects: the panel fades
	// them the way it fades the plain rows.
	RegisterStackScene(u"photo_adjust_disabled"_q, [] {
		auto curve = SceneFx("adjust.curve", {}, "curve", SceneCurve());
		auto grading = SceneGrading();
		auto table = SceneFx("adjust.hsl", {}, "table", SceneTable());
		curve.enabled = false;
		grading.enabled = false;
		table.enabled = false;
		return std::vector<FxInstance>{
			std::move(curve),
			std::move(grading),
			std::move(table),
		};
	});
	// The same editors in the wide panel of a narrow window: the graph
	// with its channels and hint is a centered column, the wheels and
	// the color ranges are spread over the width.
	RegisterStackScene(u"photo_adjust_wide"_q, [] {
		return std::vector<FxInstance>{
			SceneFx("adjust.curve", {}, "curve", SceneChannelCurves()),
			SceneGrading(),
			SceneFx("adjust.hsl", {}, "table", SceneTable()),
		};
	}, 500);

	RegisterEditorWidgetScene<CurveEditor>(
		u"photo_adjust_curve_points"_q,
		SceneCurve,
		[](not_null<CurveEditor*> editor) {
			editor->selectChannel(0);
			editor->selectPoint(2);
		});
	// A color channel: its curve over the faded curves of the others,
	// the readout in the bottom right corner.
	RegisterEditorWidgetScene<CurveEditor>(
		u"photo_adjust_curve_blue"_q,
		SceneChannelCurves,
		[](not_null<CurveEditor*> editor) {
			editor->selectChannel(3);
			editor->selectPoint(1);
		});
	RegisterEditorWidgetScene<CurveEditor>(
		u"photo_adjust_curve_empty"_q,
		[] { return QByteArray(); },
		[](not_null<CurveEditor*> editor) {
		});
	RegisterEditorWidgetScene<HslEditor>(
		u"photo_adjust_hsl_blue"_q,
		SceneTable,
		[](not_null<HslEditor*> editor) {
			editor->selectRange(int(HslRange::Blue));
		});
	RegisterEditorWidgetScene<WheelsEditor>(
		u"photo_adjust_wheels"_q,
		SceneWheels,
		[](not_null<WheelsEditor*> editor) {
		});

	// The whole editor in a comfortable window, as it is right after the
	// curve was added to a photo with a tone, a grading and a vignette:
	// the Layer tab is scrolled to its end, to the card of the curve with
	// the histogram of what the curve gets from the effects above it.
	RegisterEditorScene({
		.name = u"photo_adjust_editor"_q,
		.size = QSize(1280, 1000),
		.document = SceneDocument([] {
			return std::vector<FxInstance>{
				SceneLight(),
				SceneGrading(),
				SceneFx("adjust.vignette", {}),
				SceneFx("adjust.curve", {}, "curve", SceneCurve()),
			};
		}),
		.tab = PhotoEditorTab::Layer,
		.prepare = [](not_null<Controller*> controller) {
			ChoosePhoto(controller);
			controller->showLayerEffects();
		},
	});
	RegisterEditorScene({
		.name = u"photo_adjust_editor_bw"_q,
		.document = SceneDocument([] {
			return std::vector<FxInstance>{
				SceneFx("adjust.bw", { { "blue", -60. }, { "orange", 30. } }),
				SceneFx("adjust.presence", { { "clarity", 40. } }),
				SceneFx("adjust.grain", { { "amount", 45. }, { "size", 40. } }),
			};
		}),
		.tab = PhotoEditorTab::Layer,
		.prepare = ChoosePhoto,
	});
});

} // namespace
} // namespace Oblivion::Photo
