/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_cloud_social_ui.h"

#include "base/call_delayed.h"
#include "base/weak_ptr.h"
#include "boxes/peer_list_box.h"
#include "boxes/peer_list_controllers.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "oblivion/oblivion_badge.h"
#include "oblivion/oblivion_cloud.h"
#include "oblivion/oblivion_cloud_share.h"
#include "oblivion/oblivion_cloud_social.h"
#include "oblivion/oblivion_lang.h"
#include "oblivion/oblivion_look.h"
#include "oblivion/oblivion_look_ui.h"
#include "oblivion/oblivion_room.h"
#include "oblivion/oblivion_settings.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "settings/settings_common.h"
#include "ui/boxes/confirm_box.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/effects/animation_value.h"
#include "ui/emoji_config.h"
#include "ui/empty_userpic.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/text/text.h"
#include "ui/text/text_options.h"
#include "ui/text/text_utilities.h"
#include "ui/userpic_view.h"
#include "ui/vertical_list.h"
#include "ui/widgets/box_content_divider.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_info.h"
#include "styles/style_info_profile_actions.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"
#include "styles/style_window.h"

#include <QtCore/QBuffer>
#include <QtCore/QJsonArray>
#include <QtGui/QPainterPath>

namespace Oblivion::Social {
namespace {

constexpr auto kChipHeight = 26;
constexpr auto kChipPadding = 9;
constexpr auto kChipIcon = 14;
constexpr auto kChipIconSkip = 6;
constexpr auto kChipSkip = 6;
constexpr auto kChipJoinSkip = 8;
constexpr auto kChipJoinInset = 3;
constexpr auto kChipJoinPadding = 9;
constexpr auto kChipBgOpacity = 0.14;
constexpr auto kChipLabelOpacity = 0.62;

// «Войти» in the colour of a person: a fill brighter than this gets a
// dark text instead of a white one; the percent it changes by under the
// mouse; how dark the dark text is.
constexpr auto kChipJoinDarkFrom = 0.36;
constexpr auto kChipJoinOverFactor = 110;
constexpr auto kChipJoinDarkAlpha = 217;
constexpr auto kCardGlyph = 16;
constexpr auto kCardSkip = 4;
constexpr auto kCardRadius = 10;
constexpr auto kCardTop = 10;
constexpr auto kCardBottom = 8;
constexpr auto kStatusLines = 2;
constexpr auto kNameLimit = 64;
constexpr auto kStatusLimit = 80;
constexpr auto kCellSize = 36;
constexpr auto kCellRadius = 8;
constexpr auto kCellsPerRow = 8;
constexpr auto kSwatchSize = 22;
constexpr auto kAvatarSide = 160;
constexpr auto kAvatarBytes = 60 * 1024;
constexpr auto kSceneWidth = 340;

// The looks («Тема Oblivion», oblivion_look.h), at the 100% scale. The
// plain look uses the values above and paints what it always has.
//
// «Родной, но лучше» and «Ночной эфир»: the chips are a little higher,
// «Войти» is smaller inside them.
constexpr auto kLookChipHeight = 30;
constexpr auto kLookChipLeft = 9;
constexpr auto kLookChipRight = 12;
constexpr auto kLookChipSkip = 8;
constexpr auto kLookJoinInset = 4;
// «Ночной эфир»: «слушает» is a card in the whole width, with a mark of
// the gradient at its left.
constexpr auto kLiveHeight = 54;
constexpr auto kLiveMark = 34;
constexpr auto kLiveMarkRadius = 11;
constexpr auto kLivePadding = 10;
constexpr auto kLiveSkip = 12;
constexpr auto kLiveValueSize = 15;
// «Тишина»: a chip is a row with a hairline under it, the label is small
// capitals over the value.
constexpr auto kRowHeight = 48;
constexpr auto kRowIcon = 16;
constexpr auto kRowSkip = 12;
constexpr auto kRowJoinHeight = 28;
constexpr auto kRowJoinPadding = 14;
constexpr auto kCapsSize = 11;
constexpr auto kCapsSpacing = 1.1;
// The block of a profile as a card: what it has around its content.
constexpr auto kBlockCardPadding = 12;
// A row of a list under the mouse.
constexpr auto kRowHoverInset = 6;
constexpr auto kFlatCellRadius = 4;

// The edits of «Выбранные люди» are sent as one request this long after
// the last click; a "too often" of the server is waited out, not longer
// than the second value.
constexpr auto kChosenSendDelay = crl::time(1500);
constexpr auto kChosenRetryMax = crl::time(30'000);

// The quick choices of the editor. Anything else that is already set for
// the account is kept and shown first.
const char *const kStatusEmoji[] = {
	"🎧", "🎵", "🎸", "😴", "🔥", "💻", "🎮",
	"📚", "☕", "🌙", "✨", "🚀", "🍿", "🤫", "👀",
};
// Seven colours and "none" make one full line of the strip.
constexpr uint32 kAccentColors[] = {
	0x7c5cff,
	0x2aa9e0,
	0x3ecf8e,
	0xf5c542,
	0xff8a3d,
	0xff5c8a,
	0xb06cf0,
};

[[nodiscard]] int Scaled(int value) {
	return style::ConvertScale(value);
}

[[nodiscard]] QString SampleText(const char *ru, const char *en) {
	return QString::fromUtf8(CurrentLanguageIsRussian() ? ru : en);
}

[[nodiscard]] style::margins RowPadding() {
	return st::boxRowPadding + style::margins(0, 0, 0, st::boxLittleSkip);
}

// A strip of choices stands out of the column of the box by the gap
// around a swatch: the first and the last choice of a line are exactly
// under the edges of the fields above.
[[nodiscard]] style::margins StripPadding() {
	const auto out = (Scaled(kCellSize) - Scaled(kSwatchSize)) / 2;
	return st::boxRowPadding
		+ style::margins(-out, 0, -out, st::boxLittleSkip);
}

// A state of a list that has no rows: a text in the middle of the box,
// the way the lists of the app say that they are empty.
[[nodiscard]] style::margins EmptyPadding() {
	return st::boxRowPadding
		+ style::margins(0, st::boxMediumSkip, 0, st::boxMediumSkip);
}

void Toast(const std::shared_ptr<Ui::Show> &show, const QString &text) {
	if (show && show->valid() && !text.isEmpty()) {
		show->showToast(text);
	}
}

[[nodiscard]] ChipPhrases LangPhrases() {
	return {
		.listening = tr::lng_oblivion_social_chip_text_listening(
			tr::now,
			lt_text,
			u"{text}"_q),
		.room = tr::lng_oblivion_social_chip_text_room(
			tr::now,
			lt_title,
			u"{title}"_q),
		.roomUntitled = tr::lng_oblivion_social_chip_text_room_plain(tr::now),
		.online = tr::lng_oblivion_social_chip_text_online(tr::now),
	};
}

// What a phrase of a chip begins with, «слушает: » of «слушает: {text}».
// A chip paints it lighter than the track or the room that follows.
[[nodiscard]] QString PhraseLabel(const QString &phrase, const QString &tag) {
	const auto index = phrase.indexOf(tag);
	return (index > 0) ? phrase.left(index) : QString();
}

// One emoji the app knows, or nothing: what has come from another person
// is never painted as it is.
[[nodiscard]] EmojiPtr FindEmoji(const QString &text) {
	if (text.isEmpty()) {
		return nullptr;
	}
	auto length = 0;
	const auto result = Ui::Emoji::Find(text, &length);
	return (result && length == text.size()) ? result : nullptr;
}

[[nodiscard]] QString StatusLine(const Profile &profile) {
	const auto emoji = FindEmoji(profile.statusEmoji);
	const auto text = profile.statusText.simplified();
	return !emoji
		? text
		: text.isEmpty()
		? emoji->text()
		: (emoji->text() + QChar(' ') + text);
}

[[nodiscard]] QString SharedLine(const Profile &profile) {
	auto parts = QStringList();
	if (profile.publicPlaylists > 0) {
		parts.push_back(tr::lng_oblivion_social_block_playlists(
			tr::now,
			lt_count,
			profile.publicPlaylists));
	}
	if (profile.publicPresets > 0) {
		parts.push_back(tr::lng_oblivion_social_block_presets(
			tr::now,
			lt_count,
			profile.publicPresets));
	}
	return parts.join(QString::fromUtf8(" · "));
}

// Whether the «Oblivion» block of a profile has anything a person would
// see: a status line as it is painted (an emoji the app knows, a text)
// or shared things. A profile made of anything else shows no block.
[[nodiscard]] bool HasVisibleContent(const Profile &profile) {
	return profile.hasContent()
		&& (!StatusLine(profile).isEmpty()
			|| !SharedLine(profile).isEmpty());
}

[[nodiscard]] QString SharedItemTitle(const SharedItem &item) {
	const auto detail = item.playlist
		? tr::lng_oblivion_social_shared_tracks(tr::now, lt_count, item.count)
		: (item.kind == u"video"_q)
		? tr::lng_oblivion_social_shared_video(tr::now)
		: tr::lng_oblivion_social_shared_photo(tr::now);
	const auto title = item.title.simplified();
	return title.isEmpty()
		? detail
		: (title + QString::fromUtf8(" · ") + detail);
}

// Whether a dark text reads better than a white one on a fill: by the
// relative luminance of the fill.
[[nodiscard]] bool DarkTextOn(const QColor &fill) {
	const auto channel = [](double value) {
		return (value <= 0.03928)
			? (value / 12.92)
			: std::pow((value + 0.055) / 1.055, 2.4);
	};
	const auto luminance = 0.2126 * channel(fill.redF())
		+ 0.7152 * channel(fill.greenF())
		+ 0.0722 * channel(fill.blueF());
	return (luminance > kChipJoinDarkFrom);
}

// The icons of the chips are painted in code, in the colour of the chip.
void PaintChipIcon(QPainter &p, ChipType type, QRectF rect, QColor color) {
	if (type == ChipType::Online) {
		Badge::Paint(p, rect.toRect(), color);
		return;
	}
	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(color);
	const auto side = rect.width();
	const auto x = rect.x();
	const auto y = rect.y();
	if (type == ChipType::Listening) {
		// Three bars of an equalizer.
		const auto bar = side * 0.2;
		const auto gap = (side - 3 * bar) / 2.;
		const auto bottom = y + side * 0.93;
		auto left = x;
		for (const auto part : { 0.55, 1., 0.72 }) {
			const auto height = side * 0.86 * part;
			p.drawRoundedRect(
				QRectF(left, bottom - height, bar, height),
				bar / 2.,
				bar / 2.);
			left += bar + gap;
		}
	} else {
		// Two people: a room is where somebody else is.
		const auto person = [&](double center, double top, double scale) {
			const auto head = side * 0.17 * scale;
			const auto middle = x + side * center;
			const auto headTop = y + side * top;
			p.drawEllipse(QPointF(middle, headTop + head), head, head);
			const auto half = side * 0.27 * scale;
			const auto bodyTop = headTop + head * 2 + side * 0.06;
			const auto bodyBottom = y + side * 0.93;
			auto path = QPainterPath();
			path.moveTo(middle - half, bodyBottom);
			path.quadTo(middle - half, bodyTop, middle, bodyTop);
			path.quadTo(middle + half, bodyTop, middle + half, bodyBottom);
			path.closeSubpath();
			p.drawPath(path);
		};
		p.setOpacity(0.55);
		person(0.70, 0.14, 0.85);
		p.setOpacity(1.);
		person(0.36, 0.06, 1.);
	}
	p.restore();
}

// The small capitals of «Ночной эфир» and «Тишина».
[[nodiscard]] QFont CapsFont() {
	auto result = st::semiboldFont->f;
	result.setPixelSize(Scaled(kCapsSize));
	result.setLetterSpacing(
		QFont::AbsoluteSpacing,
		style::ConvertScaleExact(kCapsSpacing));
	return result;
}

// «слушает: » as a line of its own over the track: «СЛУШАЕТ».
[[nodiscard]] QString CapsLabel(QString label) {
	label = label.trimmed();
	while (label.endsWith(QChar(':'))) {
		label.chop(1);
	}
	return label.trimmed().toUpper();
}

// The track in the big «слушает» card of «Ночной эфир».
[[nodiscard]] const style::font &LiveFont() {
	static const auto result = style::font(
		Scaled(kLiveValueSize),
		st::semiboldFont->flags(),
		st::semiboldFont->family());
	return result;
}

[[nodiscard]] Look::Chip ChipKind(ChipType type) {
	return (type == ChipType::Room)
		? Look::Chip::Room
		: (type == ChipType::Online)
		? Look::Chip::Neutral
		: Look::Chip::Accent;
}

// The activity chips: rounded pills with an icon and a text, wrapped to
// as many lines as the width needs. «слушает:» and «в комнате:» are
// painted lighter, the track and the room stand out. The chip of a room
// that lets people in ends with the button «Войти».
//
// With a look («Тема Oblivion») the chips have its shapes and colours:
// tinted pills in «Родной, но лучше»; pills with a stroke and a big card
// for «слушает» in «Ночной эфир»; rows with hairlines in «Тишина». The
// colour of a person stays on the mark of a chip in every look.
class ChipsView final : public Ui::RpWidget {
public:
	explicit ChipsView(QWidget *parent);

	void setChips(std::vector<Chip> chips, std::optional<QColor> accent);
	void setJoinCallback(Fn<void(QString)> callback);

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	struct Item {
		Chip chip;
		QString label; // «слушает: », or nothing.
		QString value; // The rest of the text.
		QString shown; // The value as it fits.
		QString caps; // A look: the label as a line of its own.
		int labelWidth = 0;
		QRect rect;
		QRect join;
		bool live = false; // The big card of «Ночной эфир».
	};

	[[nodiscard]] int joinAt(QPoint point) const;
	void setHovered(int index);
	[[nodiscard]] int layoutPlain(int newWidth);
	[[nodiscard]] int layoutLook(int newWidth);
	void paintPlain(QPainter &p);
	void paintLook(QPainter &p);

	const QString _joinText;
	const QString _listeningLabel;
	const QString _roomLabel;
	std::vector<Item> _items;
	std::optional<QColor> _accent;
	Fn<void(QString)> _join;
	int _hovered = -1;
	int _pressed = -1;

};

ChipsView::ChipsView(QWidget *parent)
: RpWidget(parent)
, _joinText(tr::lng_oblivion_social_chip_join(tr::now))
, _listeningLabel(PhraseLabel(LangPhrases().listening, u"{text}"_q))
, _roomLabel(PhraseLabel(LangPhrases().room, u"{title}"_q)) {
	setMouseTracking(true);

	// A look has chips of its own size.
	Look::Updates(
	) | rpl::on_next([=] {
		if (width() > 0) {
			resizeToWidth(width());
		}
		update();
	}, lifetime());
}

void ChipsView::setChips(
		std::vector<Chip> chips,
		std::optional<QColor> accent) {
	_accent = accent;
	_hovered = _pressed = -1;
	_items.clear();
	_items.reserve(chips.size());
	for (auto &chip : chips) {
		const auto &label = (chip.type == ChipType::Listening)
			? _listeningLabel
			: (chip.type == ChipType::Room)
			? _roomLabel
			: QString();
		const auto split = !label.isEmpty()
			&& (chip.text.size() > label.size())
			&& chip.text.startsWith(label);
		auto item = Item();
		item.label = split ? label : QString();
		item.value = split ? chip.text.mid(label.size()) : chip.text;
		item.chip = std::move(chip);
		_items.push_back(std::move(item));
	}
	if (width() > 0) {
		resizeToWidth(width());
	}
	update();
}

void ChipsView::setJoinCallback(Fn<void(QString)> callback) {
	_join = std::move(callback);
}

int ChipsView::resizeGetHeight(int newWidth) {
	if (_items.empty()) {
		return 0;
	}
	return Look::Is(Look::kPlain)
		? layoutPlain(newWidth)
		: layoutLook(newWidth);
}

int ChipsView::layoutLook(int newWidth) {
	const auto look = Look::Current();
	const auto capsMetrics = QFontMetrics(CapsFont());
	const auto &joinFont = st::semiboldFont;
	const auto full = std::max(newWidth, 1);
	if (look == Look::kSilence) {
		// Rows: the mark, the label over the value, «Войти» at the right.
		const auto height = Scaled(kRowHeight);
		const auto joinHeight = Scaled(kRowJoinHeight);
		const auto joinWidth = joinFont->width(_joinText)
			+ 2 * Scaled(kRowJoinPadding);
		const auto textLeft = Scaled(kRowIcon) + Scaled(kRowSkip);
		auto top = 0;
		for (auto &item : _items) {
			const auto join = !item.chip.joinCode.isEmpty();
			const auto available = std::max(
				newWidth
					- textLeft
					- (join ? (joinWidth + Scaled(kRowSkip)) : 0),
				0);
			item.live = false;
			item.labelWidth = 0;
			item.caps = capsMetrics.elidedText(
				CapsLabel(item.label),
				Qt::ElideRight,
				available);
			item.shown = st::normalFont->elided(item.value, available);
			item.rect = QRect(0, top, full, height);
			item.join = join
				? QRect(
					full - joinWidth,
					top + (height - joinHeight) / 2,
					joinWidth,
					joinHeight)
				: QRect();
			top += height;
		}
		return top;
	}

	// Pills, and under them the card of «слушает» in «Ночной эфир».
	const auto height = Scaled(kLookChipHeight);
	const auto skip = Scaled(kLookChipSkip);
	const auto inset = Scaled(kLookJoinInset);
	const auto joinWidth = joinFont->width(_joinText)
		+ 2 * Scaled(kChipJoinPadding);
	const auto icon = Scaled(kChipIcon) + Scaled(kChipIconSkip);
	auto left = 0;
	auto top = 0;
	auto pills = false;
	Item *live = nullptr;
	for (auto &item : _items) {
		item.caps = QString();
		item.live = !live
			&& (look == Look::kNightAir)
			&& (item.chip.type == ChipType::Listening);
		if (item.live) {
			live = &item;
			continue;
		}
		const auto join = !item.chip.joinCode.isEmpty();
		const auto &font = item.label.isEmpty()
			? st::normalFont
			: st::semiboldFont;
		item.labelWidth = item.label.isEmpty()
			? 0
			: st::normalFont->width(item.label);
		const auto fixed = Scaled(kLookChipLeft)
			+ icon
			+ item.labelWidth
			+ (join
				? (Scaled(kChipJoinSkip) + joinWidth + inset)
				: Scaled(kLookChipRight));
		const auto available = std::max(newWidth - fixed, 0);
		item.shown = font->elided(item.value, available);
		const auto width = std::min(fixed + font->width(item.shown), full);
		if (left > 0 && left + width > newWidth) {
			left = 0;
			top += height + skip;
		}
		item.rect = QRect(left, top, width, height);
		item.join = join
			? QRect(
				left + width - inset - joinWidth,
				top + inset,
				joinWidth,
				height - 2 * inset)
			: QRect();
		left += width + skip;
		pills = true;
	}
	auto bottom = pills ? (top + height) : 0;
	if (live) {
		const auto cardTop = pills ? (bottom + skip) : 0;
		const auto cardHeight = Scaled(kLiveHeight);
		const auto available = std::max(
			newWidth
				- 2 * Scaled(kLivePadding)
				- Scaled(kLiveMark)
				- Scaled(kLiveSkip),
			0);
		live->labelWidth = 0;
		live->caps = capsMetrics.elidedText(
			CapsLabel(live->label),
			Qt::ElideRight,
			available);
		live->shown = LiveFont()->elided(live->value, available);
		live->rect = QRect(0, cardTop, full, cardHeight);
		live->join = QRect();
		bottom = cardTop + cardHeight;
	}
	return bottom;
}

int ChipsView::layoutPlain(int newWidth) {
	const auto height = Scaled(kChipHeight);
	const auto padding = Scaled(kChipPadding);
	const auto skip = Scaled(kChipSkip);
	const auto inset = Scaled(kChipJoinInset);
	const auto joinWidth = st::semiboldFont->width(_joinText)
		+ 2 * Scaled(kChipJoinPadding);
	const auto icon = Scaled(kChipIcon) + Scaled(kChipIconSkip);
	auto left = 0;
	auto top = 0;
	for (auto &item : _items) {
		const auto join = !item.chip.joinCode.isEmpty();
		const auto &font = item.label.isEmpty()
			? st::normalFont
			: st::semiboldFont;
		item.labelWidth = item.label.isEmpty()
			? 0
			: st::normalFont->width(item.label);

		// The button stands inside the chip, the same distance from its
		// top, bottom and right edges.
		const auto fixed = padding
			+ icon
			+ item.labelWidth
			+ (join ? (Scaled(kChipJoinSkip) + joinWidth + inset) : padding);
		const auto available = std::max(newWidth - fixed, 0);
		item.shown = font->elided(item.value, available);
		const auto width = std::min(
			fixed + font->width(item.shown),
			std::max(newWidth, 1));
		if (left > 0 && left + width > newWidth) {
			left = 0;
			top += height + skip;
		}
		item.rect = QRect(left, top, width, height);
		item.join = join
			? QRect(
				left + width - inset - joinWidth,
				top + inset,
				joinWidth,
				height - 2 * inset)
			: QRect();
		left += width + skip;
	}
	return top + height;
}

void ChipsView::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	if (Look::Is(Look::kPlain)) {
		paintPlain(p);
	} else {
		paintLook(p);
	}
}

void ChipsView::paintLook(QPainter &p) {
	using Role = Look::Role;
	const auto look = Look::Current();
	const auto rows = (look == Look::kSilence);
	const auto air = (look == Look::kNightAir);
	const auto capsFont = CapsFont();
	const auto capsMetrics = QFontMetrics(capsFont);
	const auto capsLine = capsMetrics.height() + Scaled(2);
	const auto &joinFont = st::semiboldFont;
	auto index = 0;
	for (const auto &item : _items) {
		const auto over = (index++ == _hovered);
		const auto &rect = item.rect;
		const auto kind = ChipKind(item.chip.type);
		const auto quiet = (kind == Look::Chip::Neutral);
		// The colour of a person stays on the mark of the chip, without
		// one the mark has the colour the look gives it.
		const auto mark = _accent.value_or(rows
			? Look::Color(Role::SubText)
			: air
			? Look::Color(Role::Accent)
			: quiet
			? Look::ChipLabel(kind, QColor())
			: Look::ChipText(kind, QColor()));
		Look::PaintChip(
			p,
			QRectF(rect),
			QColor(),
			0,
			item.live ? Look::Chip::Live : kind);
		if (item.live) {
			// The mark: a tile of the gradient (of the colour of a person
			// who has one) with the equalizer on it.
			const auto size = Scaled(kLiveMark);
			const auto glyph = Scaled(kRowIcon);
			const auto radius = Scaled(kLiveMarkRadius);
			const auto tile = QRectF(
				rect.x() + Scaled(kLivePadding),
				rect.y() + (rect.height() - size) / 2.,
				size,
				size);
			if (_accent) {
				p.setPen(Qt::NoPen);
				p.setBrush(*_accent);
				p.drawRoundedRect(tile, radius, radius);
			} else {
				Look::PaintAccentGradient(p, tile, radius, QColor());
			}
			PaintChipIcon(
				p,
				item.chip.type,
				QRectF(
					tile.x() + (size - glyph) / 2.,
					tile.y() + (size - glyph) / 2.,
					glyph,
					glyph),
				((_accent && DarkTextOn(*_accent))
					? QColor(0, 0, 0, kChipJoinDarkAlpha)
					: QColor(255, 255, 255)));
			const auto textLeft = rect.x()
				+ Scaled(kLivePadding)
				+ size
				+ Scaled(kLiveSkip);
			const auto &valueFont = LiveFont();
			const auto labeled = !item.caps.isEmpty();
			auto top = rect.y()
				+ (rect.height()
					- valueFont->height
					- (labeled ? capsLine : 0)) / 2;
			if (labeled) {
				p.setFont(capsFont);
				p.setPen(Look::ChipLabel(Look::Chip::Live, QColor()));
				p.drawText(textLeft, top + capsMetrics.ascent(), item.caps);
				top += capsLine;
			}
			p.setFont(valueFont);
			p.setPen(Look::ChipText(Look::Chip::Live, QColor()));
			p.drawText(textLeft, top + valueFont->ascent, item.shown);
			continue;
		} else if (rows) {
			if (&item == &_items.front()) {
				Look::PaintDivider(
					p,
					QRectF(rect.x(), rect.y(), rect.width(), st::lineWidth),
					st::shadowFg->c);
			}
			const auto icon = Scaled(kRowIcon);
			PaintChipIcon(
				p,
				item.chip.type,
				QRectF(
					rect.x(),
					rect.y() + (rect.height() - icon) / 2.,
					icon,
					icon),
				mark);
			const auto textLeft = rect.x() + icon + Scaled(kRowSkip);
			const auto &valueFont = st::normalFont;
			const auto labeled = !item.caps.isEmpty();
			auto top = rect.y()
				+ (rect.height()
					- valueFont->height
					- (labeled ? capsLine : 0)) / 2;
			if (labeled) {
				p.setFont(capsFont);
				p.setPen(Look::Color(Role::SubText));
				p.drawText(textLeft, top + capsMetrics.ascent(), item.caps);
				top += capsLine;
			}
			p.setFont(valueFont);
			p.setPen(Look::Color(Role::Text));
			p.drawText(textLeft, top + valueFont->ascent, item.shown);
		} else {
			const auto icon = Scaled(kChipIcon);
			PaintChipIcon(
				p,
				item.chip.type,
				QRectF(
					rect.x() + Scaled(kLookChipLeft),
					rect.y() + (rect.height() - icon) / 2.,
					icon,
					icon),
				mark);
			auto textLeft = rect.x()
				+ Scaled(kLookChipLeft)
				+ icon
				+ Scaled(kChipIconSkip);
			const auto baseline = rect.y()
				+ (rect.height() - st::normalFont->height) / 2
				+ st::normalFont->ascent;
			if (!item.label.isEmpty()) {
				p.setFont(st::normalFont);
				p.setPen(Look::ChipLabel(kind, QColor()));
				p.drawText(textLeft, baseline, item.label);
				textLeft += item.labelWidth;
			}
			p.setFont(item.label.isEmpty()
				? st::normalFont
				: st::semiboldFont);
			p.setPen(quiet
				? Look::ChipLabel(kind, QColor())
				: Look::ChipText(kind, QColor()));
			p.drawText(textLeft, baseline, item.shown);
		}
		if (!item.join.isEmpty()) {
			// «Войти»: the fill of the room of the look, its gradient in
			// «Ночной эфир»; under the mouse a touch of the text colour
			// lies over it.
			const auto join = QRectF(item.join);
			const auto corner = join.height() / 2.;
			const auto text = Look::Color(air
				? Role::OnAccent
				: Role::OnRoom);
			if (air) {
				Look::PaintAccentGradient(p, join, corner, QColor());
			} else {
				p.setPen(Qt::NoPen);
				p.setBrush(Look::Color(Role::RoomFill));
				p.drawRoundedRect(join, corner, corner);
			}
			if (over) {
				auto veil = text;
				veil.setAlpha(36);
				p.setPen(Qt::NoPen);
				p.setBrush(veil);
				p.drawRoundedRect(join, corner, corner);
			}
			p.setFont(joinFont);
			p.setPen(text);
			p.drawText(
				item.join.x()
					+ (item.join.width() - joinFont->width(_joinText)) / 2,
				item.join.y()
					+ (item.join.height() - joinFont->height) / 2
					+ joinFont->ascent,
				_joinText);
		}
	}
}

void ChipsView::paintPlain(QPainter &p) {
	const auto accent = _accent.value_or(st::windowActiveTextFg->c);
	const auto padding = Scaled(kChipPadding);
	const auto icon = Scaled(kChipIcon);
	auto index = 0;
	for (const auto &item : _items) {
		const auto &rect = item.rect;
		const auto radius = rect.height() / 2.;
		p.setPen(Qt::NoPen);
		p.setBrush(anim::with_alpha(accent, kChipBgOpacity));
		p.drawRoundedRect(rect, radius, radius);
		PaintChipIcon(
			p,
			item.chip.type,
			QRectF(
				rect.x() + padding,
				rect.y() + (rect.height() - icon) / 2.,
				icon,
				icon),
			accent);
		auto textLeft = rect.x() + padding + icon + Scaled(kChipIconSkip);
		const auto baseline = rect.y()
			+ (rect.height() - st::normalFont->height) / 2
			+ st::normalFont->ascent;
		if (!item.label.isEmpty()) {
			// The colour of the text, lighter: on a tinted chip it stays
			// easier to read than the grey of the secondary texts.
			p.setFont(st::normalFont);
			p.setPen(anim::with_alpha(st::windowFg->c, kChipLabelOpacity));
			p.drawText(textLeft, baseline, item.label);
			textLeft += item.labelWidth;
		}
		p.setFont(item.label.isEmpty() ? st::normalFont : st::semiboldFont);
		p.setPen(st::windowFg);
		p.drawText(textLeft, baseline, item.shown);
		if (!item.join.isEmpty()) {
			// A small filled button. A person with a colour of their own
			// has it in that colour, with the text that reads better on
			// it; without one it is an active button of the app.
			const auto &join = item.join;
			const auto corner = join.height() / 2.;
			const auto over = (index == _hovered);
			const auto dark = _accent && DarkTextOn(*_accent);
			const auto fill = !_accent
				? (over ? st::activeButtonBgOver : st::activeButtonBg)->c
				: !over
				? (*_accent)
				: dark
				? _accent->darker(kChipJoinOverFactor)
				: _accent->lighter(kChipJoinOverFactor);
			p.setPen(Qt::NoPen);
			p.setBrush(fill);
			p.drawRoundedRect(join, corner, corner);
			const auto &joinFont = st::semiboldFont;
			p.setFont(joinFont);
			p.setPen(!_accent
				? st::activeButtonFg->c
				: dark
				? QColor(0, 0, 0, kChipJoinDarkAlpha)
				: st::windowFgActive->c);
			p.drawText(
				join.x() + (join.width() - joinFont->width(_joinText)) / 2,
				join.y()
					+ (join.height() - joinFont->height) / 2
					+ joinFont->ascent,
				_joinText);
		}
		++index;
	}
}

int ChipsView::joinAt(QPoint point) const {
	// The whole end of the chip around the button takes the click.
	const auto inset = Scaled(kChipJoinInset);
	const auto around = QMargins(inset, inset, inset, inset);
	auto index = 0;
	for (const auto &item : _items) {
		if (!item.join.isEmpty()
			&& item.join.marginsAdded(around).contains(point)) {
			return index;
		}
		++index;
	}
	return -1;
}

void ChipsView::setHovered(int index) {
	if (_hovered == index) {
		return;
	}
	_hovered = index;
	setCursor((index >= 0) ? style::cur_pointer : style::cur_default);
	update();
}

void ChipsView::mouseMoveEvent(QMouseEvent *e) {
	setHovered(joinAt(e->pos()));
}

void ChipsView::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_pressed = joinAt(e->pos());
	}
}

void ChipsView::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = std::exchange(_pressed, -1);
	if (e->button() != Qt::LeftButton
		|| pressed < 0
		|| pressed != joinAt(e->pos())
		|| pressed >= int(_items.size())) {
		return;
	}
	const auto code = _items[pressed].chip.joinCode;
	if (const auto callback = _join; callback && !code.isEmpty()) {
		callback(code);
	}
}

void ChipsView::leaveEventHook(QEvent *e) {
	setHovered(-1);
}

// What the card shows: the chips, and under them the «Oblivion» block of
// a profile (the status with its emoji, the public playlists and presets).
struct CardData {
	std::optional<Profile> profile;
	std::vector<Chip> chips;
	bool self = false; // The own profile: «Изменить».
	bool preview = false; // In the editor: the block is always there.

	[[nodiscard]] bool profileShown() const {
		return profile && (preview || HasVisibleContent(*profile));
	}
	[[nodiscard]] bool empty() const {
		return chips.empty() && !profileShown();
	}
	friend inline bool operator==(
		const CardData&,
		const CardData&) = default;
};

class ProfileCard final : public Ui::RpWidget {
public:
	ProfileCard(QWidget *parent, style::margins padding, bool framed);

	void setData(CardData data);
	void setCallbacks(
		Fn<void(QString)> join,
		Fn<void()> shared,
		Fn<void()> edit);
	[[nodiscard]] bool isEmpty() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	void refreshStatus();

	const style::margins _padding;
	const bool _framed = false;
	const not_null<ChipsView*> _chips;
	const not_null<Ui::LinkButton*> _shared;
	const not_null<Ui::LinkButton*> _edit;
	CardData _data;
	Ui::Text::String _status;
	bool _statusHint = false; // Not a status: what to do to get the block.
	int _titleTop = 0;
	int _statusTop = 0;

	// A look («Тема Oblivion») that has cards puts the block of a profile
	// page into one, this is where it lies.
	QRect _card;

};

ProfileCard::ProfileCard(QWidget *parent, style::margins padding, bool framed)
: RpWidget(parent)
, _padding(padding)
, _framed(framed)
, _chips(Ui::CreateChild<ChipsView>(this))
, _shared(Ui::CreateChild<Ui::LinkButton>(this, QString()))
, _edit(Ui::CreateChild<Ui::LinkButton>(
	this,
	tr::lng_oblivion_social_block_edit(tr::now))) {
	_chips->hide();
	_shared->hide();
	_edit->hide();

	// A look has its own chips, type and card: everything is laid out
	// again (the chips have just done that for themselves).
	Look::Updates(
	) | rpl::on_next([=] {
		refreshStatus();
		if (width() > 0) {
			resizeToWidth(width());
		}
		update();
	}, lifetime());
}

void ProfileCard::refreshStatus() {
	if (!_data.profileShown()) {
		_statusHint = false;
		_status = Ui::Text::String();
		return;
	}
	// The preview of a profile that has nothing to show yet says how
	// to get the block, instead of a title with nothing under it.
	_statusHint = _data.preview && !HasVisibleContent(*_data.profile);

	// «Ночной эфир» and «Тишина» write the status in a heavier type.
	const auto heavy = !_statusHint && Look::CapsLabels();
	_status.setText(
		heavy ? st::semiboldTextStyle : st::defaultTextStyle,
		(_statusHint
			? tr::lng_oblivion_social_editor_preview_empty(tr::now)
			: StatusLine(*_data.profile)),
		Ui::NameTextOptions());
}

void ProfileCard::setData(CardData data) {
	if (_data == data) {
		return;
	}
	_data = std::move(data);
	_chips->setChips(
		_data.chips,
		_data.profile ? _data.profile->accent : std::nullopt);
	refreshStatus();
	_shared->setText(_data.profileShown()
		? SharedLine(*_data.profile)
		: QString());
	if (width() > 0) {
		resizeToWidth(width());
	}
	update();
}

void ProfileCard::setCallbacks(
		Fn<void(QString)> join,
		Fn<void()> shared,
		Fn<void()> edit) {
	_chips->setJoinCallback(std::move(join));
	_shared->setClickedCallback(std::move(shared));
	_edit->setClickedCallback(std::move(edit));
}

bool ProfileCard::isEmpty() const {
	return _data.empty();
}

int ProfileCard::resizeGetHeight(int newWidth) {
	const auto profile = _data.profileShown();
	const auto hasChips = !_data.chips.empty();
	const auto statusShown = profile && !_status.isEmpty();
	const auto sharedShown = profile
		&& !SharedLine(*_data.profile).isEmpty();
	const auto editShown = profile && _data.self && !_data.preview;
	_chips->setVisible(hasChips);
	_shared->setVisible(sharedShown);
	_edit->setVisible(editShown);
	if (_data.empty()) {
		return 0;
	}
	const auto left = _padding.left();
	const auto inner = std::max(newWidth - left - _padding.right(), 1);
	const auto skip = Scaled(kCardSkip);

	// «Родной, но лучше» and «Ночной эфир»: the block of a profile page
	// is a card. It has the edges of the chips over it, its content
	// stands inside.
	const auto look = Look::Current();
	const auto carded = profile
		&& !_framed
		&& (look == Look::kNative || look == Look::kNightAir);
	const auto inset = carded ? Scaled(kBlockCardPadding) : 0;
	const auto blockLeft = left + inset;
	const auto blockInner = std::max(inner - 2 * inset, 1);
	_card = QRect();
	auto top = _padding.top();
	if (hasChips) {
		_chips->resizeToWidth(inner);
		_chips->moveToLeft(left, top, newWidth);
		top += _chips->height();
	}
	if (profile) {
		if (hasChips) {
			top += 2 * skip;
		}
		const auto cardTop = top;
		top += inset;
		const auto line = st::semiboldFont->height;
		_titleTop = top;
		if (editShown) {
			_edit->moveToRight(
				_padding.right() + inset,
				top + (line - _edit->height()) / 2,
				newWidth);
		}
		top += line;
		if (statusShown) {
			top += skip;
			_statusTop = top;
			top += std::min(
				_status.countHeight(blockInner),
				kStatusLines * st::defaultTextStyle.font->height);
		}
		if (sharedShown) {
			top += skip;
			_shared->moveToLeft(blockLeft, top, newWidth);
			top += _shared->height();
		}
		if (carded) {
			top += inset;
			_card = QRect(left, cardTop, inner, top - cardTop);
		}
	}
	return top + _padding.bottom();
}

void ProfileCard::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	if (_data.empty()) {
		return;
	}
	// With the plain look Look::PaintCard() is the rounded rect that was
	// always here; a look paints its own card (or, in «Тишина», only a
	// hairline along the bottom).
	if (_framed) {
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = Scaled(kCardRadius);
		Look::PaintCard(p, QRectF(rect()), st::windowBgOver->c, radius);
	} else if (!_card.isEmpty()) {
		Look::PaintCard(
			p,
			QRectF(_card),
			st::windowBgOver->c,
			Scaled(kCardRadius));
	}
	if (!_data.profileShown()) {
		return;
	}
	// The content of a card stands inside it, see resizeGetHeight().
	const auto inset = _card.isEmpty() ? 0 : Scaled(kBlockCardPadding);
	const auto left = _padding.left() + inset;
	const auto inner = std::max(
		width() - left - _padding.right() - inset,
		1);
	// The mark keeps the colour of the person in every look.
	const auto quiet = Look::Is(Look::kSilence);
	const auto accent = _data.profile->accent.value_or(quiet
		? st::windowSubTextFg->c
		: st::windowActiveTextFg->c);
	const auto line = st::semiboldFont->height;
	const auto glyph = Scaled(kCardGlyph);
	const auto glyphLeft = rtl() ? (width() - left - glyph) : left;
	const auto title = tr::lng_oblivion_social_block_title(tr::now);
	Badge::Paint(
		p,
		QRect(glyphLeft, _titleTop + (line - glyph) / 2, glyph, glyph),
		accent);
	if (Look::CapsLabels()) {
		// «Ночной эфир» and «Тишина»: a quiet label in small capitals.
		const auto font = CapsFont();
		const auto metrics = QFontMetrics(font);
		p.setFont(font);
		p.setPen(st::windowSubTextFg);
		p.drawTextLeft(
			left + glyph + Scaled(kCardSkip + 2),
			_titleTop + (line - metrics.height()) / 2,
			width(),
			title.toUpper());
	} else {
		p.setFont(st::semiboldFont);
		p.setPen(st::windowActiveTextFg);
		p.drawTextLeft(
			left + glyph + Scaled(kCardSkip),
			_titleTop,
			width(),
			title);
	}
	if (!_status.isEmpty()) {
		p.setPen(_statusHint ? st::windowSubTextFg : st::windowFg);
		_status.drawLeftElided(
			p,
			left,
			_statusTop,
			inner,
			width(),
			kStatusLines);
	}
}

[[nodiscard]] style::margins InfoCardPadding() {
	return style::margins(
		st::infoProfileLabeledPadding.left(),
		Scaled(kCardTop),
		st::infoProfileLabeledPadding.right(),
		Scaled(kCardBottom));
}

[[nodiscard]] style::margins FramedCardPadding() {
	const auto side = Scaled(kCardRadius) + Scaled(kCardSkip);
	return style::margins(side, Scaled(kCardTop), side, Scaled(kCardTop));
}

// A row of a person: the userpic (of the Telegram account when there is
// one, initials otherwise), the name, a line under it and an optional
// link on the right.
struct Person {
	uint64 id = 0;
	QString name;
	QString about;
	bool aboutActive = false;
	QString action;
	QString joinCode;
	PeerData *peer = nullptr;

	friend inline bool operator==(const Person&, const Person&) = default;
};

class PersonRow final : public Ui::AbstractButton {
public:
	PersonRow(QWidget *parent, Person person);

	void setActionCallback(Fn<void()> callback);

protected:
	void paintEvent(QPaintEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;
	int resizeGetHeight(int newWidth) override;

private:
	const style::PeerListItem &_st;
	const Person _person;
	Ui::Text::String _name;
	Ui::Text::String _about;
	Ui::PeerUserpicView _userpicView;
	const Ui::EmptyUserpic _initials;
	Ui::LinkButton *_action = nullptr;

	// The peer is not touched after its session has gone.
	base::weak_ptr<Main::Session> _session;

};

PersonRow::PersonRow(QWidget *parent, Person person)
: AbstractButton(parent)
, _st(st::peerListBoxItem)
, _person(std::move(person))
, _name(_st.nameStyle, _person.name, Ui::NameTextOptions())
, _about(st::defaultTextStyle, _person.about, Ui::NameTextOptions())
, _initials(
	Ui::EmptyUserpic::UserpicColor(
		Ui::EmptyUserpic::ColorIndex(_person.id)),
	_person.name) {
	if (const auto peer = _person.peer) {
		_session = base::make_weak(&peer->session());

		// The userpic may still be on its way from the cloud.
		peer->session().downloaderTaskFinished(
		) | rpl::on_next([=] {
			update();
		}, lifetime());
	}
	if (!_person.action.isEmpty()) {
		_action = Ui::CreateChild<Ui::LinkButton>(this, _person.action);
	}
}

void PersonRow::setActionCallback(Fn<void()> callback) {
	if (_action) {
		_action->setClickedCallback(std::move(callback));
	}
}

void PersonRow::onStateChanged(State was, StateChangeSource source) {
	update();
}

int PersonRow::resizeGetHeight(int newWidth) {
	if (_action) {
		_action->moveToRight(
			_st.photoPosition.x(),
			(_st.height - _action->height()) / 2,
			newWidth);
	}
	return _st.height;
}

void PersonRow::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto over = isOver() || isDown();
	// The looks («Тема Oblivion»): a rounded row under the mouse where
	// the look has cards, hairlines between the rows in «Тишина».
	const auto look = Look::Current();
	if (over && (look == Look::kNative || look == Look::kNightAir)) {
		auto hq = PainterHighQualityEnabler(p);
		const auto inset = Scaled(kRowHoverInset);
		const auto radius = Look::RowRadius(0);
		p.setPen(Qt::NoPen);
		p.setBrush(_st.button.textBgOver);
		p.drawRoundedRect(
			QRectF(rect()).marginsRemoved(QMarginsF(inset, 1, inset, 1)),
			radius,
			radius);
	} else if (over) {
		p.fillRect(rect(), _st.button.textBgOver);
	}
	if (look == Look::kSilence) {
		const auto from = _st.namePosition.x();
		Look::PaintDivider(
			p,
			QRectF(
				from,
				height() - st::lineWidth,
				width() - from - _st.photoPosition.x(),
				st::lineWidth),
			st::shadowFg->c);
	}
	const auto outer = width();
	const auto photoLeft = _st.photoPosition.x();
	const auto photoTop = _st.photoPosition.y();
	if (_person.peer && _session) {
		_person.peer->paintUserpicLeft(
			p,
			_userpicView,
			photoLeft,
			photoTop,
			outer,
			_st.photoSize);
	} else {
		_initials.paintCircle(p, photoLeft, photoTop, outer, _st.photoSize);
	}
	const auto right = _action
		? (_action->width() + 2 * photoLeft)
		: photoLeft;
	const auto available = outer - _st.namePosition.x() - right;
	if (available <= 0) {
		return;
	}
	// A row with nothing under the name has it in the middle, next to
	// the userpic, not hanging at the top.
	const auto nameTop = _person.about.isEmpty()
		? ((_st.height - _st.nameStyle.font->height) / 2)
		: _st.namePosition.y();
	p.setPen(_st.nameFg);
	_name.drawLeftElided(
		p,
		_st.namePosition.x(),
		nameTop,
		available,
		outer);
	if (_person.about.isEmpty()) {
		return;
	}
	p.setPen(_person.aboutActive
		? _st.statusFgActive
		: over
		? _st.statusFgOver
		: _st.statusFg);
	_about.drawLeftElided(
		p,
		_st.statusPosition.x(),
		_st.statusPosition.y(),
		available,
		outer);
}

// A row of small square cells to choose one of: the emoji of the status,
// the accent colour. The cells are painted by the owner. A full line is
// kCellsPerRow cells spread evenly over the whole width.
class ChoiceStrip final : public Ui::RpWidget {
public:
	using Paint = Fn<void(QPainter &p, int index, QRect cell)>;

	ChoiceStrip(QWidget *parent, int count, int selected, Paint paint);

	[[nodiscard]] int selected() const;
	[[nodiscard]] rpl::producer<int> selectedChanges() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	[[nodiscard]] QRect cellRect(int index) const;
	[[nodiscard]] int indexAt(QPoint point) const;
	void setHovered(int index);

	const int _count = 0;
	const int _cell = 0;
	const Paint _paint;
	int _selected = 0;
	int _hovered = -1;
	int _perRow = 1;
	double _pitch = 0.; // From the left of a cell to the left of the next.
	rpl::event_stream<int> _changes;

};

ChoiceStrip::ChoiceStrip(QWidget *parent, int count, int selected, Paint paint)
: RpWidget(parent)
, _count(count)
, _cell(Scaled(kCellSize))
, _paint(std::move(paint))
, _selected(std::clamp(selected, 0, std::max(count - 1, 0))) {
	setMouseTracking(true);
}

int ChoiceStrip::selected() const {
	return _selected;
}

rpl::producer<int> ChoiceStrip::selectedChanges() const {
	return _changes.events();
}

int ChoiceStrip::resizeGetHeight(int newWidth) {
	_perRow = std::clamp(newWidth / _cell, 1, kCellsPerRow);
	_pitch = (_perRow > 1)
		? std::max((newWidth - _cell) / double(_perRow - 1), double(_cell))
		: double(_cell);
	const auto rows = (_count + _perRow - 1) / _perRow;
	return rows * _cell;
}

QRect ChoiceStrip::cellRect(int index) const {
	return QRect(
		qRound((index % _perRow) * _pitch),
		(index / _perRow) * _cell,
		_cell,
		_cell);
}

int ChoiceStrip::indexAt(QPoint point) const {
	if (point.x() < 0 || point.y() < 0 || _pitch <= 0.) {
		return -1;
	}
	const auto row = point.y() / _cell;
	const auto column = int(point.x() / _pitch);
	for (const auto candidate : { column, column + 1 }) {
		const auto index = row * _perRow + candidate;
		if (candidate < _perRow
			&& index < _count
			&& cellRect(index).contains(point)) {
			return index;
		}
	}
	return -1;
}

void ChoiceStrip::setHovered(int index) {
	if (_hovered == index) {
		return;
	}
	_hovered = index;
	setCursor((index >= 0) ? style::cur_pointer : style::cur_default);
	update();
}

void ChoiceStrip::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	// «Тишина» has almost square shapes.
	const auto radius = Scaled(Look::HasCards()
		? kCellRadius
		: kFlatCellRadius);
	const auto inset = Scaled(2);
	for (auto i = 0; i != _count; ++i) {
		const auto cell = cellRect(i);
		if (!cell.intersects(e->rect())) {
			continue;
		}
		const auto inner = cell.marginsRemoved(
			{ inset, inset, inset, inset });
		if (i == _selected || i == _hovered) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgOver);
			p.drawRoundedRect(inner, radius, radius);
			if (i == _selected) {
				auto pen = st::windowActiveTextFg->p;
				pen.setWidthF(style::ConvertScaleExact(1.5));
				p.setPen(pen);
				p.setBrush(Qt::NoBrush);
				p.drawRoundedRect(
					QRectF(inner).marginsRemoved({ 0.75, 0.75, 0.75, 0.75 }),
					radius,
					radius);
			}
		}
		_paint(p, i, cell);
	}
}

void ChoiceStrip::mouseMoveEvent(QMouseEvent *e) {
	setHovered(indexAt(e->pos()));
}

void ChoiceStrip::mousePressEvent(QMouseEvent *e) {
	const auto index = indexAt(e->pos());
	if (e->button() != Qt::LeftButton || index < 0 || index == _selected) {
		return;
	}
	_selected = index;
	update();
	_changes.fire_copy(index);
}

void ChoiceStrip::leaveEventHook(QEvent *e) {
	setHovered(-1);
}

// "Nothing" in a strip: a circle crossed out.
void PaintNoneCell(QPainter &p, QRect cell) {
	auto hq = PainterHighQualityEnabler(p);
	const auto size = Scaled(kSwatchSize);
	const auto rect = QRectF(
		cell.x() + (cell.width() - size) / 2.,
		cell.y() + (cell.height() - size) / 2.,
		size,
		size).marginsRemoved({ 1., 1., 1., 1. });
	auto pen = st::windowSubTextFg->p;
	pen.setWidthF(style::ConvertScaleExact(1.5));
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	p.drawEllipse(rect);
	const auto shift = rect.width() * 0.35;
	p.drawLine(
		rect.center() + QPointF(-shift, shift),
		rect.center() + QPointF(shift, -shift));
}

// The emoji of the status: "none", what the account has now (if it is
// not one of the quick choices) and the quick choices.
[[nodiscard]] std::vector<EmojiPtr> EmojiChoices(const QString &current) {
	auto result = std::vector<EmojiPtr>{ nullptr };
	const auto now = FindEmoji(current);
	for (const auto text : kStatusEmoji) {
		const auto emoji = FindEmoji(QString::fromUtf8(text));
		if (emoji && !ranges::contains(result, emoji)) {
			result.push_back(emoji);
		}
	}
	if (now && !ranges::contains(result, now)) {
		result.insert(begin(result) + 1, now);
	}
	return result;
}

[[nodiscard]] std::vector<std::optional<QColor>> ColorChoices(
		const QString &current) {
	auto result = std::vector<std::optional<QColor>>{ std::nullopt };
	for (const auto rgb : kAccentColors) {
		result.push_back(QColor(QRgb(0xFF000000U | rgb)));
	}
	const auto now = QColor(current);
	if (!current.isEmpty()
		&& now.isValid()
		&& !ranges::contains(result, std::optional<QColor>(now))) {
		result.insert(begin(result) + 1, now);
	}
	return result;
}

// The avatar as the server takes it: a small JPEG.
[[nodiscard]] QByteArray EncodeAvatar(QImage image) {
	if (image.isNull()) {
		return QByteArray();
	}
	image = image.convertToFormat(QImage::Format_RGB32);
	for (const auto quality : { 88, 76, 62, 48, 34 }) {
		auto bytes = QByteArray();
		auto buffer = QBuffer(&bytes);
		if (image.save(&buffer, "JPG", quality)
			&& !bytes.isEmpty()
			&& bytes.size() <= kAvatarBytes) {
			return bytes;
		}
	}
	return QByteArray();
}

// The editor «Мой профиль Oblivion». Everything session-bound comes in
// as callbacks, so the box is shown in a snapshot scene as it is. The
// callbacks tell the user about a failure themselves and answer with
// false (std::nullopt); they may answer after the box is gone.
//
// setShown is «Показывать в профиле», setListed is «Показывать всем в
// «Общих наборах»»: two switches for two different audiences. Both
// answer with the item as the server has it after the change.
struct EditorArgs {
	using Changed = Fn<void(std::optional<SharedItem>)>;

	std::shared_ptr<Ui::Show> show;
	Cloud::Me me;
	Fn<void(
		Fn<void(std::vector<SharedItem>)> done,
		Fn<void()> fail)> loadShared;
	Fn<void(SharedItem item, bool shown, Changed done)> setShown;
	Fn<void(SharedItem item, bool listed, Changed done)> setListed;
	Fn<void(QJsonObject patch, Fn<void(bool)> done)> save;
	Fn<void(Fn<void(bool)> done)> setPhoto;
	Fn<void(Fn<void(bool)> done)> removePhoto;

	// The account is not connected any more: nothing in the box can be
	// done and what was asked will not be answered, so the box closes.
	rpl::producer<> closeRequests;
};

void EditorBox(not_null<Ui::GenericBox*> box, EditorArgs &&args) {
	box->setTitle(tr::lng_oblivion_social_my_profile());
	box->setWidth(st::boxWideWidth);

	struct State {
		std::vector<SharedItem> shared;
		rpl::variable<bool> hasPhoto = false;
		bool saving = false;
		bool photoBusy = false;
	};
	const auto state = box->lifetime().make_state<State>();
	state->hasPhoto = (args.me.avatarRev > 0);
	const auto userId = args.me.id;
	const auto content = box->verticalLayout();
	const auto emojiList = EmojiChoices(args.me.statusEmoji);
	const auto colorList = ColorChoices(args.me.accent);

	Ui::AddSubsectionTitle(
		content,
		tr::lng_oblivion_social_editor_preview());
	const auto card = content->add(
		object_ptr<ProfileCard>(content, FramedCardPadding(), true),
		RowPadding());

	const auto name = content->add(
		object_ptr<Ui::InputField>(
			content,
			st::defaultInputField,
			tr::lng_oblivion_social_editor_name(),
			args.me.name.left(kNameLimit)),
		RowPadding());
	name->setMaxLength(kNameLimit);
	const auto status = content->add(
		object_ptr<Ui::InputField>(
			content,
			st::defaultInputField,
			tr::lng_oblivion_social_editor_status(),
			args.me.statusText.left(kStatusLimit)),
		RowPadding());
	status->setMaxLength(kStatusLimit);

	Ui::AddSubsectionTitle(content, tr::lng_oblivion_social_editor_emoji());
	const auto emojiNow = FindEmoji(args.me.statusEmoji);
	const auto emojiIndex = emojiNow
		? int(ranges::find(emojiList, emojiNow) - begin(emojiList))
		: 0;
	const auto emoji = content->add(
		object_ptr<ChoiceStrip>(
			content,
			int(emojiList.size()),
			emojiIndex,
			[=](QPainter &p, int index, QRect cell) {
				const auto value = emojiList[index];
				if (!value) {
					PaintNoneCell(p, cell);
					return;
				}
				const auto size = Ui::Emoji::GetSizeLarge();
				const auto side = size / style::DevicePixelRatio();
				Ui::Emoji::Draw(
					p,
					value,
					size,
					cell.x() + (cell.width() - side) / 2,
					cell.y() + (cell.height() - side) / 2);
			}),
		StripPadding());

	Ui::AddSubsectionTitle(content, tr::lng_oblivion_social_editor_accent());
	const auto colorNow = QColor(args.me.accent);
	const auto colorFound = ranges::find(
		colorList,
		std::optional<QColor>(colorNow));
	const auto colorIndex = (args.me.accent.isEmpty()
		|| !colorNow.isValid()
		|| colorFound == end(colorList))
		? 0
		: int(colorFound - begin(colorList));
	const auto color = content->add(
		object_ptr<ChoiceStrip>(
			content,
			int(colorList.size()),
			colorIndex,
			[=](QPainter &p, int index, QRect cell) {
				const auto &value = colorList[index];
				if (!value) {
					PaintNoneCell(p, cell);
					return;
				}
				auto hq = PainterHighQualityEnabler(p);
				const auto size = Scaled(kSwatchSize);
				p.setPen(Qt::NoPen);
				p.setBrush(*value);
				p.drawEllipse(QRectF(
					cell.x() + (cell.width() - size) / 2.,
					cell.y() + (cell.height() - size) / 2.,
					size,
					size));
			}),
		StripPadding() - style::margins(0, 0, 0, st::boxLittleSkip / 2));

	// The preview shows the colour only on a small mark: what else it
	// paints is said in words.
	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			tr::lng_oblivion_social_editor_accent_about(),
			st::boxDividerLabel),
		RowPadding());

	const auto refresh = [=] {
		auto profile = Profile{ .id = userId };
		profile.statusText = status->getLastText().simplified().left(
			kStatusLimit);
		if (const auto value = emojiList[emoji->selected()]) {
			profile.statusEmoji = value->text();
		}
		profile.accent = colorList[color->selected()];
		for (const auto &item : state->shared) {
			if (item.shown) {
				++(item.playlist
					? profile.publicPlaylists
					: profile.publicPresets);
			}
		}
		card->setData({ .profile = std::move(profile), .preview = true });
	};
	status->changes() | rpl::on_next(refresh, status->lifetime());
	emoji->selectedChanges() | rpl::on_next([=](int) {
		refresh();
	}, emoji->lifetime());
	color->selectedChanges() | rpl::on_next([=](int) {
		refresh();
	}, color->lifetime());
	refresh();

	// What other people see of the own playlists and presets that are on
	// the server. Two different switches, each sent at once:
	//  - «Показывать в профиле»: for those who see the profile. Playlists
	//    always have it; presets only where the server keeps the profile
	//    apart from the gallery (SharedItem::profileSwitch);
	//  - «Показывать всем в «Общих наборах»»: a preset in the gallery,
	//    for everybody who uses Oblivion with the name of the owner next
	//    to it. Turning it on is asked about first, turning it off is not.
	const auto list = content->add(object_ptr<Ui::VerticalLayout>(content));
	const auto clear = [=] {
		while (list->count()) {
			delete list->widgetAt(0);
		}
	};
	const auto note = [=](const QString &text) {
		clear();
		Ui::AddSubsectionTitle(list, tr::lng_oblivion_social_editor_shared());
		list->add(
			object_ptr<Ui::FlatLabel>(list, text, st::boxDividerLabel),
			RowPadding());
		list->resizeToWidth(content->width());
	};
	const auto about = [=](rpl::producer<QString> text) {
		list->add(
			object_ptr<Ui::FlatLabel>(
				list,
				std::move(text),
				st::boxDividerLabel),
			st::boxRowPadding + style::margins(
				0,
				st::boxLittleSkip / 2,
				0,
				st::boxLittleSkip));
	};
	const auto show = args.show;
	const auto setShown = args.setShown;
	const auto setListed = args.setListed;
	const auto addSwitch = [=](int index, bool gallery) {
		const auto &item = state->shared[index];
		const auto button = list->add(object_ptr<Ui::SettingsButton>(
			list,
			rpl::single(SharedItemTitle(item)),
			st::settingsButtonNoIcon));
		const auto value = button->lifetime().make_state<
			rpl::variable<bool>>(gallery ? item.listed : item.shown);
		const auto pending = button->lifetime().make_state<bool>(false);
		const auto alive = QPointer<Ui::SettingsButton>(button);
		button->toggleOn(value->value(), true);
		const auto send = [=](bool now) {
			const auto &callback = gallery ? setListed : setShown;
			if (!alive || *pending || !callback) {
				return;
			}
			*pending = true;
			callback(
				state->shared[index],
				now,
				crl::guard(button, [=](std::optional<SharedItem> result) {
					*pending = false;
					if (result) {
						*value = gallery ? result->listed : result->shown;
						state->shared[index] = std::move(*result);
						refresh();
					}
				}));
		};
		button->setClickedCallback([=] {
			const auto now = !value->current();
			if (*pending) {
				return;
			} else if (!gallery || !now) {
				send(now);
				return;
			} else if (!show || !show->valid()) {
				return;
			}
			const auto title = state->shared[index].title.simplified();
			show->showBox(Ui::MakeConfirmBox({
				.text = tr::lng_oblivion_social_gallery_confirm(
					tr::now,
					lt_title,
					title.isEmpty()
						? SharedItemTitle(state->shared[index])
						: title),
				.confirmed = [=](Fn<void()> close) {
					close();
					send(true);
				},
				.confirmText = tr::lng_oblivion_social_gallery_confirm_yes(),
				.title = tr::lng_oblivion_social_gallery_confirm_title(),
			}));
		});
	};
	const auto fill = [=](std::vector<SharedItem> items) {
		state->shared = std::move(items);
		if (state->shared.empty()) {
			note(tr::lng_oblivion_social_editor_shared_empty(tr::now));
			refresh();
			return;
		}
		clear();
		auto profile = std::vector<int>();
		auto presets = std::vector<int>();
		auto apart = true;
		for (auto i = 0, count = int(state->shared.size()); i != count; ++i) {
			const auto &item = state->shared[i];
			if (item.playlist || item.profileSwitch) {
				profile.push_back(i);
			}
			if (!item.playlist) {
				presets.push_back(i);
				apart = apart && item.profileSwitch;
			}
		}
		if (!profile.empty()) {
			Ui::AddSubsectionTitle(
				list,
				tr::lng_oblivion_social_editor_shared());
			for (const auto index : profile) {
				addSwitch(index, false);
			}
			about(tr::lng_oblivion_social_editor_shared_about());
		}
		if (!presets.empty()) {
			Ui::AddSubsectionTitle(
				list,
				tr::lng_oblivion_social_editor_gallery());
			for (const auto index : presets) {
				addSwitch(index, true);
			}
			about(apart
				? tr::lng_oblivion_social_editor_gallery_apart()
				: tr::lng_oblivion_social_editor_gallery_about());
		}
		list->resizeToWidth(content->width());
		refresh();
	};
	note(tr::lng_oblivion_social_shared_loading(tr::now));
	if (const auto load = args.loadShared) {
		load(crl::guard(box, fill), crl::guard(box, [=] {
			note(tr::lng_oblivion_social_shared_failed(tr::now));
		}));
	}
	if (auto closes = std::move(args.closeRequests)) {
		std::move(
			closes
		) | rpl::on_next([=] {
			box->closeBox();
		}, box->lifetime());
	}

	// The photo: sent only by a click here.
	Ui::AddSubsectionTitle(content, tr::lng_oblivion_social_editor_photo());
	const auto photoDone = [=](bool value) {
		return crl::guard(box, [=](bool success) {
			state->photoBusy = false;
			if (success) {
				state->hasPhoto = value;
			}
		});
	};
	const auto setPhoto = args.setPhoto;
	const auto removePhoto = args.removePhoto;
	content->add(object_ptr<Ui::SettingsButton>(
		content,
		tr::lng_oblivion_social_editor_photo_set(),
		st::settingsButtonNoIcon)
	)->setClickedCallback([=] {
		if (!state->photoBusy && setPhoto) {
			state->photoBusy = true;
			setPhoto(photoDone(true));
		}
	});
	const auto remove = content->add(
		object_ptr<Ui::SlideWrap<Ui::SettingsButton>>(
			content,
			object_ptr<Ui::SettingsButton>(
				content,
				tr::lng_oblivion_social_editor_photo_remove(),
				st::settingsAttentionButton)));
	remove->setDuration(0)->toggleOn(state->hasPhoto.value());
	remove->entity()->setClickedCallback([=] {
		if (!state->photoBusy && removePhoto) {
			state->photoBusy = true;
			removePhoto(photoDone(false));
		}
	});
	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			tr::lng_oblivion_social_editor_photo_about(),
			st::boxDividerLabel),
		st::boxRowPadding + style::margins(0, st::boxLittleSkip / 2, 0, 0));

	const auto save = args.save;
	const auto submit = [=] {
		if (state->saving || !save) {
			return;
		}
		auto profile = QJsonObject();
		profile.insert(
			u"status_text"_q,
			status->getLastText().simplified().left(kStatusLimit));
		const auto emojiValue = emojiList[emoji->selected()];
		profile.insert(
			u"status_emoji"_q,
			emojiValue ? emojiValue->text() : QString());
		const auto &colorValue = colorList[color->selected()];
		profile.insert(
			u"accent"_q,
			colorValue ? colorValue->name(QColor::HexRgb) : QString());
		auto patch = QJsonObject();
		patch.insert(
			u"name"_q,
			name->getLastText().simplified().left(kNameLimit));
		patch.insert(u"profile"_q, profile);
		state->saving = true;
		save(std::move(patch), crl::guard(box, [=](bool success) {
			state->saving = false;
			if (success) {
				box->closeBox();
			}
		}));
	};
	name->submits() | rpl::on_next([=] {
		status->setFocus();
	}, name->lifetime());
	status->submits() | rpl::on_next(submit, status->lifetime());
	box->setFocusCallback([=] {
		status->setFocusFast();
	});
	box->addButton(tr::lng_settings_save(), submit);
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
}

// The public playlists and presets of somebody, asked by a click.
struct SharedArgs {
	Fn<void(
		Fn<void(std::vector<SharedItem>)> done,
		Fn<void()> fail)> load;
	Fn<void(SharedItem item)> open;

	// The account is not connected any more: a list that is still being
	// loaded will not come.
	rpl::producer<> aborted;
};

void SharedBox(not_null<Ui::GenericBox*> box, SharedArgs &&args) {
	box->setTitle(tr::lng_oblivion_social_shared_title());
	box->setWidth(st::boxWideWidth);
	box->setMaxHeight(st::boxMaxListHeight);

	const auto content = box->verticalLayout();
	const auto list = content->add(object_ptr<Ui::VerticalLayout>(content));
	const auto note = [=](const QString &text) {
		while (list->count()) {
			delete list->widgetAt(0);
		}
		list->add(
			object_ptr<Ui::FlatLabel>(list, text, st::defaultPeerListAbout),
			EmptyPadding(),
			style::al_justify);
		list->resizeToWidth(content->width());
	};
	const auto open = args.open;
	const auto loading = box->lifetime().make_state<bool>(true);
	const auto fill = [=](std::vector<SharedItem> items) {
		*loading = false;
		if (items.empty()) {
			note(tr::lng_oblivion_social_shared_empty(tr::now));
			return;
		}
		while (list->count()) {
			delete list->widgetAt(0);
		}
		const auto section = [&](bool playlists, rpl::producer<QString> title) {
			auto added = false;
			for (const auto &item : items) {
				if (item.playlist != playlists) {
					continue;
				} else if (!std::exchange(added, true)) {
					if (list->count()) {
						Ui::AddSkip(list);
					}
					Ui::AddSubsectionTitle(list, rpl::duplicate(title));
				}
				const auto detail = playlists
					? tr::lng_oblivion_social_shared_tracks(
						tr::now,
						lt_count,
						item.count)
					: (item.kind == u"video"_q)
					? tr::lng_oblivion_social_shared_video(tr::now)
					: tr::lng_oblivion_social_shared_photo(tr::now);
				::Settings::AddButtonWithLabel(
					list,
					rpl::single(item.title.simplified()),
					rpl::single(detail),
					st::settingsButtonNoIcon
				)->setClickedCallback([=] {
					const auto callback = open;
					const auto chosen = item;
					box->closeBox();
					if (callback) {
						callback(chosen);
					}
				});
			}
		};
		section(true, tr::lng_oblivion_social_shared_playlists());
		section(false, tr::lng_oblivion_social_shared_presets());
		list->resizeToWidth(content->width());
	};
	const auto failed = [=] {
		if (base::take(*loading)) {
			note(tr::lng_oblivion_social_shared_failed(tr::now));
		}
	};
	note(tr::lng_oblivion_social_shared_loading(tr::now));
	if (const auto load = args.load) {
		load(crl::guard(box, fill), crl::guard(box, failed));
	}
	if (auto aborted = std::move(args.aborted)) {
		std::move(aborted) | rpl::on_next(failed, box->lifetime());
	}
	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
	});
}

// «Друзья в Oblivion».
struct FriendsState {
	Status status = Status::Loading;
	bool hidden = false; // The own profile is shown to nobody.
	std::vector<Person> people;

	friend inline bool operator==(
		const FriendsState&,
		const FriendsState&) = default;
};

struct FriendsArgs {
	rpl::producer<FriendsState> state;
	Fn<void(uint64 id)> open;
	Fn<void(QString code)> join;
	Fn<void()> profile;
};

void FriendsBox(not_null<Ui::GenericBox*> box, FriendsArgs &&args) {
	box->setTitle(tr::lng_oblivion_social_friends());
	box->setWidth(st::boxWideWidth);
	box->setMaxHeight(st::boxMaxListHeight);

	const auto content = box->verticalLayout();
	const auto list = content->add(object_ptr<Ui::VerticalLayout>(content));
	const auto last = box->lifetime().make_state<
		std::optional<FriendsState>>();
	const auto open = args.open;
	const auto join = args.join;
	const auto rebuild = [=](const FriendsState &state) {
		if (*last == state) {
			return;
		}
		*last = state;
		while (list->count()) {
			delete list->widgetAt(0);
		}
		const auto note = [&](const QString &text) {
			list->add(
				object_ptr<Ui::FlatLabel>(list, text, st::boxDividerLabel),
				RowPadding());
		};
		// A list without rows says what is going on in the middle of the
		// box, the same way as «Пока никого нет» below.
		const auto about = [&](TextWithEntities text) {
			list->add(
				object_ptr<Ui::FlatLabel>(
					list,
					rpl::single(std::move(text)),
					st::defaultPeerListAbout),
				EmptyPadding(),
				style::al_justify);
		};
		const auto empty = state.people.empty();
		if (state.status == Status::Offline) {
			if (empty) {
				about({ tr::lng_oblivion_social_friends_offline_empty(
					tr::now) });
			} else {
				note(tr::lng_oblivion_social_friends_offline(tr::now));
			}
		} else if (state.status == Status::Loading && empty) {
			about({ tr::lng_oblivion_social_friends_loading(tr::now) });
		}
		for (const auto &person : state.people) {
			const auto id = person.id;
			const auto code = person.joinCode;
			const auto row = list->add(object_ptr<PersonRow>(list, person));
			// The box may take the row (and this callback) away with it:
			// everything needed after closeBox() is copied first.
			row->setClickedCallback([=] {
				const auto callback = open;
				const auto user = id;
				box->closeBox();
				if (callback) {
					callback(user);
				}
			});
			row->setActionCallback([=] {
				const auto callback = join;
				const auto room = code;
				box->closeBox();
				if (callback) {
					callback(room);
				}
			});
		}
		if (empty && state.status == Status::Ready) {
			about(tr::bold(
				tr::lng_oblivion_social_friends_empty_title(tr::now)
			).append(u"\n\n"_q).append(
				tr::lng_oblivion_social_friends_empty(tr::now)));
		}
		if (state.hidden && state.status != Status::Off) {
			note(tr::lng_oblivion_social_friends_hidden(tr::now));
		}
		list->resizeToWidth(content->width());
	};
	std::move(
		args.state
	) | rpl::on_next(rebuild, box->lifetime());

	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
	});
	if (const auto profile = args.profile) {
		box->addLeftButton(tr::lng_oblivion_social_my_profile(), [=] {
			const auto callback = profile;
			box->closeBox();
			callback();
		});
	}
}

// «Выбранные люди».
struct ChosenArgs {
	rpl::producer<std::vector<Person>> people;
	Fn<void(uint64 id)> remove;
	Fn<void()> add;
};

void ChosenBox(not_null<Ui::GenericBox*> box, ChosenArgs &&args) {
	box->setTitle(tr::lng_oblivion_social_chosen());
	box->setWidth(st::boxWideWidth);
	box->setMaxHeight(st::boxMaxListHeight);

	const auto content = box->verticalLayout();
	Ui::AddDividerText(
		content,
		tr::lng_oblivion_social_chosen_about(),
		st::defaultBoxDividerLabelPadding,
		st::defaultDividerLabel,
		RectPart::Bottom);
	const auto list = content->add(object_ptr<Ui::VerticalLayout>(content));
	const auto remove = args.remove;
	std::move(
		args.people
	) | rpl::on_next([=](const std::vector<Person> &people) {
		while (list->count()) {
			delete list->widgetAt(0);
		}
		if (people.empty()) {
			list->add(
				object_ptr<Ui::FlatLabel>(
					list,
					tr::lng_oblivion_social_chosen_empty(),
					st::defaultPeerListAbout),
				st::boxRowPadding + style::margins(
					0,
					st::boxMediumSkip,
					0,
					st::boxMediumSkip),
				style::al_justify);
		}
		for (const auto &person : people) {
			const auto id = person.id;
			const auto row = list->add(object_ptr<PersonRow>(list, person));
			row->setActionCallback([=] {
				if (remove) {
					remove(id);
				}
			});
		}
		list->resizeToWidth(content->width());
	}, box->lifetime());

	if (const auto add = args.add) {
		box->addButton(tr::lng_oblivion_social_chosen_add(), add);
	}
	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
	});
}

// The contacts to add to the chosen people: a click adds one, the box
// stays open for the next.
class ChosenAddController final : public ContactsBoxController {
public:
	ChosenAddController(
		not_null<Main::Session*> session,
		Fn<bool(uint64)> chosen,
		Fn<bool(not_null<UserData*>)> add);

	void rowClicked(not_null<PeerListRow*> row) override;

protected:
	void prepareViewHook() override;
	std::unique_ptr<PeerListRow> createRow(
		not_null<UserData*> user) override;

private:
	const Fn<bool(uint64)> _chosen;
	const Fn<bool(not_null<UserData*>)> _add;

};

ChosenAddController::ChosenAddController(
	not_null<Main::Session*> session,
	Fn<bool(uint64)> chosen,
	Fn<bool(not_null<UserData*>)> add)
: ContactsBoxController(session, nullptr)
, _chosen(std::move(chosen))
, _add(std::move(add)) {
}

void ChosenAddController::prepareViewHook() {
	delegate()->peerListSetTitle(tr::lng_oblivion_social_chosen_add_title());
}

std::unique_ptr<PeerListRow> ChosenAddController::createRow(
		not_null<UserData*> user) {
	if (user->isSelf()
		|| user->isBot()
		|| user->isServiceUser()
		|| user->isInaccessible()
		|| _chosen(peerToUser(user->id).bare)) {
		return nullptr;
	}
	return ContactsBoxController::createRow(user);
}

void ChosenAddController::rowClicked(not_null<PeerListRow*> row) {
	const auto user = row->peer()->asUser();
	if (user && _add(user)) {
		delegate()->peerListRemoveRow(row);
		delegate()->peerListRefreshRows();
	}
}

[[nodiscard]] bool IsFriend(not_null<UserData*> user) {
	if (user->isSelf()
		|| user->isBot()
		|| user->isServiceUser()
		|| user->isInaccessible()) {
		return false;
	} else if (user->isContact()) {
		return true;
	}
	const auto history = user->owner().historyLoaded(user.get());
	return history && history->inChatList();
}

// The directory the server shows this account, crossed here with the
// contacts and the chats of the account: nothing about them is sent.
[[nodiscard]] FriendsState BuildFriends(not_null<Main::Session*> session) {
	auto result = FriendsState{
		.status = CurrentStatus(session),
		.hidden = (AudienceNow(session, AudienceKind::Profile)
			== Audience::Nobody),
	};
	const auto phrases = LangPhrases();
	const auto rooms = Get().cloudRooms();
	const auto joinText = tr::lng_oblivion_social_chip_join(tr::now);
	const auto plain = tr::lng_oblivion_social_friends_plain(tr::now);
	struct Entry {
		Person person;
		int rank = 0;
		QString key;
	};
	auto entries = std::vector<Entry>();
	for (const auto &profile : Directory(session)) {
		const auto user = session->data().userLoaded(UserId(profile.id));
		if (!user || !IsFriend(user)) {
			continue;
		}
		const auto activity = ActivityOf(session, profile.id);
		const auto chips = BuildChips(activity, phrases, rooms);
		auto person = Person{
			.id = profile.id,
			.name = user->name(),
			.peer = user,
		};
		if (!chips.empty()) {
			person.about = chips.front().text;
			person.aboutActive = true;
			for (const auto &chip : chips) {
				if (!chip.joinCode.isEmpty()) {
					person.action = joinText;
					person.joinCode = chip.joinCode;
					break;
				}
			}
		} else {
			const auto status = StatusLine(profile);
			person.about = status.isEmpty() ? plain : status;
		}
		entries.push_back({
			.person = std::move(person),
			.rank = ActivityRank(activity),
			.key = user->name().toLower(),
		});
	}
	ranges::sort(entries, [](const Entry &a, const Entry &b) {
		return std::tie(a.rank, a.key, a.person.id)
			< std::tie(b.rank, b.key, b.person.id);
	});
	result.people.reserve(entries.size());
	for (auto &entry : entries) {
		result.people.push_back(std::move(entry.person));
	}
	return result;
}

[[nodiscard]] std::vector<Person> ChosenPeople(
		not_null<Main::Session*> session,
		const std::vector<uint64> &ids) {
	auto result = std::vector<Person>();
	const auto remove = tr::lng_oblivion_social_chosen_remove(tr::now);
	for (const auto id : ids) {
		const auto user = session->data().userLoaded(UserId(id));
		result.push_back({
			.id = id,
			.name = (user
				? user->name()
				: tr::lng_oblivion_social_chosen_unknown(
					tr::now,
					lt_name,
					QString::number(id))),
			.action = remove,
			.peer = user,
		});
	}
	return result;
}

// «Выбранные люди» while the list is being edited. Clicks come faster
// than the answers, and the server takes PATCH /v1/me only so often (20
// at once, then one in two seconds): the list is kept here, shown at
// once, and sent as a whole a moment after the last click, one request
// at a time. So adding twenty people is a request or two, not twenty.
//
// The object is shared by the box, the picker of contacts and whatever
// is on its way (the timer, the request), so an edit made right before
// the box was closed is still sent. A request the server refuses puts
// the list back to what the server has, with a toast; only "too often"
// (429) is waited out and tried again, once.
class ChosenEdit final : public std::enable_shared_from_this<ChosenEdit> {
public:
	ChosenEdit(
		not_null<Main::Session*> session,
		std::shared_ptr<Ui::Show> show);

	[[nodiscard]] const std::vector<uint64> &list();
	[[nodiscard]] rpl::producer<> changes() const;
	bool add(uint64 id);
	void remove(uint64 id);

private:
	void sync();
	void edited();
	void sendSoon(crl::time delay);
	void send();
	void done();
	void fail(const Cloud::Error &error);

	const base::weak_ptr<Main::Session> _session;
	const base::weak_ptr<Cloud::Account> _account;
	const std::shared_ptr<Ui::Show> _show;
	std::vector<uint64> _list;
	bool _dirty = false; // Edited here and not sent yet.
	bool _waiting = false; // The timer of the next request runs.
	bool _sending = false; // A request is on its way.
	bool _retried = false;
	rpl::event_stream<> _changes;

};

ChosenEdit::ChosenEdit(
	not_null<Main::Session*> session,
	std::shared_ptr<Ui::Show> show)
: _session(base::make_weak(session))
, _account(base::make_weak(&Cloud::For(session)))
, _show(std::move(show)) {
}

const std::vector<uint64> &ChosenEdit::list() {
	sync();
	return _list;
}

rpl::producer<> ChosenEdit::changes() const {
	return _changes.events();
}

// What the server has replaces the list whenever nothing of this device
// is waiting to be sent or answered.
void ChosenEdit::sync() {
	const auto account = _account.get();
	if (!account) {
		return;
	} else if (!account->ready()) {
		// Nothing can be sent, and what was on its way is dropped with
		// the connection: it will never be answered.
		_dirty = _sending = false;
	}
	if (!_dirty && !_sending) {
		_list = account->me().chosen;
	}
}

bool ChosenEdit::add(uint64 id) {
	sync();
	const auto account = _account.get();
	if (!account || !id || ranges::contains(_list, id)) {
		return false;
	} else if (int64(_list.size()) >= account->limit("chosen", 1000)) {
		Toast(_show, tr::lng_oblivion_social_chosen_full(tr::now));
		return false;
	}
	_list.push_back(id);
	edited();
	return true;
}

void ChosenEdit::remove(uint64 id) {
	sync();
	const auto i = ranges::find(_list, id);
	if (i != end(_list)) {
		_list.erase(i);
		edited();
	}
}

void ChosenEdit::edited() {
	_dirty = true;
	_changes.fire({});
	sendSoon(kChosenSendDelay);
}

void ChosenEdit::sendSoon(crl::time delay) {
	const auto session = _session.get();
	if (!session || _waiting || _sending) {
		return;
	}
	_waiting = true;
	base::call_delayed(delay, session, [self = shared_from_this()] {
		self->_waiting = false;
		self->send();
	});
}

void ChosenEdit::send() {
	const auto account = _account.get();
	if (!account || !_dirty || _sending) {
		return;
	}
	auto ids = QJsonArray();
	for (const auto id : _list) {
		ids.push_back(double(id));
	}
	auto privacy = QJsonObject();
	privacy.insert(u"chosen"_q, ids);
	auto patch = QJsonObject();
	patch.insert(u"privacy"_q, privacy);
	_dirty = false;
	_sending = true;
	const auto self = shared_from_this();
	account->patchMe(std::move(patch), [=] {
		self->done();
	}, [=](const Cloud::Error &error) {
		self->fail(error);
	});
}

void ChosenEdit::done() {
	_sending = false;
	_retried = false;
	if (_dirty) {
		sendSoon(kChosenSendDelay);
	} else {
		sync();
	}
	_changes.fire({});
}

void ChosenEdit::fail(const Cloud::Error &error) {
	_sending = false;
	const auto often = (error.type == Cloud::Error::Type::Http)
		&& (error.status == 429);
	if (often && !std::exchange(_retried, true)) {
		_dirty = true;
		sendSoon(std::clamp(
			error.retryAfter,
			kChosenSendDelay,
			kChosenRetryMax));
		return;
	}
	_retried = false;
	_dirty = false;
	sync();
	_changes.fire({});
	Cloud::ShowError(_show, error);
}

[[nodiscard]] QJsonObject FlagPatch(Flag flag, bool value) {
	auto result = QJsonObject();
	if (flag == Flag::Badge) {
		result.insert(u"badge"_q, value);
		return result;
	}
	auto chips = QJsonObject();
	chips.insert(
		((flag == Flag::ChipListening)
			? u"listening"_q
			: (flag == Flag::ChipRoom)
			? u"room"_q
			: u"online"_q),
		value);
	auto privacy = QJsonObject();
	privacy.insert(u"chips"_q, chips);
	result.insert(u"privacy"_q, privacy);
	return result;
}

[[nodiscard]] CardData CardFor(
		not_null<Main::Session*> session,
		uint64 userId,
		bool self) {
	auto result = CardData{ .self = self };
	if (!self && !Get().cloudProfileShow()) {
		return result;
	}
	result.profile = ProfileOf(session, userId);
	result.chips = BuildChips(
		ActivityOf(session, userId),
		LangPhrases(),
		Get().cloudRooms());
	return result;
}

// Fires when the account stops being connected («Отключиться», a ban,
// an old protocol): the requests that are on their way are dropped then
// and their callbacks are never called.
[[nodiscard]] rpl::producer<> DisconnectedEvents(
		not_null<Main::Session*> session) {
	return Cloud::For(session).readyValue(
	) | rpl::filter([](bool ready) {
		return !ready;
	}) | rpl::to_empty;
}

void ShowShared(
		not_null<Window::SessionController*> controller,
		uint64 userId) {
	const auto session = &controller->session();
	const auto weak = base::make_weak(controller);
	controller->show(Box(SharedBox, SharedArgs{
		.load = [=](
				Fn<void(std::vector<SharedItem>)> done,
				Fn<void()> fail) {
			LoadShared(session, userId, done, [=](const Cloud::Error &) {
				fail();
			});
		},
		.open = [=](SharedItem item) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			} else if (item.playlist) {
				Share::OpenPlaylistLink(strong, item.id);
			} else {
				Share::OpenPresetLink(strong, item.id);
			}
		},
		.aborted = DisconnectedEvents(session),
	}));
}

// A column of sample widgets on the background of a window.
class SceneColumn final : public Ui::VerticalLayout {
public:
	using VerticalLayout::VerticalLayout;

protected:
	void paintEvent(QPaintEvent *e) override {
		QPainter(this).fillRect(e->rect(), st::windowBg);
	}

};

[[nodiscard]] Profile SampleProfile() {
	auto result = Profile{ .id = 111 };
	result.name = SampleText("Аня", "Anna");
	result.badge = true;
	result.statusText = SampleText(
		"сплю до обеда, не будите",
		"sleeping till noon, do not wake me");
	result.statusEmoji = QString::fromUtf8("😴");
	result.accent = QColor(0xff, 0x5c, 0x8a);
	result.publicPlaylists = 2;
	result.publicPresets = 5;
	return result;
}

[[nodiscard]] Activity SampleListening() {
	auto result = Activity{ .userId = 111, .online = true, .listening = true };
	result.title = SampleText("Группа крови", "Blood Type");
	result.performer = SampleText("Кино", "Kino");
	result.durationMs = 285'000;
	return result;
}

[[nodiscard]] Activity SampleRoom(bool open) {
	auto result = Activity{ .userId = 111, .online = true, .inRoom = true };
	result.roomTitle = SampleText("Ночной эфир", "Night air");
	result.roomMembers = 4;
	if (open) {
		result.roomCode = u"K7QM2XPA9Z"_q;
	}
	return result;
}

[[nodiscard]] std::vector<Chip> SampleChips(const Activity &activity) {
	return BuildChips(activity, LangPhrases(), true);
}

// Two playlists with the switch of the profile and two presets with the
// switch of the gallery, the way the server of today has them. With
// "apart" the presets have a switch of the profile of their own too.
[[nodiscard]] std::vector<SharedItem> SampleShared(bool apart = false) {
	return {
		{
			.id = QString(22, QChar('a')),
			.playlist = true,
			.title = SampleText("Ночная", "Night drive"),
			.count = 24,
			.shown = true,
			.profileSwitch = true,
		},
		{
			.id = QString(22, QChar('b')),
			.playlist = true,
			.title = SampleText("В дорогу", "On the road"),
			.count = 112,
			.profileSwitch = true,
		},
		{
			.id = QString(22, QChar('c')),
			.title = u"CCD 2004"_q,
			.kind = u"photo"_q,
			.shown = !apart,
			.listed = true,
			.profileSwitch = apart,
		},
		{
			.id = QString(22, QChar('d')),
			.title = SampleText("VHS с дачи", "VHS from the 90s"),
			.kind = u"video"_q,
			.shown = apart,
			.profileSwitch = apart,
		},
	};
}

[[nodiscard]] std::vector<Person> SamplePeople() {
	const auto phrases = LangPhrases();
	const auto join = tr::lng_oblivion_social_chip_join(tr::now);
	const auto listening = BuildChips(SampleListening(), phrases, true);
	const auto room = BuildChips(SampleRoom(true), phrases, true);
	return {
		{
			.id = 11,
			.name = SampleText("Аня Смирнова", "Anna Smirnova"),
			.about = listening.front().text,
			.aboutActive = true,
		},
		{
			.id = 12,
			.name = SampleText("Миша", "Michael"),
			.about = room.front().text,
			.aboutActive = true,
			.action = join,
			.joinCode = room.front().joinCode,
		},
		{
			.id = 13,
			.name = SampleText(
				"Константин Константинопольский-Задунайский",
				"Constantine Constantinopolsky-Zadunaisky"),
			.about = tr::lng_oblivion_social_chip_text_online(tr::now),
			.aboutActive = true,
		},
		{
			.id = 14,
			.name = SampleText("Лена", "Helen"),
			.about = QString::fromUtf8("📚 ")
				+ SampleText("сессия, не отвлекать", "exams, do not disturb"),
		},
		{
			.id = 15,
			.name = SampleText("Дима Орлов", "Dmitry Orlov"),
			.about = tr::lng_oblivion_social_friends_plain(tr::now),
		},
	};
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	const auto width = Scaled(kSceneWidth);
	const auto boxSize = QSize(st::boxWideWidth * 2, 0);

	// The block as it stands in a profile: the chips, the «Oblivion»
	// block and the separator the page puts after it.
	// looks: the scene is rendered with every look («Тема Oblivion») too,
	// as name_look1, name_look2 and name_look3.
	const auto block = [=](
			const QString &name,
			Fn<CardData()> data,
			bool looks = false) {
		const auto create = [=](not_null<Ui::RpWidget*> parent) -> QWidget* {
			const auto column = Ui::CreateChild<SceneColumn>(parent.get());
			column->add(
				object_ptr<ProfileCard>(column, InfoCardPadding(), false)
			)->setData(data());
			Ui::AddSkip(column, st::infoProfileSkip);
			column->add(object_ptr<Ui::BoxContentDivider>(column));
			return column;
		};
		RegisterScene(name, QSize(width, 0), create);
		if (looks) {
			Look::RegisterScenes(name, QSize(width, 0), create);
		}
	};
	block(u"social_profile_block"_q, [] {
		auto activity = SampleListening();
		activity.inRoom = true;
		activity.roomTitle = SampleRoom(true).roomTitle;
		activity.roomCode = SampleRoom(true).roomCode;
		return CardData{
			.profile = SampleProfile(),
			.chips = SampleChips(activity),
		};
	}, true);

	// The same without a colour of the person: the marks, «Войти» and the
	// card of «слушает» have the colours of the look.
	block(u"social_profile_block_no_accent"_q, [] {
		auto profile = SampleProfile();
		profile.accent = std::nullopt;
		auto activity = SampleListening();
		activity.inRoom = true;
		activity.roomTitle = SampleRoom(true).roomTitle;
		activity.roomCode = SampleRoom(true).roomCode;
		return CardData{
			.profile = std::move(profile),
			.chips = SampleChips(activity),
		};
	}, true);
	block(u"social_profile_block_plain"_q, [] {
		auto profile = SampleProfile();
		profile.accent = std::nullopt;
		profile.statusEmoji = QString();
		profile.publicPresets = 0;
		return CardData{ .profile = std::move(profile) };
	});
	block(u"social_profile_block_chips_only"_q, [] {
		return CardData{ .chips = SampleChips(SampleRoom(false)) };
	});

	// Everything as long as it gets: a track and a room that don't fit
	// (the room keeps its «Войти»), a status of the full length.
	block(u"social_profile_block_long"_q, [] {
		auto profile = SampleProfile();
		profile.accent = QColor(0x7c, 0x5c, 0xff);
		profile.statusEmoji = QString::fromUtf8("📚");
		profile.statusText = SampleText(
			"до конца месяца на сессии, отвечаю по вечерам, "
			"в субботу собираю комнату с кино",
			"exams till the end of the month, I answer in the "
			"evenings, a movie room on Saturday");
		auto activity = SampleListening();
		activity.performer = SampleText(
			"Симфонический оркестр Мариинского театра",
			"The Mariinsky Theatre Symphony Orchestra");
		activity.title = SampleText(
			"Времена года. Декабрь. Святки",
			"The Seasons. December. Christmas");
		activity.inRoom = true;
		activity.roomTitle = SampleText(
			"Смотрим «Властелина колец» всю ночь напролёт",
			"Watching The Lord of the Rings all night long");
		activity.roomCode = SampleRoom(true).roomCode;
		return CardData{
			.profile = std::move(profile),
			.chips = SampleChips(activity),
		};
	});
	block(u"social_profile_block_self"_q, [] {
		auto profile = SampleProfile();
		profile.publicPlaylists = profile.publicPresets = 0;
		profile.accent = QColor(0x3e, 0xcf, 0x8e);
		return CardData{
			.profile = std::move(profile),
			.chips = SampleChips(Activity{ .userId = 111, .online = true }),
			.self = true,
		};
	});

	// The chips in every variant, each on a line of its own: a track
	// that does not fit, a room with and without «Войти», «в Oblivion»,
	// with the accent colours of different people.
	const auto withLooks = [](
			const QString &name,
			QSize size,
			SnapshotScene create) {
		RegisterScene(name, size, create);
		Look::RegisterScenes(name, size, create);
	};
	withLooks(
		u"social_chips"_q,
		QSize(width, 0),
		[=](not_null<Ui::RpWidget*> parent) -> QWidget* {
			const auto column = Ui::CreateChild<SceneColumn>(parent.get());
			const auto margin = style::margins(
				st::boxLittleSkip,
				st::boxLittleSkip,
				st::boxLittleSkip,
				0);
			const auto add = [&](
					const Activity &activity,
					std::optional<QColor> accent) {
				column->add(
					object_ptr<ChipsView>(column),
					margin
				)->setChips(SampleChips(activity), accent);
			};
			add(SampleListening(), std::nullopt);
			auto longTrack = SampleListening();
			longTrack.performer = SampleText(
				"Симфонический оркестр Мариинского театра",
				"The Mariinsky Theatre Symphony Orchestra");
			longTrack.title = SampleText(
				"Времена года. Декабрь. Святки",
				"The Seasons. December. Christmas");
			add(longTrack, QColor(0xf5, 0xc5, 0x42));
			add(SampleRoom(true), QColor(0xff, 0x5c, 0x8a));
			add(SampleRoom(false), QColor(0x3e, 0xcf, 0x8e));
			auto both = SampleListening();
			both.inRoom = true;
			both.roomTitle = SampleRoom(true).roomTitle;
			both.roomCode = SampleRoom(true).roomCode;
			add(both, QColor(0x7c, 0x5c, 0xff));
			add(
				Activity{ .userId = 1, .online = true },
				QColor(0x2a, 0xa9, 0xe0));

			// A long title next to «Войти», a room without a title.
			auto longRoom = SampleRoom(true);
			longRoom.roomTitle = SampleText(
				"Смотрим «Властелина колец» всю ночь напролёт",
				"Watching The Lord of the Rings all night long");
			add(longRoom, std::nullopt);
			add(
				Activity{ .userId = 1, .inRoom = true },
				QColor(0xff, 0x8a, 0x3d));
			Ui::AddSkip(column, st::boxLittleSkip);
			return column;
		});

	const auto sampleMe = [] {
		auto me = Cloud::Me();
		me.id = 111;
		me.name = SampleText("Аня", "Anna");
		me.avatarRev = 3;
		me.statusText = SampleProfile().statusText;
		me.statusEmoji = SampleProfile().statusEmoji;
		me.accent = u"#ff5c8a"_q;
		return me;
	};
	const auto editor = [=](
			std::shared_ptr<Ui::Show> show) -> object_ptr<Ui::BoxContent> {
		return Box(EditorBox, EditorArgs{
			.show = show,
			.me = sampleMe(),
			.loadShared = [](
					Fn<void(std::vector<SharedItem>)> done,
					Fn<void()> fail) {
				done(SampleShared());
			},
		});
	};
	RegisterBoxScene(u"social_editor"_q, boxSize, editor);
	Look::RegisterBoxScenes(u"social_editor"_q, boxSize, editor);
	RegisterBoxScene(u"social_editor_apart"_q, boxSize, [=](
			std::shared_ptr<Ui::Show> show) {
		return Box(EditorBox, EditorArgs{
			.show = show,
			.me = sampleMe(),
			.loadShared = [](
					Fn<void(std::vector<SharedItem>)> done,
					Fn<void()> fail) {
				done(SampleShared(true));
			},
		});
	});
	RegisterBoxScene(u"social_gallery_confirm"_q, boxSize, [=](
			std::shared_ptr<Ui::Show> show) {
		return Ui::MakeConfirmBox({
			.text = tr::lng_oblivion_social_gallery_confirm(
				tr::now,
				lt_title,
				u"CCD 2004"_q),
			.confirmText = tr::lng_oblivion_social_gallery_confirm_yes(),
			.title = tr::lng_oblivion_social_gallery_confirm_title(),
		});
	});
	RegisterBoxScene(u"social_editor_new"_q, boxSize, [=](
			std::shared_ptr<Ui::Show> show) {
		auto me = Cloud::Me();
		me.id = 111;
		return Box(EditorBox, EditorArgs{
			.show = show,
			.me = me,
			.loadShared = [](
					Fn<void(std::vector<SharedItem>)> done,
					Fn<void()> fail) {
				done({});
			},
		});
	});
	RegisterBoxScene(u"social_editor_loading"_q, boxSize, [=](
			std::shared_ptr<Ui::Show> show) {
		return Box(EditorBox, EditorArgs{ .show = show, .me = sampleMe() });
	});

	const auto friends = [=](
			const QString &name,
			FriendsState state,
			bool looks = false) {
		const auto create = [=](
				std::shared_ptr<Ui::Show> show) -> object_ptr<Ui::BoxContent> {
			return Box(FriendsBox, FriendsArgs{
				.state = rpl::single(state),
				.profile = [] {},
			});
		};
		RegisterBoxScene(name, boxSize, create);
		if (looks) {
			Look::RegisterBoxScenes(name, boxSize, create);
		}
	};
	friends(u"social_friends"_q, {
		.status = Status::Ready,
		.people = SamplePeople(),
	}, true);
	friends(u"social_friends_empty"_q, {
		.status = Status::Ready,
		.hidden = true,
	});
	friends(u"social_friends_hidden"_q, {
		.status = Status::Ready,
		.hidden = true,
		.people = SamplePeople(),
	});
	friends(u"social_friends_offline"_q, {
		.status = Status::Offline,
		.people = SamplePeople(),
	});
	friends(u"social_friends_offline_empty"_q, {
		.status = Status::Offline,
	});
	friends(u"social_friends_loading"_q, { .status = Status::Loading });

	RegisterBoxScene(u"social_chosen"_q, boxSize, [=](
			std::shared_ptr<Ui::Show> show) {
		auto people = SamplePeople();
		people.resize(3);
		const auto remove = tr::lng_oblivion_social_chosen_remove(tr::now);
		for (auto &person : people) {
			person.about = QString();
			person.aboutActive = false;
			person.action = remove;
		}
		people.push_back({
			.id = 5550001234,
			.name = tr::lng_oblivion_social_chosen_unknown(
				tr::now,
				lt_name,
				u"5550001234"_q),
			.action = remove,
		});
		return Box(ChosenBox, ChosenArgs{
			.people = rpl::single(std::move(people)),
			.add = [] {},
		});
	});
	RegisterBoxScene(u"social_chosen_empty"_q, boxSize, [=](
			std::shared_ptr<Ui::Show> show) {
		return Box(ChosenBox, ChosenArgs{
			.people = rpl::single(std::vector<Person>()),
			.add = [] {},
		});
	});

	RegisterBoxScene(u"social_shared"_q, boxSize, [=](
			std::shared_ptr<Ui::Show> show) {
		return Box(SharedBox, SharedArgs{
			.load = [](
					Fn<void(std::vector<SharedItem>)> done,
					Fn<void()> fail) {
				done(SampleShared());
			},
		});
	});
	RegisterBoxScene(u"social_shared_failed"_q, boxSize, [=](
			std::shared_ptr<Ui::Show> show) {
		return Box(SharedBox, SharedArgs{
			.load = [](
					Fn<void(std::vector<SharedItem>)> done,
					Fn<void()> fail) {
				fail();
			},
		});
	});
	RegisterBoxScene(u"social_shared_empty"_q, boxSize, [=](
			std::shared_ptr<Ui::Show> show) {
		return Box(SharedBox, SharedArgs{
			.load = [](
					Fn<void(std::vector<SharedItem>)> done,
					Fn<void()> fail) {
				done({});
			},
		});
	});

	// The rows of the badge in Settings > Oblivion, with the button that
	// is there while the old marker is still in the bio, and the boxes
	// those two rows open.
	RegisterScene(
		u"social_badge_settings"_q,
		QSize(st::boxWideWidth + st::boxLittleSkip * 4, 0),
		[=](not_null<Ui::RpWidget*> parent) {
			const auto column = Ui::CreateChild<SceneColumn>(parent.get());
			Ui::AddSubsectionTitle(column, tr::lng_oblivion_social_section());
			::Settings::AddButtonWithIcon(
				column,
				tr::lng_oblivion_social_badge(),
				st::settingsButton,
				{ &st::menuIconSigned }
			)->toggleOn(rpl::single(true));
			::Settings::AddButtonWithIcon(
				column,
				tr::lng_oblivion_social_marker_remove(),
				st::settingsButton,
				{ &st::menuIconDelete });
			::Settings::AddButtonWithLabel(
				column,
				tr::lng_oblivion_social_profile_audience(),
				tr::lng_oblivion_social_audience_chosen(),
				st::settingsButton,
				{ &st::menuIconPermissions });
			Ui::AddSkip(column);
			Ui::AddDividerText(
				column,
				tr::lng_oblivion_social_settings_about());
			return column;
		});
	RegisterBoxScene(u"social_badge_confirm"_q, boxSize, [=](
			std::shared_ptr<Ui::Show> show) {
		return Ui::MakeConfirmBox({
			.text = tr::lng_oblivion_social_badge_confirm(),
			.confirmText = tr::lng_oblivion_social_badge_confirm_enable(),
			.title = tr::lng_oblivion_social_badge(),
		});
	});
	RegisterBoxScene(u"social_marker_confirm"_q, boxSize, [=](
			std::shared_ptr<Ui::Show> show) {
		return Ui::MakeConfirmBox({
			.text = tr::lng_oblivion_social_marker_about(),
			.confirmText = tr::lng_oblivion_social_chosen_remove(),
			.title = tr::lng_oblivion_social_marker_remove(),
		});
	});
});

} // namespace

QString AudienceName(Audience audience) {
	switch (audience) {
	case Audience::Everyone:
		return tr::lng_oblivion_social_audience_everyone(tr::now);
	case Audience::Chosen:
		return tr::lng_oblivion_social_audience_chosen(tr::now);
	case Audience::Nobody: break;
	}
	return tr::lng_oblivion_social_audience_nobody(tr::now);
}

rpl::producer<QString> SettingsAboutValue(not_null<Main::Session*> session) {
	return rpl::combine(
		LeftPublicValue(session),
		tr::lng_oblivion_social_settings_about(),
		tr::lng_oblivion_social_settings_left()
	) | rpl::map([](bool left, const QString &about, const QString &note) {
		return left ? (note + u"\n\n"_q + about) : about;
	});
}

// The switches show what the server has (FlagNow), also for an account
// that is switched off: a click then asks to connect again first, and
// what was clicked is changed on the server after that. So a badge or a
// chip that is still public can always be switched off from here.
void ToggleFlag(not_null<Window::SessionController*> controller, Flag flag) {
	const auto session = &controller->session();
	const auto account = base::make_weak(&Cloud::For(session));
	const auto show = controller->uiShow();
	const auto apply = [=](bool value) {
		const auto strong = account.get();
		if (!strong) {
			return;
		}
		strong->patchMe(FlagPatch(flag, value), [=] {
			if (flag == Flag::Badge) {
				Toast(show, value
					? tr::lng_oblivion_social_badge_on(tr::now)
					: tr::lng_oblivion_social_badge_off(tr::now));
			} else if (value
				&& (AudienceNow(session, AudienceKind::Activity)
					== Audience::Nobody)) {
				Toast(show, tr::lng_oblivion_social_chip_hint_nobody(tr::now));
			}
		}, [=](const Cloud::Error &error) {
			Cloud::ShowError(show, error);
		});
	};
	if (FlagNow(session, flag)) {
		// Switching off asks nothing, but needs the server all the same.
		Cloud::RequireConsent(controller, [=] {
			apply(false);
		});
		return;
	}
	Cloud::RequireConsent(controller, [=] {
		if (flag != Flag::Badge) {
			apply(true);
			return;
		}

		// The badge is a public statement, it is said so once more.
		show->showBox(Ui::MakeConfirmBox({
			.text = tr::lng_oblivion_social_badge_confirm(),
			.confirmed = [=](Fn<void()> close) {
				close();
				apply(true);
			},
			.confirmText = tr::lng_oblivion_social_badge_confirm_enable(),
			.title = tr::lng_oblivion_social_badge(),
		}));
	});
}

void ShowAudienceBox(
		not_null<Window::SessionController*> controller,
		AudienceKind kind) {
	const auto session = &controller->session();
	const auto account = base::make_weak(&Cloud::For(session));
	const auto show = controller->uiShow();
	const auto weak = base::make_weak(controller);
	const auto apply = [=](Audience value) {
		const auto strong = account.get();
		if (!strong) {
			return;
		}
		auto privacy = QJsonObject();
		privacy.insert(
			(kind == AudienceKind::Profile) ? u"profile"_q : u"activity"_q,
			AudienceToWire(value));
		auto patch = QJsonObject();
		patch.insert(u"privacy"_q, privacy);
		strong->patchMe(std::move(patch), [=] {
			const auto strong = account.get();
			if (strong
				&& (value == Audience::Chosen)
				&& strong->me().chosen.empty()) {
				Toast(show, tr::lng_oblivion_social_chosen_hint(tr::now));
			}
		}, [=](const Cloud::Error &error) {
			Cloud::ShowError(show, error);
		});
	};
	const auto choose = [=](int index) {
		const auto value = (index == 2)
			? Audience::Everyone
			: (index == 1)
			? Audience::Chosen
			: Audience::Nobody;
		const auto strong = weak.get();
		if (!strong || value == AudienceNow(session, kind)) {
			return;
		}
		Cloud::RequireConsent(strong, [=] {
			apply(value);
		});
	};
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto options = std::vector<QString>{
			AudienceName(Audience::Nobody),
			AudienceName(Audience::Chosen),
			AudienceName(Audience::Everyone),
		};
		SingleChoiceBox(box, {
			.title = ((kind == AudienceKind::Profile)
				? tr::lng_oblivion_social_profile_audience()
				: tr::lng_oblivion_social_activity_audience()),
			.options = options,
			.initialSelection = int(AudienceNow(session, kind)),
			.callback = choose,
		});
	}));
}

void ShowRemoveMarker(not_null<Window::SessionController*> controller) {
	const auto weak = base::make_weak(controller);
	controller->show(Ui::MakeConfirmBox({
		.text = tr::lng_oblivion_social_marker_about(),
		.confirmed = [=](Fn<void()> close) {
			close();
			if (const auto strong = weak.get()) {
				Badge::RemoveOldMarker(strong);
			}
		},
		.confirmText = tr::lng_oblivion_social_chosen_remove(),
		.title = tr::lng_oblivion_social_marker_remove(),
	}));
}

void ShowFriends(not_null<Window::SessionController*> controller) {
	const auto weak = base::make_weak(controller);
	Cloud::RequireConsent(controller, [=] {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		const auto session = &strong->session();
		Refresh(session);
		auto state = rpl::single(
			rpl::empty
		) | rpl::then(rpl::merge(
			Changes(session),
			Cloud::For(session).meUpdated()
		)) | rpl::map([=] {
			return BuildFriends(session);
		});
		strong->show(Box(FriendsBox, FriendsArgs{
			.state = std::move(state),
			.open = [=](uint64 id) {
				const auto strong = weak.get();
				const auto user = strong
					? strong->session().data().userLoaded(UserId(id))
					: nullptr;
				if (user) {
					strong->showPeerInfo(user);
				}
			},
			.join = [=](QString code) {
				if (const auto strong = weak.get()) {
					Rooms::OpenLink(strong, code);
				}
			},
			.profile = [=] {
				if (const auto strong = weak.get()) {
					ShowMyProfile(strong);
				}
			},
		}));
	});
}

void ShowMyProfile(not_null<Window::SessionController*> controller) {
	const auto weak = base::make_weak(controller);
	Cloud::RequireConsent(controller, [=] {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		const auto session = &strong->session();
		const auto account = base::make_weak(&Cloud::For(session));
		const auto show = std::shared_ptr<Ui::Show>(strong->uiShow());
		const auto failed = [=](Fn<void(bool)> done) {
			return [=](const Cloud::Error &error) {
				Cloud::ShowError(show, error);
				if (done) {
					done(false);
				}
			};
		};
		show->showBox(Box(EditorBox, EditorArgs{
			.show = show,
			.me = Cloud::For(session).me(),
			.loadShared = [=](
					Fn<void(std::vector<SharedItem>)> done,
					Fn<void()> fail) {
				LoadOwnShared(session, done, [=](const Cloud::Error &) {
					fail();
				});
			},
			.setShown = [=](
					SharedItem item,
					bool shown,
					EditorArgs::Changed done) {
				SetSharedShown(session, item, shown, [=](SharedItem now) {
					done(std::move(now));
				}, [=](const Cloud::Error &error) {
					Cloud::ShowError(show, error);
					done(std::nullopt);
				});
			},
			.setListed = [=](
					SharedItem item,
					bool listed,
					EditorArgs::Changed done) {
				SetPresetListed(session, item, listed, [=](SharedItem now) {
					done(std::move(now));
				}, [=](const Cloud::Error &error) {
					Cloud::ShowError(show, error);
					done(std::nullopt);
				});
			},
			.save = [=](QJsonObject patch, Fn<void(bool)> done) {
				const auto strong = account.get();
				if (!strong) {
					done(false);
					return;
				}
				strong->patchMe(std::move(patch), [=] {
					const auto hidden = (AudienceNow(
						session,
						AudienceKind::Profile) == Audience::Nobody);
					Toast(show, hidden
						? tr::lng_oblivion_social_editor_hidden(tr::now)
						: tr::lng_oblivion_social_editor_saved(tr::now));
					done(true);
				}, failed(done));
			},
			.setPhoto = [=](Fn<void(bool)> done) {
				const auto strong = account.get();
				if (!strong) {
					done(false);
					return;
				}

				// The userpic the app has already loaded for itself.
				const auto user = session->user();
				auto view = user->createUserpicView();
				user->loadUserpic();
				const auto bytes = (user->hasUserpic()
					&& user->userpicCloudImage(view))
					? EncodeAvatar(PeerData::GenerateUserpicImage(
						user,
						view,
						kAvatarSide,
						0))
					: QByteArray();
				if (bytes.isEmpty()) {
					Toast(
						show,
						tr::lng_oblivion_social_editor_photo_none(tr::now));
					done(false);
					return;
				}
				strong->request(
					Cloud::PutRawRequest(
						u"/v1/me/avatar"_q,
						bytes,
						"image/jpeg"),
					[=](const Cloud::Response &response) {
						Toast(
							show,
							tr::lng_oblivion_social_editor_photo_done(
								tr::now));
						done(true);
					},
					failed(done));
			},
			.removePhoto = [=](Fn<void(bool)> done) {
				const auto strong = account.get();
				if (!strong) {
					done(false);
					return;
				}
				strong->request(
					Cloud::DeleteRequest(u"/v1/me/avatar"_q),
					[=](const Cloud::Response &response) {
						Toast(
							show,
							tr::lng_oblivion_social_editor_photo_removed(
								tr::now));
						done(true);
					},
					failed(done));
			},
			.closeRequests = DisconnectedEvents(session),
		}));
	});
}

void ShowChosenList(not_null<Window::SessionController*> controller) {
	const auto weak = base::make_weak(controller);
	Cloud::RequireConsent(controller, [=] {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		const auto session = &strong->session();
		const auto show = std::shared_ptr<Ui::Show>(strong->uiShow());
		const auto edit = std::make_shared<ChosenEdit>(session, show);
		const auto remove = [=](uint64 id) {
			edit->remove(id);
		};
		const auto add = [=](not_null<UserData*> user) {
			return edit->add(peerToUser(user->id).bare);
		};
		const auto chosen = [=](uint64 id) {
			return ranges::contains(edit->list(), id);
		};

		// The rows are what is being edited here, at once; when nothing
		// is on its way they are what the server has. (The edit is not
		// held by what listens to its own changes.)
		const auto editing = std::weak_ptr<ChosenEdit>(edit);
		auto people = rpl::single(
			rpl::empty
		) | rpl::then(rpl::merge(
			Cloud::For(session).meUpdated(),
			edit->changes()
		)) | rpl::map([=] {
			const auto strong = editing.lock();
			return ChosenPeople(
				session,
				strong ? strong->list() : Cloud::For(session).me().chosen);
		});
		show->showBox(Box(ChosenBox, ChosenArgs{
			.people = std::move(people),
			.remove = remove,
			.add = [=] {
				show->showBox(Box<PeerListBox>(
					std::make_unique<ChosenAddController>(
						session,
						chosen,
						add),
					[](not_null<PeerListBox*> box) {
						box->addButton(tr::lng_close(), [=] {
							box->closeBox();
						});
					}));
			},
		}));
	});
}

ProfileBlock CreateProfileBlock(
		not_null<QWidget*> parent,
		not_null<Window::SessionController*> controller,
		not_null<UserData*> user) {
	if (user->isBot() || user->isServiceUser() || user->isInaccessible()) {
		return {};
	}
	const auto session = &user->session();
	const auto userId = peerToUser(user->id).bare;
	const auto self = user->isSelf();
	const auto weak = base::make_weak(controller);
	auto card = object_ptr<ProfileCard>(parent, InfoCardPadding(), false);
	const auto raw = card.data();
	raw->setCallbacks([=](QString code) {
		if (const auto strong = weak.get()) {
			Rooms::OpenLink(strong, code);
		}
	}, [=] {
		if (const auto strong = weak.get()) {
			ShowShared(strong, userId);
		}
	}, [=] {
		if (const auto strong = weak.get()) {
			ShowMyProfile(strong);
		}
	});
	auto wrap = object_ptr<Ui::SlideWrap<Ui::RpWidget>>(
		parent,
		object_ptr<Ui::RpWidget>(std::move(card)));
	const auto slide = wrap.data();
	slide->setDuration(0);
	const auto update = [=] {
		raw->setData(CardFor(session, userId, self));
		slide->toggle(!raw->isEmpty(), anim::type::instant);
	};
	rpl::merge(
		Changes(session),
		Get().changes()
	) | rpl::on_next(update, raw->lifetime());
	update();

	// The chips of the page are what the server shows now, not what an
	// event has told some time ago: the list is asked again (no id is
	// sent, not more often than once a minute).
	if (!self && Get().cloudProfileShow()) {
		RefreshActivity(session);
	}

	auto shown = slide->toggledValue();
	return {
		.widget = std::move(wrap),
		.shown = std::move(shown),
	};
}

void AddMainMenuEntry(
		not_null<Ui::VerticalLayout*> menu,
		not_null<Window::SessionController*> controller) {
	const auto wrap = menu->add(
		object_ptr<Ui::SlideWrap<Ui::SettingsButton>>(
			menu,
			::Settings::CreateButtonWithIcon(
				menu,
				tr::lng_oblivion_social_friends(),
				st::mainMenuButton,
				{ &st::menuIconRatingUsers })));
	wrap->setDuration(0)->toggleOn(rpl::single(
		rpl::empty
	) | rpl::then(
		Get().changes()
	) | rpl::map([] {
		return Get().cloudFriends();
	}) | rpl::distinct_until_changed());
	wrap->entity()->setClickedCallback([=] {
		ShowFriends(controller);
	});
}

} // namespace Oblivion::Social
