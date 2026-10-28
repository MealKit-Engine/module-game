#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/core/TextFormat.h"
#include "mealkit/core/Types.h"
#include "mealkit/game/ProjectSettings.h"
#include "mealkit/platform/Paths.h"

#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// 디싱크 덤프 (docs/ROADMAP.md 3단계 "디싱크 진단 도구"): 호스트가 틱 해시가 다르다고 알리면 피어마다 그 틱의 상태를 텍스트로 쓴다.
// game::Simulation이 여러 명 매치에서 스냅샷을 몇 개 남겨 두었다가, 갈라진 틱 앞의 스냅샷에서 그 틱까지 다시 돌려 덤프한다
// (호스트는 몇 틱 늦게 알아채므로 지금 상태가 아니라 갈라진 틱의 상태).
// 두 피어의 파일을 tools\desync_diff.ps1로 비교하면 처음 다른 틱, 다른 상태 필드, 다른 바디가 나온다.
//
// 형식 (텍스트, 줄 단위):
//   # MealKit desync dump
//   @format desync 1
//   player 1 / tick 812 / seed 000000.. / content 000000.. / note <설명>   머리 (키 값)
//   [hashes]  "<틱> <해시16진>" (갈라진 틱까지 남아 있는 것)
//   [state]   "<필드 경로> = <값>" (선언한 상태 필드, reflect::toText)
//   [bodies]  physics::PhysicsWorld::writeState 줄
namespace mealkit::game {

inline constexpr const char* kDesyncFolder = "desync";

struct DesyncDumpHeader {
    u32 player = 0;
    u32 tick = 0;
    u64 seed = 0;
    u64 contentHash = 0;
    std::string note; // 비어 있지 않으면 덤프가 불완전한 이유 등
};

// 덤프 폴더: 개발 중(프로젝트 폴더에 build\가 있다)이면 <프로젝트>/build/desync, 출시본은 사용자 폴더의 desync/
inline std::string desyncDumpFolder(const std::string& userFolder) {
    const std::string projectFile = findAsset(kProjectFileName);
    const std::string project = projectFile.empty() ? std::string() : parentDirectory(projectFile);
    if (!project.empty() && directoryExists(project + "/build")) return project + "/build/" + kDesyncFolder;
    return userFolder.empty() ? std::string() : userFolder + "/" + kDesyncFolder;
}

inline std::string desyncDumpPath(const std::string& folder, u64 seed, u32 tick, u32 player) {
    char name[96];
    std::snprintf(name, sizeof(name), "desync_%016llx_t%u_p%u.txt", static_cast<unsigned long long>(seed), tick,
                  player + 1);
    return folder + "/" + name;
}

// world: 갈라진 틱을 실행한 직후의 상태. hashes: (틱, 해시) 오래된 것부터
template <class World>
bool writeDesyncDump(const std::string& path, const DesyncDumpHeader& h, const World& world,
                     const std::vector<std::pair<u32, u64>>& hashes) {
    std::string out = "# MealKit desync dump. Compare two peers with tools\\desync_diff.ps1 -A <file> -B <file>\n";
    out += text::formatLine(text::kDesync) + "\n";
    char line[160];
    std::snprintf(line, sizeof(line), "player %u\ntick %u\nseed %016llx\ncontent %016llx\n", h.player + 1, h.tick,
                  static_cast<unsigned long long>(h.seed), static_cast<unsigned long long>(h.contentHash));
    out += line;
    if (!h.note.empty()) out += "note " + h.note + "\n";
    out += "[hashes]\n";
    for (const auto& [tick, hash] : hashes) {
        std::snprintf(line, sizeof(line), "%u %016llx\n", tick, static_cast<unsigned long long>(hash));
        out += line;
    }
    out += "[state]\n";
    reflect::TextFields fields;
    reflect::toText(reflect::typeOf<std::remove_cvref_t<decltype(world.state)>>(), &world.state, fields);
    for (const auto& [key, value] : fields) out += key + " = " + value + "\n";
    out += "[bodies]\n";
    world.physics.writeState(out);
    if (!parentDirectory(path).empty()) createDirectories(parentDirectory(path));
    return writeTextFile(path, out);
}

} // namespace mealkit::game
