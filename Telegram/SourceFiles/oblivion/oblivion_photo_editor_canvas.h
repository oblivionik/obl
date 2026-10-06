/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/effects/radial_animation.h"
#include "ui/rp_widget.h"

// The image area of the photo editor (see oblivion_photo_editor.h).
//
// It shows a rendered frame of a known full-resolution size scaled to fit
// the widget, zoomed (Cmd+wheel, pinch, double click) and panned (drag,
// wheel, Space+drag). The frame image itself can have any resolution, the
// owner renders it for renderPixels() and re-renders on viewChanges().
//
// In the crop mode the frame is always fit and a crop rectangle
// (normalized to the frame) is edited with the mouse, optionally keeping
// an aspect ratio (Shift keeps the current one while dragging).
//
// Outside of the crop mode the current canvas tool (see Tool in
// oblivion_photo_editor.h), if there is one, gets the left button, the
// hover, the wheel and paints its overlay over the frame. Space + drag
// and the middle button always pan.
namespace Oblivion::Photo {
class Tool;
struct ToolMouseEvent;
} // namespace Oblivion::Photo

namespace Oblivion::Photo::EditorUi {

class Canvas final : public Ui::RpWidget {
public:
	explicit Canvas(QWidget *parent);
	~Canvas();

	// Full-resolution size of the shown frame. A different aspect ratio
	// resets the zoom.
	void setFrameSize(QSize size);
	[[nodiscard]] QSize frameSize() const;
	void setImage(QImage image);
	void setBeforeImage(QImage image);
	void clearImages();
	[[nodiscard]] bool hasImage() const;
	void setComparing(bool comparing);
	[[nodiscard]] bool comparing() const;
	void setCheckerboard(bool enabled);
	void setLoading(bool loading);

	void setCropMode(bool enabled);
	[[nodiscard]] bool cropMode() const;
	void setCrop(QRectF crop);
	// Width / height of the crop in frame pixels, nullopt: free.
	void setCropRatio(std::optional<float64> ratio);
	void setGridVisible(bool visible);
	[[nodiscard]] bool cropDragging() const;
	[[nodiscard]] rpl::producer<QRectF> cropChanges() const;
	[[nodiscard]] rpl::producer<> cropFinished() const;

	void setPanMode(bool enabled);

	// The current tool, null for the plain view. The provider is asked
	// on every event, toolChanged() must be called when its answer
	// changes (it drops a drag the old tool had).
	void setToolProvider(Fn<Tool*()> provider);
	void toolChanged();
	// True from a press the tool took till its release (or till the tool
	// is replaced meanwhile).
	[[nodiscard]] rpl::producer<bool> toolDragValue() const;
	// Document (canvas) coordinates -> pixels of the frame, see
	// OutputTransform() in oblivion_photo_doc.h.
	void setFrameTransform(const QTransform &documentToFrame);
	[[nodiscard]] QTransform documentToWidget() const;
	[[nodiscard]] float64 documentScale() const;

	// Output sizes (in pixels of the result) worth rendering: for the
	// frame fit into the widget and for the current zoom.
	[[nodiscard]] QSize fitPixels() const;
	[[nodiscard]] QSize renderPixels() const;
	[[nodiscard]] rpl::producer<> viewChanges() const;

	void zoomIn();
	void zoomOut();
	void zoomFit();
	void zoomActual();
	[[nodiscard]] bool zoomed() const;
	[[nodiscard]] rpl::producer<float64> zoomPercentValue() const;

protected:
	bool eventHook(QEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void enterEventHook(QEnterEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	class ZoomControls;

	enum class Drag : uchar {
		None,
		Pan,
		Move,
		Left,
		Top,
		Right,
		Bottom,
		TopLeft,
		TopRight,
		BottomLeft,
		BottomRight,
		Tool,
	};

	[[nodiscard]] QRectF contentRect() const;
	[[nodiscard]] float64 fitScale() const;
	[[nodiscard]] float64 scale() const;
	[[nodiscard]] float64 maxZoom() const;
	[[nodiscard]] float64 actualZoom() const;
	[[nodiscard]] QRectF imageRect() const;
	[[nodiscard]] QRectF cropFrameRect() const;
	[[nodiscard]] QRectF cropWidgetRect() const;
	[[nodiscard]] bool pannable() const;
	[[nodiscard]] Drag cropHitTest(QPointF point) const;
	void setZoom(float64 zoom, QPointF anchor);
	void toggleZoom(QPointF anchor);
	void panBy(QPointF delta);
	void clampPan();
	void viewUpdated();
	void updateCursor(QPointF point);
	void updateCropDrag(QPointF point, Qt::KeyboardModifiers modifiers);
	void finishDrag();
	void finishLostDrag(Qt::KeyboardModifiers modifiers);
	void updateZoomControls();
	[[nodiscard]] Tool *activeTool() const;
	[[nodiscard]] ToolMouseEvent toolEvent(
		QPointF position,
		Qt::MouseButton button,
		Qt::MouseButtons buttons,
		Qt::KeyboardModifiers modifiers) const;
	void paintTool(QPainter &p);

	void paintCheckerboard(QPainter &p, QRectF target);
	void paintImage(QPainter &p, const QImage &image, QRectF target);
	void paintCrop(QPainter &p);
	void paintBadge(QPainter &p);
	void paintLoading(QPainter &p);

	QSize _frame;
	QImage _image;
	QImage _before;
	bool _comparing = false;
	bool _checkerboard = false;
	bool _loading = false;
	QImage _checker;

	bool _cropMode = false;
	QRectF _crop = QRectF(0., 0., 1., 1.);
	std::optional<float64> _cropRatio;
	bool _gridVisible = false;

	float64 _zoom = 1.;
	QPointF _pan;
	bool _panMode = false;
	bool _hovered = false;

	Drag _drag = Drag::None;
	QPointF _dragStart;
	QPointF _dragLast; // Where a tool drag saw the mouse last.
	QPointF _dragPan;
	QRectF _dragCrop;

	Fn<Tool*()> _toolProvider;
	QTransform _documentToFrame;

	Ui::InfiniteRadialAnimation _radial;
	ZoomControls *_zoomControls = nullptr;

	rpl::event_stream<QRectF> _cropChanges;
	rpl::event_stream<> _cropFinished;
	rpl::event_stream<> _viewChanges;
	rpl::variable<float64> _zoomPercent = 100.;
	rpl::variable<bool> _toolDrag = false;

};

} // namespace Oblivion::Photo::EditorUi
