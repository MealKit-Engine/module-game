#include "mealkit/game/Benchmark.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <format>

namespace mealkit::game {
namespace {

void appendf(std::string& out, const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n > 0) out.append(buf, static_cast<size_t>(std::min(n, static_cast<int>(sizeof(buf) - 1))));
}

f64 mb(u64 bytes) { return static_cast<f64>(bytes) / (1024.0 * 1024.0); }

} // namespace

bool Benchmark::frame(f64 dt, bool running, const FrameSample* sample) {
    if (m_done || !running) return false;
    if (m_warm < m_config.warmupSeconds) { // periods ending during warmup are also dropped (a period overlapping warmup would be half mixed)
        m_warm += dt;
        return false;
    }
    m_frameTimes.push_back(dt);
    m_measured += dt;
    if (sample) {
        const f64 w = sample->seconds;
        m_sampleSeconds += w;
        m_draws += sample->drawCalls * w;
        m_instances += sample->instances * w;
        if (sample->hasGame) {
            m_sim += sample->simMsPerTick * w;
            m_simSeconds += w;
        }
        if (sample->hasGpuTime) {
            m_gpu += sample->gpuMs * w;
            for (u32 i = 0; i < render::kGpuPassCount; ++i) m_gpuPass[i] += sample->gpuPassMs[i] * w;
            m_gpuSeconds += w;
        }
        m_processPeak = std::max(m_processPeak, sample->processMemoryPeak);
        m_gpuPeak = std::max(m_gpuPeak, sample->gpuMemoryUsed);
        m_gpuBudget = std::max(m_gpuBudget, sample->gpuMemoryBudget);
    }
    if (m_measured >= m_config.seconds) m_done = true;
    return m_done;
}

BenchmarkResult Benchmark::result() const {
    BenchmarkResult r;
    r.seconds = m_measured;
    r.frames = static_cast<u32>(m_frameTimes.size());
    if (r.frames) {
        r.frameMs = m_measured / r.frames * 1000.0;
        r.fps = r.frameMs > 0.0 ? 1000.0 / r.frameMs : 0.0;
        std::vector<f64> sorted = m_frameTimes;
        r.p99FrameMs = frameMsPercentile(sorted, 99.0);
        r.worstFrameMs = sorted.back() * 1000.0;
    }
    if (m_sampleSeconds > 0.0) {
        r.drawCalls = m_draws / m_sampleSeconds;
        r.instances = m_instances / m_sampleSeconds;
    }
    if (m_simSeconds > 0.0) {
        r.hasSim = true;
        r.simMsPerTick = m_sim / m_simSeconds;
    }
    if (m_gpuSeconds > 0.0) {
        r.hasGpuTime = true;
        r.gpuMs = m_gpu / m_gpuSeconds;
        for (u32 i = 0; i < render::kGpuPassCount; ++i) r.gpuPassMs[i] = m_gpuPass[i] / m_gpuSeconds;
    }
    r.processMemoryPeak = m_processPeak;
    r.gpuMemoryPeak = m_gpuPeak;
    r.gpuMemoryBudget = m_gpuBudget;
    return r;
}

std::string Benchmark::report(const BenchmarkInfo& info, const BenchmarkResult& r) {
    std::string out;
    const std::string date =
        std::format("{:%Y-%m-%d %H:%M} UTC", std::chrono::floor<std::chrono::minutes>(std::chrono::system_clock::now()));
    // The report is for developers, so English (same as the window title and perf log)
    appendf(out, "MealKit benchmark (%s)\n\n", date.c_str());
    appendf(out, "game        %s %s (commit %s, simulation %s, %s)\n", info.game.c_str(), info.gameVersion.c_str(),
            info.commit.c_str(), info.simVersion.c_str(), info.determinismGroup.c_str());
    appendf(out, "cpu         %s\n", info.cpu.c_str());
    appendf(out, "gpu         %s | driver %s | %s | %s\n", info.gpu.adapter.c_str(),
            info.gpu.driverVersion.empty() ? "?" : info.gpu.driverVersion.c_str(), info.gpu.api.c_str(),
            info.gpu.uma ? "unified memory (UMA)" : "dedicated memory");
    appendf(out, "screen      %ux%u, vsync %s\n", info.width, info.height, info.vsync ? "on" : "off");
    appendf(out, "run         scene %s, seed %llu, bot input, %.1f s measured (%u frames)\n\n", info.scene.c_str(),
            static_cast<unsigned long long>(info.seed), r.seconds, r.frames);

    appendf(out, "frame       %.2f ms avg (%.0f fps) | 1%% worst %.2f ms | worst %.2f ms\n", r.frameMs, r.fps, r.p99FrameMs,
            r.worstFrameMs);
    if (r.hasSim) appendf(out, "simulation  %.3f ms/tick\n", r.simMsPerTick);
    appendf(out, "draws       %.0f draw calls, %.0f instances (avg)\n", r.drawCalls, r.instances);
    appendf(out, "memory      process %.0f MB peak | gpu %.0f MB peak of %.0f MB budget\n\n", mb(r.processMemoryPeak),
            mb(r.gpuMemoryPeak), mb(r.gpuMemoryBudget));

    if (!r.hasGpuTime) {
        out += "gpu time    not available (no timestamp queries)\n";
        return out;
    }
    // GPU budget table: pass ms and the budget from mealkit.project. Passes not measured (features not yet present) are 0
    f32 total = 0.0f;
    for (f32 b : info.gpuBudgetMs) total += b;
    appendf(out, "gpu time    %.2f ms frame (budget %.2f ms)\n", r.gpuMs, total);
    appendf(out, "  %-10s %8s %8s\n", "pass", "ms", "budget");
    f64 passes = 0.0;
    for (u32 i = 0; i < render::kGpuPassCount; ++i) {
        const f64 ms = r.gpuPassMs[i];
        const f32 budget = info.gpuBudgetMs[i];
        passes += ms;
        appendf(out, "  %-10s %8.2f %8.2f%s\n", render::gpuPassName(static_cast<render::GpuPass>(i)), ms, budget,
                ms > budget ? "  OVER" : "");
    }
    const f64 other = std::max(0.0, r.gpuMs - passes); // outside passes (barriers, upload copies, present prep)
    const f32 spare = info.gpuBudgetMs[render::kGpuPassCount];
    appendf(out, "  %-10s %8.2f %8.2f%s\n", "other", other, spare, other > spare ? "  OVER" : "");
    appendf(out, "  %-10s %8.2f %8.2f%s\n", "total", r.gpuMs, total, r.gpuMs > total ? "  OVER" : "");
    return out;
}

std::string Benchmark::summaryLine(const BenchmarkResult& r) {
    std::string out;
    appendf(out, "BENCHMARK frames=%u avg=%.2fms p99=%.2fms worst=%.2fms sim=%.3fms gpu=%.2fms mem=%.0fMB gpumem=%.0fMB",
            r.frames, r.frameMs, r.p99FrameMs, r.worstFrameMs, r.simMsPerTick, r.hasGpuTime ? r.gpuMs : 0.0,
            mb(r.processMemoryPeak), mb(r.gpuMemoryPeak));
    return out;
}

} // namespace mealkit::game
