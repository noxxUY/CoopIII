// The session's one mission: who may start one, who it waits for, who is in
// it, and how it ends. docs/missions.md 5, 9 and 12; protocol.h has the wire
// and why each message exists.
//
// Kept apart from Session because none of it is about the roster's own
// bookkeeping. It asks the roster one question, where everybody is, and the
// caller answers it with a MissionPresence per connected player.
#pragma once

#include <coopiii/mission.h>
#include <coopiii/protocol.h>

#include <cstddef>
#include <cstdint>

namespace coopiii {

class MissionSlot {
public:
	struct ClaimAnswer {
		MissionClaimVerdict verdict = MISSION_CLAIM_BUSY;
		uint8_t             ownerId = INVALID_PLAYER;
		uint8_t             missing = 0;   // bit i: player i is not there yet
		// Granted without these: their game was in a mission of its own for
		// the whole of the busy wait (SetBusyWaitMs).
		uint8_t             leftOut = 0;
	};

	// The server's missionFailOnDeath and missionMargin.
	void     SetFailOnDeath(bool on) { m_failOnDeath = on; }
	void     SetMarginCm(uint16_t cm) { m_marginCm = cm; }
	void     SetEnemies(uint8_t rule, uint16_t scalePct) {
		m_enemies  = rule;
		m_scalePct = scalePct;
	}
	// How long a claim waits for players whose game is in a mission of its
	// own before it goes on without them. MISSION_BUSY_WAIT_MS unless a test
	// wants to see it happen.
	void     SetBusyWaitMs(uint32_t ms) { m_busyWaitMs = ms; }
	uint32_t BusyWaitMs() const { return m_busyWaitMs; }
	// The server's missionCheckpointWait and missionTimedCheckpoints, and who
	// is brought to the owner (missionCatchUp, missionFallBehind): all of it
	// the owner's machine and the participants' apply, and all of it in
	// S_MissionState. The wait is also what the countdown everybody is shown
	// runs from.
	void SetCheckpointRules(uint16_t waitS, bool timedToo, uint16_t catchUpM, uint16_t behindM,
	                        uint16_t behindS) {
		m_cpWaitS  = waitS;
		m_timedCp  = timedToo;
		m_catchUpM = catchUpM;
		m_behindM  = behindM;
		m_behindS  = behindS;
	}
	uint16_t CheckpointWaitS() const { return m_cpWaitS; }
	bool     TimedCheckpoints() const { return m_timedCp; }
	uint16_t CatchUpM() const { return m_catchUpM; }
	uint16_t BehindM() const { return m_behindM; }
	uint16_t BehindS() const { return m_behindS; }
	uint8_t  Enemies() const { return m_enemies; }
	uint16_t ScalePct() const { return m_scalePct; }
	bool     FailOnDeath() const { return m_failOnDeath; }
	uint16_t MarginCm() const { return m_marginCm; }

	// A claim at a start gate (C_MissionClaim). Granted when nothing else holds
	// the slot and every other player in `players` is inside the area or
	// within the start's radius of it (StartMarginMetres: 50 m, or the
	// margin when that is wider); a player nothing has placed yet is missing.
	// Refreshing a claim re-asks the question, so somebody who walks off
	// before the mission starts takes the grant back with them. A player
	// whose game is in a mission of its own is missing too, but once the
	// claim has waited the busy wait for nobody else, it goes on without them:
	// they come into the mission as a joiner does when their game is out.
	ClaimAnswer Claim(uint8_t playerId, const C_MissionClaim &claim,
	                  const MissionPresence *players, size_t count, uint32_t nowMs);

	// A claim nobody refreshed for MISSION_CLAIM_TTL_MS goes: its player
	// walked out of the marker. True when one did.
	bool Expire(uint32_t nowMs);

	// START_MISSION ran on `playerId`'s machine. Everybody in `connected`
	// (bit i: player i) is in it. A mission that started without its claim
	// being granted still becomes the session's, because it is running there
	// whatever the server thinks; one that starts while another runs does
	// not, and stays that machine's own. True when the session's mission
	// changed.
	bool Start(uint8_t playerId, uint16_t missionNumber, uint8_t connected);

	// The owner's mission ended. True when it was the owner saying so.
	bool End(uint8_t playerId, uint16_t missionNumber, uint8_t outcome);

	// A participant died or was busted. With the death rule on, the first
	// one in a mission is the order to fail it, in `out`, for the owner;
	// the owner's own death is the engine's to fail and never an order.
	bool FailFor(uint8_t playerId, MissionFailReason reason, S_MissionFail *out);

	// The owner at a checkpoint, waiting for `missing` (none: the wait is over).
	// Only the owner of a running mission is heard. The wait is counted down
	// from the first report at `where`: the owner's mission goes on without
	// whoever is still missing the server's checkpoint wait after it, and
	// everybody is told how long is left.
	bool Checkpoint(uint8_t playerId, uint8_t missing, const Vec3 &where, uint32_t nowMs = 0);

	// Somebody left. The owner leaving fails the mission for everybody
	// (missions.md 12.3); anybody else leaves the participants, their claim
	// and the list of who is waited for. True when the session's mission
	// changed.
	bool Leave(uint8_t playerId);

	// Somebody joined while a mission runs: they are in it. True when they
	// were added.
	bool Join(uint8_t playerId);

	// Somebody's game went into a mission of its own (C_MissionBusy): out of
	// the running mission, the owner's failing it as their leaving would, and
	// their claim let go of. Unlike leaving, they are still missing wherever
	// they were waited for. True when the session's mission changed.
	bool StandAside(uint8_t playerId);

	// Whether what `playerId`'s mission shows (C_MissionEffect) is relayed:
	// from the running mission's owner, or from the player whose claim is in
	// hand, whose trigger prints the title before the launch.
	bool MayRelayEffect(uint8_t playerId) const {
		if (m_state == MISSION_STATE_RUNNING)
			return playerId == m_owner;
		return m_claimed && playerId == m_claimOwner;
	}

	bool     Running() const { return m_state == MISSION_STATE_RUNNING; }
	uint8_t  Owner() const { return m_owner; }
	uint16_t Number() const { return m_number; }
	uint8_t  Participants() const { return m_participants; }

	// The session's mission as S_MissionState says it, header aside.
	S_MissionState State() const;

	// Who is waited for, when that changed since the last call: the
	// S_MissionWaiting everybody is sent, header aside. False when nothing
	// changed.
	bool TakeWaitingChange(S_MissionWaiting *out);

	// The claim in hand, for a test and the server's log.
	bool    HasClaim() const { return m_claimed; }
	uint8_t Claimant() const { return m_claimed ? m_claimOwner : INVALID_PLAYER; }

private:
	void SetWait(uint8_t owner, uint8_t missing, uint8_t what, uint16_t hint, const Vec3 &where,
	             uint8_t busy = 0, uint32_t goesOnAtMs = 0, uint16_t goesOnInS = 0);
	void ClearWait();

	bool     m_failOnDeath = true;                        // roadmap.md 5.4
	uint16_t m_marginCm    = MISSION_MARGIN_CM_DEFAULT;   // missions.md 9
	uint8_t  m_enemies     = MISSION_ENEMIES_ORIGINAL;    // missions.md 10
	uint16_t m_scalePct    = MISSION_SCALE_DEFAULT;
	uint32_t m_busyWaitMs  = MISSION_BUSY_WAIT_MS;
	uint16_t m_cpWaitS     = MISSION_CHECKPOINT_WAIT_MS / 1000;
	bool     m_timedCp     = false;
	uint16_t m_catchUpM    = MISSION_CATCH_UP_M_DEFAULT;
	uint16_t m_behindM     = MISSION_BEHIND_M_DEFAULT;
	uint16_t m_behindS     = MISSION_BEHIND_S_DEFAULT;

	uint8_t  m_state        = MISSION_STATE_IDLE;
	uint8_t  m_owner        = INVALID_PLAYER;
	uint16_t m_number       = MISSION_NONE;   // running, or the one that last ended
	uint8_t  m_participants = 0;
	uint8_t  m_outcome      = MISSION_OUTCOME_NONE;
	bool     m_failOrdered  = false;

	bool        m_claimed    = false;
	uint8_t     m_claimOwner = INVALID_PLAYER;
	uint32_t    m_claimKey   = 0;
	uint16_t    m_claimHint  = MISSION_NONE;
	MissionArea m_claimArea  = {};
	uint32_t    m_claimAtMs    = 0;   // last refreshed
	uint32_t    m_claimSinceMs = 0;   // first made, for the busy wait

	uint8_t  m_waitOwner   = INVALID_PLAYER;
	uint8_t  m_waitMissing = 0;
	uint8_t  m_waitWhat    = MISSION_WAIT_NONE;
	uint16_t m_waitHint    = MISSION_NONE;
	Vec3     m_waitWhere   = {};
	uint8_t  m_waitBusy    = 0;
	uint32_t m_waitGoesOnAt = 0;   // when the start goes on without m_waitBusy; 0 never
	uint16_t m_waitGoesOnIn = 0;   // the same, in seconds from when it was set
	bool     m_waitDirty   = false;
	uint32_t m_cpSinceMs   = 0;   // when the checkpoint waited for now began to
};

} // namespace coopiii
