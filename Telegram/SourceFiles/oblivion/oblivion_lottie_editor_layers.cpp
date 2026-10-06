/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_lottie_editor_layers.h"

#include "base/flat_set.h"
#include "base/timer.h"
#include "lang/lang_keys.h"
#include "oblivion/oblivion_lottie_doc.h"
#include "oblivion/oblivion_lottie_editor.h"
#include "oblivion/oblivion_lottie_editor_masks.h"
#include "oblivion/oblivion_lottie_editor_palette.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/effects/animation_value.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/tooltip.h"
#include "styles/style_dialogs.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>
#include <QtGui/QStyleHints>

#include <cmath>
#include <unordered_map>

namespace Oblivion::LottieEdit {
namespace {

constexpr auto kPadding = 10;
constexpr auto kHeaderHeight = 44;
constexpr auto kSearchSkip = 6;
constexpr auto kRowHeight = 28;
constexpr auto kIndent = 14;
constexpr auto kArrowWidth = 16;
constexpr auto kIconSize = 16;
constexpr auto kIconSkip = 7;
constexpr auto kEyeWidth = 30;
constexpr auto kRenameHeight = 24;
constexpr auto kMaxDepth = 32;
constexpr auto kAutoScrollZone = 28;
constexpr auto kAutoScrollStep = 8;
constexpr auto kAutoScrollDelay = crl::time(16);
constexpr auto kTooltipDelay = 800;
constexpr auto kBottomSkip = 8;

[[nodiscard]] int Scaled(int value) {
	return style::ConvertScale(value);
}

// Identity of a row in the tree: the chain of node ids from the root
// (a precomposition asset may be shown under several precomp layers).
[[nodiscard]] uint64 RowKey(uint64 parent, NodeId id) {
	auto result = parent * 0x100000001B3ULL;
	result ^= id + 0x9E3779B97F4A7C15ULL + (parent << 6) + (parent >> 2);
	return result ? result : 1;
}

[[nodiscard]] bool IsTransformItem(const Document &document, NodeId id) {
	const auto node = document.node(id);
	return node
		&& (node->kind == NodeKind::Shape)
		&& (node->shapeType == ShapeType::Transform);
}

[[nodiscard]] bool CanHide(const NodeInfo &node) {
	return (node.kind == NodeKind::Layer)
		|| ((node.kind == NodeKind::Shape)
			&& (node.shapeType != ShapeType::Transform));
}

[[nodiscard]] bool CanMove(const NodeInfo &node) {
	return CanHide(node);
}

[[nodiscard]] bool IsShapeContainer(const NodeInfo &node) {
	return ((node.kind == NodeKind::Layer)
			&& (node.layerType == LayerType::Shape))
		|| ((node.kind == NodeKind::Shape)
			&& (node.shapeType == ShapeType::Group));
}

// Children rows of a node: masks, effects and shape items of a layer
// (without the group transforms), layers of a precomposition asset.
[[nodiscard]] std::vector<NodeId> TreeChildren(
		const Document &document,
		const NodeInfo &node) {
	auto result = std::vector<NodeId>();
	const auto addShapes = [&] {
		for (const auto id : node.children) {
			if (!IsTransformItem(document, id)) {
				result.push_back(id);
			}
		}
	};
	if (node.kind == NodeKind::Layer) {
		result.insert(end(result), begin(node.masks), end(node.masks));
		result.insert(end(result), begin(node.effects), end(node.effects));
		addShapes();
		if (node.layerType == LayerType::Precomp && node.precomp) {
			const auto layers = document.layers(node.precomp);
			result.insert(end(result), begin(layers), end(layers));
		}
	} else if (node.kind == NodeKind::Shape
		&& node.shapeType == ShapeType::Group) {
		addShapes();
	}
	return result;
}

// Node ids of the rows from a root layer down to the row that holds the
// node (not including the node), empty for root layers.
[[nodiscard]] std::vector<NodeId> TreeAncestors(
		const Document &document,
		NodeId id) {
	auto result = std::vector<NodeId>();
	auto current = document.node(id);
	for (auto guard = 0; current && guard != kMaxDepth; ++guard) {
		auto parent = NodeId();
		switch (current->kind) {
		case NodeKind::Layer: {
			const auto composition = document.node(current->composition);
			if (composition && composition->kind == NodeKind::Asset) {
				const auto users = document.assetUsers(composition->id);
				if (!users.empty()) {
					parent = users.front();
				}
			}
		} break;
		case NodeKind::Shape:
		case NodeKind::Mask:
		case NodeKind::Effect:
			parent = current->parent;
			break;
		default:
			break;
		}
		if (!parent || ranges::contains(result, parent)) {
			break;
		}
		result.push_back(parent);
		current = document.node(parent);
	}
	ranges::reverse(result);
	return result;
}

// Shape layer / group to add new shapes into for the selected node.
[[nodiscard]] NodeId ShapeContainerFor(const Document &document, NodeId id) {
	const auto node = document.node(id);
	if (!node) {
		return 0;
	} else if (IsShapeContainer(*node)) {
		return node->id;
	} else if (node->kind == NodeKind::Shape) {
		const auto parent = document.node(node->parent);
		return (parent && IsShapeContainer(*parent)) ? parent->id : 0;
	}
	return 0;
}

struct TreeRow {
	NodeId id = 0;
	uint64 key = 0;
	uint64 parentKey = 0;
	int parentRow = -1;
	int depth = 0;
	int blockEnd = 0; // After the last shown descendant.
	NodeKind kind = NodeKind::Layer;
	LayerType layerType = LayerType::Unknown;
	ShapeType shapeType = ShapeType::Unknown;
	QString name;
	QColor color; // Fill / stroke / solid / gradient start.
	QColor color2; // Gradient end.
	bool expandable = false;
	bool expanded = false;
	bool hidden = false;
	bool dimmed = false; // Hidden itself or inside a hidden node.
	bool context = false; // Shown only as a parent of search results.
	bool canHide = false;
	bool canMove = false;
};

void PaintArrow(QPainter &p, QRectF rect, bool expanded, const QColor &fg) {
	auto hq = PainterHighQualityEnabler(p);
	auto pen = QPen(fg);
	pen.setWidthF(Scaled(15) / 10.);
	pen.setCapStyle(Qt::RoundCap);
	pen.setJoinStyle(Qt::RoundJoin);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	const auto c = rect.center();
	const auto s = Scaled(3) + 0.5;
	auto path = QPainterPath();
	if (expanded) {
		path.moveTo(c.x() - s, c.y() - s / 2.);
		path.lineTo(c.x(), c.y() + s / 2.);
		path.lineTo(c.x() + s, c.y() - s / 2.);
	} else {
		path.moveTo(c.x() - s / 2., c.y() - s);
		path.lineTo(c.x() + s / 2., c.y());
		path.lineTo(c.x() - s / 2., c.y() + s);
	}
	p.drawPath(path);
}

void PaintEye(QPainter &p, QRectF rect, const QColor &fg, bool crossed) {
	auto hq = PainterHighQualityEnabler(p);
	auto pen = QPen(fg);
	pen.setWidthF(Scaled(13) / 10.);
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	const auto c = rect.center();
	const auto w = rect.width() * 0.46;
	const auto h = rect.height() * 0.3;
	auto path = QPainterPath();
	path.moveTo(c.x() - w, c.y());
	path.quadTo(c.x(), c.y() - 2 * h, c.x() + w, c.y());
	path.quadTo(c.x(), c.y() + 2 * h, c.x() - w, c.y());
	p.drawPath(path);
	p.drawEllipse(c, h * 0.62, h * 0.62);
	if (crossed) {
		const auto d = rect.width() * 0.36;
		p.drawLine(
			QPointF(c.x() - d, c.y() + d),
			QPointF(c.x() + d, c.y() - d));
	}
}

[[nodiscard]] QPainterPath StarPath(QPointF center, double outer, double inner) {
	auto path = QPainterPath();
	for (auto i = 0; i != 10; ++i) {
		const auto radius = (i % 2) ? inner : outer;
		const auto angle = (-90. + i * 36.) * M_PI / 180.;
		const auto point = center + QPointF(
			radius * std::cos(angle),
			radius * std::sin(angle));
		if (i) {
			path.lineTo(point);
		} else {
			path.moveTo(point);
		}
	}
	path.closeSubpath();
	return path;
}

// Type glyphs, drawn with lines to match both themes; fills, strokes,
// gradients and solids show their own color.
void PaintNodeIcon(
		QPainter &p,
		QRectF rect,
		const TreeRow &row,
		const QColor &fg) {
	auto hq = PainterHighQualityEnabler(p);
	const auto s = rect.width();
	const auto line = std::max(Scaled(13) / 10., 1.);
	auto pen = QPen(fg);
	pen.setWidthF(line);
	pen.setCapStyle(Qt::RoundCap);
	pen.setJoinStyle(Qt::RoundJoin);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	const auto m = s * 0.1;
	const auto r = rect.marginsRemoved(QMarginsF(m, m, m, m));
	const auto c = r.center();
	const auto swatchBorder = [&] {
		auto border = QPen(anim::with_alpha(st::windowFg->c, 0.25));
		border.setWidthF(line * 0.8);
		return border;
	};
	// Thin edges around a stroke ring (s * 0.2 wide, see below), like the
	// border of the fill swatches: a dark stroke stays visible on the
	// night background and a light one on the day background.
	const auto paintRingBorder = [&] {
		const auto inset = s * 0.14;
		const auto half = s * 0.1;
		p.setPen(swatchBorder());
		p.setBrush(Qt::NoBrush);
		const auto ring = r.marginsRemoved(
			QMarginsF(inset, inset, inset, inset));
		p.drawEllipse(ring.marginsAdded(
			QMarginsF(half, half, half, half)));
		p.drawEllipse(ring.marginsRemoved(
			QMarginsF(half, half, half, half)));
	};
	switch (row.kind) {
	case NodeKind::Layer:
		switch (row.layerType) {
		case LayerType::Shape:
			p.drawRoundedRect(
				QRectF(r.x(), r.y() + r.height() * 0.36, r.width() * 0.6, r.height() * 0.64),
				s * 0.08,
				s * 0.08);
			p.drawEllipse(QRectF(
				r.x() + r.width() * 0.34,
				r.y(),
				r.width() * 0.66,
				r.height() * 0.66));
			break;
		case LayerType::Null: {
			auto dashed = pen;
			dashed.setDashPattern({ 2., 1.6 });
			dashed.setCapStyle(Qt::FlatCap);
			p.setPen(dashed);
			p.drawRect(r.marginsRemoved(QMarginsF(line, line, line, line)));
			p.setPen(pen);
			p.drawLine(QPointF(c.x() - s * 0.14, c.y()), QPointF(c.x() + s * 0.14, c.y()));
			p.drawLine(QPointF(c.x(), c.y() - s * 0.14), QPointF(c.x(), c.y() + s * 0.14));
		} break;
		case LayerType::Precomp: {
			const auto front = QRectF(r.x(), r.y() + r.height() * 0.3, r.width() * 0.7, r.height() * 0.7);
			p.drawRoundedRect(front, s * 0.08, s * 0.08);
			auto back = QPainterPath();
			back.moveTo(r.x() + r.width() * 0.3, r.y() + r.height() * 0.3);
			back.lineTo(r.x() + r.width() * 0.3, r.y());
			back.lineTo(r.right(), r.y());
			back.lineTo(r.right(), r.y() + r.height() * 0.7);
			back.lineTo(r.x() + r.width() * 0.7, r.y() + r.height() * 0.7);
			p.drawPath(back);
		} break;
		case LayerType::Solid:
			p.setPen(swatchBorder());
			p.setBrush(row.color.isValid() ? row.color : fg);
			p.drawRoundedRect(r, s * 0.14, s * 0.14);
			break;
		case LayerType::Image: {
			p.drawRoundedRect(r, s * 0.1, s * 0.1);
			auto mountains = QPainterPath();
			mountains.moveTo(r.x() + r.width() * 0.12, r.bottom() - r.height() * 0.14);
			mountains.lineTo(r.x() + r.width() * 0.42, r.y() + r.height() * 0.5);
			mountains.lineTo(r.x() + r.width() * 0.62, r.y() + r.height() * 0.7);
			mountains.lineTo(r.x() + r.width() * 0.74, r.y() + r.height() * 0.58);
			mountains.lineTo(r.right() - r.width() * 0.1, r.bottom() - r.height() * 0.14);
			p.drawPath(mountains);
			p.drawEllipse(QPointF(r.x() + r.width() * 0.7, r.y() + r.height() * 0.28), s * 0.07, s * 0.07);
		} break;
		case LayerType::Text:
			p.drawLine(QPointF(r.x() + r.width() * 0.12, r.y() + line), QPointF(r.right() - r.width() * 0.12, r.y() + line));
			p.drawLine(QPointF(c.x(), r.y() + line), QPointF(c.x(), r.bottom()));
			break;
		default:
			p.drawEllipse(r);
			break;
		}
		break;
	case NodeKind::Shape:
		switch (row.shapeType) {
		case ShapeType::Group: {
			auto folder = QPainterPath();
			const auto top = r.y() + r.height() * 0.12;
			folder.moveTo(r.x(), r.bottom() - r.height() * 0.08);
			folder.lineTo(r.x(), top);
			folder.lineTo(r.x() + r.width() * 0.38, top);
			folder.lineTo(r.x() + r.width() * 0.5, top + r.height() * 0.16);
			folder.lineTo(r.right(), top + r.height() * 0.16);
			folder.lineTo(r.right(), r.bottom() - r.height() * 0.08);
			folder.closeSubpath();
			p.drawPath(folder);
		} break;
		case ShapeType::Rectangle:
			p.drawRoundedRect(r.marginsRemoved(QMarginsF(0, s * 0.1, 0, s * 0.1)), s * 0.06, s * 0.06);
			break;
		case ShapeType::Ellipse:
			p.drawEllipse(r.marginsRemoved(QMarginsF(line / 2, line / 2, line / 2, line / 2)));
			break;
		case ShapeType::Star:
			p.drawPath(StarPath(c, r.width() * 0.52, r.width() * 0.22));
			break;
		case ShapeType::Path: {
			auto path = QPainterPath();
			path.moveTo(r.x() + line, r.bottom() - line);
			path.cubicTo(
				QPointF(r.x() + r.width() * 0.2, r.y()),
				QPointF(r.x() + r.width() * 0.8, r.bottom()),
				QPointF(r.right() - line, r.y() + line));
			p.drawPath(path);
			p.setBrush(fg);
			p.drawEllipse(QPointF(r.x() + line, r.bottom() - line), line, line);
			p.drawEllipse(QPointF(r.right() - line, r.y() + line), line, line);
		} break;
		case ShapeType::Fill:
			p.setPen(swatchBorder());
			p.setBrush(row.color.isValid() ? row.color : fg);
			p.drawEllipse(r.marginsRemoved(QMarginsF(m, m, m, m)));
			break;
		case ShapeType::Stroke: {
			auto ring = QPen(row.color.isValid() ? row.color : fg);
			ring.setWidthF(s * 0.2);
			p.setPen(ring);
			p.drawEllipse(r.marginsRemoved(QMarginsF(s * 0.14, s * 0.14, s * 0.14, s * 0.14)));
			paintRingBorder();
		} break;
		case ShapeType::GradientFill:
		case ShapeType::GradientStroke: {
			auto gradient = QLinearGradient(r.topLeft(), r.bottomRight());
			gradient.setColorAt(0., row.color.isValid() ? row.color : fg);
			gradient.setColorAt(1., row.color2.isValid() ? row.color2 : fg);
			if (row.shapeType == ShapeType::GradientFill) {
				p.setPen(swatchBorder());
				p.setBrush(gradient);
				p.drawEllipse(r.marginsRemoved(QMarginsF(m, m, m, m)));
			} else {
				auto ring = QPen(QBrush(gradient), s * 0.2);
				p.setPen(ring);
				p.drawEllipse(r.marginsRemoved(QMarginsF(s * 0.14, s * 0.14, s * 0.14, s * 0.14)));
				paintRingBorder();
			}
		} break;
		case ShapeType::TrimPaths:
			p.drawArc(r.marginsRemoved(QMarginsF(m, m, m, m)), 90 * 16, -250 * 16);
			p.setBrush(fg);
			p.drawEllipse(QPointF(c.x(), r.y() + m), line, line);
			break;
		case ShapeType::Repeater:
			for (auto i = 0; i != 3; ++i) {
				const auto size = r.width() * 0.4;
				p.drawRect(QRectF(
					r.x() + i * r.width() * 0.3,
					r.y() + i * r.height() * 0.3,
					size,
					size));
			}
			break;
		case ShapeType::Transform:
			p.drawEllipse(c, s * 0.2, s * 0.2);
			p.drawLine(QPointF(c.x(), r.y()), QPointF(c.x(), r.bottom()));
			p.drawLine(QPointF(r.x(), c.y()), QPointF(r.right(), c.y()));
			break;
		default: {
			// Modifiers: two overlapping circles.
			const auto size = r.width() * 0.62;
			p.drawEllipse(QRectF(r.x(), r.y() + (r.height() - size) / 2, size, size));
			p.drawEllipse(QRectF(r.right() - size, r.y() + (r.height() - size) / 2, size, size));
		} break;
		}
		break;
	case NodeKind::Mask: {
		p.drawRoundedRect(r, s * 0.1, s * 0.1);
		auto dashed = pen;
		dashed.setDashPattern({ 1.6, 1.4 });
		p.setPen(dashed);
		p.drawEllipse(c, r.width() * 0.26, r.width() * 0.26);
	} break;
	case NodeKind::Effect: {
		auto spark = QPainterPath();
		const auto o = r.width() * 0.5;
		const auto i = r.width() * 0.12;
		spark.moveTo(c.x(), c.y() - o);
		spark.quadTo(c.x() + i, c.y() - i, c.x() + o, c.y());
		spark.quadTo(c.x() + i, c.y() + i, c.x(), c.y() + o);
		spark.quadTo(c.x() - i, c.y() + i, c.x() - o, c.y());
		spark.quadTo(c.x() - i, c.y() - i, c.x(), c.y() - o);
		p.drawPath(spark);
	} break;
	default:
		p.drawEllipse(r);
		break;
	}
}

} // namespace

class LayersPanel::Tree final
	: public Ui::RpWidget
	, public Ui::AbstractTooltipShower {
public:
	Tree(QWidget *parent, not_null<EditorController*> controller);
	~Tree();

	void setFilter(const QString &text);
	void setExpandedAll(bool expanded);
	void setVisibleRange(int top, int bottom);
	void selectAdjacent(int delta, bool extend);
	void selectFirstMatch();
	void renamePrimary();
	void setRestoreFocus(Fn<void()> callback);

	// { top, bottom } to make visible, the scroll delta while dragging.
	[[nodiscard]] rpl::producer<std::pair<int, int>> scrollToRequests() const;
	[[nodiscard]] rpl::producer<int> scrollByRequests() const;

	QString tooltipText() const override;
	QPoint tooltipPos() const override;
	bool tooltipWindowActive() const override;

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	enum class Part : uchar {
		None,
		Arrow,
		Icon,
		Name,
		Eye,
	};
	struct Drop {
		NodeId container = 0;
		int index = 0;
		int line = -1; // Insertion line y, -1 when dropping "into".
		int lineLeft = 0;
		int into = -1; // Row of the container when dropping "into".
		bool valid = false;
	};

	void rebuild();
	void appendRow(
		const Document &document,
		NodeId id,
		int parentRow,
		uint64 parentKey,
		int depth,
		bool dimmed,
		std::vector<NodeId> &assets);
	[[nodiscard]] bool matches(const Document &document, NodeId id) const;
	[[nodiscard]] bool subtreeMatches(
		const Document &document,
		NodeId id,
		int depth);
	void fillRow(TreeRow &row, const Document &document, const NodeInfo &node);
	void revealSelection();
	void toggleExpanded(int index, bool recursive);
	void setExpandedRecursive(
		const Document &document,
		NodeId id,
		uint64 key,
		bool expanded,
		int depth);
	void scrollToRow(int index);

	[[nodiscard]] int rowHeight() const;
	[[nodiscard]] int rowAt(int y) const;
	[[nodiscard]] int rowIndex(NodeId id) const;
	[[nodiscard]] int rowLeft(int depth) const;
	[[nodiscard]] Part partAt(int index, QPoint position) const;
	[[nodiscard]] QRect arrowRect(int index) const;
	[[nodiscard]] QRect iconRect(int index) const;
	[[nodiscard]] QRect nameRect(int index) const;
	[[nodiscard]] QRect eyeRect(int index) const;

	void clickRow(int index, Qt::KeyboardModifiers modifiers);
	void toggleHidden(int index);
	void updateOver(QPoint position);
	void updateTooltip();

	void startDrag();
	void updateDrop();
	void finishDrag(bool apply);
	void autoScrollStep();
	[[nodiscard]] Drop computeDrop(QPoint position) const;

	void startRename(int index);
	void finishRename(bool apply);
	void updateRenameGeometry();

	void showMenu(int index, QPoint globalPosition);
	void paintRow(QPainter &p, int index);
	void paintDrop(QPainter &p);

	const not_null<EditorController*> _controller;
	std::vector<TreeRow> _rows;
	base::flat_set<uint64> _expanded;
	QString _filter;
	std::unordered_map<NodeId, bool> _matches;
	int _visibleTop = 0;
	int _visibleBottom = 0;

	int _over = -1;
	Part _overPart = Part::None;
	int _pressed = -1;
	Part _pressedPart = Part::None;
	QPoint _pressPosition;
	NodeId _anchor = 0;

	NodeId _dragId = 0;
	uint64 _dragParentKey = 0;
	int _dragRow = -1;
	bool _dragging = false;
	Drop _drop;
	QPoint _lastGlobal;
	base::Timer _autoScrollTimer;

	NodeId _renameId = 0;
	base::unique_qptr<FieldHost> _renameHost;
	Ui::InputField *_renameField = nullptr;

	QString _tooltip;
	Fn<void()> _restoreFocus;
	base::unique_qptr<Ui::PopupMenu> _menu;

	rpl::event_stream<std::pair<int, int>> _scrollToRequests;
	rpl::event_stream<int> _scrollByRequests;

};

LayersPanel::Tree::Tree(
	QWidget *parent,
	not_null<EditorController*> controller)
: RpWidget(parent)
, _controller(controller)
, _autoScrollTimer([=] { autoScrollStep(); }) {
	setMouseTracking(true);

	_controller->documentChanged(
	) | rpl::on_next([=](const DocumentChange &change) {
		if (change.source == ChangeSource::Load) {
			_expanded.clear();
			_anchor = 0;
			finishRename(false);
			finishDrag(false);
		}
		rebuild();
	}, lifetime());

	_controller->selectionChanged(
	) | rpl::on_next([=] {
		revealSelection();
		update();
	}, lifetime());

	style::PaletteChanged() | rpl::on_next([=] {
		update();
	}, lifetime());

	rebuild();
}

LayersPanel::Tree::~Tree() {
	_renameField = nullptr;
	_renameHost = nullptr;
}

void LayersPanel::Tree::setRestoreFocus(Fn<void()> callback) {
	_restoreFocus = std::move(callback);
}

rpl::producer<std::pair<int, int>> LayersPanel::Tree::scrollToRequests() const {
	return _scrollToRequests.events();
}

rpl::producer<int> LayersPanel::Tree::scrollByRequests() const {
	return _scrollByRequests.events();
}

int LayersPanel::Tree::rowHeight() const {
	return Scaled(kRowHeight);
}

void LayersPanel::Tree::setFilter(const QString &text) {
	const auto filter = text.trimmed();
	if (_filter == filter) {
		return;
	}
	_filter = filter;
	finishDrag(false);
	rebuild();
	if (!_filter.isEmpty()) {
		_scrollToRequests.fire({ 0, 0 });
	} else {
		revealSelection();
	}
}

void LayersPanel::Tree::setExpandedAll(bool expanded) {
	const auto &document = _controller->document();
	if (!expanded) {
		_expanded.clear();
	} else if (document.valid()) {
		for (const auto id : document.layers()) {
			setExpandedRecursive(document, id, RowKey(0, id), true, 0);
		}
	}
	rebuild();
}

void LayersPanel::Tree::setVisibleRange(int top, int bottom) {
	const auto heightChanged = (bottom - top) != (_visibleBottom - _visibleTop);
	_visibleTop = top;
	_visibleBottom = bottom;
	if (heightChanged && width() > 0) {
		resizeToWidth(width());
	}
}

bool LayersPanel::Tree::matches(const Document &document, NodeId id) const {
	const auto node = document.node(id);
	return node
		&& (NodeDisplayName(document, id).contains(_filter, Qt::CaseInsensitive)
			|| NodeTypeText(*node).contains(_filter, Qt::CaseInsensitive));
}

bool LayersPanel::Tree::subtreeMatches(
		const Document &document,
		NodeId id,
		int depth) {
	if (const auto i = _matches.find(id); i != end(_matches)) {
		return i->second;
	}
	_matches[id] = false; // Guards against precomposition cycles.
	auto result = matches(document, id);
	if (!result && depth < kMaxDepth) {
		if (const auto node = document.node(id)) {
			for (const auto child : TreeChildren(document, *node)) {
				if (subtreeMatches(document, child, depth + 1)) {
					result = true;
					break;
				}
			}
		}
	}
	_matches[id] = result;
	return result;
}

void LayersPanel::Tree::fillRow(
		TreeRow &row,
		const Document &document,
		const NodeInfo &node) {
	row.kind = node.kind;
	row.layerType = node.layerType;
	row.shapeType = node.shapeType;
	row.name = NodeDisplayName(document, node.id);
	row.hidden = node.hidden;
	row.canHide = CanHide(node);
	row.canMove = CanMove(node);
	if (node.kind == NodeKind::Shape) {
		switch (node.shapeType) {
		case ShapeType::Fill:
		case ShapeType::Stroke:
			if (const auto value = document.baseValue({ node.id, "c" })) {
				row.color = value->color();
			}
			break;
		case ShapeType::GradientFill:
		case ShapeType::GradientStroke:
			if (const auto value = document.baseValue({ node.id, "g.k" })) {
				const auto count = document.json(node.id)
					.get("g")
					.get("p")
					.toInt(0);
				const auto stops = ColorStops(*value, count);
				if (!stops.empty()) {
					row.color = stops.front().color;
					row.color2 = stops.back().color;
				}
			}
			break;
		default:
			break;
		}
	} else if (node.kind == NodeKind::Layer
		&& node.layerType == LayerType::Solid) {
		if (const auto value = document.baseValue({ node.id, "sc" })) {
			row.color = value->color();
		}
	}
}

void LayersPanel::Tree::appendRow(
		const Document &document,
		NodeId id,
		int parentRow,
		uint64 parentKey,
		int depth,
		bool dimmed,
		std::vector<NodeId> &assets) {
	const auto node = document.node(id);
	if (!node || depth > kMaxDepth) {
		return;
	}
	const auto filtering = !_filter.isEmpty();
	if (filtering && !subtreeMatches(document, id, depth)) {
		return;
	}
	auto children = TreeChildren(document, *node);
	auto asset = NodeId();
	if (node->kind == NodeKind::Layer
		&& node->layerType == LayerType::Precomp
		&& node->precomp) {
		if (ranges::contains(assets, node->precomp)) {
			children.clear();
		} else {
			asset = node->precomp;
		}
	}
	const auto key = RowKey(parentKey, id);
	const auto index = int(_rows.size());
	{
		auto row = TreeRow();
		row.id = id;
		row.key = key;
		row.parentKey = parentKey;
		row.parentRow = parentRow;
		row.depth = depth;
		fillRow(row, document, *node);
		row.dimmed = dimmed || node->hidden;
		row.expandable = !children.empty();
		row.context = filtering && !matches(document, id);
		if (row.expandable) {
			row.expanded = filtering
				? ranges::any_of(children, [&](NodeId child) {
					return subtreeMatches(document, child, depth + 1);
				})
				: _expanded.contains(key);
		}
		_rows.push_back(std::move(row));
	}
	if (_rows[index].expanded) {
		if (asset) {
			assets.push_back(asset);
		}
		const auto childrenDimmed = _rows[index].dimmed;
		for (const auto child : children) {
			appendRow(
				document,
				child,
				index,
				key,
				depth + 1,
				childrenDimmed,
				assets);
		}
		if (asset) {
			assets.pop_back();
		}
	}
	_rows[index].blockEnd = int(_rows.size());
}

void LayersPanel::Tree::rebuild() {
	const auto &document = _controller->document();
	_rows.clear();
	_matches.clear();
	if (document.valid()) {
		auto assets = std::vector<NodeId>();
		for (const auto id : document.layers()) {
			appendRow(document, id, -1, 0, 0, false, assets);
		}
	}
	_over = -1;
	_overPart = Part::None;
	if (_dragging) {
		_dragRow = -1;
		for (auto i = 0; i != int(_rows.size()); ++i) {
			if (_rows[i].id == _dragId
				&& _rows[i].parentKey == _dragParentKey) {
				_dragRow = i;
				break;
			}
		}
		if (_dragRow < 0) {
			finishDrag(false);
		} else {
			_pressed = _dragRow;
			_drop = computeDrop(mapFromGlobal(_lastGlobal));
		}
	} else {
		_pressed = -1;
	}
	if (width() > 0) {
		resizeToWidth(width());
	}
	updateRenameGeometry();
	update();
}

int LayersPanel::Tree::resizeGetHeight(int newWidth) {
	return std::max(
		int(_rows.size()) * rowHeight() + Scaled(kBottomSkip),
		_visibleBottom - _visibleTop);
}

int LayersPanel::Tree::rowAt(int y) const {
	if (y < 0) {
		return -1;
	}
	const auto index = y / rowHeight();
	return (index < int(_rows.size())) ? index : -1;
}

int LayersPanel::Tree::rowIndex(NodeId id) const {
	if (!id) {
		return -1;
	}
	for (auto i = 0; i != int(_rows.size()); ++i) {
		if (_rows[i].id == id) {
			return i;
		}
	}
	return -1;
}

int LayersPanel::Tree::rowLeft(int depth) const {
	return Scaled(kPadding) / 2 + depth * Scaled(kIndent);
}

QRect LayersPanel::Tree::arrowRect(int index) const {
	return QRect(
		rowLeft(_rows[index].depth),
		index * rowHeight(),
		Scaled(kArrowWidth),
		rowHeight());
}

QRect LayersPanel::Tree::iconRect(int index) const {
	const auto arrow = arrowRect(index);
	const auto size = Scaled(kIconSize);
	return QRect(
		arrow.x() + arrow.width(),
		arrow.y() + (rowHeight() - size) / 2,
		size,
		size);
}

QRect LayersPanel::Tree::eyeRect(int index) const {
	const auto size = Scaled(kEyeWidth);
	return QRect(
		width() - Scaled(kPadding) / 2 - size,
		index * rowHeight(),
		size,
		rowHeight());
}

QRect LayersPanel::Tree::nameRect(int index) const {
	const auto icon = iconRect(index);
	const auto left = icon.x() + icon.width() + Scaled(kIconSkip);
	const auto right = _rows[index].canHide
		? eyeRect(index).x()
		: (width() - Scaled(kPadding));
	return QRect(
		left,
		index * rowHeight(),
		std::max(right - left, 0),
		rowHeight());
}

LayersPanel::Tree::Part LayersPanel::Tree::partAt(
		int index,
		QPoint position) const {
	if (index < 0 || index >= int(_rows.size())) {
		return Part::None;
	}
	const auto &row = _rows[index];
	if (row.canHide && eyeRect(index).contains(position)) {
		return Part::Eye;
	} else if (row.expandable
		&& arrowRect(index).marginsAdded(
			QMargins(Scaled(3), 0, 0, 0)).contains(position)) {
		return Part::Arrow;
	} else if (iconRect(index).marginsAdded(
			QMargins(0, Scaled(6), 0, Scaled(6))).contains(position)) {
		return Part::Icon;
	}
	return Part::Name;
}

void LayersPanel::Tree::setExpandedRecursive(
		const Document &document,
		NodeId id,
		uint64 key,
		bool expanded,
		int depth) {
	const auto node = document.node(id);
	if (!node || depth > kMaxDepth) {
		return;
	}
	const auto children = TreeChildren(document, *node);
	if (children.empty()) {
		return;
	}
	if (expanded) {
		_expanded.insert(key);
	} else {
		_expanded.remove(key);
	}
	for (const auto child : children) {
		setExpandedRecursive(
			document,
			child,
			RowKey(key, child),
			expanded,
			depth + 1);
	}
}

void LayersPanel::Tree::toggleExpanded(int index, bool recursive) {
	if (!_filter.isEmpty() || index < 0 || index >= int(_rows.size())) {
		return;
	}
	const auto row = _rows[index];
	if (!row.expandable) {
		return;
	}
	const auto expand = !row.expanded;
	if (recursive) {
		setExpandedRecursive(
			_controller->document(),
			row.id,
			row.key,
			expand,
			row.depth);
	} else if (expand) {
		_expanded.insert(row.key);
	} else {
		_expanded.remove(row.key);
	}
	rebuild();
}

void LayersPanel::Tree::scrollToRow(int index) {
	if (index >= 0 && index < int(_rows.size())) {
		_scrollToRequests.fire({
			index * rowHeight(),
			(index + 1) * rowHeight(),
		});
	}
}

void LayersPanel::Tree::revealSelection() {
	const auto primary = _controller->primarySelection();
	if (!primary) {
		return;
	}
	auto index = rowIndex(primary);
	if (index < 0 && _filter.isEmpty()) {
		const auto &document = _controller->document();
		const auto chain = TreeAncestors(document, primary);
		auto key = uint64();
		auto changed = false;
		for (const auto id : chain) {
			key = RowKey(key, id);
			if (!_expanded.contains(key)) {
				_expanded.insert(key);
				changed = true;
			}
		}
		if (changed) {
			rebuild();
		}
		index = rowIndex(primary);
		if (index < 0 && !chain.empty()) {
			// A group transform: show its group.
			index = rowIndex(chain.back());
		}
	}
	scrollToRow(index);
}

void LayersPanel::Tree::selectAdjacent(int delta, bool extend) {
	if (_rows.empty()) {
		return;
	}
	const auto count = int(_rows.size());
	auto index = rowIndex(_controller->primarySelection());
	index = (index < 0)
		? ((delta > 0) ? 0 : (count - 1))
		: std::clamp(index + delta, 0, count - 1);
	const auto id = _rows[index].id;
	_controller->select(id, extend ? SelectMode::Add : SelectMode::Replace);
	if (!extend) {
		_anchor = id;
	}
	scrollToRow(index);
}

void LayersPanel::Tree::selectFirstMatch() {
	for (auto i = 0; i != int(_rows.size()); ++i) {
		if (!_rows[i].context) {
			_anchor = _rows[i].id;
			_controller->select(_rows[i].id, SelectMode::Replace);
			scrollToRow(i);
			return;
		}
	}
}

void LayersPanel::Tree::renamePrimary() {
	startRename(rowIndex(_controller->primarySelection()));
}

void LayersPanel::Tree::clickRow(
		int index,
		Qt::KeyboardModifiers modifiers) {
	const auto id = _rows[index].id;
	if (modifiers & Qt::ControlModifier) {
		_controller->select(id, SelectMode::Toggle);
		_anchor = id;
		return;
	}
	const auto anchor = rowIndex(_anchor);
	if ((modifiers & Qt::ShiftModifier) && anchor >= 0) {
		auto ids = std::vector<NodeId>();
		const auto from = std::min(anchor, index);
		const auto till = std::max(anchor, index);
		for (auto i = from; i <= till; ++i) {
			if (_rows[i].id != id && !ranges::contains(ids, _rows[i].id)) {
				ids.push_back(_rows[i].id);
			}
		}
		ids.push_back(id); // Primary.
		_controller->setSelection(std::move(ids));
		return;
	}
	_anchor = id;
	_controller->select(id, SelectMode::Replace);
}

void LayersPanel::Tree::toggleHidden(int index) {
	const auto &row = _rows[index];
	const auto &document = _controller->document();
	auto ids = std::vector<NodeId>();
	if (_controller->isSelected(row.id)) {
		for (const auto id : _controller->selection()) {
			const auto node = document.node(id);
			if (node && CanHide(*node)) {
				ids.push_back(id);
			}
		}
	}
	if (ids.empty()) {
		ids.push_back(row.id);
	}
	_controller->setHidden(ids, !row.hidden);
}

void LayersPanel::Tree::updateOver(QPoint position) {
	const auto index = rowAt(position.y());
	const auto part = partAt(index, position);
	if (_over == index && _overPart == part) {
		return;
	}
	const auto rowChanged = (_over != index);
	const auto eyeChanged = (_overPart == Part::Eye) != (part == Part::Eye);
	_over = index;
	_overPart = part;
	setCursor((part == Part::Arrow || part == Part::Eye)
		? style::cur_pointer
		: style::cur_default);
	update();
	if (rowChanged || eyeChanged) {
		updateTooltip();
	}
}

void LayersPanel::Tree::updateTooltip() {
	_tooltip = QString();
	if (_over >= 0 && _over < int(_rows.size()) && !_dragging) {
		const auto &document = _controller->document();
		const auto &row = _rows[_over];
		if (_overPart == Part::Eye) {
			_tooltip = tr::lng_oblivion_lottie_layers_visibility(tr::now);
		} else if (const auto node = document.node(row.id)) {
			auto parts = QStringList{ row.name, NodeTypeText(*node) };
			if (node->hidden) {
				parts.push_back(tr::lng_oblivion_lottie_layers_hidden(tr::now));
			}
			if (node->parentLayer) {
				parts.push_back(tr::lng_oblivion_lottie_layers_parent(
					tr::now,
					lt_name,
					NodeDisplayName(document, node->parentLayer)));
			}
			if (node->matte != MatteMode::None) {
				parts.push_back(MatteModeText(node->matte));
			}
			_tooltip = parts.join(u" · "_q);
		}
	}
	if (_tooltip.isEmpty()) {
		Ui::Tooltip::Hide();
	} else {
		Ui::Tooltip::Show(kTooltipDelay, this);
	}
}

QString LayersPanel::Tree::tooltipText() const {
	return _tooltip;
}

QPoint LayersPanel::Tree::tooltipPos() const {
	return QCursor::pos();
}

bool LayersPanel::Tree::tooltipWindowActive() const {
	return Ui::AppInFocus() && Ui::InFocusChain(window());
}

void LayersPanel::Tree::mouseMoveEvent(QMouseEvent *e) {
	_lastGlobal = e->globalPosition().toPoint();
	if (_pressed >= 0
		&& !_dragging
		&& (e->buttons() & Qt::LeftButton)
		&& (_pressedPart == Part::Name || _pressedPart == Part::Icon)
		&& _pressed < int(_rows.size())
		&& _rows[_pressed].canMove
		&& _filter.isEmpty()) {
		const auto distance = (e->pos() - _pressPosition).manhattanLength();
		if (distance >= QGuiApplication::styleHints()->startDragDistance()) {
			startDrag();
		}
	}
	if (_dragging) {
		updateDrop();
		return;
	}
	updateOver(e->pos());
}

void LayersPanel::Tree::mousePressEvent(QMouseEvent *e) {
	_lastGlobal = e->globalPosition().toPoint();
	if (e->button() != Qt::LeftButton) {
		return;
	}
	finishRename(true);
	const auto index = rowAt(e->pos().y());
	const auto part = partAt(index, e->pos());
	if (index < 0) {
		if (!(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
			_controller->clearSelection();
		}
		return;
	} else if (part == Part::Arrow) {
		toggleExpanded(index, (e->modifiers() & Qt::AltModifier) != 0);
		return;
	} else if (part == Part::Eye) {
		toggleHidden(index);
		return;
	}
	_pressed = index;
	_pressedPart = part;
	_pressPosition = e->pos();
	clickRow(index, e->modifiers());
}

void LayersPanel::Tree::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	if (_dragging) {
		finishDrag(true);
	}
	_pressed = -1;
	_pressedPart = Part::None;
	updateOver(e->pos());
}

void LayersPanel::Tree::mouseDoubleClickEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto index = rowAt(e->pos().y());
	const auto part = partAt(index, e->pos());
	if (part == Part::Name) {
		startRename(index);
	} else if (part == Part::Icon) {
		toggleExpanded(index, false);
	}
}

void LayersPanel::Tree::contextMenuEvent(QContextMenuEvent *e) {
	const auto index = rowAt(e->pos().y());
	if (index < 0) {
		return;
	}
	const auto id = _rows[index].id;
	const auto key = _rows[index].key;
	if (!_controller->isSelected(id)) {
		_anchor = id;
		_controller->select(id, SelectMode::Replace);
	}
	// Selecting may have rebuilt the rows (revealing), find it again.
	auto found = -1;
	for (auto i = 0; i != int(_rows.size()); ++i) {
		if (_rows[i].key == key) {
			found = i;
			break;
		}
	}
	showMenu((found >= 0) ? found : rowIndex(id), e->globalPos());
}

void LayersPanel::Tree::leaveEventHook(QEvent *e) {
	if (!_dragging && _over >= 0) {
		_over = -1;
		_overPart = Part::None;
		update();
	}
	_tooltip = QString();
	Ui::Tooltip::Hide();
}

void LayersPanel::Tree::showMenu(int index, QPoint globalPosition) {
	if (index < 0 || index >= int(_rows.size())) {
		return;
	}
	const auto row = _rows[index];
	const auto id = row.id;
	const auto &document = _controller->document();
	const auto node = document.node(id);
	if (!node) {
		return;
	}
	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuWithIcons);
	_menu->addAction(
		tr::lng_oblivion_lottie_layers_rename(tr::now),
		[=] { startRename(rowIndex(id)); },
		&st::menuIconEdit);
	if (row.canMove) {
		_menu->addAction(
			tr::lng_oblivion_lottie_layers_duplicate(tr::now),
			[=] { _controller->duplicateNodes(_controller->selection()); },
			&st::menuIconCopy);
	}
	if (row.canHide) {
		const auto hidden = row.hidden;
		_menu->addAction(
			(hidden
				? tr::lng_oblivion_lottie_layers_show(tr::now)
				: tr::lng_oblivion_lottie_layers_hide(tr::now)),
			[=] {
				const auto index = rowIndex(id);
				if (index >= 0) {
					toggleHidden(index);
				}
			},
			hidden ? &st::menuIconShowInChat : &st::menuIconStealth);
	}
	if (row.canMove) {
		_menu->addAction(
			tr::lng_oblivion_lottie_layers_move_up(tr::now),
			[=] { _controller->reorderNode(id, -1); },
			&st::menuIconAbove);
		_menu->addAction(
			tr::lng_oblivion_lottie_layers_move_down(tr::now),
			[=] { _controller->reorderNode(id, 1); },
			&st::menuIconBelow);
	}
	const auto parent = (row.parentRow >= 0)
		? _rows[row.parentRow].id
		: NodeId();
	if (parent) {
		_menu->addAction(
			tr::lng_oblivion_lottie_layers_select_parent(tr::now),
			[=] {
				_anchor = parent;
				_controller->select(parent, SelectMode::Replace);
			},
			&st::menuIconSelect);
	}
	if (node->kind == NodeKind::Layer) {
		_menu->addAction(
			tr::lng_oblivion_lottie_mask_add(tr::now),
			[=] {
				const auto &document = _controller->document();
				if (document.contains(id)) {
					_controller->addMask(
						id,
						DefaultMaskPath(
							document,
							id,
							_controller->currentFrame()),
						MaskMode::Add,
						NewMaskName(document, id));
				}
			},
			&st::menuIconAdd);
	}
	_menu->addSeparator();
	_menu->addAction(
		tr::lng_oblivion_lottie_layers_expand_all(tr::now),
		[=] { setExpandedAll(true); },
		&st::menuIconExpand);
	_menu->addAction(
		tr::lng_oblivion_lottie_layers_collapse_all(tr::now),
		[=] { setExpandedAll(false); },
		&st::menuIconCollapse);
	if (row.canMove || node->kind == NodeKind::Mask
		|| node->kind == NodeKind::Effect) {
		_menu->addSeparator();
		_menu->addAction(
			tr::lng_oblivion_lottie_layers_delete(tr::now),
			[=] { _controller->deleteNodes(_controller->selection()); },
			&st::menuIconDelete);
	}
	_menu->popup(globalPosition);
}

void LayersPanel::Tree::startDrag() {
	if (_pressed < 0 || _pressed >= int(_rows.size())) {
		return;
	}
	finishRename(true);
	_dragging = true;
	_dragId = _rows[_pressed].id;
	_dragParentKey = _rows[_pressed].parentKey;
	_dragRow = _pressed;
	_tooltip = QString();
	Ui::Tooltip::Hide();
	setCursor(Qt::ClosedHandCursor);
	_autoScrollTimer.callEach(kAutoScrollDelay);
	updateDrop();
}

void LayersPanel::Tree::updateDrop() {
	_drop = computeDrop(mapFromGlobal(_lastGlobal));
	update();
}

void LayersPanel::Tree::autoScrollStep() {
	if (!_dragging) {
		_autoScrollTimer.cancel();
		return;
	}
	const auto y = mapFromGlobal(_lastGlobal).y();
	const auto zone = Scaled(kAutoScrollZone);
	const auto step = Scaled(kAutoScrollStep);
	auto delta = 0;
	if (y < _visibleTop + zone && _visibleTop > 0) {
		delta = -step;
	} else if (y > _visibleBottom - zone && _visibleBottom < height()) {
		delta = step;
	}
	if (delta) {
		_scrollByRequests.fire_copy(delta);
		updateDrop();
	}
}

void LayersPanel::Tree::finishDrag(bool apply) {
	if (!_dragging) {
		return;
	}
	_dragging = false;
	_autoScrollTimer.cancel();
	setCursor(style::cur_default);
	const auto drop = std::exchange(_drop, Drop());
	const auto id = std::exchange(_dragId, NodeId());
	_dragRow = -1;
	update();
	if (apply && drop.valid && id) {
		_controller->moveNode(id, drop.container, drop.index);
	}
}

LayersPanel::Tree::Drop LayersPanel::Tree::computeDrop(QPoint position) const {
	const auto &document = _controller->document();
	const auto dragged = document.node(_dragId);
	if (!dragged || _rows.empty() || _dragRow < 0) {
		return {};
	}
	const auto h = rowHeight();
	const auto y = std::clamp(position.y(), 0, int(_rows.size()) * h - 1);
	const auto index = y / h;
	const auto within = y - index * h;
	const auto padding = Scaled(kPadding);

	if (dragged->kind == NodeKind::Layer) {
		// Rows on the level of the dragged layer (same parent row).
		auto target = index;
		while (target >= 0
			&& !(_rows[target].kind == NodeKind::Layer
				&& _rows[target].parentKey == _dragParentKey)) {
			target = _rows[target].parentRow;
		}
		if (target < 0 || target == _dragRow) {
			return {};
		}
		const auto &row = _rows[target];
		const auto node = document.node(row.id);
		if (!node || node->composition != dragged->composition) {
			return {};
		}
		const auto before = (index == target) && (within < h / 2);
		return {
			.container = dragged->composition,
			.index = before ? node->index : (node->index + 1),
			.line = before ? (target * h) : (row.blockEnd * h),
			.lineLeft = rowLeft(row.depth) + padding,
			.valid = true,
		};
	} else if (dragged->kind != NodeKind::Shape) {
		return {};
	}
	const auto &row = _rows[index];
	const auto node = document.node(row.id);
	if (!node
		|| row.id == _dragId
		|| document.isDescendant(row.id, _dragId)) {
		return {};
	}
	const auto container = IsShapeContainer(*node);
	if (node->kind == NodeKind::Layer || node->kind != NodeKind::Shape) {
		if (!container) {
			return {};
		}
		return {
			.container = row.id,
			.index = 0,
			.into = index,
			.valid = true,
		};
	}
	const auto quarter = h / 4;
	if (container
		&& ((within >= quarter && within < h - quarter)
			|| (row.expanded && within >= h - quarter))) {
		return {
			.container = row.id,
			.index = 0,
			.into = index,
			.valid = true,
		};
	}
	const auto before = (within < h / 2);
	return {
		.container = node->parent,
		.index = before ? node->index : (node->index + 1),
		.line = before ? (index * h) : (row.blockEnd * h),
		.lineLeft = rowLeft(row.depth) + padding,
		.valid = true,
	};
}

void LayersPanel::Tree::startRename(int index) {
	if (index < 0 || index >= int(_rows.size())) {
		return;
	}
	finishRename(true);
	const auto &row = _rows[index];
	const auto node = _controller->document().node(row.id);
	if (!node) {
		return;
	}
	_renameId = row.id;
	_renameHost = base::make_unique_q<FieldHost>(this);
	_renameField = Ui::CreateChild<Ui::InputField>(
		_renameHost.get(),
		PanelFieldStyle(),
		Ui::InputField::Mode::SingleLine,
		nullptr,
		node->name.isEmpty() ? row.name : node->name);
	updateRenameGeometry();
	_renameHost->show();
	_renameField->show();
	_renameField->selectAll();
	_renameField->setFocusFast();
	_renameField->submits() | rpl::on_next([=] {
		finishRename(true);
	}, _renameField->lifetime());
	_renameField->cancelled() | rpl::on_next([=] {
		finishRename(false);
	}, _renameField->lifetime());
	_renameField->focusedChanges() | rpl::filter(
		!rpl::mappers::_1
	) | rpl::on_next([=] {
		finishRename(true);
	}, _renameField->lifetime());
	scrollToRow(index);
}

void LayersPanel::Tree::updateRenameGeometry() {
	if (!_renameHost) {
		return;
	}
	const auto index = rowIndex(_renameId);
	if (index < 0) {
		finishRename(false);
		return;
	}
	const auto name = nameRect(index);
	const auto height = std::min(Scaled(kRenameHeight), rowHeight());
	const auto left = name.x() - Scaled(6);
	_renameHost->setGeometry(
		left,
		index * rowHeight() + (rowHeight() - height) / 2,
		std::max(width() - Scaled(kPadding) - left, Scaled(60)),
		height);
	_renameField->setGeometry(_renameHost->rect());
}

void LayersPanel::Tree::finishRename(bool apply) {
	if (!_renameHost) {
		return;
	}
	auto host = std::move(_renameHost);
	const auto field = std::exchange(_renameField, nullptr);
	const auto id = std::exchange(_renameId, NodeId());
	const auto text = field->getLastText().trimmed();
	if (Ui::InFocusChain(host.get()) && _restoreFocus) {
		_restoreFocus();
	}
	host->hide();
	host.release()->deleteLater();
	if (apply && !text.isEmpty()) {
		const auto node = _controller->document().node(id);
		if (node && node->name != text) {
			_controller->rename(id, text);
		}
	}
}

void LayersPanel::Tree::paintRow(QPainter &p, int index) {
	const auto &row = _rows[index];
	const auto h = rowHeight();
	const auto y = index * h;
	const auto selected = _controller->isSelected(row.id);
	if (selected) {
		p.fillRect(0, y, width(), h, st::windowBgActive);
	} else if (index == _over && !_dragging) {
		p.fillRect(0, y, width(), h, st::windowBgOver);
	}
	if (_dragging && _drop.valid && _drop.into == index) {
		auto hq = PainterHighQualityEnabler(p);
		auto pen = QPen(st::activeLineFg);
		pen.setWidthF(Scaled(2));
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		const auto inset = Scaled(2);
		p.drawRoundedRect(
			QRectF(0, y, width(), h).marginsRemoved(
				QMarginsF(inset, inset, inset, inset)),
			Scaled(4),
			Scaled(4));
	}
	const auto fg = selected ? st::windowFgActive->c : st::windowFg->c;
	const auto sub = selected
		? anim::with_alpha(st::windowFgActive->c, 0.75)
		: st::windowSubTextFg->c;
	const auto dragged = _dragging && (index == _dragRow);
	p.setOpacity(dragged ? 0.4 : row.dimmed ? 0.5 : 1.);

	if (row.expandable) {
		PaintArrow(p, arrowRect(index), row.expanded, sub);
	}
	PaintNodeIcon(p, iconRect(index), row, selected ? fg : sub);

	const auto name = nameRect(index);
	p.setFont(st::normalFont);
	p.setPen(row.context ? sub : fg);
	p.drawText(
		name,
		Qt::AlignLeft | Qt::AlignVCenter,
		st::normalFont->elided(row.name, name.width()));

	if (row.canHide
		&& (row.hidden || (index == _over && !_dragging))) {
		const auto eye = eyeRect(index);
		const auto size = Scaled(kIconSize);
		const auto overEye = (index == _over) && (_overPart == Part::Eye);
		PaintEye(
			p,
			QRectF(
				eye.x() + (eye.width() - size) / 2.,
				eye.y() + (eye.height() - size) / 2.,
				size,
				size),
			overEye ? fg : sub,
			row.hidden);
	}
	p.setOpacity(1.);
}

void LayersPanel::Tree::paintDrop(QPainter &p) {
	if (!_dragging || !_drop.valid || _drop.line < 0) {
		return;
	}
	auto hq = PainterHighQualityEnabler(p);
	const auto thickness = Scaled(2);
	const auto radius = Scaled(3);
	const auto left = _drop.lineLeft;
	const auto y = std::clamp(_drop.line, thickness, height() - thickness);
	p.setPen(Qt::NoPen);
	p.setBrush(st::activeLineFg);
	p.drawRoundedRect(
		QRectF(
			left + radius,
			y - thickness / 2.,
			std::max(width() - Scaled(kPadding) - left - radius, 0),
			thickness),
		thickness / 2.,
		thickness / 2.);
	auto pen = QPen(st::activeLineFg);
	pen.setWidthF(thickness);
	p.setPen(pen);
	p.setBrush(st::windowBg);
	p.drawEllipse(QPointF(left + radius, y), radius, radius);
}

void LayersPanel::Tree::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto clip = e->rect();
	p.fillRect(clip, st::windowBg);
	if (_rows.empty()) {
		const auto padding = Scaled(kPadding);
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		const auto text = _filter.isEmpty()
			? tr::lng_oblivion_lottie_editor_no_layers(tr::now)
			: tr::lng_oblivion_lottie_layers_nothing_found(tr::now);
		p.drawText(
			QRect(padding, Scaled(8), width() - 2 * padding, rowHeight()),
			Qt::AlignLeft | Qt::AlignVCenter,
			st::normalFont->elided(text, width() - 2 * padding));
		if (_filter.isEmpty()) {
			// How to start, the + button is in the panel header above.
			const auto top = Scaled(8) + rowHeight();
			p.drawText(
				QRect(
					padding,
					top,
					width() - 2 * padding,
					std::max(height() - top, 0)),
				Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
				tr::lng_oblivion_lottie_layers_empty_hint(tr::now));
		}
		return;
	}
	const auto h = rowHeight();
	const auto from = std::max(clip.top() / h, 0);
	const auto till = std::min(clip.bottom() / h + 1, int(_rows.size()));
	for (auto i = from; i < till; ++i) {
		paintRow(p, i);
	}
	paintDrop(p);
}

// LayersPanel.

LayersPanel::LayersPanel(
	QWidget *parent,
	not_null<EditorController*> controller)
: RpWidget(parent)
, _controller(controller)
, _add(Ui::CreateChild<ToolButton>(
	this,
	st::menuIconAdd,
	tr::lng_oblivion_lottie_layers_add()))
, _searchHost(Ui::CreateChild<FieldHost>(this))
, _search(Ui::CreateChild<Ui::InputField>(
	_searchHost.get(),
	PanelSearchStyle(),
	Ui::InputField::Mode::SingleLine,
	tr::lng_oblivion_lottie_layers_search(),
	QString()))
, _scroll(Ui::CreateChild<Ui::ScrollArea>(this, st::defaultScrollArea))
, _tree(_scroll->setOwnedWidget(
	object_ptr<Tree>(_scroll.get(), controller)).data()) {
	setFocusPolicy(Qt::ClickFocus);
	_add->setClickedCallback([=] { showAddMenu(); });
	setupSearch();
	setupTree();
	style::PaletteChanged() | rpl::on_next([=] {
		update();
	}, lifetime());
}

LayersPanel::~LayersPanel() = default;

void LayersPanel::setupSearch() {
	_searchHost->sizeValue() | rpl::on_next([=](QSize size) {
		_search->setGeometry(QRect(QPoint(), size));
	}, _searchHost->lifetime());

	// Clear button, like in the chats list search.
	const auto cancel = Ui::CreateChild<Ui::CrossButton>(
		this,
		st::dialogsCancelSearch);
	cancel->toggle(false, anim::type::instant);
	cancel->setClickedCallback([=] {
		_search->clear();
		_search->setFocusFast();
	});
	_searchHost->geometryValue() | rpl::on_next([=](QRect geometry) {
		cancel->moveToLeft(
			geometry.x() + geometry.width() - cancel->width(),
			geometry.y() + (geometry.height() - cancel->height()) / 2,
			width());
		cancel->raise();
	}, cancel->lifetime());

	_search->changes() | rpl::on_next([=] {
		const auto text = _search->getLastText();
		cancel->toggle(!text.isEmpty(), anim::type::normal);
		_tree->setFilter(text);
	}, _search->lifetime());
	_search->submits() | rpl::on_next([=] {
		_tree->selectFirstMatch();
	}, _search->lifetime());
	_search->cancelled() | rpl::on_next([=] {
		if (!_search->getLastText().isEmpty()) {
			_search->clear();
		} else {
			setFocus();
		}
	}, _search->lifetime());
}

void LayersPanel::setupTree() {
	_tree->setRestoreFocus([=] { setFocus(); });
	const auto updateVisible = [=] {
		const auto top = _scroll->scrollTop();
		_tree->setVisibleRange(top, top + _scroll->height());
	};
	_scroll->scrollTopValue() | rpl::on_next(updateVisible, lifetime());
	_scroll->heightValue() | rpl::on_next(updateVisible, lifetime());
	_scroll->widthValue() | rpl::on_next([=](int width) {
		_tree->resizeToWidth(width);
	}, lifetime());
	_tree->scrollToRequests(
	) | rpl::on_next([=](std::pair<int, int> range) {
		crl::on_main(this, [=] {
			if (range.first == range.second) {
				_scroll->scrollToY(range.first);
			} else {
				_scroll->scrollToY(range.first, range.second);
			}
		});
	}, lifetime());
	_tree->scrollByRequests() | rpl::on_next([=](int delta) {
		_scroll->scrollToY(_scroll->scrollTop() + delta);
	}, lifetime());
}

void LayersPanel::setFilter(const QString &text) {
	_search->setText(text);
	_tree->setFilter(text);
}

void LayersPanel::setExpandedAll(bool expanded) {
	_tree->setExpandedAll(expanded);
}

int LayersPanel::headerHeight() const {
	return Scaled(kHeaderHeight);
}

void LayersPanel::updateGeometries() {
	const auto padding = Scaled(kPadding);
	const auto header = headerHeight();
	_add->moveToRight(
		padding - Scaled(6),
		(header - _add->height()) / 2,
		width());
	const auto searchHeight = PanelSearchStyle().heightMin;
	_searchHost->setGeometry(
		padding,
		header - Scaled(4),
		std::max(width() - 2 * padding, 0),
		searchHeight);
	const auto top = _searchHost->y()
		+ searchHeight
		+ Scaled(kSearchSkip);
	_scroll->setGeometry(0, top, width(), std::max(height() - top, 0));
}

void LayersPanel::resizeEvent(QResizeEvent *e) {
	updateGeometries();
}

void LayersPanel::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(e->rect(), st::windowBg);
	const auto padding = Scaled(kPadding);
	const auto header = headerHeight();
	p.setFont(st::semiboldFont);
	p.setPen(st::windowBoldFg);
	p.drawText(
		padding + Scaled(2),
		(header - Scaled(4) - st::semiboldFont->height) / 2
			+ st::semiboldFont->ascent,
		tr::lng_oblivion_lottie_editor_layers(tr::now));
}

void LayersPanel::keyPressEvent(QKeyEvent *e) {
	const auto modifiers = e->modifiers()
		& ~(Qt::KeypadModifier | Qt::GroupSwitchModifier);
	const auto shift = (modifiers & Qt::ShiftModifier) != 0;
	const auto plain = !(modifiers
		& (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
	if (plain) {
		switch (e->key()) {
		case Qt::Key_Up:
			_tree->selectAdjacent(-1, shift);
			return;
		case Qt::Key_Down:
			_tree->selectAdjacent(1, shift);
			return;
		case Qt::Key_Return:
		case Qt::Key_Enter:
		case Qt::Key_F2:
			if (!shift) {
				_tree->renamePrimary();
				return;
			}
			break;
		}
	}
	RpWidget::keyPressEvent(e);
}

void LayersPanel::showAddMenu() {
	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuWithIcons);
	const auto addLayer = [=](
			LayerTemplate type,
			QString name,
			std::optional<ShapeTemplate> content) {
		return [=] {
			_controller->addLayer(type, name, content);
		};
	};
	_menu->addAction(
		tr::lng_oblivion_lottie_layers_new_rectangle(tr::now),
		addLayer(
			LayerTemplate::Shape,
			ShapeTypeText(ShapeType::Rectangle),
			ShapeTemplate::Rectangle),
		&st::menuIconAdd);
	_menu->addAction(
		tr::lng_oblivion_lottie_layers_new_ellipse(tr::now),
		addLayer(
			LayerTemplate::Shape,
			ShapeTypeText(ShapeType::Ellipse),
			ShapeTemplate::Ellipse),
		&st::menuIconAdd);
	_menu->addAction(
		tr::lng_oblivion_lottie_layers_new_star(tr::now),
		addLayer(
			LayerTemplate::Shape,
			ShapeTypeText(ShapeType::Star),
			ShapeTemplate::Star),
		&st::menuIconAdd);
	_menu->addAction(
		tr::lng_oblivion_lottie_layers_new_empty(tr::now),
		addLayer(
			LayerTemplate::Shape,
			LayerTypeText(LayerType::Shape),
			std::nullopt),
		&st::menuIconAdd);
	_menu->addAction(
		tr::lng_oblivion_lottie_layers_new_null(tr::now),
		addLayer(
			LayerTemplate::Null,
			LayerTypeText(LayerType::Null),
			std::nullopt),
		&st::menuIconAdd);

	const auto &document = _controller->document();
	const auto container = ShapeContainerFor(
		document,
		_controller->primarySelection());
	if (container) {
		_menu->addSeparator();
		const auto name = NodeDisplayName(document, container);
		const auto add = [&](ShapeType type, ShapeTemplate shape) {
			const auto typeName = ShapeTypeText(type);
			_menu->addAction(
				tr::lng_oblivion_lottie_layers_add_into(
					tr::now,
					lt_type,
					typeName,
					lt_name,
					name),
				[=] { _controller->addShape(container, shape, typeName); },
				nullptr);
		};
		add(ShapeType::Group, ShapeTemplate::Group);
		add(ShapeType::Rectangle, ShapeTemplate::Rectangle);
		add(ShapeType::Ellipse, ShapeTemplate::Ellipse);
		add(ShapeType::Star, ShapeTemplate::Star);
		add(ShapeType::Fill, ShapeTemplate::Fill);
		add(ShapeType::Stroke, ShapeTemplate::Stroke);
		add(ShapeType::GradientFill, ShapeTemplate::GradientFill);
		add(ShapeType::GradientStroke, ShapeTemplate::GradientStroke);
		add(ShapeType::TrimPaths, ShapeTemplate::TrimPaths);
		add(ShapeType::Repeater, ShapeTemplate::Repeater);
		add(ShapeType::RoundCorners, ShapeTemplate::RoundCorners);
	}
	// A mask for the layer of the selection: a rectangle around what the
	// layer shows, its points are then edited with the pen on the canvas.
	if (const auto layer = document.owningLayer(
			_controller->primarySelection())) {
		_menu->addSeparator();
		_menu->addAction(
			tr::lng_oblivion_lottie_mask_add_to(
				tr::now,
				lt_name,
				NodeDisplayName(document, layer)),
			[=] {
				const auto &document = _controller->document();
				if (document.contains(layer)) {
					_controller->addMask(
						layer,
						DefaultMaskPath(
							document,
							layer,
							_controller->currentFrame()),
						MaskMode::Add,
						NewMaskName(document, layer));
				}
			},
			&st::menuIconAdd);
	}
	_menu->popup(_add->mapToGlobal(QPoint(0, _add->height())));
}

// Snapshot scenes (OBLIVION_SELFTEST=ui, see oblivion_ui_snapshots.h).

namespace {

[[nodiscard]] QWidget *CreateLayersScene(not_null<Ui::RpWidget*> parent) {
	return Ui::CreateChild<PanelSceneHost>(
		parent.get(),
		u":/animations/palette.tgs"_q,
		[](QWidget *parent, not_null<EditorController*> controller) {
			return not_null<Ui::RpWidget*>(
				Ui::CreateChild<LayersPanel>(parent, controller));
		});
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	RegisterScene(u"lottie_layers"_q, QSize(300, 640), CreateLayersScene, [](
			not_null<QWidget*> widget) {
		const auto host = static_cast<PanelSceneHost*>(widget.get());
		const auto controller = host->controller();
		const auto &document = controller->document();
		const auto layers = document.layers();
		if (layers.size() > 3) {
			controller->setHidden({ layers[3] }, true);
		}
		if (const auto stroke = FindShapeOfType(
				controller->document(),
				ShapeType::Stroke)) {
			controller->select(stroke);
		}
	});

	RegisterScene(u"lottie_layers_search"_q, QSize(300, 480), CreateLayersScene, [](
			not_null<QWidget*> widget) {
		const auto host = static_cast<PanelSceneHost*>(widget.get());
		static_cast<LayersPanel*>(host->panel().get())->setFilter(
			u"stroke"_q);
	});
});

} // namespace

} // namespace Oblivion::LottieEdit
