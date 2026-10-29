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

// Open/join/leave matches (docs/CODING_MODEL.md "Screen flow from title to results"). The engine (shipping exe, editor play, join windows) has one
// and keeps the lockstep session inside it. The game's screen flow (app/) only calls this via ctx.match and never opens or closes the session directly:
//   menu: playOffline / host / hostOnline (Steam) / join (address or LAN room) / joinOnline → lobby
//   lobby: names/ready/loadout/chat via ctx.session, host calls start → match
//   match end (decided by the simulation) → results screen → host calls returnToLobby → lobby. Anyone calls leave → menu
// Command lines (--host, --join, --steam) are opened in advance by the engine with the same functions. Requests are handled immediately; on failure, false and error().
namespace mealkit::game {

struct InputRecording; // game/InputRecord.h

enum class MatchPhase : u8 {
    Menu,       // no session (title/main menu)
    Opening,    // creating or entering an online lobby (Steam)
    Connecting, // connecting to the host
    Lobby,      // before the match (ready, loadout, chat)
    Running,    // in a match
    Ended,      // disconnected (error() is the reason). leave to go to the menu
};

struct MatchServiceConfig {
    u64 contentHash = 0;
    bool acceptOtherBuilds = false; // for tests (--any-build)
    std::string gameName;           // LAN room announcements (only the same game shows in the list)
    u16 defaultPort = 27015;        // when host/join don't give a port (game project setting)
    u32 defaultMaxPlayers = 4;
    u32 inputDelay = 3;          // ticks
    bool loopbackOnly = false;   // 127.0.0.1 only (same-PC tests, editor multiplayer play)
    bool announceRooms = true;   // the host sends LAN room announcements (off for editor play)
    u64 seed = 0;                // if 0, the current time per match. If given, the first match uses this value, the next +1 (test reproduction)
    online::Service* online = nullptr; // Steam lobby (IP/LAN only if absent or init failed)
    net::NetConditions conditions; // network condition simulation (--net-lag/--net-jitter/--net-loss)
    net::AutoDelay autoDelay;      // automatic input delay adjustment (inputDelay fixed if off)
    BuildInfo build;        // this build (peers with a different simulation version are rejected; the game version is only shown)
    u64 contentParts[static_cast<u32>(net::ContentPart::Count)] = {}; // content hash components (rejection reasons)
    std::string playerName; // name shown in the lobby (if empty, the online account name, or failing that this computer's user name)
};

// One LAN room in the join menu
struct RoomEntry {
    std::string address;  // passed to join as is ("IP:port")
    std::string hostName;
    u32 players = 0, maxPlayers = 0;
    bool compatible = true; // same build and game data (otherwise the connection is rejected)
    bool sameBuild = true;  // same simulation version and determinism group
    std::string version;    // the host's game version
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

    // From the menu (only when there's no session). false on failure, reason in error()
    bool playOffline();                                // solo: starts at the lobby (pick a loadout and start)
    bool host(u16 port = 0, u32 maxPlayers = 0);       // IP/LAN room (0 = default)
    bool hostOnline(u32 maxPlayers = 0);               // online friends lobby (Steam). Only when onlineAvailable
    bool join(const std::string& address);             // "address[:port]" (the address from the LAN room list too)
    bool joinOnline(u64 lobbyId);
    // A saved replay straight into a match (no network, my input is discarded). When the match ends and returnToLobby is called, back to the menu (game/InputRecord.h)
    bool playReplay(const InputRecording& recording);
    bool isReplay() const { return m_session.isReplay(); }
    // In the lobby (host). matchData is the match setting decided by the game
    bool start(u32 matchData = 0);
    // After the match (host): everyone back to the lobby
    void returnToLobby();
    // Anywhere: close the session and go to the menu
    void leave();

    bool onlineAvailable() const;
    bool usesOnline() const { return m_online; }
    void inviteFriends(); // friend invite window of the online lobby

    MatchPhase phase() const;
    const std::string& error() const { return m_error; }
    bool isHost() const { return m_session.isHost(); }
    bool isOffline() const { return m_session.isOffline(); }
    u16 port() const { return m_port; }

    // People in the lobby whose ping exceeds the input delay cap (bit = player index): they'll stutter during the match. The screen warns
    u32 slowPlayers() const;

    // LAN room search: on while the join menu is open
    void setBrowsing(bool on);
    bool browsing() const { return m_finder.isOpen(); }
    const std::vector<RoomEntry>& rooms() const { return m_rooms; }

    // Every frame (before game update): session, online lobby, LAN room announce/search
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
    bool m_online = false;      // this session uses an online lobby
    bool m_onlineHost = false;
    f64 m_now = 0.0;
};

} // namespace mealkit::game
