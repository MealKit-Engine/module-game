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

// 튜닝 파일을 읽는다. path가 비어 있으면 assets/game.tuning. 파일이 없으면 선언의 기본값 그대로 (경고 로그).
// 읽은 파일 경로를 돌려준다 (없으면 빈 문자열).
std::string loadTuning(const reflect::TypeInfo& type, void* data, const std::string& path = {});

// 접속할 때 비교하는 해시: 씬(게임 결과에 영향을 주는 부분) + 모델 파일 + 튜닝 값
// + 프로젝트 설정(엔진 ProjectSettings와 게임 항목 중 MEALKIT_VIEW_FIELD가 아닌 것).
u64 contentHash(const scene::Scene& scene, asset::ModelCache& models, const scene::TypeList* gameTypes,
                const reflect::TypeInfo* tuningType, const void* tuning, reflect::View project = {},
                reflect::View gameProject = {});

} // namespace mealkit::game
