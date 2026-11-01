#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/core/Types.h"

#include <string>
#include <vector>

// Scenario tests (--test <file>): checks game rules on the headless World ("pushing makes the opponent fall").
// The project's tests/*.scenario (engine samples: samples/<name>/tests/). check.ps1 and project CI run all of them.
//
//   # Bot pushes the dummy off the arena          comment
//   seed 7                                        world seed (default 1)
//   scene scenes/arena.scene                      (optional) scene in assets, like --scene (default: the project's start scene)
//   inputs push.rec                               (optional) input recording next to the file (--record-inputs, editor [Play] > Save Scenario).
//                                                 Without it the solo bot plays (Game::botInput, one player)
//   ticks 900                                     (optional) ticks to run (default: the last check tick, or the whole recording)
//   tick 300 slots[0].falls == 1                  state after running tick 300: path op value
//   tick 10s phase == Results                     tick in seconds (rounded by the tick rate)
//
// Paths are WorldState field paths, the same as the editor [State] panel: a.b, array[2], pool[slot] (live slots only).
// Ops: == != (any scalar, value read the same way as files: enum names, "x y z" for Vec3, seconds for Ticks) and < <= > >= (numbers).
// Output: "SCENARIO FAIL <file>:<line> ..." per failed check, then "SCENARIO ok <file> checks=N" or "SCENARIO FAIL <file> failed=K/N".
// --update-expected rewrites the values of == checks with the current results (when simulation results change on purpose, like record_replay.ps1).
namespace mealkit::game {

inline constexpr int kScenarioFailedExit = 6;

struct ScenarioCheck {
    u32 line = 0;
    u32 tick = 0;
    bool seconds = false; // tick given in seconds (converted when the run starts, after the tick rate is known)
    f32 secondsValue = 0.0f;
    std::string path, op, expected;
};

struct Scenario {
    std::string file;
    u64 seed = 1;
    std::string scene;  // asset path, empty = start scene
    std::string inputs; // full path of the input recording, empty = bot
    u32 ticks = 0;      // 0 = last check tick (bot) or the whole recording
    std::vector<ScenarioCheck> checks;
};

bool loadScenario(const std::string& file, Scenario& out, std::string& error);

// One run: afterTick for every executed tick (state after running that tick), then finish.
class ScenarioRun {
public:
    ScenarioRun(Scenario scenario, const reflect::TypeInfo& stateType, bool updateExpected);

    u32 lastCheckTick() const;
    void afterTick(u32 tick, const void* state);
    // Prints the result line (and rewrites the file with --update-expected). Exit code 0 or kScenarioFailedExit.
    int finish(u32 lastTick);

private:
    struct Result {
        bool done = false;
        bool passed = false;
        std::string actual;
    };
    bool evaluate(const ScenarioCheck& check, const void* state, Result& result);
    bool writeExpected() const;

    Scenario m_scenario;
    const reflect::TypeInfo* m_stateType;
    bool m_updateExpected;
    std::vector<Result> m_results;
};

} // namespace mealkit::game
