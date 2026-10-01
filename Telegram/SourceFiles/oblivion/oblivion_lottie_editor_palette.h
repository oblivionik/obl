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

namespace style {
struct InputField;
} // namespace style

namespace Ui {
class Checkbox;
class GenericBox;
class InputField;
class LinkButton;
class PopupMenu;
class RoundButton;
class Show;
} // namespace Ui

// Lottie editor: the palette tab of the inspector (every color of the
// animation, replace a color everywhere, global HSL correction) and the
// small pieces the layers / inspector / palette panels share: field
// styles, color helpers, the color picker box, LiveSession (previewed
// edits that end up as one undo step) and the snapshot scene host.
namespace Oblivion::LottieEdit {

class EditorController;

// Compact rounded single line field (inline rename, numbers, hex).
[[nodiscard]] const style::InputField &PanelFieldStyle();

// Rounded search field.
[[nodiscard]] const style::InputField &PanelSearchStyle();

// Smaller secondary text (counts, hints).
[[nodiscard]] const style::font &PanelSmallFont();

// For wrapped texts (hints, checkbox labels): a word of one or two letters
// (a preposition, a conjunction) stays with the next word and a dash with
// the word before it, by no-break spaces, so no line ends with "с" or
// starts with "—".
[[nodiscard]] QString BindShortWords(QString text);

// A transparent parent for an inline Ui::InputField. The field lets
// Escape through (after firing cancelled()), the host swallows it, so it
// doesn't reach the editor's global shortcuts (clear selection / close).
class FieldHost final : public Ui::RpWidget {
public:
	using RpWidget::RpWidget;

protected:
	void keyPressEvent(QKeyEvent *e) override;

};

// "#RRGGBB" (upper case) and back, an invalid QColor if the text is not
// "#RGB" / "#RRGGBB" (the "#" is optional, spaces are ignored).
[[nodiscard]] QString ColorHex(const QColor &color);
[[nodiscard]] QColor ParseColorHex(QString text);

// A rounded color sample with a thin border that stays visible on both
// light and dark backgrounds, an accent ring around a selected one.
void PaintSwatch(
	QPainter &p,
	QRectF rect,
	const QColor &color,
	double radius,
	bool selected = false);

// Replaces a plain JSON member ("lc", "lj", "ml", "r", "t", "m", "sy",
// "st", "sr"...) of a node, the property operations only cover animatable
// values. Unchanged document if the value is already there.
[[nodiscard]] Edit SetNodeMember(
	const Document &document,
	NodeId id,
	const QByteArray &key,
	Json::Value value);

// A preview-while-dragging edit (color picker, HSL sliders): every apply()
// gets an edit computed from baseline(), the document at begin(), so
// previews never accumulate. Previews go to the undo history as the
// controller merges them; squash() leaves exactly one undo step for all
// of them, cancel() undoes them back to the baseline. The session ends by
// itself when the document changes in any other way (undo, other edits).
class LiveSession final {
public:
	LiveSession(not_null<EditorController*> controller, Command command);
	~LiveSession();

	[[nodiscard]] bool active() const;
	[[nodiscard]] const Document &baseline() const;

	// Starts from the current document if not active yet.
	void begin();

	// The edit must be computed from baseline(). An edit that gives the
	// baseline back undoes the previews instead of adding a step.
	bool apply(Edit &&edit);

	void squash(); // One undo step for everything since begin(), active.
	void finish(); // squash() and end().
	void cancel(); // Back to the baseline and end().
	void end(); // Keeps the document as it is, stops tracking.

	[[nodiscard]] bool changed() const; // Document differs from baseline.
	[[nodiscard]] rpl::producer<> ended() const;

private:
	void rollback();

	const not_null<EditorController*> _controller;
	const Command _command;
	Document _baseline;
	QByteArray _key;
	std::vector<NodeId> _nodes;
	bool _structural = false;
	bool _active = false;
	bool _applying = false;
	int _steps = 0; // Upper estimate of the undo steps the previews made.
	crl::time _lastApply = 0;
	rpl::event_stream<> _ended;
	rpl::lifetime _lifetime;

};

// The color picker box (ColorEditor in HSL mode). preview is called on
// every change, done with the final color ("Done" / Enter), cancelled on
// any other way of closing.
struct ColorBoxArgs {
	QString title;
	QColor color;
	Fn<void(QColor)> preview;
	Fn<void(QColor)> done;
	Fn<void()> cancelled;
};
void ColorBox(not_null<Ui::GenericBox*> box, ColorBoxArgs &&args);
void ShowColorBox(std::shared_ptr<Ui::Show> show, ColorBoxArgs &&args);

// Picker for a property value at the current frame (auto-keyframed like
// EditorController::setValue), previewed live with a LiveSession: `make`
// builds the full value from the picked color. Needs uiShow().
void EditColorWithPicker(
	not_null<EditorController*> controller,
	PropertyRef ref,
	QString title,
	QColor current,
	Fn<PropValue(QColor)> make);

// A slider with a painted track (hue rainbow, saturation or lightness
// ramp) for the HSL correction, values are integers in [min, max], a
// double click resets to 0. Doesn't take focus (Left / Right stay the
// editor's frame stepping).
class ColorSlider final : public Ui::RpWidget {
public:
	enum class Track : uchar {
		Hue,
		Saturation,
		Lightness,
	};

	ColorSlider(QWidget *parent, Track track, int min, int max);

	void setValue(int value); // Doesn't fire changes().
	[[nodiscard]] int value() const;

	// Every value while dragging (and the double click reset).
	[[nodiscard]] rpl::producer<int> changes() const;
	// The mouse was released / the reset was done.
	[[nodiscard]] rpl::producer<> finishes() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void enterEventHook(QEnterEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	[[nodiscard]] QRect trackRect() const;
	[[nodiscard]] int valueAt(int x) const;
	void change(int value);

	const Track _track;
	const int _min = 0;
	const int _max = 0;
	int _value = 0;
	bool _pressed = false;
	bool _over = false;
	rpl::event_stream<int> _changes;
	rpl::event_stream<> _finishes;

};

// Palette tab of the inspector: every color of the animation as swatches
// with usage counts (a click selects the nodes using it, a double click
// replaces it everywhere), the active color with a hex field, and global
// hue / saturation / lightness correction with a reset. Lays itself out
// by width (resizeToWidth), meant to live in a scroll area.
class PalettePanel final
	: public Ui::RpWidget
	, public Ui::AbstractTooltipShower {
public:
	PalettePanel(QWidget *parent, not_null<EditorController*> controller);
	~PalettePanel();

	// Makes the index-th color (most used first) the active one, as a click
	// on its swatch without selecting the nodes; when the palette is still
	// being computed, as soon as it is ready. For the snapshot scenes.
	void activateEntry(int index);

	QString tooltipText() const override;
	QPoint tooltipPos() const override;
	bool tooltipWindowActive() const override;

protected:
	int resizeGetHeight(int newWidth) override;
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	struct Hsl {
		int hue = 0;
		int saturation = 0;
		int lightness = 0;
		bool limited = false; // Only the selected nodes.
		std::vector<NodeId> scope;

		friend inline bool operator==(const Hsl &a, const Hsl &b) = default;
	};

	void setupControls();
	int relayout(int width);
	void schedulePalette();
	void computePalette();
	void applyPalette(std::vector<PaletteEntry> entries);
	void setActive(std::optional<QRgb> color);
	void updateHexField();
	void commitHex();
	void selectOccurrences(QRgb color);
	void editColor(QRgb color);
	void replaceActive(const QColor &to);
	void showSwatchMenu(QRgb color, QPoint globalPosition);
	[[nodiscard]] int swatchAt(QPoint position) const;
	[[nodiscard]] QRect swatchRect(int index) const;
	[[nodiscard]] const PaletteEntry *entry(QRgb color) const;

	void hslChanged();
	void startHslJob();
	void hslFinished();
	void resetHsl();
	void zeroHslSliders();
	void refreshReset();
	[[nodiscard]] Hsl wantedHsl() const;

	const not_null<EditorController*> _controller;

	std::vector<PaletteEntry> _entries;
	bool _paletteReady = false;
	bool _paletteRunning = false;
	bool _paletteDirty = false;
	std::optional<QRgb> _active;
	std::optional<int> _activateIndex;
	int _over = -1;
	int _pressed = -1;
	QString _tooltip;

	// Layout.
	int _layoutWidth = -1;
	int _columns = 1;
	int _gridInner = 0; // The swatch columns fill this width.
	int _gridTop = 0;
	int _gridHeight = 0;
	int _activeTop = 0;
	int _hintTop = 0;
	int _hintHeight = 0;
	int _adjustTop = 0;
	QString _hint;

	FieldHost *_hexHost = nullptr;
	Ui::InputField *_hex = nullptr;
	Ui::LinkButton *_edit = nullptr;
	Ui::LinkButton *_select = nullptr;
	struct SliderRow {
		ColorSlider *slider = nullptr;
		QString label;
		int top = 0;
	};
	std::array<SliderRow, 3> _sliders;
	Ui::Checkbox *_selectionOnly = nullptr;
	Ui::RoundButton *_reset = nullptr;

	LiveSession _hsl;
	std::optional<Hsl> _hslRunning;
	bool _hslSquash = false;
	base::unique_qptr<Ui::PopupMenu> _menu;

};

// Snapshot scenes of the panels (OBLIVION_SELFTEST=ui): owns a controller
// with a bundled sample animation and one panel that fills it.
class PanelSceneHost final : public Ui::RpWidget {
public:
	using Factory = Fn<not_null<Ui::RpWidget*>(
		QWidget *parent,
		not_null<EditorController*> controller)>;

	// resource: ":/animations/palette.tgs" and alike.
	PanelSceneHost(QWidget *parent, const QString &resource, Factory create);
	~PanelSceneHost();

	[[nodiscard]] not_null<EditorController*> controller() const;
	[[nodiscard]] not_null<Ui::RpWidget*> panel() const;

private:
	const std::unique_ptr<EditorController> _controller;
	base::unique_qptr<Ui::RpWidget> _panel;

};

// First node with the name (and the kind), depth-first, 0 if none.
[[nodiscard]] NodeId FindNodeByName(
	const Document &document,
	const QString &name,
	std::optional<NodeKind> kind = std::nullopt);

// The skip-th (0-based) shape item of the type, 0 if none.
[[nodiscard]] NodeId FindShapeOfType(
	const Document &document,
	ShapeType type,
	int skip = 0);

} // namespace Oblivion::LottieEdit
