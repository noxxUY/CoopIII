// Transport over ENet. It owns no thread and starts none; everything ENet
// does happens wherever the owner calls Service() from.
//
// On the client that's the socket thread (client/src/netthread.h), not the
// game thread. Applying net state from the wrong thread would race the world
// update (see docs/protocol.md §1.1), so the socket thread just queues
// messages and the game thread drains them once a frame.
#pragma once

#include "protocol.h"

#include <cstdint>
#include <cstring>
#include <vector>

struct _ENetHost;
struct _ENetPeer;

namespace coopiii {

struct Message {
	uint8_t              opcode;
	uint8_t              channel;
	std::vector<uint8_t> data;   // full packet, header included

	template <class T>
	const T *as() const {
		return data.size() == sizeof(T) && opcode == T::OPCODE
		           ? reinterpret_cast<const T *>(data.data())
		           : nullptr;
	}
};

// Global ENet init/deinit, refcounted. Safe to call from any number of
// NetClient/servers in one process.
bool NetInit();
void NetDeinit();

class NetClient {
public:
	enum State : uint8_t { DISCONNECTED, CONNECTING, CONNECTED };

	NetClient();
	~NetClient();
	NetClient(const NetClient &) = delete;
	NetClient &operator=(const NetClient &) = delete;

	bool Connect(const char *host, uint16_t port);
	void Disconnect();
	State GetState() const { return m_state; }
	bool  IsConnected() const { return m_state == CONNECTED; }
	uint32_t RoundTripMs() const;

	template <class T>
	bool Send(const T &pkt, Channel ch) {
		return SendRaw(&pkt, sizeof(T), ch);
	}
	bool SendRaw(const void *bytes, size_t len, Channel ch);

	// Non-blocking. Flushes outbound, drains inbound into `out` (appended),
	// and moves state on connect/disconnect/timeout.
	void Service(std::vector<Message> &out);

private:
	_ENetHost *m_host = nullptr;
	_ENetPeer *m_peer = nullptr;
	State      m_state = DISCONNECTED;
};

// Server side. `peerId` is an ENet-level connection id, distinct from the
// protocol-level playerId the session layer assigns after C_HELLO.
using PeerId = uint32_t;
constexpr PeerId INVALID_PEER = 0xFFFFFFFF;

struct ServerEvent {
	enum Type : uint8_t { CONNECT, DISCONNECT, MESSAGE } type;
	PeerId  peer;
	Message msg;    // only for MESSAGE
};

class NetServer {
public:
	NetServer();
	~NetServer();
	NetServer(const NetServer &) = delete;
	NetServer &operator=(const NetServer &) = delete;

	bool Listen(uint16_t port, uint8_t maxPeers);
	void Shutdown();
	bool IsListening() const { return m_host != nullptr; }

	template <class T>
	bool SendTo(PeerId peer, const T &pkt, Channel ch) {
		return SendToRaw(peer, &pkt, sizeof(T), ch);
	}
	template <class T>
	void Broadcast(const T &pkt, Channel ch, PeerId except = INVALID_PEER) {
		BroadcastRaw(&pkt, sizeof(T), ch, except);
	}
	bool SendToRaw(PeerId peer, const void *bytes, size_t len, Channel ch);
	void BroadcastRaw(const void *bytes, size_t len, Channel ch,
	                  PeerId except = INVALID_PEER);

	void Disconnect(PeerId peer, uint8_t reason);

	// ENet's own estimate of the round trip to this peer, in ms. The server
	// GUI shows it per player; nothing in the protocol depends on it.
	uint32_t RoundTripMs(PeerId peer) const;

	// Blocks up to timeoutMs waiting for the first event, then drains the rest.
	void Service(std::vector<ServerEvent> &out, uint32_t timeoutMs = 0);

private:
	_ENetHost *m_host = nullptr;
};

template <class T>
void InitHeader(T &pkt, uint32_t sendTimeMs) {
	std::memset(&pkt, 0, sizeof(T));
	pkt.hdr.opcode     = T::OPCODE;
	pkt.hdr.sendTimeMs = sendTimeMs;
}

} // namespace coopiii
