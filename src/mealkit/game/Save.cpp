#include "mealkit/game/Save.h"

#include "mealkit/core/Log.h"
#include "mealkit/platform/Paths.h"

#include <cstdlib>

namespace mealkit::game {
namespace {

constexpr const char* kHeader = "# MealKit save ";

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

} // namespace

std::string savePath(const std::string& userFolder, const std::string& name) {
    return userFolder + "/" + kSavesFolder + "/" + name + ".save";
}

bool writeSave(const reflect::TypeInfo& type, const void* data, const std::string& userFolder, const std::string& name, u32 version) {
    if (userFolder.empty()) return false; // automated runs: no saving
    const std::string path = savePath(userFolder, name);
    createDirectories(parentDirectory(path));
    reflect::TextFields fields;
    reflect::toText(type, data, fields);
    std::string text = std::string(kHeader) + "(" + type.name + ")\nversion " + std::to_string(version) + "\n";
    for (const auto& [k, v] : fields) text += k + " " + v + "\n";
    std::string previous;
    if (readTextFile(path, previous)) writeTextFile(path + ".bak", previous);
    if (!writeTextFile(path, text)) {
        MEALKIT_ERROR("Save: cannot write %s", path.c_str());
        return false;
    }
    return true;
}

SaveInfo readSave(const reflect::TypeInfo& type, void* data, const std::string& userFolder, const std::string& name, u32 version,
                  SaveMigrate migrate) {
    SaveInfo info;
    if (userFolder.empty()) return info;
    info.path = savePath(userFolder, name);
    std::string text;
    if (!readTextFile(info.path, text)) return info;
    info.found = true;
    reflect::TextFields fields;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line[0] == '#') continue;
        const size_t space = line.find(' ');
        const std::string key = space == std::string::npos ? line : line.substr(0, space);
        const std::string value = space == std::string::npos ? std::string() : trim(line.substr(space + 1));
        if (key == "version") info.version = static_cast<u32>(std::strtoul(value.c_str(), nullptr, 10));
        else fields.emplace_back(key, value);
    }
    if (info.version > version)
        MEALKIT_WARN("Save %s is version %u, newer than this game (%u) - unknown fields are dropped", info.path.c_str(), info.version, version);
    if (migrate && info.version < version) migrate(info.version, fields);
    reflect::fromText(type, data, fields, &info.warnings); // unknown fields are dropped, missing fields get defaults
    if (!info.warnings.empty()) MEALKIT_INFO("Save %s: %s", info.path.c_str(), info.warnings.c_str());
    return info;
}

} // namespace mealkit::game
