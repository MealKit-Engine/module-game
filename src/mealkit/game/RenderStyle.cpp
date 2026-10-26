#include "mealkit/game/RenderStyle.h"

#include "mealkit/core/Log.h"
#include "mealkit/platform/Image.h"
#include "mealkit/platform/Paths.h"

namespace mealkit::game {

namespace {
constexpr u32 kMaxRampWidth = 1024; // 램프는 첫 줄만 쓴다. 더 큰 그림은 줄여 읽는다
} // namespace

bool applyRenderStyle(const ProjectSettings& settings, render::Renderer& renderer, RenderStyleState& state) {
    const std::string rampFile = settings.toonRamp.empty() ? std::string() : findAsset("assets/" + settings.toonRamp);
    const unsigned long long rampTime = rampFile.empty() ? 0 : fileWriteTime(rampFile);
    if (state.applied && state.lightingModel == settings.lightingModel && state.toonRamp == settings.toonRamp &&
        state.rampTime == rampTime)
        return false;

    const bool modelChanged = !state.applied || state.lightingModel != settings.lightingModel;
    renderer.setLightingModel(static_cast<render::LightingModel>(settings.lightingModel));
    if (!state.applied || state.toonRamp != settings.toonRamp || state.rampTime != rampTime) {
        Image ramp;
        if (settings.toonRamp.empty()) {
            renderer.setToonRamp(nullptr);
        } else if (!rampFile.empty() && loadImage(rampFile, kMaxRampWidth, ramp)) {
            renderer.setToonRamp(&ramp);
        } else {
            MEALKIT_WARN("Toon ramp assets/%s: cannot read the image, using the default 3 steps", settings.toonRamp.c_str());
            renderer.setToonRamp(nullptr);
        }
    }
    if (modelChanged && state.applied)
        MEALKIT_INFO("Lighting model: %s", reflect::enumName(settings.lightingModel).c_str());
    state.applied = true;
    state.lightingModel = settings.lightingModel;
    state.toonRamp = settings.toonRamp;
    state.rampTime = rampTime;
    return true;
}

} // namespace mealkit::game
