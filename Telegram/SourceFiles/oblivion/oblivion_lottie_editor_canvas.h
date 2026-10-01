/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/unique_qptr.h"
#include "oblivion/oblivion_lottie_doc.h"
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

	std::optional<QPointF> _cursor; // Composition coordinates.
	std::unique_ptr<Drag> _drag;
	base::unique_qptr<Ui::PopupMenu> _menu;
	uint64 _gestures = 0;

};

} // namespace Oblivion::LottieEdit
