#pragma once

#include "mealkit/core/Clock.h"
#include "mealkit/core/Log.h"
#include "mealkit/core/Profile.h"
#include "mealkit/core/Reflect.h"
#include "mealkit/game/GameInstance.h"
#include "mealkit/game/SimDebug.h"
#include "mealkit/game/WorldSnapshot.h"
#include "mealkit/net/Lockstep.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

// 결정론 시뮬레이션 실행기: 게임의 World를 고정 틱으로 돌린다 (app/GameSession이 하나 갖는다).
//   - 확정된 틱만 실행하고 로컬 입력을 보낸다 (락스텝 순서: canExecute → inputs → step → tickExecuted → submitLocalInput)
//   - 화면 보간용 직전 상태
//   - 그림자 시뮬레이션 (PlayOptions::shadow): 같은 입력으로 World를 하나 더 돌려 매 틱 해시를 비교한다.
//     다르면 멈추고 처음 갈라진 틱과 상태 필드 차이를 남긴다 (초기화 안 된 값, 전역 상태, 포인터 순서, 실제 시간 읽기를 잡는다)
//   - 기록 (PlayOptions::record): 틱 입력과 해시, 주기적 스냅샷 → 일시정지, 한 틱 진행, 되감기
//
// World 약속: state(MEALKIT_STRUCT, state.tick), physics, events, step(const net::TickInputs&), hash().
// 매치를 시작할 때 reset(World&, 물리 스레드 수)으로 진짜·그림자 World를 똑같이 초기화한다.
namespace mealkit::game {

template <class World>
class Simulation final : public SimDebug {
public:
    using State = std::remove_cvref_t<decltype(std::declval<World&>().state)>;
    static_assert(!reflect::hasStringField<State>(),
                  "simulation state must not contain std::string (keep names in tuning/scene data, use ids in state)");

    static constexpr u32 kSnapshotInterval = 150; // 틱 (30Hz에서 5초)
    static constexpr u32 kKeepSnapshots = 40;     // 약 3분 20초를 되감을 수 있다
    static constexpr int kCatchUpTicks = 8;       // 한 프레임에 따라잡을 최대 틱

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
            reset(*m_shadow, 1u); // 스레드 수는 결과에 영향이 없다
            m_shadowStatus.enabled = true;
        }
        m_previous = m_world->state;
        m_started = true;
        m_finished = false;
        m_captureDone = m_captureDue = false;
        m_nextTick = 0;
        m_accumulator = 0.0;
        m_paused = m_viewingPast = false;
        m_stepRequests = 0;
        m_replayError.clear();
        clearHistory();
    }

    bool started() const { return m_started; }
    bool finished() const { return m_finished; }
    u32 nextTick() const { return m_nextTick; }
    World& world() { return *m_world; }
    const World& world() const { return *m_world; }
    const State& previous() const { return m_previous; }
    // 직전 틱과 이번 틱 사이 보간 비율 (멈춰 있으면 1)
    f32 alpha() const {
        if (!m_started || m_paused) return 1.0f;
        return std::clamp(static_cast<f32>(m_accumulator / m_tickDt), 0.0f, 1.0f);
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

    // 매 프레임. sample(tick) → 이번 틱에 보낼 로컬 입력(net::PlayerInput), onTick(world) → 이번 틱 이벤트를 화면 쪽으로.
    template <class Sample, class OnTick>
    void update(net::LockstepSession& session, f64 dt, Sample&& sample, OnTick&& onTick) {
        if (!m_started || m_finished) return;
        MEALKIT_ZONE("Simulation");
        const f64 simStart = nowSeconds();
        if (m_paused) {
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
            // 입력을 기다리며 멈춘 동안 시간 빚이 무한히 쌓이지 않게 한다
            m_accumulator = std::min(m_accumulator, static_cast<f64>(m_tickDt) * kCatchUpTicks);
        }
        m_simSeconds += nowSeconds() - simStart;
    }

    // 튜닝 바꾸기 같은 매치 중 변경: 진짜·그림자 World에 똑같이 하고, 기록은 지금부터 다시 시작한다 (예전 입력으로는 재현되지 않으므로)
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
        if (!m_viewingPast) { // 지금 상태를 남겨 두고 과거로
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
        // 기록된 입력으로 다시 돌리며 기록된 해시와 비교한다
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
        m_previous = m_world->state;
        m_world->step(inputs);
        const u64 hash = m_world->hash();
        session.tickExecuted(m_nextTick, hash);

        if (m_shadow && !m_shadowStatus.desynced) {
            m_shadow->step(inputs);
            if (m_shadow->hash() != hash) reportShadowDesync();
        }
        if (m_options.record) record(inputs, hash);
        onTick(*m_world); // 틱 이벤트는 다음 step()에서 지워지므로 틱마다 바로 화면 쪽으로 옮긴다

        session.submitLocalInput(m_nextTick + session.inputDelay(), sample(m_nextTick));
        if (m_options.capture && !m_captureDone && m_nextTick >= m_options.captureTick)
            m_captureDue = m_captureDone = true;
        const bool last = m_options.exitTick && m_nextTick >= m_options.exitTick;
        ++m_nextTick;
        ++m_simTicks;
        if (last) m_finished = true;
    }

    void reportShadowDesync() {
        m_shadowStatus.desynced = true;
        m_shadowStatus.tick = m_nextTick;
        m_shadowStatus.diff.clear();
        reflect::diffStruct(reflect::typeOf<State>(), &m_world->state, &m_shadow->state, m_shadowStatus.diff, 24);
        MEALKIT_ERROR("Shadow simulation diverged at tick %u (%zu state field(s) differ%s)", m_nextTick,
                      m_shadowStatus.diff.size(), m_shadowStatus.diff.empty() ? ", physics only" : "");
        for (const std::string& line : m_shadowStatus.diff) MEALKIT_ERROR("  %s", line.c_str());
        m_paused = true; // 갈라진 틱에서 멈춰 상태를 볼 수 있게
    }

    void record(const net::TickInputs& inputs, u64 hash) {
        m_inputs.push_back(inputs);
        m_hashes.push_back(hash);
        if (m_world->state.tick % kSnapshotInterval == 0) takeSnapshot();
    }

    void takeSnapshot() {
        m_snapshots.emplace_back();
        saveSnapshot(*m_world, m_snapshots.back());
        if (m_snapshots.size() > kKeepSnapshots) {
            m_snapshots.pop_front();
            const u32 first = m_snapshots.front().tick; // 이보다 앞의 입력은 더 이상 쓸 일이 없다
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
        if (m_options.record && m_world) takeSnapshot();
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
    std::deque<net::TickInputs> m_inputs; // m_inputBase 틱부터
    std::deque<u64> m_hashes;
    u32 m_inputBase = 0;
    WorldSnapshot m_live; // 과거를 보는 동안 현재 상태
    State m_livePrevious;
};

} // namespace mealkit::game
