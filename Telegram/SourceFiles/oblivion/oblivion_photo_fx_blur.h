/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "oblivion/oblivion_photo_fx.h"

// Photo editor: the configurable blurs of the layer effects registry
// (oblivion_photo_fx.h), group FxGroup::Blur:
//
//   blur.gaussian    radius
//   blur.box         radius
//   blur.motion      angle, distance
//   blur.spin        center, angle (a rotation around the center)
//   blur.zoom        center, amount (streaks from the center)
//   blur.tilt_shift  radius, a sharp band: center, angle, width, falloff
//   blur.lens        radius, bokeh shape and rotation, highlights
//   blur.surface     radius, threshold (keeps the edges)
//
// Every one except the tilt-shift has an optional region: the whole layer,
// a linear gradient or a radial one. Inside a gradient the blur grows
// smoothly from nothing to the full strength: the image is blurred with
// a few strengths and every pixel is mixed from the two nearest ones, so
// a half-way pixel is really blurred by half and not a ghost of the sharp
// picture over the blurred one.
//
// The effects register themselves, nothing has to be called. The raster
// functions below are what they are made of, for other effects that need
// a blur as a building block (bloom, halation, a blurred collage
// background). All of them work in place on any image (it is converted to
// Format_ARGB32_Premultiplied), repeat the edge pixels outside of the
// image, cost about the same for any radius and may be called from any
// thread. Lengths are in pixels of the given image: multiply source pixel
// lengths with FxContext::px() first.
namespace Oblivion::Photo {

// A box blur: every pixel becomes the mean of the (2 * radiusX + 1) x
// (2 * radiusY + 1) pixels around it. The radii may be fractional (the
// outermost pixels then count partly), zero skips that direction.
void FxBoxBlur(QImage &image, double radiusX, double radiusY);

// The picture smeared along a straight line: angle in degrees (0 is
// horizontal, positive is clockwise on the screen), distance is the full
// length of the smear. False: cancelled or out of memory.
[[nodiscard]] bool FxMotionBlur(
	QImage &image,
	double angle,
	double distance,
	const std::atomic<bool> *cancel = nullptr);

// The picture smeared along circles around center (pixel-index
// coordinates) by angle degrees in total.
[[nodiscard]] bool FxSpinBlur(
	QImage &image,
	QPointF center,
	double angle,
	const std::atomic<bool> *cancel = nullptr);

// The picture smeared along the rays from center: amount is the length
// of the smear as a part of the distance to the center (0.2 is a streak
// of 20 px for a pixel that is 100 px away).
[[nodiscard]] bool FxZoomBlur(
	QImage &image,
	QPointF center,
	double amount,
	const std::atomic<bool> *cancel = nullptr);

enum class FxBokehShape : uchar {
	Disc,
	Pentagon,
	Hexagon,
	Octagon,
};

struct FxLensBlurArgs {
	double radius = 0.;
	FxBokehShape shape = FxBokehShape::Disc;
	double rotation = 0.; // Degrees, for the shapes with corners.
	// 0..1: how much brighter the pixels above the threshold (0..1 of the
	// full brightness) are counted, they become the bokeh discs.
	double highlights = 0.;
	double threshold = 1.;
};

// An out-of-focus lens: every pixel becomes a disc (or a polygon of the
// aperture blades), mixed in linear light.
[[nodiscard]] bool FxLensBlur(
	QImage &image,
	const FxLensBlurArgs &args,
	const std::atomic<bool> *cancel = nullptr);

// An edge preserving blur: flat areas (where the picture changes by less
// than about threshold, 0..1 of the full range) are smoothed with the
// radius, strong edges stay sharp.
[[nodiscard]] bool FxSurfaceBlur(
	QImage &image,
	double radius,
	double threshold,
	const std::atomic<bool> *cancel = nullptr);

} // namespace Oblivion::Photo
