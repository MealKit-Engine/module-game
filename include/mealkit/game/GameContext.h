#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/render/Renderer.h"

namespace mealkit {
class Window;
namespace asset {
class ModelCache;
}
namespace net {
class LockstepSession;
}
} // namespace mealkit

// 게임의 화면 쪽(app/, view/)이 쓰는 엔진 서비스 묶음 (docs/CODING_MODEL.md "엔진 기능에 어떻게 접근하나").
// 엔진(출시 exe의 main, 범용 에디터)이 만들어 GameInstance의 함수마다 넘긴다. 게임은 이것을 보관하지 않는다.
// sim/은 이것을 받지 않는다 (World&만). 소리·게임 UI·디버그 UI가 생기면 여기에 더한다 (그때 kApiVersion을 올린다).
namespace mealkit::game {

struct GameContext {
    Window& window; // 입력: window.input()
    render::Renderer& renderer;
    asset::ModelCache& models;
    net::LockstepSession& session;

    // 엔진이 매 프레임 정하는 값
    render::Viewport viewport{}; // 게임 화면 영역 (에디터에서는 씬 뷰)
    bool keyboardAllowed = true; // 에디터: 게임 뷰가 키보드를 받을 때만
    bool mouseAllowed = true;
};

} // namespace mealkit::game
