#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/core/Types.h"
#include "mealkit/game/GameInstance.h"
#include "mealkit/game/Simulation.h"

#include <type_traits>

// GameInstance for a game that runs one game::Simulation<World> (every MealKit game: the samples and templates derive from this).
// Implements the GameInstance functions that only report the simulation (tick, hash, bodies, physics, state view, debug, tuning),
// so the game's GameSession writes only what it decides: start, stop, update, buildFrame, and optionally lobby, menus, debugUi.
//   class GameSession final : public mealkit::game::SimulationGame<World> { ... };  // sim() is the runner, TuningType comes from World
// World contract (on top of Simulation's): physics (physics::PhysicsWorld), tuning (the game's Tuning) and setTuning(const Tuning&).
// lobby() keeps its own arguments: the engine calls it before the view services in GameContext exist (solo play from the command line).
// Not in Simulation.h, which is part of the simulation version (changing this file doesn't invalidate replays or peers).
namespace mealkit::game {

template <class World>
class SimulationGame : public GameInstance {
public:
    using TuningType = std::remove_cvref_t<decltype(std::declval<World&>().tuning)>; // editor [Tuning] panel (MEALKIT_GAME_MODULE)

    bool started() const override { return m_sim.started(); }
    u32 nextTick() const override { return m_sim.nextTick(); }
    u64 stateHash() const override { return m_sim.started() ? m_sim.world().hash() : 0; }
    u32 activeBodies() const override { return m_sim.started() ? m_sim.world().physics.activeBodyCount() : 0; }
    bool finished() const override { return m_sim.finished(); }
    bool captureDue() override { return m_sim.captureDue(); }
    f64 takeSimMsPerTick() override { return m_sim.takeSimMsPerTick(); }
    // Sound occlusion rays read the match's physics (between ticks)
    const physics::PhysicsWorld* physics() const override { return m_sim.started() ? &m_sim.world().physics : nullptr; }
    void setTuning(const void* tuning) override {
        const TuningType& value = *static_cast<const TuningType*>(tuning);
        m_sim.modifyWorlds([&](World& world) { world.setTuning(value); });
    }
    reflect::View stateView() const override {
        if (!m_sim.started()) return {};
        return {&reflect::typeOf<typename Simulation<World>::State>(), &m_sim.world().state};
    }
    SimDebug* debug() override { return &m_sim; }

protected:
    // Fixed tick execution, previous state for interpolation, shadow simulation, recording/rewind (Simulation.h)
    Simulation<World>& sim() { return m_sim; }
    const Simulation<World>& sim() const { return m_sim; }

private:
    Simulation<World> m_sim;
};

} // namespace mealkit::game
