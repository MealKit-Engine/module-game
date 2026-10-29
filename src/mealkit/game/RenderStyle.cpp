#include "mealkit/game/RenderStyle.h"

#include "mealkit/core/Log.h"
#include "mealkit/platform/Image.h"
#include "mealkit/platform/Paths.h"

namespace mealkit::game {

namespace {
constexpr u32 kMaxRampWidth = 1024; // only the first row of the ramp is used. Larger images are downscaled on read
} // namespace

bool applyRenderStyle(const ProjectSettings& settings, render::Renderer& renderer, RenderStyleState& state) {
    renderer.setLightsPerObject(settings.pointLightsPerObject); // just a few values, so passed every time
    renderer.setShadows({settings.shadows, settings.shadowCascades, settings.shadowMapSize, settings.shadowDistance});
    const f32 gpuBudgetMs = settings.gpuBudgetOpaqueMs + settings.gpuBudgetShadowMs + settings.gpuBudgetPostMs + settings.gpuBudgetVfxMs +
                            settings.gpuBudgetUiMs + settings.gpuBudgetSpareMs;
    renderer.setDynamicResolution(settings.dynamicResolution, settings.dynamicResolutionMinScale, gpuBudgetMs, settings.upscaleSharpness);
    const std::string rampFile = settings.toonRamp.empty() ? std::string() : findAssetFile(settings.toonRamp);
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
