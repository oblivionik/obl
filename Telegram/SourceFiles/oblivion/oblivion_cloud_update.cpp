/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_cloud_update.h"

#include "base/platform/base_platform_info.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "oblivion/oblivion_lang.h"
#include "oblivion/oblivion_settings.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/text/format_values.h"
#include "ui/text/text_utilities.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "settings.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStandardPaths>

namespace Oblivion::Update {
namespace {

constexpr auto kDay = int64(24 * 60 * 60);
constexpr auto kTick = crl::time(30 * 60 * 1000);
constexpr auto kFirstDelay = crl::time(45 * 1000);
constexpr auto kEventDelay = crl::time(5 * 1000);
constexpr auto kMaxVersion = 32;
constexpr auto kMaxNotes = 4000;
constexpr auto kMaxFileName = 128;
constexpr auto kMaxFileSize = int64(4) << 30;
constexpr auto kMaxBuild = int64(1) << 53;
constexpr auto kProgressHeight = 6;
constexpr auto kHashChunk = 1024 * 1024;

struct Package {
	QString name;
	int64 size = 0;
	QString sha256;

	[[nodiscard]] bool valid() const {
		return !name.isEmpty();
	}
	// Made from the name: what the server says in "url" is not used.
	[[nodiscard]] QString path() const {
		return u"/v1/updates/files/"_q + name;
	}
};

struct Manifest {
	bool published = false; // The server has a version at all.
	QString version;
	int64 build = 0;
	int64 minBuild = 0;
	int64 publishedAt = 0; // Unixtime.
	QString notesRu;
	QString notesEn;
	Package mac;
	Package win;
};

enum class Verdict {
	Latest,
	Available,
	Required, // This build is older than the cloud accepts.
};

[[nodiscard]] bool SafeFileName(const QString &name) {
	if (name.isEmpty() || name.size() > kMaxFileName) {
		return false;
	}
	for (const auto ch : name) {
		const auto code = ch.unicode();
		const auto good = (code >= 'a' && code <= 'z')
			|| (code >= 'A' && code <= 'Z')
			|| (code >= '0' && code <= '9')
			|| (code == '.')
			|| (code == '_')
			|| (code == '-');
		if (!good) {
			return false;
		}
	}
	return (name[0] != QChar('.')) && (name[0] != QChar('-'));
}

[[nodiscard]] bool SafeVersion(const QString &version) {
	if (version.isEmpty() || version.size() > kMaxVersion) {
		return false;
	}
	for (const auto ch : version) {
		const auto code = ch.unicode();
		const auto good = (code >= 'a' && code <= 'z')
			|| (code >= 'A' && code <= 'Z')
			|| (code >= '0' && code <= '9')
			|| (code == '.')
			|| (code == '_')
			|| (code == '-')
			|| (code == ' ');
		if (!good) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] Package ParsePackage(const QJsonValue &value) {
	auto result = Package();
	const auto object = value.toObject();
	const auto name = object.value(u"name"_q).toString();
	const auto sha256 = object.value(u"sha256"_q).toString();
	const auto size = Cloud::JsonInt(object.value(u"size"_q));
	if (!SafeFileName(name)
		|| !Cloud::ValidMediaId(sha256)
		|| size <= 0
		|| size > kMaxFileSize) {
		return result;
	}
	result.name = name;
	result.sha256 = sha256;
	result.size = size;
	return result;
}

[[nodiscard]] Manifest ParseManifest(const QJsonObject &object) {
	auto result = Manifest();
	const auto version = object.value(u"version"_q);
	const auto build = Cloud::JsonInt(object.value(u"build"_q));
	if (!version.isString()
		|| !SafeVersion(version.toString())
		|| build <= 0
		|| build > kMaxBuild) {
		return result;
	}
	result.published = true;
	result.version = version.toString();
	result.build = build;
	result.minBuild = std::clamp(
		Cloud::JsonInt(object.value(u"min_build"_q)),
		int64(0),
		kMaxBuild);
	result.publishedAt = std::max(
		Cloud::JsonInt(object.value(u"published_at"_q)),
		int64(0)) / 1000;
	const auto notes = object.value(u"notes"_q).toObject();
	result.notesRu = Cloud::JsonText(notes.value(u"ru"_q), kMaxNotes, false);
	result.notesEn = Cloud::JsonText(notes.value(u"en"_q), kMaxNotes, false);
	const auto files = object.value(u"files"_q).toObject();
	result.mac = ParsePackage(files.value(u"mac"_q));
	result.win = ParsePackage(files.value(u"win"_q));
	return result;
}

[[nodiscard]] Verdict Compare(const Manifest &manifest, int64 build) {
	return !manifest.published
		? Verdict::Latest
		: (manifest.minBuild > build)
		? Verdict::Required
		: (manifest.build > build)
		? Verdict::Available
		: Verdict::Latest;
}

[[nodiscard]] QString NotesFor(const Manifest &manifest, bool russian) {
	const auto &first = russian ? manifest.notesRu : manifest.notesEn;
	const auto &second = russian ? manifest.notesEn : manifest.notesRu;
	return first.isEmpty() ? second : first;
}

[[nodiscard]] Package PackageFor(const Manifest &manifest) {
	return Platform::IsMac()
		? manifest.mac
		: Platform::IsWindows()
		? manifest.win
		: Package();
}

// "name (2).zip" next to a file with this name that is something else.
[[nodiscard]] QString NumberedName(const QString &name, int number) {
	const auto dot = name.lastIndexOf(QChar('.'));
	const auto suffix = u" (%1)"_q.arg(number);
	return (dot > 0)
		? (name.left(dot) + suffix + name.mid(dot))
		: (name + suffix);
}

[[nodiscard]] QString FileSha256(const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return QString();
	}
	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	while (!file.atEnd()) {
		const auto chunk = file.read(kHashChunk);
		if (chunk.isEmpty()) {
			break;
		}
		hash.addData(chunk);
	}
	return QString::fromLatin1(hash.result().toHex());
}

// ---- The box.

class ProgressBar final : public Ui::RpWidget {
public:
	explicit ProgressBar(QWidget *parent);

	void setValue(float64 value);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	float64 _value = 0.;

};

ProgressBar::ProgressBar(QWidget *parent)
: RpWidget(parent) {
	resize(width(), style::ConvertScale(kProgressHeight));
}

void ProgressBar::setValue(float64 value) {
	const auto clamped = std::clamp(value, 0., 1.);
	if (_value != clamped) {
		_value = clamped;
		update();
	}
}

void ProgressBar::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto radius = height() / 2.;
	p.setPen(Qt::NoPen);
	p.setBrush(st::windowBgRipple);
	p.drawRoundedRect(QRectF(rect()), radius, radius);
	const auto filled = width() * _value;
	if (filled >= 1.) {
		p.setBrush(st::windowBgActive);
		p.drawRoundedRect(
			QRectF(0., 0., std::max(filled, height() * 1.), height()),
			radius,
			radius);
	}
}

struct UpdateBoxArgs {
	enum class Phase {
		Idle,
		Loading,
		Ready,
		Failed,
	};
	QString version;
	QString current; // The version that runs now.
	int64 published = 0; // Unixtime.
	QString notes;
	bool required = false;
	QString fileName; // Empty: no build for this system yet.
	int64 fileSize = 0;

	// Starts the download and gives the way to cancel it. The callbacks
	// may come at once.
	Fn<Fn<void()>(
		Fn<void(int64 ready, int64 total)> progress,
		Fn<void(const QString &path)> done,
		Fn<void(const QString &error)> fail)> download;
	Fn<void(const QString &path)> showFile;

	// What the box starts with, for the scenes.
	Phase phase = Phase::Idle;
	int64 ready = 0;
	QString path;
	QString error;
};

void UpdateBox(not_null<Ui::GenericBox*> box, UpdateBoxArgs &&args) {
	using Phase = UpdateBoxArgs::Phase;
	struct State {
		Phase phase = Phase::Idle;
		int64 ready = 0;
		int64 total = 0;
		QString path;
		QString error;
		Fn<void()> cancel;
		Fn<void()> refresh;
		rpl::variable<QString> status;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto download = std::move(args.download);
	const auto showFile = std::move(args.showFile);
	const auto hasFile = !args.fileName.isEmpty();
	const auto fileName = args.fileName;
	const auto fileSize = args.fileSize;
	state->phase = args.phase;
	state->ready = args.ready;
	state->total = args.fileSize;
	state->path = args.path;
	state->error = args.error;

	box->setTitle(tr::lng_oblivion_update_title());
	box->setWidth(st::boxWideWidth);
	box->setMaxHeight(st::boxMaxListHeight);
	box->setCloseByOutsideClick(false);

	const auto padding = st::boxRowPadding
		+ style::margins(0, 0, 0, st::boxLittleSkip);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(tr::bold(tr::lng_oblivion_update_version(
				tr::now,
				lt_version,
				args.version))),
			st::boxLabel),
		st::boxRowPadding);
	auto meta = QStringList();
	if (args.published) {
		meta.push_back(langDayOfMonthFull(
			QDateTime::fromSecsSinceEpoch(args.published).date()));
	}
	if (!args.current.isEmpty()) {
		meta.push_back(tr::lng_oblivion_update_current(
			tr::now,
			lt_version,
			args.current));
	}
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(meta.join(QString::fromUtf8(" \xC2\xB7 "))),
			st::boxDividerLabel),
		padding);
	if (args.required) {
		const auto label = box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				tr::lng_oblivion_update_required(),
				st::boxLabel),
			padding);
		label->setTextColorOverride(st::boxTextFgError->c);
	}
	const auto notes = box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(args.notes.isEmpty()
				? tr::lng_oblivion_update_no_notes(tr::now)
				: args.notes),
			st::boxLabel),
		padding);
	notes->setSelectable(true);

	const auto bar = box->addRow(
		object_ptr<Ui::SlideWrap<ProgressBar>>(
			box,
			object_ptr<ProgressBar>(box),
			style::margins(0, st::boxLittleSkip, 0, st::boxLittleSkip)),
		st::boxRowPadding);
	const auto status = box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			state->status.value(),
			st::boxDividerLabel),
		padding);
	status->setSelectable(true);

	const auto start = [=] {
		if (!download || state->phase == Phase::Loading) {
			return;
		}
		state->phase = Phase::Loading;
		state->ready = 0;
		state->error = QString();

		// The button of this click goes away: not from inside of it.
		Ui::PostponeCall(box, [=] {
			state->refresh();
		});
		state->cancel = download(crl::guard(box, [=](int64 ready, int64 total) {
			if (state->phase == Phase::Loading) {
				state->ready = ready;
				if (total > 0) {
					state->total = total;
				}
				state->refresh();
			}
		}), crl::guard(box, [=](const QString &path) {
			state->phase = Phase::Ready;
			state->path = path;
			state->cancel = nullptr;
			Ui::PostponeCall(box, [=] {
				state->refresh();
			});
		}), crl::guard(box, [=](const QString &error) {
			state->phase = Phase::Failed;
			state->error = error;
			state->cancel = nullptr;
			Ui::PostponeCall(box, [=] {
				state->refresh();
			});
		}));
	};
	const auto stop = [=] {
		if (const auto cancel = base::take(state->cancel)) {
			cancel();
		}
		state->phase = Phase::Idle;
		Ui::PostponeCall(box, [=] {
			state->refresh();
		});
	};
	const auto shown = std::make_shared<int>(-1);
	state->refresh = [=] {
		const auto phase = state->phase;
		bar->toggle(phase == Phase::Loading, anim::type::instant);
		bar->entity()->setValue((state->total > 0)
			? (state->ready / float64(state->total))
			: 0.);
		status->setTextColorOverride((phase == Phase::Failed)
			? std::make_optional(st::boxTextFgError->c)
			: std::nullopt);
		state->status = !hasFile
			? tr::lng_oblivion_update_no_file(tr::now)
			: (phase == Phase::Loading)
			? tr::lng_oblivion_update_progress(
				tr::now,
				lt_ready,
				Ui::FormatSizeText(state->ready),
				lt_total,
				Ui::FormatSizeText(state->total))
			: (phase == Phase::Ready)
			? (tr::lng_oblivion_update_saved(
				tr::now,
				lt_path,
				QDir::toNativeSeparators(state->path))
				+ QChar('\n')
				+ (Platform::IsMac()
					? tr::lng_oblivion_update_install_mac(tr::now)
					: tr::lng_oblivion_update_install_win(tr::now)))
			: (phase == Phase::Failed)
			? state->error
			: tr::lng_oblivion_update_file(
				tr::now,
				lt_name,
				fileName,
				lt_size,
				Ui::FormatSizeText(fileSize));

		const auto mark = hasFile ? int(phase) : 100;
		if (std::exchange(*shown, mark) == mark) {
			return;
		}
		box->clearButtons();
		const auto close = [=] {
			box->closeBox();
		};
		if (!hasFile) {
			box->addButton(tr::lng_close(), close);
		} else if (phase == Phase::Loading) {
			box->addButton(tr::lng_cancel(), stop);
		} else if (phase == Phase::Ready) {
			box->addButton(tr::lng_oblivion_update_show(), [=] {
				if (showFile) {
					showFile(state->path);
				}
			});
			box->addButton(tr::lng_close(), close);
		} else {
			box->addButton((phase == Phase::Failed)
				? tr::lng_oblivion_update_retry()
				: tr::lng_oblivion_update_download(), start);
			box->addButton(tr::lng_oblivion_update_later(), close);
		}
	};
	state->refresh();

	box->boxClosing() | rpl::on_next([=] {
		if (const auto cancel = base::take(state->cancel)) {
			cancel();
		}
	}, box->lifetime());
}

// ---- The checks.

struct Global {
	base::Timer timer;
	base::Timer soon;
	Manifest latest;
	Cloud::RequestId request = 0;
	bool started = false;
	bool requiredShown = false;
	std::vector<Fn<void(const Manifest*, const Cloud::Error&)>> waiting;
};

[[nodiscard]] Global &State() {
	static const auto result = new Global();
	return *result;
}

[[nodiscard]] QString DownloadFolder() {
	const auto folder = QStandardPaths::writableLocation(
		QStandardPaths::DownloadLocation);
	return (folder.isEmpty() ? cWorkingDir() : (folder + '/'));
}

void ShowUpdate(std::shared_ptr<Ui::Show> show, const Manifest &manifest) {
	if (!show || !show->valid()) {
		return;
	}
	const auto file = PackageFor(manifest);
	const auto required = (Compare(manifest, kOblivionBuild)
		== Verdict::Required);
	show->showBox(Box(UpdateBox, UpdateBoxArgs{
		.version = manifest.version,
		.current = QString::fromLatin1(kOblivionVersion),
		.published = manifest.publishedAt,
		.notes = NotesFor(manifest, CurrentLanguageIsRussian()),
		.required = required,
		.fileName = file.name,
		.fileSize = file.size,
		.download = [=](
				Fn<void(int64 ready, int64 total)> progress,
				Fn<void(const QString &path)> done,
				Fn<void(const QString &error)> fail) -> Fn<void()> {
			struct Run {
				Cloud::TransferId id = 0;
				bool cancelled = false;
			};
			const auto run = std::make_shared<Run>();
			const auto folder = DownloadFolder();
			const auto begin = [=](const QString &target) {
				if (run->cancelled) {
					return;
				}
				run->id = Cloud::PublicDownload({
					.path = file.path(),
					.to = target,
					.sha256 = file.sha256,
					.auth = false,
					.done = [=](const QString &path) {
						run->id = 0;
						done(path);
					},
					.fail = [=](const Cloud::Error &error) {
						run->id = 0;
						fail((error.type == Cloud::Error::Type::File)
							? tr::lng_oblivion_update_error_file(tr::now)
							: Cloud::ErrorText(error));
					},
					.progress = progress,
				});
			};
			// A file with this name may be there already: the same build
			// that was saved before (nothing to download), or something
			// else (it is left alone, the update gets another name).
			crl::async([=] {
				QDir().mkpath(folder);
				auto ready = QString();
				auto target = QString();
				for (auto i = 1; i != 50; ++i) {
					const auto name = (i == 1)
						? file.name
						: NumberedName(file.name, i);
					const auto path = folder + name;
					const auto info = QFileInfo(path);
					if (!info.exists()) {
						target = path;
						break;
					} else if (info.isFile()
						&& info.size() == file.size
						&& FileSha256(path) == file.sha256) {
						ready = path;
						break;
					}
				}
				crl::on_main([=] {
					if (run->cancelled) {
						return;
					} else if (!ready.isEmpty()) {
						done(ready);
					} else if (target.isEmpty()) {
						fail(tr::lng_oblivion_update_error_file(tr::now));
					} else {
						begin(target);
					}
				});
			});
			return [=] {
				run->cancelled = true;
				if (const auto id = base::take(run->id)) {
					Cloud::CancelPublicDownload(id);
				}
			};
		},
		.showFile = [](const QString &path) {
			::File::ShowInFolder(path);
		},
	}));
}

// One request at a time, everybody who has asked gets its answer.
void RequestManifest(Fn<void(const Manifest*, const Cloud::Error&)> done) {
	auto &state = State();
	state.waiting.push_back(std::move(done));
	if (state.request) {
		return;
	}
	const auto finish = [](const Manifest *manifest, Cloud::Error error) {
		auto &state = State();
		state.request = 0;
		if (manifest) {
			state.latest = *manifest;
		}
		const auto waiting = base::take(state.waiting);
		for (const auto &callback : waiting) {
			if (callback) {
				callback(manifest, error);
			}
		}
	};
	state.request = Cloud::PublicRequest(
		Cloud::GetRequest(u"/v1/updates/manifest"_q),
		[=](const Cloud::Response &response) {
			const auto manifest = ParseManifest(response.json);
			finish(&manifest, Cloud::Error());
		},
		[=](const Cloud::Error &error) {
			finish(nullptr, error);
		});
}

// The box of a background check never interrupts something: only into a
// window without a layer and without the passcode lock, otherwise the
// next tick tries again.
void OfferLatest() {
	auto &state = State();
	const auto verdict = Compare(state.latest, kOblivionBuild);
	if (verdict == Verdict::Latest
		|| (verdict == Verdict::Available
			&& Get().cloudUpdateSeenBuild() >= state.latest.build)
		|| (verdict == Verdict::Required && state.requiredShown)) {
		return;
	}
	const auto window = Core::IsAppLaunched()
		? Core::App().activePrimaryWindow()
		: nullptr;
	const auto controller = window ? window->sessionController() : nullptr;
	if (!controller
		|| Core::App().passcodeLocked()
		|| controller->isLayerShown()) {
		return;
	}
	state.requiredShown = (verdict == Verdict::Required);
	Get().setCloudUpdateSeenBuild(state.latest.build);
	ShowUpdate(controller->uiShow(), state.latest);
}

void BackgroundCheck(bool force) {
	if (!Get().cloudUpdateCheck() || !Cloud::AnyReady()) {
		return;
	}
	const auto now = int64(base::unixtime::now());
	const auto last = Get().cloudUpdateLastCheck();
	const auto fresh = (last > 0) && (last <= now) && (now - last < kDay);
	if (fresh && !force) {
		// Something that could not be shown at the time of the check.
		OfferLatest();
		return;
	}
	RequestManifest([](const Manifest *manifest, const Cloud::Error &error) {
		if (!manifest) {
			// Quietly: the next tick tries again.
			return;
		}
		Get().setCloudUpdateLastCheck(int64(base::unixtime::now()));
		OfferLatest();
	});
}

// ---- Self-test.

class Checker final {
public:
	explicit Checker(QStringList &log);

	void operator()(bool condition, const char *what);
	void section(const char *name);
	[[nodiscard]] int failed() const;

private:
	QStringList &_log;
	int _failed = 0;
	int _sectionPassed = 0;
	int _sectionFailed = 0;

};

Checker::Checker(QStringList &log)
: _log(log) {
}

void Checker::operator()(bool condition, const char *what) {
	if (condition) {
		++_sectionPassed;
	} else {
		++_failed;
		++_sectionFailed;
		_log.push_back(u"FAILED: "_q + QString::fromUtf8(what));
	}
}

void Checker::section(const char *name) {
	_log.push_back(u"%1: %2 passed, %3 failed"_q.arg(
		QString::fromUtf8(name),
		QString::number(_sectionPassed),
		QString::number(_sectionFailed)));
	_sectionPassed = _sectionFailed = 0;
}

int Checker::failed() const {
	return _failed;
}

[[nodiscard]] QJsonObject ManifestJson(const QByteArray &text) {
	return QJsonDocument::fromJson(text).object();
}

void TestManifest(Checker &check) {
	const auto sha = QByteArray(64, 'a');
	const auto text = QByteArray("{\"version\":\"5.0.1\",\"build\":5000001,"
		"\"min_build\":0,\"published_at\":1759800000000,"
		"\"notes\":{\"ru\":\"New in Russian\\nsecond line\","
		"\"en\":\"What's new\"},"
		"\"files\":{\"mac\":{\"name\":\"Oblivion-5.0.1-mac.zip\","
		"\"size\":123456789,\"sha256\":\"") + sha + QByteArray("\","
		"\"url\":\"https://evil.example/file\"},"
		"\"win\":{\"name\":\"Oblivion-5.0.1-win.exe\",\"size\":98765432,"
		"\"sha256\":\"") + sha + QByteArray("\","
		"\"url\":\"/v1/updates/files/Oblivion-5.0.1-win.exe\"}}}");
	const auto manifest = ParseManifest(ManifestJson(text));
	check(manifest.published, "manifest: a published version is parsed");
	check(manifest.version == u"5.0.1"_q && manifest.build == 5000001,
		"manifest: the version and the build");
	check(manifest.publishedAt == 1759800000, "manifest: the date");
	check(manifest.notesRu.contains(QChar('\n'))
		&& manifest.notesEn == u"What's new"_q,
		"manifest: the notes keep their lines");
	check(manifest.mac.valid()
		&& manifest.mac.name == u"Oblivion-5.0.1-mac.zip"_q
		&& manifest.mac.size == 123456789
		&& manifest.mac.sha256 == QString::fromLatin1(sha),
		"manifest: the mac file");
	check(manifest.mac.path() == u"/v1/updates/files/Oblivion-5.0.1-mac.zip"_q,
		"manifest: the path is made from the name, not from the url");
	check(manifest.win.valid() && manifest.win.size == 98765432,
		"manifest: the win file");
	check(NotesFor(manifest, true) == manifest.notesRu
		&& NotesFor(manifest, false) == manifest.notesEn,
		"manifest: the notes of the language");

	check(Compare(manifest, 5000000) == Verdict::Available,
		"compare: a newer build is offered");
	check(Compare(manifest, 5000001) == Verdict::Latest,
		"compare: the same build is the latest");
	check(Compare(manifest, 5000002) == Verdict::Latest,
		"compare: an older build of the server is not offered");
	auto demanding = manifest;
	demanding.minBuild = 5000001;
	check(Compare(demanding, 5000000) == Verdict::Required,
		"compare: a build below the minimum must update");
	check(Compare(demanding, 5000001) == Verdict::Latest,
		"compare: the minimum itself is fine");

	const auto empty = ParseManifest(ManifestJson("{\"version\":null}"));
	check(!empty.published, "manifest: nothing is published");
	check(Compare(empty, 1) == Verdict::Latest,
		"compare: nothing published, nothing offered");
	check(!ParseManifest(QJsonObject()).published, "manifest: an empty one");
	check(!ParseManifest(ManifestJson(
		"{\"version\":\"5.1\",\"build\":0}")).published,
		"manifest: a build must be positive");
	check(!ParseManifest(ManifestJson(
		"{\"version\":\"5.1\",\"build\":\"abc\"}")).published,
		"manifest: a build must be a number");
	check(!ParseManifest(ManifestJson(
		"{\"version\":\"<b>5.1</b>\",\"build\":7}")).published,
		"manifest: a version with markup is refused");
	check(!ParseManifest(ManifestJson(
		"{\"version\":5.1,\"build\":7}")).published,
		"manifest: a version must be a string");

	const auto hostile = [&](const QByteArray &file) {
		return ParseManifest(ManifestJson(
			QByteArray("{\"version\":\"6\",\"build\":6000000,"
				"\"files\":{\"mac\":") + file + QByteArray("}}")));
	};
	const auto good = QByteArray("\"size\":10,\"sha256\":\"") + sha + '"';
	check(hostile("{\"name\":\"ok-1.zip\"," + good + "}").mac.valid(),
		"file: a clean name is taken");
	check(!hostile("{\"name\":\"../../evil.zip\"," + good + "}").mac.valid(),
		"file: a path in the name is refused");
	check(!hostile("{\"name\":\"a/b.zip\"," + good + "}").mac.valid(),
		"file: a slash in the name is refused");
	check(!hostile("{\"name\":\"a\\\\b.zip\"," + good + "}").mac.valid(),
		"file: a backslash in the name is refused");
	check(!hostile("{\"name\":\".hidden\"," + good + "}").mac.valid(),
		"file: a hidden file is refused");
	check(!hostile("{\"name\":\"a b.zip\"," + good + "}").mac.valid(),
		"file: a space in the name is refused");
	check(!hostile("{\"name\":\"\"," + good + "}").mac.valid(),
		"file: an empty name is refused");
	check(!hostile("{\"name\":\"ok.zip\",\"size\":10,"
		"\"sha256\":\"abc\"}").mac.valid(),
		"file: a bad hash is refused");
	check(!hostile("{\"name\":\"ok.zip\",\"size\":0,\"sha256\":\""
		+ sha + "\"}").mac.valid(),
		"file: an empty file is refused");
	check(!hostile("{\"name\":\"ok.zip\",\"size\":99999999999999,"
		"\"sha256\":\"" + sha + "\"}").mac.valid(),
		"file: a huge file is refused");
	check(!hostile("\"text\"").mac.valid(), "file: not an object");
	check(hostile("null").published && !hostile("null").mac.valid(),
		"file: a version without a build for this system");

	check(NumberedName(u"Oblivion-5.0.1-mac.zip"_q, 2)
		== u"Oblivion-5.0.1-mac (2).zip"_q,
		"name: a number before the extension");
	check(NumberedName(u"Oblivion"_q, 3) == u"Oblivion (3)"_q,
		"name: a number without an extension");
	check(kOblivionBuild > 0 && QByteArray(kOblivionVersion).size() > 0,
		"build: this app has a build number and a version");
	check.section("update");
}

// ---- Snapshot scenes.

[[nodiscard]] QString Sample(const char *ru, const char *en) {
	return QString::fromUtf8(CurrentLanguageIsRussian() ? ru : en);
}

const auto SnapshotScenes = SelfTest::SceneRegistrar([] {
	using namespace SelfTest;
	using Phase = UpdateBoxArgs::Phase;

	const auto size = QSize(st::boxWideWidth * 2, 0);
	const auto scene = [=](
			const QString &name,
			Fn<void(UpdateBoxArgs&)> change) {
		RegisterBoxScene(name, size, [=](std::shared_ptr<Ui::Show> show) {
			auto args = UpdateBoxArgs{
				.version = u"5.1"_q,
				.current = u"5.0"_q,
				.published = 1791327935,
				.notes = Sample(
					"• Комнаты: видео вместе без рассинхрона.\n"
					"• Общие наборы: поиск по названию.\n"
					"• Плейлисты: загрузка стала быстрее.\n"
					"• Исправления и мелкие улучшения.",
					"• Rooms: watching together without drift.\n"
					"• Shared presets: search by name.\n"
					"• Playlists: the upload is faster.\n"
					"• Fixes and small improvements."),
				.fileName = Platform::IsWindows()
					? u"Oblivion-5.1-win.exe"_q
					: u"Oblivion-5.1-mac.zip"_q,
				.fileSize = 187'654'321,
			};
			if (change) {
				change(args);
			}
			return Box(UpdateBox, std::move(args));
		});
	};
	scene(u"update_available"_q, nullptr);
	scene(u"update_required"_q, [](UpdateBoxArgs &args) {
		args.required = true;
	});
	scene(u"update_downloading"_q, [](UpdateBoxArgs &args) {
		args.phase = Phase::Loading;
		args.ready = 61'234'567;
	});
	scene(u"update_ready"_q, [](UpdateBoxArgs &args) {
		args.phase = Phase::Ready;
		args.path = Platform::IsWindows()
			? u"C:/Users/anna/Downloads/Oblivion-5.1-win.exe"_q
			: u"/Users/anna/Downloads/Oblivion-5.1-mac.zip"_q;
	});
	scene(u"update_failed"_q, [](UpdateBoxArgs &args) {
		args.phase = Phase::Failed;
		args.error = tr::lng_oblivion_cloud_error_network(tr::now);
	});
	scene(u"update_no_file"_q, [](UpdateBoxArgs &args) {
		args.fileName = QString();
		args.notes = QString();
	});
});

} // namespace

void Start(not_null<Main::Session*> session) {
	auto &state = State();
	if (!std::exchange(state.started, true)) {
		state.timer.setCallback([] {
			BackgroundCheck(false);
		});
		state.timer.callEach(kTick);
		state.soon.setCallback([] {
			BackgroundCheck(false);
		});
		state.soon.callOnce(kFirstDelay);
	}
	// The server tells the connected clients about a new version at once.
	Cloud::For(session).events(
	) | rpl::filter([](const Cloud::Event &event) {
		return (event.type == u"update"_q);
	}) | rpl::on_next([] {
		auto &state = State();
		state.soon.setCallback([] {
			BackgroundCheck(true);
		});
		state.soon.callOnce(kEventDelay);
	}, session->lifetime());
}

void CheckNow(not_null<Window::SessionController*> controller) {
	const auto weak = base::make_weak(controller);
	controller->showToast(tr::lng_oblivion_update_checking(tr::now));
	RequestManifest([=](const Manifest *manifest, const Cloud::Error &error) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		} else if (!manifest) {
			Cloud::ShowError(strong->uiShow(), error);
			return;
		}
		Get().setCloudUpdateLastCheck(int64(base::unixtime::now()));
		if (Compare(*manifest, kOblivionBuild) == Verdict::Latest) {
			strong->showToast(tr::lng_oblivion_update_latest(
				tr::now,
				lt_version,
				QString::fromLatin1(kOblivionVersion)));
			return;
		}
		Get().setCloudUpdateSeenBuild(manifest->build);
		ShowUpdate(strong->uiShow(), *manifest);
	});
}

bool RunSelfTest(QStringList &log) {
	auto check = Checker(log);
	TestManifest(check);
	return !check.failed();
}

} // namespace Oblivion::Update
