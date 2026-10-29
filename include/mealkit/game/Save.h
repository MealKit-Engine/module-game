#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/core/Types.h"

#include <string>

// Progress data saves (ROADMAP stage 1 "Saves"): data that persists outside matches (unlocks, stats, inventory) as one game MEALKIT_STRUCT.
//   saves/<name>.save in the user folder (game::userFolder, GameContext::userFolder). Text ("path value" lines), so human readable.
//   Format version on the first line: unknown fields are dropped and missing fields read as defaults. For older versions, migrate gets a chance to fix lines.
//   Writes go to a temp file and are swapped in (a power loss doesn't corrupt the original), keeping the previous file as .bak.
// The simulation doesn't read progress data. Values affecting the match (chosen character, gear) are sent as match start data.
// Cloud: Steam uses Auto-Cloud (specify the saves/ folder in Steamworks, no code), other platforms the online service layer (stage 3 EOS).
namespace mealkit::game {

inline constexpr const char* kSavesFolder = "saves";

struct SaveInfo {
    bool found = false;
    u32 version = 0; // the file's format version
    std::string path;
    std::string warnings; // unknown fields etc.
};

// Function that fixes lines: file version → current version (e.g. moving renamed fields)
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
