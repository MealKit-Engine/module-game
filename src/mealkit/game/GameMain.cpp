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
#include "mealkit/game/GameOptions.h"
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
    MatchOptions match;
    WindowOptions win;
    if (!parseGameArgs(argc, argv, match, &win)) {
        printGameUsage(module.name, false);
        return 1;
    }
    if (win.systemInfo) return printSystemInfo();
    const bool automated = match.exitTick > 0;
    if (automated) log::setDialogs(false);

    MEALKIT_INFO("CPU: %s", cpuName().c_str()); // 다른 PC 결정론 기록용
    Window window;
    if (!window.create({module.name, win.width, win.height})) return 1;

    render::Renderer renderer;
    if (!renderer.init({window.nativeHandle(), window.width(), window.height(), win.vsync, win.debugLayer}))
        MEALKIT_FATAL("Renderer initialization failed.\n%s", renderer.initError());
    asset::ModelCache models;
    models.setRenderer(&renderer);

    // 매치 데이터 (헤드리스·에디터 플레이와 같은 함수): 프로젝트 설정(출시본은 exe 옆, 개발 중에는 프로젝트 폴더의 mealkit.project),
    // 씬, 튜닝, 콘텐츠 해시. 실행 파일 + 이 해시가 같아야 접속할 수 있다
    MatchData data;
    if (!loadMatchData(gameTypes(module), {match.scenePath, match.tuningPath}, models, data))
        MEALKIT_FATAL("Cannot load the match data (see log)");
    const ProjectFile& project = data.project;
    if (spec.defaults && data.gameProject.data()) {
        spec.defaults(match, data.gameProject.data()); // 헤드리스와 같은 기본값 함수 (포트·인원)
    }
    const scene::Scene& scene = data.scene;

    net::LockstepSession session;
    session.setContentHash(data.contentHash);
    session.setAcceptOtherBuilds(match.anyBuild);
    const u64 seed = match.seed ? match.seed : static_cast<u64>(std::time(nullptr));
    switch (match.mode) {
    case MatchOptions::Mode::Offline: break; // 아래에서 게임이 로드아웃을 정한 뒤 연다
    case MatchOptions::Mode::Host:
        if (!session.host(match.port, match.maxPlayers, match.inputDelay, match.loopbackOnly))
            MEALKIT_FATAL("Could not host on port %u", match.port);
        break;
    case MatchOptions::Mode::Join: {
        net::Address address;
        if (!net::startup() || !net::resolveAddress(match.joinAddress.c_str(), match.port, address))
            MEALKIT_FATAL("Could not resolve '%s'", match.joinAddress.c_str());
        net::cleanup();
        if (!session.join(address)) MEALKIT_FATAL("Could not open a UDP socket");
        break;
    }
    case MatchOptions::Mode::SteamHost:
    case MatchOptions::Mode::SteamJoin: break; // 로비가 준비되면 아래 루프에서 세션을 연다
    }

    // 온라인 로비(Steam): 로비를 만들거나 들어간 뒤, 방장 주소로 락스텝 세션을 연다 (플랫폼 릴레이 경유)
    const bool useSteam = match.mode == MatchOptions::Mode::SteamHost || match.mode == MatchOptions::Mode::SteamJoin;
    online::Service online;
    if (useSteam) {
        if (online::Service::backend() != online::Backend::Steam || !online.init())
            MEALKIT_FATAL(
                "Steam is not available. Run the Steam client, and build with the Steamworks SDK (see " MEALKIT_DOCS_URL
                "manual/multiplayer.html#steamworks).");
        if (match.mode == MatchOptions::Mode::SteamHost) online.createLobby(match.maxPlayers);
        else online.joinLobby(match.steamLobby);
    }

    GameInstance* game = module.create();
    PlayOptions gameOptions;
    gameOptions.bot = win.bot;
    gameOptions.exitTick = match.exitTick;
    gameOptions.capture = !win.capturePath.empty();
    gameOptions.captureTick = win.captureTick;
    gameOptions.physicsThreads = match.physicsThreads;
    gameOptions.tuningPath = match.tuningPath;
    gameOptions.shadow = match.shadow;
    gameOptions.record = win.rewindTest;
    gameOptions.hashLogPath = match.hashLogPath;
    gameOptions.recordInputsPath = match.recordInputsPath;
    const std::string userFolderPath = automated ? std::string() : userFolder(project.settings);
    if (match.mode == MatchOptions::Mode::Offline) {
        game->lobby(session, gameOptions, userFolderPath); // 매치 시작 데이터 (로드아웃)
        session.startOffline(seed, match.matchData);
    }

    FrameLimiter limiter;
    limiter.setTargetFps(win.vsync ? 0.0 : win.fpsCap);

    // 소리: 기본 출력 장치 (없으면 소리 없이 돈다). 창이 포커스를 잃으면 음소거 (플레이어 설정에서 끌 수 있다)
    audio::Engine audio;
    audio.init({project.settings.audioVoices, win.mute});
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
        c.bindingsUserFile = userFolder(project.settings) + "/" + input::kBindingsTable;
        c.settingsFile = automated ? std::string() : userFolder(project.settings) + "/" + settings::kSettingsFile;
        c.viewTuningType = module.viewTuningType ? module.viewTuningType() : nullptr; // 파일이 바뀌면 다시 읽는다
        front.init(c);
        if (win.openSettings) front.settings().open();
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
                    session.hostWith(online.createTransport(), match.maxPlayers, match.inputDelay);
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
            if (input.keyPressed(key::Enter) || (match.autoStart && session.lobbyPlayerCount() >= match.autoStart))
                session.startMatch(seed, match.matchData);
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
        game->update(ctx, dt);
        audio.setMuted(win.mute || (front.settings().values().audio.muteUnfocused && !window.focused()));
        audio.update(dt);
        feedback.update(dt);
        if (game->captureDue()) renderer.requestCapture(win.capturePath.c_str());
        if (game->finished() && exitDeadline == 0.0) {
            exitCode = reportFinish(*game, session, match, win);
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
            std::string status = lobbyStatus(session, online, useSteam, match.maxPlayers, front.strings());
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
