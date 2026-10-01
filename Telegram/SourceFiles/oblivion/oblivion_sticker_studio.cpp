/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_sticker_studio.h"

#include "base/event_filter.h"
#include "base/weak_ptr.h"
#include "chat_helpers/compose/compose_show.h"
#include "core/file_location.h"
#include "core/file_utilities.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_session.h"
#include "data/data_star_gift.h"
#include "lang/lang_keys.h"
#include "lang/lang_tag.h"
#include "lottie/lottie_common.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "oblivion/oblivion_lottie.h"
#include "oblivion/oblivion_lottie_editor.h"
#include "oblivion/oblivion_sticker_packs.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "platform/platform_file_utilities.h"
#include "settings.h"
#include "settings/settings_common.h"
#include "storage/file_download.h"
#include "ui/abstract_button.h"
#include "ui/effects/animations.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_basic.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <crl/crl_async.h>
#include <crl/crl_object_on_queue.h>

#include <QtCore/QBuffer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QUrl>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QPainterPath>

namespace Oblivion {
namespace {

constexpr auto kPreviewHeight = 240; // In a narrow window, see ColumnsFit().
constexpr auto kPreviewPadding = 16;
constexpr auto kColumnSkip = 12; // From the preview to the right column.
constexpr auto kMaxPreviewSide = 1024;
constexpr auto kPlayButtonSize = 32;
constexpr auto kPlayBadgeSize = 28;
constexpr auto kCheckerCell = 8; // Same as in the Lottie editor canvas.
constexpr auto kMaxFileSize = qint64(64 * 1024 * 1024);
constexpr auto kMaxExportSide = 4096;
constexpr auto kMaxNameLength = 64;
constexpr auto kLongToastDuration = crl::time(6000);
constexpr auto kPngSides = std::array{ 256, 512, 1024, 0 };
constexpr auto kDefaultPngSide = 1;

enum class Background : uchar {
	Transparent,
	Dark,
	Light,
};

enum class Format : uchar {
	Tgs,
	Json,
	Png,
	Svg,
};

enum class PlayState : uchar {
	None, // Nothing to play: no animation yet or a single frame.
	Playing,
	Paused,
};

// Remembered while the app runs, main thread only.
auto LastBackground = Background::Transparent;
auto LastPngSide = kDefaultPngSide;

struct Colors {
	int hue = 0;
	int saturation = 0;
	int lightness = 0;

	[[nodiscard]] bool neutral() const {
		return !hue && !saturation && !lightness;
	}
	friend inline bool operator==(const Colors &, const Colors &) = default;
};

struct Animation {
	QByteArray source; // As opened: .tgs or JSON.
	QByteArray json;
	QString name; // Base file name for saving.
	QString title; // Shown in the box, may be empty.
	Lottie::Info info;
	bool packed = false; // source is .tgs.
};

struct Source {
	QByteArray bytes;
	QString name;
	QString title;
	DocumentData *document = nullptr;
	Data::FileOrigin origin;
};

// A fixed initial state instead of the remembered one (the UI snapshot
// scenes use it): LastBackground / LastPngSide are neither used nor
// changed, playback stays paused on the given frame after opening.
struct Preset {
	Colors colors;
	Background background = Background::Transparent;
	float64 frame = 0.; // Position in the animation, [0, 1].

	// Called once the preset frame (or the error status) is shown.
	Fn<void(not_null<Ui::GenericBox*>)> shown;
};

struct StudioArgs {
	std::shared_ptr<Ui::Show> show; // Toasts.
	Source source;
	std::optional<Preset> preset;
};

struct Exported {
	QByteArray bytes;
	bool raster = false;
	bool tooLarge = false; // .tgs that lib_lottie refuses to play.
};

struct OpenedFile {
	QByteArray bytes;
	QString name;
	bool failed = false;
};

// Lives on its own queue: parses animations and renders preview frames,
// so the UI thread only receives ready images. Each renderer is tagged
// with a generation, renders requested for another one return nothing.
class PreviewWorker final {
public:
	void set(int generation, std::unique_ptr<Lottie::Renderer> renderer) {
		_generation = generation;
		_renderer = std::move(renderer);
	}
	[[nodiscard]] int generation() const {
		return _generation;
	}
	[[nodiscard]] QImage render(int generation, int frame, QSize size) {
		return (_renderer && _generation == generation)
			? _renderer->render(frame, size)
			: QImage();
	}

private:
	int _generation = 0;
	std::unique_ptr<Lottie::Renderer> _renderer;

};

[[nodiscard]] QString SanitizeName(QString name) {
	static const auto kForbidden = u"\\/:*?\"<>|"_q;
	for (auto &ch : name) {
		if (ch.unicode() < 0x20 || kForbidden.contains(ch)) {
			ch = QChar('_');
		}
	}
	name = name.trimmed();
	if (name.size() > kMaxNameLength) {
		name = name.left(kMaxNameLength);
		if (name.back().isHighSurrogate()) {
			name.chop(1);
		}
		name = name.trimmed();
	}
	while (name.startsWith('.')) {
		name = name.mid(1);
	}
	return name.isEmpty() ? u"sticker"_q : name;
}

[[nodiscard]] QString SignedValue(int value, const QString &unit) {
	const auto sign = (value > 0)
		? u"+"_q
		: (value < 0)
		? QString(QChar(0x2212))
		: QString();
	return sign + QString::number(std::abs(value)) + unit;
}

[[nodiscard]] QString FormatPercent(float64 progress) {
	return QString::number(int(std::round(
		std::clamp(progress, 0., 1.) * 100))) + '%';
}

[[nodiscard]] QString RoundedDecimal(double value) {
	return Lang::FormatExactCountDecimal(std::round(value * 100.) / 100.);
}

[[nodiscard]] QString InfoText(const Animation &animation) {
	const auto &info = animation.info;
	const auto size = QString::number(info.size.width())
		+ QChar(0x00D7)
		+ QString::number(info.size.height());
	const auto text = tr::lng_oblivion_studio_info(
		tr::now,
		lt_size,
		size,
		lt_fps,
		RoundedDecimal(info.fps),
		lt_duration,
		tr::lng_oblivion_studio_seconds(
			tr::now,
			lt_value,
			RoundedDecimal(info.duration / 1000.)));
	return animation.title.isEmpty()
		? text
		: (animation.title + u" · "_q + text);
}

[[nodiscard]] QString FrameText(int frame, int total) {
	return tr::lng_oblivion_studio_frame(
		tr::now,
		lt_index,
		QString::number(frame + 1),
		lt_total,
		QString::number(total));
}

[[nodiscard]] QString FileFilter(const QString &name, const QString &mask) {
	return name + u" ("_q + mask + u")"_q;
}

[[nodiscard]] QString OpenFilter() {
	return FileFilter(
			tr::lng_oblivion_studio_filter(tr::now),
			u"*.tgs *.json"_q)
		+ u";;"_q
		+ FileFilter(tr::lng_oblivion_studio_filter_all(tr::now), u"*"_q);
}

[[nodiscard]] QString FormatExtension(Format format) {
	switch (format) {
	case Format::Tgs: return u"tgs"_q;
	case Format::Json: return u"json"_q;
	case Format::Png: return u"png"_q;
	case Format::Svg: return u"svg"_q;
	}
	Unexpected("Format in FormatExtension.");
}

[[nodiscard]] QString SaveFilter(Format format) {
	const auto mask = u"*."_q + FormatExtension(format);
	switch (format) {
	case Format::Tgs:
		return FileFilter(tr::lng_oblivion_file_tgs(tr::now), mask);
	case Format::Json:
		return FileFilter(tr::lng_oblivion_file_lottie(tr::now), mask);
	case Format::Png:
		return FileFilter(tr::lng_oblivion_file_png(tr::now), mask);
	case Format::Svg:
		return FileFilter(tr::lng_oblivion_studio_file_svg(tr::now), mask);
	}
	Unexpected("Format in SaveFilter.");
}

[[nodiscard]] QString SaveCaption(Format format) {
	return (format == Format::Tgs || format == Format::Json)
		? tr::lng_oblivion_studio_save_animation(tr::now)
		: tr::lng_oblivion_studio_save_frame(tr::now);
}

[[nodiscard]] QString ExportFileName(
		const QString &name,
		Colors colors,
		Format format,
		int frame) {
	auto result = name;
	if (colors.hue) {
		result += u"_h"_q + QString::number(colors.hue);
	}
	if (colors.saturation) {
		result += u"_s"_q + QString::number(colors.saturation);
	}
	if (colors.lightness) {
		result += u"_l"_q + QString::number(colors.lightness);
	}
	if (format == Format::Png || format == Format::Svg) {
		result += '_' + QString::number(frame + 1);
	}
	return result + '.' + FormatExtension(format);
}

[[nodiscard]] QString SuggestedPath(const QString &fileName) {
	if (cDialogLastPath().isEmpty()) {
		Platform::FileDialog::InitLastPath();
	}
	return filedialogNextFilename(fileName, QString());
}

[[nodiscard]] QSize PngSize(QSize original, int side) {
	auto result = side
		? original.scaled(side, side, Qt::KeepAspectRatio)
		: original;
	if (result.width() > kMaxExportSide || result.height() > kMaxExportSide) {
		result = result.scaled(
			kMaxExportSide,
			kMaxExportSide,
			Qt::KeepAspectRatio);
	}
	return result.expandedTo(QSize(1, 1));
}

[[nodiscard]] QByteArray AdjustedJson(const QByteArray &json, Colors colors) {
	return colors.neutral()
		? json
		: Lottie::AdjustColors(
			json,
			colors.hue,
			colors.saturation,
			colors.lightness);
}

// Runs on a background thread.
[[nodiscard]] Exported PrepareExport(
		const Animation &animation,
		Colors colors,
		Format format,
		int frame,
		QSize pngSize) {
	const auto adjusted = AdjustedJson(animation.json, colors);
	if (adjusted.isEmpty()) {
		return {};
	}
	switch (format) {
	case Format::Tgs:
		return {
			.bytes = ((colors.neutral() && animation.packed)
				? animation.source
				: Lottie::PackTgs(adjusted)),
			.tooLarge = (adjusted.size() > ::Lottie::kMaxFileSize),
		};
	case Format::Json:
		return { adjusted };
	case Format::Png: {
		auto renderer = Lottie::Renderer(adjusted);
		const auto image = renderer.valid()
			? renderer.render(frame, pngSize)
			: QImage();
		if (image.isNull()) {
			return {};
		}
		auto bytes = QByteArray();
		auto buffer = QBuffer(&bytes);
		if (!buffer.open(QIODevice::WriteOnly)
			|| !image.save(&buffer, "PNG")) {
			return {};
		}
		buffer.close();
		return { bytes };
	}
	case Format::Svg: {
		const auto result = Lottie::ExportSvg(adjusted, frame);
		return { result.svg, !result.vector };
	}
	}
	Unexpected("Format in PrepareExport.");
}

[[nodiscard]] bool WriteFile(const QString &path, const QByteArray &bytes) {
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	const auto written = file.write(bytes);
	file.close();
	return (written == bytes.size());
}

[[nodiscard]] QByteArray ReadLocalFile(const QString &path) {
	auto file = QFile(path);
	if (file.size() > kMaxFileSize || !file.open(QIODevice::ReadOnly)) {
		return QByteArray();
	}
	return file.readAll();
}

[[nodiscard]] OpenedFile ReadOpenResult(const FileDialog::OpenResult &result) {
	if (!result.remoteContent.isEmpty()) {
		return { .bytes = result.remoteContent };
	} else if (result.paths.isEmpty()) {
		return {};
	}
	const auto path = result.paths.front();
	auto bytes = ReadLocalFile(path);
	return {
		.bytes = bytes,
		.name = QFileInfo(path).completeBaseName(),
		.failed = bytes.isEmpty(),
	};
}

[[nodiscard]] QString DroppedPath(const QMimeData *data) {
	if (!data || !data->hasUrls()) {
		return QString();
	}
	for (const auto &url : data->urls()) {
		if (!url.isLocalFile()) {
			continue;
		}
		const auto path = url.toLocalFile();
		const auto suffix = QFileInfo(path).suffix().toLower();
		if (suffix == u"tgs"_q || suffix == u"json"_q) {
			return path;
		}
	}
	return QString();
}

[[nodiscard]] bool IsLottieDocument(not_null<DocumentData*> document) {
	if (const auto sticker = document->sticker()) {
		return sticker->isLottie();
	} else if (document->mimeString().toLower()
		== u"application/x-tgsticker"_q) {
		return true;
	}
	const auto name = document->filename().toLower();
	return name.endsWith(u".tgs"_q) || name.endsWith(u".json"_q);
}

[[nodiscard]] QByteArray ReadDocumentBytes(
		not_null<DocumentData*> document,
		const std::shared_ptr<Data::DocumentMedia> &media) {
	auto bytes = media->bytes();
	if (!bytes.isEmpty()) {
		return bytes;
	}
	const auto &location = document->location(true);
	if (location.isEmpty() || !location.accessEnable()) {
		return QByteArray();
	}
	auto file = QFile(location.name());
	if (file.size() <= kMaxFileSize && file.open(QIODevice::ReadOnly)) {
		bytes = file.readAll();
		file.close();
	}
	location.accessDisable();
	return bytes;
}

void PaintPlayIcon(QPainter &p, QRectF rect, bool pause) {
	const auto side = std::min(rect.width(), rect.height());
	const auto center = rect.center();
	if (pause) {
		const auto width = side * 0.12;
		const auto height = side * 0.38;
		const auto gap = side * 0.07;
		const auto radius = width / 3.;
		p.drawRoundedRect(
			QRectF(
				center.x() - gap - width,
				center.y() - height / 2.,
				width,
				height),
			radius,
			radius);
		p.drawRoundedRect(
			QRectF(center.x() + gap, center.y() - height / 2., width, height),
			radius,
			radius);
	} else {
		const auto height = side * 0.4;
		const auto width = height * 0.87;
		const auto left = center.x() - width / 2. + width / 8.;
		auto triangle = QPolygonF();
		triangle
			<< QPointF(left, center.y() - height / 2.)
			<< QPointF(left + width, center.y())
			<< QPointF(left, center.y() + height / 2.);
		p.drawPolygon(triangle);
	}
}

class PlayButton final : public Ui::AbstractButton {
public:
	explicit PlayButton(QWidget *parent);

	void setPlaying(bool playing);

protected:
	void paintEvent(QPaintEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;

private:
	bool _playing = false;

};

PlayButton::PlayButton(QWidget *parent) : AbstractButton(parent) {
	const auto size = style::ConvertScale(kPlayButtonSize);
	resize(size, size);
}

void PlayButton::setPlaying(bool playing) {
	if (_playing != playing) {
		_playing = playing;
		update();
	}
}

void PlayButton::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto over = isOver() && isEnabled();
	p.setOpacity(isEnabled() ? 1. : 0.5);
	p.setPen(Qt::NoPen);
	p.setBrush(over ? st::activeButtonBg : st::lightButtonBgOver);
	p.drawEllipse(rect());
	p.setBrush(over ? st::activeButtonFg : st::lightButtonFg);
	PaintPlayIcon(p, QRectF(rect()), _playing);
}

void PlayButton::onStateChanged(State was, StateChangeSource source) {
	update();
}

class Preview final : public Ui::AbstractButton {
public:
	// Square while it is not wider than maxHeight, otherwise that high.
	Preview(QWidget *parent, int maxHeight);

	void setFrame(QImage frame);
	void setBackground(Background background);

	// No animation to show (opening, downloading, an error): a neutral
	// placeholder with the text is painted instead of the frame.
	void setStatus(
		const QString &status,
		std::optional<float64> progress = std::nullopt);
	void setError(const QString &error);

	void setPlayState(PlayState state);
	void setDropHighlight(bool highlight);

	[[nodiscard]] QSize renderSize() const;

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;

private:
	void refreshChecker();
	void paintStatus(QPainter &p);
	void paintDropHint(QPainter &p);
	void paintPlayBadge(QPainter &p);

	const int _maxHeight = 0;
	QImage _frame;
	QBrush _checker;
	Background _background = Background::Transparent;
	QString _status;
	std::optional<float64> _progress;
	PlayState _playState = PlayState::None;
	bool _error = false;
	bool _dropHighlight = false;

};

Preview::Preview(QWidget *parent, int maxHeight)
: AbstractButton(parent)
, _maxHeight(maxHeight) {
	setPointerCursor(false);
	refreshChecker();
	style::PaletteChanged(
	) | rpl::on_next([=] {
		refreshChecker();
		update();
	}, lifetime());
}

void Preview::refreshChecker() {
	// Softer than style::TransparentPlaceholder() (always white and gray)
	// and taken from the palette, so it doesn't glare in the night mode.
	const auto ratio = style::DevicePixelRatio();
	const auto cell = style::ConvertScale(kCheckerCell);
	auto image = QImage(
		QSize(cell * 2, cell * 2) * ratio,
		QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(ratio);
	image.fill(anim::color(st::windowBg, st::windowFg, 0.04));
	{
		auto p = QPainter(&image);
		const auto dark = anim::color(st::windowBg, st::windowFg, 0.12);
		p.fillRect(cell, 0, cell, cell, dark);
		p.fillRect(0, cell, cell, cell, dark);
	}
	_checker = QBrush(std::move(image));
}

void Preview::setFrame(QImage frame) {
	_frame = std::move(frame);
	update();
}

void Preview::setBackground(Background background) {
	if (_background != background) {
		_background = background;
		update();
	}
}

void Preview::setStatus(
		const QString &status,
		std::optional<float64> progress) {
	_status = status;
	_progress = progress;
	_error = false;
	update();
}

void Preview::setError(const QString &error) {
	_status = error;
	_progress = std::nullopt;
	_error = true;
	update();
}

void Preview::setPlayState(PlayState state) {
	if (_playState != state) {
		_playState = state;
		setPointerCursor(state != PlayState::None);
		update();
	}
}

void Preview::setDropHighlight(bool highlight) {
	if (_dropHighlight != highlight) {
		_dropHighlight = highlight;
		update();
	}
}

QSize Preview::renderSize() const {
	const auto padding = style::ConvertScale(kPreviewPadding);
	const auto side = std::min(width(), height()) - 2 * padding;
	if (side <= 0) {
		return QSize();
	}
	const auto pixels = std::min(
		side * style::DevicePixelRatio(),
		kMaxPreviewSide);
	return QSize(pixels, pixels);
}

int Preview::resizeGetHeight(int newWidth) {
	return std::min(newWidth, _maxHeight);
}

void Preview::onStateChanged(State was, StateChangeSource source) {
	update(); // The play / pause badge is shown on hover.
}

void Preview::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);

	if (_dropHighlight) {
		paintDropHint(p);
		return;
	}

	const auto radius = st::roundRadiusLarge;
	auto path = QPainterPath();
	if (!_status.isEmpty()) {
		path.addRoundedRect(QRectF(rect()), radius, radius);
		p.fillPath(path, st::windowBgOver->b);
		paintStatus(p);
		return;
	}

	path.addRoundedRect(
		QRectF(rect()).marginsRemoved(QMarginsF(0.5, 0.5, 0.5, 0.5)),
		radius,
		radius);
	switch (_background) {
	case Background::Transparent:
		p.fillPath(path, _checker);
		break;
	case Background::Dark:
		p.fillPath(path, QColor(0x1E, 0x1E, 0x1E));
		break;
	case Background::Light:
		p.fillPath(path, QColor(0xFF, 0xFF, 0xFF));
		break;
	}
	if (!_frame.isNull()) {
		const auto size = QSizeF(_frame.size()) / _frame.devicePixelRatio();
		const auto left = (width() - size.width()) / 2.;
		const auto top = (height() - size.height()) / 2.;
		p.drawImage(QRectF(QPointF(left, top), size), _frame);
	}
	auto pen = QPen(st::shadowFg->c);
	pen.setWidthF(1.);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	p.drawPath(path);

	if (isOver() && !_frame.isNull() && _playState != PlayState::None) {
		paintPlayBadge(p);
	}
}

void Preview::paintStatus(QPainter &p) {
	const auto &font = st::normalFont;
	const auto margin = 2 * style::ConvertScale(kPreviewPadding);
	const auto available = std::max(width() - 2 * margin, 1);
	const auto flags = int(Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap);
	const auto text = QFontMetrics(font->f).boundingRect(
		QRect(0, 0, available, height()),
		flags,
		_status);

	// The sad sticker takes the middle third of the icon, the rest of it
	// is transparent padding, so only the middle part is laid out.
	const auto &icon = st::stickersEmpty;
	const auto iconPadding = _error ? (icon.height() / 3) : 0;
	const auto iconHeight = _error ? (icon.height() - 2 * iconPadding) : 0;
	const auto iconSkip = _error ? style::ConvertScale(12) : 0;
	const auto barSkip = _progress ? style::ConvertScale(12) : 0;
	const auto barHeight = _progress ? style::ConvertScale(4) : 0;
	const auto full = iconHeight
		+ iconSkip
		+ text.height()
		+ barSkip
		+ barHeight;
	auto top = (height() - full) / 2;
	if (_error) {
		icon.paint(
			p,
			(width() - icon.width()) / 2,
			top - iconPadding,
			width());
		top += iconHeight + iconSkip;
	}

	p.setPen(st::windowSubTextFg->p);
	p.setFont(font);
	p.drawText(QRect(margin, top, available, text.height()), flags, _status);
	if (!_progress) {
		return;
	}
	const auto barWidth = std::min(
		std::max(text.width(), style::ConvertScale(160)),
		available);
	const auto bar = QRectF(
		(width() - barWidth) / 2.,
		top + text.height() + barSkip,
		barWidth,
		barHeight);
	const auto round = barHeight / 2.;
	p.setPen(Qt::NoPen);
	p.setBrush(anim::color(st::windowBgOver, st::windowSubTextFg, 0.3));
	p.drawRoundedRect(bar, round, round);
	auto filled = bar;
	filled.setWidth(bar.width() * std::clamp(*_progress, 0., 1.));
	if (filled.width() > 0.) {
		const auto radius = std::min(round, filled.width() / 2.);
		p.setBrush(st::windowBgActive->b);
		p.drawRoundedRect(filled, radius, radius);
	}
}

void Preview::paintDropHint(QPainter &p) {
	const auto line = style::ConvertScaleExact(2.);
	const auto radius = st::roundRadiusLarge;
	auto path = QPainterPath();
	path.addRoundedRect(
		QRectF(rect()).marginsRemoved(
			QMarginsF(line / 2., line / 2., line / 2., line / 2.)),
		radius,
		radius);
	p.fillPath(path, st::lightButtonBgOver->b);
	auto pen = QPen(st::windowBgActive->c);
	pen.setWidthF(line);
	pen.setStyle(Qt::DashLine);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	p.drawPath(path);

	const auto margin = 2 * style::ConvertScale(kPreviewPadding);
	p.setPen(st::windowActiveTextFg->p);
	p.setFont(st::semiboldFont);
	p.drawText(
		rect().marginsRemoved({ margin, margin, margin, margin }),
		int(Qt::AlignCenter | Qt::TextWordWrap),
		tr::lng_oblivion_studio_drop(tr::now));
}

void Preview::paintPlayBadge(QPainter &p) {
	const auto size = style::ConvertScale(kPlayBadgeSize);
	const auto skip = style::ConvertScale(10);
	const auto badge = QRect(
		width() - skip - size,
		height() - skip - size,
		size,
		size);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0, 0, 0, 120));
	p.drawEllipse(badge);
	p.setBrush(QColor(0xFF, 0xFF, 0xFF));
	PaintPlayIcon(
		p,
		QRectF(badge),
		(_playState == PlayState::Playing));
}

// The neutral value is in the middle, so the track is filled from the
// center to the knob, not from the left edge where zero looks like 50%.
// The same as the bipolar sliders of the photo editor.
class BipolarSlider final : public Ui::MediaSliderWheelless {
public:
	BipolarSlider(QWidget *parent, const style::MediaSlider &st);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	const style::MediaSlider &_st;

};

BipolarSlider::BipolarSlider(QWidget *parent, const style::MediaSlider &st)
: MediaSliderWheelless(parent, st)
, _st(st) {
	// The knob must stay visible, it's the only mark of zero.
	setAlwaysDisplayMarker(true);
}

void BipolarSlider::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setOpacity(fadeOpacity());

	const auto over = getCurrentOverFactor();
	const auto seek = getSeekRect(); // The same mapping as for the mouse.
	const auto value = std::clamp(getCurrentValue(), 0., 1.);
	const auto x = seek.x() + value * seek.width();
	const auto center = seek.x() + seek.width() / 2.;
	const auto line = float64(_st.width);
	const auto radius = line / 2.;
	const auto top = (height() - line) / 2.;

	p.setBrush(anim::brush(_st.inactiveFg, _st.inactiveFgOver, over));
	p.drawRoundedRect(QRectF(0., top, width(), line), radius, radius);

	const auto active = anim::brush(_st.activeFg, _st.activeFgOver, over);
	p.setBrush(active);
	const auto from = std::min(x, center);
	const auto till = std::max(x, center);
	if (till - from >= 1.) {
		p.drawRoundedRect(
			QRectF(from, top, till - from, line),
			radius,
			radius);
	}

	const auto knob = QSizeF(_st.seekSize);
	p.drawEllipse(QRectF(
		x - knob.width() / 2.,
		(height() - knob.height()) / 2.,
		knob.width(),
		knob.height()));
}

struct ValueSlider {
	not_null<Ui::MediaSlider*> slider;
	Fn<void(int)> set;
};

ValueSlider AddValueSlider(
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<QString> title,
		int minimum,
		int maximum,
		const QString &unit,
		Fn<void(int)> changed) {
	const auto head = container->add(
		object_ptr<Ui::RpWidget>(container),
		st::boxRowPadding);
	const auto label = Ui::CreateChild<Ui::FlatLabel>(
		head,
		std::move(title),
		st::defaultFlatLabel);
	const auto value = Ui::CreateChild<Ui::FlatLabel>(
		head,
		SignedValue(0, unit),
		st::settingsScaleLabel);
	head->resize(
		head->width(),
		std::max(
			st::defaultFlatLabel.style.font->height,
			st::settingsScaleLabel.style.font->height));
	rpl::combine(
		head->widthValue(),
		value->widthValue()
	) | rpl::on_next([=](int width, int valueWidth) {
		label->moveToLeft(0, 0, width);
		value->moveToLeft(width - valueWidth, 0, width);
	}, head->lifetime());

	const auto slider = container->add(
		object_ptr<BipolarSlider>(container, st::settingsScale),
		st::boxRowPadding + QMargins(
			0,
			style::ConvertScale(6),
			0,
			style::ConvertScale(10)));
	slider->resize(slider->width(), st::settingsScale.seekSize.height());

	const auto setLabel = [=](int now) {
		value->setText(SignedValue(now, unit));
	};
	slider->setPseudoDiscrete(
		maximum - minimum + 1,
		[=](int index) { return minimum + index; },
		0,
		[=](int now) {
			setLabel(now);
			changed(now);
		});
	const auto set = [=](int now) {
		slider->setValue((now - minimum) / float64(maximum - minimum));
		setLabel(now);
	};
	base::install_event_filter(slider, [=](not_null<QEvent*> e) {
		if (e->type() != QEvent::MouseButtonDblClick) {
			return base::EventFilterResult::Continue;
		}
		set(0);
		changed(0);
		return base::EventFilterResult::Cancel;
	});
	return { .slider = slider, .set = set };
}

struct State : base::has_weak_ptr {
	crl::object_on_queue<PreviewWorker> worker;
	Animation animation;
	Colors colors;
	Colors applied;
	int requests = 0;
	int generation = 0;
	bool ready = false;
	bool adjusting = false;
	bool adjustPending = false;
	bool rendering = false;
	std::optional<int> renderPending;
	int frame = 0;
	int shownFrame = -1;
	bool playing = false;
	crl::time playStarted = 0;
	int playStartFrame = 0;
	Ui::Animations::Basic playback;
	int pngSide = kDefaultPngSide;
	rpl::variable<int> frameLabelWidth;
	rpl::variable<bool> controlsShown; // An animation was opened.
	std::shared_ptr<Data::DocumentMedia> media;
	bool downloading = false;
	rpl::lifetime downloadLifetime;
	bool preset = false; // Opened with a Preset, see above.
	std::optional<float64> presetFrame;
	Fn<void()> presetShown;

	Fn<void(QByteArray, QString, QString)> open;
	Fn<void(int, Animation, Colors)> opened;
	Fn<void()> applyColors;
	Fn<void(int)> requestRender;
	Fn<void(int)> updateFrameUi;
	Fn<void()> play;
	Fn<void()> pause;
	Fn<void(Format)> save;
	Fn<void()> chooseFile;
};

// Two columns: the square preview with the playback on the left, the colors
// and the export on the right. The right column is as wide as a usual box
// and has the same rows, the left rows end kColumnSkip before it.
[[nodiscard]] int ColumnsWidth() {
	return st::boxWideWidth * 2;
}

[[nodiscard]] int RightColumnLeft(int width) {
	return width / 2;
}

[[nodiscard]] style::margins LeftRowPadding() {
	return style::margins(
		st::boxRowPadding.left(),
		0,
		style::ConvertScale(kColumnSkip),
		0);
}

[[nodiscard]] int ColumnsPreviewSide() {
	const auto padding = LeftRowPadding();
	return RightColumnLeft(ColumnsWidth()) - padding.left() - padding.right();
}

// In a window narrower than the two columns the right one goes under the
// left one, as in a usual box.
[[nodiscard]] bool ColumnsFit(const std::shared_ptr<Ui::Show> &show) {
	const auto shadow = st::boxRoundShadow.extend;
	const auto available = show
		? (show->toastParent()->width() - shadow.left() - shadow.right())
		: 0;
	return (available <= 0) || (available >= ColumnsWidth());
}

not_null<Ui::FlatLabel*> AddTitle(
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<QString> text) {
	// Aligned with the box rows (sliders, labels) below.
	const auto shift = st::boxRowPadding.left()
		- st::defaultSubsectionTitlePadding.left();
	return Ui::AddSubsectionTitle(
		container,
		std::move(text),
		style::margins(shift, 0, 0, 0));
}

void StickerStudioBox(not_null<Ui::GenericBox*> box, StudioArgs &&args) {
	const auto show = args.show;
	const auto inColumns = ColumnsFit(show);
	box->setTitle(tr::lng_oblivion_tools_sticker_studio());
	box->setWidth(inColumns ? ColumnsWidth() : st::boxWideWidth);

	const auto state = box->lifetime().make_state<State>();
	const auto weak = base::make_weak(state);
	state->pngSide = LastPngSide;
	const auto background = args.preset
		? args.preset->background
		: LastBackground;
	if (const auto &preset = args.preset) {
		state->preset = true;
		state->colors = preset->colors;
		state->presetFrame = std::clamp(preset->frame, 0., 1.);
		if (const auto shown = preset->shown) {
			state->presetShown = [=] {
				shown(box);
			};
		}
	}

	// Until an animation is opened only the preview is shown, as wide as
	// the box: there is nothing to adjust or save yet.
	const auto columns = box->addRow(
		object_ptr<Ui::RpWidget>(box),
		style::margins());
	const auto leftColumn = Ui::CreateChild<Ui::VerticalLayout>(columns);
	const auto rightWrap = Ui::CreateChild<Ui::SlideWrap<Ui::VerticalLayout>>(
		columns,
		object_ptr<Ui::VerticalLayout>(columns));
	rightWrap->hide(anim::type::instant);
	const auto controls = rightWrap->entity();

	// The left rows end kColumnSkip before the right column, or where the
	// usual box rows end while the left column takes the whole width.
	const auto leftPadding = LeftRowPadding();
	const auto leftOverhang = st::boxRowPadding.right() - leftPadding.right();
	const auto split = [=] {
		return inColumns && state->controlsShown.current();
	};
	const auto place = [=] {
		const auto width = columns->width();
		const auto leftHeight = leftColumn->height();
		const auto rightHeight = rightWrap->height();
		if (split()) {
			rightWrap->moveToLeft(RightColumnLeft(width), 0, width);
			columns->resize(width, std::max(leftHeight, rightHeight));
		} else {
			rightWrap->moveToLeft(0, leftHeight, width);
			columns->resize(width, leftHeight + rightHeight);
		}
	};
	rpl::combine(
		columns->widthValue(),
		state->controlsShown.value()
	) | rpl::on_next([=](int width, bool) {
		const auto right = split() ? RightColumnLeft(width) : 0;
		leftColumn->resizeToWidth(
			std::max(split() ? right : (width - leftOverhang), 1));
		rightWrap->resizeToWidth(std::max(width - right, 1));
		leftColumn->moveToLeft(0, 0, width);
		place();
	}, columns->lifetime());
	rpl::merge(
		leftColumn->heightValue() | rpl::to_empty,
		rightWrap->heightValue() | rpl::to_empty
	) | rpl::on_next(place, columns->lifetime());

	const auto preview = leftColumn->add(
		object_ptr<Preview>(
			leftColumn,
			(inColumns
				? ColumnsPreviewSide()
				: style::ConvertScale(kPreviewHeight))),
		leftPadding);
	preview->setBackground(background);

	const auto previewWrap = leftColumn->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			leftColumn,
			object_ptr<Ui::VerticalLayout>(leftColumn)));
	previewWrap->hide(anim::type::instant);
	const auto previewControls = previewWrap->entity();

	const auto infoLabel = previewControls->add(
		object_ptr<Ui::FlatLabel>(
			previewControls,
			QString(),
			st::defaultSubTextLabel),
		leftPadding + QMargins(0, style::ConvertScale(8), 0, 0),
		style::al_top);

	const auto frameRow = previewControls->add(
		object_ptr<Ui::RpWidget>(previewControls),
		leftPadding + QMargins(0, style::ConvertScale(8), 0, 0));
	frameRow->resize(frameRow->width(), style::ConvertScale(kPlayButtonSize));
	const auto playButton = Ui::CreateChild<PlayButton>(frameRow);
	const auto frameSlider = Ui::CreateChild<Ui::MediaSliderWheelless>(
		frameRow,
		st::settingsScale);
	frameSlider->resize(
		frameSlider->width(),
		st::settingsScale.seekSize.height());
	frameSlider->setAlwaysDisplayMarker(true);
	const auto frameLabel = Ui::CreateChild<Ui::FlatLabel>(
		frameRow,
		QString(),
		st::defaultSubTextLabel);
	rpl::combine(
		frameRow->widthValue(),
		frameLabel->widthValue(),
		state->frameLabelWidth.value()
	) | rpl::on_next([=](int width, int labelWidth, int reserved) {
		const auto height = frameRow->height();
		const auto skip = style::ConvertScale(12);
		playButton->moveToLeft(0, (height - playButton->height()) / 2, width);
		frameLabel->moveToLeft(
			width - labelWidth,
			(height - frameLabel->height()) / 2,
			width);
		const auto left = playButton->width() + skip;
		const auto right = std::max(labelWidth, reserved) + skip;
		frameSlider->resizeToWidth(std::max(width - left - right, 1));
		frameSlider->moveToLeft(
			left,
			(height - frameSlider->height()) / 2,
			width);
	}, frameRow->lifetime());

	// Captioned the same way as the PNG size choice below, without the
	// caption the "Transparent / Dark / Light" tabs don't say what they set.
	previewControls->add(
		object_ptr<Ui::FlatLabel>(
			previewControls,
			tr::lng_oblivion_studio_background(),
			st::defaultSubTextLabel),
		leftPadding + QMargins(0, style::ConvertScale(14), 0, 0));
	const auto backgrounds = previewControls->add(
		object_ptr<Ui::SettingsSlider>(previewControls, st::settingsSlider),
		leftPadding + QMargins(0, style::ConvertScale(4), 0, 0));
	backgrounds->setSections(std::vector<QString>{
		tr::lng_oblivion_studio_bg_transparent(tr::now),
		tr::lng_oblivion_studio_bg_dark(tr::now),
		tr::lng_oblivion_studio_bg_light(tr::now),
	});
	backgrounds->setActiveSectionFast(int(background));
	backgrounds->sectionActivated(
	) | rpl::on_next([=](int index) {
		LastBackground = Background(index);
		preview->setBackground(LastBackground);
	}, backgrounds->lifetime());

	// In the columns the left one ends with the about text, it's shorter.
	if (inColumns) {
		previewControls->add(
			object_ptr<Ui::FlatLabel>(
				previewControls,
				tr::lng_oblivion_studio_about(),
				st::boxDividerLabel),
			leftPadding + QMargins(0, style::ConvertScale(16), 0, 0));
	}
	Ui::AddSkip(previewControls);

	if (!inColumns) {
		Ui::AddDivider(controls);
		Ui::AddSkip(controls);
	}
	const auto colorsTitle = AddTitle(
		controls,
		tr::lng_oblivion_studio_colors());
	const auto reset = Ui::CreateChild<Ui::LinkButton>(
		controls,
		tr::lng_oblivion_studio_reset(tr::now));
	reset->hide();
	rpl::combine(
		colorsTitle->geometryValue(),
		controls->widthValue(),
		reset->widthValue()
	) | rpl::on_next([=](QRect geometry, int width, int resetWidth) {
		reset->moveToLeft(
			width - st::boxRowPadding.right() - resetWidth,
			geometry.y() + (geometry.height() - reset->height()) / 2,
			width);
	}, reset->lifetime());

	const auto colorsChanged = [=] {
		reset->setVisible(!state->colors.neutral());
		state->applyColors();
	};
	const auto degree = QString(QChar(0x00B0));
	const auto percent = QString(QChar('%'));
	const auto hue = AddValueSlider(
		controls,
		tr::lng_oblivion_studio_hue(),
		-180,
		180,
		degree,
		[=](int value) {
			state->colors.hue = value;
			colorsChanged();
		});
	const auto saturation = AddValueSlider(
		controls,
		tr::lng_oblivion_studio_saturation(),
		-100,
		100,
		percent,
		[=](int value) {
			state->colors.saturation = value;
			colorsChanged();
		});
	const auto lightness = AddValueSlider(
		controls,
		tr::lng_oblivion_studio_lightness(),
		-100,
		100,
		percent,
		[=](int value) {
			state->colors.lightness = value;
			colorsChanged();
		});
	reset->setClickedCallback([=] {
		state->colors = Colors();
		hue.set(0);
		saturation.set(0);
		lightness.set(0);
		colorsChanged();
	});
	if (!state->colors.neutral()) {
		// From a preset, the animation is opened with them already.
		hue.set(state->colors.hue);
		saturation.set(state->colors.saturation);
		lightness.set(state->colors.lightness);
		reset->show();
	}

	// The animation with the current colors in the full Lottie editor (its
	// own window, this box stays open). The pencil, the brush icon is
	// already the one of the SVG export below.
	Ui::AddSkip(controls);
	::Settings::AddButtonWithIcon(
		controls,
		tr::lng_oblivion_studio_full_editor(),
		st::settingsButton,
		{ &st::menuIconEdit }
	)->setClickedCallback([=] {
		if (!state->ready) {
			return;
		}
		const auto json = state->animation.json;
		const auto name = state->animation.name;
		const auto colors = state->colors;
		crl::async([=, show = show]() mutable {
			auto adjusted = AdjustedJson(json, colors);
			crl::on_main([=,
					strong = std::move(show),
					adjusted = std::move(adjusted)]() mutable {
				if (!adjusted.isEmpty()) {
					LottieEdit::ShowLottieEditor(std::move(adjusted), name);
				} else if (strong->valid()) {
					strong->showToast(
						tr::lng_oblivion_studio_export_failed(tr::now));
				}
			});
		});
	});

	if (!inColumns) {
		Ui::AddDivider(controls);
	}
	Ui::AddSkip(controls);

	const auto addSave = [=](
			rpl::producer<QString> text,
			const style::icon &icon,
			Format format) {
		::Settings::AddButtonWithIcon(
			controls,
			std::move(text),
			st::settingsButton,
			{ &icon }
		)->setClickedCallback([=] {
			state->save(format);
		});
	};
	AddTitle(controls, tr::lng_oblivion_studio_save_animation());
	addSave(
		tr::lng_oblivion_studio_save_tgs(),
		st::menuIconStickers,
		Format::Tgs);
	addSave(
		tr::lng_oblivion_studio_save_json(),
		st::menuIconFile,
		Format::Json);
	::Settings::AddButtonWithIcon(
		controls,
		tr::lng_oblivion_packs_add_title(),
		st::settingsButton,
		{ &st::menuIconStickerAdd }
	)->setClickedCallback([=] {
		if (!state->ready) {
			return;
		}
		const auto json = state->animation.json;
		const auto name = state->animation.name;
		const auto colors = state->colors;
		crl::async([=, show = show]() mutable {
			auto adjusted = AdjustedJson(json, colors);
			crl::on_main([=,
					strong = std::move(show),
					adjusted = std::move(adjusted)]() mutable {
				if (!strong->valid()) {
					return;
				} else if (adjusted.isEmpty()) {
					strong->showToast(
						tr::lng_oblivion_studio_export_failed(tr::now));
					return;
				}
				auto source = StickerSource::FromLottie(std::move(adjusted));
				source.name = name;
				AddToStickerPack(std::move(source));
			});
		});
	});
	Ui::AddSkip(controls);
	AddTitle(controls, tr::lng_oblivion_studio_save_frame());
	controls->add(
		object_ptr<Ui::FlatLabel>(
			controls,
			tr::lng_oblivion_studio_png_size(),
			st::defaultSubTextLabel),
		st::boxRowPadding);
	const auto sizes = controls->add(
		object_ptr<Ui::SettingsSlider>(controls, st::settingsSlider),
		st::boxRowPadding + QMargins(
			0,
			style::ConvertScale(4),
			0,
			style::ConvertScale(6)));
	sizes->setSections(std::vector<QString>{
		QString::number(kPngSides[0]),
		QString::number(kPngSides[1]),
		QString::number(kPngSides[2]),
		tr::lng_oblivion_studio_size_original(tr::now),
	});
	sizes->setActiveSectionFast(state->pngSide);
	sizes->sectionActivated(
	) | rpl::on_next([=](int index) {
		state->pngSide = LastPngSide = index;
	}, sizes->lifetime());
	addSave(
		tr::lng_oblivion_studio_save_png(),
		st::menuIconPhoto,
		Format::Png);
	addSave(
		tr::lng_oblivion_studio_save_svg(),
		st::menuIconDraw,
		Format::Svg);
	Ui::AddSkip(controls);
	if (!inColumns) {
		Ui::AddDividerText(controls, tr::lng_oblivion_studio_about());
	}

	state->updateFrameUi = [=](int frame) {
		const auto total = state->animation.info.frames;
		frameLabel->setText(FrameText(frame, total));
		if (total > 1 && !frameSlider->isChanging()) {
			frameSlider->setValue(frame / float64(total - 1));
		}
	};

	state->requestRender = [=](int frame) {
		if (!state->ready) {
			return;
		} else if (state->rendering) {
			state->renderPending = frame;
			return;
		}
		const auto size = preview->renderSize();
		if (size.isEmpty()) {
			return;
		}
		state->rendering = true;
		const auto generation = state->generation;
		state->worker.with([=](PreviewWorker &worker) {
			auto image = worker.render(generation, frame, size);
			crl::on_main(weak, [=, image = std::move(image)]() mutable {
				state->rendering = false;
				if (!image.isNull() && generation == state->generation) {
					image.setDevicePixelRatio(style::DevicePixelRatio());
					preview->setFrame(std::move(image));
					state->shownFrame = frame;
					if (state->playing) {
						state->updateFrameUi(frame);
					}
					if (state->presetShown
						&& frame == state->frame
						&& !state->adjusting
						&& !state->renderPending) {
						base::take(state->presetShown)();
					}
				}
				if (const auto pending = base::take(state->renderPending)) {
					state->requestRender(*pending);
				}
			});
		});
	};

	state->playback.init([=](crl::time now) {
		const auto &info = state->animation.info;
		if (!state->ready || !state->playing || info.frames <= 1) {
			return false;
		}
		const auto elapsed = std::max(now - state->playStarted, crl::time());
		const auto advanced = int64(elapsed * info.fps / 1000.);
		const auto frame = int((state->playStartFrame + advanced)
			% info.frames);
		if (frame != state->frame) {
			state->frame = frame;
			state->requestRender(frame);
		}
		return true;
	});

	state->play = [=] {
		if (!state->ready) {
			return;
		}
		state->playing = true;
		state->playStarted = crl::now();
		state->playStartFrame = state->frame;
		playButton->setPlaying(true);
		preview->setPlayState(PlayState::Playing);
		state->requestRender(state->frame);
		if (state->animation.info.frames > 1) {
			state->playback.start();
		}
	};
	state->pause = [=] {
		state->playing = false;
		state->playback.stop();
		playButton->setPlaying(false);
		preview->setPlayState(PlayState::Paused);
		state->updateFrameUi(state->frame);
	};
	const auto togglePlay = [=] {
		if (!state->ready || state->animation.info.frames <= 1) {
			return;
		} else if (state->playing) {
			state->pause();
		} else {
			state->play();
		}
	};
	playButton->setClickedCallback(togglePlay);
	preview->setClickedCallback(togglePlay);

	preview->widthValue(
	) | rpl::on_next([=] {
		state->requestRender(state->frame);
	}, preview->lifetime());

	state->applyColors = [=] {
		if (!state->ready) {
			return;
		} else if (state->adjusting) {
			state->adjustPending = true;
			return;
		}
		state->adjusting = true;
		const auto generation = state->generation;
		const auto json = state->animation.json;
		const auto colors = state->colors;
		state->worker.with([=](PreviewWorker &worker) {
			auto applied = false;
			if (worker.generation() == generation) {
				const auto adjusted = AdjustedJson(json, colors);
				auto renderer = adjusted.isEmpty()
					? nullptr
					: std::make_unique<Lottie::Renderer>(adjusted);
				if (renderer && renderer->valid()) {
					worker.set(generation, std::move(renderer));
					applied = true;
				}
			}
			crl::on_main(weak, [=] {
				state->adjusting = false;
				if (applied && generation == state->generation) {
					state->applied = colors;
					state->requestRender(state->frame);
				}
				if (base::take(state->adjustPending)) {
					state->applyColors();
				}
			});
		});
	};

	state->opened = [=](int request, Animation animation, Colors colors) {
		if (!animation.info.valid()) {
			if (state->ready) {
				show->showToast(tr::lng_oblivion_studio_bad_file(tr::now));
			} else {
				preview->setError(tr::lng_oblivion_studio_bad_file(tr::now));
			}
			if (const auto shown = base::take(state->presetShown)) {
				shown();
			}
			return;
		}
		const auto frames = animation.info.frames;
		state->generation = request;
		state->animation = std::move(animation);
		state->applied = colors;
		state->frame = 0;
		state->shownFrame = -1;
		state->renderPending = std::nullopt;
		state->ready = true;

		infoLabel->setText(InfoText(state->animation));
		const auto &font = st::defaultSubTextLabel.style.font;
		state->frameLabelWidth = font->width(FrameText(frames - 1, frames));
		if (frames > 1) {
			frameSlider->setDisabled(false);
			frameSlider->setAttribute(Qt::WA_TransparentForMouseEvents, false);
			frameSlider->setPseudoDiscrete(
				frames,
				[](int index) { return index; },
				0,
				[=](int frame) {
					if (state->playing) {
						state->pause();
					}
					state->frame = frame;
					frameLabel->setText(FrameText(frame, frames));
					state->requestRender(frame);
				});
		} else {
			// ContinuousSlider::setDisabled() doesn't block the mouse,
			// so drop the previous animation's callbacks and the input.
			frameSlider->setAdjustCallback(nullptr);
			frameSlider->setChangeProgressCallback(nullptr);
			frameSlider->setAttribute(Qt::WA_TransparentForMouseEvents, true);
			frameSlider->setValue(0.);
			frameSlider->setDisabled(true);
		}
		state->updateFrameUi(0);
		preview->setStatus(QString());
		const auto animated = state->preset
			? anim::type::instant
			: anim::type::normal;
		state->controlsShown = true;
		previewWrap->show(animated);
		rightWrap->show(animated);
		playButton->setDisabled(frames <= 1);

		if (state->colors != colors) {
			state->applyColors();
		}
		const auto presetFrame = base::take(state->presetFrame);
		if (frames > 1 && presetFrame) {
			state->frame = std::clamp(
				int(std::round(*presetFrame * (frames - 1))),
				0,
				frames - 1);
			state->pause();
			state->requestRender(state->frame);
		} else if (frames > 1) {
			state->play();
		} else {
			state->playing = false;
			state->playback.stop();
			playButton->setPlaying(false);
			preview->setPlayState(PlayState::None);
			state->requestRender(0);
		}
	};

	state->open = [=](QByteArray bytes, QString name, QString title) {
		const auto request = ++state->requests;
		const auto colors = state->colors;
		if (!state->ready) {
			preview->setStatus(tr::lng_oblivion_studio_opening(tr::now));
		}
		state->worker.with([=](PreviewWorker &worker) {
			const auto json = Lottie::Unpack(bytes);
			const auto adjusted = json.isEmpty()
				? QByteArray()
				: AdjustedJson(json, colors);
			auto renderer = adjusted.isEmpty()
				? nullptr
				: std::make_unique<Lottie::Renderer>(adjusted);
			auto animation = Animation();
			if (renderer && renderer->valid()) {
				animation = Animation{
					.source = bytes,
					.json = json,
					.name = name,
					.title = title,
					.info = renderer->info(),
					.packed = (json.size() != bytes.size()) || (json != bytes),
				};
				worker.set(request, std::move(renderer));
			}
			crl::on_main(weak, [=, animation = std::move(animation)]() mutable {
				state->opened(request, std::move(animation), colors);
			});
		});
	};

	state->save = [=](Format format) {
		if (!state->ready) {
			return;
		}
		const auto animation = state->animation;
		const auto colors = state->colors;
		const auto frame = std::clamp(
			((state->playing && state->shownFrame >= 0)
				? state->shownFrame
				: state->frame),
			0,
			animation.info.frames - 1);
		const auto side = std::clamp(
			state->pngSide,
			0,
			int(kPngSides.size()) - 1);
		const auto pngSize = PngSize(animation.info.size, kPngSides[side]);
		const auto fileName = ExportFileName(
			animation.name,
			colors,
			format,
			frame);
		FileDialog::GetWritePath(
			box.get(),
			SaveCaption(format),
			SaveFilter(format),
			SuggestedPath(fileName),
			crl::guard(box, [=](QString &&result) {
				const auto path = result;
				if (path.isEmpty()) {
					return;
				}
				// The show is moved to the main thread callback, so that
				// it is never released on the background thread.
				crl::async([=, show = show]() mutable {
					const auto exported = PrepareExport(
						animation,
						colors,
						format,
						frame,
						pngSize);
					const auto written = !exported.bytes.isEmpty()
						&& WriteFile(path, exported.bytes);
					const auto failed = exported.bytes.isEmpty();
					const auto raster = exported.raster;
					const auto tooLarge = exported.tooLarge;
					crl::on_main([=, strong = std::move(show)] {
						if (!strong->valid()) {
							return;
						} else if (!failed && !written) {
							strong->showToast(
								tr::lng_oblivion_write_failed(tr::now));
							return;
						}
						const auto text = failed
							? tr::lng_oblivion_studio_export_failed(tr::now)
							: tr::lng_oblivion_saved_to(
								tr::now,
								lt_path,
								QDir::toNativeSeparators(path));
						const auto note = (raster && !failed)
							? Lottie::SvgRasterNote()
							: tooLarge
							? tr::lng_oblivion_studio_tgs_too_large(tr::now)
							: QString();
						if (note.isEmpty()) {
							strong->showToast(text);
						} else {
							strong->showToast(
								text + u"\n\n"_q + note,
								kLongToastDuration);
						}
					});
				});
			}));
	};

	const auto openFile = [=](const OpenedFile &file) {
		if (file.failed) {
			show->showToast(tr::lng_oblivion_studio_read_failed(tr::now));
		} else if (!file.bytes.isEmpty()) {
			state->downloading = false;
			state->open(file.bytes, SanitizeName(file.name), file.name);
		}
	};
	state->chooseFile = [=] {
		FileDialog::GetOpenPath(
			box.get(),
			tr::lng_oblivion_studio_open_title(tr::now),
			OpenFilter(),
			crl::guard(box, [=](FileDialog::OpenResult &&result) {
				openFile(ReadOpenResult(result));
			}));
	};

	box->setAcceptDrops(true);
	base::install_event_filter(box, [=](not_null<QEvent*> e) {
		const auto type = e->type();
		if (type == QEvent::DragEnter || type == QEvent::DragMove) {
			const auto drag = static_cast<QDragMoveEvent*>(e.get());
			if (DroppedPath(drag->mimeData()).isEmpty()) {
				preview->setDropHighlight(false);
				drag->ignore();
			} else {
				preview->setDropHighlight(true);
				drag->setDropAction(Qt::CopyAction);
				drag->accept();
			}
			return base::EventFilterResult::Cancel;
		} else if (type == QEvent::DragLeave) {
			preview->setDropHighlight(false);
			return base::EventFilterResult::Cancel;
		} else if (type == QEvent::Drop) {
			const auto drop = static_cast<QDropEvent*>(e.get());
			preview->setDropHighlight(false);
			const auto path = DroppedPath(drop->mimeData());
			if (path.isEmpty()) {
				drop->ignore();
			} else {
				drop->setDropAction(Qt::CopyAction);
				drop->accept();
				auto bytes = ReadLocalFile(path);
				openFile({
					.bytes = bytes,
					.name = QFileInfo(path).completeBaseName(),
					.failed = bytes.isEmpty(),
				});
			}
			return base::EventFilterResult::Cancel;
		}
		return base::EventFilterResult::Continue;
	});

	box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	box->addLeftButton(tr::lng_oblivion_studio_open_file(), [=] {
		state->chooseFile();
	});

	const auto &source = args.source;
	if (!source.bytes.isEmpty() || !source.document) {
		state->open(source.bytes, source.name, source.title);
		return;
	}

	// A document from the chat or a gift: download it first.
	const auto document = source.document;
	const auto name = source.name;
	const auto title = source.title;
	state->media = document->createMediaView();
	const auto tryOpen = [=] {
		if (!state->media->loaded(true)) {
			return false;
		}
		auto bytes = ReadDocumentBytes(document, state->media);
		if (bytes.isEmpty()) {
			return false;
		}
		state->downloading = false;
		state->open(std::move(bytes), name, title);
		return true;
	};
	if (tryOpen()) {
		return;
	}
	const auto showProgress = [=] {
		const auto progress = document->progress();
		preview->setStatus(
			tr::lng_oblivion_studio_downloading(
				tr::now,
				lt_percent,
				FormatPercent(progress)),
			progress);
	};
	state->downloading = true;
	showProgress();
	document->session().data().documentLoadProgress(
	) | rpl::filter([=](not_null<DocumentData*> loaded) {
		return state->downloading && (loaded == document);
	}) | rpl::on_next([=] {
		if (tryOpen()) {
			return;
		} else if (document->loading()) {
			showProgress();
		} else {
			state->downloading = false;
			preview->setError(
				tr::lng_oblivion_studio_download_failed(tr::now));
		}
	}, state->downloadLifetime);
	if (!document->loading()) {
		document->save(source.origin, QString());
	}
	if (state->downloading) {
		tryOpen();
	}
}

void ShowStudioBox(
		not_null<Window::SessionController*> controller,
		Source source) {
	controller->show(Box(StickerStudioBox, StudioArgs{
		.show = controller->uiShow(),
		.source = std::move(source),
	}));
}

// UI snapshot scenes (OBLIVION_SELFTEST=ui, see oblivion_ui_snapshots.h).

constexpr auto kSnapshotWidth = 840; // The two columns fit.
constexpr auto kSnapshotNarrowWidth = 480; // They don't.
constexpr auto kSnapshotReady = "oblivionStudioSnapshotReady";

[[nodiscard]] QByteArray SnapshotSticker() {
	auto file = QFile(u":/animations/cake.tgs"_q);
	return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

[[nodiscard]] object_ptr<Ui::BoxContent> SnapshotBox(
		std::shared_ptr<Ui::Show> show,
		QByteArray bytes,
		Preset preset) {
	preset.shown = [](not_null<Ui::GenericBox*> box) {
		box->setProperty(kSnapshotReady, true);
	};
	return Box(StickerStudioBox, StudioArgs{
		.show = std::move(show),
		.source = Source{
			.bytes = std::move(bytes),
			.name = u"cake"_q,
			.title = u"cake"_q,
		},
		.preset = std::move(preset),
	});
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;
	const auto add = [](
			QString name,
			BoxScene box,
			int width = kSnapshotWidth) {
		RegisterScene({
			.name = std::move(name),
			.size = QSize(style::ConvertScale(width), 0),
			.box = std::move(box),
			.ready = [](not_null<QWidget*> widget) {
				return widget->property(kSnapshotReady).toBool();
			},
		});
	};

	// Opened and paused on the first frame, the remembered defaults.
	add(u"studio_default"_q, [](std::shared_ptr<Ui::Show> show) {
		return SnapshotBox(std::move(show), SnapshotSticker(), Preset());
	});

	// Recolored, paused in the middle, on the dark background.
	add(u"studio_adjusted"_q, [](std::shared_ptr<Ui::Show> show) {
		return SnapshotBox(std::move(show), SnapshotSticker(), Preset{
			.colors = Colors{ .hue = 120, .saturation = 20 },
			.background = Background::Dark,
			.frame = 0.5,
		});
	});

	// In a window too narrow for the two columns: one under the other.
	add(u"studio_narrow"_q, [](std::shared_ptr<Ui::Show> show) {
		return SnapshotBox(std::move(show), SnapshotSticker(), Preset());
	}, kSnapshotNarrowWidth);

	// A damaged file: only the preview with the error, no controls.
	add(u"studio_bad_file"_q, [](std::shared_ptr<Ui::Show> show) {
		return SnapshotBox(
			std::move(show),
			SnapshotSticker().left(256),
			Preset());
	});

	// The preview placeholder while a sticker from a chat is downloading,
	// as wide as in the box before the columns (the download itself needs
	// a session).
	const auto previewWidth = ColumnsWidth()
		- st::boxRowPadding.left()
		- st::boxRowPadding.right();
	RegisterScene(u"studio_downloading"_q, QSize(previewWidth, 0), [](
			not_null<Ui::RpWidget*> parent) -> QWidget* {
		const auto progress = 0.42;
		const auto preview = Ui::CreateChild<Preview>(
			parent.get(),
			ColumnsPreviewSide());
		preview->setStatus(
			tr::lng_oblivion_studio_downloading(
				tr::now,
				lt_percent,
				FormatPercent(progress)),
			progress);
		return preview;
	});
});

} // namespace

void ShowStickerStudio(
		not_null<Window::SessionController*> controller,
		QByteArray data,
		QString name) {
	// No parsing here on the main thread: the box checks the animation
	// on its worker and shows lng_oblivion_studio_bad_file if it's broken.
	const auto title = name.trimmed();
	ShowStudioBox(controller, Source{
		.bytes = std::move(data),
		.name = SanitizeName(name),
		.title = title,
	});
}

void ShowStickerStudioFor(
		not_null<Window::SessionController*> controller,
		not_null<DocumentData*> document,
		Data::FileOrigin origin,
		const QString &name) {
	if (!IsLottieDocument(document)) {
		controller->showToast(tr::lng_oblivion_studio_unsupported(tr::now));
		return;
	} else if (document->size >= Storage::kMaxFileInMemory) {
		controller->showToast(tr::lng_oblivion_studio_bad_file(tr::now));
		return;
	}
	const auto title = name.trimmed();
	ShowStudioBox(controller, Source{
		.name = SanitizeName(title.isEmpty()
			? QFileInfo(document->filename()).completeBaseName()
			: title),
		.title = title,
		.document = document,
		.origin = origin ? origin : document->stickerSetOrigin(),
	});
}

void ShowStickerStudioImport(
		not_null<Window::SessionController*> controller) {
	FileDialog::GetOpenPath(
		controller->widget().get(),
		tr::lng_oblivion_studio_open_title(tr::now),
		OpenFilter(),
		crl::guard(controller, [=](FileDialog::OpenResult &&result) {
			const auto file = ReadOpenResult(result);
			if (file.failed) {
				controller->showToast(
					tr::lng_oblivion_studio_read_failed(tr::now));
			} else if (!file.bytes.isEmpty()) {
				ShowStickerStudio(controller, file.bytes, file.name);
			}
		}));
}

void AddGiftStudioActions(
		not_null<Ui::PopupMenu*> menu,
		std::shared_ptr<ChatHelpers::Show> show,
		std::shared_ptr<Data::UniqueGift> unique) {
	if (!unique) {
		return;
	}
	const auto add = [&](
			const QString &text,
			not_null<DocumentData*> document,
			const QString &attribute,
			const style::icon &icon) {
		const auto name = attribute.isEmpty()
			? unique->title
			: (unique->title + ' ' + attribute);
		menu->addAction(text, [=] {
			if (const auto window = show->resolveWindow()) {
				ShowStickerStudioFor(window, document, {}, name);
			}
		}, &icon);
	};
	add(
		tr::lng_oblivion_studio_gift_model(tr::now),
		unique->model.document,
		unique->model.name,
		st::menuIconEdit);
	add(
		tr::lng_oblivion_studio_gift_pattern(tr::now),
		unique->pattern.document,
		unique->pattern.name,
		st::menuIconDraw);
}

} // namespace Oblivion
