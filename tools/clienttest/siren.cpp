// The siren gate and the traffic horn: the pure half of both, game/siren.h and
// the traffic section of game/horn.h. And the status a car copy is left in,
// game/carstatus.h, which both of them depend on.
//
// Neither engine half runs here. The siren is a detour on the audio and the
// traffic horn is a byte written after CGame::Process. What runs here is every
// decision in front of them: which models the audio holds back, which cars get
// past it, which flags are written on change, what the host puts on the wire
// and what a replica does with it.

#include "game/carstatus.h"
#include "game/horn.h"
#include "game/siren.h"

#include <coopiii/protocol.h>

#include <cstdio>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_sirenFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_sirenFailures;
}

// cAudioManager::UsesSiren's jump table at 0x00607494, one entry per audio
// index from 7 to 30, read out of the image: true where the entry is
// 0x0056C3DA, false where it is 0x0056C3E2.
constexpr bool USES_SIREN_TABLE[24] = {
    true,  false, false, false, false, false, false, false,   //  7..14
    false, true,  true,  false, false, false, false, false,   // 15..22
    false, false, false, true,  true,  false, false, true,    // 23..30
};

// cAudioManager::UsesSirenSwitching's, 0x006074F4, indices 16 to 30: true at
// 0x0056C40A, false at 0x0056C412.
constexpr bool SWITCHING_TABLE[15] = {
    true,  false, false, false, false, false, false, false,   // 16..23
    false, false, true,  true,  false, false, true,           // 24..30
};

void TestWhichModelsTheAudioGivesASiren() {
	std::printf("the siren: which models the audio holds back\n");

	bool agrees = true;
	for (int i = 0; i < 24; ++i) {
		const uint16_t model = static_cast<uint16_t>(AUDIO_VEHICLE_INDEX_BASE + 7 + i);
		agrees = agrees && AudioUsesSiren(model) == USES_SIREN_TABLE[i];
	}
	Check(agrees, "AudioUsesSiren matches the table at 0x00607494, entry for entry");

	bool outside = false;
	for (int m = 0; m < 400; ++m)
		if (m < AUDIO_VEHICLE_INDEX_BASE + 7 || m > AUDIO_VEHICLE_INDEX_BASE + 30)
			outside = outside || AudioUsesSiren(static_cast<uint16_t>(m));
	Check(!outside, "and nothing outside indices 7..30, which the `ja` sends to false");

	Check(AudioUsesSiren(MODEL_POLICE) && AudioUsesSiren(MODEL_ENFORCER) &&
	          AudioUsesSiren(MODEL_FBICAR) && AudioUsesSiren(MODEL_AMBULAN) &&
	          AudioUsesSiren(MODEL_FIRETRUK),
	      "police car, Enforcer, FBI car, ambulance and fire truck");
	Check(!AudioUsesSiren(MODEL_MRWHOOP),
	      "Mr Whoopee is not one: his jingle is the alarm path, never gated");
	Check(!AudioUsesSiren(90) && !AudioUsesSiren(110),
	      "nor a Landstalker or a taxi");

	bool switching = true;
	for (int i = 0; i < 15; ++i) {
		const uint16_t model = static_cast<uint16_t>(AUDIO_VEHICLE_INDEX_BASE + 16 + i);
		switching = switching && AudioSwitchesSirenForHorn(model) == SWITCHING_TABLE[i];
	}
	Check(switching, "AudioSwitchesSirenForHorn matches the table at 0x006074F4");
	Check(!AudioSwitchesSirenForHorn(MODEL_FIRETRUK) &&
	          !AudioSwitchesSirenForHorn(MODEL_FBICAR),
	      "the fire truck and the FBI car keep their horn with the siren on");
	Check(!AudioSwitchesSirenForHorn(MODEL_FIRETRUK - 1) &&
	          !AudioSwitchesSirenForHorn(MODEL_PREDATOR + 1),
	      "and nothing outside indices 16..30");
}

void TestWhoGetsPastTheGate() {
	std::printf("\nthe siren: which cars the audio is shown as not abandoned\n");
	constexpr uint8_t ABANDONED = ENTITY_STATUS_ABANDONED;

	Check(ReplicaSirenGateOpens(MODEL_POLICE, ABANDONED, true, true),
	      "a replica police car, siren on, somebody else driving: through");
	Check(ReplicaSirenGateOpens(MODEL_FIRETRUK, ABANDONED, true, true) &&
	          ReplicaSirenGateOpens(MODEL_AMBULAN, ABANDONED, true, true) &&
	          ReplicaSirenGateOpens(MODEL_FBICAR, ABANDONED, true, true) &&
	          ReplicaSirenGateOpens(MODEL_ENFORCER, ABANDONED, true, true),
	      "and the other four siren models");
	Check(!ReplicaSirenGateOpens(MODEL_POLICE, ABANDONED, true, false),
	      "nobody at the wheel: stays shut, as it does for the owner's own "
	      "parked car");
	Check(!ReplicaSirenGateOpens(MODEL_POLICE, ABANDONED, false, true),
	      "siren off: nothing to open it for");
	Check(!ReplicaSirenGateOpens(MODEL_POLICE, 0, true, true) &&
	          !ReplicaSirenGateOpens(MODEL_POLICE, ENTITY_STATUS_PHYSICS, true, true),
	      "a car that isn't status 4 was never held back");
	Check(!ReplicaSirenGateOpens(MODEL_POLICE, ENTITY_STATUS_WRECKED, true, true),
	      "a wreck isn't status 4 either, and stays silent");
	Check(!ReplicaSirenGateOpens(MODEL_MRWHOOP, ABANDONED, true, true) &&
	          !ReplicaSirenGateOpens(110, ABANDONED, true, true),
	      "a model the audio doesn't gate is left alone");
	Check(ENTITY_STATUS_PHYSICS != ENTITY_STATUS_ABANDONED &&
	          ENTITY_STATUS_PHYSICS != 0,
	      "what the detour writes is neither the gate's 4 nor PLAYER's 0");
}

void TestWhichFlagsAreWrittenOnChange() {
	std::printf("\nthe siren: how often ApplyRemoteVehicle writes it\n");
	const uint8_t base = VEH_ENGINE_ON;
	Check(VehicleFlagsWrittenOnChange(base | VEH_SIREN) !=
	          VehicleFlagsWrittenOnChange(base),
	      "the siren going on is a change");
	Check(VehicleFlagsWrittenOnChange(0) != VehicleFlagsWrittenOnChange(base),
	      "so is the engine");
	Check(VehicleFlagsWrittenOnChange(base | VEH_HORN) ==
	          VehicleFlagsWrittenOnChange(base),
	      "a honk is not: the horn is written every frame");
	Check(VehicleFlagsWrittenOnChange(base | VEH_LIGHTS) ==
	          VehicleFlagsWrittenOnChange(base),
	      "nor the headlights, for the same reason");
	Check(VehicleFlagsWrittenOnChange(0xFF) != 0xFF,
	      "a real flag set never equals the row's \"nothing applied yet\"");
}

void TestWhatTheHostPutsOnTheWire() {
	std::printf("\nthe traffic horn: what the host sends\n");
	Check(TrafficHornOnWire(110, false, 44) && TrafficHornOnWire(110, false, 1),
	      "a taxi with its timer running is honking, first frame to last");
	Check(!TrafficHornOnWire(110, false, 0), "no timer, no honk");
	Check(!TrafficHornOnWire(MODEL_POLICE, true, 30),
	      "a police car with its siren on is wailing faster, not honking");
	Check(TrafficHornOnWire(MODEL_POLICE, false, 30),
	      "the same car with the siren off is honking");
	Check(TrafficHornOnWire(MODEL_FIRETRUK, true, 30),
	      "the fire truck honks over its siren, so it goes out");
	Check(!TrafficHornOnWire(MODEL_MRWHOOP, false, 30), "Mr Whoopee never honks");
}

void TestTheHornMask() {
	std::printf("\nthe traffic horn: one bit per row\n");
	bool distinct = true;
	uint8_t all   = 0;
	for (uint8_t r = 0; r < MAX_CAR_STATES; ++r) {
		distinct = distinct && (all & CarStateHornBit(r)) == 0 && CarStateHornBit(r) != 0;
		all = static_cast<uint8_t>(all | CarStateHornBit(r));
	}
	Check(distinct && all == 0xFF, "eight rows, eight bits, the whole byte");
	Check(CarStateHornBit(MAX_CAR_STATES) == 0 && CarStateHornBit(255) == 0,
	      "a row past the batch has no bit");

	const uint8_t mask = CarStateHornBit(0) | CarStateHornBit(5);
	Check(CarStateHornSet(mask, 0) && CarStateHornSet(mask, 5) &&
	          !CarStateHornSet(mask, 1) && !CarStateHornSet(mask, 7),
	      "reading it back gives the rows that were set and no others");
	Check(!CarStateHornSet(0xFF, MAX_CAR_STATES), "and never a row past the batch");
}

void TestWhenAReplicaHonks() {
	std::printf("\nthe traffic horn: when a replica sounds it\n");
	const uint32_t at = 90000;
	Check(ReplicaTrafficHornSounds(true, false, at, at + 10), "a fresh row with the bit: honk");
	Check(!ReplicaTrafficHornSounds(false, false, at, at + 10), "without it: silent");
	Check(!ReplicaTrafficHornSounds(true, true, at, at + 10), "a wreck: silent");
	Check(!ReplicaTrafficHornSounds(true, false, 0, at), "no row ever: silent");
	Check(ReplicaTrafficHornSounds(true, false, at, at + 200),
	      "one lost batch at 10 Hz is still inside the window");
	Check(ReplicaTrafficHornSounds(true, false, at, at + HORN_FRESH_MS) &&
	          !ReplicaTrafficHornSounds(true, false, at, at + HORN_FRESH_MS + 1),
	      "and a car its host stopped streaming goes quiet at HORN_FRESH_MS");
	Check(ReplicaTrafficHornSounds(true, false, 0xFFFFFFF0u, 0x00000010u),
	      "across the clock's wrap");
}

void TestTheReplicaCountsItDown() {
	std::printf("\nthe traffic horn: the replica's own countdown\n");
	uint8_t left = 0;
	bool    runs = true;
	for (int f = 0; f < TRAFFIC_HORN_START; ++f)
		runs = runs && TrafficHornTimer(true, left) == TRAFFIC_HORN_START - f;
	Check(runs, "44, 43 ... 1, one a frame, the values the host's audio sees");
	Check(TrafficHornTimer(true, left) == TRAFFIC_HORN_START,
	      "and still honking after 1, it starts another honk at 44");
	Check(TrafficHornTimer(true, left) == TRAFFIC_HORN_START - 1, "which runs on");
	Check(TrafficHornTimer(false, left) == 0 && left == 0,
	      "the row lets go: nothing written, countdown dropped");
	Check(TrafficHornTimer(true, left) == TRAFFIC_HORN_START,
	      "and the next honk starts from the top");

	// The countdown doesn't line up with the host's. That is only fine
	// because each honk opens and closes on silence in every rhythm.
	bool quietStart = true, heard = true;
	for (int p = 0; p < HORN_PATTERNS; ++p) {
		quietStart = quietStart &&
		             !ReplicaHornAudible(static_cast<uint8_t>(p), TRAFFIC_HORN_START) &&
		             !ReplicaHornAudible(static_cast<uint8_t>(p), TRAFFIC_HORN_START - 1) &&
		             !ReplicaHornAudible(static_cast<uint8_t>(p), 1);
		bool any = false;
		for (int t = 1; t <= TRAFFIC_HORN_START; ++t)
			any = any || ReplicaHornAudible(static_cast<uint8_t>(p), static_cast<uint8_t>(t));
		heard = heard && any;
	}
	Check(quietStart, "every rhythm is silent on 44, 43 and 1, so a restart "
	                  "a few frames off the host's makes no extra blip");
	Check(heard, "and every rhythm sounds somewhere in 44..1");
}

void TestWhatASeatingLeavesTheCarIn() {
	std::printf("\nthe car's status: what a seating leaves it in\n");
	constexpr uint8_t PLAYER    = ENTITY_STATUS_PLAYER;
	constexpr uint8_t PHYSICS   = ENTITY_STATUS_PHYSICS;
	constexpr uint8_t ABANDONED = ENTITY_STATUS_ABANDONED;
	constexpr uint8_t WRECKED   = ENTITY_STATUS_WRECKED;

	Check(StatusAfterSeating(ABANDONED, true) == PHYSICS,
	      "a remote driver seated in an empty copy: PHYSICS, as the warp wrote");
	Check(StatusAfterSeating(ABANDONED, false) == ABANDONED,
	      "a passenger in an empty copy: still ABANDONED");
	Check(StatusAfterSeating(PLAYER, false) == PLAYER,
	      "a remote passenger beside the local driver: his car stays PLAYER");
	Check(StatusAfterSeating(PHYSICS, false) == PHYSICS,
	      "the local player as a passenger in a driven copy: stays PHYSICS, "
	      "not the PLAYER the warp gives him");
	Check(StatusAfterSeating(WRECKED, true) == WRECKED &&
	          StatusAfterSeating(WRECKED, false) == WRECKED,
	      "a wreck stays a wreck, either seat");
	Check(StatusAfterSeating(PHYSICS, true) == PHYSICS,
	      "a driver re-seated in a copy that was already driven: no change");

	constexpr uint8_t PHYSICS_BITS = PHYSICS << ENTITY_STATUS_SHIFT;
	Check(PHYSICS_BITS == 0x18,
	      "3 in bits 3-7 is the warp's `or al,18h` at 0x004D7E9B");
}

void TestWhoseCarTheAiDrives() {
	std::printf("\nthe car's status: whose car this machine's AI may steer\n");
	constexpr uint8_t PHYSICS = ENTITY_STATUS_PHYSICS;

	Check(!CarAiMayRun(PHYSICS, /*copy=*/true, /*localDrives=*/false),
	      "a copy somebody else drives, in PHYSICS: the AI keeps off it");
	Check(CarAiMayRun(PHYSICS, false, false),
	      "this machine's own traffic in PHYSICS: the AI drives it");
	Check(CarAiMayRun(PHYSICS, true, true),
	      "a copy the local player is at the wheel of: his, not a copy's");
	Check(CarAiMayRun(ENTITY_STATUS_ABANDONED, true, false) &&
	          CarAiMayRun(2, true, false) &&
	          CarAiMayRun(ENTITY_STATUS_PLAYER, true, false),
	      "any other status is left to its own arm, copy or not");
	Check(CarAiMayRun(2, false, false),
	      "cars on rails, which is nearly every car that comes through, go "
	      "straight through");
}

void TestTrafficSirenOnTheWire() {
	std::printf("\nthe traffic siren: the mask and what a replica makes of it\n");

	bool same = true;
	for (uint8_t r = 0; r <= MAX_CAR_STATES; ++r)
		same = same && CarStateSirenBit(r) == CarStateHornBit(r);
	Check(same, "the siren mask names rows exactly the way the horn mask does");
	const uint8_t mask = CarStateSirenBit(2) | CarStateSirenBit(7);
	Check(CarStateSirenSet(mask, 2) && CarStateSirenSet(mask, 7) &&
	          !CarStateSirenSet(mask, 0) && !CarStateSirenSet(0xFF, MAX_CAR_STATES),
	      "a set bit is its own row's and never one past the batch");
	Check(!CarStateSirenSet(0, 0),
	      "zero, what InitHeader leaves in an older build's byte, is siren off");

	Check(ReplicaTrafficSirenOn(true, false), "the host says on: on");
	Check(!ReplicaTrafficSirenOn(false, false), "the host says off: off");
	Check(!ReplicaTrafficSirenOn(true, true),
	      "a wreck never keeps one, whatever the last row said");

	constexpr uint8_t ABANDONED = ENTITY_STATUS_ABANDONED;
	Check(ReplicaSirenGateOpens(MODEL_POLICE, ABANDONED, true, /*driverSaid=*/true),
	      "a traffic police car whose driver the host names, not seated yet: heard");
	Check(!ReplicaSirenGateOpens(MODEL_POLICE, ABANDONED, true, /*driverSaid=*/false),
	      "one its crew got out of: lights only, as on the host");
	Check(!ReplicaSirenGateOpens(MODEL_AMBULAN, ENTITY_STATUS_PHYSICS, true, true),
	      "one with its driver seated is PHYSICS and needs no help");
}

} // namespace

int RunSirenTests() {
	std::printf("\n");
	TestWhichModelsTheAudioGivesASiren();
	TestWhoGetsPastTheGate();
	TestWhichFlagsAreWrittenOnChange();
	TestWhatTheHostPutsOnTheWire();
	TestTheHornMask();
	TestWhenAReplicaHonks();
	TestTheReplicaCountsItDown();
	TestTrafficSirenOnTheWire();
	TestWhatASeatingLeavesTheCarIn();
	TestWhoseCarTheAiDrives();
	return g_sirenFailures;
}
