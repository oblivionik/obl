/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "oblivion/oblivion_photo_fx.h"

#include <array>

// Photo editor: the Lightroom-style adjustments as layer effects.
//
// Effects registered by oblivion_photo_fx_adjust.cpp (FxGroup in brackets):
//
//   adjust.light     [Light]   exposure (EV, linear light with a soft
//                              shoulder), contrast, highlights, shadows
//                              (both local: they follow an edge-aware base
//                              of the luminance, so the local contrast
//                              stays), whites, blacks.
//   adjust.curve     [Light]   tone curve: a master curve and one per
//                              channel, monotone cubic through the points
//                              (custom parameter "curve", type
//                              kAdjustCurveType).
//   adjust.color     [Color]   temperature (a shift along the Planckian
//                              locus, gains in linear light), tint,
//                              vibrance and saturation (Oklab chroma).
//   adjust.hsl       [Color]   hue / saturation / luminance for eight
//                              color ranges (custom parameter "table",
//                              type kAdjustHslType).
//   adjust.grading   [Color]   color grading: a tint for the shadows, the
//                              midtones and the highlights (custom
//                              parameter "wheels", type kAdjustWheelsType),
//                              a luminance for each, blending, balance.
//   adjust.bw        [Color]   black and white with a mix of eight colors.
//   adjust.presence  [Detail]  texture, clarity (edge-aware local
//                              contrast, no halos at strong edges), dehaze
//                              (dark channel prior, refined transmission).
//   adjust.sharpen   [Detail]  amount, radius (source pixels), masking.
//   adjust.denoise   [Detail]  luminance and color noise reduction (guided
//                              filters, edges are kept).
//   adjust.vignette  [Finish]  amount, midpoint, roundness, feather.
//   adjust.grain     [Finish]  amount, size, roughness, a seed.
//
// Everything is deterministic and scale independent: the sizes are
// relative to the layer (or given in source pixels and scaled with
// FxContext::scale), so a downscaled preview looks like the export.
// Neutral values change nothing: the effects report it through
// FxDescriptor::identity and are skipped.
//
// The three custom parameters are stored as short canonical ASCII texts
// (an empty value is the neutral one), the models below parse and write
// them. The widgets that edit them are registered by
// oblivion_photo_fx_adjust_ui.cpp.
namespace Oblivion::Photo {

inline constexpr auto kAdjustCurveType = "adjust.curve";
inline constexpr auto kAdjustHslType = "adjust.hsl";
inline constexpr auto kAdjustWheelsType = "adjust.wheels";

//
// Tone curve.
//

inline constexpr auto kCurveChannels = 4; // Master, red, green, blue.
inline constexpr auto kCurveUnit = 1000; // Coordinates are 0..kCurveUnit.
inline constexpr auto kCurveMaxPoints = 16; // Per channel.
inline constexpr auto kCurveMinGap = 10; // Between the x of two points.

struct CurvePoint {
	int x = 0; // Input, 0..kCurveUnit.
	int y = 0; // Output, 0..kCurveUnit.

	friend bool operator==(const CurvePoint &a, const CurvePoint &b) = default;
};

using CurvePoints = std::vector<CurvePoint>;

struct ToneCurve {
	// Always valid: every channel has 2..kCurveMaxPoints points sorted by
	// x with at least kCurveMinGap between them. Left of the first point
	// and right of the last one the curve is flat.
	std::array<CurvePoints, kCurveChannels> channels;

	ToneCurve();

	// The channel / the whole curve maps every value to itself.
	[[nodiscard]] bool identity(int channel) const;
	[[nodiscard]] bool identity() const;

	friend bool operator==(const ToneCurve &a, const ToneCurve &b) = default;
};

// (0, 0) - (kCurveUnit, kCurveUnit).
[[nodiscard]] CurvePoints DefaultCurvePoints();
// Clamps, sorts, removes points that are too close to the previous one,
// cuts to kCurveMaxPoints; less than two points give the default.
[[nodiscard]] CurvePoints NormalizedCurvePoints(CurvePoints points);

// "m=0,0;250,180;1000,1000/b=0,0;1000,900": only the channels that are
// not the default two points, an empty array for the default curve.
// Parsing is tolerant: anything broken gives the default for that channel.
[[nodiscard]] QByteArray SerializeToneCurve(const ToneCurve &curve);
[[nodiscard]] ToneCurve ParseToneCurve(const QByteArray &data);
[[nodiscard]] QByteArray NormalizeToneCurve(const QByteArray &data);

// The value of the curve at x (both 0..1): a monotone cubic (PCHIP)
// through the points, it never leaves the range of two neighbour points.
[[nodiscard]] double CurveValue(const CurvePoints &points, double x);
// table[i] = CurveValue(points, i / (size - 1)), size >= 2.
void FillCurveTable(const CurvePoints &points, float *table, int size);

//
// Color ranges.
//

inline constexpr auto kHslRanges = 8;

enum class HslRange : uchar {
	Red,
	Orange,
	Yellow,
	Green,
	Aqua,
	Blue,
	Purple,
	Magenta,
};

struct HslTable {
	// -100..100 for every range.
	std::array<int, kHslRanges> hue = {};
	std::array<int, kHslRanges> saturation = {};
	std::array<int, kHslRanges> luminance = {};

	[[nodiscard]] bool identity() const;

	friend bool operator==(const HslTable &a, const HslTable &b) = default;
};

// "h=0,0,20,0,0,0,0,0/s=0,0,0,0,0,-40,0,0": only the rows with a value,
// an empty array for the neutral table.
[[nodiscard]] QByteArray SerializeHslTable(const HslTable &table);
[[nodiscard]] HslTable ParseHslTable(const QByteArray &data);
[[nodiscard]] QByteArray NormalizeHslTable(const QByteArray &data);

// The color that shows a range in the interface.
[[nodiscard]] QColor HslRangeColor(int range);

//
// Color grading wheels.
//

inline constexpr auto kGradeRanges = 3; // Shadows, midtones, highlights.

struct GradeWheel {
	int hue = 0; // 0..359, as in HSV: 0 red, 120 green, 240 blue.
	int saturation = 0; // 0..100, the strength of the tint.

	friend bool operator==(const GradeWheel &a, const GradeWheel &b) = default;
};

struct GradeWheels {
	std::array<GradeWheel, kGradeRanges> ranges;

	[[nodiscard]] bool identity() const;

	friend bool operator==(
		const GradeWheels &a,
		const GradeWheels &b) = default;
};

// "s=210,35/h=40,20": only the ranges with a saturation (the hue of
// a range without one is 0), an empty array for no tint at all.
[[nodiscard]] QByteArray SerializeGradeWheels(const GradeWheels &wheels);
[[nodiscard]] GradeWheels ParseGradeWheels(const QByteArray &data);
[[nodiscard]] QByteArray NormalizeGradeWheels(const QByteArray &data);

// What a wheel shows at this hue and saturation.
[[nodiscard]] QColor GradeWheelColor(int hue, int saturation);

} // namespace Oblivion::Photo
