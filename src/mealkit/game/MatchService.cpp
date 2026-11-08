#include "mealkit/game/MatchService.h"

#include "mealkit/core/Log.h"
#include "mealkit/game/InputRecord.h"
#include "mealkit/platform/System.h"
#if MEALKIT_HAS_NET
#include "mealkit/online/Online.h"
#include "mealkit/platform/Socket.h"
#endif

#include <algorithm>
#include <ctime>

namespace mealkit::game {

MatchService::~MatchService() { shutdown(); }

void MatchService::init(const MatchServiceConfig& config) {
    shutdown();
    m_config = config;
    std::string name = config.playerName;
#if MEALKIT_HAS_NET
    if (name.empty() && config.online && config.online->available()) name = config.online->displayName();
    m_net.reset();
    m_net.setContentHash(config.contentHash);
    m_net.setAcceptOtherBuilds(config.acceptOtherBuilds);
    m_net.setSameInputOnly(config.sameInputOnly);
    m_net.setConditions(config.conditions);
    m_net.setAutoDelay(config.autoDelay);
    m_net.setContentParts(config.contentParts);
    m_net.setBuild(config.build.simVersion, config.build.gameVersion);
    if (name.empty()) name = userName();
    m_net.setLocalName(name);
#else
    if (name.empty()) name = userName();
#endif
    m_local.reset();
    m_local.setContentHash(config.contentHash);
    m_local.setLocalName(name);
    m_active.setTarget(m_local);
    m_error.clear();
}

void MatchService::resetSessions() {
#if MEALKIT_HAS_NET
    m_net.reset();
#endif
    m_local.reset();
    m_active.setTarget(m_local);
}

void MatchService::setCrossplay(bool allowed) {
#if MEALKIT_HAS_NET
    m_net.setCrossplay(allowed);
#endif
    m_local.setCrossplay(allowed);
}

void MatchService::setLocalInputKind(session::InputKind kind) {
#if MEALKIT_HAS_NET
    m_net.setLocalInputKind(kind);
#endif
    m_local.setLocalInputKind(kind);
}

bool MatchService::fail(const std::string& message) {
    m_error = message;
    MEALKIT_WARN("%s", message.c_str());
    resetSessions();
    m_online = false;
    return false;
}

u64 MatchService::nextSeed() {
    const u64 index = m_active.matchIndex(); // count up to just before the match starting now
    if (m_config.seed) return m_config.seed + index;
    return (static_cast<u64>(std::time(nullptr)) << 8) ^ index;
}

bool MatchService::playOffline() {
    if (m_active.state() != session::SessionState::Idle) return false;
    m_error.clear();
    m_online = false;
    m_active.setTarget(m_local);
    return m_local.openOffline();
}

bool MatchService::playReplay(const InputRecording& recording) {
    if (m_active.state() != session::SessionState::Idle) return false;
    m_error.clear();
    m_online = false;
    m_active.setTarget(m_local);
    return m_local.startReplay(recording.match.seed, recording.match.matchData, recording.match.loadouts,
                                 recording.inputs, recording.hashes);
}

#if MEALKIT_HAS_NET
bool MatchService::host(u16 port, u32 maxPlayers) {
    if (m_active.state() != session::SessionState::Idle) return false;
    m_error.clear();
    m_online = false;
    useNetwork();
    m_port = port ? port : m_config.defaultPort;
    m_maxPlayers = maxPlayers ? maxPlayers : m_config.defaultMaxPlayers;
    if (!m_net.host(m_port, m_maxPlayers, m_config.inputDelay, m_config.loopbackOnly))
        return fail("Could not host on port " + std::to_string(m_port) + " (is another game using it?)");
    if (m_config.announceRooms) m_announcer.open(!m_config.loopbackOnly); // with --local, same PC only
    return true;
}

bool MatchService::hostOnline(u32 maxPlayers) {
    if (m_active.state() != session::SessionState::Idle || !onlineAvailable()) return false;
    m_error.clear();
    m_online = true;
    m_onlineHost = true;
    useNetwork();
    m_maxPlayers = maxPlayers ? maxPlayers : m_config.defaultMaxPlayers;
    m_config.online->createLobby(m_maxPlayers); // when ready, update opens the session
    return true;
}

bool MatchService::join(const std::string& address) {
    if (m_active.state() != session::SessionState::Idle) return false;
    m_error.clear();
    m_online = false;
    useNetwork();
    net::Address to;
    const bool resolved = net::startup() && net::resolveAddress(address.c_str(), m_config.defaultPort, to);
    net::cleanup();
    if (!resolved) return fail("Could not find the address '" + address + "'");
    if (!m_net.join(to)) return fail("Could not open a UDP socket");
    return true;
}

bool MatchService::joinOnline(u64 lobbyId) {
    if (m_active.state() != session::SessionState::Idle || !onlineAvailable()) return false;
    m_error.clear();
    m_online = true;
    m_onlineHost = false;
    useNetwork();
    m_config.online->joinLobby(lobbyId);
    return true;
}

#else
// Built without the net module (preset solo): the menu's network entries report it instead of failing to build
bool MatchService::host(u16, u32) { return fail("This build has no multiplayer (engine module net is off)"); }
bool MatchService::hostOnline(u32) { return false; }
bool MatchService::join(const std::string&) { return fail("This build has no multiplayer (engine module net is off)"); }
bool MatchService::joinOnline(u64) { return false; }
#endif

bool MatchService::start(u32 matchData) {
    if (!m_active.isHost() || m_active.state() != session::SessionState::Lobby) return false;
    m_active.startMatch(nextSeed(), matchData);
    return true;
}

void MatchService::returnToLobby() {
    if (m_active.isReplay()) leave(); // replays have no lobby
    else m_active.returnToLobby();
}

void MatchService::leave() {
#if MEALKIT_HAS_NET
    if (m_online && m_config.online) { // when the host leaves, lobby ownership also goes to the lockstep successor (so Steam doesn't pick someone else)
        if (m_net.isHost()) m_config.online->setLobbyOwner(m_net.successorAddress().userId);
        m_config.online->leaveLobby();
    }
    m_announcer.close();
#endif
    m_online = false;
    resetSessions();
    m_error.clear();
}

#if MEALKIT_HAS_NET
bool MatchService::onlineAvailable() const { return m_config.online && m_config.online->available(); }

void MatchService::inviteFriends() {
    if (m_online && m_config.online) m_config.online->inviteFriends();
}
#else
bool MatchService::onlineAvailable() const { return false; }
void MatchService::inviteFriends() {}
#endif

MatchPhase MatchService::phase() const {
    switch (m_active.state()) {
    case session::SessionState::Idle: return m_online ? MatchPhase::Opening : MatchPhase::Menu;
    case session::SessionState::Connecting: return MatchPhase::Connecting;
    case session::SessionState::Lobby: return MatchPhase::Lobby;
    case session::SessionState::Running: return MatchPhase::Running;
    case session::SessionState::Ended: return MatchPhase::Ended;
    }
    return MatchPhase::Menu;
}

u32 MatchService::slowPlayers() const {
    if (!m_config.autoDelay.enabled) return 0;
    u32 mask = 0;
    for (const session::LobbyMember& m : m_active.lobbyMembers())
        if (m.player < 32 && static_cast<f32>(m.pingMs) * 1.25f + m_config.autoDelay.tickMs >
                                 m_config.autoDelay.tickMs * static_cast<f32>(m_config.autoDelay.maxTicks))
            mask |= 1u << m.player;
    return mask;
}

#if MEALKIT_HAS_NET
bool MatchService::browsing() const { return m_finder.isOpen(); }

void MatchService::setBrowsing(bool on) {
    if (on == m_finder.isOpen()) return;
    if (on) m_finder.open(m_config.gameName);
    else {
        m_finder.close();
        m_rooms.clear();
    }
}

#else
bool MatchService::browsing() const { return false; }
void MatchService::setBrowsing(bool) {}
#endif

void MatchService::update(f64 now) {
    m_now = now;
    m_local.update(now);
#if !MEALKIT_HAS_NET
    if (m_active.state() == session::SessionState::Ended && m_error.empty()) m_error = m_active.endReason();
#else
    online::Service* online = m_config.online;
    if (online && online->available()) {
        online->update();
        if (m_online && m_net.state() == session::SessionState::Idle) {
            if (online->lobbyState() == online::LobbyState::Failed) {
                fail("The online lobby failed (see log)");
            } else if (online->lobbyState() == online::LobbyState::InLobby) { // lockstep session to the host's address (platform relay)
                if (online->isLobbyOwner()) {
                    m_net.hostWith(online->createTransport(), m_maxPlayers, m_config.inputDelay);
                    online->inviteFriends();
                } else {
                    m_net.joinWith(online->createTransport(), online->lobbyOwnerAddress());
                }
            }
        }
        if (const u64 invited = online->takeJoinRequest()) { // accepted friend invite: join right away if at the menu
            if (phase() == MatchPhase::Menu) joinOnline(invited);
            else MEALKIT_INFO("Invite to lobby %llu received - leave the current match to join", static_cast<unsigned long long>(invited));
        }
    }

    m_net.update(now);
    if (m_active.state() == session::SessionState::Ended && m_error.empty()) m_error = m_active.endReason();
    // After host migration: if the platform chose me as lobby owner but the lockstep host is someone else, hand it to them
    if (m_online && online && online->isLobbyOwner() && !m_net.isHost() && m_net.hostAddress().userId)
        online->setLobbyOwner(m_net.hostAddress().userId);

    // LAN room announcements: during the lobby of a room opened by IP
    if (m_announcer.isOpen()) {
        if (m_net.isHost() && m_net.state() == session::SessionState::Lobby) {
            net::RoomAnnouncement room;
            room.game = m_config.gameName;
            const std::vector<session::LobbyMember>& members = m_net.lobbyMembers();
            room.hostName = members.empty() ? std::string() : members.front().name;
            room.simVersion = config().build.simVersion ? config().build.simVersion : session::buildId();
            room.version = config().build.gameVersion;
            room.group = session::determinismGroup();
            room.platform = session::platformName();
            room.crossplay = m_net.crossplay();
            room.contentHash = m_config.contentHash;
            room.port = m_port;
            room.players = static_cast<u8>(m_net.lobbyPlayerCount());
            room.maxPlayers = static_cast<u8>(m_maxPlayers);
            m_announcer.update(now, room);
        } else if (m_net.state() != session::SessionState::Running) {
            m_announcer.close();
        }
    }
    if (m_finder.isOpen()) {
        m_finder.update(now);
        m_rooms.clear();
        for (const net::Room& r : m_finder.rooms()) {
            // Crossplay off on either side: rooms on another OS aren't shown (joining would be rejected)
            if (r.platform != session::platformName() && (!r.crossplay || !m_net.crossplay())) continue;
            RoomEntry e;
            e.address = r.address.toString();
            e.hostName = r.hostName;
            e.players = r.players;
            e.maxPlayers = r.maxPlayers;
            e.version = r.version;
            e.sameBuild = r.simVersion == (m_config.build.simVersion ? m_config.build.simVersion : session::buildId()) &&
                          r.group == session::determinismGroup();
            e.compatible = e.sameBuild && r.contentHash == m_config.contentHash;
            m_rooms.push_back(std::move(e));
        }
    }
#endif
}

void MatchService::shutdown() {
#if MEALKIT_HAS_NET
    if (m_online && m_config.online) {
        if (m_net.isHost()) m_config.online->setLobbyOwner(m_net.successorAddress().userId);
        m_config.online->leaveLobby();
    }
    m_online = false;
    m_announcer.close();
    m_finder.close();
    m_rooms.clear();
    m_net.shutdown();
#endif
    m_local.reset();
    m_active.setTarget(m_local);
}

} // namespace mealkit::game
