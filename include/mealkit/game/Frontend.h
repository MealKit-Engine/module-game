#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/game/GameContext.h"
#include "mealkit/game/GameData.h"
#include "mealkit/game/MatchService.h"
#include "mealkit/input/Actions.h"
#include "mealkit/loc/Strings.h"
#include "mealkit/platform/Gamepad.h"
#include "mealkit/settings/Settings.h"
#include "mealkit/ui/Ui.h"

#include <string>

namespace mealkit {
class FrameLimiter;
class Input;
class Window;
} // namespace mealkit

// 창 있는 게임의 화면 쪽 서비스 묶음 (docs/CODING_MODEL.md "창 있는 게임을 돌리는 프레임 루프").
// 출시 exe(game::runGame)·에디터 플레이·참가 창이 하나씩 갖고 같은 순서로 돌린다:
//   front.init(config);                                  // 바인딩·문자열·UI·설정 읽기 (다시 부르면 다시 읽는다)
//   GameContext ctx = front.context(window, renderer, models, match);   // match: 세션을 가진 MatchService
//   매 프레임: ctx.viewport·허용 플래그를 정하고 → front.update(now, dt, input, padAllowed, ctx) → game.update(ctx, dt)
//             → game.buildFrame → front.afterBuildFrame(frame) → 3D 다음 front.renderUi(renderer)
// 소리·연출은 주인(실행 파일, 에디터)이 만들고 갱신한다. 에디터는 [소리]·[연출] 미리보기와 같이 쓴다.
namespace mealkit::game {

// 표준 자막 화면 (assets/ui/subtitles.ui, 있으면): 자막이 있는 소리가 나면 화면 아래에 (플레이어 설정 gameplay.subtitles)
MEALKIT_STRUCT(SubtitleLineVm) {
    MEALKIT_FIELD(std::string, text, {}, "Subtitle text");
};
MEALKIT_VIEWMODEL(SubtitlesVm) {
    MEALKIT_LIST(SubtitleLineVm, lines, 4, "Subtitle lines");
};
inline constexpr const char* kSubtitleScreen = "ui/subtitles.ui"; // assets 기준
inline constexpr i32 kSubtitleLayer = 5;                         // HUD 위, 메뉴 아래

struct FrontendConfig {
    render::Renderer* renderer = nullptr; // 게임 UI가 그릴 곳
    audio::Engine* audio = nullptr;
    feedback::Player* feedback = nullptr;
    // 플레이어 설정의 화면 항목(창 모드·크기·수직동기·프레임 제한)을 적용할 곳. 에디터·자동 실행은 nullptr
    Window* displayWindow = nullptr;
    FrameLimiter* limiter = nullptr;
    std::string language;        // 게임 글자 언어 (비어 있으면 projectLanguage)
    std::string projectLanguage; // mealkit.project의 language (없는 키를 찾는 기본 언어)
    std::string bindingsUserFile; // 플레이어가 바꾼 키 파일. 비어 있으면 프로젝트 바인딩만
    std::string settingsFile;     // settings.ini. 비어 있으면 기본값으로 돌고 저장하지 않는다
    // 화면용 튜닝 (게임의 ViewTuningType). nullptr이면 주인이 ctx.viewTuningData를 직접 넘긴다 (에디터)
    const reflect::TypeInfo* viewTuningType = nullptr;
    std::string viewTuningPath; // 비어 있으면 assets/view.tuning
};

class Frontend {
public:
    Frontend() = default;
    Frontend(const Frontend&) = delete;
    Frontend& operator=(const Frontend&) = delete;

    void init(const FrontendConfig& config);
    // 서비스를 가리키는 GameContext (창 크기·허용 플래그는 주인이 매 프레임 정한다)
    GameContext context(Window& window, render::Renderer& renderer, asset::ModelCache& models, MatchService& match);

    // 게임 update 전, 매 프레임 한 번. UI가 마우스·키보드를 가져가면 ctx의 허용 플래그를 끈다.
    // padAllowed: 게임패드 입력을 넘길지 (출시 exe는 창 포커스, 에디터는 게임 뷰가 키보드를 받을 때)
    void update(f64 now, f64 dt, const Input& input, bool padAllowed, GameContext& ctx);
    // game.buildFrame 다음: 월드 위 UI의 카메라, 소리 듣는 위치, 연출의 화면 색(frame의 tint에 더한다)
    void afterBuildFrame(render::FrameParams& frame);
    // 3D 다음, 디버그·에디터 UI 전
    void renderUi(render::Renderer& renderer);

    input::Actions& actions() { return m_actions; }
    loc::Strings& strings() { return m_strings; }
    ui::System& ui() { return m_ui; }
    settings::Service& settings() { return m_settings; }

private:
    FrontendConfig m_config;
    input::Actions m_actions;
    Gamepads m_pads;
    loc::Strings m_strings;
    ui::System m_ui;
    settings::Service m_settings;
    LiveTuning m_viewTuning;
    ui::ScreenId m_subtitleScreen = ui::kNoScreen;
    SubtitlesVm m_subtitleVm;
    bool m_hasSubtitleScreen = false;
};

} // namespace mealkit::game
