#include "mealkit/game/GameData.h"

#include "mealkit/asset/ModelCache.h"
#include "mealkit/core/Log.h"
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
    if (!reflect::loadTextFile(type, data, file, &error)) {
        MEALKIT_ERROR("Cannot read tuning %s: %s", file.c_str(), error.c_str());
        return {};
    }
    return file;
}

u64 contentHash(const scene::Scene& scene, asset::ModelCache& models, const scene::TypeList* gameTypes,
                const reflect::TypeInfo* tuningType, const void* tuning, reflect::View project,
                reflect::View gameProject) {
    Hasher h;
    h.add(scene.contentHash(gameTypes));
    h.add(models.hashModels(scene));
    if (tuningType && tuning) reflect::hashStruct(*tuningType, tuning, h);
    if (project.type && project.data) reflect::hashStruct(*project.type, project.data, h, reflect::kFieldView);
    if (gameProject.type && gameProject.data)
        reflect::hashStruct(*gameProject.type, gameProject.data, h, reflect::kFieldView);
    return h.value;
}

} // namespace mealkit::game
