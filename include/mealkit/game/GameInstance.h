#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/core/Types.h"
#include "mealkit/game/BuildInfo.h"
#include "mealkit/game/GameContext.h"
#include "mealkit/game/GameData.h"
#include "mealkit/game/SimDebug.h"
#include "mealkit/render/Renderer.h"

#include <string>
#include <vector>

namespace mealkit {
namespace render {
class DrawList;
}
namespace physics {
class PhysicsWorld;
}
namespace scene {
struct Scene;
}
} // namespace mealkit

// Boundary between the engine (game executable, general-purpose editor) and game code.
// The game implements GameInstance, and the editor game DLL exports a factory function via MEALKIT_GAME_MODULE.
// The shipping exe uses the game class directly (if final, even virtual calls disappear).
namespace mealkit::game {

// Bump when the DLL boundary contract changes. The editor doesn't load game DLLs with a different number.
// 2: GameContext, 3: game object kinds, 4: tuning, 5: state view, 6: debug controls, 7: project settings/string fields, 8: hash log/input recording,
// 9: view tuning (ViewTuningType, GameContext::viewTuning)/Ticks/Color fields, 10: definition data tables (MEALKIT_TABLE, RowRef),
// 11: sound (GameContext::audio), FrameParams::cameraForward, 12: feedback table (GameContext::feedback, EventType), MEALKIT_ENUM,
// 13: input actions (GameContext::actions), 14: debug UI (GameInstance::debugUi, the editor and game DLL share one ImGui from mealkit.dll),
// 15: string tables (GameContext::strings), 16: game screen UI (GameContext::ui, GameModule::viewModels),
// 17: player settings (GameContext::settings), 18: GameContext::userFolder (saves), 19: match start data (GameInstance::lobby, MatchInfo::loadouts),
// 20: screen flow (GameContext::match, GameInstance::menus/stop, lobby names/ready/chat), 21: GameModule::build (build info),
// 22: GameInstance::physics (sound occlusion rays), 23: GameContext::get (core/Services.h registry), 24: ctx.session/MatchService::session() is session::Session (net optional),
// 25: asset::ModelCache without renderer parts (setGpu, render/ModelDraw.h draws models, terrain, scatter)
inline constexpr u32 kApiVersion = 25;

struct PlayOptions {
    bool bot = false;    // scripted input (automated tests)
    u32 exitTick = 0;    // finished() = true after running this tick
    u32 captureTick = 0; // captureDue() = true after passing this tick (once)
    bool capture = false;
    u32 physicsThreads = 0; // 0 = auto
    std::string tuningPath; // assets/game.tuning if empty (the editor passes unsaved values as a temp file)
    bool shadow = false;    // shadow simulation: runs one more World and compares every tick (editor, 2x simulation CPU)
    bool record = false;    // input recording + snapshots: pause, single tick step, rewind (editor solo play)
    std::string hashLogPath;      // if not empty, one "<tick> <state hash>" line per tick (--hash-log, comparing across builds/platforms)
    std::string recordInputsPath; // if not empty, confirmed inputs and hashes go to a file (--record-inputs, game/InputRecord.h)
    // Desync dump folder (game/DesyncDump.h): if not empty, keeps a few snapshots in multiplayer matches and writes the diverged tick's state on desync
    std::string desyncDumpDir;
    u32 desyncTestTick = 0; // if not 0, perturbs physics slightly before running this tick (--desync-test, desync diagnostics test)
};

// One match: runs the deterministic simulation, local input, camera, drawing.
class GameInstance {
public:
    virtual ~GameInstance() = default;

    // Every frame before the match starts (once just before opening the session for solo play): decides this player's match start data
    // (game::setLoadout, mealkit/game/Loadout.h). userFolder is where to read saves (none if empty)
    virtual void lobby(session::Session& session, const PlayOptions& options, const std::string& userFolder) {
        (void)session, (void)options, (void)userFolder;
    }
    // Called once when the session becomes Running (that frame is one frame after drawing the loading screen).
    virtual void start(GameContext& ctx, const scene::Scene& scene, const PlayOptions& options) = 0;
    virtual bool started() const = 0;
    // Once when the match ends and the session is no longer Running (back to lobby, left, disconnected). start is called again for the next match
    virtual void stop(GameContext& ctx) { (void)ctx; }
    // The game draws the screen flow (title/menu/lobby/results screens, ctx.match). Then the engine starts at the menu (no session) when there's no command line
    // and doesn't use Esc/Enter. If false, plays solo directly from the command line as before; the host starts with Enter and quits with Esc
    virtual bool menus() const { return false; }
    // Every frame: camera input and running simulation ticks.
    virtual void update(GameContext& ctx, f64 dt) = 0;
    // Fills the data to draw the scene (ctx.viewport area, local player is ctx.session.localPlayer()).
    virtual void buildFrame(GameContext& ctx, f64 now, render::FrameParams& frame, render::DrawList& out) = 0;

    // The values below are also called before a match (menu/lobby/loading frames): return 0 or empty values when there's no match
    virtual u32 nextTick() const = 0;
    virtual u64 stateHash() const = 0;
    virtual u32 activeBodies() const = 0;
    virtual bool finished() const = 0;
    virtual bool captureDue() = 0;
    virtual f64 takeSimMsPerTick() = 0; // average since the last call
    // The match's physics world, read by the engine between ticks for sound occlusion rays (audio::Engine::setOcclusionWorld). nullptr = no occlusion
    virtual const physics::PhysicsWorld* physics() const { return nullptr; }

    // Changes tuning values immediately (editor solo play). tuning is a value of type GameModule::tuningType.
    virtual void setTuning(const void* tuning) = 0;
    // Simulation state (editor [State] panel, read-only). Valid until the next update.
    virtual reflect::View stateView() const = 0;
    // Pause/rewind/shadow simulation (implemented by game::Simulation). nullptr if none.
    virtual SimDebug* debug() { return nullptr; }
    // Debug UI (dev builds/editor, docs/CODING_MODEL.md "UI written in code"): called inside an ImGui frame. Wrap the body in #if MEALKIT_DEBUG_UI
    // (shipping builds have no ImGui). Buttons that change the simulation don't modify state; they send confirmed commands (mealkit/sim/Commands.h).
    // The shipping exe toggles it with F1. The editor always calls it during play.
    virtual void debugUi(GameContext& ctx) { (void)ctx; }
};

// Info exported by the game DLL. The editor receives it via mealkitGetGameModule().
struct GameModule {
    u32 apiVersion = kApiVersion;
    const char* name = "";
    GameInstance* (*create)() = nullptr;      // when the editor presses play
    void (*destroy)(GameInstance*) = nullptr; // deleted inside the DLL (same heap)
    // Scene object kinds the game declared with MEALKIT_OBJECT (editor [Add] menu, inspector). Invalid after the DLL is unloaded.
    const std::vector<const reflect::TypeInfo*>* (*objectTypes)() = nullptr;
    // Game tuning struct (GameClass::TuningType). The editor [Tuning] panel edits assets/game.tuning with this type.
    const reflect::TypeInfo* (*tuningType)() = nullptr;
    // Project settings added by the game (GameClass::ProjectSettingsType, nullptr if none). game.* lines in mealkit.project.
    const reflect::TypeInfo* (*projectSettingsType)() = nullptr;
    // View tuning (GameClass::ViewTuningType, nullptr if none). The "View" tab of the editor [Tuning] panel, assets/view.tuning.
    const reflect::TypeInfo* (*viewTuningType)() = nullptr;
    // Definition data tables the game registered with MEALKIT_TABLE (editor [Tables] panel, content hash). Invalid after the DLL is unloaded.
    const reflect::TableList* (*tables)() = nullptr;
    // Tick event type (GameClass::EventType, nullptr if none). The editor [Feedback] panel shows kind (MEALKIT_ENUM) names and fields.
    const reflect::TypeInfo* (*eventType)() = nullptr;
    // View models the game declared with MEALKIT_VIEWMODEL (fake values in the editor [UI] preview, binding checks). Invalid after the DLL is unloaded.
    const std::vector<const reflect::TypeInfo*>* (*viewModels)() = nullptr;
    // Game build info (game version/commit/simulation version, mealkit/game/BuildInfo.h)
    BuildInfo build;
};

// Game types used when reading match data (game::loadMatchData)
inline GameTypes gameTypes(const GameModule& module) {
    GameTypes t;
    t.objectTypes = module.objectTypes ? module.objectTypes() : nullptr;
    t.tuningType = module.tuningType ? module.tuningType() : nullptr;
    t.projectSettingsType = module.projectSettingsType ? module.projectSettingsType() : nullptr;
    t.tables = module.tables ? module.tables() : nullptr;
    return t;
}

// The GameClass::ProjectSettingsType type if present (used by MEALKIT_GAME_MODULE)
template <class GameClass>
const reflect::TypeInfo* gameProjectSettingsType() {
    if constexpr (requires { typename GameClass::ProjectSettingsType; })
        return &reflect::typeOf<typename GameClass::ProjectSettingsType>();
    else
        return nullptr;
}

// The GameClass::ViewTuningType type if present
template <class GameClass>
const reflect::TypeInfo* gameViewTuningType() {
    if constexpr (requires { typename GameClass::ViewTuningType; })
        return &reflect::typeOf<typename GameClass::ViewTuningType>();
    else
        return nullptr;
}

template <class GameClass>
const reflect::TypeInfo* gameEventType() {
    if constexpr (requires { typename GameClass::EventType; })
        return &reflect::typeOf<typename GameClass::EventType>();
    else
        return nullptr;
}

// Module info of the game class (MEALKIT_GAME_MODULE: editor DLL; MEALKIT_GAME_MAIN: the shipping exe uses the same)
template <class GameClass>
const GameModule& gameModuleOf(const char* name) {
    static const GameModule module{
        kApiVersion,
        name,
        []() -> GameInstance* { return new GameClass(); },
        [](GameInstance* g) { delete g; },
        []() -> const std::vector<const reflect::TypeInfo*>* { return &reflect::moduleObjectTypes(); },
        []() -> const reflect::TypeInfo* { return &reflect::typeOf<typename GameClass::TuningType>(); },
        &gameProjectSettingsType<GameClass>,
        &gameViewTuningType<GameClass>,
        []() -> const reflect::TableList* { return &reflect::moduleTables(); },
        &gameEventType<GameClass>,
        []() -> const std::vector<const reflect::TypeInfo*>* { return &reflect::moduleViewModels(); },
        MEALKIT_THIS_BUILD};
    return module;
}

using GetGameModuleFn = const GameModule* (*)();
inline constexpr const char* kGetGameModuleSymbol = "mealkitGetGameModule";

} // namespace mealkit::game

// Exports a function from the game library: the editor finds mealkitGetGameModule by name (Windows .dll, Linux .so, macOS .dylib).
// Clang/GCC outside Windows reject __declspec without -fdeclspec, and default visibility is what they export with
#if defined(_WIN32)
#define MEALKIT_GAME_EXPORT __declspec(dllexport)
#else
#define MEALKIT_GAME_EXPORT __attribute__((visibility("default")))
#endif

// Used once in the game DLL: MEALKIT_GAME_MODULE("Sandbox", sandbox::GameSession)
#define MEALKIT_GAME_MODULE(NameLiteral, GameClass)                                                                    \
    extern "C" MEALKIT_GAME_EXPORT const ::mealkit::game::GameModule* mealkitGetGameModule() {                       \
        return &::mealkit::game::gameModuleOf<GameClass>(NameLiteral);                                                 \
    }
