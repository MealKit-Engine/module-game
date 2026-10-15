#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/core/Types.h"
#include "mealkit/game/GameContext.h"
#include "mealkit/render/Renderer.h"

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
inline constexpr u32 kApiVersion = 3; // 2: GameContext, 3: 게임 오브젝트 종류

struct PlayOptions {
    bool bot = false;       // 스크립트 입력 (자동 테스트)
    u32 exitTick = 0;       // 이 틱을 실행하면 finished() = true
    u32 captureTick = 0;    // 이 틱을 지나면 captureDue() = true (한 번)
    bool capture = false;
    u32 physicsThreads = 0; // 0 = 자동
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
};

// 게임 DLL이 내보내는 정보. 에디터가 mealkitGetGameModule()로 받는다.
struct GameModule {
    u32 apiVersion = kApiVersion;
    const char* name = "";
    GameInstance* (*create)() = nullptr;  // 에디터가 플레이를 누를 때
    void (*destroy)(GameInstance*) = nullptr; // DLL 안에서 지운다 (같은 힙)
    // 게임이 MEALKIT_OBJECT로 선언한 씬 오브젝트 종류 (에디터 [추가] 메뉴, 인스펙터). DLL을 내리면 무효가 된다.
    const std::vector<const reflect::TypeInfo*>* (*objectTypes)() = nullptr;
};

using GetGameModuleFn = const GameModule* (*)();
inline constexpr const char* kGetGameModuleSymbol = "mealkitGetGameModule";

} // namespace mealkit::game

// 게임 DLL에서 한 번 쓴다: MEALKIT_GAME_MODULE("Sandbox", sandbox::GameSession)
#define MEALKIT_GAME_MODULE(NameLiteral, GameClass)                                                          \
    extern "C" __declspec(dllexport) const ::mealkit::game::GameModule* mealkitGetGameModule() {             \
        static const ::mealkit::game::GameModule module{                                                     \
            ::mealkit::game::kApiVersion, NameLiteral,                                                        \
            []() -> ::mealkit::game::GameInstance* { return new GameClass(); },                              \
            [](::mealkit::game::GameInstance* g) { delete g; },                                              \
            []() -> const std::vector<const ::mealkit::reflect::TypeInfo*>* {                                \
                return &::mealkit::reflect::moduleObjectTypes();                                             \
            }};                                                                                             \
        return &module;                                                                                    \
    }
