#pragma once

#include "mealkit/asset/ModelCache.h"
#include "mealkit/core/Clock.h"
#include "mealkit/core/Log.h"
#include "mealkit/core/Reflect.h"
#include "mealkit/game/BuildInfo.h"
#include "mealkit/game/GameData.h"
#include "mealkit/game/GameOptions.h"
#include "mealkit/game/InputRecord.h"
#include "mealkit/game/DesyncDump.h"
#include "mealkit/game/ProjectSettings.h"
#include "mealkit/game/Scenario.h"
#include "mealkit/game/Simulation.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/System.h"
#include "mealkit/scene/Scene.h"
#if MEALKIT_HAS_NET
#include "mealkit/net/Lockstep.h"
#else
#include "mealkit/session/LocalSession.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <type_traits>
#include <variant>

// Headless run: runs only the World without window/renderer/audio (CI determinism comparison, rule tests, mp_test).
//   The <game>_headless executable (sim/ + app/Headless.cpp only, links only the engine core) and the shipping exe's --headless are the same code.
//   Input is always bot input. Per-tick hash log (--hash-log), input recording (--record-inputs) and replay (--replay-inputs, game/InputRecord.h).
//   Scenario tests (--test <file>, game/Scenario.h): the solo bot run or a recording replay with state checks after each tick.
//
// The game puts one class in app/Headless.cpp and uses MEALKIT_HEADLESS_GAME(class) (docs/CODING_MODEL.md "Headless entry point"):
//   using World = ...;                  // game::Simulation's World contract (state, step, hash)
//   using TuningType = ...;             // assets/game.tuning
//   using ProjectSettingsType = ...;    // (optional) game.* items in mealkit.project
//   static void startWorld(World&, const game::MatchInfo&, const TuningType&, const scene::Scene& flatScene,
//                          asset::ModelCache&, u32 physicsThreads);   // match start (GameSession uses the same function)
//   static session::PlayerInput botInput(u32 tick, u8 player);            // --bot input (GameSession uses the same function)
//   static void botCommands(u32 tick, u8 player, session::Session&); // (optional) --bot confirmed commands (mealkit/sim/Commands.h)
//   static void botLoadout(u8 player, session::Session&);       // (optional) bot match start data (mealkit/game/Loadout.h)
//   static void defaults(game::MatchOptions&, const ProjectSettingsType&); // (optional) port/player count/match data defaults
namespace mealkit::game {

// Headless runs own their session: lockstep with the net module, otherwise local (solo and replays only)
#if MEALKIT_HAS_NET
using HeadlessSession = net::LockstepSession;
#else
using HeadlessSession = session::LocalSession;
#endif

// Opens a session (solo, create room, join). On failure, logs and returns false.
bool startHeadlessSession(HeadlessSession& session, const MatchOptions& options, u64 seed);
void printFinal(u32 tick, u64 hash, bool desynced);
void printFinalState(const reflect::TypeInfo& type, const void* state); // --final-state

template <class Game>
auto headlessGameProjectSettings() {
    if constexpr (requires { typename Game::ProjectSettingsType; }) return typename Game::ProjectSettingsType{};
    else return std::monostate{};
}

// --replay-inputs: reruns the World from the start with recorded inputs and compares with the recorded hash every tick
// With --test (scenario), state checks every tick; recorded hashes that differ are only reported (rules may have changed on purpose)
template <class World, class Start>
int replayHeadless(const MatchOptions& options, u64 contentHash, Start&& start, ScenarioRun* scenario = nullptr) {
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
    if (rec.contentHash != contentHash)
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
        if (scenario) scenario->afterTick(t, &world->state);
        if (hash != rec.hashes[t] && firstMismatch == 0xFFFFFFFFu) firstMismatch = t;
    }
    hashLog.close();
    const bool ok = firstMismatch == 0xFFFFFFFFu;
    if (scenario) {
        if (!ok) MEALKIT_INFO("Recorded hashes differ from tick %u (simulation results changed since recording)", firstMismatch);
        return scenario->finish(ticks - 1);
    }
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
    useBuildProjectDirectory(MEALKIT_THIS_BUILD);
    MatchOptions options;
    if (!parseGameArgs(argc, argv, options, nullptr)) {
        printGameUsage(nullptr, true);
        return 1;
    }
    MEALKIT_INFO("CPU: %s", cpuName().c_str()); // for recording determinism on other PCs
    GameTypes types;
    types.objectTypes = &reflect::moduleObjectTypes();
    types.tuningType = &reflect::typeOf<Tuning>();
    if constexpr (kHasGameProject) types.projectSettingsType = &reflect::typeOf<GameProject>();
    types.tables = &reflect::moduleTables();
    asset::ModelCache models; // CPU data only (no renderer)
    Scenario scenarioFile;
    if (!options.testPath.empty()) {
        std::string error;
        if (!loadScenario(options.testPath, scenarioFile, error, options.updateExpected)) {
            MEALKIT_ERROR("Cannot read scenario %s: %s", options.testPath.c_str(), error.c_str());
            return 1;
        }
        // The scenario decides the match: the solo offline bot, or its input recording
        options.mode = MatchOptions::Mode::Offline;
        options.seed = scenarioFile.seed;
        if (!scenarioFile.scene.empty()) { // an asset path (scenes/x.scene); a missing scene fails instead of quietly using the start scene
            options.scenePath = findAssetFile(scenarioFile.scene);
            if (options.scenePath.empty()) {
                MEALKIT_ERROR("%s: scene %s not found in assets", options.testPath.c_str(), scenarioFile.scene.c_str());
                return 1;
            }
        }
        options.replayInputsPath = scenarioFile.inputs;
    }
    MatchData data;
    if (!loadMatchData(types, {options.scenePath, options.tuningPath}, models, data)) return 1;
    options.inputDelay = msToTicks(options.inputDelayMs); // after the tick rate (project setting) is known
    const Tuning& tuning = *static_cast<const Tuning*>(data.tuning.data());
    if constexpr (kHasGameProject)
        if constexpr (requires { Game::defaults(options, std::declval<const GameProject&>()); })
            Game::defaults(options, *static_cast<const GameProject*>(data.gameProject.data()));
    const scene::Scene flatScene = scene::flattened(data.scene);

    auto start = [&](World& world, const MatchInfo& match, u32 physicsThreads) {
        Game::startWorld(world, match, tuning, flatScene, models, physicsThreads);
    };
    std::unique_ptr<ScenarioRun> scenario;
    if (!options.testPath.empty()) { // after loading: tick times in seconds need the project's tick rate
        using State = std::remove_cvref_t<decltype(std::declval<World&>().state)>;
        scenario = std::make_unique<ScenarioRun>(scenarioFile, reflect::typeOf<State>(), options.updateExpected);
        if (scenarioFile.ticks) options.exitTick = scenarioFile.ticks - 1;
        else if (scenarioFile.inputs.empty()) options.exitTick = std::max(scenario->lastCheckTick(), 1u);
        else options.exitTick = 0; // whole recording
    }
    if (!options.replayInputsPath.empty())
        return replayHeadless<World>(options, data.contentHash, start, scenario.get());
    if (options.exitTick == 0) {
        MEALKIT_ERROR("Headless runs need --exit-tick <t> (or --replay-inputs <file>)");
        return 1;
    }

    HeadlessSession session;
    session.setContentHash(data.contentHash);
    if (!options.playerName.empty()) session.setLocalName(options.playerName);
    const BuildInfo build = MEALKIT_THIS_BUILD; // expanded in the game's Headless.cpp (build info header)
#if MEALKIT_HAS_NET
    session.setAcceptOtherBuilds(options.anyBuild);
    session.setConditions({options.netLagMs, options.netJitterMs, options.netLossPercent});
    session.setBuild(build.simVersion, build.gameVersion);
    session.setContentParts(data.contentParts);
#endif
    MEALKIT_INFO("Build: version %s, commit %s, simulation %016llx, %s", build.gameVersion, build.commit,
                 static_cast<unsigned long long>(build.simVersion), session::determinismGroup());
    session::AutoDelay autoDelay = autoDelayOf(data.project.settings, 1000.0f / static_cast<f32>(reflect::tickRate()));
    autoDelay.enabled = autoDelay.enabled && !options.delayGiven; // --delay: fixed
#if MEALKIT_HAS_NET
    session.setAutoDelay(autoDelay);
#endif
    const u64 seed = options.seed ? options.seed : static_cast<u64>(std::time(nullptr));
    auto loadout = [&] {
        if constexpr (requires { Game::botLoadout(u8{0}, session); }) Game::botLoadout(session.localPlayer(), session);
    };
    loadout(); // solo starts right when the session opens, so before that
    if (!startHeadlessSession(session, options, seed)) return 1;

    PlayOptions play;
    play.bot = true;
    play.exitTick = options.exitTick;
    play.physicsThreads = options.physicsThreads;
    play.tuningPath = options.tuningPath;
    play.shadow = options.shadow;
    play.hashLogPath = options.hashLogPath;
    play.recordInputsPath = options.recordInputsPath;
    play.desyncDumpDir = desyncDumpFolder(userFolder(data.project.settings));
    play.desyncTestTick = options.desyncTestTick;

    Simulation<World> sim;
    const bool offline = options.mode == MatchOptions::Mode::Offline;
    // Solo runs without rest. With networking, sleeps 1ms only when no tick advanced this iteration (waiting for input).
    // Busy waiting would steal time from the physics worker threads of multiple peers on PCs with few cores (CI runners).
    const u32 autoStart = options.autoStart ? options.autoStart : options.maxPlayers;
    const f64 frameDt = static_cast<f64>(tickDt()) * Simulation<World>::kCatchUpTicks; // catch up as much as possible at once
    int exitCode = 0;
    u32 scenarioTick = 0; // ticks executed (onTick runs once per tick, from tick 0)
    f64 exitDeadline = 0.0;
    for (;;) {
        const f64 now = nowSeconds();
        if (exitDeadline > 0.0 && now >= exitDeadline) break;
        session.update(now);
        if (session.state() == session::SessionState::Lobby) loadout();
        if (session.isHost() && session.state() == session::SessionState::Lobby && session.lobbyPlayerCount() >= autoStart)
            session.startMatch(seed, options.matchData);
        if (session.state() == session::SessionState::Ended && exitDeadline == 0.0) {
            MEALKIT_ERROR("Session ended before the exit tick: %s", session.endReason());
            exitCode = 3;
            break;
        }
        if (!sim.started() && session.state() == session::SessionState::Running) {
            sim.start(play, tickDt(), [&](World& world, u32 physicsThreads) {
                start(world, matchInfo(session), physicsThreads);
            });
            MEALKIT_INFO("Headless match: %u player(s), you are player %u, match data %u", session.playerCount(),
                         session.localPlayer(), session.matchData());
        }
        const u32 tickBefore = sim.started() ? sim.nextTick() : 0;
        sim.update(
            session, frameDt,
            [&](u32 tick) {
                if constexpr (requires { Game::botCommands(tick, session.localPlayer(), session); })
                    Game::botCommands(tick, session.localPlayer(), session);
                return Game::botInput(tick, session.localPlayer());
            },
            [&](const World& world) {
                if (scenario) scenario->afterTick(scenarioTick++, &world.state);
            });
        // Simulated crash: only the first successor host, the moment it passes this tick (later successors don't crash)
        if (options.abortHostTick && session.isHost() && session.hostMigrations() == 1 && sim.started() &&
            tickBefore <= options.abortHostTick && sim.nextTick() > options.abortHostTick) {
            MEALKIT_WARN("--abort-host-at %u: exiting without closing the session", options.abortHostTick);
            std::fflush(stdout);
            std::_Exit(9);
        }
        if (sim.finished() && exitDeadline == 0.0) {
            printFinal(sim.nextTick() - 1, sim.world().hash(), session.desynced());
            if (options.finalState) {
                using State = std::remove_cvref_t<decltype(sim.world().state)>;
                printFinalState(reflect::typeOf<State>(), &sim.world().state);
            }
            exitCode = session.desynced() ? 2 : 0;
            if (scenario) {
                const int scenarioCode = scenario->finish(sim.nextTick() - 1);
                if (exitCode == 0) exitCode = scenarioCode;
            }
            if (options.shadow) {
                std::printf("SHADOW %s tick=%u\n", sim.shadow().desynced ? "DESYNC" : "ok", sim.shadow().tick);
                if (sim.shadow().desynced) exitCode = 5;
            }
            std::fflush(stdout);
            exitDeadline = offline ? now : now + 1.0; // exit after giving the other side time to receive the last tick
        }
        const bool idle = exitDeadline > 0.0 || !sim.started() || sim.nextTick() == tickBefore;
        if (!offline && idle) sleepSeconds(0.001);
    }
    session.shutdown();
    return exitCode;
}

} // namespace mealkit::game

// Called when the shipping exe's main receives --headless (defined by MEALKIT_HEADLESS_GAME)
int mealkitHeadlessMain(int argc, char** argv);

// Used once in app/Headless.cpp: MEALKIT_HEADLESS_GAME(sandbox::Headless)
// In the <game>_headless target (MEALKIT_HEADLESS_TARGET) it also becomes main.
#if defined(MEALKIT_HEADLESS_TARGET)
#define MEALKIT_HEADLESS_MAIN_                                                                                         \
    int main(int argc, char** argv) { return mealkitHeadlessMain(argc, argv); }
#else
#define MEALKIT_HEADLESS_MAIN_
#endif
#define MEALKIT_HEADLESS_GAME(GameClass)                                                                               \
    int mealkitHeadlessMain(int argc, char** argv) { return ::mealkit::game::runHeadless<GameClass>(argc, argv); }   \
    MEALKIT_HEADLESS_MAIN_
