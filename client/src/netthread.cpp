#include "netthread.h"

#include "clock.h"
#include "log.h"

#include <chrono>
#include <cstring>

namespace coopiii {

namespace {

// How often the socket thread wakes up. Well under the 40 ms snapshot
// interval so an outbound packet never sits waiting for a meaningful chunk
// of a tick, and cheap enough that the cost doesn't register.
constexpr auto SERVICE_INTERVAL = std::chrono::milliseconds(2);

// Between connection attempts. Long enough to not hammer a server that isn't
// there yet; short enough that starting the game before the server is up
// isn't a permanent failure.
constexpr uint32_t RECONNECT_DELAY_MS = 2000;

} // namespace

NetThread::~NetThread() {
	Stop();
}

bool NetThread::Start(std::string host, uint16_t port, std::string nick) {
	if (m_running.load())
		return true;

	m_host = std::move(host);
	m_port = port;
	m_nick = std::move(nick);

	if (!NetInit()) {
		Log("net: ENet failed to initialise");
		return false;
	}

	m_running.store(true);
	try {
		m_thread = std::thread(&NetThread::Run, this);
	} catch (...) {
		m_running.store(false);
		NetDeinit();
		Log("net: could not start the socket thread");
		return false;
	}
	return true;
}

void NetThread::Stop() {
	if (!m_running.exchange(false))
		return;
	if (m_thread.joinable())
		m_thread.join();

	Log("net: stopped (%llu sent, %llu received)",
	    static_cast<unsigned long long>(m_sent.load()),
	    static_cast<unsigned long long>(m_received.load()));

	m_outbound.Clear();
	m_inbound.Clear();
	m_state.store(NetClient::DISCONNECTED);
	NetDeinit();
}

void NetThread::SendRaw(const void *bytes, size_t len, Channel ch) {
	// Outbound traffic gets dropped while disconnected on purpose. Queueing
	// it would just deliver a burst of stale positions the moment we
	// reconnect, and every single one of them would be wrong by then.
	if (!IsConnected())
		return;

	Outbound out;
	out.bytes.resize(len);
	std::memcpy(out.bytes.data(), bytes, len);
	out.channel = ch;
	m_outbound.Push(std::move(out));
}

void NetThread::DrainInbound(std::vector<Message> &out) {
	m_inbound.DrainInto(out);
}

void NetThread::SendHello(NetClient &client) {
	C_Hello hello;
	InitHeader(hello, WallClock::NowMs());
	hello.protocolVersion = PROTOCOL_VERSION;
	// 0 is MI_PLAYER, i.e. Claude. Not a placeholder, not "unset" - GTA III
	// only has one protagonist model, everybody wears it, and this field
	// just carries that explicitly. See game/addresses.h, MI_PLAYER.
	hello.modelId         = 0;

	std::memset(hello.nick, 0, sizeof(hello.nick));
	// NICK_LEN-1 so the field always ends up terminated. Config::SanitizeNick
	// already trims to that length; this is just belt and braces at the wire.
	std::strncpy(hello.nick, m_nick.c_str(), sizeof(hello.nick) - 1);

	client.Send(hello, CH_EVENT);
	m_sent.fetch_add(1, std::memory_order_relaxed);
	Log("net: sent hello (protocol v%u, nick \"%s\")", PROTOCOL_VERSION, hello.nick);
}

void NetThread::Run() {
	NetClient client;
	std::vector<Message> messages;
	std::vector<Outbound> pending;

	uint32_t nextAttemptMs = 0;
	bool     helloSent     = false;

	while (m_running.load(std::memory_order_relaxed)) {
		const uint32_t now = WallClock::NowMs();

		if (client.GetState() == NetClient::DISCONNECTED) {
			if (helloSent) {
				// Was connected before. The game-side registry needs to know, so
				// log the transition instead of just quietly retrying.
				Log("net: disconnected from %s:%u", m_host.c_str(), m_port);
				helloSent = false;
			}
			m_state.store(NetClient::DISCONNECTED, std::memory_order_relaxed);
			m_playerId.store(0xFF, std::memory_order_relaxed);

			if (static_cast<int32_t>(now - nextAttemptMs) >= 0) {
				nextAttemptMs = now + RECONNECT_DELAY_MS;
				if (client.Connect(m_host.c_str(), m_port))
					Log("net: connecting to %s:%u", m_host.c_str(), m_port);
			}
		}

		// Flush whatever the game thread has queued up since the last pass.
		pending.clear();
		m_outbound.DrainInto(pending);
		for (const Outbound &o : pending) {
			if (client.SendRaw(o.bytes.data(), o.bytes.size(), o.channel))
				m_sent.fetch_add(1, std::memory_order_relaxed);
		}

		messages.clear();
		client.Service(messages);

		const NetClient::State state = client.GetState();
		m_state.store(state, std::memory_order_relaxed);

		if (state == NetClient::CONNECTED) {
			m_rttMs.store(client.RoundTripMs(), std::memory_order_relaxed);
			if (!helloSent) {
				SendHello(client);
				helloSent = true;
			}
		}

		for (Message &msg : messages) {
			m_received.fetch_add(1, std::memory_order_relaxed);

			// Peeked here instead of on the game thread so PlayerId() is valid
			// as early as possible. The message still gets forwarded though,
			// so the game thread sees the welcome as well.
			if (const S_Welcome *welcome = msg.as<S_Welcome>()) {
				m_playerId.store(welcome->playerId, std::memory_order_relaxed);
				Log("net: welcomed as player %u", welcome->playerId);
			}
			m_inbound.Push(std::move(msg));
		}

		std::this_thread::sleep_for(SERVICE_INTERVAL);
	}

	client.Disconnect();
}

} // namespace coopiii
