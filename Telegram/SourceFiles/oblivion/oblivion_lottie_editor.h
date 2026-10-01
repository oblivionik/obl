/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "base/unique_qptr.h"
#include "oblivion/oblivion_lottie_doc.h"
#include "ui/rp_widget.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/rp_window.h"
#include "ui/widgets/tooltip.h"

class DocumentData;

namespace Data {
struct FileOrigin;
struct UniqueGift;
} // namespace Data

namespace Ui {
class GenericBox;
class LayerManager;
class PopupMenu;
class RoundButton;
class Show;
} // namespace Ui

// Lottie editor: a separate resizable top-level window ("a program inside
// the app") around one EditorController (see oblivion_lottie_doc.h).
//
// Window layout (EditorWidget):
//
//   +------------------------------------------------------------------+
//   | New Open Save Export | Undo Redo | 512x512 · 60 fps · 3 s | - % + |
//   +---------------+----------------------------------+---------------+
//   | LayersPanel   |           CanvasPanel            | InspectorPanel|
//   |   (left)      |            (center)              |    (right)    |
//   +---------------+----------------------------------+---------------+
//   |                       TimelinePanel (bottom)                     |
//   +------------------------------------------------------------------+
//
// The side panels and the timeline are resized with draggable splitters
// (sizes are kept for the app session, a double click on a splitter
// restores the default size). The toolbar and the splitters belong to
// EditorWidget, everything else to the four panels (the panels' own docs
// are at the top of their headers).
//
// Entry points: Settings > Oblivion > Tools > "Lottie editor" (file
// dialog, ShowLottieEditorImport), "Lottie editor" in the context menu of
// animated stickers (oblivion_message_tools.cpp) and of unique gifts
// (model / symbol, info_peer_gifts_widget.cpp), both download first
// (ShowLottieEditorFor), and the sticker studio's "Lottie editor" button
// (the animation with the studio's colors, ShowLottieEditor).
//
// Panel contract (oblivion_lottie_editor_{canvas,layers,inspector,
// timeline}.h, each filled by its own UI owner):
//  - a Ui::RpWidget subclass constructed as
//      Panel(QWidget *parent, not_null<EditorController*> controller);
//    the controller outlives the panel;
//  - EditorWidget sets the geometry, the panel lays its content out in
//    resizeEvent() / sizeValue() and paints its own background (the
//    splitter lines between panels are painted by EditorWidget);
//  - all state lives in the controller: read it from there, observe its
//    rpl signals (documentChanged, selectionChanged, currentFrameValue,
//    playingValue, zoomValue...), change it only through its methods, so
//    that every edit is undoable and every panel stays in sync;
//  - Qt::ClickFocus: key presses a panel doesn't accept propagate to
//    EditorWidget, which handles the global shortcuts listed below, so a
//    panel may override any of them while it has focus by accepting the
//    key event (for example Left / Right in the timeline);
//  - no Main::Session, Window::SessionController or Core::App() window
//    APIs: the editor must work in the snapshot tests without a session.
//    Boxes and toasts go through controller->uiShow() (null in unit
//    tests, check it). File dialogs / saving: controller->requestAction();
//  - heavy work (rendering, validation, big edits) runs off the main
//    thread: FrameRenderer below, crl::async + crl::guard for the rest;
//  - texts: the localized helpers below name nodes, layer / shape types,
//    properties, easings, mattes, color kinds, commands and TGS issues.
//    Ready lang keys (Russian rows exist): lng_oblivion_lottie_editor_
//    {layers,properties,timeline,canvas} (panel titles), _play, _pause,
//    _frame ("Frame {value}"), _frame_of ("Frame {value} of {total}"),
//    _selected ("Selected: {value}"), _nothing_selected, _no_layers,
//    _validate_fix. Add other keys in your own lang block;
//  - TGS check: the toolbar shows the status (EditorWidget::validation(),
//    re-run async after every change), the full list with fixes is
//    ShowValidationBox(controller); a panel that shows issues itself runs
//    Validate(controller->document()) in crl::async.
//
// Global shortcuts (EditorWidget::handleKey, Cmd is Ctrl on other OS):
//   Cmd+Z undo, Cmd+Shift+Z / Cmd+Y redo, Space play / pause,
//   Left / Right previous / next frame (Shift: 10 frames), Home / End
//   first / last frame, Delete / Backspace delete the selected keyframes
//   or nodes, Cmd+D duplicate, Esc clears the keyframe selection, then
//   the node selection (it never closes the window), Cmd+N new, Cmd+O
//   open, Cmd+S save, Cmd+Shift+S save as, Cmd+E export .tgs, Cmd+= /
//   Cmd+- zoom in / out, Cmd+0 fit (zoom 1, no pan), Cmd+W close,
//   Alt+arrows move the selection by 1 px (Shift: 10 px), J / K previous /
//   next keyframe, Cmd+C / Cmd+V copy / paste the selected keyframes (at
//   the current frame).
// Text fields get every key first: a key they accept (typing, Space,
// arrows, Delete, their own Cmd+Z...) never reaches these shortcuts. The
// editor claims its modified keys in ShortcutOverride, so the app-wide
// shortcuts of the main window (Alt+Up / Alt+Down "next chat", Cmd+0
// "Saved Messages"...) don't take them while the editor is focused.
// Editing keys (undo / redo, delete, duplicate, paste, nudge) are ignored
// while a mouse button is held, so they can't interfere with a drag.
//
// Selection model (EditorController): node selection, selected keyframes
// and the active property. Changing the node selection drops keyframes /
// the active property of unrelated nodes, so every panel shows the same
// thing and Delete / paste act on what is visible.
//
// Toolbar: New Open Save Export (.tgs / .json / current frame as PNG or
// SVG) | Undo Redo | name · info | canvas background, zoom out, zoom %
// (menu), zoom in | TGS check status (opens the check box) | done.
namespace Oblivion::LottieEdit {

class CanvasPanel;
class LayersPanel;
class InspectorPanel;
class TimelinePanel;
class EditorWindow;

struct EditorArgs {
	QByteArray data; // .tgs or Lottie JSON, empty for a new animation.
	QString name; // Without an extension, empty: from path or default.
	QString path; // Local file "Save" writes to, empty: asks on save.

	// Optional extra action (for example "Send" when opened from a chat):
	// a button with doneText in the toolbar, called with the .tgs bytes
	// and the name of the current document.
	QString doneText;
	Fn<void(QByteArray tgs, QString name)> done;
};

// Opens a new editor window (never replaces an open one) and activates
// it. If data can't be parsed the window opens with a new blank animation
// and shows an error toast. A path that is already open in another editor
// window activates that window instead (and returns it).
not_null<EditorWindow*> OpenLottieEditor(EditorArgs &&args);

// Controller-free entry points, main thread.
void ShowLottieEditor(QByteArray lottieData, QString name);
void ShowLottieEditorImport(); // File dialog, then OpenLottieEditor().

// A Lottie document from a chat or a gift (animated sticker, gift model or
// pattern, a .tgs / .json file): downloads it first if needed (a small
// progress box in show), then opens it in a new editor window. Toasts for
// documents that are not Lottie animations (video / static stickers).
void ShowLottieEditorFor(
	std::shared_ptr<Ui::Show> show,
	not_null<DocumentData*> document,
	Data::FileOrigin origin,
	QString name = QString());

// "Lottie editor" submenu (model / symbol) for a unique gift.
void AddGiftLottieEditorActions(
	not_null<Ui::PopupMenu*> menu,
	std::shared_ptr<Ui::Show> show,
	std::shared_ptr<Data::UniqueGift> unique);

// For Core::Application::preventsQuit(): if an editor window has unsaved
// changes, activates it, asks Save / Don't save / Cancel and returns true.
// Once the window is saved or discarded, the quit is attempted again
// (asking about the next window with changes, if any). While the windows
// are hidden by a lock it asks in the main window instead, without
// showing them: quit without saving or cancel to unlock and save first.
[[nodiscard]] bool PreventsQuit();

// Passcode / setup email lock (Core::Application): every editor window is
// hidden (keeping its document, history and boxes) and shown again by
// RestoreWindowsAfterLock(), windows opened meanwhile stay hidden too.
void HideWindowsForLock();
void RestoreWindowsAfterLock();

// Destroys every editor window without asking (app shutdown).
void CloseAllWindows();

// "Telegram sticker check" box: issues with "Fix" / "Select" actions.
// Uses controller->uiShow(), does nothing without it.
void ShowValidationBox(not_null<EditorController*> controller);
void ValidationBox(
	not_null<Ui::GenericBox*> box,
	not_null<EditorController*> controller);

// Localized texts, main thread.
//
// NodeDisplayName(): "nm", or a type based fallback like "Group 2"
// (1-based index in the parent) for nodes without a name.
[[nodiscard]] QString NodeDisplayName(const Document &document, NodeId id);
[[nodiscard]] QString NodeKindText(NodeKind kind);
[[nodiscard]] QString LayerTypeText(LayerType type);
[[nodiscard]] QString ShapeTypeText(ShapeType type);
// Layer type for layers, shape type for shape items, kind for the rest.
[[nodiscard]] QString NodeTypeText(const NodeInfo &node);
[[nodiscard]] QString PropertyRoleText(PropertyRole role);
// Role text, the effect value name for effects, dash / gap / offset.
[[nodiscard]] QString PropertyText(const PropertyInfo &info);
[[nodiscard]] QString EasingPresetText(EasingPreset preset);
[[nodiscard]] QString ColorKindText(ColorKind kind);
[[nodiscard]] QString MatteModeText(MatteMode mode);
[[nodiscard]] QString CommandText(Command command); // "Renaming".
[[nodiscard]] QString UndoText(Command command); // "Undo: renaming".
[[nodiscard]] QString RedoText(Command command);
[[nodiscard]] QString IssueText(const Document &document, const Issue &issue);
[[nodiscard]] QString IssueFixText(IssueType type); // Empty if not fixable.

// Numbers for the UI: up to `decimals` digits after the separator
// ("," in Russian), trailing zeros removed, "-0" printed as "0".
[[nodiscard]] QString FormatDecimal(double value, int decimals = 2);
[[nodiscard]] QString FormatFps(double fps); // "60 fps".
[[nodiscard]] QString FormatSeconds(double seconds); // "2,5 s".
[[nodiscard]] QString FormatKilobytes(int64 bytes); // "12,3 KB".
[[nodiscard]] QString FormatCanvasSize(QSize size); // "512×512".
// "512×512 · 60 fps · 3 s" of the document.
[[nodiscard]] QString DocumentInfoText(const Document &document);

// "Undo (⌘Z)" style tooltip text with a platform shortcut.
[[nodiscard]] QString WithShortcut(
	const QString &action,
	const QKeySequence &keys);

// Renders frames of documents off the main thread with one rlottie
// instance (Oblivion::Lottie::Renderer), rebuilt only when the document
// changes. Requests are coalesced: while a frame renders only the newest
// request waits, older pending ones are dropped (their callbacks never
// run), so it can be fed on every mouse move. The callback runs on the
// main thread, never after the renderer is destroyed. Main thread API.
class FrameRenderer final {
public:
	FrameRenderer();
	FrameRenderer(const FrameRenderer &other) = delete;
	FrameRenderer &operator=(const FrameRenderer &other) = delete;
	~FrameRenderer();

	// frame: index in [0, document.frames()) (EditorController's
	// frameIndex()), size in device pixels. The image is ARGB32
	// premultiplied with a transparent background, the composition is
	// fit and centered like Oblivion::Lottie::RenderFrame(); null image
	// if the document can't be rendered.
	void request(
		const Document &document,
		int frame,
		QSize size,
		Fn<void(QImage image)> done);

	// Drops the pending request (a render in progress still finishes,
	// its callback is not called).
	void cancel();

	struct Shared;

private:
	static void RenderJobs(const std::shared_ptr<Shared> &shared);

	const std::shared_ptr<Shared> _shared;

};

// An icon button with a tooltip, sized like the editor toolbar buttons
// (36x36, 24px icons from menu/*-24x24). For panel toolbars too.
class ToolButton final
	: public Ui::IconButton
	, public Ui::AbstractTooltipShower {
public:
	ToolButton(
		QWidget *parent,
		const style::icon &icon,
		rpl::producer<QString> tooltip);

	void setTooltip(QString text);

	// Unavailable buttons are disabled and drawn with a faded icon.
	void setAvailable(bool available);

	QString tooltipText() const override;
	QPoint tooltipPos() const override;
	bool tooltipWindowActive() const override;

protected:
	void enterEventHook(QEnterEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	void refreshLook();

	QString _tooltip;
	bool _available = true;

};

// Small vector icons for the editor controls the app has no icons for
// (frame stepping, loop, canvas backgrounds, zoom, keyframes). Drawn in
// the given color, so they follow the palette.
enum class Glyph : uchar {
	First, // |<<
	Previous, // <|
	Play,
	Pause,
	Next, // |>
	Last, // >>|
	Loop,
	ZoomIn,
	ZoomOut,
	Fit,
	BackgroundChecker,
	BackgroundDark,
	BackgroundLight,
	Keyframe,
};

// Paints the glyph centered in rect (designed for a 24x24 area).
void PaintGlyph(QPainter &p, Glyph glyph, const QRectF &rect, QColor color);

// A square button with a painted glyph and a tooltip, 32x32 by default.
// Active buttons (toggles that are on) use the accent colors.
class GlyphButton final
	: public Ui::AbstractButton
	, public Ui::AbstractTooltipShower {
public:
	GlyphButton(
		QWidget *parent,
		Glyph glyph,
		rpl::producer<QString> tooltip,
		int size = 0);

	void setGlyph(Glyph glyph);
	void setActive(bool active);
	void setTooltip(QString text);

	QString tooltipText() const override;
	QPoint tooltipPos() const override;
	bool tooltipWindowActive() const override;

protected:
	void paintEvent(QPaintEvent *e) override;
	void enterEventHook(QEnterEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;

private:
	Glyph _glyph = Glyph::Play;
	QString _tooltip;
	bool _active = false;

};

// The whole editor UI without the window: toolbar, panels, splitters and
// the global shortcuts. EditorWindow hosts it, snapshot scenes create it
// directly as a child widget.
class EditorWidget final : public Ui::RpWidget {
public:
	EditorWidget(
		QWidget *parent,
		not_null<EditorController*> controller,
		QString doneText = QString());
	~EditorWidget();

	[[nodiscard]] not_null<EditorController*> controller() const;
	[[nodiscard]] not_null<CanvasPanel*> canvas() const;
	[[nodiscard]] not_null<LayersPanel*> layers() const;
	[[nodiscard]] not_null<InspectorPanel*> inspector() const;
	[[nodiscard]] not_null<TimelinePanel*> timeline() const;

	// The extra toolbar button of EditorArgs::done was clicked.
	[[nodiscard]] rpl::producer<> doneRequests() const;

	// Global shortcuts, returns true if the key was handled.
	bool handleKey(not_null<QKeyEvent*> e);

	// Whether handleKey() takes this key away from the app-wide shortcuts
	// (accept its QEvent::ShortcutOverride). Window-level keys (Cmd+W,
	// Cmd+M, Cmd+Q) are left to them.
	[[nodiscard]] bool claimsShortcut(not_null<QKeyEvent*> e) const;

	// Latest validation of the current document (async, updated shortly
	// after every change), nullopt until the first one is ready.
	[[nodiscard]] const std::optional<ValidationResult> &validation() const;
	[[nodiscard]] rpl::producer<> validationUpdates() const;

protected:
	bool eventHook(QEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	class Toolbar;
	class Splitter;

	void setupSplitters();
	void setupValidation();
	void updateGeometries();
	void scheduleValidation();
	void runValidation();

	const not_null<EditorController*> _controller;
	const not_null<Toolbar*> _toolbar;
	const not_null<LayersPanel*> _layers;
	const not_null<CanvasPanel*> _canvas;
	const not_null<InspectorPanel*> _inspector;
	const not_null<TimelinePanel*> _timeline;
	Splitter *_layersSplitter = nullptr;
	Splitter *_inspectorSplitter = nullptr;
	Splitter *_timelineSplitter = nullptr;

	base::Timer _validationTimer;
	std::optional<ValidationResult> _validation;
	rpl::event_stream<> _validationUpdates;
	uint64 _validationGeneration = 0;

};

// The top-level window: owns the controller and the editor widget, shows
// boxes / toasts in its own layer, handles EditorAction requests (file
// dialogs, saving, export) and asks before closing with unsaved changes.
class EditorWindow final : public Ui::RpWindow {
public:
	explicit EditorWindow(EditorArgs &&args);
	~EditorWindow();

	[[nodiscard]] not_null<EditorController*> controller() const;
	[[nodiscard]] not_null<EditorWidget*> editor() const;
	[[nodiscard]] std::shared_ptr<Ui::Show> uiShow();

	void handleAction(EditorAction action);

	// Closes the window, asking Save / Don't save / Cancel first when there
	// are unsaved changes. closed runs after the window is destroyed (not
	// when the user cancels).
	void requestClose(Fn<void()> closed = nullptr);

	// Closes without asking.
	void forceClose();

protected:
	bool eventHook(QEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	void setupShortcuts();
	void updateTitle();
	void load(QByteArray data, QString name, QString path);
	void open();
	void createNew();
	void save(Fn<void()> saved = nullptr);
	void saveAs(Fn<void()> saved = nullptr);
	void exportAs(bool tgs);
	void write(
		QString path,
		bool tgs,
		bool asDocument,
		Fn<void()> saved);
	void closeConfirmed(Fn<void()> closed);

	// A new animation nobody touched: Open / New reuse this window.
	[[nodiscard]] bool untouchedBlank() const;

	const std::unique_ptr<EditorController> _controller;
	const std::unique_ptr<Ui::LayerManager> _layers;
	const Fn<void(QByteArray, QString)> _done;
	base::unique_qptr<EditorWidget> _editor; // Destroyed before _controller.
	Fn<void()> _closed;
	bool _closeConfirmed = false;
	bool _saving = false;

};

} // namespace Oblivion::LottieEdit
