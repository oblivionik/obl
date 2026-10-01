/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_ui_snapshots.h"

// Reference scenes for the "ui" self-test mode, they also show how a tool
// registers its own scenes (see oblivion_ui_snapshots.h).

#include "lang/lang_keys.h"
#include "settings/settings_common.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/vertical_list.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

namespace Oblivion::SelfTest {
namespace {

constexpr auto kSettingsWidth = 420;
constexpr auto kBoxWindowWidth = 480;
constexpr auto kSliderPercent = 60;

// A vertical list like the Oblivion section of the settings: subsection
// titles, buttons with icons, toggles, labels, a slider and checkboxes.
[[nodiscard]] QWidget *CreateSettingsDemo(not_null<Ui::RpWidget*> parent) {
	const auto container = Ui::CreateChild<Ui::VerticalLayout>(parent.get());
	const auto toggle = [&](
			rpl::producer<QString> text,
			const style::icon &icon,
			bool checked) {
		Settings::AddButtonWithIcon(
			container,
			std::move(text),
			st::settingsButton,
			{ &icon }
		)->toggleOn(rpl::single(checked));
	};
	const auto button = [&](
			rpl::producer<QString> text,
			const style::icon &icon) {
		Settings::AddButtonWithIcon(
			container,
			std::move(text),
			st::settingsButton,
			{ &icon });
	};

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, tr::lng_oblivion_ghost_section());
	toggle(tr::lng_oblivion_ghost_read(), st::menuIconStealth, true);
	Settings::AddButtonWithLabel(
		container,
		tr::lng_oblivion_ghost_read_exceptions(),
		rpl::single(QString::number(3)),
		st::settingsButton,
		{ &st::menuIconChats });
	toggle(
		tr::lng_oblivion_ghost_stories(),
		st::menuIconStoriesSavedSection,
		true);
	toggle(tr::lng_oblivion_ghost_typing(), st::menuIconEdit, true);
	toggle(tr::lng_oblivion_ghost_online(), st::menuIconUserHide, false);
	Ui::AddSkip(container);
	Ui::AddDividerText(container, tr::lng_oblivion_offline_send_about());
	Ui::AddSkip(container);

	Ui::AddSubsectionTitle(container, tr::lng_oblivion_tools_section());
	button(tr::lng_oblivion_tools_sticker_studio(), st::menuIconStickerCreate);
	button(tr::lng_oblivion_tools_gift_catalog(), st::menuIconGiftPremium);
	button(tr::lng_oblivion_tools_music_editor(), st::menuIconSoundSelect);
	Settings::AddButtonWithLabel(
		container,
		tr::lng_oblivion_tools_voice_effect(),
		tr::lng_oblivion_tools_voice_effect_none(),
		st::settingsButton,
		{ &st::menuIconSoundAdd });
	auto slider = Settings::MakeSliderWithLabel(
		container,
		st::settingsScale,
		st::settingsScaleLabel,
		st::normalFont->spacew * 2,
		st::settingsScaleLabel.style.font->width(u"300%"_q),
		true);
	container->add(std::move(slider.widget), st::settingsScalePadding);
	slider.slider->setAlwaysDisplayMarker(true);
	slider.slider->setValue(kSliderPercent / 100.);
	slider.label->setText(QString::number(kSliderPercent) + '%');
	Ui::AddSkip(container);
	Ui::AddDivider(container);
	Ui::AddSkip(container);

	Ui::AddSubsectionTitle(container, tr::lng_oblivion_interface());
	container->add(
		object_ptr<Ui::Checkbox>(
			container,
			tr::lng_oblivion_hide_stories_bar(tr::now),
			true,
			st::settingsCheckbox),
		st::settingsCheckboxPadding);
	container->add(
		object_ptr<Ui::Checkbox>(
			container,
			tr::lng_oblivion_hide_premium_promo(tr::now),
			false,
			st::settingsCheckbox),
		st::settingsCheckboxPadding);
	Ui::AddSkip(container);
	Ui::AddDividerText(container, tr::lng_oblivion_visual_about());
	return container;
}

// The content of a box without a layer around it: a title, a label, an
// input field, a divider and the buttons, painted on the box background.
[[nodiscard]] QWidget *CreateBoxContentDemo(not_null<Ui::RpWidget*> parent) {
	const auto container = Ui::CreateChild<Ui::VerticalLayout>(parent.get());
	container->paintRequest(
	) | rpl::on_next([=](QRect clip) {
		QPainter(container).fillRect(clip, st::boxBg);
	}, container->lifetime());

	const auto &boxSt = st::defaultBox;
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			tr::lng_oblivion_local_name_title(),
			st::boxTitle),
		style::margins(
			st::boxTitlePosition.x(),
			st::boxTitlePosition.y(),
			st::boxTitlePosition.x(),
			st::boxTitlePosition.y()));
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			tr::lng_oblivion_local_name_about(),
			st::boxLabel),
		st::boxRowPadding);
	container->add(
		object_ptr<Ui::InputField>(
			container,
			st::defaultInputField,
			tr::lng_oblivion_local_name_title(),
			tr::lng_oblivion_app_icon_default(tr::now)),
		st::boxRowPadding + style::margins(0, st::boxLittleSkip, 0, 0));
	Ui::AddSkip(container, st::boxMediumSkip);
	Ui::AddDividerText(container, tr::lng_oblivion_local_name_original());

	const auto buttons = container->add(object_ptr<Ui::RpWidget>(container));
	buttons->resize(
		buttons->width(),
		(boxSt.buttonPadding.top()
			+ boxSt.buttonHeight
			+ boxSt.buttonPadding.bottom()));
	const auto save = Ui::CreateChild<Ui::RoundButton>(
		buttons,
		tr::lng_settings_save(),
		boxSt.button);
	const auto cancel = Ui::CreateChild<Ui::RoundButton>(
		buttons,
		tr::lng_cancel(),
		boxSt.button);
	rpl::combine(
		buttons->widthValue(),
		save->widthValue(),
		cancel->widthValue()
	) | rpl::on_next([=](int width, int saveWidth, int) {
		const auto &padding = st::defaultBox.buttonPadding;
		save->moveToRight(padding.right(), padding.top(), width);
		cancel->moveToRight(
			padding.right() + saveWidth + padding.left(),
			padding.top(),
			width);
	}, buttons->lifetime());
	return container;
}

// A real Ui::GenericBox shown in a layer, as it appears over a window.
void AppIconDemoBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(tr::lng_oblivion_app_icon());
	box->setWidth(st::boxWideWidth);

	const auto group = std::make_shared<Ui::RadiobuttonGroup>(0);
	const auto options = std::vector<QString>{
		tr::lng_oblivion_app_icon_default(tr::now),
		tr::lng_oblivion_app_icon_telegram(tr::now),
		tr::lng_oblivion_app_icon_custom(tr::now),
	};
	box->addSkip(st::boxOptionListPadding.top() + st::boxLittleSkip);
	for (auto i = 0; i != int(options.size()); ++i) {
		box->addRow(
			object_ptr<Ui::Radiobutton>(
				box,
				group,
				i,
				options[i],
				st::defaultBoxCheckbox),
			style::margins(
				st::boxPadding.left() + st::boxOptionListPadding.left(),
				0,
				st::boxPadding.right(),
				st::boxOptionListSkip));
	}
	box->addRow(
		object_ptr<Ui::Checkbox>(
			box,
			tr::lng_oblivion_app_icon_finder(tr::now),
			true,
			st::defaultBoxCheckbox),
		style::margins(
			st::boxPadding.left(),
			st::boxLittleSkip,
			st::boxPadding.right(),
			st::boxLittleSkip));
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_oblivion_app_icon_finder_about(),
			st::boxDividerLabel),
		st::boxRowPadding + style::margins(0, 0, 0, st::boxLittleSkip));
	box->addButton(tr::lng_settings_save(), [=] { box->closeBox(); });
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

const auto DemoScenes = SceneRegistrar([] {
	RegisterScene(
		u"demo_settings"_q,
		QSize(style::ConvertScale(kSettingsWidth), 0),
		CreateSettingsDemo);
	RegisterScene(
		u"demo_box_content"_q,
		QSize(st::boxWideWidth, 0),
		CreateBoxContentDemo);
	RegisterBoxScene(
		u"demo_box_layer"_q,
		QSize(style::ConvertScale(kBoxWindowWidth), 0),
		[](std::shared_ptr<Ui::Show>) {
			return Box(AppIconDemoBox);
		});
});

} // namespace
} // namespace Oblivion::SelfTest
