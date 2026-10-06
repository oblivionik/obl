/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/rp_widget.h"

namespace Ui {
class PillTabs;
class ScrollArea;
} // namespace Ui

namespace Oblivion::LottieEdit {

class EditorController;
class PalettePanel;

// Right panel of the Lottie editor, two tabs.
//
// "Properties": the primary selection at the current frame.
//  - layers: transform (anchor, position, scale, rotation, opacity and
//    skew / 3D rotation when present; missing ones are shown with their
//    default values and created on the first edit), timing (in / out,
//    start time and time stretch of precomposition layers, time remap),
//    the solid color, parent and track matte info;
//  - groups: the group transform;
//  - shape items: their values (fill / stroke color with a hex field and
//    the color picker, opacity, stroke width, line caps / corners / miter
//    limit, dashes, fill rule, gradient type and stops, trim start / end /
//    offset and mode, rectangle / ellipse / star geometry...), effects:
//    their values;
//  - After Effects features (round 4). Layers: the parent (a layer
//    picker, the layer keeps its place), the track matte (mode and the
//    layer that cuts), the list of masks with their modes and "Add mask".
//    Masks: mode, path, "Edit points" (the pen tool of the canvas),
//    "Invert", opacity / expansion / feather. Gradients: a stops editor
//    (click adds, drag moves, drag away removes, the selected stop has
//    a color / an opacity and a position), "Make it a solid color" and
//    back. Strokes: the dash pattern. Repeaters, trim paths, rounded
//    corners ("Turn into real corners"), paths ("Edit points", close /
//    open, reverse), rectangles and ellipses ("Convert to path"). Red
//    hints tell what Telegram does not accept in stickers or does not
//    draw;
//  - nothing selected: the composition (canvas size, frame rate,
//    duration).
// Every animatable value has keyframe controls on the left: the diamond
// adds a keyframe at the current frame (it's filled when there is one,
// a click removes it), the arrows jump to the previous / next keyframe.
// Numbers are changed by dragging them left / right (Shift: x10, Option:
// x0.1, one undo step per drag) or by clicking and typing. Clicking a
// property name makes it the controller's active property (timeline).
//
// "Palette": PalettePanel (oblivion_lottie_editor_palette.h).
class InspectorPanel final : public Ui::RpWidget {
public:
	enum class Tab : uchar {
		Properties,
		Palette,
	};

	InspectorPanel(QWidget *parent, not_null<EditorController*> controller);
	~InspectorPanel();

	void setTab(Tab tab);
	[[nodiscard]] Tab tab() const;

	// Selects a stop of the gradient of the selected gradient fill /
	// stroke, as a click on it in the stops bar does (opacity: one of the
	// opacity stops above the bar).
	void selectGradientStop(int index, bool opacity = false);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

private:
	class Content;

	void updateGeometries();
	// remember: the tab chosen by the user, kept for the app session.
	void showTab(Tab tab, bool remember);

	const not_null<EditorController*> _controller;
	const not_null<Ui::PillTabs*> _tabs;
	const not_null<Ui::ScrollArea*> _propertiesScroll;
	const not_null<Content*> _content;
	const not_null<Ui::ScrollArea*> _paletteScroll;
	const not_null<PalettePanel*> _palette;
	Tab _tab = Tab::Properties;
	bool _switching = false;

};

} // namespace Oblivion::LottieEdit
