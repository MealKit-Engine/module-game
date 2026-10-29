#pragma once

#include "mealkit/core/Hash.h"
#include "mealkit/core/Reflect.h"
#include "mealkit/game/InputRecord.h"
#include "mealkit/net/Lockstep.h"

#include <cstring>
#include <type_traits>

// Match start data (ROADMAP stage 1 "Match start data"): values taken into the match per player, like the chosen character/gear.
//   The game declares MEALKIT_STRUCT(Loadout), and the view side sends it before the match (lobby) with game::setLoadout(session, value).
//   The host confirms and distributes everyone's at startMatch, and match start (startWorld) reads it with game::readLoadout(match, player, value)
//   and copies it into the World's player state (state fields → hash/snapshots). The simulation only reads that copy. It also goes into input recordings.
//   Sent via memcpy, so no strings/containers, size up to net::kMaxLoadoutBytes (128). The first 4 bytes are the type name hash.
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

// This player's loadout. false if not sent or the type differs (out unchanged = defaults)
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

// Values others picked in the lobby (to show on the lobby screen, net::LockstepSession::lobbyMembers). The match reads them with readLoadout
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
