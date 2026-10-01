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

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Oblivion::LottieEdit {

class EditorController;
class GlyphButton;

// Bottom panel of the Lottie editor.
//
//  - Transport: first / previous / play-pause / next / last frame, loop,
//    the frame and time, timeline zoom.
//  - Ruler with frame numbers and seconds, click or drag to scrub, the
//    playhead goes through all rows.
//  - A row per root layer with its [ip, op) bar (drag to shift the layer
//    in time, drag an edge to trim it; small marks show the keyframes of
//    collapsed layers). The layers that hold the selection are expanded:
//    their transform, the properties of the selected shapes and every
//    other animated property get rows with keyframe diamonds (diamond:
//    linear, circle: eased, square: hold).
//  - Keyframes: click selects (Shift / Cmd add / toggle), drag on an empty
//    place selects with a rectangle, dragging a keyframe moves the
//    selected ones (one undo step, Esc cancels), double click on a row
//    adds a keyframe there, the diamond in the name column toggles a
//    keyframe at the current frame, the context menu has the easing
//    presets, copy / paste (at the current frame) and delete.
//  - Wheel scrolls rows, Shift+wheel / horizontal scroll moves in time,
//    Cmd+wheel zooms the time axis.
class TimelinePanel final : public Ui::RpWidget {
public:
	TimelinePanel(QWidget *parent, not_null<EditorController*> controller);
	~TimelinePanel();

	// The editor clipboard (shared by all editor windows of the session).
	bool copyKeyframes();
	// Pastes at the current frame into the active / copied properties.
	bool pasteKeyframes();

	// Moves the playhead to the previous (-1) / next (1) keyframe of the
	// shown rows.
	bool jumpToKeyframe(int direction);

	// Width of the names column, EditorWidget aligns it with the layers
	// panel above (0: the default width).
	void setNamesWidth(int width);

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

private:
	enum class RowType : uchar {
		Layer,
		Property,
		Empty,
	};
	struct Row {
		RowType type = RowType::Layer;
		NodeId layer = 0; // The root layer the row belongs to.
		PropertyRef property;
		QString label;
		QString owner; // Name of the node that owns a nested property.
		int colorIndex = 0;
		int top = 0;
		int height = 0;
		bool hidden = false;
		bool selected = false;
		bool expanded = false;

		// Layer: [ip, op) in root frames, summary keyframe times.
		double inPoint = 0.;
		double outPoint = 0.;
		std::vector<double> summary;

		// Property: root = local * scale + offset.
		double scale = 1.;
		double offset = 0.;
		bool editable = true;
		std::vector<Keyframe> keyframes; // Local times.
	};
	enum class HitType : uchar {
		None,
		Ruler,
		Disclosure,
		LayerName,
		LayerBar,
		LayerStart,
		LayerEnd,
		LayerTrack,
		PropertyName,
		Navigator,
		Keyframe,
		PropertyTrack,
		Scrollbar,
	};
	struct Hit {
		HitType type = HitType::None;
		int row = -1;
		int keyframe = -1;

		friend inline bool operator==(const Hit &a, const Hit &b) = default;
	};
	struct Drag;

	void setupControls();
	void updateControlsGeometry();
	void refreshPlayButton();
	void invalidateRows();
	void ensureRows();
	void rebuildRows();
	void addPropertyRows(NodeId layer, int &top);
	void clampScroll();
	void revealSelection();

	[[nodiscard]] int namesWidth() const;
	[[nodiscard]] int rowsTop() const;
	[[nodiscard]] int trackLeft() const;
	[[nodiscard]] int trackRight() const;
	[[nodiscard]] double viewSpan() const;
	// The composition [ip, op] on the track: layers and keyframes may lie
	// outside of it, those parts are not drawn (and can't be hit).
	[[nodiscard]] std::pair<double, double> compositionX() const;
	[[nodiscard]] bool inComposition(double rootFrame) const;
	[[nodiscard]] QRect trackClip(int y, int height, double extend) const;
	[[nodiscard]] double xFromFrame(double frame) const;
	[[nodiscard]] double frameFromX(double x) const;
	[[nodiscard]] int frameAtX(double x) const;
	[[nodiscard]] double keyframeX(const Row &row, int index) const;
	[[nodiscard]] QRect scrollbarRect() const;
	void setViewStart(double start);
	void zoomTime(double factor, std::optional<double> anchorX = {});
	void keepPlayheadVisible();

	[[nodiscard]] Hit hitTest(QPoint point);
	[[nodiscard]] bool onPlayhead(QPoint point, const Hit &hit) const;
	[[nodiscard]] std::optional<double> localFrameOf(const Row &row) const;
	[[nodiscard]] std::optional<int> keyframeAtPlayhead(const Row &row) const;
	[[nodiscard]] KeyframeRef keyframeRef(const Row &row, int index) const;
	[[nodiscard]] std::vector<KeyframeRef> keyframesInRect(QRect rect);

	void seek(double x);
	void pressKeyframe(const Hit &hit, Qt::KeyboardModifiers modifiers);
	void pressLayer(const Hit &hit, Qt::KeyboardModifiers modifiers);
	void toggleKeyframeAtPlayhead(int row);
	void addKeyframeAt(int row, int rootFrame);
	void applyDrag(QPoint point, Qt::KeyboardModifiers modifiers);
	void finishDrag(bool cancel);
	void showKeyframeMenu(int row, QPoint globalPosition);
	void showRowMenu(int row, QPoint globalPosition);
	void setEasing(EasingPreset preset);
	bool paste(std::optional<PropertyRef> target);
	void selectAllKeyframes();
	void showToast(const QString &text);

	void paintTransport(QPainter &p, QRect clip);
	void paintRuler(QPainter &p);
	void paintRows(QPainter &p, QRect clip);
	void paintLayerRow(QPainter &p, const Row &row, int y);
	void paintPropertyRow(QPainter &p, const Row &row, int index, int y);
	void paintPlayhead(QPainter &p);
	void paintScrollbar(QPainter &p);
	// The frame number badge of the playhead on the ruler, empty when the
	// playhead is out of view. Ruler labels under it are not drawn.
	[[nodiscard]] QRectF playheadBadgeRect() const;

	const not_null<EditorController*> _controller;
	GlyphButton *_first = nullptr;
	GlyphButton *_previous = nullptr;
	GlyphButton *_play = nullptr;
	GlyphButton *_next = nullptr;
	GlyphButton *_last = nullptr;
	GlyphButton *_loop = nullptr;
	GlyphButton *_zoomOut = nullptr;
	GlyphButton *_zoomIn = nullptr;

	std::vector<Row> _rows;
	bool _rowsDirty = true;
	int _contentHeight = 0;
	std::map<NodeId, bool> _expandedOverride;
	int _scrollTop = 0;
	int _namesWidth = 0;
	double _timeZoom = 1.;
	double _viewStart = 0.;

	std::unique_ptr<Drag> _drag;
	Hit _over;
	base::unique_qptr<Ui::PopupMenu> _menu;
	uint64 _gestures = 0;

};

} // namespace Oblivion::LottieEdit
