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
// And how long after being turned away for a reason that can change - a full
// server - before asking again.
constexpr uint32_t REJECTED_RETRY_MS = 15000;

// How much may wait for a game thread that has stopped draining: alt-tabbed
// out, which stops the retail loop, or on a long load. A snapshot is only
// worth its newest, so past a few seconds of a full session they are dropped
// here. The reliable ones are the session's history and are kept up to a
// point, past which the connection is let go and the rejoin starts clean -
// held for ever, they ran a 32-bit process out of memory in about an hour.
constexpr size_t INBOUND_SNAPSHOT_CAP = 4096;
constexpr size_t INBOUND_HARD_CAP     = 65536;

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

	// On the same ordered channel, so it lands right behind the hello it
	// belongs to.
	if (!m_password.empty()) {
		C_Password pw;
		InitHeader(pw, WallClock::NowMs());
		std::strncpy(pw.password, m_password.c_str(), sizeof(pw.password) - 1);
		client.Send(pw, CH_EVENT);
		m_sent.fetch_add(1, std::memory_order_relaxed);
	}
}

void NetThread::Run() {
	NetClient client;
	std::vector<Message> messages;
	std::vector<Outbound> pending;

	uint32_t nextAttemptMs = 0;
	bool     helloSent     = false;
	bool     saidGivingUp  = false;
	bool     saidBacklog   = false;

	while (m_running.load(std::memory_order_relaxed)) {
		const uint32_t now = WallClock::NowMs();

		if (client.GetState() == NetClient::DISCONNECTED) {
			if (helloSent) {
				// Was connected before. The game-side registry needs to know, so
				// log the transition instead of just quietly retrying.
				Log("net: disconnected from %s:%u", m_host.c_str(), m_port);
				helloSent = false;
				// Thrown out by whoever runs the server. Coming straight back,
				// which is what a dropped connection gets, undid the kick in
				// a few milliseconds. Only once welcomed: a refused hello is
				// hung up on with the same reason, and its own reject already
				// says what to do.
				if (client.LastDisconnectReason() == LEAVE_KICKED &&
				    m_rejected == REJECT_NONE &&
				    m_playerId.load(std::memory_order_relaxed) != 0xFF)
					m_rejected = REJECT_KICKED;
			}
			m_state.store(NetClient::DISCONNECTED, std::memory_order_relaxed);
			m_playerId.store(0xFF, std::memory_order_relaxed);

			// Turned away. A different version or a wrong password is the
			// same answer every time, and asking every two seconds only fills
			// the server's log; a full server may have room in a while.
			if (m_rejected == REJECT_BAD_VERSION || m_rejected == REJECT_BAD_PASSWORD ||
			    m_rejected == REJECT_KICKED) {
				if (!saidGivingUp) {
					saidGivingUp = true;
					Log("net: %s:%u turned us away (%s); not trying again until the game "
					    "restarts",
					    m_host.c_str(), m_port,
					    m_rejected == REJECT_BAD_VERSION  ? "it runs a different CoopIII"
					    : m_rejected == REJECT_KICKED     ? "we were kicked"
					                                      : "the password is wrong or missing");
				}
			} else if (static_cast<int32_t>(now - nextAttemptMs) >= 0) {
				nextAttemptMs = now + (m_rejected != REJECT_NONE ? REJECTED_RETRY_MS
				                                                 : RECONNECT_DELAY_MS);
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

		size_t waiting = messages.empty() ? 0 : m_inbound.Size();
		for (Message &msg : messages) {
			m_received.fetch_add(1, std::memory_order_relaxed);

			if (waiting >= INBOUND_SNAPSHOT_CAP && msg.channel == CH_SNAPSHOT) {
				if (!saidBacklog) {
					saidBacklog = true;
					Log("net: the game has stopped taking packets; dropping snapshots "
					    "until it catches up");
				}
				continue;
			}
			if (waiting >= INBOUND_HARD_CAP) {
				Log("net: %zu packets are waiting for the game; letting the "
				    "connection go so it can start again clean", waiting);
				m_inbound.Clear();
				client.Disconnect();
				break;
			}
			++waiting;

			// Peeked here instead of on the game thread so PlayerId() is valid
			// as early as possible. The message still gets forwarded though,
			// so the game thread sees the welcome as well.
			if (const S_Welcome *welcome = msg.as<S_Welcome>()) {
				m_rejected = welcome->reject;
				if (welcome->reject == REJECT_NONE) {
					m_playerId.store(welcome->playerId, std::memory_order_relaxed);
					Log("net: welcomed as player %u", welcome->playerId);
				} else {
					Log("net: the server turned us away (reason %u)", welcome->reject);
				}
			}
			m_inbound.Push(std::move(msg));
		}

		std::this_thread::sleep_for(SERVICE_INTERVAL);
	}

	client.Disconnect();
}

} // namespace coopiii
