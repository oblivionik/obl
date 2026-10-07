/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_hub.h"

#include "base/platform/base_platform_info.h"
#include "core/version.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "dialogs/dialogs_key.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "oblivion/oblivion_app_icon.h"
#include "oblivion/oblivion_chat_stats_ui.h"
#include "oblivion/oblivion_cloud_share.h"
#include "oblivion/oblivion_cloud_social.h"
#include "oblivion/oblivion_cloud_update.h"
#include "oblivion/oblivion_deleted.h"
#include "oblivion/oblivion_gift_catalog.h"
#include "oblivion/oblivion_look.h"
#include "oblivion/oblivion_look_ui.h"
#include "oblivion/oblivion_lottie_editor.h"
#include "oblivion/oblivion_music_editor.h"
#include "oblivion/oblivion_online.h"
#include "oblivion/oblivion_photo_integration.h"
#include "oblivion/oblivion_playlists.h"
#include "oblivion/oblivion_profile_history.h"
#include "oblivion/oblivion_room.h"
#include "oblivion/oblivion_room_window.h"
#include "oblivion/oblivion_round_video.h"
#include "oblivion/oblivion_send_online.h"
#include "oblivion/oblivion_settings.h"
#include "oblivion/oblivion_sticker_batch.h"
#include "oblivion/oblivion_sticker_packs.h"
#include "oblivion/oblivion_sticker_studio.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "oblivion/oblivion_video_editor.h"
#include "oblivion/oblivion_vision.h"
#include "oblivion/oblivion_vision_ui.h"
#include "oblivion/oblivion_voice_changer.h"
#include "settings/sections/settings_oblivion.h"
#include "settings/settings_common.h"
#include "ui/effects/animation_value.h"
#include "ui/layers/box_content.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_basic.h"
#include "styles/style_dialogs.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"
#include "styles/style_window.h"

#include <QtCore/QMimeData>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainterPath>

namespace Oblivion::Hub {
namespace {

// Everything is at the 100% scale.
constexpr auto kMaxWidth = 840;
constexpr auto kTileMinWidth = 212;
constexpr auto kListTwoColumns = 560;
constexpr auto kWideHeader = 520;
constexpr auto kSearchWidth = 250;
constexpr auto kPlainRadius = 10;
constexpr auto kEmptyHeight = 140;
constexpr auto kMaxMembers = 5;
constexpr auto kLetterSpacing = 0.11; // Of the font size, as in the mockups.

enum class Id {
	PhotoEditor,
	Collage,
	VideoEditor,
	StickerStudio,
	StickerPacks,
	StickerConverter,
	StickerBatch,
	LottieEditor,
	MusicEditor,
	VoiceChanger,
	RoundVideo,
	TextFromImage,
	Cutout,

	Rooms,
	ListenTogether,
	Friends,
	MyProfile,
	Playlists,
	SharedPlaylists,
	SharedPresets,
	SendWhenOnline,

	ChatStats,
	Deleted,
	OnlineJournal,
	ProfileHistory,
	GiftCatalog,

	Ghost,
	Theme,
	AppIcon,
	Cloud,
	Updates,
	AllSettings,
};

enum class Group {
	Create,
	Together,
	Know,
	App,
};

struct Tool {
	Id id = Id();
	const char *name = "";
	Group group = Group::Create;
	tr::phrase<> title;
	tr::phrase<> about;
	const style::icon *icon = nullptr;
	int hue = 0; // Of the icon, as in the mockups.
	const char *keywords = ""; // For the search, besides the texts.
	bool fresh = false; // «НОВОЕ».
};

[[nodiscard]] const std::vector<Tool> &Tools() {
	static const auto result = std::vector<Tool>{
		{
			Id::PhotoEditor,
			"photo_editor",
			Group::Create,
			tr::lng_oblivion_photo_io_tools,
			tr::lng_oblivion_hub_s_photo_editor,
			&st::menuIconPhoto,
			205,
			"photo image picture layers filters фото картинка фильтры",
		},
		{
			Id::Collage,
			"collage",
			Group::Create,
			tr::lng_oblivion_photo_collage_settings,
			tr::lng_oblivion_hub_s_collage,
			&st::menuIconPhotoSet,
			222,
			"collage grid photo коллаж сетка",
		},
		{
			Id::VideoEditor,
			"video_editor",
			Group::Create,
			tr::lng_oblivion_tools_video_editor,
			tr::lng_oblivion_hub_s_video_editor,
			&st::menuIconVideoChat,
			262,
			"video trim gif effects видео гиф",
		},
		{
			Id::StickerStudio,
			"sticker_studio",
			Group::Create,
			tr::lng_oblivion_hub_t_sticker_studio,
			tr::lng_oblivion_hub_s_sticker_studio,
			&st::menuIconStickerCreate,
			32,
			"sticker tgs lottie hue editor стикер редактор оттенок",
		},
		{
			Id::StickerPacks,
			"sticker_packs",
			Group::Create,
			tr::lng_oblivion_tools_sticker_packs,
			tr::lng_oblivion_hub_s_sticker_packs,
			&st::menuIconStickerAdd,
			18,
			"sticker pack set стикер стикерпак набор",
		},
		{
			Id::StickerConverter,
			"sticker_converter",
			Group::Create,
			tr::lng_oblivion_tools_sticker_converter,
			tr::lng_oblivion_hub_s_sticker_converter,
			&st::menuIconStickers,
			55,
			"sticker converter webm webp png стикер видеостикер",
		},
		{
			Id::StickerBatch,
			"sticker_batch",
			Group::Create,
			tr::lng_oblivion_hub_t_sticker_batch,
			tr::lng_oblivion_hub_s_sticker_batch,
			&st::menuIconShowAll,
			5,
			"sticker emoji batch bulk стикер эмодзи пакетное массовое",
		},
		{
			Id::LottieEditor,
			"lottie_editor",
			Group::Create,
			tr::lng_oblivion_hub_t_lottie,
			tr::lng_oblivion_hub_s_lottie,
			&st::menuIconDraw,
			150,
			"lottie tgs animation keyframes лотти анимация кадры",
		},
		{
			Id::MusicEditor,
			"music_editor",
			Group::Create,
			tr::lng_oblivion_tools_music_editor,
			tr::lng_oblivion_hub_s_music_editor,
			&st::menuIconSoundSelect,
			332,
			"music audio slowed pitch tempo музыка аудио замедление",
		},
		{
			Id::VoiceChanger,
			"voice_changer",
			Group::Create,
			tr::lng_oblivion_hub_t_voice,
			tr::lng_oblivion_hub_s_voice,
			&st::menuIconSoundAdd,
			285,
			"voice changer effect голос голосовые эффект",
		},
		{
			Id::RoundVideo,
			"round_video",
			Group::Create,
			tr::lng_oblivion_hub_t_round,
			tr::lng_oblivion_hub_s_round,
			&st::menuIconGif,
			190,
			"round video message кружок кружочек видеосообщение",
		},
		{
			Id::TextFromImage,
			"text_from_image",
			Group::Create,
			tr::lng_oblivion_hub_t_ocr,
			tr::lng_oblivion_hub_s_ocr,
			&st::menuIconFont,
			48,
			"ocr text recognize photo распознавание текст фото",
		},
		{
			Id::Cutout,
			"cutout",
			Group::Create,
			tr::lng_oblivion_hub_t_cutout,
			tr::lng_oblivion_hub_s_cutout,
			&st::menuIconTransparent,
			12,
			"cutout background remove object фон вырезать объект",
		},

		{
			Id::Rooms,
			"rooms",
			Group::Together,
			tr::lng_oblivion_room_settings,
			tr::lng_oblivion_hub_s_rooms,
			&st::menuIconGroups,
			270,
			"rooms watch listen draw together комната вместе",
			true,
		},
		{
			Id::ListenTogether,
			"listen_together",
			Group::Together,
			tr::lng_oblivion_hub_t_listen,
			tr::lng_oblivion_hub_s_listen,
			&st::menuIconSoundOn,
			200,
			"listen together music слушать музыка",
		},
		{
			Id::Friends,
			"friends",
			Group::Together,
			tr::lng_oblivion_social_friends,
			tr::lng_oblivion_hub_s_friends,
			&st::menuIconRatingUsers,
			160,
			"friends activity друзья активность",
			true,
		},
		{
			Id::MyProfile,
			"my_profile",
			Group::Together,
			tr::lng_oblivion_social_my_profile,
			tr::lng_oblivion_hub_s_my_profile,
			&st::menuIconProfile,
			185,
			"profile status color профиль статус цвет",
		},
		{
			Id::Playlists,
			"playlists",
			Group::Together,
			tr::lng_oblivion_tools_playlists,
			tr::lng_oblivion_hub_s_playlists,
			&st::menuIconReorder,
			340,
			"playlists music плейлист музыка",
		},
		{
			Id::SharedPlaylists,
			"shared_playlists",
			Group::Together,
			tr::lng_oblivion_share_library,
			tr::lng_oblivion_hub_s_shared_playlists,
			&st::menuIconShare,
			320,
			"share playlists link поделиться ссылка",
		},
		{
			Id::SharedPresets,
			"shared_presets",
			Group::Together,
			tr::lng_oblivion_share_gallery_title,
			tr::lng_oblivion_hub_s_presets,
			&st::menuIconPalette,
			28,
			"presets effects gallery share пресеты эффекты галерея",
		},
		{
			Id::SendWhenOnline,
			"send_when_online",
			Group::Together,
			tr::lng_oblivion_hub_t_send_online,
			tr::lng_oblivion_hub_s_send_online,
			&st::menuIconWhenOnline,
			225,
			"send later online waiting отправить позже ожидают",
		},

		{
			Id::ChatStats,
			"chat_stats",
			Group::Know,
			tr::lng_oblivion_hub_t_chat_stats,
			tr::lng_oblivion_hub_s_chat_stats,
			&st::menuIconStats,
			212,
			"statistics stats chart export html статистика экспорт",
		},
		{
			Id::Deleted,
			"deleted",
			Group::Know,
			tr::lng_oblivion_deleted_title,
			tr::lng_oblivion_hub_s_deleted,
			&st::menuIconDelete,
			355,
			"deleted messages saved search удаленные",
		},
		{
			Id::OnlineJournal,
			"online_journal",
			Group::Know,
			tr::lng_oblivion_hub_t_online,
			tr::lng_oblivion_hub_s_online,
			&st::menuIconNotifications,
			42,
			"online journal last seen онлайн в сети заходил",
		},
		{
			Id::ProfileHistory,
			"profile_history",
			Group::Know,
			tr::lng_oblivion_hub_t_profile_history,
			tr::lng_oblivion_hub_s_profile_history,
			&st::menuIconRestore,
			175,
			"profile history names photos bio история имя",
		},
		{
			Id::GiftCatalog,
			"gift_catalog",
			Group::Know,
			tr::lng_oblivion_tools_gift_catalog,
			tr::lng_oblivion_hub_s_gift_catalog,
			&st::menuIconGiftPremium,
			310,
			"gifts catalog models nft подарки модели",
		},

		{
			Id::Ghost,
			"ghost",
			Group::App,
			tr::lng_oblivion_hub_t_ghost,
			tr::lng_oblivion_hub_s_ghost,
			&st::menuIconStealth,
			250,
			"ghost stealth read typing призрак невидимка прочтение",
		},
		{
			Id::Theme,
			"theme",
			Group::App,
			tr::lng_oblivion_look_title,
			tr::lng_oblivion_hub_s_look,
			&st::menuIconChangeColors,
			292,
			"theme look design colors тема оформление дизайн цвета",
			true,
		},
		{
			Id::AppIcon,
			"app_icon",
			Group::App,
			tr::lng_oblivion_app_icon,
			tr::lng_oblivion_hub_s_app_icon,
			&st::menuIconCustomize,
			275,
			"icon dock app иконка значок",
		},
		{
			Id::Cloud,
			"cloud",
			Group::App,
			tr::lng_oblivion_cloud_section,
			tr::lng_oblivion_hub_s_cloud,
			&st::menuIconNetwork,
			198,
			"cloud server badge sync облако сервер значок",
		},
		{
			Id::Updates,
			"updates",
			Group::App,
			tr::lng_oblivion_hub_t_updates,
			tr::lng_oblivion_hub_s_updates,
			&st::menuIconDownload,
			142,
			"update version обновление версия",
		},
		{
			Id::AllSettings,
			"settings",
			Group::App,
			tr::lng_oblivion_hub_t_settings,
			tr::lng_oblivion_hub_s_settings,
			&st::menuIconSettings,
			215,
			"settings options настройки параметры",
		},
	};
	return result;
}

// Some tools exist only where the system can do the work.
[[nodiscard]] bool Available(Id id) {
	switch (id) {
	case Id::AppIcon: return Platform::IsMac();
	case Id::TextFromImage: return Vision::TextRecognitionSupported();
	case Id::Cutout: return Vision::BackgroundRemovalSupported();
	default: return true;
	}
}

// What the screen shows of a tool.
struct Entry {
	Id id = Id();
	int group = 0;
	QString title;
	QString about;
	QString key; // SearchKey() of everything the tool is found by.
	const style::icon *icon = nullptr;
	int hue = 0;
	bool fresh = false;
};

[[nodiscard]] std::vector<Entry> BuildEntries() {
	auto result = std::vector<Entry>();
	for (const auto &tool : Tools()) {
		if (!Available(tool.id)) {
			continue;
		}
		auto entry = Entry{
			.id = tool.id,
			.group = int(tool.group),
			.title = tool.title(tr::now),
			.about = tool.about(tr::now),
			.icon = tool.icon,
			.hue = tool.hue,
			.fresh = tool.fresh,
		};
		entry.key = SearchKey(entry.title
			+ ' '
			+ entry.about
			+ ' '
			+ QString::fromUtf8(tool.keywords));
		result.push_back(std::move(entry));
	}
	return result;
}

// The room the account has open, for the banner.
struct LiveRoom {
	struct Member {
		uint64 id = 0;
		QString name;
	};
	QString code;
	QString title;
	QString playing; // Empty: nothing plays.
	QString seed; // Of the cover that is painted while there is no image.
	QImage cover;
	std::vector<Member> members; // Who is there now, a few of them.
};

// Everything the box shows and does, it can be created without a window
// (see oblivion_ui_snapshots.h).
struct BoxArgs {
	std::shared_ptr<Ui::Show> show;
	std::vector<Entry> entries;
	rpl::producer<std::optional<LiveRoom>> live;
	// Not empty: the tool can't be opened from here now, the text says
	// how to reach it. The screen stays.
	Fn<QString(Id)> hint;
	Fn<void(Id)> open;
	Fn<void(QString code)> join;
	QString query;
};

[[nodiscard]] int Scaled(int value) {
	return style::ConvertScale(value);
}

[[nodiscard]] style::font MakeFont(int size, bool semibold) {
	const auto &from = semibold ? st::semiboldFont : st::normalFont;
	return style::font(Scaled(size), from->flags(), from->family());
}

[[nodiscard]] QFont SpacedFont(const style::font &font) {
	auto result = font->f;
	result.setLetterSpacing(
		QFont::AbsoluteSpacing,
		font->f.pixelSize() * kLetterSpacing);
	return result;
}

[[nodiscard]] int SpacedWidth(const style::font &font, const QString &text) {
	return int(std::ceil(
		QFontMetricsF(SpacedFont(font)).horizontalAdvance(text)));
}

// Draws with the pen that is set.
void DrawSpaced(
		QPainter &p,
		const style::font &font,
		int x,
		int baseline,
		const QString &text) {
	p.setFont(SpacedFont(font));
	p.drawText(x, baseline, text);
}

[[nodiscard]] QColor Hsl(int hue, float64 saturation, float64 lightness) {
	return QColor::fromHslF(
		(((hue % 360) + 360) % 360) / 360.,
		saturation,
		lightness);
}

[[nodiscard]] QString GroupName(int group) {
	switch (Group(group)) {
	case Group::Create: return tr::lng_oblivion_hub_group_create(tr::now);
	case Group::Together:
		return tr::lng_oblivion_hub_group_together(tr::now);
	case Group::Know: return tr::lng_oblivion_hub_group_know(tr::now);
	case Group::App: return tr::lng_oblivion_hub_group_app(tr::now);
	}
	return QString();
}

[[nodiscard]] bool GlowLook() {
	return Look::Is(Look::kNightAir);
}

[[nodiscard]] Metrics MetricsFor(int width, bool banner) {
	const auto list = !Look::HasCards();
	const auto glow = GlowLook();
	auto result = Metrics();
	result.width = width;
	result.padding = Scaled(list ? 28 : glow ? 26 : 24);
	result.gap = Scaled(list ? 36 : 10);
	result.rowGap = list ? 0 : Scaled(10);
	result.tile = Scaled(list ? 47 : glow ? 62 : 64);
	result.label = Scaled(list ? 46 : 36);
	result.groupSkip = Scaled(list ? 14 : 6);
	const auto inner = width - 2 * result.padding;
	result.columns = list
		? ((inner >= Scaled(kListTwoColumns)) ? 2 : 1)
		: std::clamp(
			(inner + result.gap) / (Scaled(kTileMinWidth) + result.gap),
			1,
			3);
	result.list = list;
	result.banner = banner ? Scaled(glow ? 92 : 76) : 0;
	result.bannerSkip = Scaled(list ? 10 : 4);
	result.bottom = Scaled(list ? 62 : 24);
	return result;
}

[[nodiscard]] const style::InputField &SearchStyle() {
	static const auto result = [] {
		auto st = st::dialogsFilter;
		// The magnifier is painted over the left edge.
		st.textMargins = QMargins(
			Scaled(34),
			st.textMargins.top(),
			Scaled(12),
			st.textMargins.bottom());
		return st;
	}();
	return result;
}

// The box is the page itself: no title, no buttons row.
[[nodiscard]] const style::Box &BoxStyle() {
	static const auto result = [] {
		auto st = st::layerBox;
		st.buttonPadding = QMargins();
		st.buttonHeight = 0;
		st.shadowIgnoreTopSkip = true;
		st.shadowIgnoreBottomSkip = true;
		return st;
	}();
	return result;
}

[[nodiscard]] int BoxWidth(const std::shared_ptr<Ui::Show> &show) {
	const auto wide = Scaled(kMaxWidth);
	const auto shadow = st::boxRoundShadow.extend;
	const auto available = (show && show->valid())
		? (show->toastParent()->width() - shadow.left() - shadow.right())
		: 0;
	return (available > 0)
		? std::clamp(available, int(st::boxWideWidth), wide)
		: wide;
}

class Inner final : public Ui::RpWidget {
public:
	Inner(
		QWidget *parent,
		std::vector<Entry> entries,
		Fn<void(int index)> chosen,
		Fn<void()> join);

	void setLive(std::optional<LiveRoom> live);
	void setQuery(const QString &query);
	void lookChanged();
	// Enter in the search field.
	void chooseFirst();
	// With every tool shown.
	[[nodiscard]] int fullHeight(int width) const;

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	static constexpr auto kNothing = -1;
	static constexpr auto kBanner = -2;

	void refreshFonts();
	void refilter();
	void relayout();
	[[nodiscard]] std::array<int, kGroups> counts() const;
	[[nodiscard]] int lookup(QPoint point) const;
	[[nodiscard]] QRect itemRect(int item) const;
	void setOver(int over);
	void activate(int item);
	[[nodiscard]] const Entry *entryFor(const Placed::Tile &tile) const;

	void paintBanner(QPainter &p);
	void paintLabel(QPainter &p, const Placed::Label &label);
	void paintTile(
		QPainter &p,
		const Placed::Tile &tile,
		const Entry &entry,
		bool active);
	void paintRow(
		QPainter &p,
		const Placed::Tile &tile,
		const Entry &entry,
		bool active);
	void paintTag(QPainter &p, int x, int top, int lineHeight);
	void paintFooter(QPainter &p);

	const std::vector<Entry> _entries;
	const Fn<void(int index)> _chosen;
	const Fn<void()> _join;
	std::optional<LiveRoom> _live;
	QString _query;
	std::array<std::vector<int>, kGroups> _shown; // Indexes in _entries.
	Metrics _metrics;
	Placed _placed;
	int _over = kNothing;
	int _pressed = kNothing;

	style::font _titleFont;
	style::font _aboutFont;
	style::font _labelFont;
	style::font _tagFont;
	style::font _numberFont;
	style::font _liveLabelFont;
	style::font _liveTitleFont;
	QString _tag;
	int _tagWidth = 0;

};

Inner::Inner(
	QWidget *parent,
	std::vector<Entry> entries,
	Fn<void(int index)> chosen,
	Fn<void()> join)
: RpWidget(parent)
, _entries(std::move(entries))
, _chosen(std::move(chosen))
, _join(std::move(join)) {
	setMouseTracking(true);
	refreshFonts();
	refilter();
}

void Inner::refreshFonts() {
	const auto list = !Look::HasCards();
	const auto caps = Look::CapsLabels();
	_titleFont = list ? MakeFont(16, false) : MakeFont(13, true);
	_aboutFont = MakeFont(list ? 13 : 12, false);
	_labelFont = MakeFont(caps ? 11 : 12, true);
	_tagFont = MakeFont(10, true);
	_numberFont = MakeFont(12, false);
	_liveLabelFont = MakeFont(10, true);
	_liveTitleFont = MakeFont(GlowLook() ? 18 : 16, true);
	_tag = tr::lng_oblivion_hub_new(tr::now).toUpper();
	_tagWidth = SpacedWidth(_tagFont, _tag) + 2 * Scaled(6);
}

void Inner::refilter() {
	for (auto &list : _shown) {
		list.clear();
	}
	for (auto i = 0; i != int(_entries.size()); ++i) {
		const auto &entry = _entries[i];
		if (entry.group >= 0
			&& entry.group < kGroups
			&& SearchMatches(entry.key, _query)) {
			_shown[entry.group].push_back(i);
		}
	}
}

std::array<int, kGroups> Inner::counts() const {
	auto result = std::array<int, kGroups>();
	for (auto i = 0; i != kGroups; ++i) {
		result[i] = int(_shown[i].size());
	}
	return result;
}

void Inner::relayout() {
	_over = kNothing;
	_pressed = kNothing;
	setCursor(style::cur_default);
	if (width() > 0) {
		resizeToWidth(width());
	}
	update();
}

void Inner::setLive(std::optional<LiveRoom> live) {
	const auto placed = (_live.has_value() == live.has_value());
	_live = std::move(live);
	if (placed) {
		// Only what the banner says has changed.
		update(_placed.banner);
	} else {
		relayout();
	}
}

void Inner::setQuery(const QString &query) {
	const auto key = SearchKey(query);
	if (_query == key) {
		return;
	}
	_query = key;
	refilter();
	relayout();
}

void Inner::lookChanged() {
	refreshFonts();
	relayout();
}

int Inner::fullHeight(int width) const {
	auto all = std::array<int, kGroups>();
	for (const auto &entry : _entries) {
		if (entry.group >= 0 && entry.group < kGroups) {
			++all[entry.group];
		}
	}
	return Place(all, MetricsFor(width, _live.has_value())).height;
}

int Inner::resizeGetHeight(int newWidth) {
	_metrics = MetricsFor(newWidth, _live.has_value());
	_placed = Place(counts(), _metrics);
	return _placed.tiles.empty()
		? std::max(_placed.height, Scaled(kEmptyHeight))
		: _placed.height;
}

const Entry *Inner::entryFor(const Placed::Tile &tile) const {
	if (tile.group < 0 || tile.group >= kGroups) {
		return nullptr;
	}
	const auto &list = _shown[tile.group];
	return (tile.index >= 0 && tile.index < int(list.size()))
		? &_entries[list[tile.index]]
		: nullptr;
}

int Inner::lookup(QPoint point) const {
	if (_live && _placed.banner.contains(point)) {
		return kBanner;
	}
	for (auto i = 0; i != int(_placed.tiles.size()); ++i) {
		if (_placed.tiles[i].rect.contains(point)) {
			return i;
		}
	}
	return kNothing;
}

QRect Inner::itemRect(int item) const {
	if (item == kBanner) {
		return _placed.banner;
	} else if (item < 0 || item >= int(_placed.tiles.size())) {
		return QRect();
	}
	// A hovered row is painted wider than it is, see paintRow().
	const auto extend = Scaled(12);
	return _placed.tiles[item].rect.marginsAdded({ extend, 0, extend, 0 });
}

void Inner::setOver(int over) {
	if (_over == over) {
		return;
	}
	update(itemRect(_over));
	_over = over;
	update(itemRect(_over));
	setCursor((_over != kNothing) ? style::cur_pointer : style::cur_default);
}

void Inner::activate(int item) {
	if (item == kBanner) {
		// The callbacks may close the box with this widget in it.
		if (const auto join = _join) {
			join();
		}
		return;
	} else if (item < 0 || item >= int(_placed.tiles.size())) {
		return;
	}
	const auto &tile = _placed.tiles[item];
	const auto &list = _shown[tile.group];
	if (tile.index < 0 || tile.index >= int(list.size())) {
		return;
	}
	const auto index = list[tile.index];
	if (const auto chosen = _chosen) {
		chosen(index);
	}
}

void Inner::chooseFirst() {
	if (!_placed.tiles.empty()) {
		activate(0);
	}
}

void Inner::mouseMoveEvent(QMouseEvent *e) {
	setOver(lookup(e->pos()));
}

void Inner::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	setOver(lookup(e->pos()));
	_pressed = _over;
}

void Inner::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto pressed = std::exchange(_pressed, kNothing);
	setOver(lookup(e->pos()));
	if (pressed != kNothing && pressed == _over) {
		// Not from inside of the event: the click may destroy the box.
		crl::on_main(this, [=] {
			activate(pressed);
		});
	}
}

void Inner::leaveEventHook(QEvent *e) {
	setOver(kNothing);
}

void Inner::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto clip = e->rect();

	if (_live && clip.intersects(_placed.banner)) {
		paintBanner(p);
	}
	if (_placed.tiles.empty()) {
		p.setFont(st::normalFont);
		p.setPen(Look::Color(Look::Role::SubText, st::windowSubTextFg->c));
		p.drawText(
			QRect(
				_metrics.padding,
				_placed.banner.isEmpty()
					? 0
					: (_placed.banner.y() + _placed.banner.height()),
				width() - 2 * _metrics.padding,
				Scaled(kEmptyHeight) - Scaled(40)),
			tr::lng_oblivion_hub_empty(tr::now),
			style::al_center);
		return;
	}
	if (_metrics.list && !_placed.labels.empty()) {
		// .d3 .h-cols: a line of the text colour above the columns.
		const auto line = std::max(Scaled(1), 1);
		p.fillRect(
			_metrics.padding,
			_placed.labels.front().rect.y(),
			width() - 2 * _metrics.padding,
			line,
			Look::Color(Look::Role::Text, st::windowFg->c));
	}
	for (const auto &label : _placed.labels) {
		if (clip.intersects(label.rect)) {
			paintLabel(p, label);
		}
	}
	for (auto i = 0; i != int(_placed.tiles.size()); ++i) {
		const auto &tile = _placed.tiles[i];
		if (!clip.intersects(itemRect(i))) {
			continue;
		}
		const auto entry = entryFor(tile);
		if (!entry) {
			continue;
		}
		const auto active = (_over == i);
		if (_metrics.list) {
			paintRow(p, tile, *entry, active);
		} else {
			paintTile(p, tile, *entry, active);
		}
	}
	if (_metrics.list) {
		paintFooter(p);
	}
}

void Inner::paintLabel(QPainter &p, const Placed::Label &label) {
	const auto caps = Look::CapsLabels();
	auto text = GroupName(label.group);
	if (caps) {
		text = text.toUpper();
	}
	const auto count = (label.group >= 0 && label.group < kGroups)
		? QString::number(int(_shown[label.group].size()))
		: QString();
	const auto &font = _labelFont;
	const auto r = label.rect;
	const auto baseline = _metrics.list
		? (r.y() + r.height() - Scaled(10) - font->descent)
		: (r.y() + (r.height() - font->height) / 2 + font->ascent);
	const auto width = caps ? SpacedWidth(font, text) : font->width(text);
	const auto draw = [&](int x, const QString &value) {
		if (caps) {
			DrawSpaced(p, font, x, baseline, value);
		} else {
			p.setFont(font);
			p.drawText(x, baseline, value);
		}
	};
	p.setPen(Look::Color(Look::Role::SubText, st::windowSubTextFg->c));
	draw(r.x(), text);
	p.setPen(Look::Color(
		Look::Role::FaintText,
		anim::with_alpha(st::windowSubTextFg->c, 0.6)));
	draw(r.x() + width + Scaled(8), count);
	if (_metrics.list) {
		const auto line = std::max(Scaled(1), 1);
		Look::PaintDivider(
			p,
			QRectF(r.x(), r.y() + r.height() - line, r.width(), line),
			st::shadowFg->c);
	}
}

void Inner::paintTag(QPainter &p, int x, int top, int lineHeight) {
	const auto height = _tagFont->height + Scaled(2);
	const auto rect = QRectF(
		x,
		top + (lineHeight - height) / 2.,
		_tagWidth,
		height);
	const auto accent = st::windowActiveTextFg->c;
	p.setPen(Qt::NoPen);
	auto text = QColor();
	if (!Look::HasCards()) {
		// .d3 .tile.hot: the loud fill.
		const auto radius = Scaled(3);
		p.setBrush(Look::Color(Look::Role::Highlight));
		p.drawRoundedRect(rect, radius, radius);
		text = Look::Color(Look::Role::OnHighlight);
	} else if (GlowLook()) {
		Look::PaintAccentGradient(p, rect, Scaled(5), accent);
		text = Look::Color(Look::Role::OnAccent, st::windowFgActive->c);
	} else {
		const auto radius = Scaled(5);
		p.setBrush(Look::Color(
			Look::Role::Tint,
			anim::with_alpha(accent, 0.14)));
		p.drawRoundedRect(rect, radius, radius);
		text = Look::Color(Look::Role::Accent, accent);
	}
	p.setPen(text);
	DrawSpaced(
		p,
		_tagFont,
		int(rect.x()) + Scaled(6),
		int(rect.y()) + (height - _tagFont->height) / 2 + _tagFont->ascent,
		_tag);
}

void Inner::paintTile(
		QPainter &p,
		const Placed::Tile &tile,
		const Entry &entry,
		bool active) {
	const auto glow = GlowLook();
	const auto r = QRectF(tile.rect);
	const auto radius = Look::TileRadius(Scaled(kPlainRadius));
	p.setPen(Qt::NoPen);
	p.setBrush(Look::Color(Look::Role::Card, st::windowBgOver->c));
	p.drawRoundedRect(r, radius, radius);
	const auto stroke = Look::Color(Look::Role::CardStroke, QColor(0, 0, 0, 0));
	if (stroke.alpha() > 0) {
		const auto line = style::ConvertScaleExact(1.);
		const auto half = line / 2.;
		p.setPen(QPen(stroke, line));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(
			r.marginsRemoved({ half, half, half, half }),
			std::max(radius - half, 0.),
			std::max(radius - half, 0.));
	}
	if (active) {
		Look::PaintSelected(p, r, st::windowBgRipple->c, radius);
	}

	// The icon: .t-ic of the mockups, a glyph on a gradient of the hue.
	const auto side = Scaled(glow ? 40 : 38);
	const auto icon = QRect(
		tile.rect.x() + Scaled(glow ? 10 : 13),
		tile.rect.y() + (tile.rect.height() - side) / 2,
		side,
		side);
	auto gradient = glow
		? QLinearGradient(icon.topLeft(), icon.bottomRight())
		: QLinearGradient(icon.topLeft(), icon.bottomLeft());
	gradient.setColorAt(
		0.,
		Hsl(entry.hue, glow ? 0.92 : 0.72, glow ? 0.66 : 0.62));
	gradient.setColorAt(
		1.,
		Hsl(
			entry.hue + (glow ? 48 : 14),
			glow ? 0.88 : 0.66,
			glow ? 0.58 : 0.50));
	const auto iconRadius = Scaled(glow ? 14 : 11);
	p.setPen(Qt::NoPen);
	p.setBrush(gradient);
	p.drawRoundedRect(icon, iconRadius, iconRadius);
	if (entry.icon) {
		entry.icon->paintInCenter(p, icon, QColor(255, 255, 255));
	}

	const auto left = icon.x() + side + Scaled(12);
	const auto available = tile.rect.x()
		+ tile.rect.width()
		- Scaled(12)
		- left;
	if (available <= 0) {
		return;
	}
	const auto skip = Scaled(1);
	const auto lines = _titleFont->height + skip + _aboutFont->height;
	const auto top = tile.rect.y() + (tile.rect.height() - lines) / 2;
	const auto tagSkip = Scaled(7);
	const auto titleWidth = entry.fresh
		? std::max(available - _tagWidth - tagSkip, 0)
		: available;
	const auto title = _titleFont->elided(entry.title, titleWidth);
	p.setFont(_titleFont);
	p.setPen(Look::Color(Look::Role::Text, st::windowFg->c));
	p.drawText(left, top + _titleFont->ascent, title);
	if (entry.fresh && titleWidth > 0) {
		paintTag(
			p,
			left + _titleFont->width(title) + tagSkip,
			top,
			_titleFont->height);
	}
	p.setFont(_aboutFont);
	p.setPen(Look::Color(Look::Role::SubText, st::windowSubTextFg->c));
	p.drawText(
		left,
		top + _titleFont->height + skip + _aboutFont->ascent,
		_aboutFont->elided(entry.about, available));
}

void Inner::paintRow(
		QPainter &p,
		const Placed::Tile &tile,
		const Entry &entry,
		bool active) {
	// .d3 .tile: the number, the name, the description at the right and
	// a hairline under the row.
	const auto r = tile.rect;
	if (active) {
		// Wider than the row, the bar at the left does not touch the number.
		const auto extend = Scaled(12);
		Look::PaintSelected(
			p,
			QRectF(r.marginsAdded({ extend, 0, extend, 0 })),
			st::windowBgOver->c,
			0);
	}
	const auto line = std::max(Scaled(1), 1);
	Look::PaintDivider(
		p,
		QRectF(r.x(), r.y() + r.height() - line, r.width(), line),
		st::shadowFg->c);

	const auto top = r.y() + (r.height() - _titleFont->height) / 2;
	const auto baseline = top + _titleFont->ascent;
	p.setFont(_numberFont);
	p.setPen(Look::Color(
		Look::Role::FaintText,
		anim::with_alpha(st::windowSubTextFg->c, 0.6)));
	p.drawText(
		r.x(),
		baseline,
		u"%1"_q.arg(tile.number, 2, 10, QChar('0')));

	const auto left = r.x() + Scaled(34);
	const auto available = r.x() + r.width() - left;
	if (available <= 0) {
		return;
	}
	const auto tagSkip = Scaled(8);
	const auto titleLimit = entry.fresh
		? std::max(available - _tagWidth - tagSkip, 0)
		: available;
	const auto title = _titleFont->elided(entry.title, titleLimit);
	auto used = _titleFont->width(title);
	p.setFont(_titleFont);
	p.setPen(Look::Color(Look::Role::Text, st::windowFg->c));
	p.drawText(left, baseline, title);
	if (entry.fresh && titleLimit > 0) {
		paintTag(p, left + used + tagSkip, top, _titleFont->height);
		used += tagSkip + _tagWidth;
	}
	const auto aboutLimit = available - used - Scaled(14);
	if (aboutLimit >= Scaled(60)) {
		const auto about = _aboutFont->elided(entry.about, aboutLimit);
		p.setFont(_aboutFont);
		p.setPen(Look::Color(Look::Role::SubText, st::windowSubTextFg->c));
		p.drawText(
			r.x() + r.width() - _aboutFont->width(about),
			baseline,
			about);
	}
}

void Inner::paintFooter(QPainter &p) {
	// .d3 .h-foot.
	auto total = 0;
	for (const auto &list : _shown) {
		total += int(list.size());
	}
	const auto text = (u"Oblivion "_q
		+ QString::fromLatin1(AppVersionStr)
		+ u"      "_q
		+ tr::lng_oblivion_hub_footer(tr::now, lt_count, total)).toUpper();
	p.setPen(Look::Color(
		Look::Role::FaintText,
		anim::with_alpha(st::windowSubTextFg->c, 0.6)));
	DrawSpaced(
		p,
		_tagFont,
		_metrics.padding,
		_placed.height - Scaled(26) - _tagFont->descent,
		text);
}

void Inner::paintBanner(QPainter &p) {
	const auto &live = *_live;
	const auto list = !Look::HasCards();
	const auto glow = GlowLook();
	const auto whole = _placed.banner;
	const auto r = QRectF(whole);
	const auto accent = st::windowActiveTextFg->c;
	const auto over = (_over == kBanner);
	Look::PaintChip(
		p,
		r,
		anim::with_alpha(accent, over ? 0.18 : 0.12),
		Scaled(12),
		Look::Chip::Live);

	// The cover of what plays.
	const auto inset = Scaled(glow ? 14 : 12);
	const auto side = whole.height() - 2 * inset;
	const auto cover = QRect(
		whole.x() + (list ? 0 : inset),
		whole.y() + inset,
		side,
		side);
	Rooms::PaintCover(
		p,
		cover,
		live.cover,
		live.seed,
		glow ? (side / 2) : int(side * 0.16));

	// «Войти».
	const auto &buttonFont = st::semiboldFont;
	const auto joinText = tr::lng_oblivion_hub_live_join(tr::now);
	const auto buttonHeight = Scaled(34);
	const auto buttonWidth = buttonFont->width(joinText) + 2 * Scaled(18);
	const auto right = whole.x()
		+ whole.width()
		- (list ? 0 : Scaled(glow ? 20 : 14));
	const auto button = QRectF(
		right - buttonWidth,
		whole.y() + (whole.height() - buttonHeight) / 2.,
		buttonWidth,
		buttonHeight);
	if (list) {
		p.setPen(Qt::NoPen);
		p.setBrush(Look::Color(Look::Role::Highlight));
		p.drawRoundedRect(button, buttonHeight / 2., buttonHeight / 2.);
		p.setPen(Look::Color(Look::Role::OnHighlight));
	} else {
		Look::PaintAccentGradient(
			p,
			button,
			buttonHeight / 2.,
			st::activeButtonBg->c);
		p.setPen(Look::Color(Look::Role::OnAccent, st::activeButtonFg->c));
	}
	p.setFont(buttonFont);
	p.drawText(button, joinText, style::al_center);

	// Who is in the room.
	const auto textLeft = cover.x() + side + Scaled(glow ? 16 : 12);
	auto textRight = int(button.x()) - Scaled(16);
	const auto count = std::min(int(live.members.size()), kMaxMembers);
	const auto userpic = Scaled(30);
	const auto step = list ? (userpic + Scaled(4)) : (userpic - Scaled(7));
	const auto stack = count ? (userpic + (count - 1) * step) : 0;
	if (count > 0 && (textRight - stack - textLeft) >= Scaled(170)) {
		const auto x = textRight - stack;
		const auto y = whole.y() + (whole.height() - userpic) / 2;
		const auto ring = Scaled(2);
		const auto ground = Look::Color(Look::Role::Ground, st::boxBg->c);
		for (auto i = 0; i != count; ++i) {
			const auto rect = QRect(x + i * step, y, userpic, userpic);
			if (!list) {
				p.setPen(Qt::NoPen);
				p.setBrush(ground);
				p.drawEllipse(QRectF(rect).marginsAdded(
					QMarginsF(ring, ring, ring, ring)));
			}
			Rooms::PaintUserpic(
				p,
				rect,
				live.members[i].id,
				live.members[i].name);
		}
		textRight -= stack + Scaled(14);
	}

	const auto available = textRight - textLeft;
	if (available <= 0) {
		return;
	}
	const auto skip = Scaled(1);
	const auto lines = _liveLabelFont->height
		+ _liveTitleFont->height
		+ _aboutFont->height
		+ 2 * skip;
	auto top = whole.y() + (whole.height() - lines) / 2;
	const auto dot = Scaled(7);
	p.setPen(Qt::NoPen);
	p.setBrush(Look::Color(Look::Role::Online));
	p.drawEllipse(QRectF(
		textLeft,
		top + (_liveLabelFont->height - dot) / 2.,
		dot,
		dot));
	const auto faint = Look::ChipLabel(
		Look::Chip::Live,
		st::windowSubTextFg->c);
	p.setPen(faint);
	DrawSpaced(
		p,
		_liveLabelFont,
		textLeft + dot + Scaled(6),
		top + _liveLabelFont->ascent,
		tr::lng_oblivion_hub_live(tr::now).toUpper());
	top += _liveLabelFont->height + skip;
	p.setFont(_liveTitleFont);
	p.setPen(Look::ChipText(Look::Chip::Live, st::windowFg->c));
	p.drawText(
		textLeft,
		top + _liveTitleFont->ascent,
		_liveTitleFont->elided(
			(live.title.isEmpty()
				? tr::lng_oblivion_hub_live_untitled(tr::now)
				: live.title),
			available));
	top += _liveTitleFont->height + skip;
	p.setFont(_aboutFont);
	p.setPen(faint);
	p.drawText(
		textLeft,
		top + _aboutFont->ascent,
		_aboutFont->elided(
			(live.playing.isEmpty()
				? tr::lng_oblivion_hub_live_silence(tr::now)
				: live.playing),
			available));
}

class HubBox final : public Ui::BoxContent {
public:
	HubBox(QWidget*, BoxArgs &&args);

protected:
	void prepare() override;
	void setInnerFocus() override;
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	void refreshHeader();
	void placeSearch();
	void refreshDimensions();
	void choose(int index);

	BoxArgs _args;
	const object_ptr<Ui::InputField> _search;
	QPointer<Inner> _inner;
	QString _liveCode;
	style::font _titleFont;
	int _width = 0;
	int _padding = 0;
	int _titleTop = 0;
	int _rowTop = 0;
	int _headerHeight = 0;
	bool _aboutShown = false;

};

HubBox::HubBox(QWidget*, BoxArgs &&args)
: _args(std::move(args))
, _search(
	this,
	SearchStyle(),
	tr::lng_oblivion_hub_search(),
	_args.query) {
}

void HubBox::prepare() {
	_width = BoxWidth(_args.show);
	setStyle(BoxStyle());
	setNoContentMargin(true);
	// The page is painted over the whole box, with its corners.
	setCustomCornersFilling(RectPart::FullTop | RectPart::FullBottom);
	addTopButton(st::boxTitleClose, [=] { closeBox(); });

	refreshHeader();
	_inner = setInnerWidget(
		object_ptr<Inner>(
			this,
			_args.entries,
			[=](int index) { choose(index); },
			[=] {
				const auto join = _args.join;
				const auto code = _liveCode;
				if (join && !code.isEmpty()) {
					join(code);
				}
			}),
		st::boxScroll,
		_headerHeight,
		st::boxRadius); // Nothing scrolls into the round corners.
	_inner->setQuery(_args.query);
	_inner->resizeToWidth(_width);

	// The magnifier inside of the field.
	const auto icon = Ui::CreateChild<Ui::RpWidget>(_search.data());
	icon->setAttribute(Qt::WA_TransparentForMouseEvents);
	icon->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(icon);
		const auto &glyph = st::boxFieldSearchIcon;
		// The icon carries the place it has in a box field.
		glyph.paint(
			p,
			Scaled(1),
			(icon->height() - glyph.height() - Scaled(9)) / 2,
			icon->width());
	}, icon->lifetime());
	_search->sizeValue() | rpl::on_next([=](QSize size) {
		icon->setGeometry(0, 0, Scaled(34), size.height());
		icon->raise();
	}, icon->lifetime());
	icon->show();

	_search->changes() | rpl::on_next([=] {
		if (_inner) {
			_inner->setQuery(_search->getLastText());
			scrollToY(0);
		}
	}, _search->lifetime());
	_search->submits() | rpl::on_next([=] {
		if (_inner) {
			_inner->chooseFirst();
		}
	}, _search->lifetime());

	std::move(
		_args.live
	) | rpl::on_next([=](std::optional<LiveRoom> live) {
		_liveCode = live ? live->code : QString();
		if (_inner) {
			_inner->setLive(std::move(live));
		}
		refreshDimensions();
	}, lifetime());

	// The grid, the fonts and the height of the header are of the look.
	Look::Updates() | rpl::on_next([=] {
		refreshHeader();
		setInnerTopSkip(_headerHeight);
		if (_inner) {
			_inner->lookChanged();
		}
		placeSearch();
		refreshDimensions();
		update();
	}, lifetime());

	refreshDimensions();
}

void HubBox::refreshHeader() {
	const auto list = !Look::HasCards();
	const auto glow = GlowLook();
	_padding = Scaled(list ? 28 : glow ? 26 : 24);
	_titleFont = MakeFont(list ? 40 : glow ? 28 : 22, true);
	_titleTop = Scaled(list ? 20 : 16);
	_rowTop = _titleTop + _titleFont->height + Scaled(list ? 10 : 6);
	_headerHeight = _rowTop + SearchStyle().heightMin + Scaled(14);
}

void HubBox::placeSearch() {
	const auto full = (width() > 0) ? width() : _width;
	const auto inner = std::max(full - 2 * _padding, 1);
	// A narrow box has no place for the line under the title.
	_aboutShown = (inner >= Scaled(kWideHeader));
	const auto field = _aboutShown
		? std::min(Scaled(kSearchWidth), inner / 2)
		: inner;
	_search->resizeToWidth(field);
	_search->moveToLeft(full - _padding - field, _rowTop, full);
}

void HubBox::refreshDimensions() {
	if (_inner) {
		setDimensions(
			_width,
			_headerHeight + _inner->fullHeight(_width) + st::boxRadius);
	}
}

void HubBox::setInnerFocus() {
	_search->setFocusFast();
}

void HubBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);
	if (_inner && width() > 0 && _inner->width() != width()) {
		_inner->resizeToWidth(width());
	}
	placeSearch();
}

void HubBox::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Escape && !_search->getLastText().isEmpty()) {
		// The first Escape clears the search.
		_search->setText(QString());
		if (_inner) {
			_inner->setQuery(QString());
		}
		e->accept();
		return;
	}
	BoxContent::keyPressEvent(e);
}

void HubBox::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto clip = e->rect();
	const auto radius = st::boxRadius;
	p.setClipRect(clip);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(Look::Color(Look::Role::Ground, st::boxBg->c));
		p.drawRoundedRect(rect(), radius, radius);
	}
	if (Look::Current() != Look::kPlain) {
		// The glow of the look, inside of the same corners.
		p.save();
		auto path = QPainterPath();
		path.addRoundedRect(QRectF(rect()), radius, radius);
		p.setClipPath(path, Qt::IntersectClip);
		Look::PaintGround(p, clip, size());
		p.restore();
	}
	if (clip.y() >= _headerHeight) {
		return;
	}
	p.setFont(_titleFont);
	p.setPen(Look::Color(Look::Role::Text, st::boxTitleFg->c));
	p.drawText(
		_padding,
		_titleTop + _titleFont->ascent,
		tr::lng_oblivion_hub_title(tr::now));
	if (_aboutShown) {
		const auto &font = st::normalFont;
		const auto available = _search->x() - _padding - Scaled(16);
		p.setFont(font);
		p.setPen(Look::Color(Look::Role::SubText, st::windowSubTextFg->c));
		p.drawText(
			_padding,
			_rowTop + (_search->height() - font->height) / 2 + font->ascent,
			font->elided(tr::lng_oblivion_hub_about(tr::now), available));
	}
}

void HubBox::choose(int index) {
	if (index < 0 || index >= int(_args.entries.size())) {
		return;
	}
	const auto id = _args.entries[index].id;
	const auto hint = _args.hint ? _args.hint(id) : QString();
	if (!hint.isEmpty()) {
		showToast(hint);
		return;
	}
	// The tool is shown over this box and then the box goes away, so the
	// tool does not return here when it is closed.
	const auto open = _args.open;
	const auto weak = base::make_weak(this);
	if (open) {
		open(id);
	}
	if (const auto strong = weak.get()) {
		strong->closeBox();
	}
}

[[nodiscard]] std::optional<LiveRoom> LiveFrom(Rooms::Room *room) {
	if (!room || room->state().gone != Rooms::Gone::No) {
		return std::nullopt;
	}
	const auto &state = room->state();
	auto result = LiveRoom{
		.code = state.code,
		.title = state.title,
		.seed = state.code,
	};
	for (const auto kind : { Rooms::Kind::Music, Rooms::Kind::Video }) {
		const auto &player = state.player(kind);
		const auto item = player.current();
		if (!item || !player.state.playing) {
			continue;
		}
		result.playing = item->performer.isEmpty()
			? item->title
			: (item->performer + u" — "_q + item->title);
		result.seed = item->media;
		if (!item->cover.isEmpty()) {
			result.cover = room->cover(item->cover);
		}
		break;
	}
	for (const auto &member : state.members) {
		if (member.online && int(result.members.size()) < kMaxMembers) {
			result.members.push_back({ member.id, member.name });
		}
	}
	return result;
}

// The room of the moment the screen is opened: a room that is opened or
// closed later changes nothing here, the screen is over the main window.
[[nodiscard]] rpl::producer<std::optional<LiveRoom>> LiveValue(
		not_null<Main::Session*> session) {
	const auto room = Rooms::ActiveRoom(session);
	if (!room) {
		return rpl::single(std::optional<LiveRoom>());
	}
	const auto weak = base::make_weak(room);
	return rpl::single(
		rpl::empty
	) | rpl::then(
		room->changes() | rpl::to_empty
	) | rpl::map([=] {
		return LiveFrom(weak.get());
	});
}

[[nodiscard]] PeerData *ActivePeer(
		not_null<Window::SessionController*> controller) {
	const auto key = controller->activeChatCurrent();
	return key ? key.peer() : nullptr;
}

// The journal is kept for people only.
[[nodiscard]] UserData *JournalUser(PeerData *peer) {
	const auto user = peer ? peer->asUser() : nullptr;
	return (user
		&& !user->isBot()
		&& !user->isSelf()
		&& !user->isServiceUser()
		&& !user->isInaccessible())
		? user
		: nullptr;
}

[[nodiscard]] bool ClipboardHasImage() {
	const auto data = QGuiApplication::clipboard()->mimeData();
	return data && data->hasImage();
}

[[nodiscard]] QString HintFor(
		not_null<Window::SessionController*> controller,
		Id id) {
	const auto key = controller->activeChatCurrent();
	const auto peer = key ? key.peer() : nullptr;
	switch (id) {
	case Id::RoundVideo:
		return (key && key.thread())
			? QString()
			: tr::lng_oblivion_hub_h_round(tr::now);
	case Id::TextFromImage:
		return ClipboardHasImage()
			? QString()
			: tr::lng_oblivion_hub_h_ocr(tr::now);
	case Id::Cutout:
		return ClipboardHasImage()
			? QString()
			: tr::lng_oblivion_hub_h_cutout(tr::now);
	case Id::ListenTogether: return tr::lng_oblivion_hub_h_listen(tr::now);
	case Id::ChatStats:
		return peer ? QString() : tr::lng_oblivion_hub_h_chat_stats(tr::now);
	case Id::OnlineJournal:
		return JournalUser(peer)
			? QString()
			: tr::lng_oblivion_hub_h_online(tr::now);
	case Id::ProfileHistory:
		return peer
			? QString()
			: tr::lng_oblivion_hub_h_profile_history(tr::now);
	case Id::Rooms:
		return Oblivion::Get().cloudRooms()
			? QString()
			: tr::lng_oblivion_hub_h_rooms_off(tr::now);
	default: return QString();
	}
}

// Every case is the call the tool has in Settings > Oblivion or in the
// menu of a chat.
void Open(not_null<Window::SessionController*> controller, Id id) {
	const auto settings = [&](const QString &control) {
		if (!control.isEmpty()) {
			controller->setHighlightControlId(control);
		}
		controller->showSettings(::Settings::OblivionId());
	};
	switch (id) {
	case Id::PhotoEditor: ShowPhotoEditorImport(controller); break;
	case Id::Collage: ShowPhotoCollage(controller); break;
	case Id::VideoEditor: ShowVideoEditorImport(controller); break;
	case Id::StickerStudio: ShowStickerStudioImport(controller); break;
	case Id::StickerPacks: ShowStickerPacks(controller); break;
	case Id::StickerConverter: ShowStickerConverter(controller); break;
	case Id::StickerBatch: ShowStickerBatch(controller); break;
	case Id::LottieEditor: LottieEdit::ShowLottieEditorImport(); break;
	case Id::MusicEditor: ShowMusicEditor(controller, nullptr); break;
	case Id::VoiceChanger: ShowVoiceEffectBox(controller); break;
	case Id::RoundVideo: {
		const auto key = controller->activeChatCurrent();
		if (const auto thread = key ? key.thread() : nullptr) {
			ChooseVideoToRound(controller, thread);
		}
	} break;
	case Id::TextFromImage: {
		auto image = QGuiApplication::clipboard()->image();
		if (!image.isNull()) {
			ShowRecognizedText(controller, std::move(image));
		}
	} break;
	case Id::Cutout: {
		auto image = QGuiApplication::clipboard()->image();
		if (!image.isNull()) {
			ShowCutout(controller, std::move(image), u"cutout"_q);
		}
	} break;
	case Id::Rooms: Rooms::ShowRoomsBox(controller); break;
	case Id::ListenTogether: break; // Only the hint.
	case Id::Friends: Social::ShowFriends(controller); break;
	case Id::MyProfile: Social::ShowMyProfile(controller); break;
	case Id::Playlists: ShowPlaylists(controller); break;
	case Id::SharedPlaylists: Share::ShowLibrary(controller); break;
	case Id::SharedPresets: Share::ShowGallery(controller); break;
	case Id::SendWhenOnline: SendOnline::ShowList(controller); break;
	case Id::ChatStats:
		if (const auto peer = ActivePeer(controller)) {
			ShowChatStats(controller, peer);
		}
		break;
	case Id::Deleted: ShowDeletedMessages(controller, nullptr); break;
	case Id::OnlineJournal:
		if (const auto user = JournalUser(ActivePeer(controller))) {
			ShowOnlineJournal(controller, user);
		}
		break;
	case Id::ProfileHistory:
		if (const auto peer = ActivePeer(controller)) {
			ShowProfileHistory(controller, peer);
		}
		break;
	case Id::GiftCatalog: ShowGiftCatalog(controller); break;
	case Id::Ghost: settings(u"oblivion/ghost"_q); break;
	case Id::Theme: Look::ShowBox(controller); break;
	case Id::AppIcon: ShowAppIconBox(controller); break;
	case Id::Cloud: settings(u"oblivion/cloud"_q); break;
	case Id::Updates: Update::CheckNow(controller); break;
	case Id::AllSettings: settings(QString()); break;
	}
}

[[nodiscard]] LiveRoom SampleRoom() {
	return {
		.code = u"sample"_q,
		.title = u"Ночной эфир"_q,
		.playing = u"Кино — Звезда по имени Солнце"_q,
		.seed = u"sample"_q,
		.members = {
			{ 101, u"Артём"_q },
			{ 102, u"Аня"_q },
			{ 103, u"Марк"_q },
			{ 104, u"Даня"_q },
			{ 105, u"Я"_q },
		},
	};
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	const auto make = [](bool room, const QString &query) {
		return [=](std::shared_ptr<Ui::Show> show)
		-> object_ptr<Ui::BoxContent> {
			return Box<HubBox>(BoxArgs{
				.show = show,
				.entries = BuildEntries(),
				.live = rpl::single(room
					? std::make_optional(SampleRoom())
					: std::optional<LiveRoom>()),
				.hint = [](Id) { return QString(); },
				.open = [](Id) {},
				.join = [](QString) {},
				.query = query,
			});
		};
	};
	const auto scene = [&](
			const QString &name,
			QSize size,
			bool room,
			const QString &query) {
		RegisterBoxScene(name, size, make(room, query));
		Look::RegisterBoxScenes(name, size, make(room, query));
	};

	// Every tool, three columns (two columns of rows in «Тишина»).
	const auto wide = Scaled(kMaxWidth) + Scaled(60);
	scene(u"hub_box"_q, QSize(wide, Scaled(1280)), false, QString());
	// The account is in a room.
	scene(u"hub_box_room"_q, QSize(wide, Scaled(1380)), true, QString());
	// The search.
	scene(u"hub_box_search"_q, QSize(wide, Scaled(560)), true, u"стик"_q);
	// A narrow window: one column, the field under the title.
	scene(
		u"hub_box_narrow"_q,
		QSize(st::boxWideWidth + Scaled(40), Scaled(900)),
		true,
		QString());
	// Nothing is found.
	RegisterBoxScene(
		u"hub_box_empty"_q,
		QSize(wide, Scaled(420)),
		make(false, u"qwertyzzz"_q));
});

} // namespace

void Show(not_null<Window::SessionController*> controller) {
	const auto weak = base::make_weak(controller);
	controller->show(Box<HubBox>(BoxArgs{
		.show = controller->uiShow(),
		.entries = BuildEntries(),
		.live = LiveValue(&controller->session()),
		.hint = [=](Id id) {
			const auto strong = weak.get();
			return strong ? HintFor(strong, id) : QString();
		},
		.open = [=](Id id) {
			if (const auto strong = weak.get()) {
				Open(strong, id);
			}
		},
		.join = [=](QString code) {
			if (const auto strong = weak.get()) {
				Rooms::OpenLink(strong, code);
			}
		},
	}));
}

void AddMainMenuEntry(
		not_null<Ui::VerticalLayout*> menu,
		not_null<Window::SessionController*> controller) {
	menu->add(
		::Settings::CreateButtonWithIcon(
			menu,
			tr::lng_oblivion_hub_menu(),
			st::mainMenuButton,
			{ &st::menuIconShowAll })
	)->setClickedCallback([=] {
		Show(controller);
	});
}

QString SearchKey(const QString &text) {
	auto result = QString();
	result.reserve(text.size());
	auto space = true; // Nothing at the start.
	for (const auto ch : text.toLower()) {
		if (ch.isLetterOrNumber()) {
			result.push_back((ch == QChar(0x451)) ? QChar(0x435) : ch);
			space = false;
		} else if (!space) {
			result.push_back(QChar(' '));
			space = true;
		}
	}
	if (result.endsWith(QChar(' '))) {
		result.chop(1);
	}
	return result;
}

bool SearchMatches(const QString &key, const QString &query) {
	if (query.isEmpty()) {
		return true;
	}
	const auto words = QStringView(key).split(QChar(' '), Qt::SkipEmptyParts);
	const auto asked = QStringView(query).split(
		QChar(' '),
		Qt::SkipEmptyParts);
	for (const auto &word : asked) {
		const auto found = ranges::any_of(words, [&](QStringView has) {
			return has.startsWith(word)
				|| (word.size() >= 3 && has.contains(word));
		});
		if (!found) {
			return false;
		}
	}
	return true;
}

Placed Place(const std::array<int, kGroups> &counts, const Metrics &m) {
	auto result = Placed();
	const auto inner = std::max(m.width - 2 * m.padding, 1);
	auto y = 0;
	if (m.banner > 0) {
		result.banner = QRect(m.padding, y, inner, m.banner);
		y += m.banner + m.bannerSkip;
	}
	auto number = 0;
	// The end of the tools, without the skip after the last group.
	auto bottom = y;
	const auto group = [&](int index, int left, int width, int columns) {
		const auto count = counts[index];
		result.labels.push_back({ index, QRect(left, y, width, m.label) });
		y += m.label;
		for (auto i = 0; i != count; ++i) {
			const auto column = i % columns;
			const auto row = i / columns;
			// The widths differ by a pixel at most.
			const auto from = (column * (width + m.gap)) / columns;
			const auto till = ((column + 1) * (width + m.gap)) / columns
				- m.gap;
			result.tiles.push_back({
				index,
				i,
				++number,
				QRect(
					left + from,
					y + row * (m.tile + m.rowGap),
					till - from,
					m.tile),
			});
		}
		const auto rows = (count + columns - 1) / columns;
		y += rows * m.tile + (rows - 1) * m.rowGap;
		bottom = std::max(bottom, y);
		y += m.groupSkip;
	};
	if (m.list && m.columns > 1) {
		// .d3 .h-cols: создавать and приложение, вместе and знать.
		const auto width = std::max((inner - m.gap) / 2, 1);
		const auto top = y;
		const int sides[2][2] = {
			{ int(Group::Create), int(Group::App) },
			{ int(Group::Together), int(Group::Know) },
		};
		for (auto side = 0; side != 2; ++side) {
			y = top;
			const auto left = m.padding + side * (inner - width);
			for (const auto index : sides[side]) {
				if (counts[index] > 0) {
					group(index, left, width, 1);
				}
			}
		}
	} else {
		const auto columns = std::max(m.list ? 1 : m.columns, 1);
		for (auto index = 0; index != kGroups; ++index) {
			if (counts[index] > 0) {
				group(index, m.padding, inner, columns);
			}
		}
	}
	result.height = bottom + m.bottom;
	return result;
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

	// The search.
	check(
		SearchKey(u"  Lottie-редактор,  Ёлка! "_q) == u"lottie редактор елка"_q,
		u"search: the key is lower case words"_q);
	check(SearchKey(u" — "_q).isEmpty(), u"search: no words, no key"_q);
	const auto key = SearchKey(
		u"Студия стикеров перекрасить стикер sticker tgs"_q);
	check(SearchMatches(key, QString()), u"search: empty finds all"_q);
	check(SearchMatches(key, SearchKey(u"СТИК"_q)), u"search: a prefix"_q);
	check(
		SearchMatches(key, SearchKey(u"stick студ"_q)),
		u"search: every word is looked for"_q);
	check(
		!SearchMatches(key, SearchKey(u"стик музыка"_q)),
		u"search: one missing word hides the tool"_q);
	check(
		SearchMatches(key, SearchKey(u"крас"_q)),
		u"search: three letters are found inside of a word"_q);
	check(
		!SearchMatches(key, SearchKey(u"ик"_q)),
		u"search: two letters only start a word"_q);

	// The table.
	{
		const auto &tools = Tools();
		auto names = QStringList();
		auto groups = std::array<int, kGroups>();
		auto valid = !tools.empty();
		for (const auto &tool : tools) {
			const auto name = QString::fromLatin1(tool.name);
			valid = valid
				&& !name.isEmpty()
				&& !names.contains(name)
				&& (int(tool.group) >= 0)
				&& (int(tool.group) < kGroups)
				&& (tool.icon != nullptr)
				&& (tool.title.base != tool.about.base);
			for (const auto ch : name) {
				valid = valid
					&& ((ch >= 'a' && ch <= 'z') || ch == '_');
			}
			names.push_back(name);
			if (int(tool.group) >= 0 && int(tool.group) < kGroups) {
				++groups[int(tool.group)];
			}
		}
		check(valid, u"table: names are unique, every tool is complete"_q);
		check(
			ranges::all_of(groups, [](int count) { return count > 0; }),
			u"table: no group is empty"_q);
	}

	// The placing.
	const auto intersects = [](const Placed &placed) {
		for (auto i = 0; i != int(placed.tiles.size()); ++i) {
			for (auto j = i + 1; j != int(placed.tiles.size()); ++j) {
				if (placed.tiles[i].rect.intersects(placed.tiles[j].rect)) {
					return true;
				}
			}
			for (const auto &label : placed.labels) {
				if (placed.tiles[i].rect.intersects(label.rect)) {
					return true;
				}
			}
			if (placed.tiles[i].rect.intersects(placed.banner)) {
				return true;
			}
		}
		return false;
	};
	const auto inside = [](const Placed &placed, const Metrics &m) {
		const auto area = QRect(
			m.padding,
			0,
			m.width - 2 * m.padding,
			placed.height - m.bottom);
		for (const auto &tile : placed.tiles) {
			if (!area.contains(tile.rect) || tile.rect.isEmpty()) {
				return false;
			}
		}
		for (const auto &label : placed.labels) {
			if (!area.contains(label.rect)) {
				return false;
			}
		}
		return placed.banner.isEmpty() || area.contains(placed.banner);
	};
	const auto numbered = [](const Placed &placed) {
		for (auto i = 0; i != int(placed.tiles.size()); ++i) {
			if (placed.tiles[i].number != i + 1) {
				return false;
			}
		}
		return true;
	};
	const auto counts = std::array<int, kGroups>{ 13, 8, 5, 6 };
	const auto total = 13 + 8 + 5 + 6;
	auto grid = Metrics{
		.width = 840,
		.padding = 24,
		.gap = 10,
		.rowGap = 10,
		.tile = 64,
		.label = 36,
		.groupSkip = 6,
		.columns = 3,
		.banner = 76,
		.bannerSkip = 4,
		.bottom = 24,
	};
	for (const auto width : { 364, 500, 701, 840 }) {
		for (const auto columns : { 1, 2, 3 }) {
			grid.width = width;
			grid.columns = columns;
			const auto placed = Place(counts, grid);
			const auto name = u"grid %1 / %2: "_q.arg(width).arg(columns);
			check(
				int(placed.tiles.size()) == total
					&& int(placed.labels.size()) == kGroups,
				name + u"every tool and every group has a place"_q);
			check(!intersects(placed), name + u"nothing overlaps"_q);
			check(inside(placed, grid), name + u"nothing sticks out"_q);
			check(numbered(placed), name + u"the numbers go in a row"_q);
			auto even = true;
			for (const auto &tile : placed.tiles) {
				const auto delta = tile.rect.width()
					- placed.tiles.front().rect.width();
				even = even && (delta >= -1) && (delta <= 1);
			}
			check(even, name + u"the tiles are of one width"_q);
		}
	}
	{
		// A row of three: the last tile ends where the area ends.
		grid.width = 840;
		grid.columns = 3;
		const auto placed = Place(counts, grid);
		check(
			placed.tiles[2].rect.x() + placed.tiles[2].rect.width()
				== grid.width - grid.padding,
			u"grid: a row takes the whole width"_q);
		check(
			placed.tiles[3].rect.y()
				== placed.tiles[0].rect.y() + grid.tile + grid.rowGap,
			u"grid: the next row is below"_q);
	}
	{
		auto rows = grid;
		rows.width = 840;
		rows.gap = 36;
		rows.rowGap = 0;
		rows.tile = 47;
		rows.label = 46;
		rows.columns = 2;
		rows.list = true;
		const auto placed = Place(counts, rows);
		check(
			int(placed.tiles.size()) == total && !intersects(placed),
			u"rows: every tool has a row"_q);
		check(inside(placed, rows), u"rows: nothing sticks out"_q);
		check(numbered(placed), u"rows: the numbers go in a row"_q);
		// создавать, приложение | вместе, знать.
		auto order = std::vector<int>();
		for (const auto &label : placed.labels) {
			order.push_back(label.group);
		}
		check(
			order == std::vector<int>{ 0, 3, 1, 2 },
			u"rows: the groups stand as in the mockup"_q);
		check(
			placed.tiles.front().rect.x() == rows.padding
				&& (placed.tiles.back().rect.x()
					+ placed.tiles.back().rect.width()
					== rows.width - rows.padding),
			u"rows: two columns take the whole width"_q);

		rows.columns = 1;
		const auto single = Place(counts, rows);
		check(
			!intersects(single) && inside(single, rows) && numbered(single),
			u"rows: one column for a narrow box"_q);
	}
	{
		// A search that leaves one group.
		grid.banner = 0;
		const auto placed = Place({ 0, 2, 0, 0 }, grid);
		check(
			int(placed.labels.size()) == 1
				&& int(placed.tiles.size()) == 2
				&& placed.labels.front().rect.y() == 0
				&& placed.height == grid.label + grid.tile + grid.bottom,
			u"placing: an empty group takes no place"_q);
		const auto nothing = Place({ 0, 0, 0, 0 }, grid);
		check(
			nothing.tiles.empty() && nothing.height == grid.bottom,
			u"placing: nothing is found"_q);
	}
	return passed;
}

} // namespace Oblivion::Hub
