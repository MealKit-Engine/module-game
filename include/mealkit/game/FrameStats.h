#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/render/Renderer.h"

#include <string>
#include <vector>

namespace mealkit {
namespace session {
class Session;
}
namespace audio {
class Engine;
}
} // namespace mealkit

// Collecting performance numbers (docs/CODING_MODEL.md "Collecting and showing performance numbers"). Measurement is done by each subsystem's stats(),
// and this averages per report period into one batch (FrameSample) and text. Shared by the window title, F1 overlay, perf log, and editor status bar.
//   game::FrameStats perf;
//   if (perf.update(dt, {&renderer, &session, &audio, &game})) window.setTitle(perf.line().c_str());
// For timing inside functions, use the Tracy marker MEALKIT_ZONE (mealkit/core/Profile.h).
namespace mealkit::game {

class GameInstance;

// Sources: missing ones (nullptr) leave that item empty (session in edit mode, renderer in headless, etc.)
struct FrameSources {
    const render::Renderer* renderer = nullptr;
    const session::Session* session = nullptr;
    const audio::Engine* audio = nullptr;
    GameInstance* game = nullptr; // takeSimMsPerTick clears the average when read, so only FrameStats calls it
};

// Values for one report period
struct FrameSample {
    f64 seconds = 0.0; // time this batch covers
    u32 frames = 0;
    f64 frameMs = 0.0;      // average frame time
    f64 worstFrameMs = 0.0; // longest frame within the period
    f64 p99FrameMs = 0.0;   // 1% worst frames: 1% of frames are longer than this (frameMsPercentile 99)
    f64 fps = 0.0;

    bool hasGame = false;
    f64 simMsPerTick = 0.0;
    u32 tick = 0; // next tick to run

    bool hasSession = false;
    u32 localPlayer = 0, playerCount = 0;
    f32 rttMs = 0.0f;
    u32 sentBytesPerSec = 0, recvBytesPerSec = 0;
    bool desynced = false;

    bool hasRenderer = false;
    u32 drawCalls = 0, instances = 0, culled = 0;
    bool vsync = false;
    bool hasGpuTime = false; // there are timestamp query results (render::RenderStats::gpuValid)
    f64 gpuMs = 0.0;         // average GPU frame time
    f64 gpuPassMs[render::kGpuPassCount] = {};
    u64 gpuMemoryUsed = 0, gpuMemoryBudget = 0; // bytes (0 if unknown)

    u64 processMemory = 0, processMemoryPeak = 0; // bytes (platform processMemory, when there is a renderer)
    u64 taggedMemory = 0; // bytes counted by memory tags (core/Memory.h, every tag summed, when there is a renderer); per tag in json() and [Debug] > [Memory]

    bool hasAudio = false;
    u32 realVoices = 0, virtualVoices = 0;
    f64 audioMs = 0.0;
};

// Percentile (ms) of a list of frame times (seconds). Sorts the list. 0 if empty
f64 frameMsPercentile(std::vector<f64>& frameSeconds, f64 percent);

class FrameStats {
public:
    static constexpr f64 kDefaultPeriod = 0.25; // seconds (window title at 4Hz)

    explicit FrameStats(f64 period = kDefaultPeriod) : m_period(period) {}

    // Once per frame (dt in seconds). On frames where a report period ends, collects values from sources, updates sample(), and returns true.
    bool update(f64 dt, const FrameSources& from);
    // The last finished period (all 0 before the first period)
    const FrameSample& sample() const { return m_sample; }
    // "60 fps 16.67 ms (worst 18.2) | sim 0.42 ms/tick | ..." (window title, log). Performance numbers are for developers, so English as is
    std::string line() const;
    // Items of line() one per line (F1 overlay)
    std::string lines() const;
    // sample() as one JSON object (the "stats" topic of the live connection status stream, mealkit/live/Server.h). Batches from missing sources are omitted
    std::string json() const;

private:
    f64 m_period;
    f64 m_elapsed = 0.0;
    f64 m_worst = 0.0;
    u32 m_frames = 0;
    std::vector<f64> m_frameTimes;
    f64 m_gpuMs = 0.0;
    f64 m_gpuPassMs[render::kGpuPassCount] = {};
    u32 m_gpuFrames = 0;
    FrameSample m_sample;
};

} // namespace mealkit::game
