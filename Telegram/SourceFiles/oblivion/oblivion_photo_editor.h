/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "oblivion/oblivion_photo_core.h"

namespace Ui {
class RpWidget;
class Show;
} // namespace Ui

// Photo editor: a full-window dark "studio" layer on top of the window
// that owns the Ui::Show, built on the non-destructive Oblivion::Photo core.
//
//   +--------------------------------------------------------------+
//   | x  Photo editor   4032x3024     undo redo  compare reset ...  Done |
//   +---------------------------------------------+----------------+
//   |                                             | tabs           |
//   |        canvas (fit, Cmd+wheel / pinch        |  Crop Adjust   |
//   |        zoom, drag / Space+drag to pan,      |  Filters       |
//   |        crop frame on the Crop tab)          |  Effects Auto  |
//   |                                             |----------------|
//   | [filter strip, on the Filters tab]          | tool controls  |
//   +---------------------------------------------+----------------+
//
// Narrow windows put the tool panel under the canvas.
//
// The editor never touches the session: it gets a QImage and callbacks,
// renders a downscaled live preview off the main thread on every change
// (latest request wins, a sharper render follows when zoomed in) and the
// full-resolution image only when the result is requested.
//
// "Remove background" on the Effects tab (macOS 14+, oblivion_vision.h)
// cuts the subject out of the original: the original is replaced with the
// cutout, every edit keeps working over it, the button brings
// the background back. It is not a part of EditState, see
// PhotoEditorResult::source.
//
// Shortcuts: Cmd+Z undo, Cmd+Shift+Z / Cmd+Y redo, hold \ (backslash) to
// compare with the original, Cmd+= / Cmd+- zoom, Cmd+0 fit, double click
// on the photo toggles fit / 100%, Space+drag pans, 1-5 switch tabs,
// Left / Right choose the previous / next filter on the Filters tab,
// Cmd+Enter done, Cmd+S save to a file, Cmd+Shift+C copy, Esc closes
// (asks first if the photo was edited).
namespace Oblivion::Photo {

enum class PhotoEditorTab : uchar {
	Crop,
	Adjust,
	Filters,
	Effects,
	Auto,
};

struct PhotoEditorResult {
	QImage image; // Edited, Format_ARGB32_Premultiplied, device ratio 1.
	EditState state; // Normalized, can be passed back in the options.

	// Not null only if the editor replaced the original itself ("Remove
	// background" on the Effects tab): the new original with a transparent
	// background, the state applies to it. To continue editing pass this
	// image instead of the one the editor was opened with.
	QImage source;
};

// An extra item of the "More" menu of the editor.
struct PhotoEditorAction {
	QString text;
	// Called on the main thread with the full-resolution result.
	Fn<void(PhotoEditorResult result)> callback;
	// Optional menu icon, a mediaviewMenuFg-colored one fits the dark menu
	// (for example st::mediaMenuIconForward from styles/style_menu_icons.h).
	const style::icon *icon = nullptr;
	// Close the editor before calling the callback.
	bool closeEditor = true;
};

struct PhotoEditorOptions {
	// Top bar title, empty: "Photo editor".
	QString title;

	// Base name for "Save to file" (without an extension), empty: "photo".
	QString fileName;

	// The edit to start from, for example the state of a previous result
	// (the image must then be the same original, not the edited result).
	EditState state;

	PhotoEditorTab tab = PhotoEditorTab::Adjust;

	// The main button. Without a done callback the main button saves the
	// result to a file (doneText is then ignored), useful for editing
	// local files. With it the editor closes and calls done.
	QString doneText; // Empty: "Done".
	Fn<void(PhotoEditorResult result)> done;

	// Called when the editor is closed without done / a closing action.
	Fn<void()> cancelled;

	// Extra items of the "More" menu, after the built-in ones.
	std::vector<PhotoEditorAction> actions;

	// Built-in "More" menu items (and their shortcuts).
	bool allowSaveToFile = true;
	bool allowCopy = true;

	// Limits the size of the result image (keeps the aspect ratio, never
	// upscales), empty: full resolution. The preview is not affected.
	QSize maxOutputSize;
};

// Shows the editor as a full-window layer through show (a window show,
// like controller->uiShow(), or any Ui::Show that supports layers).
// A null image shows an error toast instead. Main thread.
void ShowPhotoEditor(
	std::shared_ptr<Ui::Show> show,
	QImage image,
	PhotoEditorOptions options = {});

// Loads the file off the main thread (EXIF orientation, sRGB, see
// LoadImage) and shows the editor, or an error toast. An empty
// options.fileName is taken from the file name.
void ShowPhotoEditorForFile(
	std::shared_ptr<Ui::Show> show,
	QString path,
	PhotoEditorOptions options = {});

// Asks for an image file, then ShowPhotoEditorForFile(). With the default
// options the main button saves the result to a new file.
void ChoosePhotoToEdit(
	std::shared_ptr<Ui::Show> show,
	PhotoEditorOptions options = {});

// The editor widget itself, without the layer: for embedding it in
// another container (a separate window...). It fills its parent area when
// resized by the owner. closeRequested is called when the editor wants to
// be closed (after done / cancel handling), the owner destroys it then.
[[nodiscard]] not_null<Ui::RpWidget*> CreatePhotoEditorWidget(
	not_null<QWidget*> parent,
	std::shared_ptr<Ui::Show> show,
	QImage image,
	PhotoEditorOptions options,
	Fn<void()> closeRequested);

} // namespace Oblivion::Photo
