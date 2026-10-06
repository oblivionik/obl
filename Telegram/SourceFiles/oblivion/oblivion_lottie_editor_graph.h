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
#include "ui/widgets/tooltip.h"

namespace Ui {
class PopupMenu;
} // namespace Ui

// Lottie editor: the easing graph of the timeline. It takes the place of
// the keyframe tracks (TimelinePanel shows it over them, the ruler, the
// names column and the transport stay) and shares the time axis with the
// ruler above it.
//
// It shows the active property of the controller (a click on a property
// name in the names column or in the inspector), or the property of the
// selected keyframes:
//  - "Value": the value over time, a curve per dimension (X / Y / Z) for
//    numbers and points; colors, gradients and paths have no single number,
//    for them it is the progress between the keyframes (keyframe 1, 2,
//    3... on the vertical axis);
//  - "Speed": how fast the value changes, per second.
// Every keyframe has two bezier handles, one for the segment before it
// and one for the segment after it. Dragging a handle changes the easing
// of that segment (one undo step per drag, Esc cancels); dragging from a
// keyframe point itself pulls a handle out of it (to the right: the
// outgoing one, to the left: the incoming one), so linear keyframes get
// handles too. The buttons above the plot apply the presets (linear, ease
// in, ease out, ease in-out, hold) to the selected keyframes, or to every
// keyframe of the property when none is selected.
//
// Handle limits come from EasingRange(): properties that move along a
// motion path can't overshoot, the "Along the path" switch (SetMotionPath)
// turns the path off for those who want the overshoot.
namespace Oblivion::LottieEdit {

class EditorController;

enum class GraphMode : uchar {
	Value,
	Speed,
};

// The math of the graph, pure. A segment goes from the keyframe at t0 with
// the value v0 to the keyframe at t1 with the value v1 (any units), the
// easing handles are points of the unit square like in KeyframeHandles.
namespace GraphMath {

struct Segment {
	double t0 = 0.;
	double t1 = 0.;
	double v0 = 0.;
	double v1 = 0.;
};

// Value graph: the handle as a point (time, value) and back. fallback
// gives the y of the handle when the values are equal (it can't be read
// from a flat segment then).
[[nodiscard]] QPointF ValuePoint(const Segment &segment, QPointF handle);
[[nodiscard]] QPointF ValueHandle(
	const Segment &segment,
	QPointF point,
	QPointF fallback);

// Speed graph: a handle is drawn on the level of the speed its keyframe
// has on that side, as far from the keyframe as its influence (x) goes.
// out: the handle of the keyframe at t0, otherwise of the one at t1.
[[nodiscard]] double StartSpeed(const Segment &segment, const Easing &easing);
[[nodiscard]] double EndSpeed(const Segment &segment, const Easing &easing);
[[nodiscard]] QPointF SpeedPoint(
	const Segment &segment,
	const Easing &easing,
	bool out);
[[nodiscard]] QPointF SpeedHandle(
	const Segment &segment,
	QPointF point,
	bool out,
	QPointF fallback);

// Speed of the value at the linear progress u in [0, 1] of the segment.
[[nodiscard]] double SpeedAt(
	const Segment &segment,
	const Easing &easing,
	double u);

// 1, 2, 5, 10, 20... so that span / step <= maxTicks.
[[nodiscard]] double NiceStep(double span, int maxTicks);

// The multiples of step for the grid lines of the range [min, max] with
// one more span of room on both sides, ascending. Empty when the range
// can't be walked: it is not finite, it overflows with that room, it has
// too many steps, or the step is below the precision of its numbers (a
// broken file may have any values, the count is bounded whatever they are).
[[nodiscard]] std::vector<double> GridValues(
	double min,
	double max,
	double step);

} // namespace GraphMath

class GraphEditor final
	: public Ui::RpWidget
	, public Ui::AbstractTooltipShower {
public:
	GraphEditor(QWidget *parent, not_null<EditorController*> controller);
	~GraphEditor();

	// The time axis of the timeline: root composition frames, viewSpan of
	// them from viewStart between the x coordinates left and right of this
	// widget.
	void setTimeView(int left, int right, double viewStart, double viewSpan);

	[[nodiscard]] GraphMode mode() const;
	void setMode(GraphMode mode);

	// A handle is being dragged: Esc of the timeline cancels it.
	[[nodiscard]] bool dragging() const;
	bool cancelDrag();

	// The property on the screen, invalid while there is nothing to show.
	[[nodiscard]] PropertyRef shownProperty() const;

	QString tooltipText() const override;
	QPoint tooltipPos() const override;
	bool tooltipWindowActive() const override;

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	enum class Control : uchar {
		None,
		ModeValue,
		ModeSpeed,
		PresetLinear,
		PresetEaseIn,
		PresetEaseOut,
		PresetEaseInOut,
		PresetHold,
		MotionPath,
	};
	enum class HitType : uchar {
		None,
		Control,
		Handle,
		Keyframe,
		Playhead,
	};
	struct Hit {
		HitType type = HitType::None;
		Control control = Control::None;
		int key = -1;
		bool out = false; // Handle: the outgoing one.

		friend inline bool operator==(const Hit &a, const Hit &b) = default;
	};
	struct Curve {
		std::vector<QPointF> points; // (root frame, value).
		int dimension = 0;
	};
	struct KeyPoint {
		double frame = 0.; // Root frame.
		// The points of the keyframe on the plot: one in the value graph,
		// the incoming and the outgoing speed in the speed graph.
		QPointF in;
		QPointF out;
		// Value graph of numbers: the keyframe on every curve.
		std::vector<double> values;
		bool hasIn = false; // A handle for the segment before it.
		bool hasOut = false;
		QPointF inHandle; // (root frame, value).
		QPointF outHandle;
		bool inPulled = false; // The handle is away from its keyframe.
		bool outPulled = false;
		bool selected = false;
		EasingPreset preset = EasingPreset::Linear;
	};
	struct Data;
	struct Drag;

	void invalidate();
	void ensureData();
	void rebuildData();
	void rebuildPlot();
	[[nodiscard]] PropertyRef resolveProperty() const;
	[[nodiscard]] GraphMath::Segment segmentFor(int key) const;

	[[nodiscard]] QRect headerRect() const;
	[[nodiscard]] QRect plotRect() const;
	[[nodiscard]] QRect controlRect(Control control) const;
	[[nodiscard]] bool controlShown(Control control) const;
	[[nodiscard]] bool controlActive(Control control) const;
	[[nodiscard]] QString controlText(Control control) const;
	void layoutControls();

	[[nodiscard]] double xFromFrame(double rootFrame) const;
	[[nodiscard]] double frameFromX(double x) const;
	[[nodiscard]] double yFromValue(double value) const;
	[[nodiscard]] double valueFromY(double y) const;
	[[nodiscard]] QPointF toScreen(QPointF data) const;
	[[nodiscard]] QPointF fromScreen(QPointF point) const;

	[[nodiscard]] Hit hitTest(QPoint point);
	void setOver(Hit hit);
	void clickControl(Control control);
	void applyPreset(EasingPreset preset);
	[[nodiscard]] std::vector<KeyframeRef> presetTargets() const;
	void pressKeyframe(int key, Qt::KeyboardModifiers modifiers);
	void startHandleDrag(int key, bool out);
	void applyHandleDrag(QPointF point);
	void finishDrag(bool cancel);
	void showKeyframeMenu(QPoint globalPosition);

	void paintHeader(QPainter &p);
	void paintGrid(QPainter &p, const QRect &plot);
	void paintCurves(QPainter &p, const QRect &plot);
	void paintKeys(QPainter &p, const QRect &plot);
	void paintPlayhead(QPainter &p, const QRect &plot);

	const not_null<EditorController*> _controller;
	GraphMode _mode = GraphMode::Value;

	int _timeLeft = 0;
	int _timeRight = 1;
	double _viewStart = 0.;
	double _viewSpan = 1.;

	std::unique_ptr<Data> _data;
	bool _dirty = true;
	std::vector<Curve> _curves;
	std::vector<KeyPoint> _keys;
	double _minValue = 0.;
	double _maxValue = 1.;

	std::vector<std::pair<Control, QRect>> _controls;
	Hit _over;
	std::unique_ptr<Drag> _drag;
	base::unique_qptr<Ui::PopupMenu> _menu;
	uint64 _gestures = 0;

};

} // namespace Oblivion::LottieEdit
