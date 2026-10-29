#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/core/TextFormat.h"
#include "mealkit/core/Types.h"
#include "mealkit/game/ProjectSettings.h"
#include "mealkit/platform/Paths.h"

#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// Desync dump (docs/ROADMAP.md stage 3 "Desync diagnostic tools"): when the host reports a tick hash mismatch, each peer writes that tick's state as text.
// game::Simulation keeps a few snapshots in multiplayer matches, and reruns from the snapshot before the diverged tick up to that tick to dump
// (the host notices a few ticks late, so it's the state at the diverged tick, not the current state).
// Comparing two peers' files with tools\desync_diff.ps1 shows the first differing tick, differing state fields, and differing bodies.
//
// Format (text, line based):
//   # MealKit desync dump
//   @format desync 1
//   player 1 / tick 812 / seed 000000.. / content 000000.. / note <description>   header (key value)
//   [hashes]  "<tick> <hash hex>" (those still kept up to the diverged tick)
//   [state]   "<field path> = <value>" (declared state fields, reflect::toText)
//   [bodies]  physics::PhysicsWorld::writeState lines
namespace mealkit::game {

inline constexpr const char* kDesyncFolder = "desync";

struct DesyncDumpHeader {
    u32 player = 0;
    u32 tick = 0;
    u64 seed = 0;
    u64 contentHash = 0;
    std::string note; // if not empty, why the dump is incomplete, etc.
};

// Dump folder: during development (the project folder has build\) <project>/build/desync, shipping builds desync/ in the user folder
inline std::string desyncDumpFolder(const std::string& userFolder) {
    const std::string projectFile = findAsset(kProjectFileName);
    const std::string project = projectFile.empty() ? std::string() : parentDirectory(projectFile);
    if (!project.empty() && directoryExists(project + "/build")) return project + "/build/" + kDesyncFolder;
    return userFolder.empty() ? std::string() : userFolder + "/" + kDesyncFolder;
}

inline std::string desyncDumpPath(const std::string& folder, u64 seed, u32 tick, u32 player) {
    char name[96];
    std::snprintf(name, sizeof(name), "desync_%016llx_t%u_p%u.txt", static_cast<unsigned long long>(seed), tick,
                  player + 1);
    return folder + "/" + name;
}

// world: state right after running the diverged tick. hashes: (tick, hash) oldest first
template <class World>
bool writeDesyncDump(const std::string& path, const DesyncDumpHeader& h, const World& world,
                     const std::vector<std::pair<u32, u64>>& hashes) {
    std::string out = "# MealKit desync dump. Compare two peers with tools\\desync_diff.ps1 -A <file> -B <file>\n";
    out += text::formatLine(text::kDesync) + "\n";
    char line[160];
    std::snprintf(line, sizeof(line), "player %u\ntick %u\nseed %016llx\ncontent %016llx\n", h.player + 1, h.tick,
                  static_cast<unsigned long long>(h.seed), static_cast<unsigned long long>(h.contentHash));
    out += line;
    if (!h.note.empty()) out += "note " + h.note + "\n";
    out += "[hashes]\n";
    for (const auto& [tick, hash] : hashes) {
        std::snprintf(line, sizeof(line), "%u %016llx\n", tick, static_cast<unsigned long long>(hash));
        out += line;
    }
    out += "[state]\n";
    reflect::TextFields fields;
    reflect::toText(reflect::typeOf<std::remove_cvref_t<decltype(world.state)>>(), &world.state, fields);
    for (const auto& [key, value] : fields) out += key + " = " + value + "\n";
    out += "[bodies]\n";
    world.physics.writeState(out);
    if (!parentDirectory(path).empty()) createDirectories(parentDirectory(path));
    return writeTextFile(path, out);
}

} // namespace mealkit::game
