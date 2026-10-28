#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/game/BuildInfo.h"
#include "mealkit/net/Lockstep.h"
#include "mealkit/net/Rooms.h"

#include <string>
#include <vector>

namespace mealkit::online {
class Service;
}

// 매치 열기·참가·나가기 (docs/CODING_MODEL.md "타이틀부터 결과까지 화면 흐름"). 엔진(출시 exe, 에디터 플레이, 참가 창)이 하나 갖고
// 락스텝 세션을 그 안에 둔다. 게임의 화면 흐름(app/)은 ctx.match로 이것만 부르고, 세션을 직접 열거나 닫지 않는다:
//   메뉴: playOffline / host / hostOnline(Steam) / join(주소 또는 LAN 방) / joinOnline → 로비
//   로비: ctx.session으로 이름·준비·로드아웃·채팅, 호스트는 start → 매치
//   매치 끝(시뮬레이션이 정한다) → 결과 화면 → 호스트가 returnToLobby → 로비. 누구든 leave → 메뉴
// 명령줄(--host, --join, --steam)은 엔진이 같은 함수로 미리 연다. 요청은 바로 처리하고, 실패하면 false와 error().
namespace mealkit::game {

struct InputRecording; // game/InputRecord.h

enum class MatchPhase : u8 {
    Menu,       // 세션 없음 (타이틀·메인 메뉴)
    Opening,    // 온라인 로비를 만들거나 들어가는 중 (Steam)
    Connecting, // 호스트에 접속 중
    Lobby,      // 매치 전 (준비, 로드아웃, 채팅)
    Running,    // 매치 중
    Ended,      // 끊김 (error()가 이유). leave로 메뉴로
};

struct MatchServiceConfig {
    u64 contentHash = 0;
    bool acceptOtherBuilds = false; // 시험용 (--any-build)
    std::string gameName;           // LAN 방 알림 (같은 게임만 목록에 보인다)
    u16 defaultPort = 27015;        // host·join에서 포트를 주지 않을 때 (게임 프로젝트 설정)
    u32 defaultMaxPlayers = 4;
    u32 inputDelay = 3;          // 틱
    bool loopbackOnly = false;   // 127.0.0.1에만 (같은 PC 시험, 에디터 여러 명 플레이)
    bool announceRooms = true;   // 호스트가 LAN 방 알림을 보낸다 (에디터 플레이는 끈다)
    u64 seed = 0;                // 0이면 매치마다 현재 시각. 주면 첫 매치가 이 값, 다음 매치는 +1 (시험 재현)
    online::Service* online = nullptr; // Steam 로비 (없거나 init 실패면 IP·LAN만)
    net::NetConditions conditions; // 네트워크 상태 흉내 (--net-lag/--net-jitter/--net-loss)
    net::AutoDelay autoDelay;      // 입력 지연 자동 조절 (꺼져 있으면 inputDelay 고정)
    BuildInfo build;        // 이 빌드 (시뮬레이션 버전이 다른 피어는 받지 않는다, 게임 버전은 보이기만)
    u64 contentParts[static_cast<u32>(net::ContentPart::Count)] = {}; // 콘텐츠 해시 구성 (거부 이유)
    std::string playerName; // 로비에 보이는 이름 (비어 있으면 온라인 계정 이름, 그것도 없으면 이 컴퓨터의 사용자 이름)
};

// 참가 메뉴의 LAN 방 하나
struct RoomEntry {
    std::string address;  // join에 그대로 ("IP:포트")
    std::string hostName;
    u32 players = 0, maxPlayers = 0;
    bool compatible = true; // 같은 빌드·같은 게임 데이터 (아니면 접속이 거부된다)
    bool sameBuild = true;  // 같은 시뮬레이션 버전·결정론 그룹
    std::string version;    // 방장의 게임 버전
};

class MatchService {
public:
    MatchService() = default;
    ~MatchService();
    MatchService(const MatchService&) = delete;
    MatchService& operator=(const MatchService&) = delete;

    void init(const MatchServiceConfig& config);
    const MatchServiceConfig& config() const { return m_config; }
    net::LockstepSession& session() { return m_session; }
    const net::LockstepSession& session() const { return m_session; }

    // 메뉴에서 (세션이 없을 때만). 실패하면 false, 이유는 error()
    bool playOffline();                                // 혼자: 로비부터 (로드아웃을 고르고 start)
    bool host(u16 port = 0, u32 maxPlayers = 0);       // IP·LAN 방 (0 = 기본값)
    bool hostOnline(u32 maxPlayers = 0);               // 온라인 친구 로비 (Steam). onlineAvailable일 때만
    bool join(const std::string& address);             // "주소[:포트]" (LAN 방 목록의 address도 이것)
    bool joinOnline(u64 lobbyId);
    // 저장된 리플레이를 바로 매치로 (네트워크 없음, 내 입력은 버린다). 매치가 끝나 returnToLobby하면 메뉴로 (game/InputRecord.h)
    bool playReplay(const InputRecording& recording);
    bool isReplay() const { return m_session.isReplay(); }
    // 로비에서 (호스트). matchData는 게임이 정하는 매치 설정값
    bool start(u32 matchData = 0);
    // 매치 뒤 (호스트): 모두 로비로
    void returnToLobby();
    // 어디서든: 세션을 닫고 메뉴로
    void leave();

    bool onlineAvailable() const;
    bool usesOnline() const { return m_online; }
    void inviteFriends(); // 온라인 로비의 친구 초대 창

    MatchPhase phase() const;
    const std::string& error() const { return m_error; }
    bool isHost() const { return m_session.isHost(); }
    bool isOffline() const { return m_session.isOffline(); }
    u16 port() const { return m_port; }

    // 로비에서 핑이 입력 지연 상한보다 큰 사람 (비트 = 플레이어 번호): 매치 중 멈칫거린다. 화면이 경고한다
    u32 slowPlayers() const;

    // LAN 방 찾기: 참가 메뉴가 열려 있는 동안 켠다
    void setBrowsing(bool on);
    bool browsing() const { return m_finder.isOpen(); }
    const std::vector<RoomEntry>& rooms() const { return m_rooms; }

    // 매 프레임 (게임 update 전): 세션, 온라인 로비, LAN 방 알림·찾기
    void update(f64 now);
    void shutdown();

private:
    bool fail(const std::string& message);
    u64 nextSeed();

    MatchServiceConfig m_config;
    net::LockstepSession m_session;
    net::RoomAnnouncer m_announcer;
    net::RoomFinder m_finder;
    std::vector<RoomEntry> m_rooms;
    std::string m_error;
    u16 m_port = 0;
    u32 m_maxPlayers = 0;
    bool m_online = false;      // 이 세션이 온라인 로비를 쓴다
    bool m_onlineHost = false;
    f64 m_now = 0.0;
};

} // namespace mealkit::game
