/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_app_icon.h"

#include "base/call_delayed.h"
#include "base/custom_app_icon.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "editor/photo_editor_common.h"
#include "editor/photo_editor_layer_widget.h"
#include "lang/lang_keys.h"
#include "oblivion/oblivion_settings.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "settings.h"
#include "settings/settings_common.h"
#include "ui/abstract_button.h"
#include "ui/effects/animation_value.h"
#include "ui/effects/animations.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/text/text.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/main_window.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_basic.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QSaveFile>

// How it works.
//
// The bundle icon (Images.xcassets) is the Oblivion one. A different
// choice is shown by overriding the Dock tile with
// -[NSApplication setApplicationIconImage:] (see Window::CreateIcon),
// which is safe, doesn't touch the app bundle and is re-applied on every
// launch, but works only while the app is running.
//
// Optionally ("Also change in Finder") the choice is written into the
// bundle with base::SetCustomAppIcon: it creates the "Icon\r" file with
// a resource fork and sets the custom icon flag in the FinderInfo xattr of
// Oblivion.app, then restarts the Dock. This is persistent and shows in
// Finder / Launchpad. Codesign refuses to sign a bundle with such Finder
// info, so the build strips it (strip_finder_info.sh in CMakeLists.txt)
// and StartAppIcon() writes the icon back when it finds it missing (after
// a rebuild or an update that replaced the bundle).
// We only ever clear an icon that we have set ourselves (by its digest)
// and never overwrite someone else's one on launch.

namespace Oblivion {
namespace {

constexpr auto kTelegramChoice = "telegram";
constexpr auto kCustomChoice = "custom";

constexpr auto kIconSize = 1024;
constexpr auto kBodyMargin = 100;
constexpr auto kBodySize = 824;
constexpr auto kShadowOffset = 10;
constexpr auto kShadowBlurSide = 48;
constexpr auto kShadowAlpha = 80;
constexpr auto kReadMaxSide = 2048;
constexpr auto kFinderCheckDelay = crl::time(3000);

constexpr auto kTilePreview = 64;
constexpr auto kTileTopSkip = 10;
constexpr auto kTileTextSkip = 8;
constexpr auto kTileBottomSkip = 8;
constexpr auto kTileRingSkip = 4;
constexpr auto kTileRadius = 10;
constexpr auto kTileTextPadding = 4;

// Only a QImage here, static QPixmap / QIcon can't outlive QApplication.
struct Current {
	QString choice;
	QImage image;
};

[[nodiscard]] Current &CurrentState() {
	static auto result = Current();
	return result;
}

[[nodiscard]] QString CustomIconPath() {
	return cWorkingDir() + u"tdata/oblivion_app_icon.png"_q;
}

[[nodiscard]] QImage TelegramIcon() {
	return QImage(u":/gui/art/oblivion/telegram_icon.png"_q);
}

[[nodiscard]] QImage CustomIcon() {
	return QImage(CustomIconPath());
}

[[nodiscard]] QImage ChoiceImage(const QString &choice) {
	if (choice == kTelegramChoice) {
		return TelegramIcon();
	} else if (choice == kCustomChoice) {
		return CustomIcon();
	}
	return QImage();
}

void RefreshCurrent() {
	auto &state = CurrentState();
	state.choice = Get().appIcon();
	state.image = ChoiceImage(state.choice);
}

void ApplyDockIcon() {
	RefreshCurrent();
	Core::App().refreshApplicationIcon();
}

// White icon body on a transparent 1024x1024 canvas.
[[nodiscard]] const QImage &ShapeMask() {
	static const auto result = [] {
		auto mask = QImage(u":/gui/art/oblivion/squircle_mask.png"_q);
		if (mask.isNull()) {
			mask = QImage(
				QSize(kIconSize, kIconSize),
				QImage::Format_ARGB32_Premultiplied);
			mask.fill(Qt::transparent);
			auto p = QPainter(&mask);
			auto hq = PainterHighQualityEnabler(p);
			const auto radius = kBodySize * 0.225;
			p.setPen(Qt::NoPen);
			p.setBrush(Qt::white);
			p.drawRoundedRect(
				QRectF(kBodyMargin, kBodyMargin, kBodySize, kBodySize),
				radius,
				radius);
		}
		if (mask.width() != kIconSize || mask.height() != kIconSize) {
			mask = mask.scaled(
				kIconSize,
				kIconSize,
				Qt::IgnoreAspectRatio,
				Qt::SmoothTransformation);
		}
		return std::move(mask).convertToFormat(
			QImage::Format_ARGB32_Premultiplied);
	}();
	return result;
}

[[nodiscard]] bool HasTransparentCorners(const QImage &image) {
	const auto w = image.width();
	const auto h = image.height();
	const auto inset = std::max(1, std::min(w, h) / 64);
	const auto points = {
		QPoint(inset, inset),
		QPoint(w - 1 - inset, inset),
		QPoint(inset, h - 1 - inset),
		QPoint(w - 1 - inset, h - 1 - inset),
	};
	for (const auto &point : points) {
		if (qAlpha(image.pixel(point)) > 32) {
			return false;
		}
	}
	return true;
}

// Pictures with transparent corners are treated as ready icons, the
// other ones get the macOS icon shape, margins and shadow.
[[nodiscard]] QImage PrepareIcon(QImage image) {
	image = std::move(image).convertToFormat(
		QImage::Format_ARGB32_Premultiplied);
	auto result = QImage(
		QSize(kIconSize, kIconSize),
		QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);

	if (HasTransparentCorners(image)) {
		const auto size = image.size().scaled(
			kIconSize,
			kIconSize,
			Qt::KeepAspectRatio);
		{
			auto p = QPainter(&result);
			p.drawImage(
				QPoint(
					(kIconSize - size.width()) / 2,
					(kIconSize - size.height()) / 2),
				image.scaled(
					size,
					Qt::IgnoreAspectRatio,
					Qt::SmoothTransformation));
		}
		return result;
	}

	const auto &mask = ShapeMask();
	auto shadow = mask.scaled(
		kShadowBlurSide,
		kShadowBlurSide,
		Qt::IgnoreAspectRatio,
		Qt::SmoothTransformation
	).scaled(
		kIconSize,
		kIconSize,
		Qt::IgnoreAspectRatio,
		Qt::SmoothTransformation
	).convertToFormat(QImage::Format_ARGB32_Premultiplied);
	{
		auto p = QPainter(&shadow);
		p.setCompositionMode(QPainter::CompositionMode_SourceIn);
		p.fillRect(shadow.rect(), QColor(0, 0, 0, kShadowAlpha));
	}

	const auto side = std::min(image.width(), image.height());
	const auto square = image.copy(
		(image.width() - side) / 2,
		(image.height() - side) / 2,
		side,
		side
	).scaled(
		kBodySize,
		kBodySize,
		Qt::IgnoreAspectRatio,
		Qt::SmoothTransformation);
	auto body = QImage(
		QSize(kIconSize, kIconSize),
		QImage::Format_ARGB32_Premultiplied);
	body.fill(Qt::transparent);
	{
		auto p = QPainter(&body);
		p.drawImage(kBodyMargin, kBodyMargin, square);
		p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
		p.drawImage(0, 0, mask);
	}

	{
		auto p = QPainter(&result);
		p.drawImage(0, kShadowOffset, shadow);
		p.drawImage(0, 0, body);
	}
	return result;
}

// Writes the choice into the bundle (or removes it) when needed.
// Without force only a missing icon is written back (a rebuild strips the
// Finder info, an update replaces the bundle). An icon set by someone else
// (the round icon from the advanced settings, one pasted in Finder) is left
// as it is and the option is turned off.
void SyncFinderIcon(bool force, Fn<void(QString)> toast) {
#ifdef Q_OS_MAC
	auto &settings = Get();
	const auto &image = CurrentState().image;
	const auto want = settings.appIconFinder() && !image.isNull();
	const auto current = base::CurrentCustomAppIconDigest();
	const auto ours = settings.appIconDigest();
	const auto oursActive = ours && current && (*current == ours);
	const auto missing = current && !*current;
	const auto foreign = current && *current && (*current != ours);
	if (want && !force && foreign) {
		LOG(("Oblivion: the Finder app icon was replaced, leaving it."));
		settings.setAppIconDigest(0);
		settings.setAppIconFinder(false);
	} else if (want && (force || missing)) {
		const auto digest = base::SetCustomAppIcon(image);
		settings.setAppIconDigest(digest.value_or(0));
		if (!digest) {
			LOG(("Oblivion: could not set the Finder app icon."));
			settings.setAppIconFinder(false);
		}
		if (toast) {
			toast(digest
				? tr::lng_oblivion_app_icon_finder_done(tr::now)
				: tr::lng_oblivion_app_icon_finder_failed(tr::now));
		}
	} else if (!want && oursActive) {
		const auto cleared = base::ClearCustomAppIcon();
		settings.setAppIconDigest(0);
		if (toast) {
			toast(cleared
				? tr::lng_oblivion_app_icon_finder_cleared(tr::now)
				: tr::lng_oblivion_app_icon_finder_failed(tr::now));
		}
	} else if (!want && ours) {
		// Someone else has replaced or removed our icon.
		settings.setAppIconDigest(0);
	}
#endif // Q_OS_MAC
}

class IconTile final : public Ui::AbstractButton {
public:
	IconTile(QWidget *parent, rpl::producer<QString> text);

	void setImage(QImage image);
	void setSelected(
		bool selected,
		anim::type animated = anim::type::normal);

	[[nodiscard]] bool hasImage() const;
	[[nodiscard]] static int ComputeHeight();

private:
	void paintEvent(QPaintEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;

	void paintPlaceholder(QPainter &p, QRect rect);

	QImage _image;
	QImage _prepared;
	Ui::Text::String _text;
	Ui::Animations::Simple _selectedAnimation;
	bool _selected = false;

};

IconTile::IconTile(QWidget *parent, rpl::producer<QString> text)
: AbstractButton(parent) {
	std::move(text) | rpl::on_next([=](const QString &value) {
		_text.setText(st::defaultTextStyle, value);
		update();
	}, lifetime());
}

int IconTile::ComputeHeight() {
	// The names are single words, one elided line is enough.
	return style::ConvertScale(kTileTopSkip)
		+ style::ConvertScale(kTilePreview)
		+ style::ConvertScale(kTileTextSkip)
		+ st::defaultTextStyle.font->height
		+ style::ConvertScale(kTileBottomSkip);
}

void IconTile::setImage(QImage image) {
	_image = std::move(image);
	_prepared = QImage();
	update();
}

bool IconTile::hasImage() const {
	return !_image.isNull();
}

void IconTile::setSelected(bool selected, anim::type animated) {
	if (_selected == selected) {
		if (animated == anim::type::instant) {
			_selectedAnimation.stop();
			update();
		}
		return;
	}
	_selected = selected;
	if (animated == anim::type::normal) {
		_selectedAnimation.start(
			[=] { update(); },
			_selected ? 0. : 1.,
			_selected ? 1. : 0.,
			st::universalDuration);
	} else {
		_selectedAnimation.stop();
		update();
	}
}

void IconTile::onStateChanged(State was, StateChangeSource source) {
	update();
}

void IconTile::paintPlaceholder(QPainter &p, QRect rect) {
	// The same body as the real icons have (and the selection ring uses),
	// without the transparent margins of the macOS icon canvas.
	const auto margin = rect.width() * double(kBodyMargin) / kIconSize;
	const auto body = QRectF(rect).marginsRemoved(
		{ margin, margin, margin, margin });
	const auto radius = body.width() * 0.225;
	p.setPen(Qt::NoPen);
	// The hovered tile is filled with windowBgOver, keep the shape seen.
	p.setBrush((isOver() || isDown()) ? st::windowBgRipple : st::windowBgOver);
	p.drawRoundedRect(body, radius, radius);

	// A plain accent '+' sized for the icon body, a 24px menu icon looked
	// lost in it.
	const auto half = body.width() * 0.18;
	const auto center = body.center();
	auto pen = st::windowActiveTextFg->p;
	pen.setWidthF(style::ConvertScaleExact(2.));
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	p.drawLine(
		QPointF(center.x() - half, center.y()),
		QPointF(center.x() + half, center.y()));
	p.drawLine(
		QPointF(center.x(), center.y() - half),
		QPointF(center.x(), center.y() + half));
}

void IconTile::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	auto hq = PainterHighQualityEnabler(p);

	const auto preview = style::ConvertScale(kTilePreview);
	const auto radius = style::ConvertScale(kTileRadius);
	const auto ringSkip = style::ConvertScale(kTileRingSkip);
	const auto left = (width() - preview) / 2;
	const auto top = style::ConvertScale(kTileTopSkip);
	const auto selected = _selectedAnimation.value(_selected ? 1. : 0.);

	if (isOver() || isDown()) {
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(rect(), radius, radius);
	}
	const auto area = QRect(left, top, preview, preview);
	if (selected > 0.) {
		// Around the icon body (without the transparent margins of the
		// macOS icon canvas), concentric with its rounded corners.
		const auto stroke = style::ConvertScaleExact(2.);
		const auto margin = preview * double(kBodyMargin) / kIconSize;
		const auto body = QRectF(area).marginsRemoved(
			{ margin, margin, margin, margin });
		const auto skip = ringSkip + stroke / 2.;
		const auto ring = body.marginsAdded({ skip, skip, skip, skip });
		const auto ringRadius = body.width() * 0.225 + skip;
		auto pen = st::activeButtonBg->p;
		pen.setWidthF(stroke);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.setOpacity(selected);
		p.drawRoundedRect(ring, ringRadius, ringRadius);
		p.setOpacity(1.);
	}
	if (_image.isNull()) {
		paintPlaceholder(p, area);
	} else {
		const auto ratio = style::DevicePixelRatio();
		if (_prepared.width() != preview * ratio) {
			_prepared = _image.scaled(
				preview * ratio,
				preview * ratio,
				Qt::IgnoreAspectRatio,
				Qt::SmoothTransformation);
			_prepared.setDevicePixelRatio(ratio);
		}
		p.drawImage(area.topLeft(), _prepared);
	}

	p.setPen(anim::color(st::windowFg, st::windowActiveTextFg, selected));
	const auto padding = style::ConvertScale(kTileTextPadding);
	_text.drawElided(
		p,
		padding,
		top + preview + style::ConvertScale(kTileTextSkip),
		width() - 2 * padding,
		1,
		style::al_top);
}

// Everything the box shows and the session-bound actions, so that it can
// be created without a window (see oblivion_ui_snapshots.h).
struct AppIconBoxArgs {
	QImage oblivion; // The bundle icon.
	QImage telegram;
	QImage custom; // Null: the '+' placeholder that opens the picker.
	rpl::producer<QString> choice; // Empty, kTelegramChoice, kCustomChoice.
	bool finder = false;

	// Applies a different choice.
	Fn<void(const QString &choice)> choose;
	// Stores a prepared custom icon and applies it, false on a failure.
	Fn<bool(const QImage &icon)> saveCustom;
	// Lets the user crop a non-square picture, without it the center
	// square is used.
	Fn<void(
		not_null<QWidget*> parent,
		QImage image,
		Fn<void(QImage &&cropped)> done)> crop;
	Fn<void(bool checked)> finderChanged;
	// The box is closing after something has changed.
	Fn<void()> finished;
};

void AppIconBox(not_null<Ui::GenericBox*> box, AppIconBoxArgs &&args) {
	box->setTitle(tr::lng_oblivion_app_icon());
	box->setWidth(st::boxWideWidth);

	struct State {
		QString choice;
		bool initialized = false;
		bool dirty = false;
		Fn<void(const QString &)> choose;
		Fn<bool(const QImage &)> saveCustom;
		Fn<void(not_null<QWidget*>, QImage, Fn<void(QImage &&)>)> crop;
		Fn<void(bool)> finderChanged;
		Fn<void()> finished;
	};
	const auto state = box->lifetime().make_state<State>(State{
		.choose = std::move(args.choose),
		.saveCustom = std::move(args.saveCustom),
		.crop = std::move(args.crop),
		.finderChanged = std::move(args.finderChanged),
		.finished = std::move(args.finished),
	});

	const auto row = box->addRow(
		object_ptr<Ui::RpWidget>(box),
		style::margins(
			st::boxRowPadding.left() / 2,
			st::boxRowPadding.top(),
			st::boxRowPadding.right() / 2,
			st::boxRowPadding.bottom()));
	row->resize(row->width(), IconTile::ComputeHeight());

	const auto oblivion = Ui::CreateChild<IconTile>(
		row,
		tr::lng_oblivion_app_icon_default());
	const auto telegram = Ui::CreateChild<IconTile>(
		row,
		tr::lng_oblivion_app_icon_telegram());
	const auto custom = Ui::CreateChild<IconTile>(
		row,
		tr::lng_oblivion_app_icon_custom_short());
	const auto tiles = std::vector<not_null<IconTile*>>{
		oblivion,
		telegram,
		custom,
	};

	oblivion->setImage(std::move(args.oblivion));
	telegram->setImage(std::move(args.telegram));
	custom->setImage(std::move(args.custom));

	row->widthValue() | rpl::on_next([=](int width) {
		const auto count = int(tiles.size());
		const auto height = IconTile::ComputeHeight();
		for (auto i = 0; i != count; ++i) {
			const auto left = width * i / count;
			const auto right = width * (i + 1) / count;
			tiles[i]->setGeometry(left, 0, right - left, height);
		}
	}, row->lifetime());

	std::move(
		args.choice
	) | rpl::on_next([=](const QString &choice) {
		// The current choice is shown without an animation on open.
		const auto animated = state->initialized
			? anim::type::normal
			: anim::type::instant;
		state->initialized = true;
		state->choice = choice;
		oblivion->setSelected(choice.isEmpty(), animated);
		telegram->setSelected(choice == kTelegramChoice, animated);
		custom->setSelected(choice == kCustomChoice, animated);
	}, box->lifetime());

	const auto choose = [=](const QString &choice) {
		if (state->choice == choice) {
			return;
		}
		if (state->choose) {
			state->choose(choice);
		}
		state->dirty = true;
	};
	const auto setCustom = [=](QImage image) {
		auto icon = PrepareIcon(std::move(image));
		if (!state->saveCustom || !state->saveCustom(icon)) {
			box->uiShow()->showToast(tr::lng_oblivion_write_failed(tr::now));
			return;
		}
		custom->setImage(std::move(icon));
		state->dirty = true;
	};
	const auto pickCustom = [=] {
		const auto filter = tr::lng_oblivion_app_icon_filter(tr::now)
			+ u" (*.png *.jpg *.jpeg *.heic *.heif *.icns"_q
			+ u" *.tif *.tiff *.webp *.gif *.bmp)"_q;
		const auto done = crl::guard(box, [=](
				FileDialog::OpenResult &&result) {
			auto image = QImage();
			auto ready = false;
			if (!result.paths.isEmpty()) {
				const auto &path = result.paths.front();
				ready = path.endsWith(u".icns"_q, Qt::CaseInsensitive);
				image = internal::ReadIconImage(path, kReadMaxSide);
			} else if (!result.remoteContent.isEmpty()) {
				image = QImage::fromData(result.remoteContent);
			}
			if (image.isNull()) {
				box->uiShow()->showToast(
					tr::lng_oblivion_app_icon_bad_image(tr::now));
				return;
			} else if (ready
				|| (image.width() == image.height())
				|| !state->crop) {
				setCustom(std::move(image));
				return;
			}
			state->crop(
				box,
				std::move(image),
				crl::guard(box, [=](QImage &&cropped) {
					setCustom(std::move(cropped));
				}));
		});
		FileDialog::GetOpenPath(
			box.get(),
			tr::lng_choose_image(tr::now),
			filter,
			done);
	};

	oblivion->setClickedCallback([=] { choose(QString()); });
	telegram->setClickedCallback([=] { choose(kTelegramChoice); });
	custom->setClickedCallback([=] {
		// The '+' placeholder (no readable custom icon) opens the picker.
		if (!custom->hasImage()) {
			pickCustom();
		} else {
			choose(kCustomChoice);
		}
	});

	Ui::AddSkip(box->verticalLayout());
	::Settings::AddButtonWithIcon(
		box->verticalLayout(),
		tr::lng_oblivion_app_icon_custom(),
		st::settingsButton,
		{ &st::menuIconPhoto }
	)->setClickedCallback(pickCustom);

#ifdef Q_OS_MAC
	// A settings row with a toggle, like the row above: the icons and the
	// texts of both rows are aligned (a box checkbox was not). The toggle
	// keeps its own skip from the right edge, so the right padding is
	// dropped for the long label to fit without eliding.
	const auto toggleSt = box->lifetime().make_state<style::SettingsButton>(
		st::settingsButton);
	toggleSt->padding.setRight(0);
	const auto finder = ::Settings::AddButtonWithIcon(
		box->verticalLayout(),
		tr::lng_oblivion_app_icon_finder(),
		*toggleSt,
		{ &st::menuIconShowInFolder });
	finder->toggleOn(rpl::single(args.finder));
	finder->toggledChanges(
	) | rpl::on_next([=](bool checked) {
		if (state->finderChanged) {
			state->finderChanged(checked);
		}
		state->dirty = true;
	}, finder->lifetime());
	Ui::AddSkip(box->verticalLayout());
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_oblivion_app_icon_finder_about(),
			st::boxDividerLabel),
		st::boxRowPadding + style::margins(0, 0, 0, st::boxLittleSkip));
#endif // Q_OS_MAC

	box->boxClosing() | rpl::on_next([=] {
		if (!state->dirty) {
			return;
		}
		state->dirty = false;
		if (state->finished) {
			state->finished();
		}
	}, box->lifetime());

	box->addButton(tr::lng_box_done(), [=] { box->closeBox(); });
}

// Scenes for the "ui" self-test mode, see oblivion_ui_snapshots.h.
[[nodiscard]] object_ptr<Ui::BoxContent> SampleAppIconBox(
		const QString &choice,
		QImage custom,
		bool finder) {
	const auto current = std::make_shared<rpl::variable<QString>>(choice);
	return Box(AppIconBox, AppIconBoxArgs{
		.oblivion = internal::BundleIconImage(kIconSize / 2),
		.telegram = TelegramIcon(),
		.custom = std::move(custom),
		.choice = current->value(),
		.finder = finder,
		.choose = [=](const QString &value) {
			*current = value;
		},
		.saveCustom = [=](const QImage &) {
			*current = QString(kCustomChoice);
			return true;
		},
	});
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	constexpr auto kSceneWidth = 480;
	constexpr auto kSceneWait = crl::time(600);
	const auto width = style::ConvertScale(kSceneWidth);

	// First open: the bundle icon is used, no custom picture yet.
	RegisterScene({
		.name = u"appicon_box"_q,
		.size = QSize(width, 0),
		.box = [](std::shared_ptr<Ui::Show>) {
			return SampleAppIconBox(QString(), QImage(), false);
		},
		.wait = kSceneWait,
	});

	// A photo chosen as the icon: shaped, selected, written to Finder.
	RegisterScene({
		.name = u"appicon_box_custom"_q,
		.size = QSize(width, 0),
		.box = [](std::shared_ptr<Ui::Show>) {
			return SampleAppIconBox(
				QString(kCustomChoice),
				PrepareIcon(QImage(u":/gui/art/themeimage.jpg"_q)),
				true);
		},
		.wait = kSceneWait,
	});
});

} // namespace

QIcon AppIconOverride() {
	const auto &image = CurrentState().image;
	return image.isNull()
		? QIcon()
		: QIcon(Ui::PixmapFromImage(base::duplicate(image)));
}

void StartAppIcon() {
	auto &settings = Get();
	if (settings.appIcon().isEmpty() && !settings.appIconDigest()) {
		return;
	}
	RefreshCurrent();
	if (!settings.appIcon().isEmpty() && CurrentState().image.isNull()) {
		LOG(("Oblivion: app icon \"%1\" is missing, using the default."
			).arg(settings.appIcon()));
		settings.setAppIcon(QString());
		RefreshCurrent();
	}
	if (!CurrentState().image.isNull()) {
		Core::App().refreshApplicationIcon();
	}

	// The Finder icon lives in the bundle and is lost with its update.
	base::call_delayed(kFinderCheckDelay, [] {
		SyncFinderIcon(false, nullptr);
	});
}

void ShowAppIconBox(not_null<Window::SessionController*> controller) {
	const auto weak = base::make_weak(controller);
	controller->show(Box(AppIconBox, AppIconBoxArgs{
		.oblivion = internal::BundleIconImage(kIconSize / 2),
		.telegram = TelegramIcon(),
		.custom = CustomIcon(),
		.choice = rpl::single(
			rpl::empty
		) | rpl::then(
			Get().changes()
		) | rpl::map([] {
			return Get().appIcon();
		}),
		.finder = Get().appIconFinder(),
		.choose = [](const QString &choice) {
			if (Get().appIcon() == choice) {
				return;
			}
			Get().setAppIcon(choice);
			ApplyDockIcon();
		},
		.saveCustom = [](const QImage &icon) {
			// Write to a temporary file first, a failed save keeps the old.
			auto file = QSaveFile(CustomIconPath());
			if (!file.open(QIODevice::WriteOnly)
				|| !icon.save(&file, "PNG")
				|| !file.commit()) {
				return false;
			}
			Get().setAppIcon(kCustomChoice);
			ApplyDockIcon();
			return true;
		},
		.crop = [=](
				not_null<QWidget*> parent,
				QImage image,
				Fn<void(QImage &&cropped)> done) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			Editor::PrepareProfilePhoto(
				parent,
				&strong->window(),
				Editor::EditorData{
					.cropType = Editor::EditorData::CropType::RoundedRect,
					.keepAspectRatio = true,
				},
				std::move(done),
				std::move(image));
		},
		.finderChanged = [](bool checked) {
			Get().setAppIconFinder(checked);
		},
		.finished = [=] {
			crl::on_main([=] {
				SyncFinderIcon(true, [=](QString text) {
					if (const auto strong = weak.get()) {
						strong->showToast(std::move(text));
					}
				});
			});
		},
	}));
}

#ifndef Q_OS_MAC
namespace internal {

QImage ReadIconImage(const QString &path, int maxSide) {
	auto result = QImage(path);
	if (!result.isNull()
		&& maxSide > 0
		&& std::max(result.width(), result.height()) > maxSide) {
		result = result.scaled(
			maxSide,
			maxSide,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
	}
	return result;
}

QImage BundleIconImage(int size) {
	return Window::Logo().scaled(
		size,
		size,
		Qt::KeepAspectRatio,
		Qt::SmoothTransformation);
}

} // namespace internal
#endif // !Q_OS_MAC

} // namespace Oblivion
