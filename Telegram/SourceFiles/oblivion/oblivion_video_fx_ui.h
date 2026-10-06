/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/unique_qptr.h"
#include "oblivion/oblivion_video_fx.h"
#include "ui/rp_widget.h"

namespace style {
struct PopupMenu;
} // namespace style

namespace Ui {
class PopupMenu;
class Show;
class VerticalLayout;
} // namespace Ui

// Video effects (oblivion_video_fx.h): the stack of effects of a project
// of the video editor (oblivion_video_editor.h) as a list of cards. A card
// has a switch, the buttons that move it up and down and remove it, and,
// when it is opened, the controls of the effect made from the description
// of its parameters: sliders, switches, lists of options, colours and
// a "randomize" button for a seed.
//
// The panel only edits a VideoFx::Stack value and tells about every change,
// the frames of the preview are the business of its owner. It needs no
// session and no network.
namespace Oblivion::VideoFx {

// More of them at once are too slow to be previewed while the video plays.
inline constexpr auto kMaxEffects = 12;

struct PanelArgs {
	std::shared_ptr<Ui::Show> show; // Toasts and the box of a colour.
	Stack stack;
	int expanded = -1; // The effect whose parameters are shown, if any.

	// Before a change: the moment for an undo to remember the stack.
	// Called once for a whole drag of a slider.
	Fn<void()> started;

	// After every change. tuning: a slider is being dragged and more
	// changes follow, the last one of them comes without it.
	Fn<void(const Stack &stack, bool tuning)> changed;

	// The card that was added or opened, from top to bottom in the
	// coordinates of the panel: its owner scrolls to it.
	Fn<void(int top, int bottom)> revealed;
};

class Panel final : public Ui::RpWidget {
public:
	Panel(QWidget *parent, PanelArgs &&args);
	~Panel();

	[[nodiscard]] const Stack &stack() const {
		return _stack;
	}

	// From the outside (an undo): nothing is reported back.
	void setStack(const Stack &stack);

	// The menus of the buttons the owner has: all the effects by their
	// groups and the ready stacks.
	void showAddMenu(QPoint globalPosition);
	void showPresetsMenu(QPoint globalPosition);

	// Reported as a change, like everything the user does here.
	void removeAll();

	// The list is rebuilt a moment after a change, not inside of the click
	// that has made it.
	[[nodiscard]] bool rebuilding() const {
		return _rebuildPending;
	}

protected:
	int resizeGetHeight(int newWidth) override;

private:
	void rebuild();
	void scheduleRebuild();
	void buildBody(int index);
	void started();
	void changed(bool tuning);
	[[nodiscard]] bool valid(int generation, int index, Type type) const;

	void add(Type type);
	void applyPreset(int index);
	void remove(int index);
	void move(int index, int delta);
	void toggle(int index);
	void expand(int index);
	void setValue(int index, int param, float64 value, bool tuning);
	void setMix(int index, float64 percent, bool tuning);
	void chooseColor(int index, int param);

	const std::shared_ptr<Ui::Show> _show;
	const Fn<void()> _started;
	const Fn<void(const Stack &stack, bool tuning)> _changed;
	const Fn<void(int top, int bottom)> _revealed;
	const not_null<Ui::VerticalLayout*> _list;
	Stack _stack;
	int _expanded = -1;

	// The rows of an older list do nothing: their indices may be wrong.
	int _generation = 0;
	bool _rebuildPending = false;
	bool _revealPending = false;

	// While the rows are replaced the panel keeps its height: a list that
	// is empty for a moment would make the box around it forget how far
	// it was scrolled.
	bool _rebuilding = false;

	// The menu of all the effects is not taller than the screen. It keeps
	// a reference to its style, so the style is destroyed after it.
	std::unique_ptr<style::PopupMenu> _addMenuSt;
	base::unique_qptr<Ui::PopupMenu> _menu;

};

// The names of the effects that are switched on, "Blob tracking, Old TV":
// what a button that opens the panel says. Empty without them.
[[nodiscard]] QString Summary(const Stack &stack);

// Where the effect that was at the index in one stack is in another one
// (what an undo has brought back): at the same place when it only got
// other parameters, at the new place when the effects were moved, added
// or removed. -1 when it is not there.
[[nodiscard]] int FollowEffect(const Stack &was, int index, const Stack &now);

// What is under the mouse on something painted with windowBgOver (a card,
// a pill, a button of the editor): windowBgRipple is almost the same
// colour in the dark themes.
[[nodiscard]] QColor OverBg();

} // namespace Oblivion::VideoFx
