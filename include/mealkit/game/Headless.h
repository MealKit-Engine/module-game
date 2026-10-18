#pragma once

#include "mealkit/asset/ModelCache.h"
#include "mealkit/core/Clock.h"
#include "mealkit/core/Log.h"
#include "mealkit/core/Reflect.h"
#include "mealkit/game/InputRecord.h"
#include "mealkit/game/ProjectSettings.h"
#include "mealkit/game/Simulation.h"
#include "mealkit/net/Lockstep.h"
#include "mealkit/scene/Scene.h"

#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <type_traits>
#include <variant>

// 헤드리스 실행: 창·렌더러·오디오 없이 World만 돌린다 (CI 결정론 비교, 규칙 테스트, mp_test).
//   <게임>_headless 실행 파일(sim/ + app/Headless.cpp만, 엔진 코어만 링크)과 출시 exe의 --headless가 같은 코드다.
//   입력은 항상 봇 입력. 틱별 해시 로그(--hash-log), 입력 기록(--record-inputs)·재생(--replay-inputs, game/InputRecord.h).
//
// 게임은 app/Headless.cpp에 클래스 하나를 두고 MEALKIT_HEADLESS_GAME(클래스)를 쓴다 (docs/CODING_MODEL.md "헤드리스 진입점"):
//   using World = ...;                  // game::Simulation의 World 약속 (state, step, hash)
//   using TuningType = ...;             // assets/game.tuning
//   using ProjectSettingsType = ...;    // (선택) mealkit.project의 game.* 항목
//   static constexpr f32 kTickDt;
//   static void startWorld(World&, const game::MatchInfo&, const TuningType&, const scene::Scene& flatScene,
//                          asset::ModelCache&, u32 physicsThreads);   // 매치 시작 (GameSession도 같은 함수를 쓴다)
//   static net::PlayerInput botInput(u32 tick, u8 player);            // --bot 입력 (GameSession도 같은 함수를 쓴다)
//   static void defaults(game::HeadlessOptions&, const ProjectSettingsType&); // (선택) 포트·인원·매치 데이터 기본값
namespace mealkit::game {

struct HeadlessOptions {
    enum class Mode { Offline, Host, Join } mode = Mode::Offline;
    u16 port = 0; // 0 = 게임 defaults()가 정한다
    std::string joinAddress;
    bool loopbackOnly = false;
    u32 maxPlayers = 0;
    u32 autoStart = 0; // 호스트: 이 인원이 모이면 시작 (0 = maxPlayers)
    u32 matchData = 0; // 게임이 정하는 매치 설정값 (sandbox: 상자 수)
    u32 inputDelay = 3;
    u64 seed = 0; // 0 = 현재 시각
    u32 physicsThreads = 0;
    u32 exitTick = 0; // 재생이 아니면 필수
    bool shadow = false;
    bool anyBuild = false;
    std::string scenePath, tuningPath;
    std::string hashLogPath, recordInputsPath, replayInputsPath;
};

bool parseHeadlessArgs(int argc, char** argv, HeadlessOptions& out);
void printHeadlessUsage();

// 매치 데이터: 씬(콘텐츠 해시용 원본, 시뮬레이션용 평평한 목록), 모델(CPU 데이터만), 프로젝트 설정, 콘텐츠 해시
struct HeadlessData {
    ProjectFile project;
    scene::Scene scene;
    scene::Scene flatScene;
    asset::ModelCache models;
    u64 contentHash = 0;
};

// 실패하면 로그를 남기고 false. tuning·gameProject는 게임 타입의 값 (gameProjectType이 없으면 nullptr).
bool loadHeadlessData(const HeadlessOptions& options, const reflect::TypeInfo& tuningType, void* tuning,
                      const reflect::TypeInfo* gameProjectType, void* gameProject, HeadlessData& out);
// 세션을 연다 (혼자, 방 만들기, 참가). 실패하면 로그를 남기고 false.
bool startHeadlessSession(net::LockstepSession& session, const HeadlessOptions& options, u64 seed);
void printFinal(u32 tick, u64 hash, bool desynced);

template <class Game>
auto headlessGameProjectSettings() {
    if constexpr (requires { typename Game::ProjectSettingsType; }) return typename Game::ProjectSettingsType{};
    else return std::monostate{};
}

// --replay-inputs: 기록된 입력으로 World를 처음부터 다시 돌리고, 틱마다 기록된 해시와 비교한다
template <class World, class Start>
int replayHeadless(const HeadlessOptions& options, const HeadlessData& data, Start&& start) {
    InputRecording rec;
    std::string error;
    if (!loadInputRecording(options.replayInputsPath, rec, error)) {
        MEALKIT_ERROR("Cannot replay %s: %s", options.replayInputsPath.c_str(), error.c_str());
        return 1;
    }
    if (rec.inputs.empty()) {
        MEALKIT_ERROR("Input record %s has no ticks", options.replayInputsPath.c_str());
        return 1;
    }
    if (rec.contentHash != data.contentHash)
        MEALKIT_WARN("Input record was made with different content (scene, models, tuning or project settings) - "
                     "hashes will differ");
    u32 ticks = static_cast<u32>(rec.inputs.size());
    if (options.exitTick && options.exitTick + 1 < ticks) ticks = options.exitTick + 1;
    MEALKIT_INFO("Replaying %s: %u tick(s), %u player(s), seed %llu", options.replayInputsPath.c_str(), ticks,
                 rec.match.playerCount, static_cast<unsigned long long>(rec.match.seed));

    auto world = std::make_unique<World>();
    start(*world, rec.match, options.physicsThreads);
    HashLog hashLog;
    if (!options.hashLogPath.empty() && !hashLog.open(options.hashLogPath)) return 1;
    u32 firstMismatch = 0xFFFFFFFFu;
    u64 hash = 0;
    for (u32 t = 0; t < ticks; ++t) {
        world->step(rec.inputs[t]);
        hash = world->hash();
        hashLog.write(t, hash);
        if (hash != rec.hashes[t] && firstMismatch == 0xFFFFFFFFu) firstMismatch = t;
    }
    hashLog.close();
    const bool ok = firstMismatch == 0xFFFFFFFFu;
    if (ok) std::printf("REPLAY ok ticks=%u\n", ticks);
    else std::printf("REPLAY DESYNC tick=%u ticks=%u\n", firstMismatch, ticks);
    printFinal(ticks - 1, hash, !ok);
    return ok ? 0 : 2;
}

template <class Game>
int runHeadless(int argc, char** argv) {
    using World = typename Game::World;
    using Tuning = typename Game::TuningType;
    using GameProject = decltype(headlessGameProjectSettings<Game>());
    constexpr bool kHasGameProject = !std::is_same_v<GameProject, std::monostate>;

    log::setDialogs(false);
    HeadlessOptions options;
    if (!parseHeadlessArgs(argc, argv, options)) {
        printHeadlessUsage();
        return 1;
    }
    Tuning tuning;
    GameProject gameProject{};
    const reflect::TypeInfo* gameProjectType = nullptr;
    if constexpr (kHasGameProject) gameProjectType = &reflect::typeOf<GameProject>();
    HeadlessData data;
    if (!loadHeadlessData(options, reflect::typeOf<Tuning>(), &tuning, gameProjectType,
                          kHasGameProject ? static_cast<void*>(&gameProject) : nullptr, data))
        return 1;
    if constexpr (requires { Game::defaults(options, gameProject); }) Game::defaults(options, gameProject);

    auto start = [&](World& world, const MatchInfo& match, u32 physicsThreads) {
        Game::startWorld(world, match, tuning, data.flatScene, data.models, physicsThreads);
    };
    if (!options.replayInputsPath.empty()) return replayHeadless<World>(options, data, start);
    if (options.exitTick == 0) {
        MEALKIT_ERROR("Headless runs need --exit-tick <t> (or --replay-inputs <file>)");
        return 1;
    }

    net::LockstepSession session;
    session.setContentHash(data.contentHash);
    session.setAcceptOtherBuilds(options.anyBuild);
    const u64 seed = options.seed ? options.seed : static_cast<u64>(std::time(nullptr));
    if (!startHeadlessSession(session, options, seed)) return 1;

    PlayOptions play;
    play.bot = true;
    play.exitTick = options.exitTick;
    play.physicsThreads = options.physicsThreads;
    play.tuningPath = options.tuningPath;
    play.shadow = options.shadow;
    play.hashLogPath = options.hashLogPath;
    play.recordInputsPath = options.recordInputsPath;

    Simulation<World> sim;
    const bool offline = options.mode == HeadlessOptions::Mode::Offline;
    // 혼자면 쉬지 않고 돈다. 네트워크면 이번 반복에 틱이 나아가지 않았을 때만(입력 대기) 1ms 잔다.
    // 바쁜 대기를 하면 코어가 적은 PC(CI 러너)에서 여러 피어의 물리 작업 스레드가 돌 시간을 뺏는다.
    const u32 autoStart = options.autoStart ? options.autoStart : options.maxPlayers;
    const f64 frameDt = static_cast<f64>(Game::kTickDt) * Simulation<World>::kCatchUpTicks; // 한 번에 최대한 따라잡는다
    int exitCode = 0;
    f64 exitDeadline = 0.0;
    for (;;) {
        const f64 now = nowSeconds();
        if (exitDeadline > 0.0 && now >= exitDeadline) break;
        session.update(now);
        if (session.isHost() && session.state() == net::SessionState::Lobby && session.lobbyPlayerCount() >= autoStart)
            session.startMatch(seed, options.matchData);
        if (session.state() == net::SessionState::Ended && exitDeadline == 0.0) {
            MEALKIT_ERROR("Session ended before the exit tick: %s", session.endReason());
            exitCode = 3;
            break;
        }
        if (!sim.started() && session.state() == net::SessionState::Running) {
            sim.start(play, Game::kTickDt, [&](World& world, u32 physicsThreads) {
                start(world, {session.seed(), session.playerCount(), session.matchData()}, physicsThreads);
            });
            MEALKIT_INFO("Headless match: %u player(s), you are player %u, match data %u", session.playerCount(),
                         session.localPlayer(), session.matchData());
        }
        const u32 tickBefore = sim.started() ? sim.nextTick() : 0;
        sim.update(
            session, frameDt, [&](u32 tick) { return Game::botInput(tick, session.localPlayer()); },
            [](const World&) {});
        if (sim.finished() && exitDeadline == 0.0) {
            printFinal(sim.nextTick() - 1, sim.world().hash(), session.desynced());
            exitCode = session.desynced() ? 2 : 0;
            if (options.shadow) {
                std::printf("SHADOW %s tick=%u\n", sim.shadow().desynced ? "DESYNC" : "ok", sim.shadow().tick);
                if (sim.shadow().desynced) exitCode = 5;
            }
            std::fflush(stdout);
            exitDeadline = offline ? now : now + 1.0; // 상대가 마지막 틱을 받을 시간을 준 뒤 종료
        }
        const bool idle = exitDeadline > 0.0 || !sim.started() || sim.nextTick() == tickBefore;
        if (!offline && idle) sleepSeconds(0.001);
    }
    session.shutdown();
    return exitCode;
}

} // namespace mealkit::game

// 출시 exe의 main이 --headless를 받으면 부른다 (MEALKIT_HEADLESS_GAME이 정의한다)
int mealkitHeadlessMain(int argc, char** argv);

// app/Headless.cpp에서 한 번 쓴다: MEALKIT_HEADLESS_GAME(sandbox::Headless)
// <게임>_headless 타깃(MEALKIT_HEADLESS_TARGET)에서는 main도 된다.
#if defined(MEALKIT_HEADLESS_TARGET)
#define MEALKIT_HEADLESS_MAIN_                                                                                         \
    int main(int argc, char** argv) { return mealkitHeadlessMain(argc, argv); }
#else
#define MEALKIT_HEADLESS_MAIN_
#endif
#define MEALKIT_HEADLESS_GAME(GameClass)                                                                               \
    int mealkitHeadlessMain(int argc, char** argv) { return ::mealkit::game::runHeadless<GameClass>(argc, argv); }   \
    MEALKIT_HEADLESS_MAIN_
