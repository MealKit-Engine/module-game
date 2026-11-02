// main of the shipping exe (mealkit/game/GameMain.h). The game's app/main.cpp calls it via MEALKIT_GAME_MAIN.
//
//   <game>.exe                         games with menus start at the title (GameInstance::menus), otherwise solo play
//   <game>.exe --host [port]           create a room (starts at the lobby. Games without menus start with Enter)
//   <game>.exe --join 127.0.0.1[:port] join
//   <game>.exe --scene my.scene        play a different scene (all players must have the same file)
//   <game>.exe --headless ...          simulation only without a window (the game's app/Headless.cpp, same as <game>_headless.exe)
//
// The start scene comes from mealkit.project; default port/player count from the game headless class's defaults() (usually game.* items in mealkit.project).
// Engine keys: V vsync, F12 screenshot, F1 debug UI (dev builds), Esc quit (games without menus only), I invite friends (online lobby)

#include "mealkit/game/GameMain.h"

#include "mealkit/asset/ModelCache.h"
#if MEALKIT_DEBUG_UI
#include "mealkit/debug/DebugDrawRender.h"
#endif
#include "mealkit/audio/Audio.h"
#include "mealkit/core/Clock.h"
#include "mealkit/core/Log.h"
#include "mealkit/core/Profile.h"
#include "mealkit/core/Version.h"
#include "mealkit/platform/Pack.h"
#include "mealkit/feedback/Feedback.h"
#include "mealkit/game/Benchmark.h"
#include "mealkit/game/BugReport.h"
#include "mealkit/game/Cook.h"
#include "mealkit/game/FrameStats.h"
#include "mealkit/game/DesyncDump.h"
#include "mealkit/game/Frontend.h"
#include "mealkit/game/GameData.h"
#include "mealkit/game/GameOptions.h"
#include "mealkit/game/InputRecord.h"
#include "mealkit/game/MatchService.h"
#include "mealkit/game/ProjectSettings.h"
#include "mealkit/game/RenderStyle.h"
#include "mealkit/live/Server.h"
#include "mealkit/net/Lockstep.h"
#include "mealkit/net/Socket.h"
#include "mealkit/online/Online.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Process.h"
#include "mealkit/platform/System.h"
#include "mealkit/platform/Window.h"
#include "mealkit/render/DrawList.h"
#include "mealkit/rhi/GpuCapture.h"
#include "mealkit/render/Renderer.h"
#include "mealkit/scene/Scene.h"

#if MEALKIT_DEBUG_UI
#include "mealkit/debug/ImGuiLayer.h"

#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <ctime>
#include <string>
#include <utility>

namespace mealkit::game {
namespace {

// Prints min-spec hardware measurement results in a shape that's easy to copy into the CHANGELOG.
int printSystemInfo() {
    std::printf("CPU: %s\n", cpuName().c_str());
    render::GpuInfo gpu;
    std::string error;
    if (!render::queryGpu(gpu, &error)) {
        std::printf("GPU: none (%s)\n", error.c_str());
        return 2;
    }
    std::printf("Graphics API: %s (Feature level, Shader Model and binding tier are Direct3D 12 only)\n", gpu.api.c_str());
    std::printf("GPU: %s (vendor 0x%04X, device 0x%04X)\n"
                "Driver: %s\n"
                "Memory: %llu MB dedicated, %llu MB shared, %s\n"
                "Feature level: %u_%u\n"
                "Shader Model: %u.%u\n"
                "Resource binding tier: %u\n"
                "Wave lanes: %u-%u\n"
                "Mesh shader tier: %u\n",
                gpu.adapter.c_str(), gpu.vendorId, gpu.deviceId,
                gpu.driverVersion.empty() ? "unknown" : gpu.driverVersion.c_str(),
                static_cast<unsigned long long>(gpu.dedicatedVideoMemory >> 20),
                static_cast<unsigned long long>(gpu.sharedSystemMemory >> 20), gpu.uma ? "UMA" : "discrete",
                gpu.featureLevel >> 12, (gpu.featureLevel >> 8) & 0xF, gpu.shaderModel >> 4, gpu.shaderModel & 0xF,
                gpu.resourceBindingTier, gpu.waveLaneCountMin, gpu.waveLaneCountMax, gpu.meshShaderTier);
    const std::string below = render::gpuMinimumError(gpu);
    std::printf("Minimum spec: %s\n", below.empty() ? "OK" : below.c_str());
    return below.empty() ? 0 : 2;
}

// When --benchmark finishes: the report file and the BENCHMARK line read by automated tests
bool writeBenchmark(const Benchmark& bench, const ProjectSettings& project, const GameMainSpec& spec,
                    const render::Renderer& renderer, const MatchOptions& match, const std::string& path) {
    BenchmarkInfo info;
    info.game = project.name.empty() ? spec.module->name : project.name;
    info.gameVersion = spec.build.gameVersion;
    info.commit = spec.build.commit;
    char sim[24];
    std::snprintf(sim, sizeof(sim), "%016llx", static_cast<unsigned long long>(spec.build.simVersion));
    info.simVersion = sim;
    info.determinismGroup = net::determinismGroup();
    info.cpu = cpuName();
    info.gpu = renderer.gpu();
    info.width = renderer.width();
    info.height = renderer.height();
    info.vsync = renderer.vsync();
    info.seed = match.seed;
    info.scene = match.scenePath.empty() ? project.scene : match.scenePath;
    const f32 budgets[] = {project.gpuBudgetOpaqueMs, project.gpuBudgetShadowMs, project.gpuBudgetPostMs,
                           project.gpuBudgetVfxMs,    project.gpuBudgetUiMs,     project.gpuBudgetSpareMs};
    static_assert(std::size(budgets) == std::size(info.gpuBudgetMs), "one budget per GPU pass + spare");
    std::copy(std::begin(budgets), std::end(budgets), std::begin(info.gpuBudgetMs));

    const BenchmarkResult result = bench.result();
    const std::string text = Benchmark::report(info, result);
    std::printf("%s\n", Benchmark::summaryLine(result).c_str());
    std::fflush(stdout);
    if (!writeTextFile(path, text)) {
        MEALKIT_ERROR("Cannot write the benchmark report: %s", path.c_str());
        return false;
    }
    MEALKIT_INFO("Benchmark report: %s", path.c_str());
    return true;
}

// When --exit-tick is reached: writes the lines read by automated tests (FINAL, SHADOW, REWIND) and returns the exit code
int reportFinish(GameInstance& game, const net::LockstepSession& session, const MatchOptions& match,
                 const WindowOptions& win) {
    std::printf("FINAL tick=%u hash=%016llx desync=%d\n", game.nextTick() - 1,
                static_cast<unsigned long long>(game.stateHash()), session.desynced() ? 1 : 0);
    int exitCode = session.desynced() ? 2 : 0;
    if (SimDebug* debug = game.debug(); debug && (match.shadow || win.rewindTest)) {
        if (match.shadow) {
            const ShadowStatus& shadow = debug->shadow();
            std::printf("SHADOW %s tick=%u\n", shadow.desynced ? "DESYNC" : "ok", shadow.tick);
            if (shadow.desynced) exitCode = 5;
        }
        if (win.rewindTest) { // rewinds the whole recorded range bit by bit, reruns, returns to the present, and checks the hashes match
            const u64 liveHash = game.stateHash();
            debug->setPaused(true);
            const Timeline t = debug->timeline();
            for (u32 tick = t.first; tick < t.live; tick += 37) debug->seek(tick);
            debug->setPaused(false);
            const bool ok = debug->replayError().empty() && game.stateHash() == liveHash;
            std::printf("REWIND %s ticks=%u..%u memory=%.1fMB %s\n", ok ? "ok" : "FAIL", t.first, t.live,
                        static_cast<f64>(t.memoryBytes) / (1024.0 * 1024.0), debug->replayError().c_str());
            if (!ok) exitCode = 4;
        }
    }
    std::fflush(stdout);
    return exitCode;
}

// Status written in the window title when no match is in progress (title.* keys in the game string table). Empty string if in progress or in a menu
std::string lobbyStatus(const MatchService& match, const loc::Strings& strings) {
    const net::LockstepSession& session = match.session();
    const auto num = [](u32 v) { return std::to_string(v); };
    switch (match.phase()) {
    case MatchPhase::Opening:
        return strings.get(match.config().online && match.config().online->lobbyState() == online::LobbyState::Creating
                               ? "title.steamCreating"
                               : "title.steamJoining");
    case MatchPhase::Connecting: return strings.format("title.connecting", {{"count", num(session.lobbyPlayerCount())}});
    case MatchPhase::Lobby:
        if (session.isOffline()) return {};
        if (session.isHost())
            return strings.format(match.usesOnline() ? "title.lobbyHostSteam" : "title.lobbyHost",
                                  {{"count", num(session.lobbyPlayerCount())}, {"max", num(session.maxPlayers())}});
        return strings.format("title.waiting", {{"count", num(session.lobbyPlayerCount())}});
    case MatchPhase::Ended: return strings.format("title.ended", {{"reason", match.error()}});
    default: return {};
    }
}

} // namespace

void printBuildStamp() {
    std::string stamp;
    if (readTextFile(executableDirectory() + "/build.txt", stamp)) std::printf("\nThis build (build.txt):\n%s", stamp.c_str());
}

void mountGamePacks(const std::string& explicitPack) {
    std::vector<std::string> packs;
    if (!explicitPack.empty()) packs.push_back(explicitPack);
    else
        for (const std::string& f : listFiles(executableDirectory(), ".pak")) packs.push_back(executableDirectory() + "/" + f);
    for (const std::string& p : packs) {
        std::string error;
        if (mountPack(p, error)) MEALKIT_INFO("Pack: %s (content %016llx)", p.c_str(), static_cast<unsigned long long>(mountedPackHash()));
        else MEALKIT_FATAL("Cannot read the game data: %s", error.c_str());
    }
}

int runGame(int argc, char** argv, const GameMainSpec& spec) {
    MatchOptions opt;
    WindowOptions win;
    if (!parseGameArgs(argc, argv, opt, &win)) {
        printGameUsage(spec.module->name, false);
        printBuildStamp();
        return 1;
    }
    if (!win.cookPath.empty()) return cookGame(win.cookPath) ? 0 : 1;
    mountGamePacks(win.packPath);
    if (win.systemInfo) return printSystemInfo();
    if (win.benchmarkSeconds > 0.0) { // benchmark: the same match every time (fixed seed + bots), no display sync, no sound
        if (opt.mode != MatchOptions::Mode::Offline) {
            std::printf("--benchmark runs offline only (no --host/--join)\n");
            return 1;
        }
        win.bot = true;
        win.mute = true;
        if (!win.windowGiven) win.vsync = false;
        if (opt.seed == 0) opt.seed = 1;
    }
    return runGame(std::move(opt), win, spec);
}

int runGame(MatchOptions opt, const WindowOptions& win, const GameMainSpec& spec, const GameRunOptions& run) {
    const GameModule& module = *spec.module;
    const bool benchmark = win.benchmarkSeconds > 0.0;
    const bool automated = opt.exitTick > 0 || win.frames > 0 || benchmark;
    if (automated) log::setDialogs(false);

    MEALKIT_INFO("CPU: %s", cpuName().c_str()); // for recording determinism on other PCs
    MEALKIT_INFO("Build: version %s, commit %s, simulation %016llx, %s", spec.build.gameVersion, spec.build.commit,
                 static_cast<unsigned long long>(spec.build.simVersion), net::determinismGroup());
    Window window;
    if (!window.create({run.title.empty() ? module.name : run.title.c_str(), win.width, win.height, run.captionColor}))
        return 1;

    render::Renderer renderer;
    if (!renderer.init({window.nativeHandle(), window.width(), window.height(), win.vsync, win.debugLayer, win.gpuCapture}))
        MEALKIT_FATAL("Renderer initialization failed.\n%s", renderer.initError());
    asset::ModelCache models;
    models.setRenderer(&renderer);

    // Match data (same function as headless/editor play): project settings (next to the exe for shipping, the project folder's mealkit.project during development),
    // scene, tuning, content hash. The executable + this hash must match to connect
    MatchData data;
    MatchDataOptions dataOptions;
    dataOptions.scenePath = opt.scenePath;
    dataOptions.tuningPath = opt.tuningPath;
    dataOptions.project = run.project;
    if (!loadMatchData(gameTypes(module), dataOptions, models, data))
        MEALKIT_FATAL("Cannot load the match data (see log)");
    const ProjectFile& project = data.project;
    RenderStyleState renderStyle;
    applyRenderStyle(project.settings, renderer, renderStyle); // lighting model/toon ramp (renderer.init already built the pipelines)
    if (spec.defaults && data.gameProject.data()) spec.defaults(opt, data.gameProject.data()); // same defaults as headless (port/player count)
    opt.inputDelay = msToTicks(opt.inputDelayMs); // after the tick rate (project setting) is known
    const scene::Scene& scene = data.scene;

#if MEALKIT_LIVE
    // Live connection (contract between tools, mealkit/live/Server.h): dev builds only. Connection info is in the project's build/live/game-<pid>.json
    live::Server liveServer;
    {
        live::ServerConfig c;
        c.kind = "game";
        const std::string projectFile = findAsset(kProjectFileName);
        c.projectDir = projectFile.empty() ? std::string() : parentDirectory(projectFile);
        c.projectName = project.settings.name.empty() ? module.name : project.settings.name;
        liveServer.start(c);
        liveServer.addTopic("stats"); // game::FrameStats::json, every report period
    }
#endif

    // Online services (Steam/EOS): if included in the build, logs in at startup (menu [Create Steam Room], accepting friend invites)
    const bool steamAsked = opt.mode == MatchOptions::Mode::SteamHost || opt.mode == MatchOptions::Mode::SteamJoin;
    online::Service online;
    if (online::Service::backend() != online::Backend::None && (!automated || steamAsked) && run.onlineService) online.init();
    if (steamAsked && !online.available())
        MEALKIT_FATAL("The online backend (%s) is not available. Steam: run the Steam client. EOS: check the internet "
                      "connection. Without an SDK the build has no backend (see " MEALKIT_DOCS_URL
                      "manual/multiplayer.html#steamworks).",
                      online::Service::backendName());

    // The session lives inside MatchService. The game's screen flow (menus) opens and joins rooms with the same functions
    MatchService matches;
    {
        MatchServiceConfig c;
        c.contentHash = data.contentHash;
        c.acceptOtherBuilds = opt.anyBuild;
        c.playerName = opt.playerName;
        c.gameName = project.settings.name.empty() ? module.name : project.settings.name;
        c.defaultPort = opt.port;
        c.defaultMaxPlayers = opt.maxPlayers;
        c.inputDelay = opt.inputDelay;
        c.loopbackOnly = opt.loopbackOnly;
        c.seed = opt.seed;
        c.conditions = {opt.netLagMs, opt.netJitterMs, opt.netLossPercent};
        c.build = spec.build;
        std::copy(std::begin(data.contentParts), std::end(data.contentParts), std::begin(c.contentParts));
        c.autoDelay = autoDelayOf(project.settings, 1000.0f / static_cast<f32>(reflect::tickRate()));
        c.autoDelay.enabled = c.autoDelay.enabled && !opt.delayGiven; // --delay: fixed
        c.online = online.available() ? &online : nullptr;
        matches.init(c);
    }
    net::LockstepSession& session = matches.session();

    GameInstance* game = module.create();
    const bool menus = game->menus();
    PlayOptions gameOptions;
    gameOptions.bot = win.bot;
    gameOptions.exitTick = opt.exitTick;
    gameOptions.capture = !win.capturePath.empty() && (win.captureTick > 0 || win.frames == 0); // without a tick, the last frame of --frames
    gameOptions.captureTick = win.captureTick;
    gameOptions.physicsThreads = opt.physicsThreads;
    gameOptions.tuningPath = opt.tuningPath;
    gameOptions.shadow = opt.shadow;
    gameOptions.record = win.rewindTest;
    gameOptions.hashLogPath = opt.hashLogPath;
    gameOptions.recordInputsPath = opt.recordInputsPath;
    gameOptions.desyncDumpDir = desyncDumpFolder(userFolder(project.settings));
    gameOptions.desyncTestTick = opt.desyncTestTick;
    // User folder (saves/settings/rebound keys): not used by automated runs (--exit-tick) and join windows
    const bool playerFiles = !automated && run.playerFiles;
    const std::string userFolderPath = playerFiles ? userFolder(project.settings) : std::string();
    // Replays (game/InputRecord.h): each match writes to replays/current.rec in the user folder and keeps it when the match ends.
    // With --record-inputs, only to that one file; nothing is written while watching a replay
    const bool watching = !opt.replayInputsPath.empty();
    const bool keepingReplays = gameOptions.recordInputsPath.empty() && !watching && !userFolderPath.empty() &&
                                project.settings.keepReplays > 0;
    // Crash reports (game/BugReport.h): a crash left by the previous run is bundled first, before current.rec is kept or reused
    const bool crashReports = project.settings.bugReports && (!automated || win.crashAt) && run.playerFiles;
    if (crashReports) {
        const std::string crashZip = collectCrashReport(userFolder(project.settings));
        if (!crashZip.empty() && !automated &&
            askDialog("Crash report",
                      ("The game crashed last time. A crash report was saved:\n" + crashZip + "\n\nOpen the folder?").c_str()))
            openInExplorer(parentDirectory(crashZip));
    }
    if (keepingReplays) {
        keepReplay(userFolderPath, project.settings.keepReplays); // recording left over from a previous run that ended mid-match
        gameOptions.recordInputsPath = replayRecordingPath(userFolderPath);
    }
    if (watching) gameOptions.recordInputsPath.clear();
    if (crashReports) {
        BugReportInfo info;
        info.build = spec.build;
        info.contentHash = data.contentHash;
        info.scenePath = opt.scenePath.empty() ? project.settings.scene : opt.scenePath;
        info.tuningPath = opt.tuningPath.empty() ? std::string(kDefaultTuningAsset) : opt.tuningPath;
        armCrashReport(userFolder(project.settings), info, gameOptions.recordInputsPath);
    }

    // Start decided by the command line: create/join a room starts at the lobby. Solo play starts at the menu for games with menus (automated runs/bots go straight to the match)
    if (watching) {
        InputRecording recording;
        std::string error;
        if (!loadInputRecording(opt.replayInputsPath, recording, error) || recording.inputs.empty())
            MEALKIT_FATAL("Cannot play the replay %s: %s", opt.replayInputsPath.c_str(),
                          error.empty() ? "it has no ticks" : error.c_str());
        if (recording.contentHash != data.contentHash) // still watch, but pause at the tick where it diverges
            MEALKIT_WARN("Replay %s was recorded with different game data (content hash %016llx, now %016llx)",
                         opt.replayInputsPath.c_str(), static_cast<unsigned long long>(recording.contentHash),
                         static_cast<unsigned long long>(data.contentHash));
        matches.playReplay(recording);
    } else switch (opt.mode) {
    case MatchOptions::Mode::Offline:
        if (!menus || opt.exitTick > 0 || win.bot) { // automated tests (--exit-tick)/bots go straight to the match without menus
            matches.playOffline();
            game->lobby(session, gameOptions, userFolderPath); // match start data (loadout)
            matches.start(opt.matchData);
        }
        break;
    case MatchOptions::Mode::Host:
        if (!matches.host(opt.port, opt.maxPlayers)) MEALKIT_FATAL("%s", matches.error().c_str());
        break;
    case MatchOptions::Mode::Join:
        if (!matches.join(opt.joinAddress)) MEALKIT_FATAL("%s", matches.error().c_str());
        break;
    case MatchOptions::Mode::SteamHost: matches.hostOnline(opt.maxPlayers); break;
    case MatchOptions::Mode::SteamJoin: matches.joinOnline(opt.steamLobby); break;
    }

    FrameLimiter limiter;
    limiter.setTargetFps(win.vsync ? 0.0 : win.fpsCap);

    // Sound: default output device (runs without sound if none). Muted when the window loses focus (can be turned off in player settings)
    audio::Engine audio;
    audio.init({project.settings.audioVoices, win.mute});
#if MEALKIT_LIVE
    audio::addLiveService(liveServer, [&audio] { return &audio; }); // MealKit Audio: playback in the game, reload, status
#endif
    feedback::Player feedback(audio); // assets/feedback.table: tick events → sound/shake/flash/hitstop

    // View-side services: input actions (assets/input.bindings + keys rebound by the player), string tables, game UI, player settings, view tuning.
    // Automated runs (--exit-tick) use default settings and don't save (so this PC's settings don't change test results; window size from the command line)
    Frontend front;
    {
        FrontendConfig c;
        c.renderer = &renderer;
        c.audio = &audio;
        c.feedback = &feedback;
        const bool display = !automated && !win.windowGiven;
        c.displayWindow = display ? &window : nullptr;
        c.limiter = &limiter;
        c.language = win.language;
        c.projectLanguage = project.settings.language;
        c.bindingsUserFile = run.playerFiles ? userFolder(project.settings) + "/" + input::kBindingsTable : std::string();
        c.settingsFile = playerFiles ? userFolderPath + "/" + settings::kSettingsFile : std::string();
        c.viewTuningType = module.viewTuningType ? module.viewTuningType() : nullptr; // reread when the file changes
        c.viewTuningPath = run.viewTuningPath;
        front.init(c);
        if (win.openSettings) front.settings().open();
    }
    // Dynamic resolution: off for automated runs (tests/benchmarks) so images don't vary with GPU speed. --render-scale fixes the scale
    if (automated || win.renderScale > 0.0f) {
        const render::ResolutionSettings& r = renderer.resolution();
        renderer.setDynamicResolution(false, r.minScale, r.budgetMs, r.sharpness);
        if (win.renderScale > 0.0f) renderer.setRenderScale(win.renderScale);
    }

    GameContext ctx = front.context(window, renderer, models, matches);
    ctx.userFolder = userFolderPath; // saves (automated runs don't save)
    ctx.scene = &scene;
#if MEALKIT_DEBUG_UI
    // Debug UI (F1): dev builds only (shipping build.bat Export has no ImGui). Created when first turned on (reads fonts)
    debug::ImGuiLayer debugUi;
    bool debugUiReady = false, debugUiShown = false;
    std::string perfText;
#endif
    render::DrawList drawList;
    f64 lastTime = nowSeconds();
    FrameStats perf; // performance numbers (window title/F1/perf log, mealkit/game/FrameStats.h)
    Benchmark bench({win.benchmarkSeconds}); // --benchmark (accumulates perf batches)
    u32 perfLogCounter = 0;
    int exitCode = 0;
    f64 exitDeadline = 0.0;  // exit time after reaching --exit-tick
    bool loadingShown = false; // the loading screen was drawn once after the session became Running (start on the next frame)
    u32 frameCount = 0;        // --frames
    bool replayStopped = false; // the replay diverged and paused
    // Bug report (F9, game/BugReport.h): in dev builds, a one-line note window → screenshot request → bundle on the next frame (after endFrame writes the PNG)
    enum class BugStep { None, Note, Shot, Write };
    BugStep bugStep = BugStep::None;
    char bugNote[256] = {};
    const std::string bugFolder = project.settings.bugReports && (!automated || win.bugReportAt) ? userFolder(project.settings) : std::string();
    const std::string bugShot = bugFolder + "/" + kBugReportFolder + "/screenshot.png";

    while (window.pumpMessages()) {
        const f64 now = nowSeconds();
        const f64 dt = std::min(now - lastTime, 0.25);
        lastTime = now;
        Input& input = window.input();
        if (exitDeadline > 0.0 && now >= exitDeadline) break;

        // Esc: games with menus use it themselves (pause/back). Games without menus quit (if the settings screen is open, it closes the screen)
        if (!menus && input.keyPressed(key::Escape) && !front.settings().isOpen() && bugStep == BugStep::None) break;
        if (input.keyPressed('V') && !front.ui().editingText()) renderer.setVsync(!renderer.vsync());
        if (input.keyPressed(key::F12)) renderer.requestCapture("screenshot.png");
        if (input.keyPressed(key::F11)) rhi::captureNextGpuFrame(); // --gpu-capture (does nothing without the tool)
        if (bugStep == BugStep::Write) {
            BugReportInfo info;
            info.build = spec.build;
            info.contentHash = data.contentHash;
            info.scenePath = opt.scenePath.empty() ? project.settings.scene : opt.scenePath;
            info.tuningPath = opt.tuningPath.empty() ? std::string(kDefaultTuningAsset) : opt.tuningPath;
            info.tick = game->started() && game->debug() ? game->debug()->timeline().live : 0;
            info.players = session.playerCount();
            info.localPlayer = session.localPlayer();
            info.note = bugNote;
            writeBugReport(bugFolder, info, game->started() ? gameOptions.recordInputsPath : std::string(), bugShot);
            bugNote[0] = 0;
            bugStep = BugStep::None;
        }
        if (bugStep == BugStep::Shot) {
            createDirectories(parentDirectory(bugShot));
            renderer.requestCapture(bugShot.c_str());
            bugStep = BugStep::Write;
        }
        if (!bugFolder.empty() && bugStep == BugStep::None && input.keyPressed(key::F9) && !front.ui().editingText())
#if MEALKIT_DEBUG_UI
            bugStep = BugStep::Note;
#else
            bugStep = BugStep::Shot; // shipping builds have no note window
#endif
        if (!bugFolder.empty() && win.bugReportAt && frameCount == win.bugReportAt) bugStep = BugStep::Shot;
        if (win.gpuCaptureAt && frameCount == win.gpuCaptureAt) rhi::captureNextGpuFrame(false);
        if (crashReports)
            updateCrashReport(game->started() && game->debug() ? game->debug()->timeline().live : 0, session.playerCount(),
                              session.localPlayer());
        if (win.crashAt && frameCount == win.crashAt) {
            MEALKIT_WARN("--crash-at: crashing on purpose at frame %u", frameCount);
            *static_cast<volatile int*>(nullptr) = 0;
        }
        if (window.consumeResize()) renderer.resize(window.width(), window.height());
        if (matches.usesOnline() && input.keyPressed('I') && !front.ui().editingText()) matches.inviteFriends();

        matches.update(now);
        const net::SessionState state = session.state();
        if (!game->started() && state != net::SessionState::Running && state != net::SessionState::Ended)
            game->lobby(session, gameOptions, ctx.userFolder); // lobby: decide the loadout (confirmed when the host starts)

        if (session.isHost() && state == net::SessionState::Lobby && !session.isOffline()) {
            const bool enter = !menus && input.keyPressed(key::Enter);
            if (enter || (opt.autoStart && session.lobbyPlayerCount() >= opt.autoStart)) matches.start(opt.matchData);
        }
        if (state == net::SessionState::Ended && opt.exitTick > 0 && exitDeadline == 0.0) {
            MEALKIT_ERROR("Session ended before the exit tick: %s", session.endReason());
            exitCode = 3;
            break;
        }
        if (run.exitWhenSessionEnds && exitDeadline == 0.0) {
            if (state == net::SessionState::Ended) {
                MEALKIT_INFO("Session ended: %s", session.endReason());
                exitCode = session.desynced() ? 2 : 0;
                break;
            }
            if (matches.phase() == MatchPhase::Menu) break; // the game pressed [Leave]
        }

        // Match start/end: start is on the frame after the loading screen is drawn once (if the game's screen flow is Running it opens the loading screen)
        if (!game->started() && session.state() == net::SessionState::Running) {
            if (loadingShown) {
                game->start(ctx, scene, gameOptions);
                audio.setReverbZones(scene);
                MEALKIT_INFO("World ready: %u player(s), you are player %u, %u moving bodies", session.playerCount(),
                             session.localPlayer(), game->activeBodies());
            }
            loadingShown = !loadingShown;
        } else if (game->started() && session.state() != net::SessionState::Running) {
            game->stop(ctx); // back to lobby, left, disconnected
            audio.setReverbZones(std::vector<audio::ReverbZone>{});
            loadingShown = false;
            if (keepingReplays) keepReplay(userFolderPath, project.settings.keepReplays);
        }
        // When the replay diverges from the recording (different build/data), pause at that tick
        if (session.isReplay() && session.desynced() && !replayStopped && game->debug()) {
            game->debug()->setPaused(true);
            replayStopped = true;
        }

        ctx.viewport = {0.0f, 0.0f, static_cast<f32>(window.width()), static_cast<f32>(window.height())};
        ctx.keyboardAllowed = ctx.mouseAllowed = window.focused();
#if MEALKIT_DEBUG_UI
        if (input.keyPressed(key::F1) || (win.debugUi && !debugUiReady) || (bugStep == BugStep::Note && !debugUiShown)) {
            if (!debugUiReady) debugUiReady = debugUi.init(window, renderer, {});
            debugUiShown = debugUiReady && !debugUiShown;
        }
        if (debugUiShown) { // over debug windows the game doesn't receive keys/mouse
            const ImGuiIO& io = ImGui::GetIO();
            if (io.WantCaptureKeyboard) ctx.keyboardAllowed = false;
            if (io.WantCaptureMouse) ctx.mouseAllowed = false;
        }
#endif
        front.update(now, dt, input, window.focused(), ctx);
        for (const auto& [pressFrame, node] : win.uiPresses) // screen flow tests: press the widget on this frame (the game receives the action this frame)
            if (pressFrame == frameCount && !front.ui().pressAny(node)) MEALKIT_WARN("--ui-press: no widget '%s' at frame %u", node.c_str(), pressFrame);
        game->update(ctx, dt);
        audio.setOcclusionWorld(game->started() ? game->physics() : nullptr); // every frame: never left pointing at a finished match
        audio.setMuted(win.mute || (front.settings().values().audio.muteUnfocused && !window.focused()));
        audio.update(dt);
        feedback.update(dt);
        if (game->captureDue()) renderer.requestCapture(win.capturePath.c_str());
        ++frameCount;
        const bool lastFrame = win.frames && frameCount >= win.frames;
        if (lastFrame && !win.capturePath.empty() && win.captureTick == 0) renderer.requestCapture(win.capturePath.c_str());
        if (game->finished() && exitDeadline == 0.0) {
            exitCode = reportFinish(*game, session, opt, win);
            exitDeadline = now + 1.0; // exit after giving the other side time to receive the last tick
        }

        if (!window.minimized()) {
            MEALKIT_ZONE("Render");
            render::FrameParams frame;
            drawList.clear();
            game->buildFrame(ctx, now, frame, drawList);
            feedback.addParticles(drawList); // effects from the feedback table (drawn after opaque 3D)
            front.afterBuildFrame(frame);
            frame.time = static_cast<f32>(std::fmod(now, 4096.0)); // wind sway (wraps within f32 precision)
#if MEALKIT_DEBUG_UI
            if (debugUiShown) {
                debugUi.newFrame();
                ImGui::SetNextWindowPos(ImVec2(10.0f, static_cast<f32>(window.height()) - 10.0f), ImGuiCond_Always,
                                        ImVec2(0.0f, 1.0f)); // bottom left (the game window is top left)
                if (ImGui::Begin("MealKit", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::TextUnformatted(perfText.c_str());
                    ImGui::TextDisabled("F1: hide | V: vsync | F9: bug report | F12: screenshot");
                }
                ImGui::End();
                game->debugUi(ctx);
            }
            if (bugStep == BugStep::Note && debugUiShown) {
                ImGui::SetNextWindowPos(ImVec2(static_cast<f32>(window.width()) * 0.5f, static_cast<f32>(window.height()) * 0.5f),
                                        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                if (ImGui::Begin("Bug report", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::TextUnformatted("What happened? (Enter: save, Esc: cancel)");
                    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                    if (ImGui::InputText("##note", bugNote, sizeof(bugNote), ImGuiInputTextFlags_EnterReturnsTrue))
                        bugStep = BugStep::Shot; // the next frame is captured without this window
                    else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) bugStep = BugStep::None;
                }
                ImGui::End();
            }
            // Game code's debug::line/box/text (text only when the debug UI is on)
            debug::flushDebugDraw(drawList, models.boxMesh(), frame, now, debugUiShown);
#endif
            const bool active = renderer.beginFrame(frame);
            if (active) drawList.submit(renderer);
            if (active) front.renderUi(renderer); // after 3D, before debug UI
#if MEALKIT_DEBUG_UI
            if (debugUiShown) debugUi.render(renderer, active);
#endif
            if (active) renderer.endFrame();
        }

        limiter.wait();
        MEALKIT_FRAME_MARK();
        if (lastFrame) break;

        // ---- Status in the window title (4Hz) ----
        const bool perfSample = perf.update(dt, {&renderer, &session, &audio, game});
        if (benchmark && bench.frame(dt, game->started(), perfSample ? &perf.sample() : nullptr)) {
            exitCode = writeBenchmark(bench, project.settings, spec, renderer, opt, win.benchmarkPath) ? 0 : 1;
            break;
        }
#if MEALKIT_LIVE
        if (perfSample && liveServer.listening("stats")) liveServer.publish("stats", perf.json());
        liveServer.update();
#endif
        if (perfSample) {
            // Text shown to players comes from string tables (assets/strings); performance numbers are for developers, so English as is
            std::string status = lobbyStatus(matches, front.strings());
            const bool running = status.empty();
            if (running) status = perf.line();
            const std::string version = spec.build.gameVersion[0] ? std::string(" v") + spec.build.gameVersion : std::string();
            const std::string title = run.title.empty() ? front.strings().get("game.title") + version : run.title;
            window.setTitle((title + " | " + status).c_str());
#if MEALKIT_DEBUG_UI
            perfText = running ? perf.lines() : status;
#endif
            if (automated && running && (perfLogCounter++ % 8) == 7) MEALKIT_INFO("perf: %s", status.c_str());
        }
    }

    if (game->started()) game->stop(ctx);
    if (keepingReplays) keepReplay(userFolderPath, project.settings.keepReplays); // the window was closed mid-match
    module.destroy(game);
    matches.shutdown();
    audio.shutdown();
    online.shutdown();
#if MEALKIT_DEBUG_UI
    debugUi.shutdown();
#endif
    renderer.shutdown();
    return exitCode;
}

} // namespace mealkit::game
