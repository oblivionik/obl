/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_photo_fx_digicam.h"

#include "lang/lang_keys.h"
#include "oblivion/oblivion_photo_fx_lofi.h"

#include <QtCore/QElapsedTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace Oblivion::Photo {
namespace {

constexpr auto kTwoPi = 6.28318530717958647692f;
constexpr auto kMinWorkSide = 8;
// The grain of the filters lives on the pixel grid of a picture of this
// size (larger photos get a softer grain of the same look).
constexpr auto kNominalSide = 1080;
constexpr auto kMaxDoubledSide = 8192;
constexpr auto kKnots = 9;
constexpr auto kLookUnit = 0.001f;
constexpr auto kFittedLooks = 18;
constexpr auto kReflectionWidth = 320;
constexpr auto kReflectionHeight = 180;
// A triangular noise of [-1, 1] brought to the deviation of the grain.
constexpr auto kFineGrainLevel = 0.32f;
constexpr auto kCoarseGrainLevel = 0.242f;
constexpr auto kCoarseGrainCellX = 2.2f;
constexpr auto kCoarseGrainCellY = 1.4f;
// A tint color at full amount moves a channel by this much at most.
constexpr auto kTintRange = 0.2f;
constexpr auto kBilateralSteps = 1024;

// The indices of the "look" parameter, stored in documents.
enum class Look : uchar {
	Nashville,
	ChiefKeef,
	Nuke,
	Phreshboy,
	Sepia,
	Year2014,
	Money,
	Pandora,
	BleachBypass,
	Stressor,
	Mono,
	MonoSoft,
	Desaturate,
	HsvDesaturate,
	Ccd,
	Nokia,
	Webcam,
	Phone,
	Soap,
	Calvin,
};
static_assert(int(Look::Calvin) + 1 == kDigicamLookCount);

// A filter is three steps: a tone curve for every channel, a mix of the
// three results with the largest and the smallest of them (that is what
// saturation, "vibrance" and lightness-based black and white are made of)
// and one more tone curve for every channel. The curves are polylines
// with nine evenly placed points, everything is in thousandths.
struct LookData {
	short pre[3][kKnots];
	short mix[3][5];
	short post[3][kKnots];
};

// In the order of Look, without Desaturate and HsvDesaturate (those two
// are plain formulas).
constexpr LookData kLooks[] = {
	{ // nashville
		.pre = {
			{ 53, 154, 279, 414, 567, 680, 770, 841, 885 },
			{ 43, 140, 255, 376, 484, 596, 700, 785, 840 },
			{ 71, 253, 429, 598, 733, 824, 895, 937, 931 },
		},
		.mix = {
			{ 1163, 0, 0, 0, 0 },
			{ 0, 1247, 0, 0, 0 },
			{ 0, 0, 1033, 2, 1 },
		},
		.post = {
			{ -77, 81, 223, 371, 534, 653, 772, 879, 980 },
			{ 56, 182, 320, 462, 579, 679, 753, 815, 874 },
			{ 374, 427, 473, 516, 553, 588, 625, 673, 730 },
		},
	},
	{ // chief_keef
		.pre = {
			{ 28, 111, 178, 240, 283, 328, 396, 598, 1151 },
			{ 37, 89, 143, 185, 241, 296, 381, 523, 725 },
			{ -28, 94, 137, 178, 219, 289, 347, 402, 447 },
		},
		.mix = {
			{ 1658, 377, 53, -213, -166 },
			{ 203, 1742, 66, -135, -174 },
			{ 71, 154, 2394, -72, -338 },
		},
		.post = {
			{ 56, 127, 270, 478, 774, 914, 953, 973, 994 },
			{ -3, 61, 240, 549, 796, 904, 955, 980, 997 },
			{ 100, 151, 231, 416, 608, 708, 807, 910, 999 },
		},
	},
	{ // nuke
		.pre = {
			{ 113, 161, 251, 303, 331, 360, 688, 1420, 2512 },
			{ -22, 90, 451, 616, 768, 821, 889, 943, 915 },
			{ 229, 266, 359, 458, 550, 648, 752, 876, 1011 },
		},
		.mix = {
			{ 2184, -41, 19, 27, -127 },
			{ 15, 805, 5, -3, 6 },
			{ -49, -269, 1017, 81, 149 },
		},
		.post = {
			{ -467, -70, 359, 524, 622, 757, 989, 987, 1000 },
			{ 208, 324, 373, 433, 538, 648, 1001, 999, 1060 },
			{ -43, 307, 414, 504, 598, 688, 779, 856, 961 },
		},
	},
	{ // phreshboy
		.pre = {
			{ -9, 159, 291, 402, 509, 619, 744, 877, 1010 },
			{ 106, 206, 306, 407, 507, 623, 740, 858, 976 },
			{ 106, 207, 307, 408, 508, 624, 742, 859, 977 },
		},
		.mix = {
			{ 989, 0, 0, 0, 0 },
			{ 0, 987, 0, 0, 0 },
			{ 0, 0, 984, 0, 0 },
		},
		.post = {
			{ 20, 154, 278, 386, 491, 612, 734, 858, 987 },
			{ -60, 26, 182, 338, 493, 628, 762, 895, 1027 },
			{ -59, 26, 182, 338, 494, 629, 763, 896, 1029 },
		},
	},
	{ // sepia
		.pre = {
			{ 84, 187, 258, 329, 407, 493, 582, 678, 777 },
			{ -10, 217, 467, 618, 741, 845, 951, 1059, 1166 },
			{ -29, 31, 79, 120, 161, 202, 246, 289, 336 },
		},
		.mix = {
			{ 1456, -70, -44, -2, 18 },
			{ -12, 942, 42, -80, -7 },
			{ 31, -59, 3136, -68, -113 },
		},
		.post = {
			{ 15, 96, 227, 388, 534, 661, 782, 894, 1000 },
			{ 13, 104, 177, 235, 342, 481, 654, 826, 997 },
			{ 0, 105, 212, 316, 415, 506, 596, 678, 755 },
		},
	},
	{ // 2014
		.pre = {
			{ -4, 160, 309, 727, 911, 1033, 1115, 1195, 1205 },
			{ -16, 86, 153, 217, 277, 387, 609, 790, 970 },
			{ -6, 130, 203, 243, 265, 287, 305, 326, 342 },
		},
		.mix = {
			{ 751, -77, -97, 132, 150 },
			{ -10, 1268, 45, 18, -8 },
			{ -48, -54, 3021, 59, 109 },
		},
		.post = {
			{ 127, 261, 412, 468, 510, 559, 653, 788, 1000 },
			{ 90, 206, 425, 659, 775, 831, 879, 936, 1000 },
			{ 238, 265, 298, 321, 374, 429, 533, 734, 1000 },
		},
	},
	{ // $$$
		.pre = {
			{ 84, 201, 318, 436, 553, 670, 787, 904, 1020 },
			{ 85, 185, 285, 385, 485, 586, 686, 787, 887 },
			{ 73, 183, 294, 407, 517, 629, 739, 850, 961 },
		},
		.mix = {
			{ 735, 235, 59, 1, 0 },
			{ 95, 982, 56, 0, 0 },
			{ 80, 113, 782, 2, 1 },
		},
		.post = {
			{ 133, 242, 350, 458, 566, 674, 782, 891, 1000 },
			{ -39, 11, 152, 294, 435, 577, 718, 859, 1000 },
			{ 198, 269, 338, 406, 474, 543, 611, 679, 748 },
		},
	},
	{ // pandora
		.pre = {
			{ 24, 168, 271, 363, 464, 572, 692, 822, 912 },
			{ 1, 150, 251, 355, 468, 585, 710, 834, 910 },
			{ 24, 174, 277, 366, 466, 576, 690, 797, 901 },
		},
		.mix = {
			{ 738, -100, 9, 522, -20 },
			{ -27, 634, 7, 541, -13 },
			{ -32, -87, 741, 521, -3 },
		},
		.post = {
			{ 20, 147, 266, 391, 526, 663, 789, 898, 1000 },
			{ 35, 166, 280, 397, 528, 660, 784, 894, 1000 },
			{ 57, 253, 339, 421, 505, 591, 675, 771, 1000 },
		},
	},
	{ // bleach_bypass
		.pre = {
			{ -16, 26, 64, 97, 131, 167, 206, 256, 326 },
			{ 64, 131, 204, 279, 362, 463, 579, 739, 946 },
			{ 44, 84, 120, 152, 183, 216, 252, 299, 349 },
		},
		.mix = {
			{ 1904, 819, 221, -308, 129 },
			{ 283, 1333, 157, -229, 54 },
			{ 529, 1024, 1831, -547, -141 },
		},
		.post = {
			{ -5, 24, 118, 309, 563, 766, 894, 960, 999 },
			{ -13, 8, 126, 336, 611, 791, 901, 958, 997 },
			{ 36, -6, 54, 197, 441, 696, 858, 946, 997 },
		},
	},
	{ // stressor
		.pre = {
			{ 179, 218, 262, 317, 381, 441, 495, 542, 584 },
			{ -162, -90, -6, 71, 154, 235, 313, 396, 465 },
			{ 198, 224, 318, 426, 532, 634, 722, 782, 815 },
		},
		.mix = {
			{ 950, 1264, 98, -34, -655 },
			{ 362, 1382, 37, 46, 42 },
			{ 292, 1153, 300, 206, -480 },
		},
		.post = {
			{ 15, -5, 40, 217, 450, 732, 997, 1001, 996 },
			{ 4, 72, 206, 393, 597, 822, 999, 1000, 999 },
			{ 0, 0, 64, 244, 469, 736, 989, 1003, 982 },
		},
	},
	{ // b and w
		.pre = {
			{ -435, -285, -137, 9, 165, 326, 469, 616, 752 },
			{ -312, -136, 38, 211, 393, 572, 742, 916, 1082 },
			{ 272, 285, 298, 311, 325, 340, 353, 368, 382 },
		},
		.mix = {
			{ 224, 614, 1014, 28, -10 },
			{ 224, 614, 1014, 28, -10 },
			{ 224, 614, 1014, 28, -10 },
		},
		.post = {
			{ 0, 70, 143, 239, 377, 555, 741, 907, 1000 },
			{ 0, 70, 143, 239, 377, 555, 741, 907, 1000 },
			{ 0, 70, 143, 239, 377, 555, 741, 907, 1000 },
		},
	},
	{ // b and w_clamped
		.pre = {
			{ -26, 84, 194, 304, 415, 526, 638, 750, 862 },
			{ -26, 85, 195, 305, 415, 526, 638, 750, 862 },
			{ -26, 84, 195, 304, 416, 526, 638, 750, 863 },
		},
		.mix = {
			{ -1, 0, -1, 586, 593 },
			{ -1, 0, -1, 586, 593 },
			{ -1, 0, -1, 586, 593 },
		},
		.post = {
			{ 139, 186, 266, 373, 516, 649, 747, 817, 855 },
			{ 139, 186, 266, 373, 516, 649, 747, 817, 855 },
			{ 139, 186, 266, 373, 516, 649, 747, 817, 855 },
		},
	},
	{ // ccd
		.pre = {
			{ 98, 141, 241, 327, 424, 529, 653, 771, 872 },
			{ 98, 125, 239, 329, 425, 529, 649, 815, 954 },
			{ 12, 88, 220, 329, 431, 546, 661, 797, 948 },
		},
		.mix = {
			{ 1638, 605, 206, -760, -421 },
			{ 176, 1303, -135, -348, 35 },
			{ 385, 265, 987, -273, -299 },
		},
		.post = {
			{ 1, 91, 202, 348, 464, 575, 681, 814, 998 },
			{ 3, 123, 284, 429, 558, 693, 823, 990, 1004 },
			{ 85, 141, 277, 413, 543, 671, 815, 989, 1003 },
		},
	},
	{ // nokia
		.pre = {
			{ -88, 19, 128, 241, 359, 471, 578, 678, 773 },
			{ -63, 53, 173, 293, 420, 531, 624, 711, 794 },
			{ -53, 46, 145, 248, 356, 469, 583, 688, 783 },
		},
		.mix = {
			{ 809, 11, -113, 308, 228 },
			{ 49, 753, 12, 352, -38 },
			{ 196, 107, 721, 287, -30 },
		},
		.post = {
			{ 1, 155, 279, 389, 492, 595, 700, 807, 919 },
			{ 2, 164, 288, 403, 511, 630, 765, 907, 1022 },
			{ 0, 1, 182, 334, 460, 574, 679, 783, 891 },
		},
	},
	{ // laptop
		.pre = {
			{ 41, 47, 56, 69, 82, 96, 109, 147, 180 },
			{ 6, 18, 31, 46, 62, 78, 94, 798, 1698 },
			{ 8, 19, 32, 45, 56, 64, 73, 753, 1063 },
		},
		.mix = {
			{ 6997, 69, 45, -48, -401 },
			{ 506, 8652, 40, -3, 17 },
			{ 1210, 974, 11862, -908, -2704 },
		},
		.post = {
			{ -742, -407, -72, 263, 478, 705, 975, 1000, 1000 },
			{ -69, 64, 197, 348, 500, 649, 814, 999, 1000 },
			{ -253, 114, 273, 413, 561, 753, 997, 1002, 1000 },
		},
	},
	{ // iphone3new
		.pre = {
			{ 30, 62, 92, 147, 204, 260, 317, 365, 415 },
			{ 53, 106, 156, 311, 428, 535, 643, 727, 803 },
			{ 224, 247, 267, 289, 314, 338, 363, 387, 412 },
		},
		.mix = {
			{ 1769, -123, -772, 477, 732 },
			{ -10, 1332, -281, -123, 373 },
			{ -95, -73, 800, 144, 165 },
		},
		.post = {
			{ -3, 189, 272, 355, 442, 529, 618, 724, 828 },
			{ 2, 175, 220, 284, 372, 470, 570, 681, 805 },
			{ -1135, -452, 230, 694, 1138, 1563, 1973, 2375, 2774 },
		},
	},
	{ // coloredvignette
		.pre = {
			{ 110, 215, 308, 396, 489, 588, 691, 796, 894 },
			{ 144, 205, 254, 312, 373, 449, 542, 671, 791 },
			{ 41, 114, 181, 230, 276, 319, 361, 403, 452 },
		},
		.mix = {
			{ 1135, 343, 121, -300, -91 },
			{ 18, 1317, -46, 27, 96 },
			{ 188, 310, 1926, -152, -399 },
		},
		.post = {
			{ -68, 10, 161, 300, 448, 586, 722, 856, 1000 },
			{ -393, -123, 147, 341, 521, 667, 796, 898, 1000 },
			{ -292, 88, 219, 335, 488, 654, 827, 998, 1002 },
		},
	},
	{ // calvin
		.pre = {
			{ 60, 64, 70, 77, 87, 99, 115, 134, 155 },
			{ -172, -105, -25, 65, 361, 805, 1425, 2272, 3445 },
			{ 93, 140, 200, 277, 375, 502, 651, 817, 1023 },
		},
		.mix = {
			{ 4552, 106, 103, -15, -6 },
			{ 1660, 268, 175, -39, 449 },
			{ 1512, 75, 606, 35, 108 },
		},
		.post = {
			{ -726, -384, -39, 318, 534, 689, 841, 996, 1001 },
			{ 12, 221, 409, 555, 669, 784, 893, 993, 1000 },
			{ -25, 4, 276, 478, 622, 752, 878, 998, 1000 },
		},
	},
};
static_assert(sizeof(kLooks) / sizeof(kLooks[0]) == kFittedLooks);

[[nodiscard]] QSize FullSize(const QImage &image, const FxContext &context) {
	if (!context.fullSize.isEmpty()) {
		return context.fullSize;
	}
	const auto scale = (context.scale > 0.) ? context.scale : 1.;
	return QSize(
		std::max(int(std::lround(image.width() / scale)), 1),
		std::max(int(std::lround(image.height() / scale)), 1));
}

[[nodiscard]] QSize SizeWithLongSide(QSize full, int side) {
	const auto longSide = std::max(full.width(), full.height());
	if (side >= longSide || longSide <= 0) {
		return full;
	}
	const auto scale = std::max(side, kMinWorkSide) / double(longSide);
	return QSize(
		std::max(int(std::lround(full.width() * scale)), 1),
		std::max(int(std::lround(full.height() * scale)), 1));
}

[[nodiscard]] float Percent(const FxParams &params, QByteArrayView id) {
	return float(params.number(id) / 100.);
}

[[nodiscard]] uint32 Seed(
		const FxParams &params,
		const FxContext &context,
		uint32 salt) {
	return uint32(FxHashCombine(
		FxHashCombine(context.seed, uint64(params.integer("seed"))),
		salt));
}

[[nodiscard]] std::array<float, 3> Channels(const QColor &color) {
	return {
		float(color.redF()),
		float(color.greenF()),
		float(color.blueF()),
	};
}

// What a tint color adds to the channels: the middle gray adds nothing,
// a pure color adds to its own channel and takes from the others.
[[nodiscard]] std::array<float, 3> TintOffsets(
		const QColor &color,
		float amount) {
	const auto channels = Channels(color);
	const auto k = 2.f * kTintRange * amount;
	return {
		(channels[0] - 0.5f) * k,
		(channels[1] - 0.5f) * k,
		(channels[2] - 0.5f) * k,
	};
}

[[nodiscard]] inline float Luma709(float r, float g, float b) {
	return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

[[nodiscard]] inline float Overlay(float base, float blend) {
	return (base < 0.5f)
		? (2.f * base * blend)
		: (1.f - 2.f * (1.f - base) * (1.f - blend));
}

[[nodiscard]] inline float Curve(const float *knots, float value) {
	const auto position = FxClamp01(value) * float(kKnots - 1);
	const auto index = std::min(int(position), kKnots - 2);
	return FxMix(knots[index], knots[index + 1], position - index);
}

[[nodiscard]] inline int ByteIndex(float value) {
	return std::clamp(int(value * 255.f + 0.5f), 0, 255);
}

// A number in [0, 1), the same for a pixel of the grid and a seed.
[[nodiscard]] inline float Uniform(int x, int y, uint32 seed) {
	return float(FxHash32(uint32(x), uint32(y), seed) >> 8)
		* (1.f / 16777216.f);
}

//
// The color filters.
//

class LookFilter final {
public:
	LookFilter(int look, float intensity);

	[[nodiscard]] bool empty() const {
		return _intensity <= 0.f;
	}
	void apply(FxRgba &color) const;

private:
	Look _look = Look::Nashville;
	float _intensity = 1.f;
	std::array<std::array<float, 256>, 3> _pre = {};
	float _mix[3][5] = {};
	float _post[3][kKnots] = {};

};

LookFilter::LookFilter(int look, float intensity)
: _look(Look(std::clamp(look, 0, kDigicamLookCount - 1)))
, _intensity(FxClamp01(intensity)) {
	if (_look == Look::Desaturate || _look == Look::HsvDesaturate) {
		return;
	}
	const auto index = (int(_look) < int(Look::Desaturate))
		? int(_look)
		: (int(_look) - 2);
	const auto &data = kLooks[std::clamp(index, 0, kFittedLooks - 1)];
	for (auto c = 0; c != 3; ++c) {
		auto knots = std::array<float, kKnots>{};
		for (auto i = 0; i != kKnots; ++i) {
			knots[i] = data.pre[c][i] * kLookUnit;
			_post[c][i] = data.post[c][i] * kLookUnit;
		}
		for (auto i = 0; i != 256; ++i) {
			_pre[c][i] = Curve(knots.data(), i / 255.f);
		}
		for (auto i = 0; i != 5; ++i) {
			_mix[c][i] = data.mix[c][i] * kLookUnit;
		}
	}
}

void LookFilter::apply(FxRgba &color) const {
	const auto r = FxClamp01(color.r);
	const auto g = FxClamp01(color.g);
	const auto b = FxClamp01(color.b);
	auto result = std::array<float, 3>{ r, g, b };
	if (_look == Look::Desaturate) {
		const auto gray = Luma709(r, g, b);
		result = { gray, gray, gray };
	} else if (_look == Look::HsvDesaturate) {
		// The saturation of HSV taken down: the color goes to its largest
		// channel, not to its brightness.
		const auto value = std::max({ r, g, b });
		result = { value, value, value };
	} else {
		const auto v = std::array<float, 3>{
			_pre[0][ByteIndex(r)],
			_pre[1][ByteIndex(g)],
			_pre[2][ByteIndex(b)],
		};
		const auto high = std::max({ v[0], v[1], v[2] });
		const auto low = std::min({ v[0], v[1], v[2] });
		for (auto c = 0; c != 3; ++c) {
			const auto &m = _mix[c];
			result[c] = FxClamp01(Curve(
				_post[c],
				m[0] * v[0] + m[1] * v[1] + m[2] * v[2]
					+ m[3] * high
					+ m[4] * low));
		}
	}
	color.r = FxMix(r, result[0], _intensity);
	color.g = FxMix(g, result[1], _intensity);
	color.b = FxMix(b, result[2], _intensity);
}

// Noise that lives on a grid of its own (the pixels of the nominal
// picture), read at the pixels of the rendered one. A grid coarser than
// the pixels is interpolated, the way a small noisy picture is enlarged;
// a finer one (a downscaled preview of a large photo) would average out
// in the export, so the preview gets a weaker noise instead.
class Lattice final {
public:
	Lattice(float cellX, float cellY, uint32 seed)
	: _stepX(1.f / std::max(cellX, 0.05f))
	, _stepY(1.f / std::max(cellY, 0.05f))
	, _gain(std::clamp(std::sqrt(cellX * cellY), 0.35f, 1.f))
	, _direct(cellX <= 1.05f && cellY <= 1.05f)
	, _seed(seed) {
	}

	// About [-1, 1].
	[[nodiscard]] float at(int x, int y) const {
		if (_direct) {
			return FxNoise(uint32(x), uint32(y), _seed) * _gain;
		}
		const auto fx = (x + 0.5f) * _stepX - 0.5f;
		const auto fy = (y + 0.5f) * _stepY - 0.5f;
		const auto ix = int(std::floor(fx));
		const auto iy = int(std::floor(fy));
		const auto tx = fx - ix;
		const auto ty = fy - iy;
		const auto a = FxNoise(uint32(ix), uint32(iy), _seed);
		const auto b = FxNoise(uint32(ix + 1), uint32(iy), _seed);
		const auto c = FxNoise(uint32(ix), uint32(iy + 1), _seed);
		const auto d = FxNoise(uint32(ix + 1), uint32(iy + 1), _seed);
		return FxMix(FxMix(a, b, tx), FxMix(c, d, tx), ty);
	}

private:
	float _stepX = 1.f;
	float _stepY = 1.f;
	float _gain = 1.f;
	bool _direct = true;
	uint32 _seed = 0;

};

struct LookSpec {
	int look = 0;
	float intensity = 1.f;
	float vignette = 0.f;
	float grain = 0.f;
	float cell = 1.f; // Rendered pixels in a pixel of the nominal picture.
	uint32 seed = 0;
};

// The filter, its vignette (a soft dark frame laid over the picture in
// the "overlay" mode, so the corners keep their contrast) and its fine
// gray grain, stronger in the shadows.
void StageLook(QImage &image, const LookSpec &spec) {
	const auto filter = LookFilter(spec.look, spec.intensity);
	const auto grain = Lattice(spec.cell, spec.cell, spec.seed);
	const auto w = image.width();
	const auto h = image.height();
	const auto vignette = FxClamp01(spec.vignette);
	FxForEachColor(image, [&](FxRgba &c, int x, int y) {
		if (!filter.empty()) {
			filter.apply(c);
		}
		if (vignette > 0.f) {
			const auto dx = (x + 0.5f) / w - 0.5f;
			const auto dy = (y + 0.5f) / h - 0.5f;
			const auto frame = 0.498f - 0.4f * FxSmoothStep(
				0.22f,
				0.75f,
				std::sqrt(dx * dx + dy * dy));
			c.r = FxMix(c.r, Overlay(c.r, frame), vignette);
			c.g = FxMix(c.g, Overlay(c.g, frame), vignette);
			c.b = FxMix(c.b, Overlay(c.b, frame), vignette);
		}
		if (spec.grain > 0.f) {
			const auto shadows = 1.f - 0.5f * Luma709(c.r, c.g, c.b);
			const auto blend = 0.5f + spec.grain * shadows * std::clamp(
				grain.at(x, y) * kFineGrainLevel,
				-0.5f,
				0.5f);
			c.r = Overlay(c.r, blend);
			c.g = Overlay(c.g, blend);
			c.b = Overlay(c.b, blend);
		}
	});
}

// A coarse colored grain, wider than tall: what a small sensor of a phone
// gives after its own noise reduction.
void StageCoarseGrain(
		QImage &image,
		float amount,
		float shadows,
		float cell,
		uint32 seed) {
	const auto cellX = kCoarseGrainCellX * cell;
	const auto cellY = kCoarseGrainCellY * cell;
	const auto common = Lattice(cellX, cellY, seed);
	const auto red = Lattice(cellX, cellY, seed ^ 0x51ED270BU);
	const auto green = Lattice(cellX, cellY, seed ^ 0x7F4A7C15U);
	const auto blue = Lattice(cellX, cellY, seed ^ 0xA3C59AC3U);
	FxForEachColor(image, [&](FxRgba &c, int x, int y) {
		const auto strength = amount
			* kCoarseGrainLevel
			* FxMix(1.f, 1.f - Luma709(c.r, c.g, c.b), shadows);
		const auto shared = 0.84f * common.at(x, y);
		const auto blend = [&](const Lattice &own) {
			return 0.5f + strength * std::clamp(
				shared + 0.55f * own.at(x, y),
				-2.f,
				2.f);
		};
		c.r = Overlay(c.r, blend(red));
		c.g = Overlay(c.g, blend(green));
		c.b = Overlay(c.b, blend(blue));
	});
}

//
// The tinted bloom.
//

struct GlowSpec {
	float amount = 1.f;
	std::array<float, 3> tint = { 1.f, 1.f, 1.f };
	float gamma = 3.5f; // The higher, the brighter a light has to be.
	float sigma = 18.f; // Rendered pixels.
	float white = 1.f; // Below one the lights are pushed up.
	int reduce = 0; // The highlights are blurred on a copy this much
	// smaller, zero: chosen by the size of the blur.
};

[[nodiscard]] const std::array<float, 256> &LinearTable() {
	static const auto result = [] {
		auto table = std::array<float, 256>();
		for (auto i = 0; i != 256; ++i) {
			table[i] = float(FxSrgbToLinear(i / 255.));
		}
		return table;
	}();
	return result;
}

[[nodiscard]] inline float ToSrgb(float linear) {
	return (linear <= 0.0031308f)
		? (std::max(linear, 0.f) * 12.92f)
		: (linear >= 1.f)
		? 1.f
		: (1.055f * std::pow(linear, 1.f / 2.4f) - 0.055f);
}

// The brightness of the picture raised to a power leaves only the lights,
// they are blurred, colored and added to the picture in linear light:
// a light source gets a colored haze around it, the way a cheap lens and
// a CCD sensor draw it.
void StageGlow(QImage &image, const GlowSpec &spec) {
	const auto w = image.width();
	const auto h = image.height();
	const auto reduce = (spec.reduce > 0)
		? spec.reduce
		: std::clamp(int(std::lround(spec.sigma / 3.f)), 1, 48);
	const auto rw = std::max(int(std::lround(w / float(reduce))), 1);
	const auto rh = std::max(int(std::lround(h / float(reduce))), 1);
	const auto &linear = LinearTable();
	auto map = std::vector<float>(size_t(rw) * rh);
	{
		// A copy of the same size shares the pixels of the picture: it
		// must be gone before the rows are written from several threads.
		const auto reduced = FxResized(image, QSize(rw, rh));
		if (reduced.isNull()) {
			return;
		}
		for (auto y = 0; y != rh; ++y) {
			const auto line = FxRow(reduced, y);
			const auto to = map.data() + size_t(y) * rw;
			for (auto x = 0; x != rw; ++x) {
				const auto p = line[x];
				const auto level = Luma709(
					linear[(p >> 16) & 0xFFU],
					linear[(p >> 8) & 0xFFU],
					linear[p & 0xFFU]);
				to[x] = std::pow(level, spec.gamma);
			}
		}
	}
	FxGaussianBlur(map, rw, rh, std::max(spec.sigma * rw / float(w), 0.3f));

	const auto scaleX = rw / float(w);
	const auto scaleY = rh / float(h);
	const auto tone = (spec.white < 0.999f);
	const auto whiteSquared = std::max(spec.white * spec.white, 0.00001f);
	FxParallelRows(w, h, [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto line = FxRow(image, y);
			const auto fy = std::clamp(
				(y + 0.5f) * scaleY - 0.5f,
				0.f,
				float(rh - 1));
			const auto y0 = int(fy);
			const auto ty = fy - y0;
			const auto top = map.data() + size_t(y0) * rw;
			const auto bottom = map.data()
				+ size_t(std::min(y0 + 1, rh - 1)) * rw;
			for (auto x = 0; x != w; ++x) {
				if (!(line[x] >> 24)) {
					continue;
				}
				const auto fx = std::clamp(
					(x + 0.5f) * scaleX - 0.5f,
					0.f,
					float(rw - 1));
				const auto x0 = int(fx);
				const auto x1 = std::min(x0 + 1, rw - 1);
				const auto tx = fx - x0;
				const auto light = spec.amount * FxMix(
					FxMix(top[x0], top[x1], tx),
					FxMix(bottom[x0], bottom[x1], tx),
					ty);
				if (light < 0.0004f && !tone) {
					continue;
				}
				auto c = FxUnpack(line[x]);
				const auto develop = [&](float value, float tint) {
					auto result = linear[ByteIndex(value)] + light * tint;
					if (tone) {
						result = result * (1.f + result / whiteSquared)
							/ (1.f + result);
					}
					return ToSrgb(result);
				};
				c.r = develop(c.r, spec.tint[0]);
				c.g = develop(c.g, spec.tint[1]);
				c.b = develop(c.b, spec.tint[2]);
				line[x] = FxPack(c);
			}
		}
	});
}

//
// The lens reflection.
//

struct Blob {
	float x = 0.f;
	float y = 0.f;
	float rx = 1.f;
	float ry = 1.f;
	float level = 0.f;
};

struct ReflectionShape {
	std::array<Blob, 4> blobs;
	int count = 0;
	float warm = 0.f; // Towards magenta.
};

// Soft glowing bands and spots of a light that got between the lenses.
constexpr ReflectionShape kReflections[kDigicamReflectionCount] = {
	{ { {
		{ 0.35f, 0.33f, 0.55f, 0.13f, 0.55f },
		{ 0.40f, 0.62f, 0.50f, 0.12f, 0.45f },
	} }, 2, 0.f },
	{ { {
		{ 0.80f, 0.18f, 0.35f, 0.22f, 1.05f },
		{ 0.10f, 0.85f, 0.30f, 0.28f, 1.05f },
		{ 0.62f, 0.88f, 0.32f, 0.16f, 0.90f },
		{ 0.45f, 0.42f, 0.32f, 0.16f, 0.65f },
	} }, 4, 0.35f },
	{ { {
		{ 0.62f, 0.55f, 0.55f, 0.26f, 1.25f },
		{ 0.82f, 0.20f, 0.26f, 0.07f, 0.60f },
	} }, 2, 0.15f },
	{ { {
		{ 0.42f, 0.73f, 0.36f, 0.08f, 0.95f },
		{ 0.60f, 0.43f, 0.65f, 0.045f, 0.25f },
	} }, 2, 0.f },
	{ { {
		{ 0.42f, 0.27f, 0.32f, 0.11f, 0.75f },
		{ 0.42f, 0.70f, 0.26f, 0.07f, 0.42f },
	} }, 2, 0.f },
	{ { {
		{ 0.72f, 0.25f, 0.55f, 0.11f, 1.00f },
		{ 0.62f, 0.76f, 0.52f, 0.11f, 1.05f },
		{ 0.88f, 0.50f, 0.25f, 0.16f, 0.90f },
	} }, 3, 0.f },
	{ { {
		{ 0.50f, 0.52f, 0.48f, 0.13f, 0.62f },
		{ 0.20f, 0.32f, 0.26f, 0.08f, 0.38f },
	} }, 2, 0.6f },
	{ { {
		{ 0.24f, 0.50f, 0.30f, 0.27f, 1.10f },
		{ 0.76f, 0.56f, 0.32f, 0.22f, 0.55f },
	} }, 2, 0.1f },
	{ { {
		{ 0.24f, 0.44f, 0.26f, 0.30f, 1.10f },
		{ 0.66f, 0.76f, 0.36f, 0.09f, 0.70f },
	} }, 2, 0.f },
	{ { {
		{ 0.50f, 0.22f, 0.42f, 0.09f, 0.90f },
		{ 0.82f, 0.50f, 0.26f, 0.16f, 0.75f },
		{ 0.30f, 0.72f, 0.46f, 0.08f, 0.50f },
	} }, 3, 0.f },
};

[[nodiscard]] QColor DefaultReflectionColor() {
	return QColor(120, 50, 240);
}

// The reflection is smooth, so it is painted small and stretched over
// the picture.
[[nodiscard]] QImage MakeReflection(int variant, const QColor &color) {
	const auto index = std::clamp(variant, 0, kDigicamReflectionCount - 1);
	const auto &shape = kReflections[index];
	auto result = QImage(
		kReflectionWidth,
		kReflectionHeight,
		QImage::Format_ARGB32_Premultiplied);
	if (result.isNull()) {
		return result;
	}
	auto base = Channels(color);
	base[0] = FxMix(base[0], 0.75f, shape.warm * 0.5f);
	base[1] = FxMix(base[1], 0.1f, shape.warm * 0.5f);
	base[2] = FxMix(base[2], 0.55f, shape.warm * 0.5f);
	for (auto y = 0; y != kReflectionHeight; ++y) {
		const auto line = FxRow(result, y);
		const auto v = (y + 0.5f) / kReflectionHeight;
		// Faint horizontal streaks.
		const auto streaks = 1.f - 0.14f
			* (0.5f + 0.5f * std::cos((v * 9.f + index * 0.37f) * kTwoPi));
		for (auto x = 0; x != kReflectionWidth; ++x) {
			const auto u = (x + 0.5f) / kReflectionWidth;
			auto sum = 0.f;
			for (auto i = 0; i != shape.count; ++i) {
				const auto &blob = shape.blobs[i];
				const auto dx = (u - blob.x) / blob.rx;
				const auto dy = (v - blob.y) / blob.ry;
				const auto distance = dx * dx + dy * dy;
				if (distance < 14.f) {
					sum += blob.level * std::exp(-distance);
				}
			}
			sum *= streaks;
			const auto body = std::min(sum, 1.f);
			const auto core = 0.9f * FxSmoothStep(0.6f, 1.3f, sum);
			line[x] = FxPack({
				base[0] * body + (1.f - base[0]) * core,
				base[1] * body + (1.f - base[1]) * core,
				base[2] * body + (1.f - base[2]) * core,
				1.f,
			});
		}
	}
	return result;
}

void StageReflection(
		QImage &image,
		int variant,
		float strength,
		const QColor &color) {
	const auto reflection = MakeReflection(variant, color);
	if (reflection.isNull()) {
		return;
	}
	const auto scaleX = reflection.width() / float(image.width());
	const auto scaleY = reflection.height() / float(image.height());
	FxForEachColor(image, [&](FxRgba &c, int x, int y) {
		const auto p = FxSample(
			reflection,
			(x + 0.5f) * scaleX - 0.5f,
			(y + 0.5f) * scaleY - 0.5f);
		// The "screen" mode: light is added, never taken.
		c.r += strength * (((p >> 16) & 0xFFU) / 255.f) * (1.f - c.r);
		c.g += strength * (((p >> 8) & 0xFFU) / 255.f) * (1.f - c.g);
		c.b += strength * ((p & 0xFFU) / 255.f) * (1.f - c.b);
	});
}

//
// The color vignette.
//

struct TintVignetteSpec {
	std::array<float, 3> inner = {};
	std::array<float, 3> outer = {};
	float innerSize = 0.18f; // Parts of the height of the picture.
	float outerSize = 0.18f;
	float softness = 0.6f;
	float stretch = 1.f; // Above one the oval is narrower.
};

// One tint fades out from the center, another one fades in towards the
// edges: the uneven color of a small lens over a small sensor.
void StageTintVignette(QImage &image, const TintVignetteSpec &spec) {
	const auto w = image.width();
	const auto h = image.height();
	const auto aspect = spec.stretch * w / float(std::max(h, 1));
	const auto softness = std::max(spec.softness, 0.01f);
	FxForEachColor(image, [&](FxRgba &c, int x, int y) {
		const auto dx = ((x + 0.5f) / w - 0.5f) * aspect;
		const auto dy = (y + 0.5f) / h - 0.5f;
		const auto distance = std::sqrt(dx * dx + dy * dy);
		const auto inner = 1.f - FxSmoothStep(
			spec.innerSize,
			spec.innerSize + softness,
			distance);
		const auto outer = FxSmoothStep(
			spec.outerSize,
			spec.outerSize + softness,
			distance);
		c.r += inner * spec.inner[0] + outer * spec.outer[0];
		c.g += inner * spec.inner[1] + outer * spec.outer[1];
		c.b += inner * spec.inner[2] + outer * spec.outer[2];
	});
}

//
// What the camera does with the picture.
//

// The plain 3 x 3 sharpening of a camera: the pixel against its four
// neighbours.
void StageSharpen3(QImage &image, float amount) {
	const auto source = image.copy();
	if (source.isNull()) {
		return;
	}
	const auto w = image.width();
	const auto h = image.height();
	FxParallelRows(w, h, [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto line = FxRow(image, y);
			const auto middle = FxRow(source, y);
			const auto above = FxRow(source, std::max(y - 1, 0));
			const auto below = FxRow(source, std::min(y + 1, h - 1));
			for (auto x = 0; x != w; ++x) {
				const auto p = middle[x];
				const auto a = int(p >> 24);
				if (!a) {
					continue;
				}
				const auto left = middle[std::max(x - 1, 0)];
				const auto right = middle[std::min(x + 1, w - 1)];
				auto result = uint32(a) << 24;
				for (auto shift = 0; shift != 24; shift += 8) {
					const auto value = int((p >> shift) & 0xFFU);
					const auto around = int((left >> shift) & 0xFFU)
						+ int((right >> shift) & 0xFFU)
						+ int((above[x] >> shift) & 0xFFU)
						+ int((below[x] >> shift) & 0xFFU);
					const auto sharp = value
						+ int(std::lround((4 * value - around) * amount));
					result |= uint32(std::clamp(sharp, 0, a)) << shift;
				}
				line[x] = result;
			}
		}
	});
}

struct ShotNoiseSpec {
	float amount = 0.f;
	bool mono = false;
	bool overlay = false;
	float shadows = 0.f; // One: nothing in the lights.
	uint32 seed = 0;
};

// The noise of the sensor, a new value for every pixel: added to the
// picture or laid over it in the "overlay" mode (then black and white
// stay clean and the middle tones get all of it).
void StageShotNoise(QImage &image, const ShotNoiseSpec &spec) {
	const auto green = spec.seed ^ 0x51ED270BU;
	const auto blue = spec.seed ^ 0xA3C59AC3U;
	FxForEachColor(image, [&](FxRgba &c, int x, int y) {
		const auto strength = spec.amount
			* FxMix(1.f, 1.f - Luma709(c.r, c.g, c.b), spec.shadows);
		const auto first = Uniform(x, y, spec.seed);
		const auto second = spec.mono ? first : Uniform(x, y, green);
		const auto third = spec.mono ? first : Uniform(x, y, blue);
		if (spec.overlay) {
			c.r = FxMix(c.r, Overlay(c.r, first), strength);
			c.g = FxMix(c.g, Overlay(c.g, second), strength);
			c.b = FxMix(c.b, Overlay(c.b, third), strength);
		} else {
			c.r += (first - 0.5f) * strength;
			c.g += (second - 0.5f) * strength;
			c.b += (third - 0.5f) * strength;
		}
	});
}

// Even areas of the middle tones are pulled towards a strongly blurred
// copy, edges and texture stay: the skin goes smooth, "soapy".
void StageSoap(QImage &image, float amount, float sigma) {
	auto blurred = image.copy();
	if (blurred.isNull()) {
		return;
	}
	FxGaussianBlur(blurred, sigma);
	const auto w = image.width();
	FxParallelRows(w, image.height(), [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto line = FxRow(image, y);
			const auto soft = FxRow(std::as_const(blurred), y);
			for (auto x = 0; x != w; ++x) {
				if (!(line[x] >> 24)) {
					continue;
				}
				auto c = FxUnpack(line[x]);
				const auto b = FxUnpack(soft[x]);
				const auto level = Luma709(c.r, c.g, c.b);
				const auto detail = level - Luma709(b.r, b.g, b.b);
				const auto k = amount
					* 4.f * level * (1.f - level)
					* (1.f - FxSmoothStep(0.f, 0.5f, std::abs(detail)));
				c.r -= k * (c.r - b.r);
				c.g -= k * (c.g - b.g);
				c.b -= k * (c.b - b.b);
				line[x] = FxPack(c);
			}
		}
	});
}

// The noise reduction of a camera: a pixel becomes the mean of the
// neighbours that look like it, so noise and JPEG blocks melt into flat
// spots and the edges stay.
void StageBilateral(QImage &image, float sigmaSpace, float sigmaLevel) {
	const auto source = image.copy();
	if (source.isNull()) {
		return;
	}
	const auto w = image.width();
	const auto h = image.height();
	const auto space = std::max(sigmaSpace, 0.1f);
	const auto level = std::max(sigmaLevel, 0.0005f);
	const auto radius = std::clamp(int(std::floor(space * 2.f)), 1, 8);
	const auto side = 2 * radius + 1;
	auto spatial = std::vector<float>(size_t(side) * side);
	for (auto j = -radius; j <= radius; ++j) {
		for (auto i = -radius; i <= radius; ++i) {
			spatial[size_t(j + radius) * side + (i + radius)] = std::exp(
				-(i * i + j * j) / (2.f * space * space));
		}
	}
	// The difference of two pixels is the difference of the lengths of
	// their colors, zero to the square root of three.
	constexpr auto kLongest = 1.7320508f;
	auto similar = std::array<float, kBilateralSteps>();
	for (auto i = 0; i != kBilateralSteps; ++i) {
		const auto d = i * (kLongest / (kBilateralSteps - 1));
		similar[i] = std::exp(-(d * d) / (2.f * level * level));
	}
	auto lengths = std::vector<float>(size_t(w) * h);
	for (auto y = 0; y != h; ++y) {
		const auto line = FxRow(source, y);
		const auto to = lengths.data() + size_t(y) * w;
		for (auto x = 0; x != w; ++x) {
			const auto p = line[x];
			const auto r = float((p >> 16) & 0xFFU);
			const auto g = float((p >> 8) & 0xFFU);
			const auto b = float(p & 0xFFU);
			to[x] = std::sqrt(r * r + g * g + b * b) * (1.f / 255.f);
		}
	}
	const auto toStep = (kBilateralSteps - 1) / kLongest;
	FxParallelRows(w, h, [&](int from, int till) {
		for (auto y = from; y != till; ++y) {
			const auto line = FxRow(image, y);
			const auto own = lengths.data() + size_t(y) * w;
			for (auto x = 0; x != w; ++x) {
				const auto a = (line[x] >> 24);
				if (!a) {
					continue;
				}
				const auto center = own[x];
				auto sum = std::array<float, 3>{};
				auto weights = 0.f;
				for (auto j = -radius; j <= radius; ++j) {
					const auto sy = std::clamp(y + j, 0, h - 1);
					const auto row = FxRow(source, sy);
					const auto rowLengths = lengths.data() + size_t(sy) * w;
					const auto rowSpatial = spatial.data()
						+ size_t(j + radius) * side
						+ radius;
					for (auto i = -radius; i <= radius; ++i) {
						const auto sx = std::clamp(x + i, 0, w - 1);
						const auto step = std::min(
							int(std::abs(rowLengths[sx] - center) * toStep),
							kBilateralSteps - 1);
						const auto weight = rowSpatial[i] * similar[step];
						const auto p = row[sx];
						sum[0] += float((p >> 16) & 0xFFU) * weight;
						sum[1] += float((p >> 8) & 0xFFU) * weight;
						sum[2] += float(p & 0xFFU) * weight;
						weights += weight;
					}
				}
				if (weights <= 0.f) {
					continue;
				}
				const auto channel = [&](float value) {
					return std::min(
						uint32(std::clamp(
							int(std::lround(value / weights)),
							0,
							255)),
						a);
				};
				line[x] = (a << 24)
					| (channel(sum[0]) << 16)
					| (channel(sum[1]) << 8)
					| channel(sum[2]);
			}
		}
	});
}

// A camera takes the photo down to its resolution, works there and gives
// a small file: the picture of the layer is made from it again.
struct Shot {
	QImage work;
	QSize rendered;
};

[[nodiscard]] bool BeginShot(
		Shot &shot,
		const QImage &image,
		const FxContext &context,
		int side) {
	shot.rendered = image.size();
	shot.work = FxResized(
		image,
		SizeWithLongSide(FullSize(image, context), side));
	return FxPrepare(shot.work);
}

[[nodiscard]] bool FinishShot(
		QImage &image,
		Shot &shot,
		bool pixels,
		const FxContext &context) {
	auto result = std::move(shot.work);
	const auto doubled = result.size() * 2;
	if (pixels
		&& std::max(doubled.width(), doubled.height()) <= kMaxDoubledSide) {
		// Every pixel becomes four: enlarged after that, the picture keeps
		// the little squares of a photo opened on a large screen.
		auto large = result.scaled(
			doubled,
			Qt::IgnoreAspectRatio,
			Qt::FastTransformation);
		if (!large.isNull()) {
			result = std::move(large);
		}
	}
	result = FxResized(result, shot.rendered);
	if (result.isNull()) {
		return false;
	}
	image = std::move(result);
	return !context.cancelled();
}

struct DevelopSpec {
	float sharpen = 0.f;
	ShotNoiseSpec noise;
	int quality = 70;
};

[[nodiscard]] DevelopSpec DevelopFrom(
		const FxParams &params,
		const FxContext &context,
		float shadows) {
	return {
		.sharpen = Percent(params, "sharpen"),
		.noise = {
			.amount = Percent(params, "noise"),
			.mono = params.boolean("noise_mono"),
			.overlay = (params.integer("noise_blend") == 1),
			.shadows = shadows,
			.seed = Seed(params, context, 0x0D161CA3U),
		},
		.quality = params.integer("quality"),
	};
}

// The end of every camera: sharpening, noise and the JPEG file.
[[nodiscard]] bool StageDevelop(
		QImage &work,
		const DevelopSpec &spec,
		const FxContext &context) {
	if (spec.sharpen > 0.f) {
		StageSharpen3(work, spec.sharpen);
	}
	if (spec.noise.amount > 0.f) {
		StageShotNoise(work, spec.noise);
	}
	if (context.cancelled()) {
		return false;
	}
	return LofiJpegRoundTrip(work, spec.quality, 1, context.cancel);
}

void StageFilter(QImage &image, Look look, float intensity) {
	if (intensity > 0.f) {
		StageLook(image, { .look = int(look), .intensity = intensity });
	}
}

[[nodiscard]] bool ApplyJpegCamera(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	auto shot = Shot();
	if (!BeginShot(shot, image, context, params.integer("side"))
		|| !StageDevelop(
			shot.work,
			DevelopFrom(params, context, 0.f),
			context)) {
		return false;
	}
	return FinishShot(image, shot, params.boolean("pixels"), context);
}

[[nodiscard]] bool ApplyCcd(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	auto shot = Shot();
	if (!BeginShot(shot, image, context, params.integer("side"))) {
		return false;
	}
	auto &work = shot.work;
	if (params.boolean("soften")) {
		FxGaussianBlur(work, 0.7);
	}
	StageFilter(work, Look::Ccd, Percent(params, "look"));
	if (context.cancelled()) {
		return false;
	}
	if (const auto bloom = Percent(params, "bloom"); bloom > 0.f) {
		StageGlow(work, {
			.amount = bloom,
			.tint = Channels(params.color("tint")),
			.gamma = 3.5f,
			.sigma = 18.f,
			.white = 1.f,
			.reduce = 6,
		});
	}
	const auto reflection = Percent(params, "reflection_strength");
	if (reflection > 0.f) {
		StageReflection(
			work,
			params.integer("reflection") - 1,
			reflection,
			DefaultReflectionColor());
	}
	if (context.cancelled()
		|| !StageDevelop(work, DevelopFrom(params, context, 0.f), context)) {
		return false;
	}
	return FinishShot(image, shot, params.boolean("pixels"), context);
}

[[nodiscard]] bool ApplyNokia(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	auto shot = Shot();
	if (!BeginShot(shot, image, context, params.integer("side"))) {
		return false;
	}
	auto &work = shot.work;
	StageFilter(work, Look::Nokia, Percent(params, "look"));
	if (const auto edges = Percent(params, "edges"); edges > 0.f) {
		// Red and blue fall off towards the corners, green stays.
		StageTintVignette(work, {
			.outer = { -0.1f * edges, 0.f, -0.1f * edges },
			.outerSize = 0.3f,
			.softness = 0.3f,
			.stretch = 1.2f,
		});
	}
	if (context.cancelled()
		|| !StageDevelop(work, DevelopFrom(params, context, 1.f), context)) {
		return false;
	}
	return FinishShot(image, shot, params.boolean("pixels"), context);
}

[[nodiscard]] bool ApplyWebcam(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	auto shot = Shot();
	if (!BeginShot(shot, image, context, params.integer("side"))) {
		return false;
	}
	auto &work = shot.work;
	StageFilter(work, Look::Webcam, Percent(params, "look"));
	if (const auto bloom = Percent(params, "bloom"); bloom > 0.f) {
		StageGlow(work, {
			.amount = bloom,
			.tint = Channels(params.color("tint")),
			.gamma = 3.f,
			.sigma = 16.f,
			.white = 1.f,
			.reduce = 4,
		});
	}
	if (context.cancelled()
		|| !StageDevelop(work, DevelopFrom(params, context, 0.f), context)) {
		return false;
	}
	if (const auto denoise = Percent(params, "denoise"); denoise > 0.f) {
		StageBilateral(work, 2.f, 0.1f * denoise);
	}
	return FinishShot(image, shot, params.boolean("pixels"), context);
}

[[nodiscard]] bool ApplyPhone(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	auto shot = Shot();
	if (!BeginShot(shot, image, context, params.integer("side"))) {
		return false;
	}
	auto &work = shot.work;
	// A haze over the whole frame, then one more from the lights alone.
	if (const auto bloom = Percent(params, "bloom"); bloom > 0.f) {
		StageGlow(work, {
			.amount = bloom,
			.gamma = 2.5f,
			.sigma = 106.f,
			.reduce = 15,
		});
	}
	if (const auto lights = Percent(params, "highlights"); lights > 0.f) {
		StageGlow(work, {
			.amount = lights,
			.gamma = 2.5f,
			.sigma = 103.f,
			.reduce = 15,
		});
	}
	if (context.cancelled()) {
		return false;
	}
	StageFilter(work, Look::Phone, Percent(params, "look"));
	// A tight glow around the lights, the lights themselves pushed up.
	StageGlow(work, {
		.amount = Percent(params, "bloom"),
		.gamma = 4.f,
		.sigma = 23.f,
		.white = 0.8f,
		.reduce = 4,
	});
	if (const auto grain = Percent(params, "grain"); grain > 0.f) {
		StageCoarseGrain(
			work,
			grain,
			0.7f,
			1.f,
			Seed(params, context, 0x1F0E36U));
	}
	return !context.cancelled() && FinishShot(image, shot, false, context);
}

[[nodiscard]] bool ApplySoap(
		QImage &image,
		const FxParams &params,
		const FxContext &context) {
	auto shot = Shot();
	if (!BeginShot(shot, image, context, params.integer("side"))) {
		return false;
	}
	auto &work = shot.work;
	StageGlow(work, {
		.amount = Percent(params, "bloom"),
		.gamma = 3.f,
		.sigma = 23.f,
		.white = 0.9f,
		.reduce = 4,
	});
	if (const auto smooth = Percent(params, "smooth"); smooth > 0.f) {
		StageSoap(work, smooth, 16.5f);
	}
	if (context.cancelled()) {
		return false;
	}
	// A warm center and greenish edges, before the filter and once more
	// after it.
	const auto vignette = Percent(params, "vignette");
	const auto tints = TintVignetteSpec{
		.inner = { 0.04f * vignette, -0.008f * vignette, -0.008f * vignette },
		.outer = { 0.02f * vignette, 0.04f * vignette, 0.01f * vignette },
		.innerSize = 0.18f,
		.outerSize = 0.18f,
		.softness = 0.6f,
	};
	if (vignette > 0.f) {
		StageTintVignette(work, tints);
	}
	StageFilter(work, Look::Soap, Percent(params, "look"));
	if (vignette > 0.f) {
		StageTintVignette(work, tints);
	}
	if (context.cancelled()
		|| !StageDevelop(work, DevelopFrom(params, context, 0.8f), context)) {
		return false;
	}
	return FinishShot(image, shot, params.boolean("pixels"), context);
}

//
// Registration.
//

[[nodiscard]] std::vector<FxText> LookNames() {
	return {
		tr::lng_oblivion_photo_digicam_look_nashville,
		tr::lng_oblivion_photo_digicam_look_chief_keef,
		tr::lng_oblivion_photo_digicam_look_nuke,
		tr::lng_oblivion_photo_digicam_look_phreshboy,
		tr::lng_oblivion_photo_digicam_look_sepia,
		tr::lng_oblivion_photo_digicam_look_2014,
		tr::lng_oblivion_photo_digicam_look_money,
		tr::lng_oblivion_photo_digicam_look_pandora,
		tr::lng_oblivion_photo_digicam_look_bleach,
		tr::lng_oblivion_photo_digicam_look_stressor,
		tr::lng_oblivion_photo_digicam_look_bw,
		tr::lng_oblivion_photo_digicam_look_bw_soft,
		tr::lng_oblivion_photo_digicam_look_desat,
		tr::lng_oblivion_photo_digicam_look_hsv,
		tr::lng_oblivion_photo_digicam_look_ccd,
		tr::lng_oblivion_photo_digicam_look_nokia,
		tr::lng_oblivion_photo_digicam_webcam,
		tr::lng_oblivion_photo_digicam_iphone,
		tr::lng_oblivion_photo_digicam_soap,
		tr::lng_oblivion_photo_digicam_look_calvin,
	};
}

struct CameraDefaults {
	int side = 640;
	int quality = 80;
	int noise = 0;
	int blend = 0;
	int sharpen = 0;
	bool pixels = false;
};

// The parameters every camera with a JPEG file has: StageDevelop() and
// FinishShot() read them.
void AppendCameraParams(
		std::vector<FxParam> &to,
		const CameraDefaults &defaults) {
	const auto noisy = [](const FxParams &params) {
		return params.integer("noise") > 0;
	};
	to.push_back(FxInt(
		"side",
		tr::lng_oblivion_photo_lofi_resolution,
		40,
		2048,
		defaults.side,
		u" px"_q).under(tr::lng_oblivion_photo_lofi_sec_sensor));
	to.push_back(FxBool(
		"pixels",
		tr::lng_oblivion_photo_digicam_pixels,
		defaults.pixels));
	to.push_back(FxInt(
		"noise",
		tr::lng_oblivion_photo_lofi_noise_luma,
		0,
		100,
		defaults.noise).under(tr::lng_oblivion_photo_lofi_sec_noise));
	to.push_back(FxBool(
		"noise_mono",
		tr::lng_oblivion_photo_digicam_noise_mono,
		false).when(noisy));
	to.push_back(FxChoice(
		"noise_blend",
		tr::lng_oblivion_photo_digicam_noise_blend,
		{
			tr::lng_oblivion_photo_digicam_blend_add,
			tr::lng_oblivion_photo_digicam_blend_overlay,
		},
		defaults.blend).when(noisy));
	to.push_back(FxSeed().when(noisy));
	to.push_back(FxInt(
		"sharpen",
		tr::lng_oblivion_photo_lofi_sharpen,
		0,
		100,
		defaults.sharpen).under(tr::lng_oblivion_photo_lofi_sec_processing));
	to.push_back(FxInt(
		"quality",
		tr::lng_oblivion_photo_lofi_quality,
		1,
		100,
		defaults.quality).under(tr::lng_oblivion_photo_lofi_sec_jpeg));
}

[[nodiscard]] FxParam LookAmountParam() {
	return FxInt(
		"look",
		tr::lng_oblivion_photo_digicam_intensity,
		0,
		100,
		100,
		u"%"_q);
}

[[nodiscard]] FxParam BloomParam(int value) {
	return FxInt(
		"bloom",
		tr::lng_oblivion_photo_lofi_glow,
		0,
		200,
		value,
		u"%"_q);
}

void RegisterCameras() {
	{
		auto params = std::vector<FxParam>();
		AppendCameraParams(params, { .side = 400, .quality = 50 });
		RegisterFx({
			.id = "digicam.jpeg",
			.group = FxGroup::Lofi,
			.name = tr::lng_oblivion_photo_digicam_jpeg,
			.params = std::move(params),
			.flags = kFxNeighbours | kFxSeeded,
			.order = 200,
			.apply = ApplyJpegCamera,
		});
	}
	{
		auto params = std::vector<FxParam>{
			LookAmountParam().under(tr::lng_oblivion_photo_lofi_sec_optics),
			FxBool("soften", tr::lng_oblivion_photo_digicam_soften, true),
			BloomParam(100),
			FxColor(
				"tint",
				tr::lng_oblivion_photo_digicam_tint,
				QColor(64, 0, 255)).when([](const FxParams &params) {
					return params.integer("bloom") > 0;
				}),
			FxInt(
				"reflection_strength",
				tr::lng_oblivion_photo_digicam_reflection,
				0,
				100,
				45,
				u"%"_q),
			FxInt(
				"reflection",
				tr::lng_oblivion_photo_digicam_reflection_variant,
				1,
				kDigicamReflectionCount,
				1).when([](const FxParams &params) {
					return params.integer("reflection_strength") > 0;
				}),
		};
		AppendCameraParams(params, {
			.side = 500,
			.quality = 70,
			.noise = 20,
			.blend = 1,
			.sharpen = 50,
			.pixels = true,
		});
		RegisterFx({
			.id = "digicam.ccd",
			.group = FxGroup::Lofi,
			.name = tr::lng_oblivion_photo_digicam_ccd,
			.params = std::move(params),
			.flags = kFxNeighbours | kFxSeeded,
			.order = 210,
			.apply = ApplyCcd,
		});
	}
	{
		auto params = std::vector<FxParam>{
			LookAmountParam().under(tr::lng_oblivion_photo_lofi_sec_optics),
			FxInt(
				"edges",
				tr::lng_oblivion_photo_digicam_edges,
				0,
				100,
				60,
				u"%"_q),
		};
		// The site asks for "pixels x2" here too, but switches it off for
		// every picture larger than 500 pixels, so it never happens.
		AppendCameraParams(params, {
			.side = 640,
			.quality = 80,
			.noise = 25,
			.sharpen = 30,
		});
		RegisterFx({
			.id = "digicam.nokia",
			.group = FxGroup::Lofi,
			.name = tr::lng_oblivion_photo_digicam_nokia,
			.params = std::move(params),
			.flags = kFxNeighbours | kFxSeeded,
			.order = 220,
			.apply = ApplyNokia,
		});
	}
	{
		auto params = std::vector<FxParam>{
			LookAmountParam().under(tr::lng_oblivion_photo_lofi_sec_optics),
			BloomParam(100),
			FxColor(
				"tint",
				tr::lng_oblivion_photo_digicam_tint,
				QColor(0, 0, 255)).when([](const FxParams &params) {
					return params.integer("bloom") > 0;
				}),
			FxInt(
				"denoise",
				tr::lng_oblivion_photo_digicam_denoise,
				0,
				100,
				100,
				u"%"_q),
		};
		AppendCameraParams(params, {
			.side = 640,
			.quality = 80,
			.noise = 30,
		});
		RegisterFx({
			.id = "digicam.webcam",
			.group = FxGroup::Lofi,
			.name = tr::lng_oblivion_photo_digicam_webcam,
			.params = std::move(params),
			.flags = kFxNeighbours | kFxSeeded,
			.order = 230,
			.apply = ApplyWebcam,
		});
	}
	RegisterFx({
		.id = "digicam.iphone",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_digicam_iphone,
		.params = {
			LookAmountParam().under(tr::lng_oblivion_photo_lofi_sec_optics),
			BloomParam(100),
			FxInt(
				"highlights",
				tr::lng_oblivion_photo_digicam_highlight_bloom,
				0,
				100,
				40,
				u"%"_q),
			FxInt(
				"grain",
				tr::lng_oblivion_photo_digicam_grain,
				0,
				100,
				40).under(tr::lng_oblivion_photo_lofi_sec_noise),
			FxSeed().when([](const FxParams &params) {
				return params.integer("grain") > 0;
			}),
			FxInt(
				"side",
				tr::lng_oblivion_photo_lofi_resolution,
				320,
				2048,
				kNominalSide,
				u" px"_q).under(tr::lng_oblivion_photo_lofi_sec_sensor),
		},
		.flags = kFxNeighbours | kFxSeeded,
		.order = 240,
		.apply = ApplyPhone,
	});
	{
		auto params = std::vector<FxParam>{
			LookAmountParam().under(tr::lng_oblivion_photo_lofi_sec_optics),
			BloomParam(30),
			FxInt(
				"smooth",
				tr::lng_oblivion_photo_digicam_smooth,
				0,
				100,
				20,
				u"%"_q),
			FxInt(
				"vignette",
				tr::lng_oblivion_photo_digicam_vignette,
				0,
				100,
				100,
				u"%"_q),
		};
		AppendCameraParams(params, {
			.side = 600,
			.quality = 80,
			.noise = 25,
			.blend = 1,
		});
		RegisterFx({
			.id = "digicam.soap",
			.group = FxGroup::Lofi,
			.name = tr::lng_oblivion_photo_digicam_soap,
			.params = std::move(params),
			.flags = kFxNeighbours | kFxSeeded,
			.order = 250,
			.apply = ApplySoap,
		});
	}
}

void RegisterParts() {
	RegisterFx({
		.id = "digicam.look",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_digicam_look,
		.params = {
			FxChoice(
				"look",
				tr::lng_oblivion_photo_digicam_filter,
				LookNames()),
			FxInt(
				"intensity",
				tr::lng_oblivion_photo_digicam_intensity,
				0,
				100,
				100,
				u"%"_q),
			FxInt(
				"vignette",
				tr::lng_oblivion_photo_digicam_vignette_amount,
				0,
				100,
				100,
				u"%"_q),
			FxInt("grain", tr::lng_oblivion_photo_digicam_grain, 0, 100, 35),
			FxSeed().when([](const FxParams &params) {
				return params.integer("grain") > 0;
			}),
		},
		.flags = kFxSeeded,
		.order = 260,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			const auto full = FullSize(image, context);
			const auto nominal = std::clamp(
				std::max(full.width(), full.height()),
				1,
				kNominalSide);
			StageLook(image, {
				.look = params.integer("look"),
				.intensity = Percent(params, "intensity"),
				.vignette = Percent(params, "vignette"),
				.grain = Percent(params, "grain"),
				.cell = std::max(image.width(), image.height())
					/ float(nominal),
				.seed = Seed(params, context, 0x100C2016U),
			});
			return !context.cancelled();
		},
		.identity = [](const FxParams &params) {
			return params.integer("intensity") <= 0
				&& params.integer("vignette") <= 0
				&& params.integer("grain") <= 0;
		},
	});
	RegisterFx({
		.id = "digicam.glow",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_digicam_glow,
		.params = {
			FxInt(
				"amount",
				tr::lng_oblivion_photo_lofi_amount,
				0,
				200,
				100,
				u"%"_q),
			FxColor(
				"tint",
				tr::lng_oblivion_photo_digicam_tint,
				QColor(64, 0, 255)),
			FxFloat(
				"highlights",
				tr::lng_oblivion_photo_digicam_highlights,
				1.,
				6.,
				3.5,
				1).stepped(0.1),
			FxPixels(
				"radius",
				tr::lng_oblivion_photo_lofi_radius,
				1.,
				300.,
				40.),
			FxInt(
				"white",
				tr::lng_oblivion_photo_digicam_white,
				50,
				100,
				100,
				u"%"_q),
		},
		.flags = kFxNeighbours,
		.order = 270,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			StageGlow(image, {
				.amount = Percent(params, "amount"),
				.tint = Channels(params.color("tint")),
				.gamma = float(params.number("highlights")),
				.sigma = std::max(
					float(context.px(params.number("radius"))),
					0.3f),
				.white = Percent(params, "white"),
			});
			return !context.cancelled();
		},
		.identity = [](const FxParams &params) {
			return params.integer("amount") <= 0
				&& params.integer("white") >= 100;
		},
	});
	RegisterFx({
		.id = "digicam.reflection",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_digicam_reflection,
		// The effect alone starts with a reflection that is seen at once
		// on any photo: the wide spot of the third shape. The first shape
		// at 45% (what the CCD camera has) is two faint bands, on a bright
		// picture the effect looked like it did nothing.
		.params = {
			FxInt(
				"variant",
				tr::lng_oblivion_photo_digicam_reflection_variant,
				1,
				kDigicamReflectionCount,
				3),
			FxInt(
				"strength",
				tr::lng_oblivion_photo_lofi_amount,
				0,
				100,
				60,
				u"%"_q),
			FxColor(
				"color",
				tr::lng_oblivion_photo_lofi_stamp_color,
				DefaultReflectionColor()),
		},
		.order = 280,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			StageReflection(
				image,
				params.integer("variant") - 1,
				Percent(params, "strength"),
				params.color("color"));
			return !context.cancelled();
		},
		.identity = [](const FxParams &params) {
			return params.integer("strength") <= 0;
		},
	});
	RegisterFx({
		.id = "digicam.vignette",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_digicam_vignette,
		// The colors are the ones of the "washed" camera, three times as
		// strong: at its 20 the tints are a part of a look, alone they
		// could not be told from the original photo.
		.params = {
			FxColor(
				"inner",
				tr::lng_oblivion_photo_digicam_inner,
				QColor(255, 102, 102)),
			FxInt(
				"inner_amount",
				tr::lng_oblivion_photo_digicam_inner_amount,
				0,
				100,
				60),
			FxColor(
				"outer",
				tr::lng_oblivion_photo_digicam_outer,
				QColor(191, 255, 159)),
			FxInt(
				"outer_amount",
				tr::lng_oblivion_photo_digicam_outer_amount,
				0,
				100,
				60),
			FxInt(
				"size",
				tr::lng_oblivion_photo_lofi_size,
				0,
				100,
				18,
				u"%"_q),
			FxInt(
				"softness",
				tr::lng_oblivion_photo_digicam_softness,
				1,
				100,
				60,
				u"%"_q),
		},
		.order = 290,
		.apply = [](
				QImage &image,
				const FxParams &params,
				const FxContext &context) {
			const auto size = Percent(params, "size");
			StageTintVignette(image, {
				.inner = TintOffsets(
					params.color("inner"),
					Percent(params, "inner_amount")),
				.outer = TintOffsets(
					params.color("outer"),
					Percent(params, "outer_amount")),
				.innerSize = size,
				.outerSize = size,
				.softness = Percent(params, "softness"),
			});
			return !context.cancelled();
		},
		.identity = [](const FxParams &params) {
			return params.integer("inner_amount") <= 0
				&& params.integer("outer_amount") <= 0;
		},
	});
}

// The three "quality" buttons of the JPEG camera.
void RegisterPresets() {
	const auto whole = [](int value) {
		return FxValue::Integer(value);
	};
	RegisterFxPreset({
		.id = "digicam.preset_jpeg_low",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_digicam_preset_jpeg_low,
		.stack = {
			MakeFx("digicam.jpeg", {
				{ "side", whole(250) },
				{ "pixels", FxValue::Boolean(true) },
				{ "noise", whole(20) },
				{ "noise_blend", whole(1) },
				{ "sharpen", whole(20) },
				{ "quality", whole(60) },
			}),
		},
		.order = 20,
	});
	RegisterFxPreset({
		.id = "digicam.preset_jpeg_medium",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_digicam_preset_jpeg_medium,
		.stack = {
			MakeFx("digicam.jpeg", {
				{ "side", whole(400) },
				{ "quality", whole(50) },
			}),
		},
		.order = 21,
	});
	RegisterFxPreset({
		.id = "digicam.preset_jpeg_high",
		.group = FxGroup::Lofi,
		.name = tr::lng_oblivion_photo_digicam_preset_jpeg_high,
		.stack = {
			MakeFx("digicam.jpeg", {
				{ "side", whole(650) },
				{ "noise", whole(25) },
				{ "noise_blend", whole(1) },
				{ "quality", whole(40) },
			}),
		},
		.order = 22,
	});
}

const auto Registered = FxRegistrar([] {
	RegisterCameras();
	RegisterParts();
	RegisterPresets();
});

//
// Self-test.
//

[[nodiscard]] bool SamePixels(const QImage &a, const QImage &b) {
	if (a.size() != b.size() || a.format() != b.format()) {
		return false;
	}
	for (auto y = 0; y != a.height(); ++y) {
		if (std::memcmp(
				a.constScanLine(y),
				b.constScanLine(y),
				size_t(a.width()) * 4) != 0) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool ValidPixels(const QImage &image) {
	if (image.format() != QImage::Format_ARGB32_Premultiplied) {
		return false;
	}
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

[[nodiscard]] bool SameAlpha(const QImage &a, const QImage &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (auto y = 0; y != a.height(); ++y) {
		const auto one = FxRow(a, y);
		const auto two = FxRow(b, y);
		for (auto x = 0; x != a.width(); ++x) {
			if ((one[x] >> 24) != (two[x] >> 24)) {
				return false;
			}
		}
	}
	return true;
}

// The mean color of a part of a picture, 0 .. 1.
[[nodiscard]] FxRgba MeanColor(const QImage &image, QRect rect) {
	auto sum = std::array<double, 3>{};
	auto count = 0;
	rect = rect.intersected(image.rect());
	for (auto y = rect.top(); y <= rect.bottom(); ++y) {
		const auto line = FxRow(image, y);
		for (auto x = rect.left(); x <= rect.right(); ++x) {
			const auto c = FxUnpack(line[x]);
			sum[0] += c.r;
			sum[1] += c.g;
			sum[2] += c.b;
			++count;
		}
	}
	const auto k = count ? (1. / count) : 0.;
	return { float(sum[0] * k), float(sum[1] * k), float(sum[2] * k), 1.f };
}

[[nodiscard]] bool RunDigicamSelfTest(QStringList &log) {
	auto ok = true;
	const auto check = [&](bool condition, const QString &what) {
		log.push_back((condition ? u"OK: "_q : u"FAIL: "_q) + what);
		ok = ok && condition;
	};
	const auto info = [&](const QString &what) {
		log.push_back(u"   "_q + what);
	};
	const auto full = FxTestImage(640, 480);
	const auto cut = FxTestImage(320, 240, true);
	const auto context = FxContext{
		.scale = 1.,
		.seed = 4242,
		.fullSize = full.size(),
	};
	const auto run = [&](
			const FxInstance &instance,
			const QImage &source,
			FxContext used) {
		auto result = source;
		used.fullSize = source.size();
		return ApplyFx(result, instance, used) ? result : QImage();
	};
	const auto whole = [](int value) {
		return FxValue::Integer(value);
	};
	const auto ids = std::vector<QByteArray>{
		"digicam.jpeg",
		"digicam.ccd",
		"digicam.nokia",
		"digicam.webcam",
		"digicam.iphone",
		"digicam.soap",
		"digicam.look",
		"digicam.glow",
		"digicam.reflection",
		"digicam.vignette",
	};

	// Every effect: registered, changes the picture, keeps valid pixels
	// and the alpha channel of a picture with transparency, does the same
	// thing twice.
	{
		auto registered = true;
		auto changes = true;
		auto valid = true;
		auto repeats = true;
		auto timer = QElapsedTimer();
		auto timings = QStringList();
		for (const auto &id : ids) {
			const auto name = QString::fromLatin1(id);
			const auto descriptor = FindFx(id);
			if (!descriptor || descriptor->group != FxGroup::Lofi) {
				registered = false;
				info(name + u" is not registered"_q);
				continue;
			}
			const auto instance = MakeFx(id);
			timer.start();
			const auto first = run(instance, full, context);
			timings.push_back(u"%1 %2"_q.arg(
				name.mid(8),
				QString::number(timer.nsecsElapsed() / 1e6, 'f', 1)));
			if (first.isNull() || FxImageDifference(first, full) < 0.05) {
				changes = false;
				info(name + u" does not change the picture"_q);
			}
			if (!SamePixels(first, run(instance, full, context))) {
				repeats = false;
				info(name + u" is not deterministic"_q);
			}
			const auto transparent = run(instance, cut, context);
			if (transparent.isNull()
				|| transparent.size() != cut.size()
				|| !ValidPixels(transparent)) {
				valid = false;
				info(name + u" breaks premultiplied pixels"_q);
			}
		}
		check(registered, u"%1 digicam effects are registered"_q.arg(
			ids.size()));
		check(changes, u"every effect changes the picture by default"_q);
		check(repeats, u"every effect is deterministic"_q);
		check(valid, u"every effect keeps valid premultiplied pixels"_q);
		info(u"640x480, ms: "_q + timings.join(u", "_q));

		// The effects that work at the size of the layer never touch the
		// alpha channel.
		auto alpha = true;
		for (const auto id : {
			"digicam.look",
			"digicam.glow",
			"digicam.reflection",
			"digicam.vignette",
		}) {
			alpha = alpha && SameAlpha(run(MakeFx(id), cut, context), cut);
		}
		check(alpha, u"filters, bloom, reflection and vignette keep alpha"_q);
	}

	// Seeds.
	{
		auto differ = true;
		auto same = true;
		for (const auto id : {
			"digicam.jpeg",
			"digicam.ccd",
			"digicam.nokia",
			"digicam.iphone",
			"digicam.look",
		}) {
			const auto one = MakeFx(id, {
				{ "seed", whole(1) },
				{ "noise", whole(30) },
			});
			const auto two = MakeFx(id, {
				{ "seed", whole(2) },
				{ "noise", whole(30) },
			});
			const auto first = run(one, full, context);
			differ = differ && !SamePixels(first, run(two, full, context));
			same = same && SamePixels(first, run(one, full, context));
			auto other = context;
			other.seed = 99;
			differ = differ && !SamePixels(first, run(one, full, other));
		}
		check(same, u"the same seed gives the same picture"_q);
		check(differ, u"another seed or instance gives another picture"_q);
	}

	// Zero strength.
	{
		const auto none = std::vector<FxInstance>{
			MakeFx("digicam.look", {
				{ "intensity", whole(0) },
				{ "vignette", whole(0) },
				{ "grain", whole(0) },
			}),
			MakeFx("digicam.glow", { { "amount", whole(0) } }),
			MakeFx("digicam.reflection", { { "strength", whole(0) } }),
			MakeFx("digicam.vignette", {
				{ "inner_amount", whole(0) },
				{ "outer_amount", whole(0) },
			}),
		};
		auto identity = true;
		for (const auto &instance : none) {
			auto image = full;
			const auto fine = FxIsIdentity(instance)
				&& ApplyFx(image, instance, context)
				&& (image.cacheKey() == full.cacheKey());
			if (!fine) {
				identity = false;
				info(QString::fromLatin1(instance.id)
					+ u" is not an identity at zero"_q);
			}
		}
		check(identity, u"zero strength changes nothing (%1 effects)"_q.arg(
			none.size()));
	}

	// The filters as formulas.
	{
		const auto look = [](Look which, FxRgba color, float intensity = 1.f) {
			return DigicamLookColor(int(which), color, intensity);
		};
		const auto black = FxRgba{ 0.f, 0.f, 0.f, 1.f };
		const auto white = FxRgba{ 1.f, 1.f, 1.f, 1.f };
		const auto brick = FxRgba{ 0.8f, 0.2f, 0.1f, 0.5f };
		const auto gray = [](FxRgba c, float tolerance) {
			return std::abs(c.r - c.g) <= tolerance
				&& std::abs(c.g - c.b) <= tolerance;
		};
		auto inside = true;
		auto changing = true;
		auto untouched = true;
		auto halfway = true;
		for (auto i = 0; i != kDigicamLookCount; ++i) {
			auto largest = 0.f;
			for (auto step = 0; step != 125; ++step) {
				const auto color = FxRgba{
					(step % 5) / 4.f,
					((step / 5) % 5) / 4.f,
					(step / 25) / 4.f,
					1.f,
				};
				const auto result = DigicamLookColor(i, color, 1.f);
				inside = inside
					&& (result.r >= 0.f && result.r <= 1.f)
					&& (result.g >= 0.f && result.g <= 1.f)
					&& (result.b >= 0.f && result.b <= 1.f)
					&& (result.a == color.a);
				largest = std::max({
					largest,
					std::abs(result.r - color.r),
					std::abs(result.g - color.g),
					std::abs(result.b - color.b),
				});
				const auto none = DigicamLookColor(i, color, 0.f);
				untouched = untouched
					&& (none.r == color.r)
					&& (none.g == color.g)
					&& (none.b == color.b);
				const auto half = DigicamLookColor(i, color, 0.5f);
				halfway = halfway
					&& (std::abs(half.r - (color.r + result.r) / 2.f) < 0.001f)
					&& (std::abs(half.g - (color.g + result.g) / 2.f) < 0.001f)
					&& (std::abs(half.b - (color.b + result.b) / 2.f) < 0.001f);
			}
			if (largest < 0.04f) {
				changing = false;
				info(u"filter %1 changes nothing"_q.arg(i));
			}
		}
		check(inside, u"%1 filters give colors in the range, alpha kept"_q.arg(
			kDigicamLookCount));
		check(changing, u"every filter changes the colors"_q);
		check(untouched, u"a filter at zero intensity changes nothing"_q);
		check(halfway, u"half intensity is half the way"_q);

		const auto nashville = look(Look::Nashville, black);
		const auto ccd = look(Look::Ccd, black);
		const auto soft = look(Look::MonoSoft, black);
		const auto softWhite = look(Look::MonoSoft, white);
		const auto sepia = look(Look::Sepia, { 0.5f, 0.5f, 0.5f, 1.f });
		check(
			nashville.b > 0.3f && nashville.r < 0.08f,
			u"Nashville has blue shadows (%1 %2 %3)"_q
				.arg(nashville.r, 0, 'f', 3)
				.arg(nashville.g, 0, 'f', 3)
				.arg(nashville.b, 0, 'f', 3));
		check(
			ccd.r > 0.05f && ccd.r < 0.17f && gray(ccd, 0.03f),
			u"CCD lifts the black (%1)"_q.arg(ccd.g, 0, 'f', 3));
		check(
			gray(look(Look::Mono, brick), 0.02f)
				&& gray(look(Look::MonoSoft, brick), 0.02f)
				&& soft.g > 0.08f
				&& softWhite.g < 0.92f
				&& softWhite.g > soft.g + 0.5f,
			u"black and white filters are gray, the soft one is faded "
			"(%1 .. %2)"_q
				.arg(soft.g, 0, 'f', 3)
				.arg(softWhite.g, 0, 'f', 3));
		check(
			sepia.r > sepia.g && sepia.g > sepia.b + 0.08f,
			u"sepia is brown"_q);
		const auto plain = look(Look::Desaturate, brick);
		const auto hsv = look(Look::HsvDesaturate, brick);
		check(
			gray(plain, 0.0001f)
				&& std::abs(plain.g - Luma709(0.8f, 0.2f, 0.1f)) < 0.0001f
				&& gray(hsv, 0.0001f)
				&& std::abs(hsv.g - 0.8f) < 0.0001f,
			u"desaturation goes to the brightness, the HSV one to the "
			"largest channel"_q);
		const auto outOfRange = DigicamLookColor(99, brick, 1.f);
		const auto last = look(Look::Calvin, brick);
		check(
			outOfRange.r == last.r
				&& outOfRange.g == last.g
				&& outOfRange.b == last.b,
			u"a filter index out of the range is clamped"_q);
	}

	// Parts of the looks, where they can be told apart.
	{
		const auto flat = [](QColor color) {
			auto result = QImage(320, 240, QImage::Format_ARGB32_Premultiplied);
			result.fill(color);
			return result;
		};
		const auto center = QRect(140, 100, 40, 40);
		const auto corner = QRect(0, 0, 24, 24);
		const auto mid = flat(QColor(128, 128, 128));

		// The vignette of the filters darkens the corners only.
		const auto framed = run(MakeFx("digicam.look", {
			{ "intensity", whole(0) },
			{ "grain", whole(0) },
		}), mid, context);
		const auto framedCenter = MeanColor(framed, center);
		const auto framedCorner = MeanColor(framed, corner);
		check(
			std::abs(framedCenter.g - 0.5f) < 0.02f
				&& framedCorner.g < 0.3f,
			u"the filter vignette darkens the corners (%1 against %2)"_q
				.arg(framedCorner.g, 0, 'f', 3)
				.arg(framedCenter.g, 0, 'f', 3));

		// The color vignette: red and blue leave the corners with a dark
		// green tint, the default one warms the center.
		const auto greenEdges = run(MakeFx("digicam.vignette", {
			{ "inner_amount", whole(0) },
			{ "outer", FxValue::Color(QColor(0, 128, 0)) },
			{ "outer_amount", whole(50) },
			{ "size", whole(20) },
			{ "softness", whole(30) },
		}), mid, context);
		const auto edge = MeanColor(greenEdges, corner);
		const auto middle = MeanColor(greenEdges, center);
		const auto warm = MeanColor(
			run(MakeFx("digicam.vignette"), mid, context),
			center);
		check(
			edge.g > edge.r + 0.05f
				&& edge.g > edge.b + 0.05f
				&& std::abs(edge.g - 0.5f) < 0.02f
				&& std::abs(middle.r - 0.5f) < 0.02f
				&& warm.r > warm.g + 0.02f,
			u"the color vignette tints the edges and the center"_q);

		// The bloom: a light spot gets a colored haze, a dark picture
		// stays as it is.
		auto night = flat(QColor(20, 20, 24));
		for (auto y = 100; y != 140; ++y) {
			for (auto x = 140; x != 180; ++x) {
				FxRow(night, y)[x] = 0xFFFFFFFFU;
			}
		}
		const auto glowing = run(MakeFx("digicam.glow", {
			{ "radius", FxValue::Number(12.) },
		}), night, context);
		const auto beside = MeanColor(glowing, QRect(184, 110, 8, 20));
		const auto away = MeanColor(glowing, QRect(0, 0, 40, 40));
		const auto before = MeanColor(night, QRect(184, 110, 8, 20));
		check(
			beside.b > before.b + 0.1f
				&& beside.b > beside.g + 0.08f
				&& std::abs(away.b - before.b) < 0.01f,
			u"the bloom is a violet haze around a light (%1 %2 %3)"_q
				.arg(beside.r, 0, 'f', 3)
				.arg(beside.g, 0, 'f', 3)
				.arg(beside.b, 0, 'f', 3));
		const auto dark = flat(QColor(30, 30, 30));
		check(
			FxImageDifference(
				run(MakeFx("digicam.glow"), dark, context),
				dark) < 0.5,
			u"the bloom leaves a dark picture alone"_q);

		// The reflection only adds light, every variant is a different one.
		const auto dusk = flat(QColor(40, 40, 40));
		auto lighter = true;
		auto different = true;
		auto faintest = 1.f;
		auto previous = QImage();
		for (auto i = 1; i <= kDigicamReflectionCount; ++i) {
			const auto image = run(MakeFx("digicam.reflection", {
				{ "variant", whole(i) },
				{ "strength", whole(100) },
			}), dusk, context);
			if (image.isNull()) {
				lighter = false;
				break;
			}
			const auto mean = MeanColor(image, image.rect());
			faintest = std::min(faintest, mean.b);
			lighter = lighter && (mean.b > mean.g + 0.02f);
			for (auto y = 0; lighter && y < image.height(); y += 7) {
				const auto line = FxRow(image, y);
				for (auto x = 0; x < image.width(); x += 7) {
					if ((line[x] & 0xFFU) < 40U) {
						lighter = false;
						break;
					}
				}
			}
			different = different
				&& (previous.isNull()
					|| FxImageDifference(image, previous) > 1.);
			previous = image;
		}
		check(
			lighter && faintest > 0.185f,
			u"a reflection only adds violet light (the faintest gives "
			"%1)"_q.arg(faintest, 0, 'f', 3));
		check(different, u"the reflections differ"_q);

		// Noise laid over leaves black and white clean, added noise
		// does not.
		const auto noisy = [&](const QImage &source, int blend) {
			return FxImageDifference(run(MakeFx("digicam.jpeg", {
				{ "side", whole(2048) },
				{ "noise", whole(60) },
				{ "noise_blend", whole(blend) },
				{ "quality", whole(100) },
			}), source, context), source);
		};
		const auto blackPicture = flat(QColor(0, 0, 0));
		const auto overlaidBlack = noisy(blackPicture, 1);
		const auto addedBlack = noisy(blackPicture, 0);
		const auto overlaidMid = noisy(mid, 1);
		if (LofiJpegAvailable()) {
			check(
				overlaidBlack < 1. && addedBlack > 3. && overlaidMid > 3.,
				u"overlaid noise keeps the black clean (%1), added noise "
				"does not (%2), the middle gets it (%3)"_q
					.arg(overlaidBlack, 0, 'f', 2)
					.arg(addedBlack, 0, 'f', 2)
					.arg(overlaidMid, 0, 'f', 2));
		}

		// Pixels x2: the picture is made of 2 x 2 squares of the sensor.
		const auto squares = run(MakeFx("digicam.jpeg", {
			{ "side", whole(160) },
			{ "pixels", FxValue::Boolean(true) },
			{ "quality", whole(95) },
		}), FxTestImage(320, 240), context);
		auto pairs = 0;
		auto equal = 0;
		for (auto y = 0; y < squares.height(); ++y) {
			const auto line = FxRow(squares, y);
			for (auto x = 0; x + 1 < squares.width(); ++x) {
				++pairs;
				equal += (line[x] == line[x + 1]) ? 1 : 0;
			}
		}
		// Every other pair of neighbours is inside one doubled pixel.
		check(
			pairs > 0 && equal * 2 >= pairs,
			u"pixels x2 doubles every pixel of the sensor (%1 of %2 "
			"neighbours are equal)"_q.arg(equal).arg(pairs));
	}

	// Presets only set parameters.
	{
		auto count = 0;
		auto fine = true;
		for (const auto preset : AllFxPresets()) {
			if (!preset->id.startsWith("digicam.")) {
				continue;
			}
			++count;
			fine = fine
				&& (preset->group == FxGroup::Lofi)
				&& !preset->stack.empty()
				&& (preset->stack.size() <= 6);
			for (const auto &instance : preset->stack) {
				const auto descriptor = FindFx(instance.id);
				if (!descriptor
					|| !instance.enabled
					|| instance.uid
					|| !(NormalizedFxParams(*descriptor, instance.params)
						== instance.params)) {
					fine = false;
					info(u"preset %1: bad instance %2"_q.arg(
						QString::fromLatin1(preset->id),
						QString::fromLatin1(instance.id)));
				}
			}
			auto image = full;
			if (!ApplyFxStack(image, preset->stack, context)
				|| FxImageDifference(image, full) < 0.5) {
				fine = false;
				info(u"preset %1 does not change the picture"_q.arg(
					QString::fromLatin1(preset->id)));
			}
		}
		check(
			count == 3 && fine,
			u"%1 presets are valid parameter sets of registered effects"_q.arg(
				count));
	}

	// The cameras work at their own resolution and the rest measures in
	// source pixels, so the preview is the export, only smaller.
	{
		const auto large = FxTestImage(1600, 1200);
		const auto half = FxResized(large, large.size() / 2);
		const auto compare = [&](const FxInstance &instance) {
			auto exported = large;
			auto preview = half;
			const auto done = ApplyFx(exported, instance, FxContext{
				.scale = 1.,
				.seed = 7,
				.fullSize = large.size(),
			}) && ApplyFx(preview, instance, FxContext{
				.scale = 0.5,
				.seed = 7,
				.fullSize = large.size(),
				.preview = true,
			});
			return done
				? FxImageDifference(preview, FxResized(exported, half.size()))
				: 255.;
		};
		auto cameras = true;
		auto numbers = QStringList();
		for (const auto id : {
			"digicam.jpeg",
			"digicam.ccd",
			"digicam.nokia",
			"digicam.webcam",
			"digicam.iphone",
			"digicam.soap",
		}) {
			const auto difference = compare(MakeFx(id));
			cameras = cameras && (difference < 6.);
			numbers.push_back(u"%1 %2"_q.arg(
				QString::fromLatin1(id).mid(8),
				QString::number(difference, 'f', 2)));
		}
		const auto filter = compare(MakeFx("digicam.look", {
			{ "grain", whole(0) },
		}));
		const auto glow = compare(MakeFx("digicam.glow"));
		const auto reflection = compare(MakeFx("digicam.reflection"));
		const auto vignette = compare(MakeFx("digicam.vignette"));
		check(
			cameras,
			u"the preview of a camera matches the export ("_q
				+ numbers.join(u", "_q)
				+ u")"_q);
		check(
			filter < 2. && glow < 3. && reflection < 2. && vignette < 2.,
			u"the preview of the parts matches the export (filter %1, "
			"bloom %2, reflection %3, vignette %4)"_q
				.arg(filter, 0, 'f', 2)
				.arg(glow, 0, 'f', 2)
				.arg(reflection, 0, 'f', 2)
				.arg(vignette, 0, 'f', 2));
	}

	// Cancelling.
	{
		const auto cancel = std::atomic<bool>(true);
		auto stopped = true;
		for (const auto &id : ids) {
			auto image = full;
			stopped = stopped && !ApplyFx(image, MakeFx(id), FxContext{
				.fullSize = full.size(),
				.cancel = &cancel,
			});
		}
		check(stopped, u"a cancelled effect reports it"_q);
	}

	// Timings on a larger picture.
	{
		const auto large = FxTestImage(2048, 1536);
		auto timer = QElapsedTimer();
		auto timings = QStringList();
		for (const auto &id : ids) {
			auto image = large;
			timer.start();
			const auto done = ApplyFx(image, MakeFx(id), FxContext{
				.scale = 1.,
				.seed = 1,
				.fullSize = large.size(),
			});
			timings.push_back(u"%1 %2%3"_q.arg(
				QString::fromLatin1(id).mid(8),
				QString::number(timer.nsecsElapsed() / 1e6, 'f', 0),
				done ? QString() : u" (failed)"_q));
		}
		info(u"2048x1536, ms: "_q + timings.join(u", "_q));
	}
	return ok;
}

const auto DigicamSelfTest = SelfTestRegistrar(
	SelfTestSuite::Fx,
	"digicam",
	&RunDigicamSelfTest);

} // namespace

FxRgba DigicamLookColor(int look, FxRgba color, float intensity) {
	LookFilter(look, intensity).apply(color);
	return color;
}

} // namespace Oblivion::Photo
