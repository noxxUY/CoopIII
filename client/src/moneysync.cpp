#include "moneysync.h"

#include "log.h"

#include <coopiii/net.h>

namespace coopiii {

namespace {

const char *RuleName(uint8_t rule) {
	return rule == MONEY_RULE_SHARED ? "shared" : rule == MONEY_RULE_OWN ? "own" : "off";
}

} // namespace

// ---- inbound ----------------------------------------------------------------

void MoneySync::OnMoney(const S_Money &pkt, uint8_t localPlayerId) {
	if (localPlayerId == INVALID_PLAYER)
		return;

	const uint8_t rule = SaneMoneyRule(pkt.rule);
	if (rule != m_rule) {
		m_rule = rule;
		ResetWallet();
		PushSession(true);
		if (!m_saidRule) {
			m_saidRule = true;
			Log("money: the session's money rule is %s", RuleName(rule));
		}
	}
	if (rule != MONEY_RULE_SHARED)
		return;

	// Whatever our engine did since the last frame goes out first, so the
	// pending list is complete before the total is written over it.
	Observe(pkt.hdr.sendTimeMs);

	if ((pkt.flags & MONEY_POOL_SEEDED) == 0) {
		// Nobody has said what they have yet. Ours fills it, as soon as there
		// is a player to read it from; the server keeps the first and treats
		// any other as nothing.
		m_seedWanted = true;
		if (m_haveBaseline) {
			SendChange(0, m_written, pkt.hdr.sendTimeMs);
			m_seedWanted = false;
		}
		return;
	}

	DropAcked(pkt.ackSeq);
	m_total     = pkt.total;
	m_haveTotal = true;
	m_seedWanted = false;

	if (!m_haveBaseline || !m_bridge || !m_bridge->ReadMoney)
		return;   // written on the first read that works
	int32_t cash = 0, life = 0;
	if (!m_bridge->ReadMoney(cash, life) || life != m_life) {
		m_haveBaseline = false;
		return;
	}
	Adopt(cash);

	if (pkt.fromPlayerId != localPlayerId && pkt.fromPlayerId != INVALID_PLAYER &&
	    pkt.delta != 0 && !m_saidChange) {
		m_saidChange = true;
		Log("money: player %u's cash moved by %d; the session has $%d and so do we",
		    pkt.fromPlayerId, pkt.delta, pkt.total);
	}
}

void MoneySync::OnAward(const S_MoneyAward &pkt, uint8_t localPlayerId) {
	if (m_rule == MONEY_RULE_OFF || localPlayerId == INVALID_PLAYER)
		return;
	const MoneyAwardBody &in = pkt.body;
	if (in.toPlayerId != localPlayerId || !IsSaneMoneyAward(in.unit))
		return;
	if (!m_bridge || !m_bridge->PayExplosionAward)
		return;

	m_bridge->PayExplosionAward(in.unit);
	if (!m_saidPaid) {
		m_saidPaid = true;
		Log("money: player %u's game decided a car we %s and sent the reward "
		    "here; paid $%d a car, times our own chain",
		    pkt.fromPlayerId, in.kind == MONEY_AWARD_BOMB ? "bombed" : "set burning",
		    in.unit);
	}
}

// ---- per frame --------------------------------------------------------------

void MoneySync::Send(uint8_t localPlayerId, uint32_t nowMs) {
	if (localPlayerId == INVALID_PLAYER)
		return;
	SendAwards(localPlayerId, nowMs);
	if (m_rule == MONEY_RULE_SHARED)
		Observe(nowMs);
}

void MoneySync::SendAwards(uint8_t localPlayerId, uint32_t nowMs) {
	if (!m_bridge || !m_bridge->DrainMoneyAwards)
		return;
	LocalMoneyAward awards[8];
	const uint8_t   n = m_bridge->DrainMoneyAwards(awards, 8);
	if (m_rule == MONEY_RULE_OFF)
		return;   // the engine half queues nothing with the rule off anyway

	for (uint8_t i = 0; i < n; ++i) {
		const LocalMoneyAward &a = awards[i];
		uint8_t to = a.toUs ? localPlayerId : a.toPlayerId;
		if (to == INVALID_PLAYER && a.toNetId != INVALID_NETID && m_playerForNet)
			to = m_playerForNet(m_rosterCtx, a.toNetId);
		if (to == INVALID_PLAYER || !IsSaneMoneyAward(a.unit)) {
			if (!m_saidNobody) {
				m_saidNobody = true;
				Log("money: a wreck we decided was caused by a player who has "
				    "left; nobody is paid for it");
			}
			continue;
		}

		C_MoneyAward out;
		InitHeader(out, nowMs);
		out.body.toPlayerId = to;
		out.body.kind       = a.kind;
		out.body.model      = a.model;
		out.body.unit       = a.unit;
		out.body.key        = a.key;
		Out(out, CH_EVENT);
		if (!m_saidForwarded) {
			m_saidForwarded = true;
			Log("money: our game decided a wreck (model %u) that pays player %u; "
			    "$%d a car went to them instead of here",
			    a.model, to, a.unit);
		}
	}
}

void MoneySync::Clear() {
	if (m_bridge && m_bridge->DrainMoneyAwards) {
		LocalMoneyAward drop[8];
		while (m_bridge->DrainMoneyAwards(drop, 8) == 8) {
		}
	}
	m_rule = MONEY_RULE_OFF;
	ResetWallet();
	PushSession(false);
}

// ---- the shared wallet ------------------------------------------------------

void MoneySync::PushSession(bool inSession) {
	if (m_bridge && m_bridge->SetMoneySession)
		m_bridge->SetMoneySession(inSession, m_rule);
}

void MoneySync::ResetWallet() {
	m_haveTotal    = false;
	m_total        = 0;
	m_haveBaseline = false;
	m_life         = 0;
	m_written      = 0;
	m_seedWanted   = false;
	m_pendingCount = 0;
	// m_nextSeq carries on: the server remembers the last one it took from
	// this connection, and a number it has seen would be dropped.
}

void MoneySync::Observe(uint32_t nowMs) {
	if (m_rule != MONEY_RULE_SHARED || !m_bridge || !m_bridge->ReadMoney)
		return;
	int32_t cash = 0, life = 0;
	if (!m_bridge->ReadMoney(cash, life)) {
		m_haveBaseline = false;
		return;
	}

	if (!m_haveBaseline || life != m_life) {
		// A player we have not read before: the first frame of the session,
		// or a load or a new game since. Whatever it holds is not a change -
		// it's a save's money - so the pool is written over it.
		if (m_haveBaseline && !m_saidNewLife) {
			m_saidNewLife = true;
			Log("money: the player was rebuilt (a load or a new game); the "
			    "session's money is written over whatever the save had");
		}
		m_haveBaseline = true;
		m_life         = life;
		m_written      = cash;
		if (m_haveTotal) {
			Adopt(cash);
		} else if (m_seedWanted) {
			SendChange(0, cash, nowMs);
			m_seedWanted = false;
		}
		return;
	}

	if (cash == m_written)
		return;
	SendChange(cash - m_written, cash, nowMs);
	m_written = cash;
}

void MoneySync::SendChange(int32_t delta, int32_t have, uint32_t nowMs) {
	if (m_pendingCount == MONEY_PENDING) {
		// Nothing has come back for thirty-two changes. The oldest is let go
		// of here; the server still has it, and the next total says so.
		for (size_t i = 1; i < m_pendingCount; ++i)
			m_pending[i - 1] = m_pending[i];
		--m_pendingCount;
		if (!m_saidFull) {
			m_saidFull = true;
			Log("money: %zu changes are waiting on the server; the oldest is "
			    "no longer counted here", MONEY_PENDING);
		}
	}
	const uint32_t seq = m_nextSeq++;
	m_pending[m_pendingCount++] = Pending{seq, delta};

	C_MoneyChange out;
	InitHeader(out, nowMs);
	out.body.seq   = seq;
	out.body.delta = delta;
	out.body.have  = have;
	Out(out, CH_EVENT);

	if (delta == 0 && !m_haveTotal && !m_saidSeeded) {
		m_saidSeeded = true;
		Log("money: the session had no money yet; our $%d is where it starts", have);
	}
}

void MoneySync::Adopt(int32_t cash) {
	int32_t target = m_total;
	for (size_t i = 0; i < m_pendingCount; ++i)
		target = AddToMoneyPool(target, m_pending[i].delta);
	// Only m_nMoney: the HUD's counter walks to it on its own, the way it
	// does for money earned here.
	if (target != cash && m_bridge && m_bridge->WriteMoney)
		m_bridge->WriteMoney(target);
	m_written = target;
	if (!m_saidAdopted) {
		m_saidAdopted = true;
		Log("money: our cash is the session's now, $%d (we had $%d)", target, cash);
	}
}

void MoneySync::DropAcked(uint32_t ackSeq) {
	size_t keep = 0;
	for (size_t i = 0; i < m_pendingCount; ++i)
		if (m_pending[i].seq > ackSeq)
			m_pending[keep++] = m_pending[i];
	m_pendingCount = keep;
}

} // namespace coopiii
