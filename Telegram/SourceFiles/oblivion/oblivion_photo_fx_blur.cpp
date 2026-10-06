/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_photo_fx_blur.h"

#include "oblivion/oblivion_photo_editor_controls.h"
#include "oblivion/oblivion_photo_panels.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/rp_widget.h"

#include <QtCore/QElapsedTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace Oblivion::Photo {
namespace {

using Pixel = uint32;

constexpr auto kPi = 3.14159265358979323846;
constexpr auto kMinSigma = 0.3;
constexpr auto kRegionLevels = 3;
constexpr auto kTiltLevels = 4;
constexpr auto kMaxBoxRadius = 4096.;
constexpr auto kAverageStep = 0.75;
constexpr auto kMaxAveragePasses = 16;
constexpr auto kMaxSpinAngle = 360.;
constexpr auto kMaxZoomAmount = 4.;
constexpr auto kLensMinRadius = 0.25;
constexpr auto kLensWorkRadius = 16.;
constexpr auto kLensSubSamples = 8;
constexpr auto kLensGain = 12.;
constexpr auto kLensGamma = 2.2;
constexpr auto kLensFull = uint32(0x00FFFFFFU);
constexpr auto kLensAlphaUnit = uint32(0x00010101U);
constexpr auto kSurfaceSigma = 0.6;
constexpr auto kSurfaceWorkSigma = 2.;
constexpr auto kSurfaceMinWorkSigma = 0.5;
constexpr auto kSurfaceMaxPixels = 2'500'000.;
constexpr auto kSurfaceMaxFactor = 64.;
constexpr auto kSurfaceRange = 0.25;

struct Source {
	const Pixel *data = nullptr;
	int width = 0;
	int height = 0;
	ptrdiff_t stride = 0;

	[[nodiscard]] const Pixel *row(int y) const {
		return data + y * stride;
	}
};

struct Target {
	Pixel *data = nullptr;
	ptrdiff_t stride = 0;

	[[nodiscard]] Pixel *row(int y) const {
		return data + y * stride;
	}
};

[[nodiscard]] Source SourceOf(const QImage &image) {
	return {
		reinterpret_cast<const Pixel*>(image.constBits()),
		image.width(),
		image.height(),
		ptrdiff_t(image.bytesPerLine() / 4),
	};
}

[[nodiscard]] Target TargetOf(QImage &image) {
	return {
		reinterpret_cast<Pixel*>(image.bits()),
		ptrdiff_t(image.bytesPerLine() / 4),
	};
}

[[nodiscard]] bool Cancelled(const std::atomic<bool> *cancel) {
	return cancel && cancel->load(std::memory_order_relaxed);
}

[[nodiscard]] QImage SameSize(const QImage &image) {
	auto result = QImage(image.size(), QImage::Format_ARGB32_Premultiplied);
	if (!result.isNull()) {
		result.setDevicePixelRatio(1.);
	}
	return result;
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

[[nodiscard]] inline Pixel SampleClamped(
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

// The mean of two premultiplied pixels. The half that is lost is rounded
// down or up for all four channels at once (so a color never gets above
// its alpha), the caller alternates the direction in a checkerboard: many
// passes of plain rounding up would make the picture visibly brighter.
[[nodiscard]] inline Pixel Average(Pixel a, Pixel b, bool up) {
	const auto half = ((a ^ b) & 0xFEFEFEFEU) >> 1;
	return up ? ((a | b) - half) : ((a & b) + half);
}

void FixPremultiplied(QImage &image) {
	const auto target = TargetOf(image);
	const auto width = image.width();
	FxParallelRows(width, image.height(), [&](int top, int bottom) {
		for (auto y = top; y != bottom; ++y) {
			const auto line = target.row(y);
			for (auto x = 0; x != width; ++x) {
				const auto p = line[x];
				const auto a = (p >> 24);
				if (a == 0xFFU) {
					continue;
				}
				line[x] = (a << 24)
					| (std::min((p >> 16) & 0xFFU, a) << 16)
					| (std::min((p >> 8) & 0xFFU, a) << 8)
					| std::min(p & 0xFFU, a);
			}
		}
	});
}

[[nodiscard]] bool GaussianBlur(QImage &image, double sigma) {
	if (!(sigma >= kMinSigma)) {
		return true;
	} else if (!FxPrepare(image)) {
		return false;
	}
	FxGaussianBlur(image, sigma);
	FixPremultiplied(image);
	return true;
}

//
// Box blur.
//

// The window of a box blur with a fractional radius: 2 * whole + 1 pixels
// count fully and the two pixels next to them with the weight edge / 256.
struct BoxWindow {
	int whole = 0;
	uint32 edge = 0;
	double scale = 1.;
};

[[nodiscard]] BoxWindow WindowFor(double radius) {
	radius = std::clamp(radius, 0., kMaxBoxRadius);
	const auto whole = int(std::floor(radius));
	const auto edge = uint32(std::lround((radius - whole) * 256.));
	return {
		.whole = whole,
		.edge = edge,
		.scale = 1. / ((2 * whole + 1) * 256. + 2. * edge),
	};
}

void BoxRows(const Source &from, const Target &to, const BoxWindow &window) {
	const auto width = from.width;
	const auto last = width - 1;
	const auto reach = window.whole;
	const auto edge = window.edge;
	const auto scale = window.scale;
	FxParallelRows(width, from.height, [&](int top, int bottom) {
		for (auto y = top; y != bottom; ++y) {
			const auto in = from.row(y);
			const auto out = to.row(y);
			uint32 sums[4] = { 0, 0, 0, 0 };
			for (auto c = 0; c != 4; ++c) {
				sums[c] = ((in[0] >> (8 * c)) & 0xFFU) * uint32(reach + 1);
			}
			for (auto i = 1; i <= reach; ++i) {
				const auto p = in[std::min(i, last)];
				for (auto c = 0; c != 4; ++c) {
					sums[c] += (p >> (8 * c)) & 0xFFU;
				}
			}
			for (auto x = 0; x != width; ++x) {
				const auto before = in[std::max(x - reach - 1, 0)];
				const auto after = in[std::min(x + reach + 1, last)];
				const auto drop = in[std::max(x - reach, 0)];
				auto result = Pixel(0);
				for (auto c = 0; c != 4; ++c) {
					const auto shift = 8 * c;
					const auto next = (after >> shift) & 0xFFU;
					const auto sides = ((before >> shift) & 0xFFU) + next;
					const auto value = (double(sums[c]) * 256.
						+ double(sides * edge)) * scale;
					result |= Pixel(int(value + 0.5)) << shift;
					sums[c] += next - ((drop >> shift) & 0xFFU);
				}
				out[x] = result;
			}
		}
	});
}

// The vertical pass walks the rows from the top to the bottom keeping
// a running sum for every column of its band, so the memory is read row
// by row (walking every column separately would miss the cache at each
// step of a big picture).
void BoxColumns(
		const Source &from,
		const Target &to,
		const BoxWindow &window) {
	const auto height = from.height;
	const auto last = height - 1;
	const auto reach = window.whole;
	const auto edge = window.edge;
	const auto scale = window.scale;
	FxParallel(from.width, 64, [&](int left, int right) {
		const auto count = right - left;
		auto sums = std::vector<uint32>(size_t(count) * 4, 0);
		const auto first = from.row(0) + left;
		for (auto i = 0; i != count; ++i) {
			for (auto c = 0; c != 4; ++c) {
				sums[i * 4 + c] = ((first[i] >> (8 * c)) & 0xFFU)
					* uint32(reach + 1);
			}
		}
		for (auto k = 1; k <= reach; ++k) {
			const auto line = from.row(std::min(k, last)) + left;
			for (auto i = 0; i != count; ++i) {
				for (auto c = 0; c != 4; ++c) {
					sums[i * 4 + c] += (line[i] >> (8 * c)) & 0xFFU;
				}
			}
		}
		for (auto y = 0; y != height; ++y) {
			const auto before = from.row(std::max(y - reach - 1, 0)) + left;
			const auto after = from.row(std::min(y + reach + 1, last)) + left;
			const auto drop = from.row(std::max(y - reach, 0)) + left;
			const auto out = to.row(y) + left;
			for (auto i = 0; i != count; ++i) {
				auto result = Pixel(0);
				for (auto c = 0; c != 4; ++c) {
					const auto shift = 8 * c;
					const auto next = (after[i] >> shift) & 0xFFU;
					const auto sides = ((before[i] >> shift) & 0xFFU) + next;
					auto &sum = sums[i * 4 + c];
					const auto value = (double(sum) * 256.
						+ double(sides * edge)) * scale;
					result |= Pixel(int(value + 0.5)) << shift;
					sum += next - ((drop[i] >> shift) & 0xFFU);
				}
				out[i] = result;
			}
		}
	});
}

[[nodiscard]] bool BoxBlur(QImage &image, double radiusX, double radiusY) {
	const auto horizontal = (radiusX > 0.);
	const auto vertical = (radiusY > 0.);
	if (!horizontal && !vertical) {
		return true;
	} else if (!FxPrepare(image)) {
		return false;
	}
	auto other = SameSize(image);
	if (other.isNull()) {
		return false;
	}
	if (horizontal) {
		BoxRows(SourceOf(image), TargetOf(other), WindowFor(radiusX));
		image.swap(other);
	}
	if (vertical) {
		BoxColumns(SourceOf(image), TargetOf(other), WindowFor(radiusY));
		image.swap(other);
	}
	return true;
}

//
// Motion, spin and zoom blurs.
//
// All three are the mean of the picture moved along a path: shifted along
// a line, turned around a point or scaled from a point. The mean of 2^n
// evenly spaced copies is made in n passes: the first one averages two
// copies that are one step apart, every next one averages the result with
// itself moved twice as far as before. So the cost grows with the
// logarithm of the length of the smear, and a short smear moves the
// copies by a small part of a pixel, which leaves the picture as sharp as
// it was (a nearly zero strength gives a nearly untouched picture).
//

// Source coordinates of a pixel: sx = xx * x + xy * y + x0 and
// sy = yx * x + yy * y + y0.
struct Affine {
	double xx = 1.;
	double xy = 0.;
	double x0 = 0.;
	double yx = 0.;
	double yy = 1.;
	double y0 = 0.;
};

[[nodiscard]] Affine Shift(double dx, double dy) {
	return { 1., 0., dx, 0., 1., dy };
}

[[nodiscard]] Affine Around(
		QPointF center,
		double xx,
		double xy,
		double yx,
		double yy) {
	return {
		xx,
		xy,
		center.x() - xx * center.x() - xy * center.y(),
		yx,
		yy,
		center.y() - yx * center.x() - yy * center.y(),
	};
}

void AveragePass(
		const Source &from,
		const Target &to,
		const Affine *first,
		const Affine &second,
		int pass) {
	const auto width = from.width;
	FxParallelRows(width, from.height, [&](int top, int bottom) {
		for (auto y = top; y != bottom; ++y) {
			const auto out = to.row(y);
			const auto direct = from.row(y);
			const auto secondX = second.xy * y + second.x0;
			const auto secondY = second.yy * y + second.y0;
			const auto firstX = first ? (first->xy * y + first->x0) : 0.;
			const auto firstY = first ? (first->yy * y + first->y0) : 0.;
			for (auto x = 0; x != width; ++x) {
				const auto one = first
					? SampleClamped(
						from,
						float(first->xx * x + firstX),
						float(first->yx * x + firstY))
					: direct[x];
				const auto two = SampleClamped(
					from,
					float(second.xx * x + secondX),
					float(second.yx * x + secondY));
				out[x] = Average(one, two, ((x + y + pass) & 1) != 0);
			}
		}
	});
}

// total is the length of the path in the units of the parameter that
// transform takes, reach is how many pixels the farthest pixel of the
// picture travels along it.
template <typename Transform>
[[nodiscard]] bool AverageAlong(
		QImage &image,
		double total,
		double reach,
		const Transform &transform,
		const std::atomic<bool> *cancel) {
	if (!(total > 0.) || !(reach > 0.)) {
		return !Cancelled(cancel);
	} else if (!FxPrepare(image)) {
		return false;
	}
	auto other = SameSize(image);
	if (other.isNull()) {
		return false;
	}
	const auto passes = std::clamp(
		int(std::ceil(std::log2(std::max(reach / kAverageStep, 1.)))),
		1,
		kMaxAveragePasses);
	const auto step = total / double(1 << passes);
	const auto start = (step - total) / 2.;
	const auto first = transform(start);
	AveragePass(
		SourceOf(image),
		TargetOf(other),
		&first,
		transform(start + step),
		0);
	image.swap(other);
	for (auto pass = 1; pass != passes; ++pass) {
		if (Cancelled(cancel)) {
			return false;
		}
		AveragePass(
			SourceOf(image),
			TargetOf(other),
			nullptr,
			transform(step * double(1 << pass)),
			pass);
		image.swap(other);
	}
	return !Cancelled(cancel);
}

[[nodiscard]] double FarthestCorner(QSize size, QPointF center) {
	auto result = 0.;
	for (const auto x : { 0., size.width() - 1. }) {
		for (const auto y : { 0., size.height() - 1. }) {
			result = std::max(
				result,
				std::hypot(x - center.x(), y - center.y()));
		}
	}
	return result;
}

//
// Regions.
//

enum class FieldType : uchar {
	Whole,
	Linear,
	Radial,
	Band,
};

// How strong the blur is at every pixel, 0..1.
struct Field {
	FieldType type = FieldType::Whole;
	float centerX = 0.f;
	float centerY = 0.f;
	float directionX = 1.f;
	float directionY = 0.f;
	float start = 0.f;
	float length = 1.f;
	bool invert = false;

	[[nodiscard]] float at(int x, int y) const {
		const auto dx = x - centerX;
		const auto dy = y - centerY;
		auto value = 1.f;
		switch (type) {
		case FieldType::Whole:
			return 1.f;
		case FieldType::Linear:
			value = (dx * directionX + dy * directionY) / length + 0.5f;
			break;
		case FieldType::Radial:
			value = (std::sqrt(dx * dx + dy * dy) - start) / length;
			break;
		case FieldType::Band:
			value = (std::abs(dx * directionX + dy * directionY) - start)
				/ length;
			break;
		}
		value = FxClamp01(value);
		value = value * value * (3.f - 2.f * value);
		return invert ? (1.f - value) : value;
	}
};

void MixLevel(
		const Target &to,
		const Source &level,
		const Field &field,
		int levels,
		int index) {
	const auto width = level.width;
	FxParallelRows(width, level.height, [&](int top, int bottom) {
		for (auto y = top; y != bottom; ++y) {
			const auto out = to.row(y);
			const auto in = level.row(y);
			for (auto x = 0; x != width; ++x) {
				const auto part = field.at(x, y) * levels - (index - 1);
				if (part <= 0.f) {
					continue;
				} else if (part >= 1.f) {
					out[x] = in[x];
				} else {
					out[x] = Blend256(
						out[x],
						in[x],
						uint32(int(part * 256.f + 0.5f)));
				}
			}
		}
	});
}

// blur(QImage &target, double part) blurs target with the given part of
// the full strength, false if it was cancelled. Inside a gradient the
// picture is blurred with 1 / levels, 2 / levels... of the strength and
// a pixel where the field is between two of them is mixed from those two.
template <typename Blur>
[[nodiscard]] bool ApplyField(
		QImage &image,
		const Field &field,
		int levels,
		const FxContext &context,
		const Blur &blur) {
	if (field.type == FieldType::Whole) {
		return blur(image, 1.) && !image.isNull() && !context.cancelled();
	} else if (!FxPrepare(image)) {
		return false;
	}
	const auto source = image;
	for (auto index = 1; index <= levels; ++index) {
		auto level = source;
		if (!blur(level, index / double(levels))
			|| context.cancelled()
			|| level.size() != source.size()
			|| level.format() != QImage::Format_ARGB32_Premultiplied) {
			return false;
		}
		MixLevel(TargetOf(image), SourceOf(level), field, levels, index);
	}
	return !context.cancelled();
}

[[nodiscard]] Field ReadRegion(const FxParams &params, QSize size) {
	auto result = Field();
	const auto type = params.integer("region");
	if (type != 1 && type != 2) {
		return result;
	}
	const auto center = params.point("region_center");
	const auto side = double(std::min(size.width(), size.height()));
	result.centerX = float(center.x() * size.width() - 0.5);
	result.centerY = float(center.y() * size.height() - 0.5);
	result.invert = params.boolean("region_invert");
	if (type == 1) {
		const auto radians = params.number("region_angle") * kPi / 180.;
		result.type = FieldType::Linear;
		result.directionX = float(std::cos(radians));
		result.directionY = float(std::sin(radians));
		result.length = std::max(
			float(params.number("region_length") / 100. * side),
			1.f);
	} else {
		result.type = FieldType::Radial;
		result.start = float(params.number("region_radius") / 100. * side);
		result.length = std::max(
			float(params.number("region_feather") / 100. * side),
			1.f);
	}
	return result;
}

[[nodiscard]] std::vector<FxParam> WithRegion(std::vector<FxParam> params) {
	const auto linear = [](const FxParams &values) {
		return values.integer("region") == 1;
	};
	const auto radial = [](const FxParams &values) {
		return values.integer("region") == 2;
	};
	const auto limited = [](const FxParams &values) {
		return values.integer("region") != 0;
	};
	params.push_back(FxChoice(
		"region",
		tr::lng_oblivion_photo_blur_region,
		{
			tr::lng_oblivion_photo_blur_region_whole,
			tr::lng_oblivion_photo_blur_region_linear,
			tr::lng_oblivion_photo_blur_region_radial,
		}));
	params.push_back(FxPoint(
		"region_center",
		tr::lng_oblivion_photo_blur_region_center).when(limited));
	params.push_back(FxAngle(
		"region_angle",
		tr::lng_oblivion_photo_blur_region_angle,
		90.).when(linear));
	params.push_back(FxFloat(
		"region_length",
		tr::lng_oblivion_photo_blur_region_length,
		1.,
		200.,
		50.,
		0,
		u"%"_q).when(linear));
	params.push_back(FxFloat(
		"region_radius",
		tr::lng_oblivion_photo_blur_region_radius,
		0.,
		150.,
		25.,
		0,
		u"%"_q).when(radial));
	params.push_back(FxFloat(
		"region_feather",
		tr::lng_oblivion_photo_blur_region_feather,
		1.,
		150.,
		30.,
		0,
		u"%"_q).when(radial));
	params.push_back(FxBool(
		"region_invert",
		tr::lng_oblivion_photo_blur_region_invert,
		false).when(limited));
	return params;
}

//
// Lens blur.
//
// Every pixel is replaced by the mean of the pixels under the aperture
// shape around it, taken in linear light with the highlights boosted
// (that is what turns bright points into discs). The sum under the shape
// is not recomputed for every pixel: moving one pixel to the right only
// changes it at the left and right borders of every row of the shape, so
// a pixel costs a few additions per row of the shape instead of its whole
// area. Radii above kLensWorkRadius are computed on a reduced picture:
// the blur hides the lost detail and the time stops growing.
//

struct LensTap {
	ptrdiff_t offset = 0;
	int weight = 0;
};

struct LensKernel {
	int reach = 0;
	qint64 total = 0;
	std::vector<LensTap> all;
	std::vector<LensTap> moves;
};

struct LensTone {
	std::array<uint32, 256> expand = {};
};

[[nodiscard]] int BokehCorners(FxBokehShape shape) {
	switch (shape) {
	case FxBokehShape::Pentagon: return 5;
	case FxBokehShape::Hexagon: return 6;
	case FxBokehShape::Octagon: return 8;
	case FxBokehShape::Disc: break;
	}
	return 0;
}

[[nodiscard]] bool InsideBokeh(
		double x,
		double y,
		double radius,
		int corners,
		double rotation) {
	const auto distance = std::sqrt(x * x + y * y);
	if (distance > radius) {
		return false;
	} else if (corners < 3) {
		return true;
	}
	const auto sector = 2. * kPi / corners;
	auto angle = std::atan2(y, x) - rotation;
	angle -= std::floor(angle / sector) * sector;
	return distance * std::cos(angle - sector / 2.)
		<= radius * std::cos(sector / 2.);
}

// rowLength is the distance between the rows of the buffer the offsets
// are for, in its elements (four per pixel).
[[nodiscard]] LensKernel BuildLensKernel(
		double radius,
		FxBokehShape shape,
		double rotation,
		ptrdiff_t rowLength) {
	auto result = LensKernel();
	const auto corners = BokehCorners(shape);
	const auto reach = std::max(int(std::ceil(radius)), 1);
	const auto side = 2 * reach + 1;
	auto weights = std::vector<int>(size_t(side) * side, 0);
	for (auto dy = -reach; dy <= reach; ++dy) {
		for (auto dx = -reach; dx <= reach; ++dx) {
			auto inside = 0;
			for (auto j = 0; j != kLensSubSamples; ++j) {
				const auto y = dy + (j + 0.5) / kLensSubSamples - 0.5;
				for (auto i = 0; i != kLensSubSamples; ++i) {
					const auto x = dx + (i + 0.5) / kLensSubSamples - 0.5;
					if (InsideBokeh(x, y, radius, corners, rotation)) {
						++inside;
					}
				}
			}
			weights[size_t(dy + reach) * side + (dx + reach)] = inside;
			result.total += inside;
		}
	}
	result.reach = reach;
	for (auto dy = -reach; dy <= reach; ++dy) {
		const auto line = weights.data() + size_t(dy + reach) * side + reach;
		auto previous = 0;
		for (auto dx = -reach; dx <= reach + 1; ++dx) {
			const auto current = (dx <= reach) ? line[dx] : 0;
			const auto offset = dy * rowLength + ptrdiff_t(dx) * 4;
			if (current) {
				result.all.push_back({ offset, current });
			}
			if (previous != current) {
				result.moves.push_back({ offset, previous - current });
			}
			previous = current;
		}
	}
	return result;
}

[[nodiscard]] LensTone BuildLensTone(double highlights, double threshold) {
	auto result = LensTone();
	const auto gain = std::clamp(highlights, 0., 1.) * kLensGain;
	const auto from = std::clamp(threshold, 0., 0.999);
	for (auto i = 0; i != 256; ++i) {
		const auto value = i / 255.;
		const auto above = std::clamp((value - from) / (1. - from), 0., 1.);
		const auto boost = above * above * (3. - 2. * above);
		const auto light = std::pow(value, kLensGamma)
			* (1. + gain * boost)
			/ (1. + gain);
		result.expand[i] = uint32(std::lround(light * kLensFull));
	}
	return result;
}

[[nodiscard]] inline int LensCompress(const LensTone &tone, uint32 value) {
	auto low = 0;
	auto high = 255;
	while (low < high) {
		const auto middle = (low + high + 1) / 2;
		if (tone.expand[middle] <= value) {
			low = middle;
		} else {
			high = middle - 1;
		}
	}
	return (low < 255
		&& (tone.expand[low + 1] - value) < (value - tone.expand[low]))
		? (low + 1)
		: low;
}

inline void LensExpand(const LensTone &tone, Pixel p, uint32 *out) {
	const auto a = (p >> 24);
	if (a == 0xFFU) {
		out[0] = tone.expand[p & 0xFFU];
		out[1] = tone.expand[(p >> 8) & 0xFFU];
		out[2] = tone.expand[(p >> 16) & 0xFFU];
		out[3] = kLensFull;
	} else if (!a) {
		out[0] = out[1] = out[2] = out[3] = 0;
	} else {
		for (auto c = 0; c != 3; ++c) {
			const auto value = (p >> (8 * c)) & 0xFFU;
			const auto straight = std::min((value * 255U + a / 2) / a, 255U);
			out[c] = uint32(uint64(tone.expand[straight]) * a / 255U);
		}
		out[3] = a * kLensAlphaUnit;
	}
}

[[nodiscard]] inline Pixel LensResult(
		const LensTone &tone,
		const qint64 *sums,
		qint64 total) {
	const auto coverage = sums[3];
	if (coverage <= 0) {
		return 0;
	}
	const auto alpha = std::min(
		int(double(coverage) / (double(total) * kLensAlphaUnit) + 0.5),
		255);
	if (!alpha) {
		return 0;
	}
	const auto scale = double(kLensFull) / double(coverage);
	auto result = Pixel(alpha) << 24;
	for (auto c = 0; c != 3; ++c) {
		const auto value = std::clamp(
			double(sums[c]) * scale + 0.5,
			0.,
			double(kLensFull));
		const auto straight = LensCompress(tone, uint32(value));
		result |= Pixel((straight * alpha + 127) / 255) << (8 * c);
	}
	return result;
}

void LensRows(
		const Source &from,
		const Target &to,
		const LensKernel &kernel,
		const LensTone &tone,
		int top,
		int bottom,
		const std::atomic<bool> *cancel) {
	const auto width = from.width;
	const auto height = from.height;
	const auto reach = kernel.reach;
	const auto padded = width + 2 * reach;
	const auto rowLength = size_t(padded) * 4;
	const auto rows = (bottom - top) + 2 * reach;
	auto band = std::vector<uint32>(rowLength * rows);
	for (auto i = 0; i != rows; ++i) {
		const auto in = from.row(std::clamp(top - reach + i, 0, height - 1));
		const auto out = band.data() + rowLength * i;
		for (auto x = 0; x != padded; ++x) {
			LensExpand(
				tone,
				in[std::clamp(x - reach, 0, width - 1)],
				out + size_t(x) * 4);
		}
	}
	for (auto y = top; y != bottom; ++y) {
		if (Cancelled(cancel)) {
			return;
		}
		const auto out = to.row(y);
		auto center = band.data()
			+ rowLength * (y - top + reach)
			+ size_t(reach) * 4;
		qint64 sums[4] = { 0, 0, 0, 0 };
		for (const auto &tap : kernel.all) {
			const auto at = center + tap.offset;
			for (auto c = 0; c != 4; ++c) {
				sums[c] += qint64(at[c]) * tap.weight;
			}
		}
		for (auto x = 0; x != width; ++x) {
			out[x] = LensResult(tone, sums, kernel.total);
			if (x + 1 == width) {
				break;
			}
			for (const auto &tap : kernel.moves) {
				const auto at = center + tap.offset;
				for (auto c = 0; c != 4; ++c) {
					sums[c] += qint64(at[c]) * tap.weight;
				}
			}
			center += 4;
		}
	}
}

// The picture for a big radius is reduced in the same linear light the
// blur works in: a plain resize averages the encoded values and would
// lose most of the light of small bright points, the very points that
// should become the bokeh discs.
[[nodiscard]] QImage LensReduce(
		const QImage &image,
		QSize size,
		const LensTone &tone) {
	auto result = QImage(size, QImage::Format_ARGB32_Premultiplied);
	if (result.isNull()) {
		return result;
	}
	result.setDevicePixelRatio(1.);
	const auto from = SourceOf(image);
	const auto to = TargetOf(result);
	const auto width = size.width();
	const auto height = size.height();
	const auto first = [](int index, int full, int reduced) {
		return int((qint64(index) * full + reduced - 1) / reduced);
	};
	auto starts = std::vector<int>(width + 1);
	for (auto x = 0; x <= width; ++x) {
		starts[x] = first(x, from.width, width);
	}
	FxParallelRows(width, height, [&](int top, int bottom) {
		auto sums = std::vector<qint64>(size_t(width) * 4);
		for (auto y = top; y != bottom; ++y) {
			std::fill(begin(sums), end(sums), 0);
			const auto fromY = first(y, from.height, height);
			const auto tillY = first(y + 1, from.height, height);
			for (auto sy = fromY; sy != tillY; ++sy) {
				const auto in = from.row(sy);
				for (auto x = 0; x != width; ++x) {
					const auto sum = sums.data() + size_t(x) * 4;
					for (auto sx = starts[x]; sx != starts[x + 1]; ++sx) {
						uint32 expanded[4];
						LensExpand(tone, in[sx], expanded);
						for (auto c = 0; c != 4; ++c) {
							sum[c] += expanded[c];
						}
					}
				}
			}
			const auto out = to.row(y);
			for (auto x = 0; x != width; ++x) {
				out[x] = LensResult(
					tone,
					sums.data() + size_t(x) * 4,
					qint64(tillY - fromY) * (starts[x + 1] - starts[x]));
			}
		}
	});
	return result;
}

[[nodiscard]] bool LensDirect(
		QImage &image,
		double radius,
		FxBokehShape shape,
		double rotation,
		const LensTone &tone,
		const std::atomic<bool> *cancel) {
	const auto reach = std::max(int(std::ceil(radius)), 1);
	const auto kernel = BuildLensKernel(
		radius,
		shape,
		rotation,
		ptrdiff_t(image.width() + 2 * reach) * 4);
	if (kernel.total <= 0) {
		return true;
	}
	auto result = SameSize(image);
	if (result.isNull()) {
		return false;
	}
	const auto from = SourceOf(image);
	const auto to = TargetOf(result);
	FxParallelRows(from.width, from.height, [&](int top, int bottom) {
		LensRows(from, to, kernel, tone, top, bottom, cancel);
	});
	if (Cancelled(cancel)) {
		return false;
	}
	image = std::move(result);
	return true;
}

//
// Surface blur.
//
// A guided filter, every channel guided by itself: around a pixel the
// result is "slope * pixel + base". Where the channel hardly changes
// (by less than the threshold) the slope is zero and the pixel becomes
// the local mean, across a strong edge the slope is one and the pixel
// stays as it was. The slope goes from zero to one faster than in the
// textbook filter (by the square of the local variance): otherwise big
// thresholds mix the sharp picture with the blurred one half and half
// and it looks like fog. The slopes and bases are smooth, so they are
// computed on a reduced picture and stretched over the full one.
//

struct Stretch {
	std::vector<int> index;
	std::vector<float> part;
};

[[nodiscard]] Stretch StretchFor(int full, int reduced) {
	auto result = Stretch{
		std::vector<int>(full),
		std::vector<float>(full),
	};
	for (auto i = 0; i != full; ++i) {
		const auto position = std::clamp(
			(i + 0.5) * reduced / full - 0.5,
			0.,
			double(reduced - 1));
		result.index[i] = std::min(int(position), std::max(reduced - 2, 0));
		result.part[i] = float(position - result.index[i]);
	}
	return result;
}

//
// The effects.
//

[[nodiscard]] FxBokehShape ReadShape(const FxParams &params) {
	switch (params.integer("shape")) {
	case 1: return FxBokehShape::Pentagon;
	case 2: return FxBokehShape::Hexagon;
	case 3: return FxBokehShape::Octagon;
	}
	return FxBokehShape::Disc;
}

[[nodiscard]] FxParam RadiusParam(double limit, double value) {
	return FxPixels(
		"radius",
		tr::lng_oblivion_photo_blur_radius,
		0.,
		limit,
		value);
}

[[nodiscard]] QPointF ReadCenter(
		const FxParams &params,
		QByteArrayView id,
		QSize size) {
	const auto point = params.point(id);
	return QPointF(
		point.x() * size.width() - 0.5,
		point.y() * size.height() - 0.5);
}

void RegisterBlurs() {
	RegisterFx({
		.id = "blur.gaussian",
		.group = FxGroup::Blur,
		.name = tr::lng_oblivion_photo_blur_gaussian,
		.params = WithRegion({
			RadiusParam(250., 12.),
		}),
		.flags = kFxNeighbours,
		.order = 10,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			const auto sigma = context.px(params.number("radius"));
			return ApplyField(
				image,
				ReadRegion(params, image.size()),
				kRegionLevels,
				context,
				[&](QImage &target, double part) {
					return GaussianBlur(target, sigma * part);
				});
		},
		.identity = [](const FxParams &params) {
			return params.number("radius") <= 0.;
		},
	});
	RegisterFx({
		.id = "blur.box",
		.group = FxGroup::Blur,
		.name = tr::lng_oblivion_photo_blur_box,
		.params = WithRegion({
			RadiusParam(250., 12.),
		}),
		.flags = kFxNeighbours,
		.order = 20,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			const auto radius = context.px(params.number("radius"));
			return ApplyField(
				image,
				ReadRegion(params, image.size()),
				kRegionLevels,
				context,
				[&](QImage &target, double part) {
					return BoxBlur(target, radius * part, radius * part);
				});
		},
		.identity = [](const FxParams &params) {
			return params.number("radius") <= 0.;
		},
	});
	RegisterFx({
		.id = "blur.motion",
		.group = FxGroup::Blur,
		.name = tr::lng_oblivion_photo_blur_motion,
		.params = WithRegion({
			FxAngle("angle", tr::lng_oblivion_photo_blur_angle, 0., -90., 90.),
			FxPixels(
				"distance",
				tr::lng_oblivion_photo_blur_distance,
				0.,
				1000.,
				60.),
		}),
		.flags = kFxNeighbours,
		.order = 30,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			const auto angle = params.number("angle");
			const auto distance = context.px(params.number("distance"));
			return ApplyField(
				image,
				ReadRegion(params, image.size()),
				kRegionLevels,
				context,
				[&](QImage &target, double part) {
					return FxMotionBlur(
						target,
						angle,
						distance * part,
						context.cancel);
				});
		},
		.identity = [](const FxParams &params) {
			return params.number("distance") <= 0.;
		},
	});
	RegisterFx({
		.id = "blur.spin",
		.group = FxGroup::Blur,
		.name = tr::lng_oblivion_photo_blur_spin,
		// The strength goes first in every card, the point picked on the
		// photo after it (the same order the distortions have).
		.params = WithRegion({
			FxAngle(
				"angle",
				tr::lng_oblivion_photo_blur_spin_angle,
				10.,
				0.,
				180.),
			FxPoint("center", tr::lng_oblivion_photo_blur_center),
		}),
		.flags = kFxNeighbours,
		.order = 40,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			const auto center = ReadCenter(params, "center", image.size());
			const auto angle = params.number("angle");
			return ApplyField(
				image,
				ReadRegion(params, image.size()),
				kRegionLevels,
				context,
				[&](QImage &target, double part) {
					return FxSpinBlur(
						target,
						center,
						angle * part,
						context.cancel);
				});
		},
		.identity = [](const FxParams &params) {
			return params.number("angle") <= 0.;
		},
	});
	RegisterFx({
		.id = "blur.zoom",
		.group = FxGroup::Blur,
		.name = tr::lng_oblivion_photo_blur_zoom,
		.params = WithRegion({
			FxInt(
				"amount",
				tr::lng_oblivion_photo_blur_amount,
				0,
				100,
				20,
				u"%"_q),
			FxPoint("center", tr::lng_oblivion_photo_blur_center),
		}),
		.flags = kFxNeighbours,
		.order = 50,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			const auto center = ReadCenter(params, "center", image.size());
			const auto amount = params.integer("amount") / 100.;
			return ApplyField(
				image,
				ReadRegion(params, image.size()),
				kRegionLevels,
				context,
				[&](QImage &target, double part) {
					return FxZoomBlur(
						target,
						center,
						amount * part,
						context.cancel);
				});
		},
		.identity = [](const FxParams &params) {
			return params.integer("amount") <= 0;
		},
	});
	RegisterFx({
		.id = "blur.tilt_shift",
		.group = FxGroup::Blur,
		.name = tr::lng_oblivion_photo_blur_tilt,
		.params = {
			RadiusParam(150., 24.),
			FxPoint("center", tr::lng_oblivion_photo_blur_tilt_center),
			FxAngle("angle", tr::lng_oblivion_photo_blur_angle, 0., -90., 90.),
			FxFloat(
				"width",
				tr::lng_oblivion_photo_blur_tilt_width,
				0.,
				100.,
				20.,
				0,
				u"%"_q),
			FxFloat(
				"falloff",
				tr::lng_oblivion_photo_blur_tilt_falloff,
				1.,
				100.,
				25.,
				0,
				u"%"_q),
		},
		.flags = kFxNeighbours,
		.order = 60,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			const auto sigma = context.px(params.number("radius"));
			const auto center = ReadCenter(params, "center", image.size());
			const auto radians = params.number("angle") * kPi / 180.;
			const auto side = double(
				std::min(image.width(), image.height()));
			const auto field = Field{
				.type = FieldType::Band,
				.centerX = float(center.x()),
				.centerY = float(center.y()),
				.directionX = float(-std::sin(radians)),
				.directionY = float(std::cos(radians)),
				.start = float(params.number("width") / 100. * side / 2.),
				.length = std::max(
					float(params.number("falloff") / 100. * side),
					1.f),
			};
			return ApplyField(
				image,
				field,
				kTiltLevels,
				context,
				[&](QImage &target, double part) {
					return GaussianBlur(target, sigma * part);
				});
		},
		.identity = [](const FxParams &params) {
			return params.number("radius") <= 0.;
		},
	});
	RegisterFx({
		.id = "blur.lens",
		.group = FxGroup::Blur,
		.name = tr::lng_oblivion_photo_blur_lens,
		.params = WithRegion({
			RadiusParam(120., 16.),
			FxChoice("shape", tr::lng_oblivion_photo_blur_lens_shape, {
				tr::lng_oblivion_photo_blur_lens_disc,
				tr::lng_oblivion_photo_blur_lens_pentagon,
				tr::lng_oblivion_photo_blur_lens_hexagon,
				tr::lng_oblivion_photo_blur_lens_octagon,
			}),
			FxAngle(
				"rotation",
				tr::lng_oblivion_photo_blur_lens_rotation,
				0.,
				-90.,
				90.).when([](const FxParams &values) {
				return values.integer("shape") != 0;
			}),
			FxInt(
				"highlights",
				tr::lng_oblivion_photo_blur_lens_highlights,
				0,
				100,
				30,
				u"%"_q),
			FxInt(
				"threshold",
				tr::lng_oblivion_photo_blur_lens_threshold,
				0,
				100,
				80,
				u"%"_q).when([](const FxParams &values) {
				return values.integer("highlights") > 0;
			}),
		}),
		.flags = kFxNeighbours | kFxSlow,
		.order = 70,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			const auto args = FxLensBlurArgs{
				.radius = context.px(params.number("radius")),
				.shape = ReadShape(params),
				.rotation = params.number("rotation"),
				.highlights = params.integer("highlights") / 100.,
				.threshold = params.integer("threshold") / 100.,
			};
			return ApplyField(
				image,
				ReadRegion(params, image.size()),
				kRegionLevels,
				context,
				[&](QImage &target, double part) {
					auto scaled = args;
					scaled.radius *= part;
					return FxLensBlur(target, scaled, context.cancel);
				});
		},
		.identity = [](const FxParams &params) {
			return params.number("radius") <= 0.;
		},
	});
	RegisterFx({
		.id = "blur.surface",
		.group = FxGroup::Blur,
		.name = tr::lng_oblivion_photo_blur_surface,
		.params = WithRegion({
			RadiusParam(60., 14.),
			FxInt(
				"threshold",
				tr::lng_oblivion_photo_blur_surface_threshold,
				1,
				100,
				30,
				u"%"_q),
		}),
		.flags = kFxNeighbours,
		.order = 80,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			const auto radius = context.px(params.number("radius"));
			const auto threshold = params.integer("threshold") / 100.;
			return ApplyField(
				image,
				ReadRegion(params, image.size()),
				kRegionLevels,
				context,
				[&](QImage &target, double part) {
					return FxSurfaceBlur(
						target,
						radius * part,
						threshold,
						context.cancel);
				});
		},
		.identity = [](const FxParams &params) {
			return params.number("radius") <= 0.;
		},
	});
}

const auto Registered = FxRegistrar([] {
	RegisterBlurs();
});

//
// Self-test.
//

[[nodiscard]] QImage FlatImage(int width, int height, QColor color) {
	auto result = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
	result.fill(color);
	return result;
}

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

[[nodiscard]] double ColorSum(const QImage &image, bool linear = false) {
	auto result = 0.;
	for (auto y = 0; y != image.height(); ++y) {
		const auto line = FxRow(image, y);
		for (auto x = 0; x != image.width(); ++x) {
			for (auto shift = 0; shift != 24; shift += 8) {
				const auto value = (line[x] >> shift) & 0xFFU;
				result += linear
					? std::pow(value / 255., kLensGamma)
					: double(value);
			}
		}
	}
	return result;
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

[[nodiscard]] int Channel(const QImage &image, int x, int y, int shift = 8) {
	return int((FxRow(image, y)[x] >> shift) & 0xFFU);
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

[[nodiscard]] bool RunBlurSelfTest(QStringList &log) {
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
	const auto picture = FxTestImage(160, 120);
	const auto middle = QPointF(79.5, 59.5);
	const auto lens = [](double radius, double highlights = 0.) {
		return FxLensBlurArgs{
			.radius = radius,
			.highlights = highlights,
			.threshold = 0.5,
		};
	};

	// Nothing to do: the picture is not even copied.
	{
		auto same = true;
		const auto untouched = [&](const QImage &image) {
			same = same && (image.cacheKey() == picture.cacheKey());
		};
		auto image = picture;
		FxBoxBlur(image, 0., 0.);
		untouched(image);
		same = same && FxMotionBlur(image, 30., 0.);
		untouched(image);
		same = same && FxSpinBlur(image, middle, 0.);
		untouched(image);
		same = same && FxZoomBlur(image, middle, 0.);
		untouched(image);
		same = same && FxLensBlur(image, lens(0.));
		untouched(image);
		same = same && FxSurfaceBlur(image, 0., 0.3);
		untouched(image);
		check(same, u"zero strength leaves the picture alone"_q);

		auto identity = true;
		const auto strengths = std::vector<std::pair<QByteArray, QByteArray>>{
			{ "blur.gaussian", "radius" },
			{ "blur.box", "radius" },
			{ "blur.motion", "distance" },
			{ "blur.spin", "angle" },
			{ "blur.zoom", "amount" },
			{ "blur.tilt_shift", "radius" },
			{ "blur.lens", "radius" },
			{ "blur.surface", "radius" },
		};
		for (const auto &[id, param] : strengths) {
			const auto zero = MakeFx(id, { { param, FxValue::Number(0.) } });
			const auto fine = FindFx(id)
				&& FxIsIdentity(zero)
				&& !FxIsIdentity(MakeFx(id));
			if (!fine) {
				identity = false;
				log.push_back(
					u"FAIL: identity of "_q + QString::fromLatin1(id));
			}
		}
		check(identity, u"every blur is an identity at zero and only there"_q);
	}

	// A plain color stays the same plain color.
	{
		const auto flat = FlatImage(96, 72, QColor(180, 90, 40));
		const auto point = QPointF(40., 30.);
		auto worst = 0;
		const auto compare = [&](const QImage &image) {
			worst = std::max(worst, MaxDifference(image, flat));
		};
		auto image = flat;
		FxBoxBlur(image, 7.5, 3.25);
		compare(image);
		image = flat;
		auto done = GaussianBlur(image, 9.);
		compare(image);
		image = flat;
		done = FxMotionBlur(image, 33., 40.) && done;
		compare(image);
		image = flat;
		done = FxSpinBlur(image, point, 50.) && done;
		compare(image);
		image = flat;
		done = FxZoomBlur(image, point, 0.5) && done;
		compare(image);
		image = flat;
		done = FxLensBlur(image, lens(6.5, 1.)) && done;
		compare(image);
		image = flat;
		done = FxLensBlur(image, lens(40., 0.5)) && done;
		compare(image);
		image = flat;
		done = FxSurfaceBlur(image, 12., 0.3) && done;
		compare(image);
		check(
			done && worst <= 1,
			u"a plain color survives every blur (off by %1)"_q.arg(worst));

		const auto clear = FlatImage(64, 48, QColor(200, 40, 90, 120));
		image = clear;
		done = FxLensBlur(image, lens(5., 0.6));
		auto soft = MaxDifference(image, clear);
		image = clear;
		done = FxSurfaceBlur(image, 10., 0.3) && done;
		soft = std::max(soft, MaxDifference(image, clear));
		check(
			done && soft <= 2,
			u"a plain translucent color survives too (off by %1)"_q.arg(
				soft));
	}

	// The amount of light is kept.
	{
		auto blob = FlatImage(160, 120, QColor(40, 40, 40));
		for (auto y = 45; y != 75; ++y) {
			const auto line = FxRow(blob, y);
			for (auto x = 60; x != 100; ++x) {
				line[x] = 0xFFE6C878U;
			}
		}
		const auto before = ColorSum(blob);
		const auto change = [&](const QImage &image, bool linear = false) {
			const auto base = linear ? ColorSum(blob, true) : before;
			return std::abs(ColorSum(image, linear) - base) * 100. / base;
		};
		auto image = blob;
		auto done = GaussianBlur(image, 6.);
		const auto gaussian = change(image);
		image = blob;
		FxBoxBlur(image, 5., 5.);
		const auto box = change(image);
		image = blob;
		done = FxMotionBlur(image, 30., 20.) && done;
		const auto motion = change(image);
		image = blob;
		done = FxSpinBlur(image, middle, 25.) && done;
		const auto spin = change(image);
		image = blob;
		done = FxZoomBlur(image, middle, 0.2) && done;
		const auto zoom = change(image);
		image = blob;
		done = FxLensBlur(image, lens(6.)) && done;
		const auto disc = change(image, true);
		image = blob;
		done = FxLensBlur(image, lens(36.)) && done;
		const auto large = change(image, true);
		image = blob;
		done = FxSurfaceBlur(image, 10., 0.5) && done;
		const auto surface = change(image);
		info(u"light changes, %: gaussian %1, box %2, motion %3, spin %4, "
			"zoom %5, lens %6, big lens %7, surface %8"_q.arg(
				number(gaussian),
				number(box),
				number(motion),
				number(spin),
				number(zoom),
				number(disc),
				number(large),
				number(surface)));
		check(
			done
				&& gaussian < 0.5
				&& box < 0.5
				&& motion < 0.5
				&& spin < 0.5
				&& zoom < 2.
				&& disc < 2.
				&& large < 4.
				&& surface < 1.,
			u"blurs keep the amount of light"_q);
	}

	// Known kernels.
	{
		auto dot = FlatImage(41, 41, QColor(0, 0, 0));
		FxRow(dot, 20)[20] = 0xFFFFFFFFU;
		auto image = dot;
		FxBoxBlur(image, 2., 2.);
		auto square = true;
		for (auto y = 0; y != 41; ++y) {
			for (auto x = 0; x != 41; ++x) {
				const auto inside = (std::abs(x - 20) <= 2)
					&& (std::abs(y - 20) <= 2);
				square = square
					&& (FxRow(image, y)[x]
						== (inside ? 0xFF0A0A0AU : 0xFF000000U));
			}
		}
		check(square, u"a box blur of a point is a square of 255 / 25"_q);

		image = dot;
		FxBoxBlur(image, 1.5, 0.);
		check(
			Channel(image, 20, 20) == 64
				&& Channel(image, 19, 20) == 64
				&& Channel(image, 21, 20) == 64
				&& Channel(image, 18, 20) == 32
				&& Channel(image, 22, 20) == 32
				&& Channel(image, 17, 20) == 0
				&& Channel(image, 20, 19) == 0,
			u"a fractional box radius counts the outer pixels partly"_q);

		auto bar = FlatImage(81, 9, QColor(0, 0, 0));
		for (auto y = 0; y != 9; ++y) {
			FxRow(bar, y)[40] = 0xFFFFFFFFU;
		}
		image = bar;
		const auto blurred = GaussianBlur(image, 4.);
		auto sum = 0.;
		auto spread = 0.;
		for (auto x = 0; x != 81; ++x) {
			const auto value = Channel(image, x, 4);
			sum += value;
			spread += value * (x - 40.) * (x - 40.);
		}
		const auto sigma = (sum > 0.) ? std::sqrt(spread / sum) : 0.;
		check(
			blurred && std::abs(sigma - 4.) < 0.6 && std::abs(sum - 255.) < 8.,
			u"a gaussian blur spreads a line by its sigma (%1 for 4)"_q.arg(
				number(sigma)));

		auto block = FlatImage(120, 60, QColor(0, 0, 0));
		for (auto y = 28; y != 32; ++y) {
			for (auto x = 58; x != 62; ++x) {
				FxRow(block, y)[x] = 0xFFFFFFFFU;
			}
		}
		image = block;
		const auto moved = FxMotionBlur(image, 0., 24.);
		auto lit = 0;
		for (auto x = 0; x != 120; ++x) {
			lit += (Channel(image, x, 30) > 0) ? 1 : 0;
		}
		auto aside = 0;
		for (auto x = 0; x != 120; ++x) {
			aside = std::max({
				aside,
				Channel(image, x, 27),
				Channel(image, x, 32),
			});
		}
		check(
			moved
				&& !aside
				&& std::abs(lit - 28) <= 3
				&& std::abs(ColorSum(image) - ColorSum(block)) < 400.,
			u"a horizontal motion blur stays in its rows (%1 px for 4 "
			"+ 24)"_q.arg(lit));
		image = block;
		const auto turned = FxMotionBlur(image, 90., 24.);
		auto tall = 0;
		for (auto y = 0; y != 60; ++y) {
			tall += (Channel(image, 60, y) > 0) ? 1 : 0;
		}
		check(
			turned
				&& !Channel(image, 57, 30)
				&& !Channel(image, 62, 30)
				&& std::abs(tall - 28) <= 3,
			u"a vertical motion blur stays in its columns"_q);
	}

	// Spin and zoom keep their center, short smears keep the picture.
	{
		auto image = picture;
		auto done = FxSpinBlur(image, QPointF(80., 60.), 30.);
		const auto spinCenter = std::abs(
			Channel(image, 80, 60) - Channel(picture, 80, 60));
		const auto spinFar = FxImageDifference(image, picture);
		image = picture;
		done = FxZoomBlur(image, QPointF(80., 60.), 0.3) && done;
		const auto zoomCenter = std::abs(
			Channel(image, 80, 60) - Channel(picture, 80, 60));
		const auto zoomFar = FxImageDifference(image, picture);
		check(
			done
				&& spinCenter <= 2
				&& zoomCenter <= 2
				&& spinFar > 1.
				&& zoomFar > 1.,
			u"spin and zoom blurs keep the center sharp"_q);

		image = picture;
		done = FxMotionBlur(image, 17., 0.2);
		const auto motion = FxImageDifference(image, picture);
		image = picture;
		done = FxSpinBlur(image, middle, 0.05) && done;
		const auto spin = FxImageDifference(image, picture);
		image = picture;
		done = FxZoomBlur(image, middle, 0.001) && done;
		const auto zoom = FxImageDifference(image, picture);
		check(
			done && motion < 1. && spin < 1. && zoom < 1.,
			u"a tiny strength hardly changes the picture (%1, %2, %3)"_q.arg(
				number(motion),
				number(spin),
				number(zoom)));
	}

	// Regions.
	{
		const auto context = FxContext{ .fullSize = picture.size() };
		auto whole = picture;
		auto done = ApplyFx(
			whole,
			MakeFx("blur.gaussian", { { "radius", FxValue::Number(8.) } }),
			context);
		const auto regional = [&](int region, bool invert) {
			auto image = picture;
			done = ApplyFx(image, MakeFx("blur.gaussian", {
				{ "radius", FxValue::Number(8.) },
				{ "region", FxValue::Integer(region) },
				{ "region_angle", FxValue::Number(0.) },
				{ "region_length", FxValue::Number(20.) },
				{ "region_radius", FxValue::Number(10.) },
				{ "region_feather", FxValue::Number(20.) },
				{ "region_invert", FxValue::Boolean(invert) },
			}), context) && done;
			return image;
		};
		const auto left = QRect(0, 0, 60, 120);
		const auto right = QRect(100, 0, 60, 120);
		const auto linear = regional(1, false);
		const auto inverted = regional(1, true);
		check(
			done
				&& !MaxDifference(linear, picture, left)
				&& !MaxDifference(linear, whole, right)
				&& !MaxDifference(inverted, whole, left)
				&& !MaxDifference(inverted, picture, right)
				&& FxImageDifference(whole, picture) > 2.,
			u"a linear region goes from the sharp picture to the full "
			"blur"_q);
		const auto band = QRect(70, 0, 20, 120);
		const auto between = FxImageDifference(
			linear.copy(band),
			picture.copy(band));
		const auto full = FxImageDifference(
			whole.copy(band),
			picture.copy(band));
		check(
			between > 0.1 * full && between < 0.9 * full,
			u"the middle of a gradient is blurred by a part (%1 of %2)"_q.arg(
				number(between),
				number(full)));
		const auto radial = regional(2, false);
		const auto core = QRect(72, 52, 16, 16);
		const auto corner = QRect(0, 0, 30, 30);
		check(
			done
				&& !MaxDifference(radial, picture, core)
				&& !MaxDifference(radial, whole, corner)
				&& ValidPixels(radial),
			u"a radial region keeps its center sharp"_q);

		auto tilted = picture;
		done = ApplyFx(tilted, MakeFx("blur.tilt_shift", {
			{ "radius", FxValue::Number(8.) },
			{ "width", FxValue::Number(30.) },
			{ "falloff", FxValue::Number(20.) },
		}), context);
		check(
			done
				&& !MaxDifference(tilted, picture, QRect(0, 43, 160, 34))
				&& !MaxDifference(tilted, whole, QRect(0, 0, 160, 16))
				&& !MaxDifference(tilted, whole, QRect(0, 104, 160, 16)),
			u"tilt-shift keeps its band sharp and blurs the rest"_q);

		const auto clear = FxTestImage(160, 120, true);
		auto valid = true;
		for (const auto descriptor : FxInGroup(FxGroup::Blur)) {
			for (const auto region : { 1, 2 }) {
				auto image = clear;
				const auto applied = ApplyFx(image, MakeFx(descriptor->id, {
					{ "region", FxValue::Integer(region) },
				}), FxContext{ .seed = 5, .fullSize = clear.size() });
				if (!applied
					|| image.size() != clear.size()
					|| !ValidPixels(image)) {
					valid = false;
					log.push_back(u"FAIL: region of "_q
						+ QString::fromLatin1(descriptor->id));
				}
			}
		}
		check(valid, u"blurs in a region keep valid translucent pixels"_q);
	}

	// Lens blur makes discs, surface blur keeps edges.
	{
		auto dot = FlatImage(64, 64, QColor(12, 12, 12));
		FxRow(dot, 32)[32] = 0xFFFFFFFFU;
		auto disc = dot;
		auto done = FxLensBlur(disc, lens(8., 1.));
		const auto center = Channel(disc, 32, 32);
		check(
			done
				&& center > 20
				&& std::abs(Channel(disc, 38, 32) - center) <= 2
				&& std::abs(Channel(disc, 32, 26) - center) <= 2
				&& std::abs(Channel(disc, 36, 36) - center) <= 2
				&& Channel(disc, 43, 32) == 12
				&& Channel(disc, 40, 40) == 12,
			u"lens blur turns a bright point into an even disc (%1)"_q.arg(
				center));
		auto plain = dot;
		done = FxLensBlur(plain, lens(8.));
		check(
			done && Channel(plain, 32, 32) < center,
			u"the highlights setting makes the disc brighter"_q);
		auto hexagon = dot;
		auto args = lens(8., 1.);
		args.shape = FxBokehShape::Hexagon;
		done = FxLensBlur(hexagon, args);
		check(
			done
				&& Channel(hexagon, 32, 32) > center
				&& std::abs(
					Channel(hexagon, 39, 32) - Channel(hexagon, 32, 32)) <= 2
				&& Channel(hexagon, 32, 40) == 12
				&& Channel(disc, 32, 40) > 12,
			u"the aperture shape has corners"_q);
		auto wide = FlatImage(160, 160, QColor(12, 12, 12));
		FxRow(wide, 80)[80] = 0xFFFFFFFFU;
		done = FxLensBlur(wide, lens(40., 1.));
		check(
			done
				&& Channel(wide, 80, 80) >= 15
				&& Channel(wide, 100, 80) >= 15
				&& Channel(wide, 80, 130) == 12,
			u"a big radius keeps the light of a bright point (%1 over "
			"12)"_q.arg(Channel(wide, 80, 80)));

		auto step = QImage(120, 80, QImage::Format_ARGB32_Premultiplied);
		for (auto y = 0; y != 80; ++y) {
			const auto line = FxRow(step, y);
			for (auto x = 0; x != 120; ++x) {
				const auto value = ((x < 60) ? 70 : 180)
					+ int(std::lround(FxNoise(x, y, 3) * 8.f));
				line[x] = FxPack({
					value / 255.f,
					value / 255.f,
					value / 255.f,
					1.f,
				});
			}
		}
		const auto rough = [](const QImage &image, int from) {
			auto sum = 0.;
			auto squares = 0.;
			auto count = 0;
			for (auto y = 10; y != 70; ++y) {
				for (auto x = from; x != from + 30; ++x) {
					const auto value = Channel(image, x, y);
					sum += value;
					squares += value * value;
					++count;
				}
			}
			const auto mean = sum / count;
			return std::sqrt(std::max(squares / count - mean * mean, 0.));
		};
		auto smooth = step;
		done = FxSurfaceBlur(smooth, 8., 0.15);
		const auto before = rough(step, 15);
		const auto after = rough(smooth, 15);
		const auto edge = Channel(smooth, 61, 40) - Channel(smooth, 58, 40);
		check(
			done && after < before * 0.5 && edge > 85,
			u"surface blur smooths the noise (%1 -> %2) and keeps the edge "
			"(%3 of 110)"_q.arg(number(before), number(after)).arg(edge));
	}

	// The preview looks like the export.
	{
		const auto full = FxTestImage(512, 384);
		const auto half = FxResized(full, full.size() / 2);
		auto worst = 0.;
		auto done = true;
		for (const auto descriptor : FxInGroup(FxGroup::Blur)) {
			const auto instance = MakeFx(descriptor->id);
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
			u"blur previews match the export (worst %1)"_q.arg(
				number(worst)));
	}

	check(
		SurvivesExtremes(FxGroup::Blur, log),
		u"blurs survive extreme settings and tiny pictures"_q);

	// Cancelling.
	{
		const auto cancel = std::atomic<bool>(true);
		auto image = picture;
		auto stopped = !FxMotionBlur(image, 0., 30., &cancel);
		image = picture;
		stopped = stopped && !FxSpinBlur(image, middle, 20., &cancel);
		image = picture;
		stopped = stopped && !FxLensBlur(image, lens(6.), &cancel);
		image = picture;
		stopped = stopped && !FxSurfaceBlur(image, 8., 0.3, &cancel);
		check(stopped, u"a cancelled blur reports it"_q);
	}

	// Timings: big radii must stay fast.
	{
		const auto large = FxTestImage(2000, 1500);
		auto timer = QElapsedTimer();
		auto times = QStringList();
		auto done = true;
		const auto measure = [&](const QString &name, auto &&blur) {
			auto image = large;
			timer.start();
			done = blur(image) && done;
			times.push_back(u"%1 %2"_q.arg(name).arg(
				number(timer.nsecsElapsed() / 1e6, 0)));
		};
		measure(u"gaussian 4"_q, [](QImage &image) {
			return GaussianBlur(image, 4.);
		});
		measure(u"gaussian 200"_q, [](QImage &image) {
			return GaussianBlur(image, 200.);
		});
		measure(u"box 4"_q, [](QImage &image) {
			return BoxBlur(image, 4., 4.);
		});
		measure(u"box 200"_q, [](QImage &image) {
			return BoxBlur(image, 200., 200.);
		});
		measure(u"motion 20"_q, [](QImage &image) {
			return FxMotionBlur(image, 30., 20.);
		});
		measure(u"motion 800"_q, [](QImage &image) {
			return FxMotionBlur(image, 30., 800.);
		});
		measure(u"spin 5"_q, [](QImage &image) {
			return FxSpinBlur(image, QPointF(1000., 750.), 5.);
		});
		measure(u"spin 90"_q, [](QImage &image) {
			return FxSpinBlur(image, QPointF(1000., 750.), 90.);
		});
		measure(u"zoom 0.6"_q, [](QImage &image) {
			return FxZoomBlur(image, QPointF(1000., 750.), 0.6);
		});
		measure(u"lens 6"_q, [&](QImage &image) {
			return FxLensBlur(image, lens(6., 0.3));
		});
		measure(u"lens 16"_q, [&](QImage &image) {
			return FxLensBlur(image, lens(16., 0.3));
		});
		measure(u"lens 100"_q, [&](QImage &image) {
			return FxLensBlur(image, lens(100., 0.3));
		});
		measure(u"surface 4"_q, [](QImage &image) {
			return FxSurfaceBlur(image, 4., 0.3);
		});
		measure(u"surface 80"_q, [](QImage &image) {
			return FxSurfaceBlur(image, 80., 0.3);
		});
		measure(u"tilt-shift"_q, [](QImage &image) {
			return ApplyFx(
				image,
				MakeFx("blur.tilt_shift"),
				FxContext{ .fullSize = image.size() });
		});
		info(u"2000x1500, ms: "_q + times.join(u", "_q));
		check(done, u"every blur ran on a 3 MP picture"_q);
	}

	return ok;
}

const auto BlurSelfTest = SelfTestRegistrar(
	SelfTestSuite::Fx,
	"blur",
	&RunBlurSelfTest);

//
// UI snapshot scenes.
//

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	RegisterEditorScene({
		.name = u"photo_blur_tilt_shift"_q,
		.tab = PhotoEditorTab::Layer,
		.prepare = [](not_null<Controller*> controller) {
			controller->addFx(
				controller->activeLayerId(),
				MakeFx("blur.tilt_shift", {
					{ "radius", FxValue::Number(30.) },
					{ "center", FxValue::Point(QPointF(0.5, 0.6)) },
					{ "angle", FxValue::Number(-6.) },
					{ "width", FxValue::Number(16.) },
				}));
		},
	});
	RegisterEditorScene({
		.name = u"photo_blur_lens_region"_q,
		.tab = PhotoEditorTab::Layer,
		.prepare = [](not_null<Controller*> controller) {
			controller->addFx(
				controller->activeLayerId(),
				MakeFx("blur.lens", {
					{ "radius", FxValue::Number(22.) },
					{ "shape", FxValue::Integer(2) },
					{ "rotation", FxValue::Number(15.) },
					{ "highlights", FxValue::Integer(60) },
					{ "threshold", FxValue::Integer(70) },
					{ "region", FxValue::Integer(2) },
					{ "region_center", FxValue::Point(QPointF(0.45, 0.55)) },
					{ "region_radius", FxValue::Number(22.) },
					{ "region_feather", FxValue::Number(35.) },
				}));
		},
	});
	RegisterPanelScene({
		.name = u"photo_blur_panel"_q,
		.size = QSize(EditorUi::Px(340), 0),
		.prepare = [](not_null<Controller*> controller) {
			const auto layer = controller->activeLayerId();
			for (const auto descriptor : FxInGroup(FxGroup::Blur)) {
				const auto region = (descriptor->id == "blur.gaussian")
					? 1
					: (descriptor->id == "blur.lens")
					? 2
					: 0;
				controller->addFx(layer, MakeFx(descriptor->id, {
					{ "region", FxValue::Integer(region) },
					{ "shape", FxValue::Integer(2) },
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
});

} // namespace

void FxBoxBlur(QImage &image, double radiusX, double radiusY) {
	if (!image.isNull()) {
		[[maybe_unused]] const auto done = BoxBlur(image, radiusX, radiusY);
	}
}

bool FxMotionBlur(
		QImage &image,
		double angle,
		double distance,
		const std::atomic<bool> *cancel) {
	if (image.isNull()) {
		return false;
	}
	const auto radians = angle * kPi / 180.;
	const auto dx = std::cos(radians);
	const auto dy = std::sin(radians);
	const auto length = std::min(
		distance,
		4. * std::hypot(image.width(), image.height()));
	return AverageAlong(image, length, length, [=](double t) {
		return Shift(t * dx, t * dy);
	}, cancel);
}

bool FxSpinBlur(
		QImage &image,
		QPointF center,
		double angle,
		const std::atomic<bool> *cancel) {
	if (image.isNull()) {
		return false;
	}
	const auto total = std::min(std::abs(angle), kMaxSpinAngle) * kPi / 180.;
	const auto reach = total * FarthestCorner(image.size(), center);
	return AverageAlong(image, total, reach, [=](double t) {
		const auto cosine = std::cos(t);
		const auto sine = std::sin(t);
		return Around(center, cosine, -sine, sine, cosine);
	}, cancel);
}

bool FxZoomBlur(
		QImage &image,
		QPointF center,
		double amount,
		const std::atomic<bool> *cancel) {
	if (image.isNull()) {
		return false;
	}
	const auto total = std::min(std::abs(amount), kMaxZoomAmount);
	const auto reach = (std::exp(total / 2.) - std::exp(-total / 2.))
		* FarthestCorner(image.size(), center);
	return AverageAlong(image, total, reach, [=](double t) {
		const auto scale = std::exp(t);
		return Around(center, scale, 0., 0., scale);
	}, cancel);
}

bool FxLensBlur(
		QImage &image,
		const FxLensBlurArgs &args,
		const std::atomic<bool> *cancel) {
	if (image.isNull()) {
		return false;
	} else if (!(args.radius >= kLensMinRadius)) {
		return !Cancelled(cancel);
	} else if (!FxPrepare(image)) {
		return false;
	}
	const auto tone = BuildLensTone(args.highlights, args.threshold);
	const auto rotation = args.rotation * kPi / 180.;
	if (args.radius <= kLensWorkRadius) {
		return LensDirect(
			image,
			args.radius,
			args.shape,
			rotation,
			tone,
			cancel);
	}
	const auto size = image.size();
	const auto factor = args.radius / kLensWorkRadius;
	const auto reducedSize = QSize(
		std::max(int(std::lround(size.width() / factor)), 1),
		std::max(int(std::lround(size.height() / factor)), 1));
	auto reduced = LensReduce(image, reducedSize, tone);
	if (reduced.isNull()) {
		return false;
	}
	const auto radius = args.radius * reducedSize.width() / size.width();
	if (!LensDirect(reduced, radius, args.shape, rotation, tone, cancel)) {
		return false;
	}
	auto result = FxResized(reduced, size);
	if (!FxPrepare(result)) {
		return false;
	}
	FixPremultiplied(result);
	image = std::move(result);
	return !Cancelled(cancel);
}

bool FxSurfaceBlur(
		QImage &image,
		double radius,
		double threshold,
		const std::atomic<bool> *cancel) {
	if (image.isNull()) {
		return false;
	}
	const auto sigma = radius * kSurfaceSigma;
	if (!(sigma >= kMinSigma) || !(threshold > 0.)) {
		return !Cancelled(cancel);
	} else if (!FxPrepare(image)) {
		return false;
	}
	const auto width = image.width();
	const auto height = image.height();
	const auto factor = std::clamp(
		std::max(
			sigma / kSurfaceWorkSigma,
			std::sqrt(double(width) * height / kSurfaceMaxPixels)),
		1.,
		kSurfaceMaxFactor);
	const auto reducedSize = QSize(
		std::max(int(std::lround(width / factor)), 1),
		std::max(int(std::lround(height / factor)), 1));
	const auto reduced = (reducedSize == image.size())
		? image
		: FxResized(image, reducedSize);
	if (reduced.isNull()) {
		return false;
	}
	const auto reducedWidth = reduced.width();
	const auto reducedHeight = reduced.height();
	const auto count = size_t(reducedWidth) * reducedHeight;
	const auto reducedSigma = std::max(
		sigma * reducedWidth / width,
		kSurfaceMinWorkSigma);
	const auto blur = [&](std::vector<float> &plane) {
		FxGaussianBlur(plane, reducedWidth, reducedHeight, reducedSigma);
	};
	const auto limit = std::clamp(threshold, 0., 1.) * kSurfaceRange;
	const auto epsilon = float(limit * limit);
	const auto columns = StretchFor(width, reducedWidth);
	const auto lines = StretchFor(height, reducedHeight);
	const auto from = SourceOf(reduced);
	const auto source = image;
	const auto original = SourceOf(source);
	const auto to = TargetOf(image);
	auto slope = std::vector<float>(count);
	auto base = std::vector<float>(count);
	for (auto channel = 3; channel >= 0; --channel) {
		const auto shift = 8 * channel;
		auto uniform = std::atomic<bool>(true);
		FxParallelRows(reducedWidth, reducedHeight, [&](int top, int bottom) {
			const auto first = (from.row(0)[0] >> shift) & 0xFFU;
			auto same = true;
			for (auto y = top; y != bottom; ++y) {
				const auto in = from.row(y);
				const auto index = size_t(y) * reducedWidth;
				for (auto x = 0; x != reducedWidth; ++x) {
					const auto byte = (in[x] >> shift) & 0xFFU;
					const auto value = byte * (1.f / 255.f);
					same = same && (byte == first);
					base[index + x] = value;
					slope[index + x] = value * value;
				}
			}
			if (!same) {
				uniform = false;
			}
		});
		if (uniform) {
			continue;
		}
		blur(base);
		blur(slope);
		for (auto i = size_t(0); i != count; ++i) {
			const auto mean = base[i];
			const auto spread = std::max(slope[i] - mean * mean, 0.f);
			const auto square = spread * spread;
			const auto value = square / (square + epsilon * epsilon);
			slope[i] = value;
			base[i] = (1.f - value) * mean;
		}
		blur(base);
		blur(slope);
		if (Cancelled(cancel)) {
			return false;
		}
		FxParallelRows(width, height, [&](int top, int bottom) {
			const auto last = reducedWidth - 1;
			for (auto y = top; y != bottom; ++y) {
				const auto in = original.row(y);
				const auto out = to.row(y);
				const auto line = size_t(lines.index[y]) * reducedWidth;
				const auto next = size_t(
					std::min(lines.index[y] + 1, reducedHeight - 1))
					* reducedWidth;
				const auto py = lines.part[y];
				for (auto x = 0; x != width; ++x) {
					const auto left = columns.index[x];
					const auto right = std::min(left + 1, last);
					const auto px = columns.part[x];
					const auto mix = [&](const std::vector<float> &plane) {
						const auto above = FxMix(
							plane[line + left],
							plane[line + right],
							px);
						const auto below = FxMix(
							plane[next + left],
							plane[next + right],
							px);
						return FxMix(above, below, py);
					};
					const auto value = mix(slope)
						* (((in[x] >> shift) & 0xFFU) * (1.f / 255.f))
						+ mix(base);
					const auto byte = uint32(std::clamp(
						int(value * 255.f + 0.5f),
						0,
						255));
					out[x] = (out[x] & ~(0xFFU << shift)) | (byte << shift);
				}
			}
		});
	}
	FixPremultiplied(image);
	return !Cancelled(cancel);
}

} // namespace Oblivion::Photo
