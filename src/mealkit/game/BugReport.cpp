#include "mealkit/game/BugReport.h"

#include "mealkit/core/Log.h"
#include "mealkit/core/Version.h"
#include "mealkit/core/Zip.h"
#include "mealkit/net/Lockstep.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/System.h"

#include <cstdio>

namespace mealkit::game {
namespace {

bool readBytes(const std::string& path, std::vector<u8>& out) {
    FILE* f = openFile(path, "rb");
    if (!f) return false;
    u8 buffer[65536];
    for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;) out.insert(out.end(), buffer, buffer + n);
    std::fclose(f);
    return true;
}

ZipEntry textEntry(const char* name, const std::string& text) { return {name, std::vector<u8>(text.begin(), text.end())}; }

std::string hex(u64 v) {
    char s[17];
    std::snprintf(s, sizeof(s), "%016llx", static_cast<unsigned long long>(v));
    return s;
}

// info.txt without the lines that change during a run (tick, players, localPlayer: matchLines)
std::string infoText(const char* header, const BugReportInfo& info) {
    std::string note = info.note;
    for (char& c : note)
        if (c == '\n' || c == '\r') c = ' ';
    std::string text = header;
    text += "engine = " + std::string(kEngineVersion) + "\n";
    text += "gameVersion = " + std::string(info.build.gameVersion) + "\n";
    text += "commit = " + std::string(info.build.commit) + "\n";
    text += "simulation = " + hex(info.build.simVersion) + "\n";
    text += "determinismGroup = " + std::string(net::determinismGroup()) + "\n";
    text += "contentHash = " + hex(info.contentHash) + "\n";
    text += "scene = " + info.scenePath + "\n";
    text += "tuning = " + info.tuningPath + "\n";
    text += "cpu = " + cpuName() + "\n";
    text += "note = " + note + "\n";
    return text;
}

std::string matchLines(u32 tick, u32 players, u32 localPlayer) {
    return "tick = " + std::to_string(tick) + "\nplayers = " + std::to_string(players) +
           "\nlocalPlayer = " + std::to_string(localPlayer) + "\n";
}

// "YYYY-MM-DD_HH-MM" of the file's write time, a free name in folder (same scheme as replay names)
std::string stampedPath(const std::string& folder, const std::string& file) {
    std::string stamp = formatFileTime(fileWriteTime(file), true);
    for (char& c : stamp)
        if (c == ' ') c = '_';
        else if (c == ':') c = '-';
    std::string path = folder + "/" + stamp + ".zip";
    for (u32 n = 2; fileExists(path); ++n) path = folder + "/" + stamp + "-" + std::to_string(n) + ".zip";
    return path;
}

bool writeText(const std::string& path, const std::string& text) {
    FILE* f = openFile(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    return ok;
}

// Crash report state for this run (armCrashReport). The hook runs inside the crash handler: it only formats and writes files
struct CrashState {
    std::string pending, info, recording;
    u32 tick = 0, players = 0, localPlayer = 0;
};
CrashState g_crash;

void onCrash() {
    const CrashState& c = g_crash;
    writeText(c.pending + "/info.txt", c.info + matchLines(c.tick, c.players, c.localPlayer));
    writeText(c.pending + "/log.txt", log::recentLines());
    writeText(c.pending + "/recording.txt", c.recording);
}

} // namespace

std::string writeBugReport(const std::string& userFolder, const BugReportInfo& info, const std::string& recordingPath,
                           const std::string& screenshotPath) {
    if (userFolder.empty()) return {};
    const std::string text =
        infoText("# MealKit bug report\n", info) + matchLines(info.tick, info.players, info.localPlayer);

    std::vector<ZipEntry> entries;
    entries.push_back(textEntry("info.txt", text));
    ZipEntry rec{"replay.rec", {}};
    if (!recordingPath.empty() && readBytes(recordingPath, rec.data)) entries.push_back(std::move(rec));
    ZipEntry shot{"screenshot.png", {}};
    if (!screenshotPath.empty() && readBytes(screenshotPath, shot.data)) entries.push_back(std::move(shot));
    entries.push_back(textEntry("log.txt", log::recentLines()));

    const std::string folder = userFolder + "/" + kBugReportFolder;
    createDirectories(folder);
    const std::string temp = folder + "/new.zip";
    if (!writeZip(temp, entries)) {
        MEALKIT_ERROR("Cannot write the bug report %s", temp.c_str());
        return {};
    }
    const std::string path = stampedPath(folder, temp); // named by the time written
    if (!moveFile(temp, path)) {
        MEALKIT_ERROR("Cannot write the bug report %s", path.c_str());
        return {};
    }
    if (!screenshotPath.empty()) removeFile(screenshotPath);
    MEALKIT_INFO("Bug report saved: %s (tick %u)", path.c_str(), info.tick);
    return path;
}

void armCrashReport(const std::string& userFolder, const BugReportInfo& info, const std::string& recordingPath) {
    if (userFolder.empty()) return;
    g_crash.pending = userFolder + "/" + kCrashReportFolder + "/pending";
    BugReportInfo i = info;
    i.note.clear();
    g_crash.info = infoText("# MealKit crash report\n", i);
    g_crash.recording = recordingPath;
    createDirectories(g_crash.pending);
    installCrashHandler(g_crash.pending, onCrash);
}

void updateCrashReport(u32 tick, u32 players, u32 localPlayer) {
    g_crash.tick = tick;
    g_crash.players = players;
    g_crash.localPlayer = localPlayer;
}

std::string collectCrashReport(const std::string& userFolder) {
    if (userFolder.empty()) return {};
    const std::string folder = userFolder + "/" + kCrashReportFolder;
    const std::string pending = folder + "/pending";
    if (!fileExists(pending + "/crash.txt")) return {};
    std::vector<ZipEntry> entries;
    for (const char* name : {"info.txt", "crash.txt", "log.txt", "crash.dmp"}) {
        ZipEntry e{name, {}};
        if (readBytes(pending + "/" + name, e.data)) entries.push_back(std::move(e));
    }
    std::vector<u8> recording;
    readBytes(pending + "/recording.txt", recording);
    const std::string recordingPath(recording.begin(), recording.end());
    ZipEntry rec{"replay.rec", {}};
    if (!recordingPath.empty() && readBytes(recordingPath, rec.data)) entries.push_back(std::move(rec));
    const std::string temp = folder + "/new.zip";
    if (!writeZip(temp, entries)) {
        MEALKIT_ERROR("Cannot write the crash report %s", temp.c_str());
        return {};
    }
    const std::string path = stampedPath(folder, pending + "/crash.txt"); // named by the crash time
    if (!moveFile(temp, path)) {
        MEALKIT_ERROR("Cannot write the crash report %s", path.c_str());
        return {};
    }
    for (const char* name : {"info.txt", "crash.txt", "log.txt", "crash.dmp", "recording.txt"})
        removeFile(pending + "/" + name);
    MEALKIT_WARN("The last run crashed. Crash report saved: %s", path.c_str());
    return path;
}

bool extractBugReport(const std::string& zipPath, const std::string& outFolder, std::string& recordingPath,
                      std::string& info, std::string& error) {
    std::vector<ZipEntry> entries;
    if (!readZip(zipPath, entries, error)) return false;
    recordingPath.clear();
    info.clear();
    createDirectories(outFolder);
    for (const ZipEntry& e : entries) {
        if (e.name == "info.txt") info.assign(e.data.begin(), e.data.end());
        if (e.name != "replay.rec") continue;
        recordingPath = outFolder + "/replay.rec";
        FILE* f = openFile(recordingPath, "wb");
        const bool ok = f && std::fwrite(e.data.data(), 1, e.data.size(), f) == e.data.size();
        if (f) std::fclose(f);
        if (!ok) {
            error = "cannot write " + recordingPath;
            return false;
        }
    }
    if (recordingPath.empty()) error = "the report has no input recording (replay.rec)";
    return !recordingPath.empty();
}

} // namespace mealkit::game
