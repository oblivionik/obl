/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_photo_fx_distort.h"

#include "base/weak_ptr.h"
#include "oblivion/oblivion_photo_editor_controls.h"
#include "oblivion/oblivion_photo_panels.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/effects/animation_value.h"
#include "ui/effects/ripple_animation.h"
#include "ui/painter.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_basic.h"
#include "styles/style_calls.h"

#include <QtCore/QElapsedTimer>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Oblivion::Photo {
namespace {

using Pixel = uint32;

constexpr auto kPi = 3.14159265358979323846;
constexpr auto kCoordinateLimit = 1e7f;
constexpr auto kSingleSampleStretch = 1.5625f;
constexpr auto kBrokenStretch = 1e10f;
constexpr auto kMinDepth = 1e-6;
constexpr auto kKeystoneStrength = 0.3;
constexpr auto kLensStrength = 1.2;
constexpr auto kLensTableSize = 1024;
constexpr auto kLensTableRange = 8.;
constexpr auto kLensSolveSteps = 16;
constexpr auto kBulgeStrength = 0.9f;
constexpr auto kPinchStrength = 1.5f;
constexpr auto kMeshMapSide = 512;
constexpr auto kMeshFineCells = 32;
constexpr auto kMeshOutside = 2.;
constexpr auto kMeshPointMin = -0.5;
constexpr auto kMeshPointMax = 1.5;
constexpr auto kMeshDefaultSide = 4;
constexpr auto kMeshRegularEpsilon = 1e-5;
constexpr auto kMinCornersArea = 0.002;

const auto kMeshType = QByteArray("distort.mesh");
const auto kCornersType = QByteArray("distort.corners");

struct Source {
	const Pixel *data = nullptr;
	int width = 0;
	int height = 0;
	ptrdiff_t stride = 0;

	[[nodiscard]] const Pixel *row(int y) const {
		return data + y * stride;
	}
};

[[nodiscard]] bool Cancelled(const std::atomic<bool> *cancel) {
	return cancel && cancel->load(std::memory_order_relaxed);
}

[[nodiscard]] inline Pixel Blend256(Pixel x, Pixel y, uint32 b) {
	const auto a = 256U - b;
	auto low = (x & 0x00FF00FFU) * a + (y & 0x00FF00FFU) * b + 0x00800080U;
	low = (low >> 8) & 0x00FF00FFU;
	auto high = ((x >> 8) & 0x00FF00FFU) * a
		+ ((y >> 8) & 0x00FF00FFU) * b
		+ 0x00800080U;
	high &= 0xFF00FF00U;
	return high | low;
}

[[nodiscard]] inline Pixel SampleStretch(
		const Source &source,
		float x,
		float y) {
	x = (x > 0.f) ? std::min(x, float(source.width - 1)) : 0.f;
	y = (y > 0.f) ? std::min(y, float(source.height - 1)) : 0.f;
	const auto x0 = int(x);
	const auto y0 = int(y);
	const auto x1 = std::min(x0 + 1, source.width - 1);
	const auto y1 = std::min(y0 + 1, source.height - 1);
	const auto wx = uint32(int((x - x0) * 256.f + 0.5f));
	const auto wy = uint32(int((y - y0) * 256.f + 0.5f));
	const auto top = source.row(y0);
	const auto bottom = source.row(y1);
	return Blend256(
		Blend256(top[x0], top[x1], wx),
		Blend256(bottom[x0], bottom[x1], wx),
		wy);
}

[[nodiscard]] inline Pixel SampleClear(const Source &source, float x, float y) {
	const auto inside = (x > -1.f)
		&& (y > -1.f)
		&& (x < float(source.width))
		&& (y < float(source.height));
	if (!inside) {
		return 0;
	}
	const auto fx = std::floor(x);
	const auto fy = std::floor(y);
	const auto x0 = int(fx);
	const auto y0 = int(fy);
	const auto wx = uint32(int((x - fx) * 256.f + 0.5f));
	const auto wy = uint32(int((y - fy) * 256.f + 0.5f));
	const auto at = [&](int px, int py) {
		const auto outside = (px < 0)
			|| (py < 0)
			|| (px >= source.width)
			|| (py >= source.height);
		return outside ? Pixel(0) : source.row(py)[px];
	};
	return Blend256(
		Blend256(at(x0, y0), at(x0 + 1, y0), wx),
		Blend256(at(x0, y0 + 1), at(x0 + 1, y0 + 1), wx),
		wy);
}

// The coordinate of the picture reflected at its edges: the pixels go
// 2 1 0 | 0 1 2 ... n-1 | n-1 n-2 and so on in both directions.
[[nodiscard]] inline float Reflected(float value, float size) {
	const auto period = 2.f * size;
	auto shifted = std::fmod(value + 0.5f, period);
	if (shifted < 0.f) {
		shifted += period;
	}
	if (shifted > size) {
		shifted = period - shifted;
	}
	return shifted - 0.5f;
}

[[nodiscard]] inline Pixel SampleMirror(
		const Source &source,
		float x,
		float y) {
	return SampleStretch(
		source,
		Reflected(x, float(source.width)),
		Reflected(y, float(source.height)));
}

[[nodiscard]] inline int SamplesFor(float stretch) {
	return !(stretch > kSingleSampleStretch && stretch < kBrokenStretch)
		? 1
		: (stretch > 9.f)
		? 4
		: (stretch > 4.f)
		? 3
		: 2;
}

// For every pixel the map says where it comes from. How far the sources
// of the pixels next to it (to the right and below) are shows how much
// the picture is squeezed there in each direction: when it is more than
// a pixel and a quarter the result is the mean of two to four samples
// spread along that direction over the source area of the pixel,
// otherwise fine detail would turn into sparkle. A neighbour that comes
// from nowhere says nothing about the pixel: that direction is sampled
// once and its difference is dropped (it is not a number and would
// poison the coordinates of every sample).
template <typename Sample>
void RemapRows(
		const Source &source,
		Pixel *to,
		ptrdiff_t toStride,
		const FxRowMap &map,
		const std::atomic<bool> *cancel,
		const Sample &sample) {
	const auto width = source.width;
	FxParallelRows(width, source.height, [&](int top, int bottom) {
		const auto count = width + 1;
		auto buffer = std::vector<float>(size_t(count) * 4);
		auto thisX = buffer.data();
		auto thisY = thisX + count;
		auto nextX = thisY + count;
		auto nextY = nextX + count;
		map(float(top), count, thisX, thisY);
		for (auto y = top; y != bottom; ++y) {
			if (Cancelled(cancel)) {
				return;
			}
			map(float(y + 1), count, nextX, nextY);
			const auto out = to + y * toStride;
			for (auto x = 0; x != width; ++x) {
				const auto sx = thisX[x];
				const auto sy = thisY[x];
				const auto finite = (std::abs(sx) < kCoordinateLimit)
					&& (std::abs(sy) < kCoordinateLimit);
				if (!finite) {
					out[x] = 0;
					continue;
				}
				auto xx = thisX[x + 1] - sx;
				auto xy = thisY[x + 1] - sy;
				auto yx = nextX[x] - sx;
				auto yy = nextY[x] - sy;
				const auto across = SamplesFor(xx * xx + xy * xy);
				const auto along = SamplesFor(yx * yx + yy * yy);
				if (across == 1 && along == 1) {
					out[x] = sample(sx, sy);
					continue;
				}
				if (across == 1) {
					xx = xy = 0.f;
				}
				if (along == 1) {
					yx = yy = 0.f;
				}
				uint32 sums[4] = { 0, 0, 0, 0 };
				for (auto j = 0; j != along; ++j) {
					const auto v = (j + 0.5f) / along - 0.5f;
					for (auto i = 0; i != across; ++i) {
						const auto u = (i + 0.5f) / across - 0.5f;
						const auto p = sample(
							sx + u * xx + v * yx,
							sy + u * xy + v * yy);
						sums[0] += p & 0xFFU;
						sums[1] += (p >> 8) & 0xFFU;
						sums[2] += (p >> 16) & 0xFFU;
						sums[3] += (p >> 24);
					}
				}
				const auto total = uint32(across * along);
				const auto half = total / 2;
				out[x] = (((sums[3] + half) / total) << 24)
					| (((sums[2] + half) / total) << 16)
					| (((sums[1] + half) / total) << 8)
					| ((sums[0] + half) / total);
			}
			std::swap(thisX, nextX);
			std::swap(thisY, nextY);
		}
	});
}

[[nodiscard]] FxEdges ReadEdges(const FxParams &params) {
	switch (params.integer("edges")) {
	case 1: return FxEdges::Stretch;
	case 2: return FxEdges::Mirror;
	}
	return FxEdges::Clear;
}

[[nodiscard]] FxParam EdgesParam(FxEdges value) {
	return FxChoice(
		"edges",
		tr::lng_oblivion_photo_distort_edges,
		{
			tr::lng_oblivion_photo_distort_edges_clear,
			tr::lng_oblivion_photo_distort_edges_stretch,
			tr::lng_oblivion_photo_distort_edges_mirror,
		},
		int(value));
}

//
// Mesh.
//

[[nodiscard]] QPointF RegularPoint(const FxMesh &mesh, int column, int row) {
	return QPointF(
		column / double(mesh.columns - 1),
		row / double(mesh.rows - 1));
}

// A point of the grid, or of its straight continuation outside.
[[nodiscard]] QPointF MeshNode(const FxMesh &mesh, int column, int row) {
	if (column < 0) {
		return 2. * MeshNode(mesh, 0, row) - MeshNode(mesh, 1, row);
	} else if (column >= mesh.columns) {
		return 2. * MeshNode(mesh, mesh.columns - 1, row)
			- MeshNode(mesh, mesh.columns - 2, row);
	} else if (row < 0) {
		return 2. * MeshNode(mesh, column, 0) - MeshNode(mesh, column, 1);
	} else if (row >= mesh.rows) {
		return 2. * MeshNode(mesh, column, mesh.rows - 1)
			- MeshNode(mesh, column, mesh.rows - 2);
	}
	return mesh.points[size_t(row) * mesh.columns + column];
}

void SplineWeights(double t, double *weights) {
	const auto t2 = t * t;
	const auto t3 = t2 * t;
	weights[0] = (-t3 + 2. * t2 - t) / 2.;
	weights[1] = (3. * t3 - 5. * t2 + 2.) / 2.;
	weights[2] = (-3. * t3 + 4. * t2 + t) / 2.;
	weights[3] = (t3 - t2) / 2.;
}

[[nodiscard]] QByteArray MeshCoordinate(double value) {
	value = std::round(
		std::clamp(value, kMeshPointMin, kMeshPointMax) * 100000.) / 100000.;
	if (value == 0.) {
		return QByteArray("0");
	}
	auto result = QByteArray::number(value, 'f', 5);
	while (result.endsWith('0')) {
		result.chop(1);
	}
	if (result.endsWith('.')) {
		result.chop(1);
	}
	return result;
}

// What the parameter of an effect means: the stored grid, or the regular
// one of the default size if there is nothing usable.
[[nodiscard]] FxMesh MeshFromData(const QByteArray &data) {
	auto result = FxMeshParse(data);
	return result.valid()
		? result
		: FxMeshRegular(kMeshDefaultSide, kMeshDefaultSide);
}

[[nodiscard]] FxMesh CornersFromData(const QByteArray &data) {
	auto result = FxMeshParse(data);
	return (result.valid() && result.columns == 2 && result.rows == 2)
		? result
		: FxMeshRegular(2, 2);
}

// The corners of a 2x2 grid as a quad: top left, top right, bottom right,
// bottom left, scaled to the size.
[[nodiscard]] QPolygonF CornersQuad(const FxMesh &corners, QSizeF size) {
	auto result = QPolygonF();
	for (const auto index : { 0, 1, 3, 2 }) {
		const auto point = corners.points[index];
		result.push_back(QPointF(
			point.x() * size.width(),
			point.y() * size.height()));
	}
	return result;
}

// A quad that can be dragged to: convex, not turned inside out and not
// collapsed.
[[nodiscard]] bool ValidCorners(const FxMesh &corners) {
	if (!corners.valid() || corners.columns != 2 || corners.rows != 2) {
		return false;
	}
	const auto quad = CornersQuad(corners, QSizeF(1., 1.));
	auto area = 0.;
	for (auto i = 0; i != 4; ++i) {
		const auto a = quad[i];
		const auto b = quad[(i + 1) % 4];
		area += a.x() * b.y() - b.x() * a.y();
	}
	auto transform = QTransform();
	return (area / 2. > kMinCornersArea)
		&& QuadTransform(QSizeF(1., 1.), quad, transform);
}

// A parameter exactly as the effect stores it, to put it back when a drag
// on the canvas is cancelled. Writing the parsed grid instead would turn
// an empty default into its text: the picture is the same, the document
// is not, and that is an undo step that changes nothing.
[[nodiscard]] std::optional<FxValue> StoredParam(
		const FxInstance &instance,
		const QByteArray &id) {
	return instance.params.has(id)
		? std::make_optional(instance.params.value(id))
		: std::nullopt;
}

void RestoreParam(
		FxInstance &instance,
		const QByteArray &id,
		const std::optional<FxValue> &stored) {
	if (stored) {
		instance.params.set(id, *stored);
	} else {
		instance.params.remove(id);
	}
}

// The mesh says where every point of the picture goes, a remap needs the
// opposite: where a pixel of the result comes from. The warped grid is
// cut into small triangles which are painted into a coarse table of
// source coordinates (the warp is smooth, the table is stretched over the
// result). Around the picture the grid is continued far outside, moved
// like its nearest edge point, so the whole result is covered and the
// "edges" setting decides what the continuation shows.
struct CoordinateMap {
	int width = 0;
	int height = 0;
	float step = 1.f;
	std::vector<float> xs;
	std::vector<float> ys;
};

struct MeshVertex {
	double x = 0.;
	double y = 0.;
	double sourceX = 0.;
	double sourceY = 0.;
};

void PaintTriangle(
		CoordinateMap &map,
		const MeshVertex &a,
		const MeshVertex &b,
		const MeshVertex &c) {
	const auto area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
	if (std::abs(area) < 1e-12) {
		return;
	}
	const auto left = std::max(
		int(std::floor(std::min({ a.x, b.x, c.x }))),
		0);
	const auto right = std::min(
		int(std::ceil(std::max({ a.x, b.x, c.x }))),
		map.width - 1);
	const auto top = std::max(
		int(std::floor(std::min({ a.y, b.y, c.y }))),
		0);
	const auto bottom = std::min(
		int(std::ceil(std::max({ a.y, b.y, c.y }))),
		map.height - 1);
	const auto inverse = 1. / area;
	constexpr auto kOutside = -1e-4;
	for (auto y = top; y <= bottom; ++y) {
		for (auto x = left; x <= right; ++x) {
			const auto first = ((b.x - x) * (c.y - y) - (b.y - y) * (c.x - x))
				* inverse;
			const auto second = ((c.x - x) * (a.y - y) - (c.y - y) * (a.x - x))
				* inverse;
			const auto third = 1. - first - second;
			if (first < kOutside || second < kOutside || third < kOutside) {
				continue;
			}
			const auto index = size_t(y) * map.width + x;
			map.xs[index] = float(first * a.sourceX
				+ second * b.sourceX
				+ third * c.sourceX);
			map.ys[index] = float(first * a.sourceY
				+ second * b.sourceY
				+ third * c.sourceY);
		}
	}
}

[[nodiscard]] int MeshCells(int points) {
	return (points - 1)
		* std::clamp((kMeshFineCells + points - 2) / (points - 1), 4, 16);
}

[[nodiscard]] CoordinateMap BuildMeshMap(
		const FxMesh &mesh,
		int width,
		int height) {
	auto map = CoordinateMap();
	const auto step = std::max(
		(std::max(width, height) + kMeshMapSide - 1) / kMeshMapSide,
		1);
	map.step = float(step);
	map.width = width / step + 2;
	map.height = height / step + 2;
	const auto count = size_t(map.width) * map.height;
	map.xs.assign(count, std::numeric_limits<float>::quiet_NaN());
	map.ys.assign(count, std::numeric_limits<float>::quiet_NaN());

	const auto cellsX = MeshCells(mesh.columns);
	const auto cellsY = MeshCells(mesh.rows);
	auto moved = std::vector<QPointF>(size_t(cellsX + 1) * (cellsY + 1));
	for (auto j = 0; j <= cellsY; ++j) {
		for (auto i = 0; i <= cellsX; ++i) {
			moved[size_t(j) * (cellsX + 1) + i] = FxMeshPoint(
				mesh,
				i / double(cellsX),
				j / double(cellsY));
		}
	}
	const auto countX = cellsX + 3;
	const auto countY = cellsY + 3;
	auto vertices = std::vector<MeshVertex>(size_t(countX) * countY);
	for (auto b = 0; b != countY; ++b) {
		const auto j = std::clamp(b - 1, 0, cellsY);
		const auto inner = j / double(cellsY);
		const auto v = (b == 0)
			? -kMeshOutside
			: (b == countY - 1)
			? (1. + kMeshOutside)
			: inner;
		for (auto a = 0; a != countX; ++a) {
			const auto i = std::clamp(a - 1, 0, cellsX);
			const auto across = i / double(cellsX);
			const auto u = (a == 0)
				? -kMeshOutside
				: (a == countX - 1)
				? (1. + kMeshOutside)
				: across;
			const auto shift = moved[size_t(j) * (cellsX + 1) + i]
				- QPointF(across, inner);
			vertices[size_t(b) * countX + a] = {
				((u + shift.x()) * width - 0.5) / step,
				((v + shift.y()) * height - 0.5) / step,
				u * width - 0.5,
				v * height - 0.5,
			};
		}
	}
	for (const auto outside : { true, false }) {
		for (auto b = 0; b + 1 != countY; ++b) {
			for (auto a = 0; a + 1 != countX; ++a) {
				const auto ring = (a == 0)
					|| (b == 0)
					|| (a == countX - 2)
					|| (b == countY - 2);
				if (ring != outside) {
					continue;
				}
				const auto corner = vertices.data() + size_t(b) * countX + a;
				const auto &opposite = corner[countX + 1];
				PaintTriangle(map, corner[0], corner[1], opposite);
				PaintTriangle(map, corner[0], opposite, corner[countX]);
			}
		}
	}
	return map;
}

//
// The effects.
//

[[nodiscard]] QPolygonF KeystoneQuad(
		QSizeF size,
		double vertical,
		double horizontal,
		double zoom) {
	const auto center = QPointF(size.width() / 2., size.height() / 2.);
	const auto halfWidth = center.x() * zoom;
	const auto halfHeight = center.y() * zoom;
	const auto narrow = vertical * kKeystoneStrength;
	const auto shorten = horizontal * kKeystoneStrength;
	return QPolygonF({
		center + QPointF(
			-halfWidth * (1. - narrow),
			-halfHeight * (1. + shorten)),
		center + QPointF(
			halfWidth * (1. - narrow),
			-halfHeight * (1. - shorten)),
		center + QPointF(
			halfWidth * (1. + narrow),
			halfHeight * (1. - shorten)),
		center + QPointF(
			-halfWidth * (1. + narrow),
			halfHeight * (1. + shorten)),
	});
}

// How much farther from the center the source of a pixel is, by the
// squared distance of the pixel from the center (in half diagonals), for
// the pincushion: the exact opposite of the barrel with the same
// strength, found by Newton's method.
[[nodiscard]] std::vector<float> PincushionTable(double strength) {
	auto result = std::vector<float>(kLensTableSize);
	for (auto i = 0; i != kLensTableSize; ++i) {
		const auto distance = std::sqrt(
			kLensTableRange * i / (kLensTableSize - 1));
		if (!(distance > 0.)) {
			result[i] = float(1. + strength);
			continue;
		}
		const auto target = distance * (1. + strength);
		auto value = target;
		for (auto k = 0; k != kLensSolveSteps; ++k) {
			const auto square = value * value;
			value -= (value * (1. + strength * square) - target)
				/ (1. + 3. * strength * square);
		}
		result[i] = float(value / distance);
	}
	return result;
}

[[nodiscard]] QPointF PixelPoint(QPointF normalized, QSize size) {
	return QPointF(
		normalized.x() * size.width() - 0.5,
		normalized.y() * size.height() - 0.5);
}

[[nodiscard]] bool ApplyKeystone(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	return FxWarpPerspective(
		image,
		KeystoneQuad(
			QSizeF(image.size()),
			params.number("vertical") / 100.,
			params.number("horizontal") / 100.,
			params.number("zoom") / 100.),
		ReadEdges(params),
		context.cancel);
}

[[nodiscard]] bool ApplyCorners(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	const auto corners = CornersFromData(params.data("corners"));
	if (FxMeshIsRegular(corners)) {
		return !context.cancelled();
	}
	return FxWarpPerspective(
		image,
		CornersQuad(corners, QSizeF(image.size())),
		ReadEdges(params),
		context.cancel);
}

[[nodiscard]] bool ApplySkew(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	const auto across = float(
		std::tan(params.number("horizontal") * kPi / 180.));
	const auto along = float(
		std::tan(params.number("vertical") * kPi / 180.));
	const auto centerX = (image.width() - 1) / 2.f;
	const auto centerY = (image.height() - 1) / 2.f;
	const auto xx = 1.f + across * along;
	return FxRemap(image, ReadEdges(params), [=](
			float y,
			int count,
			float *sx,
			float *sy) {
		const auto dy = y - centerY;
		for (auto x = 0; x != count; ++x) {
			const auto dx = x - centerX;
			sx[x] = centerX + xx * dx - across * dy;
			sy[x] = centerY - along * dx + dy;
		}
	}, context.cancel);
}

[[nodiscard]] bool ApplyLens(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	const auto amount = params.number("amount") / 100.;
	const auto strength = float(std::abs(amount) * kLensStrength);
	const auto shrink = float(100. / std::max(params.number("zoom"), 1.));
	const auto centerX = (image.width() - 1) / 2.f;
	const auto centerY = (image.height() - 1) / 2.f;
	const auto unit = 4.f / (float(image.width()) * image.width()
		+ float(image.height()) * image.height());
	if (amount >= 0.) {
		const auto divide = 1.f / (1.f + strength);
		return FxRemap(image, ReadEdges(params), [=](
				float y,
				int count,
				float *sx,
				float *sy) {
			const auto dy = (y - centerY) * shrink;
			for (auto x = 0; x != count; ++x) {
				const auto dx = (x - centerX) * shrink;
				const auto scale = (1.f + strength * (dx * dx + dy * dy) * unit)
					* divide;
				sx[x] = centerX + dx * scale;
				sy[x] = centerY + dy * scale;
			}
		}, context.cancel);
	}
	const auto table = PincushionTable(strength);
	const auto last = float(kLensTableSize - 1);
	const auto perUnit = last / float(kLensTableRange);
	return FxRemap(image, ReadEdges(params), [&](
			float y,
			int count,
			float *sx,
			float *sy) {
		const auto dy = (y - centerY) * shrink;
		for (auto x = 0; x != count; ++x) {
			const auto dx = (x - centerX) * shrink;
			const auto position = std::min(
				(dx * dx + dy * dy) * unit * perUnit,
				last);
			const auto index = std::min(int(position), kLensTableSize - 2);
			const auto scale = FxMix(
				table[index],
				table[index + 1],
				position - index);
			sx[x] = centerX + dx * scale;
			sy[x] = centerY + dy * scale;
		}
	}, context.cancel);
}

struct Spot {
	float centerX = 0.f;
	float centerY = 0.f;
	float radius = 1.f;
};

[[nodiscard]] Spot ReadSpot(const FxParams &params, QSize size) {
	const auto center = PixelPoint(params.point("center"), size);
	return {
		float(center.x()),
		float(center.y()),
		std::max(
			float(params.number("radius") / 100.
				* std::min(size.width(), size.height())),
			1.f),
	};
}

[[nodiscard]] bool ApplyBulge(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	const auto spot = ReadSpot(params, image.size());
	const auto amount = float(params.number("strength") / 100.);
	const auto strength = amount
		* ((amount > 0.f) ? kBulgeStrength : kPinchStrength);
	const auto limit = spot.radius * spot.radius;
	const auto inverse = 1.f / spot.radius;
	return FxRemap(image, ReadEdges(params), [=](
			float y,
			int count,
			float *sx,
			float *sy) {
		const auto dy = y - spot.centerY;
		for (auto x = 0; x != count; ++x) {
			const auto dx = x - spot.centerX;
			const auto distance = dx * dx + dy * dy;
			if (distance >= limit) {
				sx[x] = float(x);
				sy[x] = y;
				continue;
			}
			const auto rest = 1.f - std::sqrt(distance) * inverse;
			const auto scale = 1.f - strength * rest * rest;
			sx[x] = spot.centerX + dx * scale;
			sy[x] = spot.centerY + dy * scale;
		}
	}, context.cancel);
}

[[nodiscard]] bool ApplyTwirl(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	const auto spot = ReadSpot(params, image.size());
	const auto angle = float(params.number("angle") * kPi / 180.);
	const auto limit = spot.radius * spot.radius;
	const auto inverse = 1.f / spot.radius;
	return FxRemap(image, ReadEdges(params), [=](
			float y,
			int count,
			float *sx,
			float *sy) {
		const auto dy = y - spot.centerY;
		for (auto x = 0; x != count; ++x) {
			const auto dx = x - spot.centerX;
			const auto distance = dx * dx + dy * dy;
			if (distance >= limit) {
				sx[x] = float(x);
				sy[x] = y;
				continue;
			}
			const auto rest = 1.f - std::sqrt(distance) * inverse;
			const auto turn = -angle * rest * rest;
			const auto cosine = std::cos(turn);
			const auto sine = std::sin(turn);
			sx[x] = spot.centerX + dx * cosine - dy * sine;
			sy[x] = spot.centerY + dx * sine + dy * cosine;
		}
	}, context.cancel);
}

[[nodiscard]] bool ApplyWave(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	const auto amplitude = float(context.px(params.number("amplitude")));
	const auto length = std::max(
		float(context.px(params.number("wavelength"))),
		1.f);
	const auto radians = params.number("direction") * kPi / 180.;
	const auto alongX = float(std::cos(radians));
	const auto alongY = float(std::sin(radians));
	const auto phase = float(params.number("phase") / 360.);
	const auto zigzag = (params.integer("shape") == 1);
	const auto centerX = (image.width() - 1) / 2.f;
	const auto centerY = (image.height() - 1) / 2.f;
	const auto inverse = 1.f / length;
	return FxRemap(image, ReadEdges(params), [=](
			float y,
			int count,
			float *sx,
			float *sy) {
		const auto dy = y - centerY;
		for (auto x = 0; x != count; ++x) {
			const auto dx = x - centerX;
			const auto cycles = (dx * alongX + dy * alongY) * inverse + phase;
			auto value = 0.f;
			if (zigzag) {
				const auto shifted = cycles - 0.25f;
				const auto part = shifted - std::floor(shifted);
				value = 4.f * std::abs(part - 0.5f) - 1.f;
			} else {
				value = std::sin(float(2. * kPi) * cycles);
			}
			const auto move = amplitude * value;
			sx[x] = x + alongY * move;
			sy[x] = y - alongX * move;
		}
	}, context.cancel);
}

[[nodiscard]] bool ApplyRipple(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	const auto center = PixelPoint(params.point("center"), image.size());
	const auto centerX = float(center.x());
	const auto centerY = float(center.y());
	const auto amplitude = float(context.px(params.number("amplitude")));
	const auto length = std::max(
		float(context.px(params.number("wavelength"))),
		1.f);
	const auto phase = float(params.number("phase") / 360.);
	const auto decay = float(params.number("decay") / 100.)
		/ (0.5f * std::hypot(float(image.width()), float(image.height())));
	const auto inverse = 1.f / length;
	return FxRemap(image, ReadEdges(params), [=](
			float y,
			int count,
			float *sx,
			float *sy) {
		const auto dy = y - centerY;
		for (auto x = 0; x != count; ++x) {
			const auto dx = x - centerX;
			const auto distance = std::sqrt(dx * dx + dy * dy);
			if (distance < 1e-3f) {
				sx[x] = float(x);
				sy[x] = y;
				continue;
			}
			const auto fade = std::max(1.f - decay * distance, 0.f);
			const auto move = amplitude
				* fade
				* fade
				* std::sin(float(2. * kPi) * (distance * inverse - phase))
				/ distance;
			sx[x] = x + dx * move;
			sy[x] = y + dy * move;
		}
	}, context.cancel);
}

[[nodiscard]] FxParam PhaseParam() {
	return FxAngle(
		"phase",
		tr::lng_oblivion_photo_distort_phase,
		0.,
		0.,
		360.);
}

void RegisterDistortions() {
	const auto percent = u"%"_q;
	RegisterFx({
		.id = "distort.keystone",
		.group = FxGroup::Distort,
		.name = tr::lng_oblivion_photo_distort_keystone,
		.params = {
			FxFloat(
				"vertical",
				tr::lng_oblivion_photo_distort_vertical,
				-100.,
				100.,
				25.),
			FxFloat(
				"horizontal",
				tr::lng_oblivion_photo_distort_horizontal,
				-100.,
				100.,
				0.),
			FxFloat(
				"zoom",
				tr::lng_oblivion_photo_distort_zoom,
				50.,
				200.,
				100.,
				0,
				percent),
			EdgesParam(FxEdges::Clear),
		},
		.flags = kFxGeometry | kFxNeighbours,
		.order = 10,
		.apply = ApplyKeystone,
		.identity = [](const FxParams &params) {
			return (params.number("vertical") == 0.)
				&& (params.number("horizontal") == 0.)
				&& (params.number("zoom") == 100.);
		},
	});
	RegisterFx({
		.id = "distort.corners",
		.group = FxGroup::Distort,
		.name = tr::lng_oblivion_photo_distort_corners,
		.params = {
			FxCustom(
				"corners",
				tr::lng_oblivion_photo_distort_corners,
				kCornersType),
			EdgesParam(FxEdges::Clear),
		},
		.flags = kFxGeometry | kFxNeighbours,
		.order = 20,
		.apply = ApplyCorners,
		.identity = [](const FxParams &params) {
			return FxMeshIsRegular(CornersFromData(params.data("corners")));
		},
	});
	RegisterFx({
		.id = "distort.skew",
		.group = FxGroup::Distort,
		.name = tr::lng_oblivion_photo_distort_skew,
		.params = {
			FxAngle(
				"horizontal",
				tr::lng_oblivion_photo_distort_horizontal,
				12.,
				-60.,
				60.),
			FxAngle(
				"vertical",
				tr::lng_oblivion_photo_distort_vertical,
				0.,
				-60.,
				60.),
			EdgesParam(FxEdges::Clear),
		},
		.flags = kFxGeometry | kFxNeighbours,
		.order = 30,
		.apply = ApplySkew,
		.identity = [](const FxParams &params) {
			return (params.number("horizontal") == 0.)
				&& (params.number("vertical") == 0.);
		},
	});
	RegisterFx({
		.id = "distort.lens",
		.group = FxGroup::Distort,
		.name = tr::lng_oblivion_photo_distort_lens,
		.params = {
			FxFloat(
				"amount",
				tr::lng_oblivion_photo_distort_lens_amount,
				-100.,
				100.,
				35.),
			FxFloat(
				"zoom",
				tr::lng_oblivion_photo_distort_zoom,
				50.,
				200.,
				100.,
				0,
				percent),
			EdgesParam(FxEdges::Stretch),
		},
		.flags = kFxGeometry | kFxNeighbours,
		.order = 40,
		.apply = ApplyLens,
		.identity = [](const FxParams &params) {
			return (params.number("amount") == 0.)
				&& (params.number("zoom") == 100.);
		},
	});
	RegisterFx({
		.id = "distort.bulge",
		.group = FxGroup::Distort,
		.name = tr::lng_oblivion_photo_distort_bulge,
		.params = {
			FxFloat(
				"strength",
				tr::lng_oblivion_photo_distort_strength,
				-100.,
				100.,
				60.),
			FxFloat(
				"radius",
				tr::lng_oblivion_photo_distort_radius,
				1.,
				150.,
				45.,
				0,
				percent),
			FxPoint("center", tr::lng_oblivion_photo_distort_center),
			EdgesParam(FxEdges::Stretch),
		},
		.flags = kFxGeometry | kFxNeighbours,
		.order = 50,
		.apply = ApplyBulge,
		.identity = [](const FxParams &params) {
			return params.number("strength") == 0.;
		},
	});
	RegisterFx({
		.id = "distort.twirl",
		.group = FxGroup::Distort,
		.name = tr::lng_oblivion_photo_distort_twirl,
		.params = {
			FxAngle(
				"angle",
				tr::lng_oblivion_photo_distort_angle,
				120.,
				-720.,
				720.),
			FxFloat(
				"radius",
				tr::lng_oblivion_photo_distort_radius,
				1.,
				150.,
				45.,
				0,
				percent),
			FxPoint("center", tr::lng_oblivion_photo_distort_center),
			EdgesParam(FxEdges::Stretch),
		},
		.flags = kFxGeometry | kFxNeighbours,
		.order = 60,
		.apply = ApplyTwirl,
		.identity = [](const FxParams &params) {
			return params.number("angle") == 0.;
		},
	});
	RegisterFx({
		.id = "distort.wave",
		.group = FxGroup::Distort,
		.name = tr::lng_oblivion_photo_distort_wave,
		.params = {
			FxPixels(
				"amplitude",
				tr::lng_oblivion_photo_distort_amplitude,
				0.,
				300.,
				24.),
			FxPixels(
				"wavelength",
				tr::lng_oblivion_photo_distort_wavelength,
				4.,
				2000.,
				240.),
			FxAngle("direction", tr::lng_oblivion_photo_distort_direction),
			PhaseParam(),
			FxChoice("shape", tr::lng_oblivion_photo_distort_wave_shape, {
				tr::lng_oblivion_photo_distort_wave_sine,
				tr::lng_oblivion_photo_distort_wave_triangle,
			}),
			EdgesParam(FxEdges::Stretch),
		},
		.flags = kFxGeometry | kFxNeighbours,
		.order = 70,
		.apply = ApplyWave,
		.identity = [](const FxParams &params) {
			return params.number("amplitude") <= 0.;
		},
	});
	RegisterFx({
		.id = "distort.ripple",
		.group = FxGroup::Distort,
		.name = tr::lng_oblivion_photo_distort_ripple,
		.params = {
			FxPixels(
				"amplitude",
				tr::lng_oblivion_photo_distort_amplitude,
				0.,
				300.,
				16.),
			FxPixels(
				"wavelength",
				tr::lng_oblivion_photo_distort_wavelength,
				4.,
				2000.,
				160.),
			PhaseParam(),
			FxFloat(
				"decay",
				tr::lng_oblivion_photo_distort_decay,
				0.,
				100.,
				50.,
				0,
				percent),
			FxPoint("center", tr::lng_oblivion_photo_distort_center),
			EdgesParam(FxEdges::Stretch),
		},
		.flags = kFxGeometry | kFxNeighbours,
		.order = 80,
		.apply = ApplyRipple,
		.identity = [](const FxParams &params) {
			return params.number("amplitude") <= 0.;
		},
	});
	RegisterFx({
		.id = "distort.mesh",
		.group = FxGroup::Distort,
		.name = tr::lng_oblivion_photo_distort_mesh,
		.params = {
			FxCustom("mesh", tr::lng_oblivion_photo_distort_mesh, kMeshType),
			EdgesParam(FxEdges::Clear),
		},
		.flags = kFxGeometry | kFxNeighbours,
		.order = 90,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			return FxWarpMesh(
				image,
				MeshFromData(params.data("mesh")),
				ReadEdges(params),
				context.cancel);
		},
		.identity = [](const FxParams &params) {
			return FxMeshIsRegular(MeshFromData(params.data("mesh")));
		},
	});
}

const auto Registered = FxRegistrar([] {
	RegisterDistortions();
});

//
// Self-test.
//

[[nodiscard]] int MaxDifference(const QImage &a, const QImage &b) {
	if (a.size() != b.size() || a.isNull() || b.isNull()) {
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

[[nodiscard]] int MaxDifference(
		const QImage &a,
		const QImage &b,
		QRect rect) {
	return MaxDifference(a.copy(rect), b.copy(rect));
}

[[nodiscard]] bool ValidPixels(const QImage &image) {
	for (auto y = 0; y != image.height(); ++y) {
		const auto line = FxRow(image, y);
		for (auto x = 0; x != image.width(); ++x) {
			const auto p = line[x];
			const auto a = (p >> 24);
			if (((p >> 16) & 0xFFU) > a
				|| ((p >> 8) & 0xFFU) > a
				|| (p & 0xFFU) > a) {
				return false;
			}
		}
	}
	return true;
}

[[nodiscard]] bool AllOpaque(const QImage &image) {
	for (auto y = 0; y != image.height(); ++y) {
		const auto line = FxRow(image, y);
		for (auto x = 0; x != image.width(); ++x) {
			if ((line[x] >> 24) != 0xFFU) {
				return false;
			}
		}
	}
	return true;
}

[[nodiscard]] QPoint Brightest(const QImage &image) {
	auto result = QPoint();
	auto best = -1;
	for (auto y = 0; y != image.height(); ++y) {
		const auto line = FxRow(image, y);
		for (auto x = 0; x != image.width(); ++x) {
			const auto value = int((line[x] >> 8) & 0xFFU);
			if (value > best) {
				best = value;
				result = QPoint(x, y);
			}
		}
	}
	return result;
}

[[nodiscard]] QImage DotImage(int width, int height, QPoint dot) {
	auto result = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
	result.fill(QColor(20, 20, 20));
	for (auto y = dot.y() - 1; y <= dot.y() + 1; ++y) {
		for (auto x = dot.x() - 1; x <= dot.x() + 1; ++x) {
			FxRow(result, y)[x] = 0xFFFFFFFFU;
		}
	}
	return result;
}

// Every effect of the group with all its parameters at one end of their
// ranges, at the other end and mixed, on pictures of a few pixels.
[[nodiscard]] bool SurvivesExtremes(FxGroup group, QStringList &log) {
	const auto pictures = std::vector<QImage>{
		FxTestImage(1, 1),
		FxTestImage(3, 2, true),
		FxTestImage(37, 11, true),
	};
	auto result = true;
	for (const auto descriptor : FxInGroup(group)) {
		for (auto variant = 0; variant != 3; ++variant) {
			auto instance = MakeFx(descriptor->id);
			auto index = 0;
			for (const auto &param : descriptor->params) {
				const auto high = (variant == 1)
					|| (variant == 2 && (index++ % 2));
				if (param.kind == FxParamKind::Point) {
					const auto edge = high ? 5. : -4.;
					instance.params.set(
						param.id,
						FxValue::Point(QPointF(edge, edge)));
				} else if (param.kind != FxParamKind::Custom) {
					instance.params.set(
						param.id,
						FxValue::Number(high ? param.max : param.min));
				}
			}
			for (const auto &picture : pictures) {
				auto image = picture;
				const auto applied = ApplyFx(
					image,
					instance,
					FxContext{ .seed = 3, .fullSize = picture.size() });
				if (!applied
					|| image.size() != picture.size()
					|| !ValidPixels(image)) {
					result = false;
					log.push_back(u"FAIL: %1, extremes %2, %3x%4"_q.arg(
						QString::fromLatin1(descriptor->id)).arg(
							variant).arg(
								picture.width()).arg(picture.height()));
				}
			}
		}
	}
	return result;
}

[[nodiscard]] bool RunDistortSelfTest(QStringList &log) {
	auto ok = true;
	const auto check = [&](bool condition, const QString &what) {
		log.push_back((condition ? u"OK: "_q : u"FAIL: "_q) + what);
		ok = ok && condition;
	};
	const auto info = [&](const QString &what) {
		log.push_back(u"   "_q + what);
	};
	const auto number = [](double value, int digits = 2) {
		return QString::number(value, 'f', digits);
	};
	const auto picture = FxTestImage(101, 81);
	const auto clear = FxTestImage(101, 81, true);
	const auto context = FxContext{ .seed = 9, .fullSize = picture.size() };
	const auto modes = std::array<FxEdges, 3>{ {
		FxEdges::Clear,
		FxEdges::Stretch,
		FxEdges::Mirror,
	} };
	const auto straight = [](float y, int count, float *sx, float *sy) {
		for (auto x = 0; x != count; ++x) {
			sx[x] = float(x);
			sy[x] = y;
		}
	};

	// The resampler.
	{
		auto same = true;
		for (const auto mode : modes) {
			auto image = clear;
			same = FxRemap(image, mode, straight)
				&& !MaxDifference(image, clear)
				&& same;
		}
		check(same, u"a remap to the same place changes nothing"_q);

		const auto moved = [](float y, int count, float *sx, float *sy) {
			for (auto x = 0; x != count; ++x) {
				sx[x] = float(x - 3);
				sy[x] = y;
			}
		};
		const auto at = [](const QImage &image, int x) {
			return FxRow(image, 40)[x];
		};
		auto empty = picture;
		auto repeated = picture;
		auto mirrored = picture;
		const auto done = FxRemap(empty, FxEdges::Clear, moved)
			&& FxRemap(repeated, FxEdges::Stretch, moved)
			&& FxRemap(mirrored, FxEdges::Mirror, moved);
		check(
			done
				&& !at(empty, 0)
				&& !at(empty, 2)
				&& at(empty, 3) == at(picture, 0)
				&& at(empty, 100) == at(picture, 97)
				&& at(repeated, 0) == at(picture, 0)
				&& at(repeated, 2) == at(picture, 0)
				&& at(repeated, 50) == at(picture, 47)
				&& at(mirrored, 2) == at(picture, 0)
				&& at(mirrored, 1) == at(picture, 1)
				&& at(mirrored, 0) == at(picture, 2)
				&& at(mirrored, 60) == at(picture, 57),
			u"edges: transparent, stretched and mirrored"_q);

		auto board = QImage(64, 64, QImage::Format_ARGB32_Premultiplied);
		for (auto y = 0; y != 64; ++y) {
			for (auto x = 0; x != 64; ++x) {
				FxRow(board, y)[x] = ((x + y) & 1) ? 0xFFFFFFFFU : 0xFF000000U;
			}
		}
		const auto squeezed = FxRemap(board, FxEdges::Stretch, [](
				float y,
				int count,
				float *sx,
				float *sy) {
			for (auto x = 0; x != count; ++x) {
				sx[x] = x * 2.f + 0.5f;
				sy[x] = y * 2.f + 0.5f;
			}
		});
		auto lowest = 255;
		auto highest = 0;
		for (auto y = 0; y != 30; ++y) {
			for (auto x = 0; x != 30; ++x) {
				const auto value = int(FxRow(board, y)[x] & 0xFFU);
				lowest = std::min(lowest, value);
				highest = std::max(highest, value);
			}
		}
		check(
			squeezed && lowest >= 120 && highest <= 136,
			u"a squeezed picture is averaged, not skipped (%1..%2)"_q.arg(
				lowest).arg(highest));

		auto broken = picture;
		const auto holes = FxRemap(broken, FxEdges::Stretch, [](
				float y,
				int count,
				float *sx,
				float *sy) {
			for (auto x = 0; x != count; ++x) {
				const auto bad = (x % 7 == 3);
				sx[x] = bad
					? std::numeric_limits<float>::quiet_NaN()
					: float(x);
				sy[x] = (x % 11 == 5)
					? std::numeric_limits<float>::infinity()
					: y;
			}
		});
		check(
			holes
				&& !FxRow(broken, 10)[3]
				&& !FxRow(broken, 10)[5]
				&& FxRow(broken, 10)[4] == FxRow(picture, 10)[4]
				&& ValidPixels(broken),
			u"coordinates that are not numbers give transparent pixels"_q);

		auto mixed = picture;
		const auto partly = FxRemap(mixed, FxEdges::Mirror, [](
				float y,
				int count,
				float *sx,
				float *sy) {
			const auto bad = (int(y) % 5 == 2);
			for (auto x = 0; x != count; ++x) {
				sx[x] = x * 3.f;
				sy[x] = bad ? std::numeric_limits<float>::quiet_NaN() : y;
			}
		});
		check(
			partly
				&& !FxRow(mixed, 2)[10]
				&& (FxRow(mixed, 1)[10] >> 24) == 0xFFU
				&& (FxRow(mixed, 3)[10] >> 24) == 0xFFU
				&& ValidPixels(mixed),
			u"a squeezed row next to a row from nowhere is still sampled"_q);

		const auto cancel = std::atomic<bool>(true);
		auto stopped = picture;
		check(
			!FxRemap(stopped, FxEdges::Clear, straight, &cancel),
			u"a cancelled remap reports it"_q);
	}

	// Zero strength.
	{
		struct Zero {
			QByteArray id;
			std::vector<FxParams::Entry> values;
		};
		const auto zeros = std::vector<Zero>{
			{ "distort.keystone", { { "vertical", FxValue::Number(0.) } } },
			{ "distort.corners", {} },
			{ "distort.skew", { { "horizontal", FxValue::Number(0.) } } },
			{ "distort.lens", { { "amount", FxValue::Number(0.) } } },
			{ "distort.bulge", { { "strength", FxValue::Number(0.) } } },
			{ "distort.twirl", { { "angle", FxValue::Number(0.) } } },
			{ "distort.wave", { { "amplitude", FxValue::Number(0.) } } },
			{ "distort.ripple", { { "amplitude", FxValue::Number(0.) } } },
			{ "distort.mesh", {} },
		};
		auto identity = true;
		auto exact = true;
		for (const auto &zero : zeros) {
			const auto descriptor = FindFx(zero.id);
			if (!descriptor) {
				identity = false;
				continue;
			}
			auto instance = MakeFx(zero.id);
			for (const auto &entry : zero.values) {
				instance.params.set(entry.id, entry.value);
			}
			const auto params = NormalizedFxParams(
				*descriptor,
				instance.params);
			identity = identity && FxIsIdentity(instance);
			for (const auto mode : modes) {
				auto forced = params;
				forced.set("edges", FxValue::Integer(int(mode)));
				auto image = clear;
				const auto applied = FxPrepare(image)
					&& descriptor->apply(image, forced, context);
				if (!applied || MaxDifference(image, clear)) {
					exact = false;
					log.push_back(u"FAIL: zero strength of %1 differs by %2"_q
						.arg(QString::fromLatin1(zero.id))
						.arg(MaxDifference(image, clear)));
				}
			}
		}
		check(
			zeros.size() == FxInGroup(FxGroup::Distort).size() && identity,
			u"every distortion is an identity at zero strength"_q);
		check(
			exact,
			u"zero strength gives back exactly the same pixels"_q);
	}

	// Perspective.
	{
		const auto size = QSizeF(picture.size());
		const auto width = size.width();
		const auto height = size.height();
		auto flipped = picture;
		const auto mirror = QPolygonF({
			QPointF(width, 0.),
			QPointF(0., 0.),
			QPointF(0., height),
			QPointF(width, height),
		});
		auto done = FxWarpPerspective(flipped, mirror, FxEdges::Clear);
		auto expected = picture;
		for (auto y = 0; y != expected.height(); ++y) {
			const auto line = FxRow(expected, y);
			std::reverse(line, line + expected.width());
		}
		check(
			done && MaxDifference(flipped, expected) <= 1,
			u"corners swapped left to right mirror the picture"_q);

		const auto large = FxTestImage(200, 120);
		auto half = large;
		done = FxWarpPerspective(half, QPolygonF({
			QPointF(0., 0.),
			QPointF(100., 0.),
			QPointF(100., 120.),
			QPointF(0., 120.),
		}), FxEdges::Clear);
		const auto reduced = FxResized(large, QSize(100, 120));
		const auto top = QRect(0, 0, 100, 80);
		check(
			done
				&& FxImageDifference(half.copy(top), reduced.copy(top)) < 2.
				&& !FxRow(half, 60)[101]
				&& !FxRow(half, 60)[199]
				&& (FxRow(half, 60)[98] >> 24) == 0xFFU,
			u"a quad over the left half squeezes the picture there (%1)"_q
				.arg(number(FxImageDifference(
					half.copy(top),
					reduced.copy(top)))));

		const auto quad = QPolygonF({
			QPointF(14., 9.),
			QPointF(92., 4.),
			QPointF(97., 72.),
			QPointF(6., 78.),
		});
		auto forward = QTransform();
		auto exact = QuadTransform(size, quad, forward);
		const auto corners = QPolygonF({
			QPointF(0., 0.),
			QPointF(width, 0.),
			QPointF(width, height),
			QPointF(0., height),
		});
		auto worst = 0.;
		auto back = QPolygonF();
		const auto inverse = forward.inverted();
		const auto apart = [&](QPointF a, QPointF b) {
			worst = std::max(worst, std::hypot(a.x() - b.x(), a.y() - b.y()));
		};
		for (auto i = 0; i != 4; ++i) {
			apart(forward.map(corners[i]), quad[i]);
			apart(inverse.map(quad[i]), corners[i]);
			back.push_back(inverse.map(corners[i]));
		}
		for (const auto point : { QPointF(31., 17.), QPointF(77.5, 60.25) }) {
			apart(inverse.map(forward.map(point)), point);
			apart(forward.map(inverse.map(point)), point);
		}
		check(
			exact && worst < 1e-6,
			u"the perspective transform and its inverse agree (%1)"_q.arg(
				number(worst, 9)));
		auto smooth = picture;
		FxGaussianBlur(smooth, 1.5);
		auto there = smooth;
		done = FxWarpPerspective(there, quad, FxEdges::Stretch);
		auto again = there;
		done = FxWarpPerspective(again, back, FxEdges::Stretch) && done;
		const auto difference = FxImageDifference(again, smooth);
		check(
			done
				&& difference < 3.
				&& FxImageDifference(there, smooth) > 2. * difference
				&& AllOpaque(again),
			u"a perspective warp and the opposite one give the picture "
			"back (%1, the warp alone %2)"_q.arg(
				number(difference),
				number(FxImageDifference(there, smooth))));

		auto folded = picture;
		done = FxWarpPerspective(folded, QPolygonF({
			QPointF(0., 0.),
			QPointF(width, height),
			QPointF(width, 0.),
			QPointF(0., height),
		}), FxEdges::Clear);
		check(
			done && folded.cacheKey() == picture.cacheKey(),
			u"a folded quad changes nothing"_q);

		auto tilted = picture;
		done = ApplyFx(tilted, MakeFx("distort.keystone", {
			{ "vertical", FxValue::Number(100.) },
		}), context);
		const auto alpha = [&](int x, int y) {
			return int(FxRow(tilted, y)[x] >> 24);
		};
		check(
			done
				&& !alpha(5, 2)
				&& !alpha(95, 2)
				&& alpha(50, 2) == 255
				&& alpha(5, 78) == 255
				&& alpha(95, 78) == 255,
			u"a vertical tilt narrows the top"_q);
	}

	// Known mappings of the other distortions.
	{
		const auto at = [](const QImage &image, int x, int y) {
			return FxRow(image, y)[x];
		};
		auto skewed = picture;
		auto done = ApplyFx(skewed, MakeFx("distort.skew", {
			{ "horizontal", FxValue::Number(26.565051177078) },
			{ "edges", FxValue::Integer(1) },
		}), context);
		check(
			done
				&& at(skewed, 30, 40) == at(picture, 30, 40)
				&& at(skewed, 30, 50) == at(picture, 25, 50)
				&& at(skewed, 30, 28) == at(picture, 36, 28),
			u"a skew of one to two shifts a row by half its distance from "
			"the middle"_q);

		auto waved = picture;
		done = ApplyFx(waved, MakeFx("distort.wave", {
			{ "amplitude", FxValue::Number(6.) },
			{ "wavelength", FxValue::Number(80.) },
		}), context);
		check(
			done
				&& at(waved, 50, 30) == at(picture, 50, 30)
				&& at(waved, 90, 30) == at(picture, 90, 30)
				&& at(waved, 70, 30) == at(picture, 70, 24)
				&& at(waved, 30, 30) == at(picture, 30, 36),
			u"a wave moves the columns up and down by its amplitude"_q);

		auto bulged = picture;
		done = ApplyFx(bulged, MakeFx("distort.bulge", {
			{ "strength", FxValue::Number(80.) },
			{ "radius", FxValue::Number(25.) },
		}), context);
		auto pinched = picture;
		done = ApplyFx(pinched, MakeFx("distort.bulge", {
			{ "strength", FxValue::Number(-80.) },
			{ "radius", FxValue::Number(25.) },
		}), context) && done;
		const auto outside = QRect(0, 0, 28, 81);
		check(
			done
				&& !MaxDifference(bulged, picture, outside)
				&& !MaxDifference(pinched, picture, outside)
				&& at(bulged, 50, 40) == at(picture, 50, 40)
				&& FxImageDifference(bulged, picture) > 0.1
				&& FxImageDifference(pinched, picture) > 0.1
				&& AllOpaque(bulged)
				&& AllOpaque(pinched),
			u"bulge and pinch keep everything outside of the circle (they "
			"change %1 and %2)"_q.arg(
				number(FxImageDifference(bulged, picture)),
				number(FxImageDifference(pinched, picture))));

		const auto spot = DotImage(121, 121, QPoint(60, 50));
		auto twirled = spot;
		done = ApplyFx(twirled, MakeFx("distort.twirl", {
			{ "angle", FxValue::Number(90.) },
			{ "radius", FxValue::Number(150.) },
		}), FxContext{ .fullSize = spot.size() });
		const auto found = Brightest(twirled);
		check(
			done
				&& std::abs(found.x() - 70) <= 2
				&& std::abs(found.y() - 58) <= 2,
			u"a positive twirl turns the middle clockwise (the dot is at "
			"%1, %2)"_q.arg(found.x()).arg(found.y()));

		const auto wide = FxTestImage(200, 150);
		auto barrel = wide;
		done = ApplyFx(barrel, MakeFx("distort.lens", {
			{ "amount", FxValue::Number(60.) },
		}), FxContext{ .fullSize = wide.size() });
		auto restored = barrel;
		done = ApplyFx(restored, MakeFx("distort.lens", {
			{ "amount", FxValue::Number(-60.) },
		}), FxContext{ .fullSize = wide.size() }) && done;
		const auto core = QRect(60, 30, 80, 60);
		const auto bent = FxImageDifference(barrel.copy(core), wide.copy(core));
		const auto fixed = FxImageDifference(
			restored.copy(core),
			wide.copy(core));
		check(
			done
				&& fixed < 3.
				&& bent > 2. * fixed
				&& AllOpaque(barrel),
			u"a pincushion undoes the barrel of the same strength (%1 "
			"left of %2)"_q.arg(number(fixed), number(bent)));

		auto rippled = picture;
		done = ApplyFx(rippled, MakeFx("distort.ripple", {
			{ "amplitude", FxValue::Number(5.) },
			{ "wavelength", FxValue::Number(30.) },
		}), context);
		check(
			done
				&& FxImageDifference(rippled, picture) > 0.5
				&& AllOpaque(rippled),
			u"a ripple bends the picture around its center"_q);
	}

	// Mesh.
	{
		const auto regular = FxMeshRegular(4, 4);
		auto untouched = picture;
		auto done = FxWarpMesh(untouched, regular, FxEdges::Clear);
		check(
			done
				&& untouched.cacheKey() == picture.cacheKey()
				&& FxMeshIsRegular(regular)
				&& FxMeshIsRegular(FxMeshResampled(regular, 7, 3)),
			u"a regular mesh changes nothing"_q);

		auto random = FxRandom(77);
		auto bent = FxMeshRegular(5, 4);
		for (auto &point : bent.points) {
			point += QPointF(
				(random.unit() - 0.5) * 0.12,
				(random.unit() - 0.5) * 0.12);
		}
		const auto bytes = FxMeshSerialize(bent);
		const auto parsed = FxMeshParse(bytes);
		auto nodes = 0.;
		for (auto row = 0; row != parsed.rows; ++row) {
			for (auto column = 0; column != parsed.columns; ++column) {
				const auto delta = FxMeshPoint(
					parsed,
					column / double(parsed.columns - 1),
					row / double(parsed.rows - 1))
					- parsed.points[size_t(row) * parsed.columns + column];
				nodes = std::max(nodes, std::hypot(delta.x(), delta.y()));
			}
		}
		check(
			parsed.valid()
				&& parsed.columns == 5
				&& parsed.rows == 4
				&& FxMeshSerialize(parsed) == bytes
				&& !FxMeshIsRegular(parsed)
				&& nodes < 1e-9,
			u"a mesh survives serialization and passes through its points "
			"(%1 bytes)"_q.arg(bytes.size()));
		check(
			!FxMeshParse("").valid()
				&& !FxMeshParse("4x4:0,0;1,1").valid()
				&& !FxMeshParse("1x9:0,0").valid()
				&& !FxMeshParse("2x2:0,0;1,0;0,1;nan,1").valid()
				&& !FxMeshParse("2x2:0,0;1,0;0,1;1").valid()
				&& FxMeshParse("2x2:0,0;1,0;0,1;9,-7").points[3]
					== QPointF(kMeshPointMax, kMeshPointMin)
				&& FxMeshSerialize(FxMeshRegular(2, 2))
					== "2x2:0,0;1,0;0,1;1,1",
			u"broken mesh data is refused, wild points are clamped"_q);

		const auto resampled = FxMeshResampled(parsed, 9, 9);
		auto apart = 0.;
		for (auto i = 0; i <= 20; ++i) {
			for (auto j = 0; j <= 20; ++j) {
				const auto delta = FxMeshPoint(parsed, i / 20., j / 20.)
					- FxMeshPoint(resampled, i / 20., j / 20.);
				apart = std::max(apart, std::hypot(delta.x(), delta.y()));
			}
		}
		check(
			apart < 0.02,
			u"a denser grid describes nearly the same warp (%1)"_q.arg(
				number(apart, 4)));

		const auto large = FxTestImage(240, 180);
		auto shifted = FxMeshRegular(4, 4);
		for (auto &point : shifted.points) {
			point += QPointF(12. / 240., 6. / 180.);
		}
		auto moved = large;
		done = FxWarpMesh(moved, shifted, FxEdges::Clear);
		check(
			done
				&& MaxDifference(
					moved.copy(QRect(20, 12, 200, 150)),
					large.copy(QRect(8, 6, 200, 150))) <= 1
				&& !FxRow(moved, 100)[5]
				&& !FxRow(moved, 2)[100],
			u"a mesh moved as a whole moves the picture with it"_q);

		const auto spot = DotImage(240, 180, QPoint(80, 60));
		auto pulled = FxMeshRegular(4, 4);
		pulled.points[5] += QPointF(18. / 240., 10. / 180.);
		auto dragged = spot;
		done = FxWarpMesh(dragged, pulled, FxEdges::Stretch);
		const auto found = Brightest(dragged);
		check(
			done
				&& std::abs(found.x() - 98) <= 2
				&& std::abs(found.y() - 70) <= 2
				&& AllOpaque(dragged),
			u"the picture under a mesh point follows the point (%1, %2 "
			"for 98, 70)"_q.arg(found.x()).arg(found.y()));

		auto wild = FxMeshRegular(6, 6);
		for (auto &point : wild.points) {
			point += QPointF(
				(random.unit() - 0.5) * 0.3,
				(random.unit() - 0.5) * 0.3);
		}
		auto solid = large;
		auto holes = large;
		auto reflected = FxTestImage(240, 180, true);
		done = FxWarpMesh(solid, wild, FxEdges::Stretch)
			&& FxWarpMesh(holes, wild, FxEdges::Clear)
			&& FxWarpMesh(reflected, wild, FxEdges::Mirror);
		check(
			done
				&& AllOpaque(solid)
				&& !AllOpaque(holes)
				&& ValidPixels(holes)
				&& ValidPixels(reflected),
			u"a wild mesh leaves no holes when the edges are stretched"_q);

		check(
			CornersFromData("3x3:0,0").columns == 2
				&& FxMeshIsRegular(CornersFromData(QByteArray()))
				&& ValidCorners(FxMeshRegular(2, 2))
				&& ValidCorners(FxMeshParse("2x2:0.1,0;0.9,0.1;0,1;1,0.8"))
				&& !ValidCorners(FxMeshParse("2x2:1,0;0,0;1,1;0,1"))
				&& !ValidCorners(FxMeshParse("2x2:0,0;1,0;0.6,0.4;1,1"))
				&& !ValidCorners(FxMeshRegular(3, 3)),
			u"only convex, not mirrored corners are accepted"_q);
	}

	// What the editors on the canvas rely on.
	{
		auto pulled = FxMeshRegular(4, 4);
		pulled.points[5] += QPointF(0.07, 0.04);
		const auto bytes = FxMeshSerialize(pulled);

		auto fresh = MakeFx("distort.mesh");
		const auto untouched = fresh;
		const auto stored = StoredParam(fresh, "mesh");
		fresh.params.set("mesh", FxValue::Data(bytes));
		const auto dragged = !(fresh == untouched);
		RestoreParam(fresh, "mesh", stored);

		auto bare = FxInstance{ .id = QByteArray("distort.corners") };
		const auto missing = StoredParam(bare, "corners");
		bare.params.set(
			"corners",
			FxValue::Data(FxMeshSerialize(FxMeshRegular(2, 2))));
		const auto written = bare.params.has("corners");
		RestoreParam(bare, "corners", missing);

		auto edited = MakeFx("distort.mesh", {
			{ "mesh", FxValue::Data(bytes) },
		});
		const auto kept = edited;
		const auto before = StoredParam(edited, "mesh");
		edited.params.set(
			"mesh",
			FxValue::Data(FxMeshSerialize(FxMeshRegular(4, 4))));
		RestoreParam(edited, "mesh", before);

		check(
			dragged
				&& stored
				&& (fresh == untouched)
				&& written
				&& !missing
				&& bare.params.empty()
				&& before
				&& (edited == kept)
				&& (edited.params.data("mesh") == bytes),
			u"a cancelled drag puts the parameter back as it was stored"_q);
		check(
			FxMeshIsRegular(MeshFromData(untouched.params.data("mesh")))
				&& FxMeshIsRegular(CornersFromData(QByteArray()))
				&& !FxMeshIsRegular(MeshFromData(bytes)),
			u"a grid that was never edited has nothing to reset"_q);
	}

	// Every distortion with every kind of edges.
	{
		auto valid = true;
		for (const auto descriptor : FxInGroup(FxGroup::Distort)) {
			for (const auto mode : modes) {
				auto instance = MakeFx(descriptor->id, {
					{ "edges", FxValue::Integer(int(mode)) },
					{ "mesh", FxValue::Data("3x3:0,0;0.5,0.1;1,0;0.1,0.5;"
						"0.6,0.4;1,0.5;0,1;0.5,0.9;1.1,1") },
					{ "corners", FxValue::Data("2x2:0.1,0.05;0.9,0;0,1;"
						"0.95,0.9") },
				});
				auto image = clear;
				const auto applied = ApplyFx(image, instance, context);
				const auto fine = applied
					&& (image.size() == clear.size())
					&& ValidPixels(image)
					&& (FxImageDifference(image, clear) > 0.2);
				if (!fine) {
					valid = false;
					log.push_back(u"FAIL: %1 with edges %2"_q.arg(
						QString::fromLatin1(descriptor->id)).arg(int(mode)));
				}
			}
		}
		check(valid, u"every distortion works with every kind of edges"_q);
	}

	check(
		SurvivesExtremes(FxGroup::Distort, log),
		u"distortions survive extreme settings and tiny pictures"_q);
	{
		auto tiny = FxTestImage(2, 1, true);
		auto mesh = FxMeshRegular(9, 9);
		mesh.points[40] = QPointF(kMeshPointMax, kMeshPointMin);
		mesh.points[0] = QPointF(kMeshPointMax, kMeshPointMax);
		const auto warped = FxWarpMesh(tiny, mesh, FxEdges::Mirror);
		auto corner = FxTestImage(5, 4, true);
		const auto bent = FxWarpPerspective(corner, QPolygonF({
			QPointF(-900., -700.),
			QPointF(4., 0.2),
			QPointF(5., 4.),
			QPointF(0.5, 3.9),
		}), FxEdges::Clear);
		check(
			warped
				&& bent
				&& tiny.size() == QSize(2, 1)
				&& corner.size() == QSize(5, 4)
				&& ValidPixels(tiny)
				&& ValidPixels(corner),
			u"a folded mesh and a far corner on a few pixels are fine"_q);
	}

	// The preview looks like the export.
	{
		const auto full = FxTestImage(512, 384);
		const auto half = FxResized(full, full.size() / 2);
		auto worst = 0.;
		auto done = true;
		for (const auto descriptor : FxInGroup(FxGroup::Distort)) {
			const auto instance = MakeFx(descriptor->id, {
				{ "mesh", FxValue::Data("3x3:0,0;0.5,0.1;1,0;0.1,0.5;"
					"0.6,0.4;1,0.5;0,1;0.5,0.9;1.1,1") },
				{ "corners", FxValue::Data("2x2:0.1,0.05;0.9,0;0,1;"
					"0.95,0.9") },
			});
			auto large = full;
			auto reduced = half;
			done = ApplyFx(
				large,
				instance,
				FxContext{ .scale = 1., .fullSize = full.size() })
				&& ApplyFx(
					reduced,
					instance,
					FxContext{ .scale = 0.5, .fullSize = full.size() })
				&& done;
			const auto difference = FxImageDifference(
				reduced,
				FxResized(large, half.size()));
			worst = std::max(worst, difference);
			if (difference >= 2.5) {
				log.push_back(u"FAIL: preview of %1 differs by %2"_q.arg(
					QString::fromLatin1(descriptor->id),
					number(difference)));
			}
		}
		check(
			done && worst < 2.5,
			u"distortion previews match the export (worst %1)"_q.arg(
				number(worst)));
	}

	// Timings.
	{
		const auto large = FxTestImage(2000, 1500);
		auto timer = QElapsedTimer();
		auto times = QStringList();
		auto done = true;
		for (const auto descriptor : FxInGroup(FxGroup::Distort)) {
			const auto instance = MakeFx(descriptor->id, {
				{ "mesh", FxValue::Data("3x3:0,0;0.5,0.1;1,0;0.1,0.5;"
					"0.6,0.4;1,0.5;0,1;0.5,0.9;1.1,1") },
				{ "corners", FxValue::Data("2x2:0.1,0.05;0.9,0;0,1;"
					"0.95,0.9") },
			});
			auto image = large;
			timer.start();
			done = ApplyFx(
				image,
				instance,
				FxContext{ .fullSize = large.size() }) && done;
			times.push_back(u"%1 %2"_q.arg(
				QString::fromLatin1(descriptor->id).mid(8),
				number(timer.nsecsElapsed() / 1e6, 0)));
		}
		info(u"2000x1500, ms: "_q + times.join(u", "_q));
		check(done, u"every distortion ran on a 3 MP picture"_q);
	}

	return ok;
}

const auto DistortSelfTest = SelfTestRegistrar(
	SelfTestSuite::Fx,
	"distort",
	&RunDistortSelfTest);

//
// The editors of the mesh and of the four corners.
//
// Both parameters are edited right on the picture: the custom editor in
// the effect card is a few buttons, one of them turns on a temporary
// canvas tool that shows the grid over the layer and lets its points be
// dragged. The tool works with the effect instance through the controller
// only (it reads the parameter and writes it back), so undo, redo and the
// card stay in sync by themselves, and the card finds out whether its
// tool is the current one by asking the controller.
//
// The card of an effect is filled with the color the common panel buttons
// and chips are made of (they are dark wells on the lighter panel), on the
// card they would be a bare text. So the controls here are painted with
// the panel color instead: pills that are lighter than the card.
//

constexpr auto kHandleRadius = 5;
constexpr auto kHandleActiveRadius = 7;
constexpr auto kHandleReach = 12;
constexpr auto kMeshLineSteps = 6;
constexpr auto kEditorSkip = 8;
constexpr auto kMeshSides = std::array<int, 5>{ { 3, 4, 5, 6, 8 } };
constexpr auto kCardPillHeight = 30;
constexpr auto kCardPillPadding = 14;
constexpr auto kCardPillTextSkip = 8;
constexpr auto kCardPillsTop = 4;
constexpr auto kCardPillsBottom = 4;
constexpr auto kCardRowHeight = 40;
constexpr auto kSidesSegment = 42;
constexpr auto kSidesLabelSkip = 12;
constexpr auto kCardSelectedFill = 0.16;
constexpr auto kCardDimmedOpacity = 0.4;

enum class GridKind : uchar {
	Mesh,
	Corners,
};

class GridTool;

[[nodiscard]] std::vector<GridTool*> &GridTools() {
	static auto result = std::vector<GridTool*>();
	return result;
}

class GridTool final : public Tool {
public:
	GridTool(
		not_null<Controller*> controller,
		GridKind kind,
		LayerId layerId,
		uint64 fxUid,
		QByteArray paramId);
	~GridTool();

	[[nodiscard]] bool edits(uint64 fxUid, const QByteArray &paramId) const;

	void deactivated() override;
	bool mousePress(const ToolMouseEvent &e) override;
	void mouseMove(const ToolMouseEvent &e) override;
	void mouseRelease(const ToolMouseEvent &e) override;
	void mouseLeave() override;
	bool cancel() override;
	void paint(QPainter &p, const ToolPaintContext &context) override;
	QCursor cursor(const ToolMouseEvent &e) override;

private:
	[[nodiscard]] const Layer *layer() const;
	[[nodiscard]] FxMesh stored(const Layer &layer) const;
	[[nodiscard]] QPointF toWidget(
		const Layer &layer,
		QPointF normalized,
		const QTransform &documentToWidget) const;
	[[nodiscard]] std::optional<QPointF> fromDocument(
		const Layer &layer,
		QPointF document) const;
	[[nodiscard]] int handleAt(
		const Layer &layer,
		const FxMesh &mesh,
		QPointF widget) const;
	void store(LayerId layerId, const FxMesh &mesh, bool commit);
	void restore(LayerId layerId);
	void finish(bool keep);

	const not_null<Controller*> _controller;
	const GridKind _kind;
	const LayerId _layerId = 0;
	const uint64 _fxUid = 0;
	const QByteArray _paramId;
	std::optional<FxValue> _initial;
	FxMesh _current;
	QPointF _grab;
	int _dragging = -1;
	int _over = -1;
	bool _moved = false;

};

[[nodiscard]] GridTool *FindGridTool(Tool *tool) {
	for (const auto known : GridTools()) {
		if (static_cast<Tool*>(known) == tool) {
			return known;
		}
	}
	return nullptr;
}

GridTool::GridTool(
	not_null<Controller*> controller,
	GridKind kind,
	LayerId layerId,
	uint64 fxUid,
	QByteArray paramId)
: _controller(controller)
, _kind(kind)
, _layerId(layerId)
, _fxUid(fxUid)
, _paramId(std::move(paramId)) {
	GridTools().push_back(this);
}

GridTool::~GridTool() {
	auto &list = GridTools();
	list.erase(ranges::remove(list, this), end(list));
}

bool GridTool::edits(uint64 fxUid, const QByteArray &paramId) const {
	return (_fxUid == fxUid) && (_paramId == paramId);
}

const Layer *GridTool::layer() const {
	if (!_controller->hasDocument()) {
		return nullptr;
	}
	const auto &document = _controller->document();
	const auto result = _layerId
		? document.find(_layerId)
		: _controller->activeLayer();
	return (result
		&& !result->size().isEmpty()
		&& LayerFx(document, result->id, _fxUid))
		? result
		: nullptr;
}

FxMesh GridTool::stored(const Layer &layer) const {
	const auto data = _controller->fxParam(layer.id, _fxUid, _paramId).data();
	return (_kind == GridKind::Corners)
		? CornersFromData(data)
		: MeshFromData(data);
}

QPointF GridTool::toWidget(
		const Layer &layer,
		QPointF normalized,
		const QTransform &documentToWidget) const {
	const auto size = layer.size();
	return documentToWidget.map(layer.transform.map(QPointF(
		normalized.x() * size.width(),
		normalized.y() * size.height())));
}

std::optional<QPointF> GridTool::fromDocument(
		const Layer &layer,
		QPointF document) const {
	const auto local = LayerPoint(layer, document);
	if (!local) {
		return std::nullopt;
	}
	const auto size = layer.size();
	return QPointF(local->x() / size.width(), local->y() / size.height());
}

int GridTool::handleAt(
		const Layer &layer,
		const FxMesh &mesh,
		QPointF widget) const {
	const auto transform = _controller->documentToWidget();
	const auto reach = double(EditorUi::Px(kHandleReach));
	auto result = -1;
	auto best = reach * reach;
	for (auto i = 0; i != int(mesh.points.size()); ++i) {
		const auto delta = toWidget(layer, mesh.points[i], transform) - widget;
		const auto distance = delta.x() * delta.x() + delta.y() * delta.y();
		if (distance <= best) {
			best = distance;
			result = i;
		}
	}
	return result;
}

void GridTool::store(LayerId layerId, const FxMesh &mesh, bool commit) {
	_controller->setFxParam(
		layerId,
		_fxUid,
		_paramId,
		FxValue::Data(FxMeshSerialize(mesh)),
		commit);
}

void GridTool::restore(LayerId layerId) {
	_controller->changeFx(layerId, _fxUid, [&](FxInstance &instance) {
		RestoreParam(instance, _paramId, _initial);
	}, false);
}

void GridTool::finish(bool keep) {
	if (_dragging < 0) {
		return;
	}
	const auto moved = _moved;
	_dragging = -1;
	_moved = false;
	if (moved) {
		if (const auto target = layer()) {
			const auto layerId = target->id;
			if (keep) {
				store(layerId, _current, true);
			} else {
				restore(layerId);
			}
		}
	}
	_controller->updateCanvas();
}

// What was dragged so far is already in the document as a live change
// (it becomes an undo step by itself). Nothing is written from here: the
// tool may be dropped in the middle of another change of the document.
void GridTool::deactivated() {
	_dragging = -1;
	_moved = false;
}

bool GridTool::mousePress(const ToolMouseEvent &e) {
	const auto target = layer();
	if (!target) {
		return false;
	}
	const auto mesh = stored(*target);
	const auto index = handleAt(*target, mesh, e.widget);
	if (index < 0) {
		return false;
	} else if (target->locked) {
		_controller->showToast(tr::lng_oblivion_photo_distort_locked(tr::now));
		return false;
	}
	const auto point = fromDocument(*target, e.document);
	const auto instance = LayerFx(_controller->document(), target->id, _fxUid);
	if (!point || !instance) {
		return false;
	}
	_initial = StoredParam(*instance, _paramId);
	_current = mesh;
	_grab = mesh.points[index] - *point;
	_dragging = index;
	_over = index;
	_moved = false;
	_controller->updateCanvas();
	return true;
}

void GridTool::mouseMove(const ToolMouseEvent &e) {
	const auto target = layer();
	if (_dragging < 0) {
		const auto over = target
			? handleAt(*target, stored(*target), e.widget)
			: -1;
		if (_over != over) {
			_over = over;
			_controller->updateCanvas();
		}
		return;
	} else if (!target || _dragging >= int(_current.points.size())) {
		return;
	}
	const auto point = fromDocument(*target, e.document);
	if (!point) {
		return;
	}
	auto next = _current;
	next.points[_dragging] = QPointF(
		std::clamp(point->x() + _grab.x(), kMeshPointMin, kMeshPointMax),
		std::clamp(point->y() + _grab.y(), kMeshPointMin, kMeshPointMax));
	if (next.points[_dragging] == _current.points[_dragging]) {
		return;
	} else if (_kind == GridKind::Corners && !ValidCorners(next)) {
		return;
	}
	const auto layerId = target->id;
	_current = std::move(next);
	_moved = true;
	store(layerId, _current, false);
	_controller->updateCanvas();
}

void GridTool::mouseRelease(const ToolMouseEvent &e) {
	finish(true);
}

void GridTool::mouseLeave() {
	if (_dragging < 0 && _over >= 0) {
		_over = -1;
		_controller->updateCanvas();
	}
}

bool GridTool::cancel() {
	if (_dragging < 0) {
		return false;
	}
	finish(false);
	return true;
}

void GridTool::paint(QPainter &p, const ToolPaintContext &context) {
	const auto target = layer();
	if (!target) {
		return;
	}
	const auto mesh = (_dragging >= 0) ? _current : stored(*target);
	if (!mesh.valid()) {
		return;
	}
	const auto place = [&](QPointF normalized) {
		return toWidget(*target, normalized, context.documentToWidget);
	};
	auto lines = QPainterPath();
	if (_kind == GridKind::Corners) {
		lines.moveTo(place(mesh.points[0]));
		lines.lineTo(place(mesh.points[1]));
		lines.lineTo(place(mesh.points[3]));
		lines.lineTo(place(mesh.points[2]));
		lines.closeSubpath();
	} else {
		const auto across = (mesh.columns - 1) * kMeshLineSteps;
		const auto along = (mesh.rows - 1) * kMeshLineSteps;
		for (auto row = 0; row != mesh.rows; ++row) {
			const auto v = row / double(mesh.rows - 1);
			for (auto i = 0; i <= across; ++i) {
				const auto point = place(
					FxMeshPoint(mesh, i / double(across), v));
				if (i) {
					lines.lineTo(point);
				} else {
					lines.moveTo(point);
				}
			}
		}
		for (auto column = 0; column != mesh.columns; ++column) {
			const auto u = column / double(mesh.columns - 1);
			for (auto i = 0; i <= along; ++i) {
				const auto point = place(
					FxMeshPoint(mesh, u, i / double(along)));
				if (i) {
					lines.lineTo(point);
				} else {
					lines.moveTo(point);
				}
			}
		}
	}
	// White with a dark halo: the grid lies over any picture, so its colors
	// are fixed, the same way the point handle of the panels is painted.
	auto hq = PainterHighQualityEnabler(p);
	const auto line = double(std::max(EditorUi::Px(1), 1));
	p.setBrush(Qt::NoBrush);
	p.setPen(QPen(
		QColor(0, 0, 0, 120),
		line + 2.,
		Qt::SolidLine,
		Qt::RoundCap,
		Qt::RoundJoin));
	p.drawPath(lines);
	p.setPen(QPen(
		QColor(255, 255, 255, 230),
		line,
		Qt::SolidLine,
		Qt::RoundCap,
		Qt::RoundJoin));
	p.drawPath(lines);
	const auto active = (_dragging >= 0) ? _dragging : _over;
	for (auto i = 0; i != int(mesh.points.size()); ++i) {
		const auto center = place(mesh.points[i]);
		const auto chosen = (i == active);
		const auto radius = double(EditorUi::Px(
			chosen ? kHandleActiveRadius : kHandleRadius));
		p.setPen(QPen(QColor(0, 0, 0, 150), line));
		p.setBrush(chosen ? st::groupCallActiveFg->c : QColor(255, 255, 255));
		p.drawEllipse(center, radius, radius);
	}
}

QCursor GridTool::cursor(const ToolMouseEvent &e) {
	return QCursor((_dragging >= 0 || _over >= 0)
		? Qt::SizeAllCursor
		: Qt::ArrowCursor);
}

void StartGridTool(
		not_null<Controller*> controller,
		GridKind kind,
		LayerId layerId,
		uint64 fxUid,
		const QByteArray &paramId) {
	controller->setTemporaryTool(std::make_unique<GridTool>(
		controller,
		kind,
		layerId,
		fxUid,
		paramId));
}

// What is chosen or turned on: the accent outline of the selected chips.
void PaintSelectedPill(QPainter &p, QRectF rect) {
	const auto line = double(std::max(EditorUi::Px(1), 1));
	const auto half = line / 2.;
	const auto radius = rect.height() / 2. - half;
	p.setPen(QPen(st::groupCallActiveFg->c, line));
	p.setBrush(anim::with_alpha(st::groupCallActiveFg->c, kCardSelectedFill));
	p.drawRoundedRect(
		rect.marginsRemoved({ half, half, half, half }),
		radius,
		radius);
}

// A button on the card: a pill of the panel color, outlined with the
// accent color while the thing it turns on is active.
class CardPill final : public Ui::RippleButton {
public:
	CardPill(QWidget *parent, rpl::producer<QString> text);

	void setActive(bool active);
	void setAvailable(bool available);
	// The width the text needs together with the paddings.
	[[nodiscard]] rpl::producer<int> naturalWidthValue() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	QImage prepareRippleMask() const override;

private:
	QString _text;
	rpl::variable<int> _naturalWidth = 0;
	bool _active = false;
	bool _available = true;

};

CardPill::CardPill(QWidget *parent, rpl::producer<QString> text)
: RippleButton(parent, st::groupCallRipple) {
	std::move(text) | rpl::on_next([=](QString value) {
		_text = std::move(value);
		_naturalWidth = st::normalFont->width(_text)
			+ 2 * EditorUi::Px(kCardPillPadding);
		update();
	}, lifetime());
}

void CardPill::setActive(bool active) {
	if (_active != active) {
		_active = active;
		update();
	}
}

void CardPill::setAvailable(bool available) {
	if (_available != available) {
		_available = available;
		setDisabled(!available);
		update();
	}
}

rpl::producer<int> CardPill::naturalWidthValue() const {
	return _naturalWidth.value();
}

void CardPill::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	{
		auto hq = PainterHighQualityEnabler(p);
		if (_active) {
			PaintSelectedPill(p, QRectF(rect()));
		} else {
			auto bg = (isOver() && _available)
				? st::groupCallMembersBgOver->c
				: st::groupCallMembersBg->c;
			if (!_available) {
				bg = anim::with_alpha(bg, kCardDimmedOpacity);
			}
			const auto radius = height() / 2.;
			p.setPen(Qt::NoPen);
			p.setBrush(bg);
			p.drawRoundedRect(rect(), radius, radius);
		}
	}
	paintRipple(p, 0, 0);
	const auto color = _active
		? st::groupCallActiveFg->c
		: st::groupCallMembersFg->c;
	const auto &font = st::normalFont;
	const auto available = std::max(
		width() - 2 * EditorUi::Px(kCardPillTextSkip),
		0);
	p.setPen(_available
		? color
		: anim::with_alpha(color, kCardDimmedOpacity));
	p.setFont(font);
	p.drawText(rect(), Qt::AlignCenter, font->elided(_text, available));
}

QImage CardPill::prepareRippleMask() const {
	return Ui::RippleAnimation::RoundRectMask(size(), height() / 2);
}

// "Grid ......... (3x3 | 4x4 | 5x5 | 6x6 | 8x8)": the label on the left as
// in the other rows of the card, the sizes on the right in one well, so
// they take the same place in every language and never wrap.
class SidesRow final : public Ui::RpWidget {
public:
	SidesRow(
		QWidget *parent,
		rpl::producer<QString> label,
		std::vector<QString> sides);

	void setSelected(int index);
	[[nodiscard]] rpl::producer<int> chosen() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	[[nodiscard]] QRect wellRect() const;
	[[nodiscard]] QRect segmentRect(int index) const;
	[[nodiscard]] int segmentAt(QPoint point) const;
	void setOver(int index);

	QString _label;
	const std::vector<QString> _sides;
	int _selected = -1;
	int _over = -1;
	int _pressed = -1;
	rpl::event_stream<int> _chosen;

};

SidesRow::SidesRow(
	QWidget *parent,
	rpl::producer<QString> label,
	std::vector<QString> sides)
: RpWidget(parent)
, _sides(std::move(sides)) {
	std::move(label) | rpl::on_next([=](QString value) {
		_label = std::move(value);
		update();
	}, lifetime());
	setMouseTracking(true);
}

void SidesRow::setSelected(int index) {
	if (_selected != index) {
		_selected = index;
		update();
	}
}

rpl::producer<int> SidesRow::chosen() const {
	return _chosen.events();
}

int SidesRow::resizeGetHeight(int newWidth) {
	return EditorUi::Px(kCardRowHeight);
}

QRect SidesRow::wellRect() const {
	const auto count = int(_sides.size());
	if (!count) {
		return QRect();
	}
	const auto label = std::min(st::normalFont->width(_label), width() / 3)
		+ EditorUi::Px(kSidesLabelSkip);
	const auto segment = std::min(
		EditorUi::Px(kSidesSegment),
		std::max(width() - label, 0) / count);
	const auto wellHeight = EditorUi::Px(kCardPillHeight);
	const auto total = segment * count;
	return QRect(
		width() - total,
		(height() - wellHeight) / 2,
		total,
		wellHeight);
}

QRect SidesRow::segmentRect(int index) const {
	const auto well = wellRect();
	const auto count = int(_sides.size());
	if (well.isEmpty() || index < 0 || index >= count) {
		return QRect();
	}
	const auto segment = well.width() / count;
	return QRect(
		well.x() + index * segment,
		well.y(),
		segment,
		well.height());
}

int SidesRow::segmentAt(QPoint point) const {
	const auto well = wellRect();
	const auto count = int(_sides.size());
	if (well.isEmpty() || !well.contains(point)) {
		return -1;
	}
	return std::clamp(
		(point.x() - well.x()) * count / well.width(),
		0,
		count - 1);
}

void SidesRow::setOver(int index) {
	if (_over != index) {
		_over = index;
		setCursor((index >= 0) ? style::cur_pointer : style::cur_default);
		update();
	}
}

void SidesRow::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto &font = st::normalFont;
	const auto well = wellRect();
	const auto count = int(_sides.size());
	const auto labelWidth = well.isEmpty()
		? width()
		: std::max(well.x() - EditorUi::Px(kSidesLabelSkip), 0);
	p.setFont(font);
	p.setPen(st::groupCallMembersFg);
	p.drawText(
		QRect(0, 0, labelWidth, height()),
		Qt::AlignLeft | Qt::AlignVCenter,
		font->elided(_label, labelWidth));
	if (well.isEmpty()) {
		return;
	}
	{
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = well.height() / 2.;
		p.setPen(Qt::NoPen);
		p.setBrush(st::groupCallMembersBg);
		p.drawRoundedRect(well, radius, radius);
		if (_over >= 0 && _over != _selected) {
			p.setBrush(st::groupCallMembersBgRipple);
			p.drawRoundedRect(segmentRect(_over), radius, radius);
		}
		if (_selected >= 0 && _selected < count) {
			PaintSelectedPill(p, QRectF(segmentRect(_selected)));
		}
	}
	for (auto i = 0; i != count; ++i) {
		p.setPen((i == _selected)
			? st::groupCallActiveFg
			: st::groupCallMembersFg);
		p.drawText(segmentRect(i), Qt::AlignCenter, _sides[i]);
	}
}

void SidesRow::mouseMoveEvent(QMouseEvent *e) {
	setOver(segmentAt(e->pos()));
}

void SidesRow::mousePressEvent(QMouseEvent *e) {
	_pressed = (e->button() == Qt::LeftButton) ? segmentAt(e->pos()) : -1;
}

void SidesRow::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, -1);
	if (pressed >= 0 && pressed == segmentAt(e->pos())) {
		_chosen.fire_copy(pressed);
	}
}

void SidesRow::leaveEventHook(QEvent *e) {
	setOver(-1);
}

class GridEditor final : public Ui::VerticalLayout {
public:
	GridEditor(QWidget *parent, GridKind kind, FxCustomEditorArgs &&args);
	~GridEditor();

protected:
	void hideEvent(QHideEvent *e) override;

private:
	[[nodiscard]] GridTool *ownTool() const;
	[[nodiscard]] const Layer *layer() const;
	[[nodiscard]] FxMesh mesh() const;
	void change(const FxMesh &mesh);
	void toggle();
	void stopTool();
	void refresh();

	const GridKind _kind;
	const base::weak_ptr<Controller> _controller;
	const LayerId _layerId = 0;
	const uint64 _fxUid = 0;
	const QByteArray _paramId;
	const Fn<void(QByteArray value, bool finished)> _changed;
	QByteArray _value;
	rpl::variable<QString> _editText;
	rpl::variable<bool> _dimmed = false;
	SidesRow *_sides = nullptr;
	CardPill *_edit = nullptr;
	rpl::lifetime _toolLifetime;

};

GridEditor::GridEditor(
	QWidget *parent,
	GridKind kind,
	FxCustomEditorArgs &&args)
: VerticalLayout(parent)
, _kind(kind)
, _controller(args.controller)
, _layerId(args.layerId)
, _fxUid(args.fxUid)
, _paramId(args.paramId)
, _changed(std::move(args.changed))
, _value(std::move(args.value))
, _editText(tr::lng_oblivion_photo_distort_edit(tr::now)) {
	// The two actions share one row right under the title of the card:
	// the wide one turns the canvas tool on and off, the short one puts
	// the points back.
	const auto skip = EditorUi::Px(kEditorSkip);
	const auto buttons = add(
		object_ptr<Ui::FixedHeightWidget>(
			this,
			EditorUi::Px(kCardPillHeight)),
		style::margins(
			0,
			EditorUi::Px(kCardPillsTop),
			0,
			EditorUi::Px(kCardPillsBottom)));
	_edit = Ui::CreateChild<CardPill>(buttons, _editText.value());
	_edit->show();
	_edit->setClickedCallback([=] {
		toggle();
	});
	const auto reset = Ui::CreateChild<CardPill>(
		buttons,
		tr::lng_oblivion_photo_distort_reset());
	reset->show();
	reset->setClickedCallback([=] {
		const auto now = mesh();
		if (!FxMeshIsRegular(now)) {
			change(FxMeshRegular(now.columns, now.rows));
		}
	});
	const auto edit = _edit;
	rpl::combine(
		buttons->widthValue(),
		reset->naturalWidthValue()
	) | rpl::on_next([=](int width, int natural) {
		const auto resetWidth = std::clamp(
			natural,
			0,
			std::max((width - skip) / 2, 0));
		const auto editWidth = std::max(width - resetWidth - skip, 0);
		edit->setGeometry(0, 0, editWidth, buttons->height());
		reset->setGeometry(
			width - resetWidth,
			0,
			resetWidth,
			buttons->height());
	}, buttons->lifetime());

	if (_kind == GridKind::Mesh) {
		auto names = std::vector<QString>();
		for (const auto side : kMeshSides) {
			names.push_back(QString::number(side)
				+ QChar(0x00D7)
				+ QString::number(side));
		}
		_sides = add(object_ptr<SidesRow>(
			this,
			tr::lng_oblivion_photo_distort_mesh_grid(),
			std::move(names)));
		_sides->chosen() | rpl::on_next([=](int index) {
			if (index < 0 || index >= int(kMeshSides.size())) {
				return;
			}
			const auto side = kMeshSides[index];
			const auto now = mesh();
			if (now.columns != side || now.rows != side) {
				change(FxMeshResampled(now, side, side));
			}
		}, _sides->lifetime());
	}

	if (args.values) {
		std::move(
			args.values
		) | rpl::on_next([=](const QByteArray &value) {
			_value = value;
			refresh();
		}, lifetime());
	}
	if (const auto controller = _controller.get()) {
		controller->toolChanges() | rpl::on_next([=] {
			refresh();
		}, _toolLifetime);
		// The effects of a locked layer can't be clicked, "Done" as well.
		controller->documentChanges() | rpl::on_next([=] {
			const auto target = ownTool() ? layer() : nullptr;
			if (target && target->locked) {
				stopTool();
			}
		}, _toolLifetime);
	}
	// An effect that is switched off is not drawn: its grid would be
	// dragged blind and the picture would jump when it is switched on.
	if (args.dimmed) {
		_dimmed = std::move(args.dimmed);
	}
	_dimmed.value() | rpl::on_next([=](bool dimmed) {
		if (dimmed) {
			stopTool();
		}
		refresh();
	}, lifetime());
}

GridEditor::~GridEditor() {
	_toolLifetime.destroy();
	stopTool();
}

// The card went out of sight together with its "Done" button (another
// tab, something shown over the editor). A minimized window is not that.
void GridEditor::hideEvent(QHideEvent *e) {
	VerticalLayout::hideEvent(e);
	if (!e->spontaneous()) {
		stopTool();
	}
}

GridTool *GridEditor::ownTool() const {
	const auto controller = _controller.get();
	const auto tool = controller ? FindGridTool(controller->tool()) : nullptr;
	return (tool && _fxUid && tool->edits(_fxUid, _paramId))
		? tool
		: nullptr;
}

const Layer *GridEditor::layer() const {
	const auto controller = _controller.get();
	return (controller && controller->hasDocument())
		? controller->document().find(_layerId
			? _layerId
			: controller->activeLayerId())
		: nullptr;
}

void GridEditor::stopTool() {
	if (ownTool()) {
		if (const auto controller = _controller.get()) {
			controller->clearTemporaryTool();
		}
	}
}

FxMesh GridEditor::mesh() const {
	return (_kind == GridKind::Corners)
		? CornersFromData(_value)
		: MeshFromData(_value);
}

void GridEditor::change(const FxMesh &mesh) {
	_value = FxMeshSerialize(mesh);
	if (_changed) {
		_changed(_value, true);
	}
	refresh();
	if (const auto controller = _controller.get()) {
		controller->updateCanvas();
	}
}

void GridEditor::toggle() {
	const auto controller = _controller.get();
	if (!controller || !_fxUid || !controller->hasDocument()) {
		return;
	} else if (ownTool()) {
		controller->clearTemporaryTool();
		return;
	}
	const auto target = layer();
	if (!target) {
		return;
	} else if (target->locked) {
		controller->showToast(tr::lng_oblivion_photo_distort_locked(tr::now));
		return;
	} else if (_dimmed.current()) {
		controller->showToast(
			tr::lng_oblivion_photo_distort_switched_off(tr::now));
		return;
	}
	StartGridTool(controller, _kind, target->id, _fxUid, _paramId);
	controller->showToast((_kind == GridKind::Corners)
		? tr::lng_oblivion_photo_distort_corners_hint(tr::now)
		: tr::lng_oblivion_photo_distort_mesh_hint(tr::now));
}

void GridEditor::refresh() {
	const auto editing = (ownTool() != nullptr);
	_editText = editing
		? tr::lng_oblivion_photo_distort_edit_done(tr::now)
		: tr::lng_oblivion_photo_distort_edit(tr::now);
	_edit->setActive(editing);
	_edit->setAvailable(_controller.get() && _fxUid);
	if (_sides) {
		const auto now = mesh();
		auto selected = -1;
		for (auto i = 0; i != int(kMeshSides.size()); ++i) {
			if (now.columns == kMeshSides[i] && now.rows == kMeshSides[i]) {
				selected = i;
			}
		}
		_sides->setSelected(selected);
	}
}

const auto RegisteredEditors = FxRegistrar([] {
	RegisterFxCustomEditor({
		.type = kMeshType,
		.create = [](
				not_null<QWidget*> parent,
				FxCustomEditorArgs &&args) -> object_ptr<Ui::RpWidget> {
			return object_ptr<GridEditor>(
				parent.get(),
				GridKind::Mesh,
				std::move(args));
		},
		.normalize = [](const QByteArray &value) {
			return FxMeshSerialize(FxMeshParse(value));
		},
	});
	RegisterFxCustomEditor({
		.type = kCornersType,
		.create = [](
				not_null<QWidget*> parent,
				FxCustomEditorArgs &&args) -> object_ptr<Ui::RpWidget> {
			return object_ptr<GridEditor>(
				parent.get(),
				GridKind::Corners,
				std::move(args));
		},
		.normalize = [](const QByteArray &value) {
			const auto parsed = FxMeshParse(value);
			return (parsed.columns == 2 && parsed.rows == 2)
				? FxMeshSerialize(parsed)
				: QByteArray();
		},
	});
});

[[nodiscard]] QByteArray SceneMesh() {
	auto mesh = FxMeshRegular(4, 4);
	mesh.points[5] += QPointF(0.07, 0.05);
	mesh.points[6] += QPointF(-0.04, 0.08);
	mesh.points[9] += QPointF(0.05, -0.06);
	mesh.points[10] += QPointF(-0.08, -0.03);
	mesh.points[3] += QPointF(-0.05, 0.04);
	mesh.points[12] += QPointF(0.04, -0.03);
	return FxMeshSerialize(mesh);
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	RegisterEditorScene({
		.name = u"photo_distort_mesh"_q,
		.tab = PhotoEditorTab::Layer,
		.prepare = [](not_null<Controller*> controller) {
			const auto layer = controller->activeLayerId();
			const auto uid = controller->addFx(layer, MakeFx("distort.mesh", {
				{ "mesh", FxValue::Data(SceneMesh()) },
			}));
			if (uid) {
				StartGridTool(controller, GridKind::Mesh, layer, uid, "mesh");
			}
		},
	});
	RegisterEditorScene({
		.name = u"photo_distort_corners"_q,
		.tab = PhotoEditorTab::Layer,
		.prepare = [](not_null<Controller*> controller) {
			const auto layer = controller->activeLayerId();
			const auto uid = controller->addFx(
				layer,
				MakeFx("distort.corners", {
					{ "corners", FxValue::Data(
						"2x2:0.12,0.08;0.9,0.02;0.04,0.95;0.97,0.84") },
				}));
			if (uid) {
				StartGridTool(
					controller,
					GridKind::Corners,
					layer,
					uid,
					"corners");
			}
		},
	});
	RegisterEditorScene({
		.name = u"photo_distort_stack"_q,
		.tab = PhotoEditorTab::Layer,
		.prepare = [](not_null<Controller*> controller) {
			const auto layer = controller->activeLayerId();
			controller->addFx(layer, MakeFx("distort.twirl", {
				{ "angle", FxValue::Number(160.) },
				{ "radius", FxValue::Number(40.) },
				{ "center", FxValue::Point(QPointF(0.62, 0.45)) },
			}));
			controller->addFx(layer, MakeFx("distort.wave", {
				{ "amplitude", FxValue::Number(18.) },
				{ "wavelength", FxValue::Number(260.) },
				{ "direction", FxValue::Number(25.) },
				{ "edges", FxValue::Integer(2) },
			}));
		},
	});
	RegisterPanelScene({
		.name = u"photo_distort_panel"_q,
		.size = QSize(EditorUi::Px(340), 0),
		.prepare = [](not_null<Controller*> controller) {
			const auto layer = controller->activeLayerId();
			for (const auto descriptor : FxInGroup(FxGroup::Distort)) {
				controller->addFx(layer, MakeFx(descriptor->id, {
					{ "mesh", FxValue::Data(SceneMesh()) },
				}));
			}
		},
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			return CreateFxStackPanel(
				parent,
				controller,
				controller->activeLayerValue());
		},
	});
	// The two cards with their own controls in the states the scenes above
	// don't have: the densest grid chosen (the last segment), other edges,
	// and a card that is switched off (its controls are dimmed).
	RegisterPanelScene({
		.name = u"photo_distort_grid_cards"_q,
		.size = QSize(EditorUi::Px(340), 0),
		.prepare = [](not_null<Controller*> controller) {
			const auto layer = controller->activeLayerId();
			controller->addFx(layer, MakeFx("distort.mesh", {
				{ "mesh", FxValue::Data(
					FxMeshSerialize(FxMeshRegular(8, 8))) },
				{ "edges", FxValue::Integer(2) },
			}));
			auto corners = MakeFx("distort.corners", {
				{ "corners", FxValue::Data(
					"2x2:0.12,0.08;0.9,0.02;0.04,0.95;0.97,0.84") },
				{ "edges", FxValue::Integer(1) },
			});
			corners.enabled = false;
			controller->addFx(layer, std::move(corners));
		},
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			return CreateFxStackPanel(
				parent,
				controller,
				controller->activeLayerValue());
		},
	});
});

} // namespace

bool FxMesh::valid() const {
	return (columns >= kFxMeshMinSide)
		&& (columns <= kFxMeshMaxSide)
		&& (rows >= kFxMeshMinSide)
		&& (rows <= kFxMeshMaxSide)
		&& (points.size() == size_t(columns) * rows);
}

bool FxRemap(
		QImage &image,
		FxEdges edges,
		const FxRowMap &map,
		const std::atomic<bool> *cancel) {
	if (image.isNull() || !map) {
		return false;
	}
	if (image.format() != QImage::Format_ARGB32_Premultiplied) {
		image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
		if (image.isNull()) {
			return false;
		}
	}
	auto result = QImage(image.size(), QImage::Format_ARGB32_Premultiplied);
	if (result.isNull()) {
		return false;
	}
	result.setDevicePixelRatio(1.);
	const auto source = Source{
		reinterpret_cast<const Pixel*>(image.constBits()),
		image.width(),
		image.height(),
		ptrdiff_t(image.bytesPerLine() / 4),
	};
	const auto to = reinterpret_cast<Pixel*>(result.bits());
	const auto stride = ptrdiff_t(result.bytesPerLine() / 4);
	switch (edges) {
	case FxEdges::Clear:
		RemapRows(source, to, stride, map, cancel, [&](float x, float y) {
			return SampleClear(source, x, y);
		});
		break;
	case FxEdges::Stretch:
		RemapRows(source, to, stride, map, cancel, [&](float x, float y) {
			return SampleStretch(source, x, y);
		});
		break;
	case FxEdges::Mirror:
		RemapRows(source, to, stride, map, cancel, [&](float x, float y) {
			return SampleMirror(source, x, y);
		});
		break;
	}
	if (Cancelled(cancel)) {
		return false;
	}
	image = std::move(result);
	return true;
}

bool FxWarpPerspective(
		QImage &image,
		const QPolygonF &quad,
		FxEdges edges,
		const std::atomic<bool> *cancel) {
	if (image.isNull()) {
		return false;
	}
	auto forward = QTransform();
	auto invertible = false;
	if (!QuadTransform(QSizeF(image.size()), quad, forward)) {
		return !Cancelled(cancel);
	}
	const auto inverse = forward.inverted(&invertible);
	if (!invertible) {
		return !Cancelled(cancel);
	}
	const auto nowhere = std::numeric_limits<float>::quiet_NaN();
	return FxRemap(image, edges, [=](
			float y,
			int count,
			float *sx,
			float *sy) {
		const auto py = y + 0.5;
		const auto baseX = inverse.m21() * py + inverse.m31();
		const auto baseY = inverse.m22() * py + inverse.m32();
		const auto baseW = inverse.m23() * py + inverse.m33();
		for (auto x = 0; x != count; ++x) {
			const auto px = x + 0.5;
			const auto w = inverse.m13() * px + baseW;
			const auto u = (inverse.m11() * px + baseX) / w;
			const auto v = (inverse.m12() * px + baseY) / w;
			const auto depth = forward.m13() * u
				+ forward.m23() * v
				+ forward.m33();
			if (depth > kMinDepth) {
				sx[x] = float(u - 0.5);
				sy[x] = float(v - 0.5);
			} else {
				sx[x] = nowhere;
				sy[x] = nowhere;
			}
		}
	}, cancel);
}

FxMesh FxMeshRegular(int columns, int rows) {
	auto result = FxMesh{
		.columns = std::clamp(columns, kFxMeshMinSide, kFxMeshMaxSide),
		.rows = std::clamp(rows, kFxMeshMinSide, kFxMeshMaxSide),
	};
	result.points.reserve(size_t(result.columns) * result.rows);
	for (auto row = 0; row != result.rows; ++row) {
		for (auto column = 0; column != result.columns; ++column) {
			result.points.push_back(RegularPoint(result, column, row));
		}
	}
	return result;
}

QByteArray FxMeshSerialize(const FxMesh &mesh) {
	if (!mesh.valid()) {
		return QByteArray();
	}
	auto result = QByteArray::number(mesh.columns)
		+ 'x'
		+ QByteArray::number(mesh.rows)
		+ ':';
	auto first = true;
	for (const auto &point : mesh.points) {
		if (!first) {
			result += ';';
		}
		first = false;
		result += MeshCoordinate(point.x());
		result += ',';
		result += MeshCoordinate(point.y());
	}
	return result;
}

FxMesh FxMeshParse(const QByteArray &data) {
	auto result = FxMesh();
	const auto cross = data.indexOf('x');
	const auto colon = data.indexOf(':');
	if (cross <= 0 || colon <= cross + 1) {
		return result;
	}
	auto columnsRead = false;
	auto rowsRead = false;
	const auto columns = data.left(cross).toInt(&columnsRead);
	const auto rows = data.mid(cross + 1, colon - cross - 1).toInt(&rowsRead);
	const auto fits = columnsRead
		&& rowsRead
		&& (columns >= kFxMeshMinSide)
		&& (columns <= kFxMeshMaxSide)
		&& (rows >= kFxMeshMinSide)
		&& (rows <= kFxMeshMaxSide);
	if (!fits) {
		return result;
	}
	const auto list = data.mid(colon + 1).split(';');
	if (list.size() != columns * rows) {
		return result;
	}
	auto points = std::vector<QPointF>();
	points.reserve(list.size());
	for (const auto &entry : list) {
		const auto comma = entry.indexOf(',');
		if (comma <= 0) {
			return result;
		}
		auto xRead = false;
		auto yRead = false;
		const auto x = entry.left(comma).toDouble(&xRead);
		const auto y = entry.mid(comma + 1).toDouble(&yRead);
		if (!xRead || !yRead || !std::isfinite(x) || !std::isfinite(y)) {
			return result;
		}
		points.push_back(QPointF(
			std::clamp(x, kMeshPointMin, kMeshPointMax),
			std::clamp(y, kMeshPointMin, kMeshPointMax)));
	}
	result.columns = columns;
	result.rows = rows;
	result.points = std::move(points);
	return result;
}

bool FxMeshIsRegular(const FxMesh &mesh) {
	if (!mesh.valid()) {
		return true;
	}
	for (auto row = 0; row != mesh.rows; ++row) {
		for (auto column = 0; column != mesh.columns; ++column) {
			const auto delta = mesh.points[size_t(row) * mesh.columns + column]
				- RegularPoint(mesh, column, row);
			if (std::abs(delta.x()) > kMeshRegularEpsilon
				|| std::abs(delta.y()) > kMeshRegularEpsilon) {
				return false;
			}
		}
	}
	return true;
}

QPointF FxMeshPoint(const FxMesh &mesh, double u, double v) {
	if (!mesh.valid()) {
		return QPointF(u, v);
	}
	const auto across = std::clamp(u, 0., 1.) * (mesh.columns - 1);
	const auto along = std::clamp(v, 0., 1.) * (mesh.rows - 1);
	const auto column = std::min(int(across), mesh.columns - 2);
	const auto row = std::min(int(along), mesh.rows - 2);
	double columnWeights[4];
	double rowWeights[4];
	SplineWeights(across - column, columnWeights);
	SplineWeights(along - row, rowWeights);
	auto result = QPointF();
	for (auto j = 0; j != 4; ++j) {
		auto line = QPointF();
		for (auto i = 0; i != 4; ++i) {
			line += columnWeights[i]
				* MeshNode(mesh, column - 1 + i, row - 1 + j);
		}
		result += rowWeights[j] * line;
	}
	return result;
}

FxMesh FxMeshResampled(const FxMesh &mesh, int columns, int rows) {
	auto result = FxMeshRegular(columns, rows);
	if (!mesh.valid()) {
		return result;
	}
	for (auto row = 0; row != result.rows; ++row) {
		for (auto column = 0; column != result.columns; ++column) {
			const auto regular = RegularPoint(result, column, row);
			result.points[size_t(row) * result.columns + column] = FxMeshPoint(
				mesh,
				regular.x(),
				regular.y());
		}
	}
	return result;
}

bool FxWarpMesh(
		QImage &image,
		const FxMesh &mesh,
		FxEdges edges,
		const std::atomic<bool> *cancel) {
	if (image.isNull()) {
		return false;
	} else if (FxMeshIsRegular(mesh)) {
		return !Cancelled(cancel);
	}
	const auto map = BuildMeshMap(mesh, image.width(), image.height());
	const auto inverse = 1.f / map.step;
	const auto lastColumn = float(map.width - 1);
	const auto lastRow = float(map.height - 1);
	return FxRemap(image, edges, [&](
			float y,
			int count,
			float *sx,
			float *sy) {
		const auto along = std::clamp(y * inverse, 0.f, lastRow);
		const auto row = std::min(int(along), map.height - 2);
		const auto ty = along - row;
		const auto upper = size_t(row) * map.width;
		const auto lower = upper + map.width;
		for (auto x = 0; x != count; ++x) {
			const auto across = std::min(x * inverse, lastColumn);
			const auto column = std::min(int(across), map.width - 2);
			const auto tx = across - column;
			const auto mix = [&](const std::vector<float> &plane) {
				return FxMix(
					FxMix(plane[upper + column], plane[upper + column + 1], tx),
					FxMix(plane[lower + column], plane[lower + column + 1], tx),
					ty);
			};
			sx[x] = mix(map.xs);
			sy[x] = mix(map.ys);
		}
	}, cancel);
}

} // namespace Oblivion::Photo
