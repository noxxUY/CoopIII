#include "passexit.h"

#include "addresses.h"
#include "leadcheck.h"
#include "ped.h"
#include "pause.h"
#include "seat.h"
#include "../log.h"

#include <windows.h>

#include <cmath>
#include <cstring>

namespace coopiii::game {

namespace {

using ExitFn = void(__thiscall *)(void *, void *, uint32_t);
using RoomFn = bool(__thiscall *)(void *, uint32_t, void *);

// The room test's six bytes as retail has them: `call [edi+80h]`.
constexpr uint8_t ROOM_CALL_BYTES[SET_EXIT_CAR_ROOM_CALL_LEN] = {0xFF, 0x97, 0x80, 0x00, 0x00, 0x00};

bool g_exitCall = false;   // 0x004DA157 comes to ExitCarFromObjective
bool g_roomCall = false;   // 0x004E1256 comes to RoomToLeave

// The door SetExitCar is letting a player out by, for the length of the call.
uint16_t g_heldDoor = 0;

int  g_saidOverruled = 0;
int  g_saidNoSeat    = 0;
int  g_saidLeft      = 0;
bool g_saidBus       = false;

constexpr int SAID_TIMES = 4;

void *PlayerPed() { return Func<void *(__cdecl *)()>(FindPlayerPed)(); }

bool WriteCode(uintptr_t at, const uint8_t *bytes, size_t len) {
	DWORD old = 0;
	if (!VirtualProtect(reinterpret_cast<void *>(at), len, PAGE_EXECUTE_READWRITE, &old))
		return false;
	std::memcpy(reinterpret_cast<void *>(at), bytes, len);
	VirtualProtect(reinterpret_cast<void *>(at), len, old, &old);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(at), len);
	return true;
}

// Points the `call` at `site` at `to`, only while it still calls `from`.
bool RedirectCall(uintptr_t site, uintptr_t from, uintptr_t to) {
	if (!RelCallAt(Ptr<uint8_t>(site), site, from))
		return false;
	uint8_t code[5] = {0xE8};
	const int32_t rel = static_cast<int32_t>(to - (site + 5));
	std::memcpy(code + 1, &rel, sizeof rel);
	return WriteCode(site, code, sizeof code);
}

// Where `ped` sits in `car`, the way SetExitCar looks: the wheel, then the
// passenger slots in order, then none.
int SeatOf(void *car, void *ped) {
	if (Field<void *>(car, offs::VEH_DRIVER) == ped)
		return EXIT_SEAT_DRIVER;
	const uint8_t max = Field<uint8_t>(car, offs::VEH_NUM_MAX_PASSENGERS);
	const uint8_t n   = max < offs::VEH_MAX_PASSENGERS ? max : offs::VEH_MAX_PASSENGERS;
	for (uint8_t i = 0; i < n; ++i)
		if (Field<void *>(car, offs::VEH_PASSENGERS + i * sizeof(void *)) == ped)
			return i;
	return EXIT_SEAT_NONE;
}

// In place of `call [edi+80h]` at 0x004E1256, `this` in ecx and the door and
// the offset where the engine pushed them: __fastcall with a spare edx, and
// callee-cleaned like the virtual's `ret 8`.
bool __fastcall RoomToLeave(void *car, void * /*edx*/, uint32_t door, void *offset) {
	const uintptr_t vt     = Field<uintptr_t>(car, offs::VTABLE);
	const RoomFn    room   = *reinterpret_cast<const RoomFn *>(vt + VEH_VT_IS_ROOM_FOR_PED_TO_LEAVE_CAR);
	const bool      engine = room(car, door, offset);
	const bool      answer = ExitRoomAnswer(engine, static_cast<uint16_t>(door), g_heldDoor);
	if (answer != engine && g_saidOverruled < SAID_TIMES) {
		++g_saidOverruled;
		Log("passexit: the engine found no room at door %u and would have sent a player riding "
		    "here round to the far side of the car (the driver's door, for the front seat); he "
		    "leaves by his own, and is stood clear of the car at the end if something is there",
		    static_cast<unsigned>(door));
	}
	return answer;
}

void OwnDoorExit(void *ped, void *car, const char *who) {
	const int  seat = SeatOf(car, ped);
	const bool bus  = (Field<uint8_t>(car, offs::VEH_FLAGS_B_BUS) & offs::VEH_IS_BUS) != 0;
	const uint16_t door =
	    Field<uintptr_t>(car, offs::VTABLE) == CAutomobile__vtable ? OwnExitDoor(seat, bus) : 0;

	if (seat == EXIT_SEAT_NONE && g_saidNoSeat < SAID_TIMES) {
		++g_saidNoSeat;
		Log("passexit: %s is getting out of a car he is in no seat of (driver %p); left "
		    "to itself the engine would use the driver's door, so he leaves by the front "
		    "passenger's, where he is drawn",
		    who, Field<void *>(car, offs::VEH_DRIVER));
	}
	if (bus && !g_saidBus) {
		g_saidBus = true;
		Log("passexit: %s is getting out of a bus; every seat of one leaves at the front, as "
		    "the engine has it (said once)",
		    who);
	}

	if (!door) {
		Func<ExitFn>(CPed__SetExitCar)(ped, car, 0);
		return;
	}

	g_heldDoor = door;
	Func<ExitFn>(CPed__SetExitCar)(ped, car, door);
	g_heldDoor = 0;

	if (g_saidLeft < SAID_TIMES && Field<uint32_t>(ped, offs::PED_STATE) == PEDSTATE_EXIT_CAR) {
		++g_saidLeft;
		Log("passexit: %s gets out of %s by door %u%s", who,
		    seat >= 0 ? "a passenger seat" : "no seat", static_cast<unsigned>(Field<uint16_t>(ped, offs::PED_VEH_DOOR)),
		    Field<void *>(car, offs::VEH_DRIVER) ? "" : ", nobody at the wheel");
	}
}

// In place of CPed::ProcessObjective's call to SetExitCar in its LEAVE_CAR
// arm, the only call to it in the image and where the exit key lands: the
// ped in ecx, the car and the door pushed. `ret 8`, as SetExitCar's.
void __fastcall ExitCarFromObjective(void *ped, void * /*edx*/, void *car, uint32_t door) {
	if (door == 0 && ped && car && ped == PlayerPed()) {
		OwnDoorExit(ped, car, "our player");
		return;
	}
	Func<ExitFn>(CPed__SetExitCar)(ped, car, door);
}

// In place of PedSetInCarCB's SetObjective(LEAVE_CAR, car) on a passenger of
// a new driver: the passenger in ecx, the objective and the car pushed,
// `ret 8` like SetObjective's.
bool g_leaveCall      = false;   // 0x004CF4ED comes to NewDriverTellsPassenger
int  g_saidStayedSeat = 0;

void __fastcall NewDriverTellsPassenger(void *ped, void * /*edx*/, uint32_t objective,
                                        void *car) {
	uint16_t   netId  = 0;
	const bool local  = ped != nullptr && ped == PlayerPed();
	const bool remote = ped != nullptr && !local && RemotePlayerForPed(ped, netId);
	if (!NewDriverSendsOut(local, remote)) {
		if (g_saidStayedSeat < SAID_TIMES) {
			++g_saidStayedSeat;
			Log("passexit: somebody got in at the wheel and the engine told %s, sitting beside "
			    "him, to get out; a player keeps his seat",
			    local ? "our player" : "another player's copy");
		}
		return;
	}
	Func<void(__thiscall *)(void *, uint32_t, void *)>(CPed__SetObjective)(ped, objective, car);
}

bool TakeRoomCall() {
	if (std::memcmp(Ptr<uint8_t>(SET_EXIT_CAR_ROOM_CALL), ROOM_CALL_BYTES, sizeof ROOM_CALL_BYTES) != 0)
		return false;
	uint8_t code[SET_EXIT_CAR_ROOM_CALL_LEN];
	code[0]           = 0xE8;
	const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&RoomToLeave) -
	                                         (SET_EXIT_CAR_ROOM_CALL + 5));
	std::memcpy(code + 1, &rel, sizeof rel);
	code[5] = 0x90;
	return WriteCode(SET_EXIT_CAR_ROOM_CALL, code, sizeof code);
}

bool GiveRoomCallBack() {
	if (!RelCallAt(Ptr<uint8_t>(SET_EXIT_CAR_ROOM_CALL), SET_EXIT_CAR_ROOM_CALL,
	               reinterpret_cast<uintptr_t>(&RoomToLeave)))
		return false;
	return WriteCode(SET_EXIT_CAR_ROOM_CALL, ROOM_CALL_BYTES, sizeof ROOM_CALL_BYTES);
}

} // namespace

bool InstallPassengerExit() {
	if (!g_exitCall)
		g_exitCall = RedirectCall(LEAVE_CAR_SET_EXIT_CAR_CALL, CPed__SetExitCar,
		                          reinterpret_cast<uintptr_t>(&ExitCarFromObjective));
	if (!g_roomCall)
		g_roomCall = TakeRoomCall();
	if (!g_leaveCall)
		g_leaveCall = RedirectCall(NEW_DRIVER_PASSENGER_LEAVE_CALL, CPed__SetObjective,
		                           reinterpret_cast<uintptr_t>(&NewDriverTellsPassenger));
	if (g_leaveCall)
		Log("passexit: a new driver's call at 0x%08X that sends the passengers out comes to "
		    "us; a player keeps his seat when another takes the wheel",
		    static_cast<unsigned>(NEW_DRIVER_PASSENGER_LEAVE_CALL));
	else
		Log("passexit: FAILED to take a new driver's call at 0x%08X; another player taking "
		    "the wheel still throws our player out of the seat beside him",
		    static_cast<unsigned>(NEW_DRIVER_PASSENGER_LEAVE_CALL));

	if (!g_exitCall)
		Log("passexit: FAILED to take the LEAVE_CAR arm's SetExitCar call at 0x%08X; our "
		    "player leaves a passenger seat by whichever door the engine picks",
		    static_cast<unsigned>(LEAVE_CAR_SET_EXIT_CAR_CALL));
	if (!g_roomCall)
		Log("passexit: FAILED to take SetExitCar's room test at 0x%08X; a player whose own "
		    "door the engine finds no room at still goes round to the far side",
		    static_cast<unsigned>(SET_EXIT_CAR_ROOM_CALL));
	if (g_exitCall && g_roomCall)
		Log("passexit: the exit key's SetExitCar call and its room test come to us at 0x%08X "
		    "and 0x%08X; a player riding as a passenger leaves by his own door",
		    static_cast<unsigned>(LEAVE_CAR_SET_EXIT_CAR_CALL),
		    static_cast<unsigned>(SET_EXIT_CAR_ROOM_CALL));
	return g_exitCall && g_roomCall && g_leaveCall;
}

void RemovePassengerExit() {
	if (g_leaveCall && RedirectCall(NEW_DRIVER_PASSENGER_LEAVE_CALL,
	                                reinterpret_cast<uintptr_t>(&NewDriverTellsPassenger),
	                                CPed__SetObjective))
		g_leaveCall = false;
	if (g_roomCall && GiveRoomCallBack())
		g_roomCall = false;
	if (g_exitCall && RedirectCall(LEAVE_CAR_SET_EXIT_CAR_CALL,
	                               reinterpret_cast<uintptr_t>(&ExitCarFromObjective),
	                               CPed__SetExitCar))
		g_exitCall = false;
}

namespace {

bool     g_exitKeyWasDown = false;
uint32_t g_exitPressedMs  = 0;
bool     g_exitPressed    = false;
int      g_saidLetOut     = 0;
uint32_t g_keyElsewhereMs = 0;

} // namespace

void TickPassengerExit(uint32_t nowMs) {
	void *const ped  = PlayerPed();
	const bool  down = Field<int16_t>(Ptr<void>(CPad__Pads), pad::NEWSTATE + pad::TRIANGLE * sizeof(int16_t)) != 0;
	const bool  edge = down && !g_exitKeyWasDown;
	g_exitKeyWasDown = down;

	// Not a key meant for the chat line or the menu, both of which take the
	// controls with our own bit (game/pause.h), nor one pressed in another
	// window: the enter key sends a chat line and picks a menu item too.
	const bool elsewhere =
	    (Global<uint8_t>(CPad__Pads + pad::DISABLE_PLAYER_CONTROLS) & pad::PLAYERCONTROL_COOPIII) != 0 ||
	    Global<uint8_t>(CMenuManager__m_bMenuActive) != 0 || !GameWindowInFront();
	if (elsewhere || nowMs - g_keyElsewhereMs < PASSENGER_EXIT_WAIT_MS) {
		if (elsewhere)
			g_keyElsewhereMs = nowMs;
		g_exitPressed = false;
		return;
	}

	void *const car    = ped && Field<bool>(ped, offs::PED_IN_VEHICLE) ? Field<void *>(ped, offs::PED_MY_VEHICLE)
	                                                                   : nullptr;
	const bool  riding = car && Field<void *>(car, offs::VEH_DRIVER) != ped;
	if (!riding) {
		g_exitPressed = false;
		return;
	}
	if (edge && !g_exitPressed) {
		g_exitPressed   = true;
		g_exitPressedMs = nowMs;
	}
	if (!g_exitPressed)
		return;

	const uint32_t state    = Field<uint32_t>(ped, offs::PED_STATE);
	const bool     exiting  = state == PEDSTATE_EXIT_CAR;
	const float   *v        = &Field<float>(car, offs::MOVE_SPEED);
	const float    speed    = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	// A bus lets its seats out one after another, 1.2 s apart (the LEAVE_CAR
	// objective's m_leaveCarTimer), so it is left to the engine.
	const bool     isCar    = Field<int32_t>(car, offs::VEH_TYPE) == VEHICLE_TYPE_CAR &&
	                   (Field<uint8_t>(car, offs::VEH_FLAGS_B_BUS) & offs::VEH_IS_BUS) == 0;
	const bool     cutscene = Global<uint8_t>(CCutsceneMgr__ms_running) != 0;
	if (exiting) {
		g_exitPressed = false;   // the engine took it
		return;
	}
	if (!PassengerLetOut(true, exiting, isCar, speed, cutscene, nowMs - g_exitPressedMs)) {
		// Too fast, a boat or a cutscene: the press is spent, as the engine
		// spends it.
		if (nowMs - g_exitPressedMs >= PASSENGER_EXIT_WAIT_MS)
			g_exitPressed = false;
		return;
	}
	g_exitPressed = false;
	const int32_t lock     = Field<int32_t>(car, offs::VEH_DOOR_LOCK);
	const uint8_t controls = Global<uint8_t>(CPad__Pads + pad::DISABLE_PLAYER_CONTROLS);
	if (UnseatLocalPlayer() && g_saidLetOut < SAID_TIMES) {
		++g_saidLetOut;
		Log("passexit: the exit key did not get our player out of the seat he rides in (door lock "
		    "%d, controls 0x%02X); he is let out beside the car",
		    static_cast<int>(lock), static_cast<unsigned>(controls));
	}
}

void ExitCarByOwnDoor(void *ped, void *car, bool player) {
	if (!ped || !car)
		return;
	if (player || ped == PlayerPed()) {
		OwnDoorExit(ped, car, player ? "another player's copy" : "our player");
		return;
	}
	Func<ExitFn>(CPed__SetExitCar)(ped, car, 0);
}

} // namespace coopiii::game
