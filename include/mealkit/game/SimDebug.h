#pragma once

#include "mealkit/core/Types.h"

#include <string>
#include <vector>

// 에디터가 플레이 중인 시뮬레이션을 조작하는 창구 (GameInstance::debug()). 구현은 game::Simulation<World>.
namespace mealkit::game {

struct Timeline {
    bool recording = false; // 입력 기록과 스냅샷을 남기는 중 (되감기 가능)
    bool paused = false;
    bool viewingPast = false; // 기록을 다시 돌려 과거 틱을 보는 중 (계속하면 현재로 돌아간다)
    u32 first = 0;            // 되감을 수 있는 가장 이른 틱
    u32 live = 0;             // 실제로 진행한 틱 (다음에 실행할 틱)
    u32 view = 0;             // 화면에 보이는 틱
    size_t memoryBytes = 0;   // 스냅샷·입력 기록이 쓰는 메모리
};

struct ShadowStatus {
    bool enabled = false;
    bool desynced = false;
    u32 tick = 0;                  // 처음 갈라진 틱
    std::vector<std::string> diff; // "경로: 진짜 -> 그림자" (상태 필드). 비어 있으면 물리 상태만 다르다
};

class SimDebug {
public:
    virtual ~SimDebug() = default;

    virtual Timeline timeline() const = 0;
    virtual void setPaused(bool paused) = 0; // 계속하면 과거를 보던 중이어도 현재 틱으로 돌아간다
    virtual void stepOnce() = 0;             // 일시정지 중 한 틱 앞으로 (과거를 보는 중이면 기록으로)
    virtual bool seek(u32 tick) = 0;         // 일시정지 중 기록된 범위의 틱으로 간다
    virtual const ShadowStatus& shadow() const = 0;
    // 되감기로 다시 돌린 결과가 기록과 다를 때의 설명 (선언하지 않은 상태, 결정론 위반). 없으면 빈 문자열
    virtual const std::string& replayError() const = 0;
};

} // namespace mealkit::game
