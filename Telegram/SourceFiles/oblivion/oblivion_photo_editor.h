/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "oblivion/oblivion_photo_doc.h"

#include <QtCore/QPointer>
#include <QtGui/QCursor>

class QKeyEvent;
class QPainter;

namespace Ui {
class RpWidget;
class Show;
} // namespace Ui

// Photo editor: a full-window dark "studio" layer on top of the window
// that owns the Ui::Show, built on the layered document of
// oblivion_photo_doc.h and the effects of oblivion_photo_fx.h.
//
//   +----------------------------------------------------------------+
//   | x  Photo editor  4032x3024    undo redo  compare reset ... Done |
//   +---+-------------------------------------------+----------------+
//   | t |                                           | tabs: Crop     |
//   | o |      canvas (fit, Cmd+wheel / pinch       |  Adjust Filters|
//   | o |      zoom, drag / Space+drag to pan,      |  Effects Auto  |
//   | l |      crop frame on the Crop tab,          |  Layer Tool    |
//   | s |      the overlay of the current tool)     |----------------|
//   |   |                                           | page of the tab|
//   |   | [filter strip, on the Filters tab]        |----------------|
//   |   |                                           | layers panel   |
//   +---+-------------------------------------------+----------------+
//
// Narrow windows put the panel under the canvas, the layers panel becomes
// one more tab there.
//
// What is where:
//  - Crop, Adjust, Filters, Effects and Auto edit Document::global, the
//    whole picture (all layers together), exactly as the single-image
//    editor did. The Crop page also has the canvas size of the document
//    (CanvasResized() in oblivion_photo_doc.h). The Adjust and Effects
//    pages end with a button that adds an effect of the registry to the
//    active layer (the fine adjustments / the blurs, distortions and
//    packs) and shows it on the Layer tab;
//  - the Layer tab (oblivion_photo_panels.h) edits the active layer: its
//    opacity and blend mode, the sections registered for its kind (see
//    PanelSlot::LayerProperties) and its stack of effects;
//  - the Tool tab shows the options of the current canvas tool;
//  - the tool strip switches the canvas tools registered with
//    RegisterTool(), the first one is the built-in view (pan / zoom) tool;
//  - the layers panel is whatever is registered for PanelSlot::Layers
//    (a small built-in list if nothing is).
//
// The editor never touches the session: it gets a QImage (or a Document)
// and callbacks, renders a downscaled live preview off the main thread on
// every change (latest request wins, a sharper render follows when zoomed
// in) and the full-resolution image only when the result is requested.
//
// Everything that edits goes through the Controller: it owns the document
// with its undo history, the active layer and the current tool, and gives
// access to the canvas view. Tools, panels and parameter editors get
// a Controller*, they never see the editor widget itself.
//
// Shortcuts: Cmd+Z undo, Cmd+Shift+Z / Cmd+Y redo, hold \ (backslash) to
// compare with the original, Cmd+= / Cmd+- zoom, Cmd+0 fit, double click
// on the photo toggles fit / 100% (view tool), Space+drag pans, 1-7
// switch tabs, the keys of the registered tools switch tools, Left / Right
// choose the previous / next filter on the Filters tab, Cmd+V pastes an
// image as a new layer, Cmd+Enter done, Cmd+S save to a file, Cmd+Shift+C
// copy, Esc cancels what the tool does, then closes (asks first if the
// photo was edited). Keys go to the current tool first.
namespace Oblivion::Photo {

class Controller;

enum class PhotoEditorTab : uchar {
	Crop,
	Adjust,
	Filters,
	Effects,
	Auto,
	Layer,
	Tool, // Shown only while the current tool has options.
	Layers, // Shown only in a narrow window.
};

struct PhotoEditorResult {
	QImage image; // Edited, Format_ARGB32_Premultiplied, device ratio 1.
	EditState state; // Document::global, normalized.

	// Not null only if the editor replaced the original itself ("Remove
	// background" on the Effects tab) and the document still is that one
	// image: the new original with a transparent background, the state
	// applies to it. For callers that keep (image, state) pairs.
	QImage source;

	// The whole edit with its layers. Pass it back in
	// PhotoEditorOptions::document to continue editing: it is the only
	// thing that restores the layers (state alone can't).
	std::shared_ptr<const Document> document;
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

	// A document to continue (PhotoEditorResult::document): the image
	// and the state above are ignored then.
	std::shared_ptr<const Document> document;

	// The document to continue has changes nobody got a result of (an
	// interrupted edit, see below): closing asks about them even if
	// nothing more was changed.
	bool unsaved = false;

	// The canvas tool to start with, empty: the view tool.
	QByteArray tool;

	// Not null: an editor with unsaved changes that is destroyed not by
	// the user (the passcode lock, a switch to another account, a chat
	// opened from a notification close every layer of the window) keeps
	// its document, see TakeInterruptedPhotoEdit(), and this is called
	// with the id of what was kept. It is called later, from the main
	// queue, when the editor, its window and maybe its session are gone:
	// the place to tell the user that the edit can be continued.
	Fn<void(int id)> interrupted;
};

// The edit of an editor that was closed not by the user, with all its
// layers: one, the latest, kept in the memory till it is taken, dropped
// or the application quits. Main thread.
struct InterruptedPhotoEdit {
	std::shared_ptr<const Document> document;
	QString fileName; // PhotoEditorOptions::fileName of that editor.
};
// Keeps an edit the way an interrupted editor does (replacing the one
// kept before) and returns its id: for what shows the result of an
// editor and may be closed the same way. It only remembers the document,
// so it is safe while a window is being destroyed.
int KeepInterruptedPhotoEdit(InterruptedPhotoEdit edit);
// Changes with every kept edit, 0: there is none.
[[nodiscard]] int InterruptedPhotoEditId();
// An empty document if there is none. The edit is not kept after this.
[[nodiscard]] InterruptedPhotoEdit TakeInterruptedPhotoEdit();
// id == 0 drops whatever is kept, otherwise only the edit with this id.
void DropInterruptedPhotoEdit(int id = 0);

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

//
// Canvas tools.
//

struct ToolMouseEvent {
	QPointF document; // Canvas (document) coordinates, see the doc header.
	QPointF widget; // Logical pixels of the canvas widget.
	Qt::MouseButton button = Qt::NoButton; // Press / release: which one.
	Qt::MouseButtons buttons;
	Qt::KeyboardModifiers modifiers;
	// Widget pixels per document pixel now: divide a handle size or
	// a hit distance in screen pixels by it to get document pixels.
	double scale = 1.;
};

struct ToolWheelEvent {
	QPointF document;
	QPointF widget;
	QPointF angleDelta; // As QWheelEvent gives it (120 is one notch).
	QPointF pixelDelta; // Not null for a trackpad.
	Qt::KeyboardModifiers modifiers;
	double scale = 1.;
};

struct ToolPaintContext {
	// Document coordinates -> widget coordinates (zoom, pan and the
	// whole-image rotation / crop are in it). Map points with it and draw
	// in widget pixels, so lines and handles keep their thickness.
	QTransform documentToWidget;
	double scale = 1.; // Widget pixels per document pixel.
	QSize documentSize;
	QRect widgetRect; // The whole canvas widget.
};

// A canvas tool: the object that gets the mouse and the keys while it is
// the current one and paints its overlay over the picture. Drawing,
// transforming a layer, painting a mask, a mesh warp, the splitters of
// a collage are tools. A tool is created by the factory of its
// ToolDescriptor when the user chooses it and destroyed when another one
// is chosen (deactivated() is called first). It works with the document
// through the Controller it was created with:
//
//  - read controller->document() / activeLayer() when an action starts;
//  - while the mouse moves either only repaint the overlay
//    (controller->updateCanvas(), the cheap way for a stroke in progress)
//    or apply a changed copy of the document without committing
//    (controller->apply(document, false), a live preview);
//  - when the action is complete apply the final document with
//    commit = true: that is one undo step.
//
// All methods are called on the main thread.
class Tool {
public:
	virtual ~Tool() = default;

	virtual void activated() {
	}
	// Finish or drop what is in progress, the object is destroyed next.
	virtual void deactivated() {
	}

	// Left button. Return true to take the press: the moves and the
	// release then come here. false leaves it to the canvas (panning).
	virtual bool mousePress(const ToolMouseEvent &e) {
		return false;
	}
	// With buttons while a taken press lasts, without them it is a hover.
	virtual void mouseMove(const ToolMouseEvent &e) {
	}
	virtual void mouseRelease(const ToolMouseEvent &e) {
	}
	// false: the double click is delivered as one more mousePress().
	virtual bool mouseDoubleClick(const ToolMouseEvent &e) {
		return false;
	}
	virtual void mouseLeave() {
	}
	// true: handled, the canvas doesn't scroll / zoom.
	virtual bool wheel(const ToolWheelEvent &e) {
		return false;
	}
	// Keys come here before the editor shortcuts. true: handled.
	virtual bool keyPress(not_null<QKeyEvent*> e) {
		return false;
	}
	virtual bool keyRelease(not_null<QKeyEvent*> e) {
		return false;
	}
	// Escape: true if there was something to cancel (the editor then
	// neither drops the tool nor closes).
	virtual bool cancel() {
		return false;
	}

	// The overlay, painted over the picture in widget coordinates.
	virtual void paint(QPainter &p, const ToolPaintContext &context) {
	}
	[[nodiscard]] virtual QCursor cursor(const ToolMouseEvent &e) {
		return QCursor(Qt::ArrowCursor);
	}
};

struct ToolDescriptor {
	QByteArray id; // Stable, [a-z0-9_.].
	FxText name; // The tooltip of the tool strip button.
	int key = 0; // Qt::Key of the shortcut without modifiers, 0: none.
	int order = 0; // In the strip, then the registration order.

	// The icon: draw with the color inside rect (24 x 24 logical pixels,
	// antialiasing is on).
	Fn<void(QPainter &p, QRectF rect, QColor color)> paintIcon;

	Fn<std::unique_ptr<Tool>(not_null<Controller*> controller)> create;

	// Optional: false dims the button (for example "needs a drawing
	// layer"). Checked after every document / active layer change.
	Fn<bool(not_null<const Controller*> controller)> available;

	// Optional: the options of the tool, shown in the Tool tab while
	// the tool is current (laid out by resizeToWidth(), height by
	// heightValue()). Use the EditorUi controls and the parameter rows
	// of oblivion_photo_panels.h, the panel is dark in every theme.
	Fn<object_ptr<Ui::RpWidget>(
		not_null<QWidget*> parent,
		not_null<Controller*> controller)> options;
};

// The id of the built-in pan / zoom tool (no Tool object).
inline const auto kViewTool = QByteArray("view");

//
// Panels.
//

enum class PanelSlot : uchar {
	// The layers list in the bottom part of the side panel. The widget
	// gets a fixed geometry and scrolls its content itself. The last
	// registered one wins. See PanelDescriptor::height.
	Layers,
	// A section of the Layer tab above the effects, shown while visible()
	// is true for the active layer (the settings of a collage, of a text
	// layer...). Laid out by resizeToWidth(), height by heightValue().
	LayerProperties,
};

struct PanelDescriptor {
	QByteArray id;
	PanelSlot slot = PanelSlot::LayerProperties;
	int order = 0;
	FxText title; // LayerProperties: the section title, may be empty.
	// LayerProperties: is the section shown now? Checked after every
	// document / active layer change, keep it cheap.
	Fn<bool(not_null<const Controller*> controller)> visible;
	Fn<object_ptr<Ui::RpWidget>(
		not_null<QWidget*> parent,
		not_null<Controller*> controller)> create;
	// Layers, optional: the height the panel needs to show everything it
	// has now (its header and all the rows). The slot is not made taller
	// than that, so a single layer leaves the room to the pages above.
	// Without it the slot always takes its full part of the side panel.
	// Checked after every document change, keep it cheap.
	Fn<int(not_null<const Controller*> controller)> height;
};

// A kind of layers: how layers with this LayerContent::type() are named
// and drawn in lists, and how the "+" menu of a layers panel adds one.
// The image kind ("image", adding asks for files) is built in, the
// drawing, text and collage modules register theirs.
struct LayerKindDescriptor {
	QByteArray type; // LayerContent::type().
	FxText name; // "Drawing": the item of the "+" menu, the list tooltip.
	int order = 0;
	// 24 x 24 logical pixels, like ToolDescriptor::paintIcon.
	Fn<void(QPainter &p, QRectF rect, QColor color)> paintIcon;
	// Adds a new layer of this kind (usually Controller::addLayer() with
	// an empty content of the canvas size), null: not offered in menus.
	Fn<void(not_null<Controller*> controller)> create;
};

// Registration of tools, panels and layer kinds, from the callback of
// a static EditorRegistrar in the module's own .cpp (it runs once, on the
// main thread, when the first editor is created):
//
//   const auto Registered = Oblivion::Photo::EditorRegistrar([] {
//       Oblivion::Photo::RegisterTool({
//           .id = "draw.pen",
//           .name = tr::lng_oblivion_photo_draw_pen,
//           .key = Qt::Key_B,
//           .order = 20,
//           .paintIcon = PaintPenIcon,
//           .create = [](not_null<Controller*> controller) {
//               return std::make_unique<PenTool>(controller);
//           },
//           .options = CreatePenOptions,
//       });
//   });
void RegisterTool(ToolDescriptor &&descriptor);
void RegisterPanel(PanelDescriptor &&descriptor);
void RegisterLayerKind(LayerKindDescriptor &&descriptor);

class EditorRegistrar final {
public:
	explicit EditorRegistrar(Fn<void()> registerAll);

	EditorRegistrar(const EditorRegistrar &other) = delete;
	EditorRegistrar &operator=(const EditorRegistrar &other) = delete;

};

// Main thread.
[[nodiscard]] const std::vector<const ToolDescriptor*> &AllTools();
[[nodiscard]] const ToolDescriptor *FindTool(QByteArrayView id);
[[nodiscard]] const std::vector<const PanelDescriptor*> &AllPanels();
[[nodiscard]] const std::vector<const LayerKindDescriptor*> &AllLayerKinds();
[[nodiscard]] const LayerKindDescriptor *FindLayerKind(QByteArrayView type);

// A picture loaded for the editor (EXIF orientation applied, sRGB).
struct ImportedImage {
	QImage image;
	QString name; // The file name without the extension.
};

//
// The controller.
//

// The state of one open editor. Main thread only. Every pointer to it
// that may outlive the editor (an async callback) must be guarded:
// crl::guard(controller, ...) / base::make_weak(controller).
class Controller final : public base::has_weak_ptr {
public:
	explicit Controller(std::shared_ptr<Ui::Show> show);
	~Controller();

	// The document. document() is the working copy: what is on the
	// screen, including changes that are not an undo step yet.
	[[nodiscard]] const Document &document() const;
	[[nodiscard]] bool hasDocument() const;
	// Fires after every change of document(): an edit (live or
	// committed), undo, redo, a loaded document.
	[[nodiscard]] rpl::producer<> documentChanges() const;
	// Grows with every change of document().
	[[nodiscard]] uint64 revision() const;
	// Grows only when the canvas size or the layers changed (not for
	// Document::global): what layer lists and thumbnails watch.
	[[nodiscard]] uint64 layersRevision() const;

	// Replaces the working document. commit == true makes it an undo
	// step right away; false is a live change (a slider being dragged,
	// a layer being moved): the screen follows, and the step is made by
	// the next commit(), by an apply() with commit == true or by itself
	// after a short pause. Equal documents change nothing.
	void apply(Document document, bool commit = true);
	// Copies the document, lets modify change the copy, applies it.
	void change(Fn<void(Document&)> modify, bool commit = true);
	// The same for one layer / one effect, nothing if it is not there.
	void changeLayer(LayerId id, Fn<void(Layer&)> modify, bool commit = true);
	void changeFx(
		LayerId id,
		uint64 uid,
		Fn<void(FxInstance&)> modify,
		bool commit = true);
	void commit();

	[[nodiscard]] bool canUndo() const;
	[[nodiscard]] bool canRedo() const;
	void undo();
	void redo();
	[[nodiscard]] rpl::producer<> historyChanges() const;

	// Loads a document: the history starts from it, modified() is false.
	void setDocument(Document document);
	[[nodiscard]] const Document &initialDocument() const;
	[[nodiscard]] bool modified() const;

	// The active layer: the one the Layer tab and the tools work with.
	// 0 only for a document without layers. It stays valid: when the
	// layer disappears (undo, delete) the nearest one becomes active.
	[[nodiscard]] LayerId activeLayerId() const;
	[[nodiscard]] const Layer *activeLayer() const;
	void setActiveLayer(LayerId id);
	[[nodiscard]] rpl::producer<LayerId> activeLayerValue() const;

	// Common operations, each one undo step.
	// Adds the layer above the active one and makes it active.
	LayerId addLayer(Layer layer);
	// An image layer scaled down to fit the canvas, centered.
	LayerId addImageLayer(QImage image, QString name);
	void removeLayer(LayerId id);
	LayerId duplicateLayer(LayerId id);
	uint64 addFx(LayerId id, FxInstance instance);

	// A parameter of an effect instance, for parameter editors that work
	// on the canvas. An empty value if it is not there.
	[[nodiscard]] FxValue fxParam(
		LayerId id,
		uint64 uid,
		QByteArrayView param) const;
	void setFxParam(
		LayerId id,
		uint64 uid,
		const QByteArray &param,
		FxValue value,
		bool commit);

	// Importing pictures as new layers (the file dialog allows several
	// files, they are loaded off the main thread).
	void chooseAndImport();
	void importFiles(const QStringList &paths);
	// The same loading for other uses (the cells of a collage, replacing
	// the picture of a layer): done gets the pictures that could be
	// loaded, it is not called if there are none (a toast says so) or
	// the editor was closed meanwhile.
	void chooseImages(
		Fn<void(std::vector<ImportedImage> images)> done,
		bool multiple = true);
	void loadImages(
		const QStringList &paths,
		Fn<void(std::vector<ImportedImage> images)> done);

	// Slow work with the "busy" cover over the editor: work runs on
	// a worker thread, a returned document is applied as one undo step
	// (std::nullopt changes nothing). Nothing is applied if the document
	// changed meanwhile. For merging, flattening, anything that makes
	// pixels.
	void runBusy(
		QString text,
		Fn<std::optional<Document>(const Document &document)> work,
		Fn<void(bool applied)> done = nullptr);
	[[nodiscard]] rpl::producer<QString> busyValue() const; // Empty: no.
	[[nodiscard]] bool busy() const;

	// Tools. The current tool is the one chosen in the strip (toolId(),
	// kViewTool has no object) unless a temporary tool is set over it:
	// a tool that is not in the strip and lives until it is cleared, the
	// strip tool is changed or Escape is pressed (finished is called
	// then). Parameter editors use temporary tools to pick a point or to
	// edit a mesh on the canvas.
	[[nodiscard]] QByteArray toolId() const;
	void setTool(const QByteArray &id);
	[[nodiscard]] rpl::producer<QByteArray> toolValue() const;
	[[nodiscard]] Tool *tool() const; // Temporary or strip tool, or null.
	void setTemporaryTool(
		std::unique_ptr<Tool> tool,
		Fn<void()> finished = nullptr);
	void clearTemporaryTool();
	[[nodiscard]] bool hasTemporaryTool() const;
	[[nodiscard]] rpl::producer<> toolChanges() const; // Any of the two.
	// Asks the editor to show the Tool tab with the options of the
	// current tool (choosing the tool that is current already changes
	// nothing, so a "set up..." button of a layer section calls this).
	void showToolOptions();
	[[nodiscard]] rpl::producer<> toolOptionsRequests() const;
	// Asks the editor to show the Layer tab scrolled to the top of the
	// last effect card: the effect that was just added to the active
	// layer, with its header.
	void showLayerEffects();
	[[nodiscard]] rpl::producer<> layerEffectsRequests() const;
	// Asks the editor to scroll the page of the side panel the widget is
	// on so that the widget is in view: for a section that shows up under
	// the visible part of the page after something was chosen on the
	// canvas (the settings of a collage cell). Nothing happens if the
	// widget is not on the page that is shown.
	void revealInPanel(not_null<QWidget*> widget);
	[[nodiscard]] rpl::producer<QPointer<QWidget>> revealRequests() const;

	// The canvas view. Identity / 1 while there is no canvas (a panel
	// shown alone).
	[[nodiscard]] QTransform documentToWidget() const;
	[[nodiscard]] QTransform widgetToDocument() const;
	[[nodiscard]] double viewScale() const; // Widget px per document px.
	[[nodiscard]] rpl::producer<> viewChanges() const; // Zoom, pan, size.
	void updateCanvas(); // Repaints the canvas with the tool overlay.

	// The revision() of the document the picture on the canvas was
	// rendered from, 0 while nothing was shown (or there is no canvas).
	// A tool that keeps painting what it has just put into the document
	// over the canvas (a stroke) stops when this reaches the revision the
	// document got with it: the preview has it from then on.
	[[nodiscard]] uint64 shownRevision() const;
	[[nodiscard]] rpl::producer<uint64> shownRevisionValue() const;

	// True while a tool holds the mouse (a drag, a stroke). Live changes
	// applied meanwhile don't become undo steps by themselves, however
	// long the mouse rests: the step is made by the commit on release (or
	// by the usual pause after it).
	[[nodiscard]] bool interacting() const;

	// The compositor of the previews, shared so that tools and panels
	// reuse its caches: layerPixels() for thumbnails and hit tests. Use
	// it on worker threads only.
	[[nodiscard]] std::shared_ptr<Compositor> compositor() const;
	// A picture of one layer (content, effects, mask; not transformed)
	// that fits into size (device pixels), delivered on the main thread
	// unless the controller is gone. It is scaled down so that thin
	// things stay visible: a few strokes on a large drawing layer are
	// still lines in it, not an empty square.
	void requestLayerThumbnail(
		LayerId id,
		QSize size,
		Fn<void(QImage image)> done);
	// True while some of the requested thumbnails are not delivered yet.
	[[nodiscard]] bool thumbnailsPending() const;

	[[nodiscard]] std::shared_ptr<Ui::Show> uiShow() const;
	void showToast(const QString &text);

	[[nodiscard]] rpl::lifetime &lifetime();

	// For the editor shell.
	struct View {
		Fn<QTransform()> documentToWidget;
		Fn<void()> update;
	};
	void setView(View view);
	void notifyViewChanged();
	void setShownRevision(uint64 revision);
	void setInteracting(bool interacting);
	void shutdown(); // Destroys the tools, nothing is applied after it.

private:
	void changed(bool layers);
	void validateActiveLayer();
	void addImported(std::vector<ImportedImage> images);
	void placeAdded();

	const std::shared_ptr<Ui::Show> _show;
	const std::shared_ptr<Compositor> _compositor;
	History _history;
	Document _document;
	Document _initial;
	bool _hasDocument = false;
	bool _shutdown = false;
	uint64 _revision = 0;
	uint64 _layersRevision = 0;
	base::Timer _commitTimer;

	rpl::variable<LayerId> _activeLayer = LayerId(0);
	rpl::variable<QByteArray> _toolId;
	std::unique_ptr<Tool> _tool;
	std::unique_ptr<Tool> _temporaryTool;
	Fn<void()> _temporaryFinished;
	rpl::variable<QString> _busy;
	rpl::variable<uint64> _shownRevision = uint64(0);
	bool _interacting = false;
	int _thumbnailsPending = 0;
	View _view;

	rpl::event_stream<> _documentChanges;
	rpl::event_stream<> _historyChanges;
	rpl::event_stream<> _toolChanges;
	rpl::event_stream<> _toolOptionsRequests;
	rpl::event_stream<> _layerEffectsRequests;
	rpl::event_stream<QPointer<QWidget>> _revealRequests;
	rpl::event_stream<> _viewChanges;
	rpl::lifetime _lifetime;

};

// The localized names the panels of other modules need. Main thread.
[[nodiscard]] QString BlendModeName(BlendMode mode);
[[nodiscard]] QString FxGroupName(FxGroup group);
// "Layer 3": a default name that is not used in the document yet.
[[nodiscard]] QString NewLayerName(const Document &document);

//
// UI snapshot scenes (OBLIVION_SELFTEST=ui, oblivion_ui_snapshots.h).
//

// Pictures made in code (no files, no session).
[[nodiscard]] QImage SampleSceneImage();
// The sample photo with two more layers: a smaller picture (multiply,
// with an effect) and a masked one.
[[nodiscard]] Document SampleSceneDocument();

// The whole editor in a given state. Call it from the callback of
// a SelfTest::SceneRegistrar. The document is loaded synchronously, then
// prepare runs: choose the active layer, a tool, apply changes.
struct EditorSceneArgs {
	QString name; // [a-z0-9_], "photo_draw_pen".
	QSize size; // Empty: 1120 x 720.
	Fn<Document()> document; // Null: the sample photo alone.
	PhotoEditorTab tab = PhotoEditorTab::Layer;
	QByteArray tool; // Empty: the view tool.
	Fn<void(not_null<Controller*> controller)> prepare;
	crl::time wait = 0; // 0: the default of the editor scenes.
};
void RegisterEditorScene(EditorSceneArgs &&args);

// One panel alone on the dark panel background, with a controller that
// has no canvas. The widget is laid out by resizeToWidth(size.width())
// unless size has a height.
struct PanelSceneArgs {
	QString name;
	QSize size; // A zero height: as tall as the panel.
	Fn<Document()> document; // Null: SampleSceneDocument().
	Fn<void(not_null<Controller*> controller)> prepare; // Before create.
	Fn<object_ptr<Ui::RpWidget>(
		not_null<QWidget*> parent,
		not_null<Controller*> controller)> create;
};
void RegisterPanelScene(PanelSceneArgs &&args);

} // namespace Oblivion::Photo
