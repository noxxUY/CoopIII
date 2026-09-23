// Money, session side. No engine code in here: game/money.cpp is the engine
// half and everything it does is reached through MoneyBridge, so
// tools/clienttest drives all of this with a stub.
//
// protocol.h's MoneyRule is the design. In short:
//
//   - the rule arrives in S_Money after the welcome, and with no S_Money it
//     is off and nothing here does anything;
//   - under `own` and `shared` the engine half hands over the explosion
//     awards it did not pay here, this sends them to their player
//     (C_MoneyAward), and the ones that come back for us are paid through our
//     own chain;
//   - under `shared` every change to our cash goes out as a delta
//     (C_MoneyChange). The server's total comes back in S_Money and is
//     written over our cash, plus whatever of ours it doesn't hold yet - so a
//     change still on its way is never written away and put back.
//
// A reward every machine's own script pays for the same thing - a shared
// rampage, which rampage.sc pays on every machine that runs it - reaches a
// shared wallet once per player. That is the total `own` pays out for it
// too, so it is left alone rather than guessed at.
#pragma once

#include <coopiii/protocol.h>

#include <cstddef>
#include <cstdint>

namespace coopiii {

// An award the engine half did not pay here, and who it is for.
struct LocalMoneyAward {
	// Our own player. Only ever for a car several machines decide: that one
	// goes through the server too, so it is paid once whoever decides it.
	bool     toUs       = false;
	// Somebody else, by player id when the engine found them at the wheel of
	// a car, by netId when it found their ped.
	uint8_t  toPlayerId = INVALID_PLAYER;
	uint16_t toNetId    = INVALID_NETID;
	uint8_t  kind       = MONEY_AWARD_FIRE;
	uint16_t model      = 0;
	int32_t  unit       = 0;
	UnownedVehicleKey key{MONEY_AWARD_UNKEYED, 0, 0};
};

// The engine seam. Every entry is optional; with none set money stays what it
// was before, each machine's own.
struct MoneyBridge {
	// Whether there is a session and what its rule is. The award detour reads
	// both from inside the engine, so it is told rather than asked.
	void (*SetMoneySession)(bool inSession, uint8_t rule) = nullptr;
	// The local player's cash, and which life of the player it belongs to:
	// the ped's pool reference, which a load or a new game changes. False
	// with no player at all.
	bool (*ReadMoney)(int32_t &money, int32_t &life) = nullptr;
	void (*WriteMoney)(int32_t money) = nullptr;
	// The awards the engine half forwarded since the last call.
	uint8_t (*DrainMoneyAwards)(LocalMoneyAward *out, uint8_t max) = nullptr;
	// One that somebody's machine forwarded to us: one car's worth, which the
	// engine half multiplies by our own chain.
	void (*PayExplosionAward)(int32_t unit) = nullptr;
};

// How many of our changes can be on their way at once. A change is an event -
// a pickup, a fine, a car - so a handful per round trip is a lot.
constexpr size_t MONEY_PENDING = 32;

class MoneySync {
public:
	using SendFn = void (*)(void *ctx, const void *bytes, size_t len, Channel ch);
	// A remote player's id from their netId, out of the roster.
	using PlayerForNetFn = uint8_t (*)(void *ctx, uint16_t netId);

	void Bind(const MoneyBridge *bridge, SendFn send, void *sendCtx,
	          PlayerForNetFn playerForNet, void *rosterCtx) {
		m_bridge       = bridge;
		m_send         = send;
		m_sendCtx      = sendCtx;
		m_playerForNet = playerForNet;
		m_rosterCtx    = rosterCtx;
	}

	// ---- inbound ------------------------------------------------------------
	void OnMoney(const S_Money &pkt, uint8_t localPlayerId);
	void OnAward(const S_MoneyAward &pkt, uint8_t localPlayerId);

	// ---- per frame ----------------------------------------------------------
	// After the simulation: the awards our engine forwarded, and under
	// `shared` whatever our cash did this frame.
	void Send(uint8_t localPlayerId, uint32_t nowMs);

	// The session is gone: back to off. The cash stays where it is, pool and
	// all - a dropped connection is not a reason to take anybody's money.
	void Clear();

	uint8_t Rule() const { return m_rule; }

	// ---- for the tests ------------------------------------------------------
	size_t  PendingCount() const { return m_pendingCount; }
	bool    HaveTotal() const { return m_haveTotal; }
	int32_t Total() const { return m_total; }

private:
	struct Pending {
		uint32_t seq   = 0;
		int32_t  delta = 0;
	};

	void PushSession(bool inSession);
	void ResetWallet();
	// Reads our cash and sends what changed since we last left it. Also where
	// a new life of the player is noticed and given the pool instead.
	void Observe(uint32_t nowMs);
	void SendChange(int32_t delta, int32_t have, uint32_t nowMs);
	// Writes the pool plus our changes it doesn't hold yet.
	void Adopt(int32_t cash);
	void DropAcked(uint32_t ackSeq);
	void SendAwards(uint8_t localPlayerId, uint32_t nowMs);

	template <class T>
	void Out(const T &pkt, Channel ch) {
		if (m_send)
			m_send(m_sendCtx, &pkt, sizeof(T), ch);
	}

	const MoneyBridge *m_bridge       = nullptr;
	SendFn             m_send         = nullptr;
	void              *m_sendCtx      = nullptr;
	PlayerForNetFn     m_playerForNet = nullptr;
	void              *m_rosterCtx    = nullptr;

	uint8_t  m_rule = MONEY_RULE_OFF;

	// ---- the shared wallet ----
	bool     m_haveTotal    = false;
	int32_t  m_total        = 0;
	// Whether m_written is our cash as we last left it, for this life of the
	// player. False before the first read and after any read that failed.
	bool     m_haveBaseline = false;
	int32_t  m_life         = 0;
	int32_t  m_written      = 0;
	// The pool was empty when we were told the rule, and our cash has not
	// gone out to fill it yet.
	bool     m_seedWanted   = false;
	uint32_t m_nextSeq      = 1;
	Pending  m_pending[MONEY_PENDING];
	size_t   m_pendingCount = 0;

	bool m_saidRule       = false;
	bool m_saidAdopted    = false;
	bool m_saidSeeded     = false;
	bool m_saidChange     = false;
	bool m_saidForwarded  = false;
	bool m_saidPaid       = false;
	bool m_saidNobody     = false;
	bool m_saidFull       = false;
	bool m_saidNewLife    = false;
};

} // namespace coopiii
