#include "missionslot.h"

namespace coopiii {

MissionSlot::ClaimAnswer MissionSlot::Claim(uint8_t playerId, const C_MissionClaim &claim,
                                            const MissionPresence *players, size_t count,
                                            uint32_t nowMs) {
	ClaimAnswer answer;
	if (playerId >= MAX_PLAYERS)
		return answer;

	if (m_state == MISSION_STATE_RUNNING) {
		answer.ownerId = m_owner;
		return answer;
	}
	// Somebody else is standing in a marker of their own. Theirs until it
	// lapses: two players can't both be waited for at once.
	if (m_claimed && m_claimOwner != playerId &&
	    static_cast<uint32_t>(nowMs - m_claimAtMs) < MISSION_CLAIM_TTL_MS) {
		answer.ownerId = m_claimOwner;
		return answer;
	}

	// The same gate asked again is the same claim, however long it has
	// waited; another gate, another player, or one that lapsed is a new one.
	const bool same = m_claimed && m_claimOwner == playerId && m_claimKey == claim.launchKey &&
	                  static_cast<uint32_t>(nowMs - m_claimAtMs) < MISSION_CLAIM_TTL_MS;
	m_claimed    = true;
	m_claimOwner = playerId;
	m_claimKey   = claim.launchKey;
	m_claimHint  = claim.missionHint;
	m_claimArea  = claim.area;
	m_claimAtMs  = nowMs;
	if (!same)
		m_claimSinceMs = nowMs;

	// The start's radius, not the checkpoints' margin: a friend in his own
	// car across the street is at the start (MISSION_START_RADIUS_M).
	const float margin  = StartMarginMetres(m_marginCm);
	uint8_t     missing = 0;
	uint8_t     busy    = 0;
	for (size_t i = 0; i < count; ++i) {
		const MissionPresence &p = players[i];
		if (p.playerId == playerId || p.playerId >= MAX_PLAYERS)
			continue;
		if (!p.havePos || !InMissionArea(p.pos, m_claimArea, margin)) {
			missing |= PlayerBit(p.playerId);
			if (p.busy)
				busy |= PlayerBit(p.playerId);
		}
	}

	// Waiting for nobody but games in a mission of their own: not for ever.
	const uint32_t waited   = nowMs - m_claimSinceMs;
	const bool     onlyBusy = missing != 0 && missing == busy;
	if (onlyBusy && waited >= m_busyWaitMs) {
		answer.leftOut = missing;
		missing        = 0;
	}

	answer.ownerId = playerId;
	answer.missing = missing;
	if (missing == 0) {
		answer.verdict = MISSION_CLAIM_GRANTED;
		ClearWait();
	} else {
		answer.verdict = MISSION_CLAIM_WAITING;
		uint32_t goesOnAt = 0;
		uint16_t goesOnIn = 0;
		if (onlyBusy) {
			const uint32_t left = (m_busyWaitMs - waited + 999) / 1000;
			goesOnAt = m_claimSinceMs + m_busyWaitMs;
			goesOnAt = goesOnAt != 0 ? goesOnAt : 1;
			goesOnIn = static_cast<uint16_t>(left < 0xFFFF ? left : 0xFFFF);
		}
		SetWait(playerId, missing, MISSION_WAIT_START, claim.missionHint, m_claimArea.centre,
		        busy, goesOnAt, goesOnIn);
	}
	return answer;
}

bool MissionSlot::Expire(uint32_t nowMs) {
	if (!m_claimed || static_cast<uint32_t>(nowMs - m_claimAtMs) < MISSION_CLAIM_TTL_MS)
		return false;
	m_claimed    = false;
	m_claimOwner = INVALID_PLAYER;
	if (m_waitWhat == MISSION_WAIT_START)
		ClearWait();
	return true;
}

bool MissionSlot::Start(uint8_t playerId, uint16_t missionNumber, uint8_t connected) {
	if (playerId >= MAX_PLAYERS || m_state == MISSION_STATE_RUNNING)
		return false;
	m_state        = MISSION_STATE_RUNNING;
	m_owner        = playerId;
	m_number       = missionNumber;
	m_participants = static_cast<uint8_t>(connected | PlayerBit(playerId));
	m_outcome      = MISSION_OUTCOME_NONE;
	m_failOrdered  = false;
	m_claimed      = false;
	m_claimOwner   = INVALID_PLAYER;
	ClearWait();
	return true;
}

bool MissionSlot::End(uint8_t playerId, uint16_t missionNumber, uint8_t outcome) {
	if (m_state != MISSION_STATE_RUNNING || playerId != m_owner)
		return false;
	(void)missionNumber;   // the owner's mission ended, whatever it says it was
	m_state        = MISSION_STATE_IDLE;
	m_outcome      = outcome == MISSION_OUTCOME_PASSED ? MISSION_OUTCOME_PASSED
	                                                   : MISSION_OUTCOME_FAILED;
	m_participants = 0;
	ClearWait();
	return true;
}

bool MissionSlot::FailFor(uint8_t playerId, MissionFailReason reason, S_MissionFail *out) {
	if (m_state != MISSION_STATE_RUNNING || !m_failOnDeath || m_failOrdered)
		return false;
	if (playerId == m_owner || (m_participants & PlayerBit(playerId)) == 0)
		return false;
	m_failOrdered = true;
	if (out) {
		out->missionNumber = m_number;
		out->reason        = static_cast<uint8_t>(reason);
		out->playerId      = playerId;
	}
	return true;
}

bool MissionSlot::Checkpoint(uint8_t playerId, uint8_t missing, const Vec3 &where,
                             uint32_t nowMs) {
	if (m_state != MISSION_STATE_RUNNING || playerId != m_owner)
		return false;
	missing = static_cast<uint8_t>(missing & m_participants & ~PlayerBit(m_owner));
	if (missing == 0) {
		ClearWait();
		return true;
	}
	// The same checkpoint with somebody else still missing is the same wait,
	// and its time keeps running down.
	const bool same = m_waitWhat == MISSION_WAIT_CHECKPOINT && m_waitWhere.x == where.x &&
	                  m_waitWhere.y == where.y && m_waitWhere.z == where.z;
	if (!same)
		m_cpSinceMs = nowMs;
	const uint32_t waited = nowMs - m_cpSinceMs;
	const uint32_t waitMs = static_cast<uint32_t>(m_cpWaitS) * 1000u;
	const uint32_t left   = waited < waitMs ? waitMs - waited : 0;
	uint32_t goesOnAt     = m_cpSinceMs + waitMs;
	goesOnAt          = goesOnAt != 0 ? goesOnAt : 1;
	const uint32_t inS = (left + 999) / 1000;
	SetWait(m_owner, missing, MISSION_WAIT_CHECKPOINT, m_number, where, 0, goesOnAt,
	        static_cast<uint16_t>(inS == 0 ? 1 : (inS < 0xFFFF ? inS : 0xFFFF)));
	return true;
}

bool MissionSlot::Leave(uint8_t playerId) {
	const uint8_t bit = PlayerBit(playerId);
	if (bit == 0)
		return false;

	if (m_claimed && m_claimOwner == playerId) {
		m_claimed    = false;
		m_claimOwner = INVALID_PLAYER;
		if (m_waitWhat == MISSION_WAIT_START)
			ClearWait();
	}
	if (m_waitWhat != MISSION_WAIT_NONE && (m_waitMissing & bit) != 0) {
		const uint8_t still = static_cast<uint8_t>(m_waitMissing & ~bit);
		if (still == 0)
			ClearWait();
		else
			SetWait(m_waitOwner, still, m_waitWhat, m_waitHint, m_waitWhere,
			        static_cast<uint8_t>(m_waitBusy & still), m_waitGoesOnAt, m_waitGoesOnIn);
	}

	if (m_state != MISSION_STATE_RUNNING)
		return false;
	if (playerId == m_owner) {
		m_state        = MISSION_STATE_IDLE;
		m_outcome      = MISSION_OUTCOME_OWNER_LEFT;
		m_participants = 0;
		ClearWait();
		return true;
	}
	if ((m_participants & bit) == 0)
		return false;
	m_participants = static_cast<uint8_t>(m_participants & ~bit);
	return true;
}

bool MissionSlot::StandAside(uint8_t playerId) {
	const uint8_t bit = PlayerBit(playerId);
	if (bit == 0)
		return false;
	if (m_claimed && m_claimOwner == playerId) {
		m_claimed    = false;
		m_claimOwner = INVALID_PLAYER;
		if (m_waitWhat == MISSION_WAIT_START)
			ClearWait();
	}
	if (m_state != MISSION_STATE_RUNNING)
		return false;
	if (playerId == m_owner) {
		m_state        = MISSION_STATE_IDLE;
		m_outcome      = MISSION_OUTCOME_OWNER_LEFT;
		m_participants = 0;
		ClearWait();
		return true;
	}
	if ((m_participants & bit) == 0)
		return false;
	m_participants = static_cast<uint8_t>(m_participants & ~bit);
	return true;
}

bool MissionSlot::Join(uint8_t playerId) {
	const uint8_t bit = PlayerBit(playerId);
	if (m_state != MISSION_STATE_RUNNING || bit == 0 || (m_participants & bit) != 0)
		return false;
	m_participants = static_cast<uint8_t>(m_participants | bit);
	return true;
}

S_MissionState MissionSlot::State() const {
	S_MissionState s{};
	s.state         = m_state;
	s.ownerId       = m_owner;
	s.missionNumber = m_number;
	s.participants  = m_participants;
	s.flags         = m_failOnDeath ? MISSION_FLAG_FAIL_ON_DEATH : 0;
	s.outcome       = m_state == MISSION_STATE_RUNNING ? MISSION_OUTCOME_NONE : m_outcome;
	s.marginCm      = m_marginCm;
	s.enemies       = m_enemies;
	s.scalePct      = m_scalePct;
	s.checkpointWaitS = m_cpWaitS;
	s.catchUpM        = m_catchUpM;
	s.behindM         = m_behindM;
	s.behindS         = m_behindS;
	if (m_timedCp)
		s.flags = static_cast<uint8_t>(s.flags | MISSION_FLAG_TIMED_CHECKPOINTS);
	return s;
}

bool MissionSlot::TakeWaitingChange(S_MissionWaiting *out) {
	if (!m_waitDirty)
		return false;
	m_waitDirty = false;
	if (out) {
		out->ownerId     = m_waitOwner;
		out->missingMask = m_waitMissing;
		out->what        = m_waitWhat;
		out->missionHint = m_waitHint;
		out->where       = m_waitWhere;
		out->busyMask    = m_waitBusy;
		out->goesOnInS   = m_waitGoesOnIn;
	}
	return true;
}

void MissionSlot::SetWait(uint8_t owner, uint8_t missing, uint8_t what, uint16_t hint,
                          const Vec3 &where, uint8_t busy, uint32_t goesOnAtMs,
                          uint16_t goesOnInS) {
	if (m_waitOwner == owner && m_waitMissing == missing && m_waitWhat == what &&
	    m_waitHint == hint && m_waitWhere.x == where.x && m_waitWhere.y == where.y &&
	    m_waitWhere.z == where.z && m_waitBusy == busy && m_waitGoesOnAt == goesOnAtMs)
		return;
	m_waitOwner    = owner;
	m_waitMissing  = missing;
	m_waitWhat     = what;
	m_waitHint     = hint;
	m_waitWhere    = where;
	m_waitBusy     = busy;
	m_waitGoesOnAt = goesOnAtMs;
	m_waitGoesOnIn = goesOnInS;
	m_waitDirty    = true;
}

void MissionSlot::ClearWait() {
	if (m_waitWhat == MISSION_WAIT_NONE)
		return;
	m_waitOwner    = INVALID_PLAYER;
	m_waitMissing  = 0;
	m_waitWhat     = MISSION_WAIT_NONE;
	m_waitHint     = MISSION_NONE;
	m_waitWhere    = {};
	m_waitBusy     = 0;
	m_waitGoesOnAt = 0;
	m_waitGoesOnIn = 0;
	m_waitDirty    = true;
}

} // namespace coopiii
