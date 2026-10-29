#include "mealkit/game/GameData.h"

#include "mealkit/asset/ModelCache.h"
#include "mealkit/core/Log.h"
#include "mealkit/data/Tables.h"
#include "mealkit/platform/Paths.h"

namespace mealkit::game {

std::string loadTuning(const reflect::TypeInfo& type, void* data, const std::string& path) {
    const std::string file = path.empty() ? findAsset(kDefaultTuningAsset) : path;
    if (file.empty() || !fileExists(file)) {
        MEALKIT_WARN("Tuning file %s not found - using the defaults in %s",
                     path.empty() ? kDefaultTuningAsset : path.c_str(), type.name);
        return {};
    }
    std::string error;
    if (!reflect::loadTextFile(type, data, file, text::kTuning, &error)) {
        MEALKIT_ERROR("Cannot read tuning %s: %s", file.c_str(), error.c_str());
        return {};
    }
    return file;
}

void LiveTuning::open(const reflect::TypeInfo& type, const std::string& path) {
    m_value = reflect::Instance(type);
    m_path = path.empty() ? findAsset(kViewTuningAsset) : path;
    if (m_path.empty()) { // not there yet: read when it appears next to game.tuning
        const std::string gameTuning = findAsset(kDefaultTuningAsset);
        if (!gameTuning.empty()) m_path = parentDirectory(gameTuning) + "/view.tuning";
    }
    m_writeTime = 0;
    m_nextCheck = 0.0;
    reload();
}

void LiveTuning::close() {
    m_value = {};
    m_path.clear();
}

void LiveTuning::reload() {
    m_writeTime = m_path.empty() ? 0 : fileWriteTime(m_path);
    if (m_writeTime == 0) return; // no file: defaults as is
    reflect::Instance fresh(*m_value.type()); // read into a fresh value so deleted lines revert to defaults
    std::string error;
    if (!reflect::loadTextFile(*m_value.type(), fresh.data(), m_path, text::kTuning, &error)) {
        MEALKIT_WARN("Cannot read %s: %s", m_path.c_str(), error.c_str());
        return;
    }
    m_value = std::move(fresh);
}

bool LiveTuning::poll(f64 now) {
    if (!m_value.type() || m_path.empty() || now < m_nextCheck) return false;
    m_nextCheck = now + 0.25;
    if (fileWriteTime(m_path) == m_writeTime) return false;
    reload();
    MEALKIT_INFO("Reloaded %s", m_path.c_str());
    return true;
}

bool loadMatchData(const GameTypes& types, const MatchDataOptions& options, asset::ModelCache& models, MatchData& out) {
    out.gameProject = types.projectSettingsType ? reflect::Instance(*types.projectSettingsType) : reflect::Instance();
    if (options.project) {
        out.project = *options.project;
        if (types.projectSettingsType) applyGameSettings(out.project, *types.projectSettingsType, out.gameProject.data());
    } else {
        loadGameProject(out.project, types.projectSettingsType, out.gameProject.data());
    }
    applyTickRate(out.project.settings); // time fields (seconds in tuning/scenes/tables) are read with this tick rate
    asset::ModelBudget budgets[asset::kModelKindCount];
    modelBudgets(out.project.settings, budgets);
    models.setBudgets(budgets); // models over budget warn on load
    models.setProbeLimit(out.project.settings.probeMaxCount);
    if (options.scene) {
        out.scene = *options.scene;
    } else {
        const std::string scenePath =
            options.scenePath.empty() ? findAsset(startSceneAsset(out.project.settings)) : options.scenePath;
        if (!scenePath.empty()) {
            std::string error;
            if (!scene::load(out.scene, scenePath, &error)) {
                MEALKIT_ERROR("Cannot load scene %s: %s", scenePath.c_str(), error.c_str());
                return false;
            }
            MEALKIT_INFO("Scene: %s (%zu entities)", scenePath.c_str(), out.scene.entities.size());
        } else {
            out.scene = scene::makeDefault();
            MEALKIT_INFO("Scene file not found - using the built-in arena");
        }
    }
    out.tuning = types.tuningType ? reflect::Instance(*types.tuningType) : reflect::Instance();
    if (types.tuningType) loadTuning(*types.tuningType, out.tuning.data(), options.tuningPath);
    out.contentHash = contentHash(out.scene, models, types.objectTypes, types.tuningType, out.tuning.data(),
                                  {&reflect::typeOf<ProjectSettings>(), &out.project.settings},
                                  {types.projectSettingsType, out.gameProject.data()}, types.tables);
    // Per component (to report which one differs; the combined hash is the one above)
    u64* parts = out.contentParts;
    parts[static_cast<u32>(net::ContentPart::Scene)] = out.scene.contentHash(types.objectTypes);
    parts[static_cast<u32>(net::ContentPart::Models)] = models.hashModels(out.scene);
    Hasher tuningHash, projectHash, tablesHash;
    if (types.tuningType) reflect::hashStruct(*types.tuningType, out.tuning.data(), tuningHash);
    reflect::hashStruct(reflect::typeOf<ProjectSettings>(), &out.project.settings, projectHash, reflect::kFieldView);
    if (types.projectSettingsType) reflect::hashStruct(*types.projectSettingsType, out.gameProject.data(), projectHash, reflect::kFieldView);
    if (types.tables) data::hashTables(*types.tables, tablesHash);
    parts[static_cast<u32>(net::ContentPart::Tuning)] = tuningHash.value;
    parts[static_cast<u32>(net::ContentPart::Project)] = projectHash.value;
    parts[static_cast<u32>(net::ContentPart::Tables)] = tablesHash.value;
    return true;
}

u64 contentHash(const scene::Scene& scene, asset::ModelCache& models, const scene::TypeList* gameTypes,
                const reflect::TypeInfo* tuningType, const void* tuning, reflect::View project,
                reflect::View gameProject, const reflect::TableList* tables) {
    Hasher h;
    h.add(scene.contentHash(gameTypes));
    h.add(models.hashModels(scene));
    if (tuningType && tuning) reflect::hashStruct(*tuningType, tuning, h);
    if (project.type && project.data) reflect::hashStruct(*project.type, project.data, h, reflect::kFieldView);
    if (gameProject.type && gameProject.data)
        reflect::hashStruct(*gameProject.type, gameProject.data, h, reflect::kFieldView);
    if (tables) data::hashTables(*tables, h);
    return h.value;
}

} // namespace mealkit::game
