#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/game/BuildInfo.h"

#include <string>

// Bug report bundle: players/testers hand "this match right now" to developers with one key (F9).
// User folder bugreports/<YYYY-MM-DD_HH-MM>.zip (uncompressed zip, core/Zip.h):
//   replay.rec  input recording up to then (same format as replays, game/InputRecord.h)
//   info.txt    engine/game version, commit, simulation version, determinism group, content hash, scene/tuning paths, tick, CPU, a line written by the player
//   log.txt     recent log (core/Log.h recentLines)
//   screenshot.png
// When the editor's [File] > [Open Replay] opens the zip, it extracts replay.rec, replays up to that tick, and pauses (the recording ends there).
// It's a deterministic simulation, so the match is recreated from inputs alone. A different build/data is reported via the content hash.
namespace mealkit::game {

inline constexpr const char* kBugReportFolder = "bugreports";
inline constexpr const char* kCrashReportFolder = "crash";

struct BugReportInfo {
    BuildInfo build;
    u64 contentHash = 0;
    std::string scenePath, tuningPath;
    u32 tick = 0;
    u32 players = 0, localPlayer = 0;
    std::string note; // a line written by the player
};

// Path of the saved zip (empty string on failure). If the recording/screenshot files are missing, bundles without just those
std::string writeBugReport(const std::string& userFolder, const BugReportInfo& info, const std::string& recordingPath,
                           const std::string& screenshotPath);

// Crash reports: the same zip, left by a crash instead of F9, in the user folder crash/<YYYY-MM-DD_HH-MM>.zip.
// At the crash, the platform handler (platform/System.h installCrashHandler) writes crash/pending/crash.txt + crash.dmp (minidump),
// and the hook adds info.txt, log.txt and recording.txt (path of the input recording, which is flushed every tick).
// The next run bundles pending/ into the zip (collectCrashReport) before replays/current.rec is kept or overwritten.
// Arms crash reports for this run. info's tick/players are updated each frame with updateCrashReport
void armCrashReport(const std::string& userFolder, const BugReportInfo& info, const std::string& recordingPath);
void updateCrashReport(u32 tick, u32 players, u32 localPlayer);
// Bundles a crash left by an earlier run into a zip. Path of the zip, empty string if there was none
std::string collectCrashReport(const std::string& userFolder);

// Extracts replay.rec from the zip (bug report or crash report) to outFolder/replay.rec. info is the info.txt contents
bool extractBugReport(const std::string& zipPath, const std::string& outFolder, std::string& recordingPath,
                      std::string& info, std::string& error);

} // namespace mealkit::game
