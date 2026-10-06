/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_photo_editor.h"

#include "base/unique_qptr.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "core/shortcuts.h"
#include "lang/lang_keys.h"
#include "oblivion/oblivion_photo_editor_canvas.h"
#include "oblivion/oblivion_photo_editor_controls.h"
#include "oblivion/oblivion_photo_panels.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "oblivion/oblivion_vision.h"
#include "ui/boxes/confirm_box.h"
#include "ui/effects/animation_value.h"
#include "ui/effects/panel_animation.h"
#include "ui/effects/radial_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/layer_manager.h"
#include "ui/layers/layer_widget.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/text/text.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/color_editor.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_basic.h"
#include "styles/style_calls.h"
#include "styles/style_compose_ai_box.h"
#include "styles/style_editor.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_media_view.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <crl/crl_async.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QPointer>
#include <QtGui/QClipboard>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDropEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QPainterPath>
#include <QtGui/QScreen>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QTextEdit>

#include <array>

namespace Oblivion::Photo {
namespace {

using namespace EditorUi;
using Tab = PhotoEditorTab;

constexpr auto kTopBarHeight = 56;
constexpr auto kTopBarSide = 8;
constexpr auto kTopButton = 40;
constexpr auto kTopButtonSkip = 2;
constexpr auto kTopGroupSkip = 14;
constexpr auto kTitleSkip = 6;
constexpr auto kSubtitleSkip = 2;
constexpr auto kTitleMinWidth = 90;
constexpr auto kDoneRight = 12;
constexpr auto kPanelWidth = 340;
constexpr auto kPanelMargin = 12;
constexpr auto kPanelRadius = 14;
constexpr auto kPanelPadding = 16;
constexpr auto kPanelTabsInset = 4;
constexpr auto kPanelPagesSkip = 4;
constexpr auto kRowButtonPadding = 4;
constexpr auto kCanvasPanelGap = 4;
constexpr auto kNarrowWidth = 760;
constexpr auto kNarrowPanelMin = 260;
constexpr auto kNarrowPanelPercent = 46;
constexpr auto kSkip = 8;
constexpr auto kLargeSkip = 14;
constexpr auto kHintTop = 10;
constexpr auto kPageBottom = 20;
constexpr auto kCardRadius = 12;
constexpr auto kCardPadding = 12;
constexpr auto kCardSkip = 10;
constexpr auto kBusySpinner = 40;
constexpr auto kBusyLine = 3;
constexpr auto kBusyTextSkip = 16;
constexpr auto kBusyHintSkip = 8;
constexpr auto kBusyHintWidth = 320;
constexpr auto kBusyHintMinWidth = 120;
constexpr auto kBusyHintPadding = 24;
constexpr auto kBusyCancelSkip = 20;
constexpr auto kBusyPlateSide = 16;
constexpr auto kBusyPlateSkip = 20;

constexpr auto kBusySlowHintDelay = crl::time(4000);
constexpr auto kCommitDelay = crl::time(600);
constexpr auto kCommitDelayMax = crl::time(2000);
constexpr auto kDetailDelay = crl::time(150);
constexpr auto kThumbsDelay = crl::time(250);
constexpr auto kHistoryLimit = 200;
constexpr auto kPreviewMinSide = 1600;
constexpr auto kPreviewMaxInitialSide = 3072;
constexpr auto kPreviewScreenPart = 0.75;
constexpr auto kPreviewGrow = 1.25;
constexpr auto kPreviewShrink = 1.6;
constexpr auto kThumbSourceSide = 400;
constexpr auto kAutoSide = 512;
constexpr auto kStraightenStep = 10;
constexpr auto kStraightenMax = 45;
constexpr auto kDetailThreshold = 1.1;
constexpr auto kRatioEpsilon = 0.01;
constexpr auto kSaveQuality = 92;
constexpr auto kSceneWait = crl::time(4000);

struct RatioPreset {
	int width = 0; // 0: free, -1: the original proportions.
	int height = 0;
};

constexpr auto kRatioFree = 0;
constexpr auto kRatioPresets = std::array<RatioPreset, 9>{ {
	{ 0, 0 },
	{ -1, -1 },
	{ 1, 1 },
	{ 4, 3 },
	{ 3, 4 },
	{ 3, 2 },
	{ 2, 3 },
	{ 16, 9 },
	{ 9, 16 },
} };

constexpr auto kTabCount = 8;
constexpr auto kToolStripButton = 40;
constexpr auto kToolStripSkip = 2;
constexpr auto kToolStripPadding = 6;
constexpr auto kToolStripGap = 8;
constexpr auto kToolStripGroupGap = 9;
constexpr auto kToolStripSeparatorInset = 8;
constexpr auto kLayersPercent = 36;
constexpr auto kLayersMin = 150;
constexpr auto kLayersMax = 320;
constexpr auto kLayersCompactMin = 84; // A header and one row.
constexpr auto kLayersSkip = 8;
constexpr auto kPagesMin = 170;
constexpr auto kPagesFade = 28;
constexpr auto kSceneSettle = crl::time(700);
constexpr auto kImportLimit = 12;
constexpr auto kThumbnailRound = 8;
constexpr auto kThumbnailOversample = 8;
constexpr auto kHeldKeyFirstWait = crl::time(2000);
constexpr auto kHeldKeyRepeatWait = crl::time(600);
constexpr auto kBackgroundLostAfter = crl::time(180) * 1000;

// The pause before a change becomes an undo step is longer than a double
// click of this system takes: the editors that reset a value by a double
// click (the tone curve, the color wheels) change it with the first click
// already, and that must not be a step of its own. Not longer than
// kCommitDelayMax for the systems where a double click may take seconds.
[[nodiscard]] crl::time CommitDelay() {
	return std::clamp(
		crl::time(QApplication::doubleClickInterval()) + 50,
		kCommitDelay,
		kCommitDelayMax);
}

[[nodiscard]] EditState GeometryOnly(const EditState &state) {
	auto result = EditState();
	result.crop = state.crop;
	result.quarterTurns = state.quarterTurns;
	result.straighten = state.straighten;
	result.flipHorizontal = state.flipHorizontal;
	result.flipVertical = state.flipVertical;
	return result;
}

[[nodiscard]] EditState WithoutFilter(EditState state) {
	state.filter = kOriginalFilter;
	state.filterIntensity = 100;
	return state;
}

[[nodiscard]] bool SameAdjustments(const EditState &a, const EditState &b) {
	for (const auto &info : AdjustList()) {
		if (a.value(info.id) != b.value(info.id)) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool HasTransparentPixels(const QImage &image) {
	if (image.isNull() || !image.hasAlphaChannel()) {
		return false;
	}
	const auto converted = (image.format()
		== QImage::Format_ARGB32_Premultiplied)
		? image
		: image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	for (auto y = 0; y != converted.height(); ++y) {
		const auto line = reinterpret_cast<const QRgb*>(
			converted.constScanLine(y));
		for (auto x = 0; x != converted.width(); ++x) {
			if (qAlpha(line[x]) != 255) {
				return true;
			}
		}
	}
	return false;
}

[[nodiscard]] QRectF RotateCrop(QRectF crop, bool clockwise) {
	return clockwise
		? QRectF(
			1. - crop.y() - crop.height(),
			crop.x(),
			crop.height(),
			crop.width())
		: QRectF(
			crop.y(),
			1. - crop.x() - crop.width(),
			crop.height(),
			crop.width());
}

[[nodiscard]] QRectF LargestCrop(
		float64 ratio,
		QSize frame,
		QPointF center) {
	if (frame.isEmpty() || ratio <= 0.) {
		return QRectF(0., 0., 1., 1.);
	}
	auto width = 1.;
	auto height = (frame.width() / ratio) / frame.height();
	if (height > 1.) {
		height = 1.;
		width = (frame.height() * ratio) / frame.width();
	}
	const auto x = std::clamp(center.x() - width / 2., 0., 1. - width);
	const auto y = std::clamp(center.y() - height / 2., 0., 1. - height);
	return QRectF(x, y, width, height);
}

[[nodiscard]] QString SizeText(QSize size) {
	return QString::number(size.width())
		+ QString(QChar(0x00D7))
		+ QString::number(size.height());
}

[[nodiscard]] QString DegreesText(int tenths) {
	return FormatDecimal(tenths / float64(kStraightenStep), 1)
		+ QChar(0x00B0);
}

[[nodiscard]] QString EffectValueText(EffectParam param, int value) {
	switch (param) {
	case EffectParam::Angle:
		return QString::number(value) + QChar(0x00B0);
	case EffectParam::Red:
	case EffectParam::Green:
	case EffectParam::Blue:
		return QString::number(value) + '%';
	default:
		return QString::number(value);
	}
}

[[nodiscard]] std::vector<QColor> SwatchPresets() {
	return {
		QColor(0x1B, 0x2A, 0x6B),
		QColor(0xF6, 0xB2, 0x6B),
		QColor(0x00, 0x00, 0x00),
		QColor(0xFF, 0xFF, 0xFF),
		QColor(0xE5, 0x39, 0x35),
		QColor(0xFF, 0x80, 0xAB),
		QColor(0x8E, 0x24, 0xAA),
		QColor(0x1E, 0x88, 0xE5),
		QColor(0x00, 0x89, 0x7B),
		QColor(0x7C, 0xB3, 0x42),
		QColor(0xFD, 0xD8, 0x35),
		QColor(0xFB, 0x8C, 0x00),
	};
}

// Shortcut letters keep working with a non-Latin keyboard layout: on
// macOS the key code of the physical key is used when Qt reports a
// letter of another alphabet (Cyrillic "я" for the Z key and so on).
// The codes are the kVK_ANSI_* virtual key codes of the US layout.
[[nodiscard]] int LatinKey(not_null<QKeyEvent*> e) {
	const auto key = e->key();
	if (key < 0x80 || key >= Qt::Key_Escape) {
		return key;
	}
#ifdef Q_OS_MAC
	switch (e->nativeVirtualKey()) {
	case 0x00: return Qt::Key_A;
	case 0x01: return Qt::Key_S;
	case 0x02: return Qt::Key_D;
	case 0x03: return Qt::Key_F;
	case 0x04: return Qt::Key_H;
	case 0x05: return Qt::Key_G;
	case 0x06: return Qt::Key_Z;
	case 0x07: return Qt::Key_X;
	case 0x08: return Qt::Key_C;
	case 0x09: return Qt::Key_V;
	case 0x0B: return Qt::Key_B;
	case 0x0C: return Qt::Key_Q;
	case 0x0D: return Qt::Key_W;
	case 0x0E: return Qt::Key_E;
	case 0x0F: return Qt::Key_R;
	case 0x10: return Qt::Key_Y;
	case 0x11: return Qt::Key_T;
	case 0x1F: return Qt::Key_O;
	case 0x20: return Qt::Key_U;
	case 0x22: return Qt::Key_I;
	case 0x23: return Qt::Key_P;
	case 0x25: return Qt::Key_L;
	case 0x26: return Qt::Key_J;
	case 0x28: return Qt::Key_K;
	case 0x2D: return Qt::Key_N;
	case 0x2E: return Qt::Key_M;
	case 0x2A: return Qt::Key_Backslash;
	}
#elif defined Q_OS_WIN // Q_OS_MAC
	const auto native = int(e->nativeVirtualKey());
	if (native >= 'A' && native <= 'Z') {
		return Qt::Key_A + (native - 'A');
	}
#endif // Q_OS_MAC || Q_OS_WIN
	return key;
}

template <typename Compute>
[[nodiscard]] rpl::producer<QString> LangValue(Compute compute) {
	return rpl::single(rpl::empty) | rpl::then(
		Lang::Updated()
	) | rpl::map([=] {
		return compute();
	});
}

[[nodiscard]] style::margins RowMargins(int top = 0) {
	const auto padding = Px(kPanelPadding);
	return style::margins(padding, top, padding, 0);
}

// The best scale of the layers for a render that must give this many
// pixels of an output that is fullOutput at the scale 1: rounded up to
// a step of sqrt(2), so slight zoom changes reuse the compositor caches.
[[nodiscard]] double ScaleForOutput(QSize needed, QSize fullOutput) {
	if (fullOutput.isEmpty() || needed.isEmpty()) {
		return 1.;
	}
	const auto raw = std::max(
		needed.width() / double(fullOutput.width()),
		needed.height() / double(fullOutput.height()));
	if (raw >= 1.) {
		return 1.;
	}
	return std::min(
		std::pow(2., std::ceil(std::log2(std::max(raw, 1e-4)) * 2.) / 2.),
		1.);
}

[[nodiscard]] QStringList ImagePaths(not_null<const QMimeData*> data) {
	auto result = QStringList();
	if (!data->hasUrls()) {
		return result;
	}
	for (const auto &url : data->urls()) {
		if (!url.isLocalFile()) {
			continue;
		}
		const auto path = url.toLocalFile();
		const auto suffix = QFileInfo(path).suffix().toLower();
		static const auto kSuffixes = QStringList{
			u"png"_q,
			u"jpg"_q,
			u"jpeg"_q,
			u"webp"_q,
			u"bmp"_q,
			u"gif"_q,
			u"tif"_q,
			u"tiff"_q,
			u"heic"_q,
			u"heif"_q,
			u"avif"_q,
		};
		if (kSuffixes.contains(suffix)) {
			result.push_back(path);
		}
	}
	return result;
}

// The document on a canvas of another size, the old canvas in the middle
// of the new one, shifted by whole pixels: what CanvasResized() does, but
// for any size (a canvas made by another tool may be out of its limits)
// and without touching Document::global.
[[nodiscard]] Document OnCanvas(Document document, QSize size) {
	if (document.empty() || size.isEmpty() || document.size == size) {
		return document;
	}
	const auto shift = QPointF(
		std::round((size.width() - document.size.width()) * 0.5),
		std::round((size.height() - document.size.height()) * 0.5));
	document.size = size;
	if (!shift.isNull()) {
		const auto move = QTransform::fromTranslate(shift.x(), shift.y());
		for (auto &layer : document.layers) {
			layer.transform = layer.transform * move;
		}
	}
	return document;
}

// The pixels of a layer scaled down to a thumbnail so that thin things
// stay visible: a pixel of the result has the average color of its block
// and the largest opacity found there. A few strokes on a large drawing
// layer are still lines then (a smooth scale fades them to nothing), an
// opaque photo comes out as the plain average.
[[nodiscard]] QImage LayerThumbnail(const QImage &pixels, QSize size) {
	const auto source = (pixels.isNull()
		|| pixels.format() == QImage::Format_ARGB32_Premultiplied)
		? pixels
		: pixels.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	const auto target = (source.isNull() || size.isEmpty())
		? QSize()
		: FitSize(source.size(), size);
	if (target.isEmpty()) {
		return QImage();
	} else if (target == source.size()) {
		return source;
	}
	auto result = QImage(target, QImage::Format_ARGB32_Premultiplied);
	if (result.isNull()) {
		return QImage();
	}
	const auto fromWidth = qint64(source.width());
	const auto fromHeight = qint64(source.height());
	for (auto y = 0; y != target.height(); ++y) {
		const auto top = int(y * fromHeight / target.height());
		const auto bottom = std::max(
			int((y + 1) * fromHeight / target.height()),
			top + 1);
		const auto to = reinterpret_cast<uint32*>(result.scanLine(y));
		for (auto x = 0; x != target.width(); ++x) {
			const auto left = int(x * fromWidth / target.width());
			const auto right = std::max(
				int((x + 1) * fromWidth / target.width()),
				left + 1);
			auto alpha = uint64(0);
			auto red = uint64(0);
			auto green = uint64(0);
			auto blue = uint64(0);
			auto strongest = uint64(0);
			for (auto line = top; line != bottom; ++line) {
				const auto from = reinterpret_cast<const uint32*>(
					source.constScanLine(line));
				for (auto column = left; column != right; ++column) {
					const auto pixel = from[column];
					const auto opacity = uint64(pixel >> 24);
					alpha += opacity;
					red += (pixel >> 16) & 0xFFU;
					green += (pixel >> 8) & 0xFFU;
					blue += pixel & 0xFFU;
					strongest = std::max(strongest, opacity);
				}
			}
			to[x] = !alpha
				? uint32(0)
				: ((uint32(strongest) << 24)
					| (uint32(red * strongest / alpha) << 16)
					| (uint32(green * strongest / alpha) << 8)
					| uint32(blue * strongest / alpha));
		}
	}
	result.setDevicePixelRatio(1.);
	return result;
}

// A key that is held when the editor closes (Cmd+Enter of "Done", Enter
// in the question about closing, Escape) goes on repeating into whatever
// gets the focus next. Under the editor that may be the send files box:
// the repeats would send its files, or close it and drop them. So the
// repeats of Enter and Escape are swallowed until the key is released or
// something is pressed anew. There is no release event for a key pressed
// together with Cmd on macOS, so a pause in the repeats ends it as well.
// The guard lives on its own: the editor is destroyed long before.
class HeldKeyGuard final : public QObject {
public:
	explicit HeldKeyGuard(not_null<QObject*> application);

	void arm();
	[[nodiscard]] bool finished() const;

protected:
	bool eventFilter(QObject *watched, QEvent *e) override;

private:
	void finish();

	base::Timer _timer;
	crl::time _lastRepeat = 0;
	bool _finished = false;

};

HeldKeyGuard::HeldKeyGuard(not_null<QObject*> application)
: QObject(application)
, _timer([=] { finish(); }) {
	application->installEventFilter(this);
}

void HeldKeyGuard::arm() {
	_lastRepeat = 0;
	_timer.callOnce(kHeldKeyFirstWait);
}

bool HeldKeyGuard::finished() const {
	return _finished;
}

void HeldKeyGuard::finish() {
	if (std::exchange(_finished, true)) {
		return;
	}
	_timer.cancel();
	if (const auto application = parent()) {
		application->removeEventFilter(this);
	}
	deleteLater();
}

bool HeldKeyGuard::eventFilter(QObject *watched, QEvent *e) {
	const auto type = e->type();
	if (_finished
		|| (type != QEvent::KeyPress && type != QEvent::KeyRelease)) {
		return false;
	}
	const auto event = static_cast<QKeyEvent*>(e);
	const auto key = event->key();
	const auto guarded = (key == Qt::Key_Return)
		|| (key == Qt::Key_Enter)
		|| (key == Qt::Key_Escape);
	const auto modifier = (key == Qt::Key_Shift)
		|| (key == Qt::Key_Control)
		|| (key == Qt::Key_Meta)
		|| (key == Qt::Key_Alt)
		|| (key == Qt::Key_AltGr)
		|| (key == Qt::Key_CapsLock);
	if (type == QEvent::KeyRelease) {
		// Windows and X11 send a release with every repeat.
		if (guarded && !event->isAutoRepeat()) {
			finish();
		}
		return false;
	} else if (!event->isAutoRepeat()) {
		// A modifier pressed or released doesn't stop the repeats.
		if (!modifier) {
			finish();
		}
		return false;
	} else if (!guarded) {
		return false;
	}
	// The first repeat comes after the system delay, the next ones at
	// the system rate, both may be set slow.
	const auto now = crl::now();
	const auto wait = _lastRepeat
		? std::clamp(
			(now - _lastRepeat) * 3,
			kHeldKeyRepeatWait,
			2 * kHeldKeyFirstWait)
		: kHeldKeyFirstWait;
	_lastRepeat = now;
	_timer.callOnce(wait);
	return true;
}

void GuardHeldKeys() {
	static auto Guard = QPointer<HeldKeyGuard>();
	const auto application = QCoreApplication::instance();
	if (!application) {
		return;
	} else if (!Guard || Guard->finished()) {
		Guard = new HeldKeyGuard(application);
	}
	Guard->arm();
}

// See InterruptedPhotoEdit. It is never destroyed: the document may hold
// contents of other modules to the very end, when what they rely on may
// be gone already.
struct InterruptedSlot {
	InterruptedPhotoEdit edit;
	int id = 0;
	int lastId = 0;
};

[[nodiscard]] InterruptedSlot &Interrupted() {
	static const auto result = new InterruptedSlot();
	return *result;
}

// The pure parts of the shell, a sub-test of OBLIVION_SELFTEST=photo_doc.
bool RunShellSelfTest(QStringList &log) {
	auto ok = true;
	const auto check = [&](bool condition, const QString &what) {
		log.push_back((condition ? u"OK: "_q : u"FAIL: "_q) + what);
		ok = ok && condition;
	};
	const auto alphaRange = [](const QImage &image) {
		auto low = 255;
		auto high = 0;
		for (auto y = 0; y != image.height(); ++y) {
			const auto line = reinterpret_cast<const uint32*>(
				image.constScanLine(y));
			for (auto x = 0; x != image.width(); ++x) {
				low = std::min(low, int(line[x] >> 24));
				high = std::max(high, int(line[x] >> 24));
			}
		}
		return std::pair(low, high);
	};

	// Layer thumbnails.
	{
		auto drawing = QImage(1200, 900, QImage::Format_ARGB32_Premultiplied);
		drawing.fill(Qt::transparent);
		for (auto y = 300; y != 303; ++y) {
			const auto line = reinterpret_cast<uint32*>(drawing.scanLine(y));
			for (auto x = 100; x != 1100; ++x) {
				line[x] = 0xFFE53935U;
			}
		}
		const auto thumbnail = LayerThumbnail(drawing, QSize(60, 60));
		const auto stroke = thumbnail.isNull()
			? QRgb(0)
			: thumbnail.pixel(30, 15);
		check(
			(thumbnail.size() == QSize(60, 45))
				&& (thumbnail.format() == QImage::Format_ARGB32_Premultiplied)
				&& (stroke == qRgba(0xE5, 0x39, 0x35, 0xFF))
				&& (qAlpha(thumbnail.pixel(30, 5)) == 0)
				&& (qAlpha(thumbnail.pixel(2, 15)) == 0),
			u"shell: a 3 px stroke on a large layer stays in its thumbnail"_q);

		const auto photo = FxTestImage(640, 480);
		const auto small = LayerThumbnail(photo, QSize(60, 60));
		const auto difference = small.isNull()
			? 255.
			: FxImageDifference(small, FxResized(photo, small.size()));
		check(
			(small.size() == QSize(60, 45))
				&& (alphaRange(small) == std::pair(255, 255))
				&& (difference < 16.),
			u"shell: the thumbnail of a photo is the photo (differs by %1)"_q.arg(
				QString::number(difference, 'f', 2)));
		check(
			LayerThumbnail(QImage(), QSize(60, 60)).isNull()
				&& LayerThumbnail(photo, QSize()).isNull()
				&& (LayerThumbnail(small, QSize(60, 60)).size()
					== small.size()),
			u"shell: thumbnails of nothing and of a small picture"_q);
	}

	// The original on the canvas of the result, for the comparison.
	{
		auto document = DocumentFromImage(FxTestImage(64, 48), EditState());
		auto layer = MakeImageLayer(FxTestImage(20, 10), u"over"_q);
		layer.transform = QTransform::fromTranslate(7., 9.);
		AddLayer(document, std::move(layer));
		document.global.contrast = 15;
		const auto size = QSize(101, 77);
		const auto expected = CanvasResized(document, size);
		const auto moved = OnCanvas(document, size);
		check(
			(moved.size == size)
				&& (moved.size == expected.size)
				&& (moved.layers == expected.layers)
				&& (moved.global == document.global),
			u"shell: the original is put on a resized canvas like the edit"_q);
		const auto beyond = QSize(20000, 9);
		check(
			(OnCanvas(document, beyond).size == beyond)
				&& (OnCanvas(document, document.size) == document)
				&& OnCanvas(Document(), size).empty(),
			u"shell: any canvas size is taken as it is"_q);
	}

	// The edit kept from an interrupted editor.
	if (!InterruptedPhotoEditId()) {
		const auto document = std::make_shared<const Document>(
			DocumentFromImage(FxTestImage(32, 24), EditState()));
		const auto first = KeepInterruptedPhotoEdit({
			.document = document,
			.fileName = u"first"_q,
		});
		const auto second = KeepInterruptedPhotoEdit({
			.document = document,
			.fileName = u"second"_q,
		});
		DropInterruptedPhotoEdit(first);
		const auto stays = (InterruptedPhotoEditId() == second);
		const auto taken = TakeInterruptedPhotoEdit();
		check(
			first
				&& second
				&& (first != second)
				&& stays
				&& (taken.document == document)
				&& (taken.fileName == u"second"_q)
				&& !InterruptedPhotoEditId()
				&& !TakeInterruptedPhotoEdit().document,
			u"shell: the latest interrupted edit is kept till it is taken"_q);
		const auto empty = KeepInterruptedPhotoEdit({});
		const auto third = KeepInterruptedPhotoEdit({ .document = document });
		const auto kept = (InterruptedPhotoEditId() == third);
		DropInterruptedPhotoEdit();
		check(
			!empty && third && kept && !InterruptedPhotoEditId(),
			u"shell: an interrupted edit is dropped, an empty one is not kept"_q);
	}
	return ok;
}

const auto ShellSelfTest = SelfTestRegistrar(
	SelfTestSuite::Doc,
	"shell",
	&RunShellSelfTest);

void PaintViewToolIcon(QPainter &p, QRectF rect, QColor color) {
	auto path = QPainterPath();
	path.moveTo(6.5, 3.5);
	path.lineTo(6.5, 19.);
	path.lineTo(10.4, 15.4);
	path.lineTo(13.1, 21.);
	path.lineTo(15.5, 19.9);
	path.lineTo(12.9, 14.4);
	path.lineTo(18., 14.4);
	path.closeSubpath();
	p.translate(rect.topLeft());
	p.scale(rect.width() / 24., rect.height() / 24.);
	p.setPen(Qt::NoPen);
	p.setBrush(color);
	p.drawPath(path);
}

// The usual sign of cropping, two corners that cross each other: the
// style icon used before (a sheet with a mark for "aspect ratio") said
// nothing while the tab shows no label.
void PaintCropIcon(QPainter &p, QRectF rect, QColor color) {
	p.translate(rect.topLeft());
	p.scale(rect.width() / 24., rect.height() / 24.);
	auto pen = QPen(color, 1.6);
	pen.setJoinStyle(Qt::RoundJoin);
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	auto first = QPainterPath();
	first.moveTo(7.5, 3.5);
	first.lineTo(7.5, 16.5);
	first.lineTo(20.5, 16.5);
	p.drawPath(first);
	auto second = QPainterPath();
	second.moveTo(3.5, 7.5);
	second.lineTo(16.5, 7.5);
	second.lineTo(16.5, 20.5);
	p.drawPath(second);
}

void PaintSheetsIcon(QPainter &p, QRectF rect, QColor color, int sheets) {
	p.translate(rect.topLeft());
	p.scale(rect.width() / 24., rect.height() / 24.);
	auto pen = QPen(color, 1.6);
	pen.setJoinStyle(Qt::RoundJoin);
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	const auto step = (sheets > 2) ? 4. : 5.;
	const auto top = (sheets > 2) ? 3.5 : 5.;
	for (auto i = sheets; i != 0;) {
		--i;
		const auto y = top + i * step;
		auto sheet = QPainterPath();
		sheet.moveTo(12., y);
		sheet.lineTo(20., y + 4.5);
		sheet.lineTo(12., y + 9.);
		sheet.lineTo(4., y + 4.5);
		sheet.closeSubpath();
		p.setBrush((i == 0)
			? QBrush(anim::with_alpha(color, 0.28))
			: QBrush(Qt::NoBrush));
		if (i == 0) {
			p.drawPath(sheet);
		} else {
			auto edge = QPainterPath();
			edge.moveTo(20., y + 4.5);
			edge.lineTo(12., y + 9.);
			edge.lineTo(4., y + 4.5);
			p.setBrush(Qt::NoBrush);
			p.drawPath(edge);
		}
	}
}

void PaintImageKindIcon(QPainter &p, QRectF rect, QColor color) {
	p.translate(rect.topLeft());
	p.scale(rect.width() / 24., rect.height() / 24.);
	auto pen = QPen(color, 1.6);
	pen.setJoinStyle(Qt::RoundJoin);
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(QRectF(4., 5., 16., 14.), 2.5, 2.5);
	auto hills = QPainterPath();
	hills.moveTo(5.5, 16.5);
	hills.lineTo(10., 11.5);
	hills.lineTo(13., 14.5);
	hills.lineTo(15.5, 12.5);
	hills.lineTo(18.5, 16.);
	p.drawPath(hills);
	p.setPen(Qt::NoPen);
	p.setBrush(color);
	p.drawEllipse(QPointF(15.5, 9.), 1.4, 1.4);
}

struct EditorRegistry {
	std::vector<std::unique_ptr<ToolDescriptor>> tools;
	std::vector<const ToolDescriptor*> sortedTools;
	std::vector<std::unique_ptr<PanelDescriptor>> panels;
	std::vector<const PanelDescriptor*> sortedPanels;
	std::vector<std::unique_ptr<LayerKindDescriptor>> kinds;
	std::vector<const LayerKindDescriptor*> sortedKinds;
	bool built = false;
	bool building = false;
};

[[nodiscard]] std::vector<Fn<void()>> &EditorRegistrars() {
	static auto result = std::vector<Fn<void()>>();
	return result;
}

[[nodiscard]] EditorRegistry &EditorRegistryData() {
	static auto result = EditorRegistry();
	return result;
}

[[nodiscard]] const EditorRegistry &BuiltEditorRegistry() {
	auto &result = EditorRegistryData();
	if (result.built) {
		return result;
	}
	result.built = true;
	result.building = true;
	RegisterLayerKind({
		.type = "image",
		.name = tr::lng_oblivion_photo_panel_layer_photo,
		.order = -100,
		.paintIcon = PaintImageKindIcon,
		.create = [](not_null<Controller*> controller) {
			controller->chooseAndImport();
		},
	});
	for (const auto &callback : EditorRegistrars()) {
		if (callback) {
			callback();
		}
	}
	result.building = false;
	for (const auto &tool : result.tools) {
		result.sortedTools.push_back(tool.get());
	}
	std::stable_sort(
		begin(result.sortedTools),
		end(result.sortedTools),
		[](const ToolDescriptor *a, const ToolDescriptor *b) {
			return a->order < b->order;
		});
	for (const auto &panel : result.panels) {
		result.sortedPanels.push_back(panel.get());
	}
	std::stable_sort(
		begin(result.sortedPanels),
		end(result.sortedPanels),
		[](const PanelDescriptor *a, const PanelDescriptor *b) {
			return a->order < b->order;
		});
	for (const auto &kind : result.kinds) {
		result.sortedKinds.push_back(kind.get());
	}
	std::stable_sort(
		begin(result.sortedKinds),
		end(result.sortedKinds),
		[](const LayerKindDescriptor *a, const LayerKindDescriptor *b) {
			return a->order < b->order;
		});
	return result;
}

struct RenderKey {
	uint64 revision = 0;
	bool uncropped = false;

	friend bool operator==(
		const RenderKey &a,
		const RenderKey &b) = default;
};

// Renders previews off the main thread, one at a time. A request made
// while a render is running replaces the pending one (the latest wins),
// the running render is not cancelled, so a continuous stream of
// requests (a slider drag) still shows intermediate results at the
// rendering speed. Only a cancellable render (a slow full resolution
// one) is dropped as soon as any newer request comes, so the following
// previews don't wait for it, and it waits for a pending regular one
// instead of replacing it. Results come back on the main thread, in
// order, cancelled renders don't come back at all.
class PreviewRenderer final : public base::has_weak_ptr {
public:
	struct Request {
		Document document;
		double scale = 1.;
		QSize size;
		LayerId active = 0;
		RenderKey key;
		bool cancellable = false;
	};
	struct Result {
		QImage image;
		RenderKey key;
		QSize size;
		QSize frame;
		uint64 id = 0;
	};

	PreviewRenderer(
		std::shared_ptr<Compositor> compositor,
		Fn<void(Result)> done);
	~PreviewRenderer();

	uint64 request(Request request);
	[[nodiscard]] uint64 lastId() const;

private:
	struct Job {
		Request request;
		uint64 id = 0;
	};

	void start(Job job);
	void finished(Result result);

	const std::shared_ptr<Compositor> _compositor;
	const Fn<void(Result)> _done;
	std::optional<Job> _pending;
	std::optional<Job> _pendingAfter; // Cancellable, after _pending.
	std::shared_ptr<std::atomic<bool>> _cancel;
	bool _running = false;
	bool _runningCancellable = false;
	uint64 _counter = 0;

};

PreviewRenderer::PreviewRenderer(
	std::shared_ptr<Compositor> compositor,
	Fn<void(Result)> done)
: _compositor(std::move(compositor))
, _done(std::move(done)) {
}

PreviewRenderer::~PreviewRenderer() {
	if (_cancel) {
		_cancel->store(true);
	}
}

uint64 PreviewRenderer::request(Request request) {
	const auto cancellable = request.cancellable;
	auto job = Job{
		.request = std::move(request),
		.id = ++_counter,
	};
	const auto id = job.id;
	if (_running) {
		if (_runningCancellable) {
			// finished() gets a null image soon and starts the pending job.
			_cancel->store(true);
		}
		if (cancellable && _pending && !_pending->request.cancellable) {
			// The latest regular preview is shown before the slow one.
			_pendingAfter = std::move(job);
		} else {
			_pending = std::move(job);
			_pendingAfter = std::nullopt;
		}
	} else {
		start(std::move(job));
	}
	return id;
}

uint64 PreviewRenderer::lastId() const {
	return _counter;
}

void PreviewRenderer::start(Job job) {
	_running = true;
	_runningCancellable = job.request.cancellable;
	_cancel = std::make_shared<std::atomic<bool>>(false);
	crl::async([
			weak = base::make_weak(this),
			cancel = _cancel,
			compositor = _compositor,
			job = std::move(job)]() mutable {
		const auto &request = job.request;
		auto image = compositor->render(request.document, {
			.scale = request.scale,
			.maxSize = request.size,
			.preview = (request.scale < 1.),
			.active = request.active,
			.cancel = cancel.get(),
		});
		auto result = Result{
			.image = std::move(image),
			.key = request.key,
			.size = request.size,
			.frame = OutputSize(request.document),
			.id = job.id,
		};
		crl::on_main(weak, [=, result = std::move(result)]() mutable {
			// The render could pass its last check before it was cancelled.
			if (cancel->load()) {
				result.image = QImage();
			}
			weak->finished(std::move(result));
		});
	});
}

void PreviewRenderer::finished(Result result) {
	_running = false;
	if (auto next = base::take(_pending)) {
		start(std::move(*next));
		_pending = base::take(_pendingAfter);
	}
	if (!result.image.isNull()) {
		_done(std::move(result));
	}
}

// Covers the editor while it works. A work that can take long and can be
// given up gets a cancel button and a hint: the hint shows up after a
// delay, its place is reserved from the start, so the button never moves.
class BusyOverlay final : public Ui::RpWidget {
public:
	explicit BusyOverlay(QWidget *parent);

	void setBusy(bool busy, QString text = QString());
	void setCancellable(Fn<void()> cancel, const QString &slowHint);
	[[nodiscard]] bool settled() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

private:
	struct Layout {
		int top = 0;
		int textTop = 0;
		int hintTop = 0;
		int hintWidth = 0;
		int cancelTop = 0;
	};

	[[nodiscard]] Layout countLayout() const;
	void updateCancelGeometry();

	QString _text;
	Ui::Text::String _hint;
	bool _hintShown = false;
	base::Timer _hintTimer;
	Fn<void()> _cancelCallback;
	const not_null<Ui::RoundButton*> _cancel;
	Ui::InfiniteRadialAnimation _radial;

};

BusyOverlay::BusyOverlay(QWidget *parent)
: RpWidget(parent)
, _hint(Px(kBusyHintMinWidth))
, _hintTimer([=] {
	_hintShown = true;
	update();
})
, _cancel(Ui::CreateChild<Ui::RoundButton>(
	this,
	tr::lng_cancel(),
	st::desktopCaptureCancel))
, _radial([=] { update(); }, st::defaultInfiniteRadialAnimation) {
	_cancel->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	_cancel->setClickedCallback([=] {
		if (const auto onstack = _cancelCallback) {
			onstack();
		}
	});
	_cancel->widthValue() | rpl::on_next([=] {
		updateCancelGeometry();
	}, _cancel->lifetime());
	_cancel->hide();
	hide();
}

void BusyOverlay::setBusy(bool busy, QString text) {
	_text = std::move(text);
	_cancelCallback = nullptr;
	_hintTimer.cancel();
	_hintShown = false;
	_cancel->hide();
	if (busy) {
		_radial.start();
		show();
		raise();
	} else {
		_radial.stop(anim::type::instant);
		hide();
	}
	update();
}

void BusyOverlay::setCancellable(
		Fn<void()> cancel,
		const QString &slowHint) {
	_cancelCallback = std::move(cancel);
	_hint.setText(st::defaultTextStyle, slowHint);
	_hintShown = false;
	_hintTimer.callOnce(kBusySlowHintDelay);
	_cancel->show();
	updateCancelGeometry();
	update();
}

bool BusyOverlay::settled() const {
	return !_hintTimer.isActive();
}

BusyOverlay::Layout BusyOverlay::countLayout() const {
	const auto size = Px(kBusySpinner);
	const auto textHeight = st::semiboldFont->height;
	auto result = Layout();
	auto full = size + Px(kBusyTextSkip) + textHeight;
	auto hintHeight = 0;
	if (_cancelCallback) {
		result.hintWidth = std::max(
			std::min(
				width() - 2 * Px(kBusyHintPadding),
				Px(kBusyHintWidth)),
			Px(kBusyHintMinWidth));
		hintHeight = _hint.countHeight(result.hintWidth);
		full += Px(kBusyHintSkip)
			+ hintHeight
			+ Px(kBusyCancelSkip)
			+ _cancel->height();
	}
	result.top = (height() - full) / 2;
	result.textTop = result.top + size + Px(kBusyTextSkip);
	result.hintTop = result.textTop + textHeight + Px(kBusyHintSkip);
	result.cancelTop = result.hintTop + hintHeight + Px(kBusyCancelSkip);
	return result;
}

void BusyOverlay::updateCancelGeometry() {
	if (!_cancelCallback) {
		return;
	}
	_cancel->moveToLeft(
		(width() - _cancel->width()) / 2,
		countLayout().cancelTop,
		width());
}

void BusyOverlay::resizeEvent(QResizeEvent *e) {
	updateCancelGeometry();
}

void BusyOverlay::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(e->rect(), st::layerBg);
	const auto size = Px(kBusySpinner);
	const auto line = Px(kBusyLine);
	const auto &font = st::semiboldFont;
	const auto layout = countLayout();
	if (_cancelCallback) {
		// The hint is a secondary text: over a bright photo the fade alone
		// doesn't make it readable, so this block gets a plate of its own.
		const auto side = Px(kBusyPlateSide);
		const auto skip = Px(kBusyPlateSkip);
		const auto radius = Px(kCardRadius);
		const auto plate = QRect(
			(width() - layout.hintWidth) / 2 - side,
			layout.top - skip,
			layout.hintWidth + 2 * side,
			layout.cancelTop + _cancel->height() - layout.top + 2 * skip);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::groupCallBg);
		p.drawRoundedRect(plate, radius, radius);
		p.setBrush(Qt::NoBrush);
	}
	auto pen = QPen(st::groupCallMembersFg->c);
	pen.setWidth(line);
	pen.setCapStyle(Qt::RoundCap);
	{
		auto hq = PainterHighQualityEnabler(p);
		Ui::InfiniteRadialAnimation::Draw(
			p,
			_radial.computeState(),
			QPoint((width() - size) / 2, layout.top),
			QSize(size, size),
			width(),
			pen,
			line);
	}
	p.setPen(st::groupCallMembersFg);
	p.setFont(font);
	p.drawText(
		QRect(0, layout.textTop, width(), font->height),
		Qt::AlignHCenter | Qt::AlignTop,
		_text);
	if (_cancelCallback && _hintShown) {
		p.setPen(st::groupCallMemberNotJoinedStatus);
		_hint.draw(p, {
			.position = QPoint(
				(width() - layout.hintWidth) / 2,
				layout.hintTop),
			.outerWidth = width(),
			.availableWidth = layout.hintWidth,
			.align = style::al_top,
		});
	}
}

// The column of the canvas tools at the left of the canvas: the built-in
// view tool, then the registered ones. More tools than fit in one column
// continue in the next one.
class ToolStrip final : public Ui::RpWidget {
public:
	ToolStrip(QWidget *parent, not_null<Controller*> controller);

	[[nodiscard]] int widthForHeight(int height) const;

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

private:
	struct Entry {
		QByteArray id;
		const ToolDescriptor *descriptor = nullptr;
		not_null<ToolButton*> button;
		// Tools of one kind stand together: the view and the layer tools,
		// the drawing ones, the collage. A thin line parts the groups.
		int group = 0;
	};
	struct Place {
		int column = 0;
		int top = 0;
		bool separated = false; // A line of another group is above.
	};
	struct Arrangement {
		std::vector<Place> places;
		int columns = 1;
		int bottom = 0; // Of the lowest button.
	};

	[[nodiscard]] Arrangement placementFor(int height) const;
	void refresh();

	const not_null<Controller*> _controller;
	std::vector<Entry> _entries;

};

ToolStrip::ToolStrip(QWidget *parent, not_null<Controller*> controller)
: RpWidget(parent)
, _controller(controller) {
	const auto size = Px(kToolStripButton);
	const auto add = [&](
			QByteArray id,
			const ToolDescriptor *descriptor,
			IconRef icon,
			QString tooltip) {
		const auto button = Ui::CreateChild<ToolButton>(
			this,
			std::move(icon),
			size);
		button->setTooltip(std::move(tooltip));
		button->setClickedCallback([=] {
			_controller->setTool(id);
		});
		button->show();
		// Orders: under 20 the layer tools, 20.. drawing, 60.. collage.
		const auto order = descriptor ? descriptor->order : 0;
		_entries.push_back({
			.id = id,
			.descriptor = descriptor,
			.button = button,
			.group = (order < 20) ? 0 : (order / 10),
		});
	};
	add(
		kViewTool,
		nullptr,
		IconRef{ .paint = PaintViewToolIcon },
		tr::lng_oblivion_photo_panel_tool_view(tr::now));
	for (const auto descriptor : AllTools()) {
		auto tooltip = descriptor->name.now();
		if (descriptor->key) {
			tooltip = WithShortcut(tooltip, QKeySequence(descriptor->key));
		}
		add(
			descriptor->id,
			descriptor,
			IconRef{ .paint = descriptor->paintIcon },
			std::move(tooltip));
	}
	rpl::merge(
		_controller->documentChanges(),
		_controller->activeLayerValue() | rpl::to_empty,
		_controller->toolValue() | rpl::to_empty
	) | rpl::on_next([=] {
		refresh();
	}, lifetime());
	refresh();
}

void ToolStrip::refresh() {
	const auto current = _controller->toolId();
	for (const auto &entry : _entries) {
		entry.button->setActive(entry.id == current);
		entry.button->setAvailable(!entry.descriptor
			|| !entry.descriptor->available
			|| entry.descriptor->available(_controller.get()));
	}
}

// Buttons go down the column, a group that starts in the middle of
// a column gets a gap (with a line) above it, what doesn't fit continues
// at the top of the next column.
ToolStrip::Arrangement ToolStrip::placementFor(int height) const {
	const auto size = Px(kToolStripButton);
	const auto skip = Px(kToolStripSkip);
	const auto padding = Px(kToolStripPadding);
	const auto gap = Px(kToolStripGroupGap);
	const auto limit = std::max(height - padding, padding + size);
	const auto total = int(_entries.size());
	const auto place = [&](int perColumn) {
		auto result = Arrangement();
		result.places.reserve(total);
		auto column = 0;
		auto inColumn = 0;
		auto top = padding;
		for (auto i = 0; i != total; ++i) {
			auto separated = (i > 0)
				&& (inColumn > 0)
				&& (_entries[i].group != _entries[i - 1].group);
			if (inColumn > 0
				&& ((inColumn >= perColumn)
					|| (top + (separated ? gap : 0) + size > limit))) {
				++column;
				inColumn = 0;
				top = padding;
				separated = false;
			}
			if (separated) {
				top += gap;
			}
			result.places.push_back({
				.column = column,
				.top = top,
				.separated = separated,
			});
			result.bottom = std::max(result.bottom, top + size);
			top += size + skip;
			++inColumn;
		}
		result.columns = column + 1;
		return result;
	};
	auto result = place(std::max(total, 1));
	if (result.columns > 1) {
		// The same number of columns, but of an even height: a full column
		// next to a short tail of the last tools looked accidental.
		const auto columns = result.columns;
		auto even = place((total + columns - 1) / columns);
		if (even.columns == columns) {
			result = std::move(even);
		}
	}
	return result;
}

int ToolStrip::widthForHeight(int height) const {
	const auto columns = placementFor(height).columns;
	return 2 * Px(kToolStripPadding)
		+ columns * Px(kToolStripButton)
		+ (columns - 1) * Px(kToolStripSkip);
}

void ToolStrip::resizeEvent(QResizeEvent *e) {
	const auto placement = placementFor(height());
	const auto padding = Px(kToolStripPadding);
	const auto step = Px(kToolStripButton) + Px(kToolStripSkip);
	for (auto i = 0; i != int(_entries.size()); ++i) {
		const auto &place = placement.places[i];
		_entries[i].button->move(padding + place.column * step, place.top);
	}
}

void ToolStrip::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto placement = placementFor(height());
	const auto padding = Px(kToolStripPadding);
	const auto size = Px(kToolStripButton);
	const auto step = size + Px(kToolStripSkip);
	const auto plate = QRect(
		0,
		0,
		width(),
		std::min(placement.bottom + padding, height()));
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::groupCallMembersBg);
		p.drawRoundedRect(plate, Px(kPanelRadius), Px(kPanelRadius));
	}
	const auto line = std::max(Px(1), 1);
	const auto gap = Px(kToolStripGroupGap);
	const auto inset = Px(kToolStripSeparatorInset);
	for (const auto &place : placement.places) {
		if (!place.separated) {
			continue;
		}
		// In the middle between the two buttons.
		const auto middle = place.top - (gap + Px(kToolStripSkip)) / 2;
		p.fillRect(
			QRect(
				padding + place.column * step + inset,
				middle - line / 2,
				size - 2 * inset,
				line),
			st::groupCallMembersBgOver);
	}
}

class Editor final : public Ui::RpWidget {
public:
	Editor(
		QWidget *parent,
		std::shared_ptr<Ui::Show> show,
		QImage image,
		PhotoEditorOptions options);
	~Editor();

	[[nodiscard]] rpl::producer<> closeRequests() const;
	[[nodiscard]] not_null<Controller*> controller() const;
	void requestClose();
	void selectTab(Tab tab);
	void setComparing(bool comparing);
	void showBackgroundProgress();
	// For the snapshot scenes.
	void scrollTabToEnd();
	void finishAnimations();
	[[nodiscard]] bool snapshotReady() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void keyReleaseEvent(QKeyEvent *e) override;
	void focusOutEvent(QFocusEvent *e) override;
	void dragEnterEvent(QDragEnterEvent *e) override;
	void dropEvent(QDropEvent *e) override;

private:
	struct Page {
		not_null<Ui::ScrollArea*> scroll;
		not_null<Ui::VerticalLayout*> content;
	};
	struct AutoState {
		EditState base;
		EditState target;
		EditState applied;
		int amount = 100;
	};

	void setupTopBar();
	void setupPanel();
	void setupCanvas();
	void setupStrip();
	void setupShortcuts();
	void setupController();
	[[nodiscard]] Page createPage();
	not_null<SectionTitle*> addTitle(
		not_null<Ui::VerticalLayout*> page,
		rpl::producer<QString> text,
		int top = 0);
	void addHint(
		not_null<Ui::VerticalLayout*> page,
		rpl::producer<QString> text);
	void setupCropPage(not_null<Ui::VerticalLayout*> page);
	void setupCanvasSection(not_null<Ui::VerticalLayout*> page);
	void resizeCanvas(QSize size);
	void setupAdjustPage(not_null<Ui::VerticalLayout*> page);
	void setupFiltersPage(not_null<Ui::VerticalLayout*> page);
	void setupEffectsPage(not_null<Ui::VerticalLayout*> page);
	void setupAutoPage(not_null<Ui::VerticalLayout*> page);
	void rebuildEffects();
	void addEffectCard(int index);
	void showEffectMenu(int index, QPoint globalPosition);
	void chooseCustomColor(int index, EffectParam param);
	void addEffect(EffectType type);
	void rebuildToolPage();
	void addLayerFxSection(
		not_null<Ui::VerticalLayout*> page,
		rpl::producer<QString> title,
		rpl::producer<QString> about,
		rpl::producer<QString> button,
		std::vector<FxGroup> groups,
		bool flat);
	void showLayerEffects();
	void revealInPanel(QWidget *widget);

	void startLoading(QImage image);
	void sourceReady(QImage source);
	void documentReady(Document document);
	[[nodiscard]] const Layer *backgroundLayer() const;
	void refreshBackgroundState();
	void toggleBackground();
	void startBackgroundRequest(
		LayerId id,
		std::shared_ptr<const ImageContent> content);
	void cancelBackground();
	void finishBackground();
	void backgroundReady(
		int token,
		LayerId id,
		std::shared_ptr<const ImageContent> from,
		std::shared_ptr<const ImageContent> cutout);
	void keepInterrupted();

	void apply(EditState state, bool commitNow);
	void commit();
	void undo();
	void redo();
	void documentChanged();
	void stateChanged();
	void refreshHistoryButtons();
	[[nodiscard]] EditState displayState() const;
	[[nodiscard]] Document displayDocument() const;
	[[nodiscard]] RenderKey displayKey() const;
	[[nodiscard]] double proxyScale() const;
	[[nodiscard]] bool dirty() const;
	[[nodiscard]] bool resetAvailable() const;

	void refreshPreview(bool force);
	void requestDetail();
	void previewReady(PreviewRenderer::Result &&result);
	void refreshBefore();
	void beforeReady(PreviewRenderer::Result &&result);
	void refreshAnalysis();
	void analysisReady(QImage reduced, bool alpha, uint64 revision);
	void scheduleThumbnails();
	void refreshThumbnails();

	void inferRatio();
	void syncRatio();
	[[nodiscard]] std::optional<float64> cropRatio() const;
	void selectRatio(int index);
	void rotate(bool clockwise);
	void flip(bool horizontal);
	void resetGeometry();
	void stepFilter(int delta);
	void selectFilter(const QString &id);

	void autoEnhance();
	void autoReady(EditState target);
	void applyAuto(int amount, bool commitNow);

	void updateLayout();
	void updateTopBar();
	[[nodiscard]] int layersNaturalHeight() const;
	void refreshPagesFade();
	[[nodiscard]] bool narrow() const;
	[[nodiscard]] QRect topBarRect() const;
	[[nodiscard]] bool handleToolKey(not_null<QKeyEvent*> e);
	[[nodiscard]] bool importFrom(not_null<const QMimeData*> data);

	void setExporting(bool exporting, QString text = QString());
	void exportResult(Fn<void(PhotoEditorResult)> callback);
	void done();
	void saveToFile(bool closeAfter);
	void copyToClipboard();
	void showMoreMenu();
	void runAction(const PhotoEditorAction &action);
	void finish(Fn<void()> after);
	void toast(const QString &text);

	const std::shared_ptr<Ui::Show> _outerShow;
	PhotoEditorOptions _options;
	QSize _sourceSize;
	std::unique_ptr<Ui::LayerManager> _layers;
	std::unique_ptr<Controller> _controller;

	QImage _thumbSource;
	bool _hasAlpha = false;
	bool _sourceReady = false;
	int _previewSide = 0;
	uint64 _analysisRevision = 0;
	bool _analysisRunning = false;

	// "Remove background" replaces the pixels of an image layer with the
	// cutout that remembers what it was made from, so the button (and
	// undo) brings the background back.
	rpl::variable<bool> _backgroundRemoved = false;

	// The system can't be stopped once it was asked for a cutout. A
	// cancelled request works on and its result is dropped, unless the
	// button was pressed again meanwhile: then that result is awaited
	// instead of asking twice, if it is of the same layer. If the button
	// was pressed for another layer, the request for that one starts when
	// the running one is over: one request at a time. The result is
	// applied only to the layer and the pixels it is wanted for.
	bool _removingBackground = false;
	bool _backgroundRequested = false;
	int _backgroundToken = 0;
	crl::time _backgroundStarted = 0;
	LayerId _backgroundWantedId = 0;
	std::shared_ptr<const ImageContent> _backgroundWanted;

	// Document::global of the working document (the state itself while
	// the picture is still being loaded).
	EditState _state;
	rpl::event_stream<> _refreshControls;
	rpl::event_stream<> _refreshCanvasSize;

	Tab _tab = Tab::Adjust;
	int _ratioIndex = kRatioFree;
	bool _layingOut = false;
	bool _comparing = false;
	bool _exporting = false;
	bool _controllerBusy = false;
	bool _finished = false;
	std::optional<AutoState> _auto;
	bool _autoApplying = false;
	bool _autoBusy = false;
	std::vector<EffectType> _effectsBuilt;

	ToolButton *_close = nullptr;
	ToolButton *_undo = nullptr;
	ToolButton *_redo = nullptr;
	ToolButton *_compare = nullptr;
	ToolButton *_reset = nullptr;
	ToolButton *_more = nullptr;
	Ui::RoundButton *_done = nullptr;
	int _titleRight = 0;

	ToolStrip *_toolStrip = nullptr;
	Ui::RpWidget *_panel = nullptr;
	TabBar *_tabs = nullptr;
	std::vector<Page> _pages;
	// A fade over the bottom edge of the page while it continues below:
	// the scroll bar shows up only under the mouse, and a page cut in the
	// middle of a control gave no hint that there is more. A smaller one
	// over the top edge of a page that was scrolled.
	Ui::RpWidget *_pagesFade = nullptr;
	Ui::RpWidget *_pagesFadeAbove = nullptr;
	Ui::RpWidget *_layerPanel = nullptr;
	Ui::RpWidget *_layersPanel = nullptr;
	int _layersTop = 0;
	int _layersNatural = 0;
	bool _toolHasOptions = false;
	ChipsFlow *_ratios = nullptr;
	ValueSlider *_straighten = nullptr;
	Ui::VerticalLayout *_effectsList = nullptr;
	PanelButton *_autoButton = nullptr;
	rpl::variable<QString> _filterName;
	rpl::variable<QString> _autoButtonText;
	rpl::variable<bool> _autoAvailable = false;
	rpl::variable<bool> _effectsEmpty = true;

	Canvas *_canvas = nullptr;
	FilterStrip *_strip = nullptr;
	BusyOverlay *_busy = nullptr;
	base::unique_qptr<Ui::PopupMenu> _menu;

	std::unique_ptr<PreviewRenderer> _renderer;
	std::unique_ptr<PreviewRenderer> _beforeRenderer;
	base::Timer _detailTimer;
	base::Timer _thumbsTimer;
	RenderKey _fastKey;
	QSize _fastSize;
	uint64 _fastId = 0;
	RenderKey _detailKey;
	QSize _detailSize;
	uint64 _detailId = 0;
	// The last detail render is still in progress or is on the screen and
	// nothing requested after it can replace it.
	bool _detailValid = false;
	EditState _beforeState;
	QSize _beforeSize;
	QSize _beforeCanvas;
	uint64 _beforeId = 0;
	bool _beforeValid = false;
	uint64 _shownId = 0;
	RenderKey _shownKey;
	QSize _shownSize;
	bool _shownAny = false;
	std::optional<EditState> _thumbsState;
	bool _thumbsDirty = true;
	uint64 _thumbsGeneration = 0;

	// Snapshot scenes: when everything was first seen ready, the picture
	// is taken a little later (see snapshotReady()).
	mutable crl::time _snapshotSettledFrom = 0;

	rpl::event_stream<> _closeRequests;

};

Editor::Editor(
	QWidget *parent,
	std::shared_ptr<Ui::Show> show,
	QImage image,
	PhotoEditorOptions options)
: RpWidget(parent)
, _outerShow(std::move(show))
, _options(std::move(options))
, _sourceSize((_options.document && !_options.document->empty())
	? _options.document->size
	: image.size())
, _layers(std::make_unique<Ui::LayerManager>(this))
, _controller(std::make_unique<Controller>(_layers->uiShow()))
, _state((_options.document && !_options.document->empty())
	? Normalized(_options.document->global)
	: Normalized(_options.state))
, _tab(_options.tab)
, _renderer(std::make_unique<PreviewRenderer>(
	_controller->compositor(),
	[=](PreviewRenderer::Result result) {
		previewReady(std::move(result));
	}))
, _beforeRenderer(std::make_unique<PreviewRenderer>(
	_controller->compositor(),
	[=](PreviewRenderer::Result result) {
		beforeReady(std::move(result));
	}))
, _detailTimer([=] { requestDetail(); })
, _thumbsTimer([=] { refreshThumbnails(); }) {
	setFocusPolicy(Qt::StrongFocus);
	setAcceptDrops(true);
	_autoButtonText = tr::lng_oblivion_photo_ui_auto_button(tr::now);
	if (_tab == Tab::Tool || _tab == Tab::Layers) {
		_tab = Tab::Layer;
	}
	inferRatio();

	setupCanvas();
	setupStrip();
	_toolStrip = Ui::CreateChild<ToolStrip>(this, _controller.get());
	_toolStrip->show();
	setupPanel();
	setupTopBar();
	setupShortcuts();
	_busy = Ui::CreateChild<BusyOverlay>(this);
	setupController();

	_layers->layerShownValue() | rpl::filter(
		!rpl::mappers::_1
	) | rpl::on_next([=] {
		if (!_exporting) {
			setFocus();
		}
	}, lifetime());

	// Under the photo the panel height follows the current page.
	for (auto i = 0; i != int(_pages.size()); ++i) {
		_pages[i].content->heightValue(
		) | rpl::skip(1) | rpl::on_next([=] {
			if (narrow() && int(_tab) == i) {
				updateLayout();
			}
		}, _pages[i].content->lifetime());
	}

	stateChanged();
	selectTab(_tab);
	refreshHistoryButtons();
	startLoading(std::move(image));
}

Editor::~Editor() {
	keepInterrupted();

	// The tools and the panels work with the controller, some of them
	// in their destructors: they go first, while everything is alive.
	_controller->shutdown();
	_menu = nullptr;
	delete base::take(_toolStrip);
	delete base::take(_panel);
}

// The editor is destroyed without finish(): not by the user, with the
// layers of its window (see PhotoEditorOptions::interrupted). Only the
// document is copied here, its heavy data is shared. Nothing of the
// window or the session may be touched, they may be half destroyed.
void Editor::keepInterrupted() {
	if (_finished
		|| !_options.interrupted
		|| !_sourceReady
		|| !_controller->hasDocument()
		|| !dirty()
		|| Core::Quitting()) {
		return;
	}
	const auto id = KeepInterruptedPhotoEdit({
		.document = std::make_shared<const Document>(
			_controller->document()),
		.fileName = _options.fileName,
	});
	if (id) {
		crl::on_main([id, callback = _options.interrupted] {
			callback(id);
		});
	}
}

rpl::producer<> Editor::closeRequests() const {
	return _closeRequests.events();
}

not_null<Controller*> Editor::controller() const {
	return _controller.get();
}

void Editor::setupController() {
	_controller->setView({
		.documentToWidget = [=] { return _canvas->documentToWidget(); },
		.update = [=] { _canvas->update(); },
	});
	_canvas->setToolProvider([=] { return _controller->tool(); });
	_canvas->viewChanges() | rpl::on_next([=] {
		_controller->notifyViewChanged();
	}, _canvas->lifetime());

	_controller->documentChanges() | rpl::on_next([=] {
		documentChanged();
	}, lifetime());
	_controller->historyChanges() | rpl::on_next([=] {
		refreshHistoryButtons();
	}, lifetime());
	_controller->toolChanges() | rpl::on_next([=] {
		_canvas->toolChanged();
	}, lifetime());
	_controller->toolValue() | rpl::on_next([=] {
		rebuildToolPage();
	}, lifetime());
	_controller->toolOptionsRequests() | rpl::on_next([=] {
		if (_toolHasOptions && !_exporting) {
			selectTab(Tab::Tool);
		}
	}, lifetime());
	_controller->layerEffectsRequests() | rpl::on_next([=] {
		showLayerEffects();
	}, lifetime());
	_controller->revealRequests(
	) | rpl::on_next([=](QPointer<QWidget> widget) {
		// The section may be just appearing: after the page is laid out.
		crl::on_main(this, [=] {
			revealInPanel(widget.data());
		});
	}, lifetime());
	_canvas->toolDragValue() | rpl::on_next([=](bool dragging) {
		_controller->setInteracting(dragging);
	}, _canvas->lifetime());
	_controller->activeLayerValue() | rpl::skip(1) | rpl::on_next([=] {
		refreshBackgroundState();
		// The picture under another layer is worth keeping now.
		refreshPreview(false);
	}, lifetime());
	_controller->busyValue() | rpl::on_next([=](const QString &text) {
		if (!text.isEmpty()) {
			if (!_exporting) {
				_controllerBusy = true;
				setExporting(true, text);
			}
		} else if (_controllerBusy) {
			_controllerBusy = false;
			setExporting(false);
		}
	}, lifetime());
}

void Editor::setupCanvas() {
	_canvas = Ui::CreateChild<Canvas>(this);
	_canvas->setLoading(true);
	_canvas->setCheckerboard(true);
	_canvas->setCrop(_state.crop);
	_canvas->setCropRatio(cropRatio());
	_canvas->show();

	_canvas->viewChanges() | rpl::on_next([=] {
		refreshPreview(false);
	}, _canvas->lifetime());

	_canvas->cropChanges() | rpl::on_next([=](QRectF crop) {
		auto state = _state;
		state.crop = crop;
		apply(std::move(state), false);
	}, _canvas->lifetime());

	_canvas->cropFinished() | rpl::on_next([=] {
		commit();
	}, _canvas->lifetime());
}

void Editor::setupStrip() {
	_strip = Ui::CreateChild<FilterStrip>(this);
	auto items = std::vector<StripItem>();
	for (const auto &id : FilterIds()) {
		items.push_back({ .id = id, .name = FilterName(id) });
	}
	_strip->setItems(std::move(items));
	_strip->setSelected(_state.filter, anim::type::instant);
	_strip->selections() | rpl::on_next([=](QString id) {
		selectFilter(id);
	}, _strip->lifetime());
	_strip->hide();
}

void Editor::setupShortcuts() {
	using Command = Shortcuts::Command;
	// Cmd+0 is the app-wide "Saved Messages" shortcut, so it never comes
	// to keyPressEvent(). While the editor has the focus it fits the photo.
	Shortcuts::Requests(
	) | rpl::filter([=] {
		return isActiveWindow()
			&& Ui::InFocusChain(this)
			&& !_layers->topShownLayer();
	}) | rpl::on_next([=](not_null<Shortcuts::Request*> request) {
		request->check(Command::ChatSelf, 1) && request->handle([=] {
			if (!_exporting) {
				_canvas->zoomFit();
			}
			return true;
		});
	}, lifetime());
}

void Editor::setupTopBar() {
	const auto size = Px(kTopButton);
	const auto button = [&](IconRef icon, QString tooltip) {
		const auto result = Ui::CreateChild<ToolButton>(this, icon, size);
		result->setTooltip(std::move(tooltip));
		result->show();
		return result;
	};
	_close = button(
		{ &st::infoTopBarClose.icon },
		WithShortcut(
			tr::lng_oblivion_photo_ui_close(tr::now),
			QKeySequence(Qt::Key_Escape)));
	_close->setClickedCallback([=] { requestClose(); });

	_undo = button(
		{ &st::photoEditorUndoButton.icon },
		WithShortcut(
			tr::lng_oblivion_photo_ui_undo(tr::now),
			QKeySequence(QKeySequence::Undo)));
	_undo->setClickedCallback([=] { undo(); });

	_redo = button(
		{ &st::photoEditorRedoButton.icon },
		WithShortcut(
			tr::lng_oblivion_photo_ui_redo(tr::now),
			QKeySequence(QKeySequence::Redo)));
	_redo->setClickedCallback([=] { redo(); });

	_compare = button(
		{ &st::menuIconShowInChat },
		WithShortcut(
			tr::lng_oblivion_photo_ui_compare(tr::now),
			QKeySequence(Qt::Key_Backslash)));
	_compare->events() | rpl::on_next([=](not_null<QEvent*> e) {
		const auto type = e->type();
		if (type == QEvent::MouseButtonPress
			|| type == QEvent::MouseButtonDblClick) {
			setComparing(true);
		} else if (type == QEvent::MouseButtonRelease) {
			setComparing(false);
		}
	}, _compare->lifetime());

	_reset = button(
		{ &st::menuIconRestore },
		tr::lng_oblivion_photo_ui_reset_all(tr::now));
	_reset->setClickedCallback([=] {
		_ratioIndex = kRatioFree;
		if (_ratios) {
			_ratios->setSelected(_ratioIndex);
		}
		_canvas->setCropRatio(cropRatio());
		if (!_controller->hasDocument()) {
			apply(EditState(), true);
			return;
		}
		// Back to what was opened, without the whole-image edits.
		auto document = _controller->initialDocument();
		document.global = EditState();
		_controller->apply(std::move(document), true);
	});

	_more = button(
		{ &st::infoTopBarMenuActive },
		tr::lng_oblivion_photo_ui_more(tr::now));
	_more->setClickedCallback([=] { showMoreMenu(); });

	auto doneText = !_options.done
		? tr::lng_oblivion_photo_ui_save()
		: _options.doneText.isEmpty()
		? tr::lng_oblivion_photo_ui_done()
		: rpl::producer<QString>(rpl::single(_options.doneText));
	_done = Ui::CreateChild<Ui::RoundButton>(
		this,
		std::move(doneText),
		st::defaultActiveButton);
	_done->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	_done->setFullRadius(true);
	_done->setClickedCallback([=] { done(); });
	_done->setDisabled(true);
	_done->show();
	_done->widthValue() | rpl::on_next([=] {
		updateTopBar();
	}, _done->lifetime());
}

Editor::Page Editor::createPage() {
	const auto scroll = Ui::CreateChild<Ui::ScrollArea>(
		_panel,
		PanelScrollStyle());
	const auto content = scroll->setOwnedWidget(
		object_ptr<Ui::VerticalLayout>(scroll)).data();
	scroll->hide();
	return { .scroll = scroll, .content = content };
}

not_null<SectionTitle*> Editor::addTitle(
		not_null<Ui::VerticalLayout*> page,
		rpl::producer<QString> text,
		int top) {
	return page->add(
		object_ptr<SectionTitle>(page, std::move(text)),
		RowMargins(top));
}

void Editor::addHint(
		not_null<Ui::VerticalLayout*> page,
		rpl::producer<QString> text) {
	page->add(
		object_ptr<Ui::FlatLabel>(page, std::move(text), HintLabelStyle()),
		RowMargins(Px(kHintTop)));
}

void Editor::setupPanel() {
	_panel = Ui::CreateChild<Ui::RpWidget>(this);
	_panel->show();
	_panel->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(_panel);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::groupCallMembersBg);
		const auto radius = Px(kPanelRadius);
		p.drawRoundedRect(_panel->rect(), radius, radius);
		if (_layersTop > 0) {
			// The layers list is a part of the same plate, under a line.
			const auto line = std::max(Px(1), 1);
			p.fillRect(
				QRect(
					Px(kPanelPadding),
					_layersTop - Px(kLayersSkip) / 2,
					_panel->width() - 2 * Px(kPanelPadding),
					line),
				st::groupCallMembersBgOver);
		}
	}, _panel->lifetime());

	auto tabs = std::vector<TabInfo>();
	tabs.push_back({
		tr::lng_oblivion_photo_ui_tab_crop(),
		IconRef{ .paint = PaintCropIcon },
	});
	tabs.push_back({
		tr::lng_oblivion_photo_ui_tab_adjust(),
		{ &st::menuIconCustomize },
	});
	tabs.push_back({
		tr::lng_oblivion_photo_ui_tab_filters(),
		{ &st::menuIconPalette },
	});
	tabs.push_back({
		tr::lng_oblivion_photo_ui_tab_effects(),
		{ &st::menuIconUnique },
	});
	tabs.push_back({
		tr::lng_oblivion_photo_ui_tab_auto(),
		{ &st::aiComposeTabStyleIcon },
	});
	tabs.push_back({
		tr::lng_oblivion_photo_panel_tab_layer(),
		IconRef{ .paint = [](QPainter &p, QRectF rect, QColor color) {
			PaintSheetsIcon(p, rect, color, 2);
		} },
	});
	tabs.push_back({
		tr::lng_oblivion_photo_panel_tab_tool(),
		{ &st::menuIconEdit },
	});
	tabs.push_back({
		tr::lng_oblivion_photo_panel_tab_layers(),
		IconRef{ .paint = [](QPainter &p, QRectF rect, QColor color) {
			PaintSheetsIcon(p, rect, color, 3);
		} },
	});
	_tabs = Ui::CreateChild<TabBar>(_panel, std::move(tabs));
	_tabs->setTabVisible(int(Tab::Tool), false);
	_tabs->setTabVisible(int(Tab::Layers), false);
	_tabs->setActive(int(_tab), anim::type::instant);
	_tabs->show();
	_tabs->activeChanges() | rpl::on_next([=](int index) {
		if (Tab(index) != _tab) {
			selectTab(Tab(index));
		}
	}, _tabs->lifetime());

	for (auto i = 0; i != kTabCount; ++i) {
		_pages.push_back(createPage());
	}
	setupCropPage(_pages[int(Tab::Crop)].content);
	setupAdjustPage(_pages[int(Tab::Adjust)].content);
	setupFiltersPage(_pages[int(Tab::Filters)].content);
	setupEffectsPage(_pages[int(Tab::Effects)].content);
	setupAutoPage(_pages[int(Tab::Auto)].content);
	const auto layerPage = _pages[int(Tab::Layer)].content;
	_layerPanel = layerPage->add(
		CreateLayerPanel(layerPage, _controller.get()));
	for (auto i = 0; i != kTabCount; ++i) {
		if (i != int(Tab::Tool) && i != int(Tab::Layers)) {
			_pages[i].content->add(object_ptr<Ui::FixedHeightWidget>(
				_pages[i].content,
				Px(kPageBottom)));
		}
	}

	const auto makeFade = [&](bool above) {
		const auto result = Ui::CreateChild<Ui::RpWidget>(_panel);
		result->setAttribute(Qt::WA_TransparentForMouseEvents);
		result->hide();
		result->paintRequest() | rpl::on_next([=] {
			auto p = QPainter(result);
			const auto color = st::groupCallMembersBg->c;
			auto gradient = QLinearGradient(0, 0, 0, result->height());
			gradient.setColorAt(above ? 1. : 0., anim::with_alpha(color, 0.));
			gradient.setColorAt(above ? 0. : 1., color);
			p.fillRect(result->rect(), gradient);
		}, result->lifetime());
		return result;
	};
	_pagesFade = makeFade(false);
	_pagesFadeAbove = makeFade(true);
	for (const auto &page : _pages) {
		rpl::merge(
			page.scroll->scrolls(),
			page.scroll->innerResizes(),
			page.scroll->geometryChanged()
		) | rpl::on_next([=] {
			refreshPagesFade();
		}, page.scroll->lifetime());
	}

	auto layers = CreateLayersPanel(_panel, _controller.get());
	_layersPanel = layers.release();
	_layersPanel->show();
}

// The page of the current tab goes on under its bottom edge (or above
// the top one, when it was scrolled).
void Editor::refreshPagesFade() {
	if (!_pagesFade || !_pagesFadeAbove || _pages.empty()) {
		return;
	}
	const auto scroll = _pages[int(_tab)].scroll;
	const auto shown = (_tab != Tab::Layers) && !scroll->isHidden();
	const auto top = scroll->scrollTop();
	const auto below = shown && (top + Px(2) < scroll->scrollTopMax());
	const auto above = shown && (top > Px(2));
	// Not over the round corners of the plate.
	const auto inset = Px(kPanelRadius) / 2;
	const auto fadeWidth = std::max(scroll->width() - 2 * inset, 1);
	if (below) {
		const auto fade = std::min(Px(kPagesFade), scroll->height() / 2);
		_pagesFade->setGeometry(
			scroll->x() + inset,
			scroll->y() + scroll->height() - fade,
			fadeWidth,
			fade);
	}
	if (above) {
		const auto fade = std::min(
			Px(kPagesFade) / 2,
			scroll->height() / 4);
		_pagesFadeAbove->setGeometry(
			scroll->x() + inset,
			scroll->y(),
			fadeWidth,
			fade);
	}
	_pagesFade->setVisible(below);
	_pagesFadeAbove->setVisible(above);
}

// What the layers list needs to show all its rows, 0: it didn't say.
int Editor::layersNaturalHeight() const {
	auto chosen = (const PanelDescriptor*)(nullptr);
	for (const auto descriptor : AllPanels()) {
		if (descriptor->slot == PanelSlot::Layers) {
			chosen = descriptor;
		}
	}
	return (chosen && chosen->height)
		? std::max(chosen->height(_controller.get()), 0)
		: 0;
}

void Editor::rebuildToolPage() {
	if (_pages.empty()) {
		return;
	}
	const auto page = _pages[int(Tab::Tool)].content;
	page->clear();
	const auto descriptor = FindTool(_controller->toolId());
	_toolHasOptions = descriptor && descriptor->options;
	if (_toolHasOptions) {
		addTitle(page, rpl::single(descriptor->name.now()));
		page->add(descriptor->options(page, _controller.get()));
		page->add(object_ptr<Ui::FixedHeightWidget>(page, Px(kPageBottom)));
		page->resizeToWidth(_panel->width());
	}
	_tabs->setTabVisible(int(Tab::Tool), _toolHasOptions);
	if (_toolHasOptions) {
		selectTab(Tab::Tool);
	} else if (_tab == Tab::Tool
		|| (_tab == Tab::Crop && _controller->toolId() != kViewTool)) {
		// A tool can't work while the crop frame owns the canvas.
		selectTab(Tab::Layer);
	} else {
		updateLayout();
	}
}

// The whole-picture tabs lead to the effects of the active layer: the
// fine adjustments (curves, color ranges...) from the Adjust tab, the
// blurs, distortions and the other packs from the Effects tab. The chosen
// effect is added to the layer and shown on the Layer tab.
void Editor::addLayerFxSection(
		not_null<Ui::VerticalLayout*> page,
		rpl::producer<QString> title,
		rpl::producer<QString> about,
		rpl::producer<QString> button,
		std::vector<FxGroup> groups,
		bool flat) {
	addTitle(page, std::move(title), Px(kLargeSkip));
	page->add(
		object_ptr<Ui::FlatLabel>(page, std::move(about), HintLabelStyle()),
		RowMargins());
	const auto add = page->add(
		object_ptr<PanelButton>(page, std::move(button), false),
		RowMargins(Px(kSkip)));
	add->setClickedCallback([=] {
		if (_exporting || !_sourceReady) {
			return;
		}
		const auto weak = base::make_weak(this);
		ShowAddFxMenu(
			this,
			add->mapToGlobal(QPoint(0, add->height())),
			[=](std::vector<FxInstance> stack) {
				const auto strong = weak.get();
				if (strong
					&& !strong->_exporting
					&& AppendLayerFx(
						strong->_controller.get(),
						strong->_controller->activeLayerId(),
						std::move(stack))) {
					strong->showLayerEffects();
				}
			},
			groups,
			flat);
	});
}

void Editor::showLayerEffects() {
	if (_exporting || _pages.empty()) {
		return;
	}
	selectTab(Tab::Layer);
	// The effects are the last section of the Layer page, the new card is
	// the last one of them: it is there after the page was laid out. Its
	// top is shown, a card may be taller than the page and scrolling to
	// the very end left it without its header.
	const auto scroll = _pages[int(Tab::Layer)].scroll;
	crl::on_main(this, [=] {
		if (_tab != Tab::Layer) {
			return;
		}
		const auto top = _layerPanel
			? LayerPanelLastFxTop(_layerPanel)
			: -1;
		const auto max = scroll->scrollTopMax();
		scroll->scrollToY((top >= 0)
			? std::clamp(_layerPanel->y() + top - Px(kSkip), 0, max)
			: max);
	});
}

void Editor::revealInPanel(QWidget *widget) {
	if (!widget || _exporting || _tab == Tab::Layers) {
		return;
	}
	const auto &page = _pages[int(_tab)];
	if (!page.content->isAncestorOf(widget)) {
		return;
	}
	const auto top = widget->mapTo(page.content, QPoint()).y();
	page.scroll->scrollToY(top, top + widget->height());
}

void Editor::setupCropPage(not_null<Ui::VerticalLayout*> page) {
	addTitle(page, tr::lng_oblivion_photo_ui_aspect());
	_ratios = page->add(object_ptr<ChipsFlow>(page), RowMargins());
	for (auto i = 0; i != int(kRatioPresets.size()); ++i) {
		const auto preset = kRatioPresets[i];
		auto text = (preset.width == 0)
			? tr::lng_oblivion_photo_ui_aspect_free()
			: (preset.width < 0)
			? tr::lng_oblivion_photo_ui_aspect_original()
			: rpl::producer<QString>(rpl::single(
				QString::number(preset.width)
					+ ':'
					+ QString::number(preset.height)));
		_ratios->addChip(std::move(text), [=] { selectRatio(i); });
	}
	_ratios->setSelected(_ratioIndex);

	addTitle(page, tr::lng_oblivion_photo_ui_rotate_section(), Px(kSkip));
	const auto rowMargins = style::margins(
		Px(kRowButtonPadding),
		0,
		Px(kRowButtonPadding),
		0);
	const auto addRow = [&](
			rpl::producer<QString> text,
			IconRef icon,
			Fn<void()> callback) {
		const auto row = page->add(
			object_ptr<RowButton>(page, std::move(text), icon),
			rowMargins);
		row->setClickedCallback(std::move(callback));
	};
	const auto rotateIcon = &st::photoEditorRotateButton.icon;
	const auto flipIcon = &st::photoEditorFlipButton.icon;
	addRow(
		tr::lng_oblivion_photo_ui_rotate_left(),
		{ .icon = rotateIcon, .mirrored = true },
		[=] { rotate(false); });
	addRow(
		tr::lng_oblivion_photo_ui_rotate_right(),
		{ .icon = rotateIcon },
		[=] { rotate(true); });
	addRow(
		tr::lng_oblivion_photo_ui_flip_horizontal(),
		{ .icon = flipIcon },
		[=] { flip(true); });
	addRow(
		tr::lng_oblivion_photo_ui_flip_vertical(),
		{ .icon = flipIcon, .rotation = 90 },
		[=] { flip(false); });

	const auto limit = kStraightenMax * kStraightenStep;
	_straighten = page->add(
		object_ptr<ValueSlider>(page, SliderArgs{
			.label = tr::lng_oblivion_photo_ui_straighten(),
			.min = -limit,
			.max = limit,
			.defaultValue = 0,
			.value = int(std::round(_state.straighten * kStraightenStep)),
			.format = DegreesText,
		}),
		RowMargins(Px(kSkip)));
	_straighten->changes() | rpl::on_next([=](SliderChange change) {
		auto state = _state;
		state.straighten = change.value / float64(kStraightenStep);
		_canvas->setGridVisible(!change.finished);
		apply(std::move(state), change.finished);
	}, _straighten->lifetime());
	_refreshControls.events() | rpl::on_next([=] {
		_straighten->setValue(
			int(std::round(_state.straighten * kStraightenStep)));
	}, _straighten->lifetime());

	const auto reset = page->add(
		object_ptr<PanelButton>(
			page,
			tr::lng_oblivion_photo_ui_reset_crop(),
			false),
		RowMargins(Px(kLargeSkip)));
	reset->setClickedCallback([=] { resetGeometry(); });
	_refreshControls.events() | rpl::on_next([=] {
		reset->setAvailable(HasGeometry(_state));
	}, reset->lifetime());

	addHint(page, tr::lng_oblivion_photo_ui_crop_hint());
	setupCanvasSection(page);
}

// The canvas of the whole document: its size can be typed (or dragged)
// and extended to an aspect ratio, so there is room around the photo for
// the other layers. See CanvasResized() in oblivion_photo_doc.h.
void Editor::setupCanvasSection(not_null<Ui::VerticalLayout*> page) {
	addTitle(page, tr::lng_oblivion_photo_panel_canvas_title(), Px(kLargeSkip));
	const auto current = [=] {
		const auto size = _controller->hasDocument()
			? _controller->document().size
			: _sourceSize;
		auto result = FxParams();
		result.set("width", FxValue::Integer(size.width()));
		result.set("height", FxValue::Integer(size.height()));
		return result;
	};
	const auto initial = _sourceSize.isEmpty()
		? QSize(kCanvasMinSide, kCanvasMinSide)
		: _sourceSize;
	// A photo that was opened may be larger than what can be asked for.
	const auto limit = std::max({
		kCanvasMaxSide,
		initial.width(),
		initial.height(),
	});
	page->add(
		CreateParamsPanel(page, ParamsPanelArgs{
			.params = {
				FxInt(
					"width",
					tr::lng_oblivion_photo_panel_canvas_width,
					kCanvasMinSide,
					limit,
					initial.width(),
					u" px"_q),
				FxInt(
					"height",
					tr::lng_oblivion_photo_panel_canvas_height,
					kCanvasMinSide,
					limit,
					initial.height(),
					u" px"_q),
			},
			.values = current(),
			.updates = rpl::merge(
				_controller->documentChanges(),
				_refreshCanvasSize.events()
			) | rpl::map(current),
			.changed = [=](
					const QByteArray &id,
					FxValue value,
					bool finished) {
				if (!finished || !_controller->hasDocument()) {
					return;
				}
				auto size = _controller->document().size;
				if (id == "width") {
					size.setWidth(value.integer());
				} else {
					size.setHeight(value.integer());
				}
				resizeCanvas(size);
			},
			.commitOnRelease = true,
			// 16 .. 16384: on an even scale the knob of any usual photo
			// stood at the very start of the track.
			.logarithmic = { QByteArray("width"), QByteArray("height") },
		}),
		RowMargins());
	page->add(
		object_ptr<Ui::FlatLabel>(
			page,
			tr::lng_oblivion_photo_panel_canvas_extend(),
			HintLabelStyle()),
		RowMargins(Px(kSkip)));
	const auto chips = page->add(
		object_ptr<ChipsFlow>(page),
		RowMargins(Px(kSkip)));
	for (const auto &preset : kRatioPresets) {
		if (preset.width <= 0) {
			continue;
		}
		const auto width = preset.width;
		const auto height = preset.height;
		chips->addChip(
			rpl::single(QString::number(width) + ':' + QString::number(height)),
			[=] {
				if (_controller->hasDocument()) {
					resizeCanvas(CanvasSizeForAspect(
						_controller->document().size,
						width,
						height));
				}
			});
	}
	addHint(page, tr::lng_oblivion_photo_panel_canvas_about());
}

void Editor::resizeCanvas(QSize size) {
	// A value that was not taken as it was (a limit, the editor is busy)
	// is replaced with the real one. Later: a slider that reports the end
	// of its drag doesn't take values from outside yet.
	crl::on_main(this, [=] {
		_refreshCanvasSize.fire({});
	});
	if (_exporting || !_sourceReady || !_controller->hasDocument()) {
		return;
	}
	const auto valid = ValidCanvasSize(size);
	if (valid == _controller->document().size) {
		return;
	}
	// The crop frame is reset with the canvas, its ratio too.
	_ratioIndex = kRatioFree;
	if (_ratios) {
		_ratios->setSelected(_ratioIndex);
	}
	_canvas->setCropRatio(cropRatio());
	_controller->apply(
		CanvasResized(_controller->document(), valid),
		true);
}

void Editor::setupAdjustPage(not_null<Ui::VerticalLayout*> page) {
	struct Group {
		rpl::producer<QString> title;
		std::vector<Adjust> ids;
	};
	auto groups = std::vector<Group>();
	groups.push_back({
		tr::lng_oblivion_photo_ui_group_light(),
		{
			Adjust::Exposure,
			Adjust::Brightness,
			Adjust::Contrast,
			Adjust::Highlights,
			Adjust::Shadows,
			Adjust::Whites,
			Adjust::Blacks,
			Adjust::Fade,
		},
	});
	groups.push_back({
		tr::lng_oblivion_photo_ui_group_color(),
		{
			Adjust::Temperature,
			Adjust::Tint,
			Adjust::Saturation,
			Adjust::Vibrance,
		},
	});
	groups.push_back({
		tr::lng_oblivion_photo_ui_group_details(),
		{ Adjust::Clarity, Adjust::Sharpen, Adjust::Blur },
	});
	groups.push_back({
		tr::lng_oblivion_photo_ui_group_finish(),
		{
			Adjust::Vignette,
			Adjust::VignetteFeather,
			Adjust::Grain,
			Adjust::GrainSize,
		},
	});
	auto first = true;
	for (auto &group : groups) {
		const auto title = addTitle(
			page,
			std::move(group.title),
			first ? 0 : Px(kSkip));
		first = false;
		const auto ids = group.ids;
		title->setAction(tr::lng_oblivion_photo_ui_reset(), [=] {
			auto state = _state;
			for (const auto id : ids) {
				state.setValue(id, AdjustDescriptor(id).defaultValue);
			}
			apply(std::move(state), true);
		});
		_refreshControls.events() | rpl::on_next([=] {
			const auto changed = ranges::any_of(ids, [&](Adjust id) {
				return _state.value(id) != AdjustDescriptor(id).defaultValue;
			});
			title->setActionVisible(changed);
		}, title->lifetime());

		for (const auto id : ids) {
			const auto &info = AdjustDescriptor(id);
			const auto track = (id == Adjust::Temperature)
				? SliderTrack::Temperature
				: (id == Adjust::Tint)
				? SliderTrack::Tint
				: SliderTrack::Plain;
			const auto slider = page->add(
				object_ptr<ValueSlider>(page, SliderArgs{
					.label = LangValue([=] { return AdjustName(id); }),
					.min = info.min,
					.max = info.max,
					.defaultValue = info.defaultValue,
					.value = _state.value(id),
					.format = (info.min < 0)
						? Fn<QString(int)>(FormatSigned)
						: Fn<QString(int)>(),
					.track = track,
				}),
				RowMargins());
			slider->changes() | rpl::on_next([=](SliderChange change) {
				auto state = _state;
				state.setValue(id, change.value);
				apply(std::move(state), change.finished);
			}, slider->lifetime());
			_refreshControls.events() | rpl::on_next([=] {
				slider->setValue(_state.value(id));
				slider->setDimmed(
					(id == Adjust::VignetteFeather && !_state.vignette)
					|| (id == Adjust::GrainSize && !_state.grain));
			}, slider->lifetime());
		}
	}
	addHint(page, tr::lng_oblivion_photo_ui_adjust_hint());
	addLayerFxSection(
		page,
		tr::lng_oblivion_photo_panel_fine_title(),
		tr::lng_oblivion_photo_panel_fine_about(),
		tr::lng_oblivion_photo_panel_fine_button(),
		{ FxGroup::Light, FxGroup::Color, FxGroup::Detail, FxGroup::Finish },
		true);
}

void Editor::setupFiltersPage(not_null<Ui::VerticalLayout*> page) {
	const auto title = addTitle(page, _filterName.value());
	title->setAction(tr::lng_oblivion_photo_ui_reset(), [=] {
		selectFilter(kOriginalFilter);
	});
	const auto intensity = page->add(
		object_ptr<ValueSlider>(page, SliderArgs{
			.label = tr::lng_oblivion_photo_ui_intensity(),
			.min = 0,
			.max = 100,
			.defaultValue = 100,
			.value = _state.filterIntensity,
		}),
		RowMargins());
	intensity->changes() | rpl::on_next([=](SliderChange change) {
		auto state = _state;
		state.filterIntensity = change.value;
		apply(std::move(state), change.finished);
	}, intensity->lifetime());
	_refreshControls.events() | rpl::on_next([=] {
		const auto original = (_state.filter == kOriginalFilter)
			|| !FilterExists(_state.filter);
		_filterName = FilterName(_state.filter);
		title->setActionVisible(!original);
		intensity->setValue(_state.filterIntensity);
		intensity->setDimmed(original);
		_strip->setSelected(_state.filter, anim::type::normal);
	}, intensity->lifetime());
	addHint(page, tr::lng_oblivion_photo_ui_filters_hint());
}

void Editor::setupEffectsPage(not_null<Ui::VerticalLayout*> page) {
	const auto background = Vision::BackgroundRemovalSupported();
	if (background) {
		addTitle(page, tr::lng_oblivion_vision_editor_title());
		const auto toggle = page->add(
			object_ptr<PanelButton>(
				page,
				_backgroundRemoved.value(
				) | rpl::map([](bool removed) {
					return removed
						? tr::lng_oblivion_vision_editor_restore()
						: tr::lng_oblivion_vision_editor_remove();
				}) | rpl::flatten_latest(),
				false),
			RowMargins());
		toggle->setClickedCallback([=] { toggleBackground(); });
		addHint(page, tr::lng_oblivion_vision_editor_hint());
	}
	const auto applied = addTitle(
		page,
		tr::lng_oblivion_photo_ui_effects_applied(),
		background ? Px(kLargeSkip) : 0);
	applied->setAction(tr::lng_oblivion_photo_ui_effects_clear(), [=] {
		auto state = _state;
		state.effects.clear();
		apply(std::move(state), true);
	});
	_effectsEmpty.value() | rpl::on_next([=](bool empty) {
		applied->setActionVisible(!empty);
	}, applied->lifetime());

	const auto empty = page->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			page,
			object_ptr<Ui::FlatLabel>(
				page,
				tr::lng_oblivion_photo_ui_effects_empty(),
				HintLabelStyle()),
			RowMargins()));
	empty->toggleOn(_effectsEmpty.value(), anim::type::instant);

	_effectsList = page->add(object_ptr<Ui::VerticalLayout>(page));

	addTitle(page, tr::lng_oblivion_photo_ui_effects_add(), Px(kSkip));
	const auto chips = page->add(object_ptr<ChipsFlow>(page), RowMargins());
	for (const auto type : EffectTypes()) {
		chips->addChip(
			LangValue([=] { return EffectName(type); }),
			[=] { addEffect(type); });
	}
	addHint(page, tr::lng_oblivion_photo_ui_effects_hint());
	addLayerFxSection(
		page,
		tr::lng_oblivion_photo_panel_more_title(),
		tr::lng_oblivion_photo_panel_more_about(),
		tr::lng_oblivion_photo_panel_more_button(),
		{
			FxGroup::Blur,
			FxGroup::Distort,
			FxGroup::Lofi,
			FxGroup::Glitch,
			FxGroup::Stylize,
			FxGroup::Classic,
		},
		false);
	rebuildEffects();
}

void Editor::rebuildEffects() {
	auto types = std::vector<EffectType>();
	types.reserve(_state.effects.size());
	for (const auto &effect : _state.effects) {
		types.push_back(effect.type);
	}
	if (types == _effectsBuilt && _effectsList->count() == int(types.size())) {
		return;
	}
	_effectsBuilt = types;
	_effectsList->clear();
	for (auto i = 0; i != int(types.size()); ++i) {
		addEffectCard(i);
	}
	_effectsEmpty = types.empty();
	_effectsList->resizeToWidth(_effectsList->width());
}

void Editor::addEffectCard(int index) {
	const auto type = _state.effects[index].type;
	const auto card = _effectsList->add(
		object_ptr<Ui::VerticalLayout>(_effectsList),
		style::margins(
			Px(kPanelPadding),
			index ? Px(kCardSkip) : Px(kSkip),
			Px(kPanelPadding),
			0));
	MarkAsCard(card);
	card->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(card);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::groupCallBg);
		const auto radius = Px(kCardRadius);
		p.drawRoundedRect(card->rect(), radius, radius);
	}, card->lifetime());

	const auto currentEffect = [=]() -> const Effect* {
		return (index < int(_state.effects.size()))
			? &_state.effects[index]
			: nullptr;
	};
	const auto change = [=](Fn<void(Effect&)> modify, bool commitNow) {
		if (index >= int(_state.effects.size())) {
			return;
		}
		auto state = _state;
		modify(state.effects[index]);
		apply(std::move(state), commitNow);
	};

	const auto header = card->add(object_ptr<SwitchHeader>(
		card,
		EffectName(type),
		_state.effects[index].enabled));
	header->toggles() | rpl::on_next([=](bool enabled) {
		change([=](Effect &effect) { effect.enabled = enabled; }, true);
	}, header->lifetime());
	header->menuRequests() | rpl::on_next([=](QPoint position) {
		showEffectMenu(index, position);
	}, header->lifetime());
	_refreshControls.events() | rpl::on_next([=] {
		if (const auto current = currentEffect()) {
			header->setChecked(current->enabled, anim::type::normal);
		}
	}, header->lifetime());

	const auto padding = Px(kCardPadding);
	const auto paramMargins = style::margins(padding, 0, padding, 0);
	for (const auto &info : EffectParams(type)) {
		const auto param = info.param;
		if (info.color) {
			const auto current = (param == EffectParam::Color1)
				? _state.effects[index].color1
				: _state.effects[index].color2;
			const auto swatches = card->add(
				object_ptr<ColorSwatches>(
					card,
					LangValue([=] { return EffectParamName(param); }),
					SwatchPresets(),
					QColor::fromRgb(current)),
				paramMargins);
			swatches->chosen() | rpl::on_next([=](QColor color) {
				change([=](Effect &effect) {
					((param == EffectParam::Color1)
						? effect.color1
						: effect.color2) = color.rgb();
				}, true);
			}, swatches->lifetime());
			swatches->customRequests() | rpl::on_next([=] {
				chooseCustomColor(index, param);
			}, swatches->lifetime());
			_refreshControls.events() | rpl::on_next([=] {
				if (const auto current = currentEffect()) {
					swatches->setColor(QColor::fromRgb(
						(param == EffectParam::Color1)
							? current->color1
							: current->color2));
				}
			}, swatches->lifetime());
		} else if (info.toggle) {
			const auto toggle = card->add(
				object_ptr<SwitchHeader>(
					card,
					EffectParamName(param),
					_state.effects[index].value(param) != 0,
					false),
				paramMargins);
			toggle->toggles() | rpl::on_next([=](bool enabled) {
				change([=](Effect &effect) {
					effect.setValue(param, enabled ? 1 : 0);
				}, true);
			}, toggle->lifetime());
			_refreshControls.events() | rpl::on_next([=] {
				if (const auto current = currentEffect()) {
					toggle->setChecked(
						current->value(param) != 0,
						anim::type::normal);
				}
			}, toggle->lifetime());
		} else {
			const auto slider = card->add(
				object_ptr<ValueSlider>(card, SliderArgs{
					.label = LangValue([=] {
						return EffectParamName(param);
					}),
					.min = info.min,
					.max = info.max,
					.defaultValue = info.defaultValue,
					.value = _state.effects[index].value(param),
					.format = [=](int value) {
						return EffectValueText(param, value);
					},
				}),
				paramMargins);
			slider->changes() | rpl::on_next([=](SliderChange value) {
				change([=](Effect &effect) {
					effect.setValue(param, value.value);
				}, value.finished);
			}, slider->lifetime());
			_refreshControls.events() | rpl::on_next([=] {
				if (const auto current = currentEffect()) {
					slider->setValue(current->value(param));
					slider->setDimmed(!current->enabled);
				}
			}, slider->lifetime());
		}
	}
	card->add(object_ptr<Ui::FixedHeightWidget>(card, Px(kSkip)));
}

void Editor::showEffectMenu(int index, QPoint globalPosition) {
	const auto count = int(_state.effects.size());
	if (index >= count) {
		return;
	}
	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::groupCallPopupMenu);
	const auto move = [=](int to) {
		auto state = _state;
		if (index < int(state.effects.size())
			&& to >= 0
			&& to < int(state.effects.size())) {
			std::swap(state.effects[index], state.effects[to]);
			apply(std::move(state), true);
		}
	};
	if (index > 0) {
		_menu->addAction(
			tr::lng_oblivion_photo_ui_effect_up(tr::now),
			[=] { move(index - 1); });
	}
	if (index + 1 < count) {
		_menu->addAction(
			tr::lng_oblivion_photo_ui_effect_down(tr::now),
			[=] { move(index + 1); });
	}
	_menu->addAction(tr::lng_oblivion_photo_ui_effect_reset(tr::now), [=] {
		auto state = _state;
		if (index < int(state.effects.size())) {
			const auto enabled = state.effects[index].enabled;
			state.effects[index] = DefaultEffect(state.effects[index].type);
			state.effects[index].enabled = enabled;
			apply(std::move(state), true);
		}
	});
	if (count < kMaxEffects) {
		_menu->addAction(
			tr::lng_oblivion_photo_ui_effect_duplicate(tr::now),
			[=] {
				auto state = _state;
				if (index < int(state.effects.size())) {
					const auto copy = state.effects[index];
					state.effects.insert(
						begin(state.effects) + index + 1,
						copy);
					apply(std::move(state), true);
				}
			});
	}
	_menu->addAction(tr::lng_oblivion_photo_ui_effect_remove(tr::now), [=] {
		auto state = _state;
		if (index < int(state.effects.size())) {
			state.effects.erase(begin(state.effects) + index);
			apply(std::move(state), true);
		}
	});
	_menu->popup(globalPosition);
}

void Editor::chooseCustomColor(int index, EffectParam param) {
	if (index >= int(_state.effects.size())) {
		return;
	}
	const auto &effect = _state.effects[index];
	const auto initial = QColor::fromRgb((param == EffectParam::Color1)
		? effect.color1
		: effect.color2);
	const auto weak = base::make_weak(this);
	_layers->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_oblivion_photo_ui_color_title());
		const auto editor = box->addRow(
			object_ptr<ColorEditor>(box, ColorEditor::Mode::HSL, initial),
			style::margins());
		box->setWidth(editor->width());
		const auto save = [=] {
			const auto color = editor->color();
			box->closeBox();
			if (const auto strong = weak.get()) {
				auto state = strong->_state;
				if (index < int(state.effects.size())) {
					((param == EffectParam::Color1)
						? state.effects[index].color1
						: state.effects[index].color2) = color.rgb();
					strong->apply(std::move(state), true);
				}
			}
		};
		editor->submitRequests() | rpl::on_next(save, editor->lifetime());
		box->setFocusCallback([=] { editor->setInnerFocus(); });
		box->addButton(tr::lng_settings_save(), save);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		box->boxClosing() | rpl::on_next([=] {
			if (const auto strong = weak.get()) {
				strong->setFocus();
			}
		}, box->lifetime());
	}));
}

void Editor::addEffect(EffectType type) {
	if (int(_state.effects.size()) >= kMaxEffects) {
		toast(tr::lng_oblivion_photo_ui_effects_limit(
			tr::now,
			lt_max,
			QString::number(kMaxEffects)));
		return;
	}
	auto state = _state;
	state.effects.push_back(DefaultEffect(type));
	apply(std::move(state), true);
	const auto scroll = _pages[int(Tab::Effects)].scroll;
	crl::on_main(this, [=] {
		if (_effectsList->count() > 0) {
			const auto last = _effectsList->widgetAt(
				_effectsList->count() - 1);
			const auto top = _effectsList->y() + last->y();
			scroll->scrollToY(top, top + last->height());
		}
	});
}

void Editor::setupAutoPage(not_null<Ui::VerticalLayout*> page) {
	addTitle(page, tr::lng_oblivion_photo_ui_auto_title());
	page->add(
		object_ptr<Ui::FlatLabel>(
			page,
			tr::lng_oblivion_photo_ui_auto_about(),
			HintLabelStyle()),
		RowMargins());
	_autoButton = page->add(
		object_ptr<PanelButton>(page, _autoButtonText.value(), true),
		RowMargins(Px(kLargeSkip)));
	_autoButton->setClickedCallback([=] { autoEnhance(); });

	const auto amount = page->add(
		object_ptr<Ui::SlideWrap<ValueSlider>>(
			page,
			object_ptr<ValueSlider>(page, SliderArgs{
				.label = tr::lng_oblivion_photo_ui_auto_amount(),
				.min = 0,
				.max = 100,
				.defaultValue = 100,
				.value = 100,
			}),
			RowMargins(Px(kLargeSkip))));
	amount->toggleOn(_autoAvailable.value(), anim::type::normal);
	amount->entity()->changes() | rpl::on_next([=](SliderChange change) {
		applyAuto(change.value, change.finished);
	}, amount->lifetime());
	_refreshControls.events() | rpl::on_next([=] {
		if (_auto) {
			amount->entity()->setValue(_auto->amount);
		}
	}, amount->lifetime());
}

void Editor::startLoading(QImage image) {
	const auto screen = QGuiApplication::primaryScreen();
	const auto screenSide = screen
		? int(std::max(screen->size().width(), screen->size().height())
			* screen->devicePixelRatio()
			* kPreviewScreenPart)
		: kPreviewMinSide;
	_previewSide = std::clamp(
		screenSide,
		kPreviewMinSide,
		kPreviewMaxInitialSide);
	if (_options.document && !_options.document->empty()) {
		documentReady(*_options.document);
		return;
	} else if (image.isNull()) {
		_canvas->setLoading(false);
		return;
	}
	crl::async([weak = base::make_weak(this), image = std::move(image)] {
		auto source = PrepareSource(image);
		crl::on_main(weak, [=, source = std::move(source)]() mutable {
			weak->sourceReady(std::move(source));
		});
	});
}

void Editor::sourceReady(QImage source) {
	if (source.isNull()) {
		_canvas->setLoading(false);
		toast(tr::lng_oblivion_photo_ui_export_failed(tr::now));
		return;
	}
	documentReady(DocumentFromImage(
		std::move(source),
		_state,
		tr::lng_oblivion_photo_panel_layer_photo(tr::now)));
}

void Editor::documentReady(Document document) {
	if (document.empty()) {
		_canvas->setLoading(false);
		toast(tr::lng_oblivion_photo_ui_export_failed(tr::now));
		return;
	}
	_sourceReady = true;
	_done->setDisabled(false);
	_controller->setDocument(std::move(document));
	if (!_options.tool.isEmpty()) {
		_controller->setTool(_options.tool);
	}
	update();
}

const Layer *Editor::backgroundLayer() const {
	if (!_controller->hasDocument()) {
		return nullptr;
	}
	const auto active = _controller->activeLayer();
	if (active && AsImage(active->content)) {
		return active;
	}
	for (const auto &layer : _controller->document().layers) {
		if (AsImage(layer.content)) {
			return &layer;
		}
	}
	return nullptr;
}

void Editor::refreshBackgroundState() {
	const auto layer = backgroundLayer();
	const auto image = layer ? AsImage(layer->content) : nullptr;
	_backgroundRemoved = image && (image->original() != nullptr);
}

void Editor::toggleBackground() {
	if (_exporting || !_sourceReady) {
		return;
	}
	const auto layer = backgroundLayer();
	if (!layer) {
		toast(tr::lng_oblivion_photo_panel_no_image_layer(tr::now));
		return;
	} else if (layer->locked) {
		toast(tr::lng_oblivion_photo_panel_layer_locked(tr::now));
		return;
	}
	const auto id = layer->id;
	const auto content = std::static_pointer_cast<const ImageContent>(
		layer->content);
	if (const auto original = content->original()) {
		_controller->changeLayer(id, [=](Layer &layer) {
			layer.content = original;
		});
		return;
	}
	showBackgroundProgress();
	if (!_removingBackground) {
		return;
	}
	_backgroundWantedId = id;
	_backgroundWanted = content;
	// A request that runs for minutes will hardly ever answer, its result
	// is not waited for anymore.
	if (!_backgroundRequested
		|| (crl::now() - _backgroundStarted >= kBackgroundLostAfter)) {
		startBackgroundRequest(id, content);
	}
}

void Editor::startBackgroundRequest(
		LayerId id,
		std::shared_ptr<const ImageContent> content) {
	const auto token = ++_backgroundToken;
	_backgroundRequested = true;
	_backgroundStarted = crl::now();
	crl::async([weak = base::make_weak(this), content, id, token] {
		auto removed = Vision::RemoveBackground(content->image());
		auto cutout = std::shared_ptr<const ImageContent>();
		if (removed.ok) {
			cutout = MakeImageContent(std::move(removed.cutout), content);
		} else {
			LOG(("Oblivion Vision Error: no background removal, %1."
				).arg(removed.error));
		}
		crl::on_main(weak, [=] {
			weak->backgroundReady(token, id, content, cutout);
		});
	});
}

void Editor::showBackgroundProgress() {
	if (_exporting) {
		return;
	}
	_removingBackground = true;
	setExporting(true, tr::lng_oblivion_vision_editor_progress(tr::now));
	_busy->setCancellable(
		[=] { cancelBackground(); },
		tr::lng_oblivion_vision_slow(tr::now));
}

void Editor::cancelBackground() {
	if (_removingBackground) {
		finishBackground();
	}
}

void Editor::finishBackground() {
	_removingBackground = false;
	_backgroundWantedId = 0;
	_backgroundWanted = nullptr;
	setExporting(false);
}

void Editor::backgroundReady(
		int token,
		LayerId id,
		std::shared_ptr<const ImageContent> from,
		std::shared_ptr<const ImageContent> cutout) {
	if (token != _backgroundToken) {
		// Of a request that was given up for lost, another one runs.
		return;
	}
	_backgroundRequested = false;
	if (!_removingBackground || _finished) {
		return;
	}
	const auto wantedId = _backgroundWantedId;
	const auto wanted = _backgroundWanted;
	if (id != wantedId || from != wanted) {
		// The result of a cancelled request, while the button was pressed
		// for another layer since: that layer has its turn now, the cover
		// stays.
		const auto layer = _controller->document().find(wantedId);
		if (wanted && layer && !layer->locked && layer->content == wanted) {
			startBackgroundRequest(wantedId, wanted);
		} else {
			finishBackground();
		}
		return;
	}
	finishBackground();
	const auto layer = _controller->document().find(id);
	if (!cutout
		|| cutout->image().isNull()
		|| !layer
		|| layer->content != from) {
		toast(tr::lng_oblivion_vision_cutout_not_found(tr::now));
		return;
	}
	_controller->changeLayer(id, [=](Layer &layer) {
		layer.content = cutout;
	});
}

void Editor::apply(EditState state, bool commitNow) {
	state = Normalized(std::move(state));
	if (!_controller->hasDocument()) {
		// Still loading: the document starts from this state.
		if (!(state == _state)) {
			_state = std::move(state);
			stateChanged();
		}
		return;
	}
	auto document = _controller->document();
	document.global = std::move(state);
	_controller->apply(std::move(document), commitNow);
}

void Editor::commit() {
	_controller->commit();
}

void Editor::undo() {
	if (!_controller->canUndo()) {
		return;
	}
	_controller->undo();
	_canvas->setGridVisible(false);
	syncRatio();
}

void Editor::redo() {
	if (!_controller->canRedo()) {
		return;
	}
	_controller->redo();
	_canvas->setGridVisible(false);
	syncRatio();
}

void Editor::documentChanged() {
	const auto &document = _controller->document();
	_state = document.global;
	_sourceSize = document.size;
	stateChanged();
	// The layers list (under the pages, or the page of its own tab in
	// a narrow window) is as tall as its rows.
	if ((!narrow() || _tab == Tab::Layers)
		&& layersNaturalHeight() != _layersNatural) {
		updateLayout();
	}
}

void Editor::syncRatio() {
	const auto ratio = cropRatio();
	if (!ratio || _sourceSize.isEmpty()) {
		return;
	}
	const auto frame = OrientedSize(_sourceSize, _state.quarterTurns);
	const auto crop = _state.crop;
	const auto current = (crop.width() * frame.width())
		/ std::max(crop.height() * frame.height(), 1.);
	if (std::abs(current - *ratio) >= kRatioEpsilon) {
		inferRatio();
		if (_ratios) {
			_ratios->setSelected(_ratioIndex);
		}
	}
}

void Editor::refreshHistoryButtons() {
	if (_undo) {
		_undo->setAvailable(_controller->canUndo());
		_redo->setAvailable(_controller->canRedo());
	}
}

bool Editor::resetAvailable() const {
	if (!IsIdentity(_state)) {
		return true;
	} else if (!_controller->hasDocument()) {
		return false;
	}
	const auto &now = _controller->document();
	const auto &initial = _controller->initialDocument();
	return (now.size != initial.size) || !(now.layers == initial.layers);
}

void Editor::stateChanged() {
	if (_auto && !_autoApplying && !(_state == _auto->applied)) {
		_auto = std::nullopt;
		_autoAvailable = false;
	}
	if (_effectsList) {
		rebuildEffects();
	}
	_canvas->setCrop(_state.crop);
	_canvas->setCropRatio(cropRatio());
	_refreshControls.fire({});
	refreshBackgroundState();
	if (_reset) {
		_reset->setAvailable(resetAvailable());
	}
	if (_undo) {
		refreshHistoryButtons();
	}
	refreshPreview(false);
	if (_comparing) {
		refreshBefore();
	}
	refreshAnalysis();
	scheduleThumbnails();
	update(topBarRect());
}

EditState Editor::displayState() const {
	auto result = _state;
	if (_tab == Tab::Crop) {
		result.crop = QRectF(0., 0., 1., 1.);
	}
	return result;
}

Document Editor::displayDocument() const {
	auto result = _controller->document();
	result.global = displayState();
	return result;
}

RenderKey Editor::displayKey() const {
	return {
		.revision = _controller->revision(),
		.uncropped = (_tab == Tab::Crop),
	};
}

double Editor::proxyScale() const {
	return ScaleForSide(_controller->document().size, _previewSide);
}

bool Editor::dirty() const {
	return _controller->modified()
		|| (_options.unsaved && _controller->hasDocument());
}

void Editor::refreshPreview(bool force) {
	if (!_sourceReady) {
		return;
	}
	const auto key = displayKey();
	const auto display = displayDocument();
	const auto frame = OutputSize(display);
	if (frame != _canvas->frameSize()) {
		_beforeValid = false;
	}
	_canvas->setFrameSize(frame);
	_canvas->setFrameTransform(OutputTransform(display, frame));
	const auto fit = _canvas->fitPixels();
	if (fit.isEmpty()) {
		return;
	}
	const auto fitSide = std::max(fit.width(), fit.height());
	const auto sourceSide = std::max(
		display.size.width(),
		display.size.height());
	const auto target = std::min(
		std::max(kPreviewMinSide, int(fitSide * kPreviewGrow)),
		sourceSide);
	if (_previewSide * kDetailThreshold < target
		|| _previewSide > target * kPreviewShrink) {
		_previewSide = target;
		force = true;
	}
	const auto active = _controller->activeLayerId();
	if (force || !(key == _fastKey) || fit != _fastSize) {
		_fastKey = key;
		_fastSize = fit;
		_fastId = _renderer->request({
			.document = display,
			.scale = proxyScale(),
			.size = fit,
			.active = active,
			.key = key,
		});

		// It drops a detail render still in progress and its result may
		// replace the detail one on the screen (an undo and a redo in a
		// row), so the next detail render can't be skipped as a repeat.
		_detailValid = false;
	}
	const auto needed = _canvas->renderPixels();
	const auto fast = OutputSize(
		ScaledSize(display.size, proxyScale()),
		display.global,
		fit);
	if (needed.width() > fast.width() * kDetailThreshold
		|| needed.height() > fast.height() * kDetailThreshold) {
		_detailTimer.callOnce(kDetailDelay);
	} else {
		_detailTimer.cancel();
	}
}

void Editor::requestDetail() {
	if (!_sourceReady) {
		return;
	}
	const auto key = displayKey();
	const auto needed = _canvas->renderPixels();
	if (_detailValid && key == _detailKey && needed == _detailSize) {
		return;
	}
	_detailKey = key;
	_detailSize = needed;
	const auto display = displayDocument();
	_detailId = _renderer->request({
		.document = display,
		.scale = ScaleForOutput(needed, OutputSize(display)),
		.size = needed,
		.active = _controller->activeLayerId(),
		.key = key,
		.cancellable = true,
	});
	_detailValid = true;
}

void Editor::previewReady(PreviewRenderer::Result &&result) {
	if (result.id < _shownId || result.frame != _canvas->frameSize()) {
		return;
	}
	if (_shownAny
		&& result.key == _shownKey
		&& _canvas->hasImage()
		&& result.image.width() < _shownSize.width()
		&& result.size == _fastSize
		&& _detailKey == _shownKey) {
		if (result.id == _fastId && _shownId == _detailId) {
			// Everything requested is done and the detail render stays.
			_detailValid = true;
		}
		return;
	}
	_shownAny = true;
	_shownId = result.id;
	_shownKey = result.key;
	_shownSize = result.image.size();
	_canvas->setImage(std::move(result.image));
	_canvas->setLoading(false);
	_controller->setShownRevision(_shownKey.revision);
}

void Editor::refreshBefore() {
	if (!_sourceReady) {
		return;
	}
	const auto display = GeometryOnly(displayState());
	const auto needed = _canvas->renderPixels();
	const auto canvas = _controller->document().size;
	if (_beforeValid
		&& display == _beforeState
		&& needed == _beforeSize
		&& canvas == _beforeCanvas) {
		return;
	} else if (!(display == _beforeState) || canvas != _beforeCanvas) {
		// Another geometry: the old original would be shown turned or
		// flipped the wrong way until the new one is ready.
		_beforeValid = false;
		_canvas->setBeforeImage(QImage());
	}
	_beforeState = display;
	_beforeSize = needed;
	_beforeCanvas = canvas;
	// The canvas may have got another size since the picture was opened
	// (the Crop tab, the proportions of a collage). The original is shown
	// on the canvas of the result, where it was put when the canvas was
	// resized: in the same frame, not stretched over it.
	auto document = OnCanvas(_controller->initialDocument(), canvas);
	document.global = display;
	const auto scale = std::max(
		ScaleForSide(document.size, _previewSide),
		ScaleForOutput(needed, OutputSize(document)));
	_beforeId = _beforeRenderer->request({
		.document = std::move(document),
		.scale = scale,
		.size = needed,
	});
}

void Editor::beforeReady(PreviewRenderer::Result &&result) {
	if (result.id != _beforeId
		|| result.size != _beforeSize
		|| result.frame != _canvas->frameSize()) {
		return;
	}
	_beforeValid = true;
	_canvas->setBeforeImage(std::move(result.image));
}

// A little picture of the flattened layers: the source of the filter
// thumbnails and the answer to "does the result have transparency".
void Editor::refreshAnalysis() {
	if (!_sourceReady || _analysisRunning) {
		return;
	}
	const auto revision = _controller->layersRevision();
	if (_analysisRevision == revision) {
		return;
	}
	_analysisRunning = true;
	crl::async([
			weak = base::make_weak(this),
			compositor = _controller->compositor(),
			document = _controller->document(),
			revision] {
		auto reduced = compositor->render(document, {
			.scale = ScaleForSide(document.size, kThumbSourceSide),
			.global = false,
			.preview = true,
		});
		const auto alpha = HasTransparentPixels(reduced);
		crl::on_main(weak, [=, reduced = std::move(reduced)]() mutable {
			weak->analysisReady(std::move(reduced), alpha, revision);
		});
	});
}

void Editor::analysisReady(QImage reduced, bool alpha, uint64 revision) {
	_analysisRunning = false;
	_analysisRevision = revision;
	_thumbSource = std::move(reduced);
	_hasAlpha = alpha;
	_thumbsState = std::nullopt;
	++_thumbsGeneration;
	scheduleThumbnails();
	refreshAnalysis();
}

void Editor::setComparing(bool comparing) {
	if (_comparing == comparing) {
		return;
	}
	_comparing = comparing;
	if (comparing) {
		refreshBefore();
	}
	_canvas->setComparing(comparing);
	_compare->setActive(comparing);
}

void Editor::scheduleThumbnails() {
	_thumbsDirty = true;
	if (_tab == Tab::Filters && _sourceReady) {
		_thumbsTimer.callOnce(_strip->thumbnailsReady() ? kThumbsDelay : 0);
	}
}

void Editor::refreshThumbnails() {
	if (!_thumbsDirty || _thumbSource.isNull()) {
		return;
	}
	_thumbsDirty = false;
	const auto state = WithoutFilter(_state);
	if (_thumbsState && *_thumbsState == state) {
		return;
	}
	_thumbsState = state;
	const auto generation = ++_thumbsGeneration;
	const auto ratio = style::DevicePixelRatio();
	const auto side = FilterStrip::ThumbnailSide() * ratio;
	crl::async([
			weak = base::make_weak(this),
			source = _thumbSource,
			state,
			side,
			ratio,
			generation] {
		auto thumbnails = FilterThumbnails(source, state, side);
		auto images = std::vector<QImage>();
		images.reserve(thumbnails.size());
		for (auto &thumbnail : thumbnails) {
			thumbnail.image.setDevicePixelRatio(ratio);
			images.push_back(std::move(thumbnail.image));
		}
		crl::on_main(weak, [=, images = std::move(images)]() mutable {
			if (weak->_thumbsGeneration == generation) {
				weak->_strip->setThumbnails(std::move(images));
			}
		});
	});
}

void Editor::inferRatio() {
	_ratioIndex = kRatioFree;
	if (_sourceSize.isEmpty()) {
		return;
	}
	const auto crop = _state.crop;
	if (crop == QRectF(0., 0., 1., 1.)) {
		return;
	}
	const auto frame = OrientedSize(_sourceSize, _state.quarterTurns);
	const auto ratio = (crop.width() * frame.width())
		/ std::max(crop.height() * frame.height(), 1.);
	for (auto i = 0; i != int(kRatioPresets.size()); ++i) {
		const auto preset = kRatioPresets[i];
		if (preset.width > 0
			&& std::abs(ratio - preset.width / float64(preset.height))
				< kRatioEpsilon) {
			_ratioIndex = i;
			return;
		}
	}
}

std::optional<float64> Editor::cropRatio() const {
	const auto preset = kRatioPresets[_ratioIndex];
	if (preset.width == 0) {
		return std::nullopt;
	} else if (preset.width < 0) {
		const auto frame = OrientedSize(_sourceSize, _state.quarterTurns);
		return frame.isEmpty()
			? std::nullopt
			: std::make_optional(frame.width() / float64(frame.height()));
	}
	return preset.width / float64(preset.height);
}

void Editor::selectRatio(int index) {
	_ratioIndex = std::clamp(index, 0, int(kRatioPresets.size()) - 1);
	_ratios->setSelected(_ratioIndex);
	const auto ratio = cropRatio();
	_canvas->setCropRatio(ratio);
	if (!ratio) {
		return;
	}
	auto state = _state;
	state.crop = LargestCrop(
		*ratio,
		OrientedSize(_sourceSize, state.quarterTurns),
		state.crop.center());
	apply(std::move(state), true);
}

void Editor::rotate(bool clockwise) {
	auto state = _state;
	const auto oneFlip = (state.flipHorizontal != state.flipVertical);
	const auto turnClockwise = (clockwise != oneFlip);
	state.quarterTurns = (state.quarterTurns + (turnClockwise ? 1 : 3)) % 4;
	state.crop = RotateCrop(state.crop, clockwise);
	const auto preset = kRatioPresets[_ratioIndex];
	if (preset.width > 0 && preset.width != preset.height) {
		for (auto i = 0; i != int(kRatioPresets.size()); ++i) {
			if (kRatioPresets[i].width == preset.height
				&& kRatioPresets[i].height == preset.width) {
				_ratioIndex = i;
				_ratios->setSelected(i);
				break;
			}
		}
	}
	apply(std::move(state), true);
}

void Editor::flip(bool horizontal) {
	auto state = _state;
	const auto crop = state.crop;
	if (horizontal) {
		state.flipHorizontal = !state.flipHorizontal;
		state.crop = QRectF(
			1. - crop.x() - crop.width(),
			crop.y(),
			crop.width(),
			crop.height());
	} else {
		state.flipVertical = !state.flipVertical;
		state.crop = QRectF(
			crop.x(),
			1. - crop.y() - crop.height(),
			crop.width(),
			crop.height());
	}
	state.straighten = -state.straighten;
	apply(std::move(state), true);
}

void Editor::resetGeometry() {
	auto state = _state;
	state.crop = QRectF(0., 0., 1., 1.);
	state.quarterTurns = 0;
	state.straighten = 0.;
	state.flipHorizontal = false;
	state.flipVertical = false;
	_ratioIndex = kRatioFree;
	_ratios->setSelected(_ratioIndex);
	apply(std::move(state), true);
}

void Editor::selectFilter(const QString &id) {
	auto state = _state;
	if (state.filter != id) {
		state.filter = id;
		state.filterIntensity = 100;
	}
	apply(std::move(state), true);
}

void Editor::stepFilter(int delta) {
	const auto &ids = FilterIds();
	const auto i = ranges::find(ids, _state.filter);
	const auto index = (i == end(ids)) ? 0 : int(i - begin(ids));
	const auto next = std::clamp(index + delta, 0, int(ids.size()) - 1);
	if (next != index) {
		selectFilter(ids[next]);
	}
}

void Editor::autoEnhance() {
	if (_autoBusy || !_sourceReady) {
		return;
	}
	_autoBusy = true;
	_autoButtonText = tr::lng_oblivion_photo_ui_auto_working(tr::now);
	_autoButton->setBusy(true);
	auto document = _controller->document();
	document.global = GeometryOnly(_state);
	crl::async([
			weak = base::make_weak(this),
			compositor = _controller->compositor(),
			document = std::move(document)] {
		const auto framed = compositor->render(document, {
			.scale = ScaleForSide(document.size, 2 * kAutoSide),
			.maxSize = QSize(kAutoSide, kAutoSide),
			.preview = true,
		});
		auto target = AutoEnhance(framed);
		crl::on_main(weak, [=, target = std::move(target)]() mutable {
			weak->autoReady(std::move(target));
		});
	});
}

void Editor::autoReady(EditState target) {
	_autoBusy = false;
	_autoButtonText = tr::lng_oblivion_photo_ui_auto_button(tr::now);
	_autoButton->setBusy(false);
	if (SameAdjustments(target, EditState())) {
		toast(tr::lng_oblivion_photo_ui_auto_nothing(tr::now));
		return;
	}
	const auto from = _auto ? _auto->base : _state;
	_auto = AutoState{
		.base = from,
		.target = std::move(target),
	};
	applyAuto(100, true);
	_autoAvailable = true;
}

void Editor::applyAuto(int amount, bool commitNow) {
	if (!_auto) {
		return;
	}
	auto state = _state;
	for (const auto &info : AdjustList()) {
		const auto delta = _auto->target.value(info.id) - info.defaultValue;
		const auto from = _auto->base.value(info.id);
		state.setValue(
			info.id,
			from + int(std::round(delta * amount / 100.)));
	}
	_autoApplying = true;
	apply(std::move(state), commitNow);
	_autoApplying = false;
	if (_auto) {
		_auto->applied = _state;
		_auto->amount = amount;
	}
}

bool Editor::narrow() const {
	return width() < Px(kNarrowWidth);
}

QRect Editor::topBarRect() const {
	return QRect(0, 0, width(), Px(kTopBarHeight));
}

void Editor::updateTopBar() {
	if (!_done) {
		return;
	}
	const auto top = Px(kTopBarHeight);
	const auto size = Px(kTopButton);
	const auto buttonTop = (top - size) / 2;
	_close->moveToLeft(Px(kTopBarSide), buttonTop, width());

	auto right = width() - Px(kDoneRight);
	_done->moveToLeft(
		right - _done->width(),
		(top - _done->height()) / 2,
		width());
	right = _done->x() - Px(kSkip);
	if (!_more->isHidden()) {
		right -= size;
		_more->moveToLeft(right, buttonTop, width());
	}
	right -= Px(kTopGroupSkip);
	for (const auto button : { _reset, _compare }) {
		right -= size;
		button->moveToLeft(right, buttonTop, width());
		right -= Px(kTopButtonSkip);
	}
	right -= Px(kTopGroupSkip);
	for (const auto button : { _redo, _undo }) {
		right -= size;
		button->moveToLeft(right, buttonTop, width());
		right -= Px(kTopButtonSkip);
	}
	_titleRight = right;
	update(topBarRect());
}

void Editor::updateLayout() {
	if (width() <= 0 || height() <= 0 || _layingOut) {
		return;
	}
	_layingOut = true;
	updateTopBar();
	const auto top = Px(kTopBarHeight);
	const auto margin = Px(kPanelMargin);
	const auto stripHeight = (_tab == Tab::Filters)
		? FilterStrip::StripHeight()
		: 0;
	const auto isNarrow = narrow();
	_tabs->setTabVisible(int(Tab::Layers), isNarrow);
	if (!isNarrow && _tab == Tab::Layers) {
		_tab = Tab::Layer;
		_tabs->setActive(int(_tab), anim::type::instant);
		for (auto i = 0; i != int(_pages.size()); ++i) {
			_pages[i].scroll->setVisible(i == int(_tab));
		}
	}
	const auto panelWidth = isNarrow
		? std::max(width() - 2 * margin, 1)
		: std::min(Px(kPanelWidth), width() / 2);
	const auto inset = Px(kPanelTabsInset);
	_tabs->resizeToWidth(panelWidth - 2 * inset);
	const auto pagesTop = inset + _tabs->height() + Px(kPanelPagesSkip);
	const auto radius = Px(kPanelRadius);
	for (const auto &page : _pages) {
		page.content->resizeToWidth(panelWidth);
	}
	auto panel = QRect();
	auto canvasBottom = 0;
	_layersNatural = layersNaturalHeight();
	if (isNarrow) {
		// Under the photo the panel takes only what the current page
		// needs (up to a part of the window), the rest goes to the photo.
		const auto maxHeight = std::max(
			height() * kNarrowPanelPercent / 100,
			1);
		const auto &page = _pages[int(_tab)];
		const auto natural = (_tab != Tab::Layers)
			? (pagesTop + page.content->height() + radius / 2)
			: (_layersNatural > 0)
			? (pagesTop + _layersNatural + radius / 2)
			: maxHeight;
		const auto panelHeight = std::clamp(
			natural,
			std::min(Px(kNarrowPanelMin), maxHeight),
			maxHeight);
		panel = QRect(
			margin,
			height() - margin - panelHeight,
			panelWidth,
			panelHeight);
		canvasBottom = panel.y() - stripHeight - Px(kCanvasPanelGap);
	} else {
		panel = QRect(
			width() - margin - panelWidth,
			top,
			panelWidth,
			height() - top - margin);
		canvasBottom = height() - stripHeight;
	}
	const auto toolsHeight = std::max(
		(isNarrow ? canvasBottom : (height() - margin)) - top,
		1);
	const auto toolsWidth = _toolStrip->widthForHeight(toolsHeight);
	_toolStrip->setGeometry(margin, top, toolsWidth, toolsHeight);
	const auto canvasLeft = margin + toolsWidth + Px(kToolStripGap);
	const auto canvasRight = isNarrow
		? width()
		: (panel.x() - Px(kCanvasPanelGap));
	const auto canvas = QRect(
		canvasLeft,
		top,
		std::max(canvasRight - canvasLeft, 1),
		std::max(canvasBottom - top, 1));
	_panel->setGeometry(panel);
	_canvas->setGeometry(canvas);
	if (stripHeight) {
		_strip->setGeometry(
			canvas.x(),
			canvas.y() + canvas.height(),
			canvas.width(),
			stripHeight);
	}
	_strip->setVisible(stripHeight > 0);

	_tabs->moveToLeft(inset, inset, panel.width());
	const auto pagesBottom = panel.height() - radius / 2;
	auto pagesHeight = std::max(pagesBottom - pagesTop, 1);
	if (isNarrow) {
		_layersTop = 0;
		_layersPanel->setGeometry(0, pagesTop, panel.width(), pagesHeight);
		_layersPanel->setVisible(_tab == Tab::Layers);
	} else {
		// The layers list takes the bottom part of the plate: what its
		// rows need, but not more than a part of the plate (it scrolls
		// then). The pages never get less than they need to stay usable.
		const auto limit = std::clamp(
			panel.height() * kLayersPercent / 100,
			Px(kLayersMin),
			Px(kLayersMax));
		const auto wanted = (_layersNatural > 0)
			? std::clamp(_layersNatural, Px(kLayersCompactMin), limit)
			: limit;
		const auto layersHeight = std::max(
			std::min(wanted, pagesHeight - Px(kPagesMin) - Px(kLayersSkip)),
			0);
		const auto shown = (layersHeight >= Px(kLayersMin) / 2);
		if (shown) {
			pagesHeight -= layersHeight + Px(kLayersSkip);
			_layersTop = pagesTop + pagesHeight + Px(kLayersSkip);
			_layersPanel->setGeometry(
				0,
				_layersTop,
				panel.width(),
				std::max(pagesBottom - _layersTop, 1));
		} else {
			_layersTop = 0;
		}
		_layersPanel->setVisible(shown);
	}
	for (const auto &page : _pages) {
		page.scroll->setGeometry(
			0,
			pagesTop,
			panel.width(),
			std::max(pagesHeight, 1));
	}
	refreshPagesFade();
	_panel->update();
	if (_busy) {
		_busy->setGeometry(rect());
	}
	_layingOut = false;
}

void Editor::selectTab(Tab tab) {
	if (tab == Tab::Tool && !_toolHasOptions) {
		tab = Tab::Layer;
	} else if (tab == Tab::Layers && !narrow()) {
		tab = Tab::Layer;
	}
	const auto wasCrop = (_tab == Tab::Crop);
	_tab = tab;
	if (wasCrop && tab != Tab::Crop) {
		commit();
	}
	_tabs->setActive(int(tab));
	for (auto i = 0; i != int(_pages.size()); ++i) {
		_pages[i].scroll->setVisible(
			(i == int(tab)) && (tab != Tab::Layers));
	}
	_canvas->setCropMode(tab == Tab::Crop);
	updateLayout();
	refreshPreview(false);
	if (_comparing) {
		refreshBefore();
	}
	if (tab == Tab::Filters) {
		scheduleThumbnails();
	}
}

void Editor::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(e->rect(), st::groupCallBg);
	if (!e->rect().intersects(topBarRect())) {
		return;
	}
	const auto left = _close->x() + _close->width() + Px(kTitleSkip);
	const auto available = _titleRight - left;
	if (available < Px(kTitleMinWidth)) {
		return;
	}
	const auto &titleFont = TitleFont();
	const auto &subtitleFont = SmallFont();
	const auto subtitle = SizeText(OutputSize(_sourceSize, _state));
	const auto full = titleFont->height
		+ Px(kSubtitleSkip)
		+ subtitleFont->height;
	const auto top = (Px(kTopBarHeight) - full) / 2;
	const auto title = _options.title.isEmpty()
		? tr::lng_oblivion_photo_ui_title(tr::now)
		: _options.title;
	p.setFont(titleFont);
	p.setPen(st::groupCallMembersFg);
	p.drawText(
		QRect(left, top, available, titleFont->height),
		Qt::AlignLeft | Qt::AlignVCenter,
		titleFont->elided(title, available));
	p.setFont(subtitleFont);
	p.setPen(st::groupCallMemberNotJoinedStatus);
	p.drawText(
		QRect(
			left,
			top + titleFont->height + Px(kSubtitleSkip),
			available,
			subtitleFont->height),
		Qt::AlignLeft | Qt::AlignVCenter,
		subtitleFont->elided(subtitle, available));
}

void Editor::resizeEvent(QResizeEvent *e) {
	updateLayout();
}

// A key that a text field inside of the editor had no use for (Backspace
// in a field that is empty already, an arrow at the end of the text)
// comes up here through the parents of the field. It is not a shortcut of
// the editor or of the tool then: Backspace removes a layer or a photo of
// a collage there. Only the modifier keys and what is pressed with
// Ctrl / Cmd (save, zoom, done) go on.
[[nodiscard]] bool KeyOfTextField(
		not_null<const QWidget*> editor,
		not_null<const QKeyEvent*> e) {
	switch (e->key()) {
	case Qt::Key_Shift:
	case Qt::Key_Control:
	case Qt::Key_Meta:
	case Qt::Key_Alt:
	case Qt::Key_AltGr:
		return false;
	}
	if (e->modifiers() & Qt::ControlModifier) {
		return false;
	}
	const auto focused = QApplication::focusWidget();
	return focused
		&& editor->isAncestorOf(focused)
		&& (qobject_cast<QTextEdit*>(focused)
			|| qobject_cast<QLineEdit*>(focused));
}

bool Editor::handleToolKey(not_null<QKeyEvent*> e) {
	const auto tool = _canvas->cropMode() ? nullptr : _controller->tool();
	return tool && tool->keyPress(e);
}

bool Editor::importFrom(not_null<const QMimeData*> data) {
	if (!_sourceReady || _exporting) {
		return false;
	}
	const auto paths = ImagePaths(data);
	if (!paths.isEmpty()) {
		_controller->importFiles(paths);
		return true;
	} else if (data->hasImage()) {
		auto image = qvariant_cast<QImage>(data->imageData());
		if (!image.isNull()) {
			_controller->addImageLayer(std::move(image), QString());
			return true;
		}
	}
	return false;
}

void Editor::dragEnterEvent(QDragEnterEvent *e) {
	const auto data = e->mimeData();
	if (_sourceReady
		&& !_exporting
		&& data
		&& (!ImagePaths(data).isEmpty() || data->hasImage())) {
		e->setDropAction(Qt::CopyAction);
		e->accept();
	} else {
		e->ignore();
	}
}

void Editor::dropEvent(QDropEvent *e) {
	const auto data = e->mimeData();
	if (data && importFrom(data)) {
		e->setDropAction(Qt::CopyAction);
		e->accept();
		setFocus();
	} else {
		e->ignore();
	}
}

void Editor::keyPressEvent(QKeyEvent *e) {
	if (_exporting) {
		if (_removingBackground
			&& e->key() == Qt::Key_Escape
			&& !e->isAutoRepeat()) {
			cancelBackground();
		}
		e->accept();
		return;
	}
	const auto key = LatinKey(e);
	const auto modifiers = e->modifiers()
		& ~(Qt::KeypadModifier | Qt::GroupSwitchModifier);
	const auto command = (modifiers & Qt::ControlModifier) != 0;
	const auto shift = (modifiers & Qt::ShiftModifier) != 0;
	if (key == Qt::Key_Escape) {
		// A held key that has just cancelled something must not go on
		// and close the editor.
		if (e->isAutoRepeat()) {
			e->accept();
			return;
		}
		const auto tool = _canvas->cropMode() ? nullptr : _controller->tool();
		if (tool && tool->cancel()) {
			_canvas->update();
		} else if (_controller->hasTemporaryTool()) {
			_controller->clearTemporaryTool();
		} else {
			requestClose();
		}
		e->accept();
		return;
	}
	if (KeyOfTextField(this, e)) {
		e->accept();
		return;
	}
	if (handleToolKey(e)) {
		e->accept();
		return;
	}
	if (e->matches(QKeySequence::Undo)
		|| (command && !shift && key == Qt::Key_Z)) {
		undo();
	} else if (e->matches(QKeySequence::Redo)
		|| (command && shift && key == Qt::Key_Z)
		|| (command && !shift && key == Qt::Key_Y)) {
		redo();
	} else if (key == Qt::Key_Backslash && !command) {
		if (!e->isAutoRepeat()) {
			setComparing(true);
		}
	} else if (key == Qt::Key_Space && !command) {
		if (!e->isAutoRepeat()) {
			_canvas->setPanMode(true);
		}
	} else if (command && (key == Qt::Key_Return || key == Qt::Key_Enter)) {
		if (!e->isAutoRepeat()) {
			done();
		}
	} else if (command && !shift && key == Qt::Key_S) {
		if (!e->isAutoRepeat()
			&& (_options.allowSaveToFile || !_options.done)) {
			saveToFile(false);
		}
	} else if (command && shift && key == Qt::Key_C) {
		if (!e->isAutoRepeat() && _options.allowCopy) {
			copyToClipboard();
		}
	} else if (command && !shift && key == Qt::Key_V) {
		// A held key would add a layer with every repeat.
		if (!e->isAutoRepeat()) {
			const auto data = QGuiApplication::clipboard()->mimeData();
			if (!data || !importFrom(data)) {
				toast(tr::lng_oblivion_photo_io_paste_empty(tr::now));
			}
		}
	} else if (e->matches(QKeySequence::ZoomIn)
		|| (command && (key == Qt::Key_Equal || key == Qt::Key_Plus))) {
		_canvas->zoomIn();
	} else if (e->matches(QKeySequence::ZoomOut)
		|| (command && (key == Qt::Key_Minus || key == Qt::Key_Underscore))) {
		_canvas->zoomOut();
	} else if (command && key == Qt::Key_0) {
		_canvas->zoomFit();
	} else if (!modifiers && key >= Qt::Key_1 && key <= Qt::Key_7) {
		// 7 is the Tool tab, the Layer tab while the tool has no options.
		selectTab(Tab(key - Qt::Key_1));
	} else if (!modifiers
		&& _tab == Tab::Filters
		&& (key == Qt::Key_Left || key == Qt::Key_Right)) {
		stepFilter((key == Qt::Key_Left) ? -1 : 1);
	} else {
		if (!modifiers && !e->isAutoRepeat() && _tab != Tab::Crop) {
			for (const auto descriptor : AllTools()) {
				if (descriptor->key
					&& descriptor->key == key
					&& (!descriptor->available
						|| descriptor->available(_controller.get()))) {
					_controller->setTool(descriptor->id);
					e->accept();
					return;
				}
			}
		}
		e->ignore();
		return;
	}
	e->accept();
}

void Editor::keyReleaseEvent(QKeyEvent *e) {
	if (e->isAutoRepeat()) {
		return;
	}
	const auto key = LatinKey(e);
	if (key == Qt::Key_Backslash) {
		setComparing(false);
	} else if (key == Qt::Key_Space) {
		_canvas->setPanMode(false);
	} else if (!_exporting && !_canvas->cropMode()) {
		if (const auto tool = _controller->tool()) {
			if (tool->keyRelease(e)) {
				e->accept();
			}
		}
	}
}

void Editor::focusOutEvent(QFocusEvent *e) {
	setComparing(false);
	_canvas->setPanMode(false);
	RpWidget::focusOutEvent(e);
}

void Editor::toast(const QString &text) {
	_layers->uiShow()->showToast(text);
}

void Editor::setExporting(bool exporting, QString text) {
	_exporting = exporting;
	_busy->setBusy(exporting, std::move(text));
	if (!exporting) {
		setFocus();
	}
}

void Editor::exportResult(Fn<void(PhotoEditorResult)> callback) {
	if (_exporting || !_sourceReady || _finished) {
		return;
	}
	commit();
	setExporting(true, tr::lng_oblivion_photo_ui_processing(tr::now));
	const auto document = std::make_shared<const Document>(
		_controller->document());
	crl::async([
			weak = base::make_weak(this),
			document,
			maxSize = _options.maxOutputSize,
			callback = std::move(callback)]() mutable {
		auto image = RenderDocument(*document, maxSize);
		// The callback is moved to the main thread lambda, so whatever it
		// holds (shows, windows...) is never released on this worker.
		crl::on_main(weak, [
				weak,
				document,
				image = std::move(image),
				callback = std::move(callback)]() mutable {
			weak->setExporting(false);
			if (image.isNull()) {
				weak->toast(tr::lng_oblivion_photo_ui_export_failed(tr::now));
				return;
			}
			auto source = QImage();
			if (IsPlainImage(*document)) {
				const auto content = AsImage(document->layers.front().content);
				if (content->original()) {
					source = content->image();
				}
			}
			callback({
				.image = std::move(image),
				.state = document->global,
				.source = std::move(source),
				.document = document,
			});
		});
	});
}

void Editor::done() {
	if (!_options.done) {
		saveToFile(true);
		return;
	}
	exportResult([=](PhotoEditorResult result) {
		const auto callback = _options.done;
		finish([=] {
			callback(result);
		});
	});
}

void Editor::saveToFile(bool closeAfter) {
	if (_exporting || !_sourceReady || _finished) {
		return;
	}
	struct Format {
		SaveFormat format;
		QString name;
	};
	auto formats = std::vector<Format>();
	for (const auto format : SupportedSaveFormats()) {
		const auto name = (format == SaveFormat::Png)
			? tr::lng_oblivion_photo_ui_format_png(tr::now)
			: (format == SaveFormat::Jpeg)
			? tr::lng_oblivion_photo_ui_format_jpeg(tr::now)
			: tr::lng_oblivion_photo_ui_format_webp(tr::now);
		formats.push_back({ format, name });
	}
	if (formats.empty()) {
		toast(tr::lng_oblivion_photo_ui_save_failed(tr::now));
		return;
	}
	const auto preferred = _hasAlpha ? SaveFormat::Png : SaveFormat::Jpeg;
	std::stable_partition(begin(formats), end(formats), [&](const Format &f) {
		return f.format == preferred;
	});
	auto filters = QStringList();
	for (const auto &format : formats) {
		const auto extension = SaveFormatExtension(format.format);
		auto patterns = u"*."_q + extension;
		if (format.format == SaveFormat::Jpeg) {
			patterns += u" *.jpeg"_q;
		}
		filters.push_back(format.name + u" ("_q + patterns + ')');
	}
	const auto baseName = _options.fileName.isEmpty()
		? tr::lng_oblivion_photo_ui_default_name(tr::now)
		: _options.fileName;
	const auto defaultFormat = formats.front().format;
	const auto initial = filedialogDefaultName(
		baseName,
		u"."_q + SaveFormatExtension(defaultFormat));
	const auto weak = base::make_weak(this);
	FileDialog::GetWritePath(
		this,
		tr::lng_oblivion_photo_ui_save_title(tr::now),
		filters.join(u";;"_q),
		initial,
		[=](QString &&chosen) {
			const auto strong = weak.get();
			if (!strong || chosen.isEmpty()) {
				return;
			}
			auto path = chosen;
			const auto suffix = QFileInfo(path).suffix().toLower();
			auto format = defaultFormat;
			if (suffix == u"png"_q) {
				format = SaveFormat::Png;
			} else if (suffix == u"jpg"_q || suffix == u"jpeg"_q) {
				format = SaveFormat::Jpeg;
			} else if (suffix == u"webp"_q) {
				format = SaveFormat::Webp;
			} else {
				path += u"."_q + SaveFormatExtension(defaultFormat);
			}
			if (!SaveFormatSupported(format)) {
				format = defaultFormat;
			}
			strong->commit();
			strong->setExporting(
				true,
				tr::lng_oblivion_photo_ui_saving(tr::now));
			crl::async([
					weak,
					document = strong->_controller->document(),
					maxSize = strong->_options.maxOutputSize,
					path,
					format,
					closeAfter] {
				const auto image = RenderDocument(document, maxSize);
				const auto ok = !image.isNull()
					&& SaveImage(image, path, format, kSaveQuality);
				crl::on_main(weak, [=] {
					weak->setExporting(false);
					if (!ok) {
						weak->toast(
							tr::lng_oblivion_photo_ui_save_failed(tr::now));
					} else if (closeAfter) {
						const auto show = weak->_outerShow;
						weak->finish([=] {
							if (show && show->valid()) {
								show->showToast(
									tr::lng_oblivion_photo_ui_saved(tr::now));
							}
						});
					} else {
						weak->toast(tr::lng_oblivion_photo_ui_saved(tr::now));
					}
				});
			});
		});
}

void Editor::copyToClipboard() {
	exportResult([=](PhotoEditorResult result) {
		QGuiApplication::clipboard()->setImage(result.image);
		toast(tr::lng_oblivion_photo_ui_copied(tr::now));
	});
}

void Editor::runAction(const PhotoEditorAction &action) {
	const auto callback = action.callback;
	const auto closeEditor = action.closeEditor;
	if (!callback) {
		return;
	}
	exportResult([=](PhotoEditorResult result) {
		if (closeEditor) {
			finish([=] {
				callback(result);
			});
		} else {
			callback(std::move(result));
		}
	});
}

void Editor::showMoreMenu() {
	if (_exporting) {
		return;
	}
	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::mediaviewPopupMenu);
	if (_sourceReady) {
		_menu->addAction(
			tr::lng_oblivion_photo_panel_import(tr::now),
			[=] { _controller->chooseAndImport(); },
			&st::mediaMenuIconShowAll);
	}
	if (_options.allowSaveToFile) {
		_menu->addAction(
			tr::lng_oblivion_photo_ui_save_file(tr::now),
			[=] { saveToFile(false); },
			&st::mediaMenuIconDownload);
	}
	if (_options.allowCopy) {
		_menu->addAction(
			tr::lng_oblivion_photo_ui_copy(tr::now),
			[=] { copyToClipboard(); },
			&st::mediaMenuIconCopy);
	}
	for (auto i = 0; i != int(_options.actions.size()); ++i) {
		_menu->addAction(
			_options.actions[i].text,
			[=] { runAction(_options.actions[i]); },
			_options.actions[i].icon);
	}
	if (_menu->empty()) {
		_menu = nullptr;
		return;
	}
	_menu->setForcedOrigin(Ui::PanelAnimation::Origin::TopRight);
	_menu->popup(_more->mapToGlobal(
		QPoint(_more->width(), _more->height())));
}

void Editor::requestClose() {
	if (_removingBackground) {
		// The first request only gives up the long work, the photo and
		// the edits stay. Nothing may be shown under the overlay anyway.
		cancelBackground();
		return;
	} else if (_exporting || _finished) {
		return;
	}
	commit();
	if (!dirty()) {
		const auto cancelled = _options.cancelled;
		finish(cancelled);
		return;
	}
	const auto weak = base::make_weak(this);
	_layers->showBox(Ui::MakeConfirmBox({
		.text = tr::lng_oblivion_photo_ui_close_text(),
		.confirmed = [=](Fn<void()> close) {
			close();
			if (const auto strong = weak.get()) {
				const auto cancelled = strong->_options.cancelled;
				strong->finish(cancelled);
			}
		},
		.confirmText = tr::lng_oblivion_photo_ui_close(),
		.title = tr::lng_oblivion_photo_ui_close_title(),
	}));
}

void Editor::finish(Fn<void()> after) {
	if (_finished) {
		return;
	}
	_finished = true;
	// What is under the editor gets the focus next.
	GuardHeldKeys();
	crl::on_main(this, [=] {
		_closeRequests.fire({});
		if (after) {
			after();
		}
	});
}

void Editor::scrollTabToEnd() {
	if (_pages.empty() || _tab == Tab::Layers) {
		return;
	}
	const auto scroll = _pages[int(_tab)].scroll;
	scroll->scrollToY(scroll->scrollTopMax());
}

// The highlight of the tab bar slides to the tab a scene has chosen: in
// a picture it is where it is going.
void Editor::finishAnimations() {
	_tabs->finishAnimating();
}

bool Editor::snapshotReady() const {
	const auto ready = _sourceReady
		&& _canvas->hasImage()
		&& _shownAny
		&& (_shownKey == displayKey())
		&& (_tab != Tab::Filters || _strip->thumbnailsReady())
		&& (!_comparing || _beforeValid)
		&& !_controller->busy()
		&& !_controller->thumbnailsPending()
		&& _busy->settled();
	if (!ready) {
		_snapshotSettledFrom = 0;
		return false;
	}
	// The picture is there, but the highlight of the tab bar and the
	// switches are still sliding and the thumbnails of the layers list
	// are asked for with a delay: a scene taken right now showed them
	// half way. Give them time to finish.
	const auto now = crl::now();
	if (!_snapshotSettledFrom) {
		_snapshotSettledFrom = now;
	}
	return (now - _snapshotSettledFrom >= kSceneSettle);
}

class EditorLayer final : public Ui::LayerWidget {
public:
	EditorLayer(
		QWidget *parent,
		std::shared_ptr<Ui::Show> show,
		QImage image,
		PhotoEditorOptions options);

	void parentResized() override;
	bool closeByOutsideClick() const override;
	bool closeByBackButton() override;

protected:
	void doSetInnerFocus() override;
	int resizeGetHeight(int newWidth) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	const not_null<Editor*> _editor;

};

EditorLayer::EditorLayer(
	QWidget *parent,
	std::shared_ptr<Ui::Show> show,
	QImage image,
	PhotoEditorOptions options)
: LayerWidget(parent)
, _editor(Ui::CreateChild<Editor>(
	this,
	std::move(show),
	std::move(image),
	std::move(options))) {
	_editor->show();
	sizeValue() | rpl::on_next([=](QSize size) {
		_editor->setGeometry(QRect(QPoint(), size));
	}, lifetime());
	_editor->closeRequests() | rpl::on_next([=] {
		closeLayer();
	}, lifetime());
}

void EditorLayer::parentResized() {
	resizeToWidth(parentWidget()->width());
}

bool EditorLayer::closeByOutsideClick() const {
	return false;
}

bool EditorLayer::closeByBackButton() {
	_editor->requestClose();
	return true;
}

void EditorLayer::doSetInnerFocus() {
	_editor->setFocus();
}

int EditorLayer::resizeGetHeight(int newWidth) {
	return parentWidget() ? parentWidget()->height() : height();
}

void EditorLayer::keyPressEvent(QKeyEvent *e) {
	// The layer stack closes every layer on Escape, the boxes under the
	// editor too (the send files box) and without asking about the edits.
	// It gets here only if the focus is outside of the editor itself.
	if (e->key() == Qt::Key_Escape) {
		e->accept();
		if (!e->isAutoRepeat()) {
			_editor->requestClose();
		}
		return;
	}
	LayerWidget::keyPressEvent(e);
}

[[nodiscard]] QImage SampleImage() {
	auto result = LoadImage(u":/gui/art/themeimage.jpg"_q);
	if (!result.isNull()) {
		return result;
	}
	result = QImage(960, 640, QImage::Format_ARGB32_Premultiplied);
	auto p = QPainter(&result);
	auto gradient = QLinearGradient(0, 0, 0, result.height());
	gradient.setColorAt(0., QColor(64, 132, 214));
	gradient.setColorAt(0.6, QColor(250, 196, 120));
	gradient.setColorAt(1., QColor(60, 90, 60));
	p.fillRect(result.rect(), gradient);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(255, 236, 170));
	p.drawEllipse(QPoint(700, 220), 70, 70);
	p.end();
	return result;
}

[[nodiscard]] QWidget *CreateScene(
		not_null<Ui::RpWidget*> parent,
		PhotoEditorOptions options) {
	options.done = [](PhotoEditorResult) {};
	return Ui::CreateChild<Editor>(
		parent.get(),
		SelfTest::SceneShow(parent),
		SampleImage(),
		std::move(options));
}

void RegisterOptionsScene(
		QString name,
		QSize size,
		Fn<PhotoEditorOptions()> options,
		Fn<void(not_null<Editor*>)> prepare = nullptr,
		crl::time wait = kSceneWait) {
	SelfTest::RegisterScene(SelfTest::SceneDescriptor{
		.name = std::move(name),
		.size = size,
		.create = [=](not_null<Ui::RpWidget*> parent) {
			return CreateScene(parent, options());
		},
		.prepare = [=](not_null<QWidget*> widget) {
			const auto editor = static_cast<Editor*>(widget.get());
			if (prepare) {
				prepare(editor);
			}
			editor->finishAnimations();
		},
		.ready = [](not_null<QWidget*> widget) {
			return static_cast<Editor*>(widget.get())->snapshotReady();
		},
		.wait = wait,
	});
}

// A panel of the editor alone, see RegisterPanelScene().
class PanelSceneHost final : public Ui::RpWidget {
public:
	PanelSceneHost(
		QWidget *parent,
		std::shared_ptr<Ui::Show> show,
		const PanelSceneArgs &args);
	~PanelSceneHost();

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	const int _fixedHeight = 0;
	std::unique_ptr<Controller> _controller;
	Ui::RpWidget *_panel = nullptr;

};

PanelSceneHost::PanelSceneHost(
	QWidget *parent,
	std::shared_ptr<Ui::Show> show,
	const PanelSceneArgs &args)
: RpWidget(parent)
, _fixedHeight(args.size.height())
, _controller(std::make_unique<Controller>(std::move(show))) {
	_controller->setDocument(args.document
		? args.document()
		: SampleSceneDocument());
	if (args.prepare) {
		args.prepare(_controller.get());
	}
	if (args.create) {
		auto panel = args.create(this, _controller.get());
		_panel = panel.release();
	}
	if (_panel) {
		_panel->show();
		_panel->heightValue() | rpl::skip(1) | rpl::on_next([=] {
			if (!_fixedHeight) {
				resizeToWidth(width());
			}
		}, _panel->lifetime());
	}
}

PanelSceneHost::~PanelSceneHost() {
	_controller->shutdown();
	delete base::take(_panel);
}

void PanelSceneHost::paintEvent(QPaintEvent *e) {
	QPainter(this).fillRect(e->rect(), st::groupCallMembersBg);
}

int PanelSceneHost::resizeGetHeight(int newWidth) {
	if (!_panel) {
		return _fixedHeight;
	} else if (_fixedHeight) {
		_panel->setGeometry(0, 0, newWidth, _fixedHeight);
		return _fixedHeight;
	}
	_panel->resizeToWidth(newWidth);
	_panel->moveToLeft(0, 0, newWidth);
	return _panel->height();
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	const auto wide = QSize(1120, 720);
	RegisterOptionsScene(u"photo_editor_adjust"_q, wide, [] {
		auto options = PhotoEditorOptions();
		options.tab = Tab::Adjust;
		options.state.exposure = 12;
		options.state.contrast = 18;
		options.state.shadows = 25;
		options.state.temperature = 15;
		options.state.saturation = 10;
		options.state.vignette = 30;
		return options;
	});
	RegisterOptionsScene(u"photo_editor_filters"_q, wide, [] {
		auto options = PhotoEditorOptions();
		options.tab = Tab::Filters;
		options.state.filter = u"film"_q;
		options.state.filterIntensity = 80;
		return options;
	});
	RegisterOptionsScene(u"photo_editor_crop"_q, wide, [] {
		auto options = PhotoEditorOptions();
		options.tab = Tab::Crop;
		options.state.crop = QRectF(0.1, 0.05, 0.72, 0.9);
		options.state.straighten = 2.5;
		return options;
	});
	RegisterOptionsScene(u"photo_editor_effects"_q, wide, [] {
		auto options = PhotoEditorOptions();
		options.tab = Tab::Effects;
		auto glitch = DefaultEffect(EffectType::Glitch);
		glitch.amount = 45;
		options.state.effects.push_back(glitch);
		options.state.effects.push_back(DefaultEffect(EffectType::Duotone));
		return options;
	});
	// The overlay of a running background removal, with the hint about
	// the slow first run that shows up after kBusySlowHintDelay.
	RegisterOptionsScene(u"photo_editor_background_busy"_q, wide, [] {
		auto options = PhotoEditorOptions();
		options.tab = Tab::Effects;
		return options;
	}, [](not_null<Editor*> editor) {
		editor->showBackgroundProgress();
	}, kBusySlowHintDelay + kSceneWait);
	RegisterOptionsScene(u"photo_editor_compare"_q, wide, [] {
		auto options = PhotoEditorOptions();
		options.tab = Tab::Adjust;
		options.state.filter = u"noir"_q;
		return options;
	}, [](not_null<Editor*> editor) {
		editor->setComparing(true);
	});
	RegisterOptionsScene(u"photo_editor_narrow"_q, QSize(520, 820), [] {
		auto options = PhotoEditorOptions();
		options.tab = Tab::Filters;
		options.state.filter = u"vivid"_q;
		return options;
	});
	// The canvas size section at the end of the Crop page.
	RegisterOptionsScene(u"photo_editor_crop_canvas"_q, wide, [] {
		auto options = PhotoEditorOptions();
		options.tab = Tab::Crop;
		return options;
	}, [](not_null<Editor*> editor) {
		editor->scrollTabToEnd();
	});
	// The ends of the whole-picture pages: the way from them to the
	// effects of the active layer.
	RegisterOptionsScene(u"photo_editor_adjust_fine"_q, wide, [] {
		auto options = PhotoEditorOptions();
		options.tab = Tab::Adjust;
		return options;
	}, [](not_null<Editor*> editor) {
		editor->scrollTabToEnd();
	});
	RegisterOptionsScene(u"photo_editor_effects_more"_q, wide, [] {
		auto options = PhotoEditorOptions();
		options.tab = Tab::Effects;
		options.state.effects.push_back(DefaultEffect(EffectType::Sepia));
		return options;
	}, [](not_null<Editor*> editor) {
		editor->scrollTabToEnd();
	});

	// The layered shell: three layers, the Layer tab with the effects of
	// the active one, the layers list under it.
	RegisterEditorScene({
		.name = u"photo_editor_layers"_q,
		.document = SampleSceneDocument,
		.tab = Tab::Layer,
		.prepare = [](not_null<Controller*> controller) {
			const auto &layers = controller->document().layers;
			if (layers.size() > 1) {
				controller->setActiveLayer(layers[1].id);
			}
		},
	});
	RegisterEditorScene({
		.name = u"photo_editor_layers_adjust"_q,
		.document = [] {
			auto document = SampleSceneDocument();
			document.global.contrast = 20;
			document.global.crop = QRectF(0.05, 0.05, 0.9, 0.85);
			return document;
		},
		.tab = Tab::Adjust,
	});
	RegisterEditorScene({
		.name = u"photo_editor_layers_narrow"_q,
		.size = QSize(520, 820),
		.document = SampleSceneDocument,
		.tab = Tab::Layers,
	});
	// What a just opened photo looks like on the Layer tab: no effects
	// yet, one row in the layers list (the list is as tall as that row).
	RegisterEditorScene({
		.name = u"photo_editor_layer_plain"_q,
		.tab = Tab::Layer,
	});
	// The Auto tab before the button was pressed.
	RegisterOptionsScene(u"photo_editor_auto"_q, wide, [] {
		auto options = PhotoEditorOptions();
		options.tab = Tab::Auto;
		return options;
	});
	// More layers than the list can show: it stops growing and scrolls,
	// the page above keeps its place.
	RegisterEditorScene({
		.name = u"photo_editor_layers_many"_q,
		.document = [] {
			auto document = SampleSceneDocument();
			if (document.empty()) {
				return document;
			}
			const auto canvas = document.size;
			for (auto i = 0; i != 6; ++i) {
				auto layer = MakeImageLayer(
					FxTestImage(
						std::max(canvas.width() / 5, 8),
						std::max(canvas.height() / 5, 8),
						(i % 2) == 1),
					NewLayerName(document));
				layer.transform = QTransform::fromTranslate(
					canvas.width() * (0.06 + 0.13 * i),
					canvas.height() * (0.08 + 0.06 * (i % 3)));
				layer.opacity = 1. - 0.15 * (i % 3);
				AddLayer(document, std::move(layer));
			}
			return document;
		},
		.tab = Tab::Adjust,
	});
	// A picture of a camera size (about 10 MP): it is scaled down to fit,
	// while the small sample of the other scenes is enlarged.
	RegisterEditorScene({
		.name = u"photo_editor_large"_q,
		.document = [] {
			return DocumentFromImage(
				SampleImage().scaledToWidth(4032, Qt::SmoothTransformation),
				EditState(),
				tr::lng_oblivion_photo_panel_layer_photo(tr::now));
		},
		.tab = Tab::Layer,
	});
	// The question about closing with changes nobody got a result of.
	RegisterOptionsScene(u"photo_editor_close_confirm"_q, wide, [] {
		auto options = PhotoEditorOptions();
		options.tab = Tab::Adjust;
		options.document = std::make_shared<const Document>(
			DocumentFromImage(
				SampleImage(),
				EditState(),
				tr::lng_oblivion_photo_panel_layer_photo(tr::now)));
		options.unsaved = true;
		return options;
	}, [](not_null<Editor*> editor) {
		editor->requestClose();
	});
});

} // namespace

int KeepInterruptedPhotoEdit(InterruptedPhotoEdit edit) {
	auto &slot = Interrupted();
	if (!edit.document || edit.document->empty()) {
		return 0;
	}
	slot.edit = std::move(edit);
	slot.id = ++slot.lastId;
	return slot.id;
}

int InterruptedPhotoEditId() {
	return Interrupted().id;
}

InterruptedPhotoEdit TakeInterruptedPhotoEdit() {
	auto &slot = Interrupted();
	slot.id = 0;
	return base::take(slot.edit);
}

void DropInterruptedPhotoEdit(int id) {
	auto &slot = Interrupted();
	if (!id || slot.id == id) {
		slot.id = 0;
		slot.edit = InterruptedPhotoEdit();
	}
}

void RegisterTool(ToolDescriptor &&descriptor) {
	auto &registry = EditorRegistryData();
	if (!registry.building
		|| descriptor.id.isEmpty()
		|| descriptor.id == kViewTool
		|| !descriptor.create) {
		return;
	}
	for (const auto &existing : registry.tools) {
		if (existing->id == descriptor.id) {
			return;
		}
	}
	registry.tools.push_back(
		std::make_unique<ToolDescriptor>(std::move(descriptor)));
}

void RegisterPanel(PanelDescriptor &&descriptor) {
	auto &registry = EditorRegistryData();
	if (!registry.building || !descriptor.create) {
		return;
	}
	registry.panels.push_back(
		std::make_unique<PanelDescriptor>(std::move(descriptor)));
}

void RegisterLayerKind(LayerKindDescriptor &&descriptor) {
	auto &registry = EditorRegistryData();
	if (!registry.building || descriptor.type.isEmpty()) {
		return;
	}
	for (const auto &existing : registry.kinds) {
		if (existing->type == descriptor.type) {
			return;
		}
	}
	registry.kinds.push_back(
		std::make_unique<LayerKindDescriptor>(std::move(descriptor)));
}

EditorRegistrar::EditorRegistrar(Fn<void()> registerAll) {
	EditorRegistrars().push_back(std::move(registerAll));
}

const std::vector<const ToolDescriptor*> &AllTools() {
	return BuiltEditorRegistry().sortedTools;
}

const ToolDescriptor *FindTool(QByteArrayView id) {
	for (const auto descriptor : AllTools()) {
		if (QByteArrayView(descriptor->id) == id) {
			return descriptor;
		}
	}
	return nullptr;
}

const std::vector<const PanelDescriptor*> &AllPanels() {
	return BuiltEditorRegistry().sortedPanels;
}

const std::vector<const LayerKindDescriptor*> &AllLayerKinds() {
	return BuiltEditorRegistry().sortedKinds;
}

const LayerKindDescriptor *FindLayerKind(QByteArrayView type) {
	for (const auto descriptor : AllLayerKinds()) {
		if (QByteArrayView(descriptor->type) == type) {
			return descriptor;
		}
	}
	return nullptr;
}

Controller::Controller(std::shared_ptr<Ui::Show> show)
: _show(std::move(show))
, _compositor(std::make_shared<Compositor>())
, _commitTimer([=] { commit(); })
, _toolId(kViewTool) {
}

Controller::~Controller() {
	shutdown();
}

const Document &Controller::document() const {
	return _document;
}

bool Controller::hasDocument() const {
	return _hasDocument;
}

rpl::producer<> Controller::documentChanges() const {
	return _documentChanges.events();
}

uint64 Controller::revision() const {
	return _revision;
}

uint64 Controller::layersRevision() const {
	return _layersRevision;
}

void Controller::changed(bool layers) {
	++_revision;
	if (layers) {
		++_layersRevision;
	}
	validateActiveLayer();
	_documentChanges.fire({});
}

void Controller::validateActiveLayer() {
	if (_document.find(_activeLayer.current())) {
		return;
	}
	const auto top = _document.top();
	_activeLayer = top ? top->id : LayerId(0);
}

void Controller::apply(Document document, bool commitNow) {
	if (_shutdown || !_hasDocument) {
		return;
	}
	const auto different = !(document == _document);
	if (different) {
		const auto layers = (document.size != _document.size)
			|| !(document.layers == _document.layers);
		// Ids are never reused, whatever copy the change was made from.
		document.nextId = std::max(document.nextId, _document.nextId);
		_document = std::move(document);
		changed(layers);
	}
	if (commitNow) {
		commit();
	} else if (different && !_interacting) {
		_commitTimer.callOnce(CommitDelay());
	}
}

void Controller::change(Fn<void(Document&)> modify, bool commitNow) {
	if (!modify || !_hasDocument) {
		return;
	}
	auto document = _document;
	modify(document);
	apply(std::move(document), commitNow);
}

void Controller::changeLayer(
		LayerId id,
		Fn<void(Layer&)> modify,
		bool commitNow) {
	if (!modify || !_document.find(id)) {
		return;
	}
	auto document = _document;
	modify(*document.find(id));
	apply(std::move(document), commitNow);
}

void Controller::changeFx(
		LayerId id,
		uint64 uid,
		Fn<void(FxInstance&)> modify,
		bool commitNow) {
	if (!modify || !LayerFx(_document, id, uid)) {
		return;
	}
	auto document = _document;
	const auto instance = LayerFx(document, id, uid);
	const auto keep = instance->uid;
	modify(*instance);
	instance->uid = keep;
	apply(std::move(document), commitNow);
}

void Controller::commit() {
	_commitTimer.cancel();
	if (_shutdown || !_hasDocument) {
		return;
	} else if (_history.push(_document)) {
		_historyChanges.fire({});
	}
}

bool Controller::canUndo() const {
	return _hasDocument
		&& (_history.canUndo() || !(_history.current() == _document));
}

bool Controller::canRedo() const {
	return _hasDocument && _history.canRedo();
}

void Controller::undo() {
	if (_shutdown || !_hasDocument) {
		return;
	}
	commit();
	if (!_history.undo()) {
		return;
	}
	const auto &target = _history.current();
	const auto layers = (target.size != _document.size)
		|| !(target.layers == _document.layers);
	const auto nextId = _document.nextId;
	_document = target;
	_document.nextId = std::max(_document.nextId, nextId);
	changed(layers);
	_historyChanges.fire({});
}

void Controller::redo() {
	if (_shutdown || !_hasDocument) {
		return;
	}
	commit();
	if (!_history.redo()) {
		return;
	}
	const auto &target = _history.current();
	const auto layers = (target.size != _document.size)
		|| !(target.layers == _document.layers);
	const auto nextId = _document.nextId;
	_document = target;
	_document.nextId = std::max(_document.nextId, nextId);
	changed(layers);
	_historyChanges.fire({});
}

rpl::producer<> Controller::historyChanges() const {
	return _historyChanges.events();
}

void Controller::setDocument(Document document) {
	if (_shutdown) {
		return;
	}
	_commitTimer.cancel();
	document.nextId = std::max(document.nextId, _document.nextId);
	_document = std::move(document);
	_initial = _document;
	_history.reset(_document);
	_hasDocument = !_document.empty();
	const auto top = _document.top();
	_activeLayer = top ? top->id : LayerId(0);
	changed(true);
	_historyChanges.fire({});
}

const Document &Controller::initialDocument() const {
	return _initial;
}

bool Controller::modified() const {
	return _hasDocument && !(_document == _initial);
}

LayerId Controller::activeLayerId() const {
	return _activeLayer.current();
}

const Layer *Controller::activeLayer() const {
	return _document.find(_activeLayer.current());
}

void Controller::setActiveLayer(LayerId id) {
	if (_document.find(id)) {
		_activeLayer = id;
	}
}

rpl::producer<LayerId> Controller::activeLayerValue() const {
	return _activeLayer.value();
}

LayerId Controller::addLayer(Layer layer) {
	if (_shutdown || !_hasDocument || !layer.content) {
		return 0;
	}
	if (layer.name.isEmpty()) {
		layer.name = NewLayerName(_document);
	}
	auto document = _document;
	const auto index = document.indexOf(_activeLayer.current());
	const auto id = AddLayer(
		document,
		std::move(layer),
		(index >= 0) ? (index + 1) : -1);
	_document = std::move(document);
	_activeLayer = id;
	changed(true);
	commit();
	return id;
}

LayerId Controller::addImageLayer(QImage image, QString name) {
	if (image.isNull() || !_hasDocument) {
		return 0;
	}
	auto layer = MakeImageLayer(std::move(image), std::move(name));
	layer.transform = PlaceTransform(
		layer.size(),
		_document.size,
		Placement::Fit);
	const auto result = addLayer(std::move(layer));
	if (result) {
		placeAdded();
	}
	return result;
}

// A picture that was just added is most likely about to be moved and
// resized: the plain view tool gives way to the transform tool (a brush or
// the collage tool stays what it is).
void Controller::placeAdded() {
	static const auto kPlaceTool = QByteArray("layer.transform");
	if (_shutdown || _temporaryTool || _toolId.current() != kViewTool) {
		return;
	}
	const auto descriptor = FindTool(kPlaceTool);
	if (descriptor
		&& (!descriptor->available || descriptor->available(this))) {
		setTool(kPlaceTool);
	}
}

void Controller::removeLayer(LayerId id) {
	change([=](Document &document) {
		RemoveLayer(document, id);
	});
}

LayerId Controller::duplicateLayer(LayerId id) {
	if (_shutdown || !_hasDocument) {
		return 0;
	}
	auto document = _document;
	const auto copy = DuplicateLayer(document, id);
	if (!copy) {
		return 0;
	}
	_document = std::move(document);
	_activeLayer = copy;
	changed(true);
	commit();
	return copy;
}

uint64 Controller::addFx(LayerId id, FxInstance instance) {
	if (_shutdown || !_hasDocument) {
		return 0;
	}
	auto document = _document;
	const auto uid = AddLayerFx(document, id, std::move(instance));
	if (uid) {
		apply(std::move(document), true);
	}
	return uid;
}

FxValue Controller::fxParam(
		LayerId id,
		uint64 uid,
		QByteArrayView param) const {
	const auto instance = LayerFx(_document, id, uid);
	if (!instance) {
		return FxValue();
	}
	const auto descriptor = FindFx(instance->id);
	const auto info = descriptor ? descriptor->param(param) : nullptr;
	return info
		? info->normalized(instance->params.value(param))
		: instance->params.value(param);
}

void Controller::setFxParam(
		LayerId id,
		uint64 uid,
		const QByteArray &param,
		FxValue value,
		bool commitNow) {
	changeFx(id, uid, [&](FxInstance &instance) {
		instance.params.set(param, std::move(value));
	}, commitNow);
}

void Controller::chooseAndImport() {
	const auto weak = base::make_weak(this);
	chooseImages([=](std::vector<ImportedImage> images) {
		if (const auto strong = weak.get()) {
			strong->addImported(std::move(images));
		}
	});
}

void Controller::importFiles(const QStringList &paths) {
	const auto weak = base::make_weak(this);
	loadImages(paths, [=](std::vector<ImportedImage> images) {
		if (const auto strong = weak.get()) {
			strong->addImported(std::move(images));
		}
	});
}

void Controller::addImported(std::vector<ImportedImage> images) {
	if (_shutdown || !_hasDocument || images.empty()) {
		return;
	}
	auto document = _document;
	auto index = document.indexOf(_activeLayer.current());
	auto last = LayerId(0);
	for (auto &entry : images) {
		auto layer = MakeImageLayer(
			std::move(entry.image),
			entry.name.isEmpty() ? NewLayerName(document) : entry.name);
		layer.transform = PlaceTransform(
			layer.size(),
			document.size,
			Placement::Fit);
		last = AddLayer(
			document,
			std::move(layer),
			(index >= 0) ? ++index : -1);
	}
	_document = std::move(document);
	_activeLayer = last;
	changed(true);
	commit();
	placeAdded();
}

void Controller::chooseImages(
		Fn<void(std::vector<ImportedImage> images)> done,
		bool multiple) {
	if (_shutdown || !_show || !_show->valid() || !done) {
		return;
	}
	const auto weak = base::make_weak(this);
	const auto callback = [=](FileDialog::OpenResult &&result) {
		if (const auto strong = weak.get()) {
			strong->loadImages(result.paths, done);
		}
	};
	if (multiple) {
		FileDialog::GetOpenPaths(
			_show->toastParent().get(),
			tr::lng_oblivion_photo_panel_import_title(tr::now),
			FileDialog::ImagesFilter(),
			callback);
	} else {
		FileDialog::GetOpenPath(
			_show->toastParent().get(),
			tr::lng_oblivion_photo_ui_open_title(tr::now),
			FileDialog::ImagesFilter(),
			callback);
	}
}

void Controller::loadImages(
		const QStringList &paths,
		Fn<void(std::vector<ImportedImage> images)> done) {
	if (_shutdown || paths.isEmpty() || busy() || !done) {
		return;
	}
	_busy = tr::lng_oblivion_photo_io_loading(tr::now);
	crl::async([
			weak = base::make_weak(this),
			list = paths.mid(0, kImportLimit),
			requested = int(paths.size()),
			done = std::move(done)]() mutable {
		auto loaded = std::vector<ImportedImage>();
		for (const auto &path : list) {
			auto image = LoadImage(path);
			if (!image.isNull()) {
				loaded.push_back({
					std::move(image),
					QFileInfo(path).completeBaseName(),
				});
			}
		}
		crl::on_main(weak, [
				weak,
				requested,
				loaded = std::move(loaded),
				done = std::move(done)]() mutable {
			weak->_busy = QString();
			if (weak->_shutdown) {
				return;
			} else if (loaded.empty()) {
				weak->showToast(
					tr::lng_oblivion_photo_ui_open_failed(tr::now));
				return;
			} else if (int(loaded.size()) < requested) {
				// Over the limit of one import or not opened: said, not
				// left out silently.
				weak->showToast(tr::lng_oblivion_photo_panel_import_partial(
					tr::now,
					lt_added,
					QString::number(loaded.size()),
					lt_total,
					QString::number(requested),
					lt_limit,
					QString::number(kImportLimit)));
			}
			done(std::move(loaded));
		});
	});
}

void Controller::runBusy(
		QString text,
		Fn<std::optional<Document>(const Document &document)> work,
		Fn<void(bool applied)> done) {
	if (_shutdown || !_hasDocument || busy() || !work) {
		if (done) {
			done(false);
		}
		return;
	}
	commit();
	_busy = text.isEmpty()
		? tr::lng_oblivion_photo_ui_processing(tr::now)
		: text;
	crl::async([
			weak = base::make_weak(this),
			document = _document,
			revision = _revision,
			work = std::move(work),
			done = std::move(done)]() mutable {
		auto result = work(document);
		crl::on_main(weak, [
				weak,
				revision,
				result = std::move(result),
				done = std::move(done)]() mutable {
			weak->_busy = QString();
			const auto fits = result.has_value()
				&& !weak->_shutdown
				&& (weak->_revision == revision);
			if (fits) {
				weak->apply(std::move(*result), true);
			}
			if (done) {
				done(fits);
			}
		});
	});
}

rpl::producer<QString> Controller::busyValue() const {
	return _busy.value();
}

bool Controller::busy() const {
	return !_busy.current().isEmpty();
}

QByteArray Controller::toolId() const {
	return _toolId.current();
}

void Controller::setTool(const QByteArray &id) {
	if (_shutdown) {
		return;
	}
	const auto descriptor = (id == kViewTool) ? nullptr : FindTool(id);
	const auto real = descriptor ? descriptor->id : kViewTool;
	clearTemporaryTool();
	if (_toolId.current() == real) {
		return;
	}
	if (_tool) {
		_tool->deactivated();
	}
	auto old = std::move(_tool);
	_tool = descriptor ? descriptor->create(this) : nullptr;
	if (_tool) {
		_tool->activated();
	}
	_toolId = real;
	_toolChanges.fire({});
	updateCanvas();
}

rpl::producer<QByteArray> Controller::toolValue() const {
	return _toolId.value();
}

Tool *Controller::tool() const {
	return _temporaryTool ? _temporaryTool.get() : _tool.get();
}

void Controller::setTemporaryTool(
		std::unique_ptr<Tool> tool,
		Fn<void()> finished) {
	clearTemporaryTool();
	if (_shutdown || !tool) {
		return;
	}
	_temporaryTool = std::move(tool);
	_temporaryFinished = std::move(finished);
	_temporaryTool->activated();
	_toolChanges.fire({});
	updateCanvas();
}

void Controller::clearTemporaryTool() {
	if (!_temporaryTool) {
		return;
	}
	_temporaryTool->deactivated();
	const auto old = std::move(_temporaryTool);
	const auto finished = base::take(_temporaryFinished);
	_toolChanges.fire({});
	updateCanvas();
	if (finished) {
		finished();
	}
}

bool Controller::hasTemporaryTool() const {
	return (_temporaryTool != nullptr);
}

rpl::producer<> Controller::toolChanges() const {
	return _toolChanges.events();
}

void Controller::showToolOptions() {
	if (!_shutdown) {
		_toolOptionsRequests.fire({});
	}
}

rpl::producer<> Controller::toolOptionsRequests() const {
	return _toolOptionsRequests.events();
}

void Controller::showLayerEffects() {
	if (!_shutdown) {
		_layerEffectsRequests.fire({});
	}
}

rpl::producer<> Controller::layerEffectsRequests() const {
	return _layerEffectsRequests.events();
}

void Controller::revealInPanel(not_null<QWidget*> widget) {
	if (!_shutdown) {
		_revealRequests.fire(QPointer<QWidget>(widget.get()));
	}
}

rpl::producer<QPointer<QWidget>> Controller::revealRequests() const {
	return _revealRequests.events();
}

QTransform Controller::documentToWidget() const {
	return _view.documentToWidget ? _view.documentToWidget() : QTransform();
}

QTransform Controller::widgetToDocument() const {
	auto invertible = false;
	const auto result = documentToWidget().inverted(&invertible);
	return invertible ? result : QTransform();
}

double Controller::viewScale() const {
	const auto determinant = std::abs(documentToWidget().determinant());
	return (determinant > 0.) ? std::sqrt(determinant) : 1.;
}

rpl::producer<> Controller::viewChanges() const {
	return _viewChanges.events();
}

void Controller::updateCanvas() {
	if (_view.update) {
		_view.update();
	}
}

std::shared_ptr<Compositor> Controller::compositor() const {
	return _compositor;
}

void Controller::requestLayerThumbnail(
		LayerId id,
		QSize size,
		Fn<void(QImage image)> done) {
	const auto layer = _document.find(id);
	if (!done) {
		return;
	} else if (!layer || !layer->content || size.isEmpty()) {
		done(QImage());
		return;
	}
	const auto full = layer->size();
	if (full.isEmpty()) {
		done(QImage());
		return;
	}
	// A few fixed steps of the scale, so thumbnails of slightly different
	// sizes share the cached pixels. The layer is rendered larger than
	// the thumbnail, so that thin strokes are still there to be kept by
	// LayerThumbnail(). Not much larger while it has effects: they are
	// applied at that size after every change.
	const auto plain = ranges::all_of(layer->effects, FxIsIdentity);
	const auto exact = std::min({
		size.width() / double(full.width()),
		size.height() / double(full.height()),
		1.,
	}) * (plain ? kThumbnailOversample : 2);
	const auto scale = std::min(
		std::pow(2., std::ceil(std::log2(std::max(exact, 1e-4)))),
		1.);
	++_thumbnailsPending;
	crl::async([
			weak = base::make_weak(this),
			compositor = _compositor,
			copy = *layer,
			scale,
			size,
			done = std::move(done)]() mutable {
		auto image = LayerThumbnail(
			compositor->layerPixels(copy, scale),
			size);
		crl::on_main(weak, [
				weak,
				image = std::move(image),
				done = std::move(done)]() mutable {
			--weak->_thumbnailsPending;
			done(std::move(image));
		});
	});
}

bool Controller::thumbnailsPending() const {
	return (_thumbnailsPending > 0);
}

std::shared_ptr<Ui::Show> Controller::uiShow() const {
	return _show;
}

void Controller::showToast(const QString &text) {
	if (_show && _show->valid()) {
		_show->showToast(text);
	}
}

rpl::lifetime &Controller::lifetime() {
	return _lifetime;
}

void Controller::setView(View view) {
	_view = std::move(view);
	_viewChanges.fire({});
}

void Controller::notifyViewChanged() {
	_viewChanges.fire({});
}

uint64 Controller::shownRevision() const {
	return _shownRevision.current();
}

rpl::producer<uint64> Controller::shownRevisionValue() const {
	return _shownRevision.value();
}

void Controller::setShownRevision(uint64 revision) {
	if (!_shutdown) {
		_shownRevision = revision;
	}
}

bool Controller::interacting() const {
	return _interacting;
}

void Controller::setInteracting(bool interacting) {
	if (_interacting == interacting || _shutdown) {
		return;
	}
	if (interacting) {
		// What was changed before the press is a step of its own.
		commit();
		_interacting = true;
	} else {
		_interacting = false;
		if (_hasDocument && !(_history.current() == _document)) {
			_commitTimer.callOnce(CommitDelay());
		}
	}
}

void Controller::shutdown() {
	if (_shutdown) {
		return;
	}
	_shutdown = true;
	_interacting = false;
	_commitTimer.cancel();
	if (_temporaryTool) {
		_temporaryTool->deactivated();
	}
	if (_tool) {
		_tool->deactivated();
	}
	_view = View();
	_temporaryFinished = nullptr;
	_temporaryTool = nullptr;
	_tool = nullptr;
}

QString BlendModeName(BlendMode mode) {
	switch (mode) {
	case BlendMode::Normal:
		return tr::lng_oblivion_photo_panel_blend_normal(tr::now);
	case BlendMode::Multiply:
		return tr::lng_oblivion_photo_panel_blend_multiply(tr::now);
	case BlendMode::Screen:
		return tr::lng_oblivion_photo_panel_blend_screen(tr::now);
	case BlendMode::Overlay:
		return tr::lng_oblivion_photo_panel_blend_overlay(tr::now);
	case BlendMode::SoftLight:
		return tr::lng_oblivion_photo_panel_blend_soft_light(tr::now);
	case BlendMode::HardLight:
		return tr::lng_oblivion_photo_panel_blend_hard_light(tr::now);
	case BlendMode::Darken:
		return tr::lng_oblivion_photo_panel_blend_darken(tr::now);
	case BlendMode::Lighten:
		return tr::lng_oblivion_photo_panel_blend_lighten(tr::now);
	case BlendMode::ColorDodge:
		return tr::lng_oblivion_photo_panel_blend_color_dodge(tr::now);
	case BlendMode::ColorBurn:
		return tr::lng_oblivion_photo_panel_blend_color_burn(tr::now);
	case BlendMode::Difference:
		return tr::lng_oblivion_photo_panel_blend_difference(tr::now);
	case BlendMode::Exclusion:
		return tr::lng_oblivion_photo_panel_blend_exclusion(tr::now);
	case BlendMode::Add:
		return tr::lng_oblivion_photo_panel_blend_add(tr::now);
	case BlendMode::Hue:
		return tr::lng_oblivion_photo_panel_blend_hue(tr::now);
	case BlendMode::Saturation:
		return tr::lng_oblivion_photo_panel_blend_saturation(tr::now);
	case BlendMode::Color:
		return tr::lng_oblivion_photo_panel_blend_color(tr::now);
	case BlendMode::Luminosity:
		return tr::lng_oblivion_photo_panel_blend_luminosity(tr::now);
	}
	return QString();
}

QString FxGroupName(FxGroup group) {
	switch (group) {
	case FxGroup::Light:
		return tr::lng_oblivion_photo_panel_group_light(tr::now);
	case FxGroup::Color:
		return tr::lng_oblivion_photo_panel_group_color(tr::now);
	case FxGroup::Detail:
		return tr::lng_oblivion_photo_panel_group_detail(tr::now);
	case FxGroup::Finish:
		return tr::lng_oblivion_photo_panel_group_finish(tr::now);
	case FxGroup::Blur:
		return tr::lng_oblivion_photo_panel_group_blur(tr::now);
	case FxGroup::Distort:
		return tr::lng_oblivion_photo_panel_group_distort(tr::now);
	case FxGroup::Lofi:
		return tr::lng_oblivion_photo_panel_group_lofi(tr::now);
	case FxGroup::Glitch:
		return tr::lng_oblivion_photo_panel_group_glitch(tr::now);
	case FxGroup::Stylize:
		return tr::lng_oblivion_photo_panel_group_stylize(tr::now);
	case FxGroup::Classic:
		return tr::lng_oblivion_photo_panel_group_classic(tr::now);
	}
	return QString();
}

QString NewLayerName(const Document &document) {
	for (auto index = int(document.layers.size()) + 1;; ++index) {
		const auto name = tr::lng_oblivion_photo_panel_layer_name(
			tr::now,
			lt_index,
			QString::number(index));
		if (!ranges::contains(document.layers, name, &Layer::name)) {
			return name;
		}
	}
}

QImage SampleSceneImage() {
	return SampleImage();
}

Document SampleSceneDocument() {
	auto document = DocumentFromImage(
		SampleImage(),
		EditState(),
		tr::lng_oblivion_photo_panel_layer_photo(tr::now));
	if (document.empty()) {
		return document;
	}
	const auto canvas = document.size;
	const auto side = std::max(std::min(canvas.width(), canvas.height()), 8);

	auto card = FxTestImage(side * 6 / 10, side * 4 / 10);
	auto second = MakeImageLayer(std::move(card), NewLayerName(document));
	second.transform = ComposeTransform({
		.center = QPointF(canvas.width() * 0.66, canvas.height() * 0.38),
		.scaleX = 1.,
		.scaleY = 1.,
		.rotation = -8.,
	}, QSizeF(second.size()));
	second.blend = BlendMode::Multiply;
	second.opacity = 0.85;
	second.effects.push_back(MakeFx("classic.duotone"));
	second.effects.push_back(MakeFx("classic.light", {
		{ "contrast", FxValue::Integer(25) },
	}));
	AddLayer(document, std::move(second));

	auto third = MakeImageLayer(
		FxTestImage(side * 4 / 10, side * 4 / 10, true),
		NewLayerName(document));
	third.transform = QTransform::fromTranslate(
		canvas.width() * 0.12,
		canvas.height() * 0.5);
	third.blend = BlendMode::Screen;
	const auto maskSize = MaskSizeFor(third.size());
	auto mask = QImage(maskSize, QImage::Format_Grayscale8);
	if (!mask.isNull()) {
		for (auto y = 0; y != maskSize.height(); ++y) {
			const auto line = mask.scanLine(y);
			for (auto x = 0; x != maskSize.width(); ++x) {
				line[x] = uchar(255 * (maskSize.height() - y)
					/ maskSize.height());
			}
		}
		third.mask = MakeMask(std::move(mask));
	}
	AddLayer(document, std::move(third));
	return document;
}

void RegisterEditorScene(EditorSceneArgs &&args) {
	const auto shared = std::make_shared<EditorSceneArgs>(std::move(args));
	RegisterOptionsScene(
		shared->name,
		shared->size.isEmpty() ? QSize(1120, 720) : shared->size,
		[=] {
			auto options = PhotoEditorOptions();
			options.tab = shared->tab;
			options.tool = shared->tool;
			options.document = std::make_shared<const Document>(
				shared->document
					? shared->document()
					: DocumentFromImage(
						SampleImage(),
						EditState(),
						tr::lng_oblivion_photo_panel_layer_photo(tr::now)));
			return options;
		},
		[=](not_null<Editor*> editor) {
			if (shared->prepare) {
				shared->prepare(editor->controller());
			}
			// Choosing a tool with options opens its tab.
			editor->selectTab(shared->tab);
		},
		(shared->wait > 0) ? shared->wait : kSceneWait);
}

void RegisterPanelScene(PanelSceneArgs &&args) {
	const auto shared = std::make_shared<PanelSceneArgs>(std::move(args));
	SelfTest::RegisterScene(
		shared->name,
		shared->size,
		[=](not_null<Ui::RpWidget*> parent) {
			return Ui::CreateChild<PanelSceneHost>(
				parent.get(),
				SelfTest::SceneShow(parent),
				*shared);
		});
}

void ShowPhotoEditor(
		std::shared_ptr<Ui::Show> show,
		QImage image,
		PhotoEditorOptions options) {
	const auto continued = options.document && !options.document->empty();
	if (!show || !show->valid()) {
		return;
	} else if (Core::IsAppLaunched() && Core::App().passcodeLocked()) {
		// The app has locked itself while the picture was loading:
		// nothing is shown above the passcode screen.
		return;
	} else if (image.isNull() && !continued) {
		show->showToast(tr::lng_oblivion_photo_ui_open_failed(tr::now));
		return;
	}
	auto layer = std::make_unique<EditorLayer>(
		show->toastParent(),
		show,
		std::move(image),
		std::move(options));
	show->showLayer(std::move(layer), Ui::LayerOption::KeepOther);
}

void ShowPhotoEditorForFile(
		std::shared_ptr<Ui::Show> show,
		QString path,
		PhotoEditorOptions options) {
	if (!show) {
		return;
	}
	if (options.fileName.isEmpty()) {
		options.fileName = QFileInfo(path).completeBaseName();
	}
	crl::async([=, options = std::move(options)]() mutable {
		auto error = QString();
		auto image = LoadImage(path, &error);
		// Everything holding the show goes back to the main thread.
		crl::on_main([
				show = std::move(show),
				image = std::move(image),
				options = std::move(options)]() mutable {
			if (!show->valid()) {
				return;
			}
			ShowPhotoEditor(show, std::move(image), std::move(options));
		});
	});
}

void ChoosePhotoToEdit(
		std::shared_ptr<Ui::Show> show,
		PhotoEditorOptions options) {
	if (!show || !show->valid()) {
		return;
	}
	const auto shared = std::make_shared<PhotoEditorOptions>(
		std::move(options));
	FileDialog::GetOpenPath(
		show->toastParent().get(),
		tr::lng_oblivion_photo_ui_open_title(tr::now),
		FileDialog::ImagesFilter(),
		[=](FileDialog::OpenResult &&result) {
			if (result.paths.isEmpty() || !show->valid()) {
				return;
			}
			ShowPhotoEditorForFile(show, result.paths.front(), *shared);
		});
}

not_null<Ui::RpWidget*> CreatePhotoEditorWidget(
		not_null<QWidget*> parent,
		std::shared_ptr<Ui::Show> show,
		QImage image,
		PhotoEditorOptions options,
		Fn<void()> closeRequested) {
	const auto result = Ui::CreateChild<Editor>(
		parent.get(),
		std::move(show),
		std::move(image),
		std::move(options));
	result->closeRequests() | rpl::on_next([=] {
		if (closeRequested) {
			closeRequested();
		}
	}, result->lifetime());
	result->show();
	result->setFocus();
	return result;
}

} // namespace Oblivion::Photo
