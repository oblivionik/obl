/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/unique_qptr.h"
#include "oblivion/oblivion_lottie_doc.h"
#include "oblivion/oblivion_lottie_editor_masks.h"
#include "ui/rp_widget.h"

#include <QtGui/QPainterPath>
#include <QtGui/QPolygonF>

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Oblivion::LottieEdit {

class EditorController;
class FrameRenderer;

// What is drawn under the composition.
enum class CanvasBackground : uchar {
	Checker, // Transparency checkerboard in the palette colors.
	Dark, // A dark chat-like backdrop.
	Light, // White.
};

// Center panel of the Lottie editor: the composition preview and the
// on-canvas editing.
//
//  - The current frame is rendered off the main thread (FrameRenderer, one
//    rlottie instance rebuilt only when the document changes, the newest
//    request wins), so playback runs at the document frame rate as long as
//    rlottie keeps up; while playing the render size is capped.
//  - View: zoom (controller zoom, 1 = fit) with Cmd+wheel / pinch /
//    the toolbar, pan with the wheel / trackpad, the middle button or
//    dragging an empty place.
//  - Selection: the oriented bounding box of every selected node at the
//    current frame (from Document::transformAt() / outlineAt()), the
//    anchor point of the primary one, a hover outline.
//  - Click selects the topmost root layer under the cursor (or a sibling
//    of the selected shape inside the same layer), Cmd+click toggles,
//    Shift+click adds, double click goes one level deeper into groups,
//    Alt+click selects the deepest shape.
//  - Dragging moves the selected layers / groups / shapes (their position,
//    auto-keyframed at the current frame if animated) as one undo step,
//    Shift keeps the move horizontal or vertical, Esc cancels.
//  - Masks of the selected layers are outlined with a dashed line.
//  - The pen tool (CanvasTool::Pen, the toolbar or P; V goes back to the
//    selection) edits the points of the selected path item or mask:
//    dragging a point moves it, dragging a handle bends the curve (a smooth
//    point keeps both handles on one line, Alt moves one handle alone),
//    a click on the outline adds a point, a double click or Alt+click on
//    a point switches it between a corner and a smooth one, Alt+drag pulls
//    new handles out of it, Delete removes the selected point (and only
//    it: the press that removed a point, held or repeated, does not go on
//    to delete the path, that takes a click on the canvas first). With the
//    last (or the first) point of an open path selected a click on an
//    empty place continues the path, a click on its other end closes it.
//    With a shape layer or a group selected two clicks start a new path in
//    it (not next to a selected rectangle / ellipse, which is to be
//    converted to a path); the paths and masks inside the selection are
//    outlined and a click on one of them selects it. Every drag is one undo
//    step, Esc cancels it, then drops the point selection.
class CanvasPanel final : public Ui::RpWidget {
public:
	CanvasPanel(QWidget *parent, not_null<EditorController*> controller);
	~CanvasPanel();

	[[nodiscard]] CanvasBackground background() const;
	void setBackground(CanvasBackground background);
	[[nodiscard]] rpl::producer<CanvasBackground> backgroundValue() const;

	// Logical pixels per composition pixel at the controller zoom 1.
	[[nodiscard]] double fitScale() const;

	// The displayed scale: fitScale() * zoom (1. is "actual size").
	[[nodiscard]] double scale() const;
	[[nodiscard]] rpl::producer<double> scaleValue() const;

	// anchor: the panel point that keeps its composition point, the
	// panel center by default.
	void zoomToScale(double scale, std::optional<QPointF> anchor = {});
	void zoomBy(double factor, std::optional<QPointF> anchor = {});
	void zoomToFit();

	// Moves the selection by delta composition pixels (screen aligned),
	// continuous nudges merge into one undo step. False if nothing moved.
	bool nudgeSelection(QPointF delta);

	// The frame for the current document and frame is on the screen.
	[[nodiscard]] bool frameReady() const;

	// The tool of this editor (CurrentTool() of the controller).
	[[nodiscard]] CanvasTool tool() const;
	void setTool(CanvasTool tool);

	// The point of the edited path the pen has selected, -1 for none.
	[[nodiscard]] int selectedPathVertex() const;
	void selectPathVertex(int index);

	// A rectangle / an ellipse of the selection becomes a path item (the
	// pen works on paths only), false if the selection is something else.
	bool convertSelectionToPath();

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	bool eventHook(QEvent *e) override;

private:
	enum class HitDepth : uchar {
		Keep, // The depth of the current selection.
		Deeper, // One level below the selection.
		Deepest,
	};
	struct OverlayItem {
		NodeId id = 0;
		QPainterPath outline; // Composition coordinates.
		QPolygonF box; // Oriented bounding box.
		std::optional<QPointF> anchor;
		bool primary = false;
	};
	struct MoveTarget;
	struct Drag;

	// The path the pen edits: the selected path item / mask at the
	// current frame.
	struct PenTarget {
		NodeId node = 0;
		PropertyRef ref;
		PathData path; // In its own coordinates.
		QTransform toCanvas; // Path coordinates -> composition.
		double localFrame = 0.;

		[[nodiscard]] bool valid() const {
			return ref.valid();
		}
	};
	struct PenGeometry {
		PenTarget target;
		// Other paths / masks of the selection, composition coordinates.
		std::vector<std::pair<NodeId, QPainterPath>> candidates;
		// Masks of the selected layers, composition coordinates.
		std::vector<std::pair<NodeId, QPainterPath>> masks;
	};

	[[nodiscard]] QRectF canvasRect() const;
	[[nodiscard]] QRectF canvasRect(double scale, QPointF pan) const;
	[[nodiscard]] QTransform viewTransform() const;
	[[nodiscard]] QPointF toCanvas(QPointF point) const;
	[[nodiscard]] QPointF clampPan(QPointF pan, double scale) const;
	void setView(double zoom, QPointF pan);
	void refreshScale();

	void requestFrame();
	void invalidateGeometry();
	void ensureGeometry();
	void ensureLayerOutlines();
	void ensureOverlay();
	void ensureHoverPath();

	[[nodiscard]] NodeId hitTest(QPointF point, HitDepth depth);
	[[nodiscard]] bool selectionContains(QPointF point);
	[[nodiscard]] std::vector<MoveTarget> collectTargets(
		const Document &document) const;
	[[nodiscard]] static Edit MoveEdit(
		const Document &start,
		const std::vector<MoveTarget> &targets,
		QPointF delta);
	void startMove();
	void applyMove(QPointF delta);
	void finishDrag(bool cancel);
	void setHovered(NodeId id);
	void updateCursor(QPointF point);
	void showMenu(QPoint globalPosition);

	void paintBackground(QPainter &p, const QRectF &rect);
	void paintOverlay(QPainter &p);
	void paintInfo(QPainter &p);

	void ensurePenGeometry();
	void resetPen();
	[[nodiscard]] double pickRadius() const;
	[[nodiscard]] QTransform penToView() const;
	[[nodiscard]] PathPick penPick(QPointF point);
	[[nodiscard]] NodeId pickCandidate(QPointF point);
	void penPress(QPointF point, Qt::KeyboardModifiers modifiers);
	void penHover(QPointF point);
	void penDoubleClick(QPointF point);
	bool penKey(not_null<QKeyEvent*> e);
	// part Vertex with the mode Mirrored pulls new handles out of the
	// vertex, with any other mode it moves the vertex.
	void startPenDrag(
		QPointF point,
		int vertex,
		PathPart part,
		TangentMode mode);
	void applyPenDrag(QPointF point, Qt::KeyboardModifiers modifiers);
	bool startNewPath(NodeId container, QPointF first, QPointF second);
	bool performPen(Edit &&edit);
	void toggleVertex(int index);
	void removeVertex(int index);
	void addPenMenuItems(QPointF point);
	[[nodiscard]] QString penHint();
	void paintMasks(QPainter &p);
	void paintPen(QPainter &p);
	void paintHint(QPainter &p);

	const not_null<EditorController*> _controller;
	const std::unique_ptr<FrameRenderer> _renderer;
	rpl::variable<CanvasBackground> _background;
	rpl::variable<double> _scale = 1.;

	// The frame on the screen and the last requested one.
	QImage _frame;
	Document _frameDocument;
	int _frameIndex = -1;
	Document _requestedDocument;
	int _requestedFrame = -1;
	QSize _requestedSize;
	QPixmap _checker;

	// Geometry at the current frame, rebuilt lazily.
	Document _geometryDocument;
	int _geometryFrame = -1;
	std::vector<std::pair<NodeId, QPainterPath>> _layerOutlines;
	bool _layerOutlinesValid = false;
	std::vector<OverlayItem> _overlay;
	bool _overlayValid = false;
	NodeId _hovered = 0;
	QPainterPath _hoverPath;
	bool _hoverPathValid = false;

	// The pen tool.
	CanvasTool _tool = CanvasTool::Select;
	PenGeometry _pen;
	bool _penValid = false;
	int _penVertex = -1;
	// A point was just removed: one more Delete is taken for a slip of the
	// finger and not for "delete the whole path" until the next click.
	bool _penPointRemoved = false;
	PathPick _penOver;
	// The first point of a new path (composition coordinates) waits for
	// the second one in this shape layer / group.
	std::optional<QPointF> _penPending;
	NodeId _penPendingContainer = 0;

	std::optional<QPointF> _cursor; // Composition coordinates.
	std::unique_ptr<Drag> _drag;
	base::unique_qptr<Ui::PopupMenu> _menu;
	uint64 _gestures = 0;

};

} // namespace Oblivion::LottieEdit
