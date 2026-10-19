#include "mealkit/game/MatchService.h"

#include "mealkit/core/Log.h"
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
    m_session.setConditions(config.conditions);
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
    const u64 index = m_session.matchIndex(); // 이번에 시작할 매치 앞까지의 수
    if (m_config.seed) return m_config.seed + index;
    return (static_cast<u64>(std::time(nullptr)) << 8) ^ index;
}

bool MatchService::playOffline() {
    if (m_session.state() != net::SessionState::Idle) return false;
    m_error.clear();
    m_online = false;
    return m_session.openOffline();
}

bool MatchService::host(u16 port, u32 maxPlayers) {
    if (m_session.state() != net::SessionState::Idle) return false;
    m_error.clear();
    m_online = false;
    m_port = port ? port : m_config.defaultPort;
    m_maxPlayers = maxPlayers ? maxPlayers : m_config.defaultMaxPlayers;
    if (!m_session.host(m_port, m_maxPlayers, m_config.inputDelay, m_config.loopbackOnly))
        return fail("Could not host on port " + std::to_string(m_port) + " (is another game using it?)");
    if (m_config.announceRooms) m_announcer.open(!m_config.loopbackOnly); // --local이면 같은 PC에만
    return true;
}

bool MatchService::hostOnline(u32 maxPlayers) {
    if (m_session.state() != net::SessionState::Idle || !onlineAvailable()) return false;
    m_error.clear();
    m_online = true;
    m_onlineHost = true;
    m_maxPlayers = maxPlayers ? maxPlayers : m_config.defaultMaxPlayers;
    m_config.online->createLobby(m_maxPlayers); // 준비되면 update가 세션을 연다
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

void MatchService::returnToLobby() { m_session.returnToLobby(); }

void MatchService::leave() {
    if (m_online && m_config.online) m_config.online->leaveLobby();
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
            } else if (online->lobbyState() == online::LobbyState::InLobby) { // 방장 주소로 락스텝 세션 (플랫폼 릴레이)
                if (online->isLobbyOwner()) {
                    m_session.hostWith(online->createTransport(), m_maxPlayers, m_config.inputDelay);
                    online->inviteFriends();
                } else {
                    m_session.joinWith(online->createTransport(), online->lobbyOwnerAddress());
                }
            }
        }
        if (const u64 invited = online->takeJoinRequest()) { // 친구 초대 수락: 메뉴에 있으면 바로 들어간다
            if (phase() == MatchPhase::Menu) joinOnline(invited);
            else MEALKIT_INFO("Invite to lobby %llu received - leave the current match to join", static_cast<unsigned long long>(invited));
        }
    }

    m_session.update(now);
    if (m_session.state() == net::SessionState::Ended && m_error.empty()) m_error = m_session.endReason();

    // LAN 방 알림: IP로 연 방의 로비 동안
    if (m_announcer.isOpen()) {
        if (m_session.isHost() && m_session.state() == net::SessionState::Lobby) {
            net::RoomAnnouncement room;
            room.game = m_config.gameName;
            const std::vector<net::LobbyMember>& members = m_session.lobbyMembers();
            room.hostName = members.empty() ? std::string() : members.front().name;
            room.build = net::buildId();
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
            RoomEntry e;
            e.address = r.address.toString();
            e.hostName = r.hostName;
            e.players = r.players;
            e.maxPlayers = r.maxPlayers;
            e.sameBuild = r.build == net::buildId();
            e.compatible = e.sameBuild && r.contentHash == m_config.contentHash;
            m_rooms.push_back(std::move(e));
        }
    }
}

void MatchService::shutdown() {
    if (m_online && m_config.online) m_config.online->leaveLobby();
    m_online = false;
    m_announcer.close();
    m_finder.close();
    m_rooms.clear();
    m_session.shutdown();
}

} // namespace mealkit::game
