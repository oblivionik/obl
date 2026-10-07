/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

// Oblivion looks (oblivion_look.h) on two screens of Telegram itself. Each
// function is called from one place of the upstream code, paints only what
// a look adds there and does nothing at all with the plain look: the caller
// paints what it painted before.
namespace Oblivion::Look {

// Info::Profile::TopBar::paintEvent(), right after the background of the
// bar, for a profile that has no colour of its own.
//
//   «Родной, но лучше»  the part of the bar with the userpic and the name
//                       is a panel, the action buttons stay on the page.
//   «Ночной эфир»       the cover behind the upper part of the userpic, it
//                       goes up with the userpic when the bar collapses,
//                       and a gradient ring around a round userpic.
//   «Тишина»            nothing, as in the mockup.
struct ProfileTop {
	QRect bar; // The whole top bar.
	QRect userpic; // Where the userpic is painted now.
	bool roundEdges = false; // The bar has the rounded top corners of a box.
	bool actions = false; // The row of action buttons is in the bar.
	bool ring = false; // The userpic is a circle with nothing around it.
};
void PaintProfileTop(QPainter &p, const ProfileTop &args);

// Dialogs::Ui::PaintRow(), instead of the fill of the row. bg is what the
// row would be filled with: the colour of the chosen or of the hovered row.
// True: the row is painted (the chosen one as a rounded card, in «Тишина»
// flat with the accent bar; the hovered one as a rounded card). False:
// nothing is painted, fill the row as usual.
[[nodiscard]] bool PaintDialogRow(
	QPainter &p,
	const QRect &row,
	const QBrush &base,
	const QBrush &bg,
	bool active,
	bool selected);

// Dialogs::Ui::PaintRow(), around the ripple of the row: while it lives,
// the painter is clipped to the card PaintDialogRow() paints, so a click
// does not flash the corners around the card. card: the row is chosen or
// hovered. Does nothing with the plain look and in «Тишина».
class DialogRippleClip final {
public:
	DialogRippleClip(QPainter &p, const QRect &row, bool card);
	~DialogRippleClip();

	DialogRippleClip(const DialogRippleClip &other) = delete;
	DialogRippleClip &operator=(const DialogRippleClip &other) = delete;

private:
	QPainter *_p = nullptr;

};

// OBLIVION_SELFTEST=look_hooks: with the plain look nothing is painted.
[[nodiscard]] bool RunHooksSelfTest(QStringList &log);

} // namespace Oblivion::Look
