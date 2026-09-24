// A replica turned back into a ped or car of ours: client/src/game/adopt.h.
// The bytes the conversion writes, walked with no engine.

#include "game/adopt.h"

#include <cmath>
#include <cstdio>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_adoptFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_adoptFailures;
}

void TestWhatIsTakenAndWhatIsLetGo() {
	std::printf("\nwhich replicas can be taken over\n");
	Check(PlanPedAdoption(true, true, false, PEDTYPE_CIVMALE) == AdoptPlan::Convert &&
	          PlanPedAdoption(true, true, false, PEDTYPE_CIVFEMALE) == AdoptPlan::Convert,
	      "a civilian with a replica here");
	Check(PlanPedAdoption(false, false, false, PEDTYPE_CIVMALE) == AdoptPlan::Release,
	      "no row, nothing to take");
	Check(PlanPedAdoption(true, false, false, PEDTYPE_CIVMALE) == AdoptPlan::Release,
	      "a row whose replica has not been built");
	Check(PlanPedAdoption(true, true, true, PEDTYPE_CIVMALE) == AdoptPlan::Release,
	      "a corpse");
	Check(PlanPedAdoption(true, true, false, PEDTYPE_COP) == AdoptPlan::Release,
	      "a cop, whose replica is a civilian");
	Check(AMBIENT_PEDTYPE_CIVMALE == PEDTYPE_CIVMALE &&
	          AMBIENT_PEDTYPE_CIVFEMALE == PEDTYPE_CIVFEMALE,
	      "the wire's two civilian types are the engine's");

	Check(PlanCarAdoption(true, true, false, false) == AdoptPlan::Convert, "a car");
	Check(PlanCarAdoption(true, true, true, false) == AdoptPlan::Release, "not a wreck");
	Check(PlanCarAdoption(true, false, false, false) == AdoptPlan::Release,
	      "not one we never built");
	Check(PlanCarAdoption(true, true, false, true) == AdoptPlan::KeepForClaim,
	      "and one we are at the wheel of waits for its promotion");
	Check(PlanCarAdoption(true, true, true, true) == AdoptPlan::Release,
	      "unless it is a wreck anyway");
}

void TestAPedIsPutBack() {
	std::printf("\na pedestrian's bytes, back to a generated one's\n");
	AdoptPedBytes replica;
	replica.createdBy = CHAR_CREATED_BY_MISSION;
	replica.entityB   = static_cast<uint8_t>(offs::ENTITY_EXPLOSION_PROOF | 0x40);
	replica.entityC   = static_cast<uint8_t>(0xF0 | offs::ENTITY_BULLET_PROOF |
	                                         offs::ENTITY_FIRE_PROOF |
	                                         offs::ENTITY_COLLISION_PROOF |
	                                         offs::ENTITY_MELEE_PROOF);
	replica.pedC      = 0x41;   // bRespondsToThreats cleared, bit 6 set
	replica.pedG      = 0x80;   // bAllowMedics cleared, bFadeOut set
	replica.zone      = LEVEL_IGNORE;

	const AdoptPedBytes b = PedBytesAfterAdoption(replica);
	Check(b.createdBy == CHAR_CREATED_BY_RANDOM,
	      "RANDOM_CHAR, so CanBeDeleted lets the reaper have him");
	Check(b.entityB == 0x40 && b.entityC == 0xF0, "all five proofs off, nothing else in those bytes");
	Check(b.pedC == (0x41 | offs::PED_RESPONDS_TO_THREATS) &&
	          b.pedG == (0x80 | offs::PED_ALLOW_MEDICS),
	      "threats and medics back on, as CPed::CPed has them");
	Check(b.zone == LEVEL_GENERIC, "the generic level a generator's crowd has");

	Check(AdoptedPedWanders(false, PEDSTATE_IDLE), "on his feet, he walks");
	Check(!AdoptedPedWanders(true, PEDSTATE_DRIVING), "in a car, the car is his business");
	Check(!AdoptedPedWanders(false, PEDSTATE_DIE) && !AdoptedPedWanders(false, PEDSTATE_DEAD),
	      "dying, he finishes");
}

void TestHeWalksTheWayHeFaces() {
	std::printf("\nthe direction he wanders in\n");
	const float PI = 3.14159265f;
	Check(WanderDirForHeading(0.0f) == 0, "facing north (+y): 0");
	Check(WanderDirForHeading(-PI / 2) == 2, "facing east (+x), heading -90: 2");
	Check(WanderDirForHeading(PI / 2) == 6, "facing west: 6");
	Check(WanderDirForHeading(PI) == 4 && WanderDirForHeading(-PI) == 4, "facing south: 4");
	Check(WanderDirForHeading(-PI / 4) == 1 && WanderDirForHeading(PI / 4) == 7,
	      "and the diagonals between");
	bool bounded = true;
	for (float h = -20.0f; h <= 20.0f; h += 0.01f)
		bounded = bounded && WanderDirForHeading(h) < 8;
	Check(bounded, "never outside 0..7, whatever the heading");
	Check(WanderDirForHeading(std::nanf("")) == 0 && WanderDirForHeading(1.0e30f) == 0,
	      "and nonsense is north");
}

void TestACarIsPutBack() {
	std::printf("\na car's bytes, back to traffic\n");
	AdoptCarBytes replica;
	replica.status     = ENTITY_STATUS_ABANDONED;
	replica.flagsA     = static_cast<uint8_t>(offs::VEH_IS_LOCKED | offs::VEH_LIGHTS_ON);
	replica.flagsC     = static_cast<uint8_t>(offs::VEH_HAS_BEEN_OWNED_BY_PLAYER | 0x01);
	replica.entityC    = static_cast<uint8_t>(offs::ENTITY_COLLISION_PROOF | 0x80);
	replica.mission    = CAR_MISSION_NONE;
	replica.cruise     = 0;
	replica.maxTraffic = 0.0f;
	replica.zone       = LEVEL_INDUSTRIAL;

	const AdoptCarBytes parked = CarBytesAfterAdoption(replica, /*driven=*/false);
	Check((parked.flagsA & offs::VEH_IS_LOCKED) == 0 && (parked.flagsA & offs::VEH_LIGHTS_ON),
	      "unlocked, so the traffic reaper can have it, and nothing else in the byte");
	Check(parked.flagsC == 0x01 && parked.entityC == 0x80,
	      "nobody's car, and no longer collision-proof");
	Check(parked.cruise == 9 && parked.maxTraffic == 9.0f,
	      "CREATE_CAR's speeds where the replica zeroed them - the AI divides by one");
	Check(parked.mission == CAR_MISSION_NONE && parked.status == ENTITY_STATUS_ABANDONED &&
	          (parked.flagsA & offs::VEH_ENGINE_ON) == 0,
	      "with nobody in it, parked: no mission, engine off, still abandoned");
	Check(parked.zone == LEVEL_GENERIC, "and the generic level");

	const AdoptCarBytes driven = CarBytesAfterAdoption(replica, /*driven=*/true);
	Check(driven.mission == CAR_MISSION_CRUISE && (driven.flagsA & offs::VEH_ENGINE_ON),
	      "with a driver, CAR_WANDER_RANDOMLY: cruising, engine on");
	Check(driven.status == ENTITY_STATUS_PHYSICS, "and in PHYSICS, where a driven car runs its AI");

	AdoptCarBytes slow = replica;
	slow.cruise     = 3;
	slow.maxTraffic = 3.0f;
	slow.status     = ENTITY_STATUS_PHYSICS;
	const AdoptCarBytes s = CarBytesAfterAdoption(slow, true);
	Check(s.cruise == CAR_WANDER_MIN_CRUISE && s.maxTraffic == 3.0f,
	      "a crawl is lifted to the wander's floor of 6; a real max speed is kept");
	Check(s.status == ENTITY_STATUS_PHYSICS, "a car already in PHYSICS stays there");

	Check(AdoptedCarIsDriven(true, PEDSTATE_DRIVING), "a driver at the wheel drives");
	Check(!AdoptedCarIsDriven(false, 0) && !AdoptedCarIsDriven(true, PEDSTATE_DEAD),
	      "an empty seat or a dead man does not");
}

} // namespace

int RunAdoptTests() {
	TestWhatIsTakenAndWhatIsLetGo();
	TestAPedIsPutBack();
	TestHeWalksTheWayHeFaces();
	TestACarIsPutBack();
	return g_adoptFailures;
}
