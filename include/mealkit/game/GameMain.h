#pragma once

#include "mealkit/game/BuildInfo.h"
#include "mealkit/game/GameInstance.h"
#include "mealkit/game/Headless.h"
#include "mealkit/platform/System.h"
#include "mealkit/platform/Window.h"

#include <string_view>
#include <type_traits>

// main of the shipping exe (docs/CODING_MODEL.md "Frame loop for running windowed games"). The game writes just one line in app/main.cpp:
//   MEALKIT_GAME_MAIN(sandbox::GameSession, sandbox::Headless)
// the engine handles the command line (--help), window/renderer, session (solo/host/join/Steam lobby), view-side services (game::Frontend), debug UI (F1),
// automated test output (FINAL, SHADOW, REWIND), and window title (string keys game.title, title.*). --headless goes to the headless entry point.
namespace mealkit::game {

struct GameMainSpec {
    const GameModule* module = nullptr;
    // (optional) defaults of the headless class: port/player count defaults (gameProject is the value of GameModule::projectSettingsType)
    void (*defaults)(MatchOptions& options, const void* gameProject) = nullptr;
    BuildInfo build; // this game build (game version/commit/simulation version, made in the game's main.cpp)
};

// What to change when something other than the shipping exe runs the same frame loop (join windows of editor multiplayer play, editor::runPlayer).
// Defaults are the shipping exe as is.
struct GameRunOptions {
    const ProjectFile* project = nullptr; // already read project settings. If nullptr, loadMatchData finds it
    std::string title;                    // first part of the window title. If empty, string key game.title + game version
    u32 captionColor = kDefaultCaptionColor;
    std::string viewTuningPath;       // view tuning file. assets/view.tuning if empty (reread when changed)
    bool playerFiles = true;          // false: don't read/write settings, rebound keys, saves in the user folder (multiple windows on the same PC)
    bool onlineService = true;        // false: don't log into the online service (Steam)
    bool exitWhenSessionEnds = false; // exit when the session ends or the game leaves the match (menu)
};

// Reads the command line and calls runGame below (--help, --system-info)
int runGame(int argc, char** argv, const GameMainSpec& spec);
// Window, renderer, session, Frontend, frame loop. Exit codes (0 normal, 2 desync, 3 session ended during a test, 4 rewind failure, 5 shadow desync)
int runGame(MatchOptions match, const WindowOptions& window, const GameMainSpec& spec, const GameRunOptions& run = {});

template <class GameClass, class HeadlessClass>
GameMainSpec gameMainSpec(const char* name) {
    GameMainSpec spec;
    spec.module = &gameModuleOf<GameClass>(name);
    spec.build = MEALKIT_THIS_BUILD;
    if constexpr (requires { typename HeadlessClass::ProjectSettingsType; }) {
        using Project = typename HeadlessClass::ProjectSettingsType;
        if constexpr (requires { typename GameClass::ProjectSettingsType; })
            static_assert(std::is_same_v<Project, typename GameClass::ProjectSettingsType>,
                          "GameClass and HeadlessClass must use the same ProjectSettingsType");
        if constexpr (requires(MatchOptions& o, const Project& p) { HeadlessClass::defaults(o, p); })
            spec.defaults = [](MatchOptions& o, const void* p) {
                HeadlessClass::defaults(o, *static_cast<const Project*>(p));
            };
    }
    return spec;
}

} // namespace mealkit::game

// Used once in the game's app/main.cpp. MEALKIT_GAME_NAME is the project name (exe name) passed by mealkit_add_game()
#define MEALKIT_GAME_MAIN(GameClass, HeadlessClass)                                                                    \
    int main(int argc, char** argv) {                                                                                  \
        ::mealkit::attachParentConsole(); /* the shipping exe is a GUI program */                                                 \
        for (int i = 1; i < argc; ++i)                                                                                 \
            if (std::string_view(argv[i]) == "--headless") return mealkitHeadlessMain(argc, argv);                     \
        return ::mealkit::game::runGame(argc, argv,                                                                    \
                                        ::mealkit::game::gameMainSpec<GameClass, HeadlessClass>(MEALKIT_GAME_NAME));   \
    }
