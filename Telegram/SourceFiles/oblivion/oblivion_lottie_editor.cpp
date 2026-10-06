/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_lottie_editor.h"

#include "core/application.h"
#include "core/file_location.h"
#include "core/file_utilities.h"
#include "core/shortcuts.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_session.h"
#include "data/data_star_gift.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "oblivion/oblivion_lang.h"
#include "oblivion/oblivion_lottie.h"
#include "oblivion/oblivion_lottie_editor_canvas.h"
#include "oblivion/oblivion_lottie_editor_inspector.h"
#include "oblivion/oblivion_lottie_editor_layers.h"
#include "oblivion/oblivion_lottie_editor_timeline.h"
#include "oblivion/oblivion_sticker_packs.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "platform/platform_file_utilities.h"
#include "settings.h"
#include "storage/file_download.h"
#include "ui/effects/animation_value.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/layer_manager.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "styles/style_iv.h"
#include "styles/style_layers.h"
#include "styles/style_media_player.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMutex>
#include <QtCore/QSaveFile>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QKeySequence>
#include <QtGui/QScreen>

#include <cmath>

namespace Oblivion::LottieEdit {
namespace {

constexpr auto kToolbarHeight = 48;
constexpr auto kToolbarPadding = 8;
constexpr auto kToolbarSkip = 6;
constexpr auto kToolbarGroupSkip = 14;
constexpr auto kToolbarNameRoom = 130;
constexpr auto kSplitterGrab = 7;
constexpr auto kLayersDefault = 250;
constexpr auto kLayersMin = 170;
constexpr auto kLayersMax = 520;
constexpr auto kInspectorDefault = 300;
constexpr auto kInspectorMin = 230;
constexpr auto kInspectorMax = 560;
constexpr auto kTimelineDefault = 230;
constexpr auto kTimelineMin = 110;
constexpr auto kCanvasMinWidth = 240;
constexpr auto kCanvasMinHeight = 160;
constexpr auto kWindowDefaultWidth = 1280;
constexpr auto kWindowDefaultHeight = 820;
constexpr auto kWindowMinWidth = 900;
constexpr auto kWindowMinHeight = 560;
constexpr auto kWindowCascade = 26;
constexpr auto kValidationDelay = crl::time(300);
constexpr auto kZoomStep = 1.25;
constexpr auto kTooltipDelay = 800;
constexpr auto kMaxExportSide = 4096;
constexpr auto kNudgeLarge = 10.;

// The letters on the V and P keys of the Russian layout: the tool
// shortcuts work without switching the keyboard.
constexpr auto kCyrillicOnV = 0x041C;
constexpr auto kCyrillicOnP = 0x0417;

// Panel sizes and the last window geometry, kept for the app session.
struct SessionLayout {
	int layers = 0;
	int inspector = 0;
	int timeline = 0;
	std::optional<QRect> geometry;
};

[[nodiscard]] SessionLayout &Layout() {
	static auto result = SessionLayout();
	return result;
}

[[nodiscard]] std::vector<std::unique_ptr<EditorWindow>> &Windows() {
	static auto result = std::vector<std::unique_ptr<EditorWindow>>();
	return result;
}

struct LockState {
	bool locked = false;
	std::vector<QPointer<EditorWindow>> hidden;
};

[[nodiscard]] LockState &WindowsLock() {
	static auto result = LockState();
	return result;
}

[[nodiscard]] int Scaled(int value) {
	return style::ConvertScale(value);
}

[[nodiscard]] QString LowerFirst(QString text) {
	if (!text.isEmpty()) {
		text[0] = text[0].toLower();
	}
	return text;
}

[[nodiscard]] QString FileFilter(const QString &name, const QString &mask) {
	return name + u" ("_q + mask + u")"_q;
}

[[nodiscard]] QString OpenFilter() {
	return FileFilter(
			tr::lng_oblivion_lottie_editor_filter_lottie(tr::now),
			u"*.tgs *.json"_q)
		+ u";;"_q
		+ FileFilter(
			tr::lng_oblivion_lottie_editor_filter_all(tr::now),
			u"*"_q);
}

[[nodiscard]] QString TgsFilter() {
	return FileFilter(tr::lng_oblivion_file_tgs(tr::now), u"*.tgs"_q);
}

[[nodiscard]] QString JsonFilter() {
	return FileFilter(tr::lng_oblivion_file_lottie(tr::now), u"*.json"_q);
}

[[nodiscard]] bool IsJsonPath(const QString &path) {
	return path.endsWith(u".json"_q, Qt::CaseInsensitive);
}

[[nodiscard]] QString WithExtension(QString path, const QString &extension) {
	const auto suffix = QFileInfo(path).suffix();
	if (suffix.compare(extension, Qt::CaseInsensitive) != 0) {
		path += '.' + extension;
	}
	return path;
}

[[nodiscard]] QString SuggestedPath(const QString &fileName) {
	if (cDialogLastPath().isEmpty()) {
		Platform::FileDialog::InitLastPath();
	}
	return filedialogNextFilename(fileName, QString());
}

[[nodiscard]] QString SafeFileName(QString name) {
	static const auto kForbidden = QString(u"\\/:*?\"<>|"_q);
	for (auto &ch : name) {
		if (kForbidden.contains(ch) || ch.unicode() < 32) {
			ch = '_';
		}
	}
	name = name.trimmed();
	return name.isEmpty()
		? tr::lng_oblivion_lottie_editor_new_name(tr::now)
		: name;
}

[[nodiscard]] bool SamePath(const QString &a, const QString &b) {
	if (a.isEmpty() || b.isEmpty()) {
		return false;
	}
	const auto left = QFileInfo(a).absoluteFilePath();
	const auto right = QFileInfo(b).absoluteFilePath();
	return !left.compare(right, Qt::CaseInsensitive);
}

struct OpenedFile {
	QByteArray bytes;
	QString name;
	QString path;
	bool failed = false;
};

[[nodiscard]] OpenedFile ReadOpenResult(const FileDialog::OpenResult &result) {
	if (!result.remoteContent.isEmpty()) {
		return { .bytes = result.remoteContent };
	} else if (result.paths.isEmpty()) {
		return {};
	}
	const auto path = result.paths.front();
	auto file = QFile(path);
	auto bytes = file.open(QIODevice::ReadOnly)
		? file.readAll()
		: QByteArray();
	const auto failed = bytes.isEmpty();
	return {
		.bytes = std::move(bytes),
		.name = QFileInfo(path).completeBaseName(),
		.path = path,
		.failed = failed,
	};
}

[[nodiscard]] QWidget *ActiveAppWindow() {
	if (!Core::IsAppLaunched()) {
		return nullptr;
	}
	for (const auto &window : Windows()) {
		if (window->isActiveWindow()) {
			return window.get();
		}
	}
	if (const auto window = Core::App().activeWindow()) {
		return window->widget().get();
	} else if (const auto primary = Core::App().activePrimaryWindow()) {
		return primary->widget().get();
	}
	return nullptr;
}

void ShowAppToast(const QString &text) {
	LOG(("Oblivion LottieEdit: %1").arg(text));
	if (!Core::IsAppLaunched()) {
		return;
	}
	for (const auto &window : Windows()) {
		if (window->isActiveWindow()) {
			window->uiShow()->showToast(text);
			return;
		}
	}
	if (const auto window = Core::App().activeWindow()) {
		window->uiShow()->showToast(text);
	} else if (const auto primary = Core::App().activePrimaryWindow()) {
		primary->uiShow()->showToast(text);
	}
}

void ActivateWindow(not_null<EditorWindow*> window) {
	if (window->isMinimized()) {
		window->showNormal();
	} else if (window->isHidden()) {
		window->show();
	}
	window->raise();
	window->activateWindow();
}

void RemoveWindow(not_null<EditorWindow*> window) {
	auto &list = Windows();
	const auto i = ranges::find(
		list,
		window.get(),
		&std::unique_ptr<EditorWindow>::get);
	if (i != end(list)) {
		auto taken = std::move(*i);
		list.erase(i);
		taken = nullptr;
	}
}

[[nodiscard]] QRect DefaultWindowGeometry() {
	auto size = QSize(Scaled(kWindowDefaultWidth), Scaled(kWindowDefaultHeight));
	const auto screen = QGuiApplication::primaryScreen();
	const auto available = screen ? screen->availableGeometry() : QRect();
	const auto fits = [&](const QRect &rect) {
		if (!rect.isValid()) {
			return false;
		}
		for (const auto screen : QGuiApplication::screens()) {
			if (screen->availableGeometry().intersects(rect)) {
				return true;
			}
		}
		return false;
	};
	const auto &saved = Layout().geometry;
	auto result = QRect();
	if (saved && fits(*saved)) {
		result = *saved;
	} else if (!available.isEmpty()) {
		size = size.boundedTo(QSize(
			available.width() * 94 / 100,
			available.height() * 92 / 100));
		result = QRect(QPoint(), size);
		result.moveCenter(available.center());
	} else {
		result = QRect(QPoint(), size);
	}
	const auto shift = Scaled(kWindowCascade) * int(Windows().size());
	result.translate(shift, shift);
	if (!available.isEmpty() && !available.contains(result)) {
		result.moveTopLeft(QPoint(
			std::clamp(
				result.x(),
				available.x(),
				std::max(available.x(), available.right() - result.width())),
			std::clamp(
				result.y(),
				available.y(),
				std::max(
					available.y(),
					available.bottom() - result.height()))));
	}
	return result;
}

[[nodiscard]] Document SampleDocument(QString *name) {
	auto path = u":/animations/cake.tgs"_q;
	if (!QFile::exists(path)) {
		auto iterator = QDirIterator(
			u":/animations"_q,
			{ u"*.tgs"_q },
			QDir::Files,
			QDirIterator::Subdirectories);
		path = iterator.hasNext() ? iterator.next() : QString();
	}
	auto file = QFile(path);
	const auto bytes = file.open(QIODevice::ReadOnly)
		? file.readAll()
		: QByteArray();
	auto result = Document::FromData(bytes);
	if (name) {
		*name = QFileInfo(path).completeBaseName();
	}
	return result.valid() ? result : Document::Blank();
}

} // namespace

// Localized texts.

QString NodeKindText(NodeKind kind) {
	switch (kind) {
	case NodeKind::Composition:
		return tr::lng_oblivion_lottie_node_composition(tr::now);
	case NodeKind::Asset: return tr::lng_oblivion_lottie_node_asset(tr::now);
	case NodeKind::Layer: return tr::lng_oblivion_lottie_node_layer(tr::now);
	case NodeKind::Shape: return tr::lng_oblivion_lottie_node_shape(tr::now);
	case NodeKind::Mask: return tr::lng_oblivion_lottie_node_mask(tr::now);
	case NodeKind::Effect: return tr::lng_oblivion_lottie_node_effect(tr::now);
	}
	return tr::lng_oblivion_lottie_node_layer(tr::now);
}

QString LayerTypeText(LayerType type) {
	switch (type) {
	case LayerType::Precomp:
		return tr::lng_oblivion_lottie_layer_precomp(tr::now);
	case LayerType::Solid: return tr::lng_oblivion_lottie_layer_solid(tr::now);
	case LayerType::Image: return tr::lng_oblivion_lottie_layer_image(tr::now);
	case LayerType::Null: return tr::lng_oblivion_lottie_layer_null(tr::now);
	case LayerType::Shape: return tr::lng_oblivion_lottie_layer_shape(tr::now);
	case LayerType::Text: return tr::lng_oblivion_lottie_layer_text(tr::now);
	case LayerType::Unknown: break;
	}
	return tr::lng_oblivion_lottie_node_layer(tr::now);
}

QString ShapeTypeText(ShapeType type) {
	switch (type) {
	case ShapeType::Group: return tr::lng_oblivion_lottie_shape_group(tr::now);
	case ShapeType::Rectangle:
		return tr::lng_oblivion_lottie_shape_rectangle(tr::now);
	case ShapeType::Ellipse:
		return tr::lng_oblivion_lottie_shape_ellipse(tr::now);
	case ShapeType::Star: return tr::lng_oblivion_lottie_shape_star(tr::now);
	case ShapeType::Path: return tr::lng_oblivion_lottie_shape_path(tr::now);
	case ShapeType::Fill: return tr::lng_oblivion_lottie_shape_fill(tr::now);
	case ShapeType::Stroke:
		return tr::lng_oblivion_lottie_shape_stroke(tr::now);
	case ShapeType::GradientFill:
		return tr::lng_oblivion_lottie_shape_gradient_fill(tr::now);
	case ShapeType::GradientStroke:
		return tr::lng_oblivion_lottie_shape_gradient_stroke(tr::now);
	case ShapeType::Transform:
		return tr::lng_oblivion_lottie_shape_transform(tr::now);
	case ShapeType::TrimPaths:
		return tr::lng_oblivion_lottie_shape_trim(tr::now);
	case ShapeType::Repeater:
		return tr::lng_oblivion_lottie_shape_repeater(tr::now);
	case ShapeType::MergePaths:
		return tr::lng_oblivion_lottie_shape_merge(tr::now);
	case ShapeType::RoundCorners:
		return tr::lng_oblivion_lottie_shape_round(tr::now);
	case ShapeType::OffsetPath:
		return tr::lng_oblivion_lottie_shape_offset(tr::now);
	case ShapeType::PuckerBloat:
		return tr::lng_oblivion_lottie_shape_pucker(tr::now);
	case ShapeType::Twist: return tr::lng_oblivion_lottie_shape_twist(tr::now);
	case ShapeType::ZigZag:
		return tr::lng_oblivion_lottie_shape_zigzag(tr::now);
	case ShapeType::Unknown: break;
	}
	return tr::lng_oblivion_lottie_node_shape(tr::now);
}

QString NodeTypeText(const NodeInfo &node) {
	switch (node.kind) {
	case NodeKind::Layer: return LayerTypeText(node.layerType);
	case NodeKind::Shape: return ShapeTypeText(node.shapeType);
	default: return NodeKindText(node.kind);
	}
}

QString NodeDisplayName(const Document &document, NodeId id) {
	const auto node = document.node(id);
	if (!node) {
		return QString();
	} else if (!node->name.trimmed().isEmpty()) {
		return node->name;
	} else if (node->kind == NodeKind::Composition) {
		return NodeKindText(node->kind);
	}
	return NodeTypeText(*node) + ' ' + QString::number(node->index + 1);
}

QString PropertyRoleText(PropertyRole role) {
	switch (role) {
	case PropertyRole::Anchor:
		return tr::lng_oblivion_lottie_prop_anchor(tr::now);
	case PropertyRole::Position:
		return tr::lng_oblivion_lottie_prop_position(tr::now);
	case PropertyRole::PositionX:
		return tr::lng_oblivion_lottie_prop_position_x(tr::now);
	case PropertyRole::PositionY:
		return tr::lng_oblivion_lottie_prop_position_y(tr::now);
	case PropertyRole::PositionZ:
		return tr::lng_oblivion_lottie_prop_position_z(tr::now);
	case PropertyRole::Scale:
		return tr::lng_oblivion_lottie_prop_scale(tr::now);
	case PropertyRole::Rotation:
		return tr::lng_oblivion_lottie_prop_rotation(tr::now);
	case PropertyRole::RotationX:
		return tr::lng_oblivion_lottie_prop_rotation_x(tr::now);
	case PropertyRole::RotationY:
		return tr::lng_oblivion_lottie_prop_rotation_y(tr::now);
	case PropertyRole::Opacity:
		return tr::lng_oblivion_lottie_prop_opacity(tr::now);
	case PropertyRole::Skew: return tr::lng_oblivion_lottie_prop_skew(tr::now);
	case PropertyRole::SkewAxis:
		return tr::lng_oblivion_lottie_prop_skew_axis(tr::now);
	case PropertyRole::Color:
		return tr::lng_oblivion_lottie_prop_color(tr::now);
	case PropertyRole::StrokeWidth:
		return tr::lng_oblivion_lottie_prop_stroke_width(tr::now);
	case PropertyRole::Size: return tr::lng_oblivion_lottie_prop_size(tr::now);
	case PropertyRole::Roundness:
		return tr::lng_oblivion_lottie_prop_roundness(tr::now);
	case PropertyRole::StartPoint:
		return tr::lng_oblivion_lottie_prop_start_point(tr::now);
	case PropertyRole::EndPoint:
		return tr::lng_oblivion_lottie_prop_end_point(tr::now);
	case PropertyRole::Gradient:
		return tr::lng_oblivion_lottie_prop_gradient(tr::now);
	case PropertyRole::HighlightLength:
		return tr::lng_oblivion_lottie_prop_highlight_length(tr::now);
	case PropertyRole::HighlightAngle:
		return tr::lng_oblivion_lottie_prop_highlight_angle(tr::now);
	case PropertyRole::Path: return tr::lng_oblivion_lottie_prop_path(tr::now);
	case PropertyRole::TrimStart:
		return tr::lng_oblivion_lottie_prop_trim_start(tr::now);
	case PropertyRole::TrimEnd:
		return tr::lng_oblivion_lottie_prop_trim_end(tr::now);
	case PropertyRole::TrimOffset:
		return tr::lng_oblivion_lottie_prop_trim_offset(tr::now);
	case PropertyRole::Points:
		return tr::lng_oblivion_lottie_prop_points(tr::now);
	case PropertyRole::InnerRadius:
		return tr::lng_oblivion_lottie_prop_inner_radius(tr::now);
	case PropertyRole::OuterRadius:
		return tr::lng_oblivion_lottie_prop_outer_radius(tr::now);
	case PropertyRole::InnerRoundness:
		return tr::lng_oblivion_lottie_prop_inner_roundness(tr::now);
	case PropertyRole::OuterRoundness:
		return tr::lng_oblivion_lottie_prop_outer_roundness(tr::now);
	case PropertyRole::Copies:
		return tr::lng_oblivion_lottie_prop_copies(tr::now);
	case PropertyRole::Offset:
		return tr::lng_oblivion_lottie_prop_offset(tr::now);
	case PropertyRole::StartOpacity:
		return tr::lng_oblivion_lottie_prop_start_opacity(tr::now);
	case PropertyRole::EndOpacity:
		return tr::lng_oblivion_lottie_prop_end_opacity(tr::now);
	case PropertyRole::MaskPath:
		return tr::lng_oblivion_lottie_prop_mask_path(tr::now);
	case PropertyRole::MaskOpacity:
		return tr::lng_oblivion_lottie_prop_mask_opacity(tr::now);
	case PropertyRole::MaskExpansion:
		return tr::lng_oblivion_lottie_prop_mask_expansion(tr::now);
	case PropertyRole::TimeRemap:
		return tr::lng_oblivion_lottie_prop_time_remap(tr::now);
	case PropertyRole::EffectValue:
		return tr::lng_oblivion_lottie_prop_value(tr::now);
	case PropertyRole::Dash: return tr::lng_oblivion_lottie_prop_dash(tr::now);
	case PropertyRole::Radius:
		return tr::lng_oblivion_lottie_prop_radius(tr::now);
	case PropertyRole::Amount:
		return tr::lng_oblivion_lottie_prop_amount(tr::now);
	case PropertyRole::MaskFeather:
		return tr::lng_oblivion_lottie_mask_prop_feather(tr::now);
	case PropertyRole::Other: break;
	}
	return tr::lng_oblivion_lottie_prop_other(tr::now);
}

QString PropertyText(const PropertyInfo &info) {
	if (info.role == PropertyRole::EffectValue) {
		return info.name.trimmed().isEmpty()
			? PropertyRoleText(info.role)
			: info.name.trimmed();
	} else if (info.role == PropertyRole::Dash) {
		// Dash items: "n" is "d" (dash), "g" (gap) or "o" (offset), the
		// exporters put "dash" / "gap" / "offset" into "nm". The last
		// item is the offset for the renderer whatever its name says.
		const auto name = info.name.trimmed().toLower();
		if (info.dashOffset) {
			return tr::lng_oblivion_lottie_prop_dash_offset(tr::now);
		} else if (name == u"g"_q || name.startsWith(u"gap"_q)) {
			return tr::lng_oblivion_lottie_prop_gap(tr::now);
		} else if (name == u"o"_q || name.startsWith(u"offset"_q)) {
			return tr::lng_oblivion_lottie_prop_dash_offset(tr::now);
		}
		return tr::lng_oblivion_lottie_prop_dash(tr::now);
	}
	return PropertyRoleText(info.role);
}

QString EasingPresetText(EasingPreset preset) {
	switch (preset) {
	case EasingPreset::Linear:
		return tr::lng_oblivion_lottie_easing_linear(tr::now);
	case EasingPreset::EaseIn:
		return tr::lng_oblivion_lottie_easing_in(tr::now);
	case EasingPreset::EaseOut:
		return tr::lng_oblivion_lottie_easing_out(tr::now);
	case EasingPreset::EaseInOut:
		return tr::lng_oblivion_lottie_easing_in_out(tr::now);
	case EasingPreset::Hold:
		return tr::lng_oblivion_lottie_easing_hold(tr::now);
	case EasingPreset::Custom: break;
	}
	return tr::lng_oblivion_lottie_easing_custom(tr::now);
}

QString ColorKindText(ColorKind kind) {
	switch (kind) {
	case ColorKind::Fill: return ShapeTypeText(ShapeType::Fill);
	case ColorKind::Stroke: return ShapeTypeText(ShapeType::Stroke);
	case ColorKind::GradientFill:
		return ShapeTypeText(ShapeType::GradientFill);
	case ColorKind::GradientStroke:
		return ShapeTypeText(ShapeType::GradientStroke);
	case ColorKind::Solid: return tr::lng_oblivion_lottie_color_solid(tr::now);
	case ColorKind::Effect:
		return tr::lng_oblivion_lottie_color_effect(tr::now);
	case ColorKind::TextFill:
		return tr::lng_oblivion_lottie_color_text_fill(tr::now);
	case ColorKind::TextStroke:
		return tr::lng_oblivion_lottie_color_text_stroke(tr::now);
	}
	return tr::lng_oblivion_lottie_prop_color(tr::now);
}

QString MatteModeText(MatteMode mode) {
	switch (mode) {
	case MatteMode::None: return tr::lng_oblivion_lottie_matte_none(tr::now);
	case MatteMode::Alpha: return tr::lng_oblivion_lottie_matte_alpha(tr::now);
	case MatteMode::AlphaInverted:
		return tr::lng_oblivion_lottie_matte_alpha_inverted(tr::now);
	case MatteMode::Luma: return tr::lng_oblivion_lottie_matte_luma(tr::now);
	case MatteMode::LumaInverted:
		return tr::lng_oblivion_lottie_matte_luma_inverted(tr::now);
	}
	return tr::lng_oblivion_lottie_matte_none(tr::now);
}

QString CommandText(Command command) {
	switch (command) {
	case Command::Unknown: break;
	case Command::SetValue: return tr::lng_oblivion_lottie_cmd_value(tr::now);
	case Command::ToggleAnimated:
		return tr::lng_oblivion_lottie_cmd_animated(tr::now);
	case Command::AddKeyframe:
		return tr::lng_oblivion_lottie_cmd_add_keyframe(tr::now);
	case Command::ChangeKeyframe:
		return tr::lng_oblivion_lottie_cmd_keyframe(tr::now);
	case Command::SetEasing:
		return tr::lng_oblivion_lottie_cmd_easing(tr::now);
	case Command::RemoveKeyframes:
		return tr::lng_oblivion_lottie_cmd_remove_keyframes(tr::now);
	case Command::MoveKeyframes:
		return tr::lng_oblivion_lottie_cmd_move_keyframes(tr::now);
	case Command::Rename: return tr::lng_oblivion_lottie_cmd_rename(tr::now);
	case Command::SetHidden:
		return tr::lng_oblivion_lottie_cmd_hidden(tr::now);
	case Command::Delete: return tr::lng_oblivion_lottie_cmd_delete(tr::now);
	case Command::Duplicate:
		return tr::lng_oblivion_lottie_cmd_duplicate(tr::now);
	case Command::Move: return tr::lng_oblivion_lottie_cmd_move(tr::now);
	case Command::AddLayer:
		return tr::lng_oblivion_lottie_cmd_add_layer(tr::now);
	case Command::AddShape:
		return tr::lng_oblivion_lottie_cmd_add_shape(tr::now);
	case Command::LayerTiming:
		return tr::lng_oblivion_lottie_cmd_timing(tr::now);
	case Command::ReplaceColor:
		return tr::lng_oblivion_lottie_cmd_replace_color(tr::now);
	case Command::Recolor:
		return tr::lng_oblivion_lottie_cmd_recolor(tr::now);
	case Command::CanvasSize:
		return tr::lng_oblivion_lottie_cmd_canvas(tr::now);
	case Command::FrameRate: return tr::lng_oblivion_lottie_cmd_fps(tr::now);
	case Command::Duration:
		return tr::lng_oblivion_lottie_cmd_duration(tr::now);
	case Command::Speed: return tr::lng_oblivion_lottie_cmd_speed(tr::now);
	case Command::Trim: return tr::lng_oblivion_lottie_cmd_trim(tr::now);
	case Command::AutoFix:
		return tr::lng_oblivion_lottie_cmd_autofix(tr::now);
	case Command::Optimize:
		return tr::lng_oblivion_lottie_cmd_optimize(tr::now);
	case Command::AddMask:
		return tr::lng_oblivion_lottie_mask_cmd_add_mask(tr::now);
	case Command::ChangeMask:
		return tr::lng_oblivion_lottie_mask_cmd_change_mask(tr::now);
	case Command::TrackMatte:
		return tr::lng_oblivion_lottie_mask_cmd_matte(tr::now);
	case Command::Parent:
		return tr::lng_oblivion_lottie_mask_cmd_parent(tr::now);
	case Command::ShapeOption:
		return tr::lng_oblivion_lottie_mask_cmd_option(tr::now);
	case Command::Gradient:
		return tr::lng_oblivion_lottie_mask_cmd_gradient(tr::now);
	case Command::ConvertPaint:
		return tr::lng_oblivion_lottie_mask_cmd_convert_paint(tr::now);
	case Command::Dashes:
		return tr::lng_oblivion_lottie_mask_cmd_dashes(tr::now);
	case Command::BakeCorners:
		return tr::lng_oblivion_lottie_mask_cmd_bake(tr::now);
	case Command::AddPath:
		return tr::lng_oblivion_lottie_mask_cmd_add_path(tr::now);
	case Command::EditPath:
		return tr::lng_oblivion_lottie_mask_cmd_edit_path(tr::now);
	}
	return tr::lng_oblivion_lottie_cmd_change(tr::now);
}

QString UndoText(Command command) {
	return (command == Command::Unknown)
		? tr::lng_oblivion_lottie_editor_undo(tr::now)
		: tr::lng_oblivion_lottie_editor_undo_action(
			tr::now,
			lt_action,
			LowerFirst(CommandText(command)));
}

QString RedoText(Command command) {
	return (command == Command::Unknown)
		? tr::lng_oblivion_lottie_editor_redo(tr::now)
		: tr::lng_oblivion_lottie_editor_redo_action(
			tr::now,
			lt_action,
			LowerFirst(CommandText(command)));
}

QString IssueText(const Document &document, const Issue &issue) {
	switch (issue.type) {
	case IssueType::InvalidComposition:
		return tr::lng_oblivion_lottie_issue_invalid(tr::now);
	case IssueType::MissingVersion:
		return tr::lng_oblivion_lottie_issue_version(tr::now);
	case IssueType::CanvasSize:
		return tr::lng_oblivion_lottie_issue_canvas(
			tr::now,
			lt_size,
			FormatCanvasSize(document.size()));
	case IssueType::FrameRate:
		return tr::lng_oblivion_lottie_issue_fps(
			tr::now,
			lt_value,
			FormatDecimal(issue.value));
	case IssueType::Duration:
		return tr::lng_oblivion_lottie_issue_duration(
			tr::now,
			lt_value,
			FormatDecimal(issue.value));
	case IssueType::FileSize:
		return tr::lng_oblivion_lottie_issue_size(
			tr::now,
			lt_value,
			FormatDecimal(issue.value / 1024., 1));
	case IssueType::Images:
		return tr::lng_oblivion_lottie_issue_images(tr::now);
	case IssueType::Expressions:
		return tr::lng_oblivion_lottie_issue_expressions(tr::now);
	case IssueType::Layers3D:
		return tr::lng_oblivion_lottie_issue_3d(tr::now);
	case IssueType::TextLayers:
		return tr::lng_oblivion_lottie_issue_text(tr::now);
	case IssueType::BrokenKeyframes:
		return tr::lng_oblivion_lottie_issue_keyframes(tr::now);
	case IssueType::MissingTgsMarker:
		return tr::lng_oblivion_lottie_issue_marker(tr::now);
	case IssueType::Masks:
		// Listed under "Telegram does not accept in stickers" since the
		// issues are grouped: the older "they slow the sticker down" text
		// would argue with that title.
		return tr::lng_oblivion_lottie_mask_issue_masks(tr::now);
	case IssueType::Effects:
		return tr::lng_oblivion_lottie_issue_effects(tr::now);
	case IssueType::Solids:
		return tr::lng_oblivion_lottie_issue_solids(tr::now);
	case IssueType::TimeStretch:
		return tr::lng_oblivion_lottie_issue_stretch(tr::now);
	case IssueType::TimeRemap:
		return tr::lng_oblivion_lottie_issue_remap(tr::now);
	case IssueType::MergePaths:
		return tr::lng_oblivion_lottie_issue_merge(tr::now);
	case IssueType::UnsupportedShapes:
		return tr::lng_oblivion_lottie_issue_modifiers(tr::now);
	case IssueType::Repeaters:
		return tr::lng_oblivion_lottie_issue_repeaters(tr::now);
	case IssueType::StarShapes:
		return tr::lng_oblivion_lottie_issue_stars(tr::now);
	case IssueType::GradientStrokes:
		return tr::lng_oblivion_lottie_issue_gradient_strokes(tr::now);
	case IssueType::OutOfCanvas:
		return tr::lng_oblivion_lottie_issue_edge(tr::now);
	case IssueType::AutoOrient:
		return tr::lng_oblivion_lottie_issue_auto_orient(tr::now);
	case IssueType::TrackMattes:
		return tr::lng_oblivion_lottie_mask_issue_mattes(tr::now);
	case IssueType::BrokenMattes:
		return tr::lng_oblivion_lottie_mask_issue_broken_mattes(tr::now);
	case IssueType::MatteLinks:
		return tr::lng_oblivion_lottie_mask_issue_matte_links(tr::now);
	case IssueType::MasksOff:
		return tr::lng_oblivion_lottie_mask_issue_masks_off(tr::now);
	case IssueType::MaskModes:
		return tr::lng_oblivion_lottie_mask_issue_modes(tr::now);
	case IssueType::MaskOptions:
		return tr::lng_oblivion_lottie_mask_issue_options(tr::now);
	case IssueType::MaskInverted:
		return tr::lng_oblivion_lottie_mask_issue_inverted(tr::now);
	case IssueType::PathVertices:
		return tr::lng_oblivion_lottie_mask_issue_vertices(tr::now);
	case IssueType::KeyOrder:
		return tr::lng_oblivion_lottie_mask_issue_key_order(tr::now);
	case IssueType::ParentLinks:
		return tr::lng_oblivion_lottie_mask_issue_parents(tr::now);
	case IssueType::RendererHang:
		return tr::lng_oblivion_lottie_mask_issue_hang(tr::now);
	}
	return tr::lng_oblivion_lottie_issue_invalid(tr::now);
}

QString IssueCategoryText(IssueCategory category) {
	switch (category) {
	case IssueCategory::File:
		return tr::lng_oblivion_lottie_mask_category_file(tr::now);
	case IssueCategory::Forbidden:
		return tr::lng_oblivion_lottie_mask_category_forbidden(tr::now);
	case IssueCategory::NotRendered:
		return tr::lng_oblivion_lottie_mask_category_not_rendered(tr::now);
	case IssueCategory::Advice:
		return tr::lng_oblivion_lottie_mask_category_advice(tr::now);
	}
	return QString();
}

QString IssueFixText(IssueType type) {
	switch (type) {
	case IssueType::MissingVersion:
		return tr::lng_oblivion_lottie_fix_version(tr::now);
	case IssueType::CanvasSize:
		return tr::lng_oblivion_lottie_fix_canvas(tr::now);
	case IssueType::FrameRate:
		return tr::lng_oblivion_lottie_fix_fps(tr::now);
	case IssueType::Duration:
		return tr::lng_oblivion_lottie_fix_duration(tr::now);
	case IssueType::FileSize:
		return tr::lng_oblivion_lottie_fix_size(tr::now);
	case IssueType::Images:
	case IssueType::Expressions:
	case IssueType::TextLayers:
	case IssueType::Effects:
	case IssueType::MergePaths:
	case IssueType::UnsupportedShapes:
		return tr::lng_oblivion_lottie_fix_remove(tr::now);
	case IssueType::Layers3D:
		return tr::lng_oblivion_lottie_fix_3d(tr::now);
	case IssueType::BrokenKeyframes:
		return tr::lng_oblivion_lottie_fix_keyframes(tr::now);
	case IssueType::MissingTgsMarker:
		return tr::lng_oblivion_lottie_fix_marker(tr::now);
	case IssueType::Solids:
		return tr::lng_oblivion_lottie_fix_solids(tr::now);
	case IssueType::AutoOrient:
		return tr::lng_oblivion_lottie_fix_auto_orient(tr::now);
	case IssueType::MasksOff:
		return tr::lng_oblivion_lottie_mask_fix_masks_on(tr::now);
	case IssueType::MaskModes:
		return tr::lng_oblivion_lottie_mask_fix_modes(tr::now);
	case IssueType::MaskOptions:
		return tr::lng_oblivion_lottie_mask_fix_options(tr::now);
	case IssueType::MaskInverted:
		return tr::lng_oblivion_lottie_mask_fix_inverted(tr::now);
	case IssueType::KeyOrder:
		return tr::lng_oblivion_lottie_mask_fix_key_order(tr::now);
	case IssueType::ParentLinks:
		return tr::lng_oblivion_lottie_mask_fix_parents(tr::now);
	case IssueType::RendererHang:
		return tr::lng_oblivion_lottie_mask_fix_hang(tr::now);
	case IssueType::InvalidComposition:
	case IssueType::Masks:
	case IssueType::TimeStretch:
	case IssueType::TimeRemap:
	case IssueType::Repeaters:
	case IssueType::StarShapes:
	case IssueType::GradientStrokes:
	case IssueType::OutOfCanvas:
	case IssueType::TrackMattes:
	case IssueType::BrokenMattes:
	case IssueType::MatteLinks:
	case IssueType::PathVertices:
		break;
	}
	return QString();
}

QString FormatDecimal(double value, int decimals) {
	if (!std::isfinite(value)) {
		return u"0"_q;
	}
	auto result = QString::number(value, 'f', std::clamp(decimals, 0, 6));
	if (result.contains('.')) {
		while (result.endsWith('0')) {
			result.chop(1);
		}
		if (result.endsWith('.')) {
			result.chop(1);
		}
	}
	if (result == u"-0"_q) {
		result = u"0"_q;
	}
	if (CurrentLanguageIsRussian()) {
		result.replace('.', ',');
	}
	return result;
}

QString FormatFps(double fps) {
	return tr::lng_oblivion_lottie_editor_fps(
		tr::now,
		lt_value,
		FormatDecimal(fps));
}

QString FormatSeconds(double seconds) {
	return tr::lng_oblivion_lottie_editor_seconds(
		tr::now,
		lt_value,
		FormatDecimal(seconds));
}

QString FormatKilobytes(int64 bytes) {
	return tr::lng_oblivion_lottie_editor_kilobytes(
		tr::now,
		lt_value,
		FormatDecimal(bytes / 1024., 1));
}

QString FormatCanvasSize(QSize size) {
	return QString::number(size.width())
		+ QChar(0x00D7)
		+ QString::number(size.height());
}

QString DocumentInfoText(const Document &document) {
	const auto fps = document.frameRate();
	return tr::lng_oblivion_lottie_editor_info(
		tr::now,
		lt_size,
		FormatCanvasSize(document.size()),
		lt_fps,
		FormatFps(fps),
		lt_duration,
		FormatSeconds((fps > 0.) ? (document.frames() / fps) : 0.));
}

QString WithShortcut(const QString &action, const QKeySequence &keys) {
	const auto text = keys.toString(QKeySequence::NativeText);
	return text.isEmpty()
		? action
		: tr::lng_oblivion_lottie_editor_shortcut(
			tr::now,
			lt_action,
			action,
			lt_keys,
			text);
}

// FrameRenderer.

struct FrameRenderer::Shared {
	struct Job {
		Document document;
		int frame = 0;
		QSize size;
		Fn<void(QImage)> done;
		uint64 generation = 0;
	};

	QMutex mutex;
	std::optional<Job> pending; // Guarded by mutex.
	bool running = false; // Guarded by mutex.

	// Main thread only.
	bool alive = true;
	uint64 generation = 0;

	// The worker only (at most one runs at a time).
	Document rendered;
	std::unique_ptr<Oblivion::Lottie::Renderer> renderer;
};

void FrameRenderer::RenderJobs(const std::shared_ptr<Shared> &shared) {
	while (true) {
		auto job = Shared::Job();
		{
			QMutexLocker lock(&shared->mutex);
			if (!shared->pending) {
				shared->running = false;
				return;
			}
			job = std::move(*shared->pending);
			shared->pending = std::nullopt;
		}
		if (!shared->renderer || !job.document.sameAs(shared->rendered)) {
			shared->renderer = nullptr;
			shared->rendered = job.document;
			const auto json = job.document.valid()
				? job.document.toJson()
				: QByteArray();
			if (!json.isEmpty()) {
				shared->renderer = std::make_unique<Oblivion::Lottie::Renderer>(
					json);
			}
		}
		auto image = (shared->renderer
			&& shared->renderer->valid()
			&& !job.size.isEmpty())
			? shared->renderer->render(job.frame, job.size)
			: QImage();
		crl::on_main([
				shared,
				done = std::move(job.done),
				generation = job.generation,
				image = std::move(image)]() mutable {
			if (shared->alive && shared->generation == generation && done) {
				done(std::move(image));
			}
		});
	}
}

FrameRenderer::FrameRenderer()
: _shared(std::make_shared<Shared>()) {
}

FrameRenderer::~FrameRenderer() {
	_shared->alive = false;
	cancel();
}

void FrameRenderer::request(
		const Document &document,
		int frame,
		QSize size,
		Fn<void(QImage image)> done) {
	auto start = false;
	{
		QMutexLocker lock(&_shared->mutex);
		_shared->pending = Shared::Job{
			.document = document,
			.frame = std::max(frame, 0),
			.size = size,
			.done = std::move(done),
			.generation = _shared->generation,
		};
		if (!_shared->running) {
			_shared->running = start = true;
		}
	}
	if (start) {
		crl::async([shared = _shared] {
			RenderJobs(shared);
		});
	}
}

void FrameRenderer::cancel() {
	++_shared->generation;
	QMutexLocker lock(&_shared->mutex);
	_shared->pending = std::nullopt;
}

// ToolButton.

ToolButton::ToolButton(
	QWidget *parent,
	const style::icon &icon,
	rpl::producer<QString> tooltip)
: IconButton(parent, st::ivEditorToolbarButton) {
	setIconOverride(&icon, &icon);
	std::move(tooltip) | rpl::on_next([=](QString text) {
		setTooltip(std::move(text));
	}, lifetime());
	style::PaletteChanged() | rpl::on_next([=] {
		refreshLook();
	}, lifetime());
}

void ToolButton::setTooltip(QString text) {
	_tooltip = std::move(text);
}

void ToolButton::setAvailable(bool available) {
	if (_available == available) {
		return;
	}
	_available = available;
	setDisabled(!available);
	refreshLook();
}

void ToolButton::refreshLook() {
	setIconColorOverride(_available
		? std::nullopt
		: std::make_optional(anim::with_alpha(st::windowBoldFg->c, 0.3)));
	update();
}

QString ToolButton::tooltipText() const {
	return _tooltip;
}

QPoint ToolButton::tooltipPos() const {
	return QCursor::pos();
}

bool ToolButton::tooltipWindowActive() const {
	return Ui::AppInFocus() && Ui::InFocusChain(window());
}

void ToolButton::enterEventHook(QEnterEvent *e) {
	if (!_tooltip.isEmpty()) {
		Ui::Tooltip::Show(kTooltipDelay, this);
	}
	IconButton::enterEventHook(e);
}

void ToolButton::leaveEventHook(QEvent *e) {
	Ui::Tooltip::Hide();
	IconButton::leaveEventHook(e);
}

// Glyphs.

void PaintGlyph(QPainter &p, Glyph glyph, const QRectF &rect, QColor color) {
	auto hq = PainterHighQualityEnabler(p);
	const auto u = std::min(rect.width(), rect.height()) / 24.;
	const auto c = rect.center();
	const auto at = [&](double x, double y) {
		return c + QPointF(x * u, y * u);
	};
	const auto box = [&](double x1, double y1, double x2, double y2) {
		return QRectF(at(x1, y1), at(x2, y2));
	};
	const auto stroke = [&](double width) {
		auto pen = QPen(color);
		pen.setWidthF(width * u);
		pen.setCapStyle(Qt::RoundCap);
		pen.setJoinStyle(Qt::RoundJoin);
		return pen;
	};
	// Filled polygons get a thin round-joined outline of the same color,
	// so their corners are softened like the app icons.
	const auto triangle = [&](QPointF a, QPointF b, QPointF d) {
		p.setPen(stroke(1.2));
		p.setBrush(color);
		p.drawPolygon(QPolygonF({ a, b, d }));
	};
	const auto bar = [&](double x1, double y1, double x2, double y2) {
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawRoundedRect(box(x1, y1, x2, y2), u, u);
	};
	const auto square = [&] {
		return box(-6.5, -6.5, 6.5, 6.5);
	};
	p.save();
	switch (glyph) {
	case Glyph::First:
		bar(-7, -5.5, -5, 5.5);
		triangle(at(-3.5, 0), at(1, -5), at(1, 5));
		triangle(at(1.5, 0), at(6, -5), at(6, 5));
		break;
	case Glyph::Previous:
		triangle(at(-5.5, 0), at(2.5, -5.5), at(2.5, 5.5));
		bar(4, -5.5, 6, 5.5);
		break;
	case Glyph::Play:
		triangle(at(-4, -6), at(6.5, 0), at(-4, 6));
		break;
	case Glyph::Pause:
		bar(-5, -6, -1.5, 6);
		bar(1.5, -6, 5, 6);
		break;
	case Glyph::Next:
		bar(-6, -5.5, -4, 5.5);
		triangle(at(5.5, 0), at(-2.5, -5.5), at(-2.5, 5.5));
		break;
	case Glyph::Last:
		triangle(at(-1, 0), at(-6, -5), at(-6, 5));
		triangle(at(3.5, 0), at(-1.5, -5), at(-1.5, 5));
		bar(5, -5.5, 7, 5.5);
		break;
	case Glyph::Loop: {
		const auto radius = 5.5;
		const auto start = 70.;
		const auto sweep = 280.;
		auto path = QPainterPath();
		const auto circle = box(-radius, -radius, radius, radius);
		path.arcMoveTo(circle, start);
		path.arcTo(circle, start, sweep);
		p.setPen(stroke(1.7));
		p.setBrush(Qt::NoBrush);
		p.drawPath(path);
		const auto angle = (start + sweep) * M_PI / 180.;
		const auto end = at(radius * std::cos(angle), -radius * std::sin(angle));
		const auto tangent = QPointF(-std::sin(angle), -std::cos(angle));
		const auto normal = QPointF(std::cos(angle), -std::sin(angle));
		const auto size = 2.6 * u;
		triangle(
			end + tangent * size,
			end - tangent * (size * 0.4) + normal * size,
			end - tangent * (size * 0.4) - normal * size);
	} break;
	case Glyph::ZoomIn:
	case Glyph::ZoomOut: {
		p.setPen(stroke(1.7));
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(at(-1.5, -1.5), 5. * u, 5. * u);
		p.setPen(stroke(2.2));
		p.drawLine(at(2.4, 2.4), at(6.5, 6.5));
		p.setPen(stroke(1.5));
		p.drawLine(at(-4, -1.5), at(1, -1.5));
		if (glyph == Glyph::ZoomIn) {
			p.drawLine(at(-1.5, -4), at(-1.5, 1));
		}
	} break;
	case Glyph::Fit: {
		p.setPen(stroke(1.7));
		p.setBrush(Qt::NoBrush);
		const auto e = 6.5;
		const auto l = 3.5;
		for (const auto &[x, y] : {
			std::pair{ -1., -1. },
			std::pair{ 1., -1. },
			std::pair{ 1., 1. },
			std::pair{ -1., 1. },
		}) {
			p.drawLine(at(x * e, y * e), at(x * (e - l), y * e));
			p.drawLine(at(x * e, y * e), at(x * e, y * (e - l)));
		}
	} break;
	case Glyph::BackgroundChecker: {
		auto clip = QPainterPath();
		clip.addRoundedRect(square(), 2.5 * u, 2.5 * u);
		p.setClipPath(clip, Qt::IntersectClip);
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawRect(box(-6.5, -6.5, 0, 0));
		p.drawRect(box(0, 0, 6.5, 6.5));
		p.setClipping(false);
		p.setPen(stroke(1.4));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(square(), 2.5 * u, 2.5 * u);
	} break;
	case Glyph::BackgroundDark:
		p.setPen(stroke(1.4));
		p.setBrush(color);
		p.drawRoundedRect(square(), 2.5 * u, 2.5 * u);
		break;
	case Glyph::BackgroundLight:
		p.setPen(stroke(1.4));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(square(), 2.5 * u, 2.5 * u);
		break;
	case Glyph::Keyframe:
		p.setPen(stroke(1.2));
		p.setBrush(color);
		p.drawPolygon(QPolygonF({
			at(0, -6),
			at(6, 0),
			at(0, 6),
			at(-6, 0),
		}));
		break;
	case Glyph::Graph: {
		// An ease in-out curve between two keyframe dots.
		auto path = QPainterPath();
		path.moveTo(at(-6.5, 5.5));
		path.cubicTo(at(-0.5, 5.5), at(0.5, -5.5), at(6.5, -5.5));
		p.setPen(stroke(1.7));
		p.setBrush(Qt::NoBrush);
		p.drawPath(path);
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawEllipse(at(-6.5, 5.5), 2. * u, 2. * u);
		p.drawEllipse(at(6.5, -5.5), 2. * u, 2. * u);
	} break;
	case Glyph::Cursor:
		p.setPen(stroke(1.5));
		p.setBrush(color);
		p.drawPolygon(QPolygonF({
			at(-5, -7.5),
			at(5.5, 2),
			at(0.5, 2.6),
			at(3.2, 8),
			at(1, 9),
			at(-1.8, 3.6),
			at(-5, 7),
		}));
		break;
	case Glyph::Pen: {
		// A pen nib over a curve with a point.
		p.setPen(stroke(1.5));
		p.setBrush(Qt::NoBrush);
		p.drawPolygon(QPolygonF({
			at(1.5, -8),
			at(8, -1.5),
			at(3.5, 1.5),
			at(-6, 4),
			at(-8, 8),
			at(-4, 6),
			at(-1.5, -3.5),
		}));
		p.drawLine(at(-7.2, 7.2), at(-1.2, 1.2));
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawEllipse(at(0.2, -0.2), 1.6 * u, 1.6 * u);
	} break;
	}
	p.restore();
}

GlyphButton::GlyphButton(
	QWidget *parent,
	Glyph glyph,
	rpl::producer<QString> tooltip,
	int size)
: AbstractButton(parent)
, _glyph(glyph) {
	const auto side = size ? size : Scaled(32);
	resize(side, side);
	std::move(tooltip) | rpl::on_next([=](QString text) {
		setTooltip(std::move(text));
	}, lifetime());
}

void GlyphButton::setGlyph(Glyph glyph) {
	if (_glyph != glyph) {
		_glyph = glyph;
		update();
	}
}

void GlyphButton::setActive(bool active) {
	if (_active != active) {
		_active = active;
		update();
	}
}

void GlyphButton::setTooltip(QString text) {
	_tooltip = std::move(text);
}

QString GlyphButton::tooltipText() const {
	return _tooltip;
}

QPoint GlyphButton::tooltipPos() const {
	return QCursor::pos();
}

bool GlyphButton::tooltipWindowActive() const {
	return Ui::AppInFocus() && Ui::InFocusChain(window());
}

void GlyphButton::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto margin = Scaled(2);
	const auto inner = QRectF(rect()).marginsRemoved(
		QMarginsF(margin, margin, margin, margin));
	const auto radius = Scaled(6);
	const auto over = isOver() || isDown();
	const auto disabled = isDisabled();
	if (!disabled && (_active || over)) {
		p.setPen(Qt::NoPen);
		p.setBrush(_active
			? st::lightButtonBgOver
			: isDown()
			? st::windowBgRipple
			: st::windowBgOver);
		p.drawRoundedRect(inner, radius, radius);
	}
	const auto color = disabled
		? anim::with_alpha(st::menuIconFg->c, 0.35)
		: _active
		? st::windowActiveTextFg->c
		: over
		? st::menuIconFgOver->c
		: st::menuIconFg->c;
	const auto side = Scaled(24) * 1.;
	PaintGlyph(
		p,
		_glyph,
		QRectF((width() - side) / 2., (height() - side) / 2., side, side),
		color);
}

void GlyphButton::enterEventHook(QEnterEvent *e) {
	if (!_tooltip.isEmpty()) {
		Ui::Tooltip::Show(kTooltipDelay, this);
	}
	AbstractButton::enterEventHook(e);
}

void GlyphButton::leaveEventHook(QEvent *e) {
	Ui::Tooltip::Hide();
	AbstractButton::leaveEventHook(e);
}

void GlyphButton::onStateChanged(State was, StateChangeSource source) {
	update();
}

namespace {

// The zoom percent in the toolbar, a menu with zoom presets on click.
class ZoomLabel final
	: public Ui::AbstractButton
	, public Ui::AbstractTooltipShower {
public:
	explicit ZoomLabel(QWidget *parent) : AbstractButton(parent) {
		resize(
			st::normalFont->width(u"3200%"_q) + Scaled(16),
			Scaled(36));
	}

	void setText(QString text) {
		if (_text != text) {
			_text = std::move(text);
			update();
		}
	}

	QString tooltipText() const override {
		return tr::lng_oblivion_lottie_canvas_zoom(tr::now);
	}
	QPoint tooltipPos() const override {
		return QCursor::pos();
	}
	bool tooltipWindowActive() const override {
		return Ui::AppInFocus() && Ui::InFocusChain(window());
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		const auto over = isOver() || isDown();
		if (over) {
			auto hq = PainterHighQualityEnabler(p);
			const auto margin = Scaled(2);
			const auto radius = Scaled(6);
			p.setPen(Qt::NoPen);
			p.setBrush(isDown() ? st::windowBgRipple : st::windowBgOver);
			p.drawRoundedRect(
				QRectF(rect()).marginsRemoved(
					QMarginsF(margin, margin, margin, margin)),
				radius,
				radius);
		}
		p.setFont(st::normalFont);
		p.setPen(over ? st::windowBoldFg : st::windowFg);
		p.drawText(rect(), _text, style::al_center);
	}
	void enterEventHook(QEnterEvent *e) override {
		Ui::Tooltip::Show(kTooltipDelay, this);
		AbstractButton::enterEventHook(e);
	}
	void leaveEventHook(QEvent *e) override {
		Ui::Tooltip::Hide();
		AbstractButton::leaveEventHook(e);
	}
	void onStateChanged(State was, StateChangeSource source) override {
		update();
	}

private:
	QString _text;

};

[[nodiscard]] QString PercentText(double scale) {
	const auto percent = scale * 100.;
	return tr::lng_oblivion_lottie_canvas_percent(
		tr::now,
		lt_value,
		FormatDecimal(percent, (percent < 10.) ? 1 : 0));
}

[[nodiscard]] Glyph BackgroundGlyph(CanvasBackground background) {
	switch (background) {
	case CanvasBackground::Checker: return Glyph::BackgroundChecker;
	case CanvasBackground::Dark: return Glyph::BackgroundDark;
	case CanvasBackground::Light: return Glyph::BackgroundLight;
	}
	return Glyph::BackgroundChecker;
}

[[nodiscard]] QString BackgroundText(CanvasBackground background) {
	switch (background) {
	case CanvasBackground::Checker:
		return tr::lng_oblivion_lottie_canvas_bg_checker(tr::now);
	case CanvasBackground::Dark:
		return tr::lng_oblivion_lottie_canvas_bg_dark(tr::now);
	case CanvasBackground::Light:
		return tr::lng_oblivion_lottie_canvas_bg_light(tr::now);
	}
	return QString();
}

// Current frame as PNG (composition size) or SVG (vector when rlottie's
// render tree converts, an embedded PNG otherwise), written off the main
// thread.
void ExportFrame(
		not_null<EditorController*> controller,
		not_null<QWidget*> parent,
		bool svg) {
	const auto document = controller->document();
	const auto frame = controller->frameIndex();
	const auto extension = svg ? u"svg"_q : u"png"_q;
	// The frame number as the timeline shows it ("Frame 90").
	const auto name = SafeFileName(tr::lng_oblivion_lottie_editor_frame_file(
		tr::now,
		lt_name,
		SafeFileName(controller->name()),
		lt_value,
		QString::number(controller->currentFrame())));
	const auto filter = svg
		? FileFilter(
			tr::lng_oblivion_lottie_editor_filter_svg(tr::now),
			u"*.svg"_q)
		: FileFilter(
			tr::lng_oblivion_lottie_editor_filter_png(tr::now),
			u"*.png"_q);
	const auto weak = base::make_weak(controller.get());
	FileDialog::GetWritePath(
		parent.get(),
		tr::lng_oblivion_lottie_editor_export_frame_title(tr::now),
		filter,
		SuggestedPath(name + '.' + extension),
		crl::guard(parent, [=](QString &&result) {
			if (result.isEmpty()) {
				return;
			}
			const auto path = WithExtension(result, extension);
			crl::async([=] {
				const auto json = document.toJson();
				auto bytes = QByteArray();
				auto raster = false;
				auto reason = QString();
				if (svg) {
					auto exported = Oblivion::Lottie::ExportSvg(json, frame);
					bytes = std::move(exported.svg);
					raster = !exported.vector;
					reason = exported.reason;
				} else {
					const auto size = document.size().boundedTo(
						QSize(kMaxExportSide, kMaxExportSide));
					const auto image = Oblivion::Lottie::RenderFrame(
						json,
						frame,
						size);
					if (!image.isNull()) {
						auto buffer = QBuffer(&bytes);
						if (!buffer.open(QIODevice::WriteOnly)
							|| !image.save(&buffer, "PNG")) {
							bytes = QByteArray();
						}
					}
				}
				auto written = false;
				if (!bytes.isEmpty()) {
					auto file = QSaveFile(path);
					written = file.open(QIODevice::WriteOnly)
						&& (file.write(bytes) == bytes.size())
						&& file.commit();
				}
				crl::on_main(weak, [=] {
					const auto show = weak->uiShow();
					if (!written) {
						LOG(("Oblivion LottieEdit: could not export \"%1\"."
							).arg(path));
						if (show) {
							show->showToast(
								tr::lng_oblivion_lottie_editor_export_failed(
									tr::now));
						}
						return;
					} else if (raster && !reason.isEmpty()) {
						LOG(("Oblivion LottieEdit: SVG raster fallback: %1"
							).arg(reason));
					}
					if (show) {
						auto text = tr::lng_oblivion_saved_to(
							tr::now,
							lt_path,
							QDir::toNativeSeparators(path));
						if (raster) {
							text += '\n' + Oblivion::Lottie::SvgRasterNote();
						}
						show->showToast(text);
					}
				});
			});
		}));
}

} // namespace

// EditorWidget::Splitter: a thin draggable area over the line between
// two panels.

class EditorWidget::Splitter final : public Ui::RpWidget {
public:
	// vertical: a vertical line, dragged horizontally.
	Splitter(QWidget *parent, bool vertical);

	// Offset of the mouse since the press, in pixels.
	[[nodiscard]] rpl::producer<int> drags() const {
		return _drags.events();
	}
	[[nodiscard]] rpl::producer<> dragStarts() const {
		return _starts.events();
	}
	// Double click: back to the default size.
	[[nodiscard]] rpl::producer<> resets() const {
		return _resets.events();
	}

protected:
	void paintEvent(QPaintEvent *e) override;
	void enterEventHook(QEnterEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;

private:
	const bool _vertical = false;
	bool _over = false;
	std::optional<QPoint> _pressed;
	rpl::event_stream<int> _drags;
	rpl::event_stream<> _starts;
	rpl::event_stream<> _resets;

};

EditorWidget::Splitter::Splitter(QWidget *parent, bool vertical)
: RpWidget(parent)
, _vertical(vertical) {
	setCursor(vertical ? Qt::SplitHCursor : Qt::SplitVCursor);
	setMouseTracking(true);
}

void EditorWidget::Splitter::paintEvent(QPaintEvent *e) {
	if (!_over && !_pressed) {
		return;
	}
	auto p = QPainter(this);
	const auto line = std::max(Scaled(2), 2);
	const auto rect = _vertical
		? QRect((width() - line) / 2, 0, line, height())
		: QRect(0, (height() - line) / 2, width(), line);
	p.fillRect(rect, st::windowActiveTextFg);
}

void EditorWidget::Splitter::enterEventHook(QEnterEvent *e) {
	_over = true;
	update();
}

void EditorWidget::Splitter::leaveEventHook(QEvent *e) {
	_over = false;
	update();
}

void EditorWidget::Splitter::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	_pressed = e->globalPosition().toPoint();
	_starts.fire({});
	update();
}

void EditorWidget::Splitter::mouseMoveEvent(QMouseEvent *e) {
	if (!_pressed) {
		return;
	}
	const auto delta = e->globalPosition().toPoint() - *_pressed;
	_drags.fire(_vertical ? delta.x() : delta.y());
}

void EditorWidget::Splitter::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton || !_pressed) {
		return;
	}
	_pressed = std::nullopt;
	_over = rect().contains(e->pos());
	update();
}

void EditorWidget::Splitter::mouseDoubleClickEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_resets.fire({});
	}
}

// EditorWidget::Toolbar.

class EditorWidget::Toolbar final : public Ui::RpWidget {
public:
	Toolbar(
		QWidget *parent,
		not_null<EditorController*> controller,
		QString doneText);

	void setValidation(const std::optional<ValidationResult> &validation);

	// Binds the background / zoom controls, called once by EditorWidget.
	void setCanvas(not_null<CanvasPanel*> canvas);

	[[nodiscard]] rpl::producer<> doneRequests() const {
		return _doneRequests.events();
	}

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

private:
	enum class Status {
		None,
		Checking,
		Ok,
		Warnings,
		Errors,
	};

	not_null<Ui::RoundButton*> addTextButton(
		rpl::producer<QString> text,
		Fn<void()> callback);
	void showExportMenu();
	void showBackgroundMenu();
	void showZoomMenu();
	void refreshUndoRedo();
	void refreshStatus(Status status, int count);
	void updateLayout();

	const not_null<EditorController*> _controller;
	std::vector<not_null<Ui::RoundButton*>> _buttons;
	Ui::RoundButton *_export = nullptr;
	const not_null<ToolButton*> _undo;
	const not_null<ToolButton*> _redo;
	const not_null<GlyphButton*> _background;
	const not_null<GlyphButton*> _zoomOut;
	const not_null<ZoomLabel*> _zoom;
	const not_null<GlyphButton*> _zoomIn;
	GlyphButton *_toolSelect = nullptr;
	GlyphButton *_toolPen = nullptr;
	CanvasPanel *_canvas = nullptr;
	base::unique_qptr<Ui::RoundButton> _status;
	Ui::RoundButton *_done = nullptr;
	base::unique_qptr<Ui::PopupMenu> _menu;
	rpl::event_stream<> _doneRequests;
	Status _statusKind = Status::None;
	int _statusCount = -1;
	int _infoLeft = 0;
	int _infoRight = 0;

};

EditorWidget::Toolbar::Toolbar(
	QWidget *parent,
	not_null<EditorController*> controller,
	QString doneText)
: RpWidget(parent)
, _controller(controller)
, _undo(Ui::CreateChild<ToolButton>(
	this,
	st::ivEditorToolbarUndoIcon,
	rpl::single(QString())))
, _redo(Ui::CreateChild<ToolButton>(
	this,
	st::ivEditorToolbarRedoIcon,
	rpl::single(QString())))
, _background(Ui::CreateChild<GlyphButton>(
	this,
	Glyph::BackgroundChecker,
	tr::lng_oblivion_lottie_canvas_bg(),
	Scaled(36)))
, _zoomOut(Ui::CreateChild<GlyphButton>(
	this,
	Glyph::ZoomOut,
	tr::lng_oblivion_lottie_canvas_zoom_out(
	) | rpl::map([](const QString &text) {
		return WithShortcut(
			text,
			QKeySequence(Qt::ControlModifier | Qt::Key_Minus));
	}),
	Scaled(36)))
, _zoom(Ui::CreateChild<ZoomLabel>(this))
, _zoomIn(Ui::CreateChild<GlyphButton>(
	this,
	Glyph::ZoomIn,
	tr::lng_oblivion_lottie_canvas_zoom_in(
	) | rpl::map([](const QString &text) {
		return WithShortcut(
			text,
			QKeySequence(Qt::ControlModifier | Qt::Key_Equal));
	}),
	Scaled(36))) {
	const auto request = [=](EditorAction action) {
		return [=] { _controller->requestAction(action); };
	};
	addTextButton(
		tr::lng_oblivion_lottie_editor_new(),
		request(EditorAction::New));
	addTextButton(
		tr::lng_oblivion_lottie_editor_open(),
		request(EditorAction::Open));
	addTextButton(
		tr::lng_oblivion_lottie_editor_save(),
		request(EditorAction::Save));
	_export = addTextButton(
		tr::lng_oblivion_lottie_editor_export(),
		[=] { showExportMenu(); });

	_undo->setClickedCallback([=] { _controller->undo(); });
	_redo->setClickedCallback([=] { _controller->redo(); });

	const auto toolButton = [&](Glyph glyph, CanvasTool tool, int key) {
		const auto result = Ui::CreateChild<GlyphButton>(
			this,
			glyph,
			rpl::single(WithShortcut(
				CanvasToolText(tool),
				QKeySequence(key))),
			Scaled(36));
		result->setClickedCallback([=] {
			SetCurrentTool(_controller, tool);
		});
		return result;
	};
	_toolSelect = toolButton(Glyph::Cursor, CanvasTool::Select, Qt::Key_V);
	_toolPen = toolButton(Glyph::Pen, CanvasTool::Pen, Qt::Key_P);
	CurrentToolValue(
		_controller
	) | rpl::on_next([=](CanvasTool tool) {
		_toolSelect->setActive(tool == CanvasTool::Select);
		_toolPen->setActive(tool == CanvasTool::Pen);
	}, lifetime());

	if (!doneText.isEmpty()) {
		_done = Ui::CreateChild<Ui::RoundButton>(
			this,
			rpl::single(doneText),
			st::defaultActiveButton);
		_done->setClickedCallback([=] { _doneRequests.fire({}); });
	}

	rpl::merge(
		_controller->undoAvailable() | rpl::to_empty,
		_controller->redoAvailable() | rpl::to_empty,
		_controller->documentChanged() | rpl::to_empty,
		_controller->nameValue() | rpl::to_empty,
		_controller->dirtyValue() | rpl::to_empty
	) | rpl::on_next([=] {
		refreshUndoRedo();
		update();
	}, lifetime());

	refreshStatus(Status::Checking, 0);
}

not_null<Ui::RoundButton*> EditorWidget::Toolbar::addTextButton(
		rpl::producer<QString> text,
		Fn<void()> callback) {
	const auto result = Ui::CreateChild<Ui::RoundButton>(
		this,
		std::move(text),
		st::defaultBoxButton);
	result->setClickedCallback(std::move(callback));
	result->widthValue() | rpl::on_next([=] {
		updateLayout();
	}, result->lifetime());
	_buttons.push_back(result);
	return result;
}

void EditorWidget::Toolbar::showExportMenu() {
	const auto request = [=](EditorAction action) {
		return [=] { _controller->requestAction(action); };
	};
	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuWithIcons);
	_menu->addAction(
		tr::lng_oblivion_lottie_editor_export_tgs(tr::now),
		request(EditorAction::ExportTgs),
		&st::menuIconStickers);
	_menu->addAction(
		tr::lng_oblivion_lottie_editor_export_json(tr::now),
		request(EditorAction::ExportJson),
		&st::menuIconFile);
	_menu->addAction(
		tr::lng_oblivion_packs_add_title(tr::now),
		[=] {
			// What Telegram would reject or draw differently is shown
			// first, the pack box comes after "Add anyway".
			CheckBeforeSticker(
				_controller,
				ValidationPurpose::StickerPack,
				crl::guard(this, [=] {
					auto source = StickerSource::FromLottie(
						_controller->document().toJson());
					source.name = _controller->name();
					AddToStickerPack(std::move(source));
				}));
		},
		&st::menuIconStickerAdd);
	_menu->addSeparator();
	_menu->addAction(
		tr::lng_oblivion_lottie_editor_export_png(tr::now),
		[=] { ExportFrame(_controller, this, false); },
		&st::menuIconPhoto);
	_menu->addAction(
		tr::lng_oblivion_lottie_editor_export_svg(tr::now),
		[=] { ExportFrame(_controller, this, true); },
		&st::menuIconExport);
	_menu->addSeparator();
	_menu->addAction(
		tr::lng_oblivion_lottie_editor_save_as(tr::now),
		request(EditorAction::SaveAs),
		&st::menuIconDownload);
	_menu->popup(_export->mapToGlobal(QPoint(0, _export->height())));
}

void EditorWidget::Toolbar::setCanvas(not_null<CanvasPanel*> canvas) {
	_canvas = canvas;
	_background->setClickedCallback([=] { showBackgroundMenu(); });
	_zoomOut->setClickedCallback([=] { canvas->zoomBy(1. / kZoomStep); });
	_zoomIn->setClickedCallback([=] { canvas->zoomBy(kZoomStep); });
	_zoom->setClickedCallback([=] { showZoomMenu(); });
	canvas->backgroundValue(
	) | rpl::on_next([=](CanvasBackground background) {
		_background->setGlyph(BackgroundGlyph(background));
	}, lifetime());
	canvas->scaleValue(
	) | rpl::on_next([=](double scale) {
		_zoom->setText(PercentText(scale));
	}, lifetime());
}

void EditorWidget::Toolbar::showBackgroundMenu() {
	if (!_canvas) {
		return;
	}
	const auto canvas = _canvas;
	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuWithIcons);
	for (const auto background : {
		CanvasBackground::Checker,
		CanvasBackground::Dark,
		CanvasBackground::Light,
	}) {
		_menu->addAction(
			BackgroundText(background),
			[=] { canvas->setBackground(background); },
			(canvas->background() == background)
				? &st::mediaPlayerMenuCheck
				: nullptr);
	}
	_menu->popup(
		_background->mapToGlobal(QPoint(0, _background->height())));
}

void EditorWidget::Toolbar::showZoomMenu() {
	if (!_canvas) {
		return;
	}
	const auto canvas = _canvas;
	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuWithIcons);
	const auto fitted = std::abs(_controller->zoom() - 1.) < 1e-6
		&& _controller->pan().isNull();
	_menu->addAction(
		tr::lng_oblivion_lottie_canvas_zoom_fit(tr::now),
		[=] { canvas->zoomToFit(); },
		fitted ? &st::mediaPlayerMenuCheck : nullptr);
	_menu->addSeparator();
	const auto current = canvas->scale();
	for (const auto scale : { 0.25, 0.5, 1., 2., 4., 8. }) {
		_menu->addAction(
			PercentText(scale),
			[=] { canvas->zoomToScale(scale); },
			(!fitted && std::abs(current - scale) < 1e-3)
				? &st::mediaPlayerMenuCheck
				: nullptr);
	}
	_menu->popup(_zoom->mapToGlobal(QPoint(0, _zoom->height())));
}

void EditorWidget::Toolbar::refreshUndoRedo() {
	_undo->setAvailable(_controller->canUndo());
	_redo->setAvailable(_controller->canRedo());
	_undo->setTooltip(WithShortcut(
		UndoText(_controller->undoCommand()),
		QKeySequence(QKeySequence::Undo)));
	_redo->setTooltip(WithShortcut(
		RedoText(_controller->redoCommand()),
		QKeySequence(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_Z)));
}

void EditorWidget::Toolbar::setValidation(
		const std::optional<ValidationResult> &validation) {
	if (!validation) {
		refreshStatus(Status::Checking, 0);
		return;
	}
	auto errors = 0;
	auto warnings = 0;
	for (const auto &issue : validation->issues) {
		++((issue.severity == IssueSeverity::Error) ? errors : warnings);
	}
	if (errors) {
		refreshStatus(Status::Errors, errors);
	} else if (warnings) {
		refreshStatus(Status::Warnings, warnings);
	} else {
		refreshStatus(Status::Ok, 0);
	}
}

void EditorWidget::Toolbar::refreshStatus(Status status, int count) {
	if (_statusKind == status && _statusCount == count) {
		return;
	}
	_statusKind = status;
	_statusCount = count;
	const auto text = [&] {
		switch (status) {
		case Status::None:
		case Status::Checking:
			return tr::lng_oblivion_lottie_editor_tgs_checking(tr::now);
		case Status::Ok: return tr::lng_oblivion_lottie_editor_tgs_ok(tr::now);
		case Status::Warnings:
			return tr::lng_oblivion_lottie_editor_tgs_warnings(
				tr::now,
				lt_value,
				QString::number(count));
		case Status::Errors:
			return tr::lng_oblivion_lottie_editor_tgs_errors(
				tr::now,
				lt_value,
				QString::number(count));
		}
		return QString();
	}();
	_status = base::make_unique_q<Ui::RoundButton>(
		this,
		rpl::single(text),
		(status == Status::Errors)
			? st::attentionBoxButton
			: st::defaultBoxButton);
	_status->setClickedCallback([=] {
		ShowValidationBox(_controller);
	});
	// A status, not an action: green when the sticker is fine, gray while
	// checking, the accent for recommendations and red for errors, so it
	// doesn't read as one more command next to "New", "Open"...
	const auto raw = _status.get();
	const auto refreshColor = [=] {
		switch (status) {
		case Status::Ok:
			// Not boxTextFgGood: the night themes make it blue, the same
			// as the commands. The green peer color stays green everywhere.
			raw->setTextFgOverride(st::historyPeer2NameFg->c);
			break;
		case Status::None:
		case Status::Checking:
			raw->setTextFgOverride(st::windowSubTextFg->c);
			break;
		case Status::Warnings:
		case Status::Errors:
			raw->setTextFgOverride(std::nullopt);
			break;
		}
	};
	refreshColor();
	style::PaletteChanged(
	) | rpl::on_next(refreshColor, raw->lifetime());
	_status->show();
	updateLayout();
}

void EditorWidget::Toolbar::updateLayout() {
	if (!_toolSelect || !_toolPen) {
		// Called by the first buttons while the constructor still runs.
		return;
	}
	const auto padding = Scaled(kToolbarPadding);
	const auto skip = Scaled(kToolbarSkip);
	const auto group = Scaled(kToolbarGroupSkip);
	const auto centered = [&](not_null<Ui::RpWidget*> widget, int x) {
		widget->moveToLeft(x, (height() - widget->height()) / 2, width());
	};
	auto left = padding;
	for (const auto &button : _buttons) {
		centered(button, left);
		left += button->width() + skip;
	}
	left += group - skip;
	centered(_undo, left);
	left += _undo->width();
	centered(_redo, left);
	left += _redo->width() + group;

	auto right = width() - padding;
	if (_done) {
		right -= _done->width();
		centered(_done, right);
		right -= skip;
	}
	if (_status) {
		right -= _status->width();
		centered(_status.get(), right);
		right -= group;
	}
	// Canvas controls, the least important ones are dropped first when the
	// window is too narrow (the name / info text is dropped before them).
	const auto tools = _toolSelect->width() + _toolPen->width() + group;
	const auto full = _zoomIn->width()
		+ _zoom->width()
		+ _zoomOut->width()
		+ skip
		+ _background->width()
		+ group
		+ tools;
	const auto withoutLabel = full - _zoom->width();
	// The zoom percent gives its place to the name of the animation.
	const auto showLabel = (right - full >= left + Scaled(kToolbarNameRoom));
	const auto showControls = (right - withoutLabel >= left);
	// The tools stay when the view controls don't fit: they have shortcuts
	// (V / P), but the buttons are how people find them.
	const auto showTools = showControls || (right - tools >= left);
	_zoom->setVisible(showLabel);
	_zoomIn->setVisible(showControls);
	_zoomOut->setVisible(showControls);
	_background->setVisible(showControls);
	_toolSelect->setVisible(showTools);
	_toolPen->setVisible(showTools);
	if (showControls) {
		right -= _zoomIn->width();
		centered(_zoomIn, right);
		if (showLabel) {
			right -= _zoom->width();
			centered(_zoom, right);
		}
		right -= _zoomOut->width();
		centered(_zoomOut, right);
		right -= skip + _background->width();
		centered(_background, right);
		right -= group;
	}
	if (showTools) {
		right -= _toolPen->width();
		centered(_toolPen, right);
		right -= _toolSelect->width();
		centered(_toolSelect, right);
		right -= group;
	}
	_infoLeft = left;
	_infoRight = right;
	update();
}

void EditorWidget::Toolbar::resizeEvent(QResizeEvent *e) {
	updateLayout();
}

void EditorWidget::Toolbar::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(e->rect(), st::windowBg);
	p.fillRect(
		0,
		height() - st::lineWidth,
		width(),
		st::lineWidth,
		st::shadowFg);

	const auto available = _infoRight - _infoLeft;
	if (available <= 0) {
		return;
	}
	const auto &document = _controller->document();
	const auto name = (_controller->dirty() ? u"• "_q : QString())
		+ (_controller->name().isEmpty()
			? tr::lng_oblivion_lottie_editor_new_name(tr::now)
			: _controller->name());
	const auto info = u"  ·  "_q + DocumentInfoText(document);
	const auto &nameFont = st::semiboldFont;
	const auto &infoFont = st::normalFont;
	const auto infoWidth = infoFont->width(info);
	// The name wins: the info is dropped first, then the name is elided.
	const auto nameFull = nameFont->width(name);
	const auto nameWidth = std::min(nameFull, available);
	const auto showInfo = (available - nameFull >= infoWidth);
	const auto full = nameWidth + (showInfo ? infoWidth : 0);
	auto x = _infoLeft + (available - full) / 2;
	const auto y = (height() - nameFont->height) / 2;
	p.setFont(nameFont);
	p.setPen(st::windowBoldFg);
	p.drawText(
		x,
		y + nameFont->ascent,
		nameFont->elided(name, std::max(nameWidth, 0)));
	x += nameWidth;
	if (showInfo) {
		p.setFont(infoFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(x, y + infoFont->ascent, info);
	}
}

// EditorWidget.

EditorWidget::EditorWidget(
	QWidget *parent,
	not_null<EditorController*> controller,
	QString doneText)
: RpWidget(parent)
, _controller(controller)
, _toolbar(Ui::CreateChild<Toolbar>(this, controller, doneText))
, _layers(Ui::CreateChild<LayersPanel>(this, controller))
, _canvas(Ui::CreateChild<CanvasPanel>(this, controller))
, _inspector(Ui::CreateChild<InspectorPanel>(this, controller))
, _timeline(Ui::CreateChild<TimelinePanel>(this, controller))
, _validationTimer([=] { runValidation(); }) {
	setFocusPolicy(Qt::StrongFocus);
	for (const auto panel : std::initializer_list<not_null<QWidget*>>{
		_layers,
		_canvas,
		_inspector,
		_timeline,
	}) {
		if (panel->focusPolicy() == Qt::NoFocus) {
			panel->setFocusPolicy(Qt::ClickFocus);
		}
	}
	_toolbar->setCanvas(_canvas);
	setupSplitters();
	setupValidation();
	updateGeometries();

	// Another file opened in this window: the panels reset their own view
	// state, the layer search of the old file is dropped here.
	_controller->documentChanged(
	) | rpl::filter([](const DocumentChange &change) {
		return (change.source == ChangeSource::Load);
	}) | rpl::on_next([=] {
		_layers->setFilter(QString());
	}, lifetime());
}

EditorWidget::~EditorWidget() = default;

not_null<EditorController*> EditorWidget::controller() const {
	return _controller;
}

not_null<CanvasPanel*> EditorWidget::canvas() const {
	return _canvas;
}

not_null<LayersPanel*> EditorWidget::layers() const {
	return _layers;
}

not_null<InspectorPanel*> EditorWidget::inspector() const {
	return _inspector;
}

not_null<TimelinePanel*> EditorWidget::timeline() const {
	return _timeline;
}

rpl::producer<> EditorWidget::doneRequests() const {
	return _toolbar->doneRequests();
}

const std::optional<ValidationResult> &EditorWidget::validation() const {
	return _validation;
}

rpl::producer<> EditorWidget::validationUpdates() const {
	return _validationUpdates.events();
}

void EditorWidget::setupSplitters() {
	_layersSplitter = Ui::CreateChild<Splitter>(this, true);
	_inspectorSplitter = Ui::CreateChild<Splitter>(this, true);
	_timelineSplitter = Ui::CreateChild<Splitter>(this, false);

	const auto start = lifetime().make_state<int>(0);
	_layersSplitter->dragStarts() | rpl::on_next([=] {
		*start = _layers->width();
	}, lifetime());
	_layersSplitter->drags() | rpl::on_next([=](int delta) {
		// Never 0, that means "the default size".
		Layout().layers = std::max(*start + delta, 1);
		updateGeometries();
	}, lifetime());

	_inspectorSplitter->dragStarts() | rpl::on_next([=] {
		*start = _inspector->width();
	}, lifetime());
	_inspectorSplitter->drags() | rpl::on_next([=](int delta) {
		Layout().inspector = std::max(*start - delta, 1);
		updateGeometries();
	}, lifetime());

	_timelineSplitter->dragStarts() | rpl::on_next([=] {
		*start = _timeline->height();
	}, lifetime());
	_timelineSplitter->drags() | rpl::on_next([=](int delta) {
		Layout().timeline = std::max(*start - delta, 1);
		updateGeometries();
	}, lifetime());

	// Double click on a splitter: back to the default size (0 = default).
	_layersSplitter->resets() | rpl::on_next([=] {
		Layout().layers = 0;
		updateGeometries();
	}, lifetime());
	_inspectorSplitter->resets() | rpl::on_next([=] {
		Layout().inspector = 0;
		updateGeometries();
	}, lifetime());
	_timelineSplitter->resets() | rpl::on_next([=] {
		Layout().timeline = 0;
		updateGeometries();
	}, lifetime());
}

void EditorWidget::setupValidation() {
	_controller->documentChanged(
	) | rpl::on_next([=] {
		scheduleValidation();
	}, lifetime());
	_validationTimer.callOnce(crl::time(50));
}

void EditorWidget::scheduleValidation() {
	_validationTimer.callOnce(kValidationDelay);
}

void EditorWidget::runValidation() {
	const auto document = _controller->document();
	const auto generation = ++_validationGeneration;
	const auto weak = QPointer<EditorWidget>(this);
	crl::async([=] {
		auto result = Validate(document);
		crl::on_main(weak, [=, result = std::move(result)]() mutable {
			if (generation != _validationGeneration) {
				return;
			}
			_validation = std::move(result);
			_toolbar->setValidation(_validation);
			_validationUpdates.fire({});
		});
	});
}

void EditorWidget::updateGeometries() {
	const auto line = st::lineWidth;
	const auto toolbarHeight = Scaled(kToolbarHeight);
	const auto w = width();
	const auto h = height();
	_toolbar->setGeometry(0, 0, w, toolbarHeight);

	const auto top = toolbarHeight;
	const auto available = std::max(h - top, 0);

	auto &layout = Layout();
	const auto layersWanted = layout.layers
		? layout.layers
		: Scaled(kLayersDefault);
	const auto inspectorWanted = layout.inspector
		? layout.inspector
		: Scaled(kInspectorDefault);
	const auto timelineWanted = layout.timeline
		? layout.timeline
		: Scaled(kTimelineDefault);

	auto layers = std::clamp(
		layersWanted,
		Scaled(kLayersMin),
		std::max(Scaled(kLayersMin), std::min(Scaled(kLayersMax), w / 3)));
	auto inspector = std::clamp(
		inspectorWanted,
		Scaled(kInspectorMin),
		std::max(
			Scaled(kInspectorMin),
			std::min(Scaled(kInspectorMax), w / 3)));
	auto overflow = layers + inspector + 2 * line + Scaled(kCanvasMinWidth) - w;
	if (overflow > 0) {
		const auto take = std::min(overflow, inspector - Scaled(kInspectorMin));
		inspector -= std::max(take, 0);
		overflow -= std::max(take, 0);
	}
	if (overflow > 0) {
		const auto take = std::min(overflow, layers - Scaled(kLayersMin));
		layers -= std::max(take, 0);
	}
	const auto timeline = std::clamp(
		timelineWanted,
		Scaled(kTimelineMin),
		std::max(
			Scaled(kTimelineMin),
			available - line - Scaled(kCanvasMinHeight)));
	const auto topHeight = std::max(available - timeline - line, 0);
	const auto canvasLeft = layers + line;
	const auto canvasWidth = std::max(w - layers - inspector - 2 * line, 0);

	_layers->setGeometry(0, top, layers, topHeight);
	_canvas->setGeometry(canvasLeft, top, canvasWidth, topHeight);
	_inspector->setGeometry(w - inspector, top, inspector, topHeight);
	_timeline->setGeometry(0, top + topHeight + line, w, timeline);
	// The timeline's names column ends on the line under the layers panel.
	_timeline->setNamesWidth(layers + line);

	const auto grab = Scaled(kSplitterGrab);
	_layersSplitter->setGeometry(
		layers + line / 2 - grab / 2,
		top,
		grab,
		topHeight);
	_inspectorSplitter->setGeometry(
		w - inspector - line + line / 2 - grab / 2,
		top,
		grab,
		topHeight);
	_timelineSplitter->setGeometry(
		0,
		top + topHeight + line / 2 - grab / 2,
		w,
		grab);
	_layersSplitter->raise();
	_inspectorSplitter->raise();
	_timelineSplitter->raise();
	update();
}

void EditorWidget::resizeEvent(QResizeEvent *e) {
	updateGeometries();
}

void EditorWidget::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(e->rect(), st::windowBg);
	const auto line = st::lineWidth;
	const auto top = _layers->y();
	const auto topHeight = _layers->height();
	p.fillRect(_layers->width(), top, line, topHeight, st::shadowFg);
	p.fillRect(_inspector->x() - line, top, line, topHeight, st::shadowFg);
	p.fillRect(0, _timeline->y() - line, width(), line, st::shadowFg);
}

bool EditorWidget::eventHook(QEvent *e) {
	if (e->type() == QEvent::ShortcutOverride) {
		// Keys not accepted by the focused child (a text field accepts
		// what it edits with) come here before the app-wide shortcuts.
		if (claimsShortcut(static_cast<QKeyEvent*>(e))) {
			e->accept();
			return true;
		}
	}
	return RpWidget::eventHook(e);
}

void EditorWidget::keyPressEvent(QKeyEvent *e) {
	if (!handleKey(e)) {
		RpWidget::keyPressEvent(e);
	}
}

bool EditorWidget::claimsShortcut(not_null<QKeyEvent*> e) const {
	const auto key = e->key();
	const auto modifiers = e->modifiers()
		& ~(Qt::KeypadModifier | Qt::GroupSwitchModifier);
	const auto command = (modifiers & Qt::ControlModifier) != 0;
	const auto alt = (modifiers & Qt::AltModifier) != 0;
	const auto meta = (modifiers & Qt::MetaModifier) != 0;
	if (meta) {
		return false;
	} else if (alt && !command) {
		// Alt+Up / Alt+Down are "previous / next chat" in the main window.
		return (key == Qt::Key_Left)
			|| (key == Qt::Key_Right)
			|| (key == Qt::Key_Up)
			|| (key == Qt::Key_Down);
	} else if (command && !alt) {
		switch (key) {
		case Qt::Key_Z:
		case Qt::Key_Y:
		case Qt::Key_C:
		case Qt::Key_V:
		case Qt::Key_D:
		case Qt::Key_N:
		case Qt::Key_O:
		case Qt::Key_S:
		case Qt::Key_E:
		case Qt::Key_Equal:
		case Qt::Key_Plus:
		case Qt::Key_Minus:
		case Qt::Key_Underscore:
		case Qt::Key_0: // "Saved Messages" in the main window.
			return true;
		}
	}
	return false;
}

bool EditorWidget::handleKey(not_null<QKeyEvent*> e) {
	const auto key = e->key();
	const auto modifiers = e->modifiers()
		& ~(Qt::KeypadModifier | Qt::GroupSwitchModifier);
	const auto command = (modifiers & Qt::ControlModifier) != 0;
	const auto shift = (modifiers & Qt::ShiftModifier) != 0;
	const auto plain = !(modifiers
		& (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
	const auto request = [&](EditorAction action) {
		_controller->requestAction(action);
		return true;
	};
	const auto alt = (modifiers & Qt::AltModifier) != 0;
	// A panel drag (canvas move, keyframes, a layer bar, a value scrub)
	// recomputes its edit from the document at the press: an undo or a
	// delete in the middle of it would be overwritten or break it.
	const auto dragging = (QGuiApplication::mouseButtons() != Qt::NoButton);
	if (e->matches(QKeySequence::Undo)) {
		if (!dragging) {
			_controller->undo();
		}
		return true;
	} else if (e->matches(QKeySequence::Redo)
		|| (command && shift && key == Qt::Key_Z)
		|| (command && !shift && key == Qt::Key_Y)) {
		if (!dragging) {
			_controller->redo();
		}
		return true;
	} else if (alt && !command) {
		// Alt+arrows nudge the selection on the canvas.
		const auto step = shift ? kNudgeLarge : 1.;
		const auto delta = [&]() -> std::optional<QPointF> {
			switch (key) {
			case Qt::Key_Left: return QPointF(-step, 0.);
			case Qt::Key_Right: return QPointF(step, 0.);
			case Qt::Key_Up: return QPointF(0., -step);
			case Qt::Key_Down: return QPointF(0., step);
			}
			return std::nullopt;
		}();
		if (delta) {
			if (!dragging) {
				_canvas->nudgeSelection(*delta);
			}
			return true;
		}
		return false;
	} else if (command) {
		switch (key) {
		case Qt::Key_C:
			return _timeline->copyKeyframes();
		case Qt::Key_V:
			return !dragging && _timeline->pasteKeyframes();
		case Qt::Key_D:
			if (!dragging) {
				_controller->duplicateSelection();
			}
			return true;
		case Qt::Key_N: return request(EditorAction::New);
		case Qt::Key_O: return request(EditorAction::Open);
		case Qt::Key_S:
			return request(shift ? EditorAction::SaveAs : EditorAction::Save);
		case Qt::Key_E: return request(EditorAction::ExportTgs);
		case Qt::Key_W: return request(EditorAction::Close);
		case Qt::Key_Equal:
		case Qt::Key_Plus:
			_canvas->zoomBy(kZoomStep);
			return true;
		case Qt::Key_Minus:
		case Qt::Key_Underscore:
			_canvas->zoomBy(1. / kZoomStep);
			return true;
		case Qt::Key_0:
			_canvas->zoomToFit();
			return true;
		}
		return false;
	} else if (!plain && key != Qt::Key_Left && key != Qt::Key_Right) {
		return false;
	}
	switch (key) {
	case Qt::Key_J:
		return shift ? false : _timeline->jumpToKeyframe(-1);
	case Qt::Key_K:
		return shift ? false : _timeline->jumpToKeyframe(1);
	case Qt::Key_V:
	case Qt::Key_P:
	case kCyrillicOnV:
	case kCyrillicOnP:
		if (shift) {
			return false;
		} else if (!dragging) {
			SetCurrentTool(
				_controller,
				(key == Qt::Key_P || key == kCyrillicOnP)
					? CanvasTool::Pen
					: CanvasTool::Select);
		}
		return true;
	case Qt::Key_Space:
		_controller->togglePlaying();
		return true;
	case Qt::Key_Left:
		_controller->setPlaying(false);
		_controller->stepFrame(shift ? -10 : -1);
		return true;
	case Qt::Key_Right:
		_controller->setPlaying(false);
		_controller->stepFrame(shift ? 10 : 1);
		return true;
	case Qt::Key_Home:
		_controller->setCurrentFrame(_controller->firstFrame());
		return true;
	case Qt::Key_End:
		_controller->setCurrentFrame(_controller->lastFrame());
		return true;
	case Qt::Key_Delete:
	case Qt::Key_Backspace:
		// One press deletes one thing: a held key would go on from the
		// selected keyframes to the layers that own them.
		if (!dragging && !e->isAutoRepeat()) {
			_controller->deleteSelection();
		}
		return true;
	case Qt::Key_Escape:
		// Panels cancel their drags on Escape themselves. A stray Escape
		// never closes the window: it's a whole program with its own
		// document, Cmd+W / the close button do that.
		if (!_controller->selectedKeyframes().empty()) {
			_controller->setSelectedKeyframes({});
			return true;
		} else if (!_controller->selection().empty()) {
			_controller->clearSelection();
			return true;
		} else if (_controller->activeProperty()) {
			_controller->setActiveProperty(std::nullopt);
			return true;
		}
		return false;
	}
	return false;
}

// Validation box.

namespace {

class LinksRow final : public Ui::RpWidget {
public:
	explicit LinksRow(QWidget *parent) : RpWidget(parent) {
	}

	void add(const QString &text, Fn<void()> callback) {
		const auto link = Ui::CreateChild<Ui::LinkButton>(this, text);
		link->setClickedCallback(std::move(callback));
		_links.push_back(link);
		resizeToWidth(width());
	}

	[[nodiscard]] bool empty() const {
		return _links.empty();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		auto left = 0;
		auto height = 0;
		for (const auto &link : _links) {
			link->moveToLeft(left, 0, newWidth);
			left += link->width() + Scaled(16);
			height = std::max(height, link->height());
		}
		return height;
	}

	void paintEvent(QPaintEvent *e) override {
	}

private:
	std::vector<not_null<Ui::LinkButton*>> _links;

};

// A tinted panel with a wrapped text at the top of the check box: what the
// check means for the thing the user is about to do.
class NoticeRow final : public Ui::RpWidget {
public:
	NoticeRow(QWidget *parent, QString text, bool attention)
	: RpWidget(parent)
	, _text(std::move(text))
	, _attention(attention) {
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto padding = Scaled(12);
		const auto inner = std::max(newWidth - 2 * padding, 1);
		const auto height = QFontMetrics(st::semiboldFont->f).boundingRect(
			QRect(0, 0, inner, 1 << 20),
			Qt::TextWordWrap,
			_text).height();
		return height + 2 * Scaled(10);
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto color = _attention
			? st::attentionButtonFg->c
			: st::windowActiveTextFg->c;
		p.setPen(Qt::NoPen);
		p.setBrush(anim::with_alpha(color, 0.1));
		p.drawRoundedRect(QRectF(rect()), Scaled(8), Scaled(8));
		const auto padding = Scaled(12);
		p.setFont(st::semiboldFont);
		p.setPen(_attention ? st::attentionButtonFg : st::windowBoldFg);
		p.drawText(
			rect().marginsRemoved(
				QMargins(padding, Scaled(10), padding, Scaled(10))),
			Qt::TextWordWrap,
			_text);
	}

private:
	const QString _text;
	const bool _attention = false;

};

// A secondary line under an issue (what Telegram does with the feature,
// what a fix changes).
[[nodiscard]] const style::FlatLabel &IssueNoteStyle() {
	static const auto result = [] {
		auto st = st::boxLabel;
		st.textFg = st::windowSubTextFg;
		return st;
	}();
	return result;
}

void FillValidationBox(
		not_null<Ui::GenericBox*> box,
		not_null<EditorController*> controller,
		std::shared_ptr<ValidationArgs> args) {
	const auto content = box->verticalLayout();
	content->clear();
	box->clearButtons();

	const auto document = controller->document();
	const auto result = Validate(document);
	const auto purpose = args->purpose;
	const auto warns = NeedsStickerWarning(result);
	const auto defer = [=](Fn<void()> callback) {
		// The box is rebuilt on document changes, so the clicked link
		// must not be destroyed while its click is being handled.
		return [=] {
			crl::on_main(box, callback);
		};
	};
	const auto toast = [=](const QString &text) {
		if (const auto show = controller->uiShow()) {
			show->showToast(text);
		}
	};
	const auto fix = [=](std::vector<IssueType> types) {
		const auto fixed = controller->autoFix(types);
		if (!fixed) {
			toast(tr::lng_oblivion_lottie_editor_validate_nothing(tr::now));
		} else if (!Validate(controller->document(), {
				.measureSize = false,
			}).ok()) {
			toast(tr::lng_oblivion_lottie_editor_validate_still(tr::now));
		} else {
			toast(tr::lng_oblivion_lottie_editor_validate_fixed(tr::now));
		}
	};

	// Before a sticker goes somewhere: what the list below means for it.
	if (purpose != ValidationPurpose::Check && warns) {
		const auto blocked = !result.ok()
			&& (purpose == ValidationPurpose::StickerPack);
		const auto text = blocked
			? tr::lng_oblivion_lottie_mask_check_pack_errors(tr::now)
			: !result.ok()
			? tr::lng_oblivion_lottie_mask_check_tgs_errors(tr::now)
			: (purpose == ValidationPurpose::StickerPack)
			? tr::lng_oblivion_lottie_mask_check_pack_warnings(tr::now)
			: tr::lng_oblivion_lottie_mask_check_tgs_warnings(tr::now);
		// The buttons below are "continue" and "cancel", a third one does
		// not fit next to them, so the fix is a link under the notice.
		// (With nothing to continue with it is a button, see below.)
		const auto fixLink = result.hasFixable() && !blocked;
		content->add(
			object_ptr<NoticeRow>(content, text, !result.ok()),
			(st::boxRowPadding
				+ QMargins(0, Scaled(4), 0, Scaled(fixLink ? 4 : 8))));
		if (fixLink) {
			const auto links = content->add(
				object_ptr<LinksRow>(content),
				st::boxRowPadding + QMargins(0, Scaled(4), 0, Scaled(4)));
			links->add(
				tr::lng_oblivion_lottie_editor_validate_fix_all(tr::now),
				defer([=] { fix({}); }));
		}
	}
	if (result.issues.empty()) {
		Ui::AddSkip(content);
		content->add(
			object_ptr<Ui::FlatLabel>(
				content,
				tr::lng_oblivion_lottie_editor_validate_ok(),
				st::boxLabel),
			st::boxRowPadding);
	}
	// Section titles start at the left edge of the issue texts (the
	// subsection padding is 2px narrower than the box row padding).
	const auto titleShift = QMargins(
		st::boxRowPadding.left() - st::defaultSubsectionTitlePadding.left(),
		0,
		0,
		0);
	const auto addGroup = [&](
			Fn<bool(const Issue&)> filter,
			QString title,
			QString about = QString()) {
		auto first = true;
		for (const auto &issue : result.issues) {
			if (!filter(issue)) {
				continue;
			}
			if (first) {
				first = false;
				Ui::AddSubsectionTitle(
					content,
					rpl::single(title),
					titleShift);
				if (!about.isEmpty()) {
					content->add(
						object_ptr<Ui::FlatLabel>(
							content,
							about,
							IssueNoteStyle()),
						st::boxRowPadding + QMargins(0, 0, 0, Scaled(10)));
				}
			}
			// An issue and its actions are one group: the links are close
			// to the text, the groups are separated by a larger gap.
			content->add(
				object_ptr<Ui::FlatLabel>(
					content,
					IssueText(document, issue),
					st::boxLabel),
				st::boxRowPadding);
			const auto fixText = issue.fixable
				? IssueFixText(issue.type)
				: QString();
			auto notes = QStringList();
			if (issue.category == IssueCategory::Forbidden
				&& issue.severity == IssueSeverity::Warning
				&& !issue.rendered) {
				notes.push_back(
					tr::lng_oblivion_lottie_mask_note_ignored(tr::now));
			}
			if (!fixText.isEmpty() && issue.fixChangesPicture) {
				notes.push_back(
					tr::lng_oblivion_lottie_mask_note_fix_changes(tr::now));
			}
			if (!notes.isEmpty()) {
				content->add(
					object_ptr<Ui::FlatLabel>(
						content,
						notes.join(QChar(' ')),
						IssueNoteStyle()),
					st::boxRowPadding + QMargins(0, Scaled(2), 0, 0));
			}
			const auto links = content->add(
				object_ptr<LinksRow>(content),
				st::boxRowPadding + QMargins(0, Scaled(2), 0, Scaled(12)));
			if (!fixText.isEmpty()) {
				const auto type = issue.type;
				links->add(fixText, defer([=] {
					fix({ type });
				}));
			}
			if (!issue.nodes.empty()) {
				const auto nodes = issue.nodes;
				links->add(
					tr::lng_oblivion_lottie_editor_validate_select(tr::now),
					defer([=] {
						controller->setSelection(nodes);
						box->closeBox();
					}));
			}
		}
	};
	const auto warning = [](IssueCategory category) {
		return [=](const Issue &issue) {
			return (issue.severity == IssueSeverity::Warning)
				&& (issue.category == category);
		};
	};
	addGroup([](const Issue &issue) {
		return (issue.severity == IssueSeverity::Error);
	}, tr::lng_oblivion_lottie_editor_validate_errors(tr::now));
	addGroup(
		warning(IssueCategory::Forbidden),
		IssueCategoryText(IssueCategory::Forbidden),
		tr::lng_oblivion_lottie_mask_note_rendered(tr::now));
	addGroup(
		warning(IssueCategory::NotRendered),
		IssueCategoryText(IssueCategory::NotRendered));
	// Warnings about the file and plain advice go under one title: as two
	// groups they were "Recommendations" and then "Advice", the same word
	// twice for the reader.
	addGroup([](const Issue &issue) {
		return (issue.severity == IssueSeverity::Warning)
			&& (issue.category == IssueCategory::File
				|| issue.category == IssueCategory::Advice);
	}, tr::lng_oblivion_lottie_editor_validate_warnings(tr::now));
	if (result.packedSize >= 0) {
		// After an issue the gap is already there (under its links).
		if (result.issues.empty()) {
			Ui::AddSkip(content);
		}
		Ui::AddDividerText(
			content,
			tr::lng_oblivion_lottie_editor_validate_size(
				lt_size,
				rpl::single(FormatKilobytes(result.packedSize)),
				lt_limit,
				rpl::single(FormatKilobytes(kTgsMaxPackedSize))));
	}
	if (!result.issues.empty()) {
		// Only .tgs stickers are concerned, the other exports keep all.
		Ui::AddSkip(content);
		content->add(
			object_ptr<Ui::FlatLabel>(
				content,
				tr::lng_oblivion_lottie_mask_check_scope(),
				IssueNoteStyle()),
			st::boxRowPadding);
	}
	Ui::AddSkip(content);

	const auto fixAll = result.hasFixable();
	if (purpose == ValidationPurpose::Check) {
		if (fixAll) {
			box->addButton(
				tr::lng_oblivion_lottie_editor_validate_fix_all(),
				defer([=] { fix({}); }));
		}
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		return;
	}
	// A sticker pack refuses a sticker with errors, a file can be written
	// anyway (the user may want to finish it elsewhere).
	const auto canProceed = result.ok()
		|| (purpose != ValidationPurpose::StickerPack);
	if (!canProceed) {
		// Nothing to continue with: the box only informs, so the fix
		// takes the place of the main button and "Cancel" (of what?)
		// becomes "Close", as in the plain check.
		if (fixAll) {
			box->addButton(
				tr::lng_oblivion_lottie_editor_validate_fix_all(),
				defer([=] { fix({}); }));
		}
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		return;
	}
	auto text = (purpose == ValidationPurpose::StickerPack)
		? (warns
			? tr::lng_oblivion_lottie_mask_check_add_anyway()
			: tr::lng_oblivion_lottie_mask_check_add())
		: (purpose == ValidationPurpose::ExportTgs)
		? (warns
			? tr::lng_oblivion_lottie_mask_check_save_anyway()
			: tr::lng_oblivion_lottie_editor_save())
		: (warns
			? tr::lng_oblivion_lottie_mask_check_continue_anyway()
			: tr::lng_oblivion_lottie_mask_check_continue());
	box->addButton(std::move(text), [=] {
		const auto proceed = args->proceed;
		box->closeBox();
		if (proceed) {
			proceed();
		}
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

} // namespace

bool NeedsStickerWarning(const ValidationResult &result) {
	return ranges::any_of(result.issues, [](const Issue &issue) {
		return (issue.severity == IssueSeverity::Error)
			|| (issue.category == IssueCategory::Forbidden)
			|| (issue.category == IssueCategory::NotRendered);
	});
}

void ValidationBoxFor(
		not_null<Ui::GenericBox*> box,
		not_null<EditorController*> controller,
		ValidationArgs &&args) {
	const auto shared = std::make_shared<ValidationArgs>(std::move(args));
	box->setTitle((shared->purpose == ValidationPurpose::Check)
		? tr::lng_oblivion_lottie_editor_validate_title()
		: tr::lng_oblivion_lottie_mask_check_title());
	box->setWidth(st::boxWideWidth);
	box->setMaxHeight(Scaled(560));
	FillValidationBox(box, controller, shared);
	controller->documentChanged(
	) | rpl::on_next([=] {
		FillValidationBox(box, controller, shared);
	}, box->lifetime());
}

void ValidationBox(
		not_null<Ui::GenericBox*> box,
		not_null<EditorController*> controller) {
	ValidationBoxFor(box, controller, ValidationArgs());
}

void ShowValidationBox(not_null<EditorController*> controller) {
	if (const auto show = controller->uiShow()) {
		show->show(Box(ValidationBox, controller));
	}
}

void CheckBeforeSticker(
		not_null<EditorController*> controller,
		ValidationPurpose purpose,
		Fn<void()> proceed) {
	if (!controller->uiShow()) {
		if (proceed) {
			proceed();
		}
		return;
	}
	const auto document = controller->document();
	const auto weak = base::make_weak(controller.get());
	crl::async([=] {
		auto result = Validate(document);
		crl::on_main(weak, [=, result = std::move(result)] {
			const auto show = weak->uiShow();
			if (!show || !NeedsStickerWarning(result)) {
				if (proceed) {
					proceed();
				}
				return;
			}
			show->show(Box(ValidationBoxFor, weak.get(), ValidationArgs{
				.purpose = purpose,
				.proceed = proceed,
			}));
		});
	});
}

// EditorWindow.

EditorWindow::EditorWindow(EditorArgs &&args)
: _controller(std::make_unique<EditorController>())
, _layers(std::make_unique<Ui::LayerManager>(body()))
, _done(std::move(args.done)) {
	_layers->setHideByBackgroundClick(true);
	_controller->setShow(_layers->uiShow());

	setMinimumSize(QSize(Scaled(kWindowMinWidth), Scaled(kWindowMinHeight)));
	setGeometry(DefaultWindowGeometry());

	body()->paintRequest() | rpl::on_next([=](QRect clip) {
		QPainter(body().get()).fillRect(clip, st::windowBg);
	}, body()->lifetime());

	_editor = base::make_unique_q<EditorWidget>(
		body().get(),
		_controller.get(),
		_done ? args.doneText : QString());
	body()->sizeValue() | rpl::on_next([=](QSize size) {
		_editor->setGeometry(QRect(QPoint(), size));
	}, _editor->lifetime());

	load(std::move(args.data), std::move(args.name), std::move(args.path));

	_controller->actionRequests(
	) | rpl::on_next([=](EditorAction action) {
		handleAction(action);
	}, lifetime());

	rpl::merge(
		_controller->nameValue() | rpl::to_empty,
		_controller->dirtyValue() | rpl::to_empty
	) | rpl::on_next([=] {
		updateTitle();
	}, lifetime());

	_editor->doneRequests() | rpl::on_next([=] {
		if (!_done) {
			return;
		}
		CheckBeforeSticker(
			_controller.get(),
			ValidationPurpose::Done,
			crl::guard(this, [=] {
				const auto document = _controller->document();
				const auto name = _controller->name();
				const auto done = _done;
				crl::async([=] {
					auto tgs = document.toTgs();
					crl::on_main([=, tgs = std::move(tgs)] {
						if (tgs.isEmpty()) {
							ShowAppToast(
								tr::lng_oblivion_lottie_editor_save_failed(
									tr::now));
						} else {
							done(tgs, name);
						}
					});
				});
			}));
	}, lifetime());

	setupShortcuts();
}

EditorWindow::~EditorWindow() {
	// The panels observe the controller, destroy them first.
	_editor = nullptr;
}

not_null<EditorController*> EditorWindow::controller() const {
	return _controller.get();
}

not_null<EditorWidget*> EditorWindow::editor() const {
	return _editor.get();
}

std::shared_ptr<Ui::Show> EditorWindow::uiShow() {
	return _layers->uiShow();
}

void EditorWindow::load(QByteArray data, QString name, QString path) {
	auto error = QString();
	auto document = data.isEmpty()
		? Document()
		: Document::FromData(data, &error);
	const auto failed = !data.isEmpty() && !document.valid();
	if (!document.valid()) {
		document = Document::Blank();
	}
	if (failed) {
		LOG(("Oblivion LottieEdit: could not open \"%1\": %2"
			).arg(path.isEmpty() ? name : path
			).arg(error));
		path = QString();
		name = QString();
		crl::on_main(this, [=] {
			uiShow()->showToast(
				tr::lng_oblivion_lottie_editor_open_failed(tr::now));
		});
	}
	if (name.trimmed().isEmpty()) {
		name = !path.isEmpty()
			? QFileInfo(path).completeBaseName()
			: data.isEmpty()
			? tr::lng_oblivion_lottie_editor_new_name(tr::now)
			: document.name().trimmed();
		if (name.isEmpty()) {
			name = tr::lng_oblivion_lottie_editor_new_name(tr::now);
		}
	}
	_controller->load(std::move(document), std::move(name), std::move(path));
}

void EditorWindow::updateTitle() {
	const auto name = _controller->name().isEmpty()
		? tr::lng_oblivion_lottie_editor_new_name(tr::now)
		: _controller->name();
	setTitle(tr::lng_oblivion_lottie_editor_window_title(
		tr::now,
		lt_name,
		(_controller->dirty() ? u"• "_q : QString()) + name));
}

void EditorWindow::setupShortcuts() {
	using Command = Shortcuts::Command;
	// Quitting from anywhere (Cmd+Q in any window) first asks about the
	// unsaved changes of every editor window, one by one.
	Shortcuts::Requests(
	) | rpl::on_next([=](not_null<Shortcuts::Request*> request) {
		if (!_controller->dirty() || _closeConfirmed) {
			return;
		}
		request->check(Command::Quit, 1) && request->handle([] {
			if (!PreventsQuit()) {
				Core::Quit();
			}
			return true;
		});
	}, lifetime());

	Shortcuts::Requests(
	) | rpl::filter([=] {
		return isActiveWindow();
	}) | rpl::on_next([=](not_null<Shortcuts::Request*> request) {
		request->check(Command::Close, 1) && request->handle([=] {
			requestClose();
			return true;
		});
		request->check(Command::Minimize, 1) && request->handle([=] {
			showMinimized();
			return true;
		});
		request->check(Command::ChatSelf, 1) && request->handle([=] {
			// Cmd+0 is taken by "Saved Messages", here it fits the canvas.
			_controller->setZoom(1.);
			_controller->setPan(QPointF());
			return true;
		});
		// Chat navigation, search, voice recording... make no sense here
		// and must not act on the main window behind the editor.
		const auto from = int(Command::Search);
		const auto till = int(Command::ShowAdminLog);
		for (auto i = from; i <= till; ++i) {
			const auto command = Command(i);
			if (command == Command::ChatSelf) {
				continue;
			}
			request->check(command, 1) && request->handle([] {
				return true;
			});
		}
	}, lifetime());
}

bool EditorWindow::eventHook(QEvent *e) {
	const auto type = e->type();
	if (type == QEvent::Close) {
		if (!_closeConfirmed && _controller->dirty()) {
			e->ignore();
			requestClose();
			return true;
		}
		e->accept();
		_controller->setPlaying(false);
		if (!isMinimized() && !isFullScreen()) {
			Layout().geometry = geometry();
		}
		auto closed = base::take(_closed);
		crl::on_main(this, [=] {
			// The lambda (and closed) outlives the window it destroys.
			const auto after = closed;
			RemoveWindow(this);
			if (after) {
				after();
			}
		});
		return true;
	} else if (type == QEvent::ShortcutOverride) {
		// No focused widget inside the editor (after a box was closed, for
		// example): the window itself gets the editor's keys.
		const auto key = static_cast<QKeyEvent*>(e);
		if (!_layers->topShownLayer() && _editor->claimsShortcut(key)) {
			e->accept();
			return true;
		}
	}
	return Ui::RpWindow::eventHook(e);
}

void EditorWindow::keyPressEvent(QKeyEvent *e) {
	// Keys from inside the editor were already offered to handleKey() by
	// EditorWidget::keyPressEvent(), they get here only when unhandled.
	const auto focused = focusWidget();
	const auto fromEditor = focused
		&& (focused == _editor.get() || _editor->isAncestorOf(focused));
	if (_layers->topShownLayer() || fromEditor || !_editor->handleKey(e)) {
		Ui::RpWindow::keyPressEvent(e);
	}
}

void EditorWindow::closeConfirmed(Fn<void()> closed) {
	_closeConfirmed = true;
	_closed = std::move(closed);
	close();
}

void EditorWindow::requestClose(Fn<void()> closed) {
	if (_closeConfirmed || !_controller->dirty()) {
		closeConfirmed(std::move(closed));
		return;
	}
	ActivateWindow(this);
	const auto name = _controller->name().isEmpty()
		? tr::lng_oblivion_lottie_editor_new_name(tr::now)
		: _controller->name();
	uiShow()->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_oblivion_lottie_editor_close_title());
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_oblivion_lottie_editor_close_text(
					lt_name,
					rpl::single(name)),
				st::boxLabel),
			st::boxPadding);
		box->addButton(tr::lng_oblivion_lottie_editor_save(), [=] {
			box->closeBox();
			// Closes only when the file is really written (not when the
			// "Save as" dialog is cancelled or the write fails).
			save([=] {
				closeConfirmed(closed);
			});
		});
		box->addButton(tr::lng_cancel(), [=] {
			box->closeBox();
		});
		box->addLeftButton(
			tr::lng_oblivion_lottie_editor_close_discard(),
			[=] { closeConfirmed(closed); },
			st::attentionBoxButton);
	}), Ui::LayerOption::CloseOther);
}

void EditorWindow::forceClose() {
	closeConfirmed(nullptr);
}

void EditorWindow::handleAction(EditorAction action) {
	switch (action) {
	case EditorAction::New: createNew(); break;
	case EditorAction::Open: open(); break;
	case EditorAction::Save: save(); break;
	case EditorAction::SaveAs: saveAs(); break;
	case EditorAction::ExportTgs:
		// A .tgs is a Telegram sticker: the check comes first when there
		// is something to warn about. JSON keeps everything, no questions.
		CheckBeforeSticker(
			_controller.get(),
			ValidationPurpose::ExportTgs,
			crl::guard(this, [=] { exportAs(true); }));
		break;
	case EditorAction::ExportJson: exportAs(false); break;
	case EditorAction::Close: requestClose(); break;
	}
}

bool EditorWindow::untouchedBlank() const {
	return _controller->filePath().isEmpty()
		&& !_controller->dirty()
		&& !_controller->canUndo()
		&& !_controller->canRedo()
		&& _controller->document().layers().empty();
}

void EditorWindow::createNew() {
	if (untouchedBlank()) {
		return;
	}
	OpenLottieEditor({});
}

void EditorWindow::open() {
	FileDialog::GetOpenPath(
		this,
		tr::lng_oblivion_lottie_editor_open_title(tr::now),
		OpenFilter(),
		crl::guard(this, [=](FileDialog::OpenResult &&result) {
			auto file = ReadOpenResult(result);
			if (file.failed) {
				uiShow()->showToast(
					tr::lng_oblivion_lottie_editor_read_failed(tr::now));
				return;
			} else if (file.bytes.isEmpty()) {
				return;
			}
			auto error = QString();
			if (!Document::FromData(file.bytes, &error).valid()) {
				LOG(("Oblivion LottieEdit: could not open \"%1\": %2"
					).arg(file.path
					).arg(error));
				uiShow()->showToast(
					tr::lng_oblivion_lottie_editor_open_failed(tr::now));
				return;
			}
			for (const auto &window : Windows()) {
				if (SamePath(window->controller()->filePath(), file.path)) {
					ActivateWindow(window.get());
					return;
				}
			}
			if (untouchedBlank()) {
				load(
					std::move(file.bytes),
					std::move(file.name),
					std::move(file.path));
			} else {
				OpenLottieEditor({
					.data = std::move(file.bytes),
					.name = std::move(file.name),
					.path = std::move(file.path),
				});
			}
		}));
}

void EditorWindow::save(Fn<void()> saved) {
	const auto path = _controller->filePath();
	if (path.isEmpty()) {
		saveAs(std::move(saved));
		return;
	}
	write(path, !IsJsonPath(path), true, std::move(saved));
}

void EditorWindow::saveAs(Fn<void()> saved) {
	const auto current = _controller->filePath();
	const auto json = IsJsonPath(current);
	const auto suggested = current.isEmpty()
		? SuggestedPath(SafeFileName(_controller->name()) + u".tgs"_q)
		: current;
	const auto filter = json
		? (JsonFilter() + u";;"_q + TgsFilter())
		: (TgsFilter() + u";;"_q + JsonFilter());
	FileDialog::GetWritePath(
		this,
		tr::lng_oblivion_lottie_editor_save_title(tr::now),
		filter,
		suggested,
		crl::guard(this, [=](QString &&result) {
			if (result.isEmpty()) {
				return;
			}
			const auto isJson = IsJsonPath(result);
			const auto path = isJson
				? result
				: WithExtension(result, u"tgs"_q);
			write(path, !isJson, true, saved);
		}));
}

void EditorWindow::exportAs(bool tgs) {
	const auto extension = tgs ? u"tgs"_q : u"json"_q;
	FileDialog::GetWritePath(
		this,
		tr::lng_oblivion_lottie_editor_save_title(tr::now),
		tgs ? TgsFilter() : JsonFilter(),
		SuggestedPath(SafeFileName(_controller->name()) + '.' + extension),
		crl::guard(this, [=](QString &&result) {
			if (!result.isEmpty()) {
				write(WithExtension(result, extension), tgs, false, nullptr);
			}
		}));
}

void EditorWindow::write(
		QString path,
		bool tgs,
		bool asDocument,
		Fn<void()> saved) {
	if (_saving) {
		return;
	}
	_saving = true;
	const auto document = _controller->document();
	const auto weak = QPointer<EditorWindow>(this);
	crl::async([=] {
		const auto bytes = tgs ? document.toTgs() : document.toJson();
		auto written = false;
		if (!bytes.isEmpty()) {
			auto file = QSaveFile(path);
			written = file.open(QIODevice::WriteOnly)
				&& (file.write(bytes) == bytes.size())
				&& file.commit();
		}
		auto problem = std::optional<Issue>();
		if (written && tgs) {
			const auto validation = Validate(document);
			for (const auto &issue : validation.issues) {
				if (issue.severity == IssueSeverity::Error) {
					problem = issue;
					break;
				}
			}
		}
		crl::on_main(weak, [=] {
			// The texts are localized, on the main thread only.
			_saving = false;
			if (!written) {
				LOG(("Oblivion LottieEdit: could not write \"%1\"."
					).arg(path));
				uiShow()->showToast(
					tr::lng_oblivion_lottie_editor_save_failed(tr::now));
				return;
			}
			if (asDocument) {
				_controller->setFilePath(path);
				_controller->setName(QFileInfo(path).completeBaseName());
				if (_controller->document().sameAs(document)) {
					_controller->markSaved();
				}
			}
			uiShow()->showToast(!problem
				? tr::lng_oblivion_saved_to(
					tr::now,
					lt_path,
					QDir::toNativeSeparators(path))
				: tr::lng_oblivion_lottie_editor_saved_warning(
					tr::now,
					lt_problem,
					IssueText(document, *problem)));
			if (saved) {
				saved();
			}
		});
	});
}

// Entry points.

not_null<EditorWindow*> OpenLottieEditor(EditorArgs &&args) {
	if (!args.path.isEmpty()) {
		for (const auto &window : Windows()) {
			if (SamePath(window->controller()->filePath(), args.path)) {
				if (!WindowsLock().locked) {
					ActivateWindow(window.get());
				}
				return window.get();
			}
		}
	}
	auto &list = Windows();
	list.push_back(std::make_unique<EditorWindow>(std::move(args)));
	const auto result = list.back().get();
	if (WindowsLock().locked) {
		WindowsLock().hidden.push_back(result);
		return result;
	}
	result->show();
	ActivateWindow(result);
	result->editor()->setFocus();
	return result;
}

void ShowLottieEditor(QByteArray lottieData, QString name) {
	OpenLottieEditor({
		.data = std::move(lottieData),
		.name = std::move(name),
	});
}

void ShowLottieEditorImport() {
	FileDialog::GetOpenPath(
		ActiveAppWindow(),
		tr::lng_oblivion_lottie_editor_open_title(tr::now),
		OpenFilter(),
		[](FileDialog::OpenResult &&result) {
			auto file = ReadOpenResult(result);
			if (file.failed) {
				ShowAppToast(
					tr::lng_oblivion_lottie_editor_read_failed(tr::now));
				return;
			} else if (file.bytes.isEmpty()) {
				return;
			}
			auto error = QString();
			if (!Document::FromData(file.bytes, &error).valid()) {
				LOG(("Oblivion LottieEdit: could not open \"%1\": %2"
					).arg(file.path
					).arg(error));
				ShowAppToast(
					tr::lng_oblivion_lottie_editor_open_failed(tr::now));
				return;
			}
			OpenLottieEditor({
				.data = std::move(file.bytes),
				.name = std::move(file.name),
				.path = std::move(file.path),
			});
		});
}

namespace {

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
	if (file.size() < Storage::kMaxFileInMemory
		&& file.open(QIODevice::ReadOnly)) {
		bytes = file.readAll();
		file.close();
	}
	location.accessDisable();
	return bytes;
}

[[nodiscard]] QString ProgressText(float64 progress) {
	return QString::number(int(std::round(
		std::clamp(progress, 0., 1.) * 100))) + '%';
}

// "Downloading… 42%" with Cancel, closes itself and opens the editor when
// the document is loaded. Lives in the chat window's layer, so it goes
// away with the session.
void DownloadBox(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<Ui::Show> show,
		not_null<DocumentData*> document,
		Fn<bool()> open) {
	struct State {
		rpl::variable<QString> status;
		bool finished = false;
	};
	const auto state = box->lifetime().make_state<State>();
	box->setTitle(tr::lng_oblivion_lottie_editor_title());
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			state->status.value(),
			st::boxLabel),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });

	const auto updateStatus = [=] {
		state->status = tr::lng_oblivion_studio_downloading(
			tr::now,
			lt_percent,
			ProgressText(document->progress()));
	};
	const auto check = [=] {
		if (state->finished) {
			return;
		} else if (open()) {
			state->finished = true;
			box->closeBox();
		} else if (document->loading()) {
			updateStatus();
		} else {
			state->finished = true;
			show->showToast(
				tr::lng_oblivion_lottie_editor_download_failed(tr::now));
			box->closeBox();
		}
	};
	document->session().data().documentLoadProgress(
	) | rpl::filter([=](not_null<DocumentData*> loaded) {
		return (loaded == document);
	}) | rpl::on_next(check, box->lifetime());
	updateStatus();
}

} // namespace

void ShowLottieEditorFor(
		std::shared_ptr<Ui::Show> show,
		not_null<DocumentData*> document,
		Data::FileOrigin origin,
		QString name) {
	if (!IsLottieDocument(document)) {
		show->showToast(tr::lng_oblivion_lottie_editor_open_failed(tr::now));
		return;
	} else if (document->size >= Storage::kMaxFileInMemory) {
		show->showToast(tr::lng_oblivion_lottie_editor_read_failed(tr::now));
		return;
	}
	if (name.trimmed().isEmpty()) {
		name = QFileInfo(document->filename()).completeBaseName();
	}
	const auto media = document->createMediaView();
	const auto open = [=] {
		if (!media->loaded(true)) {
			return false;
		}
		auto bytes = ReadDocumentBytes(document, media);
		if (bytes.isEmpty()) {
			return false;
		}
		// Parsed in the window: a broken file opens a blank animation with
		// an error toast there, nothing is left half-open here.
		ShowLottieEditor(std::move(bytes), name);
		return true;
	};
	if (open()) {
		return;
	} else if (!document->loading()) {
		document->save(
			origin ? origin : document->stickerSetOrigin(),
			QString());
		if (open()) {
			return;
		} else if (!document->loading()) {
			show->showToast(
				tr::lng_oblivion_lottie_editor_download_failed(tr::now));
			return;
		}
	}
	show->showBox(Box(DownloadBox, show, document, open));
}

void AddGiftLottieEditorActions(
		not_null<Ui::PopupMenu*> menu,
		std::shared_ptr<Ui::Show> show,
		std::shared_ptr<Data::UniqueGift> unique) {
	if (!unique) {
		return;
	}
	const auto model = unique->model.document;
	const auto pattern = unique->pattern.document;
	const auto modelName = unique->title + ' ' + unique->model.name;
	const auto patternName = unique->title + ' ' + unique->pattern.name;
	const auto addAction = Ui::Menu::CreateAddActionCallback(menu);
	addAction(Ui::Menu::MenuCallback::Args{
		.text = tr::lng_oblivion_tools_lottie_editor(tr::now),
		.handler = nullptr,
		.icon = &st::menuIconDraw,
		.fillSubmenu = [=](not_null<Ui::PopupMenu*> submenu) {
			submenu->addAction(tr::lng_gift_unique_model(tr::now), [=] {
				ShowLottieEditorFor(show, model, {}, modelName);
			}, &st::menuIconEdit);
			submenu->addAction(tr::lng_gift_unique_symbol(tr::now), [=] {
				ShowLottieEditorFor(show, pattern, {}, patternName);
			}, &st::menuIconDraw);
		},
	});
}

bool PreventsQuit() {
	for (const auto &window : Windows()) {
		if (!window->controller()->dirty()) {
			continue;
		} else if (WindowsLock().locked) {
			// Asking in the editor window would show it over the lock.
			const auto primary = Core::App().activePrimaryWindow();
			if (!primary) {
				return false;
			}
			primary->activate();
			primary->show(Box([=](not_null<Ui::GenericBox*> box) {
				box->addRow(
					object_ptr<Ui::FlatLabel>(
						box,
						tr::lng_oblivion_lottie_editor_locked_quit(),
						st::boxLabel),
					st::boxPadding);
				box->addButton(tr::lng_cancel(), [=] {
					box->closeBox();
				});
				box->addLeftButton(
					tr::lng_oblivion_lottie_editor_close_discard(),
					[=] {
						box->closeBox();
						CloseAllWindows();
						Core::Quit();
					},
					st::attentionBoxButton);
			}), Ui::LayerOption::CloseOther);
			return true;
		}
		window->requestClose([] {
			// Asks about the next window with changes, if any.
			if (!PreventsQuit()) {
				Core::Quit();
			}
		});
		return true;
	}
	return false;
}

void HideWindowsForLock() {
	auto &lock = WindowsLock();
	lock.locked = true;
	for (const auto &window : Windows()) {
		if (!window->isHidden()) {
			window->controller()->setPlaying(false);
			window->hide();
			lock.hidden.push_back(window.get());
		}
	}
}

void RestoreWindowsAfterLock() {
	auto &lock = WindowsLock();
	lock.locked = false;
	for (const auto &window : base::take(lock.hidden)) {
		if (window) {
			window->show();
		}
	}
}

void CloseAllWindows() {
	auto windows = base::take(Windows());
	windows.clear();
}

// Snapshot scenes (OBLIVION_SELFTEST=ui, see oblivion_ui_snapshots.h).

namespace {

// Owns a controller with the bundled sample animation and one editor
// widget / panel built by the factory, sized to the host.
class SceneHost final : public Ui::RpWidget {
public:
	using Factory = Fn<QWidget*(
		QWidget *parent,
		not_null<EditorController*> controller)>;

	SceneHost(QWidget *parent, Factory factory, bool sample = true)
	: RpWidget(parent)
	, _controller(std::make_unique<EditorController>()) {
		if (sample) {
			auto name = QString();
			auto document = SampleDocument(&name);
			_controller->load(std::move(document), name);
		}
		_controller->setShow(SelfTest::SceneShow(this));
		_content = base::unique_qptr<QWidget>(
			factory(this, _controller.get()));
		sizeValue() | rpl::on_next([=](QSize size) {
			if (_content) {
				_content->setGeometry(QRect(QPoint(), size));
			}
		}, lifetime());
	}
	~SceneHost() {
		_content = nullptr;
	}

	[[nodiscard]] not_null<EditorController*> controller() const {
		return _controller.get();
	}
	[[nodiscard]] QWidget *content() const {
		return _content.get();
	}

private:
	const std::unique_ptr<EditorController> _controller;
	base::unique_qptr<QWidget> _content;

};

[[nodiscard]] SceneHost *Host(not_null<QWidget*> widget) {
	return static_cast<SceneHost*>(widget.get());
}

// The root layer with the most animated properties (the most interesting
// one to show in the timeline).
[[nodiscard]] NodeId MostAnimatedLayer(const Document &document) {
	auto result = NodeId();
	auto best = -1;
	for (const auto layer : document.layers()) {
		const auto count = int(document.animatedProperties(layer).size());
		if (count > best) {
			best = count;
			result = layer;
		}
	}
	return result;
}

// Selects the most animated layer, the keyframes of its first animated
// property and moves to `part` of the animation.
void PrepareAnimatedState(
		not_null<EditorController*> controller,
		double part,
		bool keyframes) {
	const auto &document = controller->document();
	const auto layer = MostAnimatedLayer(document);
	if (layer) {
		controller->select(layer);
		const auto animated = document.animatedProperties(layer);
		if (keyframes && !animated.empty()) {
			const auto ref = animated.front();
			auto refs = std::vector<KeyframeRef>();
			for (const auto time : document.keyframeTimes(ref)) {
				refs.push_back({ .property = ref, .time = time });
			}
			controller->setActiveProperty(ref);
			controller->setSelectedKeyframes(std::move(refs));
		}
	}
	controller->setCurrentFrame(controller->firstFrame()
		+ int(std::round((controller->frameCount() - 1) * part)));
}

[[nodiscard]] bool EditorReady(not_null<QWidget*> widget) {
	const auto editor = static_cast<EditorWidget*>(Host(widget)->content());
	return editor
		&& editor->canvas()->frameReady()
		&& editor->validation().has_value();
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;

	const auto editor = [](
			QWidget *parent,
			not_null<EditorController*> controller) -> QWidget* {
		return Ui::CreateChild<EditorWidget>(parent, controller);
	};

	// The canvas background is remembered for the app session, every scene
	// sets its own so that the scene order doesn't change the pictures.
	const auto background = [](
			not_null<QWidget*> widget,
			CanvasBackground background) {
		const auto host = Host(widget);
		if (const auto editor = static_cast<EditorWidget*>(host->content())) {
			editor->canvas()->setBackground(background);
		}
	};

	// The whole editor: a layer selected in the middle of the animation.
	RegisterScene(SceneDescriptor{
		.name = u"lottie_editor"_q,
		.size = QSize(1200, 760),
		.create = [=](not_null<Ui::RpWidget*> parent) {
			return Ui::CreateChild<SceneHost>(parent.get(), editor);
		},
		.prepare = [=](not_null<QWidget*> widget) {
			background(widget, CanvasBackground::Checker);
			PrepareAnimatedState(Host(widget)->controller(), 0.5, false);
		},
		.ready = EditorReady,
	});

	// Keyframes selected, dark canvas background, zoomed in canvas.
	RegisterScene(SceneDescriptor{
		.name = u"lottie_editor_keyframes"_q,
		.size = QSize(1200, 760),
		.create = [=](not_null<Ui::RpWidget*> parent) {
			return Ui::CreateChild<SceneHost>(parent.get(), editor);
		},
		.prepare = [=](not_null<QWidget*> widget) {
			const auto host = Host(widget);
			const auto editor = static_cast<EditorWidget*>(host->content());
			PrepareAnimatedState(host->controller(), 0.3, true);
			background(widget, CanvasBackground::Dark);
			if (editor) {
				editor->canvas()->zoomBy(1.4);
			}
		},
		.ready = EditorReady,
	});

	// A new empty animation: empty states of every panel, the toolbar at a
	// narrow width (the info and the zoom percent are dropped, the name is
	// elided when the tool buttons leave it less room than it needs).
	RegisterScene(SceneDescriptor{
		.name = u"lottie_editor_blank"_q,
		.size = QSize(1000, 640),
		.create = [=](not_null<Ui::RpWidget*> parent) {
			return Ui::CreateChild<SceneHost>(parent.get(), editor, false);
		},
		.prepare = [=](not_null<QWidget*> widget) {
			background(widget, CanvasBackground::Checker);
		},
		.ready = EditorReady,
	});

	// The canvas alone at a bigger size: selection box and anchor on a
	// light background.
	RegisterScene(SceneDescriptor{
		.name = u"lottie_editor_canvas"_q,
		.size = QSize(720, 560),
		.create = [](not_null<Ui::RpWidget*> parent) {
			return Ui::CreateChild<SceneHost>(parent.get(), [](
					QWidget *parent,
					not_null<EditorController*> controller) -> QWidget* {
				return Ui::CreateChild<CanvasPanel>(parent, controller);
			});
		},
		.prepare = [](not_null<QWidget*> widget) {
			const auto host = Host(widget);
			PrepareAnimatedState(host->controller(), 0.5, false);
			if (const auto canvas = static_cast<CanvasPanel*>(
					host->content())) {
				canvas->setBackground(CanvasBackground::Light);
			}
		},
		.ready = [](not_null<QWidget*> widget) {
			const auto canvas = static_cast<CanvasPanel*>(
				Host(widget)->content());
			return canvas && canvas->frameReady();
		},
	});

	// The timeline alone: an expanded layer with selected keyframes.
	RegisterScene(SceneDescriptor{
		.name = u"lottie_editor_timeline"_q,
		.size = QSize(1100, 340),
		.create = [](not_null<Ui::RpWidget*> parent) {
			return Ui::CreateChild<SceneHost>(parent.get(), [](
					QWidget *parent,
					not_null<EditorController*> controller) -> QWidget* {
				return Ui::CreateChild<TimelinePanel>(parent, controller);
			});
		},
		.prepare = [](not_null<QWidget*> widget) {
			PrepareAnimatedState(Host(widget)->controller(), 0.25, true);
		},
	});

	RegisterBoxScene(u"lottie_editor_validation"_q, QSize(480, 0), [](
			std::shared_ptr<Ui::Show> show) {
		struct State {
			std::unique_ptr<EditorController> controller;
		};
		auto document = Document::FromJson(
			R"({"v":"5.5.2","fr":25,"ip":0,"op":100,"w":400,"h":300,)"
			R"("layers":[{"ty":1,"ind":1,"sc":"#ff0000","sw":400,)"
			R"("sh":300,"ip":0,"op":100,"st":0,"sr":1,"ks":{}}]})");
		const auto state = std::make_shared<State>(State{
			.controller = std::make_unique<EditorController>(
				std::move(document),
				u"sample"_q),
		});
		state->controller->setShow(show);
		return Box([=](not_null<Ui::GenericBox*> box) {
			box->lifetime().add([state] {});
			ValidationBox(box, state->controller.get());
		});
	});

	// Before "Add to sticker pack": a sticker-sized animation with the
	// After Effects features Telegram does not accept or does not draw
	// (a mask with an inverted flag and an opacity, a track matte, a
	// repeater, a star, a gradient stroke), the "add anyway" button.
	RegisterBoxScene(u"lottie_validation_telegram"_q, QSize(480, 0), [](
			std::shared_ptr<Ui::Show> show) {
		struct State {
			std::unique_ptr<EditorController> controller;
		};
		auto document = Document::Blank();
		const auto apply = [&](Edit &&edit) {
			auto created = edit.created;
			if (edit.ok()) {
				document = std::move(edit.document);
			} else {
				created.clear();
			}
			return created;
		};
		apply(AddLayer(
			document,
			LayerTemplate::Shape,
			u"Круг"_q,
			0,
			0,
			ShapeTemplate::Ellipse));
		apply(AddLayer(
			document,
			LayerTemplate::Shape,
			u"Звезда"_q,
			0,
			1,
			ShapeTemplate::Star));
		const auto layers = document.layers();
		if (layers.size() >= 2) {
			const auto matte = layers[0];
			const auto star = layers[1];
			apply(SetTrackMatte(document, star, MatteMode::Alpha, matte));
			const auto masks = apply(AddMask(
				document,
				star,
				DefaultMaskPath(document, star, 0.)));
			if (!masks.empty()) {
				apply(SetMaskInverted(document, masks.front(), true));
				apply(SetValueAt(
					document,
					PropertyRef{ masks.front(), QByteArray("o") },
					PropValue::Scalar(60.),
					0.));
			}
			const auto node = document.node(star);
			if (node && !node->children.empty()) {
				const auto group = node->children.front();
				apply(AddShape(
					document,
					group,
					ShapeTemplate::Repeater,
					ShapeTypeText(ShapeType::Repeater)));
				apply(AddShape(
					document,
					group,
					ShapeTemplate::GradientStroke,
					ShapeTypeText(ShapeType::GradientStroke)));
			}
		}
		const auto state = std::make_shared<State>(State{
			.controller = std::make_unique<EditorController>(
				std::move(document),
				u"sample"_q),
		});
		state->controller->setShow(show);
		return Box([=](not_null<Ui::GenericBox*> box) {
			box->lifetime().add([state] {});
			ValidationBoxFor(box, state->controller.get(), ValidationArgs{
				.purpose = ValidationPurpose::StickerPack,
				.proceed = [] {},
			});
		});
	});

	// The same check with errors (a wrong canvas, frame rate and duration,
	// a mask with a mode Telegram skips).
	const auto withErrors = [](
			std::shared_ptr<Ui::Show> show,
			ValidationPurpose purpose) {
		struct State {
			std::unique_ptr<EditorController> controller;
		};
		auto document = Document::FromJson(
			R"({"v":"5.5.2","fr":25,"ip":0,"op":100,"w":400,"h":300,)"
			R"("layers":[{"ty":4,"ind":1,"ddd":0,"ip":0,"op":100,"st":0,)"
			R"("sr":1,"hasMask":true,"masksProperties":[{"mode":"l",)"
			R"("inv":false,"pt":{"a":0,"k":{"c":true,"v":[[0,0],[100,0],)"
			R"([100,100]],"i":[[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],)"
			R"([0,0]]}},"o":{"a":0,"k":100}}],"ks":{},"shapes":[]}]})");
		const auto state = std::make_shared<State>(State{
			.controller = std::make_unique<EditorController>(
				std::move(document),
				u"sample"_q),
		});
		state->controller->setShow(show);
		return Box([=](not_null<Ui::GenericBox*> box) {
			box->lifetime().add([state] {});
			ValidationBoxFor(box, state->controller.get(), ValidationArgs{
				.purpose = purpose,
				.proceed = [] {},
			});
		});
	};

	// A sticker pack can't take it, there is nothing to continue with:
	// "Fix automatically" and "Close".
	RegisterBoxScene(u"lottie_validation_blocked"_q, QSize(480, 0), [=](
			std::shared_ptr<Ui::Show> show) {
		return withErrors(show, ValidationPurpose::StickerPack);
	});

	// "Export .tgs" of the same animation: the file can be written anyway,
	// the fix is a link under the notice.
	RegisterBoxScene(u"lottie_validation_tgs"_q, QSize(480, 0), [=](
			std::shared_ptr<Ui::Show> show) {
		return withErrors(show, ValidationPurpose::ExportTgs);
	});
});

} // namespace

} // namespace Oblivion::LottieEdit
