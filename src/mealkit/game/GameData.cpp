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

void LiveTuning::open(const reflect::TypeInfo& type, const std::string& path) {
    m_value = reflect::Instance(type);
    m_path = path.empty() ? findAsset(kViewTuningAsset) : path;
    if (m_path.empty()) { // 아직 없다: game.tuning 옆에 생기면 읽는다
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
    if (m_writeTime == 0) return; // 파일 없음: 기본값 그대로
    reflect::Instance fresh(*m_value.type()); // 지운 줄은 기본값으로 돌아가게 새 값에 읽는다
    std::string error;
    if (!reflect::loadTextFile(*m_value.type(), fresh.data(), m_path, &error)) {
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
