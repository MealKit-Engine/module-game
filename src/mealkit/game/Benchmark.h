#pragma once

#include "mealkit/game/FrameStats.h"

#include <string>
#include <vector>

// Benchmark mode (docs/ROADMAP.md stage 2 "Benchmark mode"): `--benchmark [seconds]` of the shipping exe.
// Fixed seed + bot input, so the same build and data run the same match every time (the camera follows the bot character along the same path).
// Values are collected by game::FrameStats; this accumulates those batches and frame times over the measurement window and writes a text report (benchmark.txt).
//   Benchmark bench({30.0, 3.0});
//   every frame: if (bench.frame(dt, matchRunning, perf.update(...) ? &perf.sample() : nullptr)) { bench.write(...); done }
namespace mealkit::game {

struct BenchmarkConfig {
    f64 seconds = 30.0;      // measurement time
    f64 warmupSeconds = 3.0; // time discarded after match start (warming shaders/caches)
};

// Report header (run environment)
struct BenchmarkInfo {
    std::string game, gameVersion, commit, simVersion, determinismGroup;
    std::string cpu;
    rhi::GpuInfo gpu;
    u32 width = 0, height = 0;
    bool vsync = false;
    u64 seed = 0;
    std::string scene;
    f32 gpuBudgetMs[render::kGpuPassCount + 1] = {}; // pass order + last slot is headroom (mealkit.project gpuBudget*)
};

struct BenchmarkResult {
    f64 seconds = 0.0;
    u32 frames = 0;
    f64 frameMs = 0.0, p99FrameMs = 0.0, worstFrameMs = 0.0, fps = 0.0;
    bool hasSim = false;
    f64 simMsPerTick = 0.0;
    f64 drawCalls = 0.0, instances = 0.0; // averages
    bool hasGpuTime = false;
    f64 gpuMs = 0.0;
    f64 gpuPassMs[render::kGpuPassCount] = {};
    u64 processMemoryPeak = 0, gpuMemoryPeak = 0, gpuMemoryBudget = 0;
    // Main thread heap allocations in the render + UI part of measured frames (debug/AllocCount.h; dev game exes only)
    bool hasAllocations = false;
    f64 allocationsPerFrame = 0.0;
    u64 allocationsMax = 0, allocationFrames = 0; // most in one frame, frames with any
};

class Benchmark {
public:
    explicit Benchmark(const BenchmarkConfig& config) : m_config(config) {}

    // Once per frame. running: the match is running (time before that and warmup isn't counted).
    // sample: the batch if a FrameStats period ended this frame, otherwise nullptr. true on the frame measurement finishes.
    // allocations: this frame's render + UI heap allocations, or -1 when not counted
    bool frame(f64 dt, bool running, const FrameSample* sample, i64 allocations = -1);
    bool done() const { return m_done; }
    BenchmarkResult result() const;

    // Report text (benchmark.txt). Passes over budget are marked OVER
    static std::string report(const BenchmarkInfo& info, const BenchmarkResult& result);
    // One line read by automated tests: "BENCHMARK frames=... avg=... p99=... gpu=..."
    static std::string summaryLine(const BenchmarkResult& result);

private:
    BenchmarkConfig m_config;
    f64 m_warm = 0.0, m_measured = 0.0;
    bool m_done = false;
    std::vector<f64> m_frameTimes;
    // Sum of period batches (weighted by period length)
    f64 m_sampleSeconds = 0.0, m_simSeconds = 0.0, m_gpuSeconds = 0.0;
    f64 m_sim = 0.0, m_draws = 0.0, m_instances = 0.0, m_gpu = 0.0;
    f64 m_gpuPass[render::kGpuPassCount] = {};
    u64 m_processPeak = 0, m_gpuPeak = 0, m_gpuBudget = 0;
    bool m_hasAllocs = false;
    u64 m_allocs = 0, m_allocMax = 0, m_allocFrames = 0;
};

} // namespace mealkit::game
