// Taking an engine entity out of the world (game/teardown.h): which pointers
// are still entities, what a garage is left holding, who gets out of a car
// first, the "no car" a mission's cleanup sends - and, with a copy of the
// retail exe, the instructions that make each of those the rule.

#include "game/addresses.h"
#include "game/look.h"
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

// ---- the camera, a boat's culprit, a renamed model ---------------------------

void *&CameraTarget(std::vector<uint8_t> &cam) {
	return *reinterpret_cast<void **>(cam.data() + offs::CAMERA_TARGET);
}

void *&CamTarget(std::vector<uint8_t> &cam, size_t i) {
	return *reinterpret_cast<void **>(cam.data() + offs::CAMERA_CAMS + i * CAMERA_CAM_STRIDE +
	                                  offs::CAM_TARGET_ENTITY);
}

void TestTheCameraLetsGo() {
	std::printf("\ntaking an entity away: the camera still pointed at it\n");
	std::vector<uint8_t> cam(offs::CAMERA_TARGET + 0x10, 0);
	int car = 0, other = 0, player = 0;

	// POINT_CAMERA_AT_CAR stored the car and Process has not run since.
	CameraTarget(cam) = &car;
	CamTarget(cam, 0) = &player;
	CamTarget(cam, 1) = &other;
	Check(RepointCameraTargets(cam.data(), &car, &player) == 1 && CameraTarget(cam) == &player,
	      "a target the next Process has not registered yet goes to the player, as Process "
	      "puts a nil one");
	Check(CamTarget(cam, 0) == &player && CamTarget(cam, 1) == &other,
	      "and the cams looking at something else are left alone");

	// Process ran: the car is the target and the active cam's too.
	CameraTarget(cam) = &car;
	CamTarget(cam, 0) = &car;
	CamTarget(cam, 2) = &car;
	Check(RepointCameraTargets(cam.data(), &car, &player) == 3 && CameraTarget(cam) == &player &&
	          CamTarget(cam, 0) == &player && CamTarget(cam, 2) == &player && CamTarget(cam, 1) == &other,
	      "a cam on it takes the camera's target, as Process fills a nil one");

	// Restored already: the target is the player's car, an old cam still on ours.
	CameraTarget(cam) = &other;
	CamTarget(cam, 1) = &car;
	Check(RepointCameraTargets(cam.data(), &car, &player) == 1 && CameraTarget(cam) == &other &&
	          CamTarget(cam, 1) == &other,
	      "an old cam still on it takes whatever the camera is on now, not the player");

	Check(RepointCameraTargets(cam.data(), &car, &player) == 0, "and a second pass finds nothing");
	CameraTarget(cam) = &car;
	Check(RepointCameraTargets(cam.data(), &car, nullptr) == 1 && CameraTarget(cam) == nullptr,
	      "with no player yet it is nil, which Process handles");
	Check(RepointCameraTargets(cam.data(), nullptr, &player) == 0 &&
	          RepointCameraTargets(nullptr, &car, &player) == 0,
	      "nothing going touches nothing");

	Check(TheCamera + offs::CAMERA_TARGET == 0x006FB49C &&
	          TheCamera + offs::CAMERA_CAMS + 0x0C == TheCamera__Cams_Mode &&
	          offs::CAMERA_CAMS + offs::CAM_TARGET_ENTITY == 0x32C &&
	          offs::CAM_TARGET_ENTITY + 4 <= CAMERA_CAM_STRIDE,
	      "pTargetEntity is 0x006FB49C and Cams[0].CamTargetEntity camera + 0x32C");
}

void TestABurningBoatForgetsItsCulprit() {
	std::printf("\ntaking an entity away: a burning boat that names it\n");
	const size_t          stride = offs::SIZEOF_AUTOMOBILE;
	std::vector<uint8_t>  pool(4 * stride, 0);
	uint8_t               flags[4] = {0x01, 0x02, 0x83, 0x04};   // slot 2 free
	int                   ped = 0, other = 0;
	auto type = [&](size_t i) -> int32_t & {
		return *reinterpret_cast<int32_t *>(pool.data() + i * stride + offs::VEH_TYPE);
	};
	auto culprit = [&](size_t i) -> void *& {
		return *reinterpret_cast<void **>(pool.data() + i * stride + offs::BOAT_SET_ON_FIRE_ENTITY);
	};
	type(0) = VEHICLE_TYPE_BOAT;  culprit(0) = &ped;
	type(1) = VEHICLE_TYPE_BOAT;  culprit(1) = &other;
	type(2) = VEHICLE_TYPE_BOAT;  culprit(2) = &ped;     // a freed slot
	type(3) = 0;                  culprit(3) = &ped;     // a car: not its field
	Check(ClearBoatCulprits(pool.data(), flags, 4, stride, &ped) == 1 && culprit(0) == nullptr,
	      "a live boat set alight by it blames nobody");
	Check(culprit(1) == &other, "one set alight by somebody else still blames them");
	Check(culprit(2) == &ped && culprit(3) == &ped,
	      "and neither a free slot nor a car's bytes are written");
	Check(ClearBoatCulprits(pool.data(), flags, 4, stride, nullptr) == 0 &&
	          ClearBoatCulprits(nullptr, flags, 4, stride, &ped) == 0,
	      "nothing going, or no pool, clears nothing");
	Check(offs::BOAT_SET_ON_FIRE_ENTITY + 4 <= offs::SIZEOF_BOAT &&
	          offs::BOAT_SET_ON_FIRE_ENTITY != offs::AUTO_SET_ON_FIRE_ENTITY,
	      "the boat's culprit is its own member, not the car's");
}

void TestARenameTakesItsModelAway() {
	std::printf("\ntaking an entity away: a model renamed under what is built from it\n");
	Check(RenameTakesModelAway(26, "misty", "joey"), "special01 under another name");
	Check(RenameTakesModelAway(29, "ray", "love") && RenameTakesModelAway(0, "player", "playerp"),
	      "special04, and model 0");
	Check(!RenameTakesModelAway(26, "misty", "misty"),
	      "the same name is only a RequestModel and takes nothing away");
	Check(!RenameTakesModelAway(30, "a", "b") && !RenameTakesModelAway(25, "a", "b") &&
	          !RenameTakesModelAway(90, "cutobj01", "cutobj02"),
	      "and nothing but model 0 and the four special slots");
	Check(!RenameTakesModelAway(26, nullptr, "joey") && !RenameTakesModelAway(26, "misty", nullptr),
	      "nor a model with no name, or no name asked for");
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
	Check(HowToEndOccupant(true, false, true, true, true) == OccupantEnd::PutOnFoot,
	      "a living mission character is put on foot, never deleted under the script");
	Check(HowToEndOccupant(true, true, true, true, true) == OccupantEnd::RefuseCar,
	      "the local player still keeps the car");
	Check(HowToEndOccupant(true, false, false, true, true) == OccupantEnd::ClearSeat,
	      "a mission character already gone only has his seat emptied");

	std::printf("\na full ped pool\n");
	const uint8_t full[3] = {0x01, 0x02, 0x03};
	const uint8_t room[3] = {0x01, 0x80, 0x03};
	Check(!PoolHasFreeSlot(full, 3), "every slot taken: no room, AddPed would go on with a null");
	Check(PoolHasFreeSlot(room, 3), "one free slot is room");
	Check(!PoolHasFreeSlot(nullptr, 3) && !PoolHasFreeSlot(room, 0), "no pool is no room");
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

// ---- a handle kept across frames --------------------------------------------

void TestWhichHandlesAPoolCanAnswerFor() {
	std::printf("\na kept handle: which ones a pool can be asked about\n");
	Check(PoolHandleShapeOk((7 << 8) | 0x05, 110), "a slot inside the pool with its id");
	Check(PoolHandleShapeOk(0, 110), "slot 0, id 0");
	Check(PoolHandleShapeOk((109 << 8) | 0x7F, 110), "the last slot, the highest id");
	Check(!PoolHandleShapeOk(-1, 110),
	      "not -1: GetAt would read the byte before the flag array");
	Check(!PoolHandleShapeOk(-2, 110) && !PoolHandleShapeOk(INT32_MIN, 110),
	      "nor any other negative");
	Check(!PoolHandleShapeOk(110 << 8, 110), "not one slot past the pool");
	Check(!PoolHandleShapeOk(0x7FFFFF00, 110), "nor far past it");
	Check(!PoolHandleShapeOk((7 << 8) | 0x85, 110),
	      "not one whose low byte has the free bit: made from a slot already freed");
	Check(!PoolHandleShapeOk(0, 0) && !PoolHandleShapeOk(0, -1), "and nothing from no pool");
}

replay::Encoded CameraOnPed(int32_t ped) {
	replay::Encoded e;
	e.kind          = replay::Kind::Plain;
	e.code[0]       = 0x59;
	e.code[1]       = 0x01;
	e.code[2]       = scripts::PARAM_INT32;
	std::memcpy(e.code + 3, &ped, 4);
	const int32_t mode = 15, swap = 2;
	e.code[7]       = scripts::PARAM_INT32;
	std::memcpy(e.code + 8, &mode, 4);
	e.code[12]      = scripts::PARAM_INT32;
	std::memcpy(e.code + 13, &swap, 4);
	e.length = 17;
	return e;
}

replay::Encoded CutsceneAnim(int32_t object) {
	replay::Encoded e;
	e.kind    = replay::Kind::Plain;
	e.code[0] = 0xE6;
	e.code[1] = 0x02;
	e.code[2] = scripts::PARAM_INT32;
	std::memcpy(e.code + 3, &object, 4);
	std::memcpy(e.code + 7, "PLAYER\0\0", 8);
	e.length = 15;
	return e;
}

void TestAnInstructionOnAnEntityThatWentIsNotRun() {
	std::printf("\na kept handle: an owner's instruction on something of ours that went\n");
	int asked = 0;
	auto none = [&](replay::Arg, int32_t) { ++asked; return false; };
	auto all  = [&](replay::Arg, int32_t) { ++asked; return true; };
	replay::Arg seenArg    = replay::Arg::Value;
	int32_t     seenHandle = 0;
	auto        note       = [&](replay::Arg a, int32_t h) {
		seenArg    = a;
		seenHandle = h;
		return true;
	};

	const replay::Encoded cam = CameraOnPed(0x0305);
	Check(replay::EntitiesLive(cam.code, cam.length, all), "a pedestrian still there: run");
	Check(!replay::EntitiesLive(cam.code, cam.length, none), "one that went: not run");
	Check(replay::EntitiesLive(cam.code, cam.length, note) && seenArg == replay::Arg::Char &&
	          seenHandle == 0x0305,
	      "and it is the pedestrian's own handle that is asked about, as a pedestrian");

	const replay::Encoded anim = CutsceneAnim(0x1203);
	Check(!replay::EntitiesLive(anim.code, anim.length, none),
	      "a cutscene object that went: SET_CUTSCENE_ANIM is not run");
	Check(replay::EntitiesLive(anim.code, anim.length, note) && seenArg == replay::Arg::Object &&
	          seenHandle == 0x1203,
	      "asked about as an object");

	const replay::Encoded car = TargetCar(4, 0x0A02);
	Check(!replay::EntitiesLive(car.code, car.length, none), "a garage's car that went: not run");
	Check(replay::EntitiesLive(car.code, car.length, note) && seenArg == replay::Arg::Car &&
	          seenHandle == 0x0A02,
	      "asked about as a car, and the garage number is not asked about at all");
	asked = 0;
	const replay::Encoded noCar = TargetCar(4, -1);
	Check(replay::EntitiesLive(noCar.code, noCar.length, none) && asked == 0,
	      "the mission's \"no car\" is run without asking");

	replay::Encoded unlisted = cam;
	unlisted.code[0] = 0xFF;
	unlisted.code[1] = 0x7F;
	Check(!replay::EntitiesLive(unlisted.code, unlisted.length, all),
	      "an instruction the list does not know is not run either");
	Check(!replay::EntitiesLive(cam.code, replay::MAX_CODE + 1, all),
	      "nor one longer than an instruction can be");
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

	// The camera's target: raw from TakeControl, registered by Process.
	Check(Byte(img, 0x0047157F) == 0xA3 && Dword(img, 0x00471580) == TheCamera + offs::CAMERA_TARGET,
	      "TakeControl stores the entity straight into pTargetEntity");
	bool onlyFinds = true;
	for (uint32_t va = CCamera__TakeControl; va < 0x004715A9; ++va)
		if (Byte(img, va) == 0xE8) {
			const uint32_t to = va + 5 + Dword(img, va + 1);
			onlyFinds = onlyFinds && (to == FindPlayerVehicle || to == FindPlayerPed);
		}
	Check(onlyFinds && Bytes(img, 0x004715A6, {0xC2, 0x10, 0x00}),
	      "and calls nothing but FindPlayerVehicle and FindPlayerPed - no RegisterReference");
	Check(Bytes(img, 0x0046A1F6, {0x68}) && Dword(img, 0x0046A1F7) == TheCamera + offs::CAMERA_TARGET &&
	          CallsTo(img, 0x0046A20C, CEntity__RegisterReference),
	      "Process registers &pTargetEntity once the switch is made");
	Check(Bytes(img, 0x0046A215, {0x69, 0xC9}) && Dword(img, 0x0046A217) == CAMERA_CAM_STRIDE &&
	          Bytes(img, 0x0046A21B, {0x8D, 0x84, 0x29}) &&
	          Dword(img, 0x0046A21E) == offs::CAMERA_CAMS + offs::CAM_TARGET_ENTITY &&
	          CallsTo(img, 0x0046A22A, CEntity__RegisterReference),
	      "and &Cams[ActiveCam].CamTargetEntity at camera + 0x32C, 0x1A4 a cam");
	Check(Bytes(img, CCamera__Process_TargetFallback, {0x83, 0xBB}) &&
	          Dword(img, CCamera__Process_TargetFallback + 2) == offs::CAMERA_TARGET &&
	          Bytes(img, CCamera__Process_TargetFallback + 6, {0x00, 0x75}) &&
	          CallsTo(img, 0x0046D4A8, FindPlayerPed) && Bytes(img, 0x0046D4AD, {0x89, 0x83}) &&
	          Dword(img, 0x0046D4AF) == offs::CAMERA_TARGET,
	      "Process puts a nil pTargetEntity on FindPlayerPed()");
	Check(Bytes(img, 0x0046D4BD, {0x83, 0xBC, 0x13}) &&
	          Dword(img, 0x0046D4C0) == offs::CAMERA_CAMS + offs::CAM_TARGET_ENTITY &&
	          Bytes(img, 0x0046D4C7, {0x8B, 0x83}) && Dword(img, 0x0046D4C9) == offs::CAMERA_TARGET &&
	          Bytes(img, 0x0046D4CD, {0x89, 0x84, 0x13}) &&
	          Bytes(img, 0x0046D4E7, {0x83, 0xBC, 0x03}) &&
	          Bytes(img, 0x0046D4F7, {0x89, 0xBC, 0x03}),
	      "and each of the two cams' nil CamTargetEntity on pTargetEntity");
	Check(CallsTo(img, 0x0043F5F5, CCamera__TakeControl) && Bytes(img, 0x0043F5EB, {0xB9}) &&
	          Dword(img, 0x0043F5EC) == TheCamera && CallsTo(img, 0x0043F5D4, 0x0043EB30),
	      "POINT_CAMERA_AT_CHAR hands TakeControl a ped out of the ped pool, as _CAR does a car");

	// A burning boat's culprit.
	Check(Bytes(img, BOAT_SET_ON_FIRE_STORE - 6, {0x8B, 0x85}) &&
	          Dword(img, BOAT_SET_ON_FIRE_STORE - 4) == object::DAMAGE_ENTITY &&
	          Bytes(img, BOAT_SET_ON_FIRE_STORE, {0x89, 0x85}) &&
	          Dword(img, BOAT_SET_ON_FIRE_STORE + 2) == offs::BOAT_SET_ON_FIRE_ENTITY &&
	          Bytes(img, BOAT_SET_ON_FIRE_STORE + 6, {0xD9, 0xEE}),
	      "a boat copies m_pDamageEntity into m_pSetOnFireEntity and calls nothing after it");
	Check(Bytes(img, 0x0052FFC8, {0x8B, 0x85}) && Dword(img, 0x0052FFCA) == object::DAMAGE_ENTITY &&
	          Bytes(img, 0x0052FFCE, {0x89, 0x85}) && Dword(img, 0x0052FFD0) == offs::AUTO_SET_ON_FIRE_ENTITY &&
	          CallsTo(img, 0x0052FFE5, CEntity__RegisterReference),
	      "where a car's same copy registers the reference");
	Check(Bytes(img, BOAT_FIRE_TIMER_CULPRIT, {0x8B, 0x85}) &&
	          Dword(img, BOAT_FIRE_TIMER_CULPRIT + 2) == offs::BOAT_SET_ON_FIRE_ENTITY &&
	          Bytes(img, BOAT_FIRE_TIMER_CULPRIT + 9, {0xFF, 0x56, 0x74}) &&
	          Dword(img, CBoat__vtable + 0x74) == CBoat__BlowUpCar,
	      "the boat's fire timer hands it to BlowUpCar");
	Check(CallsTo(img, CBoat__BlowUpCar_Explode, CExplosion__AddExplosion) &&
	          Bytes(img, CBoat__BlowUpCar_Explode - 2, {0x50, 0x55}) &&
	          Bytes(img, 0x005592BA, {0x8D, 0x43, 0x18}) && CallsTo(img, 0x005592C5, CEntity__RegisterReference),
	      "which blames it for the blast, and the blast registers a reference on it");

	// A renamed model.
	Check(CallsTo(img, 0x0040A981, CStreaming__RemoveModel) && Bytes(img, 0x0040A91C, {0x74, 0x62}) &&
	          CallsTo(img, 0x0040A905, 0x005A0920),
	      "RequestSpecialModel renames the model and removes it, whatever is built from it");
	Check(Bytes(img, 0x0043EAFA, {0xC1, 0xF8, 0x08, 0x8B, 0x56, 0x04, 0x0F, 0xB6, 0x0C, 0x02}),
	      "GetAt indexes the flag array with handle >> 8 and nothing bounds it first, so a "
	      "kept handle is shaped before it is asked about");
	Check(Bytes(img, CPools__GetObject + 4, {0x8B, 0x0D}) &&
	          Dword(img, CPools__GetObject + 6) == CPools__ms_pObjectPool,
	      "CPools::GetObject asks ms_pObjectPool");
	Check(CallsTo(img, 0x004402D8, 0x0043EAF0) &&
	          Bytes(img, 0x004402E9, {0x50, 0x89, 0xC1}) && CallsTo(img, 0x004402F5, 0x0049FA00),
	      "SET_CAR_HEADING calls SetHeading on whatever GetAt gave it, null included");
}

} // namespace

int RunTeardownTests() {
	std::printf("\n");
	TestWhereAPointerSitsInAPool();
	TestWhatALivingEntityIs();
	TestAHandleMadeFromAFreedPointerStillResolves();
	TestRemovalsInFlight();
	TestAGarageForgetsACarThatGoes();
	TestTheCameraLetsGo();
	TestABurningBoatForgetsItsCulprit();
	TestARenameTakesItsModelAway();
	TestWhoGetsOutOfACarThatGoes();
	TestAGarageIsToldItsCarIsNone();
	TestWhichHandlesAPoolCanAnswerFor();
	TestAnInstructionOnAnEntityThatWentIsNotRun();
	TestAgainstTheImage();
	return g_failures;
}
