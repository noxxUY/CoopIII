#include "coopiii/net.h"

#include <enet/enet.h>

namespace coopiii {

namespace {
int g_initRefs = 0;

Message MessageFrom(const ENetPacket *pkt, uint8_t channel) {
	Message msg;
	msg.channel = channel;
	msg.opcode  = pkt->dataLength > 0 ? pkt->data[0] : 0;
	msg.data.assign(pkt->data, pkt->data + pkt->dataLength);
	return msg;
}
} // namespace

bool NetInit() {
	if (g_initRefs++ > 0)
		return true;
	if (enet_initialize() != 0) {
		g_initRefs = 0;
		return false;
	}
	return true;
}

void NetDeinit() {
	if (g_initRefs > 0 && --g_initRefs == 0)
		enet_deinitialize();
}

NetClient::NetClient() = default;

NetClient::~NetClient() {
	Disconnect();
	if (m_host) {
		enet_host_destroy(m_host);
		m_host = nullptr;
	}
}

bool NetClient::Connect(const char *host, uint16_t port) {
	if (m_state != DISCONNECTED)
		return false;

	if (!m_host) {
		m_host = enet_host_create(nullptr, 1, CH_COUNT, 0, 0);
		if (!m_host)
			return false;
	}

	ENetAddress addr;
	if (enet_address_set_host(&addr, host) != 0)
		return false;
	addr.port = port;

	m_peer = enet_host_connect(m_host, &addr, CH_COUNT, 0);
	if (!m_peer)
		return false;

	m_state = CONNECTING;
	return true;
}

void NetClient::Disconnect() {
	if (!m_peer)
		return;

	enet_peer_disconnect(m_peer, LEAVE_QUIT);

	// Give the peer a moment to acknowledge; drop it if it doesn't.
	ENetEvent ev;
	while (enet_host_service(m_host, &ev, 200) > 0) {
		if (ev.type == ENET_EVENT_TYPE_RECEIVE)
			enet_packet_destroy(ev.packet);
		else if (ev.type == ENET_EVENT_TYPE_DISCONNECT)
			break;
	}
	if (m_peer)
		enet_peer_reset(m_peer);

	m_peer  = nullptr;
	m_state = DISCONNECTED;
}

uint32_t NetClient::RoundTripMs() const {
	return m_peer ? m_peer->roundTripTime : 0;
}

bool NetClient::SendRaw(const void *bytes, size_t len, Channel ch) {
	if (m_state != CONNECTED || !m_peer)
		return false;

	const enet_uint32 flags =
	    ch == CH_EVENT ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;

	ENetPacket *pkt = enet_packet_create(bytes, len, flags);
	if (!pkt)
		return false;
	return enet_peer_send(m_peer, ch, pkt) == 0;
}

void NetClient::Service(std::vector<Message> &out) {
	if (!m_host)
		return;

	ENetEvent ev;
	while (enet_host_service(m_host, &ev, 0) > 0) {
		switch (ev.type) {
		case ENET_EVENT_TYPE_CONNECT:
			m_state = CONNECTED;
			break;
		case ENET_EVENT_TYPE_RECEIVE:
			out.push_back(MessageFrom(ev.packet, ev.channelID));
			enet_packet_destroy(ev.packet);
			break;
		case ENET_EVENT_TYPE_DISCONNECT:
			m_state = DISCONNECTED;
			m_peer  = nullptr;
			break;
		default:
			break;
		}
	}
}

// --------------------------------------------------------------------------
// NetServer
// --------------------------------------------------------------------------

namespace {
// ENet gives us ENetPeer*; the session layer wants a stable integer. The peer
// array is fixed-size for the host's lifetime, so the index is that integer.
PeerId PeerIndex(const ENetHost *host, const ENetPeer *peer) {
	return static_cast<PeerId>(peer - host->peers);
}
} // namespace

NetServer::NetServer() = default;

NetServer::~NetServer() {
	Shutdown();
}

bool NetServer::Listen(uint16_t port, uint8_t maxPeers) {
	if (m_host)
		return false;

	ENetAddress addr;
	addr.host = ENET_HOST_ANY;
	addr.port = port;

	m_host = enet_host_create(&addr, maxPeers, CH_COUNT, 0, 0);
	return m_host != nullptr;
}

void NetServer::Shutdown() {
	if (!m_host)
		return;
	enet_host_destroy(m_host);
	m_host = nullptr;
}

bool NetServer::SendToRaw(PeerId peer, const void *bytes, size_t len, Channel ch) {
	if (!m_host || peer >= m_host->peerCount)
		return false;

	ENetPeer *p = &m_host->peers[peer];
	if (p->state != ENET_PEER_STATE_CONNECTED)
		return false;

	const enet_uint32 flags =
	    ch == CH_EVENT ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;

	ENetPacket *pkt = enet_packet_create(bytes, len, flags);
	if (!pkt)
		return false;
	return enet_peer_send(p, ch, pkt) == 0;
}

void NetServer::BroadcastRaw(const void *bytes, size_t len, Channel ch, PeerId except) {
	if (!m_host)
		return;
	for (PeerId i = 0; i < m_host->peerCount; ++i) {
		if (i == except)
			continue;
		SendToRaw(i, bytes, len, ch);
	}
}

uint32_t NetServer::RoundTripMs(PeerId peer) const {
	if (!m_host || peer >= m_host->peerCount)
		return 0;
	return m_host->peers[peer].roundTripTime;
}

void NetServer::Disconnect(PeerId peer, uint8_t reason) {
	if (!m_host || peer >= m_host->peerCount)
		return;
	enet_peer_disconnect(&m_host->peers[peer], reason);
}

void NetServer::Service(std::vector<ServerEvent> &out, uint32_t timeoutMs) {
	if (!m_host)
		return;

	ENetEvent ev;
	// First call may block up to timeoutMs; subsequent ones drain with 0.
	int rc = enet_host_service(m_host, &ev, timeoutMs);
	while (rc > 0) {
		ServerEvent se;
		se.peer = PeerIndex(m_host, ev.peer);

		switch (ev.type) {
		case ENET_EVENT_TYPE_CONNECT:
			se.type = ServerEvent::CONNECT;
			out.push_back(std::move(se));
			break;
		case ENET_EVENT_TYPE_RECEIVE:
			se.type = ServerEvent::MESSAGE;
			se.msg  = MessageFrom(ev.packet, ev.channelID);
			out.push_back(std::move(se));
			enet_packet_destroy(ev.packet);
			break;
		case ENET_EVENT_TYPE_DISCONNECT:
			se.type = ServerEvent::DISCONNECT;
			out.push_back(std::move(se));
			break;
		default:
			break;
		}

		rc = enet_host_service(m_host, &ev, 0);
	}
}

} // namespace coopiii
