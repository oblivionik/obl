/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "oblivion/oblivion_selftest.h"

#include "core/launcher.h"
#include "oblivion/oblivion_app_icon.h"
#include "oblivion/oblivion_attach_tools.h"
#include "oblivion/oblivion_audio.h"
#include "oblivion/oblivion_badge.h"
#include "oblivion/oblivion_chat_stats.h"
#include "oblivion/oblivion_cloud.h"
#include "oblivion/oblivion_cloud_share.h"
#include "oblivion/oblivion_cloud_social.h"
#include "oblivion/oblivion_cloud_sync.h"
#include "oblivion/oblivion_deleted_search.h"
#include "oblivion/oblivion_ghost_button.h"
#include "oblivion/oblivion_hub.h"
#include "oblivion/oblivion_listen.h"
#include "oblivion/oblivion_look.h"
#include "oblivion/oblivion_look_hooks.h"
#include "oblivion/oblivion_lottie.h"
#include "oblivion/oblivion_lottie_doc.h"
#include "oblivion/oblivion_lottie_editor_masks.h"
#include "oblivion/oblivion_noise.h"
#include "oblivion/oblivion_online.h"
#include "oblivion/oblivion_photo_core.h"
#include "oblivion/oblivion_photo_doc.h"
#include "oblivion/oblivion_photo_fx.h"
#include "oblivion/oblivion_profile_history.h"
#include "oblivion/oblivion_room.h"
#include "oblivion/oblivion_room_canvas.h"
#include "oblivion/oblivion_room_extras.h"
#include "oblivion/oblivion_room_video.h"
#include "oblivion/oblivion_round_video_convert.h"
#include "oblivion/oblivion_send_online.h"
#include "oblivion/oblivion_stats_export.h"
#include "oblivion/oblivion_sticker_batch.h"
#include "oblivion/oblivion_sticker_export.h"
#include "oblivion/oblivion_sticker_packs_core.h"
#include "oblivion/oblivion_ui_snapshots.h"
#include "oblivion/oblivion_unified_chats.h"
#include "oblivion/oblivion_video_core.h"
#include "oblivion/oblivion_video_fx.h"
#include "oblivion/oblivion_video_project.h"
#include "oblivion/oblivion_vision.h"
#include "oblivion/oblivion_voice_changer.h"
#include "settings.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>

#include <cstdio>
#include <cstdlib>

namespace Oblivion {
namespace {

constexpr auto kModeVariable = "OBLIVION_SELFTEST";
constexpr auto kOutputVariable = "OBLIVION_SELFTEST_OUT";

struct Test {
	QString name;
	bool (*run)(QStringList &log) = nullptr;
	bool explicitOnly = false; // Runs only by name, not as a part of "all".
};

[[nodiscard]] std::vector<Test> Tests() {
	return {
		{ u"lottie"_q, &Lottie::RunSelfTest },
		{ u"lottie_doc"_q, &LottieEdit::RunSelfTest },
		{ u"audio"_q, &Audio::RunSelfTest },
		{ u"round"_q, &RoundVideo::RunSelfTest },
		{ u"photo"_q, &Photo::RunSelfTest },
		{ u"vision"_q, &Vision::RunSelfTest },
		{ u"video_core"_q, &VideoCore::RunSelfTest },
		{ u"sticker_packs"_q, &StickerPacks::RunSelfTest },
		{ u"video_editor"_q, &VideoEdit::RunSelfTest },
		{ u"noise"_q, &Noise::RunSelfTest },
		{ u"voice"_q, &RunVoiceChangerSelfTest },
		{ u"online"_q, &Online::RunSelfTest },
		{ u"profile_history"_q, &ProfileHistory::RunSelfTest },
		{ u"ghost_button"_q, &GhostMode::RunSelfTest },
		{ u"unified_chats"_q, &UnifiedChats::RunSelfTest },
		// Round 4.
		{ u"photo_doc"_q, &Photo::RunDocSelfTest },
		{ u"photo_fx"_q, &Photo::RunFxSelfTest },
		{ u"video_fx"_q, &VideoFx::RunSelfTest },
		{ u"sticker_export"_q, &StickerExport::RunSelfTest },
		{ u"sticker_batch"_q, &StickerBatch::RunSelfTest },
		{ u"attach_tools"_q, &AttachTools::RunSelfTest },
		{ u"chat_stats"_q, &ChatStats::RunSelfTest },
		{ u"badge"_q, &Badge::RunSelfTest },
		{ u"listen"_q, &Listen::RunSelfTest },
		{ u"lottie_editor"_q, &LottieEdit::RunEditorSelfTest },
		// Round 5. "cloud" talks to the real server only when
		// OBLIVION_SELFTEST_CLOUD_LIVE=1 is set, see oblivion_cloud.h.
		{ u"cloud"_q, &Cloud::RunSelfTest },
		{ u"room"_q, &Rooms::RunSelfTest },
		{ u"room_sync"_q, &Rooms::RunSyncSelfTest },
		{ u"room_canvas"_q, &Rooms::RunCanvasSelfTest },
		{ u"room_extras"_q, &Rooms::RunExtrasSelfTest },
		{ u"room_video"_q, &Rooms::RunVideoSelfTest },
		{ u"cloud_social"_q, &Social::RunSelfTest },
		{ u"cloud_share"_q, &Share::RunSelfTest },
		{ u"cloud_sync"_q, &Sync::RunSelfTest },
		{ u"deleted_search"_q, &DeletedSearch::RunSelfTest },
		{ u"send_online"_q, &SendOnline::RunSelfTest },
		{ u"stats_export"_q, &StatsExport::RunSelfTest },
		// Oblivion looks: icons.
		{ u"app_icon"_q, &RunAppIconSelfTest },
		// Oblivion looks: core.
		{ u"look"_q, &Look::RunSelfTest },
		// Oblivion looks: hub.
		{ u"hub"_q, &Hub::RunSelfTest },
		// Oblivion looks: upstream.
		{ u"look_hooks"_q, &Look::RunHooksSelfTest },
		{ u"ui"_q, &SelfTest::RunUiSnapshots, true },
	};
}

[[nodiscard]] QString OutputPath() {
	const auto custom = qEnvironmentVariable(kOutputVariable).trimmed();
	return custom.isEmpty()
		? (cWorkingDir() + u"oblivion_selftest.txt"_q)
		: QDir(cWorkingDir()).absoluteFilePath(custom);
}

} // namespace

bool SelfTestRequested() {
	// The usual ways to switch a flag off must not replace a normal launch.
	const auto mode = qEnvironmentVariable(kModeVariable).trimmed().toLower();
	return !mode.isEmpty()
		&& (mode != u"0"_q)
		&& (mode != u"false"_q)
		&& (mode != u"off"_q)
		&& (mode != u"no"_q);
}

int ExecuteSelfTest() {
	const auto mode = qEnvironmentVariable(kModeVariable).trimmed().toLower();
	const auto path = OutputPath();

	auto lines = QStringList();
	auto written = false;
	const auto add = [&](const QString &line) {
		lines.push_back(line);
		const auto utf8 = (line + '\n').toUtf8();
		std::fwrite(utf8.constData(), 1, utf8.size(), stdout);
		std::fflush(stdout);
	};
	const auto write = [&] {
		// Rewritten after every step, so that a crash inside a test
		// still leaves a report showing where it happened.
		auto file = QFile(path);
		const auto utf8 = (lines.join('\n') + '\n').toUtf8();
		written = file.open(QIODevice::WriteOnly | QIODevice::Truncate)
			&& (file.write(utf8) == utf8.size());
	};

	add(u"Oblivion self-test"_q);
	add(u"Started: "_q + QDateTime::currentDateTime().toString(Qt::ISODate));
	add(u"Mode: "_q + mode);
	add(u"Working dir: "_q + cWorkingDir());
	add(u"Report: "_q + path);
	if (!Core::Launcher::Instance().customWorkingDir()) {
		add(u"WARNING: no -workdir given, the default working dir is used."_q);
	}

	const auto requested = mode.split(
		QRegularExpression(u"[,;\\s]+"_q),
		Qt::SkipEmptyParts);
	const auto all = requested.contains(u"all"_q)
		|| requested.contains(u"1"_q);
	auto unknown = requested;
	unknown.removeAll(u"all"_q);
	unknown.removeAll(u"1"_q);

	auto passed = true;
	auto ran = 0;
	for (const auto &test : Tests()) {
		unknown.removeAll(test.name);
		if ((!all || test.explicitOnly)
			&& !requested.contains(test.name)) {
			continue;
		}
		++ran;
		add(QString());
		add(u"== "_q + test.name + u" =="_q);
		write();

		auto log = QStringList();
		const auto started = crl::now();
		const auto ok = test.run(log);
		const auto elapsed = crl::now() - started;
		for (const auto &line : log) {
			add(line);
		}
		add(u"== %1: %2 in %3 ms =="_q.arg(
			test.name,
			ok ? u"PASSED"_q : u"FAILED"_q,
			QString::number(elapsed)));
		passed = passed && ok;
		write();
	}

	add(QString());
	if (!unknown.isEmpty()) {
		auto expected = QStringList{ u"all"_q };
		for (const auto &test : Tests()) {
			expected.push_back(test.name);
		}
		add(u"ERROR: unknown test names: "_q
			+ unknown.join(u", "_q)
			+ u". Expected: "_q
			+ expected.join(u", "_q)
			+ u"."_q);
		passed = false;
	}
	if (!ran) {
		add(u"ERROR: no tests were run."_q);
		passed = false;
	}
	add(passed ? u"RESULT: PASSED"_q : u"RESULT: FAILED"_q);
	write();

	if (!written) {
		const auto error = (u"ERROR: could not write the report to "_q
			+ path
			+ '\n').toUtf8();
		std::fwrite(error.constData(), 1, error.size(), stderr);
		std::fflush(stderr);
		passed = false;
	}
	LOG(("Oblivion Self-Test: %1, report: %2."
		).arg(passed ? "passed" : "failed"
		).arg(path));

	// UI snapshots: Core::Application was created without run(), so it
	// can't be destroyed normally, the report is written, exit right away.
	if (SelfTest::UiEnvironmentCreated()) {
		std::fflush(stdout);
		std::fflush(stderr);
		std::_Exit(passed ? 0 : 1);
	}
	return passed ? 0 : 1;
}

} // namespace Oblivion
