// The police helicopter, session side. No engine code in here: the engine half
// is game/heli.cpp and everything it does is reached through HeliBridge, so
// tools/clienttest drives all of this with a stub.
//
// protocol.h's entry 32 is the design. In short:
//
//   - the machine whose engine built a police helicopter owns it and streams
//     it (C_HeliState) until it is finished (C_HeliGone);
//   - every other machine builds a replica, keeps it out of the engine's own
//     helicopter array and puts it where the stream says;
//   - a hit the local player lands on a replica goes to the owner
//     (C_HeliHit), whose engine decides what it cost;
//   - a replica whose stream stops, or whose owner leaves, climbs away here
//     and is removed;
//   - every round the owner's helicopter fires goes out (C_HeliShot) and is
//     drawn from the replica when its playback gets there, dealing no damage
//     (game/heligun.h).
#pragma once

#include "clock.h"
#include "interp.h"

#include <coopiii/protocol.h>

#include <cstddef>
#include <cstdint>

namespace coopiii {

// How long a replica is held with nothing new from its owner before it is
// treated as abandoned. Twenty states at HELI_STATE_HZ.
constexpr uint32_t HELI_STALE_MS = 2000;

// An abandoned replica climbs at this rate until it is out of sight, then is
// removed. Not the engine's fly-away transcribed - the engine accelerates
// toward z = 1000 and clamps the climb at 0.3 per timestep, 15 m/s - just a
// steady rate in the same range, so the helicopter visibly leaves instead of
// vanishing in front of the player.
constexpr float    HELI_ORPHAN_CLIMB_MPS = 10.0f;
// UpdateHelis deletes a helicopter that is flying away once it is higher than
// this (`fcomp [00601BC4h]`, 150.0, at 0x00549BC0).
constexpr float    HELI_GONE_HEIGHT      = 150.0f;
// And a cap, for a replica that was abandoned somewhere already high.
constexpr uint32_t HELI_ORPHAN_MAX_MS    = 15000;

// One observer can hold two helicopters per other player.
constexpr size_t MAX_REMOTE_HELIS = MAX_PLAYERS * HELI_POLICE_SLOTS;

// Serials an observer remembers per owner as finished, so a state that
// overtook its own C_HeliGone doesn't build the helicopter again.
constexpr uint8_t HELI_GONE_RING = 8;

// What a replica is told to look like, beside its transform.
struct HeliLook {
	uint8_t status               = HELI_STATUS_HOVER;
	uint8_t flags                = 0;
	float   searchLightX         = 0.0f;
	float   searchLightY         = 0.0f;
	float   searchLightIntensity = 0.0f;
	Vec3    velocity             = {};   // m/s
};

// Somebody else's police helicopter, as this machine holds it.
struct RemoteHeli {
	bool     active = false;
	uint8_t  owner  = INVALID_PLAYER;
	uint8_t  slot   = 0;
	uint16_t serial = 0;

	// CPools::GetVehicleRef of the replica, -1 until it is built.
	int32_t  poolHandle = -1;

	VehicleInterpBuffer interp;
	VehicleTransform    last{};
	HeliLook            look{};
	bool                haveState = false;
	uint32_t            lastHeardMs = 0;

	// The first blast has been played on the replica.
	bool     tailBlownPlayed = false;

	// Nothing more is coming: the stream went quiet or the owner left. The
	// replica climbs from `last` and is removed.
	bool     orphaned     = false;
	bool     ownerLeft    = false;
	uint32_t orphanedAtMs = 0;
	uint32_t lastTickMs   = 0;

	bool Spawned() const { return poolHandle >= 0; }
};

// One of the local engine's own police helicopters, as the sampler reads it.
// `body.serial` is left for HeliSync to fill; everything else is the engine's.
struct OwnHeliSample {
	int32_t       handle = -1;   // CPools::GetVehicleRef
	HeliStateBody body{};
};

// One of the local engine's own police helicopters leaving its slot.
struct OwnHeliGone {
	uint8_t slot           = 0;
	int32_t handle         = -1;
	uint8_t reason         = HELI_GONE_VANISHED;
	uint8_t creditPlayerId = INVALID_PLAYER;
	Vec3    pos            = {};
};

// A round one of the local engine's own police helicopters fired: the two
// points it handed FireOneInstantHitRound. Drained in the same frame it was
// fired, so the C_HeliShot is stamped with the time the frame's C_HeliState
// sample is - the round and the pose it was fired from share an instant.
struct OwnHeliShot {
	uint8_t slot   = 0;
	int32_t handle = -1;   // CPools::GetVehicleRef of the one that fired
	Vec3    source = {};
	Vec3    target = {};
};

// How long an observer holds a round before drawing it, so it goes off when
// the replica's playback reaches the instant it was fired.
//
// The replica is drawn about VehicleInterpBuffer::DELAY_MS behind the newest
// state its owner sent, so a round fired at `shotSendMs` belongs
// `shotSendMs - newestStateMs + DELAY_MS` from now. A round that is older than
// that is drawn at once, and one that is far newer than every state - the
// states were lost - is held no longer than HELI_SHOT_MAX_HOLD_MS.
constexpr uint32_t HELI_SHOT_MAX_HOLD_MS = 2 * VehicleInterpBuffer::DELAY_MS;

inline uint32_t HeliShotHoldMs(uint32_t shotSendMs, uint32_t newestStateMs) {
	const int32_t ahead = static_cast<int32_t>(shotSendMs - newestStateMs);
	const int64_t hold  = static_cast<int64_t>(ahead) + VehicleInterpBuffer::DELAY_MS;
	if (hold <= 0)
		return 0;
	if (hold >= HELI_SHOT_MAX_HOLD_MS)
		return HELI_SHOT_MAX_HOLD_MS;
	return static_cast<uint32_t>(hold);
}

// Rounds waiting for their replica to catch up. Two helicopters for each of
// seven other players, each firing at most every 200 ms, held at most
// HELI_SHOT_MAX_HOLD_MS: 7 * 2 * 2 = 28.
constexpr size_t MAX_PENDING_HELI_SHOTS = 32;

// A hit the local player's gun or rocket landed on a replica.
struct LocalHeliHit {
	uint8_t  owner  = INVALID_PLAYER;
	uint8_t  slot   = 0;
	uint16_t serial = 0;
	uint8_t  kind   = HELI_HIT_BULLET;
	uint16_t damage = 0;
};

// The engine seam. Every entry is optional; with none set the helicopter stays
// what it was before, one machine's own.
struct HeliBridge {
	// ---- the owner ----------------------------------------------------------
	// The local engine's police helicopters right now, slots 0 and 1 only.
	uint8_t (*SampleOwnHelis)(OwnHeliSample *out, uint8_t max) = nullptr;
	// The ones that left their slot since the last call, oldest first.
	uint8_t (*DrainOwnHeliGone)(OwnHeliGone *out, uint8_t max) = nullptr;
	// Somebody else's hit on our own helicopter, through the engine's rule.
	// False when the helicopter is no longer the one in that slot.
	bool (*ApplyHeliHit)(uint8_t slot, int32_t handle, uint8_t attackerId,
	                     const HeliHitBody &hit) = nullptr;
	// The session has ended: forget who hit what.
	void (*ResetHeliSession)() = nullptr;

	// ---- the shooter --------------------------------------------------------
	uint8_t (*DrainLocalHeliHits)(LocalHeliHit *out, uint8_t max) = nullptr;
	// Our hit brought somebody else's helicopter down. The crime and the
	// statistics the owner's engine would have given itself.
	void (*CreditHeliShootDown)(uint8_t slot, const Vec3 &pos) = nullptr;
	// And the $250 its engine took back, when the session's money rule says
	// an award goes to whoever earned it (protocol.h, MoneyRule).
	void (*PayHeliShootDown)() = nullptr;

	// ---- the observer -------------------------------------------------------
	// Ask for MI_CHOPPER; true once it is loaded.
	bool (*RequestHeliModel)() = nullptr;
	// Build the replica at heli.last. Sets poolHandle. False for "not yet".
	bool (*SpawnHeliReplica)(RemoteHeli &heli) = nullptr;
	void (*DespawnHeliReplica)(RemoteHeli &heli) = nullptr;
	// Where it goes this frame. False when the engine no longer has it, and
	// the caller builds it again.
	bool (*PoseHeliReplica)(RemoteHeli &heli, const VehicleTransform &at,
	                        const HeliLook &look) = nullptr;
	// The first half of the owner's explosion: tail and back rotor off.
	void (*BlowTailOffHeliReplica)(RemoteHeli &heli) = nullptr;
	// The second half, at `pos`. The row may have no replica.
	void (*ExplodeHeliReplica)(RemoteHeli &heli, const Vec3 &pos) = nullptr;

	// ---- the helicopter's gun ----------------------------------------------
	// The rounds our own helicopters fired since the last call, oldest first.
	uint8_t (*DrainOwnHeliShots)(OwnHeliShot *out, uint8_t max) = nullptr;
	// Draw and sound one of the owner's rounds on the replica. Must not deal
	// damage: the owner's engine already did. False when the replica is gone.
	bool (*DrawHeliShot)(RemoteHeli &heli, const Vec3 &source,
	                     const Vec3 &target) = nullptr;
};

// Should a hit on this row go to its owner?
inline bool HeliHitIsWorthSending(const RemoteHeli &heli, uint8_t localPlayerId) {
	if (!heli.active || !heli.Spawned() || heli.orphaned)
		return false;
	if (heli.owner == INVALID_PLAYER || heli.owner == localPlayerId)
		return false;
	return true;
}

// Where an abandoned replica is, `elapsedMs` after it was abandoned at `from`.
inline VehicleTransform OrphanedHeliAt(const VehicleTransform &from,
                                       uint32_t elapsedMs) {
	VehicleTransform at = from;
	at.pos.z += HELI_ORPHAN_CLIMB_MPS * (static_cast<float>(elapsedMs) / 1000.0f);
	return at;
}

// Has an abandoned replica gone far enough to take away?
inline bool OrphanedHeliIsGone(const VehicleTransform &now, uint32_t elapsedMs) {
	return now.pos.z > HELI_GONE_HEIGHT || elapsedMs >= HELI_ORPHAN_MAX_MS;
}

class HeliSync {
public:
	using SendFn = void (*)(void *ctx, const void *bytes, size_t len, Channel ch);

	void Bind(const HeliBridge *bridge, SendFn send, void *ctx) {
		m_bridge  = bridge;
		m_send    = send;
		m_sendCtx = ctx;
	}

	// ---- inbound ------------------------------------------------------------
	void OnState(const S_HeliState &pkt, uint8_t localPlayerId, uint32_t nowMs);
	void OnGone(const S_HeliGone &pkt, uint8_t localPlayerId);
	void OnHit(const S_HeliHit &pkt, uint8_t localPlayerId);
	// One round somebody else's helicopter fired. Held until the replica
	// catches up with it, then drawn from Tick; dropped when there is no
	// replica to draw it from.
	void OnShot(const S_HeliShot &pkt, uint8_t localPlayerId, uint32_t nowMs);
	void OnOwnerLeft(uint8_t owner, uint32_t nowMs);

	// ---- per frame ----------------------------------------------------------
	// Before the world updates: build, place, abandon and remove replicas.
	void Tick(uint32_t nowMs);
	// After it: our own helicopters out, our hits on theirs out.
	void Send(uint8_t localPlayerId, uint32_t nowMs);

	// The session is gone. Every replica is destroyed and our own helicopters
	// start again under new serials in the next one.
	void Clear();

	// Whether a shoot-down we are credited with pays us the $250. Off, which
	// is the money rule's default, it pays nobody.
	void SetShootDownPaid(bool paid) { m_payShootDown = paid; }

	bool IsReplica(int32_t poolHandle) const;

	// ---- for the tests ------------------------------------------------------
	const RemoteHeli *Find(uint8_t owner, uint16_t serial) const;
	size_t ActiveCount() const;
	size_t PendingShotCount() const;
	uint16_t OwnSerial(uint8_t slot) const {
		return slot < HELI_POLICE_SLOTS && m_own[slot].active ? m_own[slot].serial : 0;
	}

private:
	struct OwnHeli {
		bool     active = false;
		int32_t  handle = -1;
		uint16_t serial = 0;
		Vec3     lastPos{};
	};

	struct GoneRing {
		uint16_t serial[HELI_GONE_RING] = {};
		uint8_t  count = 0;
		uint8_t  next  = 0;
		bool Has(uint16_t s) const {
			for (uint8_t i = 0; i < count; ++i)
				if (serial[i] == s)
					return true;
			return false;
		}
		void Add(uint16_t s) {
			serial[next] = s;
			next = static_cast<uint8_t>((next + 1) % HELI_GONE_RING);
			if (count < HELI_GONE_RING)
				++count;
		}
	};

	struct PendingShot {
		bool     used   = false;
		uint8_t  owner  = INVALID_PLAYER;
		uint16_t serial = 0;
		uint32_t dueMs  = 0;
		Vec3     source{};
		Vec3     target{};
	};

	RemoteHeli *FindRow(uint8_t owner, uint16_t serial);
	void Remove(RemoteHeli &heli);
	void DrawDueShots(uint32_t nowMs);
	void SendOwnShots(uint32_t nowMs);
	void Orphan(RemoteHeli &heli, uint32_t nowMs);
	void SendGone(const OwnHeli &own, uint8_t slot, uint8_t reason,
	              uint8_t credit, const Vec3 &pos, uint32_t nowMs);
	uint16_t NextSerial();

	template <class T>
	void Out(const T &pkt, Channel ch) {
		if (m_send)
			m_send(m_sendCtx, &pkt, sizeof(T), ch);
	}

	const HeliBridge *m_bridge  = nullptr;
	SendFn            m_send    = nullptr;
	void             *m_sendCtx = nullptr;

	RemoteHeli  m_rows[MAX_REMOTE_HELIS];
	GoneRing    m_gone[MAX_PLAYERS];
	PendingShot m_shots[MAX_PENDING_HELI_SHOTS];

	OwnHeli     m_own[HELI_POLICE_SLOTS];
	uint16_t    m_nextSerial = 1;
	bool        m_payShootDown = false;
	RateLimiter m_stateRate{HELI_STATE_HZ};

	bool m_saidReplica  = false;
	bool m_saidFull     = false;
	bool m_saidHitSent  = false;
	bool m_saidHitTaken = false;
	bool m_saidOrphan   = false;
	bool m_saidStream   = false;

	bool m_saidShotSent      = false;
	bool m_saidShotDrawn     = false;
	bool m_saidShotNoReplica = false;
	bool m_saidShotsFull     = false;
};

} // namespace coopiii
