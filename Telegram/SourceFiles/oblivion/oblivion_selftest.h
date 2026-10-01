/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

// Self-test mode for the media toolkits.
//
// OBLIVION_SELFTEST=all|lottie|audio (comma separated list allowed),
// unset, empty, 0, false, off or no mean a normal launch.
// OBLIVION_SELFTEST_OUT=<report path> (default <working dir>/
// oblivion_selftest.txt, relative paths resolve against the working dir)
//
// Core::Sandbox::start() checks it right after the QApplication object
// is created and before the single-instance socket, lock file, accounts
// and windows, so with a separate -workdir it neither connects to nor
// disturbs a running instance and never touches its data:
//
// (one command line, an environment variable in front of the binary)
//   OBLIVION_SELFTEST=all
//   out/Release/Oblivion.app/Contents/MacOS/Oblivion
//   -workdir /private/tmp/claude-501/oblivion-selftest
//
// OBLIVION_SELFTEST=ui renders widget scenes offscreen into PNG files; it
// is not included in "all", see oblivion/oblivion_ui_snapshots.h.
namespace Oblivion {

[[nodiscard]] bool SelfTestRequested();

// Runs the requested tests, prints the report to stdout and writes it
// to the report file. Returns the process exit code: 0 if everything
// passed, 1 otherwise.
[[nodiscard]] int ExecuteSelfTest();

} // namespace Oblivion
