#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/game/ProjectSettings.h"
#include "mealkit/net/Lockstep.h"
#include "mealkit/scene/Scene.h"

#include <string>

namespace mealkit::asset {
class ModelCache;
}

// 매치 데이터 (씬, 모델, 튜닝): 게임 실행 파일과 에디터가 같은 방법으로 읽고, 같은 콘텐츠 해시를 만든다.
namespace mealkit::game {

inline constexpr const char* kDefaultTuningAsset = "assets/game.tuning";
inline constexpr const char* kViewTuningAsset = "assets/view.tuning";

// 튜닝 파일을 읽는다. path가 비어 있으면 assets/game.tuning. 파일이 없으면 선언의 기본값 그대로 (경고 로그).
// 읽은 파일 경로를 돌려준다 (없으면 빈 문자열).
std::string loadTuning(const reflect::TypeInfo& type, void* data, const std::string& path = {});

// 화면용 튜닝 파일 (assets/view.tuning, 게임의 ViewTuningType): 콘텐츠 해시에 넣지 않고, 파일이 바뀌면 다시 읽는다.
// 게임 실행 파일·참가 창·에디터가 하나씩 갖고 매 프레임 poll()한 뒤 GameContext::viewTuningData로 넘긴다.
// 에디터는 패널에서 고친 값을 바로 쓰고(set), 여러 명 플레이 중에는 참가 창이 읽는 사본 파일을 다시 쓴다.
class LiveTuning {
public:
    // path가 비어 있으면 assets/view.tuning. 파일이 없으면 선언의 기본값.
    void open(const reflect::TypeInfo& type, const std::string& path = {});
    void close();
    // 1초에 몇 번 파일 시각을 보고, 바뀌었으면 다시 읽는다. 다시 읽었으면 true.
    bool poll(f64 now);
    reflect::View view() const { return {m_value.type(), m_value.data()}; }
    const std::string& path() const { return m_path; }

private:
    void reload();

    reflect::Instance m_value;
    std::string m_path;
    unsigned long long m_writeTime = 0;
    f64 m_nextCheck = 0.0;
};

// 매치 데이터에 쓰는 게임 타입 (출시 exe·에디터는 GameModule에서 gameTypes(module), 헤드리스는 게임 클래스에서)
struct GameTypes {
    const scene::TypeList* objectTypes = nullptr;          // 씬 오브젝트 종류 (MEALKIT_OBJECT)
    const reflect::TypeInfo* tuningType = nullptr;         // assets/game.tuning
    const reflect::TypeInfo* projectSettingsType = nullptr; // mealkit.project의 game.* 항목 (없으면 nullptr)
    const reflect::TableList* tables = nullptr;            // 정의 데이터 표 (MEALKIT_TABLE)
};

struct MatchDataOptions {
    std::string scenePath;  // 비어 있으면 프로젝트의 시작 씬 (파일이 없으면 기본 아레나)
    std::string tuningPath; // 비어 있으면 assets/game.tuning
    const scene::Scene* scene = nullptr;     // 이미 있는 씬 (에디터: 편집 중인 씬). 주면 scenePath를 읽지 않는다
    const ProjectFile* project = nullptr;    // 이미 읽은 프로젝트 설정 (에디터). nullptr이면 loadGameProject로 찾는다
};

// 한 매치에 필요한 데이터: 출시 exe·헤드리스·에디터 플레이·참가 창이 모두 loadMatchData 하나로 읽는다.
// 같은 함수가 같은 일곱 가지(씬, 모델, 오브젝트 종류, 튜닝, 프로젝트 설정, 게임 프로젝트 설정, 표)로 콘텐츠 해시를 만들므로
// 실행 방식이 달라도 피어 해시와 입력 기록이 갈리지 않는다.
struct MatchData {
    ProjectFile project;
    reflect::Instance gameProject; // 게임 항목 값 (types.projectSettingsType, 없으면 빈 값)
    scene::Scene scene;            // 원본 (콘텐츠 해시, GameInstance::start). 시뮬레이션은 scene::flattened로 펼쳐 쓴다
    reflect::Instance tuning;      // types.tuningType 값
    u64 contentHash = 0;
    u64 contentParts[static_cast<u32>(net::ContentPart::Count)] = {}; // 구성별 해시 (접속 거부 이유)
};

// 실패하면(주어진 씬 파일을 읽을 수 없음) 로그를 남기고 false. models는 해시에 쓸 모델 파일을 읽는다 (CPU 데이터만이어도 된다).
bool loadMatchData(const GameTypes& types, const MatchDataOptions& options, asset::ModelCache& models, MatchData& out);

// 접속할 때 비교하는 해시: 씬(게임 결과에 영향을 주는 부분) + 모델 파일 + 튜닝 값
// + 프로젝트 설정(엔진 ProjectSettings와 게임 항목 중 MEALKIT_VIEW_FIELD가 아닌 것) + 시뮬레이션 표(kTableView가 아닌 MEALKIT_TABLE).
u64 contentHash(const scene::Scene& scene, asset::ModelCache& models, const scene::TypeList* gameTypes,
                const reflect::TypeInfo* tuningType, const void* tuning, reflect::View project = {},
                reflect::View gameProject = {}, const reflect::TableList* tables = nullptr);

} // namespace mealkit::game
