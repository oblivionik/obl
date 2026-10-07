/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtGui/QBrush>

// Oblivion looks («Тема Oblivion»).
//
// A look is a way the app is painted. Look 0 is the plain Telegram (the
// default): with it nothing here changes a single pixel. Looks 1..3 are
// the three directions of the design mockups (design-mockups/src/styles.py
// has the exact colours):
//
//   1  «Родной, но лучше»  Telegram colours, cards and pills.
//   2  «Ночной эфир»       violet night, translucent cards, one gradient.
//   3  «Тишина»            near black or paper, hairlines, one acid accent.
//
// A look consists of:
//
//  - a palette layer: a list of colours put over the main palette (so the
//    whole app changes colours) and taken off exactly. It has a day and a
//    night set, the one that fits the theme under it is used. It is never
//    written anywhere: the theme files and the theme cache of the user
//    keep the colours of the theme, see the hooks at the end of this file;
//
//  - the colours and the shapes of the surfaces the mod paints by itself,
//    see Color(), CardRadius() and the Paint...() helpers below. They are
//    cheap (a cached table) and are meant to be called from paintEvent.
//    With look 0 every helper that takes a "plain" value returns or paints
//    exactly that value, so a caller that passes what it paints today
//    changes nothing by default.
//
// Repainting: a look change replaces palette colours and then fires
// style::PaletteChanged(), exactly like a theme change, so the main
// window repaints by itself. A widget that caches something made of the
// colours, or lives in its own window, or has a layout that depends on
// the look, subscribes to Updates().

namespace style {
class palette;
} // namespace style

namespace Ui {
struct ChatThemeBackground;
} // namespace Ui

namespace Oblivion::Look {

inline constexpr auto kPlain = 0;
inline constexpr auto kNative = 1;
inline constexpr auto kNightAir = 2;
inline constexpr auto kSilence = 3;
inline constexpr auto kCount = 4;

// The look that is painted now. It is the saved choice, except while a
// Telegram theme is being edited (then it is 0, the editor has to show the
// colours of the theme) and before the themes are started.
[[nodiscard]] int Current();
[[nodiscard]] bool Is(int look);
// The current look, then every other one.
[[nodiscard]] rpl::producer<int> Value();
// Fires after the look, its day / night set or its chat background has
// changed, when the palette already has the new colours.
[[nodiscard]] rpl::producer<> Updates();
// Whether the night set of the colours is in use. It follows the theme
// under the look: a dark theme gets the night set.
[[nodiscard]] bool Dark();

// The saved choice («Настройки → Oblivion → Тема Oblivion») and the way
// to change it: saved to oblivion.json and applied at once.
[[nodiscard]] int Chosen();
[[nodiscard]] rpl::producer<int> ChosenValue();
void Set(int look);

// The colours of the surfaces painted by the mod.
enum class Role {
	Ground,      // The page behind the cards (a window of the mod).
	Card,        // A card on the ground.
	CardRaised,  // A card on a Telegram surface (a box, the profile).
	CardStroke,  // The outline of a card, transparent when there is none.
	Pill,        // Pills, inputs and tags inside a card.
	Divider,     // Hairlines.
	Text,
	SubText,
	FaintText,
	Accent,      // Accent text and icons.
	AccentFill,  // A filled accent: buttons, sliders.
	OnAccent,    // Text over AccentFill and over PaintAccentGradient().
	Tint,        // A translucent accent fill.
	Selected,    // A selected or hovered row.
	Online,
	Highlight,   // The loudest fill: «Войти», counters.
	OnHighlight,
	Room,        // The chip «в комнате»: text and icon.
	RoomTint,    // Its fill.
	RoomFill,    // Its button.
	OnRoom,
	Inverse,     // The selected tab.
	OnInverse,

	kCount,
};
// With look 0: the closest colour of the Telegram theme.
[[nodiscard]] QColor Color(Role role);
// With look 0: plain, whatever it is.
[[nodiscard]] QColor Color(Role role, const QColor &plain);

// The shapes. Radiuses are in pixels of the current interface scale,
// "plain" is what the caller uses today and what look 0 returns.
[[nodiscard]] int CardRadius(int plain);
[[nodiscard]] int TileRadius(int plain); // Tool tiles, list cells.
[[nodiscard]] int RowRadius(int plain); // A selected row.
[[nodiscard]] int ChipRadius(int height, int plain);
[[nodiscard]] int AvatarRadius(int size); // Half of the size, 30% in look 3.
// Look 3 draws hairlines where the others draw cards.
[[nodiscard]] bool HasCards();
// Looks 2 and 3 write section labels in capitals.
[[nodiscard]] bool CapsLabels();
// The accent gradient of look 2 (pink, violet, blue, 120 degrees); the
// other looks have both stops of one colour, Role::AccentFill.
[[nodiscard]] QGradientStops AccentStops();
[[nodiscard]] QBrush AccentBrush(const QRectF &rect);

// Painting. Every helper sets the pen and the brush it needs and leaves
// them changed; with a look on, the shapes are antialiased inside the
// helper, with look 0 the hints of the painter are not touched.
enum class Surface {
	Ground, // The card lies on Role::Ground.
	Window, // The card lies on a Telegram surface (windowBg).
};
// Look 0: a rounded rect of plainBg with plainRadius, nothing else.
// Look 1: a filled card. Look 2: a translucent card with a stroke.
// Look 3: no card, a hairline along the bottom edge.
void PaintCard(
	QPainter &p,
	const QRectF &rect,
	const QColor &plainBg,
	int plainRadius,
	Surface surface = Surface::Window);

enum class Chip {
	Accent, // An activity chip («слушает», «в Oblivion»).
	Room, // «в комнате».
	Neutral, // A tag or a preset pill.
	Live, // The big «сейчас слушает» block: a card, not a pill. In
	// look 2 it has the soft gradient.
};
// Look 0: a rounded rect of plainBg with plainRadius. Look 3 paints only
// a hairline under the chip (its chips are rows).
void PaintChip(
	QPainter &p,
	const QRectF &rect,
	const QColor &plainBg,
	int plainRadius,
	Chip chip = Chip::Accent);
// The colours of the text on a chip: the value and the label before it.
[[nodiscard]] QColor ChipText(Chip chip, const QColor &plain);
[[nodiscard]] QColor ChipLabel(Chip chip, const QColor &plain);

// A filled accent shape: the main button, a counter, a progress. Look 0:
// plain. Look 2: the gradient. Text over it: Color(Role::OnAccent, ...).
void PaintAccentGradient(
	QPainter &p,
	const QRectF &rect,
	qreal radius,
	const QColor &plain);
// A selected row. Look 0: a rounded rect of plainBg with plainRadius.
// Look 2: the soft gradient with a stroke. Look 3: a flat fill and an
// accent bar at the left edge.
void PaintSelected(
	QPainter &p,
	const QRectF &rect,
	const QColor &plainBg,
	int plainRadius);
// A hairline. Look 0: plain.
void PaintDivider(QPainter &p, const QRectF &rect, const QColor &plain);

// The helpers below paint only what a look adds. They return false and
// paint nothing when the current look has no such thing (always with
// look 0), the caller then paints what it painted before.
//
// The page of a window of the mod (the room, the tools): the ground and,
// in look 2, the colour glow over it. "whole" is the size of the window
// the glow is laid out in (rect is the part to repaint), empty: rect.
bool PaintGround(QPainter &p, const QRect &rect, QSize whole = QSize());
// The cover at the top of a profile (look 2).
bool PaintCover(QPainter &p, const QRect &rect);
// The gradient ring around a round track cover (look 2): "cover" is the
// square of the picture, the ring is painted outside of it. Returns the
// width the ring takes on every side, 0 when there is none.
int PaintCoverRing(QPainter &p, const QRectF &cover);

// The colours a look puts over the palette, for the settings preview and
// for the self-test.
struct PaletteEntry {
	int index = 0; // In style::palette.
	QColor value;
};
[[nodiscard]] const std::vector<PaletteEntry> &PaletteFor(
	int look,
	bool dark);

// The colour of the Telegram theme, even while a look hides it.
[[nodiscard]] QColor ThemeColor(const style::color &color);
// Whether the theme under the look is a dark one.
[[nodiscard]] bool ThemeDark();

// What a small preview of a look needs (oblivion_look_ui.h), with look 0
// these are the colours of the Telegram theme.
struct PreviewColors {
	QColor ground;
	QColor list;
	QColor chat;
	QColor card;
	QColor cardStroke;
	QColor selected;
	QColor selectedStroke;
	QColor selectedText;
	QColor selectedSubText;
	QColor text;
	QColor subText;
	QColor accent;
	QColor badge;
	QColor bubbleIn;
	QColor bubbleInStroke;
	QColor bubbleInText;
	QColor bubbleOut;
	QColor bubbleOutTo; // The end of the gradient, or bubbleOut again.
	QColor bubbleOutText;
	QColor avatars[3];
	QColor bar; // At the left of the selected row, transparent: none.
	QColor chip;
	QColor chipStroke;
	QColor chipText;
	QColor divider;
	QColor glow[3]; // Transparent when there is none.
	QColor gradient[3];
	bool gradients = false;
	bool cards = true;
	int avatarPercent = 50;
	int bubbleRadius = 0; // At the 100% scale.
	int rowRadius = 0;
};
[[nodiscard]] PreviewColors PreviewFor(int look, bool dark);

// Snapshot scenes and tests only: the look that is painted whatever the
// settings say, -1 to follow them again. Nothing is saved.
void ForceForTests(int look);

// The "look" self-test (see oblivion_selftest.h): the palette layer is
// taken off exactly, the day and the night sets are complete, the setting
// survives a save.
[[nodiscard]] bool RunSelfTest(QStringList &log);

// The hooks of window/themes/window_theme.cpp. The theme code replaces the
// whole palette (a new theme, the night mode, the editor), changes a few
// colours by itself (the service colours follow the wallpaper) and saves
// the palette to be able to revert a theme. So:
//
//  - before it reads or replaces the palette it calls TakeOffForTheme():
//    the palette is exactly the one of the theme again;
//  - when it has finished it calls PutOnAfterTheme(): the look is put
//    over the palette that is there now. True: the palette was changed
//    and style::NotifyPaletteChanged() has to follow.
//
// Both do nothing with look 0.
void ThemeStarted(rpl::lifetime &lifetime);
void TakeOffForTheme();
[[nodiscard]] bool PutOnAfterTheme();
void ThemeEditingChanged();
// The chat background of the look, used only while the user has not
// chosen a wallpaper. False: the look has none or it must not be used.
[[nodiscard]] bool ChatBackground(Ui::ChatThemeBackground &result);
// The gradient of the outgoing bubbles from the top of the chat to the
// bottom, the way Telegram paints the bubbles of its gradient themes.
// Empty: the bubbles are plain (msgOutBg).
[[nodiscard]] std::vector<QColor> ChatBubbles();

namespace details {

// Puts colours over a palette and takes them off, see TakeOffForTheme().
class PaletteLayer final {
public:
	[[nodiscard]] bool on() const {
		return _on;
	}
	// Whether every colour is still the one that was put on. False: the
	// palette was replaced under the layer.
	[[nodiscard]] bool intact() const;

	void putOn(
		const style::palette &palette,
		const std::vector<PaletteEntry> &entries);
	// Restores what the palette had in putOn().
	void takeOff();
	// The palette was replaced, there is nothing to restore.
	void forget();

	// The colour that was in the palette before putOn().
	[[nodiscard]] std::optional<QColor> original(
		const style::color &color) const;

private:
	struct Applied {
		style::color color;
		QColor value;
		QColor original;
	};
	std::vector<Applied> _list;
	bool _on = false;

};

// The six colours that the theme code derives from the wallpaper
// ("adjusted" in colors.palette), put on for a look that has its own chat
// background. The theme code may change them at any moment, so each one
// is restored only while it still has the value of the look.
class ServiceLayer final {
public:
	[[nodiscard]] bool on() const {
		return !_list.empty();
	}

	void putOn(const style::palette &palette, const QColor &background);
	// Puts back the colours the theme code has changed. True: some were.
	bool refresh(const QColor &background);
	void takeOff();
	void forget();

	// The colour the theme has there now, under the one of the look.
	[[nodiscard]] std::optional<QColor> original(
		const style::color &color) const;

private:
	struct Applied {
		style::color color;
		QColor value;
		QColor original;
	};
	std::vector<Applied> _list;

};

} // namespace details
} // namespace Oblivion::Look
