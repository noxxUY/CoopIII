// The lobby: launchers waiting together before anybody's game is running,
// and the one who has waited longest starting everybody's game at once.
// docs/protocol.md 1.31; protocol.h has the wire.
//
// Kept apart from Session because a lobby member is nobody the session knows:
// no slot, no ped, no netId. It is a connection with a name, and a place in
// the queue that decides who the host is.
#pragma once

#include <coopiii/net.h>
#include <coopiii/protocol.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace coopiii {

class Lobby {
public:
	struct Member {
		bool        active = false;
		PeerId      peer   = INVALID_PEER;
		uint8_t     id     = INVALID_PLAYER;
		uint32_t    order  = 0;   // arrival: the lowest is the host
		std::string nick;
	};

	// A launcher coming in, with the name it gave. Null when the lobby is
	// full (`reject` says why) or the connection is in already.
	const Member *Join(PeerId peer, const char *nick, RejectReason &reject);

	// Its connection went. True when it was in the lobby.
	bool Leave(PeerId peer);

	const Member *FindByPeer(PeerId peer) const;
	bool          Has(PeerId peer) const { return FindByPeer(peer) != nullptr; }
	size_t        Count() const;

	// The member who has waited longest, INVALID_PLAYER with nobody waiting.
	// Stays the host until they go, whoever comes after.
	uint8_t HostId() const;

	// The host starting everybody's game. True for the host, once in
	// LOBBY_START_COOLDOWN_MS: the same click arriving twice is one start.
	bool MayStart(PeerId peer, uint32_t nowMs);

	// The lobby's part of S_Lobby: its members, longest waiting first, with
	// the host marked, and `hostLobbyId`, `waiting` and `count` to match.
	void FillRoster(S_Lobby &out) const;

	// Every place, taken or not; `active` says which.
	struct Places {
		const Member *b, *e;
		const Member *begin() const { return b; }
		const Member *end() const { return e; }
	};
	Places Members() const { return {m_members, m_members + LOBBY_MAX}; }

private:
	Member   m_members[LOBBY_MAX];
	uint32_t m_nextOrder   = 1;
	uint32_t m_lastStartMs = 0;
	bool     m_started     = false;
};

} // namespace coopiii
