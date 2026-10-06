/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_photo_transform.h"

#include <array>
#include <cmath>

namespace Oblivion::Photo {
namespace {

constexpr auto kPi = 3.14159265358979323846;
constexpr auto kTiny = 1e-9;
constexpr auto kMaxCanvasExtent = 1e6;
constexpr auto kMinDepth = 1e-6;
constexpr auto kEdgeFitRadii = 4.5;
constexpr auto kEdgeLinePart = 0.5;
constexpr auto kSnapLayersLimit = 48;
constexpr auto kParallelPixels = 65536;

[[nodiscard]] double Length(QPointF point) {
	return std::hypot(point.x(), point.y());
}

[[nodiscard]] double Dot(QPointF a, QPointF b) {
	return a.x() * b.x() + a.y() * b.y();
}

[[nodiscard]] double Cross(QPointF a, QPointF b) {
	return a.x() * b.y() - a.y() * b.x();
}

[[nodiscard]] bool Finite(QPointF point) {
	return std::isfinite(point.x()) && std::isfinite(point.y());
}

[[nodiscard]] QPointF ContentCenter(QSizeF content) {
	return QPointF(content.width() / 2., content.height() / 2.);
}

[[nodiscard]] QPointF QuadCenter(const QPolygonF &quad) {
	auto result = QPointF();
	for (const auto &point : quad) {
		result += point;
	}
	return quad.isEmpty() ? result : (result / double(quad.size()));
}

// The corners of an edge handle in the LayerQuad() order.
[[nodiscard]] std::pair<int, int> EdgeCorners(TransformHandle handle) {
	switch (handle) {
	case TransformHandle::Top: return { 0, 1 };
	case TransformHandle::Right: return { 1, 2 };
	case TransformHandle::Bottom: return { 2, 3 };
	case TransformHandle::Left: return { 3, 0 };
	default: return { -1, -1 };
	}
}

[[nodiscard]] double DistanceToSegment(QPointF point, QPointF a, QPointF b) {
	const auto axis = b - a;
	const auto square = Dot(axis, axis);
	const auto t = (square > kTiny)
		? std::clamp(Dot(point - a, axis) / square, 0., 1.)
		: 0.;
	return Length(point - (a + axis * t));
}

// The nearest line: false if none is closer than the threshold.
[[nodiscard]] bool NearestLine(
		const std::vector<double> &lines,
		double value,
		double threshold,
		double &delta,
		double &line) {
	auto found = false;
	for (const auto candidate : lines) {
		const auto distance = candidate - value;
		if (std::abs(distance) > threshold
			|| (found && std::abs(distance) >= std::abs(delta))) {
			continue;
		}
		found = true;
		delta = distance;
		line = candidate;
	}
	return found;
}

[[nodiscard]] bool SnapValues(
		const std::vector<double> &lines,
		std::initializer_list<double> values,
		double threshold,
		double &delta,
		std::optional<double> &guide) {
	auto found = false;
	for (const auto value : values) {
		auto nowDelta = 0.;
		auto nowLine = 0.;
		if (!NearestLine(lines, value, threshold, nowDelta, nowLine)
			|| (found && std::abs(nowDelta) >= std::abs(delta))) {
			continue;
		}
		found = true;
		delta = nowDelta;
		guide = nowLine;
	}
	return found;
}

[[nodiscard]] int MaxDifference(const QImage &a, const QImage &b) {
	if (a.size() != b.size() || a.isNull()) {
		return 255;
	}
	auto result = 0;
	for (auto y = 0; y != a.height(); ++y) {
		const auto one = FxRow(a, y);
		const auto two = FxRow(b, y);
		for (auto x = 0; x != a.width(); ++x) {
			for (auto shift = 0; shift != 32; shift += 8) {
				result = std::max(result, std::abs(
					int((one[x] >> shift) & 0xFFU)
						- int((two[x] >> shift) & 0xFFU)));
			}
		}
	}
	return result;
}

[[nodiscard]] QImage SolidImage(int width, int height, QColor color) {
	auto result = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
	result.fill(color);
	return result;
}

} // namespace

bool IsCornerHandle(TransformHandle handle) {
	return (HandleCorner(handle) >= 0);
}

bool IsEdgeHandle(TransformHandle handle) {
	return (EdgeCorners(handle).first >= 0);
}

int HandleCorner(TransformHandle handle) {
	switch (handle) {
	case TransformHandle::TopLeft: return 0;
	case TransformHandle::TopRight: return 1;
	case TransformHandle::BottomRight: return 2;
	case TransformHandle::BottomLeft: return 3;
	default: return -1;
	}
}

TransformHandle CornerHandle(int corner) {
	switch (corner) {
	case 0: return TransformHandle::TopLeft;
	case 1: return TransformHandle::TopRight;
	case 2: return TransformHandle::BottomRight;
	case 3: return TransformHandle::BottomLeft;
	}
	return TransformHandle::None;
}

QPolygonF ContentQuad(QSizeF content) {
	return QPolygonF({
		QPointF(0., 0.),
		QPointF(content.width(), 0.),
		QPointF(content.width(), content.height()),
		QPointF(0., content.height()),
	});
}

QPolygonF TransformedQuad(const QTransform &transform, QSizeF content) {
	// Point by point: mapping a polygon with a perspective transform clips
	// it and may give another number of points.
	auto result = QPolygonF();
	result.reserve(4);
	for (const auto &point : ContentQuad(content)) {
		result.push_back(transform.map(point));
	}
	return result;
}

QPointF HandleLocalPoint(TransformHandle handle, QSizeF content) {
	const auto w = content.width();
	const auto h = content.height();
	switch (handle) {
	case TransformHandle::TopLeft: return QPointF(0., 0.);
	case TransformHandle::Top: return QPointF(w / 2., 0.);
	case TransformHandle::TopRight: return QPointF(w, 0.);
	case TransformHandle::Right: return QPointF(w, h / 2.);
	case TransformHandle::BottomRight: return QPointF(w, h);
	case TransformHandle::Bottom: return QPointF(w / 2., h);
	case TransformHandle::BottomLeft: return QPointF(0., h);
	case TransformHandle::Left: return QPointF(0., h / 2.);
	case TransformHandle::Rotate: return QPointF(w / 2., 0.);
	case TransformHandle::None:
	case TransformHandle::Move: break;
	}
	return ContentCenter(content);
}

QPointF HandleAnchorPoint(
		TransformHandle handle,
		QSizeF content,
		bool aroundCenter) {
	if (aroundCenter || (!IsCornerHandle(handle) && !IsEdgeHandle(handle))) {
		return ContentCenter(content);
	}
	const auto grip = HandleLocalPoint(handle, content);
	return QPointF(content.width() - grip.x(), content.height() - grip.y());
}

bool EdgeHandlesFit(const QPolygonF &quad, double radius) {
	if (quad.size() != 4) {
		return false;
	}
	for (auto i = 0; i != 4; ++i) {
		if (Length(quad[(i + 1) % 4] - quad[i]) < kEdgeFitRadii * radius) {
			return false;
		}
	}
	return true;
}

QPointF HandlePoint(const QPolygonF &quad, TransformHandle handle) {
	if (quad.size() != 4) {
		return QPointF();
	}
	const auto corner = HandleCorner(handle);
	if (corner >= 0) {
		return quad[corner];
	}
	const auto edge = EdgeCorners((handle == TransformHandle::Rotate)
		? TransformHandle::Top
		: handle);
	if (edge.first >= 0) {
		return (quad[edge.first] + quad[edge.second]) / 2.;
	}
	return QuadCenter(quad);
}

QPointF RotateHandlePoint(const QPolygonF &quad, double offset) {
	if (quad.size() != 4) {
		return QPointF();
	}
	const auto middle = (quad[0] + quad[1]) / 2.;
	const auto outside = middle - QuadCenter(quad);
	const auto edge = quad[1] - quad[0];
	const auto length = Length(edge);
	auto normal = QPointF(0., -1.);
	if (length > kTiny) {
		normal = QPointF(edge.y() / length, -edge.x() / length);
		if (Dot(normal, outside) < 0.) {
			normal = -normal;
		}
	} else if (Length(outside) > kTiny) {
		normal = outside / Length(outside);
	}
	return middle + normal * offset;
}

double ShortestSide(const QPolygonF &quad) {
	if (quad.size() != 4) {
		return 0.;
	}
	auto result = Length(quad[1] - quad[0]);
	for (auto i = 1; i != 4; ++i) {
		result = std::min(result, Length(quad[(i + 1) % 4] - quad[i]));
	}
	return result;
}

RotateHandlePlace PlaceRotateHandle(
		const QPolygonF &quad,
		const HandleMetrics &metrics) {
	if (quad.size() != 4) {
		return {};
	}
	const auto offset = metrics.rotateOffset;
	const auto top = (quad[0] + quad[1]) / 2.;
	const auto above = RotateHandlePlace{
		.handle = RotateHandlePoint(quad, offset),
		.edge = top,
	};
	if (metrics.view.isEmpty()) {
		return above;
	}
	const auto margin = std::max(metrics.radius, 0.);
	const auto seen = metrics.view.marginsRemoved(
		QMarginsF(margin, margin, margin, margin));
	if (seen.isEmpty() || seen.contains(above.handle)) {
		return above;
	}
	const auto bottom = (quad[2] + quad[3]) / 2.;
	const auto inside = RotateHandlePoint(quad, -offset);
	if (Length(bottom - top) >= 2. * (offset + margin)
		&& seen.contains(inside)
		&& quad.containsPoint(inside, Qt::OddEvenFill)) {
		return RotateHandlePlace{ .handle = inside, .edge = top };
	}
	const auto under = RotateHandlePoint(
		QPolygonF({ quad[2], quad[3], quad[0], quad[1] }),
		offset);
	if (seen.contains(under)) {
		return RotateHandlePlace{ .handle = under, .edge = bottom };
	}
	return above;
}

TransformHandle HitTestTransform(
		const QPolygonF &quad,
		QPointF point,
		const HandleMetrics &metrics) {
	if (quad.size() != 4) {
		return TransformHandle::None;
	}
	const auto radius = metrics.radius;
	if (metrics.rotate) {
		const auto handle = PlaceRotateHandle(quad, metrics).handle;
		if (Length(point - handle) <= radius) {
			return TransformHandle::Rotate;
		}
	}
	const auto inside = quad.containsPoint(point, Qt::OddEvenFill);
	auto best = TransformHandle::None;
	auto distance = inside
		? std::min(radius, std::max(ShortestSide(quad), radius) / 3.)
		: radius;
	for (auto i = 0; i != 4; ++i) {
		const auto now = Length(point - quad[i]);
		if (now <= distance) {
			distance = now;
			best = CornerHandle(i);
		}
	}
	if (best != TransformHandle::None) {
		return best;
	}
	distance = radius;
	if (metrics.edges && EdgeHandlesFit(quad, radius)) {
		const auto edges = {
			TransformHandle::Top,
			TransformHandle::Right,
			TransformHandle::Bottom,
			TransformHandle::Left,
		};
		for (const auto handle : edges) {
			const auto now = Length(point - HandlePoint(quad, handle));
			if (now <= distance) {
				distance = now;
				best = handle;
			}
		}
		if (best != TransformHandle::None) {
			return best;
		}
		// The whole edge resizes, not only its handle.
		distance = radius * kEdgeLinePart;
		for (const auto handle : edges) {
			const auto corners = EdgeCorners(handle);
			const auto now = DistanceToSegment(
				point,
				quad[corners.first],
				quad[corners.second]);
			if (now <= distance) {
				distance = now;
				best = handle;
			}
		}
		if (best != TransformHandle::None) {
			return best;
		}
	}
	return inside ? TransformHandle::Move : TransformHandle::None;
}

QTransform MovedTransform(const QTransform &start, QPointF delta) {
	return start * QTransform::fromTranslate(delta.x(), delta.y());
}

QPointF ConstrainedDelta(QPointF delta) {
	return (std::abs(delta.x()) >= std::abs(delta.y()))
		? QPointF(delta.x(), 0.)
		: QPointF(0., delta.y());
}

QTransform LocalScaled(
		const QTransform &start,
		QPointF anchor,
		double scaleX,
		double scaleY) {
	auto local = QTransform();
	local.translate(anchor.x(), anchor.y());
	local.scale(scaleX, scaleY);
	local.translate(-anchor.x(), -anchor.y());
	return local * start;
}

QTransform ScaledTransform(
		const QTransform &start,
		QSizeF content,
		const ScaleArgs &args,
		SnapResult *snap) {
	if (snap) {
		*snap = SnapResult();
	}
	const auto handle = args.handle;
	if ((!IsCornerHandle(handle) && !IsEdgeHandle(handle))
		|| content.isEmpty()
		|| !Finite(args.point)) {
		return start;
	}
	auto invertible = false;
	const auto inverse = start.inverted(&invertible);
	if (!invertible) {
		return start;
	}
	const auto grip = HandleLocalPoint(handle, content);
	const auto anchor = HandleAnchorPoint(handle, content, args.aroundCenter);
	const auto span = grip - anchor;
	const auto byX = std::abs(span.x()) > kTiny;
	const auto byY = std::abs(span.y()) > kTiny;
	const auto mouse = inverse.map(args.point);
	if (!Finite(mouse)) {
		return start;
	}
	auto scaleX = byX ? ((mouse.x() - anchor.x()) / span.x()) : 1.;
	auto scaleY = byY ? ((mouse.y() - anchor.y()) / span.y()) : 1.;
	const auto anchorCanvas = start.map(anchor);
	const auto gripCanvas = start.map(grip);
	if (args.keepAspect) {
		if (byX && byY) {
			auto uniform = 1.;
			if (start.isAffine()) {
				// The handle follows the cursor along the diagonal as it
				// is seen on the canvas.
				const auto diagonal = gripCanvas - anchorCanvas;
				const auto square = Dot(diagonal, diagonal);
				if (square > kTiny) {
					uniform = Dot(args.point - anchorCanvas, diagonal) / square;
				}
			} else {
				uniform = Dot(mouse - anchor, span) / Dot(span, span);
			}
			scaleX = scaleY = uniform;
		} else if (byX) {
			scaleY = scaleX;
		} else {
			scaleX = scaleY;
		}
	}

	const auto w = content.width();
	const auto h = content.height();
	const auto extentX = Length(
		start.map(QPointF(w, h / 2.)) - start.map(QPointF(0., h / 2.)));
	const auto extentY = Length(
		start.map(QPointF(w / 2., h)) - start.map(QPointF(w / 2., 0.)));
	const auto least = [&](double extent) {
		return (extent > kTiny)
			? std::min(std::max(args.minSize, kTiny) / extent, 1.)
			: 1.;
	};
	const auto most = [&](double extent) {
		return (extent > kTiny)
			? std::max(kMaxCanvasExtent / extent, 1.)
			: 1.;
	};
	const auto limited = [](double value, double from, double till) {
		if (!std::isfinite(value)) {
			return 1.;
		}
		const auto sign = (value < 0.) ? -1. : 1.;
		return sign * std::clamp(std::abs(value), from, std::max(from, till));
	};
	if (args.keepAspect) {
		scaleX = scaleY = limited(
			scaleX,
			std::max(least(extentX), least(extentY)),
			std::min(most(extentX), most(extentY)));
	} else {
		scaleX = limited(scaleX, least(extentX), most(extentX));
		scaleY = limited(scaleY, least(extentY), most(extentY));
	}

	if (args.targets && args.threshold > 0. && IsAxisAligned(start)) {
		const auto moved = LocalScaled(
			start,
			anchor,
			scaleX,
			scaleY).map(grip);
		struct Correction {
			bool found = false;
			double ratio = 1.;
			double delta = 0.;
			double line = 0.;
		};
		const auto correction = [&](
				const std::vector<double> &lines,
				double value,
				double from,
				bool affected) {
			auto result = Correction();
			if (!affected || std::abs(value - from) <= kTiny) {
				return result;
			}
			if (!NearestLine(
					lines,
					value,
					args.threshold,
					result.delta,
					result.line)) {
				return result;
			}
			const auto ratio = (result.line - from) / (value - from);
			if (std::isfinite(ratio) && ratio > kTiny) {
				result.found = true;
				result.ratio = ratio;
			}
			return result;
		};
		const auto startReach = gripCanvas - anchorCanvas;
		const auto alongX = correction(
			args.targets->xs,
			moved.x(),
			anchorCanvas.x(),
			std::abs(startReach.x()) > kTiny);
		const auto alongY = correction(
			args.targets->ys,
			moved.y(),
			anchorCanvas.y(),
			std::abs(startReach.y()) > kTiny);
		// A layer turned by 90 degrees has its width along the canvas Y.
		const auto swapped = std::abs(start.m11()) < std::abs(start.m12());
		if (args.keepAspect) {
			const auto useX = alongX.found
				&& (!alongY.found
					|| std::abs(alongX.delta) <= std::abs(alongY.delta));
			if (useX) {
				scaleX *= alongX.ratio;
				scaleY *= alongX.ratio;
				if (snap) {
					snap->guideX = alongX.line;
				}
			} else if (alongY.found) {
				scaleX *= alongY.ratio;
				scaleY *= alongY.ratio;
				if (snap) {
					snap->guideY = alongY.line;
				}
			}
		} else {
			if (alongX.found) {
				(swapped ? scaleY : scaleX) *= alongX.ratio;
				if (snap) {
					snap->guideX = alongX.line;
				}
			}
			if (alongY.found) {
				(swapped ? scaleX : scaleY) *= alongY.ratio;
				if (snap) {
					snap->guideY = alongY.line;
				}
			}
		}
		if (snap) {
			snap->delta = LocalScaled(
				start,
				anchor,
				scaleX,
				scaleY).map(grip) - moved;
		}
	}
	return LocalScaled(start, anchor, scaleX, scaleY);
}

double NormalizedAngle(double degrees) {
	if (!std::isfinite(degrees)) {
		return 0.;
	}
	auto result = std::fmod(degrees, 360.);
	if (result > 180.) {
		result -= 360.;
	} else if (result <= -180.) {
		result += 360.;
	}
	return result;
}

double AngleBetween(QPointF pivot, QPointF from, QPointF to) {
	const auto a = from - pivot;
	const auto b = to - pivot;
	if (Length(a) <= kTiny || Length(b) <= kTiny) {
		return 0.;
	}
	const auto first = std::atan2(a.y(), a.x());
	const auto second = std::atan2(b.y(), b.x());
	return NormalizedAngle((second - first) * 180. / kPi);
}

double SnappedAngle(double degrees, double step) {
	return (step > 0.) ? (std::round(degrees / step) * step) : degrees;
}

QTransform RotatedTransform(
		const QTransform &start,
		QPointF pivot,
		double degrees) {
	auto turn = QTransform();
	turn.translate(pivot.x(), pivot.y());
	turn.rotate(degrees);
	turn.translate(-pivot.x(), -pivot.y());
	return start * turn;
}

QTransform FlippedTransform(
		const QTransform &start,
		QSizeF content,
		bool horizontal) {
	const auto center = start.map(ContentCenter(content));
	auto flip = QTransform();
	flip.translate(center.x(), center.y());
	flip.scale(horizontal ? -1. : 1., horizontal ? 1. : -1.);
	flip.translate(-center.x(), -center.y());
	return start * flip;
}

QTransform FlippedTransform(
		const QTransform &start,
		QSizeF content,
		bool horizontal,
		const QTransform &orientation) {
	const auto seen = ViewOrientation(orientation);
	auto invertible = false;
	const auto back = seen.inverted(&invertible);
	if (!invertible || seen.isIdentity()) {
		return FlippedTransform(start, content, horizontal);
	}
	const auto center = start.map(ContentCenter(content));
	return start
		* QTransform::fromTranslate(-center.x(), -center.y())
		* seen
		* QTransform::fromScale(horizontal ? -1. : 1., horizontal ? 1. : -1.)
		* back
		* QTransform::fromTranslate(center.x(), center.y());
}

std::optional<QTransform> CornerMovedTransform(
		const QTransform &start,
		QSizeF content,
		int corner,
		QPointF point) {
	if (corner < 0 || corner > 3 || content.isEmpty() || !Finite(point)) {
		return std::nullopt;
	}
	auto quad = TransformedQuad(start, content);
	quad[corner] = point;
	for (auto i = 0; i != 4; ++i) {
		if (Length(quad[(i + 1) % 4] - quad[i]) < 1.) {
			return std::nullopt;
		}
	}
	auto result = QTransform();
	if (!QuadTransform(content, quad, result)
		|| !ValidTransform(result, content)) {
		return std::nullopt;
	}
	return result;
}

std::optional<QTransform> EdgeSkewedTransform(
		const QTransform &start,
		QSizeF content,
		TransformHandle edge,
		QPointF delta) {
	const auto corners = EdgeCorners(edge);
	if (corners.first < 0 || content.isEmpty() || !Finite(delta)) {
		return std::nullopt;
	}
	auto quad = TransformedQuad(start, content);
	const auto axis = quad[corners.second] - quad[corners.first];
	const auto length = Length(axis);
	if (length <= kTiny) {
		return std::nullopt;
	}
	const auto direction = axis / length;
	const auto shift = direction * Dot(delta, direction);
	quad[corners.first] += shift;
	quad[corners.second] += shift;
	auto result = QTransform();
	if (!QuadTransform(content, quad, result)
		|| !ValidTransform(result, content)) {
		return std::nullopt;
	}
	return result;
}

QTransform FittedTransform(
		const QTransform &start,
		QSizeF content,
		QSizeF canvas,
		bool fill) {
	const auto bounds = TransformedQuad(start, content).boundingRect();
	if (bounds.width() <= kTiny
		|| bounds.height() <= kTiny
		|| canvas.isEmpty()) {
		return start;
	}
	const auto kx = canvas.width() / bounds.width();
	const auto ky = canvas.height() / bounds.height();
	const auto scale = fill ? std::max(kx, ky) : std::min(kx, ky);
	const auto center = bounds.center();
	auto place = QTransform();
	place.translate(canvas.width() / 2., canvas.height() / 2.);
	place.scale(scale, scale);
	place.translate(-center.x(), -center.y());
	return start * place;
}

QTransform CenteredTransform(
		const QTransform &start,
		QSizeF content,
		QSizeF canvas) {
	const auto bounds = TransformedQuad(start, content).boundingRect();
	return MovedTransform(
		start,
		QPointF(canvas.width() / 2., canvas.height() / 2.) - bounds.center());
}

QTransform DefaultTransform(QSize content, QSize canvas) {
	return PlaceTransform(content, canvas, Placement::Fit);
}

bool IsAxisAligned(const QTransform &transform) {
	if (!transform.isAffine()) {
		return false;
	}
	const auto a = std::abs(transform.m11());
	const auto b = std::abs(transform.m12());
	const auto c = std::abs(transform.m21());
	const auto d = std::abs(transform.m22());
	const auto scale = std::max({ a, b, c, d });
	if (!(scale > kTiny)) {
		return false;
	}
	const auto epsilon = scale * 1e-6;
	return (b < epsilon && c < epsilon) || (a < epsilon && d < epsilon);
}

bool ValidTransform(const QTransform &transform, QSizeF content) {
	const auto values = {
		transform.m11(),
		transform.m12(),
		transform.m13(),
		transform.m21(),
		transform.m22(),
		transform.m23(),
		transform.m31(),
		transform.m32(),
		transform.m33(),
	};
	for (const auto value : values) {
		if (!std::isfinite(value)) {
			return false;
		}
	}
	if (content.isEmpty()) {
		return false;
	}
	auto invertible = false;
	[[maybe_unused]] const auto inverse = transform.inverted(&invertible);
	if (!invertible) {
		return false;
	}
	for (const auto &point : ContentQuad(content)) {
		const auto depth = transform.m13() * point.x()
			+ transform.m23() * point.y()
			+ transform.m33();
		if (!(depth > kMinDepth)) {
			return false;
		}
	}
	const auto quad = TransformedQuad(transform, content);
	for (const auto &point : quad) {
		if (!Finite(point)) {
			return false;
		}
	}
	const auto bounds = quad.boundingRect();
	const auto longer = std::max(bounds.width(), bounds.height());
	return (longer >= 0.5)
		&& (longer <= kMaxCanvasExtent * 4.)
		&& (std::abs(bounds.center().x()) <= kMaxCanvasExtent * 4.)
		&& (std::abs(bounds.center().y()) <= kMaxCanvasExtent * 4.);
}

double LocalScaleAt(const QTransform &transform, QPointF local) {
	const auto origin = transform.map(local);
	const auto alongX = transform.map(local + QPointF(1., 0.)) - origin;
	const auto alongY = transform.map(local + QPointF(0., 1.)) - origin;
	const auto area = std::abs(Cross(alongX, alongY));
	return (std::isfinite(area) && area > kTiny) ? std::sqrt(area) : 1.;
}

SnapTargets MakeSnapTargets(const Document &document, LayerId except) {
	auto result = SnapTargets();
	const auto add = [&](const QRectF &rect) {
		if (!std::isfinite(rect.x())
			|| !std::isfinite(rect.y())
			|| !std::isfinite(rect.width())
			|| !std::isfinite(rect.height())) {
			return;
		}
		result.xs.push_back(rect.left());
		result.xs.push_back(rect.center().x());
		result.xs.push_back(rect.right());
		result.ys.push_back(rect.top());
		result.ys.push_back(rect.center().y());
		result.ys.push_back(rect.bottom());
	};
	add(QRectF(QPointF(), QSizeF(document.size)));
	auto left = kSnapLayersLimit;
	for (auto i = int(document.layers.size()); i != 0 && left > 0;) {
		const auto &layer = document.layers[--i];
		if (layer.id == except
			|| !layer.visible
			|| !layer.content
			|| layer.size().isEmpty()) {
			continue;
		}
		add(TransformedQuad(
			layer.transform,
			QSizeF(layer.size())).boundingRect());
		--left;
	}
	return result;
}

SnapResult SnapRect(
		const QRectF &bounds,
		const SnapTargets &targets,
		double threshold,
		bool horizontal,
		bool vertical) {
	auto result = SnapResult();
	if (threshold <= 0.) {
		return result;
	}
	auto dx = 0.;
	auto dy = 0.;
	if (horizontal && SnapValues(
			targets.xs,
			{ bounds.left(), bounds.center().x(), bounds.right() },
			threshold,
			dx,
			result.guideX)) {
		result.delta.setX(dx);
	}
	if (vertical && SnapValues(
			targets.ys,
			{ bounds.top(), bounds.center().y(), bounds.bottom() },
			threshold,
			dy,
			result.guideY)) {
		result.delta.setY(dy);
	}
	return result;
}

SnapResult SnapPoint(
		QPointF point,
		const SnapTargets &targets,
		double threshold,
		bool horizontal,
		bool vertical) {
	auto result = SnapResult();
	if (threshold <= 0.) {
		return result;
	}
	auto dx = 0.;
	auto dy = 0.;
	if (horizontal && SnapValues(
			targets.xs,
			{ point.x() },
			threshold,
			dx,
			result.guideX)) {
		result.delta.setX(dx);
	}
	if (vertical && SnapValues(
			targets.ys,
			{ point.y() },
			threshold,
			dy,
			result.guideY)) {
		result.delta.setY(dy);
	}
	return result;
}

QTransform ViewOrientation(const QTransform &view) {
	if (!view.isAffine()) {
		return QTransform();
	}
	const auto alongX = QPointF(view.m11(), view.m12());
	const auto length = Length(alongX);
	const auto cross = Cross(alongX, QPointF(view.m21(), view.m22()));
	if (!std::isfinite(length)
		|| !std::isfinite(cross)
		|| !(length > 0.)
		|| !(std::abs(cross) > 0.)) {
		return QTransform();
	}
	const auto x = alongX / length;
	if (!Finite(x)) {
		return QTransform();
	}
	const auto sign = (cross < 0.) ? -1. : 1.;
	return QTransform(x.x(), x.y(), -sign * x.y(), sign * x.x(), 0., 0.);
}

QTransform QuarterOrientation(const QTransform &orientation) {
	const auto seen = ViewOrientation(orientation);
	const auto x = (std::abs(seen.m11()) >= std::abs(seen.m12()))
		? QPointF((seen.m11() < 0.) ? -1. : 1., 0.)
		: QPointF(0., (seen.m12() < 0.) ? -1. : 1.);
	const auto sign = (seen.determinant() < 0.) ? -1. : 1.;
	return QTransform(x.x(), x.y(), -sign * x.y(), sign * x.x(), 0., 0.);
}

QPointF SeenStep(
		const QTransform &orientation,
		QPointF direction,
		double step,
		double viewScale) {
	if (!Finite(direction) || !std::isfinite(step) || !(step > 0.)) {
		return QPointF();
	}
	auto invertible = false;
	const auto back = ViewOrientation(orientation).inverted(&invertible);
	const auto along = invertible ? back.map(direction) : direction;
	const auto length = Length(along);
	if (!std::isfinite(length) || !(length > kTiny)) {
		return QPointF();
	}
	const auto zoom = (std::isfinite(viewScale) && viewScale > kTiny)
		? std::min(viewScale, 1.)
		: 1.;
	const auto wanted = std::min(step / zoom, kMaxCanvasExtent);
	const auto unit = along / length;
	const auto alongAxis = [](double value) {
		return std::abs(value) < 1e-6;
	};
	if (!alongAxis(unit.x()) && !alongAxis(unit.y())) {
		return unit * wanted;
	}
	const auto whole = std::max(std::round(wanted), 1.);
	const auto part = [&](double value) {
		return alongAxis(value) ? 0. : (value < 0.) ? -whole : whole;
	};
	return QPointF(part(unit.x()), part(unit.y()));
}

double SeenTurn(const QTransform &orientation, double degrees) {
	return (ViewOrientation(orientation).determinant() < 0.)
		? -degrees
		: degrees;
}

QPointF SeenShift(
		const QTransform &orientation,
		QPointF point,
		QSizeF canvas) {
	return ViewOrientation(orientation).map(
		point - QPointF(canvas.width() / 2., canvas.height() / 2.));
}

QPointF PointFromSeenShift(
		const QTransform &orientation,
		QPointF shift,
		QSizeF canvas) {
	auto invertible = false;
	const auto back = ViewOrientation(orientation).inverted(&invertible);
	return QPointF(canvas.width() / 2., canvas.height() / 2.)
		+ (invertible ? back.map(shift) : shift);
}

LinkedPercents LinkPercents(
		double changed,
		double other,
		double value,
		double from,
		double till) {
	if (!std::isfinite(value)
		|| !std::isfinite(changed)
		|| !std::isfinite(other)) {
		return LinkedPercents{ .changed = changed, .other = other };
	} else if (!(changed > kTiny)
		|| !(other > kTiny)
		|| !(from > 0.)
		|| !(till >= from)) {
		return LinkedPercents{ .changed = value, .other = other };
	}
	// A side that is out of the limits already (a layer scaled that far
	// by its handles) is not brought back, it only can't go further.
	const auto least = std::min(from / other, 1.);
	const auto most = std::max(till / other, 1.);
	const auto ratio = std::clamp(value / changed, least, most);
	return LinkedPercents{
		.changed = changed * ratio,
		.other = other * ratio,
	};
}

TransformNumbers NumbersFromTransform(
		const QTransform &transform,
		QSizeF content) {
	const auto parts = DecomposeTransform(transform, content);
	auto result = TransformNumbers();
	result.x = parts.center.x();
	result.y = parts.center.y();
	auto scaleX = parts.scaleX;
	auto scaleY = parts.scaleY;
	auto rotation = NormalizedAngle(parts.rotation);
	if (scaleY < 0.) {
		// The same mirrored picture can be told as "flipped vertically
		// and turned by 180" or as "flipped horizontally": the one with
		// the smaller turn is shown.
		if (std::abs(rotation) > 90.) {
			rotation = NormalizedAngle(rotation - 180.);
			scaleX = -scaleX;
			scaleY = -scaleY;
			result.flippedX = true;
		} else {
			result.flippedY = true;
		}
	}
	result.width = std::abs(scaleX) * 100.;
	result.height = std::abs(scaleY) * 100.;
	result.rotation = rotation;
	result.skew = std::atan(parts.shear) * 180. / kPi;
	return result;
}

QTransform TransformFromNumbers(
		const TransformNumbers &numbers,
		QSizeF content) {
	auto parts = TransformParts();
	parts.center = QPointF(numbers.x, numbers.y);
	parts.scaleX = (numbers.width / 100.) * (numbers.flippedX ? -1. : 1.);
	parts.scaleY = (numbers.height / 100.) * (numbers.flippedY ? -1. : 1.);
	parts.rotation = numbers.rotation;
	parts.shear = std::tan(std::clamp(numbers.skew, -89., 89.) * kPi / 180.);
	return ComposeTransform(parts, content);
}

TransformNumbers WithRotation(
		const TransformNumbers &numbers,
		double degrees,
		std::optional<LayerFlips> flips) {
	auto result = numbers;
	if (flips) {
		result.flippedX = flips->x;
		result.flippedY = flips->y;
	}
	result.rotation = degrees;
	return result;
}

QImage AlphaMapFromPixels(const QImage &pixels) {
	if (pixels.isNull()) {
		return QImage();
	}
	auto result = pixels.convertToFormat(QImage::Format_Alpha8);
	result.setDevicePixelRatio(1.);
	return result;
}

bool AlphaMapHit(
		const QImage &map,
		QSizeF content,
		QPointF local,
		int threshold) {
	if (map.isNull()
		|| map.format() != QImage::Format_Alpha8
		|| content.isEmpty()
		|| !Finite(local)) {
		return true;
	}
	const auto width = map.width();
	const auto height = map.height();
	const auto x = std::clamp(
		int(std::floor(local.x() / content.width() * width)),
		0,
		width - 1);
	const auto y = std::clamp(
		int(std::floor(local.y() / content.height() * height)),
		0,
		height - 1);
	for (auto row = std::max(y - 1, 0); row <= std::min(y + 1, height - 1); ++row) {
		const auto line = map.constScanLine(row);
		for (auto column = std::max(x - 1, 0)
			; column <= std::min(x + 1, width - 1)
			; ++column) {
			if (line[column] >= threshold) {
				return true;
			}
		}
	}
	return false;
}

LayerId LayerAtPixels(
		const Document &document,
		QPointF canvasPoint,
		const Fn<const QImage*(LayerId)> &alphaMap,
		bool withLocked) {
	for (auto i = int(document.layers.size()); i != 0;) {
		const auto &layer = document.layers[--i];
		if (!layer.visible
			|| !layer.content
			|| !(layer.opacity > 0.)
			|| (layer.locked && !withLocked)) {
			continue;
		}
		const auto size = QSizeF(layer.size());
		const auto point = LayerPoint(layer, canvasPoint);
		if (!point || !QRectF(QPointF(), size).contains(*point)) {
			continue;
		}
		const auto map = alphaMap ? alphaMap(layer.id) : nullptr;
		if (!map || AlphaMapHit(*map, size, *point)) {
			return layer.id;
		}
	}
	return 0;
}

int LayerRowToIndex(int count, int row) {
	return (row >= 0 && row < count) ? (count - 1 - row) : -1;
}

int LayerIndexToRow(int count, int index) {
	return LayerRowToIndex(count, index);
}

int LayerDropIndex(int count, int fromRow, int gap) {
	if (count <= 0 || fromRow < 0 || fromRow >= count) {
		return -1;
	}
	const auto place = std::clamp(gap, 0, count);
	const auto row = (place > fromRow) ? (place - 1) : place;
	return (row == fromRow) ? -1 : (count - 1 - row);
}

uint64 LayerThumbnailKey(const Layer &layer) {
	auto result = FxHashCombine(
		0x7B1D5EEDULL,
		layer.content ? layer.content->serial() : 0);
	for (const auto &instance : layer.effects) {
		result = FxHashCombine(
			FxHashCombine(result, FxHash(instance)),
			instance.uid);
	}
	return FxHashCombine(
		result,
		(layer.mask && layer.maskEnabled) ? layer.mask->serial() : 0);
}

bool CanMergeDown(const Document &document, LayerId id) {
	const auto index = document.indexOf(id);
	if (index <= 0) {
		return false;
	}
	const auto &upper = document.layers[index];
	const auto &lower = document.layers[index - 1];
	return !upper.locked && !lower.locked && lower.content;
}

QPointF MaskPoint(QSizeF content, QSize mask, QPointF local) {
	if (content.isEmpty()) {
		return QPointF();
	}
	return QPointF(
		local.x() * mask.width() / content.width(),
		local.y() * mask.height() / content.height());
}

double MaskScale(QSizeF content, QSize mask) {
	if (content.isEmpty() || mask.isEmpty()) {
		return 1.;
	}
	return std::sqrt((mask.width() / content.width())
		* (mask.height() / content.height()));
}

QRect MaskStampSegment(
		QImage &coverage,
		QPointF from,
		QPointF to,
		double radiusFrom,
		double radiusTo,
		double hardness) {
	if (coverage.isNull()
		|| coverage.format() != QImage::Format_Alpha8
		|| !Finite(from)
		|| !Finite(to)
		|| !std::isfinite(radiusFrom)
		|| !std::isfinite(radiusTo)) {
		return QRect();
	}
	const auto first = std::max(radiusFrom, 0.5);
	const auto second = std::max(radiusTo, 0.5);
	const auto reach = std::max(first, second) + 1.;
	const auto area = QRectF(
		QPointF(
			std::min(from.x(), to.x()) - reach,
			std::min(from.y(), to.y()) - reach),
		QPointF(
			std::max(from.x(), to.x()) + reach,
			std::max(from.y(), to.y()) + reach));
	// Far outside: toAlignedRect() must not overflow.
	const auto whole = QRectF(coverage.rect());
	if (!area.intersects(whole)) {
		return QRect();
	}
	const auto bounds = area.intersected(whole).toAlignedRect().intersected(
		coverage.rect());
	if (bounds.isEmpty()) {
		return QRect();
	}
	const auto hard = std::clamp(hardness, 0., 1.);
	const auto axis = to - from;
	const auto square = Dot(axis, axis);
	const auto bits = coverage.bits();
	const auto stride = coverage.bytesPerLine();
	const auto left = bounds.x();
	const auto top = bounds.y();
	const auto width = bounds.width();
	const auto body = [&](int fromRow, int tillRow) {
		for (auto row = fromRow; row != tillRow; ++row) {
			const auto line = bits + qsizetype(top + row) * stride + left;
			const auto py = top + row + 0.5;
			for (auto column = 0; column != width; ++column) {
				if (line[column] == 255) {
					continue;
				}
				const auto point = QPointF(left + column + 0.5, py);
				const auto t = (square > kTiny)
					? std::clamp(Dot(point - from, axis) / square, 0., 1.)
					: 0.;
				const auto distance = Length(point - (from + axis * t));
				const auto radius = first + (second - first) * t;
				const auto outer = radius + 0.5;
				if (distance >= outer) {
					continue;
				}
				const auto inner = std::max(
					std::min(radius * hard, outer - 1.),
					0.);
				auto value = 255;
				if (distance > inner) {
					const auto u = (outer - distance) / (outer - inner);
					value = int(u * u * (3. - 2. * u) * 255. + 0.5);
				}
				if (value > line[column]) {
					line[column] = uchar(value);
				}
			}
		}
	};
	if (qint64(width) * bounds.height() >= kParallelPixels) {
		FxParallelRows(width, bounds.height(), body);
	} else {
		body(0, bounds.height());
	}
	return bounds;
}

void MaskComposeStroke(
		QImage &mask,
		const QImage &base,
		const QImage &coverage,
		QRect rect,
		int target,
		double opacity) {
	if (mask.isNull()
		|| mask.format() != QImage::Format_Grayscale8
		|| base.format() != QImage::Format_Grayscale8
		|| coverage.format() != QImage::Format_Alpha8
		|| base.size() != mask.size()
		|| coverage.size() != mask.size()) {
		return;
	}
	const auto area = rect.intersected(mask.rect());
	if (area.isEmpty()) {
		return;
	}
	const auto goal = std::clamp(target, 0, 255);
	const auto strength = int(std::clamp(opacity, 0., 1.) * 256. + 0.5);
	const auto bits = mask.bits();
	const auto stride = mask.bytesPerLine();
	const auto left = area.x();
	const auto width = area.width();
	const auto body = [&](int fromRow, int tillRow) {
		for (auto row = fromRow; row != tillRow; ++row) {
			const auto y = area.y() + row;
			const auto to = bits + qsizetype(y) * stride + left;
			const auto was = base.constScanLine(y) + left;
			const auto cover = coverage.constScanLine(y) + left;
			for (auto column = 0; column != width; ++column) {
				const auto k = (int(cover[column]) * strength + 127) / 255;
				const auto before = int(was[column]);
				to[column] = uchar(before + ((goal - before) * k) / 256);
			}
		}
	};
	if (qint64(width) * area.height() >= kParallelPixels) {
		FxParallelRows(width, area.height(), body);
	} else {
		body(0, area.height());
	}
}

QImage InvertedMask(const QImage &mask) {
	if (mask.isNull()) {
		return QImage();
	}
	auto result = (mask.format() == QImage::Format_Grayscale8)
		? mask.copy()
		: mask.convertToFormat(QImage::Format_Grayscale8);
	if (result.isNull()) {
		return QImage();
	}
	const auto bits = result.bits();
	const auto stride = result.bytesPerLine();
	const auto width = result.width();
	for (auto y = 0; y != result.height(); ++y) {
		const auto line = bits + qsizetype(y) * stride;
		for (auto x = 0; x != width; ++x) {
			line[x] = uchar(255 - line[x]);
		}
	}
	return result;
}

void UpdateMaskOverlay(
		QImage &overlay,
		const QImage &mask,
		QRect rect,
		QColor color) {
	if (mask.isNull() || mask.format() != QImage::Format_Grayscale8) {
		overlay = QImage();
		return;
	}
	if (overlay.size() != mask.size()
		|| overlay.format() != QImage::Format_ARGB32_Premultiplied) {
		overlay = QImage(mask.size(), QImage::Format_ARGB32_Premultiplied);
		if (overlay.isNull()) {
			return;
		}
		rect = mask.rect();
	}
	const auto area = rect.intersected(mask.rect());
	if (area.isEmpty()) {
		return;
	}
	auto table = std::array<uint32, 256>();
	for (auto i = 0; i != 256; ++i) {
		const auto alpha = ((255 - i) * color.alpha() + 127) / 255;
		table[i] = qRgba(
			(color.red() * alpha + 127) / 255,
			(color.green() * alpha + 127) / 255,
			(color.blue() * alpha + 127) / 255,
			alpha);
	}
	const auto bits = overlay.bits();
	const auto stride = overlay.bytesPerLine();
	const auto left = area.x();
	const auto width = area.width();
	const auto body = [&](int fromRow, int tillRow) {
		for (auto row = fromRow; row != tillRow; ++row) {
			const auto y = area.y() + row;
			const auto to = reinterpret_cast<uint32*>(
				bits + qsizetype(y) * stride) + left;
			const auto from = mask.constScanLine(y) + left;
			for (auto column = 0; column != width; ++column) {
				to[column] = table[from[column]];
			}
		}
	};
	if (qint64(width) * area.height() >= kParallelPixels) {
		FxParallelRows(width, area.height(), body);
	} else {
		body(0, area.height());
	}
}

namespace {

bool RunLayersSelfTest(QStringList &log) {
	using Handle = TransformHandle;

	auto ok = true;
	const auto check = [&](bool condition, const QString &what) {
		log.push_back((condition ? u"OK: "_q : u"FAIL: "_q) + what);
		ok = ok && condition;
	};
	const auto samePoint = [](QPointF a, QPointF b, double epsilon = 1e-6) {
		return Length(a - b) <= epsilon;
	};
	const auto sameQuad = [&](
			const QPolygonF &a,
			const QPolygonF &b,
			double epsilon = 1e-6) {
		if (a.size() != 4 || b.size() != 4) {
			return false;
		}
		for (auto i = 0; i != 4; ++i) {
			if (!samePoint(a[i], b[i], epsilon)) {
				return false;
			}
		}
		return true;
	};
	const auto rectQuad = [](double x, double y, double w, double h) {
		return QPolygonF({
			QPointF(x, y),
			QPointF(x + w, y),
			QPointF(x + w, y + h),
			QPointF(x, y + h),
		});
	};
	const auto about = [](double a, double b, double epsilon = 1e-6) {
		return std::abs(a - b) <= epsilon;
	};

	const auto content = QSizeF(200., 100.);
	const auto base = QTransform::fromTranslate(100., 100.);
	const auto center = QPointF(200., 150.);
	const auto turned = RotatedTransform(base, center, 30.);
	const auto quadOf = [&](const QTransform &transform) {
		return TransformedQuad(transform, content);
	};

	// The handles of the frame.
	{
		check(
			IsCornerHandle(Handle::TopLeft)
				&& !IsCornerHandle(Handle::Top)
				&& IsEdgeHandle(Handle::Left)
				&& !IsEdgeHandle(Handle::Rotate)
				&& !IsEdgeHandle(Handle::Move),
			u"handles: corners and edges are told apart"_q);
		auto corners = true;
		for (auto i = 0; i != 4; ++i) {
			corners = corners && (HandleCorner(CornerHandle(i)) == i);
		}
		check(
			corners
				&& (HandleCorner(Handle::Top) == -1)
				&& (CornerHandle(7) == Handle::None),
			u"handles: corner indexes round trip"_q);
		check(
			samePoint(
				HandleLocalPoint(Handle::Right, content),
				QPointF(200., 50.))
				&& samePoint(
					HandleAnchorPoint(Handle::Right, content, false),
					QPointF(0., 50.))
				&& samePoint(
					HandleAnchorPoint(Handle::TopLeft, content, false),
					QPointF(200., 100.))
				&& samePoint(
					HandleAnchorPoint(Handle::TopLeft, content, true),
					QPointF(100., 50.)),
			u"handles: the anchor is the opposite handle or the center"_q);
		check(
			sameQuad(quadOf(base), rectQuad(100., 100., 200., 100.))
				&& sameQuad(
					quadOf(base),
					QPolygonF(base.map(ContentQuad(content)))),
			u"handles: the frame of a moved layer"_q);
	}

	// Hit testing.
	{
		const auto metrics = HandleMetrics();
		const auto quad = quadOf(base);
		const auto hit = [&](double x, double y) {
			return HitTestTransform(quad, QPointF(x, y), metrics);
		};
		check(
			(hit(101., 99.) == Handle::TopLeft)
				&& (hit(300., 100.) == Handle::TopRight)
				&& (hit(298., 203.) == Handle::BottomRight)
				&& (hit(95., 200.) == Handle::BottomLeft),
			u"hit test: corners"_q);
		check(
			(hit(200., 100.) == Handle::Top)
				&& (hit(300., 152.) == Handle::Right)
				&& (hit(203., 204.) == Handle::Bottom)
				&& (hit(96., 150.) == Handle::Left),
			u"hit test: edge handles"_q);
		check(
			(hit(150., 102.) == Handle::Top)
				&& (hit(297., 180.) == Handle::Right)
				&& (hit(150., 108.) == Handle::Move),
			u"hit test: the whole edge resizes, the inside moves"_q);
		check(
			(hit(200., 150.) == Handle::Move)
				&& (hit(50., 50.) == Handle::None)
				&& (hit(200., 230.) == Handle::None),
			u"hit test: inside and outside"_q);
		check(
			samePoint(RotateHandlePoint(quad, 26.), QPointF(200., 74.))
				&& (hit(200., 74.) == Handle::Rotate)
				&& (hit(204., 70.) == Handle::Rotate)
				&& (hit(200., 86.) == Handle::None),
			u"hit test: the rotate handle is above the top edge"_q);
		auto corners = metrics;
		corners.edges = false;
		corners.rotate = false;
		check(
			(HitTestTransform(quad, QPointF(200., 100.5), corners)
				== Handle::Move)
				&& (HitTestTransform(quad, QPointF(200., 74.), corners)
					== Handle::None)
				&& (HitTestTransform(quad, QPointF(101., 99.), corners)
					== Handle::TopLeft),
			u"hit test: only corners in the perspective mode"_q);

		const auto tiny = rectQuad(0., 0., 30., 20.);
		check(
			!EdgeHandlesFit(tiny, metrics.radius)
				&& EdgeHandlesFit(quad, metrics.radius)
				&& (HitTestTransform(tiny, QPointF(15., 4.), metrics)
					== Handle::Move)
				&& (HitTestTransform(tiny, QPointF(29., 19.), metrics)
					== Handle::BottomRight),
			u"hit test: a small frame has no edge handles"_q);

		const auto quarter = quadOf(RotatedTransform(base, center, 90.));
		check(
			samePoint(quarter[0], QPointF(250., 50.))
				&& samePoint(quarter[1], QPointF(250., 250.))
				&& samePoint(
					RotateHandlePoint(quarter, 26.),
					QPointF(276., 150.))
				&& (HitTestTransform(quarter, QPointF(276., 150.), metrics)
					== Handle::Rotate)
				&& (HitTestTransform(quarter, QPointF(250., 150.), metrics)
					== Handle::Top)
				&& (HitTestTransform(quarter, QPointF(200., 150.), metrics)
					== Handle::Move),
			u"hit test: the handles follow a turned layer"_q);

		const auto mirrored = quadOf(FlippedTransform(base, content, false));
		check(
			samePoint(
				RotateHandlePoint(mirrored, 26.),
				QPointF(200., 226.)),
			u"hit test: the rotate handle of a flipped layer is outside"_q);

		const auto speck = rectQuad(0., 0., 10., 10.);
		const auto hitSpeck = [&](double x, double y) {
			return HitTestTransform(speck, QPointF(x, y), metrics);
		};
		check(
			about(ShortestSide(tiny), 20.)
				&& about(ShortestSide(speck), 10.)
				&& (hitSpeck(5., 5.) == Handle::Move)
				&& (hitSpeck(4., 6.) == Handle::Move)
				&& (hitSpeck(1., 1.) == Handle::TopLeft)
				&& (hitSpeck(-3., -2.) == Handle::TopLeft)
				&& (hitSpeck(12., 13.) == Handle::BottomRight)
				&& (hitSpeck(5., 30.) == Handle::None),
			u"hit test: a tiny frame moves from inside, resizes outside"_q);

		auto limited = metrics;
		limited.view = QRectF(0., 0., 400., 300.);
		const auto above = PlaceRotateHandle(quad, limited);
		const auto whole = rectQuad(0., 0., 400., 300.);
		const auto within = PlaceRotateHandle(whole, limited);
		check(
			samePoint(above.handle, QPointF(200., 74.))
				&& samePoint(above.edge, QPointF(200., 100.))
				&& samePoint(
					PlaceRotateHandle(whole, metrics).handle,
					QPointF(200., -26.))
				&& samePoint(within.handle, QPointF(200., 26.))
				&& samePoint(within.edge, QPointF(200., 0.))
				&& (HitTestTransform(whole, QPointF(200., 26.), limited)
					== Handle::Rotate)
				&& (HitTestTransform(whole, QPointF(200., 60.), limited)
					== Handle::Move)
				&& (HitTestTransform(whole, QPointF(200., 26.), metrics)
					== Handle::Move),
			u"hit test: the rotate handle of a layer that fills the view"_q);
		const auto shallow = rectQuad(150., 5., 100., 40.);
		const auto under = PlaceRotateHandle(shallow, limited);
		const auto away = rectQuad(150., -500., 100., 40.);
		check(
			samePoint(under.handle, QPointF(200., 71.))
				&& samePoint(under.edge, QPointF(200., 45.))
				&& (HitTestTransform(shallow, QPointF(200., 71.), limited)
					== Handle::Rotate)
				&& (HitTestTransform(shallow, QPointF(200., 25.), limited)
					== Handle::Move)
				&& samePoint(
					PlaceRotateHandle(away, limited).handle,
					QPointF(200., -526.)),
			u"hit test: the rotate handle goes under a low frame at the top"_q);
	}

	// Moving.
	{
		const auto moved = MovedTransform(base, QPointF(15., -20.));
		check(
			sameQuad(quadOf(moved), rectQuad(115., 80., 200., 100.)),
			u"move: the frame is shifted"_q);
		check(
			samePoint(ConstrainedDelta(QPointF(10., -3.)), QPointF(10., 0.))
				&& samePoint(
					ConstrainedDelta(QPointF(2., 9.)),
					QPointF(0., 9.)),
			u"move: Shift keeps one axis"_q);
	}

	// Scaling.
	{
		const auto drag = [&](
				const QTransform &start,
				Handle handle,
				QPointF point,
				bool keepAspect = false,
				bool aroundCenter = false) {
			return ScaledTransform(start, content, {
				.handle = handle,
				.point = point,
				.keepAspect = keepAspect,
				.aroundCenter = aroundCenter,
			});
		};
		check(
			sameQuad(
				quadOf(drag(base, Handle::BottomRight, QPointF(500., 300.))),
				rectQuad(100., 100., 400., 200.)),
			u"scale: a corner doubles the layer, the opposite one stays"_q);
		check(
			sameQuad(
				quadOf(drag(base, Handle::BottomRight, QPointF(400., 300.))),
				rectQuad(100., 100., 300., 200.)),
			u"scale: a corner changes both sides freely"_q);
		check(
			sameQuad(
				quadOf(drag(
					base,
					Handle::BottomRight,
					QPointF(400., 300.),
					true)),
				rectQuad(100., 100., 320., 160.)),
			u"scale: proportions are kept along the diagonal"_q);
		check(
			sameQuad(
				quadOf(drag(base, Handle::TopLeft, QPointF(200., 150.))),
				rectQuad(200., 150., 100., 50.)),
			u"scale: the top left corner keeps the bottom right one"_q);
		check(
			sameQuad(
				quadOf(drag(base, Handle::Right, QPointF(360., 500.))),
				rectQuad(100., 100., 260., 100.)),
			u"scale: an edge changes one side only"_q);
		check(
			sameQuad(
				quadOf(drag(base, Handle::Right, QPointF(400., 150.), true)),
				rectQuad(100., 75., 300., 150.)),
			u"scale: an edge with kept proportions grows evenly"_q);
		check(
			sameQuad(
				quadOf(drag(
					base,
					Handle::Right,
					QPointF(350., 150.),
					false,
					true)),
				rectQuad(50., 100., 300., 100.)),
			u"scale: around the center both edges move"_q);
		const auto crossed = drag(base, Handle::Right, QPointF(40., 150.));
		check(
			samePoint(crossed.map(QPointF(200., 0.)), QPointF(40., 100.))
				&& samePoint(crossed.map(QPointF(0., 0.)), QPointF(100., 100.))
				&& (crossed.determinant() < 0.),
			u"scale: dragging across the anchor mirrors the layer"_q);
		const auto collapsed = drag(
			base,
			Handle::BottomRight,
			QPointF(100., 100.));
		const auto collapsedBounds = quadOf(collapsed).boundingRect();
		check(
			ValidTransform(collapsed, content)
				&& (collapsedBounds.width() >= 0.999)
				&& (collapsedBounds.height() >= 0.999)
				&& (collapsedBounds.width() < 3.),
			u"scale: the layer never collapses to nothing"_q);

		const auto target = turned.map(QPointF(300., 150.));
		const auto scaled = drag(turned, Handle::BottomRight, target);
		check(
			samePoint(scaled.map(QPointF(0., 0.)), turned.map(QPointF(0., 0.)))
				&& samePoint(scaled.map(QPointF(200., 100.)), target)
				&& about(DecomposeTransform(scaled, content).rotation, 30.)
				&& about(DecomposeTransform(scaled, content).scaleX, 1.5)
				&& about(DecomposeTransform(scaled, content).scaleY, 1.5),
			u"scale: a turned layer is scaled along its own sides"_q);
		const auto edge = drag(
			turned,
			Handle::Bottom,
			turned.map(QPointF(100., 200.)));
		check(
			about(DecomposeTransform(edge, content).scaleX, 1.)
				&& about(DecomposeTransform(edge, content).scaleY, 2.)
				&& about(DecomposeTransform(edge, content).rotation, 30.)
				&& samePoint(
					edge.map(QPointF(100., 0.)),
					turned.map(QPointF(100., 0.))),
			u"scale: an edge of a turned layer"_q);

		const auto targets = SnapTargets{
			.xs = { 0., 400., 800. },
			.ys = { 0., 300., 600. },
		};
		auto snap = SnapResult();
		const auto snapped = ScaledTransform(base, content, {
			.handle = Handle::BottomRight,
			.point = QPointF(397., 250.),
			.targets = &targets,
			.threshold = 5.,
		}, &snap);
		check(
			sameQuad(quadOf(snapped), rectQuad(100., 100., 300., 150.))
				&& snap.guideX
				&& about(*snap.guideX, 400.)
				&& !snap.guideY,
			u"scale: the dragged edge sticks to a line"_q);
		const auto snappedBoth = ScaledTransform(base, content, {
			.handle = Handle::BottomRight,
			.point = QPointF(398., 248.),
			.keepAspect = true,
			.targets = &targets,
			.threshold = 5.,
		}, &snap);
		check(
			sameQuad(quadOf(snappedBoth), rectQuad(100., 100., 300., 150.))
				&& snap.guideX
				&& about(*snap.guideX, 400.),
			u"scale: sticking keeps the proportions"_q);
		const auto loose = ScaledTransform(base, content, {
			.handle = Handle::BottomRight,
			.point = QPointF(380., 250.),
			.targets = &targets,
			.threshold = 5.,
		}, &snap);
		check(
			sameQuad(quadOf(loose), rectQuad(100., 100., 280., 150.))
				&& !snap.guideX
				&& !snap.guideY,
			u"scale: nothing sticks beyond the threshold"_q);
		const auto quarter = RotatedTransform(base, center, 90.);
		const auto quarterSnapped = ScaledTransform(quarter, content, {
			.handle = Handle::Right,
			.point = QPointF(200., 297.),
			.targets = &targets,
			.threshold = 5.,
		}, &snap);
		check(
			about(quadOf(quarterSnapped).boundingRect().bottom(), 300.)
				&& about(quadOf(quarterSnapped).boundingRect().top(), 50.)
				&& snap.guideY
				&& about(*snap.guideY, 300.),
			u"scale: sticking works for a layer turned by 90 degrees"_q);
	}

	// Rotating.
	{
		check(
			about(AngleBetween(QPointF(), QPointF(1., 0.), QPointF(0., 1.)), 90.)
				&& about(
					AngleBetween(QPointF(), QPointF(1., 0.), QPointF(0., -1.)),
					-90.)
				&& about(
					AngleBetween(QPointF(), QPointF(1., 0.), QPointF(-1., 0.)),
					180.)
				&& about(
					AngleBetween(
						QPointF(5., 5.),
						QPointF(5., 0.),
						QPointF(10., 5.)),
					90.),
			u"rotate: the angle is clockwise on the screen"_q);
		check(
			about(NormalizedAngle(270.), -90.)
				&& about(NormalizedAngle(-180.), 180.)
				&& about(NormalizedAngle(725.), 5.)
				&& about(SnappedAngle(37., 15.), 30.)
				&& about(SnappedAngle(38., 15.), 45.)
				&& about(SnappedAngle(-8., 15.), -15.),
			u"rotate: angles are normalized and snapped"_q);
		const auto quarter = RotatedTransform(base, center, 90.);
		check(
			samePoint(quarter.map(QPointF(100., 50.)), center)
				&& samePoint(quarter.map(QPointF(0., 0.)), QPointF(250., 50.))
				&& about(DecomposeTransform(quarter, content).rotation, 90.),
			u"rotate: the pivot stays, the corners turn"_q);
		auto full = base;
		for (auto i = 0; i != 4; ++i) {
			full = RotatedTransform(full, center, 90.);
		}
		check(
			sameQuad(quadOf(full), quadOf(base)),
			u"rotate: four quarter turns give the layer back"_q);
		check(
			about(DecomposeTransform(turned, content).rotation, 30.)
				&& samePoint(turned.map(QPointF(100., 50.)), center)
				&& about(
					DecomposeTransform(
						RotatedTransform(turned, center, -45.),
						content).rotation,
					-15.),
			u"rotate: turns add up"_q);
	}

	// Flipping.
	{
		const auto flipped = FlippedTransform(turned, content, true);
		const auto before = quadOf(turned);
		const auto after = quadOf(flipped);
		auto mirrored = true;
		for (auto i = 0; i != 4; ++i) {
			mirrored = mirrored && samePoint(
				after[i],
				QPointF(2. * center.x() - before[i].x(), before[i].y()));
		}
		check(
			mirrored
				&& samePoint(flipped.map(QPointF(100., 50.)), center)
				&& (flipped.determinant() * turned.determinant() < 0.),
			u"flip: the picture is mirrored around its center"_q);
		check(
			sameQuad(
				quadOf(FlippedTransform(flipped, content, true)),
				before)
				&& sameQuad(
					quadOf(FlippedTransform(
						FlippedTransform(turned, content, false),
						content,
						false)),
					before),
			u"flip: twice is nothing"_q);
		check(
			sameQuad(
				quadOf(FlippedTransform(base, content, false)),
				QPolygonF({
					QPointF(100., 200.),
					QPointF(300., 200.),
					QPointF(300., 100.),
					QPointF(100., 100.),
				})),
			u"flip: vertical swaps the top and the bottom"_q);
	}

	// The whole picture turned, mirrored and straightened.
	{
		const auto sameTurn = [&](const QTransform &a, const QTransform &b) {
			return about(a.m11(), b.m11())
				&& about(a.m12(), b.m12())
				&& about(a.m21(), b.m21())
				&& about(a.m22(), b.m22())
				&& about(a.dx(), b.dx())
				&& about(a.dy(), b.dy());
		};
		const auto none = QTransform();
		const auto quarter = QTransform(0., 1., -1., 0., 0., 0.);
		const auto mirror = QTransform(-1., 0., 0., 1., 0., 0.);
		const auto zoomed = QTransform(0., 1., -1., 0., 395., 0.)
			* QTransform::fromScale(0.2, 0.2)
			* QTransform::fromTranslate(30., 40.);
		const auto tilted = QTransform().rotate(30.)
			* QTransform::fromScale(2., 2.);
		check(
			ViewOrientation(none).isIdentity()
				&& sameTurn(ViewOrientation(zoomed), quarter)
				&& sameTurn(
					ViewOrientation(QTransform(-1., 0., 0., 1., 654., 0.)),
					mirror)
				&& sameTurn(ViewOrientation(tilted), QTransform().rotate(30.))
				&& ViewOrientation(
					QTransform(0., 0., 0., 0., 5., 5.)).isIdentity(),
			u"view: the turn and the mirror without the zoom and the shift"_q);
		check(
			sameTurn(QuarterOrientation(tilted), none)
				&& sameTurn(QuarterOrientation(zoomed), quarter)
				&& sameTurn(QuarterOrientation(mirror), mirror)
				&& sameTurn(
					QuarterOrientation(QTransform().rotate(80.)),
					quarter),
			u"view: a straightened picture counts as the nearest quarter"_q);

		const auto step = [&](
				const QTransform &view,
				double x,
				double y,
				double size = 1.,
				double scale = 1.) {
			return SeenStep(view, QPointF(x, y), size, scale);
		};
		check(
			samePoint(step(none, -1., 0.), QPointF(-1., 0.))
				&& samePoint(step(none, 0., 1., 10., 3.), QPointF(0., 10.))
				&& samePoint(step(quarter, -1., 0.), QPointF(0., 1.))
				&& samePoint(step(quarter, 0., -1.), QPointF(-1., 0.))
				&& samePoint(step(zoomed, 1., 0.), QPointF(0., -1.))
				&& samePoint(step(mirror, -1., 0.), QPointF(1., 0.))
				&& samePoint(step(mirror, 0., 1.), QPointF(0., 1.)),
			u"view: an arrow key moves the layer where the arrow points"_q);
		check(
			samePoint(step(none, 1., 0., 1., 0.2), QPointF(5., 0.))
				&& samePoint(step(none, 1., 0., 10., 0.3), QPointF(33., 0.))
				&& samePoint(step(quarter, 0., 1., 1., 0.25), QPointF(4., 0.))
				&& samePoint(step(none, 1., 0., 1., 0.), QPointF(1., 0.))
				&& samePoint(step(none, 0., 0.), QPointF())
				&& samePoint(step(none, 1., 0., 0.), QPointF()),
			u"view: a step is at least a pixel of the screen"_q);
		const auto slanted = step(tilted, 1., 0., 1., 2.);
		check(
			about(Length(slanted), 1.)
				&& samePoint(
					ViewOrientation(tilted).map(slanted),
					QPointF(1., 0.)),
			u"view: a straightened picture keeps the screen directions"_q);

		const auto before = quadOf(turned);
		const auto flipped = FlippedTransform(turned, content, true, tilted);
		const auto after = quadOf(flipped);
		const auto middle = tilted.map(center);
		auto mirrored = true;
		for (auto i = 0; i != 4; ++i) {
			const auto was = tilted.map(before[i]);
			mirrored = mirrored && samePoint(
				tilted.map(after[i]),
				QPointF(2. * middle.x() - was.x(), was.y()),
				1e-6);
		}
		check(
			mirrored
				&& sameQuad(
					quadOf(FlippedTransform(flipped, content, true, tilted)),
					before,
					1e-6)
				&& sameQuad(
					quadOf(FlippedTransform(turned, content, true, quarter)),
					quadOf(FlippedTransform(turned, content, false)))
				&& sameQuad(
					quadOf(FlippedTransform(turned, content, false, zoomed)),
					quadOf(FlippedTransform(turned, content, true)))
				&& sameQuad(
					quadOf(FlippedTransform(turned, content, true, mirror)),
					quadOf(FlippedTransform(turned, content, true)))
				&& sameQuad(
					quadOf(FlippedTransform(turned, content, true, none)),
					quadOf(FlippedTransform(turned, content, true))),
			u"view: a flip mirrors the layer as it is seen"_q);
		check(
			about(SeenTurn(none, 90.), 90.)
				&& about(SeenTurn(quarter, -90.), -90.)
				&& about(SeenTurn(tilted, 90.), 90.)
				&& about(SeenTurn(mirror, 90.), -90.)
				&& about(SeenTurn(mirror * quarter, -90.), 90.),
			u"view: a mirrored picture turns the other way"_q);

		const auto canvas = QSizeF(654., 395.);
		const auto point = QPointF(400., 100.);
		check(
			samePoint(SeenShift(none, point, canvas), QPointF(73., -97.5))
				&& samePoint(
					SeenShift(quarter, point, canvas),
					QPointF(97.5, 73.))
				&& samePoint(
					SeenShift(mirror, point, canvas),
					QPointF(-73., -97.5))
				&& samePoint(
					PointFromSeenShift(
						quarter,
						SeenShift(quarter, point, canvas),
						canvas),
					point)
				&& samePoint(
					PointFromSeenShift(
						tilted,
						SeenShift(tilted, point, canvas),
						canvas),
					point)
				&& samePoint(
					PointFromSeenShift(quarter, QPointF(10., 0.), canvas),
					QPointF(327., 187.5)),
			u"view: the shift of a layer is told as it is seen"_q);

		// The same against the real geometry of a document.
		auto arrows = true;
		auto flips = true;
		auto shifts = true;
		auto document = Document();
		document.size = QSize(654, 395);
		const auto directions = {
			QPointF(-1., 0.),
			QPointF(1., 0.),
			QPointF(0., -1.),
			QPointF(0., 1.),
		};
		for (auto variant = 0; variant != 32; ++variant) {
			document.global.quarterTurns = variant % 4;
			document.global.flipHorizontal = ((variant / 4) % 2) != 0;
			document.global.flipVertical = ((variant / 8) % 2) != 0;
			document.global.straighten = (variant / 16) ? -12.5 : 0.;
			const auto output = OutputTransform(document, QSize());
			const auto seen = ViewOrientation(output);
			const auto onScreen = [&](QPointF canvasVector) {
				return output.map(canvasVector) - output.map(QPointF());
			};
			for (const auto &direction : directions) {
				const auto moved = onScreen(
					step(seen, direction.x(), direction.y()));
				arrows = arrows
					&& about(Cross(moved, direction), 0., 1e-6)
					&& (Dot(moved, direction) > 0.5);
			}
			const auto flippedQuad = quadOf(
				FlippedTransform(turned, content, true, seen));
			const auto axis = output.map(center);
			for (auto i = 0; i != 4; ++i) {
				const auto was = output.map(before[i]);
				flips = flips && samePoint(
					output.map(flippedQuad[i]),
					QPointF(2. * axis.x() - was.x(), was.y()),
					1e-6);
			}
			const auto right = PointFromSeenShift(
				seen,
				SeenShift(seen, point, canvas) + QPointF(7., 0.),
				canvas);
			const auto shifted = onScreen(right - point);
			shifts = shifts
				&& about(Cross(shifted, QPointF(1., 0.)), 0., 1e-6)
				&& (shifted.x() > 6.9);
		}
		check(arrows, u"view: arrows are right in every turned picture"_q);
		check(flips, u"view: flips are right in every turned picture"_q);
		check(shifts, u"view: shifts are right in every turned picture"_q);
	}

	// A free corner and a skewed edge.
	{
		const auto moved = CornerMovedTransform(
			base,
			content,
			2,
			QPointF(350., 260.));
		check(
			moved
				&& IsPerspective(*moved)
				&& ValidTransform(*moved, content)
				&& sameQuad(
					quadOf(*moved),
					QPolygonF({
						QPointF(100., 100.),
						QPointF(300., 100.),
						QPointF(350., 260.),
						QPointF(100., 200.),
					}),
					1e-5),
			u"perspective: one corner moves, three stay"_q);
		check(
			!CornerMovedTransform(base, content, 2, QPointF(50., 50.))
				&& !CornerMovedTransform(base, content, 2, QPointF(300., 100.2))
				&& !CornerMovedTransform(base, content, 5, QPointF(350., 260.)),
			u"perspective: a folded or collapsed frame is refused"_q);
		if (moved) {
			auto layer = MakeImageLayer(
				SolidImage(200, 100, QColor(255, 0, 0)),
				QString());
			layer.transform = *moved;
			const auto local = QPointF(50., 25.);
			const auto back = LayerPoint(layer, moved->map(local));
			const auto again = CornerMovedTransform(
				*moved,
				content,
				2,
				QPointF(300., 200.));
			check(
				back
					&& samePoint(*back, local, 1e-6)
					&& sameQuad(quadOf(*moved), LayerQuad(layer), 1e-9)
					&& (LocalScaleAt(*moved, local) > 0.)
					&& again
					&& sameQuad(quadOf(*again), quadOf(base), 1e-5),
				u"perspective: points map back, the corner can return"_q);
		}
		const auto skewed = EdgeSkewedTransform(
			base,
			content,
			Handle::Top,
			QPointF(40., 77.));
		check(
			skewed
				&& skewed->isAffine()
				&& sameQuad(
					quadOf(*skewed),
					QPolygonF({
						QPointF(140., 100.),
						QPointF(340., 100.),
						QPointF(300., 200.),
						QPointF(100., 200.),
					}),
					1e-6)
				&& !EdgeSkewedTransform(
					base,
					content,
					Handle::TopLeft,
					QPointF(40., 0.)),
			u"skew: an edge slides along itself"_q);
		check(
			about(LocalScaleAt(base, QPointF(10., 10.)), 1.)
				&& about(
					LocalScaleAt(
						LocalScaled(base, QPointF(), 2., 2.),
						QPointF(10., 10.)),
					2.)
				&& about(
					LocalScaleAt(
						LocalScaled(turned, QPointF(), 4., 1.),
						QPointF(10., 10.)),
					2.),
			u"transform: the local scale is the root of the area scale"_q);
	}

	// Fit, fill, center, reset, validity.
	{
		const auto canvas = QSizeF(800., 600.);
		const auto fitted = FittedTransform(turned, content, canvas, false);
		const auto fitBounds = quadOf(fitted).boundingRect();
		check(
			samePoint(fitBounds.center(), QPointF(400., 300.))
				&& (fitBounds.width() <= 800. + 1e-6)
				&& (fitBounds.height() <= 600. + 1e-6)
				&& (about(fitBounds.width(), 800.)
					|| about(fitBounds.height(), 600.))
				&& about(DecomposeTransform(fitted, content).rotation, 30.),
			u"fit: the turned layer fits the canvas and keeps its turn"_q);
		const auto filled = FittedTransform(turned, content, canvas, true);
		const auto fillBounds = quadOf(filled).boundingRect();
		check(
			samePoint(fillBounds.center(), QPointF(400., 300.))
				&& (fillBounds.width() >= 800. - 1e-6)
				&& (fillBounds.height() >= 600. - 1e-6)
				&& (about(fillBounds.width(), 800.)
					|| about(fillBounds.height(), 600.)),
			u"fill: the layer covers the canvas"_q);
		check(
			sameQuad(
				quadOf(CenteredTransform(base, content, canvas)),
				rectQuad(300., 250., 200., 100.)),
			u"center: the layer goes to the middle of the canvas"_q);
		check(
			DefaultTransform(QSize(800, 600), QSize(800, 600)).isIdentity()
				&& sameQuad(
					TransformedQuad(
						DefaultTransform(QSize(1600, 1200), QSize(800, 600)),
						QSizeF(1600., 1200.)),
					rectQuad(0., 0., 800., 600.))
				&& sameQuad(
					TransformedQuad(
						DefaultTransform(QSize(200, 100), QSize(800, 600)),
						content),
					rectQuad(300., 250., 200., 100.)),
			u"reset: a layer fits the canvas or sits in its middle"_q);
		check(
			IsAxisAligned(base)
				&& IsAxisAligned(RotatedTransform(base, center, 90.))
				&& IsAxisAligned(FlippedTransform(base, content, true))
				&& !IsAxisAligned(turned)
				&& !IsAxisAligned(
					*EdgeSkewedTransform(
						base,
						content,
						Handle::Top,
						QPointF(40., 0.))),
			u"transform: axis aligned layers are recognized"_q);
		check(
			ValidTransform(base, content)
				&& ValidTransform(turned, content)
				&& !ValidTransform(QTransform(0., 0., 0., 0., 0., 0.), content)
				&& !ValidTransform(QTransform::fromScale(1e9, 1e9), content)
				&& !ValidTransform(base, QSizeF())
				&& !ValidTransform(
					QTransform(
						1., 0., std::nan(""),
						0., 1., 0.,
						0., 0., 1.),
					content)
				&& !ValidTransform(
					QTransform(1., 0., -0.01, 0., 1., 0., 0., 0., 1.),
					content),
			u"transform: broken transforms are refused"_q);
	}

	// The numbers of the fields.
	{
		const auto parts = TransformParts{
			.center = QPointF(320., 240.),
			.scaleX = 1.5,
			.scaleY = 0.75,
			.rotation = 25.,
			.shear = 0.2,
		};
		const auto composed = ComposeTransform(parts, content);
		const auto numbers = NumbersFromTransform(composed, content);
		check(
			about(numbers.x, 320.)
				&& about(numbers.y, 240.)
				&& about(numbers.width, 150.)
				&& about(numbers.height, 75.)
				&& about(numbers.rotation, 25.)
				&& about(numbers.skew, std::atan(0.2) * 180. / kPi)
				&& !numbers.flippedX
				&& !numbers.flippedY,
			u"numbers: position, size, turn and skew are read"_q);
		check(
			sameQuad(
				quadOf(TransformFromNumbers(numbers, content)),
				quadOf(composed)),
			u"numbers: the transform is built back"_q);
		auto changed = numbers;
		changed.rotation = 40.;
		changed.width = 200.;
		const auto rebuilt = DecomposeTransform(
			TransformFromNumbers(changed, content),
			content);
		check(
			about(rebuilt.rotation, 40.)
				&& about(rebuilt.scaleX, 2.)
				&& about(rebuilt.scaleY, 0.75)
				&& about(rebuilt.shear, 0.2)
				&& samePoint(rebuilt.center, QPointF(320., 240.)),
			u"numbers: one field changes one thing"_q);

		const auto flippedX = FlippedTransform(base, content, true);
		const auto flippedY = FlippedTransform(base, content, false);
		const auto numbersX = NumbersFromTransform(flippedX, content);
		const auto numbersY = NumbersFromTransform(flippedY, content);
		const auto half = NumbersFromTransform(
			RotatedTransform(base, center, 180.),
			content);
		check(
			numbersX.flippedX
				&& !numbersX.flippedY
				&& about(numbersX.rotation, 0.)
				&& about(numbersX.width, 100.)
				&& numbersY.flippedY
				&& !numbersY.flippedX
				&& about(numbersY.rotation, 0.)
				&& !half.flippedX
				&& !half.flippedY
				&& about(half.rotation, 180.),
			u"numbers: a mirrored layer is one flip, not a turn"_q);
		check(
			sameQuad(
				quadOf(TransformFromNumbers(numbersX, content)),
				quadOf(flippedX))
				&& sameQuad(
					quadOf(TransformFromNumbers(numbersY, content)),
					quadOf(flippedY))
				&& sameQuad(
					quadOf(TransformFromNumbers(
						NumbersFromTransform(
							FlippedTransform(turned, content, true),
							content),
						content)),
					quadOf(FlippedTransform(turned, content, true))),
			u"numbers: flips survive the round trip"_q);

		const auto linked = LinkPercents(100., 50., 200., 1., 1000.);
		const auto capped = LinkPercents(100., 800., 200., 1., 1000.);
		const auto lowest = LinkPercents(100., 4., 10., 1., 1000.);
		const auto beyond = LinkPercents(100., 2000., 150., 1., 1000.);
		const auto broken = LinkPercents(100., 50., std::nan(""), 1., 1000.);
		check(
			about(linked.changed, 200.)
				&& about(linked.other, 100.)
				&& about(capped.changed, 125.)
				&& about(capped.other, 1000.)
				&& about(lowest.changed, 25.)
				&& about(lowest.other, 1.)
				&& about(beyond.changed, 100.)
				&& about(beyond.other, 2000.)
				&& about(LinkPercents(100., 2000., 50., 1., 1000.).other, 1000.)
				&& about(broken.changed, 100.)
				&& about(broken.other, 50.),
			u"numbers: linked sides keep the proportions at the limits"_q);

		// The Rotation slider of a mirrored layer dragged past a quarter
		// of a turn, a degree at a step.
		auto smooth = true;
		auto jumped = false;
		for (const auto keep : { true, false }) {
			auto transform = TransformFromNumbers(
				WithRotation(numbersY, 85.),
				content);
			auto flips = std::optional<LayerFlips>();
			auto previous = DecomposeTransform(transform, content).rotation;
			for (auto degrees = 86.; degrees < 96.5; degrees += 1.) {
				const auto next = WithRotation(
					NumbersFromTransform(transform, content),
					degrees,
					keep ? flips : std::optional<LayerFlips>());
				flips = LayerFlips{ .x = next.flippedX, .y = next.flippedY };
				transform = TransformFromNumbers(next, content);
				const auto now = DecomposeTransform(
					transform,
					content).rotation;
				const auto turn = std::abs(NormalizedAngle(now - previous));
				if (keep) {
					smooth = smooth && about(turn, 1., 1e-6);
				} else {
					jumped = jumped || (turn > 90.);
				}
				previous = now;
			}
		}
		check(
			smooth && jumped,
			u"numbers: a mirrored layer turns smoothly with its slider"_q);
	}

	// Snapping.
	{
		const auto targets = SnapTargets{
			.xs = { 0., 400., 800. },
			.ys = { 0., 300., 600. },
		};
		const auto corner = SnapRect(
			QRectF(3., 248., 100., 50.),
			targets,
			5.);
		check(
			samePoint(corner.delta, QPointF(-3., 2.))
				&& corner.guideX
				&& about(*corner.guideX, 0.)
				&& corner.guideY
				&& about(*corner.guideY, 300.),
			u"snap: an edge sticks to the canvas edge and center line"_q);
		const auto middle = SnapRect(
			QRectF(352., 100., 100., 100.),
			targets,
			5.);
		check(
			samePoint(middle.delta, QPointF(-2., 0.))
				&& middle.guideX
				&& about(*middle.guideX, 400.)
				&& !middle.guideY,
			u"snap: the center sticks to the canvas center"_q);
		const auto none = SnapRect(
			QRectF(120., 130., 100., 40.),
			targets,
			5.);
		check(
			samePoint(none.delta, QPointF())
				&& !none.guideX
				&& !none.guideY,
			u"snap: nothing sticks beyond the threshold"_q);
		const auto closest = SnapRect(
			QRectF(-4., 100., 403., 40.),
			targets,
			5.);
		check(
			samePoint(closest.delta, QPointF(1., 0.))
				&& closest.guideX
				&& about(*closest.guideX, 400.),
			u"snap: the closest line wins"_q);
		const auto onlyY = SnapRect(
			QRectF(3., 248., 100., 50.),
			targets,
			5.,
			false,
			true);
		check(
			samePoint(onlyY.delta, QPointF(0., 2.)) && !onlyY.guideX,
			u"snap: an axis can be left alone"_q);
		const auto point = SnapPoint(QPointF(398., 50.), targets, 5.);
		check(
			samePoint(point.delta, QPointF(2., 0.))
				&& point.guideX
				&& !point.guideY
				&& samePoint(
					SnapPoint(
						QPointF(398., 50.),
						targets,
						5.,
						false,
						true).delta,
					QPointF()),
			u"snap: a point sticks to lines"_q);

		auto document = Document();
		document.size = QSize(800, 600);
		const auto first = AddLayer(
			document,
			MakeImageLayer(SolidImage(8, 6, QColor(1, 2, 3)), u"a"_q));
		auto other = MakeImageLayer(SolidImage(200, 100, QColor(9, 9, 9)), u"b"_q);
		other.transform = base;
		const auto second = AddLayer(document, std::move(other));
		const auto withOther = MakeSnapTargets(document, first);
		const auto withoutOther = MakeSnapTargets(document, second);
		const auto has = [](const std::vector<double> &list, double value) {
			return ranges::contains(list, value);
		};
		check(
			has(withOther.xs, 0.)
				&& has(withOther.xs, 400.)
				&& has(withOther.xs, 800.)
				&& has(withOther.ys, 300.)
				&& has(withOther.xs, 100.)
				&& has(withOther.xs, 200.)
				&& has(withOther.xs, 300.)
				&& has(withOther.ys, 150.)
				&& !has(withoutOther.xs, 100.)
				&& !has(withoutOther.ys, 150.)
				&& has(withoutOther.xs, 8.),
			u"snap: lines come from the canvas and the other layers"_q);
	}

	// Which layer is under the cursor.
	{
		auto pixels = QImage(4, 4, QImage::Format_ARGB32_Premultiplied);
		pixels.fill(Qt::transparent);
		for (auto y = 0; y != 4; ++y) {
			const auto line = FxRow(pixels, y);
			line[2] = 0xFFFFFFFFU;
			line[3] = 0xFFFFFFFFU;
		}
		const auto map = AlphaMapFromPixels(pixels);
		check(
			(map.format() == QImage::Format_Alpha8)
				&& !AlphaMapHit(map, content, QPointF(10., 50.))
				&& AlphaMapHit(map, content, QPointF(150., 50.))
				&& AlphaMapHit(map, content, QPointF(90., 50.))
				&& AlphaMapHit(QImage(), content, QPointF(10., 50.)),
			u"layer hit: transparent parts are not hit"_q);

		auto document = Document();
		document.size = QSize(200, 100);
		const auto bottom = AddLayer(
			document,
			MakeImageLayer(SolidImage(200, 100, QColor(10, 20, 30)), u"a"_q));
		const auto top = AddLayer(
			document,
			MakeImageLayer(SolidImage(200, 100, QColor(40, 50, 60)), u"b"_q));
		const auto maps = [&](LayerId id) -> const QImage* {
			return (id == top) ? &map : nullptr;
		};
		const auto leftPoint = QPointF(10., 50.);
		const auto rightPoint = QPointF(150., 50.);
		check(
			(LayerAtPixels(document, leftPoint, maps) == bottom)
				&& (LayerAtPixels(document, rightPoint, maps) == top)
				&& (LayerAtPixels(document, leftPoint, nullptr) == top)
				&& (LayerAtPixels(document, QPointF(-5., 50.), maps) == 0),
			u"layer hit: a click goes through transparent pixels"_q);
		auto locked = document;
		locked.find(top)->locked = true;
		auto hidden = document;
		hidden.find(top)->visible = false;
		auto moved = document;
		moved.find(top)->transform = QTransform::fromTranslate(100., 0.);
		check(
			(LayerAtPixels(locked, rightPoint, maps) == bottom)
				&& (LayerAtPixels(locked, rightPoint, maps, true) == top)
				&& (LayerAtPixels(hidden, rightPoint, maps) == bottom)
				&& (LayerAtPixels(moved, QPointF(120., 50.), maps) == bottom)
				&& (LayerAtPixels(moved, QPointF(260., 50.), maps) == top)
				&& (LayerAtPixels(moved, QPointF(50., 50.), maps) == bottom),
			u"layer hit: locked, hidden and moved layers"_q);
	}

	// The rows of the layers list.
	{
		check(
			(LayerRowToIndex(3, 0) == 2)
				&& (LayerRowToIndex(3, 2) == 0)
				&& (LayerRowToIndex(3, 3) == -1)
				&& (LayerRowToIndex(3, -1) == -1)
				&& (LayerIndexToRow(3, 0) == 2)
				&& (LayerRowToIndex(0, 0) == -1),
			u"rows: the first row is the top layer"_q);
		check(
			(LayerDropIndex(3, 0, 0) == -1)
				&& (LayerDropIndex(3, 0, 1) == -1)
				&& (LayerDropIndex(3, 0, 2) == 1)
				&& (LayerDropIndex(3, 0, 3) == 0)
				&& (LayerDropIndex(3, 2, 0) == 2)
				&& (LayerDropIndex(3, 2, 3) == -1)
				&& (LayerDropIndex(3, 1, 99) == 0)
				&& (LayerDropIndex(3, 5, 0) == -1)
				&& (LayerDropIndex(0, 0, 0) == -1),
			u"rows: a drop gives the new index or nothing"_q);

		auto matches = true;
		auto idsKept = true;
		for (auto count = 1; count != 6; ++count) {
			auto document = Document();
			document.size = QSize(2, 2);
			for (auto i = 0; i != count; ++i) {
				AddLayer(
					document,
					MakeImageLayer(
						SolidImage(2, 2, QColor(i * 40, 0, 0)),
						QString::number(i)));
			}
			const auto rows = [](const Document &document) {
				auto result = std::vector<LayerId>();
				for (auto i = int(document.layers.size()); i != 0;) {
					result.push_back(document.layers[--i].id);
				}
				return result;
			};
			const auto before = rows(document);
			for (auto from = 0; from != count; ++from) {
				for (auto gap = 0; gap <= count; ++gap) {
					auto expected = before;
					const auto id = expected[from];
					expected.erase(begin(expected) + from);
					expected.insert(
						begin(expected) + ((gap > from) ? (gap - 1) : gap),
						id);
					auto copy = document;
					const auto index = LayerDropIndex(count, from, gap);
					const auto moved = (index >= 0)
						&& MoveLayer(copy, id, index);
					matches = matches
						&& (rows(copy) == expected)
						&& (moved == (expected != before));
					idsKept = idsKept
						&& (copy.find(id) != nullptr)
						&& (copy.find(id)->name == document.find(id)->name)
						&& (int(copy.layers.size()) == count);
				}
			}
		}
		check(matches, u"reorder: every drop gives the order of the list"_q);
		check(idsKept, u"reorder: layers keep their ids and names"_q);

		auto document = Document();
		document.size = QSize(2, 2);
		const auto a = AddLayer(
			document,
			MakeImageLayer(SolidImage(2, 2, QColor(1, 1, 1)), u"a"_q));
		const auto b = AddLayer(
			document,
			MakeImageLayer(SolidImage(2, 2, QColor(2, 2, 2)), u"b"_q));
		const auto c = AddLayer(
			document,
			MakeImageLayer(SolidImage(2, 2, QColor(3, 3, 3)), u"c"_q));
		auto history = History(document);
		auto moved = document;
		MoveLayer(moved, c, LayerDropIndex(3, 0, 3));
		const auto pushed = history.push(moved);
		const auto order = (moved.layers[0].id == c)
			&& (moved.layers[1].id == a)
			&& (moved.layers[2].id == b);
		const auto undone = history.undo() && (history.current() == document);
		const auto redone = history.redo() && (history.current() == moved);
		check(
			pushed && order && undone && redone,
			u"reorder: one undo step, undone and redone"_q);

		auto copy = document;
		const auto duplicate = DuplicateLayer(copy, a);
		check(
			duplicate
				&& (copy.layers.size() == 4)
				&& (copy.layers[1].id == duplicate)
				&& (copy.layers[1].content == copy.layers[0].content)
				&& RemoveLayer(copy, duplicate)
				&& (copy == document),
			u"layers: a duplicate goes above and shares the pixels"_q);
		check(
			!CanMergeDown(document, a)
				&& CanMergeDown(document, b)
				&& CanMergeDown(document, c)
				&& !CanMergeDown(document, 999),
			u"merge: only a layer with one under it"_q);
		auto locked = document;
		locked.find(a)->locked = true;
		check(
			!CanMergeDown(locked, b) && CanMergeDown(locked, c),
			u"merge: locked layers are not merged"_q);
	}

	// Merge down and flatten on tiny pictures.
	{
		const auto flat = [](const Document &document) {
			auto compositor = Compositor();
			return compositor.render(document, {
				.scale = 1.,
				.global = false,
				.cache = false,
			});
		};
		auto document = Document();
		document.size = QSize(8, 6);
		const auto bottom = AddLayer(
			document,
			MakeImageLayer(SolidImage(8, 6, QColor(40, 80, 120)), u"bottom"_q));
		auto upper = MakeImageLayer(
			SolidImage(4, 4, QColor(200, 50, 50)),
			u"upper"_q);
		upper.transform = QTransform::fromTranslate(2., 1.);
		upper.opacity = 0.5;
		upper.blend = BlendMode::Multiply;
		auto mask = QImage(4, 4, QImage::Format_Grayscale8);
		mask.fill(255);
		for (auto y = 0; y != 4; ++y) {
			mask.scanLine(y)[0] = 0;
			mask.scanLine(y)[1] = 128;
		}
		upper.mask = MakeMask(std::move(mask));
		const auto upperId = AddLayer(document, std::move(upper));

		const auto reference = flat(document);
		const auto merged = MergedDown(document, upperId);
		check(
			!reference.isNull()
				&& (reference.pixel(0, 0) == qRgba(40, 80, 120, 255))
				&& (reference.pixel(2, 2) == qRgba(40, 80, 120, 255))
				&& (reference.pixel(5, 2) != qRgba(40, 80, 120, 255)),
			u"merge: the sample is blended, masked and placed"_q);
		check(
			merged
				&& (merged->layers.size() == 1)
				&& (merged->layers[0].id == bottom)
				&& (merged->layers[0].name == u"bottom"_q)
				&& (merged->layers[0].blend == BlendMode::Normal)
				&& merged->layers[0].transform.isIdentity()
				&& !merged->layers[0].mask
				&& merged->layers[0].effects.empty()
				&& (merged->layers[0].size() == document.size)
				&& (merged->size == document.size),
			u"merge down: one canvas sized layer with the lower id"_q);
		check(
			merged && (MaxDifference(flat(*merged), reference) <= 1),
			u"merge down: the picture stays the same"_q);
		const auto same = MergedDown(document, bottom);
		check(
			same && (*same == document),
			u"merge down: nothing under the bottom layer"_q);

		auto top = MakeImageLayer(
			SolidImage(3, 2, QColor(10, 200, 30, 180)),
			u"top"_q);
		top.transform = QTransform::fromTranslate(4., 3.);
		top.blend = BlendMode::Screen;
		auto three = document;
		const auto topId = AddLayer(three, std::move(top));
		three.global.contrast = 10;
		const auto threeReference = flat(three);
		const auto partly = MergedDown(three, topId);
		check(
			partly
				&& (partly->layers.size() == 2)
				&& (partly->layers[0].id == bottom)
				&& (partly->layers[0].content == three.layers[0].content)
				&& (partly->layers[1].id == upperId)
				&& (partly->layers[1].blend == BlendMode::Multiply)
				&& about(partly->layers[1].opacity, 0.5)
				&& AsImage(partly->layers[1].content)
				&& (partly->global == three.global),
			u"merge down: the lower layer keeps its blending"_q);

		const auto flattened = Flattened(three);
		check(
			flattened
				&& (flattened->layers.size() == 1)
				&& (flattened->layers[0].id == bottom)
				&& (flattened->layers[0].name == u"bottom"_q)
				&& (flattened->layers[0].size() == three.size)
				&& (flattened->global == three.global)
				&& IsPlainImage(*flattened),
			u"flatten: one plain layer, the whole-image edit stays"_q);
		check(
			flattened
				&& (MaxDifference(flat(*flattened), threeReference) <= 1),
			u"flatten: the picture stays the same"_q);

		auto hidden = three;
		hidden.find(upperId)->visible = false;
		const auto hiddenFlat = Flattened(hidden);
		check(
			hiddenFlat
				&& (hiddenFlat->layers.size() == 1)
				&& (MaxDifference(flat(*hiddenFlat), flat(hidden)) <= 1)
				&& (MaxDifference(flat(*hiddenFlat), threeReference) > 1),
			u"flatten: hidden layers are dropped"_q);

		auto history = History(three);
		const auto step = flattened && history.push(*flattened);
		check(
			step && history.undo() && (history.current() == three),
			u"flatten: undo brings the layers back"_q);
	}

	// The thumbnails of the list.
	{
		auto layer = MakeImageLayer(SolidImage(2, 2, QColor(5, 5, 5)), u"a"_q);
		const auto key = LayerThumbnailKey(layer);
		auto same = layer;
		same.name = u"b"_q;
		same.visible = false;
		same.locked = true;
		same.opacity = 0.3;
		same.blend = BlendMode::Screen;
		same.transform = QTransform::fromTranslate(5., 5.);
		auto masked = layer;
		masked.mask = MakeMask(QSize(2, 2), 100);
		auto disabled = masked;
		disabled.maskEnabled = false;
		auto repainted = masked;
		repainted.mask = MakeMask(QSize(2, 2), 100);
		auto effect = layer;
		effect.effects.push_back(MakeFx("classic.invert"));
		auto other = MakeImageLayer(SolidImage(2, 2, QColor(5, 5, 5)), u"a"_q);
		check(
			(LayerThumbnailKey(same) == key)
				&& (LayerThumbnailKey(masked) != key)
				&& (LayerThumbnailKey(disabled) == key)
				&& (LayerThumbnailKey(repainted) != LayerThumbnailKey(masked))
				&& (LayerThumbnailKey(effect) != key)
				&& (LayerThumbnailKey(other) != key),
			u"thumbnails: redrawn only when the picture of a layer changes"_q);
	}

	// The mask brush.
	{
		const auto plane = [] {
			auto result = QImage(64, 64, QImage::Format_Alpha8);
			result.fill(0);
			return result;
		};
		const auto at = [](const QImage &image, int x, int y) {
			return int(image.constScanLine(y)[x]);
		};
		const auto dot = QPointF(32., 32.);

		auto hard = plane();
		const auto hardRect = MaskStampSegment(hard, dot, dot, 10., 10., 1.);
		check(
			(at(hard, 32, 32) == 255)
				&& (at(hard, 32, 24) == 255)
				&& (at(hard, 39, 39) == 0)
				&& (at(hard, 32, 45) == 0)
				&& (at(hard, 5, 5) == 0)
				&& hardRect.contains(QPoint(32, 32))
				&& hardRect.contains(QPoint(22, 41))
				&& hard.rect().contains(hardRect)
				&& (hardRect.width() <= 24),
			u"mask brush: a hard dot is filled, the rest is untouched"_q);
		auto outside = true;
		for (auto y = 0; y != 64; ++y) {
			for (auto x = 0; x != 64; ++x) {
				if (!hardRect.contains(QPoint(x, y)) && at(hard, x, y) != 0) {
					outside = false;
				}
			}
		}
		check(outside, u"mask brush: nothing changes outside of the rect"_q);

		auto soft = plane();
		MaskStampSegment(soft, dot, dot, 10., 10., 0.);
		auto falling = (at(soft, 32, 32) >= 240) && (at(soft, 43, 32) == 0);
		for (auto x = 33; x != 44; ++x) {
			falling = falling && (at(soft, x, 32) <= at(soft, x - 1, 32));
		}
		check(
			falling
				&& (at(soft, 37, 32) > 60)
				&& (at(soft, 37, 32) < 200)
				&& (at(hard, 37, 32) == 255),
			u"mask brush: a soft dot fades to its edge"_q);

		auto line = plane();
		MaskStampSegment(
			line,
			QPointF(10., 32.),
			QPointF(50., 32.),
			4.,
			4.,
			1.);
		check(
			(at(line, 30, 32) == 255)
				&& (at(line, 30, 29) == 255)
				&& (at(line, 30, 40) == 0)
				&& (at(line, 8, 32) == 255)
				&& (at(line, 2, 32) == 0)
				&& (at(line, 52, 32) == 255)
				&& (at(line, 58, 32) == 0),
			u"mask brush: a segment is a capsule"_q);

		auto cone = plane();
		MaskStampSegment(
			cone,
			QPointF(10., 32.),
			QPointF(50., 32.),
			2.,
			10.,
			1.);
		check(
			(at(cone, 12, 38) == 0) && (at(cone, 48, 38) == 255),
			u"mask brush: the radius changes along a segment"_q);

		auto twice = line;
		MaskStampSegment(
			twice,
			QPointF(10., 32.),
			QPointF(50., 32.),
			4.,
			4.,
			1.);
		auto softTwice = soft;
		MaskStampSegment(softTwice, dot, dot, 10., 10., 0.);
		check(
			(twice == line) && (softTwice == soft),
			u"mask brush: overlapping segments of a stroke don't add up"_q);
		auto empty = plane();
		check(
			MaskStampSegment(
				empty,
				QPointF(-100., -100.),
				QPointF(-90., -90.),
				5.,
				5.,
				1.).isEmpty()
				&& MaskStampSegment(
					empty,
					QPointF(1e12, 1e12),
					QPointF(1e12, 1e12),
					5.,
					5.,
					1.).isEmpty()
				&& (empty == plane()),
			u"mask brush: a stroke outside of the layer changes nothing"_q);
		auto big = QImage(600, 400, QImage::Format_Alpha8);
		big.fill(0);
		auto bigAgain = big;
		const auto bigRect = MaskStampSegment(
			big,
			QPointF(100., 100.),
			QPointF(500., 300.),
			150.,
			120.,
			0.5);
		MaskStampSegment(
			bigAgain,
			QPointF(100., 100.),
			QPointF(500., 300.),
			150.,
			120.,
			0.5);
		check(
			(big == bigAgain)
				&& (bigRect == big.rect())
				&& (at(big, 300, 200) == 255)
				&& (at(big, 599, 0) == 0),
			u"mask brush: a big brush gives the same pixels every time"_q);

		auto base = QImage(64, 64, QImage::Format_Grayscale8);
		base.fill(255);
		auto mask = base.copy();
		MaskComposeStroke(mask, base, hard, hardRect, 0, 1.);
		check(
			(at(mask, 32, 32) == 0)
				&& (at(mask, 5, 5) == 255)
				&& (at(mask, 32, 45) == 255),
			u"mask stroke: hiding paints black under the brush"_q);
		auto halfMask = base.copy();
		MaskComposeStroke(halfMask, base, hard, hardRect, 0, 0.5);
		check(
			(std::abs(at(halfMask, 32, 32) - 128) <= 1)
				&& (at(halfMask, 5, 5) == 255),
			u"mask stroke: the strength limits one stroke"_q);
		// Painting again while the stroke lasts starts from the base.
		MaskComposeStroke(halfMask, base, hard, hardRect, 0, 0.5);
		check(
			std::abs(at(halfMask, 32, 32) - 128) <= 1,
			u"mask stroke: redoing a stroke doesn't darken it"_q);
		auto shown = mask.copy();
		MaskComposeStroke(shown, mask, hard, hardRect, 255, 1.);
		auto brighter = true;
		for (auto y = 0; y != 64; ++y) {
			for (auto x = 0; x != 64; ++x) {
				brighter = brighter && (at(shown, x, y) >= at(mask, x, y));
			}
		}
		check(
			brighter
				&& (at(shown, 32, 32) == 255)
				&& (at(shown, 27, 29) == 255)
				&& (at(shown, 5, 5) == 255),
			u"mask stroke: revealing brings the layer back"_q);
		auto partial = base.copy();
		MaskComposeStroke(
			partial,
			base,
			hard,
			QRect(0, 0, 32, 64),
			0,
			1.);
		check(
			(at(partial, 28, 32) == 0) && (at(partial, 34, 32) == 255),
			u"mask stroke: only the given rectangle is touched"_q);

		const auto inverted = InvertedMask(mask);
		check(
			(at(inverted, 32, 32) == 255)
				&& (at(inverted, 5, 5) == 0)
				&& (InvertedMask(inverted) == mask)
				&& InvertedMask(QImage()).isNull(),
			u"mask: inverting swaps hidden and shown"_q);

		// Premultiplied pixels as they are stored.
		const auto stored = [](const QImage &image, int x, int y) {
			return FxRow(image, y)[x];
		};
		const auto tint = uint32(qRgba(128, 0, 0, 128));
		auto overlay = QImage();
		UpdateMaskOverlay(overlay, mask, QRect(), QColor(255, 0, 0, 128));
		const auto red = (overlay.size() == mask.size())
			&& (stored(overlay, 32, 32) == tint)
			&& (stored(overlay, 5, 5) == 0);
		UpdateMaskOverlay(
			overlay,
			inverted,
			QRect(0, 0, 8, 8),
			QColor(255, 0, 0, 128));
		check(
			red
				&& (stored(overlay, 5, 5) == tint)
				&& (stored(overlay, 32, 32) == tint)
				&& (stored(overlay, 20, 20) == 0),
			u"mask overlay: hidden parts are tinted, updates are partial"_q);

		check(
			samePoint(
				MaskPoint(content, QSize(100, 50), QPointF(50., 20.)),
				QPointF(25., 10.))
				&& about(MaskScale(content, QSize(100, 50)), 0.5)
				&& about(MaskScale(QSizeF(), QSize(100, 50)), 1.),
			u"mask: layer points map to mask pixels"_q);
	}

	return ok;
}

const auto SelfTest = SelfTestRegistrar(
	SelfTestSuite::Doc,
	"layers",
	&RunLayersSelfTest);

} // namespace
} // namespace Oblivion::Photo
