// 출시 exe의 main (mealkit/game/GameMain.h). 게임의 app/main.cpp가 MEALKIT_GAME_MAIN으로 부른다.
//
//   <게임>.exe                         메뉴가 있는 게임은 타이틀부터 (GameInstance::menus), 없으면 혼자 플레이
//   <게임>.exe --host [port]           방 만들기 (로비부터. 메뉴가 없는 게임은 Enter로 시작)
//   <게임>.exe --join 127.0.0.1[:port] 참가
//   <게임>.exe --scene my.scene        다른 씬으로 플레이 (모든 플레이어가 같은 파일이어야 한다)
//   <게임>.exe --headless ...          창 없이 시뮬레이션만 (게임의 app/Headless.cpp, <게임>_headless.exe와 같다)
//
// 시작 씬은 mealkit.project, 기본 포트·인원은 게임 헤드리스 클래스의 defaults()(보통 mealkit.project의 game.* 항목)에서 읽는다.
// 엔진 키: V 수직동기, F12 스크린샷, F1 디버그 UI(개발 빌드), Esc 종료(메뉴가 없는 게임만), I 친구 초대(온라인 로비)

#include "mealkit/game/GameMain.h"

#include "mealkit/asset/ModelCache.h"
#include "mealkit/audio/Audio.h"
#include "mealkit/core/Clock.h"
#include "mealkit/core/Log.h"
#include "mealkit/core/Profile.h"
#include "mealkit/core/Version.h"
#include "mealkit/feedback/Feedback.h"
#include "mealkit/game/Benchmark.h"
#include "mealkit/game/FrameStats.h"
#include "mealkit/game/Frontend.h"
#include "mealkit/game/GameData.h"
#include "mealkit/game/GameOptions.h"
#include "mealkit/game/MatchService.h"
#include "mealkit/game/ProjectSettings.h"
#include "mealkit/game/RenderStyle.h"
#include "mealkit/live/Server.h"
#include "mealkit/net/Lockstep.h"
#include "mealkit/net/Socket.h"
#include "mealkit/online/Online.h"
#include "mealkit/platform/Paths.h"
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

// 최소 사양 실기 측정 결과를 CHANGELOG에 옮겨 적기 쉬운 모양으로 출력한다.
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

// --benchmark가 끝났을 때: 보고서 파일과 자동 시험이 읽는 BENCHMARK 줄
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

// --exit-tick에 닿았을 때: 자동 시험이 읽는 줄(FINAL, SHADOW, REWIND)을 쓰고 종료 코드를 돌려준다
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
        if (win.rewindTest) { // 기록 범위 전체를 조금씩 되감아 다시 돌리고, 현재로 돌아와 해시가 같은지 본다
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

// 매치가 진행 중이 아닐 때 창 제목에 쓰는 상태 (게임 문자열 표의 title.* 키). 진행 중이거나 메뉴면 빈 문자열
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

int runGame(int argc, char** argv, const GameMainSpec& spec) {
    MatchOptions opt;
    WindowOptions win;
    if (!parseGameArgs(argc, argv, opt, &win)) {
        printGameUsage(spec.module->name, false);
        return 1;
    }
    if (win.systemInfo) return printSystemInfo();
    if (win.benchmarkSeconds > 0.0) { // 벤치마크: 매번 같은 매치 (고정 시드 + 봇), 화면 동기 없이, 소리 없이
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

    MEALKIT_INFO("CPU: %s", cpuName().c_str()); // 다른 PC 결정론 기록용
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

    // 매치 데이터 (헤드리스·에디터 플레이와 같은 함수): 프로젝트 설정(출시본은 exe 옆, 개발 중에는 프로젝트 폴더의 mealkit.project),
    // 씬, 튜닝, 콘텐츠 해시. 실행 파일 + 이 해시가 같아야 접속할 수 있다
    MatchData data;
    MatchDataOptions dataOptions;
    dataOptions.scenePath = opt.scenePath;
    dataOptions.tuningPath = opt.tuningPath;
    dataOptions.project = run.project;
    if (!loadMatchData(gameTypes(module), dataOptions, models, data))
        MEALKIT_FATAL("Cannot load the match data (see log)");
    const ProjectFile& project = data.project;
    RenderStyleState renderStyle;
    applyRenderStyle(project.settings, renderer, renderStyle); // 조명 모델·툰 램프 (파이프라인은 renderer.init이 이미 만들었다)
    if (spec.defaults && data.gameProject.data()) spec.defaults(opt, data.gameProject.data()); // 헤드리스와 같은 기본값 (포트·인원)
    opt.inputDelay = msToTicks(opt.inputDelayMs); // 틱레이트(프로젝트 설정)를 안 뒤
    const scene::Scene& scene = data.scene;

#if MEALKIT_LIVE
    // 라이브 연결 (도구 사이 약속, mealkit/live/Server.h): 개발 빌드만. 접속 정보는 프로젝트의 build/live/game-<pid>.json
    live::Server liveServer;
    {
        live::ServerConfig c;
        c.kind = "game";
        const std::string projectFile = findAsset(kProjectFileName);
        c.projectDir = projectFile.empty() ? std::string() : parentDirectory(projectFile);
        c.projectName = project.settings.name.empty() ? module.name : project.settings.name;
        liveServer.start(c);
        liveServer.addTopic("stats"); // game::FrameStats::json, 보고 주기마다
    }
#endif

    // 온라인 서비스(Steam): 빌드에 들어 있으면 시작할 때 로그인한다 (메뉴의 [Steam 방 만들기], 친구 초대 수락)
    const bool steamAsked = opt.mode == MatchOptions::Mode::SteamHost || opt.mode == MatchOptions::Mode::SteamJoin;
    online::Service online;
    if (online::Service::backend() == online::Backend::Steam && !automated && run.onlineService) online.init();
    if (steamAsked && !online.available())
        MEALKIT_FATAL("Steam is not available. Run the Steam client, and build with the Steamworks SDK (see " MEALKIT_DOCS_URL
                      "manual/multiplayer.html#steamworks).");

    // 세션은 MatchService 안에 있다. 게임의 화면 흐름(메뉴)도 같은 함수로 방을 열고 들어간다
    MatchService matches;
    {
        MatchServiceConfig c;
        c.contentHash = data.contentHash;
        c.acceptOtherBuilds = opt.anyBuild;
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
        c.autoDelay.enabled = c.autoDelay.enabled && !opt.delayGiven; // --delay: 고정
        c.online = online.available() ? &online : nullptr;
        matches.init(c);
    }
    net::LockstepSession& session = matches.session();

    GameInstance* game = module.create();
    const bool menus = game->menus();
    PlayOptions gameOptions;
    gameOptions.bot = win.bot;
    gameOptions.exitTick = opt.exitTick;
    gameOptions.capture = !win.capturePath.empty() && (win.captureTick > 0 || win.frames == 0); // 틱이 없으면 --frames의 마지막 프레임
    gameOptions.captureTick = win.captureTick;
    gameOptions.physicsThreads = opt.physicsThreads;
    gameOptions.tuningPath = opt.tuningPath;
    gameOptions.shadow = opt.shadow;
    gameOptions.record = win.rewindTest;
    gameOptions.hashLogPath = opt.hashLogPath;
    gameOptions.recordInputsPath = opt.recordInputsPath;
    // 사용자 폴더(세이브·설정·바꾼 키): 자동 실행(--exit-tick)과 참가 창은 쓰지 않는다
    const bool playerFiles = !automated && run.playerFiles;
    const std::string userFolderPath = playerFiles ? userFolder(project.settings) : std::string();

    // 명령줄로 정한 시작: 방 만들기·참가는 로비부터. 혼자 플레이는 메뉴가 있는 게임이면 메뉴부터(자동 실행·봇은 바로 매치)
    switch (opt.mode) {
    case MatchOptions::Mode::Offline:
        if (!menus || opt.exitTick > 0 || win.bot) { // 자동 시험(--exit-tick)·봇은 메뉴 없이 바로 매치
            matches.playOffline();
            game->lobby(session, gameOptions, userFolderPath); // 매치 시작 데이터 (로드아웃)
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

    // 소리: 기본 출력 장치 (없으면 소리 없이 돈다). 창이 포커스를 잃으면 음소거 (플레이어 설정에서 끌 수 있다)
    audio::Engine audio;
    audio.init({project.settings.audioVoices, win.mute});
#if MEALKIT_LIVE
    audio::addLiveService(liveServer, [&audio] { return &audio; }); // MealKit Audio: 게임에서 재생, 다시 읽기, 상태
#endif
    feedback::Player feedback(audio); // assets/feedback.table: 틱 이벤트 → 소리·흔들림·번쩍임·화면 정지

    // 화면 쪽 서비스: 입력 액션(assets/input.bindings + 플레이어가 바꾼 키), 문자열 표, 게임 UI, 플레이어 설정, 화면용 튜닝.
    // 자동 실행(--exit-tick)은 설정 기본값으로 돌고 저장하지 않는다 (이 PC의 설정이 시험 결과를 바꾸지 않게, 창 크기는 명령줄)
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
        c.viewTuningType = module.viewTuningType ? module.viewTuningType() : nullptr; // 파일이 바뀌면 다시 읽는다
        c.viewTuningPath = run.viewTuningPath;
        front.init(c);
        if (win.openSettings) front.settings().open();
    }
    // 동적 해상도: 자동 실행(시험·벤치마크)은 GPU 속도에 그림이 달라지지 않게 끈다. --render-scale은 배율 고정
    if (automated || win.renderScale > 0.0f) {
        const render::ResolutionSettings& r = renderer.resolution();
        renderer.setDynamicResolution(false, r.minScale, r.budgetMs, r.sharpness);
        if (win.renderScale > 0.0f) renderer.setRenderScale(win.renderScale);
    }

    GameContext ctx = front.context(window, renderer, models, matches);
    ctx.userFolder = userFolderPath; // 세이브 (자동 실행은 저장하지 않는다)
    ctx.scene = &scene;
#if MEALKIT_DEBUG_UI
    // 디버그 UI (F1): 개발 빌드만 (출시 빌드 build.bat Export에는 ImGui가 없다). 처음 켤 때 만든다 (글꼴 읽기)
    debug::ImGuiLayer debugUi;
    bool debugUiReady = false, debugUiShown = false;
    std::string perfText;
#endif
    render::DrawList drawList;
    f64 lastTime = nowSeconds();
    FrameStats perf; // 성능 숫자 (창 제목·F1·perf 로그, mealkit/game/FrameStats.h)
    Benchmark bench({win.benchmarkSeconds}); // --benchmark (perf의 묶음을 쌓는다)
    u32 perfLogCounter = 0;
    int exitCode = 0;
    f64 exitDeadline = 0.0;  // --exit-tick 도달 후 종료 시각
    bool loadingShown = false; // 세션이 Running이 된 뒤 로딩 화면을 한 번 그렸다 (그다음 프레임에 start)
    u32 frameCount = 0;        // --frames

    while (window.pumpMessages()) {
        const f64 now = nowSeconds();
        const f64 dt = std::min(now - lastTime, 0.25);
        lastTime = now;
        Input& input = window.input();
        if (exitDeadline > 0.0 && now >= exitDeadline) break;

        // Esc: 메뉴가 있는 게임은 게임이 쓴다 (일시정지·뒤로). 없는 게임은 끝낸다 (설정 화면이 열려 있으면 화면을 닫는다)
        if (!menus && input.keyPressed(key::Escape) && !front.settings().isOpen()) break;
        if (input.keyPressed('V') && !front.ui().editingText()) renderer.setVsync(!renderer.vsync());
        if (input.keyPressed(key::F12)) renderer.requestCapture("screenshot.png");
        if (input.keyPressed(key::F11)) rhi::captureNextGpuFrame(); // --gpu-capture (도구가 없으면 아무것도 안 한다)
        if (win.gpuCaptureAt && frameCount == win.gpuCaptureAt) rhi::captureNextGpuFrame(false);
        if (window.consumeResize()) renderer.resize(window.width(), window.height());
        if (matches.usesOnline() && input.keyPressed('I') && !front.ui().editingText()) matches.inviteFriends();

        matches.update(now);
        const net::SessionState state = session.state();
        if (!game->started() && state != net::SessionState::Running && state != net::SessionState::Ended)
            game->lobby(session, gameOptions, ctx.userFolder); // 로비: 로드아웃을 정한다 (호스트가 시작할 때 확정)

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
            if (matches.phase() == MatchPhase::Menu) break; // 게임이 [나가기]를 눌렀다
        }

        // 매치 시작·끝: 시작은 로딩 화면을 한 번 그린 다음 프레임 (게임 화면 흐름이 Running이면 로딩 화면을 연다)
        if (!game->started() && session.state() == net::SessionState::Running) {
            if (loadingShown) {
                game->start(ctx, scene, gameOptions);
                MEALKIT_INFO("World ready: %u player(s), you are player %u, %u moving bodies", session.playerCount(),
                             session.localPlayer(), game->activeBodies());
            }
            loadingShown = !loadingShown;
        } else if (game->started() && session.state() != net::SessionState::Running) {
            game->stop(ctx); // 로비로 돌아감, 나감, 끊김
            loadingShown = false;
        }

        ctx.viewport = {0.0f, 0.0f, static_cast<f32>(window.width()), static_cast<f32>(window.height())};
        ctx.keyboardAllowed = ctx.mouseAllowed = window.focused();
#if MEALKIT_DEBUG_UI
        if (input.keyPressed(key::F1) || (win.debugUi && !debugUiReady)) {
            if (!debugUiReady) debugUiReady = debugUi.init(window, renderer, {});
            debugUiShown = debugUiReady && !debugUiShown;
        }
        if (debugUiShown) { // 디버그 창 위에서는 게임이 키·마우스를 받지 않는다
            const ImGuiIO& io = ImGui::GetIO();
            if (io.WantCaptureKeyboard) ctx.keyboardAllowed = false;
            if (io.WantCaptureMouse) ctx.mouseAllowed = false;
        }
#endif
        front.update(now, dt, input, window.focused(), ctx);
        for (const auto& [pressFrame, node] : win.uiPresses) // 화면 흐름 시험: 이 프레임에 위젯을 누른다 (게임이 이번 프레임에 액션을 받는다)
            if (pressFrame == frameCount && !front.ui().pressAny(node)) MEALKIT_WARN("--ui-press: no widget '%s' at frame %u", node.c_str(), pressFrame);
        game->update(ctx, dt);
        audio.setMuted(win.mute || (front.settings().values().audio.muteUnfocused && !window.focused()));
        audio.update(dt);
        feedback.update(dt);
        if (game->captureDue()) renderer.requestCapture(win.capturePath.c_str());
        ++frameCount;
        const bool lastFrame = win.frames && frameCount >= win.frames;
        if (lastFrame && !win.capturePath.empty() && win.captureTick == 0) renderer.requestCapture(win.capturePath.c_str());
        if (game->finished() && exitDeadline == 0.0) {
            exitCode = reportFinish(*game, session, opt, win);
            exitDeadline = now + 1.0; // 상대가 마지막 틱을 받을 시간을 준 뒤 종료
        }

        if (!window.minimized()) {
            MEALKIT_ZONE("Render");
            render::FrameParams frame;
            drawList.clear();
            game->buildFrame(ctx, now, frame, drawList);
            feedback.addParticles(drawList); // 연출 표의 effect (불투명 3D 다음에 그린다)
            front.afterBuildFrame(frame);
            frame.time = static_cast<f32>(std::fmod(now, 4096.0)); // 바람 흔들림 (f32 정밀도 안에서 돈다)
#if MEALKIT_DEBUG_UI
            if (debugUiShown) {
                debugUi.newFrame();
                ImGui::SetNextWindowPos(ImVec2(10.0f, static_cast<f32>(window.height()) - 10.0f), ImGuiCond_Always,
                                        ImVec2(0.0f, 1.0f)); // 왼쪽 아래 (게임 창은 왼쪽 위)
                if (ImGui::Begin("MealKit", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::TextUnformatted(perfText.c_str());
                    ImGui::TextDisabled("F1: hide | V: vsync | F12: screenshot");
                }
                ImGui::End();
                game->debugUi(ctx);
            }
#endif
            const bool active = renderer.beginFrame(frame);
            if (active) drawList.submit(renderer);
            if (active) front.renderUi(renderer); // 3D 다음, 디버그 UI 전
#if MEALKIT_DEBUG_UI
            if (debugUiShown) debugUi.render(renderer, active);
#endif
            if (active) renderer.endFrame();
        }

        limiter.wait();
        MEALKIT_FRAME_MARK();
        if (lastFrame) break;

        // ---- 창 제목에 상태 표시 (4Hz) ----
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
            // 플레이어에게 보이는 글자는 문자열 표(assets/strings), 성능 숫자는 개발용이라 영어 그대로
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
