/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_ui_snapshots.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "core/launcher.h"
#include "core/sandbox.h"
#include "lang/lang_file_parser.h"
#include "lang/lang_instance.h"
#include "oblivion/oblivion_lang.h"
#include "settings.h"
#include "ui/cached_round_corners.h"
#include "ui/effects/animation_value.h"
#include "ui/effects/spoiler_mess.h"
#include "ui/emoji_config.h"
#include "ui/layers/box_content.h"
#include "ui/layers/layer_manager.h"
#include "ui/layers/layer_widget.h"
#include "ui/layers/show.h"
#include "ui/rp_widget.h"
#include "ui/style/style_core_font.h"
#include "ui/text/text_options.h"
#include "ui/ui_utility.h"
#include "window/themes/window_theme.h"
#include "base/unique_qptr.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>

// The @autoreleasepool primitives, exported by libobjc.
extern "C" void *objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void *pool);
#endif // __APPLE__

#ifndef _WIN32
#include <csignal>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#endif // !_WIN32

namespace Oblivion::SelfTest {
namespace {

constexpr auto kModeVariable = "OBLIVION_SELFTEST";
constexpr auto kOutDirVariable = "OBLIVION_SELFTEST_OUT_DIR";
constexpr auto kScenesVariable = "OBLIVION_SELFTEST_SCENES";
constexpr auto kThemesVariable = "OBLIVION_SELFTEST_THEMES";
constexpr auto kLangVariable = "OBLIVION_SELFTEST_LANG";
constexpr auto kLangFileVariable = "OBLIVION_SELFTEST_LANG_FILE";
constexpr auto kWaitVariable = "OBLIVION_SELFTEST_WAIT";
constexpr auto kScaleVariable = "OBLIVION_SELFTEST_SCALE";
constexpr auto kRatioVariable = "OBLIVION_SELFTEST_RATIO";

constexpr auto kDefaultWait = crl::time(1500);
constexpr auto kMaxWait = crl::time(120'000);
constexpr auto kMinReadyWait = crl::time(100);
constexpr auto kStartWait = crl::time(300);
constexpr auto kDestroyWait = crl::time(30);
constexpr auto kTickInterval = 25;
constexpr auto kMaxSide = 4096;
constexpr auto kBoxWindowHeight = 1200;
constexpr auto kBoxCropPadding = 8;

// The Russian cloud language pack can't be downloaded here, those are the
// usual translations of the most common upstream buttons, so that boxes in
// the snapshots look like in the app. OBLIVION_SELFTEST_LANG_FILE wins,
// the Oblivion rows from oblivion_lang.cpp are applied over both.
struct StandInRow {
	const char *key = nullptr;
	const char *value = nullptr;
};
const StandInRow kRussianStandIn[] = {
	{ "lng_cancel", "Отмена" },
	{ "lng_close", "Закрыть" },
	{ "lng_box_ok", "ОК" },
	{ "lng_box_done", "Готово" },
	{ "lng_about_done", "Готово" },
	{ "lng_settings_save", "Сохранить" },
	{ "lng_continue", "Продолжить" },
	{ "lng_box_delete", "Удалить" },
	{ "lng_box_remove", "Удалить" },
	{ "lng_box_yes", "Да" },
	{ "lng_box_no", "Нет" },
	{ "lng_send_button", "Отправить" },
	{ "lng_selected_clear", "Отмена" },
	{ "lng_create_group_next", "Далее" },
	{ "lng_restart_button", "Перезапустить" },
	{ "lng_open_link", "Открыть" },
	{ "lng_saved_messages", "Избранное" },
	{ "lng_context_copy_text", "Копировать текст" },
	{ "lng_mac_menu_undo", "Отменить" },
	{ "lng_mac_menu_redo", "Повторить" },
	{ "lng_settings_scale", "Масштаб интерфейса" },
};

struct Registry {
	std::vector<Fn<void()>> registrars;
	std::vector<SceneDescriptor> scenes;
};

struct Environment {
	Core::Application *application = nullptr;
	bool ready = false;
};

struct Rendered {
	QString error;
	QString path;
	QSize pixels;
};

[[nodiscard]] Registry &GetRegistry() {
	static auto result = Registry();
	return result;
}

[[nodiscard]] Environment &GetEnvironment() {
	static auto result = Environment();
	return result;
}

[[nodiscard]] base::flat_map<QWidget*, Ui::LayerManager*> &LayerManagers() {
	static auto result = base::flat_map<QWidget*, Ui::LayerManager*>();
	return result;
}

// Same parsing as in oblivion_selftest.cpp, without Qt: it runs before
// main(), see DisableAppStateRestoration().
[[nodiscard]] bool UiModeRequested() {
	const auto value = std::getenv(kModeVariable);
	if (!value) {
		return false;
	}
	auto token = std::string();
	for (auto ch = value; ; ++ch) {
		const auto c = static_cast<unsigned char>(*ch);
		if (!c || c == ',' || c == ';' || std::isspace(c)) {
			if (token == "ui") {
				return true;
			}
			token.clear();
			if (!c) {
				break;
			}
		} else {
			token.push_back(char(std::tolower(c)));
		}
	}
	return false;
}

// Every "ui" run crashed (SIGSEGV) ~31 s after the start, whatever scene
// was being rendered, each time a few ms after "-[NSPersistentUIManager
// flushAllChanges] Emptying for initial flush" in the system log. That is
// AppKit's first Resume state flush, 30 s after it finished launching
// (the first QEventLoop::exec() runs [NSApp run]). By then the scenes have
// created and destroyed dozens of windows, and the saved state it empties
// is the real app's one (the bundle id is the same). The snapshots have
// nothing to save or restore, so the persistence is switched off, and the
// automatic termination too (AppKit marks the window-less process as
// terminable a few seconds after the launch). Only the volatile argument
// domain is changed, the preferences of the real app are not touched.
// Runs before main(), so before NSApplication reads any of those defaults.
// If a crash still happens, see InstallCrashReport() for the backtrace.
void DisableAppStateRestoration() {
#ifdef __APPLE__
	using Send0 = id(*)(id, SEL);
	using Send1 = id(*)(id, SEL, id);
	using SendVoid0 = void(*)(id, SEL);
	using SendVoid1 = void(*)(id, SEL, id);
	using SendVoid2 = void(*)(id, SEL, id, id);
	const auto send0 = reinterpret_cast<Send0>(objc_msgSend);
	const auto send1 = reinterpret_cast<Send1>(objc_msgSend);
	const auto sendVoid0 = reinterpret_cast<SendVoid0>(objc_msgSend);
	const auto sendVoid1 = reinterpret_cast<SendVoid1>(objc_msgSend);
	const auto send2 = reinterpret_cast<SendVoid2>(objc_msgSend);
	const auto object = [](const void *value) {
		return reinterpret_cast<id>(const_cast<void*>(value));
	};
	const auto classObject = [](const char *name) {
		return reinterpret_cast<id>(objc_getClass(name));
	};

	const auto pool = objc_autoreleasePoolPush();
	try {
		const auto defaults = classObject("NSUserDefaults")
			? send0(
				classObject("NSUserDefaults"),
				sel_registerName("standardUserDefaults"))
			: nullptr;
		if (defaults) {
			const auto name = CFSTR("NSArgumentDomain");
			auto existing = id();
			try {
				existing = send1(
					defaults,
					sel_registerName("volatileDomainForName:"),
					object(name));
			} catch (...) {
			}
			const auto domain = existing
				? CFDictionaryCreateMutableCopy(
					kCFAllocatorDefault,
					0,
					reinterpret_cast<CFDictionaryRef>(existing))
				: CFDictionaryCreateMutable(
					kCFAllocatorDefault,
					0,
					&kCFTypeDictionaryKeyCallBacks,
					&kCFTypeDictionaryValueCallBacks);
			if (domain) {
				// Both keys are read in AppKit's NSPersistentUI.m.
				CFDictionarySetValue(
					domain,
					CFSTR("ApplePersistence"),
					kCFBooleanFalse);
				CFDictionarySetValue(
					domain,
					CFSTR("NSDisablePersistence"),
					kCFBooleanTrue);
				CFDictionarySetValue(
					domain,
					CFSTR("ApplePersistenceIgnoreState"),
					kCFBooleanTrue);
				CFDictionarySetValue(
					domain,
					CFSTR("NSQuitAlwaysKeepsWindows"),
					kCFBooleanFalse);
				// Some implementations refuse to replace an existing
				// volatile domain, the merged copy is set instead of it.
				try {
					sendVoid1(
						defaults,
						sel_registerName("removeVolatileDomainForName:"),
						object(name));
				} catch (...) {
				}
				send2(
					defaults,
					sel_registerName("setVolatileDomain:forName:"),
					object(domain),
					object(name));
				CFRelease(domain);
			}
		}
		const auto info = classObject("NSProcessInfo")
			? send0(
				classObject("NSProcessInfo"),
				sel_registerName("processInfo"))
			: nullptr;
		if (info) {
			sendVoid1(
				info,
				sel_registerName("disableAutomaticTermination:"),
				object(CFSTR("Oblivion UI snapshots")));
			sendVoid0(info, sel_registerName("disableSuddenTermination"));
		}
	} catch (...) {
		// An Objective-C exception, the run goes on with the defaults.
	}
	objc_autoreleasePoolPop(pool);
#endif // __APPLE__
}

[[maybe_unused]] const auto kAppStateRestorationGuard = [] {
	if (UiModeRequested()) {
		DisableAppStateRestoration();
	}
	return true;
}();

#ifndef _WIN32

// A crash leaves only "RUNNING <scene>" in the manifest, so the crashed
// thread's backtrace (the binary has symbols) is written next to it.
struct CrashReport {
	char path[2048] = { 0 };
	char stage[512] = { 0 };
};

CrashReport GlobalCrashReport;

void CopyToBuffer(char *buffer, std::size_t size, const QByteArray &value) {
	const auto count = std::min(std::size_t(value.size()), size - 1);
	std::memcpy(buffer, value.constData(), count);
	buffer[count] = 0;
}

void WriteAll(int fd, const char *data, std::size_t size) {
	while (size > 0) {
		const auto written = ::write(fd, data, size);
		if (written <= 0) {
			return;
		}
		data += written;
		size -= std::size_t(written);
	}
}

void WriteText(int fd, const char *text) {
	WriteAll(fd, text, std::strlen(text));
}

void WriteNumber(int fd, int value) {
	char buffer[16];
	auto position = sizeof(buffer);
	auto left = (value < 0) ? -value : value;
	do {
		buffer[--position] = char('0' + (left % 10));
		left /= 10;
	} while (left > 0 && position > 1);
	if (value < 0) {
		buffer[--position] = '-';
	}
	WriteAll(fd, buffer + position, sizeof(buffer) - position);
}

void CrashHandler(int signal) {
	void *frames[128];
	const auto count = ::backtrace(frames, int(std::size(frames)));
	const auto file = GlobalCrashReport.path[0]
		? ::open(GlobalCrashReport.path, O_WRONLY | O_CREAT | O_TRUNC, 0644)
		: -1;
	for (const auto fd : { int(STDERR_FILENO), file }) {
		if (fd < 0) {
			continue;
		}
		WriteText(fd, "\nOblivion UI snapshots: signal ");
		WriteNumber(fd, signal);
		WriteText(fd, " while ");
		WriteText(fd, GlobalCrashReport.stage);
		WriteText(fd, ".\nBacktrace of the crashed thread:\n");
		::backtrace_symbols_fd(frames, count, fd);
	}
	if (file >= 0) {
		::close(file);
	}
	::signal(signal, SIG_DFL);
	::raise(signal);
}

void SetCrashStage(const QString &stage) {
	CopyToBuffer(
		GlobalCrashReport.stage,
		sizeof(GlobalCrashReport.stage),
		stage.toUtf8());
}

[[nodiscard]] QString InstallCrashReport(const QString &folder) {
	const auto path = folder + u"/snapshots-crash.txt"_q;
	QFile::remove(path);
	CopyToBuffer(
		GlobalCrashReport.path,
		sizeof(GlobalCrashReport.path),
		QFile::encodeName(path));
	SetCrashStage(u"starting"_q);
	const auto caught = {
		SIGSEGV,
		SIGBUS,
		SIGILL,
		SIGFPE,
		SIGABRT,
		SIGTRAP,
	};
	for (const auto signal : caught) {
		::signal(signal, CrashHandler);
	}
	return path;
}

#else // !_WIN32

void SetCrashStage(const QString &stage) {
}

[[nodiscard]] QString InstallCrashReport(const QString &folder) {
	return QString();
}

#endif // !_WIN32

[[nodiscard]] QString EnvString(const char *name) {
	return qEnvironmentVariable(name).trimmed();
}

[[nodiscard]] int EnvInt(const char *name, int fallback) {
	auto ok = false;
	const auto result = EnvString(name).toInt(&ok);
	return ok ? result : fallback;
}

[[nodiscard]] QStringList EnvList(const char *name) {
	return EnvString(name).split(
		QRegularExpression(u"[,;\\s]+"_q),
		Qt::SkipEmptyParts);
}

// Returns an empty string on success or the error description.
template <typename Callback>
[[nodiscard]] QString Guarded(Callback &&callback) {
	try {
		callback();
		return QString();
	} catch (const std::exception &e) {
		return u"exception: "_q + QString::fromUtf8(e.what());
	} catch (...) {
		return u"unknown exception"_q;
	}
}

template <typename Callback>
[[nodiscard]] QString GuardedInEvent(Callback &&callback) {
	return Guarded([&] {
		Core::Sandbox::Instance().customEnterFromEventLoop(callback);
	});
}

// Runs the event loop, so that layout, timers, animations, crl::on_main
// and the async work of the scene can finish.
void WaitFor(crl::time duration, Fn<bool()> ready = nullptr) {
	const auto started = crl::now();
	auto loop = QEventLoop();
	auto timer = QTimer();
	timer.setInterval(kTickInterval);
	QObject::connect(&timer, &QTimer::timeout, [&] {
		const auto passed = crl::now() - started;
		auto finished = (passed >= duration);
		if (!finished && ready && passed >= kMinReadyWait) {
			try {
				finished = ready();
			} catch (...) {
				finished = true;
			}
		}
		if (finished) {
			timer.stop();
			loop.quit();
		}
	});
	timer.start();
	loop.exec();
}

void FlushDeletes() {
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	WaitFor(kDestroyWait);
}

[[nodiscard]] QString SanitizeName(const QString &name) {
	auto result = name.trimmed().toLower();
	for (auto &ch : result) {
		const auto good = (ch >= 'a' && ch <= 'z')
			|| (ch >= '0' && ch <= '9')
			|| (ch == '_');
		if (!good) {
			ch = '_';
		}
	}
	return result;
}

[[nodiscard]] bool SceneSelected(
		const QString &name,
		const QStringList &patterns) {
	auto hasIncludes = false;
	auto included = false;
	for (const auto &pattern : patterns) {
		const auto exclude = pattern.startsWith('-');
		const auto body = exclude ? pattern.mid(1) : pattern;
		if (body.isEmpty()) {
			continue;
		}
		// The same as QRegularExpression::fromWildcard(), which Qt 5 (the
		// default Qt of the upstream Windows x64 build) doesn't have.
		const auto regex = QRegularExpression(
			QRegularExpression::wildcardToRegularExpression(body),
			QRegularExpression::CaseInsensitiveOption);
		const auto matches = regex.match(name).hasMatch();
		if (exclude) {
			if (matches) {
				return false;
			}
		} else {
			hasIncludes = true;
			included = included || matches;
		}
	}
	return included || !hasIncludes;
}

[[nodiscard]] bool IsUniform(const QImage &image) {
	if (image.isNull() || image.width() < 2 || image.height() < 2) {
		return true;
	}
	const auto converted = (image.format()
		== QImage::Format_ARGB32_Premultiplied)
		? image
		: image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	const auto first = reinterpret_cast<const uint32*>(
		converted.constScanLine(0))[0];
	for (auto y = 0; y != converted.height(); ++y) {
		const auto line = reinterpret_cast<const uint32*>(
			converted.constScanLine(y));
		for (auto x = 0; x != converted.width(); ++x) {
			if (line[x] != first) {
				return false;
			}
		}
	}
	return true;
}

[[nodiscard]] not_null<Ui::LayerManager*> SceneLayers(
		not_null<Ui::RpWidget*> parent) {
	const auto key = static_cast<QWidget*>(parent.get());
	auto &managers = LayerManagers();
	const auto i = managers.find(key);
	if (i != end(managers)) {
		return i->second;
	}
	const auto result = parent->lifetime().make_state<Ui::LayerManager>(
		parent);
	managers.emplace(key, result);
	parent->lifetime().add([=] {
		LayerManagers().remove(key);
	});
	return result;
}

[[nodiscard]] QString ApplyLanguage() {
	auto id = EnvString(kLangVariable).toLower();
	if (id.isEmpty()) {
		id = u"ru"_q;
	}
	auto &lang = Core::App().langpack();
	lang.switchToId({
		.id = id,
		.pluralId = id,
		.baseId = QString(),
		.name = id,
		.nativeName = QString(),
	});

	auto notes = QStringList();
	auto strings = QVector<MTPLangPackString>();
	const auto russian = Oblivion::CurrentLanguageIsRussian();
	if (russian) {
		for (const auto &row : kRussianStandIn) {
			strings.push_back(MTP_langPackString(
				MTP_string(row.key),
				MTP_string(row.value)));
		}
		notes.push_back(u"Oblivion strings, %1 stand-in strings"_q.arg(
			int(std::size(kRussianStandIn))));
	}
	const auto file = EnvString(kLangFileVariable);
	if (!file.isEmpty()) {
		const auto absolute = QFileInfo(file).absoluteFilePath();
		const auto content = Lang::FileParser::ReadFile(absolute, file);
		auto count = 0;
		if (!content.isEmpty()) {
			const auto parser = Lang::FileParser(content, [&](
					QLatin1String key,
					const QByteArray &value) {
				strings.push_back(MTP_langPackString(
					MTP_bytes(QByteArray(key.data(), key.size())),
					MTP_bytes(value)));
				++count;
			});
			if (!parser.errors().isEmpty()) {
				notes.push_back(u"language file errors: "_q
					+ parser.errors());
			}
		}
		notes.push_back(u"%1 strings from %2"_q.arg(
			QString::number(count),
			absolute));
	}
	if (!strings.isEmpty()) {
		lang.applyDifference(
			Lang::Pack::Current,
			MTP_langPackDifference(
				MTP_string(lang.id()),
				MTP_int(0),
				MTP_int(1),
				MTP_vector<MTPLangPackString>(std::move(strings))
			).c_langPackDifference());
	}
	if (!russian) {
		notes.push_back(u"no Russian overrides"_q);
	}
	return lang.id() + u" ("_q + notes.join(u"; "_q) + ')';
}

[[nodiscard]] bool StartEnvironment(QStringList &log) {
	auto &environment = GetEnvironment();
	if (environment.ready) {
		return true;
	} else if (environment.application || Core::IsAppLaunched()) {
		log.push_back(u"ERROR: the application is already created."_q);
		return false;
	}
	const auto ratio = std::clamp(EnvInt(kRatioVariable, 2), 1, 3);
	style::SetDevicePixelRatio(ratio);
	const auto requestedScale = EnvInt(kScaleVariable, style::kScaleDefault);
	const auto scale = style::CheckScale((requestedScale > 0)
		? requestedScale
		: style::kScaleDefault);
	cSetScreenScale(scale);
	cSetConfigScale(scale);
	ValidateScale();

	auto language = QString();
	const auto error = GuardedInEvent([&] {
		// Never deleted: ~Application() expects everything that run()
		// starts, the self-test exits the process right after the report.
		environment.application = new Core::Application();

		style::SetCustomFont(Core::App().settings().customFontFamily());
		style::internal::StartFonts();
		style::StartManager(cScale());
		Ui::InitTextOptions();
		Ui::StartCachedCorners();
		Ui::Emoji::Init();
		Ui::PreloadTextSpoilerMask();

		language = ApplyLanguage();
	});
	if (!error.isEmpty()) {
		log.push_back(u"ERROR: could not start the UI environment, "_q
			+ error);
		return false;
	}
	WaitFor(kStartWait);

	environment.ready = true;
	log.push_back(u"Language: "_q + language);
	log.push_back(u"Scale: %1%, device pixel ratio: %2."_q.arg(
		QString::number(cScale()),
		QString::number(style::DevicePixelRatio())));
	return true;
}

[[nodiscard]] QString ThemePath(const QString &theme) {
	if (theme == u"night"_q) {
		return Window::Theme::NightThemePath();
	} else if (theme == u"dayblue"_q) {
		return u":/gui/day-blue.tdesktop-theme"_q;
	} else if (theme == u"tinted"_q) {
		return u":/gui/night-green.tdesktop-theme"_q;
	}
	return QString();
}

[[nodiscard]] bool KnownTheme(const QString &theme) {
	return (theme == u"day"_q) || !ThemePath(theme).isEmpty();
}

// Returns an empty string on success or the error description.
[[nodiscard]] QString ApplyTheme(const QString &theme) {
	auto result = QString();
	const auto error = GuardedInEvent([&] {
		const auto path = ThemePath(theme);
		if (path.isEmpty()) {
			style::main_palette::reset();
		} else {
			auto instance = Window::Theme::Instance();
			if (!Window::Theme::LoadFromFile(
					path,
					&instance,
					nullptr,
					nullptr)) {
				result = u"could not load "_q + path;
				return;
			}
			style::main_palette::apply(instance.palette);
		}
		Window::Theme::SetNightModeValue(
			(theme == u"night"_q) || (theme == u"tinted"_q));
		style::NotifyPaletteChanged();
	});
	return error.isEmpty() ? result : error;
}

void FitWidget(not_null<QWidget*> widget, int width, int height) {
	if (const auto rp = qobject_cast<Ui::RpWidget*>(widget.get())) {
		rp->resizeToWidth(width);
	} else if (height <= 0) {
		const auto hint = widget->sizeHint();
		widget->resize(
			width,
			(hint.height() > 0) ? hint.height() : widget->height());
	}
	if (height > 0) {
		widget->resize(width, height);
	}
}

[[nodiscard]] Rendered RenderScene(
		const SceneDescriptor &scene,
		const QString &theme,
		const QString &folder,
		crl::time defaultWait) {
	auto result = Rendered();
	const auto isBox = (scene.box != nullptr);
	const auto width = scene.size.width();
	const auto fixedHeight = std::max(scene.size.height(), 0);
	const auto autoHeight = !fixedHeight;
	if (width <= 0 || width > kMaxSide || fixedHeight > kMaxSide) {
		result.error = u"bad size %1x%2"_q
			.arg(scene.size.width())
			.arg(scene.size.height());
		return result;
	} else if (!isBox && !scene.create) {
		result.error = u"no create() or box() callback"_q;
		return result;
	}

	auto container = base::unique_qptr<Ui::RpWidget>();
	auto separate = QPointer<QWidget>();
	auto widget = QPointer<QWidget>();

	// Create the scene in a hidden container (a top-level widget that is
	// "shown" for Qt, but never appears on the screen).
	auto failure = QString();
	auto error = GuardedInEvent([&] {
		const auto raw = container.emplace();
		raw->setAttribute(Qt::WA_DontShowOnScreen);
		raw->resize(
			width,
			fixedHeight
				? fixedHeight
				: isBox
				? style::ConvertScale(kBoxWindowHeight)
				: 1);
		raw->paintRequest(
		) | rpl::on_next([=](QRect clip) {
			QPainter(raw).fillRect(clip, st::windowBg);
		}, raw->lifetime());

		if (isBox) {
			raw->show();
			Ui::SendPendingMoveResizeEvents(raw);
			const auto layers = SceneLayers(raw);
			auto box = scene.box(layers->uiShow());
			if (!box) {
				failure = u"box() returned nothing"_q;
				return;
			}
			widget = box.data();
			layers->showBox(
				std::move(box),
				Ui::LayerOption::KeepOther,
				anim::type::instant);
			return;
		}
		const auto created = scene.create(raw);
		if (!created) {
			failure = u"create() returned nothing"_q;
			return;
		}
		widget = created;
		if (created->isWindow()) {
			separate = created;
			created->setAttribute(Qt::WA_DontShowOnScreen);
		} else if (scene.fit) {
			created->move(0, 0);
		}
		if (scene.fit) {
			FitWidget(created, width, fixedHeight);
		}
		if (autoHeight && !separate) {
			const auto follow = [=] {
				raw->resize(
					width,
					std::max(created->y() + created->height(), 1));
			};
			follow();
			if (const auto rp = qobject_cast<Ui::RpWidget*>(created)) {
				rp->geometryValue(
				) | rpl::on_next([=](QRect) {
					follow();
				}, rp->lifetime());
			}
		}
		raw->show();
		if (const auto top = separate.data()) {
			top->show();
			Ui::SendPendingMoveResizeEvents(top);
		}
		Ui::SendPendingMoveResizeEvents(raw);
		if (const auto rp = qobject_cast<Ui::RpWidget*>(created)) {
			rp->setVisibleTopBottom(0, rp->height());
		}
	});
	if (error.isEmpty()) {
		error = failure;
	}

	// Let the scene set its state and finish its work.
	if (error.isEmpty() && scene.prepare && widget) {
		error = GuardedInEvent([&] {
			scene.prepare(widget.data());
		});
	}
	if (error.isEmpty()) {
		const auto wait = (scene.wait >= 0) ? scene.wait : defaultWait;
		const auto ready = scene.ready;
		WaitFor(std::min(wait, kMaxWait), ready ? Fn<bool()>([=] {
			return widget && ready(widget.data());
		}) : Fn<bool()>());
		if (!container) {
			error = u"the scene container was destroyed"_q;
		}
	}

	// Grab the pixels.
	auto image = QImage();
	if (error.isEmpty()) {
		error = GuardedInEvent([&] {
			const auto raw = container.get();
			const auto target = separate
				? separate.data()
				: static_cast<QWidget*>(raw);
			auto crop = QRect();
			if (isBox && autoHeight) {
				const auto layer = SceneLayers(raw)->topShownLayer();
				if (layer && layer->isVisible()) {
					const auto padding = style::ConvertScale(kBoxCropPadding);
					crop = QRect(layer->mapTo(raw, QPoint()), layer->size())
						.marginsAdded(st::boxRoundShadow.extend)
						.marginsAdded({ padding, padding, padding, padding })
						.intersected(raw->rect());
				}
			}
			image = Ui::GrabWidgetToImage(target, crop, st::windowBg->c);
		});
	}
	if (error.isEmpty()) {
		if (image.isNull()) {
			error = u"the grab is empty"_q;
		} else if (IsUniform(image)) {
			error = u"the image is a single color, nothing was drawn"_q;
		}
	}
	if (error.isEmpty()) {
		result.path = folder + '/' + scene.name + '_' + theme + u".png"_q;
		result.pixels = image.size();
		if (!image.save(result.path, "PNG")) {
			error = u"could not write "_q + result.path;
		}
	}

	// Destroy everything before the next scene or palette.
	const auto destroyed = GuardedInEvent([&] {
		if (const auto top = separate.data()) {
			delete top;
		}
		container = nullptr;
	});
	FlushDeletes();
	if (error.isEmpty() && !destroyed.isEmpty()) {
		error = u"destroying: "_q + destroyed;
	}
	result.error = error;
	return result;
}

[[nodiscard]] QString OutputFolder() {
	const auto custom = EnvString(kOutDirVariable);
	const auto path = custom.isEmpty()
		? (cWorkingDir() + u"snapshots"_q)
		: QDir(cWorkingDir()).absoluteFilePath(custom);
	return QDir::cleanPath(path);
}

void WriteManifest(const QString &folder, const QStringList &lines) {
	auto file = QFile(folder + u"/snapshots.txt"_q);
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write((lines.join('\n') + '\n').toUtf8());
	}
}

} // namespace

void RegisterScene(SceneDescriptor &&descriptor) {
	GetRegistry().scenes.push_back(std::move(descriptor));
}

void RegisterScene(
		QString name,
		QSize size,
		SnapshotScene create,
		ScenePrepare prepare) {
	RegisterScene({
		.name = std::move(name),
		.size = size,
		.create = std::move(create),
		.prepare = std::move(prepare),
	});
}

void RegisterBoxScene(
		QString name,
		QSize size,
		BoxScene create,
		ScenePrepare prepare) {
	RegisterScene({
		.name = std::move(name),
		.size = size,
		.box = std::move(create),
		.prepare = std::move(prepare),
	});
}

SceneRegistrar::SceneRegistrar(Fn<void()> registerScenes) {
	if (registerScenes) {
		GetRegistry().registrars.push_back(std::move(registerScenes));
	}
}

std::shared_ptr<Ui::Show> SceneShow(not_null<Ui::RpWidget*> parent) {
	return SceneLayers(parent)->uiShow();
}

bool UiEnvironmentCreated() {
	return (GetEnvironment().application != nullptr);
}

bool RunUiSnapshots(QStringList &log) {
	const auto runStarted = crl::now();
	if (!Core::Launcher::Instance().customWorkingDir()) {
		log.push_back(u"ERROR: run the ui snapshots with -workdir <folder>, "
			"the real data folder must not be used."_q);
		return false;
	}
	const auto folder = OutputFolder();
	if (!QDir().mkpath(folder)) {
		log.push_back(u"ERROR: could not create "_q + folder);
		return false;
	}
	const auto crashReport = InstallCrashReport(folder);
	SetCrashStage(u"starting the UI environment"_q);
	if (!StartEnvironment(log)) {
		return false;
	}
	if (!crashReport.isEmpty()) {
		log.push_back(u"Crash backtraces (only on a crash): "_q
			+ crashReport);
	}
	SetCrashStage(u"registering the scenes"_q);

	// Collect the scenes. Registrars may register more scenes, so the
	// list is copied only after all of them ran.
	auto &registry = GetRegistry();
	auto passed = true;
	for (const auto &registrar : base::take(registry.registrars)) {
		const auto error = GuardedInEvent([&] {
			registrar();
		});
		if (!error.isEmpty()) {
			log.push_back(u"ERROR: a scene registrar failed, "_q + error);
			passed = false;
		}
	}
	auto all = std::vector<SceneDescriptor>();
	auto names = QStringList();
	for (auto &scene : registry.scenes) {
		const auto name = SanitizeName(scene.name);
		if (name.isEmpty()) {
			log.push_back(u"ERROR: a scene without a name."_q);
			passed = false;
			continue;
		} else if (name != scene.name) {
			log.push_back(u"WARNING: scene '%1' is renamed to '%2'."_q
				.arg(scene.name, name));
			scene.name = name;
		}
		if (names.contains(name)) {
			log.push_back(u"ERROR: scene '%1' is registered twice."_q
				.arg(name));
			passed = false;
			continue;
		}
		names.push_back(name);
		all.push_back(scene);
	}
	const auto patterns = EnvList(kScenesVariable);
	auto scenes = std::vector<SceneDescriptor>();
	for (const auto &scene : all) {
		if (SceneSelected(scene.name, patterns)) {
			scenes.push_back(scene);
		}
	}
	log.push_back(u"Scenes: %1 registered (%2), %3 selected."_q.arg(
		QString::number(int(all.size())),
		names.join(u", "_q),
		QString::number(int(scenes.size()))));

	auto themes = EnvList(kThemesVariable);
	if (themes.isEmpty()) {
		themes = QStringList{ u"day"_q, u"night"_q };
	}
	themes.removeDuplicates();
	const auto defaultWait = std::clamp(
		crl::time(EnvInt(kWaitVariable, int(kDefaultWait))),
		crl::time(0),
		kMaxWait);
	log.push_back(u"Themes: %1, wait: %2 ms."_q.arg(
		themes.join(u", "_q),
		QString::number(defaultWait)));
	log.push_back(u"Output: "_q + folder);

	auto manifest = QStringList{
		u"Oblivion UI snapshots, "_q
			+ QDateTime::currentDateTime().toString(Qt::ISODate),
	};
	for (const auto &line : log) {
		manifest.push_back(u"# "_q + line);
	}
	WriteManifest(folder, manifest);

	auto rendered = 0;
	for (const auto &theme : themes) {
		const auto name = theme.toLower();
		SetCrashStage(u"applying the theme "_q + name);
		const auto themeError = KnownTheme(name)
			? ApplyTheme(name)
			: u"unknown theme, expected day, night, dayblue or tinted"_q;
		if (!themeError.isEmpty()) {
			log.push_back(u"[%1] FAILED: %2"_q.arg(theme, themeError));
			manifest.push_back(u"FAILED\t*\t%1\t%2"_q.arg(theme, themeError));
			WriteManifest(folder, manifest);
			passed = false;
			continue;
		}
		for (const auto &scene : scenes) {
			manifest.push_back(u"RUNNING\t%1\t%2"_q.arg(scene.name, name));
			WriteManifest(folder, manifest);
			SetCrashStage(u"rendering scene %1 (%2), %3 s after the start"_q
				.arg(scene.name, name)
				.arg((crl::now() - runStarted) / 1000));

			const auto started = crl::now();
			const auto result = RenderScene(
				scene,
				name,
				folder,
				defaultWait);
			const auto elapsed = crl::now() - started;
			if (result.error.isEmpty()) {
				++rendered;
				const auto size = u"%1x%2"_q
					.arg(result.pixels.width())
					.arg(result.pixels.height());
				manifest.back() = u"OK\t%1\t%2\t%3\t%4"_q.arg(
					scene.name,
					name,
					size,
					result.path);
				log.push_back(u"[%1] OK %2, %3 px, %4 ms: %5"_q.arg(
					name,
					scene.name,
					size,
					QString::number(elapsed),
					result.path));
			} else {
				passed = false;
				manifest.back() = u"FAILED\t%1\t%2\t%3"_q.arg(
					scene.name,
					name,
					result.error);
				log.push_back(u"[%1] FAILED %2: %3"_q.arg(
					name,
					scene.name,
					result.error));
			}
			WriteManifest(folder, manifest);
		}
	}
	SetCrashStage(u"finishing after the scenes"_q);
	if (!rendered) {
		log.push_back(u"ERROR: no snapshots were rendered."_q);
		passed = false;
	}
	return passed;
}

} // namespace Oblivion::SelfTest
