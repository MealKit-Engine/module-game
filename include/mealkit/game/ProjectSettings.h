#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/net/Lockstep.h"

#include <algorithm>

#include <string>

// 프로젝트 설정: 프로젝트 폴더 최상위의 mealkit.project (docs/CODING_MODEL.md "프로젝트 단위 설정").
// 엔진 공통 항목은 ProjectSettings, 게임이 더하는 항목은 게임의 MEALKIT_STRUCT(GameClass::ProjectSettingsType)이고
// 파일에는 "game." 접두사로 들어간다. 형식은 .tuning과 같은 "경로 값" 줄이다 (예전 "키=값" 줄도 읽는다).
//
//   name sandbox
//   engine D:\MealKit
//   engineVersion 0.4.1
//   scene scenes/arena.scene
//   game.defaultPort 27015
//
// 게임 실행 파일·범용 에디터·도구(new_project, upgrade_project, build.bat)가 모두 이 파일 하나를 읽는다.
// 시뮬레이션에 닿는 항목만 콘텐츠 해시에 넣는다: 해시에서 뺄 항목은 MEALKIT_VIEW_FIELD(또는 kFieldView)로 선언한다.
namespace mealkit::game {

inline constexpr const char* kProjectFileName = "mealkit.project";
inline constexpr const char* kGameSettingsPrefix = "game.";

// 필드 설명은 에디터 글자라 영어로 쓰고 한국어는 engine/res/strings/ko.txt (docs/CODING_MODEL.md "에디터 UI 문자열과 언어")
MEALKIT_STRUCT(ProjectSettings) {
    MEALKIT_FIELD_FLAGS(std::string, name, {}, "Game name (exe name)", reflect::kFieldView | reflect::kFieldReadOnly);
    MEALKIT_FIELD_FLAGS(std::string, engine, {}, "Engine folder", reflect::kFieldView | reflect::kFieldReadOnly);
    MEALKIT_FIELD_FLAGS(std::string, engineVersion, {}, "Engine version", reflect::kFieldView | reflect::kFieldReadOnly);
    MEALKIT_VIEW_FIELD(std::string, scene, "scenes/arena.scene", "Start scene (under assets)");
    // 게임 버전: 창 제목·타이틀·로비에 보이고, 시뮬레이션 버전에 들어간다(빌드할 때, 다른 버전과는 접속하지 않는다)
    MEALKIT_VIEW_FIELD(std::string, version, "0.1.0", "Game version (shown to players, compared when joining)");
    MEALKIT_VIEW_FIELD(bool, exportVersionFolder, false, "Put the version in the export folder name (export/<name>-<version>)");
    // 프로젝트 관리자가 보여 준다. 아이콘은 관리자에서 그림을 고르면 프로젝트 폴더에 PNG로 복사된다
    MEALKIT_VIEW_FIELD(std::string, icon, "icon.png", "Game icon (PNG, project folder)");
    MEALKIT_FIELD_FLAGS(std::string, created, {}, "Development started (YYYY-MM-DD)",
                        reflect::kFieldView | reflect::kFieldReadOnly);
    MEALKIT_VIEW_FIELD(u32, audioVoices, 32, "Sound voices mixed at once");
    // 시뮬레이션 틱레이트 (콘텐츠 해시에 들어간다). 시간 값은 데이터에 초로 적혀 있어 바꿔도 튜닝·표를 고치지 않는다.
    // 60은 반응·충돌이 좋아지지만 시뮬레이션 CPU와 입력 패킷이 2배다 (물체 수천 개짜리 게임에는 쓰지 않는다)
    MEALKIT_FIELD(u32, tickRate, 30, "Simulation ticks per second (30 or 60)");
    MEALKIT_VIEW_FIELD(std::string, language, "ko", "Default language (assets/strings/<language>.txt)");
    // 입력 지연 자동 조절 (호스트가 정한다, 피어마다 같을 필요가 없어 해시 밖). 지금은 PC(유선) 값, 다른 플랫폼은 5단계에서 종류별로
    MEALKIT_VIEW_FIELD(bool, inputDelayAuto, true, "Choose the input delay from the players' ping");
    MEALKIT_VIEW_FIELD(u32, inputDelayMinMs, 60, "Shortest input delay (ms)");
    MEALKIT_VIEW_FIELD(u32, inputDelayMaxMs, 270, "Longest input delay (ms, slower players are warned in the lobby)");
};

struct ProjectFile {
    ProjectSettings settings;
    // "game." 줄 (접두사를 뗀 경로). 게임 타입을 모르는 쪽(게임 DLL을 불러오기 전의 에디터)도 저장할 때 잃지 않는다.
    reflect::TextFields gameFields;
    reflect::TextFields unknownFields; // 이 엔진이 모르는 줄 (새 엔진이 쓴 항목). 그대로 다시 쓴다
    std::string header;                // 첫 키 앞의 주석 줄 ("# ..."), 저장할 때 그대로
};

// 실패하면 false와 이유. 모르는 키는 경고하고 unknownFields에 둔다.
bool loadProjectFile(const std::string& path, ProjectFile& out, std::string* error = nullptr);
bool saveProjectFile(const std::string& path, const ProjectFile& project);

// gameFields를 게임 타입 값에 넣는다 (선언되지 않은 경로는 경고).
void applyGameSettings(const ProjectFile& project, const reflect::TypeInfo& gameType, void* gameData);

// 게임 실행 파일용: 실행 파일 폴더(출시본)나 그 위(개발 중)에서 mealkit.project를 찾아 읽고 게임 항목을 gameData에 넣는다.
// 파일이 없으면 기본값 그대로 두고 false (경고 로그).
bool loadGameProject(ProjectFile& out, const reflect::TypeInfo* gameType = nullptr, void* gameData = nullptr);

// 프로젝트의 틱레이트를 프로세스 틱레이트로 (reflect::setTickRate, 10..120). 시간 필드(Ticks)를 읽기 전에 부른다.
// loadGameProject·loadMatchData·에디터의 프로젝트 열기가 부른다
void applyTickRate(const ProjectSettings& settings);
// 한 틱의 길이 (초, 프로세스 틱레이트)
inline f32 tickDt() { return 1.0f / static_cast<f32>(reflect::tickRate()); }
// 밀리초를 틱으로 (반올림, 1 이상)
inline u32 msToTicks(u32 ms) { return std::max(1u, static_cast<u32>(static_cast<f32>(ms) * static_cast<f32>(reflect::tickRate()) / 1000.0f + 0.5f)); }

// 프로젝트 설정의 입력 지연 범위를 틱으로 (tickMs: 한 틱 ms)
net::AutoDelay autoDelayOf(const ProjectSettings& settings, f32 tickMs);

// 시작 씬의 findAsset 경로 ("assets/scenes/arena.scene")
inline std::string startSceneAsset(const ProjectSettings& settings) { return "assets/" + settings.scene; }
// 플레이어별 파일 폴더 (바꾼 키, 설정, 세이브): 사용자 앱 데이터 폴더/<게임 이름>. 콘텐츠 해시와 무관하다.
std::string userFolder(const ProjectSettings& settings);

} // namespace mealkit::game
