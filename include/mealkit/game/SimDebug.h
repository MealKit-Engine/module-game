#pragma once

#include "mealkit/core/Types.h"

#include <string>
#include <vector>

// The editor's window for controlling the running simulation (GameInstance::debug()). Implemented by game::Simulation<World>.
namespace mealkit::game {

struct Timeline {
    bool recording = false; // keeping input recordings and snapshots (rewind possible)
    bool paused = false;
    bool viewingPast = false; // replaying the recording to view a past tick (continuing returns to the present)
    u32 first = 0;            // earliest tick that can be rewound to
    u32 live = 0;             // tick actually advanced to (next tick to run)
    u32 view = 0;             // tick shown on screen
    size_t memoryBytes = 0;   // memory used by snapshots/input recording
};

struct ShadowStatus {
    bool enabled = false;
    bool desynced = false;
    u32 tick = 0;                  // first diverged tick
    std::vector<std::string> diff; // "path: real -> shadow" (state fields). If empty, only physics state differs
};

class SimDebug {
public:
    virtual ~SimDebug() = default;

    virtual Timeline timeline() const = 0;
    virtual void setPaused(bool paused) = 0; // continuing returns to the current tick even while viewing the past
    virtual void stepOnce() = 0;             // one tick forward while paused (from the recording if viewing the past)
    virtual bool seek(u32 tick) = 0;         // goes to a tick in the recorded range while paused
    virtual const ShadowStatus& shadow() const = 0;
    // Description when the result rerun by rewind differs from the recording (undeclared state, determinism violation). Empty string if none
    virtual const std::string& replayError() const = 0;
};

} // namespace mealkit::game
