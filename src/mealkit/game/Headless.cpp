#include "mealkit/game/Headless.h"

#if MEALKIT_HAS_NET
#include "mealkit/net/Relay.h"
#include "mealkit/platform/Socket.h"
#endif
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/System.h"

namespace mealkit::game {

#if MEALKIT_HAS_NET
namespace {

// --relay: enters the room (waiting here, there is nothing else to do) and opens the session over it
bool startRelaySession(HeadlessSession& session, const MatchOptions& o) {
    net::RelayRoom room;
    u64 code = 0;
    if (o.mode == MatchOptions::Mode::Host) {
        room.create(o.relayUrl, o.relayGame);
    } else if (net::parseRoomCode(o.joinAddress, code)) {
        room.join(o.relayUrl, code, o.relayGame);
    } else {
        MEALKIT_ERROR("--relay joins by room code, not '%s'", o.joinAddress.c_str());
        return false;
    }
    const f64 deadline = nowSeconds() + 15.0;
    while (room.state() == net::RelayRoom::State::Connecting && nowSeconds() < deadline) {
        room.update();
        sleepSeconds(0.001);
    }
    if (room.state() != net::RelayRoom::State::InRoom) {
        MEALKIT_ERROR("Could not enter a room on the relay %s", o.relayUrl.c_str());
        return false;
    }
    // The transport keeps the connection after the room object goes
    return room.isOwner() ? session.hostWith(room.createTransport(), o.maxPlayers, o.inputDelay)
                          : session.joinWith(room.createTransport(), room.ownerAddress());
}

} // namespace
#endif

bool startHeadlessSession(HeadlessSession& session, const MatchOptions& o, u64 seed) {
#if MEALKIT_HAS_NET
    if (!o.relayUrl.empty() && !o.dedicated && (o.mode == MatchOptions::Mode::Host || o.mode == MatchOptions::Mode::Join))
        return startRelaySession(session, o);
#endif
    switch (o.mode) {
    case MatchOptions::Mode::Offline: return session.startOffline(seed, o.matchData);
#if MEALKIT_HAS_NET
    case MatchOptions::Mode::Host:
        if (!(o.dedicated ? session.hostDedicated(o.port, o.maxPlayers, o.inputDelay, o.loopbackOnly)
                          : session.host(o.port, o.maxPlayers, o.inputDelay, o.loopbackOnly))) {
            MEALKIT_ERROR("Could not host on port %u", o.port);
            return false;
        }
        return true;
    case MatchOptions::Mode::SteamHost:
    case MatchOptions::Mode::SteamJoin: MEALKIT_ERROR("Steam lobbies need a window (not headless)"); return false;
    case MatchOptions::Mode::Join: {
        net::Address address;
        const bool resolved = net::startup() && net::resolveAddress(o.joinAddress.c_str(), o.port, address);
        net::cleanup();
        if (!resolved) {
            MEALKIT_ERROR("Could not resolve '%s'", o.joinAddress.c_str());
            return false;
        }
        if (!session.join(address)) {
            MEALKIT_ERROR("Could not open a UDP socket");
            return false;
        }
        return true;
    }
#else
    case MatchOptions::Mode::Host:
    case MatchOptions::Mode::Join:
    case MatchOptions::Mode::SteamHost:
    case MatchOptions::Mode::SteamJoin: MEALKIT_ERROR("This build has no multiplayer (engine module net is off)"); return false;
#endif
    }
    return false;
}

namespace {

void printStateValues(const reflect::TypeInfo& type, const void* data, const std::string& prefix) {
    for (const reflect::FieldInfo& f : type.fields) {
        const reflect::ValueInfo& v = *f.value;
        const void* at = static_cast<const u8*>(data) + f.offset;
        const std::string path = prefix + f.name;
        if (v.kind == reflect::Kind::Struct && v.type) {
            printStateValues(*v.type, at, path + ".");
        } else if (v.kind == reflect::Kind::Array && v.element && v.element->kind == reflect::Kind::Struct && v.element->type) {
            for (u32 i = 0; i < v.count; ++i) printStateValues(*v.element->type, static_cast<const u8*>(at) + i * v.element->size, path + "[].");
        } else if (v.enumNames.empty() && (v.kind == reflect::Kind::Bool || v.kind == reflect::Kind::Int || v.kind == reflect::Kind::UInt ||
                                           v.kind == reflect::Kind::Float)) {
            const std::string text = v.kind == reflect::Kind::Bool ? (*static_cast<const bool*>(at) ? "1" : "0") : reflect::formatScalar(v, at);
            std::printf("STATE %s %s\n", path.c_str(), text.c_str());
        }
    }
}

} // namespace

void printFinalState(const reflect::TypeInfo& type, const void* state) { printStateValues(type, state, ""); }

void printFinal(u32 tick, u64 hash, bool desynced) {
    std::printf("FINAL tick=%u hash=%016llx desync=%d\n", tick, static_cast<unsigned long long>(hash), desynced ? 1 : 0);
    std::fflush(stdout);
}

} // namespace mealkit::game
