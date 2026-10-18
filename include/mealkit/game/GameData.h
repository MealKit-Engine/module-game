#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/scene/Scene.h"

#include <string>

namespace mealkit::asset {
class ModelCache;
}

// 매치 데이터 (씬, 모델, 튜닝): 게임 실행 파일과 에디터가 같은 방법으로 읽고, 같은 콘텐츠 해시를 만든다.
namespace mealkit::game {

inline constexpr const char* kDefaultTuningAsset = "assets/game.tuning";
inline constexpr const char* kViewTuningAsset = "assets/view.tuning";

// 튜닝 파일을 읽는다. path가 비어 있으면 assets/game.tuning. 파일이 없으면 선언의 기본값 그대로 (경고 로그).
// 읽은 파일 경로를 돌려준다 (없으면 빈 문자열).
std::string loadTuning(const reflect::TypeInfo& type, void* data, const std::string& path = {});

// 화면용 튜닝 파일 (assets/view.tuning, 게임의 ViewTuningType): 콘텐츠 해시에 넣지 않고, 파일이 바뀌면 다시 읽는다.
// 게임 실행 파일·참가 창·에디터가 하나씩 갖고 매 프레임 poll()한 뒤 GameContext::viewTuningData로 넘긴다.
// 에디터는 패널에서 고친 값을 바로 쓰고(set), 여러 명 플레이 중에는 참가 창이 읽는 사본 파일을 다시 쓴다.
class LiveTuning {
public:
    // path가 비어 있으면 assets/view.tuning. 파일이 없으면 선언의 기본값.
    void open(const reflect::TypeInfo& type, const std::string& path = {});
    void close();
    // 1초에 몇 번 파일 시각을 보고, 바뀌었으면 다시 읽는다. 다시 읽었으면 true.
    bool poll(f64 now);
    reflect::View view() const { return {m_value.type(), m_value.data()}; }
    const std::string& path() const { return m_path; }

private:
    void reload();

    reflect::Instance m_value;
    std::string m_path;
    unsigned long long m_writeTime = 0;
    f64 m_nextCheck = 0.0;
};

// 접속할 때 비교하는 해시: 씬(게임 결과에 영향을 주는 부분) + 모델 파일 + 튜닝 값
// + 프로젝트 설정(엔진 ProjectSettings와 게임 항목 중 MEALKIT_VIEW_FIELD가 아닌 것).
u64 contentHash(const scene::Scene& scene, asset::ModelCache& models, const scene::TypeList* gameTypes,
                const reflect::TypeInfo* tuningType, const void* tuning, reflect::View project = {},
                reflect::View gameProject = {});

} // namespace mealkit::game
