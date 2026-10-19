#pragma once

#include "mealkit/core/Types.h"

// 이 게임 빌드의 정보 (ROADMAP 1단계 "버전 표시와 접속 거부 이유"): 창 제목·타이틀·로비에 보이고, 접속할 때 비교한다.
//   gameVersion  mealkit.project의 version (사람이 정한다)
//   commit       빌드한 프로젝트 폴더의 git 커밋 (고친 파일이 있으면 끝에 +)
//   simVersion   게임 버전 + 시뮬레이션에 닿는 소스(게임 sim/, 헤드리스 파일, 엔진 코어 폴더, Jolt 버전)의 해시.
//                실행 파일이 달라도(다른 OS·컴파일러) 소스가 같으면 같은 값이라, 실행 파일 대신 이것을 비교한다 (크로스플레이 준비)
// 값은 빌드할 때 cmake/BuildInfo.cmake가 만든 mealkit_build_info.h에 있고, 게임 타깃의 코드(엔진 매크로 MEALKIT_GAME_MAIN,
// MEALKIT_GAME_MODULE, MEALKIT_HEADLESS_GAME)만 그 헤더를 본다. 엔진만으로 빌드한 코드에서는 비어 있다 (simVersion 0 = 실행 파일 id로 대신).
namespace mealkit::game {

struct BuildInfo {
    u64 simVersion = 0;
    const char* gameVersion = "";
    const char* commit = "";
};

} // namespace mealkit::game

#if __has_include("mealkit_build_info.h")
#include "mealkit_build_info.h"
#define MEALKIT_THIS_BUILD                                                                                             \
    ::mealkit::game::BuildInfo { MEALKIT_BUILD_SIM_VERSION, MEALKIT_BUILD_GAME_VERSION, MEALKIT_BUILD_COMMIT }
#else
#define MEALKIT_THIS_BUILD                                                                                             \
    ::mealkit::game::BuildInfo {}
#endif
