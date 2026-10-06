/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_photo_collage.h"

#include "base/event_filter.h"
#include "base/flat_map.h"
#include "base/weak_qptr.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "oblivion/oblivion_photo_editor_canvas.h"
#include "oblivion/oblivion_photo_editor_controls.h"
#include "oblivion/oblivion_photo_panels.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/effects/animation_value.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/toast/toast.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_basic.h"
#include "styles/style_calls.h"
#include "styles/style_editor.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <crl/crl_async.h>

#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QRandomGenerator>
#include <QtGui/QClipboard>
#include <QtGui/QDragMoveEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QApplication>

#include <array>

namespace Oblivion::Photo {
namespace {

constexpr auto kPi = 3.14159265358979323846;
constexpr auto kHeroWeight = 0.62;
constexpr auto kMaxTemplates = 18;
constexpr auto kWeightEpsilon = 1e-12;
constexpr auto kLayoutEpsilon = 0.5;
constexpr auto kSlackEpsilon = 1e-6;
constexpr auto kLowestZoom = 0.02;
constexpr auto kBlurSide = 384;
constexpr auto kBlurLeast = 0.006; // Sigma, a part of the longer side.
constexpr auto kBlurMost = 0.06;
constexpr auto kBlurDimOpacity = 0.7;
constexpr auto kLevelLeastSide = 48;
constexpr auto kLevelsLimit = 12;
constexpr auto kHiddenCellsLimit = 2 * kCollageMaxCells;
constexpr auto kCanvasDefaultSide = 2560;
constexpr auto kCanvasLeastSide = 1280;
constexpr auto kCanvasLimitSide = 8192;
constexpr auto kCanvasTinySide = 16;
constexpr auto kSignatureSize = QSize(1200, 1000);
constexpr auto kTemplateOrderBias = 0.01;
constexpr auto kRenderScaleLimit = 4.;
constexpr auto kRenderPixelsLimit = 64. * 1000 * 1000;

// The longer side of a photo by the number of photos in the collage.
struct PhotoTier {
	int photos = 0;
	int side = 0;
};
constexpr auto kPhotoTiers = std::array<PhotoTier, 3>{ {
	{ 4, kCollagePhotoMaxSide },
	{ 9, 2560 },
	{ 16, 2048 },
} };
constexpr auto kPhotoCrowdSide = 1600;

struct Span {
	double from = 0.;
	double till = 0.;
};

[[nodiscard]] bool Cancelled(const std::atomic<bool> *cancel) {
	return cancel && cancel->load(std::memory_order_relaxed);
}

[[nodiscard]] double Clamp01(double value, double fallback) {
	return std::isfinite(value) ? std::clamp(value, 0., 1.) : fallback;
}

// Positive sizes that sum to 1, none smaller than the lowest allowed
// part. Valid sizes are not touched at all, so normalizing twice gives
// the same bits.
void NormalizeWeights(std::vector<double> &weights) {
	const auto count = int(weights.size());
	if (!count) {
		return;
	}
	const auto lowest = (count * kCollageMinWeight <= 1.)
		? kCollageMinWeight
		: (1. / count);
	auto total = 0.;
	auto valid = true;
	for (const auto weight : weights) {
		if (!std::isfinite(weight) || weight < lowest - kWeightEpsilon) {
			valid = false;
		}
		total += weight;
	}
	if (valid && std::abs(total - 1.) <= kWeightEpsilon) {
		return;
	}
	total = 0.;
	for (auto &weight : weights) {
		if (!std::isfinite(weight) || weight <= 0.) {
			weight = 0.;
		}
		total += weight;
	}
	if (total <= 0.) {
		ranges::fill(weights, 1. / count);
		return;
	}
	for (auto &weight : weights) {
		weight /= total;
	}
	// The ones that are too small get the lowest part, the others give
	// the room for that in proportion to their sizes.
	for (auto pass = 0; pass != count; ++pass) {
		auto fixed = 0.;
		auto rest = 0.;
		for (const auto weight : weights) {
			if (weight <= lowest) {
				fixed += lowest;
			} else {
				rest += weight;
			}
		}
		if (rest <= 0. || fixed >= 1.) {
			ranges::fill(weights, 1. / count);
			return;
		}
		const auto scale = (1. - fixed) / rest;
		auto again = false;
		for (auto &weight : weights) {
			if (weight <= lowest) {
				weight = lowest;
			} else {
				weight *= scale;
				if (weight < lowest) {
					again = true;
				}
			}
		}
		if (!again) {
			break;
		}
	}
}

// Divides [start, start + length] between the weights with gaps. With
// snap the start, the length and the spacing must be whole numbers, all
// the results are whole then too.
[[nodiscard]] std::vector<Span> Spans(
		double start,
		double length,
		const std::vector<double> &weights,
		double spacing,
		bool snap,
		double *available = nullptr) {
	const auto count = int(weights.size());
	auto result = std::vector<Span>(count);
	if (!count) {
		return result;
	}
	length = std::max(length, 0.);
	// The gaps never take more than a half of everything.
	spacing = (count > 1)
		? std::clamp(spacing, 0., length / (2. * (count - 1)))
		: 0.;
	if (snap) {
		spacing = std::floor(spacing + 1e-9);
	}
	const auto room = length - spacing * (count - 1);
	if (available) {
		*available = room;
	}
	auto total = 0.;
	for (const auto weight : weights) {
		total += weight;
	}
	if (!(total > 0.)) {
		total = 1.;
	}
	auto sum = 0.;
	auto from = start;
	for (auto i = 0; i != count; ++i) {
		sum += weights[i];
		auto till = (i + 1 == count)
			? (start + length)
			: (start + room * (sum / total) + spacing * i);
		if (snap && i + 1 != count) {
			till = std::round(till);
		}
		till = std::clamp(till, from, start + length);
		result[i] = { from, till };
		from = std::min(till + spacing, start + length);
	}
	return result;
}

struct GridSpans {
	std::vector<Span> tracks;
	std::vector<std::vector<Span>> cells;
	double tracksRoom = 0.;
	std::vector<double> cellsRoom;
	double crossStart = 0.;
	double crossLength = 0.;
};

[[nodiscard]] GridSpans ComputeSpans(
		const CollageGrid &grid,
		QSizeF size,
		double spacing,
		double margin,
		bool snap) {
	const auto width = std::max(size.width(), 0.);
	const auto height = std::max(size.height(), 0.);
	const auto shorter = std::min(width, height);
	margin = std::isfinite(margin)
		? std::clamp(margin, 0., shorter * 0.4)
		: 0.;
	spacing = std::isfinite(spacing) ? std::max(spacing, 0.) : 0.;
	if (snap) {
		margin = std::round(margin);
		spacing = std::round(spacing);
	}
	const auto mainLength = (grid.columns ? width : height) - 2. * margin;
	const auto crossLength = (grid.columns ? height : width) - 2. * margin;
	auto result = GridSpans();
	result.crossStart = margin;
	result.crossLength = std::max(crossLength, 0.);
	result.tracks = Spans(
		margin,
		mainLength,
		grid.tracks,
		spacing,
		snap,
		&result.tracksRoom);
	const auto count = std::min(grid.tracks.size(), grid.cells.size());
	result.tracks.resize(count);
	for (auto i = 0; i != int(count); ++i) {
		auto room = 0.;
		result.cells.push_back(Spans(
			margin,
			crossLength,
			grid.cells[i],
			spacing,
			snap,
			&room));
		result.cellsRoom.push_back(room);
	}
	return result;
}

[[nodiscard]] QRectF SpansRect(bool columns, Span track, Span cell) {
	return columns
		? QRectF(
			track.from,
			cell.from,
			track.till - track.from,
			cell.till - cell.from)
		: QRectF(
			cell.from,
			track.from,
			cell.till - cell.from,
			track.till - track.from);
}

[[nodiscard]] std::vector<int> Parts(const CollageGrid &grid) {
	auto result = std::vector<int>();
	for (const auto &cells : grid.cells) {
		result.push_back(int(cells.size()));
	}
	return result;
}

// The grid of a template: equal tracks of equal cells, only "one photo
// and a strip of the others" gives the single photo more room.
[[nodiscard]] CollageGrid GridFromParts(
		bool columns,
		const std::vector<int> &parts) {
	auto result = CollageGrid{ .columns = columns };
	const auto count = int(parts.size());
	for (const auto part : parts) {
		const auto cells = std::max(part, 1);
		result.tracks.push_back(1. / count);
		result.cells.push_back(std::vector<double>(cells, 1. / cells));
	}
	if (count == 2
		&& std::min(parts[0], parts[1]) == 1
		&& std::max(parts[0], parts[1]) >= 2) {
		const auto hero = (parts[0] == 1) ? 0 : 1;
		result.tracks[hero] = kHeroWeight;
		result.tracks[1 - hero] = 1. - kHeroWeight;
	}
	return result;
}

// What a layout looks like, whatever it is made of: three rows of one
// cell are the same as one column of three.
[[nodiscard]] std::vector<QRectF> Signature(const CollageGrid &grid) {
	auto result = CollageCellRects(
		GridFromParts(grid.columns, Parts(grid)),
		QSizeF(kSignatureSize),
		0.,
		0.,
		false);
	ranges::sort(result, [](const QRectF &a, const QRectF &b) {
		return (std::abs(a.y() - b.y()) > kLayoutEpsilon)
			? (a.y() < b.y())
			: (a.x() < b.x());
	});
	return result;
}

[[nodiscard]] bool SameSignature(
		const std::vector<QRectF> &a,
		const std::vector<QRectF> &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (auto i = 0; i != int(a.size()); ++i) {
		if (std::abs(a[i].x() - b[i].x()) > kLayoutEpsilon
			|| std::abs(a[i].y() - b[i].y()) > kLayoutEpsilon
			|| std::abs(a[i].width() - b[i].width()) > kLayoutEpsilon
			|| std::abs(a[i].height() - b[i].height()) > kLayoutEpsilon) {
			return false;
		}
	}
	return true;
}

// A collage is drawn from its photos at any size, so it may be asked for
// more pixels than it has (a layer enlarged by its transform), within
// reason.
[[nodiscard]] double RenderScale(QSize size, double requested) {
	if (!std::isfinite(requested) || size.isEmpty()) {
		return 1.;
	}
	const auto area = double(size.width()) * size.height();
	const auto most = std::clamp(
		std::sqrt(kRenderPixelsLimit / area),
		1.,
		kRenderScaleLimit);
	return std::clamp(requested, 1e-4, most);
}

// The longer side a photo of this size is kept at, see CollagePhotoSize().
[[nodiscard]] int PhotoSideLimit(QSize photo, int photos, QSize canvas) {
	auto result = kPhotoCrowdSide;
	for (const auto &tier : kPhotoTiers) {
		if (photos <= tier.photos) {
			result = tier.side;
			break;
		}
	}
	const auto shorter = std::min(photo.width(), photo.height());
	const auto longer = std::max(photo.width(), photo.height());
	if (!canvas.isEmpty() && shorter > 0) {
		// A photo that covers the whole canvas has a shorter side as long
		// as the longer side of the canvas. A small canvas counts as
		// a usual one: it may be made larger later.
		const auto covering = std::max({
			canvas.width(),
			canvas.height(),
			kCanvasLeastSide,
		});
		const auto enough = (qint64(covering) * longer + shorter - 1)
			/ shorter;
		result = int(std::min(qint64(result), enough));
	}
	return std::max(result, 1);
}

// The square canvas of a new collage of pictures this large.
[[nodiscard]] QSize DefaultCanvas(int longest) {
	const auto side = std::clamp(
		longest,
		kCanvasLeastSide,
		kCanvasDefaultSide);
	return QSize(side, side);
}

[[nodiscard]] CollageCell MakeCell(const CollagePhoto &photo) {
	return {
		.source = MakeCollageSource(photo.content),
		.name = photo.name,
	};
}

// The photo of a cell drawn into the pixels rectangle of the painter
// device, with rounded corners if the radii are positive.
void PaintCell(
		QPainter &p,
		const CollageCell &cell,
		QRect pixels,
		double radiusX,
		double radiusY) {
	if (!cell.source || pixels.isEmpty()) {
		return;
	}
	const auto image = QSizeF(cell.source->size());
	if (image.isEmpty()) {
		return;
	}
	const auto oriented = (cell.turns & 1) ? image.transposed() : image;
	const auto full = CollageImageRect(
		oriented,
		QSizeF(pixels.size()),
		cell.zoom,
		cell.offset);
	const auto scale = full.width() / oriented.width();
	auto factor = QSizeF(1., 1.);
	const auto level = cell.source->level(scale, &factor);
	if (level.isNull() || factor.isEmpty()) {
		return;
	}
	const auto transform = QTransform::fromScale(
		1. / factor.width(),
		1. / factor.height()
	) * CollageOrientation(
		image,
		cell.turns,
		cell.mirror
	) * QTransform::fromScale(
		scale,
		scale
	) * QTransform::fromTranslate(full.x(), full.y());
	if (radiusX <= 0. || radiusY <= 0.) {
		p.save();
		p.setClipRect(pixels);
		p.setTransform(
			transform * QTransform::fromTranslate(pixels.x(), pixels.y()));
		p.drawImage(QPointF(), level);
		p.restore();
		return;
	}
	// The photo first goes to a buffer of the cell size, then the buffer
	// fills an antialiased rounded rectangle.
	auto buffer = QImage(pixels.size(), QImage::Format_ARGB32_Premultiplied);
	if (buffer.isNull()) {
		return;
	}
	buffer.fill(Qt::transparent);
	{
		auto q = QPainter(&buffer);
		q.setRenderHints(
			QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
		q.setTransform(transform);
		q.drawImage(QPointF(), level);
	}
	p.save();
	p.setPen(Qt::NoPen);
	p.setBrush(QBrush(buffer));
	p.setBrushOrigin(pixels.topLeft());
	p.drawRoundedRect(QRectF(pixels), radiusX, radiusY);
	p.restore();
}

void PaintBackground(
		QPainter &p,
		const CollageData &data,
		int blurCell,
		QSize target) {
	const auto rect = QRect(QPoint(), target);
	switch (data.background) {
	case CollageBackground::None: return;
	case CollageBackground::Color:
		p.fillRect(rect, data.color1);
		return;
	case CollageBackground::Gradient: {
		const auto angle = data.gradientAngle * kPi / 180.;
		const auto direction = QPointF(std::cos(angle), std::sin(angle));
		const auto half = (std::abs(direction.x()) * target.width()
			+ std::abs(direction.y()) * target.height()) / 2.;
		const auto center = QPointF(target.width() / 2., target.height() / 2.);
		auto gradient = QLinearGradient(
			center - direction * half,
			center + direction * half);
		gradient.setColorAt(0., data.color1);
		gradient.setColorAt(1., data.color2);
		p.fillRect(rect, gradient);
	} return;
	case CollageBackground::Blur: break;
	}
	const auto valid = (blurCell >= 0)
		&& (blurCell < int(data.cells.size()))
		&& data.cells[blurCell].source;
	// The blur is always made at the same small size, so the preview
	// and the export look the same, and it costs nothing.
	const auto work = ScaledSize(target, ScaleForSide(target, kBlurSide));
	auto buffer = valid
		? QImage(work, QImage::Format_ARGB32_Premultiplied)
		: QImage();
	if (buffer.isNull()) {
		p.fillRect(rect, data.color1);
		return;
	}
	buffer.fill(data.color1);
	{
		auto centered = data.cells[blurCell];
		centered.zoom = 1.;
		centered.offset = QPointF(0.5, 0.5);
		auto q = QPainter(&buffer);
		q.setRenderHints(
			QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
		PaintCell(q, centered, QRect(QPoint(), work), 0., 0.);
	}
	const auto longer = std::max(work.width(), work.height());
	FxGaussianBlur(
		buffer,
		longer * (kBlurLeast + (kBlurMost - kBlurLeast) * data.blurAmount));
	p.drawImage(QRectF(rect), buffer, QRectF(buffer.rect()));
	if (data.blurDim != 0.) {
		const auto alpha = int(std::lround(
			std::abs(data.blurDim) * kBlurDimOpacity * 255.));
		p.fillRect(rect, (data.blurDim > 0.)
			? QColor(0, 0, 0, alpha)
			: QColor(255, 255, 255, alpha));
	}
}

// The shape of the cell over the photo: what cuts the photo layer made
// from a cell. Null if the cell hides nothing of the photo.
[[nodiscard]] MaskPtr CellMask(
		const CollageCell &cell,
		QRectF rect,
		double radius,
		const QTransform &imageToCollage) {
	const auto size = cell.source->size();
	const auto mapped = imageToCollage.mapRect(
		QRectF(QPointF(), QSizeF(size)));
	if (radius <= 0.
		&& rect.adjusted(-0.01, -0.01, 0.01, 0.01).contains(mapped)) {
		return nullptr;
	}
	auto invertible = false;
	const auto inverted = imageToCollage.inverted(&invertible);
	const auto maskSize = MaskSizeFor(size);
	if (!invertible || maskSize.isEmpty()) {
		return nullptr;
	}
	auto image = QImage(maskSize, QImage::Format_Grayscale8);
	if (image.isNull()) {
		return nullptr;
	}
	image.fill(0);
	{
		auto p = QPainter(&image);
		p.setRenderHint(QPainter::Antialiasing);
		p.setTransform(inverted * QTransform::fromScale(
			maskSize.width() / double(size.width()),
			maskSize.height() / double(size.height())));
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(255, 255, 255));
		if (radius > 0.) {
			p.drawRoundedRect(rect, radius, radius);
		} else {
			p.drawRect(rect);
		}
	}
	return MakeMask(std::move(image));
}

[[nodiscard]] int BlurCellAfter(
		const CollageData &data,
		const CollageSource *source) {
	if (!source) {
		return -1;
	}
	for (auto i = 0; i != int(data.cells.size()); ++i) {
		if (data.cells[i].source.get() == source) {
			return i;
		}
	}
	return -1;
}

[[nodiscard]] const CollageSource *BlurSourceOf(const CollageData &data) {
	return (data.blurCell >= 0 && data.blurCell < int(data.cells.size()))
		? data.cells[data.blurCell].source.get()
		: nullptr;
}

} // namespace

int CollageGrid::count() const {
	auto result = 0;
	for (const auto &track : cells) {
		result += int(track.size());
	}
	return result;
}

CollageGrid NormalizedCollageGrid(CollageGrid grid) {
	if (grid.tracks.empty()) {
		grid.tracks.push_back(1.);
	}
	if (int(grid.tracks.size()) > kCollageMaxTracks) {
		grid.tracks.resize(kCollageMaxTracks);
	}
	if (grid.cells.size() != grid.tracks.size()) {
		grid.cells.resize(grid.tracks.size());
	}
	for (auto &cells : grid.cells) {
		if (cells.empty()) {
			cells.push_back(1.);
		} else if (int(cells.size()) > kCollageMaxTracks) {
			cells.resize(kCollageMaxTracks);
		}
		NormalizeWeights(cells);
	}
	NormalizeWeights(grid.tracks);
	return grid;
}

CollageGrid UniformCollageGrid(int rows, int columns) {
	rows = std::clamp(rows, 1, kCollageMaxTracks);
	columns = std::clamp(columns, 1, kCollageMaxTracks);
	auto result = CollageGrid();
	result.tracks.assign(rows, 1. / rows);
	result.cells.assign(rows, std::vector<double>(columns, 1. / columns));
	return NormalizedCollageGrid(std::move(result));
}

QSize CollageGridSize(const CollageGrid &grid) {
	auto longest = 1;
	for (const auto &cells : grid.cells) {
		longest = std::max(longest, int(cells.size()));
	}
	const auto tracks = std::max(int(grid.cells.size()), 1);
	return grid.columns ? QSize(tracks, longest) : QSize(longest, tracks);
}

std::vector<CollageTemplate> CollageTemplates(int count) {
	count = std::clamp(count, 1, kCollageMaxCells);
	auto result = std::vector<CollageTemplate>();
	auto signatures = std::vector<std::vector<QRectF>>();
	const auto add = [&](bool columns, const std::vector<int> &parts) {
		if (int(result.size()) >= kMaxTemplates) {
			return;
		}
		auto grid = NormalizedCollageGrid(GridFromParts(columns, parts));
		auto signature = Signature(grid);
		for (const auto &existing : signatures) {
			if (SameSignature(existing, signature)) {
				return;
			}
		}
		auto id = QByteArray(columns ? "c" : "r");
		for (auto i = 0; i != int(parts.size()); ++i) {
			if (i) {
				id += '.';
			}
			id += QByteArray::number(parts[i]);
		}
		signatures.push_back(std::move(signature));
		result.push_back({ std::move(id), std::move(grid) });
	};

	// The numbers of tracks that can hold the photos, those that give
	// the most even picture first.
	auto tracks = std::vector<int>();
	for (auto k = 1; k <= std::min(count, kCollageMaxTracks); ++k) {
		if ((count + k - 1) / k <= kCollageMaxTracks) {
			tracks.push_back(k);
		}
	}
	const auto ideal = std::sqrt(double(count));
	ranges::stable_sort(tracks, [&](int a, int b) {
		return std::abs(a - ideal) < std::abs(b - ideal);
	});
	auto heroes = false;
	for (const auto k : tracks) {
		// One big photo and a strip of the others: right after the most
		// even layouts, they are the ones people look for.
		if (!result.empty()
			&& !heroes
			&& count >= 3
			&& count - 1 <= kCollageMaxTracks) {
			heroes = true;
			add(true, { 1, count - 1 });
			add(false, { 1, count - 1 });
			add(true, { count - 1, 1 });
			add(false, { count - 1, 1 });
		}
		const auto base = count / k;
		const auto extra = count % k;
		auto variants = std::vector<std::vector<int>>();
		auto last = std::vector<int>(k, base);
		for (auto i = 0; i != extra; ++i) {
			++last[k - 1 - i];
		}
		variants.push_back(std::move(last));
		if (extra) {
			auto first = std::vector<int>(k, base);
			for (auto i = 0; i != extra; ++i) {
				++first[i];
			}
			variants.push_back(std::move(first));
			if (k == 3) {
				auto middle = std::vector<int>(k, base);
				if (extra == 1) {
					++middle[1];
				} else {
					++middle[0];
					++middle[2];
				}
				variants.push_back(std::move(middle));
			}
		}
		for (const auto &parts : variants) {
			add(false, parts);
			add(true, parts);
		}
	}
	if (result.empty()) {
		result.push_back({ QByteArray("r1"), UniformCollageGrid(1, 1) });
	}
	return result;
}

CollageGrid DefaultCollageGrid(int count) {
	return CollageTemplates(count).front().grid;
}

bool SameCollageLayout(const CollageGrid &a, const CollageGrid &b) {
	return (a.count() == b.count())
		&& SameSignature(Signature(a), Signature(b));
}

std::vector<QRectF> CollageCellRects(
		const CollageGrid &grid,
		QSizeF size,
		double spacing,
		double margin,
		bool snap) {
	const auto spans = ComputeSpans(grid, size, spacing, margin, snap);
	auto result = std::vector<QRectF>();
	for (auto i = 0; i != int(spans.tracks.size()); ++i) {
		for (const auto &cell : spans.cells[i]) {
			result.push_back(SpansRect(grid.columns, spans.tracks[i], cell));
		}
	}
	return result;
}

std::vector<CollageSplitter> CollageSplitters(
		const CollageGrid &grid,
		QSizeF size,
		double spacing,
		double margin,
		bool snap) {
	const auto spans = ComputeSpans(grid, size, spacing, margin, snap);
	auto result = std::vector<CollageSplitter>();
	const auto tracks = int(spans.tracks.size());
	const auto cross = Span{
		spans.crossStart,
		spans.crossStart + spans.crossLength,
	};
	for (auto i = 0; i + 1 < tracks; ++i) {
		const auto gap = Span{ spans.tracks[i].till, spans.tracks[i + 1].from };
		result.push_back({
			.track = i,
			.index = -1,
			.vertical = grid.columns,
			.rect = SpansRect(grid.columns, gap, cross),
			.length = spans.tracksRoom,
		});
	}
	for (auto i = 0; i != tracks; ++i) {
		const auto &cells = spans.cells[i];
		for (auto j = 0; j + 1 < int(cells.size()); ++j) {
			const auto gap = Span{ cells[j].till, cells[j + 1].from };
			result.push_back({
				.track = i,
				.index = j,
				.vertical = !grid.columns,
				.rect = SpansRect(grid.columns, spans.tracks[i], gap),
				.length = spans.cellsRoom[i],
			});
		}
	}
	return result;
}

CollageGrid MoveCollageSplitter(
		const CollageGrid &grid,
		const CollageSplitter &splitter,
		double delta,
		bool linked) {
	auto result = grid;
	if (!(splitter.length > 0.) || !std::isfinite(delta)) {
		return result;
	}
	const auto move = [&](std::vector<double> &weights, int index) {
		if (index < 0 || index + 1 >= int(weights.size())) {
			return;
		}
		auto total = 0.;
		for (const auto weight : weights) {
			total += weight;
		}
		const auto lowest = kCollageMinWeight * total;
		const auto pair = weights[index] + weights[index + 1];
		if (!(pair > 2. * lowest)) {
			return;
		}
		const auto first = std::clamp(
			weights[index] + delta / splitter.length * total,
			lowest,
			pair - lowest);
		if (first == weights[index]) {
			// Not moved: the other size is not recomputed either, so the
			// grid stays the same bit for bit.
			return;
		}
		weights[index] = first;
		weights[index + 1] = pair - first;
	};
	if (splitter.index < 0) {
		move(result.tracks, splitter.track);
		return result;
	} else if (splitter.track < 0
		|| splitter.track >= int(result.cells.size())) {
		return result;
	}
	const auto &mine = grid.cells[splitter.track];
	const auto regular = linked && ranges::all_of(
		grid.cells,
		[&](const std::vector<double> &other) {
			if (other.size() != mine.size()) {
				return false;
			}
			for (auto i = 0; i != int(mine.size()); ++i) {
				if (std::abs(other[i] - mine[i]) > 1e-6) {
					return false;
				}
			}
			return true;
		});
	if (regular) {
		for (auto &cells : result.cells) {
			move(cells, splitter.index);
		}
	} else {
		move(result.cells[splitter.track], splitter.index);
	}
	return result;
}

double CollageMinZoom(QSizeF image, QSizeF cell) {
	if (image.isEmpty() || cell.isEmpty()) {
		return 1.;
	}
	const auto kx = cell.width() / image.width();
	const auto ky = cell.height() / image.height();
	return std::max(std::min(kx, ky) / std::max(kx, ky), kLowestZoom);
}

double CollageClampZoom(QSizeF image, QSizeF cell, double zoom) {
	return std::clamp(
		std::isfinite(zoom) ? zoom : 1.,
		CollageMinZoom(image, cell),
		kCollageMaxZoom);
}

QRectF CollageImageRect(
		QSizeF image,
		QSizeF cell,
		double zoom,
		QPointF offset) {
	if (image.isEmpty() || cell.isEmpty()) {
		return QRectF();
	}
	const auto scale = std::max(
		cell.width() / image.width(),
		cell.height() / image.height()
	) * CollageClampZoom(image, cell, zoom);
	const auto shown = image * scale;
	return QRectF(
		(cell.width() - shown.width()) * Clamp01(offset.x(), 0.5),
		(cell.height() - shown.height()) * Clamp01(offset.y(), 0.5),
		shown.width(),
		shown.height());
}

CollagePlacement PlaceCollagePhoto(
		QSizeF image,
		QSizeF cell,
		double zoom,
		QPointF offset) {
	const auto full = CollageImageRect(image, cell, zoom, offset);
	if (full.isEmpty()) {
		return {};
	}
	const auto target = full.intersected(QRectF(QPointF(), cell));
	const auto scale = full.width() / image.width();
	return {
		.source = QRectF(
			(target.x() - full.x()) / scale,
			(target.y() - full.y()) / scale,
			target.width() / scale,
			target.height() / scale),
		.target = target,
	};
}

QPointF PanCollagePhoto(
		QSizeF image,
		QSizeF cell,
		double zoom,
		QPointF offset,
		QPointF delta) {
	auto result = QPointF(
		Clamp01(offset.x(), 0.5),
		Clamp01(offset.y(), 0.5));
	const auto full = CollageImageRect(image, cell, zoom, offset);
	if (full.isEmpty()) {
		return result;
	}
	// The left edge of the photo is at slack * offset, whether the photo
	// is larger than the cell (a negative slack) or smaller.
	const auto slackX = cell.width() - full.width();
	const auto slackY = cell.height() - full.height();
	if (std::abs(slackX) > kSlackEpsilon) {
		result.setX(std::clamp(result.x() + delta.x() / slackX, 0., 1.));
	}
	if (std::abs(slackY) > kSlackEpsilon) {
		result.setY(std::clamp(result.y() + delta.y() / slackY, 0., 1.));
	}
	return result;
}

CollageZoom ZoomCollagePhoto(
		QSizeF image,
		QSizeF cell,
		double zoom,
		QPointF offset,
		QPointF anchor,
		double newZoom) {
	auto result = CollageZoom{
		.zoom = CollageClampZoom(image, cell, newZoom),
		.offset = QPointF(
			Clamp01(offset.x(), 0.5),
			Clamp01(offset.y(), 0.5)),
	};
	const auto before = CollageImageRect(image, cell, zoom, offset);
	const auto after = CollageImageRect(image, cell, result.zoom, offset);
	if (before.isEmpty() || after.isEmpty()) {
		return result;
	}
	// The point of the photo under the anchor, as a part of the photo.
	const auto u = std::clamp(
		(anchor.x() - before.x()) / before.width(),
		0.,
		1.);
	const auto v = std::clamp(
		(anchor.y() - before.y()) / before.height(),
		0.,
		1.);
	const auto slackX = cell.width() - after.width();
	const auto slackY = cell.height() - after.height();
	if (std::abs(slackX) > kSlackEpsilon) {
		result.offset.setX(std::clamp(
			(anchor.x() - u * after.width()) / slackX,
			0.,
			1.));
	}
	if (std::abs(slackY) > kSlackEpsilon) {
		result.offset.setY(std::clamp(
			(anchor.y() - v * after.height()) / slackY,
			0.,
			1.));
	}
	return result;
}

QTransform CollageOrientation(QSizeF image, int turns, bool mirror) {
	auto result = mirror
		? QTransform(-1., 0., 0., 1., image.width(), 0.)
		: QTransform();
	auto height = image.height();
	auto width = image.width();
	for (auto i = 0; i != (turns & 3); ++i) {
		// A clockwise quarter turn: (x, y) -> (height - y, x).
		result = result * QTransform(0., 1., -1., 0., height, 0.);
		std::swap(width, height);
	}
	return result;
}

CollageSource::CollageSource(std::shared_ptr<const ImageContent> content)
: _content(std::move(content)) {
}

QSize CollageSource::size() const {
	return _content ? _content->size() : QSize();
}

QImage CollageSource::level(double scale, QSizeF *factor) const {
	if (factor) {
		*factor = QSizeF(1., 1.);
	}
	if (!_content) {
		return QImage();
	}
	const auto &base = _content->image();
	if (base.isNull()
		|| base.format() != QImage::Format_ARGB32_Premultiplied) {
		return base;
	}
	// How many times the image can be halved and still have the pixels.
	auto wanted = 0;
	auto rest = std::isfinite(scale) ? std::clamp(scale, 1e-6, 1.) : 1.;
	while (rest <= 0.5 && wanted < kLevelsLimit) {
		rest *= 2.;
		++wanted;
	}
	const auto shorter = std::min(base.width(), base.height());
	while (wanted > 0 && (shorter >> wanted) < kLevelLeastSide) {
		--wanted;
	}
	if (!wanted) {
		return base;
	}
	auto result = QImage();
	{
		const auto guard = std::lock_guard(_mutex);
		while (int(_levels.size()) < wanted) {
			auto next = CollageHalfSized(
				_levels.empty() ? base : _levels.back());
			if (next.isNull()) {
				break;
			}
			_levels.push_back(std::move(next));
		}
		const auto have = std::min(wanted, int(_levels.size()));
		result = have ? _levels[have - 1] : base;
	}
	if (factor) {
		*factor = QSizeF(
			result.width() / double(base.width()),
			result.height() / double(base.height()));
	}
	return result;
}

qint64 CollageSource::memoryUsage() const {
	auto result = _content ? _content->memoryUsage() : qint64(0);
	const auto guard = std::lock_guard(_mutex);
	for (const auto &level : _levels) {
		result += qint64(level.sizeInBytes());
	}
	return result;
}

void CollageSource::memoryParts(
		const Fn<void(const void *object, qint64 bytes)> &visit) const {
	if (_content) {
		_content->memoryParts(visit);
	}
	auto levels = qint64(0);
	{
		const auto guard = std::lock_guard(_mutex);
		for (const auto &level : _levels) {
			levels += qint64(level.sizeInBytes());
		}
	}
	visit(this, levels);
}

CollageSourcePtr MakeCollageSource(
		std::shared_ptr<const ImageContent> content) {
	return content
		? std::make_shared<const CollageSource>(std::move(content))
		: nullptr;
}

QImage CollageHalfSized(const QImage &image) {
	if (image.isNull()
		|| image.format() != QImage::Format_ARGB32_Premultiplied) {
		return QImage();
	}
	const auto sourceWidth = image.width();
	const auto sourceHeight = image.height();
	const auto width = (sourceWidth + 1) / 2;
	const auto height = (sourceHeight + 1) / 2;
	auto result = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
	if (result.isNull()) {
		return QImage();
	}
	FxParallelRows(width, height, [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto top = FxRow(image, std::min(2 * y, sourceHeight - 1));
			const auto bottom = FxRow(
				image,
				std::min(2 * y + 1, sourceHeight - 1));
			const auto to = FxRow(result, y);
			for (auto x = 0; x != width; ++x) {
				const auto left = 2 * x;
				const auto right = std::min(left + 1, sourceWidth - 1);
				const auto a = top[left];
				const auto b = top[right];
				const auto c = bottom[left];
				const auto d = bottom[right];
				// Two channels at a time, premultiplied values average
				// correctly.
				const auto even = (a & 0x00FF00FFU)
					+ (b & 0x00FF00FFU)
					+ (c & 0x00FF00FFU)
					+ (d & 0x00FF00FFU)
					+ 0x00020002U;
				const auto odd = ((a >> 8) & 0x00FF00FFU)
					+ ((b >> 8) & 0x00FF00FFU)
					+ ((c >> 8) & 0x00FF00FFU)
					+ ((d >> 8) & 0x00FF00FFU)
					+ 0x00020002U;
				to[x] = ((even >> 2) & 0x00FF00FFU)
					| (((odd >> 2) & 0x00FF00FFU) << 8);
			}
		}
	});
	return result;
}

CollageData NormalizedCollage(CollageData data) {
	data.size = QSize(
		std::max(data.size.width(), 1),
		std::max(data.size.height(), 1));
	data.grid = NormalizedCollageGrid(std::move(data.grid));
	const auto shown = data.grid.count();
	auto &cells = data.cells;
	if (int(cells.size()) < shown) {
		cells.resize(shown);
	}
	while (int(cells.size()) > shown && !cells.back().source) {
		cells.pop_back();
	}
	if (int(cells.size()) > std::max(shown, kHiddenCellsLimit)) {
		cells.resize(std::max(shown, kHiddenCellsLimit));
	}
	for (auto &cell : cells) {
		cell.zoom = std::isfinite(cell.zoom)
			? std::clamp(cell.zoom, kLowestZoom, kCollageMaxZoom)
			: 1.;
		cell.offset = QPointF(
			Clamp01(cell.offset.x(), 0.5),
			Clamp01(cell.offset.y(), 0.5));
		cell.turns &= 3;
	}
	const auto part = [](double value, double most) {
		return std::isfinite(value) ? std::clamp(value, 0., most) : 0.;
	};
	data.spacing = part(data.spacing, kCollageMaxSpacing);
	data.margin = part(data.margin, kCollageMaxMargin);
	data.radius = part(data.radius, kCollageMaxRadius);
	if (!data.color1.isValid()) {
		data.color1 = QColor(255, 255, 255);
	}
	if (!data.color2.isValid()) {
		data.color2 = QColor(30, 136, 229);
	}
	if (!std::isfinite(data.gradientAngle)) {
		data.gradientAngle = 90.;
	} else if (data.gradientAngle < -360. || data.gradientAngle > 360.) {
		data.gradientAngle = std::fmod(data.gradientAngle, 360.);
	}
	if (data.blurCell < 0 || data.blurCell >= int(cells.size())) {
		data.blurCell = -1;
	}
	data.blurAmount = Clamp01(data.blurAmount, 0.5);
	data.blurDim = std::isfinite(data.blurDim)
		? std::clamp(data.blurDim, -1., 1.)
		: 0.;
	return data;
}

CollageGrid BestCollageGrid(const CollageData &data) {
	auto sizes = std::vector<QSizeF>();
	for (const auto &cell : data.cells) {
		if (cell.source && int(sizes.size()) < kCollageMaxCells) {
			sizes.push_back(CollageOrientedSize(cell));
		}
	}
	const auto count = int(sizes.size());
	const auto templates = CollageTemplates(std::max(count, 1));
	if (!count || data.size.isEmpty()) {
		return templates.front().grid;
	}
	const auto shorter = std::min(data.size.width(), data.size.height());
	auto best = 0;
	auto bestScore = -1.;
	for (auto i = 0; i != int(templates.size()); ++i) {
		const auto rects = CollageCellRects(
			templates[i].grid,
			QSizeF(data.size),
			data.spacing * shorter,
			data.margin * shorter,
			false);
		// The part of every photo that is seen when it covers its cell.
		auto seen = 0.;
		for (auto j = 0; j != count && j != int(rects.size()); ++j) {
			if (rects[j].isEmpty() || sizes[j].isEmpty()) {
				continue;
			}
			const auto ratio = (rects[j].width() / rects[j].height())
				/ (sizes[j].width() / sizes[j].height());
			seen += std::min(ratio, 1. / ratio);
		}
		const auto score = (seen / count) - i * kTemplateOrderBias;
		if (score > bestScore + 1e-9) {
			best = i;
			bestScore = score;
		}
	}
	return templates[best].grid;
}

int CollagePhotoCount(const CollageData &data) {
	return int(ranges::count_if(data.cells, [](const CollageCell &cell) {
		return cell.source != nullptr;
	}));
}

double CollageSpacing(const CollageData &data) {
	return data.spacing * std::min(data.size.width(), data.size.height());
}

double CollageMargin(const CollageData &data) {
	return data.margin * std::min(data.size.width(), data.size.height());
}

QSizeF CollageOrientedSize(const CollageCell &cell) {
	if (!cell.source) {
		return QSizeF();
	}
	const auto size = QSizeF(cell.source->size());
	return (cell.turns & 1) ? size.transposed() : size;
}

QTransform CollageCellTransform(const CollageCell &cell, QRectF rect) {
	if (!cell.source) {
		return QTransform();
	}
	const auto image = QSizeF(cell.source->size());
	const auto oriented = CollageOrientedSize(cell);
	const auto full = CollageImageRect(
		oriented,
		rect.size(),
		cell.zoom,
		cell.offset);
	if (full.isEmpty()) {
		return QTransform();
	}
	const auto scale = full.width() / oriented.width();
	return CollageOrientation(
		image,
		cell.turns,
		cell.mirror
	) * QTransform::fromScale(
		scale,
		scale
	) * QTransform::fromTranslate(rect.x() + full.x(), rect.y() + full.y());
}

CollageContent::CollageContent(CollageData data)
: _data(NormalizedCollage(std::move(data)))
, _rects(CollageCellRects(
	_data.grid,
	QSizeF(_data.size),
	CollageSpacing(_data),
	CollageMargin(_data))) {
}

QByteArray CollageContent::type() const {
	return kCollageType;
}

QSize CollageContent::size() const {
	return _data.size;
}

int CollageContent::cellAt(QPointF point) const {
	for (auto i = 0; i != int(_rects.size()); ++i) {
		if (_rects[i].contains(point)) {
			return i;
		}
	}
	return -1;
}

double CollageContent::cellRadius(int index) const {
	if (index < 0 || index >= int(_rects.size())) {
		return 0.;
	}
	const auto &rect = _rects[index];
	return std::min(
		_data.radius * std::min(_data.size.width(), _data.size.height()),
		std::min(rect.width(), rect.height()) / 2.);
}

std::vector<CollageSplitter> CollageContent::splitters() const {
	return CollageSplitters(
		_data.grid,
		QSizeF(_data.size),
		CollageSpacing(_data),
		CollageMargin(_data));
}

int CollageContent::blurCell() const {
	const auto shown = std::min(int(_rects.size()), int(_data.cells.size()));
	if (_data.blurCell >= 0
		&& _data.blurCell < shown
		&& _data.cells[_data.blurCell].source) {
		return _data.blurCell;
	}
	for (auto i = 0; i != shown; ++i) {
		if (_data.cells[i].source) {
			return i;
		}
	}
	return -1;
}

QImage CollageContent::renderBackground(
		double scale,
		const std::atomic<bool> *cancel) const {
	if (_data.background == CollageBackground::None || Cancelled(cancel)) {
		return QImage();
	}
	const auto target = ScaledSize(_data.size, RenderScale(_data.size, scale));
	auto result = QImage(target, QImage::Format_ARGB32_Premultiplied);
	if (result.isNull()) {
		return QImage();
	}
	result.fill(Qt::transparent);
	{
		auto p = QPainter(&result);
		p.setRenderHints(
			QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
		PaintBackground(p, _data, blurCell(), target);
	}
	return Cancelled(cancel) ? QImage() : result;
}

QImage CollageContent::render(const ContentRequest &request) const {
	if (Cancelled(request.cancel)) {
		return QImage();
	}
	const auto target = ScaledSize(
		_data.size,
		RenderScale(_data.size, request.scale));
	auto result = QImage(target, QImage::Format_ARGB32_Premultiplied);
	if (result.isNull()) {
		return QImage();
	}
	result.fill(Qt::transparent);
	const auto kx = target.width() / double(_data.size.width());
	const auto ky = target.height() / double(_data.size.height());
	const auto shown = std::min(int(_rects.size()), int(_data.cells.size()));
	{
		auto p = QPainter(&result);
		p.setRenderHints(
			QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
		PaintBackground(p, _data, blurCell(), target);
		for (auto i = 0; i != shown; ++i) {
			if (Cancelled(request.cancel)) {
				break;
			}
			const auto &cell = _data.cells[i];
			if (!cell.source) {
				continue;
			}
			// Whole pixels: the neighbours share their edges exactly.
			const auto &rect = _rects[i];
			const auto left = int(std::lround(rect.left() * kx));
			const auto top = int(std::lround(rect.top() * ky));
			const auto right = int(std::lround(rect.right() * kx));
			const auto bottom = int(std::lround(rect.bottom() * ky));
			const auto radius = cellRadius(i);
			PaintCell(
				p,
				cell,
				QRect(left, top, right - left, bottom - top),
				radius * kx,
				radius * ky);
		}
	}
	return Cancelled(request.cancel) ? QImage() : result;
}

qint64 CollageContent::memoryUsage() const {
	auto result = qint64(0);
	auto seen = std::vector<const void*>();
	memoryParts([&](const void *object, qint64 bytes) {
		if (!ranges::contains(seen, object)) {
			seen.push_back(object);
			result += bytes;
		}
	});
	return result;
}

void CollageContent::memoryParts(
		const Fn<void(const void *object, qint64 bytes)> &visit) const {
	visit(
		this,
		qint64(sizeof(CollageContent))
			+ qint64(_data.cells.size() * sizeof(CollageCell)));
	for (const auto &cell : _data.cells) {
		if (cell.source) {
			cell.source->memoryParts(visit);
		}
	}
}

std::shared_ptr<const CollageContent> MakeCollageContent(CollageData data) {
	return std::make_shared<const CollageContent>(std::move(data));
}

const CollageContent *AsCollage(const ContentPtr &content) {
	return (content && content->type() == kCollageType)
		? static_cast<const CollageContent*>(content.get())
		: nullptr;
}

QSize CollagePhotoSize(QSize photo, int photos, QSize canvas) {
	if (photo.isEmpty()) {
		return photo;
	}
	const auto side = PhotoSideLimit(photo, photos, canvas);
	return FitSize(photo, QSize(side, side));
}

std::vector<CollagePhoto> PrepareCollagePhotos(
		std::vector<ImportedImage> images,
		int photos,
		QSize canvas) {
	auto result = std::vector<CollagePhoto>();
	for (auto &entry : images) {
		if (entry.image.isNull()) {
			continue;
		}
		const auto side = PhotoSideLimit(entry.image.size(), photos, canvas);
		auto image = PrepareSource(entry.image, QSize(side, side));
		entry.image = QImage();
		if (image.isNull()) {
			continue;
		}
		result.push_back({
			.content = MakeImageContent(std::move(image)),
			.name = std::move(entry.name),
		});
	}
	return result;
}

CollageData AddCollagePhotos(
		CollageData data,
		const std::vector<CollagePhoto> &photos,
		int startCell,
		int *added) {
	data = NormalizedCollage(std::move(data));
	auto list = std::vector<const CollagePhoto*>();
	for (const auto &photo : photos) {
		if (photo.content && !photo.content->size().isEmpty()) {
			list.push_back(&photo);
		}
	}
	const auto count = int(list.size());
	const auto shown = data.grid.count();
	auto &cells = data.cells;
	auto next = 0;
	if (startCell >= 0 && startCell < shown && next != count) {
		cells[startCell] = MakeCell(*list[next++]);
	}
	for (auto i = 0; i != shown && next != count; ++i) {
		if (!cells[i].source) {
			cells[i] = MakeCell(*list[next++]);
		}
	}
	if (next != count && CollagePhotoCount(data) < kCollageMaxCells) {
		// No room left: a bigger grid, with the waiting photos too. A full
		// collage takes nothing more, so its grid stays as it is.
		const auto blur = BlurSourceOf(data);
		cells.erase(
			ranges::remove_if(cells, [](const CollageCell &cell) {
				return !cell.source;
			}),
			end(cells));
		while (next != count && int(cells.size()) < kCollageMaxCells) {
			cells.push_back(MakeCell(*list[next++]));
		}
		data.blurCell = BlurCellAfter(data, blur);
		data.grid = BestCollageGrid(data);
	}
	if (added) {
		*added = next;
	}
	return NormalizedCollage(std::move(data));
}

CollageData SwapCollageCells(CollageData data, int a, int b) {
	const auto count = int(data.cells.size());
	if (a < 0 || b < 0 || a >= count || b >= count || a == b) {
		return data;
	}
	const auto blur = BlurSourceOf(data);
	std::swap(data.cells[a], data.cells[b]);
	data.blurCell = BlurCellAfter(data, blur);
	return data;
}

CollageData RemoveCollagePhoto(CollageData data, int index) {
	if (index < 0 || index >= int(data.cells.size())) {
		return data;
	}
	if (data.blurCell == index) {
		data.blurCell = -1;
	}
	data.cells[index] = CollageCell();
	return NormalizedCollage(std::move(data));
}

CollageData DeleteCollageCell(CollageData data, int index) {
	data = NormalizedCollage(std::move(data));
	const auto shown = data.grid.count();
	if (index < 0 || index >= shown) {
		return data;
	} else if (shown <= 1) {
		return RemoveCollagePhoto(std::move(data), index);
	}
	const auto blur = (data.blurCell == index) ? nullptr : BlurSourceOf(data);
	auto &grid = data.grid;
	auto left = index;
	for (auto track = 0; track != int(grid.cells.size()); ++track) {
		auto &cells = grid.cells[track];
		if (left >= int(cells.size())) {
			left -= int(cells.size());
			continue;
		}
		cells.erase(begin(cells) + left);
		if (cells.empty()) {
			grid.cells.erase(begin(grid.cells) + track);
			grid.tracks.erase(begin(grid.tracks) + track);
		}
		break;
	}
	data.cells.erase(begin(data.cells) + index);
	data.blurCell = BlurCellAfter(data, blur);
	return NormalizedCollage(std::move(data));
}

CollageData WithCollageGrid(CollageData data, CollageGrid grid) {
	const auto blur = BlurSourceOf(data);
	std::stable_partition(
		begin(data.cells),
		end(data.cells),
		[](const CollageCell &cell) { return cell.source != nullptr; });
	data.blurCell = BlurCellAfter(data, blur);
	data.grid = std::move(grid);
	return NormalizedCollage(std::move(data));
}

Document CollageFromLayers(
		const Document &document,
		const std::vector<CollagePhoto> &extra,
		const QString &name,
		LayerId *created) {
	auto result = document;
	auto data = CollageData();
	data.size = document.size;
	auto position = -1;
	auto kept = std::vector<Layer>();
	for (auto &layer : result.layers) {
		const auto usable = layer.visible
			&& !layer.locked
			&& AsImage(layer.content)
			&& !layer.size().isEmpty()
			&& (int(data.cells.size()) < kCollageMaxCells);
		if (!usable) {
			kept.push_back(std::move(layer));
			continue;
		}
		if (position < 0) {
			position = int(kept.size());
		}
		data.cells.push_back({
			.source = MakeCollageSource(
				std::static_pointer_cast<const ImageContent>(layer.content)),
			.name = layer.name,
		});
	}
	for (const auto &photo : extra) {
		if (photo.content
			&& !photo.content->size().isEmpty()
			&& int(data.cells.size()) < kCollageMaxCells) {
			data.cells.push_back(MakeCell(photo));
		}
	}
	data.grid = data.cells.empty()
		? UniformCollageGrid(2, 2)
		: BestCollageGrid(data);
	result.layers = std::move(kept);
	const auto id = AddLayer(
		result,
		MakeLayer(MakeCollageContent(std::move(data)), name),
		position);
	if (created) {
		*created = id;
	}
	return result;
}

std::optional<Document> CollageToLayers(
		const Document &document,
		LayerId id,
		const QString &backgroundName,
		const QString &cellName,
		const std::atomic<bool> *cancel) {
	const auto index = document.indexOf(id);
	const auto collage = (index >= 0)
		? AsCollage(document.layers[index].content)
		: nullptr;
	if (!collage) {
		return std::nullopt;
	}
	const auto &origin = document.layers[index];
	const auto &data = collage->data();
	const auto inherit = [&](Layer &layer) {
		layer.visible = origin.visible;
		layer.opacity = origin.opacity;
		layer.blend = origin.blend;
		layer.effects = origin.effects;
		layer.transform = layer.transform * origin.transform;
	};
	auto made = std::vector<Layer>();
	if (data.background != CollageBackground::None) {
		auto image = collage->renderBackground(1., cancel);
		if (image.isNull()) {
			return std::nullopt;
		}
		auto layer = MakeImageLayer(std::move(image), backgroundName);
		inherit(layer);
		made.push_back(std::move(layer));
	}
	const auto &rects = collage->cellRects();
	const auto shown = std::min(int(rects.size()), int(data.cells.size()));
	auto number = 0;
	for (auto i = 0; i != shown; ++i) {
		if (Cancelled(cancel)) {
			return std::nullopt;
		}
		const auto &cell = data.cells[i];
		if (!cell.source
			|| rects[i].isEmpty()
			|| cell.source->size().isEmpty()) {
			continue;
		}
		++number;
		auto layer = MakeLayer(
			cell.source->content(),
			cell.name.isEmpty() ? cellName.arg(number) : cell.name);
		const auto placement = CollageCellTransform(cell, rects[i]);
		layer.mask = CellMask(
			cell,
			rects[i],
			collage->cellRadius(i),
			placement);
		layer.transform = placement;
		inherit(layer);
		made.push_back(std::move(layer));
	}
	if (made.empty()) {
		return std::nullopt;
	}
	auto result = document;
	result.layers.erase(begin(result.layers) + index);
	auto position = index;
	for (auto &layer : made) {
		AddLayer(result, std::move(layer), position++);
	}
	return result;
}

QSize CollageSizeForAspect(QSize current, int ratioWidth, int ratioHeight) {
	if (current.isEmpty() || ratioWidth <= 0 || ratioHeight <= 0) {
		return current;
	}
	const auto area = double(current.width()) * current.height();
	const auto ratio = ratioWidth / double(ratioHeight);
	auto width = std::sqrt(area * ratio);
	auto height = width / ratio;
	const auto longer = std::max(width, height);
	if (longer > kCanvasLimitSide) {
		width *= kCanvasLimitSide / longer;
		height *= kCanvasLimitSide / longer;
	}
	return QSize(
		std::max(int(std::lround(width)), kCanvasTinySide),
		std::max(int(std::lround(height)), kCanvasTinySide));
}

Document ResizedCollage(const Document &document, LayerId id, QSize size) {
	auto result = document;
	const auto layer = result.find(id);
	const auto collage = layer ? AsCollage(layer->content) : nullptr;
	if (!collage || size.isEmpty() || size == collage->size()) {
		return result;
	}
	const auto old = collage->size();
	const auto covers = layer->transform.isIdentity()
		&& (old == document.size);
	auto data = collage->data();
	data.size = size;
	layer->content = MakeCollageContent(std::move(data));
	const auto shift = QPointF(
		(size.width() - old.width()) / 2.,
		(size.height() - old.height()) / 2.);
	if (covers) {
		result.size = size;
		for (auto &other : result.layers) {
			if (other.id != id) {
				other.transform = other.transform
					* QTransform::fromTranslate(shift.x(), shift.y());
			}
		}
		result.global.crop = QRectF(0., 0., 1., 1.);
	} else {
		layer->transform = QTransform::fromTranslate(-shift.x(), -shift.y())
			* layer->transform;
	}
	return result;
}

Document CollageDocument(
		const std::vector<CollagePhoto> &photos,
		const QString &name,
		QSize canvas) {
	auto result = Document();
	auto data = CollageData();
	auto longest = 0;
	for (const auto &photo : photos) {
		if (!photo.content
			|| photo.content->size().isEmpty()
			|| int(data.cells.size()) >= kCollageMaxCells) {
			continue;
		}
		const auto size = photo.content->size();
		longest = std::max({ longest, size.width(), size.height() });
		data.cells.push_back(MakeCell(photo));
	}
	if (data.cells.empty()) {
		return result;
	}
	if (canvas.isEmpty()) {
		canvas = DefaultCanvas(longest);
	}
	result.size = canvas;
	data.size = canvas;
	data.grid = BestCollageGrid(data);
	AddLayer(result, MakeLayer(MakeCollageContent(std::move(data)), name));
	return result;
}

bool RunCollageSelfTest(QStringList &log) {
	auto ok = true;
	const auto check = [&](bool condition, const QString &what) {
		log.push_back((condition ? u"OK: "_q : u"FAIL: "_q) + what);
		ok = ok && condition;
	};
	const auto about = [](double a, double b, double epsilon = 1e-6) {
		return std::abs(a - b) <= epsilon;
	};
	// Pictures without fine detail: different ways of resampling give
	// nearly the same pixels, so only real differences are measured.
	const auto smooth = [](int width, int height, int hue) {
		auto result = QImage(
			width,
			height,
			QImage::Format_ARGB32_Premultiplied);
		auto p = QPainter(&result);
		auto gradient = QLinearGradient(0., 0., width, height);
		gradient.setColorAt(0., QColor::fromHsv(hue % 360, 150, 230));
		gradient.setColorAt(1., QColor::fromHsv((hue + 70) % 360, 200, 90));
		p.fillRect(result.rect(), gradient);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor::fromHsv((hue + 180) % 360, 120, 250));
		p.drawEllipse(
			QPointF(width * 0.3, height * 0.4),
			width * 0.18,
			height * 0.22);
		p.setBrush(QColor::fromHsv((hue + 250) % 360, 220, 140));
		p.drawEllipse(
			QPointF(width * 0.72, height * 0.65),
			width * 0.2,
			height * 0.16);
		p.end();
		return result;
	};
	const auto whole = [](double value) {
		return value == std::round(value);
	};

	// Sizes of tracks and cells.
	{
		auto grid = CollageGrid();
		grid.tracks = { 5., 0., -3., 1. };
		grid.cells = { { 1., 1. }, {}, { 0.001, 10. }, { 2. } };
		const auto normalized = NormalizedCollageGrid(grid);
		auto good = (normalized.tracks.size() == 4)
			&& (normalized.cells.size() == 4)
			&& (normalized.count() == 6);
		auto total = 0.;
		for (const auto weight : normalized.tracks) {
			total += weight;
			good = good && (weight >= kCollageMinWeight - 1e-9);
		}
		good = good && about(total, 1., 1e-9);
		for (const auto &cells : normalized.cells) {
			auto sum = 0.;
			for (const auto weight : cells) {
				sum += weight;
				good = good && (weight >= kCollageMinWeight - 1e-9);
			}
			good = good && about(sum, 1., 1e-9);
		}
		check(good, u"grid sizes are repaired, sum to 1, keep the minimum"_q);
		check(
			NormalizedCollageGrid(normalized) == normalized,
			u"a valid grid is not changed by normalizing"_q);
		const auto uniform = UniformCollageGrid(3, 4);
		check(
			(uniform.count() == 12)
				&& (CollageGridSize(uniform) == QSize(4, 3))
				&& (UniformCollageGrid(0, 99).count() == kCollageMaxTracks),
			u"uniform grids"_q);
	}

	// Templates.
	{
		auto good = true;
		auto few = false;
		auto most = 0;
		for (auto count = 1; count <= kCollageMaxCells; ++count) {
			const auto templates = CollageTemplates(count);
			most = std::max(most, int(templates.size()));
			if (templates.empty()
				|| !(DefaultCollageGrid(count) == templates.front().grid)) {
				good = false;
				continue;
			}
			if (count >= 2 && count <= 9 && templates.size() < 2) {
				few = true;
			}
			auto ids = std::vector<QByteArray>();
			for (const auto &entry : templates) {
				const auto &grid = entry.grid;
				if (grid.count() != count
					|| !(NormalizedCollageGrid(grid) == grid)
					|| ranges::contains(ids, entry.id)) {
					good = false;
				}
				ids.push_back(entry.id);
			}
			for (auto i = 0; i != int(templates.size()); ++i) {
				for (auto j = i + 1; j != int(templates.size()); ++j) {
					if (SameCollageLayout(
							templates[i].grid,
							templates[j].grid)) {
						good = false;
					}
				}
			}
		}
		check(good, u"templates for 1..36 photos: counts, ids, no twins"_q);
		check(!few, u"2..9 photos have a choice of templates"_q);
		check(
			(CollageTemplates(1).size() == 1)
				&& (CollageTemplates(2).size() == 2)
				&& (CollageTemplates(3).size() == 6)
				&& (most <= kMaxTemplates),
			u"template counts (1: 1, 2: 2, 3: 6, never over %1)"_q.arg(
				kMaxTemplates));
		auto rows = CollageGrid();
		rows.tracks = { 0.2, 0.3, 0.5 };
		rows.cells = { { 1. }, { 1. }, { 1. } };
		auto column = CollageGrid();
		column.columns = true;
		column.tracks = { 1. };
		column.cells = { { 0.6, 0.2, 0.2 } };
		check(
			SameCollageLayout(rows, column)
				&& !SameCollageLayout(rows, UniformCollageGrid(1, 3)),
			u"layouts compare by the picture, not by the sizes"_q);
	}

	// Geometry: margins, gaps, no overlaps.
	{
		struct Metrics {
			double spacing = 0.;
			double margin = 0.;
		};
		const auto sizes = {
			QSize(1200, 900),
			QSize(1001, 777),
			QSize(640, 1280),
		};
		const auto metrics = {
			Metrics{ 0., 0. },
			Metrics{ 7., 13. },
			Metrics{ 24.4, 0. },
			Metrics{ 0., 31.6 },
		};
		auto inside = true;
		auto integers = true;
		auto apart = true;
		auto covered = true;
		auto margins = true;
		auto gaps = true;
		auto checked = 0;
		for (auto count = 1; count <= 12; ++count) {
			for (const auto &entry : CollageTemplates(count)) {
				const auto &grid = entry.grid;
				for (const auto size : sizes) {
					for (const auto &metric : metrics) {
						const auto rects = CollageCellRects(
							grid,
							QSizeF(size),
							metric.spacing,
							metric.margin);
						++checked;
						if (int(rects.size()) != count) {
							inside = false;
							continue;
						}
						const auto margin = std::round(metric.margin);
						const auto spacing = std::round(metric.spacing);
						const auto inner = QRectF(
							margin,
							margin,
							size.width() - 2 * margin,
							size.height() - 2 * margin);
						auto area = 0.;
						auto bounds = QRectF();
						for (const auto &rect : rects) {
							if (!whole(rect.x())
								|| !whole(rect.y())
								|| !whole(rect.width())
								|| !whole(rect.height())) {
								integers = false;
							}
							if (rect.width() <= 0.
								|| rect.height() <= 0.
								|| !inner.contains(rect)) {
								inside = false;
							}
							area += rect.width() * rect.height();
							bounds = bounds.united(rect);
						}
						for (auto i = 0; i != count; ++i) {
							for (auto j = i + 1; j != count; ++j) {
								const auto common = rects[i].intersected(
									rects[j]);
								if (common.width() > 0.
									&& common.height() > 0.) {
									apart = false;
								}
							}
						}
						if (bounds != inner) {
							margins = false;
						}
						if (spacing == 0.
							&& !about(area, inner.width() * inner.height())) {
							covered = false;
						}
						// Neighbours in a track are exactly a gap apart.
						auto index = 0;
						for (const auto &cells : grid.cells) {
							for (auto j = 0; j + 1 < int(cells.size()); ++j) {
								const auto &a = rects[index + j];
								const auto &b = rects[index + j + 1];
								const auto gap = grid.columns
									? (b.top() - a.bottom())
									: (b.left() - a.right());
								if (gap != spacing) {
									gaps = false;
								}
							}
							index += int(cells.size());
						}
					}
				}
			}
		}
		log.push_back(u"   %1 template layouts measured"_q.arg(checked));
		check(integers, u"cells are whole pixels"_q);
		check(inside, u"cells are inside the margins and not empty"_q);
		check(apart, u"cells never overlap"_q);
		check(margins, u"the outer cells touch the margins exactly"_q);
		check(covered, u"without spacing the cells leave no gaps"_q);
		check(gaps, u"neighbours are exactly the spacing apart"_q);

		const auto loose = CollageCellRects(
			UniformCollageGrid(2, 2),
			QSizeF(101., 51.),
			1.5,
			2.5,
			false);
		check(
			(loose.size() == 4)
				&& about(loose[0].x(), 2.5)
				&& about(loose[0].width(), (101. - 5. - 1.5) / 2.)
				&& about(loose[3].right(), 98.5)
				&& about(loose[3].bottom(), 48.5),
			u"unsnapped geometry keeps fractions"_q);
		const auto huge = CollageCellRects(
			UniformCollageGrid(3, 3),
			QSizeF(90., 90.),
			1000.,
			1000.);
		auto sane = (huge.size() == 9);
		for (const auto &rect : huge) {
			sane = sane
				&& (rect.width() >= 0.)
				&& (rect.height() >= 0.)
				&& QRectF(0., 0., 90., 90.).contains(rect.center());
		}
		check(sane, u"absurd spacing and margin are limited"_q);
	}

	// Splitters.
	{
		const auto grid = UniformCollageGrid(2, 3);
		const auto size = QSizeF(1200., 800.);
		const auto splitters = CollageSplitters(grid, size, 10., 20.);
		const auto rects = CollageCellRects(grid, size, 10., 20.);
		check(
			(splitters.size() == 5)
				&& (splitters[0].index == -1)
				&& !splitters[0].vertical
				&& splitters[1].vertical
				&& about(splitters[0].rect.top(), rects[0].bottom())
				&& about(splitters[0].rect.bottom(), rects[3].top())
				&& about(splitters[0].rect.left(), 20.)
				&& about(splitters[0].rect.right(), 1180.)
				&& about(splitters[1].rect.left(), rects[0].right())
				&& about(splitters[1].rect.right(), rects[1].left())
				&& about(splitters[1].rect.top(), rects[0].top())
				&& about(splitters[1].rect.bottom(), rects[0].bottom())
				&& about(splitters[0].length, 800. - 40. - 10.)
				&& about(splitters[1].length, 1200. - 40. - 20.),
			u"splitters sit in the gaps"_q);

		const auto moved = MoveCollageSplitter(
			grid,
			splitters[0],
			splitters[0].length * 0.1);
		check(
			about(moved.tracks[0], 0.6)
				&& about(moved.tracks[1], 0.4)
				&& (moved.cells == grid.cells),
			u"a track splitter moves by the dragged distance"_q);
		const auto forward = MoveCollageSplitter(grid, splitters[0], 1e9);
		const auto back = MoveCollageSplitter(grid, splitters[0], -1e9);
		check(
			about(forward.tracks[1], kCollageMinWeight)
				&& about(forward.tracks[0], 1. - kCollageMinWeight)
				&& about(back.tracks[0], kCollageMinWeight)
				&& (NormalizedCollageGrid(forward) == forward),
			u"a splitter stops at the smallest cell size"_q);
		const auto linked = MoveCollageSplitter(
			grid,
			splitters[1],
			splitters[1].length * 0.05);
		const auto alone = MoveCollageSplitter(
			grid,
			splitters[1],
			splitters[1].length * 0.05,
			false);
		check(
			about(linked.cells[0][0], 1. / 3. + 0.05)
				&& about(linked.cells[0][1], 1. / 3. - 0.05)
				&& about(linked.cells[0][2], 1. / 3.)
				&& (linked.cells[1] == linked.cells[0])
				&& (alone.cells[0] == linked.cells[0])
				&& (alone.cells[1] == grid.cells[1]),
			u"cell splitters: the whole column in a regular grid"_q);
		const auto after = MoveCollageSplitter(
			alone,
			splitters[1],
			splitters[1].length * 0.05);
		check(
			(after.cells[1] == grid.cells[1])
				&& about(after.cells[0][0], 1. / 3. + 0.1),
			u"an irregular grid moves one splitter only"_q);
		auto sums = true;
		for (const auto &result : { moved, forward, back, linked, alone }) {
			auto total = 0.;
			for (const auto weight : result.tracks) {
				total += weight;
			}
			sums = sums && about(total, 1., 1e-9);
			for (const auto &cells : result.cells) {
				auto sum = 0.;
				for (const auto weight : cells) {
					sum += weight;
				}
				sums = sums && about(sum, 1., 1e-9);
			}
		}
		check(sums, u"moving splitters keeps the sums"_q);
		auto bad = splitters[0];
		bad.track = 7;
		check(
			(MoveCollageSplitter(grid, bad, 50.) == grid)
				&& (MoveCollageSplitter(grid, splitters[1], std::nan("")) == grid),
			u"a wrong splitter changes nothing"_q);
		// Sizes whose sum is not exact in binary: a drag that comes back
		// to where it started must give the very same grid, or it would
		// be an undo step that changes nothing.
		auto uneven = CollageGrid();
		uneven.tracks = { 0.1, 0.2, 0.7 };
		uneven.cells = { { 0.3, 0.7 }, { 0.1, 0.2, 0.7 }, { 1. } };
		uneven = NormalizedCollageGrid(std::move(uneven));
		auto still = true;
		for (const auto &splitter : CollageSplitters(uneven, size, 10., 20.)) {
			still = still
				&& (MoveCollageSplitter(uneven, splitter, 0.) == uneven)
				&& (MoveCollageSplitter(uneven, splitter, 0., false) == uneven);
		}
		check(still, u"a splitter that is not moved keeps the grid exactly"_q);
		check(
			MoveCollageSplitter(forward, splitters[0], 1e9) == forward,
			u"a splitter pushed against its limit keeps the grid exactly"_q);
	}

	// A photo in its cell.
	{
		const auto image = QSizeF(4000., 3000.);
		const auto cell = QSizeF(500., 500.);
		const auto center = QPointF(0.5, 0.5);
		const auto cover = PlaceCollagePhoto(image, cell, 1., center);
		check(
			about(cover.target.x(), 0.)
				&& about(cover.target.y(), 0.)
				&& about(cover.target.width(), 500.)
				&& about(cover.target.height(), 500.)
				&& about(cover.source.width(), 3000.)
				&& about(cover.source.height(), 3000.)
				&& about(cover.source.x(), 500.)
				&& about(cover.source.y(), 0.),
			u"zoom 1 covers the cell with the middle of the photo"_q);
		const auto leftSide = PlaceCollagePhoto(image, cell, 1., QPointF(0., 0.5));
		const auto rightSide = PlaceCollagePhoto(image, cell, 1., QPointF(1., 0.5));
		check(
			about(leftSide.source.x(), 0.)
				&& about(rightSide.source.right(), 4000.),
			u"offsets 0 and 1 show the edges of the photo"_q);
		const auto lowest = CollageMinZoom(image, cell);
		const auto contain = PlaceCollagePhoto(image, cell, lowest, center);
		check(
			about(lowest, 0.75)
				&& about(contain.source.width(), 4000.)
				&& about(contain.source.height(), 3000.)
				&& about(contain.target.width(), 500.)
				&& about(contain.target.height(), 375.)
				&& about(contain.target.y(), 62.5)
				&& about(CollageClampZoom(image, cell, 0.1), lowest)
				&& about(CollageClampZoom(image, cell, 100.), kCollageMaxZoom),
			u"the smallest zoom shows the whole photo"_q);
		auto aspects = true;
		for (const auto zoom : { 1., 1.7, 4., 8. }) {
			for (const auto offset : { QPointF(0., 0.), QPointF(0.3, 0.9) }) {
				const auto placed = PlaceCollagePhoto(
					image,
					QSizeF(640., 360.),
					zoom,
					offset);
				aspects = aspects
					&& about(
						placed.source.width() / placed.source.height(),
						640. / 360.,
						1e-9)
					&& QRectF(QPointF(), image).adjusted(
						-1e-6,
						-1e-6,
						1e-6,
						1e-6).contains(placed.source)
					&& about(placed.target.width(), 640.)
					&& about(placed.target.height(), 360.);
			}
		}
		check(aspects, u"the visible part has the shape of the cell"_q);

		const auto panned = PanCollagePhoto(
			image,
			cell,
			1.,
			center,
			QPointF(50., 50.));
		const auto before = CollageImageRect(image, cell, 1., center);
		const auto after = CollageImageRect(image, cell, 1., panned);
		check(
			about(after.x() - before.x(), 50.)
				&& about(after.y(), before.y())
				&& about(panned.y(), 0.5),
			u"a drag moves the photo by the same distance"_q);
		const auto stuck = PanCollagePhoto(
			image,
			cell,
			1.,
			center,
			QPointF(1e6, -1e6));
		check(
			about(stuck.x(), 0.) && about(stuck.y(), 0.5),
			u"a photo can't be dragged out of its cell"_q);
		const auto inner = PanCollagePhoto(
			image,
			cell,
			lowest,
			center,
			QPointF(0., 20.));
		check(
			about(CollageImageRect(image, cell, lowest, inner).y(), 82.5),
			u"a photo smaller than its cell moves inside it"_q);

		const auto anchor = QPointF(100., 400.);
		const auto zoomed = ZoomCollagePhoto(
			image,
			cell,
			1.5,
			QPointF(0.4, 0.6),
			anchor,
			3.);
		const auto was = CollageImageRect(image, cell, 1.5, QPointF(0.4, 0.6));
		const auto now = CollageImageRect(
			image,
			cell,
			zoomed.zoom,
			zoomed.offset);
		check(
			about(zoomed.zoom, 3.)
				&& about(
					(anchor.x() - was.x()) / was.width(),
					(anchor.x() - now.x()) / now.width(),
					1e-9)
				&& about(
					(anchor.y() - was.y()) / was.height(),
					(anchor.y() - now.y()) / now.height(),
					1e-9),
			u"zooming keeps the point under the cursor"_q);
		const auto out = ZoomCollagePhoto(image, cell, 3., zoomed.offset, anchor, 0.);
		check(
			about(out.zoom, lowest)
				&& (out.offset.x() >= 0.)
				&& (out.offset.x() <= 1.),
			u"zoom is limited"_q);

		const auto turned = CollageOrientation(QSizeF(40., 30.), 1, false);
		const auto mirrored = CollageOrientation(QSizeF(40., 30.), 0, true);
		const auto both = CollageOrientation(QSizeF(40., 30.), 3, true);
		check(
			(turned.map(QPointF(0., 0.)) == QPointF(30., 0.))
				&& (turned.map(QPointF(40., 30.)) == QPointF(0., 40.))
				&& (mirrored.map(QPointF(0., 0.)) == QPointF(40., 0.))
				&& both.mapRect(QRectF(0., 0., 40., 30.)) == QRectF(0., 0., 30., 40.),
			u"turns and the mirror"_q);
	}

	// The pyramid.
	const auto photo = [](int width, int height, bool alpha = false) {
		return MakeImageContent(FxTestImage(width, height, alpha));
	};
	{
		auto flat = QImage(5, 3, QImage::Format_ARGB32_Premultiplied);
		flat.fill(QColor(200, 100, 50));
		const auto half = CollageHalfSized(flat);
		check(
			(half.size() == QSize(3, 2))
				&& (half.pixel(0, 0) == flat.pixel(0, 0))
				&& (half.pixel(2, 1) == flat.pixel(4, 2)),
			u"a halved image keeps flat colors"_q);
		auto pair = QImage(2, 2, QImage::Format_ARGB32_Premultiplied);
		pair.setPixel(0, 0, qRgba(255, 0, 0, 255));
		pair.setPixel(1, 0, qRgba(0, 0, 0, 0));
		pair.setPixel(0, 1, qRgba(255, 0, 0, 255));
		pair.setPixel(1, 1, qRgba(0, 0, 0, 0));
		const auto mixed = CollageHalfSized(pair);
		check(
			(mixed.size() == QSize(1, 1))
				&& (FxRow(mixed, 0)[0] == 0x80800000U)
				&& CollageHalfSized(QImage()).isNull(),
			u"halving averages premultiplied pixels"_q);

		const auto source = MakeCollageSource(photo(1000, 600));
		auto factor = QSizeF();
		const auto full = source->level(0.7, &factor);
		const auto full2 = source->level(1., nullptr);
		auto good = (full.size() == QSize(1000, 600))
			&& about(factor.width(), 1.)
			&& (full2.size() == QSize(1000, 600));
		const auto second = source->level(0.5, &factor);
		good = good
			&& (second.size() == QSize(500, 300))
			&& about(factor.width(), 0.5);
		const auto same = source->level(0.26, &factor);
		good = good
			&& (same.size() == QSize(500, 300))
			&& about(factor.height(), 0.5);
		const auto third = source->level(0.25, &factor);
		good = good
			&& (third.size() == QSize(250, 150))
			&& about(factor.height(), 0.25);
		const auto tiny = source->level(0.0001, &factor);
		good = good
			&& (std::min(tiny.width(), tiny.height()) >= kLevelLeastSide)
			&& (tiny.width() < 250);
		check(good, u"the pyramid gives the smallest copy that is enough"_q);
		check(
			source->memoryUsage() > qint64(1000) * 600 * 4,
			u"the pyramid counts its memory"_q);
		auto parts = qint64(0);
		auto objects = std::vector<const void*>();
		source->memoryParts([&](const void *object, qint64 bytes) {
			parts += bytes;
			objects.push_back(object);
		});
		check(
			(objects.size() == 2)
				&& (objects[0] == source->content().get())
				&& (objects[1] == source.get())
				&& (parts == source->memoryUsage()),
			u"a source reports the photo and its copies apart"_q);
		check(
			!MakeCollageSource(nullptr),
			u"no source without an image"_q);
	}

	// Rendering.
	const auto red = QColor(200, 30, 40);
	const auto collageOf = [&](int count, QSize size) {
		auto data = CollageData();
		data.size = size;
		for (auto i = 0; i != count; ++i) {
			data.cells.push_back({
				.source = MakeCollageSource(
					photo(800 + 100 * i, 600 + 50 * (i % 2))),
				.name = u"p%1"_q.arg(i),
			});
		}
		data.grid = DefaultCollageGrid(std::max(count, 1));
		data.color1 = red;
		return data;
	};
	{
		auto data = collageOf(2, QSize(800, 600));
		data.spacing = 0.05; // 30 px.
		data.margin = 0.1; // 60 px.
		const auto content = MakeCollageContent(data);
		const auto &rects = content->cellRects();
		check(
			(content->type() == kCollageType)
				&& (content->size() == QSize(800, 600))
				&& AsCollage(content)
				&& !AsCollage(photo(10, 10))
				&& (rects.size() == 2)
				&& (rects[0] == QRectF(60., 60., 325., 480.))
				&& (rects[1] == QRectF(415., 60., 325., 480.))
				&& (content->cellAt(QPointF(100., 100.)) == 0)
				&& (content->cellAt(QPointF(500., 100.)) == 1)
				&& (content->cellAt(QPointF(400., 100.)) == -1)
				&& (content->splitters().size() == 1),
			u"a collage content and its cells"_q);
		const auto full = content->render({});
		const auto opaqueRed = red.rgba();
		check(
			(full.size() == QSize(800, 600))
				&& (full.format() == QImage::Format_ARGB32_Premultiplied)
				&& (full.pixel(10, 10) == opaqueRed)
				&& (full.pixel(400, 300) == opaqueRed)
				&& (full.pixel(59, 300) == opaqueRed)
				&& (full.pixel(60, 300) != opaqueRed)
				&& (full.pixel(384, 300) != opaqueRed)
				&& (full.pixel(385, 300) == opaqueRed)
				&& (full.pixel(415, 61) != opaqueRed)
				&& (full.pixel(739, 539) != opaqueRed)
				&& (full.pixel(740, 539) == opaqueRed),
			u"photos are in their cells, the background around"_q);
		check(
			full == content->render({}),
			u"rendering is deterministic"_q);
		const auto preview = content->render({ .scale = 0.5, .preview = true });
		const auto difference = FxImageDifference(preview, full);
		log.push_back(u"   preview against export: %1"_q.arg(difference));
		check(
			(preview.size() == QSize(400, 300)) && (difference < 2.5),
			u"the preview looks like the export"_q);
		const auto doubled = content->render({ .scale = 2. });
		check(
			(doubled.size() == QSize(1600, 1200))
				&& (FxImageDifference(full, doubled) < 2.5)
				&& (content->render({ .scale = 1000. }).size()
					== QSize(3200, 2400)),
			u"an enlarged collage is drawn from the photos, within reason"_q);
		auto flag = std::atomic<bool>(true);
		check(
			content->render({ .cancel = &flag }).isNull()
				&& content->renderBackground(1., &flag).isNull(),
			u"a cancelled render gives nothing"_q);

		// The photo itself: the cell shows the middle of the photo.
		const auto &cell = content->data().cells[0];
		const auto transform = CollageCellTransform(cell, rects[0]);
		const auto mapped = transform.mapRect(
			QRectF(QPointF(), QSizeF(cell.source->size())));
		check(
			about(mapped.height(), 480.)
				&& about(mapped.top(), 60.)
				&& about(mapped.center().x(), rects[0].center().x()),
			u"the transform of a cell covers it"_q);

		auto rounded = data;
		rounded.radius = 0.2;
		rounded.cells[1] = CollageCell();
		const auto round = MakeCollageContent(rounded);
		const auto image = round->render({});
		check(
			about(round->cellRadius(0), 120.)
				&& (image.pixel(61, 61) == opaqueRed)
				&& (image.pixel(383, 538) == opaqueRed)
				&& (image.pixel(222, 300) != opaqueRed)
				&& (image.pixel(222, 62) != opaqueRed),
			u"rounded corners show the background"_q);
		check(
			(image.pixel(500, 300) == opaqueRed)
				&& (CollagePhotoCount(round->data()) == 1),
			u"an empty cell shows the background"_q);

		auto clear = data;
		clear.background = CollageBackground::None;
		const auto none = MakeCollageContent(clear);
		check(
			(qAlpha(none->render({}).pixel(10, 10)) == 0)
				&& none->renderBackground(1.).isNull(),
			u"no background leaves the gaps transparent"_q);
		auto gradient = data;
		gradient.background = CollageBackground::Gradient;
		gradient.color1 = QColor(0, 0, 0);
		gradient.color2 = QColor(255, 255, 255);
		gradient.gradientAngle = 90.;
		const auto shaded = MakeCollageContent(gradient)->renderBackground(1.);
		check(
			(qRed(shaded.pixel(400, 2)) < 10)
				&& (qRed(shaded.pixel(400, 597)) > 245)
				&& (std::abs(qRed(shaded.pixel(5, 300))
					- qRed(shaded.pixel(790, 300))) <= 1),
			u"a gradient background follows its angle"_q);
		auto blurred = data;
		blurred.background = CollageBackground::Blur;
		blurred.blurCell = 1;
		const auto soft = MakeCollageContent(blurred);
		const auto softFull = soft->renderBackground(1.);
		const auto softHalf = soft->renderBackground(0.5);
		const auto softDifference = FxImageDifference(softHalf, softFull);
		log.push_back(u"   blurred background, preview against export: %1"_q.arg(
			softDifference));
		check(
			(soft->blurCell() == 1)
				&& (softFull.size() == QSize(800, 600))
				&& (qAlpha(softFull.pixel(3, 3)) >= 254)
				&& (softDifference < 1.5),
			u"a blurred photo background is the same at any scale"_q);
		blurred.blurCell = 17;
		const auto fallback = MakeCollageContent(blurred);
		blurred.blurDim = 1.;
		const auto dimmed = MakeCollageContent(blurred);
		const auto plainRed = qRed(
			fallback->renderBackground(1.).pixel(400, 300));
		const auto dimmedRed = qRed(
			dimmed->renderBackground(1.).pixel(400, 300));
		check(
			(fallback->blurCell() == 0)
				&& (dimmed->blurCell() == 0)
				&& (dimmedRed <= plainRed * (1. - kBlurDimOpacity) + 2.),
			u"the blurred background: the first photo, dimming"_q);
	}

	// Photos in and out, grids.
	{
		auto data = collageOf(0, QSize(600, 600));
		data.grid = UniformCollageGrid(2, 2);
		data = NormalizedCollage(std::move(data));
		check(
			(data.cells.size() == 4)
				&& (CollagePhotoCount(data) == 0)
				&& (NormalizedCollage(data) == data),
			u"an empty collage has empty cells"_q);
		auto photos = std::vector<CollagePhoto>();
		for (auto i = 0; i != 6; ++i) {
			photos.push_back({ photo(300, 200), u"n%1"_q.arg(i) });
		}
		const auto two = std::vector<CollagePhoto>{ photos[0], photos[1] };
		const auto placed = AddCollagePhotos(data, two, 2);
		check(
			(placed.grid == data.grid)
				&& (placed.cells[2].name == u"n0"_q)
				&& (placed.cells[0].name == u"n1"_q)
				&& !placed.cells[1].source
				&& !placed.cells[3].source,
			u"photos go to the chosen cell, then to the empty ones"_q);
		const auto grown = AddCollagePhotos(placed, photos);
		check(
			(grown.grid.count() == 8)
				&& (CollagePhotoCount(grown) == 8)
				&& (grown.cells[1].name == u"n0"_q)
				&& (grown.cells[7].name == u"n5"_q),
			u"more photos than cells make the grid bigger"_q);
		const auto fewer = WithCollageGrid(grown, UniformCollageGrid(1, 3));
		const auto again = WithCollageGrid(fewer, DefaultCollageGrid(8));
		check(
			(fewer.grid.count() == 3)
				&& (fewer.cells.size() == 8)
				&& (MakeCollageContent(fewer)->cellRects().size() == 3)
				&& (again.cells == grown.cells),
			u"a smaller grid keeps the photos that do not fit"_q);
		const auto swapped = SwapCollageCells(grown, 0, 7);
		check(
			(swapped.cells[0].name == u"n5"_q)
				&& (swapped.cells[7].source == grown.cells[0].source)
				&& (SwapCollageCells(grown, 0, 99) == grown),
			u"swapping cells"_q);
		const auto removed = RemoveCollagePhoto(grown, 3);
		const auto deleted = DeleteCollageCell(grown, 3);
		check(
			(removed.grid == grown.grid)
				&& !removed.cells[3].source
				&& (CollagePhotoCount(removed) == 7)
				&& (deleted.grid.count() == 7)
				&& (deleted.cells.size() == 7)
				&& (deleted.cells[3].source == grown.cells[4].source)
				&& (NormalizedCollageGrid(deleted.grid) == deleted.grid),
			u"removing a photo and deleting a cell"_q);
		auto single = collageOf(1, QSize(300, 300));
		const auto last = DeleteCollageCell(single, 0);
		check(
			(last.grid.count() == 1) && !last.cells[0].source,
			u"the last cell stays, empty"_q);
		auto spread = placed;
		const auto closed = WithCollageGrid(spread, DefaultCollageGrid(2));
		check(
			closed.cells[0].source
				&& closed.cells[1].source
				&& (closed.cells.size() == 2),
			u"a template closes the empty cells up"_q);
		auto many = std::vector<CollagePhoto>();
		for (auto i = 0; i != kCollageMaxCells + 5; ++i) {
			many.push_back({ photos[0].content, QString() });
		}
		auto taken = -1;
		const auto limited = AddCollagePhotos(data, many, -1, &taken);
		check(
			(limited.grid.count() == kCollageMaxCells)
				&& (CollagePhotoCount(limited) == kCollageMaxCells)
				&& (taken == kCollageMaxCells),
			u"a collage has at most %1 photos"_q.arg(kCollageMaxCells));
		auto counted = -1;
		const auto counting = AddCollagePhotos(placed, photos, 1, &counted);
		check(
			(counted == 6) && (CollagePhotoCount(counting) == 8),
			u"the photos that were put in are counted"_q);

		// A full collage: nothing is added, so nothing changes. The grid
		// with the splitters moved by hand stays too.
		auto tuned = limited;
		tuned.grid.tracks[0] += 0.05;
		tuned.grid.tracks[1] -= 0.05;
		tuned = NormalizedCollage(std::move(tuned));
		auto none = -1;
		const auto full = AddCollagePhotos(tuned, two, -1, &none);
		auto packed = WithCollageGrid(limited, UniformCollageGrid(2, 2));
		auto nothing = -1;
		const auto dense = AddCollagePhotos(packed, two, -1, &nothing);
		auto one = -1;
		const auto swapped36 = AddCollagePhotos(tuned, two, 5, &one);
		check(
			(none == 0)
				&& (full == tuned)
				&& (nothing == 0)
				&& (dense == packed)
				&& (dense.grid.count() == 4),
			u"a full collage takes no more photos and keeps its grid"_q);
		check(
			(one == 1)
				&& (swapped36.grid == tuned.grid)
				&& (swapped36.cells[5].name == u"n0"_q)
				&& (CollagePhotoCount(swapped36) == kCollageMaxCells),
			u"a photo of a full collage can still be replaced"_q);
		auto nearly = std::vector<CollagePhoto>(
			begin(many),
			begin(many) + kCollageMaxCells - 2);
		auto fitted = -1;
		const auto topped = AddCollagePhotos(
			AddCollagePhotos(data, nearly),
			photos,
			-1,
			&fitted);
		check(
			(fitted == 2) && (CollagePhotoCount(topped) == kCollageMaxCells),
			u"only the photos that fit are taken"_q);

		auto images = std::vector<ImportedImage>();
		images.push_back({ FxTestImage(5000, 100), u"wide"_q });
		images.push_back({ QImage(), u"null"_q });
		images.push_back({ FxTestImage(64, 48), u"little"_q });
		const auto prepared = PrepareCollagePhotos(std::move(images));
		check(
			(prepared.size() == 2)
				&& (prepared[0].content->size()
					== QSize(kCollagePhotoMaxSide, 82))
				&& (prepared[0].name == u"wide"_q)
				&& (prepared[1].content->size() == QSize(64, 48)),
			u"huge photos are scaled down for cells"_q);

		// How large a photo is kept: by the number of photos and by what
		// the canvas can show.
		const auto camera = QSize(4032, 3024);
		const auto canvas = QSize(2560, 2560);
		const auto pixels = [](QSize size) {
			return qint64(size.width()) * size.height() * 4;
		};
		check(
			(CollagePhotoSize(camera) == camera)
				&& (CollagePhotoSize(camera, 4, QSize(8000, 6000)) == camera)
				&& (CollagePhotoSize(camera, 9, canvas) == QSize(2560, 1920))
				&& (CollagePhotoSize(camera, 16, canvas) == QSize(2048, 1536))
				&& (CollagePhotoSize(camera, 17, canvas) == QSize(1600, 1200))
				&& (CollagePhotoSize(camera.transposed(), 16, canvas)
					== QSize(1536, 2048))
				&& (CollagePhotoSize(QSize(640, 480), 36, canvas)
					== QSize(640, 480))
				&& CollagePhotoSize(QSize(), 3, canvas).isEmpty(),
			u"the more photos a collage has, the smaller they are kept"_q);
		const auto covering = CollagePhotoSize(camera, 2, canvas);
		const auto little = CollagePhotoSize(camera, 2, QSize(400, 300));
		check(
			(covering.height() >= 2560)
				&& (covering.height() <= 2562)
				&& (covering.width() < camera.width())
				&& (little.height() >= 1280)
				&& (little.height() <= 1282)
				&& (CollagePhotoSize(QSize(5000, 100), 2, canvas)
					== QSize(kCollagePhotoMaxSide, 82)),
			u"a photo is not kept larger than covers the canvas"_q);
		check(
			(16 * pixels(CollagePhotoSize(camera, 16, canvas))
				< qint64(256) * 1024 * 1024)
				&& (kCollageMaxCells
					* pixels(CollagePhotoSize(camera, kCollageMaxCells, canvas))
					< qint64(384) * 1024 * 1024),
			u"16 camera photos fit in 256 MB, %1 in 384 MB"_q.arg(
				kCollageMaxCells));
		auto large = std::vector<ImportedImage>();
		large.push_back({ FxTestImage(2400, 1800), u"big"_q });
		large.push_back({ FxTestImage(300, 200), u"small"_q });
		const auto budgeted = PrepareCollagePhotos(
			std::move(large),
			20,
			QSize(1600, 1600));
		check(
			(budgeted.size() == 2)
				&& (budgeted[0].content->size() == QSize(1600, 1200))
				&& (budgeted[0].content->size()
					== CollagePhotoSize(
						QSize(2400, 1800),
						20,
						QSize(1600, 1600)))
				&& (budgeted[1].content->size() == QSize(300, 200)),
			u"photos are prepared at the size the collage needs"_q);

		// The template a new collage gets.
		const auto bestFor = [&](QSize first, QSize second, int turns) {
			auto fresh = CollageData();
			fresh.size = QSize(1000, 1000);
			fresh.spacing = 0.;
			fresh.margin = 0.;
			for (const auto size : { first, second }) {
				fresh.cells.push_back({
					.source = MakeCollageSource(
						photo(size.width(), size.height())),
					.turns = turns,
				});
			}
			return CollageCellRects(
				BestCollageGrid(fresh),
				QSizeF(fresh.size),
				0.,
				0.);
		};
		const auto landscape = bestFor(QSize(400, 300), QSize(400, 300), 0);
		const auto portrait = bestFor(QSize(300, 400), QSize(300, 400), 0);
		const auto turnedOver = bestFor(QSize(300, 400), QSize(300, 400), 1);
		check(
			(landscape.size() == 2)
				&& (landscape[0] == QRectF(0., 0., 1000., 500.))
				&& (portrait.size() == 2)
				&& (portrait[0] == QRectF(0., 0., 500., 1000.))
				&& (turnedOver == landscape)
				&& (BestCollageGrid(CollageData()) == DefaultCollageGrid(1)),
			u"a new collage gets the template that cuts the least"_q);

		// The memory: every content knows what it keeps, the undo history
		// counts a photo once however many steps and layers share it.
		auto own = CollageData();
		own.size = QSize(600, 300);
		own.grid = UniformCollageGrid(1, 2);
		own.cells.push_back({ .source = MakeCollageSource(photo(300, 200)) });
		own.cells.push_back({ .source = MakeCollageSource(photo(300, 200)) });
		const auto bytes = qint64(300) * 200 * 4;
		const auto first = MakeCollageContent(own);
		const auto second = MakeCollageContent(SwapCollageCells(own, 0, 1));
		check(
			(first->memoryUsage() >= bytes * 2)
				&& (first->memoryUsage() < bytes * 3)
				&& (second->memoryUsage() == first->memoryUsage()),
			u"a collage counts each of its photos once"_q);
		auto twice = own;
		twice.cells[1] = twice.cells[0];
		check(
			MakeCollageContent(twice)->memoryUsage() < bytes * 2,
			u"a photo shown in two cells is one photo"_q);
		auto stepOne = Document();
		stepOne.size = own.size;
		AddLayer(stepOne, MakeLayer(first, u"collage"_q));
		auto stepTwo = stepOne;
		stepTwo.layers[0].content = second;
		auto steps = History(stepOne);
		check(
			steps.push(stepTwo)
				&& (steps.memoryUsage() >= bytes * 2)
				&& (steps.memoryUsage() < bytes * 3),
			u"the undo steps of a collage share its photos (%1 bytes)"_q.arg(
				steps.memoryUsage()));
		const auto layered = photo(300, 200);
		auto third = CollageData();
		third.size = QSize(300, 300);
		third.cells.push_back({ .source = MakeCollageSource(layered) });
		auto before = Document();
		before.size = third.size;
		AddLayer(before, MakeLayer(layered, u"photo"_q));
		auto after = before;
		after.layers[0].content = MakeCollageContent(third);
		auto mixed = History(before);
		check(
			mixed.push(after)
				&& (mixed.memoryUsage() >= bytes)
				&& (mixed.memoryUsage() < bytes * 2),
			u"a photo that is a layer too is counted once (%1 bytes)"_q.arg(
				mixed.memoryUsage()));

		// A collage twice over the undo memory limit: its steps share the
		// photos, so they cost next to nothing and undo keeps working.
		auto heavy = History(stepOne);
		heavy.setLimits(100, bytes);
		auto pushed = true;
		for (auto i = 0; i != 5; ++i) {
			auto changed = own;
			changed.spacing = 0.03 + 0.01 * i;
			auto next = heavy.current();
			next.layers[0].content = MakeCollageContent(std::move(changed));
			pushed = heavy.push(std::move(next)) && pushed;
		}
		check(
			pushed
				&& (heavy.count() == 6)
				&& heavy.canUndo()
				&& (heavy.memoryUsage() > bytes * 2 - 1),
			u"a heavy collage keeps its undo steps (%1 of 6)"_q.arg(
				heavy.count()));
	}

	// From layers and back.
	{
		auto document = DocumentFromImage(
			smooth(900, 600, 20),
			EditState(),
			u"A"_q);
		auto second = MakeImageLayer(smooth(300, 400, 120), u"B"_q);
		second.transform = QTransform::fromTranslate(100., 50.);
		AddLayer(document, std::move(second));
		auto hidden = MakeImageLayer(smooth(200, 200, 200), u"C"_q);
		hidden.visible = false;
		const auto hiddenId = AddLayer(document, std::move(hidden));
		auto third = MakeImageLayer(smooth(500, 500, 270), u"D"_q);
		AddLayer(document, std::move(third));
		auto created = LayerId(0);
		const auto extra = std::vector<CollagePhoto>{
			{ MakeImageContent(smooth(640, 480, 330)), u"E"_q },
		};
		const auto made = CollageFromLayers(
			document,
			extra,
			u"Collage"_q,
			&created);
		const auto collageLayer = made.find(created);
		const auto collage = collageLayer
			? AsCollage(collageLayer->content)
			: nullptr;
		check(
			collage
				&& (made.layers.size() == 2)
				&& (made.indexOf(created) == 0)
				&& (made.layers[1].id == hiddenId)
				&& (made.size == document.size)
				&& (collage->size() == document.size)
				&& (collage->data().grid.count() == 4)
				&& (collage->data().cells[0].name == u"A"_q)
				&& (collage->data().cells[1].name == u"B"_q)
				&& (collage->data().cells[2].name == u"D"_q)
				&& (collage->data().cells[3].name == u"E"_q)
				&& (collage->data().cells[0].source->content().get()
					== document.layers[0].content.get())
				&& (collageLayer->name == u"Collage"_q),
			u"image layers become the cells of a collage"_q);
		auto nothing = Document();
		nothing.size = QSize(100, 80);
		const auto none = CollageFromLayers(nothing, {}, u"c"_q);
		const auto blank = none.layers.empty()
			? nullptr
			: AsCollage(none.layers[0].content);
		check(
			blank
				&& (blank->data().grid.count() == 4)
				&& (CollagePhotoCount(blank->data()) == 0),
			u"a collage of nothing is an empty 2 x 2 grid"_q);
		if (!collage || !blank) {
			return false;
		}

		// The collage and the layers made from it look the same.
		auto tuned = made;
		{
			auto data = collage->data();
			data.spacing = 0.03;
			data.margin = 0.04;
			data.radius = 0.08;
			data.background = CollageBackground::Gradient;
			data.color1 = QColor(20, 40, 90);
			data.color2 = QColor(240, 200, 120);
			data.gradientAngle = 30.;
			data.cells[0].zoom = 1.6;
			data.cells[0].offset = QPointF(0.2, 0.7);
			data.cells[1].turns = 1;
			data.cells[2].mirror = true;
			data.cells[3].zoom = 0.1; // Clamped: the whole photo.
			const auto layer = tuned.find(created);
			layer->content = MakeCollageContent(std::move(data));
			layer->transform = QTransform::fromScale(0.9, 0.9)
				* QTransform::fromTranslate(30., 20.);
			layer->opacity = 1.;
		}
		const auto layered = CollageToLayers(
			tuned,
			created,
			u"Background"_q,
			u"Photo %1"_q);
		const auto before = RenderDocument(tuned);
		const auto after = layered ? RenderDocument(*layered) : QImage();
		const auto difference = after.isNull()
			? 255.
			: FxImageDifference(after, before);
		log.push_back(u"   collage against its layers: %1"_q.arg(difference));
		check(
			layered
				&& (layered->layers.size() == 6)
				&& (layered->layers[0].name == u"Background"_q)
				&& (layered->layers[1].name == u"A"_q)
				&& (layered->layers[4].name == u"E"_q)
				&& (layered->layers[5].id == hiddenId)
				&& AsImage(layered->layers[1].content)
				&& (layered->layers[1].content.get()
					== document.layers[0].content.get())
				&& layered->layers[1].mask
				&& !layered->find(created),
			u"a collage becomes a background layer and photo layers"_q);
		check(
			!before.isNull() && (difference < 1.5),
			u"the layers look like the collage did"_q);
		auto bare = none;
		{
			auto data = blank->data();
			data.background = CollageBackground::None;
			bare.layers[0].content = MakeCollageContent(std::move(data));
		}
		const auto onlyBackground = CollageToLayers(
			none,
			none.layers[0].id,
			u"b"_q,
			QString());
		check(
			!CollageToLayers(tuned, hiddenId, QString(), QString())
				&& !CollageToLayers(
					bare,
					bare.layers[0].id,
					QString(),
					QString())
				&& onlyBackground
				&& (onlyBackground->layers.size() == 1)
				&& AsImage(onlyBackground->layers[0].content),
			u"only a collage with something in it is converted"_q);
		auto flag = std::atomic<bool>(true);
		check(
			!CollageToLayers(tuned, created, QString(), QString(), &flag),
			u"a cancelled conversion gives nothing"_q);

		// Another canvas shape.
		const auto square = CollageSizeForAspect(QSize(900, 600), 1, 1);
		const auto wide = CollageSizeForAspect(QSize(900, 600), 16, 9);
		check(
			(square == QSize(735, 735))
				&& about(wide.width() / double(wide.height()), 16. / 9., 0.01)
				&& about(
					double(wide.width()) * wide.height(),
					540000.,
					2000.)
				&& (CollageSizeForAspect(QSize(900, 600), 0, 1) == QSize(900, 600)),
			u"canvas sizes for an aspect ratio keep the area"_q);
		const auto resized = ResizedCollage(made, created, square);
		const auto moved = resized.find(hiddenId);
		check(
			(resized.size == square)
				&& (resized.find(created)->size() == square)
				&& resized.find(created)->transform.isIdentity()
				&& moved
				&& about(moved->transform.dx(), (735 - 900) / 2.)
				&& about(moved->transform.dy(), (735 - 600) / 2.),
			u"the canvas follows a collage that covers it"_q);
		const auto partial = ResizedCollage(tuned, created, square);
		const auto center = [](const Layer &layer) {
			return layer.transform.map(QPointF(
				layer.size().width() / 2.,
				layer.size().height() / 2.));
		};
		const auto was = center(*tuned.find(created));
		const auto now = center(*partial.find(created));
		check(
			(partial.size == tuned.size)
				&& about(was.x(), now.x())
				&& about(was.y(), now.y())
				&& (partial.find(hiddenId)->transform
					== tuned.find(hiddenId)->transform),
			u"a placed collage changes around its center"_q);

		const auto fresh = CollageDocument(extra, u"New"_q);
		const auto sized = CollageDocument(extra, u"New"_q, QSize(400, 300));
		check(
			(fresh.size == QSize(kCanvasLeastSide, kCanvasLeastSide))
				&& (fresh.layers.size() == 1)
				&& AsCollage(fresh.layers[0].content)
				&& (sized.size == QSize(400, 300))
				&& CollageDocument({}, u"New"_q).empty(),
			u"a new collage document"_q);
	}
	return ok;
}

namespace {

const auto SelfTest = SelfTestRegistrar(
	SelfTestSuite::Doc,
	"collage",
	&RunCollageSelfTest);

} // namespace

// UI.
//
// Everything below works on the main thread with a Controller: the canvas
// tool, the panel that is the options of the tool, the section of the
// Layer tab, the registration and the entry points. The harness that runs
// the self-test without the app cuts the file at the marker above.

namespace {

using namespace EditorUi;

constexpr auto kPadding = 16;
constexpr auto kRowPadding = 4;
constexpr auto kSkip = 8;
constexpr auto kLargeSkip = 14;
constexpr auto kTileSide = 52;
constexpr auto kTileSkip = 8;
constexpr auto kTileInset = 10;
constexpr auto kTileRadius = 10;
constexpr auto kSplitterHit = 7; // Screen pixels on each side.
constexpr auto kHandleLength = 28;
constexpr auto kHandleThickness = 5;
constexpr auto kGripRadius = 15;
constexpr auto kGripLeastCell = 76;
constexpr auto kPlusSize = 10;
constexpr auto kClickDistance = 4;
constexpr auto kZoomUnits = 100; // Slider steps per doubling.
constexpr auto kZoomSliderFrom = -200;
constexpr auto kZoomSliderTill = 300;
constexpr auto kWheelNotch = 0.12;
constexpr auto kWheelPixels = 300.;
constexpr auto kZoomEpsilon = 1e-3;
constexpr auto kAspectEpsilon = 0.006;
constexpr auto kToolOrder = 60;
constexpr auto kKindOrder = 40;
constexpr auto kEntryPhotosLimit = 16;
constexpr auto kEntryToastDuration = 60 * crl::time(1000);
constexpr auto kAddFilesLimit = 12; // As many as the editor imports at once.
constexpr auto kSceneCanvas = QSize(1600, 1200);

struct Aspect {
	int width = 1;
	int height = 1;
};

constexpr auto kAspects = std::array<Aspect, 9>{ {
	{ 1, 1 },
	{ 4, 5 },
	{ 3, 4 },
	{ 2, 3 },
	{ 9, 16 },
	{ 5, 4 },
	{ 4, 3 },
	{ 3, 2 },
	{ 16, 9 },
} };

[[nodiscard]] QColor SubTextColor() {
	return st::groupCallMemberNotJoinedStatus->c;
}

[[nodiscard]] QColor AccentColor() {
	return st::groupCallActiveFg->c;
}

[[nodiscard]] style::margins RowMargins(int top = 0) {
	return style::margins(Px(kPadding), top, Px(kPadding), 0);
}

[[nodiscard]] style::margins ButtonMargins() {
	return style::margins(Px(kRowPadding), 0, Px(kRowPadding), 0);
}

// What the tool and the panels of one editor share: which cell of the
// active collage is selected. It lives as long as the controller.
struct UiState {
	rpl::variable<int> selected = -1;
};

[[nodiscard]] not_null<UiState*> StateFor(not_null<Controller*> controller) {
	static auto Map = base::flat_map<
		not_null<Controller*>,
		std::unique_ptr<UiState>>();
	const auto i = Map.find(controller);
	if (i != end(Map)) {
		return i->second.get();
	}
	const auto result = Map.emplace(
		controller,
		std::make_unique<UiState>()
	).first->second.get();
	controller->lifetime().add([=] {
		Map.remove(controller);
	});
	return result;
}

[[nodiscard]] const Layer *CollageLayer(
		not_null<const Controller*> controller) {
	const auto layer = controller->hasDocument()
		? controller->activeLayer()
		: nullptr;
	return (layer && AsCollage(layer->content)) ? layer : nullptr;
}

[[nodiscard]] bool Editable(
		not_null<Controller*> controller,
		const Layer &layer,
		bool notify = true) {
	if (!layer.locked) {
		return true;
	} else if (notify) {
		controller->showToast(tr::lng_oblivion_photo_collage_locked(tr::now));
	}
	return false;
}

// The selected cell of the active collage, -1 if there is none.
[[nodiscard]] int SelectedCell(not_null<Controller*> controller) {
	const auto layer = CollageLayer(controller);
	if (!layer) {
		return -1;
	}
	const auto index = StateFor(controller)->selected.current();
	const auto shown = AsCollage(layer->content)->data().grid.count();
	return (index >= 0 && index < shown) ? index : -1;
}

// Gives the layer another collage. Data that changes nothing makes no
// undo step (every content is a new object, the document would differ).
void SetCollage(
		not_null<Controller*> controller,
		LayerId id,
		CollageData data,
		bool commit) {
	const auto layer = controller->document().find(id);
	const auto current = layer ? AsCollage(layer->content) : nullptr;
	if (!current) {
		return;
	}
	data = NormalizedCollage(std::move(data));
	if (data == current->data()) {
		if (commit) {
			controller->commit();
		}
		return;
	}
	const auto content = MakeCollageContent(std::move(data));
	controller->changeLayer(id, [&](Layer &layer) {
		layer.content = content;
	}, commit);
}

[[nodiscard]] QStringList ImagePaths(not_null<const QMimeData*> data) {
	auto result = QStringList();
	if (!data->hasUrls()) {
		return result;
	}
	static const auto kSuffixes = QStringList{
		u"png"_q,
		u"jpg"_q,
		u"jpeg"_q,
		u"webp"_q,
		u"bmp"_q,
		u"gif"_q,
		u"tif"_q,
		u"tiff"_q,
		u"heic"_q,
		u"heif"_q,
		u"avif"_q,
	};
	for (const auto &url : data->urls()) {
		if (!url.isLocalFile()) {
			continue;
		}
		const auto path = url.toLocalFile();
		if (kSuffixes.contains(QFileInfo(path).suffix().toLower())) {
			result.push_back(path);
		}
	}
	return result;
}

// True if the active collage is full and startCell is not one of its
// shown cells: a photo could only replace another one, there is none to
// replace, so nothing can be added. Says so.
[[nodiscard]] bool NoRoomFor(not_null<Controller*> controller, int startCell) {
	const auto layer = CollageLayer(controller);
	if (!layer) {
		return false;
	}
	const auto &data = AsCollage(layer->content)->data();
	if (CollagePhotoCount(data) < kCollageMaxCells
		|| (startCell >= 0 && startCell < data.grid.count())) {
		return false;
	}
	controller->showToast(tr::lng_oblivion_photo_collage_full(
		tr::now,
		lt_max,
		QString::number(kCollageMaxCells)));
	return true;
}

// The image layers CollageFromLayers() takes into a collage.
[[nodiscard]] int UsableLayers(const Document &document) {
	return int(ranges::count_if(document.layers, [](const Layer &layer) {
		return layer.visible
			&& !layer.locked
			&& AsImage(layer.content)
			&& !layer.size().isEmpty();
	}));
}

// Reads the files one at a time: a loaded photo is made ready (scaled
// down to what a collage of photos photos on this canvas shows) before
// the next one takes its memory, so twelve 48 MP files never sit decoded
// together. At most limit pictures. Worker thread.
[[nodiscard]] std::vector<CollagePhoto> LoadCollagePhotos(
		const QStringList &paths,
		int limit,
		int photos,
		QSize canvas,
		int *longest = nullptr) {
	auto result = std::vector<CollagePhoto>();
	for (const auto &path : paths) {
		if (int(result.size()) >= limit) {
			break;
		}
		auto image = LoadImage(path);
		if (image.isNull()) {
			continue;
		} else if (longest) {
			// The size the photo has in its file.
			*longest = std::max({ *longest, image.width(), image.height() });
		}
		auto list = std::vector<ImportedImage>();
		list.push_back({
			std::move(image),
			QFileInfo(path).completeBaseName(),
		});
		auto ready = PrepareCollagePhotos(std::move(list), photos, canvas);
		for (auto &photo : ready) {
			result.push_back(std::move(photo));
		}
	}
	return result;
}

// What the worker of AddToCollage() tells the main thread.
struct AddOutcome {
	bool ran = false;
	int ready = 0; // The pictures that could be opened.
	int skipped = 0; // Those of them the collage had no room for.
	LayerId created = 0;
};

// Puts pictures into the active collage (startCell: the cell the first
// one replaces, -1: the empty cells), or makes a collage of the image
// layers and these pictures if the active layer is not a collage.
// prepare gives the pictures on a worker, under the busy cover: it gets
// the number of photos the collage is going to have and its canvas, for
// PrepareCollagePhotos(). The result is one undo step. Pictures that were
// asked for (wanted of them, expected at most) and did not get in are
// told about: over the limit of a collage, of one import, not opened.
void AddToCollage(
		not_null<Controller*> controller,
		int startCell,
		int expected,
		int wanted,
		Fn<std::vector<CollagePhoto>(int photos, QSize canvas)> prepare) {
	if (expected <= 0 || !controller->hasDocument()) {
		return;
	}
	const auto layer = CollageLayer(controller);
	if (layer && !Editable(controller, *layer)) {
		return;
	} else if (NoRoomFor(controller, startCell)) {
		return;
	}
	const auto id = layer ? layer->id : LayerId(0);
	const auto name = tr::lng_oblivion_photo_collage_kind(tr::now);
	const auto outcome = std::make_shared<AddOutcome>();
	const auto weak = base::make_weak(controller.get());
	controller->runBusy(
		tr::lng_oblivion_photo_collage_building(tr::now),
		[=](const Document &document) -> std::optional<Document> {
			outcome->ran = true;
			auto result = document;
			const auto target = id ? result.find(id) : nullptr;
			const auto collage = target
				? AsCollage(target->content)
				: nullptr;
			if (id && !collage) {
				return std::nullopt;
			}
			const auto replaced = collage
				&& (startCell >= 0)
				&& (startCell < collage->data().grid.count())
				&& collage->data().cells[startCell].source;
			const auto had = collage
				? CollagePhotoCount(collage->data())
				: UsableLayers(document);
			const auto photos = prepare(
				std::min(
					had - (replaced ? 1 : 0) + expected,
					kCollageMaxCells),
				collage
					? collage->size().expandedTo(document.size)
					: document.size);
			const auto count = int(photos.size());
			outcome->ready = count;
			if (!count) {
				return std::nullopt;
			} else if (!collage) {
				auto made = CollageFromLayers(
					document,
					photos,
					name,
					&outcome->created);
				const auto fresh = made.find(outcome->created);
				const auto content = fresh
					? AsCollage(fresh->content)
					: nullptr;
				if (!content) {
					return std::nullopt;
				}
				const auto layers = int(document.layers.size())
					- (int(made.layers.size()) - 1);
				outcome->skipped = count
					- (CollagePhotoCount(content->data()) - layers);
				return made;
			}
			auto added = 0;
			auto updated = AddCollagePhotos(
				collage->data(),
				photos,
				startCell,
				&added);
			outcome->skipped = count - added;
			if (updated == collage->data()) {
				return std::nullopt;
			}
			target->content = MakeCollageContent(std::move(updated));
			return result;
		},
		[=](bool applied) {
			const auto strong = weak.get();
			if (!strong || !outcome->ran) {
				return;
			}
			// One toast, about what matters the most.
			if (outcome->skipped > 0) {
				strong->showToast(tr::lng_oblivion_photo_collage_limit(
					tr::now,
					lt_max,
					QString::number(kCollageMaxCells),
					lt_skipped,
					QString::number(outcome->skipped)));
			} else if (!outcome->ready) {
				strong->showToast(
					tr::lng_oblivion_photo_collage_open_failed(tr::now));
			} else if (outcome->ready < wanted) {
				strong->showToast(tr::lng_oblivion_photo_panel_import_partial(
					tr::now,
					lt_added,
					QString::number(outcome->ready),
					lt_total,
					QString::number(wanted),
					lt_limit,
					QString::number(kAddFilesLimit)));
			}
			if (!applied) {
				return;
			}
			if (outcome->created) {
				strong->setActiveLayer(outcome->created);
			}
			if (startCell >= 0) {
				StateFor(strong)->selected = startCell;
			}
		});
}

// Pictures that are in the memory already (the clipboard).
void AddPhotos(
		not_null<Controller*> controller,
		std::vector<ImportedImage> images,
		int startCell) {
	const auto count = int(ranges::count_if(
		images,
		[](const ImportedImage &entry) {
			return !entry.image.isNull();
		}));
	// Moved out by the worker: every original is released as soon as its
	// copy of the right size is made, not when all of them are ready.
	const auto loaded = std::make_shared<std::vector<ImportedImage>>(
		std::move(images));
	AddToCollage(controller, startCell, count, count, [=](
			int photos,
			QSize canvas) {
		return PrepareCollagePhotos(std::move(*loaded), photos, canvas);
	});
}

// Files: read on the worker one by one, so their memory is taken one at
// a time too. No more than kAddFilesLimit of them at once.
void AddPhotoFiles(
		not_null<Controller*> controller,
		const QStringList &paths,
		int startCell) {
	const auto list = paths.mid(0, kAddFilesLimit);
	AddToCollage(
		controller,
		startCell,
		int(list.size()),
		int(paths.size()),
		[=](int photos, QSize canvas) {
			return LoadCollagePhotos(list, kAddFilesLimit, photos, canvas);
		});
}

void ChoosePhotos(
		not_null<Controller*> controller,
		int startCell,
		bool multiple) {
	const auto show = controller->uiShow();
	if (!show || !show->valid() || controller->busy()) {
		return;
	} else if (NoRoomFor(controller, startCell)) {
		return;
	}
	// The dialog of the collage (its own title). The files are read off
	// the main thread, with the EXIF orientation, like the editor does.
	const auto weak = base::make_weak(controller.get());
	const auto chosen = [=](FileDialog::OpenResult &&result) {
		const auto strong = weak.get();
		if (!strong || result.paths.isEmpty()) {
			return;
		}
		AddPhotoFiles(strong, result.paths, startCell);
	};
	const auto title = tr::lng_oblivion_photo_collage_choose_title(tr::now);
	if (multiple) {
		FileDialog::GetOpenPaths(
			show->toastParent().get(),
			title,
			FileDialog::ImagesFilter(),
			chosen);
	} else {
		FileDialog::GetOpenPath(
			show->toastParent().get(),
			title,
			FileDialog::ImagesFilter(),
			chosen);
	}
}

// Files or a picture from a drop or from the clipboard. False if there
// is nothing to take.
bool ImportMime(
		not_null<Controller*> controller,
		not_null<const QMimeData*> data,
		int startCell) {
	const auto paths = ImagePaths(data);
	if (!paths.isEmpty()) {
		AddPhotoFiles(controller, paths, startCell);
		return true;
	} else if (data->hasImage()) {
		auto image = qvariant_cast<QImage>(data->imageData());
		if (!image.isNull()) {
			auto images = std::vector<ImportedImage>();
			images.push_back({ std::move(image), QString() });
			AddPhotos(controller, std::move(images), startCell);
			return true;
		}
	}
	return false;
}

bool PastePhotos(not_null<Controller*> controller, int startCell) {
	const auto data = QGuiApplication::clipboard()->mimeData();
	return data && ImportMime(controller, data, startCell);
}

[[nodiscard]] int UsableLayers(not_null<const Controller*> controller) {
	return controller->hasDocument()
		? UsableLayers(controller->document())
		: 0;
}

void MakeFromLayers(not_null<Controller*> controller) {
	if (!controller->hasDocument() || controller->busy()) {
		return;
	}
	auto created = LayerId(0);
	auto document = CollageFromLayers(
		controller->document(),
		{},
		tr::lng_oblivion_photo_collage_kind(tr::now),
		&created);
	controller->apply(std::move(document));
	controller->setActiveLayer(created);
}

void ConvertToLayers(not_null<Controller*> controller) {
	const auto layer = CollageLayer(controller);
	if (!layer || !Editable(controller, *layer)) {
		return;
	}
	const auto collage = AsCollage(layer->content);
	const auto &data = collage->data();
	auto photos = 0;
	const auto shown = std::min(data.grid.count(), int(data.cells.size()));
	for (auto i = 0; i != shown; ++i) {
		if (data.cells[i].source) {
			++photos;
		}
	}
	if (!photos && data.background == CollageBackground::None) {
		controller->showToast(
			tr::lng_oblivion_photo_collage_to_layers_empty(tr::now));
		return;
	}
	const auto id = layer->id;
	const auto background = tr::lng_oblivion_photo_collage_background_layer(
		tr::now);
	const auto cell = tr::lng_oblivion_photo_collage_cell_name(
		tr::now,
		lt_index,
		u"%1"_q);
	const auto weak = base::make_weak(controller.get());
	controller->runBusy(
		tr::lng_oblivion_photo_collage_converting(tr::now),
		[=](const Document &document) {
			return CollageToLayers(document, id, background, cell);
		},
		[=](bool applied) {
			// There is no collage any more: back to the layers.
			if (const auto strong = weak.get(); strong && applied) {
				if (strong->toolId() == kCollageTool) {
					strong->setTool(kViewTool);
				}
			}
		});
}

[[nodiscard]] bool IsPaste(not_null<QKeyEvent*> e) {
	if (e->matches(QKeySequence::Paste)) {
		return true;
	}
	const auto modifiers = e->modifiers()
		& ~(Qt::KeypadModifier | Qt::GroupSwitchModifier);
	if (modifiers != Qt::ControlModifier) {
		return false;
	} else if (e->key() == Qt::Key_V) {
		return true;
	}
	// The V key of a keyboard layout without Latin letters.
#ifdef Q_OS_MAC
	return (e->key() >= 0x80)
		&& (e->key() < Qt::Key_Escape)
		&& (e->nativeVirtualKey() == 0x09);
#elif defined Q_OS_WIN // Q_OS_MAC
	return (e->nativeVirtualKey() == 'V');
#else // Q_OS_MAC || Q_OS_WIN
	return false;
#endif // Q_OS_MAC || Q_OS_WIN
}

void PaintCollageIcon(QPainter &p, QRectF rect, QColor color) {
	p.translate(rect.topLeft());
	p.scale(rect.width() / 24., rect.height() / 24.);
	auto pen = QPen(color, 1.6);
	pen.setJoinStyle(Qt::RoundJoin);
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(QRectF(4., 4., 16., 16.), 3., 3.);
	p.drawLine(QPointF(11., 4.), QPointF(11., 20.));
	p.drawLine(QPointF(11., 12.5), QPointF(20., 12.5));
}

void PaintPlus(
		QPainter &p,
		QPointF center,
		double size,
		QColor color,
		double width) {
	auto pen = QPen(color, width);
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	p.drawLine(center - QPointF(size, 0.), center + QPointF(size, 0.));
	p.drawLine(center - QPointF(0., size), center + QPointF(0., size));
}

// The round handle that is dragged to swap two photos.
void PaintGrip(QPainter &p, QPointF center, bool active) {
	const auto radius = double(Px(kGripRadius));
	const auto unit = Px(24) / 24.;
	p.setPen(QPen(QColor(255, 255, 255, 200), std::max(Px(1), 1)));
	p.setBrush(active ? AccentColor() : QColor(0, 0, 0, 140));
	p.drawEllipse(center, radius, radius);
	auto pen = QPen(QColor(255, 255, 255), 1.6 * unit);
	pen.setCapStyle(Qt::RoundCap);
	pen.setJoinStyle(Qt::RoundJoin);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	for (const auto sign : { -1., 1. }) {
		const auto y = center.y() + sign * 3.5 * unit;
		const auto tip = center.x() - sign * 6.5 * unit;
		const auto tail = center.x() + sign * 6.5 * unit;
		p.drawLine(QPointF(tail, y), QPointF(tip, y));
		auto head = QPainterPath();
		head.moveTo(tip + sign * 3. * unit, y - 3. * unit);
		head.lineTo(tip, y);
		head.lineTo(tip + sign * 3. * unit, y + 3. * unit);
		p.drawPath(head);
	}
}

// The outline of a cell on the screen.
[[nodiscard]] QPainterPath MappedCell(
		const QTransform &toWidget,
		QRectF rect,
		double radius) {
	auto result = QPainterPath();
	if (toWidget.type() <= QTransform::TxScale) {
		const auto mapped = toWidget.mapRect(rect);
		const auto rx = radius * std::abs(toWidget.m11());
		const auto ry = radius * std::abs(toWidget.m22());
		if (rx > 0.5 && ry > 0.5) {
			result.addRoundedRect(mapped, rx, ry);
		} else {
			result.addRect(mapped);
		}
	} else {
		result.addPolygon(toWidget.map(QPolygonF(rect)));
		result.closeSubpath();
	}
	return result;
}

// The canvas tool of a collage: the splitters, the photos inside their
// cells, swapping, files dropped on cells.
class CollageTool final
	: public Tool
	, public base::has_weak_ptr {
public:
	explicit CollageTool(not_null<Controller*> controller);
	~CollageTool();

	void activated() override;
	void deactivated() override;
	bool mousePress(const ToolMouseEvent &e) override;
	void mouseMove(const ToolMouseEvent &e) override;
	void mouseRelease(const ToolMouseEvent &e) override;
	bool mouseDoubleClick(const ToolMouseEvent &e) override;
	void mouseLeave() override;
	bool wheel(const ToolWheelEvent &e) override;
	bool keyPress(not_null<QKeyEvent*> e) override;
	bool cancel() override;
	void paint(QPainter &p, const ToolPaintContext &context) override;
	QCursor cursor(const ToolMouseEvent &e) override;

private:
	enum class Drag : uchar {
		None,
		Splitter,
		Pan,
		Swap,
		Empty, // A press on an empty cell: a click chooses a photo.
	};
	enum class HitType : uchar {
		None,
		Splitter,
		Grip,
		Cell,
	};
	struct Hit {
		HitType type = HitType::None;
		int index = -1;

		friend bool operator==(const Hit &a, const Hit &b) = default;
	};
	struct Target {
		const Layer *layer = nullptr;
		const CollageContent *collage = nullptr;
		QTransform toWidget; // Collage pixels -> canvas widget.
		double pixel = 1.; // Collage pixels in one screen pixel.
	};

	[[nodiscard]] Target target(const QTransform &documentToWidget) const;
	[[nodiscard]] Target target() const;
	[[nodiscard]] Hit hitTest(const Target &target, QPointF local) const;
	[[nodiscard]] bool gripShown(const Target &target, int index) const;
	[[nodiscard]] int selected(const Target &target) const;
	[[nodiscard]] QCursor splitterCursor(
		const Target &target,
		const CollageSplitter &splitter) const;
	void setHover(Hit hit);
	void stopDrag(bool restore);
	void applyDrag(CollageData data);
	void restoreBase();
	void installDropFilter();
	[[nodiscard]] int dropCell(
		not_null<QWidget*> editor,
		QPointF position) const;
	[[nodiscard]] base::EventFilterResult handleDrop(
		not_null<QWidget*> editor,
		not_null<QEvent*> e);

	const not_null<Controller*> _controller;
	const not_null<UiState*> _state;
	Hit _hover;
	Drag _drag = Drag::None;
	LayerId _dragLayer = 0;
	CollageData _base;
	// The content the drag started from: put back as the same object when
	// the drag is cancelled or ends where it began, so the document is
	// equal to the undo step again and no step that changes nothing is
	// made.
	ContentPtr _baseContent;
	CollageSplitter _splitter;
	int _cell = -1;
	int _swapTarget = -1;
	QPointF _pressLocal;
	QPointF _pressWidget;
	QPointF _cursor;
	bool _moved = false;
	int _dropCell = -1;
	QPointer<QObject> _dropFilter;
	rpl::lifetime _lifetime;

};

CollageTool::CollageTool(not_null<Controller*> controller)
: _controller(controller)
, _state(StateFor(controller)) {
	_controller->documentChanges() | rpl::on_next([=] {
		_controller->updateCanvas();
	}, _lifetime);
	_state->selected.changes() | rpl::on_next([=] {
		_controller->updateCanvas();
	}, _lifetime);
	_controller->activeLayerValue() | rpl::skip(1) | rpl::on_next([=] {
		stopDrag(false);
		_hover = Hit();
		_state->selected = -1;
	}, _lifetime);
}

CollageTool::~CollageTool() {
	if (const auto filter = _dropFilter.data()) {
		filter->deleteLater();
	}
}

void CollageTool::activated() {
	_state->selected = -1;
	if (_controller->hasDocument() && !CollageLayer(_controller)) {
		// The tool was chosen to work with a collage: the topmost one.
		const auto &layers = _controller->document().layers;
		for (auto i = int(layers.size()); i != 0;) {
			if (AsCollage(layers[--i].content)) {
				_controller->setActiveLayer(layers[i].id);
				break;
			}
		}
	}
	installDropFilter();
}

void CollageTool::deactivated() {
	stopDrag(true);
	_dropCell = -1;
	_state->selected = -1;
}

CollageTool::Target CollageTool::target(
		const QTransform &documentToWidget) const {
	auto result = Target();
	result.layer = CollageLayer(_controller);
	result.collage = result.layer
		? AsCollage(result.layer->content)
		: nullptr;
	if (!result.collage) {
		return result;
	}
	result.toWidget = result.layer->transform * documentToWidget;
	const auto &matrix = result.toWidget;
	const auto determinant = std::abs(
		matrix.m11() * matrix.m22() - matrix.m12() * matrix.m21());
	result.pixel = (determinant > 1e-12)
		? (1. / std::sqrt(determinant))
		: 1.;
	return result;
}

CollageTool::Target CollageTool::target() const {
	return target(_controller->documentToWidget());
}

int CollageTool::selected(const Target &target) const {
	const auto index = _state->selected.current();
	return (target.collage
		&& index >= 0
		&& index < int(target.collage->cellRects().size()))
		? index
		: -1;
}

bool CollageTool::gripShown(const Target &target, int index) const {
	const auto &rects = target.collage->cellRects();
	const auto &cells = target.collage->data().cells;
	if (index < 0
		|| index >= int(rects.size())
		|| index >= int(cells.size())
		|| !cells[index].source
		|| rects.size() < 2) {
		return false;
	}
	const auto &rect = rects[index];
	return std::min(rect.width(), rect.height())
		>= Px(kGripLeastCell) * target.pixel;
}

CollageTool::Hit CollageTool::hitTest(
		const Target &target,
		QPointF local) const {
	const auto tolerance = Px(kSplitterHit) * target.pixel;
	const auto splitters = target.collage->splitters();
	auto best = -1;
	auto bestDistance = 0.;
	for (auto i = 0; i != int(splitters.size()); ++i) {
		const auto &rect = splitters[i].rect;
		const auto vertical = splitters[i].vertical;
		const auto along = vertical ? local.y() : local.x();
		const auto across = vertical ? local.x() : local.y();
		const auto from = vertical ? rect.top() : rect.left();
		const auto till = vertical ? rect.bottom() : rect.right();
		if (along < from || along > till) {
			continue;
		}
		const auto center = vertical ? rect.center().x() : rect.center().y();
		const auto half = (vertical ? rect.width() : rect.height()) / 2.;
		const auto distance = std::abs(across - center);
		if (distance > std::max(half, tolerance)) {
			continue;
		} else if (best < 0 || distance < bestDistance) {
			best = i;
			bestDistance = distance;
		}
	}
	if (best >= 0) {
		return { HitType::Splitter, best };
	}
	const auto index = target.collage->cellAt(local);
	if (index < 0) {
		return {};
	} else if (gripShown(target, index)) {
		const auto center = target.collage->cellRects()[index].center();
		if (QLineF(center, local).length() <= Px(kGripRadius) * target.pixel) {
			return { HitType::Grip, index };
		}
	}
	return { HitType::Cell, index };
}

QCursor CollageTool::splitterCursor(
		const Target &target,
		const CollageSplitter &splitter) const {
	// A vertical line is dragged sideways, unless the layer is turned.
	const auto direction = target.toWidget.map(
		QLineF(0., 0., splitter.vertical ? 0. : 1., splitter.vertical ? 1. : 0.));
	const auto upright = std::abs(direction.dy()) >= std::abs(direction.dx());
	return QCursor(upright ? Qt::SplitHCursor : Qt::SplitVCursor);
}

void CollageTool::setHover(Hit hit) {
	if (_hover != hit) {
		_hover = hit;
		_controller->updateCanvas();
	}
}

void CollageTool::stopDrag(bool restore) {
	const auto drag = std::exchange(_drag, Drag::None);
	_swapTarget = -1;
	if (drag == Drag::None) {
		_baseContent = nullptr;
		return;
	} else if (restore && (drag == Drag::Splitter || drag == Drag::Pan)) {
		restoreBase();
	}
	_baseContent = nullptr;
	_controller->updateCanvas();
}

void CollageTool::restoreBase() {
	const auto layer = _controller->document().find(_dragLayer);
	if (!layer || !AsCollage(layer->content)) {
		return;
	} else if (!_baseContent) {
		SetCollage(_controller, _dragLayer, _base, false);
		return;
	}
	const auto content = _baseContent;
	_controller->changeLayer(_dragLayer, [&](Layer &changed) {
		changed.content = content;
	}, false);
}

void CollageTool::applyDrag(CollageData data) {
	data = NormalizedCollage(std::move(data));
	if (_baseContent && data == _base) {
		restoreBase();
	} else {
		SetCollage(_controller, _dragLayer, std::move(data), false);
	}
}

bool CollageTool::mousePress(const ToolMouseEvent &e) {
	stopDrag(true);
	const auto target = this->target();
	if (!target.collage) {
		return false;
	}
	const auto local = LayerPoint(*target.layer, e.document);
	const auto hit = local ? hitTest(target, *local) : Hit();
	if (hit.type == HitType::None) {
		_state->selected = -1;
		return false;
	} else if (!Editable(_controller, *target.layer)) {
		return false;
	}
	const auto &data = target.collage->data();
	_dragLayer = target.layer->id;
	_base = data;
	_baseContent = target.layer->content;
	_pressLocal = *local;
	_pressWidget = e.widget;
	_cursor = e.widget;
	_moved = false;
	_swapTarget = -1;
	_hover = Hit();
	switch (hit.type) {
	case HitType::Splitter:
		_drag = Drag::Splitter;
		_splitter = target.collage->splitters()[hit.index];
		break;
	case HitType::Grip:
		_drag = Drag::Swap;
		_cell = hit.index;
		_state->selected = hit.index;
		break;
	case HitType::Cell: {
		_cell = hit.index;
		_state->selected = hit.index;
		const auto filled = (data.cells[hit.index].source != nullptr);
		const auto several = (target.collage->cellRects().size() > 1);
		_drag = !filled
			? Drag::Empty
			: ((e.modifiers & Qt::AltModifier) && several)
			? Drag::Swap
			: Drag::Pan;
	} break;
	case HitType::None: break;
	}
	_controller->updateCanvas();
	return true;
}

void CollageTool::mouseMove(const ToolMouseEvent &e) {
	_cursor = e.widget;
	const auto target = this->target();
	if (!target.collage) {
		stopDrag(false);
		setHover(Hit());
		return;
	}
	const auto local = LayerPoint(*target.layer, e.document);
	if (_drag == Drag::None) {
		setHover(local ? hitTest(target, *local) : Hit());
		return;
	} else if (target.layer->id != _dragLayer) {
		stopDrag(false);
		return;
	}
	if (QLineF(e.widget, _pressWidget).length() > Px(kClickDistance)) {
		_moved = true;
	}
	if (!local) {
		return;
	}
	const auto delta = *local - _pressLocal;
	switch (_drag) {
	case Drag::Splitter: {
		auto data = _base;
		data.grid = MoveCollageSplitter(
			_base.grid,
			_splitter,
			_splitter.vertical ? delta.x() : delta.y(),
			!(e.modifiers & Qt::AltModifier));
		applyDrag(std::move(data));
	} break;
	case Drag::Pan: {
		const auto &rects = target.collage->cellRects();
		if (_cell < 0
			|| _cell >= int(rects.size())
			|| _cell >= int(_base.cells.size())
			|| !_base.cells[_cell].source) {
			break;
		}
		auto data = _base;
		auto &cell = data.cells[_cell];
		cell.offset = PanCollagePhoto(
			CollageOrientedSize(cell),
			rects[_cell].size(),
			cell.zoom,
			cell.offset,
			delta);
		applyDrag(std::move(data));
	} break;
	case Drag::Swap: {
		const auto index = target.collage->cellAt(*local);
		_swapTarget = (index != _cell) ? index : -1;
		_controller->updateCanvas();
	} break;
	case Drag::Empty:
	case Drag::None: break;
	}
}

void CollageTool::mouseRelease(const ToolMouseEvent &e) {
	const auto drag = std::exchange(_drag, Drag::None);
	const auto swapTarget = std::exchange(_swapTarget, -1);
	_baseContent = nullptr;
	switch (drag) {
	case Drag::Splitter:
	case Drag::Pan:
		_controller->commit();
		break;
	case Drag::Swap: {
		const auto target = this->target();
		if (!target.collage
			|| target.layer->id != _dragLayer
			|| swapTarget < 0
			|| swapTarget >= int(target.collage->cellRects().size())) {
			break;
		}
		SetCollage(
			_controller,
			_dragLayer,
			SwapCollageCells(target.collage->data(), _cell, swapTarget),
			true);
		_state->selected = swapTarget;
	} break;
	case Drag::Empty:
		if (!_moved) {
			// Not from inside the mouse event: the dialog is modal.
			const auto cell = _cell;
			crl::on_main(base::make_weak(this), [=] {
				if (CollageLayer(_controller)) {
					ChoosePhotos(_controller, cell, true);
				}
			});
		}
		break;
	case Drag::None: break;
	}
	_controller->updateCanvas();
}

bool CollageTool::mouseDoubleClick(const ToolMouseEvent &e) {
	const auto target = this->target();
	if (!target.collage) {
		return false;
	}
	const auto local = LayerPoint(*target.layer, e.document);
	const auto hit = local ? hitTest(target, *local) : Hit();
	if (hit.type != HitType::Cell && hit.type != HitType::Grip) {
		return false;
	}
	const auto &data = target.collage->data();
	const auto &cell = data.cells[hit.index];
	if (!cell.source || !Editable(_controller, *target.layer)) {
		// The first click on an empty cell has opened the file dialog.
		return true;
	}
	// The whole photo, the next time the cell filled again.
	const auto oriented = CollageOrientedSize(cell);
	const auto size = target.collage->cellRects()[hit.index].size();
	const auto lowest = CollageMinZoom(oriented, size);
	const auto zoom = CollageClampZoom(oriented, size, cell.zoom);
	auto changed = data;
	auto &result = changed.cells[hit.index];
	result.offset = QPointF(0.5, 0.5);
	result.zoom = (zoom > lowest + kZoomEpsilon && lowest < 1. - kZoomEpsilon)
		? lowest
		: 1.;
	if (std::abs(zoom - 1.) > kZoomEpsilon
		&& std::abs(zoom - lowest) > kZoomEpsilon) {
		result.zoom = 1.;
	}
	_state->selected = hit.index;
	SetCollage(_controller, target.layer->id, std::move(changed), true);
	return true;
}

void CollageTool::mouseLeave() {
	if (_drag == Drag::None) {
		setHover(Hit());
	}
}

bool CollageTool::wheel(const ToolWheelEvent &e) {
	// Cmd / Ctrl + wheel zooms the canvas, a plain wheel zooms the photo
	// that was clicked before (not just any photo under the cursor: that
	// would make scrolling the canvas a lottery).
	if (e.modifiers & Qt::ControlModifier) {
		return false;
	}
	const auto target = this->target();
	if (!target.collage || _drag != Drag::None) {
		return false;
	}
	const auto local = LayerPoint(*target.layer, e.document);
	const auto index = local ? target.collage->cellAt(*local) : -1;
	if (index < 0 || index != selected(target) || target.layer->locked) {
		return false;
	}
	const auto &data = target.collage->data();
	const auto &cell = data.cells[index];
	if (!cell.source) {
		return false;
	}
	const auto steps = !e.pixelDelta.isNull()
		? (e.pixelDelta.y() / kWheelPixels)
		: (e.angleDelta.y() / 120. * kWheelNotch);
	if (!steps) {
		// A mouse wheel turned sideways (or with Shift) is not a zoom: it
		// moves the canvas as it does anywhere else. The sideways part of
		// a touchpad swipe is ignored, or zooming a photo would also
		// shake the canvas.
		return !e.pixelDelta.isNull() || (e.angleDelta.x() == 0.);
	}
	const auto &rect = target.collage->cellRects()[index];
	const auto oriented = CollageOrientedSize(cell);
	const auto zoomed = ZoomCollagePhoto(
		oriented,
		rect.size(),
		cell.zoom,
		cell.offset,
		*local - rect.topLeft(),
		CollageClampZoom(oriented, rect.size(), cell.zoom) * std::exp(steps));
	auto changed = data;
	changed.cells[index].zoom = zoomed.zoom;
	changed.cells[index].offset = zoomed.offset;
	// Becomes an undo step by itself when the wheel stops.
	SetCollage(_controller, target.layer->id, std::move(changed), false);
	return true;
}

bool CollageTool::keyPress(not_null<QKeyEvent*> e) {
	const auto target = this->target();
	if (!target.collage) {
		return false;
	}
	if (IsPaste(e)) {
		// A held key would paste once more with every repeat.
		if (e->isAutoRepeat() || !Editable(_controller, *target.layer)) {
			return true;
		}
		// Nothing to paste: the editor says so.
		return PastePhotos(_controller, selected(target));
	}
	const auto modifiers = e->modifiers()
		& ~(Qt::KeypadModifier | Qt::GroupSwitchModifier);
	const auto index = selected(target);
	if (!modifiers
		&& (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace)
		&& index >= 0
		&& target.collage->data().cells[index].source) {
		// One press removes one photo, and only a press made over the
		// canvas: not a held key and not a key that a field of the panel
		// had no use for (Backspace in a number field that is empty).
		const auto show = _controller->uiShow();
		const auto editor = (show && show->valid())
			? show->toastParent().get()
			: nullptr;
		if (e->isAutoRepeat()
			|| !editor
			|| (QApplication::focusWidget() != editor)) {
			return false;
		}
		if (Editable(_controller, *target.layer)) {
			SetCollage(
				_controller,
				target.layer->id,
				RemoveCollagePhoto(target.collage->data(), index),
				true);
		}
		return true;
	}
	return false;
}

bool CollageTool::cancel() {
	if (_drag != Drag::None) {
		stopDrag(true);
		return true;
	} else if (_state->selected.current() >= 0) {
		_state->selected = -1;
		return true;
	}
	return false;
}

QCursor CollageTool::cursor(const ToolMouseEvent &e) {
	const auto target = this->target();
	if (!target.collage) {
		return QCursor(Qt::ArrowCursor);
	}
	switch (_drag) {
	case Drag::Splitter: return splitterCursor(target, _splitter);
	case Drag::Pan: return QCursor(Qt::ClosedHandCursor);
	case Drag::Swap: return QCursor(Qt::DragMoveCursor);
	case Drag::Empty: return QCursor(Qt::PointingHandCursor);
	case Drag::None: break;
	}
	const auto local = LayerPoint(*target.layer, e.document);
	const auto hit = local ? hitTest(target, *local) : Hit();
	switch (hit.type) {
	case HitType::Splitter:
		return splitterCursor(
			target,
			target.collage->splitters()[hit.index]);
	case HitType::Grip: return QCursor(Qt::SizeAllCursor);
	case HitType::Cell:
		return QCursor(target.collage->data().cells[hit.index].source
			? Qt::OpenHandCursor
			: Qt::PointingHandCursor);
	case HitType::None: break;
	}
	return QCursor(Qt::ArrowCursor);
}

void CollageTool::paint(QPainter &p, const ToolPaintContext &context) {
	const auto target = this->target(context.documentToWidget);
	if (!target.collage) {
		return;
	}
	auto hq = PainterHighQualityEnabler(p);
	const auto &data = target.collage->data();
	const auto &rects = target.collage->cellRects();
	const auto count = std::min(int(rects.size()), int(data.cells.size()));
	const auto accent = AccentColor();
	const auto line = double(std::max(Px(1), 1));
	const auto chosen = selected(target);
	const auto idle = (_drag == Drag::None);
	const auto hovered = (idle
		&& (_hover.type == HitType::Cell || _hover.type == HitType::Grip)
		&& _hover.index < count)
		? _hover.index
		: -1;
	const auto shape = [&](int index) {
		return MappedCell(
			target.toWidget,
			rects[index],
			target.collage->cellRadius(index));
	};
	const auto center = [&](int index) {
		return target.toWidget.map(rects[index].center());
	};
	const auto side = [&](int index) {
		return std::min(rects[index].width(), rects[index].height())
			/ target.pixel;
	};

	for (auto i = 0; i != count; ++i) {
		if (!data.cells[i].source) {
			// An empty cell is seen only here: a dashed frame and a plus.
			p.setBrush(QColor(255, 255, 255, (i == hovered) ? 52 : 26));
			p.setPen(QPen(QColor(0, 0, 0, 70), line * 2.5));
			p.drawPath(shape(i));
			p.setBrush(Qt::NoBrush);
			p.setPen(QPen(QColor(255, 255, 255, 215), line * 1.5, Qt::DashLine));
			p.drawPath(shape(i));
			if (side(i) >= Px(kPlusSize) * 4) {
				PaintPlus(
					p,
					center(i),
					Px(kPlusSize),
					QColor(255, 255, 255, 230),
					line * 2.);
			}
		} else if (i == hovered && i != chosen) {
			p.setBrush(Qt::NoBrush);
			p.setPen(QPen(QColor(255, 255, 255, 120), line));
			p.drawPath(shape(i));
		}
	}
	if (chosen >= 0 && chosen < count) {
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(QColor(0, 0, 0, 90), line * 4.));
		p.drawPath(shape(chosen));
		p.setPen(QPen(accent, line * 2.));
		p.drawPath(shape(chosen));
	}

	const auto splitters = target.collage->splitters();
	for (auto i = 0; i != int(splitters.size()); ++i) {
		const auto &splitter = splitters[i];
		const auto active = (_drag == Drag::Splitter)
			? (splitter.track == _splitter.track
				&& splitter.index == _splitter.index)
			: (idle
				&& _hover.type == HitType::Splitter
				&& _hover.index == i);
		const auto &rect = splitter.rect;
		const auto middle = rect.center();
		const auto full = target.toWidget.map(splitter.vertical
			? QLineF(middle.x(), rect.top(), middle.x(), rect.bottom())
			: QLineF(rect.left(), middle.y(), rect.right(), middle.y()));
		const auto length = full.length();
		if (length < Px(kHandleLength) * 1.5) {
			continue;
		}
		if (active) {
			p.setPen(QPen(anim::with_alpha(accent, 0.75), line * 2.));
			p.drawLine(full);
		}
		const auto part = Px(kHandleLength) / (2. * length);
		const auto handle = QLineF(
			full.pointAt(0.5 - part),
			full.pointAt(0.5 + part));
		p.setPen(QPen(
			QColor(0, 0, 0, 110),
			Px(kHandleThickness) + line * 2.,
			Qt::SolidLine,
			Qt::RoundCap));
		p.drawLine(handle);
		p.setPen(QPen(
			active ? accent : QColor(255, 255, 255, 240),
			Px(kHandleThickness),
			Qt::SolidLine,
			Qt::RoundCap));
		p.drawLine(handle);
	}

	if (idle) {
		const auto grip = [&](int index) {
			if (index >= 0 && index < count && gripShown(target, index)) {
				PaintGrip(
					p,
					center(index),
					(_hover.type == HitType::Grip && _hover.index == index));
			}
		};
		grip(chosen);
		if (hovered != chosen) {
			grip(hovered);
		}
	} else if (_drag == Drag::Swap && _cell >= 0 && _cell < count) {
		p.setPen(Qt::NoPen);
		p.setBrush(anim::with_alpha(accent, 0.3));
		p.drawPath(shape(_cell));
		if (_swapTarget >= 0 && _swapTarget < count) {
			p.setBrush(anim::with_alpha(accent, 0.18));
			p.setPen(QPen(accent, line * 3.));
			p.drawPath(shape(_swapTarget));
		}
		PaintGrip(p, _cursor, true);
	}
	if (_dropCell >= 0 && _dropCell < count) {
		p.setBrush(anim::with_alpha(accent, 0.22));
		p.setPen(QPen(accent, line * 3.));
		p.drawPath(shape(_dropCell));
		PaintPlus(
			p,
			center(_dropCell),
			Px(kPlusSize),
			QColor(255, 255, 255),
			line * 2.5);
	}
}

void CollageTool::installDropFilter() {
	const auto show = _controller->uiShow();
	if (_dropFilter || !show || !show->valid()) {
		return;
	}
	// The editor itself takes the drops (and makes layers of them): while
	// this tool is current the files dropped on a cell go to the cell.
	const auto editor = show->toastParent();
	const auto weak = base::make_weak(this);
	_dropFilter = base::install_event_filter(editor, [=](
			not_null<QEvent*> e) {
		const auto strong = weak.get();
		return strong
			? strong->handleDrop(editor, e)
			: base::EventFilterResult::Continue;
	}).get();
}

int CollageTool::dropCell(
		not_null<QWidget*> editor,
		QPointF position) const {
	const auto target = this->target();
	if (!target.collage || target.layer->locked || _controller->busy()) {
		return -1;
	}
	auto canvas = (Canvas*)(nullptr);
	for (const auto child : editor->children()) {
		canvas = dynamic_cast<Canvas*>(child);
		if (canvas) {
			break;
		}
	}
	if (!canvas
		|| !canvas->isVisible()
		|| canvas->cropMode()
		|| canvas->comparing()) {
		return -1;
	}
	const auto point = canvas->mapFrom(editor, position.toPoint());
	if (!canvas->rect().contains(point)) {
		return -1;
	}
	const auto local = LayerPoint(
		*target.layer,
		_controller->widgetToDocument().map(QPointF(point)));
	return local ? target.collage->cellAt(*local) : -1;
}

base::EventFilterResult CollageTool::handleDrop(
		not_null<QWidget*> editor,
		not_null<QEvent*> e) {
	using Result = base::EventFilterResult;
	const auto type = e->type();
	const auto reset = [&](int cell) {
		if (_dropCell != cell) {
			_dropCell = cell;
			_controller->updateCanvas();
		}
	};
	if (type == QEvent::DragLeave) {
		reset(-1);
		return Result::Continue;
	} else if (type != QEvent::DragEnter
		&& type != QEvent::DragMove
		&& type != QEvent::Drop) {
		return Result::Continue;
	}
	const auto drop = static_cast<QDropEvent*>(e.get());
	const auto data = drop->mimeData();
	const auto cell = (data
		&& (!ImagePaths(data).isEmpty() || data->hasImage()))
		? dropCell(editor, drop->position())
		: -1;
	if (type != QEvent::Drop) {
		reset(cell);
		if (cell < 0) {
			return Result::Continue;
		}
		drop->setDropAction(Qt::CopyAction);
		drop->accept();
		return Result::Cancel;
	}
	reset(-1);
	if (cell < 0 || !ImportMime(_controller, data, cell)) {
		return Result::Continue;
	}
	drop->setDropAction(Qt::CopyAction);
	drop->accept();
	editor->setFocus();
	return Result::Cancel;
}

// The templates for the current number of photos, small pictures in rows.
class TemplateStrip final : public Ui::RpWidget {
public:
	explicit TemplateStrip(QWidget *parent);

	void setTemplates(std::vector<CollageTemplate> list, QSize aspect);
	void setCurrent(const CollageGrid &grid);
	[[nodiscard]] rpl::producer<CollageGrid> chosen() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	[[nodiscard]] int perRow(int width) const;
	[[nodiscard]] int tileWidth(int width) const;
	[[nodiscard]] QRect tileRect(int index) const;
	[[nodiscard]] int tileAt(QPoint point) const;
	void setOver(int index);

	std::vector<CollageTemplate> _list;
	QSize _aspect = QSize(1, 1);
	int _current = -1;
	int _over = -1;
	int _pressed = -1;
	rpl::event_stream<CollageGrid> _chosen;

};

TemplateStrip::TemplateStrip(QWidget *parent) : RpWidget(parent) {
	setMouseTracking(true);
}

void TemplateStrip::setTemplates(
		std::vector<CollageTemplate> list,
		QSize aspect) {
	_list = std::move(list);
	_aspect = aspect.isEmpty() ? QSize(1, 1) : aspect;
	_current = _over = _pressed = -1;
	if (width() > 0) {
		resizeToWidth(width());
	}
	update();
}

void TemplateStrip::setCurrent(const CollageGrid &grid) {
	auto current = -1;
	for (auto i = 0; i != int(_list.size()); ++i) {
		if (SameCollageLayout(_list[i].grid, grid)) {
			current = i;
			break;
		}
	}
	if (_current != current) {
		_current = current;
		update();
	}
}

rpl::producer<CollageGrid> TemplateStrip::chosen() const {
	return _chosen.events();
}

int TemplateStrip::perRow(int width) const {
	const auto side = Px(kTileSide);
	const auto skip = Px(kTileSkip);
	return std::max((width + skip) / (side + skip), 1);
}

// The tiles of a row take the whole width: they get a little wider
// instead of drifting apart, so the gaps between the columns are the same
// as between the rows, and the strip is not any taller for it.
int TemplateStrip::tileWidth(int width) const {
	const auto side = Px(kTileSide);
	const auto count = perRow(width);
	return std::clamp(
		(width - (count - 1) * Px(kTileSkip)) / count,
		side,
		2 * side);
}

QRect TemplateStrip::tileRect(int index) const {
	const auto side = Px(kTileSide);
	const auto wide = tileWidth(width());
	const auto count = perRow(width());
	// What is left by the rounding of the width is spread between them.
	const auto step = (count > 1)
		? ((width() - wide) / double(count - 1))
		: 0.;
	return QRect(
		int(std::lround((index % count) * step)),
		(index / count) * (side + Px(kTileSkip)),
		wide,
		side);
}

int TemplateStrip::tileAt(QPoint point) const {
	for (auto i = 0; i != int(_list.size()); ++i) {
		if (tileRect(i).contains(point)) {
			return i;
		}
	}
	return -1;
}

int TemplateStrip::resizeGetHeight(int newWidth) {
	const auto count = int(_list.size());
	if (!count) {
		return 0;
	}
	const auto rows = (count + perRow(newWidth) - 1) / perRow(newWidth);
	return rows * Px(kTileSide) + (rows - 1) * Px(kTileSkip);
}

void TemplateStrip::setOver(int index) {
	if (_over != index) {
		_over = index;
		setCursor((index >= 0) ? style::cur_pointer : style::cur_default);
		update();
	}
}

void TemplateStrip::mouseMoveEvent(QMouseEvent *e) {
	setOver(tileAt(e->pos()));
}

void TemplateStrip::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = tileAt(e->pos());
	}
}

void TemplateStrip::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, -1);
	if (e->button() == Qt::LeftButton
		&& pressed >= 0
		&& pressed == tileAt(e->pos())) {
		_chosen.fire_copy(_list[pressed].grid);
	}
}

void TemplateStrip::leaveEventHook(QEvent *e) {
	setOver(-1);
}

void TemplateStrip::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto accent = AccentColor();
	const auto radius = double(Px(kTileRadius));
	const auto line = double(std::max(Px(1), 1));
	const auto inset = Px(kTileInset);
	// The little layout has the proportions of the collage.
	const auto inner = QSizeF(_aspect).scaled(
		tileWidth(width()) - 2 * inset,
		Px(kTileSide) - 2 * inset,
		Qt::KeepAspectRatio);
	for (auto i = 0; i != int(_list.size()); ++i) {
		const auto tile = tileRect(i);
		if (!tile.intersects(e->rect())) {
			continue;
		}
		const auto current = (i == _current);
		if (current) {
			p.setPen(QPen(accent, line));
			p.setBrush(anim::with_alpha(accent, 0.16));
			p.drawRoundedRect(
				QRectF(tile).marginsRemoved(
					{ line / 2., line / 2., line / 2., line / 2. }),
				radius,
				radius);
		} else {
			p.setPen(Qt::NoPen);
			p.setBrush((i == _over)
				? st::groupCallMembersBgRipple
				: st::groupCallBg);
			p.drawRoundedRect(tile, radius, radius);
		}
		const auto origin = QPointF(
			tile.x() + (tile.width() - inner.width()) / 2.,
			tile.y() + (tile.height() - inner.height()) / 2.);
		p.setPen(Qt::NoPen);
		p.setBrush(current
			? accent
			: anim::with_alpha(SubTextColor(), (i == _over) ? 1. : 0.8));
		const auto cells = CollageCellRects(
			_list[i].grid,
			inner,
			Px(2),
			0.,
			false);
		for (const auto &cell : cells) {
			p.drawRoundedRect(cell.translated(origin), line, line);
		}
	}
}

[[nodiscard]] FxParams ParamValues(const CollageData &data) {
	auto result = FxParams();
	const auto grid = CollageGridSize(data.grid);
	const auto percent = [](double value, double most) {
		return FxValue::Number(std::round(value / most * 100.));
	};
	result.set("rows", FxValue::Integer(grid.height()));
	result.set("columns", FxValue::Integer(grid.width()));
	result.set("spacing", percent(data.spacing, kCollageMaxSpacing));
	result.set("margin", percent(data.margin, kCollageMaxMargin));
	result.set("radius", percent(data.radius, kCollageMaxRadius));
	result.set("background", FxValue::Integer(int(data.background)));
	result.set("color", FxValue::Color(data.color1));
	result.set("color2", FxValue::Color(data.color2));
	result.set("angle", FxValue::Number(data.gradientAngle));
	result.set("blur_amount", percent(data.blurAmount, 1.));
	result.set("blur_dim", percent(data.blurDim, 1.));
	return result;
}

// The step of the zoom slider of the selected photo for this zoom.
[[nodiscard]] int ZoomSliderValue(double zoom) {
	return std::clamp(
		int(std::lround(std::log2(std::max(zoom, 1e-6)) * kZoomUnits)),
		kZoomSliderFrom,
		kZoomSliderTill);
}

// The options of the collage tool: making a collage while there is none,
// then everything about the active one.
class CollagePanel final : public Ui::VerticalLayout {
public:
	CollagePanel(QWidget *parent, not_null<Controller*> controller);

private:
	void setupCreate(not_null<Ui::VerticalLayout*> page);
	void setupEdit(not_null<Ui::VerticalLayout*> page);
	void setupCell(not_null<Ui::VerticalLayout*> page);
	void addParams(
		not_null<Ui::VerticalLayout*> page,
		std::vector<FxParam> params);
	RowButton *addRow(
		not_null<Ui::VerticalLayout*> page,
		rpl::producer<QString> text,
		IconRef icon,
		Fn<void()> callback);
	void refresh();
	void refreshCell(not_null<const CollageContent*> collage);
	void refreshLater();
	void showCellSection();
	void paramChanged(const QByteArray &id, const FxValue &value, bool finished);
	void changeCell(
		Fn<void(CollageData &data, int index, QSizeF size)> modify,
		bool commit = true);
	void chooseAspect(int index);
	void nextBlurPhoto();
	void shuffle();

	const not_null<Controller*> _controller;
	const not_null<UiState*> _state;
	Ui::SlideWrap<Ui::VerticalLayout> *_create = nullptr;
	Ui::SlideWrap<Ui::VerticalLayout> *_edit = nullptr;
	PanelButton *_fromLayers = nullptr;

	rpl::variable<QString> _templatesTitle;
	TemplateStrip *_templates = nullptr;
	int _templatesCount = 0;
	QSize _templatesAspect;
	Ui::SlideWrap<Ui::FlatLabel> *_hidden = nullptr;
	rpl::variable<QString> _hiddenText;
	rpl::event_stream<FxParams> _values;
	Ui::SlideWrap<RowButton> *_blurPhoto = nullptr;
	rpl::variable<QString> _blurPhotoText;
	ChipsFlow *_aspects = nullptr;

	bool _refreshPosted = false;

	rpl::variable<QString> _cellTitle;
	Ui::RpWidget *_cellHeader = nullptr;
	Ui::SlideWrap<Ui::VerticalLayout> *_cell = nullptr;
	Ui::SlideWrap<Ui::FlatLabel> *_cellHint = nullptr;
	ValueSlider *_zoom = nullptr;
	rpl::variable<QString> _replaceText;
	rpl::variable<QString> _fitText;
	RowButton *_fitRow = nullptr;
	RowButton *_deleteRow = nullptr;
	std::vector<RowButton*> _photoRows;
	RowButton *_shuffleRow = nullptr;

};

CollagePanel::CollagePanel(
	QWidget *parent,
	not_null<Controller*> controller)
: VerticalLayout(parent)
, _controller(controller)
, _state(StateFor(controller)) {
	_create = add(object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
		this,
		object_ptr<Ui::VerticalLayout>(this)));
	_edit = add(object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
		this,
		object_ptr<Ui::VerticalLayout>(this)));
	setupCreate(_create->entity());
	setupEdit(_edit->entity());

	rpl::merge(
		_controller->documentChanges(),
		_controller->activeLayerValue() | rpl::to_empty,
		_state->selected.changes() | rpl::to_empty
	) | rpl::on_next([=] {
		refresh();
	}, lifetime());
	refresh();

	// A photo was clicked on the canvas: its section may be below the
	// templates, out of sight.
	_state->selected.changes(
	) | rpl::filter([](int index) {
		return (index >= 0);
	}) | rpl::on_next([=] {
		crl::on_main(this, [=] {
			showCellSection();
		});
	}, lifetime());
}

RowButton *CollagePanel::addRow(
		not_null<Ui::VerticalLayout*> page,
		rpl::producer<QString> text,
		IconRef icon,
		Fn<void()> callback) {
	const auto result = page->add(
		object_ptr<RowButton>(page, std::move(text), icon),
		ButtonMargins());
	result->setClickedCallback(std::move(callback));
	return result;
}

void CollagePanel::addParams(
		not_null<Ui::VerticalLayout*> page,
		std::vector<FxParam> params) {
	const auto layer = CollageLayer(_controller);
	page->add(
		CreateParamsPanel(page, ParamsPanelArgs{
			.params = std::move(params),
			.values = layer
				? ParamValues(AsCollage(layer->content)->data())
				: FxParams(),
			.updates = _values.events(),
			.changed = [=](
					const QByteArray &id,
					FxValue value,
					bool finished) {
				paramChanged(id, value, finished);
			},
			.controller = _controller.get(),
		}),
		RowMargins());
}

void CollagePanel::setupCreate(not_null<Ui::VerticalLayout*> page) {
	page->add(
		object_ptr<Ui::FlatLabel>(
			page,
			tr::lng_oblivion_photo_collage_create_about(),
			HintLabelStyle()),
		RowMargins());
	const auto choose = page->add(
		object_ptr<PanelButton>(
			page,
			tr::lng_oblivion_photo_collage_add(),
			true),
		RowMargins(Px(kLargeSkip)));
	choose->setClickedCallback([=] {
		ChoosePhotos(_controller, -1, true);
	});
	_fromLayers = page->add(
		object_ptr<PanelButton>(
			page,
			tr::lng_oblivion_photo_collage_from_layers(),
			false),
		RowMargins(Px(kSkip)));
	_fromLayers->setClickedCallback([=] {
		MakeFromLayers(_controller);
	});
	page->add(object_ptr<Ui::FixedHeightWidget>(page, Px(kSkip)));
	addRow(
		page,
		tr::lng_oblivion_photo_collage_paste(),
		{ .icon = &st::menuIconCopy },
		[=] {
			if (!PastePhotos(_controller, -1)) {
				_controller->showToast(
					tr::lng_oblivion_photo_collage_paste_empty(tr::now));
			}
		});
}

void CollagePanel::setupEdit(not_null<Ui::VerticalLayout*> page) {
	page->add(
		object_ptr<SectionTitle>(page, _templatesTitle.value()),
		RowMargins());
	_templates = page->add(object_ptr<TemplateStrip>(page), RowMargins());
	_templates->chosen() | rpl::on_next([=](const CollageGrid &grid) {
		const auto layer = CollageLayer(_controller);
		if (layer && Editable(_controller, *layer)) {
			SetCollage(
				_controller,
				layer->id,
				WithCollageGrid(AsCollage(layer->content)->data(), grid),
				true);
		}
	}, _templates->lifetime());
	_hidden = page->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			page,
			object_ptr<Ui::FlatLabel>(
				page,
				_hiddenText.value(),
				HintLabelStyle()),
			RowMargins(Px(kSkip))));
	_hidden->toggle(false, anim::type::instant);
	const auto more = page->add(
		object_ptr<PanelButton>(
			page,
			tr::lng_oblivion_photo_collage_add(),
			true),
		RowMargins(Px(kLargeSkip)));
	more->setClickedCallback([=] {
		ChoosePhotos(_controller, -1, true);
	});

	// The selected photo is right under the templates: it is what was
	// just clicked on the canvas.
	setupCell(page);

	const auto background = [](int value) {
		return [=](const FxParams &params) {
			return params.integer("background") == value;
		};
	};
	page->add(
		object_ptr<SectionTitle>(page, tr::lng_oblivion_photo_collage_look()),
		RowMargins(Px(kSkip)));
	addParams(page, {
		FxFloat(
			"spacing",
			tr::lng_oblivion_photo_collage_spacing,
			0.,
			100.,
			20.),
		FxFloat(
			"margin",
			tr::lng_oblivion_photo_collage_margin,
			0.,
			100.,
			13.),
		FxFloat(
			"radius",
			tr::lng_oblivion_photo_collage_radius,
			0.,
			100.,
			0.),
		FxChoice(
			"background",
			tr::lng_oblivion_photo_collage_background,
			{
				tr::lng_oblivion_photo_collage_bg_none,
				tr::lng_oblivion_photo_collage_bg_color,
				tr::lng_oblivion_photo_collage_bg_gradient,
				tr::lng_oblivion_photo_collage_bg_blur,
			},
			int(CollageBackground::Color)),
		FxColor(
			"color",
			tr::lng_oblivion_photo_collage_color,
			QColor(255, 255, 255)
		).when([](const FxParams &params) {
			const auto value = params.integer("background");
			return (value == int(CollageBackground::Color))
				|| (value == int(CollageBackground::Gradient));
		}),
		FxColor(
			"color2",
			tr::lng_oblivion_photo_collage_color2,
			QColor(30, 136, 229)
		).when(background(int(CollageBackground::Gradient))),
		FxAngle(
			"angle",
			tr::lng_oblivion_photo_collage_angle,
			90.
		).when(background(int(CollageBackground::Gradient))),
		FxFloat(
			"blur_amount",
			tr::lng_oblivion_photo_collage_blur_amount,
			0.,
			100.,
			50.
		).when(background(int(CollageBackground::Blur))),
		FxFloat(
			"blur_dim",
			tr::lng_oblivion_photo_collage_blur_dim,
			-100.,
			100.,
			0.
		).when(background(int(CollageBackground::Blur))),
	});
	_blurPhoto = page->add(
		object_ptr<Ui::SlideWrap<RowButton>>(
			page,
			object_ptr<RowButton>(
				page,
				_blurPhotoText.value(),
				IconRef{ .icon = &st::menuIconPhoto }),
			ButtonMargins()));
	_blurPhoto->entity()->setClickedCallback([=] {
		nextBlurPhoto();
	});
	_blurPhoto->toggle(false, anim::type::instant);

	page->add(
		object_ptr<SectionTitle>(page, tr::lng_oblivion_photo_collage_grid()),
		RowMargins(Px(kSkip)));
	addParams(page, {
		FxInt(
			"rows",
			tr::lng_oblivion_photo_collage_rows,
			1,
			kCollageMaxTracks,
			2),
		FxInt(
			"columns",
			tr::lng_oblivion_photo_collage_columns,
			1,
			kCollageMaxTracks,
			2),
	});

	page->add(
		object_ptr<SectionTitle>(
			page,
			tr::lng_oblivion_photo_collage_aspect()),
		RowMargins(Px(kSkip)));
	_aspects = page->add(object_ptr<ChipsFlow>(page), RowMargins());
	for (auto i = 0; i != int(kAspects.size()); ++i) {
		_aspects->addChip(
			rpl::single(u"%1:%2"_q.arg(kAspects[i].width).arg(
				kAspects[i].height)),
			[=] { chooseAspect(i); });
	}

	page->add(object_ptr<Ui::FixedHeightWidget>(page, Px(kLargeSkip)));
	addRow(
		page,
		tr::lng_oblivion_photo_collage_paste(),
		{ .icon = &st::menuIconCopy },
		[=] {
			if (!PastePhotos(_controller, SelectedCell(_controller))) {
				_controller->showToast(
					tr::lng_oblivion_photo_collage_paste_empty(tr::now));
			}
		});
	_shuffleRow = addRow(
		page,
		tr::lng_oblivion_photo_collage_shuffle(),
		{ .icon = &st::menuIconReorder },
		[=] { shuffle(); });
	addRow(
		page,
		tr::lng_oblivion_photo_collage_to_layers(),
		{ .icon = &st::menuIconShowAll },
		[=] { ConvertToLayers(_controller); });
	page->add(
		object_ptr<Ui::FlatLabel>(
			page,
			tr::lng_oblivion_photo_collage_hint(),
			HintLabelStyle()),
		RowMargins(Px(kSkip)));
}

void CollagePanel::setupCell(not_null<Ui::VerticalLayout*> page) {
	_cellHeader = page->add(
		object_ptr<SectionTitle>(page, _cellTitle.value()),
		RowMargins(Px(kSkip)));
	_cellHint = page->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			page,
			object_ptr<Ui::FlatLabel>(
				page,
				tr::lng_oblivion_photo_collage_cell_hint(),
				HintLabelStyle()),
			RowMargins()));
	_cell = page->add(object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
		page,
		object_ptr<Ui::VerticalLayout>(page)));
	const auto inner = _cell->entity();

	_zoom = inner->add(
		object_ptr<ValueSlider>(inner, SliderArgs{
			.label = tr::lng_oblivion_photo_collage_cell_zoom(),
			.min = kZoomSliderFrom,
			.max = kZoomSliderTill,
			.defaultValue = 0,
			.value = 0,
			.format = [](int value) {
				const auto zoom = std::pow(2., value / double(kZoomUnits));
				return QString::number(int(std::lround(zoom * 100.))) + '%';
			},
		}),
		RowMargins());
	_zoom->changes() | rpl::on_next([=](SliderChange change) {
		const auto wanted = std::pow(2., change.value / double(kZoomUnits));
		changeCell([=](CollageData &data, int index, QSizeF size) {
			auto &cell = data.cells[index];
			if (!cell.source) {
				return;
			}
			const auto oriented = CollageOrientedSize(cell);
			const auto current = CollageClampZoom(oriented, size, cell.zoom);
			if (ZoomSliderValue(current) == change.value) {
				// The step the photo is at already (a click on the knob,
				// a drag that came back): its exact zoom is not rounded
				// to the step.
				return;
			}
			const auto zoomed = ZoomCollagePhoto(
				oriented,
				size,
				cell.zoom,
				cell.offset,
				QPointF(size.width() / 2., size.height() / 2.),
				wanted);
			cell.zoom = zoomed.zoom;
			cell.offset = zoomed.offset;
		}, change.finished);
		if (change.finished) {
			// The knob was left where the photo could not follow (smaller
			// than the whole photo): it comes back to the zoom there is.
			refreshLater();
		}
	}, _zoom->lifetime());

	addRow(
		inner,
		_replaceText.value(),
		{ .icon = &st::menuIconReplace },
		[=] {
			const auto index = SelectedCell(_controller);
			const auto layer = CollageLayer(_controller);
			if (index >= 0 && layer && Editable(_controller, *layer)) {
				ChoosePhotos(_controller, index, false);
			}
		});
	_fitRow = addRow(
		inner,
		_fitText.value(),
		{ .icon = &st::menuIconExpand },
		[=] {
			changeCell([](CollageData &data, int index, QSizeF size) {
				auto &cell = data.cells[index];
				if (!cell.source) {
					return;
				}
				const auto oriented = CollageOrientedSize(cell);
				const auto lowest = CollageMinZoom(oriented, size);
				const auto zoom = CollageClampZoom(oriented, size, cell.zoom);
				cell.zoom = (zoom > lowest + kZoomEpsilon) ? lowest : 1.;
				cell.offset = QPointF(0.5, 0.5);
			});
		});
	const auto rotate = addRow(
		inner,
		tr::lng_oblivion_photo_collage_cell_rotate(),
		{ .icon = &st::photoEditorRotateButton.icon },
		[=] {
			changeCell([](CollageData &data, int index, QSizeF size) {
				auto &cell = data.cells[index];
				cell.turns = (cell.turns + 1) & 3;
			});
		});
	const auto mirror = addRow(
		inner,
		tr::lng_oblivion_photo_collage_cell_mirror(),
		{ .icon = &st::photoEditorFlipButton.icon },
		[=] {
			changeCell([](CollageData &data, int index, QSizeF size) {
				auto &cell = data.cells[index];
				cell.mirror = !cell.mirror;
			});
		});
	const auto remove = addRow(
		inner,
		tr::lng_oblivion_photo_collage_cell_remove(),
		{ .icon = &st::menuIconCancel },
		[=] {
			changeCell([](CollageData &data, int index, QSizeF size) {
				data = RemoveCollagePhoto(std::move(data), index);
			});
		});
	_deleteRow = addRow(
		inner,
		tr::lng_oblivion_photo_collage_cell_delete(),
		{ .icon = &st::menuIconDelete },
		[=] {
			changeCell([](CollageData &data, int index, QSizeF size) {
				data = DeleteCollageCell(std::move(data), index);
			});
			_state->selected = -1;
		});
	_photoRows = { rotate, mirror, remove };
}

void CollagePanel::refresh() {
	const auto layer = CollageLayer(_controller);
	const auto collage = layer ? AsCollage(layer->content) : nullptr;
	_create->toggle(!collage, anim::type::instant);
	_edit->toggle(collage != nullptr, anim::type::instant);
	if (!collage) {
		_fromLayers->setAvailable(UsableLayers(_controller) > 0);
		return;
	}
	const auto &data = collage->data();
	_values.fire(ParamValues(data));

	const auto shown = std::min(data.grid.count(), int(data.cells.size()));
	const auto photos = CollagePhotoCount(data);
	auto visible = 0;
	for (auto i = 0; i != shown; ++i) {
		if (data.cells[i].source) {
			++visible;
		}
	}
	const auto count = photos
		? std::min(photos, kCollageMaxCells)
		: data.grid.count();
	if (_templatesCount != count || _templatesAspect != data.size) {
		_templatesCount = count;
		_templatesAspect = data.size;
		_templates->setTemplates(CollageTemplates(count), data.size);
		_templatesTitle = tr::lng_oblivion_photo_collage_templates(
			tr::now,
			lt_count,
			count);
	}
	_templates->setCurrent(data.grid);
	const auto waiting = photos - visible;
	if (waiting > 0) {
		_hiddenText = tr::lng_oblivion_photo_collage_hidden(
			tr::now,
			lt_count,
			waiting);
	}
	_hidden->toggle(waiting > 0, anim::type::instant);

	const auto blur = (data.background == CollageBackground::Blur)
		&& (visible > 1);
	if (blur) {
		const auto current = collage->blurCell();
		auto number = 0;
		for (auto i = 0; i <= current && i < shown; ++i) {
			if (data.cells[i].source) {
				++number;
			}
		}
		_blurPhotoText = tr::lng_oblivion_photo_collage_blur_photo(
			tr::now,
			lt_index,
			QString::number(number),
			lt_total,
			QString::number(visible));
	}
	_blurPhoto->toggle(blur, anim::type::instant);

	// The chip of the proportions the result has now.
	const auto &document = _controller->document();
	const auto covers = layer->transform.isIdentity()
		&& (data.size == document.size);
	const auto turned = covers && (document.global.quarterTurns & 1);
	const auto ratio = turned
		? (data.size.height() / double(data.size.width()))
		: (data.size.width() / double(data.size.height()));
	auto aspect = -1;
	for (auto i = 0; i != int(kAspects.size()); ++i) {
		const auto preset = kAspects[i].width / double(kAspects[i].height);
		if (std::abs(ratio - preset) <= kAspectEpsilon * preset) {
			aspect = i;
		}
	}
	_aspects->setSelected(aspect);
	_shuffleRow->setAvailable(visible > 1);

	refreshCell(collage);
}

void CollagePanel::refreshCell(not_null<const CollageContent*> collage) {
	const auto &data = collage->data();
	const auto index = SelectedCell(_controller);
	_cell->toggle(index >= 0, anim::type::instant);
	_cellHint->toggle(index < 0, anim::type::instant);
	const auto filled = (index >= 0) && (data.cells[index].source != nullptr);
	_cellTitle = (filled || index < 0)
		? tr::lng_oblivion_photo_collage_cell(tr::now)
		: tr::lng_oblivion_photo_collage_cell_empty(tr::now);
	if (index < 0) {
		return;
	}
	_replaceText = filled
		? tr::lng_oblivion_photo_collage_cell_replace(tr::now)
		: tr::lng_oblivion_photo_collage_cell_choose(tr::now);
	_zoom->setDimmed(!filled);
	for (const auto row : _photoRows) {
		row->setAvailable(filled);
	}
	_deleteRow->setAvailable(data.grid.count() > 1);
	auto whole = false;
	auto fits = false;
	if (filled) {
		const auto &cell = data.cells[index];
		const auto oriented = CollageOrientedSize(cell);
		const auto size = collage->cellRects()[index].size();
		const auto lowest = CollageMinZoom(oriented, size);
		const auto zoom = CollageClampZoom(oriented, size, cell.zoom);
		whole = (zoom <= lowest + kZoomEpsilon);
		fits = (lowest < 1. - kZoomEpsilon)
			|| (std::abs(zoom - 1.) > kZoomEpsilon);
		if (!_zoom->dragging()) {
			_zoom->setValue(ZoomSliderValue(zoom));
		}
	}
	_fitText = whole
		? tr::lng_oblivion_photo_collage_cell_fill(tr::now)
		: tr::lng_oblivion_photo_collage_cell_fit(tr::now);
	_fitRow->setAvailable(filled && fits);
}

// The controls go back to what the collage has after a change that was
// refused (the layer is locked) or limited (a zoom below the whole
// photo). Not right away: a slider takes no value from outside until it
// has let the mouse go, and it tells about its last change before that.
void CollagePanel::refreshLater() {
	if (_refreshPosted) {
		return;
	}
	_refreshPosted = true;
	crl::on_main(this, [=] {
		_refreshPosted = false;
		refresh();
	});
}

void CollagePanel::showCellSection() {
	if (!_cellHeader || !_cell || SelectedCell(_controller) < 0) {
		return;
	}
	// The panel is somewhere inside the scroll area of the Tool tab,
	// unless it is shown alone.
	auto scroll = (Ui::ScrollArea*)(nullptr);
	auto parent = parentWidget();
	while (parent && !scroll) {
		scroll = dynamic_cast<Ui::ScrollArea*>(parent);
		parent = parent->parentWidget();
	}
	const auto inner = scroll ? scroll->widget() : nullptr;
	if (!inner || !inner->isAncestorOf(this)) {
		return;
	}
	const auto top = _cellHeader->mapTo(inner, QPoint()).y();
	const auto bottom = _cell->mapTo(inner, QPoint(0, _cell->height())).y();
	// Only down to a section that is below the fold: someone who works
	// further down the panel is left where they are.
	const auto shownFrom = scroll->scrollTop();
	if (top >= shownFrom && bottom > shownFrom + scroll->height()) {
		scroll->scrollToY(top, bottom);
	}
}

void CollagePanel::paramChanged(
		const QByteArray &id,
		const FxValue &value,
		bool finished) {
	const auto layer = CollageLayer(_controller);
	if (!layer) {
		return;
	}
	auto data = AsCollage(layer->content)->data();
	if (!Editable(_controller, *layer, finished)) {
		refreshLater();
		return;
	}
	const auto part = [&](double most) {
		return std::clamp(value.number(), -100., 100.) / 100. * most;
	};
	if (id == "rows" || id == "columns") {
		auto size = CollageGridSize(data.grid);
		const auto number = std::clamp(value.integer(), 1, kCollageMaxTracks);
		if (id == "rows") {
			size.setHeight(number);
		} else {
			size.setWidth(number);
		}
		data = WithCollageGrid(
			std::move(data),
			UniformCollageGrid(size.height(), size.width()));
	} else if (id == "spacing") {
		data.spacing = part(kCollageMaxSpacing);
	} else if (id == "margin") {
		data.margin = part(kCollageMaxMargin);
	} else if (id == "radius") {
		data.radius = part(kCollageMaxRadius);
	} else if (id == "background") {
		data.background = CollageBackground(std::clamp(
			value.integer(),
			int(CollageBackground::None),
			int(CollageBackground::Blur)));
	} else if (id == "color") {
		auto color = value.color();
		color.setAlpha(255);
		data.color1 = color;
	} else if (id == "color2") {
		auto color = value.color();
		color.setAlpha(255);
		data.color2 = color;
	} else if (id == "angle") {
		data.gradientAngle = value.number();
	} else if (id == "blur_amount") {
		data.blurAmount = part(1.);
	} else if (id == "blur_dim") {
		data.blurDim = part(1.);
	} else {
		return;
	}
	SetCollage(_controller, layer->id, std::move(data), finished);
}

void CollagePanel::changeCell(
		Fn<void(CollageData &data, int index, QSizeF size)> modify,
		bool commit) {
	const auto layer = CollageLayer(_controller);
	const auto index = SelectedCell(_controller);
	if (!layer || index < 0) {
		return;
	} else if (!Editable(_controller, *layer, commit)) {
		refreshLater();
		return;
	}
	const auto collage = AsCollage(layer->content);
	auto data = collage->data();
	modify(data, index, collage->cellRects()[index].size());
	SetCollage(_controller, layer->id, std::move(data), commit);
}

void CollagePanel::chooseAspect(int index) {
	const auto layer = CollageLayer(_controller);
	if (!layer
		|| index < 0
		|| index >= int(kAspects.size())
		|| !Editable(_controller, *layer)) {
		return;
	}
	const auto &document = _controller->document();
	const auto size = layer->size();
	auto aspect = kAspects[index];
	// The proportions are those of the result: a turned canvas is made
	// the other way round.
	if (layer->transform.isIdentity()
		&& (size == document.size)
		&& (document.global.quarterTurns & 1)) {
		std::swap(aspect.width, aspect.height);
	}
	_controller->apply(ResizedCollage(
		document,
		layer->id,
		CollageSizeForAspect(size, aspect.width, aspect.height)));
}

void CollagePanel::nextBlurPhoto() {
	const auto layer = CollageLayer(_controller);
	if (!layer || !Editable(_controller, *layer)) {
		return;
	}
	const auto collage = AsCollage(layer->content);
	auto data = collage->data();
	const auto shown = std::min(data.grid.count(), int(data.cells.size()));
	const auto current = collage->blurCell();
	for (auto step = 1; step <= shown; ++step) {
		const auto index = (std::max(current, 0) + step) % shown;
		if (data.cells[index].source) {
			data.blurCell = index;
			break;
		}
	}
	SetCollage(_controller, layer->id, std::move(data), true);
}

void CollagePanel::shuffle() {
	const auto layer = CollageLayer(_controller);
	if (!layer || !Editable(_controller, *layer)) {
		return;
	}
	const auto &original = AsCollage(layer->content)->data();
	auto data = original;
	const auto shown = std::min(data.grid.count(), int(data.cells.size()));
	auto filled = std::vector<int>();
	for (auto i = 0; i != shown; ++i) {
		if (data.cells[i].source) {
			filled.push_back(i);
		}
	}
	const auto count = int(filled.size());
	if (count < 2) {
		return;
	}
	for (auto i = count - 1; i > 0; --i) {
		const auto j = int(QRandomGenerator::global()->bounded(i + 1));
		data = SwapCollageCells(std::move(data), filled[i], filled[j]);
	}
	if (data == original) {
		// The dice gave the same order: something must change.
		data = SwapCollageCells(std::move(data), filled[0], filled[1]);
	}
	_state->selected = -1;
	SetCollage(_controller, layer->id, std::move(data), true);
}

// The section of the Layer tab shown for a collage layer: the settings
// themselves are the options of the tool.
[[nodiscard]] object_ptr<Ui::RpWidget> CreateLayerSection(
		not_null<QWidget*> parent,
		not_null<Controller*> controller) {
	auto result = object_ptr<Ui::VerticalLayout>(parent);
	const auto raw = result.data();
	raw->add(
		object_ptr<Ui::FlatLabel>(
			raw,
			tr::lng_oblivion_photo_collage_layer_about(),
			HintLabelStyle()),
		RowMargins());
	const auto open = raw->add(
		object_ptr<PanelButton>(
			raw,
			tr::lng_oblivion_photo_collage_layer_open(),
			false),
		RowMargins(Px(kLargeSkip)));
	open->setClickedCallback([=] {
		// Choosing the current tool again would not show its tab.
		controller->setTool(kCollageTool);
		controller->showToolOptions();
	});
	return result;
}

// What the "Collage" item of the "+" menu of the layers adds: an empty
// grid over the canvas, to be filled with the tool.
void AddEmptyCollage(not_null<Controller*> controller) {
	if (!controller->hasDocument()) {
		return;
	}
	auto data = CollageData();
	data.size = controller->document().size;
	data.grid = UniformCollageGrid(2, 2);
	data.background = CollageBackground::None;
	controller->addLayer(MakeLayer(
		MakeCollageContent(std::move(data)),
		tr::lng_oblivion_photo_collage_kind(tr::now)));
	controller->setTool(kCollageTool);
}

const auto Registered = EditorRegistrar([] {
	RegisterTool({
		.id = kCollageTool,
		.name = tr::lng_oblivion_photo_collage_kind,
		.key = Qt::Key_G,
		.order = kToolOrder,
		.paintIcon = PaintCollageIcon,
		.create = [](not_null<Controller*> controller)
		-> std::unique_ptr<Tool> {
			return std::make_unique<CollageTool>(controller);
		},
		.options = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller)
		-> object_ptr<Ui::RpWidget> {
			return object_ptr<CollagePanel>(parent, controller);
		},
	});
	RegisterLayerKind({
		.type = kCollageType,
		.name = tr::lng_oblivion_photo_collage_kind,
		.order = kKindOrder,
		.paintIcon = PaintCollageIcon,
		.create = [](not_null<Controller*> controller) {
			AddEmptyCollage(controller);
		},
	});
	RegisterPanel({
		.id = "collage.layer",
		.slot = PanelSlot::LayerProperties,
		.order = kKindOrder,
		.title = tr::lng_oblivion_photo_collage_kind,
		.visible = [](not_null<const Controller*> controller) {
			return CollageLayer(controller) != nullptr;
		},
		.create = CreateLayerSection,
	});
});

// The files of a collage are chosen and read for a while, the app may be
// locked meanwhile: an editor shown then would be above the passcode
// screen, with the photos in it.
[[nodiscard]] bool AppLocked() {
	return Core::IsAppLaunched() && Core::App().passcodeLocked();
}

// False if the editor was not shown.
bool ShowCollageDocument(
		std::shared_ptr<Ui::Show> show,
		Document document,
		PhotoEditorOptions options) {
	if (!show || !show->valid() || AppLocked()) {
		return false;
	} else if (document.empty()) {
		show->showToast(tr::lng_oblivion_photo_collage_open_failed(tr::now));
		return false;
	}
	options.document = std::make_shared<const Document>(std::move(document));
	options.tool = kCollageTool;
	options.tab = PhotoEditorTab::Tool;
	ShowPhotoEditor(std::move(show), QImage(), std::move(options));
	return true;
}

// A collage that is being opened from outside of the editor: its files
// are chosen, then loaded on a worker, which takes seconds. One at a time,
// or a second click on the same row would open a second editor with its
// own copy of every photo. Main thread only.
enum class EntryStage : uchar {
	None,
	Choosing,
	Loading,
};
auto CurrentEntryStage = EntryStage::None;
auto EntryToast = base::weak_ptr<Ui::Toast::Instance>();

void HideEntryToast() {
	if (const auto toast = base::take(EntryToast).get()) {
		toast->hideAnimated();
	}
}

// Stays until the editor opens, the time is only a limit. One toast, in
// the window that asked last.
void ShowEntryToast(const std::shared_ptr<Ui::Show> &show) {
	HideEntryToast();
	EntryToast = show->showToast(
		tr::lng_oblivion_photo_collage_building(tr::now),
		kEntryToastDuration);
}

// True (and says that the photos are loading) if a collage is being
// opened already.
[[nodiscard]] bool EntryBusy(const std::shared_ptr<Ui::Show> &show) {
	if (CurrentEntryStage == EntryStage::None) {
		return false;
	} else if (CurrentEntryStage == EntryStage::Loading) {
		ShowEntryToast(show);
	}
	return true;
}

void StartEntryLoading(const std::shared_ptr<Ui::Show> &show) {
	CurrentEntryStage = EntryStage::Loading;
	ShowEntryToast(show);
}

// The photos are loaded (or could not be): the editor, or a toast. Files
// that were left out are told about, not dropped silently.
void FinishEntryLoading(
		std::shared_ptr<Ui::Show> show,
		Document document,
		PhotoEditorOptions options,
		int added,
		int requested) {
	CurrentEntryStage = EntryStage::None;
	HideEntryToast();
	const auto partial = (added > 0) && (added < requested);
	const auto keep = partial ? show : nullptr;
	const auto shown = ShowCollageDocument(
		std::move(show),
		std::move(document),
		std::move(options));
	if (shown && keep && keep->valid()) {
		keep->showToast(tr::lng_oblivion_photo_panel_import_partial(
			tr::now,
			lt_added,
			QString::number(added),
			lt_total,
			QString::number(requested),
			lt_limit,
			QString::number(kEntryPhotosLimit)));
	}
}

// Pictures made in code for the snapshot scenes.
[[nodiscard]] QImage ScenePhoto(int index) {
	struct Look {
		QColor top;
		QColor bottom;
		QColor sun;
		QColor hills;
	};
	static const auto kLooks = std::array<Look, 5>{ {
		{ { 255, 153, 102 }, { 255, 94, 98 }, { 255, 236, 170 }, { 90, 40, 80 } },
		{ { 72, 149, 239 }, { 144, 224, 239 }, { 255, 255, 255 }, { 20, 80, 130 } },
		{ { 20, 30, 70 }, { 120, 60, 140 }, { 250, 220, 240 }, { 15, 15, 40 } },
		{ { 190, 230, 170 }, { 250, 240, 190 }, { 255, 200, 90 }, { 40, 110, 70 } },
		{ { 240, 240, 245 }, { 200, 205, 220 }, { 250, 120, 100 }, { 90, 95, 120 } },
	} };
	const auto &look = kLooks[std::abs(index) % kLooks.size()];
	const auto size = (index % 2) ? QSize(720, 960) : QSize(1200, 800);
	auto result = QImage(size, QImage::Format_ARGB32_Premultiplied);
	auto p = QPainter(&result);
	p.setRenderHint(QPainter::Antialiasing);
	auto sky = QLinearGradient(0., 0., 0., size.height());
	sky.setColorAt(0., look.top);
	sky.setColorAt(1., look.bottom);
	p.fillRect(result.rect(), sky);
	p.setPen(Qt::NoPen);
	p.setBrush(look.sun);
	const auto radius = size.height() * 0.11;
	p.drawEllipse(
		QPointF(size.width() * (0.3 + 0.1 * (index % 4)), size.height() * 0.34),
		radius,
		radius);
	for (auto layer = 0; layer != 2; ++layer) {
		auto hills = QPainterPath();
		const auto base = size.height() * (0.62 + 0.14 * layer);
		hills.moveTo(0., base);
		hills.cubicTo(
			size.width() * 0.25,
			base - size.height() * (0.2 - 0.05 * layer),
			size.width() * 0.45,
			base + size.height() * 0.08,
			size.width() * 0.62,
			base - size.height() * 0.06);
		hills.cubicTo(
			size.width() * 0.78,
			base - size.height() * 0.2,
			size.width() * 0.9,
			base - size.height() * 0.02,
			size.width(),
			base - size.height() * 0.1);
		hills.lineTo(size.width(), size.height());
		hills.lineTo(0., size.height());
		hills.closeSubpath();
		p.setBrush(anim::with_alpha(look.hills, layer ? 1. : 0.55));
		p.drawPath(hills);
	}
	p.end();
	return result;
}

[[nodiscard]] Document SceneCollage(
		int count,
		Fn<void(CollageData &data)> tune = nullptr) {
	auto images = std::vector<ImportedImage>();
	images.push_back({ SampleSceneImage(), u"photo"_q });
	for (auto i = 1; i < count; ++i) {
		images.push_back({ ScenePhoto(i - 1), u"photo %1"_q.arg(i + 1) });
	}
	auto result = CollageDocument(
		PrepareCollagePhotos(std::move(images)),
		tr::lng_oblivion_photo_collage_kind(tr::now),
		kSceneCanvas);
	if (tune && !result.layers.empty()) {
		auto &layer = result.layers.front();
		auto data = AsCollage(layer.content)->data();
		tune(data);
		layer.content = MakeCollageContent(std::move(data));
	}
	return result;
}

void TuneWarm(CollageData &data) {
	const auto templates = CollageTemplates(CollagePhotoCount(data));
	data.grid = templates[std::min(1, int(templates.size()) - 1)].grid;
	data.spacing = 0.025;
	data.margin = 0.035;
	data.radius = 0.04;
	data.background = CollageBackground::Gradient;
	data.color1 = QColor(255, 214, 165);
	data.color2 = QColor(142, 36, 170);
	data.gradientAngle = 60.;
	if (data.cells.size() > 2) {
		data.cells[2].zoom = 1.6;
		data.cells[2].offset = QPointF(0.3, 0.6);
	}
}

void TuneBlur(CollageData &data) {
	data.spacing = 0.03;
	data.margin = 0.06;
	data.radius = 0.07;
	data.background = CollageBackground::Blur;
	data.blurCell = 1;
	data.blurAmount = 0.6;
	data.blurDim = 0.25;
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	const auto width = Px(340);
	const auto column = [](
			not_null<QWidget*> parent,
			object_ptr<Ui::RpWidget> panel) {
		auto result = object_ptr<Ui::VerticalLayout>(parent);
		result->add(std::move(panel));
		result->add(object_ptr<Ui::FixedHeightWidget>(
			result.data(),
			Px(kPadding)));
		return object_ptr<Ui::RpWidget>(std::move(result));
	};

	// The editor with a collage: the tool overlay (a selected photo, the
	// splitter handles) and the collage panel in the Tool tab.
	RegisterEditorScene({
		.name = u"photo_collage_editor"_q,
		.document = [] { return SceneCollage(4, TuneWarm); },
		.tab = PhotoEditorTab::Tool,
		.tool = kCollageTool,
		.prepare = [](not_null<Controller*> controller) {
			StateFor(controller)->selected = 1;
		},
	});
	// A blurred photo behind three photos, one cell left empty.
	RegisterEditorScene({
		.name = u"photo_collage_blur"_q,
		.document = [] {
			return SceneCollage(3, [](CollageData &data) {
				TuneBlur(data);
				data.grid = UniformCollageGrid(2, 2);
			});
		},
		.tab = PhotoEditorTab::Tool,
		.tool = kCollageTool,
	});
	// A photo without a collage: the tool offers to make one.
	RegisterEditorScene({
		.name = u"photo_collage_create"_q,
		.tab = PhotoEditorTab::Tool,
		.tool = kCollageTool,
	});
	// A new collage layer over a photo, exactly as the "+" menu of the
	// layers adds it: nothing but empty cells, one of them clicked.
	RegisterEditorScene({
		.name = u"photo_collage_empty"_q,
		.tab = PhotoEditorTab::Tool,
		.tool = kCollageTool,
		.prepare = [](not_null<Controller*> controller) {
			AddEmptyCollage(controller);
			StateFor(controller)->selected = 0;
		},
	});
	RegisterEditorScene({
		.name = u"photo_collage_narrow"_q,
		.size = QSize(520, 820),
		.document = [] { return SceneCollage(5, TuneWarm); },
		.tab = PhotoEditorTab::Tool,
		.tool = kCollageTool,
	});
	// The Layer tab of a collage layer with the collage section.
	RegisterEditorScene({
		.name = u"photo_collage_layer"_q,
		.document = [] { return SceneCollage(6, TuneBlur); },
		.tab = PhotoEditorTab::Layer,
	});

	// The whole panel: a photo selected, a blurred background.
	RegisterPanelScene({
		.name = u"photo_collage_panel"_q,
		.size = QSize(width, 0),
		.document = [] { return SceneCollage(5, TuneBlur); },
		.prepare = [](not_null<Controller*> controller) {
			StateFor(controller)->selected = 0;
		},
		.create = [=](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			return column(
				parent,
				object_ptr<CollagePanel>(parent, controller));
		},
	});
	// Nothing selected, more photos than the grid shows.
	RegisterPanelScene({
		.name = u"photo_collage_panel_hidden"_q,
		.size = QSize(width, 0),
		.document = [] {
			return SceneCollage(7, [](CollageData &data) {
				data.grid = UniformCollageGrid(2, 2);
			});
		},
		.create = [=](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			return column(
				parent,
				object_ptr<CollagePanel>(parent, controller));
		},
	});
	// Two photos on a gradient, a third column added by hand and its
	// empty cell selected: the short list of templates, what an empty
	// cell offers, the controls of the gradient.
	RegisterPanelScene({
		.name = u"photo_collage_panel_gradient"_q,
		.size = QSize(width, 0),
		.document = [] {
			return SceneCollage(2, [](CollageData &data) {
				TuneWarm(data);
				data.grid = UniformCollageGrid(1, 3);
			});
		},
		.prepare = [](not_null<Controller*> controller) {
			StateFor(controller)->selected = 2;
		},
		.create = [=](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			return column(
				parent,
				object_ptr<CollagePanel>(parent, controller));
		},
	});
	RegisterPanelScene({
		.name = u"photo_collage_panel_create"_q,
		.size = QSize(width, 0),
		.create = [=](
				not_null<QWidget*> parent,
				not_null<Controller*> controller) {
			return column(
				parent,
				object_ptr<CollagePanel>(parent, controller));
		},
	});
});

} // namespace

void ShowCollageEditor(
		std::shared_ptr<Ui::Show> show,
		QStringList paths,
		PhotoEditorOptions options) {
	if (!show || !show->valid() || AppLocked() || EntryBusy(show)) {
		return;
	}
	const auto name = tr::lng_oblivion_photo_collage_kind(tr::now);
	if (options.fileName.isEmpty()) {
		options.fileName = tr::lng_oblivion_photo_collage_file_name(tr::now);
	}
	StartEntryLoading(show);
	crl::async([=, options = std::move(options)]() mutable {
		const auto requested = int(paths.size());
		auto longest = 0;
		auto photos = LoadCollagePhotos(
			paths,
			kEntryPhotosLimit,
			std::min(requested, kEntryPhotosLimit),
			DefaultCanvas(kCanvasDefaultSide),
			&longest);
		const auto added = int(photos.size());
		// The canvas is chosen by the photos as they were in the files.
		auto document = CollageDocument(photos, name, DefaultCanvas(longest));
		photos.clear();
		// Everything holding the show goes back to the main thread.
		crl::on_main([
				show = std::move(show),
				document = std::move(document),
				options = std::move(options),
				added,
				requested]() mutable {
			FinishEntryLoading(
				std::move(show),
				std::move(document),
				std::move(options),
				added,
				requested);
		});
	});
}

void ShowCollageEditor(
		std::shared_ptr<Ui::Show> show,
		std::vector<ImportedImage> images,
		PhotoEditorOptions options) {
	if (!show || !show->valid() || AppLocked() || EntryBusy(show)) {
		return;
	}
	const auto name = tr::lng_oblivion_photo_collage_kind(tr::now);
	if (options.fileName.isEmpty()) {
		options.fileName = tr::lng_oblivion_photo_collage_file_name(tr::now);
	}
	const auto requested = int(images.size());
	if (requested > kEntryPhotosLimit) {
		images.resize(kEntryPhotosLimit);
	}
	auto longest = 0;
	for (const auto &entry : images) {
		longest = std::max({
			longest,
			entry.image.width(),
			entry.image.height(),
		});
	}
	StartEntryLoading(show);
	crl::async([
			show = std::move(show),
			images = std::move(images),
			options = std::move(options),
			name,
			requested,
			longest]() mutable {
		const auto expected = int(images.size());
		auto photos = PrepareCollagePhotos(
			std::move(images),
			expected,
			DefaultCanvas(kCanvasDefaultSide));
		const auto added = int(photos.size());
		auto document = CollageDocument(photos, name, DefaultCanvas(longest));
		photos.clear();
		crl::on_main([
				show = std::move(show),
				document = std::move(document),
				options = std::move(options),
				added,
				requested]() mutable {
			FinishEntryLoading(
				std::move(show),
				std::move(document),
				std::move(options),
				added,
				requested);
		});
	});
}

void ChoosePhotosForCollage(
		std::shared_ptr<Ui::Show> show,
		PhotoEditorOptions options) {
	if (!show || !show->valid() || AppLocked() || EntryBusy(show)) {
		return;
	}
	// Taken before the dialog: a platform whose file dialog does not block
	// the window would open a second one over it.
	CurrentEntryStage = EntryStage::Choosing;
	const auto shared = std::make_shared<PhotoEditorOptions>(
		std::move(options));
	FileDialog::GetOpenPaths(
		show->toastParent().get(),
		tr::lng_oblivion_photo_collage_choose_title(tr::now),
		FileDialog::ImagesFilter(),
		[=](FileDialog::OpenResult &&result) {
			CurrentEntryStage = EntryStage::None;
			if (result.paths.isEmpty() || !show->valid()) {
				return;
			}
			ShowCollageEditor(show, result.paths, *shared);
		},
		[] {
			CurrentEntryStage = EntryStage::None;
		});
}

} // namespace Oblivion::Photo
