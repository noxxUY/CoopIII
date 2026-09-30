#include "lobby.h"

#include "session.h"

#include <algorithm>
#include <cstring>

namespace coopiii {

const Lobby::Member *Lobby::Join(PeerId peer, const char *nick, RejectReason &reject) {
	reject = REJECT_NONE;
	if (FindByPeer(peer))
		return nullptr;
	for (uint8_t i = 0; i < LOBBY_MAX; ++i) {
		Member &m = m_members[i];
		if (m.active)
			continue;
		m        = Member{};
		m.active = true;
		m.peer   = peer;
		m.id     = i;
		m.order  = m_nextOrder++;
		m.nick   = SanitizeText(nick, NICK_LEN);
		return &m;
	}
	reject = REJECT_FULL;
	return nullptr;
}

bool Lobby::Leave(PeerId peer) {
	for (Member &m : m_members)
		if (m.active && m.peer == peer) {
			m = Member{};
			return true;
		}
	return false;
}

const Lobby::Member *Lobby::FindByPeer(PeerId peer) const {
	for (const Member &m : m_members)
		if (m.active && m.peer == peer)
			return &m;
	return nullptr;
}

size_t Lobby::Count() const {
	size_t n = 0;
	for (const Member &m : m_members)
		if (m.active)
			++n;
	return n;
}

uint8_t Lobby::HostId() const {
	const Member *host = nullptr;
	for (const Member &m : m_members)
		if (m.active && (!host || m.order < host->order))
			host = &m;
	return host ? host->id : INVALID_PLAYER;
}

bool Lobby::MayStart(PeerId peer, uint32_t nowMs) {
	const Member *m = FindByPeer(peer);
	if (!m || m->id != HostId())
		return false;
	if (m_started && static_cast<int32_t>(nowMs - m_lastStartMs) <
	                     static_cast<int32_t>(LOBBY_START_COOLDOWN_MS))
		return false;
	m_started     = true;
	m_lastStartMs = nowMs;
	return true;
}

void Lobby::FillRoster(S_Lobby &out) const {
	const Member *order[LOBBY_MAX];
	size_t        n = 0;
	for (const Member &m : m_members)
		if (m.active)
			order[n++] = &m;
	std::sort(order, order + n, [](const Member *a, const Member *b) { return a->order < b->order; });

	const uint8_t host = HostId();
	out.hostLobbyId    = host;
	out.waiting        = static_cast<uint8_t>(n);
	out.count          = static_cast<uint8_t>(n);
	for (size_t i = 0; i < n; ++i) {
		LobbyEntry &e = out.entries[i];
		e.id          = order[i]->id;
		e.flags       = order[i]->id == host ? LOBBY_ENTRY_HOST : 0;
		std::memset(e.nick, 0, sizeof e.nick);
		std::strncpy(e.nick, order[i]->nick.c_str(), NICK_LEN - 1);
	}
}

} // namespace coopiii
