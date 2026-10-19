// 출시 exe의 main (mealkit/game/GameMain.h). 게임의 app/main.cpp가 MEALKIT_GAME_MAIN으로 부른다.
//
//   <게임>.exe                         혼자 플레이
//   <게임>.exe --host [port]           방 만들기 (로비에서 Enter로 시작)
//   <게임>.exe --join 127.0.0.1[:port] 참가
//   <게임>.exe --scene my.scene        다른 씬으로 플레이 (모든 플레이어가 같은 파일이어야 한다)
//   <게임>.exe --headless ...          창 없이 시뮬레이션만 (게임의 app/Headless.cpp, <게임>_headless.exe와 같다)
//
// 시작 씬은 mealkit.project, 기본 포트·인원은 게임 헤드리스 클래스의 defaults()(보통 mealkit.project의 game.* 항목)에서 읽는다.
// 엔진 키: V 수직동기, F12 스크린샷, F1 디버그 UI(개발 빌드), Esc 종료

#include "mealkit/game/GameMain.h"

#include "mealkit/asset/ModelCache.h"
#include "mealkit/audio/Audio.h"
#include "mealkit/core/Clock.h"
#include "mealkit/core/Log.h"
#include "mealkit/core/Profile.h"
#include "mealkit/core/Version.h"
#include "mealkit/feedback/Feedback.h"
#include "mealkit/game/FrameStats.h"
#include "mealkit/game/Frontend.h"
#include "mealkit/game/GameData.h"
#include "mealkit/game/ProjectSettings.h"
#include "mealkit/net/Lockstep.h"
#include "mealkit/net/Socket.h"
#include "mealkit/online/Online.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/System.h"
#include "mealkit/platform/Window.h"
#include "mealkit/render/DrawList.h"
#include "mealkit/render/Renderer.h"
#include "mealkit/scene/Scene.h"

#if MEALKIT_DEBUG_UI
#include "mealkit/debug/ImGuiLayer.h"

#include <imgui.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

namespace mealkit::game {
namespace {

struct Options {
    enum class Mode { Offline, Host, Join, SteamHost, SteamJoin } mode = Mode::Offline;
    u64 steamLobby = 0;
    u16 port = 0; // 0 = 게임 defaults() (보통 mealkit.project의 game.defaultPort)
    std::string joinAddress;
    std::string scenePath;
    std::string tuningPath;
    u32 maxPlayers = 0; // 0 = 게임 defaults()
    u32 matchData = 0;  // 게임이 정하는 매치 설정값 (호스트가 정한다. sandbox: 상자 수, 0 = 씬의 상자 생성 영역)
    u32 inputDelay = 3;
    u64 seed = 0;
    bool vsync = true;
    f64 fpsCap = 0.0;
    u32 width = 1280, height = 720;
    bool debugLayer = false;
    bool loopbackOnly = false;
    u32 physicsThreads = 0;
    // 자동 테스트용
    u32 autoStart = 0;
    u32 exitTick = 0;
    bool bot = false;
    bool shadow = false;     // 그림자 시뮬레이션으로 결정론 검사
    bool rewindTest = false; // 끝날 때 기록을 되감아 다시 돌려 보고 결과를 출력
    std::string capturePath;
    u32 captureTick = 0;
    bool systemInfo = false; // CPU·GPU 정보만 출력하고 끝 (최소 사양 실기 측정 기록용)
    bool anyBuild = false;   // 호스트가 다른 빌드의 피어도 받는다 (컴파일러 비교 시험)
    bool mute = false;       // 소리 끄기 (자동 시험에서 여러 창을 띄울 때)
    bool debugUi = false;    // 디버그 UI(F1)를 켠 채로 시작 (개발 빌드)
    bool openSettings = false; // 설정 화면을 연 채로 시작 (캡처·시험)
    bool windowGiven = false; // --size/--vsync/--fps를 줬다: 이번 실행은 플레이어 설정의 화면 항목보다 명령줄
    std::string language;    // 게임 글자 언어 (비어 있으면 mealkit.project의 language)
    std::string hashLogPath;      // 틱별 해시 로그 (헤드리스와 비교)
    std::string recordInputsPath; // 확정된 입력 기록 (<게임>_headless --replay-inputs로 재생)
};

void printUsage(const char* name) {
    std::printf("%s (MealKit %s)\n", name, MEALKIT_VERSION);
    std::printf("  --host [port]          host a match (default: the game's default port, game.defaultPort in mealkit.project)\n"
                "  --join <addr[:port]>   join a host\n"
                "  --steam                host a Steam friends-only lobby (invite with I or Shift+Tab)\n"
                "  --steam-join <lobby>   join a Steam lobby by id (invites from Steam join automatically)\n"
                "  --scene <file>         scene to play (default: scene in mealkit.project)\n"
                "  --tuning <file>        tuning values (default assets/game.tuning)\n"
                "  --players <n>          max players when hosting (default: the game's default, max 8)\n"
                "  --match-data <n>       match setting chosen by the host and sent to everyone (the game decides; alias --bodies)\n"
                "  --delay <ticks>        input delay in ticks (default 3)\n"
                "  --seed <n>             world seed\n"
                "  --vsync <0|1>          vertical sync (default 1)\n"
                "  --fps <n>              frame cap when vsync is off\n"
                "  --size <w> <h>         window size\n"
                "  --debug                enable the D3D12 debug layer\n"
                "  --local                host on 127.0.0.1 only (same-PC testing)\n"
                "  --threads <n>          physics worker threads (default: auto)\n"
                "  --auto-start <n>       host: start when n players are in the lobby\n"
                "  --exit-tick <t>        print the state hash after tick t and exit\n"
                "  --bot                  drive the local player with scripted input\n"
                "  --shadow               run a shadow simulation and compare state hashes every tick\n"
                "  --rewind-test          record inputs and snapshots, rewind and replay at the exit tick\n"
                "  --capture <png> <tick> save a screenshot after the given tick\n"
                "  --system-info          print CPU/GPU capabilities and exit (exit code 2 if below minimum spec)\n"
                "  --any-build            host: accept peers running a different build (compiler comparison tests)\n"
                "  --mute                 start with sound off (automated tests)\n"
                "  --debug-ui             start with the debug UI shown (F1, development builds)\n"
                "  --settings             start with the settings screen open\n"
                "  --lang <code>          language of the game text (assets/strings/<code>.txt, default: language in mealkit.project)\n"
                "  --hash-log <file>      write \"<tick> <hash>\" for every tick\n"
                "  --record-inputs <file> save confirmed inputs and hashes (replay: --headless --replay-inputs)\n"
                "  --headless --help      run without a window (headless options)\n");
}

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char*& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        auto hasValue = [&] { return i + 1 < argc && argv[i + 1][0] != '-'; };
        const char* v = nullptr;
        if (a == "--host") {
            o.mode = Options::Mode::Host;
            if (hasValue()) o.port = static_cast<u16>(std::atoi(argv[++i]));
        } else if (a == "--join" && next(v)) {
            o.mode = Options::Mode::Join;
            o.joinAddress = v;
        } else if (a == "--steam") o.mode = Options::Mode::SteamHost;
        else if (a == "--steam-join" && next(v)) {
            o.mode = Options::Mode::SteamJoin;
            o.steamLobby = std::strtoull(v, nullptr, 10);
        } else if (a == "+connect_lobby" && next(v)) { // Steam이 친구 초대를 수락했을 때 붙이는 인자
            o.mode = Options::Mode::SteamJoin;
            o.steamLobby = std::strtoull(v, nullptr, 10);
        } else if (a == "--scene" && next(v)) o.scenePath = v;
        else if (a == "--tuning" && next(v)) o.tuningPath = v;
        else if (a == "--players" && next(v)) o.maxPlayers = static_cast<u32>(std::atoi(v));
        else if ((a == "--bodies" || a == "--match-data") && next(v)) o.matchData = static_cast<u32>(std::atoi(v));
        else if (a == "--delay" && next(v)) o.inputDelay = static_cast<u32>(std::atoi(v));
        else if (a == "--seed" && next(v)) o.seed = std::strtoull(v, nullptr, 10);
        else if (a == "--vsync" && next(v)) o.vsync = std::atoi(v) != 0, o.windowGiven = true;
        else if (a == "--fps" && next(v)) o.fpsCap = std::atof(v), o.windowGiven = true;
        else if (a == "--size" && i + 2 < argc) {
            o.windowGiven = true;
            o.width = static_cast<u32>(std::atoi(argv[++i]));
            o.height = static_cast<u32>(std::atoi(argv[++i]));
        } else if (a == "--debug") o.debugLayer = true;
        else if (a == "--local") o.loopbackOnly = true;
        else if (a == "--threads" && next(v)) o.physicsThreads = static_cast<u32>(std::atoi(v));
        else if (a == "--auto-start" && next(v)) o.autoStart = static_cast<u32>(std::atoi(v));
        else if (a == "--exit-tick" && next(v)) o.exitTick = static_cast<u32>(std::atoi(v));
        else if (a == "--bot") o.bot = true;
        else if (a == "--shadow") o.shadow = true;
        else if (a == "--rewind-test") o.rewindTest = true;
        else if (a == "--capture" && i + 2 < argc) {
            o.capturePath = argv[++i];
            o.captureTick = static_cast<u32>(std::atoi(argv[++i]));
        } else if (a == "--system-info") o.systemInfo = true;
        else if (a == "--any-build") o.anyBuild = true;
        else if (a == "--mute") o.mute = true;
        else if (a == "--debug-ui") o.debugUi = true;
        else if (a == "--settings") o.openSettings = true;
        else if (a == "--lang" && i + 1 < argc) o.language = argv[++i];
        else if (a == "--hash-log" && next(v)) o.hashLogPath = v;
        else if (a == "--record-inputs" && next(v)) o.recordInputsPath = v;
        else if (a == "--help" || a == "-h") return false;
        else {
            std::printf("Unknown option: %s\n", a.c_str());
            return false;
        }
    }
#if defined(MEALKIT_DEBUG)
    o.debugLayer = true;
#endif
    return true;
}

// 최소 사양 실기 측정 결과를 CHANGELOG에 옮겨 적기 쉬운 모양으로 출력한다.
int printSystemInfo() {
    std::printf("CPU: %s\n", cpuName().c_str());
    render::GpuInfo gpu;
    std::string error;
    if (!render::queryGpu(gpu, &error)) {
        std::printf("GPU: none (%s)\n", error.c_str());
        return 2;
    }
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

// --exit-tick에 닿았을 때: 자동 시험이 읽는 줄(FINAL, SHADOW, REWIND)을 쓰고 종료 코드를 돌려준다
int reportFinish(GameInstance& game, const net::LockstepSession& session, const Options& opt) {
    std::printf("FINAL tick=%u hash=%016llx desync=%d\n", game.nextTick() - 1,
                static_cast<unsigned long long>(game.stateHash()), session.desynced() ? 1 : 0);
    int exitCode = session.desynced() ? 2 : 0;
    if (SimDebug* debug = game.debug(); debug && (opt.shadow || opt.rewindTest)) {
        if (opt.shadow) {
            const ShadowStatus& shadow = debug->shadow();
            std::printf("SHADOW %s tick=%u\n", shadow.desynced ? "DESYNC" : "ok", shadow.tick);
            if (shadow.desynced) exitCode = 5;
        }
        if (opt.rewindTest) { // 기록 범위 전체를 조금씩 되감아 다시 돌리고, 현재로 돌아와 해시가 같은지 본다
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

// 매치가 진행 중이 아닐 때 창 제목에 쓰는 상태 (게임 문자열 표의 title.* 키). 진행 중이면 빈 문자열
std::string lobbyStatus(const net::LockstepSession& session, const online::Service& online, bool useSteam,
                        u32 maxPlayers, const loc::Strings& strings) {
    const net::SessionState state = session.state();
    const auto num = [](u32 v) { return std::to_string(v); };
    if (useSteam && state == net::SessionState::Idle)
        return strings.get(online.lobbyState() == online::LobbyState::Creating ? "title.steamCreating"
                                                                               : "title.steamJoining");
    if (state == net::SessionState::Lobby && session.isHost())
        return strings.format(useSteam ? "title.lobbyHostSteam" : "title.lobbyHost",
                              {{"count", num(session.lobbyPlayerCount())}, {"max", num(maxPlayers)}});
    if (state == net::SessionState::Lobby || state == net::SessionState::Connecting)
        return strings.format(state == net::SessionState::Connecting ? "title.connecting" : "title.waiting",
                              {{"count", num(session.lobbyPlayerCount())}});
    if (state == net::SessionState::Ended) return strings.format("title.ended", {{"reason", session.endReason()}});
    return {};
}

} // namespace

int runGame(int argc, char** argv, const GameMainSpec& spec) {
    const GameModule& module = *spec.module;
    Options opt;
    if (!parseArgs(argc, argv, opt)) {
        printUsage(module.name);
        return 1;
    }
    if (opt.systemInfo) return printSystemInfo();
    const bool automated = opt.exitTick > 0;
    if (automated) log::setDialogs(false);

    // 프로젝트 설정: 출시본은 exe 옆, 개발 중에는 프로젝트 폴더의 mealkit.project
    ProjectFile project;
    const reflect::TypeInfo* gameProjectType = module.projectSettingsType ? module.projectSettingsType() : nullptr;
    reflect::Instance gameProject = gameProjectType ? reflect::Instance(*gameProjectType) : reflect::Instance();
    loadGameProject(project, gameProjectType, gameProject.data());
    MEALKIT_INFO("CPU: %s", cpuName().c_str()); // 다른 PC 결정론 기록용
    if (spec.defaults && gameProject.data()) {
        HeadlessOptions d; // 헤드리스와 같은 기본값 함수 (포트·인원)
        d.port = opt.port;
        d.maxPlayers = opt.maxPlayers;
        spec.defaults(d, gameProject.data());
        opt.port = d.port;
        opt.maxPlayers = d.maxPlayers;
    }

    scene::Scene scene;
    const std::string scenePath = opt.scenePath.empty() ? findAsset(startSceneAsset(project.settings)) : opt.scenePath;
    if (!scenePath.empty()) {
        std::string error;
        if (!scene::load(scene, scenePath, &error))
            MEALKIT_FATAL("Cannot load scene %s: %s", scenePath.c_str(), error.c_str());
        MEALKIT_INFO("Scene: %s (%zu entities)", scenePath.c_str(), scene.entities.size());
    } else {
        scene = scene::makeDefault();
        MEALKIT_INFO("Scene file not found - using the built-in arena");
    }

    Window window;
    if (!window.create({module.name, opt.width, opt.height})) return 1;

    render::Renderer renderer;
    if (!renderer.init({window.nativeHandle(), window.width(), window.height(), opt.vsync, opt.debugLayer}))
        MEALKIT_FATAL("Renderer initialization failed.\n%s", renderer.initError());
    asset::ModelCache models;
    models.setRenderer(&renderer);

    net::LockstepSession session;
    // 실행 파일 + 씬 + 모델 파일 + 튜닝 + 프로젝트 설정(해시 대상 항목) + 시뮬레이션 표가 모두 같아야 접속할 수 있다
    const reflect::TypeInfo* tuningType = module.tuningType ? module.tuningType() : nullptr;
    reflect::Instance tuning = tuningType ? reflect::Instance(*tuningType) : reflect::Instance();
    if (tuningType) loadTuning(*tuningType, tuning.data(), opt.tuningPath);
    session.setContentHash(contentHash(scene, models, module.objectTypes ? module.objectTypes() : nullptr, tuningType,
                                       tuning.data(), {&reflect::typeOf<ProjectSettings>(), &project.settings},
                                       {gameProjectType, gameProject.data()}, module.tables ? module.tables() : nullptr));
    session.setAcceptOtherBuilds(opt.anyBuild);
    const u64 seed = opt.seed ? opt.seed : static_cast<u64>(std::time(nullptr));
    switch (opt.mode) {
    case Options::Mode::Offline: break; // 아래에서 게임이 로드아웃을 정한 뒤 연다
    case Options::Mode::Host:
        if (!session.host(opt.port, opt.maxPlayers, opt.inputDelay, opt.loopbackOnly))
            MEALKIT_FATAL("Could not host on port %u", opt.port);
        break;
    case Options::Mode::Join: {
        net::Address address;
        if (!net::startup() || !net::resolveAddress(opt.joinAddress.c_str(), opt.port, address))
            MEALKIT_FATAL("Could not resolve '%s'", opt.joinAddress.c_str());
        net::cleanup();
        if (!session.join(address)) MEALKIT_FATAL("Could not open a UDP socket");
        break;
    }
    case Options::Mode::SteamHost:
    case Options::Mode::SteamJoin: break; // 로비가 준비되면 아래 루프에서 세션을 연다
    }

    // 온라인 로비(Steam): 로비를 만들거나 들어간 뒤, 방장 주소로 락스텝 세션을 연다 (플랫폼 릴레이 경유)
    const bool useSteam = opt.mode == Options::Mode::SteamHost || opt.mode == Options::Mode::SteamJoin;
    online::Service online;
    if (useSteam) {
        if (online::Service::backend() != online::Backend::Steam || !online.init())
            MEALKIT_FATAL(
                "Steam is not available. Run the Steam client, and build with the Steamworks SDK (see " MEALKIT_DOCS_URL
                "manual/multiplayer.html#steamworks).");
        if (opt.mode == Options::Mode::SteamHost) online.createLobby(opt.maxPlayers);
        else online.joinLobby(opt.steamLobby);
    }

    GameInstance* game = module.create();
    PlayOptions gameOptions;
    gameOptions.bot = opt.bot;
    gameOptions.exitTick = opt.exitTick;
    gameOptions.capture = !opt.capturePath.empty();
    gameOptions.captureTick = opt.captureTick;
    gameOptions.physicsThreads = opt.physicsThreads;
    gameOptions.tuningPath = opt.tuningPath;
    gameOptions.shadow = opt.shadow;
    gameOptions.record = opt.rewindTest;
    gameOptions.hashLogPath = opt.hashLogPath;
    gameOptions.recordInputsPath = opt.recordInputsPath;
    const std::string userFolderPath = automated ? std::string() : userFolder(project.settings);
    if (opt.mode == Options::Mode::Offline) {
        game->lobby(session, gameOptions, userFolderPath); // 매치 시작 데이터 (로드아웃)
        session.startOffline(seed, opt.matchData);
    }

    FrameLimiter limiter;
    limiter.setTargetFps(opt.vsync ? 0.0 : opt.fpsCap);

    // 소리: 기본 출력 장치 (없으면 소리 없이 돈다). 창이 포커스를 잃으면 음소거 (플레이어 설정에서 끌 수 있다)
    audio::Engine audio;
    audio.init({project.settings.audioVoices, opt.mute});
    feedback::Player feedback(audio); // assets/feedback.table: 틱 이벤트 → 소리·흔들림·번쩍임·화면 정지

    // 화면 쪽 서비스: 입력 액션(assets/input.bindings + 플레이어가 바꾼 키), 문자열 표, 게임 UI, 플레이어 설정, 화면용 튜닝.
    // 자동 실행(--exit-tick)은 설정 기본값으로 돌고 저장하지 않는다 (이 PC의 설정이 시험 결과를 바꾸지 않게, 창 크기는 명령줄)
    Frontend front;
    {
        FrontendConfig c;
        c.renderer = &renderer;
        c.audio = &audio;
        c.feedback = &feedback;
        const bool display = !automated && !opt.windowGiven;
        c.displayWindow = display ? &window : nullptr;
        c.limiter = &limiter;
        c.language = opt.language;
        c.projectLanguage = project.settings.language;
        c.bindingsUserFile = userFolder(project.settings) + "/" + input::kBindingsTable;
        c.settingsFile = automated ? std::string() : userFolder(project.settings) + "/" + settings::kSettingsFile;
        c.viewTuningType = module.viewTuningType ? module.viewTuningType() : nullptr; // 파일이 바뀌면 다시 읽는다
        front.init(c);
        if (opt.openSettings) front.settings().open();
    }

    GameContext ctx = front.context(window, renderer, models, session);
    ctx.userFolder = userFolderPath; // 세이브 (자동 실행은 저장하지 않는다)
#if MEALKIT_DEBUG_UI
    // 디버그 UI (F1): 개발 빌드만 (출시 빌드 build.bat Export에는 ImGui가 없다). 처음 켤 때 만든다 (글꼴 읽기)
    debug::ImGuiLayer debugUi;
    bool debugUiReady = false, debugUiShown = false;
    std::string perfText;
#endif
    render::DrawList drawList;
    f64 lastTime = nowSeconds();
    FrameStats perf; // 성능 숫자 (창 제목·F1·perf 로그, mealkit/game/FrameStats.h)
    u32 perfLogCounter = 0;
    int exitCode = 0;
    f64 exitDeadline = 0.0; // --exit-tick 도달 후 종료 시각

    while (window.pumpMessages()) {
        const f64 now = nowSeconds();
        const f64 dt = std::min(now - lastTime, 0.25);
        lastTime = now;
        Input& input = window.input();
        if (exitDeadline > 0.0 && now >= exitDeadline) break;

        if (input.keyPressed(key::Escape) && !front.settings().isOpen()) break; // 설정 화면이 열려 있으면 Esc는 화면을 닫는다
        if (input.keyPressed('V')) renderer.setVsync(!renderer.vsync());
        if (input.keyPressed(key::F12)) renderer.requestCapture("screenshot.png");
        if (window.consumeResize()) renderer.resize(window.width(), window.height());

        if (useSteam) {
            online.update();
            if (online.lobbyState() == online::LobbyState::Failed) MEALKIT_FATAL("Online lobby failed (see log)");
            if (session.state() == net::SessionState::Idle && online.lobbyState() == online::LobbyState::InLobby) {
                if (online.isLobbyOwner()) {
                    session.hostWith(online.createTransport(), opt.maxPlayers, opt.inputDelay);
                    online.inviteFriends();
                } else {
                    session.joinWith(online.createTransport(), online.lobbyOwnerAddress());
                }
            }
            if (const u64 invited = online.takeJoinRequest())
                MEALKIT_INFO("Invite to lobby %llu received - restart the game from the invite to join",
                             static_cast<unsigned long long>(invited));
            if (input.keyPressed('I')) online.inviteFriends();
        }

        session.update(now);
        if (!game->started() && session.state() != net::SessionState::Running && session.state() != net::SessionState::Ended)
            game->lobby(session, gameOptions, ctx.userFolder); // 로비: 로드아웃을 정한다 (호스트가 시작할 때 확정)

        if (session.isHost() && session.state() == net::SessionState::Lobby) {
            if (input.keyPressed(key::Enter) || (opt.autoStart && session.lobbyPlayerCount() >= opt.autoStart))
                session.startMatch(seed, opt.matchData);
        }
        if (session.state() == net::SessionState::Ended && automated && exitDeadline == 0.0) {
            MEALKIT_ERROR("Session ended before the exit tick: %s", session.endReason());
            exitCode = 3;
            break;
        }

        if (!game->started() && session.state() == net::SessionState::Running) {
            game->start(ctx, scene, gameOptions);
            MEALKIT_INFO("World ready: %u player(s), you are player %u, %u moving bodies", session.playerCount(),
                         session.localPlayer(), game->activeBodies());
        }

        ctx.viewport = {0.0f, 0.0f, static_cast<f32>(window.width()), static_cast<f32>(window.height())};
        ctx.keyboardAllowed = ctx.mouseAllowed = window.focused();
#if MEALKIT_DEBUG_UI
        if (input.keyPressed(key::F1) || (opt.debugUi && !debugUiReady)) {
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
        game->update(ctx, dt);
        audio.setMuted(opt.mute || (front.settings().values().audio.muteUnfocused && !window.focused()));
        audio.update(dt);
        feedback.update(dt);
        if (game->captureDue()) renderer.requestCapture(opt.capturePath.c_str());
        if (game->finished() && exitDeadline == 0.0) {
            exitCode = reportFinish(*game, session, opt);
            exitDeadline = now + 1.0; // 상대가 마지막 틱을 받을 시간을 준 뒤 종료
        }

        if (!window.minimized()) {
            MEALKIT_ZONE("Render");
            render::FrameParams frame;
            drawList.clear();
            game->buildFrame(ctx, now, frame, drawList);
            front.afterBuildFrame(frame);
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

        // ---- 창 제목에 상태 표시 (4Hz) ----
        if (perf.update(dt, {&renderer, &session, &audio, game})) {
            // 플레이어에게 보이는 글자는 문자열 표(assets/strings), 성능 숫자는 개발용이라 영어 그대로
            std::string status = lobbyStatus(session, online, useSteam, opt.maxPlayers, front.strings());
            const bool running = status.empty();
            if (running) status = perf.line();
            window.setTitle((front.strings().get("game.title") + " | " + status).c_str());
#if MEALKIT_DEBUG_UI
            perfText = running ? perf.lines() : status;
#endif
            if (automated && running && (perfLogCounter++ % 8) == 7) MEALKIT_INFO("perf: %s", status.c_str());
        }
    }

    module.destroy(game);
    session.shutdown();
    audio.shutdown();
    online.shutdown();
#if MEALKIT_DEBUG_UI
    debugUi.shutdown();
#endif
    renderer.shutdown();
    return exitCode;
}

} // namespace mealkit::game
