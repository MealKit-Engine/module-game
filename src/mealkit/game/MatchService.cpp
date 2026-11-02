#include "mealkit/game/MatchService.h"

#include "mealkit/core/Log.h"
#include "mealkit/game/InputRecord.h"
#include "mealkit/net/Socket.h"
#include "mealkit/online/Online.h"
#include "mealkit/platform/System.h"

#include <algorithm>
#include <ctime>

namespace mealkit::game {

MatchService::~MatchService() { shutdown(); }

void MatchService::init(const MatchServiceConfig& config) {
    shutdown();
    m_config = config;
    m_session.reset();
    m_session.setContentHash(config.contentHash);
    m_session.setAcceptOtherBuilds(config.acceptOtherBuilds);
    m_session.setSameInputOnly(config.sameInputOnly);
    m_session.setConditions(config.conditions);
    m_session.setAutoDelay(config.autoDelay);
    m_session.setContentParts(config.contentParts);
    m_session.setBuild(config.build.simVersion, config.build.gameVersion);
    std::string name = config.playerName;
    if (name.empty() && config.online && config.online->available()) name = config.online->displayName();
    if (name.empty()) name = userName();
    m_session.setLocalName(name);
    m_error.clear();
}

bool MatchService::fail(const std::string& message) {
    m_error = message;
    MEALKIT_WARN("%s", message.c_str());
    m_session.reset();
    m_online = false;
    return false;
}

u64 MatchService::nextSeed() {
    const u64 index = m_session.matchIndex(); // count up to just before the match starting now
    if (m_config.seed) return m_config.seed + index;
    return (static_cast<u64>(std::time(nullptr)) << 8) ^ index;
}

bool MatchService::playOffline() {
    if (m_session.state() != net::SessionState::Idle) return false;
    m_error.clear();
    m_online = false;
    return m_session.openOffline();
}

bool MatchService::playReplay(const InputRecording& recording) {
    if (m_session.state() != net::SessionState::Idle) return false;
    m_error.clear();
    m_online = false;
    return m_session.startReplay(recording.match.seed, recording.match.matchData, recording.match.loadouts,
                                 recording.inputs, recording.hashes);
}

bool MatchService::host(u16 port, u32 maxPlayers) {
    if (m_session.state() != net::SessionState::Idle) return false;
    m_error.clear();
    m_online = false;
    m_port = port ? port : m_config.defaultPort;
    m_maxPlayers = maxPlayers ? maxPlayers : m_config.defaultMaxPlayers;
    if (!m_session.host(m_port, m_maxPlayers, m_config.inputDelay, m_config.loopbackOnly))
        return fail("Could not host on port " + std::to_string(m_port) + " (is another game using it?)");
    if (m_config.announceRooms) m_announcer.open(!m_config.loopbackOnly); // with --local, same PC only
    return true;
}

bool MatchService::hostOnline(u32 maxPlayers) {
    if (m_session.state() != net::SessionState::Idle || !onlineAvailable()) return false;
    m_error.clear();
    m_online = true;
    m_onlineHost = true;
    m_maxPlayers = maxPlayers ? maxPlayers : m_config.defaultMaxPlayers;
    m_config.online->createLobby(m_maxPlayers); // when ready, update opens the session
    return true;
}

bool MatchService::join(const std::string& address) {
    if (m_session.state() != net::SessionState::Idle) return false;
    m_error.clear();
    m_online = false;
    net::Address to;
    const bool resolved = net::startup() && net::resolveAddress(address.c_str(), m_config.defaultPort, to);
    net::cleanup();
    if (!resolved) return fail("Could not find the address '" + address + "'");
    if (!m_session.join(to)) return fail("Could not open a UDP socket");
    return true;
}

bool MatchService::joinOnline(u64 lobbyId) {
    if (m_session.state() != net::SessionState::Idle || !onlineAvailable()) return false;
    m_error.clear();
    m_online = true;
    m_onlineHost = false;
    m_config.online->joinLobby(lobbyId);
    return true;
}

bool MatchService::start(u32 matchData) {
    if (!m_session.isHost() || m_session.state() != net::SessionState::Lobby) return false;
    m_session.startMatch(nextSeed(), matchData);
    return true;
}

void MatchService::returnToLobby() {
    if (m_session.isReplay()) leave(); // replays have no lobby
    else m_session.returnToLobby();
}

void MatchService::leave() {
    if (m_online && m_config.online) { // when the host leaves, lobby ownership also goes to the lockstep successor (so Steam doesn't pick someone else)
        if (m_session.isHost()) m_config.online->setLobbyOwner(m_session.successorAddress().userId);
        m_config.online->leaveLobby();
    }
    m_online = false;
    m_announcer.close();
    m_session.reset();
    m_error.clear();
}

bool MatchService::onlineAvailable() const { return m_config.online && m_config.online->available(); }

void MatchService::inviteFriends() {
    if (m_online && m_config.online) m_config.online->inviteFriends();
}

MatchPhase MatchService::phase() const {
    switch (m_session.state()) {
    case net::SessionState::Idle: return m_online ? MatchPhase::Opening : MatchPhase::Menu;
    case net::SessionState::Connecting: return MatchPhase::Connecting;
    case net::SessionState::Lobby: return MatchPhase::Lobby;
    case net::SessionState::Running: return MatchPhase::Running;
    case net::SessionState::Ended: return MatchPhase::Ended;
    }
    return MatchPhase::Menu;
}

u32 MatchService::slowPlayers() const {
    if (!m_config.autoDelay.enabled) return 0;
    u32 mask = 0;
    for (const net::LobbyMember& m : m_session.lobbyMembers())
        if (m.player < 32 && static_cast<f32>(m.pingMs) * 1.25f + m_config.autoDelay.tickMs >
                                 m_config.autoDelay.tickMs * static_cast<f32>(m_config.autoDelay.maxTicks))
            mask |= 1u << m.player;
    return mask;
}

void MatchService::setBrowsing(bool on) {
    if (on == m_finder.isOpen()) return;
    if (on) m_finder.open(m_config.gameName);
    else {
        m_finder.close();
        m_rooms.clear();
    }
}

void MatchService::update(f64 now) {
    m_now = now;
    online::Service* online = m_config.online;
    if (online && online->available()) {
        online->update();
        if (m_online && m_session.state() == net::SessionState::Idle) {
            if (online->lobbyState() == online::LobbyState::Failed) {
                fail("The online lobby failed (see log)");
            } else if (online->lobbyState() == online::LobbyState::InLobby) { // lockstep session to the host's address (platform relay)
                if (online->isLobbyOwner()) {
                    m_session.hostWith(online->createTransport(), m_maxPlayers, m_config.inputDelay);
                    online->inviteFriends();
                } else {
                    m_session.joinWith(online->createTransport(), online->lobbyOwnerAddress());
                }
            }
        }
        if (const u64 invited = online->takeJoinRequest()) { // accepted friend invite: join right away if at the menu
            if (phase() == MatchPhase::Menu) joinOnline(invited);
            else MEALKIT_INFO("Invite to lobby %llu received - leave the current match to join", static_cast<unsigned long long>(invited));
        }
    }

    m_session.update(now);
    if (m_session.state() == net::SessionState::Ended && m_error.empty()) m_error = m_session.endReason();
    // After host migration: if the platform chose me as lobby owner but the lockstep host is someone else, hand it to them
    if (m_online && online && online->isLobbyOwner() && !m_session.isHost() && m_session.hostAddress().userId)
        online->setLobbyOwner(m_session.hostAddress().userId);

    // LAN room announcements: during the lobby of a room opened by IP
    if (m_announcer.isOpen()) {
        if (m_session.isHost() && m_session.state() == net::SessionState::Lobby) {
            net::RoomAnnouncement room;
            room.game = m_config.gameName;
            const std::vector<net::LobbyMember>& members = m_session.lobbyMembers();
            room.hostName = members.empty() ? std::string() : members.front().name;
            room.simVersion = config().build.simVersion ? config().build.simVersion : net::buildId();
            room.version = config().build.gameVersion;
            room.group = net::determinismGroup();
            room.platform = net::platformName();
            room.crossplay = m_session.crossplay();
            room.contentHash = m_config.contentHash;
            room.port = m_port;
            room.players = static_cast<u8>(m_session.lobbyPlayerCount());
            room.maxPlayers = static_cast<u8>(m_maxPlayers);
            m_announcer.update(now, room);
        } else if (m_session.state() != net::SessionState::Running) {
            m_announcer.close();
        }
    }
    if (m_finder.isOpen()) {
        m_finder.update(now);
        m_rooms.clear();
        for (const net::Room& r : m_finder.rooms()) {
            // Crossplay off on either side: rooms on another OS aren't shown (joining would be rejected)
            if (r.platform != net::platformName() && (!r.crossplay || !m_session.crossplay())) continue;
            RoomEntry e;
            e.address = r.address.toString();
            e.hostName = r.hostName;
            e.players = r.players;
            e.maxPlayers = r.maxPlayers;
            e.version = r.version;
            e.sameBuild = r.simVersion == (m_config.build.simVersion ? m_config.build.simVersion : net::buildId()) &&
                          r.group == net::determinismGroup();
            e.compatible = e.sameBuild && r.contentHash == m_config.contentHash;
            m_rooms.push_back(std::move(e));
        }
    }
}

void MatchService::shutdown() {
    if (m_online && m_config.online) {
        if (m_session.isHost()) m_config.online->setLobbyOwner(m_session.successorAddress().userId);
        m_config.online->leaveLobby();
    }
    m_online = false;
    m_announcer.close();
    m_finder.close();
    m_rooms.clear();
    m_session.shutdown();
}

} // namespace mealkit::game
