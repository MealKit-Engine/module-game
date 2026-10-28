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

} // namespace

std::string writeBugReport(const std::string& userFolder, const BugReportInfo& info, const std::string& recordingPath,
                           const std::string& screenshotPath) {
    if (userFolder.empty()) return {};
    std::string note = info.note;
    for (char& c : note)
        if (c == '\n' || c == '\r') c = ' ';
    std::string text = "# MealKit bug report\n";
    text += "engine = " + std::string(kEngineVersion) + "\n";
    text += "gameVersion = " + std::string(info.build.gameVersion) + "\n";
    text += "commit = " + std::string(info.build.commit) + "\n";
    text += "simulation = " + hex(info.build.simVersion) + "\n";
    text += "determinismGroup = " + std::string(net::determinismGroup()) + "\n";
    text += "contentHash = " + hex(info.contentHash) + "\n";
    text += "scene = " + info.scenePath + "\n";
    text += "tuning = " + info.tuningPath + "\n";
    text += "tick = " + std::to_string(info.tick) + "\n";
    text += "players = " + std::to_string(info.players) + "\n";
    text += "localPlayer = " + std::to_string(info.localPlayer) + "\n";
    text += "cpu = " + cpuName() + "\n";
    text += "note = " + note + "\n";

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
    // 이름은 쓴 시각 (리플레이 이름과 같은 방식)
    std::string stamp = formatFileTime(fileWriteTime(temp), true);
    for (char& c : stamp)
        if (c == ' ') c = '_';
        else if (c == ':') c = '-';
    std::string path = folder + "/" + stamp + ".zip";
    for (u32 n = 2; fileExists(path); ++n) path = folder + "/" + stamp + "-" + std::to_string(n) + ".zip";
    if (!moveFile(temp, path)) {
        MEALKIT_ERROR("Cannot write the bug report %s", path.c_str());
        return {};
    }
    if (!screenshotPath.empty()) removeFile(screenshotPath);
    MEALKIT_INFO("Bug report saved: %s (tick %u)", path.c_str(), info.tick);
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
    if (recordingPath.empty()) error = "the bug report has no input recording (replay.rec)";
    return !recordingPath.empty();
}

} // namespace mealkit::game
