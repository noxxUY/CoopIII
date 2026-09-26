#include "launcher/lobby.h"

#include <cstring>

namespace coopiii::launcher {

const char *LobbyRefusalText(uint8_t reject) {
	switch (reject) {
	case REJECT_BAD_VERSION:
		return "The server runs another CoopIII release. Both have to be the same one.";
	case REJECT_FULL:
		return "The lobby is full.";
	case REJECT_BAD_PASSWORD:
		return "The server wants a password. Put it in CoopIII.ini as password = ...";
	default:
		return "The server turned this launcher away.";
	}
}

LobbyClient::LobbyClient() = default;

LobbyClient::~LobbyClient() {
	Leave();
	m_net.reset();
	if (m_netUp)
		NetDeinit();
}

bool LobbyClient::Join(const std::string &host, uint16_t port, const std::string &nick,
                       const std::string &password, uint32_t nowMs) {
	Leave();
	if (!m_netUp && !(m_netUp = NetInit())) {
		m_phase   = Phase::Lost;
		m_problem = "The network could not start.";
		return false;
	}
	if (!m_net)
		m_net.reset(new NetClient());
	if (!m_net->Connect(host.c_str(), port)) {
		m_phase   = Phase::Lost;
		m_problem = "Could not find " + host + ".";
		return false;
	}
	m_nick     = nick;
	m_password = password;
	m_phase    = Phase::Connecting;
	m_sinceMs  = nowMs;
	return true;
}

void LobbyClient::Leave() {
	if (m_net)
		m_net->Disconnect();
	m_phase = Phase::Idle;
	m_problem.clear();
	m_people.clear();
	m_lobbyId    = INVALID_PLAYER;
	m_hostId     = INVALID_PLAYER;
	m_started    = false;
	m_startTaken = false;
	m_startMode  = 0;
	m_startBy.clear();
}

void LobbyClient::Lose(const char *why) {
	if (m_net)
		m_net->Disconnect();
	m_phase   = Phase::Lost;
	m_problem = why;
	m_people.clear();
	m_lobbyId = INVALID_PLAYER;
	m_hostId  = INVALID_PLAYER;
}

void LobbyClient::Service(uint32_t nowMs) {
	if (!m_net || m_phase == Phase::Idle || m_phase == Phase::Refused || m_phase == Phase::Lost)
		return;
	m_inbox.clear();
	m_net->Service(m_inbox);
	for (const Message &msg : m_inbox)
		OnMessage(msg);

	switch (m_phase) {
	case Phase::Connecting:
		if (m_net->IsConnected()) {
			C_LobbyJoin join{};
			InitHeader(join, nowMs);
			join.protocolVersion = PROTOCOL_VERSION;
			std::strncpy(join.nick, m_nick.c_str(), NICK_LEN - 1);
			std::strncpy(join.password, m_password.c_str(), PASSWORD_LEN - 1);
			m_net->Send(join, CH_EVENT);
			m_phase   = Phase::Asking;
			m_sinceMs = nowMs;
		} else if (m_net->GetState() == NetClient::DISCONNECTED) {
			Lose("Could not reach the server.");
		} else if (nowMs - m_sinceMs >= LOBBY_CONNECT_WAIT_MS) {
			Lose("The server did not answer. Is it running, and is the address right?");
		}
		break;
	case Phase::Asking:
		if (!m_net->IsConnected())
			Lose("The server hung up.");
		else if (nowMs - m_sinceMs >= LOBBY_ANSWER_WAIT_MS)
			Lose("This server has no lobby; it may run an older CoopIII. Start your game "
			     "on your own.");
		break;
	case Phase::In:
		if (!m_net->IsConnected())
			Lose("The connection to the server was lost.");
		break;
	default:
		break;
	}
}

void LobbyClient::OnMessage(const Message &msg) {
	if (const S_LobbyAnswer *a = msg.as<S_LobbyAnswer>()) {
		if (m_phase != Phase::Asking)
			return;
		if (a->reject == REJECT_NONE && a->lobbyId < LOBBY_MAX) {
			m_lobbyId = a->lobbyId;
			m_phase   = Phase::In;
		} else {
			m_phase   = Phase::Refused;
			m_problem = LobbyRefusalText(a->reject);
		}
	} else if (const S_Lobby *l = msg.as<S_Lobby>()) {
		if (m_phase != Phase::In || l->count > LOBBY_MAX + MAX_PLAYERS)
			return;
		m_hostId = l->hostLobbyId;
		m_people.clear();
		for (uint8_t i = 0; i < l->count; ++i) {
			const LobbyEntry &e = l->entries[i];
			Person            p;
			p.nick    = std::string(e.nick, strnlen(e.nick, NICK_LEN));
			p.id      = e.id;
			p.playing = (e.flags & LOBBY_ENTRY_PLAYING) != 0;
			p.host    = !p.playing && (e.flags & LOBBY_ENTRY_HOST) != 0;
			p.you     = !p.playing && e.id == m_lobbyId;
			m_people.push_back(p);
		}
	} else if (const S_LobbyStart *st = msg.as<S_LobbyStart>()) {
		if (m_phase != Phase::In || m_started)
			return;
		if (st->mode != LOBBY_START_MENU && st->mode != LOBBY_START_NEW_GAME)
			return;
		m_started   = true;
		m_startMode = st->mode;
		m_startBy.clear();
		for (const Person &p : m_people)
			if (!p.playing && p.id == st->byLobbyId)
				m_startBy = p.nick;
	}
}

size_t LobbyClient::Waiting() const {
	size_t n = 0;
	for (const Person &p : m_people)
		if (!p.playing)
			++n;
	return n;
}

size_t LobbyClient::Playing() const {
	return m_people.size() - Waiting();
}

bool LobbyClient::WeAreHost() const {
	return m_phase == Phase::In && m_lobbyId != INVALID_PLAYER && m_hostId == m_lobbyId;
}

std::string LobbyClient::HostNick() const {
	for (const Person &p : m_people)
		if (p.host)
			return p.nick;
	return std::string();
}

bool LobbyClient::Start(uint8_t mode, uint32_t nowMs) {
	if (!WeAreHost() || !m_net || (mode != LOBBY_START_MENU && mode != LOBBY_START_NEW_GAME))
		return false;
	C_LobbyStart out{};
	InitHeader(out, nowMs);
	out.mode = mode;
	return m_net->Send(out, CH_EVENT);
}

bool LobbyClient::TakeStart(uint8_t *mode, std::string *byNick) {
	if (!m_started || m_startTaken)
		return false;
	m_startTaken = true;
	if (mode)
		*mode = m_startMode;
	if (byNick)
		*byNick = m_startBy;
	return true;
}

} // namespace coopiii::launcher
