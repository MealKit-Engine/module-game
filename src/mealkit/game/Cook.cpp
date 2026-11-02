#include "mealkit/game/Cook.h"

#include "mealkit/asset/Cooked.h"
#include "mealkit/asset/Environment.h"
#include "mealkit/asset/TextureCache.h"
#include "mealkit/core/Log.h"
#include "mealkit/game/ProjectSettings.h"
#include "mealkit/platform/Pack.h"
#include "mealkit/platform/Paths.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <set>
#include <string_view>
#include <vector>

namespace mealkit::game {

#if MEALKIT_ASSET_IMPORT
namespace {

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
bool startsWith(std::string_view s, std::string_view prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

// Files under a folder (any depth), as prefix + relative path with '/'
void walk(const std::string& dir, const std::string& prefix, std::vector<std::string>& out) {
    for (const std::string& f : listFiles(dir)) {
        if (f == ".gitkeep" || endsWith(f, ".autosave") || endsWith(f, ".tmp")) continue;
        out.push_back(prefix + f);
    }
    for (const std::string& d : listDirectories(dir)) walk(dir + "/" + d, prefix + d + "/", out);
}

// Binary kinds: not scanned for references
bool binary(std::string_view p) {
    for (const char* ext : {".glb", ".png", ".jpg", ".jpeg", ".wav", ".ogg", ".hdr", ".otf", ".ttf", ".mkprobe", ".bin", ".cube"})
        if (endsWith(p, ext)) return true;
    return false;
}

// Data the engine opens by fixed name or by folder, always packed (small text and fonts)
bool alwaysPacked(std::string_view p) {
    std::string_view local = p;
    if (startsWith(local, "modules/")) { // modules/<name>/<asset path>
        const size_t slash = local.find('/', 8);
        local = slash == std::string_view::npos ? std::string_view() : local.substr(slash + 1);
    }
    for (const char* ext : {".table", ".tuning", ".bindings", ".events", ".ui", ".fx", ".otf", ".ttf"})
        if (endsWith(local, ext)) return true;
    return startsWith(local, "strings/") || startsWith(local, "fonts/");
}

// Words of a text that could be asset paths
void words(std::string_view text, std::vector<std::string>& out) {
    const auto pathChar = [](unsigned char c) {
        return c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
               c == '/' || c == '-' || c == '\\';
    };
    for (size_t i = 0; i < text.size();) {
        while (i < text.size() && !pathChar(static_cast<unsigned char>(text[i]))) ++i;
        const size_t start = i;
        while (i < text.size() && pathChar(static_cast<unsigned char>(text[i]))) ++i;
        if (i - start < 3) continue;
        std::string w(text.substr(start, i - start));
        std::replace(w.begin(), w.end(), '\\', '/');
        if (startsWith(w, "assets/")) w.erase(0, 7);
        if (w.find('.') != std::string::npos) out.push_back(std::move(w));
    }
}

// String literals in game source code ("ui/hud.ui")
void literals(std::string_view text, std::vector<std::string>& out) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '"') continue;
        const size_t end = text.find_first_of("\"\n", i + 1);
        if (end == std::string_view::npos) break;
        if (text[end] == '"') {
            std::string w(text.substr(i + 1, end - i - 1));
            if (startsWith(w, "assets/")) w.erase(0, 7);
            if (!w.empty()) out.push_back(std::move(w));
        }
        i = end;
    }
}

void sourceFiles(const std::string& dir, std::vector<std::string>& out) {
    if (!directoryExists(dir)) return;
    for (const std::string& f : listFiles(dir))
        if (endsWith(f, ".cpp") || endsWith(f, ".h")) out.push_back(dir + "/" + f);
    for (const std::string& d : listDirectories(dir)) sourceFiles(dir + "/" + d, out);
}

} // namespace

bool cookGame(const std::string& outPack) {
    const std::string projectFile = findAsset(kProjectFileName);
    if (projectFile.empty()) {
        MEALKIT_WARN("Cook: %s not found", kProjectFileName);
        return false;
    }
    const std::string projectDir = parentDirectory(projectFile);
    ProjectFile project;
    std::string error;
    if (!loadProjectFile(projectFile, project, &error)) {
        MEALKIT_WARN("Cook: %s: %s", projectFile.c_str(), error.c_str());
        return false;
    }

    // Every asset (paths relative to assets, module assets as modules/<name>/...)
    std::vector<std::string> all;
    walk(projectDir + "/assets", "", all);
    for (const std::string& m : listDirectories(projectDir + "/modules"))
        if (directoryExists(projectDir + "/modules/" + m + "/assets")) walk(projectDir + "/modules/" + m + "/assets", "modules/" + m + "/", all);
    const std::set<std::string> exists(all.begin(), all.end());

    std::set<std::string> reached;
    std::deque<std::string> queue;
    const auto reach = [&](const std::string& p, const char* from) {
        if (exists.count(p)) {
            if (reached.insert(p).second) queue.push_back(p);
            return true;
        }
        bool folder = false; // packInclude folders
        for (const std::string& f : all)
            if (startsWith(f, p + "/")) {
                folder = true;
                if (reached.insert(f).second) queue.push_back(f);
            }
        if (!folder && from) MEALKIT_WARN("Cook: %s names %s, which is not under assets/", from, p.c_str());
        return folder;
    };
    for (const std::string& f : all)
        if (alwaysPacked(f)) reach(f, nullptr);
    reach(project.settings.scene, "the start scene");
    std::vector<std::string> listed;
    words(project.settings.scenes, listed);
    for (const std::string& s : listed) reach(s, "scenes");
    listed.clear();
    for (std::string_view rest = project.settings.packInclude; !rest.empty();) { // folders have no dot, so not words()
        const size_t space = rest.find(' ');
        const std::string w(rest.substr(0, space));
        if (!w.empty()) reach(startsWith(w, "assets/") ? w.substr(7) : w, "packInclude");
        rest = space == std::string_view::npos ? std::string_view() : rest.substr(space + 1);
    }
    while (!queue.empty()) {
        const std::string p = queue.front();
        queue.pop_front();
        if (binary(p) || endsWith(p, ".gltf")) continue;
        std::string text;
        if (!readTextFile(projectDir + "/" + assetFilePath(p), text)) continue;
        std::vector<std::string> refs;
        words(text, refs);
        for (const std::string& r : refs) reach(r, nullptr);
    }

    // Cook and pack
    PackWriter pack;
    u32 failed = 0;
    for (const std::string& p : reached) {
        const std::string packed = assetFilePath(p);
        const std::string file = projectDir + "/" + packed;
        if (endsWith(p, ".glb") || endsWith(p, ".gltf")) {
            asset::CookedModel model;
            std::string modelError, cameraError;
            const bool isModel = asset::importModel(file, model, &asset::loadCompressedTexture, modelError);
            if (isModel) pack.add(packed + asset::kCookedModelSuffix, asset::writeCookedModel(model));
            std::vector<asset::CameraClip> clips;
            const bool isCamera = asset::loadCameraClips(file, clips, cameraError);
            if (isCamera) pack.add(packed + asset::kCookedCameraSuffix, asset::writeCameraClips(clips));
            if (!isModel && !isCamera) {
                MEALKIT_WARN("Cook: %s: %s", p.c_str(), modelError.c_str());
                ++failed;
            }
            continue;
        }
        if (endsWith(p, ".hdr")) {
            asset::EnvironmentMap env;
            std::string bytes;
            const std::string cache = asset::loadEnvironment(file, env, error) ? asset::environmentCacheFile(file) : std::string();
            if (cache.empty() || !readTextFile(cache, bytes)) {
                MEALKIT_WARN("Cook: %s: %s", p.c_str(), cache.empty() ? error.c_str() : "cannot read the baked cache");
                ++failed;
            } else pack.add(packed + asset::kCookedEnvironmentSuffix, bytes, false); // RGB9E5 texels barely compress
            continue;
        }
        std::string bytes;
        if (!readTextFile(file, bytes)) {
            MEALKIT_WARN("Cook: cannot read %s", file.c_str());
            ++failed;
            continue;
        }
        const bool compressed = endsWith(p, ".png") || endsWith(p, ".jpg") || endsWith(p, ".jpeg") || endsWith(p, ".ogg");
        pack.add(packed, bytes, !compressed);
    }
    // Project settings without the dev PC lines (like the old Export copy)
    {
        std::string text, kept;
        readTextFile(projectFile, text);
        for (size_t pos = 0; pos < text.size();) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            const std::string_view line = std::string_view(text).substr(pos, end - pos);
            const size_t start = line.find_first_not_of(" \t");
            const std::string_view key = start == std::string_view::npos ? std::string_view() : line.substr(start, line.find_first_of(" \t=", start) - start);
            if (key != "engine" && key != "engineVersion") (kept += line) += '\n';
            pos = end + 1;
        }
        pack.add(kProjectFileName, kept);
    }

    // Asset paths in code that weren't reached (opened by path string: add them to packInclude)
    std::vector<std::string> sources;
    for (const char* dir : {"source", "samples"}) sourceFiles(projectDir + "/" + dir, sources);
    for (const std::string& m : listDirectories(projectDir + "/modules")) sourceFiles(projectDir + "/modules/" + m + "/source", sources);
    std::set<std::string> warned;
    for (const std::string& s : sources) {
        std::string text;
        if (!readTextFile(s, text)) continue;
        std::vector<std::string> found;
        literals(text, found);
        for (const std::string& f : found)
            if (exists.count(f) && !reached.count(f) && warned.insert(f).second)
                MEALKIT_WARN("Cook: %s names assets/%s, which nothing else references - not packed (add it to packInclude in "
                             "Project Settings)", s.c_str(), f.c_str());
    }

    if (!pack.write(outPack, error)) {
        MEALKIT_WARN("Cook: %s", error.c_str());
        return false;
    }
    char hash[24];
    std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(pack.contentHash()));
    std::string manifest = "# MealKit pack manifest (written by --cook)\ncontent " + std::string(hash) + "\nfiles " +
                           std::to_string(pack.count()) + "\n";
    for (const std::string& p : reached) manifest += "asset " + p + "\n";
    writeTextFile(outPack + ".txt", manifest);
    MEALKIT_INFO("Cook: %zu of %zu assets -> %s (%zu entries, %.1f MB before compression, content %s)%s", reached.size(), all.size(),
                 outPack.c_str(), pack.count(), static_cast<double>(pack.bytes()) / (1024.0 * 1024.0), hash,
                 failed ? " - some assets failed, see above" : "");
    return true;
}

#else

bool cookGame(const std::string&) {
    MEALKIT_WARN("--cook needs a development build (build.bat), shipping builds don't read source assets");
    return false;
}

#endif

} // namespace mealkit::game
