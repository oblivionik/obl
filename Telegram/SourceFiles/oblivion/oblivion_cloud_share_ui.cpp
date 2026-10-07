/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_cloud_share_ui.h"

#include "base/unique_qptr.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "data/data_peer_id.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "oblivion/oblivion_lang.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "settings/settings_common.h"
#include "ui/boxes/confirm_box.h"
#include "ui/effects/ripple_animation.h"
#include "ui/empty_userpic.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/text/format_values.h"
#include "ui/text/text_utilities.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_calls.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace Oblivion::Share {
namespace {

constexpr auto kCoverSize = 38;
constexpr auto kCoverColors = 7;
constexpr auto kProgressHeight = 6;
constexpr auto kRowProgressHeight = 2;
constexpr auto kPreviewWidth = 640;
constexpr auto kPreviewHeight = 400;
constexpr auto kGalleryPage = 30;
constexpr auto kCardRadius = 10;
constexpr auto kCardPadding = 12;
constexpr auto kCardSkip = 8;
constexpr auto kPillRadius = 9;
constexpr auto kChipPadding = 12;
constexpr auto kChipSkip = 5;
constexpr auto kChipGap = 8;
constexpr auto kLinkHeight = 40;
constexpr auto kFetchPart = 0.45;
constexpr auto kSendPart = 0.5;

[[nodiscard]] int Scaled(int value) {
	return style::ConvertScale(value);
}

[[nodiscard]] style::margins RowPadding() {
	return st::boxRowPadding + style::margins(0, 0, 0, st::boxLittleSkip);
}

[[nodiscard]] QString Dot() {
	return QString::fromUtf8(" \xC2\xB7 ");
}

[[nodiscard]] QString Percent(float64 part) {
	return QString::number(int(std::clamp(part, 0., 1.) * 100.)) + u"%"_q;
}

// Ui::FormatSizeText() stops at megabytes: a quota of three gigabytes
// would read as "3072.0 MB".
[[nodiscard]] QString SizeText(int64 bytes) {
	constexpr auto kGigabyte = int64(1) << 30;
	if (bytes < kGigabyte) {
		return Ui::FormatSizeText(bytes);
	}
	const auto tenths = (bytes * 10) / kGigabyte;
	auto result = QString::number(tenths / 10);
	if (const auto rest = int(tenths % 10)) {
		result += QChar('.') + QString::number(rest);
	}
	return result + u" GB"_q;
}

void Toast(const std::shared_ptr<Ui::Show> &show, const QString &text) {
	if (show && show->valid() && !text.isEmpty()) {
		show->showToast(text);
	}
}

void CopyLink(const std::shared_ptr<Ui::Show> &show, const QString &link) {
	if (link.isEmpty()) {
		return;
	}
	QGuiApplication::clipboard()->setText(link);
	Toast(show, tr::lng_oblivion_share_link_copied(tr::now));
}

[[nodiscard]] QString KindName(const QString &kind) {
	return (kind == u"video"_q)
		? tr::lng_oblivion_share_kind_video(tr::now)
		: tr::lng_oblivion_share_kind_photo(tr::now);
}

[[nodiscard]] uint8 ColorIndex(const QString &id) {
	auto sum = uint32(0);
	for (const auto ch : id) {
		sum = sum * 31 + ch.unicode();
	}
	return uint8(sum % kCoverColors);
}

[[nodiscard]] QString PlaylistInfo(const Playlist &playlist) {
	auto result = tr::lng_oblivion_playlists_tracks_count(
		tr::now,
		lt_count,
		playlist.full ? int(playlist.tracks.size()) : playlist.trackCount);
	if (playlist.totalDuration >= 1000) {
		result += Dot() + Ui::FormatDurationText(playlist.totalDuration / 1000);
	}
	if (playlist.canEdit) {
		result += Dot() + tr::lng_oblivion_share_view_yours(tr::now);
		if (playlist.followers > 0) {
			result += Dot() + tr::lng_oblivion_share_view_followers(
				tr::now,
				lt_count,
				playlist.followers);
		}
	} else if (!playlist.ownerName.isEmpty()) {
		result += Dot() + tr::lng_oblivion_share_view_by(
			tr::now,
			lt_name,
			playlist.ownerName);
	}
	return result;
}

// ---- A row of a list: a round cover or a number, two lines of text,
// something at the right, the dots of a menu, a thin line of progress.

class Row final : public Ui::RippleButton {
public:
	Row(QWidget *parent, bool menu);

	void setTexts(QString title, QString status, QString right);
	void setCover(uint8 color, const style::icon *icon);
	void setIndex(QString index);
	void setActive(bool active);
	void setFailed(bool failed);
	void setProgress(float64 progress); // Below zero: no line.

	[[nodiscard]] rpl::producer<> menuRequests() const {
		return _menuRequests.events();
	}

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;
	QImage prepareRippleMask() const override;

private:
	const style::PeerListItem &_st;
	Ui::IconButton *_more = nullptr;
	QString _index;
	QString _title;
	QString _status;
	QString _right;
	std::optional<uint8> _cover;
	const style::icon *_icon = nullptr;
	float64 _progress = -1.;
	bool _active = false;
	bool _failed = false;
	rpl::event_stream<> _menuRequests;

};

Row::Row(QWidget *parent, bool menu)
: RippleButton(parent, st::defaultRippleAnimation)
, _st(st::defaultPeerListItem) {
	if (menu) {
		_more = Ui::CreateChild<Ui::IconButton>(this, st::themesMenuToggle);
		_more->setClickedCallback([=] {
			_menuRequests.fire({});
		});
	}
	resize(width(), _st.height);
}

void Row::setTexts(QString title, QString status, QString right) {
	if (_title == title && _status == status && _right == right) {
		return;
	}
	_title = std::move(title);
	_status = std::move(status);
	_right = std::move(right);
	setAccessibleName(_title);
	update();
}

void Row::setCover(uint8 color, const style::icon *icon) {
	_cover = color;
	_icon = icon;
	update();
}

void Row::setIndex(QString index) {
	_index = std::move(index);
	update();
}

void Row::setActive(bool active) {
	if (_active != active) {
		_active = active;
		update();
	}
}

void Row::setFailed(bool failed) {
	if (_failed != failed) {
		_failed = failed;
		update();
	}
}

void Row::setProgress(float64 progress) {
	if (_progress != progress) {
		_progress = progress;
		update();
	}
}

int Row::resizeGetHeight(int newWidth) {
	if (_more) {
		_more->moveToRight(
			std::max(st::boxTitleMenu.width / 2 - _more->width() / 2, 0),
			(_st.height - _more->height()) / 2,
			newWidth);
	}
	return _st.height;
}

void Row::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);

	const auto over = isOver() || isDown();
	p.fillRect(e->rect(), over ? st::windowBgOver : st::windowBg);
	paintRipple(p, 0, 0);

	// A cover or a number sits where the icon of a settings button would,
	// the texts start where its text does: the rows line up with the
	// buttons above a list.
	const auto iconCenter = st::settingsButton.iconLeft
		+ st::menuIconSoundOn.width() / 2;
	auto left = st::boxRowPadding.left();
	if (_cover) {
		const auto size = Scaled(kCoverSize);
		const auto rect = QRect(
			iconCenter - size / 2,
			(height() - size) / 2,
			size,
			size);
		const auto colors = Ui::EmptyUserpic::UserpicColor(*_cover);
		{
			auto hq = PainterHighQualityEnabler(p);
			auto gradient = QLinearGradient(rect.topLeft(), rect.bottomLeft());
			gradient.setStops({
				{ 0., colors.color1->c },
				{ 1., colors.color2->c },
			});
			p.setPen(Qt::NoPen);
			p.setBrush(gradient);
			p.drawEllipse(rect);
		}
		if (_icon) {
			_icon->paintInCenter(p, rect, st::historyPeerUserpicFg->c);
		}
		left = st::settingsButton.padding.left();
	} else if (!_index.isEmpty()) {
		const auto indexWidth = st::normalFont->width(u"0000"_q);
		p.setFont(st::normalFont);
		p.setPen(_active
			? st::windowActiveTextFg
			: over
			? st::windowSubTextFgOver
			: st::windowSubTextFg);
		p.drawText(
			QRect(iconCenter - indexWidth / 2, 0, indexWidth, height()),
			_index,
			style::al_center);
		left = st::settingsButton.padding.left();
	}

	const auto titleTop = _status.isEmpty()
		? (height() - st::semiboldFont->height) / 2
		: _st.namePosition.y();
	auto right = _more ? _more->x() : (width() - st::boxRowPadding.right());
	if (!_right.isEmpty()) {
		const auto rightWidth = st::normalFont->width(_right);
		p.setFont(st::normalFont);
		p.setPen(over ? st::windowSubTextFgOver : st::windowSubTextFg);
		p.drawTextLeft(
			right - rightWidth,
			(height() - st::normalFont->height) / 2,
			width(),
			_right);
		right -= rightWidth + st::boxLittleSkip;
	}
	const auto textWidth = right - left;
	if (textWidth > 0) {
		p.setFont(st::semiboldFont);
		p.setPen(_active ? st::windowActiveTextFg : st::contactsNameFg);
		p.drawTextLeft(
			left,
			titleTop,
			width(),
			st::semiboldFont->elided(_title, textWidth));
	}
	if (textWidth > 0 && !_status.isEmpty()) {
		p.setFont(st::normalFont);
		p.setPen(_failed
			? st::boxTextFgError
			: over
			? st::windowSubTextFgOver
			: st::windowSubTextFg);
		p.drawTextLeft(
			left,
			_st.statusPosition.y(),
			width(),
			st::normalFont->elided(_status, textWidth));
	}
	if (_progress >= 0. && textWidth > 0) {
		const auto line = Scaled(kRowProgressHeight);
		const auto top = height() - line - Scaled(2);
		p.fillRect(left, top, textWidth, line, st::windowBgRipple);
		p.fillRect(
			left,
			top,
			int(textWidth * std::clamp(_progress, 0., 1.)),
			line,
			st::windowBgActive);
	}
}

void Row::contextMenuEvent(QContextMenuEvent *e) {
	e->accept();
	_menuRequests.fire({});
}

QImage Row::prepareRippleMask() const {
	return Ui::RippleAnimation::RectMask(size());
}

class ProgressBar final : public Ui::RpWidget {
public:
	explicit ProgressBar(QWidget *parent);

	void setValue(float64 value, bool failed);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	float64 _value = 0.;
	bool _failed = false;

};

ProgressBar::ProgressBar(QWidget *parent)
: RpWidget(parent) {
	resize(width(), Scaled(kProgressHeight));
}

void ProgressBar::setValue(float64 value, bool failed) {
	const auto clamped = std::clamp(value, 0., 1.);
	if (_value != clamped || _failed != failed) {
		_value = clamped;
		_failed = failed;
		update();
	}
}

void ProgressBar::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto radius = height() / 2.;
	p.setPen(Qt::NoPen);
	p.setBrush(st::windowBgRipple);
	p.drawRoundedRect(QRectF(rect()), radius, radius);
	const auto filled = width() * _value;
	if (filled >= 1.) {
		p.setBrush(_failed ? st::boxTextFgError : st::windowBgActive);
		p.drawRoundedRect(
			QRectF(0., 0., std::max(filled, height() * 1.), height()),
			radius,
			radius);
	}
}

// ---- The names of effects as small pills.

class Pills final : public Ui::RpWidget {
public:
	Pills(QWidget *parent, QStringList names);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;

private:
	struct Pill {
		QString text;
		QRect rect;
	};
	std::vector<Pill> _pills;

};

Pills::Pills(QWidget *parent, QStringList names)
: RpWidget(parent) {
	for (auto &name : names) {
		_pills.push_back({ std::move(name) });
	}
}

int Pills::resizeGetHeight(int newWidth) {
	const auto padding = Scaled(9);
	const auto height = st::normalFont->height + Scaled(6);
	const auto skip = Scaled(6);
	auto left = 0;
	auto top = 0;
	for (auto &pill : _pills) {
		const auto available = std::max(newWidth - 2 * padding, 1);
		const auto width = std::min(
			st::normalFont->width(pill.text),
			available) + 2 * padding;
		if (left > 0 && left + width > newWidth) {
			left = 0;
			top += height + skip;
		}
		pill.rect = QRect(left, top, width, height);
		left += width + skip;
	}
	return _pills.empty() ? 0 : (top + height);
}

void Pills::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto padding = Scaled(9);
	const auto radius = Scaled(kPillRadius);
	p.setFont(st::normalFont);
	for (const auto &pill : _pills) {
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(pill.rect, radius, radius);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			pill.rect.marginsRemoved({ padding, 0, padding, 0 }),
			st::normalFont->elided(
				pill.text,
				pill.rect.width() - 2 * padding),
			style::al_left);
	}
}

// ---- A stack on the sample picture: the left half as it was, the
// right half with the effects.

class SplitPreview final : public Ui::RpWidget {
public:
	SplitPreview(QWidget *parent, QImage before);

	void setAfter(QImage after);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;

private:
	void paintLabel(QPainter &p, const QString &text, bool right);

	QImage _before;
	QImage _after;

};

SplitPreview::SplitPreview(QWidget *parent, QImage before)
: RpWidget(parent)
, _before(std::move(before)) {
}

void SplitPreview::setAfter(QImage after) {
	_after = std::move(after);
	update();
}

int SplitPreview::resizeGetHeight(int newWidth) {
	return newWidth * kPreviewHeight / kPreviewWidth;
}

void SplitPreview::paintLabel(QPainter &p, const QString &text, bool right) {
	const auto padding = Scaled(8);
	const auto skip = Scaled(8);
	const auto height = st::normalFont->height + Scaled(4);
	const auto width = st::normalFont->width(text) + 2 * padding;
	const auto rect = QRect(
		right ? (this->width() - skip - width) : skip,
		this->height() - skip - height,
		width,
		height);
	// Over a picture, like the time of a photo in a chat.
	p.setPen(Qt::NoPen);
	p.setBrush(st::msgDateImgBg);
	p.drawRoundedRect(rect, height / 2., height / 2.);
	p.setPen(st::msgDateImgFg);
	p.setFont(st::normalFont);
	p.drawText(rect, text, style::al_center);
}

void SplitPreview::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	const auto radius = Scaled(kCardRadius);
	auto clip = QPainterPath();
	clip.addRoundedRect(QRectF(rect()), radius, radius);
	p.setClipPath(clip);
	if (_before.isNull()) {
		p.fillRect(rect(), st::windowBgOver);
		return;
	}
	const auto half = width() / 2;
	const auto ready = !_after.isNull();
	p.drawImage(rect(), _before);
	if (ready) {
		const auto part = QRectF(
			_after.width() / 2.,
			0.,
			_after.width() / 2.,
			_after.height());
		p.drawImage(QRectF(half, 0, width() - half, height()), _after, part);
		p.fillRect(half, 0, Scaled(2), height(), st::msgDateImgFg);
		paintLabel(p, tr::lng_oblivion_share_preset_before(tr::now), false);
		paintLabel(p, tr::lng_oblivion_share_preset_after(tr::now), true);
	}
}

// ---- A card of the gallery.

class Card final : public Ui::RippleButton {
public:
	Card(QWidget *parent, const Preset &preset);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	QImage prepareRippleMask() const override;

private:
	[[nodiscard]] QRect inner() const;

	QString _title;
	QString _meta;
	QString _effects;
	QString _text;

};

Card::Card(QWidget *parent, const Preset &preset)
: RippleButton(parent, st::defaultRippleAnimation)
, _title(preset.title)
, _effects(EffectNames(preset.kind, preset.tags))
, _text(preset.text.simplified()) {
	auto meta = QStringList();
	if (!preset.ownerName.isEmpty()) {
		meta.push_back(tr::lng_oblivion_share_view_by(
			tr::now,
			lt_name,
			preset.ownerName));
	}
	if (preset.uses > 0) {
		meta.push_back(tr::lng_oblivion_share_gallery_uses(
			tr::now,
			lt_count,
			preset.uses));
	}
	_meta = meta.join(Dot());
	setAccessibleName(_title);
}

QRect Card::inner() const {
	return rect().marginsRemoved({
		st::boxRowPadding.left(),
		Scaled(kCardSkip) / 2,
		st::boxRowPadding.right(),
		Scaled(kCardSkip) / 2,
	});
}

int Card::resizeGetHeight(int newWidth) {
	auto lines = st::semiboldFont->height;
	for (const auto &text : { _meta, _effects, _text }) {
		if (!text.isEmpty()) {
			lines += st::normalFont->height + Scaled(2);
		}
	}
	return lines + 2 * Scaled(kCardPadding) + Scaled(kCardSkip);
}

void Card::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto rect = inner();
	const auto radius = Scaled(kCardRadius);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush((isOver() || isDown())
			? st::windowBgRipple
			: st::windowBgOver);
		p.drawRoundedRect(rect, radius, radius);
	}
	paintRipple(p, rect.x(), rect.y());

	const auto padding = Scaled(kCardPadding);
	const auto left = rect.x() + padding;
	const auto available = rect.width() - 2 * padding;
	if (available <= 0) {
		return;
	}
	auto top = rect.y() + padding;
	p.setFont(st::semiboldFont);
	p.setPen(st::windowFg);
	p.drawTextLeft(
		left,
		top,
		width(),
		st::semiboldFont->elided(_title, available));
	top += st::semiboldFont->height;
	const auto line = [&](const QString &text, const style::color &color) {
		if (text.isEmpty()) {
			return;
		}
		top += Scaled(2);
		p.setFont(st::normalFont);
		p.setPen(color);
		p.drawTextLeft(
			left,
			top,
			width(),
			st::normalFont->elided(text, available));
		top += st::normalFont->height;
	};
	// What it is made of, what the author says about it, whose it is.
	line(_effects, st::windowActiveTextFg);
	line(_text, st::windowFg);
	line(_meta, st::windowSubTextFg);
}

QImage Card::prepareRippleMask() const {
	return Ui::RippleAnimation::RoundRectMask(
		inner().size(),
		Scaled(kCardRadius));
}

// ---- A choice of one of a few short names: round chips in a row. Not
// one more slider right under the slider of the kinds.

class Chip final : public Ui::AbstractButton {
public:
	Chip(QWidget *parent, const QString &text);

	void setActive(bool active);

protected:
	void paintEvent(QPaintEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;

private:
	const QString _text;
	bool _active = false;

};

Chip::Chip(QWidget *parent, const QString &text)
: AbstractButton(parent)
, _text(text) {
	resize(
		st::normalFont->width(_text) + 2 * Scaled(kChipPadding),
		st::normalFont->height + 2 * Scaled(kChipSkip));
	setAccessibleName(_text);
}

void Chip::setActive(bool active) {
	if (_active != active) {
		_active = active;
		update();
	}
}

void Chip::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto radius = height() / 2.;
	p.setPen(Qt::NoPen);
	p.setBrush(_active
		? st::windowBgActive
		: (isOver() || isDown())
		? st::windowBgRipple
		: st::windowBgOver);
	p.drawRoundedRect(QRectF(rect()), radius, radius);
	p.setFont(st::normalFont);
	p.setPen(_active ? st::windowFgActive : st::windowSubTextFg);
	p.drawText(rect(), _text, style::al_center);
}

void Chip::onStateChanged(State was, StateChangeSource source) {
	update();
}

class Chips final : public Ui::RpWidget {
public:
	Chips(QWidget *parent, const QStringList &names);

	[[nodiscard]] rpl::producer<int> activated() const {
		return _activated.events();
	}

protected:
	int resizeGetHeight(int newWidth) override;

private:
	std::vector<not_null<Chip*>> _chips;
	int _active = 0;
	rpl::event_stream<int> _activated;

};

Chips::Chips(QWidget *parent, const QStringList &names)
: RpWidget(parent) {
	auto index = 0;
	for (const auto &name : names) {
		const auto chip = Ui::CreateChild<Chip>(this, name);
		const auto my = index++;
		chip->setActive(my == _active);
		chip->setClickedCallback([=] {
			if (_active == my) {
				return;
			}
			_active = my;
			auto other = 0;
			for (const auto &button : _chips) {
				button->setActive(other++ == my);
			}
			_activated.fire_copy(my);
		});
		_chips.push_back(chip);
	}
}

int Chips::resizeGetHeight(int newWidth) {
	const auto gap = Scaled(kChipGap);
	auto left = 0;
	auto result = 0;
	for (const auto &chip : _chips) {
		chip->moveToLeft(left, 0, newWidth);
		left += chip->width() + gap;
		result = std::max(result, chip->height());
	}
	return result;
}

// ---- A link in a rounded field: it does not break in the middle of a
// word the way a label does, a click copies it.

class LinkField final : public Ui::RippleButton {
public:
	explicit LinkField(QWidget *parent);

	void setLink(const QString &link);

protected:
	void paintEvent(QPaintEvent *e) override;
	QImage prepareRippleMask() const override;

private:
	QString _text;

};

LinkField::LinkField(QWidget *parent)
: RippleButton(parent, st::defaultRippleAnimation) {
	resize(width(), Scaled(kLinkHeight));
}

void LinkField::setLink(const QString &link) {
	// Without the scheme: more of the link itself fits.
	const auto scheme = link.indexOf(u"://"_q);
	const auto text = (scheme >= 0) ? link.mid(scheme + 3) : link;
	if (_text != text) {
		_text = text;
		setAccessibleName(link);
		update();
	}
}

void LinkField::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto radius = Scaled(kCardRadius);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(rect(), radius, radius);
	}
	paintRipple(p, 0, 0);

	const auto padding = Scaled(kCardPadding);
	const auto &icon = st::menuIconCopy;
	const auto iconLeft = width() - padding - icon.width();
	icon.paint(p, iconLeft, (height() - icon.height()) / 2, width());
	const auto available = iconLeft - 2 * padding;
	if (available > 0) {
		p.setFont(st::normalFont);
		p.setPen(st::windowFg);
		p.drawTextLeft(
			padding,
			(height() - st::normalFont->height) / 2,
			width(),
			st::normalFont->elided(_text, available, Qt::ElideMiddle));
	}
}

QImage LinkField::prepareRippleMask() const {
	return Ui::RippleAnimation::RoundRectMask(
		size(),
		Scaled(kCardRadius));
}

// ---- The upload of a playlist.

struct UploadBoxArgs {
	rpl::producer<UploadStatus> status;
	Fn<void()> cancel;
	Fn<void()> retry;
	Fn<void(const QString &playlistId)> open;
	Fn<QString(const Cloud::Error &error)> errorText;
	Fn<void()> closed;
};

[[nodiscard]] float64 UploadTotal(const UploadStatus &status) {
	using Stage = UploadStatus::Stage;
	if (status.stage == Stage::Done) {
		return 1.;
	} else if (status.count <= 0) {
		return 0.;
	}
	const auto part = std::clamp(status.part, 0., 1.);
	const auto inside = (status.stage == Stage::Fetching)
		? (part * kFetchPart)
		: (status.stage == Stage::Sending)
		? (kFetchPart + part * kSendPart)
		: (status.stage == Stage::Adding)
		? (kFetchPart + kSendPart)
		: 0.;
	return std::clamp((status.index + inside) / status.count, 0., 1.);
}

void UploadBox(not_null<Ui::GenericBox*> box, UploadBoxArgs &&args) {
	using Stage = UploadStatus::Stage;
	struct State {
		UploadStatus status;
		rpl::variable<QString> name;
		rpl::variable<QString> line;
		rpl::variable<QString> stage;
		rpl::variable<QString> note;
		rpl::variable<QString> link;
		int buttons = -1;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto cancel = std::move(args.cancel);
	const auto retry = std::move(args.retry);
	const auto open = std::move(args.open);
	const auto errorText = std::move(args.errorText);
	const auto closed = std::move(args.closed);

	box->setTitle(tr::lng_oblivion_share_upload_title());
	box->setWidth(st::boxWideWidth);
	box->setCloseByOutsideClick(false);

	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			state->name.value() | rpl::map(tr::bold),
			st::boxLabel),
		RowPadding());
	const auto bar = box->addRow(object_ptr<ProgressBar>(box), RowPadding());
	box->addRow(
		object_ptr<Ui::FlatLabel>(box, state->line.value(), st::boxLabel),
		RowPadding());
	const auto stage = box->addRow(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			box,
			object_ptr<Ui::FlatLabel>(
				box,
				state->stage.value(),
				st::boxDividerLabel),
			style::margins(0, 0, 0, st::boxLittleSkip)),
		st::boxRowPadding);
	const auto note = box->addRow(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			box,
			object_ptr<Ui::FlatLabel>(
				box,
				state->note.value(),
				st::boxDividerLabel),
			style::margins(0, 0, 0, st::boxLittleSkip)),
		st::boxRowPadding);
	const auto link = box->addRow(
		object_ptr<Ui::SlideWrap<LinkField>>(
			box,
			object_ptr<LinkField>(box),
			style::margins(0, st::boxLittleSkip / 2, 0, st::boxLittleSkip)),
		st::boxRowPadding);
	link->toggle(false, anim::type::instant);

	const auto show = box->uiShow();
	link->entity()->setClickedCallback([=] {
		CopyLink(show, state->link.current());
	});
	const auto buttonsKind = [](const UploadStatus &status) {
		return !status.finished()
			? 0
			: (status.stage == Stage::Done && !status.playlistId.isEmpty())
			? 1
			: (status.stage == Stage::Failed)
			? 2
			: 3;
	};
	const auto buttonsMark = [=](const UploadStatus &status) {
		return buttonsKind(status) * 2
			+ (status.playlistId.isEmpty() ? 0 : 1);
	};
	const auto rebuildButtons = [=] {
		const auto &status = state->status;
		const auto kind = buttonsKind(status);
		state->buttons = buttonsMark(status);
		box->clearButtons();
		const auto close = [=] {
			box->closeBox();
		};
		const auto openIt = [=] {
			const auto id = state->status.playlistId;
			const auto callback = open;
			box->closeBox();
			if (callback && !id.isEmpty()) {
				callback(id);
			}
		};
		if (kind == 0) {
			box->addButton(tr::lng_oblivion_share_upload_hide(), close);
			box->addButton(tr::lng_oblivion_share_upload_cancel(), [=] {
				if (cancel) {
					cancel();
				}
			});
		} else if (kind == 1) {
			// Three buttons with «Скопировать ссылку» among them are
			// wider than the box: it is closed with the cross.
			box->addButton(tr::lng_oblivion_share_copy_link(), [=] {
				CopyLink(show, state->link.current());
			});
			box->addButton(tr::lng_oblivion_share_open(), openIt);
			box->addTopButton(st::boxTitleClose, close);
		} else if (kind == 2) {
			box->addButton(tr::lng_oblivion_share_retry(), [=] {
				if (retry) {
					retry();
				}
			});
			box->addButton(tr::lng_close(), close);
			if (!status.playlistId.isEmpty()) {
				box->addLeftButton(tr::lng_oblivion_share_open(), openIt);
			}
		} else {
			box->addButton(tr::lng_close(), close);
			if (!status.playlistId.isEmpty()) {
				box->addLeftButton(tr::lng_oblivion_share_open(), openIt);
			}
		}
	};

	const auto apply = [=](const UploadStatus &status) {
		state->status = status;
		state->name = status.title;
		bar->setValue(UploadTotal(status), status.stage == Stage::Failed);

		const auto working = !status.finished();
		const auto number = std::min(status.index + 1, status.count);
		auto line = tr::lng_oblivion_share_upload_track(
			tr::now,
			lt_index,
			QString::number(number),
			lt_total,
			QString::number(status.count));
		if (working && !status.track.isEmpty()) {
			line += Dot() + status.track;
		}
		auto notes = QStringList();
		if (status.skipped > 0) {
			notes.push_back(tr::lng_oblivion_share_upload_skipped(
				tr::now,
				lt_count,
				status.skipped));
		}
		auto stageText = QString();
		auto error = false;
		switch (status.stage) {
		case Stage::Idle:
		case Stage::Fetching:
			stageText = tr::lng_oblivion_share_upload_fetch(tr::now);
			if (status.part > 0.) {
				stageText += Dot() + Percent(status.part);
			}
			break;
		case Stage::Sending:
			stageText = tr::lng_oblivion_share_upload_send(tr::now)
				+ Dot()
				+ Percent(status.part);
			break;
		case Stage::Adding:
			stageText = tr::lng_oblivion_share_upload_add(tr::now);
			break;
		case Stage::Done:
			line = status.added
				? tr::lng_oblivion_share_upload_done(
					tr::now,
					lt_count,
					status.added)
				: tr::lng_oblivion_share_upload_done_none(tr::now);
			break;
		case Stage::Failed:
			error = true;
			stageText = errorText
				? errorText(status.error)
				: Cloud::ErrorText(status.error);
			break;
		case Stage::Cancelled:
			line = tr::lng_oblivion_share_upload_cancelled(tr::now);
			break;
		}
		if (working) {
			notes.push_back(tr::lng_oblivion_share_upload_hint(tr::now));
		} else if (status.stage != Stage::Done && status.added > 0) {
			notes.push_back(tr::lng_oblivion_share_upload_kept(
				tr::now,
				lt_count,
				status.added));
		}
		state->line = line;
		state->stage = stageText;
		stage->entity()->setTextColorOverride(error
			? std::make_optional(st::boxTextFgError->c)
			: std::nullopt);
		stage->toggle(!stageText.isEmpty(), anim::type::instant);
		state->note = notes.join(QChar('\n'));
		note->toggle(!notes.isEmpty(), anim::type::instant);

		const auto showLink = status.finished()
			&& !status.playlistId.isEmpty()
			&& (status.added > 0 || !status.created);
		state->link = showLink
			? Cloud::MakeLink(Cloud::LinkKind::Playlist, status.playlistId)
			: QString();
		link->entity()->setLink(state->link.current());
		link->toggle(showLink, anim::type::instant);
		if (state->buttons < 0) {
			rebuildButtons();
		} else if (state->buttons != buttonsMark(status)) {
			// The status may change from a click on one of the buttons.
			Ui::PostponeCall(box, [=] {
				if (state->buttons != buttonsMark(state->status)) {
					rebuildButtons();
				}
			});
		}
	};
	std::move(args.status) | rpl::on_next(apply, box->lifetime());
	if (state->buttons < 0) {
		rebuildButtons();
	}

	box->boxClosing() | rpl::on_next([=] {
		if (closed) {
			closed();
		}
	}, box->lifetime());
}

// ---- A shared playlist.

class PlaylistBackend {
public:
	struct State {
		Playlist playlist; // Valid when there is something to show.
		bool loading = false;
		QString error;
		bool gone = false; // Not on the server any more: no «Повторить».
		bool kept = false;
		bool keeping = false;
		int keptTracks = 0;
		uint64 selfId = 0;
	};

	virtual ~PlaylistBackend() = default;

	[[nodiscard]] virtual State state() = 0;
	[[nodiscard]] virtual Playing playing() = 0;
	[[nodiscard]] virtual rpl::producer<> changes() = 0;
	[[nodiscard]] virtual rpl::producer<> closeRequests() = 0;

	virtual void reload() = 0;
	virtual void play(int index) = 0;
	virtual void toggleKept() = 0;
	virtual void setCollab(bool value) = 0;
	virtual void removeTrack(const QString &trackId) = 0;
	virtual void addFiles() = 0;
	virtual void remove() = 0;

};

void PlaylistBox(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<PlaylistBackend> backend) {
	struct State {
		base::unique_qptr<Ui::PopupMenu> menu;
		std::vector<std::pair<not_null<Row*>, Track>> rows;
		QStringList built;
		rpl::variable<QString> title;
		rpl::variable<QString> info;
		rpl::variable<QString> description;
		rpl::variable<QString> keepText;
		rpl::variable<QString> keepLabel;
		rpl::variable<QString> notice;
		rpl::variable<bool> collab = false;
		QString link;
		QString playlistId;
		bool owner = false;
		bool error = false;
		bool has = false;
		int buttons = -1;
		Fn<void()> rebuildButtons;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto show = box->uiShow();

	box->setWidth(st::boxWideWidth);
	box->setMaxHeight(st::boxMaxListHeight);
	box->setTitle(state->title.value());

	const auto content = box->verticalLayout();
	const auto header = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)),
		style::margins());
	const auto top = header->entity();
	top->add(
		object_ptr<Ui::FlatLabel>(
			top,
			state->info.value(),
			st::boxDividerLabel),
		RowPadding());
	const auto description = top->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			top,
			object_ptr<Ui::FlatLabel>(
				top,
				state->description.value(),
				st::boxLabel),
			RowPadding()),
		style::margins());

	const auto playWrap = top->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			top,
			object_ptr<Ui::VerticalLayout>(top)),
		style::margins());
	::Settings::AddButtonWithIcon(
		playWrap->entity(),
		tr::lng_oblivion_share_view_play(),
		st::settingsButton,
		{ &st::menuIconSoundOn }
	)->setClickedCallback([=] {
		backend->play(0);
	});
	const auto keep = ::Settings::AddButtonWithLabel(
		top,
		state->keepText.value(),
		state->keepLabel.value(),
		st::settingsButton,
		{ &st::menuIconDownload });
	keep->setClickedCallback([=] {
		backend->toggleKept();
	});
	const auto collabWrap = top->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			top,
			object_ptr<Ui::VerticalLayout>(top)),
		style::margins());
	const auto collab = ::Settings::AddButtonWithIcon(
		collabWrap->entity(),
		tr::lng_oblivion_share_view_collab(),
		st::settingsButton,
		{ &st::menuIconInvite });
	collab->toggleOn(state->collab.value());
	collab->toggledChanges(
	) | rpl::filter([=](bool value) {
		return (value != state->collab.current());
	}) | rpl::on_next([=](bool value) {
		state->collab = value;
		backend->setCollab(value);
	}, collab->lifetime());
	const auto addWrap = top->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			top,
			object_ptr<Ui::VerticalLayout>(top)),
		style::margins());
	::Settings::AddButtonWithIcon(
		addWrap->entity(),
		tr::lng_oblivion_share_view_add_files(),
		st::settingsButton,
		{ &st::menuIconSoundAdd }
	)->setClickedCallback([=] {
		backend->addFiles();
	});
	Ui::AddSkip(top);
	Ui::AddDivider(top);
	Ui::AddSkip(top);

	const auto list = content->add(
		object_ptr<Ui::VerticalLayout>(content),
		style::margins());
	const auto notice = content->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			content,
			object_ptr<Ui::FlatLabel>(
				content,
				state->notice.value(),
				st::membersAbout),
			st::boxRowPadding + style::margins(
				0,
				st::boxMediumSkip,
				0,
				st::boxMediumSkip)),
		style::margins(),
		style::al_top); // A short notice stays in the middle too.
	const auto retryWrap = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)),
		style::margins());
	::Settings::AddButtonWithIcon(
		retryWrap->entity(),
		tr::lng_oblivion_share_retry(),
		st::settingsButton,
		{ &st::menuIconRestore }
	)->setClickedCallback([=] {
		backend->reload();
	});
	const auto about = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)),
		style::margins());
	Ui::AddSkip(about->entity());
	Ui::AddDividerText(about->entity(), tr::lng_oblivion_share_view_about());

	const auto showTrackMenu = [=](const Track &track, bool removable) {
		state->menu = base::make_unique_q<Ui::PopupMenu>(
			box,
			st::popupMenuWithIcons);
		const auto id = track.id;
		if (removable) {
			state->menu->addAction(
				tr::lng_oblivion_share_view_remove_track(tr::now),
				[=] { backend->removeTrack(id); },
				&st::menuIconDelete);
		}
		if (state->menu->empty()) {
			state->menu = nullptr;
		} else {
			state->menu->popup(QCursor::pos());
		}
	};

	const auto updateRows = [=] {
		const auto playing = backend->playing();
		const auto ours = (playing.playlistId == state->playlistId);
		auto index = 0;
		for (const auto &[row, track] : state->rows) {
			const auto current = ours
				&& (playing.index == index)
				&& (playing.media == track.media);
			const auto loading = current && playing.loading;
			const auto failed = current && playing.failed;
			const auto status = loading
				? (tr::lng_oblivion_share_view_track_loading(tr::now)
					+ ((playing.progress > 0.)
						? (Dot() + Percent(playing.progress))
						: QString()))
				: failed
				? tr::lng_oblivion_share_view_track_failed(tr::now)
				: !track.title.isEmpty()
				? track.performer
				: QString();
			row->setTexts(
				track.title.isEmpty()
					? TrackName(track.title, track.performer, track.fileName)
					: track.title,
				status,
				(track.duration >= 1000)
					? Ui::FormatDurationText(track.duration / 1000)
					: QString());
			row->setActive(current && !failed);
			row->setFailed(failed);
			row->setProgress(loading ? playing.progress : -1.);
			++index;
		}
	};

	const auto refresh = [=] {
		const auto data = backend->state();
		const auto &playlist = data.playlist;
		const auto has = playlist.valid();
		state->playlistId = playlist.id;
		state->link = playlist.link;
		state->owner = playlist.canEdit;
		state->title = has
			? playlist.title
			: tr::lng_oblivion_share_view_title(tr::now);
		state->info = has ? PlaylistInfo(playlist) : QString();
		state->description = playlist.description;
		header->toggle(has, anim::type::instant);
		description->toggle(
			!playlist.description.isEmpty(),
			anim::type::instant);
		playWrap->toggle(!playlist.tracks.empty(), anim::type::instant);

		const auto total = int(playlist.tracks.size());
		state->keepText = data.kept
			? tr::lng_oblivion_share_view_unkeep(tr::now)
			: tr::lng_oblivion_share_view_keep(tr::now);
		// Before the click: how much will be saved to this device.
		state->keepLabel = !data.kept
			? ((playlist.totalBytes > 0)
				? SizeText(playlist.totalBytes)
				: QString())
			: (data.keeping || data.keptTracks < total)
			? tr::lng_oblivion_share_view_keeping(
				tr::now,
				lt_ready,
				QString::number(data.keptTracks),
				lt_total,
				QString::number(total))
			: tr::lng_oblivion_share_view_kept_all(tr::now);
		if (state->collab.current() != playlist.collab) {
			state->collab = playlist.collab;
		}
		collabWrap->toggle(has && playlist.canEdit, anim::type::instant);
		addWrap->toggle(has && playlist.canAdd, anim::type::instant);

		auto ids = QStringList();
		for (const auto &track : playlist.tracks) {
			ids.push_back(track.id);
		}
		if (ids != state->built) {
			state->built = ids;
			state->rows.clear();
			list->clear();
			auto number = 0;
			for (const auto &track : playlist.tracks) {
				const auto index = number++;
				// The owner removes any track, the others what they added.
				const auto removable = playlist.canEdit
					|| (data.selfId && track.addedBy == data.selfId);
				const auto row = list->add(object_ptr<Row>(list, removable));
				row->setIndex(QString::number(index + 1));
				row->setClickedCallback([=] {
					backend->play(index);
				});
				row->menuRequests() | rpl::on_next([=] {
					showTrackMenu(track, removable);
				}, row->lifetime());
				state->rows.emplace_back(row, track);
			}
			list->resizeToWidth(content->width());
		}
		updateRows();

		const auto text = !has
			? (data.loading
				? tr::lng_oblivion_share_view_loading(tr::now)
				: data.error)
			: !data.error.isEmpty()
			? data.error
			: playlist.tracks.empty()
			? tr::lng_oblivion_share_view_empty(tr::now)
			: QString();
		state->notice = text;
		// A failure is red like in the other boxes; "it is gone" is not
		// one, it is what has happened to the playlist.
		notice->entity()->setTextColorOverride(
			(!data.loading && !data.error.isEmpty() && !data.gone)
				? std::make_optional(st::boxTextFgError->c)
				: std::nullopt);
		notice->toggle(!text.isEmpty(), anim::type::instant);
		// A playlist that was deleted will not come back with a retry.
		retryWrap->toggle(
			!has && !data.loading && !data.error.isEmpty() && !data.gone,
			anim::type::instant);
		about->toggle(has && !playlist.tracks.empty(), anim::type::instant);

		state->has = has;
		if (state->buttons >= 0 && state->buttons != (has ? 1 : 0)) {
			// Not from inside of a click on one of the buttons.
			Ui::PostponeCall(box, [=] {
				if (state->rebuildButtons) {
					state->rebuildButtons();
				}
			});
		}
	};

	const auto showMenu = [=] {
		if (state->playlistId.isEmpty()) {
			return;
		}
		state->menu = base::make_unique_q<Ui::PopupMenu>(
			box,
			st::popupMenuWithIcons);
		state->menu->addAction(
			tr::lng_oblivion_share_copy_link(tr::now),
			[=] { CopyLink(show, state->link); },
			&st::menuIconLink);
		if (state->owner) {
			state->menu->addAction(
				tr::lng_oblivion_share_view_delete(tr::now),
				[=] { backend->remove(); },
				&st::menuIconDelete);
		}
		state->menu->popup(QCursor::pos());
	};
	// While there is no playlist (it is being loaded, it is gone) there
	// is no link to copy and nothing for the menu: only «Закрыть».
	state->rebuildButtons = [=] {
		const auto mark = state->has ? 1 : 0;
		if (std::exchange(state->buttons, mark) == mark) {
			return;
		}
		box->clearButtons();
		box->addButton(tr::lng_close(), [=] {
			box->closeBox();
		});
		if (state->has) {
			box->addLeftButton(tr::lng_oblivion_share_copy_link(), [=] {
				CopyLink(show, state->link);
			});
			box->addTopButton(st::boxTitleMenu, showMenu);
		}
	};

	backend->changes() | rpl::on_next(refresh, box->lifetime());
	backend->closeRequests() | rpl::on_next([=] {
		box->closeBox();
	}, box->lifetime());
	refresh();
	state->rebuildButtons();
}

// ---- The list of the shared playlists of the user.

struct LibraryArgs {
	// What is known without the server: the playlists kept on this device.
	std::vector<Playlist> kept;
	// The published and the kept ones from the server.
	Fn<void(Fn<void(std::vector<Playlist>)> done, Cloud::Fail fail)> load;
	rpl::producer<> reloads;
	Fn<void(const Playlist &playlist)> open;
};

void LibraryBox(not_null<Ui::GenericBox*> box, LibraryArgs &&args) {
	struct State {
		std::vector<Playlist> list;
		rpl::variable<QString> notice;
		bool loading = false;
		bool loaded = false;
		QString error;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto load = std::move(args.load);
	const auto open = std::move(args.open);
	state->list = std::move(args.kept);

	box->setWidth(st::boxWideWidth);
	box->setMaxHeight(st::boxMaxListHeight);
	box->setTitle(tr::lng_oblivion_share_library());

	const auto content = box->verticalLayout();
	const auto list = content->add(
		object_ptr<Ui::VerticalLayout>(content),
		style::margins());
	const auto notice = content->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			content,
			object_ptr<Ui::FlatLabel>(
				content,
				state->notice.value(),
				st::membersAbout),
			st::boxRowPadding + style::margins(
				0,
				st::boxMediumSkip,
				0,
				st::boxMediumSkip)),
		style::margins(),
		style::al_top); // A short notice stays in the middle too.
	Ui::AddSkip(content);
	Ui::AddDividerText(content, tr::lng_oblivion_share_library_about());

	const auto rebuild = [=] {
		list->clear();
		for (const auto &playlist : state->list) {
			const auto row = list->add(object_ptr<Row>(list, false));
			row->setCover(ColorIndex(playlist.id), &st::infoIconMediaAudio);
			row->setTexts(playlist.title, PlaylistInfo(playlist), QString());
			row->setClickedCallback([=] {
				if (open) {
					open(playlist);
				}
			});
		}
		list->resizeToWidth(content->width());
		const auto text = !state->list.empty()
			? state->error
			: state->loading
			? tr::lng_oblivion_share_library_loading(tr::now)
			: !state->error.isEmpty()
			? state->error
			: tr::lng_oblivion_share_library_empty(tr::now);
		state->notice = text;
		notice->entity()->setTextColorOverride(
			(!state->loading && !state->error.isEmpty())
				? std::make_optional(st::boxTextFgError->c)
				: std::nullopt);
		notice->toggle(!text.isEmpty(), anim::type::instant);
	};
	const auto request = [=] {
		if (state->loading || !load) {
			return;
		}
		state->loading = true;
		state->error = QString();
		rebuild();
		load(crl::guard(box, [=](std::vector<Playlist> playlists) {
			state->loading = false;
			state->loaded = true;
			state->list = std::move(playlists);
			rebuild();
		}), crl::guard(box, [=](const Cloud::Error &error) {
			state->loading = false;
			state->error = Cloud::ErrorText(error);
			rebuild();
		}));
	};
	if (args.reloads) {
		std::move(args.reloads) | rpl::on_next([=] {
			if (state->loaded) {
				request();
			}
		}, box->lifetime());
	}
	rebuild();
	request();

	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
	});
}

// ---- Presets.

void SaveNameBox(
		not_null<Ui::GenericBox*> box,
		rpl::producer<QString> title,
		QString initial,
		QString about,
		rpl::producer<QString> submitText,
		Fn<void(QString)> done) {
	box->setTitle(std::move(title));
	const auto field = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::lng_oblivion_share_preset_name(),
			initial.left(kMaxPresetTitle)));
	field->setMaxLength(kMaxPresetTitle);
	if (!about.isEmpty()) {
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(about),
				st::boxDividerLabel),
			st::boxRowPadding + style::margins(0, st::boxLittleSkip, 0, 0));
	}
	box->setFocusCallback([=] {
		field->setFocusFast();
		field->selectAll();
	});
	const auto submit = [=] {
		const auto name = field->getLastText().simplified();
		if (name.isEmpty()) {
			field->showError();
			return;
		}
		const auto callback = done;
		box->closeBox();
		if (callback) {
			callback(name);
		}
	};
	field->submits() | rpl::on_next(submit, field->lifetime());
	box->addButton(std::move(submitText), submit);
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
}

struct MyPresetsArgs {
	QString kind; // Empty: of both editors.
	Fn<std::vector<LocalPreset>()> list;
	rpl::producer<> changes;
	Fn<void(const LocalPreset &preset)> apply; // Null without an editor.
	Fn<void(uint64 id, const QString &title)> rename;
	Fn<void(uint64 id)> remove;
	Fn<void(const LocalPreset &preset)> share;
	Fn<void()> gallery;
};

void MyPresetsBox(not_null<Ui::GenericBox*> box, MyPresetsArgs &&args) {
	struct State {
		base::unique_qptr<Ui::PopupMenu> menu;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto show = box->uiShow();
	const auto kind = args.kind;
	const auto getList = std::move(args.list);
	const auto apply = std::move(args.apply);
	const auto rename = std::move(args.rename);
	const auto remove = std::move(args.remove);
	const auto share = std::move(args.share);
	const auto gallery = std::move(args.gallery);

	box->setWidth(st::boxWideWidth);
	box->setMaxHeight(st::boxMaxListHeight);
	box->setTitle(tr::lng_oblivion_share_mine_title());

	const auto content = box->verticalLayout();
	const auto list = content->add(
		object_ptr<Ui::VerticalLayout>(content),
		style::margins());
	const auto empty = content->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			content,
			object_ptr<Ui::FlatLabel>(
				content,
				tr::lng_oblivion_share_mine_empty(),
				st::membersAbout),
			st::boxRowPadding + style::margins(
				0,
				st::boxMediumSkip,
				0,
				st::boxMediumSkip)),
		style::margins(),
		style::al_top);

	const auto applyIt = [=](const LocalPreset &preset) {
		if (!apply) {
			return;
		}
		const auto callback = apply;
		const auto copy = preset;
		box->closeBox();
		callback(copy);
	};
	const auto showMenu = [=](const LocalPreset &preset) {
		state->menu = base::make_unique_q<Ui::PopupMenu>(
			box,
			st::popupMenuWithIcons);
		const auto id = preset.id;
		const auto title = preset.title;
		if (apply) {
			state->menu->addAction(
				tr::lng_oblivion_share_preset_apply(tr::now),
				[=] { applyIt(preset); },
				&st::menuIconPalette);
		}
		state->menu->addAction(
			tr::lng_oblivion_share_mine_rename(tr::now),
			[=] {
				show->showBox(Box(
					SaveNameBox,
					tr::lng_oblivion_share_mine_rename(),
					title,
					QString(),
					tr::lng_settings_save(),
					[=](QString name) {
						if (rename) {
							rename(id, name);
						}
					}));
			},
			&st::menuIconEdit);
		if (share) {
			state->menu->addAction(
				tr::lng_oblivion_share_mine_share(tr::now),
				[=] { share(preset); },
				&st::menuIconShare);
		}
		state->menu->addAction(
			tr::lng_oblivion_share_mine_delete(tr::now),
			[=] {
				show->showBox(Ui::MakeConfirmBox({
					.text = tr::lng_oblivion_share_mine_delete_sure(
						lt_name,
						rpl::single(title)),
					.confirmed = [=](Fn<void()> close) {
						if (remove) {
							remove(id);
						}
						close();
					},
					.confirmText = tr::lng_box_delete(),
					.confirmStyle = &st::attentionBoxButton,
				}));
			},
			&st::menuIconDelete);
		state->menu->popup(QCursor::pos());
	};
	const auto rebuild = [=] {
		list->clear();
		auto count = 0;
		const auto presets = getList ? getList() : std::vector<LocalPreset>();
		for (const auto &preset : presets) {
			if (!kind.isEmpty() && preset.kind != kind) {
				continue;
			}
			++count;
			const auto row = list->add(object_ptr<Row>(list, true));
			row->setCover(
				ColorIndex(QString::number(preset.id)),
				&st::menuIconPalette);
			row->setTexts(
				preset.title,
				EffectNames(
					preset.kind,
					StackEffects(preset.kind, preset.stack)),
				kind.isEmpty() ? KindName(preset.kind) : QString());
			row->setClickedCallback([=] {
				if (apply) {
					applyIt(preset);
				} else {
					showMenu(preset);
				}
			});
			row->menuRequests() | rpl::on_next([=] {
				showMenu(preset);
			}, row->lifetime());
		}
		list->resizeToWidth(content->width());
		empty->toggle(!count, anim::type::instant);
	};
	if (args.changes) {
		std::move(args.changes) | rpl::on_next([=] {
			// Not from inside of the click that has changed the list.
			Ui::PostponeCall(box, rebuild);
		}, box->lifetime());
	}
	rebuild();

	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
	});
	if (gallery) {
		box->addLeftButton(tr::lng_oblivion_share_gallery_title(), [=] {
			gallery();
		});
	}
}

void LinkBox(
		not_null<Ui::GenericBox*> box,
		rpl::producer<QString> title,
		rpl::producer<QString> about,
		QString link) {
	box->setTitle(std::move(title));
	box->setWidth(st::boxWideWidth);
	box->addRow(
		object_ptr<Ui::FlatLabel>(box, std::move(about), st::boxLabel),
		RowPadding());
	const auto show = box->uiShow();
	const auto field = box->addRow(object_ptr<LinkField>(box), RowPadding());
	field->setLink(link);
	field->setClickedCallback([=] {
		CopyLink(show, link);
	});
	box->addButton(tr::lng_oblivion_share_copy_link(), [=] {
		CopyLink(show, link);
	});
	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
	});
}

struct SharePresetArgs {
	QString kind;
	QString title;
	QString effects; // Their names.
	Fn<void(
		const QString &title,
		const QString &text,
		bool listed,
		Fn<void(const QString &link)> done,
		Cloud::Fail fail)> submit;
};

void SharePresetBox(not_null<Ui::GenericBox*> box, SharePresetArgs &&args) {
	struct State {
		rpl::variable<QString> error;
		bool sending = false;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto submit = std::move(args.submit);
	const auto show = box->uiShow();

	box->setTitle(tr::lng_oblivion_share_preset_share_title());
	box->setWidth(st::boxWideWidth);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_oblivion_share_preset_share_about(),
			st::boxLabel),
		RowPadding());
	if (!args.effects.isEmpty()) {
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(KindName(args.kind) + Dot() + args.effects),
				st::boxDividerLabel),
			RowPadding());
	}
	const auto title = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::lng_oblivion_share_preset_name(),
			args.title.left(kMaxPresetTitle)),
		RowPadding());
	title->setMaxLength(kMaxPresetTitle);
	const auto text = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::lng_oblivion_share_preset_text(),
			QString()),
		RowPadding());
	text->setMaxLength(kMaxPresetDescription / 2);
	const auto listed = box->addRow(
		object_ptr<Ui::Checkbox>(
			box,
			tr::lng_oblivion_share_preset_listed(tr::now),
			false, // The gallery is for everybody: only by a click.
			st::defaultCheckbox),
		st::boxRowPadding + style::margins(
			0,
			st::boxLittleSkip,
			0,
			st::boxLittleSkip));
	const auto error = box->addRow(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			box,
			object_ptr<Ui::FlatLabel>(
				box,
				state->error.value(),
				st::boxDividerLabel),
			style::margins(0, 0, 0, st::boxLittleSkip)),
		st::boxRowPadding);
	error->entity()->setTextColorOverride(st::boxTextFgError->c);
	error->toggle(false, anim::type::instant);

	const auto send = [=] {
		const auto name = title->getLastText().simplified();
		if (state->sending) {
			return;
		} else if (name.isEmpty() || !submit) {
			title->showError();
			return;
		}
		state->sending = true;
		error->toggle(false, anim::type::instant);
		submit(
			name,
			text->getLastText().simplified(),
			listed->checked(),
			crl::guard(box, [=](const QString &link) {
				box->closeBox();
				show->showBox(Box(
					LinkBox,
					tr::lng_oblivion_share_preset_published_title(),
					tr::lng_oblivion_share_preset_published_about(),
					link));
			}),
			crl::guard(box, [=](const Cloud::Error &failure) {
				state->sending = false;
				state->error = (failure.is("limit_reached")
					&& failure.detail("limit") == u"presets"_q)
					? tr::lng_oblivion_share_preset_cloud_limit(tr::now)
					: Cloud::ErrorText(failure);
				error->toggle(true, anim::type::instant);
			}));
	};
	title->submits() | rpl::on_next(send, title->lifetime());
	text->submits() | rpl::on_next(send, text->lifetime());
	box->setFocusCallback([=] {
		title->setFocusFast();
	});
	box->addButton(tr::lng_oblivion_share_preset_publish(), send);
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
}

struct PresetBoxArgs {
	Preset preset; // A summary or a full one.
	// Null when the preset is full already.
	Fn<void(Fn<void(Preset)> done, Cloud::Fail fail)> load;
	Fn<void(const Preset &preset)> apply; // Null without an editor.
	Fn<void(const Preset &preset)> save; // To «Мои наборы».
	Fn<bool(const Preset &preset)> saved;
	Fn<void(const Preset &preset)> report;
	Fn<void(const Preset &preset, Fn<void()> done)> remove;
};

void PresetBox(not_null<Ui::GenericBox*> box, PresetBoxArgs &&args) {
	struct State {
		Preset preset;
		base::unique_qptr<Ui::PopupMenu> menu;
		rpl::variable<QString> title;
		rpl::variable<QString> meta;
		rpl::variable<QString> text;
		rpl::variable<QString> notice;
		Ui::VerticalLayout *pills = nullptr;
		Fn<void()> refresh;
		Fn<void()> showMenu;
		bool loading = false;
		bool rendered = false;
		bool wasSaved = false;
		int buttons = -1;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto show = box->uiShow();
	const auto load = std::move(args.load);
	const auto apply = std::move(args.apply);
	const auto save = std::move(args.save);
	const auto saved = std::move(args.saved);
	const auto report = std::move(args.report);
	const auto remove = std::move(args.remove);
	state->preset = std::move(args.preset);

	box->setTitle(state->title.value());
	box->setWidth(st::boxWideWidth);

	const auto ratio = style::DevicePixelRatio();
	const auto sample = SampleImage(
		QSize(kPreviewWidth, kPreviewHeight) * std::min(ratio, 2) / 2);
	// Hidden for a preset this build can't open: the sample without any
	// effect on it would only mislead.
	const auto previewWrap = box->addRow(
		object_ptr<Ui::SlideWrap<SplitPreview>>(
			box,
			object_ptr<SplitPreview>(box, sample),
			style::margins(0, 0, 0, st::boxLittleSkip)),
		st::boxRowPadding);
	const auto preview = previewWrap->entity();
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			state->meta.value(),
			st::boxDividerLabel),
		RowPadding());
	const auto text = box->addRow(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			box,
			object_ptr<Ui::FlatLabel>(box, state->text.value(), st::boxLabel),
			style::margins(0, 0, 0, st::boxLittleSkip)),
		st::boxRowPadding);
	state->pills = box->addRow(
		object_ptr<Ui::VerticalLayout>(box),
		st::boxRowPadding);
	const auto notice = box->addRow(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			box,
			object_ptr<Ui::FlatLabel>(
				box,
				state->notice.value(),
				st::boxDividerLabel),
			style::margins(0, st::boxLittleSkip, 0, 0)),
		st::boxRowPadding);

	const auto render = [=] {
		const auto &preset = state->preset;
		if (state->rendered || preset.stack.isEmpty()) {
			return;
		}
		state->rendered = true;
		const auto kind = preset.kind;
		const auto stack = preset.stack;
		const auto weak = QPointer<SplitPreview>(preview);
		crl::async([=] {
			auto image = RenderPreview(kind, stack, sample);
			crl::on_main(weak, [=, image = std::move(image)]() mutable {
				if (const auto strong = weak.data()) {
					strong->setAfter(std::move(image));
				}
			});
		});
	};
	const auto rebuildButtons = [=] {
		const auto &preset = state->preset;
		const auto usable = preset.full && !preset.stack.isEmpty();
		const auto kept = usable && saved && saved(preset);
		const auto mark = (usable ? 1 : 0)
			| ((usable && apply) ? 2 : 0)
			| (kept ? 4 : 0);
		if (std::exchange(state->buttons, mark) == mark) {
			return;
		}
		box->clearButtons();
		// clearButtons() takes the dots of the menu away as well.
		box->addTopButton(st::boxTitleMenu, [=] {
			if (state->showMenu) {
				state->showMenu();
			}
		});
		if (usable && apply) {
			box->addButton(tr::lng_oblivion_share_preset_apply(), [=] {
				const auto callback = apply;
				const auto copy = state->preset;
				box->closeBox();
				callback(copy);
			});
		}
		if (usable && save && !kept) {
			box->addButton(tr::lng_oblivion_share_preset_keep(), [=] {
				save(state->preset);
				state->wasSaved = true;

				// The button of this click goes away: not from inside of it.
				Ui::PostponeCall(box, [=] {
					if (state->refresh) {
						state->refresh();
					}
				});
			});
		}
		box->addButton(tr::lng_close(), [=] {
			box->closeBox();
		});
	};
	const auto refresh = [=] {
		const auto &preset = state->preset;
		state->title = preset.title.isEmpty()
			? tr::lng_oblivion_share_gallery_title(tr::now)
			: preset.title;
		auto meta = QStringList{ KindName(preset.kind) };
		if (!preset.ownerName.isEmpty()) {
			meta.push_back(tr::lng_oblivion_share_view_by(
				tr::now,
				lt_name,
				preset.ownerName));
		}
		if (preset.uses > 0) {
			meta.push_back(tr::lng_oblivion_share_gallery_uses(
				tr::now,
				lt_count,
				preset.uses));
		}
		state->meta = meta.join(Dot());
		state->text = preset.text;
		text->toggle(!preset.text.isEmpty(), anim::type::instant);

		state->pills->clear();
		auto names = QStringList();
		for (const auto &tag : preset.tags) {
			const auto name = EffectName(preset.kind, tag);
			if (!name.isEmpty() && !names.contains(name)) {
				names.push_back(name);
			}
		}
		if (!names.isEmpty()) {
			state->pills->add(
				object_ptr<Pills>(state->pills, names),
				style::margins(0, 0, 0, st::boxLittleSkip));
		}
		state->pills->resizeToWidth(state->pills->width());

		const auto tooNew = preset.full && preset.stack.isEmpty();
		previewWrap->toggle(!tooNew, anim::type::instant);

		// Saved already and no editor to apply it in: without a word the
		// box would have nothing but «Закрыть».
		const auto kept = preset.full
			&& !preset.stack.isEmpty()
			&& saved
			&& saved(preset);
		const auto message = state->loading
			? tr::lng_oblivion_share_preset_loading(tr::now)
			: tooNew
			? tr::lng_oblivion_share_preset_too_new(tr::now)
			: preset.hidden
			? tr::lng_oblivion_share_preset_hidden(tr::now)
			: (state->wasSaved || (kept && !apply))
			? tr::lng_oblivion_share_preset_kept_hint(tr::now)
			: QString();
		state->notice = message;
		notice->entity()->setTextColorOverride(tooNew
			? std::make_optional(st::boxTextFgError->c)
			: std::nullopt);
		notice->toggle(!message.isEmpty(), anim::type::instant);
		render();
		rebuildButtons();
	};
	state->refresh = refresh;

	const auto weakBox = QPointer<Ui::GenericBox>(box.get());
	state->showMenu = [=] {
		const auto &preset = state->preset;
		state->menu = base::make_unique_q<Ui::PopupMenu>(
			box,
			st::popupMenuWithIcons);
		const auto link = preset.link;
		state->menu->addAction(
			tr::lng_oblivion_share_copy_link(tr::now),
			[=] { CopyLink(show, link); },
			&st::menuIconLink);
		if (preset.canEdit && remove) {
			state->menu->addAction(
				tr::lng_oblivion_share_view_delete(tr::now),
				[=] {
					show->showBox(Ui::MakeConfirmBox({
						.text = tr::lng_oblivion_share_preset_delete_sure(),
						.confirmed = [=](Fn<void()> close) {
							close();
							if (const auto strong = weakBox.data()) {
								remove(state->preset, crl::guard(strong, [=] {
									strong->closeBox();
								}));
							}
						},
						.confirmText = tr::lng_box_delete(),
						.confirmStyle = &st::attentionBoxButton,
					}));
				},
				&st::menuIconDelete);
		} else if (!preset.canEdit && report) {
			state->menu->addAction(
				tr::lng_oblivion_share_preset_report(tr::now),
				[=] {
					show->showBox(Ui::MakeConfirmBox({
						.text = tr::lng_oblivion_share_preset_report_sure(),
						.confirmed = [=](Fn<void()> close) {
							close();
							if (weakBox) {
								report(state->preset);
							}
						},
						.confirmText = tr::lng_oblivion_share_preset_report(),
					}));
				},
				&st::menuIconReport);
		}
		state->menu->popup(QCursor::pos());
	};

	if (load && !state->preset.full) {
		state->loading = true;
		load(crl::guard(box, [=](Preset preset) {
			state->loading = false;
			if (preset.valid()) {
				state->preset = std::move(preset);
			}
			refresh();
		}), crl::guard(box, [=](const Cloud::Error &error) {
			state->loading = false;
			refresh();
			state->notice = Cloud::ErrorText(error);
			notice->entity()->setTextColorOverride(st::boxTextFgError->c);
			notice->toggle(true, anim::type::instant);
		}));
	}
	refresh();
}

struct GalleryPage {
	std::vector<Preset> presets;
	QString next;
};

struct GalleryArgs {
	QString kind; // Fixed (an editor has asked), or empty for a switch.
	Fn<void(
		const QString &kind,
		const QString &sort,
		const QString &cursor,
		Fn<void(GalleryPage)> done,
		Cloud::Fail fail)> load;
	Fn<void(const Preset &summary)> open;
};

void GalleryBox(not_null<Ui::GenericBox*> box, GalleryArgs &&args) {
	struct State {
		QString kind;
		QString sort = u"new"_q;
		QString next;
		std::vector<Preset> presets;
		rpl::variable<QString> notice;
		int generation = 0;
		bool loading = false;
		bool error = false;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto load = std::move(args.load);
	const auto open = std::move(args.open);
	const auto fixed = !args.kind.isEmpty();
	state->kind = fixed ? args.kind : u"photo"_q;

	box->setWidth(st::boxWideWidth);
	box->setMaxHeight(st::boxMaxListHeight);
	box->setTitle(fixed
		? rpl::single(tr::lng_oblivion_share_gallery_title(tr::now)
			+ Dot()
			+ KindName(args.kind))
		: tr::lng_oblivion_share_gallery_title());

	const auto content = box->verticalLayout();
	const auto kinds = fixed
		? nullptr
		: content->add(
			object_ptr<Ui::SettingsSlider>(content, st::settingsSlider),
			st::boxRowPadding);
	if (kinds) {
		kinds->setSections(std::vector<QString>{
			tr::lng_oblivion_share_kind_photo(tr::now),
			tr::lng_oblivion_share_kind_video(tr::now),
		});
	}
	// The order is a pair of chips: a second slider right under the first
	// one reads as its underline.
	const auto sorts = content->add(
		object_ptr<Chips>(content, QStringList{
			tr::lng_oblivion_share_gallery_new(tr::now),
			tr::lng_oblivion_share_gallery_top(tr::now),
		}),
		st::boxRowPadding + style::margins(
			0,
			fixed ? (st::boxLittleSkip / 2) : st::boxLittleSkip,
			0,
			st::boxLittleSkip));

	const auto list = content->add(
		object_ptr<Ui::VerticalLayout>(content),
		style::margins());
	const auto notice = content->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			content,
			object_ptr<Ui::FlatLabel>(
				content,
				state->notice.value(),
				st::membersAbout),
			st::boxRowPadding + style::margins(
				0,
				st::boxMediumSkip,
				0,
				st::boxMediumSkip)),
		style::margins(),
		style::al_top); // A short notice stays in the middle too.
	const auto moreWrap = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)),
		style::margins());
	const auto more = ::Settings::AddButtonWithIcon(
		moreWrap->entity(),
		tr::lng_oblivion_share_gallery_more(),
		st::settingsButton,
		{ &st::menuIconShowAll });
	// The first page did not come: without this the only way to ask
	// again was to switch the kind or the order there and back.
	const auto retryWrap = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)),
		style::margins());
	const auto retry = ::Settings::AddButtonWithIcon(
		retryWrap->entity(),
		tr::lng_oblivion_share_retry(),
		st::settingsButton,
		{ &st::menuIconRestore });
	retryWrap->toggle(false, anim::type::instant);
	Ui::AddSkip(content);

	const auto refresh = [=](int from) {
		const auto count = int(state->presets.size());
		if (!from) {
			list->clear();
		}
		for (auto i = from; i < count; ++i) {
			const auto preset = state->presets[i];
			list->add(object_ptr<Card>(list, preset))->setClickedCallback([=] {
				if (open) {
					open(preset);
				}
			});
		}
		list->resizeToWidth(content->width());
		notice->entity()->setTextColorOverride(state->error
			? std::make_optional(st::boxTextFgError->c)
			: std::nullopt);
		notice->toggle(
			!state->notice.current().isEmpty(),
			anim::type::instant);
		moreWrap->toggle(
			!state->next.isEmpty() && !state->loading,
			anim::type::instant);
		retryWrap->toggle(
			state->error && state->presets.empty() && !state->loading,
			anim::type::instant);
	};
	const auto request = [=](bool reset) {
		if (!load || (state->loading && !reset)) {
			return;
		}
		if (reset) {
			state->presets.clear();
			state->next = QString();
		}
		const auto generation = ++state->generation;
		const auto from = int(state->presets.size());
		state->loading = true;
		state->error = false;
		state->notice = from
			? QString()
			: tr::lng_oblivion_share_gallery_loading(tr::now);
		refresh(reset ? 0 : from);
		load(
			state->kind,
			state->sort,
			state->next,
			crl::guard(box, [=](GalleryPage page) {
				if (state->generation != generation) {
					return;
				}
				state->loading = false;
				state->next = page.next;
				for (auto &preset : page.presets) {
					state->presets.push_back(std::move(preset));
				}
				state->notice = state->presets.empty()
					? tr::lng_oblivion_share_gallery_empty(tr::now)
					: QString();
				refresh(from);
			}),
			crl::guard(box, [=](const Cloud::Error &error) {
				if (state->generation != generation) {
					return;
				}
				state->loading = false;
				state->error = true;
				state->notice = Cloud::ErrorText(error);
				refresh(from);
			}));
	};
	if (kinds) {
		kinds->sectionActivated() | rpl::on_next([=](int index) {
			const auto kind = index ? u"video"_q : u"photo"_q;
			if (state->kind != kind) {
				state->kind = kind;
				request(true);
			}
		}, kinds->lifetime());
	}
	sorts->activated() | rpl::on_next([=](int index) {
		const auto sort = index ? u"top"_q : u"new"_q;
		if (state->sort != sort) {
			state->sort = sort;
			request(true);
		}
	}, sorts->lifetime());
	more->setClickedCallback([=] {
		request(false);
	});
	retry->setClickedCallback([=] {
		request(true);
	});
	request(true);

	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
	});
}

// ---- The session part.

[[nodiscard]] Main::Session *ActiveSession() {
	if (!Core::IsAppLaunched()) {
		return nullptr;
	}
	const auto window = Core::App().activePrimaryWindow();
	const auto controller = window ? window->sessionController() : nullptr;
	return controller ? &controller->session() : nullptr;
}

void Request(
		const base::weak_ptr<Main::Session> &weak,
		Cloud::Request &&request,
		Cloud::Done done,
		Cloud::Fail fail) {
	if (const auto session = weak.get()) {
		Cloud::For(session).request(
			std::move(request),
			std::move(done),
			std::move(fail));
	} else if (fail) {
		fail({ .type = Cloud::Error::Type::NotConnected });
	}
}

[[nodiscard]] QString UploadErrorText(
		const Cloud::Error &error,
		int64 quota,
		int64 daily,
		int64 tracks,
		int64 playlists) {
	if (error.is("limit_reached")) {
		const auto limit = error.detail("limit");
		if (limit == u"playlist_quota"_q) {
			return tr::lng_oblivion_share_error_quota(
				tr::now,
				lt_size,
				SizeText(quota));
		} else if (limit == u"playlist_tracks"_q) {
			return tr::lng_oblivion_share_error_tracks(
				tr::now,
				lt_limit,
				QString::number(tracks));
		} else if (limit == u"playlists"_q) {
			return tr::lng_oblivion_share_error_playlists(
				tr::now,
				lt_limit,
				QString::number(playlists));
		} else if (limit == u"upload_daily"_q) {
			return tr::lng_oblivion_share_error_daily(
				tr::now,
				lt_size,
				SizeText(daily));
		} else if (limit == u"uploads"_q || limit == u"uploads_address"_q) {
			return tr::lng_oblivion_share_error_uploads(tr::now);
		} else if (limit == u"user_tracks"_q) {
			// All the playlists of the account together: a number of
			// tracks, or the size of what is written about them.
			return tr::lng_oblivion_share_error_user_tracks(tr::now);
		}
	} else if (error.status == 404) {
		return tr::lng_oblivion_share_error_gone(tr::now);
	}
	return Cloud::ErrorText(error);
}

void ShowPlaylist(
	not_null<Main::Session*> session,
	std::shared_ptr<Ui::Show> show,
	const QString &id);

void ShowUploadBox(
		not_null<Main::Session*> session,
		std::shared_ptr<Ui::Show> show) {
	const auto service = &ServiceFor(session);
	const auto uploader = service->uploader();
	if (!uploader || !show || !show->valid()) {
		return;
	}
	const auto weak = base::make_weak(service);
	const auto weakSession = base::make_weak(session);
	auto &account = service->account();
	const auto quota = account.limit("playlist_quota", int64(3) << 30);
	const auto daily = account.limit("upload_daily_bytes", int64(6) << 30);
	const auto tracks = account.limit("playlist_tracks", kMaxPlaylistTracks);
	const auto playlists = account.limit("playlists", 50);
	++uploader->shownBoxes;
	show->showBox(Box(UploadBox, UploadBoxArgs{
		.status = uploader->statusValue(),
		.cancel = [=] {
			const auto strong = weak.get();
			if (const auto current = strong ? strong->uploader() : nullptr) {
				current->cancel();
			}
		},
		.retry = [=] {
			const auto strong = weak.get();
			if (const auto current = strong ? strong->uploader() : nullptr) {
				current->retry();
			}
		},
		.open = [=](const QString &playlistId) {
			if (const auto strong = weakSession.get()) {
				ShowPlaylist(strong, show, playlistId);
			}
		},
		.errorText = [=](const Cloud::Error &error) {
			return UploadErrorText(error, quota, daily, tracks, playlists);
		},
		.closed = [=] {
			const auto strong = weak.get();
			if (const auto current = strong ? strong->uploader() : nullptr) {
				// Another upload may have replaced the one of this box.
				if (current == uploader && current->shownBoxes > 0) {
					--current->shownBoxes;
				}
				strong->dropUpload();
			}
		},
	}));
}

void StartUpload(
		not_null<Main::Session*> session,
		std::shared_ptr<Ui::Show> show,
		UploadRequest &&request) {
	const auto service = &ServiceFor(session);
	const auto uploader = service->startUpload(std::move(request));
	if (!uploader) {
		Toast(show, tr::lng_oblivion_share_upload_busy(tr::now));
	} else {
		// The box tells the result itself while it is there, a hidden
		// upload ends with a toast.
		const auto weak = base::make_weak(service);
		uploader->statusValue(
		) | rpl::filter([](const UploadStatus &status) {
			return status.finished();
		}) | rpl::take(1) | rpl::on_next([=](const UploadStatus &status) {
			using Stage = UploadStatus::Stage;
			if (uploader->shownBoxes > 0) {
				return;
			} else if (!Core::IsAppLaunched()
				|| !Core::App().passcodeLocked()) {
				// Nothing about a cancelled upload (the user or a logout
				// has cancelled it), and no name of a playlist over the
				// passcode lock.
				Toast(show, (status.stage == Stage::Done)
					? tr::lng_oblivion_share_upload_done_toast(
						tr::now,
						lt_name,
						status.title)
					: (status.stage == Stage::Failed)
					? tr::lng_oblivion_share_upload_failed_toast(
						tr::now,
						lt_name,
						status.title)
					: QString());
			}
			if (const auto strong = weak.get()) {
				strong->dropUpload();
			}
		}, uploader->lifetime());
	}
	ShowUploadBox(session, show);
}

class SessionPlaylistBackend final
	: public PlaylistBackend
	, public base::has_weak_ptr {
public:
	SessionPlaylistBackend(
		not_null<Main::Session*> session,
		std::shared_ptr<Ui::Show> show,
		const QString &id);

	State state() override;
	Playing playing() override;
	rpl::producer<> changes() override;
	rpl::producer<> closeRequests() override;

	void reload() override;
	void play(int index) override;
	void toggleKept() override;
	void setCollab(bool value) override;
	void removeTrack(const QString &trackId) override;
	void addFiles() override;
	void remove() override;

private:
	void apply(const Cloud::Response &response);
	void fail(const Cloud::Error &error);

	const base::weak_ptr<Main::Session> _session;
	const base::weak_ptr<Service> _service;
	const std::shared_ptr<Ui::Show> _show;
	const QString _id;
	Cloud::Sender _sender;
	Playlist _playlist;
	QString _error;
	bool _gone = false; // The error is "not on the server any more".
	bool _loading = false;
	int _keptTracks = -1; // Counted on the disk, -1: count again.
	rpl::event_stream<> _changes;
	rpl::event_stream<> _closeRequests;
	rpl::lifetime _lifetime;

};

SessionPlaylistBackend::SessionPlaylistBackend(
	not_null<Main::Session*> session,
	std::shared_ptr<Ui::Show> show,
	const QString &id)
: _session(base::make_weak(session))
, _service(base::make_weak(&ServiceFor(session)))
, _show(std::move(show))
, _id(id)
, _sender(&Cloud::For(session)) {
	const auto service = &ServiceFor(session);
	if (const auto kept = service->store().findSaved(id)) {
		_playlist = *kept;
	}
	service->updates(
	) | rpl::filter([=](const Playlist &playlist) {
		return (playlist.id == _id);
	}) | rpl::on_next([=](const Playlist &playlist) {
		_playlist = playlist;
		_error = QString();
		_gone = false;
		_keptTracks = -1;
		_changes.fire({});
	}, _lifetime);
	service->keptChanges() | rpl::on_next([=] {
		_keptTracks = -1;
		_changes.fire({});
	}, _lifetime);
	service->playingChanges() | rpl::on_next([=] {
		_changes.fire({});
	}, _lifetime);

	Cloud::For(session).events(
	) | rpl::filter([=](const Cloud::Event &event) {
		return (event.data.value(u"id"_q).toString() == _id)
			&& (event.type == u"playlist.updated"_q
				|| event.type == u"playlist.deleted"_q);
	}) | rpl::on_next([=](const Cloud::Event &event) {
		if (event.type == u"playlist.deleted"_q) {
			_error = tr::lng_oblivion_share_error_gone(tr::now);
			_gone = true;
			_changes.fire({});
		} else if (Cloud::JsonInt(event.data.value(u"rev"_q))
			> _playlist.rev) {
			reload();
		}
	}, _lifetime);

	// The account was switched off while the playlist was on its way:
	// the core drops the request without calling back.
	Cloud::For(session).readyValue(
	) | rpl::filter([=](bool ready) {
		return !ready && _loading;
	}) | rpl::on_next([=] {
		_sender.cancelAll();
		fail({ .type = Cloud::Error::Type::NotConnected });
	}, _lifetime);
	reload();
}

PlaylistBackend::State SessionPlaylistBackend::state() {
	auto result = State{
		.playlist = _playlist,
		.loading = _loading,
		.error = _error,
		.gone = _gone,
	};
	if (const auto service = _service.get()) {
		result.selfId = service->account().userId();
		result.kept = service->kept(_id);
		result.keeping = service->keeping(_id);
		if (!result.kept) {
			_keptTracks = 0;
		} else if (_keptTracks < 0) {
			_keptTracks = service->keptTracks(_playlist);
		}
		result.keptTracks = _keptTracks;
	}
	return result;
}

Playing SessionPlaylistBackend::playing() {
	const auto service = _service.get();
	return service ? service->playing() : Playing();
}

rpl::producer<> SessionPlaylistBackend::changes() {
	return _changes.events();
}

rpl::producer<> SessionPlaylistBackend::closeRequests() {
	return _closeRequests.events();
}

void SessionPlaylistBackend::apply(const Cloud::Response &response) {
	const auto playlist = ParsePlaylist(
		response.json.value(u"playlist"_q).toObject());
	_loading = false;
	if (!playlist.valid()) {
		fail({ .type = Cloud::Error::Type::Protocol });
		return;
	}
	_error = QString();
	_gone = false;
	_playlist = playlist;
	if (const auto service = _service.get()) {
		// Fires the update this object listens to as well.
		service->remember(playlist);
	} else {
		_changes.fire({});
	}
}

void SessionPlaylistBackend::fail(const Cloud::Error &error) {
	_loading = false;
	_gone = (error.status == 404);
	_error = _gone
		? tr::lng_oblivion_share_error_gone(tr::now)
		: Cloud::ErrorText(error);
	_changes.fire({});
}

void SessionPlaylistBackend::reload() {
	if (_loading) {
		return;
	}
	_loading = true;
	_error = QString();
	_gone = false;
	_changes.fire({});
	_sender.request(
		Cloud::GetRequest(u"/v1/playlists/"_q + _id),
		crl::guard(this, [=](const Cloud::Response &response) {
			apply(response);
		}),
		crl::guard(this, [=](const Cloud::Error &error) {
			fail(error);
		}));
}

void SessionPlaylistBackend::play(int index) {
	if (const auto service = _service.get()) {
		service->play(_playlist, index);
	}
}

void SessionPlaylistBackend::toggleKept() {
	const auto service = _service.get();
	if (!service || !_playlist.valid() || !_playlist.full) {
		return;
	} else if (service->kept(_id)) {
		service->forget(_playlist);
		Toast(_show, tr::lng_oblivion_share_view_unkept(tr::now));
	} else {
		service->keep(_playlist);
		Toast(_show, tr::lng_oblivion_share_view_kept(tr::now));
	}
}

void SessionPlaylistBackend::setCollab(bool value) {
	auto patch = QJsonObject();
	patch.insert(u"collab"_q, value);
	_sender.request(
		Cloud::PatchRequest(u"/v1/playlists/"_q + _id, patch),
		crl::guard(this, [=](const Cloud::Response &response) {
			apply(response);
		}),
		crl::guard(this, [=](const Cloud::Error &error) {
			Cloud::ShowError(_show, error);
			_changes.fire({});
		}));
}

void SessionPlaylistBackend::removeTrack(const QString &trackId) {
	_sender.request(
		Cloud::DeleteRequest(
			u"/v1/playlists/"_q + _id + u"/tracks/"_q + trackId),
		crl::guard(this, [=](const Cloud::Response &response) {
			apply(response);
		}),
		crl::guard(this, [=](const Cloud::Error &error) {
			Cloud::ShowError(_show, error);
		}));
}

void SessionPlaylistBackend::addFiles() {
	const auto weak = _session;
	const auto show = _show;
	const auto id = _id;
	const auto title = _playlist.title;
	const auto filter = u"Audio (*.mp3 *.m4a *.aac *.flac *.ogg *.opus *.wav)"
		";;"_q + FileDialog::AllFilesFilter();
	FileDialog::GetOpenPaths(
		Core::App().getFileDialogParent(),
		tr::lng_oblivion_share_choose_files(tr::now),
		filter,
		[=](FileDialog::OpenResult &&result) {
			const auto session = weak.get();
			if (!session || result.paths.isEmpty()) {
				return;
			}
			auto request = UploadRequest{
				.title = title,
				.playlistId = id,
			};
			for (const auto &path : result.paths) {
				if (int(request.sources.size()) >= 100) {
					break;
				}
				request.sources.push_back({ .path = path });
			}
			StartUpload(session, show, std::move(request));
		});
}

void SessionPlaylistBackend::remove() {
	if (!_show || !_show->valid()) {
		return;
	}
	const auto weak = base::make_weak(this);
	_show->showBox(Ui::MakeConfirmBox({
		.text = tr::lng_oblivion_share_view_delete_sure(
			lt_name,
			rpl::single(_playlist.title)),
		.confirmed = [=](Fn<void()> close) {
			close();
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			strong->_sender.request(
				Cloud::DeleteRequest(u"/v1/playlists/"_q + strong->_id),
				crl::guard(strong, [=](const Cloud::Response &response) {
					if (const auto service = strong->_service.get()) {
						service->store().forgetPublished(strong->_id);
						service->forget(strong->_playlist);
					}
					Toast(
						strong->_show,
						tr::lng_oblivion_share_view_deleted(tr::now));
					strong->_closeRequests.fire({});
				}),
				crl::guard(strong, [=](const Cloud::Error &error) {
					Cloud::ShowError(strong->_show, error);
				}));
		},
		.confirmText = tr::lng_box_delete(),
		.confirmStyle = &st::attentionBoxButton,
	}));
}

void ShowPlaylist(
		not_null<Main::Session*> session,
		std::shared_ptr<Ui::Show> show,
		const QString &id) {
	if (!Cloud::ValidShareId(id) || !show || !show->valid()) {
		return;
	}
	show->showBox(Box(
		PlaylistBox,
		std::make_shared<SessionPlaylistBackend>(session, show, id)));
}

void ConfirmShare(
		not_null<Main::Session*> session,
		std::shared_ptr<Ui::Show> show,
		UploadRequest request) {
	const auto weak = base::make_weak(session);
	const auto count = int(request.sources.size());
	const auto fresh = request.playlistId.isEmpty();
	const auto name = tr::bold(request.title);
	const auto tracks = TextWithEntities{
		tr::lng_oblivion_playlists_tracks_count(tr::now, lt_count, count),
	};
	show->showBox(Ui::MakeConfirmBox({
		.text = fresh
			? tr::lng_oblivion_share_playlist_sure(
				tr::now,
				lt_name,
				name,
				lt_tracks,
				tracks,
				tr::marked)
			: tr::lng_oblivion_share_playlist_new_sure(
				tr::now,
				lt_name,
				name,
				lt_tracks,
				tracks,
				tr::marked),
		.confirmed = [=](Fn<void()> close) {
			close();
			if (const auto strong = weak.get()) {
				auto copy = request;
				StartUpload(strong, show, std::move(copy));
			}
		},
		.confirmText = tr::lng_oblivion_share_playlist_upload(),
	}));
}

void ShareResolved(
		not_null<Main::Session*> session,
		std::shared_ptr<Ui::Show> show,
		uint64 localId,
		const QString &name,
		std::vector<LocalTrack> tracks) {
	const auto service = &ServiceFor(session);
	const auto current = service->uploader();
	if (current && !current->status().finished()) {
		Toast(show, tr::lng_oblivion_share_upload_busy(tr::now));
		ShowUploadBox(session, show);
		return;
	}
	const auto sources = [=](Fn<bool(const LocalTrack&)> wanted) {
		auto result = std::vector<UploadSource>();
		for (const auto &track : tracks) {
			if (int(result.size()) >= kMaxPlaylistTracks) {
				break;
			} else if (!wanted || wanted(track)) {
				result.push_back({
					.item = FullMsgId(PeerId(track.peer), MsgId(track.msg)),
					.document = track.document,
					.title = track.title,
					.performer = track.performer,
					.fileName = track.fileName,
					.duration = int64(track.duration) * 1000,
				});
			}
		}
		return result;
	};
	const auto startFresh = [=] {
		ConfirmShare(session, show, {
			.localId = localId,
			.title = name,
			.sources = sources(nullptr),
		});
	};
	const auto published = service->store().publishedId(localId);
	if (published.isEmpty()) {
		startFresh();
		return;
	}
	// Shared before: what is new since then?
	const auto weak = base::make_weak(session);
	Request(
		weak,
		Cloud::GetRequest(u"/v1/playlists/"_q + published),
		[=](const Cloud::Response &response) {
			const auto strong = weak.get();
			const auto playlist = ParsePlaylist(
				response.json.value(u"playlist"_q).toObject());
			if (!strong || !playlist.valid()) {
				return;
			}
			const auto service = &ServiceFor(strong);
			service->remember(playlist);
			auto has = base::flat_set<QString>();
			for (const auto &track : playlist.tracks) {
				has.emplace(track.media);
			}
			auto fresh = sources([&](const LocalTrack &track) {
				const auto known = service->store().knownMedia(
					track.document);
				return known.isEmpty() || !has.contains(known);
			});
			if (fresh.empty() || !playlist.canAdd) {
				ShowPlaylist(strong, show, playlist.id);
				return;
			}
			ConfirmShare(strong, show, {
				.localId = localId,
				.title = playlist.title,
				.playlistId = playlist.id,
				.sources = std::move(fresh),
			});
		},
		[=](const Cloud::Error &error) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			} else if (error.status == 404) {
				// Deleted from the server since: shared anew.
				ServiceFor(strong).store().forgetPublished(published);
				startFresh();
			} else {
				Cloud::ShowError(show, error);
			}
		});
}

// ---- Presets with a session. apply is CheckedApply() of the editor:
// true when the editor has taken the stack.

void ShowPresetWith(
	base::weak_ptr<Main::Session> weak,
	std::shared_ptr<Ui::Show> show,
	Preset preset,
	Fn<bool(const QByteArray &stack)> apply);

void CountUse(const base::weak_ptr<Main::Session> &weak, const QString &id) {
	Request(
		weak,
		Cloud::PostRequest(u"/v1/presets/"_q + id + u"/use"_q),
		[](const Cloud::Response &response) {},
		[](const Cloud::Error &error) {});
}

void ShowGalleryWith(
		base::weak_ptr<Main::Session> weak,
		std::shared_ptr<Ui::Show> show,
		const QString &kind,
		Fn<bool(const QByteArray &stack)> apply) {
	if (!show || !show->valid()) {
		return;
	}
	show->showBox(Box(GalleryBox, GalleryArgs{
		.kind = kind,
		.load = [=](
				const QString &kind,
				const QString &sort,
				const QString &cursor,
				Fn<void(GalleryPage)> done,
				Cloud::Fail fail) {
			auto query = Cloud::Query{
				{ u"kind"_q, kind },
				{ u"sort"_q, sort },
				{ u"limit"_q, QString::number(kGalleryPage) },
			};
			if (!cursor.isEmpty()) {
				query.push_back({ u"cursor"_q, cursor });
			}
			Request(
				weak,
				Cloud::GetRequest(u"/v1/presets/gallery"_q, std::move(query)),
				[=](const Cloud::Response &response) {
					auto page = GalleryPage();
					const auto list = response.json.value(
						u"presets"_q).toArray();
					for (const auto &value : list) {
						auto preset = ParsePreset(value.toObject());
						if (preset.valid()
							&& int(page.presets.size()) < 2 * kGalleryPage) {
							page.presets.push_back(std::move(preset));
						}
					}
					page.next = Cloud::JsonText(
						response.json.value(u"next_cursor"_q),
						32);
					done(std::move(page));
				},
				std::move(fail));
		},
		.open = [=](const Preset &summary) {
			ShowPresetWith(weak, show, summary, apply);
		},
	}));
}

void ShowPresetWith(
		base::weak_ptr<Main::Session> weak,
		std::shared_ptr<Ui::Show> show,
		Preset preset,
		Fn<bool(const QByteArray &stack)> apply) {
	if (!preset.valid() || !show || !show->valid()) {
		return;
	}
	const auto id = preset.id;
	show->showBox(Box(PresetBox, PresetBoxArgs{
		.preset = std::move(preset),
		.load = [=](Fn<void(Preset)> done, Cloud::Fail fail) {
			Request(
				weak,
				Cloud::GetRequest(u"/v1/presets/"_q + id),
				[=](const Cloud::Response &response) {
					auto parsed = ParsePreset(
						response.json.value(u"preset"_q).toObject());
					if (parsed.valid()) {
						done(std::move(parsed));
					} else if (fail) {
						fail({ .type = Cloud::Error::Type::Protocol });
					}
				},
				std::move(fail));
		},
		.apply = apply
			? [=](const Preset &preset) {
				// The editor may refuse the stack (it says why itself):
				// no «Набор применён» over that and no use is counted.
				if (apply(preset.stack)) {
					Toast(
						show,
						tr::lng_oblivion_share_preset_applied(tr::now));
					CountUse(weak, preset.id);
				}
			}
			: Fn<void(const Preset&)>(),
		.save = [=](const Preset &preset) {
			const auto saved = PresetLibrary().add({
				.kind = preset.kind,
				.title = preset.title,
				.stack = preset.stack,
				.cloudId = preset.id,
			});
			Toast(show, saved
				? tr::lng_oblivion_share_preset_saved(tr::now)
				: tr::lng_oblivion_share_preset_limit(tr::now));
			if (saved) {
				CountUse(weak, preset.id);
			}
		},
		.saved = [](const Preset &preset) {
			return PresetLibrary().findByCloudId(preset.id) != nullptr;
		},
		.report = [=](const Preset &preset) {
			auto body = QJsonObject();
			body.insert(u"reason"_q, QString());
			Request(
				weak,
				Cloud::PostRequest(
					u"/v1/presets/"_q + preset.id + u"/report"_q,
					body),
				[=](const Cloud::Response &response) {
					Toast(
						show,
						tr::lng_oblivion_share_preset_reported(tr::now));
				},
				[=](const Cloud::Error &error) {
					Cloud::ShowError(show, error);
				});
		},
		.remove = [=](const Preset &preset, Fn<void()> done) {
			const auto cloudId = preset.id;
			Request(
				weak,
				Cloud::DeleteRequest(u"/v1/presets/"_q + cloudId),
				[=](const Cloud::Response &response) {
					Toast(
						show,
						tr::lng_oblivion_share_preset_deleted(tr::now));
					if (done) {
						done();
					}
				},
				[=](const Cloud::Error &error) {
					Cloud::ShowError(show, error);
				});
		},
	}));
}

void SharePresetWith(
		not_null<Main::Session*> session,
		std::shared_ptr<Ui::Show> show,
		const QString &kind,
		const QString &title,
		const QByteArray &stack,
		uint64 localId) {
	if (!show || !show->valid()) {
		return;
	}
	const auto weak = base::make_weak(session);
	const auto tags = StackEffects(kind, stack);
	show->showBox(Box(SharePresetBox, SharePresetArgs{
		.kind = kind,
		.title = title,
		.effects = EffectNames(kind, tags),
		.submit = [=](
				const QString &title,
				const QString &text,
				bool listed,
				Fn<void(const QString &link)> done,
				Cloud::Fail fail) {
			auto body = QJsonObject();
			body.insert(u"kind"_q, kind);
			body.insert(u"title"_q, title.left(kMaxPresetTitle));
			body.insert(u"description"_q, ComposeDescription(text, tags));
			body.insert(u"public"_q, listed);
			body.insert(u"app_build"_q, Cloud::kAppBuild);
			body.insert(u"data"_q, PresetData(kind, stack));
			Request(
				weak,
				Cloud::PostRequest(u"/v1/presets"_q, body),
				[=](const Cloud::Response &response) {
					const auto preset = ParsePreset(
						response.json.value(u"preset"_q).toObject());
					if (!preset.valid()) {
						if (fail) {
							fail({ .type = Cloud::Error::Type::Protocol });
						}
						return;
					}
					if (localId) {
						PresetLibrary().setCloudId(localId, preset.id);
					}
					done(preset.link);
				},
				std::move(fail));
		},
	}));
}

// The cloud parts of the menu of an editor: the account of the active
// window, after its consent.
void WithAccount(
		std::shared_ptr<Ui::Show> show,
		Fn<void(not_null<Main::Session*> session)> done) {
	const auto session = ActiveSession();
	if (!session) {
		Toast(show, tr::lng_oblivion_share_no_account(tr::now));
		return;
	}
	const auto weak = base::make_weak(session);
	Cloud::RequireConsent(session, show, [=] {
		if (const auto strong = weak.get()) {
			done(strong);
		}
	});
}

void ShowMyPresets(PresetHost host) {
	const auto show = host.show;
	const auto kind = host.kind;
	const auto apply = CheckedApply(host);
	show->showBox(Box(MyPresetsBox, MyPresetsArgs{
		.kind = kind,
		.list = [] { return PresetLibrary().list(); },
		.changes = PresetLibrary().changes(),
		.apply = apply
			? [=](const LocalPreset &preset) {
				if (apply(preset.stack)) {
					Toast(
						show,
						tr::lng_oblivion_share_preset_applied(tr::now));
				}
			}
			: Fn<void(const LocalPreset&)>(),
		.rename = [](uint64 id, const QString &title) {
			PresetLibrary().rename(id, title);
		},
		.remove = [](uint64 id) {
			PresetLibrary().remove(id);
		},
		.share = [=](const LocalPreset &preset) {
			WithAccount(show, [=](not_null<Main::Session*> session) {
				SharePresetWith(
					session,
					show,
					preset.kind,
					preset.title,
					preset.stack,
					preset.id);
			});
		},
		.gallery = [=] {
			WithAccount(show, [=](not_null<Main::Session*> session) {
				ShowGalleryWith(base::make_weak(session), show, kind, apply);
			});
		},
	}));
}

// ---- Snapshot scenes (OBLIVION_SELFTEST=ui): sample data, no session
// and no network.

[[nodiscard]] QString Sample(const char *ru, const char *en) {
	return QString::fromUtf8(CurrentLanguageIsRussian() ? ru : en);
}

[[nodiscard]] Playlist SamplePlaylist(bool owner) {
	struct Entry {
		const char *title;
		const char *performer;
		int seconds;
	};
	const auto entries = std::vector<Entry>{
		{ "Midnight City", "M83", 243 },
		{ "Instant Crush", "Daft Punk", 337 },
		{ "The Less I Know the Better", "Tame Impala", 216 },
		{ "Do I Wanna Know?", "Arctic Monkeys", 272 },
		{ "Nightcall", "Kavinsky", 258 },
		{ "Resonance", "Home", 212 },
		{ "Sunset Lover", "Petit Biscuit", 237 },
	};
	auto result = Playlist();
	result.id = owner ? u"mK3abcdefghijklmnopq22"_q : u"Qw9abcdefghijklmnopq77"_q;
	result.link = Cloud::MakeLink(Cloud::LinkKind::Playlist, result.id);
	result.ownerId = owner ? 1001 : 1002;
	result.ownerName = Sample("Миша", "Misha");
	result.title = Sample("Ночная дорога", "Night drive");
	result.description = owner
		? QString()
		: Sample(
			"Для поздних поездок и долгих разговоров.",
			"For late rides and long talks.");
	result.collab = owner;
	result.canEdit = owner;
	result.canAdd = owner;
	result.followers = owner ? 4 : 12;
	result.rev = 7;
	result.full = true;
	auto index = 0;
	for (const auto &entry : entries) {
		auto track = Track();
		track.id = u"t%1"_q.arg(++index);
		track.media = QString(64, QChar('a' + (index % 6)));
		track.size = 9'000'000;
		track.mime = u"audio/mpeg"_q;
		track.title = QString::fromUtf8(entry.title);
		track.performer = QString::fromUtf8(entry.performer);
		track.duration = entry.seconds * int64(1000);
		track.addedBy = result.ownerId;
		result.totalDuration += track.duration;
		result.totalBytes += track.size;
		result.tracks.push_back(std::move(track));
	}
	result.trackCount = int(result.tracks.size());
	return result;
}

class SamplePlaylistBackend final : public PlaylistBackend {
public:
	SamplePlaylistBackend(State state, Playing playing)
	: _state(std::move(state))
	, _playing(std::move(playing)) {
	}

	State state() override {
		return _state;
	}
	Playing playing() override {
		return _playing;
	}
	rpl::producer<> changes() override {
		return rpl::never<>();
	}
	rpl::producer<> closeRequests() override {
		return rpl::never<>();
	}
	void reload() override {
	}
	void play(int index) override {
	}
	void toggleKept() override {
	}
	void setCollab(bool value) override {
	}
	void removeTrack(const QString &trackId) override {
	}
	void addFiles() override {
	}
	void remove() override {
	}

private:
	const State _state;
	const Playing _playing;

};

// A few different stacks: a list must not show the same line of effects
// in every row.
[[nodiscard]] QByteArray SampleVideoStack(int variant = 0) {
	const auto join = [](std::initializer_list<const char*> ids) -> QByteArray {
		auto result = QByteArray("[");
		for (const auto id : ids) {
			if (result.size() > 1) {
				result.append(',');
			}
			result.append("{\"fx\":\"");
			result.append(id);
			result.append("\",\"on\":true,\"mix\":1,\"p\":{}}");
		}
		result.append(']');
		return result;
	};
	switch (variant) {
	case 1: return join({ "rgbsplit", "crt" });
	case 2: return join({ "glitch", "datamosh" });
	case 3: return join({ "grain" });
	}
	return join({ "glitch", "crt", "grain" });
}

[[nodiscard]] std::vector<Preset> SamplePresets(const QString &kind) {
	struct Entry {
		const char *ru;
		const char *en;
		const char *ownerRu;
		const char *owner;
		int uses;
		const char *textRu;
		const char *textEn;
	};
	const auto entries = std::vector<Entry>{
		{
			"Кассета 1998",
			"Tape 1998",
			"Миша",
			"Misha",
			128,
			"Тёплый шум и полосы, как на старой плёнке.",
			"Warm noise and lines of an old tape.",
		},
		{
			"Неоновый сон",
			"Neon dream",
			"Аня",
			"Anna",
			64,
			"Цвета разъезжаются, как на вывеске ночью.",
			"The colours drift apart like a sign at night.",
		},
		{
			"Сломанный телевизор",
			"Broken TV",
			"Дима",
			"Dima",
			17,
			"Осторожно: мерцает.",
			"Careful: it flickers.",
		},
		{ "Просто зерно", "Just grain", "Лена", "Lena", 0, "", "" },
	};
	auto result = std::vector<Preset>();
	auto index = 0;
	for (const auto &entry : entries) {
		const auto stack = SanitizeStack(
			u"video"_q,
			SampleVideoStack(index));
		auto preset = Preset();
		preset.id = u"Xy%1abcdefghijklmnopq2"_q.arg(index);
		preset.link = Cloud::MakeLink(Cloud::LinkKind::Preset, preset.id);
		preset.kind = u"video"_q;
		preset.title = Sample(entry.ru, entry.en);
		preset.text = Sample(entry.textRu, entry.textEn);
		preset.tags = StackEffects(u"video"_q, stack);
		preset.ownerId = 2000 + index;
		preset.ownerName = Sample(entry.ownerRu, entry.owner);
		preset.listed = true;
		preset.uses = entry.uses;
		preset.full = true;
		preset.stack = stack;
		result.push_back(std::move(preset));
		++index;
	}
	return result;
}

[[nodiscard]] std::vector<LocalPreset> SampleLocalPresets() {
	auto result = std::vector<LocalPreset>();
	const auto add = [&](const char *ru, const char *en, int variant) {
		result.push_back({
			.id = uint64(result.size() + 1),
			.kind = u"video"_q,
			.title = Sample(ru, en),
			.stack = SanitizeStack(u"video"_q, SampleVideoStack(variant)),
		});
	};
	add("Кассета 1998", "Tape 1998", 0);
	add("Для сторис", "For stories", 1);
	add("Сломанный телевизор", "Broken TV", 2);
	return result;
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	const auto size = QSize(st::boxWideWidth * 2, 0);
	const auto upload = [=](const QString &name, UploadStatus status) {
		RegisterBoxScene(name, size, [=](std::shared_ptr<Ui::Show> show) {
			auto copy = status;
			copy.title = Sample("Ночная дорога", "Night drive");
			return Box(UploadBox, UploadBoxArgs{
				.status = rpl::single(copy),
				.errorText = [](const Cloud::Error &error) {
					return UploadErrorText(
						error,
						int64(3) << 30,
						int64(6) << 30,
						500,
						50);
				},
			});
		});
	};
	using Stage = UploadStatus::Stage;
	upload(u"share_upload_fetching"_q, {
		.stage = Stage::Fetching,
		.index = 2,
		.count = 12,
		.track = QString::fromUtf8("Tame Impala \xE2\x80\x94 Let It Happen"),
		.part = 0.4,
		.added = 2,
	});
	upload(u"share_upload_sending"_q, {
		.stage = Stage::Sending,
		.index = 7,
		.count = 12,
		.track = QString::fromUtf8("Kavinsky \xE2\x80\x94 Nightcall"),
		.part = 0.73,
		.added = 6,
		.skipped = 1,
		.playlistId = u"mK3abcdefghijklmnopq22"_q,
		.created = true,
	});
	upload(u"share_upload_done"_q, {
		.stage = Stage::Done,
		.index = 12,
		.count = 12,
		.added = 11,
		.skipped = 1,
		.playlistId = u"mK3abcdefghijklmnopq22"_q,
		.created = true,
	});
	upload(u"share_upload_quota"_q, {
		.stage = Stage::Failed,
		.index = 9,
		.count = 12,
		.track = QString::fromUtf8("Home \xE2\x80\x94 Resonance"),
		.added = 9,
		.error = {
			.type = Cloud::Error::Type::Http,
			.status = 422,
			.code = u"limit_reached"_q,
			.details = QJsonObject{ { u"limit"_q, u"playlist_quota"_q } },
		},
		.playlistId = u"mK3abcdefghijklmnopq22"_q,
		.created = true,
	});
	upload(u"share_upload_offline"_q, {
		.stage = Stage::Failed,
		.index = 0,
		.count = 12,
		.error = { .type = Cloud::Error::Type::Network },
	});
	RegisterBoxScene(u"share_playlist_sure"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Ui::MakeConfirmBox({
			.text = tr::lng_oblivion_share_playlist_sure(
				tr::now,
				lt_name,
				tr::bold(Sample("Ночная дорога", "Night drive")),
				lt_tracks,
				TextWithEntities{ tr::lng_oblivion_playlists_tracks_count(
					tr::now,
					lt_count,
					12) },
				tr::marked),
			.confirmText = tr::lng_oblivion_share_playlist_upload(),
		});
	});

	const auto view = [=](
			const QString &name,
			PlaylistBackend::State state,
			Playing playing) {
		RegisterBoxScene(name, size, [=](std::shared_ptr<Ui::Show> show) {
			return Box(
				PlaylistBox,
				std::make_shared<SamplePlaylistBackend>(state, playing));
		});
	};
	const auto guest = SamplePlaylist(false);
	const auto own = SamplePlaylist(true);
	view(u"share_playlist_view"_q, { .playlist = guest }, {
		.playlistId = guest.id,
		.media = guest.tracks[1].media,
		.index = 1,
	});
	view(u"share_playlist_loading_track"_q, {
		.playlist = guest,
		.kept = true,
		.keeping = true,
		.keptTracks = 3,
	}, {
		.playlistId = guest.id,
		.media = guest.tracks[2].media,
		.index = 2,
		.loading = true,
		.progress = 0.4,
	});
	view(u"share_playlist_owner"_q, {
		.playlist = own,
		.kept = true,
		.keptTracks = int(own.tracks.size()),
	}, {});
	view(u"share_playlist_loading"_q, { .loading = true }, {});
	view(u"share_playlist_gone"_q, {
		.error = tr::lng_oblivion_share_error_gone(tr::now),
		.gone = true,
	}, {});
	view(u"share_playlist_offline"_q, {
		.error = tr::lng_oblivion_cloud_error_network(tr::now),
	}, {});

	RegisterBoxScene(u"share_library"_q, size, [=](
			std::shared_ptr<Ui::Show> show) {
		// Summaries, as the list of the server gives them: every playlist
		// with its own name, length and author.
		const auto summary = [&](
				const QString &id,
				const QString &title,
				const QString &owner,
				int tracks,
				int seconds) {
			auto result = guest;
			result.id = id;
			result.title = title;
			result.ownerName = owner;
			result.full = false;
			result.tracks.clear();
			result.trackCount = tracks;
			result.totalDuration = seconds * int64(1000);
			return result;
		};
		return Box(LibraryBox, LibraryArgs{
			.kept = {
				own,
				summary(
					u"Qw9abcdefghijklmnopq77"_q,
					Sample("Синтвейв на вечер", "Evening synthwave"),
					Sample("Миша", "Misha"),
					24,
					5832),
				summary(
					u"Zz1abcdefghijklmnopq55"_q,
					Sample("Утро без кофе", "Morning without coffee"),
					Sample("Аня", "Anna"),
					12,
					2768),
			},
		});
	});
	RegisterBoxScene(u"share_library_empty"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(LibraryBox, LibraryArgs{});
	});

	const auto gallery = [=](
			const QString &name,
			const QString &kind,
			Fn<void(Fn<void(GalleryPage)>, Cloud::Fail)> answer) {
		RegisterBoxScene(name, size, [=](std::shared_ptr<Ui::Show> show) {
			return Box(GalleryBox, GalleryArgs{
				.kind = kind,
				.load = [=](
						const QString &kind,
						const QString &sort,
						const QString &cursor,
						Fn<void(GalleryPage)> done,
						Cloud::Fail fail) {
					answer(done, fail);
				},
			});
		});
	};
	gallery(u"share_gallery"_q, QString(), [](
			Fn<void(GalleryPage)> done,
			Cloud::Fail fail) {
		done({ .presets = SamplePresets(u"video"_q), .next = u"30"_q });
	});
	gallery(u"share_gallery_video"_q, u"video"_q, [](
			Fn<void(GalleryPage)> done,
			Cloud::Fail fail) {
		done({ .presets = SamplePresets(u"video"_q) });
	});
	gallery(u"share_gallery_empty"_q, QString(), [](
			Fn<void(GalleryPage)> done,
			Cloud::Fail fail) {
		done({});
	});
	gallery(u"share_gallery_loading"_q, QString(), [](
			Fn<void(GalleryPage)> done,
			Cloud::Fail fail) {
	});
	gallery(u"share_gallery_error"_q, QString(), [](
			Fn<void(GalleryPage)> done,
			Cloud::Fail fail) {
		fail({ .type = Cloud::Error::Type::Network });
	});

	RegisterBoxScene(u"share_preset"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(PresetBox, PresetBoxArgs{
			.preset = SamplePresets(u"video"_q).front(),
			.apply = [](const Preset &preset) {},
			.save = [](const Preset &preset) {},
		});
	});
	// Opened by a link in a chat: no editor to apply it in, it can be
	// saved, and after that the box tells where to find it.
	RegisterBoxScene(u"share_preset_from_link"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		auto preset = SamplePresets(u"video"_q)[1];
		return Box(PresetBox, PresetBoxArgs{
			.preset = std::move(preset),
			.save = [](const Preset &preset) {},
			.saved = [](const Preset &preset) { return false; },
		});
	});
	RegisterBoxScene(u"share_preset_saved"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		auto preset = SamplePresets(u"video"_q)[1];
		return Box(PresetBox, PresetBoxArgs{
			.preset = std::move(preset),
			.save = [](const Preset &preset) {},
			.saved = [](const Preset &preset) { return true; },
		});
	});
	RegisterBoxScene(u"share_preset_too_new"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		auto preset = SamplePresets(u"video"_q)[2];
		preset.stack = QByteArray();
		preset.tags = QStringList();
		return Box(PresetBox, PresetBoxArgs{
			.preset = std::move(preset),
			.save = [](const Preset &preset) {},
		});
	});
	RegisterBoxScene(u"share_presets_mine"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(MyPresetsBox, MyPresetsArgs{
			.kind = u"video"_q,
			.list = [] { return SampleLocalPresets(); },
			.apply = [](const LocalPreset &preset) {},
			.gallery = [] {},
		});
	});
	RegisterBoxScene(u"share_presets_mine_empty"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(MyPresetsBox, MyPresetsArgs{
			.gallery = [] {},
		});
	});
	RegisterBoxScene(u"share_preset_save"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		const auto stack = SanitizeStack(u"video"_q, SampleVideoStack());
		return Box(
			SaveNameBox,
			tr::lng_oblivion_share_preset_save_title(),
			Sample("Кассета 1998", "Tape 1998"),
			EffectNames(u"video"_q, StackEffects(u"video"_q, stack)),
			tr::lng_settings_save(),
			[](QString) {});
	});
	RegisterBoxScene(u"share_preset_publish"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		const auto stack = SanitizeStack(u"video"_q, SampleVideoStack());
		return Box(SharePresetBox, SharePresetArgs{
			.kind = u"video"_q,
			.title = Sample("Кассета 1998", "Tape 1998"),
			.effects = EffectNames(
				u"video"_q,
				StackEffects(u"video"_q, stack)),
		});
	});
	RegisterBoxScene(u"share_preset_published"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(
			LinkBox,
			tr::lng_oblivion_share_preset_published_title(),
			tr::lng_oblivion_share_preset_published_about(),
			Cloud::MakeLink(
				Cloud::LinkKind::Preset,
				u"Xy7abcdefghijklmnopq22"_q));
	});
});

} // namespace

void OpenPlaylistLink(
		not_null<Window::SessionController*> controller,
		const QString &id) {
	if (!Cloud::ValidShareId(id)) {
		return;
	}
	Cloud::RequireConsent(controller, crl::guard(controller, [=] {
		ShowPlaylist(&controller->session(), controller->uiShow(), id);
	}));
}

void OpenPresetLink(
		not_null<Window::SessionController*> controller,
		const QString &id) {
	if (!Cloud::ValidShareId(id)) {
		return;
	}
	Cloud::RequireConsent(controller, crl::guard(controller, [=] {
		const auto session = &controller->session();
		const auto show = controller->uiShow();
		const auto weak = base::make_weak(session);
		Request(
			weak,
			Cloud::GetRequest(u"/v1/presets/"_q + id),
			[=](const Cloud::Response &response) {
				auto preset = ParsePreset(
					response.json.value(u"preset"_q).toObject());
				if (preset.valid()) {
					ShowPresetWith(weak, show, std::move(preset), nullptr);
				} else {
					Cloud::ShowError(
						show,
						{ .type = Cloud::Error::Type::Protocol });
				}
			},
			[=](const Cloud::Error &error) {
				Cloud::ShowError(show, error);
			});
	}));
}

void ShowGallery(
		not_null<Window::SessionController*> controller,
		const QString &kind) {
	const auto fixed = ValidKind(kind) ? kind : QString();
	Cloud::RequireConsent(controller, crl::guard(controller, [=] {
		ShowGalleryWith(
			base::make_weak(&controller->session()),
			controller->uiShow(),
			fixed,
			nullptr);
	}));
}

void ShowLibrary(not_null<Window::SessionController*> controller) {
	Cloud::RequireConsent(controller, crl::guard(controller, [=] {
		const auto session = &controller->session();
		const auto show = controller->uiShow();
		const auto weak = base::make_weak(session);
		const auto service = &ServiceFor(session);
		const auto shown = [=](const std::vector<Playlist> &list) {
			// What the user has published and what was added by a click:
			// a playlist that was only opened once is not in the list.
			auto result = std::vector<Playlist>();
			const auto strong = weak.get();
			for (const auto &playlist : list) {
				if (playlist.canEdit
					|| (strong && ServiceFor(strong).kept(playlist.id))) {
					result.push_back(playlist);
				}
			}
			return result;
		};
		controller->show(Box(LibraryBox, LibraryArgs{
			.kept = service->store().saved(),
			.load = [=](
					Fn<void(std::vector<Playlist>)> done,
					Cloud::Fail fail) {
				Request(
					weak,
					Cloud::GetRequest(u"/v1/playlists"_q),
					[=](const Cloud::Response &response) {
						auto list = std::vector<Playlist>();
						const auto array = response.json.value(
							u"playlists"_q).toArray();
						for (const auto &value : array) {
							auto playlist = ParsePlaylist(value.toObject());
							if (playlist.valid() && list.size() < 500) {
								list.push_back(std::move(playlist));
							}
						}
						done(shown(list));
					},
					std::move(fail));
			},
			.reloads = rpl::merge(
				service->store().changes(),
				service->updates() | rpl::to_empty),
			.open = [=](const Playlist &playlist) {
				if (const auto strong = weak.get()) {
					ShowPlaylist(strong, show, playlist.id);
				}
			},
		}));
	}));
}

void SharePlaylist(
		not_null<Window::SessionController*> controller,
		uint64 localId,
		const QString &name,
		std::vector<LocalTrack> tracks) {
	if (tracks.empty()) {
		controller->showToast(tr::lng_oblivion_share_playlist_empty(tr::now));
		return;
	}
	Cloud::RequireConsent(controller, crl::guard(controller, [=] {
		ShareResolved(
			&controller->session(),
			controller->uiShow(),
			localId,
			name,
			tracks);
	}));
}

void FillPresetsMenu(not_null<Ui::PopupMenu*> menu, PresetHost host) {
	if (!ValidKind(host.kind) || !host.show) {
		return;
	}
	if (!menu->empty()) {
		menu->addSeparator();
	}
	const auto show = host.show;
	const auto kind = host.kind;
	const auto current = [=] {
		const auto stack = host.current
			? SanitizeStack(kind, host.current())
			: QByteArray();
		if (stack.isEmpty()) {
			Toast(show, tr::lng_oblivion_share_preset_no_effects(tr::now));
		}
		return stack;
	};
	menu->addAction(tr::lng_oblivion_share_preset_save(tr::now), [=] {
		const auto stack = current();
		if (stack.isEmpty()) {
			return;
		}
		show->showBox(Box(
			SaveNameBox,
			tr::lng_oblivion_share_preset_save_title(),
			QString(),
			EffectNames(kind, StackEffects(kind, stack)),
			tr::lng_settings_save(),
			[=](QString name) {
				const auto saved = PresetLibrary().add({
					.kind = kind,
					.title = name,
					.stack = stack,
				});
				Toast(show, saved
					? tr::lng_oblivion_share_preset_saved(tr::now)
					: tr::lng_oblivion_share_preset_limit(tr::now));
			}));
	});
	menu->addAction(tr::lng_oblivion_share_preset_mine(tr::now), [=] {
		ShowMyPresets(host);
	});
	menu->addAction(tr::lng_oblivion_share_preset_share(tr::now), [=] {
		const auto stack = current();
		if (stack.isEmpty()) {
			return;
		}
		WithAccount(show, [=](not_null<Main::Session*> session) {
			SharePresetWith(session, show, kind, QString(), stack, 0);
		});
	});
	menu->addAction(tr::lng_oblivion_share_preset_gallery(tr::now), [=] {
		const auto apply = CheckedApply(host);
		WithAccount(show, [=](not_null<Main::Session*> session) {
			ShowGalleryWith(base::make_weak(session), show, kind, apply);
		});
	});
}

void ShowPresetsMenu(
		not_null<QWidget*> parent,
		QPoint globalPosition,
		PresetHost host) {
	// One menu at a time, the previous one is not kept till its parent
	// dies.
	static auto Last = QPointer<Ui::PopupMenu>();
	if (const auto previous = Last.data()) {
		previous->hide();
		previous->deleteLater();
	}
	const auto menu = Ui::CreateChild<Ui::PopupMenu>(
		parent.get(),
		host.dark ? st::groupCallPopupMenu : st::defaultPopupMenu);
	Last = menu;
	FillPresetsMenu(menu, std::move(host));
	if (menu->empty()) {
		Last = nullptr;
		delete menu;
		return;
	}
	menu->popup(globalPosition);
}

} // namespace Oblivion::Share
