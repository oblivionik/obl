/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_look_hooks.h"

#include "oblivion/oblivion_look.h"
#include "oblivion/oblivion_look_ui.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/ui_utility.h"
#include "styles/style_basic.h"
#include "styles/style_dialogs.h"
#include "styles/style_info.h"
#include "styles/style_info_profile_top_bar.h"
#include "styles/style_layers.h"

#include <QtGui/QPainterPath>

namespace Oblivion::Look {
namespace {

// Between the panel of «Родной, но лучше» and the action buttons.
constexpr auto kHeroGap = 6;
// The cover ends a little below the middle of the userpic.
constexpr auto kCoverPart = 0.62;
// At the 100% scale, for the userpic of the full size.
constexpr auto kRingGap = 3.;
constexpr auto kRingWidth = 3.;
// The ring goes away while the userpic shrinks: the part of the full size.
constexpr auto kRingFadeFrom = 0.55;
constexpr auto kRingFadeTill = 0.85;
// .d1 .row and .d2 .row of the mockups: the margins of the card.
constexpr auto kRowSide = 6;
constexpr auto kRowSideGlow = 8;
constexpr auto kRowSkip = 1;
// The second profile of the sample is that much lower than an open one.
constexpr auto kSampleLess = 40;

[[nodiscard]] int Scaled(int value) {
	return style::ConvertScale(value);
}

// The card of a chosen or a hovered row, empty: the look has none.
[[nodiscard]] QRectF RowCard(const QRect &row) {
	const auto look = Current();
	if (look == kPlain || !HasCards()) {
		return QRectF();
	}
	const auto side = Scaled((look == kNightAir) ? kRowSideGlow : kRowSide);
	const auto skip = Scaled(kRowSkip);
	const auto result = QRectF(row).marginsRemoved(
		QMarginsF(side, skip, side, skip));
	return (result.width() > 0. && result.height() > 0.)
		? result
		: QRectF();
}

// What the two hooks paint, without the screens around them: three rows
// of a chats list and the top of a profile, open and half collapsed.
class Sample final : public Ui::RpWidget {
public:
	using RpWidget::RpWidget;

private:
	void paintEvent(QPaintEvent *e) override;

	void paintProfile(QPainter &p, QRect bar, float64 shown);

};

void Sample::paintProfile(QPainter &p, QRect bar, float64 shown) {
	const auto full = st::infoProfileTopBarPhotoSize;
	const auto size = int(full * shown);
	const auto top = int(st::infoProfileTopBarPhotoTop * shown)
		- int((1. - shown) * full * 0.25);
	const auto userpic = QRect(
		bar.x() + (bar.width() - size) / 2,
		bar.y() + top,
		size,
		size);
	p.save();
	p.setClipRect(bar);
	p.fillRect(bar, st::boxDividerBg);
	PaintProfileTop(p, {
		.bar = bar,
		.userpic = userpic,
		.roundEdges = false,
		.actions = true,
		.ring = true,
	});
	{
		auto hq = PainterHighQualityEnabler(p);
		auto gradient = QLinearGradient(
			userpic.topLeft(),
			userpic.bottomRight());
		gradient.setColorAt(0., QColor(0x7b, 0x5c, 0xe6));
		gradient.setColorAt(1., QColor(0x2a, 0x1d, 0x6b));
		p.setPen(Qt::NoPen);
		p.setBrush(gradient);
		p.drawEllipse(userpic);
	}
	const auto name = u"Артём Волков"_q;
	const auto nameTop = userpic.y() + size + Scaled(10);
	p.setFont(st::semiboldFont);
	p.setPen(st::windowBoldFg);
	p.drawText(
		bar.x() + (bar.width() - st::semiboldFont->width(name)) / 2,
		nameTop + st::semiboldFont->ascent,
		name);
	const auto status = u"в сети"_q;
	p.setFont(st::normalFont);
	p.setPen(st::windowActiveTextFg);
	p.drawText(
		bar.x() + (bar.width() - st::normalFont->width(status)) / 2,
		nameTop + st::semiboldFont->height + st::normalFont->ascent,
		status);

	// The action buttons at the bottom of the bar.
	const auto buttons = std::clamp(
		bar.height() - st::infoLayerTopBarHeight,
		0,
		int(st::infoProfileTopBarActionButtonsHeight));
	if (buttons > Scaled(16)) {
		auto hq = PainterHighQualityEnabler(p);
		const auto skip = Scaled(8);
		const auto count = 4;
		const auto width = (bar.width() - (count + 1) * skip) / count;
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBg);
		for (auto i = 0; i != count; ++i) {
			p.drawRoundedRect(
				bar.x() + skip + i * (width + skip),
				bar.y() + bar.height() - buttons + skip / 2,
				width,
				buttons - skip,
				Scaled(10),
				Scaled(10));
		}
	}
	p.restore();
}

void Sample::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(rect(), st::windowBg);

	// The chats list: a usual row, the chosen one, the hovered one.
	const auto listWidth = Scaled(290);
	const auto rowHeight = st::defaultDialogRow.height;
	const auto photo = st::defaultDialogRow.photoSize;
	const auto padding = st::defaultDialogRow.padding;
	const auto names = std::array{
		u"Аня"_q,
		u"Кенты"_q,
		u"Марк"_q,
		u"Ночной эфир"_q,
	};
	p.fillRect(0, 0, listWidth, height(), st::dialogsBg);
	for (auto i = 0; i != int(names.size()); ++i) {
		const auto row = QRect(0, i * rowHeight, listWidth, rowHeight);
		const auto active = (i == 1);
		const auto selected = (i == 2);
		const auto &bg = active
			? st::dialogsBgActive
			: selected
			? st::dialogsBgOver
			: st::dialogsBg;
		if (!PaintDialogRow(
				p,
				row,
				st::dialogsBg->b,
				bg->b,
				active,
				selected)) {
			p.fillRect(row, bg);
		}
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(QColor::fromHsl((i * 83 + 200) % 360, 150, 140));
			p.drawEllipse(
				row.x() + padding.left(),
				row.y() + padding.top(),
				photo,
				photo);
		}
		const auto left = row.x() + padding.left() + photo + padding.left();
		p.setFont(st::semiboldFont);
		p.setPen(active
			? st::dialogsNameFgActive
			: selected
			? st::dialogsNameFgOver
			: st::dialogsNameFg);
		p.drawText(
			left,
			row.y() + padding.top() + st::semiboldFont->ascent,
			names[i]);
		p.setFont(st::normalFont);
		p.setPen(active
			? st::dialogsTextFgActive
			: selected
			? st::dialogsTextFgOver
			: st::dialogsTextFg);
		p.drawText(
			left,
			row.y()
				+ padding.top()
				+ st::semiboldFont->height
				+ Scaled(4)
				+ st::normalFont->ascent,
			u"ну что, сегодня в комнате?"_q);
	}

	// The profile: open and on the way to the collapsed bar.
	const auto skip = Scaled(16);
	const auto barWidth = width() - listWidth - 2 * skip;
	const auto barLeft = listWidth + skip;
	const auto open = QRect(
		barLeft,
		skip,
		barWidth,
		st::infoProfileTopBarHeightMax);
	paintProfile(p, open, 1.);
	const auto less = QRect(
		barLeft,
		open.y() + open.height() + skip,
		barWidth,
		st::infoProfileTopBarHeightMax - Scaled(kSampleLess));
	paintProfile(p, less, 0.7);
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	const auto size = QSize(
		Scaled(290 + 16 + 360 + 16),
		2 * st::infoProfileTopBarHeightMax + Scaled(3 * 16 - kSampleLess));
	const auto create = [](not_null<Ui::RpWidget*> parent) -> QWidget* {
		return Ui::CreateChild<Sample>(parent.get());
	};
	RegisterScene(u"look_hooks"_q, size, create);
	RegisterScenes(u"look_hooks"_q, size, create);
});

} // namespace

void PaintProfileTop(QPainter &p, const ProfileTop &args) {
	const auto look = Current();
	if ((look != kNative && look != kNightAir) || args.bar.isEmpty()) {
		return;
	}
	p.save();
	if (args.roundEdges) {
		// The shape Info::Profile::TopBar::paintEdges() fills.
		const auto radius = st::boxRadius;
		auto path = QPainterPath();
		path.addRoundedRect(
			QRectF(args.bar.marginsAdded({ 0, 0, 0, radius + 1 })),
			radius,
			radius);
		p.setRenderHint(QPainter::Antialiasing);
		p.setClipPath(path, Qt::IntersectClip);
	}
	if (look == kNative) {
		// .d1 .p-hero: the panel ends above the action buttons, they lie
		// on the page. A bar that is collapsed to the title is a panel
		// as a whole.
		const auto title = std::min(
			args.bar.height(),
			int(st::infoLayerTopBarHeight));
		const auto buttons = args.actions
			? std::clamp(
				args.bar.height() - title,
				0,
				int(st::infoProfileTopBarActionButtonsHeight))
			: 0;
		const auto gap = buttons ? Scaled(kHeroGap) : 0;
		p.fillRect(
			args.bar.x(),
			args.bar.y(),
			args.bar.width(),
			std::max(args.bar.height() - buttons - gap, title),
			st::windowBg);
	} else {
		// .d2 .p-cover: it has the place it has under the userpic of the
		// full size and moves up together with a shrinking userpic.
		const auto full = st::infoProfileTopBarPhotoSize;
		const auto height = st::infoProfileTopBarPhotoTop
			+ int(std::round(full * kCoverPart));
		const auto bottom = args.userpic.y()
			+ int(std::round(args.userpic.height() * kCoverPart));
		if (bottom > args.bar.y()) {
			PaintCover(
				p,
				QRect(args.bar.x(), bottom - height, args.bar.width(), height));
		}
		// .d2 .p-avw: the gradient ring and a gap of the surface colour.
		const auto shown = (full > 0)
			? (args.userpic.width() / float64(full))
			: 0.;
		const auto opacity = std::clamp(
			(shown - kRingFadeFrom) / (kRingFadeTill - kRingFadeFrom),
			0.,
			1.);
		if (args.ring && opacity > 0.) {
			const auto gap = style::ConvertScaleExact(kRingGap) * shown;
			const auto width = style::ConvertScaleExact(kRingWidth) * shown;
			const auto inner = QRectF(args.userpic).marginsAdded(
				{ gap, gap, gap, gap });
			const auto outer = inner.marginsAdded(
				{ width, width, width, width });
			p.setRenderHint(QPainter::Antialiasing);
			p.setOpacity(p.opacity() * opacity);
			p.setPen(Qt::NoPen);
			p.setBrush(AccentBrush(outer));
			p.drawEllipse(outer);
			p.setBrush(st::boxDividerBg);
			p.drawEllipse(inner);
		}
	}
	p.restore();
}

bool PaintDialogRow(
		QPainter &p,
		const QRect &row,
		const QBrush &base,
		const QBrush &bg,
		bool active,
		bool selected) {
	const auto look = Current();
	if (look == kPlain || (!active && !selected) || row.isEmpty()) {
		return false;
	}
	if (!HasCards()) {
		// .d3 .row.sel: flat, with the accent bar at the left. A hovered
		// row is filled as usual.
		if (!active) {
			return false;
		}
		p.save();
		PaintSelected(p, QRectF(row), bg.color(), 0);
		p.restore();
		return true;
	}
	const auto card = RowCard(row);
	if (card.isEmpty()) {
		return false;
	}
	p.save();
	p.fillRect(row, base);
	if (active && look == kNightAir) {
		// .d2 .row.sel: the soft gradient with a stroke.
		PaintSelected(p, card, bg.color(), 0);
	} else {
		// .d1 .row.sel: a pillow of the colour of the chosen chat, the
		// texts over it keep the contrast the palette gives them.
		const auto radius = RowRadius(0);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(bg);
		p.drawRoundedRect(card, radius, radius);
	}
	p.restore();
	return true;
}

DialogRippleClip::DialogRippleClip(QPainter &p, const QRect &row, bool card) {
	const auto rect = card ? RowCard(row) : QRectF();
	if (rect.isEmpty()) {
		return;
	}
	const auto radius = RowRadius(0);
	auto path = QPainterPath();
	path.addRoundedRect(rect, radius, radius);
	p.save();
	p.setClipPath(path, Qt::IntersectClip);
	_p = &p;
}

DialogRippleClip::~DialogRippleClip() {
	if (_p) {
		_p->restore();
	}
}

bool RunHooksSelfTest(QStringList &log) {
	if (Current() != kPlain) {
		// Only the plain look can be checked without the styles.
		log.push_back(u"skipped: a look is on"_q);
		return true;
	}
	auto passed = true;
	const auto check = [&](bool condition, const QString &what) {
		if (!condition) {
			passed = false;
			log.push_back(u"FAIL: "_q + what);
		}
		return condition;
	};
	auto image = QImage(64, 64, QImage::Format_ARGB32_Premultiplied);
	image.fill(QColor(1, 2, 3));
	const auto before = image.copy();
	auto painted = true;
	{
		auto p = QPainter(&image);
		painted = PaintDialogRow(
			p,
			QRect(0, 0, 64, 32),
			QBrush(QColor(255, 0, 0)),
			QBrush(QColor(0, 0, 255)),
			true,
			true);
		PaintProfileTop(p, {
			.bar = QRect(0, 0, 64, 64),
			.userpic = QRect(16, 8, 32, 32),
			.roundEdges = true,
			.actions = true,
			.ring = true,
		});
		{
			const auto clip = DialogRippleClip(p, QRect(0, 0, 64, 32), true);
			check(!p.hasClipping(), u"plain: the ripple is not clipped"_q);
		}
		check(
			!p.testRenderHint(QPainter::Antialiasing)
				&& !p.hasClipping()
				&& (p.brush().style() == Qt::NoBrush),
			u"plain: the painter is left as it was"_q);
	}
	check(!painted, u"plain: the row is left to the caller"_q);
	check(image == before, u"plain: nothing is painted"_q);
	return passed;
}

} // namespace Oblivion::Look
