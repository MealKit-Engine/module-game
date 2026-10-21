#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/render/Renderer.h"

#include <string>
#include <vector>

namespace mealkit {
namespace net {
class LockstepSession;
}
namespace audio {
class Engine;
}
} // namespace mealkit

// 성능 숫자 모으기 (docs/CODING_MODEL.md "성능 숫자 모으기와 보여 주기"). 측정은 각 하위 시스템의 stats()가 하고,
// 이것은 보고 주기마다 평균을 내 한 묶음(FrameSample)과 글자로 만든다. 창 제목·F1 오버레이·perf 로그·에디터 상태 표시줄이 같이 쓴다.
//   game::FrameStats perf;
//   if (perf.update(dt, {&renderer, &session, &audio, &game})) window.setTitle(perf.line().c_str());
// 함수 안 시간 재기는 Tracy 마커 MEALKIT_ZONE (mealkit/core/Profile.h).
namespace mealkit::game {

class GameInstance;

// 출처: 없는 것(nullptr)은 그 항목을 비운다 (편집 모드의 세션, 헤드리스의 렌더러 등)
struct FrameSources {
    const render::Renderer* renderer = nullptr;
    const net::LockstepSession* session = nullptr;
    const audio::Engine* audio = nullptr;
    GameInstance* game = nullptr; // takeSimMsPerTick은 읽으면 평균을 비우므로 FrameStats만 부른다
};

// 보고 주기 하나의 값
struct FrameSample {
    f64 seconds = 0.0; // 이 묶음이 덮는 시간
    u32 frames = 0;
    f64 frameMs = 0.0;      // 평균 프레임 시간
    f64 worstFrameMs = 0.0; // 주기 안에서 가장 긴 프레임
    f64 p99FrameMs = 0.0;   // 1% 최악 프레임: 이보다 긴 프레임이 1% (frameMsPercentile 99)
    f64 fps = 0.0;

    bool hasGame = false;
    f64 simMsPerTick = 0.0;
    u32 tick = 0; // 다음에 실행할 틱

    bool hasSession = false;
    u32 localPlayer = 0, playerCount = 0;
    f32 rttMs = 0.0f;
    u32 sentBytesPerSec = 0, recvBytesPerSec = 0;
    bool desynced = false;

    bool hasRenderer = false;
    u32 drawCalls = 0, instances = 0;
    bool vsync = false;
    bool hasGpuTime = false; // 타임스탬프 쿼리 결과가 있다 (render::RenderStats::gpuValid)
    f64 gpuMs = 0.0;         // 평균 GPU 프레임 시간
    f64 gpuPassMs[render::kGpuPassCount] = {};
    u64 gpuMemoryUsed = 0, gpuMemoryBudget = 0; // 바이트 (모르면 0)

    u64 processMemory = 0, processMemoryPeak = 0; // 바이트 (platform processMemory, 렌더러가 있을 때)

    bool hasAudio = false;
    u32 realVoices = 0, virtualVoices = 0;
    f64 audioMs = 0.0;
};

// 프레임 시간(초) 목록의 백분위수 (ms). 목록을 정렬한다. 비면 0
f64 frameMsPercentile(std::vector<f64>& frameSeconds, f64 percent);

class FrameStats {
public:
    static constexpr f64 kDefaultPeriod = 0.25; // 초 (창 제목 4Hz)

    explicit FrameStats(f64 period = kDefaultPeriod) : m_period(period) {}

    // 매 프레임 한 번 (dt는 초). 보고 주기가 끝난 프레임에 출처에서 값을 모아 sample()을 바꾸고 true.
    bool update(f64 dt, const FrameSources& from);
    // 마지막으로 끝난 주기 (첫 주기 전에는 모두 0)
    const FrameSample& sample() const { return m_sample; }
    // "60 fps 16.67 ms (worst 18.2) | sim 0.42 ms/tick | ..." (창 제목, 로그). 성능 숫자는 개발용이라 영어 그대로
    std::string line() const;
    // line()의 항목을 한 줄씩 (F1 오버레이)
    std::string lines() const;

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
