#pragma once

#include "aster/core/Types.h"
#include "aster/render/Renderer.h"

namespace aster {
class Window;
namespace asset {
class ModelCache;
}
namespace net {
class LockstepSession;
}
namespace render {
class DrawList;
}
namespace scene {
struct Scene;
}
} // namespace aster

// 엔진(게임 실행 파일, 범용 에디터)과 게임 코드 사이의 경계.
// 게임은 GameInstance를 구현하고, 에디터용 게임 DLL은 ASTER_GAME_MODULE로 만드는 함수를 내보낸다.
// 출시용 exe는 게임 클래스를 직접 쓴다 (final이면 가상 호출도 사라진다).
namespace aster::game {

// DLL 경계의 약속이 바뀌면 올린다. 에디터는 번호가 다른 게임 DLL을 불러오지 않는다.
inline constexpr u32 kApiVersion = 1;

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
    virtual void start(const scene::Scene& scene, const net::LockstepSession& session, const PlayOptions& options,
                       asset::ModelCache& models) = 0;
    virtual bool started() const = 0;
    // 매 프레임: 카메라 입력과 시뮬레이션 틱 실행.
    virtual void update(f64 dt, net::LockstepSession& session, Window& window, bool keyboardAllowed,
                        bool mouseAllowed) = 0;
    // 장면을 그릴 데이터를 채운다.
    virtual void buildFrame(f64 now, u8 localPlayer, render::Viewport viewport, render::FrameParams& frame,
                            render::DrawList& out) = 0;

    virtual u32 nextTick() const = 0;
    virtual u64 stateHash() const = 0;
    virtual u32 activeBodies() const = 0;
    virtual bool finished() const = 0;
    virtual bool captureDue() = 0;
    virtual f64 takeSimMsPerTick() = 0; // 지난 호출 이후 평균
};

// 게임 DLL이 내보내는 정보. 에디터가 asterGetGameModule()로 받는다.
struct GameModule {
    u32 apiVersion = kApiVersion;
    const char* name = "";
    GameInstance* (*create)() = nullptr;  // 에디터가 플레이를 누를 때
    void (*destroy)(GameInstance*) = nullptr; // DLL 안에서 지운다 (같은 힙)
};

using GetGameModuleFn = const GameModule* (*)();
inline constexpr const char* kGetGameModuleSymbol = "asterGetGameModule";

} // namespace aster::game

// 게임 DLL에서 한 번 쓴다: ASTER_GAME_MODULE("Sandbox", sandbox::GameSession)
#define ASTER_GAME_MODULE(NameLiteral, GameClass)                                                          \
    extern "C" __declspec(dllexport) const ::aster::game::GameModule* asterGetGameModule() {             \
        static const ::aster::game::GameModule module{                                                     \
            ::aster::game::kApiVersion, NameLiteral,                                                        \
            []() -> ::aster::game::GameInstance* { return new GameClass(); },                              \
            [](::aster::game::GameInstance* g) { delete g; }};                                             \
        return &module;                                                                                    \
    }
