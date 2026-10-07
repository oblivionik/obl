/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Ui {
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

// «Инструменты Oblivion»: every tool of the mod on one screen, as in the
// "hub" sheets of the design mockups. The tools are tiles in four groups
// (создавать, вместе, знать, приложение) with a search field above them
// and, while the account has a room open, a «Комната идёт сейчас» banner
// with «Войти».
//
// A tile only opens the entry point the tool has already (the same call
// as its row in Settings > Oblivion); a tool that needs a chat is opened
// for the chat of the window if there is one, otherwise a toast says how
// to reach it. Nothing is sent or requested by opening the screen.
//
// The screen follows the Oblivion look (oblivion_look.h): a grid of cards
// in the plain look and in «Родной, но лучше», translucent cards on the
// glow in «Ночной эфир», numbered rows between hairlines in «Тишина».
namespace Oblivion::Hub {

// The main menu, Settings > Oblivion.
void Show(not_null<Window::SessionController*> controller);

// The hook of Window::MainMenu: the row «Инструменты Oblivion».
void AddMainMenuEntry(
	not_null<Ui::VerticalLayout*> menu,
	not_null<Window::SessionController*> controller);

// ---- Pure logic (self-tested, no session, no styles).

// What the search compares: lower case, «ё» as «е», everything that is
// not a letter or a digit is one space.
[[nodiscard]] QString SearchKey(const QString &text);
// Every word of the query (a SearchKey) starts some word of the key or,
// when it is three letters or longer, is found inside one.
[[nodiscard]] bool SearchMatches(const QString &key, const QString &query);

inline constexpr auto kGroups = 4;

struct Metrics {
	int width = 0;
	int padding = 0; // At the left and at the right.
	int gap = 0; // Between the columns.
	int rowGap = 0;
	int tile = 0; // The height of a tile or of a row.
	int label = 0; // The height of the label of a group.
	int groupSkip = 0; // After a group.
	int columns = 1;
	bool list = false; // Rows, the groups stand in two columns.
	int banner = 0; // The height of the room banner, 0: none.
	int bannerSkip = 0;
	int bottom = 0;
};

struct Placed {
	struct Label {
		int group = 0;
		QRect rect;
	};
	struct Tile {
		int group = 0;
		int index = 0; // In the group.
		int number = 0; // From 1, in the order of reading.
		QRect rect;
	};
	std::vector<Label> labels;
	std::vector<Tile> tiles;
	QRect banner;
	int height = 0;
};

// counts: how many tools of each group are shown. A group without tools
// takes no place.
[[nodiscard]] Placed Place(
	const std::array<int, kGroups> &counts,
	const Metrics &metrics);

// OBLIVION_SELFTEST=hub: the search, the placing, the table of the tools.
[[nodiscard]] bool RunSelfTest(QStringList &log);

} // namespace Oblivion::Hub
