#include "garage.h"

#include "addresses.h"
#include "../hook/hook.h"
#include "../log.h"

namespace coopiii::game {

namespace {

Detour g_garageUpdate;

// The union of what every other machine reports, one bit per garage. Written
// once a frame by ApplyRemoteGaragesImpl on the game thread and read by the
// detour on the same thread, so there is nothing to synchronise.
uint32_t g_heldMask = 0;

// What this machine's own state machine says, which is what goes on the wire.
uint32_t g_localMask = 0;

// Garages whose state byte was CoopIII's rather than the engine's on the
// previous frame - because CoopIII was holding it, or because the release
// wrote it.
//
// This exists to keep g_localMask honest, and without it the feature
// oscillates. The local bit is read back out of m_eGarageState after the
// engine's own arm has run; on a held garage that byte is CoopIII's own
// writing, or the arm that would have changed it was suppressed, so reading
// it back has this machine report a deviation it invented. It then stops
// being held, the engine's completion arm fires on the wrong car, and the
// whole thing repeats.
uint32_t g_ownedLastFrame = 0;

// Garages this machine is currently holding *shut* on somebody else's behalf.
// Needed on release: a serviced garage that has arrived at GS_FULLYCLOSED has
// its m_nTimeToStartAction already in the past, so the moment the hold comes
// off, the local engine's own arm would fire and respray the local player's
// car. Releasing therefore means winding the door back up, not just letting
// go. See ReleaseHold.
uint32_t g_holdingShut = 0;

// Resprays this machine's own engine has completed and not yet handed over.
// A ring rather than a queue: a player cannot be in two spray shops at once,
// and four is already three more than any frame can produce.
struct PendingRespray {
	uint8_t garage  = 0;
	uint8_t colour1 = 0;
	uint8_t colour2 = 0;
};
constexpr uint8_t MAX_PENDING_RESPRAYS = 4;
PendingRespray g_resprays[MAX_PENDING_RESPRAYS];
uint8_t        g_resprayCount = 0;

bool g_saidHold    = false;
bool g_saidStuck   = false;
bool g_saidRespray = false;
bool g_saidApplied = false;

// Which garage this is, or NUM_GARAGES for "not one of the 32". The exactness
// check matters: the detour is on CGarage::Update and nothing stops a future
// build of the engine calling it on something else, and a bad index here would
// be a bit written into the wrong garage's mask rather than a crash, which is
// the kind of wrong that takes a session to notice.
size_t GarageIndex(const void *self) {
	const uintptr_t base = CGarages__aGarages;
	const uintptr_t at   = reinterpret_cast<uintptr_t>(self);
	if (at < base)
		return NUM_GARAGES;
	const uintptr_t delta = at - base;
	if (delta % SIZEOF_GARAGE != 0)
		return NUM_GARAGES;
	const size_t index = delta / SIZEOF_GARAGE;
	return index < NUM_GARAGES ? index : NUM_GARAGES;
}

bool TestBit(uint32_t mask, size_t index) {
	return (mask & (1u << index)) != 0;
}

void WriteBit(uint32_t &mask, size_t index, bool on) {
	const uint32_t bit = 1u << index;
	mask = on ? (mask | bit) : (mask & ~bit);
}

void OpenGarage(void *garage) {
	Func<void(__thiscall *)(void *)>(CGarage__OpenThisGarage)(garage);
}

void CloseGarage(void *garage) {
	Func<void(__thiscall *)(void *)>(CGarage__CloseThisGarage)(garage);
}

void *PlayerVehicle() {
	return Func<void *(__cdecl *)()>(FindPlayerVehicle)();
}

void PushRespray(uint8_t garage, uint8_t colour1, uint8_t colour2) {
	if (g_resprayCount >= MAX_PENDING_RESPRAYS)
		return;
	g_resprays[g_resprayCount++] = PendingRespray{garage, colour1, colour2};
}

// The local engine's Pay'n'Spray just finished. The edge is GS_FULLYCLOSED ->
// GS_OPENING on a GARAGE_RESPRAY, and it is the right edge for one reason
// worth writing down: by the time the state byte has been written, the arm has
// already fixed the car, chosen the colours and put them on it (addresses.h,
// "the Pay'n'Spray, which is three things and not one"). So the colours are
// read back off the car the owner's engine actually painted, rather than
// CoopIII rolling its own - which is the whole point, since
// ChooseVehicleColour is a per-machine round robin and two machines calling it
// produce two different cars.
void NoteLocalRespray(size_t index) {
	void *const car = PlayerVehicle();
	if (!car) {
		// The respray happened with nobody in a car - the retail arm allows
		// it, the player having got out while the doors were shut. There is
		// nothing to repaint anywhere, but the door and the sound are still
		// worth replaying, so the report goes out with the colours the
		// receiver will ignore.
		PushRespray(static_cast<uint8_t>(index), 0, 0);
		return;
	}
	PushRespray(static_cast<uint8_t>(index),
	            Field<uint8_t>(car, offs::VEH_COLOUR1),
	            Field<uint8_t>(car, offs::VEH_COLOUR2));
	if (!g_saidRespray) {
		g_saidRespray = true;
		Log("garage: our first respray, garage %u, colours %u/%u; telling the "
		    "session",
		    static_cast<unsigned>(index),
		    static_cast<unsigned>(Field<uint8_t>(car, offs::VEH_COLOUR1)),
		    static_cast<unsigned>(Field<uint8_t>(car, offs::VEH_COLOUR2)));
	}
}

// Let go of a garage this machine was holding shut for somebody else.
//
// Not a no-op, and the reason is the trap this whole feature is built around.
// A serviced garage sitting at GS_FULLYCLOSED has m_nTimeToStartAction set to
// a moment that is now in the past - the CLOSING arm set it on the way down -
// so the very next unsuppressed frame would run the completion arm and hand
// the local player a free respray, a free repair and a cleared wanted level,
// because somebody else visited a spray shop across the city. Winding the door
// back up through the engine's own OpenThisGarage is the observer's copy of
// "the visit ended", and it leaves GS_OPENING, whose arm is pure animation.
void ReleaseHold(void *garage, size_t index) {
	if (!TestBit(g_holdingShut, index))
		return;
	WriteBit(g_holdingShut, index, false);
	const uint8_t state = Field<uint8_t>(garage, offs::GARAGE_STATE);
	if (state == GS_FULLYCLOSED || state == GS_CLOSING ||
	    state == GS_CLOSEDCONTAINSCAR) {
		OpenGarage(garage);
		WriteBit(g_ownedLastFrame, index, true);
	}
}

void __fastcall HookedGarageUpdate(void *self, void * /*edx*/) {
	const size_t index = GarageIndex(self);
	if (index >= NUM_GARAGES) {
		g_garageUpdate.Original<void(__thiscall *)(void *)>()(self);
		return;
	}

	const uint8_t type = Field<uint8_t>(self, offs::GARAGE_TYPE);

	// "Somebody else has this garage away from rest, and we are not using it
	// ourselves." The second half is not tidiness: two players in the same
	// garage must both get the engine's own behaviour, because whatever they
	// are each doing in there is real on their own machine. The engine's own
	// IsAnyOtherCarTouchingGarage normally stops that happening at all - a
	// remote player's car is a real car in this world and it is touching the
	// garage - but a race where both drive in on the same frame must not end
	// with one of them locked in.
	const bool held =
	    TestBit(g_heldMask, index) && !TestBit(g_localMask, index);

	const uint8_t before = Field<uint8_t>(self, offs::GARAGE_STATE);
	const bool    mayRun = GarageUpdateMayRun(type, before, held);
	if (mayRun)
		g_garageUpdate.Original<void(__thiscall *)(void *)>()(self);

	const uint8_t after = Field<uint8_t>(self, offs::GARAGE_STATE);

	// Our own Pay'n'Spray completing. Checked before the hold below, because
	// the hold can write the state byte and this edge is about what the
	// engine did with it.
	if (type == GARAGE_RESPRAY && before == GS_FULLYCLOSED &&
	    after == GS_OPENING && mayRun)
		NoteLocalRespray(index);

	// What this machine's own state machine says, and the one line that keeps
	// it honest.
	//
	// The bit is read back out of m_eGarageState, which is only this
	// machine's own opinion while CoopIII is not the one deciding it. On a
	// held garage it is not: either CoopIII wrote the byte, or it suppressed
	// the arm that would have moved it. Reading it back there would have this
	// machine report a deviation it invented - and two machines doing that to
	// each other hold a door in a state neither of them ever decided and
	// neither can release.
	//
	// So a held garage is not sampled at all, and neither is the frame after
	// one, because the release itself writes the byte. What that costs is
	// real and small: while somebody else is holding a garage open, the local
	// player walking into it is not reported to anybody until the hold comes
	// off, which is one round trip. Their own door is already open and stays
	// open, because the engine's own arm is still running.
	const bool owned = held || TestBit(g_ownedLastFrame, index);
	if (!owned)
		WriteBit(g_localMask, index, GarageDeviates(type, after));

	// True for the next frame if we are holding it now; the two writes below
	// set it for the frames where CoopIII moves the byte without holding.
	WriteBit(g_ownedLastFrame, index, held);

	if (!held) {
		ReleaseHold(self, index);
		return;
	}

	// Which way a held garage has to go, and whether its door has got there.
	// A garage that rests open is held shut; one that rests shut is held open.
	const bool  holdOpen   = !GarageRestsOpen(type);
	const float doorPos    = Field<float>(self, offs::GARAGE_DOOR_POS);
	const float doorHeight = Field<float>(self, offs::GARAGE_DOOR_HEIGHT);
	const bool  doorAtEnd  = holdOpen ? (doorPos >= doorHeight) : (doorPos <= 0.0f);

	switch (DecideGarageHold(type, after, held, doorAtEnd)) {
	case GarageHold::Ramp:
		// Through the engine's own mutators, so the transition is one the
		// engine would have made and the ramp arm that follows is the
		// engine's own.
		if (holdOpen)
			OpenGarage(self);
		else
			CloseGarage(self);
		// ...except that neither of them accepts every state. OpenThisGarage
		// takes GS_FULLYCLOSED, GS_CLOSING and GS_CLOSEDCONTAINSCAR;
		// CloseThisGarage takes GS_OPENED and GS_OPENING. Both twenty-byte
		// functions are transcribed in addresses.h, and between them they
		// leave two states with no way out: GS_OPENEDCONTAINSCAR, which is
		// where a spray shop parks after refusing a stolen car, and
		// GS_AFTERDROPOFF. Handed one of those, the mutator returns having
		// done nothing and the hold would ask again every frame forever.
		//
		// So the answer is checked rather than assumed. This writes the
		// moving state the mutator would have written, which is the same
		// transition by a different route - not a state the engine never
		// makes.
		if (Field<uint8_t>(self, offs::GARAGE_STATE) == after) {
			Field<uint8_t>(self, offs::GARAGE_STATE) =
			    holdOpen ? GS_OPENING : GS_CLOSING;
			if (!g_saidStuck) {
				g_saidStuck = true;
				Log("garage: garage %u was in state %u, which the engine's own "
				    "open/close refuses; moved it by hand",
				    static_cast<unsigned>(index), static_cast<unsigned>(after));
			}
		}
		WriteBit(g_ownedLastFrame, index, true);
		break;
	case GarageHold::Latch:
		// The door is already at the end it needs to be at. Writing
		// GS_OPENING/GS_CLOSING here instead would send it through the
		// arrival every single frame, which means the garage-door sound at
		// 60 Hz for as long as somebody stands in their safehouse.
		Field<uint8_t>(self, offs::GARAGE_STATE) = GarageLatchState(type);
		WriteBit(g_ownedLastFrame, index, true);
		break;
	case GarageHold::None:
		break;
	}

	if (!holdOpen)
		WriteBit(g_holdingShut, index, true);

	if (!g_saidHold) {
		g_saidHold = true;
		Log("garage: holding garage %u (type %u) %s for another player",
		    static_cast<unsigned>(index), static_cast<unsigned>(type),
		    holdOpen ? "open" : "shut");
	}
}

// ---- the bridge -----------------------------------------------------------

bool SampleLocalGaragesImpl(uint32_t &mask) {
	// No separate sweep over aGarages. The detour has already read every
	// garage the engine updated this frame, and it read them at the one
	// moment the state byte is the engine's own opinion rather than partly
	// CoopIII's. A sweep from PreFrame would be sampling the byte after the
	// hold had written it.
	//
	// The consequence is that this returns false until the detour has run at
	// least once, which is exactly "there is no world yet": CGarages::Update
	// is not called in the frontend or on a loading screen.
	if (!g_garageUpdate.IsInstalled())
		return false;
	mask = g_localMask;
	return true;
}

void ApplyRemoteGaragesImpl(uint32_t mask) {
	g_heldMask = mask;
}

uint8_t DrainLocalRespraysImpl(LocalRespray *out, uint8_t max) {
	uint8_t written = 0;
	for (uint8_t i = 0; i < g_resprayCount && written < max; ++i) {
		out[written].garage  = g_resprays[i].garage;
		out[written].colour1 = g_resprays[i].colour1;
		out[written].colour2 = g_resprays[i].colour2;
		++written;
	}
	g_resprayCount = 0;
	return written;
}

void ApplyRemoteResprayImpl(RemoteVehicle *vehicle, const ResprayBody &body) {
	// SEAM (wanted level). Nothing here clears anybody's stars, on purpose.
	//
	// The wanted level travels on its own, in PlayerFlags since version 18
	// (docs/wanted.md), and this packet has no field for it. The respray
	// already clears the stars of the player who paid for it, on their own
	// machine, through the engine's own CWanted::Reset inside the arm that
	// produced this packet - CoopIII does nothing to make that happen and
	// must do nothing to undo it. What an observer must not do is clear its
	// *own* player's stars because somebody else bought a paint job, and it
	// does not, because HookedGarageUpdate never lets the arm that would run
	// here.
	//
	// The `shared` wanted rule did land, and this place did not have to
	// change: the payer's own engine clears the payer's level, and
	// game/wanted.h (PlanWanted) decides what that does to the session's
	// floor. Never a second CWanted::Reset of CoopIII's own here.

	if (!vehicle || vehicle->poolHandle < 0)
		return;
	void *const car =
	    Func<void *(__cdecl *)(int32_t)>(CPools__GetVehicle)(vehicle->poolHandle);
	if (!car)
		return;

	// The engine's own three writes, in the engine's own order, straight out
	// of the arm at 0x00422767. Fix() is a CAutomobile method and the retail
	// arm reaches it without an IsCar() check - that is a retail bug that only
	// fires when a player parks a boat in a spray shop, and CoopIII is not
	// going to reproduce it on somebody else's boat.
	if (Field<int32_t>(car, offs::VEH_TYPE) != VEHICLE_TYPE_CAR)
		return;

	Field<float>(car, offs::VEH_HEALTH)            = 1000.0f;
	Field<float>(car, offs::AUTO_FIRE_BLOWUP_TIMER) = 0.0f;
	Func<void(__thiscall *)(void *)>(CAutomobile__Fix)(car);

	// And the paint, which is the whole reason this packet carries anything.
	// Written, never chosen: an observer calling ChooseVehicleColour would
	// walk its own m_lastColorVariation cursor and produce a different car.
	Field<uint8_t>(car, offs::VEH_COLOUR1) = body.colour1;
	Field<uint8_t>(car, offs::VEH_COLOUR2) = body.colour2;
	vehicle->colour1 = body.colour1;
	vehicle->colour2 = body.colour2;

	if (!g_saidApplied) {
		g_saidApplied = true;
		Log("garage: took a respray off the wire for net %u, colours %u/%u",
		    static_cast<unsigned>(body.vehicleNetId),
		    static_cast<unsigned>(body.colour1),
		    static_cast<unsigned>(body.colour2));
	}
}

} // namespace

// ---- install --------------------------------------------------------------

bool InstallGarageHook() {
	g_heldMask        = 0;
	g_localMask       = 0;
	g_ownedLastFrame = 0;
	g_holdingShut     = 0;
	g_resprayCount    = 0;

	const bool ok = g_garageUpdate.Install(
	    "CGarage::Update", reinterpret_cast<void *>(CGarage__Update),
	    reinterpret_cast<void *>(&HookedGarageUpdate));
	if (ok)
		Log("garage: hooked CGarage::Update at 0x%08X; %u garages, stride %u",
		    static_cast<unsigned>(CGarage__Update),
		    static_cast<unsigned>(NUM_GARAGES),
		    static_cast<unsigned>(SIZEOF_GARAGE));
	else
		Log("garage: FAILED to hook CGarage::Update at 0x%08X - doors, garages "
		    "and the Pay'n'Spray stay local to each machine",
		    static_cast<unsigned>(CGarage__Update));
	return ok;
}

void AddGaragesToBridge(WorldBridge &bridge) {
	if (!g_garageUpdate.IsInstalled())
		return;
	bridge.SampleLocalGarages  = &SampleLocalGaragesImpl;
	bridge.ApplyRemoteGarages  = &ApplyRemoteGaragesImpl;
	bridge.DrainLocalResprays  = &DrainLocalRespraysImpl;
	bridge.ApplyRemoteRespray  = &ApplyRemoteResprayImpl;
}

} // namespace coopiii::game
