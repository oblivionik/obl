/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "oblivion/oblivion_photo_fx.h"

#include <QtGui/QPolygonF>

// Photo editor: the distortions of the layer effects registry
// (oblivion_photo_fx.h), group FxGroup::Distort:
//
//   distort.keystone  vertical and horizontal perspective tilt, scale
//   distort.corners   free perspective: the four corners are dragged on
//                     the canvas
//   distort.skew      horizontal and vertical slant
//   distort.lens      barrel / pincushion, scale
//   distort.bulge     bulge / pinch: center, radius, strength
//   distort.twirl     center, radius, angle
//   distort.wave      amplitude, wavelength, direction, phase, shape
//   distort.ripple    rings from a center: amplitude, wavelength, decay
//   distort.mesh      a mesh warp: the grid points are dragged on the
//                     canvas
//
// Every one has the "edges" choice: what is shown where the distortion
// looks outside of the picture (FxEdges).
//
// The effects register themselves, nothing has to be called. Below is
// what they are made of, for other modules: all distortions go through
// FxRemap(), which for every pixel of the result asks where in the source
// it comes from, so there are never holes, and takes several samples
// where the picture is squeezed, so there is no sparkle.
//
// The perspective maths is the one of the document: FxWarpPerspective()
// uses QuadTransform() of oblivion_photo_doc.h, exactly what the transform
// of a layer with four free corners does on the canvas.
namespace Oblivion::Photo {

enum class FxEdges : uchar {
	Clear, // Nothing outside of the picture: transparent pixels.
	Stretch, // The edge pixels go on forever.
	Mirror, // The picture is reflected at its edges.
};

// Fills the source coordinates for the pixels 0 .. count - 1 of the row
// y of the result: pixel-index coordinates, pixel centers are at whole
// numbers. It is also asked for the row and the column right after the
// picture (y == height, count == width + 1), from several threads at
// once. A coordinate that is not a finite number gives a transparent
// pixel.
using FxRowMap = Fn<void(float y, int count, float *sx, float *sy)>;

// Resamples the picture in place (it keeps its size, the format becomes
// Format_ARGB32_Premultiplied). False: cancelled or out of memory.
[[nodiscard]] bool FxRemap(
	QImage &image,
	FxEdges edges,
	const FxRowMap &map,
	const std::atomic<bool> *cancel = nullptr);

// Moves the corners of the picture to the corners of the quad (top left,
// top right, bottom right, bottom left, in pixels of the picture, the
// rectangle of the picture is (0, 0) .. (width, height)). A quad that is
// not convex changes nothing.
[[nodiscard]] bool FxWarpPerspective(
	QImage &image,
	const QPolygonF &quad,
	FxEdges edges,
	const std::atomic<bool> *cancel = nullptr);

// A grid of control points for the mesh warp: columns x rows points, row
// by row, each one says where the point of the regular grid has moved,
// in coordinates normalized to the picture ((0, 0) is the top left
// corner, (1, 1) the bottom right one). The picture between the points
// follows them smoothly (a Catmull-Rom surface).
struct FxMesh {
	int columns = 0;
	int rows = 0;
	std::vector<QPointF> points;

	[[nodiscard]] bool valid() const;

	friend bool operator==(const FxMesh &a, const FxMesh &b) = default;
};

inline constexpr auto kFxMeshMinSide = 2;
inline constexpr auto kFxMeshMaxSide = 9;

// A grid that changes nothing.
[[nodiscard]] FxMesh FxMeshRegular(int columns, int rows);
// "4x4:0,0;0.33333,0;...": what the "mesh" parameter of distort.mesh and
// the "corners" parameter of distort.corners (a 2x2 grid) store. Parsing
// anything else gives a mesh that is not valid().
[[nodiscard]] QByteArray FxMeshSerialize(const FxMesh &mesh);
[[nodiscard]] FxMesh FxMeshParse(const QByteArray &data);
[[nodiscard]] bool FxMeshIsRegular(const FxMesh &mesh);
// Where the point (u, v) of the picture goes, normalized coordinates.
[[nodiscard]] QPointF FxMeshPoint(const FxMesh &mesh, double u, double v);
// The same warp described by another number of points (approximately).
[[nodiscard]] FxMesh FxMeshResampled(
	const FxMesh &mesh,
	int columns,
	int rows);

[[nodiscard]] bool FxWarpMesh(
	QImage &image,
	const FxMesh &mesh,
	FxEdges edges,
	const std::atomic<bool> *cancel = nullptr);

} // namespace Oblivion::Photo
