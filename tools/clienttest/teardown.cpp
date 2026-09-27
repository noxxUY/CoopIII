// Taking an engine entity out of the world (game/teardown.h): which pointers
// are still entities, what a garage is left holding, who gets out of a car
// first, the "no car" a mission's cleanup sends - and, with a copy of the
// retail exe, the instructions that make each of those the rule.

#include "game/addresses.h"
#include "game/missionaddr.h"
#include "game/replay.h"
#include "game/teardown.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_failures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_failures;
}

// ---- which pointers are still entities ------------------------------------

void TestWhereAPointerSitsInAPool() {
	std::printf("\ntaking an entity away: where a pointer sits in its pool\n");
	const uintptr_t base   = 0x10000000;
	const size_t    stride = offs::SIZEOF_AUTOMOBILE;
	Check(PoolSlotOf(base, stride, 110, base) == 0, "the first slot");
	Check(PoolSlotOf(base, stride, 110, base + 7 * stride) == 7, "the eighth");
	Check(PoolSlotOf(base, stride, 110, base + 109 * stride) == 109, "the last");
	Check(PoolSlotOf(base, stride, 110, base + 110 * stride) == -1, "not one past it");
	Check(PoolSlotOf(base, stride, 110, base + 7 * stride + 4) == -1,
	      "not a pointer part-way into a car");
	Check(PoolSlotOf(base, stride, 110, base - stride) == -1, "not one before the array");
	Check(PoolSlotOf(0, stride, 110, 0) == -1 && PoolSlotOf(base, 0, 110, base) == -1 &&
	          PoolSlotOf(base, stride, 0, base) == -1,
	      "and nothing at all for a pool that is not there");
}

void TestWhatALivingEntityIs() {
	std::printf("\ntaking an entity away: what still counts as an entity\n");
	const uintptr_t car = CAutomobile__vtable;
	Check(ClassifyEntity(3, 0x05, car, false) == EntityState::Live, "an occupied slot with its class's vtable");
	Check(ClassifyEntity(-1, 0x05, car, false) == EntityState::NotInPool, "nothing outside the pool");
	Check(ClassifyEntity(3, 0x85, car, false) == EntityState::FreedSlot,
	      "a slot whose free bit is set, whatever the rest of the byte says");
	Check(ClassifyEntity(3, 0x05, CPlaceable__vtable, false) == EntityState::Destructed,
	      "CPlaceable's vtable: the destructor chain has run");
	Check(ClassifyEntity(3, 0x05, 0, false) == EntityState::Destructed, "and no vtable at all");
	Check(ClassifyEntity(3, 0x85, CPlaceable__vtable, false) == EntityState::FreedSlot,
	      "a deleted car is a freed slot first - that is the crash's car");
	Check(ClassifyEntity(3, 0x05, car, true) == EntityState::BeingRemoved,
	      "one already on its way out further up the stack");
	Check(std::strcmp(EntityStateName(EntityState::Destructed), "already destroyed") == 0,
	      "and each has a name for the line that says it was skipped");
}

// A freed pointer cannot be caught by turning it into a handle and back:
// GetVehicleRef builds the handle's low byte out of the flag byte as it is
// now, free bit and all, and GetAt only compares the two. Modelled here with
// the same arithmetic the two functions do.
void TestAHandleMadeFromAFreedPointerStillResolves() {
	std::printf("\ntaking an entity away: why the free bit is tested and not a handle\n");
	uint8_t      flags[4] = {0x01, 0x02, 0x83, 0x04};   // slot 2 freed
	const int32_t slot    = 2;
	const int32_t ref     = (slot << 8) + flags[slot];          // GetIndex
	const bool    resolves = flags[ref >> 8] == (ref & 0xFF);   // GetAt
	Check(resolves, "the handle made from a freed slot resolves to that slot");
	Check(ClassifyEntity(slot, flags[slot], CPlaceable__vtable, false) == EntityState::FreedSlot,
	      "and the pool check refuses it");
}

void TestRemovalsInFlight() {
	std::printf("\ntaking an entity away: never the same one twice at once\n");
	RemovalsInFlight f;
	int a = 0, b = 0;
	Check(f.Enter(&a) && f.Has(&a), "the first teardown goes ahead");
	Check(!f.Enter(&a), "one reaching back into itself does not");
	Check(f.Enter(&b), "another entity's does");
	f.Leave(&a);
	Check(!f.Has(&a) && f.Has(&b), "and leaving takes only the one that left");
	Check(!f.Enter(nullptr), "nothing is not an entity");
	RemovalsInFlight full;
	int many[RemovalsInFlight::CAPACITY + 1] = {};
	bool all = true;
	for (size_t i = 0; i < RemovalsInFlight::CAPACITY; ++i)
		all = full.Enter(&many[i]) && all;
	Check(all && !full.Enter(&many[RemovalsInFlight::CAPACITY]),
	      "and a full table refuses rather than forgets");
}

// ---- what a garage is left holding ----------------------------------------

void TestAGarageForgetsACarThatGoes() {
	std::printf("\ntaking a car away: the garages waiting for it\n");
	std::vector<uint8_t> garages(game::NUM_GARAGES * SIZEOF_GARAGE, 0);
	int carA = 0, carB = 0;
	auto target = [&](size_t g) -> void *& {
		return *reinterpret_cast<void **>(garages.data() + g * SIZEOF_GARAGE +
		                                  offs::GARAGE_TARGET);
	};
	target(4)  = &carA;   // Luigi's lockup, say
	target(19) = &carA;
	target(7)  = &carB;
	const size_t n = ClearGarageTargets(garages.data(), game::NUM_GARAGES, SIZEOF_GARAGE,
	                                    offs::GARAGE_TARGET, &carA);
	Check(n == 2 && target(4) == nullptr && target(19) == nullptr,
	      "every garage waiting for the car that goes waits for nothing");
	Check(target(7) == &carB, "a garage waiting for another car still does");
	Check(ClearGarageTargets(garages.data(), game::NUM_GARAGES, SIZEOF_GARAGE, offs::GARAGE_TARGET,
	                         &carA) == 0,
	      "and a second pass finds nothing");
	bool rest = true;
	for (size_t i = 0; i < garages.size(); ++i) {
		const size_t in = i % SIZEOF_GARAGE;
		if ((i / SIZEOF_GARAGE == 7) && in >= offs::GARAGE_TARGET && in < offs::GARAGE_TARGET + 4)
			continue;
		rest = rest && garages[i] == 0;
	}
	Check(rest, "and nothing but the target is written");
	Check(ClearGarageTargets(garages.data(), game::NUM_GARAGES, SIZEOF_GARAGE, offs::GARAGE_TARGET,
	                         nullptr) == 0,
	      "a null car clears no garage that is waiting for none");
	Check(CGarages__aGarages + offs::GARAGE_TARGET == 0x0072BD2C,
	      "aGarages + m_pTarget is the address SetTargetCarForMissonGarage writes");
}

// ---- who gets out first -----------------------------------------------------

void TestWhoGetsOutOfACarThatGoes() {
	std::printf("\ntaking a car away: whoever is still in it\n");
	Check(HowToEndOccupant(false, false, false, false) == OccupantEnd::None, "an empty seat");
	Check(HowToEndOccupant(true, true, true, true) == OccupantEnd::RefuseCar,
	      "the local player: the car stays");
	Check(HowToEndOccupant(true, false, true, true) == OccupantEnd::FlagPed,
	      "somebody in it goes the way the engine's own delivery sends them");
	Check(HowToEndOccupant(true, false, false, true) == OccupantEnd::ClearSeat,
	      "a seat pointing at a ped that is gone is only emptied");
	Check(HowToEndOccupant(true, false, true, false) == OccupantEnd::ClearSeat,
	      "and so is one whose ped thinks it is in some other car");
}

// ---- the mission's "no car" -------------------------------------------------

replay::Encoded TargetCar(int32_t garage, int32_t car) {
	replay::Encoded e;
	e.kind    = replay::Kind::Plain;
	e.code[0] = 0x1B;
	e.code[1] = 0x02;
	e.code[2] = scripts::PARAM_INT32;
	std::memcpy(e.code + 3, &garage, 4);
	e.code[7] = scripts::PARAM_INT32;
	std::memcpy(e.code + 8, &car, 4);
	e.length = 12;
	return e;
}

int32_t CarOperand(const replay::Encoded &e) {
	int32_t v = 0;
	std::memcpy(&v, e.code + 8, 4);
	return v;
}

int32_t NoCarHere(int32_t) { return -1; }
int32_t CarSeven(int32_t netId) { return netId == 7 ? 0x1234 : -1; }

void TestAGarageIsToldItsCarIsNone() {
	std::printf("\ntaking a car away: a mission's garage let go of on every machine\n");
	Check(replay::CarMayBeNone(scripts::op::SET_TARGET_CAR_FOR_MISSION_GARAGE),
	      "SET_TARGET_CAR_FOR_MISSION_GARAGE may name no car");
	Check(!replay::CarMayBeNone(0x00A6) && !replay::CarMayBeNone(0x021C),
	      "DELETE_CAR and IS_CAR_IN_MISSION_GARAGE may not");
	Check(replay::Find(0x021B) && replay::Find(0x021B)->args[1] == replay::Arg::Car,
	      "and its second operand is the car");

	replay::Handles h;
	h.carOf = &CarSeven;
	replay::Encoded run;
	Check(replay::Translate(TargetCar(4, -1), h, &run) && CarOperand(run) == -1,
	      "a cleanup's -1 is run here as -1, not dropped as a car nobody has");
	Check(replay::Translate(TargetCar(4, 7), h, &run) && CarOperand(run) == 0x1234,
	      "a car the session names is still ours in its place");
	h.carOf = &NoCarHere;
	Check(!replay::Translate(TargetCar(4, 7), h, &run),
	      "and a named car this machine does not have is still dropped");

	replay::Encoded out = TargetCar(4, -1);
	Check(replay::ToWire(&out, &NoCarHere, &NoCarHere) && CarOperand(out) == -1,
	      "the owner sends the -1 as it is");
	out = TargetCar(4, 99);
	Check(!replay::ToWire(&out, &NoCarHere, &NoCarHere),
	      "and still holds back a car the session has no name for");
}

// ---- against the real exe --------------------------------------------------

bool LoadExe(std::vector<uint8_t> &image, std::string &from) {
	std::vector<std::string> candidates;
	if (const char *env = std::getenv("COOPIII_GTA3_EXE"))
		candidates.push_back(env);
	candidates.push_back("reference/bin/gta3.exe");
	candidates.push_back("../../../../reference/bin/gta3.exe");
	for (const std::string &path : candidates) {
		FILE *fh = std::fopen(path.c_str(), "rb");
		if (!fh)
			continue;
		std::fseek(fh, 0, SEEK_END);
		const long size = std::ftell(fh);
		std::fseek(fh, 0, SEEK_SET);
		image.resize(size > 0 ? size_t(size) : 0);
		const size_t got = image.empty() ? 0 : std::fread(image.data(), 1, image.size(), fh);
		std::fclose(fh);
		if (got == image.size() && image.size() == IMAGE_SIZE) {
			from = path;
			return true;
		}
	}
	return false;
}

// .text and .rdata sit at the same offsets in the file as in memory, so a
// virtual address is a file offset once the image base is taken off
// (carremoval.cpp's checks lean on the same).
uint8_t Byte(const std::vector<uint8_t> &img, uint32_t va) { return img[va - IMAGE_BASE]; }

uint32_t Dword(const std::vector<uint8_t> &img, uint32_t va) {
	const size_t o = va - IMAGE_BASE;
	return uint32_t(img[o]) | uint32_t(img[o + 1]) << 8 | uint32_t(img[o + 2]) << 16 |
	       uint32_t(img[o + 3]) << 24;
}

bool Bytes(const std::vector<uint8_t> &img, uint32_t va, std::initializer_list<int> want) {
	uint32_t at = va;
	for (int b : want) {
		if (b >= 0 && Byte(img, at) != static_cast<uint8_t>(b))
			return false;
		++at;
	}
	return true;
}

bool CallsTo(const std::vector<uint8_t> &img, uint32_t site, uint32_t to) {
	return Byte(img, site) == 0xE8 && site + 5 + Dword(img, site + 1) == to;
}

void TestAgainstTheImage() {
	std::printf("\ntaking an entity away: against the retail exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "crash site, the pools and the garage target against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	// The crash.
	Check(Bytes(img, 0x004AEA70, {0x89, 0xD9, 0x8B, 0x31, 0xFF, 0x56, 0x08, 0x8A, 0x43, 0x50}),
	      "CWorld::Remove calls the entity's slot 2 and comes back to 0x004AEA77");
	Check(Dword(img, CPlaceable__vtable) == 0x0049FBD0 && Dword(img, CPlaceable__vtable + 8) == 0,
	      "and a destroyed entity's slot 2, CPlaceable's, is zero");
	Check(CallsTo(img, 0x005527C1, CWorld__Remove) &&
	          Bytes(img, 0x00552786, {0xFF, 0x56, VTABLE_FLAG_TO_DESTROY * 4}),
	      "DestroyVehicleAndDriverAndPassengers flags its people, then CWorld::Remove");
	Check(CallsTo(img, GARAGE_DESTROY_MISSION_CALL, DestroyVehicleAndDriverAndPassengers) &&
	          Bytes(img, 0x0042321A, {0x8B, 0x45, 0x5C}) &&
	          Bytes(img, 0x00423223, {0xC7, 0x45, 0x5C, 0x00, 0x00, 0x00, 0x00}),
	      "the mission garage hands it m_pTarget and nils m_pTarget itself afterwards");

	// The garage's target is a raw pointer.
	Check(CallsTo(img, 0x00443744, CGarages__SetTargetCarForMissonGarage) &&
	          Bytes(img, 0x00443726, {0x85, 0xC0, 0x7D, 0x06, 0x6A, 0x00}),
	      "SET_TARGET_CAR_FOR_MISSION_GARAGE passes null for a negative car");
	Check(Bytes(img, CGarages__SetTargetCarForMissonGarage + 0x31, {0x89, 0x82}) &&
	          Dword(img, CGarages__SetTargetCarForMissonGarage + 0x33) ==
	              CGarages__aGarages + offs::GARAGE_TARGET,
	      "SetTargetCarForMissonGarage stores the car into aGarages + m_pTarget");
	bool noCall = true;
	for (uint32_t va = CGarages__SetTargetCarForMissonGarage; va < 0x00426C18; ++va)
		noCall = noCall && Byte(img, va) != 0xE8;
	Check(noCall && Byte(img, 0x00426C17) == 0xC3,
	      "and calls nothing - no RegisterReference - before it returns");

	// The pools.
	Check(Bytes(img, 0x00429068, {0x8B, 0x51, 0x04, 0x0F, 0xB6, 0x0C, 0x1A, 0x89, 0xD8, 0xC1,
	                              0xE0, 0x08, 0x01, 0xC8}),
	      "GetVehicleRef puts the whole flag byte, free bit included, into the handle");
	Check(Bytes(img, 0x0043EB06, {0x81, 0xE2, 0xFF, 0x00, 0x00, 0x00, 0x39, 0xCA}) &&
	          Bytes(img, 0x0043EB10, {0x69, 0xC0}) &&
	          Dword(img, 0x0043EB12) == offs::SIZEOF_AUTOMOBILE,
	      "the vehicle pool's GetAt compares the whole byte, over a 0x5A8 stride");
	Check(Bytes(img, 0x0043EB50, {0x69, 0xC0}) && Dword(img, 0x0043EB52) == offs::SIZEOF_PLAYER_PED,
	      "the ped pool's GetAt over a 0x5F0 stride");
	Check(Bytes(img, CPools__GetVehicle + 4, {0x8B, 0x0D}) &&
	          Dword(img, CPools__GetVehicle + 6) == CPools__ms_pVehiclePool &&
	          CallsTo(img, CPools__GetVehicle + 11, 0x0043EAF0),
	      "CPools::GetVehicle is that GetAt on ms_pVehiclePool");
	Check(Bytes(img, CPools__GetPed + 4, {0x8B, 0x0D}) && Dword(img, CPools__GetPed + 6) == CPools__ms_pPedPool &&
	          CallsTo(img, CPools__GetPed + 11, 0x0043EB30),
	      "and CPools::GetPed the other on ms_pPedPool");
}

} // namespace

int RunTeardownTests() {
	std::printf("\n");
	TestWhereAPointerSitsInAPool();
	TestWhatALivingEntityIs();
	TestAHandleMadeFromAFreedPointerStillResolves();
	TestRemovalsInFlight();
	TestAGarageForgetsACarThatGoes();
	TestWhoGetsOutOfACarThatGoes();
	TestAGarageIsToldItsCarIsNone();
	TestAgainstTheImage();
	return g_failures;
}
