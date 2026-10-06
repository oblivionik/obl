/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "oblivion/oblivion_photo_doc.h"

// Photo editor: the maths of the layer tools (pure, no UI, any thread).
//
//  - the transform of a layer as the transform tool changes it: move,
//    scale by a handle (around the opposite handle or the center, with or
//    without the proportions), rotate, flip, a free corner (perspective),
//    a skewed edge, fit / fill / center, the numbers shown in the fields;
//  - hit testing: the handles of the transform frame and "which layer is
//    under the cursor" that looks at the pixels (a click goes through the
//    transparent parts of a layer);
//  - snapping of a moved rectangle or point to the canvas edges and
//    center and to the other layers;
//  - the brush of the layer mask: a stroke is collected in a coverage
//    plane and mixed into the mask, so one stroke never gets darker where
//    its segments overlap;
//  - the rows of the layers list (the top layer is the first row).
//
// The tools and the panels themselves are in oblivion_photo_layers_ui.cpp.
// Coordinates are the ones of oblivion_photo_doc.h: "local" is the layer
// content, "canvas" the document. Angles are degrees, positive is
// clockwise on the screen.
namespace Oblivion::Photo {

//
// The transform frame.
//

enum class TransformHandle : uchar {
	None,
	Move, // Inside of the frame.
	TopLeft,
	Top,
	TopRight,
	Right,
	BottomRight,
	Bottom,
	BottomLeft,
	Left,
	Rotate, // The round handle, see PlaceRotateHandle().
};

[[nodiscard]] bool IsCornerHandle(TransformHandle handle);
[[nodiscard]] bool IsEdgeHandle(TransformHandle handle);
// The index of the corner in the LayerQuad() order, -1 for other handles.
[[nodiscard]] int HandleCorner(TransformHandle handle);
[[nodiscard]] TransformHandle CornerHandle(int corner);

// The corners of a content of this size: top left, top right, bottom
// right, bottom left.
[[nodiscard]] QPolygonF ContentQuad(QSizeF content);
// The same corners on the canvas (mapped one by one, so there are always
// four of them, whatever the perspective is).
[[nodiscard]] QPolygonF TransformedQuad(
	const QTransform &transform,
	QSizeF content);
// Where the handle is on the content (the center for None / Move).
[[nodiscard]] QPointF HandleLocalPoint(TransformHandle handle, QSizeF content);
// What stays in place while the handle is dragged: the opposite corner or
// edge middle, or the center of the content.
[[nodiscard]] QPointF HandleAnchorPoint(
	TransformHandle handle,
	QSizeF content,
	bool aroundCenter);

struct HandleMetrics {
	double radius = 9.; // The distance a handle is caught from.
	double rotateOffset = 26.; // Of the rotate handle from the top edge.
	bool edges = true; // false: only the corners (the perspective mode).
	bool rotate = true;
	// What of the frame coordinates is on the screen (the canvas widget),
	// empty: everything. The rotate handle is kept inside of it.
	QRectF view;
};

// Everything below works with a frame given by its four corners in the
// LayerQuad() order, in any coordinates (the tools use widget pixels, so
// the handles keep their size on the screen).
//
// Edge handles are left out while an edge is too short for them.
[[nodiscard]] bool EdgeHandlesFit(const QPolygonF &quad, double radius);
[[nodiscard]] QPointF HandlePoint(const QPolygonF &quad, TransformHandle handle);
[[nodiscard]] QPointF RotateHandlePoint(const QPolygonF &quad, double offset);
// The shortest of the four sides of the frame.
[[nodiscard]] double ShortestSide(const QPolygonF &quad);

struct RotateHandlePlace {
	QPointF handle;
	QPointF edge; // The middle of the edge the handle hangs on.
};
// Where the rotate handle is: above the top edge while that place is in
// HandleMetrics::view, otherwise inside of the frame under the top edge
// (a layer that fills the view has nothing above it), otherwise under the
// bottom edge. The same place is painted and hit.
[[nodiscard]] RotateHandlePlace PlaceRotateHandle(
	const QPolygonF &quad,
	const HandleMetrics &metrics);
// A press inside of a frame that is small on the screen catches a corner
// only right at it, so the rest of the frame still moves the layer; from
// the outside the corners are caught from the whole radius.
[[nodiscard]] TransformHandle HitTestTransform(
	const QPolygonF &quad,
	QPointF point,
	const HandleMetrics &metrics);

//
// Changing the transform. All of them return a new "layer -> canvas"
// transform made from the one the drag started with.
//

[[nodiscard]] QTransform MovedTransform(const QTransform &start, QPointF delta);
// Only the longer of the two axes (Shift while moving).
[[nodiscard]] QPointF ConstrainedDelta(QPointF delta);

// Scales the content around a point given in layer coordinates.
[[nodiscard]] QTransform LocalScaled(
	const QTransform &start,
	QPointF anchor,
	double scaleX,
	double scaleY);

struct SnapTargets {
	std::vector<double> xs; // Vertical lines, canvas coordinates.
	std::vector<double> ys; // Horizontal lines.
};

struct SnapResult {
	QPointF delta; // What to add to get to the lines.
	std::optional<double> guideX; // The lines that were caught.
	std::optional<double> guideY;
};

struct ScaleArgs {
	TransformHandle handle = TransformHandle::None;
	QPointF point; // Where the handle is dragged to, canvas coordinates.
	bool keepAspect = false;
	bool aroundCenter = false;
	// The smallest side the content may get, canvas pixels.
	double minSize = 1.;
	// Optional: the dragged edges of a layer that is not rotated stick to
	// these lines when they come closer than the threshold.
	const SnapTargets *targets = nullptr;
	double threshold = 0.;
};
// Dragging the handle across the anchor mirrors the layer. snap, if
// given, gets the lines that were caught.
[[nodiscard]] QTransform ScaledTransform(
	const QTransform &start,
	QSizeF content,
	const ScaleArgs &args,
	SnapResult *snap = nullptr);

// The angle the vector pivot -> from must be turned by to point at to,
// in (-180, 180].
[[nodiscard]] double AngleBetween(QPointF pivot, QPointF from, QPointF to);
[[nodiscard]] double NormalizedAngle(double degrees); // To (-180, 180].
[[nodiscard]] double SnappedAngle(double degrees, double step);
// Turns the layer around a canvas point.
[[nodiscard]] QTransform RotatedTransform(
	const QTransform &start,
	QPointF pivot,
	double degrees);
// Mirrors the layer as it is seen on the canvas, its center stays.
[[nodiscard]] QTransform FlippedTransform(
	const QTransform &start,
	QSizeF content,
	bool horizontal);
// The same as it is seen on the screen, see ViewOrientation().
[[nodiscard]] QTransform FlippedTransform(
	const QTransform &start,
	QSizeF content,
	bool horizontal,
	const QTransform &orientation);
// One corner (the LayerQuad() order) goes to the canvas point, the three
// others stay: a free perspective. nullopt if the frame would fold.
[[nodiscard]] std::optional<QTransform> CornerMovedTransform(
	const QTransform &start,
	QSizeF content,
	int corner,
	QPointF point);
// An edge slides along itself by the projection of delta: a skew.
[[nodiscard]] std::optional<QTransform> EdgeSkewedTransform(
	const QTransform &start,
	QSizeF content,
	TransformHandle edge,
	QPointF delta);
// Scales the layer as it is (rotated, mirrored...) so that its bounds fit
// into the canvas (fill: cover it) and centers it.
[[nodiscard]] QTransform FittedTransform(
	const QTransform &start,
	QSizeF content,
	QSizeF canvas,
	bool fill);
[[nodiscard]] QTransform CenteredTransform(
	const QTransform &start,
	QSizeF content,
	QSizeF canvas);
// What "Reset" gives: the place a new layer of this size gets.
[[nodiscard]] QTransform DefaultTransform(QSize content, QSize canvas);

// Not rotated (or turned by 90 degrees), not skewed, no perspective.
[[nodiscard]] bool IsAxisAligned(const QTransform &transform);
// Something a layer can live with: finite, invertible, the whole content
// in front of the perspective horizon, a sane size on the canvas.
[[nodiscard]] bool ValidTransform(const QTransform &transform, QSizeF content);
// Canvas pixels per layer pixel around a layer point (the square root of
// the area scale, so it works for a perspective too).
[[nodiscard]] double LocalScaleAt(const QTransform &transform, QPointF local);

//
// Snapping.
//

// The edges and the center of the canvas and of the other visible layers.
[[nodiscard]] SnapTargets MakeSnapTargets(
	const Document &document,
	LayerId except);
// The left / center / right and top / center / bottom of the rectangle
// against the lines.
[[nodiscard]] SnapResult SnapRect(
	const QRectF &bounds,
	const SnapTargets &targets,
	double threshold,
	bool horizontal = true,
	bool vertical = true);
[[nodiscard]] SnapResult SnapPoint(
	QPointF point,
	const SnapTargets &targets,
	double threshold,
	bool horizontal = true,
	bool vertical = true);

//
// The canvas as it is seen. The whole picture can be turned, mirrored and
// straightened (Document::global), so "left" on the screen is not always
// "less x" on the canvas. What is done with a key, a button or a typed
// number goes through the orientation of the view (a drag does not need
// it: the cursor already is where the layer is seen).
//

// The turn and the mirror of a "canvas -> screen" transform alone, no
// zoom and no shift. Identity for a transform that has none of them.
[[nodiscard]] QTransform ViewOrientation(const QTransform &view);
// The same with the turn rounded to quarters: which canvas axis is seen
// as the horizontal one and where it points.
[[nodiscard]] QTransform QuarterOrientation(const QTransform &orientation);
// The canvas shift that moves a layer by step in a screen direction (one
// of the four, y down). A step is never less than a pixel of the screen
// (viewScale: screen pixels per canvas pixel) and is a whole number of
// canvas pixels unless the picture is straightened.
[[nodiscard]] QPointF SeenStep(
	const QTransform &orientation,
	QPointF direction,
	double step,
	double viewScale);
// The canvas angle that turns a layer by degrees clockwise on the screen.
[[nodiscard]] double SeenTurn(const QTransform &orientation, double degrees);
// A canvas point as its shift from the canvas center on the screen (x to
// the right, y down, canvas pixels), and back.
[[nodiscard]] QPointF SeenShift(
	const QTransform &orientation,
	QPointF point,
	QSizeF canvas);
[[nodiscard]] QPointF PointFromSeenShift(
	const QTransform &orientation,
	QPointF shift,
	QSizeF canvas);

//
// The numbers of the transform fields.
//

struct LinkedPercents {
	double changed = 100.;
	double other = 100.;
};
// The width and the height (percents) after one of them was set to value
// while they are linked: both change by the same ratio, and the ratio
// stops where the other one would leave from .. till, so the proportions
// are not lost at the limits.
[[nodiscard]] LinkedPercents LinkPercents(
	double changed,
	double other,
	double value,
	double from,
	double till);

struct TransformNumbers {
	double x = 0.; // The center of the layer on the canvas, pixels.
	double y = 0.;
	double width = 100.; // Percents of the content size, positive.
	double height = 100.;
	double rotation = 0.; // (-180, 180].
	double skew = 0.; // Degrees.
	bool flippedX = false; // A mirrored layer is shown as one flip.
	bool flippedY = false;
};
// The perspective part of the transform, if any, is dropped.
[[nodiscard]] TransformNumbers NumbersFromTransform(
	const QTransform &transform,
	QSizeF content);
[[nodiscard]] QTransform TransformFromNumbers(
	const TransformNumbers &numbers,
	QSizeF content);

struct LayerFlips {
	bool x = false;
	bool y = false;
};
// The numbers with the rotation set to degrees. A mirrored layer is told
// as the one of its two flips that needs the smaller turn, and which one
// that is changes when the turn passes a quarter: taking the flips anew
// on every step of a dragged slider would throw the layer half a turn
// back and forth there. flips are the ones the previous step ended with
// (nullopt for the first step: those of the numbers).
[[nodiscard]] TransformNumbers WithRotation(
	const TransformNumbers &numbers,
	double degrees,
	std::optional<LayerFlips> flips = std::nullopt);

//
// Which layer is under the cursor.
//

// The opacity of the layer pixels as one byte per pixel (Format_Alpha8),
// made from any picture of the layer in layer coordinates (a thumbnail).
[[nodiscard]] QImage AlphaMapFromPixels(const QImage &pixels);
// Is there something visible around the layer point? A null map says yes.
[[nodiscard]] bool AlphaMapHit(
	const QImage &map,
	QSizeF content,
	QPointF local,
	int threshold = 16);
// The topmost visible layer that has pixels at the point. alphaMap gives
// the map of a layer or null if it is not known (the frame is used then).
[[nodiscard]] LayerId LayerAtPixels(
	const Document &document,
	QPointF canvasPoint,
	const Fn<const QImage*(LayerId)> &alphaMap,
	bool withLocked = false);

//
// The layers list: the first row is the top layer.
//

// -1 for a row that is not there.
[[nodiscard]] int LayerRowToIndex(int count, int row);
[[nodiscard]] int LayerIndexToRow(int count, int index);
// A row dragged from fromRow and dropped into the gap above the row gap
// (count: under the last one): the index for MoveLayer(), -1 if the layer
// stays where it is.
[[nodiscard]] int LayerDropIndex(int count, int fromRow, int gap);
// Changes when the picture of the layer alone (content, effects, mask)
// changes, not when it is moved, renamed, hidden or blended differently.
[[nodiscard]] uint64 LayerThumbnailKey(const Layer &layer);
// There is a layer under this one and none of the two is locked.
[[nodiscard]] bool CanMergeDown(const Document &document, LayerId id);

//
// The mask brush.
//

// A layer point -> a pixel position in a mask of this size.
[[nodiscard]] QPointF MaskPoint(QSizeF content, QSize mask, QPointF local);
// Mask pixels per layer pixel.
[[nodiscard]] double MaskScale(QSizeF content, QSize mask);

// Adds a brush segment to the coverage plane of a stroke (Format_Alpha8
// of the mask size, 0: untouched, 255: fully painted): a round brush
// moving from one point to another, the radius changes linearly between
// them. hardness 0..1 is the part of the radius that is fully covered.
// Returns the changed rectangle (empty if nothing was touched).
QRect MaskStampSegment(
	QImage &coverage,
	QPointF from,
	QPointF to,
	double radiusFrom,
	double radiusTo,
	double hardness);
// mask = base moved towards target (0 hides, 255 shows) by the coverage
// and the opacity 0..1, inside of rect.
void MaskComposeStroke(
	QImage &mask,
	const QImage &base,
	const QImage &coverage,
	QRect rect,
	int target,
	double opacity);
[[nodiscard]] QImage InvertedMask(const QImage &mask);
// The picture of the hidden parts for the "show the mask" overlay: the
// color with the opacity of (255 - mask), premultiplied. rect limits the
// update of an overlay made before (it must have the size of the mask).
void UpdateMaskOverlay(
	QImage &overlay,
	const QImage &mask,
	QRect rect,
	QColor color);

} // namespace Oblivion::Photo
