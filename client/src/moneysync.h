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
//   - under `shared` our own cash is kept beside the wallet: what we had
//     when the wallet was first written over it, moved by every change our
//     own engine makes. A save made in the session stores that rather than
//     the wallet (game/money.h, PutOwnMoneyForSave), and leaving the session
//     or the rule puts it back in our pocket. Another player's hidden
//     package pays our own cash alone (DrainOwnCredit): his engine paid the
//     wallet for it, and our save counts the package too.
//
// A shared rampage is the one reward every machine's own script pays for the
// same thing: rampage.sc pays its player as soon as it is handed PASSED, on
// every machine in the frenzy. Under `shared` only the payer's goes into the
// wallet (S_RampageEnd::payerId, the player whose report won). Every other
// machine takes its own back within RAMPAGE_REWARD_WAIT_MS of the verdict:
// exactly what the engine half saw its rampage.sc pay (OnRampageRewardPaid),
// so other money that lands in the same frame still goes into the wallet; or,
// with no such report, the first time its cash rises by what rampage.sc pays.
// Under `own` each player keeps his, which is what single player pays him.
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
	// Our own cash under `shared`, whenever it changes, and `have` false when
	// there is none to speak of. A save made in the session writes it in
	// place of the wallet.
	void (*SetOwnMoney)(bool have, int32_t own) = nullptr;
	// Money for our own cash alone that the engine half has kept out of
	// m_nMoney since the last call: another player's hidden package under
	// `shared` (game/money.h, CreditOwnMoneyOnly), which the wallet already
	// has from him.
	int32_t (*DrainOwnCredit)() = nullptr;
};

// Our own cash under `shared`, moved by a change our own engine made: the
// wallet's own arithmetic, so it never goes below nothing (AddToMoneyPool).
// A teammate's changes never reach it.
inline int32_t OwnMoneyAfter(int32_t own, int32_t delta) { return AddToMoneyPool(own, delta); }

// What leaving `shared` puts in our pocket: our own cash, plus whatever our
// engine did to the cash since it was last read (`cash` against `written`,
// what we last left there) and so has not been counted yet.
inline int32_t OwnMoneyOnLeaving(int32_t own, int32_t cash, int32_t written) {
	int64_t delta = static_cast<int64_t>(cash) - written;
	if (delta > INT32_MAX)
		delta = INT32_MAX;
	if (delta < INT32_MIN)
		delta = INT32_MIN;
	return AddToMoneyPool(own, static_cast<int32_t>(delta));
}

// How many of our changes can be on their way at once. A change is an event -
// a pickup, a fine, a car - so a handful per round trip is a lot.
constexpr size_t MONEY_PENDING = 32;

// What rampage.sc's RAMPAGE_REWARDS pays: $5000 times the rampages this game
// has passed, the one just passed included, while that is under 20, and
// $1000000 for the twentieth (0109 ADD_SCORE, both arms).
inline bool IsRampageReward(int32_t delta) {
	return delta == 1000000 || (delta >= 5000 && delta <= 19 * 5000 && delta % 5000 == 0);
}

// How long after a shared rampage passes our own reward is looked for. The
// script is held on ONGOING until the verdict and pays in the same pass that
// reads it (game/darkel.cpp, HookedReadStatus), so this is slack for a
// machine whose frames are slow to come.
constexpr uint32_t RAMPAGE_REWARD_WAIT_MS = 10000;

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
	// A shared rampage's verdict, once our script has been handed it.
	void OnRampageEnd(const S_RampageEnd &pkt, uint8_t localPlayerId, uint32_t nowMs);
	// What our rampage.sc just paid for that pass, measured by the engine
	// half over the script's own pass (game/darkel.h, RewardPaid). When it is
	// known, exactly that much is taken back, and whatever else our cash did
	// in the same frame goes out as an ordinary change.
	void OnRampageRewardPaid(int32_t amount);

	// ---- per frame ----------------------------------------------------------
	// After the simulation: the awards our engine forwarded, and under
	// `shared` whatever our cash did this frame.
	void Send(uint8_t localPlayerId, uint32_t nowMs);

	// The session is gone: back to off. Under `shared` our own cash goes back
	// in our pocket in place of the wallet (OwnMoneyOnLeaving); under the
	// other rules the cash was always ours and stays where it is.
	void Clear();

	uint8_t Rule() const { return m_rule; }

	// ---- for the tests ------------------------------------------------------
	size_t  PendingCount() const { return m_pendingCount; }
	bool    WaitingForRampageReward() const { return m_rampageWait; }
	bool    HaveTotal() const { return m_haveTotal; }
	int32_t Total() const { return m_total; }
	bool    HaveOwn() const { return m_haveOwn; }
	int32_t Own() const { return m_own; }
	int32_t OwnCreditWaiting() const { return m_ownCredit; }

private:
	struct Pending {
		uint32_t seq   = 0;
		int32_t  delta = 0;
	};

	void PushSession(bool inSession);
	void PushOwn();
	void ResetWallet();
	// Leaving `shared`: our own cash back in place of the wallet.
	void GiveOwnBack();
	// DrainOwnCredit onto our own cash, once there is one for this life.
	void TakeOwnCredit();
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
	// Our own cash beside the wallet, for this life of the player: what it
	// held when we first read it, plus every change our engine made since.
	bool     m_haveOwn       = false;
	// Own-only money taken from the engine half before there was an own cash
	// to put it on.
	int32_t  m_ownCredit     = 0;
	int32_t  m_own           = 0;
	// A shared rampage somebody else was paid for has passed, and our own
	// script's reward for it has not been taken back yet.
	bool     m_rampageWait   = false;
	uint32_t m_rampageWaitMs = 0;
	// The reward our script paid, once the engine half has said; 0 until
	// then, and the reward is then told by its size alone.
	int32_t  m_rampagePaid   = 0;

	bool m_saidRule       = false;
	bool m_saidAdopted    = false;
	bool m_saidSeeded     = false;
	bool m_saidChange     = false;
	bool m_saidForwarded  = false;
	bool m_saidPaid       = false;
	bool m_saidNobody     = false;
	bool m_saidFull       = false;
	bool m_saidNewLife    = false;
	bool m_saidRampage    = false;
	bool m_saidOwnBack    = false;
};

} // namespace coopiii
