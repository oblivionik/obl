/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_look.h"

#include "core/launcher.h"
#include "oblivion/oblivion_settings.h"
#include "ui/chat/chat_theme.h"
#include "ui/image/image_prepare.h"
#include "ui/painter.h"
#include "ui/style/style_core_palette.h"
#include "ui/style/style_palette_colorizer.h"
#include "window/themes/window_theme.h"
#include "settings.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace Oblivion::Look {
namespace {

constexpr auto kDarkThreshold = 0.5;
constexpr auto kPi = 3.14159265358979323846;
// The mockups are drawn for a profile 400 px wide and a chat 820 x 1000.
constexpr auto kCoverReferenceWidth = 400.;
constexpr auto kChatReference = QSize(820, 1000);
// Like the gradient wallpapers of Telegram: a small picture, dithered
// and then stretched over the chat (Ui::GenerateDitheredGradient).
constexpr auto kChatImage = QSize(512, 512);
constexpr auto kGroundScale = 4; // The glow is rendered 4 times smaller.
constexpr auto kGroundCacheSize = 3;
constexpr auto kRingWidth = 10;
constexpr auto kRingGap = 5;
constexpr auto kAccentBar = 3;

[[nodiscard]] QColor Rgb(uint32 value) {
	return QColor(
		int((value >> 16) & 0xFF),
		int((value >> 8) & 0xFF),
		int(value & 0xFF));
}

[[nodiscard]] QColor WithAlpha(QColor color, float64 alpha) {
	color.setAlpha(std::clamp(qRound(alpha * 255.), 0, 255));
	return color;
}

[[nodiscard]] QColor Rgba(uint32 value, float64 alpha) {
	return WithAlpha(Rgb(value), alpha);
}

// A translucent colour over an opaque one, opaque again.
[[nodiscard]] QColor Over(const QColor &bottom, const QColor &top) {
	const auto alpha = top.alpha();
	const auto mix = [&](int from, int to) {
		return (from * (255 - alpha) + to * alpha + 127) / 255;
	};
	return QColor(
		mix(bottom.red(), top.red()),
		mix(bottom.green(), top.green()),
		mix(bottom.blue(), top.blue()));
}

[[nodiscard]] QColor Mix(const QColor &a, const QColor &b, float64 ratio) {
	return Over(a, WithAlpha(QColor(b.red(), b.green(), b.blue()), ratio));
}

[[nodiscard]] QColor Hsl(float64 hue, float64 saturation, float64 lightness) {
	const auto color = QColor::fromHslF(
		hue / 360.,
		saturation,
		lightness).toRgb();
	return QColor(color.red(), color.green(), color.blue());
}

[[nodiscard]] bool Same(const QColor &a, const QColor &b) {
	return a.rgba() == b.rgba();
}

void Put(const style::color &color, const QColor &value) {
	color.set(
		uchar(value.red()),
		uchar(value.green()),
		uchar(value.blue()),
		uchar(value.alpha()));
}

struct Shape {
	int card = 0; // Radiuses at the 100% scale.
	int tile = 0;
	int row = 0;
	int bubble = 0;
	int avatarPercent = 50;
	bool cards = true;
	bool strokes = false;
	bool gradients = false;
	bool caps = false;
	bool background = false; // Has a chat background of its own.
};

// Everything a look is made of. The values come from the mockup styles
// (design-mockups/src/styles.py), the comments name the CSS variables.
struct Tokens {
	// The palette layer. Opaque, except for the line.
	QColor window; // --panel (1), --solid (2), --bg (3)
	QColor over;
	QColor ripple;
	QColor raised; // Inputs: --panel2
	QColor strip; // Between the sections of settings: --bg
	QColor title;
	QColor menu;
	QColor menuOver;
	QColor dialogs;
	QColor dialogsOver;
	QColor sideBar;
	QColor line; // --line, --stroke
	QColor border;
	QColor fg; // --fg
	QColor fg2; // --fg2
	QColor fg3; // --fg3
	QColor accent; // --accent
	QColor accentIcon;
	QColor service; // Names and media in the chats list.
	QColor onlineText;
	QColor fill; // Filled accent.
	QColor fillOver;
	QColor fillRipple;
	QColor onFill; // --on-accent
	QColor sel; // --sel, --soft
	QColor selRipple;
	QColor selFg;
	QColor selFg2; // --sel-fg2
	QColor selDraft;
	QColor selBadge;
	QColor onSelBadge;
	QColor selBadgeMuted;
	QColor selOnline;
	QColor badge; // --badge, --grad, --hl
	QColor onBadge;
	QColor badgeMuted; // --badge-muted
	QColor tick;
	QColor green; // --green
	QColor bubIn; // --bub-in
	QColor bubInSel;
	QColor bubInShadow;
	QColor inFg2;
	QColor inAccent;
	QColor inLink;
	QColor replyIn;
	QColor bubOut; // --bub-out, --out
	QColor bubOutSel;
	QColor bubOutShadow;
	QColor outFg;
	QColor outFg2; // --bub-out-meta
	QColor outAccent;
	QColor outLink;
	QColor outTick;
	QColor fileIn;
	QColor fileInOver;
	QColor fileInIcon;
	QColor fileOut;
	QColor fileOutOver;
	QColor fileOutIcon;
	QColor waveIn;
	QColor waveInOff;
	QColor waveOut;
	QColor waveOutOff;
	QColor userpicFg;
	float64 userpicSaturation = -1.; // Below zero: those of the theme.
	float64 userpicLightness = 0.;

	// The surfaces painted by the mod, may be translucent.
	QColor ground; // --bg
	QColor card; // --panel
	QColor cardRaised;
	QColor cardStroke; // --stroke
	QColor pill; // --panel2
	QColor divider;
	QColor sub;
	QColor faint;
	QColor tint; // --tint
	QColor selected;
	QColor highlight; // --hl
	QColor onHighlight; // --hl-fg
	QColor room; // --violet
	QColor roomTint; // --violet-tint
	QColor roomFill;
	QColor onRoom; // --violet-on
	QColor inverse; // --inv
	QColor onInverse; // --inv-fg
	QColor solid; // Behind the rings: --solid
	QColor gradient[3]; // --g1, --g2, --g3
	QColor soft[3]; // --soft
	QColor softStroke;
	QColor glow[3]; // --aur1, --aur2, --aur3
	QColor chat; // The chat background in a preview.
	Shape shape;
};

void FillNoGradients(Tokens &t) {
	for (auto &color : t.gradient) {
		color = t.fill;
	}
	for (auto &color : t.soft) {
		color = t.selected;
	}
	for (auto &color : t.glow) {
		color = QColor(0, 0, 0, 0);
	}
	t.softStroke = QColor(0, 0, 0, 0);
}

// 1. «Родной, но лучше»: body.d1 and body.d1.light.
[[nodiscard]] Tokens NativeTokens(bool dark) {
	auto t = Tokens();
	if (dark) {
		t.window = Rgb(0x17212b);
		t.over = Rgb(0x202e3c);
		t.ripple = Rgb(0x283848);
		t.raised = Rgb(0x202e3c);
		t.strip = Rgb(0x0e1621);
		t.title = Rgb(0x1c2836);
		t.menu = Rgb(0x17212b);
		t.menuOver = Rgb(0x202e3c);
		t.dialogs = Rgb(0x17212b);
		t.dialogsOver = Rgb(0x1e2a37);
		t.sideBar = Rgb(0x0e1621);
		t.line = Rgba(0x0b121a, 0.8);
		t.fg = Rgb(0xf4f6f8);
		t.fg2 = Rgb(0x8194a7);
		t.fg3 = Rgb(0x566777);
		t.accent = Rgb(0x62b6f7);
		t.fill = Rgb(0x4a9fe8);
		t.fillOver = Rgb(0x55a8ee);
		t.fillRipple = Rgb(0x3f8fd6);
		t.sel = Rgb(0x2b5278);
		t.selRipple = Rgb(0x346089);
		t.selFg2 = Rgb(0xc3d8ec);
		t.selDraft = Rgb(0xa9caf0);
		t.selBadgeMuted = Rgb(0x7aa3ca);
		t.badge = Rgb(0x4a9fe8);
		t.badgeMuted = Rgb(0x3d5266);
		t.green = Rgb(0x4fd266);
		t.bubIn = Rgb(0x182533);
		t.bubInShadow = QColor(0, 0, 0, 0);
		t.bubOut = Rgb(0x2b5278);
		t.bubOutShadow = QColor(0, 0, 0, 0);
		t.outFg = Rgb(0xf4f6f8);
		t.outFg2 = Rgb(0x8fb6dc);
		t.outAccent = Rgb(0x9fd2ff);
		t.outLink = Rgb(0x9fd2ff);
		t.outTick = Rgb(0x7cc4ff);
		t.fileOut = Rgb(0x5aa9ee);
		t.fileOutOver = Rgb(0x66b2f3);
		t.waveInOff = Rgb(0x3a4d61);
		t.waveOut = Rgb(0x9fd2ff);
		t.waveOutOff = Rgb(0x4b7fb3);
		t.ground = Rgb(0x0e1621);
		t.divider = Rgb(0x0b121a);
		t.tint = Rgba(0x62b6f7, 0.14);
		t.room = Rgb(0xa998ff);
		t.roomTint = Rgba(0xa998ff, 0.16);
		t.onRoom = Rgb(0x17122f);
	} else {
		t.window = Rgb(0xffffff);
		t.over = Rgb(0xeff2f5);
		t.ripple = Rgb(0xe3e8ed);
		t.raised = Rgb(0xeff2f5);
		t.strip = Rgb(0xedf0f3);
		t.title = Rgb(0xeff2f5);
		t.menu = Rgb(0xffffff);
		t.menuOver = Rgb(0xeff2f5);
		t.dialogs = Rgb(0xffffff);
		t.dialogsOver = Rgb(0xf3f5f7);
		t.sideBar = Rgb(0xedf0f3);
		t.line = Rgb(0xe6e9ed);
		t.fg = Rgb(0x10171e);
		t.fg2 = Rgb(0x75828f);
		t.fg3 = Rgb(0xa4aeb8);
		t.accent = Rgb(0x2f8fe6);
		t.fill = Rgb(0x2f8fe6);
		t.fillOver = Rgb(0x3a97ea);
		t.fillRipple = Rgb(0x2680d2);
		t.sel = Rgb(0x3f96e3);
		t.selRipple = Rgb(0x3589d4);
		t.selFg2 = Rgb(0xe2f0fc);
		t.selDraft = Rgb(0xc6e1f7);
		t.selBadgeMuted = Rgb(0xc6e1f7);
		t.badge = Rgb(0x3f96e3);
		t.badgeMuted = Rgb(0xb3bcc5);
		t.green = Rgb(0x3fc457);
		t.bubIn = Rgb(0xffffff);
		t.bubInShadow = Rgba(0x748ea2, 0.16);
		t.bubOut = Rgb(0xe3f8cf);
		t.bubOutShadow = Rgba(0x3ac346, 0.11);
		t.outFg = Rgb(0x10171e);
		t.outFg2 = Rgb(0x5aa851);
		t.outAccent = Rgb(0x3d9a35);
		t.outLink = Rgb(0x2f8fe6);
		t.outTick = Rgb(0x52b345);
		t.fileOut = Rgb(0x5fbe67);
		t.fileOutOver = Rgb(0x55b35d);
		t.waveInOff = Rgb(0xd4dee6);
		t.waveOut = Rgb(0x5ebd66);
		t.waveOutOff = Rgb(0xb3e2b4);
		t.ground = Rgb(0xedf0f3);
		t.divider = Rgb(0xe6e9ed);
		t.tint = Rgba(0x2f8fe6, 0.11);
		t.room = Rgb(0x7a63f0);
		t.roomTint = Rgba(0x7a63f0, 0.12);
		t.onRoom = Rgb(0xffffff);
	}
	t.border = Mix(t.window, t.fg, 0.12);
	t.accentIcon = t.accent;
	t.service = t.accent;
	t.onlineText = t.accent;
	t.onFill = Rgb(0xffffff);
	t.selFg = Rgb(0xffffff);
	t.selBadge = Rgb(0xffffff);
	t.onSelBadge = t.sel;
	t.selOnline = Rgb(0xffffff);
	t.onBadge = Rgb(0xffffff);
	t.tick = t.accent;
	t.bubInSel = Mix(t.bubIn, t.fill, dark ? 0.45 : 0.22);
	t.inFg2 = dark ? Rgb(0x6f8192) : Rgb(0x8e9aa6);
	t.inAccent = t.accent;
	t.inLink = t.accent;
	t.replyIn = t.accent;
	t.bubOutSel = dark
		? Mix(t.bubOut, t.fill, 0.45)
		: Mix(t.bubOut, Rgb(0x4fae4e), 0.25);
	t.fileIn = t.fill;
	t.fileInOver = t.fillOver;
	t.fileInIcon = Rgb(0xffffff);
	t.fileOutIcon = Rgb(0xffffff);
	t.waveIn = t.accent;
	t.userpicFg = Rgb(0xffffff);

	t.card = t.window;
	t.cardRaised = t.over;
	t.cardStroke = QColor(0, 0, 0, 0);
	t.pill = t.over;
	t.sub = t.fg2;
	t.faint = t.fg3;
	t.selected = t.tint;
	t.highlight = t.fill;
	t.onHighlight = t.onFill;
	t.roomFill = t.room;
	t.inverse = t.tint;
	t.onInverse = t.accent;
	t.solid = t.window;
	FillNoGradients(t);
	t.chat = dark ? Rgb(0x0e1621) : Rgb(0xcfe0c8);
	t.shape = {
		.card = 14,
		.tile = 13,
		.row = 12,
		.bubble = 16,
	};
	return t;
}

// 2. «Ночной эфир»: body.d2 and body.d2.light. The translucent values of
// the mockup are composed over --solid here, the palette needs opaque
// backgrounds and text.
[[nodiscard]] Tokens NightAirTokens(bool dark) {
	auto t = Tokens();
	if (dark) {
		t.window = Rgb(0x150e26);
		t.over = Rgb(0x231c33);
		t.ripple = Rgb(0x2c253c);
		t.strip = Rgb(0x0b0716);
		t.title = Rgb(0x110b20);
		t.menu = Rgb(0x1c1430);
		t.menuOver = Rgb(0x2a2140);
		t.dialogsOver = Rgb(0x1f1731);
		t.sideBar = Rgb(0x0f0a1c);
		t.line = Rgba(0xffffff, 0.11);
		t.border = Rgb(0x2f2940);
		t.fg = Rgb(0xf7f3ff);
		t.fg2 = Rgb(0x998fb1);
		t.fg3 = Rgb(0x645b78);
		t.accent = Rgb(0xbb9bff);
		t.onlineText = Rgb(0x4be3a4);
		t.fill = Rgb(0x8f61ff);
		t.fillOver = Rgb(0x9b71ff);
		t.fillRipple = Rgb(0x7d50ea);
		t.sel = Rgb(0x35224f);
		t.selRipple = Rgb(0x402a5e);
		t.selFg2 = Rgb(0xe0d7f3);
		t.selDraft = Rgb(0xff8fa3);
		t.badge = Rgb(0xb765f0);
		t.badgeMuted = Rgb(0x4b4366);
		t.tick = Rgb(0x44c6ff);
		t.green = Rgb(0x4be3a4);
		t.bubIn = Rgb(0x272040);
		t.bubInShadow = QColor(0, 0, 0, 0);
		t.replyIn = Rgb(0xff5fc4);
		t.waveInOff = Rgb(0x645b78);
		t.ground = Rgb(0x0b0716);
		t.card = Rgba(0xffffff, 0.06);
		t.cardStroke = Rgba(0xffffff, 0.11);
		t.pill = Rgba(0xffffff, 0.10);
		t.divider = Rgba(0xffffff, 0.11);
		t.sub = Rgba(0xe4d8ff, 0.64);
		t.faint = Rgba(0xe4d8ff, 0.38);
		t.tint = Rgba(0xbb9bff, 0.16);
		t.inverse = Rgb(0xffffff);
		t.onInverse = Rgb(0x170f2b);
		t.solid = Rgb(0x150e26);
		t.soft[0] = Rgba(0xff5fc4, 0.24);
		t.soft[1] = Rgba(0x9a6bff, 0.22);
		t.soft[2] = Rgba(0x44c6ff, 0.16);
		t.softStroke = Rgba(0xffffff, 0.16);
		t.glow[0] = Rgba(0xa05aff, 0.44);
		t.glow[1] = Rgba(0xff54b4, 0.27);
		t.glow[2] = Rgba(0x3caaff, 0.22);
		t.chat = Rgb(0x170f2e);
	} else {
		t.window = Rgb(0xfbf9ff);
		t.over = Rgb(0xf2eefd);
		t.ripple = Rgb(0xe6e0f9);
		t.strip = Rgb(0xf4f0fd);
		t.title = Rgb(0xf4f0fd);
		t.menu = Rgb(0xffffff);
		t.menuOver = Rgb(0xf2eefd);
		t.dialogsOver = Rgb(0xf4f0fd);
		t.sideBar = Rgb(0xf4f0fd);
		t.line = Rgba(0x6e50c8, 0.15);
		t.border = Rgb(0xddd5f3);
		t.fg = Rgb(0x1a1230);
		t.fg2 = Rgb(0x776d95);
		t.fg3 = Rgb(0xb1aac4);
		t.accent = Rgb(0x7a4dff);
		t.onlineText = Rgb(0x17b877);
		t.fill = Rgb(0x7a4dff);
		t.fillOver = Rgb(0x8659ff);
		t.fillRipple = Rgb(0x6a3df0);
		t.sel = Rgb(0xe9defc);
		t.selRipple = Rgb(0xddd0f8);
		t.selFg2 = Rgb(0x3a2f5c);
		t.selDraft = Rgb(0xd63a5a);
		t.badge = Rgb(0xb25cf0);
		t.badgeMuted = Rgb(0xb9b0d4);
		t.tick = Rgb(0x3aa9e8);
		t.green = Rgb(0x17b877);
		t.bubIn = Rgb(0xebe5fb);
		t.bubInShadow = Rgba(0x6e50c8, 0.10);
		t.replyIn = Rgb(0xe64aa8);
		t.waveInOff = Rgb(0xb1aac4);
		t.ground = Rgb(0xf4f0fd);
		t.card = Rgba(0xffffff, 0.74);
		t.cardStroke = Rgba(0x6e50c8, 0.15);
		t.pill = Rgba(0x7a5adc, 0.10);
		t.divider = Rgba(0x6e50c8, 0.15);
		t.sub = Rgba(0x2d1e5a, 0.64);
		t.faint = Rgba(0x2d1e5a, 0.36);
		t.tint = Rgba(0x7a4dff, 0.11);
		t.inverse = Rgb(0x1a1230);
		t.onInverse = Rgb(0xffffff);
		t.solid = Rgb(0xffffff);
		t.soft[0] = Rgba(0xff5fc4, 0.20);
		t.soft[1] = Rgba(0x9a6bff, 0.18);
		t.soft[2] = Rgba(0x44c6ff, 0.16);
		t.softStroke = Rgba(0x7a4dff, 0.20);
		t.glow[0] = Rgba(0xaa78ff, 0.42);
		t.glow[1] = Rgba(0xff78c8, 0.30);
		t.glow[2] = Rgba(0x5abeff, 0.30);
		t.chat = Rgb(0xefe6fc);
	}
	t.raised = t.over;
	t.dialogs = t.window;
	t.accentIcon = t.accent;
	t.service = t.accent;
	t.onFill = Rgb(0xffffff);
	t.selFg = t.fg;
	t.selBadge = t.badge;
	t.onSelBadge = Rgb(0xffffff);
	t.selBadgeMuted = t.badgeMuted;
	t.selOnline = t.green;
	t.onBadge = Rgb(0xffffff);
	t.bubInSel = Mix(t.bubIn, t.fill, dark ? 0.40 : 0.25);
	t.inFg2 = t.fg2;
	t.inAccent = t.accent;
	t.inLink = t.accent;
	// --out is a gradient from #8d5cff to #5a7dff. This is its middle,
	// a little deeper for the white text to be read well.
	t.bubOut = Rgb(0x705cf5);
	t.bubOutSel = Mix(t.bubOut, Rgb(0xffffff), 0.18);
	t.bubOutShadow = QColor(0, 0, 0, 0);
	t.outFg = Rgb(0xffffff);
	t.outFg2 = Rgb(0xdcd7ff);
	t.outAccent = Rgb(0xffffff);
	t.outLink = Rgb(0xcfeaff);
	t.outTick = Rgb(0xe6e1ff);
	t.fileIn = t.fill;
	t.fileInOver = t.fillOver;
	t.fileInIcon = Rgb(0xffffff);
	t.fileOut = Rgb(0xffffff);
	t.fileOutOver = Rgb(0xf0edff);
	t.fileOutIcon = t.bubOut;
	t.waveIn = t.accent;
	t.waveOut = Rgb(0xffffff);
	t.waveOutOff = Mix(t.bubOut, Rgb(0xffffff), 0.45);
	t.userpicFg = Rgb(0xffffff);

	t.cardRaised = t.card;
	t.selected = t.soft[1];
	t.highlight = Rgb(0x9a6bff);
	t.onHighlight = Rgb(0xffffff);
	t.room = t.accent;
	t.roomTint = t.pill;
	t.roomFill = Rgb(0x9a6bff);
	t.onRoom = Rgb(0xffffff);
	t.gradient[0] = Rgb(0xff5fc4);
	t.gradient[1] = Rgb(0x9a6bff);
	t.gradient[2] = Rgb(0x44c6ff);
	t.shape = {
		.card = 22,
		.tile = 19,
		.row = 20,
		.bubble = 20,
		.strokes = true,
		.gradients = true,
		.caps = true,
		.background = true,
	};
	return t;
}

// 3. «Тишина»: body.d3 and body.d3.light.
[[nodiscard]] Tokens SilenceTokens(bool dark) {
	auto t = Tokens();
	const auto lime = Rgb(0xd6ff57);
	const auto onLime = Rgb(0x12160a);
	if (dark) {
		t.window = Rgb(0x0d0d0e);
		t.over = Rgb(0x171718);
		t.ripple = Rgb(0x212123);
		t.strip = Rgb(0x131314);
		t.menu = Rgb(0x171718);
		t.menuOver = Rgb(0x212123);
		t.dialogsOver = Rgb(0x131314);
		t.line = Rgb(0x262628);
		t.fg = Rgb(0xf3f2ee);
		t.fg2 = Rgb(0x93928c);
		t.fg3 = Rgb(0x5e5d59);
		t.accent = lime;
		t.accentIcon = lime;
		t.fill = lime;
		t.fillOver = Rgb(0xe0ff7d);
		t.fillRipple = Rgb(0xc2ec3d);
		t.onFill = onLime;
		t.selDraft = Rgb(0xff6b6b);
		t.selOnline = lime;
		t.badgeMuted = Rgb(0x6f6e69);
		t.green = lime;
		t.bubIn = Rgb(0x151516);
		t.bubOut = Rgb(0x212123);
		t.inFg2 = Rgb(0x6f6e69);
		t.inLink = lime;
		t.outLink = lime;
		t.userpicFg = Rgb(0xf3f2ee);
		t.userpicSaturation = 0.22;
		t.userpicLightness = 0.34;
		t.tint = Rgba(0xd6ff57, 0.12);
	} else {
		t.window = Rgb(0xf5f4f0);
		t.over = Rgb(0xeceae4);
		t.ripple = Rgb(0xe3e1da);
		t.strip = Rgb(0xeeede8);
		t.menu = Rgb(0xfbfaf7);
		t.menuOver = Rgb(0xeceae4);
		t.dialogsOver = Rgb(0xf0eee9);
		t.line = Rgb(0xdcdad2);
		t.fg = Rgb(0x131312);
		t.fg2 = Rgb(0x76746d);
		t.fg3 = Rgb(0xaca9a0);
		// --accent is the text colour here, links would not be seen in a
		// message: a dark shade of the lime is used for them.
		t.accent = Rgb(0x4f6a00);
		t.accentIcon = Rgb(0x131312);
		t.fill = Rgb(0x131312);
		t.fillOver = Rgb(0x2a2a28);
		t.fillRipple = Rgb(0x3a3936);
		t.onFill = Rgb(0xf5f4f0);
		t.selDraft = Rgb(0xc0392b);
		t.selOnline = Rgb(0x3aa655);
		t.badgeMuted = Rgb(0xc9c6bd);
		t.green = Rgb(0x3aa655);
		t.bubIn = Rgb(0xfcfbf8);
		t.bubOut = Rgb(0xe3e1da);
		t.inFg2 = Rgb(0x9a978f);
		t.inLink = Rgb(0x4f6a00);
		t.outLink = Rgb(0x4f6a00);
		t.userpicFg = Rgb(0x131312);
		t.userpicSaturation = 0.26;
		t.userpicLightness = 0.70;
		t.tint = Rgba(0x131312, 0.07);
	}
	t.raised = t.over;
	t.title = t.window;
	t.dialogs = t.window;
	t.sideBar = t.window;
	t.border = t.line;
	t.service = t.fg;
	t.onlineText = t.fg;
	t.sel = t.over;
	t.selRipple = t.ripple;
	t.selFg = t.fg;
	t.selFg2 = t.fg2;
	t.selBadge = lime;
	t.onSelBadge = onLime;
	t.selBadgeMuted = t.badgeMuted;
	t.badge = lime;
	t.onBadge = onLime;
	t.tick = t.fg2;
	t.bubInSel = Mix(t.bubIn, lime, dark ? 0.14 : 0.30);
	// The mockup has outlined bubbles, the palette has only the line
	// under a bubble for that.
	t.bubInShadow = t.line;
	t.inAccent = t.fg;
	t.replyIn = t.fg;
	t.bubOutSel = Mix(t.bubOut, lime, dark ? 0.14 : 0.30);
	t.bubOutShadow = QColor(0, 0, 0, 0);
	t.outFg = t.fg;
	t.outFg2 = t.fg2;
	t.outAccent = t.fg;
	t.outTick = t.fg2;
	t.fileIn = t.fg;
	t.fileInOver = dark ? Rgb(0xffffff) : Rgb(0x2a2a28);
	t.fileInIcon = t.window;
	t.fileOut = t.fg;
	t.fileOutOver = t.fileInOver;
	t.fileOutIcon = t.window;
	t.waveIn = t.fg;
	t.waveInOff = t.fg3;
	t.waveOut = t.fg;
	t.waveOutOff = t.fg3;

	t.ground = t.window;
	t.card = QColor(0, 0, 0, 0);
	t.cardRaised = QColor(0, 0, 0, 0);
	t.cardStroke = QColor(0, 0, 0, 0);
	t.pill = QColor(0, 0, 0, 0);
	t.divider = t.line;
	t.sub = t.fg2;
	t.faint = t.fg3;
	t.selected = t.over;
	t.highlight = lime;
	t.onHighlight = onLime;
	t.room = t.fg;
	t.roomTint = QColor(0, 0, 0, 0);
	t.roomFill = lime;
	t.onRoom = onLime;
	t.inverse = t.fg;
	t.onInverse = t.window;
	t.solid = t.window;
	FillNoGradients(t);
	t.chat = t.window;
	t.shape = {
		.bubble = 15,
		.avatarPercent = 30,
		.cards = false,
		.caps = true,
		.background = true,
	};
	return t;
}

[[nodiscard]] Tokens TokensFor(int look, bool dark) {
	switch (look) {
	case kNative: return NativeTokens(dark);
	case kNightAir: return NightAirTokens(dark);
	case kSilence: return SilenceTokens(dark);
	}
	Unexpected("Look in TokensFor.");
}

struct KeyColor {
	const char *key = nullptr;
	QColor color;
};

// The palette keys a look sets. A key that is not here gets the colour
// of the key it falls back to in colors.palette when that one is here
// (boxBg follows windowBg, like in a theme file), see Derive(). All the
// other keys keep the colours of the theme under the look.
[[nodiscard]] std::vector<KeyColor> PaletteSpec(const Tokens &t) {
	auto list = std::vector<KeyColor>();
	list.reserve(256);
	const auto set = [&](const char *key, const QColor &color) {
		list.push_back({ key, color });
	};

	// Window, boxes, text.
	set("windowBg", t.window);
	set("windowFg", t.fg);
	set("windowBgOver", t.over);
	set("windowBgRipple", t.ripple);
	set("windowFgOver", t.fg);
	set("windowSubTextFg", t.fg2);
	set("windowSubTextFgOver", t.fg2);
	set("windowBoldFg", t.fg);
	set("windowBoldFgOver", t.fg);
	set("windowBgActive", t.fill);
	set("windowFgActive", t.onFill);
	set("windowActiveTextFg", t.accent);
	set("shadowFg", t.line);
	set("boxTitleFg", t.fg);
	set("boxTitleAdditionalFg", t.fg2);
	set("boxTextFgGood", t.green);
	set("boxDividerBg", t.strip);
	set("contactsStatusFgOnline", t.onlineText);
	set("profileStatusFgOver", t.fg2);

	// Buttons, inputs, sliders.
	set("activeButtonBg", t.fill);
	set("activeButtonBgOver", t.fillOver);
	set("activeButtonBgRipple", t.fillRipple);
	set("activeButtonSecondaryFg", Mix(t.fill, t.onFill, 0.7));
	set("activeLineFg", t.accentIcon);
	set("lightButtonBgOver", Over(t.window, WithAlpha(t.accent, 0.10)));
	set("lightButtonBgRipple", Over(t.window, WithAlpha(t.accent, 0.18)));
	set("placeholderFg", t.fg3);
	set("placeholderFgActive", Mix(t.fg3, t.window, 0.35));
	set("inputBorderFg", t.border);
	set("filterInputBorderFg", t.accent);
	set("filterInputInactiveBg", t.raised);
	set("checkboxFg", Mix(t.fg3, t.fg2, 0.5));
	set("sliderBgInactive", Mix(t.window, t.fg, 0.16));
	set("tooltipBg", t.raised);
	set("tooltipFg", t.fg);
	set("tooltipBorderFg", t.border);
	set("smallCloseIconFg", t.fg3);
	set("smallCloseIconFgOver", t.fg2);
	set("overviewCheckFgActive", t.onFill);
	set("overviewPhotoSelectOverlay", WithAlpha(t.fill, 0.2));
	set("mediaPlayerDisabledFg", Mix(t.window, t.fill, 0.5));

	// Menus.
	set("menuBg", t.menu);
	set("menuBgOver", t.menuOver);
	set("menuIconFg", t.fg2);
	set("menuIconFgOver", Mix(t.fg2, t.fg, 0.5));
	set("menuSubmenuArrowFg", t.fg2);
	set("menuFgDisabled", t.fg3);
	set("menuSeparatorFg", t.border);

	// Scroll bars.
	set("scrollBarBg", WithAlpha(t.fg, 0.33));
	set("scrollBarBgOver", WithAlpha(t.fg, 0.48));
	set("scrollBg", WithAlpha(t.fg, 0.10));
	set("scrollBgOver", WithAlpha(t.fg, 0.17));

	// The title bar.
	set("titleBg", t.title);
	set("titleButtonFg", t.fg3);
	set("titleButtonBgOver", t.over);
	set("titleButtonFgOver", t.fg);
	set("titleFg", t.fg3);
	set("titleFgActive", t.fg2);

	// The chats list.
	set("dialogsBg", t.dialogs);
	set("dialogsBgOver", t.dialogsOver);
	set("dialogsTextFgService", t.service);
	set("dialogsSendingIconFg", t.fg3);
	set("dialogsSentIconFg", t.tick);
	set("dialogsSentIconFgOver", t.tick);
	set("dialogsUnreadBg", t.badge);
	set("dialogsUnreadBgMuted", t.badgeMuted);
	set("dialogsUnreadFg", t.onBadge);
	set("dialogsOnlineBadgeFg", t.green);
	set("dialogsMentionIconFg", t.badge);
	set("dialogsBgActive", t.sel);
	set("dialogsNameFgActive", t.selFg);
	set("dialogsDateFgActive", t.selFg2);
	set("dialogsTextFgActive", t.selFg2);
	set("dialogsDraftFgActive", t.selDraft);
	set("dialogsSendingIconFgActive", WithAlpha(t.selFg2, 0.6));
	set("dialogsUnreadBgActive", t.selBadge);
	set("dialogsUnreadBgMutedActive", t.selBadgeMuted);
	set("dialogsUnreadFgActive", t.onSelBadge);
	set("dialogsOnlineBadgeFgActive", t.selOnline);
	set("dialogsRippleBgActive", t.selRipple);
	set("searchedBarBg", t.over);
	set("sideBarBg", t.sideBar);
	set("sideBarBgActive", t.over);
	set("sideBarBgRipple", t.ripple);
	set("sideBarTextFg", t.fg2);
	set("sideBarTextFgActive", t.accent);
	set("sideBarIconFg", t.fg2);
	set("sideBarIconFgActive", t.accent);
	set("sideBarBadgeBg", t.badge);
	set("sideBarBadgeBgMuted", t.badgeMuted);
	set("sideBarBadgeFg", t.onBadge);
	set("mainMenuCoverBg", t.over);
	set("callBarBg", t.fill);
	set("callBarFg", t.onFill);

	// The chat: bars and panels.
	set("topBarBg", t.window);
	set("historyComposeAreaBg", t.window);
	set("historySendIconFg", t.accentIcon);
	set("historySendIconFgOver", t.accentIcon);
	set("historyReplyIconFg", t.accentIcon);
	set("historyUnreadBarBg", t.over);
	set("historyUnreadBarFg", t.accent);
	set("emojiPanHeaderBg", WithAlpha(t.window, 0.95));
	set("emojiIconFg", Mix(t.fg3, t.fg2, 0.6));
	set("stickerPreviewBg", WithAlpha(t.window, 0.69));

	// The chat: bubbles.
	set("msgInBg", t.bubIn);
	set("msgInBgSelected", t.bubInSel);
	set("msgOutBg", t.bubOut);
	set("msgOutBgSelected", t.bubOutSel);
	set("msgSelectOverlay", WithAlpha(t.fill, 0.30));
	set("msgStickerOverlay", WithAlpha(t.fill, 0.50));
	set("msgInShadow", t.bubInShadow);
	set("msgInShadowSelected", t.bubInShadow);
	set("msgOutShadow", t.bubOutShadow);
	set("msgOutShadowSelected", t.bubOutShadow);
	set("historyTextOutFg", t.outFg);
	set("historyLinkInFg", t.inLink);
	set("historyLinkOutFg", t.outLink);
	set("historyOutIconFg", t.outTick);
	set("historyOutIconFgSelected", t.outTick);
	set("historySendingOutIconFg", t.outFg2);
	set("historySendingInIconFg", t.fg3);
	set("msgInServiceFg", t.inAccent);
	set("msgInServiceFgSelected", t.inAccent);
	set("msgOutServiceFg", t.outAccent);
	set("msgOutServiceFgSelected", t.outAccent);
	set("msgInDateFg", t.inFg2);
	set("msgInDateFgSelected", t.inFg2);
	set("msgOutDateFg", t.outFg2);
	set("msgOutDateFgSelected", t.outFg2);
	set("msgInReplyBarColor", t.replyIn);
	set("msgInReplyBarSelColor", t.replyIn);
	set("msgOutReplyBarColor", t.outAccent);
	set("msgOutReplyBarSelColor", t.outAccent);
	set("msgInMonoFg", Mix(t.fg2, t.inLink, 0.5));
	set("msgOutMonoFg", t.outFg2);
	set("msgFileThumbLinkOutFg", t.outAccent);
	set("msgFileThumbLinkOutFgSelected", t.outAccent);

	// The chat: files and voice messages.
	set("msgFileInBg", t.fileIn);
	set("msgFileInBgOver", t.fileInOver);
	set("msgFileInBgSelected", t.fileIn);
	set("msgFileOutBg", t.fileOut);
	set("msgFileOutBgSelected", t.fileOutOver);
	set("historyFileInIconFg", t.fileInIcon);
	set("historyFileInIconFgSelected", t.fileInIcon);
	set("historyFileOutIconFg", t.fileOutIcon);
	set("historyFileOutIconFgSelected", t.fileOutIcon);
	set("msgWaveformInActive", t.waveIn);
	set("msgWaveformInActiveSelected", t.waveIn);
	set("msgWaveformInInactive", t.waveInOff);
	set("msgWaveformInInactiveSelected", t.waveInOff);
	set("msgWaveformOutActive", t.waveOut);
	set("msgWaveformOutActiveSelected", t.waveOut);
	set("msgWaveformOutInactive", t.waveOutOff);
	set("msgWaveformOutInactiveSelected", t.waveOutOff);

	// Userpics. Look 3 has muted ones, the others keep the colours of
	// the theme and only make sure the letters are white.
	set("historyPeerUserpicFg", t.userpicFg);
	if (t.userpicSaturation >= 0.) {
		// The hues of the eight colours of colors.palette.
		constexpr auto kHues = std::array{
			14., 90., 45., 209., 260., 343., 191., 35.,
		};
		static constexpr auto kKeys = std::array{
			"historyPeer1UserpicBg",
			"historyPeer2UserpicBg",
			"historyPeer3UserpicBg",
			"historyPeer4UserpicBg",
			"historyPeer5UserpicBg",
			"historyPeer6UserpicBg",
			"historyPeer7UserpicBg",
			"historyPeer8UserpicBg",
		};
		for (auto i = 0; i != int(kKeys.size()); ++i) {
			set(kKeys[i], Hsl(
				kHues[i],
				t.userpicSaturation,
				t.userpicLightness));
		}
	}
	return list;
}

// The keys a look never touches, even through a fallback.
[[nodiscard]] const std::vector<const char*> &KeptKeys() {
	static const auto result = std::vector<const char*>{
		// Changed by the theme code itself ("adjusted" in colors.palette,
		// see ChatBackground in window_theme.cpp), the look puts its own
		// values there in a separate way: details::ServiceLayer.
		"msgServiceBg",
		"msgServiceBgSelected",
		"historyScrollBg",
		"historyScrollBgOver",
		"historyScrollBarBg",
		"historyScrollBarBgOver",
		// Painted over photos, wallpapers and dark overlays: they fall
		// back to windowFgActive only because that one is white in a
		// usual theme.
		"radialFg",
		"titleButtonCloseFgOver",
		"stickerPanDeleteFg",
		"historyIconFgInverted",
		"historyForwardChooseFg",
		"msgServiceFg",
		"youtubePlayIconFg",
		"mediaviewMenuFg",
		"mediaviewFileExtFg",
		"historyFileThumbIconFg",
		"historyFileThumbIconFgSelected",
		"overviewCheckBorder",
	};
	return result;
}

struct Derived {
	std::vector<PaletteEntry> entries;
	QStringList errors;
};

[[nodiscard]] int KeyIndex(QLatin1String name) {
	return style::internal::GetPaletteIndex(name);
}

[[nodiscard]] Derived Derive(const std::vector<KeyColor> &spec) {
	auto result = Derived();
	auto values = std::vector<std::optional<QColor>>(style::palette::kCount);
	auto kept = std::vector<bool>(style::palette::kCount, false);
	for (const auto key : KeptKeys()) {
		const auto index = KeyIndex(QLatin1String(key));
		if (index < 0) {
			result.errors.push_back(u"unknown kept key "_q
				+ QString::fromLatin1(key));
		} else {
			kept[index] = true;
		}
	}
	for (const auto &[key, color] : spec) {
		const auto name = QString::fromLatin1(key);
		const auto index = KeyIndex(QLatin1String(key));
		if (index < 0) {
			result.errors.push_back(u"unknown key "_q + name);
		} else if (kept[index]) {
			result.errors.push_back(u"a kept key is set: "_q + name);
		} else if (values[index]) {
			result.errors.push_back(u"a key is set twice: "_q + name);
		} else {
			values[index] = color;
		}
	}
	// A key refers only to the keys above it, so one pass is enough.
	const auto rows = style::main_palette::data();
	for (const auto &row : rows) {
		const auto index = KeyIndex(row.name);
		if (index < 0 || values[index] || kept[index]) {
			continue;
		}
		const auto copy = (row.value.size() > 0)
			&& (row.value.data()[0] != '#');
		const auto parent = copy ? row.value : row.fallback;
		if (parent.size() <= 0) {
			continue;
		}
		const auto from = KeyIndex(parent);
		if (from >= 0 && from < index && values[from]) {
			values[index] = values[from];
		}
	}
	for (auto i = 0; i != style::palette::kCount; ++i) {
		if (values[i]) {
			result.entries.push_back({ i, *values[i] });
		}
	}
	return result;
}

struct Prepared {
	Tokens tokens;
	std::vector<PaletteEntry> palette;
	QImage chat; // The chat background, when it is an image.
	QColor chatAverage;
	bool ready = false;
	bool chatReady = false;
};

[[nodiscard]] Prepared &PreparedFor(int look, bool dark) {
	Expects(look > kPlain && look < kCount);

	static Prepared All[kCount][2];
	auto &result = All[look][dark ? 1 : 0];
	if (!result.ready) {
		result.tokens = TokensFor(look, dark);
		result.palette = Derive(PaletteSpec(result.tokens)).entries;
		result.ready = true;
	}
	return result;
}

struct GroundCache {
	QSize whole;
	int look = 0;
	int scale = 0;
	bool dark = false;
	QImage image;
	uint64 used = 0;
};

struct State {
	details::PaletteLayer layer;
	details::ServiceLayer service;
	const Tokens *tokens = nullptr;
	std::array<QColor, int(Role::kCount)> colors;
	QColor serviceBackground;
	int forced = -1;
	int setting = 0;
	int look = 0; // Of the layer that is on.
	bool dark = false;
	bool background = false;
	bool started = false;

	int publishedLook = 0;
	bool publishedDark = false;
	bool publishedBackground = false;
	rpl::variable<int> shown = 0;
	rpl::event_stream<> updates;

	std::array<GroundCache, kGroundCacheSize> grounds;
	uint64 groundUse = 0;
};

[[nodiscard]] State &Data() {
	static auto result = State();
	return result;
}

[[nodiscard]] QLinearGradient CssGradient(
		const QRectF &rect,
		float64 degrees) {
	// 0deg points up and the angle grows clockwise, the line is as long
	// as the box is along it.
	const auto angle = degrees * kPi / 180.;
	const auto dx = std::sin(angle);
	const auto dy = -std::cos(angle);
	const auto half = (std::abs(rect.width() * dx)
		+ std::abs(rect.height() * dy)) / 2.;
	const auto center = rect.center();
	return QLinearGradient(
		center.x() - dx * half,
		center.y() - dy * half,
		center.x() + dx * half,
		center.y() + dy * half);
}

// radial-gradient(rx ry at center, color, transparent 70%).
void PaintSpot(
		QPainter &p,
		QPointF center,
		float64 rx,
		float64 ry,
		const QColor &color) {
	if (!color.alpha() || rx <= 0. || ry <= 0.) {
		return;
	}
	auto gradient = QRadialGradient(QPointF(), 1.);
	gradient.setColorAt(0., color);
	gradient.setColorAt(0.7, WithAlpha(color, 0.));
	gradient.setColorAt(1., WithAlpha(color, 0.));
	p.save();
	p.translate(center);
	p.scale(rx, ry);
	p.fillRect(QRectF(-1., -1., 2., 2.), gradient);
	p.restore();
}

// The colour spots of .screen in the mockups of look 2, sx and sy turn
// the pixels of the mockup into the pixels of the target.
void PaintGlow(
		QPainter &p,
		const QRectF &whole,
		const Tokens &t,
		float64 sx,
		float64 sy) {
	struct Spot {
		float64 rx = 0.;
		float64 ry = 0.;
		float64 x = 0.;
		float64 y = 0.;
	};
	constexpr auto kSpots = std::array{
		Spot{ 540., 430., 0.10, -0.06 },
		Spot{ 500., 420., 1.06, 0.20 },
		Spot{ 600., 480., 0.58, 1.12 },
	};
	for (auto i = 0; i != int(kSpots.size()); ++i) {
		const auto &spot = kSpots[i];
		PaintSpot(
			p,
			QPointF(
				whole.x() + whole.width() * spot.x,
				whole.y() + whole.height() * spot.y),
			spot.rx * sx,
			spot.ry * sy,
			t.glow[i]);
	}
}

void PrepareChat(Prepared &prepared) {
	if (prepared.chatReady) {
		return;
	}
	prepared.chatReady = true;
	const auto &t = prepared.tokens;
	if (!t.shape.gradients) {
		prepared.chatAverage = t.ground;
		return;
	}
	auto image = QImage(kChatImage, QImage::Format_ARGB32_Premultiplied);
	image.fill(t.ground);
	{
		auto p = QPainter(&image);
		auto hq = PainterHighQualityEnabler(p);
		PaintGlow(
			p,
			QRectF(QPointF(), QSizeF(kChatImage)),
			t,
			kChatImage.width() / float64(kChatReference.width()),
			kChatImage.height() / float64(kChatReference.height()));
	}
	prepared.chatAverage = Ui::CountAverageColor(image);
	prepared.chat = Images::DitherImage(image);
	if (prepared.chat.format() != QImage::Format_ARGB32_Premultiplied) {
		prepared.chat = std::move(prepared.chat).convertToFormat(
			QImage::Format_ARGB32_Premultiplied);
	}
}

[[nodiscard]] bool WantsBackground(int look) {
	const auto &data = Data();
	if (data.forced >= 0
		|| !data.started
		|| !PreparedFor(look, false).tokens.shape.background) {
		return false;
	}
	// Only while nothing was chosen by the user: one of the themes that
	// come with the app and its own background.
	const auto &object = Window::Theme::Background()->themeObject();
	return !object.cloud.id
		&& Window::Theme::IsEmbeddedTheme(object.pathAbsolute)
		&& !Window::Theme::IsNonDefaultBackground();
}

[[nodiscard]] int Wanted() {
	const auto &data = Data();
	if (data.forced >= 0) {
		return data.forced;
	} else if (!data.started) {
		return kPlain;
	} else if (Window::Theme::Background()->editingTheme().has_value()) {
		// The editor shows and saves the colours of the theme.
		return kPlain;
	}
	return std::clamp(data.setting, 0, kCount - 1);
}

void FillColors(State &data) {
	const auto &t = *data.tokens;
	const auto set = [&](Role role, const QColor &color) {
		data.colors[int(role)] = color;
	};
	set(Role::Ground, t.ground);
	set(Role::Card, t.card);
	set(Role::CardRaised, t.cardRaised);
	set(Role::CardStroke, t.cardStroke);
	set(Role::Pill, t.pill);
	set(Role::Divider, t.divider);
	set(Role::Text, t.fg);
	set(Role::SubText, t.sub);
	set(Role::FaintText, t.faint);
	set(Role::Accent, t.accent);
	set(Role::AccentFill, t.fill);
	set(Role::OnAccent, t.onFill);
	set(Role::Tint, t.tint);
	set(Role::Selected, t.selected);
	set(Role::Online, t.green);
	set(Role::Highlight, t.highlight);
	set(Role::OnHighlight, t.onHighlight);
	set(Role::Room, t.room);
	set(Role::RoomTint, t.roomTint);
	set(Role::RoomFill, t.roomFill);
	set(Role::OnRoom, t.onRoom);
	set(Role::Inverse, t.inverse);
	set(Role::OnInverse, t.onInverse);
}

void TakeOffNow(State &data) {
	if (data.layer.on()) {
		if (data.layer.intact()) {
			data.service.takeOff();
			data.layer.takeOff();
		} else {
			// The whole palette was replaced under the layer, what it
			// remembers belongs to the palette that is gone.
			data.service.forget();
			data.layer.forget();
		}
	}
	data.look = kPlain;
	data.background = false;
	data.tokens = nullptr;
}

// Makes the main palette show the wanted look. True: colours changed.
[[nodiscard]] bool Apply() {
	auto &data = Data();
	const auto look = Wanted();
	const auto background = (look != kPlain) && WantsBackground(look);
	auto changed = false;
	if (data.layer.on()) {
		if (data.layer.intact()
			&& look == data.look
			&& background == data.background) {
			// The colours that follow the wallpaper may have been set
			// again by the theme code.
			if (data.service.on()
				&& data.service.refresh(data.serviceBackground)) {
				style::internal::ResetIcons();
				return true;
			}
			return false;
		}
		TakeOffNow(data);
		changed = true;
	}
	if (look != kPlain) {
		const auto palette = style::main_palette::get();
		// The layer is off: this is the colour of the theme.
		const auto dark = (palette->dialogsBg()->c.valueF()
			< kDarkThreshold);
		auto &prepared = PreparedFor(look, dark);
		data.layer.putOn(*palette, prepared.palette);
		if (background) {
			PrepareChat(prepared);
			data.serviceBackground = prepared.chatAverage;
			data.service.putOn(*palette, data.serviceBackground);
		}
		data.look = look;
		data.dark = dark;
		data.background = background;
		data.tokens = &prepared.tokens;
		FillColors(data);
		changed = true;
	}
	if (changed) {
		style::internal::ResetIcons();
	}
	return changed;
}

void Publish() {
	auto &data = Data();
	const auto same = (data.publishedLook == data.look)
		&& (data.publishedDark == data.dark || data.look == kPlain)
		&& (data.publishedBackground == data.background);
	data.publishedLook = data.look;
	data.publishedDark = data.dark;
	data.publishedBackground = data.background;
	data.shown = data.look;
	if (!same) {
		data.updates.fire({});
	}
}

void Sync() {
	if (Apply()) {
		style::NotifyPaletteChanged();
	}
	Publish();
}

[[nodiscard]] QColor PlainColor(Role role) {
	switch (role) {
	case Role::Ground: return st::boxDividerBg->c;
	case Role::Card: return st::windowBg->c;
	case Role::CardRaised: return st::windowBgOver->c;
	case Role::CardStroke: return QColor(0, 0, 0, 0);
	case Role::Pill: return st::windowBgRipple->c;
	case Role::Divider: return st::shadowFg->c;
	case Role::Text: return st::windowFg->c;
	case Role::SubText: return st::windowSubTextFg->c;
	case Role::FaintText: return st::placeholderFg->c;
	case Role::Accent: return st::windowActiveTextFg->c;
	case Role::AccentFill: return st::activeButtonBg->c;
	case Role::OnAccent: return st::activeButtonFg->c;
	case Role::Tint: return WithAlpha(st::windowActiveTextFg->c, 0.14);
	case Role::Selected: return st::windowBgOver->c;
	case Role::Online: return st::dialogsOnlineBadgeFg->c;
	case Role::Highlight: return st::activeButtonBg->c;
	case Role::OnHighlight: return st::activeButtonFg->c;
	case Role::Room: return st::windowActiveTextFg->c;
	case Role::RoomTint: return WithAlpha(st::windowActiveTextFg->c, 0.14);
	case Role::RoomFill: return st::activeButtonBg->c;
	case Role::OnRoom: return st::activeButtonFg->c;
	case Role::Inverse: return st::windowFg->c;
	case Role::OnInverse: return st::windowBg->c;
	case Role::kCount: break;
	}
	Unexpected("Role in PlainColor.");
}

[[nodiscard]] int Scaled(int value) {
	return style::ConvertScale(value);
}

[[nodiscard]] qreal Clamped(qreal radius, const QRectF &rect) {
	return std::max(
		std::min({ radius, rect.width() / 2., rect.height() / 2. }),
		0.);
}

void FillRounded(
		QPainter &p,
		const QRectF &rect,
		qreal radius,
		const QBrush &brush) {
	p.setPen(Qt::NoPen);
	p.setBrush(brush);
	const auto r = Clamped(radius, rect);
	p.drawRoundedRect(rect, r, r);
}

// A one pixel stroke inside the rect.
void StrokeRounded(
		QPainter &p,
		const QRectF &rect,
		qreal radius,
		const QColor &color) {
	if (!color.alpha()) {
		return;
	}
	const auto width = style::ConvertScaleExact(1.);
	const auto half = width / 2.;
	const auto inner = rect.marginsRemoved({ half, half, half, half });
	const auto r = Clamped(radius - half, inner);
	p.setPen(QPen(color, width));
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(inner, r, r);
}

void PaintHairline(QPainter &p, const QRectF &rect, const QColor &color) {
	const auto line = std::max(style::ConvertScaleExact(1.), 1.);
	p.fillRect(
		QRectF(rect.x(), rect.y() + rect.height() - line, rect.width(), line),
		color);
}

[[nodiscard]] QBrush SoftBrush(const Tokens &t, const QRectF &rect) {
	auto gradient = CssGradient(rect, 110.);
	gradient.setColorAt(0., t.soft[0]);
	gradient.setColorAt(0.55, t.soft[1]);
	gradient.setColorAt(1., t.soft[2]);
	return QBrush(gradient);
}

[[nodiscard]] QImage GroundImage(State &data, QSize whole) {
	const auto scale = style::Scale();
	auto oldest = &data.grounds.front();
	for (auto &cache : data.grounds) {
		if (cache.whole == whole
			&& cache.look == data.look
			&& cache.dark == data.dark
			&& cache.scale == scale
			&& !cache.image.isNull()) {
			cache.used = ++data.groundUse;
			return cache.image;
		} else if (cache.used < oldest->used) {
			oldest = &cache;
		}
	}
	const auto &t = *data.tokens;
	const auto size = QSize(
		std::max(whole.width() / kGroundScale, 1),
		std::max(whole.height() / kGroundScale, 1));
	auto image = QImage(size, QImage::Format_ARGB32_Premultiplied);
	image.fill(t.ground);
	{
		auto p = QPainter(&image);
		auto hq = PainterHighQualityEnabler(p);
		const auto unit = style::ConvertScaleExact(1.) / kGroundScale;
		PaintGlow(p, QRectF(QPointF(), QSizeF(size)), t, unit, unit);
	}
	*oldest = GroundCache{
		.whole = whole,
		.look = data.look,
		.scale = scale,
		.dark = data.dark,
		.image = image,
		.used = ++data.groundUse,
	};
	return image;
}

[[nodiscard]] float64 Luminance(const QColor &color) {
	const auto channel = [](int value) {
		const auto v = value / 255.;
		return (v <= 0.03928)
			? (v / 12.92)
			: std::pow((v + 0.055) / 1.055, 2.4);
	};
	return 0.2126 * channel(color.red())
		+ 0.7152 * channel(color.green())
		+ 0.0722 * channel(color.blue());
}

[[nodiscard]] float64 Contrast(const QColor &a, const QColor &b) {
	const auto la = Luminance(a);
	const auto lb = Luminance(b);
	return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

} // namespace

namespace details {

bool PaletteLayer::intact() const {
	for (const auto &item : _list) {
		if (!Same(item.color->c, item.value)) {
			return false;
		}
	}
	return true;
}

void PaletteLayer::putOn(
		const style::palette &palette,
		const std::vector<PaletteEntry> &entries) {
	Expects(!_on);

	_list.clear();
	_list.reserve(entries.size());
	for (const auto &entry : entries) {
		const auto color = palette.colorAtIndex(entry.index);
		_list.push_back({ color, entry.value, color->c });
		Put(color, entry.value);
	}
	_on = true;
}

void PaletteLayer::takeOff() {
	for (const auto &item : _list) {
		Put(item.color, item.original);
	}
	forget();
}

void PaletteLayer::forget() {
	_list.clear();
	_on = false;
}

std::optional<QColor> PaletteLayer::original(
		const style::color &color) const {
	for (const auto &item : _list) {
		if (item.color.get() == color.get()) {
			return item.original;
		}
	}
	return std::nullopt;
}

void ServiceLayer::putOn(
		const style::palette &palette,
		const QColor &background) {
	Expects(_list.empty());

	// The same six as in ChatBackground::ChatBackground().
	const auto colors = std::array{
		palette.msgServiceBg(),
		palette.msgServiceBgSelected(),
		palette.historyScrollBg(),
		palette.historyScrollBgOver(),
		palette.historyScrollBarBg(),
		palette.historyScrollBarBgOver(),
	};
	_list.reserve(colors.size());
	for (const auto &color : colors) {
		// What the theme code does with them for a wallpaper of this
		// colour: the hue of the background, their own lightness.
		const auto original = color->c;
		const auto adjusted = Ui::ThemeAdjustedColor(original, background);
		const auto value = QColor(
			adjusted.red(),
			adjusted.green(),
			adjusted.blue(),
			adjusted.alpha());
		_list.push_back({ color, value, original });
		Put(color, value);
	}
}

bool ServiceLayer::refresh(const QColor &background) {
	auto changed = false;
	for (auto &item : _list) {
		const auto current = item.color->c;
		if (Same(current, item.value)) {
			continue;
		}
		const auto adjusted = Ui::ThemeAdjustedColor(current, background);
		item.original = current;
		item.value = QColor(
			adjusted.red(),
			adjusted.green(),
			adjusted.blue(),
			adjusted.alpha());
		Put(item.color, item.value);
		changed = true;
	}
	return changed;
}

void ServiceLayer::takeOff() {
	for (const auto &item : _list) {
		// A colour the theme code has set since belongs to the theme.
		if (Same(item.color->c, item.value)) {
			Put(item.color, item.original);
		}
	}
	forget();
}

void ServiceLayer::forget() {
	_list.clear();
}

std::optional<QColor> ServiceLayer::original(
		const style::color &color) const {
	for (const auto &item : _list) {
		if (item.color.get() == color.get()) {
			return Same(item.color->c, item.value)
				? item.original
				: item.color->c;
		}
	}
	return std::nullopt;
}

} // namespace details

int Current() {
	return Data().look;
}

bool Is(int look) {
	return Data().look == look;
}

rpl::producer<int> Value() {
	return Data().shown.value();
}

rpl::producer<> Updates() {
	return Data().updates.events();
}

bool Dark() {
	const auto &data = Data();
	return (data.look != kPlain) ? data.dark : ThemeDark();
}

int Chosen() {
	return Get().look();
}

rpl::producer<int> ChosenValue() {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		Get().changes()
	) | rpl::map([] {
		return Get().look();
	}) | rpl::distinct_until_changed();
}

void Set(int look) {
	Get().setLook(look);
}

QColor Color(Role role) {
	Expects(role != Role::kCount);

	const auto &data = Data();
	return (data.look != kPlain) ? data.colors[int(role)] : PlainColor(role);
}

QColor Color(Role role, const QColor &plain) {
	Expects(role != Role::kCount);

	const auto &data = Data();
	return (data.look != kPlain) ? data.colors[int(role)] : plain;
}

int CardRadius(int plain) {
	const auto &data = Data();
	return data.tokens ? Scaled(data.tokens->shape.card) : plain;
}

int TileRadius(int plain) {
	const auto &data = Data();
	return data.tokens ? Scaled(data.tokens->shape.tile) : plain;
}

int RowRadius(int plain) {
	const auto &data = Data();
	return data.tokens ? Scaled(data.tokens->shape.row) : plain;
}

int ChipRadius(int height, int plain) {
	const auto &data = Data();
	return !data.tokens
		? plain
		: data.tokens->shape.cards
		? (height / 2)
		: 0;
}

int AvatarRadius(int size) {
	const auto &data = Data();
	const auto percent = data.tokens ? data.tokens->shape.avatarPercent : 50;
	return (size * percent) / 100;
}

bool HasCards() {
	const auto &data = Data();
	return !data.tokens || data.tokens->shape.cards;
}

bool CapsLabels() {
	const auto &data = Data();
	return data.tokens && data.tokens->shape.caps;
}

QGradientStops AccentStops() {
	const auto &data = Data();
	if (!data.tokens) {
		const auto color = st::activeButtonBg->c;
		return { { 0., color }, { 1., color } };
	}
	const auto &t = *data.tokens;
	if (!t.shape.gradients) {
		return { { 0., t.fill }, { 1., t.fill } };
	}
	return {
		{ 0., t.gradient[0] },
		{ 0.55, t.gradient[1] },
		{ 1., t.gradient[2] },
	};
}

QBrush AccentBrush(const QRectF &rect) {
	const auto &data = Data();
	if (!data.tokens) {
		return QBrush(st::activeButtonBg->c);
	} else if (!data.tokens->shape.gradients) {
		return QBrush(data.tokens->fill);
	}
	auto gradient = CssGradient(rect, 120.);
	gradient.setStops(AccentStops());
	return QBrush(gradient);
}

void PaintCard(
		QPainter &p,
		const QRectF &rect,
		const QColor &plainBg,
		int plainRadius,
		Surface surface) {
	const auto &data = Data();
	if (!data.tokens) {
		p.setPen(Qt::NoPen);
		p.setBrush(plainBg);
		p.drawRoundedRect(rect, plainRadius, plainRadius);
		return;
	}
	const auto &t = *data.tokens;
	if (!t.shape.cards) {
		PaintHairline(p, rect, t.divider);
		return;
	}
	auto hq = PainterHighQualityEnabler(p);
	const auto radius = Scaled(t.shape.card);
	FillRounded(
		p,
		rect,
		radius,
		(surface == Surface::Ground) ? t.card : t.cardRaised);
	if (t.shape.strokes) {
		StrokeRounded(p, rect, radius, t.cardStroke);
	}
}

void PaintChip(
		QPainter &p,
		const QRectF &rect,
		const QColor &plainBg,
		int plainRadius,
		Chip chip) {
	const auto &data = Data();
	if (!data.tokens) {
		p.setPen(Qt::NoPen);
		p.setBrush(plainBg);
		p.drawRoundedRect(rect, plainRadius, plainRadius);
		return;
	}
	const auto &t = *data.tokens;
	if (!t.shape.cards) {
		PaintHairline(p, rect, t.divider);
		return;
	}
	auto hq = PainterHighQualityEnabler(p);
	// The big block is a card, everything else is a pill.
	const auto live = (chip == Chip::Live);
	const auto radius = live
		? qreal(Scaled(t.shape.row))
		: (rect.height() / 2.);
	if (t.shape.gradients) {
		if (live) {
			FillRounded(p, rect, radius, SoftBrush(t, rect));
			StrokeRounded(p, rect, radius, t.softStroke);
		} else {
			FillRounded(p, rect, radius, t.pill);
			StrokeRounded(p, rect, radius, t.cardStroke);
		}
		return;
	}
	FillRounded(
		p,
		rect,
		radius,
		((chip == Chip::Room)
			? t.roomTint
			: (chip == Chip::Neutral)
			? t.pill
			: t.tint));
}

QColor ChipText(Chip chip, const QColor &plain) {
	const auto &data = Data();
	if (!data.tokens) {
		return plain;
	}
	const auto &t = *data.tokens;
	if (t.shape.gradients || !t.shape.cards || chip == Chip::Neutral) {
		return t.fg;
	}
	return (chip == Chip::Room) ? t.room : t.accent;
}

QColor ChipLabel(Chip chip, const QColor &plain) {
	const auto &data = Data();
	if (!data.tokens) {
		return plain;
	}
	const auto &t = *data.tokens;
	if (t.shape.gradients || !t.shape.cards || chip == Chip::Neutral) {
		return t.sub;
	}
	return WithAlpha((chip == Chip::Room) ? t.room : t.accent, 0.85);
}

void PaintAccentGradient(
		QPainter &p,
		const QRectF &rect,
		qreal radius,
		const QColor &plain) {
	const auto &data = Data();
	if (!data.tokens) {
		p.setPen(Qt::NoPen);
		p.setBrush(plain);
		p.drawRoundedRect(rect, radius, radius);
		return;
	}
	auto hq = PainterHighQualityEnabler(p);
	FillRounded(p, rect, radius, AccentBrush(rect));
}

void PaintSelected(
		QPainter &p,
		const QRectF &rect,
		const QColor &plainBg,
		int plainRadius) {
	const auto &data = Data();
	if (!data.tokens) {
		p.setPen(Qt::NoPen);
		p.setBrush(plainBg);
		p.drawRoundedRect(rect, plainRadius, plainRadius);
		return;
	}
	const auto &t = *data.tokens;
	if (!t.shape.cards) {
		p.fillRect(rect, t.selected);
		p.fillRect(
			QRectF(rect.x(), rect.y(), Scaled(kAccentBar), rect.height()),
			t.highlight);
		return;
	}
	auto hq = PainterHighQualityEnabler(p);
	const auto radius = Scaled(t.shape.row);
	if (t.shape.gradients) {
		FillRounded(p, rect, radius, SoftBrush(t, rect));
		StrokeRounded(p, rect, radius, t.softStroke);
	} else {
		FillRounded(p, rect, radius, t.selected);
	}
}

void PaintDivider(QPainter &p, const QRectF &rect, const QColor &plain) {
	const auto &data = Data();
	p.fillRect(rect, data.tokens ? data.tokens->divider : plain);
}

bool PaintGround(QPainter &p, const QRect &rect, QSize whole) {
	auto &data = Data();
	if (!data.tokens || rect.isEmpty()) {
		return false;
	}
	const auto &t = *data.tokens;
	if (!t.shape.gradients) {
		p.fillRect(rect, t.ground);
		return true;
	}
	const auto origin = whole.isEmpty() ? rect.topLeft() : QPoint();
	if (whole.isEmpty()) {
		whole = rect.size();
	}
	const auto image = GroundImage(data, whole);
	const auto kx = image.width() / float64(whole.width());
	const auto ky = image.height() / float64(whole.height());
	const auto from = QRectF(
		(rect.x() - origin.x()) * kx,
		(rect.y() - origin.y()) * ky,
		rect.width() * kx,
		rect.height() * ky);
	p.save();
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	p.drawImage(QRectF(rect), image, from);
	p.restore();
	return true;
}

bool PaintCover(QPainter &p, const QRect &rect) {
	const auto &data = Data();
	if (!data.tokens || !data.tokens->shape.gradients || rect.isEmpty()) {
		return false;
	}
	// .d2 .p-cover: a violet gradient, a pink and a blue spot, thin
	// rings in the corner. The same in the day and in the night.
	const auto unit = rect.width() / kCoverReferenceWidth;
	const auto area = QRectF(rect);
	p.save();
	p.setClipRect(rect, Qt::IntersectClip);
	auto hq = PainterHighQualityEnabler(p);
	auto base = CssGradient(area, 120.);
	base.setColorAt(0., Rgb(0x5a2bd6));
	base.setColorAt(0.5, Rgb(0x8f4bff));
	base.setColorAt(1., Rgb(0x3a1c8f));
	p.fillRect(area, base);
	PaintSpot(
		p,
		QPointF(
			area.x() + area.width() * 0.88,
			area.y() - area.height() * 0.12),
		320. * unit,
		230. * unit,
		Rgba(0x44c6ff, 0.9));
	PaintSpot(
		p,
		QPointF(
			area.x() + area.width() * 0.14,
			area.y() + area.height() * 1.12),
		280. * unit,
		210. * unit,
		Rgba(0xff5fc4, 0.95));
	const auto center = QPointF(
		area.x() + area.width() - 125. * unit,
		area.y() + 105. * unit);
	p.setPen(QPen(Rgba(0xffffff, 0.2), std::max(1.2 * unit, 1.)));
	p.setBrush(Qt::NoBrush);
	for (const auto radius : { 60., 105., 150. }) {
		p.drawEllipse(center, radius * unit, radius * unit);
	}
	p.restore();
	return true;
}

int PaintCoverRing(QPainter &p, const QRectF &cover) {
	const auto &data = Data();
	if (!data.tokens || !data.tokens->shape.gradients || cover.isEmpty()) {
		return 0;
	}
	// .d2 .np-art: conic-gradient(from 210deg, g1, g2, g3, g1) and a gap
	// of the surface colour between the ring and the picture.
	const auto &t = *data.tokens;
	const auto width = Scaled(kRingWidth);
	const auto gap = Scaled(kRingGap);
	auto hq = PainterHighQualityEnabler(p);
	// Qt counts the angle from three o'clock against the clock.
	auto gradient = QConicalGradient(cover.center(), 90. - 210.);
	gradient.setColorAt(0., t.gradient[0]);
	gradient.setColorAt(1. / 3., t.gradient[2]);
	gradient.setColorAt(2. / 3., t.gradient[1]);
	gradient.setColorAt(1., t.gradient[0]);
	p.setPen(Qt::NoPen);
	p.setBrush(gradient);
	p.drawEllipse(cover.marginsAdded({
		qreal(width),
		qreal(width),
		qreal(width),
		qreal(width) }));
	p.setBrush(t.solid);
	p.drawEllipse(cover.marginsAdded({
		qreal(gap),
		qreal(gap),
		qreal(gap),
		qreal(gap) }));
	return width;
}

const std::vector<PaletteEntry> &PaletteFor(int look, bool dark) {
	if (look <= kPlain || look >= kCount) {
		static const auto empty = std::vector<PaletteEntry>();
		return empty;
	}
	return PreparedFor(look, dark).palette;
}

QColor ThemeColor(const style::color &color) {
	const auto &data = Data();
	if (const auto original = data.layer.original(color)) {
		return *original;
	} else if (const auto service = data.service.original(color)) {
		return *service;
	}
	return color->c;
}

bool ThemeDark() {
	return ThemeColor(st::dialogsBg).valueF() < kDarkThreshold;
}

PreviewColors PreviewFor(int look, bool dark) {
	auto result = PreviewColors();
	if (look <= kPlain || look >= kCount) {
		// The Telegram theme as it is. Its wallpaper is a picture, the
		// service colour is what the theme code has derived from it.
		const auto window = ThemeColor(st::windowBg);
		const auto service = ThemeColor(st::msgServiceBg);
		const auto accent = ThemeColor(st::windowActiveTextFg);
		result.ground = ThemeColor(st::boxDividerBg);
		result.list = ThemeColor(st::dialogsBg);
		result.chat = Mix(window, service, service.alphaF() * 0.7);
		result.card = ThemeColor(st::windowBgOver);
		result.cardStroke = QColor(0, 0, 0, 0);
		result.selected = ThemeColor(st::dialogsBgActive);
		result.selectedStroke = QColor(0, 0, 0, 0);
		result.selectedText = ThemeColor(st::dialogsNameFgActive);
		result.selectedSubText = ThemeColor(st::dialogsTextFgActive);
		result.text = ThemeColor(st::dialogsNameFg);
		result.subText = ThemeColor(st::dialogsTextFg);
		result.accent = accent;
		result.badge = ThemeColor(st::dialogsUnreadBg);
		result.bubbleIn = ThemeColor(st::msgInBg);
		result.bubbleInStroke = QColor(0, 0, 0, 0);
		result.bubbleInText = ThemeColor(st::historyTextInFg);
		result.bubbleOut = ThemeColor(st::msgOutBg);
		result.bubbleOutTo = result.bubbleOut;
		result.bubbleOutText = ThemeColor(st::historyTextOutFg);
		result.avatars[0] = ThemeColor(st::historyPeer1UserpicBg);
		result.avatars[1] = ThemeColor(st::historyPeer4UserpicBg);
		result.avatars[2] = ThemeColor(st::historyPeer2UserpicBg);
		result.bar = QColor(0, 0, 0, 0);
		result.chip = WithAlpha(accent, 0.14);
		result.chipStroke = QColor(0, 0, 0, 0);
		result.chipText = accent;
		result.divider = ThemeColor(st::shadowFg);
		for (auto i = 0; i != 3; ++i) {
			result.glow[i] = QColor(0, 0, 0, 0);
			result.gradient[i] = ThemeColor(st::activeButtonBg);
		}
		result.bubbleRadius = 16;
		return result;
	}
	const auto &t = PreparedFor(look, dark).tokens;
	result.ground = t.ground;
	result.list = t.shape.gradients ? QColor(0, 0, 0, 0) : t.dialogs;
	result.chat = t.chat;
	result.card = t.card;
	result.cardStroke = t.cardStroke;
	result.selected = t.shape.gradients ? t.soft[1] : t.sel;
	result.selectedStroke = t.softStroke;
	result.selectedText = t.selFg;
	result.selectedSubText = t.selFg2;
	result.text = t.fg;
	result.subText = t.fg2;
	result.accent = t.accent;
	result.badge = t.badge;
	result.bubbleIn = t.bubIn;
	result.bubbleInStroke = t.shape.cards ? QColor(0, 0, 0, 0) : t.line;
	result.bubbleInText = t.fg;
	// The gradient of ChatBubbles().
	result.bubbleOut = t.shape.gradients ? Rgb(0x8d5cff) : t.bubOut;
	result.bubbleOutTo = t.shape.gradients ? Rgb(0x5a7dff) : t.bubOut;
	result.bubbleOutText = t.outFg;
	if (t.userpicSaturation >= 0.) {
		const auto s = t.userpicSaturation;
		const auto l = t.userpicLightness;
		result.avatars[0] = Hsl(14., s, l);
		result.avatars[1] = Hsl(209., s, l);
		result.avatars[2] = Hsl(90., s, l);
	} else {
		// The first colours of historyPeer1 / 4 / 2 UserpicBg.
		result.avatars[0] = Rgb(0xff845e);
		result.avatars[1] = Rgb(0x5caffa);
		result.avatars[2] = Rgb(0x9ad164);
	}
	result.bar = t.shape.cards ? QColor(0, 0, 0, 0) : t.highlight;
	result.chip = t.shape.gradients
		? t.pill
		: t.shape.cards
		? t.tint
		: QColor(0, 0, 0, 0);
	result.chipStroke = t.shape.gradients ? t.cardStroke : QColor(0, 0, 0, 0);
	result.chipText = t.shape.cards ? t.accent : t.fg;
	result.divider = t.divider;
	for (auto i = 0; i != 3; ++i) {
		result.glow[i] = t.glow[i];
		result.gradient[i] = t.gradient[i];
	}
	result.gradients = t.shape.gradients;
	result.cards = t.shape.cards;
	result.avatarPercent = t.shape.avatarPercent;
	result.bubbleRadius = t.shape.bubble;
	result.rowRadius = t.shape.row;
	return result;
}

void ForceForTests(int look) {
	auto &data = Data();
	const auto forced = (look < 0) ? -1 : std::clamp(look, 0, kCount - 1);
	if (data.forced == forced) {
		return;
	}
	data.forced = forced;
	Sync();
}

void ThemeStarted(rpl::lifetime &lifetime) {
	auto &data = Data();
	data.started = true;
	data.setting = Get().look();

	Get().changes(
	) | rpl::on_next([] {
		auto &data = Data();
		const auto look = Get().look();
		if (data.setting != look) {
			data.setting = look;
			Sync();
		}
	}, lifetime);

	// The themes are gone with the app: the palette is not ours anymore.
	lifetime.add([] {
		auto &data = Data();
		data.started = false;
		data.service.forget();
		data.layer.forget();
		data.look = kPlain;
		data.background = false;
		data.tokens = nullptr;
	});

	Sync();
}

void TakeOffForTheme() {
	auto &data = Data();
	if (!data.layer.on()) {
		return;
	}
	TakeOffNow(data);
	style::internal::ResetIcons();
}

bool PutOnAfterTheme() {
	if (!Data().started) {
		return false;
	}
	const auto changed = Apply();
	Publish();
	return changed;
}

void ThemeEditingChanged() {
	if (Data().started) {
		Sync();
	}
}

bool ChatBackground(Ui::ChatThemeBackground &result) {
	const auto &data = Data();
	if (!data.tokens || !data.background) {
		return false;
	}
	auto &prepared = PreparedFor(data.look, data.dark);
	PrepareChat(prepared);
	result = Ui::ChatThemeBackground();
	if (prepared.chat.isNull()) {
		result.colorForFill = prepared.tokens.ground;
	} else {
		result.gradientForFill = prepared.chat;
	}
	return true;
}

std::vector<QColor> ChatBubbles() {
	const auto &data = Data();
	if (!data.tokens || !data.tokens->shape.gradients) {
		return {};
	}
	// .d2 --out: linear-gradient(135deg, #8d5cff, #5a7dff).
	return { Rgb(0x8d5cff), Rgb(0x5a7dff) };
}

bool RunSelfTest(QStringList &log) {
	auto passed = true;
	const auto check = [&](bool ok, const QString &what) {
		log.push_back((ok ? u"OK: "_q : u"FAILED: "_q) + what);
		passed = passed && ok;
	};
	const auto named = [](int look, bool dark) {
		return u"look %1 %2"_q.arg(look).arg(dark ? u"night"_q : u"day"_q);
	};
	const auto valueOf = [](
			const std::vector<PaletteEntry> &list,
			const char *key) -> std::optional<QColor> {
		const auto index = KeyIndex(QLatin1String(key));
		for (const auto &entry : list) {
			if (entry.index == index) {
				return entry.value;
			}
		}
		return std::nullopt;
	};

	// The sets: every key exists, nothing is set twice, the keys that
	// the theme code changes are left alone, the keys that matter are
	// there, the day and the night sets cover the same keys.
	const auto essential = std::vector<const char*>{
		"windowBg", "windowFg", "windowBgOver", "windowSubTextFg",
		"windowBgActive", "windowFgActive", "windowActiveTextFg",
		"boxBg", "boxTextFg", "boxDividerBg", "shadowFg",
		"dialogsBg", "dialogsNameFg", "dialogsTextFg", "dialogsBgActive",
		"dialogsNameFgActive", "dialogsUnreadBg", "dialogsUnreadFg",
		"topBarBg", "historyComposeAreaBg", "msgInBg", "msgOutBg",
		"historyTextInFg", "historyTextOutFg", "historyLinkInFg",
		"historyLinkOutFg", "msgInDateFg", "msgOutDateFg",
		"activeButtonBg", "activeButtonFg", "lightButtonFg",
		"menuBg", "menuBgOver", "menuIconFg", "titleBg", "titleFgActive",
		"scrollBarBg", "scrollBg", "sideBarBg", "emojiPanBg",
		"mediaPlayerBg", "mainMenuBg", "placeholderFg", "inputBorderFg",
		"msgFileInBg", "msgWaveformInActive", "historyPeerUserpicFg",
	};
	for (auto look = 1; look != kCount; ++look) {
		for (const auto dark : { false, true }) {
			const auto name = named(look, dark);
			const auto tokens = TokensFor(look, dark);
			const auto derived = Derive(PaletteSpec(tokens));
			check(
				derived.errors.isEmpty(),
				name + u": the keys are known and set once"_q
					+ (derived.errors.isEmpty()
						? QString()
						: (u" ("_q + derived.errors.join(u"; "_q) + ')')));
			const auto &list = PaletteFor(look, dark);
			check(
				list.size() >= 250,
				name + u": %1 colours in the set"_q.arg(int(list.size())));
			auto missing = QStringList();
			for (const auto key : essential) {
				if (!valueOf(list, key)) {
					missing.push_back(QString::fromLatin1(key));
				}
			}
			check(
				missing.isEmpty(),
				name + u": the main keys are covered"_q
					+ (missing.isEmpty()
						? QString()
						: (u" (no "_q + missing.join(u", "_q) + ')')));
			auto touched = QStringList();
			for (const auto key : KeptKeys()) {
				if (valueOf(list, key)) {
					touched.push_back(QString::fromLatin1(key));
				}
			}
			check(
				touched.isEmpty(),
				name + u": the kept keys are not in the set"_q
					+ (touched.isEmpty()
						? QString()
						: (u" ("_q + touched.join(u", "_q) + ')')));

			// It can be read: text against what it is painted on.
			const auto pair = [&](const char *fg, const char *bg) {
				const auto a = valueOf(list, fg);
				const auto b = valueOf(list, bg);
				return (a && b) ? Contrast(*a, *b) : 0.;
			};
			const auto window = pair("windowFg", "windowBg");
			const auto in = pair("historyTextInFg", "msgInBg");
			const auto out = pair("historyTextOutFg", "msgOutBg");
			const auto button = pair("activeButtonFg", "activeButtonBg");
			const auto chosen = pair("dialogsNameFgActive", "dialogsBgActive");
			const auto badge = pair("dialogsUnreadFg", "dialogsUnreadBg");
			check(
				window >= 7.
					&& in >= 4.5
					&& out >= 4.5
					&& button >= 2.5
					&& chosen >= 2.5
					&& badge >= 2.5,
				name + u": contrast, window %1, in %2, out %3, button %4,"
					u" chosen row %5, counter %6"_q
					.arg(window, 0, 'f', 1)
					.arg(in, 0, 'f', 1)
					.arg(out, 0, 'f', 1)
					.arg(button, 0, 'f', 1)
					.arg(chosen, 0, 'f', 1)
					.arg(badge, 0, 'f', 1));
			const auto darkWindow = valueOf(list, "windowBg").value_or(
				QColor()).valueF() < kDarkThreshold;
			check(
				darkWindow == dark,
				name + (dark
					? u": the night set is dark"_q
					: u": the day set is light"_q));
		}
		const auto &day = PaletteFor(look, false);
		const auto &night = PaletteFor(look, true);
		auto sameKeys = (day.size() == night.size());
		auto different = 0;
		for (auto i = 0; sameKeys && i != int(day.size()); ++i) {
			sameKeys = (day[i].index == night[i].index);
			if (!Same(day[i].value, night[i].value)) {
				++different;
			}
		}
		check(
			sameKeys,
			u"look %1: the day and the night sets have the same keys"_q
				.arg(look));
		check(
			sameKeys && different * 2 > int(day.size()),
			u"look %1: %2 of %3 colours differ between day and night"_q
				.arg(look)
				.arg(different)
				.arg(int(day.size())));
	}
	for (auto a = 1; a != kCount; ++a) {
		for (auto b = a + 1; b != kCount; ++b) {
			const auto &first = PaletteFor(a, true);
			const auto &second = PaletteFor(b, true);
			auto different = 0;
			const auto count = std::min(first.size(), second.size());
			for (auto i = 0; i != int(count); ++i) {
				if (!Same(first[i].value, second[i].value)) {
					++different;
				}
			}
			check(
				different * 2 > int(count),
				u"looks %1 and %2 are different palettes"_q.arg(a).arg(b));
		}
	}
	check(
		PaletteFor(kPlain, false).empty() && PaletteFor(kPlain, true).empty(),
		u"look 0 has no colours of its own"_q);

	// The layer: the palettes of the two themes that come with the app.
	auto day = style::palette();
	day.finalize();
	auto night = Window::Theme::Instance();
	const auto nightLoaded = Window::Theme::LoadFromFile(
		Window::Theme::NightThemePath(),
		&night,
		nullptr,
		nullptr,
		style::colorizer());
	check(nightLoaded, u"the night theme of the app is loaded"_q);
	if (!nightLoaded) {
		night.palette.finalize();
	}
	const auto dayBytes = day.save();
	const auto nightBytes = night.palette.save();
	check(
		dayBytes != nightBytes || !nightLoaded,
		u"the two themes are different palettes"_q);

	struct Base {
		QString name;
		not_null<style::palette*> palette;
		bool dark = false;
	};
	const auto bases = std::vector<Base>{
		{ u"day"_q, &day, false },
		{ u"night"_q, &night.palette, true },
	};
	for (const auto &base : bases) {
		const auto before = base.palette->save();
		for (auto look = 1; look != kCount; ++look) {
			const auto &list = PaletteFor(look, base.dark);
			auto layer = details::PaletteLayer();
			layer.putOn(*base.palette, list);
			const auto during = base.palette->save();

			// Every colour of the set is there, nothing else has moved.
			auto applied = layer.on() && layer.intact();
			auto expected = before;
			for (const auto &entry : list) {
				const auto now = base.palette->colorAtIndex(entry.index)->c;
				applied = applied && Same(now, entry.value);
				expected[entry.index * 4 + 0] = char(entry.value.red());
				expected[entry.index * 4 + 1] = char(entry.value.green());
				expected[entry.index * 4 + 2] = char(entry.value.blue());
				expected[entry.index * 4 + 3] = char(entry.value.alpha());
			}
			const auto first = list.empty()
				? std::optional<QColor>()
				: layer.original(base.palette->colorAtIndex(list[0].index));
			const auto remembered = !list.empty()
				&& first
				&& (uchar(before[list[0].index * 4 + 0]) == first->red())
				&& (uchar(before[list[0].index * 4 + 1]) == first->green())
				&& (uchar(before[list[0].index * 4 + 2]) == first->blue())
				&& (uchar(before[list[0].index * 4 + 3]) == first->alpha());
			layer.takeOff();
			const auto after = base.palette->save();
			check(
				applied && (during == expected) && (during != before),
				u"%1 theme, look %2: the layer changes its keys only"_q
					.arg(base.name)
					.arg(look));
			check(
				remembered,
				u"%1 theme, look %2: the colour of the theme is known"_q
					.arg(base.name)
					.arg(look));
			check(
				!layer.on() && (after == before),
				u"%1 theme, look %2: taken off, every colour is back"_q
					.arg(base.name)
					.arg(look));
		}

		// One look after another, like a click through the choices.
		auto layer = details::PaletteLayer();
		for (auto look = 1; look != kCount; ++look) {
			layer.putOn(*base.palette, PaletteFor(look, base.dark));
			layer.takeOff();
		}
		check(
			base.palette->save() == before,
			u"%1 theme: three looks in a row leave nothing behind"_q
				.arg(base.name));
	}

	// The theme is replaced while a look is on (the night mode switch):
	// the layer must notice it and must not write the old colours.
	{
		auto palette = style::palette();
		palette = day;
		palette.finalize();
		auto layer = details::PaletteLayer();
		layer.putOn(palette, PaletteFor(kNightAir, false));
		const auto intactBefore = layer.intact();
		palette = night.palette;
		const auto intactAfter = layer.intact();
		layer.forget();
		layer.putOn(palette, PaletteFor(kNightAir, true));
		layer.takeOff();
		check(
			intactBefore && (!intactAfter || !nightLoaded),
			u"a replaced palette is noticed by the layer"_q);
		check(
			palette.save() == nightBytes,
			u"after a theme switch the new theme is restored exactly"_q);
	}

	// The colours that follow the wallpaper.
	{
		auto palette = style::palette();
		palette = day;
		palette.finalize();
		const auto before = palette.save();
		const auto background = Rgb(0x0b0716);
		auto service = details::ServiceLayer();
		service.putOn(palette, background);
		const auto put = (palette.save() != before);
		service.takeOff();
		check(
			put && (palette.save() == before),
			u"the service colours are put on and taken off exactly"_q);

		// The theme code sets one of them while the look is on.
		service.putOn(palette, background);
		const auto theirs = QColor(1, 2, 3, 4);
		Put(palette.msgServiceBg(), theirs);
		const auto refreshed = service.refresh(background);
		const auto adjusted = palette.msgServiceBg()->c;
		Put(palette.msgServiceBg(), theirs);
		service.takeOff();
		auto expected = before;
		const auto index = palette.indexOfColor(palette.msgServiceBg());
		expected[index * 4 + 0] = char(theirs.red());
		expected[index * 4 + 1] = char(theirs.green());
		expected[index * 4 + 2] = char(theirs.blue());
		expected[index * 4 + 3] = char(theirs.alpha());
		check(
			refreshed && (adjusted.alpha() == theirs.alpha()),
			u"a service colour set by the theme code is adjusted again"_q);
		check(
			palette.save() == expected,
			u"a service colour set by the theme code is left to it"_q);
	}

	// Look 0 changes nothing: no layer, the helpers give back the plain.
	{
		const auto &data = Data();
		const auto plain = QColor(12, 34, 56, 78);
		check(
			(data.look != kPlain)
				|| (!data.layer.on()
					&& Same(Color(Role::Card, plain), plain)
					&& Same(ChipText(Chip::Room, plain), plain)
					&& Same(ChipLabel(Chip::Accent, plain), plain)
					&& (CardRadius(7) == 7)
					&& (TileRadius(8) == 8)
					&& (RowRadius(9) == 9)
					&& (ChipRadius(26, 5) == 5)
					&& (AvatarRadius(40) == 20)
					&& HasCards()
					&& !CapsLabels()),
			u"with look 0 the helpers return the plain values"_q);
	}

	// The setting.
	auto &settings = Get();
	const auto was = settings.look();
	if (!Core::Launcher::Instance().customWorkingDir()) {
		log.push_back(u"SKIPPED: the setting is not written without"
			u" a separate -workdir."_q);
		return passed;
	}
	QDir().mkpath(cWorkingDir() + u"tdata"_q);
	auto fired = 0;
	auto lifetime = rpl::lifetime();
	settings.changes() | rpl::on_next([&] { ++fired; }, lifetime);
	const auto other = (was == kNightAir) ? kSilence : kNightAir;
	settings.setLook(other);
	check(
		settings.look() == other && fired == 1,
		u"the setting is changed and announced once"_q);
	settings.setLook(other);
	check(fired == 1, u"the same value is not announced again"_q);
	settings.setLook(kCount + 5);
	check(
		settings.look() == kPlain,
		u"an unknown look becomes the plain one"_q);
	settings.setLook(other);
	const auto saved = QJsonDocument::fromJson(
		settings.syncSnapshot()).object();
	check(
		saved.value(u"look"_q).toInt(-1) == other,
		u"the setting is saved as \"look\""_q);
	auto changed = saved;
	changed.insert(u"look"_q, kNative);
	check(
		settings.syncApply(QJsonDocument(changed).toJson())
			&& settings.look() == kNative,
		u"the setting is read back from the file"_q);
	changed.insert(u"look"_q, 42);
	check(
		settings.syncApply(QJsonDocument(changed).toJson())
			&& settings.look() == kPlain,
		u"a wrong number in the file is the plain look"_q);
	settings.setLook(was);
	check(settings.look() == was, u"the setting is put back"_q);
	return passed;
}

} // namespace Oblivion::Look
