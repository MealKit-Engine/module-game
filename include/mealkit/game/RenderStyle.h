#pragma once

#include "mealkit/game/ProjectSettings.h"
#include "mealkit/render/Renderer.h"

#include <string>

// 프로젝트 설정의 스타일 프리셋(조명 모델, 툰 램프 그림)을 렌더러에 넘긴다 (docs/CODING_MODEL.md "셰이더 코드").
// 출시 exe·에디터가 프로젝트를 읽은 뒤 부르고, 에디터는 프로젝트 설정을 바꾸거나 램프 그림 파일이 바뀌면 다시 부른다.
namespace mealkit::game {

// 마지막으로 넘긴 값. 같으면 다시 읽지 않는다 (에디터가 0.5초마다 부른다)
struct RenderStyleState {
    bool applied = false;
    LightingModelSetting lightingModel = LightingModelSetting::Pbr;
    std::string toonRamp;
    unsigned long long rampTime = 0; // 램프 그림 파일 시각 (바뀌면 다시 읽는다)
};

// 바뀐 것이 있으면 넘기고 true. 램프 그림이 없거나 못 읽으면 경고하고 엔진 기본 3단계
bool applyRenderStyle(const ProjectSettings& settings, render::Renderer& renderer, RenderStyleState& state);

} // namespace mealkit::game
