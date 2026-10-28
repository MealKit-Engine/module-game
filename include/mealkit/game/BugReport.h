#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/game/BuildInfo.h"

#include <string>

// 버그 리포트 묶음: 플레이어·테스터가 키 하나(F9)로 "지금 이 판"을 개발자에게 넘긴다.
// 사용자 폴더 bugreports/<YYYY-MM-DD_HH-MM>.zip (압축 없는 zip, core/Zip.h):
//   replay.rec  그때까지의 입력 기록 (리플레이와 같은 형식, game/InputRecord.h)
//   info.txt    엔진·게임 버전, 커밋, 시뮬레이션 버전, 결정론 그룹, 콘텐츠 해시, 씬·튜닝 경로, 틱, CPU, 플레이어가 적은 한 줄
//   log.txt     최근 로그 (core/Log.h recentLines)
//   screenshot.png
// 에디터 [파일] > [리플레이 열기]가 zip을 열면 replay.rec를 꺼내 그 틱까지 재생하고 멈춘다 (기록이 거기서 끝난다).
// 결정론 시뮬레이션이라 입력만으로 그 판이 다시 만들어진다. 다른 빌드·데이터면 콘텐츠 해시로 알린다.
namespace mealkit::game {

inline constexpr const char* kBugReportFolder = "bugreports";

struct BugReportInfo {
    BuildInfo build;
    u64 contentHash = 0;
    std::string scenePath, tuningPath;
    u32 tick = 0;
    u32 players = 0, localPlayer = 0;
    std::string note; // 플레이어가 적은 한 줄
};

// 남긴 zip 경로 (실패하면 빈 문자열). 기록·스크린샷 파일이 없으면 그것만 빼고 묶는다
std::string writeBugReport(const std::string& userFolder, const BugReportInfo& info, const std::string& recordingPath,
                           const std::string& screenshotPath);

// zip의 replay.rec를 outFolder/replay.rec로 꺼낸다. info는 info.txt 내용
bool extractBugReport(const std::string& zipPath, const std::string& outFolder, std::string& recordingPath,
                      std::string& info, std::string& error);

} // namespace mealkit::game
