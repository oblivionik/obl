/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_lottie_editor_graph.h"

#include "lang/lang_keys.h"
#include "oblivion/oblivion_lottie_editor.h"
#include "oblivion/oblivion_lottie_editor_palette.h"
#include "oblivion/oblivion_lottie_editor_timeline.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/effects/animation_value.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/popup_menu.h"
#include "styles/style_media_player.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtGui/QContextMenuEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>
#include <QtGui/QWheelEvent>

#include <cmath>
#include <functional>
#include <limits>

namespace Oblivion::LottieEdit {
namespace GraphMath {
namespace {

constexpr auto kSlopeStep = 1e-3;
constexpr auto kFlat = 1e-9;
constexpr auto kMaxGridSteps = 64.;
constexpr auto kGridPrecision = 1e-12;

[[nodiscard]] double Duration(const Segment &segment) {
	return segment.t1 - segment.t0;
}

// Value change per unit of time at the linear speed.
[[nodiscard]] double Rate(const Segment &segment) {
	const auto duration = Duration(segment);
	return (duration > kFlat)
		? ((segment.v1 - segment.v0) / duration)
		: 0.;
}

[[nodiscard]] double StartSlope(const Easing &easing) {
	if (easing.hold) {
		return 0.;
	}
	return easing.apply(kSlopeStep) / kSlopeStep;
}

[[nodiscard]] double EndSlope(const Easing &easing) {
	if (easing.hold) {
		return 0.;
	}
	return (1. - easing.apply(1. - kSlopeStep)) / kSlopeStep;
}

} // namespace

QPointF ValuePoint(const Segment &segment, QPointF handle) {
	return QPointF(
		segment.t0 + handle.x() * Duration(segment),
		segment.v0 + handle.y() * (segment.v1 - segment.v0));
}

QPointF ValueHandle(
		const Segment &segment,
		QPointF point,
		QPointF fallback) {
	const auto duration = Duration(segment);
	const auto change = segment.v1 - segment.v0;
	return QPointF(
		(duration > kFlat)
			? std::clamp((point.x() - segment.t0) / duration, 0., 1.)
			: fallback.x(),
		(std::abs(change) > kFlat)
			? ((point.y() - segment.v0) / change)
			: fallback.y());
}

double StartSpeed(const Segment &segment, const Easing &easing) {
	return StartSlope(easing) * Rate(segment);
}

double EndSpeed(const Segment &segment, const Easing &easing) {
	return EndSlope(easing) * Rate(segment);
}

QPointF SpeedPoint(const Segment &segment, const Easing &easing, bool out) {
	const auto duration = Duration(segment);
	const auto rate = Rate(segment);
	if (easing.hold) {
		return QPointF(out ? segment.t0 : segment.t1, 0.);
	} else if (out) {
		const auto x = std::clamp(easing.out.x(), 0., 1.);
		const auto slope = (x > kSlopeStep)
			? (easing.out.y() / x)
			: StartSlope(easing);
		return QPointF(segment.t0 + x * duration, slope * rate);
	}
	const auto x = std::clamp(easing.in.x(), 0., 1.);
	const auto slope = (1. - x > kSlopeStep)
		? ((1. - easing.in.y()) / (1. - x))
		: EndSlope(easing);
	return QPointF(segment.t0 + x * duration, slope * rate);
}

QPointF SpeedHandle(
		const Segment &segment,
		QPointF point,
		bool out,
		QPointF fallback) {
	const auto duration = Duration(segment);
	if (duration <= kFlat) {
		return fallback;
	}
	const auto x = std::clamp((point.x() - segment.t0) / duration, 0., 1.);
	const auto rate = Rate(segment);
	if (std::abs(rate) <= kFlat) {
		return QPointF(x, fallback.y());
	}
	const auto slope = point.y() / rate;
	return out
		? QPointF(x, slope * x)
		: QPointF(x, 1. - slope * (1. - x));
}

double SpeedAt(const Segment &segment, const Easing &easing, double u) {
	if (easing.hold) {
		return 0.;
	}
	const auto from = std::clamp(u - kSlopeStep, 0., 1.);
	const auto till = std::clamp(u + kSlopeStep, 0., 1.);
	if (till - from <= kFlat) {
		return 0.;
	}
	const auto slope = (easing.apply(till) - easing.apply(from))
		/ (till - from);
	return slope * Rate(segment);
}

double NiceStep(double span, int maxTicks) {
	if (!std::isfinite(span) || span <= 0.) {
		return 1.;
	}
	const auto raw = span / std::max(maxTicks, 1);
	const auto power = std::pow(10., std::floor(std::log10(raw)));
	if (!std::isfinite(power) || power <= 0.) {
		return 1.;
	}
	const auto mantissa = raw / power;
	const auto epsilon = 1e-9;
	const auto nice = (mantissa <= 1. + epsilon)
		? 1.
		: (mantissa <= 2. + epsilon)
		? 2.
		: (mantissa <= 5. + epsilon)
		? 5.
		: 10.;
	return nice * power;
}

std::vector<double> GridValues(double min, double max, double step) {
	auto result = std::vector<double>();
	const auto span = max - min;
	if (!std::isfinite(span)
		|| !std::isfinite(step)
		|| span <= 0.
		|| step <= 0.
		|| span / step > kMaxGridSteps) {
		return result;
	}
	const auto first = std::ceil((min - span) / step) * step;
	const auto last = max + span;
	const auto magnitude = std::max(std::abs(min), std::abs(max));
	if (!std::isfinite(first)
		|| !std::isfinite(last)
		|| step <= magnitude * kGridPrecision) {
		return result;
	}
	const auto count = int(std::ceil(3. * (span / step))) + 1;
	result.reserve(count + 1);
	for (auto i = 0; i <= count; ++i) {
		const auto value = first + i * step;
		if (!std::isfinite(value) || value > last) {
			break;
		}
		result.push_back(value);
	}
	return result;
}

} // namespace GraphMath

namespace {

constexpr auto kHeaderHeight = 34;
constexpr auto kPillHeight = 24;
constexpr auto kPillPadding = 10;
constexpr auto kPresetWidth = 30;
constexpr auto kControlSkip = 4;
constexpr auto kGroupSkip = 12;
constexpr auto kPlotPadding = 16;
constexpr auto kHitRadius = 7;
constexpr auto kHandleRadius = 4;
constexpr auto kKeyRadius = 5;
constexpr auto kDragThreshold = 3;
constexpr auto kPulledDistance = 5;
constexpr auto kSamplesPerSegment = 32;
constexpr auto kLengthSamples = 32;
constexpr auto kMaxSamples = 1600;
constexpr auto kLabelSpacing = 44;
constexpr auto kLabelRoom = 96;
constexpr auto kMaxPlotValue = 1e15;
constexpr auto kTooltipDelay = 800;
constexpr auto kTimeEpsilon = 1e-3;

[[nodiscard]] int Scaled(int value) {
	return style::ConvertScale(value);
}

[[nodiscard]] GraphMode &LastMode() {
	static auto result = GraphMode::Value;
	return result;
}

[[nodiscard]] style::font SmallFont() {
	return style::font(Scaled(11), 0, st::normalFont->family());
}

[[nodiscard]] const style::color &DimensionColor(int dimension, int count) {
	if (count <= 1) {
		return st::windowActiveTextFg;
	}
	switch (dimension) {
	case 0: return st::historyPeer1NameFg;
	case 1: return st::historyPeer2NameFg;
	}
	return st::historyPeer4NameFg;
}

[[nodiscard]] QString DimensionName(int dimension) {
	switch (dimension) {
	case 0: return u"X"_q;
	case 1: return u"Y"_q;
	}
	return u"Z"_q;
}

[[nodiscard]] bool IsPositionRole(PropertyRole role) {
	switch (role) {
	case PropertyRole::Position:
	case PropertyRole::Anchor:
	case PropertyRole::StartPoint:
	case PropertyRole::EndPoint:
		return true;
	default:
		return false;
	}
}

[[nodiscard]] double NumberAt(const PropValue &value, int index) {
	return (index >= 0 && index < int(value.numbers.size()))
		? value.numbers[index]
		: 0.;
}

[[nodiscard]] double Distance(
		const PropValue &a,
		const PropValue &b,
		int dimensions) {
	auto sum = 0.;
	for (auto i = 0; i != dimensions; ++i) {
		const auto delta = NumberAt(a, i) - NumberAt(b, i);
		sum += delta * delta;
	}
	return std::sqrt(sum);
}

} // namespace

struct GraphEditor::Data {
	PropertyRef ref;
	PropertyInfo info;
	std::vector<Keyframe> keys; // Times in the time base of the node.
	QString title;

	// Root composition frame = local * scale + offset.
	double scale = 1.;
	double offset = 0.;
	bool mapped = true;

	// A number / a point: real values on the vertical axis, otherwise the
	// progress between the keyframes.
	bool numeric = false;
	int dimensions = 1; // Curves in the value graph.
	int measured = 1; // Numbers that make the distance between values.
	bool pathCapable = false; // Has the "Along the path" switch.
	EasingLimits limits;
	double fps = 60.;

	// Per segment (from the keyframe with the same index).
	std::vector<int> driver; // The dimension that changes the most.
	std::vector<double> length; // How far the value goes.

	[[nodiscard]] double root(double local) const {
		return local * scale + offset;
	}
	[[nodiscard]] double local(double rootFrame) const {
		return (rootFrame - offset) / scale;
	}
	[[nodiscard]] int segments() const {
		return std::max(int(keys.size()) - 1, 0);
	}
};

struct GraphEditor::Drag {
	enum class Type : uchar {
		Keyframe, // Pressed on a keyframe, becomes Handle when dragged.
		Handle,
		Scrub,
	};
	Type type = Type::Keyframe;
	QPoint press;
	bool applied = false;
	int key = -1;
	bool out = false;
	KeyframeRef keyframe;
	Document start;
	QByteArray mergeKey;
	GraphMath::Segment segment;
	QPointF fallback;
	double minValue = 0.;
	double maxValue = 1.;
};

GraphEditor::GraphEditor(
	QWidget *parent,
	not_null<EditorController*> controller)
: RpWidget(parent)
, _controller(controller)
, _mode(LastMode()) {
	setMouseTracking(true);

	_controller->documentChanged(
	) | rpl::on_next([=](const DocumentChange &change) {
		if (change.source == ChangeSource::Load && _drag) {
			_drag = nullptr;
		}
		invalidate();
	}, lifetime());

	rpl::merge(
		_controller->activePropertyChanged(),
		_controller->keyframeSelectionChanged()
	) | rpl::on_next([=] {
		invalidate();
	}, lifetime());

	_controller->currentFrameValue(
	) | rpl::on_next([=] {
		update();
	}, lifetime());

	style::PaletteChanged(
	) | rpl::on_next([=] {
		update();
	}, lifetime());
}

GraphEditor::~GraphEditor() = default;

void GraphEditor::setTimeView(
		int left,
		int right,
		double viewStart,
		double viewSpan) {
	right = std::max(right, left + 1);
	viewSpan = std::max(viewSpan, 1e-6);
	if (_timeLeft == left
		&& _timeRight == right
		&& _viewStart == viewStart
		&& _viewSpan == viewSpan) {
		return;
	}
	_timeLeft = left;
	_timeRight = right;
	_viewStart = viewStart;
	_viewSpan = viewSpan;
	invalidate();
}

GraphMode GraphEditor::mode() const {
	return _mode;
}

void GraphEditor::setMode(GraphMode mode) {
	if (_mode == mode) {
		return;
	}
	finishDrag(true);
	_mode = mode;
	LastMode() = mode;
	invalidate();
}

bool GraphEditor::dragging() const {
	return _drag != nullptr;
}

bool GraphEditor::cancelDrag() {
	if (!_drag) {
		return false;
	}
	finishDrag(true);
	return true;
}

PropertyRef GraphEditor::shownProperty() const {
	return _data ? _data->ref : PropertyRef();
}

void GraphEditor::invalidate() {
	_dirty = true;
	update();
}

void GraphEditor::ensureData() {
	if (_dirty) {
		_dirty = false;
		rebuildData();
		rebuildPlot();
		layoutControls();
	}
}

PropertyRef GraphEditor::resolveProperty() const {
	const auto &document = _controller->document();
	if (const auto active = _controller->activeProperty()) {
		if (document.animated(*active)) {
			return *active;
		}
	}
	for (const auto &keyframe : _controller->selectedKeyframes()) {
		if (document.animated(keyframe.property)) {
			return keyframe.property;
		}
	}
	return PropertyRef();
}

void GraphEditor::rebuildData() {
	_data = nullptr;
	const auto &document = _controller->document();
	const auto ref = resolveProperty();
	if (!ref) {
		return;
	}
	const auto info = document.property(ref);
	if (!info || !info->animated) {
		return;
	}
	auto data = std::make_unique<Data>();
	data->ref = ref;
	data->info = *info;
	data->keys = document.keyframes(ref);
	if (data->keys.empty()) {
		return;
	}
	data->title = PropertyText(*info);
	if (const auto node = document.node(ref.node)) {
		const auto owner = (node->kind == NodeKind::Shape
			&& node->shapeType == ShapeType::Transform)
			? node->parent
			: ref.node;
		const auto name = NodeDisplayName(document, owner);
		if (!name.isEmpty()) {
			data->title = tr::lng_oblivion_lottie_graph_title(
				tr::now,
				lt_property,
				data->title,
				lt_name,
				name);
		}
	}
	const auto first = double(_controller->firstFrame());
	const auto from = document.localFrame(ref.node, first);
	const auto till = document.localFrame(ref.node, first + 100.);
	if (std::abs(till - from) > 1e-6) {
		data->scale = 100. / (till - from);
		data->offset = first - from * data->scale;
	} else {
		data->mapped = false;
	}
	data->numeric = (info->type == PropertyType::Scalar)
		|| (info->type == PropertyType::Vector);
	data->dimensions = (info->type == PropertyType::Vector)
		? std::clamp(info->dimensions, 1, 2)
		: 1;
	data->measured = (info->type == PropertyType::Vector)
		? std::clamp(info->dimensions, 1, 3)
		: 1;
	data->pathCapable = (info->type == PropertyType::Vector)
		&& IsPositionRole(info->role);
	data->limits = EasingRange(document, ref);
	data->fps = std::max(_controller->fps(), 1e-3);

	const auto segments = data->segments();
	data->driver.assign(segments, 0);
	data->length.assign(segments, 1.);
	if (data->numeric) {
		for (auto i = 0; i != segments; ++i) {
			const auto &a = data->keys[i].value;
			const auto &b = data->keys[i + 1].value;
			auto best = 0.;
			for (auto d = 0; d != data->dimensions; ++d) {
				const auto change = std::abs(NumberAt(b, d) - NumberAt(a, d));
				if (change > best) {
					best = change;
					data->driver[i] = d;
				}
			}
			data->length[i] = Distance(a, b, data->measured);
		}
		if (info->spatial && data->measured > 1 && segments > 0) {
			// The value moves along a curve by its length: the length is
			// how far the samples go divided by how far the easing goes.
			auto frames = std::vector<double>();
			frames.reserve(segments * (kLengthSamples + 1));
			for (auto i = 0; i != segments; ++i) {
				const auto t0 = data->keys[i].time;
				const auto t1 = data->keys[i + 1].time;
				for (auto j = 0; j <= kLengthSamples; ++j) {
					frames.push_back(t0 + (t1 - t0) * j / kLengthSamples);
				}
			}
			const auto values = document.valuesAt(ref, frames);
			if (values.size() == frames.size()) {
				for (auto i = 0; i != segments; ++i) {
					const auto &easing = data->keys[i].easing;
					if (easing.hold) {
						continue;
					}
					const auto base = i * (kLengthSamples + 1);
					auto travelled = 0.;
					auto eased = 0.;
					for (auto j = 0; j != kLengthSamples; ++j) {
						travelled += Distance(
							values[base + j],
							values[base + j + 1],
							data->measured);
						eased += std::abs(
							easing.apply(double(j + 1) / kLengthSamples)
								- easing.apply(double(j) / kLengthSamples));
					}
					if (eased > 1e-6) {
						data->length[i] = travelled / eased;
					}
				}
			}
		}
	}
	_data = std::move(data);
}

GraphMath::Segment GraphEditor::segmentFor(int key) const {
	auto result = GraphMath::Segment();
	if (!_data || key < 0 || key >= _data->segments()) {
		return result;
	}
	const auto &a = _data->keys[key];
	const auto &b = _data->keys[key + 1];
	result.t0 = _data->root(a.time);
	result.t1 = _data->root(b.time);
	if (_mode == GraphMode::Value) {
		if (_data->numeric) {
			const auto d = _data->driver[key];
			result.v0 = NumberAt(a.value, d);
			result.v1 = NumberAt(b.value, d);
		} else {
			result.v0 = key;
			result.v1 = key + 1;
		}
	} else if (!_data->numeric) {
		result.v1 = _data->fps;
	} else if (_data->measured > 1) {
		result.v1 = _data->length[key] * _data->fps;
	} else {
		result.v0 = NumberAt(a.value, 0) * _data->fps;
		result.v1 = NumberAt(b.value, 0) * _data->fps;
	}
	return result;
}

void GraphEditor::rebuildPlot() {
	_curves.clear();
	_keys.clear();
	if (!_data || !_data->mapped) {
		return;
	}
	const auto &document = _controller->document();
	const auto plot = plotRect();
	const auto count = int(_data->keys.size());
	const auto segments = _data->segments();
	const auto viewFrom = frameFromX(plot.left());
	const auto viewTill = frameFromX(plot.right() + 1);
	const auto firstRoot = _data->root(_data->keys.front().time);
	const auto lastRoot = _data->root(_data->keys.back().time);

	if (_mode == GraphMode::Value && _data->numeric) {
		auto frames = std::vector<double>();
		const auto samples = std::clamp(plot.width() / 2, 1, kMaxSamples);
		frames.reserve(samples + 1 + 2 * count);
		if (viewTill > viewFrom) {
			for (auto i = 0; i != samples; ++i) {
				frames.push_back(_data->local(
					viewFrom + (viewTill - viewFrom) * i / samples));
			}
		}
		frames.push_back(_data->local(viewTill));
		for (const auto &key : _data->keys) {
			frames.push_back(key.time);
			// Hold segments jump right before the next keyframe.
			frames.push_back(key.time - kTimeEpsilon);
		}
		if (_data->scale < 0.) {
			ranges::sort(frames, std::greater<>());
		} else {
			ranges::sort(frames);
		}
		frames.erase(
			std::unique(begin(frames), end(frames)),
			end(frames));
		const auto values = document.valuesAt(_data->ref, frames);
		if (values.size() == frames.size()) {
			for (auto d = 0; d != _data->dimensions; ++d) {
				auto curve = Curve{ .dimension = d };
				curve.points.reserve(frames.size());
				for (auto i = 0; i != int(frames.size()); ++i) {
					curve.points.push_back(QPointF(
						_data->root(frames[i]),
						NumberAt(values[i], d)));
				}
				_curves.push_back(std::move(curve));
			}
		}
	} else {
		auto curve = Curve();
		const auto speed = (_mode == GraphMode::Speed);
		const auto edge = [&](int key) {
			return speed ? 0. : double(key);
		};
		curve.points.push_back(QPointF(
			std::min(viewFrom, firstRoot),
			edge(0)));
		curve.points.push_back(QPointF(firstRoot, edge(0)));
		for (auto i = 0; i != segments; ++i) {
			const auto segment = segmentFor(i);
			const auto &easing = _data->keys[i].easing;
			const auto duration = segment.t1 - segment.t0;
			for (auto j = 0; j <= kSamplesPerSegment; ++j) {
				const auto u = double(j) / kSamplesPerSegment;
				const auto value = speed
					? GraphMath::SpeedAt(segment, easing, u)
					: (i + (easing.hold ? 0. : easing.apply(u)));
				curve.points.push_back(QPointF(
					segment.t0 + duration * u,
					value));
			}
			if (!speed && easing.hold) {
				curve.points.push_back(QPointF(segment.t1, i + 1.));
			}
		}
		curve.points.push_back(QPointF(lastRoot, edge(count - 1)));
		curve.points.push_back(QPointF(
			std::max(viewTill, lastRoot),
			edge(count - 1)));
		_curves.push_back(std::move(curve));
	}

	// Keyframes and handles.
	_keys.reserve(count);
	for (auto k = 0; k != count; ++k) {
		const auto &key = _data->keys[k];
		auto point = KeyPoint();
		point.frame = _data->root(key.time);
		point.selected = _controller->isKeyframeSelected(
			KeyframeRef{ _data->ref, key.time });
		point.preset = key.last
			? ((k > 0 && _data->keys[k - 1].easing.hold)
				? EasingPreset::Hold
				: EasingPreset::Linear)
			: key.easing.preset();
		point.hasIn = (k > 0);
		point.hasOut = (k + 1 < count);
		const auto before = point.hasIn
			? segmentFor(k - 1)
			: GraphMath::Segment();
		const auto after = point.hasOut
			? segmentFor(k)
			: GraphMath::Segment();
		if (_mode == GraphMode::Value) {
			const auto own = [&](int segment) {
				return _data->numeric
					? NumberAt(key.value, _data->driver[segment])
					: double(k);
			};
			point.in = QPointF(point.frame, point.hasIn
				? own(k - 1)
				: point.hasOut
				? own(k)
				: (_data->numeric ? NumberAt(key.value, 0) : 0.));
			point.out = QPointF(point.frame, point.hasOut
				? own(k)
				: point.in.y());
			if (_data->numeric) {
				for (auto d = 0; d != _data->dimensions; ++d) {
					point.values.push_back(NumberAt(key.value, d));
				}
			}
			if (point.hasIn) {
				const auto &easing = _data->keys[k - 1].easing;
				point.inHandle = easing.hold
					? point.in
					: GraphMath::ValuePoint(before, easing.in);
			}
			if (point.hasOut) {
				point.outHandle = key.easing.hold
					? point.out
					: GraphMath::ValuePoint(after, key.easing.out);
			}
		} else {
			const auto incoming = point.hasIn
				? GraphMath::EndSpeed(before, _data->keys[k - 1].easing)
				: 0.;
			const auto outgoing = point.hasOut
				? GraphMath::StartSpeed(after, key.easing)
				: 0.;
			point.in = QPointF(point.frame, point.hasIn ? incoming : outgoing);
			point.out = QPointF(
				point.frame,
				point.hasOut ? outgoing : incoming);
			if (point.hasIn) {
				const auto &easing = _data->keys[k - 1].easing;
				point.inHandle = easing.hold
					? point.in
					: GraphMath::SpeedPoint(before, easing, false);
			}
			if (point.hasOut) {
				point.outHandle = key.easing.hold
					? point.out
					: GraphMath::SpeedPoint(after, key.easing, true);
			}
		}
		_keys.push_back(point);
	}

	// Vertical range: what is in view, with the handles.
	if (_drag && _drag->type == Drag::Type::Handle) {
		_minValue = _drag->minValue;
		_maxValue = _drag->maxValue;
	} else {
		auto min = std::numeric_limits<double>::max();
		auto max = std::numeric_limits<double>::lowest();
		const auto consider = [&](QPointF point) {
			if (std::isfinite(point.y())) {
				min = std::min(min, point.y());
				max = std::max(max, point.y());
			}
		};
		const auto inView = [&](double frame) {
			return (frame >= viewFrom - 1e-6) && (frame <= viewTill + 1e-6);
		};
		for (const auto &curve : _curves) {
			for (const auto &point : curve.points) {
				if (inView(point.x())) {
					consider(point);
				}
			}
		}
		for (const auto &key : _keys) {
			if (!inView(key.frame)) {
				continue;
			}
			consider(key.in);
			consider(key.out);
			for (const auto value : key.values) {
				consider(QPointF(key.frame, value));
			}
			if (key.hasIn) {
				consider(key.inHandle);
			}
			if (key.hasOut) {
				consider(key.outHandle);
			}
		}
		if (min > max) {
			// Nothing of the animation is in view: its nearest value.
			const auto value = _curves.empty() || _curves.front().points.empty()
				? 0.
				: (viewTill < firstRoot)
				? _curves.front().points.front().y()
				: _curves.front().points.back().y();
			min = max = value;
		}
		if (_mode == GraphMode::Speed) {
			min = std::min(min, 0.);
			max = std::max(max, 0.);
		}
		if (max - min < 1e-6) {
			const auto pad = std::max(std::abs(max) * 0.1, 1.);
			min -= pad;
			max += pad;
		}
		if (!(std::abs(min) <= kMaxPlotValue)
			|| !(std::abs(max) <= kMaxPlotValue)) {
			// Numbers of a broken file: no plot can show them, and nothing
			// here should compute or drag with a range that overflows.
			_curves.clear();
			_keys.clear();
			_minValue = 0.;
			_maxValue = 1.;
			return;
		}
		_minValue = min;
		_maxValue = max;
	}
	const auto pulled = [&](QPointF anchor, QPointF handle) {
		const auto delta = toScreen(handle) - toScreen(anchor);
		return std::hypot(delta.x(), delta.y()) > Scaled(kPulledDistance);
	};
	for (auto &key : _keys) {
		key.inPulled = key.hasIn && pulled(key.in, key.inHandle);
		key.outPulled = key.hasOut && pulled(key.out, key.outHandle);
	}
}

// Layout.

QRect GraphEditor::headerRect() const {
	return QRect(0, 0, width(), Scaled(kHeaderHeight));
}

QRect GraphEditor::plotRect() const {
	const auto top = Scaled(kHeaderHeight);
	return QRect(0, top, width(), std::max(height() - top, 1));
}

bool GraphEditor::controlShown(Control control) const {
	switch (control) {
	case Control::None:
		return false;
	case Control::ModeValue:
	case Control::ModeSpeed:
		return true;
	case Control::MotionPath:
		return _data && _data->pathCapable;
	default:
		return (_data != nullptr);
	}
}

bool GraphEditor::controlActive(Control control) const {
	const auto preset = [&](EasingPreset value) {
		if (!_data) {
			return false;
		}
		auto found = false;
		for (const auto &target : presetTargets()) {
			for (const auto &key : _data->keys) {
				if (std::abs(key.time - target.time) >= kTimeEpsilon
					|| key.last) {
					continue;
				} else if (key.easing.preset() != value) {
					return false;
				}
				found = true;
			}
		}
		return found;
	};
	switch (control) {
	case Control::ModeValue: return (_mode == GraphMode::Value);
	case Control::ModeSpeed: return (_mode == GraphMode::Speed);
	case Control::PresetLinear: return preset(EasingPreset::Linear);
	case Control::PresetEaseIn: return preset(EasingPreset::EaseIn);
	case Control::PresetEaseOut: return preset(EasingPreset::EaseOut);
	case Control::PresetEaseInOut: return preset(EasingPreset::EaseInOut);
	case Control::PresetHold: return preset(EasingPreset::Hold);
	case Control::MotionPath: return _data && _data->info.spatial;
	case Control::None: break;
	}
	return false;
}

QString GraphEditor::controlText(Control control) const {
	switch (control) {
	case Control::ModeValue:
		return tr::lng_oblivion_lottie_graph_value(tr::now);
	case Control::ModeSpeed:
		return tr::lng_oblivion_lottie_graph_speed(tr::now);
	case Control::MotionPath:
		return tr::lng_oblivion_lottie_graph_path(tr::now);
	case Control::PresetLinear:
		return EasingPresetText(EasingPreset::Linear);
	case Control::PresetEaseIn:
		return EasingPresetText(EasingPreset::EaseIn);
	case Control::PresetEaseOut:
		return EasingPresetText(EasingPreset::EaseOut);
	case Control::PresetEaseInOut:
		return EasingPresetText(EasingPreset::EaseInOut);
	case Control::PresetHold:
		return EasingPresetText(EasingPreset::Hold);
	case Control::None:
		break;
	}
	return QString();
}

void GraphEditor::layoutControls() {
	_controls.clear();
	const auto header = headerRect();
	const auto height = Scaled(kPillHeight);
	const auto top = header.y() + (header.height() - height) / 2;
	const auto skip = Scaled(kControlSkip);
	const auto &font = st::normalFont;
	const auto textWidth = [&](Control control) {
		return font->width(controlText(control)) + 2 * Scaled(kPillPadding);
	};
	auto left = Scaled(kPillPadding);
	for (const auto control : { Control::ModeValue, Control::ModeSpeed }) {
		const auto width = textWidth(control);
		_controls.emplace_back(control, QRect(left, top, width, height));
		left += width + skip;
	}
	auto right = header.width() - Scaled(kPillPadding);
	for (const auto control : {
		Control::PresetHold,
		Control::PresetEaseInOut,
		Control::PresetEaseOut,
		Control::PresetEaseIn,
		Control::PresetLinear,
	}) {
		if (!controlShown(control)) {
			continue;
		}
		const auto width = Scaled(kPresetWidth);
		right -= width;
		if (right < left + Scaled(kGroupSkip)) {
			return;
		}
		_controls.emplace_back(control, QRect(right, top, width, height));
		right -= skip;
	}
	if (controlShown(Control::MotionPath)) {
		const auto width = textWidth(Control::MotionPath);
		right -= Scaled(kGroupSkip) - skip + width;
		if (right >= left + Scaled(kGroupSkip)) {
			_controls.emplace_back(
				Control::MotionPath,
				QRect(right, top, width, height));
		}
	}
}

QRect GraphEditor::controlRect(Control control) const {
	for (const auto &[type, rect] : _controls) {
		if (type == control) {
			return rect;
		}
	}
	return QRect();
}

double GraphEditor::xFromFrame(double rootFrame) const {
	return _timeLeft
		+ (rootFrame - _viewStart) * (_timeRight - _timeLeft) / _viewSpan;
}

double GraphEditor::frameFromX(double x) const {
	return _viewStart
		+ (x - _timeLeft) * _viewSpan / std::max(_timeRight - _timeLeft, 1);
}

double GraphEditor::yFromValue(double value) const {
	const auto plot = plotRect();
	const auto padding = std::min(Scaled(kPlotPadding), plot.height() / 4);
	const auto inner = std::max(plot.height() - 2 * padding, 1);
	const auto span = std::max(_maxValue - _minValue, 1e-9);
	return plot.y() + padding + inner * (_maxValue - value) / span;
}

double GraphEditor::valueFromY(double y) const {
	const auto plot = plotRect();
	const auto padding = std::min(Scaled(kPlotPadding), plot.height() / 4);
	const auto inner = std::max(plot.height() - 2 * padding, 1);
	const auto span = std::max(_maxValue - _minValue, 1e-9);
	return _maxValue - (y - plot.y() - padding) * span / inner;
}

QPointF GraphEditor::toScreen(QPointF data) const {
	return QPointF(xFromFrame(data.x()), yFromValue(data.y()));
}

QPointF GraphEditor::fromScreen(QPointF point) const {
	return QPointF(frameFromX(point.x()), valueFromY(point.y()));
}

void GraphEditor::resizeEvent(QResizeEvent *e) {
	invalidate();
}

// Interaction.

auto GraphEditor::hitTest(QPoint point) -> Hit {
	ensureData();
	for (const auto &[control, rect] : _controls) {
		if (rect.contains(point)) {
			return { .type = HitType::Control, .control = control };
		}
	}
	const auto plot = plotRect();
	if (!_data || !plot.contains(point)) {
		return {};
	}
	const auto radius = double(Scaled(kHitRadius));
	const auto distance = [&](QPointF data) {
		const auto delta = toScreen(data) - QPointF(point);
		return std::hypot(delta.x(), delta.y());
	};
	auto result = Hit();
	auto best = radius;
	// Handles of every keyframe when none is selected, otherwise only of
	// the selected ones (as they are painted).
	const auto anySelected = ranges::any_of(_keys, &KeyPoint::selected);
	for (auto k = 0; k != int(_keys.size()); ++k) {
		const auto &key = _keys[k];
		if (anySelected && !key.selected) {
			continue;
		}
		if (key.outPulled) {
			const auto d = distance(key.outHandle);
			if (d <= best) {
				best = d;
				result = { .type = HitType::Handle, .key = k, .out = true };
			}
		}
		if (key.inPulled) {
			const auto d = distance(key.inHandle);
			if (d <= best) {
				best = d;
				result = { .type = HitType::Handle, .key = k, .out = false };
			}
		}
	}
	if (result.type != HitType::None) {
		return result;
	}
	for (auto k = 0; k != int(_keys.size()); ++k) {
		const auto &key = _keys[k];
		auto d = std::min(distance(key.in), distance(key.out));
		for (const auto value : key.values) {
			d = std::min(d, distance(QPointF(key.frame, value)));
		}
		if (d <= best) {
			best = d;
			result = { .type = HitType::Keyframe, .key = k };
		}
	}
	if (result.type != HitType::None) {
		return result;
	}
	const auto playhead = xFromFrame(_controller->currentFrame());
	if (std::abs(point.x() - playhead) <= Scaled(3)) {
		return { .type = HitType::Playhead };
	}
	return {};
}

void GraphEditor::setOver(Hit hit) {
	if (_over == hit) {
		return;
	}
	_over = hit;
	switch (hit.type) {
	case HitType::Control:
	case HitType::Keyframe:
		setCursor(style::cur_pointer);
		break;
	case HitType::Handle:
		setCursor(Qt::SizeAllCursor);
		break;
	case HitType::Playhead:
		setCursor(style::cur_sizehor);
		break;
	case HitType::None:
		setCursor(style::cur_default);
		break;
	}
	if (hit.type == HitType::Control) {
		Ui::Tooltip::Show(kTooltipDelay, this);
	} else {
		Ui::Tooltip::Hide();
	}
	update();
}

QString GraphEditor::tooltipText() const {
	if (_over.type != HitType::Control) {
		return QString();
	}
	switch (_over.control) {
	case Control::ModeValue:
		return (_data && !_data->numeric)
			? tr::lng_oblivion_lottie_graph_value_progress_about(tr::now)
			: tr::lng_oblivion_lottie_graph_value_about(tr::now);
	case Control::ModeSpeed:
		return tr::lng_oblivion_lottie_graph_speed_about(tr::now);
	case Control::MotionPath:
		return tr::lng_oblivion_lottie_graph_path_about(tr::now);
	case Control::None:
		return QString();
	default:
		return controlText(_over.control);
	}
}

QPoint GraphEditor::tooltipPos() const {
	return QCursor::pos();
}

bool GraphEditor::tooltipWindowActive() const {
	return Ui::AppInFocus() && Ui::InFocusChain(window());
}

std::vector<KeyframeRef> GraphEditor::presetTargets() const {
	auto result = std::vector<KeyframeRef>();
	if (!_data) {
		return result;
	}
	for (const auto &keyframe : _controller->selectedKeyframes()) {
		if (keyframe.property == _data->ref) {
			result.push_back(keyframe);
		}
	}
	if (result.empty()) {
		for (const auto &key : _data->keys) {
			if (!key.last) {
				result.push_back(KeyframeRef{ _data->ref, key.time });
			}
		}
	}
	return result;
}

void GraphEditor::applyPreset(EasingPreset preset) {
	const auto targets = presetTargets();
	if (!targets.empty()) {
		_controller->setKeyframeEasing(targets, Easing::FromPreset(preset));
	}
}

void GraphEditor::clickControl(Control control) {
	switch (control) {
	case Control::ModeValue: setMode(GraphMode::Value); break;
	case Control::ModeSpeed: setMode(GraphMode::Speed); break;
	case Control::PresetLinear: applyPreset(EasingPreset::Linear); break;
	case Control::PresetEaseIn: applyPreset(EasingPreset::EaseIn); break;
	case Control::PresetEaseOut: applyPreset(EasingPreset::EaseOut); break;
	case Control::PresetEaseInOut: applyPreset(EasingPreset::EaseInOut); break;
	case Control::PresetHold: applyPreset(EasingPreset::Hold); break;
	case Control::MotionPath:
		if (_data) {
			_controller->setMotionPath(_data->ref, !_data->info.spatial);
		}
		break;
	case Control::None:
		break;
	}
}

void GraphEditor::pressKeyframe(int key, Qt::KeyboardModifiers modifiers) {
	if (!_data || key < 0 || key >= int(_data->keys.size())) {
		return;
	}
	const auto ref = KeyframeRef{ _data->ref, _data->keys[key].time };
	const auto toggle = (modifiers & Qt::ControlModifier) != 0;
	const auto add = (modifiers & Qt::ShiftModifier) != 0;
	_controller->setActiveProperty(_data->ref);
	auto selected = _controller->selectedKeyframes();
	const auto already = _controller->isKeyframeSelected(ref);
	if (toggle && already) {
		selected.erase(
			ranges::remove_if(selected, [&](const KeyframeRef &item) {
				return (item.property == ref.property)
					&& std::abs(item.time - ref.time) < kTimeEpsilon;
			}),
			end(selected));
		_controller->setSelectedKeyframes(std::move(selected));
	} else if (toggle || add) {
		if (!already) {
			selected.push_back(ref);
			_controller->setSelectedKeyframes(std::move(selected));
		}
	} else if (!already) {
		_controller->setSelectedKeyframes({ ref });
	}
}

void GraphEditor::startHandleDrag(int key, bool out) {
	ensureData();
	if (!_data
		|| !_drag
		|| key < 0
		|| key >= int(_keys.size())
		|| (out ? !_keys[key].hasOut : !_keys[key].hasIn)) {
		_drag = nullptr;
		return;
	}
	const auto segment = out ? key : (key - 1);
	const auto &easing = _data->keys[segment].easing;
	_controller->setPlaying(false);
	_controller->setActiveProperty(_data->ref);
	_drag->type = Drag::Type::Handle;
	_drag->key = key;
	_drag->out = out;
	_drag->keyframe = KeyframeRef{ _data->ref, _data->keys[key].time };
	_drag->start = _controller->document();
	_drag->segment = segmentFor(segment);
	_drag->fallback = easing.hold
		? (out ? QPointF(0., 0.) : QPointF(1., 1.))
		: (out ? easing.out : easing.in);
	_drag->minValue = _minValue;
	_drag->maxValue = _maxValue;
	_drag->mergeKey = "graph-handle-" + QByteArray::number(++_gestures);
	_controller->beginGesture(_drag->mergeKey);
}

void GraphEditor::applyHandleDrag(QPointF point) {
	if (!_drag || _drag->type != Drag::Type::Handle || !_data) {
		return;
	}
	const auto data = fromScreen(point);
	auto handle = (_mode == GraphMode::Value)
		? GraphMath::ValueHandle(_drag->segment, data, _drag->fallback)
		: GraphMath::SpeedHandle(
			_drag->segment,
			data,
			_drag->out,
			_drag->fallback);
	const auto &limits = _data->limits;
	handle = QPointF(
		std::clamp(handle.x(), 0., 1.),
		std::clamp(handle.y(), limits.minY, limits.maxY));
	if (!std::isfinite(handle.x()) || !std::isfinite(handle.y())) {
		return;
	}
	auto edit = SetKeyframeHandles(
		_drag->start,
		_drag->keyframe,
		_drag->out ? std::nullopt : std::make_optional(handle),
		_drag->out ? std::make_optional(handle) : std::nullopt);
	if (_controller->perform(
			Command::SetEasing,
			std::move(edit),
			_drag->mergeKey)) {
		_drag->applied = true;
	}
}

void GraphEditor::finishDrag(bool cancel) {
	if (!_drag) {
		return;
	}
	const auto drag = std::move(_drag);
	if (drag->type == Drag::Type::Handle) {
		if (drag->applied) {
			if (cancel) {
				_controller->cancelGesture(drag->mergeKey);
			} else {
				_controller->finishMerge();
			}
		}
		invalidate();
	}
}

void GraphEditor::mousePressEvent(QMouseEvent *e) {
	_menu = nullptr;
	if (_drag || e->button() != Qt::LeftButton) {
		return;
	}
	const auto hit = hitTest(e->pos());
	switch (hit.type) {
	case HitType::Control:
		clickControl(hit.control);
		return;
	case HitType::Handle:
		_drag = std::make_unique<Drag>();
		_drag->press = e->pos();
		startHandleDrag(hit.key, hit.out);
		return;
	case HitType::Keyframe:
		pressKeyframe(hit.key, e->modifiers());
		if (!(e->modifiers() & Qt::ControlModifier)
			&& _data
			&& _data->mapped) {
			_drag = std::make_unique<Drag>();
			_drag->type = Drag::Type::Keyframe;
			_drag->press = e->pos();
			_drag->key = hit.key;
		}
		return;
	case HitType::Playhead:
		_controller->setPlaying(false);
		_drag = std::make_unique<Drag>();
		_drag->type = Drag::Type::Scrub;
		_drag->press = e->pos();
		return;
	case HitType::None:
		if (plotRect().contains(e->pos())
			&& !(e->modifiers()
				& (Qt::ShiftModifier | Qt::ControlModifier))) {
			_controller->setSelectedKeyframes({});
		}
		return;
	}
}

void GraphEditor::mouseMoveEvent(QMouseEvent *e) {
	if (!_drag) {
		setOver(hitTest(e->pos()));
		return;
	}
	switch (_drag->type) {
	case Drag::Type::Keyframe: {
		const auto delta = e->pos().x() - _drag->press.x();
		if (std::abs(delta) < Scaled(kDragThreshold)) {
			return;
		}
		const auto key = _drag->key;
		ensureData();
		if (key < 0 || key >= int(_keys.size())) {
			_drag = nullptr;
			return;
		}
		const auto out = (delta > 0)
			? _keys[key].hasOut
			: !_keys[key].hasIn;
		startHandleDrag(key, out);
		applyHandleDrag(e->position());
	} break;
	case Drag::Type::Handle:
		applyHandleDrag(e->position());
		break;
	case Drag::Type::Scrub:
		_controller->setCurrentFrame(
			int(std::floor(frameFromX(e->pos().x()) + 0.5)));
		break;
	}
}

void GraphEditor::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton || !_drag) {
		return;
	}
	finishDrag(false);
	setOver(hitTest(e->pos()));
}

void GraphEditor::mouseDoubleClickEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	finishDrag(false);
	const auto hit = hitTest(e->pos());
	if (hit.type == HitType::Keyframe
		&& hit.key >= 0
		&& hit.key < int(_keys.size())) {
		_controller->setPlaying(false);
		_controller->setCurrentFrame(
			int(std::round(_keys[hit.key].frame)));
	} else if (hit.type == HitType::Control) {
		clickControl(hit.control);
	}
}

void GraphEditor::wheelEvent(QWheelEvent *e) {
	// The timeline under the graph scrolls and zooms the time.
	e->ignore();
}

void GraphEditor::contextMenuEvent(QContextMenuEvent *e) {
	if (_drag) {
		return;
	}
	const auto hit = hitTest(e->pos());
	if (hit.type == HitType::Keyframe && _data) {
		const auto ref = KeyframeRef{
			_data->ref,
			_data->keys[hit.key].time,
		};
		if (!_controller->isKeyframeSelected(ref)) {
			pressKeyframe(hit.key, Qt::NoModifier);
		}
		showKeyframeMenu(e->globalPos());
	}
	e->accept();
}

void GraphEditor::showKeyframeMenu(QPoint globalPosition) {
	ensureData();
	if (!_data) {
		return;
	}
	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuWithIcons);
	const auto add = [&](EasingPreset preset, Control control) {
		_menu->addAction(
			EasingPresetText(preset),
			[=] { applyPreset(preset); },
			controlActive(control) ? &st::mediaPlayerMenuCheck : nullptr);
	};
	add(EasingPreset::Linear, Control::PresetLinear);
	add(EasingPreset::EaseIn, Control::PresetEaseIn);
	add(EasingPreset::EaseOut, Control::PresetEaseOut);
	add(EasingPreset::EaseInOut, Control::PresetEaseInOut);
	add(EasingPreset::Hold, Control::PresetHold);
	const auto selected = _controller->selectedKeyframes();
	if (selected.size() == 1 && _data->mapped) {
		const auto frame = int(std::round(
			_data->root(selected.front().time)));
		_menu->addSeparator();
		_menu->addAction(
			tr::lng_oblivion_lottie_timeline_go_to(tr::now),
			[=] {
				_controller->setPlaying(false);
				_controller->setCurrentFrame(frame);
			},
			&st::menuIconTimer);
	}
	_menu->popup(globalPosition);
}

void GraphEditor::leaveEventHook(QEvent *e) {
	if (!_drag) {
		setOver(Hit());
	}
	RpWidget::leaveEventHook(e);
}

// Painting.

void GraphEditor::paintEvent(QPaintEvent *e) {
	ensureData();
	auto p = QPainter(this);
	p.fillRect(e->rect(), st::windowBg);
	const auto plot = plotRect();
	// The playhead is drawn in the plot only: across the header it would
	// cut through the title text and hide behind the buttons in pieces.
	paintHeader(p);
	if (!_data || !_data->mapped || _keys.empty()) {
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		const auto margin = Scaled(kPlotPadding);
		p.drawText(
			plot.marginsRemoved(QMargins(margin, margin, margin, margin)),
			Qt::AlignCenter | Qt::TextWordWrap,
			tr::lng_oblivion_lottie_graph_empty(tr::now));
		paintPlayhead(p, plot);
		return;
	}
	p.save();
	p.setClipRect(plot);
	paintGrid(p, plot);
	paintCurves(p, plot);
	paintKeys(p, plot);
	p.restore();
	paintPlayhead(p, plot);
}

void GraphEditor::paintHeader(QPainter &p) {
	const auto header = headerRect();
	p.fillRect(
		header.x(),
		header.bottom() + 1 - st::lineWidth,
		header.width(),
		st::lineWidth,
		anim::with_alpha(st::shadowFg->c, 0.5));
	auto hq = PainterHighQualityEnabler(p);
	const auto &font = st::normalFont;
	auto titleLeft = Scaled(kPillPadding);
	auto titleRight = header.width() - Scaled(kPillPadding);
	for (const auto &[control, rect] : _controls) {
		const auto over = (_over.type == HitType::Control)
			&& (_over.control == control);
		const auto active = controlActive(control);
		const auto radius = rect.height() / 2.;
		// "Value" / "Speed" are the two tabs of the graph and look like
		// the tabs of the inspector: a pill under the chosen one, the
		// other is a plain gray name. (As two pills they differed only in
		// a tint, and in the night theme the chosen one was the dimmer.)
		// The rest are buttons: always a pill, tinted while switched on.
		const auto tab = (control == Control::ModeValue)
			|| (control == Control::ModeSpeed);
		if (!tab || active) {
			p.setPen(Qt::NoPen);
			p.setBrush(tab
				? st::windowBgOver
				: active
				? st::lightButtonBgOver
				: over
				? st::windowBgRipple
				: st::windowBgOver);
			p.drawRoundedRect(QRectF(rect), radius, radius);
		}
		const auto color = active
			? st::windowActiveTextFg->c
			: over
			? st::windowBoldFg->c
			: tab
			? st::windowSubTextFg->c
			: st::windowFg->c;
		const auto text = tab || (control == Control::MotionPath);
		if (text) {
			p.setFont(font);
			p.setPen(color);
			p.drawText(rect, controlText(control), style::al_center);
			if (control != Control::MotionPath) {
				titleLeft = std::max(titleLeft, rect.right() + 1);
			} else {
				titleRight = std::min(titleRight, rect.left());
			}
			continue;
		}
		titleRight = std::min(titleRight, rect.left());
		// A small picture of the easing curve.
		const auto box = QRectF(rect).marginsRemoved(QMarginsF(
			Scaled(8),
			Scaled(6),
			Scaled(8),
			Scaled(6)));
		const auto from = box.bottomLeft();
		const auto to = box.topRight();
		const auto at = [&](double x, double y) {
			return QPointF(
				box.left() + box.width() * x,
				box.bottom() - box.height() * y);
		};
		auto path = QPainterPath();
		path.moveTo(from);
		switch (control) {
		case Control::PresetLinear:
			path.lineTo(to);
			break;
		case Control::PresetEaseIn:
			path.cubicTo(at(0.6, 0.), at(1., 1.), to);
			break;
		case Control::PresetEaseOut:
			path.cubicTo(at(0., 0.), at(0.4, 1.), to);
			break;
		case Control::PresetEaseInOut:
			path.cubicTo(at(0.6, 0.), at(0.4, 1.), to);
			break;
		case Control::PresetHold:
			path.lineTo(at(1., 0.));
			path.lineTo(to);
			break;
		default:
			break;
		}
		auto pen = QPen(color);
		pen.setWidthF(Scaled(15) / 10.);
		pen.setCapStyle(Qt::RoundCap);
		pen.setJoinStyle(Qt::RoundJoin);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawPath(path);
	}
	if (_data) {
		const auto left = titleLeft + Scaled(kGroupSkip);
		const auto available = titleRight - Scaled(kGroupSkip) - left;
		if (available > Scaled(40)) {
			// What the graph shows. A short line parts it from the tabs:
			// the tab that is not chosen is a plain name too.
			const auto separator = Scaled(14);
			p.fillRect(
				titleLeft + Scaled(kGroupSkip) / 2,
				header.y() + (header.height() - separator) / 2,
				st::lineWidth,
				separator,
				anim::with_alpha(st::windowSubTextFg->c, 0.4));
			p.setFont(font);
			p.setPen(st::windowFg);
			p.drawText(
				QRect(left, header.y(), available, header.height()),
				Qt::AlignLeft | Qt::AlignVCenter,
				font->elided(_data->title, available));
		}
	}
}

void GraphEditor::paintGrid(QPainter &p, const QRect &plot) {
	const auto labels = SmallFont();
	const auto span = _maxValue - _minValue;
	const auto ticks = std::max(plot.height() / Scaled(kLabelSpacing), 2);
	const auto progress = (_mode == GraphMode::Value) && !_data->numeric;
	auto step = GraphMath::NiceStep(span, ticks);
	if (progress) {
		step = std::max(std::ceil(step), 1.);
	}
	const auto decimals = (step >= 1. || !(step > 0.))
		? 0
		: std::clamp(int(std::ceil(-std::log10(step))), 1, 4);
	const auto line = st::lineWidth;
	const auto sub = st::windowSubTextFg->c;
	p.setFont(labels);

	// The first keyframes of an animation stand where the numbers are
	// written, and a keyframe on a round value is right on a grid line: its
	// mark would cover the number. The mark stays (it is what gets clicked
	// and dragged), the number steps aside: higher above its line, or under
	// the line when the mark is above it.
	const auto labelLeft = plot.left() + Scaled(6);
	const auto reach = Scaled(kKeyRadius) * 1.15 + Scaled(1);
	auto marks = std::vector<QRectF>();
	const auto addMark = [&](QPointF center) {
		if (std::isfinite(center.y())
			&& (center.x() + reach > labelLeft)
			&& (center.x() - reach < labelLeft + Scaled(kLabelRoom))) {
			marks.emplace_back(
				center.x() - reach,
				center.y() - reach,
				2 * reach,
				2 * reach);
		}
	};
	for (const auto &key : _keys) {
		if (!key.values.empty()) {
			for (const auto value : key.values) {
				addMark(toScreen(QPointF(key.frame, value)));
			}
			continue;
		}
		const auto in = toScreen(key.in);
		const auto out = toScreen(key.out);
		addMark(in);
		if (std::abs(in.y() - out.y()) > 1.) {
			addMark(out);
		}
	}
	const auto labelTopFor = [&](int top, int lineTop, int width) {
		const auto clearAt = [&](int candidate) {
			const auto box = QRectF(
				labelLeft,
				candidate,
				width,
				labels->ascent);
			return (candidate >= plot.top())
				&& (candidate + labels->height <= plot.bottom() + 1)
				&& ranges::none_of(marks, [&](const QRectF &mark) {
					return mark.intersects(box);
				});
		};
		const auto descent = labels->height - labels->ascent;
		const auto full = int(std::ceil(reach)) + Scaled(1);
		const auto slight = std::clamp(full - Scaled(1) - descent, 1, full);
		const auto under = lineTop + line + Scaled(1);
		for (const auto candidate : {
			top,
			top - slight,
			top - full,
			under,
			under + slight,
		}) {
			if (clearAt(candidate)) {
				return candidate;
			}
		}
		return top;
	};

	for (const auto value : GraphMath::GridValues(
			_minValue,
			_maxValue,
			step)) {
		const auto y = yFromValue(value);
		if (!(y >= plot.top() - 1) || !(y <= plot.bottom() + 1)) {
			continue;
		}
		const auto zero = (std::abs(value) < step / 2.)
			&& (_mode == GraphMode::Speed);
		// A line too close to the header has no room for its number: it
		// would be a second line right under the header's own, about
		// nothing. The zero of the speed graph is always worth a line.
		const auto top = int(std::round(y)) - labels->height - Scaled(1);
		const auto fits = (top >= plot.top());
		if (!fits && !zero) {
			continue;
		}
		p.fillRect(
			QRectF(plot.left(), std::round(y), plot.width(), line),
			anim::with_alpha(sub, zero ? 0.45 : 0.14));
		auto text = QString();
		if (progress) {
			const auto index = int(std::round(value));
			if (index < 0 || index >= int(_keys.size())) {
				continue;
			}
			text = tr::lng_oblivion_lottie_graph_key(
				tr::now,
				lt_value,
				QString::number(index + 1));
		} else {
			text = FormatDecimal(value, decimals);
		}
		if (!fits) {
			continue;
		}
		const auto labelTop = marks.empty()
			? top
			: labelTopFor(top, int(std::round(y)), labels->width(text));
		p.setPen(anim::with_alpha(sub, 0.9));
		p.drawText(labelLeft, labelTop + labels->ascent, text);
	}

	// What the vertical axis shows, with the curve colors of X / Y.
	auto caption = progress
		? tr::lng_oblivion_lottie_graph_axis_progress(tr::now)
		: (_mode == GraphMode::Value)
		? tr::lng_oblivion_lottie_graph_axis_value(tr::now)
		: _data->numeric
		? tr::lng_oblivion_lottie_graph_axis_speed(tr::now)
		: tr::lng_oblivion_lottie_graph_axis_speed_progress(tr::now);
	const auto captionWidth = labels->width(caption);
	const auto dimensions = (_mode == GraphMode::Value)
		&& (_curves.size() > 1);
	auto right = plot.right() - Scaled(8);
	const auto captionTop = plot.top() + Scaled(6);
	const auto baseline = captionTop + labels->ascent;
	{
		// A grid line that happens to be at this height (the 100% of an
		// opacity is one) must not strike the text through.
		auto legendWidth = captionWidth;
		if (dimensions) {
			legendWidth += Scaled(10) - Scaled(8);
			for (const auto &curve : _curves) {
				legendWidth += labels->width(DimensionName(curve.dimension))
					+ Scaled(8);
			}
		}
		const auto pad = Scaled(4);
		p.fillRect(
			QRect(
				right - legendWidth - pad,
				captionTop - Scaled(2),
				legendWidth + 2 * pad,
				labels->height + 2 * Scaled(2)),
			st::windowBg);
	}
	p.setPen(sub);
	p.drawText(right - captionWidth, baseline, caption);
	right -= captionWidth + Scaled(10);
	if (dimensions) {
		for (auto i = int(_curves.size()); i != 0;) {
			--i;
			const auto name = DimensionName(_curves[i].dimension);
			const auto width = labels->width(name);
			p.setPen(DimensionColor(
				_curves[i].dimension,
				int(_curves.size())));
			p.drawText(right - width, baseline, name);
			right -= width + Scaled(8);
		}
	}
}

void GraphEditor::paintCurves(QPainter &p, const QRect &plot) {
	auto hq = PainterHighQualityEnabler(p);
	p.setBrush(Qt::NoBrush);
	const auto count = int(_curves.size());
	for (const auto &curve : _curves) {
		if (curve.points.size() < 2) {
			continue;
		}
		auto path = QPainterPath();
		auto started = false;
		for (const auto &point : curve.points) {
			const auto screen = toScreen(point);
			if (!std::isfinite(screen.x()) || !std::isfinite(screen.y())) {
				continue;
			}
			// Far points would make huge coordinates for the rasterizer.
			const auto bounded = QPointF(
				std::clamp(screen.x(), -1e5, 1e5),
				std::clamp(screen.y(), -1e5, 1e5));
			if (started) {
				path.lineTo(bounded);
			} else {
				path.moveTo(bounded);
				started = true;
			}
		}
		auto pen = QPen(DimensionColor(curve.dimension, count)->c);
		pen.setWidthF(Scaled(2) * 0.9);
		pen.setJoinStyle(Qt::RoundJoin);
		pen.setCapStyle(Qt::RoundCap);
		p.setPen(pen);
		p.drawPath(path);
	}
}

void GraphEditor::paintKeys(QPainter &p, const QRect &plot) {
	auto hq = PainterHighQualityEnabler(p);
	const auto accent = st::windowActiveTextFg->c;
	const auto sub = st::windowSubTextFg->c;
	const auto anySelected = ranges::any_of(_keys, &KeyPoint::selected);
	const auto dragged = (_drag && _drag->type == Drag::Type::Handle)
		? _drag->key
		: -1;
	const auto paintHandle = [&](int index, bool out) {
		const auto &key = _keys[index];
		if (out ? !key.outPulled : !key.inPulled) {
			return;
		}
		const auto anchor = toScreen(out ? key.out : key.in);
		const auto handle = toScreen(out ? key.outHandle : key.inHandle);
		const auto active = (_over.type == HitType::Handle
			&& _over.key == index
			&& _over.out == out)
			|| (dragged == index && _drag->out == out);
		auto line = QPen(anim::with_alpha(active ? accent : sub, 0.8));
		line.setWidthF(Scaled(1) * 1.);
		p.setPen(line);
		p.drawLine(anchor, handle);
		auto border = QPen(accent);
		border.setWidthF(Scaled(15) / 10.);
		p.setPen(border);
		p.setBrush(active ? QBrush(accent) : st::windowBg->b);
		const auto radius = Scaled(kHandleRadius) * 1.;
		p.drawEllipse(handle, radius, radius);
	};
	for (auto k = 0; k != int(_keys.size()); ++k) {
		if (anySelected && !_keys[k].selected) {
			continue;
		}
		paintHandle(k, false);
		paintHandle(k, true);
	}
	for (auto k = 0; k != int(_keys.size()); ++k) {
		const auto &key = _keys[k];
		const auto over = (_over.type == HitType::Keyframe)
			&& (_over.key == k);
		const auto half = Scaled(kKeyRadius) + (over ? 1. : 0.);
		const auto in = toScreen(key.in);
		const auto out = toScreen(key.out);
		if (key.values.empty() && std::abs(in.y() - out.y()) > 1.) {
			auto link = QPen(anim::with_alpha(sub, 0.6));
			link.setWidthF(Scaled(1) * 1.);
			link.setStyle(Qt::DotLine);
			p.setPen(link);
			p.drawLine(in, out);
		}
		auto border = QPen(st::windowBg->c);
		border.setWidthF(Scaled(1) * 1.);
		p.setPen(border);
		p.setBrush(key.selected ? accent : sub);
		const auto mark = [&](QPointF center) {
			switch (key.preset) {
			case EasingPreset::Hold:
				p.drawRoundedRect(
					QRectF(
						center.x() - half * 0.85,
						center.y() - half * 0.85,
						half * 1.7,
						half * 1.7),
					half * 0.25,
					half * 0.25);
				break;
			case EasingPreset::Linear:
				p.drawPolygon(QPolygonF({
					center + QPointF(0., -half * 1.15),
					center + QPointF(half * 1.15, 0.),
					center + QPointF(0., half * 1.15),
					center + QPointF(-half * 1.15, 0.),
				}));
				break;
			default:
				p.drawEllipse(center, half * 0.95, half * 0.95);
				break;
			}
		};
		if (!key.values.empty()) {
			// The keyframe is seen on every curve of the value.
			for (const auto value : key.values) {
				mark(toScreen(QPointF(key.frame, value)));
			}
			continue;
		}
		mark(in);
		if (std::abs(in.y() - out.y()) > 1.) {
			mark(out);
		}
	}
}

void GraphEditor::paintPlayhead(QPainter &p, const QRect &area) {
	const auto x = xFromFrame(_controller->currentFrame());
	if (x < area.left() || x > area.right() + 1) {
		return;
	}
	const auto line = Scaled(1) * 1.5;
	p.fillRect(
		QRectF(x - line / 2., area.top(), line, area.height()),
		st::windowActiveTextFg->c);
}

// Snapshot scenes (OBLIVION_SELFTEST=ui, see oblivion_ui_snapshots.h).

namespace {

[[nodiscard]] QWidget *CreateGraphScene(not_null<Ui::RpWidget*> parent) {
	return Ui::CreateChild<PanelSceneHost>(
		parent.get(),
		u":/animations/palette.tgs"_q,
		[](QWidget *parent, not_null<EditorController*> controller) {
			return not_null<Ui::RpWidget*>(
				Ui::CreateChild<TimelinePanel>(parent, controller));
		});
}

// A point property with several keyframes (two curves and handles to
// show), the layer that owns it gets selected.
void PrepareGraphScene(not_null<QWidget*> widget, GraphMode mode) {
	const auto host = static_cast<PanelSceneHost*>(widget.get());
	const auto controller = host->controller();
	const auto timeline = static_cast<TimelinePanel*>(host->panel().get());
	const auto &document = controller->document();
	auto chosen = PropertyRef();
	auto best = 0;
	for (const auto layer : document.layers()) {
		for (const auto &ref : document.animatedProperties(layer)) {
			const auto info = document.property(ref);
			if (!info || ref.node != layer) {
				continue;
			}
			const auto score = info->keyframes
				+ ((info->type == PropertyType::Vector) ? 100 : 0);
			if (info->keyframes >= 3 && score > best) {
				best = score;
				chosen = ref;
			}
		}
	}
	if (chosen) {
		controller->select(chosen.node);
		controller->setActiveProperty(chosen);
		const auto times = document.keyframeTimes(chosen);
		if (times.size() > 1) {
			controller->setSelectedKeyframes({
				KeyframeRef{ chosen, times[1] },
			});
			controller->setCurrentFrame(int(std::round(
				(times[0] + times[1]) / 2.)));
		}
	}
	timeline->graph()->setMode(mode);
	timeline->setGraphShown(true);
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	// The value graph of a position: X and Y curves, the handles of the
	// selected keyframe, the presets above.
	RegisterScene(
		u"lottie_graph"_q,
		QSize(1100, 360),
		CreateGraphScene,
		[](not_null<QWidget*> widget) {
			PrepareGraphScene(widget, GraphMode::Value);
		});

	RegisterScene(
		u"lottie_graph_speed"_q,
		QSize(1100, 360),
		CreateGraphScene,
		[](not_null<QWidget*> widget) {
			PrepareGraphScene(widget, GraphMode::Speed);
		});

	// Nothing to show: the hint in place of the plot.
	RegisterScene(
		u"lottie_graph_empty"_q,
		QSize(900, 300),
		CreateGraphScene,
		[](not_null<QWidget*> widget) {
			const auto host = static_cast<PanelSceneHost*>(widget.get());
			const auto timeline = static_cast<TimelinePanel*>(
				host->panel().get());
			host->controller()->setActiveProperty(std::nullopt);
			// The mode is remembered for the app session: without this the
			// picture would depend on the scene rendered before it.
			timeline->graph()->setMode(GraphMode::Value);
			timeline->setGraphShown(true);
		});
});

} // namespace

} // namespace Oblivion::LottieEdit
