#pragma once

#include "mealkit/core/Types.h"

#include <string>
#include <utility>
#include <vector>

// 게임 실행 파일의 명령줄 (출시 exe, --headless, <게임>_headless): 파서와 --help가 한 벌이다.
//   매치 옵션(MatchOptions): 방 만들기·참가·인원·시드·자동 시험 출력. 헤드리스와 창 있는 exe가 같은 값을 쓰고,
//   화면 흐름의 메뉴(방 만들기·참가)도 같은 구조체를 코드에서 채워 세션을 연다.
//   창 옵션(WindowOptions): 창·렌더러·화면 쪽 시험. 헤드리스는 받고 무시한다 (같은 명령줄로 둘 다 돌릴 수 있게).
namespace mealkit::game {

struct MatchOptions {
    enum class Mode { Offline, Host, Join, SteamHost, SteamJoin } mode = Mode::Offline;
    u16 port = 0; // 0 = 게임 defaults() (보통 mealkit.project의 game.defaultPort)
    std::string joinAddress;
    u64 steamLobby = 0;     // SteamJoin: 로비 id
    bool loopbackOnly = false; // 127.0.0.1에만 (같은 PC 시험, 방화벽 창이 뜨지 않는다)
    u32 maxPlayers = 0;     // 0 = 게임 defaults()
    u32 autoStart = 0;      // 호스트: 이 인원이 모이면 시작 (헤드리스는 0 = maxPlayers)
    u32 matchData = 0;      // 게임이 정하는 매치 설정값 (호스트가 정해 모두에게 보낸다. sandbox: 상자 수, 0 = 씬)
    u32 inputDelay = 3;     // 틱
    u64 seed = 0;           // 0 = 현재 시각
    u32 physicsThreads = 0; // 0 = 자동
    u32 exitTick = 0;       // 이 틱을 실행하면 FINAL을 쓰고 끝 (헤드리스는 재생이 아니면 필수)
    bool shadow = false;    // 그림자 시뮬레이션
    bool anyBuild = false;  // 호스트: 다른 빌드의 피어도 받는다 (컴파일러 비교 시험)
    std::string scenePath, tuningPath;
    std::string hashLogPath, recordInputsPath;
    std::string replayInputsPath; // 헤드리스 전용
};
using HeadlessOptions = MatchOptions; // 예전 이름 (게임 헤드리스 클래스의 defaults)

struct WindowOptions {
    bool vsync = true;
    f64 fpsCap = 0.0;
    u32 width = 1280, height = 720;
    bool windowGiven = false; // --size/--vsync/--fps를 줬다: 이번 실행은 플레이어 설정의 화면 항목보다 명령줄
    bool debugLayer = false;
    bool bot = false;        // 로컬 플레이어를 스크립트 입력으로 (헤드리스는 항상)
    bool rewindTest = false; // 끝날 때 기록을 되감아 다시 돌려 보고 결과를 출력
    std::string capturePath;
    u32 captureTick = 0;      // 0이면 --frames의 마지막 프레임 (메뉴 화면 캡처)
    u32 frames = 0;           // 이만큼 그리고 끝 (매치 밖 화면 시험, 에디터의 --frames와 같다)
    std::vector<std::pair<u32, std::string>> uiPresses; // --ui-press: 이 프레임에 이 이름의 위젯을 누른다 (화면 흐름 시험)
    bool systemInfo = false;   // CPU·GPU 정보만 출력하고 끝
    bool mute = false;         // 소리 끄기 (자동 시험에서 여러 창을 띄울 때)
    bool debugUi = false;      // 디버그 UI(F1)를 켠 채로 시작 (개발 빌드)
    bool openSettings = false; // 설정 화면을 연 채로 시작
    std::string language;      // 게임 글자 언어 (비어 있으면 mealkit.project의 language)
};

// window가 nullptr이면 헤드리스: 창 옵션은 받고 무시하고, Steam은 받지 않는다. 모르는 옵션·--help면 false.
bool parseGameArgs(int argc, char** argv, MatchOptions& match, WindowOptions* window);
// --help 출력 (name: 게임 이름, 헤드리스면 nullptr)
void printGameUsage(const char* name, bool headless);

} // namespace mealkit::game
