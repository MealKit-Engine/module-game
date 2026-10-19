#include "mealkit/game/Headless.h"

#include "mealkit/net/Socket.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/System.h"

namespace mealkit::game {

bool startHeadlessSession(net::LockstepSession& session, const MatchOptions& o, u64 seed) {
    switch (o.mode) {
    case MatchOptions::Mode::Offline: return session.startOffline(seed, o.matchData);
    case MatchOptions::Mode::Host:
        if (!session.host(o.port, o.maxPlayers, o.inputDelay, o.loopbackOnly)) {
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
    }
    return false;
}

void printFinal(u32 tick, u64 hash, bool desynced) {
    std::printf("FINAL tick=%u hash=%016llx desync=%d\n", tick, static_cast<unsigned long long>(hash), desynced ? 1 : 0);
    std::fflush(stdout);
}

} // namespace mealkit::game
