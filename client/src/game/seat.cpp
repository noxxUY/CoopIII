#include "seat.h"

#include "addresses.h"
#include "carstatus.h"
#include "ped.h"
#include "pedanim.h"
#include "vehicle.h"
#include "../client.h"
#include "../log.h"

#include <windows.h>

#include <cmath>
#include <cstdio>

namespace coopiii::game {

namespace {

void *PlayerPed() {
	return Func<void *(__cdecl *)()>(FindPlayerPed)();
}

int g_seatKey = 'G';

// Edge state for the key. Held across calls, read only from the game thread.
bool g_keyWasDown = false;

bool g_saidSeated   = false;
bool g_saidNoSeat   = false;
bool g_saidNotReady = false;
bool g_saidBusy     = false;
bool g_saidNoCar    = false;
bool g_saidUnfocused = false;
bool g_saidUnseated  = false;

// How far to the side of the car to put somebody getting out. A GTA III
// car is about two metres across, so this clears the body and no more.
constexpr float PED_STEP_OUT_M = 2.0f;

// The car a pool ref names, or null. One argument, not two: CPools::GetVehicle
// reaches ms_pVehiclePool itself, the same way every CPools::GetPed call in
// this codebase does. Handing it the pool as well pushes the pointer where the
// handle belongs, and it reads an address as an index.
void *VehicleFromHandle(int32_t handle) {
	if (handle < 0)
		return nullptr;
	using GetFn = void *(__cdecl *)(int32_t);
	return Func<GetFn>(CPools__GetVehicle)(handle);
}

// Which passenger slot holds this ped, or -1. Seat numbering on the wire is
// the driver's seat as 0 and passenger slot n as n + 1, which is how
// SeatRemotePed reads it back on the other side.
int32_t PassengerSeatOf(void *vehicle, void *ped) {
	for (size_t i = 0; i < offs::VEH_MAX_PASSENGERS; ++i) {
		const size_t at = offs::VEH_PASSENGERS + i * sizeof(void *);
		if (Field<void *>(vehicle, at) == ped)
			return static_cast<int32_t>(i) + 1;
	}
	return -1;
}

// Is there room for one more passenger? Asked before warping rather than
// after, because WarpPedIntoCar's passenger arm walks the slots and simply
// does nothing when they are all taken - it still sets bInVehicle and the
// ped's state first, which would leave the player believing they are in a
// car that never gave them a seat. addresses.h, CPed__WarpPedIntoCar.
bool HasFreePassengerSeat(void *vehicle) {
	const uint8_t maxPassengers = Field<uint8_t>(vehicle, offs::VEH_NUM_MAX_PASSENGERS);
	const size_t  seats =
	    maxPassengers < offs::VEH_MAX_PASSENGERS ? maxPassengers : offs::VEH_MAX_PASSENGERS;
	for (size_t i = 0; i < seats; ++i) {
		const size_t at = offs::VEH_PASSENGERS + i * sizeof(void *);
		if (Field<void *>(vehicle, at) == nullptr)
			return true;
	}
	return false;
}

// A free passenger slot, as a wire seat number, or -1.
//
// WarpPedIntoCar picked this itself and never said which. The engine's
// animated entry cannot: SetEnterCar switches on the door it is given, so the
// seat has to be chosen before the walk starts rather than read back after.
//
// One on the player's own side of the car if there is one: client.h,
// PickPassengerSeat. The side is the player's position against the car's
// right row, the same row UnseatLocalPlayer steps out along.
int32_t ChooseFreePassengerSeat(void *vehicle, void *ped) {
	const uint8_t maxPassengers = Field<uint8_t>(vehicle, offs::VEH_NUM_MAX_PASSENGERS);
	const size_t  seats =
	    maxPassengers < offs::VEH_MAX_PASSENGERS ? maxPassengers : offs::VEH_MAX_PASSENGERS;
	uint16_t freeSeats = 0;
	for (size_t i = 0; i < seats; ++i) {
		const size_t at = offs::VEH_PASSENGERS + i * sizeof(void *);
		if (Field<void *>(vehicle, at) == nullptr)
			freeSeats = static_cast<uint16_t>(freeSeats | (1u << (i + 1)));
	}

	const float *const right = &Field<float>(vehicle, offs::MATRIX_RIGHT);
	const float *const car   = &Field<float>(vehicle, offs::POSITION);
	const float *const at    = &Field<float>(ped, offs::POSITION);
	const float across = (at[0] - car[0]) * right[0] + (at[1] - car[1]) * right[1] +
	                     (at[2] - car[2]) * right[2];
	return PickPassengerSeat(freeSeats, across > 0.0f);
}

// An entry the engine is walking. The car is held as a pool handle and not as
// a pointer: an entry takes a couple of seconds, and a car can be destroyed
// inside one.
int32_t    g_entryCarHandle = -1;
uint8_t    g_entrySeat      = 0;
uint32_t   g_entryStartMs   = 0;
uint32_t   g_entryDeadline  = 0;
EntryWatch g_entryWatch;

bool g_saidWalking  = false;
bool g_saidNoSlot   = false;

// The warp is meant to be the last resort, so each time it is taken instead
// of the door says why - a handful of times rather than once, because the
// first one is rarely the interesting one.
constexpr int SEAT_WARP_REASONS_SAID = 6;
int g_warpReasonsSaid = 0;
int g_doorEntriesSaid = 0;

// What the door the entry wanted looked like, for those lines. Everything
// BeginPedEnterCar tests, read off the car and the ped as they are.
void SayWhyNoDoor(const char *what, void *ped, void *vehicle, uint8_t seat) {
	if (g_warpReasonsSaid >= SEAT_WARP_REASONS_SAID)
		return;
	++g_warpReasonsSaid;

	const float *const cp = &Field<float>(vehicle, offs::POSITION);
	const float *const pp = &Field<float>(ped, offs::POSITION);
	const float *const cv = &Field<float>(vehicle, offs::MOVE_SPEED);
	const float dx = cp[0] - pp[0], dy = cp[1] - pp[1], dz = cp[2] - pp[2];
	const float dist  = std::sqrt(dx * dx + dy * dy + dz * dz);
	const float speed = std::sqrt(cv[0] * cv[0] + cv[1] * cv[1] + cv[2] * cv[2]);

	void *const anim = Field<void *>(ped, offs::PED_VEHICLE_ANIM);
	Log("seat: %s, seat %u - so the seat was taken directly. Ped state %u, "
	    "vehicle anim %d; car %.1f m away doing %.3f, getting in 0x%02X, out "
	    "0x%02X, being jacked %d, status %u",
	    what, seat, Field<uint32_t>(ped, offs::PED_STATE),
	    anim ? Field<int32_t>(anim, ANIM_ID) : -1, dist, speed,
	    Field<uint8_t>(vehicle, offs::VEH_GETTING_IN_FLAGS),
	    Field<uint8_t>(vehicle, offs::VEH_GETTING_OUT_FLAGS),
	    (Field<uint8_t>(vehicle, offs::VEH_FLAGS_C) & offs::VEH_IS_BEING_CARJACKED) ? 1 : 0,
	    static_cast<unsigned>(Field<uint8_t>(vehicle, offs::ENTITY_FLAGS) >> ENTITY_STATUS_SHIFT));
}

// The old way in, kept as the fallback for every case where the engine
// refuses to animate: a car on its roof, a car moving, a door that will not
// open. Getting in instantly is worse than getting in with the door open, and
// both are better than the seat key doing nothing.
int32_t WarpIntoSeat(void *ped, void *vehicle) {
	// The objective first, and it is not ceremony: WarpPedIntoCar reads it to
	// decide which seat to give. Anything that is not the driver's objective
	// takes the passenger arm, which walks to the first free slot itself.
	using ObjFn  = void(__thiscall *)(void *, uint32_t, void *);
	using WarpFn = void(__thiscall *)(void *, void *);
	Func<ObjFn>(CPed__SetObjective)(ped, OBJECTIVE_ENTER_CAR_AS_PASSENGER, vehicle);

	// The warp makes the car STATUS_PLAYER for the player whichever seat he
	// takes (0x004D7E83), and this is nearly always somebody else's car. In
	// that status ProcessControl reads this machine's horn key into it
	// (0x00534191) and runs DoDriveByShootings with its driver's weapon. The
	// door entry leaves a passenger's car alone, so the warp is made to as
	// well: whatever it was before, it is again (game/carstatus.h).
	uint8_t      &flags  = Field<uint8_t>(vehicle, offs::ENTITY_FLAGS);
	const uint8_t before = static_cast<uint8_t>(flags >> ENTITY_STATUS_SHIFT);
	Func<WarpFn>(CPed__WarpPedIntoCar)(ped, vehicle);
	flags = static_cast<uint8_t>(
	    (flags & 0x07u) |
	    (StatusAfterSeating(before, /*driverSeat=*/false) << ENTITY_STATUS_SHIFT));

	// Which seat it actually gave us. Read back rather than assumed, because
	// the engine chose it and the number is what the session has to be told.
	const int32_t seat = PassengerSeatOf(vehicle, ped);
	if (seat < 0) {
		// It said yes and then seated nobody. Undo what the warp set so the
		// player is not left believing they are in a car with no seat.
		Field<bool>(ped, offs::PED_IN_VEHICLE)   = false;
		Field<void *>(ped, offs::PED_MY_VEHICLE) = nullptr;
		Field<uint32_t>(ped, offs::PED_OBJECTIVE)      = OBJECTIVE_NONE;
		Field<uint32_t>(ped, offs::PED_PREV_OBJECTIVE) = OBJECTIVE_NONE;
		Field<void *>(ped, offs::PED_CAR_IN_OBJECTIVE) = nullptr;
		Log("seat: CPed::WarpPedIntoCar took the passenger arm and left us in no "
		    "slot at all; put the player back on the pavement");
		return SEAT_LOCAL_REFUSED;
	}

	// The objective goes once the seat is taken, for the same reason
	// UnseatRemotePed clears it: a ped left holding ENTER_CAR_AS_PASSENGER
	// and a car pointer keeps trying to carry the objective out.
	Field<uint32_t>(ped, offs::PED_OBJECTIVE)      = OBJECTIVE_NONE;
	Field<uint32_t>(ped, offs::PED_PREV_OBJECTIVE) = OBJECTIVE_NONE;
	Field<void *>(ped, offs::PED_CAR_IN_OBJECTIVE) = nullptr;

	if (!g_saidSeated) {
		g_saidSeated = true;
		Log("seat: got into somebody else's car as a passenger, seat %d. The "
		    "driver owns the physics, so nothing about this car goes on the "
		    "wire from here",
		    seat);
	}
	return seat;
}

} // namespace

void SetSeatKey(int virtualKey) {
	if (virtualKey > 0 && virtualKey < 256)
		g_seatKey = virtualKey;
}

bool LocalWantsSeatToggle() {
	// GetAsyncKeyState rather than the game's own CPad, deliberately. GTA III
	// composes CPad from DirectInput device state, and this is a key the
	// original game has no binding for, so there is nothing in CPad to read.
	// Polled here rather than hooked: a key with no meaning to the engine
	// needs no hook, and CPad::UpdatePads is somebody else's seam.
	const bool down = (GetAsyncKeyState(g_seatKey) & 0x8000) != 0;
	const bool edge = down && !g_keyWasDown;
	g_keyWasDown    = down;

	// Only when the game has the keyboard. Alt-tabbed, the key belongs to
	// whatever window has focus, and a co-op mod that seats you in a car
	// because you typed a G in a chat window somewhere else is worse than one
	// with no passenger seat at all.
	//
	// Asked of the foreground window's process, not of GetActiveWindow. That
	// one is per thread and answers null whenever the tick is not on the
	// thread that owns the window the player is looking at - a windowed-mode
	// wrapper's window is the usual way to get there. The log had "foreground
	// 000206D6, active 00000000" in both games, and null says nothing about
	// whose window that was. Two copies of the game on one desktop are two
	// processes, so the one the key was not meant for still says no.
	if (edge) {
		const HWND fg    = GetForegroundWindow();
		DWORD      owner = 0;
		if (fg)
			GetWindowThreadProcessId(fg, &owner);
		if (owner != GetCurrentProcessId()) {
			if (!g_saidUnfocused) {
				g_saidUnfocused = true;
				Log("seat: the seat key was pressed while another program had the "
				    "keyboard (foreground %p belongs to pid %lu, we are %lu), so it "
				    "was ignored",
				    static_cast<void *>(fg), static_cast<unsigned long>(owner),
				    static_cast<unsigned long>(GetCurrentProcessId()));
			}
			return false;
		}
	}
	return edge;
}

bool LocalIsPassenger() {
	void *const ped = PlayerPed();
	if (!ped || !Field<bool>(ped, offs::PED_IN_VEHICLE))
		return false;
	void *const car = Field<void *>(ped, offs::PED_MY_VEHICLE);
	if (!car)
		return false;
	return Field<void *>(car, offs::VEH_DRIVER) != ped;
}

int32_t SeatLocalPlayerIn(int32_t vehicleHandle, uint8_t *seatAsked) {
	void *const ped = PlayerPed();
	if (!ped)
		return -1;

	if (Field<bool>(ped, offs::PED_IN_VEHICLE)) {
		if (!g_saidBusy) {
			g_saidBusy = true;
			Log("seat: asked for a passenger seat while already in a car. The "
			    "way out is the game's own exit key, not this one");
		}
		return -1;
	}

	// A ped that is not standing around cannot be put in a car without
	// fighting whatever it is doing. The engine's own enter path refuses for
	// the same reason.
	const uint32_t state = Field<uint32_t>(ped, offs::PED_STATE);
	if (state == PEDSTATE_DIE || state == PEDSTATE_DEAD) {
		if (!g_saidNotReady) {
			g_saidNotReady = true;
			Log("seat: asked for a passenger seat while dying or dead, so no");
		}
		return -1;
	}

	void *const vehicle = VehicleFromHandle(vehicleHandle);
	if (!vehicle) {
		// The client picked a car out of its own roster and the pool will not
		// give it to us, which means the session and the engine disagree about
		// what exists. Worth a line: silent is how this whole class of bug
		// hides.
		if (!g_saidNoCar) {
			g_saidNoCar = true;
			Log("seat: the session named vehicle handle %d and the pool has "
			    "nothing there, so nobody got in",
			    vehicleHandle);
		}
		return -1;
	}

	if (!HasFreePassengerSeat(vehicle)) {
		if (!g_saidNoSeat) {
			g_saidNoSeat = true;
			Log("seat: the nearest car has no free passenger seat, so nobody got "
			    "in. It holds %u passengers",
			    Field<uint8_t>(vehicle, offs::VEH_NUM_MAX_PASSENGERS));
		}
		return -1;
	}

	// The animated way in: ask the engine to walk to the door and open it,
	// which is what the other machine has been drawing for a remote player
	// ever since entercar landed. This key used to call WarpPedIntoCar, and a
	// warp is a teleport on both screens at once.
	const int32_t want = ChooseFreePassengerSeat(vehicle, ped);
	if (want < 0) {
		if (!g_saidNoSlot) {
			g_saidNoSlot = true;
			Log("seat: the car said it had room and then named no free slot, so "
			    "nobody got in");
		}
		return SEAT_LOCAL_REFUSED;
	}

	if (StartCarEntry(ped, vehicle, static_cast<uint8_t>(want))) {
		const uint32_t now = GetTickCount();
		g_entryCarHandle = vehicleHandle;
		g_entrySeat      = static_cast<uint8_t>(want);
		g_entryStartMs   = now;
		g_entryDeadline  = now + SEAT_ANIM_TIMEOUT_MS;
		g_entryWatch.Begin(now, CarEntryMark(ped));
		if (seatAsked)
			*seatAsked = static_cast<uint8_t>(want);
		if (!g_saidWalking) {
			g_saidWalking = true;
			Log("seat: walking to the door to get in as a passenger, seat %d. "
			    "The session is told now rather than at the end, so the other "
			    "machines open the same door at the same time",
			    want);
		}
		return SEAT_LOCAL_WALKING;
	}

	// The engine refused to animate it: a car on its roof, a car moving, a
	// door that will not open. Fall back rather than refuse, because getting
	// in instantly is worse than getting in with the door open and better
	// than the key doing nothing.
	SayWhyNoDoor("the door would not open for us", ped, vehicle, static_cast<uint8_t>(want));
	return WarpIntoSeat(ped, vehicle);
}

int32_t PollLocalSeatEntry() {
	if (g_entryCarHandle < 0)
		return SEAT_LOCAL_REFUSED;

	void *const ped     = PlayerPed();
	void *const vehicle = VehicleFromHandle(g_entryCarHandle);
	if (!ped || !vehicle) {
		// One half of it went while the ped was still walking. An entry takes
		// seconds and a car can be destroyed inside one, which is the whole
		// reason the car is held as a handle here and not as a pointer.
		g_entryCarHandle = -1;
		if (ped)
			CancelCarEntry(ped);
		return SEAT_LOCAL_REFUSED;
	}

	const uint8_t progress = PollCarEntry(ped, vehicle, g_entrySeat);
	if (progress == SEAT_DONE) {
		g_entryCarHandle = -1;
		// Read the seat back instead of trusting the one that was asked for.
		// The engine is what assigned it, and the session has already been
		// told the slot we asked for - so this number is what decides whether
		// that has to be corrected.
		const int32_t seat = PassengerSeatOf(vehicle, ped);
		if (seat < 0)
			return SEAT_LOCAL_REFUSED;
		// A few of these, with the time, so a session log shows the door
		// working and not only the times it did not.
		if (g_doorEntriesSaid < SEAT_WARP_REASONS_SAID) {
			++g_doorEntriesSaid;
			Log("seat: opened the door and got into somebody else's car as a "
			    "passenger, seat %d, in %u ms. The driver owns the physics, so "
			    "nothing about this car goes on the wire from here",
			    seat, static_cast<unsigned>(GetTickCount() - g_entryStartMs));
		}
		return seat;
	}

	// Still going: wait for as long as the animation is moving, up to the cap.
	// The cap is a wall clock and the chain is not - it runs on CTimer, which
	// a window that has lost frames runs slow - so the stall test is what
	// normally ends a bad entry, and this is only what ends a strange one.
	const uint32_t now     = GetTickCount();
	const bool     running = progress == SEAT_RUNNING;
	const bool     stalled = running && g_entryWatch.Stalled(now, CarEntryMark(ped));
	const bool     late    = running && now >= g_entryDeadline;
	if (running && !stalled && !late)
		return SEAT_LOCAL_WALKING;

	// Refused, interrupted, stuck or out of time. Take the half-played entry
	// off the ped - that is what gives the door back to the car - and seat
	// them the old way, so the key still does something. Said before the
	// cancel, while the ped still shows where the entry had got to.
	char what[96];
	std::snprintf(what, sizeof what, "the door-opening entry %s after %u ms",
	              !running ? "was dropped by the engine"
	              : stalled ? "stopped moving"
	                        : "ran past the cap",
	              static_cast<unsigned>(now - g_entryStartMs));
	SayWhyNoDoor(what, ped, vehicle, g_entrySeat);

	g_entryCarHandle = -1;
	CancelCarEntry(ped);
	return WarpIntoSeat(ped, vehicle);
}


// Get the local player out of the car they are riding in.
//
// This was going to be the game's own exit key, on the grounds that the player
// is a real occupant of a real car and the engine is entitled to manage them.
// It is here anyway because that could not be shown to work: driven with the
// game's own enter/exit inputs, a warped-in passenger stayed put. Whether the
// engine refuses to let a passenger out or the synthetic input never reached
// the handler, the answer is the same - a seat you cannot leave is worse than
// no seat at all, so CoopIII provides the way out it can prove.
//
// The teardown is UnseatRemotePed's, field for field, because that one is
// already load-bearing for every remote player who ever gets out of a car.
// The difference is the last step: a remote ped is pulled out of the car by
// the next snapshot, and the local player has nothing pulling them, so they
// have to be put down beside it by hand.
bool UnseatLocalPlayer() {
	void *const ped = PlayerPed();
	if (!ped || !Field<bool>(ped, offs::PED_IN_VEHICLE))
		return false;

	void *const car = Field<void *>(ped, offs::PED_MY_VEHICLE);
	if (!car)
		return false;

	// Never take the wheel out from under ourselves: driving is the engine's
	// own business and its own key.
	if (Field<void *>(car, offs::VEH_DRIVER) == ped)
		return false;

	// Pressed halfway through the game's own exit, the door that exit claimed
	// would stay claimed. ped.h, ReleaseExitDoor.
	ReleaseExitDoor(ped, car);

	Func<void(__thiscall *)(void *, void *)>(CVehicle__RemovePassenger)(car, ped);

	Field<bool>(ped, offs::PED_IN_VEHICLE)     = false;
	Field<void *>(ped, offs::PED_MY_VEHICLE)   = nullptr;
	Field<uint32_t>(ped, offs::PED_STATE)      = PEDSTATE_IDLE;
	Field<uint32_t>(ped, offs::PED_LAST_STATE) = PEDSTATE_NONE;
	Field<uint8_t>(ped, offs::ENTITY_FLAGS_A) |= offs::ENTITY_USES_COLLISION;

	Field<uint32_t>(ped, offs::PED_OBJECTIVE)      = OBJECTIVE_NONE;
	Field<uint32_t>(ped, offs::PED_PREV_OBJECTIVE) = OBJECTIVE_NONE;
	Field<void *>(ped, offs::PED_CAR_IN_OBJECTIVE) = nullptr;

	float *const vel = &Field<float>(ped, offs::MOVE_SPEED);
	vel[0] = vel[1] = vel[2] = 0.0f;

	// Beside the car, not inside it. The right row of the car's matrix is a
	// unit vector across it, so a step along it clears the door; a metre up
	// as well, so the drop settles onto the pavement rather than through it.
	const float *const right = &Field<float>(car, offs::MATRIX_RIGHT);
	const float *const from  = &Field<float>(car, offs::POSITION);
	float *const       to    = &Field<float>(ped, offs::POSITION);
	to[0] = from[0] + right[0] * PED_STEP_OUT_M;
	to[1] = from[1] + right[1] * PED_STEP_OUT_M;
	to[2] = from[2] + right[2] * PED_STEP_OUT_M + 1.0f;

	if (!g_saidUnseated) {
		g_saidUnseated = true;
		Log("seat: got out of the car we were riding in and stood down beside it");
	}
	return true;
}

void AddSeatToBridge(WorldBridge &bridge) {
	bridge.LocalWantsSeatToggle = &LocalWantsSeatToggle;
	bridge.SeatLocalPlayerIn    = &SeatLocalPlayerIn;
	bridge.PollLocalSeatEntry   = &PollLocalSeatEntry;
	bridge.LocalIsPassenger     = &LocalIsPassenger;
	bridge.UnseatLocalPlayer    = &UnseatLocalPlayer;
}

} // namespace coopiii::game
