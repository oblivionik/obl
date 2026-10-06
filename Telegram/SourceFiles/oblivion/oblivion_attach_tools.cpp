/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_attach_tools.h"

#include "api/api_common.h"
#include "base/base_file_utilities.h"
#include "base/random.h"
#include "base/unique_qptr.h"
#include "chat_helpers/compose/compose_show.h"
#include "core/file_location.h"
#include "data/data_chat_participant_status.h"
#include "data/data_document.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "dialogs/dialogs_key.h"
#include "editor/photo_editor_common.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "menu/menu_send_details.h"
#include "oblivion/oblivion_lang.h"
#include "oblivion/oblivion_lottie_editor.h"
#include "oblivion/oblivion_music_editor.h"
#include "oblivion/oblivion_photo_integration.h"
#include "oblivion/oblivion_round_video.h"
#include "oblivion/oblivion_settings.h"
#include "oblivion/oblivion_sticker_packs.h"
#include "oblivion/oblivion_sticker_trim.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "oblivion/oblivion_video_core.h"
#include "oblivion/oblivion_video_editor.h"
#include "oblivion/oblivion_vision.h"
#include "oblivion/oblivion_vision_ui.h"
#include "storage/localimageloader.h"
#include "storage/storage_account.h"
#include "storage/storage_media_prepare.h"
#include "ui/chat/attach/attach_controls.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/effects/animation_value.h"
#include "ui/effects/ripple_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/tooltip.h"
#include "window/window_session_controller.h"
#include "styles/style_basic.h"
#include "styles/style_boxes.h"
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_chat_style.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <crl/crl_async.h>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtGui/QLinearGradient>
#include <QtGui/QPainterPath>
#include <QtGui/QRadialGradient>

#include <atomic>

namespace Oblivion {
namespace AttachTools {
namespace {

constexpr auto kTooltipDelay = 800;
constexpr auto kClickCooldown = crl::time(600);

// A "GIF" is a short looped clip: longer videos are not offered the
// conversion, the video editor exports a fragment of them as a GIF.
constexpr auto kGifMaxDuration = crl::time(60'000);

// Telegram Desktop sends a video as a GIF when it has no audio track, is
// H.264 and takes at most 10 MB (Media::Clip, kMaxInMemory). The size of
// the frame is chosen to get a file somewhat below that, and when the
// encoder still writes more, the conversion is repeated with a smaller one.
constexpr auto kGifMaxBytes = int64(10) * 1024 * 1024;
constexpr auto kGifTargetBytes = int64(8) * 1024 * 1024;
constexpr auto kGifMinSide = 240;
constexpr auto kGifAttempts = 3;

// What the mp4 writer of VideoCore spends on a pixel of a frame when the
// bitrate is not given (bits, see Mp4Options::bitrate).
constexpr auto kMp4BitsPerPixel = 0.15;

// What Oblivion::Lottie::Unpack() and the Lottie editor accept.
constexpr auto kLottieMaxBytes = int64(5) * 1024 * 1024;

// The design sizes of a button, in pixels at the 100% interface scale.
constexpr auto kChipHeight = 28;
constexpr auto kChipFontSize = 12;
constexpr auto kChipIconSize = 16;
constexpr auto kRowTopSkip = 10;
constexpr auto kLineSkip = 6;
constexpr auto kChipOpacity = 0.12;
constexpr auto kChipOverOpacity = 0.18;

// What kind of a file is attached, as far as the tools are concerned.
enum class Kind : uchar {
	None,
	Image, // A photo or a static image file.
	Video,
	Gif, // An animated GIF file.
	Audio,
	Lottie, // A .tgs file.
	Json, // A .json file: Lottie only if its content says so.
};

enum class Tool : uchar {
	PhotoEditor,
	MakeSticker,
	RemoveBackground,
	CopyText,
	VideoEditor,
	RoundVideo,
	VideoSticker,
	Gif,
	MusicEditor,
	LottieEditor,
};

// What the box has found inside of the file.
enum class Content : uchar {
	Unknown,
	Image,
	Video,
	Song,
};

// Everything the choice of the tools needs to know about a file, taken
// out of Ui::PreparedFile so that the choice is a pure function.
struct FileTraits {
	Ui::PreparedFile::Type type = Ui::PreparedFile::Type::File;
	Content content = Content::Unknown;
	bool hasPixels = false; // Image: the decoded image is there.
	bool animated = false; // Image: more than one frame (GIF, .tgs).
	bool silent = false; // Video: no sound, Telegram sends it as a GIF.
	bool webmSticker = false; // Video: already a video sticker.
	crl::time duration = -1; // Video, -1 if unknown.
	bool hasPath = false;
	bool hasBytes = false;
	int64 size = 0; // 0 if unknown.
	QString name; // The file name with the extension.
	QString mime;
};

// What is possible here and now, besides the file itself.
struct Features {
	// The tools that are published as fillers of the "..." menu decide
	// themselves whether they take the file (see Filled() below).
	bool photoEditor = false;
	bool videoEditor = false;
	bool roundVideo = false;

	bool textRecognition = false; // macOS.
	bool backgroundRemoval = false; // macOS 14+.
	bool gifAllowed = true; // GIFs may be sent to this chat.
	bool lottieJson = false; // The .json file was read: it is Lottie.
};

[[nodiscard]] Kind KindOf(const FileTraits &traits) {
	using Type = Ui::PreparedFile::Type;

	// A .tgs has a thumbnail, so the box sees it as an animated image.
	const auto name = traits.name.toLower();
	const auto tgs = name.endsWith(u".tgs"_q)
		|| (traits.mime.toLower() == u"application/x-tgsticker"_q);
	const auto json = !tgs && name.endsWith(u".json"_q);
	if (tgs || json) {
		const auto fits = traits.hasPath
			&& (traits.size <= kLottieMaxBytes)
			&& (traits.content == Content::Unknown
				|| traits.content == Content::Image);
		return !fits ? Kind::None : tgs ? Kind::Lottie : Kind::Json;
	}
	const auto readable = traits.hasPath || traits.hasBytes;
	switch (traits.content) {
	case Content::Image: {
		if (!traits.hasPixels) {
			return Kind::None;
		} else if (traits.animated) {
			const auto gif = name.endsWith(u".gif"_q)
				|| (traits.mime.toLower() == u"image/gif"_q);
			return (gif && readable) ? Kind::Gif : Kind::None;
		}
		return (traits.type == Type::Photo || traits.type == Type::File)
			? Kind::Image
			: Kind::None;
	} break;
	case Content::Video:
		return (traits.type == Type::Video && readable)
			? Kind::Video
			: Kind::None;
	case Content::Song:
		return (traits.type == Type::Music && traits.hasPath)
			? Kind::Audio
			: Kind::None;
	case Content::Unknown: break;
	}
	return Kind::None;
}

// The buttons of the row, in the order they are shown in.
[[nodiscard]] std::vector<Tool> ToolsFor(
		const FileTraits &traits,
		const Features &features) {
	auto result = std::vector<Tool>();
	const auto add = [&](Tool tool, bool available = true) {
		if (available) {
			result.push_back(tool);
		}
	};
	switch (KindOf(traits)) {
	case Kind::Image:
		add(Tool::PhotoEditor, features.photoEditor);
		add(Tool::MakeSticker);
		add(Tool::RemoveBackground, features.backgroundRemoval);
		add(Tool::CopyText, features.textRecognition);
		break;
	case Kind::Video:
		add(Tool::VideoEditor, features.videoEditor);
		add(Tool::RoundVideo, features.roundVideo);
		add(Tool::VideoSticker, !traits.webmSticker);
		add(Tool::Gif, features.gifAllowed
			&& !traits.silent
			&& (traits.duration > 0)
			&& (traits.duration <= kGifMaxDuration));
		break;
	case Kind::Gif:
		add(Tool::VideoEditor);
		add(Tool::VideoSticker);
		break;
	case Kind::Audio:
		add(Tool::MusicEditor);
		break;
	case Kind::Lottie:
		add(Tool::LottieEditor);
		break;
	case Kind::Json:
		add(Tool::LottieEditor, features.lottieJson);
		break;
	case Kind::None:
		break;
	}
	return result;
}

// Whether the tool works with files of this kind at all.
[[nodiscard]] bool ToolFitsKind(Tool tool, Kind kind) {
	switch (tool) {
	case Tool::PhotoEditor:
	case Tool::MakeSticker:
	case Tool::RemoveBackground:
	case Tool::CopyText:
		return (kind == Kind::Image);
	case Tool::VideoEditor:
	case Tool::VideoSticker:
		return (kind == Kind::Video) || (kind == Kind::Gif);
	case Tool::RoundVideo:
	case Tool::Gif:
		return (kind == Kind::Video);
	case Tool::MusicEditor:
		return (kind == Kind::Audio);
	case Tool::LottieEditor:
		return (kind == Kind::Lottie) || (kind == Kind::Json);
	}
	return false;
}

// The tools that take the file away and send the result by themselves:
// the send box closes when they open. A GIF file is opened in the video
// editor over the box, which stays with its file.
[[nodiscard]] bool ToolClosesBox(Tool tool, Kind kind) {
	return (kind == Kind::Video)
		&& (tool == Tool::VideoEditor || tool == Tool::RoundVideo);
}

// "IMG 1234.HEIC" -> "IMG 1234", for the names the tools suggest.
[[nodiscard]] QString BaseName(const QString &fileName) {
	const auto base = QFileInfo(fileName).completeBaseName().trimmed();
	return base.isEmpty() ? QString() : base::FileNameFromUserString(base);
}

// The name of the file the "GIF" tool writes.
[[nodiscard]] QString GifFileName(const QString &fileName) {
	const auto base = BaseName(fileName);
	return (base.isEmpty() ? u"video"_q : base) + u".mp4"_q;
}

// The longer side of the GIF made of a video with this frame size and
// duration: as large as VideoCore makes it by default while the file is
// expected to stay within kGifTargetBytes. The video is never scaled up,
// so a side larger than the one of the source changes nothing.
[[nodiscard]] int GifMaxSide(QSize source, crl::time duration) {
	const auto seconds = std::max(duration, crl::time(1000)) / 1000.;
	const auto bits = (kGifTargetBytes * 8.) / seconds;
	const auto pixels = bits
		/ (kMp4BitsPerPixel * VideoCore::kGifVideoMaxFps);
	const auto ratio = source.isEmpty()
		? (16. / 9.)
		: (source.width() / float64(source.height()));
	const auto longer = std::sqrt(pixels * std::max(ratio, 1. / ratio));
	return int(std::clamp(
		std::floor(longer),
		float64(kGifMinSide),
		float64(VideoCore::kGifVideoMaxSide)));
}

struct ChipPlace {
	int line = 0;
	int left = 0;
	int width = 0;
};

struct ChipsLayout {
	std::vector<ChipPlace> places;
	int lines = 0;

	// Several lines of cells of the same width, as wide as the row.
	bool grid = false;
};

// How many lines the buttons take when no line is wider than the limit.
[[nodiscard]] int CountLines(
		const std::vector<int> &widths,
		int limit,
		int skip) {
	auto lines = 0;
	auto used = 0;
	for (const auto width : widths) {
		if (!lines || (used + skip + width > limit)) {
			++lines;
			used = width;
		} else {
			used += skip + width;
		}
	}
	return lines;
}

// The buttons keep their order and go from the left. They take as few
// lines as possible, and the lines are made about the same width: four
// buttons that don't fit into one line become two and two, not three and
// one. A button wider than the row is cut to the width of the row.
//
// In one line the buttons keep their own widths. Several lines become
// a grid when every button fits a cell of it: the cells are of the same
// width and fill the row, so the buttons stand in columns.
[[nodiscard]] ChipsLayout PlaceChips(
		const std::vector<int> &widths,
		int available,
		int skip) {
	auto result = ChipsLayout();
	if (widths.empty() || available <= 0) {
		return result;
	}
	const auto lines = CountLines(widths, available, skip);

	// The number of lines never grows with the limit, so the narrowest
	// limit that still gives this number of them is found by bisection.
	const auto widest = ranges::max(widths);
	auto low = std::min(widest, available);
	auto high = available;
	while (low < high) {
		const auto middle = (low + high) / 2;
		if (CountLines(widths, middle, skip) <= lines) {
			high = middle;
		} else {
			low = middle + 1;
		}
	}
	const auto limit = low;

	const auto count = int(widths.size());
	result.places.reserve(count);
	auto line = -1;
	auto used = 0;
	for (const auto full : widths) {
		const auto width = std::min(full, available);
		if ((line < 0) || (used + skip + width > limit)) {
			++line;
			result.places.push_back({
				.line = line,
				.left = 0,
				.width = width,
			});
			used = width;
		} else {
			result.places.push_back({
				.line = line,
				.left = used + skip,
				.width = width,
			});
			used += skip + width;
		}
	}
	result.lines = line + 1;
	if (result.lines < 2) {
		return result;
	}

	// The grid has the same number of lines: full ones, then the rest.
	const auto columns = (count + result.lines - 1) / result.lines;
	const auto cell = (available - skip * (columns - 1)) / columns;
	if (widest > cell) {
		return result;
	}
	for (auto i = 0; i != count; ++i) {
		const auto column = i % columns;
		auto &place = result.places[i];
		place.line = i / columns;
		place.left = column * (cell + skip);
		place.width = (column == columns - 1)
			? (available - place.left)
			: cell;
	}
	result.grid = true;
	return result;
}

// A line of two or more buttons is made as wide as the row, so that it
// ends where the preview above it ends: cells of the same width when every
// button fits one, otherwise each button gets the same share of the spare
// width. A single button and several lines stay as PlaceChips() made them.
// Returns whether the buttons were stretched: they center their content.
[[nodiscard]] bool StretchLine(
		ChipsLayout &layout,
		int available,
		int skip) {
	const auto count = int(layout.places.size());
	if (layout.lines != 1 || count < 2) {
		return false;
	}
	const auto &last = layout.places.back();
	const auto spare = available - (last.left + last.width);
	if (spare <= 0) {
		return false;
	}
	auto widest = 0;
	for (const auto &place : layout.places) {
		widest = std::max(widest, place.width);
	}
	const auto cell = (available - skip * (count - 1)) / count;
	const auto share = spare / count;
	const auto extra = spare % count;
	auto left = 0;
	for (auto i = 0; i != count; ++i) {
		auto &place = layout.places[i];
		const auto width = (i == count - 1)
			? (available - left)
			: (widest <= cell)
			? cell
			: (place.width + share + ((i < extra) ? 1 : 0));
		place.left = left;
		place.width = width;
		left += width + skip;
	}
	return true;
}

[[nodiscard]] int Scaled(int value) {
	return style::ConvertScale(value);
}

[[nodiscard]] QString FormatPercent(float64 progress) {
	const auto percent = int(std::round(std::clamp(progress, 0., 1.) * 100));
	return QString::number(percent) + QChar('%');
}

[[nodiscard]] QString ToolText(Tool tool) {
	switch (tool) {
	case Tool::PhotoEditor:
		return tr::lng_oblivion_attach_photo_editor(tr::now);
	case Tool::MakeSticker:
		return tr::lng_oblivion_attach_sticker(tr::now);
	case Tool::RemoveBackground:
		return tr::lng_oblivion_attach_cutout(tr::now);
	case Tool::CopyText:
		return tr::lng_oblivion_attach_text(tr::now);
	case Tool::VideoEditor:
		return tr::lng_oblivion_attach_video_editor(tr::now);
	case Tool::RoundVideo:
		return tr::lng_oblivion_attach_round(tr::now);
	case Tool::VideoSticker:
		return tr::lng_oblivion_attach_video_sticker(tr::now);
	case Tool::Gif:
		return tr::lng_oblivion_attach_gif(tr::now);
	case Tool::MusicEditor:
		return tr::lng_oblivion_attach_music(tr::now);
	case Tool::LottieEditor:
		return tr::lng_oblivion_attach_lottie(tr::now);
	}
	return QString();
}

[[nodiscard]] QString ToolTooltip(Tool tool, Kind kind) {
	switch (tool) {
	case Tool::PhotoEditor:
		return tr::lng_oblivion_attach_photo_editor_tip(tr::now);
	case Tool::MakeSticker:
		return tr::lng_oblivion_attach_sticker_tip(tr::now);
	case Tool::RemoveBackground:
		return tr::lng_oblivion_attach_cutout_tip(tr::now);
	case Tool::CopyText:
		return tr::lng_oblivion_attach_text_tip(tr::now);
	case Tool::VideoEditor:
		return ToolClosesBox(tool, kind)
			? tr::lng_oblivion_attach_video_editor_closes_tip(tr::now)
			: tr::lng_oblivion_attach_video_editor_tip(tr::now);
	case Tool::RoundVideo:
		return tr::lng_oblivion_attach_round_tip(tr::now);
	case Tool::VideoSticker:
		return tr::lng_oblivion_attach_video_sticker_tip(tr::now);
	case Tool::Gif:
		return tr::lng_oblivion_attach_gif_tip(tr::now);
	case Tool::MusicEditor:
		return tr::lng_oblivion_attach_music_tip(tr::now);
	case Tool::LottieEditor:
		return tr::lng_oblivion_attach_lottie_tip(tr::now);
	}
	return QString();
}

// The icons are drawn on a 16x16 grid with one line width, in the given
// color, so they follow the palette of the button.
void PaintToolIcon(QPainter &p, Tool tool, QRectF rect, QColor color) {
	const auto unit = rect.width() / 16.;
	const auto at = [&](float64 x, float64 y) {
		return QPointF(rect.x() + x * unit, rect.y() + y * unit);
	};
	const auto line = QPen(
		color,
		1.3 * unit,
		Qt::SolidLine,
		Qt::RoundCap,
		Qt::RoundJoin);
	const auto stroke = [&](const QPainterPath &path) {
		p.setPen(line);
		p.setBrush(Qt::NoBrush);
		p.drawPath(path);
	};
	const auto fill = [&](const QPainterPath &path) {
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawPath(path);
	};
	const auto dot = [&](float64 x, float64 y, float64 rx, float64 ry) {
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawEllipse(at(x, y), rx * unit, ry * unit);
	};
	const auto ring = [&](float64 x, float64 y, float64 radius) {
		p.setPen(line);
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(at(x, y), radius * unit, radius * unit);
	};

	// Filled, with the corners rounded a bit.
	const auto triangle = [&](QPointF a, QPointF b, QPointF c) {
		auto path = QPainterPath();
		path.moveTo(a);
		path.lineTo(b);
		path.lineTo(c);
		path.closeSubpath();
		p.setPen(QPen(
			color,
			0.9 * unit,
			Qt::SolidLine,
			Qt::RoundCap,
			Qt::RoundJoin));
		p.setBrush(color);
		p.drawPath(path);
	};

	// A sticker: a rounded square with a peeled corner.
	const auto sticker = [&] {
		auto path = QPainterPath();
		path.moveTo(at(9.2, 13.5));
		path.lineTo(at(5.5, 13.5));
		path.quadTo(at(2.5, 13.5), at(2.5, 10.5));
		path.lineTo(at(2.5, 5.5));
		path.quadTo(at(2.5, 2.5), at(5.5, 2.5));
		path.lineTo(at(10.5, 2.5));
		path.quadTo(at(13.5, 2.5), at(13.5, 5.5));
		path.lineTo(at(13.5, 9.2));
		path.lineTo(at(9.2, 13.5));
		path.moveTo(at(13.5, 9.2));
		path.lineTo(at(11.2, 9.2));
		path.quadTo(at(9.2, 9.2), at(9.2, 11.2));
		path.lineTo(at(9.2, 13.5));
		stroke(path);
	};

	switch (tool) {
	case Tool::PhotoEditor: {
		// Two sliders.
		auto path = QPainterPath();
		path.moveTo(at(2.2, 5.));
		path.lineTo(at(7.3, 5.));
		path.moveTo(at(12.1, 5.));
		path.lineTo(at(13.8, 5.));
		path.moveTo(at(2.2, 11.));
		path.lineTo(at(3.9, 11.));
		path.moveTo(at(8.7, 11.));
		path.lineTo(at(13.8, 11.));
		stroke(path);
		ring(9.7, 5., 2.);
		ring(6.3, 11., 2.);
	} break;
	case Tool::MakeSticker: {
		sticker();
	} break;
	case Tool::RemoveBackground: {
		// A figure in a dashed frame.
		auto dashed = line;
		dashed.setCapStyle(Qt::FlatCap);
		dashed.setDashPattern({ 1.5, 1.5 });
		p.setPen(dashed);
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(
			QRectF(at(2., 2.), at(14., 14.)),
			3. * unit,
			3. * unit);
		dot(8., 6.3, 1.9, 1.9);
		auto body = QPainterPath();
		body.moveTo(at(4.7, 12.3));
		body.cubicTo(at(4.7, 10.), at(6.2, 9.2), at(8., 9.2));
		body.cubicTo(at(9.8, 9.2), at(11.3, 10.), at(11.3, 12.3));
		body.closeSubpath();
		fill(body);
	} break;
	case Tool::CopyText: {
		// The letter T in the corners of a frame.
		auto path = QPainterPath();
		path.moveTo(at(2., 4.8));
		path.lineTo(at(2., 2.));
		path.lineTo(at(4.8, 2.));
		path.moveTo(at(11.2, 2.));
		path.lineTo(at(14., 2.));
		path.lineTo(at(14., 4.8));
		path.moveTo(at(14., 11.2));
		path.lineTo(at(14., 14.));
		path.lineTo(at(11.2, 14.));
		path.moveTo(at(4.8, 14.));
		path.lineTo(at(2., 14.));
		path.lineTo(at(2., 11.2));
		path.moveTo(at(5.4, 5.6));
		path.lineTo(at(10.6, 5.6));
		path.moveTo(at(8., 5.6));
		path.lineTo(at(8., 10.8));
		stroke(path);
	} break;
	case Tool::VideoEditor: {
		// A fragment between two trim marks.
		auto path = QPainterPath();
		path.moveTo(at(4.5, 2.6));
		path.lineTo(at(2.4, 2.6));
		path.lineTo(at(2.4, 13.4));
		path.lineTo(at(4.5, 13.4));
		path.moveTo(at(11.5, 2.6));
		path.lineTo(at(13.6, 2.6));
		path.lineTo(at(13.6, 13.4));
		path.lineTo(at(11.5, 13.4));
		stroke(path);
		triangle(at(6.5, 5.6), at(6.5, 10.4), at(10.2, 8.));
	} break;
	case Tool::RoundVideo: {
		ring(8., 8., 5.9);
		triangle(at(6.8, 5.7), at(6.8, 10.3), at(10.5, 8.));
	} break;
	case Tool::VideoSticker: {
		sticker();
		triangle(at(5.6, 5.3), at(5.6, 9.1), at(8.6, 7.2));
	} break;
	case Tool::Gif: {
		// A loop, as the "repeat" button of a player: an arrow to the
		// right above an arrow to the left, joined by rounded corners.
		auto path = QPainterPath();
		path.moveTo(at(2.8, 8.2));
		path.lineTo(at(2.8, 7.));
		path.quadTo(at(2.8, 4.6), at(5.2, 4.6));
		path.lineTo(at(12.4, 4.6));
		path.moveTo(at(10.4, 2.6));
		path.lineTo(at(12.4, 4.6));
		path.lineTo(at(10.4, 6.6));
		path.moveTo(at(13.2, 7.8));
		path.lineTo(at(13.2, 9.));
		path.quadTo(at(13.2, 11.4), at(10.8, 11.4));
		path.lineTo(at(3.6, 11.4));
		path.moveTo(at(5.6, 9.4));
		path.lineTo(at(3.6, 11.4));
		path.lineTo(at(5.6, 13.4));
		stroke(path);
	} break;
	case Tool::MusicEditor: {
		// A note.
		dot(5.6, 11.8, 2.3, 1.8);
		auto path = QPainterPath();
		path.moveTo(at(7.6, 11.6));
		path.lineTo(at(7.6, 3.));
		path.cubicTo(at(8.4, 5.4), at(12., 5.2), at(11.4, 8.6));
		stroke(path);
	} break;
	case Tool::LottieEditor: {
		// A curve between two points.
		auto path = QPainterPath();
		path.moveTo(at(3.2, 12.6));
		path.cubicTo(at(7.4, 12.6), at(8.6, 3.4), at(12.8, 3.4));
		stroke(path);
		const auto point = [&](float64 x, float64 y) {
			auto square = QPainterPath();
			square.addRoundedRect(
				QRectF(at(x - 1.5, y - 1.5), at(x + 1.5, y + 1.5)),
				0.6 * unit,
				0.6 * unit);
			fill(square);
		};
		point(3.2, 12.6);
		point(12.8, 3.4);
	} break;
	}
}

[[nodiscard]] const style::font &ChipFont() {
	static const auto result = style::font(
		Scaled(kChipFontSize),
		st::semiboldFont->flags(),
		st::semiboldFont->family());
	return result;
}

// The usual and the tight look of a button: the tight one is used when it
// lets all the buttons stay in one line.
struct ChipMetrics {
	int left = 0;
	int iconSkip = 0;
	int right = 0;
	int skip = 0; // Between two buttons of a line.
};

[[nodiscard]] ChipMetrics MetricsFor(bool tight) {
	return tight
		? ChipMetrics{
			.left = Scaled(5),
			.iconSkip = Scaled(3),
			.right = Scaled(6),
			.skip = Scaled(4),
		}
		: ChipMetrics{
			.left = Scaled(8),
			.iconSkip = Scaled(5),
			.right = Scaled(11),
			.skip = Scaled(kLineSkip),
		};
}

// One button of the row: a painted icon and a short name, the tooltip
// tells what the tool does with a file of this kind.
class Chip final
	: public Ui::RippleButton
	, public Ui::AbstractTooltipShower {
public:
	Chip(QWidget *parent, Tool tool, Kind kind);

	[[nodiscard]] int fitWidth(bool tight) const;

	// centered: the button is a cell of a grid or of a stretched line,
	// wider than it needs.
	void setLook(bool tight, bool centered);

	QString tooltipText() const override;
	QPoint tooltipPos() const override;
	bool tooltipWindowActive() const override;

protected:
	void paintEvent(QPaintEvent *e) override;
	void enterEventHook(QEnterEvent *e) override;
	void leaveEventHook(QEvent *e) override;

	QImage prepareRippleMask() const override;

private:
	const Tool _tool;
	const QString _text;
	const QString _tooltip;
	const int _textWidth = 0;
	bool _tight = false;
	bool _centered = false;

};

Chip::Chip(QWidget *parent, Tool tool, Kind kind)
: RippleButton(parent, st::defaultLightButton.ripple)
, _tool(tool)
, _text(ToolText(tool))
, _tooltip(ToolTooltip(tool, kind))
, _textWidth(ChipFont()->width(_text)) {
	setAccessibleName(_tooltip.isEmpty() ? _text : _tooltip);
	resize(fitWidth(false), Scaled(kChipHeight));
}

int Chip::fitWidth(bool tight) const {
	const auto metrics = MetricsFor(tight);
	return metrics.left
		+ Scaled(kChipIconSize)
		+ metrics.iconSkip
		+ _textWidth
		+ metrics.right;
}

void Chip::setLook(bool tight, bool centered) {
	if (_tight != tight || _centered != centered) {
		_tight = tight;
		_centered = centered;
		update();
	}
}

QString Chip::tooltipText() const {
	return _tooltip;
}

QPoint Chip::tooltipPos() const {
	return QCursor::pos();
}

bool Chip::tooltipWindowActive() const {
	return Ui::AppInFocus() && Ui::InFocusChain(window());
}

void Chip::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	// A tint of the text color: seen on the box background of any theme
	// (in the default one it is the color of a light button under the
	// mouse), a bit stronger under the mouse.
	const auto color = st::lightButtonFg->c;
	const auto radius = height() / 2.;
	p.setPen(Qt::NoPen);
	p.setBrush(anim::with_alpha(
		color,
		isOver() ? kChipOverOpacity : kChipOpacity));
	p.drawRoundedRect(rect(), radius, radius);
	paintRipple(p, 0, 0);

	const auto metrics = MetricsFor(_tight);
	const auto icon = Scaled(kChipIconSize);
	const auto content = icon + metrics.iconSkip + _textWidth;
	const auto left = _centered
		? std::max((width() - content) / 2, metrics.left)
		: metrics.left;
	PaintToolIcon(
		p,
		_tool,
		QRectF(left, (height() - icon) / 2., icon, icon),
		color);

	const auto &font = ChipFont();
	const auto textLeft = left + icon + metrics.iconSkip;
	const auto available = width() - textLeft - metrics.right;
	if (available <= 0) {
		return;
	}
	p.setFont(font);
	p.setPen(color);
	p.drawText(
		textLeft,
		(height() - font->height) / 2 + font->ascent,
		(available < _textWidth) ? font->elided(_text, available) : _text);
}

void Chip::enterEventHook(QEnterEvent *e) {
	if (!_tooltip.isEmpty()) {
		Ui::Tooltip::Show(kTooltipDelay, this);
	}
	RippleButton::enterEventHook(e);
}

void Chip::leaveEventHook(QEvent *e) {
	Ui::Tooltip::Hide();
	RippleButton::leaveEventHook(e);
}

QImage Chip::prepareRippleMask() const {
	return Ui::RippleAnimation::RoundRectMask(size(), height() / 2);
}

// The row itself only shows the buttons it is given and tells which one
// was clicked: everything that needs the box, the chat or the account is
// done by the code below it, so the row is the same in the UI snapshots.
class Row final : public Ui::RpWidget {
public:
	explicit Row(QWidget *parent);

	// kind is the kind of the file the tools are for: the tooltips of some
	// of them depend on it.
	void setTools(const std::vector<Tool> &tools, Kind kind);
	void setClicked(Fn<void(Tool)> callback);

protected:
	int resizeGetHeight(int newWidth) override;

private:
	std::vector<Tool> _tools;
	Kind _kind = Kind::None;
	std::vector<QPointer<Chip>> _chips;
	Fn<void(Tool)> _clicked;
	crl::time _clickedTime = 0;

};

Row::Row(QWidget *parent) : RpWidget(parent) {
	hide();
	resize(width(), 0);
}

void Row::setTools(const std::vector<Tool> &tools, Kind kind) {
	if (_tools == tools && _kind == kind) {
		return;
	}
	_tools = tools;
	_kind = kind;

	// May be called from a click on one of them.
	for (const auto &chip : base::take(_chips)) {
		if (chip) {
			chip->hide();
			chip->deleteLater();
		}
	}
	for (const auto tool : _tools) {
		const auto chip = Ui::CreateChild<Chip>(this, tool, kind);
		chip->setClickedCallback([=] {
			Ui::Tooltip::Hide();

			// A double click must not open a tool twice.
			const auto now = crl::now();
			if (_clickedTime && (now - _clickedTime < kClickCooldown)) {
				return;
			}
			_clickedTime = now;

			// The tool may close the box together with this button: it
			// is started when the click is over.
			crl::on_main(this, [=] {
				if (_clicked && ranges::contains(_tools, tool)) {
					_clicked(tool);
				}
			});
		});
		chip->show();
		_chips.push_back(chip);
	}

	// The box follows the height of the row by its resize events, and
	// a hidden widget gets none: the row becomes empty before it hides
	// and is shown before it grows.
	if (_tools.empty()) {
		resizeToWidth(widthNoMargins());
		hide();
	} else {
		show();
		resizeToWidth(widthNoMargins());
	}
}

void Row::setClicked(Fn<void(Tool)> callback) {
	_clicked = std::move(callback);
}

int Row::resizeGetHeight(int newWidth) {
	if (_chips.empty() || newWidth <= 0) {
		return 0;
	}
	const auto layoutFor = [&](bool tight) {
		auto widths = std::vector<int>();
		widths.reserve(_chips.size());
		for (const auto &chip : _chips) {
			widths.push_back(chip ? chip->fitWidth(tight) : 1);
		}
		return PlaceChips(widths, newWidth, MetricsFor(tight).skip);
	};

	// One line of the usual buttons, or one line of the tight ones, or
	// several lines of the usual ones.
	auto tight = false;
	auto layout = layoutFor(false);
	if (layout.lines > 1) {
		auto other = layoutFor(true);
		if (other.lines == 1) {
			tight = true;
			layout = std::move(other);
		}
	}

	// Two and more buttons always end at the right edge of the preview,
	// in one line as well as in a grid.
	const auto stretched = StretchLine(
		layout,
		newWidth,
		MetricsFor(tight).skip);
	const auto top = Scaled(kRowTopSkip);
	const auto height = Scaled(kChipHeight);
	const auto step = height + Scaled(kLineSkip);
	for (auto i = 0, total = int(_chips.size()); i != total; ++i) {
		if (const auto chip = _chips[i].data()) {
			const auto &place = layout.places[i];
			chip->setLook(tight, layout.grid || stretched);
			chip->setGeometryToLeft(
				place.left,
				top + place.line * step,
				place.width,
				height,
				newWidth);
		}
	}
	return top + layout.lines * step - Scaled(kLineSkip);
}

// The progress of the "GIF" tool.
class ProgressLine final : public Ui::RpWidget {
public:
	ProgressLine(QWidget *parent, rpl::producer<float64> progress);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;

private:
	float64 _value = 0.;

};

ProgressLine::ProgressLine(
	QWidget *parent,
	rpl::producer<float64> progress)
: RpWidget(parent) {
	std::move(
		progress
	) | rpl::on_next([=](float64 value) {
		_value = std::clamp(value, 0., 1.);
		update();
	}, lifetime());
}

int ProgressLine::resizeGetHeight(int newWidth) {
	return Scaled(4);
}

void ProgressLine::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto radius = height() / 2.;
	p.setPen(Qt::NoPen);
	p.setBrush(st::windowBgOver);
	p.drawRoundedRect(rect(), radius, radius);
	const auto filled = std::max(
		int(std::round(width() * _value)),
		height());
	p.setBrush(st::windowBgActive);
	p.drawRoundedRect(QRect(0, 0, filled, height()), radius, radius);
}

void GifBox(
		not_null<Ui::GenericBox*> box,
		rpl::producer<float64> progress,
		Fn<void()> closed) {
	box->setTitle(tr::lng_oblivion_attach_gif_title());
	box->setWidth(st::boxWidth);
	box->setCloseByOutsideClick(false);

	const auto &padding = st::boxRowPadding;
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			rpl::duplicate(
				progress
			) | rpl::map([](float64 value) {
				return tr::lng_oblivion_attach_gif_progress(
					tr::now,
					lt_percent,
					FormatPercent(value));
			}),
			st::boxLabel),
		padding);
	box->addRow(
		object_ptr<ProgressLine>(box, std::move(progress)),
		style::margins(padding.left(), Scaled(10), padding.right(), 0));
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_oblivion_attach_gif_about(),
			st::boxDividerLabel),
		style::margins(
			padding.left(),
			Scaled(12),
			padding.right(),
			Scaled(4)));

	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
	box->boxClosing() | rpl::on_next([=] {
		if (closed) {
			closed();
		}
	}, box->lifetime());
}

// What the row needs from the box and what its tools keep between the
// clicks. Lives in the lifetime of the row: the callbacks that come later
// (from another thread, from a box above) are guarded by the row.
struct Context {
	AttachToolsArgs args;

	// Paths of the .json files that were read: whether they are Lottie.
	base::flat_map<QString, bool> json;
	base::flat_set<QString> jsonReading;

	// The music editor opens documents: one local document for a file.
	base::flat_map<QString, DocumentId> music;

	// The running conversion to a GIF, if there is one.
	VideoCore::Cancel gifCancel;
	rpl::variable<float64> gifProgress = 0.;
	base::weak_qptr<Ui::BoxContent> gifBox;

	bool lottieReading = false;
};

[[nodiscard]] FileTraits TraitsOf(const Ui::PreparedFile &file) {
	using Information = Ui::PreparedFileInformation;

	auto result = FileTraits();
	result.type = file.type;
	result.hasPath = !file.path.isEmpty();
	result.hasBytes = !file.content.isEmpty();
	result.size = file.size;

	// The name shown in the box can be changed there, the kind of a file
	// on the disk is told by its real name.
	result.name = result.hasPath
		? QFileInfo(file.path).fileName()
		: file.displayName;
	if (const auto information = file.information.get()) {
		result.mime = information->filemime;
		const auto media = &information->media;
		if (const auto image = std::get_if<Information::Image>(media)) {
			result.content = Content::Image;
			result.hasPixels = !image->data.isNull();
			result.animated = image->animated;
		} else if (const auto video = std::get_if<Information::Video>(
				media)) {
			result.content = Content::Video;
			result.silent = video->isGifv;
			result.webmSticker = video->isWebmSticker;
			result.duration = video->duration;
		} else if (v::is<Information::Song>(*media)) {
			result.content = Content::Song;
		}
	}
	return result;
}

// The image of an attached photo as it will be sent: with the changes of
// the built-in editor applied (on the main thread, as the box does it).
[[nodiscard]] QImage FileImage(const Ui::PreparedFile &file) {
	using Image = Ui::PreparedFileInformation::Image;

	const auto image = file.information
		? std::get_if<Image>(&file.information->media)
		: nullptr;
	if (!image || image->data.isNull()) {
		return QImage();
	}
	return image->modifications
		? Editor::ImageModified(image->data, image->modifications)
		: image->data;
}

// Some of the tools are published only as fillers of the "..." menu of
// the send files box. Such a filler is run on a menu that is never shown:
// whether it has added its item tells whether the tool takes this file,
// and triggering the item is exactly what a click on it in the menu does.
//
// As after a click in a shown menu, the callback of the item is called
// through the event loop (Ui::Menu::CreateAction() connects it queued, with
// the action as the context): the menu is deleted only after that. It has
// no parent, since the item may close the box.
[[nodiscard]] QAction *FilledAction(not_null<Ui::PopupMenu*> menu) {
	for (const auto &action : menu->actions()) {
		if (!action->isSeparator() && action->isEnabled()) {
			return action;
		}
	}
	return nullptr;
}

template <typename Filler>
[[nodiscard]] bool Filled(Filler &&fill) {
	const auto menu = base::make_unique_q<Ui::PopupMenu>(nullptr);
	fill(not_null<Ui::PopupMenu*>(menu.get()));
	return FilledAction(menu.get()) != nullptr;
}

template <typename Filler>
[[nodiscard]] bool TriggerFilled(Filler &&fill) {
	auto menu = base::make_unique_q<Ui::PopupMenu>(nullptr);
	fill(not_null<Ui::PopupMenu*>(menu.get()));
	const auto action = FilledAction(menu.get());
	if (!action) {
		return false;
	}
	action->trigger();
	menu.release()->deleteLater();
	return true;
}

[[nodiscard]] SendMenu::Details DetailsFor(not_null<Context*> context) {
	return context->args.sendMenuDetails
		? context->args.sendMenuDetails()
		: SendMenu::Details();
}

[[nodiscard]] FullReplyTo ReplyFor(not_null<Context*> context) {
	return context->args.replyTo ? context->args.replyTo() : FullReplyTo();
}

// The only file of the box, see HoldsOnlyFile(). Points into the list of
// the box: use it right away.
[[nodiscard]] const Ui::PreparedFile *SingleFile(
		not_null<Context*> context) {
	const auto reading = context->args.busy && context->args.busy();
	if (!context->args.list || reading) {
		return nullptr;
	}
	const auto &list = context->args.list();
	return HoldsOnlyFile(list, reading) ? &list.files.front() : nullptr;
}

[[nodiscard]] Window::SessionController *ResolveWindow(
		not_null<Context*> context) {
	const auto &show = context->args.show;
	if (!show || !show->valid()) {
		return nullptr;
	}
	const auto window = show->resolveWindow();
	return (window && (&window->session() == &context->args.peer->session()))
		? window
		: nullptr;
}

// The chat whose compose area has opened the box, for "Send to <chat>"
// in the tools: only such boxes know their chat (not the ones of
// scheduled messages, business shortcuts or story replies).
[[nodiscard]] Data::Thread *ComposeThread(
		not_null<Window::SessionController*> window,
		not_null<Context*> context,
		const SendMenu::Details &details) {
	const auto peer = context->args.peer;
	if (context->args.sendType != Api::SendType::Normal
		|| details.barePeerId != peer->id.value) {
		return nullptr;
	}
	const auto topicRootId = MsgId(details.bareTopicRootId);
	if (const auto active = window->activeChatCurrent().thread()) {
		if (active->peer() == peer && active->topicRootId() == topicRootId) {
			return active;
		}
	}
	if (topicRootId) {
		return peer->forumTopicFor(topicRootId);
	}
	return peer->owner().history(peer).get();
}

void FillPhotoEditor(
		not_null<Ui::PopupMenu*> menu,
		not_null<QWidget*> box,
		not_null<Context*> context,
		const Ui::PreparedFile &file) {
	const auto replace = context->args.replace;
	AddAttachPhotoEditorAction(
		menu,
		context->args.show,
		box,
		file,
		[=](Fn<void(Ui::PreparedFile&)> apply) {
			if (replace) {
				replace(0, std::move(apply));
			}
		});
}

void FillVideoEditor(
		not_null<Ui::PopupMenu*> menu,
		not_null<Context*> context) {
	AddSendFilesVideoEditorAction(
		menu,
		context->args.show,
		context->args.peer,
		DetailsFor(context),
		context->args.sendType,
		context->args.list(),
		context->args.closeBox);
}

void FillRoundVideo(
		not_null<Ui::PopupMenu*> menu,
		not_null<Context*> context) {
	AddSendAsRoundAction(
		menu,
		context->args.show,
		context->args.peer,
		DetailsFor(context),
		context->args.sendType,
		context->args.list(),
		ReplyFor(context),
		context->args.closeBox);
}

[[nodiscard]] QByteArray ReadBytes(const QString &path, int64 limit) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly) || file.size() > limit) {
		return QByteArray();
	}
	return file.readAll();
}

[[nodiscard]] bool WriteBytes(const QString &path, const QByteArray &bytes) {
	if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
		return false;
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return false;
	}
	const auto written = (file.write(bytes) == bytes.size());
	file.close();
	return written && (file.error() == QFileDevice::NoError);
}

[[nodiscard]] bool IsLottieData(const QByteArray &bytes) {
	return !bytes.isEmpty() && LottieEdit::Document::FromData(bytes).valid();
}

// The results of the "GIF" tool are files in the temporary folder of the
// account, they must live until they are uploaded. Every launch writes
// into a folder of its own, and what the previous launches have left is
// removed when the first row of this launch is created: nothing is being
// uploaded from there after a restart.
[[nodiscard]] QString TempRoot(not_null<Main::Session*> session) {
	return QDir(session->local().tempDirectory()).filePath(
		u"oblivion_attach"_q);
}

[[nodiscard]] QString LaunchFolder() {
	static const auto result = QString::number(
		base::RandomValue<uint32>(),
		16);
	return result;
}

void CleanupTempOnce(not_null<Main::Session*> session) {
	static auto Cleaned = base::flat_set<QString>();
	const auto root = TempRoot(session);
	if (!Cleaned.emplace(root).second) {
		return;
	}
	const auto keep = LaunchFolder();
	crl::async([=] {
		const auto folder = QDir(root);
		const auto filter = QDir::Dirs | QDir::NoDotAndDotDot;
		for (const auto &name : folder.entryList(filter)) {
			if (name != keep) {
				QDir(folder.filePath(name)).removeRecursively();
			}
		}
	});
}

[[nodiscard]] QString NextTempPath(
		not_null<Main::Session*> session,
		const QString &fileName) {
	static auto Counter = 0;
	return QDir(TempRoot(session)).filePath(LaunchFolder()
		+ QChar('/')
		+ QString::number(++Counter)
		+ QChar('/')
		+ fileName);
}

void Refresh(not_null<Row*> row, not_null<Context*> context);

// Reads the file off the main thread once, the row is refreshed when the
// answer is known. Until then the file is not a Lottie one.
[[nodiscard]] bool JsonIsLottie(
		not_null<Row*> row,
		not_null<Context*> context,
		const QString &path) {
	const auto i = context->json.find(path);
	if (i != context->json.end()) {
		return i->second;
	} else if (!context->jsonReading.emplace(path).second) {
		return false;
	}
	const auto weak = QPointer<Row>(row.get());
	crl::async([=] {
		const auto lottie = IsLottieData(ReadBytes(path, kLottieMaxBytes));
		crl::on_main(weak, [=] {
			context->jsonReading.remove(path);
			context->json[path] = lottie;
			Refresh(weak.data(), context);
		});
	});
	return false;
}

[[nodiscard]] Features FeaturesFor(
		not_null<Row*> row,
		not_null<Context*> context,
		const Ui::PreparedFile &file,
		const FileTraits &traits) {
	auto result = Features();
	result.textRecognition = Vision::TextRecognitionSupported();
	result.backgroundRemoval = Vision::BackgroundRemovalSupported();
	switch (KindOf(traits)) {
	case Kind::Image:
		if (const auto box = row->parentWidget()) {
			result.photoEditor = Filled([&](not_null<Ui::PopupMenu*> menu) {
				FillPhotoEditor(menu, box, context, file);
			});
		}
		break;
	case Kind::Video:
		result.videoEditor = Filled([&](not_null<Ui::PopupMenu*> menu) {
			FillVideoEditor(menu, context);
		});
		result.roundVideo = Filled([&](not_null<Ui::PopupMenu*> menu) {
			FillRoundVideo(menu, context);
		});
		result.gifAllowed = !Data::RestrictionError(
			context->args.peer,
			ChatRestriction::SendGifs);
		break;
	case Kind::Json:
		result.lottieJson = JsonIsLottie(row, context, file.path);
		break;
	default:
		break;
	}
	return result;
}

void Refresh(not_null<Row*> row, not_null<Context*> context) {
	const auto file = SingleFile(context);
	if (!file) {
		row->setTools({}, Kind::None);
		return;
	}
	const auto traits = TraitsOf(*file);
	row->setTools(
		ToolsFor(traits, FeaturesFor(row, context, *file, traits)),
		KindOf(traits));
}

void ShowOpenFailed(not_null<Context*> context) {
	context->args.show->showToast(
		tr::lng_oblivion_attach_open_failed(tr::now));
}

void RunPhotoEditor(
		not_null<Row*> row,
		not_null<Context*> context,
		const Ui::PreparedFile &file) {
	const auto box = row->parentWidget();
	const auto triggered = box
		&& TriggerFilled([&](not_null<Ui::PopupMenu*> menu) {
			FillPhotoEditor(menu, box, context, file);
		});
	if (!triggered) {
		ShowOpenFailed(context);
	}
}

void RunImageTool(
		not_null<Context*> context,
		Tool tool,
		const Ui::PreparedFile &file,
		const FileTraits &traits) {
	const auto window = ResolveWindow(context);
	if (!window) {
		return;
	}
	auto image = FileImage(file);
	if (image.isNull()) {
		ShowOpenFailed(context);
		return;
	}
	const auto base = BaseName(traits.name);
	if (tool == Tool::MakeSticker) {
		auto source = StickerSource::FromImage(std::move(image));
		source.name = base;
		AddToStickerPack(
			not_null<Window::SessionController*>(window),
			std::move(source));
	} else if (tool == Tool::RemoveBackground) {
		ShowCutout(
			window,
			std::move(image),
			base.isEmpty() ? u"image"_q : base,
			ComposeThread(window, context, DetailsFor(context)));
	} else if (tool == Tool::CopyText) {
		ShowRecognizedText(window, std::move(image));
	}
}

void RunVideoEditor(
		not_null<Context*> context,
		Kind kind,
		const Ui::PreparedFile &file) {
	const auto path = file.path;
	const auto content = path.isEmpty() ? file.content : QByteArray();

	// A video goes the way of the "..." menu item: the box is closed
	// when the editor is really opened for it.
	if (kind == Kind::Video
		&& TriggerFilled([&](not_null<Ui::PopupMenu*> menu) {
			FillVideoEditor(menu, context);
		})) {
		return;
	}

	// A GIF file is opened over the box, which stays with its file. For
	// a video this is reached only when the editor is busy with another
	// one, it says so itself.
	const auto window = ResolveWindow(context);
	if (!window) {
		return;
	}
	const auto thread = ComposeThread(window, context, DetailsFor(context));
	if (!path.isEmpty()) {
		ShowVideoEditor(window, path, thread);
	} else if (!content.isEmpty()) {
		ShowVideoEditorContent(window, content, thread);
	}
}

void RunRoundVideo(not_null<Context*> context) {
	// The item checks the rights of the chat and closes the box.
	const auto triggered = TriggerFilled([&](
			not_null<Ui::PopupMenu*> menu) {
		FillRoundVideo(menu, context);
	});
	if (!triggered) {
		ShowOpenFailed(context);
	}
}

void RunVideoSticker(
		not_null<Context*> context,
		const Ui::PreparedFile &file,
		const FileTraits &traits) {
	const auto window = ResolveWindow(context);
	if (!window) {
		return;
	}
	const auto weak = base::make_weak(window);
	const auto base = BaseName(traits.name);
	ShowVideoStickerTrim(
		context->args.show,
		file.path,
		file.path.isEmpty() ? file.content : QByteArray(),
		[=](QByteArray webm) {
			if (const auto strong = weak.get()) {
				auto source = StickerSource::FromVideo(std::move(webm));
				source.name = base;
				AddToStickerPack(
					not_null<Window::SessionController*>(strong),
					std::move(source));
			}
		});
}

void FinishGif(
		not_null<Context*> context,
		const VideoCore::Cancel &cancel,
		const QString &sourcePath,
		std::shared_ptr<Ui::PreparedFile> made) {
	if (context->gifCancel != cancel) {
		return;
	}
	context->gifCancel = nullptr;
	const auto cancelled = cancel->load();
	if (const auto box = context->gifBox.get()) {
		box->closeBox();
	}
	context->gifBox = nullptr;
	if (cancelled) {
		return;
	}
	const auto show = context->args.show;
	const auto replace = context->args.replace;
	if (!made || !replace) {
		show->showToast(tr::lng_oblivion_attach_gif_failed(tr::now));
		return;
	} else if (context->args.canSend && !context->args.canSend(*made)) {
		// The box has told why a GIF can't be sent here.
		return;
	}
	const auto replaced = std::make_shared<bool>(false);
	replace(0, [=](Ui::PreparedFile &file) {
		// Still the same video: nothing but this row replaces it while
		// the progress is shown above the box.
		if (file.path != sourcePath
			|| file.type != Ui::PreparedFile::Type::Video) {
			return;
		}
		made->caption = std::move(file.caption);
		made->spoiler = file.spoiler;
		file = std::move(*made);
		*replaced = true;
	});
	if (*replaced) {
		show->showToast(tr::lng_oblivion_attach_gif_done(tr::now));
	}
}

// The video becomes what Telegram calls a GIF: an H.264 video without
// an audio track. It is written to a file, so the preview of the box
// plays it, and replaces the attached video there.
void RunGif(
		not_null<Row*> row,
		not_null<Context*> context,
		const Ui::PreparedFile &file,
		const FileTraits &traits) {
	if (context->gifCancel) {
		return;
	}
	const auto sourcePath = file.path;
	auto options = VideoCore::ClipOptions();
	options.path = sourcePath;
	if (sourcePath.isEmpty()) {
		options.content = file.content;
	}
	options.maxSide = GifMaxSide(file.originalDimensions, traits.duration);
	const auto target = NextTempPath(
		&context->args.peer->session(),
		GifFileName(traits.name));
	const auto previewWidth = st::sendMediaPreviewSize;
	const auto sideLimit = PhotoSideLimit();
	const auto cancel = std::make_shared<std::atomic<bool>>(false);
	const auto weak = QPointer<Row>(row.get());

	context->gifCancel = cancel;
	context->gifProgress = 0.;
	context->gifBox = context->args.show->show(Box(
		GifBox,
		rpl::producer<float64>(context->gifProgress.value()),
		Fn<void()>([=] {
			// Closed by the button, by Escape or together with the
			// other layers: the conversion stops at its next frame.
			cancel->store(true);
			if (weak && context->gifCancel == cancel) {
				context->gifCancel = nullptr;
			}
		})));

	crl::async([=] {
		const auto progress = [=](float64 value) {
			crl::on_main(weak, [=] {
				if (context->gifCancel == cancel) {
					context->gifProgress = value;
				}
			});
		};
		auto attempt = options;
		auto result = VideoCore::ClipResult();
		for (auto i = 0; i != kGifAttempts; ++i) {
			result = VideoCore::MakeGifVideo(attempt, progress, cancel);
			if (!result.ok
				|| cancel->load()
				|| (result.content.size() <= kGifMaxBytes)) {
				break;
			}

			// Too heavy to be a GIF: half the pixels, half the size.
			result = VideoCore::ClipResult();
			attempt.maxSide = std::max(attempt.maxSide * 7 / 10, kGifMinSide);
		}
		auto made = std::shared_ptr<Ui::PreparedFile>();
		if (cancel->load()) {
			// Nothing is written.
		} else if (!result.ok || result.content.isEmpty()) {
			LOG(("Oblivion AttachTools: no GIF of this video: %1"
				).arg(result.error));
		} else if (WriteBytes(target, result.content)) {
			const auto size = int64(result.content.size());
			result.content = QByteArray();

			// The same check the box makes for a file that is added.
			auto prepared = std::make_shared<Ui::PreparedFile>(target);
			prepared->size = size;
			Storage::PrepareDetails(*prepared, previewWidth, sideLimit);
			if (prepared->information && prepared->isGifv()) {
				made = std::move(prepared);
			} else {
				LOG(("Oblivion AttachTools: the result is not a GIF."));
				QFile::remove(target);
			}
		} else {
			LOG(("Oblivion AttachTools: could not write the GIF."));
		}
		crl::on_main(weak, [=, made = std::move(made)]() mutable {
			FinishGif(context, cancel, sourcePath, std::move(made));
		});
	});
}

// The music editor opens documents (and downloads them when it has to).
// A file from the disk becomes a local document that is already "loaded":
// it has only a name and the place of the file, nothing is requested.
void RunMusicEditor(
		not_null<Context*> context,
		const Ui::PreparedFile &file) {
	const auto window = ResolveWindow(context);
	if (!window) {
		return;
	}
	const auto path = file.path;
	auto &id = context->music[path];
	if (!id) {
		id = base::RandomValue<DocumentId>();
	}
	const auto document = window->session().data().document(id);
	if (document->filepath(true).isEmpty()) {
		document->setattributes({
			MTP_documentAttributeFilename(
				MTP_string(QFileInfo(path).fileName())),
		});
		document->setLocation(Core::FileLocation(path));
	}
	if (document->filepath(true).isEmpty()) {
		ShowOpenFailed(context);
		return;
	}
	ShowMusicEditor(window, document);
}

// The editor is a window of its own, the box stays as it is. "Save" in
// the editor writes to the attached file itself, as for any opened file.
void RunLottieEditor(
		not_null<Row*> row,
		not_null<Context*> context,
		const Ui::PreparedFile &file) {
	if (context->lottieReading) {
		return;
	}
	context->lottieReading = true;
	const auto path = file.path;
	const auto name = QFileInfo(path).completeBaseName();
	const auto weak = QPointer<Row>(row.get());
	crl::async([=] {
		auto bytes = ReadBytes(path, kLottieMaxBytes);
		const auto lottie = IsLottieData(bytes);
		crl::on_main(weak, [=, bytes = std::move(bytes)]() mutable {
			context->lottieReading = false;
			if (!lottie) {
				context->args.show->showToast(
					tr::lng_oblivion_attach_lottie_failed(tr::now));
				return;
			}
			LottieEdit::OpenLottieEditor({
				.data = std::move(bytes),
				.name = name,
				.path = path,
			});
		});
	});
}

void Run(not_null<Row*> row, not_null<Context*> context, Tool tool) {
	const auto file = SingleFile(context);
	if (!file) {
		return;
	}
	const auto traits = TraitsOf(*file);
	const auto kind = KindOf(traits);
	if (!ToolFitsKind(tool, kind)) {
		// The list has changed after the click.
		return;
	}

	// The tools that may close the box (and this row with it) are the
	// last thing done here.
	switch (tool) {
	case Tool::PhotoEditor:
		RunPhotoEditor(row, context, *file);
		break;
	case Tool::MakeSticker:
	case Tool::RemoveBackground:
	case Tool::CopyText:
		RunImageTool(context, tool, *file, traits);
		break;
	case Tool::VideoEditor:
		RunVideoEditor(context, kind, *file);
		break;
	case Tool::RoundVideo:
		RunRoundVideo(context);
		break;
	case Tool::VideoSticker:
		RunVideoSticker(context, *file, traits);
		break;
	case Tool::Gif:
		RunGif(row, context, *file, traits);
		break;
	case Tool::MusicEditor:
		RunMusicEditor(context, *file);
		break;
	case Tool::LottieEditor:
		RunLottieEditor(row, context, *file);
		break;
	}
}

// UI snapshots (OBLIVION_SELFTEST=ui, see oblivion_ui_snapshots.h): the
// row as it stands in the send files box. The box itself needs a session,
// so what it shows around the row is repeated here with its own styles and
// distances: the title, the preview of the file with its buttons, the
// checkbox, the caption field and the buttons of the box.

enum class SampleKind : uchar {
	Photo,
	Video,
	Audio,
	Sticker, // An animated sticker: this is how the box shows a .tgs.
};

// The texts of the box itself come with the cloud language pack, which the
// snapshots don't have: the usual Russian ones stand in for them there.
[[nodiscard]] QString BoxText(const QString &current, const char *russian) {
	return CurrentLanguageIsRussian()
		? QString::fromUtf8(russian)
		: current;
}

class Sample final : public Ui::RpWidget {
public:
	Sample(QWidget *parent, SampleKind kind, std::vector<Tool> tools);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;

private:
	void paintPicture(QPainter &p, QRect rect) const;
	void paintSticker(QPainter &p, QRect rect) const;
	void paintFile(QPainter &p, QRect rect) const;
	void paintControls(QPainter &p) const;

	const SampleKind _kind;
	const QString _title;
	const not_null<Row*> _row;
	const not_null<Ui::InputField*> _caption;
	Ui::Checkbox *_asFile = nullptr;
	Ui::IconButton *_edit = nullptr;
	Ui::IconButton *_remove = nullptr;
	QRect _preview;

};

Sample::Sample(QWidget *parent, SampleKind kind, std::vector<Tool> tools)
: RpWidget(parent)
, _kind(kind)
, _title((kind == SampleKind::Photo)
	? BoxText(tr::lng_send_image(tr::now), "Отправить изображение")
	: (kind == SampleKind::Video)
	? BoxText(tr::lng_send_video(tr::now), "Отправить видео")
	: BoxText(tr::lng_send_file(tr::now), "Отправить как файл"))
, _row(Ui::CreateChild<Row>(this))
, _caption(Ui::CreateChild<Ui::InputField>(
	this,
	st::defaultComposeFiles.caption,
	Ui::InputField::Mode::MultiLine,
	rpl::single(BoxText(tr::lng_photo_caption(tr::now), "Подпись")))) {
	const auto &files = st::defaultComposeFiles;
	if (_kind == SampleKind::Photo || _kind == SampleKind::Video) {
		_asFile = Ui::CreateChild<Ui::Checkbox>(
			this,
			BoxText(
				tr::lng_send_as_documents_one(tr::now),
				"Отправить как файл"),
			false,
			files.checkbox,
			files.check);
	} else if (_kind == SampleKind::Audio) {
		_edit = Ui::CreateChild<Ui::IconButton>(this, files.buttonFile);
		_edit->setIconOverride(&files.buttonFileEdit);
		_remove = Ui::CreateChild<Ui::IconButton>(this, files.buttonFile);
		_remove->setIconOverride(&files.buttonFileDelete);
	}
	_row->setTools(
		tools,
		(_kind == SampleKind::Photo)
			? Kind::Image
			: (_kind == SampleKind::Video)
			? Kind::Video
			: (_kind == SampleKind::Audio)
			? Kind::Audio
			: Kind::Lottie);
}

// The same order and distances as in SendFilesBox::updateControlsGeometry().
int Sample::resizeGetHeight(int newWidth) {
	const auto left = st::boxPhotoPadding.left();
	const auto width = st::sendMediaPreviewSize;
	auto top = st::boxTitleHeight;
	if (_kind == SampleKind::Audio) {
		_preview = QRect(left, top, width, st::attachPreviewLayout.thumbSize);
	} else if (_kind == SampleKind::Sticker) {
		// A sticker has no background and is as large as the box lets.
		const auto side = st::confirmMaxHeight;
		_preview = QRect((newWidth - side) / 2, top, side, side);
	} else {
		_preview = QRect(left, top, width, (width * 2) / 3);
	}
	if (_edit && _remove) {
		// Where Ui::AbstractSingleFilePreview places its two buttons.
		const auto y = top + st::sendBoxFileGroupSkipTop;
		auto right = left + st::sendBoxFileGroupSkipRight;
		_remove->moveToRight(right, y, newWidth);
		right += st::sendBoxFileGroupEditInternalSkip + _remove->width();
		_edit->moveToRight(right, y, newWidth);
	}
	top += _preview.height();

	_row->resizeToWidth(width);
	_row->moveToLeft(left, top, newWidth);
	top += _row->height();
	if (_asFile) {
		top += st::boxPhotoCompressedSkip;
		_asFile->moveToLeft(left, top, newWidth);
		top += _asFile->heightNoMargins();
	}
	top += st::boxPhotoCaptionSkip;
	_caption->resize(width, _caption->height());
	_caption->moveToLeft(left, top, newWidth);
	return top + _caption->height();
}

void Sample::paintPicture(QPainter &p, QRect rect) const {
	const auto radius = st::bubbleRadiusSmall;
	auto clip = QPainterPath();
	clip.addRoundedRect(rect, radius, radius);
	p.save();
	p.setClipPath(clip);

	auto sky = QLinearGradient(rect.topLeft(), rect.bottomLeft());
	sky.setColorAt(0., QColor(0x4E, 0x8F, 0xD9));
	sky.setColorAt(0.62, QColor(0xF4, 0xC9, 0x9B));
	sky.setColorAt(1., QColor(0xF2, 0xA0, 0x7B));
	p.fillRect(rect, sky);

	const auto w = rect.width() * 1.;
	const auto h = rect.height() * 1.;
	const auto at = [&](float64 x, float64 y) {
		return QPointF(rect.x() + x * w, rect.y() + y * h);
	};
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0xFF, 0xF1, 0xC4));
	p.drawEllipse(at(0.68, 0.46), h * 0.11, h * 0.11);

	auto mountains = QPainterPath();
	mountains.moveTo(at(0., 0.74));
	mountains.cubicTo(at(0.2, 0.5), at(0.36, 0.56), at(0.52, 0.7));
	mountains.cubicTo(at(0.68, 0.56), at(0.84, 0.52), at(1., 0.66));
	mountains.lineTo(at(1., 1.));
	mountains.lineTo(at(0., 1.));
	mountains.closeSubpath();
	p.setBrush(QColor(0x5B, 0x6C, 0x9E));
	p.drawPath(mountains);

	auto hills = QPainterPath();
	hills.moveTo(at(0., 0.9));
	hills.cubicTo(at(0.24, 0.72), at(0.5, 0.8), at(0.7, 0.9));
	hills.cubicTo(at(0.82, 0.84), at(0.92, 0.82), at(1., 0.86));
	hills.lineTo(at(1., 1.));
	hills.lineTo(at(0., 1.));
	hills.closeSubpath();
	p.setBrush(QColor(0x2F, 0x3A, 0x5F));
	p.drawPath(hills);

	if (_kind == SampleKind::Video) {
		// The play button, as Ui::AbstractSingleMediaPreview draws it.
		const auto side = st::msgFileLayout.thumbSize;
		const auto inner = QRect(
			rect.x() + (rect.width() - side) / 2,
			rect.y() + (rect.height() - side) / 2,
			side,
			side);
		p.setPen(Qt::NoPen);
		p.setBrush(st::msgDateImgBg);
		p.drawEllipse(inner);
		st::historyFileInPlay.paintInCenter(p, inner);
	}
	p.restore();
}

// A cat with the white outline stickers usually have.
void Sample::paintSticker(QPainter &p, QRect rect) const {
	const auto side = rect.width() * 1.;
	const auto at = [&](float64 x, float64 y) {
		return QPointF(rect.x() + x * side, rect.y() + y * side);
	};
	const auto fur = QColor(0xF7, 0xA9, 0x28);
	const auto dark = QColor(0x4A, 0x2C, 0x12);
	const auto rose = QColor(0xE8, 0x6A, 0x5C);
	const auto pen = [&](QColor color, float64 width) {
		return QPen(
			color,
			side * width,
			Qt::SolidLine,
			Qt::RoundCap,
			Qt::RoundJoin);
	};

	auto ears = QPainterPath();
	ears.moveTo(at(0.2, 0.36));
	ears.lineTo(at(0.2, 0.1));
	ears.lineTo(at(0.42, 0.2));
	ears.closeSubpath();
	ears.moveTo(at(0.8, 0.36));
	ears.lineTo(at(0.8, 0.1));
	ears.lineTo(at(0.58, 0.2));
	ears.closeSubpath();
	auto face = QRadialGradient(at(0.4, 0.38), side * 0.6);
	face.setColorAt(0., QColor(0xFF, 0xE2, 0x7A));
	face.setColorAt(1., fur);

	p.setPen(pen(Qt::white, 0.045));
	p.setBrush(fur);
	p.drawPath(ears);
	p.setBrush(face);
	p.drawEllipse(at(0.5, 0.52), side * 0.4, side * 0.36);
	p.setPen(Qt::NoPen);
	p.setBrush(fur);
	p.drawPath(ears);

	p.setBrush(dark);
	p.drawEllipse(at(0.35, 0.47), side * 0.035, side * 0.05);
	p.drawEllipse(at(0.65, 0.47), side * 0.035, side * 0.05);

	auto nose = QPainterPath();
	nose.moveTo(at(0.47, 0.585));
	nose.lineTo(at(0.53, 0.585));
	nose.lineTo(at(0.5, 0.625));
	nose.closeSubpath();
	p.setPen(pen(rose, 0.012));
	p.setBrush(rose);
	p.drawPath(nose);

	auto mouth = QPainterPath();
	mouth.moveTo(at(0.41, 0.66));
	mouth.cubicTo(at(0.44, 0.71), at(0.49, 0.7), at(0.5, 0.64));
	mouth.cubicTo(at(0.51, 0.7), at(0.56, 0.71), at(0.59, 0.66));
	p.setPen(pen(dark, 0.022));
	p.setBrush(Qt::NoBrush);
	p.drawPath(mouth);

	auto whiskers = QPainterPath();
	whiskers.moveTo(at(0.27, 0.6));
	whiskers.lineTo(at(0.12, 0.56));
	whiskers.moveTo(at(0.27, 0.6));
	whiskers.lineTo(at(0.13, 0.65));
	whiskers.moveTo(at(0.73, 0.6));
	whiskers.lineTo(at(0.88, 0.56));
	whiskers.moveTo(at(0.73, 0.6));
	whiskers.lineTo(at(0.87, 0.65));
	p.setPen(pen(anim::with_alpha(dark, 0.55), 0.014));
	p.drawPath(whiskers);
}

// A music file, as Ui::AbstractSingleFilePreview draws it.
void Sample::paintFile(QPainter &p, QRect rect) const {
	const auto &files = st::defaultComposeFiles;
	const auto &layout = st::attachPreviewLayout;
	const auto circle = QRect(
		rect.x(),
		rect.y(),
		layout.thumbSize,
		layout.thumbSize);
	p.setPen(Qt::NoPen);
	p.setBrush(files.iconBg);
	p.drawEllipse(circle);
	files.iconPlay.paintInCenter(p, circle);

	const auto left = circle.x() + layout.thumbSize + layout.thumbSkip;
	p.setFont(st::semiboldFont);
	p.setPen(files.nameFg);
	p.drawText(
		left,
		rect.y() + layout.nameTop + st::semiboldFont->ascent,
		u"Night Drive.mp3"_q);
	p.setFont(st::normalFont);
	p.setPen(files.statusFg);
	p.drawText(
		left,
		rect.y() + layout.statusTop + st::normalFont->ascent,
		u"8.6 MB"_q);
}

// The "more" and "remove" buttons over a media preview, where
// Ui::AbstractSingleMediaPreview places them.
void Sample::paintControls(QPainter &p) const {
	auto controls = Ui::AttachControls();
	const auto right = st::boxPhotoPadding.right()
		+ st::sendBoxAlbumGroupSkipRight;
	controls.paint(
		p,
		width() - right - controls.width(),
		_preview.y() + st::sendBoxAlbumGroupSkipTop);
}

void Sample::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);

	// As SendFilesBox::paintEvent() draws the title.
	const auto &font = st::boxTitleFont;
	p.setFont(font);
	p.setPen(st::boxTitleFg);
	p.drawText(
		st::boxPhotoTitlePosition.x(),
		st::boxTitlePosition.y() - st::boxTopMargin + font->ascent,
		_title);

	switch (_kind) {
	case SampleKind::Photo:
	case SampleKind::Video:
		paintPicture(p, _preview);
		paintControls(p);
		break;
	case SampleKind::Sticker:
		paintSticker(p, _preview);
		paintControls(p);
		break;
	case SampleKind::Audio:
		paintFile(p, _preview);
		break;
	}
}

// The frame of the send files box around the sample: its width, no title
// of the layer (the box draws its own one) and its three buttons.
void SampleBox(
		not_null<Ui::GenericBox*> box,
		SampleKind kind,
		std::vector<Tool> tools) {
	box->setWidth(st::boxWideWidth);
	box->addRow(
		object_ptr<Sample>(box, kind, std::move(tools)),
		style::margins());
	box->addButton(tr::lng_send_button(), [] {});
	box->addButton(tr::lng_cancel(), [] {});
	box->addLeftButton(
		rpl::single(
			BoxText(tr::lng_stickers_featured_add(tr::now), "Добавить")),
		[] {});
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;
	using Type = Ui::PreparedFile::Type;

	const auto scene = [](
			const QString &name,
			SampleKind kind,
			FileTraits traits,
			Features features) {
		RegisterBoxScene(
			name,
			QSize(st::boxWideWidth + Scaled(80), 0),
			[=](std::shared_ptr<Ui::Show> show) {
				return Box(SampleBox, kind, ToolsFor(traits, features));
			});
	};

	auto photo = FileTraits();
	photo.type = Type::Photo;
	photo.content = Content::Image;
	photo.hasPixels = true;
	photo.hasPath = true;
	photo.name = u"IMG_2048.jpg"_q;
	photo.mime = u"image/jpeg"_q;

	auto video = FileTraits();
	video.type = Type::Video;
	video.content = Content::Video;
	video.duration = crl::time(24'000);
	video.hasPath = true;
	video.name = u"clip.mp4"_q;
	video.mime = u"video/mp4"_q;

	auto audio = FileTraits();
	audio.type = Type::Music;
	audio.content = Content::Song;
	audio.hasPath = true;
	audio.name = u"Night Drive.mp3"_q;
	audio.mime = u"audio/mpeg"_q;

	auto lottie = FileTraits();
	lottie.type = Type::None;
	lottie.content = Content::Image;
	lottie.hasPixels = true;
	lottie.animated = true;
	lottie.hasPath = true;
	lottie.name = u"Dancing Cat.tgs"_q;
	lottie.mime = u"application/x-tgsticker"_q;

	// Everything a Mac offers, in a chat.
	auto full = Features();
	full.photoEditor = true;
	full.videoEditor = true;
	full.roundVideo = true;
	full.textRecognition = true;
	full.backgroundRemoval = true;

	// macOS before 14: the text is recognized, the background stays.
	auto older = full;
	older.backgroundRemoval = false;

	// Windows: no on-device image analysis.
	auto basic = full;
	basic.textRecognition = false;
	basic.backgroundRemoval = false;

	// Not from the compose area of a chat (a scheduled message, a quick
	// reply): no round video message.
	auto apart = full;
	apart.roundVideo = false;

	// Four buttons in a grid, three and two in a line as wide as the row.
	scene(u"attach_tools_photo"_q, SampleKind::Photo, photo, full);
	scene(u"attach_tools_photo_three"_q, SampleKind::Photo, photo, older);
	scene(u"attach_tools_photo_basic"_q, SampleKind::Photo, photo, basic);

	// Four buttons in a tight line, three in cells of the same width.
	scene(u"attach_tools_video"_q, SampleKind::Video, video, full);
	scene(u"attach_tools_video_three"_q, SampleKind::Video, video, apart);

	// A single button keeps its own width.
	scene(u"attach_tools_audio"_q, SampleKind::Audio, audio, full);
	scene(u"attach_tools_tgs"_q, SampleKind::Sticker, lottie, full);

	// The conversion of the "GIF" tool, in the middle of it.
	RegisterBoxScene(
		u"attach_tools_gif_box"_q,
		QSize(st::boxWideWidth + Scaled(80), 0),
		[](std::shared_ptr<Ui::Show> show) {
			return Box(
				GifBox,
				rpl::producer<float64>(rpl::single(0.42)),
				Fn<void()>());
		});
});

} // namespace

bool HoldsOnlyFile(const Ui::PreparedList &list, bool reading) {
	return !reading
		&& (list.files.size() == 1)
		&& list.filesToProcess.empty();
}

bool RunSelfTest(QStringList &log) {
	using Type = Ui::PreparedFile::Type;

	auto passed = true;
	const auto check = [&](bool ok, const QString &what) {
		log.push_back((ok ? u"ok: "_q : u"FAILED: "_q) + what);
		passed = passed && ok;
	};

	// The tools that take the file away and close the box, in the row and
	// in the "..." menu of the box.
	{
		auto list = Ui::PreparedList();
		check(!HoldsOnlyFile(list, false), u"an empty box has no only file"_q);
		check(
			!HoldsOnlyFile(list, true),
			u"the first file is still being read: nothing to take yet"_q);
		list.files.emplace_back(u"clip.mp4"_q);
		check(HoldsOnlyFile(list, false), u"one file may be taken away"_q);
		check(
			!HoldsOnlyFile(list, true),
			u"one file while an added one is being read is not taken away"_q);
		list.filesToProcess.emplace_back(u"photo.jpg"_q);
		check(
			!HoldsOnlyFile(list, false),
			u"one file while an added one waits to be read is not taken away"_q);
		list.filesToProcess.clear();
		list.files.emplace_back(u"second.mp4"_q);
		check(!HoldsOnlyFile(list, false), u"an album is not taken away"_q);
	}

	// What SendFilesBox asks its stream of caption requests before such a
	// tool closes it: with no listener the caption is not marked as taken
	// and the box restores the text from before it.
	{
		auto requests = rpl::event_stream<TextWithTags>();
		check(
			!requests.has_consumers(),
			u"a caption nobody may take is left to the box"_q);
		auto offered = requests.events();
		check(
			!requests.has_consumers(),
			u"a caption that is only offered is left to the box"_q);
		auto taken = QString();
		auto lifetime = rpl::lifetime();
		std::move(offered) | rpl::on_next([&](const TextWithTags &text) {
			taken = text.text;
		}, lifetime);
		const auto listened = requests.has_consumers();
		requests.fire_copy(TextWithTags{ u"caption"_q });
		check(
			listened && (taken == u"caption"_q),
			u"a caption the message field listens for is handed over"_q);
	}
	const auto toolName = [](Tool tool) {
		switch (tool) {
		case Tool::PhotoEditor: return u"photo-editor"_q;
		case Tool::MakeSticker: return u"sticker"_q;
		case Tool::RemoveBackground: return u"cutout"_q;
		case Tool::CopyText: return u"text"_q;
		case Tool::VideoEditor: return u"video-editor"_q;
		case Tool::RoundVideo: return u"round"_q;
		case Tool::VideoSticker: return u"video-sticker"_q;
		case Tool::Gif: return u"gif"_q;
		case Tool::MusicEditor: return u"music-editor"_q;
		case Tool::LottieEditor: return u"lottie-editor"_q;
		}
		return u"?"_q;
	};
	const auto names = [&](const std::vector<Tool> &tools) {
		auto list = QStringList();
		for (const auto tool : tools) {
			list.push_back(toolName(tool));
		}
		return list.join(u", "_q);
	};

	// Sample files, as Storage::PrepareDetails() describes them.
	const auto image = [](Type type, const QString &name, const QString &mime) {
		auto result = FileTraits();
		result.type = type;
		result.content = Content::Image;
		result.hasPixels = true;
		result.hasPath = true;
		result.size = 2 * 1024 * 1024;
		result.name = name;
		result.mime = mime;
		return result;
	};
	const auto video = [](crl::time duration, bool silent) {
		auto result = FileTraits();
		result.type = Type::Video;
		result.content = Content::Video;
		result.silent = silent;
		result.duration = duration;
		result.hasPath = true;
		result.size = 12 * 1024 * 1024;
		result.name = u"clip.mp4"_q;
		result.mime = u"video/mp4"_q;
		return result;
	};
	const auto plain = [](const QString &name, const QString &mime, int64 size) {
		auto result = FileTraits();
		result.type = Type::File;
		result.hasPath = true;
		result.size = size;
		result.name = name;
		result.mime = mime;
		return result;
	};
	const auto photo = image(Type::Photo, u"IMG_2048.JPG"_q, u"image/jpeg"_q);
	const auto png = image(Type::File, u"scan.png"_q, u"image/png"_q);
	auto edited = image(Type::Photo, u"IMG_2048.png"_q, u"image/png"_q);
	edited.hasPath = false;
	edited.size = 0;
	auto gif = image(Type::None, u"cat.GIF"_q, u"image/gif"_q);
	gif.animated = true;
	auto animatedWebp = image(Type::None, u"cat.webp"_q, u"image/webp"_q);
	animatedWebp.animated = true;
	auto tgs = image(Type::None, u"cat.TGS"_q, u"application/x-tgsticker"_q);
	tgs.animated = true;
	tgs.size = 24 * 1024;
	auto broken = image(Type::Photo, u"broken.jpg"_q, u"image/jpeg"_q);
	broken.hasPixels = false;
	const auto clip = video(crl::time(24'000), false);
	const auto silent = video(crl::time(8'000), true);
	const auto film = video(crl::time(5'400'000), false);
	const auto unknown = video(crl::time(-1), false);
	auto webm = video(crl::time(2'900), true);
	webm.webmSticker = true;
	webm.name = u"sticker.webm"_q;
	auto oddVideo = video(crl::time(24'000), false);
	oddVideo.type = Type::File;
	auto pasted = video(crl::time(24'000), false);
	pasted.hasPath = false;
	pasted.hasBytes = true;
	pasted.name = QString();
	auto song = plain(u"Night Drive.mp3"_q, u"audio/mpeg"_q, 8 * 1024 * 1024);
	song.type = Type::Music;
	song.content = Content::Song;
	auto songBytes = song;
	songBytes.hasPath = false;
	songBytes.hasBytes = true;
	const auto brokenTgs = plain(
		u"cat.tgs"_q,
		u"application/gzip"_q,
		24 * 1024);
	const auto json = plain(u"data.json"_q, u"application/json"_q, 90 * 1024);
	const auto hugeJson = plain(
		u"dump.json"_q,
		u"application/json"_q,
		kLottieMaxBytes + 1);
	const auto pdf = plain(u"report.pdf"_q, u"application/pdf"_q, 300 * 1024);

	check(KindOf(photo) == Kind::Image, u"a photo is an image"_q);
	check(KindOf(png) == Kind::Image, u"an image sent as a file is an image"_q);
	check(
		KindOf(edited) == Kind::Image,
		u"an image edited in the box (no path) is still an image"_q);
	check(KindOf(gif) == Kind::Gif, u"an animated .gif is a GIF"_q);
	check(
		KindOf(animatedWebp) == Kind::None,
		u"another animated image gets no tools"_q);
	check(
		KindOf(tgs) == Kind::Lottie,
		u"a .tgs is Lottie, not the animated image the box sees"_q);
	check(
		KindOf(brokenTgs) == Kind::Lottie,
		u"a .tgs without a thumbnail is offered to the Lottie editor"_q);
	check(KindOf(json) == Kind::Json, u"a .json has to be read first"_q);
	check(KindOf(hugeJson) == Kind::None, u"a huge .json is not read"_q);
	check(KindOf(broken) == Kind::None, u"an image without pixels"_q);
	check(KindOf(clip) == Kind::Video, u"a video"_q);
	check(KindOf(silent) == Kind::Video, u"a video without sound"_q);
	check(KindOf(pasted) == Kind::Video, u"a video kept in memory"_q);
	check(
		KindOf(oddVideo) == Kind::None,
		u"a video the box can only send as a file"_q);
	check(KindOf(song) == Kind::Audio, u"a music file"_q);
	check(
		KindOf(songBytes) == Kind::None,
		u"the music editor needs a file on the disk"_q);
	check(KindOf(pdf) == Kind::None, u"a document gets no tools"_q);

	auto mac = Features();
	mac.photoEditor = true;
	mac.videoEditor = true;
	mac.roundVideo = true;
	mac.textRecognition = true;
	mac.backgroundRemoval = true;
	auto windows = mac;
	windows.textRecognition = false;
	windows.backgroundRemoval = false;
	auto oldMac = mac;
	oldMac.backgroundRemoval = false;
	auto notChat = mac;
	notChat.roundVideo = false;
	auto busyEditor = mac;
	busyEditor.videoEditor = false;
	auto noGifs = mac;
	noGifs.gifAllowed = false;
	auto checked = mac;
	checked.lottieJson = true;

	using Tools = std::vector<Tool>;
	const auto expect = [&](
			const QString &what,
			const FileTraits &traits,
			const Features &features,
			const Tools &expected) {
		const auto got = ToolsFor(traits, features);
		check(
			got == expected,
			what + u": "_q + (got.empty() ? u"nothing"_q : names(got)));
	};
	expect(u"photo, macOS 14+"_q, photo, mac, {
		Tool::PhotoEditor,
		Tool::MakeSticker,
		Tool::RemoveBackground,
		Tool::CopyText,
	});
	expect(u"photo, older macOS"_q, photo, oldMac, {
		Tool::PhotoEditor,
		Tool::MakeSticker,
		Tool::CopyText,
	});
	expect(u"photo, Windows"_q, photo, windows, {
		Tool::PhotoEditor,
		Tool::MakeSticker,
	});
	expect(u"image file, Windows"_q, png, windows, {
		Tool::PhotoEditor,
		Tool::MakeSticker,
	});
	expect(u"video in a chat"_q, clip, mac, {
		Tool::VideoEditor,
		Tool::RoundVideo,
		Tool::VideoSticker,
		Tool::Gif,
	});
	expect(u"video, the box doesn't know its chat"_q, clip, notChat, {
		Tool::VideoEditor,
		Tool::VideoSticker,
		Tool::Gif,
	});
	expect(u"video, the editor is busy"_q, clip, busyEditor, {
		Tool::RoundVideo,
		Tool::VideoSticker,
		Tool::Gif,
	});
	expect(u"video, the chat doesn't allow GIFs"_q, clip, noGifs, {
		Tool::VideoEditor,
		Tool::RoundVideo,
		Tool::VideoSticker,
	});
	expect(u"video without sound (already a GIF)"_q, silent, mac, {
		Tool::VideoEditor,
		Tool::RoundVideo,
		Tool::VideoSticker,
	});
	expect(u"long video (no GIF)"_q, film, mac, {
		Tool::VideoEditor,
		Tool::RoundVideo,
		Tool::VideoSticker,
	});
	expect(u"video of unknown duration (no GIF)"_q, unknown, mac, {
		Tool::VideoEditor,
		Tool::RoundVideo,
		Tool::VideoSticker,
	});
	expect(u"video sticker file"_q, webm, mac, {
		Tool::VideoEditor,
		Tool::RoundVideo,
	});
	expect(u"GIF file"_q, gif, mac, {
		Tool::VideoEditor,
		Tool::VideoSticker,
	});
	expect(u"music"_q, song, windows, { Tool::MusicEditor });
	expect(u".tgs"_q, tgs, windows, { Tool::LottieEditor });
	expect(u".json before it is read"_q, json, mac, {});
	expect(u".json with a Lottie animation"_q, json, checked, {
		Tool::LottieEditor,
	});
	expect(u"document"_q, pdf, checked, {});
	expect(u"animated WebP"_q, animatedWebp, mac, {});

	// Every tool a file gets works with the kind of that file, and no
	// button is shown twice.
	{
		auto consistent = true;
		const auto files = {
			photo, png, edited, gif, animatedWebp, tgs, broken, clip,
			silent, film, unknown, webm, oddVideo, pasted, song,
			songBytes, brokenTgs, json, hugeJson, pdf,
		};
		const auto sets = {
			mac, windows, oldMac, notChat, busyEditor, noGifs, checked,
		};
		for (const auto &file : files) {
			for (const auto &features : sets) {
				const auto tools = ToolsFor(file, features);
				for (const auto tool : tools) {
					if (!ToolFitsKind(tool, KindOf(file))
						|| ranges::count(tools, tool) != 1) {
						consistent = false;
					}
				}
			}
		}
		check(consistent, u"tools match the kind of the file, no repeats"_q);
	}

	// Which buttons warn in their tooltips that the send box will close.
	{
		auto closing = QStringList();
		const auto kinds = {
			Kind::None, Kind::Image, Kind::Video, Kind::Gif, Kind::Audio,
			Kind::Lottie, Kind::Json,
		};
		const auto tools = {
			Tool::PhotoEditor, Tool::MakeSticker, Tool::RemoveBackground,
			Tool::CopyText, Tool::VideoEditor, Tool::RoundVideo,
			Tool::VideoSticker, Tool::Gif, Tool::MusicEditor,
			Tool::LottieEditor,
		};
		for (const auto kind : kinds) {
			for (const auto tool : tools) {
				if (ToolClosesBox(tool, kind)) {
					closing.push_back(u"%1/%2"_q
						.arg(int(kind))
						.arg(toolName(tool)));
				}
			}
		}
		const auto got = closing.join(u", "_q);
		check(
			got == u"%1/video-editor, %1/round"_q.arg(int(Kind::Video)),
			u"only the editor and the round video take a video away: "_q
				+ got);
		check(
			!ToolClosesBox(Tool::VideoEditor, Kind::Gif),
			u"the video editor leaves the box of a GIF file open"_q);
	}

	check(BaseName(u"IMG 1234.HEIC"_q) == u"IMG 1234"_q, u"base name"_q);
	check(
		GifFileName(u"clip.final.mov"_q) == u"clip.final.mp4"_q,
		u"GIF name keeps the name of the video"_q);
	check(GifFileName(QString()) == u"video.mp4"_q, u"GIF name, no name"_q);
	check(
		GifFileName(u"what?*.mov"_q) == u"what__.mp4"_q,
		u"GIF name has no characters a file name can't have"_q);

	// The frame of a GIF: as large as its duration lets the file be.
	const auto side = [&](
			const QString &what,
			QSize source,
			crl::time duration,
			int expected) {
		const auto got = GifMaxSide(source, duration);
		check(
			got == expected,
			u"GIF side, %1: %2"_q.arg(what).arg(got));
	};
	const auto full = VideoCore::kGifVideoMaxSide;
	side(u"a minute of 1080p"_q, QSize(1920, 1080), 60'000, 664);
	side(u"a minute of a vertical video"_q, QSize(1080, 1920), 60'000, 664);
	side(u"a minute, the size is unknown"_q, QSize(), 60'000, 664);
	side(u"20 seconds"_q, QSize(640, 360), 20'000, 1151);
	side(u"45 seconds of a square video"_q, QSize(1000, 1000), 45'000, 575);
	side(u"5 seconds"_q, QSize(1920, 1080), 5'000, full);
	side(u"the duration is unknown"_q, QSize(1280, 720), -1, full);
	side(u"never smaller than this"_q, QSize(1000, 1000), 600'000, kGifMinSide);
	{
		// The file is expected to fit: the frame with this longer side,
		// 30 frames a second, kMp4BitsPerPixel bits for a pixel.
		auto fits = true;
		const auto sizes = { QSize(1920, 1080), QSize(720, 1280), QSize(800, 600) };
		for (const auto &size : sizes) {
			for (auto seconds = 10; seconds <= 60; seconds += 5) {
				const auto longer = GifMaxSide(size, seconds * crl::time(1000));
				const auto ratio = std::min(size.width(), size.height())
					/ float64(std::max(size.width(), size.height()));
				const auto bytes = longer
					* (longer * ratio)
					* VideoCore::kGifVideoMaxFps
					* kMp4BitsPerPixel
					* seconds
					/ 8.;
				if (longer < kGifMinSide
					|| longer > full
					|| (longer > kGifMinSide && bytes > kGifTargetBytes)) {
					fits = false;
				}
			}
		}
		check(fits, u"GIF sides keep the expected file size in the limit"_q);
	}

	// Wrapping of the buttons.
	const auto places = [](const ChipsLayout &layout) {
		auto list = QStringList();
		for (const auto &place : layout.places) {
			list.push_back(u"%1:%2+%3"_q
				.arg(place.line)
				.arg(place.left)
				.arg(place.width));
		}
		return list.join(' ');
	};
	const auto wrap = [&](
			const QString &what,
			const std::vector<int> &widths,
			int available,
			int skip,
			bool grid,
			const QString &expected) {
		const auto layout = PlaceChips(widths, available, skip);
		const auto got = places(layout);
		const auto lines = layout.places.empty()
			? 0
			: (layout.places.back().line + 1);
		check(
			(got == expected)
				&& (layout.lines == lines)
				&& (layout.grid == grid),
			what + u": "_q + (got.isEmpty() ? u"nothing"_q : got));
	};
	wrap(u"no buttons"_q, {}, 308, 6, false, QString());
	wrap(u"one button"_q, { 120 }, 308, 6, false, u"0:0+120"_q);
	wrap(
		u"three buttons in a line keep their widths"_q,
		{ 100, 86, 68 },
		308,
		6,
		false,
		u"0:0+100 0:106+86 0:198+68"_q);
	wrap(
		u"four buttons fit exactly"_q,
		{ 70, 70, 70, 80 },
		308,
		6,
		false,
		u"0:0+70 0:76+70 0:152+70 0:228+80"_q);
	wrap(
		u"four buttons become a grid of two and two"_q,
		{ 97, 84, 95, 73 },
		308,
		6,
		true,
		u"0:0+151 0:157+151 1:0+151 1:157+151"_q);
	wrap(
		u"five buttons become three and two"_q,
		{ 90, 90, 90, 90, 90 },
		308,
		6,
		true,
		u"0:0+98 0:104+98 0:208+100 1:0+98 1:104+98"_q);
	wrap(
		u"three long buttons: two, then one"_q,
		{ 150, 150, 120 },
		308,
		6,
		true,
		u"0:0+151 0:157+151 1:0+151"_q);
	wrap(
		u"two buttons that don't fit together"_q,
		{ 200, 180 },
		308,
		6,
		true,
		u"0:0+308 1:0+308"_q);
	wrap(
		u"no grid when a button is wider than its cell"_q,
		{ 200, 100, 250 },
		308,
		6,
		false,
		u"0:0+200 0:206+100 1:0+250"_q);
	wrap(
		u"a button as wide as the row"_q,
		{ 308, 60, 60 },
		308,
		6,
		false,
		u"0:0+308 1:0+60 1:66+60"_q);
	wrap(
		u"a button wider than the row is cut"_q,
		{ 400, 60 },
		308,
		6,
		false,
		u"0:0+308 1:0+60"_q);
	wrap(u"no width"_q, { 60, 60 }, 0, 6, false, QString());

	// A line of several buttons is stretched to the width of the row.
	const auto stretch = [&](
			const QString &what,
			const std::vector<int> &widths,
			int available,
			int skip,
			bool stretched,
			const QString &expected) {
		auto layout = PlaceChips(widths, available, skip);
		const auto done = StretchLine(layout, available, skip);
		const auto got = places(layout);
		check(
			(got == expected) && (done == stretched),
			what + u": "_q + (got.isEmpty() ? u"nothing"_q : got));
	};
	stretch(u"one button is not stretched"_q, { 120 }, 308, 6, false, u"0:0+120"_q);
	stretch(
		u"two buttons share the row"_q,
		{ 97, 83 },
		308,
		6,
		true,
		u"0:0+151 0:157+151"_q);
	stretch(
		u"three buttons take equal cells"_q,
		{ 97, 83, 72 },
		308,
		6,
		true,
		u"0:0+98 0:104+98 0:208+100"_q);
	stretch(
		u"a tight line shares the spare width"_q,
		{ 87, 76, 73, 50 },
		308,
		4,
		true,
		u"0:0+90 0:94+79 0:177+75 0:256+52"_q);
	stretch(
		u"a line as wide as the row stays"_q,
		{ 70, 70, 70, 80 },
		308,
		6,
		false,
		u"0:0+70 0:76+70 0:152+70 0:228+80"_q);
	stretch(
		u"a grid is not stretched"_q,
		{ 97, 84, 95, 73 },
		308,
		6,
		false,
		u"0:0+151 0:157+151 1:0+151 1:157+151"_q);
	{
		// Whatever the widths are: the order is kept, nothing leaves the
		// row or overlaps, no button is narrower than it has to be and no
		// layout with fewer lines exists.
		auto sound = true;
		auto grids = 0;
		auto seed = uint32(20261006);
		const auto next = [&](int from, int till) {
			seed = seed * 1664525u + 1013904223u;
			return from + int((seed >> 8) % uint32(till - from + 1));
		};
		for (auto round = 0; round != 400; ++round) {
			const auto available = next(120, 420);
			const auto skip = next(0, 12);
			auto widths = std::vector<int>(next(1, 9));
			for (auto &width : widths) {
				width = next(20, available);
			}
			const auto layout = PlaceChips(widths, available, skip);
			if (layout.places.size() != widths.size()
				|| layout.lines != CountLines(widths, available, skip)
				|| (layout.grid && layout.lines < 2)) {
				sound = false;
				break;
			}
			grids += layout.grid ? 1 : 0;
			auto line = 0;
			auto right = -skip;
			for (auto i = 0; i != int(widths.size()); ++i) {
				const auto &place = layout.places[i];
				if (place.line != line) {
					if (place.line != line + 1 || place.left != 0) {
						sound = false;
					}
					line = place.line;
					right = -skip;
				}
				const auto exact = (place.left == right + skip)
					&& (place.width == widths[i]);
				const auto cell = (place.left >= right + skip)
					&& (place.width >= widths[i]);
				if (!(layout.grid ? cell : exact)
					|| place.left + place.width > available) {
					sound = false;
				}
				right = place.left + place.width;
			}
			if (line + 1 != layout.lines) {
				sound = false;
			}
		}
		check(
			sound && (grids > 0),
			u"400 random rows are wrapped correctly, %1 as grids"_q.arg(grids));
	}
	return passed;
}

} // namespace AttachTools

object_ptr<Ui::RpWidget> CreateAttachToolsRow(
		not_null<QWidget*> box,
		AttachToolsArgs &&args) {
	using namespace AttachTools;

	if (!Get().attachTools() || !args.show || !args.list) {
		return { nullptr };
	}
	CleanupTempOnce(&args.peer->session());

	auto result = object_ptr<Row>(box.get());
	const auto row = result.data();
	auto changes = std::move(args.listChanges);
	const auto context = row->lifetime().make_state<Context>(Context{
		.args = std::move(args),
	});
	row->lifetime().add([=] {
		// The box is gone: nobody waits for the GIF anymore.
		if (const auto cancel = base::take(context->gifCancel)) {
			cancel->store(true);
		}
	});
	row->setClicked([=](Tool tool) {
		Run(row, context, tool);
	});
	if (changes) {
		std::move(
			changes
		) | rpl::on_next([=] {
			Refresh(row, context);
		}, row->lifetime());
	}
	Refresh(row, context);
	return object_ptr<Ui::RpWidget>(std::move(result));
}

} // namespace Oblivion
