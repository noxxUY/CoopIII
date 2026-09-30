// The launcher's side of the lobby (docs/protocol.md 1.31): a connection of
// its own to the server, made before any game is running, to wait with the
// others and be started with them.
//
// No window and no Windows in here, so servertest can drive it against a real
// server. The window draws what it says and calls Service every frame.
#pragma once

#include <coopiii/net.h>
#include <coopiii/protocol.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coopiii::launcher {

// How long the connection may take to come up, and the server to answer once
// it has. An older server never answers: it drops C_LobbyJoin.
constexpr uint32_t LOBBY_CONNECT_WAIT_MS = 6000;
constexpr uint32_t LOBBY_ANSWER_WAIT_MS  = 3000;

class LobbyClient {
public:
	enum class Phase : uint8_t {
		Idle,         // not in, not asking
		Connecting,   // the connection is on its way up
		Asking,       // up, and waiting for the server's answer
		In,           // in the lobby
		Refused,      // turned away; Problem() says why
		Lost,         // no connection, or it went; Problem() says why
	};

	struct Person {
		std::string nick;
		uint8_t     id      = INVALID_PLAYER;   // a lobby id, or a player id if playing
		bool        host    = false;   // may start everybody's game
		bool        playing = false;   // in the game already
		bool        you     = false;
	};

	LobbyClient();
	~LobbyClient();
	LobbyClient(const LobbyClient &) = delete;
	LobbyClient &operator=(const LobbyClient &) = delete;

	// Asks into the lobby of the server at host:port, leaving any other first.
	// False when the connection cannot even be tried; Problem() says why.
	bool Join(const std::string &host, uint16_t port, const std::string &nick,
	          const std::string &password, uint32_t nowMs);
	void Leave();

	// The network and the timeouts, every frame.
	void Service(uint32_t nowMs);

	Phase              GetPhase() const { return m_phase; }
	const std::string &Problem() const { return m_problem; }

	// Who is there, as the server last said: the lobby, longest waiting first,
	// then everybody already playing.
	const std::vector<Person> &People() const { return m_people; }
	size_t                     Waiting() const;
	size_t                     Playing() const;
	bool                       WeAreHost() const;
	std::string                HostNick() const;

	// The host's click: start everybody's game (LobbyStartMode). False when
	// this launcher is not the host, or not in.
	bool Start(uint8_t mode, uint32_t nowMs);

	// True once, when the host has started everybody's game, with the mode
	// and who started it.
	bool TakeStart(uint8_t *mode, std::string *byNick);

private:
	void Lose(const char *why);
	void OnMessage(const Message &msg);

	// Made on the first join, and gone before the network is let go of.
	std::unique_ptr<NetClient> m_net;
	bool                 m_netUp  = false;
	Phase                m_phase  = Phase::Idle;
	std::string          m_problem;
	std::string          m_nick;
	std::string          m_password;
	uint32_t             m_sinceMs = 0;   // when the current phase began
	uint8_t              m_lobbyId = INVALID_PLAYER;
	uint8_t              m_hostId  = INVALID_PLAYER;
	std::vector<Person>  m_people;
	std::vector<Message> m_inbox;
	bool                 m_started   = false;
	bool                 m_startTaken = false;
	uint8_t              m_startMode = 0;
	std::string          m_startBy;
};

// What a refusal says to the player.
const char *LobbyRefusalText(uint8_t reject);

} // namespace coopiii::launcher
