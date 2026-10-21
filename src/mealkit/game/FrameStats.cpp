#include "mealkit/game/FrameStats.h"

#include "mealkit/audio/Audio.h"
#include "mealkit/game/GameInstance.h"
#include "mealkit/net/Lockstep.h"
#include "mealkit/platform/System.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mealkit::game {
namespace {

std::vector<std::string> items(const FrameSample& s) {
    std::vector<std::string> out;
    char buf[160];
    const auto add = [&](int n) {
        if (n > 0) out.emplace_back(buf);
    };
    add(std::snprintf(buf, sizeof(buf), "%.0f fps %.2f ms (worst %.1f)", s.fps, s.frameMs, s.worstFrameMs));
    if (s.hasGpuTime) add(std::snprintf(buf, sizeof(buf), "gpu %.2f ms", s.gpuMs));
    if (s.hasGame) {
        add(std::snprintf(buf, sizeof(buf), "sim %.2f ms/tick", s.simMsPerTick));
        add(std::snprintf(buf, sizeof(buf), "tick %u", s.tick));
    }
    if (s.hasSession) {
        add(std::snprintf(buf, sizeof(buf), "P%u of %u", s.localPlayer + 1, s.playerCount));
        add(std::snprintf(buf, sizeof(buf), "rtt %.0f ms", s.rttMs));
        add(std::snprintf(buf, sizeof(buf), "net %.1f/%.1f KB/s", s.sentBytesPerSec / 1024.0, s.recvBytesPerSec / 1024.0));
    }
    if (s.hasRenderer) add(std::snprintf(buf, sizeof(buf), "%u draws %u inst (%u culled)", s.drawCalls, s.instances, s.culled));
    if (s.processMemory) add(std::snprintf(buf, sizeof(buf), "mem %.0f MB", s.processMemory / 1048576.0));
    if (s.gpuMemoryBudget)
        add(std::snprintf(buf, sizeof(buf), "vram %.0f/%.0f MB", s.gpuMemoryUsed / 1048576.0, s.gpuMemoryBudget / 1048576.0));
    if (s.hasAudio)
        add(std::snprintf(buf, sizeof(buf), "audio %u+%u voices %.2f ms", s.realVoices, s.virtualVoices, s.audioMs));
    if (s.hasSession) out.emplace_back(s.desynced ? "DESYNC" : "in sync");
    if (s.hasRenderer && s.vsync) out.emplace_back("vsync");
    return out;
}

std::string join(const std::vector<std::string>& parts, const char* sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

} // namespace

f64 frameMsPercentile(std::vector<f64>& frameSeconds, f64 percent) {
    if (frameSeconds.empty()) return 0.0;
    std::sort(frameSeconds.begin(), frameSeconds.end());
    // 가장 가까운 순위 (nearest-rank): 100프레임에 99%면 99번째
    const f64 rank = std::ceil(percent / 100.0 * static_cast<f64>(frameSeconds.size()));
    const size_t i = static_cast<size_t>(std::clamp(rank, 1.0, static_cast<f64>(frameSeconds.size()))) - 1;
    return frameSeconds[i] * 1000.0;
}

bool FrameStats::update(f64 dt, const FrameSources& from) {
    m_elapsed += dt;
    ++m_frames;
    if (dt > m_worst) m_worst = dt;
    m_frameTimes.push_back(dt);
    if (from.renderer && from.renderer->stats().gpuValid) {
        const render::RenderStats& rs = from.renderer->stats();
        m_gpuMs += rs.gpuFrameMs;
        for (u32 i = 0; i < render::kGpuPassCount; ++i) m_gpuPassMs[i] += rs.gpuPassMs[i];
        ++m_gpuFrames;
    }
    if (m_elapsed < m_period) return false;

    FrameSample s;
    s.seconds = m_elapsed;
    s.frames = m_frames;
    s.frameMs = m_elapsed / m_frames * 1000.0;
    s.worstFrameMs = m_worst * 1000.0;
    s.p99FrameMs = frameMsPercentile(m_frameTimes, 99.0);
    s.fps = s.frameMs > 0.0 ? 1000.0 / s.frameMs : 0.0;
    if (from.game) {
        s.hasGame = true;
        s.simMsPerTick = from.game->takeSimMsPerTick();
        s.tick = from.game->nextTick();
    }
    if (from.session) {
        const net::SessionStats& st = from.session->stats();
        s.hasSession = true;
        s.localPlayer = from.session->localPlayer();
        s.playerCount = from.session->playerCount();
        s.rttMs = st.rttMs;
        s.sentBytesPerSec = st.sentBytesPerSec;
        s.recvBytesPerSec = st.recvBytesPerSec;
        s.desynced = from.session->desynced();
    }
    if (from.renderer) {
        s.hasRenderer = true;
        s.drawCalls = from.renderer->stats().drawCalls;
        s.instances = from.renderer->stats().instances;
        s.culled = from.renderer->stats().culled;
        s.vsync = from.renderer->vsync();
        if (m_gpuFrames) {
            s.hasGpuTime = true;
            s.gpuMs = m_gpuMs / m_gpuFrames;
            for (u32 i = 0; i < render::kGpuPassCount; ++i) s.gpuPassMs[i] = m_gpuPassMs[i] / m_gpuFrames;
        }
        const rhi::GpuMemory vram = from.renderer->gpuMemory();
        s.gpuMemoryUsed = vram.used;
        s.gpuMemoryBudget = vram.budget;
        const ProcessMemory mem = processMemory(); // 창 있는 실행에서만 (렌더러가 출처일 때)
        s.processMemory = mem.resident;
        s.processMemoryPeak = mem.peakResident;
    }
    if (from.audio) {
        const audio::Stats& st = from.audio->stats();
        s.hasAudio = true;
        s.realVoices = st.realVoices;
        s.virtualVoices = st.virtualVoices;
        s.audioMs = st.updateMs;
    }
    m_sample = s;
    m_elapsed = m_worst = 0.0;
    m_frames = 0;
    m_frameTimes.clear();
    m_gpuMs = 0.0;
    std::fill(std::begin(m_gpuPassMs), std::end(m_gpuPassMs), 0.0);
    m_gpuFrames = 0;
    return true;
}

std::string FrameStats::line() const { return join(items(m_sample), " | "); }

std::string FrameStats::lines() const { return join(items(m_sample), "\n"); }

} // namespace mealkit::game
