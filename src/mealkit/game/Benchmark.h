#pragma once

#include "mealkit/game/FrameStats.h"

#include <string>
#include <vector>

// 벤치마크 모드 (docs/ROADMAP.md 2단계 "벤치마크 모드"): 출시 exe의 `--benchmark [초]`.
// 고정 시드 + 봇 입력이라 같은 빌드·같은 데이터면 매번 같은 매치(카메라는 봇 캐릭터를 따라가 같은 길)를 돈다.
// 값은 game::FrameStats가 모으고, 이것은 측정 구간 동안 그 묶음과 프레임 시간을 쌓아 텍스트 보고서(benchmark.txt)를 쓴다.
//   Benchmark bench({30.0, 3.0});
//   매 프레임: if (bench.frame(dt, matchRunning, perf.update(...) ? &perf.sample() : nullptr)) { bench.write(...); 끝 }
namespace mealkit::game {

struct BenchmarkConfig {
    f64 seconds = 30.0;      // 측정 시간
    f64 warmupSeconds = 3.0; // 매치 시작 뒤 버리는 시간 (셰이더·캐시 데우기)
};

// 보고서 머리 (실행 환경)
struct BenchmarkInfo {
    std::string game, gameVersion, commit, simVersion, determinismGroup;
    std::string cpu;
    rhi::GpuInfo gpu;
    u32 width = 0, height = 0;
    bool vsync = false;
    u64 seed = 0;
    std::string scene;
    f32 gpuBudgetMs[render::kGpuPassCount + 1] = {}; // 패스 순서 + 마지막 칸은 여유 (mealkit.project gpuBudget*)
};

struct BenchmarkResult {
    f64 seconds = 0.0;
    u32 frames = 0;
    f64 frameMs = 0.0, p99FrameMs = 0.0, worstFrameMs = 0.0, fps = 0.0;
    bool hasSim = false;
    f64 simMsPerTick = 0.0;
    f64 drawCalls = 0.0, instances = 0.0; // 평균
    bool hasGpuTime = false;
    f64 gpuMs = 0.0;
    f64 gpuPassMs[render::kGpuPassCount] = {};
    u64 processMemoryPeak = 0, gpuMemoryPeak = 0, gpuMemoryBudget = 0;
};

class Benchmark {
public:
    explicit Benchmark(const BenchmarkConfig& config) : m_config(config) {}

    // 매 프레임 한 번. running: 매치가 돌고 있다 (그 전과 데우기 시간은 세지 않는다).
    // sample: 이번 프레임에 FrameStats 주기가 끝났으면 그 묶음, 아니면 nullptr. 측정이 끝난 프레임에 true
    bool frame(f64 dt, bool running, const FrameSample* sample);
    bool done() const { return m_done; }
    BenchmarkResult result() const;

    // 보고서 글자 (benchmark.txt). 예산을 넘는 패스는 OVER
    static std::string report(const BenchmarkInfo& info, const BenchmarkResult& result);
    // 자동 시험이 읽는 한 줄: "BENCHMARK frames=... avg=... p99=... gpu=..."
    static std::string summaryLine(const BenchmarkResult& result);

private:
    BenchmarkConfig m_config;
    f64 m_warm = 0.0, m_measured = 0.0;
    bool m_done = false;
    std::vector<f64> m_frameTimes;
    // 주기 묶음의 합 (주기 길이로 가중)
    f64 m_sampleSeconds = 0.0, m_simSeconds = 0.0, m_gpuSeconds = 0.0;
    f64 m_sim = 0.0, m_draws = 0.0, m_instances = 0.0, m_gpu = 0.0;
    f64 m_gpuPass[render::kGpuPassCount] = {};
    u64 m_processPeak = 0, m_gpuPeak = 0, m_gpuBudget = 0;
};

} // namespace mealkit::game
