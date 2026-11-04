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
