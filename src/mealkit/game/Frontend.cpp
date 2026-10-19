#include "mealkit/game/Frontend.h"

#include "mealkit/audio/Audio.h"
#include "mealkit/feedback/Feedback.h"

namespace mealkit::game {

void Frontend::init(const FrontendConfig& config) {
    m_config = config;
    m_actions.setUserFile(config.bindingsUserFile);
    m_actions.load();
    m_strings.load(config.language.empty() ? config.projectLanguage : config.language, config.projectLanguage);
    m_ui.closeAll();
    m_ui.init(config.renderer, &m_strings);

    settings::Service::Targets t;
    t.window = config.displayWindow;
    t.renderer = config.displayWindow ? config.renderer : nullptr;
    t.limiter = config.displayWindow ? config.limiter : nullptr;
    t.audio = config.audio;
    t.strings = &m_strings;
    t.ui = &m_ui;
    t.actions = &m_actions;
    t.projectLanguage = config.language.empty() ? config.projectLanguage : config.language;
    m_settings.init(t, config.settingsFile);

    if (config.viewTuningType) m_viewTuning.open(*config.viewTuningType, config.viewTuningPath);
    else m_viewTuning.close();
}

GameContext Frontend::context(Window& window, render::Renderer& renderer, asset::ModelCache& models,
                              net::LockstepSession& session) {
    return GameContext{window, renderer, models, session, *m_config.audio, *m_config.feedback,
                       m_actions, m_strings, m_ui, m_settings};
}

void Frontend::update(f64 now, f64 dt, const Input& input, bool padAllowed, GameContext& ctx) {
    m_strings.update(dt);
    if (m_config.viewTuningType) {
        m_viewTuning.poll(now);
        ctx.viewTuningData = m_viewTuning.view();
    }
    m_pads.update(dt);
    // UI가 먼저 마우스·포커스를 가져간다 (UI 위의 클릭은 게임이 받지 않는다). 키보드·게임패드 조작은 지난 프레임의 ui.* 액션
    m_ui.update({&input, &m_actions, ctx.mouseAllowed, ctx.keyboardAllowed}, ctx.viewport, dt);
    if (m_ui.wantsMouse()) ctx.mouseAllowed = false;
    if (m_ui.wantsKeyboard()) ctx.keyboardAllowed = false;
    m_settings.update(&input, dt); // 설정 화면의 액션 (게임 update 전)
    m_actions.update(input, padAllowed ? m_pads.first() : PadState{}, ctx.keyboardAllowed, ctx.mouseAllowed, dt);
}

void Frontend::afterBuildFrame(render::FrameParams& frame) {
    m_ui.setCamera(frame.viewProj); // 월드 위 UI: buildFrame이 drawAt으로 넣은 것을 이 카메라로 화면에 놓는다
    m_config.audio->setListener(frame.cameraPos, frame.cameraForward);
    m_config.feedback->screenTint(frame.tintColor, frame.tintAmount);
}

void Frontend::renderUi(render::Renderer& renderer) { m_ui.render(renderer); }

} // namespace mealkit::game
