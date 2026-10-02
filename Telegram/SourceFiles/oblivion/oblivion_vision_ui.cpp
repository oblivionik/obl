/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_vision_ui.h"

#include "base/call_delayed.h"
#include "base/weak_ptr.h"
#include "core/file_utilities.h"
#include "data/data_document.h"
#include "data/data_forum_topic.h"
#include "data/data_photo.h"
#include "data/data_saved_sublist.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "oblivion/oblivion_photo_core.h"
#include "oblivion/oblivion_photo_integration.h"
#include "oblivion/oblivion_sticker_packs.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "oblivion/oblivion_vision.h"
#include "platform/platform_file_utilities.h"
#include "settings.h"
#include "settings/settings_common.h"
#include "ui/effects/radial_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_media_view.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <crl/crl_async.h>

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QPainterPath>
#include <QtCore/QDir>

namespace Oblivion {
namespace {

constexpr auto kPreviewMaxHeight = 260;
constexpr auto kCheckerCell = 8;
constexpr auto kProgressSize = 32;
constexpr auto kProgressLine = 3;
constexpr auto kProgressPadding = 20;
constexpr auto kProgressTextSkip = 14;
constexpr auto kEmptyPadding = 18;
constexpr auto kCenteredMinWidth = 120;
constexpr auto kSceneWidth = 480;
constexpr auto kSlowHintDelay = crl::time(4000);

using TextDone = Fn<void(Vision::TextResult)>;
using MaskDone = Fn<void(Vision::MaskResult)>;

struct TextBoxArgs {
	std::shared_ptr<Ui::Show> show;

	// Starts the recognition, done is called once, on the main thread.
	Fn<void(TextDone done)> recognize;
};

struct CutoutBoxArgs {
	std::shared_ptr<Ui::Show> show;

	// Starts the background removal, done is called once, on the main
	// thread.
	Fn<void(MaskDone done)> remove;

	QString name; // File name suggestion without an extension.
	Fn<void(QImage)> sticker; // "Make a sticker", optional.
	Fn<void(QImage)> edit; // "Open in photo editor", optional.
};

// A label with minWidth 0 never wraps (its height is always one line and
// the rest is clipped), so both centered styles get a small minimum: it is
// also the lower bound for the balanced lines, see CenteredLabel().
[[nodiscard]] const style::FlatLabel &CenteredLabelStyle() {
	static const auto result = [] {
		auto copy = st::boxLabel;
		copy.align = style::al_top;
		copy.minWidth = style::ConvertScale(kCenteredMinWidth);
		return copy;
	}();
	return result;
}

[[nodiscard]] const style::FlatLabel &CenteredSubTextStyle() {
	static const auto result = [] {
		auto copy = st::defaultSubTextLabel;
		copy.align = style::al_top;
		copy.minWidth = style::ConvertScale(kCenteredMinWidth);
		return copy;
	}();
	return result;
}

// A centered text that wraps into lines of a similar width instead of
// leaving a single word on the last one.
[[nodiscard]] object_ptr<Ui::FlatLabel> CenteredLabel(
		QWidget *parent,
		rpl::producer<QString> text,
		const style::FlatLabel &st) {
	auto result = object_ptr<Ui::FlatLabel>(parent, std::move(text), st);
	result->setTryMakeSimilarLines(true);
	return result;
}

// A spinner with a status line under it, for the time the system works.
class ProgressRow final : public Ui::RpWidget {
public:
	ProgressRow(QWidget *parent, rpl::producer<QString> text);

	void setActive(bool active);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	QString _text;
	Ui::InfiniteRadialAnimation _radial;

};

ProgressRow::ProgressRow(QWidget *parent, rpl::producer<QString> text)
: RpWidget(parent)
, _radial([=] { update(); }, st::defaultInfiniteRadialAnimation) {
	std::move(text) | rpl::on_next([=](QString value) {
		_text = std::move(value);
		update();
	}, lifetime());
}

void ProgressRow::setActive(bool active) {
	if (active) {
		_radial.start();
	} else {
		_radial.stop(anim::type::instant);
	}
}

int ProgressRow::resizeGetHeight(int newWidth) {
	return style::ConvertScale(kProgressPadding)
		+ style::ConvertScale(kProgressSize)
		+ style::ConvertScale(kProgressTextSkip)
		+ st::normalFont->height
		+ style::ConvertScale(kProgressPadding) / 2;
}

void ProgressRow::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto size = style::ConvertScale(kProgressSize);
	const auto line = style::ConvertScale(kProgressLine);
	const auto top = style::ConvertScale(kProgressPadding);
	auto pen = QPen(st::windowActiveTextFg->c);
	pen.setWidth(line);
	pen.setCapStyle(Qt::RoundCap);
	{
		auto hq = PainterHighQualityEnabler(p);
		Ui::InfiniteRadialAnimation::Draw(
			p,
			_radial.computeState(),
			QPoint((width() - size) / 2, top),
			QSize(size, size),
			width(),
			pen,
			line);
	}
	p.setPen(st::windowSubTextFg);
	p.setFont(st::normalFont);
	p.drawText(
		QRect(
			0,
			top + size + style::ConvertScale(kProgressTextSkip),
			width(),
			st::normalFont->height),
		Qt::AlignHCenter | Qt::AlignTop,
		st::normalFont->elided(_text, width()));
}

// The progress with a hint that shows up if the work takes long: the very
// first Vision request after the system started loads the models.
[[nodiscard]] not_null<Ui::SlideWrap<Ui::VerticalLayout>*> AddProgress(
		not_null<Ui::GenericBox*> box,
		rpl::producer<QString> text) {
	const auto container = box->verticalLayout();
	const auto wrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	const auto inner = wrap->entity();
	const auto row = inner->add(
		object_ptr<ProgressRow>(inner, std::move(text)));
	wrap->toggledValue() | rpl::on_next([=](bool shown) {
		row->setActive(shown);
	}, row->lifetime());
	const auto hint = inner->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			inner,
			CenteredLabel(
				inner,
				tr::lng_oblivion_vision_slow(),
				CenteredSubTextStyle()),
			st::boxRowPadding + QMargins(0, 0, 0, st::boxLittleSkip)));
	hint->hide(anim::type::instant);
	base::call_delayed(kSlowHintDelay, hint, [=] {
		hint->show(anim::type::normal);
	});
	return wrap;
}

// A centered message in place of the content: nothing found or an error.
[[nodiscard]] not_null<Ui::SlideWrap<Ui::VerticalLayout>*> AddMessage(
		not_null<Ui::GenericBox*> box,
		rpl::producer<QString> text,
		std::optional<rpl::producer<QString>> about = std::nullopt) {
	const auto container = box->verticalLayout();
	const auto wrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	const auto inner = wrap->entity();
	const auto padding = style::ConvertScale(kEmptyPadding);
	inner->add(
		CenteredLabel(inner, std::move(text), CenteredLabelStyle()),
		st::boxRowPadding + QMargins(0, padding, 0, about ? 0 : padding),
		style::al_top);
	if (about) {
		inner->add(
			CenteredLabel(
				inner,
				std::move(*about),
				CenteredSubTextStyle()),
			st::boxRowPadding + QMargins(
				0,
				st::boxLittleSkip,
				0,
				padding),
			style::al_top);
	}
	wrap->hide(anim::type::instant);
	return wrap;
}

void TextBox(not_null<Ui::GenericBox*> box, TextBoxArgs &&args) {
	struct State {
		QString text;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto show = args.show;

	box->setTitle(tr::lng_oblivion_vision_text_title());
	box->setWidth(st::boxWideWidth);

	const auto container = box->verticalLayout();
	const auto progress = AddProgress(
		box,
		tr::lng_oblivion_vision_text_progress());

	const auto result = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	result->hide(anim::type::instant);
	const auto label = result->entity()->add(
		object_ptr<Ui::FlatLabel>(result->entity(), st::boxLabel),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
	label->setSelectable(true);
	label->setContextCopyText(tr::lng_context_copy_text(tr::now));
	const auto count = result->entity()->add(
		object_ptr<Ui::FlatLabel>(
			result->entity(),
			st::defaultSubTextLabel),
		st::boxRowPadding + QMargins(
			0,
			st::boxLittleSkip,
			0,
			st::boxLittleSkip));

	const auto empty = AddMessage(
		box,
		tr::lng_oblivion_vision_text_empty(),
		tr::lng_oblivion_vision_text_empty_about());
	const auto failed = AddMessage(
		box,
		tr::lng_oblivion_vision_text_failed());

	const auto close = [=] { box->closeBox(); };
	const auto copy = [=] {
		if (state->text.isEmpty()) {
			return;
		}
		QGuiApplication::clipboard()->setText(state->text);
		show->showToast(tr::lng_oblivion_vision_text_copied(tr::now));
		box->closeBox();
	};
	const auto apply = [=](Vision::TextResult data) {
		progress->hide(anim::type::instant);
		box->clearButtons();
		if (data.ok && !data.text.isEmpty()) {
			state->text = data.text;
			label->setText(data.text);
			count->setText(tr::lng_oblivion_vision_text_lines(
				tr::now,
				lt_count,
				std::max(data.lines, 1)));
			result->show(anim::type::instant);
			box->addButton(tr::lng_oblivion_vision_text_copy(), copy);
		} else {
			(data.ok ? empty : failed)->show(anim::type::instant);
		}
		box->addButton(tr::lng_close(), close);
	};

	box->addButton(tr::lng_cancel(), close);

	// Taken out of the arguments: they live as long as the box does and
	// the starter holds the full size image.
	if (const auto recognize = base::take(args.recognize)) {
		recognize(crl::guard(box, apply));
	}
}

void PaintCheckerboard(QPainter &p, QRect rect) {
	const auto cell = style::ConvertScale(kCheckerCell);
	p.fillRect(rect, st::windowBg);
	for (auto y = rect.y(); y < rect.y() + rect.height(); y += cell) {
		const auto row = (y - rect.y()) / cell;
		for (auto x = rect.x(); x < rect.x() + rect.width(); x += cell) {
			const auto column = (x - rect.x()) / cell;
			if ((row + column) % 2) {
				p.fillRect(
					QRect(x, y, cell, cell).intersected(rect),
					st::windowBgRipple);
			}
		}
	}
}

[[nodiscard]] QString SuggestedPngPath(const QString &name) {
	if (cDialogLastPath().isEmpty()) {
		Platform::FileDialog::InitLastPath();
	}
	return filedialogNextFilename(
		(name.isEmpty() ? u"cutout"_q : name) + u".png"_q,
		QString());
}

void CutoutBox(not_null<Ui::GenericBox*> box, CutoutBoxArgs &&args) {
	struct State {
		QImage cutout;
		QRect bounds;
		bool crop = true;
		QImage preview;
		rpl::variable<QSize> size;
		uint64 generation = 0;
	};
	const auto state = box->lifetime().make_state<State>();
	// The guards for the background jobs are created here, on the main
	// thread, a worker must not touch the box or the show itself.
	const auto weakBox = base::make_weak(box);
	const auto show = args.show;
	const auto weakShow = std::weak_ptr<Ui::Show>(show);
	const auto name = args.name;

	box->setTitle(tr::lng_oblivion_vision_cutout_title());
	box->setWidth(st::boxWideWidth);

	const auto container = box->verticalLayout();
	const auto ratio = style::DevicePixelRatio();
	const auto maxHeight = style::ConvertScale(kPreviewMaxHeight);
	const auto innerWidth = st::boxWideWidth
		- st::boxRowPadding.left()
		- st::boxRowPadding.right();
	const auto fitted = [=](QSize size, int width) {
		if (size.isEmpty() || width <= 0) {
			return QRect();
		}
		auto result = size;
		if (result.width() > width || result.height() > maxHeight) {
			result = result.scaled(
				QSize(width, maxHeight),
				Qt::KeepAspectRatio);
		}
		result = QSize(
			std::max(result.width(), 1),
			std::max(result.height(), 1));
		return QRect(QPoint((width - result.width()) / 2, 0), result);
	};
	const auto chosenRect = [=] {
		return (state->crop && !state->bounds.isEmpty())
			? state->bounds
			: state->cutout.rect();
	};
	const auto chosen = [=] {
		const auto rect = chosenRect();
		return (rect == state->cutout.rect())
			? state->cutout
			: state->cutout.copy(rect);
	};

	const auto progress = AddProgress(
		box,
		tr::lng_oblivion_vision_cutout_progress());

	const auto result = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	result->hide(anim::type::instant);
	const auto content = result->entity();

	const auto preview = content->add(
		object_ptr<Ui::RpWidget>(content),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
	rpl::combine(
		preview->widthValue(),
		state->size.value()
	) | rpl::on_next([=](int width, QSize size) {
		const auto height = std::max(fitted(size, width).height(), 1);
		if (preview->height() != height) {
			preview->resize(width, height);
		}
		preview->update();
	}, preview->lifetime());
	preview->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(preview);
		const auto rect = fitted(state->size.current(), preview->width());
		if (rect.isEmpty()) {
			return;
		}
		auto hq = PainterHighQualityEnabler(p);
		auto path = QPainterPath();
		const auto radius = float64(st::roundRadiusLarge);
		path.addRoundedRect(QRectF(rect), radius, radius);
		p.setClipPath(path);
		PaintCheckerboard(p, rect);
		// Half of the cells have the color of the box, the thin outline
		// under the image shows where the transparent part ends.
		const auto half = st::lineWidth / 2.;
		p.setPen(QPen(st::windowBgRipple->c, st::lineWidth));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(
			QRectF(rect).marginsRemoved(QMarginsF(half, half, half, half)),
			radius - half,
			radius - half);
		if (!state->preview.isNull()) {
			p.drawImage(rect, state->preview);
		}
	}, preview->lifetime());

	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			state->size.value() | rpl::map([](QSize size) {
				return tr::lng_oblivion_photo_io_dimensions(
					tr::now,
					lt_width,
					QString::number(std::max(size.width(), 0)),
					lt_height,
					QString::number(std::max(size.height(), 0)));
			}),
			CenteredSubTextStyle()),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0),
		style::al_top);

	const auto refreshPreview = [=] {
		const auto rect = chosenRect();
		const auto image = state->cutout;
		const auto target = fitted(rect.size(), innerWidth).size() * ratio;
		const auto generation = ++state->generation;
		state->size = rect.size();
		crl::async([=] {
			const auto scaled = Photo::PrepareSource(
				(rect == image.rect()) ? image : image.copy(rect),
				target);
			crl::on_main(weakBox, [=] {
				if (generation == state->generation) {
					state->preview = scaled;
					preview->update();
				}
			});
		});
	};

	const auto toast = [=](const QString &text) {
		const auto strong = weakShow.lock();
		if (strong && strong->valid()) {
			strong->showToast(text);
		}
	};
	const auto copy = [=] {
		QGuiApplication::clipboard()->setImage(chosen());
		toast(tr::lng_oblivion_photo_io_copied(tr::now));
	};
	const auto saveTo = [=](QString path) {
		if (!path.endsWith(u".png"_q, Qt::CaseInsensitive)) {
			path += u".png"_q;
		}
		const auto image = state->cutout;
		const auto rect = chosenRect();
		// Not guarded by the box: the file is written even if the box is
		// closed right after the path was chosen.
		crl::async([=] {
			const auto written = Photo::SaveImage(
				(rect == image.rect()) ? image : image.copy(rect),
				path,
				Photo::SaveFormat::Png);
			crl::on_main([=] {
				toast(written
					? tr::lng_oblivion_saved_to(
						tr::now,
						lt_path,
						QDir::toNativeSeparators(path))
					: tr::lng_oblivion_photo_io_save_failed(tr::now));
			});
		});
	};
	const auto save = [=] {
		FileDialog::GetWritePath(
			box.get(),
			tr::lng_oblivion_photo_io_save_title(tr::now),
			tr::lng_oblivion_photo_io_filter_png(tr::now) + u" (*.png)"_q,
			SuggestedPngPath(name),
			crl::guard(box, [=](QString &&path) {
				if (!path.isEmpty()) {
					saveTo(std::move(path));
				}
			}));
	};

	Ui::AddSkip(content);
	Ui::AddDivider(content);
	Ui::AddSkip(content);
	const auto crop = ::Settings::AddButtonWithIcon(
		content,
		tr::lng_oblivion_vision_cutout_crop(),
		st::settingsButtonNoIcon);
	crop->toggleOn(rpl::single(state->crop))->toggledChanges(
	) | rpl::on_next([=](bool toggled) {
		if (state->crop != toggled) {
			state->crop = toggled;
			refreshPreview();
		}
	}, crop->lifetime());
	Ui::AddSkip(content);
	Ui::AddDivider(content);
	Ui::AddSkip(content);
	::Settings::AddButtonWithIcon(
		content,
		tr::lng_oblivion_vision_cutout_copy(),
		st::settingsButton,
		{ &st::menuIconCopy }
	)->setClickedCallback(copy);
	::Settings::AddButtonWithIcon(
		content,
		tr::lng_oblivion_vision_cutout_save(),
		st::settingsButton,
		{ &st::menuIconDownload }
	)->setClickedCallback(save);
	if (const auto sticker = args.sticker) {
		::Settings::AddButtonWithIcon(
			content,
			tr::lng_oblivion_vision_cutout_sticker(),
			st::settingsButton,
			{ &st::menuIconStickers }
		)->setClickedCallback([=] {
			sticker(chosen());
		});
	}
	if (const auto edit = args.edit) {
		::Settings::AddButtonWithIcon(
			content,
			tr::lng_oblivion_vision_cutout_edit(),
			st::settingsButton,
			{ &st::menuIconPalette }
		)->setClickedCallback([=] {
			// The box is closed first, everything used after that is
			// copied out of the box state.
			const auto image = chosen();
			const auto callback = edit;
			box->closeBox();
			callback(image);
		});
	}
	Ui::AddSkip(content);

	const auto failed = AddMessage(
		box,
		tr::lng_oblivion_vision_cutout_not_found());

	const auto apply = [=](Vision::MaskResult data) {
		progress->hide(anim::type::instant);
		box->clearButtons();
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		if (!data.ok || data.cutout.isNull()) {
			failed->show(anim::type::instant);
			return;
		}
		state->cutout = std::move(data.cutout);
		state->bounds = data.bounds.intersected(state->cutout.rect());
		refreshPreview();
		result->show(anim::type::instant);
	};

	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });

	// Taken out of the arguments: they live as long as the box does and
	// the starter holds the original next to the cutout.
	if (const auto remove = base::take(args.remove)) {
		remove(crl::guard(box, apply));
	}
}

// The results are moved to the main thread together with the callback,
// so whatever the callback holds is never released on the worker.
[[nodiscard]] Fn<void(TextDone)> RecognizeAsync(QImage image) {
	return [=](TextDone done) {
		crl::async([=, done = std::move(done)]() mutable {
			auto result = Vision::RecognizeText(image);
			if (!result.ok) {
				LOG(("Oblivion Vision Error: no text recognition, %1."
					).arg(result.error));
			}
			crl::on_main([
					done = std::move(done),
					result = std::move(result)]() mutable {
				done(std::move(result));
			});
		});
	};
}

[[nodiscard]] Fn<void(MaskDone)> RemoveAsync(QImage image) {
	return [=](MaskDone done) {
		crl::async([=, done = std::move(done)]() mutable {
			auto result = Vision::RemoveBackground(image);
			if (!result.ok) {
				LOG(("Oblivion Vision Error: no background removal, %1."
					).arg(result.error));
			}
			crl::on_main([
					done = std::move(done),
					result = std::move(result)]() mutable {
				done(std::move(result));
			});
		});
	};
}

[[nodiscard]] Data::Thread *ContextThread(
		not_null<Window::SessionController*> controller,
		FullMsgId context) {
	const auto item = context
		? controller->session().data().message(context)
		: nullptr;
	if (!item) {
		return nullptr;
	} else if (const auto topic = item->topic()) {
		return topic;
	} else if (const auto sublist = item->savedSublist()) {
		return sublist;
	}
	return item->history();
}

enum class Action {
	Text,
	Cutout,
};

void Run(
		not_null<Window::SessionController*> controller,
		Action action,
		PhotoData *photo,
		DocumentData *document,
		FullMsgId context) {
	const auto weak = base::make_weak(controller);
	const auto weakThread = base::make_weak(
		ContextThread(controller, context));
	const auto done = [=](QImage image, QString name) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		} else if (action == Action::Text) {
			ShowRecognizedText(strong, std::move(image));
		} else {
			ShowCutout(strong, std::move(image), name, weakThread.get());
		}
	};
	const auto title = (action == Action::Text)
		? tr::lng_oblivion_vision_text_title(tr::now)
		: tr::lng_oblivion_vision_cutout_title(tr::now);
	if (photo) {
		LoadPhotoImage(controller, photo, context, done, title);
	} else if (document) {
		LoadDocumentImage(controller, document, context, done, title);
	}
}

// Snapshot scenes (OBLIVION_SELFTEST=ui), see oblivion_ui_snapshots.h.

[[nodiscard]] Vision::TextResult SampleText() {
	auto result = Vision::TextResult();
	// Short lines, a line that has to wrap and a long one without spaces.
	// Every part has the u prefix: a part with non-ASCII text and without
	// one depends on how the compiler joins it to a UTF-16 literal (MSVC).
	result.text = u"Привет, Oblivion 2026!\n"
		u"Встречаемся в субботу в 18:30 у главного входа в парк, "
		u"возьмите с собой документы и зонт.\n"
		u"Адрес: ул. Садовая, 12, вход со двора\n"
		u"The quick brown fox jumps over the lazy dog\n"
		u"https://example.com/events/2026/spring-meeting?ref=poster\n"
		u"Wi-Fi: Oblivion_Guest, пароль: lottie-512"_q;
	result.lines = 6;
	result.ok = true;
	return result;
}

[[nodiscard]] Vision::MaskResult SampleCutout() {
	const auto size = QSize(1200, 800);
	auto image = QImage(size, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	auto p = QPainter(&image);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	auto body = QLinearGradient(0, 200, 0, 780);
	body.setColorAt(0., QColor(0x6c, 0x5c, 0xe7));
	body.setColorAt(1., QColor(0x3d, 0x2f, 0xb0));
	p.setBrush(body);
	p.drawRoundedRect(QRect(410, 430, 380, 370), 150, 150);
	auto head = QLinearGradient(0, 120, 0, 460);
	head.setColorAt(0., QColor(0xff, 0xd7, 0xa8));
	head.setColorAt(1., QColor(0xf2, 0xa1, 0x6b));
	p.setBrush(head);
	p.drawEllipse(QPoint(600, 290), 170, 170);
	p.setBrush(QColor(0x2d, 0x1f, 0x1a));
	p.drawEllipse(QPoint(540, 270), 18, 22);
	p.drawEllipse(QPoint(660, 270), 18, 22);
	p.setBrush(Qt::NoBrush);
	p.setPen(QPen(QColor(0x2d, 0x1f, 0x1a), 12, Qt::SolidLine, Qt::RoundCap));
	p.drawArc(QRect(530, 290, 140, 90), 200 * 16, 140 * 16);
	p.end();

	auto result = Vision::MaskResult();
	result.bounds = Vision::ContentBounds(image);
	result.cutout = std::move(image);
	result.ok = true;
	return result;
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;
	const auto size = QSize(style::ConvertScale(kSceneWidth), 0);
	RegisterBoxScene(u"ocr_result_box"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(TextBox, TextBoxArgs{
			.show = show,
			.recognize = [](TextDone done) { done(SampleText()); },
		});
	});
	RegisterBoxScene(u"ocr_empty_box"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(TextBox, TextBoxArgs{
			.show = show,
			.recognize = [](TextDone done) {
				done(Vision::TextResult{ .ok = true });
			},
		});
	});
	RegisterBoxScene(u"ocr_progress_box"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(TextBox, TextBoxArgs{
			.show = show,
			.recognize = [](TextDone) {},
		});
	});
	// The hint about the slow first run slides in after kSlowHintDelay.
	RegisterScene(SceneDescriptor{
		.name = u"ocr_progress_slow_box"_q,
		.size = size,
		.box = [](std::shared_ptr<Ui::Show> show) {
			return Box(TextBox, TextBoxArgs{
				.show = show,
				.recognize = [](TextDone) {},
			});
		},
		.wait = kSlowHintDelay + crl::time(1000),
	});
	RegisterBoxScene(u"cutout_box"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(CutoutBox, CutoutBoxArgs{
			.show = show,
			.remove = [](MaskDone done) { done(SampleCutout()); },
			.name = u"sample"_q,
			.sticker = [](QImage) {},
			.edit = [](QImage) {},
		});
	});
	RegisterBoxScene(u"cutout_failed_box"_q, size, [](
			std::shared_ptr<Ui::Show> show) {
		return Box(CutoutBox, CutoutBoxArgs{
			.show = show,
			.remove = [](MaskDone done) {
				done(Vision::MaskResult{ .nothingFound = true });
			},
		});
	});
});

} // namespace

void ShowRecognizedText(
		not_null<Window::SessionController*> controller,
		QImage image) {
	controller->show(Box(TextBox, TextBoxArgs{
		.show = controller->uiShow(),
		.recognize = RecognizeAsync(std::move(image)),
	}));
}

void ShowCutout(
		not_null<Window::SessionController*> controller,
		QImage image,
		QString name,
		Data::Thread *thread) {
	const auto weak = base::make_weak(controller);
	const auto weakThread = base::make_weak(thread);
	controller->show(Box(CutoutBox, CutoutBoxArgs{
		.show = controller->uiShow(),
		.remove = RemoveAsync(std::move(image)),
		.name = name,
		.sticker = [=](QImage cutout) {
			if (const auto strong = weak.get()) {
				auto source = StickerSource::FromImage(std::move(cutout));
				source.name = name;
				source.cutout = true;
				AddToStickerPack(strong, std::move(source));
			}
		},
		.edit = [=](QImage cutout) {
			if (const auto strong = weak.get()) {
				ShowPhotoEditorWithImage(
					strong,
					std::move(cutout),
					name,
					weakThread.get());
			}
		},
	}));
}

void AddVisionActions(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<PhotoData*> photo,
		HistoryItem *item) {
	if (photo->isNull()) {
		return;
	}
	const auto weak = base::make_weak(controller);
	const auto context = item ? item->fullId() : FullMsgId();
	const auto run = [=](Action action) {
		return [=] {
			if (const auto strong = weak.get()) {
				Run(strong, action, photo, nullptr, context);
			}
		};
	};
	if (Vision::TextRecognitionSupported()) {
		menu->addAction(
			tr::lng_oblivion_vision_text_context(tr::now),
			run(Action::Text),
			&st::menuIconFont);
	}
	if (Vision::BackgroundRemovalSupported()) {
		menu->addAction(
			tr::lng_oblivion_vision_cutout_context(tr::now),
			run(Action::Cutout),
			&st::menuIconStickerCreate);
	}
}

void AddVisionActions(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<DocumentData*> document,
		HistoryItem *item) {
	if (!PhotoEditorAcceptsDocument(document)) {
		return;
	}
	const auto weak = base::make_weak(controller);
	const auto context = item ? item->fullId() : FullMsgId();
	const auto run = [=](Action action) {
		return [=] {
			if (const auto strong = weak.get()) {
				Run(strong, action, nullptr, document, context);
			}
		};
	};
	if (Vision::TextRecognitionSupported()) {
		menu->addAction(
			tr::lng_oblivion_vision_text_context(tr::now),
			run(Action::Text),
			&st::menuIconFont);
	}
	if (Vision::BackgroundRemovalSupported()) {
		menu->addAction(
			tr::lng_oblivion_vision_cutout_context(tr::now),
			run(Action::Cutout),
			&st::menuIconStickerCreate);
	}
}

void AddMediaViewVisionActions(
		const Ui::Menu::MenuCallback &addAction,
		Fn<Window::SessionController*()> resolveWindow,
		PhotoData *photo,
		DocumentData *document,
		FullMsgId context,
		Fn<void()> close) {
	const auto usePhoto = photo && !photo->isNull();
	if (!resolveWindow
		|| (!usePhoto
			&& (!document || !PhotoEditorAcceptsDocument(document)))) {
		return;
	}
	const auto run = [=](Action action) {
		return [=] {
			const auto window = resolveWindow();
			if (!window) {
				return;
			}
			const auto weak = base::make_weak(window);
			if (close) {
				close();
			}
			if (const auto strong = weak.get()) {
				strong->window().activate();
				Run(
					strong,
					action,
					usePhoto ? photo : nullptr,
					usePhoto ? nullptr : document,
					context);
			}
		};
	};
	if (Vision::TextRecognitionSupported()) {
		addAction(
			tr::lng_oblivion_vision_text_context(tr::now),
			run(Action::Text),
			&st::mediaMenuIconCopy);
	}
	if (Vision::BackgroundRemovalSupported()) {
		addAction(
			tr::lng_oblivion_vision_cutout_context(tr::now),
			run(Action::Cutout),
			&st::mediaMenuIconStickers);
	}
}

} // namespace Oblivion
