#pragma once

#include "mealkit/core/Types.h"

#include <string>
#include <utility>
#include <vector>

// Command line of game executables (shipping exe, --headless, <game>_headless): the parser and --help are one set.
//   Match options (MatchOptions): create/join room, player count, seed, automated test output. Headless and windowed exes use the same values,
//   and screen flow menus (create/join room) fill the same struct in code to open a session.
//   Window options (WindowOptions): window, renderer, view-side tests. Headless accepts and ignores them (so both can run with the same command line).
namespace mealkit::game {

struct MatchOptions {
    enum class Mode { Offline, Host, Join, SteamHost, SteamJoin } mode = Mode::Offline;
    u16 port = 0; // 0 = game defaults() (usually game.defaultPort in mealkit.project)
    std::string joinAddress;
    u64 steamLobby = 0;     // SteamJoin: lobby id
    bool loopbackOnly = false; // 127.0.0.1 only (same-PC tests, no firewall prompt)
    u32 maxPlayers = 0;     // 0 = game defaults()
    u32 autoStart = 0;      // host: start when this many players gather (headless 0 = maxPlayers)
    u32 matchData = 0;      // match setting decided by the game (set by the host and sent to everyone. sandbox: box count, 0 = scene)
    u32 inputDelayMs = 100; // --delay <ms>: fixed if given, otherwise auto adjustment from project settings (this is the initial value)
    u32 inputDelay = 3;     // ticks (set from inputDelayMs and the tick rate after reading the project)
    bool delayGiven = false;
    u64 seed = 0;           // 0 = current time
    u32 physicsThreads = 0; // 0 = auto
    u32 exitTick = 0;       // after running this tick, write FINAL and finish (required for headless unless replaying)
    bool shadow = false;    // shadow simulation
    u32 abortHostTick = 0;  // --abort-host-at: the first successor host quits without announcing the disconnect once past this tick (simulated crash, host migration test)
    u32 desyncTestTick = 0; // --desync-test: perturbs physics slightly at this tick to cause a desync on purpose (desync diagnostics test)
    bool anyBuild = false;  // host: also accept peers on other builds (compiler comparison tests)
    std::string playerName; // --name: lobby name (rejoining with the same name gets the old slot, reconnect tests)
    f32 netLagMs = 0.0f, netJitterMs = 0.0f, netLossPercent = 0.0f; // network condition simulation (mealkit/net/Conditions.h)
    std::string scenePath, tuningPath;
    std::string hashLogPath, recordInputsPath;
    std::string replayInputsPath; // headless --replay-inputs (hash comparison), windowed --replay (viewing)
    std::string testPath;         // headless --test: scenario file (game/Scenario.h)
    bool finalState = false;      // --final-state: print the numbers of the state at the last tick ("STATE path value")
    bool writeKinds = false;      // --write-kinds: write build/editor/kinds.txt (object kinds for MkRender) and exit
    bool updateExpected = false;  // --update-expected: rewrite the scenario's == values with the results
};
using HeadlessOptions = MatchOptions; // old name (defaults of game headless classes)

struct WindowOptions {
    bool vsync = true;
    f64 fpsCap = 0.0;
    u32 width = 1280, height = 720;
    bool windowGiven = false; // --size/--vsync/--fps given: for this run the command line wins over display items in player settings
    f32 renderScale = 0.0f;   // --render-scale: fixed 3D resolution scale (dynamic resolution off, 0 = per settings)
    bool debugLayer = false;
    bool bot = false;        // local player driven by scripted input (always for headless)
    bool rewindTest = false; // at the end, rewind the recording, rerun it, and print the result
    std::string capturePath;
    u32 captureTick = 0;      // if 0, the last frame of --frames (menu screen capture)
    u32 frames = 0;           // draw this many and finish (out-of-match screen tests, same as the editor's --frames)
    std::vector<std::pair<u32, std::string>> uiPresses; // --ui-press: press the widget with this name on this frame (screen flow tests)
    bool systemInfo = false;   // print only CPU/GPU info and finish
    bool mute = false;         // mute sound (when opening multiple windows in automated tests)
    bool debugUi = false;      // start with the debug UI (F1) on (dev builds)
    bool openSettings = false; // start with the settings screen open
    std::string language;      // game text language (language in mealkit.project if empty)
    bool gpuCapture = false;   // --gpu-capture: load RenderDoc/PIX and capture the next frame with F11 (dev builds)
    u32 bugReportAt = 0;       // --bug-report-at <frame>: leave a bug report as if F9 were pressed on that frame (automated tests, no note)
    u32 crashAt = 0;           // --crash-at <frame>: crash on purpose on that frame (crash report tests, game/BugReport.h)
    u32 gpuCaptureAt = 0;      // --gpu-capture-at <frame>: capture that frame (automated tests, doesn't open the RenderDoc window)
    f64 benchmarkSeconds = 0.0; // --benchmark [seconds]: benchmark mode if not 0 (mealkit/game/Benchmark.h)
    std::string benchmarkPath = "benchmark.txt";
    std::string cookPath; // --cook: write the shipping pack and exit (game/Cook.h)
    std::string packPath; // --pack: mount this pack instead of the *.pak next to the exe (play the cooked game in a dev build)
};

// If window is nullptr, headless: window options are accepted and ignored, Steam isn't accepted. false for unknown options/--help.
bool parseGameArgs(int argc, char** argv, MatchOptions& match, WindowOptions* window);
// --help output (name: game name, nullptr for headless)
void printGameUsage(const char* name, bool headless);

} // namespace mealkit::game
