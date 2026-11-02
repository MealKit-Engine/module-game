#pragma once

#include "mealkit/asset/Budget.h"
#include "mealkit/core/Reflect.h"
#include "mealkit/net/Lockstep.h"
#include "mealkit/platform/Paths.h"

#include <algorithm>

#include <string>

// Project settings: mealkit.project at the top of the project folder (docs/CODING_MODEL.md "Per-project settings").
// Common engine items are ProjectSettings; items the game adds are the game's MEALKIT_STRUCT (GameClass::ProjectSettingsType),
// stored in the file with a "game." prefix. The format is "path value" lines like .tuning (old "key=value" lines are also read).
//
//   name sandbox
//   engine D:\MealKit
//   engineVersion 0.4.1
//   scene scenes/arena.scene
//   game.defaultPort 27015
//
// Game executables, the general-purpose editor, and tools (new_project, upgrade_project, build.bat) all read this one file.
// Only items that touch the simulation go into the content hash: declare items to exclude with MEALKIT_VIEW_FIELD (or kFieldView).
namespace mealkit::game {

inline constexpr const char* kProjectFileName = "mealkit.project";
inline constexpr const char* kGameSettingsPrefix = "game.";

// Lighting model (style preset, same order as render::LightingModel): the biggest switch of the game's art direction. game::applyRenderStyle passes it to the renderer
MEALKIT_ENUM(LightingModelSetting, u8, Pbr, Toon, Unlit);

// Field descriptions are editor text, so written in English with Korean in engine/res/strings/ko.txt (docs/CODING_MODEL.md "Editor UI strings and languages")
MEALKIT_STRUCT(ProjectSettings) {
    MEALKIT_FIELD_FLAGS(std::string, name, {}, "Game name (exe name)", reflect::kFieldView | reflect::kFieldReadOnly);
    MEALKIT_FIELD_FLAGS(std::string, engine, {}, "Engine folder", reflect::kFieldView | reflect::kFieldReadOnly);
    MEALKIT_FIELD_FLAGS(std::string, engineVersion, {}, "Engine version", reflect::kFieldView | reflect::kFieldReadOnly);
    MEALKIT_VIEW_FIELD(std::string, scene, "scenes/arena.scene", "Start scene (under assets)");
    // Game version: shown in the window title, title screen, and lobby, and part of the simulation version (at build time; no connecting with other versions)
    MEALKIT_VIEW_FIELD(std::string, version, "0.1.0", "Game version (shown to players, compared when joining)");
    // Pack files (game/Cook.h): the cook step starts from the start scene and these scenes, follows references, and packs only what's reached.
    // Assets opened only by a path in code go in packInclude (files or folders under assets; the cook warns about code paths it didn't reach)
    MEALKIT_VIEW_FIELD(std::string, scenes, {}, "Other scenes to ship (under assets, separated by spaces)");
    MEALKIT_VIEW_FIELD(std::string, packInclude, {}, "Always put in the pack (files or folders under assets, separated by spaces)");
    MEALKIT_VIEW_FIELD(bool, exportVersionFolder, false, "Put the version in the export folder name (export/<name>-<version>)");
    // Shown by the project manager. Choosing an image for the icon in the manager copies it as a PNG into the project folder
    MEALKIT_VIEW_FIELD(std::string, icon, "icon.png", "Game icon (PNG, project folder)");
    MEALKIT_FIELD_FLAGS(std::string, templateName, {}, "Project template it was made from (arena, empty)",
                        reflect::kFieldView | reflect::kFieldReadOnly);
    MEALKIT_FIELD_FLAGS(std::string, created, {}, "Development started (YYYY-MM-DD)",
                        reflect::kFieldView | reflect::kFieldReadOnly);
    MEALKIT_VIEW_FIELD(u32, audioVoices, 32, "Sound voices mixed at once");
    // Replays (game/InputRecord.h): keeps input recordings in the user folder replays/ per match (4 players 30 min about 2 MB). 0 keeps none
    MEALKIT_VIEW_FIELD(u32, keepReplays, 20, "Replays kept in the user folder (0: do not record)");
    // Bug report bundles (game/BugReport.h): F9 makes a zip of input recording/log/screenshot in the user folder bugreports/. Included in shipping builds too
    MEALKIT_VIEW_FIELD(bool, bugReports, true, "F9 saves a bug report (input recording, log, screenshot)");
    // Style presets (docs/CODING_MODEL.md "Shader code"): only the view changes, so outside the hash. Material features are in the model's glTF extras
    MEALKIT_VIEW_FIELD(LightingModelSetting, lightingModel, LightingModelSetting::Pbr, "Lighting model (Pbr, Toon, Unlit)");
    MEALKIT_VIEW_FIELD(std::string, toonRamp, {}, "Toon ramp image (under assets, left is shadow, right is lit, empty for 3 steps)");
    // Simulation tick rate (goes into the content hash). Time values are written in seconds in data, so changing it doesn't require editing tuning/tables.
    // 60 improves responsiveness/collisions but doubles simulation CPU and input packets (not for games with thousands of objects)
    MEALKIT_FIELD(u32, tickRate, 30, "Simulation ticks per second (30 or 60)");
    MEALKIT_VIEW_FIELD(std::string, language, "ko", "Default language (assets/strings/<language>.txt)");
    // Automatic input delay adjustment (decided by the host; doesn't need to match per peer, so outside the hash). Currently PC (wired) values; other platforms per kind in stage 5
    MEALKIT_VIEW_FIELD(bool, inputDelayAuto, true, "Choose the input delay from the players' ping");
    MEALKIT_VIEW_FIELD(u32, inputDelayMinMs, 60, "Shortest input delay (ms)");
    MEALKIT_VIEW_FIELD(u32, inputDelayMaxMs, 270, "Longest input delay (ms, slower players are warned in the lobby)");
    // GPU budget (based on min-spec Intel 11th gen integrated graphics 1280x720 60 fps; the sum is the frame GPU budget). The benchmark report compares with per-pass GPU times
    // and marks OVER when exceeded (docs/ROADMAP.md "Benchmark mode"). Tuned by min-spec measurements
    MEALKIT_VIEW_FIELD(f32, gpuBudgetOpaqueMs, 3.0f, "GPU budget: opaque objects (ms)");
    MEALKIT_VIEW_FIELD(f32, gpuBudgetShadowMs, 1.5f, "GPU budget: shadows (ms)");
    MEALKIT_VIEW_FIELD(f32, gpuBudgetPostMs, 1.0f, "GPU budget: post processing (ms)");
    MEALKIT_VIEW_FIELD(f32, gpuBudgetVfxMs, 1.0f, "GPU budget: effects and transparency (ms)");
    MEALKIT_VIEW_FIELD(f32, gpuBudgetUiMs, 0.5f, "GPU budget: UI (ms)");
    MEALKIT_VIEW_FIELD(f32, gpuBudgetSpareMs, 1.0f, "GPU budget: spare (ms)");
    // Dynamic resolution (render::ResolutionSettings): when GPU frame time exceeds the sum of the budgets above, lowers 3D resolution down to this scale (UI unchanged)
    MEALKIT_VIEW_FIELD(bool, dynamicResolution, true, "Dynamic resolution (lower 3D resolution over the GPU budget)");
    MEALKIT_VIEW_FIELD(f32, dynamicResolutionMinScale, 0.5f, "Dynamic resolution: lowest scale (0.25-1)");
    MEALKIT_VIEW_FIELD(f32, upscaleSharpness, 0.5f, "Upscale sharpening (0-1)");
    // Reflection probes (docs/ROADMAP.md stage 2): number used per scene (extra probes are dropped) and cube face size (rounded down to a power of two)
    MEALKIT_VIEW_FIELD(u32, probeMaxCount, 8, "Reflection probes per scene (max)");
    MEALKIT_VIEW_FIELD(u32, probeSize, 128, "Reflection probe size (px)");
    // Point lights (docs/ROADMAP.md stage 2): lights used per object (1..8). More makes the pixel shader heavier
    MEALKIT_VIEW_FIELD(u32, pointLightsPerObject, 4, "Point lights per object (1-8)");
    // Sun shadows (docs/ROADMAP.md stage 2, render::ShadowSettings): cascade count, map size of one cascade, shadow distance from the camera
    MEALKIT_VIEW_FIELD(bool, shadows, true, "Sun shadows");
    MEALKIT_VIEW_FIELD(u32, shadowCascades, 2, "Shadow cascades (1-2)");
    MEALKIT_VIEW_FIELD(u32, shadowMapSize, 1024, "Shadow map size (px)");
    MEALKIT_VIEW_FIELD(f32, shadowDistance, 40.0f, "Shadow distance (m)");
    // Asset budgets (asset/Budget.h): model kind is the folder under assets/models/ (characters, props, environment). Exceeding warns on load and
    // the editor inspector and Blender add-on show the same values. Defaults are based on min-spec integrated graphics (tuned by benchmark)
    MEALKIT_VIEW_FIELD(u32, budgetCharactersTriangles, asset::kDefaultBudgets[0].triangles, "Budget, characters: triangles");
    MEALKIT_VIEW_FIELD(u32, budgetCharactersTextureSize, asset::kDefaultBudgets[0].textureSize, "Budget, characters: texture size (px)");
    MEALKIT_VIEW_FIELD(u32, budgetCharactersBones, asset::kDefaultBudgets[0].bones, "Budget, characters: bones");
    MEALKIT_VIEW_FIELD(u32, budgetCharactersMaterials, asset::kDefaultBudgets[0].materials, "Budget, characters: materials");
    MEALKIT_VIEW_FIELD(u32, budgetPropsTriangles, asset::kDefaultBudgets[1].triangles, "Budget, props: triangles");
    MEALKIT_VIEW_FIELD(u32, budgetPropsTextureSize, asset::kDefaultBudgets[1].textureSize, "Budget, props: texture size (px)");
    MEALKIT_VIEW_FIELD(u32, budgetPropsBones, asset::kDefaultBudgets[1].bones, "Budget, props: bones");
    MEALKIT_VIEW_FIELD(u32, budgetPropsMaterials, asset::kDefaultBudgets[1].materials, "Budget, props: materials");
    MEALKIT_VIEW_FIELD(u32, budgetEnvironmentTriangles, asset::kDefaultBudgets[2].triangles, "Budget, environment: triangles");
    MEALKIT_VIEW_FIELD(u32, budgetEnvironmentTextureSize, asset::kDefaultBudgets[2].textureSize, "Budget, environment: texture size (px)");
    MEALKIT_VIEW_FIELD(u32, budgetEnvironmentBones, asset::kDefaultBudgets[2].bones, "Budget, environment: bones");
    MEALKIT_VIEW_FIELD(u32, budgetEnvironmentMaterials, asset::kDefaultBudgets[2].materials, "Budget, environment: materials");
    // Small-object distance cull (asset::ModelBudget::cullSize): screen size below which instances aren't drawn, 0 = off
    MEALKIT_VIEW_FIELD(f32, cullCharactersSize, asset::kDefaultBudgets[0].cullSize, "Cull below screen size: characters");
    MEALKIT_VIEW_FIELD(f32, cullPropsSize, asset::kDefaultBudgets[1].cullSize, "Cull below screen size: props");
    MEALKIT_VIEW_FIELD(f32, cullEnvironmentSize, asset::kDefaultBudgets[2].cullSize, "Cull below screen size: environment");
};

struct ProjectFile {
    ProjectSettings settings;
    // "game." lines (paths with the prefix removed). Code that doesn't know the game type (the editor before loading the game DLL) doesn't lose them on save.
    reflect::TextFields gameFields;
    reflect::TextFields unknownFields; // lines this engine doesn't know (items written by a newer engine). Written back as is
    std::string header;                // comment lines before the first key ("# ..."), kept as is on save
};

// false and a reason on failure. Unknown keys warn and go into unknownFields.
bool loadProjectFile(const std::string& path, ProjectFile& out, std::string* error = nullptr);
bool saveProjectFile(const std::string& path, const ProjectFile& project);

// Puts gameFields into the game type value (undeclared paths warn).
void applyGameSettings(const ProjectFile& project, const reflect::TypeInfo& gameType, void* gameData);

// For game executables: finds and reads mealkit.project in the executable folder (shipping) or above it (development) and puts game items into gameData.
// If the file is missing, leaves defaults as is and returns false (warning log).
bool loadGameProject(ProjectFile& out, const reflect::TypeInfo* gameType = nullptr, void* gameData = nullptr);

// Sets the project's tick rate as the process tick rate (reflect::setTickRate, 10..120). Call before reading time fields (Ticks).
// Called by loadGameProject, loadMatchData, and the editor's project open
void applyTickRate(const ProjectSettings& settings);
// Length of one tick (seconds, process tick rate)
inline f32 tickDt() { return 1.0f / static_cast<f32>(reflect::tickRate()); }
// Milliseconds to ticks (rounded, at least 1)
inline u32 msToTicks(u32 ms) { return std::max(1u, static_cast<u32>(static_cast<f32>(ms) * static_cast<f32>(reflect::tickRate()) / 1000.0f + 0.5f)); }

// The project setting's input delay range in ticks (tickMs: ms per tick)
net::AutoDelay autoDelayOf(const ProjectSettings& settings, f32 tickMs);

// Per-kind model budgets (passed to ModelCache::setBudgets)
inline void modelBudgets(const ProjectSettings& s, asset::ModelBudget (&out)[asset::kModelKindCount]) {
    out[0] = {s.budgetCharactersTriangles, s.budgetCharactersTextureSize, s.budgetCharactersBones, s.budgetCharactersMaterials,
              s.cullCharactersSize};
    out[1] = {s.budgetPropsTriangles, s.budgetPropsTextureSize, s.budgetPropsBones, s.budgetPropsMaterials, s.cullPropsSize};
    out[2] = {s.budgetEnvironmentTriangles, s.budgetEnvironmentTextureSize, s.budgetEnvironmentBones,
              s.budgetEnvironmentMaterials, s.cullEnvironmentSize};
}

// findAsset path of the start scene ("assets/scenes/arena.scene")
inline std::string startSceneAsset(const ProjectSettings& settings) { return assetFilePath(settings.scene); }
// Per-player file folder (rebound keys, settings, saves): user app data folder/<game name>. Unrelated to the content hash.
std::string userFolder(const ProjectSettings& settings);

} // namespace mealkit::game
