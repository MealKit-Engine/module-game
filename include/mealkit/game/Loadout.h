#pragma once

#include "mealkit/core/Hash.h"
#include "mealkit/core/Reflect.h"
#include "mealkit/game/InputRecord.h"
#include "mealkit/net/Lockstep.h"

#include <cstring>
#include <type_traits>

// 매치 시작 데이터 (ROADMAP 1단계 "매치 시작 데이터"): 플레이어마다 고른 캐릭터·장비처럼 매치에 가져가는 값.
//   게임이 MEALKIT_STRUCT(Loadout)을 선언하고, 화면 쪽이 매치 전(로비)에 game::setLoadout(session, 값)으로 보낸다.
//   호스트가 startMatch 때 모두의 것을 확정해 배포하고, 매치 시작(startWorld)이 game::readLoadout(match, 플레이어, 값)으로 읽어
//   World의 플레이어 상태(상태 필드 → 해시·스냅샷)에 복사한다. 시뮬레이션은 그 복사본만 읽는다. 입력 기록에도 들어간다.
//   memcpy로 보내므로 문자열·컨테이너 없음, 크기는 net::kMaxLoadoutBytes(128) 이하. 앞 4바이트는 타입 이름의 해시.
namespace mealkit::game {

template <class T>
constexpr void checkLoadoutType() {
    static_assert(std::is_trivially_copyable_v<T>, "a loadout is sent with memcpy: no std::string, containers or pointers");
    static_assert(sizeof(T) + 4 <= net::kMaxLoadoutBytes, "loadout too large (net::kMaxLoadoutBytes)");
}

template <class T>
bool setLoadout(net::LockstepSession& session, const T& loadout) {
    checkLoadoutType<T>();
    u8 buffer[4 + sizeof(T)];
    const u32 type = static_cast<u32>(fnv1a(reflect::typeOf<T>().name));
    std::memcpy(buffer, &type, 4);
    std::memcpy(buffer + 4, &loadout, sizeof(T));
    return session.setLocalLoadout(buffer, sizeof(buffer));
}

// 이 플레이어의 로드아웃. 보내지 않았거나 타입이 다르면 false (out은 그대로 = 기본값)
template <class T>
bool readLoadout(const MatchInfo& match, u32 player, T& out) {
    checkLoadoutType<T>();
    if (player >= match.loadouts.size()) return false;
    const std::vector<u8>& bytes = match.loadouts[player];
    if (bytes.size() != 4 + sizeof(T)) return false;
    u32 type = 0;
    std::memcpy(&type, bytes.data(), 4);
    if (type != static_cast<u32>(fnv1a(reflect::typeOf<T>().name))) return false;
    std::memcpy(&out, bytes.data() + 4, sizeof(T));
    return true;
}

// 로비에서 다른 사람이 고른 값 (로비 화면에 보여 주기, net::LockstepSession::lobbyMembers). 매치는 readLoadout으로 읽는다
template <class T>
bool readLobbyLoadout(const net::LobbyMember& member, T& out) {
    checkLoadoutType<T>();
    if (member.loadout.size() != 4 + sizeof(T)) return false;
    u32 type = 0;
    std::memcpy(&type, member.loadout.data(), 4);
    if (type != static_cast<u32>(fnv1a(reflect::typeOf<T>().name))) return false;
    std::memcpy(&out, member.loadout.data() + 4, sizeof(T));
    return true;
}

} // namespace mealkit::game
