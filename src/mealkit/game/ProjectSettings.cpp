#include "mealkit/game/ProjectSettings.h"

#include "mealkit/core/Log.h"
#include "mealkit/core/TextFormat.h"
#include "mealkit/platform/Paths.h"

#include <algorithm>
#include <cstring>

namespace mealkit::game {
namespace {

bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isBlank(s[a])) ++a;
    while (b > a && isBlank(s[b - 1])) --b;
    return s.substr(a, b - a);
}

void appendLine(std::string& text, const std::string& key, const std::string& value) {
    text += key;
    if (!value.empty()) text += " " + value;
    text += "\n";
}

} // namespace

bool loadProjectFile(const std::string& path, ProjectFile& out, std::string* error) {
    out = ProjectFile{};
    std::string text;
    if (!readTextFile(path, text)) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    const reflect::TypeInfo& type = reflect::typeOf<ProjectSettings>();
    const size_t prefixLength = std::strlen(kGameSettingsPrefix);
    bool seenKey = false;
    size_t start = 0;
    int lineNo = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = trim(text.substr(start, end - start));
        start = end + 1;
        ++lineNo;
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (!seenKey) out.header += line + "\n";
            continue;
        }
        u32 version = 1; // only version 1 for now, so no migrate
        if (text::readFormatLine(line, text::kProject, path, version)) continue;
        seenKey = true;
        // "path value" or the old "key=value" format
        size_t keyEnd = 0;
        while (keyEnd < line.size() && !isBlank(line[keyEnd]) && line[keyEnd] != '=') ++keyEnd;
        size_t valueStart = keyEnd;
        while (valueStart < line.size() && isBlank(line[valueStart])) ++valueStart;
        if (valueStart < line.size() && line[valueStart] == '=') ++valueStart;
        const std::string key = line.substr(0, keyEnd);
        const std::string value = trim(line.substr(valueStart));

        if (key.compare(0, prefixLength, kGameSettingsPrefix) == 0) {
            out.gameFields.emplace_back(key.substr(prefixLength), value);
        } else if (!reflect::setFromText(type, &out.settings, key, value)) {
            MEALKIT_WARN("%s:%d: unknown project key '%s' kept as is", path.c_str(), lineNo, key.c_str());
            out.unknownFields.emplace_back(key, value);
        }
    }
    return true;
}

bool saveProjectFile(const std::string& path, const ProjectFile& project) {
    std::string text = project.header.empty()
                           ? "# MealKit project (path value). Edit in the editor via [File] > [Project Settings]. Game items use game.\n"
                           : project.header;
    text += text::formatLine(text::kProject) + "\n";
    reflect::TextFields fields;
    reflect::toText(reflect::typeOf<ProjectSettings>(), &project.settings, fields);
    for (const auto& [key, value] : fields) appendLine(text, key, value);
    for (const auto& [key, value] : project.gameFields) appendLine(text, kGameSettingsPrefix + key, value);
    for (const auto& [key, value] : project.unknownFields) appendLine(text, key, value);
    if (!writeTextFile(path, text)) {
        MEALKIT_ERROR("Cannot write %s", path.c_str());
        return false;
    }
    return true;
}

void applyGameSettings(const ProjectFile& project, const reflect::TypeInfo& gameType, void* gameData) {
    std::string unknown;
    reflect::fromText(gameType, gameData, project.gameFields, &unknown);
    if (!unknown.empty())
        MEALKIT_WARN("Project settings not declared in %s (ignored): %s", gameType.name, unknown.c_str());
}

bool loadGameProject(ProjectFile& out, const reflect::TypeInfo* gameType, void* gameData) {
    const std::string path = findAsset(kProjectFileName);
    std::string error;
    if (path.empty() || !loadProjectFile(path, out, &error)) {
        MEALKIT_WARN("%s not found - using the default project settings", kProjectFileName);
        out = ProjectFile{};
        return false;
    }
    if (gameType && gameData) applyGameSettings(out, *gameType, gameData);
    return true;
}

void applyTickRate(const ProjectSettings& settings) {
    const u32 rate = std::clamp(settings.tickRate, 10u, 120u);
    if (rate != settings.tickRate) MEALKIT_WARN("tickRate %u in mealkit.project is out of range - using %u", settings.tickRate, rate);
    reflect::setTickRate(rate);
}

session::AutoDelay autoDelayOf(const ProjectSettings& s, f32 tickMs) {
    session::AutoDelay a;
    a.enabled = s.inputDelayAuto;
    a.tickMs = tickMs;
    a.minTicks = std::max(1u, static_cast<u32>(static_cast<f32>(s.inputDelayMinMs) / tickMs + 0.5f));
    a.maxTicks = std::max(a.minTicks, static_cast<u32>(static_cast<f32>(s.inputDelayMaxMs) / tickMs + 0.5f));
    return a;
}

std::string userFolder(const ProjectSettings& settings) {
    return userDataDirectory() + "/" + (settings.name.empty() ? std::string("MealKitGame") : settings.name);
}

} // namespace mealkit::game
