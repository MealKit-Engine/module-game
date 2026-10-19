#include "mealkit/game/FrameStats.h"

#include "mealkit/audio/Audio.h"
#include "mealkit/game/GameInstance.h"
#include "mealkit/net/Lockstep.h"
#include "mealkit/render/Renderer.h"

#include <cstdio>
#include <vector>

namespace mealkit::game {
namespace {

std::vector<std::string> items(const FrameSample& s) {
    std::vector<std::string> out;
    char buf[160];
    const auto add = [&](int n) {
        if (n > 0) out.emplace_back(buf);
    };
    add(std::snprintf(buf, sizeof(buf), "%.0f fps %.2f ms (worst %.1f)", s.fps, s.frameMs, s.worstFrameMs));
    if (s.hasGame) {
        add(std::snprintf(buf, sizeof(buf), "sim %.2f ms/tick", s.simMsPerTick));
        add(std::snprintf(buf, sizeof(buf), "tick %u", s.tick));
    }
    if (s.hasSession) {
        add(std::snprintf(buf, sizeof(buf), "P%u of %u", s.localPlayer + 1, s.playerCount));
        add(std::snprintf(buf, sizeof(buf), "rtt %.0f ms", s.rttMs));
        add(std::snprintf(buf, sizeof(buf), "net %.1f/%.1f KB/s", s.sentBytesPerSec / 1024.0, s.recvBytesPerSec / 1024.0));
    }
    if (s.hasRenderer) add(std::snprintf(buf, sizeof(buf), "%u draws %u inst", s.drawCalls, s.instances));
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

bool FrameStats::update(f64 dt, const FrameSources& from) {
    m_elapsed += dt;
    ++m_frames;
    if (dt > m_worst) m_worst = dt;
    if (m_elapsed < m_period) return false;

    FrameSample s;
    s.seconds = m_elapsed;
    s.frames = m_frames;
    s.frameMs = m_elapsed / m_frames * 1000.0;
    s.worstFrameMs = m_worst * 1000.0;
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
        s.vsync = from.renderer->vsync();
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
    return true;
}

std::string FrameStats::line() const { return join(items(m_sample), " | "); }

std::string FrameStats::lines() const { return join(items(m_sample), "\n"); }

} // namespace mealkit::game
