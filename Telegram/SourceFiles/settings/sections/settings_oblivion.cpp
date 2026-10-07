/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/sections/settings_oblivion.h"

#include "settings/settings_builder.h"
#include "settings/settings_common_session.h"
#include "settings/sections/settings_main.h"
#include "base/platform/base_platform_info.h"
#include "boxes/peer_list_box.h"
#include "boxes/peer_list_controllers.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "oblivion/oblivion_app_icon.h"
#include "oblivion/oblivion_badge.h"
#include "oblivion/oblivion_cloud_share.h"
#include "oblivion/oblivion_cloud_social.h"
#include "oblivion/oblivion_cloud_sync.h"
#include "oblivion/oblivion_cloud_ui.h"
#include "oblivion/oblivion_cloud_update.h"
#include "oblivion/oblivion_deleted.h"
#include "oblivion/oblivion_gift_catalog.h"
#include "oblivion/oblivion_lottie_editor.h"
#include "oblivion/oblivion_music_editor.h"
#include "oblivion/oblivion_online.h"
#include "oblivion/oblivion_photo_integration.h"
#include "oblivion/oblivion_playlists.h"
#include "oblivion/oblivion_room.h"
#include "oblivion/oblivion_send_online.h"
#include "oblivion/oblivion_settings.h"
#include "oblivion/oblivion_sticker_batch.h"
#include "oblivion/oblivion_sticker_packs.h"
#include "oblivion/oblivion_sticker_studio.h"
#include "oblivion/oblivion_transcribe.h"
#include "oblivion/oblivion_video_editor.h"
#include "oblivion/oblivion_voice_changer.h"
#include "ui/boxes/confirm_box.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

namespace Settings {
namespace {

using namespace Builder;

void EditBalanceBox(
		not_null<Ui::GenericBox*> box,
		rpl::producer<QString> title,
		int64 current,
		Fn<void(int64)> save) {
	box->setTitle(std::move(title));

	const auto field = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(QString()),
			QString::number(current)));
	field->setInstantReplaces(Ui::InstantReplaces());
	box->setFocusCallback([=] {
		field->setFocusFast();
	});

	const auto submit = [=] {
		const auto text = field->getLastText().trimmed();
		auto ok = false;
		const auto value = text.toLongLong(&ok);
		save((ok && value > 0) ? value : 0);
		box->closeBox();
	};
	field->submits() | rpl::on_next(submit, box->lifetime());

	box->addButton(tr::lng_settings_save(), submit);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

struct TranscribeLanguage {
	QString code;
	QString name;
};

[[nodiscard]] std::vector<TranscribeLanguage> TranscribeLanguages() {
	return {
		{ QString(), QString() },
		{ u"ru-RU"_q, u"Русский"_q },
		{ u"en-US"_q, u"English"_q },
		{ u"uk-UA"_q, u"Українська"_q },
		{ u"de-DE"_q, u"Deutsch"_q },
		{ u"es-ES"_q, u"Español"_q },
		{ u"fr-FR"_q, u"Français"_q },
		{ u"it-IT"_q, u"Italiano"_q },
		{ u"pt-BR"_q, u"Português"_q },
		{ u"tr-TR"_q, u"Türkçe"_q },
		{ u"kk-KZ"_q, u"Қазақ тілі"_q },
	};
}

[[nodiscard]] QString TranscribeLanguageName(const QString &code) {
	if (code.isEmpty()) {
		return tr::lng_oblivion_transcribe_language_auto(tr::now);
	}
	for (const auto &language : TranscribeLanguages()) {
		if (language.code == code) {
			return language.name;
		}
	}
	return code;
}

void TranscribeLanguageBox(not_null<Ui::GenericBox*> box) {
	const auto languages = TranscribeLanguages();
	const auto current = Oblivion::Get().transcribeLanguage();
	auto options = std::vector<QString>();
	auto selected = 0;
	for (const auto &language : languages) {
		if (language.code == current) {
			selected = int(options.size());
		}
		options.push_back(TranscribeLanguageName(language.code));
	}
	SingleChoiceBox(box, {
		.title = tr::lng_oblivion_transcribe_language(),
		.options = options,
		.initialSelection = selected,
		.callback = [=](int index) {
			if (index >= 0 && index < int(languages.size())) {
				Oblivion::Get().setTranscribeLanguage(languages[index].code);
			}
		},
	});
}

class GhostExceptionsController final : public PeerListController {
public:
	explicit GhostExceptionsController(not_null<Main::Session*> session);

	Main::Session &session() const override;
	void prepare() override;
	void rowClicked(not_null<PeerListRow*> row) override;
	void rowRightActionClicked(not_null<PeerListRow*> row) override;

private:
	void refreshRows();

	const not_null<Main::Session*> _session;

};

GhostExceptionsController::GhostExceptionsController(
	not_null<Main::Session*> session)
: _session(session) {
}

Main::Session &GhostExceptionsController::session() const {
	return *_session;
}

void GhostExceptionsController::prepare() {
	delegate()->peerListSetTitle(
		tr::lng_oblivion_ghost_read_exceptions_title());
	delegate()->peerListSetAboveWidget(object_ptr<Ui::DividerLabel>(
		(QWidget*)nullptr,
		object_ptr<Ui::FlatLabel>(
			(QWidget*)nullptr,
			tr::lng_oblivion_ghost_read_exceptions_about(),
			st::boxDividerLabel),
		st::defaultBoxDividerLabelPadding));
	refreshRows();

	Oblivion::Get().changes() | rpl::on_next([=] {
		refreshRows();
	}, lifetime());
}

void GhostExceptionsController::rowClicked(not_null<PeerListRow*> row) {
}

void GhostExceptionsController::rowRightActionClicked(
		not_null<PeerListRow*> row) {
	Oblivion::Get().setGhostReadException(row->peer()->id.value, false);
}

void GhostExceptionsController::refreshRows() {
	const auto &list = Oblivion::Get().ghostReadExceptions();
	auto count = delegate()->peerListFullRowsCount();
	for (auto i = 0; i != count;) {
		const auto row = delegate()->peerListRowAt(i);
		if (list.contains(row->peer()->id.value)) {
			++i;
		} else {
			delegate()->peerListRemoveRow(row);
			--count;
		}
	}
	for (const auto peerId : list) {
		if (delegate()->peerListFindRow(peerId)) {
			continue;
		} else if (const auto peer = _session->data().peerLoaded(
				PeerId(peerId))) {
			auto row = std::make_unique<PeerListRowWithLink>(peer);
			row->setActionLink(tr::lng_box_remove(tr::now));
			delegate()->peerListAppendRow(std::move(row));
		}
	}
	setDescriptionText(delegate()->peerListFullRowsCount()
		? QString()
		: tr::lng_oblivion_ghost_read_exceptions_empty(tr::now));
	delegate()->peerListRefreshRows();
}

void ShowGhostExceptionsBox(
		not_null<Window::SessionController*> controller) {
	auto list = std::make_unique<GhostExceptionsController>(
		&controller->session());
	auto init = [=](not_null<PeerListBox*> box) {
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		box->addLeftButton(
			tr::lng_oblivion_ghost_read_exceptions_clear(),
			[=] {
				if (Oblivion::Get().ghostReadExceptions().empty()) {
					return;
				}
				controller->show(Ui::MakeConfirmBox({
					.text = tr::lng_oblivion_ghost_read_exceptions_clear_sure(),
					.confirmed = [](Fn<void()> close) {
						Oblivion::Get().clearGhostReadExceptions();
						close();
					},
					.confirmText = tr::lng_box_remove(),
					.confirmStyle = &st::attentionBoxButton,
				}));
			},
			st::attentionBoxButton);
	};
	controller->show(Box<PeerListBox>(std::move(list), std::move(init)));
}

using SettingGetter = bool (Oblivion::Settings::*)() const;
using SettingSetter = void (Oblivion::Settings::*)(bool);

struct ToggleArgs {
	QString id;
	rpl::producer<QString> title;
	const style::icon *icon = nullptr;
	SettingGetter getter = nullptr;
	SettingSetter setter = nullptr;
	QStringList keywords;
};

void AddToggle(SectionBuilder &builder, ToggleArgs &&args) {
	const auto getter = args.getter;
	const auto setter = args.setter;
	const auto button = builder.addButton({
		.id = std::move(args.id),
		.title = std::move(args.title),
		.icon = { args.icon },
		.toggled = rpl::single((Oblivion::Get().*getter)()),
		.keywords = std::move(args.keywords),
	});
	if (button) {
		button->toggledChanges(
		) | rpl::filter([=](bool value) {
			return (value != (Oblivion::Get().*getter)());
		}) | rpl::on_next([=](bool value) {
			(Oblivion::Get().*setter)(value);
		}, button->lifetime());
	}
}

[[nodiscard]] rpl::producer<> SettingsChanges() {
	return rpl::single(rpl::empty) | rpl::then(Oblivion::Get().changes());
}

void AddSectionEnd(
		SectionBuilder &builder,
		rpl::producer<QString> about = nullptr) {
	builder.addSkip();
	if (about) {
		builder.addDividerText(std::move(about));
	} else {
		builder.addDivider();
	}
	builder.addSkip();
}

void BuildGhostSection(SectionBuilder &builder) {
	const auto controller = builder.controller();

	builder.addSubsectionTitle({
		.id = u"oblivion/ghost"_q,
		.title = tr::lng_oblivion_ghost_section(),
		.keywords = {
			u"ghost"_q,
			u"stealth"_q,
			u"призрак"_q,
			u"невидимка"_q,
		},
	});
	AddToggle(builder, {
		.id = u"oblivion/ghost_read"_q,
		.title = tr::lng_oblivion_ghost_read(),
		.icon = &st::menuIconStealth,
		.getter = &Oblivion::Settings::ghostRead,
		.setter = &Oblivion::Settings::setGhostRead,
		.keywords = {
			u"read"_q,
			u"receipts"_q,
			u"ghost"_q,
			u"прочтение"_q,
			u"прочитано"_q,
			u"призрак"_q,
		},
	});
	builder.addButton({
		.id = u"oblivion/ghost_read_exceptions"_q,
		.title = tr::lng_oblivion_ghost_read_exceptions(),
		.icon = { &st::menuIconChats },
		.label = SettingsChanges() | rpl::map([] {
			const auto count = int(
				Oblivion::Get().ghostReadExceptions().size());
			return count ? QString::number(count) : QString();
		}),
		.onClick = [=] { ShowGhostExceptionsBox(controller); },
		.keywords = {
			u"exceptions"_q,
			u"chats"_q,
			u"исключения"_q,
			u"чаты"_q,
		},
	});
	AddToggle(builder, {
		.id = u"oblivion/read_on_send"_q,
		.title = tr::lng_oblivion_read_on_send(),
		.icon = &st::menuIconMarkRead,
		.getter = &Oblivion::Settings::readOnSend,
		.setter = &Oblivion::Settings::setReadOnSend,
		.keywords = {
			u"read"_q,
			u"send"_q,
			u"reply"_q,
			u"прочитанным"_q,
			u"отправка"_q,
		},
	});
	AddToggle(builder, {
		.id = u"oblivion/ghost_stories"_q,
		.title = tr::lng_oblivion_ghost_stories(),
		.icon = &st::menuIconStoriesSavedSection,
		.getter = &Oblivion::Settings::ghostStories,
		.setter = &Oblivion::Settings::setGhostStories,
		.keywords = {
			u"stories"_q,
			u"views"_q,
			u"ghost"_q,
			u"истории"_q,
			u"просмотры"_q,
		},
	});
	AddToggle(builder, {
		.id = u"oblivion/ghost_typing"_q,
		.title = tr::lng_oblivion_ghost_typing(),
		.icon = &st::menuIconEdit,
		.getter = &Oblivion::Settings::ghostTyping,
		.setter = &Oblivion::Settings::setGhostTyping,
		.keywords = { u"typing"_q, u"ghost"_q, u"печатает"_q },
	});
	AddToggle(builder, {
		.id = u"oblivion/ghost_online"_q,
		.title = tr::lng_oblivion_ghost_online(),
		.icon = &st::menuIconUserHide,
		.getter = &Oblivion::Settings::ghostOnline,
		.setter = &Oblivion::Settings::setGhostOnline,
		.keywords = {
			u"online"_q,
			u"status"_q,
			u"ghost"_q,
			u"онлайн"_q,
			u"в сети"_q,
		},
	});
	AddToggle(builder, {
		.id = u"oblivion/offline_send"_q,
		.title = tr::lng_oblivion_offline_send(),
		.icon = &st::menuIconWhenOnline,
		.getter = &Oblivion::Settings::offlineSend,
		.setter = &Oblivion::Settings::setOfflineSend,
		.keywords = {
			u"offline"_q,
			u"send"_q,
			u"scheduled"_q,
			u"офлайн"_q,
			u"отложенные"_q,
		},
	});
	AddSectionEnd(builder, tr::lng_oblivion_offline_send_about());
}

void BuildTrackingSection(SectionBuilder &builder) {
	const auto controller = builder.controller();

	builder.addSubsectionTitle({
		.id = u"oblivion/tracking"_q,
		.title = tr::lng_oblivion_tracking_section(),
		.keywords = {
			u"tracking"_q,
			u"online"_q,
			u"слежка"_q,
			u"в сети"_q,
		},
	});
	AddToggle(builder, {
		.id = u"oblivion/online_journal"_q,
		.title = tr::lng_oblivion_online_journal(),
		.icon = &st::menuIconStats,
		.getter = &Oblivion::Settings::onlineJournal,
		.setter = &Oblivion::Settings::setOnlineJournal,
		.keywords = {
			u"online"_q,
			u"journal"_q,
			u"last seen"_q,
			u"онлайн"_q,
			u"журнал"_q,
			u"был в сети"_q,
		},
	});
	builder.addButton({
		.id = u"oblivion/online_notify"_q,
		.title = tr::lng_oblivion_online_notify(),
		.icon = { &st::menuIconNotifications },
		.label = SettingsChanges() | rpl::map([] {
			const auto count = int(
				Oblivion::Get().onlineNotifyList().size());
			return count ? QString::number(count) : QString();
		}),
		.onClick = [=] { Oblivion::ShowOnlineNotifyList(controller); },
		.keywords = {
			u"online"_q,
			u"notifications"_q,
			u"онлайн"_q,
			u"уведомления"_q,
			u"в сети"_q,
		},
	});
	AddToggle(builder, {
		.id = u"oblivion/profile_history"_q,
		.title = tr::lng_oblivion_profile_history(),
		.icon = &st::menuIconProfile,
		.getter = &Oblivion::Settings::profileHistory,
		.setter = &Oblivion::Settings::setProfileHistory,
		.keywords = {
			u"profile"_q,
			u"history"_q,
			u"name"_q,
			u"avatar"_q,
			u"профиль"_q,
			u"история"_q,
			u"аватарки"_q,
		},
	});
	AddSectionEnd(builder, tr::lng_oblivion_tracking_about());
	AddToggle(builder, {
		.id = u"oblivion/online_polling"_q,
		.title = tr::lng_oblivion_online_polling(),
		.icon = &st::menuIconReschedule,
		.getter = &Oblivion::Settings::onlinePolling,
		.setter = &Oblivion::Settings::setOnlinePolling,
		.keywords = {
			u"online"_q,
			u"status"_q,
			u"background"_q,
			u"poll"_q,
			u"онлайн"_q,
			u"статусы"_q,
			u"в фоне"_q,
			u"проверять"_q,
		},
	});
	AddSectionEnd(builder, tr::lng_oblivion_online_polling_about());
}

void BuildSavingSection(SectionBuilder &builder) {
	const auto controller = builder.controller();

	builder.addSubsectionTitle({
		.id = u"oblivion/saving"_q,
		.title = tr::lng_oblivion_saving_section(),
		.keywords = { u"saving"_q, u"сохранение"_q },
	});
	AddToggle(builder, {
		.id = u"oblivion/keep_deleted"_q,
		.title = tr::lng_oblivion_keep_deleted(),
		.icon = &st::menuIconDelete,
		.getter = &Oblivion::Settings::keepDeleted,
		.setter = &Oblivion::Settings::setKeepDeleted,
		.keywords = {
			u"deleted"_q,
			u"keep"_q,
			u"history"_q,
			u"удалённые"_q,
			u"удаленные"_q,
		},
	});
	AddToggle(builder, {
		.id = u"oblivion/keep_edit_history"_q,
		.title = tr::lng_oblivion_keep_edit_history(),
		.icon = &st::menuIconGroupLog,
		.getter = &Oblivion::Settings::keepEditHistory,
		.setter = &Oblivion::Settings::setKeepEditHistory,
		.keywords = {
			u"edit"_q,
			u"history"_q,
			u"изменения"_q,
			u"правки"_q,
		},
	});
	builder.addButton({
		.id = u"oblivion/deleted_messages"_q,
		.title = tr::lng_oblivion_deleted_title(),
		.icon = { &st::menuIconArchive },
		.onClick = [=] { Oblivion::ShowDeletedMessages(controller, nullptr); },
		.keywords = {
			u"deleted"_q,
			u"messages"_q,
			u"удалённые"_q,
			u"удаленные"_q,
		},
	});
	AddToggle(builder, {
		.id = u"oblivion/save_self_destructing"_q,
		.title = tr::lng_oblivion_save_self_destructing(),
		.icon = &st::menuIconTimer,
		.getter = &Oblivion::Settings::saveSelfDestructing,
		.setter = &Oblivion::Settings::setSaveSelfDestructing,
		.keywords = {
			u"view once"_q,
			u"self-destructing"_q,
			u"одноразовые"_q,
			u"исчезающие"_q,
		},
	});
	AddSectionEnd(builder, tr::lng_oblivion_saving_about());
}

void BuildProtectedSection(SectionBuilder &builder) {
	builder.addSubsectionTitle({
		.id = u"oblivion/protected"_q,
		.title = tr::lng_oblivion_protected_section(),
		.keywords = { u"protected"_q, u"защищённый"_q },
	});
	AddToggle(builder, {
		.id = u"oblivion/copy_protected"_q,
		.title = tr::lng_oblivion_copy_protected(),
		.icon = &st::menuIconCopy,
		.getter = &Oblivion::Settings::allowCopyProtected,
		.setter = &Oblivion::Settings::setAllowCopyProtected,
		.keywords = {
			u"copy"_q,
			u"protected"_q,
			u"restriction"_q,
			u"копирование"_q,
		},
	});
	AddToggle(builder, {
		.id = u"oblivion/save_protected"_q,
		.title = tr::lng_oblivion_save_protected(),
		.icon = &st::menuIconDownload,
		.getter = &Oblivion::Settings::allowSaveProtected,
		.setter = &Oblivion::Settings::setAllowSaveProtected,
		.keywords = {
			u"save"_q,
			u"media"_q,
			u"download"_q,
			u"сохранение"_q,
		},
	});
	AddToggle(builder, {
		.id = u"oblivion/forward_protected"_q,
		.title = tr::lng_oblivion_forward_protected(),
		.icon = &st::menuIconForward,
		.getter = &Oblivion::Settings::forwardProtectedAsCopy,
		.setter = &Oblivion::Settings::setForwardProtectedAsCopy,
		.keywords = {
			u"forward"_q,
			u"copy"_q,
			u"пересылка"_q,
			u"копия"_q,
		},
	});
	AddToggle(builder, {
		.id = u"oblivion/stories_premium"_q,
		.title = tr::lng_oblivion_stories_premium(),
		.icon = &st::menuIconStoriesSave,
		.getter = &Oblivion::Settings::storiesWithoutPremium,
		.setter = &Oblivion::Settings::setStoriesWithoutPremium,
		.keywords = {
			u"stories"_q,
			u"premium"_q,
			u"download"_q,
			u"истории"_q,
		},
	});
	AddToggle(builder, {
		.id = u"oblivion/sticker_json"_q,
		.title = tr::lng_oblivion_sticker_json(),
		.icon = &st::menuIconStickers,
		.getter = &Oblivion::Settings::exportStickerJson,
		.setter = &Oblivion::Settings::setExportStickerJson,
		.keywords = {
			u"sticker"_q,
			u"json"_q,
			u"export"_q,
			u"lottie"_q,
			u"стикеры"_q,
		},
	});
	AddSectionEnd(builder, tr::lng_oblivion_forward_protected_about());
}

void BuildVoiceSection(SectionBuilder &builder) {
	const auto controller = builder.controller();

	builder.addSubsectionTitle({
		.id = u"oblivion/voice"_q,
		.title = tr::lng_oblivion_voice_section(),
		.keywords = { u"voice"_q, u"голосовые"_q },
	});
	AddToggle(builder, {
		.id = u"oblivion/voice_noise"_q,
		.title = tr::lng_oblivion_voice_noise(),
		.icon = &st::menuIconQualityHigh,
		.getter = &Oblivion::Settings::voiceNoiseSuppression,
		.setter = &Oblivion::Settings::setVoiceNoiseSuppression,
		.keywords = {
			u"noise"_q,
			u"rnnoise"_q,
			u"voice"_q,
			u"шум"_q,
			u"шумоподавление"_q,
			u"голосовые"_q,
		},
	});
	// On-device transcription uses the system speech recognizer, which
	// exists only on macOS (see oblivion_transcribe.h). Where there is
	// none these rows would do nothing, so they are not shown at all.
	if (!Oblivion::Transcribe::SystemRecognizerSupported()) {
		AddSectionEnd(builder, tr::lng_oblivion_voice_noise_about());
		return;
	}
	AddToggle(builder, {
		.id = u"oblivion/local_transcribe"_q,
		.title = tr::lng_oblivion_local_transcribe(),
		.icon = &st::menuIconSoundOn,
		.getter = &Oblivion::Settings::localTranscribe,
		.setter = &Oblivion::Settings::setLocalTranscribe,
		.keywords = {
			u"transcribe"_q,
			u"speech"_q,
			u"text"_q,
			u"расшифровка"_q,
			u"текст"_q,
		},
	});
	builder.addButton({
		.id = u"oblivion/transcribe_language"_q,
		.title = tr::lng_oblivion_transcribe_language(),
		.icon = { &st::menuIconTranslate },
		.label = rpl::merge(
			SettingsChanges(),
			tr::lng_oblivion_transcribe_language_auto() | rpl::to_empty
		) | rpl::map([] {
			return TranscribeLanguageName(
				Oblivion::Get().transcribeLanguage());
		}),
		.onClick = [=] { controller->show(Box(TranscribeLanguageBox)); },
		.keywords = {
			u"language"_q,
			u"speech"_q,
			u"язык"_q,
			u"распознавание"_q,
		},
	});
	AddSectionEnd(builder, tr::lng_oblivion_local_transcribe_about());
}

void BuildToolsSection(SectionBuilder &builder) {
	const auto controller = builder.controller();

	builder.addSubsectionTitle({
		.id = u"oblivion/tools"_q,
		.title = tr::lng_oblivion_tools_section(),
		.keywords = { u"tools"_q, u"инструменты"_q },
	});
	builder.addButton({
		.id = u"oblivion/sticker_studio"_q,
		.title = tr::lng_oblivion_tools_sticker_studio(),
		.icon = { &st::menuIconStickerCreate },
		.onClick = [=] { Oblivion::ShowStickerStudioImport(controller); },
		.keywords = {
			u"sticker"_q,
			u"lottie"_q,
			u"tgs"_q,
			u"svg"_q,
			u"hue"_q,
			u"стикеры"_q,
			u"редактор"_q,
			u"оттенок"_q,
		},
	});
	builder.addButton({
		.id = u"oblivion/gift_catalog"_q,
		.title = tr::lng_oblivion_tools_gift_catalog(),
		.icon = { &st::menuIconGiftPremium },
		.onClick = [=] { Oblivion::ShowGiftCatalog(controller); },
		.keywords = {
			u"gifts"_q,
			u"catalog"_q,
			u"models"_q,
			u"подарки"_q,
			u"каталог"_q,
			u"модели"_q,
		},
	});
	builder.addButton({
		.id = u"oblivion/music_editor"_q,
		.title = tr::lng_oblivion_tools_music_editor(),
		.icon = { &st::menuIconSoundSelect },
		.onClick = [=] { Oblivion::ShowMusicEditor(controller, nullptr); },
		.keywords = {
			u"music"_q,
			u"slowed"_q,
			u"speed up"_q,
			u"pitch"_q,
			u"музыка"_q,
			u"замедление"_q,
			u"тональность"_q,
		},
	});
	builder.addButton({
		.id = u"oblivion/playlists"_q,
		.title = tr::lng_oblivion_tools_playlists(),
		.icon = { &st::menuIconReorder },
		.onClick = [=] { Oblivion::ShowPlaylists(controller); },
		.keywords = {
			u"playlists"_q,
			u"music"_q,
			u"плейлисты"_q,
			u"музыка"_q,
		},
	});
	builder.addButton({
		.id = u"oblivion/voice_effect"_q,
		.title = tr::lng_oblivion_tools_voice_effect(),
		.icon = { &st::menuIconSoundAdd },
		.label = rpl::merge(
			SettingsChanges(),
			tr::lng_oblivion_tools_voice_effect_none() | rpl::to_empty
		) | rpl::map([] {
			return Oblivion::VoiceEffectName(Oblivion::Get().voiceEffect());
		}),
		.onClick = [=] { Oblivion::ShowVoiceEffectBox(controller); },
		.keywords = {
			u"voice"_q,
			u"effect"_q,
			u"changer"_q,
			u"голосовые"_q,
			u"эффект"_q,
			u"войсчейнджер"_q,
		},
	});
	builder.addButton({
		.id = u"oblivion/lottie_editor"_q,
		.title = tr::lng_oblivion_tools_lottie_editor(),
		.icon = { &st::menuIconDraw },
		.onClick = [] { Oblivion::LottieEdit::ShowLottieEditorImport(); },
		.keywords = {
			u"lottie"_q,
			u"tgs"_q,
			u"animation"_q,
			u"editor"_q,
			u"keyframes"_q,
			u"анимация"_q,
			u"редактор"_q,
			u"ключевые кадры"_q,
		},
	});
	builder.addButton({
		.id = u"oblivion/photo_editor"_q,
		.title = tr::lng_oblivion_photo_io_tools(),
		.icon = { &st::menuIconPalette },
		.onClick = [=] { Oblivion::ShowPhotoEditorImport(controller); },
		.keywords = {
			u"photo"_q,
			u"image"_q,
			u"editor"_q,
			u"filters"_q,
			u"фото"_q,
			u"фоторедактор"_q,
			u"изображение"_q,
			u"фильтры"_q,
		},
	});
	// Round 4: photo collage.
	builder.addButton({
		.id = u"oblivion/photo_collage"_q,
		.title = tr::lng_oblivion_photo_collage_settings(),
		.icon = { &st::menuIconPhotoSet },
		.onClick = [=] { Oblivion::ShowPhotoCollage(controller); },
		.keywords = {
			u"collage"_q,
			u"photo"_q,
			u"grid"_q,
			u"коллаж"_q,
			u"фото"_q,
			u"сетка"_q,
		},
	});
	// Round 4: photo collage end.
	builder.addButton({
		.id = u"oblivion/video_editor"_q,
		.title = tr::lng_oblivion_tools_video_editor(),
		.icon = { &st::menuIconVideoChat },
		.onClick = [=] { Oblivion::ShowVideoEditorImport(controller); },
		.keywords = {
			u"video"_q,
			u"editor"_q,
			u"trim"_q,
			u"gif"_q,
			u"видео"_q,
			u"видеоредактор"_q,
			u"обрезка"_q,
		},
	});
	builder.addButton({
		.id = u"oblivion/sticker_packs"_q,
		.title = tr::lng_oblivion_tools_sticker_packs(),
		.icon = { &st::menuIconStickerAdd },
		.onClick = [=] { Oblivion::ShowStickerPacks(controller); },
		.keywords = {
			u"sticker"_q,
			u"pack"_q,
			u"set"_q,
			u"стикеры"_q,
			u"стикерпак"_q,
			u"набор"_q,
		},
	});
	builder.addButton({
		.id = u"oblivion/sticker_converter"_q,
		.title = tr::lng_oblivion_tools_sticker_converter(),
		.icon = { &st::menuIconStickers },
		.onClick = [=] { Oblivion::ShowStickerConverter(controller); },
		.keywords = {
			u"sticker"_q,
			u"converter"_q,
			u"webm"_q,
			u"webp"_q,
			u"png"_q,
			u"tgs"_q,
			u"стикеры"_q,
			u"конвертер"_q,
			u"видеостикер"_q,
			u"файл"_q,
		},
	});
	// Round 4: sticker batch.
	builder.addButton({
		.id = u"oblivion/sticker_batch"_q,
		.title = tr::lng_oblivion_sbatch_settings(),
		.icon = { &st::menuIconShowAll },
		.onClick = [=] { Oblivion::ShowStickerBatch(controller); },
		.keywords = {
			u"sticker"_q,
			u"emoji"_q,
			u"batch"_q,
			u"bulk"_q,
			u"pack"_q,
			u"стикеры"_q,
			u"эмодзи"_q,
			u"пакетное"_q,
			u"массовое"_q,
			u"набор"_q,
		},
	});
	// Round 4: sticker batch end.
	AddSectionEnd(builder, tr::lng_oblivion_tools_about());
}

// Round 4: badge.
// The badge is published through Oblivion Cloud since round 5: its rows
// are in BuildSocialSection() («Профиль и видимость»).
// Round 4: badge end.

void BuildInterfaceSection(SectionBuilder &builder) {
	const auto controller = builder.controller();

	builder.addSubsectionTitle({
		.id = u"oblivion/interface"_q,
		.title = tr::lng_oblivion_interface(),
		.keywords = { u"interface"_q, u"интерфейс"_q },
	});
	AddToggle(builder, {
		.id = u"oblivion/show_seconds"_q,
		.title = tr::lng_oblivion_show_seconds(),
		.icon = &st::menuIconSchedule,
		.getter = &Oblivion::Settings::showSeconds,
		.setter = &Oblivion::Settings::setShowSeconds,
		.keywords = {
			u"seconds"_q,
			u"time"_q,
			u"секунды"_q,
			u"время"_q,
		},
	});
	AddToggle(builder, {
		.id = u"oblivion/peer_ids"_q,
		.title = tr::lng_oblivion_peer_ids(),
		.icon = &st::menuIconInfo,
		.getter = &Oblivion::Settings::showPeerIds,
		.setter = &Oblivion::Settings::setShowPeerIds,
		.keywords = { u"id"_q, u"profile"_q, u"профиль"_q },
	});
	AddToggle(builder, {
		.id = u"oblivion/registration_date"_q,
		.title = tr::lng_oblivion_registration(),
		.icon = &st::menuIconHourglass,
		.getter = &Oblivion::Settings::showRegistrationDate,
		.setter = &Oblivion::Settings::setShowRegistrationDate,
		.keywords = {
			u"registration"_q,
			u"date"_q,
			u"age"_q,
			u"регистрация"_q,
		},
	});
	AddToggle(builder, {
		.id = u"oblivion/message_details"_q,
		.title = tr::lng_oblivion_message_details(),
		.icon = &st::menuIconArticle,
		.getter = &Oblivion::Settings::showMessageDetails,
		.setter = &Oblivion::Settings::setShowMessageDetails,
		.keywords = {
			u"message"_q,
			u"details"_q,
			u"info"_q,
			u"подробности"_q,
		},
	});
	AddToggle(builder, {
		.id = u"oblivion/hide_sponsored"_q,
		.title = tr::lng_oblivion_hide_sponsored(),
		.icon = &st::menuIconBlock,
		.getter = &Oblivion::Settings::hideSponsored,
		.setter = &Oblivion::Settings::setHideSponsored,
		.keywords = {
			u"ads"_q,
			u"sponsored"_q,
			u"hide"_q,
			u"реклама"_q,
		},
	});
	AddToggle(builder, {
		.id = u"oblivion/hide_stories_bar"_q,
		.title = tr::lng_oblivion_hide_stories_bar(),
		.icon = &st::menuIconStoriesArchive,
		.getter = &Oblivion::Settings::hideStoriesBar,
		.setter = &Oblivion::Settings::setHideStoriesBar,
		.keywords = {
			u"stories"_q,
			u"hide"_q,
			u"истории"_q,
			u"скрыть"_q,
		},
	});
	AddToggle(builder, {
		.id = u"oblivion/hide_premium_promo"_q,
		.title = tr::lng_oblivion_hide_premium_promo(),
		.icon = &st::menuIconPremium,
		.getter = &Oblivion::Settings::hidePremiumPromo,
		.setter = &Oblivion::Settings::setHidePremiumPromo,
		.keywords = {
			u"premium"_q,
			u"promo"_q,
			u"hide"_q,
			u"реклама"_q,
		},
	});
	AddToggle(builder, {
		.id = u"oblivion/hide_gift_promo"_q,
		.title = tr::lng_oblivion_hide_gift_promo(),
		.icon = &st::menuIconGiftPremium,
		.getter = &Oblivion::Settings::hideGiftPromo,
		.setter = &Oblivion::Settings::setHideGiftPromo,
		.keywords = {
			u"gifts"_q,
			u"promo"_q,
			u"hide"_q,
			u"подарки"_q,
		},
	});
	// The chosen icon is applied to the Dock tile and to the bundle in
	// Finder. On other systems the window and the tray icons are drawn
	// from the bundled logo (see platform/win/tray_win.cpp), a choice
	// made here would change nothing, so the row is macOS only.
	if (Platform::IsMac()) {
		builder.addButton({
			.id = u"oblivion/app_icon"_q,
			.title = tr::lng_oblivion_app_icon(),
			.icon = { &st::menuIconCustomize },
			.onClick = [=] { Oblivion::ShowAppIconBox(controller); },
			.keywords = { u"icon"_q, u"app"_q, u"иконка"_q, u"значок"_q },
		});
	}
	AddToggle(builder, {
		.id = u"oblivion/ghost_button"_q,
		.title = tr::lng_oblivion_ghost_button(),
		.icon = &st::menuIconStealthLocked,
		.getter = &Oblivion::Settings::ghostButton,
		.setter = &Oblivion::Settings::setGhostButton,
		.keywords = {
			u"ghost"_q,
			u"button"_q,
			u"призрак"_q,
			u"кнопка"_q,
		},
	});
	AddToggle(builder, {
		.id = u"oblivion/unified_chats"_q,
		.title = tr::lng_oblivion_unified_chats(),
		.icon = &st::menuIconAddAccount,
		.getter = &Oblivion::Settings::unifiedChats,
		.setter = &Oblivion::Settings::setUnifiedChats,
		.keywords = {
			u"accounts"_q,
			u"unified"_q,
			u"chats"_q,
			u"аккаунты"_q,
			u"общий"_q,
			u"чаты"_q,
		},
	});
	// Round 4: attach tools.
	AddToggle(builder, {
		.id = u"oblivion/attach_tools"_q,
		.title = tr::lng_oblivion_attach_settings(),
		.icon = &st::menuIconFile,
		.getter = &Oblivion::Settings::attachTools,
		.setter = &Oblivion::Settings::setAttachTools,
		.keywords = {
			u"attach"_q,
			u"files"_q,
			u"tools"_q,
			u"buttons"_q,
			u"send"_q,
			u"файлы"_q,
			u"отправка"_q,
			u"инструменты"_q,
			u"кнопки"_q,
		},
	});
	// Round 4: attach tools end.
	// Round 4: listen together.
	AddToggle(builder, {
		.id = u"oblivion/listen_together"_q,
		.title = tr::lng_oblivion_listen_settings(),
		.icon = &st::menuIconGroups,
		.getter = &Oblivion::Settings::listenTogether,
		.setter = &Oblivion::Settings::setListenTogether,
		.keywords = {
			u"listen"_q,
			u"together"_q,
			u"music"_q,
			u"sync"_q,
			u"слушать"_q,
			u"вместе"_q,
			u"музыка"_q,
			u"совместно"_q,
		},
	});
	AddSectionEnd(builder, tr::lng_oblivion_listen_settings_about());
	// Round 4: listen together end.
	// Round 4: badge.
	// Moved to BuildSocialSection() in round 5.
	// Round 4: badge end.
}

void BuildVisualSection(SectionBuilder &builder) {
	builder.addSubsectionTitle({
		.id = u"oblivion/visual"_q,
		.title = tr::lng_oblivion_visual(),
		.keywords = { u"visual"_q, u"визуально"_q },
	});
	AddToggle(builder, {
		.id = u"oblivion/fake_premium"_q,
		.title = tr::lng_oblivion_fake_premium(),
		.icon = &st::menuIconUnique,
		.getter = &Oblivion::Settings::fakePremium,
		.setter = &Oblivion::Settings::setFakePremium,
		.keywords = { u"premium"_q, u"visual"_q, u"fake"_q },
	});
	AddToggle(builder, {
		.id = u"oblivion/visual_gifting"_q,
		.title = tr::lng_oblivion_visual_gifting(),
		.icon = &st::menuIconUniqueProfile,
		.getter = &Oblivion::Settings::visualGifting,
		.setter = &Oblivion::Settings::setVisualGifting,
		.keywords = {
			u"gift"_q,
			u"visual"_q,
			u"send"_q,
			u"подарки"_q,
		},
	});

	const auto controller = builder.controller();
	const auto editNumber = [=](
			rpl::producer<QString> title,
			int64 current,
			Fn<void(int64)> save) {
		controller->show(
			Box(EditBalanceBox, std::move(title), current, std::move(save)));
	};

	builder.addButton({
		.id = u"oblivion/fake_stars"_q,
		.title = tr::lng_oblivion_fake_stars(),
		.icon = { &st::menuIconStar },
		.label = SettingsChanges() | rpl::map([] {
			const auto value = Oblivion::Get().fakeStars();
			return value ? QString::number(value) : QString();
		}),
		.onClick = [=] {
			editNumber(
				tr::lng_oblivion_fake_stars(),
				Oblivion::Get().fakeStars(),
				[](int64 value) { Oblivion::Get().setFakeStars(value); });
		},
		.keywords = { u"stars"_q, u"balance"_q, u"visual"_q, u"звёзды"_q },
	});

	builder.addButton({
		.id = u"oblivion/fake_ton"_q,
		.title = tr::lng_oblivion_fake_ton(),
		.icon = { &st::menuIconTon },
		.label = SettingsChanges() | rpl::map([] {
			const auto value = Oblivion::Get().fakeTon();
			return value ? QString::number(value) : QString();
		}),
		.onClick = [=] {
			editNumber(
				tr::lng_oblivion_fake_ton(),
				Oblivion::Get().fakeTon(),
				[](int64 value) { Oblivion::Get().setFakeTon(value); });
		},
		.keywords = { u"ton"_q, u"balance"_q, u"visual"_q, u"баланс"_q },
	});
	builder.addSkip();
	builder.addDividerText(tr::lng_oblivion_visual_about());
}

// Round 5. Every feature keeps its rows inside its own block; the blocks
// of the scaffold only open the entry points of the modules.
// Round 5: cloud.
// A switch that needs the cloud (it shows the user to other people or
// talks to the server by itself): turning it on first asks the consent
// to Oblivion Cloud for the account of this window, turning it off needs
// nothing. The switch itself is app-wide; the module behind it follows
// Oblivion::Get().changes() and tells the server.
void AddCloudToggle(SectionBuilder &builder, ToggleArgs &&args) {
	const auto controller = builder.controller();
	const auto getter = args.getter;
	const auto setter = args.setter;
	const auto button = builder.addButton({
		.id = std::move(args.id),
		.title = std::move(args.title),
		.icon = { args.icon },
		.onClick = [=] {
			if ((Oblivion::Get().*getter)()) {
				(Oblivion::Get().*setter)(false);
			} else if (controller) {
				Oblivion::Cloud::RequireConsent(controller, [=] {
					(Oblivion::Get().*setter)(true);
				});
			}
		},
		.keywords = std::move(args.keywords),
	});
	if (button) {
		button->toggleOn(SettingsChanges() | rpl::map([=] {
			return (Oblivion::Get().*getter)();
		}), true);
	}
}

void BuildCloudSection(SectionBuilder &builder) {
	const auto controller = builder.controller();
	const auto session = builder.session();
	const auto account = &Oblivion::Cloud::For(session);

	builder.addSubsectionTitle({
		.id = u"oblivion/cloud"_q,
		.title = tr::lng_oblivion_cloud_section(),
		.keywords = {
			u"cloud"_q,
			u"server"_q,
			u"oblivion"_q,
			u"облако"_q,
			u"сервер"_q,
		},
	});
	builder.add([=](const WidgetContext &ctx) {
		return SectionBuilder::WidgetToAdd{
			.widget = Oblivion::Cloud::CreateStatusRow(
				ctx.container.get(),
				Oblivion::Cloud::StatusValue(account)),
		};
	});
	const auto toggle = builder.addButton({
		.id = u"oblivion/cloud_toggle"_q,
		.title = tr::lng_oblivion_cloud_toggle(),
		.icon = { &st::menuIconIpAddress },
		.onClick = [=] { Oblivion::Cloud::ToggleFromSettings(controller); },
		.keywords = {
			u"cloud"_q,
			u"connect"_q,
			u"disconnect"_q,
			u"облако"_q,
			u"подключить"_q,
			u"отключить"_q,
		},
	});
	if (toggle) {
		toggle->toggleOn(Oblivion::Cloud::EnabledValue(session), true);
	}
	builder.addButton({
		.id = u"oblivion/cloud_link_device"_q,
		.title = tr::lng_oblivion_cloud_link_device(),
		.icon = { &st::menuIconDevices },
		.onClick = [=] { Oblivion::Cloud::ShowLinkDevice(controller); },
		.keywords = {
			u"link"_q,
			u"device"_q,
			u"code"_q,
			u"привязать"_q,
			u"устройство"_q,
			u"код"_q,
		},
		.shown = account->readyValue(),
	});
	builder.addButton({
		.id = u"oblivion/cloud_enter_code"_q,
		.title = tr::lng_oblivion_cloud_enter_code(),
		.icon = { &st::menuIconQrCode },
		.onClick = [=] { Oblivion::Cloud::ShowEnterCode(controller); },
		.keywords = { u"code"_q, u"link"_q, u"код"_q, u"привязать"_q },
		.shown = account->stateValue(
		) | rpl::map([](Oblivion::Cloud::State state) {
			return (state == Oblivion::Cloud::State::NeedsLink);
		}),
	});
	builder.addButton({
		.id = u"oblivion/cloud_delete"_q,
		.title = tr::lng_oblivion_cloud_delete(),
		.icon = { &st::menuIconDelete },
		.onClick = [=] { Oblivion::Cloud::ShowDeleteData(controller); },
		.keywords = {
			u"delete"_q,
			u"data"_q,
			u"server"_q,
			u"удалить"_q,
			u"данные"_q,
		},
		.shown = Oblivion::Cloud::EnabledValue(session),
	});
	AddToggle(builder, {
		.id = u"oblivion/cloud_links"_q,
		.title = tr::lng_oblivion_cloud_links(),
		.icon = &st::menuIconLink,
		.getter = &Oblivion::Settings::cloudLinks,
		.setter = &Oblivion::Settings::setCloudLinks,
		.keywords = { u"links"_q, u"rooms"_q, u"ссылки"_q, u"комнаты"_q },
	});
	AddSectionEnd(builder, tr::lng_oblivion_cloud_section_about());
}
// Round 5: cloud end.

// Round 5: rooms.
void BuildRoomsSection(SectionBuilder &builder) {
	const auto controller = builder.controller();

	builder.addSubsectionTitle({
		.id = u"oblivion/together"_q,
		.title = tr::lng_oblivion_room_section(),
		.keywords = { u"rooms"_q, u"together"_q, u"комнаты"_q, u"вместе"_q },
	});
	builder.addButton({
		.id = u"oblivion/rooms"_q,
		.title = tr::lng_oblivion_room_settings(),
		.icon = { &st::menuIconGroups },
		.onClick = [=] { Oblivion::Rooms::ShowRoomsBox(controller); },
		.keywords = {
			u"rooms"_q,
			u"listen"_q,
			u"watch"_q,
			u"draw"_q,
			u"комнаты"_q,
			u"музыка"_q,
			u"видео"_q,
			u"холст"_q,
		},
		.shown = SettingsChanges() | rpl::map([] {
			return Oblivion::Get().cloudRooms();
		}),
	});
	AddToggle(builder, {
		.id = u"oblivion/rooms_enabled"_q,
		.title = tr::lng_oblivion_room_settings_enabled(),
		.icon = &st::menuIconInvite,
		.getter = &Oblivion::Settings::cloudRooms,
		.setter = &Oblivion::Settings::setCloudRooms,
		.keywords = { u"rooms"_q, u"links"_q, u"комнаты"_q, u"ссылки"_q },
	});
	// Round 5: room extras.
	AddToggle(builder, {
		.id = u"oblivion/room_reactions"_q,
		.title = tr::lng_oblivion_rextra_settings_reactions(),
		.icon = &st::menuIconReactions,
		.getter = &Oblivion::Settings::roomReactions,
		.setter = &Oblivion::Settings::setRoomReactions,
		.keywords = {
			u"reactions"_q,
			u"stickers"_q,
			u"rooms"_q,
			u"реакции"_q,
			u"стикеры"_q,
		},
	});
	// Round 5: room extras end.
	AddSectionEnd(builder, tr::lng_oblivion_room_settings_about());
}
// Round 5: rooms end.

// Round 5: social.
// What is public about the account (the badge, the chips, who sees the
// profile and the activity, the chosen people) is kept by the server for
// one Telegram account: the rows show what the server has for the account
// of this window and change it by a click through Oblivion::Social, which
// asks the consent to Oblivion Cloud first. Nothing here is app-wide, so
// a switch flipped in one account publishes nothing about the others.
void AddSocialFlag(
		SectionBuilder &builder,
		const QString &id,
		rpl::producer<QString> title,
		const style::icon *icon,
		Oblivion::Social::Flag flag,
		QStringList keywords) {
	const auto controller = builder.controller();
	const auto session = builder.session();
	const auto button = builder.addButton({
		.id = id,
		.title = std::move(title),
		.icon = { icon },
		.onClick = [=] { Oblivion::Social::ToggleFlag(controller, flag); },
		.keywords = std::move(keywords),
	});
	if (button) {
		button->toggleOn(Oblivion::Social::FlagValue(session, flag), true);
	}
}

void AddAudienceRow(
		SectionBuilder &builder,
		const QString &id,
		tr::phrase<> title,
		const style::icon *icon,
		Oblivion::Social::AudienceKind kind) {
	const auto controller = builder.controller();
	const auto session = builder.session();
	builder.addButton({
		.id = id,
		.title = title(),
		.icon = { icon },
		.label = rpl::combine(
			Oblivion::Social::AudienceValue(session, kind),
			tr::lng_oblivion_social_audience_nobody()
		) | rpl::map([](
				Oblivion::Social::Audience audience,
				const QString &) {
			return Oblivion::Social::AudienceName(audience);
		}),
		.onClick = [=] {
			Oblivion::Social::ShowAudienceBox(controller, kind);
		},
		.keywords = {
			u"audience"_q,
			u"privacy"_q,
			u"кто видит"_q,
			u"видимость"_q,
		},
	});
}

void BuildSocialSection(SectionBuilder &builder) {
	const auto controller = builder.controller();

	builder.addSubsectionTitle({
		.id = u"oblivion/social"_q,
		.title = tr::lng_oblivion_social_section(),
		.keywords = {
			u"profile"_q,
			u"friends"_q,
			u"activity"_q,
			u"профиль"_q,
			u"друзья"_q,
			u"активность"_q,
		},
	});
	builder.addButton({
		.id = u"oblivion/social_friends"_q,
		.title = tr::lng_oblivion_social_friends(),
		.icon = { &st::menuIconRatingUsers },
		.onClick = [=] { Oblivion::Social::ShowFriends(controller); },
		.keywords = { u"friends"_q, u"друзья"_q, u"слушает"_q },
	});
	builder.addButton({
		.id = u"oblivion/social_my_profile"_q,
		.title = tr::lng_oblivion_social_my_profile(),
		.icon = { &st::menuIconProfile },
		.onClick = [=] { Oblivion::Social::ShowMyProfile(controller); },
		.keywords = {
			u"profile"_q,
			u"status"_q,
			u"профиль"_q,
			u"статус"_q,
		},
	});
	const auto session = builder.session();
	using SocialFlag = Oblivion::Social::Flag;
	using SocialAudience = Oblivion::Social::AudienceKind;
	AddSocialFlag(
		builder,
		u"oblivion/social_badge"_q,
		tr::lng_oblivion_social_badge(),
		&st::menuIconSigned,
		SocialFlag::Badge,
		{
			u"badge"_q,
			u"mark"_q,
			u"cloud"_q,
			u"значок"_q,
			u"галочка"_q,
			u"облако"_q,
		});

	// Only while the bio of this account still has the invisible marker
	// of the round 4 badge: it is removed by this click and no other way.
	builder.addButton({
		.id = u"oblivion/social_marker_remove"_q,
		.title = tr::lng_oblivion_social_marker_remove(),
		.icon = { &st::menuIconDelete },
		.onClick = [=] { Oblivion::Social::ShowRemoveMarker(controller); },
		.keywords = { u"bio"_q, u"mark"_q, u"о себе"_q, u"метка"_q },
		.shown = Oblivion::Badge::OldMarkerValue(session),
	});
	AddAudienceRow(
		builder,
		u"oblivion/social_profile_audience"_q,
		tr::lng_oblivion_social_profile_audience,
		&st::menuIconPermissions,
		SocialAudience::Profile);
	AddAudienceRow(
		builder,
		u"oblivion/social_activity_audience"_q,
		tr::lng_oblivion_social_activity_audience,
		&st::menuIconLock,
		SocialAudience::Activity);
	builder.addButton({
		.id = u"oblivion/social_chosen"_q,
		.title = tr::lng_oblivion_social_chosen(),
		.icon = { &st::menuIconInvite },
		.label = Oblivion::Social::ChosenCountValue(
			session
		) | rpl::map([](int count) {
			return count ? QString::number(count) : QString();
		}),
		.onClick = [=] { Oblivion::Social::ShowChosenList(controller); },
		.keywords = { u"chosen"_q, u"people"_q, u"выбранные"_q, u"люди"_q },
	});
	AddSocialFlag(
		builder,
		u"oblivion/social_chip_listening"_q,
		tr::lng_oblivion_social_chip_listening(),
		&st::menuIconSoundOn,
		SocialFlag::ChipListening,
		{ u"listening"_q, u"music"_q, u"слушаю"_q, u"музыка"_q });
	AddSocialFlag(
		builder,
		u"oblivion/social_chip_room"_q,
		tr::lng_oblivion_social_chip_room(),
		&st::menuIconChatBubble,
		SocialFlag::ChipRoom,
		{ u"room"_q, u"activity"_q, u"комната"_q });
	AddSocialFlag(
		builder,
		u"oblivion/social_chip_online"_q,
		tr::lng_oblivion_social_chip_online(),
		&st::menuIconWhenOnline,
		SocialFlag::ChipOnline,
		{ u"online"_q, u"activity"_q, u"в сети"_q });
	AddToggle(builder, {
		.id = u"oblivion/social_badge_show"_q,
		.title = tr::lng_oblivion_social_badge_show(),
		.icon = &st::menuIconUserShow,
		.getter = &Oblivion::Settings::cloudBadgeShow,
		.setter = &Oblivion::Settings::setCloudBadgeShow,
		.keywords = { u"badge"_q, u"show"_q, u"значки"_q },
	});
	AddToggle(builder, {
		.id = u"oblivion/social_profile_show"_q,
		.title = tr::lng_oblivion_social_profile_show(),
		.icon = &st::menuIconShowInChat,
		.getter = &Oblivion::Settings::cloudProfileShow,
		.setter = &Oblivion::Settings::setCloudProfileShow,
		.keywords = {
			u"profiles"_q,
			u"activity"_q,
			u"профили"_q,
			u"активность"_q,
		},
	});
	AddToggle(builder, {
		.id = u"oblivion/social_friends_menu"_q,
		.title = tr::lng_oblivion_social_friends_menu(),
		.icon = &st::menuIconManage,
		.getter = &Oblivion::Settings::cloudFriends,
		.setter = &Oblivion::Settings::setCloudFriends,
		.keywords = { u"friends"_q, u"menu"_q, u"друзья"_q, u"меню"_q },
	});
	AddSectionEnd(builder, tr::lng_oblivion_social_settings_about());
}
// Round 5: social end.

void BuildSyncSection(SectionBuilder &builder) {
	const auto controller = builder.controller();

	builder.addSubsectionTitle({
		.id = u"oblivion/sync"_q,
		.title = tr::lng_oblivion_sync_section(),
		.keywords = {
			u"sync"_q,
			u"update"_q,
			u"синхронизация"_q,
			u"обновления"_q,
		},
	});
	// Round 5: sync.
	// Sharing has no rows of its own in the scaffold: they open the two
	// lists of oblivion_cloud_share.h (the consent is asked on the click).
	builder.addButton({
		.id = u"oblivion/share_playlists"_q,
		.title = tr::lng_oblivion_share_library(),
		.icon = { &st::menuIconSoundOn },
		.onClick = [=] { Oblivion::Share::ShowLibrary(controller); },
		.keywords = {
			u"share"_q,
			u"playlists"_q,
			u"плейлисты"_q,
			u"поделиться"_q,
		},
	});
	builder.addButton({
		.id = u"oblivion/share_presets"_q,
		.title = tr::lng_oblivion_share_gallery_title(),
		.icon = { &st::menuIconPalette },
		.onClick = [=] { Oblivion::Share::ShowGallery(controller); },
		.keywords = {
			u"share"_q,
			u"presets"_q,
			u"gallery"_q,
			u"наборы"_q,
			u"эффекты"_q,
		},
	});
	builder.addButton({
		.id = u"oblivion/sync_settings"_q,
		.title = tr::lng_oblivion_sync_settings(),
		.icon = { &st::menuIconRestore },
		.onClick = [=] { Oblivion::Sync::ShowBox(controller); },
		.keywords = {
			u"sync"_q,
			u"settings"_q,
			u"devices"_q,
			u"синхронизация"_q,
			u"настройки"_q,
		},
	});
	AddCloudToggle(builder, {
		.id = u"oblivion/sync_auto"_q,
		.title = tr::lng_oblivion_sync_settings_auto(),
		.icon = &st::menuIconReschedule,
		.getter = &Oblivion::Settings::cloudSettingsAutoSync,
		.setter = &Oblivion::Settings::setCloudSettingsAutoSync,
		.keywords = { u"sync"_q, u"auto"_q, u"автоматически"_q },
	});
	// Round 5: sync end.
	// Round 5: update.
	builder.addButton({
		.id = u"oblivion/update_check"_q,
		.title = tr::lng_oblivion_update_settings_check(),
		.icon = { &st::menuIconDownload },
		.onClick = [=] { Oblivion::Update::CheckNow(controller); },
		.keywords = {
			u"update"_q,
			u"version"_q,
			u"обновления"_q,
			u"версия"_q,
		},
	});
	AddToggle(builder, {
		.id = u"oblivion/update_auto"_q,
		.title = tr::lng_oblivion_update_settings_auto(),
		.icon = &st::menuIconSchedule,
		.getter = &Oblivion::Settings::cloudUpdateCheck,
		.setter = &Oblivion::Settings::setCloudUpdateCheck,
		.keywords = { u"update"_q, u"auto"_q, u"обновления"_q },
	});
	// Round 5: update end.
	AddSectionEnd(builder, tr::lng_oblivion_update_settings_about());
}

// Round 5: send online.
[[nodiscard]] QString SendOnlineLimitName(int hours) {
	return (hours >= 48 && !(hours % 24))
		? tr::lng_oblivion_sendonline_days(tr::now, lt_count, hours / 24)
		: tr::lng_oblivion_sendonline_hours(tr::now, lt_count, hours);
}

void SendOnlineLimitBox(not_null<Ui::GenericBox*> box) {
	const auto values = std::vector<int>{ 1, 6, 24, 72, 168 };
	const auto current = Oblivion::Get().sendWhenOnlineHours();
	auto options = std::vector<QString>();
	auto selected = 2;
	for (const auto hours : values) {
		if (hours == current) {
			selected = int(options.size());
		}
		options.push_back(SendOnlineLimitName(hours));
	}
	SingleChoiceBox(box, {
		.title = tr::lng_oblivion_sendonline_settings_limit(),
		.options = options,
		.initialSelection = selected,
		.callback = [=](int index) {
			if (index >= 0 && index < int(values.size())) {
				Oblivion::Get().setSendWhenOnlineHours(values[index]);
			}
		},
	});
}

void BuildSendOnlineSection(SectionBuilder &builder) {
	const auto controller = builder.controller();

	builder.addSubsectionTitle({
		.id = u"oblivion/send_online"_q,
		.title = tr::lng_oblivion_sendonline_section(),
		.keywords = {
			u"send"_q,
			u"online"_q,
			u"отправить"_q,
			u"в сети"_q,
		},
	});
	AddToggle(builder, {
		.id = u"oblivion/send_online_enabled"_q,
		.title = tr::lng_oblivion_sendonline_settings(),
		.icon = &st::menuIconSend,
		.getter = &Oblivion::Settings::sendWhenOnline,
		.setter = &Oblivion::Settings::setSendWhenOnline,
		.keywords = {
			u"send"_q,
			u"online"_q,
			u"later"_q,
			u"отправить"_q,
			u"в сети"_q,
			u"позже"_q,
		},
	});
	builder.addButton({
		.id = u"oblivion/send_online_limit"_q,
		.title = tr::lng_oblivion_sendonline_settings_limit(),
		.icon = { &st::menuIconSchedule },
		.label = rpl::merge(
			SettingsChanges(),
			tr::lng_oblivion_sendonline_settings_limit() | rpl::to_empty
		) | rpl::map([] {
			return SendOnlineLimitName(
				Oblivion::Get().sendWhenOnlineHours());
		}),
		.onClick = [=] { controller->show(Box(SendOnlineLimitBox)); },
		.keywords = { u"limit"_q, u"wait"_q, u"срок"_q, u"ждать"_q },
	});
	builder.addButton({
		.id = u"oblivion/send_online_list"_q,
		.title = tr::lng_oblivion_sendonline_settings_list(),
		.icon = { &st::menuIconWhenOnline },
		.onClick = [=] { Oblivion::SendOnline::ShowList(controller); },
		.keywords = {
			u"waiting"_q,
			u"queue"_q,
			u"ожидают"_q,
			u"очередь"_q,
		},
	});
	AddSectionEnd(builder, tr::lng_oblivion_sendonline_settings_about());
}
// Round 5: send online end.

void BuildOblivionSectionContent(SectionBuilder &builder) {
	// Round 5.
	BuildCloudSection(builder);
	BuildRoomsSection(builder);
	BuildSocialSection(builder);
	BuildSyncSection(builder);
	BuildSendOnlineSection(builder);
	// Round 5 end.
	BuildGhostSection(builder);
	BuildTrackingSection(builder);
	BuildSavingSection(builder);
	BuildProtectedSection(builder);
	BuildVoiceSection(builder);
	BuildToolsSection(builder);
	BuildInterfaceSection(builder);
	BuildVisualSection(builder);
}

class OblivionSection : public Section<OblivionSection> {
public:
	OblivionSection(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();

};

const auto kMeta = BuildHelper({
	.id = OblivionSection::Id(),
	.parentId = MainId(),
	.title = &tr::lng_settings_section_oblivion,
	.icon = &st::menuIconInfo,
}, [](SectionBuilder &builder) {
	BuildOblivionSectionContent(builder);
});

const SectionBuildMethod kOblivionSection = kMeta.build;

OblivionSection::OblivionSection(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

rpl::producer<QString> OblivionSection::title() {
	return tr::lng_settings_section_oblivion();
}

void OblivionSection::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	build(content, kOblivionSection);

	Ui::ResizeFitChild(this, content);
}

} // namespace

Type OblivionId() {
	return OblivionSection::Id();
}

} // namespace Settings
