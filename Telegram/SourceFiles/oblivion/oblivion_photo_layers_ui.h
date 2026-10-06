/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "oblivion/oblivion_photo_doc.h"

// Photo editor: the layers list and the two canvas tools that work with
// a layer as a whole. Everything registers itself in the editor (see
// EditorRegistrar in oblivion_photo_editor.h), nothing has to be called:
//
//  - the layers panel (PanelSlot::Layers): the layers from the top one
//    down with their thumbnails, a click chooses the active layer, the
//    eye hides and the lock protects it, a double click renames, a drag
//    reorders, the right button (and the "..." button) opens the menu
//    with duplicate / delete / merge down / flatten / blending mode /
//    opacity / mask actions, "+" adds a layer of any registered kind or
//    pastes a picture from the clipboard;
//  - the transform tool (kTransformTool, the V key): a frame with handles
//    around the active layer. Dragging the layer moves it (a click on
//    another layer chooses that one, transparent pixels are clicked
//    through), the squares scale it, the round handle rotates it, in the
//    "Perspective" mode (or with Cmd / Ctrl in the usual one) every
//    corner moves freely. The moved edges stick to the canvas edges and
//    center and to the other layers. The Tool tab shows the numbers
//    (they can be typed) and flip / rotate / fit / fill / center / reset.
//    The arrow keys move the layer by a pixel (ten with Shift), Delete or
//    Backspace deletes it: only while the keyboard is with the canvas,
//    never while a field of a panel is being typed in. Left, right,
//    horizontal and vertical are the ones of the screen, whatever the
//    turn and the mirror of the whole picture are;
//  - the mask tool (kMaskTool, the M key): a round brush that hides or
//    reveals parts of the active layer by painting its mask (the mask is
//    created by the first stroke), with the size, hardness and strength
//    of the brush, a red highlight of the hidden parts, invert / reveal
//    all / hide all / delete.
//
// The maths of all that (and its self-test) is in
// oblivion_photo_transform.h.
namespace Oblivion::Photo {

class Controller;

// The ids of the canvas tools registered here, for Controller::setTool()
// and PhotoEditorOptions::tool.
inline const auto kTransformTool = QByteArray("layer.transform");
inline const auto kMaskTool = QByteArray("layer.mask");

// The layer actions of the menu, for other panels and menus. Each one is
// an undo step, a locked layer is refused with a toast. Main thread.
//
// Gives the layer an empty (everything is shown) mask if it has none and
// switches to the mask tool.
void AddLayerMask(not_null<Controller*> controller, LayerId id);
void RemoveLayerMask(not_null<Controller*> controller, LayerId id);
void InvertLayerMask(not_null<Controller*> controller, LayerId id);
// The layer and the one under it become one picture (a "busy" cover is
// shown while the pixels are made).
void MergeLayerDown(not_null<Controller*> controller, LayerId id);
// All visible layers become one picture.
void FlattenLayers(not_null<Controller*> controller);
// Refuses to delete the last layer of a document.
void DeleteLayer(not_null<Controller*> controller, LayerId id);
// An image (or image files) from the clipboard as new layers. False with
// a toast if there is nothing to paste.
bool PasteLayerFromClipboard(not_null<Controller*> controller);

} // namespace Oblivion::Photo
