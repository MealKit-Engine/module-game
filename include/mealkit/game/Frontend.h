#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/game/GameContext.h"
#include "mealkit/game/GameData.h"
#include "mealkit/game/MatchService.h"
#include "mealkit/input/Actions.h"
#include "mealkit/loc/Strings.h"
#include "mealkit/platform/Gamepad.h"
#include "mealkit/settings/Settings.h"
#include "mealkit/ui/Ui.h"

#include <string>

namespace mealkit {
class FrameLimiter;
class Input;
class Window;
} // namespace mealkit

// Bundle of view-side services for windowed games (docs/CODING_MODEL.md "Frame loop for running windowed games").
// The shipping exe (game::runGame), editor play, and join windows each have one and run it in the same order:
//   front.init(config);                                  // read bindings/strings/UI/settings (calling again rereads)
//   GameContext ctx = front.context(window, renderer, models, match);   // match: MatchService holding the session
//   every frame: set ctx.viewport/allow flags → front.update(now, dt, input, padAllowed, ctx) → game.update(ctx, dt)
//             → game.buildFrame → front.afterBuildFrame(frame) → after 3D front.renderUi(renderer)
// Sound/feedback are created and updated by the owner (executable, editor). The editor shares them with the [Sound]/[Feedback] previews.
namespace mealkit::game {

// Standard subtitle screen (assets/ui/subtitles.ui, if present): at the bottom of the screen when a sound with subtitles plays (player setting gameplay.subtitles)
MEALKIT_STRUCT(SubtitleLineVm) {
    MEALKIT_FIELD(std::string, text, {}, "Subtitle text");
};
MEALKIT_VIEWMODEL(SubtitlesVm) {
    MEALKIT_LIST(SubtitleLineVm, lines, 4, "Subtitle lines");
};
inline constexpr const char* kSubtitleScreen = "ui/subtitles.ui"; // relative to assets
inline constexpr i32 kSubtitleLayer = 5;                         // above HUD, below menus

struct FrontendConfig {
    render::Renderer* renderer = nullptr; // where the game UI draws
    audio::Engine* audio = nullptr;
    feedback::Player* feedback = nullptr;
    // Where to apply the display items of player settings (window mode/size/vsync/frame limit). nullptr for the editor/automated runs
    Window* displayWindow = nullptr;
    FrameLimiter* limiter = nullptr;
    std::string language;        // game text language (projectLanguage if empty)
    std::string projectLanguage; // language from mealkit.project (default language for looking up missing keys)
    std::string bindingsUserFile; // key file changed by the player. If empty, only project bindings
    std::string settingsFile;     // settings.ini. If empty, runs on defaults and doesn't save
    // View tuning (the game's ViewTuningType). If nullptr, the owner passes ctx.viewTuningData directly (editor)
    const reflect::TypeInfo* viewTuningType = nullptr;
    std::string viewTuningPath; // assets/view.tuning if empty
};

class Frontend {
public:
    Frontend() = default;
    Frontend(const Frontend&) = delete;
    Frontend& operator=(const Frontend&) = delete;

    void init(const FrontendConfig& config);
    // GameContext pointing at the services (window size/allow flags are set by the owner every frame)
    GameContext context(Window& window, render::Renderer& renderer, asset::ModelCache& models, MatchService& match);

    // Before game update, once per frame. If the UI takes the mouse/keyboard, turns off ctx's allow flags.
    // padAllowed: whether to pass gamepad input (shipping exe: window focus, editor: when the game view has the keyboard)
    void update(f64 now, f64 dt, const Input& input, bool padAllowed, GameContext& ctx);
    // After game.buildFrame: world-space UI camera, sound listener position, feedback screen color (added to the frame's tint)
    void afterBuildFrame(render::FrameParams& frame);
    // After 3D, before debug/editor UI
    void renderUi(render::Renderer& renderer);

    input::Actions& actions() { return m_actions; }
    loc::Strings& strings() { return m_strings; }
    ui::System& ui() { return m_ui; }
    settings::Service& settings() { return m_settings; }

private:
    FrontendConfig m_config;
    input::Actions m_actions;
    Gamepads m_pads;
    loc::Strings m_strings;
    ui::System m_ui;
    settings::Service m_settings;
    LiveTuning m_viewTuning;
    ui::ScreenId m_subtitleScreen = ui::kNoScreen;
    SubtitlesVm m_subtitleVm;
    bool m_hasSubtitleScreen = false;
};

} // namespace mealkit::game
