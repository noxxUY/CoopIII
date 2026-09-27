#include "missionsync.h"

#include "log.h"

#include <coopiii/net.h>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace coopiii {

// An ADD_SCORE that takes money rather than pays it, as the owner's replay
// encodes it (game/replay.h): the opcode, then the player and the amount,
// each a 32-bit literal. What a mission charges is the owner's alone; an
// owner of an older build still sends it, and it is not paid here either.
static bool IsCharge(const MissionEffectBody &body) {
	constexpr uint8_t INT32 = 1;
	if (body.length < 12 || body.code[0] != 0x09 || body.code[1] != 0x01 || body.code[7] != INT32)
		return false;
	int32_t amount = 0;
	std::memcpy(&amount, body.code + 8, 4);
	return amount < 0;
}

// ---- the engine half asks -----------------------------------------------------

bool MissionSync::AskStartGate(uint32_t launchKey, uint8_t kind, uint16_t hint,
                               const MissionArea &area, uint8_t localPlayerId, uint32_t nowMs) {
	// A server that has never said what the session's mission is does not
	// share them, and would never answer: the gate is the script's alone.
	if (!m_serverShares || localPlayerId >= MAX_PLAYERS)
		return true;
	if (m_state == MISSION_STATE_RUNNING)
		return false;

	if (m_haveVerdict && m_verdictKey == launchKey && m_verdict == MISSION_CLAIM_GRANTED &&
	    static_cast<uint32_t>(nowMs - m_verdictAtMs) < MISSION_CLAIM_TTL_MS)
		return true;

	if (!m_claimLive || m_claimKey != launchKey ||
	    static_cast<uint32_t>(nowMs - m_claimSentMs) >= MISSION_CLAIM_REFRESH_MS) {
		if (m_claimKey != launchKey)
			m_haveVerdict = false;
		C_MissionClaim claim{};
		InitHeader(claim, nowMs);
		claim.launchKey   = launchKey;
		claim.missionHint = hint;
		claim.kind        = kind;
		claim.area        = area;
		Out(claim, CH_EVENT);
		m_claimKey    = launchKey;
		m_claimLive   = true;
		m_claimSentMs = nowMs;
		if (m_claimsSent++ == 0)
		{
			char at[48];
			SessionAt(at, sizeof at);
			Log("missions: claimed the session's mission at a start gate (%s)%s", MissionName(hint), at);
		}
	}
	return false;
}

void MissionSync::Launched(uint32_t launchKey, uint16_t missionNumber, uint32_t nowMs) {
	if (!m_serverShares)
		return;
	C_MissionStarted out{};
	InitHeader(out, nowMs);
	out.launchKey     = launchKey;
	out.missionNumber = missionNumber;
	Out(out, CH_EVENT);
	m_claimLive   = false;
	m_haveVerdict = false;
	// Its first loads come before the server's answer does, and are waited on
	// all the same.
	m_launchPending  = true;
	m_launchedNumber = missionNumber;
	for (uint16_t &r : m_ready)
		r = 0;
	m_readyAsked  = 0;
	m_readyGaveUp = false;
	char at[48];
	SessionAt(at, sizeof at);
	Log("missions: %s started here%s", MissionName(missionNumber), at);
}

void MissionSync::Ended(uint16_t missionNumber, uint8_t outcome, uint32_t nowMs) {
	m_launchPending = false;
	// Over while the connection was down, or before the server took it up
	// again: nothing is left to offer.
	m_readopt     = false;
	m_readoptSent = false;
	if (!m_serverShares)
		return;
	C_MissionEnded out{};
	InitHeader(out, nowMs);
	out.missionNumber = missionNumber;
	out.outcome       = outcome == MISSION_OUTCOME_PASSED ? MISSION_OUTCOME_PASSED
	                                                      : MISSION_OUTCOME_FAILED;
	Out(out, CH_EVENT);
	char at[48];
	SessionAt(at, sizeof at);
	Log("missions: %s %s here%s", MissionName(missionNumber),
	    out.outcome == MISSION_OUTCOME_PASSED ? "was passed" : "failed", at);
}

void MissionSync::SendEffect(const MissionEffectBody &body, uint32_t nowMs) {
	if (!m_serverShares || body.length > MISSION_EFFECT_CODE)
		return;
	// Meant for a player there cannot be: to everybody instead it would be
	// shown twice to every one of them.
	if (body.onlyTo > MAX_PLAYERS)
		return;
	C_MissionEffect out;
	InitHeader(out, nowMs);
	out.body = body;
	Out(out, CH_EVENT);
}

void MissionSync::SendCampaignDelta(const CampaignDeltaBody &body, uint32_t nowMs) {
	if (!m_serverShares || body.valueCount > CAMPAIGN_VALUES ||
	    body.threadCount > CAMPAIGN_THREADS || body.opLength > MISSION_EFFECT_CODE)
		return;
	C_CampaignDelta out;
	InitHeader(out, nowMs);
	out.body     = body;
	out.body.seq = 0;
	Out(out, CH_EVENT);
}

void MissionSync::WidgetValue(uint16_t offset, int32_t value, bool timer, bool frozen,
                              uint8_t localPlayerId, uint32_t nowMs) {
	if (!m_serverShares || m_state != MISSION_STATE_RUNNING || m_owner != localPlayerId ||
	    offset == 0)
		return;
	WidgetSent *w = nullptr;
	for (WidgetSent &x : m_widgets)
		if (x.sent && x.offset == offset)
			w = &x;
	if (!w) {
		// A widget we have not sent yet: into a free slot, or over the one
		// of the other kind's that has gone quiet the longest.
		w = &m_widgets[0];
		for (WidgetSent &x : m_widgets)
			if (!x.sent || (w->sent && x.atMs - w->atMs > 0x80000000u))
				w = &x;
		*w = WidgetSent{};
	}
	const uint32_t since = nowMs - w->atMs;
	if (w->sent && since < MISSION_WIDGET_MIN_MS)
		return;
	bool due = !w->sent || since >= MISSION_WIDGET_RESYNC_MS || frozen != w->frozen;
	if (!due) {
		if (timer) {
			// The HUD counts a timer down by itself, unless it is frozen.
			const int64_t expected =
			    w->frozen ? w->value : (static_cast<int64_t>(w->value) - static_cast<int64_t>(since));
			const int64_t off = static_cast<int64_t>(value) - (expected < 0 ? 0 : expected);
			due = off > MISSION_WIDGET_DRIFT_MS || off < -MISSION_WIDGET_DRIFT_MS;
		} else {
			due = value != w->value;
		}
	}
	if (!due)
		return;
	C_MissionWidget out{};
	InitHeader(out, nowMs);
	out.missionNumber = m_number;
	out.offset        = offset;
	out.value         = value;
	Out(out, CH_EVENT);
	w->offset = offset;
	w->sent   = true;
	w->timer  = timer;
	w->frozen = frozen;
	w->value  = value;
	w->atMs   = nowMs;
	++m_widgetsSent;
}

bool MissionSync::AskCheckpoint(const MissionArea &area, uint8_t localPlayerId, uint32_t nowMs) {
	if (!m_serverShares || m_state != MISSION_STATE_RUNNING || m_owner != localPlayerId)
		return true;
	m_cpAskedMs = nowMs;

	MissionPresence others[MAX_PLAYERS];
	const size_t    n      = m_presence ? m_presence(m_rosterCtx, others, MAX_PLAYERS) : 0;
	const float     margin = MarginMetres(m_marginCm);
	uint8_t         missing = 0;
	// A timed or raced checkpoint is the owner's alone: nobody is counted
	// missing from it, and whoever is far behind is brought along instead.
	const bool waits = CheckpointsWaitNow();
	if (!waits && !m_cpSaidNoWait) {
		m_cpSaidNoWait = true;
		Statusf("%s's checkpoints don't wait for anybody: %s", MissionName(m_number),
		        m_cpWaitS == 0 ? "the server says so"
		        : m_timerUp    ? "its clock is running"
		                       : "it is a race or a side job");
	}
	for (size_t i = 0; waits && i < n; ++i) {
		const uint8_t id = others[i].playerId;
		if (id == localPlayerId || (m_participants & PlayerBit(id)) == 0 ||
		    (m_cpOutOfReach & PlayerBit(id)) != 0)
			continue;
		if (!others[i].havePos || !InMissionArea(others[i].pos, area, margin))
			missing = static_cast<uint8_t>(missing | PlayerBit(id));
	}

	// How long this checkpoint has waited, counted from the first time
	// somebody was missing from it; another checkpoint is another wait.
	const bool elsewhere = m_cpAt.x != area.centre.x || m_cpAt.y != area.centre.y ||
	                       m_cpAt.z != area.centre.z;
	m_cpAt = area.centre;
	if (elsewhere)
		m_cpGaveUp = false;
	if (missing == 0) {
		m_cpWaiting = false;
	} else if (!m_cpWaiting || elsewhere) {
		m_cpWaiting = true;
		m_cpSinceMs = nowMs;
	}
	if (missing != 0 && !m_cpGaveUp &&
	    static_cast<uint32_t>(nowMs - m_cpSinceMs) >= static_cast<uint32_t>(m_cpWaitS) * 1000u) {
		// Nobody waits for ever: whoever has not come by now is left behind,
		// and the mission, and its own timer, go on.
		m_cpGaveUp = true;
		++m_cpGivenUp;
		char who[160];
		Names(who, sizeof who, missing, localPlayerId);
		Statusf("%s went on without %s, who did not reach the checkpoint in %u s",
		        MissionName(m_number), who, static_cast<unsigned>(m_cpWaitS));
	}
	const bool    goOn   = missing == 0 || m_cpGaveUp;
	const uint8_t report = goOn ? 0 : missing;

	const bool moved = m_cpWhere.x != area.centre.x || m_cpWhere.y != area.centre.y ||
	                   m_cpWhere.z != area.centre.z;
	if (report != m_cpReported || (report != 0 && moved)) {
		C_MissionCheckpoint out{};
		InitHeader(out, nowMs);
		out.missingMask = report;
		out.where       = area.centre;
		Out(out, CH_EVENT);
		m_cpReported = report;
		m_cpWhere    = area.centre;
	}
	return goOn;
}

bool MissionSync::AskEverybodyLoaded(uint16_t readySeq, uint8_t localPlayerId, uint32_t nowMs) {
	// Somebody else's running is the session's, and ours stays our own.
	const bool ours   = m_state == MISSION_STATE_RUNNING && m_owner == localPlayerId;
	const bool theirs = m_state == MISSION_STATE_RUNNING && m_owner != localPlayerId;
	if (!m_serverShares || readySeq == 0 || theirs || !(ours || m_launchPending))
		return true;
	if (readySeq != m_readyAsked) {
		m_readyAsked   = readySeq;
		m_readySinceMs = nowMs;
		m_readyGaveUp  = false;
	}
	if (m_readyGaveUp)
		return true;
	// Before the server has said the mission is the session's, who is in it
	// is not known yet: everybody else still counts as loading.
	uint8_t missing = 0;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
		if (id != localPlayerId && (!ours || (m_participants & PlayerBit(id)) != 0) &&
		    m_ready[id] < readySeq && (ours || (m_nick && m_nick(m_rosterCtx, id) != nullptr)))
			missing = static_cast<uint8_t>(missing | PlayerBit(id));
	if (missing == 0)
		return true;
	if (nowMs - m_readySinceMs < MISSION_READY_WAIT_MS)
		return false;
	// Somebody whose game says nothing, an older build, or one that is
	// stuck: the cutscene goes on without them, as it would have.
	m_readyGaveUp = true;
	char who[160];
	Names(who, sizeof who, missing, localPlayerId);
	Log("missions: %s did not say their models were in within %u ms; %s goes on",
	    who, static_cast<unsigned>(MISSION_READY_WAIT_MS), MissionName(m_number));
	return true;
}

void MissionSync::SeatsNeeded(const MissionSeatCar *cars, size_t count, uint8_t localPlayerId,
                              uint32_t nowMs) {
	if (!m_serverShares || m_state != MISSION_STATE_RUNNING || m_owner != localPlayerId)
		return;
	if (count > MISSION_SEAT_CARS)
		count = MISSION_SEAT_CARS;
	bool same = count == m_seatsSentCount;
	for (size_t i = 0; same && i < count; ++i)
		same = cars[i].netId == m_seatsSent[i].netId && cars[i].flags == m_seatsSent[i].flags &&
		       cars[i].leave == m_seatsSent[i].leave;
	if (same && (count == 0 || nowMs - m_seatsSentMs < MISSION_SEATS_RESYNC_MS))
		return;
	C_MissionSeats out{};
	InitHeader(out, nowMs);
	out.missionNumber = m_number;
	out.count         = static_cast<uint8_t>(count);
	for (size_t i = 0; i < count; ++i) {
		out.cars[i]    = cars[i];
		m_seatsSent[i] = cars[i];
	}
	m_seatsSentCount = static_cast<uint8_t>(count);
	m_seatsSentMs    = nowMs;
	Out(out, CH_EVENT);
}

void MissionSync::PickupTaken(int32_t ownerHandle, uint32_t nowMs) {
	if (!m_serverShares || m_state != MISSION_STATE_RUNNING)
		return;
	C_MissionPickup out{};
	InitHeader(out, nowMs);
	out.missionNumber = m_number;
	out.handle        = ownerHandle;
	Out(out, CH_EVENT);
}

void MissionSync::KillRegistered(uint16_t model, uint8_t localPlayerId, uint32_t nowMs) {
	if (!m_serverShares || m_busy || m_state != MISSION_STATE_RUNNING || m_owner == localPlayerId ||
	    (m_participants & PlayerBit(localPlayerId)) == 0)
		return;
	C_MissionKill out{};
	InitHeader(out, nowMs);
	out.missionNumber = m_number;
	out.model         = model;
	Out(out, CH_EVENT);
}

void MissionSync::Answers(uint32_t hasCar, uint32_t resprayed, uint16_t shotDown,
                          uint8_t localPlayerId, uint32_t nowMs) {
	if (!m_serverShares || m_busy || m_state != MISSION_STATE_RUNNING || m_owner == localPlayerId ||
	    (m_participants & PlayerBit(localPlayerId)) == 0)
		return;
	if (hasCar == m_garageSent && resprayed == 0 && shotDown == 0)
		return;
	m_garageSent = hasCar;
	C_MissionAnswers out{};
	InitHeader(out, nowMs);
	out.missionNumber = m_number;
	out.shotDown      = shotDown;
	out.hasCar        = hasCar;
	out.resprayed     = resprayed;
	Out(out, CH_EVENT);
}

bool MissionSync::TakeShotDownElsewhere(uint16_t plane) {
	if (m_state != MISSION_STATE_RUNNING || (m_shotDown & plane) == 0)
		return false;
	m_shotDown = static_cast<uint16_t>(m_shotDown & ~plane);
	return true;
}

bool MissionSync::GarageHasCarElsewhere(uint8_t garage) const {
	if (garage >= MISSION_GARAGES || m_state != MISSION_STATE_RUNNING)
		return false;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
		if ((m_participants & PlayerBit(id)) != 0 && (m_garageHasCar[id] & (1u << garage)) != 0)
			return true;
	return false;
}

bool MissionSync::TakeResprayElsewhere(uint8_t garage) {
	if (garage >= MISSION_GARAGES || (m_garageResprays & (1u << garage)) == 0)
		return false;
	m_garageResprays &= ~(1u << garage);
	return true;
}

void MissionSync::SendBusy(bool fresh, uint32_t nowMs) {
	C_MissionBusy out{};
	InitHeader(out, nowMs);
	out.busy  = m_busy ? 1 : 0;
	out.fresh = fresh ? 1 : 0;
	Out(out, CH_EVENT);
	m_busySent = m_busy;
}

void MissionSync::SetBusy(bool busy, uint32_t nowMs) {
	if (busy != m_busy) {
		char at[48];
		SessionAt(at, sizeof at);
		Log(busy ? "missions: this game is in a mission of its own; the session's goes on without it%s"
		         : "missions: this game is out of its own mission%s",
		    at);
	}
	m_busy = busy;
	// Told once the server has said it shares missions (OnState), and on
	// every change after that.
	if (m_serverShares && m_busy != m_busySent)
		SendBusy(false, nowMs);
}

void MissionSync::StartedOver(uint8_t localPlayerId, uint32_t nowMs) {
	if (!m_serverShares || m_state != MISSION_STATE_RUNNING || m_owner == localPlayerId ||
	    (m_participants & PlayerBit(localPlayerId)) == 0)
		return;
	Log("missions: this game started over in the middle of %s, and asks for what it has up",
	    MissionName(m_number));
	// The handles those were for mean nothing in the new game.
	m_awaiting.clear();
	SendBusy(true, nowMs);
	AskCatchUp(nowMs);
}

void MissionSync::HandOver(uint8_t playerId, uint32_t nowMs) {
	m_ready[playerId] = 0;   // whoever had this id before has loaded nothing for them
	if (m_bridge && m_bridge->ResendStanding)
		m_bridge->ResendStanding(playerId);
	++m_standingHanded;
	for (WidgetSent &w : m_widgets)
		if (w.sent)
			w.atMs = nowMs - MISSION_WIDGET_RESYNC_MS;
}

void MissionSync::ObjectBroken(uint16_t global, float amount, uint8_t state, uint32_t nowMs) {
	if (!m_serverShares || m_state != MISSION_STATE_RUNNING || global == 0)
		return;
	C_MissionObjectBreak out{};
	InitHeader(out, nowMs);
	out.missionNumber = m_number;
	out.global        = global;
	out.amount        = amount;
	out.state         = state;
	Out(out, CH_EVENT);
}

const MissionSeatCar *MissionSync::SeatKept(uint16_t netId) const {
	for (uint8_t i = 0; i < m_seatsKeptCount; ++i)
		if (m_seatsKept[i].netId == netId && (m_seatsKept[i].flags & MISSION_SEAT_KEPT) != 0)
			return &m_seatsKept[i];
	return nullptr;
}

bool MissionSync::MaySit(uint16_t netId, uint32_t nowMs) {
	if (!SeatKept(netId))
		return true;
	if (!m_seatRefusedSaid || nowMs - m_seatRefusedMs >= MISSION_WAIT_REMIND_MS / 5) {
		m_seatRefusedSaid = true;
		m_seatRefusedMs   = nowMs;
		Statusf("the seats in that car are for %s's mission", NickOf(m_owner));
	}
	return false;
}

bool MissionSync::MustLeaveSeat(uint16_t netId, uint8_t localPlayerId) {
	const MissionSeatCar *car = SeatKept(netId);
	// Named, or nobody named: an owner of protocol 54 or older names nobody
	// and means everybody riding in it.
	const bool named = car && localPlayerId < MAX_PLAYERS &&
	                   (car->leave == 0 || (car->leave & PlayerBit(localPlayerId)) != 0);
	if (!car || (car->flags & MISSION_SEAT_LEAVE) == 0 || !named) {
		if (m_seatLeaveSaid == netId)
			m_seatLeaveSaid = INVALID_NETID;
		return false;
	}
	if (m_seatLeaveSaid != netId) {
		m_seatLeaveSaid = netId;
		Statusf("you got out: %s's mission needs your seat", NickOf(m_owner));
	}
	return true;
}

void MissionSync::ForgetSeats() {
	m_seatsSentCount  = 0;
	m_seatsKeptCount  = 0;
	m_seatRefusedSaid = false;
	m_seatLeaveSaid   = INVALID_NETID;
	m_board           = BoardSeat{};
	m_cpOutOfReach    = 0;
}

void MissionSync::Board(uint16_t netId, const uint8_t *seats, uint8_t flags, uint8_t localPlayerId,
                        uint32_t nowMs) {
	if (!m_serverShares || m_state != MISSION_STATE_RUNNING || m_owner != localPlayerId ||
	    netId == INVALID_NETID)
		return;
	C_MissionBoard out{};
	InitHeader(out, nowMs);
	out.missionNumber = m_number;
	out.netId         = netId;
	out.flags         = flags;
	uint8_t seated = 0, left = 0;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		if (id == localPlayerId || (m_participants & PlayerBit(id)) == 0)
			continue;
		out.seats[id] = seats[id];
		if (seats[id] != 0)
			seated = static_cast<uint8_t>(seated | PlayerBit(id));
		else
			left = static_cast<uint8_t>(left | PlayerBit(id));
	}
	Out(out, CH_EVENT);
	char who[160];
	if (seated != 0) {
		Names(who, sizeof who, seated, localPlayerId);
		Statusf("%s put its player in vehicle %u, and a seat in it for %s", MissionName(m_number),
		        netId, who);
	}
	if (left != 0) {
		Names(who, sizeof who, left, localPlayerId);
		Statusf("%s put its player in vehicle %u, with no seat left for %s%s", MissionName(m_number),
		        netId, who, (flags & MISSION_BOARD_WATER) != 0 ? " on the water" : "");
	}
}

bool MissionSync::BoardPending(uint16_t &netId, uint8_t &seat, uint16_t &givenToOthers,
                               uint32_t &sinceMs) const {
	if (!m_board.live || m_state != MISSION_STATE_RUNNING || m_busy)
		return false;
	netId         = m_board.netId;
	seat          = m_board.seat;
	givenToOthers = m_board.givenToOthers;
	sinceMs       = m_board.sinceMs;
	return true;
}

void MissionSync::SetOutOfReach(uint8_t mask, uint8_t localPlayerId) {
	if (m_state != MISSION_STATE_RUNNING || m_owner != localPlayerId)
		mask = 0;
	mask = static_cast<uint8_t>(mask & m_participants);
	if (mask == m_cpOutOfReach)
		return;
	const uint8_t gone = static_cast<uint8_t>(mask & ~m_cpOutOfReach);
	m_cpOutOfReach     = mask;
	if (gone != 0) {
		char who[160];
		Names(who, sizeof who, gone, localPlayerId);
		Statusf("%s's checkpoints on the water don't wait for %s, who has no boat",
		        MissionName(m_number), who);
	}
}

// ---- inbound ------------------------------------------------------------------

void MissionSync::OnClaim(const S_MissionClaim &pkt, uint8_t localPlayerId, uint32_t nowMs) {
	(void)localPlayerId;
	if (!m_claimLive || pkt.launchKey != m_claimKey)
		return;
	m_verdict     = pkt.verdict;
	m_verdictKey  = pkt.launchKey;
	m_verdictAtMs = nowMs;
	m_haveVerdict = true;
	if (pkt.verdict != MISSION_CLAIM_BUSY) {
		m_saidBusy = false;
		return;
	}
	if (!m_saidBusy) {
		m_saidBusy = true;
		Statusf("%s is starting a mission already - one at a time", NickOf(pkt.ownerId));
	}
}

void MissionSync::OnWaiting(const S_MissionWaiting &pkt, uint8_t localPlayerId, uint32_t nowMs) {
	const bool    start   = pkt.what == MISSION_WAIT_START;
	const uint8_t busy    = start ? static_cast<uint8_t>(pkt.busyMask & pkt.missingMask) : 0;
	// A start goes on without the ones in their own intro; a checkpoint without
	// whoever it still waits for.
	const bool    goesOn  = pkt.what != MISSION_WAIT_NONE && pkt.goesOnInS != 0;
	const bool    changed = pkt.ownerId != m_waitOwner || pkt.missingMask != m_waitMissing ||
	                     pkt.what != m_waitWhat || busy != m_waitBusy || goesOn != m_waitGoesOn;
	m_waitOwner   = pkt.ownerId;
	m_waitMissing = pkt.missingMask;
	m_waitWhat    = pkt.what;
	m_waitHint    = pkt.missionHint;
	m_waitBusy    = busy;
	m_waitGoesOn  = goesOn;
	if (goesOn)
		m_waitGoesOnMs = nowMs + static_cast<uint32_t>(pkt.goesOnInS) * 1000u;
	if (changed) {
		m_waitSaidMs = nowMs;
		AnnounceWait(localPlayerId, nowMs);
	}
}

void MissionSync::OnState(const S_MissionState &pkt, uint8_t localPlayerId, uint32_t nowMs) {
	const bool    first = !m_serverShares;
	const uint8_t was   = m_state;
	m_serverShares = true;
	// A server that has started over keeps another log, numbered from 1
	// again. What this game got from the old one stays in it.
	if (pkt.campaignLog != m_campaignLog) {
		if (m_campaignLog != 0 && !m_log.empty())
			Log("missions: the server has started over, and so has its campaign log");
		m_campaignLog = pkt.campaignLog;
		m_log.clear();
		m_early.clear();
		m_settled = 0;
	}
	// Whatever missions were passed while we were away, or before we came.
	if (first) {
		C_CampaignSince since;
		InitHeader(since, nowMs);
		since.seq = static_cast<uint32_t>(m_log.size());
		Out(since, CH_EVENT);
		// And that our game is in a mission of its own, if it is.
		if (m_busy != m_busySent)
			SendBusy(false, nowMs);
		// Back from a dropped connection with our mission still running here.
		// The server failed it for everybody when we went, but the script is
		// alive and so is everything it made: offered again, it becomes the
		// session's as any start the server did not grant does, unless
		// somebody else's runs by now.
		if (m_readopt) {
			m_readopt = false;
			char at[48];
			SessionAt(at, sizeof at);
			if (pkt.state == MISSION_STATE_IDLE) {
				C_MissionStarted again{};
				InitHeader(again, nowMs);
				again.launchKey     = 0;
				again.missionNumber = m_launchedNumber;
				Out(again, CH_EVENT);
				m_readoptSent = true;
				Log("missions: back in the session with %s still running here; offered it to "
				    "the session again%s",
				    MissionName(m_launchedNumber), at);
			} else {
				m_launchPending = false;
				Log("missions: back in the session, where %s's %s runs now; %s stays this "
				    "game's own%s",
				    NickOf(pkt.ownerId), MissionName(pkt.missionNumber),
				    MissionName(m_launchedNumber), at);
			}
		}
	}
	const uint8_t before = m_participants;
	// The server's answer to our launch: the mission is ours, or somebody
	// else's runs and ours stays our own.
	if (pkt.state == MISSION_STATE_RUNNING &&
	    (pkt.ownerId != localPlayerId || pkt.missionNumber == m_launchedNumber))
		m_launchPending = false;
	m_state        = pkt.state;
	m_owner        = pkt.ownerId;
	m_number       = pkt.missionNumber;
	m_participants = pkt.participants;
	m_flags        = pkt.flags;
	m_marginCm     = pkt.marginCm;
	m_enemies      = pkt.enemies;
	m_scalePct     = pkt.scalePct <= MISSION_SCALE_MAX ? pkt.scalePct : MISSION_SCALE_MAX;
	m_cpWaitS      = pkt.checkpointWaitS <= MISSION_CHECKPOINT_WAIT_S_MAX ? pkt.checkpointWaitS
	                                                                 : MISSION_CHECKPOINT_WAIT_S_MAX;
	m_catchUpM     = pkt.catchUpM <= MISSION_DISTANCE_M_MAX ? pkt.catchUpM : MISSION_DISTANCE_M_MAX;
	m_behindM      = pkt.behindM <= MISSION_DISTANCE_M_MAX ? pkt.behindM : MISSION_DISTANCE_M_MAX;
	m_behindS      = pkt.behindS <= MISSION_BEHIND_S_MAX ? pkt.behindS : MISSION_BEHIND_S_MAX;

	const bool ours = pkt.ownerId == localPlayerId;
	if (pkt.state != was) {
		m_cpSaidNoWait = false;
		m_behind       = false;
		m_timerUp      = false;
		for (WidgetSent &w : m_widgets)
			w = WidgetSent{};
		ForgetSeats();
		m_garageSent     = 0;
		m_garageResprays = 0;
		m_shotDown       = 0;
		for (uint32_t &g : m_garageHasCar)
			g = 0;
	}
	if (pkt.state == MISSION_STATE_RUNNING && was != MISSION_STATE_RUNNING) {
		m_claimLive   = false;
		m_haveVerdict = false;
		m_saidBusy    = false;
		if (ours && m_readoptSent) {
			// Taken up again: everybody in it has missed what it put up since
			// it failed on their screens, and is handed it as a joiner is.
			m_readoptSent = false;
			for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
				if (id != localPlayerId && (pkt.participants & PlayerBit(id)) != 0)
					HandOver(id, nowMs);
			Statusf("the session has taken up %s again, with you as its owner",
			        MissionName(pkt.missionNumber));
		} else if (ours) {
			// Who is not in it, named: a game still in a mission of its own
			// when the start went on without it (MISSION_BUSY_WAIT_MS). The
			// log is what says whether the start waited for everybody.
			uint8_t         out = 0;
			MissionPresence others[MAX_PLAYERS];
			const size_t    n = m_presence ? m_presence(m_rosterCtx, others, MAX_PLAYERS) : 0;
			for (size_t i = 0; i < n; ++i)
				if (others[i].playerId < MAX_PLAYERS && others[i].playerId != localPlayerId &&
				    (pkt.participants & PlayerBit(others[i].playerId)) == 0)
					out = static_cast<uint8_t>(out | PlayerBit(others[i].playerId));
			if (out == 0) {
				Statusf("you started %s, and everybody is in it", MissionName(pkt.missionNumber));
			} else {
				char who[160];
				Names(who, sizeof who, out, localPlayerId);
				Statusf("you started %s without %s, whose game is still in a mission of its own; "
				        "they come into it once that is over",
				        MissionName(pkt.missionNumber), who);
			}
		} else if ((pkt.participants & PlayerBit(localPlayerId)) == 0)
			Statusf("%s started %s without you: this game is in a mission of its own, and comes "
			        "into it once that is over",
			        NickOf(pkt.ownerId), MissionName(pkt.missionNumber));
		else if (first)
			Statusf("%s is on %s, and you are in it", NickOf(pkt.ownerId),
			        MissionName(pkt.missionNumber));
		else
			Statusf("%s started %s", NickOf(pkt.ownerId), MissionName(pkt.missionNumber));
	} else if (pkt.state == MISSION_STATE_IDLE && was == MISSION_STATE_RUNNING) {
		m_lastOutcome = pkt.outcome;
		if (!ours)
			EndEffects();
		switch (pkt.outcome) {
		case MISSION_OUTCOME_PASSED:
			if (ours)
				Statusf("you passed %s", MissionName(pkt.missionNumber));
			else
				Statusf("%s passed %s", NickOf(pkt.ownerId), MissionName(pkt.missionNumber));
			break;
		case MISSION_OUTCOME_OWNER_LEFT:
			Statusf("%s left, so %s failed", NickOf(pkt.ownerId), MissionName(pkt.missionNumber));
			break;
		default:
			if (IsOddJob(pkt.missionNumber) && ours)
				Statusf("your %s shift is over", MissionName(pkt.missionNumber));
			else if (IsOddJob(pkt.missionNumber))
				Statusf("%s's %s shift is over", NickOf(pkt.ownerId), MissionName(pkt.missionNumber));
			else
				Statusf("%s failed", MissionName(pkt.missionNumber));
			break;
		}
	}

	// Somebody who has just come into our running mission missed what it has
	// up, and is handed it alone (missions.md 11.5), with the widgets' values
	// straight after.
	if (ours && pkt.state == MISSION_STATE_RUNNING && was == MISSION_STATE_RUNNING) {
		const uint8_t fresh =
		    static_cast<uint8_t>(pkt.participants & ~before & ~PlayerBit(localPlayerId));
		for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
			if ((fresh & PlayerBit(id)) != 0)
				HandOver(id, nowMs);
	}
	// Our game came out of a mission of its own into somebody's running one.
	if (!ours && !first && pkt.state == MISSION_STATE_RUNNING && was == MISSION_STATE_RUNNING &&
	    (pkt.participants & PlayerBit(localPlayerId)) != 0 && (before & PlayerBit(localPlayerId)) == 0)
		Statusf("%s is on %s, and you are in it now", NickOf(pkt.ownerId),
		        MissionName(pkt.missionNumber));

	const bool wasMirroring = m_mirroring;
	SetMirror(pkt.state == MISSION_STATE_RUNNING && !ours &&
	          (pkt.participants & PlayerBit(localPlayerId)) != 0);
	// In somebody else's running mission from now: what it has made so far is
	// asked for again, since some of it may have been made while this game was
	// in a mission of its own, or away.
	if (m_mirroring && !wasMirroring) {
		AskCatchUp(nowMs);
		// Come into it while it ran, rather than at a start everybody was at:
		// joined, back from a dropped connection, or out of our own intro.
		// Wherever that left our player, they are brought to the owner.
		if (first || was == MISSION_STATE_RUNNING) {
			m_summon        = true;
			m_summonSinceMs = nowMs;
			m_summonBehind  = false;
		}
	}
	if (!m_mirroring)
		m_summon = false;
	m_ownsRunning = pkt.state == MISSION_STATE_RUNNING && ours;
	if (m_ownsRunning)
		m_launchedNumber = pkt.missionNumber;
	if (pkt.state == MISSION_STATE_RUNNING && !ours)
		m_readoptSent = false;
	if (!(pkt.state == MISSION_STATE_RUNNING && ours)) {
		m_cpReported = 0;
		m_cpWaiting  = false;
		m_cpGaveUp   = false;
	}
}

void MissionSync::OnSeats(const S_MissionSeats &pkt, uint8_t localPlayerId) {
	if (!m_serverShares || m_state != MISSION_STATE_RUNNING || pkt.ownerId != m_owner ||
	    pkt.ownerId == localPlayerId || pkt.count > MISSION_SEAT_CARS)
		return;
	m_seatsKeptCount = pkt.count;
	for (uint8_t i = 0; i < pkt.count; ++i)
		m_seatsKept[i] = pkt.cars[i];
}

void MissionSync::OnBoard(const S_MissionBoard &pkt, uint8_t localPlayerId, uint32_t nowMs) {
	if (!m_serverShares || m_busy || m_state != MISSION_STATE_RUNNING || pkt.ownerId != m_owner ||
	    pkt.ownerId == localPlayerId || pkt.missionNumber != m_number || localPlayerId >= MAX_PLAYERS ||
	    (m_participants & PlayerBit(localPlayerId)) == 0 || pkt.netId == INVALID_NETID)
		return;
	const uint8_t seat = pkt.seats[localPlayerId];
	if (seat == 0) {
		m_board.live = false;
		if ((pkt.flags & MISSION_BOARD_WATER) != 0)
			Statusf("%s put %s in a boat with no seat left for you; its checkpoints on the water "
			        "don't wait for you",
			        MissionName(m_number), NickOf(m_owner));
		else
			Statusf("%s put %s in a car with no seat left for you; follow it in one of your own",
			        MissionName(m_number), NickOf(m_owner));
		return;
	}
	uint16_t others = 0;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
		if (id != localPlayerId && pkt.seats[id] != 0 && pkt.seats[id] < 16)
			others = static_cast<uint16_t>(others | (1u << pkt.seats[id]));
	m_board = BoardSeat{true, pkt.netId, seat, others, nowMs};
}

void MissionSync::OnHandOver(const S_MissionHandOver &pkt, uint8_t localPlayerId, uint32_t nowMs) {
	if (!m_serverShares || m_state != MISSION_STATE_RUNNING || m_owner != localPlayerId ||
	    pkt.missionNumber != m_number || pkt.playerId >= MAX_PLAYERS || pkt.playerId == localPlayerId)
		return;
	HandOver(pkt.playerId, nowMs);
}

void MissionSync::OnPickupTaken(const S_MissionPickup &pkt, uint8_t localPlayerId) {
	if (!m_serverShares || m_busy || m_state != MISSION_STATE_RUNNING || pkt.playerId == localPlayerId ||
	    pkt.missionNumber != m_number)
		return;
	if (m_bridge && m_bridge->TakePickup)
		m_bridge->TakePickup(pkt.handle);
}

void MissionSync::OnKill(const S_MissionKill &pkt, uint8_t localPlayerId) {
	if (!m_serverShares || m_state != MISSION_STATE_RUNNING || m_owner != localPlayerId ||
	    pkt.playerId == localPlayerId || pkt.missionNumber != m_number)
		return;
	if (m_bridge && m_bridge->CountKill)
		m_bridge->CountKill(pkt.model);
	if (m_killsCounted++ == 0)
		Log("missions: %s's kills count for %s here too", NickOf(pkt.playerId),
		    MissionName(m_number));
}

void MissionSync::OnAnswers(const S_MissionAnswers &pkt, uint8_t localPlayerId) {
	if (!m_serverShares || m_state != MISSION_STATE_RUNNING || m_owner != localPlayerId ||
	    pkt.playerId == localPlayerId || pkt.playerId >= MAX_PLAYERS || pkt.missionNumber != m_number)
		return;
	m_garageHasCar[pkt.playerId] = pkt.hasCar;
	m_garageResprays |= pkt.resprayed;
	m_shotDown = static_cast<uint16_t>(m_shotDown | pkt.shotDown);
}

void MissionSync::OnObjectBroken(const S_MissionObjectBreak &pkt, uint8_t localPlayerId) {
	if (!m_serverShares || m_busy || m_state != MISSION_STATE_RUNNING || pkt.playerId == localPlayerId ||
	    pkt.missionNumber != m_number || pkt.global == 0)
		return;
	if (!(pkt.amount == pkt.amount) || pkt.amount < 0.0f)   // NaN, or nothing to run
		return;
	if (m_bridge && m_bridge->BreakObject) {
		m_bridge->BreakObject(pkt.global, pkt.amount, pkt.state);
		++m_objectBreaks;
	}
}

void MissionSync::OnReady(const S_MissionReady &pkt, uint8_t localPlayerId) {
	if (!m_serverShares || m_state != MISSION_STATE_RUNNING || m_owner != localPlayerId ||
	    pkt.missionNumber != m_number || pkt.playerId >= MAX_PLAYERS)
		return;
	if (pkt.readySeq > m_ready[pkt.playerId])
		m_ready[pkt.playerId] = pkt.readySeq;
}

void MissionSync::OnEffect(const S_MissionEffect &pkt, uint8_t localPlayerId, bool sharedWallet,
                           uint32_t nowMs) {
	// The server relays only the owner's, and never back to them; one the
	// owner handed somebody alone is theirs. Nothing of it runs in a game
	// that is in a mission of its own: that game is not in this one.
	if (!m_serverShares || m_busy || pkt.ownerId == localPlayerId ||
	    pkt.body.length > MISSION_EFFECT_CODE)
		return;
	if (pkt.body.onlyTo != 0 && pkt.body.onlyTo != localPlayerId + 1)
		return;
	if (pkt.body.kind == MISSION_EFFECT_PAY && (sharedWallet || IsCharge(pkt.body)))
		return;
	// Behind one that waits for a copy of ours, in the order they came: a
	// blip's colour must not arrive before the blip.
	if (!m_awaiting.empty() || !TryEffect(pkt, localPlayerId, nowMs, false)) {
		if (m_awaiting.size() >= MISSION_EFFECT_AWAIT_MAX) {
			TryEffect(m_awaiting.front().pkt, localPlayerId, nowMs, true);
			m_awaiting.erase(m_awaiting.begin());
		}
		m_awaiting.push_back(AwaitingEffect{pkt, nowMs});
		if (m_effectsAwaited++ == 0)
			Log("missions: an instruction of %s's mission names a pedestrian or car our "
			    "copy of is still being built; it waits for it, and what came after it "
			    "waits behind it",
			    NickOf(pkt.ownerId));
	}
}

bool MissionSync::TryEffect(const S_MissionEffect &pkt, uint8_t localPlayerId, uint32_t nowMs,
                            bool lastChance) {
	bool ran = false;
	if (pkt.body.kind == MISSION_EFFECT_TELEPORT) {
		uint8_t rank = 0, count = 0;
		TeleportRank(pkt.ownerId, localPlayerId, &rank, &count);
		ran = m_bridge && m_bridge->Teleport && m_bridge->Teleport(pkt.body, rank, count);
	} else {
		ran = m_bridge && m_bridge->RunEffect && m_bridge->RunEffect(pkt.body);
	}
	if (!ran && !lastChance && m_bridge && m_bridge->EffectAwaits &&
	    m_bridge->EffectAwaits(pkt.body))
		return false;
	if (ran && m_effectsRun++ == 0)
		Log("missions: showing what %s's mission shows", NickOf(pkt.ownerId));
	// The owner's mission waits until everybody has loaded what it asked for,
	// and the engine's own replay of LOAD_ALL_MODELS_NOW has just loaded ours,
	// if it could run at all (protocol.h, C_MissionReady).
	if (pkt.body.readySeq != 0 && m_state == MISSION_STATE_RUNNING && pkt.ownerId == m_owner) {
		C_MissionReady ready{};
		InitHeader(ready, nowMs);
		ready.missionNumber = pkt.body.missionNumber;
		ready.readySeq      = pkt.body.readySeq;
		Out(ready, CH_EVENT);
	}
	return true;
}

// The ones that waited, as soon as what they name is here, or once they have
// waited long enough, and always in order.
void MissionSync::RunAwaiting(uint8_t localPlayerId, uint32_t nowMs) {
	while (!m_awaiting.empty()) {
		const AwaitingEffect &w    = m_awaiting.front();
		const bool            last = static_cast<uint32_t>(nowMs - w.sinceMs) >= MISSION_EFFECT_AWAIT_MS;
		if (!TryEffect(w.pkt, localPlayerId, nowMs, last))
			return;
		m_awaiting.erase(m_awaiting.begin());
	}
}

float MissionSync::EnemyToughness() const {
	if (m_state != MISSION_STATE_RUNNING || m_enemies == MISSION_ENEMIES_ORIGINAL)
		return 1.0f;
	uint8_t players = 0;
	for (uint8_t bits = m_participants; bits != 0; bits = static_cast<uint8_t>(bits & (bits - 1)))
		++players;
	return players <= 1 ? 1.0f : 1.0f + (m_scalePct / 100.0f) * static_cast<float>(players - 1);
}

uint8_t MissionSync::EnemyCopies() const {
	if (m_state != MISSION_STATE_RUNNING || m_enemies != MISSION_ENEMIES_MORE)
		return 0;
	uint8_t players = 0;
	for (uint8_t bits = m_participants; bits != 0; bits = static_cast<uint8_t>(bits & (bits - 1)))
		++players;
	const uint8_t copies = players <= 1 ? 0 : static_cast<uint8_t>(players - 1);
	return copies < MISSION_ENEMY_COPIES_MAX ? copies : MISSION_ENEMY_COPIES_MAX;
}

void MissionSync::TeleportRank(uint8_t ownerId, uint8_t localPlayerId, uint8_t *rank,
                               uint8_t *count) const {
	MissionPresence others[MAX_PLAYERS];
	const size_t    n = m_presence ? m_presence(m_rosterCtx, others, MAX_PLAYERS) : 0;
	uint8_t         r = 0, c = 1;   // us
	// Only the others the owner's mission moves too: a player who is not in
	// it, their game in its own intro say, stays where they are, and a place
	// kept for them would leave a gap in the ring.
	for (size_t i = 0; i < n; ++i) {
		if (others[i].playerId == ownerId || others[i].playerId == localPlayerId ||
		    (m_participants & PlayerBit(others[i].playerId)) == 0)
			continue;
		++c;
		if (others[i].playerId < localPlayerId)
			++r;
	}
	*rank  = r;
	*count = c;
}

void MissionSync::OnWidget(const S_MissionWidget &pkt, uint8_t localPlayerId) {
	if (!m_serverShares || m_busy || m_state != MISSION_STATE_RUNNING ||
	    pkt.ownerId == localPlayerId || pkt.ownerId != m_owner)
		return;
	if (m_bridge && m_bridge->SetWidget)
		m_bridge->SetWidget(pkt.offset, pkt.value);
}

void MissionSync::OnCampaignDelta(const S_CampaignDelta &pkt) {
	const CampaignDeltaBody &b = pkt.body;
	if (b.seq == 0 || b.valueCount > CAMPAIGN_VALUES || b.threadCount > CAMPAIGN_THREADS ||
	    b.opLength > MISSION_EFFECT_CODE)
		return;
	if (b.seq <= m_log.size())
		return;   // had it
	CampaignEntry e;
	e.ownerId = pkt.ownerId;
	e.body    = b;
	if (b.seq != m_log.size() + 1) {
		// Ahead of one still on its way: a mission that ended while the
		// server was answering C_CampaignSince, whose answer fills the gap.
		for (const CampaignEntry &x : m_early)
			if (x.body.seq == b.seq)
				return;
		m_early.push_back(e);
		return;
	}
	m_log.push_back(e);
	for (bool more = true; more;) {
		more = false;
		for (size_t i = 0; i < m_early.size(); ++i) {
			if (m_early[i].body.seq > m_log.size() + 1)
				continue;
			if (m_early[i].body.seq == m_log.size() + 1)
				m_log.push_back(m_early[i]);
			m_early.erase(m_early.begin() + static_cast<std::ptrdiff_t>(i));
			more = true;
			break;
		}
	}
}

bool MissionSync::AlreadyHere(const CampaignDeltaBody &body, uint32_t hash) const {
	if (body.scriptHash != hash || body.valueCount == 0)
		return false;   // says nothing either way
	for (uint8_t i = 0; i < body.valueCount; ++i) {
		int32_t value = 0;
		if (!m_bridge->ReadGlobal(body.values[i].offset, &value) || value != body.values[i].value)
			return false;
	}
	return true;
}

// The log is the campaign in the order it happened, so a game that has one
// delta has every one before it: a save made after it, or this machine's own
// mission. Looking from the newest back, the first whose every value this
// game already holds is where it is up to. Each value a mission sends is one
// it changed, so a game from before it does not hold them all.
void MissionSync::SettleCampaign() {
	if (!m_bridge || !m_bridge->ApplyCampaign || !m_bridge->ScriptLife || !m_bridge->ScriptHash ||
	    !m_bridge->ReadGlobal)
		return;
	const uint32_t life = m_bridge->ScriptLife();
	if (life == 0)
		return;
	const bool newLife = life != m_life;
	if (newLife) {
		m_life    = life;
		m_settled = 0;
	}
	if (m_settled >= m_log.size())
		return;
	const uint32_t hash = m_bridge->ScriptHash();
	if (hash == 0)
		return;

	size_t from = m_settled;
	for (size_t k = m_log.size(); k-- > m_settled;)
		if (AlreadyHere(m_log[k].body, hash)) {
			from = k + 1;
			break;
		}
	uint32_t missions = 0, foreign = 0, failed = 0;
	for (size_t i = from; i < m_log.size(); ++i) {
		const CampaignEntry &e = m_log[i];
		if (e.body.scriptHash != hash) {
			++foreign;
			continue;
		}
		if (!m_bridge->ApplyCampaign(e.body)) {
			++failed;
			continue;
		}
		++m_campaignApplied;
		if (e.body.last) {
			++missions;
			Log("missions: %s's %s is in this game's campaign now (delta %u)", NickOf(e.ownerId),
			    MissionName(e.body.missionNumber), e.body.seq);
		}
	}
	if (foreign != 0 && !m_saidOtherScript) {
		m_saidOtherScript = true;
		Log("missions: %u campaign delta%s came from a main.scm that is not this one, and "
		    "nothing of %s is applied here",
		    foreign, foreign == 1 ? "" : "s", foreign == 1 ? "it" : "them");
		Notice("Somebody's main.scm is not yours: their missions do not count in your game");
	}
	if (failed != 0)
		Log("missions: %u campaign delta%s could not be applied here", failed,
		    failed == 1 ? "" : "s");
	if (missions > 1 || (newLife && missions > 0))
		Statusf("Your game caught up with the session: %u mission%s", missions,
		        missions == 1 ? "" : "s");
	m_settled = m_log.size();
}

void MissionSync::OnCarLists(const S_CarLists &pkt) {
	MergeCarLists(m_carLists, pkt.collected);
	// Put in at the next look, not a frame later than that.
	m_carListsReadMs = 0;
	m_carListsAsked  = true;
}

// How often this game's lists are read, and how soon a car the session has
// not heard of is said again while no answer has come.
constexpr uint32_t CAR_LISTS_READ_MS   = 500;
constexpr uint32_t CAR_LISTS_RESEND_MS = 2000;

static unsigned Bits(uint32_t v) {
	unsigned n = 0;
	for (; v != 0; v &= v - 1)
		++n;
	return n;
}

// The Import/Export garages' and the crane's lists are the session's
// campaign (missions.md 6.1): a car anybody brought is ticked in every game.
// Only the engine's own words are written, never its reward, so each game's
// own import.sc ticks the board and pays the crane's bonus when its player
// comes by, as it would for a car of its own; the car's own reward went to
// whoever brought it. Nothing is ever taken off, so a load or a new game
// that lacks them gets them back.
void MissionSync::TickCarLists(uint32_t nowMs) {
	if (!m_serverShares || !m_bridge || !m_bridge->ReadCarLists || !m_bridge->WriteCarLists)
		return;
	if (m_carListsReadMs != 0 && static_cast<uint32_t>(nowMs - m_carListsReadMs) < CAR_LISTS_READ_MS)
		return;
	m_carListsReadMs = nowMs == 0 ? 1 : nowMs;
	uint32_t ours[CAR_LISTS] = {};
	if (!m_bridge->ReadCarLists(ours))
		return;
	uint32_t with[CAR_LISTS];
	unsigned taken = 0;
	for (uint8_t i = 0; i < CAR_LISTS; ++i) {
		with[i] = ours[i] | m_carLists[i];
		taken += Bits(with[i] & ~ours[i]);
	}
	if (taken != 0) {
		m_bridge->WriteCarLists(with);
		m_carListsTaken += taken;
		Log("missions: %u car%s the session's Import/Export and crane lists have %s ticked in "
		    "this game now",
		    taken, taken == 1 ? "" : "s", taken == 1 ? "is" : "are");
	}
	bool news = false;
	for (uint8_t i = 0; i < CAR_LISTS; ++i)
		news = news || (ours[i] & ~m_carLists[i]) != 0;
	if ((!m_carListsAsked || news) &&
	    (m_carListsSentMs == 0 || static_cast<uint32_t>(nowMs - m_carListsSentMs) >= CAR_LISTS_RESEND_MS)) {
		C_CarLists out{};
		InitHeader(out, nowMs);
		for (uint8_t i = 0; i < CAR_LISTS; ++i)
			out.collected[i] = with[i];
		Out(out, CH_EVENT);
		m_carListsSentMs = nowMs == 0 ? 1 : nowMs;
		m_carListsAsked  = true;
		if (news)
			Log("missions: this game has a car on the Import/Export or crane lists the session "
			    "has not heard of; told it");
	}
}

void MissionSync::EndEffects() {
	// Whatever still waited for a copy of ours belonged to what just ended.
	m_awaiting.clear();
	if (m_bridge && m_bridge->EndEffects)
		m_bridge->EndEffects();
}

void MissionSync::OnFail(const S_MissionFail &pkt, uint8_t localPlayerId) {
	if (m_state != MISSION_STATE_RUNNING || m_owner != localPlayerId)
		return;
	Statusf("%s %s, so %s fails", NickOf(pkt.playerId),
	        pkt.reason == MISSION_FAIL_BUSTED ? "was busted" : "died", MissionName(m_number));
	if (m_bridge && m_bridge->FailMission)
		m_bridge->FailMission(pkt.reason);
}

// ---- per frame -----------------------------------------------------------------

void MissionSync::Tick(uint8_t localPlayerId, uint32_t nowMs) {
	if (m_waitWhat != MISSION_WAIT_NONE &&
	    static_cast<uint32_t>(nowMs - m_waitSaidMs) >= MISSION_WAIT_REMIND_MS) {
		m_waitSaidMs = nowMs;
		AnnounceWait(localPlayerId, nowMs);
	}
	// The owner walked out of the checkpoint they were waiting at, so there
	// is nothing to wait at any more.
	if (m_cpReported != 0 &&
	    static_cast<uint32_t>(nowMs - m_cpAskedMs) >= MISSION_CHECKPOINT_IDLE_MS) {
		C_MissionCheckpoint out{};
		InitHeader(out, nowMs);
		out.missingMask = 0;
		out.where       = m_cpWhere;
		Out(out, CH_EVENT);
		m_cpReported = 0;
	}
	if ((m_cpWaiting || m_cpGaveUp) &&
	    static_cast<uint32_t>(nowMs - m_cpAskedMs) >= MISSION_CHECKPOINT_IDLE_MS) {
		m_cpWaiting = false;
		m_cpGaveUp  = false;
	}
	// A claim we stopped refreshing has lapsed on the server too, and its
	// grant with it: a later gate has to be granted afresh.
	if (m_claimLive && static_cast<uint32_t>(nowMs - m_claimSentMs) >= MISSION_CLAIM_TTL_MS) {
		m_claimLive   = false;
		m_haveVerdict = false;
		m_saidBusy    = false;
	}
	// A game in a mission of its own runs nothing of the session's, and what
	// waited was for a world it has since left.
	if (m_busy)
		m_awaiting.clear();
	RunAwaiting(localPlayerId, nowMs);
	SettleCampaign();
	TickCarLists(nowMs);
}

void MissionSync::Clear() {
	SetMirror(false);
	// A new connection is a new Player on the server, which has not heard it.
	m_busySent = false;
	// Not only a running mission's: a trigger's fade before its launch is on
	// our screen too.
	m_lastOutcome = MISSION_OUTCOME_NONE;
	EndEffects();
	m_serverShares = false;
	m_state        = MISSION_STATE_IDLE;
	m_owner        = INVALID_PLAYER;
	m_number       = MISSION_NONE;
	m_participants = 0;
	m_flags        = 0;
	m_marginCm     = MISSION_MARGIN_CM_DEFAULT;
	m_cpWaitS      = MISSION_CHECKPOINT_WAIT_MS / 1000;
	m_catchUpM     = MISSION_CATCH_UP_M_DEFAULT;
	m_behindM      = MISSION_BEHIND_M_DEFAULT;
	m_behindS      = MISSION_BEHIND_S_DEFAULT;
	m_claimLive    = false;
	m_haveVerdict  = false;
	m_saidBusy     = false;
	m_waitOwner    = INVALID_PLAYER;
	m_waitMissing  = 0;
	m_waitWhat     = MISSION_WAIT_NONE;
	m_waitHint     = MISSION_NONE;
	m_waitBusy     = 0;
	m_waitGoesOn   = false;
	m_cpReported   = 0;
	m_early.clear();
	for (WidgetSent &w : m_widgets)
		w = WidgetSent{};
	for (uint16_t &r : m_ready)
		r = 0;
	m_readyAsked    = 0;
	m_readyGaveUp   = false;
	// Our own mission, running or launched and not answered yet, is still
	// the session's as far as this game goes, and is offered again on the
	// way back (OnState).
	const bool ours = m_ownsRunning || m_launchPending;
	if (ours && !m_readopt)
		Log("missions: the connection went with %s running here; it goes on, and is offered "
		    "to the session again on the way back",
		    MissionName(m_launchedNumber));
	m_launchPending = ours;
	m_readopt       = ours;
	m_readoptSent   = false;
	m_ownsRunning   = false;
	m_cpWaiting     = false;
	m_cpGaveUp      = false;
	m_summon        = false;
	m_summonBehind  = false;
	m_behind        = false;
	m_timerUp       = false;
	m_cpSaidNoWait  = false;
	// Another server's lists may be other ones: asked for again, and ours
	// told, on the next connection.
	for (uint32_t &l : m_carLists)
		l = 0;
	m_carListsAsked  = false;
	m_carListsSentMs = 0;
	m_carListsReadMs = 0;
	ForgetSeats();
}

// ---- the words -------------------------------------------------------------------

void MissionSync::AnnounceWait(uint8_t localPlayerId, uint32_t nowMs) {
	if (m_waitWhat == MISSION_WAIT_NONE || m_waitMissing == 0)
		return;
	char who[160];
	Names(who, sizeof who, m_waitMissing, localPlayerId);
	const bool ownerIsUs = m_waitOwner == localPlayerId;
	const bool weAreLate = (m_waitMissing & PlayerBit(localPlayerId)) != 0;
	if (m_waitWhat == MISSION_WAIT_START && m_waitBusy != 0) {
		// Somebody's game is in a cutscene of its own, a new game's intro:
		// they cannot come until it is over, and the start does not wait
		// for that for ever.
		const uint8_t away = static_cast<uint8_t>(m_waitMissing & ~m_waitBusy);
		char          busy[160], rest[160], tail[64] = "";
		Names(busy, sizeof busy, m_waitBusy, localPlayerId);
		Names(rest, sizeof rest, away, localPlayerId);
		const int32_t left = static_cast<int32_t>(m_waitGoesOnMs - nowMs);
		if (m_waitGoesOn && left > 0)
			std::snprintf(tail, sizeof tail, " - it starts without %s in %u s",
			              (m_waitBusy & PlayerBit(localPlayerId)) != 0 ? "you" : "them",
			              static_cast<unsigned>((left + 999) / 1000));
		if (ownerIsUs && away == 0)
			Statusf("waiting for %s, still in a cutscene of their own%s", busy, tail);
		else if (ownerIsUs)
			Statusf("waiting for %s to come to the start, and for %s, still in a cutscene of "
			        "their own", rest, busy);
		else if ((m_waitBusy & PlayerBit(localPlayerId)) != 0)
			Statusf("%s is waiting at the start of %s for your cutscene to end%s",
			        NickOf(m_waitOwner), MissionName(m_waitHint), tail);
		else if (weAreLate)
			Statusf("%s is waiting for you at the start of %s", NickOf(m_waitOwner),
			        MissionName(m_waitHint));
		else
			Statusf("%s is waiting for %s at the start of %s", NickOf(m_waitOwner), who,
			        MissionName(m_waitHint));
	} else if (m_waitWhat == MISSION_WAIT_START) {
		if (ownerIsUs)
			Statusf("waiting for %s to come to the start", who);
		else if (weAreLate)
			Statusf("%s is waiting for you at the start of %s", NickOf(m_waitOwner),
			        MissionName(m_waitHint));
		else
			Statusf("%s is waiting for %s at the start of %s", NickOf(m_waitOwner), who,
			        MissionName(m_waitHint));
	} else {
		char          tail[64] = "";
		const int32_t left     = static_cast<int32_t>(m_waitGoesOnMs - nowMs);
		if (m_waitGoesOn && left > 0)
			std::snprintf(tail, sizeof tail, " - it goes on without %s in %u s",
			              weAreLate && !ownerIsUs ? "you" : "them",
			              static_cast<unsigned>((left + 999) / 1000));
		if (ownerIsUs)
			Statusf("waiting for %s to catch up%s", who, tail);
		else if (weAreLate)
			Statusf("%s is waiting for you to catch up%s", NickOf(m_waitOwner), tail);
		else
			Statusf("waiting for %s to catch up%s", who, tail);
	}
}

bool MissionSync::WaitLine(char *out, size_t cap, uint8_t localPlayerId, uint32_t nowMs) const {
	if (cap == 0)
		return false;
	out[0] = '\0';
	if (!m_serverShares || m_waitWhat == MISSION_WAIT_NONE || m_waitMissing == 0 ||
	    m_waitOwner >= MAX_PLAYERS)
		return false;
	// A checkpoint is only for those in the running mission.
	if (m_waitWhat == MISSION_WAIT_CHECKPOINT &&
	    !(m_state == MISSION_STATE_RUNNING &&
	      (m_owner == localPlayerId || (m_participants & PlayerBit(localPlayerId)) != 0)))
		return false;
	char who[96];
	Names(who, sizeof who, m_waitMissing, localPlayerId);
	char          tail[24] = "";
	const int32_t left     = static_cast<int32_t>(m_waitGoesOnMs - nowMs);
	if (m_waitGoesOn && left > 0)
		std::snprintf(tail, sizeof tail, " - %u s", static_cast<unsigned>((left + 999) / 1000));
	const char *where = m_waitWhat == MISSION_WAIT_START ? " at the start" : "";
	int         n     = 0;
	if (m_waitOwner == localPlayerId)
		n = std::snprintf(out, cap, "Waiting for %s%s%s", who, where, tail);
	else
		n = std::snprintf(out, cap, "%s is waiting for %s%s%s", NickOf(m_waitOwner), who, where,
		                  tail);
	return n > 0;
}

// ---- bringing a late participant to the owner ------------------------------------

void MissionSync::Respawned(uint8_t localPlayerId, uint32_t nowMs) {
	if (!ParticipantHere(localPlayerId))
		return;
	m_summon        = true;
	m_summonSinceMs = nowMs;
	m_summonBehind  = false;
	Log("missions: back from the hospital or the police station in the middle of %s; "
	    "going to %s once we can",
	    MissionName(m_number), NickOf(m_owner));
}

void MissionSync::WatchBehind(const Vec3 &localPos, uint8_t localPlayerId, uint32_t nowMs) {
	if (!ParticipantHere(localPlayerId) || m_summon || CheckpointsWaitNow() || m_behindM == 0) {
		m_behind = false;
		return;
	}
	MissionPresence others[MAX_PLAYERS];
	const size_t    n     = m_presence ? m_presence(m_rosterCtx, others, MAX_PLAYERS) : 0;
	const MissionPresence *owner = nullptr;
	for (size_t i = 0; i < n; ++i)
		if (others[i].playerId == m_owner)
			owner = &others[i];
	if (!owner || !owner->havePos) {
		m_behind = false;
		return;
	}
	const float dx = owner->pos.x - localPos.x, dy = owner->pos.y - localPos.y,
	            dz = owner->pos.z - localPos.z;
	const float far = static_cast<float>(m_behindM);
	if (dx * dx + dy * dy + dz * dz <= far * far) {
		m_behind = false;
		return;
	}
	if (!m_behind) {
		m_behind        = true;
		m_behindSinceMs = nowMs;
		return;
	}
	if (static_cast<uint32_t>(nowMs - m_behindSinceMs) < static_cast<uint32_t>(m_behindS) * 1000u)
		return;
	// Owed now, with the summon's own delay already served.
	m_behind        = false;
	m_summon        = true;
	m_summonSinceMs = nowMs - MISSION_SUMMON_DELAY_MS;
	++m_behindMoves;
	m_summonBehind  = true;
	Statusf("more than %u m behind %s for %u s in %s; brought along",
	        static_cast<unsigned>(m_behindM), NickOf(m_owner),
	        static_cast<unsigned>(m_behindS), MissionName(m_number));
}

bool MissionSync::TakeSummon(const Vec3 &localPos, uint8_t localPlayerId, uint32_t nowMs,
                             Vec3 *ownerPos, uint8_t *slot, uint8_t *count) {
	if (!m_summon)
		return false;
	if (!ParticipantHere(localPlayerId)) {
		m_summon = false;
		return false;
	}
	if (static_cast<uint32_t>(nowMs - m_summonSinceMs) < MISSION_SUMMON_DELAY_MS)
		return false;
	MissionPresence others[MAX_PLAYERS];
	const size_t    n     = m_presence ? m_presence(m_rosterCtx, others, MAX_PLAYERS) : 0;
	const MissionPresence *owner = nullptr;
	for (size_t i = 0; i < n; ++i)
		if (others[i].playerId == m_owner)
			owner = &others[i];
	// Nothing says where the owner is yet: asked again next time.
	if (!owner || !owner->havePos)
		return false;
	const float dx = owner->pos.x - localPos.x, dy = owner->pos.y - localPos.y,
	            dz = owner->pos.z - localPos.z;
	m_summon          = false;
	const bool behind = m_summonBehind;
	m_summonBehind    = false;
	// A latecomer is brought only as far out as the server's missionCatchUp
	// says, and not at all with it 0; somebody who fell behind has been far
	// away for the whole of the server's time already.
	if (!behind && m_catchUpM == 0) {
		Log("missions: this server brings nobody who comes in late to %s", NickOf(m_owner));
		return false;
	}
	const float near = behind ? MISSION_SUMMON_NEAR_M : static_cast<float>(m_catchUpM);
	if (dx * dx + dy * dy + dz * dz <= near * near) {
		Log("missions: near enough to %s already; nobody is moved", NickOf(m_owner));
		return false;
	}
	TeleportRank(m_owner, localPlayerId, slot, count);
	*ownerPos = owner->pos;
	++m_summons;
	Statusf("brought beside %s, whose %s you are in", NickOf(m_owner), MissionName(m_number));
	return true;
}

void MissionSync::Names(char *out, size_t cap, uint8_t mask, uint8_t localPlayerId) const {
	if (cap == 0)
		return;
	out[0] = '\0';
	uint8_t ids[MAX_PLAYERS];
	size_t  n = 0;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id)
		if (mask & PlayerBit(id))
			ids[n++] = id;
	size_t len = 0;
	for (size_t i = 0; i < n; ++i) {
		const char *sep  = i == 0 ? "" : (i + 1 == n ? " and " : ", ");
		const char *name = ids[i] == localPlayerId ? "you" : NickOf(ids[i]);
		const int   w    = std::snprintf(out + len, cap - len, "%s%s", sep, name);
		if (w < 0 || static_cast<size_t>(w) >= cap - len)
			break;
		len += static_cast<size_t>(w);
	}
}

const char *MissionSync::NickOf(uint8_t playerId) const {
	const char *nick = m_nick ? m_nick(m_rosterCtx, playerId) : nullptr;
	return nick && nick[0] ? nick : "somebody";
}

void MissionSync::Notice(const char *line) {
	if (m_feed && line)
		m_feed(m_rosterCtx, line);
}

void MissionSync::AskCatchUp(uint32_t nowMs) {
	if (!m_serverShares)
		return;
	C_MissionCatchUp out{};
	InitHeader(out, nowMs);
	out.missionNumber = m_number;
	Out(out, CH_EVENT);
	++m_catchUps;
	char at[48];
	SessionAt(at, sizeof at);
	Log("missions: asked the session for every pedestrian and car %s's %s has made so far%s",
	    NickOf(m_owner), MissionName(m_number), at);
}

void MissionSync::SessionAt(char *out, size_t cap) const {
	if (cap == 0)
		return;
	out[0]      = '\0';
	uint32_t ms = 0;
	if (m_clock && m_clock(m_rosterCtx, &ms))
		std::snprintf(out, cap, " at session time %u ms", static_cast<unsigned>(ms));
}

void MissionSync::Statusf(const char *fmt, ...) {

	char    line[192];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(line, sizeof line, fmt, args);
	va_end(args);
	char at[48];
	SessionAt(at, sizeof at);
	if (at[0] != '\0') {
		const size_t len = std::strlen(line);
		std::snprintf(line + len, sizeof line - len, "%s", at);
	}
	if (m_status)
		m_status(m_rosterCtx, line);
}

void MissionSync::SetMirror(bool on) {
	if (on == m_mirroring)
		return;
	m_mirroring = on;
	if (m_bridge && m_bridge->SetOnMission)
		m_bridge->SetOnMission(on);
	Log("missions: this machine's $ONMISSION %s", on ? "follows the session's mission now"
	                                                 : "is its own again");
}

} // namespace coopiii
