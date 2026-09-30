// The session's one mission, client side. No engine code in here: the script
// engine's half is game/mission.cpp, and everything it does is reached through
// MissionBridge, so tools/clienttest drives all of this with a stub.
//
// protocol.h's "The session's one mission" is the wire and docs/missions.md
// the design. This machine is either the mission's owner, whose own main.scm
// runs it, or a participant in somebody else's:
//
//   - **At a start gate** the engine half asks AskStartGate, every time the
//     script does. The answer is false, and a claim goes out every
//     MISSION_CLAIM_REFRESH_MS, until the server grants the slot: everybody
//     else has to be at the start. Behind a server that has never said what
//     the session's mission is, nothing is claimed and every gate passes, as
//     before any of this existed.
//   - **The owner** reports START_MISSION (Launched) and the end (Ended), and
//     asks AskCheckpoint at every checkpoint, which is true only once every
//     participant is inside it or within the session's margin. While it is
//     false the others hear who is still missing.
//   - **A participant** has its $ONMISSION set for as long as the session's
//     mission runs (MissionBridge::SetOnMission), so its own triggers,
//     save points and rampages refuse themselves exactly as they would on a
//     mission in single player (missions.md 11.4).
//   - **The death rule** reaches the owner as S_MissionFail, and the engine
//     half fails the running mission the way the engine does for the owner's
//     own death (MissionBridge::FailMission).
//   - **The campaign.** What each mission left behind is kept here as the
//     server numbered it, and every frame the part this machine's game still
//     lacks is found against its own globals and applied, in order. Our own
//     missions' deltas, and whatever a save already had, are found to be
//     there already; a load or a new game looks again. A mission whose every
//     latch (CAMPAIGN_VALUE_LATCH) this game holds already is one its own save
//     passed, and none of it is applied: a save further on never goes back.
//   - **Whose campaign.** The host's. A guest's save may be behind the
//     host's or ahead of it, so a guest starts no story mission or payphone
//     of its own and draws the host's contacts in place of its own
//     (FollowsHostsCampaign, C_ContactMarkers).
//
// Who started or ended what, who is waited for and where, and whose seat is
// kept go to the log as status lines: what the players read is the mission's
// own text, and the chat is left to them. Who a start or a checkpoint is
// waiting for is also a small line in the HUD's corner (WaitLine), since a
// mission held for somebody looks hung otherwise. The one notice that goes in
// the chat is a main.scm that is not ours, which only the player can do
// anything about.
#pragma once

#include <coopiii/mission.h>
#include <coopiii/protocol.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coopiii {

// The engine seam. Every entry is optional; with none set this machine's
// missions stay its own.
struct MissionBridge {
	// A participant's $ONMISSION follows the session's mission. Only ever
	// called on a machine that is not the owner, whose own script keeps it.
	void (*SetOnMission)(bool on) = nullptr;
	// The owner's running mission fails, the way it would for the owner's own
	// death or arrest (missions.md 5.7). `reason` is a MissionFailReason.
	void (*FailMission)(uint8_t reason) = nullptr;
	// One instruction the owner's mission ran, for this machine's engine to
	// run too (game/replay.h). False when it could not be.
	bool (*RunEffect)(const MissionEffectBody &body) = nullptr;
	// RunEffect said no only because a pedestrian or car the instruction
	// names is not built here yet: the session has named it and our copy is on
	// its way, its model still streaming in. The instruction is early, not
	// wrong, and waits for it (MISSION_EFFECT_AWAIT_MS).
	bool (*EffectAwaits)(const MissionEffectBody &body) = nullptr;
	// The mission is over: whatever it left on this machine's radar goes.
	void (*EndEffects)() = nullptr;
	// What a mission somebody else ran left behind in the campaign: its
	// globals written here, its threads started here unless they run here
	// already (protocol.h, C_CampaignDelta). False when it could not be.
	bool (*ApplyCampaign)(const CampaignDeltaBody &body) = nullptr;
	// Which life of main.scm runs here: another after a load or a new game,
	// 0 while none does and nothing can be applied.
	uint32_t (*ScriptLife)() = nullptr;
	// This machine's main.scm, as CampaignDeltaBody::scriptHash counts it.
	uint32_t (*ScriptHash)() = nullptr;
	// One of main.scm's globals as this machine has it. False past them.
	bool (*ReadGlobal)(uint16_t offset, int32_t *value) = nullptr;
	// SET_PLAYER_COORDINATES the owner's mission ran, for this machine's own
	// player: participant `rank` of `count`, the owner not counted, beside
	// the owner's spot (missions.md 11.2). False when it could not be run.
	bool (*Teleport)(const MissionEffectBody &body, uint8_t rank, uint8_t count) = nullptr;
	// The owner's value behind one of the HUD's widgets, for ours to read.
	void (*SetWidget)(uint16_t offset, int32_t value) = nullptr;
	// One of the mission's objects, the one this machine's global at
	// `global` holds, broke on somebody else's machine: ours is broken as far
	// (protocol.h, C_MissionObjectBreak).
	void (*BreakObject)(uint16_t global, float amount, uint8_t state) = nullptr;
	// One of the mission's floating packages, the owner's `ownerHandle`, was
	// taken on somebody else's machine: the owner's mission counts it, and
	// everybody else's copy goes (protocol.h, C_MissionPickup).
	void (*TakePickup)(int32_t ownerHandle) = nullptr;
	// Somebody has just come into this machine's running mission, back from
	// a dropped connection or new to the session: what the mission has up,
	// its blips, pickups, fires, objects and widgets, goes to them alone
	// (docs/missions.md 11.5).
	void (*ResendStanding)(uint8_t playerId) = nullptr;
	// A kill a participant's machine registered, for this machine's running
	// mission to count: CDarkel::RegisteredKills, what
	// GET_NUM_OF_MODELS_KILLED_BY_PLAYER reads, one more for `model`
	// (protocol.h, C_MissionKill).
	void (*CountKill)(uint16_t model) = nullptr;
	// The Import/Export garages' and the emergency crane's lists as this
	// machine's engine has them (protocol.h, C_CarLists): false with no game
	// to read. Written back with the session's bits put in, straight into the
	// engine's own words, so nobody is paid for a car somebody else brought.
	bool (*ReadCarLists)(uint32_t (&out)[CAR_LISTS]) = nullptr;
	void (*WriteCarLists)(const uint32_t (&in)[CAR_LISTS]) = nullptr;
	// This machine's contacts that start missions, as its radar has them now
	// (protocol.h, ContactMarker), for everybody else while it is the host:
	// how many went into `out`.
	size_t (*ReadContactMarkers)(ContactMarker *out, size_t max) = nullptr;
	// This machine's places, the Ammu-Nations, bomb shops, Pay'n'Sprays and
	// safehouses its radar shows (protocol.h, C_PlaceBlips), the same way.
	size_t (*ReadPlaceBlips)(ContactMarker *out, size_t max) = nullptr;
	// The hidden packages still lying in this game's world (protocol.h,
	// C_PackagesLive): count, model and positions filled, false with no game
	// or with a count that does not add up (game/pickup.h,
	// PackagesReportable).
	bool (*ReadPackages)(C_PackagesLive &out) = nullptr;
};

// How often the host looks at its radar for a contact that came or went.
constexpr uint32_t CONTACT_MARKERS_READ_MS = 500;

// While somebody is waited for, the status line saying so comes back this
// often, so the log shows how long the wait went on and how the time ran down.
constexpr uint32_t MISSION_WAIT_REMIND_MS = 15000;
// A checkpoint wait the owner stops asking about is over: they walked out of it.
constexpr uint32_t MISSION_CHECKPOINT_IDLE_MS = 1000;
// A participant who comes into the running mission late (a join, a
// reconnect, a game out of its own intro) or comes back from the hospital or
// the police station is brought beside the owner (docs/missions.md 11.5):
// once this long has passed, so the respawn's fade and the first snapshots
// are over, and only when they are further than this from the owner.
// Anybody nearer can walk, and a start everybody was at brings nobody.
constexpr uint32_t MISSION_SUMMON_DELAY_MS = 3000;
// The distance is the server's missionCatchUp (S_MissionState::catchUpM);
// this is what it is by default.
constexpr float    MISSION_SUMMON_NEAR_M   = MISSION_CATCH_UP_M_DEFAULT;
// In a mission whose checkpoints don't wait (CheckpointsWait: a countdown on
// the screen, a race, an odd job, the RC, 4x4 and Mayhem runs), a participant
// who stays further than this from the owner for this long is brought beside
// them the same way, so they can go on helping. Both are the server's
// missionFallBehind and missionFallBehindTime (S_MissionState::behindM and
// behindS), and these are their defaults.
constexpr float    MISSION_BEHIND_FAR_M    = MISSION_BEHIND_M_DEFAULT;
constexpr uint32_t MISSION_BEHIND_MS       = MISSION_BEHIND_S_DEFAULT * 1000u;
// An instruction of the owner's mission that names a pedestrian or car our
// copy of is still being built waits this long for it, with everything that
// came after it waiting behind it in order, and then goes for what it is
// worth. The owner sends an instruction the moment the session has named what
// it made, and that name reaches us with the spawn a frame or two before our
// copy exists: CREATE_CAR, then its colour or its blip, used to be dropped here
// for arriving first.
constexpr uint32_t MISSION_EFFECT_AWAIT_MS  = 3000;
constexpr size_t   MISSION_EFFECT_AWAIT_MAX = 64;

class MissionSync {
public:
	using SendFn = void (*)(void *ctx, const void *bytes, size_t len, Channel ch);
	using FeedFn = void (*)(void *ctx, const char *line);
	// A status line, for the log and not the chat.
	using StatusFn = void (*)(void *ctx, const char *line);
	// A player's name by id; our own included.
	using NickFn = const char *(*)(void *ctx, uint8_t playerId);
	// Every other player and where their own machine last said they were.
	using PresenceFn = size_t (*)(void *ctx, MissionPresence *out, size_t max);

	// The session's clock as this machine has it, true when there is one: every
	// line about who started, came out of or went into what carries it, since
	// each machine's log counts from its own start and two of them cannot be
	// read side by side otherwise. Called with the roster's context.
	using ClockFn = bool (*)(void *ctx, uint32_t *sessionMs);
	void BindClock(ClockFn clock) { m_clock = clock; }

	void Bind(const MissionBridge *bridge, SendFn send, void *sendCtx, FeedFn feed,
	          StatusFn status, NickFn nick, PresenceFn presence, void *rosterCtx) {
		m_bridge   = bridge;
		m_send     = send;
		m_sendCtx  = sendCtx;
		m_feed     = feed;
		m_status   = status;
		m_nick     = nick;
		m_presence = presence;
		m_rosterCtx = rosterCtx;
	}

	// ---- the engine half asks -------------------------------------------------

	// The local script is at a start gate, and everything else about it says
	// go. True when the session lets it through.
	bool AskStartGate(uint32_t launchKey, uint8_t kind, uint16_t hint, const MissionArea &area,
	                  uint8_t localPlayerId, uint32_t nowMs);

	// The session has answered the gate `launchKey` with a wait: somebody is
	// not at the start yet, or another player's claim holds the slot. Not
	// before the answer, so a start everybody is there for goes straight
	// through without anything shown for it, and not once a mission runs,
	// whose own fades are the screen's then.
	bool StartHeld(uint32_t launchKey, uint32_t nowMs) const {
		return m_serverShares && m_state != MISSION_STATE_RUNNING && m_haveVerdict &&
		       m_verdictKey == launchKey && m_verdict != MISSION_CLAIM_GRANTED &&
		       static_cast<uint32_t>(nowMs - m_verdictAtMs) < MISSION_CLAIM_TTL_MS;
	}

	// The local START_MISSION ran, and whether it became the session's is the
	// server's to say.
	void Launched(uint32_t launchKey, uint16_t missionNumber, uint32_t nowMs);

	// The local mission ended: MISSION_OUTCOME_PASSED or _FAILED.
	void Ended(uint16_t missionNumber, uint8_t outcome, uint32_t nowMs);

	// Something the local mission showed, for everybody else to show too.
	void SendEffect(const MissionEffectBody &body, uint32_t nowMs);

	// What the local mission left behind in the campaign, part by part.
	void SendCampaignDelta(const CampaignDeltaBody &body, uint32_t nowMs);

	// A timer or counter the local mission has on the HUD, and the value of
	// its global now, every frame: sent when the others' copies would be
	// wrong by now (protocol.h, C_MissionWidget).
	void WidgetValue(uint16_t offset, int32_t value, bool timer, bool frozen,
	                 uint8_t localPlayerId, uint32_t nowMs);

	// A checkpoint of the local mission that the owner is in. True once every
	// participant is there too; until then the others are told who is missing.
	// True as well once it has waited the server's checkpoint wait for them: the
	// mission goes on without them. Always true when this machine does not
	// own the session's mission.
	// Not at all while CheckpointsWait says no: the owner's arrival is the
	// checkpoint then.
	bool AskCheckpoint(const MissionArea &area, uint8_t localPlayerId, uint32_t nowMs);
	// Whether the session's mission's checkpoints wait for everybody, with the
	// server's say in it (CheckpointsWait in coopiii/mission.h).
	bool CheckpointsWaitNow() const {
		return CheckpointsWait(m_number, m_timerUp, m_flags, m_cpWaitS);
	}
	uint16_t CheckpointWaitS() const { return m_cpWaitS; }
	uint16_t CatchUpM() const { return m_catchUpM; }
	// Whether the session's mission has a countdown on this machine's screen
	// now: the owner's own, or the one replayed here. Every frame.
	void SetTimerUp(bool up) { m_timerUp = up; }
	bool TimerUp() const { return m_timerUp; }
	// How many checkpoints went on without somebody.
	uint32_t CheckpointsGivenUp() const { return m_cpGivenUp; }

	// ---- bringing a late participant to the owner -------------------------------
	//
	// This machine's player came back from the hospital or the police
	// station. In somebody else's running mission, they are brought beside
	// its owner (MISSION_SUMMON_*).
	void Respawned(uint8_t localPlayerId, uint32_t nowMs);
	// This machine's player, at `localPos`, in somebody else's mission whose
	// checkpoints don't wait: MISSION_BEHIND_FAR_M from the owner for
	// MISSION_BEHIND_MS owes a move to them. As often as the engine half looks.
	void WatchBehind(const Vec3 &localPos, uint8_t localPlayerId, uint32_t nowMs);
	uint32_t BehindMoves() const { return m_behindMoves; }
	// A move to the owner is owed, for the engine half to ask about once its
	// player can be moved: alive, out of a cutscene, no other move going on.
	bool SummonPending() const { return m_summon; }
	// Whether to move this machine's player, at `localPos`, beside the owner
	// now: where the owner is, and our place among the participants who are
	// not the owner. True hands the move over and forgets it; a player near
	// the owner already is not moved, and the move is forgotten too.
	bool TakeSummon(const Vec3 &localPos, uint8_t localPlayerId, uint32_t nowMs, Vec3 *ownerPos,
	                uint8_t *slot, uint8_t *count);
	uint32_t Summons() const { return m_summons; }

	// The line the HUD shows while the session waits for somebody, at a start
	// or at a checkpoint of the mission this machine is in: who, and how long
	// until it goes on without them when it will. False, with `out` empty,
	// when nobody is waited for.
	bool WaitLine(char *out, size_t cap, uint8_t localPlayerId, uint32_t nowMs) const;

	// The local mission is waiting for the models its LOAD_ALL_MODELS_NOW
	// number `readySeq` asked for (protocol.h, C_MissionReady). True once
	// every participant has loaded them too, or MISSION_READY_WAIT_MS after
	// the first ask; always true when this machine does not own the
	// session's mission.
	bool AskEverybodyLoaded(uint16_t readySeq, uint8_t localPlayerId, uint32_t nowMs);

	// The session's cars whose seats the local mission's pedestrians are
	// heading for (protocol.h, C_MissionSeats), as often as the engine half
	// looks: sent when the list changes, and every MISSION_SEATS_RESYNC_MS
	// while it is not empty.
	void SeatsNeeded(const MissionSeatCar *cars, size_t count, uint8_t localPlayerId,
	                 uint32_t nowMs);

	// One of the running mission's floating packages was taken here, named by
	// the owner's handle for it (protocol.h, C_MissionPickup).
	void PickupTaken(int32_t ownerHandle, uint32_t nowMs);

	// This machine's engine registered a kill of `model` as the player's: its
	// own, or somebody else's hit on a pedestrian it hosts. A participant's
	// goes to the owner, whose mission counts it (protocol.h, C_MissionKill).
	void KillRegistered(uint16_t model, uint8_t localPlayerId, uint32_t nowMs);
	// Whether somebody else's hit that kills a pedestrian this machine hosts
	// is put through the engine's own kill register here: while the
	// session's mission runs with this machine in it.
	bool CreditsRemoteKills(uint8_t localPlayerId) const {
		return m_serverShares && !m_busy && m_state == MISSION_STATE_RUNNING &&
		       (m_owner == localPlayerId || (m_participants & PlayerBit(localPlayerId)) != 0);
	}

	// This machine's game runs a mission of its own that is not the session's
	// (the intro of a new game), as often as the engine half looks: the
	// server hears when it changes (protocol.h, C_MissionBusy), and nothing
	// of the session's mission is run here meanwhile.
	void SetBusy(bool busy, uint32_t nowMs);
	bool Busy() const { return m_busy; }
	// Whether the mission this machine's game launched is the session's, or
	// may be yet: the server has not answered the launch, and nobody else's
	// runs. Not one launched before the server shared missions, nor one the
	// session ended while it ran on, somebody else's having started while our
	// connection was down say.
	bool OwnLaunchIsTheSessions(uint8_t localPlayerId) const {
		if (m_state == MISSION_STATE_RUNNING)
			return m_owner == localPlayerId;
		return m_launchPending;
	}
	// This machine's game has started over, a load or a new game: whatever of
	// the running mission it had is gone, and the owner is asked for it again.
	void StartedOver(uint8_t localPlayerId, uint32_t nowMs);

	// One of the running mission's objects broke here, by this machine's own
	// doing (protocol.h, C_MissionObjectBreak): for everybody else's copy.
	void ObjectBroken(uint16_t global, float amount, uint8_t state, uint32_t nowMs);

	// Whether this machine's player may sit down in the session's car
	// `netId`: not while the owner's mission needs its seats (mission-audit.md
	// R4). False says so in the log.
	bool MaySit(uint16_t netId, uint32_t nowMs);
	// Whether this machine's player, riding in `netId`, is to get out for the
	// mission's passengers: the owner named him, or named nobody, which is
	// everybody. True says so in the log, once for each time.
	bool MustLeaveSeat(uint16_t netId, uint8_t localPlayerId);

	// A warp of the local mission has put its player in the session's car
	// `netId`, and these are the passenger seats handed out for it
	// (protocol.h, C_MissionBoard): seats[n] for player n, 0 for none.
	void Board(uint16_t netId, const uint8_t *seats, uint8_t flags, uint8_t localPlayerId,
	           uint32_t nowMs);
	// The seat the owner handed this machine's player, while it is still to
	// be taken: the car, the wire seat, bit s of `givenToOthers` for each
	// seat somebody else got, and when it was said. False with none.
	bool BoardPending(uint16_t &netId, uint8_t &seat, uint16_t &givenToOthers,
	                  uint32_t &sinceMs) const;
	void BoardDone() { m_board.live = false; }
	// The participants the local mission's checkpoints stop waiting for,
	// since they cannot follow its player where he is (game/mission.h,
	// CheckpointOutOfReach). Said in the log when it changes.
	void SetOutOfReach(uint8_t mask, uint8_t localPlayerId);
	uint8_t OutOfReach() const { return m_cpOutOfReach; }

	// ---- whose campaign the session's is -------------------------------------------
	//
	// The host's (Session::HostId, the player whose clock the session keeps;
	// protocol.h, C_ContactMarkers). Every machine runs its own save's
	// main.scm, and a guest's may be behind the host's or ahead of it. So
	// while a host is known and the session shares missions, a guest's own
	// contacts are not drawn and its own story and payphone triggers are not
	// let through (AskStartGate), and the host's contacts are drawn instead.
	// Nothing of it is written into the guest's game: its own contacts stay in
	// its radar and in its save, and come back the moment it leaves.

	// The session's host as the welcome and the world packets say: another
	// one forgets the last one's contacts.
	void    SetHost(uint8_t hostId);
	uint8_t Host() const { return m_host; }
	// This machine's own save is not the session's campaign: the server shares
	// missions, the engine half is here, and somebody else is the host.
	bool FollowsHostsCampaign(uint8_t localPlayerId) const {
		return m_serverShares && m_bridge && m_bridge->ScriptLife && m_host < MAX_PLAYERS &&
		       m_host != localPlayerId && localPlayerId < MAX_PLAYERS;
	}
	// The host's contacts, for the radar to draw: none unless this machine
	// follows the host's campaign.
	size_t HostContacts(const ContactMarker **out, uint8_t localPlayerId) const {
		*out = m_hostContacts;
		return FollowsHostsCampaign(localPlayerId) ? m_hostContactCount : 0;
	}
	void OnContactMarkers(const S_ContactMarkers &pkt, uint8_t localPlayerId);
	// The host's places, for the radar to draw beside this machine's own:
	// none unless this machine follows the host's campaign.
	size_t HostPlaces(const ContactMarker **out, uint8_t localPlayerId) const {
		*out = m_hostPlaces;
		return FollowsHostsCampaign(localPlayerId) ? m_hostPlaceCount : 0;
	}
	void OnPlaceBlips(const S_PlaceBlips &pkt, uint8_t localPlayerId);
	uint32_t PlaceListsSent() const { return m_placeListsSent; }
	uint32_t ContactListsSent() const { return m_contactListsSent; }

	// The host's own trigger has just found its player outside the locate
	// `area`. True, with the one standing there in `who`, when `area` is one
	// of our contacts' markers (the list we last sent the session), our
	// player, at `localPos`, is within MISSION_START_RADIUS_M of it, and
	// another player, not in a mission of their own, stands in it: the
	// guests walk into the host's marker, so their standing in it is the
	// host's player standing in it (protocol.h, "a guest at the host's
	// marker"). Never on a guest, and never behind a server that does not
	// share missions. The marker is matched to the locate with
	// CONTACT_LOCATE_SLACK_M (AreaHoldsMarker), and "in it" is the locate's
	// own box with CONTACT_STAND_SLACK_M, the slack a held start gate gives
	// the host's own player.
	static constexpr float CONTACT_LOCATE_SLACK_M = 3.0f;
	static constexpr float CONTACT_STAND_SLACK_M  = 0.5f;
	bool OtherPlayerAtOurContact(const MissionArea &area, const Vec3 &localPos,
	                             uint8_t localPlayerId, uint8_t *who = nullptr) const;

	// ---- inbound ----------------------------------------------------------------
	void OnClaim(const S_MissionClaim &pkt, uint8_t localPlayerId, uint32_t nowMs);
	void OnWaiting(const S_MissionWaiting &pkt, uint8_t localPlayerId, uint32_t nowMs);
	void OnState(const S_MissionState &pkt, uint8_t localPlayerId, uint32_t nowMs);
	void OnFail(const S_MissionFail &pkt, uint8_t localPlayerId);
	// Under a shared wallet the owner's pay is everybody's already, and
	// paying it again here would pay it twice.
	void OnEffect(const S_MissionEffect &pkt, uint8_t localPlayerId, bool sharedWallet,
	              uint32_t nowMs);
	// A participant has loaded what our mission asked for.
	void OnReady(const S_MissionReady &pkt, uint8_t localPlayerId);
	// The cars whose seats the owner's mission needs.
	void OnSeats(const S_MissionSeats &pkt, uint8_t localPlayerId);
	// The seats in the car the owner's mission put its player in.
	void OnBoard(const S_MissionBoard &pkt, uint8_t localPlayerId, uint32_t nowMs);
	// One of the mission's objects broke on somebody else's machine.
	void OnObjectBroken(const S_MissionObjectBreak &pkt, uint8_t localPlayerId);
	// One of the mission's floating packages was taken on somebody else's.
	void OnPickupTaken(const S_MissionPickup &pkt, uint8_t localPlayerId);
	// A participant's kill, for the running mission this machine owns.
	void OnKill(const S_MissionKill &pkt, uint8_t localPlayerId);
	uint32_t KillsCounted() const { return m_killsCounted; }

	// What this participant's own engine answers to the owner's questions
	// (protocol.h, C_MissionAnswers): `hasCar`, bit i while
	// IS_CAR_IN_MISSION_GARAGE is true of garage i here, `resprayed`, bit i for
	// a HAS_RESPRAY_HAPPENED of garage i that has just said yes, and
	// `shotDown`, a mission plane that has just gone down here
	// (MISSION_SHOT_DOWN_*). Said when any of it has news.
	void Answers(uint32_t hasCar, uint32_t resprayed, uint16_t shotDown, uint8_t localPlayerId,
	             uint32_t nowMs);
	void OnAnswers(const S_MissionAnswers &pkt, uint8_t localPlayerId);
	// The owner's side: whether somebody in the mission has its car in garage
	// `garage` now, and a respray there, or a plane of `shotDown` brought
	// down, nobody has asked about yet, which asking takes.
	bool GarageHasCarElsewhere(uint8_t garage) const;
	bool TakeResprayElsewhere(uint8_t garage);
	bool TakeShotDownElsewhere(uint16_t plane);
	// This machine is in somebody else's running mission, its game in none
	// of its own: what asks its garages for the owner.
	bool ParticipantHere(uint8_t localPlayerId) const {
		return m_serverShares && !m_busy && m_state == MISSION_STATE_RUNNING &&
		       m_owner != localPlayerId && (m_participants & PlayerBit(localPlayerId)) != 0;
	}
	// A participant's game started over: our mission hands them what it has
	// up again.
	void OnHandOver(const S_MissionHandOver &pkt, uint8_t localPlayerId, uint32_t nowMs);
	// Kept in the server's order; Tick applies what this game lacks.
	void OnCampaignDelta(const S_CampaignDelta &pkt);
	// The session's Import/Export and crane lists; Tick puts in what this
	// game lacks, and says what it has that they lack.
	void OnCarLists(const S_CarLists &pkt);
	uint32_t CarListCarsTaken() const { return m_carListsTaken; }
	void OnWidget(const S_MissionWidget &pkt, uint8_t localPlayerId);

	// ---- per frame ----------------------------------------------------------------
	void Tick(uint8_t localPlayerId, uint32_t nowMs);

	// The session is gone. A participant's $ONMISSION goes back to what its own
	// script left it at. An owner's mission goes on here, and is still counted
	// the session's (OwnLaunchIsTheSessions): the server that failed it for
	// everybody when the connection went is asked to take it up again as soon
	// as it answers the reconnect, unless somebody else's mission runs by
	// then. The campaign log stays: a reconnect to the same server only asks
	// for what came after it.
	void Clear();

	// ---- what the HUD and the tests read -------------------------------------------
	bool     Shared() const { return m_serverShares; }
	bool     Running() const { return m_state == MISSION_STATE_RUNNING; }
	uint8_t  Owner() const { return m_owner; }
	uint16_t Number() const { return m_number; }
	// How the last session's mission this machine saw end ended, as the
	// bridge's EndEffects runs: MISSION_OUTCOME_NONE when it did not see the
	// end, a lost connection say.
	uint8_t  LastOutcome() const { return m_lastOutcome; }
	uint8_t  Participants() const { return m_participants; }
	uint16_t MarginCm() const { return m_marginCm; }
	// How much tougher the running mission's enemies are for the players in
	// it (docs/missions.md 10.2, the server's missionEnemies and
	// missionScale): 1 for single player's, and for one player.
	float EnemyToughness() const;
	// How many copies of each generic enemy the running mission gets
	// (docs/missions.md 10.5, `missionEnemies = more`): one for each player
	// past the first, MISSION_ENEMY_COPIES_MAX at most; 0 under any other
	// rule.
	uint8_t EnemyCopies() const;
	bool     Mirroring() const { return m_mirroring; }
	uint8_t  WaitingFor() const { return m_waitWhat == MISSION_WAIT_NONE ? 0 : m_waitMissing; }
	// Of those, the ones whose game is in a mission of its own.
	uint8_t  WaitingInOwnMission() const {
		return m_waitWhat == MISSION_WAIT_NONE ? 0 : static_cast<uint8_t>(m_waitMissing & m_waitBusy);
	}
	uint32_t ClaimsSent() const { return m_claimsSent; }
	uint32_t EffectsRun() const { return m_effectsRun; }
	// The owner's instructions waiting here for a copy of ours to be built,
	// and how many ever had to.
	size_t   EffectsAwaiting() const { return m_awaiting.size(); }
	uint32_t EffectsAwaited() const { return m_effectsAwaited; }
	// The last campaign delta this machine has, its own included. Kept across
	// a dropped connection: it is what asks for the ones missed meanwhile.
	uint32_t CampaignSeq() const { return static_cast<uint32_t>(m_log.size()); }
	// How many of them this life of the game has, applied or found there.
	size_t   CampaignSettled() const { return m_settled; }
	uint32_t CampaignApplied() const { return m_campaignApplied; }
	// Missions of the log this game's own save had passed already, and so
	// left as it has them (protocol.h, CAMPAIGN_VALUE_LATCH).
	uint32_t CampaignKeptOwn() const { return m_campaignKeptOwn; }
	// The text key of the session's newest passed mission: the
	// REGISTER_MISSION_PASSED (0318) of the newest delta from main.scm `hash`
	// that carries one, applied here or not. `key` gets the eight bytes and a
	// terminator. False when no such delta has come, or its key is empty.
	bool     LastPassedKey(uint32_t hash, char key[9]) const;
	uint32_t WidgetsSent() const { return m_widgetsSent; }
	// How many times somebody who came into our running mission has been
	// handed what it has up.
	uint32_t StandingHandedOver() const { return m_standingHanded; }
	// How many times this machine has asked for what the running mission has
	// made (C_MissionCatchUp).
	uint32_t CatchUpsAsked() const { return m_catchUps; }
	uint32_t ObjectBreaksApplied() const { return m_objectBreaks; }
	// Where we stand when the owner's mission moves everybody: our place
	// among the session's other players, the owner not counted, and how
	// many of us there are.
	void TeleportRank(uint8_t ownerId, uint8_t localPlayerId, uint8_t *rank, uint8_t *count) const;
	uint8_t  CheckpointMissing() const { return m_cpReported; }

private:
	void Notice(const char *line);
	void Statusf(const char *fmt, ...);
	// "bob", "bob and carol", "bob, carol and dave", with "you" for us.
	void Names(char *out, size_t cap, uint8_t mask, uint8_t localPlayerId) const;
	const char *NickOf(uint8_t playerId) const;
	void AnnounceWait(uint8_t localPlayerId, uint32_t nowMs);
	void SetMirror(bool on);
	// Every pedestrian and car the running mission has made, asked for again
	// (C_MissionCatchUp).
	void AskCatchUp(uint32_t nowMs);
	// " at session time N ms", or nothing without a session clock.
	void SessionAt(char *out, size_t cap) const;
	void EndEffects();
	// One of the owner's instructions, run here. False when it names a copy
	// of ours still being built and `lastChance` is not set: then nothing was
	// run and it waits in m_awaiting.
	bool TryEffect(const S_MissionEffect &pkt, uint8_t localPlayerId, uint32_t nowMs,
	               bool lastChance);
	void RunAwaiting(uint8_t localPlayerId, uint32_t nowMs);
	const MissionSeatCar *SeatKept(uint16_t netId) const;
	void ForgetSeats();
	void SendBusy(bool fresh, uint32_t nowMs);
	// A player who has just come into our running mission is handed what it
	// has up, and the widgets' values straight after.
	void HandOver(uint8_t playerId, uint32_t nowMs);
	// The part of the log this game lacks, applied.
	void SettleCampaign();
	// Every value of `body` is already what this game has: it, and every
	// delta before it, is in.
	bool AlreadyHere(const CampaignDeltaBody &body, uint32_t hash) const;
	// The first and last of the log's parts of the mission the one at `i` is
	// a part of; false while its last part is still to come.
	bool MissionParts(size_t i, size_t *start, size_t *end) const;
	// This game's own save has passed that mission: it holds every latch the
	// mission's parts set, and there is one at least.
	bool PassedHere(size_t start, size_t end, uint32_t hash) const;

	template <class T>
	void Out(const T &pkt, Channel ch) {
		if (m_send)
			m_send(m_sendCtx, &pkt, sizeof(T), ch);
	}

	const MissionBridge *m_bridge    = nullptr;
	SendFn               m_send      = nullptr;
	void                *m_sendCtx   = nullptr;
	FeedFn               m_feed      = nullptr;
	StatusFn             m_status    = nullptr;
	NickFn               m_nick      = nullptr;
	PresenceFn           m_presence  = nullptr;
	ClockFn              m_clock     = nullptr;
	void                *m_rosterCtx = nullptr;

	// The server has said what the session's mission is at least once, so it
	// shares missions and a claim will be answered.
	bool     m_serverShares = false;

	uint8_t  m_state        = MISSION_STATE_IDLE;
	uint8_t  m_owner        = INVALID_PLAYER;
	uint16_t m_number       = MISSION_NONE;
	uint8_t  m_lastOutcome  = MISSION_OUTCOME_NONE;
	uint8_t  m_participants = 0;
	uint8_t  m_flags        = 0;
	uint16_t m_marginCm     = MISSION_MARGIN_CM_DEFAULT;
	uint8_t  m_enemies      = MISSION_ENEMIES_ORIGINAL;
	uint16_t m_scalePct     = MISSION_SCALE_DEFAULT;
	// The rest of the server's mission rules (S_MissionState).
	uint16_t m_cpWaitS      = MISSION_CHECKPOINT_WAIT_MS / 1000;
	uint16_t m_catchUpM     = MISSION_CATCH_UP_M_DEFAULT;
	uint16_t m_behindM      = MISSION_BEHIND_M_DEFAULT;
	uint16_t m_behindS      = MISSION_BEHIND_S_DEFAULT;
	bool     m_mirroring    = false;

	// Our claim at a start gate.
	uint32_t m_claimKey      = 0;
	bool     m_claimLive     = false;   // one has gone out for m_claimKey
	uint32_t m_claimSentMs   = 0;
	uint32_t m_claimsSent    = 0;
	uint8_t  m_verdict       = MISSION_CLAIM_WAITING;
	uint32_t m_verdictKey    = 0;
	uint32_t m_verdictAtMs   = 0;
	bool     m_haveVerdict   = false;
	bool     m_saidBusy      = false;

	// Who the session waits for, as S_MissionWaiting last said.
	uint8_t  m_waitOwner    = INVALID_PLAYER;
	uint8_t  m_waitMissing  = 0;
	uint8_t  m_waitWhat     = MISSION_WAIT_NONE;
	uint16_t m_waitHint     = MISSION_NONE;
	uint32_t m_waitSaidMs   = 0;
	uint8_t  m_waitBusy     = 0;       // missing because their game is in its own
	bool     m_waitGoesOn   = false;   // the start goes on without those at m_waitGoesOnMs
	uint32_t m_waitGoesOnMs = 0;

	uint32_t m_effectsRun   = 0;
	// The owner's instructions that arrived before our copy of what they name,
	// in the order they came (MISSION_EFFECT_AWAIT_MS).
	struct AwaitingEffect {
		S_MissionEffect pkt{};
		uint32_t        sinceMs = 0;
		// Not run until something is behind it (missionsync.cpp, HoldsForNext).
		bool            holdForNext = false;
	};
	std::vector<AwaitingEffect> m_awaiting;
	uint32_t m_effectsAwaited = 0;
	uint32_t m_standingHanded = 0;
	uint32_t m_catchUps       = 0;
	uint32_t m_killsCounted   = 0;
	// The garages (mission-audit.md R5): what we last said as a participant,
	// and as the owner what each participant last said, with the resprays
	// nobody has asked about yet.
	uint32_t m_garageSent                  = 0;
	uint32_t m_garageHasCar[MAX_PLAYERS]   = {};
	uint32_t m_garageResprays              = 0;
	uint16_t m_shotDown                    = 0;   // planes brought down elsewhere, unasked
	uint32_t m_objectBreaks   = 0;

	// As the owner: the last of our mission's loads each participant has
	// done (S_MissionReady), and the one our mission waits on since when.
	uint16_t m_ready[MAX_PLAYERS] = {};
	uint16_t m_readyAsked         = 0;
	uint32_t m_readySinceMs       = 0;
	bool     m_readyGaveUp        = false;
	// Our START_MISSION has gone out and the server has not said yet what
	// became of it.
	bool     m_launchPending      = false;
	uint16_t m_launchedNumber     = MISSION_NONE;

	// This machine's game in a mission of its own, and what the server was
	// last told of it on this connection.
	bool m_busy     = false;
	bool m_busySent = false;

	// As the owner, the seats our mission needs as last sent; as anybody
	// else, as the owner last said.
	MissionSeatCar m_seatsSent[MISSION_SEAT_CARS] = {};
	uint8_t        m_seatsSentCount  = 0;
	uint32_t       m_seatsSentMs     = 0;
	MissionSeatCar m_seatsKept[MISSION_SEAT_CARS] = {};
	uint8_t        m_seatsKeptCount  = 0;
	uint32_t       m_seatRefusedMs   = 0;
	bool           m_seatRefusedSaid = false;
	uint16_t       m_seatLeaveSaid   = INVALID_NETID;

	// As a participant, the seat the owner last handed us in the car its
	// mission put its player in.
	struct BoardSeat {
		bool     live          = false;
		uint16_t netId         = INVALID_NETID;
		uint8_t  seat          = 0;
		uint16_t givenToOthers = 0;
		uint32_t sinceMs       = 0;
	};
	BoardSeat m_board;
	// As the owner, who our checkpoints do not wait for.
	uint8_t m_cpOutOfReach = 0;

	// The owner's widgets as last sent.
	struct WidgetSent {
		uint16_t offset = 0;
		bool     sent   = false;
		bool     timer  = false;
		bool     frozen = false;
		int32_t  value  = 0;
		uint32_t atMs   = 0;
	};
	WidgetSent m_widgets[2];
	uint32_t   m_widgetsSent = 0;

	// The session's campaign log as this machine has it: delta i + 1 at [i],
	// with no gaps. One that arrives ahead of a gap waits in m_early.
	struct CampaignEntry {
		uint8_t           ownerId = INVALID_PLAYER;
		CampaignDeltaBody body    = {};
	};
	std::vector<CampaignEntry> m_log;
	std::vector<CampaignEntry> m_early;
	uint32_t m_campaignLog     = 0;   // S_MissionState::campaignLog it is from
	size_t   m_settled         = 0;   // how many m_life has
	uint32_t m_life            = 0;
	uint32_t m_campaignApplied = 0;
	uint32_t m_campaignKeptOwn = 0;
	bool     m_saidOtherScript = false;

	// The session's Import/Export and crane lists as the server last said,
	// and ours as last sent on this connection (TickCarLists).
	uint32_t m_carLists[CAR_LISTS] = {};
	bool     m_carListsAsked       = false;
	uint32_t m_carListsSentMs      = 0;
	uint32_t m_carListsReadMs      = 0;
	uint32_t m_carListsTaken       = 0;   // cars put into this game from the session's
	void TickCarLists(uint32_t nowMs);

	// Whose campaign it is (SetHost), the host's contacts as last heard, and
	// as the host ours as last sent on this connection.
	uint8_t       m_host             = INVALID_PLAYER;
	ContactMarker m_hostContacts[CONTACT_MARKERS_MAX] = {};
	uint8_t       m_hostContactCount = 0;
	ContactMarker m_contactsSent[CONTACT_MARKERS_MAX] = {};
	uint8_t       m_contactsSentCount = 0;
	bool          m_contactsSentLive  = false;
	uint32_t      m_contactsReadMs    = 0;
	uint32_t      m_contactListsSent  = 0;
	bool          m_saidFollowing     = false;
	// The same for the host's places (C_PlaceBlips).
	ContactMarker m_hostPlaces[PLACE_BLIPS_MAX] = {};
	uint8_t       m_hostPlaceCount   = 0;
	ContactMarker m_placesSent[PLACE_BLIPS_MAX] = {};
	uint8_t       m_placesSentCount  = 0;
	bool          m_placesSentLive   = false;
	uint32_t      m_placeListsSent   = 0;
	void TickPlaces(uint8_t localPlayerId, uint32_t nowMs);
	void TickContacts(uint8_t localPlayerId, uint32_t nowMs);
	// The hidden packages lying in this game's world, as last told on this
	// connection (TickPackages): told again when they change.
	bool     m_packagesSent    = false;
	uint32_t m_packagesSig     = 0;
	uint32_t m_packagesReadMs  = 0;
	void TickPackages(uint32_t nowMs);

	// Our own checkpoint wait, as the owner.
	uint8_t  m_cpReported   = 0;
	Vec3     m_cpWhere      = {};
	uint32_t m_cpAskedMs    = 0;
	Vec3     m_cpAt         = {};      // the checkpoint last asked about
	bool     m_cpWaiting    = false;   // somebody is missing from it
	uint32_t m_cpSinceMs    = 0;       // since when
	bool     m_cpGaveUp     = false;   // it went on without them
	uint32_t m_cpGivenUp    = 0;

	// A move to the owner that is owed (MISSION_SUMMON_*), since when.
	bool     m_summon       = false;
	// The move owed is for falling behind (WatchBehind), not for coming in late.
	bool     m_summonBehind = false;
	uint32_t m_summonSinceMs = 0;
	uint32_t m_summons      = 0;
	// Far behind the owner in a mission that does not wait, since when.
	bool     m_behind        = false;
	uint32_t m_behindSinceMs = 0;
	uint32_t m_behindMoves   = 0;
	// A countdown is on the screen; said once a mission that its checkpoints
	// go on without anybody.
	bool     m_timerUp       = false;
	bool     m_cpSaidNoWait  = false;

	// Our own mission, still running here after the connection dropped: on
	// the way back it is offered to the session again (C_MissionStarted).
	bool     m_readopt      = false;
	bool     m_readoptSent  = false;   // offered, and not answered yet
	bool     m_ownsRunning  = false;   // the session's running mission is ours
};

} // namespace coopiii
