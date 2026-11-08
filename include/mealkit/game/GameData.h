#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/game/ProjectSettings.h"
#include "mealkit/session/Session.h"
#include "mealkit/scene/Scene.h"

#include <string>

namespace mealkit::asset {
class ModelCache;
}

// Match data (scene, models, tuning): the game executable and editor read it the same way and produce the same content hash.
namespace mealkit::game {

inline constexpr const char* kDefaultTuningAsset = "assets/game.tuning";
inline constexpr const char* kViewTuningAsset = "assets/view.tuning";

// Reads the tuning file. If path is empty, assets/game.tuning. If the file is missing, the declaration's defaults as is (warning log).
// Returns the path of the file read (empty string if none).
std::string loadTuning(const reflect::TypeInfo& type, void* data, const std::string& path = {});

// View tuning file (assets/view.tuning, the game's ViewTuningType): not in the content hash, reread when the file changes.
// The game executable, join windows, and editor each have one, poll() it every frame, and pass it via GameContext::viewTuningData.
// The editor uses values edited in the panel directly (set), and during multiplayer play rewrites the copy file join windows read.
class LiveTuning {
public:
    // If path is empty, assets/view.tuning. If the file is missing, the declaration's defaults.
    void open(const reflect::TypeInfo& type, const std::string& path = {});
    void close();
    // Checks the file time a few times per second and rereads if it changed. true if reread.
    bool poll(f64 now);
    reflect::View view() const { return {m_value.type(), m_value.data()}; }
    const std::string& path() const { return m_path; }

private:
    void reload();

    reflect::Instance m_value;
    std::string m_path;
    unsigned long long m_writeTime = 0;
    f64 m_nextCheck = 0.0;
};

// Game types used for match data (shipping exe/editor via gameTypes(module) from GameModule, headless from the game class)
struct GameTypes {
    const scene::TypeList* objectTypes = nullptr;          // scene object kinds (MEALKIT_OBJECT)
    const reflect::TypeInfo* tuningType = nullptr;         // assets/game.tuning
    const reflect::TypeInfo* projectSettingsType = nullptr; // game.* items in mealkit.project (nullptr if none)
    const reflect::TableList* tables = nullptr;            // definition data tables (MEALKIT_TABLE)
};

struct MatchDataOptions {
    std::string scenePath;  // project start scene if empty (default arena if the file is missing)
    std::string tuningPath; // assets/game.tuning if empty
    const scene::Scene* scene = nullptr;     // existing scene (editor: the scene being edited). If given, scenePath isn't read
    const ProjectFile* project = nullptr;    // already read project settings (editor). If nullptr, found via loadGameProject
};

// Data needed for one match: the shipping exe, headless, editor play, and join windows all read it with the single loadMatchData.
// The same function builds the content hash from the same seven things (scene, models, object kinds, tuning, project settings, game project settings, tables), so
// peer hashes and input recordings don't diverge across run modes.
struct MatchData {
    ProjectFile project;
    reflect::Instance gameProject; // game item values (types.projectSettingsType, empty if none)
    scene::Scene scene;            // original (content hash, GameInstance::start). The simulation uses it flattened via scene::flattened
    reflect::Instance tuning;      // types.tuningType value
    u64 contentHash = 0;
    u64 contentParts[static_cast<u32>(session::ContentPart::Count)] = {}; // per-component hashes (connection rejection reasons)
};

// On failure (the given scene file can't be read), logs and returns false. models reads the model files used for the hash (CPU data only is fine).
bool loadMatchData(const GameTypes& types, const MatchDataOptions& options, asset::ModelCache& models, MatchData& out);

// Hash compared on connect: scene (parts affecting game results) + model files + tuning values
// + project settings (engine ProjectSettings and game items that aren't MEALKIT_VIEW_FIELD) + simulation tables (MEALKIT_TABLE that aren't kTableView).
u64 contentHash(const scene::Scene& scene, asset::ModelCache& models, const scene::TypeList* gameTypes,
                const reflect::TypeInfo* tuningType, const void* tuning, reflect::View project = {},
                reflect::View gameProject = {}, const reflect::TableList* tables = nullptr);

} // namespace mealkit::game
