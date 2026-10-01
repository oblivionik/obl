/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_photo_editor.h"

#include "base/timer.h"
#include "base/unique_qptr.h"
#include "base/weak_ptr.h"
#include "core/file_utilities.h"
#include "core/shortcuts.h"
#include "lang/lang_keys.h"
#include "oblivion/oblivion_photo_editor_canvas.h"
#include "oblivion/oblivion_photo_editor_controls.h"
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

#include <QtCore/QFileInfo>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>

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
	case 0x06: return Qt::Key_Z;
	case 0x10: return Qt::Key_Y;
	case 0x01: return Qt::Key_S;
	case 0x08: return Qt::Key_C;
	case 0x2A: return Qt::Key_Backslash;
	}
#endif // Q_OS_MAC
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
	struct Result {
		QImage image;
		EditState state;
		QSize size;
		uint64 id = 0;
	};

	explicit PreviewRenderer(Fn<void(Result)> done);
	~PreviewRenderer();

	uint64 request(
		QImage source,
		EditState state,
		QSize size,
		bool cancellable = false);
	[[nodiscard]] uint64 lastId() const;

private:
	struct Job {
		QImage source;
		EditState state;
		QSize size;
		uint64 id = 0;
		bool cancellable = false;
	};

	void start(Job job);
	void finished(Result result);

	const Fn<void(Result)> _done;
	std::optional<Job> _pending;
	std::optional<Job> _pendingAfter; // Cancellable, after _pending.
	std::shared_ptr<std::atomic<bool>> _cancel;
	bool _running = false;
	bool _runningCancellable = false;
	uint64 _counter = 0;

};

PreviewRenderer::PreviewRenderer(Fn<void(Result)> done)
: _done(std::move(done)) {
}

PreviewRenderer::~PreviewRenderer() {
	if (_cancel) {
		_cancel->store(true);
	}
}

uint64 PreviewRenderer::request(
		QImage source,
		EditState state,
		QSize size,
		bool cancellable) {
	auto job = Job{
		.source = std::move(source),
		.state = std::move(state),
		.size = size,
		.id = ++_counter,
		.cancellable = cancellable,
	};
	const auto id = job.id;
	if (_running) {
		if (_runningCancellable) {
			// finished() gets a null image soon and starts the pending job.
			_cancel->store(true);
		}
		if (cancellable && _pending && !_pending->cancellable) {
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
	_runningCancellable = job.cancellable;
	_cancel = std::make_shared<std::atomic<bool>>(false);
	crl::async([
			weak = base::make_weak(this),
			cancel = _cancel,
			job = std::move(job)]() mutable {
		auto image = Render(job.source, job.state, job.size, cancel.get());
		crl::on_main(weak, [=, image = std::move(image)]() mutable {
			// The render could pass its last check before it was cancelled.
			weak->finished({
				.image = cancel->load() ? QImage() : std::move(image),
				.state = job.state,
				.size = job.size,
				.id = job.id,
			});
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

class Editor final : public Ui::RpWidget {
public:
	Editor(
		QWidget *parent,
		std::shared_ptr<Ui::Show> show,
		QImage image,
		PhotoEditorOptions options);
	~Editor();

	[[nodiscard]] rpl::producer<> closeRequests() const;
	void requestClose();
	void selectTab(Tab tab);
	void setComparing(bool comparing);
	void showBackgroundProgress();
	[[nodiscard]] bool snapshotReady() const;

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;
	void keyReleaseEvent(QKeyEvent *e) override;
	void focusOutEvent(QFocusEvent *e) override;

private:
	struct Page {
		not_null<Ui::ScrollArea*> scroll;
		not_null<Ui::VerticalLayout*> content;
	};
	struct Sources {
		QImage source;
		QImage preview;
		QImage thumbs;
		bool alpha = false;
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
	[[nodiscard]] Page createPage();
	not_null<SectionTitle*> addTitle(
		not_null<Ui::VerticalLayout*> page,
		rpl::producer<QString> text,
		int top = 0);
	void addHint(
		not_null<Ui::VerticalLayout*> page,
		rpl::producer<QString> text);
	void setupCropPage(not_null<Ui::VerticalLayout*> page);
	void setupAdjustPage(not_null<Ui::VerticalLayout*> page);
	void setupFiltersPage(not_null<Ui::VerticalLayout*> page);
	void setupEffectsPage(not_null<Ui::VerticalLayout*> page);
	void setupAutoPage(not_null<Ui::VerticalLayout*> page);
	void rebuildEffects();
	void addEffectCard(int index);
	void showEffectMenu(int index, QPoint globalPosition);
	void chooseCustomColor(int index, EffectParam param);
	void addEffect(EffectType type);

	void startLoading(QImage image);
	void sourcesReady(Sources &&sources);
	void preparePreview(int side);
	void toggleBackground();
	void cancelBackground();
	void backgroundReady(Sources &&cutout);
	void replaceSources(Sources &&sources);

	void apply(EditState state, bool commitNow);
	void commit();
	void undo();
	void redo();
	void applyHistory();
	void stateChanged();
	void refreshHistoryButtons();
	[[nodiscard]] EditState displayState() const;
	[[nodiscard]] bool dirty() const;

	void refreshPreview(bool force);
	void requestDetail();
	void previewReady(PreviewRenderer::Result &&result);
	void refreshBefore();
	void beforeReady(PreviewRenderer::Result &&result);
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
	[[nodiscard]] bool narrow() const;
	[[nodiscard]] QRect topBarRect() const;

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
	const QSize _sourceSize;
	std::unique_ptr<Ui::LayerManager> _layers;

	QImage _source;
	QImage _preview;
	QImage _thumbSource;
	bool _hasAlpha = false;
	bool _sourceReady = false;
	int _previewSide = 0;
	bool _preparingPreview = false;

	// "Remove background": the cutout replaces the sources above, the
	// untouched ones wait here to bring the background back and to show
	// the original in the comparison. The generation drops the previews
	// that were being prepared from the replaced source.
	std::optional<Sources> _withBackground;
	rpl::variable<bool> _backgroundRemoved = false;
	int _sourceGeneration = 0;

	// The system can't be stopped once it was asked for a cutout. A
	// cancelled request works on and its result is dropped, unless the
	// button was pressed again meanwhile: then that result is awaited
	// instead of asking twice (the source can't change in between).
	bool _removingBackground = false;
	bool _backgroundRequested = false;

	EditState _state;
	EditState _initial;
	std::vector<EditState> _history;
	int _historyIndex = 0;
	base::Timer _commitTimer;
	rpl::event_stream<> _refreshControls;

	Tab _tab = Tab::Adjust;
	int _ratioIndex = kRatioFree;
	bool _layingOut = false;
	bool _comparing = false;
	bool _exporting = false;
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

	Ui::RpWidget *_panel = nullptr;
	TabBar *_tabs = nullptr;
	std::vector<Page> _pages;
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
	EditState _fastState;
	QSize _fastSize;
	uint64 _fastId = 0;
	EditState _detailState;
	QSize _detailSize;
	uint64 _detailId = 0;
	// The last detail render is still in progress or is on the screen and
	// nothing requested after it can replace it.
	bool _detailValid = false;
	EditState _beforeState;
	QSize _beforeSize;
	bool _beforeValid = false;
	uint64 _shownId = 0;
	EditState _shownState;
	QSize _shownSize;
	std::optional<EditState> _thumbsState;
	bool _thumbsDirty = true;
	uint64 _thumbsGeneration = 0;

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
, _sourceSize(image.size())
, _layers(std::make_unique<Ui::LayerManager>(this))
, _state(Normalized(_options.state))
, _initial(_state)
, _history({ _state })
, _commitTimer([=] { commit(); })
, _tab(_options.tab)
, _renderer(std::make_unique<PreviewRenderer>([=](
		PreviewRenderer::Result result) {
	previewReady(std::move(result));
}))
, _beforeRenderer(std::make_unique<PreviewRenderer>([=](
		PreviewRenderer::Result result) {
	beforeReady(std::move(result));
}))
, _detailTimer([=] { requestDetail(); })
, _thumbsTimer([=] { refreshThumbnails(); }) {
	setFocusPolicy(Qt::StrongFocus);
	_autoButtonText = tr::lng_oblivion_photo_ui_auto_button(tr::now);
	inferRatio();

	setupCanvas();
	setupStrip();
	setupPanel();
	setupTopBar();
	setupShortcuts();
	_busy = Ui::CreateChild<BusyOverlay>(this);

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

Editor::~Editor() = default;

rpl::producer<> Editor::closeRequests() const {
	return _closeRequests.events();
}

void Editor::setupCanvas() {
	_canvas = Ui::CreateChild<Canvas>(this);
	_canvas->setLoading(true);
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
		apply(EditState(), true);
	});

	_more = button(
		{ &st::infoTopBarMenuActive },
		tr::lng_oblivion_photo_ui_more(tr::now));
	_more->setClickedCallback([=] { showMoreMenu(); });
	const auto hasMenu = _options.allowSaveToFile
		|| _options.allowCopy
		|| !_options.actions.empty();
	_more->setVisible(hasMenu);

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
	}, _panel->lifetime());

	auto tabs = std::vector<TabInfo>();
	tabs.push_back({
		tr::lng_oblivion_photo_ui_tab_crop(),
		{ &st::photoEditorCropRatioButton.icon },
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
	_tabs = Ui::CreateChild<TabBar>(_panel, std::move(tabs));
	_tabs->setActive(int(_tab), anim::type::instant);
	_tabs->show();
	_tabs->activeChanges() | rpl::on_next([=](int index) {
		if (Tab(index) != _tab) {
			selectTab(Tab(index));
		}
	}, _tabs->lifetime());

	for (auto i = 0; i != 5; ++i) {
		_pages.push_back(createPage());
	}
	setupCropPage(_pages[int(Tab::Crop)].content);
	setupAdjustPage(_pages[int(Tab::Adjust)].content);
	setupFiltersPage(_pages[int(Tab::Filters)].content);
	setupEffectsPage(_pages[int(Tab::Effects)].content);
	setupAutoPage(_pages[int(Tab::Auto)].content);
	for (const auto &page : _pages) {
		page.content->add(object_ptr<Ui::FixedHeightWidget>(
			page.content,
			Px(kPageBottom)));
	}
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
				style::margins(0, 0, padding, 0));
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
	if (image.isNull()) {
		_canvas->setLoading(false);
		return;
	}
	const auto screen = QGuiApplication::primaryScreen();
	const auto screenSide = screen
		? int(std::max(screen->size().width(), screen->size().height())
			* screen->devicePixelRatio()
			* kPreviewScreenPart)
		: kPreviewMinSide;
	const auto side = std::clamp(
		screenSide,
		kPreviewMinSide,
		kPreviewMaxInitialSide);
	crl::async([weak = base::make_weak(this), image = std::move(image), side] {
		auto sources = Sources();
		sources.source = PrepareSource(image);
		sources.preview = PrepareSource(sources.source, QSize(side, side));
		sources.thumbs = PrepareSource(
			sources.preview,
			QSize(kThumbSourceSide, kThumbSourceSide));
		sources.alpha = HasTransparentPixels(sources.thumbs);
		crl::on_main(weak, [=, sources = std::move(sources)]() mutable {
			weak->sourcesReady(std::move(sources));
		});
	});
}

void Editor::sourcesReady(Sources &&sources) {
	_source = std::move(sources.source);
	_preview = std::move(sources.preview);
	_previewSide = std::max(_preview.width(), _preview.height());
	_thumbSource = std::move(sources.thumbs);
	_hasAlpha = sources.alpha;
	_sourceReady = !_source.isNull();
	if (!_sourceReady) {
		_canvas->setLoading(false);
		toast(tr::lng_oblivion_photo_ui_export_failed(tr::now));
		return;
	}
	_canvas->setCheckerboard(_hasAlpha);
	_done->setDisabled(false);
	stateChanged();
	update();
}

void Editor::preparePreview(int side) {
	if (_preparingPreview || _source.isNull()) {
		return;
	}
	_preparingPreview = true;
	crl::async([
			weak = base::make_weak(this),
			source = _source,
			generation = _sourceGeneration,
			side] {
		auto preview = PrepareSource(source, QSize(side, side));
		crl::on_main(weak, [=, preview = std::move(preview)]() mutable {
			weak->_preparingPreview = false;
			if (weak->_sourceGeneration != generation) {
				weak->refreshPreview(true);
			} else if (!preview.isNull()) {
				weak->_preview = std::move(preview);
				weak->_previewSide = std::max(
					weak->_preview.width(),
					weak->_preview.height());
				weak->refreshPreview(true);
			}
		});
	});
}

void Editor::toggleBackground() {
	if (_exporting || !_sourceReady) {
		return;
	} else if (_withBackground) {
		auto sources = base::take(*_withBackground);
		_withBackground = std::nullopt;
		replaceSources(std::move(sources));
		return;
	}
	showBackgroundProgress();
	if (_backgroundRequested) {
		return;
	}
	_backgroundRequested = true;
	crl::async([
			weak = base::make_weak(this),
			source = _source,
			side = std::max(_previewSide, 1)] {
		auto removed = Vision::RemoveBackground(source);
		auto sources = Sources();
		if (removed.ok) {
			sources.source = std::move(removed.cutout);
			sources.preview = PrepareSource(
				sources.source,
				QSize(side, side));
			sources.thumbs = PrepareSource(
				sources.preview,
				QSize(kThumbSourceSide, kThumbSourceSide));
			sources.alpha = true;
		} else {
			LOG(("Oblivion Vision Error: no background removal, %1."
				).arg(removed.error));
		}
		crl::on_main(weak, [=, sources = std::move(sources)]() mutable {
			weak->backgroundReady(std::move(sources));
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
	if (!_removingBackground) {
		return;
	}
	_removingBackground = false;
	setExporting(false);
}

void Editor::backgroundReady(Sources &&cutout) {
	_backgroundRequested = false;
	if (!_removingBackground || _finished) {
		return;
	}
	_removingBackground = false;
	setExporting(false);
	if (cutout.source.isNull() || cutout.preview.isNull()) {
		toast(tr::lng_oblivion_vision_cutout_not_found(tr::now));
		return;
	}
	_withBackground = Sources{
		.source = _source,
		.preview = _preview,
		.thumbs = _thumbSource,
		.alpha = _hasAlpha,
	};
	replaceSources(std::move(cutout));
}

void Editor::replaceSources(Sources &&sources) {
	++_sourceGeneration;
	_source = std::move(sources.source);
	_preview = std::move(sources.preview);
	_previewSide = std::max(_preview.width(), _preview.height());
	_thumbSource = std::move(sources.thumbs);
	_hasAlpha = sources.alpha;
	_backgroundRemoved = _withBackground.has_value();
	_canvas->setCheckerboard(_hasAlpha);

	// The state is the same, only the pixels differ: nothing rendered or
	// requested from the previous source may be kept as "up to date" and
	// a render of it that is still on its way must not get to the canvas.
	_shownId = _renderer->lastId() + 1;
	_shownSize = QSize();
	_fastSize = QSize();
	_detailState = EditState();
	_detailSize = QSize();
	_detailValid = false;
	_beforeValid = false;
	_beforeState = EditState();
	_beforeSize = QSize();
	_canvas->setBeforeImage(QImage());
	_thumbsState = std::nullopt;
	++_thumbsGeneration;
	refreshPreview(true);
	if (_comparing) {
		refreshBefore();
	}
	scheduleThumbnails();
}

void Editor::apply(EditState state, bool commitNow) {
	state = Normalized(std::move(state));
	const auto changed = !(state == _state);
	if (changed) {
		_state = std::move(state);
		stateChanged();
	}
	if (commitNow) {
		commit();
	} else if (changed) {
		_commitTimer.callOnce(kCommitDelay);
	}
}

void Editor::commit() {
	_commitTimer.cancel();
	if (_history[_historyIndex] == _state) {
		return;
	}
	_history.resize(_historyIndex + 1);
	_history.push_back(_state);
	if (int(_history.size()) > kHistoryLimit) {
		_history.erase(begin(_history));
	}
	_historyIndex = int(_history.size()) - 1;
	refreshHistoryButtons();
}

void Editor::undo() {
	commit();
	if (_historyIndex > 0) {
		--_historyIndex;
		applyHistory();
	}
}

void Editor::redo() {
	commit();
	if (_historyIndex + 1 < int(_history.size())) {
		++_historyIndex;
		applyHistory();
	}
}

void Editor::applyHistory() {
	_state = _history[_historyIndex];
	_canvas->setGridVisible(false);
	syncRatio();
	stateChanged();
	refreshHistoryButtons();
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
		_undo->setAvailable(_historyIndex > 0);
		_redo->setAvailable(_historyIndex + 1 < int(_history.size()));
	}
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
	if (_reset) {
		_reset->setAvailable(!IsIdentity(_state));
	}
	if (_undo) {
		refreshHistoryButtons();
	}
	refreshPreview(false);
	if (_comparing) {
		refreshBefore();
	}
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

bool Editor::dirty() const {
	return _withBackground.has_value()
		|| !(Normalized(_state) == _initial);
}

void Editor::refreshPreview(bool force) {
	if (!_sourceReady) {
		return;
	}
	const auto display = displayState();
	const auto frame = OutputSize(_source.size(), display);
	if (frame != _canvas->frameSize()) {
		_beforeValid = false;
	}
	_canvas->setFrameSize(frame);
	const auto fit = _canvas->fitPixels();
	if (fit.isEmpty()) {
		return;
	}
	const auto fitSide = std::max(fit.width(), fit.height());
	const auto sourceSide = std::max(_source.width(), _source.height());
	const auto target = std::min(
		std::max(kPreviewMinSide, int(fitSide * kPreviewGrow)),
		sourceSide);
	if (_previewSide * kDetailThreshold < target
		|| _previewSide > target * kPreviewShrink) {
		preparePreview(target);
	}
	if (force || !(display == _fastState) || fit != _fastSize) {
		_fastState = display;
		_fastSize = fit;
		_fastId = _renderer->request(_preview, display, fit);

		// It drops a detail render still in progress and its result may
		// replace the detail one on the screen (an undo and a redo in a
		// row), so the next detail render can't be skipped as a repeat.
		_detailValid = false;
	}
	const auto needed = _canvas->renderPixels();
	const auto fast = OutputSize(_preview.size(), display, fit);
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
	const auto display = displayState();
	const auto needed = _canvas->renderPixels();
	if (_detailValid && display == _detailState && needed == _detailSize) {
		return;
	}
	_detailState = display;
	_detailSize = needed;
	_detailId = _renderer->request(_source, display, needed, true);
	_detailValid = true;
}

void Editor::previewReady(PreviewRenderer::Result &&result) {
	if (result.id < _shownId
		|| OutputSize(_source.size(), result.state)
			!= _canvas->frameSize()) {
		return;
	}
	if (result.state == _shownState
		&& _canvas->hasImage()
		&& result.image.width() < _shownSize.width()
		&& result.size == _fastSize
		&& _detailState == _shownState) {
		if (result.id == _fastId && _shownId == _detailId) {
			// Everything requested is done and the detail render stays.
			_detailValid = true;
		}
		return;
	}
	_shownId = result.id;
	_shownState = result.state;
	_shownSize = result.image.size();
	_canvas->setImage(std::move(result.image));
	_canvas->setLoading(false);
}

void Editor::refreshBefore() {
	if (!_sourceReady) {
		return;
	}
	const auto display = GeometryOnly(displayState());
	const auto needed = _canvas->renderPixels();
	if (_beforeValid && display == _beforeState && needed == _beforeSize) {
		return;
	} else if (!(display == _beforeState)) {
		// Another geometry: the old original would be shown turned or
		// flipped the wrong way until the new one is ready.
		_beforeValid = false;
		_canvas->setBeforeImage(QImage());
	}
	_beforeState = display;
	_beforeSize = needed;
	const auto &full = _withBackground ? _withBackground->source : _source;
	const auto &small = _withBackground
		? _withBackground->preview
		: _preview;
	const auto fast = OutputSize(small.size(), display, needed);
	const auto &source = (needed.width() > fast.width() * kDetailThreshold)
		? full
		: small;
	_beforeRenderer->request(source, display, needed);
}

void Editor::beforeReady(PreviewRenderer::Result &&result) {
	if (!(result.state == _beforeState) || result.size != _beforeSize) {
		return;
	}
	_beforeValid = true;
	_canvas->setBeforeImage(std::move(result.image));
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
	crl::async([
			weak = base::make_weak(this),
			source = _preview,
			geometry = GeometryOnly(_state)] {
		const auto framed = Render(
			source,
			geometry,
			QSize(kAutoSide, kAutoSide));
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
	auto canvas = QRect();
	if (isNarrow) {
		// Under the photo the panel takes only what the current page
		// needs (up to a part of the window), the rest goes to the photo.
		const auto maxHeight = std::max(
			height() * kNarrowPanelPercent / 100,
			1);
		const auto &page = _pages[int(_tab)];
		const auto natural = pagesTop
			+ page.content->height()
			+ radius / 2;
		const auto panelHeight = std::clamp(
			natural,
			std::min(Px(kNarrowPanelMin), maxHeight),
			maxHeight);
		panel = QRect(
			margin,
			height() - margin - panelHeight,
			panelWidth,
			panelHeight);
		canvas = QRect(
			0,
			top,
			width(),
			std::max(panel.y() - top - stripHeight - Px(kCanvasPanelGap), 1));
	} else {
		panel = QRect(
			width() - margin - panelWidth,
			top,
			panelWidth,
			height() - top - margin);
		canvas = QRect(
			0,
			top,
			std::max(panel.x() - Px(kCanvasPanelGap), 1),
			std::max(height() - top - stripHeight, 1));
	}
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
	for (const auto &page : _pages) {
		page.scroll->setGeometry(
			0,
			pagesTop,
			panel.width(),
			std::max(panel.height() - pagesTop - radius / 2, 1));
	}
	if (_busy) {
		_busy->setGeometry(rect());
	}
	_layingOut = false;
}

void Editor::selectTab(Tab tab) {
	const auto wasCrop = (_tab == Tab::Crop);
	_tab = tab;
	if (wasCrop && tab != Tab::Crop) {
		commit();
	}
	_tabs->setActive(int(tab));
	for (auto i = 0; i != int(_pages.size()); ++i) {
		_pages[i].scroll->setVisible(i == int(tab));
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
	const auto subtitle = _sourceReady
		? SizeText(OutputSize(_source.size(), _state))
		: SizeText(OutputSize(_sourceSize, _state));
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
	} else if (key == Qt::Key_Escape) {
		// A held key that has just cancelled something must not go on
		// and close the editor.
		if (!e->isAutoRepeat()) {
			requestClose();
		}
	} else if (command && (key == Qt::Key_Return || key == Qt::Key_Enter)) {
		done();
	} else if (command && !shift && key == Qt::Key_S) {
		if (_options.allowSaveToFile || !_options.done) {
			saveToFile(false);
		}
	} else if (command && shift && key == Qt::Key_C) {
		if (_options.allowCopy) {
			copyToClipboard();
		}
	} else if (e->matches(QKeySequence::ZoomIn)
		|| (command && (key == Qt::Key_Equal || key == Qt::Key_Plus))) {
		_canvas->zoomIn();
	} else if (e->matches(QKeySequence::ZoomOut)
		|| (command && (key == Qt::Key_Minus || key == Qt::Key_Underscore))) {
		_canvas->zoomOut();
	} else if (command && key == Qt::Key_0) {
		_canvas->zoomFit();
	} else if (!modifiers && key >= Qt::Key_1 && key <= Qt::Key_5) {
		selectTab(Tab(key - Qt::Key_1));
	} else if (!modifiers
		&& _tab == Tab::Filters
		&& (key == Qt::Key_Left || key == Qt::Key_Right)) {
		stepFilter((key == Qt::Key_Left) ? -1 : 1);
	} else {
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
	if (_exporting || !_sourceReady) {
		return;
	}
	commit();
	setExporting(true, tr::lng_oblivion_photo_ui_processing(tr::now));
	crl::async([
			weak = base::make_weak(this),
			source = _source,
			replaced = _withBackground.has_value(),
			state = _state,
			maxSize = _options.maxOutputSize,
			callback = std::move(callback)]() mutable {
		auto image = Render(source, state, maxSize);
		// The callback is moved to the main thread lambda, so whatever it
		// holds (shows, windows...) is never released on this worker.
		crl::on_main(weak, [
				weak,
				state,
				image = std::move(image),
				source = replaced ? source : QImage(),
				callback = std::move(callback)]() mutable {
			weak->setExporting(false);
			if (image.isNull()) {
				weak->toast(tr::lng_oblivion_photo_ui_export_failed(tr::now));
				return;
			}
			callback({
				.image = std::move(image),
				.state = state,
				.source = std::move(source),
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
	if (_exporting || !_sourceReady) {
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
					source = strong->_source,
					state = strong->_state,
					maxSize = strong->_options.maxOutputSize,
					path,
					format,
					closeAfter] {
				const auto image = Render(source, state, maxSize);
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
	crl::on_main(this, [=] {
		_closeRequests.fire({});
		if (after) {
			after();
		}
	});
}

bool Editor::snapshotReady() const {
	return _sourceReady
		&& _canvas->hasImage()
		&& (_tab != Tab::Filters || _strip->thumbnailsReady())
		&& (!_comparing || _beforeValid)
		&& _busy->settled();
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

void RegisterEditorScene(
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
			if (prepare) {
				prepare(static_cast<Editor*>(widget.get()));
			}
		},
		.ready = [](not_null<QWidget*> widget) {
			return static_cast<Editor*>(widget.get())->snapshotReady();
		},
		.wait = wait,
	});
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	const auto wide = QSize(1120, 720);
	RegisterEditorScene(u"photo_editor_adjust"_q, wide, [] {
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
	RegisterEditorScene(u"photo_editor_filters"_q, wide, [] {
		auto options = PhotoEditorOptions();
		options.tab = Tab::Filters;
		options.state.filter = u"film"_q;
		options.state.filterIntensity = 80;
		return options;
	});
	RegisterEditorScene(u"photo_editor_crop"_q, wide, [] {
		auto options = PhotoEditorOptions();
		options.tab = Tab::Crop;
		options.state.crop = QRectF(0.1, 0.05, 0.72, 0.9);
		options.state.straighten = 2.5;
		return options;
	});
	RegisterEditorScene(u"photo_editor_effects"_q, wide, [] {
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
	RegisterEditorScene(u"photo_editor_background_busy"_q, wide, [] {
		auto options = PhotoEditorOptions();
		options.tab = Tab::Effects;
		return options;
	}, [](not_null<Editor*> editor) {
		editor->showBackgroundProgress();
	}, kBusySlowHintDelay + kSceneWait);
	RegisterEditorScene(u"photo_editor_compare"_q, wide, [] {
		auto options = PhotoEditorOptions();
		options.tab = Tab::Adjust;
		options.state.filter = u"noir"_q;
		return options;
	}, [](not_null<Editor*> editor) {
		editor->setComparing(true);
	});
	RegisterEditorScene(u"photo_editor_narrow"_q, QSize(520, 820), [] {
		auto options = PhotoEditorOptions();
		options.tab = Tab::Filters;
		options.state.filter = u"vivid"_q;
		return options;
	});
});

} // namespace

void ShowPhotoEditor(
		std::shared_ptr<Ui::Show> show,
		QImage image,
		PhotoEditorOptions options) {
	if (!show || !show->valid()) {
		return;
	} else if (image.isNull()) {
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
