/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_lang.h"

#include "lang/lang_instance.h"

namespace Oblivion {
namespace {

const LangOverride kCommon[] = {
	{ "lng_settings_section_oblivion", "Oblivion" },
	{ "lng_oblivion_privacy", "Конфиденциальность" },
	{ "lng_oblivion_content", "Контент" },
	{ "lng_oblivion_interface", "Интерфейс" },
	{ "lng_oblivion_visual", "Только на вашем экране" },
	{
		"lng_oblivion_visual_about",
		"Эти изменения видны только на вашем экране "
		"и никуда не отправляются.",
	},
	{ "lng_oblivion_ghost_read", "Не отправлять отметки о прочтении" },
	{ "lng_oblivion_ghost_typing", "Не отправлять статус «печатает»" },
	{ "lng_oblivion_ghost_online", "Не обновлять статус «в сети»" },
	{ "lng_oblivion_hide_sponsored", "Скрывать рекламные сообщения" },
	{ "lng_oblivion_keep_deleted", "Сохранять удалённые сообщения" },
	{
		"lng_oblivion_copy_protected",
		"Разрешить копирование в защищённых чатах",
	},
	{
		"lng_oblivion_save_protected",
		"Разрешить сохранение медиа из защищённых чатов",
	},
	{ "lng_oblivion_stories_premium", "Скачивать истории без Premium" },
	{ "lng_oblivion_sticker_json", "Экспорт стикеров в JSON" },
	{ "lng_oblivion_peer_ids", "Показывать ID в профилях" },
	{ "lng_oblivion_registration", "Показывать дату регистрации" },
	{
		"lng_oblivion_message_details",
		"Подробности сообщения в контекстном меню",
	},
	{ "lng_oblivion_fake_premium", "Показывать Premium активным" },
	{
		"lng_oblivion_visual_gifting",
		"Дарить подарки визуально (без запроса к серверу)",
	},
	{ "lng_oblivion_fake_stars", "Баланс звёзд" },
	{ "lng_oblivion_fake_ton", "Баланс TON" },
	{ "lng_oblivion_copy_backdrop", "Скопировать цвета фона" },
	{ "lng_oblivion_save_model", "Сохранить модель подарка" },
	{ "lng_oblivion_save_pattern", "Сохранить узор подарка" },
	{ "lng_oblivion_gift_add_visual", "Добавить в профиль (визуально)" },
	{ "lng_oblivion_gift_remove_visual", "Убрать визуальный подарок" },
	{ "lng_oblivion_gift_sent_visual", "Вы отправили подарок для {user}" },
	{
		"lng_oblivion_gift_not_loaded",
		"Ещё не загружено. Сначала откройте подарок.",
	},
	{ "lng_oblivion_backdrop_copied", "Цвета фона скопированы." },
	{
		"lng_oblivion_gift_added_visual",
		"Добавлено визуально. Видно только вам.",
	},
	{
		"lng_oblivion_gift_removed_visual",
		"Удалено. Откройте профиль заново, чтобы он обновился.",
	},
	{
		"lng_oblivion_premium_gifted_visual",
		"Premium подарен визуально. Видно только вам.",
	},
	{
		"lng_oblivion_gift_sent_visual_toast",
		"Подарок отправлен визуально. Видно только вам.",
	},
	{
		"lng_oblivion_gift_bought_visual",
		"Куплено визуально. Видно только вам.",
	},
	{ "lng_oblivion_sticker_not_loaded", "Стикер ещё не загружен." },
	{ "lng_oblivion_sticker_read_failed", "Не удалось прочитать анимацию." },
	{ "lng_oblivion_sticker_render_failed", "Не удалось отрисовать кадр." },
	{ "lng_oblivion_sticker_unpack_failed", "Не удалось распаковать стикер." },
	{
		"lng_oblivion_sticker_not_lottie",
		"Этот стикер не является Lottie-анимацией.",
	},
	{ "lng_oblivion_sticker_frame_title", "Сохранить кадр стикера" },
	{ "lng_oblivion_sticker_hue_title", "Сохранить перекрашенный стикер" },
	{ "lng_oblivion_file_png", "Изображение PNG" },
	{ "lng_oblivion_file_lottie", "Анимация Lottie" },
	{ "lng_oblivion_file_json", "Файл JSON" },
	{ "lng_oblivion_file_tgs", "Стикер Telegram" },
	{ "lng_oblivion_saved_to", "Сохранено: {path}" },
	{ "lng_oblivion_write_failed", "Не удалось записать файл." },
	{ "lng_oblivion_details_id", "ID" },
	{ "lng_oblivion_details_date", "Дата" },
	{ "lng_oblivion_details_edited", "Изменено" },
	{ "lng_oblivion_details_from", "Отправитель" },
	{ "lng_oblivion_details_chat", "Чат" },
	{ "lng_oblivion_details_forwarded", "Пересланное сообщение" },
	{ "lng_oblivion_details_original_id", "Исходный ID" },
	{ "lng_oblivion_details_original_date", "Исходная дата" },
	{ "lng_oblivion_details_author", "Автор" },
	{ "lng_oblivion_details_hidden", "скрыт" },
	{ "lng_oblivion_details_source", "Источник" },
	{
		"lng_oblivion_details_imported",
		"Импортировано из другого приложения",
	},
	{ "lng_profile_copy_id", "Копировать ID" },
	{ "lng_info_id_label", "ID" },
	{ "lng_info_registration_label", "Дата регистрации" },
	{ "lng_context_message_details", "Подробности сообщения" },
	{ "lng_context_save_sticker_json", "Сохранить стикер в JSON" },
	{ "lng_context_sticker_hue", "Сохранить со сдвигом оттенка" },
	{ "lng_context_sticker_frame", "Сохранить кадр в PNG" },
	{
		"lng_settings_add_account_about",
		"Вы можете добавить до 20 аккаунтов с разными номерами телефонов.",
	},
};

const LangOverride kGhost[] = {
	{ "lng_oblivion_ghost_section", "Режим призрака" },
	{ "lng_oblivion_ghost_read_exceptions", "Исключения" },
	{
		"lng_oblivion_ghost_read_exceptions_title",
		"Исключения для отметок о прочтении",
	},
	{
		"lng_oblivion_ghost_read_exceptions_about",
		"Исключения меняют настройку отметок о прочтении "
		"на противоположную для выбранных чатов: пока режим призрака "
		"включён, эти чаты читаются как обычно, а пока выключен — "
		"невидимо. Добавить чат можно через его контекстное меню "
		"в списке чатов.",
	},
	{ "lng_oblivion_ghost_read_exceptions_empty", "Исключений пока нет." },
	{
		"lng_oblivion_ghost_read_exceptions_clear",
		"Удалить все исключения",
	},
	{
		"lng_oblivion_ghost_read_exceptions_clear_sure",
		"Вы уверены, что хотите удалить все исключения?",
	},
	{ "lng_oblivion_menu_ghost_read_on", "Читать этот чат невидимо" },
	{ "lng_oblivion_menu_ghost_read_off", "Читать этот чат как обычно" },
	{
		"lng_oblivion_ghost_read_on_toast",
		"Сообщения в этом чате будут читаться невидимо.",
	},
	{
		"lng_oblivion_ghost_read_off_toast",
		"Сообщения в этом чате будут читаться как обычно.",
	},
	{ "lng_oblivion_ghost_stories", "Смотреть истории невидимо" },
	{
		"lng_oblivion_offline_send",
		"Отправлять сообщения, не появляясь в сети",
	},
	{
		"lng_oblivion_offline_send_about",
		"Сообщения отправляются как отложенные через несколько секунд, "
		"поэтому вы не появляетесь в сети.",
	},
	{
		"lng_oblivion_read_on_send",
		"Отмечать чат прочитанным при отправке",
	},
};

const LangOverride kSaving[] = {
	{ "lng_oblivion_saving_section", "Сохранение" },
	{
		"lng_oblivion_saving_about",
		"Удалённые сообщения и история изменений хранятся "
		"только на этом устройстве.",
	},
	{ "lng_oblivion_keep_edit_history", "Сохранять историю изменений" },
	{ "lng_oblivion_deleted_title", "Удалённые сообщения" },
	{ "lng_oblivion_deleted_empty", "Удалённых сообщений пока нет." },
	{ "lng_oblivion_deleted_clear", "Очистить список" },
	{
		"lng_oblivion_deleted_clear_sure",
		"Вы уверены, что хотите удалить все сохранённые удалённые "
		"сообщения? Это действие нельзя отменить.",
	},
	{ "lng_oblivion_deleted_menu", "Удалённые сообщения" },
	{
		"lng_oblivion_deleted_clear_chat_sure",
		"Вы уверены, что хотите удалить сохранённые удалённые сообщения "
		"из этого чата? Это действие нельзя отменить.",
	},
	{ "lng_oblivion_deleted_at", "удалено {date}" },
	{ "lng_oblivion_deleted_media_missing", "файл не сохранён" },
	{ "lng_oblivion_edit_history", "История изменений" },
	{ "lng_oblivion_save_self_destructing", "Сохранять исчезающие медиа" },
	{
		"lng_oblivion_self_destructing_saved",
		"Исчезающее медиа сохранено: {path}",
	},
	{
		"lng_oblivion_self_destructing_failed",
		"Не удалось сохранить исчезающее медиа.",
	},
	{ "lng_oblivion_self_destructing_open", "Открыть сохранённую копию" },
	{
		"lng_oblivion_self_destructing_folder",
		"Показать сохранённую копию в папке",
	},
	{
		"lng_oblivion_self_destructing_finder",
		"Показать сохранённую копию в Finder",
	},
};

const LangOverride kProtected[] = {
	{ "lng_oblivion_protected_section", "Защищённый контент" },
	{
		"lng_oblivion_forward_protected",
		"Пересылать из защищённых чатов копией",
	},
	{
		"lng_oblivion_forward_protected_about",
		"Сообщения из чатов, где запрещена пересылка, отправляются "
		"заново как новые — без пометки о пересылке.",
	},
	{ "lng_oblivion_forward_copy_progress", "Отправка копии…" },
	{
		"lng_oblivion_forward_copy_failed",
		"Не удалось отправить копию этого сообщения.",
	},
};

const LangOverride kVoice[] = {
	{ "lng_oblivion_voice_section", "Голосовые сообщения" },
	{
		"lng_oblivion_local_transcribe",
		"Расшифровывать голосовые без Premium",
	},
	{
		"lng_oblivion_local_transcribe_about",
		"Используется распознавание речи macOS — на этом Mac, когда это "
		"возможно, иначе через серверы Apple. В Telegram ничего "
		"не отправляется.",
	},
	{ "lng_oblivion_transcribe_language", "Язык распознавания" },
	{ "lng_oblivion_transcribe_language_auto", "Как в интерфейсе" },
	{
		"lng_oblivion_transcribe_denied",
		"Разрешите Oblivion распознавание речи в Системных настройках → "
		"Конфиденциальность и безопасность.",
	},
	{ "lng_oblivion_transcribe_failed", "Не удалось распознать речь." },
	{
		"lng_oblivion_transcribe_partial",
		"Не удалось распознать часть этого сообщения.",
	},
	{
		"lng_oblivion_transcribe_no_speech",
		"В этом сообщении не обнаружена речь.",
	},
	{
		"lng_oblivion_transcribe_unavailable",
		"Распознавание речи недоступно для этого языка.",
	},
};

const LangOverride kInterface[] = {
	{
		"lng_oblivion_show_seconds",
		"Показывать секунды во времени сообщений",
	},
	{ "lng_oblivion_hide_stories_bar", "Скрывать панель историй" },
	{ "lng_oblivion_hide_premium_promo", "Скрывать предложения Premium" },
	{ "lng_oblivion_hide_gift_promo", "Скрывать предложения подарков" },
};

const LangOverride kAppIcon[] = {
	{ "lng_oblivion_app_icon", "Иконка приложения" },
	{ "lng_oblivion_app_icon_default", "Oblivion" },
	{ "lng_oblivion_app_icon_telegram", "Telegram" },
	{ "lng_oblivion_app_icon_previous", "Прежняя" },
	{ "lng_oblivion_app_icon_eclipse", "Затмение" },
	{ "lng_oblivion_app_icon_portal", "Портал" },
	{ "lng_oblivion_app_icon_ghost", "Призрак" },
	{ "lng_oblivion_app_icon_shadow", "Тень" },
	{ "lng_oblivion_app_icon_horizon", "Горизонт" },
	{ "lng_oblivion_app_icon_custom", "Своё изображение…" },
	{ "lng_oblivion_app_icon_reset", "Вернуть стандартную" },
	{ "lng_oblivion_app_icon_custom_short", "Своя" },
	{ "lng_oblivion_app_icon_filter", "Изображения" },
	{
		"lng_oblivion_app_icon_bad_image",
		"Не удалось открыть это изображение.",
	},
	{ "lng_oblivion_app_icon_finder", "Менять также в Finder и Launchpad" },
	{
		"lng_oblivion_app_icon_finder_about",
		"Без этой настройки иконка меняется только в Dock, пока Oblivion "
		"запущен. С ней иконка запишется прямо в приложение, а Dock "
		"перезапустится.",
	},
	{
		"lng_oblivion_app_icon_finder_done",
		"Иконка в Finder обновлена, Dock перезапускается…",
	},
	{
		"lng_oblivion_app_icon_finder_cleared",
		"Иконка в Finder сброшена, Dock перезапускается…",
	},
	{
		"lng_oblivion_app_icon_finder_failed",
		"Не удалось сменить иконку в Finder. Подробности — в log.txt.",
	},
};

const LangOverride kLocalNames[] = {
	{ "lng_oblivion_local_name_set", "Задать локальное имя" },
	{ "lng_oblivion_local_name_edit", "Изменить локальное имя" },
	{ "lng_oblivion_local_name_reset", "Сбросить локальное имя" },
	{ "lng_oblivion_local_name_title", "Локальное имя" },
	{ "lng_oblivion_local_name_about", "Это имя будете видеть только вы." },
	{ "lng_oblivion_local_name_original", "Настоящее имя" },
};

// Each tool appends its rows after its own marker comment.
const LangOverride kTools[] = {
	{ "lng_oblivion_tools_section", "Инструменты" },
	{ "lng_oblivion_tools_sticker_studio", "Редактор стикеров" },
	{ "lng_oblivion_tools_gift_catalog", "Каталог подарков" },
	{ "lng_oblivion_tools_music_editor", "Музыкальный редактор" },
	{ "lng_oblivion_tools_playlists", "Плейлисты" },
	{ "lng_oblivion_tools_voice_effect", "Эффект для голосовых" },
	{ "lng_oblivion_tools_voice_effect_none", "Нет" },
	{
		"lng_oblivion_tools_about",
		"Каталог подарков — данные @GiftChanges (api.changes.tg).",
	},
	// Tools: lottie core.
	{ "lng_oblivion_lottie_svg_raster", "Некоторые эффекты этого кадра нельзя передать векторно, поэтому кадр встроен в SVG как изображение." },
	// Tools: audio core.
	// Tools: sticker studio.
	{ "lng_oblivion_studio_open_menu", "Открыть в редакторе стикеров" },
	{ "lng_oblivion_studio_gift_model", "Модель в редакторе" },
	{ "lng_oblivion_studio_gift_pattern", "Узор в редакторе" },
	{ "lng_oblivion_studio_open_file", "Открыть файл…" },
	{ "lng_oblivion_studio_open_title", "Открыть анимацию Lottie" },
	{ "lng_oblivion_studio_filter", "Анимации Lottie" },
	{ "lng_oblivion_studio_filter_all", "Все файлы" },
	{ "lng_oblivion_studio_downloading", "Загрузка… {percent}" },
	{ "lng_oblivion_studio_opening", "Открытие…" },
	{ "lng_oblivion_studio_download_failed", "Не удалось загрузить стикер." },
	{
		"lng_oblivion_studio_unsupported",
		"В редакторе открываются только анимированные стикеры Lottie "
		"(.tgs). Видеостикеры и статичные стикеры не поддерживаются.",
	},
	{
		"lng_oblivion_studio_bad_file",
		"Этот файл не является анимацией Lottie или повреждён.",
	},
	{ "lng_oblivion_studio_read_failed", "Не удалось прочитать файл." },
	{ "lng_oblivion_studio_info", "{size} · {fps} кадр/с · {duration}" },
	{ "lng_oblivion_studio_seconds", "{value} с" },
	{ "lng_oblivion_studio_frame", "Кадр {index} / {total}" },
	{ "lng_oblivion_studio_background", "Фон превью" },
	{ "lng_oblivion_studio_bg_transparent", "Прозрачный" },
	{ "lng_oblivion_studio_bg_dark", "Тёмный" },
	{ "lng_oblivion_studio_bg_light", "Светлый" },
	{ "lng_oblivion_studio_colors", "Цвета" },
	{ "lng_oblivion_studio_hue", "Оттенок" },
	{ "lng_oblivion_studio_saturation", "Насыщенность" },
	{ "lng_oblivion_studio_lightness", "Яркость" },
	{ "lng_oblivion_studio_reset", "Сбросить" },
	{ "lng_oblivion_studio_full_editor", "Полный редактор" },
	{ "lng_oblivion_studio_save_animation", "Сохранить анимацию" },
	{ "lng_oblivion_studio_save_tgs", "Стикер Telegram (.tgs)" },
	{ "lng_oblivion_studio_save_json", "Анимация Lottie (.json)" },
	{ "lng_oblivion_studio_save_frame", "Сохранить текущий кадр" },
	{ "lng_oblivion_studio_png_size", "Размер PNG" },
	{ "lng_oblivion_studio_size_original", "Исходный" },
	{ "lng_oblivion_studio_save_png", "Изображение PNG" },
	{ "lng_oblivion_studio_save_svg", "Векторное изображение SVG" },
	{ "lng_oblivion_studio_file_svg", "Изображение SVG" },
	{
		"lng_oblivion_studio_about",
		"Изменения цветов применяются ко всем сохраняемым файлам. PNG и SVG "
		"сохраняют кадр, показанный в превью. Файл .tgs или .json можно "
		"просто перетащить в это окно. Двойной щелчок по ползунку "
		"сбрасывает его.",
	},
	{
		"lng_oblivion_studio_export_failed",
		"Не удалось экспортировать анимацию.",
	},
	{
		"lng_oblivion_studio_tgs_too_large",
		"Telegram не воспроизводит файлы .tgs, в которых больше 2 МБ "
		"данных анимации.",
	},
	{ "lng_oblivion_studio_drop", "Отпустите файл, чтобы открыть его" },
	// Tools: gift catalog.
	{ "lng_oblivion_gifts_search", "Поиск подарков" },
	{ "lng_oblivion_gifts_search_items", "Поиск по названию" },
	{ "lng_oblivion_gifts_loading", "Загрузка…" },
	{ "lng_oblivion_gifts_retry", "Повторить" },
	{ "lng_oblivion_gifts_nothing_found", "Ничего не найдено." },
	{ "lng_oblivion_gifts_list_empty", "Список пуст." },
	{
		"lng_oblivion_gifts_error_network",
		"Не удалось подключиться к api.changes.tg. Проверьте подключение "
		"к интернету или настройки прокси.",
	},
	{
		"lng_oblivion_gifts_error_timeout",
		"Сервер каталога не отвечает. Попробуйте ещё раз.",
	},
	{
		"lng_oblivion_gifts_error_not_found",
		"Этот подарок не найден в каталоге.",
	},
	{ "lng_oblivion_gifts_error_file", "В каталоге нет такого файла." },
	{
		"lng_oblivion_gifts_error_server",
		"Сервер каталога временно недоступен. Попробуйте позже.",
	},
	{
		"lng_oblivion_gifts_error_data",
		"Не удалось прочитать ответ сервера каталога.",
	},
	{ "lng_oblivion_gifts_tab_models", "Модели" },
	{ "lng_oblivion_gifts_tab_patterns", "Узоры" },
	{ "lng_oblivion_gifts_tab_backdrops", "Фоны" },
	{ "lng_oblivion_gifts_released", "Выпущен {date}" },
	{
		"lng_oblivion_gifts_counts",
		"Модели: {models} · Узоры: {patterns} · Фоны: {backdrops}",
	},
	{ "lng_oblivion_gifts_open_editor", "Открыть в редакторе стикеров" },
	{ "lng_oblivion_gifts_save_tgs", "Сохранить в .tgs" },
	{ "lng_oblivion_gifts_save_json", "Сохранить в Lottie JSON" },
	{ "lng_oblivion_gifts_save_png", "Сохранить в PNG" },
	{ "lng_oblivion_gifts_copy_name", "Скопировать название" },
	{ "lng_oblivion_gifts_name_copied", "Название скопировано." },
	{ "lng_oblivion_gifts_copy_colors", "Скопировать все цвета" },
	{ "lng_oblivion_gifts_color_center", "Центр" },
	{ "lng_oblivion_gifts_color_edge", "Край" },
	{ "lng_oblivion_gifts_color_pattern", "Узор" },
	{ "lng_oblivion_gifts_color_text", "Текст" },
	{ "lng_oblivion_gifts_color_copied", "Цвет {color} скопирован." },
	{ "lng_oblivion_gifts_downloading", "Скачивание…" },
	{ "lng_oblivion_gifts_attribution", "Данные: {link} (api.changes.tg)" },
	{ "lng_oblivion_gifts_all_models", "Все модели этого подарка" },
	// Tools: voice changer.
	{ "lng_oblivion_voice_effect_off", "Без эффекта" },
	{ "lng_oblivion_voice_effect_chipmunk", "Бурундук" },
	{ "lng_oblivion_voice_effect_giant", "Великан" },
	{ "lng_oblivion_voice_effect_robot", "Робот" },
	{ "lng_oblivion_voice_effect_echo", "Эхо" },
	{ "lng_oblivion_voice_effect_telephone", "Телефон" },
	{ "lng_oblivion_voice_effect_cave", "Пещера" },
	{ "lng_oblivion_voice_effect_custom", "Своя высота тона" },
	{ "lng_oblivion_voice_effect_custom_value", "Высота тона {value}" },
	{ "lng_oblivion_voice_effect_custom_menu", "Своя высота тона…" },
	{ "lng_oblivion_voice_effect_pitch", "Сдвиг в полутонах: {value}" },
	{ "lng_oblivion_voice_effect_pitch_title", "Сдвиг в полутонах" },
	{
		"lng_oblivion_voice_effect_about",
		"Эффект применяется к голосовым сообщениям перед отправкой, "
		"видеосообщения не меняются. Чтобы быстро сменить эффект, нажмите "
		"правой кнопкой мыши на значок микрофона в чате.",
	},
	{ "lng_oblivion_voice_effect_sample", "Проба" },
	// No-break spaces ( ) keep «с» and «не» off the line ends.
	{
		"lng_oblivion_voice_effect_sample_about",
		"Запишите короткую фразу, чтобы услышать её с выбранным "
		"эффектом. Проба никуда не отправляется.",
	},
	{ "lng_oblivion_voice_effect_record", "Записать" },
	{ "lng_oblivion_voice_effect_stop", "Остановить" },
	{ "lng_oblivion_voice_effect_play", "Прослушать" },
	{ "lng_oblivion_voice_effect_pause", "Пауза" },
	{ "lng_oblivion_voice_effect_recording", "Идёт запись… {duration}" },
	{ "lng_oblivion_voice_effect_processing", "Обработка…" },
	{ "lng_oblivion_voice_effect_ready", "Записано: {duration}" },
	{
		"lng_oblivion_voice_effect_too_short",
		"Запись слишком короткая, говорите хотя бы секунду.",
	},
	{
		"lng_oblivion_voice_effect_record_failed",
		"Не удалось записать звук с микрофона.",
	},
	{
		"lng_oblivion_voice_effect_mic_busy",
		"Микрофон занят другой записью.",
	},
	{ "lng_oblivion_voice_effect_mic_unavailable", "Микрофон не найден." },
	{
		"lng_oblivion_voice_effect_menu_setup",
		"Настроить и прослушать…",
	},
	{
		"lng_oblivion_voice_effect_chosen",
		"Эффект для голосовых: {effect}",
	},
	{
		"lng_oblivion_voice_effect_disabled",
		"Голосовые будут отправляться без эффекта.",
	},
	{
		"lng_oblivion_voice_effect_applying",
		"Применяем эффект к голосовому…",
	},
	{
		"lng_oblivion_voice_effect_failed",
		"Не удалось применить эффект, голосовое отправлено без него.",
	},
	{
		"lng_oblivion_voice_effect_preview_failed",
		"Не удалось применить эффект к записи.",
	},
	{
		"lng_oblivion_voice_effect_too_long",
		"Эффекты работают только для голосовых длиной до 15 минут, "
		"это останется без эффекта.",
	},
	// Tools: round video.
	{ "lng_oblivion_round_send_as", "Отправить кружком" },
	{ "lng_oblivion_round_context", "Отправить как кружок" },
	{ "lng_oblivion_round_title", "Видеосообщение" },
	{ "lng_oblivion_round_downloading", "Загрузка видео… {percent}" },
	{ "lng_oblivion_round_opening", "Открываем видео…" },
	{ "lng_oblivion_round_converting", "Конвертация… {percent}" },
	{
		"lng_oblivion_round_range",
		"Фрагмент {from} – {till}, длительность {duration}",
	},
	{
		"lng_oblivion_round_limit",
		"Кружок может длиться не больше 60 секунд. Потяните края полосы, "
		"чтобы выбрать фрагмент.",
	},
	{
		"lng_oblivion_round_position",
		"Положение кадра: перетащите круг или ползунок.",
	},
	{ "lng_oblivion_round_mute", "Без звука" },
	{ "lng_oblivion_round_open_failed", "Не удалось открыть это видео." },
	{ "lng_oblivion_round_download_failed", "Не удалось загрузить видео." },
	{
		"lng_oblivion_round_convert_failed",
		"Не удалось конвертировать видео.",
	},
	{ "lng_oblivion_round_too_short", "Видео слишком короткое." },
	{
		"lng_oblivion_round_restricted",
		"В этот чат нельзя отправлять видеосообщения.",
	},
	{ "lng_oblivion_round_choose", "Выберите видео" },
	{ "lng_oblivion_round_filter_video", "Видеофайлы" },
	{ "lng_oblivion_round_filter_all", "Все файлы" },
	// Tools: music editor.
	{ "lng_oblivion_music_open_in", "Открыть в музыкальном редакторе" },
	{ "lng_oblivion_music_tracks", "Треки" },
	{ "lng_oblivion_music_open_file", "Открыть файл…" },
	{ "lng_oblivion_music_add_track", "Добавить трек…" },
	{
		"lng_oblivion_music_tracks_about",
		"Треки склеиваются в этом порядке. Нажмите на трек, чтобы переместить или убрать его.",
	},
	{ "lng_oblivion_music_choose", "Выберите аудио- или видеофайлы" },
	{ "lng_oblivion_music_filter_media", "Аудио и видео" },
	{ "lng_oblivion_music_filter_all", "Все файлы" },
	{ "lng_oblivion_music_move_up", "Переместить выше" },
	{ "lng_oblivion_music_move_down", "Переместить ниже" },
	{ "lng_oblivion_music_remove", "Убрать" },
	{ "lng_oblivion_music_downloading", "Загрузка… {percent}" },
	{ "lng_oblivion_music_decoding", "Открытие…" },
	{
		"lng_oblivion_music_open_failed",
		"Не удалось открыть файл: в нём нет поддерживаемого звука.",
	},
	{ "lng_oblivion_music_download_failed", "Не удалось загрузить файл." },
	{ "lng_oblivion_music_too_large", "Файл слишком большой." },
	{ "lng_oblivion_music_too_many", "Можно склеить не больше {max} треков." },
	{ "lng_oblivion_music_truncated", "Открыты только первые {duration}." },
	{
		"lng_oblivion_music_too_long",
		"Общая длительность треков — не больше {duration}.",
	},
	{ "lng_oblivion_music_info_mono", "{rate} кГц, моно" },
	{ "lng_oblivion_music_info_stereo", "{rate} кГц, стерео" },
	{ "lng_oblivion_music_default_name", "Аудио" },
	{ "lng_oblivion_music_crossfade", "Плавный переход между треками" },
	{ "lng_oblivion_music_seconds", "{value} с" },
	{ "lng_oblivion_music_off", "Выкл." },
	{ "lng_oblivion_music_fragment", "Фрагмент" },
	{
		"lng_oblivion_music_empty",
		"Откройте аудиофайл или видео: MP3, M4A, OGG, OPUS, WAV, FLAC, MP4, MOV, MKV.",
	},
	{ "lng_oblivion_music_range", "Выбрано {from} – {till} ({duration})" },
	{
		"lng_oblivion_music_trim_about",
		"Перетащите края волны, чтобы обрезать трек. Нажмите на волну, чтобы слушать с этого места.",
	},
	{ "lng_oblivion_music_processing", "Обработка…" },
	{ "lng_oblivion_music_process_failed", "Не удалось обработать звук." },
	{ "lng_oblivion_music_encoding", "Кодирование…" },
	{ "lng_oblivion_music_effects", "Эффекты" },
	{ "lng_oblivion_music_preset_original", "Оригинал" },
	{ "lng_oblivion_music_preset_slowed", "Замедление + реверб" },
	{ "lng_oblivion_music_preset_sped_up", "Ускорение" },
	{ "lng_oblivion_music_preset_nightcore", "Найткор" },
	{ "lng_oblivion_music_speed", "Скорость" },
	{ "lng_oblivion_music_mode_vinyl", "Как пластинка (тон меняется)" },
	{ "lng_oblivion_music_mode_tempo", "Только темп (тон сохраняется)" },
	{ "lng_oblivion_music_pitch", "Тональность, полутоны" },
	{ "lng_oblivion_music_reverb", "Реверберация" },
	{ "lng_oblivion_music_volume", "Громкость, дБ" },
	{ "lng_oblivion_music_normalize", "Выровнять громкость" },
	{ "lng_oblivion_music_fade_in", "Плавное нарастание в начале" },
	{ "lng_oblivion_music_fade_out", "Плавное затухание в конце" },
	{ "lng_oblivion_music_export", "Экспорт" },
	{ "lng_oblivion_music_title_field", "Название" },
	{ "lng_oblivion_music_performer_field", "Исполнитель" },
	{ "lng_oblivion_music_format_m4a", "M4A (AAC) — открывается везде" },
	{ "lng_oblivion_music_format_ogg", "OGG (Opus) — файл меньше" },
	{ "lng_oblivion_music_format_wav", "WAV — без сжатия, большой файл" },
	{ "lng_oblivion_music_file_m4a", "Аудио M4A" },
	{ "lng_oblivion_music_file_ogg", "Аудио OGG" },
	{ "lng_oblivion_music_file_wav", "Аудио WAV" },
	{ "lng_oblivion_music_save", "Сохранить…" },
	{ "lng_oblivion_music_save_title", "Сохранить аудио" },
	{ "lng_oblivion_music_send", "Отправить…" },
	{ "lng_oblivion_music_send_title", "Отправить в…" },
	{ "lng_oblivion_music_sent", "Отправлено в «{chat}»." },
	{ "lng_oblivion_music_restricted", "В этот чат нельзя отправлять музыку." },
	{ "lng_oblivion_music_nothing", "Сначала добавьте трек." },
	{
		"lng_oblivion_music_busy",
		"Подождите, предыдущий экспорт ещё не закончен.",
	},
	{ "lng_oblivion_music_encode_failed", "Не удалось закодировать звук." },
	{ "lng_oblivion_music_tag_slowed", "замедлено" },
	{ "lng_oblivion_music_tag_sped_up", "ускорено" },
	{ "lng_oblivion_music_tag_nightcore", "найткор" },
	{ "lng_oblivion_music_tag_pitch", "тон {value}" },
	{ "lng_oblivion_music_tag_reverb", "реверб" },
	// Tools: playlists.
	{ "lng_oblivion_playlists_add_to", "Добавить в плейлист" },
	{ "lng_oblivion_playlists_new", "Новый плейлист" },
	{ "lng_oblivion_playlists_new_menu", "Новый плейлист…" },
	{ "lng_oblivion_playlists_name", "Название плейлиста" },
	{ "lng_oblivion_playlists_create", "Создать" },
	{ "lng_oblivion_playlists_rename", "Переименовать" },
	{ "lng_oblivion_playlists_rename_title", "Переименовать плейлист" },
	{ "lng_oblivion_playlists_delete", "Удалить плейлист" },
	{
		"lng_oblivion_playlists_delete_sure",
		"Удалить плейлист «{name}»? Треки останутся в своих чатах.",
	},
	{
		"lng_oblivion_playlists_empty",
		"У вас пока нет плейлистов. Создайте плейлист здесь или нажмите "
		"правой кнопкой мыши на музыкальный файл в любом чате и выберите "
		"«Добавить в плейлист».",
	},
	{
		"lng_oblivion_playlists_list_about",
		"Перетаскивайте плейлисты, чтобы изменить порядок. Плейлисты "
		"хранятся только на этом устройстве.",
	},
	{ "lng_oblivion_playlists_tracks", "Треков: {number}" },
	{ "lng_oblivion_playlists_tracks_count#one", "{count} трек" },
	{ "lng_oblivion_playlists_tracks_count#few", "{count} трека" },
	{ "lng_oblivion_playlists_tracks_count#many", "{count} треков" },
	{ "lng_oblivion_playlists_tracks_count#other", "{count} трека" },
	{
		"lng_oblivion_playlists_no_tracks",
		"Плейлист пуст. Нажмите правой кнопкой мыши на музыкальный файл "
		"в любом чате и выберите «Добавить в плейлист».",
	},
	{
		"lng_oblivion_playlists_about",
		"Перетаскивайте треки, чтобы изменить порядок. Нажмите на трек, "
		"чтобы слушать плейлист с него.",
	},
	{ "lng_oblivion_playlists_play", "Слушать" },
	{ "lng_oblivion_playlists_shuffle", "Перемешать" },
	{ "lng_oblivion_playlists_play_from", "Слушать с этого трека" },
	{ "lng_oblivion_playlists_move_up", "Переместить выше" },
	{ "lng_oblivion_playlists_move_down", "Переместить ниже" },
	{ "lng_oblivion_playlists_remove_track", "Убрать из плейлиста" },
	{
		"lng_oblivion_playlists_remove_unavailable",
		"Убрать недоступные треки",
	},
	{ "lng_oblivion_playlists_unavailable", "Недоступен" },
	{
		"lng_oblivion_playlists_unavailable_toast",
		"Трек недоступен: сообщение удалено или не загружается.",
	},
	{ "lng_oblivion_playlists_loading", "Загрузка…" },
	{ "lng_oblivion_playlists_load_failed", "Не удалось загрузить" },
	{ "lng_oblivion_playlists_unknown", "Неизвестный трек" },
	{ "lng_oblivion_playlists_added", "Добавлено в «{name}»." },
	{ "lng_oblivion_playlists_already", "Этот трек уже есть в «{name}»." },
	{
		"lng_oblivion_playlists_nothing",
		"В этом плейлисте нет доступных для прослушивания треков.",
	},
	// Tools: photo editor.
	{ "lng_oblivion_photo_filter_original", "Оригинал" },
	{ "lng_oblivion_photo_filter_bw", "Чёрно-белый" },
	{ "lng_oblivion_photo_filter_bw_contrast", "Контраст Ч/Б" },
	{ "lng_oblivion_photo_filter_sepia", "Сепия" },
	{ "lng_oblivion_photo_filter_vintage", "Винтаж" },
	{ "lng_oblivion_photo_filter_film", "Плёнка" },
	{ "lng_oblivion_photo_filter_warm", "Тёплый" },
	{ "lng_oblivion_photo_filter_cool", "Холодный" },
	{ "lng_oblivion_photo_filter_cinema", "Кино" },
	{ "lng_oblivion_photo_filter_faded", "Выцветший" },
	{ "lng_oblivion_photo_filter_noir", "Нуар" },
	{ "lng_oblivion_photo_filter_pastel", "Пастель" },
	{ "lng_oblivion_photo_filter_vivid", "Яркий" },
	{ "lng_oblivion_photo_filter_sunset", "Закат" },
	{ "lng_oblivion_photo_filter_moonlight", "Лунный" },
	{ "lng_oblivion_photo_filter_cross", "Кросс-процесс" },
	{ "lng_oblivion_photo_filter_lomo", "Ломо" },
	{ "lng_oblivion_photo_filter_retro", "Ретро" },
	{ "lng_oblivion_photo_filter_drama", "Драма" },
	{ "lng_oblivion_photo_adjust_exposure", "Экспозиция" },
	{ "lng_oblivion_photo_adjust_brightness", "Яркость" },
	{ "lng_oblivion_photo_adjust_contrast", "Контраст" },
	{ "lng_oblivion_photo_adjust_highlights", "Света" },
	{ "lng_oblivion_photo_adjust_shadows", "Тени" },
	{ "lng_oblivion_photo_adjust_whites", "Белые" },
	{ "lng_oblivion_photo_adjust_blacks", "Чёрные" },
	{ "lng_oblivion_photo_adjust_saturation", "Насыщенность" },
	{ "lng_oblivion_photo_adjust_vibrance", "Красочность" },
	{ "lng_oblivion_photo_adjust_temperature", "Температура" },
	{ "lng_oblivion_photo_adjust_tint", "Оттенок" },
	{ "lng_oblivion_photo_adjust_fade", "Выцветание" },
	{ "lng_oblivion_photo_adjust_clarity", "Чёткость" },
	{ "lng_oblivion_photo_adjust_sharpen", "Резкость" },
	{ "lng_oblivion_photo_adjust_vignette", "Виньетка" },
	{ "lng_oblivion_photo_adjust_vignette_feather", "Мягкость виньетки" },
	{ "lng_oblivion_photo_adjust_grain", "Зерно" },
	{ "lng_oblivion_photo_adjust_grain_size", "Размер зерна" },
	{ "lng_oblivion_photo_adjust_blur", "Размытие" },
	{ "lng_oblivion_photo_effect_pixelate", "Пикселизация" },
	{ "lng_oblivion_photo_effect_glitch", "Глитч" },
	{ "lng_oblivion_photo_effect_vhs", "VHS" },
	{ "lng_oblivion_photo_effect_posterize", "Постеризация" },
	{ "lng_oblivion_photo_effect_duotone", "Дуотон" },
	{ "lng_oblivion_photo_effect_halftone", "Полутоновый растр" },
	{ "lng_oblivion_photo_effect_emboss", "Тиснение" },
	{ "lng_oblivion_photo_effect_chromatic", "Хроматическая аберрация" },
	{ "lng_oblivion_photo_effect_glow", "Свечение" },
	{ "lng_oblivion_photo_effect_bw", "Ч/Б микшер" },
	{ "lng_oblivion_photo_effect_sepia", "Сепия" },
	{ "lng_oblivion_photo_effect_invert", "Негатив" },
	{ "lng_oblivion_photo_effect_film", "Киноплёнка" },
	{ "lng_oblivion_photo_effect_lens_blur", "Тилт-шифт" },
	{ "lng_oblivion_photo_param_amount", "Сила" },
	{ "lng_oblivion_photo_param_size", "Размер" },
	{ "lng_oblivion_photo_param_levels", "Уровни" },
	{ "lng_oblivion_photo_param_red", "Красный" },
	{ "lng_oblivion_photo_param_green", "Зелёный" },
	{ "lng_oblivion_photo_param_blue", "Синий" },
	{ "lng_oblivion_photo_param_position", "Положение" },
	{ "lng_oblivion_photo_param_width", "Ширина" },
	{ "lng_oblivion_photo_param_feather", "Растушёвка" },
	{ "lng_oblivion_photo_param_angle", "Угол" },
	{ "lng_oblivion_photo_param_seed", "Вариант" },
	{ "lng_oblivion_photo_param_mode", "Цветной" },
	{ "lng_oblivion_photo_param_grain", "Зерно" },
	{ "lng_oblivion_photo_param_threshold", "Порог" },
	{ "lng_oblivion_photo_param_color1", "Цвет теней" },
	{ "lng_oblivion_photo_param_color2", "Цвет светлых участков" },
	// Tools: photo editor UI.
	{ "lng_oblivion_photo_ui_title", "Фоторедактор" },
	{ "lng_oblivion_photo_ui_done", "Готово" },
	{ "lng_oblivion_photo_ui_save", "Сохранить" },
	{ "lng_oblivion_photo_ui_close", "Закрыть" },
	{ "lng_oblivion_photo_ui_undo", "Отменить" },
	{ "lng_oblivion_photo_ui_redo", "Повторить" },
	{
		"lng_oblivion_photo_ui_compare",
		"Удерживайте, чтобы увидеть оригинал",
	},
	{ "lng_oblivion_photo_ui_reset_all", "Сбросить все правки" },
	{ "lng_oblivion_photo_ui_more", "Ещё" },
	{ "lng_oblivion_photo_ui_shortcut", "{action} ({keys})" },
	{ "lng_oblivion_photo_ui_decimal_separator", "," },
	{ "lng_oblivion_photo_ui_before", "Оригинал" },
	{ "lng_oblivion_photo_ui_tab_crop", "Обрезка" },
	{ "lng_oblivion_photo_ui_tab_adjust", "Настройки" },
	{ "lng_oblivion_photo_ui_tab_filters", "Фильтры" },
	{ "lng_oblivion_photo_ui_tab_effects", "Эффекты" },
	{ "lng_oblivion_photo_ui_tab_auto", "Авто" },
	{ "lng_oblivion_photo_ui_aspect", "Соотношение сторон" },
	{ "lng_oblivion_photo_ui_aspect_free", "Свободно" },
	{ "lng_oblivion_photo_ui_aspect_original", "Оригинал" },
	{ "lng_oblivion_photo_ui_rotate_section", "Поворот и отражение" },
	{ "lng_oblivion_photo_ui_rotate_left", "Повернуть влево" },
	{ "lng_oblivion_photo_ui_rotate_right", "Повернуть вправо" },
	{ "lng_oblivion_photo_ui_flip_horizontal", "Отразить по горизонтали" },
	{ "lng_oblivion_photo_ui_flip_vertical", "Отразить по вертикали" },
	{ "lng_oblivion_photo_ui_straighten", "Выравнивание" },
	{ "lng_oblivion_photo_ui_reset_crop", "Сбросить обрезку и поворот" },
	{
		"lng_oblivion_photo_ui_crop_hint",
		"Перетаскивайте углы и края рамки или саму рамку. "
		"Удерживайте Shift, чтобы сохранить пропорции.",
	},
	{ "lng_oblivion_photo_ui_group_light", "Свет" },
	{ "lng_oblivion_photo_ui_group_color", "Цвет" },
	{ "lng_oblivion_photo_ui_group_details", "Детали" },
	{ "lng_oblivion_photo_ui_group_finish", "Виньетка и зерно" },
	{ "lng_oblivion_photo_ui_reset", "Сбросить" },
	{
		"lng_oblivion_photo_ui_adjust_hint",
		"Дважды щёлкните по ползунку, чтобы сбросить его.",
	},
	{ "lng_oblivion_photo_ui_intensity", "Интенсивность" },
	{
		"lng_oblivion_photo_ui_filters_hint",
		"Миниатюры под фото показывают каждый фильтр с учётом ваших "
		"правок. Клавиши ← и → переключают фильтры.",
	},
	{ "lng_oblivion_photo_ui_effects_applied", "Применённые эффекты" },
	{ "lng_oblivion_photo_ui_effects_clear", "Убрать все" },
	{
		"lng_oblivion_photo_ui_effects_empty",
		"Эффектов пока нет. Добавьте их из списка ниже.",
	},
	{ "lng_oblivion_photo_ui_effects_add", "Добавить эффект" },
	{
		"lng_oblivion_photo_ui_effects_hint",
		"Эффекты применяются по очереди сверху вниз. Один и тот же эффект "
		"можно добавить несколько раз.",
	},
	{
		"lng_oblivion_photo_ui_effects_limit",
		"Можно добавить не больше {max} эффектов.",
	},
	{ "lng_oblivion_photo_ui_effect_up", "Переместить выше" },
	{ "lng_oblivion_photo_ui_effect_down", "Переместить ниже" },
	{ "lng_oblivion_photo_ui_effect_duplicate", "Дублировать" },
	{ "lng_oblivion_photo_ui_effect_reset", "Сбросить параметры" },
	{ "lng_oblivion_photo_ui_effect_remove", "Удалить" },
	{ "lng_oblivion_photo_ui_color_title", "Выбор цвета" },
	{ "lng_oblivion_photo_ui_auto_title", "Автоулучшение" },
	{
		"lng_oblivion_photo_ui_auto_about",
		"Подбирает экспозицию, контраст, баланс белого и насыщенность "
		"для этого фото. Результат можно доработать на вкладке "
		"«Настройки».",
	},
	{ "lng_oblivion_photo_ui_auto_button", "Улучшить" },
	{ "lng_oblivion_photo_ui_auto_working", "Анализ…" },
	{ "lng_oblivion_photo_ui_auto_amount", "Сила" },
	{
		"lng_oblivion_photo_ui_auto_nothing",
		"Это фото уже выглядит сбалансированно.",
	},
	{ "lng_oblivion_photo_ui_saving", "Сохранение…" },
	{ "lng_oblivion_photo_ui_processing", "Обработка…" },
	{ "lng_oblivion_photo_ui_save_file", "Сохранить в файл…" },
	{ "lng_oblivion_photo_ui_save_title", "Сохранить изображение" },
	{ "lng_oblivion_photo_ui_saved", "Изображение сохранено." },
	{
		"lng_oblivion_photo_ui_save_failed",
		"Не удалось сохранить изображение.",
	},
	{ "lng_oblivion_photo_ui_copy", "Копировать изображение" },
	{
		"lng_oblivion_photo_ui_copied",
		"Изображение скопировано в буфер обмена.",
	},
	{
		"lng_oblivion_photo_ui_export_failed",
		"Не удалось обработать изображение. Возможно, оно слишком большое.",
	},
	{ "lng_oblivion_photo_ui_format_png", "Изображение PNG" },
	{ "lng_oblivion_photo_ui_format_jpeg", "Изображение JPEG" },
	{ "lng_oblivion_photo_ui_format_webp", "Изображение WebP" },
	{ "lng_oblivion_photo_ui_close_title", "Закрыть редактор?" },
	{
		"lng_oblivion_photo_ui_close_text",
		"Правки этого фото не сохранятся.",
	},
	{ "lng_oblivion_photo_ui_open_title", "Выберите изображение" },
	{
		"lng_oblivion_photo_ui_open_failed",
		"Не удалось открыть изображение.",
	},
	{ "lng_oblivion_photo_ui_default_name", "фото" },
	// Tools: photo editor integration.
	{ "lng_oblivion_photo_io_tools", "Фоторедактор" },
	{ "lng_oblivion_photo_io_title", "Фоторедактор" },
	{ "lng_oblivion_photo_io_open_context", "Открыть в фоторедакторе" },
	{ "lng_oblivion_photo_io_attach_action", "Фоторедактор Oblivion" },
	{ "lng_oblivion_photo_io_viewer_action", "Редактировать в фоторедакторе" },
	{ "lng_oblivion_photo_io_drop", "Перетащите изображение сюда" },
	{
		"lng_oblivion_photo_io_drop_about",
		"или нажмите {shortcut}, чтобы вставить из буфера обмена",
	},
	{ "lng_oblivion_photo_io_drop_active", "Отпустите, чтобы открыть изображение" },
	{ "lng_oblivion_photo_io_open_file", "Открыть файл…" },
	{ "lng_oblivion_photo_io_paste", "Вставить из буфера обмена" },
	{ "lng_oblivion_photo_io_paste_empty", "В буфере обмена нет изображения." },
	{
		"lng_oblivion_photo_io_import_about",
		"Исходное изображение не меняется: результат сохраняется или отправляется как новое.",
	},
	{ "lng_oblivion_photo_io_choose", "Выберите изображение" },
	{ "lng_oblivion_photo_io_loading", "Открываем изображение…" },
	{ "lng_oblivion_photo_io_downloading", "Загрузка изображения… {percent}" },
	{ "lng_oblivion_photo_io_download_failed", "Не удалось загрузить изображение." },
	{ "lng_oblivion_photo_io_open_failed", "Не удалось открыть это изображение." },
	{
		"lng_oblivion_photo_io_process_failed",
		"Не удалось применить изменения: для такого большого изображения не хватает памяти.",
	},
	{ "lng_oblivion_photo_io_result_title", "Готовое изображение" },
	{ "lng_oblivion_photo_io_format", "Формат" },
	{ "lng_oblivion_photo_io_quality", "Качество" },
	{ "lng_oblivion_photo_io_lossless", "Без потерь" },
	{ "lng_oblivion_photo_io_dimensions", "{width} × {height} пикс." },
	{
		"lng_oblivion_photo_io_png_about",
		"PNG сохраняет каждый пиксель и прозрачность, но файл получается больше.",
	},
	{ "lng_oblivion_photo_io_jpeg_about", "JPEG — компактный формат для фотографий." },
	{
		"lng_oblivion_photo_io_jpeg_alpha",
		"В JPEG нет прозрачности: прозрачные участки станут белыми.",
	},
	{
		"lng_oblivion_photo_io_webp_about",
		"WebP сохраняет прозрачность, при качестве 100 — без потерь.",
	},
	{ "lng_oblivion_photo_io_filter_png", "Изображение PNG" },
	{ "lng_oblivion_photo_io_filter_jpeg", "Изображение JPEG" },
	{ "lng_oblivion_photo_io_filter_webp", "Изображение WebP" },
	{ "lng_oblivion_photo_io_save", "Сохранить как…" },
	{ "lng_oblivion_photo_io_save_title", "Сохранить изображение" },
	{ "lng_oblivion_photo_io_save_failed", "Не удалось сохранить изображение." },
	{ "lng_oblivion_photo_io_copy", "Копировать в буфер обмена" },
	{ "lng_oblivion_photo_io_copied", "Изображение скопировано в буфер обмена." },
	{ "lng_oblivion_photo_io_send", "Отправить в чат…" },
	{ "lng_oblivion_photo_io_send_here", "Отправить в «{chat}»" },
	{ "lng_oblivion_photo_io_send_title", "Отправить изображение" },
	{
		"lng_oblivion_photo_io_send_restricted",
		"В этот чат нельзя отправлять изображения.",
	},
	{ "lng_oblivion_photo_io_sent", "Изображение отправлено в «{chat}»." },
	{ "lng_oblivion_photo_io_edit_again", "Продолжить редактирование" },
	// Tools: lottie editor.
	{ "lng_oblivion_tools_lottie_editor", "Редактор Lottie" },
	{ "lng_oblivion_lottie_open_menu", "Открыть в редакторе Lottie" },
	{ "lng_oblivion_lottie_editor_title", "Редактор Lottie" },
	{
		"lng_oblivion_lottie_editor_window_title",
		"{name} — Редактор Lottie",
	},
	{ "lng_oblivion_lottie_editor_new_name", "Новая анимация" },
	{ "lng_oblivion_lottie_editor_new", "Новая" },
	{ "lng_oblivion_lottie_editor_open", "Открыть" },
	{ "lng_oblivion_lottie_editor_save", "Сохранить" },
	{ "lng_oblivion_lottie_editor_save_as", "Сохранить как…" },
	{ "lng_oblivion_lottie_editor_export", "Экспорт" },
	{ "lng_oblivion_lottie_editor_export_tgs", "Стикер Telegram (.tgs)" },
	{ "lng_oblivion_lottie_editor_export_json", "Анимация Lottie (.json)" },
	{ "lng_oblivion_lottie_editor_undo", "Отменить" },
	{ "lng_oblivion_lottie_editor_redo", "Повторить" },
	{ "lng_oblivion_lottie_editor_undo_action", "Отменить: {action}" },
	{ "lng_oblivion_lottie_editor_redo_action", "Повторить: {action}" },
	{ "lng_oblivion_lottie_editor_shortcut", "{action} ({keys})" },
	{ "lng_oblivion_lottie_editor_open_title", "Открыть анимацию Lottie" },
	{ "lng_oblivion_lottie_editor_save_title", "Сохранить анимацию" },
	{ "lng_oblivion_lottie_editor_filter_lottie", "Анимации Lottie" },
	{ "lng_oblivion_lottie_editor_filter_all", "Все файлы" },
	{
		"lng_oblivion_lottie_editor_read_failed",
		"Не удалось прочитать файл.",
	},
	{
		"lng_oblivion_lottie_editor_download_failed",
		"Не удалось загрузить анимацию.",
	},
	{
		"lng_oblivion_lottie_editor_open_failed",
		"Этот файл не является анимацией Lottie.",
	},
	{
		"lng_oblivion_lottie_editor_save_failed",
		"Не удалось сохранить файл.",
	},
	{
		"lng_oblivion_lottie_editor_saved_warning",
		"Сохранено, но Telegram не примет файл как стикер: {problem}",
	},
	{ "lng_oblivion_lottie_editor_close_title", "Сохранить изменения?" },
	{
		"lng_oblivion_lottie_editor_close_text",
		"В «{name}» есть несохранённые изменения. "
		"Если не сохранить их, они пропадут.",
	},
	{ "lng_oblivion_lottie_editor_close_discard", "Не сохранять" },
	{
		"lng_oblivion_lottie_editor_locked_quit",
		"В редакторе Lottie есть несохранённые изменения. Разблокируйте "
		"Telegram, чтобы сохранить их, или выйдите без сохранения.",
	},
	{ "lng_oblivion_lottie_editor_info", "{size} · {fps} · {duration}" },
	{ "lng_oblivion_lottie_editor_fps", "{value} к/с" },
	{ "lng_oblivion_lottie_editor_seconds", "{value} с" },
	{ "lng_oblivion_lottie_editor_frame", "Кадр {value}" },
	{ "lng_oblivion_lottie_editor_frame_of", "Кадр {value} из {total}" },
	{ "lng_oblivion_lottie_editor_kilobytes", "{value} КБ" },
	{ "lng_oblivion_lottie_editor_layers", "Слои" },
	{ "lng_oblivion_lottie_editor_properties", "Свойства" },
	{ "lng_oblivion_lottie_editor_timeline", "Шкала времени" },
	{ "lng_oblivion_lottie_editor_canvas", "Холст" },
	{ "lng_oblivion_lottie_editor_no_layers", "Слоёв пока нет" },
	{
		"lng_oblivion_lottie_editor_nothing_selected",
		"Выберите слой, чтобы увидеть его свойства",
	},
	{ "lng_oblivion_lottie_editor_selected", "Выбрано: {value}" },
	{ "lng_oblivion_lottie_editor_play", "Воспроизвести" },
	{ "lng_oblivion_lottie_editor_pause", "Пауза" },
	{ "lng_oblivion_lottie_editor_tgs_ok", "Подходит для Telegram" },
	{ "lng_oblivion_lottie_editor_tgs_errors", "Проблем: {value}" },
	{ "lng_oblivion_lottie_editor_tgs_warnings", "Рекомендаций: {value}" },
	{ "lng_oblivion_lottie_editor_tgs_checking", "Проверка…" },
	{ "lng_oblivion_lottie_editor_validate_title", "Проверка стикера" },
	{
		"lng_oblivion_lottie_editor_validate_ok",
		"Анимация соответствует всем требованиям Telegram к стикерам.",
	},
	{
		"lng_oblivion_lottie_editor_validate_errors",
		"Telegram не примет такой стикер",
	},
	{ "lng_oblivion_lottie_editor_validate_warnings", "Рекомендации" },
	{
		"lng_oblivion_lottie_editor_validate_size",
		"Размер в сжатом виде: {size} из {limit}.",
	},
	{ "lng_oblivion_lottie_editor_validate_fix", "Исправить" },
	{
		"lng_oblivion_lottie_editor_validate_fix_all",
		"Исправить автоматически",
	},
	{ "lng_oblivion_lottie_editor_validate_select", "Выделить" },
	{ "lng_oblivion_lottie_editor_validate_fixed", "Исправлено." },
	{
		"lng_oblivion_lottie_editor_validate_still",
		"Некоторые проблемы нужно исправить вручную.",
	},
	{
		"lng_oblivion_lottie_editor_validate_nothing",
		"Автоматически исправить ничего не удалось.",
	},
	{
		"lng_oblivion_lottie_issue_invalid",
		"Композиция повреждена: неверный размер, частота кадров "
		"или длительность.",
	},
	{
		"lng_oblivion_lottie_issue_version",
		"Не указана версия Lottie — анимация не загрузится.",
	},
	{
		"lng_oblivion_lottie_issue_canvas",
		"Холст {size}, а Telegram требует 512×512.",
	},
	{
		"lng_oblivion_lottie_issue_fps",
		"Частота {value} к/с, а Telegram требует 30 или 60.",
	},
	{
		"lng_oblivion_lottie_issue_duration",
		"Анимация длится {value} с, а максимум — 3 с.",
	},
	{
		"lng_oblivion_lottie_issue_size",
		"В сжатом виде файл занимает {value} КБ, а максимум — 64 КБ.",
	},
	{
		"lng_oblivion_lottie_issue_images",
		"Содержит изображения, а стикер может быть только векторным.",
	},
	{
		"lng_oblivion_lottie_issue_expressions",
		"Использует выражения — Telegram их не выполняет.",
	},
	{
		"lng_oblivion_lottie_issue_3d",
		"Есть 3D-слои — Telegram их не поддерживает.",
	},
	{
		"lng_oblivion_lottie_issue_text",
		"Есть текстовые слои — текст нужно преобразовать в фигуры.",
	},
	{
		"lng_oblivion_lottie_issue_keyframes",
		"Некоторые ключевые кадры Telegram покажет неправильно.",
	},
	{
		"lng_oblivion_lottie_issue_marker",
		"Нет отметки стикера Telegram.",
	},
	{
		"lng_oblivion_lottie_issue_effects",
		"Использует эффекты слоёв — Telegram их игнорирует.",
	},
	{ "lng_oblivion_lottie_issue_solids", "Есть слои сплошного цвета." },
	{
		"lng_oblivion_lottie_issue_stretch",
		"Использует растяжение времени слоёв.",
	},
	{
		"lng_oblivion_lottie_issue_remap",
		"Использует переназначение времени.",
	},
	{
		"lng_oblivion_lottie_issue_merge",
		"Использует объединение контуров — Telegram его игнорирует.",
	},
	{
		"lng_oblivion_lottie_issue_modifiers",
		"Использует модификаторы фигур, которые Telegram игнорирует: "
		"смещение контура, втягивание, скручивание, зигзаг "
		"или скругление углов.",
	},
	{ "lng_oblivion_lottie_issue_repeaters", "Использует повторители." },
	{
		"lng_oblivion_lottie_issue_stars",
		"Использует звёзды или многоугольники.",
	},
	{
		"lng_oblivion_lottie_issue_gradient_strokes",
		"Использует градиентные обводки.",
	},
	{
		"lng_oblivion_lottie_issue_edge",
		"Содержимое касается края холста.",
	},
	{
		"lng_oblivion_lottie_issue_auto_orient",
		"Использует автоориентацию слоёв.",
	},
	{ "lng_oblivion_lottie_fix_version", "Добавить версию" },
	{ "lng_oblivion_lottie_fix_canvas", "Масштабировать до 512×512" },
	{ "lng_oblivion_lottie_fix_fps", "Изменить частоту кадров" },
	{ "lng_oblivion_lottie_fix_duration", "Обрезать до 3 с" },
	{ "lng_oblivion_lottie_fix_size", "Оптимизировать" },
	{ "lng_oblivion_lottie_fix_remove", "Удалить" },
	{ "lng_oblivion_lottie_fix_3d", "Сделать двумерными" },
	{ "lng_oblivion_lottie_fix_keyframes", "Исправить ключевые кадры" },
	{ "lng_oblivion_lottie_fix_marker", "Добавить отметку" },
	{ "lng_oblivion_lottie_fix_solids", "Преобразовать в фигуры" },
	{ "lng_oblivion_lottie_fix_auto_orient", "Отключить" },
	{ "lng_oblivion_lottie_cmd_change", "Изменение" },
	{ "lng_oblivion_lottie_cmd_value", "Изменение значения" },
	{ "lng_oblivion_lottie_cmd_animated", "Включение анимации" },
	{ "lng_oblivion_lottie_cmd_add_keyframe", "Новый ключевой кадр" },
	{ "lng_oblivion_lottie_cmd_keyframe", "Изменение ключевого кадра" },
	{ "lng_oblivion_lottie_cmd_easing", "Изменение сглаживания" },
	{
		"lng_oblivion_lottie_cmd_remove_keyframes",
		"Удаление ключевых кадров",
	},
	{
		"lng_oblivion_lottie_cmd_move_keyframes",
		"Перемещение ключевых кадров",
	},
	{ "lng_oblivion_lottie_cmd_rename", "Переименование" },
	{ "lng_oblivion_lottie_cmd_hidden", "Изменение видимости" },
	{ "lng_oblivion_lottie_cmd_delete", "Удаление" },
	{ "lng_oblivion_lottie_cmd_duplicate", "Дублирование" },
	{ "lng_oblivion_lottie_cmd_move", "Изменение порядка" },
	{ "lng_oblivion_lottie_cmd_add_layer", "Новый слой" },
	{ "lng_oblivion_lottie_cmd_add_shape", "Новая фигура" },
	{ "lng_oblivion_lottie_cmd_timing", "Изменение времени слоя" },
	{ "lng_oblivion_lottie_cmd_replace_color", "Замена цвета" },
	{ "lng_oblivion_lottie_cmd_recolor", "Перекраска" },
	{ "lng_oblivion_lottie_cmd_canvas", "Размер холста" },
	{ "lng_oblivion_lottie_cmd_fps", "Частота кадров" },
	{ "lng_oblivion_lottie_cmd_duration", "Длительность" },
	{ "lng_oblivion_lottie_cmd_speed", "Скорость" },
	{ "lng_oblivion_lottie_cmd_trim", "Обрезка" },
	{ "lng_oblivion_lottie_cmd_autofix", "Автоисправление" },
	{ "lng_oblivion_lottie_cmd_optimize", "Оптимизация" },
	{ "lng_oblivion_lottie_node_composition", "Композиция" },
	{ "lng_oblivion_lottie_node_asset", "Прекомпозиция" },
	{ "lng_oblivion_lottie_node_layer", "Слой" },
	{ "lng_oblivion_lottie_node_shape", "Фигура" },
	{ "lng_oblivion_lottie_node_mask", "Маска" },
	{ "lng_oblivion_lottie_node_effect", "Эффект" },
	{ "lng_oblivion_lottie_layer_precomp", "Прекомпозиция" },
	{ "lng_oblivion_lottie_layer_solid", "Сплошной цвет" },
	{ "lng_oblivion_lottie_layer_image", "Изображение" },
	{ "lng_oblivion_lottie_layer_null", "Пустой объект" },
	{ "lng_oblivion_lottie_layer_shape", "Слой-фигура" },
	{ "lng_oblivion_lottie_layer_text", "Текст" },
	{ "lng_oblivion_lottie_shape_group", "Группа" },
	{ "lng_oblivion_lottie_shape_rectangle", "Прямоугольник" },
	{ "lng_oblivion_lottie_shape_ellipse", "Эллипс" },
	{ "lng_oblivion_lottie_shape_star", "Звезда" },
	{ "lng_oblivion_lottie_shape_path", "Контур" },
	{ "lng_oblivion_lottie_shape_fill", "Заливка" },
	{ "lng_oblivion_lottie_shape_stroke", "Обводка" },
	{ "lng_oblivion_lottie_shape_gradient_fill", "Градиентная заливка" },
	{ "lng_oblivion_lottie_shape_gradient_stroke", "Градиентная обводка" },
	{ "lng_oblivion_lottie_shape_transform", "Трансформация" },
	{ "lng_oblivion_lottie_shape_trim", "Обрезка контуров" },
	{ "lng_oblivion_lottie_shape_repeater", "Повторитель" },
	{ "lng_oblivion_lottie_shape_merge", "Объединение контуров" },
	{ "lng_oblivion_lottie_shape_round", "Скругление углов" },
	{ "lng_oblivion_lottie_shape_offset", "Смещение контура" },
	{ "lng_oblivion_lottie_shape_pucker", "Втягивание и раздувание" },
	{ "lng_oblivion_lottie_shape_twist", "Скручивание" },
	{ "lng_oblivion_lottie_shape_zigzag", "Зигзаг" },
	{ "lng_oblivion_lottie_prop_anchor", "Опорная точка" },
	{ "lng_oblivion_lottie_prop_position", "Положение" },
	{ "lng_oblivion_lottie_prop_position_x", "Положение по X" },
	{ "lng_oblivion_lottie_prop_position_y", "Положение по Y" },
	{ "lng_oblivion_lottie_prop_position_z", "Положение по Z" },
	{ "lng_oblivion_lottie_prop_scale", "Масштаб" },
	{ "lng_oblivion_lottie_prop_rotation", "Поворот" },
	{ "lng_oblivion_lottie_prop_rotation_x", "Поворот по X" },
	{ "lng_oblivion_lottie_prop_rotation_y", "Поворот по Y" },
	{ "lng_oblivion_lottie_prop_opacity", "Непрозрачность" },
	{ "lng_oblivion_lottie_prop_skew", "Наклон" },
	{ "lng_oblivion_lottie_prop_skew_axis", "Ось наклона" },
	{ "lng_oblivion_lottie_prop_color", "Цвет" },
	{ "lng_oblivion_lottie_prop_stroke_width", "Толщина обводки" },
	{ "lng_oblivion_lottie_prop_size", "Размер" },
	{ "lng_oblivion_lottie_prop_roundness", "Скругление" },
	{ "lng_oblivion_lottie_prop_start_point", "Начальная точка" },
	{ "lng_oblivion_lottie_prop_end_point", "Конечная точка" },
	{ "lng_oblivion_lottie_prop_gradient", "Градиент" },
	{ "lng_oblivion_lottie_prop_highlight_length", "Длина блика" },
	{ "lng_oblivion_lottie_prop_highlight_angle", "Угол блика" },
	{ "lng_oblivion_lottie_prop_path", "Контур" },
	{ "lng_oblivion_lottie_prop_trim_start", "Начало" },
	{ "lng_oblivion_lottie_prop_trim_end", "Конец" },
	{ "lng_oblivion_lottie_prop_trim_offset", "Смещение" },
	{ "lng_oblivion_lottie_prop_points", "Число вершин" },
	{ "lng_oblivion_lottie_prop_inner_radius", "Внутренний радиус" },
	{ "lng_oblivion_lottie_prop_outer_radius", "Внешний радиус" },
	{ "lng_oblivion_lottie_prop_inner_roundness", "Внутреннее скругление" },
	{ "lng_oblivion_lottie_prop_outer_roundness", "Внешнее скругление" },
	{ "lng_oblivion_lottie_prop_copies", "Число копий" },
	{ "lng_oblivion_lottie_prop_offset", "Смещение" },
	{ "lng_oblivion_lottie_prop_start_opacity", "Начальная непрозрачность" },
	{ "lng_oblivion_lottie_prop_end_opacity", "Конечная непрозрачность" },
	{ "lng_oblivion_lottie_prop_mask_path", "Контур маски" },
	{ "lng_oblivion_lottie_prop_mask_opacity", "Непрозрачность маски" },
	{ "lng_oblivion_lottie_prop_mask_expansion", "Расширение маски" },
	{ "lng_oblivion_lottie_prop_time_remap", "Переназначение времени" },
	{ "lng_oblivion_lottie_prop_value", "Значение" },
	{ "lng_oblivion_lottie_prop_dash", "Штрих" },
	{ "lng_oblivion_lottie_prop_gap", "Промежуток" },
	{ "lng_oblivion_lottie_prop_dash_offset", "Смещение штриха" },
	{ "lng_oblivion_lottie_prop_radius", "Радиус" },
	{ "lng_oblivion_lottie_prop_amount", "Величина" },
	{ "lng_oblivion_lottie_prop_other", "Свойство" },
	{ "lng_oblivion_lottie_easing_linear", "Линейно" },
	{ "lng_oblivion_lottie_easing_in", "Плавный разгон" },
	{ "lng_oblivion_lottie_easing_out", "Плавная остановка" },
	{ "lng_oblivion_lottie_easing_in_out", "Плавно" },
	{ "lng_oblivion_lottie_easing_hold", "Удержание" },
	{ "lng_oblivion_lottie_easing_custom", "Своя кривая" },
	{ "lng_oblivion_lottie_color_solid", "Сплошной цвет" },
	{ "lng_oblivion_lottie_color_effect", "Цвет эффекта" },
	{ "lng_oblivion_lottie_color_text_fill", "Заливка текста" },
	{ "lng_oblivion_lottie_color_text_stroke", "Обводка текста" },
	{ "lng_oblivion_lottie_matte_none", "Без маски" },
	{ "lng_oblivion_lottie_matte_alpha", "Альфа-маска" },
	{
		"lng_oblivion_lottie_matte_alpha_inverted",
		"Инвертированная альфа-маска",
	},
	{ "lng_oblivion_lottie_matte_luma", "Яркостная маска" },
	{
		"lng_oblivion_lottie_matte_luma_inverted",
		"Инвертированная яркостная маска",
	},
	// Tools: lottie editor panels (layers, inspector, palette).
	{ "lng_oblivion_lottie_layers_search", "Поиск слоёв" },
	{ "lng_oblivion_lottie_layers_add", "Добавить" },
	{ "lng_oblivion_lottie_layers_new_rectangle", "Слой с прямоугольником" },
	{ "lng_oblivion_lottie_layers_new_ellipse", "Слой с эллипсом" },
	{ "lng_oblivion_lottie_layers_new_star", "Слой со звездой" },
	{ "lng_oblivion_lottie_layers_new_empty", "Пустой слой-фигура" },
	{ "lng_oblivion_lottie_layers_new_null", "Пустой объект (Null)" },
	{ "lng_oblivion_lottie_layers_add_into", "{type} в «{name}»" },
	{ "lng_oblivion_lottie_layers_rename", "Переименовать" },
	{ "lng_oblivion_lottie_layers_duplicate", "Дублировать" },
	{ "lng_oblivion_lottie_layers_delete", "Удалить" },
	{ "lng_oblivion_lottie_layers_hide", "Скрыть" },
	{ "lng_oblivion_lottie_layers_show", "Показать" },
	{ "lng_oblivion_lottie_layers_visibility", "Показать или скрыть" },
	{ "lng_oblivion_lottie_layers_hidden", "Скрыто" },
	{ "lng_oblivion_lottie_layers_parent", "Родитель: {name}" },
	{ "lng_oblivion_lottie_layers_select_parent", "Выделить родителя" },
	{ "lng_oblivion_lottie_layers_move_up", "Переместить выше" },
	{ "lng_oblivion_lottie_layers_move_down", "Переместить ниже" },
	{ "lng_oblivion_lottie_layers_expand_all", "Развернуть всё" },
	{ "lng_oblivion_lottie_layers_collapse_all", "Свернуть всё" },
	{ "lng_oblivion_lottie_layers_nothing_found", "Ничего не найдено" },
	{
		"lng_oblivion_lottie_layers_empty_hint",
		"Добавьте слой кнопкой «+» выше.",
	},
	{ "lng_oblivion_lottie_inspector_transform", "Трансформация" },
	{ "lng_oblivion_lottie_inspector_timing", "Время" },
	{ "lng_oblivion_lottie_inspector_parameters", "Параметры" },
	{ "lng_oblivion_lottie_inspector_in", "Начало" },
	{ "lng_oblivion_lottie_inspector_out", "Конец" },
	{ "lng_oblivion_lottie_inspector_start", "Сдвиг времени" },
	{ "lng_oblivion_lottie_inspector_stretch", "Растяжение времени" },
	{ "lng_oblivion_lottie_inspector_caps", "Концы линий" },
	{ "lng_oblivion_lottie_inspector_cap_butt", "Плоские" },
	{ "lng_oblivion_lottie_inspector_cap_round", "Круглые" },
	{ "lng_oblivion_lottie_inspector_cap_square", "Квадратные" },
	{ "lng_oblivion_lottie_inspector_joins", "Углы" },
	{ "lng_oblivion_lottie_inspector_join_miter", "Острые" },
	{ "lng_oblivion_lottie_inspector_join_round", "Скруглённые" },
	{ "lng_oblivion_lottie_inspector_join_bevel", "Срезанные" },
	{ "lng_oblivion_lottie_inspector_miter", "Предел остроты" },
	{ "lng_oblivion_lottie_inspector_fill_rule", "Правило заливки" },
	{ "lng_oblivion_lottie_inspector_rule_nonzero", "Ненулевое" },
	{ "lng_oblivion_lottie_inspector_rule_evenodd", "Чёт-нечет" },
	{ "lng_oblivion_lottie_inspector_trim_mode", "Несколько фигур" },
	{ "lng_oblivion_lottie_inspector_trim_together", "Все сразу" },
	{ "lng_oblivion_lottie_inspector_trim_individually", "По очереди" },
	{ "lng_oblivion_lottie_inspector_gradient_type", "Тип градиента" },
	{ "lng_oblivion_lottie_inspector_gradient_linear", "Линейный" },
	{ "lng_oblivion_lottie_inspector_gradient_radial", "Радиальный" },
	{
		"lng_oblivion_lottie_inspector_gradient_hint",
		"Перетаскивайте метки, чтобы сдвинуть точки градиента. "
		"Нажмите на цветную метку, чтобы сменить цвет.",
	},
	{ "lng_oblivion_lottie_inspector_star_type", "Форма" },
	{ "lng_oblivion_lottie_inspector_star_polygon", "Многоугольник" },
	{ "lng_oblivion_lottie_inspector_path_points", "Точек: {value}" },
	{ "lng_oblivion_lottie_inspector_key_add", "Добавить ключевой кадр" },
	{ "lng_oblivion_lottie_inspector_key_remove", "Удалить ключевой кадр" },
	{
		"lng_oblivion_lottie_inspector_key_prev",
		"Предыдущий ключевой кадр",
	},
	{
		"lng_oblivion_lottie_inspector_key_next",
		"Следующий ключевой кадр",
	},
	{ "lng_oblivion_lottie_inspector_animate", "Анимировать" },
	{ "lng_oblivion_lottie_inspector_unanimate", "Убрать анимацию" },
	{
		"lng_oblivion_lottie_inspector_expression",
		"Есть выражение — Telegram его не выполняет.",
	},
	{ "lng_oblivion_lottie_inspector_change_color", "Изменить цвет" },
	{
		"lng_oblivion_lottie_inspector_matte_source",
		"Служит маской для слоя ниже",
	},
	{ "lng_oblivion_lottie_inspector_width", "Ширина" },
	{ "lng_oblivion_lottie_inspector_height", "Высота" },
	{ "lng_oblivion_lottie_inspector_fps", "Частота кадров" },
	{ "lng_oblivion_lottie_inspector_duration", "Длительность, кадров" },
	{
		"lng_oblivion_lottie_inspector_scale_content",
		"Масштабировать содержимое вместе с холстом",
	},
	{
		"lng_oblivion_lottie_inspector_keep_duration",
		"Сохранять длительность в секундах",
	},
	{
		"lng_oblivion_lottie_inspector_edit_hint",
		"Потяните значение влево или вправо, чтобы изменить его, "
		"или нажмите, чтобы ввести число. С Shift — быстрее, "
		"с Option — точнее.",
	},
	{ "lng_oblivion_lottie_palette_tab", "Палитра" },
	{ "lng_oblivion_lottie_palette_colors", "Цвета" },
	{ "lng_oblivion_lottie_palette_empty", "В анимации нет цветов." },
	{ "lng_oblivion_lottie_palette_uses", "Использований: {value}" },
	{
		"lng_oblivion_lottie_palette_hint",
		"Нажмите на цвет, чтобы выделить всё, где он используется. "
		"Двойное нажатие — заменить его везде.",
	},
	{ "lng_oblivion_lottie_palette_edit", "Изменить…" },
	{ "lng_oblivion_lottie_palette_edit_menu", "Изменить цвет" },
	{ "lng_oblivion_lottie_palette_select", "Выделить" },
	{ "lng_oblivion_lottie_palette_copy", "Скопировать HEX" },
	{ "lng_oblivion_lottie_palette_replace_title", "Замена цвета" },
	{ "lng_oblivion_lottie_palette_adjust", "Коррекция цвета" },
	{ "lng_oblivion_lottie_palette_hue", "Оттенок" },
	{ "lng_oblivion_lottie_palette_saturation", "Насыщенность" },
	{ "lng_oblivion_lottie_palette_lightness", "Яркость" },
	{
		"lng_oblivion_lottie_palette_selection_only",
		"Только выделенные слои",
	},
	{ "lng_oblivion_lottie_palette_reset", "Сбросить" },
	// Tools: lottie editor canvas and timeline.
	{ "lng_oblivion_lottie_canvas_bg", "Фон холста" },
	{ "lng_oblivion_lottie_canvas_bg_checker", "Прозрачный" },
	{ "lng_oblivion_lottie_canvas_bg_dark", "Тёмный" },
	{ "lng_oblivion_lottie_canvas_bg_light", "Светлый" },
	{ "lng_oblivion_lottie_canvas_zoom", "Масштаб" },
	{ "lng_oblivion_lottie_canvas_zoom_in", "Увеличить" },
	{ "lng_oblivion_lottie_canvas_zoom_out", "Уменьшить" },
	{ "lng_oblivion_lottie_canvas_zoom_fit", "Вписать в окно" },
	{ "lng_oblivion_lottie_canvas_zoom_actual", "Реальный размер" },
	{ "lng_oblivion_lottie_canvas_percent", "{value}%" },
	{ "lng_oblivion_lottie_canvas_select_layer", "Выбрать весь слой" },
	{ "lng_oblivion_lottie_canvas_duplicate", "Дублировать" },
	{ "lng_oblivion_lottie_canvas_hide", "Скрыть" },
	{ "lng_oblivion_lottie_canvas_show", "Показать" },
	{ "lng_oblivion_lottie_canvas_delete", "Удалить" },
	{ "lng_oblivion_lottie_editor_export_png", "Текущий кадр в PNG…" },
	{ "lng_oblivion_lottie_editor_export_svg", "Текущий кадр в SVG…" },
	{ "lng_oblivion_lottie_editor_export_frame_title", "Экспорт кадра" },
	{ "lng_oblivion_lottie_editor_frame_file", "{name} — кадр {value}" },
	{ "lng_oblivion_lottie_editor_filter_png", "Изображение PNG" },
	{ "lng_oblivion_lottie_editor_filter_svg", "Изображение SVG" },
	{
		"lng_oblivion_lottie_editor_export_failed",
		"Не удалось экспортировать кадр.",
	},
	{ "lng_oblivion_lottie_timeline_first", "Первый кадр" },
	{ "lng_oblivion_lottie_timeline_previous", "Предыдущий кадр" },
	{ "lng_oblivion_lottie_timeline_next", "Следующий кадр" },
	{ "lng_oblivion_lottie_timeline_last", "Последний кадр" },
	{ "lng_oblivion_lottie_timeline_loop", "Повтор воспроизведения" },
	{ "lng_oblivion_lottie_timeline_zoom_in", "Растянуть шкалу" },
	{ "lng_oblivion_lottie_timeline_zoom_out", "Сжать шкалу" },
	{ "lng_oblivion_lottie_timeline_position", "{frame} · {time}" },
	{
		"lng_oblivion_lottie_timeline_add_keyframe",
		"Добавить ключевой кадр",
	},
	{
		"lng_oblivion_lottie_timeline_remove_keyframe",
		"Удалить ключевой кадр",
	},
	{
		"lng_oblivion_lottie_timeline_toggle_keyframe",
		"Добавить или удалить ключевой кадр в текущем кадре",
	},
	{ "lng_oblivion_lottie_timeline_go_to", "Перейти к этому кадру" },
	{ "lng_oblivion_lottie_timeline_copy", "Копировать" },
	{ "lng_oblivion_lottie_timeline_paste", "Вставить" },
	{ "lng_oblivion_lottie_timeline_delete", "Удалить" },
	{
		"lng_oblivion_lottie_timeline_select_all",
		"Выбрать все ключевые кадры",
	},
	{ "lng_oblivion_lottie_timeline_layer_start", "Начать слой здесь" },
	{ "lng_oblivion_lottie_timeline_layer_end", "Закончить слой здесь" },
	{
		"lng_oblivion_lottie_timeline_no_properties",
		"Нет анимированных свойств",
	},
	{
		"lng_oblivion_lottie_timeline_copied",
		"Скопировано ключевых кадров: {value}",
	},
	{
		"lng_oblivion_lottie_timeline_paste_failed",
		"Скопированные ключевые кадры сюда не подходят.",
	},
};

// Each extra feature appends its rows after its own marker comment.
const LangOverride kExtras[] = {
	{ "lng_oblivion_tracking_section", "Слежка" },
	{
		"lng_oblivion_tracking_about",
		"Журнал онлайна и история профилей хранятся только на этом "
		"устройстве и пополняются, пока Oblivion открыт. Telegram "
		"сообщает, кто в сети, только пока вы сами отображаетесь в "
		"сети, поэтому полный журнал ведётся лишь для людей из списка "
		"уведомлений о входе в сеть и только пока включён переключатель "
		"«Проверять статусы в фоне» ниже.",
	},
	// Extras: online.
	{ "lng_oblivion_online_journal", "Вести журнал онлайна" },
	{ "lng_oblivion_online_notify", "Уведомления о входе в сеть" },
	{ "lng_oblivion_online_polling", "Проверять статусы в фоне" },
	{
		"lng_oblivion_online_polling_about",
		"Раз в одну-две минуты Oblivion спрашивает у Telegram, в сети ли "
		"люди из списка уведомлений о входе в сеть. Если выключить, "
		"уведомления о входе в сеть и журнал онлайна не будут работать, "
		"пока вас скрывает режим призрака или окно Oblivion неактивно. "
		"Это дополнительный фоновый запрос, которого официальное "
		"приложение не отправляет.",
	},
	{ "lng_oblivion_online_menu_notify_on", "Уведомлять, когда в сети" },
	{
		"lng_oblivion_online_menu_notify_off",
		"Не уведомлять, когда в сети",
	},
	{ "lng_oblivion_online_menu_journal", "Журнал онлайна" },
	{
		"lng_oblivion_online_notify_on_toast",
		"Oblivion сообщит, когда {user} появится в сети.",
	},
	{
		"lng_oblivion_online_notify_off_toast",
		"Oblivion больше не сообщает, когда {user} в сети.",
	},
	{ "lng_oblivion_online_now", "{user} в сети" },
	{ "lng_oblivion_online_now_text", "сейчас в сети" },
	{ "lng_oblivion_online_now_hidden", "Кто-то из вашего списка в сети" },
	{ "lng_oblivion_online_now_list", "Сейчас в сети: {users}" },
	{ "lng_oblivion_online_now_more", "{users} и ещё {value}" },
	{ "lng_oblivion_online_list_add", "Добавить пользователя" },
	{ "lng_oblivion_online_list_choose", "Выберите пользователя" },
	{
		"lng_oblivion_online_list_about",
		"Oblivion сообщит, когда эти люди появятся в сети, — не чаще "
		"раза в 10 минут о каждом. Пока в настройках (Oblivion → Слежка) "
		"включено «Проверять статусы в фоне», Oblivion запрашивает их "
		"статусы раз в одну-две минуты, даже когда его окно неактивно, "
		"поэтому уведомление может прийти с задержкой в минуту-две. Если "
		"аккаунтов несколько, каждого человека проверяет только один из "
		"них — обычно тот, которым вы сейчас пользуетесь, — поэтому в "
		"других аккаунтах в журнале этого человека возможны пропуски. "
		"Добавить человека можно и из меню чата с ним. Нажмите на "
		"строку, чтобы открыть журнал онлайна.",
	},
	{ "lng_oblivion_online_list_empty", "Здесь пока никого нет." },
	{ "lng_oblivion_online_list_unknown", "Пользователь {value}" },
	{
		"lng_oblivion_online_list_unknown_status",
		"нет данных: откройте профиль",
	},
	{
		"lng_oblivion_online_list_other_account",
		"отслеживается из другого аккаунта",
	},
	{ "lng_oblivion_online_journal_week", "7 дней" },
	{ "lng_oblivion_online_journal_month", "30 дней" },
	{ "lng_oblivion_online_journal_total", "всего в сети" },
	{ "lng_oblivion_online_journal_average", "в среднем за день" },
	{ "lng_oblivion_online_journal_visits", "число заходов" },
	{ "lng_oblivion_online_journal_typical", "Обычно в сети: {hours}" },
	{
		"lng_oblivion_online_journal_typical_none",
		"Пока мало данных, чтобы назвать обычные часы.",
	},
	{ "lng_oblivion_online_journal_by_hours", "По часам" },
	{ "lng_oblivion_online_journal_today", "Сегодня" },
	{ "lng_oblivion_online_journal_yesterday", "Вчера" },
	{ "lng_oblivion_online_journal_legend_online", "в сети" },
	{ "lng_oblivion_online_journal_legend_offline", "не в сети" },
	{ "lng_oblivion_online_journal_legend_unknown", "нет данных" },
	{
		"lng_oblivion_online_journal_empty",
		"Пока нет данных. Журнал пополняется, пока Oblivion открыт.",
	},
	{
		"lng_oblivion_online_journal_empty_hidden",
		"Пока нет данных. Этот пользователь скрывает время захода, "
		"поэтому в журнал попадают только моменты, когда он пишет или "
		"печатает в ваших общих чатах.",
	},
	{
		"lng_oblivion_online_journal_disabled",
		"Журнал онлайна выключен. Включить его можно в настройках: "
		"Oblivion → Слежка.",
	},
	{
		"lng_oblivion_online_journal_hidden_note",
		"Время захода скрыто, поэтому журнал неполный.",
	},
	{
		"lng_oblivion_online_journal_partial_note",
		"Журнал может быть неполным: Telegram сообщает статус этого "
		"человека, только пока вы сами отображаетесь в сети. Включите "
		"«Уведомлять, когда в сети» ниже — и Oblivion будет сам "
		"запрашивать статус раз в одну-две минуты (если в настройках "
		"Oblivion → Слежка включено «Проверять статусы в фоне»).",
	},
	{ "lng_oblivion_online_journal_clear", "Очистить" },
	{
		"lng_oblivion_online_journal_clear_sure",
		"Очистить журнал онлайна пользователя {user}?",
	},
	{ "lng_oblivion_online_duration_hm", "{hours} ч {minutes} мин" },
	{ "lng_oblivion_online_duration_h", "{hours} ч" },
	{ "lng_oblivion_online_duration_m", "{minutes} мин" },
	{ "lng_oblivion_online_duration_less", "< 1 мин" },
	{ "lng_oblivion_online_tip_since", "в сети с {time}" },
	{ "lng_oblivion_online_tip_seen", "был(а) в сети в {time}" },
	{
		"lng_oblivion_online_tip_unknown",
		"Нет данных: Oblivion был закрыт или без связи, либо этот "
		"аккаунт не отображался в сети и не проверял этого человека",
	},
	{ "lng_oblivion_online_tip_offline", "{time} · не в сети" },
	// Extras: profile history.
	{ "lng_oblivion_profile_history", "Сохранять историю профилей" },
	{ "lng_oblivion_profile_history_menu", "История профиля" },
	{ "lng_oblivion_profile_history_title", "История профиля" },
	{ "lng_oblivion_profile_history_since", "Отслеживается с {date}" },
	{
		"lng_oblivion_profile_history_off",
		"Запись выключена в настройках Oblivion.",
	},
	{
		"lng_oblivion_profile_history_contact",
		"Имя контакта вы задаёте сами, поэтому его смены не записываются.",
	},
	{ "lng_oblivion_profile_history_noticed", "Замечено {date}" },
	{ "lng_oblivion_profile_history_name", "Имя" },
	{ "lng_oblivion_profile_history_chat_title", "Название" },
	{ "lng_oblivion_profile_history_username", "Имя пользователя" },
	{ "lng_oblivion_profile_history_public_link", "Публичная ссылка" },
	{ "lng_oblivion_profile_history_bio", "О себе" },
	{ "lng_oblivion_profile_history_description", "Описание" },
	{ "lng_oblivion_profile_history_photo_changed", "Фото обновлено" },
	{ "lng_oblivion_profile_history_photo_removed", "Фото удалено" },
	{ "lng_oblivion_profile_history_photo_set", "Фото установлено" },
	{
		"lng_oblivion_profile_history_photo_missing",
		"Старое фото не сохранилось.",
	},
	{ "lng_oblivion_profile_history_deleted", "Аккаунт удалён" },
	{ "lng_oblivion_profile_history_was", "Было:" },
	{ "lng_oblivion_profile_history_now", "Стало:" },
	{ "lng_oblivion_profile_history_none", "нет" },
	{ "lng_oblivion_profile_history_empty", "Изменений пока нет" },
	{
		"lng_oblivion_profile_history_empty_about",
		"Oblivion запомнил этот профиль и покажет здесь смены имени, "
		"имени пользователя, описания и фото.",
	},
	{
		"lng_oblivion_profile_history_empty_waiting",
		"Oblivion ждёт свежие данные этого профиля. "
		"Отслеживание начнётся, как только они придут.",
	},
	{
		"lng_oblivion_profile_history_empty_deleted",
		"Этот аккаунт удалён, отслеживать больше нечего.",
	},
	{
		"lng_oblivion_profile_history_about",
		"Время показывает, когда Oblivion заметил изменение. "
		"История хранится только на этом устройстве.",
	},
	{ "lng_oblivion_profile_history_clear", "Очистить" },
	{
		"lng_oblivion_profile_history_clear_sure",
		"Очистить сохранённую историю этого профиля? "
		"Сохранённые старые фото тоже будут удалены.",
	},
	// Extras: ghost button.
	{ "lng_oblivion_ghost_button", "Кнопка режима призрака над чатами" },
	{ "lng_oblivion_ghost_button_on", "Режим призрака включён" },
	{ "lng_oblivion_ghost_button_off", "Режим призрака выключен" },
	{
		"lng_oblivion_ghost_button_partial",
		"Режим призрака включён частично",
	},
	{
		"lng_oblivion_ghost_button_hint",
		"Правый клик — выбрать, что переключает кнопка.",
	},
	{ "lng_oblivion_ghost_button_read", "Скрывать прочтение сообщений" },
	{ "lng_oblivion_ghost_button_typing", "Скрывать статус «печатает»" },
	{ "lng_oblivion_ghost_button_online", "Скрывать статус «в сети»" },
	{ "lng_oblivion_ghost_button_stories", "Скрывать просмотр историй" },
	{
		"lng_oblivion_ghost_button_sending",
		"Отправлять, не появляясь в сети",
	},
	{
		"lng_oblivion_ghost_button_about",
		"Кнопка включает и выключает отмеченные пункты одним нажатием.",
	},
	{ "lng_oblivion_ghost_button_settings", "Настройки режима призрака" },
	{
		"lng_oblivion_ghost_button_last",
		"Должен остаться отмеченным хотя бы один пункт.",
	},
	// Extras: vision.
	{ "lng_oblivion_vision_text_context", "Скопировать текст с фото" },
	{ "lng_oblivion_vision_text_title", "Текст с фото" },
	{ "lng_oblivion_vision_text_progress", "Распознаём текст…" },
	{ "lng_oblivion_vision_text_empty", "Текст не найден" },
	{
		"lng_oblivion_vision_text_empty_about",
		"На этом изображении нет текста, который удалось бы распознать.",
	},
	{ "lng_oblivion_vision_text_failed", "Не удалось распознать текст." },
	{ "lng_oblivion_vision_text_copy", "Копировать" },
	{
		"lng_oblivion_vision_text_copied",
		"Текст скопирован в буфер обмена.",
	},
	{ "lng_oblivion_vision_text_lines#one", "Распознана {count} строка" },
	{ "lng_oblivion_vision_text_lines#few", "Распознано {count} строки" },
	{ "lng_oblivion_vision_text_lines#many", "Распознано {count} строк" },
	{ "lng_oblivion_vision_text_lines#other", "Распознано {count} строки" },
	{
		"lng_oblivion_vision_slow",
		"Первый запуск может занять до минуты: macOS подготавливает модель.",
	},
	{ "lng_oblivion_vision_cutout_context", "Вырезать объект" },
	{ "lng_oblivion_vision_cutout_title", "Вырезать объект" },
	{ "lng_oblivion_vision_cutout_progress", "Убираем фон…" },
	{
		"lng_oblivion_vision_cutout_not_found",
		"Не удалось найти объект на этом изображении.",
	},
	{ "lng_oblivion_vision_cutout_crop", "Обрезать по границам объекта" },
	{ "lng_oblivion_vision_cutout_copy", "Скопировать" },
	{ "lng_oblivion_vision_cutout_save", "Сохранить PNG" },
	{ "lng_oblivion_vision_cutout_sticker", "Сделать стикер" },
	{ "lng_oblivion_vision_cutout_edit", "Открыть в фоторедакторе" },
	{ "lng_oblivion_vision_editor_title", "Фон" },
	{ "lng_oblivion_vision_editor_remove", "Убрать фон" },
	{ "lng_oblivion_vision_editor_restore", "Вернуть фон" },
	{ "lng_oblivion_vision_editor_progress", "Убираем фон…" },
	{
		"lng_oblivion_vision_editor_hint",
		"Оставляет объект, а фон делает прозрачным. "
		"Прозрачность сохраняется в PNG и WebP.",
	},
	// Extras: sticker packs.
	{ "lng_oblivion_tools_sticker_packs", "Мои наборы стикеров" },
	{ "lng_oblivion_packs_create", "Создать новый набор" },
	{ "lng_oblivion_packs_loading", "Загружаем ваши наборы…" },
	{ "lng_oblivion_packs_failed", "Не удалось загрузить список наборов." },
	{ "lng_oblivion_packs_retry", "Повторить" },
	{
		"lng_oblivion_packs_empty",
		"У вас пока нет своих наборов стикеров. Создайте набор "
		"и добавляйте в него стикеры из фото, анимаций и видео.",
	},
	{
		"lng_oblivion_packs_about",
		"Наборы создаются прямо здесь, без бота @stickers. "
		"Добавить ваш набор может любой, у кого есть ссылка на него.",
	},
	{ "lng_oblivion_packs_count#one", "{count} стикер" },
	{ "lng_oblivion_packs_count#few", "{count} стикера" },
	{ "lng_oblivion_packs_count#many", "{count} стикеров" },
	{ "lng_oblivion_packs_count#other", "{count} стикера" },
	{ "lng_oblivion_packs_count_emoji#one", "{count} эмодзи" },
	{ "lng_oblivion_packs_count_emoji#few", "{count} эмодзи" },
	{ "lng_oblivion_packs_count_emoji#many", "{count} эмодзи" },
	{ "lng_oblivion_packs_count_emoji#other", "{count} эмодзи" },
	{ "lng_oblivion_packs_open", "Открыть набор" },
	{ "lng_oblivion_packs_add_file", "Добавить стикер из файла…" },
	{ "lng_oblivion_packs_copy_link", "Скопировать ссылку" },
	{ "lng_oblivion_packs_link_copied", "Ссылка скопирована." },
	{ "lng_oblivion_packs_rename", "Переименовать" },
	{ "lng_oblivion_packs_renamed", "Набор переименован." },
	{ "lng_oblivion_packs_delete", "Удалить набор" },
	{
		"lng_oblivion_packs_delete_sure",
		"Удалить набор «{title}» для всех? Это действие нельзя отменить.",
	},
	{ "lng_oblivion_packs_deleted", "Набор удалён." },
	{ "lng_oblivion_packs_action_failed", "Что-то пошло не так ({error})." },
	{ "lng_oblivion_packs_create_title", "Новый набор стикеров" },
	{ "lng_oblivion_packs_rename_title", "Переименовать набор" },
	{ "lng_oblivion_packs_field_title", "Название набора" },
	{ "lng_oblivion_packs_field_name", "Короткое имя для ссылки" },
	{
		"lng_oblivion_packs_create_about",
		"Набор будет создан, когда вы добавите в него первый стикер.",
	},
	{
		"lng_oblivion_packs_name_hint",
		"Латинские буквы, цифры и подчёркивания. Ссылка на набор "
		"будет такой: t.me/addstickers/имя.",
	},
	{ "lng_oblivion_packs_name_checking", "Проверяем имя…" },
	{ "lng_oblivion_packs_name_available", "Имя свободно: {link}" },
	{ "lng_oblivion_packs_name_occupied", "Это имя уже занято." },
	{ "lng_oblivion_packs_name_invalid", "Такое имя использовать нельзя." },
	{
		"lng_oblivion_packs_name_start",
		"Имя должно начинаться с латинской буквы.",
	},
	{
		"lng_oblivion_packs_name_symbols",
		"Можно использовать только латинские буквы, цифры и подчёркивания.",
	},
	{
		"lng_oblivion_packs_name_underscores",
		"Нельзя ставить два подчёркивания подряд и подчёркивание в конце.",
	},
	{ "lng_oblivion_packs_name_long", "Имя слишком длинное." },
	{
		"lng_oblivion_packs_name_unchecked",
		"Сейчас проверить имя не удалось, оно будет проверено "
		"при создании набора: {link}",
	},
	{ "lng_oblivion_packs_next", "Далее" },
	{ "lng_oblivion_packs_add_title", "Добавить в набор стикеров" },
	{ "lng_oblivion_packs_emoji_about", "Выберите эмодзи для стикера" },
	{ "lng_oblivion_packs_preparing", "Готовим стикер…" },
	{ "lng_oblivion_packs_kind_static", "Статичный стикер" },
	{ "lng_oblivion_packs_kind_animated", "Анимированный стикер" },
	{ "lng_oblivion_packs_kind_video", "Видеостикер" },
	{ "lng_oblivion_packs_outline", "Белая обводка" },
	{ "lng_oblivion_packs_target", "Набор" },
	{ "lng_oblivion_packs_target_new", "Новый набор…" },
	{ "lng_oblivion_packs_target_new_named", "Новый: {title}" },
	{ "lng_oblivion_packs_target_loading", "Загрузка…" },
	{ "lng_oblivion_packs_target_choose", "Выбрать" },
	{ "lng_oblivion_packs_add_button", "Добавить" },
	{
		"lng_oblivion_packs_emoji_required",
		"Выберите хотя бы один эмодзи.",
	},
	{ "lng_oblivion_packs_not_ready", "Стикер ещё готовится." },
	{
		"lng_oblivion_packs_bad_image",
		"Не удалось сделать стикер из этого изображения.",
	},
	{ "lng_oblivion_packs_bad_lottie", "Этот файл — не анимация Lottie." },
	{
		"lng_oblivion_packs_bad_video",
		"Это видео — не готовый видеостикер: нужен WebM (VP9) без звука, "
		"не длиннее 3 секунд, не больше 30 кадров в секунду и 256 КБ, "
		"со стороной 512 пикселей.",
	},
	{
		"lng_oblivion_packs_lottie_errors",
		"Telegram не примет эту анимацию:",
	},
	{ "lng_oblivion_packs_lottie_fix", "Исправить автоматически" },
	{
		"lng_oblivion_packs_lottie_unfixable",
		"Часть замечаний нельзя исправить автоматически. "
		"Откройте анимацию в редакторе Lottie.",
	},
	{ "lng_oblivion_packs_lottie_more", "…и ещё: {value}" },
	{
		"lng_oblivion_packs_full",
		"В этом наборе уже {max} стикеров — больше добавить нельзя.",
	},
	{ "lng_oblivion_packs_uploading", "Загружаем стикер… {percent}" },
	{ "lng_oblivion_packs_adding", "Добавляем в набор…" },
	{ "lng_oblivion_packs_creating", "Создаём набор…" },
	{
		"lng_oblivion_packs_added",
		"Стикер добавлен в набор «{title}». {link}",
	},
	{ "lng_oblivion_packs_created", "Набор «{title}» создан. {link}" },
	{
		"lng_oblivion_packs_upload_failed",
		"Не удалось загрузить файл. Проверьте соединение "
		"и попробуйте ещё раз.",
	},
	{
		"lng_oblivion_packs_error_title",
		"Такое название набора использовать нельзя.",
	},
	{
		"lng_oblivion_packs_error_file",
		"Telegram не принял этот файл как стикер ({error}).",
	},
	{ "lng_oblivion_packs_error_emoji", "Telegram не принял эти эмодзи." },
	{
		"lng_oblivion_packs_error_flood",
		"Слишком много запросов. Попробуйте позже.",
	},
	{
		"lng_oblivion_packs_error_generic",
		"Не удалось добавить стикер ({error}).",
	},
	{ "lng_oblivion_packs_choose_file", "Выберите файл для стикера" },
	{ "lng_oblivion_packs_filter", "Изображения, анимации и видео" },
	{
		"lng_oblivion_packs_file_unknown",
		"Из этого файла нельзя сделать стикер.",
	},
	{ "lng_oblivion_packs_file_failed", "Не удалось прочитать файл." },
	{
		"lng_oblivion_packs_no_account",
		"Войдите в аккаунт, чтобы добавлять стикеры в набор.",
	},
	{ "lng_oblivion_packs_context_sticker", "Сделать стикер" },
	{ "lng_oblivion_packs_context_video", "Сделать видеостикер" },
	{ "lng_oblivion_tools_sticker_converter", "Конвертер в стикеры" },
	{ "lng_oblivion_packs_export", "Файл" },
	{ "lng_oblivion_packs_export_save", "Сохранить файл…" },
	{ "lng_oblivion_packs_export_send", "Отправить файлом…" },
	{ "lng_oblivion_packs_export_bot", "Отправить боту @Stickers" },
	{
		"lng_oblivion_packs_export_about",
		"Кнопка «Файл» сохранит стикер или отправит его файлом — "
		"например, боту @Stickers.",
	},
	{ "lng_oblivion_packs_export_save_title", "Сохранить файл стикера" },
	{ "lng_oblivion_packs_export_send_title", "Отправить файл" },
	{ "lng_oblivion_packs_export_sent", "Файл отправлен в «{chat}»." },
	{
		"lng_oblivion_packs_export_restricted",
		"В этот чат нельзя отправлять файлы.",
	},
	{
		"lng_oblivion_packs_export_bot_failed",
		"Не удалось найти бота @Stickers. Проверьте соединение "
		"и попробуйте ещё раз.",
	},
	{ "lng_oblivion_packs_export_failed", "Не удалось подготовить файл." },
	{ "lng_oblivion_vsticker_title", "Видеостикер" },
	{
		"lng_oblivion_vsticker_limit",
		"Видеостикер длится не больше 3 секунд и не содержит звука. "
		"Передвиньте рамку на полосе, чтобы выбрать фрагмент. Стрелки "
		"на клавиатуре сдвигают его на 0,1 с, вместе с Shift — "
		"на секунду.",
	},
	{ "lng_oblivion_vsticker_step_back", "← 0,1 с" },
	{ "lng_oblivion_vsticker_step_forward", "0,1 с →" },
	{ "lng_oblivion_vsticker_square", "Обрезать до квадрата" },
	{ "lng_oblivion_vsticker_zoom", "Масштаб: {value}" },
	{
		"lng_oblivion_vsticker_crop_about",
		"Перетащите рамку на кадре, чтобы выбрать, "
		"что попадёт в стикер.",
	},
	{
		"lng_oblivion_vsticker_failed",
		"Не удалось сделать видеостикер из этого видео.",
	},
	// Extras: video core.
	// Extras: video editor.
	{ "lng_oblivion_tools_video_editor", "Видеоредактор" },
	{ "lng_oblivion_video_open_in", "Открыть в видеоредакторе" },
	{ "lng_oblivion_video_choose", "Выберите видео" },
	{ "lng_oblivion_video_filter_video", "Видеофайлы" },
	{ "lng_oblivion_video_filter_all", "Все файлы" },
	{ "lng_oblivion_video_default_name", "видео" },
	{ "lng_oblivion_video_downloading", "Загрузка видео… {percent}" },
	{ "lng_oblivion_video_opening", "Открываем видео…" },
	{ "lng_oblivion_video_open_failed", "Не удалось открыть это видео." },
	{
		"lng_oblivion_video_download_failed",
		"Не удалось загрузить видео.",
	},
	{ "lng_oblivion_video_add_failed", "Не удалось открыть {name}." },
	{ "lng_oblivion_video_too_many", "Клипов может быть не больше {max}." },
	{ "lng_oblivion_video_split", "Разрезать" },
	{ "lng_oblivion_video_delete", "Удалить" },
	{ "lng_oblivion_video_rotate", "Повернуть" },
	{ "lng_oblivion_video_add", "Добавить видео" },
	{
		"lng_oblivion_video_hint",
		"Потяните за края выбранного клипа, чтобы обрезать его. "
		"Пробел — воспроизведение, S — разрезать клип там, где стоит "
		"указатель.",
	},
	{ "lng_oblivion_video_clip", "Клип {index} из {total}" },
	{ "lng_oblivion_video_frame", "Кадр" },
	{ "lng_oblivion_video_aspect_original", "Целиком" },
	{ "lng_oblivion_video_aspect_free", "Свободно" },
	{ "lng_oblivion_video_speed", "Скорость" },
	{ "lng_oblivion_video_mute", "Без звука" },
	{ "lng_oblivion_video_export", "Экспорт" },
	{ "lng_oblivion_video_export_title", "Экспорт видео" },
	{ "lng_oblivion_video_format_mp4", "Видео MP4" },
	{ "lng_oblivion_video_format_gifv", "GIF для Telegram" },
	{ "lng_oblivion_video_format_gif", "Файл GIF" },
	{ "lng_oblivion_video_format_sticker", "Видеостикер" },
	{ "lng_oblivion_video_format_emoji", "Эмодзи" },
	{
		"lng_oblivion_video_mp4_about",
		"Видео H.264 со звуком AAC. Воспроизводится везде и отправляется "
		"как обычное видео.",
	},
	{
		"lng_oblivion_video_gifv_about",
		"Видео без звука: Telegram показывает его как зацикленный GIF "
		"и позволяет сохранить в свои GIF.",
	},
	{
		"lng_oblivion_video_gif_about",
		"Настоящий файл .gif для других приложений и сайтов. Такие файлы "
		"получаются большими, поэтому картинка маленькая, а ролик может "
		"длиться не больше минуты.",
	},
	{
		"lng_oblivion_video_sticker_about",
		"Видеостикер для вашего набора стикеров: 512 пикселей, без звука, "
		"до 3 секунд. От более длинного ролика берутся первые три секунды.",
	},
	{
		"lng_oblivion_video_emoji_about",
		"Видеоэмодзи: 100×100 пикселей, без звука, до 3 секунд. Сохраните "
		"файл и добавьте его в набор эмодзи через @Stickers.",
	},
	{ "lng_oblivion_video_size", "Размер" },
	{ "lng_oblivion_video_size_px", "{value} px" },
	{ "lng_oblivion_video_fps", "Частота кадров" },
	{ "lng_oblivion_video_fps_value", "{value} кадр/с" },
	{ "lng_oblivion_video_summary", "Результат: {size}, {duration}" },
	{
		"lng_oblivion_video_summary_size",
		"Результат: {size}, {duration}, примерно {bytes}",
	},
	{ "lng_oblivion_video_exporting", "Экспорт… {percent}" },
	{ "lng_oblivion_video_export_done", "Готово: {bytes}" },
	{
		"lng_oblivion_video_export_failed",
		"Не удалось экспортировать видео.",
	},
	{
		"lng_oblivion_video_too_long",
		"Ролик слишком длинный для этого формата: можно не больше {max}.",
	},
	{ "lng_oblivion_video_save", "Сохранить" },
	{ "lng_oblivion_video_save_title", "Сохранить видео" },
	{ "lng_oblivion_video_file_mp4", "Видео MP4" },
	{ "lng_oblivion_video_file_gif", "Изображение GIF" },
	{ "lng_oblivion_video_file_webm", "Видео WebM" },
	{ "lng_oblivion_video_send", "Отправить" },
	{ "lng_oblivion_video_send_to", "Отправить в «{chat}»" },
	{ "lng_oblivion_video_send_choose", "Выбрать чат…" },
	{ "lng_oblivion_video_send_title", "Отправить видео…" },
	{
		"lng_oblivion_video_send_restricted",
		"В этот чат нельзя отправлять видео.",
	},
	{
		"lng_oblivion_video_send_too_large",
		"Файл слишком большой для отправки.",
	},
	{ "lng_oblivion_video_sent", "Отправлено в «{chat}»." },
	{ "lng_oblivion_video_to_pack", "В набор стикеров" },
	{
		"lng_oblivion_video_too_many_files",
		"Видеофайлов может быть не больше {max}.",
	},
	{ "lng_oblivion_video_writing", "Запись файла…" },
	{ "lng_oblivion_video_move_left", "Сдвинуть клип влево" },
	{ "lng_oblivion_video_move_right", "Сдвинуть клип вправо" },
	{ "lng_oblivion_video_undo", "Отменить" },
	{ "lng_oblivion_video_redo", "Повторить" },
	{
		"lng_oblivion_video_close_sure",
		"Закрыть видеоредактор? Изменения, которые не были "
		"экспортированы, будут потеряны.",
	},
	{
		"lng_oblivion_video_project_kept",
		"Видеоредактор был закрыт. Проект хранится до выхода из "
		"приложения или из этого аккаунта: откройте видеоредактор, чтобы "
		"продолжить.",
	},
	{
		"lng_oblivion_video_already_open",
		"Видеоредактор уже открыт. Закройте его, чтобы открыть другое "
		"видео.",
	},
	{
		"lng_oblivion_video_restore_text",
		"В видеоредакторе остался незавершённый проект. Продолжить работу "
		"над ним или начать новый? Если начать новый, незавершённый "
		"проект будет удалён.",
	},
	{ "lng_oblivion_video_restore_continue", "Продолжить" },
	{ "lng_oblivion_video_restore_new", "Новый проект" },
	{
		"lng_oblivion_video_export_background",
		"Экспорт продолжается в фоне. Видео будет сохранено в файл {path}",
	},
	{
		"lng_oblivion_video_export_saved",
		"Экспорт завершён, видео сохранено в файл {path}",
	},
	{
		"lng_oblivion_video_quit_exporting",
		"Экспорт видео ещё не завершён. Выйти и отменить экспорт?",
	},
	{
		"lng_oblivion_video_quit_unsaved",
		"В видеоредакторе есть изменения, которые не были "
		"экспортированы. Выйти и потерять их?",
	},
	{
		"lng_oblivion_video_quit_writing",
		"Файл ещё записывается, обычно это занимает несколько секунд. "
		"Если выйти сейчас, файл останется неполным. Всё равно выйти?",
	},
	{ "lng_oblivion_video_quit", "Выйти" },
	// Extras: audio extras.
	{ "lng_oblivion_voice_noise", "Подавлять шум в голосовых" },
	{ "lng_oblivion_voice_noise_toggle", "Шумоподавление" },
	{
		"lng_oblivion_voice_noise_about",
		"Шумоподавление убирает фоновые звуки из голосовых сообщений "
		"перед отправкой. Работает с любым эффектом и без него.",
	},
	{
		"lng_oblivion_voice_noise_on",
		"Шумоподавление для голосовых включено.",
	},
	{
		"lng_oblivion_voice_noise_off",
		"Шумоподавление для голосовых выключено.",
	},
	{
		"lng_oblivion_voice_noise_applying",
		"Убираем шум из голосового…",
	},
	{
		"lng_oblivion_voice_noise_failed",
		"Не удалось убрать шум, голосовое отправлено как записано.",
	},
	{
		"lng_oblivion_voice_noise_preview_failed",
		"Не удалось убрать шум из записи.",
	},
	{ "lng_oblivion_voice_effect_no_sample", "Пока ничего не записано." },
	{
		"lng_oblivion_voice_quit_wait",
		"Голосовое сообщение ещё обрабатывается. Oblivion закроется, "
		"как только оно отправится. Чтобы отменить, закройте приложение "
		"ещё раз и выберите «Не закрывать».",
	},
	{
		"lng_oblivion_voice_quit_sure",
		"Голосовое сообщение ещё обрабатывается и пока не отправлено. "
		"Oblivion закроется сам, как только оно отправится, нажимать "
		"ничего не нужно. Если закрыть приложение сейчас, сообщение "
		"будет потеряно.",
	},
	{ "lng_oblivion_voice_quit_now", "Закрыть сейчас" },
	{ "lng_oblivion_voice_quit_cancel", "Не закрывать" },
	{
		"lng_oblivion_voice_logout_wait",
		"Голосовое сообщение ещё обрабатывается. Выход из аккаунта "
		"произойдёт, как только оно отправится.",
	},
	{
		"lng_oblivion_voice_noise_too_long",
		"Шумоподавление работает только для голосовых длиной до 15 минут, "
		"это останется как записано.",
	},
	{ "lng_oblivion_music_denoise", "Шумоподавление (для речи)" },
	{ "lng_oblivion_save_voice_as", "Сохранить голосовое как…" },
	{ "lng_oblivion_save_voice_title", "Сохранить голосовое" },
	{ "lng_oblivion_save_voice_name", "Голосовое {name} {date}" },
	{ "lng_oblivion_save_round_as", "Сохранить кружок как MP4" },
	{ "lng_oblivion_save_round_title", "Сохранить кружок" },
	{ "lng_oblivion_save_round_name", "Кружок {name} {date}" },
	{ "lng_oblivion_save_file_mp4", "Видео MP4" },
	{
		"lng_oblivion_save_downloading",
		"Скачиваем, файл сохранится через мгновение…",
	},
	{
		"lng_oblivion_save_download_failed",
		"Не удалось скачать файл, ничего не сохранено.",
	},
	{
		"lng_oblivion_save_convert_failed",
		"Не удалось преобразовать это голосовое.",
	},
	{
		"lng_oblivion_save_write_failed",
		"Не удалось записать файл в {path}",
	},
	// Extras: unified chats.
	{ "lng_oblivion_unified_chats", "Общий список чатов всех аккаунтов" },
	{ "lng_oblivion_unified_menu", "Чаты всех аккаунтов" },
	{ "lng_oblivion_unified_title", "Все аккаунты" },
	{ "lng_oblivion_unified_account", "Аккаунт: {name}" },
	{ "lng_oblivion_unified_open_in", "Открыть в аккаунте «{name}»" },
	{ "lng_oblivion_unified_turn_off", "Выключить общий список чатов" },
	{
		"lng_oblivion_unified_off_toast",
		"Общий список чатов выключен. Включить его снова можно "
		"в списке аккаунтов главного меню.",
	},
	{ "lng_oblivion_unified_loading", "Загружаем чаты…" },
	{ "lng_oblivion_unified_empty", "Чатов пока нет" },
};

// Each round 4 feature appends its rows after its own marker comment,
// the markers are the same as in lang.strings.
const LangOverride kRound4[] = {
	// Oblivion round 4: photo layers.
	{ "lng_oblivion_photo_layers_title", "Слои" },
	{ "lng_oblivion_photo_layers_add", "Добавить слой" },
	{ "lng_oblivion_photo_layers_more", "Действия со слоем" },
	{ "lng_oblivion_photo_layers_empty", "Слоёв пока нет." },
	{ "lng_oblivion_photo_layers_paste", "Вставить из буфера обмена" },
	{ "lng_oblivion_photo_layers_paste_empty", "В буфере обмена нет изображения." },
	{ "lng_oblivion_photo_layers_rename", "Переименовать" },
	{ "lng_oblivion_photo_layers_duplicate", "Дублировать" },
	{ "lng_oblivion_photo_layers_show", "Показать слой" },
	{ "lng_oblivion_photo_layers_hide", "Скрыть слой" },
	{ "lng_oblivion_photo_layers_lock", "Заблокировать слой" },
	{ "lng_oblivion_photo_layers_unlock", "Разблокировать слой" },
	{ "lng_oblivion_photo_layers_locked", "Слой заблокирован. Сначала разблокируйте его в списке слоёв." },
	{ "lng_oblivion_photo_layers_hidden", "Слой скрыт. Сначала включите его в списке слоёв." },
	{ "lng_oblivion_photo_layers_blend_menu", "Режим наложения" },
	{ "lng_oblivion_photo_layers_opacity_menu", "Непрозрачность" },
	{ "lng_oblivion_photo_layers_percent", "{value}%" },
	{ "lng_oblivion_photo_layers_blend_info", "{name}, {percent}" },
	{ "lng_oblivion_photo_layers_move_up", "Поднять выше" },
	{ "lng_oblivion_photo_layers_move_down", "Опустить ниже" },
	{ "lng_oblivion_photo_layers_merge_down", "Объединить с нижним" },
	{ "lng_oblivion_photo_layers_flatten", "Свести все слои" },
	{ "lng_oblivion_photo_layers_merging", "Объединяем слои…" },
	{ "lng_oblivion_photo_layers_flattening", "Сводим слои…" },
	{ "lng_oblivion_photo_layers_delete", "Удалить слой" },
	{ "lng_oblivion_photo_layers_delete_last", "Единственный слой удалить нельзя." },
	{ "lng_oblivion_photo_layers_transform", "Переместить и изменить размер" },
	{ "lng_oblivion_photo_layers_mask_add", "Добавить маску" },
	{ "lng_oblivion_photo_layers_mask_paint", "Рисовать по маске" },
	{ "lng_oblivion_photo_layers_mask_invert", "Инвертировать маску" },
	{ "lng_oblivion_photo_layers_mask_disable", "Отключить маску" },
	{ "lng_oblivion_photo_layers_mask_enable", "Включить маску" },
	{ "lng_oblivion_photo_layers_mask_remove", "Удалить маску" },
	{ "lng_oblivion_photo_layers_tool_transform", "Перемещение, размер и поворот слоя" },
	{ "lng_oblivion_photo_layers_tool_mask", "Маска слоя" },
	{ "lng_oblivion_photo_layers_tf_mode_free", "Размер и поворот" },
	{ "lng_oblivion_photo_layers_tf_mode_perspective", "Перспектива" },
	{
		"lng_oblivion_photo_layers_tf_hint",
		"Перетаскивайте слой, чтобы двигать его. Квадраты на рамке "
		"меняют размер, круглая ручка поворачивает. Shift "
		"переключает сохранение пропорций, {shortcut} с углом свободно "
		"искажает слой.",
	},
	{
		"lng_oblivion_photo_layers_tf_hint_perspective",
		"Перетаскивайте углы, чтобы задать перспективу. Сам слой можно "
		"двигать как обычно.",
	},
	{ "lng_oblivion_photo_layers_tf_section", "Положение и размер" },
	{ "lng_oblivion_photo_layers_tf_reset", "Сбросить" },
	{ "lng_oblivion_photo_layers_tf_x", "Сдвиг по горизонтали" },
	{ "lng_oblivion_photo_layers_tf_y", "Сдвиг по вертикали" },
	{ "lng_oblivion_photo_layers_tf_width", "Ширина" },
	{ "lng_oblivion_photo_layers_tf_height", "Высота" },
	{ "lng_oblivion_photo_layers_tf_rotation", "Поворот" },
	{ "lng_oblivion_photo_layers_tf_skew", "Наклон" },
	{
		"lng_oblivion_photo_layers_tf_perspective_note",
		"Слой искажён перспективой, поэтому задать его положение и "
		"размер числами нельзя. Нажмите «Сбросить», чтобы убрать "
		"перспективу.",
	},
	{ "lng_oblivion_photo_layers_tf_no_layer", "Выберите слой, чтобы перемещать его и менять размер." },
	{ "lng_oblivion_photo_layers_tf_keep_aspect", "Сохранять пропорции" },
	{ "lng_oblivion_photo_layers_tf_snap", "Привязка к краям и центру" },
	{ "lng_oblivion_photo_layers_tf_actions", "Быстрые действия" },
	{ "lng_oblivion_photo_layers_tf_flip_h", "Отразить по горизонтали" },
	{ "lng_oblivion_photo_layers_tf_flip_v", "Отразить по вертикали" },
	{ "lng_oblivion_photo_layers_tf_rotate_left", "Повернуть влево" },
	{ "lng_oblivion_photo_layers_tf_rotate_right", "Повернуть вправо" },
	{ "lng_oblivion_photo_layers_tf_fit", "Вписать в холст" },
	{ "lng_oblivion_photo_layers_tf_fill", "Заполнить холст" },
	{ "lng_oblivion_photo_layers_tf_center", "По центру" },
	{ "lng_oblivion_photo_layers_mask_mode_hide", "Скрывать" },
	{ "lng_oblivion_photo_layers_mask_mode_show", "Возвращать" },
	{
		"lng_oblivion_photo_layers_mask_hint",
		"Проведите по фото, чтобы скрыть часть слоя. Переключитесь на "
		"«Возвращать», чтобы вернуть скрытое.",
	},
	{ "lng_oblivion_photo_layers_mask_none", "У этого слоя пока нет маски. Она появится с первым мазком." },
	{
		"lng_oblivion_photo_layers_mask_reveal_none",
		"На этом слое пока ничего не скрыто. Переключитесь на "
		"«Скрывать», чтобы рисовать маску.",
	},
	{ "lng_oblivion_photo_layers_mask_no_layer", "Выберите слой, чтобы рисовать его маску." },
	{ "lng_oblivion_photo_layers_mask_size", "Размер кисти" },
	{ "lng_oblivion_photo_layers_mask_hardness", "Жёсткость" },
	{ "lng_oblivion_photo_layers_mask_strength", "Сила" },
	{ "lng_oblivion_photo_layers_mask_overlay", "Подсвечивать скрытое" },
	{ "lng_oblivion_photo_layers_mask_enabled", "Маска включена" },
	{ "lng_oblivion_photo_layers_mask_actions", "Маска" },
	{ "lng_oblivion_photo_layers_mask_show_all", "Показать всё" },
	{ "lng_oblivion_photo_layers_mask_hide_all", "Скрыть всё" },
	// Oblivion round 4: photo adjust.
	{ "lng_oblivion_photo_adj_light", "Экспозиция и тон" },
	{ "lng_oblivion_photo_adj_curve", "Кривые" },
	{ "lng_oblivion_photo_adj_color", "Цвет и баланс белого" },
	{ "lng_oblivion_photo_adj_hsl", "Цвета по диапазонам (HSL)" },
	{ "lng_oblivion_photo_adj_grading", "Цветокоррекция по тонам" },
	{ "lng_oblivion_photo_adj_bw", "Чёрно-белый микс" },
	{ "lng_oblivion_photo_adj_presence", "Текстура, чёткость, дымка" },
	{ "lng_oblivion_photo_adj_sharpen", "Резкость" },
	{ "lng_oblivion_photo_adj_denoise", "Шумоподавление" },
	{ "lng_oblivion_photo_adj_vignette", "Виньетка" },
	{ "lng_oblivion_photo_adj_grain", "Зерно" },
	{ "lng_oblivion_photo_adj_exposure", "Экспозиция" },
	{ "lng_oblivion_photo_adj_contrast", "Контраст" },
	{ "lng_oblivion_photo_adj_highlights", "Света" },
	{ "lng_oblivion_photo_adj_shadows", "Тени" },
	{ "lng_oblivion_photo_adj_whites", "Белые" },
	{ "lng_oblivion_photo_adj_blacks", "Чёрные" },
	{ "lng_oblivion_photo_adj_temperature", "Температура" },
	{ "lng_oblivion_photo_adj_tint", "Оттенок" },
	{ "lng_oblivion_photo_adj_vibrance", "Красочность" },
	{ "lng_oblivion_photo_adj_saturation", "Насыщенность" },
	{ "lng_oblivion_photo_adj_hue", "Цветовой тон" },
	{ "lng_oblivion_photo_adj_luminance", "Яркость" },
	{ "lng_oblivion_photo_adj_texture", "Текстура" },
	{ "lng_oblivion_photo_adj_clarity", "Чёткость" },
	{ "lng_oblivion_photo_adj_dehaze", "Удаление дымки" },
	{ "lng_oblivion_photo_adj_amount", "Сила" },
	{ "lng_oblivion_photo_adj_vignette_amount", "Яркость краёв" },
	{ "lng_oblivion_photo_adj_radius", "Радиус" },
	{ "lng_oblivion_photo_adj_masking", "Маскирование" },
	{ "lng_oblivion_photo_adj_noise_luminance", "Яркостный шум" },
	{ "lng_oblivion_photo_adj_noise_color", "Цветовой шум" },
	{ "lng_oblivion_photo_adj_midpoint", "Средняя точка" },
	{ "lng_oblivion_photo_adj_roundness", "Округлость" },
	{ "lng_oblivion_photo_adj_feather", "Растушёвка" },
	{ "lng_oblivion_photo_adj_size", "Размер" },
	{ "lng_oblivion_photo_adj_roughness", "Неровность" },
	{ "lng_oblivion_photo_adj_midtones", "Средние тона" },
	{ "lng_oblivion_photo_adj_shadows_lum", "Яркость теней" },
	{ "lng_oblivion_photo_adj_midtones_lum", "Яркость средних тонов" },
	{ "lng_oblivion_photo_adj_highlights_lum", "Яркость светов" },
	{ "lng_oblivion_photo_adj_blending", "Смешение" },
	{ "lng_oblivion_photo_adj_balance", "Баланс" },
	{ "lng_oblivion_photo_adj_red", "Красный" },
	{ "lng_oblivion_photo_adj_orange", "Оранжевый" },
	{ "lng_oblivion_photo_adj_yellow", "Жёлтый" },
	{ "lng_oblivion_photo_adj_green", "Зелёный" },
	{ "lng_oblivion_photo_adj_aqua", "Голубой" },
	{ "lng_oblivion_photo_adj_blue", "Синий" },
	{ "lng_oblivion_photo_adj_purple", "Фиолетовый" },
	{ "lng_oblivion_photo_adj_magenta", "Пурпурный" },
	{ "lng_oblivion_photo_adj_curve_all", "Общая кривая" },
	{ "lng_oblivion_photo_adj_reset", "Сбросить" },
	{ "lng_oblivion_photo_adj_curve_point", "Вход {input} → выход {output}" },
	{
		"lng_oblivion_photo_adj_curve_hint",
		"Нажмите на кривую, чтобы добавить точку, и перетащите её. "
		"Двойной щелчок по точке удаляет её.",
	},
	{ "lng_oblivion_photo_adj_wheel_value", "{hue}° · {saturation}%" },
	{ "lng_oblivion_photo_adj_wheel_none", "Без оттенка" },
	{
		"lng_oblivion_photo_adj_wheel_hint",
		"Перетащите точку в круге, чтобы выбрать оттенок. "
		"Двойной щелчок сбрасывает его.",
	},
	// Oblivion round 4: photo blur and distort.
	{ "lng_oblivion_photo_blur_gaussian", "Размытие по Гауссу" },
	{ "lng_oblivion_photo_blur_box", "Размытие по рамке" },
	{ "lng_oblivion_photo_blur_motion", "Размытие в движении" },
	{ "lng_oblivion_photo_blur_spin", "Размытие вращением" },
	{ "lng_oblivion_photo_blur_zoom", "Размытие приближением" },
	{ "lng_oblivion_photo_blur_tilt", "Тилт-шифт" },
	{ "lng_oblivion_photo_blur_lens", "Размытие объектива (боке)" },
	{ "lng_oblivion_photo_blur_surface", "Размытие по поверхности" },
	{ "lng_oblivion_photo_blur_radius", "Радиус" },
	{ "lng_oblivion_photo_blur_angle", "Угол" },
	{ "lng_oblivion_photo_blur_distance", "Длина смаза" },
	{ "lng_oblivion_photo_blur_center", "Центр" },
	{ "lng_oblivion_photo_blur_spin_angle", "Угол поворота" },
	{ "lng_oblivion_photo_blur_amount", "Сила" },
	{ "lng_oblivion_photo_blur_tilt_center", "Центр резкой полосы" },
	{ "lng_oblivion_photo_blur_tilt_width", "Ширина резкой полосы" },
	{ "lng_oblivion_photo_blur_tilt_falloff", "Плавность перехода" },
	{ "lng_oblivion_photo_blur_lens_shape", "Форма бликов" },
	{ "lng_oblivion_photo_blur_lens_disc", "Круг" },
	{ "lng_oblivion_photo_blur_lens_pentagon", "Пятиугольник" },
	{ "lng_oblivion_photo_blur_lens_hexagon", "Шестиугольник" },
	{ "lng_oblivion_photo_blur_lens_octagon", "Восьмиугольник" },
	{ "lng_oblivion_photo_blur_lens_rotation", "Поворот формы" },
	{ "lng_oblivion_photo_blur_lens_highlights", "Яркость бликов" },
	{ "lng_oblivion_photo_blur_lens_threshold", "Порог бликов" },
	{ "lng_oblivion_photo_blur_surface_threshold", "Порог" },
	{ "lng_oblivion_photo_blur_region", "Область" },
	{ "lng_oblivion_photo_blur_region_whole", "Весь слой" },
	{ "lng_oblivion_photo_blur_region_linear", "Линейный градиент" },
	{ "lng_oblivion_photo_blur_region_radial", "Круговой градиент" },
	{ "lng_oblivion_photo_blur_region_center", "Центр области" },
	{ "lng_oblivion_photo_blur_region_angle", "Направление" },
	{ "lng_oblivion_photo_blur_region_length", "Длина перехода" },
	{ "lng_oblivion_photo_blur_region_radius", "Радиус резкой зоны" },
	{ "lng_oblivion_photo_blur_region_feather", "Растушёвка" },
	{ "lng_oblivion_photo_blur_region_invert", "Обратить область" },
	{ "lng_oblivion_photo_distort_keystone", "Наклон перспективы" },
	{ "lng_oblivion_photo_distort_corners", "Перспектива по углам" },
	{ "lng_oblivion_photo_distort_skew", "Скос" },
	{ "lng_oblivion_photo_distort_lens", "Дисторсия объектива" },
	{ "lng_oblivion_photo_distort_bulge", "Выпуклость / вогнутость" },
	{ "lng_oblivion_photo_distort_twirl", "Скручивание" },
	{ "lng_oblivion_photo_distort_wave", "Волна" },
	{ "lng_oblivion_photo_distort_ripple", "Рябь" },
	{ "lng_oblivion_photo_distort_mesh", "Деформация сеткой" },
	{ "lng_oblivion_photo_distort_vertical", "По вертикали" },
	{ "lng_oblivion_photo_distort_horizontal", "По горизонтали" },
	{ "lng_oblivion_photo_distort_zoom", "Масштаб" },
	{ "lng_oblivion_photo_distort_lens_amount", "Бочка / подушка" },
	{ "lng_oblivion_photo_distort_strength", "Сила" },
	{ "lng_oblivion_photo_distort_radius", "Радиус" },
	{ "lng_oblivion_photo_distort_center", "Центр" },
	{ "lng_oblivion_photo_distort_angle", "Угол" },
	{ "lng_oblivion_photo_distort_amplitude", "Амплитуда" },
	{ "lng_oblivion_photo_distort_wavelength", "Длина волны" },
	{ "lng_oblivion_photo_distort_direction", "Направление" },
	{ "lng_oblivion_photo_distort_phase", "Сдвиг волны" },
	{ "lng_oblivion_photo_distort_wave_shape", "Форма волны" },
	{ "lng_oblivion_photo_distort_wave_sine", "Плавная" },
	{ "lng_oblivion_photo_distort_wave_triangle", "Зигзаг" },
	{ "lng_oblivion_photo_distort_decay", "Затухание" },
	{ "lng_oblivion_photo_distort_edges", "Края" },
	{ "lng_oblivion_photo_distort_edges_clear", "Прозрачные" },
	{ "lng_oblivion_photo_distort_edges_stretch", "Растянутые" },
	{ "lng_oblivion_photo_distort_edges_mirror", "Зеркальные" },
	{ "lng_oblivion_photo_distort_mesh_grid", "Сетка" },
	{ "lng_oblivion_photo_distort_edit", "Изменить на фото" },
	{ "lng_oblivion_photo_distort_edit_done", "Готово" },
	{ "lng_oblivion_photo_distort_reset", "Сбросить" },
	{
		"lng_oblivion_photo_distort_mesh_hint",
		"Перетаскивайте узлы сетки на фото. "
			"Когда закончите, нажмите «Готово» или Esc.",
	},
	{
		"lng_oblivion_photo_distort_corners_hint",
		"Перетаскивайте углы на фото. "
			"Когда закончите, нажмите «Готово» или Esc.",
	},
	{ "lng_oblivion_photo_distort_locked", "Этот слой заблокирован." },
	{
		"lng_oblivion_photo_distort_switched_off",
		"Включите эффект, чтобы изменить его на фото.",
	},
	// Oblivion round 4: photo lofi and glitch.
	{ "lng_oblivion_photo_lofi_camera", "Старая цифровая камера" },
	{ "lng_oblivion_photo_lofi_sensor", "Разрешение сенсора" },
	{ "lng_oblivion_photo_lofi_jpeg", "Сжатие JPEG" },
	{ "lng_oblivion_photo_lofi_noise", "Шум матрицы" },
	{ "lng_oblivion_photo_lofi_aberration", "Хроматические аберрации" },
	{ "lng_oblivion_photo_lofi_bloom", "Свечение и ореолы" },
	{ "lng_oblivion_photo_lofi_flash", "Вспышка" },
	{ "lng_oblivion_photo_lofi_cast", "Сбитый баланс белого" },
	{ "lng_oblivion_photo_lofi_sharpen", "Избыточная резкость" },
	{ "lng_oblivion_photo_lofi_stamp", "Дата на фото" },
	{ "lng_oblivion_photo_lofi_depth", "Глубина цвета" },
	{ "lng_oblivion_photo_lofi_scanlines", "Строки развёртки" },
	{ "lng_oblivion_photo_lofi_vhs", "Кассета VHS" },
	{ "lng_oblivion_photo_lofi_pixelate", "Пикселизация" },
	{ "lng_oblivion_photo_lofi_posterize", "Постеризация" },
	{ "lng_oblivion_photo_lofi_halftone", "Полутоновая печать" },
	{ "lng_oblivion_photo_lofi_amount", "Сила" },
	{ "lng_oblivion_photo_lofi_size", "Размер" },
	{ "lng_oblivion_photo_lofi_radius", "Радиус" },
	{ "lng_oblivion_photo_lofi_threshold", "Порог" },
	{ "lng_oblivion_photo_lofi_center", "Центр" },
	{ "lng_oblivion_photo_lofi_angle", "Угол" },
	{ "lng_oblivion_photo_lofi_resolution", "Разрешение" },
	{ "lng_oblivion_photo_lofi_downscale", "Уменьшение" },
	{ "lng_oblivion_photo_lofi_downscale_smooth", "Плавное" },
	{ "lng_oblivion_photo_lofi_downscale_rough", "Грубое" },
	{ "lng_oblivion_photo_lofi_upscale", "Увеличение" },
	{ "lng_oblivion_photo_lofi_upscale_smooth", "Плавное" },
	{ "lng_oblivion_photo_lofi_upscale_pixels", "Пиксели" },
	{ "lng_oblivion_photo_lofi_upscale_sharp", "Резкое" },
	{ "lng_oblivion_photo_lofi_quality", "Качество" },
	{ "lng_oblivion_photo_lofi_generations", "Сохранений" },
	{ "lng_oblivion_photo_lofi_block", "Размер блоков" },
	{ "lng_oblivion_photo_lofi_noise_luma", "Шум" },
	{ "lng_oblivion_photo_lofi_noise_color", "Цветной шум" },
	{ "lng_oblivion_photo_lofi_noise_grain", "Размер зерна" },
	{ "lng_oblivion_photo_lofi_noise_hot", "Горячие пиксели" },
	{ "lng_oblivion_photo_lofi_noise_banding", "Полосы" },
	{ "lng_oblivion_photo_lofi_aberrations", "Аберрации" },
	{ "lng_oblivion_photo_lofi_fringe", "Фиолетовая кайма" },
	{ "lng_oblivion_photo_lofi_glow", "Свечение" },
	{ "lng_oblivion_photo_lofi_halation", "Красный ореол" },
	{ "lng_oblivion_photo_lofi_falloff", "Тёмные углы" },
	{ "lng_oblivion_photo_lofi_temperature", "Температура" },
	{ "lng_oblivion_photo_lofi_tint", "Оттенок" },
	{ "lng_oblivion_photo_lofi_saturation", "Насыщенность" },
	{ "lng_oblivion_photo_lofi_fade", "Выцветание" },
	{ "lng_oblivion_photo_lofi_levels", "Уровней на канал" },
	{ "lng_oblivion_photo_lofi_mono", "Чёрно-белое" },
	{ "lng_oblivion_photo_lofi_dither", "Дизеринг" },
	{ "lng_oblivion_photo_lofi_dither_none", "Нет" },
	{ "lng_oblivion_photo_lofi_dither_ordered", "Упорядоченный" },
	{ "lng_oblivion_photo_lofi_dither_bayer2", "Упорядоченный 2×2" },
	{ "lng_oblivion_photo_lofi_dither_bayer4", "Упорядоченный 4×4" },
	{ "lng_oblivion_photo_lofi_dither_bayer8", "Упорядоченный 8×8" },
	{ "lng_oblivion_photo_lofi_dither_fs", "Флойд — Стейнберг" },
	{ "lng_oblivion_photo_lofi_dither_atkinson", "Аткинсон" },
	{ "lng_oblivion_photo_lofi_dot", "Размер точки" },
	{ "lng_oblivion_photo_lofi_depth_full", "Без изменений" },
	{ "lng_oblivion_photo_lofi_depth_16", "16 бит" },
	{ "lng_oblivion_photo_lofi_depth_12", "12 бит" },
	{ "lng_oblivion_photo_lofi_depth_256", "256 цветов" },
	{ "lng_oblivion_photo_lofi_depth_64", "64 цвета" },
	{ "lng_oblivion_photo_lofi_depth_8", "8 цветов" },
	{ "lng_oblivion_photo_lofi_depth_gray", "4 оттенка серого" },
	{ "lng_oblivion_photo_lofi_depth_bw", "Чёрно-белое, 1 бит" },
	{ "lng_oblivion_photo_lofi_pitch", "Шаг строк" },
	{ "lng_oblivion_photo_lofi_comb", "Чересстрочный сдвиг" },
	{ "lng_oblivion_photo_lofi_vhs_bleed", "Растекание цвета" },
	{ "lng_oblivion_photo_lofi_vhs_soft", "Мягкость" },
	{ "lng_oblivion_photo_lofi_vhs_tracking", "Полосы трекинга" },
	{ "lng_oblivion_photo_lofi_vhs_noise", "Шум" },
	{ "lng_oblivion_photo_lofi_vhs_wobble", "Дрожание" },
	{ "lng_oblivion_photo_lofi_shape", "Форма" },
	{ "lng_oblivion_photo_lofi_shape_squares", "Квадраты" },
	{ "lng_oblivion_photo_lofi_shape_dots", "Точки" },
	{ "lng_oblivion_photo_lofi_shape_lcd", "ЖК-экран" },
	{ "lng_oblivion_photo_lofi_mode", "Режим" },
	{ "lng_oblivion_photo_lofi_mode_ink", "Одна краска" },
	{ "lng_oblivion_photo_lofi_mode_color", "Цветной" },
	{ "lng_oblivion_photo_lofi_ink", "Краска" },
	{ "lng_oblivion_photo_lofi_paper", "Бумага" },
	{ "lng_oblivion_photo_lofi_stamp_text", "Текст" },
	{ "lng_oblivion_photo_lofi_stamp_font", "Шрифт" },
	{ "lng_oblivion_photo_lofi_stamp_font_segments", "Цифры камеры" },
	{ "lng_oblivion_photo_lofi_stamp_font_pixel", "Пиксели" },
	{ "lng_oblivion_photo_lofi_stamp_font_bold", "Жирные пиксели" },
	{ "lng_oblivion_photo_lofi_stamp_font_camcorder", "Видеокамера" },
	{ "lng_oblivion_photo_lofi_stamp_color", "Цвет" },
	{ "lng_oblivion_photo_lofi_stamp_corner", "Расположение" },
	{ "lng_oblivion_photo_lofi_stamp_corner_br", "Справа внизу" },
	{ "lng_oblivion_photo_lofi_stamp_corner_bl", "Слева внизу" },
	{ "lng_oblivion_photo_lofi_stamp_corner_tr", "Справа вверху" },
	{ "lng_oblivion_photo_lofi_stamp_corner_tl", "Слева вверху" },
	{ "lng_oblivion_photo_lofi_stamp_glow", "Свечение" },
	{ "lng_oblivion_photo_lofi_stamp_margin", "Отступ" },
	{ "lng_oblivion_photo_lofi_stamp_today", "Сегодня" },
	{ "lng_oblivion_photo_lofi_stamp_now", "Сейчас" },
	{ "lng_oblivion_photo_lofi_stamp_classic", "Плёнка" },
	{ "lng_oblivion_photo_lofi_stamp_hint", "Дата или текст" },
	{ "lng_oblivion_photo_lofi_sec_sensor", "Сенсор" },
	{ "lng_oblivion_photo_lofi_sec_optics", "Оптика" },
	{ "lng_oblivion_photo_lofi_sec_light", "Свет и цвет" },
	{ "lng_oblivion_photo_lofi_sec_noise", "Шум матрицы" },
	{ "lng_oblivion_photo_lofi_sec_processing", "Обработка" },
	{ "lng_oblivion_photo_lofi_sec_jpeg", "JPEG" },
	{ "lng_oblivion_photo_lofi_sec_stamp", "Дата" },
	{ "lng_oblivion_photo_lofi_preset_phone", "Старый телефон 2005" },
	{ "lng_oblivion_photo_lofi_preset_ccd", "CCD-мыльница 2008" },
	{ "lng_oblivion_photo_lofi_preset_webcam", "Веб-камера" },
	{ "lng_oblivion_photo_lofi_preset_mms", "MMS 176×144" },
	{ "lng_oblivion_photo_lofi_preset_flash", "Вспышка в упор 2003" },
	{ "lng_oblivion_photo_lofi_preset_pocket", "Карманная пиксель-камера" },
	{ "lng_oblivion_photo_lofi_preset_cctv", "Камера наблюдения" },
	{ "lng_oblivion_photo_lofi_preset_vhs", "Домашняя видеокассета" },
	{ "lng_oblivion_photo_glitch_rgb", "Сдвиг каналов RGB" },
	{ "lng_oblivion_photo_glitch_slices", "Сдвиг полос" },
	{ "lng_oblivion_photo_glitch_blocks", "Битые блоки" },
	{ "lng_oblivion_photo_glitch_sort", "Сортировка пикселей" },
	{ "lng_oblivion_photo_glitch_smear", "Размазывание" },
	{ "lng_oblivion_photo_glitch_mosh", "Датамош" },
	{ "lng_oblivion_photo_glitch_jitter", "Дрожание строк" },
	{ "lng_oblivion_photo_glitch_wave", "Волна" },
	{ "lng_oblivion_photo_glitch_bands", "Полосы помех" },
	{ "lng_oblivion_photo_glitch_crush", "Потеря битов" },
	{ "lng_oblivion_photo_glitch_bend", "Битый JPEG" },
	{ "lng_oblivion_photo_glitch_amount", "Сила" },
	{ "lng_oblivion_photo_glitch_count", "Количество" },
	{ "lng_oblivion_photo_glitch_size", "Размер" },
	{ "lng_oblivion_photo_glitch_length", "Длина" },
	{ "lng_oblivion_photo_glitch_amplitude", "Амплитуда" },
	{ "lng_oblivion_photo_glitch_direction", "Направление" },
	{ "lng_oblivion_photo_glitch_dir_right", "Вправо" },
	{ "lng_oblivion_photo_glitch_dir_left", "Влево" },
	{ "lng_oblivion_photo_glitch_dir_down", "Вниз" },
	{ "lng_oblivion_photo_glitch_dir_up", "Вверх" },
	{ "lng_oblivion_photo_glitch_axis_horizontal", "По горизонтали" },
	{ "lng_oblivion_photo_glitch_axis_vertical", "По вертикали" },
	{ "lng_oblivion_photo_glitch_red", "Красный" },
	{ "lng_oblivion_photo_glitch_green", "Зелёный" },
	{ "lng_oblivion_photo_glitch_blue", "Синий" },
	{ "lng_oblivion_photo_glitch_shift_x", "По горизонтали" },
	{ "lng_oblivion_photo_glitch_shift_y", "По вертикали" },
	{ "lng_oblivion_photo_glitch_chaos", "Случайность" },
	{ "lng_oblivion_photo_glitch_wrap", "Перенос через край" },
	{ "lng_oblivion_photo_glitch_split", "Расслоение цвета" },
	{ "lng_oblivion_photo_glitch_mode", "Режим" },
	{ "lng_oblivion_photo_glitch_mode_mix", "Смесь" },
	{ "lng_oblivion_photo_glitch_mode_move", "Сдвиг" },
	{ "lng_oblivion_photo_glitch_mode_channels", "Каналы" },
	{ "lng_oblivion_photo_glitch_mode_invert", "Инверсия" },
	{ "lng_oblivion_photo_glitch_mode_noise", "Шум" },
	{ "lng_oblivion_photo_glitch_mode_stretch", "Растяжение" },
	{ "lng_oblivion_photo_glitch_grid", "Сетка" },
	{ "lng_oblivion_photo_glitch_low", "Нижний порог" },
	{ "lng_oblivion_photo_glitch_high", "Верхний порог" },
	{ "lng_oblivion_photo_glitch_key", "Сортировать по" },
	{ "lng_oblivion_photo_glitch_key_luma", "Яркости" },
	{ "lng_oblivion_photo_glitch_key_hue", "Оттенку" },
	{ "lng_oblivion_photo_glitch_key_saturation", "Насыщенности" },
	{ "lng_oblivion_photo_glitch_fade", "Затухание" },
	{ "lng_oblivion_photo_glitch_block", "Размер блока" },
	{ "lng_oblivion_photo_glitch_distance", "Дальность" },
	{ "lng_oblivion_photo_glitch_angle", "Угол" },
	{ "lng_oblivion_photo_glitch_passes", "Проходы" },
	{ "lng_oblivion_photo_glitch_density", "Плотность" },
	{ "lng_oblivion_photo_glitch_line_height", "Высота строк" },
	{ "lng_oblivion_photo_glitch_wavelength", "Длина волны" },
	{ "lng_oblivion_photo_glitch_phase", "Фаза" },
	{ "lng_oblivion_photo_glitch_shape", "Форма" },
	{ "lng_oblivion_photo_glitch_shape_sine", "Синус" },
	{ "lng_oblivion_photo_glitch_shape_triangle", "Треугольник" },
	{ "lng_oblivion_photo_glitch_shape_steps", "Ступени" },
	{ "lng_oblivion_photo_glitch_shape_noise", "Шум" },
	{ "lng_oblivion_photo_glitch_kind", "Вид" },
	{ "lng_oblivion_photo_glitch_kind_static", "Снег" },
	{ "lng_oblivion_photo_glitch_kind_color", "Цветной снег" },
	{ "lng_oblivion_photo_glitch_kind_dropouts", "Выпадения" },
	{ "lng_oblivion_photo_glitch_band_height", "Высота полос" },
	{ "lng_oblivion_photo_glitch_grain", "Зерно" },
	{ "lng_oblivion_photo_glitch_shift", "Сдвиг" },
	{ "lng_oblivion_photo_glitch_bits", "Биты" },
	{ "lng_oblivion_photo_glitch_crush_cut", "Отсечение" },
	{ "lng_oblivion_photo_glitch_crush_rotate", "Вращение" },
	{ "lng_oblivion_photo_glitch_crush_xor", "XOR" },
	{ "lng_oblivion_photo_glitch_in_bands", "Полосами" },
	{ "lng_oblivion_photo_glitch_breaks", "Повреждения" },
	{ "lng_oblivion_photo_glitch_drift", "Искажение цвета" },
	{ "lng_oblivion_photo_glitch_garbage", "Мусорные блоки" },
	{ "lng_oblivion_photo_glitch_preset_signal", "Сигнал потерян" },
	{ "lng_oblivion_photo_glitch_preset_file", "Битый файл" },
	{ "lng_oblivion_photo_glitch_preset_mosh", "Плывущая картинка" },
	{ "lng_oblivion_photo_glitch_preset_vapor", "Вейпорвейв" },
	{ "lng_oblivion_photo_digicam_jpeg", "Digicam JPEG" },
	{ "lng_oblivion_photo_digicam_ccd", "Digicam CCD" },
	{ "lng_oblivion_photo_digicam_nokia", "Телефон Nokia" },
	{ "lng_oblivion_photo_digicam_webcam", "Веб-камера 1/4″" },
	{ "lng_oblivion_photo_digicam_iphone", "iPhone 3GS" },
	{ "lng_oblivion_photo_digicam_soap", "Промывка мозгов" },
	{ "lng_oblivion_photo_digicam_look", "Фильтры 2016" },
	{ "lng_oblivion_photo_digicam_glow", "Цветное свечение" },
	{ "lng_oblivion_photo_digicam_reflection", "Блик объектива" },
	{ "lng_oblivion_photo_digicam_vignette", "Цветная виньетка" },
	{ "lng_oblivion_photo_digicam_filter", "Фильтр" },
	{ "lng_oblivion_photo_digicam_intensity", "Сила фильтра" },
	{ "lng_oblivion_photo_digicam_vignette_amount", "Виньетка" },
	{ "lng_oblivion_photo_digicam_grain", "Зерно" },
	{ "lng_oblivion_photo_digicam_soften", "Мягкий объектив" },
	{ "lng_oblivion_photo_digicam_tint", "Цвет свечения" },
	{ "lng_oblivion_photo_digicam_highlights", "Только света" },
	{ "lng_oblivion_photo_digicam_white", "Точка белого" },
	{ "lng_oblivion_photo_digicam_reflection_variant", "Форма блика" },
	{ "lng_oblivion_photo_digicam_noise_mono", "Одноцветный шум" },
	{ "lng_oblivion_photo_digicam_noise_blend", "Наложение шума" },
	{ "lng_oblivion_photo_digicam_blend_add", "Сложение" },
	{ "lng_oblivion_photo_digicam_blend_overlay", "Перекрытие" },
	{ "lng_oblivion_photo_digicam_noise_shadows", "Сильнее в тенях" },
	{ "lng_oblivion_photo_digicam_pixels", "Пиксели ×2" },
	{ "lng_oblivion_photo_digicam_edges", "Зелёные углы" },
	{ "lng_oblivion_photo_digicam_denoise", "Шумоподавление" },
	{ "lng_oblivion_photo_digicam_highlight_bloom", "Свечение светов" },
	{ "lng_oblivion_photo_digicam_smooth", "Сглаживание" },
	{ "lng_oblivion_photo_digicam_inner", "Цвет центра" },
	{ "lng_oblivion_photo_digicam_inner_amount", "Оттенок центра" },
	{ "lng_oblivion_photo_digicam_outer", "Цвет краёв" },
	{ "lng_oblivion_photo_digicam_outer_amount", "Оттенок краёв" },
	{ "lng_oblivion_photo_digicam_softness", "Мягкость" },
	{ "lng_oblivion_photo_digicam_look_nashville", "Nashville" },
	{ "lng_oblivion_photo_digicam_look_chief_keef", "Chief Keef" },
	{ "lng_oblivion_photo_digicam_look_nuke", "Nuke" },
	{ "lng_oblivion_photo_digicam_look_phreshboy", "Phreshboy" },
	{ "lng_oblivion_photo_digicam_look_sepia", "Сепия" },
	{ "lng_oblivion_photo_digicam_look_2014", "2014" },
	{ "lng_oblivion_photo_digicam_look_money", "$$$" },
	{ "lng_oblivion_photo_digicam_look_pandora", "Pandora" },
	{ "lng_oblivion_photo_digicam_look_bleach", "Пропуск отбеливания" },
	{ "lng_oblivion_photo_digicam_look_stressor", "Stressor" },
	{ "lng_oblivion_photo_digicam_look_bw", "Чёрно-белый" },
	{ "lng_oblivion_photo_digicam_look_bw_soft", "Мягкий чёрно-белый" },
	{ "lng_oblivion_photo_digicam_look_desat", "Обесцвечивание" },
	{ "lng_oblivion_photo_digicam_look_hsv", "Обесцвечивание (HSV)" },
	{ "lng_oblivion_photo_digicam_look_ccd", "CCD" },
	{ "lng_oblivion_photo_digicam_look_nokia", "Nokia" },
	{ "lng_oblivion_photo_digicam_look_calvin", "Calvin" },
	{ "lng_oblivion_photo_digicam_preset_jpeg_low", "JPEG: низкое качество" },
	{
		"lng_oblivion_photo_digicam_preset_jpeg_medium",
		"JPEG: среднее качество",
	},
	{
		"lng_oblivion_photo_digicam_preset_jpeg_high",
		"JPEG: высокое качество",
	},
	// Oblivion round 4: photo draw.
	{ "lng_oblivion_photo_draw_pen", "Перо" },
	{ "lng_oblivion_photo_draw_marker", "Маркер" },
	{ "lng_oblivion_photo_draw_pencil", "Карандаш" },
	{ "lng_oblivion_photo_draw_eraser", "Ластик" },
	{ "lng_oblivion_photo_draw_line", "Линия" },
	{ "lng_oblivion_photo_draw_arrow", "Стрелка" },
	{ "lng_oblivion_photo_draw_rect", "Прямоугольник" },
	{ "lng_oblivion_photo_draw_ellipse", "Эллипс" },
	{ "lng_oblivion_photo_draw_text", "Текст" },
	{ "lng_oblivion_photo_draw_layer", "Рисунок" },
	{ "lng_oblivion_photo_draw_color", "Цвет" },
	{ "lng_oblivion_photo_draw_recent", "Недавние цвета" },
	{ "lng_oblivion_photo_draw_size", "Размер" },
	{ "lng_oblivion_photo_draw_opacity", "Непрозрачность" },
	{ "lng_oblivion_photo_draw_strength", "Сила" },
	{ "lng_oblivion_photo_draw_smoothing", "Сглаживание" },
	{ "lng_oblivion_photo_draw_dynamic", "Тоньше при быстром движении" },
	{ "lng_oblivion_photo_draw_fill", "Заливка" },
	{
		"lng_oblivion_photo_draw_hint_freehand",
		"Рисуйте прямо на фото. Штрихи ложатся на выбранный слой с рисунком; если выбран другой слой, для них создаётся новый. Клавиши [ и ] меняют размер.",
	},
	{
		"lng_oblivion_photo_draw_hint_eraser",
		"Ластик стирает только нарисованное на выбранном слое с рисунком, фото и текст он не трогает. Клавиши [ и ] меняют размер.",
	},
	{
		"lng_oblivion_photo_draw_hint_line",
		"Удерживайте Shift, чтобы линия шла строго горизонтально, вертикально или под углом 45°.",
	},
	{
		"lng_oblivion_photo_draw_hint_rect",
		"Удерживайте Shift, чтобы нарисовать квадрат, Alt — чтобы рисовать от центра.",
	},
	{
		"lng_oblivion_photo_draw_hint_ellipse",
		"Удерживайте Shift, чтобы нарисовать круг, Alt — чтобы рисовать от центра.",
	},
	{
		"lng_oblivion_photo_draw_hint_text",
		"Нажмите на фото, чтобы добавить текст. Готовую надпись можно перетащить, а чтобы изменить её — нажмите на неё.",
	},
	{
		"lng_oblivion_photo_draw_locked",
		"Этот слой заблокирован. Разблокируйте его или выберите другой слой.",
	},
	{
		"lng_oblivion_photo_draw_eraser_no_layer",
		"Сначала выберите слой с рисунком: ластик стирает только на нём.",
	},
	{
		"lng_oblivion_photo_draw_too_many",
		"На этом слое слишком много штрихов. Чтобы продолжить, добавьте новый слой с рисунком.",
	},
	{ "lng_oblivion_photo_draw_text_field", "Введите текст" },
	{ "lng_oblivion_photo_draw_text_font", "Шрифт" },
	{ "lng_oblivion_photo_draw_text_font_default", "Обычный" },
	{ "lng_oblivion_photo_draw_text_font_serif", "С засечками" },
	{ "lng_oblivion_photo_draw_text_font_mono", "Моноширинный" },
	{ "lng_oblivion_photo_draw_text_weight", "Начертание" },
	{ "lng_oblivion_photo_draw_text_weight_regular", "Обычное" },
	{ "lng_oblivion_photo_draw_text_weight_bold", "Жирное" },
	{ "lng_oblivion_photo_draw_text_align", "Выравнивание" },
	{ "lng_oblivion_photo_draw_text_align_left", "По левому краю" },
	{ "lng_oblivion_photo_draw_text_align_center", "По центру" },
	{ "lng_oblivion_photo_draw_text_align_right", "По правому краю" },
	{ "lng_oblivion_photo_draw_text_plate", "Фон под текстом" },
	{ "lng_oblivion_photo_draw_text_plate_color", "Цвет фона" },
	{ "lng_oblivion_photo_draw_text_plate_opacity", "Непрозрачность фона" },
	// Oblivion round 4: photo collage.
	{ "lng_oblivion_photo_collage_kind", "Коллаж" },
	{
		"lng_oblivion_photo_collage_create_about",
		"Коллаж собирает несколько фото в сетку. Добавьте фото — те, "
		"что уже открыты в редакторе, тоже попадут в коллаж.",
	},
	{ "lng_oblivion_photo_collage_add", "Добавить фото…" },
	{ "lng_oblivion_photo_collage_from_layers", "Собрать из слоёв" },
	{ "lng_oblivion_photo_collage_paste", "Вставить из буфера обмена" },
	{ "lng_oblivion_photo_collage_paste_empty", "В буфере обмена нет изображения." },
	{ "lng_oblivion_photo_collage_building", "Собираем коллаж…" },
	{ "lng_oblivion_photo_collage_locked", "Слой с коллажем заблокирован." },
	{ "lng_oblivion_photo_collage_templates#one", "Шаблоны для {count} фото" },
	{ "lng_oblivion_photo_collage_templates#few", "Шаблоны для {count} фото" },
	{ "lng_oblivion_photo_collage_templates#many", "Шаблоны для {count} фото" },
	{ "lng_oblivion_photo_collage_templates#other", "Шаблоны для {count} фото" },
	{ "lng_oblivion_photo_collage_grid", "Своя сетка" },
	{ "lng_oblivion_photo_collage_rows", "Строки" },
	{ "lng_oblivion_photo_collage_columns", "Столбцы" },
	{ "lng_oblivion_photo_collage_look", "Оформление" },
	{ "lng_oblivion_photo_collage_spacing", "Промежутки" },
	{ "lng_oblivion_photo_collage_margin", "Поля по краям" },
	{ "lng_oblivion_photo_collage_radius", "Скругление углов" },
	{ "lng_oblivion_photo_collage_background", "Фон" },
	{ "lng_oblivion_photo_collage_bg_none", "Прозрачный" },
	{ "lng_oblivion_photo_collage_bg_color", "Цвет" },
	{ "lng_oblivion_photo_collage_bg_gradient", "Градиент" },
	{ "lng_oblivion_photo_collage_bg_blur", "Размытое фото" },
	{ "lng_oblivion_photo_collage_color", "Цвет" },
	{ "lng_oblivion_photo_collage_color2", "Второй цвет" },
	{ "lng_oblivion_photo_collage_angle", "Направление" },
	{ "lng_oblivion_photo_collage_blur_amount", "Размытие" },
	{ "lng_oblivion_photo_collage_blur_dim", "Затемнение" },
	{
		"lng_oblivion_photo_collage_blur_photo",
		"Сменить фото для фона ({index} из {total})",
	},
	{ "lng_oblivion_photo_collage_aspect", "Формат холста" },
	{ "lng_oblivion_photo_collage_cell", "Выбранное фото" },
	{ "lng_oblivion_photo_collage_cell_empty", "Выбранная ячейка" },
	{ "lng_oblivion_photo_collage_cell_hint", "Нажмите на фото в коллаже, чтобы настроить его." },
	{ "lng_oblivion_photo_collage_cell_zoom", "Масштаб" },
	{ "lng_oblivion_photo_collage_cell_replace", "Заменить фото…" },
	{ "lng_oblivion_photo_collage_cell_choose", "Выбрать фото…" },
	{ "lng_oblivion_photo_collage_cell_fit", "Показать фото целиком" },
	{ "lng_oblivion_photo_collage_cell_fill", "Заполнить ячейку" },
	{ "lng_oblivion_photo_collage_cell_rotate", "Повернуть" },
	{ "lng_oblivion_photo_collage_cell_mirror", "Отразить" },
	{ "lng_oblivion_photo_collage_cell_remove", "Убрать фото" },
	{ "lng_oblivion_photo_collage_cell_delete", "Удалить ячейку" },
	{ "lng_oblivion_photo_collage_shuffle", "Перемешать фото" },
	{ "lng_oblivion_photo_collage_to_layers", "Разобрать на слои" },
	{
		"lng_oblivion_photo_collage_to_layers_empty",
		"В коллаже нет ничего, из чего можно сделать слои.",
	},
	{ "lng_oblivion_photo_collage_converting", "Создаём слои…" },
	{ "lng_oblivion_photo_collage_background_layer", "Фон коллажа" },
	{ "lng_oblivion_photo_collage_cell_name", "Фото {index}" },
	{
		"lng_oblivion_photo_collage_hidden#one",
		"{count} фото не поместилось в сетку. Выберите шаблон выше — "
		"в нём поместятся все.",
	},
	{
		"lng_oblivion_photo_collage_hidden#few",
		"{count} фото не поместились в сетку. Выберите шаблон выше — "
		"в нём поместятся все.",
	},
	{
		"lng_oblivion_photo_collage_hidden#many",
		"{count} фото не поместились в сетку. Выберите шаблон выше — "
		"в нём поместятся все.",
	},
	{
		"lng_oblivion_photo_collage_hidden#other",
		"{count} фото не поместились в сетку. Выберите шаблон выше — "
		"в нём поместятся все.",
	},
	{
		"lng_oblivion_photo_collage_hint",
		"Тяните границы, чтобы менять размеры ячеек. Тяните фото, чтобы "
		"сдвинуть его внутри ячейки; колесо мыши над выбранным фото "
		"меняет масштаб. Чтобы поменять два фото местами, перетащите "
		"круглый значок на другую ячейку.",
	},
	{
		"lng_oblivion_photo_collage_layer_about",
		"Фото, сетка и оформление коллажа настраиваются "
		"инструментом «Коллаж».",
	},
	{ "lng_oblivion_photo_collage_layer_open", "Настроить коллаж" },
	{ "lng_oblivion_photo_collage_open_failed", "Не удалось открыть эти фото." },
	{ "lng_oblivion_photo_collage_choose_title", "Выберите фото для коллажа" },
	{ "lng_oblivion_photo_collage_file_name", "коллаж" },
	{ "lng_oblivion_photo_collage_settings", "Коллаж из фото" },
	{
		"lng_oblivion_photo_collage_full",
		"В коллаже уже {max} фото — больше в нём не поместится.",
	},
	{
		"lng_oblivion_photo_collage_limit",
		"В коллаже помещается не больше {max} фото. "
		"Не добавлено: {skipped}.",
	},
	// Oblivion round 4: photo panels.
	{ "lng_oblivion_photo_panel_seed", "Вариант" },
	{ "lng_oblivion_photo_panel_fx_filter", "Фильтр" },
	{ "lng_oblivion_photo_panel_tool_view", "Просмотр: перетаскивайте, чтобы двигать фото" },
	{ "lng_oblivion_photo_panel_tab_layer", "Слой" },
	{ "lng_oblivion_photo_panel_tab_tool", "Инструмент" },
	{ "lng_oblivion_photo_panel_tab_layers", "Все слои" },
	{ "lng_oblivion_photo_panel_layer_photo", "Фото" },
	{ "lng_oblivion_photo_panel_layer_name", "Слой {index}" },
	{ "lng_oblivion_photo_panel_no_image_layer", "Сначала выберите слой с фотографией." },
	{ "lng_oblivion_photo_panel_layer_locked", "Этот слой заблокирован." },
	{ "lng_oblivion_photo_panel_import", "Добавить фото…" },
	{ "lng_oblivion_photo_panel_import_title", "Выберите фото, чтобы добавить их слоями" },
	{ "lng_oblivion_photo_panel_no_layer", "Выберите слой, чтобы увидеть его настройки." },
	{ "lng_oblivion_photo_panel_opacity", "Непрозрачность" },
	{ "lng_oblivion_photo_panel_blend", "Наложение" },
	{ "lng_oblivion_photo_panel_effects", "Эффекты слоя" },
	{ "lng_oblivion_photo_panel_effects_empty", "У этого слоя пока нет эффектов." },
	{ "lng_oblivion_photo_panel_add_effect", "Добавить эффект" },
	{ "lng_oblivion_photo_panel_effect_unknown", "Неизвестный эффект" },
	{ "lng_oblivion_photo_panel_randomize", "Другой случайный вариант" },
	{ "lng_oblivion_photo_panel_point_pick", "Указать на фото" },
	{ "lng_oblivion_photo_panel_point_done", "Готово" },
	{ "lng_oblivion_photo_panel_point_hint", "Нажмите на фото или перетащите точку. Когда закончите, нажмите Esc." },
	{ "lng_oblivion_photo_panel_on", "Вкл." },
	{ "lng_oblivion_photo_panel_off", "Выкл." },
	{ "lng_oblivion_photo_panel_blend_normal", "Обычное" },
	{ "lng_oblivion_photo_panel_blend_multiply", "Умножение" },
	{ "lng_oblivion_photo_panel_blend_screen", "Экран" },
	{ "lng_oblivion_photo_panel_blend_overlay", "Перекрытие" },
	{ "lng_oblivion_photo_panel_blend_soft_light", "Мягкий свет" },
	{ "lng_oblivion_photo_panel_blend_hard_light", "Жёсткий свет" },
	{ "lng_oblivion_photo_panel_blend_darken", "Затемнение" },
	{ "lng_oblivion_photo_panel_blend_lighten", "Замена светлым" },
	{ "lng_oblivion_photo_panel_blend_color_dodge", "Осветление основы" },
	{ "lng_oblivion_photo_panel_blend_color_burn", "Затемнение основы" },
	{ "lng_oblivion_photo_panel_blend_difference", "Разница" },
	{ "lng_oblivion_photo_panel_blend_exclusion", "Исключение" },
	{ "lng_oblivion_photo_panel_blend_add", "Сложение" },
	{ "lng_oblivion_photo_panel_blend_hue", "Цветовой тон" },
	{ "lng_oblivion_photo_panel_blend_saturation", "Насыщенность" },
	{ "lng_oblivion_photo_panel_blend_color", "Цветность" },
	{ "lng_oblivion_photo_panel_blend_luminosity", "Яркость" },
	{ "lng_oblivion_photo_panel_group_light", "Свет" },
	{ "lng_oblivion_photo_panel_group_color", "Цвет" },
	{ "lng_oblivion_photo_panel_group_detail", "Детали" },
	{ "lng_oblivion_photo_panel_group_finish", "Виньетка и зерно" },
	{ "lng_oblivion_photo_panel_group_blur", "Размытие" },
	{ "lng_oblivion_photo_panel_group_distort", "Искажение" },
	{ "lng_oblivion_photo_panel_group_lofi", "Плохая камера" },
	{ "lng_oblivion_photo_panel_group_glitch", "Глитч" },
	{ "lng_oblivion_photo_panel_group_stylize", "Стилизация" },
	{ "lng_oblivion_photo_panel_group_classic", "Классические" },
	{ "lng_oblivion_photo_panel_fine_title", "Точная коррекция" },
	{
		"lng_oblivion_photo_panel_fine_about",
		"Кривые, цвета по диапазонам (HSL), цветокоррекция по тонам, "
		"чёрно-белый микс, резкость, шумоподавление и другое "
		"добавляются к активному слою как эффекты.",
	},
	{ "lng_oblivion_photo_panel_fine_button", "Добавить коррекцию…" },
	{ "lng_oblivion_photo_panel_more_title", "Эффекты слоя" },
	{
		"lng_oblivion_photo_panel_more_about",
		"Размытия, искажения, «плохая камера» и глитч добавляются "
		"к активному слою, их можно сочетать.",
	},
	{ "lng_oblivion_photo_panel_more_button", "Добавить эффект слоя…" },
	{
		"lng_oblivion_photo_panel_locked_about",
		"Слой заблокирован. Разблокируйте его в списке слоёв, "
		"чтобы менять эффекты.",
	},
	{ "lng_oblivion_photo_panel_canvas_title", "Холст" },
	{ "lng_oblivion_photo_panel_canvas_width", "Ширина" },
	{ "lng_oblivion_photo_panel_canvas_height", "Высота" },
	{
		"lng_oblivion_photo_panel_canvas_extend",
		"Расширить холст до пропорций:",
	},
	{
		"lng_oblivion_photo_panel_canvas_about",
		"Нажмите на число, чтобы ввести точный размер. На большем "
		"холсте вокруг фото появляется прозрачное место для других "
		"слоёв, на меньшем всё, что выступает, скрывается. Слои "
		"остаются на своих местах, рамка обрезки сбрасывается.",
	},
	{
		"lng_oblivion_photo_panel_import_partial",
		"Добавлено фото: {added} из {total}. За один раз добавляется не "
		"больше {limit}, а файлы, которые не удалось открыть, "
		"пропускаются.",
	},
	{
		"lng_oblivion_photo_panel_kept",
		"Фоторедактор был закрыт. Незаконченная работа со всеми слоями "
		"хранится до выхода из приложения или из этого аккаунта: "
		"откройте фоторедактор, чтобы продолжить.",
	},
	{
		"lng_oblivion_photo_panel_restore_text",
		"В фоторедакторе осталась незаконченная работа. Продолжить её "
		"или начать новую? Если начать новую, незаконченная работа "
		"будет удалена.",
	},
	{ "lng_oblivion_photo_panel_restore_continue", "Продолжить" },
	{ "lng_oblivion_photo_panel_restore_new", "Начать новую" },
	{
		"lng_oblivion_photo_panel_merge_blend",
		"Эти слои нельзя объединить, не изменив картинку: у нижнего "
		"слоя задан режим наложения. Сначала выберите для него "
		"наложение «Обычное».",
	},
	// Oblivion round 4: video fx.
	{ "lng_oblivion_vfx_fx_tracking", "Трекинг объектов" },
	{ "lng_oblivion_vfx_fx_edges", "Светящиеся контуры" },
	{ "lng_oblivion_vfx_fx_feedback", "Шлейф" },
	{ "lng_oblivion_vfx_fx_difference", "Разница кадров" },
	{ "lng_oblivion_vfx_fx_slitscan", "Щелевая развёртка" },
	{ "lng_oblivion_vfx_fx_pixelsort", "Сортировка пикселей" },
	{ "lng_oblivion_vfx_fx_rgbsplit", "Расслоение RGB" },
	{ "lng_oblivion_vfx_fx_displace", "Смещение по яркости" },
	{ "lng_oblivion_vfx_fx_kaleidoscope", "Калейдоскоп" },
	{ "lng_oblivion_vfx_fx_mirror", "Зеркало" },
	{ "lng_oblivion_vfx_fx_ascii", "Символы ASCII" },
	{ "lng_oblivion_vfx_fx_halftone", "Печатный растр" },
	{ "lng_oblivion_vfx_fx_dither", "Дизеринг" },
	{ "lng_oblivion_vfx_fx_threshold", "Порог" },
	{ "lng_oblivion_vfx_fx_posterize", "Постеризация" },
	{ "lng_oblivion_vfx_fx_falsecolor", "Ложные цвета" },
	{ "lng_oblivion_vfx_fx_glitch", "Глитч" },
	{ "lng_oblivion_vfx_fx_datamosh", "Датамош" },
	{ "lng_oblivion_vfx_fx_crt", "Экран с кинескопом" },
	{ "lng_oblivion_vfx_fx_grain", "Шум и зерно" },
	{ "lng_oblivion_vfx_fx_strobe", "Стробоскоп" },
	{ "lng_oblivion_vfx_group_analysis", "Трекинг и контуры" },
	{ "lng_oblivion_vfx_group_time", "Время" },
	{ "lng_oblivion_vfx_group_distort", "Искажения" },
	{ "lng_oblivion_vfx_group_stylize", "Стилизация" },
	{ "lng_oblivion_vfx_p_source", "Что отслеживать" },
	{ "lng_oblivion_vfx_p_sensitivity", "Чувствительность" },
	{ "lng_oblivion_vfx_p_min_size", "Наименьший объект" },
	{ "lng_oblivion_vfx_p_limit", "Число объектов" },
	{ "lng_oblivion_vfx_p_style", "Стиль" },
	{ "lng_oblivion_vfx_p_color", "Цвет" },
	{ "lng_oblivion_vfx_p_background", "Цвет фона" },
	{ "lng_oblivion_vfx_p_width", "Толщина линий" },
	{ "lng_oblivion_vfx_p_links", "Соединительные линии" },
	{ "lng_oblivion_vfx_p_labels", "Подписи" },
	{ "lng_oblivion_vfx_p_text_size", "Размер подписей" },
	{ "lng_oblivion_vfx_p_trails", "Следы движения" },
	{ "lng_oblivion_vfx_p_smoothing", "Сглаживание" },
	{ "lng_oblivion_vfx_p_dim", "Затемнить видео" },
	{ "lng_oblivion_vfx_p_threshold", "Порог" },
	{ "lng_oblivion_vfx_p_amount", "Сила" },
	{ "lng_oblivion_vfx_p_glow", "Свечение" },
	{ "lng_oblivion_vfx_p_colors", "Цвета" },
	{ "lng_oblivion_vfx_p_original", "Исходное видео" },
	{ "lng_oblivion_vfx_p_persistence", "Длина шлейфа" },
	{ "lng_oblivion_vfx_p_zoom", "Масштаб" },
	{ "lng_oblivion_vfx_p_rotation", "Поворот" },
	{ "lng_oblivion_vfx_p_shift_x", "Сдвиг по горизонтали" },
	{ "lng_oblivion_vfx_p_shift_y", "Сдвиг по вертикали" },
	{ "lng_oblivion_vfx_p_mode", "Режим" },
	{ "lng_oblivion_vfx_p_hue", "Сдвиг оттенка" },
	{ "lng_oblivion_vfx_p_depth", "Глубина по времени" },
	{ "lng_oblivion_vfx_p_direction", "Направление" },
	{ "lng_oblivion_vfx_p_smooth", "Плавные переходы" },
	{ "lng_oblivion_vfx_p_low", "Яркость от" },
	{ "lng_oblivion_vfx_p_high", "Яркость до" },
	{ "lng_oblivion_vfx_p_key", "Признак сортировки" },
	{ "lng_oblivion_vfx_p_length", "Длина полос" },
	{ "lng_oblivion_vfx_p_angle", "Угол" },
	{ "lng_oblivion_vfx_p_smoothness", "Плавность" },
	{ "lng_oblivion_vfx_p_segments", "Секторы" },
	{ "lng_oblivion_vfx_p_spin", "Вращение" },
	{ "lng_oblivion_vfx_p_center_x", "Центр по горизонтали" },
	{ "lng_oblivion_vfx_p_center_y", "Центр по вертикали" },
	{ "lng_oblivion_vfx_p_position", "Положение зеркала" },
	{ "lng_oblivion_vfx_p_size", "Размер" },
	{ "lng_oblivion_vfx_p_charset", "Символы" },
	{ "lng_oblivion_vfx_p_invert", "Инвертировать" },
	{ "lng_oblivion_vfx_p_shape", "Форма" },
	{ "lng_oblivion_vfx_p_levels", "Число уровней" },
	{ "lng_oblivion_vfx_p_method", "Метод" },
	{ "lng_oblivion_vfx_p_pixel", "Размер пикселя" },
	{ "lng_oblivion_vfx_p_palette", "Палитра" },
	{ "lng_oblivion_vfx_p_softness", "Мягкость" },
	{ "lng_oblivion_vfx_p_level", "Уровень" },
	{ "lng_oblivion_vfx_p_keep_hues", "Сохранять оттенки" },
	{ "lng_oblivion_vfx_p_shift", "Сдвиг палитры" },
	{ "lng_oblivion_vfx_p_cycle", "Перелив" },
	{ "lng_oblivion_vfx_p_bands", "Полосы" },
	{ "lng_oblivion_vfx_p_slices", "Сдвиг полос" },
	{ "lng_oblivion_vfx_p_blocks", "Битые блоки" },
	{ "lng_oblivion_vfx_p_rgb", "Расслоение цветов" },
	{ "lng_oblivion_vfx_p_speed", "Скорость" },
	{ "lng_oblivion_vfx_p_seed", "Вариант" },
	{ "lng_oblivion_vfx_p_interval", "Интервал обновления" },
	{ "lng_oblivion_vfx_p_block", "Размер блока" },
	{ "lng_oblivion_vfx_p_scanlines", "Строки развёртки" },
	{ "lng_oblivion_vfx_p_curvature", "Выпуклость экрана" },
	{ "lng_oblivion_vfx_p_mask", "RGB-маска" },
	{ "lng_oblivion_vfx_p_vignette", "Виньетка" },
	{ "lng_oblivion_vfx_p_flicker", "Мерцание" },
	{ "lng_oblivion_vfx_p_colored", "Цветной шум" },
	{ "lng_oblivion_vfx_p_frequency", "Частота" },
	{ "lng_oblivion_vfx_p_duty", "Длина вспышки" },
	{ "lng_oblivion_vfx_o_auto", "Авто" },
	{ "lng_oblivion_vfx_o_motion", "Движение" },
	{ "lng_oblivion_vfx_o_contrast", "Контраст" },
	{ "lng_oblivion_vfx_o_bright", "Светлые области" },
	{ "lng_oblivion_vfx_o_dark", "Тёмные области" },
	{ "lng_oblivion_vfx_o_boxes", "Рамки" },
	{ "lng_oblivion_vfx_o_corners", "Уголки" },
	{ "lng_oblivion_vfx_o_points", "Точки" },
	{ "lng_oblivion_vfx_o_inverted", "Инверсия" },
	{ "lng_oblivion_vfx_o_off", "Нет" },
	{ "lng_oblivion_vfx_o_nearest", "К ближайшему" },
	{ "lng_oblivion_vfx_o_chain", "Цепочкой" },
	{ "lng_oblivion_vfx_o_web", "Паутина" },
	{ "lng_oblivion_vfx_o_id", "Номер" },
	{ "lng_oblivion_vfx_o_coords", "Координаты" },
	{ "lng_oblivion_vfx_o_id_coords", "Номер и координаты" },
	{ "lng_oblivion_vfx_o_tint", "Один цвет" },
	{ "lng_oblivion_vfx_o_source", "Цвета видео" },
	{ "lng_oblivion_vfx_o_rainbow", "Радуга" },
	{ "lng_oblivion_vfx_o_lighten", "Осветление" },
	{ "lng_oblivion_vfx_o_screen", "Экран" },
	{ "lng_oblivion_vfx_o_blend", "Смешивание" },
	{ "lng_oblivion_vfx_o_over", "Поверх видео" },
	{ "lng_oblivion_vfx_o_down", "Сверху вниз" },
	{ "lng_oblivion_vfx_o_up", "Снизу вверх" },
	{ "lng_oblivion_vfx_o_right", "Слева направо" },
	{ "lng_oblivion_vfx_o_left", "Справа налево" },
	{ "lng_oblivion_vfx_o_outward", "От центра" },
	{ "lng_oblivion_vfx_o_inward", "К центру" },
	{ "lng_oblivion_vfx_o_brightness", "Яркость" },
	{ "lng_oblivion_vfx_o_hue", "Оттенок" },
	{ "lng_oblivion_vfx_o_saturation", "Насыщенность" },
	{ "lng_oblivion_vfx_o_linear", "Под заданным углом" },
	{ "lng_oblivion_vfx_o_radial", "От центра" },
	{ "lng_oblivion_vfx_o_mirror_left", "Левая сторона" },
	{ "lng_oblivion_vfx_o_mirror_right", "Правая сторона" },
	{ "lng_oblivion_vfx_o_mirror_top", "Верх" },
	{ "lng_oblivion_vfx_o_mirror_bottom", "Низ" },
	{ "lng_oblivion_vfx_o_mirror_quad", "Четыре стороны" },
	{ "lng_oblivion_vfx_o_classic", "Классические" },
	{ "lng_oblivion_vfx_o_blocks", "Блоки" },
	{ "lng_oblivion_vfx_o_binary", "Нули и единицы" },
	{ "lng_oblivion_vfx_o_dots", "Точки" },
	{ "lng_oblivion_vfx_o_lines", "Линии" },
	{ "lng_oblivion_vfx_o_squares", "Квадраты" },
	{ "lng_oblivion_vfx_o_two_colors", "Два цвета" },
	{ "lng_oblivion_vfx_o_cmyk", "Цветная печать" },
	{ "lng_oblivion_vfx_o_bayer4", "Узор 4×4" },
	{ "lng_oblivion_vfx_o_bayer8", "Узор 8×8" },
	{ "lng_oblivion_vfx_o_floyd", "Флойд — Стейнберг" },
	{ "lng_oblivion_vfx_o_noise", "Шум" },
	{ "lng_oblivion_vfx_o_retro", "Ретроконсоль" },
	{ "lng_oblivion_vfx_o_thermal", "Тепловизор" },
	{ "lng_oblivion_vfx_o_night", "Ночное видение" },
	{ "lng_oblivion_vfx_o_fire", "Огонь" },
	{ "lng_oblivion_vfx_o_ice", "Лёд" },
	{ "lng_oblivion_vfx_o_neon", "Неон" },
	{ "lng_oblivion_vfx_o_xray", "Рентген" },
	{ "lng_oblivion_vfx_o_black", "Чёрный" },
	{ "lng_oblivion_vfx_o_white", "Белый" },
	{ "lng_oblivion_vfx_o_negative", "Негатив" },
	{ "lng_oblivion_vfx_o_freeze", "Стоп-кадр" },
	{ "lng_oblivion_vfx_unit_seconds", "с" },
	{ "lng_oblivion_vfx_unit_hertz", "Гц" },
	{ "lng_oblivion_vfx_preset_tracker", "Трекер объектов" },
	{ "lng_oblivion_vfx_preset_thermal", "Тепловизор" },
	{ "lng_oblivion_vfx_preset_neon", "Неоновые контуры" },
	{ "lng_oblivion_vfx_preset_tv", "Старый телевизор" },
	{ "lng_oblivion_vfx_preset_mosh", "Датамош" },
	{ "lng_oblivion_vfx_preset_terminal", "Терминал" },
	{ "lng_oblivion_vfx_preset_echo", "Эхо" },
	{ "lng_oblivion_vfx_preset_print", "Газета" },
	{ "lng_oblivion_vfx_preset_scanner", "Сканер времени" },
	{ "lng_oblivion_vfx_preset_motion", "Только движение" },
	{ "lng_oblivion_vfx_title", "Эффекты" },
	{ "lng_oblivion_vfx_none", "Нет" },
	{ "lng_oblivion_vfx_all_off", "Выключены" },
	{ "lng_oblivion_vfx_add", "Добавить эффект" },
	{ "lng_oblivion_vfx_presets", "Заготовки" },
	{ "lng_oblivion_vfx_remove_all", "Убрать все" },
	{ "lng_oblivion_vfx_done", "Готово" },
	{
		"lng_oblivion_vfx_empty",
		"Эффектов пока нет. Добавьте эффект или выберите заготовку: "
		"эффекты меняют всё видео и применяются по очереди, сверху вниз.",
	},
	{
		"lng_oblivion_vfx_hint",
		"Предпросмотр показывается в пониженном качестве. В готовом видео, "
		"GIF или стикере эффекты будут в полном качестве.",
	},
	{ "lng_oblivion_vfx_mix", "Интенсивность" },
	{ "lng_oblivion_vfx_randomize", "Случайно" },
	{ "lng_oblivion_vfx_move_up", "Выше" },
	{ "lng_oblivion_vfx_move_down", "Ниже" },
	{ "lng_oblivion_vfx_remove", "Убрать эффект" },
	{
		"lng_oblivion_vfx_limit",
		"Одновременно можно использовать не больше {max} эффектов.",
	},
	{ "lng_oblivion_vfx_color_title", "Цвет" },
	{
		"lng_oblivion_vfx_strobe_warning",
		"Частые вспышки могут быть опасны для людей с фоточувствительной "
		"эпилепсией.",
	},
	// Oblivion round 4: sticker export.
	{ "lng_oblivion_sexport_save_as", "Сохранить как" },
	{ "lng_oblivion_sexport_save_set", "Сохранить весь набор (.zip)" },
	{ "lng_oblivion_sexport_format_png", "Изображение PNG" },
	{ "lng_oblivion_sexport_format_webp", "Изображение WebP" },
	{ "lng_oblivion_sexport_format_tgs", "Стикер TGS" },
	{ "lng_oblivion_sexport_format_json", "Анимация Lottie JSON" },
	{ "lng_oblivion_sexport_format_gif", "Анимация GIF" },
	{ "lng_oblivion_sexport_format_webm", "Видео WebM" },
	{ "lng_oblivion_sexport_format_zip", "Архив ZIP" },
	{ "lng_oblivion_sexport_format_original", "{format} (оригинал)" },
	{ "lng_oblivion_sexport_dialog_sticker", "Сохранить стикер" },
	{ "lng_oblivion_sexport_dialog_emoji", "Сохранить эмодзи" },
	{ "lng_oblivion_sexport_dialog_set", "Сохранить набор" },
	{ "lng_oblivion_sexport_name_sticker", "Стикер" },
	{ "lng_oblivion_sexport_name_emoji", "Эмодзи" },
	{
		"lng_oblivion_sexport_downloading",
		"Скачиваем, файл сохранится через мгновение…",
	},
	{
		"lng_oblivion_sexport_converting",
		"Преобразуем в GIF, файл сохранится через мгновение…",
	},
	{ "lng_oblivion_sexport_saved_to", "Сохранено в {path}" },
	{
		"lng_oblivion_sexport_download_failed",
		"Не удалось скачать стикер, ничего не сохранено.",
	},
	{
		"lng_oblivion_sexport_convert_failed",
		"Не удалось преобразовать стикер в этот формат, ничего не "
		"сохранено.",
	},
	{
		"lng_oblivion_sexport_write_failed",
		"Не удалось записать файл в {path}",
	},
	{
		"lng_oblivion_sexport_busy",
		"Сейчас сохраняется другой набор. Дождитесь окончания.",
	},
	{
		"lng_oblivion_sexport_interrupted",
		"Сохранение набора отменено: окно сохранения было закрыто.",
	},
	{ "lng_oblivion_sexport_box_title", "Сохранение набора" },
	{ "lng_oblivion_sexport_stickers#one", "{count} стикер" },
	{ "lng_oblivion_sexport_stickers#few", "{count} стикера" },
	{ "lng_oblivion_sexport_stickers#many", "{count} стикеров" },
	{ "lng_oblivion_sexport_stickers#other", "{count} стикера" },
	{ "lng_oblivion_sexport_emoji#one", "{count} эмодзи" },
	{ "lng_oblivion_sexport_emoji#few", "{count} эмодзи" },
	{ "lng_oblivion_sexport_emoji#many", "{count} эмодзи" },
	{ "lng_oblivion_sexport_emoji#other", "{count} эмодзи" },
	{ "lng_oblivion_sexport_loading_set", "Загружаем набор…" },
	{
		"lng_oblivion_sexport_choose_path",
		"Выберите, куда сохранить архив…",
	},
	{
		"lng_oblivion_sexport_progress",
		"Скачиваем файлы: {ready} из {total}",
	},
	{ "lng_oblivion_sexport_writing", "Записываем архив…" },
	{ "lng_oblivion_sexport_done#one", "Готово: в архиве {count} файл." },
	{ "lng_oblivion_sexport_done#few", "Готово: в архиве {count} файла." },
	{
		"lng_oblivion_sexport_done#many",
		"Готово: в архиве {count} файлов.",
	},
	{
		"lng_oblivion_sexport_done#other",
		"Готово: в архиве {count} файла.",
	},
	{
		"lng_oblivion_sexport_done_partial",
		"Файлов в архиве: {ready} из {total}.\nОстальные не удалось "
		"скачать, они отмечены в файле manifest.json.",
	},
	{
		"lng_oblivion_sexport_failed_set",
		"Не удалось загрузить набор. Возможно, он был удалён.",
	},
	{
		"lng_oblivion_sexport_failed_empty",
		"Этот набор пуст, сохранять нечего.",
	},
	{
		"lng_oblivion_sexport_failed_download",
		"Не удалось скачать файлы набора. Проверьте подключение "
		"к интернету и попробуйте ещё раз.",
	},
	{
		"lng_oblivion_sexport_failed_write",
		"Не удалось записать архив. Возможно, на диске нет места или "
		"в эту папку нельзя сохранять файлы.",
	},
	{ "lng_oblivion_sexport_show_in_folder", "Показать в папке" },
	{ "lng_oblivion_sexport_show_in_finder", "Показать в Finder" },
	// Oblivion round 4: sticker batch.
	{ "lng_oblivion_sbatch_settings", "Пакетное создание стикеров" },
	{ "lng_oblivion_sbatch_add_several", "Добавить несколько файлов…" },
	{ "lng_oblivion_sbatch_new_emoji_title", "Новый набор эмодзи" },
	{
		"lng_oblivion_sbatch_new_emoji_about",
		"Набор будет создан, когда в него загрузится первый файл.",
	},
	{
		"lng_oblivion_sbatch_new_emoji_hint",
		"Латинские буквы, цифры и подчёркивания. Ссылка на набор "
		"будет такой: t.me/addemoji/имя.",
	},
	{ "lng_oblivion_sbatch_target", "Набор" },
	{ "lng_oblivion_sbatch_target_choose", "Выбрать" },
	{ "lng_oblivion_sbatch_target_loading", "Загрузка…" },
	{ "lng_oblivion_sbatch_target_new", "Новый: {title}" },
	{ "lng_oblivion_sbatch_target_emoji", "{title} (эмодзи)" },
	{ "lng_oblivion_sbatch_menu_new_stickers", "Новый набор стикеров…" },
	{ "lng_oblivion_sbatch_menu_new_emoji", "Новый набор эмодзи…" },
	{ "lng_oblivion_sbatch_emoji_default", "Эмодзи для всех файлов" },
	{ "lng_oblivion_sbatch_emoji_default_title", "Эмодзи для всех файлов" },
	{
		"lng_oblivion_sbatch_emoji_default_about",
		"Для файлов, у которых нет своих эмодзи",
	},
	{
		"lng_oblivion_sbatch_emoji_default_empty",
		"В очереди пока нет файлов",
	},
	{ "lng_oblivion_sbatch_emoji_title", "Эмодзи файла" },
	{ "lng_oblivion_sbatch_emoji_about", "Выберите эмодзи для этого файла" },
	{
		"lng_oblivion_sbatch_emoji_required",
		"Выберите хотя бы один эмодзи.",
	},
	{ "lng_oblivion_sbatch_done", "Готово" },
	{ "lng_oblivion_sbatch_drop_title", "Перетащите файлы сюда" },
	{
		"lng_oblivion_sbatch_drop_about",
		"или нажмите, чтобы выбрать их. Подойдут картинки, GIF, видео, "
		".tgs и Lottie JSON — всё конвертируется автоматически. От видео "
		"берутся первые 3\xC2\xA0" "секунды.",
	},
	{ "lng_oblivion_sbatch_add_files", "Добавить файлы" },
	{ "lng_oblivion_sbatch_choose_files", "Выберите файлы для стикеров" },
	{ "lng_oblivion_sbatch_upload", "Загрузить" },
	{ "lng_oblivion_sbatch_pause", "Пауза" },
	{ "lng_oblivion_sbatch_resume", "Продолжить" },
	{ "lng_oblivion_sbatch_stop", "Остановить" },
	{ "lng_oblivion_sbatch_retry", "Повторить" },
	{ "lng_oblivion_sbatch_type_static", "Картинка" },
	{ "lng_oblivion_sbatch_type_animated", "Анимация" },
	{ "lng_oblivion_sbatch_type_video", "Видео" },
	{ "lng_oblivion_sbatch_seconds", "{value} с" },
	{ "lng_oblivion_sbatch_row_waiting", "Ждёт конвертации" },
	{ "lng_oblivion_sbatch_row_converting", "Конвертируем…" },
	{
		"lng_oblivion_sbatch_row_converting_percent",
		"Конвертируем… {percent}",
	},
	{ "lng_oblivion_sbatch_row_uploading", "Загружаем… {percent}" },
	{ "lng_oblivion_sbatch_row_adding", "Добавляем в набор…" },
	{ "lng_oblivion_sbatch_row_done", "Добавлено в набор" },
	{
		"lng_oblivion_sbatch_row_checking",
		"Проверяем, попал ли файл в набор…",
	},
	{
		"lng_oblivion_sbatch_row_unsure",
		"Загрузка прервана, проверьте набор",
	},
	{ "lng_oblivion_sbatch_row_fixed", "исправлена" },
	{ "lng_oblivion_sbatch_problem_read", "Не удалось прочитать файл" },
	{
		"lng_oblivion_sbatch_problem_unknown",
		"Из такого файла стикер не сделать",
	},
	{
		"lng_oblivion_sbatch_problem_image",
		"Не удалось конвертировать картинку",
	},
	{
		"lng_oblivion_sbatch_problem_lottie",
		"Telegram не примет эту анимацию",
	},
	{
		"lng_oblivion_sbatch_problem_video",
		"Не удалось конвертировать видео",
	},
	{ "lng_oblivion_sbatch_error_upload", "Не удалось загрузить файл" },
	{
		"lng_oblivion_sbatch_error_file",
		"Telegram не принял файл ({error})",
	},
	{ "lng_oblivion_sbatch_error_emoji", "Telegram не принял эмодзи" },
	{
		"lng_oblivion_sbatch_error_generic",
		"Не удалось добавить файл ({error})",
	},
	{ "lng_oblivion_sbatch_menu_emoji", "Изменить эмодзи…" },
	{ "lng_oblivion_sbatch_menu_emoji_reset", "Сбросить эмодзи файла" },
	{ "lng_oblivion_sbatch_menu_trim", "Обрезать видео…" },
	{ "lng_oblivion_sbatch_menu_retry", "Повторить" },
	{ "lng_oblivion_sbatch_menu_remove", "Убрать из очереди" },
	{ "lng_oblivion_sbatch_menu_clear", "Очистить очередь" },
	{
		"lng_oblivion_sbatch_status_drop",
		"Отпустите файлы, чтобы добавить их в очередь.",
	},
	{
		"lng_oblivion_sbatch_status_preparing",
		"Готовим файлы… {ready} из {total}",
	},
	{
		"lng_oblivion_sbatch_status_ready#one",
		"К загрузке готов {count} файл.",
	},
	{
		"lng_oblivion_sbatch_status_ready#few",
		"К загрузке готовы {count} файла.",
	},
	{
		"lng_oblivion_sbatch_status_ready#many",
		"К загрузке готово {count} файлов.",
	},
	{
		"lng_oblivion_sbatch_status_ready#other",
		"К загрузке готовы {count} файла.",
	},
	{
		"lng_oblivion_sbatch_status_invalid#one",
		"Не подходит {count} файл.",
	},
	{
		"lng_oblivion_sbatch_status_invalid#few",
		"Не подходят {count} файла.",
	},
	{
		"lng_oblivion_sbatch_status_invalid#many",
		"Не подходит {count} файлов.",
	},
	{
		"lng_oblivion_sbatch_status_invalid#other",
		"Не подходят {count} файла.",
	},
	{
		"lng_oblivion_sbatch_status_uploading",
		"Загружаем {index} из {total}…",
	},
	{
		"lng_oblivion_sbatch_status_converting",
		"Ждём, пока конвертируется следующий файл…",
	},
	{
		"lng_oblivion_sbatch_status_flood",
		"Telegram просит подождать {time}, затем продолжим.",
	},
	{
		"lng_oblivion_sbatch_status_paused",
		"Пауза. Загружено {done} из {total}.",
	},
	{
		"lng_oblivion_sbatch_status_pausing",
		"Загрузка остановится после текущего файла…",
	},
	{
		"lng_oblivion_sbatch_status_stopped",
		"Загрузка остановлена. Загружено {done} из {total}.",
	},
	{
		"lng_oblivion_sbatch_status_restored",
		"Прошлая загрузка не завершена: добавлено {done} из {total}. "
		"Остальные файлы снова в очереди.",
	},
	{
		"lng_oblivion_sbatch_status_errors",
		"Несколько ошибок подряд — загрузка приостановлена. Проверьте "
		"соединение и нажмите «Продолжить».",
	},
	{
		"lng_oblivion_sbatch_status_floods",
		"Telegram несколько раз подряд попросил подождать. Загрузка "
		"приостановлена, продолжите её позже.",
	},
	{
		"lng_oblivion_sbatch_status_full",
		"Набор заполнен (максимум — {max}). Выберите другой набор "
		"для оставшихся файлов.",
	},
	{
		"lng_oblivion_sbatch_status_name",
		"Это короткое имя занято или не подходит. Нажмите «Загрузить» "
		"и введите другое.",
	},
	{
		"lng_oblivion_sbatch_status_target",
		"Набор не найден. Выберите другой.",
	},
	{
		"lng_oblivion_sbatch_status_done#one",
		"Готово: добавлен {count} файл.",
	},
	{
		"lng_oblivion_sbatch_status_done#few",
		"Готово: добавлено {count} файла.",
	},
	{
		"lng_oblivion_sbatch_status_done#many",
		"Готово: добавлено {count} файлов.",
	},
	{
		"lng_oblivion_sbatch_status_done#other",
		"Готово: добавлено {count} файла.",
	},
	{
		"lng_oblivion_sbatch_status_failed#one",
		"Не удалось добавить {count} файл.",
	},
	{
		"lng_oblivion_sbatch_status_failed#few",
		"Не удалось добавить {count} файла.",
	},
	{
		"lng_oblivion_sbatch_status_failed#many",
		"Не удалось добавить {count} файлов.",
	},
	{
		"lng_oblivion_sbatch_status_failed#other",
		"Не удалось добавить {count} файла.",
	},
	{
		"lng_oblivion_sbatch_toast_skipped#one",
		"Пропущен {count} файл: из такого стикер не сделать.",
	},
	{
		"lng_oblivion_sbatch_toast_skipped#few",
		"Пропущено {count} файла: из таких стикер не сделать.",
	},
	{
		"lng_oblivion_sbatch_toast_skipped#many",
		"Пропущено {count} файлов: из таких стикер не сделать.",
	},
	{
		"lng_oblivion_sbatch_toast_skipped#other",
		"Пропущено {count} файла: из таких стикер не сделать.",
	},
	{
		"lng_oblivion_sbatch_toast_limit",
		"В очереди помещается не больше {max} файлов.",
	},
	{ "lng_oblivion_sbatch_toast_target", "Сначала выберите набор." },
	{
		"lng_oblivion_sbatch_toast_loading",
		"Список ваших наборов ещё загружается.",
	},
	{ "lng_oblivion_sbatch_toast_busy", "Сначала остановите загрузку." },
	{ "lng_oblivion_sbatch_toast_nothing", "Загружать пока нечего." },
	// Oblivion round 4: attach tools.
	{
		"lng_oblivion_attach_settings",
		"Кнопки инструментов при отправке файлов",
	},
	{ "lng_oblivion_attach_photo_editor", "Редактор" },
	{
		"lng_oblivion_attach_photo_editor_tip",
		"Открыть в фоторедакторе Oblivion. Результат заменит "
		"прикреплённое фото.",
	},
	{ "lng_oblivion_attach_sticker", "Стикер" },
	{
		"lng_oblivion_attach_sticker_tip",
		"Сделать из этого фото стикер и добавить его в ваш набор.",
	},
	{ "lng_oblivion_attach_cutout", "Без фона" },
	{
		"lng_oblivion_attach_cutout_tip",
		"Убрать фон: вырезать объект с фото.",
	},
	{ "lng_oblivion_attach_text", "Текст с фото" },
	{
		"lng_oblivion_attach_text_tip",
		"Распознать текст на фото, чтобы скопировать его.",
	},
	{ "lng_oblivion_attach_video_editor", "Редактор" },
	{
		"lng_oblivion_attach_video_editor_tip",
		"Открыть в видеоредакторе: обрезка, кадрирование, скорость.",
	},
	{
		"lng_oblivion_attach_video_editor_closes_tip",
		"Открыть в видеоредакторе: обрезка, кадрирование, скорость.\n"
		"Это окно закроется, подпись вернётся в поле сообщения.",
	},
	{ "lng_oblivion_attach_round", "Кружок" },
	{
		"lng_oblivion_attach_round_tip",
		"Отправить кружком — как круглое видеосообщение.\n"
		"Это окно закроется, подпись вернётся в поле сообщения.",
	},
	{ "lng_oblivion_attach_video_sticker", "Стикер" },
	{
		"lng_oblivion_attach_video_sticker_tip",
		"Сделать видеостикер из фрагмента до 3 секунд.",
	},
	{ "lng_oblivion_attach_gif", "GIF" },
	{
		"lng_oblivion_attach_gif_tip",
		"Превратить в GIF: зацикленное видео без звука заменит "
		"прикреплённое.",
	},
	{ "lng_oblivion_attach_music", "Музыкальный редактор" },
	{
		"lng_oblivion_attach_music_tip",
		"Открыть в музыкальном редакторе: темп, тон, реверберация, "
		"обрезка.",
	},
	{ "lng_oblivion_attach_lottie", "Редактор Lottie" },
	{
		"lng_oblivion_attach_lottie_tip",
		"Открыть анимацию в редакторе Lottie.\n"
		"«Сохранить» в редакторе изменит сам прикреплённый файл.",
	},
	{ "lng_oblivion_attach_gif_title", "Создание GIF" },
	{
		"lng_oblivion_attach_gif_progress",
		"Преобразование видео… {percent}",
	},
	{
		"lng_oblivion_attach_gif_about",
		"Прикреплённое видео будет заменено зацикленным роликом без "
		"звука.",
	},
	{ "lng_oblivion_attach_gif_done", "Видео заменено на GIF." },
	{
		"lng_oblivion_attach_gif_failed",
		"Не удалось сделать GIF из этого видео.",
	},
	{ "lng_oblivion_attach_open_failed", "Не удалось открыть этот файл." },
	{
		"lng_oblivion_attach_lottie_failed",
		"В этом файле нет анимации Lottie.",
	},
	// Oblivion round 4: chat stats.
	{ "lng_oblivion_stats_menu", "Статистика чата" },
	{
		"lng_oblivion_stats_unsupported",
		"Статистика считается только для личных чатов с людьми и для "
		"групп, в которых вы состоите.",
	},
	{ "lng_oblivion_stats_period_3m", "3 месяца" },
	{ "lng_oblivion_stats_period_year", "Год" },
	{ "lng_oblivion_stats_period_all", "Всё время" },
	{ "lng_oblivion_stats_refresh", "Обновить" },
	{ "lng_oblivion_stats_stop", "Остановить" },
	{ "lng_oblivion_stats_resume", "Продолжить" },
	{ "lng_oblivion_stats_retry", "Повторить" },
	{ "lng_oblivion_stats_clear", "Очистить данные" },
	{
		"lng_oblivion_stats_clear_sure",
		"Удалить сохранённую статистику этого чата? История будет "
		"загружена заново, когда вы откроете статистику в следующий раз.",
	},
	{ "lng_oblivion_stats_status_loading", "Загрузка сохранённых данных…" },
	{ "lng_oblivion_stats_status_reading", "Загрузка истории…" },
	{
		"lng_oblivion_stats_status_reading_till",
		"Загрузка истории: прочитано до {date}",
	},
	{ "lng_oblivion_stats_status_count#one", "Загружено {count} сообщение." },
	{ "lng_oblivion_stats_status_count#few", "Загружено {count} сообщения." },
	{ "lng_oblivion_stats_status_count#many", "Загружено {count} сообщений." },
	{
		"lng_oblivion_stats_status_count#other",
		"Загружено {count} сообщения.",
	},
	{
		"lng_oblivion_stats_status_background",
		"Это окно можно закрыть — загрузка продолжится.",
	},
	{
		"lng_oblivion_stats_status_waiting",
		"В очереди: сначала загружается чат «{chat}».",
	},
	{
		"lng_oblivion_stats_status_flood",
		"Telegram попросил подождать {time}. Загрузка продолжится сама.",
	},
	{
		"lng_oblivion_stats_status_stopped",
		"Загрузка остановлена, данные неполные.",
	},
	{
		"lng_oblivion_stats_status_incomplete",
		"Данные за этот период неполные.",
	},
	{
		"lng_oblivion_stats_status_failed",
		"Не удалось загрузить историю: {error}",
	},
	{
		"lng_oblivion_stats_status_failed_access",
		"Нет доступа к истории этого чата.",
	},
	{
		"lng_oblivion_stats_status_failed_stuck",
		"Сервер вернул неожиданный ответ. Попробуйте позже.",
	},
	{
		"lng_oblivion_stats_status_failed_cache",
		"Не удалось прочитать сохранённую статистику этого чата. "
		"Попробуйте ещё раз или очистите данные.",
	},
	{ "lng_oblivion_stats_updated", "Обновлено {date}" },
	{ "lng_oblivion_stats_updated_now", "Обновлено только что" },
	{ "lng_oblivion_stats_messages#one", "{count} сообщение" },
	{ "lng_oblivion_stats_messages#few", "{count} сообщения" },
	{ "lng_oblivion_stats_messages#many", "{count} сообщений" },
	{ "lng_oblivion_stats_messages#other", "{count} сообщения" },
	{ "lng_oblivion_stats_words#one", "{count} слово" },
	{ "lng_oblivion_stats_words#few", "{count} слова" },
	{ "lng_oblivion_stats_words#many", "{count} слов" },
	{ "lng_oblivion_stats_words#other", "{count} слова" },
	{ "lng_oblivion_stats_chars#one", "{count} символ" },
	{ "lng_oblivion_stats_chars#few", "{count} символа" },
	{ "lng_oblivion_stats_chars#many", "{count} символов" },
	{ "lng_oblivion_stats_chars#other", "{count} символа" },
	{ "lng_oblivion_stats_days#one", "{count} день" },
	{ "lng_oblivion_stats_days#few", "{count} дня" },
	{ "lng_oblivion_stats_days#many", "{count} дней" },
	{ "lng_oblivion_stats_days#other", "{count} дня" },
	{ "lng_oblivion_stats_people_more#one", "и ещё {count} участник" },
	{ "lng_oblivion_stats_people_more#few", "и ещё {count} участника" },
	{ "lng_oblivion_stats_people_more#many", "и ещё {count} участников" },
	{ "lng_oblivion_stats_people_more#other", "и ещё {count} участника" },
	{ "lng_oblivion_stats_span_s", "{value} с" },
	{ "lng_oblivion_stats_span_m", "{value} мин" },
	{ "lng_oblivion_stats_span_h", "{value} ч" },
	{ "lng_oblivion_stats_span_hm", "{hours} ч {minutes} мин" },
	{ "lng_oblivion_stats_cell_messages", "Сообщения" },
	{ "lng_oblivion_stats_cell_words", "Слова" },
	{ "lng_oblivion_stats_cell_people", "Участники" },
	{ "lng_oblivion_stats_cell_emoji", "Эмодзи" },
	{ "lng_oblivion_stats_cell_days", "Активные дни" },
	{ "lng_oblivion_stats_cell_per_day", "В активный день" },
	{ "lng_oblivion_stats_cell_length", "Слов в сообщении" },
	{ "lng_oblivion_stats_section_people", "Участники" },
	{ "lng_oblivion_stats_section_people_private", "Кто сколько пишет" },
	{ "lng_oblivion_stats_section_hours", "По часам" },
	{ "lng_oblivion_stats_section_weekdays", "По дням недели" },
	{ "lng_oblivion_stats_section_calendar", "Календарь" },
	{ "lng_oblivion_stats_section_days", "По дням" },
	{ "lng_oblivion_stats_section_months", "По месяцам" },
	{ "lng_oblivion_stats_section_words", "Частые слова" },
	{ "lng_oblivion_stats_section_emoji", "Частые эмодзи" },
	{ "lng_oblivion_stats_section_stickers", "Частые стикеры" },
	{ "lng_oblivion_stats_section_media", "По типам сообщений" },
	{ "lng_oblivion_stats_section_facts", "Интересное" },
	{ "lng_oblivion_stats_you", "Вы" },
	{ "lng_oblivion_stats_unknown", "Неизвестный участник" },
	{ "lng_oblivion_stats_answer_you", "отвечаете за {time}" },
	{ "lng_oblivion_stats_answer_other", "отвечает за {time}" },
	{ "lng_oblivion_stats_kind_text", "Только текст" },
	{ "lng_oblivion_stats_kind_photo", "Фото" },
	{ "lng_oblivion_stats_kind_video", "Видео" },
	{ "lng_oblivion_stats_kind_round", "Кружки" },
	{ "lng_oblivion_stats_kind_voice", "Голосовые" },
	{ "lng_oblivion_stats_kind_music", "Музыка" },
	{ "lng_oblivion_stats_kind_file", "Файлы" },
	{ "lng_oblivion_stats_kind_sticker", "Стикеры" },
	{ "lng_oblivion_stats_kind_gif", "GIF" },
	{ "lng_oblivion_stats_kind_poll", "Опросы" },
	{ "lng_oblivion_stats_kind_location", "Геопозиции" },
	{ "lng_oblivion_stats_kind_contact", "Контакты" },
	{ "lng_oblivion_stats_kind_call", "Звонки" },
	{ "lng_oblivion_stats_kind_other", "Другое" },
	{ "lng_oblivion_stats_kind_link", "Со ссылками" },
	{ "lng_oblivion_stats_fact_period", "Даты сообщений" },
	{ "lng_oblivion_stats_fact_first", "Первое сообщение" },
	{
		"lng_oblivion_stats_fact_first_period",
		"Первое сообщение за период",
	},
	{ "lng_oblivion_stats_fact_streak", "Дольше всего подряд" },
	{ "lng_oblivion_stats_fact_streak_now", "Сейчас подряд" },
	{ "lng_oblivion_stats_fact_top_day", "Самый активный день" },
	{ "lng_oblivion_stats_fact_top_days", "Самые активные дни" },
	{ "lng_oblivion_stats_fact_replies", "Ответы" },
	{ "lng_oblivion_stats_fact_forwards", "Пересланные" },
	{ "lng_oblivion_stats_fact_length", "Среднее сообщение" },
	{ "lng_oblivion_stats_tip_none", "нет сообщений" },
	{ "lng_oblivion_stats_less", "меньше" },
	{ "lng_oblivion_stats_more", "больше" },
	{
		"lng_oblivion_stats_empty_reading",
		"Первые цифры появятся через несколько секунд.",
	},
	{
		"lng_oblivion_stats_empty_waiting",
		"Статистика появится здесь, когда история загрузится.",
	},
	{
		"lng_oblivion_stats_empty_cleared",
		"Данных пока нет. Нажмите «Обновить», чтобы загрузить историю "
		"чата.",
	},
	{
		"lng_oblivion_stats_empty_chat",
		"В этом чате пока нет сообщений.",
	},
	{
		"lng_oblivion_stats_empty_period",
		"За выбранный период в этом чате нет сообщений.",
	},
	{
		"lng_oblivion_stats_about",
		"Всё считается на этом устройстве и никуда не отправляется. "
		"Ни тексты, ни список сообщений не сохраняются — только "
		"счётчики: за каждый день сколько сообщений, слов и символов "
		"отправил каждый участник, сколько было эмодзи, ответов, "
		"пересылок и ссылок, в какие часы и какого типа были сообщения и "
		"как быстро приходили ответы. Ещё сохраняются имена участников, "
		"время и автор первого и последнего учтённого сообщения и то, до "
		"какого места загружена история. Слова, эмодзи и стикеры "
		"считаются за целые месяцы. Слово, эмодзи или стикер сохраняется "
		"как есть, только когда встретится три раза (стикер — со своим "
		"эмодзи и номером одного сообщения, чтобы показать картинку). Их "
		"счётчики по "
		"месяцам хранятся обезличенно: без ключа, которого нет в файле, "
		"не узнать, какому слову они принадлежат. Сам файл не "
		"зашифрован. Сообщения, удалённые после подсчёта, остаются в "
		"статистике. Чтобы стереть всё сохранённое, нажмите «Очистить "
		"данные».",
	},
	// Oblivion round 4: badge.
	{ "lng_oblivion_badge_settings", "Значок Oblivion" },
	{
		"lng_oblivion_badge_settings_about",
		"Oblivion допишет невидимую метку в конец вашего раздела «О "
		"себе», поэтому он не должен быть пустым. По метке другие "
		"пользователи Oblivion увидят значок Oblivion рядом с вашим "
		"именем, в других приложениях ничего не изменится. Это не "
		"верификация Telegram. Если выключить, метка будет удалена. "
		"Действует только для текущего аккаунта.",
	},
	{ "lng_oblivion_badge_tooltip", "Пользователь Oblivion" },
	{ "lng_oblivion_badge_consent_title", "Значок Oblivion" },
	{
		"lng_oblivion_badge_consent_preview",
		"Так вас увидят в Oblivion",
	},
	{
		"lng_oblivion_badge_consent_about",
		"Значок Oblivion появится рядом с вашим именем. Это не "
		"верификация Telegram и не официальная галочка.",
	},
	{
		"lng_oblivion_badge_consent_bio",
		"Для этого Oblivion допишет невидимую метку из нескольких "
		"символов в конец раздела «О\xC2\xA0" "себе» аккаунта {name}. Ваш "
		"текст останется прежним, метка лишь займёт немного места из "
		"лимита длины.",
	},
	{
		"lng_oblivion_badge_consent_reveals",
		"Значок увидят все, кто откроет ваш профиль в Oblivion, и "
		"поймут, что вы тоже пользуетесь Oblivion. В обычных "
		"приложениях Telegram метка не видна, но остаётся частью "
		"текста «О\xC2\xA0" "себе».",
	},
	{
		"lng_oblivion_badge_consent_off",
		"Выключить значок можно в настройках Oblivion: метка будет "
		"убрана из «О\xC2\xA0" "себе».",
	},
	{ "lng_oblivion_badge_consent_enable", "Включить" },
	{ "lng_oblivion_badge_on", "Значок Oblivion включён." },
	{ "lng_oblivion_badge_off", "Значок Oblivion выключен." },
	{
		"lng_oblivion_badge_off_removed",
		"Значок Oblivion выключен, метка убрана из «О себе».",
	},
	{
		"lng_oblivion_badge_off_later",
		"Значок Oblivion выключен. Метка будет убрана из «О себе» при "
		"следующем запуске Oblivion с этим аккаунтом.",
	},
	{
		"lng_oblivion_badge_off_wait",
		"Значок Oblivion выключен. Telegram просит подождать, прежде "
		"чем снова менять профиль, поэтому Oblivion уберёт метку из "
		"«О себе» при запуске с этим аккаунтом после этого ожидания.",
	},
	{
		"lng_oblivion_badge_no_room#one",
		"В «О себе» не хватает места для метки. Сократите текст на "
		"{count} символ и включите значок снова.",
	},
	{
		"lng_oblivion_badge_no_room#few",
		"В «О себе» не хватает места для метки. Сократите текст на "
		"{count} символа и включите значок снова.",
	},
	{
		"lng_oblivion_badge_no_room#many",
		"В «О себе» не хватает места для метки. Сократите текст на "
		"{count} символов и включите значок снова.",
	},
	{
		"lng_oblivion_badge_no_room#other",
		"В «О себе» не хватает места для метки. Сократите текст на "
		"{count} символа и включите значок снова.",
	},
	{
		"lng_oblivion_badge_no_room_off",
		"В «О себе» не осталось места для метки Oblivion, поэтому "
		"значок выключен.",
	},
	{
		"lng_oblivion_badge_empty_bio",
		"Раздел «О себе» пуст, поэтому другие не видят ваш значок "
		"Oblivion. Напишите что-нибудь о себе, и метка вернётся. Если "
		"при следующем запуске Oblivion раздел останется пустым, значок "
		"будет выключен.",
	},
	{
		"lng_oblivion_badge_needs_bio",
		"Для значка нужен непустой раздел «О себе»: Oblivion не "
		"записывает «О себе», состоящее из одной невидимой метки. "
		"Напишите что-нибудь о себе и включите значок снова.",
	},
	{
		"lng_oblivion_badge_dropped",
		"Telegram не сохранил метку в «О себе», поэтому значок "
		"выключен. Текст «О себе» не изменился.",
	},
	{
		"lng_oblivion_badge_dropped_part",
		"Telegram сохранил метку лишь частично, поэтому значок "
		"выключен. В конце «О себе» осталось несколько невидимых "
		"символов. Oblivion уберёт их при следующем запуске.",
	},
	{
		"lng_oblivion_badge_lost",
		"Метки Oblivion больше нет в вашем «О себе», поэтому значок "
		"выключен. Включить его снова можно в настройках Oblivion.",
	},
	{
		"lng_oblivion_badge_adopted",
		"В «О себе» этого аккаунта есть метка Oblivion, поэтому значок "
		"включён и здесь. Выключить его можно в настройках Oblivion.",
	},
	{
		"lng_oblivion_badge_stuck",
		"Oblivion несколько раз не смог убрать свою метку из «О себе». "
		"Значок остаётся выключенным. Чтобы убрать метку самостоятельно, "
		"измените текст «О себе» в настройках: при сохранении метка "
		"исчезнет.",
	},
	{
		"lng_oblivion_badge_once",
		"Ради значка Oblivion меняет «О себе» не чаще одного раза за "
		"запуск. Перезапустите Oblivion и попробуйте снова.",
	},
	{
		"lng_oblivion_badge_wait",
		"Telegram просит подождать, прежде чем снова менять профиль. "
		"Попробуйте позже.",
	},
	{
		"lng_oblivion_badge_failed",
		"Не удалось связаться с Telegram. Попробуйте позже.",
	},
	{
		"lng_oblivion_badge_rejected",
		"Telegram не принял изменение «О себе», поэтому значок остался "
		"выключенным. Если текст «О себе» почти достиг лимита длины, "
		"сократите его и попробуйте снова после перезапуска Oblivion.",
	},
	{
		"lng_oblivion_badge_busy",
		"Секунду, предыдущее изменение ещё сохраняется.",
	},
	// Oblivion round 4: listen together.
	{ "lng_oblivion_listen_settings", "Слушать вместе" },
	{
		"lng_oblivion_listen_settings_about",
		"Можно запустить совместное прослушивание трека в чате или "
		"присоединиться к чужому сеансу. Работает только между "
		"пользователями Oblivion: состояние воспроизведения хранится "
		"в обычном сообщении чата, которое приложение ведущего "
		"редактирует, пока играет музыка.",
	},
	{ "lng_oblivion_listen_menu", "Слушать вместе в этом чате" },
	{ "lng_oblivion_listen_message", "Слушаем вместе: {track}" },
	{
		"lng_oblivion_listen_message_ended",
		"Совместное прослушивание завершено: {track}",
	},
	{ "lng_oblivion_listen_start_title", "Слушать вместе" },
	{
		"lng_oblivion_listen_start_text",
		"В чат «{chat}» будет отправлено сообщение «Слушаем вместе: "
		"{track}».",
	},
	{
		"lng_oblivion_listen_start_text_about",
		"Участники чата, у которых установлен Oblivion, смогут "
		"присоединиться и слушать трек одновременно с вами. Управлять "
		"воспроизведением будете только\xC2\xA0" "вы.",
	},
	{
		"lng_oblivion_listen_start_text_send",
		"В этом чате такого трека ещё нет, поэтому сам трек тоже будет "
		"туда отправлен.",
	},
	{
		"lng_oblivion_listen_start_text_online",
		"Пока идёт сеанс, приложение редактирует это сообщение, поэтому "
		"вас могут увидеть в сети, хотя сообщения отправляются без "
		"появления в сети.",
	},
	{ "lng_oblivion_listen_start_button", "Начать" },
	{ "lng_oblivion_listen_start_button_send", "Отправить и начать" },
	{ "lng_oblivion_listen_join", "Присоединиться" },
	{ "lng_oblivion_listen_leave", "Выйти" },
	{ "lng_oblivion_listen_end", "Завершить" },
	{ "lng_oblivion_listen_send", "Отправить в чат" },
	{ "lng_oblivion_listen_bar_title", "Слушаем вместе" },
	{ "lng_oblivion_listen_bar_title_offer", "{name} слушает музыку" },
	{
		"lng_oblivion_listen_bar_title_joined",
		"Слушаем вместе · ведёт {name}",
	},
	{
		"lng_oblivion_listen_bar_title_host",
		"Слушаем вместе · вы ведущий",
	},
	{
		"lng_oblivion_listen_bar_title_starting",
		"Запускаем совместное прослушивание…",
	},
	{
		"lng_oblivion_listen_bar_title_ended",
		"Совместное прослушивание завершено",
	},
	{ "lng_oblivion_listen_bar_track_unknown", "Трек из этого чата" },
	{ "lng_oblivion_listen_bar_paused", "пауза" },
	{ "lng_oblivion_listen_bar_loading", "загрузка…" },
	{ "lng_oblivion_listen_bar_own_pause", "у вас на паузе" },
	{ "lng_oblivion_listen_bar_waiting", "Ждём ведущего…" },
	{
		"lng_oblivion_listen_bar_unavailable",
		"Этот трек не найден в чате",
	},
	{ "lng_oblivion_listen_bar_away", "нет в этом чате" },
	{ "lng_oblivion_listen_bar_sending", "отправляется…" },
	{
		"lng_oblivion_listen_bar_host_away",
		"Ведущий слушает трек, которого нет в этом чате",
	},
	{
		"lng_oblivion_listen_error_off",
		"Совместное прослушивание выключено в настройках Oblivion.",
	},
	{
		"lng_oblivion_listen_error_chat",
		"Слушать вместе можно в личных чатах и в группах без тем.",
	},
	{
		"lng_oblivion_listen_error_rights",
		"Вы не можете отправлять сообщения в этот чат.",
	},
	{
		"lng_oblivion_listen_error_paid",
		"Сообщения в этот чат платные, начать там совместное "
		"прослушивание нельзя.",
	},
	{ "lng_oblivion_listen_error_track", "Это не музыкальный трек." },
	{
		"lng_oblivion_listen_error_hosting",
		"Вы уже ведёте совместное прослушивание. Сначала завершите его.",
	},
	{
		"lng_oblivion_listen_error_send_track",
		"Этот трек нельзя отправить в чат.",
	},
	{
		"lng_oblivion_listen_error_start",
		"Не удалось запустить совместное прослушивание.",
	},
	{
		"lng_oblivion_listen_error_edit",
		"Совместное прослушивание завершено: его сообщение больше "
		"нельзя изменить.",
	},
	{
		"lng_oblivion_listen_error_share_later",
		"Трек ещё не отправлен в чат: Telegram не отвечает. Он "
		"отправится сам, как только появится связь, повторять "
		"отправку не нужно.",
	},
	{
		"lng_oblivion_listen_error_share_failed",
		"Не удалось отправить трек в чат. Можно попробовать ещё раз.",
	},
	{
		"lng_oblivion_listen_error_slowmode",
		"В этой группе включён медленный режим, поэтому трек и "
		"сообщение сеанса нельзя отправить подряд. Сначала отправьте "
		"трек в чат сами, а затем запустите совместное прослушивание.",
	},
	{
		"lng_oblivion_listen_toast_away",
		"Трека «{track}» нет в чате «{chat}». Слушатели на паузе, пока "
		"вы не вернётесь к треку из этого чата или не отправите туда "
		"этот.",
	},
	{ "lng_oblivion_listen_toast_stale", "Этот сеанс уже завершён." },
	{
		"lng_oblivion_listen_toast_hosting",
		"Вы ведёте своё совместное прослушивание. Сначала завершите "
		"его.",
	},
	{ "lng_oblivion_listen_toast_host", "Вы ведущий этого сеанса." },
	{
		"lng_oblivion_listen_toast_already",
		"Вы уже слушаете вместе с этим ведущим.",
	},
	{
		"lng_oblivion_listen_toast_ended",
		"Совместное прослушивание завершено.",
	},
	{
		"lng_oblivion_listen_toast_vanished",
		"Ведущий пропал из сети, совместное прослушивание завершено.",
	},
	{
		"lng_oblivion_listen_toast_left",
		"Вы вышли из совместного прослушивания.",
	},
	// Oblivion round 4: lottie masks.
	{ "lng_oblivion_lottie_mask_tool_select", "Выделение" },
	{ "lng_oblivion_lottie_mask_tool_pen", "Перо: точки контура" },
	{ "lng_oblivion_lottie_mask_mode", "Режим" },
	{ "lng_oblivion_lottie_mask_mode_none", "Не действует" },
	{ "lng_oblivion_lottie_mask_mode_add", "Сложение" },
	{ "lng_oblivion_lottie_mask_mode_subtract", "Вычитание" },
	{ "lng_oblivion_lottie_mask_mode_intersect", "Пересечение" },
	{ "lng_oblivion_lottie_mask_mode_lighten", "Осветление" },
	{ "lng_oblivion_lottie_mask_mode_darken", "Затемнение" },
	{ "lng_oblivion_lottie_mask_mode_difference", "Разница" },
	{
		"lng_oblivion_lottie_mask_mode_skipped",
		"Маски с таким режимом Telegram пропускает. Если других масок "
		"у слоя нет, слой не рисуется вовсе.",
	},
	{ "lng_oblivion_lottie_mask_prop_feather", "Растушёвка маски" },
	{ "lng_oblivion_lottie_mask_cmd_add_mask", "Новая маска" },
	{ "lng_oblivion_lottie_mask_cmd_change_mask", "Изменение маски" },
	{ "lng_oblivion_lottie_mask_cmd_matte", "Изменение маски по слою" },
	{ "lng_oblivion_lottie_mask_cmd_parent", "Смена родителя" },
	{ "lng_oblivion_lottie_mask_cmd_option", "Изменение параметра фигуры" },
	{ "lng_oblivion_lottie_mask_cmd_gradient", "Изменение градиента" },
	{ "lng_oblivion_lottie_mask_cmd_convert_paint", "Смена типа заливки" },
	{ "lng_oblivion_lottie_mask_cmd_dashes", "Изменение пунктира" },
	{ "lng_oblivion_lottie_mask_cmd_bake", "Скругление углов" },
	{ "lng_oblivion_lottie_mask_cmd_add_path", "Новый контур" },
	{ "lng_oblivion_lottie_mask_cmd_edit_path", "Изменение контура" },
	{ "lng_oblivion_lottie_mask_issue_masks", "Использует маски." },
	{
		"lng_oblivion_lottie_mask_issue_mattes",
		"Использует маски по слою — Telegram их рисует, но они "
		"замедляют стикер.",
	},
	{
		"lng_oblivion_lottie_mask_issue_broken_mattes",
		"У слоя с маской по слою нет слоя-маски над ним — Telegram "
		"такой слой не нарисует.",
	},
	{
		"lng_oblivion_lottie_mask_issue_matte_links",
		"Маска по слою ссылается не на соседний слой — Telegram возьмёт "
		"тот слой, что лежит прямо над ним.",
	},
	{
		"lng_oblivion_lottie_mask_issue_masks_off",
		"У слоя есть маски, но они выключены — Telegram их все "
		"пропустит.",
	},
	{
		"lng_oblivion_lottie_mask_issue_modes",
		"У некоторых масок режим, который Telegram пропускает: "
		"«Не действует», «Осветление» или «Затемнение».",
	},
	{
		"lng_oblivion_lottie_mask_issue_options",
		"Непрозрачность, растушёвку и расширение масок Telegram "
		"не учитывает — маска всегда сплошная, с чётким краем.",
	},
	{
		"lng_oblivion_lottie_mask_issue_inverted",
		"Некоторые маски инвертированы флажком, который Telegram "
		"не читает, — они работают как обычные.",
	},
	{
		"lng_oblivion_lottie_mask_issue_vertices",
		"В ключевых кадрах контура разное число точек — лишние "
		"Telegram отбросит.",
	},
	{
		"lng_oblivion_lottie_mask_issue_key_order",
		"Файл записан в порядке, который Telegram читает неправильно: "
		"часть фигур или поворотов слоёв пропадает.",
	},
	{
		"lng_oblivion_lottie_mask_issue_parents",
		"У некоторых слоёв родитель указан неверно — Telegram такую "
		"связь пропустит.",
	},
	{
		"lng_oblivion_lottie_mask_issue_hang",
		"В анимации есть значения, на которых Telegram зависает, "
		"когда рисует стикер.",
	},
	{ "lng_oblivion_lottie_mask_fix_masks_on", "Включить маски" },
	{ "lng_oblivion_lottie_mask_fix_modes", "Заменить режимы" },
	{ "lng_oblivion_lottie_mask_fix_options", "Сбросить" },
	{ "lng_oblivion_lottie_mask_fix_inverted", "Инвертировать режимом" },
	{ "lng_oblivion_lottie_mask_fix_key_order", "Исправить порядок" },
	{ "lng_oblivion_lottie_mask_fix_parents", "Убрать связи" },
	{
		"lng_oblivion_lottie_mask_fix_hang",
		"Сделать значения безопасными",
	},
	{ "lng_oblivion_lottie_mask_category_file", "Файл" },
	{
		"lng_oblivion_lottie_mask_category_forbidden",
		"Telegram не принимает в стикерах",
	},
	{
		"lng_oblivion_lottie_mask_category_not_rendered",
		"Telegram нарисует иначе",
	},
	{ "lng_oblivion_lottie_mask_category_advice", "Советы" },
	{
		"lng_oblivion_lottie_mask_note_rendered",
		"Это запрещено правилами Telegram для анимированных стикеров, "
		"даже если рисуется правильно.",
	},
	{
		"lng_oblivion_lottie_mask_note_ignored",
		"Telegram это ещё и не нарисует.",
	},
	{
		"lng_oblivion_lottie_mask_note_fix_changes",
		"Исправление изменит вид стикера в Telegram.",
	},
	{
		"lng_oblivion_lottie_mask_check_title",
		"Перед тем как сделать стикер",
	},
	{
		"lng_oblivion_lottie_mask_check_pack_errors",
		"Добавить такой стикер в набор пока нельзя. Сначала исправьте "
		"проблемы ниже.",
	},
	{
		"lng_oblivion_lottie_mask_check_pack_warnings",
		"Перед добавлением в набор посмотрите, что Telegram "
		"не принимает или нарисует иначе.",
	},
	{
		"lng_oblivion_lottie_mask_check_tgs_errors",
		"Файл .tgs — это стикер Telegram, а такой стикер Telegram "
		"не примет.",
	},
	{
		"lng_oblivion_lottie_mask_check_tgs_warnings",
		"Файл .tgs — это стикер Telegram. Вот что Telegram "
		"не принимает или нарисует иначе.",
	},
	{
		"lng_oblivion_lottie_mask_check_scope",
		"Это касается только стикеров Telegram (.tgs). Экспорт в JSON, "
		"видео и GIF сохраняет всё как есть.",
	},
	{ "lng_oblivion_lottie_mask_check_add", "Добавить в набор" },
	{ "lng_oblivion_lottie_mask_check_add_anyway", "Всё равно добавить" },
	{
		"lng_oblivion_lottie_mask_check_save_anyway",
		"Всё равно сохранить",
	},
	{ "lng_oblivion_lottie_mask_check_continue", "Продолжить" },
	{
		"lng_oblivion_lottie_mask_check_continue_anyway",
		"Всё равно продолжить",
	},
	{ "lng_oblivion_lottie_mask_links", "Связи" },
	{ "lng_oblivion_lottie_mask_parent", "Родитель" },
	{ "lng_oblivion_lottie_mask_parent_none", "Нет" },
	{ "lng_oblivion_lottie_mask_matte", "Маска по слою" },
	{ "lng_oblivion_lottie_mask_matte_alpha", "Альфа" },
	{ "lng_oblivion_lottie_mask_matte_alpha_inv", "Альфа (инв.)" },
	{ "lng_oblivion_lottie_mask_matte_luma", "Яркость" },
	{ "lng_oblivion_lottie_mask_matte_luma_inv", "Яркость (инв.)" },
	{
		"lng_oblivion_lottie_mask_matte_alpha_about",
		"Этот слой виден только там, где слой-маска непрозрачен.",
	},
	{
		"lng_oblivion_lottie_mask_matte_alpha_inv_about",
		"Этот слой виден только там, где слой-маска прозрачен.",
	},
	{
		"lng_oblivion_lottie_mask_matte_luma_about",
		"Этот слой виден только там, где слой-маска светлый.",
	},
	{
		"lng_oblivion_lottie_mask_matte_luma_inv_about",
		"Этот слой виден только там, где слой-маска тёмный.",
	},
	{ "lng_oblivion_lottie_mask_matte_layer", "Слой-маска" },
	{
		"lng_oblivion_lottie_mask_matte_for",
		"Служит маской для слоя «{name}»",
	},
	{
		"lng_oblivion_lottie_mask_matte_hint",
		"Маска по слою показывает этот слой только там, где есть "
		"другой слой. «Альфа» — где тот непрозрачен, «Яркость» — где "
		"он светлый, «инв.» — наоборот.",
	},
	{
		"lng_oblivion_lottie_mask_matte_about",
		"Слой-маска держится прямо над этим слоем и сам не рисуется. "
		"Telegram такие маски рисует, но правила для анимированных "
		"стикеров их не разрешают, и они замедляют стикер.",
	},
	{
		"lng_oblivion_lottie_mask_matte_broken",
		"Над слоем нет слоя-маски, поэтому он не рисуется. Выберите "
		"слой-маску.",
	},
	{
		"lng_oblivion_lottie_mask_matte_no_layer",
		"Над этим слоем нет слоя, который можно сделать его маской.",
	},
	{
		"lng_oblivion_lottie_mask_matte_bad_layer",
		"Этот слой нельзя сделать маской: он сам под маской или уже "
		"служит маской другому слою.",
	},
	{ "lng_oblivion_lottie_mask_masks", "Маски" },
	{ "lng_oblivion_lottie_mask_add", "Добавить маску" },
	{ "lng_oblivion_lottie_mask_add_to", "Маска для слоя «{name}»" },
	{ "lng_oblivion_lottie_mask_enable", "Включить маски" },
	{
		"lng_oblivion_lottie_mask_masks_hint",
		"Маска обрезает слой по контуру. Новая маска — прямоугольник "
		"вокруг слоя, его точки двигаются пером.",
	},
	{
		"lng_oblivion_lottie_mask_masks_list_hint",
		"Маски действуют по очереди, начиная с первой. Нажмите "
		"на название, чтобы открыть маску.",
	},
	{
		"lng_oblivion_lottie_mask_masks_off",
		"Маски этого слоя выключены и ничего не обрезают.",
	},
	{
		"lng_oblivion_lottie_mask_telegram_masks",
		"Telegram не принимает маски в анимированных стикерах. "
		"В экспорте в видео, GIF и JSON они работают.",
	},
	{
		"lng_oblivion_lottie_mask_telegram_options",
		"Telegram рисует только контур маски: непрозрачность, "
		"расширение и растушёвка не учитываются.",
	},
	{ "lng_oblivion_lottie_mask_invert", "Инвертировать" },
	{
		"lng_oblivion_lottie_mask_invert_failed",
		"У этого режима нет обратного. Сначала выберите «Сложение» "
		"или «Вычитание».",
	},
	{
		"lng_oblivion_lottie_mask_inverted_flag",
		"У маски стоит флажок инверсии, который Telegram не читает. "
		"«Инвертировать» делает то же самое режимом.",
	},
	{
		"lng_oblivion_lottie_mask_gradient_hint",
		"Щёлкните под полосой, чтобы добавить цвет, над полосой — "
		"прозрачность. Метки можно двигать, а чтобы убрать метку, "
		"утащите её в сторону от полосы.",
	},
	{
		"lng_oblivion_lottie_mask_gradient_no_stop",
		"Нажмите на метку, чтобы изменить её",
	},
	{ "lng_oblivion_lottie_mask_gradient_remove", "Убрать" },
	{ "lng_oblivion_lottie_mask_gradient_stop_color", "Цвет" },
	{ "lng_oblivion_lottie_mask_gradient_stop_opacity", "Непрозрачность" },
	{ "lng_oblivion_lottie_mask_gradient_stop_position", "Позиция" },
	{ "lng_oblivion_lottie_mask_to_gradient", "Сделать градиентом" },
	{ "lng_oblivion_lottie_mask_to_solid", "Сделать сплошным цветом" },
	{
		"lng_oblivion_lottie_mask_telegram_gradient_stroke",
		"Telegram не принимает градиентные обводки в анимированных "
		"стикерах.",
	},
	{ "lng_oblivion_lottie_mask_dashes", "Пунктир" },
	{ "lng_oblivion_lottie_mask_dashes_pattern", "Узор" },
	{ "lng_oblivion_lottie_mask_dashes_none", "Без пунктира" },
	{ "lng_oblivion_lottie_mask_dashes_pairs#one", "{count} штрих" },
	{ "lng_oblivion_lottie_mask_dashes_pairs#few", "{count} штриха" },
	{ "lng_oblivion_lottie_mask_dashes_pairs#many", "{count} штрихов" },
	{ "lng_oblivion_lottie_mask_dashes_pairs#other", "{count} штриха" },
	{
		"lng_oblivion_lottie_mask_trim_hint",
		"Показывает только часть контуров, лежащих выше: от начала "
		"до конца, в процентах их длины.",
	},
	{
		"lng_oblivion_lottie_mask_repeater_hint",
		"Повторяет всё, что лежит выше в группе: каждая копия "
		"сдвигается, поворачивается и масштабируется чуть сильнее "
		"предыдущей.",
	},
	{
		"lng_oblivion_lottie_mask_telegram_repeater",
		"Telegram не принимает повторители в анимированных стикерах.",
	},
	{
		"lng_oblivion_lottie_mask_bake",
		"Превратить в настоящие скругления",
	},
	{
		"lng_oblivion_lottie_mask_telegram_round",
		"Этот модификатор Telegram не рисует. Превратите его "
		"в настоящие скругления контуров.",
	},
	{
		"lng_oblivion_lottie_mask_telegram_modifier",
		"Этот модификатор Telegram не рисует.",
	},
	{
		"lng_oblivion_lottie_mask_telegram_star",
		"Telegram не принимает звёзды и многоугольники "
		"в анимированных стикерах.",
	},
	{
		"lng_oblivion_lottie_mask_convert_hint",
		"У прямоугольника и эллипса нет точек. Преобразуйте фигуру "
		"в контур, чтобы править точки пером.",
	},
	{
		"lng_oblivion_lottie_mask_path_hint",
		"Направление важно для обрезки контуров: она идёт от первой "
		"точки.",
	},
	{ "lng_oblivion_lottie_mask_pen_edit", "Править точки" },
	{ "lng_oblivion_lottie_mask_pen_convert", "Преобразовать в контур" },
	{ "lng_oblivion_lottie_mask_pen_delete_point", "Удалить точку" },
	{ "lng_oblivion_lottie_mask_pen_make_smooth", "Сделать гладкой" },
	{ "lng_oblivion_lottie_mask_pen_make_corner", "Сделать угловой" },
	{ "lng_oblivion_lottie_mask_pen_add_point", "Добавить точку здесь" },
	{ "lng_oblivion_lottie_mask_pen_close", "Замкнуть контур" },
	{ "lng_oblivion_lottie_mask_pen_open", "Разомкнуть контур" },
	{ "lng_oblivion_lottie_mask_pen_reverse", "Развернуть направление" },
	{
		"lng_oblivion_lottie_mask_pen_min_points",
		"В контуре должно остаться хотя бы две точки.",
	},
	{
		"lng_oblivion_lottie_mask_pen_hint_select",
		"Выберите контур или маску, чтобы править точки",
	},
	{
		"lng_oblivion_lottie_mask_pen_hint_pick",
		"Нажмите на обведённый контур или маску",
	},
	{
		"lng_oblivion_lottie_mask_pen_hint_new",
		"Щёлкните в двух местах, чтобы начать новый контур",
	},
	{
		"lng_oblivion_lottie_mask_pen_hint_pick_or_new",
		"Нажмите на обведённый контур или щёлкните в двух местах "
		"для нового",
	},
	{
		"lng_oblivion_lottie_mask_pen_hint_second",
		"Щёлкните там, где будет вторая точка",
	},
	{
		"lng_oblivion_lottie_mask_pen_hint_edit",
		"Тяните точки и рычаги; щелчок по линии добавляет точку",
	},
	{
		"lng_oblivion_lottie_mask_pen_hint_continue",
		"Щёлкайте, чтобы продолжить контур; нажмите на другой конец, "
		"чтобы замкнуть",
	},
	{
		"lng_oblivion_lottie_mask_pen_hint_shape",
		"У этой фигуры нет точек: преобразуйте её в контур "
		"(правый щелчок)",
	},
	// Oblivion round 4: lottie graph.
	{ "lng_oblivion_lottie_graph_toggle", "График плавности" },
	{ "lng_oblivion_lottie_graph_show", "График плавности" },
	{ "lng_oblivion_lottie_graph_value", "Значение" },
	{ "lng_oblivion_lottie_graph_speed", "Скорость" },
	{ "lng_oblivion_lottie_graph_path", "По траектории" },
	{ "lng_oblivion_lottie_graph_title", "{property} · {name}" },
	{
		"lng_oblivion_lottie_graph_empty",
		"Раскройте слой слева и нажмите на название свойства "
		"с ключевыми кадрами — здесь появится его график.",
	},
	{ "lng_oblivion_lottie_graph_key", "Ключ {value}" },
	{ "lng_oblivion_lottie_graph_axis_value", "Значение" },
	{ "lng_oblivion_lottie_graph_axis_progress", "Путь между ключами" },
	{ "lng_oblivion_lottie_graph_axis_speed", "Единиц в секунду" },
	{
		"lng_oblivion_lottie_graph_axis_speed_progress",
		"Ключей в секунду",
	},
	{
		"lng_oblivion_lottie_graph_value_about",
		"Как значение меняется со временем. Тяните круглые рычаги, "
		"чтобы изменить плавность.",
	},
	{
		"lng_oblivion_lottie_graph_value_progress_about",
		"Это значение не число, поэтому график показывает путь "
		"от одного ключевого кадра к следующему.",
	},
	{
		"lng_oblivion_lottie_graph_speed_about",
		"Как быстро меняется значение. Рычаг задаёт скорость у своего "
		"ключевого кадра и то, как долго она держится.",
	},
	{
		"lng_oblivion_lottie_graph_path_about",
		"Значение движется по кривой и не может выйти за ключевые "
		"кадры. Выключите, чтобы плавность могла «перелетать» значения.",
	},
	// Oblivion round 4 end.
};

// Each round 5 feature appends its rows after its own marker comment,
// the markers are the same as in lang.strings. Key prefixes: cloud
// lng_oblivion_cloud_, rooms lng_oblivion_room_, room music
// lng_oblivion_rmusic_, room video lng_oblivion_rvideo_, room canvas
// lng_oblivion_rcanvas_, room extras lng_oblivion_rextra_, social
// lng_oblivion_social_, share lng_oblivion_share_, sync lng_oblivion_sync_,
// update lng_oblivion_update_, deleted search lng_oblivion_dsearch_, send
// online lng_oblivion_sendonline_, stats export lng_oblivion_statsexp_.
const LangOverride kRound5[] = {
	// Oblivion round 5: cloud.
	{ "lng_oblivion_cloud_title", "Oblivion Cloud" },
	{ "lng_oblivion_cloud_soon", "Этот раздел пока не готов." },
	{
		"lng_oblivion_cloud_consent_about",
		"Комнаты, общие плейлисты, значок и всё, что вы делаете вместе "
		"с другими, работает через Oblivion Cloud. Это сервер разработчика "
		"Oblivion, а не часть Telegram.",
	},
	{ "lng_oblivion_cloud_consent_sent_title", "Что отправляется" },
	{
		"lng_oblivion_cloud_consent_sent",
		// Every line is a row of the list in the consent box.
		"Числовой ID аккаунта Telegram {name}\n"
		"Имя и картинка, которые вы сами выберете для Oblivion\n"
		"Название этого устройства\n"
		"То, что вы сами делаете в облаке: комнаты, плейлисты и наборы, "
		"которыми делитесь\n"
		"Пока Oblivion Cloud включён, сервер видит ваш IP-адрес и то, что "
		"приложение запущено",
	},
	{ "lng_oblivion_cloud_consent_not_title", "Что не отправляется" },
	{
		"lng_oblivion_cloud_consent_not",
		"Ваши сообщения и чаты\n"
		"Список контактов и номер телефона\n"
		"Пароль и ключи входа в Telegram",
	},
	{
		"lng_oblivion_cloud_consent_off",
		"Отключиться или удалить все свои данные с сервера можно в любой "
		"момент: Настройки > Oblivion > Oblivion Cloud. Пока вы не "
		"согласились, ничего не отправляется.",
	},
	{ "lng_oblivion_cloud_consent_name", "Имя в Oblivion" },
	{
		"lng_oblivion_cloud_consent_name_about",
		"Это имя видят другие участники комнат. Можно оставить пустым.",
	},
	{ "lng_oblivion_cloud_consent_agree", "Подключиться" },
	{
		"lng_oblivion_cloud_enable_again",
		"Oblivion Cloud отключён для этого аккаунта. Включить снова?",
	},
	{ "lng_oblivion_cloud_enable", "Включить" },
	{ "lng_oblivion_cloud_link_title", "Привязать другое устройство" },
	{
		"lng_oblivion_cloud_link_about",
		"Откройте Oblivion на другом устройстве с этим же аккаунтом "
		"Telegram, зайдите в Настройки > Oblivion > Oblivion Cloud "
		"и введите этот код.",
	},
	{ "lng_oblivion_cloud_link_loading", "Получаем код…" },
	{
		"lng_oblivion_cloud_link_expires",
		"Код одноразовый и действует ещё {time}.",
	},
	{
		"lng_oblivion_cloud_link_expired",
		"Код истёк. Нажмите «Новый код».",
	},
	{ "lng_oblivion_cloud_off_title", "Отключить Oblivion Cloud?" },
	{ "lng_oblivion_cloud_delete_title", "Удалить данные с сервера?" },
	{ "lng_oblivion_cloud_link_copy", "Скопировать" },
	{ "lng_oblivion_cloud_link_copied", "Код скопирован." },
	{ "lng_oblivion_cloud_link_new", "Новый код" },
	{
		"lng_oblivion_cloud_link_warning",
		"Вводите код только на своих устройствах: тот, кто его введёт, "
		"получит доступ к вашему аккаунту Oblivion Cloud.",
	},
	{ "lng_oblivion_cloud_code_title", "Введите код" },
	{
		"lng_oblivion_cloud_code_about",
		"Этот аккаунт Telegram уже привязан к Oblivion Cloud на другом "
		"устройстве. Откройте там Настройки > Oblivion > Oblivion Cloud > "
		"«Привязать другое устройство» и введите показанный код.",
	},
	{ "lng_oblivion_cloud_code_placeholder", "Код" },
	{ "lng_oblivion_cloud_code_submit", "Привязать" },
	{
		"lng_oblivion_cloud_code_wrong",
		"Код не подошёл или устарел. Получите новый и попробуйте ещё раз.",
	},
	{
		"lng_oblivion_cloud_code_devices",
		"К этому аккаунту уже привязано слишком много устройств.",
	},
	{
		"lng_oblivion_cloud_code_done",
		"Устройство привязано к Oblivion Cloud.",
	},
	{
		"lng_oblivion_cloud_code_lost",
		"Если устройства с Oblivion больше нет, попросите у разработчика "
		"Oblivion одноразовый код.",
	},
	{
		"lng_oblivion_cloud_code_reserved_about",
		"Данные этого аккаунта Telegram хранятся в Oblivion Cloud, но к "
		"нему больше не привязано ни одного устройства: так бывает после "
		"выхода из Telegram. Попросите у разработчика Oblivion одноразовый "
		"код и введите его здесь.",
	},
	{
		"lng_oblivion_cloud_error_off",
		"Oblivion Cloud не подключён для этого аккаунта.",
	},
	{
		"lng_oblivion_cloud_error_network",
		"Нет связи с сервером Oblivion Cloud. Попробуйте позже.",
	},
	{
		"lng_oblivion_cloud_error_tls",
		"Не удалось проверить подлинность сервера Oblivion Cloud, поэтому "
		"ничего не отправлено.",
	},
	{
		"lng_oblivion_cloud_error_file",
		"Не удалось прочитать или сохранить файл.",
	},
	{
		"lng_oblivion_cloud_error_server",
		"Ошибка сервера Oblivion Cloud. Попробуйте позже.",
	},
	{
		"lng_oblivion_cloud_error_rate",
		"Слишком много запросов. Подождите немного и попробуйте снова.",
	},
	{
		"lng_oblivion_cloud_error_upgrade",
		"Эта версия Oblivion устарела для Oblivion Cloud. Обновите "
		"приложение.",
	},
	{
		"lng_oblivion_cloud_error_banned",
		"Доступ к Oblivion Cloud для этого аккаунта закрыт.",
	},
	{
		"lng_oblivion_cloud_error_bound",
		"Этот аккаунт уже привязан к Oblivion Cloud на другом устройстве.",
	},
	{ "lng_oblivion_cloud_error_forbidden", "У вас нет прав на это действие." },
	{
		"lng_oblivion_cloud_error_not_found",
		"Не найдено. Возможно, это уже удалено.",
	},
	{ "lng_oblivion_cloud_error_too_large", "Файл слишком большой." },
	{
		"lng_oblivion_cloud_error_full",
		"Сервер переполнен. Попробуйте позже.",
	},
	{ "lng_oblivion_cloud_error_limit", "Достигнут предел Oblivion Cloud." },
	{
		"lng_oblivion_cloud_error_test",
		"Oblivion Cloud не работает с аккаунтами тестового сервера "
		"Telegram.",
	},
	{ "lng_oblivion_cloud_status_none", "Не подключено" },
	{
		"lng_oblivion_cloud_status_none_about",
		"Для этого аккаунта на сервер ничего не отправляется.",
	},
	{ "lng_oblivion_cloud_status_off", "Отключено" },
	{
		"lng_oblivion_cloud_status_off_about",
		"Для этого аккаунта Oblivion Cloud выключен, на сервер ничего не "
		"отправляется. Включить снова можно в любой момент.",
	},
	{ "lng_oblivion_cloud_status_link", "Нужен код" },
	{
		"lng_oblivion_cloud_status_link_about",
		"Этот аккаунт Telegram уже привязан к Oblivion Cloud на другом "
		"устройстве. Получите там код и нажмите «Ввести код с другого "
		"устройства».",
	},
	{
		"lng_oblivion_cloud_status_reserved_about",
		"Данные этого аккаунта хранятся в Oblivion Cloud, но к нему больше "
		"не привязано ни одного устройства: так бывает после выхода из "
		"Telegram. Попросите у разработчика Oblivion одноразовый код и "
		"нажмите «Ввести код с другого устройства».",
	},
	{ "lng_oblivion_cloud_status_connecting", "Подключение…" },
	{
		"lng_oblivion_cloud_status_connecting_about",
		"Связываемся с сервером Oblivion Cloud.",
	},
	{ "lng_oblivion_cloud_status_online", "Подключено" },
	{
		"lng_oblivion_cloud_status_online_as",
		"Другие участники комнат видят вас под именем «{name}».",
	},
	{
		"lng_oblivion_cloud_status_online_noname",
		"Имя в Oblivion пока не задано.",
	},
	{ "lng_oblivion_cloud_status_offline", "Нет связи с сервером" },
	{
		"lng_oblivion_cloud_status_offline_about",
		"Oblivion сам пробует подключиться снова.",
	},
	{ "lng_oblivion_cloud_status_upgrade", "Нужно обновление" },
	{ "lng_oblivion_cloud_status_banned", "Доступ закрыт" },
	{ "lng_oblivion_cloud_on_done", "Oblivion Cloud подключён." },
	{
		"lng_oblivion_cloud_off_sure",
		"Oblivion Cloud будет отключён для этого аккаунта. Ваши данные "
		"останутся на сервере, а ключ этого устройства — на этом "
		"компьютере, чтобы можно было подключиться снова.\n\n"
		"Чтобы стереть всё, выберите «Удалить мои данные с сервера».",
	},
	{ "lng_oblivion_cloud_off_confirm", "Отключить" },
	{
		"lng_oblivion_cloud_off_done",
		"Oblivion Cloud отключён для этого аккаунта.",
	},
	{
		"lng_oblivion_cloud_delete_sure",
		"Сразу будет стёрто всё, что Oblivion Cloud хранит об этом "
		"аккаунте: имя и картинка, профиль, ваши комнаты, общие плейлисты "
		"и наборы, копия настроек и все привязанные устройства.\n\n"
		"Отменить это нельзя.",
	},
	{ "lng_oblivion_cloud_delete_confirm", "Удалить" },
	{
		"lng_oblivion_cloud_delete_done",
		"Ваши данные удалены с сервера. Oblivion Cloud для этого аккаунта "
		"отключён.",
	},
	{
		"lng_oblivion_cloud_delete_need",
		"Чтобы удалить данные, Oblivion Cloud должен быть подключён для "
		"этого аккаунта: серверу нужно убедиться, что это действительно "
		"вы.",
	},
	{ "lng_oblivion_cloud_section", "Oblivion Cloud" },
	{ "lng_oblivion_cloud_toggle", "Oblivion Cloud для этого аккаунта" },
	{ "lng_oblivion_cloud_link_device", "Привязать другое устройство" },
	{ "lng_oblivion_cloud_enter_code", "Ввести код с другого устройства" },
	{ "lng_oblivion_cloud_delete", "Удалить мои данные с сервера" },
	{ "lng_oblivion_cloud_links", "Открывать ссылки Oblivion в приложении" },
	{
		"lng_oblivion_cloud_section_about",
		"Oblivion Cloud — сервер разработчика Oblivion для комнат, общих "
		"плейлистов и наборов, значка и профилей. Включается отдельно для "
		"каждого аккаунта Telegram и только после вашего согласия. Выход "
		"из Telegram на этом компьютере только отвязывает этот компьютер "
		"от Oblivion Cloud: ваши данные остаются на сервере, пока вы сами "
		"их не удалите, а чтобы подключиться снова, понадобится код.",
	},
	// Oblivion round 5: rooms.
	{ "lng_oblivion_room_section", "Вместе" },
	{ "lng_oblivion_room_settings", "Комнаты" },
	{ "lng_oblivion_room_settings_enabled", "Комнаты в меню и по ссылкам" },
	{
		"lng_oblivion_room_settings_about",
		"Комната — общее место для музыки, видео, рисования и чата "
		"с друзьями по ссылке. Создаётся и открывается только по вашему "
		"клику.",
	},
	{ "lng_oblivion_room_title_default", "Комната" },
	{ "lng_oblivion_room_window_title", "{title} — комната Oblivion" },
	{ "lng_oblivion_room_members_count#one", "{count} участник" },
	{ "lng_oblivion_room_members_count#few", "{count} участника" },
	{ "lng_oblivion_room_members_count#many", "{count} участников" },
	{ "lng_oblivion_room_members_count#other", "{count} участника" },
	{ "lng_oblivion_room_header_status", "{members} · {online} в сети" },
	{ "lng_oblivion_room_connecting", "Нет связи, переподключаемся…" },
	{ "lng_oblivion_room_copy_link", "Скопировать ссылку" },
	{ "lng_oblivion_room_copy_code", "Скопировать код" },
	{ "lng_oblivion_room_link_copied", "Ссылка на комнату скопирована." },
	{ "lng_oblivion_room_code_copied", "Код комнаты скопирован." },
	{ "lng_oblivion_room_rename", "Переименовать комнату" },
	{ "lng_oblivion_room_rename_button", "Сохранить" },
	{ "lng_oblivion_room_name_placeholder", "Название комнаты" },
	{ "lng_oblivion_room_leave", "Выйти из комнаты" },
	{
		"lng_oblivion_room_leave_sure",
		"Выйти из комнаты? Вернуться можно по той же ссылке.",
	},
	{
		"lng_oblivion_room_leave_sure_owner",
		"Вы владелец: после вашего выхода комната перейдёт к участнику, "
		"который вошёл раньше других.",
	},
	{ "lng_oblivion_room_leave_button", "Выйти" },
	{ "lng_oblivion_room_close_all", "Закрыть комнату для всех" },
	{
		"lng_oblivion_room_close_all_sure",
		"Закрыть комнату для всех участников? Очередь, чат и холст "
		"будут удалены.",
	},
	{ "lng_oblivion_room_close_all_button", "Закрыть комнату" },
	{ "lng_oblivion_room_tab_chat", "Чат" },
	{ "lng_oblivion_room_tab_members", "Участники" },
	{ "lng_oblivion_room_gone_kicked", "Вас исключили из комнаты" },
	{ "lng_oblivion_room_gone_banned", "Вас заблокировали в этой комнате" },
	{ "lng_oblivion_room_gone_closed", "Владелец закрыл комнату" },
	{
		"lng_oblivion_room_gone_idle",
		"Комната закрыта: в ней давно никого не было",
	},
	{ "lng_oblivion_room_gone_admin", "Комнату закрыл администратор" },
	{
		"lng_oblivion_room_gone_rejected",
		"Вы больше не участник этой комнаты",
	},
	{ "lng_oblivion_room_gone_button", "Закрыть окно" },
	{ "lng_oblivion_room_chat_placeholder", "Сообщение в чат комнаты…" },
	{ "lng_oblivion_room_chat_empty", "Сообщений пока нет" },
	{
		"lng_oblivion_room_chat_empty_about",
		"Чат живёт, пока открыта комната: его сообщения не попадают "
		"в Telegram.",
	},
	{ "lng_oblivion_room_chat_loading", "Загрузка…" },
	{
		"lng_oblivion_room_chat_no_right",
		"Писать в чат этой комнаты вам не разрешено.",
	},
	{ "lng_oblivion_room_chat_copy", "Копировать текст" },
	{ "lng_oblivion_room_chat_delete", "Удалить сообщение" },
	{ "lng_oblivion_room_members_defaults", "Права новых участников" },
	{
		"lng_oblivion_room_members_defaults_about",
		"Нажмите на право, чтобы включить или выключить его для тех, кто "
		"войдёт позже. Кнопки выше меняют права сразу всем.",
	},
	{ "lng_oblivion_room_preset_all", "Все управляют" },
	{ "lng_oblivion_room_preset_owner", "Только я" },
	{
		"lng_oblivion_room_preset_sure_all",
		"Дать всем участникам все права: плеер, очередь, добавление, "
		"холст, чат и приглашения?",
	},
	{
		"lng_oblivion_room_preset_sure_owner",
		"Оставить плеер и очередь только себе? Рисовать и писать в чат "
		"участники смогут по-прежнему.",
	},
	{ "lng_oblivion_room_preset_apply", "Применить" },
	{ "lng_oblivion_room_members_list", "Участники" },
	{ "lng_oblivion_room_members_you", "вы" },
	{ "lng_oblivion_room_members_owner", "владелец" },
	{ "lng_oblivion_room_members_online", "в сети" },
	{ "lng_oblivion_room_members_offline", "не в сети" },
	{ "lng_oblivion_room_members_loading", "загружает трек, {percent}%" },
	{ "lng_oblivion_room_members_all_rights", "все права" },
	{ "lng_oblivion_room_members_my_rights", "Ваши права в этой комнате" },
	{ "lng_oblivion_room_members_banned", "Заблокированы" },
	{ "lng_oblivion_room_members_unban", "Разблокировать" },
	{ "lng_oblivion_room_member_transfer", "Сделать владельцем" },
	{
		"lng_oblivion_room_member_transfer_sure",
		"Сделать {name} владельцем комнаты? Вы останетесь участником.",
	},
	{ "lng_oblivion_room_member_kick", "Исключить" },
	{
		"lng_oblivion_room_member_kick_sure",
		"Исключить {name} из комнаты? По ссылке можно будет войти снова.",
	},
	{ "lng_oblivion_room_member_ban", "Исключить и заблокировать" },
	{
		"lng_oblivion_room_member_ban_sure",
		"Исключить {name} и больше не пускать по ссылке?",
	},
	{ "lng_oblivion_room_right_control", "Плеер" },
	{ "lng_oblivion_room_right_queue", "Очередь" },
	{ "lng_oblivion_room_right_add", "Добавление" },
	{ "lng_oblivion_room_right_draw", "Холст" },
	{ "lng_oblivion_room_right_chat", "Чат" },
	{ "lng_oblivion_room_right_invite", "Приглашения" },
	{ "lng_oblivion_room_list_loading", "Загрузка…" },
	{
		"lng_oblivion_room_list_empty",
		"Вы пока не состоите ни в одной комнате. Создайте свою или "
		"войдите по ссылке друга.",
	},
	{ "lng_oblivion_room_list_yours", "Ваша комната" },
	{ "lng_oblivion_room_list_online", "{online} в сети" },
	{ "lng_oblivion_room_list_create", "Создать комнату" },
	{ "lng_oblivion_room_list_join_placeholder", "Ссылка или код комнаты" },
	{
		"lng_oblivion_room_list_bad_code",
		"Это не похоже на ссылку или код комнаты.",
	},
	{ "lng_oblivion_room_create_title", "Новая комната" },
	{
		"lng_oblivion_room_create_about",
		"Комната — место, где вы вместе с друзьями слушаете музыку, "
		"смотрите видео, рисуете и переписываетесь. Позовите их ссылкой.",
	},
	{ "lng_oblivion_room_create_button", "Создать" },
	{ "lng_oblivion_room_create_default", "Комната: {name}" },
	{ "lng_oblivion_room_join_title", "Войти в комнату?" },
	{ "lng_oblivion_room_join_owner", "Владелец: {name}" },
	{ "lng_oblivion_room_join_members", "Участников: {members} из {max}" },
	{
		"lng_oblivion_room_join_about",
		"Участники комнаты увидят имя, которое вы указали в Oblivion "
		"Cloud. Музыка и видео включаются у всех одновременно.",
	},
	{ "lng_oblivion_room_join_button", "Войти" },
	{ "lng_oblivion_room_join_full", "В комнате нет свободных мест." },
	{
		"lng_oblivion_room_join_banned",
		"Владелец закрыл вам вход в эту комнату.",
	},
	{
		"lng_oblivion_room_join_invite",
		"В эту комнату входят только по приглашению владельца.",
	},
	{
		"lng_oblivion_room_join_not_found",
		"Такой комнаты нет: возможно, её уже закрыли.",
	},
	{
		"lng_oblivion_room_error_limit_owned",
		"У вас уже три свои комнаты. Закройте одну, чтобы создать новую.",
	},
	{
		"lng_oblivion_room_error_limit_joined",
		"Вы состоите в слишком многих комнатах. Выйдите из одной из них.",
	},
	{
		"lng_oblivion_room_error_queue_full",
		"В очереди комнаты больше нет места.",
	},
	{
		"lng_oblivion_room_error_quota",
		"Вы отправили в комнаты слишком много файлов. Уберите часть "
		"треков из очереди.",
	},
	{
		"lng_oblivion_room_error_right",
		"У вас нет на это права в этой комнате.",
	},
	{
		"lng_oblivion_room_error_unavailable",
		"Комнаты сейчас недоступны на сервере.",
	},
	{ "lng_oblivion_room_error_empty_queue", "В очереди пока пусто." },
	{
		"lng_oblivion_room_file_failed",
		"Этот файл не получится проиграть: его длительность неизвестна.",
	},
	{
		"lng_oblivion_room_file_too_big",
		"Файл слишком большой: в комнату можно отправить до {size} МБ.",
	},
	{ "lng_oblivion_room_untitled", "Без названия" },
	{ "lng_oblivion_room_open_as_room", "В комнату" },
	// Oblivion round 5: room music.
	{ "lng_oblivion_rmusic_tab", "Музыка" },
	{ "lng_oblivion_rmusic_queue", "Очередь" },
	{ "lng_oblivion_rmusic_add", "Добавить" },
	{ "lng_oblivion_rmusic_add_player", "Трек из плеера Telegram" },
	{ "lng_oblivion_rmusic_add_playlist", "Из плейлиста Oblivion…" },
	{ "lng_oblivion_rmusic_add_files", "Файлы с компьютера…" },
	{ "lng_oblivion_rmusic_add_files_title", "Выберите музыку" },
	{ "lng_oblivion_rmusic_add_to_room", "Добавить в комнату" },
	{ "lng_oblivion_rmusic_added_by", "Ставит {name}" },
	{ "lng_oblivion_rmusic_added_toast", "Трек отправляется в комнату." },
	{
		"lng_oblivion_rmusic_batch_toast#one",
		"{count} трек отправляется в комнату.",
	},
	{
		"lng_oblivion_rmusic_batch_toast#few",
		"{count} трека отправляются в комнату.",
	},
	{
		"lng_oblivion_rmusic_batch_toast#many",
		"{count} треков отправляются в комнату.",
	},
	{
		"lng_oblivion_rmusic_batch_toast#other",
		"{count} трека отправляются в комнату.",
	},
	{ "lng_oblivion_rmusic_clear", "Очистить очередь" },
	{
		"lng_oblivion_rmusic_clear_sure",
		"Убрать все треки из очереди комнаты?",
	},
	{
		"lng_oblivion_rmusic_downloading",
		"Трек скачивается из Telegram, затем отправится в комнату.",
	},
	{
		"lng_oblivion_rmusic_download_failed",
		"Не удалось скачать трек из Telegram.",
	},
	{
		"lng_oblivion_rmusic_wait",
		"Подождите немного: предыдущие треки ещё скачиваются.",
	},
	{ "lng_oblivion_rmusic_empty", "В очереди пока пусто" },
	{
		"lng_oblivion_rmusic_empty_about",
		"Добавьте трек из плеера Telegram, из плейлиста Oblivion или "
		"файл с компьютера — он заиграет у всех в комнате одновременно.",
	},
	{
		"lng_oblivion_rmusic_empty_no_right",
		"Музыку в этой комнате добавляют другие участники.",
	},
	{
		"lng_oblivion_rmusic_no_add",
		"У вас нет права добавлять музыку в этой комнате.",
	},
	{
		"lng_oblivion_rmusic_no_control",
		"У вас нет права управлять плеером в этой комнате. Попросите "
		"его у владельца.",
	},
	{ "lng_oblivion_rmusic_nothing", "Ничего не играет" },
	{
		"lng_oblivion_rmusic_player_empty",
		"В плеере Telegram сейчас нет трека.",
	},
	{
		"lng_oblivion_rmusic_protected",
		"Этот трек из защищённого чата, его нельзя отправить в комнату.",
	},
	{ "lng_oblivion_rmusic_playlists_title", "Плейлисты Oblivion" },
	{
		"lng_oblivion_rmusic_playlists_empty",
		"У вас пока нет плейлистов Oblivion.",
	},
	{ "lng_oblivion_rmusic_playlist_add_all", "Добавить все" },
	{
		"lng_oblivion_rmusic_playlist_empty",
		"В этом плейлисте пока нет треков.",
	},
	{ "lng_oblivion_rmusic_playlist_wait", "загружается…" },
	{ "lng_oblivion_rmusic_playlist_gone", "недоступен" },
	{ "lng_oblivion_rmusic_preparing", "Подготовка…" },
	{ "lng_oblivion_rmusic_uploading", "Отправка в комнату… {percent}%" },
	{
		"lng_oblivion_rmusic_too_many",
		"За один раз добавляется не больше 20 треков.",
	},
	{ "lng_oblivion_rmusic_rejoin", "Вернуться в эфир" },
	{ "lng_oblivion_rmusic_retry", "Повторить загрузку" },
	{ "lng_oblivion_rmusic_row_play", "Играть сейчас" },
	{ "lng_oblivion_rmusic_row_next", "Играть следующим" },
	{ "lng_oblivion_rmusic_row_up", "Выше" },
	{ "lng_oblivion_rmusic_row_down", "Ниже" },
	{ "lng_oblivion_rmusic_row_remove", "Убрать из очереди" },
	{ "lng_oblivion_rmusic_state_loading", "Загрузка трека… {percent}%" },
	{ "lng_oblivion_rmusic_state_failed", "Не удалось загрузить трек" },
	{ "lng_oblivion_rmusic_state_paused", "Пауза" },
	{ "lng_oblivion_rmusic_state_paused_by", "Пауза · {name}" },
	{ "lng_oblivion_rmusic_state_starting", "Синхронизация…" },
	{ "lng_oblivion_rmusic_state_synced", "В эфире · синхронно" },
	{ "lng_oblivion_rmusic_state_catching", "В эфире · подстройка" },
	{ "lng_oblivion_rmusic_state_away", "Вы не слушаете эфир" },
	{ "lng_oblivion_rmusic_state_waiting", "Следующий трек…" },
	// Oblivion round 5: room video.
	{ "lng_oblivion_rvideo_tab", "Видео" },
	{ "lng_oblivion_rvideo_add_files", "Видеофайлы с компьютера…" },
	{ "lng_oblivion_rvideo_add_files_title", "Выберите видео" },
	{ "lng_oblivion_rvideo_files_filter", "Видеофайлы" },
	{ "lng_oblivion_rvideo_add_to_room", "Добавить видео в комнату" },
	{ "lng_oblivion_rvideo_added_toast", "Видео отправляется в комнату." },
	{
		"lng_oblivion_rvideo_downloading",
		"Видео скачивается из Telegram, затем отправится в комнату.",
	},
	{
		"lng_oblivion_rvideo_download_row",
		"Скачивается из Telegram… {percent}%",
	},
	{
		"lng_oblivion_rvideo_download_failed",
		"Не удалось скачать видео из Telegram.",
	},
	{
		"lng_oblivion_rvideo_wait",
		"Подождите немного: предыдущие видео ещё готовятся.",
	},
	{
		"lng_oblivion_rvideo_too_many",
		"За один раз добавляется не больше 5 видео.",
	},
	{
		"lng_oblivion_rvideo_not_video",
		"Этот файл не получится показать в комнате: это не видео.",
	},
	{
		"lng_oblivion_rvideo_protected",
		"Это видео из защищённого чата, его нельзя отправить в комнату.",
	},
	{
		"lng_oblivion_rvideo_no_add",
		"У вас нет права добавлять видео в этой комнате.",
	},
	{ "lng_oblivion_rvideo_default_title", "Видео от {date}" },
	{
		"lng_oblivion_rvideo_clear_sure",
		"Убрать все видео из очереди комнаты?",
	},
	{ "lng_oblivion_rvideo_empty", "Пока нет видео" },
	{
		"lng_oblivion_rvideo_empty_about",
		"Добавьте видеофайл с компьютера, перетащите его сюда или выберите "
		"«Добавить видео в комнату» в меню видео в любом чате Telegram. "
		"Оно начнётся у всех в комнате одновременно.",
	},
	{
		"lng_oblivion_rvideo_empty_no_right",
		"Видео в эту комнату добавляют другие участники.",
	},
	{ "lng_oblivion_rvideo_nothing", "Сейчас ничего не идёт" },
	{ "lng_oblivion_rvideo_placeholder", "Смотрите вместе" },
	{ "lng_oblivion_rvideo_state_loading", "Загрузка видео… {percent}%" },
	{ "lng_oblivion_rvideo_state_buffering", "Буферизация… {percent}%" },
	{ "lng_oblivion_rvideo_state_failed", "Не удалось загрузить видео" },
	{
		"lng_oblivion_rvideo_state_no_space",
		"Для этого видео не хватает места на диске",
	},
	{
		"lng_oblivion_rvideo_state_unplayable",
		"Это видео не удаётся воспроизвести",
	},
	{ "lng_oblivion_rvideo_state_paused", "Пауза" },
	{ "lng_oblivion_rvideo_state_paused_by", "Пауза · {name}" },
	{ "lng_oblivion_rvideo_state_starting", "Синхронизация…" },
	{ "lng_oblivion_rvideo_state_synced", "В эфире · синхронно" },
	{ "lng_oblivion_rvideo_state_catching", "В эфире · подстройка" },
	{ "lng_oblivion_rvideo_state_away", "Вы не смотрите эфир" },
	{ "lng_oblivion_rvideo_state_waiting", "Следующее видео…" },
	{ "lng_oblivion_rvideo_rejoin", "Вернуться к просмотру" },
	{ "lng_oblivion_rvideo_retry", "Повторить" },
	{ "lng_oblivion_rvideo_others_loading", "Ещё загружают: {names}" },
	{
		"lng_oblivion_rvideo_notice",
		"В комнате идёт видео «{title}». Откройте вкладку «Видео», чтобы "
		"смотреть вместе.",
	},
	{
		"lng_oblivion_rvideo_drop",
		"Отпустите видео здесь, чтобы добавить его в очередь",
	},
	{
		"lng_oblivion_rvideo_in_fullscreen",
		"Видео открыто на весь экран",
	},
	// Oblivion round 5: room canvas.
	{ "lng_oblivion_rcanvas_tab", "Холст" },
	{ "lng_oblivion_rcanvas_status_loading", "Загружаем холст…" },
	{ "lng_oblivion_rcanvas_status_failed", "Не удалось загрузить холст." },
	{ "lng_oblivion_rcanvas_retry", "Повторить" },
	{ "lng_oblivion_rcanvas_empty", "Холст пока чистый" },
	{
		"lng_oblivion_rcanvas_empty_about",
		"Всё, что здесь рисуют, сразу видят все в комнате.",
	},
	{
		"lng_oblivion_rcanvas_status_offline",
		"Нет связи с сервером. Рисование на паузе.",
	},
	{
		"lng_oblivion_rcanvas_status_no_right",
		"Владелец комнаты не разрешил вам рисовать.",
	},
	{
		"lng_oblivion_rcanvas_status_full",
		"Холст заполнен. Очистите его, чтобы рисовать дальше.",
	},
	{
		"lng_oblivion_rcanvas_status_disabled",
		"Холст сейчас отключён на сервере.",
	},
	{ "lng_oblivion_rcanvas_status_drawing_one", "{name} рисует…" },
	{ "lng_oblivion_rcanvas_status_drawing_many", "{names} рисуют…" },
	{
		"lng_oblivion_rcanvas_status_hint",
		"Рисуйте мышью. Ctrl+Z убирает ваш последний штрих.",
	},
	{
		"lng_oblivion_rcanvas_status_hint_mac",
		"Рисуйте мышью. ⌘Z убирает ваш последний штрих.",
	},
	{
		"lng_oblivion_rcanvas_error_right",
		"У вас нет права рисовать в этой комнате.",
	},
	{ "lng_oblivion_rcanvas_save_png", "Сохранить как PNG…" },
	{ "lng_oblivion_rcanvas_open_editor", "Открыть в фоторедакторе" },
	{ "lng_oblivion_rcanvas_clear", "Очистить холст" },
	{
		"lng_oblivion_rcanvas_clear_dark",
		"Очистить и сделать фон тёмным",
	},
	{
		"lng_oblivion_rcanvas_clear_light",
		"Очистить и сделать фон светлым",
	},
	{
		"lng_oblivion_rcanvas_clear_sure",
		"Очистить холст для всех в комнате? Отменить это будет нельзя.",
	},
	{ "lng_oblivion_rcanvas_clear_button", "Очистить" },
	{ "lng_oblivion_rcanvas_save_title", "Сохранить холст" },
	{ "lng_oblivion_rcanvas_saved", "Холст сохранён: {path}" },
	{ "lng_oblivion_rcanvas_save_failed", "Не удалось сохранить файл." },
	{
		"lng_oblivion_rcanvas_editor_no_window",
		"Сначала откройте окно Telegram этого аккаунта.",
	},
	// Oblivion round 5: room extras.
	{
		"lng_oblivion_rextra_settings_reactions",
		"Реакции и стикеры в комнатах",
	},
	{ "lng_oblivion_rextra_panel_emoji", "Все эмодзи" },
	{ "lng_oblivion_rextra_panel_stickers", "Стикеры" },
	{ "lng_oblivion_rextra_voice_open", "Голосовой чат" },
	{ "lng_oblivion_rextra_voice_create", "Создать голосовой чат…" },
	{ "lng_oblivion_rextra_voice_attach", "Привязать группу…" },
	{ "lng_oblivion_rextra_voice_detach", "Отвязать голосовой чат" },
	{ "lng_oblivion_rextra_voice_create_title", "Голосовой чат комнаты" },
	{
		"lng_oblivion_rextra_voice_create_about",
		"Oblivion создаст в вашем аккаунте Telegram закрытую группу "
		"«{title}» и запустит в ней видеочат. Вы войдёте в него с "
		"выключенным микрофоном.\n\nУ всех в комнате вверху окна появится "
		"кнопка с микрофоном. В группу попадёт только тот, кто сам нажмёт "
		"её и подтвердит вступление в Telegram.\n\nУдалить группу можно в "
		"любой момент.",
	},
	{ "lng_oblivion_rextra_voice_create_button", "Создать группу" },
	{ "lng_oblivion_rextra_voice_group_title", "Голос · {title}" },
	{
		"lng_oblivion_rextra_voice_group_about",
		"Голосовой чат комнаты Oblivion.",
	},
	{ "lng_oblivion_rextra_voice_creating", "Создаём группу в Telegram…" },
	{
		"lng_oblivion_rextra_voice_create_failed",
		"Не удалось создать группу: {error}",
	},
	{
		"lng_oblivion_rextra_voice_link_failed",
		"Не удалось получить ссылку-приглашение в группу: {error}",
	},
	{
		"lng_oblivion_rextra_voice_attached",
		"Голосовой чат привязан. У всех в комнате появилась кнопка с "
		"микрофоном.",
	},
	{
		"lng_oblivion_rextra_voice_busy",
		"Голосовой чат уже настраивается, одну секунду…",
	},
	{ "lng_oblivion_rextra_voice_attach_title", "Привязать группу" },
	{
		"lng_oblivion_rextra_voice_attach_about",
		"Выберите свою группу с видеочатом. Все в комнате смогут войти в "
		"неё по кнопке с микрофоном.",
	},
	{
		"lng_oblivion_rextra_voice_attach_empty",
		"У вас нет групп, в которые вы можете приглашать по ссылке.\n\n"
		"Выберите в меню комнаты «Создать голосовой чат…» — Oblivion сам "
		"создаст для него новую группу.",
	},
	{ "lng_oblivion_rextra_voice_attach_public", "Публичная группа" },
	{
		"lng_oblivion_rextra_voice_attach_sure",
		"Привязать группу «{title}»? Oblivion создаст в ней отдельную "
		"ссылку-приглашение и покажет её всем в комнате.",
	},
	{
		"lng_oblivion_rextra_voice_attach_sure_public",
		"Привязать группу «{title}»? Все в комнате получат её публичную "
		"ссылку.",
	},
	{ "lng_oblivion_rextra_voice_attach_button", "Привязать" },
	{
		"lng_oblivion_rextra_voice_detach_sure",
		"Отвязать голосовой чат? Группа останется в Telegram, а кнопка с "
		"микрофоном исчезнет у всех в комнате.",
	},
	{ "lng_oblivion_rextra_voice_detach_button", "Отвязать" },
	{
		"lng_oblivion_rextra_voice_no_call",
		"В группе «{title}» сейчас нет видеочата. Запустите его в группе "
		"в Telegram.",
	},
	{
		"lng_oblivion_rextra_voice_no_call_member",
		"В группе «{title}» сейчас нет видеочата. Попросите владельца "
		"комнаты запустить его.",
	},
	{
		"lng_oblivion_rextra_voice_no_window",
		"Сначала откройте окно Telegram этого аккаунта.",
	},
	{
		"lng_oblivion_rextra_voice_appeared",
		"В комнате появился голосовой чат: нажмите на микрофон вверху "
		"окна.",
	},
	{
		"lng_oblivion_rextra_voice_not_group",
		"Ссылка голосового чата этой комнаты ведёт не в группу Telegram, "
		"поэтому она не открыта.",
	},
	// Oblivion round 5: social.
	{ "lng_oblivion_social_section", "Профиль и видимость" },
	{ "lng_oblivion_social_friends", "Друзья в Oblivion" },
	{ "lng_oblivion_social_my_profile", "Мой профиль Oblivion" },
	{
		"lng_oblivion_social_friends_menu",
		"«Друзья в Oblivion» в главном меню",
	},
	{ "lng_oblivion_social_badge", "Значок Oblivion" },
	{
		"lng_oblivion_social_badge_show",
		"Показывать значки Oblivion у других",
	},
	{
		"lng_oblivion_social_profile_show",
		"Показывать профили и активность Oblivion у других",
	},
	{ "lng_oblivion_social_chip_listening", "Показывать, что я слушаю" },
	{
		"lng_oblivion_social_chip_room",
		"Показывать комнату, в которой я нахожусь",
	},
	{ "lng_oblivion_social_chip_online", "Показывать, что я в Oblivion" },
	{ "lng_oblivion_social_profile_audience", "Кто видит мой профиль" },
	{ "lng_oblivion_social_activity_audience", "Кто видит мою активность" },
	{ "lng_oblivion_social_chosen", "Выбранные люди" },
	{ "lng_oblivion_social_audience_nobody", "Никто" },
	{ "lng_oblivion_social_audience_chosen", "Выбранные люди" },
	{ "lng_oblivion_social_audience_everyone", "Все в Oblivion" },
	{
		"lng_oblivion_social_settings_about",
		"Всё это выключено, пока вы сами не включите, и настраивается "
		"только для текущего аккаунта. Значок публичный: его видят все, "
		"кто пользуется Oblivion. Профиль и активность видит тот круг "
		"людей, который вы выберете. Список ваших контактов никуда не "
		"отправляется: списки скачиваются и сравниваются на этом "
		"устройстве.",
	},
	{
		"lng_oblivion_social_badge_confirm",
		"Рядом с вашим именем появится значок. Его увидят все, кто "
		"пользуется Oblivion, и поймут, что этот аккаунт тоже использует "
		"Oblivion.\n\n"
		"Это не галочка Telegram: в обычных приложениях Telegram ничего "
		"не изменится. Текст «О себе» для значка больше не меняется.",
	},
	{ "lng_oblivion_social_badge_confirm_enable", "Включить" },
	{
		"lng_oblivion_social_badge_on",
		"Значок Oblivion включён для этого аккаунта.",
	},
	{
		"lng_oblivion_social_badge_off",
		"Значок Oblivion выключен для этого аккаунта.",
	},
	{
		"lng_oblivion_social_marker_remove",
		"Убрать старую метку из «О себе»",
	},
	{
		"lng_oblivion_social_marker_none",
		"В «О себе» уже нет метки Oblivion.",
	},
	{
		"lng_oblivion_social_marker_removed",
		"Старая метка Oblivion убрана из «О себе».",
	},
	{
		"lng_oblivion_social_marker_later",
		"Старая метка будет убрана из «О себе» при следующем запуске "
		"Oblivion с этим аккаунтом.",
	},
	{
		"lng_oblivion_social_marker_wait",
		"Telegram просит подождать, прежде чем снова менять профиль. "
		"Oblivion уберёт старую метку из «О себе», когда запустится с этим "
		"аккаунтом после ожидания.",
	},
	{
		"lng_oblivion_social_marker_stuck",
		"Oblivion не смог убрать старую метку из «О себе» за несколько "
		"попыток. Чтобы убрать её самостоятельно, измените текст «О себе» "
		"в настройках: метка исчезнет при сохранении.",
	},
	{
		"lng_oblivion_social_marker_about",
		"Раньше Oblivion хранил значок в виде невидимой метки в конце "
		"«О себе». Метка больше не нужна, но всё ещё осталась в «О себе» "
		"этого аккаунта. Убрать её?",
	},
	{
		"lng_oblivion_social_chip_hint_nobody",
		"Включено. Сейчас вашу активность никто не видит: выберите круг "
		"в пункте «Кто видит мою активность».",
	},
	{
		"lng_oblivion_social_chosen_hint",
		"В «Выбранных людях» пока никого нет. Добавьте туда людей.",
	},
	{ "lng_oblivion_social_chip_text_listening", "слушает: {text}" },
	{ "lng_oblivion_social_chip_text_room", "в комнате: {title}" },
	{ "lng_oblivion_social_chip_text_room_plain", "в комнате" },
	{ "lng_oblivion_social_chip_text_online", "в Oblivion" },
	{ "lng_oblivion_social_chip_join", "Войти" },
	{ "lng_oblivion_social_block_title", "Oblivion" },
	{ "lng_oblivion_social_block_edit", "Изменить" },
	{ "lng_oblivion_social_block_playlists#one", "{count} плейлист" },
	{ "lng_oblivion_social_block_playlists#few", "{count} плейлиста" },
	{ "lng_oblivion_social_block_playlists#many", "{count} плейлистов" },
	{ "lng_oblivion_social_block_playlists#other", "{count} плейлиста" },
	{ "lng_oblivion_social_block_presets#one", "{count} набор эффектов" },
	{ "lng_oblivion_social_block_presets#few", "{count} набора эффектов" },
	{ "lng_oblivion_social_block_presets#many", "{count} наборов эффектов" },
	{ "lng_oblivion_social_block_presets#other", "{count} набора эффектов" },
	{ "lng_oblivion_social_shared_title", "Плейлисты и наборы" },
	{ "lng_oblivion_social_shared_loading", "Загрузка…" },
	{ "lng_oblivion_social_shared_empty", "Здесь пока ничего нет." },
	{
		"lng_oblivion_social_shared_failed",
		"Не удалось загрузить список. Попробуйте позже.",
	},
	{ "lng_oblivion_social_shared_playlists", "Плейлисты" },
	{ "lng_oblivion_social_shared_presets", "Наборы эффектов" },
	{ "lng_oblivion_social_shared_tracks#one", "{count} трек" },
	{ "lng_oblivion_social_shared_tracks#few", "{count} трека" },
	{ "lng_oblivion_social_shared_tracks#many", "{count} треков" },
	{ "lng_oblivion_social_shared_tracks#other", "{count} трека" },
	{ "lng_oblivion_social_shared_photo", "фото" },
	{ "lng_oblivion_social_shared_video", "видео" },
	{ "lng_oblivion_social_editor_preview", "Так это увидят другие" },
	{
		"lng_oblivion_social_editor_preview_empty",
		"Добавьте статус, и этот блок появится в вашем профиле.",
	},
	{ "lng_oblivion_social_editor_name", "Имя в Oblivion" },
	{ "lng_oblivion_social_editor_status", "Статус" },
	{ "lng_oblivion_social_editor_emoji", "Эмодзи статуса" },
	{ "lng_oblivion_social_editor_accent", "Цвет профиля" },
	{
		"lng_oblivion_social_editor_accent_about",
		"Этим цветом в профиле выделяются ваша активность («слушает», "
		"«в комнате») и метка блока Oblivion.",
	},
	{ "lng_oblivion_social_editor_shared", "Показывать в профиле" },
	{
		"lng_oblivion_social_editor_shared_about",
		"Включённое здесь показывается в вашем профиле — только тем, кто "
		"видит профиль.",
	},
	{
		"lng_oblivion_social_editor_shared_empty",
		"Вы пока не делились плейлистами и наборами эффектов.",
	},
	{ "lng_oblivion_social_editor_photo", "Фото в Oblivion" },
	{ "lng_oblivion_social_editor_photo_set", "Взять моё фото из Telegram" },
	{ "lng_oblivion_social_editor_photo_remove", "Убрать фото" },
	{
		"lng_oblivion_social_editor_photo_about",
		"Фото видят участники ваших комнат и те, кому виден ваш профиль. "
		"Оно отправляется на сервер Oblivion, только когда вы сами "
		"нажимаете кнопку выше.",
	},
	{
		"lng_oblivion_social_editor_photo_none",
		"У этого аккаунта Telegram нет фото или оно ещё загружается. "
		"Попробуйте чуть позже.",
	},
	{ "lng_oblivion_social_editor_photo_done", "Фото обновлено." },
	{ "lng_oblivion_social_editor_photo_removed", "Фото убрано." },
	{ "lng_oblivion_social_editor_saved", "Профиль сохранён." },
	{
		"lng_oblivion_social_editor_hidden",
		"Сейчас ваш профиль никто не видит. Выберите круг в пункте «Кто "
		"видит мой профиль».",
	},
	{ "lng_oblivion_social_friends_loading", "Загружаем список…" },
	{
		"lng_oblivion_social_friends_offline",
		"Сервер Oblivion сейчас недоступен. Показано то, что было "
		"сохранено раньше.",
	},
	{
		"lng_oblivion_social_friends_offline_empty",
		"Сервер Oblivion сейчас недоступен. Попробуйте позже.",
	},
	{ "lng_oblivion_social_friends_empty_title", "Пока никого нет" },
	{
		"lng_oblivion_social_friends_empty",
		"Здесь появятся люди из ваших чатов и контактов, которые "
		"пользуются Oblivion и открыли вам свой профиль. Список контактов "
		"никуда не отправляется: списки сравниваются на этом устройстве.",
	},
	{
		"lng_oblivion_social_friends_hidden",
		"Ваш профиль сейчас скрыт от всех. Чтобы друзья могли вас найти, "
		"выберите круг в пункте «Кто видит мой профиль»: Настройки > "
		"Oblivion.",
	},
	{ "lng_oblivion_social_friends_plain", "пользуется Oblivion" },
	{
		"lng_oblivion_social_chosen_about",
		"Люди, которых вы выбираете сами. Когда в настройках выбрано "
		"«Выбранные люди», ваш профиль или активность видят только они. "
		"Этот список хранится на сервере Oblivion, а список ваших "
		"контактов никуда не отправляется.",
	},
	{ "lng_oblivion_social_chosen_empty", "Пока никто не выбран." },
	{ "lng_oblivion_social_chosen_add", "Добавить людей" },
	{ "lng_oblivion_social_chosen_remove", "Убрать" },
	{ "lng_oblivion_social_chosen_unknown", "Пользователь {name}" },
	{
		"lng_oblivion_social_chosen_full",
		"Список выбранных людей заполнен.",
	},
	{ "lng_oblivion_social_chosen_add_title", "Добавить в выбранные" },
	{
		"lng_oblivion_social_editor_gallery",
		"Показывать всем в «Общих наборах»",
	},
	{
		"lng_oblivion_social_editor_gallery_about",
		"Включённый здесь набор видят в «Общих наборах» все, кто пользуется "
		"Oblivion, вместе с вашим именем в Oblivion — кому бы ни был открыт "
		"ваш профиль. Он же показывается в вашем профиле. Выключите, чтобы "
		"убрать набор из «Общих наборов» и из профиля; ссылка на него "
		"продолжит работать.",
	},
	{
		"lng_oblivion_social_editor_gallery_apart",
		"Включённый здесь набор видят в «Общих наборах» все, кто пользуется "
		"Oblivion, вместе с вашим именем в Oblivion — кому бы ни был открыт "
		"ваш профиль. Выключите, чтобы убрать набор из «Общих наборов»; "
		"ссылка на него продолжит работать.",
	},
	{ "lng_oblivion_social_gallery_confirm_title", "Показать всем?" },
	{
		"lng_oblivion_social_gallery_confirm",
		"Набор «{title}» появится в «Общих наборах» у всех, кто пользуется "
		"Oblivion, и рядом с ним будет ваше имя в Oblivion. Это не зависит "
		"от того, кто видит ваш профиль.",
	},
	{ "lng_oblivion_social_gallery_confirm_yes", "Показать всем" },
	{
		"lng_oblivion_social_settings_left",
		"Oblivion Cloud для этого аккаунта отключён, но на сервере остаётся "
		"то, что показано выше, и другие люди по-прежнему это видят. Чтобы "
		"что-то скрыть или изменить, нажмите на нужный пункт: Oblivion "
		"предложит подключиться снова и после этого изменит настройку на "
		"сервере.",
	},
	// Oblivion round 5: share.
	{ "lng_oblivion_share_playlist_menu", "Поделиться…" },
	{ "lng_oblivion_share_library", "Общие плейлисты" },
	{
		"lng_oblivion_share_library_about",
		"Плейлисты, которыми вы поделились, и те, что вы добавили к себе "
		"по ссылке. Чтобы поделиться плейлистом, откройте его и выберите "
		"в меню «Поделиться…».",
	},
	{ "lng_oblivion_share_library_empty", "Здесь пока ничего нет." },
	{ "lng_oblivion_share_library_loading", "Загрузка списка…" },
	{
		"lng_oblivion_share_playlist_empty",
		"В этом плейлисте пока нет треков.",
	},
	{
		"lng_oblivion_share_playlist_sure",
		"Загрузить плейлист «{name}» в Oblivion Cloud? Его музыка "
		"({tracks}) будет отправлена на сервер разработчика Oblivion. "
		"Слушать её сможет любой, у кого есть ссылка.",
	},
	{
		"lng_oblivion_share_playlist_new_sure",
		"Плейлистом «{name}» вы уже поделились. Загрузить в него то, что "
		"появилось нового ({tracks})?",
	},
	{ "lng_oblivion_share_playlist_upload", "Загрузить" },
	{ "lng_oblivion_share_upload_title", "Загрузка плейлиста" },
	{ "lng_oblivion_share_upload_track", "Трек {index} из {total}" },
	{ "lng_oblivion_share_upload_fetch", "Получение файла…" },
	{ "lng_oblivion_share_upload_send", "Отправка на сервер…" },
	{ "lng_oblivion_share_upload_add", "Добавление в плейлист…" },
	{
		"lng_oblivion_share_upload_skipped#one",
		"Пропущен {count} трек: файл недоступен, защищён от копирования "
		"или слишком большой.",
	},
	{
		"lng_oblivion_share_upload_skipped#few",
		"Пропущено {count} трека: файлы недоступны, защищены от "
		"копирования или слишком большие.",
	},
	{
		"lng_oblivion_share_upload_skipped#many",
		"Пропущено {count} треков: файлы недоступны, защищены от "
		"копирования или слишком большие.",
	},
	{
		"lng_oblivion_share_upload_skipped#other",
		"Пропущено {count} трека: файлы недоступны, защищены от "
		"копирования или слишком большие.",
	},
	{
		"lng_oblivion_share_upload_hint",
		"Окно можно скрыть — загрузка продолжится.",
	},
	{ "lng_oblivion_share_upload_hide", "Скрыть" },
	{ "lng_oblivion_share_upload_cancel", "Отменить загрузку" },
	{
		"lng_oblivion_share_upload_done#one",
		"Готово: на сервере {count} трек.",
	},
	{
		"lng_oblivion_share_upload_done#few",
		"Готово: на сервере {count} трека.",
	},
	{
		"lng_oblivion_share_upload_done#many",
		"Готово: на сервере {count} треков.",
	},
	{
		"lng_oblivion_share_upload_done#other",
		"Готово: на сервере {count} трека.",
	},
	{
		"lng_oblivion_share_upload_done_none",
		"Ни один трек загрузить не удалось.",
	},
	{ "lng_oblivion_share_upload_cancelled", "Загрузка отменена." },
	{
		"lng_oblivion_share_upload_kept#one",
		"{count} уже загруженный трек остался в плейлисте на сервере.",
	},
	{
		"lng_oblivion_share_upload_kept#few",
		"{count} уже загруженных трека остались в плейлисте на сервере.",
	},
	{
		"lng_oblivion_share_upload_kept#many",
		"{count} уже загруженных треков остались в плейлисте на сервере.",
	},
	{
		"lng_oblivion_share_upload_kept#other",
		"{count} уже загруженных трека остались в плейлисте на сервере.",
	},
	{
		"lng_oblivion_share_upload_busy",
		"Сейчас загружается другой плейлист. Дождитесь, пока он "
		"загрузится.",
	},
	{
		"lng_oblivion_share_upload_done_toast",
		"Плейлист «{name}» загружен. Ссылка на него — в «Общих "
		"плейлистах».",
	},
	{
		"lng_oblivion_share_upload_failed_toast",
		"Загрузка плейлиста «{name}» остановилась. Чтобы продолжить, "
		"выберите в его меню «Поделиться…».",
	},
	{
		"lng_oblivion_share_error_quota",
		"Ваше место для общих плейлистов на сервере ({size}) закончилось. "
		"Удалите старые общие плейлисты или треки и попробуйте ещё раз.",
	},
	{
		"lng_oblivion_share_error_tracks",
		"В общем плейлисте может быть не больше {limit} треков.",
	},
	{
		"lng_oblivion_share_error_playlists",
		"Поделиться можно не более чем {limit} плейлистами. Удалите с "
		"сервера те, что больше не нужны.",
	},
	{
		"lng_oblivion_share_error_daily",
		"Достигнут дневной лимит загрузки: {size} в сутки на один аккаунт, "
		"и есть общий лимит для всех, кто в одной сети с вами. Продолжите "
		"завтра.",
	},
	{
		"lng_oblivion_share_error_uploads",
		"Слишком много незавершённых загрузок. Попробуйте чуть позже.",
	},
	{
		"lng_oblivion_share_error_user_tracks",
		"Вы добавили слишком много треков в общие плейлисты: для одного "
		"аккаунта есть ограничение. Удалите ненужные треки из своих общих "
		"плейлистов и попробуйте ещё раз.",
	},
	{
		"lng_oblivion_share_error_gone",
		"Этого плейлиста на сервере больше нет.",
	},
	{ "lng_oblivion_share_view_title", "Общий плейлист" },
	{ "lng_oblivion_share_view_by", "автор: {name}" },
	{ "lng_oblivion_share_view_yours", "ваш" },
	{ "lng_oblivion_share_view_followers#one", "{count} слушатель" },
	{ "lng_oblivion_share_view_followers#few", "{count} слушателя" },
	{ "lng_oblivion_share_view_followers#many", "{count} слушателей" },
	{ "lng_oblivion_share_view_followers#other", "{count} слушателя" },
	{ "lng_oblivion_share_view_play", "Слушать всё" },
	{ "lng_oblivion_share_view_keep", "Добавить к себе" },
	{ "lng_oblivion_share_view_unkeep", "Убрать из моих" },
	{
		"lng_oblivion_share_view_kept",
		"Плейлист добавлен в «Общие плейлисты», треки сохраняются на это "
		"устройство.",
	},
	{ "lng_oblivion_share_view_unkept", "Плейлист убран из ваших." },
	{ "lng_oblivion_share_view_keeping", "скачано {ready} из {total}" },
	{ "lng_oblivion_share_view_kept_all", "скачан" },
	{ "lng_oblivion_share_view_collab", "Другие могут добавлять треки" },
	{ "lng_oblivion_share_view_add_files", "Добавить треки из файлов…" },
	{ "lng_oblivion_share_view_delete", "Удалить с сервера" },
	{
		"lng_oblivion_share_view_delete_sure",
		"Удалить плейлист «{name}» с сервера? Ссылка перестанет работать "
		"у всех.",
	},
	{ "lng_oblivion_share_view_deleted", "Плейлист удалён с сервера." },
	{ "lng_oblivion_share_view_remove_track", "Убрать из плейлиста" },
	{ "lng_oblivion_share_view_loading", "Плейлист загружается…" },
	{ "lng_oblivion_share_view_empty", "Здесь пока нет треков." },
	{
		"lng_oblivion_share_view_about",
		"Нажмите на трек, чтобы включить его в плеере приложения. Перед "
		"началом трек скачивается с сервера.",
	},
	{ "lng_oblivion_share_view_track_loading", "Скачивается…" },
	{ "lng_oblivion_share_view_track_failed", "Не удалось скачать трек" },
	{ "lng_oblivion_share_choose_files", "Выберите аудиофайлы" },
	{ "lng_oblivion_share_copy_link", "Скопировать ссылку" },
	{ "lng_oblivion_share_link_copied", "Ссылка скопирована." },
	{ "lng_oblivion_share_open", "Открыть" },
	{ "lng_oblivion_share_retry", "Повторить" },
	{
		"lng_oblivion_share_no_account",
		"Сначала откройте аккаунт Telegram.",
	},
	{ "lng_oblivion_share_presets_button", "Наборы" },
	{ "lng_oblivion_share_preset_save", "Сохранить набор…" },
	{ "lng_oblivion_share_preset_mine", "Мои наборы…" },
	{ "lng_oblivion_share_preset_share", "Поделиться набором…" },
	{ "lng_oblivion_share_preset_gallery", "Общие наборы…" },
	{ "lng_oblivion_share_preset_no_effects", "Сначала добавьте эффекты." },
	{ "lng_oblivion_share_preset_save_title", "Сохранить набор" },
	{ "lng_oblivion_share_preset_name", "Название" },
	{
		"lng_oblivion_share_preset_saved",
		"Набор сохранён в «Мои наборы».",
	},
	{
		"lng_oblivion_share_preset_limit",
		"Не удалось сохранить набор: сохранённых наборов слишком много "
		"или в этой версии Oblivion нет ни одного из его эффектов.",
	},
	{ "lng_oblivion_share_preset_applied", "Набор применён." },
	{ "lng_oblivion_share_mine_title", "Мои наборы" },
	{
		"lng_oblivion_share_mine_empty",
		"Сохранённых наборов пока нет. Соберите эффекты в редакторе и "
		"выберите «Сохранить набор…».",
	},
	{ "lng_oblivion_share_mine_rename", "Переименовать" },
	{ "lng_oblivion_share_mine_share", "Поделиться…" },
	{ "lng_oblivion_share_mine_delete", "Удалить" },
	{
		"lng_oblivion_share_mine_delete_sure",
		"Удалить набор «{name}» с этого устройства?",
	},
	{ "lng_oblivion_share_kind_photo", "Фото" },
	{ "lng_oblivion_share_kind_video", "Видео" },
	{ "lng_oblivion_share_preset_share_title", "Поделиться набором" },
	{
		"lng_oblivion_share_preset_share_about",
		"Эффекты и их настройки отправляются на сервер Oblivion Cloud. "
		"Любой, у кого есть ссылка, сможет применить их в своём "
		"редакторе.",
	},
	{ "lng_oblivion_share_preset_text", "Описание (необязательно)" },
	{
		"lng_oblivion_share_preset_listed",
		"Показывать всем в «Общих наборах»",
	},
	{ "lng_oblivion_share_preset_publish", "Опубликовать" },
	{
		"lng_oblivion_share_preset_cloud_limit",
		"Вы опубликовали слишком много наборов. Удалите с сервера те, что "
		"больше не нужны.",
	},
	{ "lng_oblivion_share_preset_published_title", "Набор опубликован" },
	{
		"lng_oblivion_share_preset_published_about",
		"Отправьте эту ссылку другу — она откроется прямо в Oblivion:",
	},
	{ "lng_oblivion_share_gallery_title", "Общие наборы" },
	{ "lng_oblivion_share_gallery_new", "Новые" },
	{ "lng_oblivion_share_gallery_top", "Популярные" },
	{ "lng_oblivion_share_gallery_loading", "Загрузка наборов…" },
	{
		"lng_oblivion_share_gallery_empty",
		"Здесь пока никто не поделился набором. Будьте первым: "
		"«Поделиться набором…» в редакторе.",
	},
	{ "lng_oblivion_share_gallery_more", "Показать ещё" },
	{ "lng_oblivion_share_gallery_uses#one", "применили {count} раз" },
	{ "lng_oblivion_share_gallery_uses#few", "применили {count} раза" },
	{ "lng_oblivion_share_gallery_uses#many", "применили {count} раз" },
	{ "lng_oblivion_share_gallery_uses#other", "применили {count} раза" },
	{ "lng_oblivion_share_preset_apply", "Применить" },
	{ "lng_oblivion_share_preset_keep", "Сохранить" },
	{
		"lng_oblivion_share_preset_kept_hint",
		"Набор сохранён в «Мои наборы». Они открываются кнопкой «Наборы» "
		"у эффектов в редакторе фото и кнопкой «Заготовки» в редакторе "
		"видео.",
	},
	{ "lng_oblivion_share_preset_report", "Пожаловаться" },
	{
		"lng_oblivion_share_preset_report_sure",
		"Пожаловаться на этот набор? Если на набор пожалуются несколько "
		"человек, он скрывается из «Общих наборов», пока его не посмотрит "
		"разработчик.",
	},
	{
		"lng_oblivion_share_preset_reported",
		"Спасибо, жалоба отправлена.",
	},
	{
		"lng_oblivion_share_preset_delete_sure",
		"Удалить этот набор с сервера? Ссылка перестанет работать у всех.",
	},
	{ "lng_oblivion_share_preset_deleted", "Набор удалён с сервера." },
	{
		"lng_oblivion_share_preset_too_new",
		"Этот набор сделан в более новой версии Oblivion, эта версия его "
		"не откроет. Обновите приложение.",
	},
	{
		"lng_oblivion_share_preset_hidden",
		"Этот набор скрыт из «Общих наборов» из-за жалоб. Ссылка на него "
		"работает.",
	},
	{ "lng_oblivion_share_preset_loading", "Загрузка набора…" },
	{ "lng_oblivion_share_preset_before", "До" },
	{ "lng_oblivion_share_preset_after", "После" },
	// Oblivion round 5: sync.
	{ "lng_oblivion_sync_section", "Обмен, синхронизация и обновления" },
	{ "lng_oblivion_sync_settings", "Синхронизация настроек" },
	{
		"lng_oblivion_sync_settings_auto",
		"Синхронизировать настройки автоматически",
	},
	{ "lng_oblivion_sync_title", "Синхронизация настроек" },
	{
		"lng_oblivion_sync_about",
		"Настройки Oblivion и сохранённые наборы шифруются на этом "
		"устройстве вашим паролем синхронизации и только потом "
		"отправляются: сервер хранит файл, который не может прочитать. "
		"Забытый пароль восстановить нельзя — отправьте настройки заново "
		"с новым.",
	},
	{ "lng_oblivion_sync_state_loading", "Проверка сервера…" },
	{ "lng_oblivion_sync_state_error", "Не удалось проверить сервер" },
	{ "lng_oblivion_sync_state_none", "На сервере пока нет копии" },
	{
		"lng_oblivion_sync_state_none_about",
		"Придумайте пароль и нажмите «Отправить настройки на сервер».",
	},
	{ "lng_oblivion_sync_state_copy", "Копия на сервере: {date}" },
	{ "lng_oblivion_sync_state_size", "Зашифрованный файл, {size}." },
	{
		"lng_oblivion_sync_state_synced",
		"Это устройство синхронизировалось {date}.",
	},
	{
		"lng_oblivion_sync_state_conflict",
		"Выберите, какие настройки оставить",
	},
	{
		"lng_oblivion_sync_conflict_about",
		"На сервере есть более новая копия с другого устройства, а "
		"настройки этого устройства тоже менялись. Ничего не заменено. "
		"«Отправить» оставит настройки этого устройства, «Получить» "
		"возьмёт их с сервера.",
	},
	{
		"lng_oblivion_sync_auto_on",
		"Автоматическая синхронизация включена.",
	},
	{
		"lng_oblivion_sync_auto_needs",
		"Автоматическая синхронизация начнётся после первой отправки или "
		"получения с паролем.",
	},
	{
		"lng_oblivion_sync_auto_paused",
		"Копии на сервере нет, поэтому сами настройки не отправляются. "
		"Автоматическая синхронизация продолжится после нажатия "
		"«Отправить настройки на сервер».",
	},
	{ "lng_oblivion_sync_password", "Пароль синхронизации" },
	{
		"lng_oblivion_sync_password_same",
		"Пароль (уже запомнен)",
	},
	{
		"lng_oblivion_sync_password_hint",
		"Используйте один и тот же пароль на всех своих устройствах, не "
		"короче 6 символов. Не берите пароль от Telegram.",
	},
	{
		"lng_oblivion_sync_password_kept",
		"Поле можно оставить пустым. Введите пароль, только если хотите "
		"сменить его или если копия на сервере сделана с другим.",
	},
	{
		"lng_oblivion_sync_password_short",
		"В пароле должно быть не меньше 6 символов.",
	},
	{
		"lng_oblivion_sync_password_wrong",
		"Этот пароль не подходит к копии на сервере. Проверьте его или "
		"отправьте настройки заново с новым паролем.",
	},
	{
		"lng_oblivion_sync_password_needed",
		"Введите пароль синхронизации: копия на сервере сделана с другим "
		"паролем.",
	},
	{ "lng_oblivion_sync_send", "Отправить настройки на сервер" },
	{ "lng_oblivion_sync_receive", "Получить настройки с сервера" },
	{
		"lng_oblivion_sync_send_first",
		"Отправить настройки этого устройства на сервер? Копии там пока "
		"нет.",
	},
	{
		"lng_oblivion_sync_send_sure",
		"Копия на сервере ({date}) будет заменена настройками этого "
		"устройства.",
	},
	{
		"lng_oblivion_sync_send_other",
		"Та копия зашифрована другим паролем. После отправки остальным "
		"вашим устройствам понадобится новый пароль.",
	},
	{ "lng_oblivion_sync_send_confirm", "Отправить" },
	{ "lng_oblivion_sync_sent", "Настройки отправлены." },
	{
		"lng_oblivion_sync_receive_sure#one",
		"{count} настройка Oblivion на этом устройстве будет заменена "
		"копией от {date} с устройства «{device}». Значок приложения, "
		"громкость и подключение к Oblivion Cloud останутся как есть.",
	},
	{
		"lng_oblivion_sync_receive_sure#few",
		"{count} настройки Oblivion на этом устройстве будут заменены "
		"копией от {date} с устройства «{device}». Значок приложения, "
		"громкость и подключение к Oblivion Cloud останутся как есть.",
	},
	{
		"lng_oblivion_sync_receive_sure#many",
		"{count} настроек Oblivion на этом устройстве будут заменены "
		"копией от {date} с устройства «{device}». Значок приложения, "
		"громкость и подключение к Oblivion Cloud останутся как есть.",
	},
	{
		"lng_oblivion_sync_receive_sure#other",
		"{count} настройки Oblivion на этом устройстве будут заменены "
		"копией от {date} с устройства «{device}». Значок приложения, "
		"громкость и подключение к Oblivion Cloud останутся как есть.",
	},
	{
		"lng_oblivion_sync_receive_presets#one",
		"{count} сохранённый набор эффектов был удалён на другом "
		"устройстве и будет удалён и с этого.",
	},
	{
		"lng_oblivion_sync_receive_presets#few",
		"{count} сохранённых набора эффектов были удалены на другом "
		"устройстве и будут удалены и с этого.",
	},
	{
		"lng_oblivion_sync_receive_presets#many",
		"{count} сохранённых наборов эффектов были удалены на другом "
		"устройстве и будут удалены и с этого.",
	},
	{
		"lng_oblivion_sync_receive_presets#other",
		"{count} сохранённых набора эффектов были удалены на другом "
		"устройстве и будут удалены и с этого.",
	},
	{ "lng_oblivion_sync_receive_confirm", "Заменить" },
	{ "lng_oblivion_sync_received", "Настройки получены." },
	{ "lng_oblivion_sync_received_same", "Настройки уже совпадают." },
	{ "lng_oblivion_sync_device_unknown", "другое устройство" },
	{
		"lng_oblivion_sync_error_newer",
		"Другое устройство только что отправило более новую копию. "
		"Посмотрите на неё и попробуйте ещё раз.",
	},
	{
		"lng_oblivion_sync_error_large",
		"Настроек слишком много, отправить их не получится.",
	},
	{ "lng_oblivion_sync_error_none", "На сервере нет копии." },
	{
		"lng_oblivion_sync_error_format",
		"Копия на сервере сделана более новой версией Oblivion или "
		"повреждена. Обновите приложение или отправьте настройки заново.",
	},
	{ "lng_oblivion_sync_forget", "Забыть пароль на этом устройстве" },
	{
		"lng_oblivion_sync_forgot",
		"Пароль на этом устройстве забыт. Автоматическая синхронизация "
		"приостановлена.",
	},
	{ "lng_oblivion_sync_erase", "Удалить копию с сервера" },
	{
		"lng_oblivion_sync_erase_sure",
		"Удалить копию настроек с сервера? Настройки на ваших устройствах "
		"останутся как есть. Автоматическая синхронизация на этом "
		"устройстве будет выключена. Новая копия появится на сервере, "
		"только когда вы нажмёте «Отправить настройки на сервер».",
	},
	{ "lng_oblivion_sync_erased", "Копия удалена с сервера." },
	{
		"lng_oblivion_sync_erased_auto",
		"Копия удалена с сервера. Автоматическая синхронизация выключена.",
	},
	{
		"lng_oblivion_sync_not_synced",
		"Не синхронизируются значок приложения, громкость, локальная "
		"расшифровка, опрос статусов и подключение к Oblivion Cloud: они "
		"на каждом устройстве свои.",
	},
	// Oblivion round 5: update.
	{ "lng_oblivion_update_settings_check", "Проверить обновления" },
	{ "lng_oblivion_update_settings_auto", "Проверять обновления раз в день" },
	{
		"lng_oblivion_update_settings_about",
		"Перед отправкой настройки шифруются на этом устройстве вашим "
		"паролем синхронизации, сервер не может их прочитать. "
		"Автоматическая проверка раз в день спрашивает у сервера Oblivion "
		"Cloud номер последней версии — только пока Oblivion Cloud "
		"подключён хотя бы для одного аккаунта. Сама она ничего не "
		"устанавливает.",
	},
	{ "lng_oblivion_update_title", "Доступна новая версия" },
	{ "lng_oblivion_update_title_loading", "Скачивание обновления" },
	{ "lng_oblivion_update_title_ready", "Обновление скачано" },
	{ "lng_oblivion_update_version", "Oblivion {version}" },
	{ "lng_oblivion_update_current", "у вас {version}" },
	{
		"lng_oblivion_update_required",
		"Это обновление нужно для Oblivion Cloud: ваша версия скоро "
		"перестанет подключаться.",
	},
	{
		"lng_oblivion_update_no_notes",
		"Описания изменений для этой версии нет.",
	},
	{
		"lng_oblivion_update_file",
		"{name}, {size}. Файл сохранится в папку «Загрузки», установить "
		"его нужно вручную.",
	},
	{
		"lng_oblivion_update_no_file",
		"Сборка для вашей системы ещё не выложена. Проверьте позже.",
	},
	{ "lng_oblivion_update_download", "Скачать" },
	{ "lng_oblivion_update_later", "Позже" },
	{ "lng_oblivion_update_retry", "Повторить" },
	{ "lng_oblivion_update_progress", "Скачано {ready} из {total}" },
	{ "lng_oblivion_update_saved", "Файл сохранён и проверен:\n{path}" },
	{
		"lng_oblivion_update_install_mac",
		"Закройте Oblivion, распакуйте архив и замените приложение в "
		"«Программах» новым.",
	},
	{
		"lng_oblivion_update_install_win",
		"Закройте Oblivion и запустите скачанный файл.",
	},
	{ "lng_oblivion_update_show", "Показать в папке" },
	{
		"lng_oblivion_update_error_file",
		"Файл не удалось сохранить, или он не прошёл проверку. Попробуйте "
		"ещё раз.",
	},
	{ "lng_oblivion_update_checking", "Проверка обновлений…" },
	{
		"lng_oblivion_update_latest",
		"У вас последняя версия Oblivion ({version}).",
	},
	// Oblivion round 5: deleted search.
	{ "lng_oblivion_dsearch_open", "Фильтры…" },
	{ "lng_oblivion_dsearch_title", "Поиск в сохранённом" },
	{ "lng_oblivion_dsearch_placeholder", "Слова, имя или файл" },
	{ "lng_oblivion_dsearch_chat_all", "Все чаты" },
	{ "lng_oblivion_dsearch_chat_this", "Этот чат" },
	{ "lng_oblivion_dsearch_period_all", "Всё время" },
	{ "lng_oblivion_dsearch_period_today", "Сегодня" },
	{ "lng_oblivion_dsearch_period_week", "За 7 дней" },
	{ "lng_oblivion_dsearch_period_month", "За 30 дней" },
	{ "lng_oblivion_dsearch_period_year", "За год" },
	{ "lng_oblivion_dsearch_period_day", "Выбрать день…" },
	{ "lng_oblivion_dsearch_kind_all", "Все записи" },
	{ "lng_oblivion_dsearch_kind_deleted", "Удалённые" },
	{ "lng_oblivion_dsearch_kind_edited", "Изменённые" },
	{ "lng_oblivion_dsearch_badge_deleted", "удалено" },
	{ "lng_oblivion_dsearch_badge_edited", "изменено" },
	{ "lng_oblivion_dsearch_empty", "Ничего не найдено" },
	{
		"lng_oblivion_dsearch_empty_hint",
		"Попробуйте другие слова или уберите фильтры.",
	},
	{ "lng_oblivion_dsearch_nothing", "Пока ничего не сохранено" },
	{
		"lng_oblivion_dsearch_nothing_hint",
		"Удалённые и изменённые сообщения появляются здесь, пока их "
		"сохранение включено в настройках.",
	},
	{ "lng_oblivion_dsearch_searching", "Ищем…" },
	{ "lng_oblivion_dsearch_unknown_chat", "Неизвестный чат" },
	{ "lng_oblivion_dsearch_record_title", "Удалённое сообщение" },
	{ "lng_oblivion_dsearch_versions_title", "История изменений" },
	{ "lng_oblivion_dsearch_version_was", "Было до {date}" },
	{ "lng_oblivion_dsearch_version_now", "Сейчас" },
	{ "lng_oblivion_dsearch_jump", "Перейти к сообщению" },
	{ "lng_oblivion_dsearch_gone", "Эта запись больше не сохранена." },
	// Oblivion round 5: send online.
	{ "lng_oblivion_sendonline_section", "Отправка, когда будет в сети" },
	{
		"lng_oblivion_sendonline_settings",
		"«Отправить, когда будет в сети» в меню отправки",
	},
	{ "lng_oblivion_sendonline_settings_limit", "Ждать не дольше" },
	{ "lng_oblivion_sendonline_settings_list", "Ожидают отправки" },
	{ "lng_oblivion_sendonline_hours#one", "{count} час" },
	{ "lng_oblivion_sendonline_hours#few", "{count} часа" },
	{ "lng_oblivion_sendonline_hours#many", "{count} часов" },
	{ "lng_oblivion_sendonline_hours#other", "{count} часа" },
	{ "lng_oblivion_sendonline_days#one", "{count} день" },
	{ "lng_oblivion_sendonline_days#few", "{count} дня" },
	{ "lng_oblivion_sendonline_days#many", "{count} дней" },
	{ "lng_oblivion_sendonline_days#other", "{count} дня" },
	{
		"lng_oblivion_sendonline_settings_about",
		"Сообщение ждёт на этом устройстве и отправляется, когда человек "
		"появится в сети (или напишет вам, если скрывает время в сети), "
		"пока Oblivion запущен. По истечении срока оно не отправляется: "
		"вас спросят, что с ним делать. Если вы удалите чат или "
		"заблокируете человека, ожидающие его сообщения тоже не "
		"отправятся. Вариант Telegram, который ждёт на сервере, остаётся "
		"в окне «Отложить отправку».",
	},
	{ "lng_oblivion_sendonline_menu", "Отправить, когда будет в сети" },
	{
		"lng_oblivion_sendonline_queued",
		"Сообщение отправится, когда {name} появится в сети.",
	},
	{
		"lng_oblivion_sendonline_queued_hidden",
		"{name} скрывает время в сети: сообщение отправится, когда вам "
		"напишут из этого чата.",
	},
	{
		"lng_oblivion_sendonline_too_long",
		"Это сообщение слишком длинное для отправки с ожиданием.",
	},
	{
		"lng_oblivion_sendonline_too_many",
		"Слишком много сообщений уже ждут отправки.",
	},
	{
		"lng_oblivion_sendonline_bar_waiting#one",
		"{count} сообщение ждёт отправки",
	},
	{
		"lng_oblivion_sendonline_bar_waiting#few",
		"{count} сообщения ждут отправки",
	},
	{
		"lng_oblivion_sendonline_bar_waiting#many",
		"{count} сообщений ждут отправки",
	},
	{
		"lng_oblivion_sendonline_bar_waiting#other",
		"{count} сообщения ждут отправки",
	},
	{ "lng_oblivion_sendonline_bar_state", "пока человек не в сети" },
	{ "lng_oblivion_sendonline_bar_sending", "Отправляется…" },
	{
		"lng_oblivion_sendonline_bar_problem#one",
		"{count} сообщение не отправлено",
	},
	{
		"lng_oblivion_sendonline_bar_problem#few",
		"{count} сообщения не отправлены",
	},
	{
		"lng_oblivion_sendonline_bar_problem#many",
		"{count} сообщений не отправлено",
	},
	{
		"lng_oblivion_sendonline_bar_problem#other",
		"{count} сообщения не отправлены",
	},
	{ "lng_oblivion_sendonline_bar_open", "Открыть" },
	{ "lng_oblivion_sendonline_reason_expired", "Срок ожидания истёк" },
	{
		"lng_oblivion_sendonline_reason_unconfirmed",
		"Не удалось подтвердить отправку, проверьте чат",
	},
	{
		"lng_oblivion_sendonline_reason_unavailable",
		"Этот чат больше недоступен",
	},
	{
		"lng_oblivion_sendonline_reason_paid",
		"Сообщения этому человеку теперь платные",
	},
	{
		"lng_oblivion_sendonline_reason_server",
		"Telegram не принял сообщение ({error})",
	},
	{ "lng_oblivion_sendonline_reason_chat_deleted", "Чат был удалён" },
	{
		"lng_oblivion_sendonline_reason_blocked",
		"Вы заблокировали этого человека",
	},
	{
		"lng_oblivion_sendonline_gone",
		"Это сообщение больше не ждёт отправки: оно уже отправлено или "
		"убрано.",
	},
	{
		"lng_oblivion_sendonline_busy",
		"Это сообщение уже отправляется: изменить или убрать его нельзя.",
	},
	{
		"lng_oblivion_sendonline_not_saved",
		"Не удалось записать на диск этого устройства, поэтому сообщение "
		"не ждёт отправки. Ничего не отправлено.",
	},
	{
		"lng_oblivion_sendonline_not_loaded",
		"Этот чат ещё не загружен. Откройте его и попробуйте ещё раз.",
	},
	{
		"lng_oblivion_sendonline_list_empty",
		"Нет сообщений, ожидающих отправки.",
	},
	{
		"lng_oblivion_sendonline_list_empty_about",
		"Нажмите правой кнопкой на кнопку отправки в личном чате и "
		"выберите «Отправить, когда будет в сети».",
	},
	{
		"lng_oblivion_sendonline_status_waiting",
		"Ждёт, когда будет в сети · до {date}",
	},
	{
		"lng_oblivion_sendonline_status_retry",
		"Повторим, когда человек будет в сети",
	},
	{ "lng_oblivion_sendonline_action_edit", "Изменить" },
	{ "lng_oblivion_sendonline_action_cancel", "Убрать" },
	{ "lng_oblivion_sendonline_action_send", "Отправить" },
	{ "lng_oblivion_sendonline_action_wait", "Ждать ещё" },
	{ "lng_oblivion_sendonline_edit_title", "Сообщение ждёт отправки" },
	{ "lng_oblivion_sendonline_edit_placeholder", "Текст сообщения" },
	{
		"lng_oblivion_sendonline_cancel_sure",
		"Убрать это сообщение из ожидающих? Оно не будет отправлено.",
	},
	{
		"lng_oblivion_sendonline_send_sure",
		"Отправить это сообщение сейчас, не дожидаясь, когда {name} "
		"появится в сети?",
	},
	{
		"lng_oblivion_sendonline_unconfirmed_sure",
		"Возможно, это сообщение уже отправлено. Сначала проверьте чат "
		"и отправляйте ещё раз, только если его там нет.",
	},
	{ "lng_oblivion_sendonline_ask_title", "Сообщения не отправлены" },
	{
		"lng_oblivion_sendonline_ask_text#one",
		"{count} ожидавшее сообщение не отправлено. Откройте список: там "
		"видно почему, и можно решить, что с ним делать.",
	},
	{
		"lng_oblivion_sendonline_ask_text#few",
		"{count} ожидавших сообщения не отправлены. Откройте список: там "
		"видно почему, и можно решить, что с ними делать.",
	},
	{
		"lng_oblivion_sendonline_ask_text#many",
		"{count} ожидавших сообщений не отправлено. Откройте список: там "
		"видно почему, и можно решить, что с ними делать.",
	},
	{
		"lng_oblivion_sendonline_ask_text#other",
		"{count} ожидавших сообщения не отправлены. Откройте список: там "
		"видно почему, и можно решить, что с ними делать.",
	},
	{ "lng_oblivion_sendonline_ask_open", "Открыть список" },
	{ "lng_oblivion_sendonline_ask_later", "Позже" },
	// Oblivion round 5: stats export.
	{ "lng_oblivion_statsexp_save", "Сохранить страницу…" },
	{ "lng_oblivion_statsexp_filter", "Страница HTML" },
	{ "lng_oblivion_statsexp_file", "Статистика" },
	{ "lng_oblivion_statsexp_failed", "Не удалось сохранить файл." },
	{ "lng_oblivion_statsexp_done_title", "Страница сохранена" },
	{
		"lng_oblivion_statsexp_done_about",
		"Один файл HTML, он открывается в любом браузере без интернета. "
		"В нём только счётчики, текстов сообщений нет.",
	},
	{ "lng_oblivion_statsexp_done_open", "Открыть" },
	{ "lng_oblivion_statsexp_done_folder", "Показать в папке" },
	{ "lng_oblivion_statsexp_page_title", "Статистика чата" },
	{ "lng_oblivion_statsexp_page_made", "Создано в Oblivion {date}" },
	{
		"lng_oblivion_statsexp_page_privacy",
		"Только счётчики: текстов сообщений на этой странице нет. Всё "
		"посчитано на устройстве.",
	},
	{
		"lng_oblivion_statsexp_page_incomplete",
		"История за этот период прочитана не полностью, числа неполные.",
	},
	{ "lng_oblivion_statsexp_page_table", "Показать таблицей" },
	{ "lng_oblivion_statsexp_page_month", "Месяц" },
	{ "lng_oblivion_statsexp_page_hour", "Час" },
	{ "lng_oblivion_statsexp_page_day", "День" },
	{
		"lng_oblivion_statsexp_page_calendar_note",
		"Последние двенадцать месяцев, один квадрат — один день.",
	},
	// Oblivion round 5 end.
};

const LangOverride kLooks[] = {
	// Oblivion looks: core.
	{ "lng_oblivion_look_title", "Тема Oblivion" },
	{ "lng_oblivion_look_plain", "Обычный Telegram" },
	{ "lng_oblivion_look_native", "Родной, но лучше" },
	{ "lng_oblivion_look_night_air", "Ночной эфир" },
	{ "lng_oblivion_look_silence", "Тишина" },
	{
		"lng_oblivion_look_plain_about",
		"Всё как в Telegram, с вашей темой.",
	},
	{
		"lng_oblivion_look_native_about",
		"Тот же Telegram, только аккуратнее: карточки и плашки.",
	},
	{
		"lng_oblivion_look_night_air_about",
		"Фиолетовая ночь, свечение и один яркий градиент.",
	},
	{
		"lng_oblivion_look_silence_about",
		"Спокойный минимализм и один кислотный акцент.",
	},
	{
		"lng_oblivion_look_note",
		"Тема Oblivion меняет цвета всего приложения и вид экранов "
		"Oblivion: блока в профиле, комнат, друзей и инструментов. "
		"Расположение обычных экранов Telegram остаётся прежним.\n\n"
		"Ваша тема Telegram и обои чата не изменяются: выберите "
		"«Обычный Telegram» — и всё вернётся. «Ночной эфир» и «Тишина» "
		"показывают свой фон чата, только пока вы сами не выбрали обои.",
	},
	{
		"lng_oblivion_look_paused",
		"Пока вы редактируете тему Telegram, тема Oblivion приостановлена. "
		"Она вернётся, когда вы закроете редактор.",
	},
	// Oblivion looks: own screens.
	{ "lng_oblivion_look_room_now_playing", "Сейчас играет" },
	// Oblivion looks: upstream.
	// Oblivion looks: hub.
	{ "lng_oblivion_hub_menu", "Инструменты Oblivion" },
	{ "lng_oblivion_hub_title", "Инструменты" },
	{
		"lng_oblivion_hub_about",
		"Всё, чего нет в обычном Telegram, — в одном месте",
	},
	{ "lng_oblivion_hub_search", "Найти инструмент" },
	{
		"lng_oblivion_hub_empty",
		"Такого инструмента нет. Попробуйте другое слово.",
	},
	{
		"lng_oblivion_hub_settings_about",
		"Все инструменты Oblivion на одном экране. Этот же экран "
		"открывается из главного меню.",
	},
	{ "lng_oblivion_hub_group_create", "Создавать" },
	{ "lng_oblivion_hub_group_together", "Вместе" },
	{ "lng_oblivion_hub_group_know", "Знать" },
	{ "lng_oblivion_hub_group_app", "Приложение" },
	{ "lng_oblivion_hub_new", "новое" },
	{ "lng_oblivion_hub_footer#one", "{count} инструмент" },
	{ "lng_oblivion_hub_footer#few", "{count} инструмента" },
	{ "lng_oblivion_hub_footer#many", "{count} инструментов" },
	{ "lng_oblivion_hub_footer#other", "{count} инструмента" },
	{ "lng_oblivion_hub_live", "Комната идёт сейчас" },
	{ "lng_oblivion_hub_live_join", "Войти" },
	{ "lng_oblivion_hub_live_untitled", "Комната" },
	{ "lng_oblivion_hub_live_silence", "Сейчас ничего не играет" },
	{ "lng_oblivion_hub_t_sticker_studio", "Студия стикеров" },
	{ "lng_oblivion_hub_t_sticker_batch", "Стикеры пачкой" },
	{ "lng_oblivion_hub_t_lottie", "Lottie-редактор" },
	{ "lng_oblivion_hub_t_voice", "Войсчейнджер" },
	{ "lng_oblivion_hub_t_round", "Видео в кружок" },
	{ "lng_oblivion_hub_t_ocr", "Текст с картинки" },
	{ "lng_oblivion_hub_t_cutout", "Вырезать фон" },
	{ "lng_oblivion_hub_t_listen", "Слушать вместе" },
	{ "lng_oblivion_hub_t_send_online", "Когда будет в сети" },
	{ "lng_oblivion_hub_t_chat_stats", "Статистика чата" },
	{ "lng_oblivion_hub_t_online", "Журнал онлайна" },
	{ "lng_oblivion_hub_t_profile_history", "История профиля" },
	{ "lng_oblivion_hub_t_ghost", "Режим призрака" },
	{ "lng_oblivion_hub_t_updates", "Обновления" },
	{ "lng_oblivion_hub_t_settings", "Настройки Oblivion" },
	{ "lng_oblivion_hub_s_photo_editor", "слои, эффекты, рисование" },
	{ "lng_oblivion_hub_s_collage", "несколько фото в одной сетке" },
	{ "lng_oblivion_hub_s_video_editor", "обрезка, эффекты, трекинг" },
	{
		"lng_oblivion_hub_s_sticker_studio",
		"перекрасить анимированный стикер",
	},
	{ "lng_oblivion_hub_s_sticker_packs", "создавать и пополнять наборы" },
	{ "lng_oblivion_hub_s_sticker_converter", "фото и видео — в стикер" },
	{
		"lng_oblivion_hub_s_sticker_batch",
		"много стикеров или эмодзи сразу",
	},
	{ "lng_oblivion_hub_s_lottie", "анимация по кадрам" },
	{ "lng_oblivion_hub_s_music_editor", "обрезка, темп, тон" },
	{ "lng_oblivion_hub_s_voice", "голосовые с эффектами" },
	{ "lng_oblivion_hub_s_round", "любое видео — кружком" },
	{ "lng_oblivion_hub_s_ocr", "распознать и скопировать" },
	{ "lng_oblivion_hub_s_cutout", "объект без фона" },
	{ "lng_oblivion_hub_s_rooms", "музыка, видео, холст" },
	{ "lng_oblivion_hub_s_listen", "прямо в чате" },
	{ "lng_oblivion_hub_s_friends", "кто что слушает" },
	{ "lng_oblivion_hub_s_my_profile", "статус, цвет, что видят другие" },
	{ "lng_oblivion_hub_s_playlists", "своя музыка по спискам" },
	{ "lng_oblivion_hub_s_shared_playlists", "поделиться по ссылке" },
	{ "lng_oblivion_hub_s_presets", "эффекты по ссылке" },
	{ "lng_oblivion_hub_s_send_online", "сообщение подождёт" },
	{ "lng_oblivion_hub_s_chat_stats", "кто, когда, сколько" },
	{ "lng_oblivion_hub_s_deleted", "поиск по сохранённым" },
	{ "lng_oblivion_hub_s_online", "кто когда заходил" },
	{ "lng_oblivion_hub_s_profile_history", "имена, фото, био" },
	{ "lng_oblivion_hub_s_gift_catalog", "модели и фоны" },
	{ "lng_oblivion_hub_s_ghost", "читать незаметно" },
	{ "lng_oblivion_hub_s_look", "три оформления и обычный Telegram" },
	{ "lng_oblivion_hub_s_app_icon", "Oblivion, Telegram, своя" },
	{ "lng_oblivion_hub_s_cloud", "значок, синхронизация, подключение" },
	{ "lng_oblivion_hub_s_updates", "проверить новую версию" },
	{ "lng_oblivion_hub_s_settings", "все переключатели" },
	{
		"lng_oblivion_hub_h_round",
		"Откройте чат, в который хотите отправить кружок, и выберите "
		"этот инструмент снова.",
	},
	{
		"lng_oblivion_hub_h_ocr",
		"Скопируйте картинку и выберите этот инструмент снова. Или "
		"нажмите правой кнопкой на фото в чате: «Скопировать текст с "
		"фото».",
	},
	{
		"lng_oblivion_hub_h_cutout",
		"Скопируйте картинку и выберите этот инструмент снова. Или "
		"нажмите правой кнопкой на фото в чате: «Вырезать объект».",
	},
	{
		"lng_oblivion_hub_h_listen",
		"Нажмите правой кнопкой на музыку в чате: «Слушать вместе в "
		"этом чате».",
	},
	{
		"lng_oblivion_hub_h_chat_stats",
		"Откройте чат и выберите этот инструмент снова. Он есть и в "
		"меню чата: «Статистика чата».",
	},
	{
		"lng_oblivion_hub_h_online",
		"Откройте чат с человеком и выберите этот инструмент снова. Он "
		"есть и в меню чата: «Журнал онлайна».",
	},
	{
		"lng_oblivion_hub_h_profile_history",
		"Откройте чат и выберите этот инструмент снова. Он есть и в "
		"меню чата: «История профиля».",
	},
	{
		"lng_oblivion_hub_h_rooms_off",
		"Комнаты выключены. Включите их: Настройки → Oblivion → "
		"«Вместе».",
	},
	// Oblivion looks end.
};

[[nodiscard]] bool IsRussianCode(QString code) {
	code = code.trimmed().toLower().replace('_', '-');
	if (code.startsWith('-')) {
		code = code.mid(1);
	}
	return (code == u"ru"_q) || code.startsWith(u"ru-"_q);
}

} // namespace

const std::vector<LangOverride> &RussianStrings() {
	static const auto result = [] {
		auto list = std::vector<LangOverride>();
		const auto append = [&](const auto &rows) {
			list.insert(end(list), std::begin(rows), std::end(rows));
		};
		append(kCommon);
		append(kGhost);
		append(kSaving);
		append(kProtected);
		append(kVoice);
		append(kInterface);
		append(kAppIcon);
		append(kLocalNames);
		append(kTools);
		append(kExtras);
		append(kRound4);
		append(kRound5);
		append(kLooks);
		return list;
	}();
	return result;
}

bool IsRussianLanguage(
		const QString &id,
		const QString &baseId,
		const QString &pluralId,
		const QString &nativeName) {
	return IsRussianCode(id)
		|| IsRussianCode(baseId)
		|| ((id == Lang::CustomLanguageId()) && IsRussianCode(pluralId))
		|| !nativeName.trimmed().compare(u"Русский"_q, Qt::CaseInsensitive);
}

bool CurrentLanguageIsRussian() {
	const auto &lang = Lang::GetInstance();
	return IsRussianLanguage(
		lang.id(),
		lang.baseId(),
		QString(),
		lang.nativeName());
}

} // namespace Oblivion
