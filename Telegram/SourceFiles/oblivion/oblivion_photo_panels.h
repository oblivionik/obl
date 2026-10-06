/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "oblivion/oblivion_photo_editor.h"

namespace Ui {
class RpWidget;
class Show;
} // namespace Ui

// Photo editor: the panels built from descriptors.
//
//  - CreateParamsPanel(): the generic parameter panel. It makes a control
//    for every FxParam of a list: sliders with a value field (click the
//    value to type it, double click the slider to reset it), switches,
//    choices, colors (presets and a picker), angles, points picked and
//    dragged on the canvas, seeds with a "randomize" button and the
//    widgets of the registered custom editors. It is not tied to effects:
//    the options of a tool or the settings of a collage can be described
//    with FxParam lists and shown with it too.
//  - ShowAddFxMenu(): the "add effect" menu, a submenu per FxGroup with
//    the effects and the presets of the group.
//  - CreateFxStackPanel(): the stack of effects of a layer, a card per
//    effect with its on / off switch, its parameters and a menu (move up
//    / down, duplicate, reset, remove), plus the "add effect" button.
//  - CreateLayerPanel(): the whole Layer tab of the editor: the name,
//    opacity and blend mode of the active layer, the sections registered
//    for PanelSlot::LayerProperties and the effects stack.
//  - CreateLayersPanel(): what the layers slot of the editor shows, the
//    panel registered for PanelSlot::Layers or a small built-in list.
//
// All of them are dark "studio" widgets (the palette of the editor, the
// same in day and night themes), laid out by resizeToWidth() with the
// height reported through heightValue(), and are meant to be put into
// a Ui::VerticalLayout inside a scroll area. Main thread.
namespace Oblivion::Photo {

struct ParamsPanelArgs {
	// What to show, in this order. Parameters with FxParam::visible are
	// hidden while it returns false for the current values.
	std::vector<FxParam> params;

	// The current values (missing ones are shown as defaults).
	FxParams values;

	// Values changed from outside (undo, redo, a preset, a reset): the
	// controls show them without calling changed.
	rpl::producer<FxParams> updates;

	// One parameter was edited. finished == false while a slider or
	// a canvas point is being dragged (a live change), true when the
	// edit is complete (make it an undo step).
	Fn<void(const QByteArray &id, FxValue value, bool finished)> changed;

	// Needed for Point parameters (they are picked on the canvas with
	// a temporary tool) and passed to custom editors. Without it Point
	// parameters only show their coordinates.
	Controller *controller = nullptr;

	// The layer whose rectangle Point parameters are normalized to, and
	// the effect instance the parameters belong to (for custom editors).
	// 0: the active layer / no instance.
	LayerId layerId = 0;
	uint64 fxUid = 0;

	// Report slider changes only when the drag ends (slow effects).
	bool commitOnRelease = false;

	// Dims every control (a disabled effect).
	rpl::producer<bool> dimmed;

	// The ids of the Int / Float parameters with a positive minimum whose
	// sliders move with the logarithm of the value: sizes with a wide
	// range (the canvas, 16 .. 16384 pixels), where an even scale keeps
	// every usual value at the very start of the track. The values and
	// their limits are the same, only the place of the knob differs.
	std::vector<QByteArray> logarithmic;
};

[[nodiscard]] object_ptr<Ui::RpWidget> CreateParamsPanel(
	not_null<QWidget*> parent,
	ParamsPanelArgs &&args);

// The value as the controls show it: "+12", "0.50", "45°", "On"...
[[nodiscard]] QString FormatParamValue(
	const FxParam &param,
	const FxValue &value);

// Shows the menu of all registered effects (not kFxHidden) grouped by
// FxGroup: a submenu per group with its one-click presets first, then
// its effects. groups limits the menu to some groups (empty: all of
// them), flat puts everything into one menu with separators between the
// groups instead of submenus (for a short list). chosen gets the
// instances to append (one for an effect, several for a preset), their
// parameters are set, their uids are not.
void ShowAddFxMenu(
	not_null<QWidget*> parent,
	QPoint globalPosition,
	Fn<void(std::vector<FxInstance> stack)> chosen,
	std::vector<FxGroup> groups = {},
	bool flat = false);

// Appends the instances to the effects of the layer as one undo step.
// A locked layer, no layer and too many effects are refused with a toast
// (false then).
bool AppendLayerFx(
	not_null<Controller*> controller,
	LayerId id,
	std::vector<FxInstance> stack);

// The effects of the layer the producer gives (0: nothing to show).
[[nodiscard]] object_ptr<Ui::RpWidget> CreateFxStackPanel(
	not_null<QWidget*> parent,
	not_null<Controller*> controller,
	rpl::producer<LayerId> layer);

[[nodiscard]] object_ptr<Ui::RpWidget> CreateLayerPanel(
	not_null<QWidget*> parent,
	not_null<Controller*> controller);
// The top of the last effect card of a panel made by CreateLayerPanel()
// (and only of such a panel), in its own coordinates: where to scroll to
// show an effect that was just added from its header. -1 while the active
// layer has no effects.
[[nodiscard]] int LayerPanelLastFxTop(not_null<Ui::RpWidget*> layerPanel);

[[nodiscard]] object_ptr<Ui::RpWidget> CreateLayersPanel(
	not_null<QWidget*> parent,
	not_null<Controller*> controller);

// The color picker box of the editor (a hue / saturation square with
// fields). done is called with the chosen color, nothing on cancel.
void ShowColorPickerBox(
	std::shared_ptr<Ui::Show> show,
	QColor color,
	Fn<void(QColor color)> done);

} // namespace Oblivion::Photo
