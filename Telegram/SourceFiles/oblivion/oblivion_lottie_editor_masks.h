/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "oblivion/oblivion_lottie_doc.h"

// Lottie editor: what the panels share for the After Effects features
// (masks, track mattes, gradients, dashes, rounded corners, parenting and
// the pen tool for path points).
//
//  - The canvas tool (selection / pen) of an editor: one value per
//    EditorController, observed by the toolbar, the canvas and the
//    inspector ("Edit points" switches to the pen).
//  - The geometry of the pen tool: which part of a path is under the
//    cursor, moving vertices and bezier handles, adding a vertex to an end
//    of an open path. Pure functions over PathData, the canvas only maps
//    the mouse to them and turns the results into undoable edits.
//  - Small document helpers the UI needs and the model does not have: the
//    path property of a node, the paths that can be picked inside a
//    selection, a rectangle / an ellipse turned into a path.
//
// Everything here except the texts is free of styles, lang and widgets and
// is covered by RunEditorSelfTest() (OBLIVION_SELFTEST=lottie_editor).
namespace Oblivion::LottieEdit {

enum class CanvasTool : uchar {
	Select, // Click selects, drag moves (the default).
	Pen, // Path points and bezier handles of shapes and masks.
};

// Main thread. The state lives as long as the controller.
[[nodiscard]] CanvasTool CurrentTool(not_null<EditorController*> controller);
void SetCurrentTool(not_null<EditorController*> controller, CanvasTool tool);
[[nodiscard]] rpl::producer<CanvasTool> CurrentToolValue(
	not_null<EditorController*> controller);

// { shape, "ks" } for a path item, { mask, "pt" } for a mask, invalid for
// anything else (rectangles, ellipses and stars have no points).
[[nodiscard]] PropertyRef PathPropertyOf(const Document &document, NodeId id);

// Path items and masks the pen can pick while the node is selected: the
// node itself, the masks of a layer, the path items inside a shape layer /
// a group (depth first, hidden ones skipped), the siblings of a fill or
// another modifier. At most limit of them.
[[nodiscard]] std::vector<NodeId> PathCandidates(
	const Document &document,
	NodeId id,
	int limit = 48);

// The shape layer / group a new path item goes into while the node is
// selected, 0 if there is none (a precomposition layer, nothing selected).
[[nodiscard]] NodeId PathContainerFor(const Document &document, NodeId id);

// Where two clicks of the pen start a new path while the node is selected:
// PathContainerFor(), but 0 for a rectangle / an ellipse. The pen asks to
// convert such a shape to a path first, so its clicks pick like the
// selection tool and leave no stray path item next to the shape.
[[nodiscard]] NodeId NewPathContainerFor(
	const Document &document,
	NodeId id);

// The path with its vertices and tangents mapped (tangents as vectors).
[[nodiscard]] PathData MappedPath(
	const PathData &path,
	const QTransform &transform);

enum class PathPart : uchar {
	None,
	Vertex,
	InTangent, // The handle before the vertex.
	OutTangent, // The handle after the vertex.
	Segment,
};

struct PathPick {
	PathPart part = PathPart::None;
	int index = -1; // Vertex / the vertex of the handle / segment.
	double t = 0.; // Segment: the position inside it.
	QPointF point; // In path coordinates.
	double distance = 0.; // In view pixels.

	explicit operator bool() const {
		return (part != PathPart::None);
	}
};

// What of the path is within radius (view pixels) of the point (view
// coordinates), toView maps path coordinates to the view. The handles of
// the selected vertex win over vertices, vertices over segments.
[[nodiscard]] PathPick PickPathPart(
	const PathData &path,
	const QTransform &toView,
	QPointF point,
	double radius,
	int selected = -1);

// Both handles lie on one line through the vertex, on its two sides.
[[nodiscard]] bool IsSmoothVertex(const PathData &path, int index);

// The tangents move with their vertex.
[[nodiscard]] PathData WithMovedVertex(
	const PathData &path,
	int index,
	QPointF position);

enum class TangentMode : uchar {
	Free, // Only the dragged handle.
	Aligned, // The other one turns to stay on the same line, keeps its length.
	Mirrored, // The other one is the opposite of the dragged one.
};

// tangent is relative to the vertex, like in PathData.
[[nodiscard]] PathData WithMovedTangent(
	const PathData &path,
	int index,
	bool out,
	QPointF tangent,
	TangentMode mode);

// Corner <-> smooth.
[[nodiscard]] PathData WithToggledVertex(const PathData &path, int index);

// A new corner vertex after the last (or before the first) vertex of an
// open path, in every keyframe (so that all of them keep the same number
// of vertices), at the given position at the frame and at the old end in
// the other keyframes. Fails for a closed path.
[[nodiscard]] Edit AppendPathVertex(
	const Document &document,
	const PropertyRef &path,
	QPointF position,
	bool atStart,
	double frame);

// Moves a vertex / a handle of the path value at the frame (a keyframe
// of an animated path, like SetValueAt()).
[[nodiscard]] Edit SetPathAt(
	const Document &document,
	const PropertyRef &path,
	const PathData &value,
	double frame);

// A rectangle or an ellipse as a path item with the same outline at the
// frame, in the same place of the list (the pen works on paths only).
// Animated size / position / roundness are taken at the frame.
[[nodiscard]] bool CanConvertToPath(const Document &document, NodeId shape);
[[nodiscard]] Edit ConvertToPath(
	const Document &document,
	NodeId shape,
	double frame);

// The masks of the layer are switched off ("hasMask" is not true) although
// one of them is meant to cut: the layers Validate() reports as
// IssueType::MasksOff. A layer with nothing but "none" masks keeps them
// off on purpose (switched on, such a layer is not drawn at all), so there
// is nothing to offer for it.
[[nodiscard]] bool LayerMasksSwitchedOff(
	const Document &document,
	NodeId layer);

// Switches the masks of this one layer on. AutoFix() of IssueType::MasksOff
// does it in every layer of the file, which is right for the check box and
// wrong for a link in the inspector of one layer.
[[nodiscard]] Edit EnableLayerMasks(const Document &document, NodeId layer);

// Localized texts, main thread.
[[nodiscard]] QString MaskModeText(MaskMode mode);
[[nodiscard]] QString CanvasToolText(CanvasTool tool);

// The name for the next mask of the layer in the language of the editor
// ("Mask 2"), as new layers and shapes are named. The model itself names
// a mask in English when no name is given, so everything in the UI that
// adds a mask passes this one.
[[nodiscard]] QString NewMaskName(const Document &document, NodeId layer);

// Self-checks of the pure parts of the editor UI (this file and the graph
// math of oblivion_lottie_editor_graph.h). No style / lang / session.
[[nodiscard]] bool RunEditorSelfTest(QStringList &log);

} // namespace Oblivion::LottieEdit
