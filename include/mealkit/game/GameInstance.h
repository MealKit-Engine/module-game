#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/core/Types.h"
#include "mealkit/game/GameContext.h"
#include "mealkit/game/SimDebug.h"
#include "mealkit/render/Renderer.h"

#include <string>
#include <vector>

namespace mealkit {
namespace render {
class DrawList;
}
namespace scene {
struct Scene;
}
} // namespace mealkit

// 엔진(게임 실행 파일, 범용 에디터)과 게임 코드 사이의 경계.
// 게임은 GameInstance를 구현하고, 에디터용 게임 DLL은 MEALKIT_GAME_MODULE로 만드는 함수를 내보낸다.
// 출시용 exe는 게임 클래스를 직접 쓴다 (final이면 가상 호출도 사라진다).
namespace mealkit::game {

// DLL 경계의 약속이 바뀌면 올린다. 에디터는 번호가 다른 게임 DLL을 불러오지 않는다.
// 2: GameContext, 3: 게임 오브젝트 종류, 4: 튜닝, 5: 상태 보기, 6: 디버그 제어, 7: 프로젝트 설정·문자열 필드, 8: 해시 로그·입력 기록,
// 9: 화면용 튜닝(ViewTuningType, GameContext::viewTuning)·Ticks·Color 필드, 10: 정의 데이터 표(MEALKIT_TABLE, RowRef),
// 11: 소리(GameContext::audio), FrameParams::cameraForward
inline constexpr u32 kApiVersion = 11;

struct PlayOptions {
    bool bot = false;    // 스크립트 입력 (자동 테스트)
    u32 exitTick = 0;    // 이 틱을 실행하면 finished() = true
    u32 captureTick = 0; // 이 틱을 지나면 captureDue() = true (한 번)
    bool capture = false;
    u32 physicsThreads = 0; // 0 = 자동
    std::string tuningPath; // 비어 있으면 assets/game.tuning (에디터는 저장 안 된 값을 임시 파일로 넘긴다)
    bool shadow = false;    // 그림자 시뮬레이션: World를 하나 더 돌려 매 틱 비교 (에디터, 시뮬레이션 CPU 2배)
    bool record = false;    // 입력 기록 + 스냅샷: 일시정지, 한 틱 진행, 되감기 (에디터 혼자 플레이)
    std::string hashLogPath;      // 비어 있지 않으면 틱마다 "<틱> <상태 해시>" 한 줄 (--hash-log, 빌드·플랫폼 사이 비교)
    std::string recordInputsPath; // 비어 있지 않으면 확정된 입력과 해시를 파일로 (--record-inputs, game/InputRecord.h)
};

// 한 판: 결정론 시뮬레이션 실행, 로컬 입력, 카메라, 그리기.
class GameInstance {
public:
    virtual ~GameInstance() = default;

    // 세션이 Running이 되면 한 번 호출한다.
    virtual void start(GameContext& ctx, const scene::Scene& scene, const PlayOptions& options) = 0;
    virtual bool started() const = 0;
    // 매 프레임: 카메라 입력과 시뮬레이션 틱 실행.
    virtual void update(GameContext& ctx, f64 dt) = 0;
    // 장면을 그릴 데이터를 채운다 (ctx.viewport 영역, 로컬 플레이어는 ctx.session.localPlayer()).
    virtual void buildFrame(GameContext& ctx, f64 now, render::FrameParams& frame, render::DrawList& out) = 0;

    virtual u32 nextTick() const = 0;
    virtual u64 stateHash() const = 0;
    virtual u32 activeBodies() const = 0;
    virtual bool finished() const = 0;
    virtual bool captureDue() = 0;
    virtual f64 takeSimMsPerTick() = 0; // 지난 호출 이후 평균

    // 튜닝 값을 바로 바꾼다 (에디터 혼자 플레이). tuning은 GameModule::tuningType 타입의 값이다.
    virtual void setTuning(const void* tuning) = 0;
    // 시뮬레이션 상태 (에디터 [상태] 패널, 읽기 전용). 다음 update까지 유효하다.
    virtual reflect::View stateView() const = 0;
    // 일시정지·되감기·그림자 시뮬레이션 (game::Simulation이 구현한다). 없으면 nullptr.
    virtual SimDebug* debug() { return nullptr; }
};

// 게임 DLL이 내보내는 정보. 에디터가 mealkitGetGameModule()로 받는다.
struct GameModule {
    u32 apiVersion = kApiVersion;
    const char* name = "";
    GameInstance* (*create)() = nullptr;      // 에디터가 플레이를 누를 때
    void (*destroy)(GameInstance*) = nullptr; // DLL 안에서 지운다 (같은 힙)
    // 게임이 MEALKIT_OBJECT로 선언한 씬 오브젝트 종류 (에디터 [추가] 메뉴, 인스펙터). DLL을 내리면 무효가 된다.
    const std::vector<const reflect::TypeInfo*>* (*objectTypes)() = nullptr;
    // 게임 튜닝 구조체 (GameClass::TuningType). 에디터 [튜닝] 패널이 assets/game.tuning을 이 타입으로 편집한다.
    const reflect::TypeInfo* (*tuningType)() = nullptr;
    // 게임이 더한 프로젝트 설정 (GameClass::ProjectSettingsType, 없으면 nullptr). mealkit.project의 game.* 줄.
    const reflect::TypeInfo* (*projectSettingsType)() = nullptr;
    // 화면용 튜닝 (GameClass::ViewTuningType, 없으면 nullptr). 에디터 [튜닝] 패널의 "화면" 탭, assets/view.tuning.
    const reflect::TypeInfo* (*viewTuningType)() = nullptr;
    // 게임이 MEALKIT_TABLE로 등록한 정의 데이터 표 (에디터 [표] 패널, 콘텐츠 해시). DLL을 내리면 무효가 된다.
    const reflect::TableList* (*tables)() = nullptr;
};

// GameClass::ProjectSettingsType이 있으면 그 타입 (MEALKIT_GAME_MODULE이 쓴다)
template <class GameClass>
const reflect::TypeInfo* gameProjectSettingsType() {
    if constexpr (requires { typename GameClass::ProjectSettingsType; })
        return &reflect::typeOf<typename GameClass::ProjectSettingsType>();
    else
        return nullptr;
}

// GameClass::ViewTuningType이 있으면 그 타입
template <class GameClass>
const reflect::TypeInfo* gameViewTuningType() {
    if constexpr (requires { typename GameClass::ViewTuningType; })
        return &reflect::typeOf<typename GameClass::ViewTuningType>();
    else
        return nullptr;
}

using GetGameModuleFn = const GameModule* (*)();
inline constexpr const char* kGetGameModuleSymbol = "mealkitGetGameModule";

} // namespace mealkit::game

// 게임 DLL에서 한 번 쓴다: MEALKIT_GAME_MODULE("Sandbox", sandbox::GameSession)
#define MEALKIT_GAME_MODULE(NameLiteral, GameClass)                                                                    \
    extern "C" __declspec(dllexport) const ::mealkit::game::GameModule* mealkitGetGameModule() {                       \
        static const ::mealkit::game::GameModule module{                                                               \
            ::mealkit::game::kApiVersion,                                                                              \
            NameLiteral,                                                                                               \
            []() -> ::mealkit::game::GameInstance* { return new GameClass(); },                                        \
            [](::mealkit::game::GameInstance* g) { delete g; },                                                        \
            []() -> const std::vector<const ::mealkit::reflect::TypeInfo*>* {                                          \
                return &::mealkit::reflect::moduleObjectTypes();                                                       \
            },                                                                                                         \
            []() -> const ::mealkit::reflect::TypeInfo* {                                                              \
                return &::mealkit::reflect::typeOf<typename GameClass::TuningType>();                                  \
            },                                                                                                         \
            &::mealkit::game::gameProjectSettingsType<GameClass>,                                                      \
            &::mealkit::game::gameViewTuningType<GameClass>,                                                           \
            []() -> const ::mealkit::reflect::TableList* { return &::mealkit::reflect::moduleTables(); }};             \
        return &module;                                                                                                \
    }
