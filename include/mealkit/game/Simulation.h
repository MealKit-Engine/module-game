#pragma once

#include "mealkit/core/Clock.h"
#include "mealkit/core/Log.h"
#include "mealkit/core/Profile.h"
#include "mealkit/core/Reflect.h"
#include "mealkit/game/DesyncDump.h"
#include "mealkit/game/GameInstance.h"
#include "mealkit/game/InputRecord.h"
#include "mealkit/game/SimDebug.h"
#include "mealkit/game/WorldSnapshot.h"
#include "mealkit/net/Lockstep.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

// Deterministic simulation runner: runs the game's World at a fixed tick (app/GameSession has one).
//   - runs only confirmed ticks and sends local input (lockstep order: canExecute → inputs → step → tickExecuted → submitLocalInput)
//   - previous state for view interpolation
//   - shadow simulation (PlayOptions::shadow): runs one more World with the same inputs and compares hashes every tick.
//     On mismatch, pauses and records the first diverged tick and state field differences (catches uninitialized values, global state, pointer order, reading real time)
//   - recording (PlayOptions::record): tick inputs and hashes, periodic snapshots → pause, single tick step, rewind
//   - hash log/input recording files (PlayOptions::hashLogPath, recordInputsPath): comparison across builds/platforms (game/InputRecord.h)
//   - desync dump (PlayOptions::desyncDumpDir): keeps kKeepDesyncSnapshots snapshots in multiplayer matches; when the session reports a desync,
//     reruns from the snapshot before the diverged tick up to that tick and writes the state to a file (game/DesyncDump.h). The current state is left as is
//   - late join/reconnect: the host provides snapshot bytes between ticks when the session wants them (provideSnapshot). The joiner loads the snapshot
//     on its first update and runs from that tick, running up to kJoinCatchUpTicks per frame while catching up (catchingUp)
//
// World contract: state (MEALKIT_STRUCT, state.tick), physics, events, step(const net::TickInputs&), hash().
// When a match starts, reset(World&, physics thread count) initializes the real and shadow Worlds identically.
namespace mealkit::game {

template <class World>
class Simulation final : public SimDebug {
public:
    using State = std::remove_cvref_t<decltype(std::declval<World&>().state)>;
    static_assert(!reflect::hasStringField<State>(),
                  "simulation state must not contain std::string (keep names in tuning/scene data, use ids in state)");

    static constexpr u32 kSnapshotInterval = 150; // ticks (5 seconds at 30Hz)
    static constexpr u32 kKeepSnapshots = 40;     // about 3 min 20 s can be rewound
    static constexpr u32 kKeepDesyncSnapshots = 3; // for desync dumps (multiplayer matches with recording/rewind off): covers the host's detection delay (within 256 ticks)
    static constexpr int kCatchUpTicks = 8;       // max ticks to catch up per frame
    static constexpr int kJoinCatchUpTicks = 240; // max ticks to run per frame when catching up after a late join

    template <class Reset>
    void start(const PlayOptions& options, f32 tickDt, Reset&& reset) {
        m_options = options;
        m_tickDt = tickDt;
        m_world = std::make_unique<World>();
        reset(*m_world, options.physicsThreads);
        m_shadow.reset();
        m_shadowStatus = {};
        if (options.shadow) {
            m_shadow = std::make_unique<World>();
            reset(*m_shadow, 1u); // the thread count doesn't affect results
            m_shadowStatus.enabled = true;
        }
        m_previous = m_world->state;
        m_started = true;
        m_finished = false;
        m_captureDone = m_captureDue = false;
        m_nextTick = 0;
        m_pending.clear();
        m_accumulator = 0.0;
        m_paused = m_viewingPast = false;
        m_stepRequests = 0;
        m_replayError.clear();
        m_history = options.record;
        m_desyncDumped = false;
        m_joinFailed = false;
        clearHistory();
        m_recordWriter.close();
        m_hashLog.close();
        if (!options.hashLogPath.empty()) m_hashLog.open(options.hashLogPath);
    }

    // Ends the match (discards the World, closes files). The next start begins a new match
    void stop() {
        m_world.reset();
        m_shadow.reset();
        m_started = m_finished = false;
        m_paused = m_viewingPast = false;
        m_snapshots.clear();
        m_inputs.clear();
        m_hashes.clear();
        m_live = {};
        m_recordWriter.close();
        m_hashLog.close();
    }

    bool started() const { return m_started; }
    bool finished() const { return m_finished; }
    u32 nextTick() const { return m_nextTick; }
    World& world() { return *m_world; }
    const World& world() const { return *m_world; }
    const State& previous() const { return m_previous; }
    // Interpolation fraction between the previous tick and this tick (1 when paused)
    f32 alpha() const {
        if (!m_started || m_paused) return 1.0f;
        return std::clamp(static_cast<f32>(m_accumulator / m_tickDt), 0.0f, 1.0f);
    }

    // My inputs sent but not yet executed (in target tick order): f(target tick, const net::PlayerInput&).
    // Used to draw my character moved ahead on screen only (hides input delay; the simulation is unchanged)
    template <class F>
    void forEachPendingInput(F&& f) const {
        for (const PendingInput& p : m_pending)
            if (p.tick >= m_nextTick) f(p.tick, p.input);
    }

    bool captureDue() {
        const bool due = m_captureDue;
        m_captureDue = false;
        return due;
    }
    f64 takeSimMsPerTick() {
        const f64 ms = m_simTicks ? m_simSeconds / m_simTicks * 1000.0 : 0.0;
        m_simSeconds = 0.0;
        m_simTicks = 0;
        return ms;
    }

    // Every frame. sample(tick) → local input to send this tick (net::PlayerInput), onTick(world) → this tick's events to the view side.
    template <class Sample, class OnTick>
    void update(net::LockstepSession& session, f64 dt, Sample&& sample, OnTick&& onTick) {
        if (!m_started || m_finished) return;
        MEALKIT_ZONE("Simulation");
        const f64 simStart = nowSeconds();
        if (m_nextTick == 0 && session.joinTick() > 0 && !joinMatch(session)) return;
        if (session.catchingUp() && !m_paused) { // late join: confirmed ticks faster than real time
            int budget = kJoinCatchUpTicks;
            while (budget-- > 0 && session.canExecute(m_nextTick) && !m_finished && session.catchingUp())
                executeTick(session, sample, onTick);
            m_accumulator = 0.0;
        } else if (m_paused) {
            m_accumulator = 0.0;
            if (m_stepRequests > 0 && !m_viewingPast && session.canExecute(m_nextTick)) {
                --m_stepRequests;
                executeTick(session, sample, onTick);
            }
        } else {
            m_accumulator += dt;
            int budget = kCatchUpTicks;
            while (m_accumulator >= m_tickDt && budget-- > 0 && session.canExecute(m_nextTick) && !m_finished &&
                   !m_paused) {
                executeTick(session, sample, onTick);
                m_accumulator -= m_tickDt;
            }
            // Keeps time debt from piling up forever while stalled waiting for input
            m_accumulator = std::min(m_accumulator, static_cast<f64>(m_tickDt) * kCatchUpTicks);
        }
        if (session.desynced() && !m_desyncDumped && !m_options.desyncDumpDir.empty()) dumpDesync(session);
        if (session.snapshotWanted() && !m_viewingPast) { // host: someone is joining (state between the current ticks)
            WorldSnapshot snapshot;
            saveSnapshot(*m_world, snapshot);
            std::vector<u8> bytes;
            writeSnapshotBytes(snapshot, bytes);
            session.provideSnapshot(m_nextTick, bytes);
        }
        m_simSeconds += nowSeconds() - simStart;
    }

    // Mid-match changes like tuning changes: applied identically to the real and shadow Worlds, and recording restarts from now (old inputs can't reproduce it)
    template <class F>
    void modifyWorlds(F&& f) {
        if (!m_started) return;
        returnToLive();
        f(*m_world);
        if (m_shadow) f(*m_shadow);
        clearHistory();
    }

    // ---------------------------------------------------------------- SimDebug

    Timeline timeline() const override {
        Timeline t;
        t.recording = m_options.record;
        t.paused = m_paused;
        t.viewingPast = m_viewingPast;
        t.first = m_snapshots.empty() ? m_nextTick : m_snapshots.front().tick;
        t.live = m_nextTick;
        t.view = m_viewingPast ? m_world->state.tick : m_nextTick;
        size_t bytes = m_inputs.size() * sizeof(net::TickInputs) + m_hashes.size() * sizeof(u64) + m_live.byteSize();
        for (const WorldSnapshot& s : m_snapshots) bytes += s.byteSize();
        t.memoryBytes = bytes;
        return t;
    }

    void setPaused(bool paused) override {
        if (!m_started) return;
        if (!paused) returnToLive();
        m_paused = paused;
        m_stepRequests = 0;
    }

    void stepOnce() override {
        if (!m_started || !m_paused) return;
        if (m_viewingPast) seek(m_world->state.tick + 1);
        else ++m_stepRequests;
    }

    bool seek(u32 tick) override {
        if (!m_started || !m_paused || !m_options.record || m_snapshots.empty()) return false;
        tick = std::clamp(tick, m_snapshots.front().tick, m_nextTick);
        if (tick == m_nextTick) {
            returnToLive();
            return true;
        }
        if (!m_viewingPast) { // keep the current state and go to the past
            saveSnapshot(*m_world, m_live);
            m_livePrevious = m_previous;
            m_viewingPast = true;
        }
        const WorldSnapshot* from = &m_snapshots.front();
        for (const WorldSnapshot& s : m_snapshots)
            if (s.tick <= tick) from = &s;
        if (!loadSnapshot(*m_world, *from)) {
            m_replayError = "The snapshot of tick " + std::to_string(from->tick) +
                            " does not restore to the hash it was saved with. A value that lives across ticks may "
                            "not be declared with MEALKIT_FIELD";
            MEALKIT_ERROR("Rewind: snapshot of tick %u does not restore to the saved hash (undeclared state?)",
                          from->tick);
        }
        // Rerun with the recorded inputs and compare with the recorded hashes
        for (u32 t = from->tick; t < tick; ++t) {
            m_previous = m_world->state;
            m_world->step(m_inputs[t - m_inputBase]);
            if (m_world->hash() != m_hashes[t - m_inputBase] && m_replayError.empty()) {
                m_replayError = "Replayed tick " + std::to_string(t) +
                                " does not match the recorded hash. There is undeclared state (missing MEALKIT_FIELD) "
                                "or nondeterminism";
                MEALKIT_ERROR(
                    "Rewind: replayed tick %u does not match the recorded hash (undeclared state or nondeterminism)",
                    t);
            }
        }
        m_previous = m_world->state;
        return true;
    }

    const ShadowStatus& shadow() const override { return m_shadowStatus; }
    const std::string& replayError() const override { return m_replayError; }

private:
    template <class Sample, class OnTick>
    void executeTick(net::LockstepSession& session, Sample& sample, OnTick& onTick) {
        const net::TickInputs& inputs = session.inputs(m_nextTick);
        if (m_nextTick == 0 && !m_history && !session.isOffline() && !m_options.desyncDumpDir.empty()) {
            m_history = true; // snapshots for desync dumps (match values and whether it's multiplayer are known after the session becomes Running)
            clearHistory();
        }
        m_previous = m_world->state;
        stepWorld(*m_world, inputs, m_nextTick);
        const u64 hash = m_world->hash();
        session.tickExecuted(m_nextTick, hash);
        if (m_nextTick == 0 && !m_options.recordInputsPath.empty()) // match values are confirmed after the session becomes Running
            m_recordWriter.open(m_options.recordInputsPath, matchInfo(session), session.contentHash());
        m_recordWriter.write(inputs, hash);
        m_hashLog.write(m_nextTick, hash);

        if (m_shadow && !m_shadowStatus.desynced) {
            m_shadow->step(inputs); // the --desync-test perturbation applies only to the real World (the shadow catches it)
            if (m_shadow->hash() != hash) reportShadowDesync();
        }
        if (m_history) record(inputs, hash);
        onTick(*m_world); // tick events are cleared by the next step(), so they're moved to the view side right away every tick

        const net::PlayerInput local = sample(m_nextTick);
        const u32 target = m_nextTick + session.inputDelay();
        session.submitLocalInput(target, local);
        while (!m_pending.empty() && m_pending.front().tick <= m_nextTick) m_pending.pop_front(); // executed ticks
        if (m_pending.empty() || m_pending.back().tick < target) m_pending.push_back({target, local});
        if (m_options.capture && !m_captureDone && m_nextTick >= m_options.captureTick)
            m_captureDue = m_captureDone = true;
        const bool last = m_options.exitTick && m_nextTick >= m_options.exitTick;
        ++m_nextTick;
        ++m_simTicks;
        if (last) {
            m_finished = true;
            m_recordWriter.close();
            m_hashLog.close();
        }
    }

    void reportShadowDesync() {
        m_shadowStatus.desynced = true;
        m_shadowStatus.tick = m_nextTick;
        m_shadowStatus.diff.clear();
        reflect::diffStruct(reflect::typeOf<State>(), &m_world->state, &m_shadow->state, m_shadowStatus.diff, 24);
        MEALKIT_ERROR("Shadow simulation diverged at tick %u (%zu state field(s) differ%s)", m_nextTick,
                      m_shadowStatus.diff.size(), m_shadowStatus.diff.empty() ? ", physics only" : "");
        for (const std::string& line : m_shadowStatus.diff) MEALKIT_ERROR("  %s", line.c_str());
        m_paused = true; // pause at the diverged tick so the state can be inspected
    }

    void record(const net::TickInputs& inputs, u64 hash) {
        m_inputs.push_back(inputs);
        m_hashes.push_back(hash);
        if (m_world->state.tick % kSnapshotInterval == 0) takeSnapshot();
    }

    void takeSnapshot() {
        m_snapshots.emplace_back();
        saveSnapshot(*m_world, m_snapshots.back());
        if (m_snapshots.size() > (m_options.record ? kKeepSnapshots : kKeepDesyncSnapshots)) {
            m_snapshots.pop_front();
            const u32 first = m_snapshots.front().tick; // inputs before this are no longer needed
            while (m_inputBase < first && !m_inputs.empty()) {
                m_inputs.pop_front();
                m_hashes.pop_front();
                ++m_inputBase;
            }
        }
    }

    void clearHistory() {
        m_snapshots.clear();
        m_inputs.clear();
        m_hashes.clear();
        m_inputBase = m_world ? m_world->state.tick : 0;
        m_live = {};
        if (m_history && m_world) takeSnapshot();
    }

    void stepWorld(World& world, const net::TickInputs& inputs, u32 tick) {
        if (m_options.desyncTestTick && tick == m_options.desyncTestTick) world.physics.nudgeForTest();
        world.step(inputs);
    }

    // Late join: replace the World with the snapshot the host sent and run from that tick (start's reset only sets up static data).
    // The same id may have a different body, so all physics bodies are deleted and the snapshot's are created. false on failure (the match isn't run)
    bool joinMatch(net::LockstepSession& session) {
        std::vector<u8> bytes;
        if (!session.takeJoinSnapshot(bytes)) return m_joinFailed ? false : true;
        WorldSnapshot snapshot;
        bool ok = readSnapshotBytes(bytes.data(), bytes.size(), snapshot) && snapshot.tick == session.joinTick();
        for (World* world : {m_world.get(), m_shadow.get()}) {
            if (!world || !ok) continue;
            world->physics.clearBodies();
            ok = loadSnapshot(*world, snapshot);
        }
        if (!ok) {
            m_joinFailed = true;
            MEALKIT_ERROR("Join: the match snapshot of tick %u did not restore to the host's hash (different build or "
                          "undeclared state)", session.joinTick());
            return false;
        }
        m_nextTick = snapshot.tick;
        m_previous = m_world->state;
        m_pending.clear();
        if (!session.isOffline() && !m_options.desyncDumpDir.empty()) m_history = true; // for desync dumps (couldn't be turned on at tick 0)
        clearHistory();
        m_inputBase = m_nextTick;
        MEALKIT_INFO("Joined the match at tick %u (%zu bytes)", m_nextTick, bytes.size());
        return true;
    }

    // Writes the state of the desync tick reported by the session to a file (once). Loads the snapshot before that tick, reruns to that tick, then restores the current state
    void dumpDesync(const net::LockstepSession& session) {
        const u32 tick = session.desyncTick();
        if (tick >= m_nextTick) return; // a tick not yet executed (next frame)
        m_desyncDumped = true;
        DesyncDumpHeader h{session.localPlayer(), tick, session.seed(), session.contentHash(), {}};
        std::vector<std::pair<u32, u64>> hashes;
        for (u32 t = m_inputBase; t <= tick && t - m_inputBase < m_hashes.size(); ++t)
            hashes.emplace_back(t, m_hashes[t - m_inputBase]);
        const WorldSnapshot* from = nullptr;
        for (const WorldSnapshot& s : m_snapshots)
            if (s.tick <= tick) from = &s;
        const std::string path = desyncDumpPath(m_options.desyncDumpDir, h.seed, tick, h.player);
        if (!m_history || !from || tick < m_inputBase) { // the remaining recording doesn't cover that tick: write the current state
            h.note = "no snapshot before the desync tick; this is the state at tick " + std::to_string(m_nextTick - 1);
            writeDesyncDump(path, h, *m_world, hashes);
            MEALKIT_ERROR("Desync dump (current state only): %s", path.c_str());
            return;
        }
        returnToLive();
        WorldSnapshot live;
        saveSnapshot(*m_world, live);
        const State previous = m_previous;
        bool ok = loadSnapshot(*m_world, *from);
        for (u32 t = from->tick; ok && t <= tick; ++t) stepWorld(*m_world, m_inputs[t - m_inputBase], t);
        if (!ok) h.note = "the snapshot did not restore to its hash (undeclared state)";
        else if (m_world->hash() != m_hashes[tick - m_inputBase])
            h.note = "replaying from the snapshot did not reproduce this peer's hash (undeclared state)";
        writeDesyncDump(path, h, *m_world, hashes);
        if (!loadSnapshot(*m_world, live)) MEALKIT_ERROR("Desync dump: the live state did not restore to its hash");
        m_previous = previous;
        MEALKIT_ERROR("Desync dump written: %s (compare with tools\\desync_diff.ps1)", path.c_str());
    }

    void returnToLive() {
        if (!m_viewingPast) return;
        if (!loadSnapshot(*m_world, m_live))
            MEALKIT_ERROR("Returning to the live tick: state hash differs after restore");
        m_previous = m_livePrevious;
        m_viewingPast = false;
        m_live = {};
    }

    PlayOptions m_options;
    f32 m_tickDt = 1.0f / 30.0f;
    std::unique_ptr<World> m_world;
    std::unique_ptr<World> m_shadow;
    State m_previous;
    bool m_started = false;
    bool m_finished = false;
    bool m_captureDone = false;
    bool m_captureDue = false;
    u32 m_nextTick = 0;
    f64 m_accumulator = 0.0;
    f64 m_simSeconds = 0.0;
    u32 m_simTicks = 0;

    bool m_paused = false;
    bool m_viewingPast = false;
    int m_stepRequests = 0;
    ShadowStatus m_shadowStatus;
    std::string m_replayError;

    std::deque<WorldSnapshot> m_snapshots;
    std::deque<net::TickInputs> m_inputs; // from tick m_inputBase
    std::deque<u64> m_hashes;
    u32 m_inputBase = 0;
    WorldSnapshot m_live; // current state while viewing the past
    State m_livePrevious;

    bool m_history = false;      // keeps inputs/hashes/snapshots (PlayOptions::record, or desync dumps)
    bool m_desyncDumped = false;
    bool m_joinFailed = false;
    InputRecordWriter m_recordWriter;
    HashLog m_hashLog;

    struct PendingInput {
        u32 tick = 0;
        net::PlayerInput input;
    };
    std::deque<PendingInput> m_pending; // my inputs sent but not yet executed
};

} // namespace mealkit::game
