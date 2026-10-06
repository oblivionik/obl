/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/effects/animations.h"
#include "ui/rp_widget.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/tooltip.h"

namespace style {
struct FlatLabel;
struct ScrollArea;
} // namespace style

namespace Ui {
class FlatLabel;
} // namespace Ui

// Small dark "studio" controls of the photo editor (see
// oblivion_photo_editor.h). They paint themselves with the group call /
// media viewer palette colors, so they look the same in every theme.
namespace Oblivion::Photo::EditorUi {

[[nodiscard]] int Px(int value);
[[nodiscard]] const style::font &SmallFont();
[[nodiscard]] const style::font &SmallSemiboldFont();
[[nodiscard]] const style::font &TitleFont();
[[nodiscard]] const style::ScrollArea &PanelScrollStyle();
[[nodiscard]] const style::FlatLabel &HintLabelStyle();

// "+25", "−25" (a real minus) or "0".
[[nodiscard]] QString FormatSigned(int value);
// Fixed decimals with the separator of the interface language.
[[nodiscard]] QString FormatDecimal(double value, int decimals);
// "Undo (⌘Z)".
[[nodiscard]] QString WithShortcut(
	const QString &text,
	const QKeySequence &keys);

// A card is a plate of the darker st::groupCallBg on the panel (an effect
// with its parameters). The secondary PanelButton and the chips are wells
// of that same color on the panel, so inside a card they would be bare
// text: there they paint themselves a step lighter. Whoever paints such
// a plate marks its widget, the controls find it among their parents.
void MarkAsCard(not_null<QWidget*> widget);
[[nodiscard]] bool InsideCard(not_null<const QWidget*> widget);

struct IconRef {
	const style::icon *icon = nullptr;
	bool mirrored = false;
	int rotation = 0;
	// An icon painted in code instead of a style one: it is asked to draw
	// itself with the color inside a centered 24 x 24 square (logical
	// pixels, already scaled with the interface).
	Fn<void(QPainter &p, QRectF rect, QColor color)> paint;
};
void PaintIcon(QPainter &p, const IconRef &icon, QRect rect, QColor color);

// A round icon button of the top bar with a tooltip.
class ToolButton final
	: public Ui::RippleButton
	, public Ui::AbstractTooltipShower {
public:
	ToolButton(QWidget *parent, IconRef icon, int size);

	void setTooltip(QString text);
	void setAvailable(bool available);
	void setActive(bool active);

	QString tooltipText() const override;
	QPoint tooltipPos() const override;
	bool tooltipWindowActive() const override;

protected:
	void paintEvent(QPaintEvent *e) override;
	void enterEventHook(QEnterEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	QImage prepareRippleMask() const override;

private:
	const IconRef _icon;
	QString _tooltip;
	bool _available = true;
	bool _active = false;

};

// A full width row with an icon and a text, like a menu item.
class RowButton final : public Ui::RippleButton {
public:
	RowButton(QWidget *parent, rpl::producer<QString> text, IconRef icon);

	void setAvailable(bool available);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	QImage prepareRippleMask() const override;

private:
	const IconRef _icon;
	QString _text;
	bool _available = true;

};

// A rounded full width button, accented if primary.
class PanelButton final : public Ui::RippleButton {
public:
	PanelButton(QWidget *parent, rpl::producer<QString> text, bool primary);

	void setAvailable(bool available);
	void setBusy(bool busy);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	QImage prepareRippleMask() const override;

private:
	QString _text;
	const bool _primary = false;
	bool _available = true;
	bool _busy = false;

};

// A small uppercase-less section title with an optional action link on
// the right ("Reset").
class SectionTitle final : public Ui::RpWidget {
public:
	SectionTitle(QWidget *parent, rpl::producer<QString> text);

	void setAction(rpl::producer<QString> text, Fn<void()> callback);
	void setActionVisible(bool visible);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	[[nodiscard]] QRect actionRect() const;
	void setActionOver(bool over);

	QString _text;
	QString _action;
	Fn<void()> _callback;
	bool _actionVisible = false;
	bool _actionOver = false;
	bool _actionDown = false;

};

enum class SliderTrack : uchar {
	Plain,
	Temperature,
	Tint,
};

struct SliderChange {
	int value = 0;
	bool finished = false;
};

struct SliderArgs {
	rpl::producer<QString> label;
	int min = 0;
	int max = 100;
	int defaultValue = 0;
	int value = 0;
	Fn<QString(int)> format;
	SliderTrack track = SliderTrack::Plain;
	// The knob moves with the logarithm of the value (only for min > 0):
	// for sizes with a wide range, like 16 .. 16384 pixels, where an even
	// scale would keep every usual value in the first tenth of the track.
	bool logarithmic = false;
};

// A labeled slider: the label and the value on top, the track below.
// The fill starts at zero when the range contains it (bipolar sliders).
// Double click resets it to the default value.
class ValueSlider final : public Ui::RpWidget {
public:
	ValueSlider(QWidget *parent, SliderArgs &&args);

	void setValue(int value);
	[[nodiscard]] int value() const;
	void setDimmed(bool dimmed);
	[[nodiscard]] bool dragging() const;

	[[nodiscard]] rpl::producer<SliderChange> changes() const;

	// With it a click on the shown value doesn't move the slider, it is
	// reported instead: the owner shows a field to type the value in
	// over valueRect().
	void setValueClickable(bool clickable);
	[[nodiscard]] rpl::producer<> valueClicks() const;
	[[nodiscard]] QRect valueRect() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void enterEventHook(QEnterEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	[[nodiscard]] QRect trackRect() const;
	[[nodiscard]] int valueFromX(int x) const;
	[[nodiscard]] int xFromValue(int value) const;
	void updateFromX(int x, bool finished);
	void paintTrack(QPainter &p, QRect track, float64 alpha);

	QString _label;
	const int _min = 0;
	const int _max = 100;
	const int _default = 0;
	int _value = 0;
	const Fn<QString(int)> _format;
	const SliderTrack _track = SliderTrack::Plain;
	const bool _logarithmic = false;
	bool _dimmed = false;
	bool _over = false;
	bool _pressed = false;
	int _pressValue = 0;
	int _pressX = 0;
	int _grabOffset = 0;
	bool _grabbed = false;
	bool _moved = false;
	bool _valueClickable = false;
	bool _valuePressed = false;
	Ui::Animations::Simple _overAnimation;
	rpl::event_stream<SliderChange> _changes;
	rpl::event_stream<> _valueClicks;

};

// Pill buttons laid out in rows, one of them may be selected.
class ChipsFlow final : public Ui::RpWidget {
public:
	explicit ChipsFlow(QWidget *parent);

	int addChip(rpl::producer<QString> text, Fn<void()> callback);
	void setSelected(int index);
	void setChipAvailable(int index, bool available);

protected:
	int resizeGetHeight(int newWidth) override;

private:
	class Chip;

	std::vector<not_null<Chip*>> _chips;
	int _selected = -1;

};

struct TabInfo {
	rpl::producer<QString> text;
	IconRef icon;
};

// Icon + label tabs with a sliding highlight. Each tab gets the width of
// its label plus an equal share of the rest, so long labels still fit.
// When the labels of all the visible tabs don't fit, only the active tab
// shows its label, the others become icons with tooltips.
class TabBar final
	: public Ui::RpWidget
	, public Ui::AbstractTooltipShower {
public:
	TabBar(QWidget *parent, std::vector<TabInfo> tabs);

	void setActive(int index, anim::type animated = anim::type::normal);
	[[nodiscard]] int active() const;
	[[nodiscard]] rpl::producer<int> activeChanges() const;
	// Puts the highlight where it is going at once.
	void finishAnimating();
	// A hidden tab takes no place and can't be clicked.
	void setTabVisible(int index, bool visible);
	[[nodiscard]] bool tabVisible(int index) const;

	QString tooltipText() const override;
	QPoint tooltipPos() const override;
	bool tooltipWindowActive() const override;

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	struct Tab {
		QString text;
		IconRef icon;
		bool visible = true;
	};
	[[nodiscard]] QRect tabRect(int index) const;
	[[nodiscard]] QRectF highlightRect() const;
	[[nodiscard]] int tabAt(QPoint point) const;
	void refreshWidths();

	std::vector<Tab> _tabs;
	std::vector<int> _lefts; // _tabs.size() + 1 edges.
	bool _compact = false;
	int _active = 0;
	int _previous = -1; // Fades out while the highlight slides.
	int _over = -1;
	int _pressed = -1;
	// The highlight slides from the rectangle it had when another tab was
	// chosen to the rectangle of that tab: the tabs between them (hidden
	// ones as well) and their widths don't matter.
	QRectF _slideFrom;
	Ui::Animations::Simple _slide;
	rpl::event_stream<int> _activeChanges;

};

// An on / off switch with a title: a header of an effect card (with the
// "..." menu button on the right, it has the side padding of the card in
// it) or a plain toggle row (withMenu == false: no padding of its own,
// the switch starts at the left edge like the label of a slider, so give
// it the same margins as the rows around it).
class SwitchHeader final : public Ui::RpWidget {
public:
	SwitchHeader(
		QWidget *parent,
		QString title,
		bool checked,
		bool withMenu = true);

	void setChecked(bool checked, anim::type animated);
	[[nodiscard]] bool checked() const;
	[[nodiscard]] rpl::producer<bool> toggles() const;
	[[nodiscard]] rpl::producer<QPoint> menuRequests() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	enum class Part : uchar {
		None,
		Toggle,
		Menu,
	};
	[[nodiscard]] QRect menuRect() const;
	[[nodiscard]] Part partAt(QPoint point) const;

	QString _title;
	bool _checked = false;
	const bool _withMenu = true;
	Part _over = Part::None;
	Part _pressed = Part::None;
	Ui::Animations::Simple _toggle;
	rpl::event_stream<bool> _toggles;
	rpl::event_stream<QPoint> _menuRequests;

};

// Preset color swatches in a row with a "custom color" button at the end.
class ColorSwatches final : public Ui::RpWidget {
public:
	ColorSwatches(
		QWidget *parent,
		rpl::producer<QString> label,
		std::vector<QColor> presets,
		QColor current);

	void setColor(QColor color);
	[[nodiscard]] rpl::producer<QColor> chosen() const;
	[[nodiscard]] rpl::producer<> customRequests() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	[[nodiscard]] int count() const;
	[[nodiscard]] QRect swatchRect(int index) const;
	[[nodiscard]] int swatchAt(QPoint point) const;

	QString _label;
	std::vector<QColor> _presets;
	QColor _current;
	int _over = -1;
	int _pressed = -1;
	rpl::event_stream<QColor> _chosen;
	rpl::event_stream<> _customRequests;

};

struct StripItem {
	QString id;
	QString name;
};

// The horizontal strip of filter previews. Wheel / drag scroll it.
class FilterStrip final : public Ui::RpWidget {
public:
	explicit FilterStrip(QWidget *parent);

	void setItems(std::vector<StripItem> items);
	void setThumbnails(std::vector<QImage> thumbnails);
	void setSelected(const QString &id, anim::type animated);
	[[nodiscard]] rpl::producer<QString> selections() const;
	[[nodiscard]] bool thumbnailsReady() const;

	[[nodiscard]] static int ThumbnailSide();
	[[nodiscard]] static int StripHeight();

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	[[nodiscard]] int tileWidth() const;
	[[nodiscard]] int contentWidth() const;
	[[nodiscard]] int maxScroll() const;
	[[nodiscard]] QRect tileRect(int index) const;
	[[nodiscard]] int tileAt(QPoint point) const;
	void scrollTo(int scroll, anim::type animated);
	void ensureVisible(int index, anim::type animated);

	std::vector<StripItem> _items;
	std::vector<QImage> _thumbnails;
	QString _selected;
	int _scroll = 0;
	int _over = -1;
	int _pressed = -1;
	QPoint _pressPoint;
	int _pressScroll = 0;
	bool _dragging = false;
	Ui::Animations::Simple _scrollAnimation;
	int _scrollFrom = 0;
	int _scrollTo = 0;
	rpl::event_stream<QString> _selections;

};

} // namespace Oblivion::Photo::EditorUi
