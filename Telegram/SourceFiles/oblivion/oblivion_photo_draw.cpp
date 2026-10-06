/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_photo_draw.h"

#include "base/platform/base_platform_info.h"
#include "base/timer.h"
#include "lang/lang_keys.h"
#include "oblivion/oblivion_photo_editor.h"
#include "oblivion/oblivion_photo_editor_controls.h"
#include "oblivion/oblivion_photo_panels.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/emoji_config.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_basic.h"
#include "styles/style_calls.h"
#include "styles/style_widgets.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtGui/QFontMetricsF>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainterPath>

#include <array>
#include <mutex>

namespace Oblivion::Photo {
namespace {

// The geometry, the contents and their rendering: no widgets, no fonts,
// no lang strings, any thread. The self-test covers this part.

constexpr auto kPi = 3.14159265358979323846;
constexpr auto kMinWidth = 0.1;
constexpr auto kMaxWidth = 4096.;
constexpr auto kMaxCoordinate = 1e6;
constexpr auto kMaxShapes = 5000;
constexpr auto kMaxShapePoints = 8000;
constexpr auto kMaxStrokePoints = 4000;
constexpr auto kMinPointFactor = 0.05;

// Captured strokes keep a point every two screen pixels (at the zoom the
// stroke was drawn with), the smoothing window is up to 28 screen pixels
// to each side.
constexpr auto kResampleScreenStep = 2.;
constexpr auto kSmoothingScreenRadius = 28.;

// "Thinner when fast": screen pixels per millisecond.
constexpr auto kDynamicWindow = 3;
constexpr auto kDynamicMinFactor = 0.35;
constexpr auto kDynamicSlowSpeed = 0.15;
constexpr auto kDynamicFastSpeed = 1.8;
constexpr auto kDynamicSubdivisions = 3;
constexpr auto kDynamicMinRadius = 0.05;

constexpr auto kArrowHeadAngle = 28. * kPi / 180.;
constexpr auto kArrowHeadWidths = 4.5;
constexpr auto kArrowHeadMaxPart = 0.6;

constexpr auto kPencilGrainPart = 0.22;
constexpr auto kPencilGrainMin = 1.;
constexpr auto kPencilBase = 0.35;
constexpr auto kPencilBandPixels = 4 * 1024 * 1024;
constexpr auto kPencilMinBand = 64;

constexpr auto kCacheEntries = 3;
constexpr auto kCacheMaxPixels = qint64(6) * 1024 * 1024;

constexpr auto kMaxTextLength = 2000;
constexpr auto kMaxTextLines = 60;
constexpr auto kMaxTextSide = 16384;
constexpr auto kMinTextSize = 4.;
constexpr auto kMaxTextSize = 4096.;
constexpr auto kLayerNameLength = 24;

[[nodiscard]] double Distance(QPointF a, QPointF b) {
	return std::hypot(a.x() - b.x(), a.y() - b.y());
}

[[nodiscard]] bool Finite(QPointF point) {
	return std::isfinite(point.x()) && std::isfinite(point.y());
}

[[nodiscard]] QPointF PointOf(const DrawPoint &point) {
	return QPointF(point.x, point.y);
}

[[nodiscard]] DrawPoint MakePoint(QPointF point, double factor = 1.) {
	return DrawPoint{ float(point.x()), float(point.y()), float(factor) };
}

[[nodiscard]] double SmoothStep(double from, double till, double value) {
	const auto t = std::clamp((value - from) / (till - from), 0., 1.);
	return t * t * (3. - 2. * t);
}

// A point of a stroke as the mouse gave it.
struct RawPoint {
	QPointF position; // Layer coordinates.
	double time = 0.; // Milliseconds from any origin.
};

// Points every step along the polyline, the ends are kept exactly.
[[nodiscard]] std::vector<RawPoint> Resample(
		const std::vector<RawPoint> &raw,
		double step) {
	auto result = std::vector<RawPoint>();
	if (raw.empty()) {
		return result;
	}
	result.push_back(raw.front());
	if (!(step > 0.)) {
		if (raw.size() > 1) {
			result.push_back(raw.back());
		}
		return result;
	}
	auto next = step; // From the start of the segment to the next sample.
	for (auto i = 1; i < int(raw.size()); ++i) {
		const auto &from = raw[i - 1];
		const auto &to = raw[i];
		const auto length = Distance(from.position, to.position);
		if (!(length > 0.)) {
			continue;
		}
		auto offset = next;
		while (offset <= length) {
			const auto t = offset / length;
			result.push_back({
				from.position + (to.position - from.position) * t,
				from.time + (to.time - from.time) * t,
			});
			offset += step;
		}
		next = offset - length;
	}
	const auto &last = raw.back();
	const auto tail = Distance(result.back().position, last.position);
	if (result.size() > 1 && tail < step * 0.5) {
		result.back() = last;
	} else if (tail > 0.) {
		result.push_back(last);
	}
	return result;
}

// A weighted moving average, the window shrinks towards the ends, so the
// stroke still starts and ends where the hand did.
void SmoothPositions(std::vector<QPointF> &points, int radius) {
	const auto count = int(points.size());
	if (radius <= 0 || count < 3) {
		return;
	}
	const auto source = points;
	for (auto i = 1; i + 1 < count; ++i) {
		const auto window = std::min({ radius, i, count - 1 - i });
		auto x = 0.;
		auto y = 0.;
		auto total = 0.;
		for (auto j = -window; j <= window; ++j) {
			const auto weight = double(window + 1 - std::abs(j));
			x += source[i + j].x() * weight;
			y += source[i + j].y() * weight;
			total += weight;
		}
		points[i] = QPointF(x / total, y / total);
	}
}

void SmoothValues(std::vector<double> &values, int radius) {
	const auto count = int(values.size());
	if (radius <= 0 || count < 2) {
		return;
	}
	const auto source = values;
	for (auto i = 0; i != count; ++i) {
		const auto from = std::max(i - radius, 0);
		const auto till = std::min(i + radius, count - 1);
		auto sum = 0.;
		for (auto j = from; j <= till; ++j) {
			sum += source[j];
		}
		values[i] = sum / (till - from + 1);
	}
}

// The width of the pen by the speed of the hand: full where it is slow,
// kDynamicMinFactor where it is fast. unit is layer pixels per screen
// pixel, samples are the resampled (not yet smoothed) points.
void ApplyDynamics(
		std::vector<DrawPoint> &points,
		const std::vector<RawPoint> &samples,
		double unit) {
	const auto count = int(points.size());
	if (count < 2
		|| samples.size() != points.size()
		|| !(samples.back().time - samples.front().time > 0.)) {
		return;
	}
	auto lengths = std::vector<double>(count, 0.);
	for (auto i = 1; i != count; ++i) {
		lengths[i] = lengths[i - 1]
			+ Distance(samples[i - 1].position, samples[i].position);
	}
	auto factors = std::vector<double>(count, 1.);
	for (auto i = 0; i != count; ++i) {
		const auto from = std::max(i - kDynamicWindow, 0);
		const auto till = std::min(i + kDynamicWindow, count - 1);
		const auto elapsed = std::max(
			samples[till].time - samples[from].time,
			0.5);
		const auto speed = (lengths[till] - lengths[from]) / unit / elapsed;
		factors[i] = 1. - (1. - kDynamicMinFactor)
			* SmoothStep(kDynamicSlowSpeed, kDynamicFastSpeed, speed);
	}
	SmoothValues(factors, kDynamicWindow);
	SmoothValues(factors, kDynamicWindow);
	for (auto i = 0; i != count; ++i) {
		points[i].w = float(std::clamp(factors[i], kDynamicMinFactor, 1.));
	}
}

struct StrokeInput {
	double smoothing = 0.4; // 0..1.
	// Layer pixels per screen pixel when the stroke was drawn: smoothing
	// works in what the hand did on the screen.
	double unit = 1.;
	bool dynamic = false; // Thinner where drawn fast.
};

// The center line of a freehand stroke from the mouse positions.
[[nodiscard]] std::vector<DrawPoint> BuildStroke(
		const std::vector<RawPoint> &raw,
		const StrokeInput &input) {
	auto clean = std::vector<RawPoint>();
	clean.reserve(raw.size());
	for (const auto &point : raw) {
		if (Finite(point.position) && std::isfinite(point.time)) {
			clean.push_back(point);
		}
	}
	auto result = std::vector<DrawPoint>();
	if (clean.empty()) {
		return result;
	}
	const auto unit = (std::isfinite(input.unit) && input.unit > 0.)
		? std::clamp(input.unit, 1e-3, 1e3)
		: 1.;
	auto length = 0.;
	for (auto i = 1; i < int(clean.size()); ++i) {
		length += Distance(clean[i - 1].position, clean[i].position);
	}
	const auto step = std::max({
		unit * kResampleScreenStep,
		length / kMaxStrokePoints,
		1e-3,
	});
	const auto samples = Resample(clean, step);
	auto positions = std::vector<QPointF>();
	positions.reserve(samples.size());
	for (const auto &sample : samples) {
		positions.push_back(sample.position);
	}
	const auto smoothing = std::isfinite(input.smoothing)
		? std::clamp(input.smoothing, 0., 1.)
		: 0.;
	SmoothPositions(
		positions,
		int(std::lround(smoothing * kSmoothingScreenRadius * unit / step)));
	result.reserve(positions.size());
	for (const auto &position : positions) {
		result.push_back(MakePoint(position));
	}
	if (input.dynamic) {
		ApplyDynamics(result, samples, unit);
	}
	return result;
}

struct ArrowHead {
	QPointF left;
	QPointF right;
	bool valid = false;
};

// The two barbs of an open arrow head at the tip.
[[nodiscard]] ArrowHead ArrowHeadFor(QPointF from, QPointF tip, double width) {
	const auto length = Distance(from, tip);
	if (!(length > 1e-6) || !std::isfinite(length)) {
		return {};
	}
	const auto direction = (tip - from) / length;
	const auto normal = QPointF(-direction.y(), direction.x());
	const auto head = std::min(
		std::max(width, 0.) * kArrowHeadWidths,
		length * kArrowHeadMaxPart);
	const auto along = direction * (head * std::cos(kArrowHeadAngle));
	const auto across = normal * (head * std::sin(kArrowHeadAngle));
	return {
		.left = tip - along - across,
		.right = tip - along + across,
		.valid = (head > 0.),
	};
}

// Shift while drawing a line: the nearest multiple of 45 degrees.
[[nodiscard]] QPointF ConstrainedLineEnd(QPointF from, QPointF to) {
	const auto length = Distance(from, to);
	if (!(length > 0.)) {
		return to;
	}
	const auto step = kPi / 4.;
	const auto angle = std::round(
		std::atan2(to.y() - from.y(), to.x() - from.x()) / step) * step;
	return from + QPointF(std::cos(angle), std::sin(angle)) * length;
}

// Shift while drawing a box: a square (a circle for an ellipse).
[[nodiscard]] QPointF ConstrainedBoxEnd(QPointF from, QPointF to) {
	const auto dx = to.x() - from.x();
	const auto dy = to.y() - from.y();
	const auto side = std::max(std::abs(dx), std::abs(dy));
	return from + QPointF(
		(dx < 0.) ? -side : side,
		(dy < 0.) ? -side : side);
}

[[nodiscard]] QPointF CatmullRom(
		QPointF p0,
		QPointF p1,
		QPointF p2,
		QPointF p3,
		double t) {
	const auto t2 = t * t;
	const auto t3 = t2 * t;
	return (p1 * 2.
		+ (p2 - p0) * t
		+ (p0 * 2. - p1 * 5. + p2 * 4. - p3) * t2
		+ (p1 * 3. - p0 - p2 * 3. + p3) * t3) * 0.5;
}

// The smooth curve through the points of a freehand stroke.
[[nodiscard]] QPainterPath CurvePath(const std::vector<DrawPoint> &points) {
	auto result = QPainterPath();
	const auto count = int(points.size());
	if (!count) {
		return result;
	}
	const auto at = [&](int index) {
		return PointOf(points[std::clamp(index, 0, count - 1)]);
	};
	result.moveTo(at(0));
	if (count == 2) {
		result.lineTo(at(1));
		return result;
	}
	for (auto i = 0; i + 1 < count; ++i) {
		const auto p1 = at(i);
		const auto p2 = at(i + 1);
		result.cubicTo(
			p1 + (p2 - at(i - 1)) / 6.,
			p2 - (at(i + 2) - p1) / 6.,
			p2);
	}
	return result;
}

// Both shapes below go around in the same direction, so with the winding
// fill any number of them is filled as one area without holes.
void AddCircle(QPainterPath &path, QPointF center, double radius) {
	const auto k = radius * 0.5522847498307936;
	const auto x = center.x();
	const auto y = center.y();
	path.moveTo(x + radius, y);
	path.cubicTo(x + radius, y + k, x + k, y + radius, x, y + radius);
	path.cubicTo(x - k, y + radius, x - radius, y + k, x - radius, y);
	path.cubicTo(x - radius, y - k, x - k, y - radius, x, y - radius);
	path.cubicTo(x + k, y - radius, x + radius, y - k, x + radius, y);
	path.closeSubpath();
}

void AddSegment(
		QPainterPath &path,
		QPointF from,
		double fromRadius,
		QPointF to,
		double toRadius) {
	const auto length = Distance(from, to);
	if (!(length > 1e-9)) {
		return;
	}
	const auto direction = (to - from) / length;
	const auto normal = QPointF(-direction.y(), direction.x());
	path.moveTo(from - normal * fromRadius);
	path.lineTo(to - normal * toRadius);
	path.lineTo(to + normal * toRadius);
	path.lineTo(from + normal * fromRadius);
	path.closeSubpath();
}

// The outline of a stroke whose width changes along the way: a union of
// circles and of the quads between them.
[[nodiscard]] QPainterPath DynamicPath(
		const std::vector<DrawPoint> &points,
		double width) {
	auto result = QPainterPath();
	result.setFillRule(Qt::WindingFill);
	const auto count = int(points.size());
	if (!count) {
		return result;
	}
	const auto at = [&](int index) {
		return PointOf(points[std::clamp(index, 0, count - 1)]);
	};
	const auto radius = [&](int index) {
		return std::max(width * points[index].w / 2., kDynamicMinRadius);
	};
	auto previous = at(0);
	auto previousRadius = radius(0);
	AddCircle(result, previous, previousRadius);
	for (auto i = 0; i + 1 < count; ++i) {
		for (auto j = 1; j <= kDynamicSubdivisions; ++j) {
			const auto t = j / double(kDynamicSubdivisions);
			const auto point = (j == kDynamicSubdivisions)
				? at(i + 1)
				: CatmullRom(at(i - 1), at(i), at(i + 1), at(i + 2), t);
			const auto now = radius(i) + (radius(i + 1) - radius(i)) * t;
			AddSegment(result, previous, previousRadius, point, now);
			AddCircle(result, point, now);
			previous = point;
			previousRadius = now;
		}
	}
	return result;
}

[[nodiscard]] bool HasDynamics(const DrawShape &shape) {
	for (const auto &point : shape.points) {
		if (std::abs(point.w - 1.f) > 1e-3f) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] QRectF BoxOf(const DrawShape &shape) {
	if (shape.points.size() < 2) {
		return QRectF();
	}
	return QRectF(
		PointOf(shape.points.front()),
		PointOf(shape.points.back())).normalized();
}

[[nodiscard]] QRectF BoundsOf(const DrawShape &shape) {
	const auto &points = shape.points;
	if (points.empty()) {
		return QRectF();
	}
	auto x1 = double(points.front().x);
	auto y1 = double(points.front().y);
	auto x2 = x1;
	auto y2 = y1;
	const auto add = [&](QPointF point) {
		x1 = std::min(x1, point.x());
		y1 = std::min(y1, point.y());
		x2 = std::max(x2, point.x());
		y2 = std::max(y2, point.y());
	};
	auto gap = 0.;
	for (auto i = 1; i < int(points.size()); ++i) {
		const auto point = PointOf(points[i]);
		add(point);
		gap = std::max(gap, Distance(PointOf(points[i - 1]), point));
	}
	if (shape.kind == DrawKind::Arrow && points.size() > 1) {
		const auto head = ArrowHeadFor(
			PointOf(points.front()),
			PointOf(points.back()),
			shape.width);
		if (head.valid) {
			add(head.left);
			add(head.right);
		}
	}
	// A curve through the points leaves their box a bit where it turns.
	const auto margin = std::max(shape.width, 0.) / 2.
		+ 1.
		+ (DrawKindIsFreehand(shape.kind) ? (gap / 3.) : 0.);
	return QRectF(x1, y1, x2 - x1, y2 - y1).adjusted(
		-margin,
		-margin,
		margin,
		margin);
}

// Makes any shape safe to keep and to paint. False: nothing to paint.
[[nodiscard]] bool NormalizeShape(DrawShape &shape) {
	if (int(shape.kind) >= kDrawKindCount) {
		return false;
	}
	shape.width = std::isfinite(shape.width)
		? std::clamp(shape.width, kMinWidth, kMaxWidth)
		: kMinWidth;
	shape.opacity = std::isfinite(shape.opacity)
		? std::clamp(shape.opacity, 0., 1.)
		: 1.;
	shape.color = shape.color.isValid()
		? shape.color.toRgb()
		: QColor(0, 0, 0);
	shape.color.setAlpha(255);
	auto &points = shape.points;
	points.erase(
		std::remove_if(begin(points), end(points), [](const DrawPoint &p) {
			return !std::isfinite(p.x) || !std::isfinite(p.y);
		}),
		end(points));
	if (points.empty()) {
		return false;
	}
	const auto freehand = DrawKindIsFreehand(shape.kind);
	if (freehand && int(points.size()) > kMaxShapePoints) {
		const auto count = int(points.size());
		auto reduced = std::vector<DrawPoint>();
		reduced.reserve(kMaxShapePoints);
		for (auto i = 0; i != kMaxShapePoints; ++i) {
			reduced.push_back(points[
				qint64(i) * (count - 1) / (kMaxShapePoints - 1)]);
		}
		points = std::move(reduced);
	} else if (!freehand && points.size() != 2) {
		const auto first = points.front();
		const auto last = points.back();
		points = { first, last };
	}
	const auto limit = float(kMaxCoordinate);
	for (auto &point : points) {
		point.x = std::clamp(point.x, -limit, limit);
		point.y = std::clamp(point.y, -limit, limit);
		point.w = (freehand && std::isfinite(point.w))
			? std::clamp(point.w, float(kMinPointFactor), 1.f)
			: 1.f;
	}
	if (shape.kind != DrawKind::Rectangle
		&& shape.kind != DrawKind::Ellipse) {
		shape.filled = false;
	}
	return true;
}

enum class PaintTarget : uchar {
	Content, // Into the pixels of the layer.
	Overlay, // Over the canvas while the stroke is being drawn.
};

// The geometry of a freehand stroke with the color, in layer coordinates
// (the painter has the transform).
void PaintStroke(QPainter &p, const DrawShape &shape, QColor color) {
	const auto &points = shape.points;
	const auto flat = (shape.kind == DrawKind::Marker);
	if (points.size() == 1) {
		const auto center = PointOf(points.front());
		const auto half = shape.width / 2.;
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		if (flat) {
			p.drawRect(QRectF(
				center.x() - half,
				center.y() - half,
				2. * half,
				2. * half));
		} else {
			p.drawEllipse(center, half, half);
		}
		return;
	} else if (shape.kind == DrawKind::Pen && HasDynamics(shape)) {
		p.fillPath(DynamicPath(points, shape.width), color);
		return;
	}
	p.strokePath(CurvePath(points), QPen(
		color,
		shape.width,
		Qt::SolidLine,
		flat ? Qt::FlatCap : Qt::RoundCap,
		Qt::RoundJoin));
}

void PaintFigure(QPainter &p, const DrawShape &shape, QColor color) {
	if (shape.points.size() < 2) {
		return;
	}
	const auto from = PointOf(shape.points.front());
	const auto to = PointOf(shape.points.back());
	const auto pen = QPen(
		color,
		shape.width,
		Qt::SolidLine,
		Qt::RoundCap,
		Qt::RoundJoin);
	const auto half = shape.width / 2.;
	switch (shape.kind) {
	case DrawKind::Line: {
		auto path = QPainterPath();
		path.moveTo(from);
		path.lineTo(to);
		p.strokePath(path, pen);
	} break;
	case DrawKind::Arrow: {
		// One path: where the head meets the shaft a semi-transparent
		// arrow is not painted twice.
		auto path = QPainterPath();
		path.moveTo(from);
		path.lineTo(to);
		const auto head = ArrowHeadFor(from, to, shape.width);
		if (head.valid) {
			path.moveTo(head.left);
			path.lineTo(to);
			path.lineTo(head.right);
		}
		p.strokePath(path, pen);
	} break;
	case DrawKind::Rectangle: {
		const auto box = BoxOf(shape);
		if (shape.filled) {
			p.setPen(Qt::NoPen);
			p.setBrush(color);
			p.drawRoundedRect(
				box.adjusted(-half, -half, half, half),
				half,
				half);
		} else {
			auto path = QPainterPath();
			path.addRect(box);
			p.strokePath(path, pen);
		}
	} break;
	case DrawKind::Ellipse: {
		const auto box = BoxOf(shape);
		if (shape.filled) {
			p.setPen(Qt::NoPen);
			p.setBrush(color);
			p.drawEllipse(box.adjusted(-half, -half, half, half));
		} else {
			auto path = QPainterPath();
			path.addEllipse(box);
			p.strokePath(path, pen);
		}
	} break;
	default: break;
	}
}

// The pencil: the stroke with a grain that is tied to the layer (so the
// small preview and the export have the same pattern). The stroke goes
// through a temporary image, in bands to keep it small. False: could not
// be done, paint a plain stroke.
[[nodiscard]] bool PaintPencil(
		QPainter &p,
		const DrawShape &shape,
		const QTransform &transform,
		double opacity) {
	const auto device = p.device();
	if (!device) {
		return false;
	}
	const auto ratio = device->devicePixelRatioF();
	if (!(ratio > 0.)) {
		return false;
	}
	const auto toPixels = transform
		* p.worldTransform()
		* QTransform::fromScale(ratio, ratio);
	auto invertible = false;
	const auto inverse = toPixels.inverted(&invertible);
	if (!invertible) {
		return false;
	}
	const auto view = QRectF(p.viewport());
	const auto limit = QRectF(
		view.x() * ratio,
		view.y() * ratio,
		view.width() * ratio,
		view.height() * ratio).toAlignedRect();
	const auto mapped = toPixels.mapRect(BoundsOf(shape));
	if (!std::isfinite(mapped.x())
		|| !std::isfinite(mapped.y())
		|| !std::isfinite(mapped.width())
		|| !std::isfinite(mapped.height())) {
		return false;
	}
	const auto bounds = mapped.toAlignedRect().intersected(limit);
	if (bounds.isEmpty()) {
		return true;
	}
	const auto affine = inverse.isAffine();
	const auto cell = std::max(
		shape.width * kPencilGrainPart,
		kPencilGrainMin);
	const auto strength = std::clamp(opacity, 0., 1.);
	auto color = shape.color;
	color.setAlpha(255);
	const auto bandHeight = std::max(
		kPencilBandPixels / std::max(bounds.width(), 1),
		kPencilMinBand);
	const auto bottom = bounds.y() + bounds.height();
	auto painted = false;
	p.save();
	p.resetTransform();
	for (auto top = bounds.y(); top < bottom; top += bandHeight) {
		const auto band = QRect(
			bounds.x(),
			top,
			bounds.width(),
			std::min(bandHeight, bottom - top));
		auto temp = QImage(band.size(), QImage::Format_ARGB32_Premultiplied);
		if (temp.isNull()) {
			// No memory. A plain stroke instead, unless a part is there.
			p.restore();
			return painted;
		}
		temp.fill(Qt::transparent);
		{
			auto q = QPainter(&temp);
			q.setRenderHint(QPainter::Antialiasing);
			q.setTransform(toPixels
				* QTransform::fromTranslate(-band.x(), -band.y()));
			PaintStroke(q, shape, color);
		}
		const auto width = temp.width();
		const auto height = temp.height();
		for (auto y = 0; y != height; ++y) {
			const auto line = reinterpret_cast<uint32*>(temp.scanLine(y));
			const auto py = band.y() + y + 0.5;
			for (auto x = 0; x != width; ++x) {
				const auto pixel = line[x];
				if (!(pixel >> 24)) {
					continue;
				}
				const auto px = band.x() + x + 0.5;
				auto lx = 0.;
				auto ly = 0.;
				if (affine) {
					lx = inverse.m11() * px + inverse.m21() * py + inverse.dx();
					ly = inverse.m12() * px + inverse.m22() * py + inverse.dy();
				} else {
					const auto local = inverse.map(QPointF(px, py));
					lx = local.x();
					ly = local.y();
				}
				const auto cx = std::isfinite(lx)
					? std::clamp(std::floor(lx / cell), -1e9, 1e9)
					: 0.;
				const auto cy = std::isfinite(ly)
					? std::clamp(std::floor(ly / cell), -1e9, 1e9)
					: 0.;
				const auto noise = (FxHash32(
					uint32(int(cx)),
					uint32(int(cy)),
					shape.seed) & 0xFFFFU) / 65535.;
				const auto factor = strength
					* (kPencilBase + (1. - kPencilBase) * noise);
				const auto k = uint32(std::clamp(
					int(std::lround(factor * 256.)),
					0,
					256));
				line[x] = ((((pixel >> 8) & 0x00FF00FFU) * k) & 0xFF00FF00U)
					| ((((pixel & 0x00FF00FFU) * k) >> 8) & 0x00FF00FFU);
			}
		}
		temp.setDevicePixelRatio(ratio);
		p.drawImage(QPointF(band.x() / ratio, band.y() / ratio), temp);
		painted = true;
	}
	p.restore();
	return true;
}

// Paints the shape with the painter, transform maps layer coordinates to
// the coordinates of the painter. The eraser wipes what is in the target
// (Content) or is shown as a light trace (Overlay).
void PaintShape(
		QPainter &p,
		const DrawShape &shape,
		const QTransform &transform,
		PaintTarget target) {
	if (shape.points.empty() || !(shape.width > 0.)) {
		return;
	}
	const auto opacity = std::clamp(shape.opacity, 0., 1.);
	auto color = shape.color;
	color.setAlphaF(float(opacity));
	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	if (shape.kind == DrawKind::Eraser) {
		if (target == PaintTarget::Content) {
			p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
			color = QColor(0, 0, 0);
			color.setAlphaF(float(opacity));
		} else {
			color = QColor(255, 255, 255);
			color.setAlphaF(float(0.15 + 0.35 * opacity));
		}
	} else if (shape.kind == DrawKind::Pencil
		&& PaintPencil(p, shape, transform, opacity)) {
		p.restore();
		return;
	}
	p.setTransform(transform, true);
	if (DrawKindIsFreehand(shape.kind)) {
		PaintStroke(p, shape, color);
	} else {
		PaintFigure(p, shape, color);
	}
	p.restore();
}

struct StoredShape {
	DrawShape shape;
	QRectF bounds;
	uint64 id = 0; // Unique in the process.
	qint64 bytes = 0;
};
using StoredPtr = std::shared_ptr<const StoredShape>;

[[nodiscard]] StoredPtr StoreShape(DrawShape &&shape) {
	static auto Counter = std::atomic<uint64>(0);
	if (!NormalizeShape(shape)) {
		return nullptr;
	}
	auto result = std::make_shared<StoredShape>();
	result->bounds = BoundsOf(shape);
	result->bytes = qint64(sizeof(StoredShape))
		+ qint64(shape.points.size()) * qint64(sizeof(DrawPoint));
	result->id = ++Counter;
	result->shape = std::move(shape);
	return result;
}

// The pictures of the first shapes of a drawing, shared by all versions
// of it (every stroke makes a new content object): the next version only
// paints its new shapes over what the previous one has rendered.
struct RenderCache {
	struct Entry {
		double scale = 0.;
		int count = 0;
		uint64 hash = 0;
		uint64 used = 0;
		QImage image;
	};
	std::mutex mutex;
	std::array<Entry, kCacheEntries> entries;
	uint64 counter = 0;
};

class DrawContent final : public LayerContent {
public:
	DrawContent(
		QSize size,
		std::vector<StoredPtr> shapes,
		std::shared_ptr<RenderCache> cache,
		qint64 ownBytes);

	QByteArray type() const override;
	QSize size() const override;
	QImage render(const ContentRequest &request) const override;
	qint64 memoryUsage() const override;

	[[nodiscard]] const std::vector<StoredPtr> &shapes() const {
		return _shapes;
	}
	[[nodiscard]] const std::shared_ptr<RenderCache> &cache() const {
		return _cache;
	}
	// When a render of this version was last completed, 0: never. Only
	// the renders at a scale above the watched one count: the tool that
	// has added a stroke waits for the preview of the canvas with it, not
	// for a thumbnail in the list of layers.
	[[nodiscard]] crl::time renderedAt() const {
		return _renderedAt.load(std::memory_order_relaxed);
	}
	void watchScale(double scale) const {
		_watchScale.store(scale, std::memory_order_relaxed);
	}

private:
	[[nodiscard]] static std::vector<uint64> Hashes(
		const std::vector<StoredPtr> &shapes);

	const QSize _size;
	const std::vector<StoredPtr> _shapes;
	const std::vector<uint64> _hashes; // Of the first N shapes.
	const std::shared_ptr<RenderCache> _cache;
	const qint64 _ownBytes = 0;
	mutable std::atomic<crl::time> _renderedAt = 0;
	mutable std::atomic<double> _watchScale = 0.;

};

DrawContent::DrawContent(
	QSize size,
	std::vector<StoredPtr> shapes,
	std::shared_ptr<RenderCache> cache,
	qint64 ownBytes)
: _size(size)
, _shapes(std::move(shapes))
, _hashes(Hashes(_shapes))
, _cache(cache ? std::move(cache) : std::make_shared<RenderCache>())
, _ownBytes(ownBytes) {
}

std::vector<uint64> DrawContent::Hashes(
		const std::vector<StoredPtr> &shapes) {
	auto result = std::vector<uint64>();
	result.reserve(shapes.size());
	auto hash = uint64(0xD2A3ULL);
	for (const auto &shape : shapes) {
		hash = FxHashCombine(hash, shape->id);
		result.push_back(hash);
	}
	return result;
}

QByteArray DrawContent::type() const {
	return kDrawLayerType;
}

QSize DrawContent::size() const {
	return _size;
}

qint64 DrawContent::memoryUsage() const {
	return _ownBytes;
}

QImage DrawContent::render(const ContentRequest &request) const {
	const auto cancelled = [&] {
		return request.cancel
			&& request.cancel->load(std::memory_order_relaxed);
	};
	if (_size.isEmpty() || cancelled()) {
		return QImage();
	}
	const auto scale = (std::isfinite(request.scale) && request.scale > 0.)
		? request.scale
		: 1.;
	const auto size = ScaledSize(_size, scale);
	const auto count = int(_shapes.size());
	auto result = QImage();
	auto start = 0;
	if (count > 0) {
		const auto guard = std::lock_guard(_cache->mutex);
		auto found = (RenderCache::Entry*)(nullptr);
		for (auto &entry : _cache->entries) {
			if (entry.count > start
				&& entry.count <= count
				&& entry.scale == scale
				&& entry.image.size() == size
				&& entry.hash == _hashes[entry.count - 1]) {
				found = &entry;
				start = entry.count;
			}
		}
		if (found) {
			found->used = ++_cache->counter;
			result = found->image;
		}
	}
	if (result.isNull()) {
		start = 0;
		result = QImage(size, QImage::Format_ARGB32_Premultiplied);
		if (result.isNull()) {
			return QImage();
		}
		result.fill(Qt::transparent);
	}
	if (start < count) {
		// Painting detaches the picture from the cached one.
		auto p = QPainter(&result);
		if (!p.isActive()) {
			return QImage();
		}
		const auto transform = QTransform::fromScale(
			size.width() / double(_size.width()),
			size.height() / double(_size.height()));
		for (auto i = start; i != count; ++i) {
			if (cancelled()) {
				return QImage();
			}
			PaintShape(p, _shapes[i]->shape, transform, PaintTarget::Content);
		}
	}
	if (cancelled()) {
		return QImage();
	}
	const auto pixels = qint64(size.width()) * size.height();
	if (start < count && pixels <= kCacheMaxPixels) {
		const auto guard = std::lock_guard(_cache->mutex);
		auto chosen = &_cache->entries.front();
		for (auto &entry : _cache->entries) {
			if (entry.used < chosen->used) {
				chosen = &entry;
			}
		}
		*chosen = RenderCache::Entry{
			.scale = scale,
			.count = count,
			.hash = _hashes.back(),
			.used = ++_cache->counter,
			.image = result,
		};
	}
	if (scale > _watchScale.load(std::memory_order_relaxed)) {
		_renderedAt.store(
			std::max(crl::now(), crl::time(1)),
			std::memory_order_relaxed);
	}
	return result;
}

[[nodiscard]] const DrawContent *AsDraw(const ContentPtr &content) {
	return (content && content->type() == kDrawLayerType)
		? static_cast<const DrawContent*>(content.get())
		: nullptr;
}

[[nodiscard]] qint64 PointersBytes(int count) {
	return qint64(sizeof(DrawContent)) + qint64(count) * sizeof(StoredPtr);
}

[[nodiscard]] QByteArray KindKey(DrawKind kind) {
	switch (kind) {
	case DrawKind::Pen: return "pen";
	case DrawKind::Marker: return "marker";
	case DrawKind::Pencil: return "pencil";
	case DrawKind::Eraser: return "eraser";
	case DrawKind::Line: return "line";
	case DrawKind::Arrow: return "arrow";
	case DrawKind::Rectangle: return "rect";
	case DrawKind::Ellipse: return "ellipse";
	}
	return QByteArray();
}

[[nodiscard]] std::optional<DrawKind> KindFromKey(const QString &key) {
	for (auto i = 0; i != kDrawKindCount; ++i) {
		const auto kind = DrawKind(i);
		if (key == QString::fromLatin1(KindKey(kind))) {
			return kind;
		}
	}
	return std::nullopt;
}

[[nodiscard]] double Rounded(double value, double units) {
	return std::round(value * units) / units;
}

[[nodiscard]] QString ColorKey(QColor color) {
	return color.name(QColor::HexRgb);
}

[[nodiscard]] QColor ColorFromKey(const QString &key, QColor fallback) {
	auto result = QColor::fromString(key);
	if (!result.isValid()) {
		return fallback;
	}
	result = result.toRgb();
	result.setAlpha(255);
	return result;
}

[[nodiscard]] double FiniteOr(double value, double fallback) {
	return std::isfinite(value) ? value : fallback;
}

//
// Text layers.
//

struct TextEmoji {
	QRectF rect;
	QImage image;
};

// What MakeTextContent() prepares on the main thread: everything the
// renderer needs, in layer coordinates.
struct TextLayout {
	QSize size;
	QPainterPath path; // The outlines of the glyphs, winding fill.
	QRectF plate; // Empty: no plate.
	double plateRadius = 0.;
	std::vector<TextEmoji> emoji;
};

class TextContent final : public LayerContent {
public:
	TextContent(TextLayerStyle style, TextLayout layout);

	QByteArray type() const override;
	QSize size() const override;
	QImage render(const ContentRequest &request) const override;
	qint64 memoryUsage() const override;

	[[nodiscard]] const TextLayerStyle &style() const {
		return _style;
	}

private:
	const TextLayerStyle _style;
	const TextLayout _layout;

	// A QPainterPath prepares its data for the rasterizer when it is
	// painted for the first time, without any locking. The preview and the
	// thumbnail of a layer are rendered by different threads at once, so
	// without this a glyph could come out broken.
	mutable std::mutex _pathMutex;

};

TextContent::TextContent(TextLayerStyle style, TextLayout layout)
: _style(std::move(style))
, _layout(std::move(layout)) {
}

QByteArray TextContent::type() const {
	return kTextLayerType;
}

QSize TextContent::size() const {
	return _layout.size;
}

qint64 TextContent::memoryUsage() const {
	auto result = qint64(sizeof(TextContent))
		+ qint64(_style.text.size()) * qint64(sizeof(QChar))
		+ qint64(_layout.path.elementCount()) * 24;
	for (const auto &emoji : _layout.emoji) {
		result += emoji.image.sizeInBytes();
	}
	return result;
}

QImage TextContent::render(const ContentRequest &request) const {
	const auto full = _layout.size;
	if (full.isEmpty()
		|| (request.cancel
			&& request.cancel->load(std::memory_order_relaxed))) {
		return QImage();
	}
	const auto scale = (std::isfinite(request.scale) && request.scale > 0.)
		? request.scale
		: 1.;
	const auto size = ScaledSize(full, scale);
	auto result = QImage(size, QImage::Format_ARGB32_Premultiplied);
	if (result.isNull()) {
		return QImage();
	}
	result.fill(Qt::transparent);
	auto p = QPainter(&result);
	if (!p.isActive()) {
		return QImage();
	}
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	p.scale(
		size.width() / double(full.width()),
		size.height() / double(full.height()));
	if (_style.plate && !_layout.plate.isEmpty()) {
		auto color = _style.plateColor;
		color.setAlphaF(float(std::clamp(_style.plateOpacity, 0., 1.)));
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawRoundedRect(
			_layout.plate,
			_layout.plateRadius,
			_layout.plateRadius);
	}
	auto color = _style.color;
	color.setAlpha(255);
	{
		const auto guard = std::lock_guard(_pathMutex);
		p.fillPath(_layout.path, color);
	}
	for (const auto &emoji : _layout.emoji) {
		if (!emoji.image.isNull()) {
			p.drawImage(emoji.rect, emoji.image);
		}
	}
	return result;
}

[[nodiscard]] const TextContent *AsText(const ContentPtr &content) {
	return (content && content->type() == kTextLayerType)
		? static_cast<const TextContent*>(content.get())
		: nullptr;
}

[[nodiscard]] TextLayerStyle NormalizedText(TextLayerStyle style) {
	style.text.remove(QChar('\r'));
	if (style.text.size() > kMaxTextLength) {
		style.text.truncate(kMaxTextLength);
		// Not a half of a surrogate pair at the end.
		if (!style.text.isEmpty() && style.text.back().isHighSurrogate()) {
			style.text.chop(1);
		}
	}
	auto lines = 1;
	for (auto i = 0; i != int(style.text.size()); ++i) {
		if (style.text[i] == QChar('\n') && ++lines > kMaxTextLines) {
			style.text.truncate(i);
			break;
		}
	}
	const auto opaque = [](QColor color, QColor fallback) {
		auto result = color.isValid() ? color.toRgb() : fallback;
		result.setAlpha(255);
		return result;
	};
	style.color = opaque(style.color, QColor(255, 255, 255));
	style.plateColor = opaque(style.plateColor, QColor(0, 0, 0));
	style.size = std::clamp(
		FiniteOr(style.size, 64.),
		kMinTextSize,
		kMaxTextSize);
	style.plateOpacity = std::clamp(FiniteOr(style.plateOpacity, 0.6), 0., 1.);
	if (int(style.weight) > int(TextLayerWeight::Bold)) {
		style.weight = TextLayerWeight::Semibold;
	}
	if (int(style.font) > int(TextLayerFont::Mono)) {
		style.font = TextLayerFont::Default;
	}
	if (int(style.align) > int(TextLayerAlign::Right)) {
		style.align = TextLayerAlign::Left;
	}
	return style;
}

// Which part of the width stays in place when the text changes.
[[nodiscard]] double AlignPart(TextLayerAlign align) {
	switch (align) {
	case TextLayerAlign::Left: return 0.;
	case TextLayerAlign::Center: return 0.5;
	case TextLayerAlign::Right: return 1.;
	}
	return 0.;
}

// How far (layer pixels) the block moves when its width changes, so the
// side the text is aligned to stays where it is.
[[nodiscard]] double TextAnchorShift(
		int oldWidth,
		int newWidth,
		TextLayerAlign align) {
	return AlignPart(align) * (oldWidth - newWidth);
}

// The first line with something in it, for the name of the layer.
[[nodiscard]] QString TextNameFrom(const QString &text) {
	for (const auto &line : text.split(QChar('\n'))) {
		const auto trimmed = line.simplified();
		if (trimmed.isEmpty()) {
			continue;
		} else if (trimmed.size() <= kLayerNameLength) {
			return trimmed;
		}
		auto cut = trimmed.left(kLayerNameLength);
		if (cut.back().isHighSurrogate()) {
			cut.chop(1);
		}
		return cut.trimmed() + QChar(0x2026);
	}
	return QString();
}

[[nodiscard]] QString WeightKey(TextLayerWeight weight) {
	switch (weight) {
	case TextLayerWeight::Regular: return u"regular"_q;
	case TextLayerWeight::Semibold: return u"semibold"_q;
	case TextLayerWeight::Bold: return u"bold"_q;
	}
	return u"semibold"_q;
}

[[nodiscard]] QString FontKey(TextLayerFont font) {
	switch (font) {
	case TextLayerFont::Default: return u"default"_q;
	case TextLayerFont::Serif: return u"serif"_q;
	case TextLayerFont::Mono: return u"mono"_q;
	}
	return u"default"_q;
}

[[nodiscard]] QString AlignKey(TextLayerAlign align) {
	switch (align) {
	case TextLayerAlign::Left: return u"left"_q;
	case TextLayerAlign::Center: return u"center"_q;
	case TextLayerAlign::Right: return u"right"_q;
	}
	return u"left"_q;
}

[[nodiscard]] bool BlankText(const QString &text) {
	return text.trimmed().isEmpty();
}

// The plate is a part of the picture only when there is something on it:
// a text that was added and not typed yet leaves nothing in a photo that
// is saved or sent meanwhile (the text tool outlines where it is).
[[nodiscard]] bool TextShowsPlate(const TextLayerStyle &style) {
	return style.plate && !BlankText(style.text);
}

[[nodiscard]] int TextLineCount(const QString &text) {
	return int(text.count(QChar('\n'))) + 1;
}

// A full text takes no more lines: the limit cuts a text at its end, so
// a line break typed in the middle would silently drop the last line.
[[nodiscard]] bool TextRefusesLines(const QString &now, const QString &next) {
	return (TextLineCount(now) >= kMaxTextLines)
		&& (TextLineCount(next) > kMaxTextLines);
}

// The fonts have two faces each: the font of the interface a regular and
// a semibold one, the serif and the monospace ones a regular and a bold
// one. So the settings offer two weights, and both heavy values of a text
// (the second one is there for the layers saved with it) give the heavy
// face of the font: a third option would always look like one of the two.
[[nodiscard]] int WeightChoice(TextLayerWeight weight) {
	return (weight == TextLayerWeight::Regular) ? 0 : 1;
}

[[nodiscard]] TextLayerWeight WeightFromChoice(int index) {
	return (index > 0) ? TextLayerWeight::Semibold : TextLayerWeight::Regular;
}

[[nodiscard]] QFont::Weight FontWeightFor(
		TextLayerFont font,
		TextLayerWeight weight) {
	return (weight == TextLayerWeight::Regular)
		? QFont::Normal
		: (font == TextLayerFont::Default)
		? QFont::DemiBold
		: QFont::Bold;
}

// A key press that the text field has not used itself (Backspace in an
// empty field or at the start of the text, an arrow at its end, Escape)
// stops at the field. Further up it is a shortcut of the editor or of the
// tool: Backspace deletes the whole layer there, a letter takes another
// tool, Escape closes the editor. Only the modifier keys and what is
// pressed with Ctrl / Cmd (save, zoom, done) go on.
[[nodiscard]] bool FieldKeepsKey(int key, Qt::KeyboardModifiers modifiers) {
	switch (key) {
	case Qt::Key_Escape:
	case Qt::Key_Backspace:
	case Qt::Key_Delete:
		return true;
	case Qt::Key_Shift:
	case Qt::Key_Control:
	case Qt::Key_Meta:
	case Qt::Key_Alt:
	case Qt::Key_AltGr:
	case Qt::Key_CapsLock:
	case Qt::Key_NumLock:
		return false;
	}
	return !(modifiers & (Qt::ControlModifier | Qt::MetaModifier));
}

// A stroke or a figure that has no part inside the layer it goes to (a
// click in the margin around the photo) would add nothing that is seen.
[[nodiscard]] bool ShapeTouches(const DrawShape &shape, QSize size) {
	return !shape.points.empty()
		&& BoundsOf(shape).intersects(QRectF(QPointF(), QSizeF(size)));
}

} // namespace

bool DrawKindIsFreehand(DrawKind kind) {
	switch (kind) {
	case DrawKind::Pen:
	case DrawKind::Marker:
	case DrawKind::Pencil:
	case DrawKind::Eraser:
		return true;
	case DrawKind::Line:
	case DrawKind::Arrow:
	case DrawKind::Rectangle:
	case DrawKind::Ellipse:
		return false;
	}
	return false;
}

QRectF DrawShapeBounds(const DrawShape &shape) {
	return BoundsOf(shape);
}

ContentPtr MakeDrawContent(QSize size, std::vector<DrawShape> shapes) {
	if (size.isEmpty()) {
		return nullptr;
	}
	auto stored = std::vector<StoredPtr>();
	stored.reserve(std::min(int(shapes.size()), kMaxShapes));
	auto bytes = qint64(0);
	for (auto &shape : shapes) {
		if (int(stored.size()) >= kMaxShapes) {
			break;
		} else if (auto one = StoreShape(std::move(shape))) {
			bytes += one->bytes;
			stored.push_back(std::move(one));
		}
	}
	const auto count = int(stored.size());
	return std::make_shared<const DrawContent>(
		size,
		std::move(stored),
		nullptr,
		PointersBytes(count) + bytes);
}

bool IsDrawContent(const ContentPtr &content) {
	return AsDraw(content) != nullptr;
}

int DrawShapeCount(const ContentPtr &content) {
	const auto drawing = AsDraw(content);
	return drawing ? int(drawing->shapes().size()) : 0;
}

std::vector<DrawShape> DrawShapes(const ContentPtr &content) {
	auto result = std::vector<DrawShape>();
	if (const auto drawing = AsDraw(content)) {
		result.reserve(drawing->shapes().size());
		for (const auto &stored : drawing->shapes()) {
			result.push_back(stored->shape);
		}
	}
	return result;
}

ContentPtr DrawWithShape(const ContentPtr &content, DrawShape shape) {
	const auto drawing = AsDraw(content);
	if (!drawing || int(drawing->shapes().size()) >= kMaxShapes) {
		return nullptr;
	}
	auto added = StoreShape(std::move(shape));
	if (!added) {
		return nullptr;
	}
	// The shapes are shared with the previous version: only the new one
	// and the list itself count as the memory of this one.
	const auto bytes = added->bytes;
	auto shapes = drawing->shapes();
	shapes.push_back(std::move(added));
	const auto count = int(shapes.size());
	return std::make_shared<const DrawContent>(
		drawing->size(),
		std::move(shapes),
		drawing->cache(),
		PointersBytes(count) + bytes);
}

QByteArray SerializeDrawing(const ContentPtr &content) {
	const auto drawing = AsDraw(content);
	if (!drawing) {
		return QByteArray();
	}
	auto shapes = QJsonArray();
	for (const auto &stored : drawing->shapes()) {
		const auto &shape = stored->shape;
		const auto freehand = DrawKindIsFreehand(shape.kind);
		auto points = QJsonArray();
		for (const auto &point : shape.points) {
			points.push_back(Rounded(point.x, 100.));
			points.push_back(Rounded(point.y, 100.));
			if (freehand) {
				points.push_back(Rounded(point.w, 1000.));
			}
		}
		auto object = QJsonObject();
		object.insert(u"k"_q, QString::fromLatin1(KindKey(shape.kind)));
		object.insert(u"c"_q, ColorKey(shape.color));
		object.insert(u"w"_q, Rounded(shape.width, 1000.));
		object.insert(u"o"_q, Rounded(shape.opacity, 1000.));
		if (shape.filled) {
			object.insert(u"f"_q, true);
		}
		if (shape.seed) {
			object.insert(u"s"_q, double(shape.seed));
		}
		object.insert(u"p"_q, points);
		shapes.push_back(object);
	}
	auto root = QJsonObject();
	root.insert(u"type"_q, QString::fromLatin1(kDrawLayerType));
	root.insert(u"v"_q, 1);
	root.insert(u"w"_q, drawing->size().width());
	root.insert(u"h"_q, drawing->size().height());
	root.insert(u"shapes"_q, shapes);
	return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

ContentPtr DeserializeDrawing(const QByteArray &json) {
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(json, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return nullptr;
	}
	const auto root = document.object();
	if (root.value(u"type"_q).toString()
		!= QString::fromLatin1(kDrawLayerType)) {
		return nullptr;
	}
	const auto side = [&](const QString &key) {
		const auto value = root.value(key).toDouble();
		return std::isfinite(value)
			? int(std::clamp(value, 0., 65536.))
			: 0;
	};
	const auto size = QSize(side(u"w"_q), side(u"h"_q));
	if (size.isEmpty()) {
		return nullptr;
	}
	auto shapes = std::vector<DrawShape>();
	for (const auto &entry : root.value(u"shapes"_q).toArray()) {
		if (int(shapes.size()) >= kMaxShapes) {
			break;
		}
		const auto object = entry.toObject();
		const auto kind = KindFromKey(object.value(u"k"_q).toString());
		if (!kind) {
			continue;
		}
		auto shape = DrawShape();
		shape.kind = *kind;
		shape.color = ColorFromKey(
			object.value(u"c"_q).toString(),
			QColor(0, 0, 0));
		shape.width = object.value(u"w"_q).toDouble(8.);
		shape.opacity = object.value(u"o"_q).toDouble(1.);
		shape.filled = object.value(u"f"_q).toBool(false);
		const auto seed = object.value(u"s"_q).toDouble(0.);
		shape.seed = (std::isfinite(seed) && seed >= 0.)
			? uint32(std::min(seed, 4294967295.))
			: 0;
		const auto values = object.value(u"p"_q).toArray();
		const auto each = DrawKindIsFreehand(*kind) ? 3 : 2;
		const auto count = std::min(
			int(values.size()) / each,
			kMaxShapePoints * 2);
		shape.points.reserve(count);
		for (auto i = 0; i != count; ++i) {
			shape.points.push_back(DrawPoint{
				float(values.at(i * each).toDouble(qQNaN())),
				float(values.at(i * each + 1).toDouble(qQNaN())),
				(each == 3)
					? float(values.at(i * each + 2).toDouble(1.))
					: 1.f,
			});
		}
		shapes.push_back(std::move(shape));
	}
	return MakeDrawContent(size, std::move(shapes));
}

bool IsTextContent(const ContentPtr &content) {
	return AsText(content) != nullptr;
}

std::optional<TextLayerStyle> TextStyleOf(const ContentPtr &content) {
	const auto text = AsText(content);
	return text ? std::make_optional(text->style()) : std::nullopt;
}

QByteArray SerializeTextStyle(const TextLayerStyle &style) {
	const auto normalized = NormalizedText(style);
	auto root = QJsonObject();
	root.insert(u"type"_q, QString::fromLatin1(kTextLayerType));
	root.insert(u"v"_q, 1);
	root.insert(u"text"_q, normalized.text);
	root.insert(u"color"_q, ColorKey(normalized.color));
	root.insert(u"size"_q, Rounded(normalized.size, 1000.));
	root.insert(u"weight"_q, WeightKey(normalized.weight));
	root.insert(u"font"_q, FontKey(normalized.font));
	root.insert(u"align"_q, AlignKey(normalized.align));
	root.insert(u"plate"_q, normalized.plate);
	root.insert(u"plate_color"_q, ColorKey(normalized.plateColor));
	root.insert(u"plate_opacity"_q, Rounded(normalized.plateOpacity, 1000.));
	return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

std::optional<TextLayerStyle> DeserializeTextStyle(const QByteArray &json) {
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(json, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return std::nullopt;
	}
	const auto root = document.object();
	if (root.value(u"type"_q).toString()
		!= QString::fromLatin1(kTextLayerType)) {
		return std::nullopt;
	}
	auto result = TextLayerStyle();
	result.text = root.value(u"text"_q).toString();
	result.color = ColorFromKey(
		root.value(u"color"_q).toString(),
		result.color);
	result.size = root.value(u"size"_q).toDouble(result.size);
	const auto weight = root.value(u"weight"_q).toString();
	for (const auto value : {
		TextLayerWeight::Regular,
		TextLayerWeight::Semibold,
		TextLayerWeight::Bold,
	}) {
		if (weight == WeightKey(value)) {
			result.weight = value;
		}
	}
	const auto font = root.value(u"font"_q).toString();
	for (const auto value : {
		TextLayerFont::Default,
		TextLayerFont::Serif,
		TextLayerFont::Mono,
	}) {
		if (font == FontKey(value)) {
			result.font = value;
		}
	}
	const auto align = root.value(u"align"_q).toString();
	for (const auto value : {
		TextLayerAlign::Left,
		TextLayerAlign::Center,
		TextLayerAlign::Right,
	}) {
		if (align == AlignKey(value)) {
			result.align = value;
		}
	}
	result.plate = root.value(u"plate"_q).toBool(false);
	result.plateColor = ColorFromKey(
		root.value(u"plate_color"_q).toString(),
		result.plateColor);
	result.plateOpacity = root.value(u"plate_opacity"_q).toDouble(
		result.plateOpacity);
	return NormalizedText(std::move(result));
}

namespace {

//
// The self-test, a part of OBLIVION_SELFTEST=photo_doc.
//

[[nodiscard]] QImage RenderAt(const ContentPtr &content, double scale) {
	return content
		? content->render({ .scale = scale, .preview = true })
		: QImage();
}

[[nodiscard]] int AlphaAt(const QImage &image, int x, int y) {
	return image.valid(x, y) ? qAlpha(image.pixel(x, y)) : -1;
}

[[nodiscard]] std::vector<RawPoint> TestWave(
		QPointF from,
		QPointF to,
		double amplitude,
		int count,
		double duration) {
	auto result = std::vector<RawPoint>();
	for (auto i = 0; i <= count; ++i) {
		const auto t = i / double(count);
		result.push_back({
			QPointF(
				from.x() + (to.x() - from.x()) * t,
				from.y() + (to.y() - from.y()) * t
					+ amplitude * std::sin(t * 4. * kPi)),
			duration * t,
		});
	}
	return result;
}

[[nodiscard]] DrawShape TestShape(
		DrawKind kind,
		QColor color,
		double width,
		std::vector<DrawPoint> points) {
	auto result = DrawShape();
	result.kind = kind;
	result.color = color;
	result.width = width;
	result.points = std::move(points);
	return result;
}

[[nodiscard]] bool SameShapes(
		const std::vector<DrawShape> &a,
		const std::vector<DrawShape> &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (auto i = 0; i != int(a.size()); ++i) {
		const auto &x = a[i];
		const auto &y = b[i];
		if (x.kind != y.kind
			|| x.color != y.color
			|| std::abs(x.width - y.width) > 0.001
			|| std::abs(x.opacity - y.opacity) > 0.001
			|| x.filled != y.filled
			|| x.seed != y.seed
			|| x.points.size() != y.points.size()) {
			return false;
		}
		for (auto j = 0; j != int(x.points.size()); ++j) {
			if (std::abs(x.points[j].x - y.points[j].x) > 0.006f
				|| std::abs(x.points[j].y - y.points[j].y) > 0.006f
				|| std::abs(x.points[j].w - y.points[j].w) > 0.0006f) {
				return false;
			}
		}
	}
	return true;
}

bool RunDrawSelfTest(QStringList &log) {
	auto ok = true;
	const auto check = [&](bool condition, const QString &what) {
		log.push_back((condition ? u"OK: "_q : u"FAIL: "_q) + what);
		ok = ok && condition;
	};
	const auto info = [&](const QString &what) {
		log.push_back(u"   "_q + what);
	};
	const auto red = QColor(230, 40, 40);
	const auto blue = QColor(30, 90, 230);

	// Resampling.
	{
		const auto line = Resample({
			{ QPointF(0., 0.), 0. },
			{ QPointF(100., 0.), 100. },
		}, 10.);
		auto even = (line.size() == 11);
		for (auto i = 0; even && i != int(line.size()); ++i) {
			even = (std::abs(line[i].position.x() - i * 10.) < 1e-9)
				&& (std::abs(line[i].time - i * 10.) < 1e-9);
		}
		check(even, u"resample: a point every step, times follow"_q);
		const auto ends = Resample({
			{ QPointF(3., 4.), 0. },
			{ QPointF(20., 4.), 1. },
			{ QPointF(20., 33.), 2. },
		}, 7.);
		check(
			ends.size() > 2
				&& ends.front().position == QPointF(3., 4.)
				&& ends.back().position == QPointF(20., 33.),
			u"resample: the ends are kept exactly"_q);
		check(
			Resample({ { QPointF(5., 5.), 0. } }, 2.).size() == 1
				&& Resample({}, 2.).empty(),
			u"resample: a single point and nothing"_q);
	}

	// Smoothing.
	{
		auto zigzag = std::vector<RawPoint>();
		for (auto i = 0; i <= 50; ++i) {
			zigzag.push_back({
				QPointF(i * 4., (i % 2) ? 5. : -5.),
				double(i),
			});
		}
		const auto deviation = [](const std::vector<DrawPoint> &points) {
			auto result = 0.;
			const auto count = int(points.size());
			for (auto i = count / 4; i < count * 3 / 4; ++i) {
				result = std::max(result, std::abs(double(points[i].y)));
			}
			return result;
		};
		const auto rough = BuildStroke(zigzag, { .smoothing = 0. });
		const auto smooth = BuildStroke(zigzag, { .smoothing = 1. });
		info(u"zigzag of 5: %1 without smoothing, %2 with the full one"_q
			.arg(deviation(rough), 0, 'f', 2)
			.arg(deviation(smooth), 0, 'f', 2));
		check(
			deviation(rough) > 3. && deviation(smooth) < 1.,
			u"smoothing: a zigzag becomes a line"_q);
		check(
			!smooth.empty()
				&& PointOf(smooth.front()) == zigzag.front().position
				&& PointOf(smooth.back()) == zigzag.back().position,
			u"smoothing: the stroke starts and ends where the hand did"_q);

		const auto straight = BuildStroke({
			{ QPointF(10., 5.), 0. },
			{ QPointF(60., 30.), 0. },
			{ QPointF(210., 105.), 0. },
		}, { .smoothing = 0.7 });
		auto off = 0.;
		for (const auto &point : straight) {
			off = std::max(off, std::abs(point.y - point.x * 0.5));
		}
		check(
			straight.size() > 10 && off < 1e-3,
			u"smoothing: a straight line stays straight"_q);

		const auto half = BuildStroke(zigzag, { .smoothing = 0.5 });
		const auto zoomed = BuildStroke(zigzag, {
			.smoothing = 0.5,
			.unit = 0.25,
		});
		check(
			zoomed.size() > half.size()
				&& deviation(zoomed) > deviation(half),
			u"smoothing: works in screen pixels (less when zoomed in)"_q);

		auto huge = std::vector<RawPoint>();
		for (auto i = 0; i <= 2000; ++i) {
			huge.push_back({ QPointF(i * 50., (i % 3) * 20.), double(i) });
		}
		const auto capped = BuildStroke(huge, {});
		check(
			capped.size() > 1000
				&& int(capped.size()) <= kMaxStrokePoints + 2,
			u"smoothing: a very long stroke has a limited size"_q);
		check(
			BuildStroke({}, {}).empty()
				&& BuildStroke({ { QPointF(qQNaN(), 1.), 0. } }, {}).empty()
				&& BuildStroke({ { QPointF(4., 4.), 0. } }, {}).size() == 1,
			u"smoothing: nothing, broken and single points"_q);
	}

	// The width by the speed.
	{
		const auto range = [](const std::vector<DrawPoint> &points) {
			auto low = 1.f;
			auto high = 0.f;
			const auto count = int(points.size());
			for (auto i = count / 4; i < count * 3 / 4; ++i) {
				low = std::min(low, points[i].w);
				high = std::max(high, points[i].w);
			}
			return std::make_pair(low, high);
		};
		const auto stroke = [&](double duration, bool dynamic) {
			return BuildStroke({
				{ QPointF(0., 0.), 0. },
				{ QPointF(150., 20.), duration / 2. },
				{ QPointF(300., 0.), duration },
			}, { .smoothing = 0.3, .dynamic = dynamic });
		};
		const auto slow = range(stroke(4000., true));
		const auto fast = range(stroke(100., true));
		const auto plain = range(stroke(100., false));
		const auto timeless = range(stroke(0., true));
		info(u"width factor: slow %1, fast %2"_q
			.arg(slow.first, 0, 'f', 2)
			.arg(fast.second, 0, 'f', 2));
		check(
			slow.first > 0.98f && fast.second < 0.45f && fast.first >= 0.34f,
			u"dynamics: full width when slow, thin when fast"_q);
		check(
			plain.first == 1.f && timeless.first == 1.f,
			u"dynamics: off and without times the width is constant"_q);
	}

	// Arrows and constraints.
	{
		const auto head = ArrowHeadFor(QPointF(0., 0.), QPointF(100., 0.), 4.);
		const auto tip = QPointF(100., 0.);
		const auto angle = std::atan2(
			std::abs(head.left.y()),
			tip.x() - head.left.x()) * 180. / kPi;
		check(
			head.valid
				&& std::abs(Distance(tip, head.left) - 18.) < 1e-9
				&& std::abs(Distance(tip, head.right) - 18.) < 1e-9
				&& std::abs(head.left.y() + head.right.y()) < 1e-9
				&& std::abs(head.left.x() - head.right.x()) < 1e-9
				&& head.left.x() < tip.x()
				&& std::abs(angle - 28.) < 1e-6,
			u"arrow: two equal barbs at 28 degrees to the shaft"_q);
		const auto turned = ArrowHeadFor(QPointF(10., 10.), QPointF(10., 90.), 4.);
		check(
			turned.valid
				&& std::abs(turned.left.y() - turned.right.y()) < 1e-9
				&& std::abs((turned.left.x() + turned.right.x()) / 2. - 10.) < 1e-9
				&& turned.left.y() < 90.,
			u"arrow: the head follows the direction"_q);
		const auto tiny = ArrowHeadFor(QPointF(0., 0.), QPointF(10., 0.), 4.);
		check(
			tiny.valid
				&& std::abs(Distance(QPointF(10., 0.), tiny.left) - 6.) < 1e-9,
			u"arrow: the head of a short arrow is a part of its length"_q);
		check(
			!ArrowHeadFor(QPointF(5., 5.), QPointF(5., 5.), 4.).valid,
			u"arrow: no head without a direction"_q);

		const auto flat = ConstrainedLineEnd(QPointF(0., 0.), QPointF(100., 8.));
		const auto diagonal = ConstrainedLineEnd(
			QPointF(10., 10.),
			QPointF(70., 80.));
		check(
			std::abs(flat.y()) < 1e-9
				&& std::abs(flat.x() - std::hypot(100., 8.)) < 1e-9
				&& std::abs((diagonal.x() - 10.) - (diagonal.y() - 10.)) < 1e-9,
			u"shift: a line snaps to 45 degrees and keeps its length"_q);
		check(
			ConstrainedBoxEnd(QPointF(0., 0.), QPointF(50., -20.))
				== QPointF(50., -50.),
			u"shift: a box becomes a square"_q);
	}

	const auto size = QSize(200, 160);
	const auto wave = BuildStroke(
		TestWave(QPointF(20., 40.), QPointF(180., 40.), 18., 60, 600.),
		{ .smoothing = 0.3 });

	// Bounds: nothing is painted outside of them.
	{
		auto dynamic = BuildStroke(
			TestWave(QPointF(20., 90.), QPointF(180., 110.), 25., 40, 90.),
			{ .smoothing = 0.2, .dynamic = true });
		auto filledBox = TestShape(DrawKind::Rectangle, blue, 4., {
			MakePoint(QPointF(120., 100.)),
			MakePoint(QPointF(40., 30.)),
		});
		filledBox.filled = true;
		auto filledOval = filledBox;
		filledOval.kind = DrawKind::Ellipse;
		const auto shapes = std::vector<DrawShape>{
			TestShape(DrawKind::Pen, red, 9., wave),
			TestShape(DrawKind::Pen, red, 14., dynamic),
			TestShape(DrawKind::Marker, blue, 22., wave),
			TestShape(DrawKind::Pencil, red, 6., wave),
			TestShape(DrawKind::Pen, red, 12., { MakePoint(QPointF(60., 60.)) }),
			TestShape(DrawKind::Marker, red, 12., { MakePoint(QPointF(60., 60.)) }),
			TestShape(DrawKind::Line, red, 6., {
				MakePoint(QPointF(20., 20.)),
				MakePoint(QPointF(150., 90.)),
			}),
			TestShape(DrawKind::Arrow, red, 5., {
				MakePoint(QPointF(30., 130.)),
				MakePoint(QPointF(170., 40.)),
			}),
			TestShape(DrawKind::Rectangle, red, 4., {
				MakePoint(QPointF(40., 30.)),
				MakePoint(QPointF(120., 100.)),
			}),
			TestShape(DrawKind::Ellipse, red, 4., {
				MakePoint(QPointF(40., 30.)),
				MakePoint(QPointF(120., 100.)),
			}),
			filledBox,
			filledOval,
		};
		auto inside = true;
		auto painted = true;
		for (const auto &shape : shapes) {
			const auto bounds = DrawShapeBounds(shape);
			const auto image = RenderAt(MakeDrawContent(size, { shape }), 1.);
			auto pixels = 0;
			for (auto y = 0; y != image.height(); ++y) {
				for (auto x = 0; x != image.width(); ++x) {
					if (!qAlpha(image.pixel(x, y))) {
						continue;
					}
					++pixels;
					if (!bounds.contains(QPointF(x + 0.5, y + 0.5))) {
						inside = false;
					}
				}
			}
			if (!pixels || !inside) {
				info(u"shape %1: %2 pixels, inside: %3"_q
					.arg(QString::fromLatin1(KindKey(shape.kind)))
					.arg(pixels)
					.arg(inside ? u"yes"_q : u"no"_q));
			}
			painted = painted && (pixels > 0);
		}
		check(painted, u"bounds: every kind of shape paints something"_q);
		check(inside, u"bounds: nothing is painted outside of them"_q);

		const auto arrow = shapes[7];
		const auto head = ArrowHeadFor(
			PointOf(arrow.points[0]),
			PointOf(arrow.points[1]),
			arrow.width);
		check(
			DrawShapeBounds(arrow).contains(head.left)
				&& DrawShapeBounds(arrow).contains(head.right)
				&& DrawShapeBounds(DrawShape()).isEmpty(),
			u"bounds: the head of an arrow is inside, no points: empty"_q);
		check(
			ShapeTouches(shapes[0], size)
				&& ShapeTouches(
					TestShape(DrawKind::Line, red, 6., {
						MakePoint(QPointF(-20., 10.)),
						MakePoint(QPointF(60., 50.)),
					}),
					size)
				&& !ShapeTouches(
					TestShape(DrawKind::Pen, red, 6., {
						MakePoint(QPointF(-40., 50.)),
					}),
					size)
				&& !ShapeTouches(
					TestShape(DrawKind::Line, red, 6., {
						MakePoint(QPointF(220., 10.)),
						MakePoint(QPointF(260., 150.)),
					}),
					size)
				&& !ShapeTouches(DrawShape(), size),
			u"bounds: a shape that never enters the layer is told apart"_q);

		const auto outline = RenderAt(MakeDrawContent(size, { shapes[8] }), 1.);
		const auto solid = RenderAt(MakeDrawContent(size, { filledBox }), 1.);
		const auto line = RenderAt(MakeDrawContent(size, { shapes[6] }), 1.);
		check(
			AlphaAt(outline, 80, 65) == 0
				&& AlphaAt(outline, 40, 65) == 255
				&& AlphaAt(solid, 80, 65) == 255
				&& AlphaAt(line, 85, 55) == 255
				&& AlphaAt(line, 150, 30) == 0,
			u"figures: an outline is empty inside, a filled box is not"_q);
	}

	// The eraser.
	{
		auto plate = TestShape(DrawKind::Rectangle, red, 2., {
			MakePoint(QPointF(10., 10.)),
			MakePoint(QPointF(90., 90.)),
		});
		plate.filled = true;
		auto eraser = TestShape(DrawKind::Eraser, QColor(), 20., {
			MakePoint(QPointF(0., 50.)),
			MakePoint(QPointF(100., 50.)),
		});
		const auto side = QSize(100, 100);
		const auto wiped = RenderAt(MakeDrawContent(side, { plate, eraser }), 1.);
		check(
			AlphaAt(wiped, 50, 50) == 0
				&& AlphaAt(wiped, 50, 44) == 0
				&& AlphaAt(wiped, 50, 36) == 255
				&& AlphaAt(wiped, 50, 20) == 255
				&& qRed(wiped.pixel(50, 20)) == 230,
			u"eraser: wipes its own width and nothing else"_q);

		auto weak = eraser;
		weak.opacity = 0.5;
		const auto halved = RenderAt(MakeDrawContent(side, { plate, weak }), 1.);
		check(
			std::abs(AlphaAt(halved, 50, 50) - 128) <= 3
				&& AlphaAt(halved, 50, 20) == 255,
			u"eraser: a half strength leaves a half"_q);

		const auto after = TestShape(DrawKind::Line, blue, 6., {
			MakePoint(QPointF(50., 10.)),
			MakePoint(QPointF(50., 90.)),
		});
		const auto kept = RenderAt(
			MakeDrawContent(side, { plate, eraser, after }),
			1.);
		check(
			AlphaAt(kept, 50, 50) == 255
				&& qBlue(kept.pixel(50, 50)) == 230
				&& AlphaAt(kept, 30, 50) == 0,
			u"eraser: what is drawn after it stays"_q);

		const auto alone = RenderAt(MakeDrawContent(side, { eraser }), 1.);
		auto empty = true;
		for (auto y = 0; empty && y != alone.height(); ++y) {
			for (auto x = 0; empty && x != alone.width(); ++x) {
				empty = (alone.pixel(x, y) == 0);
			}
		}
		check(empty, u"eraser: paints nothing by itself"_q);
		check(
			DrawShapeBounds(eraser).contains(QRectF(0., 40., 100., 20.)),
			u"eraser: its bounds cover what it wipes"_q);
	}

	// The pen of a changing width and the pencil.
	{
		const auto corner = TestShape(DrawKind::Pen, red, 16., {
			DrawPoint{ 20.f, 20.f, 1.f },
			DrawPoint{ 60.f, 100.f, 0.4f },
			DrawPoint{ 100.f, 20.f, 1.f },
		});
		const auto image = RenderAt(MakeDrawContent(size, { corner }), 1.);
		auto holes = 0;
		for (auto i = 0; i != 2; ++i) {
			const auto at = [&](int index) {
				return PointOf(corner.points[std::clamp(index, 0, 2)]);
			};
			for (auto j = 0; j <= 20; ++j) {
				const auto point = CatmullRom(
					at(i - 1),
					at(i),
					at(i + 1),
					at(i + 2),
					j / 20.);
				if (AlphaAt(image, int(point.x()), int(point.y())) < 250) {
					++holes;
				}
			}
		}
		check(
			!holes
				&& AlphaAt(image, 20, 14) == 255
				&& AlphaAt(image, 60, 104) == 0,
			u"pen: a stroke of a changing width has no holes"_q);

		const auto pencil = TestShape(DrawKind::Pencil, red, 10., wave);
		const auto pen = TestShape(DrawKind::Pen, red, 10., wave);
		const auto first = RenderAt(MakeDrawContent(size, { pencil }), 1.);
		const auto second = RenderAt(MakeDrawContent(size, { pencil }), 1.);
		const auto plain = RenderAt(MakeDrawContent(size, { pen }), 1.);
		const auto coverage = [](const QImage &image) {
			auto sum = qint64(0);
			for (auto y = 0; y != image.height(); ++y) {
				for (auto x = 0; x != image.width(); ++x) {
					sum += qAlpha(image.pixel(x, y));
				}
			}
			return sum;
		};
		const auto part = coverage(first) / double(std::max(
			coverage(plain),
			qint64(1)));
		info(u"pencil coverage: %1 of the pen"_q.arg(part, 0, 'f', 2));
		check(
			!first.isNull() && first == second,
			u"pencil: the grain is the same every time"_q);
		check(
			part > 0.4 && part < 0.9,
			u"pencil: lighter than the pen, but not empty"_q);
		auto other = pencil;
		other.seed = 77;
		check(
			RenderAt(MakeDrawContent(size, { other }), 1.) != first,
			u"pencil: another seed is another grain"_q);
	}

	// Scales and the cache of the first shapes.
	{
		const auto big = QSize(400, 300);
		const auto stroke = BuildStroke(
			TestWave(QPointF(30., 80.), QPointF(370., 120.), 40., 80, 800.),
			{ .smoothing = 0.3 });
		const auto first = TestShape(DrawKind::Pen, red, 12., stroke);
		auto second = TestShape(DrawKind::Marker, blue, 30., stroke);
		second.opacity = 0.5;
		const auto third = TestShape(DrawKind::Arrow, red, 8., {
			MakePoint(QPointF(60., 260.)),
			MakePoint(QPointF(340., 180.)),
		});
		auto fourth = TestShape(DrawKind::Ellipse, blue, 6., {
			MakePoint(QPointF(150., 150.)),
			MakePoint(QPointF(260., 280.)),
		});
		fourth.filled = true;
		const auto fifth = TestShape(DrawKind::Eraser, QColor(), 26., stroke);

		const auto base = MakeDrawContent(big, { first, second, third });
		const auto full = RenderAt(base, 1.);
		const auto half = RenderAt(base, 0.5);
		const auto difference = FxImageDifference(
			half,
			FxResized(full, half.size()));
		info(u"half scale against the full one: %1"_q
			.arg(difference, 0, 'f', 3));
		check(
			full.size() == big
				&& half.size() == QSize(200, 150)
				&& full.format() == QImage::Format_ARGB32_Premultiplied
				&& difference < 2.,
			u"scale: the small render is the big one, only smaller"_q);
		check(
			RenderAt(base, 2.).size() == QSize(800, 600),
			u"scale: a drawing can be rendered larger than it is"_q);

		const auto grown = DrawWithShape(base, fourth);
		const auto fresh = MakeDrawContent(
			big,
			{ first, second, third, fourth });
		check(
			grown
				&& DrawShapeCount(grown) == 4
				&& DrawShapeCount(base) == 3
				&& RenderAt(grown, 0.5) == RenderAt(fresh, 0.5)
				&& RenderAt(grown, 1.) == RenderAt(fresh, 1.),
			u"cache: a new stroke over the cached ones gives the same"_q);
		const auto sibling = DrawWithShape(base, fifth);
		check(
			sibling
				&& RenderAt(sibling, 0.5) == RenderAt(
					MakeDrawContent(big, { first, second, third, fifth }),
					0.5)
				&& RenderAt(sibling, 0.5) != RenderAt(grown, 0.5)
				&& RenderAt(base, 0.5) == half,
			u"cache: versions of a drawing don't mix up"_q);
		check(
			grown->memoryUsage() < base->memoryUsage()
				&& grown->memoryUsage() > 0,
			u"memory: a new version counts only what it adds"_q);

		auto cancel = std::atomic<bool>(true);
		const auto dropped = sibling->render({
			.scale = 0.25,
			.cancel = &cancel,
		});
		check(dropped.isNull(), u"render: cancelled gives nothing"_q);
		check(
			AsDraw(grown)->renderedAt() > 0
				&& !DrawWithShape(base, DrawShape())
				&& !DrawWithShape(nullptr, first)
				&& !MakeDrawContent(QSize(), { first }),
			u"content: empty shapes and sizes are refused"_q);
		check(
			RenderAt(MakeDrawContent(big), 0.5).size() == QSize(200, 150)
				&& IsDrawContent(base)
				&& !IsTextContent(base)
				&& base->type() == kDrawLayerType,
			u"content: an empty drawing is a transparent picture"_q);
	}

	// Serialization.
	{
		auto dynamic = BuildStroke(
			TestWave(QPointF(20., 90.), QPointF(180., 110.), 25., 40, 90.),
			{ .smoothing = 0.2, .dynamic = true });
		auto marker = TestShape(DrawKind::Marker, blue, 22.5, wave);
		marker.opacity = 0.45;
		auto pencil = TestShape(DrawKind::Pencil, red, 5., wave);
		pencil.seed = 123456789;
		auto oval = TestShape(DrawKind::Ellipse, QColor(1, 2, 3), 3., {
			MakePoint(QPointF(40.25, 30.5)),
			MakePoint(QPointF(120., 100.75)),
		});
		oval.filled = true;
		const auto content = MakeDrawContent(size, {
			TestShape(DrawKind::Pen, red, 14., dynamic),
			marker,
			pencil,
			TestShape(DrawKind::Arrow, red, 5., {
				MakePoint(QPointF(30., 130.)),
				MakePoint(QPointF(170., 40.)),
			}),
			oval,
			TestShape(DrawKind::Eraser, QColor(), 12., wave),
		});
		const auto json = SerializeDrawing(content);
		const auto restored = DeserializeDrawing(json);
		info(u"6 shapes are %1 bytes of JSON"_q.arg(json.size()));
		check(
			restored
				&& restored->size() == size
				&& SameShapes(DrawShapes(content), DrawShapes(restored)),
			u"json: a drawing survives the round trip"_q);
		check(
			restored && SerializeDrawing(restored) == json,
			u"json: the second round gives the same bytes"_q);
		check(
			restored
				&& FxImageDifference(
					RenderAt(content, 1.),
					RenderAt(restored, 1.)) < 0.05,
			u"json: the restored drawing looks the same"_q);
		check(
			!DeserializeDrawing("nope")
				&& !DeserializeDrawing("[1,2]")
				&& !DeserializeDrawing("{\"type\":\"text\"}")
				&& !DeserializeDrawing("{\"type\":\"draw\",\"w\":0,\"h\":5}")
				&& SerializeDrawing(nullptr).isEmpty(),
			u"json: what is not a drawing is refused"_q);
		const auto odd = DeserializeDrawing(
			"{\"type\":\"draw\",\"w\":64,\"h\":48,\"shapes\":["
			"{\"k\":\"spray\",\"p\":[1,2,3]},"
			"{\"k\":\"pen\",\"c\":\"oops\",\"w\":1e12,\"o\":-4,"
			"\"p\":[1,2,9,\"x\",5,1,30,40,0]},"
			"{\"k\":\"line\",\"w\":3,\"p\":[1,2,3,4,5,6]},"
			"{\"k\":\"rect\",\"w\":3,\"p\":[]},"
			"7]}");
		const auto shapes = DrawShapes(odd);
		check(
			shapes.size() == 2
				&& shapes[0].kind == DrawKind::Pen
				&& shapes[0].color == QColor(0, 0, 0)
				&& shapes[0].width == kMaxWidth
				&& shapes[0].opacity == 0.
				&& shapes[0].points.size() == 2
				&& shapes[0].points[0].w == 1.f
				&& shapes[0].points[1].w == float(kMinPointFactor)
				&& shapes[1].kind == DrawKind::Line
				&& shapes[1].points.size() == 2
				&& shapes[1].points[1].x == 5.f,
			u"json: broken values are clamped or dropped"_q);
		check(
			!RenderAt(odd, 1.).isNull(),
			u"json: a repaired drawing still renders"_q);
	}

	// Text layers (the layout itself needs fonts and is not checked here).
	{
		auto style = TextLayerStyle();
		style.text = u"Line one\nand two"_q;
		style.color = QColor(250, 10, 20);
		style.size = 48.5;
		style.weight = TextLayerWeight::Bold;
		style.font = TextLayerFont::Serif;
		style.align = TextLayerAlign::Center;
		style.plate = true;
		style.plateColor = QColor(0, 0, 0);
		style.plateOpacity = 0.5;
		const auto json = SerializeTextStyle(style);
		const auto restored = DeserializeTextStyle(json);
		check(
			restored && *restored == style,
			u"text: the style survives the round trip"_q);
		const auto odd = DeserializeTextStyle(
			"{\"type\":\"text\",\"text\":\"a\\r\\nb\",\"size\":1e9,"
			"\"weight\":\"heavy\",\"align\":\"right\","
			"\"plate_opacity\":7}");
		check(
			odd
				&& odd->text == u"a\nb"_q
				&& odd->size == kMaxTextSize
				&& odd->weight == TextLayerWeight::Semibold
				&& odd->align == TextLayerAlign::Right
				&& odd->plateOpacity == 1.
				&& !DeserializeTextStyle("{\"type\":\"draw\"}")
				&& !DeserializeTextStyle("?"),
			u"text: broken values are repaired, other things refused"_q);

		auto layout = TextLayout();
		layout.size = QSize(100, 40);
		layout.path.setFillRule(Qt::WindingFill);
		layout.path.addRect(QRectF(10., 10., 30., 20.));
		layout.plate = QRectF(0., 0., 100., 40.);
		layout.plateRadius = 6.;
		const auto content = std::make_shared<const TextContent>(
			style,
			layout);
		const auto image = content->render({ .scale = 1. });
		const auto large = content->render({ .scale = 2. });
		check(
			image.size() == QSize(100, 40)
				&& image.pixel(20, 20) == qRgba(250, 10, 20, 255)
				&& qRed(image.pixel(80, 20)) == 0
				&& std::abs(AlphaAt(image, 80, 20) - 128) <= 2
				&& AlphaAt(image, 0, 0) < 60
				&& large.size() == QSize(200, 80)
				&& large.pixel(50, 40) == qRgba(250, 10, 20, 255),
			u"text: the plate and the glyphs are painted at any scale"_q);
		const auto same = TextStyleOf(content);
		check(
			IsTextContent(content)
				&& !IsDrawContent(content)
				&& same
				&& *same == style
				&& !TextStyleOf(nullptr)
				&& content->memoryUsage() > 0,
			u"text: the content keeps its style"_q);
		check(
			TextAnchorShift(100, 140, TextLayerAlign::Left) == 0.
				&& TextAnchorShift(100, 140, TextLayerAlign::Center) == -20.
				&& TextAnchorShift(100, 140, TextLayerAlign::Right) == -40.,
			u"text: the aligned side stays in place when the text grows"_q);
		check(
			TextNameFrom(u"  \n  Hello   there \nmore"_q) == u"Hello there"_q
				&& TextNameFrom(u"abcdefghijklmnopqrstuvwxyz0123"_q)
					== (u"abcdefghijklmnopqrstuvwx"_q + QChar(0x2026))
				&& TextNameFrom(u" \n "_q).isEmpty(),
			u"text: the name of a layer is its first line"_q);
		auto longText = TextLayerStyle();
		longText.text = QString(kMaxTextLength + 500, QChar('a'));
		auto manyLines = TextLayerStyle();
		manyLines.text = QString(kMaxTextLines + 20, QChar('\n'));
		check(
			NormalizedText(longText).text.size() == kMaxTextLength
				&& NormalizedText(manyLines).text.count(QChar('\n'))
					== kMaxTextLines - 1,
			u"text: the length and the number of lines are limited"_q);
		const auto fullText = QString(kMaxTextLines - 1, QChar('\n'));
		check(
			TextLineCount(QString()) == 1
				&& TextLineCount(u"a\nb"_q) == 2
				&& TextRefusesLines(fullText, fullText + QChar('\n'))
				&& !TextRefusesLines(fullText, fullText + QChar('a'))
				&& !TextRefusesLines(u"a"_q, manyLines.text),
			u"text: a full text takes no more lines, a pasted one is cut"_q);

		auto plated = TextLayerStyle();
		plated.plate = true;
		const auto untyped = TextShowsPlate(plated);
		plated.text = u" \n\t "_q;
		const auto blank = TextShowsPlate(plated);
		plated.text = u"Hi"_q;
		auto bare = plated;
		bare.plate = false;
		auto box = TextLayout();
		box.size = QSize(40, 30);
		box.path.setFillRule(Qt::WindingFill);
		plated.text = QString();
		const auto nothing = std::make_shared<const TextContent>(
			plated,
			box)->render({ .scale = 1. });
		auto clear = !nothing.isNull();
		for (auto y = 0; clear && y != nothing.height(); ++y) {
			for (auto x = 0; clear && x != nothing.width(); ++x) {
				clear = (nothing.pixel(x, y) == 0);
			}
		}
		plated.text = u"Hi"_q;
		check(
			!untyped
				&& !blank
				&& TextShowsPlate(plated)
				&& !TextShowsPlate(bare)
				&& clear,
			u"text: no plate in the picture until something is typed"_q);

		using Font = TextLayerFont;
		using Weight = TextLayerWeight;
		check(
			FontWeightFor(Font::Default, Weight::Regular) == QFont::Normal
				&& FontWeightFor(Font::Serif, Weight::Regular) == QFont::Normal
				&& FontWeightFor(Font::Default, Weight::Semibold)
					== QFont::DemiBold
				&& FontWeightFor(Font::Default, Weight::Bold) == QFont::DemiBold
				&& FontWeightFor(Font::Serif, Weight::Semibold) == QFont::Bold
				&& FontWeightFor(Font::Mono, Weight::Bold) == QFont::Bold
				&& WeightChoice(Weight::Regular) == 0
				&& WeightChoice(Weight::Semibold) == 1
				&& WeightChoice(Weight::Bold) == 1
				&& WeightFromChoice(0) == Weight::Regular
				&& WeightFromChoice(1) == TextLayerStyle().weight
				&& WeightChoice(WeightFromChoice(1)) == 1,
			u"text: two weights, each one a face the font really has"_q);

		const auto none = Qt::KeyboardModifiers();
		check(
			FieldKeepsKey(Qt::Key_Backspace, none)
				&& FieldKeepsKey(Qt::Key_Delete, none)
				&& FieldKeepsKey(Qt::Key_Backspace, Qt::ControlModifier)
				&& FieldKeepsKey(Qt::Key_Escape, none)
				&& FieldKeepsKey(Qt::Key_Up, none)
				&& FieldKeepsKey(Qt::Key_Left, Qt::ShiftModifier)
				&& FieldKeepsKey(Qt::Key_Return, none)
				&& FieldKeepsKey(Qt::Key_V, none)
				&& FieldKeepsKey(Qt::Key_7, Qt::KeypadModifier),
			u"text: a key the field has not used stops at the field"_q);
		check(
			!FieldKeepsKey(Qt::Key_S, Qt::ControlModifier)
				&& !FieldKeepsKey(
					Qt::Key_Equal,
					Qt::ControlModifier | Qt::ShiftModifier)
				&& !FieldKeepsKey(Qt::Key_Return, Qt::MetaModifier)
				&& !FieldKeepsKey(Qt::Key_Shift, Qt::ShiftModifier)
				&& !FieldKeepsKey(Qt::Key_Alt, none),
			u"text: shortcuts with Ctrl / Cmd and modifiers go on"_q);
	}
	return ok;
}

const auto SelfTest = SelfTestRegistrar(
	SelfTestSuite::Doc,
	"draw",
	&RunDrawSelfTest);

} // namespace

// The interface part: tools, options and scenes.

namespace {

using namespace EditorUi;

constexpr auto kPadding = 16;
constexpr auto kSkip = 8;
constexpr auto kHintTop = 10;
constexpr auto kBottomSkip = 6;

// The size sliders are in thousandths of the shorter side of the canvas,
// so "6" looks the same on a small picture and on a 12 MP photo.
constexpr auto kMinBrushSize = 1;
constexpr auto kMaxBrushSize = 200;
constexpr auto kMinTextUnits = 8;
constexpr auto kMaxTextUnits = 400;
constexpr auto kDefaultTextUnits = 64;

constexpr auto kRecentColors = 8;
constexpr auto kSwatchTop = 6;
constexpr auto kSwatchLabelHeight = 22;
constexpr auto kSwatchRowHeight = 34;
constexpr auto kSwatchSize = 24;
constexpr auto kSwatchSkip = 8;
constexpr auto kSwatchRing = 2;
constexpr auto kSwatchPadding = 5;
constexpr auto kRecentLabelHeight = 20;

constexpr auto kMaxRawPoints = 20000;
constexpr auto kMinMoveScreen = 0.75;
constexpr auto kMinFigureScreen = 3.;
constexpr auto kDragThreshold = 4.;
constexpr auto kRingMinRadius = 3.;
constexpr auto kPendingCheck = crl::time(16);
constexpr auto kPendingAfterRender = crl::time(180);
constexpr auto kPendingTimeout = crl::time(1500);
constexpr auto kPendingWatchSide = 800;
constexpr auto kPendingWatchPart = 0.5;
constexpr auto kMaxPending = 16;

constexpr auto kTextLineHeight = 1.08;
constexpr auto kTextPadding = 0.16;
constexpr auto kTextPaddingTop = 0.12;
constexpr auto kPlatePadding = 0.42;
constexpr auto kPlatePaddingTop = 0.22;
constexpr auto kPlateRadius = 0.32;
constexpr auto kEmojiSide = 0.92;
constexpr auto kEmojiAdvance = 1.08;
constexpr auto kMaxTextEmoji = 200;
constexpr auto kEmojiCacheLimit = 256;
constexpr auto kFieldMinHeight = 64;
constexpr auto kFieldMaxHeight = 132;

// Icons: 24 x 24 logical pixels inside rect, see ToolDescriptor.

template <typename Draw>
void PaintIconGrid(QPainter &p, QRectF rect, QColor color, Draw &&draw) {
	p.save();
	p.translate(rect.topLeft());
	p.scale(rect.width() / 24., rect.height() / 24.);
	p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	p.setBrush(Qt::NoBrush);
	draw();
	p.restore();
}

// A writing tool seen from the side, its tip looks down and to the left:
// draw is given a frame where the tool stands upright, the tip at +y.
template <typename Draw>
void PaintIconTilted(QPainter &p, QRectF rect, QColor color, Draw &&draw) {
	PaintIconGrid(p, rect, color, [&] {
		p.translate(12., 12.);
		p.rotate(45.);
		draw();
	});
}

// The nib of a fountain pen with its slit and the hole above it, and
// a short piece of the holder: nothing like the pencil next to it.
void PaintPenIcon(QPainter &p, QRectF rect, QColor color) {
	PaintIconTilted(p, rect, color, [&] {
		p.drawRoundedRect(QRectF(-2.8, -9.4, 5.6, 4.2), 1., 1.);
		auto nib = QPainterPath();
		nib.moveTo(-2.8, -5.2);
		nib.cubicTo(-5.6, -1.6, -4.4, 2.4, 0., 9.4);
		nib.cubicTo(4.4, 2.4, 5.6, -1.6, 2.8, -5.2);
		p.drawPath(nib);
		p.drawLine(QPointF(0., 2.8), QPointF(0., 8.2));
		p.setBrush(color);
		p.drawEllipse(QPointF(0., 1.4), 0.7, 0.7);
	});
}

// A highlighter: a short thick barrel and a wide tip that is cut at an
// angle and filled, so it can't be taken for the nib or for the pencil.
void PaintMarkerIcon(QPainter &p, QRectF rect, QColor color) {
	PaintIconTilted(p, rect, color, [&] {
		p.drawRoundedRect(QRectF(-4., -8.8, 8., 8.6), 1.4, 1.4);
		auto tip = QPainterPath();
		tip.moveTo(-3., -0.2);
		tip.lineTo(-3., 9.2);
		tip.lineTo(3., 5.6);
		tip.lineTo(3., -0.2);
		tip.closeSubpath();
		p.drawPath(tip);
		auto felt = QPainterPath();
		felt.moveTo(-3., 3.2);
		felt.lineTo(-3., 9.2);
		felt.lineTo(3., 5.6);
		felt.lineTo(3., 3.2);
		felt.closeSubpath();
		p.setBrush(color);
		p.drawPath(felt);
	});
}

// A pencil with facets: a long thin body with an edge along it, the wavy
// line where the paint ends and a sharpened lead.
void PaintPencilIcon(QPainter &p, QRectF rect, QColor color) {
	PaintIconTilted(p, rect, color, [&] {
		auto body = QPainterPath();
		body.moveTo(-3., -9.6);
		body.lineTo(3., -9.6);
		body.lineTo(3., 3.6);
		body.lineTo(0., 9.8);
		body.lineTo(-3., 3.6);
		body.closeSubpath();
		p.drawPath(body);
		auto paint = QPainterPath();
		paint.moveTo(-3., 3.6);
		paint.quadTo(-1.5, 1.4, 0., 3.6);
		paint.quadTo(1.5, 1.4, 3., 3.6);
		p.drawPath(paint);
		p.drawLine(QPointF(0., -9.6), QPointF(0., 3.2));
		auto lead = QPainterPath();
		lead.moveTo(-1., 7.7);
		lead.lineTo(0., 9.8);
		lead.lineTo(1., 7.7);
		lead.closeSubpath();
		p.setBrush(color);
		p.drawPath(lead);
	});
}

void PaintEraserIcon(QPainter &p, QRectF rect, QColor color) {
	PaintIconTilted(p, rect, color, [&] {
		p.translate(0., -1.4);
		p.drawRoundedRect(QRectF(-4., -7.2, 8., 14.4), 1.6, 1.6);
		p.drawLine(QPointF(-4., 1.8), QPointF(4., 1.8));
	});
	PaintIconGrid(p, rect, color, [&] {
		p.drawLine(QPointF(11.5, 20.2), QPointF(20., 20.2));
	});
}

void PaintLineIcon(QPainter &p, QRectF rect, QColor color) {
	PaintIconGrid(p, rect, color, [&] {
		p.drawLine(QPointF(6.6, 17.4), QPointF(17.4, 6.6));
		p.setBrush(color);
		p.drawEllipse(QPointF(5.8, 18.2), 1.3, 1.3);
		p.drawEllipse(QPointF(18.2, 5.8), 1.3, 1.3);
	});
}

void PaintArrowIcon(QPainter &p, QRectF rect, QColor color) {
	PaintIconGrid(p, rect, color, [&] {
		auto path = QPainterPath();
		path.moveTo(5.8, 18.2);
		path.lineTo(18., 6.);
		path.moveTo(11., 6.);
		path.lineTo(18., 6.);
		path.lineTo(18., 13.);
		p.drawPath(path);
	});
}

void PaintRectIcon(QPainter &p, QRectF rect, QColor color) {
	PaintIconGrid(p, rect, color, [&] {
		p.drawRoundedRect(QRectF(4.5, 6.5, 15., 11.), 2., 2.);
	});
}

void PaintEllipseIcon(QPainter &p, QRectF rect, QColor color) {
	PaintIconGrid(p, rect, color, [&] {
		p.drawEllipse(QPointF(12., 12.), 7.8, 6.);
	});
}

void PaintTextIcon(QPainter &p, QRectF rect, QColor color) {
	PaintIconGrid(p, rect, color, [&] {
		auto path = QPainterPath();
		path.moveTo(6.5, 9.);
		path.lineTo(6.5, 6.8);
		path.lineTo(17.5, 6.8);
		path.lineTo(17.5, 9.);
		path.moveTo(12., 6.8);
		path.lineTo(12., 17.6);
		path.moveTo(9.8, 17.6);
		path.lineTo(14.2, 17.6);
		p.drawPath(path);
	});
}

// A drawing layer in the lists: a scribble.
void PaintDrawingIcon(QPainter &p, QRectF rect, QColor color) {
	PaintIconGrid(p, rect, color, [&] {
		auto path = QPainterPath();
		path.moveTo(4.6, 15.4);
		path.cubicTo(6.4, 8.8, 9.6, 7.2, 10.6, 10.6);
		path.cubicTo(11.6, 14., 8.2, 17.6, 10.4, 18.);
		path.cubicTo(13.2, 18.4, 14.6, 9.8, 19.4, 8.6);
		p.drawPath(path);
	});
}

// Sample drawings for the scenes, in the coordinates of a canvas.

template <typename Curve>
[[nodiscard]] std::vector<DrawPoint> SampleStroke(
		QSize size,
		Curve &&curve,
		int count,
		double duration,
		bool dynamic = false) {
	auto raw = std::vector<RawPoint>();
	for (auto i = 0; i <= count; ++i) {
		const auto t = i / double(count);
		const auto point = curve(t);
		raw.push_back({
			QPointF(point.x() * size.width(), point.y() * size.height()),
			duration * t * t,
		});
	}
	return BuildStroke(raw, {
		.smoothing = 0.35,
		.unit = std::max(size.width(), size.height()) / 900.,
		.dynamic = dynamic,
	});
}

[[nodiscard]] DrawShape SampleShape(
		DrawKind kind,
		QColor color,
		double width,
		double opacity,
		std::vector<DrawPoint> points) {
	auto result = DrawShape();
	result.kind = kind;
	result.color = color;
	result.width = width;
	result.opacity = opacity;
	result.seed = 7;
	result.points = std::move(points);
	return result;
}

[[nodiscard]] std::vector<DrawShape> SampleFreehand(QSize size) {
	const auto unit = std::max(std::min(size.width(), size.height()), 1)
		/ 1000.;
	auto result = std::vector<DrawShape>();
	// A marker band, something circled with the pen, a quick signature
	// that gets thinner where it is fast, a pencil hatching and an eraser
	// stroke through the band. The band and the hatching are where the
	// sample photo has its sky: a yellow marker on the yellow roofs and
	// a dark pencil on the dark water could not be seen at all.
	result.push_back(SampleShape(
		DrawKind::Marker,
		QColor(0xFD, 0xD8, 0x35),
		46. * unit,
		0.45,
		SampleStroke(size, [](double t) {
			return QPointF(
				0.2 + 0.5 * t,
				0.085 + 0.012 * std::sin(t * 9.));
		}, 60, 700.)));
	result.push_back(SampleShape(
		DrawKind::Pen,
		QColor(0xE5, 0x39, 0x35),
		9. * unit,
		1.,
		SampleStroke(size, [](double t) {
			const auto angle = -2.2 + t * 2.15 * kPi;
			const auto grow = 1. + 0.08 * t;
			return QPointF(
				0.7 + 0.16 * grow * std::cos(angle),
				0.4 + 0.2 * grow * std::sin(angle));
		}, 90, 900.)));
	result.push_back(SampleShape(
		DrawKind::Pen,
		QColor(0xFF, 0xFF, 0xFF),
		12. * unit,
		1.,
		SampleStroke(size, [](double t) {
			return QPointF(
				0.1 + 0.42 * t + 0.03 * std::sin(t * 22.),
				0.78 - 0.1 * std::sin(t * 13.) * (1. - 0.5 * t));
		}, 140, 520., true)));
	for (auto i = 0; i != 7; ++i) {
		result.push_back(SampleShape(
			DrawKind::Pencil,
			QColor(0x21, 0x21, 0x21),
			5. * unit,
			0.9,
			SampleStroke(size, [=](double t) {
				return QPointF(
					0.85 + 0.016 * i + 0.03 * t,
					0.27 - 0.17 * t + 0.004 * std::sin(t * 30. + i));
			}, 30, 200.)));
	}
	result.push_back(SampleShape(
		DrawKind::Eraser,
		QColor(),
		30. * unit,
		1.,
		SampleStroke(size, [](double t) {
			return QPointF(0.38 + 0.06 * t, 0.02 + 0.14 * t);
		}, 20, 200.)));
	return result;
}

[[nodiscard]] std::vector<DrawShape> SampleFigures(QSize size) {
	const auto unit = std::max(std::min(size.width(), size.height()), 1)
		/ 1000.;
	const auto point = [&](double x, double y) {
		return MakePoint(QPointF(x * size.width(), y * size.height()));
	};
	auto result = std::vector<DrawShape>();
	auto plate = SampleShape(
		DrawKind::Rectangle,
		QColor(0x1E, 0x88, 0xE5),
		6. * unit,
		0.35,
		{ point(0.08, 0.1), point(0.4, 0.34) });
	plate.filled = true;
	result.push_back(plate);
	result.push_back(SampleShape(
		DrawKind::Rectangle,
		QColor(0xFF, 0xFF, 0xFF),
		7. * unit,
		1.,
		{ point(0.08, 0.1), point(0.4, 0.34) }));
	result.push_back(SampleShape(
		DrawKind::Ellipse,
		QColor(0xE5, 0x39, 0x35),
		9. * unit,
		1.,
		{ point(0.56, 0.2), point(0.86, 0.58) }));
	result.push_back(SampleShape(
		DrawKind::Arrow,
		QColor(0xFD, 0xD8, 0x35),
		11. * unit,
		1.,
		{ point(0.2, 0.86), point(0.56, 0.5) }));
	result.push_back(SampleShape(
		DrawKind::Line,
		QColor(0xFF, 0xFF, 0xFF),
		5. * unit,
		0.8,
		{ point(0.62, 0.8), point(0.92, 0.8) }));
	result.push_back(SampleShape(
		DrawKind::Arrow,
		QColor(0x7C, 0xB3, 0x42),
		7. * unit,
		1.,
		{ point(0.9, 0.9), point(0.74, 0.66) }));
	return result;
}

// The end of the parts that need nothing but a painter.

[[nodiscard]] style::margins RowMargins(int top = 0) {
	return style::margins(Px(kPadding), top, Px(kPadding), 0);
}

[[nodiscard]] QColor TextColor() {
	return st::groupCallMembersFg->c;
}

[[nodiscard]] QColor SubTextColor() {
	return st::groupCallMemberNotJoinedStatus->c;
}

[[nodiscard]] const std::vector<QColor> &PalettePresets() {
	static const auto result = std::vector<QColor>{
		QColor(0xFF, 0xFF, 0xFF),
		QColor(0x00, 0x00, 0x00),
		QColor(0xE5, 0x39, 0x35),
		QColor(0xFB, 0x8C, 0x00),
		QColor(0xFD, 0xD8, 0x35),
		QColor(0x7C, 0xB3, 0x42),
		QColor(0x1E, 0x88, 0xE5),
		QColor(0x8E, 0x24, 0xAA),
	};
	return result;
}

struct BrushState {
	int size = 6;
	int opacity = 100;
	int smoothing = 40;
	bool dynamic = false;
	bool filled = false;
};

[[nodiscard]] BrushState DefaultBrush(DrawKind kind) {
	switch (kind) {
	case DrawKind::Pen: return { .size = 6, .opacity = 100, .smoothing = 40 };
	case DrawKind::Marker: return { .size = 28, .opacity = 45, .smoothing = 40 };
	case DrawKind::Pencil: return { .size = 4, .opacity = 90, .smoothing = 25 };
	case DrawKind::Eraser: return { .size = 36, .opacity = 100, .smoothing = 30 };
	case DrawKind::Line:
	case DrawKind::Arrow:
	case DrawKind::Rectangle:
	case DrawKind::Ellipse: return { .size = 6, .opacity = 100, .smoothing = 0 };
	}
	return {};
}

struct TextFocusRequest {
	const Controller *controller = nullptr;
	bool selectAll = false;
	// Set by the panel that has given the focus to its field: a field is
	// only on two pages of the side panel, another one may be open.
	bool *taken = nullptr;
};

// What the tools remember while the app runs: the settings of every
// brush, of the next text and the colors used lately. Main thread.
struct ToolState {
	QColor color = QColor(0xE5, 0x39, 0x35);
	std::array<BrushState, kDrawKindCount> brushes;
	TextLayerStyle text; // Without the text itself and the size.
	int textUnits = kDefaultTextUnits;
	std::vector<QColor> recent;
	rpl::event_stream<> changes;
	rpl::event_stream<TextFocusRequest> textFocus;
};

[[nodiscard]] ToolState &State() {
	// Never destroyed: nothing here may depend on the order in which the
	// statics die when the app quits.
	static const auto result = [] {
		const auto state = new ToolState();
		for (auto i = 0; i != kDrawKindCount; ++i) {
			state->brushes[i] = DefaultBrush(DrawKind(i));
		}
		return state;
	}();
	return *result;
}

[[nodiscard]] double DocumentUnit(QSize size) {
	return std::max(std::min(size.width(), size.height()), 1) / 1000.;
}

void RememberColor(QColor color) {
	if (!color.isValid()) {
		return;
	}
	color = color.toRgb();
	color.setAlpha(255);
	for (const auto &preset : PalettePresets()) {
		if (preset.rgb() == color.rgb()) {
			return;
		}
	}
	auto &state = State();
	const auto i = ranges::find(state.recent, color);
	if (i != end(state.recent)) {
		if (i == begin(state.recent)) {
			return;
		}
		state.recent.erase(i);
	}
	state.recent.insert(begin(state.recent), color);
	if (int(state.recent.size()) > kRecentColors) {
		state.recent.resize(kRecentColors);
	}
	state.changes.fire({});
}

[[nodiscard]] QString UniqueLayerName(
		const Document &document,
		const QString &base) {
	if (!ranges::contains(document.layers, base, &Layer::name)) {
		return base;
	}
	for (auto index = 2;; ++index) {
		const auto name = base + QChar(' ') + QString::number(index);
		if (!ranges::contains(document.layers, name, &Layer::name)) {
			return name;
		}
	}
}

void FocusEditorFrom(not_null<QWidget*> from) {
	for (auto parent = from->parentWidget()
		; parent
		; parent = parent->parentWidget()) {
		if (parent->focusPolicy() == Qt::StrongFocus) {
			parent->setFocus();
			return;
		}
	}
}

// Preset colors, the "any color" button and a row of the colors that
// were used lately.
class Palette final : public Ui::RpWidget {
public:
	Palette(QWidget *parent, rpl::producer<QString> label, QColor current);

	void setColor(QColor color);
	[[nodiscard]] QColor color() const;
	[[nodiscard]] rpl::producer<QColor> chosen() const;
	[[nodiscard]] rpl::producer<> customRequests() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	enum class Type : uchar {
		Preset,
		Custom,
		Recent,
	};
	struct Item {
		QRect rect;
		QColor color;
		Type type = Type::Preset;
	};

	[[nodiscard]] int itemAt(QPoint point) const;
	void setOver(int over);

	QString _label;
	const QString _recentLabel;
	QColor _current;
	std::vector<QColor> _recent;
	std::vector<Item> _items;
	int _recentLabelTop = -1;
	int _over = -1;
	int _pressed = -1;
	rpl::event_stream<QColor> _chosen;
	rpl::event_stream<> _customRequests;

};

Palette::Palette(
	QWidget *parent,
	rpl::producer<QString> label,
	QColor current)
: RpWidget(parent)
, _recentLabel(tr::lng_oblivion_photo_draw_recent(tr::now))
, _current(current)
, _recent(State().recent) {
	std::move(label) | rpl::on_next([=](QString value) {
		_label = std::move(value);
		update();
	}, lifetime());
	State().changes.events() | rpl::on_next([=] {
		if (_recent != State().recent) {
			_recent = State().recent;
			resizeToWidth(width());
			update();
		}
	}, lifetime());
	setMouseTracking(true);
}

void Palette::setColor(QColor color) {
	if (_current != color) {
		_current = color;
		update();
	}
}

QColor Palette::color() const {
	return _current;
}

rpl::producer<QColor> Palette::chosen() const {
	return _chosen.events();
}

rpl::producer<> Palette::customRequests() const {
	return _customRequests.events();
}

int Palette::resizeGetHeight(int newWidth) {
	_items.clear();
	const auto size = Px(kSwatchSize);
	const auto step = Px(kSwatchSize + kSwatchSkip);
	const auto left = Px(kSwatchPadding);
	const auto rowHeight = Px(kSwatchRowHeight);
	const auto perRow = std::max(
		(newWidth - left + Px(kSwatchSkip)) / std::max(step, 1),
		1);
	auto top = Px(kSwatchTop + kSwatchLabelHeight);
	auto column = 0;
	const auto place = [&](QColor color, Type type) {
		if (column >= perRow) {
			column = 0;
			top += rowHeight;
		}
		_items.push_back({
			QRect(left + column * step, top + (rowHeight - size) / 2, size, size),
			color,
			type,
		});
		++column;
	};
	for (const auto &preset : PalettePresets()) {
		place(preset, Type::Preset);
	}
	place(QColor(), Type::Custom);
	top += rowHeight;
	_recentLabelTop = -1;
	if (!_recent.empty()) {
		_recentLabelTop = top;
		top += Px(kRecentLabelHeight);
		column = 0;
		for (const auto &color : _recent) {
			place(color, Type::Recent);
		}
		top += rowHeight;
	}
	return top;
}

int Palette::itemAt(QPoint point) const {
	const auto ring = Px(kSwatchRing) * 2;
	for (auto i = 0; i != int(_items.size()); ++i) {
		if (_items[i].rect.marginsAdded({ ring, ring, ring, ring }).contains(
				point)) {
			return i;
		}
	}
	return -1;
}

void Palette::setOver(int over) {
	if (_over != over) {
		_over = over;
		setCursor((over >= 0) ? style::cur_pointer : style::cur_default);
		update();
	}
}

void Palette::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	p.setFont(st::normalFont);
	p.setPen(TextColor());
	p.drawText(
		QRect(0, Px(kSwatchTop), width(), Px(kSwatchLabelHeight)),
		Qt::AlignLeft | Qt::AlignVCenter,
		st::normalFont->elided(_label, width()));
	if (_recentLabelTop >= 0) {
		const auto &font = SmallFont();
		p.setFont(font);
		p.setPen(SubTextColor());
		p.drawText(
			QRect(0, _recentLabelTop, width(), Px(kRecentLabelHeight)),
			Qt::AlignLeft | Qt::AlignVCenter,
			font->elided(_recentLabel, width()));
	}

	const auto ring = Px(kSwatchRing);
	const auto paintRing = [&](QRect rect, QColor color) {
		p.setPen(QPen(color, ring));
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(QRectF(rect).marginsAdded(
			{ ring * 1.5, ring * 1.5, ring * 1.5, ring * 1.5 }));
	};
	// A hairline keeps the dark colors visible on the dark panel.
	auto outline = TextColor();
	outline.setAlphaF(0.2f);
	const auto hairline = style::ConvertScaleExact(1.);
	auto matched = false;
	auto custom = -1;
	for (auto i = 0; i != int(_items.size()); ++i) {
		const auto &item = _items[i];
		if (item.type == Type::Custom) {
			custom = i;
			continue;
		}
		p.setPen(Qt::NoPen);
		p.setBrush(item.color);
		p.drawEllipse(item.rect);
		p.setPen(QPen(outline, hairline));
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(QRectF(item.rect).marginsRemoved({
			hairline / 2.,
			hairline / 2.,
			hairline / 2.,
			hairline / 2. }));
		if (!matched && item.color.rgb() == _current.rgb()) {
			matched = true;
			paintRing(item.rect, TextColor());
		} else if (i == _over) {
			paintRing(item.rect, SubTextColor());
		}
	}
	if (custom < 0) {
		return;
	}
	const auto rect = _items[custom].rect;
	auto gradient = QConicalGradient(QRectF(rect).center(), 90.);
	const auto stops = 6;
	for (auto i = 0; i <= stops; ++i) {
		gradient.setColorAt(
			i / float64(stops),
			QColor::fromHsv((i * 360 / stops) % 360, 200, 255));
	}
	p.setPen(Qt::NoPen);
	p.setBrush(gradient);
	p.drawEllipse(rect);
	const auto inner = Px(kSwatchSize) / 4.;
	p.setBrush(matched ? st::groupCallMembersBg->c : _current);
	p.drawEllipse(QRectF(rect).marginsRemoved(
		{ inner, inner, inner, inner }));
	if (!matched) {
		paintRing(rect, TextColor());
	} else if (_over == custom) {
		paintRing(rect, SubTextColor());
	}
}

void Palette::mouseMoveEvent(QMouseEvent *e) {
	setOver(itemAt(e->pos()));
}

void Palette::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = itemAt(e->pos());
	}
}

void Palette::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto pressed = std::exchange(_pressed, -1);
	const auto index = itemAt(e->pos());
	if (index < 0 || index != pressed || index >= int(_items.size())) {
		return;
	}
	const auto item = _items[index];
	if (item.type == Type::Custom) {
		_customRequests.fire({});
	} else {
		_chosen.fire_copy(item.color);
	}
}

void Palette::leaveEventHook(QEvent *e) {
	setOver(-1);
}

// Adds a palette whose choice (a preset, a recent color or one from the
// color picker) is shown by it and reported to chosen.
not_null<Palette*> AddPalette(
		not_null<Ui::VerticalLayout*> layout,
		not_null<Controller*> controller,
		QColor current,
		Fn<void(QColor color)> chosen) {
	const auto palette = layout->add(
		object_ptr<Palette>(
			layout,
			tr::lng_oblivion_photo_draw_color(),
			current),
		RowMargins());
	palette->chosen() | rpl::on_next([=](QColor color) {
		palette->setColor(color);
		chosen(color);
	}, palette->lifetime());
	palette->customRequests() | rpl::on_next([=] {
		const auto weak = base::make_weak(palette);
		ShowColorPickerBox(
			controller->uiShow(),
			palette->color(),
			[=](QColor color) {
				if (!weak.get()) {
					return;
				}
				color.setAlpha(255);
				RememberColor(color);
				palette->setColor(color);
				chosen(color);
			});
	}, palette->lifetime());
	return palette;
}

// The hints name the key that draws a figure from its center the way
// a Windows keyboard does. A Mac keyboard calls the same key (it is
// Qt::AltModifier there as well) Option.
[[nodiscard]] QString WithPlatformKeyNames(QString text) {
	if (Platform::IsMac()) {
		text.replace(u"Alt"_q, u"Option"_q);
	}
	return text;
}

[[nodiscard]] rpl::producer<QString> BrushHint(DrawKind kind) {
	switch (kind) {
	case DrawKind::Pen:
	case DrawKind::Marker:
	case DrawKind::Pencil:
		return tr::lng_oblivion_photo_draw_hint_freehand();
	case DrawKind::Eraser:
		return tr::lng_oblivion_photo_draw_hint_eraser();
	case DrawKind::Line:
	case DrawKind::Arrow:
		return tr::lng_oblivion_photo_draw_hint_line();
	case DrawKind::Rectangle:
		return tr::lng_oblivion_photo_draw_hint_rect(
		) | rpl::map([](QString text) {
			return WithPlatformKeyNames(std::move(text));
		});
	case DrawKind::Ellipse:
		return tr::lng_oblivion_photo_draw_hint_ellipse(
		) | rpl::map([](QString text) {
			return WithPlatformKeyNames(std::move(text));
		});
	}
	return rpl::single(QString());
}

[[nodiscard]] std::vector<FxParam> BrushParams(DrawKind kind) {
	const auto initial = DefaultBrush(kind);
	auto result = std::vector<FxParam>();
	result.push_back(FxInt(
		"size",
		tr::lng_oblivion_photo_draw_size,
		kMinBrushSize,
		kMaxBrushSize,
		initial.size));
	result.push_back(FxInt(
		"opacity",
		(kind == DrawKind::Eraser)
			? tr::lng_oblivion_photo_draw_strength
			: tr::lng_oblivion_photo_draw_opacity,
		1,
		100,
		initial.opacity,
		u"%"_q));
	if (DrawKindIsFreehand(kind)) {
		result.push_back(FxInt(
			"smoothing",
			tr::lng_oblivion_photo_draw_smoothing,
			0,
			100,
			initial.smoothing,
			u"%"_q));
	}
	if (kind == DrawKind::Pen) {
		result.push_back(FxBool(
			"dynamic",
			tr::lng_oblivion_photo_draw_dynamic,
			initial.dynamic));
	} else if (kind == DrawKind::Rectangle || kind == DrawKind::Ellipse) {
		result.push_back(FxBool(
			"fill",
			tr::lng_oblivion_photo_draw_fill,
			initial.filled));
	}
	return result;
}

[[nodiscard]] FxParams BrushValues(DrawKind kind) {
	const auto &brush = State().brushes[int(kind)];
	auto result = FxParams();
	result.set("size", FxValue::Integer(brush.size));
	result.set("opacity", FxValue::Integer(brush.opacity));
	result.set("smoothing", FxValue::Integer(brush.smoothing));
	result.set("dynamic", FxValue::Boolean(brush.dynamic));
	result.set("fill", FxValue::Boolean(brush.filled));
	return result;
}

// The options of a brush or a figure tool, what the Tool tab shows.
[[nodiscard]] object_ptr<Ui::RpWidget> CreateBrushOptions(
		not_null<QWidget*> parent,
		not_null<Controller*> controller,
		DrawKind kind) {
	auto result = object_ptr<Ui::VerticalLayout>(parent);
	const auto raw = result.data();
	if (kind != DrawKind::Eraser) {
		AddPalette(raw, controller, State().color, [=](QColor color) {
			State().color = color;
			State().changes.fire({});
		});
	}
	raw->add(
		CreateParamsPanel(raw, ParamsPanelArgs{
			.params = BrushParams(kind),
			.values = BrushValues(kind),
			.updates = State().changes.events() | rpl::map([=] {
				return BrushValues(kind);
			}),
			.changed = [=](const QByteArray &id, FxValue value, bool) {
				auto &brush = State().brushes[int(kind)];
				if (id == "size") {
					brush.size = std::clamp(
						value.integer(),
						kMinBrushSize,
						kMaxBrushSize);
				} else if (id == "opacity") {
					brush.opacity = std::clamp(value.integer(), 1, 100);
				} else if (id == "smoothing") {
					brush.smoothing = std::clamp(value.integer(), 0, 100);
				} else if (id == "dynamic") {
					brush.dynamic = value.boolean();
				} else if (id == "fill") {
					brush.filled = value.boolean();
				}
				State().changes.fire({});
			},
			.controller = controller.get(),
			// 1 .. 200 with the usual sizes below 20: on an even scale
			// the knob would sit at the very start of the track.
			.logarithmic = { QByteArray("size") },
		}),
		RowMargins());
	raw->add(
		object_ptr<Ui::FlatLabel>(raw, BrushHint(kind), HintLabelStyle()),
		RowMargins(Px(kHintTop)));
	raw->add(object_ptr<Ui::FixedHeightWidget>(raw, Px(kBottomSkip)));
	return result;
}

//
// Text: the layout (main thread, with fonts) and the settings panel.
//

[[nodiscard]] QFont FontFor(const TextLayerStyle &style) {
	auto font = st::normalFont->f;
	switch (style.font) {
	case TextLayerFont::Default: break;
	case TextLayerFont::Serif:
		font = QFont();
		font.setFamilies({
			u"Georgia"_q,
			u"Times New Roman"_q,
			u"DejaVu Serif"_q,
		});
		font.setStyleHint(QFont::Serif);
		break;
	case TextLayerFont::Mono:
		font = QFont();
		font.setFamilies({
			u"Menlo"_q,
			u"Consolas"_q,
			u"DejaVu Sans Mono"_q,
			u"Courier New"_q,
		});
		font.setStyleHint(QFont::Monospace);
		break;
	}
	font.setPixelSize(std::max(int(std::lround(style.size)), 1));
	font.setWeight(FontWeightFor(style.font, style.weight));
	font.setItalic(false);
	font.setUnderline(false);
	font.setStrikeOut(false);
	font.setKerning(true);
	font.setHintingPreference(QFont::PreferNoHinting);
	return font;
}

// The picture of an emoji from the set of the app, as large as the app
// has it. Emoji have no outlines, so they are kept as pictures.
[[nodiscard]] QImage EmojiImage(EmojiPtr emoji) {
	static auto Cache = new base::flat_map<EmojiPtr, QImage>();
	static auto CacheSet = -1;
	// The pictures of another emoji set are other pictures.
	if (const auto set = Ui::Emoji::CurrentSetId(); CacheSet != set) {
		CacheSet = set;
		Cache->clear();
	}
	const auto i = Cache->find(emoji);
	if (i != end(*Cache)) {
		return i->second;
	}
	const auto size = Ui::Emoji::GetSizeLarge();
	if (size <= 0) {
		return QImage();
	}
	auto result = QImage(size, size, QImage::Format_ARGB32_Premultiplied);
	if (result.isNull()) {
		return result;
	}
	result.fill(Qt::transparent);
	// The sprites are drawn in logical pixels of this ratio.
	result.setDevicePixelRatio(style::DevicePixelRatio());
	{
		auto p = QPainter(&result);
		Ui::Emoji::Draw(p, emoji, size, 0, 0);
	}
	result.setDevicePixelRatio(1.);
	if (int(Cache->size()) >= kEmojiCacheLimit) {
		Cache->clear();
	}
	Cache->emplace(emoji, result);
	return result;
}

[[nodiscard]] TextLayout LayoutText(const TextLayerStyle &style) {
	struct Run {
		QString text;
		EmojiPtr emoji = nullptr;
		double width = 0.;
	};
	struct Line {
		std::vector<Run> runs;
		double width = 0.;
	};
	const auto font = FontFor(style);
	const auto metrics = QFontMetricsF(font);
	const auto ascent = metrics.ascent();
	const auto glyphs = ascent + metrics.descent();
	const auto lineHeight = std::max(metrics.lineSpacing(), glyphs)
		* kTextLineHeight;
	const auto emojiSide = glyphs * kEmojiSide;
	const auto emojiAdvance = emojiSide * kEmojiAdvance;
	auto lines = std::vector<Line>();
	auto emojiCount = 0;
	for (const auto &text : style.text.split(QChar('\n'))) {
		auto line = Line();
		auto plain = QString();
		const auto flush = [&] {
			if (plain.isEmpty()) {
				return;
			}
			const auto width = metrics.horizontalAdvance(plain);
			line.runs.push_back({ plain, nullptr, width });
			line.width += width;
			plain.clear();
		};
		const auto till = text.constData() + text.size();
		for (auto ch = text.constData(); ch != till;) {
			auto length = 0;
			const auto emoji = (emojiCount < kMaxTextEmoji)
				? Ui::Emoji::Find(ch, till, &length)
				: nullptr;
			if (emoji && length > 0) {
				flush();
				line.runs.push_back({ QString(), emoji, emojiAdvance });
				line.width += emojiAdvance;
				ch += length;
				++emojiCount;
			} else {
				plain.append(*ch++);
			}
		}
		flush();
		lines.push_back(std::move(line));
	}
	auto width = metrics.horizontalAdvance(QChar(' '));
	for (const auto &line : lines) {
		width = std::max(width, line.width);
	}
	const auto padX = style.size
		* (style.plate ? kPlatePadding : kTextPadding);
	const auto padY = style.size
		* (style.plate ? kPlatePaddingTop : kTextPaddingTop);

	auto result = TextLayout();
	result.size = QSize(
		int(std::clamp(std::ceil(width + 2. * padX), 1., double(kMaxTextSide))),
		int(std::clamp(
			std::ceil(lines.size() * lineHeight + 2. * padY),
			1.,
			double(kMaxTextSide))));
	result.path.setFillRule(Qt::WindingFill);
	const auto part = AlignPart(style.align);
	// A block wider than the limit is cut: the lines are aligned inside
	// what is left of it, or the short ones of a centered or a right
	// aligned text would be placed in the part that was cut off.
	const auto inner = std::min(width, double(kMaxTextSide) - 2. * padX);
	auto top = padY;
	for (const auto &line : lines) {
		auto x = padX + (inner - line.width) * part;
		const auto baseline = top + (lineHeight - glyphs) / 2. + ascent;
		for (const auto &run : line.runs) {
			if (run.emoji) {
				result.emoji.push_back({
					QRectF(
						x + (emojiAdvance - emojiSide) / 2.,
						baseline - ascent + (glyphs - emojiSide) / 2.,
						emojiSide,
						emojiSide),
					EmojiImage(run.emoji),
				});
			} else {
				result.path.addText(QPointF(x, baseline), font, run.text);
			}
			x += run.width;
		}
		top += lineHeight;
	}
	if (TextShowsPlate(style)) {
		result.plate = QRectF(QPointF(), QSizeF(result.size));
		result.plateRadius = std::min(
			style.size * kPlateRadius,
			result.size.height() / 2.);
	}
	return result;
}

[[nodiscard]] QString TextLayerName(const QString &text) {
	const auto name = TextNameFrom(text);
	return name.isEmpty() ? tr::lng_oblivion_photo_draw_text(tr::now) : name;
}

[[nodiscard]] int TextUnits(const TextLayerStyle &style, QSize canvas) {
	return std::clamp(
		int(std::lround(style.size / DocumentUnit(canvas))),
		kMinTextUnits,
		kMaxTextUnits);
}

// What a new text looks like: the last used settings and nothing typed
// (an empty text is a small box that the text tool outlines, so nothing
// gets into the picture until the user types).
[[nodiscard]] TextLayerStyle NextTextStyle(const Document &document) {
	auto result = State().text;
	result.text = QString();
	result.size = State().textUnits * DocumentUnit(document.size);
	return result;
}

void RememberTextStyle(TextLayerStyle style, QSize canvas) {
	auto &state = State();
	state.textUnits = TextUnits(style, canvas);
	style.text = QString();
	style.size = TextLayerStyle().size;
	state.text = std::move(style);
}

[[nodiscard]] const style::InputField &TextFieldStyle() {
	static const auto result = [] {
		auto st = st::defaultInputField;
		st.textBg = st::groupCallBg;
		st.textBgActive = st::groupCallBg;
		st.textFg = st::groupCallMembersFg;
		st.textMarkBg = st::groupCallMembersBgOver;
		st.textMargins = QMargins(Px(10), Px(8), Px(10), Px(8));
		st.textAlign = style::al_topleft;
		st.placeholderFg = st::groupCallMemberNotJoinedStatus;
		st.placeholderFgActive = st::groupCallMemberNotJoinedStatus;
		st.placeholderFgError = st::groupCallMemberNotJoinedStatus;
		st.placeholderMargins = QMargins();
		st.placeholderAlign = style::al_topleft;
		st.placeholderScale = 0.;
		st.placeholderShift = 0;
		st.placeholderFont = st::normalFont;
		st.borderFg = st::groupCallMemberInactiveIcon;
		st.borderFgActive = st::groupCallActiveFg;
		st.borderFgError = st::groupCallActiveFg;
		st.border = std::max(Px(1), 1);
		st.borderActive = st.border;
		st.borderRadius = Px(8);
		st.borderDenominator = 1;
		st.style = st::defaultTextStyle;
		st.menu = st::groupCallPopupMenu;
		st.width = 0;
		st.widthMin = 0;
		st.heightMin = Px(kFieldMinHeight);
		st.heightMax = Px(kFieldMaxHeight);
		return st;
	}();
	return result;
}

// The field where the text of a layer is typed. It is the parent of the
// real field: a key that the field has not used must not reach the
// shortcuts of the editor and of the tool, see FieldKeepsKey().
class TextField final : public Ui::RpWidget {
public:
	explicit TextField(QWidget *parent);

	[[nodiscard]] not_null<Ui::InputField*> field() const;

protected:
	int resizeGetHeight(int newWidth) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	const not_null<Ui::InputField*> _field;
	bool _resizing = false;

};

TextField::TextField(QWidget *parent)
: RpWidget(parent)
, _field(Ui::CreateChild<Ui::InputField>(
		this,
		TextFieldStyle(),
		Ui::InputField::Mode::MultiLine,
		tr::lng_oblivion_photo_draw_text_field(),
		QString())) {
	_field->setSubmitSettings(Ui::InputField::SubmitSettings::None);
	_field->setMaxLength(kMaxTextLength);
	_field->heightChanges() | rpl::on_next([=] {
		// Not while the width is being changed: width() is the old one.
		if (!_resizing) {
			resizeToWidth(width());
		}
	}, _field->lifetime());
	_field->cancelled() | rpl::on_next([=] {
		FocusEditorFrom(this);
	}, _field->lifetime());
	_field->show();
}

not_null<Ui::InputField*> TextField::field() const {
	return _field;
}

int TextField::resizeGetHeight(int newWidth) {
	_resizing = true;
	_field->resizeToWidth(newWidth);
	_resizing = false;
	_field->moveToLeft(0, 0, newWidth);
	return _field->height();
}

void TextField::keyPressEvent(QKeyEvent *e) {
	if (FieldKeepsKey(e->key(), e->modifiers())) {
		e->accept();
		return;
	}
	RpWidget::keyPressEvent(e);
}

[[nodiscard]] std::vector<FxParam> TextParams() {
	const auto plate = [](const FxParams &values) {
		return values.boolean("plate");
	};
	const auto initial = TextLayerStyle();
	return {
		FxInt(
			"size",
			tr::lng_oblivion_photo_draw_size,
			kMinTextUnits,
			kMaxTextUnits,
			kDefaultTextUnits),
		FxChoice(
			"font",
			tr::lng_oblivion_photo_draw_text_font,
			{
				tr::lng_oblivion_photo_draw_text_font_default,
				tr::lng_oblivion_photo_draw_text_font_serif,
				tr::lng_oblivion_photo_draw_text_font_mono,
			},
			int(initial.font)),
		FxChoice(
			"weight",
			tr::lng_oblivion_photo_draw_text_weight,
			{
				tr::lng_oblivion_photo_draw_text_weight_regular,
				tr::lng_oblivion_photo_draw_text_weight_bold,
			},
			WeightChoice(initial.weight)),
		FxChoice(
			"align",
			tr::lng_oblivion_photo_draw_text_align,
			{
				tr::lng_oblivion_photo_draw_text_align_left,
				tr::lng_oblivion_photo_draw_text_align_center,
				tr::lng_oblivion_photo_draw_text_align_right,
			},
			int(initial.align)),
		FxBool("plate", tr::lng_oblivion_photo_draw_text_plate, false),
		FxColor(
			"plate_color",
			tr::lng_oblivion_photo_draw_text_plate_color,
			initial.plateColor).when(plate),
		FxInt(
			"plate_opacity",
			tr::lng_oblivion_photo_draw_text_plate_opacity,
			0,
			100,
			int(std::lround(initial.plateOpacity * 100.)),
			u"%"_q).when(plate),
	};
}

// The settings of a text: of the active layer if it is a text one, of
// the next text otherwise. With toolMode it is the options page of the
// text tool, without it a section of the Layer tab.
class TextPanel final : public Ui::VerticalLayout {
public:
	TextPanel(
		QWidget *parent,
		not_null<Controller*> controller,
		bool toolMode);

private:
	[[nodiscard]] const Layer *textLayer() const;
	[[nodiscard]] QSize canvas() const;
	[[nodiscard]] FxParams values(const TextLayerStyle &style) const;
	void refresh();
	void edit(Fn<void(TextLayerStyle&)> modify, bool commit);
	[[nodiscard]] bool focusField(bool selectAll);

	const not_null<Controller*> _controller;
	Ui::SlideWrap<TextField> *_fieldWrap = nullptr;
	Ui::SlideWrap<Ui::FlatLabel> *_emptyWrap = nullptr;
	Palette *_palette = nullptr;
	rpl::event_stream<FxParams> _updates;
	LayerId _layerId = 0;
	TextLayerStyle _style; // What the controls show.
	bool _settingText = false;
	bool _fieldChanging = false;

};

TextPanel::TextPanel(
	QWidget *parent,
	not_null<Controller*> controller,
	bool toolMode)
: VerticalLayout(parent)
, _controller(controller) {
	const auto layer = textLayer();
	const auto found = layer ? TextStyleOf(layer->content) : std::nullopt;
	_layerId = found ? layer->id : LayerId(0);
	_style = found ? *found : NextTextStyle(_controller->document());

	// A little air below the field (and below the hint that is shown in
	// its place): the "Color" label belongs to the swatches under it, not
	// to what is above.
	const auto topMargins = style::margins(
		Px(kPadding),
		Px(kSkip),
		Px(kPadding),
		Px(kSkip) / 2);
	_fieldWrap = add(
		object_ptr<Ui::SlideWrap<TextField>>(
			this,
			object_ptr<TextField>(this),
			topMargins));
	const auto field = _fieldWrap->entity()->field();
	if (found) {
		field->setText(_style.text);
	}
	field->changes() | rpl::on_next([=] {
		if (_settingText) {
			return;
		}
		const auto text = field->getLastText();
		if (text == _style.text) {
			return;
		} else if (_layerId && TextRefusesLines(_style.text, text)) {
			// The field goes back to the text of the layer.
			crl::on_main(this, [=] {
				refresh();
			});
		} else {
			_fieldChanging = true;
			edit([&](TextLayerStyle &style) { style.text = text; }, false);
			_fieldChanging = false;
		}
	}, field->lifetime());
	if (toolMode) {
		_emptyWrap = add(
			object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
				this,
				object_ptr<Ui::FlatLabel>(
					this,
					tr::lng_oblivion_photo_draw_hint_text(),
					HintLabelStyle()),
				topMargins));
	}
	_fieldWrap->toggle(_layerId != 0, anim::type::instant);
	if (_emptyWrap) {
		_emptyWrap->toggle(!_layerId, anim::type::instant);
	}

	_palette = AddPalette(this, controller, _style.color, [=](QColor color) {
		edit([&](TextLayerStyle &style) { style.color = color; }, true);
	});
	add(
		CreateParamsPanel(this, ParamsPanelArgs{
			.params = TextParams(),
			.values = values(_style),
			.updates = _updates.events(),
			.changed = [=](const QByteArray &id, FxValue value, bool finished) {
				edit([&](TextLayerStyle &style) {
					if (id == "size") {
						style.size = value.integer() * DocumentUnit(canvas());
					} else if (id == "font") {
						style.font = TextLayerFont(
							std::clamp(value.integer(), 0, 2));
					} else if (id == "weight") {
						// The heavy value a text already has stays as it is.
						if (WeightChoice(style.weight) != value.integer()) {
							style.weight = WeightFromChoice(value.integer());
						}
					} else if (id == "align") {
						style.align = TextLayerAlign(
							std::clamp(value.integer(), 0, 2));
					} else if (id == "plate") {
						style.plate = value.boolean();
					} else if (id == "plate_color") {
						style.plateColor = value.color();
					} else if (id == "plate_opacity") {
						style.plateOpacity = value.integer() / 100.;
					}
				}, finished);
			},
			.controller = controller.get(),
			.logarithmic = { QByteArray("size") },
		}),
		RowMargins());
	add(object_ptr<Ui::FixedHeightWidget>(this, Px(kBottomSkip)));

	rpl::merge(
		_controller->documentChanges(),
		_controller->activeLayerValue() | rpl::to_empty
	) | rpl::on_next([=] {
		refresh();
	}, lifetime());

	State().textFocus.events(
	) | rpl::filter([=](const TextFocusRequest &request) {
		return (request.controller == _controller.get());
	}) | rpl::on_next([=](const TextFocusRequest &request) {
		if (request.taken && *request.taken) {
			return;
		} else if (focusField(request.selectAll) && request.taken) {
			*request.taken = true;
		}
	}, lifetime());
}

const Layer *TextPanel::textLayer() const {
	if (!_controller->hasDocument()) {
		return nullptr;
	}
	const auto layer = _controller->activeLayer();
	return (layer && IsTextContent(layer->content)) ? layer : nullptr;
}

QSize TextPanel::canvas() const {
	return _controller->document().size;
}

FxParams TextPanel::values(const TextLayerStyle &style) const {
	auto result = FxParams();
	result.set("size", FxValue::Integer(TextUnits(style, canvas())));
	result.set("font", FxValue::Integer(int(style.font)));
	result.set("weight", FxValue::Integer(WeightChoice(style.weight)));
	result.set("align", FxValue::Integer(int(style.align)));
	result.set("plate", FxValue::Boolean(style.plate));
	result.set("plate_color", FxValue::Color(style.plateColor));
	result.set(
		"plate_opacity",
		FxValue::Integer(int(std::lround(style.plateOpacity * 100.))));
	return result;
}

void TextPanel::refresh() {
	const auto layer = textLayer();
	const auto found = layer ? TextStyleOf(layer->content) : std::nullopt;
	const auto id = found ? layer->id : LayerId(0);
	const auto style = found ? *found : NextTextStyle(_controller->document());
	const auto other = (_layerId != id);
	_layerId = id;
	_fieldWrap->toggle(id != 0, anim::type::instant);
	if (_emptyWrap) {
		_emptyWrap->toggle(!id, anim::type::instant);
	}
	if (id) {
		const auto field = _fieldWrap->entity()->field();
		if (field->getLastText() == style.text) {
			// Nothing to show.
		} else if (_fieldChanging) {
			// The field is telling about its change right now (the layer
			// could not take the text as it is): it gets the text of the
			// layer when it is done.
			crl::on_main(this, [=] {
				refresh();
			});
		} else {
			_settingText = true;
			field->setText(style.text);
			_settingText = false;
		}
	}
	if (other || !(style == _style)) {
		_style = style;
		_palette->setColor(style.color);
		_updates.fire(values(style));
	}
}

void TextPanel::edit(Fn<void(TextLayerStyle&)> modify, bool commit) {
	auto style = _style;
	modify(style);
	RememberTextStyle(style, canvas());
	const auto layer = textLayer();
	if (!layer) {
		// Nothing to change yet: these are the settings of the next text.
		_style = style;
		return;
	} else if (layer->locked) {
		_controller->showToast(tr::lng_oblivion_photo_draw_locked(tr::now));
		refresh();
		return;
	}
	const auto id = layer->id;
	_style = style;
	_controller->changeLayer(id, [&](Layer &target) {
		SetLayerText(target, style);
	}, commit);
}

bool TextPanel::focusField(bool selectAll) {
	if (!isVisible() || !_layerId || !_fieldWrap->toggled()) {
		return false;
	}
	const auto field = _fieldWrap->entity()->field();
	field->setFocusFast();
	if (selectAll) {
		field->selectAll();
	}
	return true;
}

// The focus goes to the field with the text of the active text layer on
// the page of the side panel that is open. Such a field is only on the
// Tool page (while the text tool is the current one) and on the Layer
// page: with any other page open the Tool page is opened first, or there
// would be an outlined empty text on the photo and nowhere to type it.
void FocusTextField(not_null<Controller*> controller, bool selectAll) {
	auto taken = false;
	const auto request = TextFocusRequest{
		.controller = controller.get(),
		.selectAll = selectAll,
		.taken = &taken,
	};
	State().textFocus.fire_copy(request);
	if (!taken) {
		controller->showToolOptions();
		State().textFocus.fire_copy(request);
	}
}

//
// The tools.
//

[[nodiscard]] double TransformScale(const QTransform &transform) {
	const auto determinant = std::abs(
		transform.m11() * transform.m22() - transform.m12() * transform.m21());
	return (determinant > 0. && std::isfinite(determinant))
		? std::sqrt(determinant)
		: 1.;
}

// "[" and "]" on any keyboard layout: -1, 1 or 0.
[[nodiscard]] int BracketStep(not_null<QKeyEvent*> e) {
	const auto key = e->key();
	if (key == Qt::Key_BracketLeft || key == Qt::Key_BraceLeft) {
		return -1;
	} else if (key == Qt::Key_BracketRight || key == Qt::Key_BraceRight) {
		return 1;
	} else if (key < 0x80 || key >= Qt::Key_Escape) {
		return 0;
	}
#ifdef Q_OS_MAC
	switch (e->nativeVirtualKey()) {
	case 0x21: return -1;
	case 0x1E: return 1;
	}
#elif defined Q_OS_WIN // Q_OS_MAC
	switch (e->nativeVirtualKey()) {
	case 0xDB: return -1;
	case 0xDD: return 1;
	}
#endif // Q_OS_MAC || Q_OS_WIN
	return 0;
}

[[nodiscard]] QPainterPath PolygonPath(const QPolygonF &polygon) {
	auto result = QPainterPath();
	result.addPolygon(polygon);
	result.closeSubpath();
	return result;
}

void PaintOutline(QPainter &p, const QPolygonF &polygon, bool active) {
	const auto line = style::ConvertScaleExact(1.5);
	for (const auto shadow : { true, false }) {
		auto pen = QPen(
			shadow
				? QColor(0, 0, 0, 110)
				: QColor(255, 255, 255, active ? 255 : 200),
			shadow ? (line * 2.) : line);
		if (!active && !shadow) {
			pen.setStyle(Qt::DashLine);
		}
		pen.setJoinStyle(Qt::RoundJoin);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawPolygon(polygon);
	}
}

// Pen, marker, pencil, eraser (freehand) and line, arrow, rectangle,
// ellipse (two points). The shape that is being drawn is painted over
// the canvas, the document gets it when the mouse is released: one undo
// step. After that the shape stays painted over the canvas until the
// preview with it is ready, so it never blinks.
class VectorTool final : public Tool {
public:
	VectorTool(not_null<Controller*> controller, DrawKind kind);

	void deactivated() override;
	bool mousePress(const ToolMouseEvent &e) override;
	void mouseMove(const ToolMouseEvent &e) override;
	void mouseRelease(const ToolMouseEvent &e) override;
	void mouseLeave() override;
	bool keyPress(not_null<QKeyEvent*> e) override;
	bool keyRelease(not_null<QKeyEvent*> e) override;
	bool cancel() override;
	void paint(QPainter &p, const ToolPaintContext &context) override;
	QCursor cursor(const ToolMouseEvent &e) override;

private:
	// Where the shape goes.
	struct Target {
		LayerId id = 0; // 0: a new drawing layer over the whole canvas.
		QTransform transform; // Layer -> canvas.
		QTransform inverse;
		QSize size;
		double scale = 1.; // Canvas pixels per layer pixel.
	};
	struct Pending {
		DrawShape shape;
		Target target;
		ContentPtr content; // The version of the drawing that has it.
		crl::time since = 0;
		// Controller::revision() of the document that got the shape: the
		// canvas shows it once Controller::shownRevision() reaches this.
		uint64 revision = 0;
	};

	[[nodiscard]] bool freehand() const;
	[[nodiscard]] const BrushState &brush() const;
	[[nodiscard]] double documentWidth() const;
	[[nodiscard]] std::optional<Target> resolveTarget() const;
	[[nodiscard]] std::optional<QPointF> layerPoint(QPointF document) const;
	void rebuildShape();
	void commit(DrawShape shape);
	void drop();
	void requestUpdate();
	void checkPending();
	void validatePending();
	void dropRenderedPending(crl::time age);
	void dropShownPending();
	void paintShape(
		QPainter &p,
		const ToolPaintContext &context,
		const DrawShape &shape,
		const Target &target) const;
	void paintRing(QPainter &p, const ToolPaintContext &context);

	const not_null<Controller*> _controller;
	const DrawKind _kind;

	bool _drawing = false;
	Target _target;
	std::vector<RawPoint> _raw; // Freehand.
	QPointF _from; // Figures, layer coordinates.
	QPointF _to;
	QPointF _lastWidget;
	Qt::KeyboardModifiers _modifiers;
	double _viewScale = 1.;
	uint32 _seed = 0;
	DrawShape _shape;

	std::optional<QPointF> _hover; // Widget coordinates.
	bool _ringShown = false;
	std::vector<Pending> _pending;
	base::Timer _pendingTimer;
	bool _ownUpdate = false;

	rpl::lifetime _lifetime;

};

VectorTool::VectorTool(not_null<Controller*> controller, DrawKind kind)
: _controller(controller)
, _kind(kind)
, _pendingTimer([=] { checkPending(); }) {
	State().changes.events() | rpl::on_next([=] {
		requestUpdate();
	}, _lifetime);
	_controller->documentChanges() | rpl::on_next([=] {
		validatePending();
	}, _lifetime);
	_controller->shownRevisionValue() | rpl::on_next([=] {
		dropShownPending();
	}, _lifetime);
}

bool VectorTool::freehand() const {
	return DrawKindIsFreehand(_kind);
}

const BrushState &VectorTool::brush() const {
	return State().brushes[int(_kind)];
}

double VectorTool::documentWidth() const {
	return std::max(
		brush().size * DocumentUnit(_controller->document().size),
		0.5);
}

std::optional<VectorTool::Target> VectorTool::resolveTarget() const {
	const auto &document = _controller->document();
	const auto layer = _controller->activeLayer();
	const auto drawing = layer && IsDrawContent(layer->content);
	if (drawing && layer->locked) {
		_controller->showToast(tr::lng_oblivion_photo_draw_locked(tr::now));
		return std::nullopt;
	} else if (drawing && layer->visible) {
		auto invertible = false;
		const auto inverse = layer->transform.inverted(&invertible);
		if (invertible) {
			return Target{
				.id = layer->id,
				.transform = layer->transform,
				.inverse = inverse,
				.size = layer->size(),
				.scale = TransformScale(layer->transform),
			};
		}
	}
	if (_kind == DrawKind::Eraser) {
		_controller->showToast(
			tr::lng_oblivion_photo_draw_eraser_no_layer(tr::now));
		return std::nullopt;
	}
	return Target{ .size = document.size };
}

std::optional<QPointF> VectorTool::layerPoint(QPointF document) const {
	const auto result = _target.inverse.map(document);
	if (!Finite(result)) {
		return std::nullopt;
	} else if (!_target.transform.isAffine()) {
		// Not what is behind the horizon of a perspective.
		const auto &t = _target.transform;
		const auto w = t.m13() * result.x() + t.m23() * result.y() + t.m33();
		if (!(w > 1e-9)) {
			return std::nullopt;
		}
	}
	return result;
}

void VectorTool::requestUpdate() {
	_ownUpdate = true;
	_controller->updateCanvas();
}

void VectorTool::deactivated() {
	drop();
	_pending.clear();
	_pendingTimer.cancel();
}

void VectorTool::drop() {
	_drawing = false;
	_raw.clear();
	_shape = DrawShape();
}

bool VectorTool::mousePress(const ToolMouseEvent &e) {
	if (!_controller->hasDocument() || _controller->busy()) {
		return false;
	}
	_hover = e.widget;
	const auto target = resolveTarget();
	if (!target) {
		// Taken anyway: a brush must not start dragging the photo.
		return true;
	}
	_target = *target;
	auto point = layerPoint(e.document);
	const auto canvas = _controller->document().size;
	if (_target.id
		&& (_kind != DrawKind::Eraser)
		&& QRectF(QPointF(), QSizeF(canvas)).contains(e.document)
		&& (!point
			|| !QRectF(QPointF(), QSizeF(_target.size)).contains(*point))) {
		// The stroke starts on the canvas, but outside of the active
		// drawing (it was moved or made smaller, or the canvas was
		// extended after it): there it would not be seen, so it goes to
		// a new drawing layer over the whole canvas.
		_target = Target{ .size = canvas };
		point = layerPoint(e.document);
	}
	if (!point) {
		return true;
	}
	_drawing = true;
	_viewScale = (std::isfinite(e.scale) && e.scale > 0.) ? e.scale : 1.;
	_modifiers = e.modifiers;
	_lastWidget = e.widget;
	_seed = uint32(crl::now()) * 2654435761U + uint32(_pending.size());
	_raw.clear();
	_raw.push_back({ *point, double(crl::now()) });
	_from = _to = *point;
	rebuildShape();
	requestUpdate();
	return true;
}

void VectorTool::mouseMove(const ToolMouseEvent &e) {
	_hover = e.widget;
	if (!_drawing) {
		// Only the ring of the brush follows the mouse.
		if (freehand()
			&& (_ringShown
				|| documentWidth() * e.scale / 2. >= kRingMinRadius)) {
			requestUpdate();
		}
		return;
	}
	_modifiers = e.modifiers;
	if (const auto point = layerPoint(e.document)) {
		if (!freehand()) {
			_to = *point;
		} else if (Distance(e.widget, _lastWidget) >= kMinMoveScreen
			&& int(_raw.size()) < kMaxRawPoints) {
			_raw.push_back({ *point, double(crl::now()) });
			_lastWidget = e.widget;
		}
		rebuildShape();
	}
	requestUpdate();
}

void VectorTool::mouseRelease(const ToolMouseEvent &e) {
	_hover = e.widget;
	if (!_drawing) {
		return;
	}
	_modifiers = e.modifiers;
	if (const auto point = layerPoint(e.document)) {
		if (!freehand()) {
			_to = *point;
		} else if (Distance(e.widget, _lastWidget) > 0.
			&& int(_raw.size()) < kMaxRawPoints) {
			_raw.push_back({ *point, double(crl::now()) });
		}
	}
	rebuildShape();
	auto shape = base::take(_shape);
	drop();
	if (!freehand() && shape.points.size() > 1) {
		// A click without a drag draws nothing.
		const auto length = Distance(
			PointOf(shape.points.front()),
			PointOf(shape.points.back()));
		if (length * _target.scale * _viewScale < kMinFigureScreen) {
			shape.points.clear();
		}
	}
	commit(std::move(shape));
	requestUpdate();
}

void VectorTool::mouseLeave() {
	if (_hover) {
		_hover = std::nullopt;
		requestUpdate();
	}
}

bool VectorTool::keyPress(not_null<QKeyEvent*> e) {
	const auto key = e->key();
	if (key == Qt::Key_Space) {
		// The canvas is about to be dragged with the hand cursor.
		if (!_drawing) {
			mouseLeave();
		}
		return false;
	} else if (key == Qt::Key_Shift || key == Qt::Key_Alt) {
		if (_drawing && !freehand()) {
			_modifiers |= (key == Qt::Key_Shift)
				? Qt::ShiftModifier
				: Qt::AltModifier;
			rebuildShape();
			requestUpdate();
		}
		return false;
	}
	const auto commands = Qt::ControlModifier
		| Qt::AltModifier
		| Qt::MetaModifier;
	if (!freehand() || (e->modifiers() & commands)) {
		return false;
	}
	const auto step = BracketStep(e);
	if (!step) {
		return false;
	}
	auto &size = State().brushes[int(_kind)].size;
	const auto delta = (size < 10) ? 1 : (size < 40) ? 2 : 5;
	size = std::clamp(size + step * delta, kMinBrushSize, kMaxBrushSize);
	State().changes.fire({});
	return true;
}

bool VectorTool::keyRelease(not_null<QKeyEvent*> e) {
	const auto key = e->key();
	if ((key == Qt::Key_Shift || key == Qt::Key_Alt)
		&& _drawing
		&& !freehand()) {
		_modifiers &= ~Qt::KeyboardModifiers((key == Qt::Key_Shift)
			? Qt::ShiftModifier
			: Qt::AltModifier);
		rebuildShape();
		requestUpdate();
	}
	return false;
}

bool VectorTool::cancel() {
	if (!_drawing) {
		return false;
	}
	drop();
	requestUpdate();
	return true;
}

void VectorTool::rebuildShape() {
	const auto &settings = brush();
	auto shape = DrawShape();
	shape.kind = _kind;
	shape.color = (_kind == DrawKind::Eraser)
		? QColor(0, 0, 0)
		: State().color;
	shape.width = std::max(documentWidth() / _target.scale, kMinWidth);
	shape.opacity = settings.opacity / 100.;
	shape.seed = _seed;
	if (freehand()) {
		shape.points = BuildStroke(_raw, {
			.smoothing = settings.smoothing / 100.,
			.unit = 1. / (_viewScale * _target.scale),
			.dynamic = (_kind == DrawKind::Pen) && settings.dynamic,
		});
	} else {
		auto from = _from;
		auto to = _to;
		const auto box = (_kind == DrawKind::Rectangle)
			|| (_kind == DrawKind::Ellipse);
		if (_modifiers & Qt::ShiftModifier) {
			to = box
				? ConstrainedBoxEnd(from, to)
				: ConstrainedLineEnd(from, to);
		}
		if (box && (_modifiers & Qt::AltModifier)) {
			// From the center.
			from = from * 2. - to;
		}
		shape.filled = box && settings.filled;
		shape.points = { MakePoint(from), MakePoint(to) };
	}
	_shape = std::move(shape);
}

void VectorTool::commit(DrawShape shape) {
	if (!_controller->hasDocument() || !ShapeTouches(shape, _target.size)) {
		// Nothing of it would be seen: no layer and no undo step for it.
		return;
	}
	const auto canvas = _controller->document().size;
	auto content = ContentPtr();
	if (_target.id) {
		const auto layer = _controller->document().find(_target.id);
		if (!layer || layer->locked || !IsDrawContent(layer->content)) {
			return;
		}
		content = DrawWithShape(layer->content, shape);
		if (!content && DrawShapeCount(layer->content) > 0) {
			_controller->showToast(
				tr::lng_oblivion_photo_draw_too_many(tr::now));
		}
	} else {
		content = MakeDrawContent(canvas, { shape });
	}
	if (!content) {
		return;
	}
	if (const auto drawing = AsDraw(content)) {
		// The preview of the canvas is at least this large, the thumbnails
		// of the layers are much smaller.
		drawing->watchScale(kPendingWatchPart
			* _target.scale
			* ScaleForSide(canvas, kPendingWatchSide));
	}
	const auto color = shape.color;
	// Before the document changes: validatePending() runs from there.
	_pending.push_back({ std::move(shape), _target, content, crl::now() });
	if (int(_pending.size()) > kMaxPending) {
		_pending.erase(begin(_pending));
	}
	if (_target.id) {
		_controller->changeLayer(_target.id, [&](Layer &layer) {
			layer.content = content;
		});
	} else {
		const auto name = UniqueLayerName(
			_controller->document(),
			tr::lng_oblivion_photo_draw_layer(tr::now));
		_controller->addLayer(MakeLayer(content, name));
	}
	validatePending();
	if (!_pending.empty() && _pending.back().content == content) {
		_pending.back().revision = _controller->revision();
	}
	if (!_pending.empty() && !_pendingTimer.isActive()) {
		_pendingTimer.callEach(kPendingCheck);
	}
	if (_kind != DrawKind::Eraser) {
		RememberColor(color);
	}
}

// Undo and anything else that takes a stroke out of the document takes
// it off the canvas too: only the strokes that lead to what a layer has
// now stay.
void VectorTool::validatePending() {
	if (_pending.empty()) {
		return;
	}
	const auto &layers = _controller->document().layers;
	auto keep = 0;
	for (auto i = int(_pending.size()); i != 0; --i) {
		if (ranges::contains(layers, _pending[i - 1].content, &Layer::content)) {
			keep = i;
			break;
		}
	}
	if (keep < int(_pending.size())) {
		_pending.erase(begin(_pending) + keep, end(_pending));
	}
	if (_pending.empty()) {
		_pendingTimer.cancel();
	}
}

// A rendered version of the drawing has all the strokes before it.
void VectorTool::dropRenderedPending(crl::time age) {
	const auto now = crl::now();
	for (auto i = int(_pending.size()); i != 0; --i) {
		const auto drawing = AsDraw(_pending[i - 1].content);
		const auto rendered = drawing ? drawing->renderedAt() : crl::time(1);
		if (rendered && (now - rendered >= age)) {
			_pending.erase(begin(_pending), begin(_pending) + i);
			break;
		}
	}
	if (_pending.empty()) {
		_pendingTimer.cancel();
	}
}

// Exact: the canvas tells which revision of the document its picture
// was rendered from. The checks by time below are only for a controller
// without a canvas and for a preview that never came.
void VectorTool::dropShownPending() {
	const auto shown = _controller->shownRevision();
	if (!shown || _pending.empty()) {
		return;
	}
	const auto before = _pending.size();
	_pending.erase(
		ranges::remove_if(_pending, [&](const Pending &pending) {
			return pending.revision && (pending.revision <= shown);
		}),
		end(_pending));
	if (_pending.empty()) {
		_pendingTimer.cancel();
	}
	if (_pending.size() != before) {
		requestUpdate();
	}
}

void VectorTool::checkPending() {
	const auto before = _pending.size();
	if (!_controller->shownRevision()) {
		dropRenderedPending(kPendingAfterRender);
	}
	const auto now = crl::now();
	_pending.erase(
		ranges::remove_if(_pending, [&](const Pending &pending) {
			return (now - pending.since >= kPendingTimeout);
		}),
		end(_pending));
	if (_pending.empty()) {
		_pendingTimer.cancel();
	}
	if (_pending.size() != before) {
		requestUpdate();
	}
}

void VectorTool::paintShape(
		QPainter &p,
		const ToolPaintContext &context,
		const DrawShape &shape,
		const Target &target) const {
	if (shape.points.empty()) {
		return;
	}
	// Only what the picture will show: inside the canvas, inside what the
	// crop of the whole picture leaves and, for a layer that was moved or
	// resized, inside the layer.
	const auto &document = _controller->document();
	const auto mapped = [&](const QPolygonF &canvasPolygon) {
		return PolygonPath(context.documentToWidget.map(canvasPolygon));
	};
	const auto box = [](QSize size) {
		return QPolygonF(QRectF(QPointF(), QSizeF(size)));
	};
	auto clip = mapped(box(document.size));
	const auto output = OutputSize(document);
	auto invertible = false;
	const auto fromOutput = OutputTransform(document, output).inverted(
		&invertible);
	if (invertible && !output.isEmpty()) {
		clip = clip.intersected(mapped(fromOutput.map(box(output))));
	}
	if (target.id) {
		clip = clip.intersected(mapped(target.transform.map(box(target.size))));
	}
	p.save();
	p.setClipPath(clip, Qt::IntersectClip);
	PaintShape(
		p,
		shape,
		target.transform * context.documentToWidget,
		PaintTarget::Overlay);
	p.restore();
}

void VectorTool::paintRing(
		QPainter &p,
		const ToolPaintContext &context) {
	_ringShown = false;
	if (!freehand() || !_hover) {
		return;
	}
	const auto radius = documentWidth() * context.scale / 2.;
	if (radius < kRingMinRadius) {
		return;
	}
	_ringShown = true;
	const auto line = style::ConvertScaleExact(1.);
	p.setBrush(Qt::NoBrush);
	p.setPen(QPen(QColor(0, 0, 0, 120), line));
	p.drawEllipse(*_hover, radius + line, radius + line);
	p.setPen(QPen(QColor(255, 255, 255, 235), line));
	p.drawEllipse(*_hover, radius, radius);
}

void VectorTool::paint(QPainter &p, const ToolPaintContext &context) {
	// Without a canvas that reports what it shows a repaint that was not
	// asked for from here is most likely a new preview: what it already
	// has is not painted over it. With one dropShownPending() knows.
	if (!base::take(_ownUpdate) && !_controller->shownRevision()) {
		dropRenderedPending(0);
	}
	for (const auto &pending : _pending) {
		paintShape(p, context, pending.shape, pending.target);
	}
	if (_drawing) {
		paintShape(p, context, _shape, _target);
	}
	paintRing(p, context);
}

QCursor VectorTool::cursor(const ToolMouseEvent &e) {
	_hover = e.widget;
	if (!freehand()) {
		return QCursor(Qt::CrossCursor);
	}
	const auto radius = documentWidth() * e.scale / 2.;
	return QCursor((radius >= kRingMinRadius)
		? Qt::BlankCursor
		: Qt::CrossCursor);
}

[[nodiscard]] bool IsVectorTool(const QByteArray &id) {
	return id.startsWith("draw.") && (id != kDrawTextTool);
}

// A text layer that can be changed and has nothing typed in it.
[[nodiscard]] bool EmptyText(const Layer *layer) {
	if (!layer || layer->locked) {
		return false;
	}
	const auto style = TextStyleOf(layer->content);
	return style && BlankText(style->text);
}

// Brings a text that was added and not typed yet to another place: the
// same point of the block comes there as when a text is added, see
// MakeTextLayer().
void MoveEmptyText(
		not_null<Controller*> controller,
		const Layer &layer,
		QPointF canvasPoint) {
	const auto style = TextStyleOf(layer.content);
	const auto size = layer.size();
	const auto anchor = layer.transform.map(QPointF(
		AlignPart(style ? style->align : TextLayerAlign::Left) * size.width(),
		size.height() / 2.));
	const auto delta = canvasPoint
		- (Finite(anchor) ? anchor : LayerBounds(layer).center());
	controller->changeLayer(layer.id, [&](Layer &target) {
		target.transform = target.transform
			* QTransform::fromTranslate(delta.x(), delta.y());
	});
}

// A click on the photo adds a text layer there, a click on a text makes
// it the active one, a drag moves it. The text itself is typed in the
// field of the options (or of the Layer tab).
class TextTool final : public Tool {
public:
	explicit TextTool(not_null<Controller*> controller);

	void deactivated() override;
	bool mousePress(const ToolMouseEvent &e) override;
	void mouseMove(const ToolMouseEvent &e) override;
	void mouseRelease(const ToolMouseEvent &e) override;
	bool mouseDoubleClick(const ToolMouseEvent &e) override;
	void mouseLeave() override;
	bool keyPress(not_null<QKeyEvent*> e) override;
	bool cancel() override;
	void paint(QPainter &p, const ToolPaintContext &context) override;
	QCursor cursor(const ToolMouseEvent &e) override;

private:
	[[nodiscard]] LayerId textAt(QPointF document) const;
	void cleanup(LayerId except);
	void setHover(LayerId id);
	void requestFocus(bool selectAll);

	const not_null<Controller*> _controller;
	bool _pressed = false;
	bool _created = false;
	LayerId _dragLayer = 0;
	bool _dragMoved = false;
	QPointF _dragStart; // Canvas coordinates.
	QPointF _dragStartWidget;
	QTransform _dragTransform;
	LayerId _hover = 0;

};

TextTool::TextTool(not_null<Controller*> controller)
: _controller(controller) {
}

void TextTool::deactivated() {
	_pressed = false;
	_dragLayer = 0;
	cleanup(0);
	_controller->commit();
}

// A text that was added and never typed (or had everything erased) does
// not stay in the picture when the user goes on to something else.
void TextTool::cleanup(LayerId except) {
	const auto active = _controller->activeLayer();
	const auto &document = _controller->document();
	if (!active
		|| active->id == except
		|| !EmptyText(active)
		|| document.layers.size() < 2) {
		return;
	}
	// A new layer is added above the active one: the layer below the
	// text is the one the user was working with before the click.
	const auto id = active->id;
	const auto index = document.indexOf(id);
	const auto below = (index > 0)
		? document.layers[index - 1].id
		: LayerId(0);
	_controller->removeLayer(id);
	if (below && !_controller->document().find(id)) {
		_controller->setActiveLayer(below);
	}
}

LayerId TextTool::textAt(QPointF document) const {
	const auto &layers = _controller->document().layers;
	for (auto i = int(layers.size()); i != 0; --i) {
		const auto &layer = layers[i - 1];
		if (!layer.visible || !IsTextContent(layer.content)) {
			continue;
		}
		const auto point = LayerPoint(layer, document);
		if (point
			&& QRectF(QPointF(), QSizeF(layer.size())).contains(*point)) {
			return layer.id;
		}
	}
	return 0;
}

void TextTool::setHover(LayerId id) {
	if (_hover != id) {
		_hover = id;
		_controller->updateCanvas();
	}
}

void TextTool::requestFocus(bool selectAll) {
	FocusTextField(_controller, selectAll);
}

bool TextTool::mousePress(const ToolMouseEvent &e) {
	if (!_controller->hasDocument() || _controller->busy()) {
		return false;
	}
	_pressed = true;
	_created = false;
	_dragMoved = false;
	_dragLayer = 0;
	if (const auto hit = textAt(e.document)) {
		cleanup(hit);
		_controller->setActiveLayer(hit);
		const auto layer = _controller->document().find(hit);
		if (layer && !layer->locked) {
			_dragLayer = hit;
			_dragStart = e.document;
			_dragStartWidget = e.widget;
			_dragTransform = layer->transform;
		} else if (layer) {
			_controller->showToast(
				tr::lng_oblivion_photo_draw_locked(tr::now));
		}
		_controller->updateCanvas();
		return true;
	}
	const auto canvas = QRectF(
		QPointF(),
		QSizeF(_controller->document().size));
	if (!canvas.contains(e.document)) {
		// The margin around the photo: a text there would not be seen.
		// Taken anyway, like a brush does: the text tool does not drag
		// the photo.
		return true;
	}
	// A text that was just added and not typed yet goes to the new place
	// instead of leaving one more empty layer behind. Not a hidden one:
	// what is typed must be seen.
	const auto active = _controller->activeLayer();
	if (EmptyText(active) && active->visible) {
		MoveEmptyText(_controller, *active, e.document);
		_created = true;
		return true;
	}
	const auto added = _controller->addLayer(MakeTextLayer(
		NextTextStyle(_controller->document()),
		e.document));
	_created = (added != 0);
	return true;
}

void TextTool::mouseMove(const ToolMouseEvent &e) {
	if (!_pressed) {
		setHover(textAt(e.document));
		return;
	} else if (!_dragLayer) {
		return;
	} else if (!_dragMoved
		&& Distance(e.widget, _dragStartWidget) < kDragThreshold) {
		return;
	}
	_dragMoved = true;
	const auto delta = e.document - _dragStart;
	_controller->changeLayer(_dragLayer, [&](Layer &layer) {
		layer.transform = _dragTransform
			* QTransform::fromTranslate(delta.x(), delta.y());
	}, false);
}

void TextTool::mouseRelease(const ToolMouseEvent &e) {
	if (!_pressed) {
		return;
	}
	_pressed = false;
	if (base::take(_dragLayer)) {
		if (_dragMoved) {
			_controller->commit();
		} else {
			requestFocus(false);
		}
	} else if (_created) {
		requestFocus(true);
	}
	setHover(textAt(e.document));
}

bool TextTool::mouseDoubleClick(const ToolMouseEvent &e) {
	const auto hit = _controller->hasDocument()
		? textAt(e.document)
		: LayerId(0);
	if (!hit) {
		return false;
	}
	_controller->setActiveLayer(hit);
	requestFocus(true);
	return true;
}

void TextTool::mouseLeave() {
	setHover(0);
}

bool TextTool::keyPress(not_null<QKeyEvent*> e) {
	const auto key = e->key();
	const auto layer = _controller->activeLayer();
	if ((key == Qt::Key_Return || key == Qt::Key_Enter)
		&& !(e->modifiers() & ~Qt::KeypadModifier)
		&& layer
		&& IsTextContent(layer->content)) {
		requestFocus(false);
		return true;
	}
	return false;
}

bool TextTool::cancel() {
	if (!_pressed || !_dragLayer || !_dragMoved) {
		return false;
	}
	const auto id = base::take(_dragLayer);
	_controller->changeLayer(id, [&](Layer &layer) {
		layer.transform = _dragTransform;
	}, false);
	_dragMoved = false;
	return true;
}

void TextTool::paint(QPainter &p, const ToolPaintContext &context) {
	const auto &document = _controller->document();
	const auto outline = [&](const Layer *layer, bool active) {
		if (layer && layer->visible && IsTextContent(layer->content)) {
			PaintOutline(
				p,
				context.documentToWidget.map(LayerQuad(*layer)),
				active);
		}
	};
	const auto active = _controller->activeLayer();
	if (_hover && (!active || active->id != _hover)) {
		outline(document.find(_hover), false);
	}
	outline(active, true);
}

QCursor TextTool::cursor(const ToolMouseEvent &e) {
	return QCursor((_dragLayer || textAt(e.document))
		? Qt::SizeAllCursor
		: Qt::IBeamCursor);
}

const auto Registered = EditorRegistrar([] {
	struct Brush {
		QByteArray id;
		tr::phrase<> name;
		int key = 0;
		DrawKind kind = DrawKind::Pen;
		void (*icon)(QPainter &p, QRectF rect, QColor color) = nullptr;
	};
	const auto brushes = std::vector<Brush>{
		{
			kDrawPenTool,
			tr::lng_oblivion_photo_draw_pen,
			Qt::Key_B,
			DrawKind::Pen,
			&PaintPenIcon,
		},
		{
			kDrawMarkerTool,
			tr::lng_oblivion_photo_draw_marker,
			Qt::Key_H, // M is the mask tool, H as in "highlighter".
			DrawKind::Marker,
			&PaintMarkerIcon,
		},
		{
			kDrawPencilTool,
			tr::lng_oblivion_photo_draw_pencil,
			Qt::Key_N,
			DrawKind::Pencil,
			&PaintPencilIcon,
		},
		{
			kDrawEraserTool,
			tr::lng_oblivion_photo_draw_eraser,
			Qt::Key_E,
			DrawKind::Eraser,
			&PaintEraserIcon,
		},
		{
			kDrawLineTool,
			tr::lng_oblivion_photo_draw_line,
			Qt::Key_L,
			DrawKind::Line,
			&PaintLineIcon,
		},
		{
			kDrawArrowTool,
			tr::lng_oblivion_photo_draw_arrow,
			Qt::Key_A,
			DrawKind::Arrow,
			&PaintArrowIcon,
		},
		{
			kDrawRectTool,
			tr::lng_oblivion_photo_draw_rect,
			Qt::Key_R,
			DrawKind::Rectangle,
			&PaintRectIcon,
		},
		{
			kDrawEllipseTool,
			tr::lng_oblivion_photo_draw_ellipse,
			Qt::Key_O,
			DrawKind::Ellipse,
			&PaintEllipseIcon,
		},
	};
	auto order = 20;
	for (const auto &brush : brushes) {
		const auto kind = brush.kind;
		RegisterTool({
			.id = brush.id,
			.name = brush.name,
			.key = brush.key,
			.order = order++,
			.paintIcon = brush.icon,
			.create = [=](not_null<Controller*> controller) {
				return std::unique_ptr<Tool>(
					std::make_unique<VectorTool>(controller, kind));
			},
			.options = [=](
					not_null<QWidget*> parent,
					not_null<Controller*> controller) {
				return CreateBrushOptions(parent, controller, kind);
			},
		});
	}
	RegisterTool({
		.id = kDrawTextTool,
		.name = tr::lng_oblivion_photo_draw_text,
		.key = Qt::Key_T,
		.order = order++,
		.paintIcon = &PaintTextIcon,
		.create = [](not_null<Controller*> controller) {
			return std::unique_ptr<Tool>(
				std::make_unique<TextTool>(controller));
		},
		.options = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			return object_ptr<Ui::RpWidget>(
				object_ptr<TextPanel>(parent, controller, true));
		},
	});

	RegisterLayerKind({
		.type = kDrawLayerType,
		.name = tr::lng_oblivion_photo_draw_layer,
		.order = 10,
		.paintIcon = &PaintDrawingIcon,
		.create = [](not_null<Controller*> controller) {
			if (!controller->hasDocument()) {
				return;
			}
			const auto &document = controller->document();
			const auto name = UniqueLayerName(
				document,
				tr::lng_oblivion_photo_draw_layer(tr::now));
			const auto added = controller->addLayer(
				MakeLayer(MakeDrawContent(document.size), name));
			if (added && !IsVectorTool(controller->toolId())) {
				controller->setTool(kDrawPenTool);
			}
		},
	});
	RegisterLayerKind({
		.type = kTextLayerType,
		.name = tr::lng_oblivion_photo_draw_text,
		.order = 20,
		.paintIcon = &PaintTextIcon,
		.create = [](not_null<Controller*> controller) {
			if (!controller->hasDocument()) {
				return;
			}
			const auto &document = controller->document();
			const auto center = QPointF(
				document.size.width() / 2.,
				document.size.height() / 2.);
			const auto active = controller->activeLayer();
			if (EmptyText(active) && active->visible) {
				// A text that was added and not typed yet is the new
				// text: one more empty layer would only be left behind.
				const auto delta = center - LayerBounds(*active).center();
				controller->changeLayer(active->id, [&](Layer &target) {
					target.transform = target.transform
						* QTransform::fromTranslate(delta.x(), delta.y());
				});
			} else {
				auto layer = MakeTextLayer(NextTextStyle(document), center);
				const auto size = layer.size();
				layer.transform = QTransform::fromTranslate(
					center.x() - size.width() / 2.,
					center.y() - size.height() / 2.);
				if (!controller->addLayer(std::move(layer))) {
					return;
				}
			}
			// Choosing the tool that is current already would not show
			// its page with the field: FocusTextField() does then.
			controller->setTool(kDrawTextTool);
			FocusTextField(controller, true);
		},
	});

	RegisterPanel({
		.id = "draw.text",
		.slot = PanelSlot::LayerProperties,
		.order = 10,
		.title = tr::lng_oblivion_photo_draw_text,
		.visible = [](not_null<const Controller*> controller) {
			const auto layer = controller->activeLayer();
			return layer && IsTextContent(layer->content);
		},
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			return object_ptr<Ui::RpWidget>(
				object_ptr<TextPanel>(parent, controller, false));
		},
	});
});

//
// UI snapshot scenes.
//

// The same "recent colors" in every scene, whatever ran before it.
void SceneRecentColors() {
	RememberColor(QColor(0x00, 0xC2, 0xA8));
	RememberColor(QColor(0xFF, 0x6F, 0x91));
	RememberColor(QColor(0x7B, 0x61, 0xFF));
}

// The sample photo alone, named as the editor names it.
[[nodiscard]] Document ScenePhoto() {
	SceneRecentColors();
	auto document = SampleSceneDocument();
	while (document.layers.size() > 1) {
		RemoveLayer(document, document.layers.back().id);
	}
	return document;
}

[[nodiscard]] Document SceneDrawing(bool figures) {
	auto document = ScenePhoto();
	if (document.empty()) {
		return document;
	}
	AddLayer(document, MakeLayer(
		MakeDrawContent(
			document.size,
			figures
				? SampleFigures(document.size)
				: SampleFreehand(document.size)),
		tr::lng_oblivion_photo_draw_layer(tr::now)));
	return document;
}

[[nodiscard]] Document SceneText() {
	auto document = ScenePhoto();
	if (document.empty()) {
		return document;
	}
	const auto unit = DocumentUnit(document.size);
	const auto width = document.size.width();
	const auto height = document.size.height();

	auto note = TextLayerStyle();
	note.text = QString::fromUtf8(
		"\xD0\x9B\xD0\xB5\xD1\x82\xD0\xBE, "
		"\xD0\xB4\xD0\xB5\xD0\xBD\xD1\x8C "
		"\xD0\xBF\xD0\xB5\xD1\x80\xD0\xB2\xD1\x8B\xD0\xB9 "
		"\xE2\x98\x80\xEF\xB8\x8F");
	note.size = 52. * unit;
	note.weight = TextLayerWeight::Regular;
	note.font = TextLayerFont::Serif;
	note.color = QColor(0xFD, 0xD8, 0x35);
	// Over the sky of the sample photo: yellow letters without a plate
	// can't be read over its pavement and boats.
	AddLayer(
		document,
		MakeTextLayer(note, QPointF(width * 0.2, height * 0.075)));

	auto title = TextLayerStyle();
	title.text = QString::fromUtf8(
		"\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 "
		"\xD0\xB8\xD0\xB7 Oblivion!\n"
		"\xD0\xA2\xD0\xB5\xD0\xBA\xD1\x81\xD1\x82 "
		"\xD0\xBD\xD0\xB0 \xD1\x84\xD0\xBE\xD1\x82\xD0\xBE");
	title.size = 84. * unit;
	title.weight = TextLayerWeight::Bold;
	title.align = TextLayerAlign::Center;
	title.plate = true;
	title.plateColor = QColor(0, 0, 0);
	title.plateOpacity = 0.55;
	AddLayer(
		document,
		MakeTextLayer(title, QPointF(width * 0.5, height * 0.38)));
	return document;
}

// What a click with the text tool leaves: a text with nothing typed yet,
// only the outline of the tool shows where it is.
[[nodiscard]] Document SceneNewText() {
	auto document = ScenePhoto();
	if (document.empty()) {
		return document;
	}
	AddLayer(
		document,
		MakeTextLayer(
			NextTextStyle(document),
			QPointF(
				document.size.width() * 0.5,
				document.size.height() * 0.4)));
	return document;
}

[[nodiscard]] object_ptr<Ui::RpWidget> SceneOptions(
		not_null<QWidget*> parent,
		rpl::producer<QString> title,
		Fn<object_ptr<Ui::RpWidget>(not_null<QWidget*>)> create) {
	SceneRecentColors();
	auto result = object_ptr<Ui::VerticalLayout>(parent);
	result->add(
		object_ptr<SectionTitle>(result.data(), std::move(title)),
		RowMargins());
	result->add(create(result.data()));
	result->add(object_ptr<Ui::FixedHeightWidget>(
		result.data(),
		Px(kPadding)));
	return object_ptr<Ui::RpWidget>(std::move(result));
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	RegisterEditorScene({
		.name = u"photo_draw_pen"_q,
		.document = [] { return SceneDrawing(false); },
		.tab = PhotoEditorTab::Tool,
		.tool = kDrawPenTool,
	});
	RegisterEditorScene({
		.name = u"photo_draw_arrow"_q,
		.document = [] { return SceneDrawing(true); },
		.tab = PhotoEditorTab::Tool,
		.tool = kDrawArrowTool,
	});
	RegisterEditorScene({
		.name = u"photo_draw_text"_q,
		.document = SceneText,
		.tab = PhotoEditorTab::Tool,
		.tool = kDrawTextTool,
	});
	RegisterEditorScene({
		.name = u"photo_draw_text_new"_q,
		.document = SceneNewText,
		.tab = PhotoEditorTab::Tool,
		.tool = kDrawTextTool,
	});
	RegisterEditorScene({
		.name = u"photo_draw_text_layer"_q,
		.document = SceneText,
		.tab = PhotoEditorTab::Layer,
	});

	const auto width = Px(340);
	RegisterPanelScene({
		.name = u"photo_draw_options_marker"_q,
		.size = QSize(width, 0),
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			return SceneOptions(
				parent,
				tr::lng_oblivion_photo_draw_marker(),
				[=](not_null<QWidget*> inner) {
					return CreateBrushOptions(
						inner,
						controller,
						DrawKind::Marker);
				});
		},
	});
	RegisterPanelScene({
		.name = u"photo_draw_options_pen"_q,
		.size = QSize(width, 0),
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			return SceneOptions(
				parent,
				tr::lng_oblivion_photo_draw_pen(),
				[=](not_null<QWidget*> inner) {
					return CreateBrushOptions(inner, controller, DrawKind::Pen);
				});
		},
	});
	RegisterPanelScene({
		.name = u"photo_draw_options_rect"_q,
		.size = QSize(width, 0),
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			return SceneOptions(
				parent,
				tr::lng_oblivion_photo_draw_rect(),
				[=](not_null<QWidget*> inner) {
					return CreateBrushOptions(
						inner,
						controller,
						DrawKind::Rectangle);
				});
		},
	});
	RegisterPanelScene({
		.name = u"photo_draw_options_eraser"_q,
		.size = QSize(width, 0),
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			return SceneOptions(
				parent,
				tr::lng_oblivion_photo_draw_eraser(),
				[=](not_null<QWidget*> inner) {
					return CreateBrushOptions(
						inner,
						controller,
						DrawKind::Eraser);
				});
		},
	});
	RegisterPanelScene({
		.name = u"photo_draw_options_ellipse"_q,
		.size = QSize(width, 0),
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			// A filled figure in a color that is neither a preset nor
			// a recent one: the ring is on the "any color" button and
			// the color is shown inside it, the switch is on. All the
			// scenes share the settings of the tools, so they are put
			// back as soon as the panel has read them.
			auto &state = State();
			auto &brush = state.brushes[int(DrawKind::Ellipse)];
			const auto wasColor = state.color;
			const auto wasBrush = brush;
			state.color = QColor(0x26, 0xC6, 0xDA);
			brush.size = 14;
			brush.opacity = 70;
			brush.filled = true;
			auto result = SceneOptions(
				parent,
				tr::lng_oblivion_photo_draw_ellipse(),
				[=](not_null<QWidget*> inner) {
					return CreateBrushOptions(
						inner,
						controller,
						DrawKind::Ellipse);
				});
			state.color = wasColor;
			brush = wasBrush;
			return result;
		},
	});
	RegisterPanelScene({
		.name = u"photo_draw_options_text"_q,
		.size = QSize(width, 0),
		.document = SceneText,
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			return SceneOptions(
				parent,
				tr::lng_oblivion_photo_draw_text(),
				[=](not_null<QWidget*> inner) {
					return object_ptr<Ui::RpWidget>(
						object_ptr<TextPanel>(inner, controller, true));
				});
		},
	});
	RegisterPanelScene({
		.name = u"photo_draw_options_text_empty"_q,
		.size = QSize(width, 0),
		.document = ScenePhoto,
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			return SceneOptions(
				parent,
				tr::lng_oblivion_photo_draw_text(),
				[=](not_null<QWidget*> inner) {
					return object_ptr<Ui::RpWidget>(
						object_ptr<TextPanel>(inner, controller, true));
				});
		},
	});
});

} // namespace

ContentPtr MakeTextContent(const TextLayerStyle &style) {
	auto normalized = NormalizedText(style);
	auto layout = LayoutText(normalized);
	return std::make_shared<const TextContent>(
		std::move(normalized),
		std::move(layout));
}

Layer MakeTextLayer(const TextLayerStyle &style, QPointF canvasPoint) {
	auto content = MakeTextContent(style);
	const auto size = content->size();
	const auto part = AlignPart(NormalizedText(style).align);
	auto result = MakeLayer(std::move(content), TextLayerName(style.text));
	result.transform = QTransform::fromTranslate(
		canvasPoint.x() - part * size.width(),
		canvasPoint.y() - size.height() / 2.);
	return result;
}

bool SetLayerText(Layer &layer, const TextLayerStyle &style) {
	const auto old = AsText(layer.content);
	if (!old) {
		return false;
	}
	const auto normalized = NormalizedText(style);
	const auto oldName = TextLayerName(old->style().text);
	const auto oldWidth = old->size().width();
	auto content = MakeTextContent(normalized);
	const auto shift = TextAnchorShift(
		oldWidth,
		content->size().width(),
		normalized.align);
	if (layer.name.isEmpty() || layer.name == oldName) {
		layer.name = TextLayerName(normalized.text);
	}
	if (shift != 0.) {
		layer.transform = QTransform::fromTranslate(shift, 0.)
			* layer.transform;
	}
	layer.content = std::move(content);
	return true;
}

} // namespace Oblivion::Photo
