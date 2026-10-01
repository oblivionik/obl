/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_ghost_button.h"

#include "base/unique_qptr.h"
#include "base/weak_ptr.h"
#include "lang/lang_keys.h"
#include "menu/menu_check_item.h"
#include "oblivion/oblivion_settings.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "settings/sections/settings_oblivion.h"
#include "ui/abstract_button.h"
#include "ui/effects/animation_value.h"
#include "ui/effects/animations.h"
#include "ui/painter.h"
#include "ui/qt_object_factory.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/menu/menu.h"
#include "ui/widgets/menu/menu_multiline_action.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/shadow.h"
#include "ui/widgets/tooltip.h"
#include "window/window_session_controller.h"
#include "styles/style_chat.h"
#include "styles/style_dialogs.h"
#include "styles/style_dialogs_widget.h"
#include "styles/style_info.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtGui/QPainterPath>

namespace Oblivion {
namespace {

using Mode = GhostMode::State;
using Flags = GhostPreset;
using Field = bool GhostPreset::*;

constexpr Field kFields[] = {
	&GhostPreset::read,
	&GhostPreset::typing,
	&GhostPreset::online,
	&GhostPreset::stories,
	&GhostPreset::offlineSend,
};
constexpr auto kNone = Flags{ false, false, false, false, false };

constexpr auto kToggleDuration = crl::time(150);

// The glyph is drawn on a 24x24 grid, like the other header icons, and
// takes 14 x 16 in the middle of it, on whole pixels: the icons it stands
// next to (lock, calendar, search) are 13.5-16.5 wide and 12.5-16.5 tall
// with 1.5 strokes, a taller ghost looks oversized beside the lock.
constexpr auto kGlyphSide = 24;
constexpr auto kGlyphLeft = 5.;
constexpr auto kGlyphTop = 4.;
constexpr auto kGlyphRight = 19.;
constexpr auto kGlyphBottom = 20.;
constexpr auto kGlyphStroke = 1.5;
constexpr auto kGlyphFeet = 3;
constexpr auto kGlyphEyesY = 11.;
constexpr auto kGlyphEyeLeft = 9.7;
constexpr auto kGlyphEyeRight = 14.3;
constexpr auto kBezierCircle = 0.5522847498;

// The button is a bit narrower than the lock button it stands next to.
constexpr auto kWidthCut = 4;

// The lock icon has 13.5 empty pixels to the left of it inside its button
// and the ghost has 9 to the right, the overlap leaves 16 between the two.
constexpr auto kLockOverlap = 6;

constexpr auto kSceneWidth = 320;
constexpr auto kSceneMenuSkip = 12;

struct Model {
	Flags preset;
	Flags values = kNone;
	Flags owned = kNone;

	friend inline bool operator==(const Model &, const Model &) = default;
};

[[nodiscard]] Mode ComputeMode(const Model &model) {
	auto total = 0;
	auto enabled = 0;
	for (const auto field : kFields) {
		if (model.preset.*field) {
			++total;
			if (model.values.*field) {
				++enabled;
			}
		}
	}
	return !enabled
		? Mode::Off
		: (enabled == total)
		? Mode::On
		: Mode::Partial;
}

// A toggle is owned only while it is still in the preset and still on:
// one that was switched off elsewhere is not the button's any more.
[[nodiscard]] Flags SyncedOwned(const Model &model) {
	auto result = model.owned;
	for (const auto field : kFields) {
		if (!(model.preset.*field) || !(model.values.*field)) {
			result.*field = false;
		}
	}
	return result;
}

[[nodiscard]] Model Toggled(Model model) {
	if (model.preset.empty()) {
		return model;
	} else if (ComputeMode(model) == Mode::On) {
		const auto restore = ranges::any_of(kFields, [&](Field field) {
			return (model.preset.*field) && (model.owned.*field);
		});
		for (const auto field : kFields) {
			if ((model.preset.*field)
				&& (!restore || (model.owned.*field))) {
				model.values.*field = false;
			}
		}
		model.owned = kNone;
		return model;
	}
	for (const auto field : kFields) {
		if (!(model.preset.*field)) {
			model.owned.*field = false;
		} else if (!(model.values.*field)) {
			model.values.*field = true;
			model.owned.*field = true;
		}
	}
	return model;
}

[[nodiscard]] Model WithPreset(Model model, Flags preset) {
	const auto wasOn = (ComputeMode(model) == Mode::On);
	for (const auto field : kFields) {
		const auto was = model.preset.*field;
		const auto now = preset.*field;
		if (was && !now) {
			if (model.owned.*field) {
				model.values.*field = false;
			}
			model.owned.*field = false;
		} else if (!was && now && wasOn && !(model.values.*field)) {
			model.values.*field = true;
			model.owned.*field = true;
		}
	}
	model.preset = preset;
	return model;
}

[[nodiscard]] Model CurrentModel() {
	const auto &settings = Get();
	auto result = Model{
		.preset = settings.ghostPreset(),
		.values = {
			settings.ghostRead(),
			settings.ghostTyping(),
			settings.ghostOnline(),
			settings.ghostStories(),
			settings.offlineSend(),
		},
		.owned = settings.ghostButtonOwned(),
	};
	result.owned = SyncedOwned(result);
	return result;
}

// Every setter fires Settings::changes(), the remembered toggles must not
// be synced with a half applied state in between.
auto Applying = false;

void SyncOwned() {
	if (!Applying) {
		Get().setGhostButtonOwned(CurrentModel().owned);
	}
}

void Apply(const Model &now) {
	auto &settings = Get();
	Applying = true;
	settings.setGhostPreset(now.preset);
	settings.setGhostRead(now.values.read);
	settings.setGhostTyping(now.values.typing);
	settings.setGhostOnline(now.values.online);
	settings.setGhostStories(now.values.stories);
	settings.setOfflineSend(now.values.offlineSend);
	Applying = false;
	settings.setGhostButtonOwned(now.owned);
}

[[nodiscard]] QPainterPath GhostBodyPath(
		float64 left,
		float64 right,
		float64 top,
		float64 bottom) {
	const auto radius = (right - left) / 2.;
	const auto middle = top + radius;
	const auto control = radius * kBezierCircle;
	const auto foot = radius / kGlyphFeet;
	const auto footControl = foot * kBezierCircle;
	const auto feetTop = bottom - foot;
	auto result = QPainterPath();
	result.moveTo(left, middle);
	result.cubicTo(
		left,
		middle - control,
		left + radius - control,
		top,
		left + radius,
		top);
	result.cubicTo(
		left + radius + control,
		top,
		right,
		middle - control,
		right,
		middle);
	result.lineTo(right, feetTop);

	// The wavy bottom: round feet, drawn from the right to the left.
	for (auto i = 0; i != kGlyphFeet; ++i) {
		const auto from = right - 2 * foot * i;
		const auto center = from - foot;
		const auto till = (i + 1 == kGlyphFeet) ? left : (center - foot);
		result.cubicTo(
			from,
			feetTop + footControl,
			center + footControl,
			bottom,
			center,
			bottom);
		result.cubicTo(
			center - footControl,
			bottom,
			till,
			feetTop + footControl,
			till,
			feetTop);
	}
	result.closeSubpath();
	return result;
}

[[nodiscard]] QPainterPath GhostEyesPath() {
	auto result = QPainterPath();
	result.addEllipse(QPointF(kGlyphEyeLeft, kGlyphEyesY), 1.25, 1.65);
	result.addEllipse(QPointF(kGlyphEyeRight, kGlyphEyesY), 1.25, 1.65);
	return result;
}

// filled = 0: an outlined ghost with eyes, filled = 1: a solid ghost with
// the eyes cut out. Both have the same outer bounds (the outline is the
// solid shape inset by a half of the stroke), so they cross-fade in place.
void PaintGhostGlyph(
		QPainter &p,
		QRectF rect,
		const QColor &color,
		float64 filled) {
	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	p.translate(rect.topLeft());
	p.scale(rect.width() / kGlyphSide, rect.height() / kGlyphSide);
	const auto opacity = p.opacity();
	if (filled < 1.) {
		p.setOpacity(opacity * (1. - filled));
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(
			color,
			kGlyphStroke,
			Qt::SolidLine,
			Qt::RoundCap,
			Qt::RoundJoin));
		const auto inset = kGlyphStroke / 2.;
		p.drawPath(GhostBodyPath(
			kGlyphLeft + inset,
			kGlyphRight - inset,
			kGlyphTop + inset,
			kGlyphBottom - inset));
		p.fillPath(GhostEyesPath(), color);
	}
	if (filled > 0.) {
		p.setOpacity(opacity * filled);
		auto path = GhostBodyPath(
			kGlyphLeft,
			kGlyphRight,
			kGlyphTop,
			kGlyphBottom);
		path.addPath(GhostEyesPath());
		path.setFillRule(Qt::OddEvenFill);
		p.fillPath(path, color);
	}
	p.restore();
}

struct PresetMenuArgs {
	GhostPreset preset;
	Fn<void(GhostPreset)> change;
	Fn<void()> openSettings;
	Fn<void(const QString &)> toast;
};

void FillPresetMenu(
		not_null<Ui::Menu::Menu*> menu,
		PresetMenuArgs &&args) {
	struct Entry {
		Field field = nullptr;
		QString text;
	};
	const auto entries = {
		Entry{
			&GhostPreset::read,
			tr::lng_oblivion_ghost_button_read(tr::now),
		},
		Entry{
			&GhostPreset::typing,
			tr::lng_oblivion_ghost_button_typing(tr::now),
		},
		Entry{
			&GhostPreset::online,
			tr::lng_oblivion_ghost_button_online(tr::now),
		},
		Entry{
			&GhostPreset::stories,
			tr::lng_oblivion_ghost_button_stories(tr::now),
		},
		Entry{
			&GhostPreset::offlineSend,
			tr::lng_oblivion_ghost_button_sending(tr::now),
		},
	};
	const auto preset = menu->lifetime().make_state<GhostPreset>(
		args.preset);
	const auto change = std::move(args.change);
	const auto toast = std::move(args.toast);
	for (const auto &entry : entries) {
		auto item = base::make_unique_q<::Menu::ItemWithCheck>(
			menu,
			menu->st(),
			Ui::CreateChild<QAction>(menu.get()),
			nullptr,
			nullptr);
		item->action()->setText(entry.text);
		item->init((*preset).*(entry.field));

		const auto raw = item.get();
		const auto view = item->checkView();
		const auto field = entry.field;
		view->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			if (((*preset).*field) == checked) {
				return;
			}
			auto next = *preset;
			next.*field = checked;
			if (next.empty()) {
				crl::on_main(raw, [=] {
					view->setChecked(true, anim::type::normal);
				});
				if (toast) {
					toast(tr::lng_oblivion_ghost_button_last(tr::now));
				}
				return;
			}
			*preset = next;
			if (change) {
				change(next);
			}
		}, raw->lifetime());
		menu->addAction(std::move(item));
	}
	if (auto open = std::move(args.openSettings)) {
		menu->addSeparator();
		menu->addAction(
			tr::lng_oblivion_ghost_button_settings(tr::now),
			std::move(open),
			&st::menuIconSettings);
	}
	menu->addSeparator();
	auto about = base::make_unique_q<Ui::Menu::MultilineAction>(
		menu,
		menu->st(),
		st::historyHasCustomEmoji,
		st::historyHasCustomEmojiPosition,
		TextWithEntities{ tr::lng_oblivion_ghost_button_about(tr::now) });
	about->setAttribute(Qt::WA_TransparentForMouseEvents);
	menu->addAction(std::move(about));
}

struct ButtonArgs {
	rpl::producer<Mode> mode;
	Fn<bool()> toggle;
	Fn<PresetMenuArgs()> menu;
};

class Button final
	: public Ui::AbstractButton
	, public Ui::AbstractTooltipShower {
public:
	Button(QWidget *parent, ButtonArgs &&args);

	// Ui::AbstractTooltipShower interface.
	QString tooltipText() const override;
	QPoint tooltipPos() const override;
	bool tooltipWindowActive() const override;

protected:
	void paintEvent(QPaintEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;

private:
	void setMode(Mode mode, anim::type animated);
	void showMenu();

	const Fn<bool()> _toggle;
	const Fn<PresetMenuArgs()> _menuArgs;
	Mode _mode = Mode::Off;
	bool _modeSet = false;
	Ui::Animations::Simple _accentAnimation;
	Ui::Animations::Simple _filledAnimation;
	base::unique_qptr<Ui::PopupMenu> _menu;

};

Button::Button(QWidget *parent, ButtonArgs &&args)
: AbstractButton(parent)
, _toggle(std::move(args.toggle))
, _menuArgs(std::move(args.menu)) {
	resize(
		st::dialogsLock.width - style::ConvertScale(kWidthCut),
		st::dialogsLock.height);
	setAccessibleName(tr::lng_oblivion_ghost_section(tr::now));
	setAcceptBoth();

	std::move(
		args.mode
	) | rpl::on_next([=](Mode mode) {
		setMode(
			mode,
			_modeSet ? anim::type::normal : anim::type::instant);
		_modeSet = true;
	}, lifetime());

	setClickedCallback([=] {
		if (!_toggle || !_toggle()) {
			showMenu();
		} else if (isOver()) {
			// A tooltip shown before the click names the previous state.
			Ui::Tooltip::Show(0, this);
		}
	});
	clicks(
	) | rpl::filter([](Qt::MouseButton button) {
		return (button == Qt::RightButton);
	}) | rpl::on_next([=] {
		showMenu();
	}, lifetime());

	events(
	) | rpl::on_next([=](not_null<QEvent*> e) {
		if (e->type() == QEvent::Enter) {
			Ui::Tooltip::Show(1000, this);
		} else if (e->type() == QEvent::Leave) {
			Ui::Tooltip::Hide();
		}
	}, lifetime());
}

void Button::setMode(Mode mode, anim::type animated) {
	if (_mode == mode && _modeSet) {
		return;
	}
	const auto accent = [](Mode mode) {
		return (mode != Mode::Off) ? 1. : 0.;
	};
	const auto filled = [](Mode mode) {
		return (mode == Mode::On) ? 1. : 0.;
	};
	const auto was = _mode;
	_mode = mode;
	if (animated == anim::type::instant) {
		_accentAnimation.stop();
		_filledAnimation.stop();
	} else {
		const auto callback = [=] { update(); };
		if (accent(was) != accent(mode)) {
			_accentAnimation.start(
				callback,
				_accentAnimation.value(accent(was)),
				accent(mode),
				kToggleDuration);
		}
		if (filled(was) != filled(mode)) {
			_filledAnimation.start(
				callback,
				_filledAnimation.value(filled(was)),
				filled(mode),
				kToggleDuration);
		}
	}
	update();
}

void Button::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);

	const auto accent = _accentAnimation.value(
		(_mode != Mode::Off) ? 1. : 0.);
	const auto filled = _filledAnimation.value(
		(_mode == Mode::On) ? 1. : 0.);
	const auto &normal = (isOver() || isDown())
		? st::dialogsMenuIconFgOver
		: st::dialogsMenuIconFg;
	const auto color = anim::color(normal, st::windowActiveTextFg, accent);
	const auto side = style::ConvertScale(kGlyphSide);
	PaintGhostGlyph(
		p,
		QRectF((width() - side) / 2, (height() - side) / 2, side, side),
		color,
		filled);
}

void Button::onStateChanged(State was, StateChangeSource source) {
	update();
}

void Button::showMenu() {
	if (!_menuArgs) {
		return;
	}
	Ui::Tooltip::Hide();
	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuWithIcons);
	FillPresetMenu(_menu->menu(), _menuArgs());
	_menu->popup(QCursor::pos());
}

QString Button::tooltipText() const {
	const auto title = (_mode == Mode::On)
		? tr::lng_oblivion_ghost_button_on(tr::now)
		: (_mode == Mode::Partial)
		? tr::lng_oblivion_ghost_button_partial(tr::now)
		: tr::lng_oblivion_ghost_button_off(tr::now);
	return _menuArgs
		? (title + '\n' + tr::lng_oblivion_ghost_button_hint(tr::now))
		: title;
}

QPoint Button::tooltipPos() const {
	return QCursor::pos();
}

bool Button::tooltipWindowActive() const {
	return window() && window()->isActiveWindow();
}

// The chats list header for the snapshots: the same controls and the same
// geometry as in Dialogs::Widget, but without a session.
class HeaderSample final : public Ui::RpWidget {
public:
	HeaderSample(QWidget *parent, Mode mode, bool withLock);

protected:
	int resizeGetHeight(int newWidth) override;

private:
	const not_null<Ui::IconButton*> _mainMenu;
	const not_null<Ui::InputField*> _search;
	Ui::IconButton * const _lock;
	const not_null<Button*> _ghost;

};

HeaderSample::HeaderSample(QWidget *parent, Mode mode, bool withLock)
: RpWidget(parent)
, _mainMenu(Ui::CreateChild<Ui::IconButton>(this, st::dialogsMenuToggle))
, _search(Ui::CreateChild<Ui::InputField>(
	this,
	st::dialogsFilter,
	tr::lng_dlg_filter()))
, _lock(withLock
	? Ui::CreateChild<Ui::IconButton>(this, st::dialogsLock)
	: nullptr)
, _ghost(Ui::CreateChild<Button>(
	this,
	ButtonArgs{ .mode = rpl::single(mode) })) {
}

int HeaderSample::resizeGetHeight(int newWidth) {
	const auto padding = st::dialogsFilterPadding;
	const auto left = padding.x() + _mainMenu->width() + padding.x();
	const auto width = newWidth
		- left
		- st::dialogsFilterSkip
		- padding.x();
	_mainMenu->moveToLeft(padding.x(), padding.y(), newWidth);
	_search->setGeometryToLeft(
		left,
		(st::topBarHeight - _search->height()) / 2,
		width,
		_search->height(),
		newWidth);
	auto right = left + width;
	if (_lock) {
		_lock->move(right - _lock->width(), padding.y());
		right -= _lock->width() - GhostButtonLockOverlap();
	}
	_ghost->move(right - _ghost->width(), padding.y());
	return st::topBarHeight;
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	const auto header = [](const QString &name, Mode mode, bool withLock) {
		RegisterScene(
			name,
			QSize(style::ConvertScale(kSceneWidth), 0),
			[=](not_null<Ui::RpWidget*> parent) {
				return Ui::CreateChild<HeaderSample>(
					parent.get(),
					mode,
					withLock);
			});
	};
	header(u"ghost_button_off"_q, Mode::Off, false);
	header(u"ghost_button_partial"_q, Mode::Partial, false);

	// With a local passcode: the button stands to the left of the lock.
	header(u"ghost_button_on"_q, Mode::On, true);

	// The right click menu in the middle of the scene, inside the panel
	// that Ui::PopupMenu paints around it: a popup window itself can't be
	// a part of the scene.
	const auto skip = style::ConvertScale(kSceneMenuSkip);
	const auto outer = st::popupMenuWithIcons.menu.widthMax + 2 * skip;
	RegisterScene({
		.name = u"ghost_button_menu"_q,
		.size = QSize(outer, 0),
		.create = [=](not_null<Ui::RpWidget*> parent) {
			const auto padding = st::popupMenuWithIcons.scrollPadding;
			const auto result = Ui::CreateChild<Ui::RpWidget>(parent.get());
			const auto menu = Ui::CreateChild<Ui::Menu::Menu>(
				result,
				st::popupMenuWithIcons.menu);
			FillPresetMenu(menu, PresetMenuArgs{
				.preset = { true, true, true, true, false },
				.openSettings = [] {},
			});
			const auto shadow = result->lifetime().make_state<Ui::BoxShadow>(
				st::popupMenuWithIcons.shadow);
			result->paintRequest(
			) | rpl::on_next([=] {
				const auto radius = st::popupMenuWithIcons.radius;
				const auto panel = menu->geometry().marginsAdded(padding);
				auto p = QPainter(result);
				auto hq = PainterHighQualityEnabler(p);
				p.setPen(Qt::NoPen);
				p.setBrush(st::popupMenuWithIcons.menu.itemBg);
				p.drawRoundedRect(panel, radius, radius);
				shadow->paint(p, panel, radius);
			}, result->lifetime());
			menu->sizeValue(
			) | rpl::on_next([=](QSize size) {
				menu->move(
					(outer - size.width()) / 2,
					skip + padding.top());
				result->setGeometry(
					0,
					0,
					outer,
					skip
						+ padding.top()
						+ size.height()
						+ padding.bottom()
						+ skip);
				result->update();
			}, result->lifetime());
			return result;
		},
		.fit = false,
	});
});

} // namespace

namespace GhostMode {

State Current() {
	return ComputeMode(CurrentModel());
}

rpl::producer<State> Value() {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		Get().changes()
	) | rpl::map([] {
		return Current();
	}) | rpl::distinct_until_changed();
}

bool Toggle() {
	const auto was = CurrentModel();
	const auto now = Toggled(was);
	if (now == was) {
		return false;
	}
	Apply(now);
	return true;
}

void SetPreset(GhostPreset preset) {
	Apply(WithPreset(CurrentModel(), preset));
}

bool RunSelfTest(QStringList &log) {
	auto passed = true;
	const auto check = [&](bool condition, const QString &what) {
		if (!condition) {
			passed = false;
			log.push_back(u"FAIL: "_q + what);
		}
		return condition;
	};
	const auto all = Flags();
	const auto flags = [](
			bool read,
			bool typing,
			bool online,
			bool stories,
			bool offlineSend) {
		return Flags{ read, typing, online, stories, offlineSend };
	};

	// The state of the preset toggles.
	check(
		ComputeMode({ all, kNone, kNone }) == Mode::Off,
		u"state: nothing is on"_q);
	check(
		ComputeMode({ all, flags(1, 0, 0, 0, 0), kNone }) == Mode::Partial,
		u"state: one of five is on"_q);
	check(
		ComputeMode({ all, all, kNone }) == Mode::On,
		u"state: everything is on"_q);
	check(
		ComputeMode({ kNone, all, kNone }) == Mode::Off,
		u"state: an empty preset is never on"_q);
	check(
		ComputeMode({ flags(1, 1, 0, 0, 0), flags(1, 1, 0, 0, 1), kNone })
			== Mode::On,
		u"state: only the preset toggles count"_q);

	// Off -> on -> off.
	{
		const auto on = Toggled({ all, kNone, kNone });
		check(
			(on.values == all) && (on.owned == all) && (on.preset == all),
			u"toggle: everything is turned on and remembered"_q);
		const auto off = Toggled(on);
		check(
			(off.values == kNone) && (off.owned == kNone),
			u"toggle: everything is turned off again"_q);
	}

	// A toggle enabled in Settings before the click survives it.
	{
		const auto was = Model{ all, flags(1, 0, 0, 0, 0), kNone };
		const auto on = Toggled(was);
		check(
			(on.values == all) && (on.owned == flags(0, 1, 1, 1, 1)),
			u"toggle: only the toggles that were off are remembered"_q);
		const auto off = Toggled(on);
		check(
			(off == was) && (ComputeMode(off) == Mode::Partial),
			u"toggle: the previous toggles are restored"_q);
	}

	// Everything was enabled by hand: the click still turns it off.
	{
		const auto off = Toggled({ all, all, kNone });
		check(
			(off.values == kNone) && (off.owned == kNone),
			u"toggle: on without remembered toggles turns all off"_q);
	}

	// Toggles outside of the preset are not touched.
	{
		const auto preset = flags(1, 1, 0, 0, 0);
		const auto was = Model{ preset, flags(0, 0, 0, 1, 0), kNone };
		const auto on = Toggled(was);
		check(
			(on.values == flags(1, 1, 0, 1, 0)) && (on.owned == preset),
			u"toggle: a partial preset turns on only its toggles"_q);
		check(
			Toggled(on) == was,
			u"toggle: a partial preset turns off only its toggles"_q);
		const auto empty = Model{ kNone, flags(0, 1, 0, 0, 0), kNone };
		check(
			Toggled(empty) == empty,
			u"toggle: an empty preset switches nothing"_q);
	}

	// A toggle switched off elsewhere is not remembered any more.
	{
		auto model = Toggled({ all, kNone, kNone });
		model.values.typing = false;
		model.owned = SyncedOwned(model);
		check(
			(model.owned == flags(1, 0, 1, 1, 1))
				&& (ComputeMode(model) == Mode::Partial),
			u"sync: a toggle switched off elsewhere is forgotten"_q);
		model.values.typing = true;
		const auto off = Toggled(model);
		check(
			off.values == flags(0, 1, 0, 0, 0),
			u"sync: a toggle enabled by hand later stays on"_q);
		auto stale = Model{ flags(1, 1, 0, 0, 0), all, all };
		check(
			SyncedOwned(stale) == flags(1, 1, 0, 0, 0),
			u"sync: toggles outside of the preset are forgotten"_q);
	}

	// Changing the preset.
	{
		const auto on = Toggled({ all, flags(1, 0, 0, 0, 0), kNone });
		const auto without = WithPreset(on, flags(1, 1, 1, 1, 0));
		check(
			(without.values == flags(1, 1, 1, 1, 0))
				&& (without.owned == flags(0, 1, 1, 1, 0))
				&& (ComputeMode(without) == Mode::On),
			u"preset: a removed toggle that was turned on is turned off"_q);
		const auto kept = WithPreset(on, flags(0, 1, 1, 1, 1));
		check(
			kept.values == all,
			u"preset: a removed toggle that was on before stays on"_q);
		const auto added = WithPreset(without, all);
		check(
			(added.values == all)
				&& (added.owned == flags(0, 1, 1, 1, 1))
				&& (ComputeMode(added) == Mode::On),
			u"preset: a toggle added while on is turned on"_q);
		const auto off = Model{ flags(1, 1, 0, 0, 0), kNone, kNone };
		const auto wider = WithPreset(off, all);
		check(
			(wider.values == kNone) && (wider.preset == all),
			u"preset: a toggle added while off stays off"_q);
	}

	// The glyph, at 1x and 2x.
	for (const auto ratio : { 1, 2 }) {
		const auto side = kGlyphSide * ratio;
		const auto render = [&](float64 filled) {
			auto result = QImage(
				QSize(side, side),
				QImage::Format_ARGB32_Premultiplied);
			result.fill(Qt::transparent);
			auto p = QPainter(&result);
			PaintGhostGlyph(p, QRectF(0, 0, side, side), Qt::white, filled);
			p.end();
			return result;
		};
		const auto alpha = [&](const QImage &image, float64 x, float64 y) {
			return qAlpha(image.pixel(int(x * ratio), int(y * ratio)));
		};
		// The curves are flattened in opposite directions on the two
		// sides, so the edge pixels may differ a little.
		constexpr auto kTolerance = 48;
		const auto symmetric = [&](const QImage &image) {
			for (auto y = 0; y != side; ++y) {
				for (auto x = 0; x != side / 2; ++x) {
					const auto left = qAlpha(image.pixel(x, y));
					const auto right = qAlpha(image.pixel(side - 1 - x, y));
					if (std::abs(left - right) > kTolerance) {
						return false;
					}
				}
			}
			return true;
		};
		const auto suffix = u" at %1x"_q.arg(ratio);
		const auto solid = render(1.);
		check(
			(alpha(solid, 12, 14.5) > 200)
				&& (alpha(solid, kGlyphEyeLeft, kGlyphEyesY) < 60)
				&& (alpha(solid, kGlyphEyeRight, kGlyphEyesY) < 60),
			u"glyph: a solid body with the eyes cut out"_q + suffix);
		check(
			!alpha(solid, 1, 1)
				&& !alpha(solid, 12, 22.5)
				&& !alpha(solid, 3.5, 12)
				&& !alpha(solid, 20.5, 12),
			u"glyph: the solid one stays inside its bounds"_q + suffix);
		check(
			symmetric(solid),
			u"glyph: the solid one is symmetric"_q + suffix);
		const auto outline = render(0.);
		check(
			(alpha(outline, 12, 14.5) < 60)
				&& (alpha(outline, kGlyphEyeLeft, kGlyphEyesY) > 150)
				&& (alpha(outline, 5.75, 12) > 150)
				&& (alpha(outline, 18, 12) > 150),
			u"glyph: an outline with the eyes drawn"_q + suffix);
		check(
			!alpha(outline, 1, 1)
				&& !alpha(outline, 12, 22.5)
				&& !alpha(outline, 3.5, 12)
				&& !alpha(outline, 20.5, 12),
			u"glyph: the outlined one stays inside its bounds"_q + suffix);
		check(
			symmetric(outline),
			u"glyph: the outlined one is symmetric"_q + suffix);
	}

	if (passed) {
		log.push_back(u"ghost mode toggling, preset changes, glyph: ok"_q);
	}
	return passed;
}

} // namespace GhostMode

bool GhostButtonShown() {
	return Get().ghostButton();
}

rpl::producer<> GhostButtonShownChanges() {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		Get().changes()
	) | rpl::map([] {
		return GhostButtonShown();
	}) | rpl::distinct_until_changed() | rpl::skip(1) | rpl::to_empty;
}

object_ptr<Ui::RpWidget> CreateGhostButton(
		not_null<QWidget*> parent,
		Window::SessionController *controller) {
	const auto weak = controller
		? base::make_weak(controller)
		: base::weak_ptr<Window::SessionController>();
	const auto hasController = (controller != nullptr);
	auto result = object_ptr<Button>(parent, ButtonArgs{
		.mode = GhostMode::Value(),
		.toggle = [] { return GhostMode::Toggle(); },
		.menu = [=] {
			auto args = PresetMenuArgs{
				.preset = Get().ghostPreset(),
				.change = [](GhostPreset preset) {
					GhostMode::SetPreset(preset);
				},
			};
			if (hasController) {
				args.openSettings = [=] {
					if (const auto strong = weak.get()) {
						strong->setHighlightControlId(u"oblivion/ghost"_q);
						strong->showSettings(::Settings::OblivionId());
					}
				};
				args.toast = [=](const QString &text) {
					if (const auto strong = weak.get()) {
						strong->showToast(text);
					}
				};
			}
			return args;
		},
	});
	Get().changes() | rpl::on_next([] {
		SyncOwned();
	}, result->lifetime());
	return result;
}

int GhostButtonLockOverlap() {
	return style::ConvertScale(kLockOverlap);
}

} // namespace Oblivion
