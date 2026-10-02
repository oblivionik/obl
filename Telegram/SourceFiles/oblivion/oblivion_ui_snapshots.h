/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"

// UI snapshots: registered widget scenes are rendered offscreen into PNG
// files, so the UI can be checked without a visible (or unlocked) screen.
//
// Run it through the self-test mode (see oblivion_selftest.h), always with
// a separate empty -workdir (it is refused without one):
//
// mkdir -p /private/tmp/claude-501/oblivion-snap-wd
// OBLIVION_SELFTEST=ui
//   OBLIVION_SELFTEST_OUT_DIR=/private/tmp/claude-501/oblivion-snap
//   out/Release/Oblivion.app/Contents/MacOS/Oblivion
//   -workdir /private/tmp/claude-501/oblivion-snap-wd
//
// Optional environment:
//   OBLIVION_SELFTEST_OUT_DIR    PNG folder, default <workdir>/snapshots.
//   OBLIVION_SELFTEST_SCENES     comma separated scene names, * and ? work,
//                                a leading - excludes: "sticker_*,-*_big".
//   OBLIVION_SELFTEST_THEMES     day,night (default); dayblue, tinted too.
//   OBLIVION_SELFTEST_LANG       ru (default) or en.
//   OBLIVION_SELFTEST_LANG_FILE  an exported .strings language pack, its
//                                values are applied over the built-in ones.
//   OBLIVION_SELFTEST_WAIT       event processing per scene in ms, 1500.
//   OBLIVION_SELFTEST_SCALE      interface scale in percent, 100.
//   OBLIVION_SELFTEST_RATIO      device pixel ratio of the images, 2.
//   OBLIVION_SELFTEST_OUT        the text report (see oblivion_selftest.h).
//
// Output: <out dir>/<scene>_<theme>.png for every scene and theme, and
// <out dir>/snapshots.txt with one line per image, rewritten before and
// after each one: a scene that crashed the process stays "RUNNING" there,
// while the images rendered before it are kept. "ui" is not a part of
// OBLIVION_SELFTEST=all, it has to be requested by name.
//
// What the scenes get: Core::App() with default settings (no local
// storage is read or written), styles at the requested scale, fonts,
// palette (day or night), emoji, animations, crl, the language chosen in
// OBLIVION_SELFTEST_LANG. With "ru" the Oblivion Russian strings from
// oblivion_lang.cpp apply exactly as in the app; the Russian cloud language
// pack for the rest of Telegram strings is not available offline, so only a
// small built-in stand-in for common buttons (Cancel, Save, Close, OK...)
// is applied, other upstream strings stay English unless a full pack is
// given in OBLIVION_SELFTEST_LANG_FILE.
//
// What they don't get: accounts, Main::Session, Window::Controller or
// Window::SessionController, main window, network (MTP), local storage,
// audio playback (Media::Player is not started), notifications, the media
// viewer. Core::App().activeWindow() is null, Core::App().domain() has no
// accounts. OpenGL surfaces don't render in a grab, use the raster path.
//
// Making a tool UI snapshot-friendly (constructible without a session):
//
//  1. Keep the UI in a widget or box that takes only what it shows and
//     what it needs to call back, not the controller:
//
//       struct EditorArgs {
//           std::shared_ptr<Ui::Show> show; // toasts and nested boxes
//           QImage image;                   // or QByteArray, QString...
//           Fn<void(QImage)> send;          // everything session-bound
//       };
//       void EditorBox(not_null<Ui::GenericBox*> box, EditorArgs &&args);
//
//     or a Ui::RpWidget subclass with a constructor like
//       Editor(QWidget *parent, std::shared_ptr<Ui::Show> show, Data data);
//
//  2. The session entry point, like ShowEditor(controller, document),
//     only gathers the data from the session, fills the callbacks and
//     calls controller->show(Box(EditorBox, std::move(args))), passing
//     controller->uiShow() as the show (ChatHelpers::Show is a Ui::Show).
//
//  3. Inside the UI class don't use Core::App().activeWindow(), domain(),
//     session(), Media::Player::mixer() and don't start network requests,
//     heavy work goes to crl::async with crl::guard / base::make_weak.
//
// Sample data for scenes: generate it in code (QImage with a gradient,
// a synthetic waveform...) or take it from the app resources, for example
// the stickers in ":/animations/*.tgs" (gzipped Lottie, see
// Oblivion::Lottie::Unpack) or images in ":/gui/art/". Scenes must not read
// the user's files.
//
// Registering scenes, from the tool's own .cpp (no shared file changes):
//
//   #include "oblivion/oblivion_ui_snapshots.h"
//
//   namespace {
//
//   const auto SnapshotScenes = Oblivion::SelfTest::SceneRegistrar([] {
//       using namespace Oblivion::SelfTest;
//
//       // A box, shown in a real layer the way a window shows it. With
//       // the height 0 the image is cropped to the box and its shadow.
//       RegisterBoxScene(u"editor_box"_q, QSize(st::boxWideWidth * 2, 0), [](
//               std::shared_ptr<Ui::Show> show) {
//           return Box(EditorBox, EditorArgs{
//               .show = show,
//               .image = SampleImage(),
//           });
//       });
//
//       // A widget, created as a child of the scene container.
//       RegisterScene(u"editor_canvas"_q, QSize(480, 360), [](
//               not_null<Ui::RpWidget*> parent) {
//           return Ui::CreateChild<Canvas>(
//               parent.get(),
//               SceneShow(parent),
//               SampleData());
//       }, [](not_null<QWidget*> widget) {
//           static_cast<Canvas*>(widget.get())->selectTool(Tool::Brush);
//       });
//   });
//
//   } // namespace
//
// The registrar only stores the lambda during static initialization. It is
// called only in the "ui" self-test mode, after styles, palette and the
// language are ready, so st:: values can be used for sizes, and it never
// runs in a normal launch. Scenes are created again for every theme.
//
// Scene rules:
//  - name: [a-z0-9_], it becomes a part of the file name;
//  - size: logical pixels at the current scale, width up to 4096; height 0
//    means "as tall as the content" (for a box: cropped to the box);
//  - create: returns a child of parent (sized by the runner: resizeToWidth
//    for Ui::RpWidget, then the fixed height if there is one) or a
//    top-level widget that the runner then owns, sizes and deletes;
//    nullptr is a failure;
//  - prepare: runs after the container is shown offscreen, to set a state;
//  - ready: optional, lets the runner grab before the wait is over.

namespace Ui {
class BoxContent;
class RpWidget;
class Show;
} // namespace Ui

namespace Oblivion::SelfTest {

using SnapshotScene = Fn<QWidget*(not_null<Ui::RpWidget*> parent)>;
using BoxScene = Fn<object_ptr<Ui::BoxContent>(
	std::shared_ptr<Ui::Show> show)>;
using ScenePrepare = Fn<void(not_null<QWidget*> widget)>;
using SceneReady = Fn<bool(not_null<QWidget*> widget)>;

struct SceneDescriptor {
	QString name;
	QSize size;
	SnapshotScene create; // Either create or box.
	BoxScene box;
	ScenePrepare prepare;
	SceneReady ready;
	crl::time wait = -1; // -1: OBLIVION_SELFTEST_WAIT.
	bool fit = true; // false: create() sets the geometry itself.
};

void RegisterScene(SceneDescriptor &&descriptor);
void RegisterScene(
	QString name,
	QSize size,
	SnapshotScene create,
	ScenePrepare prepare = nullptr);
void RegisterBoxScene(
	QString name,
	QSize size,
	BoxScene create,
	ScenePrepare prepare = nullptr);

// Stores a callback that registers scenes, see the example above.
class SceneRegistrar final {
public:
	explicit SceneRegistrar(Fn<void()> registerScenes);

	SceneRegistrar(const SceneRegistrar &other) = delete;
	SceneRegistrar &operator=(const SceneRegistrar &other) = delete;

};

// A Ui::Show that shows boxes and toasts inside the scene container, for
// widgets that need one. The same layer is used by the box scenes.
[[nodiscard]] std::shared_ptr<Ui::Show> SceneShow(
	not_null<Ui::RpWidget*> parent);

// For oblivion_selftest.cpp.
[[nodiscard]] bool RunUiSnapshots(QStringList &log);

// True after RunUiSnapshots() brought up Core::Application. It is not torn
// down again, the self-test exits the process right after the report.
[[nodiscard]] bool UiEnvironmentCreated();

} // namespace Oblivion::SelfTest
