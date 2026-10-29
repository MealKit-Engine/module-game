#pragma once

#include "mealkit/game/ProjectSettings.h"
#include "mealkit/render/Renderer.h"

#include <string>

// Passes the project settings' style preset (lighting model, toon ramp image) to the renderer (docs/CODING_MODEL.md "Shader code").
// Called by the shipping exe/editor after reading the project; the editor calls it again when project settings change or the ramp image file changes.
namespace mealkit::game {

// Last passed values. If the same, not reread (the editor calls every 0.5 s)
struct RenderStyleState {
    bool applied = false;
    LightingModelSetting lightingModel = LightingModelSetting::Pbr;
    std::string toonRamp;
    unsigned long long rampTime = 0; // ramp image file time (reread when changed)
};

// If something changed, passes it and returns true. If the ramp image is missing or unreadable, warns and uses the engine default 3 steps
bool applyRenderStyle(const ProjectSettings& settings, render::Renderer& renderer, RenderStyleState& state);

} // namespace mealkit::game
