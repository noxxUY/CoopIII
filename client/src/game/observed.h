// The table the two vehicle detours read to tell whose car a bare CVehicle* is.
//
// Split out of game/vehicle.cpp with no engine in it, so tools/clienttest can
// walk the ownership transitions over the real bookkeeping. The engine side
// supplies the one thing this cannot know - whether a pool handle still names
// a live car - as `resolve`, which is CPools::GetVehicle in the game and a
// fake pool in the test.
//
// A row holds a pool HANDLE, never a pointer, and every lookup resolves it.
// CCarCtrl::PossiblyRemoveVehicle takes a wreck out of the pool a minute later
// without asking anybody, and the slot is then free for the engine's own
// traffic - so a row holding a raw pointer would start matching an unrelated
// taxi. CPool::GetAt compares the whole flags byte including the slot's free
// bit, so a dead handle resolves to null and can never match anything.
#pragma once

#include <cstddef>
#include <cstdint>

namespace coopiii::game {

struct ObservedRow {
	int32_t  handle         = -1;
	uint16_t netId          = 0;
	// Another player the session says is at the wheel, 0xFF for nobody. Never
	// the local player: a car we drive is ours whatever the row says, and the
	// detours ask CVehicle::m_pDriver about that before they look here.
	uint8_t  driverPlayerId = 0xFF;
	// Another player the session has asked to settle the car (protocol.h,
	// S_VehicleCustody), 0xFF for nobody or for us. Only ever set while
	// driverPlayerId is 0xFF - the same precedence Session::MayReportVehicle
	// reads the pair with.
	//
	// Both are rewritten from the roster before every frame's physics, for
	// every row, including the cars this machine drives or settles and
	// therefore never corrects. They used to be refreshed only by the
	// correction pass, which left the row naming whoever held the car before
	// us for as long as we held it.
	uint8_t  custodianPlayerId = 0xFF;
	// We are settling it and haven't said we're finished (client.h,
	// TakeReportedVehicleHit). Everybody else sends their hits on it to us as
	// C_VehicleHit, so a replayed round of theirs must not take health off it
	// here as well. Never set beside a driver.
	bool     weSettle          = false;
	// What a blast left the car at here while nobody held it, and whether one
	// has (vehicle.h, HealthToWrite). Every machine replays the explosion at
	// the same place and takes the same off, so until somebody who holds the
	// car has reported it, that is its health. Written by the InflictDamage
	// detour and nothing else: the upside-down drain and a burning occupant's
	// write never go through it, happen on one machine only, and must not
	// stick.
	bool     blasted           = false;
	float    blastHealth       = 0.0f;
};

template <size_t N>
class ObservedTable {
public:
	ObservedRow *begin() { return m_rows; }
	ObservedRow *end() { return m_rows + N; }

	void Clear() {
		for (ObservedRow &r : m_rows)
			r = ObservedRow{};
	}

	template <class Resolve>
	ObservedRow *Find(const void *vehicle, Resolve resolve) {
		if (!vehicle)
			return nullptr;
		for (ObservedRow &r : m_rows) {
			if (r.handle < 0)
				continue;
			if (resolve(r.handle) == vehicle)
				return &r;
		}
		return nullptr;
	}

	// A fresh row with nobody at the wheel. False only when the table is full.
	//
	// One row per netId: any older row for it goes first, since a machine only
	// ever has one CVehicle under a netId and a second row could only be a
	// stale one. A row whose handle has stopped resolving is reused, so a car
	// the engine reaped without anybody noticing never costs a slot for good.
	template <class Resolve>
	bool Remember(int32_t handle, uint16_t netId, Resolve resolve) {
		if (handle < 0)
			return true;
		Forget(netId);
		for (ObservedRow &r : m_rows)
			if (r.handle < 0 || resolve(r.handle) == nullptr) {
				r = ObservedRow{handle, netId, 0xFF, 0xFF};
				return true;
			}
		return false;
	}

	void Forget(uint16_t netId) {
		for (ObservedRow &r : m_rows)
			if (r.handle >= 0 && r.netId == netId)
				r = ObservedRow{};
	}

	// Who else holds the car under `netId`. By netId rather than by pointer
	// because the caller is the roster, which has the number and not the
	// object. A custodian given alongside a driver is dropped: a car with a
	// driver is a car whose custody is over, whatever a stale field says.
	// `blastFloorEnds`: somebody holds it and their word on its health has
	// come in since, or it is us (client.h, BlastFloorEnds).
	void NoteHolders(uint16_t netId, uint8_t driverPlayerId,
	                 uint8_t custodianPlayerId, bool weSettle,
	                 bool blastFloorEnds = false) {
		for (ObservedRow &r : m_rows)
			if (r.handle >= 0 && r.netId == netId) {
				r.driverPlayerId    = driverPlayerId;
				r.custodianPlayerId = driverPlayerId == 0xFF ? custodianPlayerId
				                                             : uint8_t{0xFF};
				r.weSettle          = driverPlayerId == 0xFF &&
				                      r.custodianPlayerId == 0xFF && weSettle;
				if (blastFloorEnds)
					r.blasted = false;
			}
	}

	// A blast has just been let through on a car nobody holds; `health` is
	// what the engine left it at.
	template <class Resolve>
	void NoteBlast(const void *vehicle, float health, Resolve resolve) {
		if (ObservedRow *const r = Find(vehicle, resolve)) {
			if (!r->blasted || health < r->blastHealth)
				r->blastHealth = health;
			r->blasted = true;
		}
	}

	size_t Count() const {
		size_t n = 0;
		for (const ObservedRow &r : m_rows)
			if (r.handle >= 0)
				++n;
		return n;
	}

private:
	ObservedRow m_rows[N]{};
};

} // namespace coopiii::game
