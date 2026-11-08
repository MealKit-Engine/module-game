#pragma once

#include "mealkit/core/Log.h"
#include "mealkit/core/Reflect.h"
#include "mealkit/core/Services.h"
#include "mealkit/core/Types.h"
#include "mealkit/render/Renderer.h"

#include <string>

namespace mealkit {
class Window;
namespace audio {
class Engine;
}
namespace feedback {
class Player;
}
namespace input {
class Actions;
}
namespace loc {
class Strings;
}
namespace ui {
class System;
}
namespace settings {
class Service;
}
namespace asset {
class ModelCache;
}
namespace net {
class LockstepSession;
}
namespace scene {
struct Scene;
}
namespace game {
class MatchService;
}
} // namespace mealkit

// Bundle of engine services used by the game's view side (app/, view/) (docs/CODING_MODEL.md "How to access engine features").
// The engine (shipping exe main, general-purpose editor) creates it and passes it to each GameInstance function. The game doesn't keep it.
// sim/ doesn't receive this (only World&). Add game UI/debug UI here as they appear (bump kApiVersion then).
namespace mealkit::game {

struct GameContext {
    Window& window; // window (cursor capture, focus). Game input is actions
    render::Renderer& renderer;
    asset::ModelCache& models;
    net::LockstepSession& session;
    // Open/join/leave matches (mealkit/game/MatchService.h): called by screen flow menus (host, join, playOffline, start, returnToLobby, leave)
    MatchService& match;
    audio::Engine& audio; // sound event playback (mealkit/audio/Audio.h). Without a device, calls do nothing
    feedback::Player& feedback; // event → feedback table (mealkit/feedback/Feedback.h): trigger per tick event, camera shake, hitstop
    // Input actions (mealkit/input/Actions.h, assets/input.bindings): read like actions.axis2("move") without looking at keys directly.
    // The engine updates them every frame before update (devices not allowed are in released state). window.input() is for engine/debug keys only.
    input::Actions& actions;
    // Game string table (mealkit/loc/Strings.h, assets/strings/<language>.txt): text shown to players via strings.get("key")
    const loc::Strings& strings;
    // Game screen UI (mealkit/ui/Ui.h, assets/ui/*.ui): open → bind (view model) → fill the view model every frame, button actions via actions()
    ui::System& ui;
    // Player settings (mealkit/settings/Settings.h): read with values() (FOV etc., never put into the simulation), standard settings screen with open()
    settings::Service& settings;

    // Values the engine sets every frame
    render::Viewport viewport{}; // game screen area (the scene view in the editor)
    bool keyboardAllowed = true; // editor: only when the game view has the keyboard
    bool mouseAllowed = true;
    // View tuning (the game's ViewTuningType value, assets/view.tuning). The engine rereads it when the file changes (even during multiplayer play).
    // The game reads it with viewTuning<ViewTuning>(). Values may change next frame, so don't keep it.
    reflect::View viewTuningData{};
    // Per-player file folder (saves mealkit/game/Save.h, settings, rebound keys). If empty, nothing is saved (automated runs, join windows)
    std::string userFolder;
    // Match scene (same as what's passed to start). Used for pre-match screens (background behind menus). May change next frame, so don't keep it
    const scene::Scene* scene = nullptr;

    // Services outside this struct (modules a game adds to its preset, such as online): core/Services.h registry.
    // Including the module's header already needs the module in the build, and its owner provides it before game code runs.
    template <class T>
    T& get() const {
        return services::get<T>();
    }

    template <class T>
    const T& viewTuning() const {
        MEALKIT_ASSERT(!viewTuningData.data || viewTuningData.type == &reflect::typeOf<T>());
        if (!viewTuningData.data) { // the engine didn't pass it (no ViewTuningType in the game module): the declaration's defaults
            static const T defaults{};
            return defaults;
        }
        return *static_cast<const T*>(viewTuningData.data);
    }
};

// Provides every GameContext member as a service for one scope (game exe main, editor play session), so code that has no
// GameContext (engine modules, tools) finds them with services::get<T>(). Withdrawn in reverse order when the scope ends.
class ContextServices {
public:
    explicit ContextServices(const GameContext& ctx)
        : m_window(ctx.window), m_renderer(ctx.renderer), m_models(ctx.models), m_session(ctx.session), m_match(ctx.match),
          m_audio(ctx.audio), m_feedback(ctx.feedback), m_actions(ctx.actions), m_strings(ctx.strings), m_ui(ctx.ui),
          m_settings(ctx.settings) {}

private:
    ServiceScope<Window> m_window;
    ServiceScope<render::Renderer> m_renderer;
    ServiceScope<asset::ModelCache> m_models;
    ServiceScope<net::LockstepSession> m_session;
    ServiceScope<MatchService> m_match;
    ServiceScope<audio::Engine> m_audio;
    ServiceScope<feedback::Player> m_feedback;
    ServiceScope<input::Actions> m_actions;
    ServiceScope<const loc::Strings> m_strings;
    ServiceScope<ui::System> m_ui;
    ServiceScope<settings::Service> m_settings;
};

} // namespace mealkit::game
