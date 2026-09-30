// The socket thread.
//
// docs/protocol.md §1.1: the game is single-threaded and frame-driven, and
// applying net state from the socket thread would race the world update. So
// this thread doesn't touch game memory, period. It owns the ENet client,
// services it in a tight loop, and only ever talks to the game thread through
// queues (client/src/queue.h).
//
// Keep the direction of these calls straight, it's easy to get backwards:
//
//   game thread  -> Send()          -> outbound queue -> socket thread
//   socket thread-> inbound queue   -> DrainInbound()  -> game thread
//
// Nothing else crosses between the two.
#pragma once

#include "queue.h"

#include <coopiii/net.h>
#include <coopiii/protocol.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace coopiii {

class NetThread {
public:
	NetThread() = default;
	~NetThread();
	NetThread(const NetThread &)            = delete;
	NetThread &operator=(const NetThread &) = delete;

	// Starts the thread and begins connecting. Returns false only if the
	// thread or ENet failed to start. A failed connection is a different
	// thing - that's async, and shows up through State() instead.
	bool Start(std::string host, uint16_t port, std::string nick);

	// Sent right behind every hello when not empty (protocol.h, C_Password).
	// Before Start.
	void SetPassword(std::string password) { m_password = std::move(password); }
	void Stop();

	NetClient::State State() const { return m_state.load(std::memory_order_relaxed); }
	bool IsConnected() const { return State() == NetClient::CONNECTED; }

	// Assigned by the server in S_Welcome. Only meaningful once connected.
	uint8_t PlayerId() const { return m_playerId.load(std::memory_order_relaxed); }

	uint32_t RoundTripMs() const { return m_rttMs.load(std::memory_order_relaxed); }

	// Callable from the game thread. The packet is copied and sent by the
	// socket thread on its next pass.
	template <class T>
	void Send(const T &pkt, Channel ch) {
		SendRaw(&pkt, sizeof(T), ch);
	}
	void SendRaw(const void *bytes, size_t len, Channel ch);

	// Game thread, once per frame. Appends everything that arrived since the
	// last call.
	void DrainInbound(std::vector<Message> &out);

	// Diagnostics, for the log line on disconnect.
	uint64_t PacketsSent() const { return m_sent.load(std::memory_order_relaxed); }
	uint64_t PacketsReceived() const { return m_received.load(std::memory_order_relaxed); }

private:
	struct Outbound {
		std::vector<uint8_t> bytes;
		Channel              channel;
	};

	void Run();
	void SendHello(NetClient &client);

	std::thread       m_thread;
	std::atomic<bool> m_running{false};

	std::atomic<NetClient::State> m_state{NetClient::DISCONNECTED};
	std::atomic<uint8_t>          m_playerId{0xFF};
	std::atomic<uint32_t>         m_rttMs{0};
	std::atomic<uint64_t>         m_sent{0};
	std::atomic<uint64_t>         m_received{0};

	SwapQueue<Outbound> m_outbound;
	SwapQueue<Message>  m_inbound;

	std::string m_host;
	uint16_t    m_port = DEFAULT_PORT;
	std::string m_nick;
	std::string m_password;

	// Why the server last turned us away, REJECT_NONE while it has not, or
	// REJECT_KICKED after a kick. Written and read on the socket thread only.
	static constexpr uint8_t REJECT_KICKED = 0xFE;
	uint8_t m_rejected = REJECT_NONE;
};

} // namespace coopiii
