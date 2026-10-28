#include "mealkit/game/GameOptions.h"

#include "mealkit/core/Version.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace mealkit::game {

void printGameUsage(const char* name, bool headless) {
    if (headless) std::printf("MealKit headless (no window, renderer or audio; bot input)\n");
    else std::printf("%s (MealKit %s)\n", name, MEALKIT_VERSION);
    std::printf("Match:\n"
                "  --host [port]          host a match (default: the game's default port, game.defaultPort in mealkit.project)\n"
                "  --join <addr[:port]>   join a host\n");
    if (!headless)
        std::printf("  --steam                host a Steam friends-only lobby (invite with I or Shift+Tab)\n"
                    "  --steam-join <lobby>   join a Steam lobby by id (invites from Steam join automatically)\n");
    std::printf("  --local                host on 127.0.0.1 only (same-PC testing)\n"
                "  --players <n>          max players when hosting (default: the game's default, max 8)\n"
                "  --auto-start <n>       host: start when n players are in the lobby%s\n"
                "  --match-data <n>       match setting chosen by the host and sent to everyone (the game decides; alias --bodies)\n"
                "  --delay <ms>           fixed input delay in milliseconds (default: automatic from the players' ping, mealkit.project)\n"
                "  --seed <n>             world seed\n"
                "  --name <text>          player name in the lobby (rejoining with the same name takes the old slot)\n"
                "  --threads <n>          physics worker threads (default: auto)\n"
                "  --scene <file>         scene to play (default: scene in mealkit.project)\n"
                "  --tuning <file>        tuning values (default assets/game.tuning)\n"
                "Tests:\n"
                "  --exit-tick <t>        print the state hash after tick t and exit%s\n"
                "  --shadow               run a shadow simulation and compare state hashes every tick\n"
                "  --hash-log <file>      write \"<tick> <hash>\" for every tick\n"
                "  --desync-test <t>      nudge the physics before tick t to cause a desync (dumps go to build/desync)\n"
                "  --record-inputs <file> save the confirmed inputs and hashes of every tick\n",
                headless ? " (default: --players)" : "", headless ? " (required unless replaying)" : "");
    if (headless)
        std::printf("  --replay-inputs <file> replay a recorded match and compare every tick hash (no network)\n");
    std::printf("  --any-build            host: accept peers running a different build (compiler comparison tests)\n"
                "  --net-lag <ms>         add round-trip time to this process's connection (testing)\n"
                "  --net-jitter <ms>      vary each packet's delay by up to this much (testing)\n"
                "  --net-loss <percent>   drop this share of sent and received packets (testing)\n");
    if (headless) {
        std::printf("Window options (--bot, --vsync, --fps, --size, --debug, ...) are accepted and ignored.\n");
        return;
    }
    std::printf("Window:\n"
                "  --vsync <0|1>          vertical sync (default 1)\n"
                "  --fps <n>              frame cap when vsync is off\n"
                "  --render-scale <s>     fixed 3D resolution scale 0.25-1 (dynamic resolution off)\n"
                "  --size <w> <h>         window size\n"
                "  --debug                enable the D3D12 debug layer\n"
                "  --gpu-capture          load RenderDoc or PIX if installed; F11 captures the next frame (development builds)\n"
                "  --bot                  drive the local player with scripted input\n"
                "  --rewind-test          record inputs and snapshots, rewind and replay at the exit tick\n"
                "  --capture <png> [tick] save a screenshot after the given tick (no tick: the last of --frames)\n"
                "  --frames <n>           draw n frames and exit (screens outside a match, e.g. the title)\n"
                "  --ui-press <f:node,..> press the UI widget named node at frame f (screen flow tests)\n"
                "  --system-info          print CPU/GPU capabilities and exit (exit code 2 if below minimum spec)\n"
                "  --mute                 start with sound off (automated tests)\n"
                "  --debug-ui             start with the debug UI shown (F1, development builds)\n"
                "  --settings             start with the settings screen open\n"
                "  --lang <code>          language of the game text (assets/strings/<code>.txt, default: language in mealkit.project)\n"
                "  --benchmark [seconds]  fixed seed, bot input, vsync off: measure 30 s after a 3 s warm-up, write benchmark.txt, exit\n"
                "  --benchmark-out <txt>  benchmark report path (default benchmark.txt)\n"
                "  --headless --help      run without a window (headless options, replay)\n");
}

bool parseGameArgs(int argc, char** argv, MatchOptions& o, WindowOptions* window) {
    WindowOptions ignored; // 헤드리스: 창 옵션은 읽고 버린다
    WindowOptions& w = window ? *window : ignored;
    const bool headless = window == nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char*& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        auto hasValue = [&] { return i + 1 < argc && argv[i + 1][0] != '-'; };
        auto u32Of = [](const char* v) { return static_cast<u32>(std::atoi(v)); };
        const char* v = nullptr;
        // ---- 매치
        if (a == "--host") {
            o.mode = MatchOptions::Mode::Host;
            if (hasValue()) o.port = static_cast<u16>(std::atoi(argv[++i]));
        } else if (a == "--join" && next(v)) {
            o.mode = MatchOptions::Mode::Join;
            o.joinAddress = v;
        } else if (a == "--steam" && !headless) o.mode = MatchOptions::Mode::SteamHost;
        else if ((a == "--steam-join" || a == "+connect_lobby") && !headless && next(v)) { // +connect_lobby: Steam 초대 수락
            o.mode = MatchOptions::Mode::SteamJoin;
            o.steamLobby = std::strtoull(v, nullptr, 10);
        } else if (a == "--local") o.loopbackOnly = true;
        else if (a == "--players" && next(v)) o.maxPlayers = u32Of(v);
        else if (a == "--auto-start" && next(v)) o.autoStart = u32Of(v);
        else if ((a == "--match-data" || a == "--bodies") && next(v)) o.matchData = u32Of(v);
        else if (a == "--delay" && next(v)) o.inputDelayMs = u32Of(v), o.delayGiven = true;
        else if (a == "--seed" && next(v)) o.seed = std::strtoull(v, nullptr, 10);
        else if (a == "--name" && next(v)) o.playerName = v;
        else if (a == "--threads" && next(v)) o.physicsThreads = u32Of(v);
        else if (a == "--scene" && next(v)) o.scenePath = v;
        else if (a == "--tuning" && next(v)) o.tuningPath = v;
        else if (a == "--exit-tick" && next(v)) o.exitTick = u32Of(v);
        else if (a == "--shadow") o.shadow = true;
        else if (a == "--desync-test" && next(v)) o.desyncTestTick = u32Of(v);
        else if (a == "--hash-log" && next(v)) o.hashLogPath = v;
        else if (a == "--record-inputs" && next(v)) o.recordInputsPath = v;
        else if (a == "--replay-inputs" && headless && next(v)) o.replayInputsPath = v;
        else if (a == "--replay" && !headless && next(v)) o.replayInputsPath = v;
        else if (a == "--any-build") o.anyBuild = true;
        else if (a == "--net-lag" && next(v)) o.netLagMs = static_cast<f32>(std::atof(v));
        else if (a == "--net-jitter" && next(v)) o.netJitterMs = static_cast<f32>(std::atof(v));
        else if (a == "--net-loss" && next(v)) o.netLossPercent = static_cast<f32>(std::atof(v));
        // ---- 창
        else if (a == "--headless") continue; // 출시 exe의 main이 이미 나눴다
        else if (a == "--vsync" && next(v)) w.vsync = std::atoi(v) != 0, w.windowGiven = true;
        else if (a == "--render-scale" && next(v)) w.renderScale = static_cast<f32>(std::atof(v));
        else if (a == "--fps" && next(v)) w.fpsCap = std::atof(v), w.windowGiven = true;
        else if (a == "--size" && i + 2 < argc) {
            w.windowGiven = true;
            w.width = u32Of(argv[++i]);
            w.height = u32Of(argv[++i]);
        } else if (a == "--debug") w.debugLayer = true;
        else if (a == "--bot") w.bot = true;
        else if (a == "--rewind-test") w.rewindTest = true;
        else if (a == "--capture" && i + 1 < argc) { // <png> [tick]
            w.capturePath = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') w.captureTick = u32Of(argv[++i]);
        } else if (a == "--frames" && next(v)) w.frames = u32Of(v);
        else if (a == "--ui-press" && next(v)) { // "프레임:노드,프레임:노드"
            const std::string list = v;
            size_t at = 0;
            while (at < list.size()) {
                size_t end = list.find(',', at);
                if (end == std::string::npos) end = list.size();
                const std::string item = list.substr(at, end - at);
                const size_t colon = item.find(':');
                if (colon != std::string::npos)
                    w.uiPresses.emplace_back(u32Of(item.substr(0, colon).c_str()), item.substr(colon + 1));
                at = end + 1;
            }
        }
        else if (a == "--system-info") w.systemInfo = true;
        else if (a == "--mute") w.mute = true;
        else if (a == "--debug-ui") w.debugUi = true;
        else if (a == "--settings") w.openSettings = true;
        else if (a == "--lang" && next(v)) w.language = v;
        else if (a == "--gpu-capture") w.gpuCapture = true;
        else if (a == "--gpu-capture-at" && next(v)) w.gpuCapture = true, w.gpuCaptureAt = u32Of(v);
        else if (a == "--benchmark") { // [초]
            w.benchmarkSeconds = 30.0;
            if (i + 1 < argc && argv[i + 1][0] != '-') w.benchmarkSeconds = std::max(1.0, std::atof(argv[++i]));
        } else if (a == "--benchmark-out" && next(v)) w.benchmarkPath = v;
        else if (a == "--help" || a == "-h") return false;
        else {
            std::printf("Unknown option: %s\n", a.c_str());
            return false;
        }
    }
#if defined(MEALKIT_DEBUG)
    w.debugLayer = true;
#endif
    return true;
}

} // namespace mealkit::game
