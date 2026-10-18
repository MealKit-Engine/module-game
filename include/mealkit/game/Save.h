#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/core/Types.h"

#include <string>

// 진행 데이터 세이브 (ROADMAP 1단계 "세이브"): 매치 밖에 남는 데이터(해금, 통계, 인벤토리)를 게임의 MEALKIT_STRUCT 하나로.
//   saves/<이름>.save in 사용자 폴더 (game::userFolder, GameContext::userFolder). 텍스트("경로 값" 줄)라 사람이 읽을 수 있다.
//   첫 줄에 형식 버전: 모르는 필드는 버리고 없는 필드는 기본값으로 읽는다. 버전이 낮으면 migrate가 줄을 고칠 기회를 받는다.
//   쓰기는 임시 파일에 쓴 뒤 바꿔 넣고(꺼져도 원본이 깨지지 않는다), 직전 파일을 .bak으로 남긴다.
// 시뮬레이션은 진행 데이터를 읽지 않는다. 매치에 영향을 주는 값(고른 캐릭터, 장비)은 매치 시작 데이터로 보낸다.
// 클라우드: Steam은 Auto-Cloud(Steamworks에서 saves/ 폴더를 지정, 코드 없음), 다른 플랫폼은 온라인 서비스 계층(3단계 EOS).
namespace mealkit::game {

inline constexpr const char* kSavesFolder = "saves";

struct SaveInfo {
    bool found = false;
    u32 version = 0; // 파일의 형식 버전
    std::string path;
    std::string warnings; // 모르는 필드 등
};

// 줄을 고치는 함수: 파일 버전 → 지금 버전 (예: 이름이 바뀐 필드 옮기기)
using SaveMigrate = void (*)(u32 fileVersion, reflect::TextFields& fields);

std::string savePath(const std::string& userFolder, const std::string& name);
bool writeSave(const reflect::TypeInfo& type, const void* data, const std::string& userFolder, const std::string& name, u32 version);
SaveInfo readSave(const reflect::TypeInfo& type, void* data, const std::string& userFolder, const std::string& name, u32 version,
                  SaveMigrate migrate = nullptr);

template <class T>
bool writeSave(const T& data, const std::string& userFolder, const std::string& name, u32 version) {
    return writeSave(reflect::typeOf<T>(), &data, userFolder, name, version);
}
template <class T>
SaveInfo readSave(T& data, const std::string& userFolder, const std::string& name, u32 version, SaveMigrate migrate = nullptr) {
    return readSave(reflect::typeOf<T>(), &data, userFolder, name, version, migrate);
}

} // namespace mealkit::game
