#pragma once

#include "mealkit/game/BuildInfo.h"
#include "mealkit/game/GameInstance.h"
#include "mealkit/game/Headless.h"
#include "mealkit/platform/Window.h"

#include <string_view>
#include <type_traits>

// 출시 exe의 main (docs/CODING_MODEL.md "창 있는 게임을 돌리는 프레임 루프"). 게임은 app/main.cpp에 한 줄만 쓴다:
//   MEALKIT_GAME_MAIN(sandbox::GameSession, sandbox::Headless)
// 명령줄(--help), 창·렌더러, 세션(혼자·호스트·참가·Steam 로비), 화면 쪽 서비스(game::Frontend), 디버그 UI(F1),
// 자동 시험 출력(FINAL, SHADOW, REWIND), 창 제목(문자열 키 game.title, title.*)은 엔진이 한다. --headless는 헤드리스 진입점으로 간다.
namespace mealkit::game {

struct GameMainSpec {
    const GameModule* module = nullptr;
    // (선택) 헤드리스 클래스의 defaults: 포트·인원 기본값 (gameProject는 GameModule::projectSettingsType의 값)
    void (*defaults)(MatchOptions& options, const void* gameProject) = nullptr;
    BuildInfo build; // 이 게임 빌드 (게임 버전·커밋·시뮬레이션 버전, 게임의 main.cpp에서 만든다)
};

// 출시 exe가 아닌 곳이 같은 프레임 루프를 돌릴 때 바꾸는 것 (에디터 여러 명 플레이의 참가 창, editor::runPlayer).
// 기본값은 출시 exe 그대로다.
struct GameRunOptions {
    const ProjectFile* project = nullptr; // 이미 읽은 프로젝트 설정. nullptr이면 loadMatchData가 찾는다
    std::string title;                    // 창 제목 앞부분. 비어 있으면 문자열 키 game.title + 게임 버전
    u32 captionColor = kDefaultCaptionColor;
    std::string viewTuningPath;       // 화면용 튜닝 파일. 비어 있으면 assets/view.tuning (바뀌면 다시 읽는다)
    bool playerFiles = true;          // false: 사용자 폴더의 설정·바꾼 키·세이브를 읽고 쓰지 않는다 (같은 PC의 여러 창)
    bool onlineService = true;        // false: 온라인 서비스(Steam)에 로그인하지 않는다
    bool exitWhenSessionEnds = false; // 세션이 끝나거나 게임이 매치를 나가면(메뉴) 종료한다
};

// 명령줄을 읽어 아래 runGame을 부른다 (--help, --system-info)
int runGame(int argc, char** argv, const GameMainSpec& spec);
// 창·렌더러·세션·Frontend·프레임 루프. 종료 코드 (0 정상, 2 디싱크, 3 시험 중 세션 끝, 4 되감기 실패, 5 그림자 디싱크)
int runGame(MatchOptions match, const WindowOptions& window, const GameMainSpec& spec, const GameRunOptions& run = {});

template <class GameClass, class HeadlessClass>
GameMainSpec gameMainSpec(const char* name) {
    GameMainSpec spec;
    spec.module = &gameModuleOf<GameClass>(name);
    spec.build = MEALKIT_THIS_BUILD;
    if constexpr (requires { typename HeadlessClass::ProjectSettingsType; }) {
        using Project = typename HeadlessClass::ProjectSettingsType;
        if constexpr (requires { typename GameClass::ProjectSettingsType; })
            static_assert(std::is_same_v<Project, typename GameClass::ProjectSettingsType>,
                          "GameClass and HeadlessClass must use the same ProjectSettingsType");
        if constexpr (requires(MatchOptions& o, const Project& p) { HeadlessClass::defaults(o, p); })
            spec.defaults = [](MatchOptions& o, const void* p) {
                HeadlessClass::defaults(o, *static_cast<const Project*>(p));
            };
    }
    return spec;
}

} // namespace mealkit::game

// 게임의 app/main.cpp에서 한 번 쓴다. MEALKIT_GAME_NAME은 mealkit_add_game()이 넘기는 프로젝트 이름(exe 이름)
#define MEALKIT_GAME_MAIN(GameClass, HeadlessClass)                                                                    \
    int main(int argc, char** argv) {                                                                                  \
        for (int i = 1; i < argc; ++i)                                                                                 \
            if (std::string_view(argv[i]) == "--headless") return mealkitHeadlessMain(argc, argv);                     \
        return ::mealkit::game::runGame(argc, argv,                                                                    \
                                        ::mealkit::game::gameMainSpec<GameClass, HeadlessClass>(MEALKIT_GAME_NAME));   \
    }
