/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_photo_layers_ui.h"

#include "base/flat_map.h"
#include "base/platform/base_platform_info.h"
#include "base/timer.h"
#include "base/unique_qptr.h"
#include "lang/lang_keys.h"
#include "oblivion/oblivion_photo_editor.h"
#include "oblivion/oblivion_photo_editor_controls.h"
#include "oblivion/oblivion_photo_panels.h"
#include "oblivion/oblivion_photo_transform.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/effects/animation_value.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_basic.h"
#include "styles/style_calls.h"
#include "styles/style_media_player.h"
#include "styles/style_widgets.h"

#include <QtCore/QMimeData>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainterPath>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QTextEdit>

namespace Oblivion::Photo {
namespace {

using namespace EditorUi;

constexpr auto kPi = 3.14159265358979323846;

constexpr auto kPadding = 16;
constexpr auto kSkip = 8;

// The layers list.
constexpr auto kHeaderHeight = 36;
constexpr auto kHeaderButton = 30;
constexpr auto kHeaderButtonsRight = 8;
constexpr auto kRowHeight = 44;
constexpr auto kListBottom = 4;
constexpr auto kRowRadius = 8;
constexpr auto kRowLeft = 14;
constexpr auto kRowRight = 8;
constexpr auto kThumb = 30;
constexpr auto kThumbRadius = 5;
constexpr auto kThumbSkip = 6;
constexpr auto kThumbCheck = 5;
constexpr auto kTextSkip = 10;
constexpr auto kKindIcon = 18;
constexpr auto kIconCell = 30;
constexpr auto kDragThreshold = 5;
constexpr auto kAutoScrollZone = 22;
constexpr auto kAutoScrollStep = 7;
constexpr auto kNameLimit = 64;
constexpr auto kFieldHeight = 28;
constexpr auto kDimmedOpacity = 0.4;
constexpr auto kTooltipDelay = 800;
constexpr auto kThumbsDelay = crl::time(140);
constexpr auto kAutoScrollInterval = crl::time(16);

// The transform tool.
constexpr auto kHandleCatch = 9;
constexpr auto kHandleSide = 9;
constexpr auto kEdgeHandleSide = 7;
constexpr auto kRotateOffset = 26;
constexpr auto kRotateRadius = 5;
constexpr auto kSnapDistance = 6;
constexpr auto kMoveThreshold = 3;
constexpr auto kRotateSnapStep = 45.;
constexpr auto kRotateSnapNear = 3.;
constexpr auto kRotateShiftStep = 15.;
constexpr auto kNudgeStep = 1.;
constexpr auto kNudgeBigStep = 10.;
constexpr auto kScaleMinPercent = 1.;
constexpr auto kScaleMaxPercent = 1000.;
constexpr auto kSkewLimit = 75.;
constexpr auto kHitMapSide = 128;
constexpr auto kHitMapsDelay = crl::time(200);
constexpr auto kBadgePadding = 6;
constexpr auto kBadgeOffset = 18;

// The mask tool.
constexpr auto kMaskSizeMin = 0.002;
constexpr auto kMaskSizeMax = 0.5;
constexpr auto kMaskSizeDefault = 0.06;
constexpr auto kMaskSizeStep = 1.15;
constexpr auto kMaskHardnessDefault = 60;
constexpr auto kMaskOverlayAlpha = 140;

[[nodiscard]] QColor TextColor() {
	return st::groupCallMembersFg->c;
}

[[nodiscard]] QColor SubTextColor() {
	return st::groupCallMemberNotJoinedStatus->c;
}

[[nodiscard]] QColor AccentColor() {
	return st::groupCallActiveFg->c;
}

[[nodiscard]] QColor GuideColor() {
	return QColor(0xFF, 0x4F, 0xA3);
}

[[nodiscard]] style::margins RowMargins(int top = 0) {
	return style::margins(Px(kPadding), top, Px(kPadding), 0);
}

[[nodiscard]] QString CommandKeyName() {
	return Platform::IsMac() ? QString(QChar(0x2318)) : u"Ctrl"_q;
}

[[nodiscard]] QString PercentText(int value) {
	return tr::lng_oblivion_photo_layers_percent(
		tr::now,
		lt_value,
		QString::number(value));
}

// What the user chose in the options of the tools: one for all editors,
// kept while the application runs.
struct ToolState {
	rpl::variable<bool> perspective = false;
	rpl::variable<bool> keepAspect = true;
	rpl::variable<bool> snapping = true;
	rpl::variable<bool> maskReveal = false;
	rpl::variable<double> maskSize = kMaskSizeDefault; // Of the canvas.
	rpl::variable<int> maskHardness = kMaskHardnessDefault;
	rpl::variable<int> maskStrength = 100;
	rpl::variable<bool> maskOverlay = false;
};

[[nodiscard]] ToolState &State() {
	// Never destroyed: nothing depends on the order of the static
	// destructors at quit then.
	static const auto result = new ToolState();
	return *result;
}

[[nodiscard]] bool HasPixels(const Layer *layer) {
	return layer && layer->content && !layer->size().isEmpty();
}

[[nodiscard]] bool Usable(const Layer *layer) {
	return HasPixels(layer) && layer->visible;
}

[[nodiscard]] QPolygonF MapQuad(
		const QTransform &transform,
		const QPolygonF &quad) {
	auto result = QPolygonF();
	result.reserve(quad.size());
	for (const auto &point : quad) {
		result.push_back(transform.map(point));
	}
	return result;
}

[[nodiscard]] QPolygonF WidgetQuad(
		const Layer &layer,
		const QTransform &documentToWidget) {
	return MapQuad(
		documentToWidget,
		TransformedQuad(layer.transform, QSizeF(layer.size())));
}

// The part of the canvas that is on the screen (what is left of it after
// the crop and the turns of the whole picture), in widget coordinates.
[[nodiscard]] QPainterPath VisiblePath(
		const Document &document,
		const QTransform &documentToWidget) {
	auto quad = ContentQuad(QSizeF(document.size));
	const auto output = OutputSize(document);
	if (!output.isEmpty()) {
		auto invertible = false;
		const auto inverse = OutputTransform(document, output).inverted(
			&invertible);
		if (invertible) {
			quad = MapQuad(inverse, ContentQuad(QSizeF(output)));
		}
	}
	auto result = QPainterPath();
	result.addPolygon(MapQuad(documentToWidget, quad));
	result.closeSubpath();
	return result;
}

[[nodiscard]] bool FiniteQuad(const QPolygonF &quad) {
	if (quad.size() != 4) {
		return false;
	}
	for (const auto &point : quad) {
		if (!std::isfinite(point.x())
			|| !std::isfinite(point.y())
			|| std::abs(point.x()) > 1e7
			|| std::abs(point.y()) > 1e7) {
			return false;
		}
	}
	return true;
}

// A letter key whatever the keyboard layout is: the Latin key or the
// Cyrillic letter that is on the same key.
[[nodiscard]] bool KeyIs(not_null<QKeyEvent*> e, int key, char16_t cyrillic) {
	if (e->key() == key) {
		return true;
	}
	const auto text = e->text();
	return (text.size() == 1) && (text[0].toLower() == QChar(cyrillic));
}

// The widget of the editor: the one that has the keyboard while the keys
// are meant for the canvas (it is what the Ui::Show of the editor works
// in). Null for a controller without an editor (a panel shown alone).
[[nodiscard]] QWidget *EditorWidget(not_null<const Controller*> controller) {
	const auto show = controller->uiShow();
	return (show && show->valid()) ? show->toastParent().get() : nullptr;
}

// A key that a text field had no use for (Backspace in a field that is
// empty already) goes up through its parents to the editor, and from
// there to the current tool like a key pressed over the canvas. The tools
// here take a key only while nothing is being typed in a field.
[[nodiscard]] bool TextInputFocused(not_null<const Controller*> controller) {
	const auto focused = QApplication::focusWidget();
	return focused
		&& (focused != EditorWidget(controller))
		&& (qobject_cast<QTextEdit*>(focused)
			|| qobject_cast<QLineEdit*>(focused)
			|| focused->testAttribute(Qt::WA_InputMethodEnabled));
}

// The keyboard is with the editor itself, not with one of the fields,
// buttons or lists inside of it. What a key can't take back by itself,
// deleting a layer, asks for that.
[[nodiscard]] bool CanvasHasKeyboard(not_null<const Controller*> controller) {
	const auto focused = QApplication::focusWidget();
	return focused && (focused == EditorWidget(controller));
}

// How the canvas is turned and mirrored on the screen (Document::global).
[[nodiscard]] QTransform SeenOrientation(const Document &document) {
	return document.size.isEmpty()
		? QTransform()
		: ViewOrientation(OutputTransform(document, QSize()));
}

//
// Icons, drawn inside a 24 x 24 box.
//

void PrepareIcon(QPainter &p, QRectF rect) {
	p.translate(rect.topLeft());
	p.scale(rect.width() / 24., rect.height() / 24.);
}

[[nodiscard]] QPen IconPen(QColor color, double width = 1.6) {
	auto result = QPen(color, width);
	result.setCapStyle(Qt::RoundCap);
	result.setJoinStyle(Qt::RoundJoin);
	return result;
}

void PaintPlusIcon(QPainter &p, QRectF rect, QColor color) {
	PrepareIcon(p, rect);
	p.setPen(IconPen(color, 1.8));
	p.drawLine(QPointF(12., 6.5), QPointF(12., 17.5));
	p.drawLine(QPointF(6.5, 12.), QPointF(17.5, 12.));
}

void PaintDotsIcon(QPainter &p, QRectF rect, QColor color) {
	PrepareIcon(p, rect);
	p.setPen(Qt::NoPen);
	p.setBrush(color);
	for (const auto x : { 6.5, 12., 17.5 }) {
		p.drawEllipse(QPointF(x, 12.), 1.6, 1.6);
	}
}

// A frame with handles in its corners and an arrow inside.
void PaintTransformIcon(QPainter &p, QRectF rect, QColor color) {
	PrepareIcon(p, rect);
	p.setPen(IconPen(color, 1.4));
	p.setBrush(Qt::NoBrush);
	p.drawRect(QRectF(5.5, 5.5, 13., 13.));
	p.setPen(Qt::NoPen);
	p.setBrush(color);
	for (const auto x : { 5.5, 18.5 }) {
		for (const auto y : { 5.5, 18.5 }) {
			p.drawRect(QRectF(x - 2., y - 2., 4., 4.));
		}
	}
	p.setPen(IconPen(color, 1.4));
	p.drawLine(QPointF(9.5, 14.5), QPointF(14.5, 9.5));
	auto head = QPainterPath();
	head.moveTo(11.3, 9.5);
	head.lineTo(14.5, 9.5);
	head.lineTo(14.5, 12.7);
	p.setBrush(Qt::NoBrush);
	p.drawPath(head);
}

// A sheet with a round hole: what a mask does.
void PaintMaskIcon(QPainter &p, QRectF rect, QColor color) {
	PrepareIcon(p, rect);
	auto sheet = QPainterPath();
	sheet.setFillRule(Qt::OddEvenFill);
	sheet.addRoundedRect(QRectF(4., 5.5, 16., 13.), 2.5, 2.5);
	sheet.addEllipse(QPointF(12., 12.), 3.6, 3.6);
	p.setPen(Qt::NoPen);
	p.setBrush(color);
	p.drawPath(sheet);
}

void PaintEyeGlyph(
		QPainter &p,
		QPointF center,
		double unit,
		QColor color,
		bool visible) {
	p.setPen(IconPen(color, 1.5 * unit));
	p.setBrush(Qt::NoBrush);
	auto shape = QPainterPath();
	shape.moveTo(center + QPointF(-8. * unit, 0.));
	shape.quadTo(
		center + QPointF(0., -7. * unit),
		center + QPointF(8. * unit, 0.));
	shape.quadTo(
		center + QPointF(0., 7. * unit),
		center + QPointF(-8. * unit, 0.));
	p.drawPath(shape);
	if (visible) {
		p.setBrush(color);
		p.setPen(Qt::NoPen);
		p.drawEllipse(center, 2.2 * unit, 2.2 * unit);
	} else {
		p.drawLine(
			center + QPointF(-6. * unit, 6. * unit),
			center + QPointF(6. * unit, -6. * unit));
	}
}

void PaintLockGlyph(
		QPainter &p,
		QPointF center,
		double unit,
		QColor color,
		bool locked) {
	p.setPen(IconPen(color, 1.5 * unit));
	p.setBrush(Qt::NoBrush);
	const auto arc = QRectF(
		center.x() - 3.2 * unit,
		center.y() - 7.6 * unit,
		6.4 * unit,
		6.4 * unit);
	auto shackle = QPainterPath();
	shackle.moveTo(center + QPointF(-3.2 * unit, -1.4 * unit));
	shackle.lineTo(center + QPointF(-3.2 * unit, -4.4 * unit));
	shackle.arcTo(arc, 180., -180.);
	if (locked) {
		shackle.lineTo(center + QPointF(3.2 * unit, -1.4 * unit));
	}
	p.drawPath(shackle);
	const auto body = QRectF(
		center.x() - 5. * unit,
		center.y() - 1.4 * unit,
		10. * unit,
		8. * unit);
	if (locked) {
		p.setPen(Qt::NoPen);
		p.setBrush(color);
	}
	p.drawRoundedRect(body, 2. * unit, 2. * unit);
}

//
// Layer actions.
//

void ToastLocked(not_null<Controller*> controller) {
	controller->showToast(tr::lng_oblivion_photo_layers_locked(tr::now));
}

// The layer can be changed: it is there and is not locked (a toast says
// so if it is).
[[nodiscard]] bool Editable(not_null<Controller*> controller, LayerId id) {
	const auto layer = controller->document().find(id);
	if (!layer || controller->busy()) {
		return false;
	} else if (layer->locked) {
		ToastLocked(controller);
		return false;
	}
	return true;
}

void ToggleLayerMask(not_null<Controller*> controller, LayerId id) {
	const auto layer = controller->document().find(id);
	if (layer && layer->mask && Editable(controller, id)) {
		controller->changeLayer(id, [](Layer &layer) {
			layer.maskEnabled = !layer.maskEnabled;
		});
	}
}

void FillLayerMask(not_null<Controller*> controller, LayerId id, int value) {
	const auto layer = controller->document().find(id);
	if (!HasPixels(layer) || !Editable(controller, id)) {
		return;
	}
	auto mask = MakeMask(
		layer->mask ? layer->mask->image().size() : MaskSizeFor(layer->size()),
		value);
	if (!mask) {
		return;
	}
	controller->changeLayer(id, [&](Layer &layer) {
		layer.mask = mask;
		layer.maskEnabled = true;
	});
}

void ChangeTransform(
		not_null<Controller*> controller,
		Fn<QTransform(
			const QTransform &transform,
			QSizeF content,
			QSizeF canvas)> make) {
	const auto layer = controller->hasDocument()
		? controller->activeLayer()
		: nullptr;
	if (!HasPixels(layer) || !Editable(controller, layer->id)) {
		return;
	}
	const auto content = QSizeF(layer->size());
	const auto result = make(
		layer->transform,
		content,
		QSizeF(controller->document().size));
	if (!ValidTransform(result, content)) {
		return;
	}
	controller->changeLayer(layer->id, [=](Layer &layer) {
		layer.transform = result;
	});
}

void FillLayerMenu(
		not_null<Ui::PopupMenu*> menu,
		not_null<Controller*> controller,
		LayerId id,
		Fn<void()> rename) {
	const auto &document = controller->document();
	const auto layer = document.find(id);
	if (!layer) {
		return;
	}
	const auto count = int(document.layers.size());
	const auto index = document.indexOf(id);
	if (rename) {
		menu->addAction(
			tr::lng_oblivion_photo_layers_rename(tr::now),
			rename);
	}
	menu->addAction(tr::lng_oblivion_photo_layers_duplicate(tr::now), [=] {
		if (!controller->busy()) {
			controller->duplicateLayer(id);
		}
	});
	menu->addAction(
		(layer->visible
			? tr::lng_oblivion_photo_layers_hide
			: tr::lng_oblivion_photo_layers_show)(tr::now),
		[=] {
			controller->changeLayer(id, [](Layer &layer) {
				layer.visible = !layer.visible;
			});
		});
	menu->addAction(
		(layer->locked
			? tr::lng_oblivion_photo_layers_unlock
			: tr::lng_oblivion_photo_layers_lock)(tr::now),
		[=] {
			controller->changeLayer(id, [](Layer &layer) {
				layer.locked = !layer.locked;
			});
		});
	{
		auto submenu = std::make_unique<Ui::PopupMenu>(
			menu.get(),
			st::groupCallPopupMenuWithIcons);
		for (const auto mode : BlendModes()) {
			submenu->addAction(BlendModeName(mode), [=] {
				controller->changeLayer(id, [=](Layer &layer) {
					layer.blend = mode;
				});
			}, (layer->blend == mode) ? &st::mediaPlayerMenuCheck : nullptr);
		}
		menu->addAction(
			tr::lng_oblivion_photo_layers_blend_menu(tr::now),
			std::move(submenu));
	}
	{
		auto submenu = std::make_unique<Ui::PopupMenu>(
			menu.get(),
			st::groupCallPopupMenuWithIcons);
		const auto current = int(std::lround(layer->opacity * 100.));
		for (const auto percent : { 100, 75, 50, 25, 10 }) {
			submenu->addAction(PercentText(percent), [=] {
				controller->changeLayer(id, [=](Layer &layer) {
					layer.opacity = percent / 100.;
				});
			}, (current == percent) ? &st::mediaPlayerMenuCheck : nullptr);
		}
		menu->addAction(
			tr::lng_oblivion_photo_layers_opacity_menu(tr::now),
			std::move(submenu));
	}

	menu->addSeparator();
	menu->addAction(tr::lng_oblivion_photo_layers_transform(tr::now), [=] {
		controller->setActiveLayer(id);
		controller->setTool(kTransformTool);
	});
	if (!layer->mask) {
		menu->addAction(tr::lng_oblivion_photo_layers_mask_add(tr::now), [=] {
			AddLayerMask(controller, id);
		});
	} else {
		menu->addAction(
			tr::lng_oblivion_photo_layers_mask_paint(tr::now),
			[=] { AddLayerMask(controller, id); });
		menu->addAction(
			tr::lng_oblivion_photo_layers_mask_invert(tr::now),
			[=] { InvertLayerMask(controller, id); });
		menu->addAction(
			(layer->maskEnabled
				? tr::lng_oblivion_photo_layers_mask_disable
				: tr::lng_oblivion_photo_layers_mask_enable)(tr::now),
			[=] { ToggleLayerMask(controller, id); });
		menu->addAction(
			tr::lng_oblivion_photo_layers_mask_remove(tr::now),
			[=] { RemoveLayerMask(controller, id); });
	}

	const auto flatten = !IsPlainImage(document);
	if (count > 1 || flatten) {
		menu->addSeparator();
	}
	if (index + 1 < count) {
		menu->addAction(tr::lng_oblivion_photo_layers_move_up(tr::now), [=] {
			controller->change([=](Document &document) {
				MoveLayer(document, id, document.indexOf(id) + 1);
			});
		});
	}
	if (index > 0) {
		menu->addAction(
			tr::lng_oblivion_photo_layers_move_down(tr::now),
			[=] {
				controller->change([=](Document &document) {
					MoveLayer(document, id, document.indexOf(id) - 1);
				});
			});
		menu->addAction(
			tr::lng_oblivion_photo_layers_merge_down(tr::now),
			[=] { MergeLayerDown(controller, id); });
	}
	if (flatten) {
		menu->addAction(
			tr::lng_oblivion_photo_layers_flatten(tr::now),
			[=] { FlattenLayers(controller); });
	}
	if (count > 1) {
		menu->addSeparator();
		menu->addAction(
			tr::lng_oblivion_photo_layers_delete(tr::now),
			[=] { DeleteLayer(controller, id); });
	}
}

//
// The layers list.
//

[[nodiscard]] const style::InputField &NameFieldStyle() {
	static const auto result = [] {
		auto st = st::defaultInputField;
		st.textBg = st::groupCallBg;
		st.textBgActive = st::groupCallBg;
		st.textFg = st::groupCallMembersFg;
		st.textMargins = QMargins(Px(8), Px(5), Px(8), Px(3));
		st.textAlign = style::al_left;
		st.placeholderMargins = QMargins();
		st.placeholderScale = 0.;
		st.placeholderShift = 0;
		st.placeholderFont = st::normalFont;
		st.borderFg = st::groupCallMemberInactiveIcon;
		st.borderFgActive = st::groupCallActiveFg;
		st.borderFgError = st::groupCallActiveFg;
		st.border = std::max(Px(1), 1);
		st.borderActive = st.border;
		st.borderRadius = Px(6);
		st.borderDenominator = 1;
		st.style = st::defaultTextStyle;
		st.width = 0;
		st.widthMin = 0;
		st.heightMin = Px(kFieldHeight);
		st.heightMax = Px(kFieldHeight);
		return st;
	}();
	return result;
}

// The parent of the name field. What is typed into the field stays with
// it: Escape and Enter that the field has handled, and the keys it had
// nothing to do with (Backspace in an empty field, an arrow at the end of
// the text), must not reach the shortcuts of the editor and of its tools,
// where Escape closes the editor and Backspace deletes a layer. Only the
// shortcuts with Cmd / Ctrl (save, zoom) go on.
class FieldHost final : public Ui::RpWidget {
public:
	using RpWidget::RpWidget;

protected:
	void keyPressEvent(QKeyEvent *e) override {
		const auto key = e->key();
		if (key == Qt::Key_Escape
			|| key == Qt::Key_Return
			|| key == Qt::Key_Enter
			|| !(e->modifiers() & Qt::ControlModifier)) {
			e->accept();
			return;
		}
		RpWidget::keyPressEvent(e);
	}

};

// Gives the keyboard back to the editor.
void FocusEditor(not_null<QWidget*> from) {
	for (auto parent = from->parentWidget()
		; parent
		; parent = parent->parentWidget()) {
		if (parent->focusPolicy() == Qt::StrongFocus) {
			parent->setFocus();
			return;
		}
	}
}

void PaintChecks(QPainter &p, QRect rect, int radius) {
	p.save();
	auto clip = QPainterPath();
	clip.addRoundedRect(QRectF(rect), radius, radius);
	p.setClipPath(clip);
	p.fillRect(rect, QColor(0x3A, 0x3D, 0x42));
	const auto step = std::max(Px(kThumbCheck), 2);
	const auto light = QColor(0x55, 0x59, 0x60);
	for (auto y = 0, row = 0; y < rect.height(); y += step, ++row) {
		for (auto x = (row % 2) * step; x < rect.width(); x += 2 * step) {
			p.fillRect(rect.x() + x, rect.y() + y, step, step, light);
		}
	}
	p.restore();
}

class LayersList final
	: public Ui::RpWidget
	, public Ui::AbstractTooltipShower {
public:
	LayersList(
		QWidget *parent,
		not_null<Controller*> controller,
		not_null<Ui::ScrollArea*> scroll);

	void showMenuFor(LayerId id, QPoint globalPosition);
	void startRename(LayerId id);

	// What the eye, the lock and the small picture of the mask do.
	QString tooltipText() const override;
	QPoint tooltipPos() const override;
	bool tooltipWindowActive() const override;

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	void hideEvent(QHideEvent *e) override;

private:
	enum class Part : uchar {
		None,
		Row,
		Mask,
		Lock,
		Eye,
	};
	struct Hit {
		int row = -1;
		Part part = Part::None;

		friend bool operator==(const Hit &a, const Hit &b) = default;
	};
	struct Geometry {
		QRect thumb;
		QRect mask; // Empty without a mask.
		QRect lock;
		QRect eye;
		int textLeft = 0;
		int textRight = 0;
	};
	struct Thumb {
		uint64 requested = 0;
		QImage image;
		uint64 maskSerial = 0;
		QImage mask;
	};

	[[nodiscard]] int count() const;
	[[nodiscard]] const Layer *layerAt(int row) const;
	[[nodiscard]] int rowOf(LayerId id) const;
	[[nodiscard]] QRect rowRect(int row) const;
	[[nodiscard]] Geometry geometry(const Layer &layer, QRect rect) const;
	[[nodiscard]] Hit hitTest(QPoint point) const;
	void refresh();
	void refreshThumbnails();
	void ensureVisible(LayerId id);
	void setOver(Hit over);
	void paintRow(QPainter &p, const Layer &layer, QRect rect, bool floating);
	void updateDrag(QPoint point);
	void finishDrag(bool apply);
	void autoScroll();
	void placeRename();
	void finishRename(bool apply, bool refocus);

	const not_null<Controller*> _controller;
	const not_null<Ui::ScrollArea*> _scroll;
	base::flat_map<LayerId, Thumb> _thumbs;
	base::Timer _thumbsTimer;
	base::Timer _autoScroll;
	Hit _over;

	LayerId _pressed = 0;
	QPoint _pressPoint;
	int _pressOffset = 0;
	bool _dragging = false;
	LayerId _dragId = 0;
	int _dragTop = 0;
	int _dragGap = 0;

	LayerId _renaming = 0;
	FieldHost *_renameHost = nullptr;
	Ui::InputField *_renameField = nullptr;

	base::unique_qptr<Ui::PopupMenu> _menu;

};

LayersList::LayersList(
	QWidget *parent,
	not_null<Controller*> controller,
	not_null<Ui::ScrollArea*> scroll)
: RpWidget(parent)
, _controller(controller)
, _scroll(scroll)
, _thumbsTimer([=] { refreshThumbnails(); })
, _autoScroll([=] { autoScroll(); }) {
	setMouseTracking(true);
	rpl::merge(
		_controller->documentChanges(),
		_controller->activeLayerValue() | rpl::to_empty,
		_controller->toolValue() | rpl::to_empty
	) | rpl::on_next([=] {
		refresh();
	}, lifetime());
	_controller->activeLayerValue(
	) | rpl::skip(1) | rpl::on_next([=](LayerId id) {
		ensureVisible(id);
	}, lifetime());
	refreshThumbnails();
}

int LayersList::count() const {
	return _controller->hasDocument()
		? int(_controller->document().layers.size())
		: 0;
}

const Layer *LayersList::layerAt(int row) const {
	const auto index = LayerRowToIndex(count(), row);
	return (index >= 0) ? &_controller->document().layers[index] : nullptr;
}

int LayersList::rowOf(LayerId id) const {
	return _controller->hasDocument()
		? LayerIndexToRow(count(), _controller->document().indexOf(id))
		: -1;
}

QRect LayersList::rowRect(int row) const {
	const auto height = Px(kRowHeight);
	return QRect(0, row * height, width(), height);
}

LayersList::Geometry LayersList::geometry(
		const Layer &layer,
		QRect rect) const {
	auto result = Geometry();
	const auto thumb = Px(kThumb);
	const auto cell = Px(kIconCell);
	const auto top = rect.y() + (rect.height() - thumb) / 2;
	auto left = rect.x() + Px(kRowLeft);
	result.thumb = QRect(left, top, thumb, thumb);
	left += thumb;
	if (layer.mask) {
		left += Px(kThumbSkip);
		result.mask = QRect(left, top, thumb, thumb);
		left += thumb;
	}
	result.textLeft = left + Px(kTextSkip);
	auto right = rect.x() + rect.width() - Px(kRowRight);
	result.eye = QRect(right - cell, rect.y(), cell, rect.height());
	right -= cell;
	result.lock = QRect(right - cell, rect.y(), cell, rect.height());
	right -= cell;
	result.textRight = right - Px(4);
	return result;
}

LayersList::Hit LayersList::hitTest(QPoint point) const {
	if (!rect().contains(point)) {
		return {};
	}
	const auto row = point.y() / Px(kRowHeight);
	const auto layer = layerAt(row);
	if (!layer) {
		return {};
	}
	const auto places = geometry(*layer, rowRect(row));
	return {
		.row = row,
		.part = places.eye.contains(point)
			? Part::Eye
			: places.lock.contains(point)
			? Part::Lock
			: places.mask.contains(point)
			? Part::Mask
			: Part::Row,
	};
}

void LayersList::refresh() {
	const auto &document = _controller->document();
	if (_renaming && !document.find(_renaming)) {
		finishRename(false, true);
	}
	if (_dragging && !document.find(_dragId)) {
		finishDrag(false);
	}
	if (_pressed && !document.find(_pressed)) {
		_pressed = 0;
	}
	resizeToWidth(width());
	placeRename();
	// A layer that has no picture yet (a new one, the first document) is
	// asked for at once, the pause is for a picture that keeps changing.
	const auto added = ranges::any_of(document.layers, [&](const Layer &layer) {
		return !_thumbs.contains(layer.id);
	});
	if (added) {
		_thumbsTimer.cancel();
		refreshThumbnails();
	} else if (!_thumbsTimer.isActive()) {
		_thumbsTimer.callOnce(kThumbsDelay);
	}
	update();
}

void LayersList::refreshThumbnails() {
	if (!_controller->hasDocument()) {
		_thumbs.clear();
		return;
	}
	const auto &layers = _controller->document().layers;
	for (auto i = begin(_thumbs); i != end(_thumbs);) {
		if (ranges::contains(layers, i->first, &Layer::id)) {
			++i;
		} else {
			i = _thumbs.erase(i);
		}
	}
	const auto ratio = style::DevicePixelRatio();
	const auto side = Px(kThumb) * ratio;
	for (const auto &layer : layers) {
		const auto id = layer.id;
		// Zero means "nothing was asked for yet".
		const auto key = LayerThumbnailKey(layer) | 1;
		const auto serial = layer.mask ? layer.mask->serial() : uint64(0);
		auto &thumb = _thumbs[id];
		const auto pictureChanged = (thumb.requested != key);
		const auto maskChanged = (thumb.maskSerial != serial);
		thumb.requested = key;
		thumb.maskSerial = serial;
		if (maskChanged && !layer.mask) {
			thumb.mask = QImage();
		}
		if (pictureChanged) {
			_controller->requestLayerThumbnail(
				id,
				QSize(side, side),
				crl::guard(this, [=](QImage image) {
					const auto i = _thumbs.find(id);
					if (i == end(_thumbs) || i->second.requested != key) {
						return;
					}
					image.setDevicePixelRatio(ratio);
					i->second.image = std::move(image);
					update();
				}));
		}
		if (maskChanged && layer.mask) {
			// The guard is made here, on the main thread.
			auto done = crl::guard(this, [=](QImage image) {
				const auto i = _thumbs.find(id);
				if (i == end(_thumbs) || i->second.maskSerial != serial) {
					return;
				}
				image.setDevicePixelRatio(ratio);
				i->second.mask = std::move(image);
				update();
			});
			crl::async([
					side,
					mask = layer.mask,
					done = std::move(done)]() mutable {
				auto image = mask->image().scaled(
					QSize(side, side),
					Qt::KeepAspectRatio,
					Qt::SmoothTransformation);
				crl::on_main([
						image = std::move(image),
						done = std::move(done)]() mutable {
					done(std::move(image));
				});
			});
		}
	}
	update();
}

void LayersList::ensureVisible(LayerId id) {
	const auto row = rowOf(id);
	if (row < 0 || _dragging) {
		return;
	}
	const auto rect = rowRect(row);
	const auto top = _scroll->scrollTop();
	if (rect.y() < top || rect.y() + rect.height() > top + _scroll->height()) {
		_scroll->scrollToY(rect.y(), rect.y() + rect.height());
	}
}

int LayersList::resizeGetHeight(int newWidth) {
	return std::max(count(), 1) * Px(kRowHeight);
}

void LayersList::setOver(Hit over) {
	if (_over == over) {
		return;
	}
	_over = over;
	setCursor(layerAt(over.row) ? style::cur_pointer : style::cur_default);
	if (tooltipText().isEmpty()) {
		Ui::Tooltip::Hide();
	} else {
		Ui::Tooltip::Show(kTooltipDelay, this);
	}
	update();
}

QString LayersList::tooltipText() const {
	const auto layer = _dragging ? nullptr : layerAt(_over.row);
	if (!layer) {
		return QString();
	}
	switch (_over.part) {
	case Part::Eye:
		return (layer->visible
			? tr::lng_oblivion_photo_layers_hide
			: tr::lng_oblivion_photo_layers_show)(tr::now);
	case Part::Lock:
		return (layer->locked
			? tr::lng_oblivion_photo_layers_unlock
			: tr::lng_oblivion_photo_layers_lock)(tr::now);
	case Part::Mask:
		return tr::lng_oblivion_photo_layers_mask_paint(tr::now);
	case Part::Row:
	case Part::None: break;
	}
	return QString();
}

QPoint LayersList::tooltipPos() const {
	return QCursor::pos();
}

bool LayersList::tooltipWindowActive() const {
	return Ui::AppInFocus() && Ui::InFocusChain(window());
}

void LayersList::mouseMoveEvent(QMouseEvent *e) {
	if (_dragging) {
		updateDrag(e->pos());
		return;
	} else if (_pressed && (e->buttons() & Qt::LeftButton)) {
		const auto distance = (e->pos() - _pressPoint).manhattanLength();
		if (distance >= Px(kDragThreshold) && count() > 1) {
			_dragging = true;
			_dragId = _pressed;
			setOver({});
			setCursor(style::cur_sizeall);
			_autoScroll.callEach(kAutoScrollInterval);
			updateDrag(e->pos());
		}
		return;
	}
	setOver(hitTest(e->pos()));
}

void LayersList::leaveEventHook(QEvent *e) {
	Ui::Tooltip::Hide();
	if (!_dragging) {
		setOver({});
	}
	RpWidget::leaveEventHook(e);
}

void LayersList::hideEvent(QHideEvent *e) {
	// The release that ends a drag never comes to a list that was hidden
	// in the middle of it (another tab was chosen with a key): without
	// this the next click would drop the row where it was left.
	Ui::Tooltip::Hide();
	finishDrag(false);
	_pressed = 0;
	RpWidget::hideEvent(e);
}

void LayersList::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton || _dragging) {
		return;
	}
	// What it said is not true after the click.
	Ui::Tooltip::Hide();
	finishRename(true, false);
	const auto hit = hitTest(e->pos());
	const auto layer = layerAt(hit.row);
	if (!layer || _controller->busy()) {
		return;
	}
	const auto id = layer->id;
	switch (hit.part) {
	case Part::Eye:
		_controller->changeLayer(id, [](Layer &layer) {
			layer.visible = !layer.visible;
		});
		return;
	case Part::Lock:
		_controller->changeLayer(id, [](Layer &layer) {
			layer.locked = !layer.locked;
		});
		return;
	case Part::Mask:
		if (e->modifiers() & Qt::ShiftModifier) {
			ToggleLayerMask(_controller, id);
		} else {
			_controller->setActiveLayer(id);
			_controller->setTool(kMaskTool);
		}
		return;
	case Part::Row:
	case Part::None: break;
	}
	_pressed = id;
	_pressPoint = e->pos();
	_pressOffset = e->pos().y() - rowRect(hit.row).y();
	_controller->setActiveLayer(id);
}

void LayersList::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	_pressed = 0;
	if (_dragging) {
		finishDrag(true);
		setOver(hitTest(e->pos()));
	}
}

void LayersList::mouseDoubleClickEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto hit = hitTest(e->pos());
	const auto layer = layerAt(hit.row);
	if (!layer) {
		return;
	} else if (hit.part != Part::Row) {
		// Quick clicks on the eye keep switching it.
		mousePressEvent(e);
		return;
	}
	const auto places = geometry(*layer, rowRect(hit.row));
	if (e->pos().x() >= places.textLeft - Px(kTextSkip)) {
		startRename(layer->id);
	}
}

void LayersList::contextMenuEvent(QContextMenuEvent *e) {
	if (_dragging) {
		return;
	}
	const auto layer = layerAt(hitTest(e->pos()).row);
	if (!layer) {
		return;
	}
	const auto id = layer->id;
	_controller->setActiveLayer(id);
	showMenuFor(id, e->globalPos());
}

void LayersList::showMenuFor(LayerId id, QPoint globalPosition) {
	if (!_controller->document().find(id) || _controller->busy()) {
		return;
	}
	finishRename(true, false);
	_pressed = 0;
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::groupCallPopupMenu);
	FillLayerMenu(_menu.get(), _controller, id, [=] {
		// After the menu is gone, or it takes the focus back.
		crl::on_main(this, [=] {
			startRename(id);
		});
	});
	_menu->popup(globalPosition);
}

void LayersList::updateDrag(QPoint point) {
	const auto height = Px(kRowHeight);
	_dragTop = std::clamp(
		point.y() - _pressOffset,
		-height / 2,
		std::max(this->height() - height / 2, 0));
	_dragGap = std::clamp((_dragTop + height) / height, 0, count());
	update();
}

void LayersList::finishDrag(bool apply) {
	if (!_dragging) {
		return;
	}
	_autoScroll.cancel();
	_dragging = false;
	const auto id = base::take(_dragId);
	const auto gap = _dragGap;
	setCursor(layerAt(_over.row) ? style::cur_pointer : style::cur_default);
	update();
	if (!apply || _controller->busy()) {
		return;
	}
	const auto total = count();
	const auto index = LayerDropIndex(total, rowOf(id), gap);
	if (index >= 0) {
		_controller->change([=](Document &document) {
			MoveLayer(document, id, index);
		});
	}
}

void LayersList::autoScroll() {
	if (!_dragging) {
		_autoScroll.cancel();
		return;
	}
	const auto global = QCursor::pos();
	const auto y = _scroll->mapFromGlobal(global).y();
	const auto zone = Px(kAutoScrollZone);
	const auto delta = (y < zone)
		? -Px(kAutoScrollStep)
		: (y > _scroll->height() - zone)
		? Px(kAutoScrollStep)
		: 0;
	if (!delta) {
		return;
	}
	const auto top = std::clamp(
		_scroll->scrollTop() + delta,
		0,
		_scroll->scrollTopMax());
	if (top != _scroll->scrollTop()) {
		_scroll->scrollToY(top);
		updateDrag(mapFromGlobal(global));
	}
}

void LayersList::startRename(LayerId id) {
	if (_dragging || _controller->busy()) {
		return;
	}
	// Before the layer is looked up: it replaces the document.
	finishRename(true, false);
	const auto layer = _controller->document().find(id);
	if (!layer) {
		return;
	}
	if (!_renameHost) {
		_renameHost = Ui::CreateChild<FieldHost>(this);
		_renameField = Ui::CreateChild<Ui::InputField>(
			_renameHost,
			NameFieldStyle(),
			Ui::InputField::Mode::SingleLine,
			nullptr,
			QString());
		_renameField->setMaxLength(kNameLimit);
		_renameField->submits() | rpl::on_next([=] {
			finishRename(true, true);
		}, _renameField->lifetime());
		_renameField->cancelled() | rpl::on_next([=] {
			finishRename(false, true);
		}, _renameField->lifetime());
		// The focus went somewhere else: keep the typed name and leave the
		// focus where it is. The menu of the field itself (right click:
		// paste, cut...) takes the focus too, the field stays for it.
		const auto field = _renameField;
		field->focusedChanges() | rpl::filter(
			!rpl::mappers::_1
		) | rpl::on_next([=] {
			if (!field->menuShown()) {
				finishRename(true, false);
			}
		}, field->lifetime());
		// The menu is gone: the field has the focus back, or it went
		// somewhere else while the menu was there. Checked later, when
		// both are settled (and not at all if the field is gone by then:
		// the menu is closed by the destructor of the field as well).
		field->menuShownValue() | rpl::skip(1) | rpl::filter(
			!rpl::mappers::_1
		) | rpl::on_next([=] {
			crl::on_main(field, [=] {
				if (_renaming && !field->menuShown() && !field->hasFocus()) {
					finishRename(true, false);
				}
			});
		}, field->lifetime());
	}
	_renaming = id;
	ensureVisible(id);
	_renameField->setText(layer->name);
	placeRename();
	_renameHost->show();
	_renameHost->raise();
	_renameField->selectAll();
	_renameField->setFocusFast();
	update();
}

void LayersList::placeRename() {
	if (!_renaming || !_renameHost) {
		return;
	}
	const auto row = rowOf(_renaming);
	const auto layer = layerAt(row);
	if (!layer) {
		return;
	}
	const auto rect = rowRect(row);
	const auto places = geometry(*layer, rect);
	const auto left = places.textLeft - Px(8);
	const auto height = Px(kFieldHeight);
	const auto fieldWidth = std::max(
		rect.x() + rect.width() - Px(kRowRight) - left,
		Px(60));
	_renameHost->setGeometry(
		left,
		rect.y() + (rect.height() - height) / 2,
		fieldWidth,
		height);
	_renameField->setGeometry(0, 0, fieldWidth, height);
}

void LayersList::finishRename(bool apply, bool refocus) {
	if (!_renaming) {
		return;
	}
	// Hiding the field takes the focus from it and brings us here again.
	const auto id = base::take(_renaming);
	const auto name = _renameField->getLastText().simplified().left(
		kNameLimit);
	if (refocus) {
		FocusEditor(this);
	}
	_renameHost->hide();
	update();
	const auto layer = _controller->document().find(id);
	if (apply && layer && !name.isEmpty() && layer->name != name) {
		_controller->changeLayer(id, [=](Layer &layer) {
			layer.name = name;
		});
	}
}

void LayersList::paintRow(
		QPainter &p,
		const Layer &layer,
		QRect rect,
		bool floating) {
	const auto active = (layer.id == _controller->activeLayerId());
	const auto over = !_dragging && (layerAt(_over.row) == &layer);
	const auto radius = Px(kRowRadius);
	const auto plate = rect.marginsRemoved(
		QMargins(Px(6), Px(2), Px(6), Px(2)));
	p.setPen(Qt::NoPen);
	if (floating) {
		p.setBrush(st::groupCallMembersBg);
		p.drawRoundedRect(plate, radius, radius);
		p.setBrush(st::groupCallMembersBgOver);
		p.setPen(QPen(AccentColor(), std::max(Px(1), 1)));
		p.drawRoundedRect(plate, radius, radius);
		p.setPen(Qt::NoPen);
	} else if (active || over) {
		p.setBrush(active
			? anim::with_alpha(AccentColor(), 0.16)
			: st::groupCallMembersBgOver->c);
		p.drawRoundedRect(plate, radius, radius);
	}

	const auto places = geometry(layer, rect);
	const auto alpha = layer.visible ? 1. : kDimmedOpacity;
	const auto thumbRadius = Px(kThumbRadius);
	const auto kind = layer.content
		? FindLayerKind(layer.content->type())
		: nullptr;
	const auto i = _thumbs.find(layer.id);
	const auto thumb = (i != end(_thumbs)) ? &i->second : nullptr;

	PaintChecks(p, places.thumb, thumbRadius);
	if (thumb && !thumb->image.isNull()) {
		const auto &image = thumb->image;
		const auto size = image.size() / image.devicePixelRatio();
		// A picture that fills the whole square keeps its round corners.
		p.save();
		auto clip = QPainterPath();
		clip.addRoundedRect(QRectF(places.thumb), thumbRadius, thumbRadius);
		p.setClipPath(clip);
		p.setOpacity(alpha);
		p.drawImage(
			QPointF(
				places.thumb.x() + (places.thumb.width() - size.width()) / 2.,
				places.thumb.y()
					+ (places.thumb.height() - size.height()) / 2.),
			image);
		p.restore();
	}

	if (!places.mask.isEmpty()) {
		const auto painting = active && (_controller->toolId() == kMaskTool);
		const auto maskReady = thumb && !thumb->mask.isNull();
		p.setPen(Qt::NoPen);
		// Black is "everything is hidden": not while the small picture of
		// the mask is still being made.
		p.setBrush(maskReady
			? QColor(0, 0, 0)
			: st::groupCallMembersBgOver->c);
		p.drawRoundedRect(places.mask, thumbRadius, thumbRadius);
		if (maskReady) {
			const auto &image = thumb->mask;
			const auto size = image.size() / image.devicePixelRatio();
			p.save();
			auto clip = QPainterPath();
			clip.addRoundedRect(QRectF(places.mask), thumbRadius, thumbRadius);
			p.setClipPath(clip);
			p.setOpacity(layer.maskEnabled ? alpha : (alpha * 0.5));
			p.drawImage(
				QPointF(
					places.mask.x()
						+ (places.mask.width() - size.width()) / 2.,
					places.mask.y()
						+ (places.mask.height() - size.height()) / 2.),
				image);
			p.restore();
		}
		p.setBrush(Qt::NoBrush);
		if (!layer.maskEnabled) {
			auto pen = QPen(QColor(0xE5, 0x39, 0x35), Px(2));
			pen.setCapStyle(Qt::RoundCap);
			p.setPen(pen);
			const auto inner = QRectF(places.mask).marginsRemoved(
				QMarginsF(Px(5), Px(5), Px(5), Px(5)));
			p.drawLine(inner.topLeft(), inner.bottomRight());
			p.drawLine(inner.topRight(), inner.bottomLeft());
		}
		p.setPen(QPen(
			painting ? AccentColor() : st::groupCallMemberInactiveIcon->c,
			painting ? Px(2) : std::max(Px(1), 1)));
		p.drawRoundedRect(
			QRectF(places.mask).marginsRemoved(
				QMarginsF(0.5, 0.5, 0.5, 0.5)),
			thumbRadius,
			thumbRadius);
	}

	auto textLeft = places.textLeft;
	if (kind && kind->paintIcon && kind->type != "image") {
		const auto side = Px(kKindIcon);
		p.save();
		p.setRenderHint(QPainter::Antialiasing);
		kind->paintIcon(
			p,
			QRectF(
				textLeft,
				rect.y() + (rect.height() - side) / 2.,
				side,
				side),
			anim::with_alpha(SubTextColor(), alpha));
		p.restore();
		textLeft += side + Px(6);
	}
	const auto textWidth = std::max(places.textRight - textLeft, 0);
	const auto renaming = (_renaming == layer.id) && !floating;
	if (!renaming && textWidth > 0) {
		const auto percent = int(std::lround(layer.opacity * 100.));
		const auto info = (layer.blend != BlendMode::Normal)
			? ((percent < 100)
				? tr::lng_oblivion_photo_layers_blend_info(
					tr::now,
					lt_name,
					BlendModeName(layer.blend),
					lt_percent,
					PercentText(percent))
				: BlendModeName(layer.blend))
			: (percent < 100)
			? PercentText(percent)
			: QString();
		const auto &font = st::normalFont;
		const auto &minor = SmallFont();
		const auto lines = info.isEmpty()
			? font->height
			: (font->height + minor->height);
		const auto top = rect.y() + (rect.height() - lines) / 2;
		p.setFont(font);
		p.setPen(anim::with_alpha(
			active ? AccentColor() : TextColor(),
			alpha));
		p.drawText(
			QRect(textLeft, top, textWidth, font->height),
			Qt::AlignLeft | Qt::AlignVCenter,
			font->elided(layer.name, textWidth));
		if (!info.isEmpty()) {
			p.setFont(minor);
			p.setPen(anim::with_alpha(SubTextColor(), alpha));
			p.drawText(
				QRect(textLeft, top + font->height, textWidth, minor->height),
				Qt::AlignLeft | Qt::AlignVCenter,
				minor->elided(info, textWidth));
		}
	}

	const auto unit = Px(24) / 24.;
	if (layer.locked || over || floating) {
		const auto overLock = over && (_over.part == Part::Lock);
		PaintLockGlyph(
			p,
			QPointF(places.lock.center()) + QPointF(0.5, 0.5),
			unit,
			layer.locked
				? TextColor()
				: anim::with_alpha(SubTextColor(), overLock ? 1. : 0.5),
			layer.locked);
	}
	const auto overEye = over && (_over.part == Part::Eye);
	PaintEyeGlyph(
		p,
		QPointF(places.eye.center()) + QPointF(0.5, 0.5),
		unit,
		overEye
			? TextColor()
			: anim::with_alpha(SubTextColor(), layer.visible ? 1. : 0.6),
		layer.visible);
}

void LayersList::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto total = count();
	if (!total) {
		p.setFont(st::normalFont);
		p.setPen(SubTextColor());
		p.drawText(
			QRect(Px(kPadding), 0, width() - 2 * Px(kPadding), Px(kRowHeight)),
			Qt::AlignLeft | Qt::AlignVCenter,
			tr::lng_oblivion_photo_layers_empty(tr::now));
		return;
	}
	const auto &layers = _controller->document().layers;
	const auto dragged = _dragging
		? _controller->document().find(_dragId)
		: nullptr;
	for (auto row = 0; row != total; ++row) {
		const auto rect = rowRect(row);
		if (!rect.intersects(e->rect())) {
			continue;
		}
		const auto &layer = layers[total - 1 - row];
		if (&layer == dragged) {
			// The place the row was taken from.
			p.setPen(Qt::NoPen);
			p.setBrush(anim::with_alpha(st::groupCallMembersBgOver->c, 0.5));
			p.drawRoundedRect(
				rect.marginsRemoved(QMargins(Px(6), Px(2), Px(6), Px(2))),
				Px(kRowRadius),
				Px(kRowRadius));
			continue;
		}
		paintRow(p, layer, rect, false);
	}
	if (dragged) {
		const auto height = Px(kRowHeight);
		const auto line = std::max(Px(2), 2);
		const auto from = rowOf(_dragId);
		if (LayerDropIndex(total, from, _dragGap) >= 0) {
			p.setPen(Qt::NoPen);
			p.setBrush(AccentColor());
			p.drawRoundedRect(
				QRectF(
					Px(10),
					std::clamp(
						_dragGap * height - line / 2,
						0,
						this->height() - line),
					width() - 2 * Px(10),
					line),
				line / 2.,
				line / 2.);
		}
		paintRow(p, *dragged, QRect(0, _dragTop, width(), height), true);
	}
}

// What the editor shows in the layers slot.
class LayersPanel final : public Ui::RpWidget {
public:
	LayersPanel(QWidget *parent, not_null<Controller*> controller);

	void startRename(LayerId id);

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	void showAddMenu();

	const not_null<Controller*> _controller;
	Ui::ScrollArea *_scroll = nullptr;
	LayersList *_list = nullptr;
	ToolButton *_add = nullptr;
	ToolButton *_more = nullptr;
	base::unique_qptr<Ui::PopupMenu> _menu;

};

LayersPanel::LayersPanel(QWidget *parent, not_null<Controller*> controller)
: RpWidget(parent)
, _controller(controller) {
	_scroll = Ui::CreateChild<Ui::ScrollArea>(this, PanelScrollStyle());
	_list = _scroll->setOwnedWidget(
		object_ptr<LayersList>(_scroll, controller, _scroll)).data();
	_scroll->show();

	const auto size = Px(kHeaderButton);
	_add = Ui::CreateChild<ToolButton>(
		this,
		IconRef{ .paint = PaintPlusIcon },
		size);
	_add->setTooltip(tr::lng_oblivion_photo_layers_add(tr::now));
	_add->setClickedCallback([=] { showAddMenu(); });
	_add->show();
	_more = Ui::CreateChild<ToolButton>(
		this,
		IconRef{ .paint = PaintDotsIcon },
		size);
	_more->setTooltip(tr::lng_oblivion_photo_layers_more(tr::now));
	_more->setClickedCallback([=] {
		_list->showMenuFor(_controller->activeLayerId(), QCursor::pos());
	});
	_more->show();

	rpl::merge(
		_controller->documentChanges(),
		_controller->activeLayerValue() | rpl::to_empty
	) | rpl::on_next([=] {
		_more->setAvailable(_controller->hasDocument()
			&& (_controller->activeLayer() != nullptr));
		_add->setAvailable(_controller->hasDocument());
	}, lifetime());
	_more->setAvailable(_controller->hasDocument()
		&& (_controller->activeLayer() != nullptr));
	_add->setAvailable(_controller->hasDocument());
}

void LayersPanel::startRename(LayerId id) {
	_list->startRename(id);
}

void LayersPanel::showAddMenu() {
	if (!_controller->hasDocument() || _controller->busy()) {
		return;
	}
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::groupCallPopupMenu);
	const auto controller = _controller;
	for (const auto kind : AllLayerKinds()) {
		if (!kind->create) {
			continue;
		}
		const auto create = kind->create;
		_menu->addAction(kind->name.now(), [=] {
			if (controller->hasDocument() && !controller->busy()) {
				create(controller);
			}
		});
	}
	_menu->addAction(tr::lng_oblivion_photo_layers_paste(tr::now), [=] {
		PasteLayerFromClipboard(controller);
	});
	_menu->popup(QCursor::pos());
}

void LayersPanel::resizeEvent(QResizeEvent *e) {
	const auto top = Px(kHeaderHeight);
	_scroll->setGeometry(0, top, width(), std::max(height() - top, 0));
	_list->resizeToWidth(width());
	const auto size = Px(kHeaderButton);
	const auto y = (top - size) / 2;
	auto right = width() - Px(kHeaderButtonsRight);
	_more->move(right - size, y);
	right -= size + Px(2);
	_add->move(right - size, y);
}

void LayersPanel::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto &font = SmallSemiboldFont();
	p.setFont(font);
	p.setPen(SubTextColor());
	p.drawText(
		QRect(
			Px(kPadding),
			0,
			std::max(_add->x() - Px(kSkip) - Px(kPadding), 0),
			Px(kHeaderHeight)),
		Qt::AlignLeft | Qt::AlignVCenter,
		tr::lng_oblivion_photo_layers_title(tr::now));
}

//
// The transform tool.
//

[[nodiscard]] QCursor ResizeCursor(QPointF direction) {
	if (std::hypot(direction.x(), direction.y()) < 1e-6) {
		return QCursor(Qt::SizeAllCursor);
	}
	auto angle = std::atan2(direction.y(), direction.x()) * 180. / kPi;
	if (angle < 0.) {
		angle += 180.;
	}
	if (angle < 22.5 || angle >= 157.5) {
		return QCursor(Qt::SizeHorCursor);
	} else if (angle < 67.5) {
		return QCursor(Qt::SizeFDiagCursor);
	} else if (angle < 112.5) {
		return QCursor(Qt::SizeVerCursor);
	}
	return QCursor(Qt::SizeBDiagCursor);
}

// A bent arrow: there is no standard "rotate" cursor.
[[nodiscard]] QCursor RotateCursor() {
	static const auto result = [] {
		const auto ratio = style::DevicePixelRatio();
		const auto side = 24;
		auto image = QImage(
			QSize(side, side) * ratio,
			QImage::Format_ARGB32_Premultiplied);
		image.setDevicePixelRatio(ratio);
		image.fill(Qt::transparent);
		{
			auto p = QPainter(&image);
			p.setRenderHint(QPainter::Antialiasing);
			const auto arc = QRectF(5.5, 5.5, 13., 13.);
			auto path = QPainterPath();
			path.arcMoveTo(arc, 20.);
			path.arcTo(arc, 20., 250.);
			const auto tip = path.currentPosition();
			auto head = QPainterPath();
			head.moveTo(tip + QPointF(4.5, 0.));
			head.lineTo(tip + QPointF(-0.5, -3.6));
			head.lineTo(tip + QPointF(-0.5, 3.6));
			head.closeSubpath();
			for (const auto outline : { true, false }) {
				auto pen = QPen(
					outline ? QColor(255, 255, 255) : QColor(0, 0, 0),
					outline ? 4.2 : 1.8);
				pen.setCapStyle(Qt::RoundCap);
				pen.setJoinStyle(Qt::RoundJoin);
				p.setPen(pen);
				p.setBrush(Qt::NoBrush);
				p.drawPath(path);
				p.setPen(outline
					? QPen(QColor(255, 255, 255), 2.4)
					: QPen(Qt::NoPen));
				p.setBrush(outline ? QColor(255, 255, 255) : QColor(0, 0, 0));
				p.drawPath(head);
			}
		}
		// Never destroyed, like the state of the tools.
		return new QCursor(
			QPixmap::fromImage(std::move(image)),
			side / 2,
			side / 2);
	}();
	return *result;
}

void PaintFrame(QPainter &p, const QPolygonF &quad, QColor color) {
	const auto line = std::max(Px(3) / 2., 1.);
	p.setBrush(Qt::NoBrush);
	p.setPen(QPen(QColor(0, 0, 0, 90), line + 2.));
	p.drawPolygon(quad);
	p.setPen(QPen(color, line));
	p.drawPolygon(quad);
}

void PaintHandle(
		QPainter &p,
		QPointF center,
		double side,
		bool round,
		bool highlighted) {
	const auto rect = QRectF(
		center.x() - side / 2.,
		center.y() - side / 2.,
		side,
		side);
	p.setPen(QPen(QColor(0, 0, 0, 110), std::max(Px(1), 1) + 2.));
	p.setBrush(Qt::NoBrush);
	if (round) {
		p.drawEllipse(rect);
	} else {
		p.drawRoundedRect(rect, 2., 2.);
	}
	p.setPen(QPen(AccentColor(), std::max(Px(3) / 2., 1.)));
	p.setBrush(highlighted ? AccentColor() : QColor(255, 255, 255));
	if (round) {
		p.drawEllipse(rect);
	} else {
		p.drawRoundedRect(rect, 2., 2.);
	}
}

void PaintBadge(
		QPainter &p,
		const QString &text,
		QPointF anchor,
		QRect bounds) {
	if (text.isEmpty()) {
		return;
	}
	const auto &font = SmallSemiboldFont();
	const auto padding = Px(kBadgePadding);
	const auto width = font->width(text) + 2 * padding;
	const auto height = font->height + padding;
	auto left = anchor.x() + Px(kBadgeOffset);
	auto top = anchor.y() + Px(kBadgeOffset);
	if (left + width > bounds.x() + bounds.width() - padding) {
		left = anchor.x() - Px(kBadgeOffset) - width;
	}
	if (top + height > bounds.y() + bounds.height() - padding) {
		top = anchor.y() - Px(kBadgeOffset) - height;
	}
	const auto rect = QRectF(left, top, width, height);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0, 0, 0, 190));
	p.drawRoundedRect(rect, height / 2., height / 2.);
	p.setFont(font);
	p.setPen(QColor(255, 255, 255));
	p.drawText(rect, Qt::AlignCenter, text);
}

void PaintGuides(
		QPainter &p,
		const ToolPaintContext &context,
		const Document &document,
		std::optional<double> guideX,
		std::optional<double> guideY) {
	if (!guideX && !guideY) {
		return;
	}
	const auto size = QSizeF(document.size);
	p.save();
	p.setClipPath(VisiblePath(document, context.documentToWidget));
	p.setPen(QPen(GuideColor(), std::max(Px(1), 1)));
	if (guideX) {
		p.drawLine(
			context.documentToWidget.map(QPointF(*guideX, 0.)),
			context.documentToWidget.map(QPointF(*guideX, size.height())));
	}
	if (guideY) {
		p.drawLine(
			context.documentToWidget.map(QPointF(0., *guideY)),
			context.documentToWidget.map(QPointF(size.width(), *guideY)));
	}
	p.restore();
}

class TransformTool final : public Tool, public base::has_weak_ptr {
public:
	explicit TransformTool(not_null<Controller*> controller);

	void deactivated() override;
	bool mousePress(const ToolMouseEvent &e) override;
	void mouseMove(const ToolMouseEvent &e) override;
	void mouseRelease(const ToolMouseEvent &e) override;
	void mouseLeave() override;
	bool keyPress(not_null<QKeyEvent*> e) override;
	bool keyRelease(not_null<QKeyEvent*> e) override;
	bool cancel() override;
	void paint(QPainter &p, const ToolPaintContext &context) override;
	QCursor cursor(const ToolMouseEvent &e) override;

private:
	enum class Action : uchar {
		None,
		Move,
		Scale,
		Rotate,
		Corner,
		Skew,
	};
	struct Drag {
		Action action = Action::None;
		TransformHandle handle = TransformHandle::None;
		LayerId layer = 0;
		QTransform start;
		QTransform current;
		QSizeF content;
		QPointF press; // Canvas coordinates.
		QPointF grab; // From the cursor to the handle.
		QPointF pivot;
		double rotation = 0.;
		bool affine = true;
		SnapTargets targets;
		bool moved = false;
		// A click without a move chooses this layer instead.
		LayerId click = 0;
		std::optional<double> guideX;
		std::optional<double> guideY;
		QString badge;
	};
	// What a press at a point works with.
	struct Pick {
		LayerId move = 0; // The layer a drag moves.
		LayerId click = 0; // The layer a plain click chooses, if another.
	};
	struct HitMap {
		uint64 requested = 0;
		QImage map;
	};

	[[nodiscard]] HandleMetrics metrics() const;
	[[nodiscard]] TransformHandle hitTest(
		const Layer &layer,
		const ToolMouseEvent &e) const;
	[[nodiscard]] Pick pick(const ToolMouseEvent &e) const;
	[[nodiscard]] QCursor handleCursor(
		const Layer &layer,
		TransformHandle handle) const;
	void startDrag(
		const Layer &layer,
		TransformHandle handle,
		const ToolMouseEvent &e);
	void updateDrag(const ToolMouseEvent &e);
	void finishDrag();
	void modifiersChanged();
	void refreshMaps();
	// direction: where on the screen, one of the four.
	bool nudge(QPointF direction, bool big);

	const not_null<Controller*> _controller;
	Drag _drag;
	ToolMouseEvent _last;
	// The canvas widget as the last paint saw it: a mouse event does not
	// say how large the canvas is.
	QRectF _viewRect;
	TransformHandle _hover = TransformHandle::None;
	base::flat_map<LayerId, HitMap> _maps;
	base::Timer _mapsTimer;
	rpl::lifetime _lifetime;

};

TransformTool::TransformTool(not_null<Controller*> controller)
: _controller(controller)
, _mapsTimer([=] { refreshMaps(); }) {
	_controller->documentChanges() | rpl::on_next([=] {
		if (!_mapsTimer.isActive()) {
			_mapsTimer.callOnce(kHitMapsDelay);
		}
		_controller->updateCanvas();
	}, _lifetime);
	_controller->activeLayerValue() | rpl::skip(1) | rpl::on_next([=] {
		_hover = TransformHandle::None;
		_controller->updateCanvas();
	}, _lifetime);
	State().perspective.changes() | rpl::on_next([=] {
		_hover = TransformHandle::None;
		_controller->updateCanvas();
	}, _lifetime);
	refreshMaps();
}

void TransformTool::deactivated() {
	finishDrag();
	_lifetime.destroy();
	_mapsTimer.cancel();
}

HandleMetrics TransformTool::metrics() const {
	const auto perspective = State().perspective.current();
	return {
		.radius = double(Px(kHandleCatch)),
		.rotateOffset = double(Px(kRotateOffset)),
		.edges = !perspective,
		.rotate = !perspective,
		.view = _viewRect,
	};
}

void TransformTool::refreshMaps() {
	if (!_controller->hasDocument()) {
		_maps.clear();
		return;
	}
	const auto &layers = _controller->document().layers;
	for (auto i = begin(_maps); i != end(_maps);) {
		if (ranges::contains(layers, i->first, &Layer::id)) {
			++i;
		} else {
			i = _maps.erase(i);
		}
	}
	for (const auto &layer : layers) {
		if (!HasPixels(&layer)) {
			continue;
		}
		const auto id = layer.id;
		const auto key = LayerThumbnailKey(layer) | 1;
		auto &entry = _maps[id];
		if (entry.requested == key) {
			continue;
		}
		entry.requested = key;
		_controller->requestLayerThumbnail(
			id,
			QSize(kHitMapSide, kHitMapSide),
			crl::guard(this, [=](QImage image) {
				const auto i = _maps.find(id);
				if (i != end(_maps) && i->second.requested == key) {
					i->second.map = AlphaMapFromPixels(image);
				}
			}));
	}
}

TransformHandle TransformTool::hitTest(
		const Layer &layer,
		const ToolMouseEvent &e) const {
	const auto quad = WidgetQuad(layer, _controller->documentToWidget());
	return FiniteQuad(quad)
		? HitTestTransform(quad, e.widget, metrics())
		: TransformHandle::None;
}

// A press on the pixels of a layer works with that layer. The frame of
// the active layer is there to be dragged too: where the active layer is
// transparent a drag inside of its frame still moves it, and only a plain
// click goes through to the layer under it.
TransformTool::Pick TransformTool::pick(const ToolMouseEvent &e) const {
	const auto &document = _controller->document();
	const auto maps = [&](LayerId id) -> const QImage* {
		const auto i = _maps.find(id);
		return (i != end(_maps) && !i->second.map.isNull())
			? &i->second.map
			: nullptr;
	};
	const auto target = LayerAtPixels(document, e.document, maps);
	const auto active = _controller->activeLayer();
	if (Usable(active) && !active->locked) {
		const auto point = LayerPoint(*active, e.document);
		const auto inside = point
			&& QRectF(QPointF(), QSizeF(active->size())).contains(*point);
		const auto above = target
			&& (document.indexOf(target) > document.indexOf(active->id));
		if (inside && !above) {
			return {
				.move = active->id,
				.click = (target != active->id) ? target : LayerId(0),
			};
		}
	}
	return { .move = target ? target : LayerAt(document, e.document) };
}

bool TransformTool::mousePress(const ToolMouseEvent &e) {
	if (e.button != Qt::LeftButton
		|| !_controller->hasDocument()
		|| _controller->busy()) {
		return false;
	}
	finishDrag();
	_last = e;
	const auto active = _controller->activeLayer();
	if (Usable(active)) {
		const auto handle = hitTest(*active, e);
		const auto anywhere = (e.modifiers & Qt::ControlModifier)
			&& (handle == TransformHandle::None
				|| handle == TransformHandle::Move);
		if (anywhere
			|| (handle != TransformHandle::None
				&& handle != TransformHandle::Move)) {
			if (active->locked) {
				ToastLocked(_controller);
				return true;
			}
			startDrag(*active, anywhere ? TransformHandle::Move : handle, e);
			return true;
		}
	}
	const auto picked = pick(e);
	const auto target = picked.move;
	const auto layer = _controller->document().find(target);
	if (!layer) {
		if (HasPixels(active)) {
			const auto point = LayerPoint(*active, e.document);
			const auto inside = point
				&& QRectF(QPointF(), QSizeF(active->size())).contains(*point);
			if (inside && active->locked) {
				ToastLocked(_controller);
				return true;
			} else if (inside && !active->visible) {
				_controller->showToast(
					tr::lng_oblivion_photo_layers_hidden(tr::now));
				return true;
			}
		}
		// Nothing to move here: the canvas pans.
		return false;
	}
	_controller->setActiveLayer(target);
	if (const auto now = _controller->document().find(target)) {
		startDrag(*now, TransformHandle::Move, e);
		_drag.click = picked.click;
	}
	return true;
}

void TransformTool::startDrag(
		const Layer &layer,
		TransformHandle handle,
		const ToolMouseEvent &e) {
	// Whatever was being changed before becomes its own undo step.
	const auto id = layer.id;
	const auto transform = layer.transform;
	const auto content = QSizeF(layer.size());
	_controller->commit();

	const auto command = (e.modifiers & Qt::ControlModifier) != 0;
	const auto freely = State().perspective.current() || command;
	auto drag = Drag();
	drag.handle = handle;
	drag.action = (handle == TransformHandle::Move)
		? Action::Move
		: (handle == TransformHandle::Rotate)
		? Action::Rotate
		: (IsCornerHandle(handle) && freely)
		? Action::Corner
		: (IsEdgeHandle(handle) && command)
		? Action::Skew
		: Action::Scale;
	drag.layer = id;
	drag.start = transform;
	drag.current = transform;
	drag.content = content;
	drag.press = e.document;
	drag.grab = (IsCornerHandle(handle) || IsEdgeHandle(handle))
		? (transform.map(HandleLocalPoint(handle, content)) - e.document)
		: QPointF();
	drag.pivot = transform.map(
		QPointF(content.width() / 2., content.height() / 2.));
	drag.affine = transform.isAffine();
	drag.rotation = DecomposeTransform(transform, content).rotation;
	if (State().snapping.current()) {
		drag.targets = MakeSnapTargets(_controller->document(), id);
	}
	_drag = std::move(drag);
	_hover = handle;
	_controller->updateCanvas();
}

void TransformTool::updateDrag(const ToolMouseEvent &e) {
	if (_drag.action == Action::None) {
		return;
	}
	const auto layer = _controller->document().find(_drag.layer);
	if (!layer || layer->locked) {
		_drag = Drag();
		_controller->updateCanvas();
		return;
	}
	const auto &start = _drag.start;
	const auto content = _drag.content;
	const auto shift = (e.modifiers & Qt::ShiftModifier) != 0;
	const auto alt = (e.modifiers & Qt::AltModifier) != 0;
	const auto snapping = !_drag.targets.xs.empty();
	const auto threshold = Px(kSnapDistance) / std::max(e.scale, 1e-6);
	const auto offset = e.document - _drag.press;
	if (!_drag.moved
		&& (std::hypot(offset.x(), offset.y()) * e.scale
			< Px(kMoveThreshold))) {
		// A click only chooses the layer, and a click on a handle that
		// trembled by a pixel does not resize or turn it.
		return;
	}
	auto result = _drag.current;
	auto guideX = std::optional<double>();
	auto guideY = std::optional<double>();
	auto badge = QString();
	switch (_drag.action) {
	case Action::Move: {
		auto delta = offset;
		if (shift) {
			delta = ConstrainedDelta(delta);
		}
		result = MovedTransform(start, delta);
		if (snapping) {
			const auto snap = SnapRect(
				TransformedQuad(result, content).boundingRect(),
				_drag.targets,
				threshold,
				!shift || (delta.x() != 0.),
				!shift || (delta.y() != 0.));
			result = MovedTransform(result, snap.delta);
			guideX = snap.guideX;
			guideY = snap.guideY;
		}
	} break;
	case Action::Scale: {
		const auto corner = IsCornerHandle(_drag.handle);
		auto snap = SnapResult();
		result = ScaledTransform(start, content, {
			.handle = _drag.handle,
			.point = e.document + _drag.grab,
			.keepAspect = corner
				? (State().keepAspect.current() != shift)
				: shift,
			.aroundCenter = alt,
			.minSize = 1.,
			.targets = snapping ? &_drag.targets : nullptr,
			.threshold = threshold,
		}, &snap);
		guideX = snap.guideX;
		guideY = snap.guideY;
		if (result.isAffine()) {
			const auto numbers = NumbersFromTransform(result, content);
			badge = QString::number(int(std::lround(
				content.width() * numbers.width / 100.))
			) + QChar(0x00D7) + QString::number(int(std::lround(
				content.height() * numbers.height / 100.)));
		}
	} break;
	case Action::Rotate: {
		auto angle = AngleBetween(_drag.pivot, _drag.press, e.document);
		const auto total = _drag.affine ? (_drag.rotation + angle) : angle;
		if (shift) {
			angle += SnappedAngle(total, kRotateShiftStep) - total;
		} else if (snapping) {
			const auto nearest = SnappedAngle(total, kRotateSnapStep);
			if (std::abs(nearest - total) <= kRotateSnapNear) {
				angle += nearest - total;
			}
		}
		result = RotatedTransform(start, _drag.pivot, angle);
		// The number the Rotation field shows for the same layer (it
		// tells a mirrored layer by another angle than the plain sum).
		badge = FormatDecimal(
			_drag.affine
				? NumbersFromTransform(result, content).rotation
				: NormalizedAngle(angle),
			1) + QChar(0x00B0);
	} break;
	case Action::Corner: {
		auto point = e.document + _drag.grab;
		if (snapping) {
			const auto snap = SnapPoint(point, _drag.targets, threshold);
			point += snap.delta;
			guideX = snap.guideX;
			guideY = snap.guideY;
		}
		const auto moved = CornerMovedTransform(
			start,
			content,
			HandleCorner(_drag.handle),
			point);
		if (moved) {
			result = *moved;
		} else {
			// The frame would fold: the last good shape stays.
			guideX = std::nullopt;
			guideY = std::nullopt;
		}
	} break;
	case Action::Skew: {
		const auto skewed = EdgeSkewedTransform(
			start,
			content,
			_drag.handle,
			e.document - _drag.press);
		if (skewed) {
			result = *skewed;
		}
	} break;
	case Action::None: break;
	}
	if (!ValidTransform(result, content)) {
		return;
	}
	_drag.moved = true;
	_drag.current = result;
	_drag.guideX = guideX;
	_drag.guideY = guideY;
	_drag.badge = badge;
	_controller->changeLayer(_drag.layer, [&](Layer &layer) {
		layer.transform = result;
	}, false);
	_controller->updateCanvas();
}

void TransformTool::finishDrag() {
	if (_drag.action == Action::None) {
		return;
	}
	const auto drag = base::take(_drag);
	if (drag.moved) {
		_controller->commit();
	} else if (drag.action == Action::Move
		&& drag.click
		&& _controller->document().find(drag.click)) {
		_controller->setActiveLayer(drag.click);
	}
	_controller->updateCanvas();
}

void TransformTool::mouseMove(const ToolMouseEvent &e) {
	_last = e;
	if (_drag.action != Action::None) {
		if (e.buttons & Qt::LeftButton) {
			updateDrag(e);
		}
		return;
	}
	const auto active = _controller->hasDocument()
		? _controller->activeLayer()
		: nullptr;
	const auto hover = (Usable(active) && !active->locked)
		? hitTest(*active, e)
		: TransformHandle::None;
	if (_hover != hover) {
		_hover = hover;
		_controller->updateCanvas();
	}
}

void TransformTool::mouseRelease(const ToolMouseEvent &e) {
	_last = e;
	finishDrag();
}

void TransformTool::mouseLeave() {
	if (_drag.action == Action::None && _hover != TransformHandle::None) {
		_hover = TransformHandle::None;
		_controller->updateCanvas();
	}
}

void TransformTool::modifiersChanged() {
	if (_drag.action == Action::None || !_drag.moved) {
		return;
	}
	auto e = _last;
	e.modifiers = QGuiApplication::queryKeyboardModifiers();
	updateDrag(e);
}

bool TransformTool::nudge(QPointF direction, bool big) {
	const auto layer = _controller->hasDocument()
		? _controller->activeLayer()
		: nullptr;
	if (!Usable(layer)) {
		return false;
	} else if (!Editable(_controller, layer->id)) {
		return true;
	}
	const auto delta = SeenStep(
		SeenOrientation(_controller->document()),
		direction,
		big ? kNudgeBigStep : kNudgeStep,
		_controller->viewScale());
	const auto result = MovedTransform(layer->transform, delta);
	if (ValidTransform(result, QSizeF(layer->size()))) {
		// Not committed: a series of key presses is one undo step.
		_controller->changeLayer(layer->id, [&](Layer &layer) {
			layer.transform = result;
		}, false);
	}
	return true;
}

bool TransformTool::keyPress(not_null<QKeyEvent*> e) {
	const auto key = e->key();
	if (key == Qt::Key_Shift
		|| key == Qt::Key_Alt
		|| key == Qt::Key_Control
		|| key == Qt::Key_Meta) {
		modifiersChanged();
		return false;
	} else if (_drag.action != Action::None) {
		// Nothing else changes the document under a drag.
		return (key != Qt::Key_Escape);
	} else if (TextInputFocused(_controller)) {
		return false;
	}
	const auto modifiers = e->modifiers()
		& ~(Qt::KeypadModifier | Qt::GroupSwitchModifier);
	if (modifiers & ~Qt::ShiftModifier) {
		return false;
	}
	const auto big = (modifiers & Qt::ShiftModifier) != 0;
	switch (key) {
	case Qt::Key_Left: return nudge(QPointF(-1., 0.), big);
	case Qt::Key_Right: return nudge(QPointF(1., 0.), big);
	case Qt::Key_Up: return nudge(QPointF(0., -1.), big);
	case Qt::Key_Down: return nudge(QPointF(0., 1.), big);
	case Qt::Key_Delete:
	case Qt::Key_Backspace:
		// One press deletes one layer, and only a press made over the
		// canvas: not a held key and not a key left over by a field.
		if (!modifiers
			&& !e->isAutoRepeat()
			&& CanvasHasKeyboard(_controller)
			&& _controller->hasDocument()
			&& _controller->activeLayer()) {
			DeleteLayer(_controller, _controller->activeLayerId());
			return true;
		}
		return false;
	}
	return false;
}

bool TransformTool::keyRelease(not_null<QKeyEvent*> e) {
	const auto key = e->key();
	if (key == Qt::Key_Shift
		|| key == Qt::Key_Alt
		|| key == Qt::Key_Control
		|| key == Qt::Key_Meta) {
		modifiersChanged();
	}
	return false;
}

bool TransformTool::cancel() {
	if (_drag.action == Action::None) {
		return false;
	}
	const auto drag = base::take(_drag);
	if (drag.moved) {
		_controller->changeLayer(drag.layer, [&](Layer &layer) {
			layer.transform = drag.start;
		}, false);
		_controller->commit();
	}
	_controller->updateCanvas();
	return true;
}

QCursor TransformTool::handleCursor(
		const Layer &layer,
		TransformHandle handle) const {
	if (handle == TransformHandle::Move) {
		return QCursor(Qt::SizeAllCursor);
	} else if (handle == TransformHandle::Rotate) {
		return RotateCursor();
	} else if (!IsCornerHandle(handle) && !IsEdgeHandle(handle)) {
		return QCursor(Qt::ArrowCursor);
	} else if (State().perspective.current()) {
		return QCursor(Qt::CrossCursor);
	}
	const auto quad = WidgetQuad(layer, _controller->documentToWidget());
	if (!FiniteQuad(quad)) {
		return QCursor(Qt::ArrowCursor);
	}
	return ResizeCursor(HandlePoint(quad, handle)
		- HandlePoint(quad, TransformHandle::Move));
}

QCursor TransformTool::cursor(const ToolMouseEvent &e) {
	if (!_controller->hasDocument()) {
		return QCursor(Qt::ArrowCursor);
	}
	if (_drag.action != Action::None) {
		if (_drag.action == Action::Corner || _drag.action == Action::Skew) {
			return QCursor(Qt::CrossCursor);
		}
		const auto layer = _controller->document().find(_drag.layer);
		return layer
			? handleCursor(*layer, _drag.handle)
			: QCursor(Qt::ArrowCursor);
	}
	const auto active = _controller->activeLayer();
	if (Usable(active) && !active->locked) {
		const auto handle = hitTest(*active, e);
		if (handle != TransformHandle::None
			&& handle != TransformHandle::Move) {
			return handleCursor(*active, handle);
		}
	}
	return pick(e).move
		? QCursor(Qt::SizeAllCursor)
		: QCursor(Qt::ArrowCursor);
}

void TransformTool::paint(QPainter &p, const ToolPaintContext &context) {
	_viewRect = QRectF(context.widgetRect);
	if (!_controller->hasDocument()) {
		return;
	}
	const auto layer = _controller->activeLayer();
	if (!Usable(layer)) {
		return;
	}
	const auto quad = WidgetQuad(*layer, context.documentToWidget);
	if (!FiniteQuad(quad)) {
		return;
	}
	const auto dragging = (_drag.action != Action::None);
	if (dragging) {
		PaintGuides(
			p,
			context,
			_controller->document(),
			_drag.guideX,
			_drag.guideY);
	}
	if (layer->locked) {
		PaintFrame(p, quad, anim::with_alpha(TextColor(), 0.6));
		const auto unit = Px(24) / 24.;
		const auto center = quad.boundingRect().topLeft()
			+ QPointF(14. * unit, 16. * unit);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, 150));
		p.drawEllipse(center, 11. * unit, 11. * unit);
		PaintLockGlyph(p, center, unit * 0.9, QColor(255, 255, 255), true);
		return;
	}
	PaintFrame(p, quad, AccentColor());

	const auto metrics = this->metrics();
	const auto highlighted = [&](TransformHandle handle) {
		return (dragging ? _drag.handle : _hover) == handle;
	};
	if (metrics.rotate) {
		const auto place = PlaceRotateHandle(quad, metrics);
		p.setPen(QPen(QColor(0, 0, 0, 90), std::max(Px(3) / 2., 1.) + 2.));
		p.drawLine(place.edge, place.handle);
		p.setPen(QPen(AccentColor(), std::max(Px(3) / 2., 1.)));
		p.drawLine(place.edge, place.handle);
		PaintHandle(
			p,
			place.handle,
			2. * Px(kRotateRadius),
			true,
			highlighted(TransformHandle::Rotate));
	}
	if (metrics.edges && EdgeHandlesFit(quad, metrics.radius)) {
		for (const auto handle : {
			TransformHandle::Top,
			TransformHandle::Right,
			TransformHandle::Bottom,
			TransformHandle::Left,
		}) {
			PaintHandle(
				p,
				HandlePoint(quad, handle),
				Px(kEdgeHandleSide),
				false,
				highlighted(handle));
		}
	}
	// A frame that is small on the screen gets smaller corners, or four
	// full ones would cover the whole layer.
	const auto cornerSide = std::min(
		double(Px(kHandleSide) + (metrics.edges ? 0 : Px(2))),
		std::max(ShortestSide(quad), double(Px(kHandleSide))) / 2.);
	for (auto i = 0; i != 4; ++i) {
		PaintHandle(
			p,
			quad[i],
			cornerSide,
			!metrics.edges,
			highlighted(CornerHandle(i)));
	}
	if (dragging) {
		PaintBadge(p, _drag.badge, _last.widget, context.widgetRect);
	}
}

//
// The mask tool.
//

[[nodiscard]] double BrushRadius(not_null<const Controller*> controller) {
	const auto size = controller->document().size;
	const auto longer = std::max({ size.width(), size.height(), 1 });
	return std::max(State().maskSize.current() * longer / 2., 0.5);
}

class MaskTool final : public Tool, public base::has_weak_ptr {
public:
	explicit MaskTool(not_null<Controller*> controller);

	void deactivated() override;
	bool mousePress(const ToolMouseEvent &e) override;
	void mouseMove(const ToolMouseEvent &e) override;
	void mouseRelease(const ToolMouseEvent &e) override;
	void mouseLeave() override;
	bool keyPress(not_null<QKeyEvent*> e) override;
	bool cancel() override;
	void paint(QPainter &p, const ToolPaintContext &context) override;
	QCursor cursor(const ToolMouseEvent &e) override;

private:
	struct Stroke {
		LayerId layer = 0;
		MaskPtr original; // Null: the layer had no mask.
		bool originalEnabled = true;
		QImage base;
		QImage coverage;
		QImage working;
		QPointF last;
		double lastRadius = 0.;
		bool started = false;
		bool changed = false;
		int target = 0;
		double opacity = 1.;
		double hardness = 1.;
	};

	void stamp(const Layer &layer, QPointF local);
	void finishStroke();

	const not_null<Controller*> _controller;
	std::optional<Stroke> _stroke;
	std::optional<QPointF> _hover;
	QImage _overlay;
	uint64 _overlaySerial = 0;
	rpl::lifetime _lifetime;

};

MaskTool::MaskTool(not_null<Controller*> controller)
: _controller(controller) {
	rpl::merge(
		State().maskOverlay.changes() | rpl::to_empty,
		State().maskSize.changes() | rpl::to_empty,
		State().maskHardness.changes() | rpl::to_empty,
		_controller->documentChanges(),
		_controller->activeLayerValue() | rpl::skip(1) | rpl::to_empty
	) | rpl::on_next([=] {
		_controller->updateCanvas();
	}, _lifetime);
}

void MaskTool::deactivated() {
	finishStroke();
	_lifetime.destroy();
	_overlay = QImage();
}

bool MaskTool::mousePress(const ToolMouseEvent &e) {
	if (e.button != Qt::LeftButton
		|| !_controller->hasDocument()
		|| _controller->busy()) {
		return false;
	}
	finishStroke();
	const auto active = _controller->activeLayer();
	if (!HasPixels(active)) {
		return false;
	} else if (active->locked) {
		ToastLocked(_controller);
		return true;
	} else if (!active->visible) {
		_controller->showToast(tr::lng_oblivion_photo_layers_hidden(tr::now));
		return true;
	} else if (!active->mask && State().maskReveal.current()) {
		// Nothing is hidden yet: a mask where everything is shown and an
		// undo step that changes nothing would be all a stroke gives.
		_controller->showToast(
			tr::lng_oblivion_photo_layers_mask_reveal_none(tr::now));
		return true;
	}
	const auto id = active->id;
	_controller->commit();
	const auto layer = _controller->document().find(id);
	if (!layer) {
		return true;
	}
	const auto local = LayerPoint(*layer, e.document);
	auto stroke = Stroke();
	stroke.layer = id;
	stroke.original = layer->mask;
	stroke.originalEnabled = layer->maskEnabled;
	if (layer->mask) {
		stroke.base = layer->mask->image();
	} else if (const auto empty = MakeMask(MaskSizeFor(layer->size()), 255)) {
		stroke.base = empty->image();
	}
	if (stroke.base.isNull()
		|| stroke.base.format() != QImage::Format_Grayscale8) {
		return true;
	}
	stroke.coverage = QImage(stroke.base.size(), QImage::Format_Alpha8);
	stroke.working = stroke.base.copy();
	if (stroke.coverage.isNull() || stroke.working.isNull()) {
		return true;
	}
	stroke.coverage.fill(0);
	stroke.target = State().maskReveal.current() ? 255 : 0;
	stroke.opacity = std::clamp(State().maskStrength.current(), 1, 100) / 100.;
	stroke.hardness = std::clamp(State().maskHardness.current(), 0, 100)
		/ 100.;
	_stroke = std::move(stroke);
	_hover = e.widget;
	if (local) {
		stamp(*layer, *local);
	}
	return true;
}

void MaskTool::stamp(const Layer &layer, QPointF local) {
	if (!_stroke) {
		return;
	}
	auto &stroke = *_stroke;
	const auto content = QSizeF(layer.size());
	const auto size = stroke.working.size();
	const auto point = MaskPoint(content, size, local);
	const auto radius = BrushRadius(_controller)
		/ std::max(LocalScaleAt(layer.transform, local), 1e-6)
		* MaskScale(content, size);
	const auto rect = MaskStampSegment(
		stroke.coverage,
		stroke.started ? stroke.last : point,
		point,
		stroke.started ? stroke.lastRadius : radius,
		radius,
		stroke.hardness);
	stroke.last = point;
	stroke.lastRadius = radius;
	stroke.started = true;
	if (rect.isEmpty()) {
		return;
	}
	MaskComposeStroke(
		stroke.working,
		stroke.base,
		stroke.coverage,
		rect,
		stroke.target,
		stroke.opacity);
	// The picture shares the pixels until the next segment changes them.
	const auto mask = MakeMask(stroke.working);
	if (!mask) {
		return;
	}
	stroke.changed = true;
	if (State().maskOverlay.current()
		&& _overlay.size() == size
		&& _overlaySerial != 0) {
		UpdateMaskOverlay(
			_overlay,
			stroke.working,
			rect,
			QColor(255, 0, 0, kMaskOverlayAlpha));
		_overlaySerial = mask->serial();
	}
	_controller->changeLayer(stroke.layer, [&](Layer &layer) {
		layer.mask = mask;
		layer.maskEnabled = true;
	}, false);
	_controller->updateCanvas();
}

void MaskTool::finishStroke() {
	if (!_stroke) {
		return;
	}
	const auto changed = _stroke->changed;
	_stroke = std::nullopt;
	if (changed) {
		_controller->commit();
	}
}

void MaskTool::mouseMove(const ToolMouseEvent &e) {
	_hover = e.widget;
	if (_stroke && (e.buttons & Qt::LeftButton)) {
		const auto layer = _controller->document().find(_stroke->layer);
		if (!layer || layer->locked) {
			finishStroke();
		} else if (const auto local = LayerPoint(*layer, e.document)) {
			stamp(*layer, *local);
		}
	}
	_controller->updateCanvas();
}

void MaskTool::mouseRelease(const ToolMouseEvent &e) {
	finishStroke();
	_controller->updateCanvas();
}

void MaskTool::mouseLeave() {
	if (_hover && !_stroke) {
		_hover = std::nullopt;
		_controller->updateCanvas();
	}
}

bool MaskTool::keyPress(not_null<QKeyEvent*> e) {
	const auto key = e->key();
	if (key == Qt::Key_Shift
		|| key == Qt::Key_Alt
		|| key == Qt::Key_Control
		|| key == Qt::Key_Meta) {
		return false;
	} else if (_stroke) {
		// Nothing else changes the document under a stroke.
		return (key != Qt::Key_Escape);
	} else if (TextInputFocused(_controller)) {
		return false;
	}
	const auto modifiers = e->modifiers()
		& ~(Qt::KeypadModifier | Qt::GroupSwitchModifier);
	if (modifiers & ~Qt::ShiftModifier) {
		return false;
	}
	auto &state = State();
	if (KeyIs(e, Qt::Key_BracketLeft, 0x0445)) {
		state.maskSize = std::max(
			state.maskSize.current() / kMaskSizeStep,
			kMaskSizeMin);
		return true;
	} else if (KeyIs(e, Qt::Key_BracketRight, 0x044A)) {
		state.maskSize = std::min(
			state.maskSize.current() * kMaskSizeStep,
			kMaskSizeMax);
		return true;
	} else if (KeyIs(e, Qt::Key_X, 0x0447) && !e->isAutoRepeat() && !_stroke) {
		state.maskReveal = !state.maskReveal.current();
		return true;
	}
	return false;
}

bool MaskTool::cancel() {
	if (!_stroke) {
		return false;
	}
	const auto stroke = base::take(_stroke);
	if (stroke->changed) {
		_overlaySerial = 0;
		_controller->changeLayer(stroke->layer, [&](Layer &layer) {
			layer.mask = stroke->original;
			layer.maskEnabled = stroke->originalEnabled;
		}, false);
		_controller->commit();
	}
	_controller->updateCanvas();
	return true;
}

QCursor MaskTool::cursor(const ToolMouseEvent &e) {
	return QCursor(Qt::CrossCursor);
}

void MaskTool::paint(QPainter &p, const ToolPaintContext &context) {
	if (!_controller->hasDocument()) {
		return;
	}
	const auto layer = _controller->activeLayer();
	if (HasPixels(layer) && layer->visible) {
		const auto content = QSizeF(layer->size());
		const auto quad = WidgetQuad(*layer, context.documentToWidget);
		const auto overlay = State().maskOverlay.current()
			&& layer->mask
			&& layer->maskEnabled
			&& !layer->mask->image().isNull();
		if (!overlay) {
			_overlay = QImage();
			_overlaySerial = 0;
		} else if (FiniteQuad(quad)) {
			const auto &mask = layer->mask->image();
			if (_overlaySerial != layer->mask->serial()
				|| _overlay.size() != mask.size()) {
				UpdateMaskOverlay(
					_overlay,
					mask,
					mask.rect(),
					QColor(255, 0, 0, kMaskOverlayAlpha));
				_overlaySerial = layer->mask->serial();
			}
			if (!_overlay.isNull()) {
				// Only over the picture, whatever sticks out of the canvas.
				p.save();
				p.setClipPath(VisiblePath(
					_controller->document(),
					context.documentToWidget));
				p.setRenderHint(QPainter::SmoothPixmapTransform);
				p.setTransform(
					QTransform::fromScale(
						content.width() / _overlay.width(),
						content.height() / _overlay.height())
						* layer->transform
						* context.documentToWidget,
					true);
				p.drawImage(QPointF(), _overlay);
				p.restore();
			}
		}
		if (FiniteQuad(quad)) {
			p.setBrush(Qt::NoBrush);
			p.setPen(QPen(QColor(0, 0, 0, 70), std::max(Px(1), 1) + 2.));
			p.drawPolygon(quad);
			auto pen = QPen(
				layer->locked
					? anim::with_alpha(TextColor(), 0.6)
					: AccentColor(),
				std::max(Px(1), 1));
			pen.setStyle(Qt::DashLine);
			p.setPen(pen);
			p.drawPolygon(quad);
		}
	}
	if (_hover) {
		const auto radius = std::max(
			BrushRadius(_controller) * context.scale,
			1.5);
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(QColor(0, 0, 0, 150), std::max(Px(1), 1) + 2.));
		p.drawEllipse(*_hover, radius, radius);
		p.setPen(QPen(QColor(255, 255, 255), std::max(Px(1), 1)));
		p.drawEllipse(*_hover, radius, radius);
		const auto hardness = std::clamp(
			State().maskHardness.current(),
			0,
			100) / 100.;
		if (hardness < 0.95 && radius * hardness > 3.) {
			auto pen = QPen(QColor(255, 255, 255, 150), std::max(Px(1), 1));
			pen.setStyle(Qt::DotLine);
			p.setPen(pen);
			p.drawEllipse(*_hover, radius * hardness, radius * hardness);
		}
	}
}

//
// The options of the tools (the Tool tab).
//

// The shifts are told as they are seen: in a picture that is turned on
// its side as a whole "horizontal" is along the canvas height.
[[nodiscard]] FxParams NumbersValues(
		const Layer &layer,
		QSize canvas,
		const QTransform &orientation) {
	const auto numbers = NumbersFromTransform(
		layer.transform,
		QSizeF(layer.size()));
	const auto shift = SeenShift(
		orientation,
		QPointF(numbers.x, numbers.y),
		QSizeF(canvas));
	auto result = FxParams();
	result.set("x", FxValue::Number(shift.x()));
	result.set("y", FxValue::Number(shift.y()));
	result.set("w", FxValue::Number(numbers.width));
	result.set("h", FxValue::Number(numbers.height));
	result.set("angle", FxValue::Number(numbers.rotation));
	result.set("skew", FxValue::Number(numbers.skew));
	return result;
}

// canvas: as it is seen (the sides are swapped in a picture turned on its
// side).
[[nodiscard]] std::vector<FxParam> NumbersParams(QSize canvas) {
	const auto w = double(std::max(canvas.width(), 1));
	const auto h = double(std::max(canvas.height(), 1));
	auto angle = FxAngle(
		"angle",
		tr::lng_oblivion_photo_layers_tf_rotation,
		0.);
	angle.decimals = 1;
	angle.step = 0.1;
	auto skew = FxAngle(
		"skew",
		tr::lng_oblivion_photo_layers_tf_skew,
		0.,
		-kSkewLimit,
		kSkewLimit);
	skew.decimals = 1;
	skew.step = 0.1;
	return {
		FxFloat(
			"x",
			tr::lng_oblivion_photo_layers_tf_x,
			-1.5 * w,
			1.5 * w,
			0.),
		FxFloat(
			"y",
			tr::lng_oblivion_photo_layers_tf_y,
			-1.5 * h,
			1.5 * h,
			0.),
		FxFloat(
			"w",
			tr::lng_oblivion_photo_layers_tf_width,
			kScaleMinPercent,
			kScaleMaxPercent,
			100.,
			1,
			u"%"_q),
		FxFloat(
			"h",
			tr::lng_oblivion_photo_layers_tf_height,
			kScaleMinPercent,
			kScaleMaxPercent,
			100.,
			1,
			u"%"_q),
		angle,
		skew,
	};
}

// The last step of the Rotation slider while it is being dragged: what
// it made of which layer.
struct TurnDrag {
	LayerId layer = 0;
	QTransform applied;
	LayerFlips flips;
};

void ApplyNumber(
		not_null<Controller*> controller,
		const QByteArray &id,
		double value,
		bool finished,
		not_null<TurnDrag*> turn) {
	const auto layer = controller->hasDocument()
		? controller->activeLayer()
		: nullptr;
	if (!HasPixels(layer) || controller->busy()) {
		return;
	} else if (layer->locked) {
		if (finished) {
			ToastLocked(controller);
		}
		return;
	}
	const auto layerId = layer->id;
	const auto canvas = QSizeF(controller->document().size);
	const auto content = QSizeF(layer->size());
	auto numbers = NumbersFromTransform(layer->transform, content);
	const auto linked = [&](double &changed, double &other) {
		if (!State().keepAspect.current()) {
			changed = value;
			return;
		}
		const auto sides = LinkPercents(
			changed,
			other,
			value,
			kScaleMinPercent,
			kScaleMaxPercent);
		changed = sides.changed;
		other = sides.other;
	};
	if (id == "x" || id == "y") {
		const auto orientation = SeenOrientation(controller->document());
		auto shift = SeenShift(
			orientation,
			QPointF(numbers.x, numbers.y),
			canvas);
		if (id == "x") {
			shift.setX(value);
		} else {
			shift.setY(value);
		}
		const auto center = PointFromSeenShift(orientation, shift, canvas);
		numbers.x = center.x();
		numbers.y = center.y();
	} else if (id == "w") {
		linked(numbers.width, numbers.height);
	} else if (id == "h") {
		linked(numbers.height, numbers.width);
	} else if (id == "angle") {
		// The next step of the same drag goes on with the flips of the
		// previous one, see WithRotation().
		const auto dragged = (turn->layer == layerId)
			&& (turn->applied == layer->transform);
		numbers = WithRotation(
			numbers,
			value,
			dragged ? std::make_optional(turn->flips) : std::nullopt);
	} else if (id == "skew") {
		numbers.skew = value;
	} else {
		return;
	}
	const auto transform = TransformFromNumbers(numbers, content);
	if (!ValidTransform(transform, content)) {
		return;
	}
	*turn = (id == "angle" && !finished)
		? TurnDrag{
			.layer = layerId,
			.applied = transform,
			.flips = { .x = numbers.flippedX, .y = numbers.flippedY },
		}
		: TurnDrag();
	controller->changeLayer(layerId, [=](Layer &layer) {
		layer.transform = transform;
	}, finished);
}

void AddSwitch(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		not_null<rpl::variable<bool>*> value) {
	const auto result = container->add(
		object_ptr<SwitchHeader>(
			container.get(),
			text,
			value->current(),
			false),
		RowMargins());
	result->toggles() | rpl::on_next([=](bool checked) {
		*value = checked;
	}, result->lifetime());
	value->changes() | rpl::on_next([=](bool checked) {
		result->setChecked(checked, anim::type::normal);
	}, result->lifetime());
}

[[nodiscard]] object_ptr<Ui::RpWidget> CreateTransformOptions(
		not_null<QWidget*> parent,
		not_null<Controller*> controller) {
	auto result = object_ptr<Ui::VerticalLayout>(parent);
	const auto raw = result.data();
	auto &state = State();

	const auto modes = raw->add(object_ptr<ChipsFlow>(raw), RowMargins());
	modes->addChip(tr::lng_oblivion_photo_layers_tf_mode_free(), [] {
		State().perspective = false;
	});
	modes->addChip(tr::lng_oblivion_photo_layers_tf_mode_perspective(), [] {
		State().perspective = true;
	});
	state.perspective.value() | rpl::on_next([=](bool perspective) {
		modes->setSelected(perspective ? 1 : 0);
	}, modes->lifetime());

	raw->add(
		object_ptr<Ui::FlatLabel>(
			raw,
			state.perspective.value() | rpl::map([](bool perspective) {
				return perspective
					? tr::lng_oblivion_photo_layers_tf_hint_perspective(
						tr::now)
					: tr::lng_oblivion_photo_layers_tf_hint(
						tr::now,
						lt_shortcut,
						CommandKeyName());
			}),
			HintLabelStyle()),
		RowMargins(Px(kSkip)));

	const auto title = raw->add(
		object_ptr<SectionTitle>(
			raw,
			tr::lng_oblivion_photo_layers_tf_section()),
		RowMargins());
	title->setAction(tr::lng_oblivion_photo_layers_tf_reset(), [=] {
		ChangeTransform(controller, [](
				const QTransform &transform,
				QSizeF content,
				QSizeF canvas) {
			return DefaultTransform(content.toSize(), canvas.toSize());
		});
	});

	const auto numbersWrap = raw->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			raw,
			object_ptr<Ui::VerticalLayout>(raw)));
	const auto note = raw->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			raw,
			object_ptr<Ui::FlatLabel>(
				raw,
				tr::lng_oblivion_photo_layers_tf_perspective_note(),
				HintLabelStyle()),
			RowMargins()));
	const auto none = raw->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			raw,
			object_ptr<Ui::FlatLabel>(
				raw,
				tr::lng_oblivion_photo_layers_tf_no_layer(),
				HintLabelStyle()),
			RowMargins()));

	struct Numbers {
		QSize canvas;
		QSize content;
		LayerId layer = 0;
		QTransform seen; // QuarterOrientation: which side is horizontal.
		bool built = false;
		TurnDrag turn;
		rpl::event_stream<FxParams> updates;
		Fn<void()> refresh;
	};
	const auto numbers = raw->lifetime().make_state<Numbers>();
	const auto current = [=](const QByteArray &id) -> std::optional<double> {
		const auto layer = controller->hasDocument()
			? controller->activeLayer()
			: nullptr;
		if (!HasPixels(layer) || IsPerspective(layer->transform)) {
			return std::nullopt;
		}
		const auto &document = controller->document();
		return NumbersValues(
			*layer,
			document.size,
			SeenOrientation(document)).value(id).number();
	};
	const auto refresh = [=] {
		const auto layer = controller->hasDocument()
			? controller->activeLayer()
			: nullptr;
		const auto shown = HasPixels(layer);
		const auto perspective = shown && IsPerspective(layer->transform);
		none->toggle(!shown, anim::type::instant);
		note->toggle(perspective, anim::type::instant);
		numbersWrap->toggle(shown && !perspective, anim::type::instant);
		title->setActionVisible(shown);
		if (!shown || perspective) {
			return;
		}
		const auto &document = controller->document();
		const auto canvas = document.size;
		const auto orientation = SeenOrientation(document);
		const auto seen = QuarterOrientation(orientation);
		auto values = NumbersValues(*layer, canvas, orientation);
		if (numbers->built
			&& numbers->canvas == canvas
			&& numbers->content == layer->size()
			&& numbers->layer == layer->id
			&& numbers->seen == seen) {
			numbers->updates.fire(std::move(values));
			return;
		}
		numbers->built = true;
		numbers->canvas = canvas;
		numbers->content = layer->size();
		numbers->layer = layer->id;
		numbers->seen = seen;
		const auto sideways = std::abs(seen.m12()) > 0.5;
		const auto inner = numbersWrap->entity();
		inner->clear();
		inner->add(
			CreateParamsPanel(inner, ParamsPanelArgs{
				.params = NumbersParams(sideways
					? canvas.transposed()
					: canvas),
				.values = std::move(values),
				.updates = numbers->updates.events(),
				.changed = [=](
						const QByteArray &id,
						FxValue value,
						bool finished) {
					const auto wanted = value.number();
					ApplyNumber(
						controller,
						id,
						wanted,
						finished,
						&numbers->turn);
					if (!finished) {
						return;
					}
					// The slider stays where it was left (it takes no
					// values while it is held) even if that was refused
					// or limited: a locked layer, linked sides at their
					// limit, a turn that is told differently. The rows
					// are made again with what really is there, later:
					// this is called from the row itself.
					const auto now = current(id);
					if (now && std::abs(*now - wanted) > 1e-3) {
						crl::on_main(raw, [=] {
							numbers->built = false;
							if (const auto onstack = numbers->refresh) {
								onstack();
							}
						});
					}
				},
				.controller = controller.get(),
				// 1% .. 1000%: on an even scale the usual 100% stood at
				// the start of the track with no room to make it smaller.
				.logarithmic = { QByteArray("w"), QByteArray("h") },
			}),
			RowMargins());
		inner->resizeToWidth(raw->width());
	};
	numbers->refresh = refresh;
	rpl::merge(
		controller->documentChanges(),
		controller->activeLayerValue() | rpl::to_empty
	) | rpl::on_next(refresh, raw->lifetime());
	refresh();

	AddSwitch(
		raw,
		tr::lng_oblivion_photo_layers_tf_keep_aspect(tr::now),
		&state.keepAspect);
	AddSwitch(
		raw,
		tr::lng_oblivion_photo_layers_tf_snap(tr::now),
		&state.snapping);

	raw->add(
		object_ptr<SectionTitle>(
			raw,
			tr::lng_oblivion_photo_layers_tf_actions()),
		RowMargins());
	const auto actions = raw->add(object_ptr<ChipsFlow>(raw), RowMargins());
	const auto action = [&](
			rpl::producer<QString> text,
			Fn<QTransform(
				const QTransform &transform,
				QSizeF content,
				QSizeF canvas)> make) {
		actions->addChip(std::move(text), [=] {
			ChangeTransform(controller, make);
		});
	};
	// "Horizontally", "left" and "right" are what they are on the screen,
	// whatever the turn and the mirror of the whole picture are.
	const auto turn = [=](double degrees) {
		return [=](const QTransform &transform, QSizeF content, QSizeF) {
			return RotatedTransform(
				transform,
				transform.map(
					QPointF(content.width() / 2., content.height() / 2.)),
				SeenTurn(SeenOrientation(controller->document()), degrees));
		};
	};
	const auto flip = [=](bool horizontal) {
		return [=](const QTransform &transform, QSizeF content, QSizeF) {
			return FlippedTransform(
				transform,
				content,
				horizontal,
				SeenOrientation(controller->document()));
		};
	};
	action(tr::lng_oblivion_photo_layers_tf_flip_h(), flip(true));
	action(tr::lng_oblivion_photo_layers_tf_flip_v(), flip(false));
	action(tr::lng_oblivion_photo_layers_tf_rotate_left(), turn(-90.));
	action(tr::lng_oblivion_photo_layers_tf_rotate_right(), turn(90.));
	action(tr::lng_oblivion_photo_layers_tf_fit(), [](
			const QTransform &transform,
			QSizeF content,
			QSizeF canvas) {
		return FittedTransform(transform, content, canvas, false);
	});
	action(tr::lng_oblivion_photo_layers_tf_fill(), [](
			const QTransform &transform,
			QSizeF content,
			QSizeF canvas) {
		return FittedTransform(transform, content, canvas, true);
	});
	action(tr::lng_oblivion_photo_layers_tf_center(), [](
			const QTransform &transform,
			QSizeF content,
			QSizeF canvas) {
		return CenteredTransform(transform, content, canvas);
	});
	return object_ptr<Ui::RpWidget>(std::move(result));
}

[[nodiscard]] object_ptr<Ui::RpWidget> CreateMaskOptions(
		not_null<QWidget*> parent,
		not_null<Controller*> controller) {
	auto result = object_ptr<Ui::VerticalLayout>(parent);
	const auto raw = result.data();
	auto &state = State();

	const auto modes = raw->add(object_ptr<ChipsFlow>(raw), RowMargins());
	modes->addChip(tr::lng_oblivion_photo_layers_mask_mode_hide(), [] {
		State().maskReveal = false;
	});
	modes->addChip(tr::lng_oblivion_photo_layers_mask_mode_show(), [] {
		State().maskReveal = true;
	});
	state.maskReveal.value() | rpl::on_next([=](bool reveal) {
		modes->setSelected(reveal ? 1 : 0);
	}, modes->lifetime());

	raw->add(
		object_ptr<Ui::FlatLabel>(
			raw,
			tr::lng_oblivion_photo_layers_mask_hint(),
			HintLabelStyle()),
		RowMargins(Px(kSkip)));

	// The size is kept as a part of the canvas and shown in pixels, so
	// the slider is made again when the canvas changes. A gap above it:
	// the label of the first slider stood right under the hint.
	const auto brush = raw->add(
		object_ptr<Ui::VerticalLayout>(raw),
		style::margins(0, Px(6), 0, 0));
	struct Brush {
		int longer = 0;
		rpl::event_stream<FxParams> updates;
	};
	const auto data = raw->lifetime().make_state<Brush>();
	const auto values = [=] {
		auto &state = State();
		auto result = FxParams();
		result.set(
			"size",
			FxValue::Number(state.maskSize.current() * data->longer));
		result.set("hardness", FxValue::Integer(state.maskHardness.current()));
		result.set("strength", FxValue::Integer(state.maskStrength.current()));
		return result;
	};
	const auto rebuild = [=] {
		const auto size = controller->document().size;
		const auto longer = std::max({ size.width(), size.height(), 1 });
		if (data->longer == longer) {
			return;
		}
		data->longer = longer;
		brush->clear();
		brush->add(
			CreateParamsPanel(brush, ParamsPanelArgs{
				.params = {
					FxFloat(
						"size",
						tr::lng_oblivion_photo_layers_mask_size,
						std::max(std::floor(longer * kMaskSizeMin), 1.),
						std::max(std::ceil(longer * kMaskSizeMax), 2.),
						longer * kMaskSizeDefault),
					FxInt(
						"hardness",
						tr::lng_oblivion_photo_layers_mask_hardness,
						0,
						100,
						kMaskHardnessDefault,
						u"%"_q),
					FxInt(
						"strength",
						tr::lng_oblivion_photo_layers_mask_strength,
						1,
						100,
						100,
						u"%"_q),
				},
				.values = values(),
				.updates = data->updates.events(),
				.changed = [=](
						const QByteArray &id,
						FxValue value,
						bool finished) {
					auto &state = State();
					if (id == "size") {
						state.maskSize = std::clamp(
							value.number() / std::max(data->longer, 1),
							kMaskSizeMin,
							kMaskSizeMax);
					} else if (id == "hardness") {
						state.maskHardness = value.integer();
					} else if (id == "strength") {
						state.maskStrength = value.integer();
					}
				},
				.controller = controller.get(),
			}),
			RowMargins());
		brush->resizeToWidth(raw->width());
	};
	rebuild();
	// The keys of the tool change the size too.
	state.maskSize.changes() | rpl::on_next([=] {
		data->updates.fire(values());
	}, raw->lifetime());

	AddSwitch(
		raw,
		tr::lng_oblivion_photo_layers_mask_overlay(tr::now),
		&state.maskOverlay);

	const auto maskWrap = raw->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			raw,
			object_ptr<Ui::VerticalLayout>(raw)));
	const auto maskPart = maskWrap->entity();
	const auto enabled = maskPart->add(
		object_ptr<SwitchHeader>(
			maskPart,
			tr::lng_oblivion_photo_layers_mask_enabled(tr::now),
			true,
			false),
		RowMargins());
	enabled->toggles() | rpl::on_next([=](bool checked) {
		const auto layer = controller->activeLayer();
		if (layer && layer->mask && (layer->maskEnabled != checked)) {
			if (layer->locked) {
				ToastLocked(controller);
				enabled->setChecked(layer->maskEnabled, anim::type::normal);
			} else {
				ToggleLayerMask(controller, layer->id);
			}
		}
	}, enabled->lifetime());
	maskPart->add(
		object_ptr<SectionTitle>(
			maskPart,
			tr::lng_oblivion_photo_layers_mask_actions()),
		RowMargins());
	const auto actions = maskPart->add(
		object_ptr<ChipsFlow>(maskPart),
		RowMargins());
	actions->addChip(tr::lng_oblivion_photo_layers_mask_invert(), [=] {
		InvertLayerMask(controller, controller->activeLayerId());
	});
	actions->addChip(tr::lng_oblivion_photo_layers_mask_show_all(), [=] {
		FillLayerMask(controller, controller->activeLayerId(), 255);
	});
	actions->addChip(tr::lng_oblivion_photo_layers_mask_hide_all(), [=] {
		FillLayerMask(controller, controller->activeLayerId(), 0);
	});
	actions->addChip(tr::lng_oblivion_photo_layers_mask_remove(), [=] {
		RemoveLayerMask(controller, controller->activeLayerId());
	});

	const auto note = raw->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			raw,
			object_ptr<Ui::FlatLabel>(
				raw,
				tr::lng_oblivion_photo_layers_mask_none(),
				HintLabelStyle()),
			RowMargins(Px(kSkip))));
	const auto none = raw->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			raw,
			object_ptr<Ui::FlatLabel>(
				raw,
				tr::lng_oblivion_photo_layers_mask_no_layer(),
				HintLabelStyle()),
			RowMargins(Px(kSkip))));

	const auto refresh = [=] {
		rebuild();
		const auto layer = controller->hasDocument()
			? controller->activeLayer()
			: nullptr;
		const auto shown = HasPixels(layer);
		const auto masked = shown && (layer->mask != nullptr);
		none->toggle(!shown, anim::type::instant);
		note->toggle(shown && !masked, anim::type::instant);
		maskWrap->toggle(masked, anim::type::instant);
		if (masked) {
			enabled->setChecked(layer->maskEnabled, anim::type::instant);
		}
	};
	rpl::merge(
		controller->documentChanges(),
		controller->activeLayerValue() | rpl::to_empty
	) | rpl::on_next(refresh, raw->lifetime());
	refresh();
	return object_ptr<Ui::RpWidget>(std::move(result));
}

//
// Registration.
//

const auto Registered = EditorRegistrar([] {
	RegisterPanel({
		.id = "layers.list",
		.slot = PanelSlot::Layers,
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller
		) -> object_ptr<Ui::RpWidget> {
			return object_ptr<LayersPanel>(parent, controller);
		},
		// The header and every row (one row for the "no layers" line).
		.height = [](not_null<const Controller*> controller) {
			const auto count = controller->hasDocument()
				? int(controller->document().layers.size())
				: 0;
			return Px(kHeaderHeight)
				+ std::max(count, 1) * Px(kRowHeight)
				+ Px(kListBottom);
		},
	});
	RegisterTool({
		.id = kTransformTool,
		.name = tr::lng_oblivion_photo_layers_tool_transform,
		.key = Qt::Key_V,
		.order = 10,
		.paintIcon = PaintTransformIcon,
		.create = [](
				not_null<Controller*> controller
		) -> std::unique_ptr<Tool> {
			return std::make_unique<TransformTool>(controller);
		},
		.available = [](not_null<const Controller*> controller) {
			return controller->hasDocument()
				&& (controller->activeLayer() != nullptr);
		},
		.options = CreateTransformOptions,
	});
	RegisterTool({
		.id = kMaskTool,
		.name = tr::lng_oblivion_photo_layers_tool_mask,
		.key = Qt::Key_M,
		.order = 12,
		.paintIcon = PaintMaskIcon,
		.create = [](
				not_null<Controller*> controller
		) -> std::unique_ptr<Tool> {
			return std::make_unique<MaskTool>(controller);
		},
		.available = [](not_null<const Controller*> controller) {
			return controller->hasDocument()
				&& (controller->activeLayer() != nullptr);
		},
		.options = CreateMaskOptions,
	});
});

//
// UI snapshot scenes.
//

// The sample layers with everything the list shows: a locked photo,
// a blended layer, a masked one and a hidden one on top.
[[nodiscard]] Document SceneDocument() {
	auto document = SampleSceneDocument();
	if (document.layers.size() < 3) {
		return document;
	}
	document.layers[0].locked = true;
	const auto canvas = document.size;
	auto extra = MakeImageLayer(
		FxTestImage(canvas.width() / 4, canvas.height() / 4),
		NewLayerName(document));
	extra.transform = QTransform::fromTranslate(
		canvas.width() * 0.7,
		canvas.height() * 0.68);
	extra.visible = false;
	extra.opacity = 0.5;
	AddLayer(document, std::move(extra));
	return document;
}

[[nodiscard]] Document PerspectiveSceneDocument() {
	auto document = SampleSceneDocument();
	if (document.layers.size() < 2) {
		return document;
	}
	auto &layer = document.layers[1];
	const auto content = QSizeF(layer.size());
	const auto canvas = QSizeF(document.size);
	auto transform = QTransform();
	const auto quad = QPolygonF({
		QPointF(canvas.width() * 0.42, canvas.height() * 0.16),
		QPointF(canvas.width() * 0.90, canvas.height() * 0.24),
		QPointF(canvas.width() * 0.86, canvas.height() * 0.66),
		QPointF(canvas.width() * 0.46, canvas.height() * 0.52),
	});
	if (QuadTransform(content, quad, transform)
		&& ValidTransform(transform, content)) {
		layer.transform = transform;
	}
	return document;
}

void ResetSceneState() {
	auto &state = State();
	state.perspective = false;
	state.keepAspect = true;
	state.snapping = true;
	state.maskReveal = false;
	state.maskSize = kMaskSizeDefault;
	state.maskHardness = kMaskHardnessDefault;
	state.maskStrength = 100;
	state.maskOverlay = false;
}

void ActivateSceneLayer(not_null<Controller*> controller, int index) {
	const auto &layers = controller->document().layers;
	if (index >= 0 && index < int(layers.size())) {
		controller->setActiveLayer(layers[index].id);
	}
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	const auto width = Px(340);
	const auto options = [](
			Fn<object_ptr<Ui::RpWidget>(
				not_null<QWidget*> parent,
				not_null<Controller*> controller)> create) {
		return [=](
				not_null<QWidget*> parent,
				not_null<Controller*> controller
		) -> object_ptr<Ui::RpWidget> {
			auto result = object_ptr<Ui::VerticalLayout>(parent);
			result->add(object_ptr<Ui::FixedHeightWidget>(
				result.data(),
				Px(kSkip)));
			result->add(create(result.data(), controller));
			result->add(object_ptr<Ui::FixedHeightWidget>(
				result.data(),
				Px(kPadding)));
			return object_ptr<Ui::RpWidget>(std::move(result));
		};
	};

	// The list alone: thumbnails, a mask, the blending line, a locked and
	// a hidden layer.
	RegisterPanelScene({
		.name = u"photo_layers_panel"_q,
		.size = QSize(width, Px(248)),
		.document = SceneDocument,
		.prepare = [](not_null<Controller*> controller) {
			ResetSceneState();
			ActivateSceneLayer(controller, 2);
		},
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller
		) -> object_ptr<Ui::RpWidget> {
			return object_ptr<LayersPanel>(parent, controller);
		},
	});
	// The same while a layer is being renamed and the mask is painted.
	RegisterPanelScene({
		.name = u"photo_layers_panel_rename"_q,
		.size = QSize(width, Px(248)),
		.document = SceneDocument,
		.prepare = [](not_null<Controller*> controller) {
			ResetSceneState();
			ActivateSceneLayer(controller, 2);
			controller->setTool(kMaskTool);
		},
		.create = [](
				not_null<QWidget*> parent,
				not_null<Controller*> controller
		) -> object_ptr<Ui::RpWidget> {
			auto result = object_ptr<LayersPanel>(parent, controller);
			const auto &layers = controller->document().layers;
			if (layers.size() > 1) {
				const auto raw = result.data();
				const auto id = layers[1].id;
				crl::on_main(raw, [=] {
					raw->startRename(id);
				});
			}
			return object_ptr<Ui::RpWidget>(std::move(result));
		},
	});
	RegisterPanelScene({
		.name = u"photo_layers_transform_options"_q,
		.size = QSize(width, 0),
		.document = SceneDocument,
		.prepare = [](not_null<Controller*> controller) {
			ResetSceneState();
			ActivateSceneLayer(controller, 1);
		},
		.create = options(CreateTransformOptions),
	});
	RegisterPanelScene({
		.name = u"photo_layers_mask_options"_q,
		.size = QSize(width, 0),
		.document = SceneDocument,
		.prepare = [](not_null<Controller*> controller) {
			ResetSceneState();
			State().maskOverlay = true;
			ActivateSceneLayer(controller, 2);
		},
		.create = options(CreateMaskOptions),
	});

	// The whole editor: the frame with handles around a turned layer.
	RegisterEditorScene({
		.name = u"photo_layers_transform"_q,
		.document = SceneDocument,
		.tab = PhotoEditorTab::Tool,
		.tool = kTransformTool,
		.prepare = [](not_null<Controller*> controller) {
			ResetSceneState();
			ActivateSceneLayer(controller, 1);
		},
	});
	// A wide and low window, the photo fits it by its height and its
	// layer has no room above the frame: the round handle is inside.
	RegisterEditorScene({
		.name = u"photo_layers_transform_full"_q,
		.size = QSize(1400, 520),
		.document = SampleSceneDocument,
		.tab = PhotoEditorTab::Tool,
		.tool = kTransformTool,
		.prepare = [](not_null<Controller*> controller) {
			ResetSceneState();
			ActivateSceneLayer(controller, 0);
		},
	});
	// Four free corners.
	RegisterEditorScene({
		.name = u"photo_layers_perspective"_q,
		.document = PerspectiveSceneDocument,
		.tab = PhotoEditorTab::Tool,
		.tool = kTransformTool,
		.prepare = [](not_null<Controller*> controller) {
			ResetSceneState();
			State().perspective = true;
			ActivateSceneLayer(controller, 1);
		},
	});
	// The mask tool with the hidden parts highlighted.
	RegisterEditorScene({
		.name = u"photo_layers_mask"_q,
		.document = SceneDocument,
		.tab = PhotoEditorTab::Tool,
		.tool = kMaskTool,
		.prepare = [](not_null<Controller*> controller) {
			ResetSceneState();
			State().maskOverlay = true;
			ActivateSceneLayer(controller, 2);
		},
	});
	// A narrow window: the list is a tab of its own.
	RegisterEditorScene({
		.name = u"photo_layers_narrow"_q,
		.size = QSize(520, 820),
		.document = SceneDocument,
		.tab = PhotoEditorTab::Layers,
		.prepare = [](not_null<Controller*> controller) {
			ResetSceneState();
			ActivateSceneLayer(controller, 1);
		},
	});
	// A narrow window with the options of the transform tool under the
	// photo: with the Tool tab the labels of all tabs don't fit there.
	RegisterEditorScene({
		.name = u"photo_layers_narrow_tool"_q,
		.size = QSize(520, 820),
		.document = SceneDocument,
		.tab = PhotoEditorTab::Tool,
		.tool = kTransformTool,
		.prepare = [](not_null<Controller*> controller) {
			ResetSceneState();
			ActivateSceneLayer(controller, 1);
		},
	});
});

} // namespace

void AddLayerMask(not_null<Controller*> controller, LayerId id) {
	const auto layer = controller->document().find(id);
	if (!HasPixels(layer) || !Editable(controller, id)) {
		return;
	}
	if (!layer->mask) {
		auto mask = MakeMask(MaskSizeFor(layer->size()), 255);
		if (!mask) {
			return;
		}
		controller->changeLayer(id, [&](Layer &layer) {
			layer.mask = mask;
			layer.maskEnabled = true;
		});
	}
	controller->setActiveLayer(id);
	controller->setTool(kMaskTool);
}

void RemoveLayerMask(not_null<Controller*> controller, LayerId id) {
	const auto layer = controller->document().find(id);
	if (!layer || !layer->mask || !Editable(controller, id)) {
		return;
	}
	controller->changeLayer(id, [](Layer &layer) {
		layer.mask = nullptr;
		layer.maskEnabled = true;
	});
}

void InvertLayerMask(not_null<Controller*> controller, LayerId id) {
	const auto layer = controller->document().find(id);
	if (!layer || !layer->mask || !Editable(controller, id)) {
		return;
	}
	auto mask = MakeMask(InvertedMask(layer->mask->image()));
	if (!mask) {
		return;
	}
	controller->changeLayer(id, [&](Layer &layer) {
		layer.mask = mask;
	});
}

void MergeLayerDown(not_null<Controller*> controller, LayerId id) {
	if (!controller->hasDocument() || controller->busy()) {
		return;
	}
	const auto &document = controller->document();
	const auto index = document.indexOf(id);
	if (index <= 0) {
		return;
	} else if (!CanMergeDown(document, id)) {
		ToastLocked(controller);
		return;
	} else if (MergeDownUnderBlendMode(document, id)) {
		controller->showToast(
			tr::lng_oblivion_photo_panel_merge_blend(tr::now));
		return;
	}
	const auto lower = document.layers[index - 1].id;
	controller->runBusy(
		tr::lng_oblivion_photo_layers_merging(tr::now),
		[=](const Document &document) {
			return MergedDown(document, id);
		},
		crl::guard(controller.get(), [=](bool applied) {
			if (applied) {
				controller->setActiveLayer(lower);
			}
		}));
}

void FlattenLayers(not_null<Controller*> controller) {
	if (!controller->hasDocument()
		|| controller->busy()
		|| IsPlainImage(controller->document())) {
		return;
	}
	controller->runBusy(
		tr::lng_oblivion_photo_layers_flattening(tr::now),
		[](const Document &document) {
			return Flattened(document);
		});
}

void DeleteLayer(not_null<Controller*> controller, LayerId id) {
	if (!controller->hasDocument() || controller->busy()) {
		return;
	}
	const auto &document = controller->document();
	const auto index = document.indexOf(id);
	if (index < 0) {
		return;
	} else if (document.layers.size() <= 1) {
		controller->showToast(
			tr::lng_oblivion_photo_layers_delete_last(tr::now));
		return;
	} else if (!Editable(controller, id)) {
		return;
	}
	if (controller->activeLayerId() == id) {
		// The layer under the deleted one is the next to work with.
		controller->setActiveLayer(
			document.layers[(index > 0) ? (index - 1) : 1].id);
	}
	controller->removeLayer(id);
}

bool PasteLayerFromClipboard(not_null<Controller*> controller) {
	if (!controller->hasDocument() || controller->busy()) {
		return false;
	}
	const auto data = QGuiApplication::clipboard()->mimeData();
	// Files first, as Cmd / Ctrl + V in the editor does: a file copied in
	// the file manager comes with a picture too, but that one is its icon.
	if (data && data->hasUrls()) {
		auto paths = QStringList();
		for (const auto &url : data->urls()) {
			if (url.isLocalFile()) {
				paths.push_back(url.toLocalFile());
			}
		}
		if (!paths.isEmpty()) {
			controller->importFiles(paths);
			return true;
		}
	}
	if (data && data->hasImage()) {
		auto image = qvariant_cast<QImage>(data->imageData());
		if (!image.isNull()) {
			controller->addImageLayer(std::move(image), QString());
			return true;
		}
	}
	controller->showToast(tr::lng_oblivion_photo_layers_paste_empty(tr::now));
	return false;
}

} // namespace Oblivion::Photo
