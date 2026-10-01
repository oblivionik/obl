/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/unique_qptr.h"
#include "ui/rp_widget.h"

namespace Ui {
class InputField;
class PopupMenu;
class ScrollArea;
} // namespace Ui

namespace Oblivion::LottieEdit {

class EditorController;
class FieldHost;
class ToolButton;

// Left panel of the Lottie editor: the layer / shape tree.
//
// Rows: root layers (first is drawn on top), a layer expands into its
// masks, effects and shape items (groups expand further, group "tr"
// transforms are not listed, they are edited in the inspector), a
// precomposition layer expands into the layers of its asset. Each row
// has a type icon (fills / strokes / gradients / solids in their own
// color), the name and a visibility eye (shown on hover, always for
// hidden nodes). The model has no "locked" flag, so there is no lock.
//
// Mouse: click selects (Cmd toggles, Shift selects a range), the arrow
// expands (Option: the whole branch), the eye hides / shows (the whole
// selection if the row is selected), double click on the name renames
// inline, dragging a row reorders layers inside their composition or
// moves shape items (between, or into groups / shape layers), the right
// button opens the context menu (rename, duplicate, delete, hide / show,
// move up / down, select the parent, expand / collapse all).
//
// Keys while the panel has focus: Up / Down select the previous / next
// row (Shift extends), Return / F2 renames; the rest (Delete, Cmd+D,
// Cmd+Z...) are the editor's global shortcuts. The search field filters
// the tree by name / type (Escape clears it). The "+" button adds new
// layers, and shapes into the selected shape layer / group.
//
// The selection is the controller's: selecting on the canvas expands
// the tree to the node and scrolls to it.
class LayersPanel final : public Ui::RpWidget {
public:
	LayersPanel(QWidget *parent, not_null<EditorController*> controller);
	~LayersPanel();

	void setFilter(const QString &text);
	void setExpandedAll(bool expanded);

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	class Tree;

	void setupSearch();
	void setupTree();
	void updateGeometries();
	void showAddMenu();
	[[nodiscard]] int headerHeight() const;

	const not_null<EditorController*> _controller;
	const not_null<ToolButton*> _add;
	const not_null<FieldHost*> _searchHost;
	const not_null<Ui::InputField*> _search;
	const not_null<Ui::ScrollArea*> _scroll;
	const not_null<Tree*> _tree;
	base::unique_qptr<Ui::PopupMenu> _menu;

};

} // namespace Oblivion::LottieEdit
