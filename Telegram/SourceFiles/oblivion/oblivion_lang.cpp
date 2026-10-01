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
	{ "lng_oblivion_photo_param_color2", "Цвет светов" },
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
		"lng_oblivion_lottie_issue_masks",
		"Использует маски — они замедляют стикер.",
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
